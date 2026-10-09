/***********************************************************************************************************************************
Test Source Backup Storage
***********************************************************************************************************************************/
#include "command/backup/blockMap.h"
#include "common/io/bufferRead.h"
#include "common/io/bufferWrite.h"
#include "common/io/io.h"
#include "storage/helper.h"
#include "storage/posix/storage.h"
#include "storage/readMulti.h"

#define TEST_FORK_FULL                                              "20261008-120000F"
#define TEST_FORK_INCR                                              "20261008-120000F_20261008-130000I"
#define TEST_FORK_OWN                                               "20261008-120000F_20261008-140000I"

/**********************************************************************************************************************************/
static String *
testForkPathExpression(const String *const expression, const String *const path)
{
    ASSERT(strEqZ(expression, STORAGE_REPO_BACKUP));
    return strNewFmt("backup/child%s%s", path == NULL ? "" : "/", path == NULL ? "" : strZ(path));
}

/**********************************************************************************************************************************/
static void
testRun(void)
{
    // *****************************************************************************************************************************
    if (testBegin("source backup files and fork metadata use separate repositories"))
    {
        const Storage *const own = storagePosixNewP(
            STRDEF(TEST_PATH "/own"), .write = true, .pathExpressionFunction = testForkPathExpression);
        const Storage *const source = storagePosixNewP(STRDEF(TEST_PATH "/source"), .write = true);
        const StringList *const inherited = strLstNewSplitZ(STRDEF(TEST_FORK_FULL "," TEST_FORK_INCR), ",");
        const Storage *const view = storageForkNew(own, source, STRDEF("child"), STRDEF("production"), inherited);

        storagePutP(storageNewWriteP(source, STRDEF("backup/production/" TEST_FORK_FULL "/pg_data/base/1/2")), BUFSTRDEF("source"));
        storagePutP(storageNewWriteP(own, STRDEF("backup/child/" TEST_FORK_FULL "/pg_data/base/1/2")), BUFSTRDEF("wrong own data"));
        storagePutP(
            storageNewWriteP(source, STRDEF("backup/production/" TEST_FORK_FULL "/backup.manifest")), BUFSTRDEF("source key"));
        storagePutP(storageNewWriteP(own, STRDEF("backup/child/" TEST_FORK_FULL "/backup.manifest")), BUFSTRDEF("own key"));
        storagePutP(
            storageNewWriteP(own, STRDEF("backup/child/" TEST_FORK_FULL "/backup.manifest.copy")), BUFSTRDEF("own key copy"));
        storagePutP(storageNewWriteP(own, STRDEF("backup/child/backup.info")), BUFSTRDEF("own metadata key"));
        storagePutP(storageNewWriteP(source, STRDEF("backup/production/backup.info")), BUFSTRDEF("source metadata key"));
        storagePutP(storageNewWriteP(own, STRDEF("backup/child/" TEST_FORK_OWN "/bundle/7")), BUFSTRDEF("new child bundle"));
        storagePutP(
            storageNewWriteP(source, STRDEF("backup/production/" TEST_FORK_OWN "/bundle/7")), BUFSTRDEF("wrong source bundle"));
        storagePutP(storageNewWriteP(own, STRDEF("backup/child-extra/" TEST_FORK_FULL "/bundle/7")), BUFSTRDEF("other stanza"));
        storagePutP(storageNewWriteP(own, STRDEF("backup/child/" TEST_FORK_FULL "-extra/bundle/7")), BUFSTRDEF("other label"));

        TEST_RESULT_STR_Z(
            strNewBuf(storageGetP(storageNewReadP(view, STRDEF(STORAGE_REPO_BACKUP "/" TEST_FORK_FULL "/pg_data/base/1/2")))),
            "source", "configured child expression resolves inherited bytes in another physical root");
        TEST_RESULT_UINT(
            storageInfoP(view, STRDEF("backup/child/" TEST_FORK_FULL "/pg_data/base/1/2")).size, 6,
            "file information describes the inherited object");
        TEST_RESULT_STR_Z(
            strNewBuf(storageGetP(storageNewReadP(view, STRDEF("backup/child/" TEST_FORK_FULL "/backup.manifest")))),
            "own key", "copied manifest uses destination metadata and key");
        TEST_RESULT_STR_Z(
            strNewBuf(storageGetP(storageNewReadP(view, STRDEF("backup/child/" TEST_FORK_FULL "/backup.manifest.copy")))),
            "own key copy", "manifest recovery copy remains in destination");
        TEST_RESULT_STR_Z(
            strNewBuf(storageGetP(storageNewReadP(view, STRDEF("backup/child/backup.info")))),
            "own metadata key", "backup metadata is never inherited");
        TEST_RESULT_STR_Z(
            strNewBuf(storageGetP(storageNewReadP(view, STRDEF("backup/child/" TEST_FORK_OWN "/bundle/7")))),
            "new child bundle", "new child backup stays in destination even if source has the same object");
        TEST_RESULT_STR_Z(
            strNewBuf(storageGetP(storageNewReadP(view, STRDEF("backup/child-extra/" TEST_FORK_FULL "/bundle/7")))),
            "other stanza", "stanza prefix collision does not redirect");
        TEST_RESULT_STR_Z(
            strNewBuf(storageGetP(storageNewReadP(view, STRDEF("backup/child/" TEST_FORK_FULL "-extra/bundle/7")))),
            "other label", "backup label prefix collision does not redirect");

        storagePutP(storageNewWriteP(own, STRDEF("backup/child/" TEST_FORK_FULL "/missing-source")), BUFSTRDEF("decoy"));
        TEST_RESULT_BOOL(
            storageExistsP(view, STRDEF("backup/child/" TEST_FORK_FULL "/missing-source")), false,
            "a missing inherited payload is not silently replaced with child bytes");
        TEST_RESULT_BOOL(
            storageGetP(storageNewReadP(
                            view, STRDEF("backup/child/" TEST_FORK_FULL "/missing-source"), .ignoreMissing = true)) == NULL,
            true, "ignore-missing preserves source ownership");

        storagePutP(storageNewWriteP(source, STRDEF("backup/production/source-only/backup.manifest")), BUFSTRDEF("not imported"));
        TEST_RESULT_BOOL(
            strLstExists(storageListP(view, STRDEF("backup/child")), STRDEF("source-only")), false,
            "listing exposes only imported and newly created destination metadata");
        TEST_RESULT_BOOL(storageFeature(view, storageFeatureHardLink), false, "view cannot hardlink into a source");
        TEST_RESULT_BOOL(storageFeature(view, storageFeatureSymLink), false, "view cannot create source links");
        TEST_ERROR(storageNewWriteP(view, STRDEF("backup/child/new")), AssertError, "assertion 'this->write' failed");
        TEST_ERROR(storageRemoveP(view, STRDEF("backup/child/backup.info")), AssertError, "assertion 'this->write' failed");
        TEST_ERROR(
            storagePathRemoveP(view, STRDEF("backup/child"), .recurse = true), AssertError, "assertion 'this->write' failed");
        TEST_RESULT_STR_Z(
            strNewBuf(storageGetP(storageNewReadP(source, STRDEF("backup/production/" TEST_FORK_FULL "/pg_data/base/1/2")))),
            "source", "rejected mutations preserve source bytes");
    }

    // *****************************************************************************************************************************
    if (testBegin("read bundle ranges and block maps from source and fork repositories"))
    {
        const Storage *const own = storagePosixNewP(STRDEF(TEST_PATH "/own"), .write = true);
        const Storage *const source = storagePosixNewP(STRDEF(TEST_PATH "/source"), .write = true);
        const StringList *const labels = strLstNewSplitZ(STRDEF(TEST_FORK_FULL "," TEST_FORK_INCR "," TEST_FORK_OWN), ",");
        const Storage *const view = storageForkNew(
            own, source, STRDEF("child"), STRDEF("production"),
            strLstNewSplitZ(STRDEF(TEST_FORK_FULL "," TEST_FORK_INCR), ","));

        storagePutP(storageNewWriteP(source, STRDEF("backup/production/" TEST_FORK_FULL "/bundle/7")), BUFSTRDEF("hFULLunused"));
        storagePutP(storageNewWriteP(source, STRDEF("backup/production/" TEST_FORK_INCR "/bundle/7")), BUFSTRDEF("hhINCRunused"));
        storagePutP(storageNewWriteP(own, STRDEF("backup/child/" TEST_FORK_OWN "/bundle/7")), BUFSTRDEF("hNEW!unused"));

        BlockMap *const map = blockMapNew();
        for (unsigned int idx = 0; idx < 3; idx++)
        {
            blockMapAdd(
                map, &(BlockMapItem)
            {
                .reference = idx, .bundleId = 7, .offset = idx == 1 ? 2 : 1, .size = 4, .superBlockSize = 4,
            });
        }

        Buffer *const mapBuffer = bufNew(0);
        IoWrite *const write = ioBufferWriteNew(mapBuffer);
        ioWriteOpen(write);
        blockMapWrite(map, write, 4, 5);
        ioWriteClose(write);
        Buffer *const bundle = bufNewC("head", 4);
        bufCat(bundle, mapBuffer);
        bufCat(bundle, BUFSTRDEF("tail"));
        storagePutP(storageNewWriteP(source, STRDEF("backup/production/" TEST_FORK_INCR "/bundle/9")), bundle);
        storagePutP(storageNewWriteP(own, STRDEF("backup/child/" TEST_FORK_OWN "/bundle/9")), bundle);

        for (unsigned int labelIdx = 1; labelIdx < 3; labelIdx++)
        {
            const Buffer *const mapRead = storageGetP(
                storageNewReadP(
                    view, strNewFmt("backup/child/%s/bundle/9", strZ(strLstGet(labels, labelIdx))),
                    .offset = 4, .limit = VARUINT64(bufUsed(mapBuffer))));
            TEST_RESULT_BOOL(bufEq(mapRead, mapBuffer), true, "read exactly the block map range from the selected bundle");
            const BlockMap *const loaded = blockMapNewRead(ioBufferReadNewOpen(mapRead), 4, 5);
            TEST_RESULT_UINT(blockMapSize(loaded), 3, "serialized block references survive inherited and own reads");

            StorageReadMulti *const read = storageNewReadMultiP(view);
            for (unsigned int blockIdx = 0; blockIdx < blockMapSize(loaded); blockIdx++)
            {
                const BlockMapItem *const item = blockMapGet(loaded, blockIdx);
                storageReadMultiAddP(
                    read,
                    strNewFmt("backup/child/%s/bundle/%" PRIu64, strZ(strLstGet(labels, item->reference)), item->bundleId),
                    .offset = item->offset, .limit = VARUINT64(item->size));
            }

            TEST_RESULT_BOOL(ioReadOpen(storageReadMultiIo(read)), true, "open mixed repository block ranges");
            Buffer *const result = bufNew(64);
            TEST_RESULT_UINT(ioRead(storageReadMultiIo(read), result), 12, "read full, incremental and new child blocks");
            TEST_RESULT_STR_Z(strNewBuf(result), "FULLINCRNEW!", "reference indices retain their original meaning");
            TEST_RESULT_BOOL(ioReadEof(storageReadMultiIo(read)), true, "ranges exclude bundle headers and unrelated bytes");
            TEST_RESULT_VOID(ioReadClose(storageReadMultiIo(read)), "close mixed read");
        }
    }
}
