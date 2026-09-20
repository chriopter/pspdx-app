#ifndef PSPDX_DOWNLOADS_H
#define PSPDX_DOWNLOADS_H
#include <stddef.h>
#include "update/catalog.h"

enum download_state {
    DOWNLOAD_NONE,
    DOWNLOAD_QUEUED,
    DOWNLOAD_PREPARING,
    DOWNLOAD_CONFIRM,
    DOWNLOAD_RUNNING,
    DOWNLOAD_DONE,
    DOWNLOAD_FAILED,
    DOWNLOAD_CANCELLED
};
struct download_status {
    enum download_state state;
    size_t done, total;
    size_t bytes_per_second;
    unsigned order;
    char phase[24];
    int cancel_requested;
};
/* Main-thread queue operations. One worker owns the network and installer. */
/* Zero means queued (or already pending), not installed. */
int downloads_enqueue(int index);
int downloads_tick(int cursor, int modal);
int downloads_busy(void);
int downloads_pending_count(void);
int downloads_active(int index);
int downloads_count(void);
int downloads_status(int index, struct download_status *out);
void downloads_cancel(int index);
void downloads_clear_finished(void);
void downloads_reset(void);
/* Exit callback: cancel and join before leaving installer files behind. */
void downloads_shutdown(void);
void downloads_focus(int index);
void downloads_focus_queue(void);
void downloads_background(void);
int downloads_focused(void);
int downloads_focus_frame(unsigned pressed);
const char *downloads_label(const struct download_status *status);
#endif
