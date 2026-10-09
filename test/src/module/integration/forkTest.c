/***********************************************************************************************************************************
Fork Integration Test
***********************************************************************************************************************************/
#include "common/compress/helper.h"
#include "common/wait.h"
#include "config/config.h"
#include "postgres/interface.h"
#include "postgres/version.h"

#include "harness/errorRetry.h"
#include "harness/host.h"
#include "harness/storage.h"

/***********************************************************************************************************************************
Test definition
***********************************************************************************************************************************/
static HrnHostTestDefine testMatrix[] =
{
    // {uncrustify_off - struct alignment}
    {.pg = "9.6", .repo = "pg2", .stg = "posix", .enc = 0, .cmp = "none", .rt = 2, .bnd = 1, .bi = 1, .repoDirect = true},
    {.pg =  "10", .repo = "pg2", .stg =    "s3", .enc = 1, .cmp =   "gz", .rt = 1, .bnd = 1, .bi = 1, .repoDirect = true},
    {.pg =  "11", .repo = "pg2", .stg = "posix", .enc = 1, .cmp =  "lz4", .rt = 2, .bnd = 1, .bi = 1, .repoDirect = true},
    {.pg =  "12", .repo = "pg2", .stg =    "s3", .enc = 1, .cmp =  "zst", .rt = 1, .bnd = 1, .bi = 1, .repoDirect = true},
    {.pg =  "13", .repo = "pg2", .stg = "posix", .enc = 0, .cmp = "none", .rt = 2, .bnd = 1, .bi = 1, .repoDirect = true},
    {.pg =  "14", .repo = "pg2", .stg =    "s3", .enc = 1, .cmp =  "bz2", .rt = 1, .bnd = 1, .bi = 1, .repoDirect = true},
    {.pg =  "15", .repo = "pg2", .stg = "posix", .enc = 1, .cmp =   "gz", .rt = 2, .bnd = 1, .bi = 1, .repoDirect = true},
    {.pg =  "16", .repo = "pg2", .stg = "posix", .enc = 1, .cmp =  "zst", .rt = 2, .bnd = 1, .bi = 1, .repoDirect = true},
    {.pg =  "17", .repo = "pg2", .stg = "posix", .enc = 0, .cmp = "none", .rt = 2, .bnd = 1, .bi = 1, .repoDirect = true},
    {.pg =  "18", .repo = "pg2", .stg =    "s3", .enc = 1, .cmp =  "lz4", .rt = 1, .bnd = 1, .bi = 1, .repoDirect = true},
    {.pg =  "19", .repo = "pg2", .stg = "posix", .enc = 0, .cmp = "none", .rt = 2, .bnd = 1, .bi = 1, .repoDirect = true},
    // {uncrustify_on}
};

#define TEST_ROWS                                                   "select string_agg(id::text, ',' order by id) from status"
#define TEST_RESTORE                                                "--reset-recovery-option --delta"

/***********************************************************************************************************************************
Wait for an archived segment. Use the storage interface for both POSIX and S3.
***********************************************************************************************************************************/
static const Storage *
testRepo(const unsigned int repo)
{
    return repo == 1 ? hrnHostRepo1Storage(hrnHostRepo()) : hrnHostRepo2Storage(hrnHostRepo());
}

static const String *
testWalFile(const unsigned int repo, const char *const stanza, const String *const segment)
{
    const String *const path = strNewFmt(
        "archive/%s/%s-1/%s", stanza, strZ(pgVersionToStr(hrnHostPgVersion())), strZ(strSubN(segment, 0, 16)));
    Wait *const wait = waitNew(20000);
    StringList *files;

    do
    {
        files = storageListP(testRepo(repo), path, .expression = strNewFmt("^%s-", strZ(segment)));
    }
    while (strLstSize(files) == 0 && waitMore(wait));

    TEST_RESULT_UINT(strLstSize(files), 1, "one archived WAL segment");
    return strNewFmt("%s/%s", strZ(path), strZ(strLstGet(files, 0)));
}

static const String *
testWalSwitch(HrnHost *const pg, const unsigned int repo, const char *const stanza)
{
    const String *const segment = pckReadStrP(
        hrnHostSqlValue(
            pg, zNewFmt("select pg_%sfile_name(pg_switch_%s())::text", strZ(pgWalName(hrnHostPgVersion())),
                        strZ(pgWalName(hrnHostPgVersion())))));
    testWalFile(repo, stanza, segment);
    return segment;
}

static const String *
testHistory(HrnHost *const pg, const unsigned int repo, const char *const stanza, const unsigned int timeline)
{
    const String *const file = strNewFmt("%08X.history", timeline);
    TEST_STORAGE_EXISTS(
        testRepo(repo), zNewFmt("archive/%s/%s-1/%s", stanza, strZ(pgVersionToStr(hrnHostPgVersion())), strZ(file)),
        .timeout = 20000);
    const String *const destination = strNewFmt("%s/%s", strZ(hrnHostDataPath(pg)), strZ(file));
    TEST_HOST_BR(
        pg, CFGCMD_ARCHIVE_GET, .stanza = stanza, .option = zNewFmt("--repo=%u", repo),
        .param = zNewFmt("%s %s", strZ(file), strZ(destination)));
    const String *const history = strNewBuf(
        storageGetP(storageNewReadP(hrnHostPgStorage(pg), strNewFmt("%s/%s", strZ(pgWalPath(hrnHostPgVersion())), strZ(file)))));
    TEST_STORAGE_GET(hrnHostDataStorage(pg), strZ(destination), strZ(history));
    return history;
}

/**********************************************************************************************************************************/
static void
testSettings(HrnHost *const pg, const unsigned int tablespaceOid)
{
    TEST_HOST_SQL_ONE_STR_Z(pg, "show data_directory", strZ(hrnHostPgDataPath(pg)));
    TEST_HOST_SQL_ONE_STR_Z(pg, "show external_pid_file", "");
    TEST_HOST_SQL_ONE_STR_Z(pg, "show log_directory", zNewFmt("%s/log", strZ(hrnHostPgDataPath(pg))));
    TEST_HOST_SQL_ONE_STR_Z(
        pg, "select pg_tablespace_location(oid) from pg_tablespace where spcname = 'ts1'",
        zNewFmt("%s.tablespaces/%u", strZ(hrnHostPgDataPath(pg)), tablespaceOid));
    TEST_RESULT_BOOL(
        strstr(strZ(pckReadStrP(hrnHostSqlValue(pg, "show archive_command"))), "--stanza=fork") != NULL, true,
        "restored server archives to the fork stanza");
}

/**********************************************************************************************************************************/
static void
testStandbySettings(HrnHost *const pg)
{
    if (hrnHostPgVersion() >= PG_VERSION_12)
    {
        TEST_HOST_SQL_ONE_STR_Z(pg, "show primary_conninfo", "");
        TEST_HOST_SQL_ONE_STR_Z(pg, "show recovery_target_timeline", "latest");
    }
    else
    {
        const String *const recovery = strNewBuf(storageGetP(storageNewReadP(hrnHostPgStorage(pg), PG_FILE_RECOVERYCONF_STR)));
        TEST_RESULT_BOOL(strstr(strZ(recovery), "primary_conninfo") == NULL, true, "streaming is disabled");
        TEST_RESULT_BOOL(strstr(strZ(recovery), "recovery_target_timeline = 'latest'") != NULL, true, "follow new timelines");
    }
}

/***********************************************************************************************************************************
Test run
***********************************************************************************************************************************/
static void
testRun(void)
{
    FUNCTION_HARNESS_VOID();

    hrnErrorRetryDetailEnable();

    if (testBegin("fork primary and standby recovery"))
    {
        HRN_HOST_BUILD(testMatrix);
        HrnHost *const pg1 = hrnHostPg1();
        HrnHost *const pg2 = hrnHostPg2();
        const unsigned int repo = hrnHostRepoTotal();
        const char *const repoOption = zNewFmt("--repo=%u", repo);
        hrnHostConfigUpdateP(.stanza = STRDEF("fork"));

        TEST_TITLE("source backup chain and PITR target");
        HRN_HOST_PG_CREATE(pg1);
        TEST_HOST_BR(
            pg1, CFGCMD_STANZA_CREATE,
            .option = hrnHostPgVersion() == PG_VERSION_18 ? "--repo1-format=5" :
                          (repo == 2 ? "--repo1-format=6 --repo2-format=6" : "--repo1-format=6"));
        const String *const sourcePid = strNewFmt("%s/source.pid", strZ(hrnHostDataPath(pg1)));
        if (hrnHostPgVersion() >= PG_VERSION_12)
        {
            HRN_HOST_SQL_EXEC(pg1, "alter system set recovery_target_inclusive = off");
            HRN_HOST_SQL_EXEC(pg1, "alter system set recovery_target_name = 'source-target'");
        }
        HRN_HOST_PG_STOP(pg1);
        String *const sourceConfig = strCatBuf(
            strNew(), storageGetP(storageNewReadP(hrnHostPgStorage(pg1), STRDEF("postgresql.conf"))));
        strCatFmt(
            sourceConfig, "\ndata_directory = '%s'\nexternal_pid_file = '%s'\n", strZ(hrnHostPgDataPath(pg1)), strZ(sourcePid));
        HRN_STORAGE_PUT(hrnHostPgStorage(pg1), "postgresql.conf", BUFSTR(sourceConfig));
        HRN_HOST_PG_START(pg1, .noConfig = true);
        HRN_STORAGE_PATH_CREATE(hrnHostDataStorage(pg1), strZ(hrnHostPgTsPath(pg1)), .mode = 0700);
        HRN_HOST_SQL_EXEC(pg1, zNewFmt("create tablespace ts1 location '%s'", strZ(hrnHostPgTsPath(pg1))));
        const unsigned int tablespaceOid = pckReadU32P(hrnHostSqlValue(pg1, "select oid from pg_tablespace where spcname = 'ts1'"));
        HRN_HOST_SQL_EXEC(pg1, "create table status (id int primary key, payload text) tablespace ts1");
        HRN_HOST_SQL_EXEC(pg1, "insert into status values (0, repeat('base', 1000))");
        TEST_HOST_BR(pg1, CFGCMD_BACKUP, .option = "--repo=1 --type=full");
        HRN_HOST_SQL_EXEC(pg1, "insert into status values (1, repeat('incremental', 1000))");
        TEST_HOST_BR(pg1, CFGCMD_BACKUP, .option = repo == 1 ? "--repo=1 --type=incr" : "--repo=1 --type=diff");
        StringList *const inherited = storageListP(testRepo(1), STRDEF("backup/" HRN_STANZA), .expression = STRDEF("^[0-9]"));
        strLstSort(inherited, sortOrderAsc);
        TEST_RESULT_UINT(strLstSize(inherited), 2, "source backup chain");
        TEST_RESULT_BOOL(strEndsWithZ(strLstGet(inherited, 0), "F"), true, "source full backup");
        TEST_RESULT_BOOL(strEndsWithZ(strLstGet(inherited, 1), repo == 1 ? "I" : "D"), true, "source dependent backup");

        // Backup metadata stores whole seconds. Keep the target in a later second.
        HRN_HOST_SQL_EXEC(pg1, "perform pg_sleep(1.1)");
        HRN_HOST_SQL_EXEC(pg1, "insert into status values (2, 'fork target')");
        const String *const target = pckReadStrP(hrnHostSqlValue(pg1, "select clock_timestamp()::text"));
        const String *const futureTarget = pckReadStrP(hrnHostSqlValue(pg1, "select (clock_timestamp() + interval '1 day')::text"));
        const String *const boundary = pckReadStrP(
            hrnHostSqlValue(pg1, zNewFmt("select pg_%sfile_name(pg_current_%s_%s())::text",
                                         strZ(pgWalName(hrnHostPgVersion())), strZ(pgWalName(hrnHostPgVersion())),
                                         hrnHostPgVersion() < PG_VERSION_10 ? "location" : "lsn")));
        HRN_HOST_SQL_EXEC(pg1, "perform pg_sleep(0.1)");
        HRN_HOST_SQL_EXEC(pg1, "insert into status values (3, 'after fork target')");
        const String *promotionTarget = pckReadStrP(hrnHostSqlValue(pg1, "select clock_timestamp()::text"));
        HRN_HOST_SQL_EXEC(pg1, "perform pg_sleep(0.1)");
        HRN_HOST_SQL_EXEC(pg1, "insert into status values (4, 'discarded source continuation')");
        testWalSwitch(pg1, 1, HRN_STANZA);

        TEST_TITLE("source history with two promotions");
        HRN_HOST_PG_STOP(pg1);
        TEST_HOST_BR(
            pg1, CFGCMD_RESTORE,
            .option = zNewFmt(
                "--repo=1 --delta --type=time --target='%s' --target-action=promote"
                " --target-timeline=latest --reset-recovery-option",
                strZ(promotionTarget)));
        HRN_HOST_PG_START(pg1, .noConfig = true);
        TEST_HOST_SQL_ONE_STR_Z(pg1, "select pg_is_in_recovery()::text", "false");
        TEST_HOST_SQL_ONE_STR_Z(pg1, TEST_ROWS, "0,1,2,3");
        const String *const sourceHistory = testHistory(pg1, 1, HRN_STANZA, 2);
        const String *const firstSwitch = strLstGet(strLstNewSplitZ(sourceHistory, "\t"), 1);
        HRN_HOST_SQL_EXEC(pg1, "insert into status values (5, 'source timeline two')");
        testWalSwitch(pg1, 1, HRN_STANZA);
        HRN_HOST_SQL_EXEC(pg1, "perform pg_sleep(1.1)");
        TEST_HOST_BR(pg1, CFGCMD_BACKUP, .option = "--repo=1 --type=full");
        promotionTarget = pckReadStrP(hrnHostSqlValue(pg1, "select clock_timestamp()::text"));
        HRN_HOST_SQL_EXEC(pg1, "perform pg_sleep(0.1)");
        HRN_HOST_SQL_EXEC(pg1, "insert into status values (6, 'discarded second continuation')");
        testWalSwitch(pg1, 1, HRN_STANZA);
        HRN_HOST_PG_STOP(pg1);
        TEST_HOST_BR(
            pg1, CFGCMD_RESTORE,
            .option = zNewFmt(
                "--repo=1 --delta --type=time --target='%s' --target-action=promote"
                " --target-timeline=latest --reset-recovery-option",
                strZ(promotionTarget)));
        HRN_HOST_PG_START(pg1, .noConfig = true);
        TEST_HOST_SQL_ONE_STR_Z(pg1, "select pg_is_in_recovery()::text", "false");
        HRN_HOST_SQL_EXEC(pg1, "insert into status values (7, 'source timeline three')");
        testWalSwitch(pg1, 1, HRN_STANZA);
        testHistory(pg1, 1, HRN_STANZA, 3);

        const String *const original = testWalFile(1, HRN_STANZA, boundary);
        testWalFile(1, HRN_STANZA, strNewFmt("00000002%s", strZ(strSub(boundary, 8))));
        storageRemoveP(testRepo(1), original);
        TEST_HOST_BR(
            pg1, CFGCMD_ARCHIVE_GET, .option = "--repo=1",
            .param = zNewFmt("%s %s/missing-source", strZ(boundary), strZ(hrnHostDataPath(pg1))), .resultExpect = 1);

        TEST_TITLE("metadata-only fork selects the inherited chain");
        if (hrnHostPgVersion() == PG_VERSION_18)
            TEST_HOST_BR(pg1, CFGCMD_STANZA_UPGRADE, .option = "--repo1-format=6");
        const String *const sourcePidBefore = strNewBuf(storageGetP(storageNewReadP(hrnHostDataStorage(pg1), sourcePid)));
        String *const metadataConfig = strCatBuf(
            strNew(), storageGetP(storageNewReadP(hrnHostDataStorage(pg2), STRDEF("cfg/pgbackrest.conf"))));
        strTruncIdx(metadataConfig, (int)(strstr(strZ(metadataConfig), "\n[" HRN_STANZA "]") - strZ(metadataConfig)));
        strCatZ(metadataConfig, "\n[fork]\n");
        HRN_STORAGE_PUT(hrnHostDataStorage(pg2), "fork.conf", BUFSTR(metadataConfig));
        const char *const forkOption = zNewFmt(
            "%s --config=%s/fork.conf --source-stanza=" HRN_STANZA " --source-repo=1 --type=time --target='%s'",
            repoOption, strZ(hrnHostDataPath(pg2)), strZ(target));
        TEST_HOST_BR(pg2, CFGCMD_FORK, .stanza = "fork", .option = forkOption);
        TEST_RESULT_BOOL(storagePathExistsP(hrnHostPgStorage(pg2), NULL), false, "fork does not create PGDATA");
        for (unsigned int labelIdx = 0; labelIdx < strLstSize(inherited); labelIdx++)
        {
            TEST_STORAGE_LIST(
                testRepo(repo), zNewFmt("backup/fork/%s", strZ(strLstGet(inherited, labelIdx))),
                "backup.manifest\nbackup.manifest.copy\n");
        }
        TEST_RESULT_UINT(
            strLstSize(storageListP(testRepo(repo), STRDEF("backup/fork"), .expression = STRDEF("^[0-9]"))), 2,
            "newer source full backup is excluded");
        TEST_HOST_BR(
            pg2, CFGCMD_RESTORE, .stanza = "fork", .option = zNewFmt("%s " TEST_RESTORE " --type=standby", repoOption),
            .resultExpect = errorTypeCode(&OptionInvalidError));
        TEST_RESULT_BOOL(storagePathExistsP(hrnHostPgStorage(pg2), NULL), false, "standby needs the first fork history");
        TEST_HOST_BR(
            pg2, CFGCMD_RESTORE, .stanza = "fork",
            .option = zNewFmt("%s " TEST_RESTORE " --tablespace-map=ts1=%s", repoOption, strZ(hrnHostPgTsPath(pg1))),
            .resultExpect = errorTypeCode(&OptionInvalidError));
        TEST_HOST_SQL_ONE_STR_Z(pg1, TEST_ROWS, "0,1,2,3,5,7");
        TEST_HOST_BR(
            pg1, CFGCMD_EXPIRE, .option = zNewFmt("--repo=1 --set=%s", strZ(strLstGet(inherited, 0))),
            .resultExpect = errorTypeCode(&FileInvalidError));

        TEST_TITLE("first history archive retry preserves recovery state");
        const String *const deny = strNewFmt("%s/deny-archive", strZ(hrnHostDataPath(pg2)));
        const String *const seen = strNewFmt("%s/archive-denied", strZ(hrnHostDataPath(pg2)));
        const String *const wrapper = strNewFmt("%s/archive-wrapper", strZ(hrnHostDataPath(pg2)));
        storagePutP(
            storageNewWriteP(hrnHostDataStorage(pg2), wrapper, .modeFile = 0700),
            BUFSTR(strNewFmt(
                       "#!/bin/sh\nfor arg do\n"
                       "  if [ \"$arg\" = archive-push ] && [ -e %s ]; then\n    touch %s\n    exit 1\n  fi\ndone\n"
                       "exec pgbackrest \"$@\"\n", strZ(deny), strZ(seen))));
        HRN_STORAGE_PUT_EMPTY(hrnHostDataStorage(pg2), strZ(deny));
        TEST_HOST_BR(
            pg2, CFGCMD_RESTORE, .stanza = "fork",
            .option = zNewFmt("%s " TEST_RESTORE " --cmd=%s", repoOption, strZ(wrapper)));
        HRN_HOST_PG_START(pg2, .noConfig = true);
        TEST_HOST_SQL_ONE_STR_Z(pg2, "select pg_is_in_recovery()::text", "false");
        TEST_HOST_SQL_ONE_STR_Z(pg2, TEST_ROWS, "0,1,2");
        testSettings(pg2, tablespaceOid);
        TEST_STORAGE_GET(hrnHostDataStorage(pg1), strZ(sourcePid), strZ(sourcePidBefore));
        TEST_STORAGE_EXISTS(hrnHostDataStorage(pg2), strZ(seen), .timeout = 20000);
        const Buffer *const pending = storageGetP(storageNewReadP(testRepo(repo), STRDEF("backup/fork/fork.info")));
        HRN_STORAGE_PUT_Z(hrnHostPgStorage(pg2), "fork-sentinel", "keep PGDATA");
        TEST_HOST_BR(pg2, CFGCMD_FORK, .stanza = "fork", .option = forkOption);
        TEST_RESULT_BOOL(
            bufEq(pending, storageGetP(storageNewReadP(testRepo(repo), STRDEF("backup/fork/fork.info")))), true,
            "pending fork retry keeps metadata");
        TEST_STORAGE_GET(hrnHostPgStorage(pg2), "fork-sentinel", "keep PGDATA");
        storageRemoveP(hrnHostDataStorage(pg2), deny);
        const StringList *const ready = storageListP(
            hrnHostPgStorage(pg2), strNewFmt("%s/archive_status", strZ(pgWalPath(hrnHostPgVersion()))),
            .expression = STRDEF("[.]ready$"));
        for (unsigned int readyIdx = 0; readyIdx < strLstSize(ready); readyIdx++)
        {
            const String *const file = strLstGet(ready, readyIdx);
            TEST_HOST_BR(
                pg2, CFGCMD_ARCHIVE_PUSH, .stanza = "fork", .option = repoOption,
                .param = zNewFmt("%s/%s/%s", strZ(hrnHostPgDataPath(pg2)), strZ(pgWalPath(hrnHostPgVersion())),
                                 strZ(strSubN(file, 0, strSize(file) - 6))));
        }
        // Restart the archiver instead of waiting for its retry interval.
        HRN_HOST_PG_STOP(pg2);
        HRN_HOST_PG_START(pg2, .noConfig = true);
        const String *const forkHistory = testHistory(pg2, repo, "fork", 4);
        const StringList *const historyRows = strLstNewSplitZ(strTrim(strDup(forkHistory)), "\n");
        const StringList *const historyFields = strLstNewSplitZ(strLstGet(historyRows, strLstSize(historyRows) - 1), "\t");
        TEST_RESULT_BOOL(
            pgLsnFromStr(strLstGet(historyFields, 1)) < pgLsnFromStr(firstSwitch), true, "fork precedes source switch");
        HRN_HOST_SQL_EXEC(pg2, "insert into status values (8, 'fork continuation')");
        testWalSwitch(pg2, repo, "fork");
        TEST_HOST_SQL_ONE_STR_Z(pg1, TEST_ROWS, "0,1,2,3,5,7");

        TEST_TITLE("missing fork boundary waits and resumes from the archive");
        HRN_HOST_PG_STOP(pg1);
        HRN_HOST_PG_STOP(pg2);
        hrnHostPgDataPathSet(pg1, strNewFmt("%s/standby", strZ(hrnHostPgPath(pg1))));
        hrnHostConfigUpdateP();
        // archive-get has no log file. Send its recovery requests to the PostgreSQL log.
        String *const getConfig = strCatBuf(
            strNew(), storageGetP(storageNewReadP(hrnHostDataStorage(pg1), STRDEF("cfg/pgbackrest.conf"))));
        strCatZ(getConfig, "\n[fork:archive-get]\nlog-level-console=info\n");
        HRN_STORAGE_PUT(hrnHostDataStorage(pg1), "cfg/pgbackrest.conf", BUFSTR(getConfig));
        const String *const ownBoundary = testWalFile(repo, "fork", strNewFmt("00000004%s", strZ(strSub(boundary, 8))));
        const Buffer *const held = storageGetP(storageNewReadP(testRepo(repo), ownBoundary));
        storageRemoveP(testRepo(repo), ownBoundary);
        TEST_HOST_BR(
            pg1, CFGCMD_RESTORE, .stanza = "fork", .option = zNewFmt("%s " TEST_RESTORE " --type=standby", repoOption));
        hrnHostExecP(
            pg1, strNewFmt("%s/pg_ctl start -D '%s' -l '%s' -s -W -o '-k %s'", strZ(hrnHostPgBinPath(pg1)),
                           strZ(hrnHostPgDataPath(pg1)), strZ(hrnHostPgLogFile(pg1)), strZ(hrnHostPgPath(pg1))));
        TEST_STORAGE_EXISTS(hrnHostPgStorage(pg1), PG_FILE_POSTMTRPID, .timeout = 10000);
        const String *const pid = strLstGet(
            strLstNewSplitZ(strNewBuf(storageGetP(storageNewReadP(hrnHostPgStorage(pg1), PG_FILE_POSTMTRPID_STR))), "\n"), 0);
        TEST_HOST_BR(
            pg1, CFGCMD_ARCHIVE_GET, .stanza = "fork", .option = repoOption,
            .param = zNewFmt("%s %s/forbidden-source", strZ(boundary), strZ(hrnHostDataPath(pg1))), .resultExpect = 1);
        TEST_RESULT_BOOL(
            storageExistsP(hrnHostDataStorage(pg1), STRDEF("forbidden-source")), false, "divergent source WAL is not returned");
        Wait *const boundaryWait = waitNew(20000);
        bool boundaryRequested = false;
        do
        {
            const Buffer *const log = storageGetP(
                storageNewReadP(
                    hrnHostDataStorage(pg1), hrnHostPgLogFile(pg1), .ignoreMissing = true));
            boundaryRequested = log != NULL && strstr(
                strZ(strNewBuf(log)), zNewFmt("unable to find 00000004%s in the archive", strZ(strSub(boundary, 8)))) != NULL;
        }
        while (!boundaryRequested && waitMore(boundaryWait));
        TEST_RESULT_BOOL(boundaryRequested, true, "standby requests the missing fork segment");
        storagePutP(storageNewWriteP(testRepo(repo), ownBoundary), held);
        TEST_HOST_SQL_ONE_STR_Z(pg1, TEST_ROWS, "0,1,2,8");
        TEST_HOST_SQL_ONE_STR_Z(pg1, "select pg_is_in_recovery()::text", "true");
        testStandbySettings(pg1);
        testSettings(pg1, tablespaceOid);
        TEST_RESULT_STR(
            strLstGet(
                strLstNewSplitZ(strNewBuf(storageGetP(storageNewReadP(hrnHostPgStorage(pg1), PG_FILE_POSTMTRPID_STR))), "\n"), 0),
            pid, "standby resumes without restarting");
        HRN_HOST_PG_STOP(pg1);

        TEST_TITLE("restore the primary before taking a fork backup");
        const Buffer *const finalized = storageGetP(storageNewReadP(testRepo(repo), STRDEF("backup/fork/fork.info")));
        TEST_HOST_BR(pg2, CFGCMD_FORK, .stanza = "fork", .option = forkOption);
        TEST_RESULT_BOOL(
            bufEq(finalized, storageGetP(storageNewReadP(testRepo(repo), STRDEF("backup/fork/fork.info")))), true,
            "stopped fork retry keeps metadata");
        TEST_STORAGE_GET(hrnHostPgStorage(pg2), "fork-sentinel", "keep PGDATA");
        TEST_RESULT_BOOL(storageExistsP(hrnHostPgStorage(pg2), PG_FILE_POSTMTRPID_STR), false, "fork does not start PostgreSQL");
        hrnHostPgDataPathSet(pg2, strNewFmt("%s/restored", strZ(hrnHostPgPath(pg2))));
        hrnHostConfigUpdateP();
        TEST_HOST_BR(pg2, CFGCMD_RESTORE, .stanza = "fork", .option = zNewFmt("%s " TEST_RESTORE, repoOption));
        HRN_HOST_PG_START(pg2, .noConfig = true);
        TEST_HOST_SQL_ONE_STR_Z(pg2, "select pg_is_in_recovery()::text", "false");
        TEST_HOST_SQL_ONE_STR_Z(pg2, TEST_ROWS, "0,1,2,8");
        testHistory(pg2, repo, "fork", 5);
        HRN_HOST_SQL_EXEC(pg2, "insert into status values (9, 'restored primary')");
        testWalSwitch(pg2, repo, "fork");
        TEST_RESULT_UINT(
            strLstSize(storageListP(testRepo(repo), STRDEF("backup/fork"), .expression = STRDEF("^[0-9]"))), 2,
            "primary recovery used only inherited backups");

        TEST_TITLE("standby promotion extends the fork history");
        TEST_HOST_BR(
            pg1, CFGCMD_RESTORE, .stanza = "fork", .option = zNewFmt("%s " TEST_RESTORE " --type=standby", repoOption));
        HRN_HOST_PG_START(pg1, .noConfig = true);
        TEST_HOST_SQL_ONE_STR_Z(pg1, TEST_ROWS, "0,1,2,8,9");
        testStandbySettings(pg1);
        HRN_HOST_PG_STOP(pg2);
        HRN_HOST_PG_PROMOTE(pg1);
        TEST_HOST_SQL_ONE_STR_Z(pg1, "select pg_is_in_recovery()::text", "false");
        HRN_HOST_SQL_EXEC(pg1, "insert into status values (10, 'promoted standby')");
        testWalSwitch(pg1, repo, "fork");
        const String *const promotedHistory = testHistory(pg1, repo, "fork", 6);
        TEST_RESULT_BOOL(strBeginsWith(promotedHistory, forkHistory), true, "promotion retains the original fork history");
        TEST_HOST_BR(
            pg2, CFGCMD_RESTORE, .stanza = "fork", .option = zNewFmt("%s " TEST_RESTORE " --type=standby", repoOption));
        HRN_HOST_PG_START(pg2, .noConfig = true);
        TEST_HOST_SQL_ONE_STR_Z(pg2, TEST_ROWS, "0,1,2,8,9,10");
        TEST_HOST_SQL_ONE_STR_Z(pg2, "select pg_is_in_recovery()::text", "true");
        testStandbySettings(pg2);

        HRN_HOST_SQL_EXEC(pg1, "insert into status values (11, 'standby follows new WAL')");
        testWalSwitch(pg1, repo, "fork");
        TEST_HOST_SQL_ONE_STR_Z(pg2, TEST_ROWS, "0,1,2,8,9,10,11");

        TEST_TITLE("incremental backup retains inherited file references");
        TEST_HOST_BR(pg1, CFGCMD_BACKUP, .stanza = "fork", .option = zNewFmt("%s --type=incr", repoOption));
        TEST_HOST_BR(pg1, CFGCMD_VERIFY, .stanza = "fork", .option = repoOption);
        StringList *const forkBackups = storageListP(testRepo(repo), STRDEF("backup/fork"), .expression = STRDEF("^[0-9]"));
        strLstSort(forkBackups, sortOrderAsc);
        TEST_RESULT_UINT(strLstSize(forkBackups), 3, "fork has its first own backup");
        const String *const forkBackup = strLstGet(forkBackups, 2);
        TEST_RESULT_BOOL(strEndsWithZ(forkBackup, "I"), true, "fork backup is incremental");
        HRN_HOST_PG_STOP(pg2);
        TEST_HOST_BR(
            pg2, CFGCMD_RESTORE, .stanza = "fork",
            .option = zNewFmt("%s " TEST_RESTORE " --type=standby --set=%s", repoOption, strZ(forkBackup)));
        HRN_HOST_PG_START(pg2, .noConfig = true);
        TEST_HOST_SQL_ONE_STR_Z(pg2, TEST_ROWS, "0,1,2,8,9,10,11");
        testSettings(pg2, tablespaceOid);
        HRN_HOST_PG_STOP(pg2);
        HRN_HOST_PG_STOP(pg1);

        if (hrnHostPgVersion() >= PG_VERSION_10)
        {
            TEST_TITLE("fork exactly at a source timeline switch");
            hrnHostPgDataPathSet(pg2, strNewFmt("%s/exact", strZ(hrnHostPgPath(pg2))));
            hrnHostConfigUpdateP(.stanza = STRDEF("exact"));
            TEST_HOST_BR(
                pg2, CFGCMD_FORK, .stanza = "exact",
                .option = zNewFmt("%s --source-stanza=" HRN_STANZA " --source-repo=1 --type=lsn --target=%s --target-exclusive",
                                  repoOption, strZ(firstSwitch)));
            TEST_HOST_BR(pg2, CFGCMD_RESTORE, .stanza = "exact", .option = zNewFmt("%s " TEST_RESTORE, repoOption));
            HRN_HOST_PG_START(pg2, .noConfig = true);
            TEST_HOST_SQL_ONE_STR_Z(pg2, "select pg_is_in_recovery()::text", "false");
            TEST_HOST_SQL_ONE_STR_Z(pg2, TEST_ROWS, "0,1,2,3");
            const String *const exactHistory = testHistory(pg2, repo, "exact", 4);
            const StringList *const exactRows = strLstNewSplitZ(strTrim(strDup(exactHistory)), "\n");
            TEST_RESULT_STR(
                strLstGet(strLstNewSplitZ(strLstGet(exactRows, strLstSize(exactRows) - 1), "\t"), 1),
                firstSwitch, "exact branch LSN");
            HRN_HOST_PG_STOP(pg2);
        }

        TEST_TITLE("target beyond the available archive");
        hrnHostPgDataPathSet(pg2, strNewFmt("%s/future", strZ(hrnHostPgPath(pg2))));
        hrnHostConfigUpdateP(.stanza = STRDEF("future"));
        TEST_HOST_BR(
            pg2, CFGCMD_FORK, .stanza = "future",
            .option = zNewFmt("%s --source-stanza=" HRN_STANZA " --source-repo=1 --type=time --target='%s'",
                              repoOption, strZ(futureTarget)));
        TEST_HOST_BR(pg2, CFGCMD_RESTORE, .stanza = "future", .option = zNewFmt("%s " TEST_RESTORE, repoOption));
        hrnHostExecP(
            pg2, strNewFmt("%s/pg_ctl start -D '%s' -l '%s' -s -W -o '-k %s -c hot_standby=off'", strZ(hrnHostPgBinPath(pg2)),
                           strZ(hrnHostPgDataPath(pg2)), strZ(hrnHostPgLogFile(pg2)), strZ(hrnHostPgPath(pg2))));
        if (hrnHostPgVersion() >= PG_VERSION_13)
        {
            Wait *const recoveryWait = waitNew(20000);
            bool targetRejected = false;
            do
            {
                targetRejected = strstr(
                    strZ(strNewBuf(storageGetP(storageNewReadP(hrnHostDataStorage(pg2), hrnHostPgLogFile(pg2))))),
                    "recovery ended before configured recovery target was reached") != NULL;
            }
            while ((!targetRejected || storageExistsP(hrnHostPgStorage(pg2), PG_FILE_POSTMTRPID_STR)) && waitMore(recoveryWait));
            TEST_RESULT_BOOL(targetRejected, true, "PostgreSQL rejects an unreachable target");
            TEST_RESULT_BOOL(
                storageExistsP(hrnHostPgStorage(pg2), PG_FILE_POSTMTRPID_STR), false, "failed recovery stops PostgreSQL");
            TEST_RESULT_BOOL(
                storageExistsP(hrnHostPgStorage(pg2), strNewFmt("%s/00000004.history", strZ(pgWalPath(hrnHostPgVersion())))), false,
                "failed recovery creates no history");
            TEST_HOST_BR(
                pg2, CFGCMD_RESTORE, .stanza = "future",
                .option = zNewFmt(
                    "%s " TEST_RESTORE " --type=standby --pg1-path=%s/rejected", repoOption, strZ(hrnHostPgPath(pg2))),
                .resultExpect = errorTypeCode(&OptionInvalidError));
            TEST_RESULT_BOOL(
                storagePathExistsP(hrnHostDataStorage(pg2), strNewFmt("%s/rejected", strZ(hrnHostPgPath(pg2)))), false,
                "failed recovery cannot initialize a standby");
        }
        else
        {
            TEST_STORAGE_EXISTS(
                hrnHostPgStorage(pg2), zNewFmt("%s/00000004.history", strZ(pgWalPath(hrnHostPgVersion()))), .timeout = 20000);
            TEST_HOST_SQL_ONE_STR_Z(pg2, "select pg_is_in_recovery()::text", "false");
            testHistory(pg2, repo, "future", 4);
            HRN_HOST_PG_STOP(pg2);
        }
    }

    FUNCTION_HARNESS_RETURN_VOID();
}
