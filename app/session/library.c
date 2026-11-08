/*
 * The library: the games and apps on the stick, read on a thread of its own
 * and handed to the main thread whole. A reading is two passes. The first
 * lists the directories and takes every title it can from the cache kept
 * of earlier readings -- on any start but the first that is all of them,
 * and the list is up after a few directory reads. The second opens what
 * the cache did not know, for its PARAM.SFO only: an EBOOT's first section,
 * a disc image's few sectors on the way to PSP_GAME/PARAM.SFO. Each pass
 * that finds something is offered to the main thread, which takes it over
 * between two frames; the thread waits for that before it builds the next,
 * so the list the main thread reads is never one being written.
 */

#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifdef __PSP__
#include <pspkernel.h>
#endif

#include "session/actions.h"
#include "session/launcher.h"
#include "session/library.h"
#include "update/pspdx.h"
#include "util/iso.h"
#include "util/pbp.h"
#include "util/runtime.h"
#include "util/sfo.h"
#include "util/storage.h"

#define SFO_MAX (64 * 1024)
#define ICON_MAX (128 * 1024)
/* What the stick holds, before it is sorted into what is listed: every
   folder and image looked at, the ones that are not listed too, so that the
   cache knows those as well and the next reading does not open them. */
struct found {
    struct library_item item;
    unsigned long long size;
    int kind;                       /* enum launch_kind, -1 for not listed */
    int known;                      /* title and kind read, here or in the cache */
};

/* ------------------------------------------------------------- the list */

/* The main thread's: the items as last taken over, and where each section
   starts in them. */
static struct library_item *g_items;
static int g_count, g_ready;
static int g_first[LIBRARY_SECTIONS], g_in[LIBRARY_SECTIONS];

/* Between the two threads: a list offered, not taken over yet. */
static struct library_item *volatile g_offer;
static volatile int g_offer_count, g_offered;
static volatile int g_running;
static int g_again;

int library_count(void) { return g_count; }
int library_ready(void) { return g_ready; }
int library_scanning(void) { return g_running || g_offered; }
const struct library_item *library_item(int n) { return n >= 0 && n < g_count ? &g_items[n] : 0; }
int library_section_first(int s) { return s >= 0 && s < LIBRARY_SECTIONS ? g_first[s] : 0; }
int library_section_count(int s) { return s >= 0 && s < LIBRARY_SECTIONS ? g_in[s] : 0; }

int library_find(unsigned key) {
    for (int n = 0; n < g_count; n++)
        if (g_items[n].key == key) return n;
    return -1;
}

static void adopt(void) {
    free(g_items);
    g_items = g_offer;
    g_count = g_offer_count;
    g_offer = 0;
    memset(g_in, 0, sizeof(g_in));
    for (int n = 0; n < g_count; n++) g_in[g_items[n].section]++;
    for (int s = 0, at = 0; s < LIBRARY_SECTIONS; at += g_in[s++]) g_first[s] = at;
    g_ready = 1;
    g_offered = 0;
}

/* ------------------------------------------------------------- the grid */

static int section_of(int n) {
    for (int s = 0; s < LIBRARY_SECTIONS; s++)
        if (n >= g_first[s] && n < g_first[s] + g_in[s]) return s;
    return LIBRARY_SECTIONS - 1;
}
static int lines_of(int s) { return (g_in[s] + LIBRARY_COLS - 1) / LIBRARY_COLS; }

int library_lines(void) {
    int lines = 0;
    for (int s = 0; s < LIBRARY_SECTIONS; s++) lines += lines_of(s);
    return lines;
}

int library_line(int n) {
    int s = section_of(n), line = 0;
    for (int k = 0; k < s; k++) line += lines_of(k);
    return line + (n - g_first[s]) / LIBRARY_COLS;
}

/* The item in column col of grid line line, or the last of a line too
   short to have one there. */
static int item_at(int line, int col) {
    for (int s = 0; s < LIBRARY_SECTIONS; s++) {
        if (line < lines_of(s)) {
            int k = line * LIBRARY_COLS + col;
            return g_first[s] + (k < g_in[s] ? k : g_in[s] - 1);
        }
        line -= lines_of(s);
    }
    return g_count - 1;
}

int library_step(int n, int dx, int dy, int ring) {
    if (g_count <= 0) return 0;
    if (n < 0 || n >= g_count) n = 0;
    int s = section_of(n), k = n - g_first[s], col = k % LIBRARY_COLS;
    if (dx) {
        if (col + dx < 0 || col + dx >= LIBRARY_COLS || k + dx < 0 || k + dx >= g_in[s]) return n;
        return n + dx;
    }
    int lines = library_lines(), to = library_line(n) + dy;
    if (to < 0 || to >= lines) {
        if (!ring) return n;
        to = to < 0 ? lines - 1 : 0;
    }
    return item_at(to, col);
}

/* -------------------------------------------------------------- reading */

int library_kind_of(const char *category) {
    if (!strcmp(category, "ME")) return LAUNCH_PS1;
    /* A PSN title, or a game a disc was made into an EBOOT of. */
    if (!strcmp(category, "EG") || !strcmp(category, "UG")) return LAUNCH_PSN;
    /* A game's update and saved data have folders of their own under
       PSP/GAME and nothing to start. */
    if (!strcmp(category, "PG") || !strcmp(category, "MS")) return -1;
    /* "MG", and whatever an author wrote or did not. */
    return LAUNCH_HOMEBREW;
}

static unsigned path_key(const char *path) {
    unsigned h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) h = (h ^ *p) * 16777619u;
    return h;
}

/* A title as the list and the cache file can carry it: whole letters, no
   line breaks or tabs. */
static void tidy(char *title) {
    for (char *p = title; *p; p++)
        if ((unsigned char)*p < 0x20) *p = ' ';
    pspdx_utf8_mend(title);
}

/* A file's own name without its folder or extension: what a disc is called
   until its PARAM.SFO is read, and if it has none. */
static void title_from_name(const char *path, int ext, char *out, size_t size) {
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    size_t n = strlen(name);
    const char *dot = ext ? strrchr(name, '.') : 0;
    if (dot) n = (size_t)(dot - name);
    snprintf(out, size, "%.*s", (int)n, name);
}

static void read_title(struct found *f) {
    void *sfo = 0;
    size_t n = 0;
    char category[8] = "";
    int image = f->kind == LAUNCH_ISO;
    int rc = image ? iso_file(f->item.path, "PSP_GAME/PARAM.SFO", SFO_MAX, &sfo, &n)
                   : pbp_section(f->item.path, PBP_SFO, &sfo, &n);
    if (rc != 0 || sfo_string(sfo, n, "TITLE", f->item.title, sizeof(f->item.title)) != 0 ||
        !f->item.title[0]) {
        /* A disc by its file's name, an EBOOT by its folder's. */
        if (image) {
            title_from_name(f->item.path, 1, f->item.title, sizeof(f->item.title));
        } else {
            char folder[LIBRARY_PATH];
            snprintf(folder, sizeof(folder), "%s", f->item.path);
            char *slash = strrchr(folder, '/');
            if (slash) *slash = '\0';
            title_from_name(folder, 0, f->item.title, sizeof(f->item.title));
        }
    }
    if (!image) {
        sfo_string(sfo, n, "CATEGORY", category, sizeof(category));
        f->kind = library_kind_of(category);
    }
    free(sfo);
    tidy(f->item.title);
    f->known = 1;
}

int library_icon_png(const char *path, int kind, void **png, size_t *len) {
    if (kind == LAUNCH_ISO) return iso_file(path, "PSP_GAME/ICON0.PNG", ICON_MAX, png, len);
    return pbp_section(path, PBP_ICON0, png, len);
}

/* ---------------------------------------------------------------- cache */

/* One line a thing looked at: its size, its kind, its title and its path,
   by tabs. Read into a table of places by the path's hash. */
#define CACHE_PLACES 2048
struct cached { const char *path, *title; unsigned long long size; int kind; };
static struct cached *g_cache;
static int g_cache_at[CACHE_PLACES];
static char *g_cache_text;
static char g_cache_path[LIBRARY_PATH];

static void cache_load(void) {
    memset(g_cache_at, -1, sizeof(g_cache_at));
    g_cache = 0;
    g_cache_text = 0;
    int len = storage_read(g_cache_path, &g_cache_text, (size_t)LIBRARY_MAX * 2 * (LIBRARY_PATH + 96));
    if (len <= 0) return;
    g_cache = calloc(CACHE_PLACES, sizeof(*g_cache));
    if (!g_cache) return;
    int n = 0;
    for (char *line = g_cache_text; line && *line && n < CACHE_PLACES;) {
        char *end = strchr(line, '\n');
        if (end) *end++ = '\0';
        char *f[4] = {line, 0, 0, 0};
        for (int i = 1; i < 4 && f[i - 1]; i++) {
            f[i] = strchr(f[i - 1], '\t');
            if (f[i]) *f[i]++ = '\0';
        }
        if (f[3] && f[3][0]) {
            struct cached *c = &g_cache[n];
            c->size = strtoull(f[0], 0, 10);
            c->kind = atoi(f[1]);
            c->title = f[2];
            c->path = f[3];
            for (unsigned at = path_key(c->path) % CACHE_PLACES;; at = (at + 1) % CACHE_PLACES)
                if (g_cache_at[at] < 0) { g_cache_at[at] = n; break; }
            n++;
        }
        line = end;
    }
}

static const struct cached *cache_find(const char *path, unsigned long long size) {
    if (!g_cache) return 0;
    for (unsigned at = path_key(path) % CACHE_PLACES, tries = 0; tries < CACHE_PLACES;
         at = (at + 1) % CACHE_PLACES, tries++) {
        if (g_cache_at[at] < 0) return 0;
        const struct cached *c = &g_cache[g_cache_at[at]];
        if (!strcmp(c->path, path)) return c->size == size ? c : 0;
    }
    return 0;
}

static void cache_free(void) {
    free(g_cache);
    free(g_cache_text);
    g_cache = 0;
    g_cache_text = 0;
}

static void cache_save(const struct found *f, int n) {
    size_t room = (size_t)n * (LIBRARY_PATH + LIBRARY_TITLE + 40) + 1, at = 0;
    char *text = malloc(room);
    if (!text) return;
    for (int i = 0; i < n; i++)
        if (f[i].known)
            at += (size_t)snprintf(text + at, room - at, "%llu\t%d\t%s\t%s\n", f[i].size, f[i].kind,
                                   f[i].item.title, f[i].item.path);
    if (storage_write_cache(g_cache_path, text, at) < 0) logline("library: cache not kept");
    free(text);
}

/* ----------------------------------------------------------------- walk */

struct walk {
    struct found *f;
    int n, cap;
};

static void add(struct walk *w, const char *path, unsigned long long size, int kind) {
    if (w->n >= LIBRARY_MAX || strlen(path) >= LIBRARY_PATH) return;
    if (w->n == w->cap) {
        int cap = w->cap ? w->cap * 2 : 64;
        struct found *more = realloc(w->f, (size_t)cap * sizeof(*more));
        if (!more) return;
        w->f = more;
        w->cap = cap;
    }
    struct found *f = &w->f[w->n++];
    memset(f, 0, sizeof(*f));
    snprintf(f->item.path, sizeof(f->item.path), "%s", path);
    f->item.key = path_key(path);
    f->size = size;
    f->kind = kind;
    const struct cached *c = cache_find(path, size);
    if (c) {
        snprintf(f->item.title, sizeof(f->item.title), "%s", c->title);
        tidy(f->item.title);
        f->kind = c->kind;
        f->known = 1;
    } else if (kind == LAUNCH_ISO) {
        /* Listed at once under its file's name; the second pass reads
           what it is really called. */
        title_from_name(path, 1, f->item.title, sizeof(f->item.title));
    }
}

/* The images in a folder under ISO/, and with depth the folders in it a
   level down -- how people sort a collection -- but never ISO/VIDEO, which
   is the XMB's films. */
static void walk_images(struct walk *w, const char *dir, int depth) {
    SceUID d = sceIoDopen(dir);
    if (d < 0) return;
    SceIoDirent e;
    char path[LIBRARY_PATH + 64];
    for (;;) {
        memset(&e, 0, sizeof(e));
        if (sceIoDread(d, &e) <= 0) break;
        if (e.d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e.d_name);
        if (FIO_S_ISDIR(e.d_stat.st_mode)) {
            if (depth && strcasecmp(e.d_name, "VIDEO")) walk_images(w, path, 0);
        } else if (iso_name(e.d_name)) {
            add(w, path, (unsigned long long)e.d_stat.st_size, LAUNCH_ISO);
        }
    }
    sceIoDclose(d);
}

/* The folders under PSP/GAME and its kin that hold an EBOOT.PBP. One whose
   name starts with a dot or a per cent sign is not one to start: a stage
   of PSPDX's own, the other half of a 1.50 kxploit pair. A ".bak" is the
   copy an update stepped aside. */
static void walk_eboots(struct walk *w, const char *dir) {
    SceUID d = sceIoDopen(dir);
    if (d < 0) return;
    SceIoDirent e;
    char path[LIBRARY_PATH + 64];
    for (;;) {
        memset(&e, 0, sizeof(e));
        if (sceIoDread(d, &e) <= 0) break;
        size_t n = strlen(e.d_name);
        if (!FIO_S_ISDIR(e.d_stat.st_mode) || e.d_name[0] == '.' || e.d_name[0] == '%' ||
            (n > 4 && !strcasecmp(e.d_name + n - 4, ".bak")))
            continue;
        snprintf(path, sizeof(path), "%s/%s/EBOOT.PBP", dir, e.d_name);
        SceIoStat st;
        memset(&st, 0, sizeof(st));
        if (sceIoGetstat(path, &st) < 0 || FIO_S_ISDIR(st.st_mode)) continue;
        add(w, path, (unsigned long long)st.st_size, LAUNCH_HOMEBREW);
    }
    sceIoDclose(d);
}

/* --------------------------------------------------------------- passes */

static char g_devices[2][5];
static int g_device_count;

static int section_for(int kind) {
    return kind == LAUNCH_PS1 ? LIBRARY_PS1 : kind == LAUNCH_HOMEBREW ? LIBRARY_APPS : LIBRARY_PSP;
}

/* A name is filed under its first letter or digit, as the store files
   them: a quote or a bracket before it does not put it first. */
static const char *filed(const char *name) {
    const char *p = name;
    while (*p && (unsigned char)*p < 0x80 && !((*p >= '0' && *p <= '9') || ((*p | 32) >= 'a' && (*p | 32) <= 'z')))
        p++;
    return *p ? p : name;
}
static int by_section_and_title(const void *a, const void *b) {
    const struct library_item *x = a, *y = b;
    if (x->section != y->section) return x->section - y->section;
    int c = strcasecmp(filed(x->title), filed(y->title));
    return c ? c : strcmp(x->path, y->path);
}

/* What the main thread is to list out of what was found: the items that
   are known and listed, and with partial the images not read yet. Offered,
   and waited for until it is taken. */
static void offer(const struct found *f, int n) {
    struct library_item *items = malloc((size_t)(n ? n : 1) * sizeof(*items));
    if (!items) return;
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (f[i].kind < 0 || (!f[i].known && f[i].kind != LAUNCH_ISO)) continue;
        items[count] = f[i].item;
        items[count].kind = (unsigned char)f[i].kind;
        items[count].section = (unsigned char)section_for(f[i].kind);
        count++;
    }
    qsort(items, (size_t)count, sizeof(*items), by_section_and_title);
    g_offer = items;
    g_offer_count = count;
    g_offered = 1;
#ifdef __PSP__
    while (g_offered) sceKernelDelayThread(10 * 1000);
#else
    adopt();
#endif
}

static void scan(void) {
    unsigned t0 = now_ms();
    struct walk w = {0, 0, 0};
    char dir[32];
    cache_load();
    for (int i = 0; i < g_device_count; i++) {
        snprintf(dir, sizeof(dir), "%s/ISO", g_devices[i]);
        walk_images(&w, dir, 1);
        static const char *const GAMES[] = {"PSP/GAME", "PSP/GAME150", "PSP/APPS"};
        for (int k = 0; k < 3; k++) {
            snprintf(dir, sizeof(dir), "%s/%s", g_devices[i], GAMES[k]);
            walk_eboots(&w, dir);
        }
    }
    int unknown = 0, gone = g_cache ? 0 : 1;
    for (int i = 0; i < w.n; i++) unknown += !w.f[i].known;
    /* Something the cache had that is not there any more: written again
       without it, or it would grow for ever. */
    if (g_cache) {
        int had = 0;
        for (int i = 0; i < CACHE_PLACES; i++) had += g_cache_at[i] >= 0;
        gone = had != w.n - unknown;
    }
    cache_free();
    unsigned listed = now_ms();
    offer(w.f, w.n);
    if (unknown) {
        for (int i = 0; i < w.n; i++)
            if (!w.f[i].known) read_title(&w.f[i]);
        offer(w.f, w.n);
    }
    if (unknown || gone) cache_save(w.f, w.n);
    logline("library: %d found, %d read, listed in %u ms, done in %u ms", w.n, unknown, listed - t0,
            now_ms() - t0);
    free(w.f);
}

/* -------------------------------------------------------------- threads */

static void devices(void) {
    g_device_count = 0;
    static const char *const DEV[] = {"ms0:", "ef0:"};
    for (int i = 0; i < 2; i++)
        if (storage_device_available(DEV[i])) snprintf(g_devices[g_device_count++], 5, "%s", DEV[i]);
    snprintf(g_cache_path, sizeof(g_cache_path), "%s", storage_path("PSP/PSPDX/CACHE/library.txt"));
}

void library_scan_now(void) {
    devices();
    scan();
}

#ifdef __PSP__
/* Below the icons and the sync: the stick is read in the time nothing else
   wants it. */
#define LIBRARY_PRIORITY 0x26
#define LIBRARY_STACK (32 * 1024)
static SceUID g_thread = -1;

static int scan_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    scan();
    g_running = 0;
    return 0;
}

static void start(void) {
    if (g_thread >= 0) {
        sceKernelWaitThreadEnd(g_thread, 0);
        sceKernelDeleteThread(g_thread);
        g_thread = -1;
    }
    devices();
    g_running = 1;
    g_thread = sceKernelCreateThread("library", scan_thread, LIBRARY_PRIORITY, LIBRARY_STACK,
                                     PSP_THREAD_ATTR_USER, 0);
    if (g_thread < 0) {
        logline("library: no thread %08x", (unsigned)g_thread);
        g_running = 0;
        return;
    }
    sceKernelStartThread(g_thread, 0, 0);
}
#else
static void start(void) { library_scan_now(); }
#endif

void library_rescan(void) {
    if (g_running) { g_again = 1; return; }
    start();
}

int library_poll(void) {
    int changed = 0;
    if (g_offered) {
        adopt();
        changed = 1;
    }
    if (g_again && !g_running && !g_offered) {
        g_again = 0;
        start();
    }
    return changed;
}
