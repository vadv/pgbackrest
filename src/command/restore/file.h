/***********************************************************************************************************************************
Restore Individual Files
***********************************************************************************************************************************/
#ifndef COMMAND_RESTORE_FILE_H
#define COMMAND_RESTORE_FILE_H

#include "info/manifest/manifest.h"

/***********************************************************************************************************************************
Restore file types
***********************************************************************************************************************************/
typedef enum
{
    restoreResultPreserve,
    restoreResultZero,
    restoreResultCopy,
} RestoreResult;

typedef struct RestoreFile
{
    const String *name;                                             // File to restore
    const Buffer *checksum;                                         // Expected checksum
    uint64_t size;                                                  // Expected size
    time_t timeModified;                                            // Original modification time
    mode_t mode;                                                    // Original mode
    bool zero;                                                      // Should the file be zeroed?
    const String *user;                                             // Original user
    const String *group;                                            // Original group
    uint64_t offset;                                                // Offset into repo file where pg file is located
    const Variant *limit;                                           // Limit for read in the repo file
    uint64_t blockIncrMapSize;                                      // Block incremental map size (0 if not incremental)
    size_t blockIncrSize;                                           // Block incremental size (when map size > 0)
    size_t blockIncrChecksumSize;                                   // Checksum size (when map size > 0)
    const String *manifestFile;                                     // Manifest file
    const Buffer *blockChecksum;                                    // Checksums for block incremental restore, set in restoreFile()
} RestoreFile;

typedef struct RestoreFileResult
{
    const String *manifestFile;                                     // Manifest file
    RestoreResult result;                                           // Restore result (e.g. preserve, copy)
    uint64_t blockIncrDeltaSize;                                    // Size restored by block incremental delta
} RestoreFileResult;

/***********************************************************************************************************************************
Repository file location
***********************************************************************************************************************************/
typedef struct RestoreFileRepo
{
    const String *name;                                             // Repository path for the file or bundle
    uint64_t offset;                                                // Start of the file data or block map
    const Variant *limit;                                           // Bytes to read, or NULL for the whole file
} RestoreFileRepo;

/***********************************************************************************************************************************
Functions
***********************************************************************************************************************************/
// Locate a file or its block map in the repository
FN_EXTERN RestoreFileRepo restoreFileRepo(const Manifest *manifest, const ManifestFile *file);

// Restore files into the supplied local storage
FN_EXTERN List *restoreFile(
    const Storage *storage, const String *repoFile, unsigned int repoIdx, CompressType repoFileCompressType,
    time_t copyTimeBegin, bool delta, bool deltaForce, bool bundleRaw, const CipherSpec *cipherSpecBackup,
    const StringList *referenceList, List *fileList);

// Restore one manifest file to a local destination and check its size
FN_EXTERN void restoreFileTo(
    unsigned int repoIdx, const Manifest *manifest, const ManifestFile *file, const Storage *storage, const String *destination);

#endif
