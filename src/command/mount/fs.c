/***********************************************************************************************************************************
FUSE Backup Mount
***********************************************************************************************************************************/
#include <build.h>

#include "command/mount/fs.h"
#include "common/debug.h"

#ifdef HAVE_LIBFUSE3
#define FUSE_USE_VERSION 35
#include <dirent.h>
#include <fcntl.h>
#include <fuse_lowlevel.h>
#include <linux/falloc.h>
#include <linux/fs.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "command/mount/cache.h"
#include "command/mount/cow.h"
#include "command/restore/restore.h"
#include "common/log.h"
#include "common/user.h"
#include "config/config.h"
#include "protocol/helper.h"
#include "storage/cifs/storage.h"
#include "storage/helper.h"
#include "storage/posix/storage.h"

#define MOUNT_TABLESPACE                                             ".pgbackrest-tablespaces"

/***********************************************************************************************************************************
Directory entries and mount state
***********************************************************************************************************************************/
typedef struct MountChild
{
    String *name;                                                   // Entry name owned by the parent directory
    fuse_ino_t ino;                                                 // Inode number of the child
    mode_t mode;                                                    // File type and mode for directory replies
} MountChild;

typedef struct MountNode
{
    fuse_ino_t ino;                                                 // Inode number, unchanged by rename
    fuse_ino_t parent;                                              // Parent directory inode
    mode_t mode;                                                    // File type and permissions
    time_t mtime;                                                   // Current modification time
    uint64_t size;                                                  // Initial file size; use cow->size after COW state exists
    uint64_t lookups;                                               // Lookup references held by the kernel
    unsigned int opens;                                             // Open file or directory handles
    bool removed;                                                   // Directory entry has been removed
    List *children;                                                 // Sorted directory entries, or NULL for other node types
    const String *link;                                             // Target of a tablespace symlink
    MountCacheFile source;                                          // Backup source; fileIdx is UINT_MAX for new files
    MountCow *cow;                                                  // COW state allocated on first use
} MountNode;

typedef struct MountFs
{
    MountCache cache;                                               // Shared cache for the selected backup
    const String *cowPath;                                          // COW directory, or NULL for a read-only mount
    List *nodes;                                                    // Inode slots; freed slots remain NULL and are not reused
    uid_t uid;                                                      // Owner of all mounted files
    gid_t gid;                                                      // Group of all mounted files
    int error;                                                      // POSIX error for the current FUSE request
} MountFs;

/***********************************************************************************************************************************
Request error handling

Catch pgBackRest errors inside each callback so longjmp cannot cross the libfuse dispatcher.
Each request uses a temporary memory context and returns a POSIX error to the kernel on failure.
***********************************************************************************************************************************/
#define MOUNT_REQUEST_BEGIN()                                                                                                      \
    MountFs *const this = fuse_req_userdata(req);                                                                                \
    this->error = 0;                                                                                                             \
    errno = 0;                                                                                                                   \
    TRY_BEGIN()                                                                                                                  \
    {                                                                                                                            \
        MEM_CONTEXT_TEMP_BEGIN()                                                                                                 \
        {

#define MOUNT_REQUEST_END()                                                                                                        \
        }                                                                                                                        \
        MEM_CONTEXT_TEMP_END();                                                                                                  \
    }                                                                                                                            \
    CATCH_ANY()                                                                                                                  \
    {                                                                                                                            \
        int requestError = this->error;                                                                                          \
        if (requestError == 0)                                                                                                   \
        {                                                                                                                        \
            requestError = errno == ENOSPC || errno == EDQUOT || errno == EMFILE ? errno : EIO;                                  \
            LOG_WARN_FMT("mount request: [%s] %s", errorName(), errorMessage());                                                 \
        }                                                                                                                        \
        fuse_reply_err(req, requestError);                                                                                       \
    }                                                                                                                            \
    TRY_END()

/***********************************************************************************************************************************
Set the POSIX error for a failed request
***********************************************************************************************************************************/
static void
mountFsCheck(MountFs *const this, const bool condition, const int error)
{
    if (!condition)
    {
        this->error = error;
        THROW_SYS_ERROR_CODE(error, FileOpenError, "mount operation failed");
    }
}

/***********************************************************************************************************************************
Find an inode and reject slots for deleted nodes
***********************************************************************************************************************************/
static MountNode *
mountFsNode(MountFs *const this, const fuse_ino_t ino)
{
    mountFsCheck(this, ino > 0 && ino <= lstSize(this->nodes), ENOENT);
    MountNode *const result = *(MountNode **)lstGet(this->nodes, (unsigned int)(ino - 1));
    mountFsCheck(this, result != NULL, ENOENT);
    return result;
}

/***********************************************************************************************************************************
Find a directory entry by name
***********************************************************************************************************************************/
static MountNode *
mountFsChild(MountFs *const this, MountNode *const parent, const char *const name)
{
    mountFsCheck(this, S_ISDIR(parent->mode), ENOTDIR);
    const String *const key = STR(name);
    const MountChild *const child = lstFind(parent->children, &key);
    return child == NULL ? NULL : mountFsNode(this, child->ino);
}

/***********************************************************************************************************************************
Allocate a node with a new inode number
***********************************************************************************************************************************/
static MountNode *
mountFsNodeNew(MountFs *const fs, const fuse_ino_t parent, const mode_t mode)
{
    MountNode *result;
    MEM_CONTEXT_BEGIN(objMemContext(fs))
    {
        // Never reuse inode numbers while the mount is active
        OBJ_NEW_BEGIN(MountNode, .childQty = MEM_CONTEXT_QTY_MAX)
        {
            *this = (MountNode)
            {
                .ino = lstSize(fs->nodes) + 1, .parent = parent, .mode = mode, .mtime = time(NULL),
                .source = {.cache = &fs->cache, .fileIdx = UINT_MAX},
                .children = S_ISDIR(mode) ? lstNewP(sizeof(MountChild), .comparator = lstComparatorStr) : NULL,
            };
        }
        OBJ_NEW_END();
        result = this;
        lstAdd(fs->nodes, &result);
    }
    MEM_CONTEXT_END();
    return result;
}

/***********************************************************************************************************************************
Add a directory entry and update the parent inode
***********************************************************************************************************************************/
static void
mountFsAttach(MountNode *const parent, MountNode *const node, const char *const name)
{
    MEM_CONTEXT_BEGIN(objMemContext(parent))
    {
        lstAdd(parent->children, &(MountChild){.name = strNewZ(name), .ino = node->ino, .mode = node->mode});
        lstSort(parent->children, sortOrderAsc);
    }
    MEM_CONTEXT_END();
    parent->mtime = time(NULL);
    node->parent = parent->ino;
}

/***********************************************************************************************************************************
Remove a directory entry without freeing its inode
***********************************************************************************************************************************/
static void
mountFsDetach(MountNode *const parent, const char *const name)
{
    const String *const key = STR(name);
    const MountChild *const child = lstFind(parent->children, &key);
    String *const oldName = child->name;
    lstRemove(parent->children, &key);
    strFree(oldName);
    parent->mtime = time(NULL);
}

/***********************************************************************************************************************************
Free a removed inode after all kernel references and open handles are released
***********************************************************************************************************************************/
static void
mountFsCollect(MountFs *const this, MountNode *const node)
{
    // Unlink does not invalidate open handles or kernel lookup references
    if (node->removed && node->opens == 0 && node->lookups == 0)
    {
        *(MountNode **)lstGet(this->nodes, (unsigned int)(node->ino - 1)) = NULL;
        objFree(node);
    }
}

/***********************************************************************************************************************************
Create COW state for a regular file on first use
***********************************************************************************************************************************/
static MountCow *
mountFsCow(MountFs *const this, MountNode *const node)
{
    mountFsCheck(this, S_ISREG(node->mode), EISDIR);
    if (node->cow == NULL)
    {
        MEM_CONTEXT_BEGIN(objMemContext(node))
        {
            node->cow = mountCowNew(
                node->size, this->cowPath == NULL ? NULL : strNewFmt("%s/%" PRIu64, strZ(this->cowPath), node->ino),
                node->source.fileIdx == UINT_MAX ? NULL : mountCacheOpen, &node->source);
        }
        MEM_CONTEXT_END();
    }
    return node->cow;
}

/***********************************************************************************************************************************
Build file attributes from the current mount state
***********************************************************************************************************************************/
static struct stat
mountFsAttr(const MountFs *const this, const MountNode *const node)
{
    const uint64_t size = node->cow != NULL ? node->cow->size : node->size;
    return (struct stat)
           {
               .st_ino = node->ino, .st_mode = node->mode, .st_uid = this->uid, .st_gid = this->gid,
               .st_nlink = node->removed ? 0 : (S_ISDIR(node->mode) ? 2 : 1), .st_size = (off_t)size,
               .st_blksize = 4096, .st_blocks = (blkcnt_t)((size + 511) / 512),
               .st_mtime = node->mtime, .st_atime = node->mtime, .st_ctime = node->mtime,
           };
}

/***********************************************************************************************************************************
Build a lookup reply and record the kernel reference
***********************************************************************************************************************************/
static struct fuse_entry_param
mountFsEntry(const MountFs *const this, MountNode *const node)
{
    node->lookups++;
    return (struct fuse_entry_param)
           {
               .ino = node->ino, .attr = mountFsAttr(this, node), .attr_timeout = 1, .entry_timeout = 1,
           };
}

/***********************************************************************************************************************************
Look up a directory entry
***********************************************************************************************************************************/
static void
mountFsLookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    MOUNT_REQUEST_BEGIN()
    {
        MountNode *const node = mountFsChild(this, mountFsNode(this, parent), name);
        mountFsCheck(this, node != NULL, ENOENT);
        const struct fuse_entry_param entry = mountFsEntry(this, node);
        if (fuse_reply_entry(req, &entry) != 0)
            node->lookups--;
    }
    MOUNT_REQUEST_END();
}

/***********************************************************************************************************************************
Release lookup references held by the kernel
***********************************************************************************************************************************/
static void
mountFsForget(fuse_req_t req, fuse_ino_t ino, uint64_t count)
{
    MountFs *const this = fuse_req_userdata(req);
    if (ino > 0 && ino <= lstSize(this->nodes))
    {
        MountNode *const node = *(MountNode **)lstGet(this->nodes, (unsigned int)(ino - 1));
        if (node != NULL)
        {
            node->lookups -= (node->lookups < count ? node->lookups : count);
            mountFsCollect(this, node);
        }
    }
    fuse_reply_none(req);
}

/***********************************************************************************************************************************
Return current file attributes
***********************************************************************************************************************************/
static void
mountFsGetAttr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *info)
{
    (void)info;
    MOUNT_REQUEST_BEGIN()
    {
        const struct stat attr = mountFsAttr(this, mountFsNode(this, ino));
        fuse_reply_attr(req, &attr, 1);
    }
    MOUNT_REQUEST_END();
}

/***********************************************************************************************************************************
Apply file size, permission, and timestamp changes
***********************************************************************************************************************************/
static void
mountFsSetAttr(fuse_req_t req, fuse_ino_t ino, struct stat *attr, int set, struct fuse_file_info *info)
{
    (void)info;
    MOUNT_REQUEST_BEGIN()
    {
        mountFsCheck(this, this->cowPath != NULL, EROFS);
        MountNode *const node = mountFsNode(this, ino);

        // All files retain the mounting user and group
        mountFsCheck(this, !(set & FUSE_SET_ATTR_UID) || attr->st_uid == this->uid, EPERM);
        mountFsCheck(this, !(set & FUSE_SET_ATTR_GID) || attr->st_gid == this->gid, EPERM);
        if (set & FUSE_SET_ATTR_SIZE)
        {
            mountFsCheck(this, attr->st_size >= 0, EINVAL);
            mountCowTruncate(mountFsCow(this, node), (uint64_t)attr->st_size);
            node->mtime = time(NULL);
        }
        if (set & FUSE_SET_ATTR_MODE)
            node->mode = (node->mode & S_IFMT) | (attr->st_mode & 0777);
        if (set & FUSE_SET_ATTR_MTIME)
            node->mtime = attr->st_mtime;
        if (set & FUSE_SET_ATTR_MTIME_NOW)
            node->mtime = time(NULL);
        const struct stat result = mountFsAttr(this, node);
        fuse_reply_attr(req, &result, 1);
    }
    MOUNT_REQUEST_END();
}

/***********************************************************************************************************************************
Create a file or directory in a writable mount
***********************************************************************************************************************************/
static MountNode *
mountFsCreateNode(MountFs *const this, fuse_ino_t parent, const char *name, mode_t mode)
{
    mountFsCheck(this, this->cowPath != NULL, EROFS);
    MountNode *const dir = mountFsNode(this, parent);
    mountFsCheck(this, !dir->removed, ENOENT);
    mountFsCheck(this, mountFsChild(this, dir, name) == NULL, EEXIST);
    MountNode *const node = mountFsNodeNew(this, parent, mode);
    mountFsAttach(dir, node, name);
    return node;
}

/***********************************************************************************************************************************
Create a regular file or directory with the requested permissions
***********************************************************************************************************************************/
static void
mountFsMakeNode(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode, dev_t device)
{
    (void)device;
    MOUNT_REQUEST_BEGIN()
    {
        mountFsCheck(this, S_ISREG(mode) || S_ISDIR(mode), EPERM);
        MountNode *const node = mountFsCreateNode(this, parent, name, mode & ~fuse_req_ctx(req)->umask);
        const struct fuse_entry_param entry = mountFsEntry(this, node);
        if (fuse_reply_entry(req, &entry) != 0)
            node->lookups--;
    }
    MOUNT_REQUEST_END();
}

/***********************************************************************************************************************************
Create a directory
***********************************************************************************************************************************/
static void
mountFsMkdir(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode)
{
    mountFsMakeNode(req, parent, name, S_IFDIR | mode, 0);
}

/***********************************************************************************************************************************
Create and open a regular file
***********************************************************************************************************************************/
static void
mountFsCreate(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode, struct fuse_file_info *info)
{
    MOUNT_REQUEST_BEGIN()
    {
        MountNode *const node = mountFsCreateNode(this, parent, name, S_IFREG | (mode & ~fuse_req_ctx(req)->umask));
        node->opens++;
        info->fh = node->ino;
        info->keep_cache = 1;
        const struct fuse_entry_param entry = mountFsEntry(this, node);
        if (fuse_reply_create(req, &entry, info) != 0)
        {
            node->opens--;
            node->lookups--;
        }
    }
    MOUNT_REQUEST_END();
}

/***********************************************************************************************************************************
Remove a file or an empty directory
***********************************************************************************************************************************/
static void
mountFsRemove(fuse_req_t req, fuse_ino_t parent, const char *name, bool directory)
{
    MOUNT_REQUEST_BEGIN()
    {
        mountFsCheck(this, this->cowPath != NULL, EROFS);
        MountNode *const dir = mountFsNode(this, parent);
        MountNode *const node = mountFsChild(this, dir, name);
        mountFsCheck(this, node != NULL, ENOENT);
        mountFsCheck(this, S_ISDIR(node->mode) == directory, directory ? ENOTDIR : EISDIR);
        mountFsCheck(this, !directory || lstEmpty(node->children), ENOTEMPTY);

        // Remove the name now, but keep the inode until its last reference is released
        mountFsDetach(dir, name);
        node->removed = true;
        mountFsCollect(this, node);
        fuse_reply_err(req, 0);
    }
    MOUNT_REQUEST_END();
}

/***********************************************************************************************************************************
Remove a file name
***********************************************************************************************************************************/
static void
mountFsUnlink(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    mountFsRemove(req, parent, name, false);
}

/***********************************************************************************************************************************
Remove an empty directory
***********************************************************************************************************************************/
static void
mountFsRmdir(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    mountFsRemove(req, parent, name, true);
}

/***********************************************************************************************************************************
Move a directory entry, replacing the destination when allowed
***********************************************************************************************************************************/
static void
mountFsRename(fuse_req_t req, fuse_ino_t parent, const char *name, fuse_ino_t newParent, const char *newName, unsigned int flags)
{
    MOUNT_REQUEST_BEGIN()
    {
        mountFsCheck(this, this->cowPath != NULL, EROFS);
        mountFsCheck(this, (flags & ~(unsigned int)RENAME_NOREPLACE) == 0, EINVAL);
        MountNode *const dir = mountFsNode(this, parent);
        MountNode *const newDir = mountFsNode(this, newParent);
        mountFsCheck(this, !newDir->removed, ENOENT);
        MountNode *const node = mountFsChild(this, dir, name);
        mountFsCheck(this, node != NULL, ENOENT);
        MountNode *const target = mountFsChild(this, newDir, newName);
        mountFsCheck(this, !(flags & RENAME_NOREPLACE) || target == NULL, EEXIST);

        // Renaming an entry to itself requires no changes
        if (node != target)
        {
            // A directory cannot be moved into itself or one of its descendants
            for (MountNode *ancestor = newDir; ancestor->ino != FUSE_ROOT_ID; ancestor = mountFsNode(this, ancestor->parent))
                mountFsCheck(this, ancestor != node, EINVAL);

            // A replaced inode can remain open after its name is removed
            if (target != NULL)
            {
                mountFsCheck(this, !S_ISDIR(node->mode) || S_ISDIR(target->mode), ENOTDIR);
                mountFsCheck(this, S_ISDIR(node->mode) || !S_ISDIR(target->mode), EISDIR);
                mountFsCheck(this, !S_ISDIR(target->mode) || lstEmpty(target->children), ENOTEMPTY);
                mountFsDetach(newDir, newName);
                target->removed = true;
                mountFsCollect(this, target);
            }
            mountFsAttach(newDir, node, newName);
            mountFsDetach(dir, name);
        }
        fuse_reply_err(req, 0);
    }
    MOUNT_REQUEST_END();
}

/***********************************************************************************************************************************
Open a regular file without reading its backup contents
***********************************************************************************************************************************/
static void
mountFsOpen(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *info)
{
    MOUNT_REQUEST_BEGIN()
    {
        MountNode *const node = mountFsNode(this, ino);
        mountFsCheck(this, S_ISREG(node->mode), EISDIR);
        mountFsCheck(this, this->cowPath != NULL || (info->flags & O_ACCMODE) == O_RDONLY, EROFS);
        if (info->flags & O_TRUNC)
        {
            mountFsCheck(this, this->cowPath != NULL, EROFS);
            mountCowTruncate(mountFsCow(this, node), 0);
        }
        node->opens++;
        info->fh = ino;
        info->keep_cache = 1;
        if (fuse_reply_open(req, info) != 0)
            node->opens--;
    }
    MOUNT_REQUEST_END();
}

/***********************************************************************************************************************************
Read file data through the COW layer
***********************************************************************************************************************************/
static void
mountFsRead(fuse_req_t req, fuse_ino_t ino, size_t size, off_t offset, struct fuse_file_info *info)
{
    (void)info;
    MOUNT_REQUEST_BEGIN()
    {
        mountFsCheck(this, offset >= 0, EINVAL);
        Buffer *const buffer = bufNew(size);
        const size_t actual = mountCowRead(mountFsCow(this, mountFsNode(this, ino)), bufPtr(buffer), size, (uint64_t)offset);
        fuse_reply_buf(req, (const char *)bufPtrConst(buffer), actual);
    }
    MOUNT_REQUEST_END();
}

/***********************************************************************************************************************************
Write file data to the COW layer
***********************************************************************************************************************************/
static void
mountFsWrite(fuse_req_t req, fuse_ino_t ino, const char *buffer, size_t size, off_t offset, struct fuse_file_info *info)
{
    MOUNT_REQUEST_BEGIN()
    {
        mountFsCheck(this, this->cowPath != NULL, EROFS);
        mountFsCheck(this, offset >= 0 && size <= (uint64_t)INT64_MAX - (uint64_t)offset, EFBIG);
        MountNode *const node = mountFsNode(this, ino);
        MountCow *const cow = mountFsCow(this, node);

        // Append uses the current COW size because requests run in one thread
        mountCowWrite(cow, buffer, size, (info->flags & O_APPEND) ? cow->size : (uint64_t)offset);
        node->mtime = time(NULL);
        fuse_reply_write(req, size);
    }
    MOUNT_REQUEST_END();
}

/***********************************************************************************************************************************
Release an open handle and close unused backing files
***********************************************************************************************************************************/
static void
mountFsRelease(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *info)
{
    (void)info;
    MOUNT_REQUEST_BEGIN()
    {
        MountNode *const node = mountFsNode(this, ino);
        mountFsCheck(this, node->opens > 0, EBADF);
        node->opens--;
        if (node->opens == 0 && node->cow != NULL)
            mountCowClose(node->cow);
        mountFsCollect(this, node);
        fuse_reply_err(req, 0);
    }
    MOUNT_REQUEST_END();
}

/***********************************************************************************************************************************
Flush COW file data to disk
***********************************************************************************************************************************/
static void
mountFsSync(fuse_req_t req, fuse_ino_t ino, int datasync, struct fuse_file_info *info)
{
    (void)datasync;
    (void)info;
    MOUNT_REQUEST_BEGIN()
    {
        const MountNode *const node = mountFsNode(this, ino);

        // fsync() does not save the file tree or block map.
        if (node->cow != NULL && node->cow->cowFd != -1)
            THROW_ON_SYS_ERROR(fsync(node->cow->cowFd) == -1, FileSyncError, "unable to sync COW file");
        fuse_reply_err(req, 0);
    }
    MOUNT_REQUEST_END();
}

/***********************************************************************************************************************************
Complete a close request; writes have already reached the local file
***********************************************************************************************************************************/
static void
mountFsFlush(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *info)
{
    (void)ino;
    (void)info;
    fuse_reply_err(req, 0);
}

/***********************************************************************************************************************************
Create a directory snapshot for an open handle
***********************************************************************************************************************************/
static void
mountFsOpenDir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *info)
{
    MOUNT_REQUEST_BEGIN()
    {
        MountNode *const node = mountFsNode(this, ino);
        mountFsCheck(this, S_ISDIR(node->mode), ENOTDIR);
        MEM_CONTEXT_BEGIN(objMemContext(this))
        {
            // Keep entry offsets stable if the directory changes between readdir requests
            List *const snapshot = lstNewP(sizeof(MountChild));
            MEM_CONTEXT_BEGIN(lstMemContext(snapshot))
            {
                lstAdd(snapshot, &(MountChild){.name = strNewZ("."), .ino = ino, .mode = S_IFDIR});
                lstAdd(snapshot, &(MountChild){.name = strNewZ(".."), .ino = node->parent, .mode = S_IFDIR});
                for (unsigned int idx = 0; idx < lstSize(node->children); idx++)
                {
                    MountChild child = *(MountChild *)lstGet(node->children, idx);
                    child.name = strDup(child.name);
                    lstAdd(snapshot, &child);
                }
            }
            MEM_CONTEXT_END();
            info->fh = (uint64_t)(uintptr_t)snapshot;
        }
        MEM_CONTEXT_END();
        node->opens++;
        if (fuse_reply_open(req, info) != 0)
        {
            node->opens--;
            lstFree((List *)(uintptr_t)info->fh);
        }
    }
    MOUNT_REQUEST_END();
}

/***********************************************************************************************************************************
Read directory entries from the handle snapshot
***********************************************************************************************************************************/
static void
mountFsReadDir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t offset, struct fuse_file_info *info)
{
    (void)ino;
    MOUNT_REQUEST_BEGIN()
    {
        mountFsCheck(this, offset >= 0, EINVAL);
        const List *const snapshot = (List *)(uintptr_t)info->fh;
        Buffer *const buffer = bufNew(size);
        size_t used = 0;

        // FUSE offsets are snapshot indices, not byte offsets
        for (uint64_t idx = (uint64_t)offset; idx < lstSize(snapshot); idx++)
        {
            const MountChild *const child = lstGet(snapshot, (unsigned int)idx);
            const struct stat attr = {.st_ino = child->ino, .st_mode = child->mode};
            const size_t length = fuse_add_direntry(
                req, (char *)bufPtr(buffer) + used, size - used, strZ(child->name), &attr, (off_t)(idx + 1));
            if (length > size - used)
                break;
            used += length;
        }
        fuse_reply_buf(req, (const char *)bufPtrConst(buffer), used);
    }
    MOUNT_REQUEST_END();
}

/***********************************************************************************************************************************
Free the directory snapshot and release the open handle
***********************************************************************************************************************************/
static void
mountFsReleaseDir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *info)
{
    lstFree((List *)(uintptr_t)info->fh);
    mountFsRelease(req, ino, info);
}

/***********************************************************************************************************************************
Return the target of a tablespace link
***********************************************************************************************************************************/
static void
mountFsReadLink(fuse_req_t req, fuse_ino_t ino)
{
    MOUNT_REQUEST_BEGIN()
    {
        const MountNode *const node = mountFsNode(this, ino);
        mountFsCheck(this, node->link != NULL, EINVAL);
        fuse_reply_readlink(req, strZ(node->link));
    }
    MOUNT_REQUEST_END();
}

/***********************************************************************************************************************************
Report free space on the local cache or COW filesystem
***********************************************************************************************************************************/
static void
mountFsStat(fuse_req_t req, fuse_ino_t ino)
{
    (void)ino;
    MOUNT_REQUEST_BEGIN()
    {
        struct statvfs stat;
        const String *const path = this->cowPath == NULL ? storagePathP(this->cache.storage, NULL) : this->cowPath;
        THROW_ON_SYS_ERROR(statvfs(strZ(path), &stat) == -1, FileInfoError, "unable to get mount free space");
        fuse_reply_statfs(req, &stat);
    }
    MOUNT_REQUEST_END();
}

/***********************************************************************************************************************************
Disable FUSE write modes that bypass COW state updates
***********************************************************************************************************************************/
static void
mountFsInit(void *data, struct fuse_conn_info *connection)
{
    (void)data;

    // Writeback can delay bitmap updates. Atomic O_TRUNC can bypass setattr.
    connection->want &= ~(unsigned int)(FUSE_CAP_WRITEBACK_CACHE | FUSE_CAP_ATOMIC_O_TRUNC);
}

/***********************************************************************************************************************************
Reserve COW disk space for a file range
***********************************************************************************************************************************/
static void
mountFsAllocate(fuse_req_t req, fuse_ino_t ino, int mode, off_t offset, off_t length, struct fuse_file_info *info)
{
    (void)info;
    MOUNT_REQUEST_BEGIN()
    {
        mountFsCheck(this, this->cowPath != NULL, EROFS);
        mountFsCheck(this, (mode & ~FALLOC_FL_KEEP_SIZE) == 0, EOPNOTSUPP);
        mountFsCheck(this, offset >= 0 && length > 0 && length <= INT64_MAX - offset, EINVAL);
        MountNode *const node = mountFsNode(this, ino);
        mountCowAllocate(mountFsCow(this, node), (uint64_t)offset, (uint64_t)length, (mode & FALLOC_FL_KEEP_SIZE) != 0);
        node->mtime = time(NULL);
        fuse_reply_err(req, 0);
    }
    MOUNT_REQUEST_END();
}

/***********************************************************************************************************************************
FUSE callbacks
***********************************************************************************************************************************/
static const struct fuse_lowlevel_ops mountFsOps =
{
    .init = mountFsInit, .lookup = mountFsLookup, .forget = mountFsForget, .getattr = mountFsGetAttr, .setattr = mountFsSetAttr,
    .mknod = mountFsMakeNode, .mkdir = mountFsMkdir, .create = mountFsCreate, .unlink = mountFsUnlink, .rmdir = mountFsRmdir,
    .rename = mountFsRename, .open = mountFsOpen, .read = mountFsRead, .write = mountFsWrite, .release = mountFsRelease,
    .flush = mountFsFlush, .fsync = mountFsSync, .opendir = mountFsOpenDir, .readdir = mountFsReadDir,
    .releasedir = mountFsReleaseDir, .fsyncdir = mountFsSync, .readlink = mountFsReadLink, .statfs = mountFsStat,
    .fallocate = mountFsAllocate,
};

/***********************************************************************************************************************************
Find or create directories for a validated manifest path
***********************************************************************************************************************************/
static MountNode *
mountFsPath(MountFs *const this, const String *const path)
{
    MountNode *node = mountFsNode(this, FUSE_ROOT_ID);
    const char *part = strZ(path);
    while (*part != 0)
    {
        const char *const end = strchr(part, '/');
        const String *const name = strNewZN(part, end == NULL ? strlen(part) : (size_t)(end - part));
        CHECK(FormatError, strSize(name) > 0 && strSize(name) <= 255 && !strEqZ(name, ".") && !strEqZ(name, ".."),
              "invalid manifest path");
        MountNode *child = mountFsChild(this, node, strZ(name));
        if (child == NULL)
        {
            child = mountFsNodeNew(this, node->ino, S_IFDIR | 0700);
            mountFsAttach(node, child, strZ(name));
        }
        node = child;
        CHECK(FormatError, S_ISDIR(node->mode), "manifest parent path is not a directory");
        if (end == NULL)
            break;
        part = end + 1;
    }
    return node;
}

/***********************************************************************************************************************************
Map a manifest name to a path inside the mount
***********************************************************************************************************************************/
static String *
mountFsManifestPath(const String *const name)
{
    // PGDATA is the mount root; reserve a separate subtree for tablespaces
    if (strEq(name, MANIFEST_TARGET_PGDATA_STR))
        return strNew();
    if (strBeginsWithZ(name, MANIFEST_TARGET_PGDATA "/"))
    {
        const String *const path = strSub(name, sizeof(MANIFEST_TARGET_PGDATA));
        CHECK(FormatError, !strEqZ(path, MOUNT_TABLESPACE) && !strBeginsWithZ(path, MOUNT_TABLESPACE "/"),
              "manifest uses reserved mount tablespace path");
        return strDup(path);
    }
    if (strEq(name, MANIFEST_TARGET_PGTBLSPC_STR))
        return strNewZ(MOUNT_TABLESPACE);
    CHECK(FormatError, strBeginsWithZ(name, MANIFEST_TARGET_PGTBLSPC "/"), "unsupported manifest target");
    return strNewFmt(MOUNT_TABLESPACE "/%s", strZ(name) + sizeof(MANIFEST_TARGET_PGTBLSPC));
}

/***********************************************************************************************************************************
Build the mount tree from manifest paths, files, and tablespace targets
***********************************************************************************************************************************/
static MountFs *
mountFsNew(const MountCache cache, const String *const cowPath)
{
    OBJ_NEW_BEGIN(MountFs, .childQty = MEM_CONTEXT_QTY_MAX)
    {
        *this = (MountFs){.cache = cache, .cowPath = strDup(cowPath), .nodes = lstNewP(sizeof(MountNode *)),
                          .uid = getuid(), .gid = getgid()};
    }
    OBJ_NEW_END();
    mountFsNodeNew(this, FUSE_ROOT_ID, S_IFDIR | 0700);
    const Manifest *const manifest = cache.manifest;
    MEM_CONTEXT_TEMP_BEGIN()
    {
        // Create directories before attaching files
        for (unsigned int idx = 0; idx < manifestPathTotal(manifest); idx++)
        {
            const ManifestPath *const path = manifestPath(manifest, idx);
            MountNode *const node = mountFsPath(this, mountFsManifestPath(path->name));
            node->mode = S_IFDIR | (path->mode & 0777);
        }

        // Keep manifest file indices so backup contents can be restored on first read
        for (unsigned int idx = 0; idx < manifestFileTotal(manifest); idx++)
        {
            const ManifestFile file = manifestFile(manifest, idx);

            // The original tablespace map would redirect PostgreSQL outside the mount
            if (strEqZ(file.name, MANIFEST_TARGET_PGDATA "/tablespace_map"))
                continue;
            const String *const path = mountFsManifestPath(file.name);
            MountNode *const parent = mountFsPath(this, strPath(path));
            const String *const name = strBase(path);
            CHECK(FormatError, strSize(name) > 0 && strSize(name) <= 255 && !strEqZ(name, ".") && !strEqZ(name, ".."),
                  "invalid manifest file name");
            CHECK(FormatError, mountFsChild(this, parent, strZ(name)) == NULL, "duplicate manifest file");
            CHECK(FormatError, file.size <= INT64_MAX, "manifest file size exceeds the supported limit");
            MountNode *const node = mountFsNodeNew(this, parent->ino, S_IFREG | (file.mode & 0777));
            node->source.fileIdx = idx;
            node->size = file.size;
            node->mtime = file.timestamp;
            mountFsAttach(parent, node, strZ(name));
        }

        // Point pg_tblspc links at the private tablespace subtree
        for (unsigned int idx = 0; idx < manifestTargetTotal(manifest); idx++)
        {
            const ManifestTarget *const target = manifestTarget(manifest, idx);
            if (target->tablespaceId != 0)
            {
                MountNode *const parent = mountFsPath(this, STRDEF("pg_tblspc"));
                const String *const name = strNewFmt("%u", target->tablespaceId);
                CHECK(FormatError, mountFsChild(this, parent, strZ(name)) == NULL, "duplicate tablespace link");
                MountNode *const node = mountFsNodeNew(this, parent->ino, S_IFLNK | 0777);
                MEM_CONTEXT_BEGIN(objMemContext(node))
                {
                    node->link = strNewFmt("../" MOUNT_TABLESPACE "/%u", target->tablespaceId);
                    node->size = strSize(node->link);
                }
                MEM_CONTEXT_END();
                mountFsAttach(parent, node, strZ(name));
            }
        }
    }
    MEM_CONTEXT_TEMP_END();
    return this;
}

/***********************************************************************************************************************************
Open a private work directory and lock it if used for COW
***********************************************************************************************************************************/
static int
mountFsDirectory(const String *const path, const bool empty)
{
    storagePathCreateP(storageLocalWrite(), path, .mode = 0700);
    const int fd = open(strZ(path), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    THROW_ON_SYS_ERROR(fd == -1, PathOpenError, "unable to open mount working directory");
    TRY_BEGIN()
    {
        struct stat info;
        THROW_ON_SYS_ERROR(fstat(fd, &info) == -1, PathOpenError, "unable to read mount working directory metadata");
        CHECK(PathOpenError, info.st_uid == getuid() && (info.st_mode & 0077) == 0,
              "mount working directories must belong to the current user and have mode 0700");

        // Lock before checking contents so two mounts cannot claim the same COW directory
        if (empty)
        {
            THROW_ON_SYS_ERROR(flock(fd, LOCK_EX | LOCK_NB) != 0, PathOpenError, "COW directory is already in use");
            CHECK(PathOpenError, strLstSize(storageListP(storageLocal(), path)) == 0, "COW directory must be empty");
        }
    }
    CATCH_ANY()
    {
        close(fd);
        RETHROW();
    }
    TRY_END();
    return fd;
}

/***********************************************************************************************************************************
Resolve existing path components before creating work directories
***********************************************************************************************************************************/
static String *
mountFsCanonical(const String *const path)
{
    // Resolve symlinks so path checks cannot miss overlapping directories
    char *const resolved = realpath(strZ(path), NULL);
    if (resolved != NULL)
    {
        String *const result = strNewZ(resolved);
        free(resolved);
        return result;
    }
    THROW_ON_SYS_ERROR(errno != ENOENT, PathOpenError, "unable to resolve mount path");

    // A new directory has no realpath yet; resolve its existing parents instead
    const String *const base = strBase(path);
    CHECK(PathOpenError, strSize(base) != 0 && !strEqZ(base, ".") && !strEqZ(base, ".."), "invalid mount path");
    return strNewFmt("%s/%s", strZ(mountFsCanonical(strPath(path))), strZ(base));
}

/***********************************************************************************************************************************
Check whether a path equals or is below another path
***********************************************************************************************************************************/
static bool
mountFsContains(const String *const parent, const String *const path)
{
    return strEqZ(parent, "/") || strEq(parent, path) || strBeginsWith(path, strNewFmt("%s/", strZ(parent)));
}

/***********************************************************************************************************************************
Mount a backup and serve FUSE requests
***********************************************************************************************************************************/
FN_EXTERN void
cmdMount(void)
{
    FUNCTION_LOG_VOID(logLevelDebug);
    CHECK(ParamRequiredError, strLstSize(cfgCommandParam()) == 1, "mount requires one mount point");
    userInit();
    umask(0077);

    // Validate paths before creating cache or COW directories
    const String *const mountpoint = mountFsCanonical(storagePathP(storageLocal(), strLstGet(cfgCommandParam(), 0)));
    const String *const cachePath = mountFsCanonical(cfgOptionStr(cfgOptMountCachePath));
    const String *const cowPath = cfgOptionTest(cfgOptMountCowPath) ? mountFsCanonical(cfgOptionStr(cfgOptMountCowPath)) : NULL;
    CHECK(OptionInvalidValueError, !mountFsContains(mountpoint, cachePath) && !mountFsContains(cachePath, mountpoint),
          "mount point and cache directory must not overlap");
    if (cowPath != NULL)
    {
        CHECK(OptionInvalidValueError,
              !mountFsContains(mountpoint, cowPath) && !mountFsContains(cowPath, mountpoint) &&
              !mountFsContains(cachePath, cowPath) && !mountFsContains(cowPath, cachePath),
              "mount point, cache and COW directories must not overlap");
    }
    const StorageInfo mountInfo = storageInfoP(storageLocal(), mountpoint);
    CHECK(PathOpenError, mountInfo.type == storageTypePath, "mount point must be a directory");
    CHECK(PathNotEmptyError, strLstSize(storageListP(storageLocal(), mountpoint)) == 0, "mount point must be empty");

    // Local work directories must not contain a repository or be placed inside one
    for (unsigned int repoIdx = 0; repoIdx < cfgOptionGroupIdxTotal(cfgOptGrpRepo); repoIdx++)
    {
        const StringId type = cfgOptionIdxStrId(cfgOptRepoType, repoIdx);
        if (!cfgOptionIdxTest(cfgOptRepoHost, repoIdx) && (type == STORAGE_POSIX_TYPE || type == STORAGE_CIFS_TYPE))
        {
            const String *const repoPath = mountFsCanonical(cfgOptionIdxStr(cfgOptRepoPath, repoIdx));
            CHECK(OptionInvalidValueError,
                  !mountFsContains(repoPath, mountpoint) && !mountFsContains(mountpoint, repoPath) &&
                  !mountFsContains(repoPath, cachePath) && !mountFsContains(cachePath, repoPath) &&
                  (cowPath == NULL || (!mountFsContains(repoPath, cowPath) && !mountFsContains(cowPath, repoPath))),
                  "mount working directories must not overlap a repository");
        }
    }

    // Repository helpers stay read-only even when the mount accepts writes
    storageHelperDryRunInit(true);

    // Treat an explicit latest value like the default during backup selection
    if (strEqZ(cfgOptionStr(cfgOptSet), "latest"))
        cfgOptionSet(cfgOptSet, cfgSourceDefault, VARSTRDEF("latest"));
    const RestoreBackupData backup = restoreBackupSet();
    Manifest *const manifest = manifestLoadFile(
        storageRepoIdx(backup.repoIdx), strNewFmt(STORAGE_REPO_BACKUP "/%s/" BACKUP_MANIFEST_FILE, strZ(backup.backupSet)),
        backup.cipherSpecManifest);
    manifestValidate(manifest, false);

    int volatile cacheFd = -1;
    int volatile cowFd = -1;
    MountFs *volatile fs = NULL;
    struct fuse_session *volatile session = NULL;
    struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
    const int signalList[] = {SIGHUP, SIGINT, SIGTERM};
    struct sigaction savedSignal[3];
    unsigned int volatile savedSignalTotal = 0;
    bool volatile mounted = false;
    bool volatile signals = false;
    TRY_BEGIN()
    {
        cacheFd = mountFsDirectory(cachePath, false);
        if (cowPath != NULL)
            cowFd = mountFsDirectory(cowPath, true);

        // Build inode state without reading backup file contents
        fs = mountFsNew((MountCache){.storage = storagePosixNewP(cachePath, .write = true),
                                     .manifest = manifest, .repoIdx = backup.repoIdx}, cowPath);

        // Let the kernel enforce file permissions for all mount users
        fuse_opt_add_arg(&args, "pgbackrest");
        fuse_opt_add_arg(&args, "-o");
        fuse_opt_add_arg(&args, cowPath == NULL ? "ro,default_permissions,fsname=pgbackrest" :
                             "rw,default_permissions,fsname=pgbackrest");
        if (cfgOptionBool(cfgOptMountAllowOther))
        {
            fuse_opt_add_arg(&args, "-o");
            fuse_opt_add_arg(&args, "allow_other");
        }
        session = fuse_session_new(&args, &mountFsOps, sizeof(mountFsOps), fs);
        CHECK(ExecuteError, session != NULL, "unable to create FUSE session");

        // Save pgBackRest handlers before resetting them; libfuse replaces only default handlers
        for (unsigned int idx = 0; idx < 3; idx++)
        {
            const struct sigaction action = {.sa_handler = SIG_DFL};
            THROW_ON_SYS_ERROR(sigaction(signalList[idx], &action, &savedSignal[idx]) == -1,
                               ExecuteError, "unable to save signal handler");
            savedSignalTotal++;
        }
        CHECK(ExecuteError, fuse_set_signal_handlers(session) == 0, "unable to install FUSE signal handlers");
        signals = true;
        CHECK(ExecuteError, fuse_session_mount(session, strZ(mountpoint)) == 0, "unable to mount backup");
        mounted = true;
        LOG_INFO_FMT(
            "mounted backup %s at %s (%s)", strZ(backup.backupSet), strZ(mountpoint), cowPath == NULL ? "read-only" : "COW");
        if (cowPath != NULL)
            LOG_WARN("Unmounting discards COW changes. Export needed data and stop PostgreSQL first.");

        // pgBackRest memory and error contexts are not thread-safe.
        struct fuse_buf buffer = {0};
        TRY_BEGIN()
        {
            while (!fuse_session_exited(session))
            {
                struct pollfd pollFd = {.fd = fuse_session_fd(session), .events = POLLIN};
                const int ready = poll(&pollFd, 1, 1000);
                if (ready < 0 && errno == EINTR)
                    continue;
                THROW_ON_SYS_ERROR(ready < 0, FileReadError, "unable to poll FUSE session");
                if (ready > 0)
                {
                    const int received = fuse_session_receive_buf(session, &buffer);
                    if (received == -EINTR)
                        continue;
                    if (received == 0 || received == -ENODEV)
                        break;
                    CHECK(ExecuteError, received > 0, "unable to receive FUSE request");
                    fuse_session_process_buf(session, &buffer);
                }

                // Keep remote repository connections alive when no FUSE requests arrive
                protocolKeepAlive();
            }
        }
        FINALLY()
        {
            free(buffer.mem);
        }
        TRY_END();
    }
    FINALLY()
    {
        // Detach FUSE before freeing inode state and removing COW files
        if (mounted)
            fuse_session_unmount(session);
        if (signals)
            fuse_remove_signal_handlers(session);
        if (session != NULL)
            fuse_session_destroy(session);
        for (unsigned int idx = 0; idx < savedSignalTotal; idx++)
            sigaction(signalList[idx], &savedSignal[idx], NULL);
        fuse_opt_free_args(&args);
        if (fs != NULL)
            objFree(fs);
        if (cowFd != -1)
            close(cowFd);
        if (cacheFd != -1)
            close(cacheFd);
    }
    TRY_END();
    FUNCTION_LOG_RETURN_VOID();
}

#else

/***********************************************************************************************************************************
Report missing FUSE support
***********************************************************************************************************************************/
FN_EXTERN void
cmdMount(void)
{
    THROW(FeatureNotSupportedError, "mount requires a Linux build with libfuse3");
}

#endif
