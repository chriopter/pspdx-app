/* Throughput test against a server on the host: the same five megabytes
   over a bare socket, then over TLS 1.3 with each cipher suite preferred.
   Loopback is faster than any radio, so what this measures is the cost of
   the stack and the cipher on the CPU, with nothing else in the way.

   Runs in PPSSPP through dev/nettest/run, which also starts the servers
   and reads the numbers back out of the log. */

#include <pspkernel.h>
#include <pspnet_inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "logic/entropy.h"
#include "network/https.h"
#include "util/runtime.h"

PSP_MODULE_INFO("nettest", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(8 * 1024);

#define HOST "127.0.0.1"
#define RAW_PORT 8080
#define TLS_URL "https://127.0.0.1:8443/5MB.bin"

static unsigned char g_buf[256 * 1024];

static unsigned kbps(unsigned bytes, unsigned us) {
    return us ? (unsigned)((unsigned long long)bytes * 1000 / us) : 0;
}

static void raw(void) {
    int sock = sceNetInetSocket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { logline("raw: no socket"); return; }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(RAW_PORT);
    sa.sin_addr.s_addr = inet_addr(HOST);
    unsigned start = now_ms();
    if (sceNetInetConnect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        int e = sceNetInetGetErrno();
        if (e != EINPROGRESS && e != EALREADY && e != EWOULDBLOCK) { logline("raw: connect errno %d", e); return; }
    }
    const char *req = "GET /5MB.bin HTTP/1.1\r\nHost: " HOST "\r\nUser-Agent: pspdx\r\nConnection: close\r\n\r\n";
    size_t sent = 0, len = strlen(req);
    while (sent < len) {
        int n = (int)sceNetInetSend(sock, req + sent, len - sent, 0);
        if (n > 0) { sent += (size_t)n; continue; }
        int e = sceNetInetGetErrno();
        if (e != EAGAIN && e != EWOULDBLOCK && e != EINTR && e != ENOTCONN && e != EINPROGRESS) { logline("raw: send errno %d", e); return; }
        if (expired(start, 10000)) { logline("raw: connect timed out"); return; }
        sceKernelDelayThread(5 * 1000);
    }
    unsigned t0 = now_us(), idle = now_ms();
    unsigned total = 0;
    for (;;) {
        int n = (int)sceNetInetRecv(sock, g_buf, sizeof(g_buf), 0);
        if (n > 0) { total += (unsigned)n; idle = now_ms(); continue; }
        if (n == 0) break;
        int e = sceNetInetGetErrno();
        if (e != EAGAIN && e != EWOULDBLOCK && e != EINTR) { logline("raw: recv errno %d", e); break; }
        if (expired(idle, 20000)) { logline("raw: stalled"); break; }
        sceKernelDelayThread(500);
    }
    unsigned us = now_us() - t0;
    sceNetInetClose(sock);
    logline("raw http: %u KB in %u ms: %u KB/s", total / 1024, us / 1000, kbps(total, us));
}

static unsigned g_got;
static int count(void *ctx, const void *data, size_t len) {
    (void)ctx; (void)data;
    g_got += (unsigned)len;
    return 0;
}

static void tls(const char *suites, const char *label) {
    https_prefer(suites);
    struct https_result r;
    g_got = 0;
    unsigned t0 = now_us();
    if (https_get(TLS_URL, count, 0, 0, 0, &r) != 0) { logline("%s: failed", label); return; }
    unsigned us = now_us() - t0;
    logline("%s: %u KB in %u ms (handshake %u ms): %u KB/s", label, g_got / 1024,
            us / 1000, r.handshake_ms, kbps(g_got, us));
}

int main(void) {
    /* wolfSSL draws its randomness from the pool; jitter is enough for a
       test whose keys protect nothing. */
    entropy_init();
    if (net_up() < 0) { logline("no network"); log_dump(); sceKernelExitGame(); }
    for (int i = 0; i < 2; i++) {
        raw(); log_dump();
        tls("TLS13-AES128-GCM-SHA256", "tls aes-128-gcm"); log_dump();
        tls("TLS13-CHACHA20-POLY1305-SHA256", "tls chacha20-poly1305"); log_dump();
    }
    logline("done");
    log_dump();
    sceKernelExitGame();
    return 0;
}
