/***********************************************************************************************************************************
Restore Individual Files
***********************************************************************************************************************************/
#include <build.h>

#include <unistd.h>
#include <utime.h>

#include "command/backup/blockMap.h"
#include "command/backup/common.h"
#include "command/restore/blockChecksum.h"
#include "command/restore/blockDelta.h"
#include "command/restore/file.h"
#include "common/crypto/cipherBlock.h"
#include "common/crypto/hash.h"
#include "common/debug.h"
#include "common/io/filter/size.h"
#include "common/io/io.h"
#include "common/io/limitRead.h"
#include "common/log.h"
#include "common/memContext.h"
#include "config/config.h"
#include "storage/helper.h"

/***********************************************************************************************************************************
Get the repository path and read range for a manifest file
***********************************************************************************************************************************/
FN_EXTERN RestoreFileRepo
restoreFileRepo(const Manifest *const manifest, const ManifestFile *const file)
{
    CHECK(FormatError, file->bundleOffset <= UINT64_MAX - file->sizeRepo, "repository range overflow");
    // A referenced file is stored under its source backup label
    RestoreFileRepo result =
    {
        .name = backupFileRepoPathP(
            file->reference != NULL ? file->reference : manifestData(manifest)->backupLabel,
            .manifestName = file->name, .bundleId = file->bundleId,
            .compressType = manifestData(manifest)->backupOptionCompressType, .blockIncr = file->blockIncrMapSize != 0),
    };

    // If block incremental then modify offset and size to where the map is stored since we need to read that first.
    if (file->blockIncrMapSize != 0)
    {
        CHECK(FormatError, file->blockIncrMapSize <= file->sizeRepo, "block map is larger than repository file");
        result.offset = file->bundleOffset + file->sizeRepo - file->blockIncrMapSize;
        result.limit = varNewUInt64(file->blockIncrMapSize);
    }
    // Else use the file range in the bundle
    else if (file->bundleId != 0)
    {
        result.offset = file->bundleOffset;
        result.limit = varNewUInt64(file->sizeRepo);
    }

    return result;
}

/***********************************************************************************************************************************
Copy a file from the backup to the specified destination
***********************************************************************************************************************************/
FN_EXTERN List *
restoreFile(
    const Storage *const storage, const String *const repoFile, const unsigned int repoIdx, const CompressType repoFileCompressType,
    const time_t copyTimeBegin,
    const bool delta, const bool deltaForce, const bool bundleRaw, const CipherSpec *const cipherSpecBackup,
    const StringList *const referenceList, List *const fileList)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE, storage);
        FUNCTION_LOG_PARAM(STRING, repoFile);
        FUNCTION_LOG_PARAM(UINT, repoIdx);
        FUNCTION_LOG_PARAM(ENUM, repoFileCompressType);
        FUNCTION_LOG_PARAM(TIME, copyTimeBegin);
        FUNCTION_LOG_PARAM(BOOL, delta);
        FUNCTION_LOG_PARAM(BOOL, deltaForce);
        FUNCTION_LOG_PARAM(BOOL, bundleRaw);
        FUNCTION_LOG_PARAM(CIPHER_SPEC, cipherSpecBackup);
        FUNCTION_LOG_PARAM(STRING_LIST, referenceList);             // List of references (for block incremental)
        FUNCTION_LOG_PARAM(LIST, fileList);                         // List of files to restore
    FUNCTION_LOG_END();

    ASSERT(repoFile != NULL);

    // Restore file results
    List *const result = lstNewP(sizeof(RestoreFileResult));

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // Check files to determine which ones need to be restored
        StorageReadMulti *const repoFileRead = storageNewReadMultiP(storageRepoIdx(repoIdx));

        for (unsigned int fileIdx = 0; fileIdx < lstSize(fileList); fileIdx++)
        {
            // Use a per-file mem context to reduce memory usage
            MEM_CONTEXT_TEMP_BEGIN()
            {
                RestoreFile *const file = lstGet(fileList, fileIdx);
                ASSERT(file->name != NULL);
                ASSERT(file->limit == NULL || varType(file->limit) == varTypeUInt64);

                RestoreFileResult *const fileResult = lstAdd(
                    result, &(RestoreFileResult){.manifestFile = file->manifestFile, .result = restoreResultCopy});

                // Perform delta if requested. Delta zero-length files to avoid overwriting the file if the timestamp is correct.
                if (delta && !file->zero)
                {
                    // Perform delta if the file exists
                    StorageInfo info = storageInfoP(storage, file->name, .ignoreMissing = true, .followLink = true);

                    if (info.exists)
                    {
                        // If force then use size/timestamp delta
                        if (deltaForce)
                        {
                            // Make sure that timestamp/size are equal and the timestamp is before the copy start time of the backup
                            if (info.size == file->size && info.timeModified == file->timeModified &&
                                info.timeModified < copyTimeBegin)
                            {
                                fileResult->result = restoreResultPreserve;
                            }
                        }
                        // Else use size and checksum
                        else
                        {
                            // Only continue delta if the file size is as expected or larger (for normal files) or if block
                            // incremental and the file to delta is not zero-length. Block incremental can potentially use almost
                            // any portion of an existing file, but of course zero-length files do not have anything to reuse.
                            if (info.size >= file->size || (file->blockIncrMapSize != 0 && info.size != 0))
                            {
                                const char *const fileName = strZ(storagePathP(storage, file->name));

                                // If the file was extended since the backup, then truncate it to the size it was during the backup
                                // as it might have only been appended to with the earlier portion being unchanged (we will verify
                                // this using the checksum below)
                                if (info.size > file->size)
                                {
                                    // Open file for write
                                    IoWrite *const pgWriteTruncate = storageWriteIo(
                                        storageNewWriteP(
                                            storage, file->name, .noAtomic = true, .noCreatePath = true,
                                            .noSyncPath = true, .noTruncate = true));
                                    ioWriteOpen(pgWriteTruncate);

                                    // Truncate to original size
                                    THROW_ON_SYS_ERROR_FMT(
                                        ftruncate(ioWriteFd(pgWriteTruncate), (off_t)file->size) == -1, FileWriteError,
                                        "unable to truncate file '%s'", fileName);

                                    // Close file
                                    ioWriteClose(pgWriteTruncate);

                                    // Update info
                                    info = storageInfoP(storage, file->name, .followLink = true);

                                    // For block incremental it is very important that the file be exactly the expected size, so
                                    // make sure the truncate worked as expected
                                    CHECK_FMT(
                                        FileWriteError, info.size == file->size, "unable to truncate '%s' to %" PRIu64 " bytes",
                                        strZ(file->name), file->size);
                                }

                                // Generate checksum for the file if size is not zero
                                IoRead *read = NULL;

                                if (file->size != 0)
                                {
                                    read = storageReadIo(storageNewReadP(storage, file->name));

                                    // Calculate checksum only when size matches
                                    if (info.size == file->size)
                                        ioFilterGroupAdd(ioReadFilterGroup(read), cryptoHashNew(hashTypeSha1));

                                    // Generate block checksum list if block incremental
                                    if (file->blockIncrMapSize != 0)
                                    {
                                        ioFilterGroupAdd(
                                            ioReadFilterGroup(read),
                                            blockChecksumNew(file->blockIncrSize, file->blockIncrChecksumSize));
                                    }

                                    ioReadDrain(read);
                                }

                                // If size/checksum is the same (or file is zero size) then no need to copy the file
                                if (file->size == 0 ||
                                    (info.size == file->size &&
                                     bufEq(
                                         file->checksum,
                                         pckReadBinP(ioFilterGroupResultP(ioReadFilterGroup(read), CRYPTO_HASH_FILTER_TYPE)))))
                                {
                                    // If the checksum/size are now the same but the time is not, then set the time back to the
                                    // backup time. This helps with unit testing, but also presents a pristine version of the
                                    // database after restore.
                                    if (info.timeModified != file->timeModified)
                                    {
                                        const struct utimbuf uTimeBuf =
                                        {
                                            .actime = file->timeModified,
                                            .modtime = file->timeModified,
                                        };

                                        THROW_ON_SYS_ERROR_FMT(
                                            utime(fileName, &uTimeBuf) == -1, FileInfoError, "unable to set time for '%s'",
                                            fileName);
                                    }

                                    fileResult->result = restoreResultPreserve;
                                }

                                // If block incremental and not preserving the file, store the block checksum list for later use in
                                // reconstructing the pg file
                                if (file->blockIncrMapSize != 0 && fileResult->result != restoreResultPreserve)
                                {
                                    PackRead *const blockChecksumResult = ioFilterGroupResultP(
                                        ioReadFilterGroup(read), BLOCK_CHECKSUM_FILTER_TYPE);

                                    MEM_CONTEXT_OBJ_BEGIN(fileList)
                                    {
                                        file->blockChecksum = pckReadBinP(blockChecksumResult);
                                    }
                                    MEM_CONTEXT_OBJ_END();
                                }
                            }
                        }
                    }
                }

                // Create zeroed and zero-length files
                if (fileResult->result == restoreResultCopy && (file->size == 0 || file->zero))
                {
                    // Create destination file
                    StorageWrite *const pgFileWrite = storageNewWriteP(
                        storage, file->name, .modeFile = file->mode, .user = file->user, .group = file->group,
                        .timeModified = file->timeModified, .noAtomic = true, .noCreatePath = true, .noSyncPath = true);

                    ioWriteOpen(storageWriteIo(pgFileWrite));

                    // Truncate the file to specified length (note in this case the file will grow, not shrink)
                    if (file->zero)
                    {
                        THROW_ON_SYS_ERROR_FMT(
                            ftruncate(ioWriteFd(storageWriteIo(pgFileWrite)), (off_t)file->size) == -1, FileWriteError,
                            "unable to truncate '%s'", strZ(file->name));
                    }

                    ioWriteClose(storageWriteIo(pgFileWrite));

                    // Report the file as zeroed or zero-length
                    fileResult->result = restoreResultZero;
                }

                // If the file will be copied add it to the read list
                if (fileResult->result == restoreResultCopy)
                {
                    storageReadMultiAddP(
                        repoFileRead, repoFile,
                        .compressible =
                            repoFileCompressType == compressTypeNone && cipherSpecType(cipherSpecBackup) == cipherTypeNone,
                        .offset = file->offset, .limit = file->limit);
                }
            }
            MEM_CONTEXT_TEMP_END();
        }

        // Copy files from repository to database
        ioReadOpen(storageReadMultiIo(repoFileRead));

        for (unsigned int fileIdx = 0; fileIdx < lstSize(fileList); fileIdx++)
        {
            // Copy file from repository to database
            RestoreFileResult *const fileResult = lstGet(result, fileIdx);

            if (fileResult->result == restoreResultCopy)
            {
                // Use a per-file mem context to reduce memory usage
                MEM_CONTEXT_TEMP_BEGIN()
                {
                    const RestoreFile *const file = lstGet(fileList, fileIdx);

                    // Create pg file
                    StorageWrite *const pgFileWrite = storageNewWriteP(
                        storage, file->name, .modeFile = file->mode, .user = file->user, .group = file->group,
                        .timeModified = file->timeModified, .noAtomic = true, .noCreatePath = true, .noSyncPath = true,
                        .noTruncate = file->blockChecksum != NULL);

                    // If block incremental file
                    const Buffer *checksum = NULL;

                    if (file->blockIncrMapSize != 0)
                    {
                        ASSERT(referenceList != NULL);

                        // Read block map. This will be compared to the block checksum list already created to determine which
                        // blocks need to be fetched from the repository. If we got here there must be at least one block to fetch.
                        IoRead *const blockMapRead = ioLimitReadNew(storageReadMultiIo(repoFileRead), varUInt64(file->limit));

                        if (cipherSpecType(cipherSpecBackup) != cipherTypeNone)
                        {
                            ioFilterGroupAdd(
                                ioReadFilterGroup(blockMapRead), cipherBlockNewP(
                                    cipherModeDecrypt, cipherSpecBackup, .header = cipherBlockHeaderNone));
                        }

                        ioReadOpen(blockMapRead);

                        const BlockMap *const blockMap = blockMapNewRead(
                            blockMapRead, file->blockIncrSize, file->blockIncrChecksumSize);

                        ioReadFree(blockMapRead);

                        // Open file to write
                        ioWriteOpen(storageWriteIo(pgFileWrite));

                        // Apply delta to file
                        BlockDelta *const blockDelta = blockDeltaNew(
                            blockMap, file->blockIncrSize, file->blockIncrChecksumSize, file->blockChecksum, cipherSpecBackup,
                            repoFileCompressType);

                        for (unsigned int readIdx = 0; readIdx < blockDeltaReadSize(blockDelta); readIdx++)
                        {
                            const BlockDeltaRead *const read = blockDeltaReadGet(blockDelta, readIdx);

                            // Open the super block list for read. Using one read for all super blocks is cheaper than reading from
                            // the file multiple times, which is especially noticeable on object stores.
                            StorageRead *const superBlockRead = storageNewReadP(
                                storageRepoIdx(repoIdx),
                                backupFileRepoPathP(
                                    strLstGet(referenceList, read->reference), .manifestName = file->manifestFile,
                                    .bundleId = read->bundleId, .blockIncr = true),
                                .offset = read->offset, .limit = VARUINT64(read->size));
                            ioReadOpen(storageReadIo(superBlockRead));

                            // Write updated blocks to the file
                            const BlockDeltaWrite *deltaWrite = blockDeltaNext(blockDelta, read, storageReadIo(superBlockRead));

                            while (deltaWrite != NULL)
                            {
                                // Seek to the block offset. It is possible we are already at the correct position but it is easier
                                // and safer to let ioWriteSeek() figure it out.
                                ioWriteSeek(storageWriteIo(pgFileWrite), deltaWrite->offset);

                                // Write block
                                ioWrite(storageWriteIo(pgFileWrite), deltaWrite->block);
                                fileResult->blockIncrDeltaSize += bufUsed(deltaWrite->block);

                                deltaWrite = blockDeltaNext(blockDelta, read, storageReadIo(superBlockRead));
                            }

                            storageReadFree(superBlockRead);
                        }

                        // Close the file to complete the update
                        ioWriteClose(storageWriteIo(pgFileWrite));

                        // Calculate checksum. In theory this is not needed because the file should always be reconstructed
                        // correctly. However, it seems better to check and the pages should still be buffered making the operation
                        // very fast.
                        IoRead *const read = storageReadIo(storageNewReadP(storage, file->name));

                        ioFilterGroupAdd(ioReadFilterGroup(read), cryptoHashNew(hashTypeSha1));
                        ioReadDrain(read);

                        checksum = pckReadBinP(ioFilterGroupResultP(ioReadFilterGroup(read), CRYPTO_HASH_FILTER_TYPE));
                    }
                    // Else normal file
                    else
                    {
                        IoFilterGroup *const filterGroup = ioWriteFilterGroup(storageWriteIo(pgFileWrite));

                        // Add decryption filter
                        if (cipherSpecType(cipherSpecBackup) != cipherTypeNone)
                        {
                            ioFilterGroupAdd(
                                filterGroup,
                                cipherBlockNewP(
                                    cipherModeDecrypt, cipherSpecBackup,
                                    .header = bundleRaw ? cipherBlockHeaderNone : cipherBlockHeaderMagic));
                        }

                        // Add decompression filter
                        if (repoFileCompressType != compressTypeNone)
                            ioFilterGroupAdd(filterGroup, decompressFilterP(repoFileCompressType, .raw = bundleRaw));

                        // Add sha1 filter
                        ioFilterGroupAdd(filterGroup, cryptoHashNew(hashTypeSha1));

                        // Add size filter
                        ioFilterGroupAdd(filterGroup, ioSizeNew());

                        // Copy file
                        ioWriteOpen(storageWriteIo(pgFileWrite));
                        ioCopyP(storageReadMultiIo(repoFileRead), storageWriteIo(pgFileWrite), .limit = file->limit);
                        ioWriteClose(storageWriteIo(pgFileWrite));

                        // Get checksum result
                        checksum = pckReadBinP(ioFilterGroupResultP(filterGroup, CRYPTO_HASH_FILTER_TYPE));
                    }

                    // Validate checksum
                    if (!bufEq(file->checksum, checksum))
                    {
                        THROW_FMT(
                            ChecksumError,
                            "error restoring '%s': actual checksum '%s' does not match expected checksum '%s'", strZ(file->name),
                            strZ(strNewEncode(encodingHex, checksum)), strZ(strNewEncode(encodingHex, file->checksum)));
                    }
                }
                MEM_CONTEXT_TEMP_END();
            }
        }
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(LIST, result);
}

/***********************************************************************************************************************************
Restore one manifest file into local cache storage
***********************************************************************************************************************************/
FN_EXTERN void
restoreFileTo(
    const unsigned int repoIdx, const Manifest *const manifest, const ManifestFile *const file, const Storage *const storage,
    const String *const destination)
{
    MEM_CONTEXT_TEMP_BEGIN()
    {
        const RestoreFileRepo repoFile = restoreFileRepo(manifest, file);
        // Use the same file parameters as ordinary restore, with a private local destination
        List *const fileList = lstNewP(sizeof(RestoreFile));
        lstAdd(
            fileList,
            &(RestoreFile)
        {
            .name = destination, .manifestFile = file->name, .size = file->size,
            .checksum = BUF(file->checksumSha1, HASH_TYPE_SHA1_SIZE), .mode = 0600,
            .offset = repoFile.offset, .limit = repoFile.limit, .blockIncrMapSize = file->blockIncrMapSize,
            .blockIncrSize = file->blockIncrSize, .blockIncrChecksumSize = file->blockIncrChecksumSize,
        });

        restoreFile(
            storage, repoFile.name, repoIdx, manifestData(manifest)->backupOptionCompressType, 0, false, false,
            file->bundleId != 0 && manifestData(manifest)->bundleRaw, manifestCipherSpec(manifest),
            manifestReferenceList(manifest), fileList);
        // Check the restored size before the cache entry becomes available to readers
        CHECK_FMT(
            ChecksumError, storageInfoP(storage, destination).size == file->size,
            "restored file size does not match the manifest for '%s'", strZ(file->name));
    }
    MEM_CONTEXT_TEMP_END();
}
