/***********************************************************************************************************************************
Backup File Cache
***********************************************************************************************************************************/
#include <build.h>

#include <fcntl.h>
#include <unistd.h>

#include "command/mount/cache.h"
#include "command/restore/file.h"
#include "common/debug.h"
#include "common/io/filter/filter.intern.h"
#include "common/io/io.h"
#include "common/stat.h"

/***********************************************************************************************************************************
Open a cached file, or restore it if the cached copy is missing or invalid
***********************************************************************************************************************************/
FN_EXTERN int
mountCacheOpen(void *const data)
{
    MountCacheFile *const source = data;
    const MountCache *const cache = source->cache;
    int volatile result = -1;

    TRY_BEGIN()
    {
        MEM_CONTEXT_TEMP_BEGIN()
        {
            const ManifestFile file = manifestFile(cache->manifest, source->fileIdx);

            // Use checksum and size so identical files can share a cache entry
            const String *const name = strNewFmt(
                "%s-%" PRIu64, strZ(strNewEncode(encodingHex, BUF(file.checksumSha1, HASH_TYPE_SHA1_SIZE))), file.size);
            const String *const path = storagePathP(cache->storage, name);
            result = open(strZ(path), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

            // Check an existing cache entry before reuse
            if (result != -1)
            {
                struct stat info;
                THROW_ON_SYS_ERROR(fstat(result, &info) == -1, FileInfoError, "unable to read cached file metadata");
                bool valid = S_ISREG(info.st_mode) && (uint64_t)info.st_size == file.size;

                // Hash the open descriptor so verification and reads use the same file
                if (valid && !source->verified)
                {
                    IoFilter *const hash = cryptoHashNew(hashTypeSha1);
                    uint8_t buffer[65536];
                    ssize_t actual;
                    while ((actual = read(result, buffer, sizeof(buffer))) != 0)
                    {
                        if (actual < 0 && errno == EINTR)
                            continue;
                        THROW_ON_SYS_ERROR(actual < 0, FileReadError, "unable to verify cached file");
                        ioFilterProcessIn(hash, BUF(buffer, (size_t)actual));
                    }
                    valid = bufEq(
                        BUF(file.checksumSha1, HASH_TYPE_SHA1_SIZE), pckReadBinP(pckReadNew(ioFilterResult(hash))));
                }
                if (!valid)
                {
                    close(result);
                    result = -1;
                }
            }
            else
                THROW_ON_SYS_ERROR(errno != ENOENT, FileOpenError, "unable to open cached file");

            // Restore missing or invalid data to a temporary file
            if (result == -1)
            {
                const String *const temporary = strNewFmt("%s.tmp.%d", strZ(name), (int)getpid());
                TRY_BEGIN()
                {
                    statInc(STRDEF("mount.cache.miss"));
                    restoreFileTo(cache->repoIdx, cache->manifest, &file, cache->storage, temporary);

                    // Replace the cache entry only after restore and checksum verification succeed
                    const String *const temporaryPath = storagePathP(cache->storage, temporary);
                    THROW_ON_SYS_ERROR(
                        chmod(strZ(temporaryPath), 0400) == -1, FileModeError, "unable to set cached file permissions");
                    THROW_ON_SYS_ERROR(
                        rename(strZ(temporaryPath), strZ(path)) == -1, FileMoveError, "unable to rename cached file");
                }
                FINALLY()
                {
                    // Remove incomplete output if restore or rename fails
                    storageRemoveP(cache->storage, temporary);
                }
                TRY_END();
                result = open(strZ(path), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
                THROW_ON_SYS_ERROR(result == -1, FileOpenError, "unable to open restored file");
            }
            else
                statInc(STRDEF("mount.cache.hit"));
            source->verified = true;
        }
        MEM_CONTEXT_TEMP_END();
    }
    CATCH_ANY()
    {
        if (result != -1)
            close(result);
        RETHROW();
    }
    TRY_END();
    return result;
}
