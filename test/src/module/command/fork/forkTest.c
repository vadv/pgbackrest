/***********************************************************************************************************************************
Test Fork Preparation
***********************************************************************************************************************************/
#include "common/io/bufferRead.h"
#include "common/io/bufferWrite.h"
#include "storage/posix/storage.h"

#include "harness/config.h"
#include "harness/info.h"
#include "harness/postgres.h"
#include "harness/storage.h"

#define FORK_TEST_FULL                                             "20261008-100000F"
#define FORK_TEST_EARLY                                            FORK_TEST_FULL "_20261008-103000I"
#define FORK_TEST_DIFF                                             FORK_TEST_FULL "_20261008-110000D"
#define FORK_TEST_INCR                                             FORK_TEST_FULL "_20261008-120000I"
#define FORK_TEST_LATE                                             "20261008-130000F"

static InfoBackup *
forkTestBackup(void)
{
    InfoBackup *const result = infoBackupNew(
        PG_VERSION_16, HRN_PG_SYSTEMID_16, hrnPgCatalogVersion(PG_VERSION_16), REPOSITORY_FORMAT_5, NULL);
    const InfoBackupData backups[] =
    {
        {.backupLabel = STRDEF(FORK_TEST_FULL), .backupLsnStop = STRDEF("0/2000100"), .backupTimestampStop = 1791453900},
        {
            .backupLabel = STRDEF(FORK_TEST_EARLY), .backupLsnStop = STRDEF("0/2800100"), .backupTimestampStop = 1791455700,
            .backupPrior = STRDEF(FORK_TEST_FULL),
        },
        {
            .backupLabel = STRDEF(FORK_TEST_DIFF), .backupLsnStop = STRDEF("0/3000100"), .backupTimestampStop = 1791457500,
            .backupPrior = STRDEF(FORK_TEST_FULL),
        },
        {
            .backupLabel = STRDEF(FORK_TEST_INCR), .backupLsnStop = STRDEF("0/4000100"), .backupTimestampStop = 1791461100,
            .backupPrior = STRDEF(FORK_TEST_DIFF),
            .backupReference = strLstNewSplitZ(STRDEF(FORK_TEST_FULL "," FORK_TEST_EARLY), ","),
        },
        {.backupLabel = STRDEF(FORK_TEST_LATE), .backupTimestampStop = 1791464700},
    };

    for (unsigned int idx = 0; idx < LENGTH_OF(backups); idx++)
    {
        InfoBackupData backup = backups[idx];
        backup.optionOnline = true;
        backup.backupLabel = strDup(backup.backupLabel);
        backup.backupLsnStop = strDup(backup.backupLsnStop);
        backup.backupPrior = strDup(backup.backupPrior);
        lstAdd(((InfoBackupPub *)result)->backup, &backup);
    }

    return result;
}

static StringList *
forkTestConfig(const char *const type, const char *const target)
{
    StringList *const args = strLstNew();
    hrnCfgArgRawZ(args, cfgOptStanza, "child");
    hrnCfgArgRawZ(args, cfgOptSourceStanza, "production");
    hrnCfgArgRawZ(args, cfgOptRepoPath, TEST_PATH "/repo");
    hrnCfgArgRawZ(args, cfgOptType, type);
    hrnCfgArgRawZ(args, cfgOptTarget, target);
    return args;
}

static StringList *
forkTestPublishConfig(void)
{
    StringList *const args = forkTestConfig("lsn", "0/9000000");
    hrnCfgArgKeyRawZ(args, cfgOptRepoPath, 2, TEST_PATH "/own");
    hrnCfgArgKeyRawZ(args, cfgOptRepoCipherType, 2, "aes-256-cbc");
    hrnCfgArgRawZ(args, cfgOptRepo, "2");
    hrnCfgArgRawZ(args, cfgOptSourceRepo, "1");
    return args;
}

// This fixture has repository metadata and a WAL header. It has no PostgreSQL data directory or executable.
static void
forkTestSource(const bool history)
{
    const Storage *const storage = storageRepoIdxWrite(0);
    InfoBackup *const backup = infoBackupNew(
        PG_VERSION_16, HRN_PG_SYSTEMID_16, hrnPgCatalogVersion(PG_VERSION_16), REPOSITORY_FORMAT_6, NULL);
    InfoArchive *const archive = infoArchiveNew(PG_VERSION_16, HRN_PG_SYSTEMID_16, REPOSITORY_FORMAT_6, NULL);
    infoArchiveSaveFile(archive, storage, STRDEF("archive/production/archive.info"), cipherSpecNewNone());

    if (history)
        HRN_STORAGE_PUT_Z(storage, "archive/production/16-1/00000002.history", "1\t0/8000000\tprevious promotion\n");

    for (unsigned int idx = 0; idx < 2; idx++)
    {
        const char *const label = idx == 0 ? FORK_TEST_FULL : FORK_TEST_INCR;
        const String *const path = strNewFmt("backup/production/%s/backup.manifest", label);
        const String *const content = strNewFmt(
            "[backup]\n"
            "backup-archive-start=\"000000010000000000000001\"\n"
            "backup-archive-stop=\"000000010000000000000003\"\n"
            "backup-label=\"%s\"\n"
            "backup-lsn-start=\"0/1000028\"\n"
            "backup-lsn-stop=\"0/3000100\"\n"
            "%s"
            "backup-timestamp-start=1791453600\n"
            "backup-timestamp-stop=1791453900\n"
            "backup-type=\"%s\"\n"
            "\n"
            "[backup:db]\n"
            "db-catalog-version=%u\n"
            "db-control-version=%u\n"
            "db-id=1\n"
            "db-system-id=" HRN_PG_SYSTEMID_16_Z "\n"
            "db-version=\"16\"\n"
            "\n"
            "[backup:option]\n"
            "option-online=true\n"
            "\n"
            "[backup:target]\n"
            "pg_data={\"path\":\"/must-not-open/source-pgdata\",\"type\":\"path\"}\n"
            "\n"
            "[target:path]\n"
            "pg_data={}\n"
            "\n"
            "[target:path:default]\n"
            "group=false\n"
            "mode=\"0700\"\n"
            "user=false\n",
            label, idx == 0 ? "" : "backup-prior=\"" FORK_TEST_FULL "\"\n", idx == 0 ? "full" : "incr",
            hrnPgCatalogVersion(PG_VERSION_16), pgControlVersion(PG_VERSION_16));
        HRN_INFO_PUT(storage, strZ(path), strZ(content), .format = REPOSITORY_FORMAT_6);
        infoBackupDataAdd(backup, manifestLoadFile(storage, path, cipherSpecNewNone()));
    }

    infoBackupSaveFile(backup, storage, STRDEF("backup/production/backup.info"), cipherSpecNewNone());
    Buffer *const header = bufNew(PG_WAL_HEADER_SIZE);
    HRN_PG_WAL_TO_BUFFER(header, PG_VERSION_16);
    bufUsedSet(header, PG_WAL_HEADER_SIZE);
    HRN_STORAGE_PUT(
        storage, "archive/production/16-1/0000000100000000/"
        "000000010000000000000001-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", header);
    HRN_STORAGE_PUT_Z(storage, "backup/production/" FORK_TEST_FULL "/bundle/1", "source payload");
}

static void
testRun(void)
{
    // *****************************************************************************************************************************
    if (testBegin("select a completed backup by time, LSN, or label"))
    {
        InfoBackup *const info = forkTestBackup();
        HRN_CFG_LOAD(cfgCmdFork, forkTestConfig("time", "2026-10-08 11:30:00+00"));
        TEST_RESULT_STR_Z(
            forkBackupSelect(info, 0, NULL, 0), FORK_TEST_DIFF, "time selects the latest backup completed before the target");
        HRN_CFG_LOAD(cfgCmdFork, forkTestConfig("time", "2026-10-08 14:30:00+03"));
        TEST_RESULT_STR_Z(
            forkBackupSelect(info, 0, NULL, 0), FORK_TEST_DIFF, "an explicit time zone denotes the same recovery target");
        HRN_CFG_LOAD(cfgCmdFork, forkTestConfig("time", "2026-10-08 11:05:00.999999+00"));
        TEST_RESULT_STR_Z(
            forkBackupSelect(info, 0, NULL, 0), FORK_TEST_EARLY,
            "a backup that ends within the target second cannot displace an older eligible backup");
        HRN_CFG_LOAD(cfgCmdFork, forkTestConfig("time", "2026-10-08 10:04:59+00"));
        TEST_ERROR(
            forkBackupSelect(info, 0, NULL, 0), BackupSetInvalidError,
            "no source backup on the selected timeline finishes before the requested recovery target");

        HRN_CFG_LOAD(cfgCmdFork, forkTestConfig("lsn", "0/40000FF"));
        TEST_RESULT_STR_Z(
            forkBackupSelect(info, 0, NULL, 0), FORK_TEST_DIFF, "LSN rejects a backup that finishes just after the target");
        HRN_CFG_LOAD(cfgCmdFork, forkTestConfig("lsn", "0/4000100"));
        TEST_RESULT_STR_Z(forkBackupSelect(info, 0, NULL, 0), FORK_TEST_INCR, "LSN accepts a backup at its consistency boundary");
        HRN_CFG_LOAD(cfgCmdFork, forkTestConfig("lsn", "0/9000000"));
        TEST_RESULT_STR_Z(
            forkBackupSelect(info, 0, NULL, 0), FORK_TEST_INCR, "missing stop LSN cannot make a newer backup eligible");

        StringList *args = forkTestConfig("time", "2026-10-08 14:00:00+00");
        hrnCfgArgRawZ(args, cfgOptSet, FORK_TEST_FULL);
        HRN_CFG_LOAD(cfgCmdFork, args);
        TEST_RESULT_STR_Z(forkBackupSelect(info, 0, NULL, 0), FORK_TEST_FULL, "explicit older backup remains selected");
        args = forkTestConfig("time", "2026-10-08 11:00:00+00");
        hrnCfgArgRawZ(args, cfgOptSet, FORK_TEST_INCR);
        HRN_CFG_LOAD(cfgCmdFork, args);
        TEST_ERROR(
            forkBackupSelect(info, 0, NULL, 0), BackupSetInvalidError,
            "no source backup on the selected timeline finishes before the requested recovery target");
        args = forkTestConfig("time", "2026-10-08 14:00:00+00");
        hrnCfgArgRawZ(args, cfgOptSet, "latest");
        HRN_CFG_LOAD(cfgCmdFork, args);
        TEST_RESULT_STR_Z(forkBackupSelect(info, 0, NULL, 0), FORK_TEST_LATE, "explicit latest matches default selection");
        infoBackupDataByLabel(info, STRDEF(FORK_TEST_LATE))->optionOnline = false;
        TEST_RESULT_STR_Z(
            forkBackupSelect(info, 0, NULL, 0), FORK_TEST_INCR, "a newer offline backup cannot hide an eligible online backup");

        for (unsigned int idx = 0; idx < infoBackupDataTotal(info); idx++)
        {
            InfoBackupData *const backup = lstGet(((InfoBackupPub *)info)->backup, idx);
            backup->backupPgId = 1;
            backup->optionOnline = true;
            backup->backupLsnStart = STRDEF("0/1000000");
            backup->backupArchiveStart = STRDEF("000000010000000000000001");
            backup->backupArchiveStop = STRDEF("000000010000000000000004");
        }

        const String *const history = STRDEF("1\t0/3800000\tswitch\n2\t0/5000000\tswitch\n");
        TEST_RESULT_STR_Z(
            forkBackupSelect(info, 1, history, 3), FORK_TEST_DIFF,
            "skip a newer backup which becomes consistent after its timeline diverges");
        TEST_RESULT_STR_Z(
            forkBackupSelect(info, 1, STRDEF("1\t0/3000080\tswitch\n"), 2), FORK_TEST_EARLY,
            "check consistency LSN rather than only the backup start");

        InfoBackupData *const diff = infoBackupDataByLabel(info, STRDEF(FORK_TEST_DIFF));
        diff->backupArchiveStart = STRDEF("000000040000000000000001");
        diff->backupArchiveStop = STRDEF("000000040000000000000004");
        TEST_RESULT_STR_Z(
            forkBackupSelect(info, 1, history, 3), FORK_TEST_EARLY, "skip a backup from an unrelated sibling timeline");

        args = forkTestConfig("time", "2026-10-08 14:00:00+00");
        hrnCfgArgRawZ(args, cfgOptSet, FORK_TEST_DIFF);
        HRN_CFG_LOAD(cfgCmdFork, args);
        TEST_ERROR(
            forkBackupSelect(info, 1, history, 3), BackupSetInvalidError,
            "no source backup on the selected timeline finishes before the requested recovery target");

        HRN_CFG_LOAD(cfgCmdFork, forkTestConfig("lsn", "0/6000100"));
        InfoBackupData *const late = infoBackupDataByLabel(info, STRDEF(FORK_TEST_LATE));
        late->backupLsnStart = STRDEF("0/5000100");
        late->backupLsnStop = STRDEF("0/6000100");
        late->backupArchiveStart = STRDEF("000000030000000000000005");
        late->backupArchiveStop = STRDEF("000000030000000000000006");
        TEST_RESULT_STR_Z(
            forkBackupSelect(info, 1, history, 3), FORK_TEST_LATE, "a consistent backup on the selected leaf remains eligible");
        late->backupLabel = STRDEF("../other");
        TEST_ERROR(
            forkBackupSelect(info, 1, history, 3), FormatError, "invalid source backup label '../other'");
    }

    // *****************************************************************************************************************************
    if (testBegin("import the backup chain and file references"))
    {
        InfoBackup *const info = forkTestBackup();
        StringList *const labels = strLstNew();
        TEST_RESULT_VOID(forkBackupDependency(info, STRDEF(FORK_TEST_INCR), labels), "collect transitive dependencies");
        strLstSort(labels, sortOrderAsc);
        TEST_RESULT_STRLST_Z(
            labels, FORK_TEST_FULL "\n" FORK_TEST_EARLY "\n" FORK_TEST_DIFF "\n" FORK_TEST_INCR "\n",
            "prior chain keeps the full and differential while a separate physical reference adds the early increment");
        TEST_RESULT_VOID(forkBackupDependency(info, STRDEF(FORK_TEST_INCR), labels), "revisiting the selection is idempotent");
        TEST_RESULT_UINT(strLstSize(labels), 4, "shared physical references do not duplicate imported manifests");
        TEST_RESULT_BOOL(strLstExists(labels, STRDEF(FORK_TEST_LATE)), false, "unrelated full is not imported");
        TEST_ERROR(
            forkBackupDependency(info, STRDEF("20261007-100000F"), labels), BackupSetInvalidError,
            "source backup 20261007-100000F is missing");
        TEST_ERROR(
            forkBackupDependency(info, STRDEF("nested/not-a-backup"), labels), FormatError,
            "invalid source backup label 'nested/not-a-backup'");

        infoBackupDataDelete(info, STRDEF(FORK_TEST_DIFF));
        TEST_ERROR(
            forkBackupDependency(info, STRDEF(FORK_TEST_INCR), strLstNew()), BackupSetInvalidError,
            "source backup " FORK_TEST_DIFF " is missing");
    }

    // *****************************************************************************************************************************
    if (testBegin("find source WAL, decrypt it, and decompress it"))
    {
        const Storage *const storage = storagePosixNewP(TEST_PATH_STR, .write = true);
        const String *const segment = STRDEF("000000010000000000000001");
        TEST_ERROR(
            forkSourceWalPath(storage, STRDEF("production"), STRDEF("16-1"), segment), FileMissingError,
            "source WAL segment 000000010000000000000001 is missing");
        const String *const path = STRDEF(
            "archive/production/16-1/0000000100000000/000000010000000000000001-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.gz");

        for (unsigned int format = REPOSITORY_FORMAT_5; format <= REPOSITORY_FORMAT_6; format++)
        {
            const CipherSpec *const cipher = cipherSpecGen(cipherTypeAes256Cbc, format);
            CipherSpecMap *const map = cipherSpecMapNew();
            if (format == REPOSITORY_FORMAT_5)
                cipherSpecMapAdd(map, CIPHER_SPEC_MAP_ID_DEFAULT_STR, cipher);
            InfoArchive *const archive = infoArchiveNew(PG_VERSION_16, HRN_PG_SYSTEMID_16, format, map);
            if (format == REPOSITORY_FORMAT_6)
                infoArchiveCipherRotate(archive, cipher, 1791453900);
            StorageWrite *const write = storageNewWriteP(storage, path);
            ioFilterGroupAdd(ioWriteFilterGroup(storageWriteIo(write)), compressFilterP(compressTypeGz, 1));
            cipherBlockFormatFilterGroupWriteAddP(
                ioWriteFilterGroup(storageWriteIo(write)), cipher, format,
                .keyId = cipherSpecMapIdCurrent(infoArchiveCipherSpecMap(archive)));
            storagePutP(write, BUFSTRDEF("original source WAL bytes"));
            TEST_RESULT_STR(
                forkSourceWalPath(storage, STRDEF("production"), STRDEF("16-1"), segment), path,
                "source lookup returns the exact object");
            IoRead *const read = forkArchiveRead(storage, path, infoArchiveCipherSpecMap(archive));
            ioReadOpen(read);
            TEST_RESULT_STR_Z(
                strNewBuf(ioReadBuf(read)), "original source WAL bytes", "repository format and gzip decode correctly");
            ioReadClose(read);
        }

        HRN_STORAGE_PUT_EMPTY(
            storage, "archive/production/16-1/0000000100000000/"
            "000000010000000000000001-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
        TEST_ERROR(
            forkSourceWalPath(storage, STRDEF("production"), STRDEF("16-1"), segment), ArchiveDuplicateError,
            "source WAL segment 000000010000000000000001 has multiple copies");
    }

    // *****************************************************************************************************************************
    if (testBegin("publish fork metadata and keep it unchanged on retry"))
    {
        hrnCfgEnvKeyRawZ(cfgOptRepoCipherPass, 2, TEST_CIPHER_PASS);
        StringList *const args = forkTestPublishConfig();
        HRN_CFG_LOAD(cfgCmdFork, args);
        forkTestSource(false);
        TEST_RESULT_VOID(cmdFork(), "publish inherited metadata without pg-path or PostgreSQL binaries");
        lockReleaseP();

        const Storage *const storage = storageRepoIdx(1);
        const InfoFork *const info = forkInfoLoad(1);
        TEST_RESULT_UINT(infoForkData(info)->state, infoForkStatePublished, "repository metadata is published");
        TEST_RESULT_UINT(infoForkData(info)->ownTimeline, 0, "publication neither starts PostgreSQL nor creates a branch");
        TEST_RESULT_UINT(infoForkData(info)->walSegmentSize, 16 * 1024 * 1024, "segment size comes from the source WAL header");
        TEST_RESULT_STR_Z(infoForkData(info)->backupLabel, FORK_TEST_INCR, "selected backup is saved for standalone restore");
        TEST_RESULT_UINT(strLstSize(infoForkData(info)->importedBackupList), 2, "prior full is retained");
        TEST_RESULT_BOOL(
            storageExistsP(storage, STRDEF(STORAGE_REPO_BACKUP "/" FORK_TEST_FULL "/bundle/1")), false,
            "backup payload remains in the source");
        const String *const pin = strNewFmt("backup/production/fork-pin/%s", strZ(forkPinName(1, STRDEF("child"))));
        TEST_RESULT_BOOL(storageExistsP(storageRepoIdx(0), pin), true, "source is pinned before references become usable");

        const Buffer *const descriptor = storageGetP(storageNewReadP(storage, STRDEF(INFO_FORK_PATH_FILE)));
        const Buffer *const archive = storageGetP(storageNewReadP(storage, INFO_ARCHIVE_PATH_FILE_STR));
        const Buffer *const backup = storageGetP(storageNewReadP(storage, INFO_BACKUP_PATH_FILE_STR));
        const Buffer *const manifest = storageGetP(
            storageNewReadP(storage, STRDEF(STORAGE_REPO_BACKUP "/" FORK_TEST_INCR "/backup.manifest")));
        const Buffer *const pinned = storageGetP(storageNewReadP(storageRepoIdx(0), pin));
        storageRemoveP(storageRepoIdxWrite(0), STRDEF("backup/production/backup.info"));
        storageRemoveP(storageRepoIdxWrite(0), STRDEF("backup/production/backup.info.copy"));
        TEST_RESULT_VOID(cmdFork(), "published retry does not require source metadata");
        lockReleaseP();
        TEST_RESULT_BOOL(
            bufEq(descriptor, storageGetP(storageNewReadP(storage, STRDEF(INFO_FORK_PATH_FILE)))), true,
            "published descriptor is unchanged byte for byte");
        TEST_RESULT_BOOL(
            bufEq(archive, storageGetP(storageNewReadP(storage, INFO_ARCHIVE_PATH_FILE_STR))), true, "archive keys unchanged");
        TEST_RESULT_BOOL(
            bufEq(backup, storageGetP(storageNewReadP(storage, INFO_BACKUP_PATH_FILE_STR))), true, "manifest keys unchanged");
        TEST_RESULT_BOOL(
            bufEq(
                manifest, storageGetP(storageNewReadP(storage, STRDEF(STORAGE_REPO_BACKUP "/" FORK_TEST_INCR "/backup.manifest")))),
            true, "copied manifest is unchanged");
        TEST_RESULT_BOOL(bufEq(pinned, storageGetP(storageNewReadP(storageRepoIdx(0), pin))), true, "pin is unchanged");

        cfgOptionSet(cfgOptTargetTimeline, cfgSourceParam, VARSTRDEF("999"));
        TEST_ERROR(cmdFork(), OptionInvalidError, "existing fork has a different target timeline");
        lockReleaseP();
        cfgOptionSet(cfgOptTargetTimeline, cfgSourceParam, VARSTRDEF("current"));
        TEST_RESULT_VOID(cmdFork(), "matching current timeline is resolved from the saved destination manifest");
        lockReleaseP();
        cfgOptionSet(cfgOptTarget, cfgSourceParam, VARSTRDEF("0/A000000"));
        TEST_ERROR(cmdFork(), OptionInvalidError, "existing fork has a different source, target or backup");
        lockReleaseP();
        TEST_RESULT_LOG(
            "P00   INFO: fork stanza 'child' from 'production', backup " FORK_TEST_INCR
            ": copied 2 manifests; backup data remains in the source repository\n"
            "P00   INFO: fork stanza 'child' is already published\n"
            "P00   INFO: fork stanza 'child' is already published");
        hrnCfgEnvKeyRemoveRaw(cfgOptRepoCipherPass, 2);
    }

    // *****************************************************************************************************************************
    if (testBegin("resume metadata publication with the saved encryption keys"))
    {
        hrnCfgEnvKeyRawZ(cfgOptRepoCipherPass, 2, TEST_CIPHER_PASS);
        HRN_CFG_LOAD(cfgCmdFork, forkTestPublishConfig());
        forkTestSource(true);
        const String *const fullPath = STRDEF("backup/production/" FORK_TEST_FULL "/backup.manifest");
        const Buffer *const full = storageGetP(storageNewReadP(storageRepoIdx(0), fullPath));
        storageRemoveP(storageRepoIdxWrite(0), fullPath);
        TEST_ERROR(
            cmdFork(), FileMissingError,
            "unable to load backup manifest file '" TEST_PATH "/repo/backup/production/" FORK_TEST_FULL "/backup.manifest' or '"
            TEST_PATH "/repo/backup/production/" FORK_TEST_FULL "/backup.manifest.copy':\n"
            "FileMissingError: unable to open missing file '" TEST_PATH "/repo/backup/production/" FORK_TEST_FULL
            "/backup.manifest' for read\n"
            "FileMissingError: unable to open missing file '" TEST_PATH "/repo/backup/production/" FORK_TEST_FULL
            "/backup.manifest.copy' for read");
        lockReleaseP();
        TEST_RESULT_UINT(
            infoForkData(forkInfoLoad(1))->state, infoForkStatePreparing, "partial metadata is unavailable to consumers");
        const InfoBackup *const backup = infoBackupLoadFile(
            storageRepoIdx(1), INFO_BACKUP_PATH_FILE_STR, cfgCipherSpecMainIdx(1));
        const InfoArchive *const archive = infoArchiveLoadFile(
            storageRepoIdx(1), INFO_ARCHIVE_PATH_FILE_STR, cfgCipherSpecMainIdx(1));
        const Buffer *const manifestKey = bufDup(cipherSpecPass(infoBackupCipherSpec(backup)));
        const CipherSpecMap *const archiveMap = infoArchiveCipherSpecMap(archive);
        const String *const archiveKeyId = strDup(cipherSpecMapIdCurrent(archiveMap));
        const Buffer *const archiveKey = bufDup(cipherSpecPass(cipherSpecMapGet(archiveMap, archiveKeyId)));
        storagePutP(storageNewWriteP(storageRepoIdxWrite(0), fullPath), full);
        TEST_RESULT_VOID(cmdFork(), "complete an interrupted publication");
        lockReleaseP();
        TEST_RESULT_UINT(infoForkData(forkInfoLoad(1))->state, infoForkStatePublished, "retry publishes complete metadata");
        TEST_RESULT_BOOL(
            bufEq(
                manifestKey,
                cipherSpecPass(infoBackupCipherSpec(
                                   infoBackupLoadFile(
                                       storageRepoIdx(1), INFO_BACKUP_PATH_FILE_STR, cfgCipherSpecMainIdx(1))))), true,
            "retry preserves the manifest encryption key");
        const InfoArchive *const resumedArchive = infoArchiveLoadFile(
            storageRepoIdx(1), INFO_ARCHIVE_PATH_FILE_STR, cfgCipherSpecMainIdx(1));
        TEST_RESULT_BOOL(
            bufEq(archiveKey, cipherSpecPass(cipherSpecMapGet(infoArchiveCipherSpecMap(resumedArchive), archiveKeyId))), true,
            "retry preserves the archive encryption key");
        IoRead *const history = forkArchiveRead(
            storageRepoIdx(1), STRDEF(STORAGE_REPO_ARCHIVE "/16-1/00000002.history"), infoArchiveCipherSpecMap(resumedArchive));
        ioReadOpen(history);
        TEST_RESULT_STR_Z(
            strNewBuf(ioReadBuf(history)), "1\t0/8000000\tprevious promotion\n", "copied history uses the current archive key");
        ioReadClose(history);
        TEST_RESULT_STR_Z(
            manifestData(forkSourceManifestLoad(1, STRDEF("child"), STRDEF(FORK_TEST_FULL)))->backupLabel,
            FORK_TEST_FULL, "copied manifests remain readable with the persisted key");

        InfoFork *const published = forkInfoLoad(1);
        InfoForkData preparing = *infoForkData(published);
        preparing.state = infoForkStatePreparing;
        infoForkSaveFile(
            infoForkNew(infoForkFormat(published), &preparing), storageRepoIdxWrite(1), STRDEF(INFO_FORK_PATH_FILE),
            cfgCipherSpecMainIdx(1));
        HRN_STORAGE_PATH_CREATE(storageRepoIdxWrite(1), INFO_FORK_PATH_FILE "." STORAGE_FILE_TEMP_EXT);
        TEST_ERROR(
            infoForkSaveFile(published, storageRepoIdxWrite(1), STRDEF(INFO_FORK_PATH_FILE), cfgCipherSpecMainIdx(1)),
            FileOpenError, "unable to open file '" TEST_PATH "/own/backup/child/fork.info' for write: [21] Is a directory");
        TEST_RESULT_UINT(
            infoForkData(forkInfoLoad(1))->state, infoForkStatePreparing,
            "failed primary commit keeps the published copy from superseding preparing metadata");
        TEST_RESULT_UINT(
            infoForkData(infoForkLoadFile(
                             storageRepoIdx(1), STRDEF(INFO_FORK_PATH_FILE INFO_COPY_EXT), cfgCipherSpecMainIdx(1)))->state,
            infoForkStatePublished, "recovery copy was written before the failed primary commit");
        HRN_STORAGE_PATH_REMOVE(storageRepoIdxWrite(1), INFO_FORK_PATH_FILE "." STORAGE_FILE_TEMP_EXT);
        TEST_RESULT_VOID(cmdFork(), "resume a failed final publication commit");
        lockReleaseP();
        TEST_RESULT_UINT(infoForkData(forkInfoLoad(1))->state, infoForkStatePublished, "retry commits published primary metadata");
        TEST_RESULT_LOG(
            "P00   INFO: fork stanza 'child' from 'production', backup " FORK_TEST_INCR
            ": copied 2 manifests; backup data remains in the source repository\n"
            "P00   INFO: fork stanza 'child' from 'production', backup " FORK_TEST_INCR
            ": copied 2 manifests; backup data remains in the source repository");
        hrnCfgEnvKeyRemoveRaw(cfgOptRepoCipherPass, 2);
    }

    // *****************************************************************************************************************************
    if (testBegin("check fork options and require an empty destination"))
    {
        HRN_CFG_LOAD(cfgCmdFork, forkTestConfig("time", "2026-10-08 11:30:00"));
        TEST_ERROR(
            cmdFork(), OptionInvalidError,
            "fork time target requires an explicit numeric time zone, for example +00 or +03:00");
        HRN_CFG_LOAD(cfgCmdFork, forkTestConfig("lsn", "0/9000000"));
        cfgOptionSet(cfgOptSourceStanza, cfgSourceParam, VARSTRDEF("child"));
        TEST_ERROR(cmdFork(), OptionInvalidError, "source and destination stanza names must differ");
        cfgOptionSet(cfgOptSourceStanza, cfgSourceParam, VARSTRDEF("production"));
        HRN_STORAGE_PUT_EMPTY(storageRepoIdxWrite(0), STORAGE_REPO_BACKUP "/existing");
        TEST_ERROR(cmdFork(), PathNotEmptyError, "destination stanza is not empty");
        lockReleaseP();
        TEST_RESULT_BOOL(
            storageExistsP(storageRepoIdx(0), STRDEF(INFO_FORK_PATH_FILE)), false, "failed validation publishes no descriptor");
    }
}
