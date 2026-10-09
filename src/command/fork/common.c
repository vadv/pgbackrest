/***********************************************************************************************************************************
Fork Repository Bindings
***********************************************************************************************************************************/
#include <build.h>

#include <inttypes.h>

#include "command/backup/common.h"
#include "command/fork/common.h"
#include "common/crypto/hash.h"
#include "common/debug.h"
#include "common/regExp.h"
#include "config/config.h"
#include "config/exec.h"
#include "config/parse.h"
#include "info/infoBackup.h"
#include "protocol/helper.h"
#include "storage/fork/storage.h"
#include "storage/helper.h"

static MemContext *forkStorageContext = NULL;
static const Storage **forkStorageCache = NULL;

static void
forkStorageReset(void *const data)
{
    (void)data;
    forkStorageContext = NULL;
    forkStorageCache = NULL;
}

FN_EXTERN String *
forkArchiveCommand(const ConfigCommand command)
{
    ASSERT(command == cfgCmdArchiveGet || command == cfgCmdArchivePush);
    String *const result = strNew();

    MEM_CONTEXT_TEMP_BEGIN()
    {
        KeyValue *const replace = kvNew();
        const char *const omit[] =
        {
            CFGOPT_EXEC_ID, CFGOPT_JOB_RETRY, CFGOPT_JOB_RETRY_INTERVAL, CFGOPT_LOG_LEVEL_CONSOLE, CFGOPT_LOG_LEVEL_FILE,
            CFGOPT_LOG_LEVEL_STDERR, CFGOPT_LOG_SUBPROCESS, CFGOPT_LOG_TIMESTAMP, CFGOPT_PROCESS_MAX, CFGOPT_CMD,
        };
        for (unsigned int idx = 0; idx < LENGTH_OF(omit); idx++)
            kvPut(replace, VARSTRZ(omit[idx]), NULL);

        StringList *const args = cfgExecParam(command, cfgCmdRoleMain, replace, true, false);
        strLstInsert(args, 0, cfgOptionStr(cfgOptCmd));

        for (unsigned int idx = 0; idx < strLstSize(args); idx++)
        {
            if (idx != 0)
                strCatChr(result, ' ');
            strCatChr(result, '\'');
            for (const char *ptr = strZ(strLstGet(args, idx)); *ptr != 0; ptr++)
            {
                if (*ptr == '\'')
                    strCatZ(result, "'\\''");
                else
                    strCatChr(result, *ptr);
            }
            strCatChr(result, '\'');
        }

        strCatZ(result, command == cfgCmdArchiveGet ? " %f \"%p\"" : " \"%p\"");
    }
    MEM_CONTEXT_TEMP_END();

    return result;
}

FN_EXTERN String *
forkPgConfigQuote(const String *const value)
{
    ASSERT(value != NULL);
    String *const result = strCatChr(strNew(), '\'');
    for (const char *ptr = strZ(value); *ptr != 0; ptr++)
    {
        if (*ptr == '\'' || *ptr == '\\')
            strCatChr(result, *ptr);
        strCatChr(result, *ptr);
    }
    return strCatChr(result, '\'');
}

FN_EXTERN String *
forkSourceId(
    const unsigned int repoIdx, const String *const stanza, const String *const archiveId, const uint64_t systemId)
{
    ASSERT(stanza != NULL && archiveId != NULL);
    String *result;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // Keep source identity stable across credential changes.
        const ConfigOption identityOption[] =
        {
            cfgOptRepoType, cfgOptRepoPath, cfgOptRepoHost, cfgOptRepoHostPort, cfgOptRepoHostUser, cfgOptRepoHostType,
            cfgOptRepoStorageHost, cfgOptRepoStoragePort,
            cfgOptRepoS3Bucket, cfgOptRepoS3Endpoint, cfgOptRepoS3Region,
            cfgOptRepoGcsBucket, cfgOptRepoGcsEndpoint,
            cfgOptRepoAzureAccount, cfgOptRepoAzureContainer, cfgOptRepoAzureEndpoint,
            cfgOptRepoSftpHost, cfgOptRepoSftpHostPort, cfgOptRepoSftpHostUser,
        };
        String *const identity = strCatFmt(strNew(), "%s\n%s\n%" PRIu64 "\n", strZ(stanza), strZ(archiveId), systemId);

        for (unsigned int optionIdx = 0; optionIdx < LENGTH_OF(identityOption); optionIdx++)
        {
            if (cfgOptionValid(identityOption[optionIdx]) && cfgOptionIdxTest(identityOption[optionIdx], repoIdx))
            {
                strCatFmt(
                    identity, "%s=%s\n", cfgParseOptionName(identityOption[optionIdx]),
                    strZ(cfgOptionIdxDisplay(identityOption[optionIdx], repoIdx)));
            }
        }

        MEM_CONTEXT_PRIOR_BEGIN()
        {
            result = strNewEncode(encodingHex, cryptoHashOne(hashTypeSha256, BUFSTR(identity)));
        }
        MEM_CONTEXT_PRIOR_END();
    }
    MEM_CONTEXT_TEMP_END();

    return result;
}

FN_EXTERN unsigned int
forkSourceRepoIdx(const InfoFork *const info)
{
    const InfoForkData *const data = infoForkData(info);

    // Match the source by identity so repository configuration keys can change.
    for (unsigned int repoIdx = 0; repoIdx < cfgOptionGroupIdxTotal(cfgOptGrpRepo); repoIdx++)
    {
        if (strEq(data->sourceId, forkSourceId(repoIdx, data->sourceStanza, data->sourceArchiveId, data->systemId)))
        {
            CHECK_FMT(
                OptionInvalidError, repoIsLocal(repoIdx),
                "fork source '%s' requires a source-authorized remote connection", strZ(data->sourceStanza));
            return repoIdx;
        }
    }

    THROW_FMT(
        OptionInvalidError, "repository for fork source '%s' is not configured (source id %s)", strZ(data->sourceStanza),
        strZ(data->sourceId));
}

static void
forkSourcePathValidate(const String *const path, const bool component)
{
    ASSERT(path != NULL);
    CHECK_FMT(
        FormatError,
        !strEmpty(path) && !strBeginsWithZ(path, "/") && !strEndsWithZ(path, "/") &&
        strstr(strZ(path), "//") == NULL && (!component || strchr(strZ(path), '/') == NULL),
        "invalid source %s '%s'", component ? "name" : "path", strZ(path));

    MEM_CONTEXT_TEMP_BEGIN()
    {
        const StringList *const partList = strLstNewSplitZ(path, "/");

        for (unsigned int partIdx = 0; partIdx < strLstSize(partList); partIdx++)
        {
            const String *const part = strLstGet(partList, partIdx);
            CHECK_FMT(FormatError, !strEqZ(part, ".") && !strEqZ(part, ".."), "invalid source path '%s'", strZ(path));
        }
    }
    MEM_CONTEXT_TEMP_END();
}

FN_EXTERN String *
forkSourcePath(const String *const stanza, const char *const type, const String *const path)
{
    ASSERT(type != NULL && (strcmp(type, STORAGE_PATH_ARCHIVE) == 0 || strcmp(type, STORAGE_PATH_BACKUP) == 0));
    forkSourcePathValidate(stanza, true);
    if (path != NULL)
        forkSourcePathValidate(path, false);
    return strNewFmt("%s/%s%s%s", type, strZ(stanza), path != NULL ? "/" : "", path != NULL ? strZ(path) : "");
}

FN_EXTERN Manifest *
forkSourceManifestLoad(const unsigned int repoIdx, const String *const stanza, const String *const backupLabel)
{
    Manifest *result;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        CHECK_FMT(
            FormatError, regExpMatchOne(backupRegExpP(.full = true, .differential = true, .incremental = true), backupLabel),
            "invalid source backup label '%s'", strZ(backupLabel));
        const Storage *const storage = storageRepoIdx(repoIdx);
        const InfoBackup *const backup = infoBackupLoadFile(
            storage, forkSourcePath(stanza, STORAGE_PATH_BACKUP, STRDEF(INFO_BACKUP_FILE)), cfgCipherSpecMainIdx(repoIdx));
        result = manifestMove(
            manifestLoadFile(
                storage, forkSourcePath(stanza, STORAGE_PATH_BACKUP, strNewFmt("%s/" BACKUP_MANIFEST_FILE, strZ(backupLabel))),
                infoBackupCipherSpec(backup)),
            memContextPrior());
    }
    MEM_CONTEXT_TEMP_END();

    return result;
}

FN_EXTERN InfoFork *
forkInfoLoad(const unsigned int repoIdx)
{
    const Storage *const storage = storageRepoIdx(repoIdx);

    if (!storageExistsP(storage, STRDEF(INFO_FORK_PATH_FILE)) &&
        !storageExistsP(storage, STRDEF(INFO_FORK_PATH_FILE INFO_COPY_EXT)))
        return NULL;

    InfoFork *const info = infoForkLoadFile(storage, STRDEF(INFO_FORK_PATH_FILE), cfgCipherSpecMainIdx(repoIdx));
    CHECK(
        OptionInvalidError,
        infoForkData(info)->state == infoForkStatePublished || cfgCommand() == cfgCmdFork || cfgCommand() == cfgCmdStanzaDelete,
        "fork metadata is incomplete; repeat the fork command before using this stanza");
    return info;
}

FN_EXTERN const Storage *
forkStorageRepoIdx(const unsigned int repoIdx)
{
    if (forkStorageContext == NULL)
    {
        MEM_CONTEXT_BEGIN(objMemContext(storageDriver(storageRepoIdx(repoIdx))))
        {
            MEM_CONTEXT_NEW_BEGIN(ForkStorage, .childQty = MEM_CONTEXT_QTY_MAX, .allocQty = MEM_CONTEXT_QTY_MAX, .callbackQty = 1)
            {
                forkStorageContext = MEM_CONTEXT_NEW();
                memContextCallbackSet(forkStorageContext, forkStorageReset, NULL);
                forkStorageCache = memNewPtrArray(cfgOptionGroupIdxTotal(cfgOptGrpRepo));
            }
            MEM_CONTEXT_NEW_END();
        }
        MEM_CONTEXT_END();
    }

    if (forkStorageCache[repoIdx] != NULL)
        return forkStorageCache[repoIdx];

    const Storage *result;
    MEM_CONTEXT_BEGIN(forkStorageContext)
    {
        const InfoFork *const info = forkInfoLoad(repoIdx);
        result = storageRepoIdx(repoIdx);

        if (info != NULL)
        {
            const InfoForkData *const data = infoForkData(info);
            result = storageForkNew(
                result, storageRepoIdx(forkSourceRepoIdx(info)), cfgOptionStr(cfgOptStanza), data->sourceStanza,
                data->importedBackupList);
        }

        forkStorageCache[repoIdx] = result;
    }
    MEM_CONTEXT_END();

    return result;
}

FN_EXTERN bool
forkPinExists(const unsigned int repoIdx)
{
    return !strLstEmpty(storageListP(storageRepoIdx(repoIdx), STRDEF(STORAGE_REPO_BACKUP "/" FORK_PIN_PATH)));
}

FN_EXTERN String *
forkPinName(const unsigned int repoIdx, const String *const stanza)
{
    return strNewFmt(
        "%s-%s.info", strZ(forkSourceId(repoIdx, stanza, STRDEF("fork"), 0)), strZ(stanza));
}
