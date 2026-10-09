/***********************************************************************************************************************************
Fork Info Handler
***********************************************************************************************************************************/
#include <build.h>

#include "command/backup/common.h"
#include "common/debug.h"
#include "common/format/cipherBlockFormat.h"
#include "common/io/bufferWrite.h"
#include "common/io/io.h"
#include "common/log.h"
#include "common/regExp.h"
#include "info/infoFork.h"

struct InfoFork
{
    Info *info;
    InfoForkData data;
    String *history;
};

/***********************************************************************************************************************************
Validate fork metadata
***********************************************************************************************************************************/
static void
infoForkValidate(const InfoForkData *const data)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM_P(VOID, data);
    FUNCTION_TEST_END();

    ASSERT(data != NULL);

    CHECK(FormatError, data->sourceId != NULL && !strEmpty(data->sourceId), "fork source identity is required");
    CHECK(FormatError, data->sourceRepoKey > 0, "fork source repository key must be greater than zero");
    CHECK(
        FormatError,
        data->sourceStanza != NULL && !strEmpty(data->sourceStanza) && strchr(strZ(data->sourceStanza), '/') == NULL &&
        !strEqZ(data->sourceStanza, ".") && !strEqZ(data->sourceStanza, ".."),
        "invalid fork source stanza");
    CHECK(
        FormatError,
        data->sourceArchiveId != NULL && !strEmpty(data->sourceArchiveId) && strchr(strZ(data->sourceArchiveId), '/') == NULL &&
        !strEqZ(data->sourceArchiveId, ".") && !strEqZ(data->sourceArchiveId, ".."),
        "invalid fork source archive id");
    CHECK(FormatError, data->pgVersion > 0 && data->systemId > 0, "fork PostgreSQL identity is required");
    CHECK(
        FormatError,
        data->walSegmentSize >= 1024 * 1024 && data->walSegmentSize <= 1024 * 1024 * 1024 &&
        (data->walSegmentSize & (data->walSegmentSize - 1)) == 0,
        "fork WAL segment size must be a power of two between 1MB and 1GB");
    CHECK(FormatError, data->sourceTimeline > 0, "fork source timeline must be greater than zero");
    CHECK(
        FormatError, data->sourceHistory != NULL && (data->sourceTimeline == 1 || !strEmpty(data->sourceHistory)),
        "fork source history is required");
    CHECK(
        FormatError, data->targetType != NULL && (strEqZ(data->targetType, "time") || strEqZ(data->targetType, "lsn")),
        "fork target type must be time or lsn");
    CHECK(FormatError, data->targetValue != NULL && !strEmpty(data->targetValue), "fork target value is required");
    CHECK(
        FormatError, data->importedBackupList != NULL && !strLstEmpty(data->importedBackupList),
        "fork imported backup list is required");
    CHECK(
        FormatError, data->backupLabel != NULL && strLstExists(data->importedBackupList, data->backupLabel),
        "fork selected backup must be in the imported backup list");

    MEM_CONTEXT_TEMP_BEGIN()
    {
        RegExp *const expression = regExpNew(backupRegExpP(.full = true, .differential = true, .incremental = true));

        for (unsigned int backupIdx = 0; backupIdx < strLstSize(data->importedBackupList); backupIdx++)
        {
            const String *const label = strLstGet(data->importedBackupList, backupIdx);
            CHECK_FMT(FormatError, regExpMatch(expression, label), "invalid fork imported backup label '%s'", strZ(label));
            CHECK_FMT(
                FormatError, strLstFindIdxP(data->importedBackupList, label) == backupIdx,
                "duplicate fork imported backup label '%s'", strZ(label));
        }
    }
    MEM_CONTEXT_TEMP_END();

    CHECK(
        FormatError, data->state == infoForkStatePreparing || data->state == infoForkStatePublished,
        "invalid fork state");
    CHECK(
        FormatError, data->state == infoForkStatePublished || data->ownTimeline == 0,
        "unpublished fork cannot have a branch");
    CHECK(FormatError, data->history != NULL, "fork accepted history is required");

    if (data->ownTimeline == 0)
    {
        CHECK(
            FormatError, data->branchLsn == 0 && data->historyTimeline == 0 && strEmpty(data->history),
            "fork branch metadata requires an own timeline");
    }
    else
    {
        CHECK(
            FormatError,
            data->ownTimeline > data->sourceTimeline && data->branchLsn > 0 && data->historyTimeline >= data->ownTimeline &&
            !strEmpty(data->history),
            "invalid fork branch metadata");
    }

    FUNCTION_TEST_RETURN_VOID();
}

/**********************************************************************************************************************************/
FN_EXTERN InfoFork *
infoForkNew(const unsigned int format, const InfoForkData *const data)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(UINT, format);
        FUNCTION_LOG_PARAM_P(VOID, data);
    FUNCTION_LOG_END();

    infoForkValidate(data);

    OBJ_NEW_BEGIN(InfoFork, .childQty = MEM_CONTEXT_QTY_MAX)
    {
        *this = (InfoFork){.info = infoNew(format, NULL), .data = *data};
        this->data.sourceId = strDup(data->sourceId);
        this->data.sourceStanza = strDup(data->sourceStanza);
        this->data.sourceArchiveId = strDup(data->sourceArchiveId);
        this->data.backupLabel = strDup(data->backupLabel);
        this->data.importedBackupList = strLstDup(data->importedBackupList);
        this->data.sourceHistory = strDup(data->sourceHistory);
        this->data.targetType = strDup(data->targetType);
        this->data.targetValue = strDup(data->targetValue);
        this->history = strDup(data->history);
        this->data.history = this->history;
    }
    OBJ_NEW_END();

    FUNCTION_LOG_RETURN(INFO_FORK, this);
}

/**********************************************************************************************************************************/
FN_EXTERN const InfoForkData *
infoForkData(const InfoFork *const this)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(INFO_FORK, this);
    FUNCTION_TEST_END();

    ASSERT(this != NULL);

    FUNCTION_TEST_RETURN_CONST_P(VOID, &this->data);
}

/**********************************************************************************************************************************/
FN_EXTERN unsigned int
infoForkFormat(const InfoFork *const this)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(INFO_FORK, this);
    FUNCTION_TEST_END();

    ASSERT(this != NULL);

    FUNCTION_TEST_RETURN(UINT, infoFormat(this->info));
}

/**********************************************************************************************************************************/
FN_EXTERN void
infoForkHistorySet(
    InfoFork *const this, const unsigned int historyTimeline, const uint64_t branchLsn, const String *const history)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(INFO_FORK, this);
        FUNCTION_LOG_PARAM(UINT, historyTimeline);
        FUNCTION_LOG_PARAM(UINT64, branchLsn);
        FUNCTION_LOG_PARAM(STRING, history);
    FUNCTION_LOG_END();

    ASSERT(this != NULL);
    ASSERT(history != NULL);

    CHECK(FormatError, historyTimeline > this->data.sourceTimeline && branchLsn > 0, "invalid fork branch metadata");
    CHECK(FormatError, historyTimeline >= this->data.historyTimeline, "fork history timeline cannot decrease");
    CHECK(
        FormatError, this->data.ownTimeline == 0 || branchLsn == this->data.branchLsn,
        "fork branch has already been confirmed");

    if (historyTimeline == this->data.historyTimeline)
        CHECK(FormatError, strEq(this->data.history, history), "fork history for this timeline has already been confirmed");
    else
    {
        InfoForkData data = this->data;

        if (data.ownTimeline == 0)
        {
            data.ownTimeline = historyTimeline;
            data.branchLsn = branchLsn;
        }

        data.historyTimeline = historyTimeline;
        data.history = history;
        infoForkValidate(&data);

        MEM_CONTEXT_OBJ_BEGIN(this)
        {
            String *const historyCopy = strDup(history);
            strFree(this->history);
            data.history = historyCopy;
            this->history = historyCopy;
            this->data = data;
        }
        MEM_CONTEXT_OBJ_END();
    }

    FUNCTION_LOG_RETURN_VOID();
}

/**********************************************************************************************************************************/
FN_EXTERN void
infoForkPublishedSet(InfoFork *const this)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(INFO_FORK, this);
    FUNCTION_LOG_END();

    ASSERT(this != NULL);
    this->data.state = infoForkStatePublished;
    infoForkValidate(&this->data);

    FUNCTION_LOG_RETURN_VOID();
}

/**********************************************************************************************************************************/
#define INFO_FORK_SECTION                                          "fork"
#define INFO_FORK_KEY_FORMAT                                       "format"
#define INFO_FORK_KEY_METADATA                                     "metadata"

typedef struct InfoForkLoadData
{
    InfoFork *infoFork;
    unsigned int format;
    bool metadataFound;
} InfoForkLoadData;

static void
infoForkLoadCallback(void *const data, const String *const section, const String *const key, JsonRead *const json)
{
    FUNCTION_LOG_BEGIN(logLevelTrace);
        FUNCTION_LOG_PARAM_P(VOID, data);
        FUNCTION_LOG_PARAM(STRING, section);
        FUNCTION_LOG_PARAM(STRING, key);
        FUNCTION_LOG_PARAM(JSON_READ, json);
    FUNCTION_LOG_END();

    InfoForkLoadData *const loadData = data;
    InfoFork *const this = loadData->infoFork;

    CHECK_FMT(FormatError, strEqZ(section, INFO_FORK_SECTION), "invalid fork section '%s'", strZ(section));

    if (strEqZ(key, INFO_FORK_KEY_FORMAT))
    {
        CHECK(FormatError, loadData->format == 0, "duplicate fork format");
        loadData->format = jsonReadUInt(json);
        CHECK_FMT(
            FormatError, loadData->format == INFO_FORK_FORMAT, "unsupported fork format %u", loadData->format);
    }
    else
    {
        CHECK_FMT(FormatError, strEqZ(key, INFO_FORK_KEY_METADATA), "invalid fork key '%s'", strZ(key));
        CHECK(FormatError, !loadData->metadataFound, "duplicate fork metadata");

        MEM_CONTEXT_OBJ_BEGIN(this)
        {
            jsonReadObjectBegin(json);
            this->data.backupLabel = jsonReadStr(jsonReadKeyRequireZ(json, "backup-label"));
            this->data.branchLsn = jsonReadUInt64(jsonReadKeyRequireZ(json, "branch-lsn"));
            this->history = jsonReadStr(jsonReadKeyRequireZ(json, "history"));
            this->data.history = this->history;
            this->data.historyTimeline = jsonReadUInt(jsonReadKeyRequireZ(json, "history-timeline"));
            this->data.importedBackupList = jsonReadStrLst(jsonReadKeyRequireZ(json, "imported-backup-list"));
            this->data.ownTimeline = jsonReadUInt(jsonReadKeyRequireZ(json, "own-timeline"));
            this->data.pgVersion = jsonReadUInt(jsonReadKeyRequireZ(json, "pg-version"));
            this->data.sourceArchiveId = jsonReadStr(jsonReadKeyRequireZ(json, "source-archive-id"));
            this->data.sourceHistory = jsonReadStr(jsonReadKeyRequireZ(json, "source-history"));
            this->data.sourceId = jsonReadStr(jsonReadKeyRequireZ(json, "source-id"));
            this->data.sourceRepoKey = jsonReadUInt(jsonReadKeyRequireZ(json, "source-repo-key"));
            this->data.sourceStanza = jsonReadStr(jsonReadKeyRequireZ(json, "source-stanza"));
            this->data.sourceTimeline = jsonReadUInt(jsonReadKeyRequireZ(json, "source-timeline"));

            String *const state = jsonReadStr(jsonReadKeyRequireZ(json, "state"));
            CHECK(
                FormatError, state != NULL && (strEqZ(state, "preparing") || strEqZ(state, "published")),
                "invalid fork state");
            this->data.state = strEqZ(state, "published") ? infoForkStatePublished : infoForkStatePreparing;
            strFree(state);

            this->data.systemId = jsonReadUInt64(jsonReadKeyRequireZ(json, "system-id"));
            this->data.targetExclusive = jsonReadBool(jsonReadKeyRequireZ(json, "target-exclusive"));
            this->data.targetType = jsonReadStr(jsonReadKeyRequireZ(json, "target-type"));
            this->data.targetValue = jsonReadStr(jsonReadKeyRequireZ(json, "target-value"));
            this->data.walSegmentSize = jsonReadUInt(jsonReadKeyRequireZ(json, "wal-segment-size"));
            jsonReadObjectEnd(json);
        }
        MEM_CONTEXT_OBJ_END();

        loadData->metadataFound = true;
    }

    FUNCTION_LOG_RETURN_VOID();
}

/**********************************************************************************************************************************/
FN_EXTERN InfoFork *
infoForkNewLoad(IoRead *const read, const CipherSpec *const cipherSpec)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(IO_READ, read);
        FUNCTION_LOG_PARAM(CIPHER_SPEC, cipherSpec);
    FUNCTION_LOG_END();

    ASSERT(read != NULL);
    ASSERT(cipherSpec != NULL);

    OBJ_NEW_BEGIN(InfoFork, .childQty = MEM_CONTEXT_QTY_MAX)
    {
        *this = (InfoFork){};
        InfoForkLoadData data = {.infoFork = this};
        this->info = infoNewLoad(read, cipherSpec, infoForkLoadCallback, &data);
        CHECK(FormatError, data.format == INFO_FORK_FORMAT && data.metadataFound, "fork format and metadata are required");
        infoForkValidate(&this->data);
    }
    OBJ_NEW_END();

    FUNCTION_LOG_RETURN(INFO_FORK, this);
}

/**********************************************************************************************************************************/
static void
infoForkSaveCallback(void *const data, const String *const sectionNext, InfoSave *const infoSaveData)
{
    FUNCTION_LOG_BEGIN(logLevelTrace);
        FUNCTION_LOG_PARAM_P(VOID, data);
        FUNCTION_LOG_PARAM(STRING, sectionNext);
        FUNCTION_LOG_PARAM(INFO_SAVE, infoSaveData);
    FUNCTION_LOG_END();

    FUNCTION_AUDIT_CALLBACK();

    const InfoFork *const this = data;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        if (infoSaveSection(infoSaveData, INFO_FORK_SECTION, sectionNext))
        {
            infoSaveValue(infoSaveData, INFO_FORK_SECTION, INFO_FORK_KEY_FORMAT, jsonFromVar(VARUINT(INFO_FORK_FORMAT)));

            JsonWrite *const json = jsonWriteObjectBegin(jsonWriteNewP());
            jsonWriteStr(jsonWriteKeyZ(json, "backup-label"), this->data.backupLabel);
            jsonWriteUInt64(jsonWriteKeyZ(json, "branch-lsn"), this->data.branchLsn);
            jsonWriteStr(jsonWriteKeyZ(json, "history"), this->data.history);
            jsonWriteUInt(jsonWriteKeyZ(json, "history-timeline"), this->data.historyTimeline);
            jsonWriteStrLst(jsonWriteKeyZ(json, "imported-backup-list"), this->data.importedBackupList);
            jsonWriteUInt(jsonWriteKeyZ(json, "own-timeline"), this->data.ownTimeline);
            jsonWriteUInt(jsonWriteKeyZ(json, "pg-version"), this->data.pgVersion);
            jsonWriteStr(jsonWriteKeyZ(json, "source-archive-id"), this->data.sourceArchiveId);
            jsonWriteStr(jsonWriteKeyZ(json, "source-history"), this->data.sourceHistory);
            jsonWriteStr(jsonWriteKeyZ(json, "source-id"), this->data.sourceId);
            jsonWriteUInt(jsonWriteKeyZ(json, "source-repo-key"), this->data.sourceRepoKey);
            jsonWriteStr(jsonWriteKeyZ(json, "source-stanza"), this->data.sourceStanza);
            jsonWriteUInt(jsonWriteKeyZ(json, "source-timeline"), this->data.sourceTimeline);
            jsonWriteZ(
                jsonWriteKeyZ(json, "state"), this->data.state == infoForkStatePublished ? "published" : "preparing");
            jsonWriteUInt64(jsonWriteKeyZ(json, "system-id"), this->data.systemId);
            jsonWriteBool(jsonWriteKeyZ(json, "target-exclusive"), this->data.targetExclusive);
            jsonWriteStr(jsonWriteKeyZ(json, "target-type"), this->data.targetType);
            jsonWriteStr(jsonWriteKeyZ(json, "target-value"), this->data.targetValue);
            jsonWriteUInt(jsonWriteKeyZ(json, "wal-segment-size"), this->data.walSegmentSize);
            infoSaveValue(infoSaveData, INFO_FORK_SECTION, INFO_FORK_KEY_METADATA, jsonWriteResult(jsonWriteObjectEnd(json)));
        }
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN_VOID();
}

/**********************************************************************************************************************************/
static void
infoForkSave(InfoFork *const this, IoWrite *const write)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(INFO_FORK, this);
        FUNCTION_LOG_PARAM(IO_WRITE, write);
    FUNCTION_LOG_END();

    ASSERT(this != NULL);
    ASSERT(write != NULL);

    infoForkValidate(&this->data);
    infoSave(this->info, write, infoForkSaveCallback, this);

    FUNCTION_LOG_RETURN_VOID();
}

/**********************************************************************************************************************************/
typedef struct InfoForkLoadFileData
{
    MemContext *memContext;
    const Storage *storage;
    const String *fileName;
    const CipherSpec *cipherSpec;
    InfoFork *infoFork;
} InfoForkLoadFileData;

static bool
infoForkLoadFileCallback(void *const data, const unsigned int try)
{
    FUNCTION_LOG_BEGIN(logLevelTrace);
        FUNCTION_LOG_PARAM_P(VOID, data);
        FUNCTION_LOG_PARAM(UINT, try);
    FUNCTION_LOG_END();

    InfoForkLoadFileData *const loadData = data;
    bool result = false;

    if (try < 2)
    {
        MEM_CONTEXT_TEMP_BEGIN()
        {
            const String *const fileName = try == 0 ? loadData->fileName : strNewFmt("%s" INFO_COPY_EXT, strZ(loadData->fileName));
            IoRead *const read = storageReadIo(storageNewReadP(loadData->storage, fileName));
            cipherBlockFormatFilterGroupReadAdd(ioReadFilterGroup(read), loadData->cipherSpec);
            loadData->infoFork = infoForkMove(infoForkNewLoad(read, loadData->cipherSpec), loadData->memContext);
            result = true;
        }
        MEM_CONTEXT_TEMP_END();
    }

    FUNCTION_LOG_RETURN(BOOL, result);
}

/**********************************************************************************************************************************/
FN_EXTERN InfoFork *
infoForkLoadFile(const Storage *const storage, const String *const fileName, const CipherSpec *const cipherSpec)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE, storage);
        FUNCTION_LOG_PARAM(STRING, fileName);
        FUNCTION_LOG_PARAM(CIPHER_SPEC, cipherSpec);
    FUNCTION_LOG_END();

    ASSERT(storage != NULL);
    ASSERT(fileName != NULL);
    ASSERT(cipherSpec != NULL);

    InfoForkLoadFileData data =
    {
        .memContext = memContextCurrent(),
        .storage = storage,
        .fileName = fileName,
        .cipherSpec = cipherSpec,
    };

    MEM_CONTEXT_TEMP_BEGIN()
    {
        const String *const path = storagePathP(storage, fileName);
        infoLoad(
            strNewFmt("unable to load fork info file '%s' or '%s" INFO_COPY_EXT "'", strZ(path), strZ(path)),
            infoForkLoadFileCallback, &data);
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(INFO_FORK, data.infoFork);
}

/**********************************************************************************************************************************/
FN_EXTERN void
infoForkSaveFile(
    InfoFork *const this, const Storage *const storage, const String *const fileName, const CipherSpec *const cipherSpec)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(INFO_FORK, this);
        FUNCTION_LOG_PARAM(STORAGE, storage);
        FUNCTION_LOG_PARAM(STRING, fileName);
        FUNCTION_LOG_PARAM(CIPHER_SPEC, cipherSpec);
    FUNCTION_LOG_END();

    ASSERT(this != NULL);
    ASSERT(storage != NULL);
    ASSERT(fileName != NULL);
    ASSERT(cipherSpec != NULL);

    MEM_CONTEXT_TEMP_BEGIN()
    {
        Buffer *const buffer = bufNew(ioBufferSize());
        IoWrite *const write = ioBufferWriteNew(buffer);
        cipherBlockFormatFilterGroupWriteAddP(ioWriteFilterGroup(write), cipherSpec, infoForkFormat(this));
        infoForkSave(this, write);
        // Write the .copy file first. If the next write fails, the main file cannot contain newer metadata.
        storagePutP(storageNewWriteP(storage, strNewFmt("%s" INFO_COPY_EXT, strZ(fileName))), buffer);
        storagePutP(storageNewWriteP(storage, fileName), buffer);
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN_VOID();
}
