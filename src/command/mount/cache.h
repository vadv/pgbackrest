/***********************************************************************************************************************************
Backup File Cache
***********************************************************************************************************************************/
#ifndef COMMAND_MOUNT_CACHE_H
#define COMMAND_MOUNT_CACHE_H

#include "info/manifest/manifest.h"

/***********************************************************************************************************************************
Cache state
***********************************************************************************************************************************/
typedef struct MountCache
{
    const Storage *storage;                                         // Writable local cache storage
    const Manifest *manifest;                                       // Manifest for the mounted backup
    unsigned int repoIdx;                                           // Repository index for backup reads
} MountCache;

typedef struct MountCacheFile
{
    const MountCache *cache;                                        // Cache shared by the mount
    unsigned int fileIdx;                                           // File index in the manifest
    bool verified;                                                  // Checksum verified for this mounted file
} MountCacheFile;

/***********************************************************************************************************************************
Functions
***********************************************************************************************************************************/
// Open the entry described by MountCacheFile. The caller must close the descriptor and use pread() for reads.
FN_EXTERN int mountCacheOpen(void *data);

#endif
