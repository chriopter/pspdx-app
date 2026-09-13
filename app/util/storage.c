#include "util/storage.h"
#include "storage_paths.inc"
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char device[5] = "ms0:";
const char *storage_device(void) { return device; }
const char *storage_path(const char *relative) {
    for (size_t i = 0; i < sizeof(paths) / sizeof(*paths); i++)
        if (!strcmp(paths[i].relative, relative))
            return paths[i].absolute;
    /* A programming error, never user input. */
    abort();
}
int storage_exists(const char *path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}
static int recover(const char *path) {
    char bak[512];
    snprintf(bak, sizeof(bak), "%s.bak", path);
    if (!storage_exists(path) && storage_exists(bak)) {
        const char *base = strrchr(path, '/');
        if (sceIoRename(bak, base ? base + 1 : path) < 0)
            return -1;
    }
    return 0;
}
int storage_read(const char *path, char **text, size_t limit) {
    *text = NULL;
    if (recover(path) < 0)
        return -1;
    int fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    if (fd < 0)
        return -1;
    SceOff size = sceIoLseek(fd, 0, PSP_SEEK_END);
    if (size < 0 || (unsigned long long)size > limit || sceIoLseek(fd, 0, PSP_SEEK_SET) < 0) {
        sceIoClose(fd);
        return -1;
    }
    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        sceIoClose(fd);
        return -1;
    }
    size_t at = 0;
    while (at < (size_t)size) {
        int n = sceIoRead(fd, buf + at, (size_t)size - at);
        if (n <= 0) {
            free(buf);
            sceIoClose(fd);
            return -1;
        }
        at += n;
    }
    if (sceIoClose(fd) < 0) {
        free(buf);
        return -1;
    }
    buf[at] = 0;
    *text = buf;
    return (int)at;
}
int storage_write(const char *path, const void *data, size_t len) {
    char tmp[512], bak[512];
    if (strlen(path) > 480 || recover(path) < 0)
        return -1;
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    snprintf(bak, sizeof(bak), "%s.bak", path);
    int fd = sceIoOpen(tmp, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0)
        return -1;
    size_t at = 0;
    while (at < len) {
        int n = sceIoWrite(fd, (const char *)data + at, len - at);
        if (n <= 0) {
            sceIoClose(fd);
            sceIoRemove(tmp);
            return -1;
        }
        at += n;
    }
    if (sceIoClose(fd) < 0 || sceIoSync(device, 0) < 0)
        return -1;
    if (storage_exists(bak) && sceIoRemove(bak) < 0)
        return -1;
    int had = storage_exists(path);
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *bakbase = strrchr(bak, '/');
    bakbase = bakbase ? bakbase + 1 : bak;
    if (had && sceIoRename(path, bakbase) < 0)
        return -1;
    if (sceIoRename(tmp, base) < 0) {
        if (had)
            sceIoRename(bak, base);
        return -1;
    }
    if (sceIoSync(device, 0) < 0)
        return -1;
    if (had && sceIoRemove(bak) < 0)
        return -1;
    return 0;
}
/* Gone means gone: a remove that returns success and leaves the file --
   PPSSPP does that for a path whose case differs from the host's -- would
   otherwise let a journal survive its own recovery and block every install
   after it with a message about state. */
static int unlink_checked(const char *path) {
    if (!storage_exists(path))
        return 0;
    if (sceIoRemove(path) < 0 || storage_exists(path))
        return -1;
    return 0;
}
int storage_remove(const char *path) {
    char bak[512];
    snprintf(bak, sizeof(bak), "%s.bak", path);
    if (unlink_checked(bak) < 0)
        return -1;
    return unlink_checked(path);
}
void storage_app_path(const char *id, char *out, size_t size) {
    snprintf(out, size, "%s/%s.pspdx", storage_path("PSP/PSPDX/INSTALLED"), id);
}
void storage_init(const char *boot) {
    if (boot && !strncmp(boot, "ef0:/", 5))
        memcpy(device, "ef0:", 5);
    for (size_t i = 0; i < sizeof(paths) / sizeof(*paths); i++)
        snprintf(paths[i].absolute, sizeof(paths[i].absolute), "%s/%s", device, paths[i].relative);
    const char *dirs[] = {"PSP",
                          "PSP/GAME",
                          "PSP/PSPDX",
                          "PSP/PSPDX/INBOX",
                          "PSP/PSPDX/INSTALLED",
                          "PSP/PSPDX/CACHE",
                          "PSP/PSPDX/CACHE/catalogs",
                          "PSP/PSPDX/CACHE/media",
                          "PSP/PSPDX/TMP",
                          "PSP/PSPDX/LOGS",
                          "PSP/PSPDX/DEBUG",
                          "PSP/PSPDX/DEBUG/PSPDX_REC",
                          "PSP/PSPDX/CRYPTO"};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(*dirs); i++)
        sceIoMkdir(storage_path(dirs[i]), 0777);
}

/* Cache files are expendable; never traverse subdirectories or user data. */
void storage_trim_cache(const char *directory, size_t limit, const char *keep) {
    SceUID d = sceIoDopen(directory);
    if (d < 0)
        return;
    SceIoDirent e;
    unsigned long long bytes = 0;
    memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0) {
        if (!FIO_S_ISDIR(e.d_stat.st_mode))
            bytes += e.d_stat.st_size;
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
    if (bytes <= limit)
        return;
    d = sceIoDopen(directory);
    if (d < 0)
        return;
    memset(&e, 0, sizeof(e));
    while (bytes > limit && sceIoDread(d, &e) > 0) {
        if (!FIO_S_ISDIR(e.d_stat.st_mode)) {
            char path[512];
            int n = snprintf(path, sizeof(path), "%s/%s", directory, e.d_name);
            if (n > 0 && n < (int)sizeof(path) && strcmp(path, keep) && sceIoRemove(path) >= 0)
                bytes -= e.d_stat.st_size;
        }
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
}

int storage_remove_tree(const char *path) {
    SceUID d = sceIoDopen(path);
    if (d < 0)
        return storage_exists(path) ? (sceIoRemove(path) < 0 ? -1 : 0) : 0;
    SceIoDirent e;
    int rc = 0;
    memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0) {
        if (strcmp(e.d_name, ".") && strcmp(e.d_name, "..")) {
            char child[512];
            snprintf(child, sizeof(child), "%s/%s", path, e.d_name);
            if (FIO_S_ISDIR(e.d_stat.st_mode) ? storage_remove_tree(child) < 0
                                             : sceIoRemove(child) < 0)
                rc = -1;
        }
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
    if (rc < 0 || sceIoRmdir(path) < 0)
        return -1;
    return 0;
}
