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
/* Takes the .new and the shadowed .bak files a cut write left in a
   directory. A .bak with no file beside it stays: it is the file. */
void storage_sweep(const char *directory);
void storage_app_path(const char *id, char *out, size_t size);
/* The directory under PSP/GAME a path names -- ms0:/PSP/GAME/PSPDX/EBOOT.PBP
   gives PSPDX -- matched without regard to case. 0 when it names none. */
int storage_game_dir(const char *path, char *out, size_t size);
/* The folder under PSP/GAME the running EBOOT was started from, or PSPDX,
   the name the release ships under, when the start path says nothing. */
const char *storage_self_dir(void);
/* Free bytes on the startup device, and the size of one cluster there, which
   every file rounds up to; -1 when the device does not say. */
long long storage_free_bytes(unsigned *cluster);
long long storage_free_bytes_on(const char *device, unsigned *cluster);
int storage_device_valid(const char *device);
int storage_device_available(const char *device);
int storage_is_go(void);
#endif
