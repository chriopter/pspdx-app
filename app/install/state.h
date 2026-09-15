#ifndef PSPDX_STATE_H
#define PSPDX_STATE_H
#include "install/install.h"
#include "update/pspdx.h"
#include <cjson/cJSON.h>
int state_load(void);
int state_validate(const cJSON *records);
int state_ok(void);
cJSON *state_snapshot(void);
int state_restore(const cJSON *snapshot);
int state_restore_app(const cJSON *snapshot, const char *id);
/* The release in m installed in PSP/GAME/<dir>, from a .pspdx that named
   file_dir, or NULL to keep what the record said. */
int state_commit(const struct manifest *m, const char *dir, const unsigned char *sha256,
                 const char *file_dir);
/* The folder an older record's .pspdx named, written into the record before
   the saved file is replaced by a newer one; nothing when it has one. */
int state_note_file_dir(const char *id, const char *file_dir);
int state_forget(const char *id);
/* A record of id from source for PSP/GAME/<dir> forgotten with its saved
   file: 1 when there was one, 0 when not, -1 when it would not go. */
int state_retire_legacy(const char *id, const char *source, const char *dir);
int state_count(void);
const char *state_id(int index);
/* The record's latest release into m, which is zeroed first and so must hold
   no text of its own. */
int state_latest(const char *id, struct manifest *m);
int state_note_latest(const struct manifest *m);
int state_read_manifest(const char *id, char **raw, struct pspdx_file *file);
int state_target_owner(const char *dir, const char *id);
#endif
