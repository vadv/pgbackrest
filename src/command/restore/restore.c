/***********************************************************************************************************************************
Restore Command
***********************************************************************************************************************************/
#include <build.h>

#include <unistd.h>

#include "command/fork/common.h"
#include "command/lock.h"
#include "command/restore/protocol.h"
#include "command/restore/restore.h"
#include "command/restore/timeline.h"
#include "common/regExp.h"
#include "common/user.h"
#include "config/config.h"
#include "config/exec.h"
#include "info/infoArchive.h"
#include "info/infoBackup.h"
#include "info/manifest/manifest.h"
#include "postgres/interface.h"
#include "postgres/version.h"
#include "protocol/helper.h"
#include "protocol/parallel.h"
#include "storage/helper.h"

#define RESTORE_FORK_FILE                                           BACKUP_MANIFEST_FILE ".fork"

#include "clean.c.inc"
#include "config.c.inc"
#include "process.c.inc"
#include "remap.c.inc"
#include "select.c.inc"
#include "validate.c.inc"

/***********************************************************************************************************************************
Omit archive-copy WAL and timeline history. Let archive-get select files from the fork's timeline history.
***********************************************************************************************************************************/
static void
restoreForkManifestClean(Manifest *const manifest)
{
    RegExp *const expression = regExpNew(
        STRDEF("^pg_data/(pg_wal|pg_xlog)/([0-F]{24}(\\.partial){0,1}|[0-F]{8}\\.history)$"));

    for (unsigned int idx = manifestFileTotal(manifest); idx > 0; idx--)
    {
        const String *const name = manifestFileNameGet(manifest, idx - 1);

        if (regExpMatch(expression, name))
            manifestFileRemove(manifest, name);
    }

    regExpFree(expression);
}

/***********************************************************************************************************************************
Resolve symbolic links before ".." components to detect source path overlap, for example, "alias/../target".
Keep missing components to validate new destinations.
***********************************************************************************************************************************/
static String *
restoreForkPath(const String *const path)
{
    ASSERT(strBeginsWith(path, FSLASH_STR));
    StringList *parts = strLstNewSplit(path, FSLASH_STR);
    StringList *resolved = strLstNew();
    unsigned int linkTotal = 0;

    for (unsigned int idx = 0; idx < strLstSize(parts); idx++)
    {
        const String *const part = strLstGet(parts, idx);

        if (strEmpty(part) || strEq(part, DOT_STR))
            continue;

        if (strEq(part, DOTDOT_STR))
        {
            if (!strLstEmpty(resolved))
                strLstRemoveIdx(resolved, strLstSize(resolved) - 1);

            continue;
        }

        strLstAdd(resolved, part);
        const String *const current = strNewFmt("/%s", strZ(strLstJoin(resolved, "/")));
        const StorageInfo info = storageInfoP(storageLocal(), current, .ignoreMissing = true);

        if (info.exists && info.type == storageTypeLink)
        {
            CHECK_FMT(LinkMapError, ++linkTotal <= 40, "too many symbolic links in fork restore path '%s'", strZ(path));
            String *const remaining = strNew();

            if (strBeginsWith(info.linkDestination, FSLASH_STR))
                strCat(remaining, info.linkDestination);
            else
                strCatFmt(remaining, "%s/%s", strZ(strPath(current)), strZ(info.linkDestination));

            for (unsigned int rest = idx + 1; rest < strLstSize(parts); rest++)
                strCatFmt(remaining, "/%s", strZ(strLstGet(parts, rest)));

            parts = strLstNewSplit(remaining, FSLASH_STR);
            resolved = strLstNew();
            idx = 0;
        }
    }

    return strNewFmt("/%s", strZ(strLstJoin(resolved, "/")));
}

static String *
restoreForkTargetPath(const Manifest *const manifest, const ManifestTarget *const target)
{
    const String *const path =
        strBeginsWith(target->path, FSLASH_STR) ? target->path :
            strNewFmt(
                "%s/%s/%s", strZ(manifestTargetBase(manifest)->path),
                strZ(strPath(manifestPathPg(target->name))), strZ(target->path));
    return restoreForkPath(target->file == NULL ? path : strNewFmt("%s/%s", strZ(path), strZ(target->file)));
}

// Identify this fork's interrupted restore when another host uses the source PGDATA path.
static String *
restoreForkIdentity(const Manifest *const manifest, const InfoFork *const fork, const unsigned int repoIdx)
{
    return strNewFmt(
        "%s\n%s\n%s\n%s\n", strZ(infoForkData(fork)->sourceId),
        strZ(forkPinName(repoIdx, cfgOptionStr(cfgOptStanza))),
        strZ(manifestData(manifest)->backupLabel), strZ(restoreForkPath(cfgOptionStr(cfgOptPgPath))));
}

/***********************************************************************************************************************************
Keep source paths for validation. Map tablespaces by OID unless the user supplies a tablespace map.
***********************************************************************************************************************************/
static StringList *
restoreForkMap(Manifest *const manifest, const InfoFork *const fork, const unsigned int repoIdx)
{
    StringList *const sourcePaths = strLstNew();
    const KeyValue *const explicitMap = cfgOptionKvNull(cfgOptTablespaceMap);
    const bool imported = strLstExists(infoForkData(fork)->importedBackupList, manifestData(manifest)->backupLabel);
    const String *const destinationPgPath = restoreForkPath(cfgOptionStr(cfgOptPgPath));
    const bool inPlace = !imported &&
                         strEq(destinationPgPath, restoreForkTargetPath(manifest, manifestTargetBase(manifest)));
    Manifest *const source =
        imported ? manifest :
            forkSourceManifestLoad(forkSourceRepoIdx(fork), infoForkData(fork)->sourceStanza, infoForkData(fork)->backupLabel);
    const String *const sourcePgPath = restoreForkTargetPath(source, manifestTargetBase(source));

    const Buffer *const restoreIdentity = storageGetP(
        storageNewReadP(storagePg(), STRDEF(RESTORE_FORK_FILE), .ignoreMissing = true));

    // At the source PGDATA path, allow an empty directory, an in-place fork restore, or a matching restore identity.
    const bool sourceDataOwned =
        strEq(destinationPgPath, sourcePgPath) &&
        (inPlace || strLstEmpty(storageListP(storageLocal(), cfgOptionStr(cfgOptPgPath))) ||
         (restoreIdentity != NULL && bufEq(restoreIdentity, BUFSTR(restoreForkIdentity(manifest, fork, repoIdx)))));

    for (unsigned int idx = 0; idx < manifestTargetTotal(source); idx++)
    {
        const String *const path = restoreForkTargetPath(source, manifestTarget(source, idx));

        if (!sourceDataOwned || (!strEq(path, sourcePgPath) && !strBeginsWith(path, strNewFmt("%s/", strZ(sourcePgPath)))))
            strLstAdd(sourcePaths, path);
    }

    if (source != manifest)
        manifestFree(source);

    for (unsigned int idx = 0; idx < manifestTargetTotal(manifest); idx++)
    {
        const ManifestTarget *const target = manifestTarget(manifest, idx);

        if (!imported && !inPlace)
            strLstAddIfMissing(sourcePaths, restoreForkTargetPath(manifest, target));

        if (!inPlace && target->tablespaceId != 0 && !cfgOptionTest(cfgOptTablespaceMapAll))
        {
            const String *const id = strNewFmt("%u", target->tablespaceId);

            if (explicitMap == NULL ||
                (kvGet(explicitMap, VARSTR(id)) == NULL && kvGet(explicitMap, VARSTR(target->tablespaceName)) == NULL))
            {
                const String *const path = strNewFmt("%s.tablespaces/%s", strZ(cfgOptionStr(cfgOptPgPath)), strZ(id));
                LOG_INFO_FMT("map tablespace '%s' to '%s'", strZ(target->name), strZ(path));
                manifestTargetUpdate(manifest, target->name, path, NULL);
                manifestLinkUpdate(manifest, strNewFmt(MANIFEST_TARGET_PGDATA "/%s", strZ(target->name)), path);
            }
        }
    }

    return sourcePaths;
}

static void
restoreForkMapValidate(const Manifest *const manifest, const StringList *const sourcePaths)
{
    for (unsigned int idx = 0; idx < manifestTargetTotal(manifest); idx++)
    {
        const String *const destination = restoreForkTargetPath(manifest, manifestTarget(manifest, idx));

        for (unsigned int sourceIdx = 0; sourceIdx < strLstSize(sourcePaths); sourceIdx++)
        {
            const String *const source = strLstGet(sourcePaths, sourceIdx);

            CHECK_FMT(
                OptionInvalidError,
                !strEq(destination, source) && !strEq(source, FSLASH_STR) && !strEq(destination, FSLASH_STR) &&
                !strBeginsWith(destination, strNewFmt("%s/", strZ(source))) &&
                !strBeginsWith(source, strNewFmt("%s/", strZ(destination))),
                "fork restore destination '%s' overlaps source path '%s'", strZ(destination), strZ(source));
        }
    }
}

/**********************************************************************************************************************************/
FN_EXTERN void
cmdRestore(void)
{
    FUNCTION_LOG_VOID(logLevelDebug);

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // Get information for the current user
        userInit();

        // PostgreSQL must be local
        pgIsLocalVerify();

        // Validate restore path
        restorePathValidate();

        // Get the backup set
        const RestoreBackupData backupData = restoreBackupSet();
        const InfoFork *const fork = forkInfoLoad(backupData.repoIdx);

        if (fork != NULL)
        {
            CHECK(
                OptionInvalidError, cfgOptionSeq(cfgOptType) != CFGOPTVAL_RESTORE_TYPE_PRESERVE,
                "fork restore cannot preserve recovery settings");
            cfgOptionSet(cfgOptRepo, cfgSourceParam, VARINT64(cfgOptionGroupIdxToKey(cfgOptGrpRepo, backupData.repoIdx)));
            restoreRecoveryOption(infoForkData(fork)->pgVersion, true);
        }

        // Use the fork's latest timeline by default. Replace inherited settings and PostgreSQL defaults.
        if (fork != NULL && !cfgOptionTest(cfgOptTargetTimeline) &&
            (cfgOptionSeq(cfgOptType) == CFGOPTVAL_RESTORE_TYPE_DEFAULT ||
             cfgOptionSeq(cfgOptType) == CFGOPTVAL_RESTORE_TYPE_STANDBY))
            cfgOptionSet(cfgOptTargetTimeline, cfgSourceParam, VARSTRDEF("latest"));

        // Remove stanza archive spool path so existing files do not interfere with the new cluster. For instance, old archive-push
        // acknowledgements could cause a new cluster to skip archiving. This should not happen if a new timeline is selected but
        // better to be safe. Missing stanza spool paths are ignored.
        storagePathRemoveP(storageSpoolWrite(), STORAGE_SPOOL_ARCHIVE_STR, .recurse = true);

        // Load manifest
        RestoreJobData jobData = {.repoIdx = backupData.repoIdx};

        jobData.manifest = manifestLoadFile(
            storageRepoIdx(backupData.repoIdx),
            strNewFmt(STORAGE_REPO_BACKUP "/%s/" BACKUP_MANIFEST_FILE, strZ(backupData.backupSet)), backupData.cipherSpecManifest);

        if (fork != NULL)
            restoreForkManifestClean(jobData.manifest);

        // Verify that the selected timeline is valid for the backup -- including current and latest timelines
        if (manifestData(jobData.manifest)->backupOptionOnline)
        {
            const ManifestData *const data = manifestData(jobData.manifest);
            const InfoArchive *const archiveInfo = infoArchiveLoadFile(
                storageRepoIdx(backupData.repoIdx), INFO_ARCHIVE_PATH_FILE_STR, cfgCipherSpecMainIdx(backupData.repoIdx));

            timelineVerify(
                storageRepoIdx(backupData.repoIdx),
                strNewFmt("%s-%u", strZ(pgVersionToStr(data->pgVersion)), data->pgId), data->pgVersion,
                cvtZToUIntBase(strZ(strSubN(data->archiveStart, 0, 8)), 16), pgLsnFromStr(data->lsnStart),
                cfgOptionStrNull(cfgOptTargetTimeline), cfgOptionSeq(cfgOptType),
                infoArchiveCipherSpecMap(archiveInfo));
        }

        // Remotes (if any) are no longer needed since the rest of the repository reads will be done by the local processes
        protocolFree();

        // If the backup was made offline and no restore type was specified then set to none. This prevents recovery.signal from
        // being generated by default for PostgreSQL >= 12. Offline backups created with wal_level=minimal will error in this case
        // so this should be a good default. However, if the user explicitly sets type and it does not equal none then they will get
        // an error if wal_level=minimal.
        if (!manifestData(jobData.manifest)->backupOptionOnline && cfgOptionSource(cfgOptType) == cfgSourceDefault)
            cfgOptionSet(cfgOptType, cfgSourceParam, VARUINT64(CFGOPTVAL_RESTORE_TYPE_NONE_STRID));

        // Validate manifest. Don't use strict mode because we'd rather ignore problems that won't affect a restore.
        manifestValidate(jobData.manifest, false);

        // Get the cipher spec used to decrypt files in the backup
        jobData.cipherSpecBackup = manifestCipherSpec(jobData.manifest);

        // Validate the manifest
        restoreManifestValidate(jobData.manifest, backupData.backupSet);

        // Log the backup set to restore. If the backup was online then append the time recovery will start from.
        String *const message = strCatFmt(
            strNew(), "%s: restore backup set %s", cfgOptionGroupName(cfgOptGrpRepo, backupData.repoIdx),
            strZ(backupData.backupSet));

        if (manifestData(jobData.manifest)->backupOptionOnline)
        {
            strCatZ(message, ", recovery will start at ");
            strCatTimeP(message, "%Y-%m-%d %H:%M:%S", manifestData(jobData.manifest)->backupTimestampStart);
        }

        LOG_INFO(strZ(message));

        // Map manifest
        const StringList *const sourcePaths = fork == NULL ? NULL : restoreForkMap(jobData.manifest, fork, backupData.repoIdx);
        restoreManifestMap(jobData.manifest);

        if (sourcePaths != NULL)
            restoreForkMapValidate(jobData.manifest, sourcePaths);

        // Check that links are sane
        manifestLinkCheck(jobData.manifest);

        // Update ownership
        restoreManifestOwner(jobData.manifest, &jobData.rootReplaceUser, &jobData.rootReplaceGroup);

        // Generate the selective restore expression
        const String *const expression = restoreSelectiveExpression(jobData.manifest);
        jobData.zeroExp = expression == NULL ? NULL : regExpNew(expression);

        if (fork != NULL)
            storagePutP(
                storageNewWriteP(storagePgWrite(), STRDEF(RESTORE_FORK_FILE)),
                BUFSTR(restoreForkIdentity(jobData.manifest, fork, backupData.repoIdx)));

        // Clean the data directory and build path/link structure
        restoreCleanBuild(jobData.manifest, jobData.rootReplaceUser, jobData.rootReplaceGroup);

        // Generate processing queues
        const uint64_t sizeTotal = restoreProcessQueue(jobData.manifest, &jobData.queueList);

        // Save manifest to the data directory so we can restart a delta restore even if the PG_VERSION file is missing
        manifestSave(jobData.manifest, storageWriteIo(storageNewWriteP(storagePgWrite(), BACKUP_MANIFEST_FILE_STR)));

        // Create the parallel executor
        ProtocolParallel *const parallelExec = protocolParallelNew(
            cfgOptionUInt64(cfgOptProtocolTimeout) / 2, restoreJobCallback, &jobData);

        for (unsigned int processIdx = 1; processIdx <= cfgOptionUInt(cfgOptProcessMax); processIdx++)
            protocolParallelClientAdd(parallelExec, protocolLocalGet(protocolStorageTypeRepo, 0, processIdx));

        // Process jobs
        uint64_t sizeRestored = 0;

        // Initialize percent complete and bytes completed/total
        unsigned int currentPercentComplete = 0;
        cmdLockWriteP(
            .percentComplete = VARUINT(currentPercentComplete), .sizeComplete = VARUINT64(sizeRestored),
            .size = VARUINT64(sizeTotal));

        unsigned int checksumErrorFileTotal = 0;

        MEM_CONTEXT_TEMP_RESET_BEGIN()
        {
            do
            {
                const unsigned int completed = protocolParallelProcess(parallelExec);

                for (unsigned int jobIdx = 0; jobIdx < completed; jobIdx++)
                {
                    sizeRestored = restoreJobResult(
                        jobData.manifest, protocolParallelResult(parallelExec), jobData.zeroExp, sizeTotal, sizeRestored,
                        &checksumErrorFileTotal, &currentPercentComplete);
                }

                // Reset the memory context occasionally so we don't use too much memory or slow down processing
                MEM_CONTEXT_TEMP_RESET(1000);
            }
            while (!protocolParallelDone(parallelExec));
        }
        MEM_CONTEXT_TEMP_END();

        // Write recovery settings. Use the data directory to set permissions and ownership for recovery files.
        StorageInfo fileInfo = storageInfoP(storagePg(), NULL);
        fileInfo.user = restoreManifestOwnerReplace(fileInfo.user, jobData.rootReplaceUser);
        fileInfo.group = restoreManifestOwnerReplace(fileInfo.group, jobData.rootReplaceGroup);
        fileInfo.mode = fileInfo.mode & (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

        restoreRecoveryWrite(jobData.manifest, &fileInfo, fork != NULL);

        // Remove backup.manifest
        storageRemoveP(storagePgWrite(), BACKUP_MANIFEST_FILE_STR);

        // Sync file link paths. These need to be synced separately because they are not linked from the data directory.
        StringList *const pathSynced = strLstNew();

        for (unsigned int targetIdx = 0; targetIdx < manifestTargetTotal(jobData.manifest); targetIdx++)
        {
            const ManifestTarget *const target = manifestTarget(jobData.manifest, targetIdx);

            if (target->type == manifestTargetTypeLink && target->file != NULL)
            {
                const String *const pgPath = manifestTargetPath(jobData.manifest, target);

                // Don't sync the same path twice. There can be multiple links to files in the same path, but syncing it more than
                // once makes the logs noisy and looks like a bug even though it doesn't hurt anything or realistically affect
                // performance.
                if (strLstExists(pathSynced, pgPath))
                    continue;
                else
                    strLstAdd(pathSynced, pgPath);

                // Sync the path
                LOG_DETAIL_FMT("sync path '%s'", strZ(pgPath));
                storagePathSyncP(storageLocalWrite(), pgPath);
            }
        }

        // Sync paths in the data directory
        for (unsigned int pathIdx = 0; pathIdx < manifestPathTotal(jobData.manifest); pathIdx++)
        {
            const String *const manifestName = manifestPath(jobData.manifest, pathIdx)->name;

            // Skip the pg_tblspc path because it only maps to the manifest. We should remove this in a future release but not much
            // can be done about it for now.
            if (strEqZ(manifestName, MANIFEST_TARGET_PGTBLSPC))
                continue;

            // We'll sync global after pg_control is written
            if (strEq(manifestName, STRDEF(MANIFEST_TARGET_PGDATA "/" PG_PATH_GLOBAL)))
                continue;

            const String *const pgPath = storagePathP(storagePg(), manifestPathPg(manifestName));

            LOG_DETAIL_FMT("sync path '%s'", strZ(pgPath));
            storagePathSyncP(storagePgWrite(), pgPath);
        }

        // Rename pg_control to remove the temp extension. This is done last to prevent a partially restored (or unsynced) cluster
        // from being started.
        if (storageExistsP(storagePg(), STRDEF(PG_PATH_GLOBAL "/" PG_FILE_PGCONTROL "." STORAGE_FILE_TEMP_EXT)))
        {
            // Invalidate the checkpoint in pg_control so the cluster cannot be started without backup_label
            if (manifestData(jobData.manifest)->backupOptionOnline)
            {
                Buffer *const pgControlBuffer = storageGetP(
                    storageNewReadP(storagePg(), STRDEF(PG_PATH_GLOBAL "/" PG_FILE_PGCONTROL "." STORAGE_FILE_TEMP_EXT)));
                const ManifestFile pgControlFile = manifestFileFind(
                    jobData.manifest, STRDEF(MANIFEST_TARGET_PGDATA "/" PG_PATH_GLOBAL "/" PG_FILE_PGCONTROL));

                pgControlCheckpointInvalidate(pgControlBuffer, cfgOptionStrNull(cfgOptPgVersionForce));
                storagePutP(
                    storageNewWriteP(
                        storagePgWrite(), STRDEF(PG_PATH_GLOBAL "/" PG_FILE_PGCONTROL "." STORAGE_FILE_TEMP_EXT),
                        .timeModified = pgControlFile.timestamp, .noAtomic = true, .noCreatePath = true, .noSyncPath = true),
                    pgControlBuffer);
            }

            LOG_INFO(
                "restore " PG_PATH_GLOBAL "/" PG_FILE_PGCONTROL " (performed last to ensure aborted restores cannot be started)");

            storageMoveP(
                storagePgWrite(),
                storageNewReadP(storagePg(), STRDEF(PG_PATH_GLOBAL "/" PG_FILE_PGCONTROL "." STORAGE_FILE_TEMP_EXT)),
                storageNewWriteP(storagePgWrite(), STRDEF(PG_PATH_GLOBAL "/" PG_FILE_PGCONTROL), .noSyncPath = true));
        }
        else
            LOG_WARN("backup does not contain '" PG_PATH_GLOBAL "/" PG_FILE_PGCONTROL "' -- cluster will not start");

        // Sync global path to persist pg_control rename. If this fails or if the system crashes before it can complete, then
        // pg_control should retain the temp extension, which will prevent the cluster from being started.
        LOG_DETAIL_FMT("sync path '%s'", strZ(storagePathP(storagePg(), PG_PATH_GLOBAL_STR)));
        storagePathSyncP(storagePgWrite(), PG_PATH_GLOBAL_STR);

        // Restore info
        LOG_INFO_FMT(
            "restore size = %s, file total = %u", strZ(strSizeFormat(sizeRestored)), manifestFileTotal(jobData.manifest));

        if (checksumErrorFileTotal > 0)
        {
            LOG_WARN_FMT(
                CFGCMD_RESTORE " command encountered page checksum error(s) in %u file(s), check the log file for details",
                checksumErrorFileTotal);
        }

        storageRemoveP(storagePgWrite(), STRDEF(RESTORE_FORK_FILE));
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN_VOID();
}
