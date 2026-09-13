#ifndef PSPDX_STORAGE_H
#define PSPDX_STORAGE_H
#include <stddef.h>
/* All paths are immutable after startup, so worker threads can share them. */
void storage_trim_cache(const char *directory,size_t limit,const char *keep);
void storage_init(const char *boot);
const char *storage_path(const char *relative);
const char *storage_device(void);
int storage_exists(const char *path);
int storage_read(const char *path, char **text, size_t limit);
int storage_write(const char *path, const void *data, size_t len);
int storage_remove(const char *path);
/* A directory and everything under it. Returns 0 when nothing is left. */
int storage_remove_tree(const char *path);
void storage_app_path(const char *id, char *out, size_t size);
#endif
