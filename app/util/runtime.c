#include "util/storage.h"
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <psputils.h>
#include <stdarg.h>
#include <stdio.h>
#include <pthread.h>
#include <string.h>

#include "util/runtime.h"

#define LOGLINES 400
#define LOGCOLS 320           /* enough for one complete, batched perf record */

/* The last LOGLINES lines, as a ring: a session that runs for an hour
   still leaves behind what happened last, not what happened first. */
static char lines[LOGLINES][LOGCOLS];
static int line_count, line_next;
static char latest_perf[LOGCOLS];
/* Lines and perf records ever logged, and how far the file on the stick
   has them: a dump appends what is new instead of writing the whole ring
   again. A Memory Stick write holds the bus in the kernel's driver, ahead
   of every thread of PSPDX: the ring whole, some 100 KB, was a frame of
   70-90 ms every ten seconds; what is new is a few KB. */
static unsigned line_seq, perf_seq;
#define LOG_PERIOD_MS (5 * 60 * 1000)
static unsigned dumped_line_seq, dumped_perf_seq;
static int dumped_once;
static unsigned file_bytes;
#define LOG_FILE_MAX (256 * 1024)
#define LOG_CHUNK (8 * 1024)
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
    line_seq++;
    sceIoWrite(1, line, strlen(line));
    sceIoWrite(1, "\n", 1);
    pthread_mutex_unlock(&log_lock);
}

/* Telemetry must not evict the events needed to diagnose a connection. */
void logperf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&log_lock);
    vsnprintf(latest_perf, sizeof(latest_perf), fmt, ap);
    va_end(ap);
    perf_seq++;
    sceIoWrite(1, latest_perf, strlen(latest_perf));
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

int log_rig(void) {
    static int rig = -1;
    /* dev/rig always leaves PSPDX.SHOTS; PSPDX.LOG asks for it alone, on
       a console on the bench. The DEBUG directory itself is made on every
       stick and says nothing. */
    if (rig < 0)
        rig = storage_exists(storage_path("PSP/PSPDX/DEBUG/PSPDX.SHOTS")) ||
              storage_exists(storage_path("PSP/PSPDX/DEBUG/PSPDX.LOG"));
    return rig;
}

void log_dump_periodic(void) {
    static unsigned last_ms;
    if (log_rig()) { log_dump_later(); return; }
    pthread_mutex_lock(&log_lock);
    int fresh = line_seq != dumped_line_seq;
    pthread_mutex_unlock(&log_lock);
    if (!fresh || now_ms() - last_ms < LOG_PERIOD_MS) return;
    last_ms = now_ms();
    log_dump_later();
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
    static char out[(LOGLINES + 1) * LOGCOLS];
    pthread_mutex_lock(&dump_lock);
    pthread_mutex_lock(&log_lock);
    unsigned fresh = line_seq - dumped_line_seq;
    /* The whole ring at the first dump of a session, when lines went by
       that the ring no longer holds, or when the file has grown to its
       limit; else only what came since the last dump. */
    int whole = !dumped_once || fresh > (unsigned)line_count || file_bytes >= LOG_FILE_MAX;
    int from = whole ? 0 : line_count - (int)fresh;
    size_t n = 0;
    for (int i = from; i < line_count; i++) {
        const char *l = lines[(line_next + LOGLINES - line_count + i) % LOGLINES];
        size_t len = strlen(l);
        memcpy(out + n, l, len);
        n += len;
        out[n++] = '\n';
    }
    if (latest_perf[0] && (whole || perf_seq != dumped_perf_seq)) {
        size_t len = strlen(latest_perf);
        memcpy(out + n, latest_perf, len);
        n += len;
        out[n++] = '\n';
    }
    dumped_line_seq = line_seq;
    dumped_perf_seq = perf_seq;
    pthread_mutex_unlock(&log_lock);
    if (n || whole) {
        int fd = sceIoOpen(storage_path("PSP/PSPDX/LOGS/pspdx.log"),
                           PSP_O_WRONLY | PSP_O_CREAT | (whole ? PSP_O_TRUNC : PSP_O_APPEND), 0777);
        int ok = fd >= 0;
        /* In pieces, with a breath between: the bus is let go for the
           main thread's frame between them. */
        for (size_t at = 0; ok && at < n; at += LOG_CHUNK) {
            size_t len = n - at < LOG_CHUNK ? n - at : LOG_CHUNK;
            if (at) sceKernelDelayThread(2000);
            ok = sceIoWrite(fd, out + at, (SceSize)len) == (int)len;
        }
        if (fd >= 0) sceIoClose(fd);
        /* A write that did not go through is made whole next time. */
        dumped_once = ok;
        file_bytes = ok ? (whole ? 0 : file_bytes) + (unsigned)n : 0;
    }
    pthread_mutex_unlock(&dump_lock);
}

/* Time since power-on, not the clock: the clock is set back and forth by
   hand, and on a console that has lost it entirely it stands still, which
   would stop every wait, fade and timeout with it. */
unsigned now_ms(void) {
    return (unsigned)(sceKernelGetSystemTimeWide() / 1000);
}

unsigned now_us(void) {
    return (unsigned)sceKernelGetSystemTimeWide();
}

unsigned wall_time(void) {
    return (unsigned)sceKernelLibcTime(NULL);
}

int expired(unsigned start, unsigned budget_ms) {
    return (now_ms() - start) > budget_ms;
}
