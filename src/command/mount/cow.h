/***********************************************************************************************************************************
Block Copy-on-Write
***********************************************************************************************************************************/
#ifndef COMMAND_MOUNT_COW_H
#define COMMAND_MOUNT_COW_H

#include "common/type/buffer.h"

// Open the cached backup file. The COW object owns the returned descriptor.
typedef int MountCowOpen(void *data);

/***********************************************************************************************************************************
Object type
***********************************************************************************************************************************/
typedef struct MountCow
{
    uint64_t size;                                                  // Current file size in bytes
    uint64_t baseLimit;                                             // End of backup data still visible after truncation
    Buffer *dirty;                                                  // One bit per changed backup block
    const String *path;                                             // Local COW path, or NULL for a read-only mount
    int baseFd;                                                     // Cached backup descriptor, or -1 when closed
    int cowFd;                                                      // Local COW descriptor, or -1 when closed
    bool exists;                                                    // COW file has been created and must be removed on free
    MountCowOpen *open;                                             // Open the cached backup file on first read
    void *openData;                                                 // Argument for the backup open callback
} MountCow;

/***********************************************************************************************************************************
Constructors
***********************************************************************************************************************************/
FN_EXTERN MountCow *mountCowNew(uint64_t size, const String *path, MountCowOpen *open, void *openData);

/***********************************************************************************************************************************
Functions
***********************************************************************************************************************************/
// Read from backup or COW storage, stopping at the current file size
FN_EXTERN size_t mountCowRead(MountCow *this, void *buffer, size_t size, uint64_t offset);

// Write local blocks and extend the visible file size when needed
FN_EXTERN void mountCowWrite(MountCow *this, const void *buffer, size_t size, uint64_t offset);

// Resize the file; removed backup bytes stay hidden if the file grows again
FN_EXTERN void mountCowTruncate(MountCow *this, uint64_t size);

// Reserve disk space, optionally keeping the visible file size unchanged
FN_EXTERN void mountCowAllocate(MountCow *this, uint64_t offset, uint64_t size, bool keepSize);

// Close descriptors while keeping COW data and the block map for later reads
FN_EXTERN void mountCowClose(MountCow *this);

#endif
