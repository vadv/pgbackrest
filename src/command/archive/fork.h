/***********************************************************************************************************************************
Fork Archive Routing
***********************************************************************************************************************************/
#ifndef COMMAND_ARCHIVE_FORK_H
#define COMMAND_ARCHIVE_FORK_H

#include "info/infoFork.h"

typedef struct ArchiveFork
{
    InfoFork *info;
    unsigned int repoIdx;                                           // Fork repository index
    unsigned int sourceRepoIdx;                                     // Source repository index
} ArchiveFork;

// Load the fork metadata. Return NULL if the stanza is not a fork.
FN_EXTERN ArchiveFork *archiveForkLoad(void);

// Use the stanza's archive lock to prevent concurrent metadata updates by synchronous archive-push commands.
FN_EXTERN String *archiveForkLockName(unsigned int repoIdx);

// Check the exact requested WAL name before looking in either repository or a local queue.
FN_EXTERN bool archiveForkWalAllowed(const InfoFork *info, const String *request, bool parent);

// Check whether the timeline history contains this timeline and LSN. Each interval excludes its end LSN.
FN_EXTERN bool archiveForkHistoryContains(
    const String *history, unsigned int timeline, unsigned int pointTimeline, uint64_t pointLsn);

// Normalize timeline history for WAL selection. archive-push preserves the original history file.
FN_EXTERN String *archiveForkHistoryNormalize(const InfoFork *info, const String *name, const String *history);

// Validate and save the new timeline history before publishing its history file.
FN_EXTERN void archiveForkHistory(const ArchiveFork *fork, const String *name, const String *history);

#endif
