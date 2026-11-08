/*
 * PSPDX as ARK's custom launcher. ARK starts VBOOT.PBP out of its own
 * folder in the home menu's place for as long as its SETTINGS.TXT has a
 * line "launcher, on"; SELECT held at power-on still brings the home menu.
 * Everything here is those two files: the line set and the EBOOT put in,
 * and both taken back. The file is the user's and ARK's as much as ours,
 * so of SETTINGS.TXT only the launcher line is ever written, and every
 * other line stays as it was, byte for byte.
 */

#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session/launcher.h"
#include "util/pbp.h"
#include "util/runtime.h"
#include "util/storage.h"

#define ARK_DIR "PSP/SAVEDATA/ARK_01234"
#define SETTINGS_MAX (64 * 1024)
#define MARK "PSP/PSPDX/launcher.resume"

static int g_mode = -1;            /* -1 until SETTINGS.TXT has been read */

/* ARK's folder: on a Go the internal storage first, where ARK is put, then
   the card; elsewhere the Memory Stick. 0 when there is one. */
static int ark_dir(char *out, size_t size) {
    static const char *const go[] = {"ef0:", "ms0:"}, *const card[] = {"ms0:"};
    int is_go = storage_is_go();
    const char *const *dev = is_go ? go : card;
    for (int i = 0; i < (is_go ? 2 : 1); i++) {
        if (!storage_device_available(dev[i])) continue;
        snprintf(out, size, "%s/" ARK_DIR, dev[i]);
        SceUID d = sceIoDopen(out);
        if (d >= 0) {
            sceIoDclose(d);
            return 0;
        }
    }
    return -1;
}

static void ark_file(const char *dir, const char *name, char *out, size_t size) {
    snprintf(out, size, "%s/%s", dir, name);
}

/* One field of a line "context, setting, value", its spaces trimmed off:
   from p up to the next comma or the line's end, which is returned. */
static const char *field(const char *p, const char *end, const char **start, size_t *n) {
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    const char *q = p;
    while (q < end && *q != ',') q++;
    const char *e = q;
    while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
    *start = p;
    *n = (size_t)(e - p);
    return q;
}

static int same(const char *a, size_t n, const char *word) {
    if (strlen(word) != n) return 0;
    for (size_t i = 0; i < n; i++)
        if ((a[i] | 0x20) != word[i]) return 0;
    return 1;
}

/* Whether the line [p, end) is the launcher's, and with on set to what it
   says. A comment, or a line with fewer than three fields, is no setting
   at all, as ARK reads it. */
static int launcher_line(const char *p, const char *end, int *on) {
    const char *word;
    size_t n;
    const char *at = field(p, end, &word, &n);
    if (!n || *word == '#' || at >= end) return 0;
    at = field(at + 1, end, &word, &n);
    if (!same(word, n, "launcher") || at >= end) return 0;
    field(at + 1, end, &word, &n);
    *on = same(word, n, "on") || same(word, n, "1") || same(word, n, "true") ||
          same(word, n, "enabled");
    return 1;
}

static int read_settings(const char *dir, char **text) {
    char path[96];
    ark_file(dir, "SETTINGS.TXT", path, sizeof(path));
    return storage_read(path, text, SETTINGS_MAX);
}

static int is_pspdx(const char *path);

/* On means PSPDX is ARK's launcher: the line on, and the VBOOT.PBP it starts
   a PSPDX. With the line on and some other launcher there -- ARK's own
   menu, or PSPLink on a test bench -- it is not ours to show as on, to
   turn off, or to suspend on the way to the home menu. */
int launcher_mode_on(void) {
    if (g_mode >= 0) return g_mode;
    g_mode = 0;
    char dir[64], *text = NULL;
    int n = ark_dir(dir, sizeof(dir)) == 0 ? read_settings(dir, &text) : -1;
    for (const char *p = text, *end = text + (n > 0 ? n : 0); p && p < end;) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *stop = nl ? nl : end;
        int on;
        if (launcher_line(p, stop, &on)) { g_mode = on; break; }
        p = nl ? nl + 1 : end;
    }
    free(text);
    if (g_mode) {
        char vboot[96];
        ark_file(dir, "VBOOT.PBP", vboot, sizeof(vboot));
        g_mode = is_pspdx(vboot);
    }
    return g_mode;
}

/* SETTINGS.TXT with its launcher line saying on or off: every such line
   rewritten in place, its line ending kept, or one added at the end when
   there was none -- in the line ending the file already uses. */
static int write_settings(const char *dir, int on) {
    char path[96], *text = NULL;
    ark_file(dir, "SETTINGS.TXT", path, sizeof(path));
    int n = storage_read(path, &text, SETTINGS_MAX);
    if (n < 0) return LAUNCHER_NO_SETTINGS;
    const char *line = on ? "always, launcher, on" : "always, launcher, off";
    size_t cap = (size_t)n * 2 + 64, len = 0;
    char *out = malloc(cap);
    if (!out) { free(text); return LAUNCHER_FAILED; }
    int found = 0, crlf = 0;
    for (const char *p = text, *end = text + n; p < end;) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *stop = nl ? nl : end;
        if (nl && nl > p && nl[-1] == '\r') crlf = 1;
        int was;
        if (launcher_line(p, stop, &was)) {
            found = 1;
            size_t keep = stop > p && stop[-1] == '\r' ? 1 : 0;
            memcpy(out + len, line, strlen(line));
            len += strlen(line);
            memcpy(out + len, stop - keep, keep + (nl ? 1 : 0));
            len += keep + (nl ? 1 : 0);
        } else {
            memcpy(out + len, p, (size_t)(stop - p) + (nl ? 1 : 0));
            len += (size_t)(stop - p) + (nl ? 1 : 0);
        }
        p = nl ? nl + 1 : end;
    }
    if (!found) {
        const char *eol = crlf ? "\r\n" : "\n";
        if (len && out[len - 1] != '\n') { memcpy(out + len, eol, strlen(eol)); len += strlen(eol); }
        memcpy(out + len, line, strlen(line));
        len += strlen(line);
        memcpy(out + len, eol, strlen(eol));
        len += strlen(eol);
    }
    free(text);
    int rc = storage_write(path, out, len);
    free(out);
    if (rc < 0) {
        logline("launcher: SETTINGS.TXT was not written");
        return LAUNCHER_FAILED;
    }
    g_mode = on;
    return 0;
}

static long long file_size(const char *path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) < 0 ? -1 : (long long)st.st_size;
}

/* An EBOOT of a few megabytes, a block at a time: there is not the room to
   hold it whole beside everything else. */
static int copy_file(const char *from, const char *to) {
    enum { BLOCK = 128 * 1024 };
    char *buf = malloc(BLOCK);
    int in = sceIoOpen(from, PSP_O_RDONLY, 0777);
    int out = in >= 0 ? sceIoOpen(to, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777) : -1;
    int rc = buf && in >= 0 && out >= 0 ? 0 : -1;
    while (rc == 0) {
        int n = sceIoRead(in, buf, BLOCK);
        if (n < 0) rc = -1;
        if (n <= 0) break;
        for (int at = 0; rc == 0 && at < n;) {
            int w = sceIoWrite(out, buf + at, (size_t)(n - at));
            if (w <= 0) rc = -1;
            else at += w;
        }
    }
    if (in >= 0) sceIoClose(in);
    if (out >= 0 && sceIoClose(out) < 0) rc = -1;
    free(buf);
    if (rc < 0) sceIoRemove(to);
    return rc;
}

/* Whether a PBP is a PSPDX: its PARAM.SFO carries the title the build
   gives it. By name and not by size -- an update changes the size, and an
   older PSPDX taken for ARK's own launcher would be put aside as that. */
static int is_pspdx(const char *path) {
    void *sfo;
    size_t n;
    if (pbp_section(path, PBP_SFO, &sfo, &n) != 0) return 0;
    static const char title[] = "PSPDX";
    int found = 0;
    for (size_t i = 0; !found && i + sizeof(title) <= n; i++)
        found = !memcmp((const char *)sfo + i, title, sizeof(title));   /* with its NUL */
    free(sfo);
    return found;
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

int launcher_set(int on) {
    char dir[64], settings[96], vboot[96], backup[96], fresh[sizeof(vboot) + 4];
    if (ark_dir(dir, sizeof(dir)) < 0) return LAUNCHER_NO_ARK;
    ark_file(dir, "SETTINGS.TXT", settings, sizeof(settings));
    if (!storage_exists(settings)) return LAUNCHER_NO_SETTINGS;
    ark_file(dir, "VBOOT.PBP", vboot, sizeof(vboot));
    ark_file(dir, "VBOOT_ARK.PBP", backup, sizeof(backup));
    /* The EBOOT this runs from, as the start path named its folder. Started
       as the launcher itself the folder is not known, and the release's
       name is what is tried. */
    char self[96];
    snprintf(self, sizeof(self), "%s/PSP/GAME/%s/EBOOT.PBP", storage_device(), storage_self_dir());
    long long size = file_size(self);
    if (!on) {
        int rc = write_settings(dir, 0);
        if (rc < 0) return rc;
        /* ARK's own launcher back where it was; without one put aside,
           ours goes, since with the line off nothing starts it -- and only
           ours, a VBOOT.PBP the size of the EBOOT this runs from. */
        if (storage_exists(backup)) {
            if (storage_exists(vboot) && sceIoRemove(vboot) < 0) return LAUNCHER_FAILED;
            if (sceIoRename(backup, "VBOOT.PBP") < 0) {
                logline("launcher: VBOOT_ARK.PBP was not put back");
                return LAUNCHER_FAILED;
            }
        } else if (is_pspdx(vboot)) {
            sceIoRemove(vboot);
        }
        logline("launcher: off");
        return 0;
    }

    if (size <= 0) {
        logline("launcher: %s is not there", self);
        return LAUNCHER_NO_SELF;
    }
    /* Copied beside the old one first, so that a copy cut short leaves
       ARK's VBOOT.PBP as it was. */
    snprintf(fresh, sizeof(fresh), "%s.new", vboot);
    if (copy_file(self, fresh) < 0 || file_size(fresh) != size) {
        sceIoRemove(fresh);
        logline("launcher: %s was not copied", self);
        return LAUNCHER_FAILED;
    }
    /* ARK's own is put aside once and never over: a second copy of PSPDX
       -- the launcher turned off in ARK's menu, then on here again -- is
       PSPDX, told by its size, and simply replaced. */
    if (storage_exists(vboot)) {
        int ours = is_pspdx(vboot);
        int put_aside = !ours && !storage_exists(backup);
        if (put_aside ? sceIoRename(vboot, base_name(backup)) < 0 : sceIoRemove(vboot) < 0) {
            sceIoRemove(fresh);
            return LAUNCHER_FAILED;
        }
    }
    if (sceIoRename(fresh, "VBOOT.PBP") < 0) {
        sceIoRemove(fresh);
        logline("launcher: VBOOT.PBP was not put in place");
        return LAUNCHER_FAILED;
    }
    int rc = write_settings(dir, 1);
    if (rc == 0) logline("launcher: on, %s in %s", self, dir);
    return rc;
}

int launcher_suspend(void) {
    char dir[64];
    if (ark_dir(dir, sizeof(dir)) < 0) return -1;
    /* The mark first: a cut between the two leaves the launcher on, which
       the next start then finds as it should be. */
    if (storage_write(storage_path(MARK), "", 0) < 0) return -1;
    if (write_settings(dir, 0) < 0) {
        storage_remove(storage_path(MARK));
        return -1;
    }
    return 0;
}

/* Started by ARK as its launcher: from its own folder, not from PSP/GAME.
   What the start is like follows from that -- the library first, nothing
   on the network -- and not from ARK's setting, which is also on when
   PSPDX is started from the home menu after leaving it for there. */
static int g_started_as_launcher;
int launcher_started(void) { return g_started_as_launcher; }

void launcher_note_start(const char *argv0) {
    g_started_as_launcher = argv0 && strstr(argv0, "SAVEDATA/ARK_01234/") != NULL;
}

/* The copy ARK starts is the PSPDX of the day it was made: after an update
   to PSP/GAME, the next start -- from either place -- copies the new one
   over it. */
static void launcher_refresh(void) {
    char dir[64], vboot[96], self[96];
    if (!launcher_mode_on() || ark_dir(dir, sizeof(dir)) < 0) return;
    ark_file(dir, "VBOOT.PBP", vboot, sizeof(vboot));
    snprintf(self, sizeof(self), "%s/PSP/GAME/%s/EBOOT.PBP", storage_device(),
             g_started_as_launcher ? "PSPDX" : storage_self_dir());
    long long size = file_size(self);
    if (size <= 0 || !is_pspdx(vboot) || file_size(vboot) == size) return;
    char fresh[sizeof(vboot) + 4];
    snprintf(fresh, sizeof(fresh), "%s.new", vboot);
    if (copy_file(self, fresh) == 0 && file_size(fresh) == size && sceIoRemove(vboot) >= 0 &&
        sceIoRename(fresh, "VBOOT.PBP") >= 0)
        logline("launcher: the copy in %s brought up to %s", dir, self);
    else
        sceIoRemove(fresh);
}

void launcher_resume(void) {
    launcher_refresh();
    if (!storage_exists(storage_path(MARK))) return;
    char dir[64];
    /* The line on again only while PSPDX is still the launcher there. */
    if (ark_dir(dir, sizeof(dir)) == 0) {
        char vboot[96];
        ark_file(dir, "VBOOT.PBP", vboot, sizeof(vboot));
        if (storage_exists(vboot) && write_settings(dir, 1) == 0)
            logline("launcher: on again after the home menu");
    }
    storage_remove(storage_path(MARK));
}
