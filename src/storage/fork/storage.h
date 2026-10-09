/***********************************************************************************************************************************
Read-only Repository View for Inherited Backup Files
***********************************************************************************************************************************/
#ifndef STORAGE_FORK_STORAGE_H
#define STORAGE_FORK_STORAGE_H

#include "storage/storage.h"

// Read inherited backup files from the source repository and copied manifests from the fork repository.
// Keep both storage objects until the caller frees this read-only view.
FN_EXTERN Storage *storageForkNew(
    const Storage *own, const Storage *source, const String *ownStanza, const String *sourceStanza,
    const StringList *backupList);

#endif
