#define _DEFAULT_SOURCE
#include <assert.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "pspkernel.h"
#include "pspctrl.h"
#include "session/downloads.h"
#include "session/view.h"
#include "gui/shell.h"

static struct catalog catalog;
static pthread_t thread;
static int (*entry)(SceSize, void *);
static sem_t answer;
static atomic_int aborted, hold_prepare, prepare_entered, in_install, hold_install;
static int completed, last_rc, forced, paused, pause_held, order[16], order_n;
static int reject_folder, fail_start, snapshot_ok;
static void *start(void *unused) {
    (void)unused;
    entry(0, NULL);
    return NULL;
}
SceUID sceKernelCreateThread(const char *n, int (*f)(SceSize, void *), int p, int st, int a,
                             void *o) {
    (void)n;
    (void)p;
    (void)st;
    (void)a;
    (void)o;
    entry = f;
    return 1;
}
int sceKernelStartThread(SceUID t, SceSize s, void *a) {
    (void)t;
    (void)s;
    (void)a;
    return fail_start ? -1 : pthread_create(&thread, NULL, start, NULL);
}
int sceKernelWaitThreadEnd(SceUID t, void *x) {
    (void)t;
    (void)x;
    return pthread_join(thread, NULL);
}
int sceKernelDeleteThread(SceUID t) {
    (void)t;
    return 0;
}
SceUID sceKernelCreateSema(const char *n, int a, int initial, int max, void *o) {
    (void)n;
    (void)a;
    (void)max;
    (void)o;
    sem_init(&answer, 0, initial);
    return 2;
}
int sceKernelWaitSema(SceUID s, int n, void *o) {
    (void)s;
    (void)n;
    (void)o;
    return sem_wait(&answer);
}
int sceKernelSignalSema(SceUID s, int n) {
    (void)s;
    (void)n;
    return sem_post(&answer);
}
int sceKernelDeleteSema(SceUID s) {
    (void)s;
    return sem_destroy(&answer);
}
int sceKernelDelayThread(unsigned us) {
    usleep(us);
    return 0;
}
unsigned now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}
struct catalog *actions_catalog(void) {
    return &catalog;
}
int actions_download_device(const struct app_entry *e, char d[5]) {
    (void)e;
    memcpy(d, "ef0:", 5);
    return 0;
}
int actions_download_connect(void) {
    return 1;
}
int actions_download_folder(const struct app_entry *e, const char *d, int r) {
    (void)e;
    (void)d;
    (void)r;
    return reject_folder ? INSTALL_CANCELLED : 0;
}
void actions_download_complete(int i, struct app_entry *e, const struct install_report *r, int rc,
                               unsigned sec) {
    (void)e;
    (void)r;
    (void)sec;
    completed++;
    last_rc = rc;
    order[order_n++] = i;
}
void entry_clear(struct app_entry *e) {
    free(e->description);
    free(e->release.raw);
    memset(e, 0, sizeof(*e));
}
void options_download_mode(int on) {
    forced = on;
}
void audio_pause(int on) {
    paused = on;
}
void player_pause(int on) {
    (void)on;
}
void preview_pause_begin(void) {
    assert(!pause_held);
    pause_held = 1;
}
int preview_pause_ready(void) {
    return 1;
}
void preview_resume(void) {
    assert(pause_held);
    pause_held = 0;
}
void shell_status(const char *s) {
    (void)s;
}
void shell_download_draw(const struct app_entry *e, const struct download_status *s) {
    (void)e;
    (void)s;
}
int inbox_count(void) {
    return 0;
}
int inbox_index(int i) {
    (void)i;
    return -1;
}
void inbox_installed(int i) {
    (void)i;
}
int catalog_prepare(struct app_entry *e) {
    atomic_store(&prepare_entered, 1);
    while (atomic_load(&hold_prepare))
        usleep(1000);
    if (snapshot_ok)
        assert(!strcmp(e->description, "original") && !strcmp(e->release.raw, "manifest"));
    return 0;
}
void install_abort(void) {
    atomic_store(&aborted, 1);
}
int install_release_to(const struct manifest *m, const char *d, struct install_report *r,
                       install_phase_cb phase, https_progress progress, void *ctx) {
    (void)m;
    (void)d;
    (void)r;
    assert(atomic_fetch_add(&in_install, 1) == 0);
    atomic_store(&aborted, 0);
    phase(ctx, "download");
    for (int n = 1; n <= 20; n++) {
        progress(ctx, n, 20);
        if (atomic_load(&aborted)) {
            atomic_fetch_sub(&in_install, 1);
            return INSTALL_CANCELLED;
        }
        while (atomic_load(&hold_install) && !atomic_load(&aborted))
            usleep(1000);
        usleep(1000);
    }
    phase(ctx, "unpack");
    progress(ctx, 1, 1);
    atomic_fetch_sub(&in_install, 1);
    return 0;
}
static void *release_prepare(void *unused) {
    (void)unused;
    usleep(20000);
    atomic_store(&hold_prepare, 0);
    return NULL;
}
static void drain(void) {
    unsigned until = now_ms() + 3000;
    do {
        downloads_tick(0, 0);
        usleep(1000);
        assert(now_ms() < until);
    } while (downloads_busy());
    assert(!forced && !paused && !pause_held && !downloads_focused());
}
static void ready(enum download_state want, int i) {
    unsigned until = now_ms() + 3000;
    for (;;) {
        struct download_status s;
        downloads_tick(0, 0);
        downloads_status(i, &s);
        if (s.state == want)
            return;
        assert(now_ms() < until);
        usleep(1000);
    }
}
int main(void) {
    catalog.count = 3;
    for (int i = 0; i < 3; i++) {
        sprintf(catalog.apps[i].name, "Test %d", i);
        catalog.apps[i].state = APP_NOT_INSTALLED;
        catalog.apps[i].description = strdup("original");
        catalog.apps[i].release.raw = strdup("manifest");
    }
    view_rebuild(&catalog);
    assert(downloads_enqueue(1) == 0 && forced && paused && downloads_focused());
    assert(downloads_enqueue(0) == 0 && downloads_enqueue(1) == 0 && downloads_count() == 2);
    assert(view_tab_at(0) == TAB_DOWNLOADS);
    while (view_tab_kind() != VIEW_TAB_DOWNLOADS)
        view_tab_move(1);
    assert(view_index(1) == 1 && view_index(2) == 0);
    downloads_focus_frame(PSP_CTRL_CIRCLE);
    assert(downloads_busy() && !paused && !downloads_focused());
    drain();
    assert(completed == 2 && order[0] == 1 && order[1] == 0);
    downloads_clear_finished();
    view_tabs_refresh();
    assert(downloads_count() == 0 && view_download_count() == 0);

    assert(downloads_enqueue(2) == 0);
    downloads_cancel(2);
    drain();
    assert(completed == 2);
    downloads_reset();
    assert(downloads_count() == 0);
    assert(downloads_enqueue(0) == 0);
    atomic_store(&hold_install, 1);
    ready(DOWNLOAD_RUNNING, 0);
    downloads_focus_frame(PSP_CTRL_SQUARE);
    atomic_store(&hold_install, 0);
    drain();
    assert(last_rc == INSTALL_CANCELLED);
    assert(downloads_enqueue(0) == 0);
    drain();
    assert(last_rc == 0);

    reject_folder = 1;
    assert(downloads_enqueue(0) == 0);
    drain();
    assert(last_rc == INSTALL_CANCELLED);
    reject_folder = 0;
    fail_start = 1;
    assert(downloads_enqueue(0) == 0);
    drain();
    struct download_status s;
    downloads_status(0, &s);
    assert(s.state == DOWNLOAD_FAILED);
    fail_start = 0;

    atomic_store(&hold_prepare, 1);
    assert(downloads_enqueue(0) == 0);
    ready(DOWNLOAD_PREPARING, 0);
    strcpy(catalog.apps[0].description, "changed");
    strcpy(catalog.apps[0].release.raw, "modified");
    snapshot_ok = 1;
    atomic_store(&hold_prepare, 0);
    drain();
    snapshot_ok = 0;
    assert(last_rc == 0);
    downloads_reset();
    atomic_store(&hold_prepare, 1);
    assert(downloads_enqueue(1) == 0);
    ready(DOWNLOAD_PREPARING, 1);
    pthread_t release;
    pthread_create(&release, NULL, release_prepare, NULL);
    int before = completed;
    downloads_shutdown();
    pthread_join(release, NULL);
    assert(completed == before && atomic_load(&in_install) == 0);
    sceKernelDeleteSema(2);
    for (int i = 0; i < 3; i++)
        entry_clear(&catalog.apps[i]);
    puts("downloads: FIFO, snapshots, background, cancellation, retry, folder refusal, start "
         "failure and shutdown passed");
    return 0;
}
