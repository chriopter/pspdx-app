#include <pspdebug.h>
#include <stdio.h>
#include <string.h>

#include "util/runtime.h"
#include "gui/screen.h"

#define TITLE "PSPDX  "

/* pspDebugScreen has its own printf; format through newlib first so the
   width can come from SCREEN_COLS instead of a literal in every call. */
static void print_padded(const char *text, int width) {
    char line[SCREEN_COLS + 1];
    snprintf(line, sizeof(line), "%-*.*s", width, width, text ? text : "");
    pspDebugScreenPrintf("%s", line);
}

void gui_init(void) { pspDebugScreenInit(); }
void gui_clear(void) { pspDebugScreenClear(); }

static void gui_header(const char *right) {
    pspDebugScreenSetXY(0, 0);
    pspDebugScreenSetTextColor(COL_TEXT);
    pspDebugScreenPrintf("%s", TITLE);
    pspDebugScreenSetTextColor(COL_DIM);
    print_padded(right, SCREEN_COLS - (int)strlen(TITLE));
    pspDebugScreenSetTextColor(COL_TEXT);
}

void gui_failure(void) {
    gui_header("failed");
    for (int i = 0; i < log_count() && i + 2 < STATUS_ROW; i++) {
        pspDebugScreenSetXY(0, 2 + i);
        char line[61];
        log_at(i, line, sizeof(line));
        pspDebugScreenPrintf("%s", line);
    }
}
