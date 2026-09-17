#include "text.h"
/*
 * Manage Files, drawn: the rows util/files.c reads off the stick, in two
 * columns where the list and the card otherwise stand, and a file's raw
 * bytes in a band over them. The model is files.c's and the room is
 * shell.c's; what is here is the drawing, the keys while the browser is
 * up, and the hand-off of a film or a sound to the media thread.
 */

#include <pspctrl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/cues.h"
#include "gui/files_view.h"
#include "gui/font.h"
#include "gui/gfx.h"
#include "gui/image.h"
#include "gui/marks.h"
#include "gui/preview.h"
#include "gui/shell_internal.h"
#include "util/storage.h"

/* Manage Files: the view util/files.c fills out of the stick, and whether
   it is on screen. */
static struct file_view g_view;
static const struct file_view *g_files;   /* the file browser, while it is open */

/* The file the media thread has been asked for, while the browser is up. */
static char g_file_shown[FILES_PATH];

void files_view_open(void) {
    files_open(&g_view);
    g_files = &g_view;
}

int files_view_shown(void) { return g_files != NULL; }

int files_view_action(void) { return g_files && files_action(g_files); }

void files_view_refresh(void) {
    if (!g_files) return;
    /* Back at the top, on the action row: what was cleared is the top's
       to show, in the sizes at the rows' ends. */
    files_open(&g_view);
}

void files_view_close(void) {
    if (!g_files) return;
    g_files = NULL;
    /* The browser closing gives the card back its entry, at once. */
    if (g_file_shown[0]) { g_file_shown[0] = '\0'; shell_reshow_card(); }
}

/* Manage Files. Left, the rows of the level the browser is on, one short
   row each with a word on the right saying how much is there; right, the
   row under the cursor: what it is, and then its lines. A file's own bytes
   are laid out at a fixed width, since a record is one long line of JSON
   with nothing to break at; everything else is a line a row. */
#define FILE_ROW_H 26
/* The action at the foot of the areas stands a step under them, a line of
   the room's light in the step, the way Sources parts its jobs from the
   user's list. */
#define FILE_STEP 10
static void draw_raw_band(void);
#define FILE_TEXT_COLS 40
#define FILE_TEXT_STEP 16

static int shell_files_page(void) {
    int y = LIST_Y + 12 + 36;
    return (FOOTER_Y - 8 - y) / FILE_TEXT_STEP;
}

/* How many lines the column and the band held at the last draw, for the
   scrolling to stop where the last of the text is in view. */
static int g_files_room, g_band_room;

/* A picture out of a file, decoded once and kept until another is asked
   for, fitted into the space given. */
static struct gfx_texture g_picture;
static char g_picture_of[FILES_PATH];

static void picture_drop(void) {
    if (g_picture_of[0]) {
        gfx_texture_free(&g_picture);
        g_picture_of[0] = '\0';
    }
}

static void draw_picture_file(const char *file, int x, int y, int maxw, int maxh);

/* The file under the cursor as what it is: a picture decoded here, a film
   the media thread plays into the card's own texture, a sound heard and
   said to be. */
static void draw_media(const struct file_view *v, int x, int y, int maxw, int maxh) {
    if (v->media_kind == FILE_MEDIA_PICTURE) { draw_picture_file(v->media, x, y, maxw, maxh); return; }
    picture_drop();
    if (v->media_kind == FILE_MEDIA_SOUND) { font_print(FONT_META, x, y + 12, g_dim, T_FILES_PLAYING); return; }
    int alpha;
    const struct gfx_texture *film = preview_film(&alpha);
    if (film && film->w > 0 && film->h > 0) {
        int pw = film->w, ph = film->h;
        if (pw > maxw) { ph = ph * maxw / pw; pw = maxw; }
        if (ph > maxh) { pw = pw * maxh / ph; ph = maxh; }
        gfx_texture_draw(film, x, y, pw, ph, RGB(255, 255, 255));
    } else {
        font_print(FONT_META, x, y + 12, g_dim, preview_film_failed() ? T_FILES_BINARY : T_FILES_LOADING);
    }
}

static void draw_picture_file(const char *file, int x, int y, int maxw, int maxh) {
    if (strcmp(g_picture_of, file)) {
        picture_drop();
        char *png = NULL;
        int n = storage_read(file, &png, 512 * 1024);
        if (n > 0 && image_decode_png(png, (size_t)n, &g_picture) == 0)
            snprintf(g_picture_of, sizeof(g_picture_of), "%s", file);
        free(png);
    }
    if (g_picture_of[0] && g_picture.w > 0 && g_picture.h > 0) {
        int pw = g_picture.w, ph = g_picture.h;
        if (pw > maxw) { ph = ph * maxw / pw; pw = maxw; }
        if (ph > maxh) { pw = pw * maxh / ph; ph = maxh; }
        gfx_texture_draw(&g_picture, x, y, pw, ph, RGB(255, 255, 255));
    } else {
        font_print(FONT_META, x, y, g_dim, T_FILES_BINARY);
    }
}

void files_view_draw(float t) {
    const struct file_view *v = g_files;
    int top = LIST_Y + 4;
    draw_shade(LIST_X + LIST_W / 2, top + FILES_ROWS * FILE_ROW_H / 2, LIST_W,
               FILES_ROWS * FILE_ROW_H);
    if (v->count == 0)
        font_print(FONT_META, LIST_X, top + 16, g_dim, T_FILES_EMPTY);
    for (int i = v->first; i < v->count && i < v->first + FILES_ROWS; i++) {
        int y = top + (i - v->first) * FILE_ROW_H;
        const struct file_row *r = &v->row[i];
        int selected = i == v->cursor;
        if (files_row_is_action(v, i)) {
            y += FILE_STEP;
            unsigned faint = rgb_pack(rgb_mix(g_tint, RGB_WHITE, 0.5f), 150);
            unsigned clear = rgb_pack(g_tint, 0);
            int cx = LIST_X + LIST_W / 2, half = LIST_W / 2, ry = y - FILE_STEP / 2 - 1;
            gfx_hgrad(cx - half, ry, half, 1, clear, faint);
            gfx_hgrad(cx, ry, half, 1, faint, clear);
        }
        if (selected) {
            float breathe = 0.85f + 0.15f * sinf(t * 2.2f);
            gfx_glow(LIST_X + 60, y + FILE_ROW_H / 2, LIST_W + 120, FILE_ROW_H * 2.2f,
                     rgb_pack(g_tint, (int)(120 * breathe)));
            gfx_glow(LIST_X + LIST_W / 2, y + FILE_ROW_H - 3, LIST_W + 30, 8,
                     rgb_pack(rgb_mix(g_tint, RGB_WHITE, 0.7f), 140));
        }
        float dw = r->detail[0] ? font_width(FONT_META, r->detail) : 0.0f;
        font_print_scrolling(FONT_TITLE, LIST_X, y + 18, LIST_W - dw - 14,
                             selected ? g_text : g_dim, r->name,
                             selected ? hover_age(1, v->level * 1000 + i) : 0.0f);
        if (r->detail[0])
            font_print(FONT_META, LIST_X + LIST_W - dw, y + 18, faded(g_dim, 200), r->detail);
    }
    if (v->count > FILES_ROWS) {
        int track = FILES_ROWS * FILE_ROW_H, knob = track * FILES_ROWS / v->count;
        int at = track * v->first / v->count;
        gfx_rect(LIST_X + LIST_W + 10, top, 2, track, RGBA(255, 255, 255, 24));
        gfx_rect(LIST_X + LIST_W + 10, top + at, 2, knob, g_accent);
    }

    int x = PANEL_X, w = SCR_W - PANEL_X - 12, y = LIST_Y + 12;
    draw_shade(x + w / 2, (y + FOOTER_Y) / 2, w, FOOTER_Y - y);
    font_print_clipped(FONT_H1, x, y, w, g_text, v->head);
    /* The path in full, broken at a slash when it is longer than the
       column, since a path has no space to break at. */
    int path_lines = 1;
    {
        const char *p = v->path;
        char first[FILES_PATH];
        snprintf(first, sizeof(first), "%s", p);
        if (font_width(FONT_META, first) > w) {
            int cut = -1;
            for (int i = 0; first[i]; i++) {
                if (first[i] != '/') continue;
                char head[128];
                snprintf(head, sizeof(head), "%.*s", i + 1, first);
                if (font_width(FONT_META, head) <= w) cut = i + 1;
            }
            if (cut > 0) {
                char head[128];
                snprintf(head, sizeof(head), "%.*s", cut, first);
                font_print(FONT_META, x, y + 16, faded(g_dim, 170), head);
                font_print_clipped(FONT_META, x, y + 32, w, faded(g_dim, 170), first + cut);
                path_lines = 2;
            } else {
                font_print_clipped(FONT_META, x, y + 16, w, faded(g_dim, 170), first);
            }
        } else {
            font_print(FONT_META, x, y + 16, faded(g_dim, 170), first);
        }
    }
    int note_y = y + 16 + 16 * path_lines + 4;
    int lines = draw_wrapped(FONT_META, x, note_y, w, 17, 2, g_dim, v->note);
    y = note_y + 17 * lines + 8;
    int room = (FOOTER_Y - 8 - y) / FILE_TEXT_STEP;
    g_files_room = room;
    if (v->media_kind) draw_media(v, x, y, w, FOOTER_Y - 8 - y);
    else picture_drop();
    /* The lines, from the one the scrolling is at; a raw file is cut at a
       fixed width so a long line becomes several. Under the band the column
       is dimmed and its lines are the band's: printed once, not twice. */
    const char *p = v->band ? "" : v->text;
    int line = 0;
    while (*p && line < v->text_first + room) {
        const char *end = strchr(p, '\n');
        int len = end ? (int)(end - p) : (int)strlen(p);
        do {
            int take = v->raw && len > FILE_TEXT_COLS ? FILE_TEXT_COLS : len;
            if (line >= v->text_first) {
                char chunk[FILE_TEXT_COLS + 1];
                if (!v->raw && take > FILE_TEXT_COLS) take = FILE_TEXT_COLS;
                memcpy(chunk, p, (size_t)take);
                chunk[take] = '\0';
                if (v->raw)
                    font_print(FONT_META, x, y + (line - v->text_first) * FILE_TEXT_STEP,
                               g_text, chunk);
                else
                    font_print_clipped(FONT_META, x, y + (line - v->text_first) * FILE_TEXT_STEP,
                                       w, g_text, chunk);
                if (!v->raw) { p += len; len = 0; take = len; }
            }
            p += take;
            len -= take;
            line++;
        } while (len > 0 && line < v->text_first + room);
        if (end && *p == '\n') p++;
        else if (!end) break;
    }
    /* The keys at the foot: into a row, back out, and the page keys over
       the lines when there are more than fit. */
    float hx = LIST_X;
    if (files_action(v)) hx = draw_hint(hx, FOOTER_BASE, MARK_CROSS, T_HINT_CLEAR, g_dim);
    else if (v->deeper) hx = draw_hint(hx, FOOTER_BASE, MARK_CROSS, T_HINT_OPEN, g_dim);
    if (v->band) { draw_raw_band(); return; }
    hx = draw_hint(hx, FOOTER_BASE, MARK_CIRCLE, T_HINT_BACK, g_dim);
    if (files_rows(v, FILE_TEXT_COLS) > room) draw_hint(hx, FOOTER_BASE, MARK_L, T_HINT_SCROLL, g_dim);
}

/* The raw bytes of a file, in a band over the dimmed columns like the
   Information band: as many fixed-width lines as fit, from the one the
   stick has scrolled to. */
#define RAW_COLS 66
#define RAW_STEP 15

static void draw_raw_band(void) {
    const struct file_view *v = g_files;
    draw_band(INFO_Y, INFO_H);
    int x = 40, w = SCR_W - 80, y = INFO_Y + 14;
    font_print_clipped(FONT_TITLE, x, y + 12, w, g_text, v->head);
    font_print_clipped(FONT_META, x, y + 27, w, faded(g_dim, 170), v->path);
    band_rule(y + 34, 200, 120);
    y += 44;
    int room = (INFO_Y + INFO_H - 22 - y) / RAW_STEP;
    g_band_room = room;
    if (v->media_kind) {
        draw_media(v, x, y, w, INFO_Y + INFO_H - 22 - y);
        float bw = hint_width(MARK_CIRCLE, T_HINT_BACK);
        draw_hint(SCR_W / 2 - bw / 2, INFO_Y + INFO_H - 8, MARK_CIRCLE, T_HINT_BACK, g_dim);
        return;
    }
    const char *p = v->text;
    int line = 0;
    while (*p && line < v->text_first + room) {
        const char *end = strchr(p, '\n');
        int len = end ? (int)(end - p) : (int)strlen(p);
        do {
            int take = len > RAW_COLS ? RAW_COLS : len;
            if (line >= v->text_first) {
                char chunk[RAW_COLS + 1];
                memcpy(chunk, p, (size_t)take);
                chunk[take] = '\0';
                font_print(FONT_META, x, y + (line - v->text_first) * RAW_STEP, g_text, chunk);
            }
            p += take;
            len -= take;
            line++;
        } while (len > 0 && line < v->text_first + room);
        if (end && *p == '\n') p++;
        else if (!end) break;
    }
    float hw = hint_width(MARK_CIRCLE, T_HINT_BACK);
    draw_hint(SCR_W / 2 - hw / 2, INFO_Y + INFO_H - 8, MARK_CIRCLE, T_HINT_BACK, g_dim);
}

/* The lines scrolled by: the model is told how many rows the text comes to
   where it is shown now (column or band) and how many of them the last
   draw held. */
static void scroll(struct file_view *v, int lines) {
    int rows = files_rows(v, v->band ? RAW_COLS : FILE_TEXT_COLS);
    files_scroll(v, lines, rows, v->band ? g_band_room : g_files_room);
}

/* The media thread's frame while the browser is up. Returns 0 while it
   is not, and the card's own entry is the shell's to sync. */
int files_view_sync(void) {
    if (!g_files) return 0;
    /* The browser's film or sound takes the card's place on the media
       thread; a row that is neither leaves it showing nothing. */
    if (g_files->media_kind >= FILE_MEDIA_FILM) {
        if (strcmp(g_file_shown, g_files->media)) {
            snprintf(g_file_shown, sizeof(g_file_shown), "%s", g_files->media);
            preview_show_file(g_files->media, g_files->media_kind == FILE_MEDIA_FILM
                              ? PREVIEW_FILE_FILM : PREVIEW_FILE_SOUND);
        }
    } else if (g_file_shown[0]) {
        g_file_shown[0] = '\0';
        preview_show(NULL, 1);
    }
    preview_tick();
    return 1;
}

/* The keys while the browser is up: up and down the rows, X into the one
   under the cursor, O a level up and, at the top, out; the triggers page
   the lines. */
void files_view_keys(unsigned pressed, const SceCtrlData *pad) {
    struct file_view *v = &g_view;
    if ((pressed & PSP_CTRL_DOWN) && v->count) { files_move(v, 1); cues_post(CUE_MOVE, v->cursor); }
    else if ((pressed & PSP_CTRL_UP) && v->count) { files_move(v, -1); cues_post(CUE_MOVE, v->cursor); }
    else if ((pressed & PSP_CTRL_CROSS) && v->count) { if (files_enter(v)) cues_post(CUE_OPEN, 0); }
    else if (pressed & PSP_CTRL_CIRCLE) {
        if (!files_back(v)) files_view_close();
    }
    else if (pressed & PSP_CTRL_RTRIGGER) scroll(v, shell_files_page());
    else if (pressed & PSP_CTRL_LTRIGGER) scroll(v, -shell_files_page());
    /* The stick scrolls the band, a line every few frames the further
       it is pushed; the columns under it are not moved by it. */
    if (v->band) {
        static int tick;
        int push = (int)pad->Ly - 128;
        if (push > 40 || push < -40) {
            int every = push > 100 || push < -100 ? 1 : 3;
            if (++tick >= every) { scroll(v, push > 0 ? 1 : -1); tick = 0; }
        } else tick = 0;
    }
}
