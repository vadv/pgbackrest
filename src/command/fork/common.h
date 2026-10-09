/***********************************************************************************************************************************
Fork Repository Bindings
***********************************************************************************************************************************/
#ifndef COMMAND_FORK_COMMON_H
#define COMMAND_FORK_COMMON_H

#include "config/config.h"
#include "info/infoFork.h"
#include "info/manifest/manifest.h"

#define FORK_PIN_PATH                                               "fork-pin"

FN_EXTERN String *forkSourceId(unsigned int repoIdx, const String *stanza, const String *archiveId, uint64_t systemId);
FN_EXTERN unsigned int forkSourceRepoIdx(const InfoFork *info);
// Build a path under the source stanza. Reject path traversal.
FN_EXTERN String *forkSourcePath(const String *stanza, const char *type, const String *path);
FN_EXTERN Manifest *forkSourceManifestLoad(unsigned int repoIdx, const String *stanza, const String *backupLabel);
FN_EXTERN InfoFork *forkInfoLoad(unsigned int repoIdx);

// Quote each archive command argument for the shell. Append PostgreSQL placeholders.
FN_EXTERN String *forkArchiveCommand(ConfigCommand command);
FN_EXTERN String *forkPgConfigQuote(const String *value);

// Read inherited backup files from the source repository. Keep manifests and all writes in the fork repository.
FN_EXTERN const Storage *forkStorageRepoIdx(unsigned int repoIdx);

// Protect source data from expiration and deletion while forks depend on it.
FN_EXTERN bool forkPinExists(unsigned int repoIdx);
FN_EXTERN String *forkPinName(unsigned int repoIdx, const String *stanza);

#endif
