/***********************************************************************************************************************************
Block Copy-on-Write

Clean blocks come from the cached backup file. Changed blocks use a sparse local file and an in-memory bitmap.
Backup bytes beyond baseLimit are no longer visible, even if the file grows again.
***********************************************************************************************************************************/
#include <build.h>

#include <fcntl.h>
#include <unistd.h>

#include "command/mount/cow.h"
#include "common/debug.h"

#define MOUNT_COW_BLOCK                                              4096

/***********************************************************************************************************************************
Close backing files without discarding the block map
***********************************************************************************************************************************/
FN_EXTERN void
mountCowClose(MountCow *const this)
{
    if (this->baseFd != -1)
        close(this->baseFd);
    if (this->cowFd != -1)
        close(this->cowFd);
    this->baseFd = this->cowFd = -1;
}

/***********************************************************************************************************************************
Close backing files and remove the COW file
***********************************************************************************************************************************/
static void
mountCowFreeResource(void *const data)
{
    MountCow *const this = data;
    mountCowClose(this);
    if (this->exists)
        unlink(strZ(this->path));
}

/***********************************************************************************************************************************
Create COW state without opening the backup or creating a local file
***********************************************************************************************************************************/
FN_EXTERN MountCow *
mountCowNew(const uint64_t size, const String *const path, MountCowOpen *const open, void *const openData)
{
    OBJ_NEW_BEGIN(MountCow, .childQty = MEM_CONTEXT_QTY_MAX, .callbackQty = 1)
    {
        *this = (MountCow)
        {
            .size = size, .baseLimit = size, .path = strDup(path), .dirty = bufNew(0),
            .baseFd = -1, .cowFd = -1, .open = open, .openData = openData,
        };
        memContextCallbackSet(objMemContext(this), mountCowFreeResource, this);
    }
    OBJ_NEW_END();
    return this;
}

/***********************************************************************************************************************************
Check whether a block must be read from COW storage
***********************************************************************************************************************************/
static bool
mountCowDirty(const MountCow *const this, const uint64_t block)
{
    // Blocks beyond the retained backup range use COW data or read as zeros
    return block * MOUNT_COW_BLOCK >= this->baseLimit ||
           (block / 8 < bufSize(this->dirty) && (bufPtrConst(this->dirty)[block / 8] & (1U << (block % 8))) != 0);
}

/***********************************************************************************************************************************
Record a changed backup block in the bitmap
***********************************************************************************************************************************/
static void
mountCowMark(MountCow *const this, const uint64_t block)
{
    // Only blocks that can still contain backup data need a bitmap entry
    if (block * MOUNT_COW_BLOCK < this->baseLimit)
    {
        const size_t oldSize = bufSize(this->dirty);
        const size_t newSize = (size_t)(block / 8 + 1);
        if (newSize > oldSize)
        {
            bufResize(this->dirty, newSize);
            memset(bufPtr(this->dirty) + oldSize, 0, newSize - oldSize);
        }
        bufPtr(this->dirty)[block / 8] |= (uint8_t)(1U << (block % 8));
    }
}

/***********************************************************************************************************************************
Create or reopen the local COW file
***********************************************************************************************************************************/
static int
mountCowFd(MountCow *const this)
{
    if (this->cowFd == -1)
    {
        CHECK(FileWriteError, this->path != NULL, "mount is read-only");

        // O_EXCL prevents reuse of a file that this COW object did not create
        this->cowFd = open(strZ(this->path), O_RDWR | O_CLOEXEC | O_NOFOLLOW | (this->exists ? 0 : O_CREAT | O_EXCL), 0600);
        THROW_ON_SYS_ERROR(this->cowFd == -1, FileOpenError, "unable to open COW file");
        this->exists = true;
    }
    return this->cowFd;
}

/***********************************************************************************************************************************
Read a range, retrying interrupted and partial reads
***********************************************************************************************************************************/
static void
mountCowReadFd(const int fd, void *const buffer, const size_t size, const uint64_t offset, const bool zeroEof)
{
    size_t done = 0;
    while (done < size)
    {
        const ssize_t actual = pread(fd, (uint8_t *)buffer + done, size - done, (off_t)(offset + done));
        if (actual < 0 && errno == EINTR)
            continue;
        THROW_ON_SYS_ERROR(actual < 0, FileReadError, "unable to read mount file");

        // A short COW file has unwritten zeros; a short backup file is an error
        if (actual == 0)
        {
            CHECK(FileReadError, zeroEof, "unexpected end of cached backup file");
            memset((uint8_t *)buffer + done, 0, size - done);
            break;
        }
        done += (size_t)actual;
    }
}

/***********************************************************************************************************************************
Read backup bytes and zero any bytes beyond the retained backup range
***********************************************************************************************************************************/
static void
mountCowBaseRead(MountCow *const this, void *const buffer, const size_t size, const uint64_t offset)
{
    memset(buffer, 0, size);
    if (offset < this->baseLimit)
    {
        // Restore the backup file only when unchanged bytes are first read
        if (this->baseFd == -1)
        {
            ASSERT(this->open != NULL);
            this->baseFd = this->open(this->openData);
        }
        const size_t available = (size_t)((uint64_t)size < this->baseLimit - offset ? size : this->baseLimit - offset);
        mountCowReadFd(this->baseFd, buffer, available, offset, false);
    }
}

/***********************************************************************************************************************************
Read a file range from backup and COW storage
***********************************************************************************************************************************/
FN_EXTERN size_t
mountCowRead(MountCow *const this, void *const buffer, const size_t size, const uint64_t offset)
{
    if (offset >= this->size)
        return 0;
    const size_t result = (size_t)((uint64_t)size < this->size - offset ? (uint64_t)size : this->size - offset);

    // Split reads at block boundaries so each part uses the correct source
    for (size_t done = 0; done < result;)
    {
        const uint64_t pos = offset + done;
        const size_t part = (result - done < MOUNT_COW_BLOCK - (size_t)(pos % MOUNT_COW_BLOCK) ?
                                 result - done : MOUNT_COW_BLOCK - (size_t)(pos % MOUNT_COW_BLOCK));
        void *const output = (uint8_t *)buffer + done;
        if (mountCowDirty(this, pos / MOUNT_COW_BLOCK))
        {
            if (this->exists)
                mountCowReadFd(mountCowFd(this), output, part, pos, true);
            else
                memset(output, 0, part);
        }
        else
            mountCowBaseRead(this, output, part, pos);
        done += part;
    }
    return result;
}

/***********************************************************************************************************************************
Write a range, retrying interrupted and partial writes
***********************************************************************************************************************************/
static void
mountCowWriteFd(const int fd, const void *const buffer, const size_t size, const uint64_t offset)
{
    size_t done = 0;
    while (done < size)
    {
        const ssize_t actual = pwrite(fd, (const uint8_t *)buffer + done, size - done, (off_t)(offset + done));
        if (actual < 0 && errno == EINTR)
            continue;
        THROW_ON_SYS_ERROR(actual < 0, FileWriteError, "unable to write COW file");
        CHECK(FileWriteError, actual != 0, "zero-length write to COW file");
        done += (size_t)actual;
    }
}

/***********************************************************************************************************************************
Write changed blocks to local COW storage
***********************************************************************************************************************************/
FN_EXTERN void
mountCowWrite(MountCow *const this, const void *const buffer, const size_t size, const uint64_t offset)
{
    CHECK(FileWriteError, offset <= INT64_MAX && size <= (uint64_t)INT64_MAX - offset, "COW write offset overflow");
    for (size_t done = 0; done < size;)
    {
        const uint64_t pos = offset + done;
        const uint64_t block = pos / MOUNT_COW_BLOCK;
        const size_t inBlock = (size_t)(pos % MOUNT_COW_BLOCK);
        const size_t part = (size - done < MOUNT_COW_BLOCK - inBlock ? size - done : MOUNT_COW_BLOCK - inBlock);
        const uint8_t *const input = (const uint8_t *)buffer + done;

        // Preserve the remaining backup bytes when a clean block is only partly overwritten
        if (part != MOUNT_COW_BLOCK && !mountCowDirty(this, block))
        {
            uint8_t copy[MOUNT_COW_BLOCK];
            mountCowBaseRead(this, copy, sizeof(copy), block * MOUNT_COW_BLOCK);
            memcpy(copy + inBlock, input, part);
            mountCowWriteFd(mountCowFd(this), copy, sizeof(copy), block * MOUNT_COW_BLOCK);
        }
        // Full block writes and writes to changed blocks do not need backup reads
        else
            mountCowWriteFd(mountCowFd(this), input, part, pos);

        // A failed write must not mark a clean block as changed.
        mountCowMark(this, block);
        done += part;
        this->size = (this->size > offset + done ? this->size : offset + done);
    }
}

/***********************************************************************************************************************************
Change the visible file size and discard bytes beyond the new end
***********************************************************************************************************************************/
FN_EXTERN void
mountCowTruncate(MountCow *const this, const uint64_t size)
{
    CHECK(FileWriteError, size <= INT64_MAX, "COW size overflow");
    if (size < this->size)
    {
        if (this->exists)
            THROW_ON_SYS_ERROR(ftruncate(mountCowFd(this), (off_t)size) == -1, FileWriteError, "unable to truncate COW file");

        // Growing the file must not expose bytes removed by truncation.
        this->baseLimit = (this->baseLimit < size ? this->baseLimit : size);
    }
    this->size = size;
}

/***********************************************************************************************************************************
Reserve local disk space without changing the block sources
***********************************************************************************************************************************/
FN_EXTERN void
mountCowAllocate(MountCow *const this, const uint64_t offset, const uint64_t size, const bool keepSize)
{
#ifdef __linux__
    CHECK(FileWriteError, offset <= INT64_MAX && size <= (uint64_t)INT64_MAX - offset, "COW allocation offset overflow");

    // Reserve space without marking clean backup blocks as changed
    const int result = posix_fallocate(mountCowFd(this), (off_t)offset, (off_t)size);
    if (result != 0)
    {
        errno = result;
        THROW_SYS_ERROR(FileWriteError, "unable to allocate COW space");
    }

    // The physical COW file can grow while the visible file size stays unchanged
    if (!keepSize && offset + size > this->size)
        this->size = offset + size;
#else
    (void)this;
    (void)offset;
    (void)size;
    (void)keepSize;
    THROW(FeatureNotSupportedError, "COW allocation requires Linux");
#endif
}
