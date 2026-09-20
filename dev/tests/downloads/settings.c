#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "session/options.h"
static const char *saved;
static char written[16];
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
int main(void) {
    options_settings_load();
    assert(actual && options_fps_requested());
    saved = "garbage";
    options_settings_load();
    assert(actual);
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
    options_download_mode(1);
    options_fps_toggle_saved();
    assert(!actual && !options_fps_requested());
    options_download_mode(0);
    assert(!actual);
    options_settings_save();
    assert(!strcmp(written, "fps=60\n"));
    options_fps_runtime_toggle();
    assert(actual);
    options_download_mode(1);
    assert(!actual);
    options_download_mode(0);
    assert(actual);
    options_settings_save();
    assert(!strcmp(written, "fps=60\n"));
    puts("settings: default 30, saved 60, temporary override, changes during download and "
         "persistence passed");
}
