#include "util/storage.h"
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <psprtc.h>
#include <stdarg.h>
#include <stdio.h>
#include <pthread.h>
#include <string.h>

#include "util/runtime.h"

#define LOGLINES 40
#define LOGCOLS 100           /* the debug screen clips at 60; the file gets it all */

/* The last LOGLINES lines, as a ring: a session that runs for an hour
   still leaves behind what happened last, not what happened first. */
static char lines[LOGLINES][LOGCOLS];
static int line_count, line_next;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t dump_lock = PTHREAD_MUTEX_INITIALIZER;

void logline(const char *fmt, ...) {
    char line[LOGCOLS];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&log_lock);
    strcpy(lines[line_next], line);
    line_next = (line_next + 1) % LOGLINES;
    if (line_count < LOGLINES) line_count++;
    sceIoWrite(1, line, strlen(line));
    sceIoWrite(1, "\n", 1);
    pthread_mutex_unlock(&log_lock);
}

static SceUID g_flush_thread = -1, g_flush_sema = -1;

static int flush_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    for (;;) {
        sceKernelWaitSema(g_flush_sema, 1, 0);
        log_dump();
    }
    return 0;
}

void log_dump_later(void) {
    if (g_flush_thread < 0) {
        g_flush_sema = sceKernelCreateSema("log_flush", 0, 0, 8, 0);
        g_flush_thread = sceKernelCreateThread("log_flush", flush_thread, 0x25, 0x4000,
                                               PSP_THREAD_ATTR_USER, 0);
        if (g_flush_thread < 0 || g_flush_sema < 0) { log_dump(); return; }
        sceKernelStartThread(g_flush_thread, 0, 0);
    }
    sceKernelSignalSema(g_flush_sema, 1);
}

int log_count(void) {
    pthread_mutex_lock(&log_lock);
    int count = line_count;
    pthread_mutex_unlock(&log_lock);
    return count;
}

/* The roots and request threads may append while the failure screen draws. */
void log_at(int index, char *out, size_t size) {
    pthread_mutex_lock(&log_lock);
    const char *line = index < 0 || index >= line_count ? "" :
        lines[(line_next + LOGLINES - line_count + index) % LOGLINES];
    snprintf(out, size, "%s", line);
    pthread_mutex_unlock(&log_lock);
}

/* One write, not two per line: eighty small writes to the stick cost a
   frame, one of four kilobytes does not. */
void log_dump(void) {
    static char out[LOGLINES * LOGCOLS];
    pthread_mutex_lock(&dump_lock);
    pthread_mutex_lock(&log_lock);
    size_t n = 0;
    for (int i = 0; i < line_count; i++) {
        const char *l = lines[(line_next + LOGLINES - line_count + i) % LOGLINES];
        size_t len = strlen(l);
        memcpy(out + n, l, len);
        n += len;
        out[n++] = '\n';
    }
    pthread_mutex_unlock(&log_lock);
    int fd = sceIoOpen(storage_path("PSP/PSPDX/LOGS/pspdx.log"), PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, out, (SceSize)n);
        sceIoClose(fd);
    }
    pthread_mutex_unlock(&dump_lock);
}

unsigned now_ms(void) {
    u64 tick = 0;
    sceRtcGetCurrentTick(&tick);
    return (unsigned)(tick / 1000);
}

unsigned now_us(void) {
    u64 tick = 0;
    sceRtcGetCurrentTick(&tick);
    return (unsigned)tick;
}

int expired(unsigned start, unsigned budget_ms) {
    return (now_ms() - start) > budget_ms;
}
