/***********************************************************************************************************************************
Test Backup Mount
***********************************************************************************************************************************/
#include <fcntl.h>
#include <unistd.h>

#include "command/mount/cow.h"
#include "harness/manifest.h"
#include "harness/storage.h"
#include "storage/posix/storage.h"

static unsigned int mountTestOpens;

static int
mountTestOpen(void *const data)
{
    mountTestOpens++;
    const int result = open(data, O_RDONLY);
    THROW_ON_SYS_ERROR(result == -1, FileOpenError, "unable to open test backup");
    return result;
}

static void
testRun(void)
{
    const Storage *const storage = storagePosixNewP(TEST_PATH_STR, .write = true);
    uint8_t original[16385];
    for (size_t idx = 0; idx < sizeof(original); idx++)
        original[idx] = (uint8_t)(idx % 251);
    char basePath[] = TEST_PATH "/base";

    // *****************************************************************************************************************************
    if (testBegin("write, truncate, reopen and delete files"))
    {
        HRN_STORAGE_PUT(storage, "base", BUF(original, sizeof(original)));
        MountCow *const cow = mountCowNew(sizeof(original), STRDEF(TEST_PATH "/cow"), mountTestOpen, basePath);
        uint8_t block[4096];
        memset(block, 0xab, sizeof(block));
        TEST_RESULT_VOID(mountCowWrite(cow, block, sizeof(block), 4096), "write a full block");
        TEST_RESULT_UINT(mountTestOpens, 0, "full block writes do not read the backup");
        uint8_t actual[32768];
        TEST_RESULT_UINT(mountCowRead(cow, actual, sizeof(block), 4096), sizeof(block), "read written block");
        TEST_RESULT_INT(memcmp(actual, block, sizeof(block)), 0, "block contents match");
        TEST_RESULT_UINT(mountTestOpens, 0, "reading a changed block does not read the backup");
        TEST_RESULT_VOID(mountCowWrite(cow, "abc", 3, 4095), "write across a boundary between clean and dirty blocks");
        uint8_t expected[32768] = {0};
        memcpy(expected, original, sizeof(original));
        memcpy(expected + 4096, block, sizeof(block));
        memcpy(expected + 4095, "abc", 3);
        TEST_RESULT_UINT(mountCowRead(cow, actual, sizeof(actual), 0), sizeof(original), "read combined file");
        TEST_RESULT_INT(memcmp(actual, expected, sizeof(original)), 0, "preserve surrounding bytes");
        TEST_RESULT_VOID(mountCowClose(cow), "close backing descriptors");
        TEST_RESULT_UINT(mountCowRead(cow, actual, sizeof(actual), 0), sizeof(original), "reopen backup and COW files");
        TEST_RESULT_INT(memcmp(actual, expected, sizeof(original)), 0, "the bitmap is unchanged after reopening");
        TEST_RESULT_VOID(mountCowTruncate(cow, 4100), "truncate in a dirty block");
        TEST_RESULT_VOID(mountCowTruncate(cow, 20000), "grow after truncate");
        memset(expected + 4100, 0, sizeof(expected) - 4100);
        TEST_RESULT_UINT(mountCowRead(cow, actual, sizeof(actual), 0), 20000, "read extended file");
        TEST_RESULT_INT(memcmp(actual, expected, 20000), 0, "removed backup bytes read as zeros");
        TEST_RESULT_VOID(objFree(cow), "free COW");
        TEST_RESULT_BOOL(storageExistsP(storage, STRDEF("cow")), false, "COW file is removed");
        TEST_RESULT_BOOL(storageExistsP(storage, STRDEF("base")), true, "backup file still exists");

        MountCow *const clean = mountCowNew(sizeof(original), STRDEF(TEST_PATH "/clean"), mountTestOpen, basePath);
        mountTestOpens = 0;
        TEST_RESULT_VOID(mountCowTruncate(clean, 3), "truncate a clean partial block");
        TEST_RESULT_VOID(mountCowTruncate(clean, 8192), "extend clean file");
        TEST_RESULT_UINT(mountTestOpens, 0, "truncate does not read the backup");
        TEST_RESULT_UINT(mountCowRead(clean, actual, sizeof(actual), 0), 8192, "read extended clean file");
        memset(expected, 0, sizeof(expected));
        memcpy(expected, original, 3);
        TEST_RESULT_INT(memcmp(actual, expected, 8192), 0, "only the first three bytes come from the backup");
        TEST_RESULT_VOID(objFree(clean), "free clean file");
    }

    // *****************************************************************************************************************************
    if (testBegin("compare COW files with a file model"))
    {
        HRN_STORAGE_PUT(storage, "base", BUF(original, sizeof(original)));
        for (unsigned int fresh = 0; fresh < 2; fresh++)
        {
            uint8_t expected[32768] = {0};
            size_t length = fresh ? 0 : sizeof(original);
            memcpy(expected, original, length);
            MountCow *const cow = mountCowNew(length, STRDEF(TEST_PATH "/model"), mountTestOpen, basePath);
            uint64_t seed = 12345;
            for (unsigned int step = 0; step < 256; step++)
            {
                seed = seed * 6364136223846793005ULL + 1;
                const size_t offset = (size_t)((seed >> 32) % 24000);
                const size_t size = (size_t)((seed >> 24) % 4097);
                switch (seed % 4)
                {
                    case 0:
                    case 1:
                    {
                        uint8_t input[4097];
                        memset(input, (uint8_t)step, size);
                        mountCowWrite(cow, input, size, offset);
                        if (size != 0)
                        {
                            memcpy(expected + offset, input, size);
                            if (offset + size > length)
                                length = offset + size;
                        }
                        break;
                    }

                    case 2:
                        mountCowTruncate(cow, offset);
                        if (offset < length)
                            memset(expected + offset, 0, sizeof(expected) - offset);
                        length = offset;
                        break;

                    case 3:
                        mountCowClose(cow);
                        break;
                }
                uint8_t actual[32768];
                TEST_RESULT_UINT(mountCowRead(cow, actual, sizeof(actual), 0), length, "model size");
                TEST_RESULT_INT(memcmp(actual, expected, length), 0, "model bytes");
            }
            objFree(cow);
        }
    }

    // *****************************************************************************************************************************
    if (testBegin("empty operations, holes and failing backup reads"))
    {
        MountCow *const cow = mountCowNew(0, STRDEF(TEST_PATH "/new"), NULL, NULL);
        uint8_t buffer[8];
        TEST_RESULT_VOID(mountCowWrite(cow, "", 0, 100), "empty write");
        TEST_RESULT_UINT(cow->size, 0, "empty write does not extend file");
        TEST_RESULT_UINT(mountCowRead(cow, buffer, sizeof(buffer), 0), 0, "empty read");
        TEST_RESULT_VOID(mountCowWrite(cow, "abc", 3, 3), "write after a hole");
        TEST_RESULT_UINT(mountCowRead(cow, buffer, sizeof(buffer), 0), 6, "read hole and data");
        TEST_RESULT_INT(memcmp(buffer, "\0\0\0abc", 6), 0, "hole reads as zeros");
        TEST_ERROR(mountCowWrite(cow, "x", 1, INT64_MAX), FileWriteError, "COW write offset overflow");
#ifdef __linux__
        TEST_RESULT_VOID(mountCowAllocate(cow, 8192, 4096, true), "allocate blocks without changing the file size");
        TEST_RESULT_UINT(cow->size, 6, "keep size");
        TEST_RESULT_VOID(mountCowAllocate(cow, 8192, 4096, false), "extend by allocation");
        TEST_RESULT_UINT(cow->size, 12288, "allocated size");
        TEST_RESULT_UINT(mountCowRead(cow, buffer, sizeof(buffer), 8192), sizeof(buffer), "read allocated hole");
        TEST_RESULT_INT(memcmp(buffer, "\0\0\0\0\0\0\0\0", sizeof(buffer)), 0, "allocated hole stays zero");
#endif
        TEST_RESULT_VOID(objFree(cow), "free new file");
        char missingPath[] = TEST_PATH "/missing";
        MountCow *const missing = mountCowNew(10, STRDEF(TEST_PATH "/missing-cow"), mountTestOpen, missingPath);
        TEST_ERROR(
            mountCowRead(missing, buffer, 1, 0), FileOpenError,
            "unable to open test backup: [2] No such file or directory");
        HRN_STORAGE_PUT_Z(storage, "missing", "0123456789");
        TEST_RESULT_UINT(mountCowRead(missing, buffer, sizeof(buffer), 0), sizeof(buffer), "retry succeeds");
        TEST_RESULT_INT(memcmp(buffer, "01234567", 8), 0, "retry reads backup bytes");
        TEST_RESULT_VOID(objFree(missing), "free retried file");
    }

    // *****************************************************************************************************************************
    if (testBegin("cache checks leave the file open"))
    {
        Manifest *manifest;
        OBJ_NEW_BASE_BEGIN(Manifest, .childQty = MEM_CONTEXT_QTY_MAX)
        {
            manifest = manifestNewInternal();
        }
        OBJ_NEW_END();
        HRN_MANIFEST_FILE_ADD(
            manifest, .name = "pg_data/test", .size = 3, .checksumSha1 = "a9993e364706816aba3e25717850c26c9cd0d89d");
        const MountCache cache = {.storage = storage, .manifest = manifest};
        MountCacheFile source = {.cache = &cache};
        HRN_STORAGE_PUT_Z(storage, "a9993e364706816aba3e25717850c26c9cd0d89d-3", "abc");
        const int fd = mountCacheOpen(&source);
        char data[3];
        TEST_RESULT_INT(pread(fd, data, sizeof(data), 0), 3, "descriptor remains open after verification");
        TEST_RESULT_INT(memcmp(data, "abc", sizeof(data)), 0, "verified bytes");
        TEST_RESULT_BOOL(source.verified, true, "verified once for this inode");
        close(fd);
        const int reopened = mountCacheOpen(&source);
        TEST_RESULT_INT(pread(reopened, data, sizeof(data), 0), 3, "reopen verified cache");
        close(reopened);
    }

    // *****************************************************************************************************************************
    if (testBegin("manifest paths and tablespaces"))
    {
#ifdef HAVE_LIBFUSE3
        Manifest *manifest;
        OBJ_NEW_BASE_BEGIN(Manifest, .childQty = MEM_CONTEXT_QTY_MAX)
        {
            manifest = manifestNewInternal();
        }
        OBJ_NEW_END();
        HRN_MANIFEST_PATH_ADD(manifest, .name = "pg_data", .mode = 0700);
        HRN_MANIFEST_FILE_ADD(manifest, .name = "pg_data/a", .mode = 0600);
        HRN_MANIFEST_FILE_ADD(manifest, .name = "pg_data/tablespace_map", .mode = 0600);
        HRN_MANIFEST_FILE_ADD(manifest, .name = "pg_tblspc/123/PG_16/test", .mode = 0600);
        HRN_MANIFEST_TARGET_ADD(manifest, .name = "pg_tblspc/123", .path = "/external", .tablespaceId = 123);
        MountFs *const fs = mountFsNew((MountCache){.storage = storage, .manifest = manifest}, NULL);
        MountNode *const root = mountFsNode(fs, FUSE_ROOT_ID);
        TEST_RESULT_BOOL(mountFsChild(fs, root, "a") != NULL, true, "file from PGDATA");
        TEST_RESULT_PTR(mountFsChild(fs, root, "tablespace_map"), NULL, "omit original tablespace paths");
        MountNode *const tablespace = mountFsChild(fs, mountFsChild(fs, root, "pg_tblspc"), "123");
        TEST_RESULT_STR_Z(tablespace->link, "../.pgbackrest-tablespaces/123", "private tablespace link");
        TEST_ERROR(mountFsPath(fs, STRDEF("../escape")), FormatError, "invalid manifest path");
        TEST_ERROR(mountFsManifestPath(STRDEF("pg_data/.pgbackrest-tablespaces")), FormatError,
                   "manifest uses reserved mount tablespace path");
        TEST_RESULT_BOOL(mountFsContains(STRDEF("/"), STRDEF("/cache")), true, "root contains all paths");
        TEST_RESULT_BOOL(mountFsContains(STRDEF("/repo"), STRDEF("/repo2")), false, "component boundary");
        TEST_RESULT_VOID(objFree(fs), "free namespace");
#endif
    }
}
