/***********************************************************************************************************************************
Test Fork Info Handler
***********************************************************************************************************************************/
#include "common/io/bufferRead.h"
#include "common/io/bufferWrite.h"
#include "postgres/version.h"
#include "storage/posix/storage.h"

#include "harness/info.h"

/**********************************************************************************************************************************/
static InfoForkData
testForkData(void)
{
    return (InfoForkData)
           {
               .sourceId = strNewZ("source-repository-identity"),
               .sourceRepoKey = 2,
               .sourceStanza = strNewZ("production"),
               .sourceArchiveId = strNewZ("16-1"),
               .backupLabel = strNewZ("20261008-120000F_20261008-130000I"),
               .importedBackupList = strLstNewSplitZ(STRDEF("20261008-120000F,20261008-120000F_20261008-130000I"), ","),
               .pgVersion = PG_VERSION_16,
               .systemId = 7612345678901234567,
               .walSegmentSize = 16 * 1024 * 1024,
               .sourceTimeline = 1,
               .sourceHistory = EMPTY_STR,
               .targetType = strNewZ("time"),
               .targetValue = strNewZ("2026-10-08 13:30:00+03"),
               .targetExclusive = true,
               .state = infoForkStatePreparing,
               .history = EMPTY_STR,
           };
}

/**********************************************************************************************************************************/
static void
testRun(void)
{
    const CipherSpec *const cipherNone = cipherSpecNewNone();

    // *****************************************************************************************************************************
    if (testBegin("fork metadata and state transitions"))
    {
        InfoFork *fork;

        MEM_CONTEXT_TEMP_BEGIN()
        {
            const InfoForkData data = testForkData();
            TEST_ASSIGN(fork, infoForkNew(REPOSITORY_FORMAT_DEFAULT, &data), "create preparing fork");
            TEST_RESULT_VOID(infoForkMove(fork, memContextPrior()), "move copied metadata outside input context");
        }
        MEM_CONTEXT_TEMP_END();

        TEST_RESULT_UINT(infoForkFormat(fork), REPOSITORY_FORMAT_DEFAULT, "payload repository format is preserved");
        TEST_RESULT_UINT(infoForkData(fork)->sourceRepoKey, 2, "repository key is one based");
        TEST_RESULT_STR_Z(infoForkData(fork)->sourceId, "source-repository-identity", "identity survives input context");
        TEST_RESULT_STR_Z(infoForkData(fork)->sourceStanza, "production", "source stanza");
        TEST_RESULT_STR_Z(infoForkData(fork)->sourceArchiveId, "16-1", "source archive id");
        TEST_RESULT_UINT(strLstSize(infoForkData(fork)->importedBackupList), 2, "dependency labels survive input context");
        TEST_RESULT_UINT(infoForkData(fork)->systemId, 7612345678901234567, "system id keeps 64 bits");
        TEST_RESULT_BOOL(infoForkData(fork)->targetExclusive, true, "exclusive target is preserved");
        TEST_ERROR(
            infoForkHistorySet(fork, 2, 0x3000100, STRDEF("1\t0/3000100\ttarget\n")), FormatError,
            "unpublished fork cannot have a branch");
        TEST_RESULT_UINT(infoForkData(fork)->ownTimeline, 0, "failed branch confirmation leaves metadata unchanged");
        TEST_ERROR(
            infoForkHistorySet(fork, 2, 0, STRDEF("1\t0/3000000\ttarget\n")), FormatError,
            "invalid fork branch metadata");

        TEST_RESULT_VOID(infoForkPublishedSet(fork), "publish repository metadata without restoring PostgreSQL");
        TEST_RESULT_VOID(infoForkPublishedSet(fork), "publication confirmation is idempotent");
        Buffer *const publishedBuffer = bufNew(0);
        infoForkSave(fork, ioBufferWriteNew(publishedBuffer));
        const InfoFork *const published = infoForkNewLoad(ioBufferReadNew(publishedBuffer), cipherNone);
        TEST_RESULT_UINT(infoForkData(published)->state, infoForkStatePublished, "published state survives a retry");
        TEST_RESULT_UINT(infoForkData(published)->ownTimeline, 0, "metadata publication requires no PostgreSQL branch");

        const String *const history = STRDEF("1\t0/3000100\ttarget\n");
        TEST_RESULT_VOID(infoForkHistorySet(fork, 2, 0x3000100, history), "confirm branch");
        TEST_RESULT_VOID(infoForkHistorySet(fork, 2, 0x3000100, history), "same confirmation is idempotent");
        TEST_ERROR(
            infoForkHistorySet(fork, 2, 0x3000200, history), FormatError, "fork branch has already been confirmed");
        const String *const laterHistory = STRDEF("1\t0/3000100\ttarget\n2\t0/5000000\tpromotion\n");
        TEST_ERROR(infoForkHistorySet(fork, 3, 0x3000100, EMPTY_STR), FormatError, "invalid fork branch metadata");
        TEST_RESULT_UINT(infoForkData(fork)->historyTimeline, 2, "failed history update leaves the accepted timeline unchanged");
        TEST_RESULT_STR(infoForkData(fork)->history, history, "failed history update keeps the accepted history");
        TEST_RESULT_VOID(infoForkHistorySet(fork, 3, 0x3000100, laterHistory), "accept a later child timeline");
        TEST_RESULT_VOID(infoForkHistorySet(fork, 3, 0x3000100, laterHistory), "history confirmation is idempotent");
        TEST_RESULT_UINT(infoForkData(fork)->historyTimeline, 3, "current accepted leaf advances");
        TEST_RESULT_UINT(infoForkData(fork)->ownTimeline, 2, "initial fork timeline does not change");
        TEST_RESULT_UINT(infoForkData(fork)->branchLsn, 0x3000100, "initial fork LSN does not change");
        TEST_ERROR(infoForkHistorySet(fork, 2, 0x3000100, history), FormatError, "fork history timeline cannot decrease");
        TEST_ERROR(
            infoForkHistorySet(fork, 3, 0x3000100, history), FormatError,
            "fork history for this timeline has already been confirmed");

        Buffer *const buffer = bufNew(0);
        TEST_RESULT_VOID(infoForkSave(fork, ioBufferWriteNew(buffer)), "serialize confirmed fork");
        InfoFork *const loaded = infoForkNewLoad(ioBufferReadNew(buffer), cipherNone);
        TEST_RESULT_STR(infoForkData(loaded)->history, laterHistory, "newlines in accepted history survive serialization");
        TEST_RESULT_UINT(infoForkData(loaded)->historyTimeline, 3, "accepted leaf survives serialization");
        TEST_RESULT_UINT(infoForkData(loaded)->state, infoForkStatePublished, "published state survives serialization");
        TEST_RESULT_STR_Z(infoForkData(loaded)->sourceHistory, "", "timeline one has an empty source history");
        TEST_RESULT_STR_Z(infoForkData(loaded)->targetValue, "2026-10-08 13:30:00+03", "target survives serialization");
        Buffer *const savedAgain = bufNew(0);
        TEST_RESULT_VOID(infoForkSave(loaded, ioBufferWriteNew(savedAgain)), "serialize loaded fork");
        TEST_RESULT_BOOL(bufEq(buffer, savedAgain), true, "serialization round trip preserves every field");
        TEST_RESULT_VOID(infoForkFree(loaded), "free loaded metadata");
        TEST_RESULT_VOID(infoForkFree(fork), "free original metadata");

        InfoForkData data = testForkData();
        data.state = (InfoForkState)99;
        TEST_ERROR(infoForkNew(REPOSITORY_FORMAT_DEFAULT, &data), FormatError, "invalid fork state");
        data = testForkData();
        data.targetType = STRDEF("name");
        TEST_ERROR(infoForkNew(REPOSITORY_FORMAT_DEFAULT, &data), FormatError, "fork target type must be time or lsn");
        data = testForkData();
        data.sourceStanza = STRDEF("../production");
        TEST_ERROR(infoForkNew(REPOSITORY_FORMAT_DEFAULT, &data), FormatError, "invalid fork source stanza");
        data = testForkData();
        data.walSegmentSize = 3 * 1024 * 1024;
        TEST_ERROR(
            infoForkNew(REPOSITORY_FORMAT_DEFAULT, &data), FormatError,
            "fork WAL segment size must be a power of two between 1MB and 1GB");
        data = testForkData();
        data.sourceRepoKey = 0;
        TEST_ERROR(
            infoForkNew(REPOSITORY_FORMAT_DEFAULT, &data), FormatError, "fork source repository key must be greater than zero");
        data = testForkData();
        StringList *const duplicateList = strLstDup(data.importedBackupList);
        strLstAdd(duplicateList, data.backupLabel);
        data.importedBackupList = duplicateList;
        TEST_ERROR(
            infoForkNew(REPOSITORY_FORMAT_DEFAULT, &data), FormatError,
            "duplicate fork imported backup label '20261008-120000F_20261008-130000I'");
        data = testForkData();
        StringList *const invalidList = strLstDup(data.importedBackupList);
        strLstAdd(invalidList, STRDEF("../outside"));
        data.importedBackupList = invalidList;
        TEST_ERROR(
            infoForkNew(REPOSITORY_FORMAT_DEFAULT, &data), FormatError, "invalid fork imported backup label '../outside'");

        TEST_ERROR(
            infoForkNewLoad(ioBufferReadNew(harnessInfoChecksumZ("[fork]\nformat=2\n")), cipherNone), FormatError,
            "unsupported fork format 2");
        TEST_ERROR(
            infoForkNewLoad(ioBufferReadNew(harnessInfoChecksumZ("[fork]\nformat=1\n")), cipherNone), FormatError,
            "fork format and metadata are required");
        TEST_ERROR(
            infoForkNewLoad(ioBufferReadNew(harnessInfoChecksumZ("[other]\nformat=1\n")), cipherNone), FormatError,
            "invalid fork section 'other'");
        TEST_ERROR(
            infoForkNewLoad(ioBufferReadNew(harnessInfoChecksumZ("[fork]\nother=1\n")), cipherNone), FormatError,
            "invalid fork key 'other'");
    }

    // *****************************************************************************************************************************
    if (testBegin("save fork metadata and use the encrypted recovery copy"))
    {
        const Storage *const storage = storagePosixNewP(TEST_PATH_STR, .write = true);
        InfoForkData data = testForkData();
        data.sourceTimeline = 4;
        data.sourceHistory = STRDEF("1\t0/2000000\tpromotion\n");
        data.targetType = STRDEF("lsn");
        data.targetValue = STRDEF("0/3000100");
        data.targetExclusive = false;

        for (unsigned int format = REPOSITORY_FORMAT_5; format <= REPOSITORY_FORMAT_6; format++)
        {
            InfoFork *const fork = infoForkNew(format, &data);
            const CipherSpec *const cipher = cipherSpecNewP(cipherTypeAes256Cbc, BUFSTRDEF("fork-metadata-main-key"));
            const String *const path = strNewFmt("backup/experiment-%u/fork.info", format);
            const String *const copy = strNewFmt("%s.copy", strZ(path));

            TEST_RESULT_VOID(infoForkSaveFile(fork, storage, path, cipher), "save encrypted preparing fork");
            TEST_RESULT_BOOL(
                bufEq(storageGetP(storageNewReadP(storage, path)), storageGetP(storageNewReadP(storage, copy))), true,
                "main and recovery copy have identical encrypted contents");

            InfoFork *loaded = infoForkLoadFile(storage, path, cipher);
            TEST_RESULT_UINT(infoForkFormat(loaded), format, "repository format is preserved");
            TEST_RESULT_UINT(infoForkData(loaded)->state, infoForkStatePreparing, "preparing state reloads");
            TEST_RESULT_STR(infoForkData(loaded)->sourceHistory, data.sourceHistory, "source lineage reloads");
            TEST_RESULT_STR_Z(infoForkData(loaded)->targetType, "lsn", "LSN target reloads");

            TEST_RESULT_VOID(
                storagePutP(storageNewWriteP(storage, path), BUFSTRDEF("corrupted primary metadata")),
                "corrupt main metadata");
            TEST_ASSIGN(loaded, infoForkLoadFile(storage, path, cipher), "recover from encrypted copy");
            TEST_RESULT_STR_Z(infoForkData(loaded)->sourceId, "source-repository-identity", "copy contains source identity");

            TEST_RESULT_VOID(storageRemoveP(storage, path), "remove corrupt primary");
            TEST_ASSIGN(loaded, infoForkLoadFile(storage, path, cipher), "recover when main metadata is missing");
            TEST_RESULT_UINT(infoForkData(loaded)->walSegmentSize, 16 * 1024 * 1024, "copy preserves WAL segment size");
        }

        TEST_ERROR(
            infoForkLoadFile(storage, STRDEF("missing/fork.info"), cipherNone), FileMissingError,
            "unable to load fork info file '" TEST_PATH "/missing/fork.info' or '" TEST_PATH "/missing/fork.info.copy':\n"
            "FileMissingError: unable to open missing file '" TEST_PATH "/missing/fork.info' for read\n"
            "FileMissingError: unable to open missing file '" TEST_PATH "/missing/fork.info.copy' for read");
    }
}
