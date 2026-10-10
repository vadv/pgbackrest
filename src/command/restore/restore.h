/***********************************************************************************************************************************
Restore Command
***********************************************************************************************************************************/
#ifndef COMMAND_RESTORE_RESTORE_H
#define COMMAND_RESTORE_RESTORE_H

#include "common/crypto/spec.h"

typedef struct RestoreBackupData
{
    unsigned int repoIdx;                                           // Internal repo index
    const CipherSpec *cipherSpecManifest;                           // Cipher spec to open the manifest, none when not encrypted
    const String *backupSet;                                        // Backup set to restore
} RestoreBackupData;

/***********************************************************************************************************************************
Functions
***********************************************************************************************************************************/
// Select a backup from the configured repositories
FN_EXTERN RestoreBackupData restoreBackupSet(void);

// Restore a backup
FN_EXTERN void cmdRestore(void);

#endif
