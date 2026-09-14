#include <pspiofilemgr.h>
#include <cjson/cJSON.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "util/files.h"
#include "install/install.h"
#include "install/state.h"
#include "text.h"
#include "update/pspdx.h"
#include "update/sources.h"
#include "util/storage.h"

/* The areas, in the order a person asks about them: where apps come from,
   what is installed, what waits, what is only kept for speed, what is only
   there while something is happening, and the rest. */
enum area { A_SOURCES, A_INSTALLED, A_INBOX, A_CACHE, A_PENDING, A_LOGS, A_SEED, A_DEBUG, A_COUNT };

static const struct { const char *name, *note, *dir; } AREA[A_COUNT] = {
    [A_SOURCES]   = { T_AREA_SOURCES,   T_AREA_SOURCES_NOTE,   "PSP/PSPDX/sources.txt" },
    [A_INSTALLED] = { T_AREA_INSTALLED, T_AREA_INSTALLED_NOTE, "PSP/PSPDX/INSTALLED" },
    [A_INBOX]     = { T_AREA_INBOX,     T_AREA_INBOX_NOTE,     "PSP/PSPDX/INBOX" },
    [A_CACHE]     = { T_AREA_CACHE,     T_AREA_CACHE_NOTE,     "PSP/PSPDX/CACHE" },
    [A_PENDING]   = { T_AREA_PENDING,   T_AREA_PENDING_NOTE,   "PSP/PSPDX/TMP" },
    [A_LOGS]      = { T_AREA_LOGS,      T_AREA_LOGS_NOTE,      "PSP/PSPDX/LOGS" },
    [A_SEED]      = { T_AREA_SEED,      T_AREA_SEED_NOTE,      "PSP/PSPDX/CRYPTO" },
    [A_DEBUG]     = { T_AREA_DEBUG,     T_AREA_DEBUG_NOTE,     "PSP/PSPDX/DEBUG" },
};

/* The rows of an area are files, and the file a row stands for is remembered
   beside the view, since the row itself carries only what is read. */
static char g_path[FILES_MAX][64];      /* relative to PSP/PSPDX */
static char g_id[FILES_MAX][96];        /* the app, in the installed area */
static struct sources g_sources;
static const char *(*g_name_of)(const char *id);

void files_names(const char *(*name_of)(const char *id)) { g_name_of = name_of; }
static int g_area_of[FILES_MAX];        /* which area a row on the top stands for, -1 the group */

/* ---------------------------------------------------------------- helpers */

static void full(const char *rel, char *out, size_t size) {
    snprintf(out, size, "%s/%s", storage_path("PSP/PSPDX"), rel);
}

static void size_text(unsigned bytes, char *out, size_t size) {
    if (bytes < 10240) snprintf(out, size, T_FILES_BYTES, bytes);
    else if (bytes < 1024 * 1024) snprintf(out, size, T_FILES_KB, bytes / 1024);
    else snprintf(out, size, T_FILES_MB, bytes >> 20, (unsigned)(((unsigned long long)bytes * 10 >> 20) % 10));
}

static void when_text(unsigned at, char *out, size_t size) {
    time_t t = at;
    struct tm *date = at ? gmtime(&t) : NULL;
    if (date) strftime(out, size, "%Y-%m-%d %H:%M", date);
    else snprintf(out, size, "%s", T_UNKNOWN);
}

static void host_of(const char *url, char *out, size_t size) {
    const char *h = strstr(url, "://");
    h = h ? h + 3 : url;
    size_t n = strcspn(h, "/");
    snprintf(out, size, "%.*s", (int)n, h);
}

/* A line onto the right column. */
static void put(struct file_view *v, const char *fmt, ...) {
    size_t at = strlen(v->text);
    if (at + 2 >= sizeof(v->text)) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v->text + at, sizeof(v->text) - at - 1, fmt, ap);
    va_end(ap);
    strcat(v->text, "\n");
    v->text_lines++;
}

static void clear_text(struct file_view *v) {
    v->text[0] = '\0';
    v->text_first = v->text_lines = v->raw = 0;
    v->image[0] = '\0';
}

/* Where the thing under the cursor is, in full, for the line under the
   title: every row stands for something on the stick. */
static void at_path(struct file_view *v, const char *rel) {
    snprintf(v->path, sizeof(v->path), "%s%s%s", storage_path("PSP/PSPDX"), rel[0] ? "/" : "", rel);
}

/* The files of a directory as rows, sorted by name, folders marked and not
   entered; prefix goes before each name, for a directory listed beside
   another. Returns how many were added. */
static int cmp_rows(const void *a, const void *b) {
    return strcasecmp(((const struct file_row *)a)->name, ((const struct file_row *)b)->name);
}

static int list_dir(struct file_view *v, const char *rel, const char *prefix) {
    char path[256];
    full(rel, path, sizeof(path));
    SceUID d = sceIoDopen(path);
    if (d < 0) return 0;
    SceIoDirent e;
    int from = v->count;
    memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0 && v->count < FILES_MAX) {
        if (strcmp(e.d_name, ".") && strcmp(e.d_name, "..")) {
            struct file_row *r = &v->row[v->count];
            int dir = FIO_S_ISDIR(e.d_stat.st_mode);
            snprintf(r->name, sizeof(r->name), "%s%s%s", prefix, e.d_name, dir ? "/" : "");
            if (dir) r->detail[0] = '\0';
            else size_text((unsigned)e.d_stat.st_size, r->detail, sizeof(r->detail));
            snprintf(g_path[v->count], sizeof(g_path[0]), "%s/%s", rel, e.d_name);
            v->count++;
        }
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
    return v->count - from;
}

static void sort_rows(struct file_view *v, int from) {
    /* The paths ride along with the rows, so the sort is done by hand. */
    for (int i = from + 1; i < v->count; i++)
        for (int j = i; j > from && cmp_rows(&v->row[j - 1], &v->row[j]) > 0; j--) {
            struct file_row t = v->row[j]; v->row[j] = v->row[j - 1]; v->row[j - 1] = t;
            char p[64]; memcpy(p, g_path[j], 64); memcpy(g_path[j], g_path[j - 1], 64); memcpy(g_path[j - 1], p, 64);
        }
}

static int count_dir(const char *rel, unsigned *bytes) {
    char path[256];
    full(rel, path, sizeof(path));
    SceUID d = sceIoDopen(path);
    if (d < 0) return 0;
    SceIoDirent e;
    int n = 0;
    memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0) {
        if (strcmp(e.d_name, ".") && strcmp(e.d_name, "..") && !FIO_S_ISDIR(e.d_stat.st_mode)) {
            n++;
            if (bytes) *bytes += (unsigned)e.d_stat.st_size;
        }
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
    return n;
}

/* A file's bytes onto the right column: the head of it, or the tail for a
   log; laid out when it is JSON and all of it fit; a note when it is not
   text at all. */
static void read_text(struct file_view *v, const char *rel, int tail, int pretty) {
    char path[256];
    full(rel, path, sizeof(path));
    static char raw[FILES_TEXT];
    int fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    if (fd < 0) return;
    int whole = 1;
    if (tail) {
        SceOff size = sceIoLseek(fd, 0, PSP_SEEK_END);
        if (size > (SceOff)(sizeof(raw) - 1)) {
            sceIoLseek(fd, size - (SceOff)(sizeof(raw) - 1), PSP_SEEK_SET);
            whole = 0;
        } else sceIoLseek(fd, 0, PSP_SEEK_SET);
    }
    int n = sceIoRead(fd, raw, sizeof(raw) - 1);
    sceIoClose(fd);
    if (n <= 0) return;
    if (n == (int)sizeof(raw) - 1) whole = 0;
    raw[n] = '\0';
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)raw[i];
        if (c == 0 || (c < 32 && c != '\n' && c != '\r' && c != '\t')) {
            put(v, "%s", T_FILES_BINARY);
            return;
        }
    }
    size_t len = strlen(rel);
    if (pretty && whole && ((len > 5 && !strcasecmp(rel + len - 5, ".json")) ||
                  (len > 6 && !strcasecmp(rel + len - 6, ".pspdx")))) {
        cJSON *j = cJSON_Parse(raw);
        char *pretty = j ? cJSON_Print(j) : NULL;
        cJSON_Delete(j);
        if (pretty) {
            snprintf(raw, sizeof(raw), "%s", pretty);
            free(pretty);
        }
    }
    /* Tabs as two spaces and carriage returns dropped: the font has neither. */
    size_t at = strlen(v->text);
    for (const char *c = raw; *c && at + 2 < sizeof(v->text); c++) {
        if (*c == '\r') continue;
        if (*c == '\t') { v->text[at++] = ' '; v->text[at++] = ' '; continue; }
        if (*c == '\n') v->text_lines++;
        v->text[at++] = *c;
    }
    v->text[at] = '\0';
    v->text_lines++;
    v->raw = 1;
}

/* ------------------------------------------------------------- the areas */

/* The rows of the top: the three a person came for, then everything PSPDX
   keeps for itself as one row, since five rows of housekeeping would bury
   the three. Inside the group the five stand as rows of their own. */
#define A_SYSTEM_FIRST A_CACHE

static void area_row(struct file_view *v, int a) {
    struct file_row *r = &v->row[v->count];
    g_area_of[v->count++] = a;
    snprintf(r->name, sizeof(r->name), "%s", AREA[a].name);
    unsigned bytes = 0;
    int n;
    switch (a) {
        case A_SOURCES:
            snprintf(r->detail, sizeof(r->detail), T_FILES_N, sources_load(&g_sources));
            break;
        case A_INSTALLED:
            snprintf(r->detail, sizeof(r->detail), T_FILES_N, state_count());
            break;
        case A_CACHE:
            count_dir("CACHE/catalogs", &bytes);
            count_dir("CACHE/media", &bytes);
            size_text(bytes, r->detail, sizeof(r->detail));
            break;
        case A_PENDING:
            n = count_dir("TMP", NULL);
            if (n) snprintf(r->detail, sizeof(r->detail), T_FILES_N, n);
            else snprintf(r->detail, sizeof(r->detail), "%s", T_FILES_NONE);
            break;
        case A_SEED:
            snprintf(r->detail, sizeof(r->detail), "%s", storage_exists(storage_path("PSP/PSPDX/CRYPTO/seed.bin")) ? "" : T_FILES_NONE);
            break;
        default:
            snprintf(r->detail, sizeof(r->detail), T_FILES_N, count_dir(AREA[a].dir + 10, NULL));
            break;
    }
}

static void fill_areas(struct file_view *v) {
    v->count = 0;
    if (v->group) {
        for (int a = A_SYSTEM_FIRST; a < A_COUNT; a++) area_row(v, a);
        return;
    }
    for (int a = 0; a < A_SYSTEM_FIRST; a++) area_row(v, a);
    struct file_row *r = &v->row[v->count];
    g_area_of[v->count++] = -1;
    snprintf(r->name, sizeof(r->name), "%s", T_AREA_SYSTEM);
    r->detail[0] = '\0';
}

/* What the right column says for the area under the cursor, before it is
   entered: what it is for, and a glance at what is in it. */
static void describe_area(struct file_view *v) {
    int a = g_area_of[v->cursor];
    if (a < 0) {
        snprintf(v->head, sizeof(v->head), "%s", T_AREA_SYSTEM);
        snprintf(v->note, sizeof(v->note), "%s", T_AREA_SYSTEM_NOTE);
        clear_text(v);
        at_path(v, "");
        for (int i = A_SYSTEM_FIRST; i < A_COUNT; i++) put(v, "%s", AREA[i].name);
        return;
    }
    snprintf(v->head, sizeof(v->head), "%s", AREA[a].name);
    snprintf(v->note, sizeof(v->note), "%s", AREA[a].note);
    clear_text(v);
    at_path(v, AREA[a].dir + 10);
    char size[24];
    unsigned bytes;
    int n;
    switch (a) {
    case A_SOURCES:
        for (int i = 0; i < g_sources.count; i++) {
            char host[48];
            host_of(g_sources.url[i], host, sizeof(host));
            put(v, "%s", host);
        }
        break;
    case A_INSTALLED:
        for (int i = 0; i < state_count(); i++) {
            const char *id = state_id(i);
            struct pspdx_file f;
            char *raw = NULL;
            struct installed rec;
            int have = id && state_read_manifest(id, &raw, &f) >= 0;
            const char *name = have ? f.name : id ? id : "";
            if (id && db_read(id, &rec) == 0) put(v, "%s  %s", name, rec.version);
            else put(v, "%s", name);
            free(raw);
        }
        break;
    case A_CACHE:
        bytes = 0; n = count_dir("CACHE/catalogs", &bytes); size_text(bytes, size, sizeof(size));
        put(v, "%s: %d, %s", T_KIND_CATALOG, n, size);
        bytes = 0; n = count_dir("CACHE/media", &bytes); size_text(bytes, size, sizeof(size));
        put(v, "Previews: %d, %s", n, size);
        break;
    default: {
        struct file_view peek;
        peek.count = 0;
        char save[FILES_MAX][64];
        memcpy(save, g_path, sizeof(save));
        list_dir(&peek, AREA[a].dir + 10, "");
        memcpy(g_path, save, sizeof(save));
        if (peek.count == 0) put(v, "%s", T_FILES_EMPTY);
        for (int i = 0; i < peek.count; i++) put(v, "%s", peek.row[i].name);
        break;
    }
    }
}

/* ------------------------------------------------------------ inside one */

static void fill_installed(struct file_view *v) {
    v->count = 0;
    for (int i = 0; i < state_count() && v->count < FILES_MAX; i++) {
        const char *id = state_id(i);
        if (!id) continue;
        struct file_row *r = &v->row[v->count];
        struct pspdx_file f;
        char *raw = NULL;
        struct installed rec;
        if (state_read_manifest(id, &raw, &f) >= 0) snprintf(r->name, sizeof(r->name), "%s", f.name);
        else snprintf(r->name, sizeof(r->name), "%s", id);
        free(raw);
        if (db_read(id, &rec) == 0) snprintf(r->detail, sizeof(r->detail), "%s", rec.version);
        else r->detail[0] = '\0';
        snprintf(g_id[v->count], sizeof(g_id[0]), "%s", id);
        v->count++;
    }
}

static void describe_app(struct file_view *v) {
    const char *id = g_id[v->cursor];
    struct pspdx_file f;
    char *raw = NULL;
    struct installed rec;
    struct manifest latest;
    int have = state_read_manifest(id, &raw, &f) >= 0;
    snprintf(v->head, sizeof(v->head), "%s", have ? f.name : id);
    snprintf(v->note, sizeof(v->note), "%s", have && f.summary[0] ? f.summary : "");
    clear_text(v);
    char rel[128];
    snprintf(rel, sizeof(rel), "INSTALLED/%s.*", id);
    at_path(v, rel);
    if (db_read(id, &rec) == 0) {
        put(v, T_APP_INSTALLED, rec.version);
        put(v, T_APP_FOLDER, rec.dir);
        put(v, T_APP_SOURCE, rec.repo);
    }
    if (state_latest(id, &latest) == 0) {
        char when[24], host[48];
        put(v, T_APP_LATEST, latest.version);
        when_text(latest.checked_at, when, sizeof(when));
        put(v, T_APP_CHECKED, when);
        if (latest.checked_from[0]) {
            host_of(latest.checked_from, host, sizeof(host));
            put(v, T_APP_VIA, host);
        }
    }
    free(raw);
}

static void fill_sources(struct file_view *v) {
    v->count = 0;
    sources_load(&g_sources);
    for (int i = 0; i < g_sources.count && v->count < FILES_MAX; i++) {
        struct file_row *r = &v->row[v->count];
        char host[48];
        host_of(g_sources.url[i], host, sizeof(host));
        snprintf(r->name, sizeof(r->name), "%s", host);
        enum source_kind k = sources_kind(g_sources.url[i]);
        snprintf(r->detail, sizeof(r->detail), "%s",
                 k == SOURCE_REPO ? T_KIND_REPO : k == SOURCE_LIST ? T_KIND_LIST : T_KIND_CATALOG);
        v->count++;
    }
}

static void describe_source(struct file_view *v) {
    const char *url = g_sources.url[v->cursor];
    host_of(url, v->head, sizeof(v->head));
    snprintf(v->note, sizeof(v->note), "%s", v->row[v->cursor].detail);
    clear_text(v);
    at_path(v, "sources.txt");
    put(v, "%s", url);
    v->raw = 1;
}

/* A cached file is named by its app and its kind -- <id>-icon-<hash>.png --
   and the row says it that way round: Icon of Lux Aeterna. The app's name
   comes from its record where it is installed; otherwise the id has to do. */
static void name_cached(struct file_row *r, const char *file) {
    const char *kinds[] = { "-icon-", "-picture-", "-film-", "-sound-" };
    const char *words[] = { T_CACHE_ICON, T_CACHE_PICTURE, T_CACHE_FILM, T_CACHE_SOUND };
    for (int k = 0; k < 4; k++) {
        const char *at = strstr(file, kinds[k]);
        if (!at) continue;
        char id[96], app[64];
        snprintf(id, sizeof(id), "%.*s", (int)(at - file), file);
        struct pspdx_file f;
        char *raw = NULL;
        const char *known = g_name_of ? g_name_of(id) : NULL;
        if (state_read_manifest(id, &raw, &f) >= 0) snprintf(app, sizeof(app), "%s", f.name);
        else if (known && known[0]) snprintf(app, sizeof(app), "%s", known);
        else {
            const char *dot = strrchr(id, '.');
            snprintf(app, sizeof(app), "%s", dot ? dot + 1 : id);
        }
        free(raw);
        snprintf(r->name, sizeof(r->name), words[k], app);
        return;
    }
    size_t len = strlen(file);
    if (len > 5 && !strcasecmp(file + len - 5, ".json"))
        snprintf(r->name, sizeof(r->name), "%s", T_CACHE_CATALOG);
}

static void fill_files(struct file_view *v, int area) {
    v->count = 0;
    if (area == A_CACHE) {
        int from = v->count;
        list_dir(v, "CACHE/catalogs", "");
        list_dir(v, "CACHE/media", "");
        for (int i = from; i < v->count; i++) name_cached(&v->row[i], v->row[i].name);
    } else {
        list_dir(v, AREA[area].dir + 10, "");
    }
    sort_rows(v, 0);
}

static void describe_file(struct file_view *v) {
    const struct file_row *r = &v->row[v->cursor];
    const char *slash = strrchr(r->name, '/');
    snprintf(v->head, sizeof(v->head), "%s", slash && slash[1] ? slash + 1 : r->name);
    snprintf(v->note, sizeof(v->note), T_FILE_NOTE, r->detail[0] ? r->detail : "", AREA[v->area].name);
    clear_text(v);
    at_path(v, g_path[v->cursor]);
    if (r->name[strlen(r->name) - 1] == '/') return;
    if (v->area == A_SEED) { put(v, "%s", T_AREA_SEED_NOTE); return; }
    /* A picture is shown as one: the icons and stills the cache holds, the
       screenshots the developer's folder does. */
    const char *file = g_path[v->cursor];
    size_t len = strlen(file);
    if (len > 4 && !strcasecmp(file + len - 4, ".png")) {
        snprintf(v->image, sizeof(v->image), "%s", v->path);
        return;
    }
    read_text(v, g_path[v->cursor], v->area == A_LOGS, 1);
}

/* ------------------------------------------------------------- the moves */

static int listing_areas(const struct file_view *v) {
    return v->level == 0 || (v->group && v->level == 1);
}

static void describe(struct file_view *v) {
    v->deeper = v->count > 0 && (listing_areas(v) || v->row[v->cursor].name[strlen(v->row[v->cursor].name) - 1] != '/');
    if (v->count == 0) {
        snprintf(v->head, sizeof(v->head), "%s", AREA[v->area].name);
        snprintf(v->note, sizeof(v->note), "%s", AREA[v->area].note);
        clear_text(v);
        at_path(v, AREA[v->area].dir + 10);
        put(v, "%s", T_FILES_EMPTY);
        return;
    }
    if (listing_areas(v)) describe_area(v);
    else if (v->area == A_INSTALLED) describe_app(v);
    else if (v->area == A_SOURCES) describe_source(v);
    else describe_file(v);
}

static void settle(struct file_view *v) {
    if (v->cursor < v->first) v->first = v->cursor;
    if (v->cursor >= v->first + FILES_ROWS) v->first = v->cursor - FILES_ROWS + 1;
    describe(v);
}

void files_open(struct file_view *v) {
    memset(v, 0, sizeof(*v));
    fill_areas(v);
    settle(v);
}

void files_move(struct file_view *v, int by) {
    if (v->count == 0 || v->band || (v->level == 2 && !v->group)) return;
    v->cursor = (v->cursor + by + v->count) % v->count;
    settle(v);
}

void files_scroll(struct file_view *v, int lines) {
    int last = v->text_lines - 1;
    v->text_first += lines;
    if (v->text_first > last) v->text_first = last;
    if (v->text_first < 0) v->text_first = 0;
}

int files_enter(struct file_view *v) {
    if (v->count == 0) return 0;
    if (listing_areas(v)) {
        int a = g_area_of[v->cursor];
        if (a < 0) {
            /* Into the group: its areas as rows, one level down. */
            v->group = 1;
            v->level = 1;
            v->cursor = v->first = 0;
            fill_areas(v);
            settle(v);
            return 1;
        }
        v->area = a;
        v->level = v->group ? 2 : 1;
        v->cursor = v->first = 0;
        if (v->area == A_INSTALLED) fill_installed(v);
        else if (v->area == A_SOURCES) fill_sources(v);
        else fill_files(v, v->area);
        settle(v);
        return 1;
    }
    if (v->level == 1 && v->area == A_INSTALLED) {
        /* The two files themselves, laid out, in the band. */
        const char *id = g_id[v->cursor];
        char rel[128];
        clear_text(v);
        snprintf(rel, sizeof(rel), "INSTALLED/%s.*", id);
        at_path(v, rel);
        snprintf(rel, sizeof(rel), "INSTALLED/%s.pspdx", id);
        put(v, "%s.pspdx", id);
        read_text(v, rel, 0, 1);
        snprintf(rel, sizeof(rel), "INSTALLED/%s.state.json", id);
        put(v, "");
        put(v, "%s.state.json", id);
        read_text(v, rel, 0, 1);
        v->raw = 1;
        v->band = 1;
        return 1;
    }
    /* Anything else that has bytes: those, as they are. */
    return files_raw(v);
    return 0;
}

int files_raw(struct file_view *v) {
    if (v->count == 0 || listing_areas(v)) return 0;
    char rel[128];
    const char *id = g_id[v->cursor];
    if (v->level == 1 && v->area == A_INSTALLED) {
        clear_text(v);
        snprintf(rel, sizeof(rel), "INSTALLED/%s.*", id);
        at_path(v, rel);
        snprintf(rel, sizeof(rel), "INSTALLED/%s.pspdx", id);
        put(v, "%s.pspdx", id);
        read_text(v, rel, 0, 0);
        snprintf(rel, sizeof(rel), "INSTALLED/%s.state.json", id);
        put(v, "");
        put(v, "%s.state.json", id);
        read_text(v, rel, 0, 0);
    } else if (v->area == A_SOURCES) {
        clear_text(v);
        at_path(v, "sources.txt");
        read_text(v, "sources.txt", 0, 0);
    } else if (listing_areas(v)) {
        return 0;
    } else {
        const struct file_row *r = &v->row[v->cursor];
        if (r->name[strlen(r->name) - 1] == '/') return 0;
        clear_text(v);
        at_path(v, g_path[v->cursor]);
        const char *file = g_path[v->cursor];
        size_t len = strlen(file);
        if (v->area == A_SEED) put(v, "%s", T_AREA_SEED_NOTE);
        else if (len > 4 && !strcasecmp(file + len - 4, ".png"))
            snprintf(v->image, sizeof(v->image), "%s", v->path);
        else read_text(v, g_path[v->cursor], 0, 0);
    }
    v->raw = 1;
    v->band = 1;
    return 1;
}

int files_back(struct file_view *v) {
    if (v->band) {
        /* The band down, the columns as they were. */
        v->band = 0;
        settle(v);
        return 1;
    }
    if (v->level == 2 && v->group) {
        /* Out of a system area, onto its row in the group. */
        int was = v->area;
        v->level = 1;
        fill_areas(v);
        v->cursor = was - A_SYSTEM_FIRST;
        v->first = 0;
        settle(v);
        return 1;
    }
    if (v->level == 2) {
        v->level = 1;
        settle(v);
        return 1;
    }
    if (v->level == 1) {
        int was = v->group ? A_SYSTEM_FIRST : v->area;
        v->level = 0;
        v->group = 0;
        fill_areas(v);
        v->cursor = was;
        v->first = 0;
        settle(v);
        return 1;
    }
    return 0;
}
