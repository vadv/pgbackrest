/***********************************************************************************************************************************
Fork Command
***********************************************************************************************************************************/
#include <build.h>

#include "command/archive/common.h"
#include "command/archive/fork.h"
#include "command/backup/common.h"
#include "command/fork/common.h"
#include "command/fork/fork.h"
#include "command/stanza/common.h"
#include "common/compress/helper.h"
#include "common/crypto/cipherBlock.h"
#include "common/debug.h"
#include "common/format/cipherBlockFormat.h"
#include "common/format/format.h"
#include "common/io/io.h"
#include "common/lock.h"
#include "common/log.h"
#include "common/regExp.h"
#include "config/config.h"
#include "info/infoArchive.h"
#include "info/infoBackup.h"
#include "postgres/interface.h"
#include "postgres/version.h"
#include "protocol/helper.h"
#include "storage/helper.h"

static IoRead *
forkArchiveRead(const Storage *const storage, const String *const path, const CipherSpecMap *const cipher)
{
    IoRead *const read = storageReadIo(storageNewReadP(storage, path));
    cipherBlockFormatFilterGroupReadAddMap(ioReadFilterGroup(read), cipher);
    const CompressType compression = compressTypeFromName(path);

    if (compression != compressTypeNone)
        ioFilterGroupAdd(ioReadFilterGroup(read), decompressFilterP(compression));

    return read;
}

static const String *
forkSourceWalPath(
    const Storage *const source, const String *const stanza, const String *const archiveId, const String *const segment)
{
    const String *const directory = forkSourcePath(
        stanza, STORAGE_PATH_ARCHIVE, strNewFmt("%s/%s", strZ(archiveId), strZ(strSubN(segment, 0, 16))));
    const StringList *const files = storageListP(
        source, directory,
        .expression = strNewFmt("^%s-[0-9a-f]{40}" COMPRESS_TYPE_REGEXP "{0,1}$", strZ(segment)));

    CHECK_FMT(FileMissingError, strLstSize(files) != 0, "source WAL segment %s is missing", strZ(segment));
    CHECK_FMT(ArchiveDuplicateError, strLstSize(files) == 1, "source WAL segment %s has multiple copies", strZ(segment));
    return strNewFmt("%s/%s", strZ(directory), strZ(strLstGet(files, 0)));
}

// Include the backup chain so later differential backups can use the inherited full backup.
static void
forkBackupDependency(const InfoBackup *const info, const String *const label, StringList *const labels)
{
    CHECK_FMT(
        FormatError, regExpMatchOne(backupRegExpP(.full = true, .differential = true, .incremental = true), label),
        "invalid source backup label '%s'", strZ(label));

    if (strLstExists(labels, label))
        return;

    CHECK_FMT(BackupSetInvalidError, infoBackupLabelExists(info, label), "source backup %s is missing", strZ(label));
    strLstAdd(labels, label);
    const InfoBackupData *const data = infoBackupDataByLabel(info, label);

    if (data->backupPrior != NULL)
        forkBackupDependency(info, data->backupPrior, labels);

    if (data->backupReference != NULL)
    {
        for (unsigned int idx = 0; idx < strLstSize(data->backupReference); idx++)
            forkBackupDependency(info, strLstGet(data->backupReference, idx), labels);
    }
}

static const String *
forkBackupSelect(
    const InfoBackup *const info, const unsigned int pgId, const String *const history, const unsigned int timeline)
{
    const bool timeTarget = strEqZ(cfgOptionDisplay(cfgOptType), "time");
    const time_t targetTime = timeTarget ? cvtZToTime(strZ(cfgOptionStr(cfgOptTarget))) : 0;
    const uint64_t targetLsn = timeTarget ? 0 : pgLsnFromStr(cfgOptionStr(cfgOptTarget));

    for (unsigned int idx = infoBackupDataTotal(info); idx > 0; idx--)
    {
        const InfoBackupData data = infoBackupData(info, idx - 1);

        if (!data.optionOnline)
            continue;

        if (cfgOptionSource(cfgOptSet) != cfgSourceDefault && !strEqZ(cfgOptionStr(cfgOptSet), "latest") &&
            !strEq(cfgOptionStr(cfgOptSet), data.backupLabel))
            continue;

        // Check both backup endpoints. The backup can become consistent after the timeline histories diverge.
        if (history != NULL &&
            (data.backupPgId != pgId || data.backupLsnStart == NULL || data.backupLsnStop == NULL ||
             data.backupArchiveStart == NULL || data.backupArchiveStop == NULL || pgLsnFromStr(data.backupLsnStop) == 0 ||
             !archiveForkHistoryContains(
                 history, timeline, pgTimelineFromWalSegment(data.backupArchiveStart), pgLsnFromStr(data.backupLsnStart)) ||
             !archiveForkHistoryContains(
                 history, timeline, pgTimelineFromWalSegment(data.backupArchiveStop), pgLsnFromStr(data.backupLsnStop) - 1)))
            continue;

        // Whole-second timestamps cannot establish that a backup completed before a target within the same second.
        if ((timeTarget && data.backupTimestampStop < targetTime) ||
            (!timeTarget && data.backupLsnStop != NULL && pgLsnFromStr(data.backupLsnStop) <= targetLsn))
        {
            CHECK_FMT(
                FormatError,
                regExpMatchOne(backupRegExpP(.full = true, .differential = true, .incremental = true), data.backupLabel),
                "invalid source backup label '%s'", strZ(data.backupLabel));
            return data.backupLabel;
        }
    }

    THROW(BackupSetInvalidError, "no source backup on the selected timeline finishes before the requested recovery target");
}

static void
forkManifestSave(Manifest *const manifest, const unsigned int repoIdx, const CipherSpec *const cipher)
{
    const String *const path = strNewFmt(
        STORAGE_REPO_BACKUP "/%s/" BACKUP_MANIFEST_FILE, strZ(manifestData(manifest)->backupLabel));
    IoWrite *const write = storageWriteIo(storageNewWriteP(storageRepoIdxWrite(repoIdx), path));
    cipherBlockFilterGroupAdd(ioWriteFilterGroup(write), cipherModeEncrypt, cipher);
    manifestSave(manifest, write);
    storageCopyP(
        storageNewReadP(storageRepoIdx(repoIdx), path),
        storageNewWriteP(storageRepoIdxWrite(repoIdx), strNewFmt("%s" INFO_COPY_EXT, strZ(path))));
}

static void
forkHistorySave(
    const unsigned int repoIdx, const InfoArchive *const archive, const String *const name, const String *const history)
{
    const CipherSpecMap *const cipherMap = infoArchiveCipherSpecMap(archive);
    const String *const keyId = cipherSpecMapIdCurrent(cipherMap);
    StorageWrite *const write = storageNewWriteP(
        storageRepoIdxWrite(repoIdx), strNewFmt(STORAGE_REPO_ARCHIVE "/%s/%s", strZ(infoArchiveId(archive)), strZ(name)));
    cipherBlockFormatFilterGroupWriteAddP(
        ioWriteFilterGroup(storageWriteIo(write)),
        keyId == NULL ? infoArchiveCipherSpec(archive) : cipherSpecMapGet(cipherMap, keyId),
        infoArchiveFormat(archive), .keyId = keyId);
    storagePutP(write, BUFSTR(history));
}

static unsigned int
forkTargetTimeline(const String *const option, const unsigned int current)
{
    const unsigned int result =
        strEqZ(option, "current") ? current :
            (strBeginsWithZ(option, "0x") ? cvtZToUIntBase(strZ(option) + 2, 16) : cvtZToUInt(strZ(option)));
    CHECK(OptionInvalidError, result > 0, "target timeline must be greater than zero");
    return result;
}

static InfoFork *
forkPrepare(const unsigned int repoIdx, const unsigned int sourceRepoIdx)
{
    const String *const sourceStanza = cfgOptionStr(cfgOptSourceStanza);
    CHECK(OptionInvalidError, repoIsLocal(repoIdx) && repoIsLocal(sourceRepoIdx), "fork requires direct repository access");
    CHECK(
        OptionInvalidError, !strEq(sourceStanza, cfgOptionStr(cfgOptStanza)),
        "source and destination stanza names must differ");

    // Use the backup lock to prevent changes during restore, backup or expire.
    lockAcquireP(
        strNewFmt("%s-backup-%u" LOCK_FILE_EXT, strZ(cfgOptionStr(cfgOptStanza)), cfgOptionGroupIdxToKey(cfgOptGrpRepo, repoIdx)));
    InfoFork *info = forkInfoLoad(repoIdx);

    if (info == NULL)
    {
        CHECK(
            PathNotEmptyError,
            strLstEmpty(storageListP(storageRepoIdx(repoIdx), STORAGE_REPO_BACKUP_STR)) &&
            strLstEmpty(storageListP(storageRepoIdx(repoIdx), STORAGE_REPO_ARCHIVE_STR)),
            "destination stanza is not empty");
    }
    else
    {
        const InfoForkData *const prior = infoForkData(info);
        CHECK(
            OptionInvalidError,
            strEq(prior->sourceStanza, sourceStanza) && forkSourceRepoIdx(info) == sourceRepoIdx &&
            strEq(prior->targetType, cfgOptionDisplay(cfgOptType)) && strEq(prior->targetValue, cfgOptionStr(cfgOptTarget)) &&
            prior->targetExclusive == cfgOptionBool(cfgOptTargetExclusive) &&
            (cfgOptionSource(cfgOptSet) == cfgSourceDefault || strEqZ(cfgOptionStr(cfgOptSet), "latest") ||
             strEq(prior->backupLabel, cfgOptionStr(cfgOptSet))),
            "existing fork has a different source, target or backup");

        if (prior->state == infoForkStatePublished)
        {
            if (cfgOptionTest(cfgOptTargetTimeline) && !strEqZ(cfgOptionStr(cfgOptTargetTimeline), "latest"))
            {
                const Manifest *const saved = forkSourceManifestLoad(repoIdx, cfgOptionStr(cfgOptStanza), prior->backupLabel);
                CHECK(
                    OptionInvalidError,
                    prior->sourceTimeline == forkTargetTimeline(
                        cfgOptionStr(cfgOptTargetTimeline), pgTimelineFromWalSegment(manifestData(saved)->archiveStart)),
                    "existing fork has a different target timeline");
            }

            LOG_INFO_FMT("fork stanza '%s' is already published", strZ(cfgOptionStr(cfgOptStanza)));
            return info;
        }
    }

    // Lock the source against expiration before reading manifests or saving the retention pin.
    lockAcquireP(strNewFmt(
                     "%s-backup-%u" LOCK_FILE_EXT, strZ(sourceStanza), cfgOptionGroupIdxToKey(cfgOptGrpRepo, sourceRepoIdx)));
    const Storage *const source = storageRepoIdx(sourceRepoIdx);
    CHECK(
        OptionInvalidError,
        !storageExistsP(source, forkSourcePath(sourceStanza, STORAGE_PATH_BACKUP, STRDEF(INFO_FORK_FILE))) &&
        !storageExistsP(
            source, forkSourcePath(sourceStanza, STORAGE_PATH_BACKUP, STRDEF(INFO_FORK_FILE INFO_COPY_EXT))),
        "forking an inherited stanza is not supported yet");
    const InfoBackup *const sourceBackup = infoBackupLoadFile(
        source, forkSourcePath(sourceStanza, STORAGE_PATH_BACKUP, STRDEF(INFO_BACKUP_FILE)), cfgCipherSpecMainIdx(sourceRepoIdx));
    const InfoArchive *const sourceArchive = infoArchiveLoadFile(
        source, forkSourcePath(sourceStanza, STORAGE_PATH_ARCHIVE, STRDEF(INFO_ARCHIVE_FILE)), cfgCipherSpecMainIdx(sourceRepoIdx));
    const String *backupLabel = info == NULL ? forkBackupSelect(sourceBackup, 0, NULL, 0) : infoForkData(info)->backupLabel;
    const InfoBackupData *selected = infoBackupDataByLabel(sourceBackup, backupLabel);
    Manifest *selectedManifest = manifestLoadFile(
        source, forkSourcePath(sourceStanza, STORAGE_PATH_BACKUP, strNewFmt("%s/" BACKUP_MANIFEST_FILE, strZ(backupLabel))),
        infoBackupCipherSpec(sourceBackup));
    const ManifestData *manifest = manifestData(selectedManifest);
    CHECK(OptionInvalidError, manifest->backupOptionOnline, "fork requires an online source backup");
    const String *const archiveId = infoArchiveIdHistoryMatch(
        sourceArchive, manifest->pgId, manifest->pgVersion, manifest->pgSystemId);
    // Save the source timeline history so recovery follows the selected timeline.
    const String *const historyPath = forkSourcePath(sourceStanza, STORAGE_PATH_ARCHIVE, archiveId);
    const StringList *const histories = strLstSort(
        storageListP(source, historyPath, .expression = STRDEF("^[0-9A-F]{8}\\.history$")), sortOrderAsc);
    unsigned int timeline = pgTimelineFromWalSegment(selected->backupArchiveStart);
    const String *history = STRDEF("");

    if (!strLstEmpty(histories))
    {
        const unsigned int latest = cvtZToUIntBase(strZ(strSubN(strLstGet(histories, strLstSize(histories) - 1), 0, 8)), 16);
        if (latest > timeline)
            timeline = latest;
    }

    if (cfgOptionTest(cfgOptTargetTimeline) && !strEqZ(cfgOptionStr(cfgOptTargetTimeline), "latest"))
    {
        timeline = forkTargetTimeline(cfgOptionStr(cfgOptTargetTimeline), pgTimelineFromWalSegment(selected->backupArchiveStart));
        CHECK(
            OptionInvalidError, info == NULL || timeline == infoForkData(info)->sourceTimeline,
            "existing fork has a different target timeline");
    }
    else if (info != NULL)
        timeline = infoForkData(info)->sourceTimeline;

    CHECK(FormatError, timeline < UINT32_MAX, "source timeline range is exhausted");

    if (timeline > 1)
    {
        IoRead *const read = forkArchiveRead(
            source, strNewFmt("%s/%08X.history", strZ(historyPath), timeline),
            infoArchiveCipherSpecMap(sourceArchive));
        ioReadOpen(read);
        history = strNewBuf(ioReadBuf(read));
        ioReadClose(read);
    }

    if (info != NULL)
        CHECK(
            ArchiveMismatchError, strEq(history, infoForkData(info)->sourceHistory), "source timeline history changed during fork");

    if (info == NULL)
    {
        backupLabel = forkBackupSelect(sourceBackup, manifest->pgId, history, timeline);
        selected = infoBackupDataByLabel(sourceBackup, backupLabel);
        selectedManifest = manifestLoadFile(
            source, forkSourcePath(sourceStanza, STORAGE_PATH_BACKUP, strNewFmt("%s/" BACKUP_MANIFEST_FILE, strZ(backupLabel))),
            infoBackupCipherSpec(sourceBackup));
        manifest = manifestData(selectedManifest);
    }

    StringList *const labels = strLstNew();
    forkBackupDependency(sourceBackup, backupLabel, labels);
    strLstSort(labels, sortOrderAsc);

    // PostgreSQL stops searching for the latest timeline at the first missing history file in numeric order.
    for (unsigned int next = pgTimelineFromWalSegment(selected->backupArchiveStart) + 1; next <= timeline; next++)
    {
        CHECK_FMT(
            FileMissingError, strLstExists(histories, strNewFmt("%08X.history", next)),
            "source timeline history %08X.history is required for archive-only replica discovery", next);
        CHECK(FormatError, next != UINT32_MAX, "source timeline range is exhausted");
    }

    IoRead *const walRead = forkArchiveRead(
        source, forkSourceWalPath(source, sourceStanza, archiveId, selected->backupArchiveStart),
        infoArchiveCipherSpecMap(sourceArchive));
    ioReadOpen(walRead);
    Buffer *const walHeader = bufNew(PG_WAL_HEADER_SIZE);
    ioRead(walRead, walHeader);
    const PgWal wal = pgWalFromBuffer(walHeader, NULL);
    ioReadClose(walRead);
    CHECK(ArchiveMismatchError, wal.systemId == manifest->pgSystemId, "source WAL and backup system identifiers differ");

    // Preserve the backup format so later incremental backups can use the inherited backup.
    const unsigned int format = selected->backrestFormat;
    if (info == NULL)
    {
        info = infoForkNew(
            format,
            &(InfoForkData)
        {
            .sourceId = forkSourceId(sourceRepoIdx, sourceStanza, archiveId, manifest->pgSystemId),
            .sourceRepoKey = cfgOptionGroupIdxToKey(cfgOptGrpRepo, sourceRepoIdx), .sourceStanza = sourceStanza,
            .sourceArchiveId = archiveId, .backupLabel = backupLabel, .importedBackupList = labels,
            .pgVersion = manifest->pgVersion, .systemId = manifest->pgSystemId, .walSegmentSize = wal.size,
            .sourceTimeline = timeline, .sourceHistory = history, .targetType = cfgOptionDisplay(cfgOptType),
            .targetValue = cfgOptionStr(cfgOptTarget), .targetExclusive = cfgOptionBool(cfgOptTargetExclusive),
            .state = infoForkStatePreparing, .history = STRDEF(""),
        });
    }

    // Save the preparation state for retries. Other commands require published metadata.
    infoForkSaveFile(info, storageRepoIdxWrite(repoIdx), STRDEF(INFO_FORK_PATH_FILE), cfgCipherSpecMainIdx(repoIdx));

    // Save the retention pin before backup references to protect source data from expiration during preparation.
    infoForkSaveFile(
        info, storageRepoIdxWrite(sourceRepoIdx),
        forkSourcePath(
            sourceStanza, STORAGE_PATH_BACKUP,
            strNewFmt(FORK_PIN_PATH "/%s", strZ(forkPinName(repoIdx, cfgOptionStr(cfgOptStanza))))),
        cfgCipherSpecMainIdx(sourceRepoIdx));

    InfoBackup *const backup =
        storageExistsP(storageRepoIdx(repoIdx), INFO_BACKUP_PATH_FILE_STR) ||
        storageExistsP(storageRepoIdx(repoIdx), INFO_BACKUP_PATH_FILE_COPY_STR) ?
            infoBackupLoadFile(storageRepoIdx(repoIdx), INFO_BACKUP_PATH_FILE_STR, cfgCipherSpecMainIdx(repoIdx)) :
            infoBackupNew(
                manifest->pgVersion, manifest->pgSystemId, manifest->pgCatalogVersion, format,
                cipherSpecGen(cfgOptionIdxStrId(cfgOptRepoCipherType, repoIdx), format));
    const CipherSpec *const manifestCipher = infoBackupCipherSpec(backup);
    InfoPgData pg =
    {
        .id = manifest->pgId, .systemId = manifest->pgSystemId, .version = manifest->pgVersion,
        .catalogVersion = manifest->pgCatalogVersion, .controlVersion = pgControlVersion(manifest->pgVersion),
    };
    lstClear(((InfoPgPub *)infoBackupPg(backup))->history);
    infoPgAdd(infoBackupPg(backup), &pg);

    InfoArchive *archive;

    if (storageExistsP(storageRepoIdx(repoIdx), INFO_ARCHIVE_PATH_FILE_STR) ||
        storageExistsP(storageRepoIdx(repoIdx), INFO_ARCHIVE_PATH_FILE_COPY_STR))
    {
        archive = infoArchiveLoadFile(storageRepoIdx(repoIdx), INFO_ARCHIVE_PATH_FILE_STR, cfgCipherSpecMainIdx(repoIdx));
    }
    else
    {
        const CipherSpec *const archiveCipher = cipherSpecGen(cfgOptionIdxStrId(cfgOptRepoCipherType, repoIdx), format);
        CipherSpecMap *const archiveCipherMap = cipherSpecMapNew();

        if (cipherSpecType(archiveCipher) != cipherTypeNone && format < REPOSITORY_FORMAT_6)
            cipherSpecMapAdd(archiveCipherMap, CIPHER_SPEC_MAP_ID_DEFAULT_STR, archiveCipher);

        archive = infoArchiveNew(manifest->pgVersion, manifest->pgSystemId, format, archiveCipherMap);
        lstClear(((InfoPgPub *)infoArchivePg(archive))->history);
        infoPgAdd(infoArchivePg(archive), &pg);

        if (cipherSpecType(archiveCipher) != cipherTypeNone && format >= REPOSITORY_FORMAT_6)
            infoArchiveCipherRotate(archive, archiveCipher, time(NULL));
    }

    infoArchiveSaveFile(archive, storageRepoIdxWrite(repoIdx), INFO_ARCHIVE_PATH_FILE_STR, cfgCipherSpecMainIdx(repoIdx));
    // Save the manifest key before copying manifests. Retries must use the same key.
    infoBackupSaveFile(backup, storageRepoIdxWrite(repoIdx), INFO_BACKUP_PATH_FILE_STR, cfgCipherSpecMainIdx(repoIdx));

    for (unsigned int idx = 0; idx < strLstSize(labels); idx++)
    {
        Manifest *const copy = manifestLoadFile(
            source,
            forkSourcePath(sourceStanza, STORAGE_PATH_BACKUP, strNewFmt("%s/" BACKUP_MANIFEST_FILE, strZ(strLstGet(labels, idx)))),
            infoBackupCipherSpec(sourceBackup));
        CHECK(BackupSetInvalidError, manifestData(copy)->pgId == manifest->pgId, "backup dependency has a different database id");
        forkManifestSave(copy, repoIdx, manifestCipher);
        if (infoBackupLabelExists(backup, manifestData(copy)->backupLabel))
            infoBackupDataDelete(backup, manifestData(copy)->backupLabel);
        infoBackupDataAdd(backup, copy);
        manifestFree(copy);
    }

    infoBackupSaveFile(backup, storageRepoIdxWrite(repoIdx), INFO_BACKUP_PATH_FILE_STR, cfgCipherSpecMainIdx(repoIdx));

    for (unsigned int idx = 0; idx < strLstSize(histories); idx++)
    {
        const String *const name = strLstGet(histories, idx);

        if (cvtZToUIntBase(strZ(strSubN(name, 0, 8)), 16) > timeline)
            continue;

        IoRead *const read = forkArchiveRead(
            source, strNewFmt("%s/%s", strZ(historyPath), strZ(name)),
            infoArchiveCipherSpecMap(sourceArchive));
        ioReadOpen(read);
        forkHistorySave(repoIdx, archive, name, strNewBuf(ioReadBuf(read)));
        ioReadClose(read);
    }

    infoForkPublishedSet(info);
    infoForkSaveFile(info, storageRepoIdxWrite(repoIdx), STRDEF(INFO_FORK_PATH_FILE), cfgCipherSpecMainIdx(repoIdx));

    LOG_INFO_FMT(
        "fork stanza '%s' from '%s', backup %s: copied %u manifests; backup data remains in the source repository",
        strZ(cfgOptionStr(cfgOptStanza)), strZ(sourceStanza), strZ(backupLabel), strLstSize(labels));
    return info;
}

FN_EXTERN void
cmdFork(void)
{
    FUNCTION_LOG_VOID(logLevelDebug);

    MEM_CONTEXT_TEMP_BEGIN()
    {
        CHECK(
            OptionInvalidError,
            strEqZ(cfgOptionDisplay(cfgOptType), "time") || strEqZ(cfgOptionDisplay(cfgOptType), "lsn"),
            "fork requires --type=time or --type=lsn and --target");
        CHECK(OptionRequiredError, cfgOptionTest(cfgOptTarget), "fork requires --target");
        CHECK(
            OptionInvalidError,
            !strEqZ(cfgOptionDisplay(cfgOptType), "time") ||
            regExpMatchOne(STRDEF("[+-][0-9]{2}(:{0,1}[0-9]{2}){0,1}$"), cfgOptionStr(cfgOptTarget)),
            "fork time target requires an explicit numeric time zone, for example +00 or +03:00");
        CHECK(
            OptionRequiredError, cfgOptionTest(cfgOptRepo) || cfgOptionGroupIdxTotal(cfgOptGrpRepo) == 1,
            "fork requires --repo when multiple repositories are configured");
        const unsigned int repoIdx = cfgOptionGroupIdxDefault(cfgOptGrpRepo);
        unsigned int sourceRepoIdx = repoIdx;

        if (cfgOptionTest(cfgOptSourceRepo))
        {
            bool found = false;
            for (unsigned int idx = 0; idx < cfgOptionGroupIdxTotal(cfgOptGrpRepo); idx++)
            {
                if (cfgOptionGroupIdxToKey(cfgOptGrpRepo, idx) == cfgOptionUInt(cfgOptSourceRepo))
                {
                    sourceRepoIdx = idx;
                    found = true;
                    break;
                }
            }
            CHECK(OptionInvalidError, found, "source-repo does not identify a configured repository");
        }

        // Set the fork repository in archive commands, including when fork selected it by default.
        cfgOptionSet(cfgOptRepo, cfgSourceParam, VARINT64(cfgOptionGroupIdxToKey(cfgOptGrpRepo, repoIdx)));
        forkPrepare(repoIdx, sourceRepoIdx);
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN_VOID();
}
