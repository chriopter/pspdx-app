#include "util/storage.h"
#include "util/runtime.h"
#include "storage_paths.inc"
#include <ctype.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __PSP__
#include <pspmscm.h>
#endif

static int card_separate = 1;

/* Legacy Go launches can alias ms0 to ef0 even with a card inserted.
   The PSP has no stat device/inode pair: test visibility of an exclusively
   created, empty marker once at startup, then remove only that marker. */
static int separate_card(void) {
    char internal[64], card[64];
    SceIoStat st;
    for (unsigned attempt = 0; attempt < 16; attempt++) {
        snprintf(internal, sizeof(internal), "ef0:/.pspdx-device-%08x-%02x", now_ms(), attempt);
        snprintf(card, sizeof(card), "ms0:%s", internal + 4);
        if (sceIoGetstat(internal, &st) >= 0 || sceIoGetstat(card, &st) >= 0) continue;
        int fd = sceIoOpen(internal, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_EXCL, 0600);
        if (fd < 0) break;
        sceIoClose(fd);
        int alias = sceIoGetstat(card, &st) >= 0;
        if (sceIoRemove(internal) < 0) logline("storage: could not remove device probe");
        return !alias;
    }
    /* Do not offer a destination whose identity could not be established. */
    return 0;
}

int storage_device_valid(const char *dev) {
    return dev && (!strcmp(dev, "ms0:") || !strcmp(dev, "ef0:"));
}
int storage_is_go(void) {
    SceUID d = sceIoDopen("ef0:/");
    if (d < 0) return 0;
    sceIoDclose(d);
    return 1;
}
int storage_device_available(const char *dev) {
    if (!storage_device_valid(dev)) return 0;
    if (!strcmp(dev, "ms0:") && !card_separate) return 0;
#ifdef __PSP__
    /* Even with distinct mappings, check physical presence: opening the
       device directory alone does not establish that an M2 is inserted. */
    if (!strcmp(dev, "ms0:") && storage_is_go() && MScmIsMediumInserted() != 1)
        return 0;
#endif
    char path[8];
    snprintf(path, sizeof(path), "%s/", dev);
    SceUID d = sceIoDopen(path);
    if (d < 0) return 0;
    sceIoDclose(d);
    return 1;
}

static char device[5] = "ms0:";
static char self_dir[64] = "PSPDX";
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
/* What a write cut short leaves beside the file: a .new that never got
   its rename, or a .bak the last step never removed. Neither is read once
   the file itself is there, so both go; a .bak with no file beside it is
   the file, and stays for recover() to move back. */
void storage_sweep(const char *directory) {
    SceUID d = sceIoDopen(directory);
    if (d < 0)
        return;
    SceIoDirent e;
    memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0) {
        size_t n = strlen(e.d_name);
        int stale = n > 4 && !strcmp(e.d_name + n - 4, ".new");
        if (n > 4 && !strcmp(e.d_name + n - 4, ".bak")) {
            char file[512];
            snprintf(file, sizeof(file), "%s/%.*s", directory, (int)n - 4, e.d_name);
            stale = storage_exists(file);
        }
        if (stale && !FIO_S_ISDIR(e.d_stat.st_mode)) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", directory, e.d_name);
            if (sceIoRemove(path) < 0)
                logline("sweep: %s would not go", e.d_name);
        }
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
}
void storage_app_path(const char *id, char *out, size_t size) {
    snprintf(out, size, "%s/%s.pspdx", storage_path("PSP/PSPDX/INSTALLED"), id);
}
int storage_game_dir(const char *path, char *out, size_t size) {
    static const char mark[] = "PSP/GAME/";
    for (const char *p = path; p && *p; p++) {
        size_t i = 0;
        while (mark[i] && toupper((unsigned char)p[i]) == mark[i])
            i++;
        if (mark[i])
            continue;
        const char *start = p + i;
        const char *slash = strchr(start, '/');
        if (!slash)
            return 0;
        size_t n = (size_t)(slash - start);
        if (n == 0 || n >= size)
            return 0;
        memcpy(out, start, n);
        out[n] = '\0';
        return 1;
    }
    return 0;
}
const char *storage_self_dir(void) { return self_dir; }
long long storage_free_bytes(unsigned *cluster) {
    return storage_free_bytes_on(device, cluster);
}
long long storage_free_bytes_on(const char *dev, unsigned *cluster) {
    if (!storage_device_valid(dev)) return -1;
    /* The Memory Stick's own count, as the information band reads it: a
       FAT32 free count walks the allocation table, so it is asked when an
       install needs the answer and not before. */
    struct ms_info {
        unsigned max_clusters, free_clusters, max_sectors, sector_size, sector_count;
    } info;
    struct {
        struct ms_info *at;
    } command = {&info};
    memset(&info, 0, sizeof(info));
    if (sceIoDevctl(dev, 0x02425818, &command, sizeof(command), NULL, 0) < 0)
        return -1;
    unsigned long long unit = (unsigned long long)info.sector_count * info.sector_size;
    if (!unit || unit > 0x10000000u)
        return -1;
    if (cluster)
        *cluster = (unsigned)unit;
    return (long long)(info.free_clusters * unit);
}
void storage_init(const char *boot) {
    int go = storage_is_go();
    card_separate = !go || separate_card();
    memcpy(device, "ms0:", 5);
    if ((boot && !strncmp(boot, "ef0:/", 5)) ||
        (go && (!card_separate || !boot || strncmp(boot, "ms0:/", 5))))
        memcpy(device, "ef0:", 5);
    char here[64];
    if (boot && storage_game_dir(boot, here, sizeof(here)))
        snprintf(self_dir, sizeof(self_dir), "%s", here);
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
