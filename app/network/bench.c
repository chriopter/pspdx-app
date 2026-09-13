#include <pspkernel.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/chacha20_poly1305.h>

#include "network/bench.h"
#include "network/https.h"
#include "util/runtime.h"

#define CHUNK (256 * 1024)
#define ROUNDS 32                       /* 8 MB */

static unsigned char g_in[CHUNK], g_out[CHUNK];

static unsigned kbps(unsigned bytes, unsigned us) {
    return us ? (unsigned)((unsigned long long)bytes * 1000 / us) : 0;
}

static void bench_bulk(void) {
    static const unsigned char key[32] = { 1, 2, 3 };
    static const unsigned char iv[12] = { 4, 5, 6 };
    unsigned char tag[16];
    for (unsigned i = 0; i < CHUNK; i++) g_in[i] = (unsigned char)i;

    Aes aes;
    wc_AesInit(&aes, 0, -1);
    wc_AesGcmSetKey(&aes, key, 16);
    unsigned t0 = now_us();
    for (int r = 0; r < ROUNDS; r++)
        wc_AesGcmEncrypt(&aes, g_out, g_in, CHUNK, iv, sizeof(iv), tag, sizeof(tag), 0, 0);
    unsigned aes_us = now_us() - t0;
    wc_AesFree(&aes);

    t0 = now_us();
    for (int r = 0; r < ROUNDS; r++)
        wc_ChaCha20Poly1305_Encrypt(key, iv, 0, 0, g_in, CHUNK, g_out, tag);
    unsigned cc_us = now_us() - t0;

    logline("bench: aes-128-gcm %u KB/s, chacha20-poly1305 %u KB/s (8 MB each)",
            kbps(CHUNK * ROUNDS, aes_us), kbps(CHUNK * ROUNDS, cc_us));
}

static int drop(void *ctx, const void *data, size_t len) {
    (void)ctx; (void)data; (void)len;
    return 0;
}

static void bench_handshake(const char *url, const char *suites, const char *label) {
    struct https_result r;
    unsigned best = 0;
    for (int i = 0; i < 3; i++) {
        https_prefer(suites); /* Measure new handshakes, not a reused connection. */
        if (https_get(url, drop, 0, 0, 0, &r) != 0) { logline("bench: %s failed", label); return; }
        if (!best || r.handshake_ms < best) best = r.handshake_ms;
    }
    logline("bench: %s handshake best of 3: %u ms", label, best);
}

/* The same file, same host, over a bare socket: what the stack manages
   with nothing to decrypt. This is the only plain-HTTP code in the client
   and it lives here so the client itself keeps refusing http://. */
#define TEST_HOST "download.thinkbroadband.com"
#define TEST_PATH "/5MB.zip"
#define TEST_URL "https://" TEST_HOST TEST_PATH

static unsigned bench_raw(void) {
    static char buf[1024];
    struct in_addr ip;
    int rid = -1;
    if (sceNetResolverCreate(&rid, buf, sizeof(buf)) < 0) return 0;
    int rc = sceNetResolverStartNtoA(rid, TEST_HOST, &ip, 2 * 1000 * 1000, 5);
    sceNetResolverDelete(rid);
    if (rc < 0) { logline("bench: dns failed"); return 0; }

    int sock = sceNetInetSocket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return 0;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(80);
    sa.sin_addr = ip;
    unsigned start = now_ms();
    int crc = sceNetInetConnect(sock, (struct sockaddr *)&sa, sizeof(sa));
    logline("bench: connect rc=%d errno=%d", crc, crc < 0 ? sceNetInetGetErrno() : 0);
    if (crc < 0) {
        int e = sceNetInetGetErrno();
        if (e != EINPROGRESS && e != EALREADY && e != EWOULDBLOCK) { sceNetInetClose(sock); return 0; }
        while (!expired(start, 10000)) sceKernelDelayThread(50 * 1000);
    }
    /* The socket is non-blocking and the connect completes on its own
       time: send until the request is out, or give up after the timeout. */
    const char *req = "GET " TEST_PATH " HTTP/1.1\r\nHost: " TEST_HOST "\r\nUser-Agent: pspdx\r\nConnection: close\r\n\r\n";
    size_t sent = 0, len = strlen(req);
    while (sent < len) {
        int n = (int)sceNetInetSend(sock, req + sent, len - sent, 0);
        if (n > 0) { sent += (size_t)n; continue; }
        int e = sceNetInetGetErrno();
        if (e != EAGAIN && e != EWOULDBLOCK && e != EINTR && e != ENOTCONN && e != EINPROGRESS) {
            logline("bench: send errno %d", e);
            sceNetInetClose(sock);
            return 0;
        }
        if (expired(start, 10000)) { logline("bench: connect timed out"); sceNetInetClose(sock); return 0; }
        sceKernelDelayThread(10 * 1000);
    }

    logline("bench: request sent, %lu bytes", (unsigned long)sent);
    unsigned t0 = now_us(), idle = now_ms();
    unsigned long total = 0;
    int first = 1;
    for (;;) {
        int n = (int)sceNetInetRecv(sock, g_in, sizeof(g_in), 0);
        if (first) { logline("bench: first recv rc=%d errno=%d", n, n < 0 ? sceNetInetGetErrno() : 0); first = 0; }
        if (n > 0) { total += (unsigned long)n; idle = now_ms(); continue; }
        if (n == 0) break;
        int e = sceNetInetGetErrno();
        if (e != EAGAIN && e != EWOULDBLOCK && e != EINTR) { logline("bench: recv errno %d", e); break; }
        if (expired(idle, 30000)) break;
        sceKernelDelayThread(1000);
    }
    unsigned us = now_us() - t0;
    sceNetInetClose(sock);
    logline("bench: raw http %lu KB in %u ms: %u KB/s", total / 1024, us / 1000,
            kbps((unsigned)total, us));
    return kbps((unsigned)total, us);
}

static unsigned g_got;
static int count(void *ctx, const void *data, size_t len) {
    (void)ctx; (void)data;
    g_got += (unsigned)len;
    return 0;
}

static void bench_tls(const char *suites, const char *label) {
    https_prefer(suites);
    struct https_result r;
    g_got = 0;
    unsigned t0 = now_us();
    if (https_get(TEST_URL, count, 0, 0, 0, &r) != 0) { logline("bench: %s download failed", label); return; }
    unsigned us = now_us() - t0;
    logline("bench: %s %u KB in %u ms (handshake %u): %u KB/s", label, g_got / 1024,
            us / 1000, r.handshake_ms, kbps(g_got, us));
}

/* The log is flushed after every step: a step that hangs still leaves the
   ones before it on the stick. */
void bench_run(const char *url) {
    bench_bulk(); log_dump();
    bench_handshake(url, "TLS13-AES128-GCM-SHA256", "aes-128-gcm"); log_dump();
    bench_handshake(url, "TLS13-CHACHA20-POLY1305-SHA256", "chacha20-poly1305"); log_dump();
    bench_raw(); log_dump();
    bench_tls("TLS13-AES128-GCM-SHA256", "https aes-128-gcm"); log_dump();
    bench_tls("TLS13-CHACHA20-POLY1305-SHA256", "https chacha20-poly1305"); log_dump();
    https_prefer(0);
}
