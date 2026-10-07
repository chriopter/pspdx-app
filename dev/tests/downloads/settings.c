#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "session/options.h"
static const char *saved;
static char written[96];
static int sort;
int view_sort(void) { return sort; }
void view_sort_set(int s) { sort = s; }
static int actual;
const char *storage_path(const char *p) {
    return p;
}
int storage_read(const char *p, char **s, size_t n) {
    (void)p;
    (void)n;
    *s = saved ? strdup(saved) : NULL;
    return saved ? (int)strlen(saved) : -1;
}
int storage_write(const char *p, const void *s, size_t n) {
    (void)p;
    memcpy(written, s, n);
    written[n] = 0;
    return 0;
}
void gfx_set_fps_cap30(int on) {
    actual = on;
}
/* The switch says why the picture does not follow: counted here. */
static int errors, pending;
static const char *missing;
static char error_text[96];
void error_show(const char *text, const char *tag, ...) {
    (void)tag;
    errors++;
    snprintf(error_text, sizeof(error_text), "%s", text);
}
int downloads_pending_count(void) { return pending; }
const char *gfx_water_missing(void) { return missing; }
int main(void) {
    options_settings_load();
    assert(!actual && !options_fps_requested());
    saved = "garbage";
    options_settings_load();
    assert(!actual);
    saved = "fps=60\n";
    options_settings_load();
    assert(!actual && !options_fps_requested());
    saved = "fps=30\n";
    options_settings_load();
    assert(actual && options_fps_requested());
    options_download_mode(1);
    assert(!actual && options_fps_requested());
    options_download_mode(0);
    assert(actual);
    /* Flipped to Baked while downloads hold the 60 FPS look: the word
       changes, the picture does not, and that is said; flipped back, or
       with nothing downloading, nothing is. A Baked look with a piece
       missing says that. */
    options_fps_toggle_saved();
    assert(!actual && !options_fps_requested() && !errors);
    options_fps_toggle_saved();
    assert(actual && options_fps_requested() && !errors);
    options_download_mode(1);
    pending = 1;
    options_fps_toggle_saved();
    options_fps_toggle_saved();
    assert(!actual && options_fps_requested() && errors == 1 && strstr(error_text, "paused"));
    options_download_mode(0);
    pending = 0;
    assert(actual);
    missing = "ripple, 12 KB free";
    options_fps_toggle_saved();
    options_fps_toggle_saved();
    assert(errors == 2 && strstr(error_text, "could not be loaded"));
    missing = NULL;
    options_download_mode(1);
    written[0] = 0;
    options_fps_toggle_saved();
    assert(!actual && !options_fps_requested());
    /* On the stick at once, not only at a clean exit. */
    assert(!strcmp(written, "fps=60\nfill=1\nsort=new\n"));
    options_download_mode(0);
    assert(!actual);
    options_settings_save();
    assert(!strcmp(written, "fps=60\nfill=1\nsort=new\n"));
    options_fps_runtime_toggle();
    assert(actual);
    options_download_mode(1);
    assert(!actual);
    options_download_mode(0);
    assert(actual);
    options_settings_save();
    assert(!strcmp(written, "fps=60\nfill=1\nsort=new\n"));
    assert(options_fill_cache());
    options_fill_cache_toggle();
    assert(!strcmp(written, "fps=60\nfill=0\nsort=new\n"));
    saved = "fps=30\nfill=0\n";
    options_settings_load();
    assert(actual && !options_fill_cache());
    saved = "fps=60\nfill=1\nsort=new\n";
    options_settings_load();
    assert(!actual && options_fill_cache());
    /* How to connect: no line until it is answered, then on the stick at
       once and read back; a line nobody wrote is no answer. */
    assert(options_cable() == 0);
    options_cable_set(2);
    assert(!strcmp(written, "fps=60\nfill=1\nsort=new\ncable=usb\n"));
    for (int c = 1; c <= 3; c++) {
        options_cable_set(c);
        static char kept[96];
        strcpy(kept, written);
        saved = kept;
        options_cable_set(c == 1 ? 2 : 1);
        options_settings_load();
        assert(options_cable() == c);
    }
    assert(!strcmp(written, "fps=60\nfill=1\nsort=new\ncable=wifi\n"));
    saved = "fps=60\nfill=1\nsort=new\ncable=both\n";
    options_settings_load();
    assert(options_cable() == 0);
    puts("settings: default 60, saved 30 and 60, temporary override, changes during download, "
         "persistence at once and the connection's answer passed");
}
