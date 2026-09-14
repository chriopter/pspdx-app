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
int state_commit(const struct manifest *m, const char *dir, const unsigned char *sha256);
int state_forget(const char *id);
int state_count(void);
const char *state_id(int index);
int state_latest(const char *id, struct manifest *m);
int state_note_latest(const struct manifest *m);
int state_read_manifest(const char *id, char **raw, struct pspdx_file *file);
int state_target_owner(const char *dir, const char *id);
#endif
