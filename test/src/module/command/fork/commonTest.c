/***********************************************************************************************************************************
Test Fork Repository Bindings
***********************************************************************************************************************************/
#include "common/exec.h"
#include "common/format/format.h"
#include "info/infoArchive.h"
#include "postgres/version.h"
#include "version.h"

#include "harness/config.h"
#include "harness/info.h"
#include "harness/postgres.h"
#include "harness/storage.h"

/**********************************************************************************************************************************/
static StringList *
testForkConfig(const bool swap)
{
    StringList *const args = strLstNew();
    hrnCfgArgRawZ(args, cfgOptStanza, "child");
    hrnCfgArgRawZ(args, cfgOptPgPath, TEST_PATH "/pg");
    hrnCfgArgKeyRawZ(args, cfgOptRepoPath, swap ? 2 : 1, TEST_PATH "/own");
    hrnCfgArgKeyRawZ(args, cfgOptRepoPath, swap ? 1 : 2, TEST_PATH "/source");
    hrnCfgArgRawZ(args, cfgOptRepo, swap ? "2" : "1");
    return args;
}

static InfoFork *
testForkInfo(const unsigned int sourceIdx)
{
    const InfoForkData data =
    {
        .sourceId = forkSourceId(sourceIdx, STRDEF("production"), STRDEF("16-1"), 1234),
        .sourceRepoKey = sourceIdx + 1,
        .sourceStanza = STRDEF("production"),
        .sourceArchiveId = STRDEF("16-1"),
        .backupLabel = STRDEF("20261008-120000F"),
        .importedBackupList = strLstNewSplitZ(STRDEF("20261008-120000F"), ","),
        .pgVersion = PG_VERSION_16,
        .systemId = 1234,
        .walSegmentSize = 16 * 1024 * 1024,
        .sourceTimeline = 1,
        .sourceHistory = EMPTY_STR,
        .targetType = STRDEF("lsn"),
        .targetValue = STRDEF("0/3000000"),
        .state = infoForkStatePublished,
        .history = EMPTY_STR,
    };
    return infoForkNew(REPOSITORY_FORMAT_DEFAULT, &data);
}

static StringList *
testForkS3Config(const char *const host, const char *const port)
{
    StringList *const args = testForkConfig(false);
    hrnCfgArgKeyRawZ(args, cfgOptRepoType, 2, "s3");
    hrnCfgArgKeyRawZ(args, cfgOptRepoS3Bucket, 2, "source-bucket");
    hrnCfgArgKeyRawZ(args, cfgOptRepoS3Endpoint, 2, "archive.example.test");
    hrnCfgArgKeyRawZ(args, cfgOptRepoS3Region, 2, "test-region");
    hrnCfgArgKeyRawZ(args, cfgOptRepoStorageHost, 2, host);
    hrnCfgArgKeyRawZ(args, cfgOptRepoStoragePort, 2, port);
    return args;
}

/**********************************************************************************************************************************/
static void
testRun(void)
{
    // *****************************************************************************************************************************
    if (testBegin("configuration changes preserve source identity and reset cached storage"))
    {
        HRN_CFG_LOAD(cfgCmdRestore, testForkConfig(false));
        InfoFork *const info = testForkInfo(1);
        const String *const pinName = forkPinName(0, STRDEF("child"));
        InfoForkData preparing = *infoForkData(info);
        preparing.state = infoForkStatePreparing;
        infoForkSaveFile(
            infoForkNew(REPOSITORY_FORMAT_DEFAULT, &preparing), storageRepoIdxWrite(0), STRDEF(INFO_FORK_PATH_FILE),
            cfgCipherSpecMainIdx(0));
        TEST_ERROR(
            forkInfoLoad(0), OptionInvalidError,
            "fork metadata is incomplete; repeat the fork command before using this stanza");
        infoForkSaveFile(info, storageRepoIdxWrite(0), STRDEF(INFO_FORK_PATH_FILE), cfgCipherSpecMainIdx(0));
        HRN_STORAGE_PUT_Z(storageRepoIdxWrite(1), "backup/production/20261008-120000F/bundle/1", "source payload");

        TEST_RESULT_UINT(forkSourceRepoIdx(info), 1, "initial binding selects source repository");
        TEST_RESULT_PTR(forkStorageRepoIdx(1), storageRepoIdx(1), "ordinary stanza keeps its original storage");
        const Storage *const view = forkStorageRepoIdx(0);
        TEST_RESULT_PTR(forkStorageRepoIdx(0), view, "repeated reads reuse the same view");
        TEST_RESULT_STR_Z(
            strNewBuf(storageGetP(storageNewReadP(view, STRDEF(STORAGE_REPO_BACKUP "/20261008-120000F/bundle/1")))),
            "source payload", "cached view reads the source object");

        TEST_RESULT_VOID(storageHelperFree(), "free the storage helper and its child cache");
        TEST_RESULT_PTR(forkStorageContext, NULL, "storage helper cleanup invalidates the fork cache");
        TEST_RESULT_PTR(forkStorageCache, NULL, "storage helper cleanup resets cached entries");

        HRN_CFG_LOAD(cfgCmdRestore, testForkConfig(true));
        TEST_RESULT_UINT(forkSourceRepoIdx(info), 0, "saved repository key is only a hint after renumbering");
        TEST_RESULT_STR(forkPinName(1, STRDEF("child")), pinName, "pin identity does not depend on repository numbering");
        TEST_RESULT_STR_Z(
            strNewBuf(storageGetP(storageNewReadP(
                                      forkStorageRepoIdx(1), STRDEF(STORAGE_REPO_BACKUP "/20261008-120000F/bundle/1")))),
            "source payload", "a new configuration builds a view bound to the reordered source");

        StringList *const wrong = strLstNew();
        hrnCfgArgRawZ(wrong, cfgOptStanza, "child");
        hrnCfgArgRawZ(wrong, cfgOptPgPath, TEST_PATH "/pg");
        hrnCfgArgRawZ(wrong, cfgOptRepoPath, TEST_PATH "/different-source");
        HRN_CFG_LOAD(cfgCmdRestore, wrong);
        TEST_ERROR_FMT(
            forkSourceRepoIdx(info), OptionInvalidError,
            "repository for fork source 'production' is not configured (source id %s)", strZ(infoForkData(info)->sourceId));
    }

    // *****************************************************************************************************************************
    if (testBegin("cached storage opens only the requested repository"))
    {
        StringList *const args = strLstNew();
        hrnCfgArgRawZ(args, cfgOptStanza, "child");
        hrnCfgArgRawZ(args, cfgOptPgPath, TEST_PATH "/pg");
        hrnCfgArgRawZ(args, cfgOptRepo, "2");
        hrnCfgArgKeyRawZ(args, cfgOptRepoHost, 1, "unused.example.test");
        hrnCfgArgKeyRawZ(args, cfgOptRepoPath, 2, TEST_PATH "/standalone");
        HRN_CFG_LOAD(cfgCmdRestore, args);
        TEST_RESULT_PTR(forkStorageRepoIdx(1), storageRepoIdx(1), "unused remote repository is not opened");
    }

    // *****************************************************************************************************************************
    if (testBegin("source paths reject traversal and manifests use the source encryption key"))
    {
        const String *const stanza = STRDEF("production");
        TEST_RESULT_STR_Z(forkSourcePath(stanza, STORAGE_PATH_ARCHIVE, NULL), "archive/production", "archive root");
        TEST_RESULT_STR_Z(
            forkSourcePath(stanza, STORAGE_PATH_ARCHIVE, STRDEF("16-1/00000001.history")),
            "archive/production/16-1/00000001.history", "qualified history path");
        TEST_RESULT_STR_Z(
            forkSourcePath(stanza, STORAGE_PATH_BACKUP, STRDEF("20261008-120000F/bundle/7")),
            "backup/production/20261008-120000F/bundle/7", "qualified bundle path");
        TEST_ERROR(
            forkSourcePath(STRDEF("../other"), STORAGE_PATH_BACKUP, NULL), FormatError, "invalid source name '../other'");
        TEST_ERROR(forkSourcePath(STRDEF(".."), STORAGE_PATH_BACKUP, NULL), FormatError, "invalid source path '..'");
        TEST_ERROR(
            forkSourcePath(stanza, STORAGE_PATH_BACKUP, STRDEF("../other/backup.info")), FormatError,
            "invalid source path '../other/backup.info'");
        TEST_ERROR(
            forkSourcePath(stanza, STORAGE_PATH_BACKUP, STRDEF("/backup.info")), FormatError,
            "invalid source path '/backup.info'");
        TEST_ERROR(
            forkSourcePath(stanza, STORAGE_PATH_BACKUP, STRDEF("a//b")), FormatError, "invalid source path 'a//b'");
        TEST_ERROR(
            forkSourceManifestLoad(1, stanza, STRDEF("../backup.manifest")), FormatError,
            "invalid source backup label '../backup.manifest'");

        StringList *const args = testForkConfig(false);
        hrnCfgArgKeyRawZ(args, cfgOptRepoCipherType, 2, "aes-256-cbc");
        hrnCfgEnvKeyRawZ(cfgOptRepoCipherPass, 2, "source-main");
        HRN_CFG_LOAD(cfgCmdRestore, args);
        const Storage *const source = storageRepoIdxWrite(1);
        const CipherSpec *const sourceMain = cfgCipherSpecMainIdx(1);
        const CipherSpec *const manifestKey = cipherSpecNewP(
            cipherTypeAes256Cbc, BUFSTRDEF("source-manifest"), .digest = hashTypeSha256);
        InfoArchive *const sourceArchive = infoArchiveNew(PG_VERSION_16, 1001, REPOSITORY_FORMAT_6, NULL);
        InfoArchive *const ownArchive = infoArchiveNew(PG_VERSION_16, 2002, REPOSITORY_FORMAT_DEFAULT, NULL);
        InfoBackup *const sourceBackup = infoBackupNew(
            PG_VERSION_16, 1001, hrnPgCatalogVersion(PG_VERSION_16), REPOSITORY_FORMAT_6, manifestKey);
        infoArchiveSaveFile(sourceArchive, source, STRDEF("archive/production/archive.info"), sourceMain);
        infoArchiveSaveFile(ownArchive, storageRepoIdxWrite(0), INFO_ARCHIVE_PATH_FILE_STR, cfgCipherSpecMainIdx(0));
        infoBackupSaveFile(sourceBackup, source, STRDEF("backup/production/backup.info"), sourceMain);
        HRN_INFO_PUT(
            source, "backup/production/20261008-120000F/backup.manifest",
            "[backup]\n"
            "backup-label=\"20261008-120000F\"\n"
            "backup-type=\"full\"\n"
            "\n"
            "[backup:target]\n"
            "pg_data={\"path\":\"/source/pgdata\",\"type\":\"path\"}\n",
            .cipherSpec = manifestKey, .format = REPOSITORY_FORMAT_6);

        const InfoArchive *const archive = infoArchiveLoadFile(
            source, forkSourcePath(stanza, STORAGE_PATH_ARCHIVE, STRDEF(INFO_ARCHIVE_FILE)), sourceMain);
        TEST_RESULT_UINT(infoPgDataCurrent(infoArchivePg(archive)).systemId, 1001, "source archive uses its own key and stanza");
        TEST_RESULT_UINT(
            infoPgDataCurrent(infoArchivePg(infoArchiveLoadFile(
                                                storageRepoIdx(0), INFO_ARCHIVE_PATH_FILE_STR, cfgCipherSpecMainIdx(0)))).systemId,
            2002, "destination remains independently bound");

        Manifest *manifest;
        MEM_CONTEXT_TEMP_BEGIN()
        {
            TEST_ASSIGN(manifest, forkSourceManifestLoad(1, stanza, STRDEF("20261008-120000F")), "decrypt source manifest");
            manifestMove(manifest, memContextPrior());
        }
        MEM_CONTEXT_TEMP_END();
        TEST_RESULT_STR_Z(manifestData(manifest)->backupLabel, "20261008-120000F", "manifest survives temporary read context");
        TEST_RESULT_STR_Z(manifestTargetBase(manifest)->path, "/source/pgdata", "source manifest content");
        storageRemoveP(source, STRDEF("backup/production/backup.info"));
        TEST_ASSIGN(manifest, forkSourceManifestLoad(1, stanza, STRDEF("20261008-120000F")), "use source metadata recovery copy");
        TEST_RESULT_STR_Z(manifestData(manifest)->backupLabel, "20261008-120000F", "fallback keeps the same source");
        hrnCfgEnvKeyRemoveRaw(cfgOptRepoCipherPass, 2);
    }

    // *****************************************************************************************************************************
    if (testBegin("source stanza pins and their recovery copies protect source backups"))
    {
        HRN_CFG_LOAD(cfgCmdRestore, testForkConfig(false));
        TEST_RESULT_BOOL(forkPinExists(0), false, "no pin directory means no dependency");
        HRN_STORAGE_PATH_CREATE(storageRepoIdxWrite(0), STORAGE_REPO_BACKUP "/" FORK_PIN_PATH);
        TEST_RESULT_BOOL(forkPinExists(0), false, "an empty pin directory does not retain a stanza");
        HRN_STORAGE_PUT_Z(storageRepoIdxWrite(0), "backup/other/fork-pin/child.info", "other stanza dependency");
        TEST_RESULT_BOOL(forkPinExists(0), false, "a pin for another stanza is unrelated");
        HRN_STORAGE_PUT_Z(storageRepoIdxWrite(0), STORAGE_REPO_BACKUP "/" FORK_PIN_PATH "/child.info.copy", "recovery copy");
        TEST_RESULT_BOOL(forkPinExists(0), true, "a pin recovery copy alone prevents data removal");
        TEST_RESULT_BOOL(forkPinExists(1), false, "pins are also repository-local");
        storageRemoveP(storageRepoIdxWrite(0), STRDEF(STORAGE_REPO_BACKUP "/" FORK_PIN_PATH "/child.info.copy"));
        TEST_RESULT_BOOL(forkPinExists(0), false, "the final dependency can be released");
    }

    // *****************************************************************************************************************************
    if (testBegin("repository identity uses endpoint settings and excludes credentials"))
    {
        hrnCfgEnvKeyRawZ(cfgOptRepoS3Key, 2, "access-one");
        hrnCfgEnvKeyRawZ(cfgOptRepoS3KeySecret, 2, "secret-one");
        HRN_CFG_LOAD(cfgCmdRestore, testForkS3Config("127.0.0.1", "9000"));
        InfoFork *const info = testForkInfo(1);
        TEST_RESULT_UINT(forkSourceRepoIdx(info), 1, "direct S3 source binds without opening a network connection");

        hrnCfgEnvKeyRawZ(cfgOptRepoS3Key, 2, "access-two");
        hrnCfgEnvKeyRawZ(cfgOptRepoS3KeySecret, 2, "secret-two");
        HRN_CFG_LOAD(cfgCmdRestore, testForkS3Config("127.0.0.1", "9000"));
        TEST_RESULT_UINT(forkSourceRepoIdx(info), 1, "credential rotation does not change the storage identity");

        HRN_CFG_LOAD(cfgCmdRestore, testForkS3Config("127.0.0.2", "9000"));
        TEST_ERROR_FMT(
            forkSourceRepoIdx(info), OptionInvalidError,
            "repository for fork source 'production' is not configured (source id %s)", strZ(infoForkData(info)->sourceId));
        HRN_CFG_LOAD(cfgCmdRestore, testForkS3Config("127.0.0.1", "9001"));
        TEST_ERROR_FMT(
            forkSourceRepoIdx(info), OptionInvalidError,
            "repository for fork source 'production' is not configured (source id %s)", strZ(infoForkData(info)->sourceId));
        hrnCfgEnvKeyRemoveRaw(cfgOptRepoS3Key, 2);
        hrnCfgEnvKeyRemoveRaw(cfgOptRepoS3KeySecret, 2);

        StringList *const remote = testForkConfig(false);
        hrnCfgArgKeyRawZ(remote, cfgOptRepoHost, 2, "archive.example.test");
        hrnCfgArgKeyRawZ(remote, cfgOptRepoHostPort, 2, "8432");
        HRN_CFG_LOAD(cfgCmdRestore, remote);
        const String *const ssh = forkSourceId(1, STRDEF("production"), STRDEF("16-1"), 1234);
        hrnCfgArgKeyRawZ(remote, cfgOptRepoHostType, 2, "tls");
        hrnCfgArgKeyRawZ(remote, cfgOptRepoHostCertFile, 2, "/not-used");
        hrnCfgArgKeyRawZ(remote, cfgOptRepoHostKeyFile, 2, "/not-used");
        HRN_CFG_LOAD(cfgCmdRestore, remote);
        TEST_RESULT_BOOL(
            strEq(ssh, forkSourceId(1, STRDEF("production"), STRDEF("16-1"), 1234)), false,
            "different remote transport is a different repository binding");
    }

    // *****************************************************************************************************************************
    if (testBegin("archive command quotes the executable and its arguments for the shell"))
    {
        StringList *const args = strLstNew();
        hrnCfgArgRawZ(args, cfgOptStanza, "child");
        hrnCfgArgRawZ(args, cfgOptPgPath, TEST_PATH "/pg");
        hrnCfgArgRawZ(args, cfgOptRepoPath, TEST_PATH "/repo '$HOME`printf bad`");
        hrnCfgArgRawZ(args, cfgOptRepo, "1");
        hrnCfgArgRawZ(args, cfgOptCmd, TEST_PATH "/archive 'tool");
        HRN_CFG_LOAD(cfgCmdRestore, args);
        storagePutP(
            storageNewWriteP(storageLocalWrite(), STRDEF(TEST_PATH "/archive 'tool"), .modeFile = 0700),
            BUFSTRDEF("#!/bin/sh\nprintf '%s\\n' \"$@\"\n"));
        StringList *const shell = strLstNew();
        strLstAddZ(shell, "/bin/sh");
        strLstAddZ(shell, "-c");
        strLstAdd(shell, forkArchiveCommand(cfgCmdArchiveGet));
        const StringList *const actual = strLstNewSplitZ(execOne(shell), "\n");
        TEST_RESULT_BOOL(
            strLstExists(actual, STRDEF("--repo1-path=" TEST_PATH "/repo '$HOME`printf bad`")), true,
            "configured repository path is passed literally without expansion or command substitution");
        TEST_RESULT_BOOL(strLstExists(actual, STRDEF("--repo=1")), true, "destination selection is carried into archive-get");
        TEST_RESULT_STR_Z(strLstGet(actual, strLstSize(actual) - 3), "archive-get", "uses the standard command");
        TEST_RESULT_STR_Z(strLstGet(actual, strLstSize(actual) - 2), "%f", "WAL name placeholder remains for PostgreSQL");
        TEST_RESULT_STR_Z(strLstGet(actual, strLstSize(actual) - 1), "%p", "destination placeholder remains for PostgreSQL");
        TEST_RESULT_STR_Z(
            forkPgConfigQuote(STRDEF("path 'quoted' \\x")), "'path ''quoted'' \\\\x'",
            "shell command is escaped independently when stored in PostgreSQL configuration");
    }
}
