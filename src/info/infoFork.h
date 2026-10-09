/***********************************************************************************************************************************
Fork Info Handler
***********************************************************************************************************************************/
#ifndef INFO_INFOFORK_H
#define INFO_INFOFORK_H

typedef struct InfoFork InfoFork;

#include "common/type/object.h"
#include "common/type/stringList.h"
#include "info/info.h"

#define INFO_FORK_FILE                                              "fork.info"
#define INFO_FORK_PATH_FILE                                         STORAGE_REPO_BACKUP "/" INFO_FORK_FILE
#define INFO_FORK_FORMAT                                            1

typedef enum
{
    infoForkStatePreparing,
    infoForkStatePublished,
} InfoForkState;

typedef struct InfoForkData
{
    const String *sourceId;                                         // Stable source identity
    unsigned int sourceRepoKey;                                     // Configured source repository number (starts at 1)
    const String *sourceStanza;
    const String *sourceArchiveId;
    const String *backupLabel;
    const StringList *importedBackupList;
    unsigned int pgVersion;
    uint64_t systemId;
    unsigned int walSegmentSize;
    unsigned int sourceTimeline;
    const String *sourceHistory;                                    // Saved source timeline history; empty on timeline 1
    const String *targetType;                                       // time or lsn
    const String *targetValue;
    bool targetExclusive;
    InfoForkState state;
    unsigned int ownTimeline;                                       // Zero until the branch is confirmed
    uint64_t branchLsn;                                             // Zero until the branch is confirmed
    unsigned int historyTimeline;                                   // Latest accepted timeline
    const String *history;                                          // Accepted timeline history; empty before branch confirmation
} InfoForkData;

// The constructor duplicates data, including strings and the imported backup list.
FN_EXTERN InfoFork *infoForkNew(unsigned int format, const InfoForkData *data);
FN_EXTERN InfoFork *infoForkNewLoad(IoRead *read, const CipherSpec *cipherSpec);
FN_EXTERN const InfoForkData *infoForkData(const InfoFork *this);
FN_EXTERN unsigned int infoForkFormat(const InfoFork *this);

FN_EXTERN void infoForkPublishedSet(InfoFork *this);

// Confirm or advance the accepted timeline history. The first confirmation sets the branch timeline and LSN. Retries must match.
FN_EXTERN void infoForkHistorySet(InfoFork *this, unsigned int historyTimeline, uint64_t branchLsn, const String *history);

// The caller selects the storage, path and repository's main cipher. Load from the .copy file if the main file fails.
FN_EXTERN InfoFork *infoForkLoadFile(const Storage *storage, const String *fileName, const CipherSpec *cipherSpec);
FN_EXTERN void infoForkSaveFile(InfoFork *this, const Storage *storage, const String *fileName, const CipherSpec *cipherSpec);

FN_INLINE_ALWAYS InfoFork *
infoForkMove(InfoFork *const this, MemContext *const parentNew)
{
    return objMove(this, parentNew);
}

FN_INLINE_ALWAYS void
infoForkFree(InfoFork *const this)
{
    objFree(this);
}

#define FUNCTION_LOG_INFO_FORK_TYPE                                                                                                \
    InfoFork *
#define FUNCTION_LOG_INFO_FORK_FORMAT(value, buffer, bufferSize)                                                                   \
    objNameToLog(value, "InfoFork", buffer, bufferSize)

#endif
