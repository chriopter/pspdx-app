#include "text.h"
#include <pspkernel.h>
#include <pspctrl.h>
#include "audio/audio.h"
#include "video/player.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "session/downloads.h"
#include "session/actions.h"
#include "session/options.h"
#include "session/questions.h"
#include "session/view.h"
#include "gui/preview.h"
#include "gui/icons.h"
#include "gui/shell.h"
#include "update/inbox.h"
#include "util/runtime.h"

struct job {
    struct app_entry *entry;
    char device[5];
    struct download_status status;
    struct install_report report;
    int rc, finished, approved;
    struct install_layout layout;
    int layout_ok;
    unsigned since;
    unsigned rate_since;
    size_t rate_bytes;
};
/* One job per package that has been queued since the list was last
   cleared, on the heap, NULL for every other package: most of a catalog
   of thousands is never downloaded. Freed only by
   downloads_clear_finished, and never while pending, so the worker's
   pointer to the current one stays good. */
static struct job *jobs[MAX_APPS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int current = -1, held, closing;
static unsigned serial;
static int focused = -1;
static unsigned focus_drawn;
static SceUID worker = -1, answer = -1;
static int shutting_down(void) {
    pthread_mutex_lock(&lock);
    int value = closing;
    pthread_mutex_unlock(&lock);
    return value;
}
static int pending(enum download_state s) {
    return s >= DOWNLOAD_QUEUED && s <= DOWNLOAD_RUNNING;
}
static void free_entry(struct job *j) {
    if (j->entry) {
        entry_clear(j->entry);
        free(j->entry);
        j->entry = NULL;
    }
}
int downloads_status(int i, struct download_status *out) {
    if (i < 0 || i >= MAX_APPS) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    pthread_mutex_lock(&lock);
    if (jobs[i])
        *out = jobs[i]->status;
    else
        memset(out, 0, sizeof(*out));
    pthread_mutex_unlock(&lock);
    return out->state != DOWNLOAD_NONE;
}
int downloads_active(int i) {
    struct download_status s;
    downloads_status(i, &s);
    return pending(s.state);
}
int downloads_pending_count(void) {
    int n = 0;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_APPS; i++)
        n += jobs[i] && pending(jobs[i]->status.state);
    pthread_mutex_unlock(&lock);
    return n;
}
int downloads_busy(void) { return downloads_pending_count() != 0; }
int downloads_count(void) {
    int n = 0;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_APPS; i++)
        n += jobs[i] && jobs[i]->status.state != DOWNLOAD_NONE;
    pthread_mutex_unlock(&lock);
    return n;
}
const char *downloads_label(const struct download_status *s) {
    if (s->cancel_requested && pending(s->state))
        return "Cancelling...";
    switch (s->state) {
    case DOWNLOAD_QUEUED:
        return "Queued";
    case DOWNLOAD_PREPARING:
        return "Preparing...";
    case DOWNLOAD_CONFIRM:
    case DOWNLOAD_LAYOUT:
        return "Waiting for confirmation";
    case DOWNLOAD_RUNNING:
        return !strcmp(s->phase, "unpack")   ? "Installing..."
               : !strcmp(s->phase, "commit") ? "Finishing..."
                                             : "Downloading...";
    case DOWNLOAD_DONE:
        return "Installed";
    case DOWNLOAD_FAILED: {
        /* The reason, where the installer gave one, for the page that
           shows the download and not only the status line. */
        static char failed[96];
        if (!s->why[0]) return "Failed - retry from Options";
        snprintf(failed, sizeof(failed), "Failed: %s", s->why);
        return failed;
    }
    case DOWNLOAD_CANCELLED:
        return "Cancelled";
    default:
        return "";
    }
}
int downloads_enqueue(int i) {
    struct catalog *c = actions_catalog();
    if (i < 0 || i >= c->count || shutting_down())
        return -1;
    if (c->apps[i].unsupported) {
        char refused[96];
        snprintf(refused, sizeof(refused), T_INSTALL_UNSUPPORTED, c->apps[i].name);
        error_show(refused, "install type %s", c->apps[i].type);
        return -1;
    }
    if (downloads_active(i)) {
        downloads_focus(i);
        return 0;
    }
    char device[5];
    int rc = actions_download_device(&c->apps[i], device);
    if (rc < 0)
        return rc;
    /* The job's own copy, text and all: the catalog may be fetched again
       before the download is through. */
    struct app_entry *e = calloc(1, sizeof(*e));
    if (!e)
        return -1;
    if (entry_copy(e, &c->apps[i]) < 0)
        goto fail;
    pthread_mutex_lock(&lock);
    if (!jobs[i] && !(jobs[i] = calloc(1, sizeof(*jobs[i])))) {
        pthread_mutex_unlock(&lock);
        goto fail;
    }
    struct job *j = jobs[i];
    free_entry(j);
    memset(j, 0, sizeof(*j));
    j->entry = e;
    memcpy(j->device, device, sizeof(device));
    j->status.state = DOWNLOAD_QUEUED;
    j->status.order = ++serial;
    pthread_mutex_unlock(&lock);
    view_download_set(i, j->status.order);
    options_download_mode(1);
    view_tabs_refresh();
    shell_status("Added to Downloads");
    downloads_focus(i);
    return 0;
fail:
    entry_clear(e);
    free(e);
    error_show("Not enough memory to queue download", "queue memory");
    return -1;
}
static void progress(void *ctx, size_t done, size_t total) {
    struct job *j = ctx;
    pthread_mutex_lock(&lock);
    j->status.done = done;
    j->status.total = total;
    int cancel = j->status.cancel_requested || closing;
    pthread_mutex_unlock(&lock);
    if (cancel)
        install_abort();
}
static void phase(void *ctx, const char *name) {
    struct job *j = ctx;
    pthread_mutex_lock(&lock);
    snprintf(j->status.phase, sizeof(j->status.phase), "%s", name);
    j->status.done = j->status.total = 0;
    j->status.bytes_per_second = 0;
    j->rate_since = now_ms();
    j->rate_bytes = 0;
    pthread_mutex_unlock(&lock);
    progress(ctx, 0, 0);
}
/* The installer, on the worker, found more than one EBOOT.PBP and went by
   the rule: the job waits while the main thread shows what goes where. */
static int layout_check(void *ctx, const struct install_layout *layout) {
    struct job *j = ctx;
    pthread_mutex_lock(&lock);
    int cancel = j->status.cancel_requested || closing;
    if (!cancel) {
        j->layout = *layout;
        j->layout_ok = -1;
        j->status.state = DOWNLOAD_LAYOUT;
    }
    pthread_mutex_unlock(&lock);
    if (cancel)
        return -1;
    sceKernelWaitSema(answer, 1, NULL);
    pthread_mutex_lock(&lock);
    int rc = j->status.cancel_requested || closing ? -1 : j->layout_ok;
    j->status.state = DOWNLOAD_RUNNING;
    pthread_mutex_unlock(&lock);
    return rc;
}

static int run(SceSize size, void *arg) {
    (void)size;
    (void)arg;
    struct job *j = jobs[current];
    int rc = catalog_prepare(j->entry);
    pthread_mutex_lock(&lock);
    int cancel = j->status.cancel_requested || closing;
    if (rc == 0 && !cancel)
        j->status.state = DOWNLOAD_CONFIRM;
    pthread_mutex_unlock(&lock);
    if (rc == 0 && !cancel) {
        sceKernelWaitSema(answer, 1, NULL);
        pthread_mutex_lock(&lock);
        cancel = j->status.cancel_requested || closing;
        rc = j->approved;
        if (rc == 0 && !cancel)
            j->status.state = DOWNLOAD_RUNNING;
        pthread_mutex_unlock(&lock);
        if (rc == 0 && !cancel) {
            /* The whole manifest, as the installer takes it, made of the
               job's own copy of the entry. */
            install_set_layout_check(layout_check);
            struct manifest *m = malloc(sizeof(*m));
            rc = m && entry_manifest(j->entry, m) == 0
                     ? install_release_to(m, j->device, &j->report, phase, progress, j)
                     : -1;
            if (m)
                manifest_forget(m);
            free(m);
        }
    }
    if (cancel)
        rc = INSTALL_CANCELLED;
    pthread_mutex_lock(&lock);
    j->rc = rc;
    j->finished = 1;
    pthread_mutex_unlock(&lock);
    return 0;
}
void downloads_cancel(int i) {
    if (i < 0 || i >= MAX_APPS)
        return;
    pthread_mutex_lock(&lock);
    struct job *j = jobs[i];
    if (!j) {
        pthread_mutex_unlock(&lock);
        return;
    }
    int running = i == current && pending(j->status.state);
    if (j->status.state == DOWNLOAD_QUEUED) {
        j->status.state = DOWNLOAD_CANCELLED;
        free_entry(j);
    } else if (running)
        j->status.cancel_requested = 1;
    pthread_mutex_unlock(&lock);
    if (running) {
        install_abort();
        if (answer >= 0)
            sceKernelSignalSema(answer, 1);
    }
}
int downloads_tick(int cursor, int modal) {
    if (shutting_down())
        return 0;
    int changed = 0;
    if (current >= 0) {
        struct job *j = jobs[current];
        pthread_mutex_lock(&lock);
        unsigned elapsed = now_ms() - j->rate_since;
        if (!strcmp(j->status.phase, "download") && elapsed >= 500) {
            size_t bytes = j->status.done >= j->rate_bytes ? j->status.done - j->rate_bytes : 0;
            j->status.bytes_per_second = (unsigned long long)bytes * 1000 / elapsed;
            j->rate_bytes = j->status.done;
            j->rate_since += elapsed;
        }
        int done = j->finished, confirm = j->status.state == DOWNLOAD_CONFIRM;
        int layout = j->status.state == DOWNLOAD_LAYOUT;
        int cancelled = j->status.cancel_requested;
        pthread_mutex_unlock(&lock);
        if (done) {
            sceKernelWaitThreadEnd(worker, NULL);
            sceKernelDeleteThread(worker);
            worker = -1;
            sceKernelDeleteSema(answer);
            answer = -1;
            actions_download_complete(current, j->entry, &j->report, j->rc,
                                      (now_ms() - j->since) / 1000);
            if (j->rc == 0)
                for (int n = 0; n < inbox_count(); n++)
                    if (inbox_index(n) == current)
                        inbox_installed(n);
            pthread_mutex_lock(&lock);
            j->status.state = j->rc == 0                   ? DOWNLOAD_DONE
                              : j->rc == INSTALL_CANCELLED ? DOWNLOAD_CANCELLED
                              : j->rc == INSTALL_DECLINED  ? DOWNLOAD_CANCELLED
                                                           : DOWNLOAD_FAILED;
            j->status.cancel_requested = 0;
            j->status.bytes_per_second = 0;
            snprintf(j->status.why, sizeof(j->status.why), "%s", j->rc ? j->report.why : "");
            free_entry(j);
            current = -1;
            view_download_running(-1);
            pthread_mutex_unlock(&lock);
            changed = 1;
        } else if (confirm && !modal) {
            int rc = cancelled ? INSTALL_CANCELLED
                               : actions_download_folder(j->entry, j->device, cursor);
            pthread_mutex_lock(&lock);
            j->approved = rc;
            j->status.state = DOWNLOAD_RUNNING;
            pthread_mutex_unlock(&lock);
            sceKernelSignalSema(answer, 1);
        } else if (layout && !modal) {
            int ok = cancelled ? -1 : actions_download_layout(j->entry, &j->layout, cursor);
            pthread_mutex_lock(&lock);
            j->layout_ok = ok;
            pthread_mutex_unlock(&lock);
            sceKernelSignalSema(answer, 1);
        }
    }
    if (current < 0 && downloads_busy() && !modal) {
        if (!held) {
            preview_pause_begin();
            held = 1;
        }
        if (!preview_pause_ready())
            return changed;
        icons_hold_soft();
        int at = -1;
        unsigned order = ~0u;
        pthread_mutex_lock(&lock);
        for (int i = 0; i < MAX_APPS; i++)
            if (jobs[i] && jobs[i]->status.state == DOWNLOAD_QUEUED &&
                jobs[i]->status.order < order) {
                at = i;
                order = jobs[i]->status.order;
            }
        pthread_mutex_unlock(&lock);
        if (at >= 0) {
            struct job *j = jobs[at];
            j->since = now_ms();
            if (!actions_download_connect()) {
                j->status.state = DOWNLOAD_FAILED;
                free_entry(j);
                changed = 1;
            } else {
                answer = sceKernelCreateSema("download_answer", 0, 0, 1, NULL);
                worker = sceKernelCreateThread("download", run, 0x24, 128 * 1024,
                                               PSP_THREAD_ATTR_USER, NULL);
                pthread_mutex_lock(&lock);
                current = at;
                j->status.state = DOWNLOAD_PREPARING;
                pthread_mutex_unlock(&lock);
                int rc = answer < 0 || worker < 0 ? -1 : sceKernelStartThread(worker, 0, NULL);
                if (rc >= 0) {
                    view_download_running(at);
                    changed = 1;
                }
                if (rc < 0) {
                    if (worker >= 0)
                        sceKernelDeleteThread(worker);
                    if (answer >= 0)
                        sceKernelDeleteSema(answer);
                    worker = answer = -1;
                    current = -1;
                    j->status.state = DOWNLOAD_FAILED;
                    free_entry(j);
                    changed = 1;
                }
            }
        }
    }
    if (!downloads_busy()) {
        downloads_background();
        if (held) {
            preview_resume();
            held = 0;
        }
        options_download_mode(0);
    }
    return changed;
}
void downloads_clear_finished(void) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_APPS; i++)
        if (!jobs[i] || !pending(jobs[i]->status.state)) {
            if (jobs[i]) {
                free_entry(jobs[i]);
                free(jobs[i]);
                jobs[i] = NULL;
            }
            view_download_set(i, 0);
        }
    pthread_mutex_unlock(&lock);
}
void downloads_reset(void) {
    if (!downloads_busy())
        downloads_clear_finished();
}
void downloads_shutdown(void) {
    pthread_mutex_lock(&lock);
    closing = 1;
    if (current >= 0 && jobs[current])
        jobs[current]->status.cancel_requested = 1;
    SceUID active = worker;
    pthread_mutex_unlock(&lock);
    if (active >= 0) {
        install_abort();
        if (answer >= 0)
            sceKernelSignalSema(answer, 1);
        sceKernelWaitThreadEnd(active, NULL);
    }
}

void downloads_focus(int index) {
    if (!downloads_active(index))
        return;
    focused = index;
    focus_drawn = 0;
    audio_pause(1);
    player_pause(1);
}
void downloads_focus_queue(void) {
    int next = -1;
    unsigned order = ~0u;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_APPS; i++)
        if (jobs[i] && pending(jobs[i]->status.state) && jobs[i]->status.order < order) {
            next = i;
            order = jobs[i]->status.order;
        }
    pthread_mutex_unlock(&lock);
    if (next >= 0)
        downloads_focus(next);
}
void downloads_background(void) {
    focused = -1;
    audio_pause(0);
    player_pause(0);
}
int downloads_focused(void) {
    return focused >= 0;
}
int downloads_focus_frame(unsigned pressed) {
    if (focused < 0)
        return 0;
    if (pressed & PSP_CTRL_CIRCLE) {
        downloads_background();
        return 1;
    }
    if (pressed & PSP_CTRL_SQUARE)
        downloads_cancel(focused);
    if (!downloads_active(focused)) {
        downloads_focus_queue();
        if (!downloads_active(focused)) {
            downloads_background();
            return 1;
        }
    }
    if (!focus_drawn || now_ms() - focus_drawn >= 150) {
        struct download_status s;
        downloads_status(focused, &s);
        shell_download_draw(&actions_catalog()->apps[focused], &s);
        focus_drawn = now_ms();
    }
    sceKernelDelayThread(10000);
    return 1;
}
