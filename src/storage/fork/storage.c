/***********************************************************************************************************************************
Read-only Repository View for Inherited Backup Files
***********************************************************************************************************************************/
#include <build.h>

#include <string.h>

#include "common/debug.h"
#include "info/manifest/manifest.h"
#include "storage/fork/storage.h"

typedef struct StorageFork
{
    STORAGE_COMMON_MEMBER;
    const Storage *own;
    const Storage *source;
    const String *ownPrefix;
    const String *sourcePrefix;
    const StringList *backupList;
} StorageFork;

// Copied manifests use the fork repository's key. Inherited backup files use the source repository's key.
static const Storage *
storageForkResolve(const StorageFork *const this, const String **const file)
{
    ASSERT(this != NULL);
    ASSERT(file != NULL && *file != NULL);

    if (strBeginsWith(*file, this->ownPrefix))
    {
        const char *const relative = strZ(*file) + strSize(this->ownPrefix);
        const char *const separator = strchr(relative, '/');

        if (separator != NULL && strLstExists(this->backupList, strNewZN(relative, (size_t)(separator - relative))) &&
            strcmp(separator + 1, BACKUP_MANIFEST_FILE) != 0 &&
            strcmp(separator + 1, BACKUP_MANIFEST_FILE INFO_COPY_EXT) != 0)
        {
            *file = strNewFmt("%s%s", strZ(this->sourcePrefix), relative);
            return this->source;
        }
    }

    return this->own;
}

static StorageInfo
storageForkInfo(THIS_VOID, const String *file, const StorageInfoLevel level, const StorageInterfaceInfoParam param)
{
    THIS(StorageFork);
    const Storage *const source = storageForkResolve(this, &file);
    return storageInterface(source).info(storageDriver(source), file, level, param);
}

static StorageList *
storageForkList(THIS_VOID, const String *const path, const StorageInfoLevel level, const StorageInterfaceListParam param)
{
    THIS(StorageFork);
    return storageInterface(this->own).list(storageDriver(this->own), path, level, param);
}

static void *
storageForkNewRead(THIS_VOID, const String *file, const StorageInterfaceNewReadParam param)
{
    THIS(StorageFork);
    const Storage *const source = storageForkResolve(this, &file);
    return storageInterface(source).newRead(storageDriver(source), file, param);
}

static void *
storageForkNewWrite(THIS_VOID, const String *const file, const StorageInterfaceNewWriteParam param)
{
    (void)thisVoid;
    (void)file;
    (void)param;
    THROW(AssertError, "inherited backup storage is read-only");
}

static bool
storageForkPathRemove(THIS_VOID, const String *const path, const bool recurse, const StorageInterfacePathRemoveParam param)
{
    (void)thisVoid;
    (void)path;
    (void)recurse;
    (void)param;
    THROW(AssertError, "inherited backup storage is read-only");
}

static void
storageForkRemove(THIS_VOID, const String *const file, const StorageInterfaceRemoveParam param)
{
    (void)thisVoid;
    (void)file;
    (void)param;
    THROW(AssertError, "inherited backup storage is read-only");
}

FN_EXTERN Storage *
storageForkNew(
    const Storage *const own, const Storage *const source, const String *const ownStanza, const String *const sourceStanza,
    const StringList *const backupList)
{
    ASSERT(own != NULL && source != NULL);
    ASSERT(ownStanza != NULL && sourceStanza != NULL && backupList != NULL);

    OBJ_NEW_BEGIN(StorageFork, .childQty = MEM_CONTEXT_QTY_MAX)
    {
        *this = (StorageFork)
        {
            .interface =
            {
                .feature = storageInterface(own).feature &
                           ~((uint64_t)1 << storageFeatureHardLink | (uint64_t)1 << storageFeatureSymLink |
                             (uint64_t)1 << storageFeaturePathSync),
                .prefetch = storageInterface(source).prefetch,
                .readOver = storageInterface(source).readOver,
                .info = storageForkInfo,
                .list = storageForkList,
                .newRead = storageForkNewRead,
                .newWrite = storageForkNewWrite,
                .pathRemove = storageForkPathRemove,
                .remove = storageForkRemove,
            },
            .own = own,
            .source = source,
            .ownPrefix = storagePathP(own, strNewFmt("backup/%s/", strZ(ownStanza))),
            .sourcePrefix = storagePathP(source, strNewFmt("backup/%s/", strZ(sourceStanza))),
            .backupList = strLstDup(backupList),
        };
    }
    OBJ_NEW_END();

    return storageNew(
        STRID5("fork", 0x5c9e60), storagePathP(own, NULL), STORAGE_MODE_FILE_DEFAULT, STORAGE_MODE_PATH_DEFAULT, false,
        storageTargetTime(own), storagePathExpression(own), this, this->interface);
}
