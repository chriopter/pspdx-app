#include "util/storage.h"
/*
 * HTTPS on the PSP: sceNetInet sockets under wolfSSL, and just enough HTTP/1.1
 * to stream one body of any size into a sink. Short-lived connections are
 * reused per host when the response has an unambiguous Content-Length.
 *
 * The four traps in this file each cost an afternoon and none is documented:
 * the BSD socket wrappers return garbage, sceNetInetSelect hangs, SO_NONBLOCK
 * and SO_ERROR do not exist in the headers, and retrying EINTR inside an IO
 * callback spins forever inside the handshake.
 */

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <psputility.h>
#include <psputility_netmodules.h>
#include <pspnet.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <psppower.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include "logic/entropy.h"
#include "network/https.h"
#include "util/runtime.h"
#include "network/ca_certs.h"

#define PORT 443
#define MAX_REDIRECTS 5
#define HEAD_MAX (8 * 1024)

#define CONNECT_TIMEOUT_MS   10000
#define HANDSHAKE_TIMEOUT_MS 20000
/* Per read, not per body: a 42 MB download over 802.11b takes minutes and
   must not be cut off for being slow, only for being stuck. */
#define STALL_TIMEOUT_MS     30000

/* ChaCha20-Poly1305 first: on a core with no AES instructions it moves
   bytes at two and a half times the rate of AES-GCM through this stack,
   which is the difference between a download that costs a third of the
   CPU and one that costs an eighth. AES stays for a server without it. */
#define DEFAULT_SUITES "TLS13-CHACHA20-POLY1305-SHA256:TLS13-AES128-GCM-SHA256"

static const char *g_suites = DEFAULT_SUITES;
static void close_idle(void);
static int g_wolf_ready;

/* What the stack is doing right now, for a status line: a literal, set by
   the thread doing the work and read by whoever draws. */
static const char *volatile g_phase = "";
static void phase(const char *p) { g_phase = p; }
static volatile int g_abort;
void https_abort(void) { g_abort = 1; }
const char *https_phase(void) { return g_phase; }

void https_prefer(const char *suites) {
    close_idle();
    g_suites = suites ? suites : DEFAULT_SUITES;
}

/* What the last handshake settled on. Written by whichever thread made it and
   read by the one that draws: four short strings that are replaced whole, so
   the worst a reader can see is the previous connection's. */
static struct https_info g_last;
const struct https_info *https_last(void) { return &g_last; }

/* ------------------------------------------------------------------- net */

static struct {
    int net, inet, resolver, apctl, connected;
} g_net;

void net_down(void) {
    close_idle();
    if (g_wolf_ready) { wolfSSL_Cleanup(); g_wolf_ready = 0; }
    if (g_net.connected) { sceNetApctlDisconnect(); g_net.connected = 0; }
    if (g_net.apctl)     { sceNetApctlTerm();       g_net.apctl = 0; }
    if (g_net.resolver)  { sceNetResolverTerm();    g_net.resolver = 0; }
    if (g_net.inet)      { sceNetInetTerm();        g_net.inet = 0; }
    if (g_net.net)       { sceNetTerm();            g_net.net = 0; }
    sceUtilityUnloadNetModule(PSP_NET_MODULE_INET);
    sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
}

#ifdef DEBUG_WOLFSSL
/* Development only: wolfSSL's own trace, appended to the stick. It is the only
   way to see which step of a chain check failed. */
static void wolf_log(const int level, const char *const msg) {
    (void)level;
    int fd = sceIoOpen(storage_path("PSP/PSPDX/LOGS/wolf.log"), PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, msg, strlen(msg));
    sceIoWrite(fd, "\n", 1);
    sceIoClose(fd);
}
#endif

int net_up(void) {
#ifdef DEBUG_WOLFSSL
    wolfSSL_SetLoggingCb(wolf_log);
    wolfSSL_Debugging_ON();
#endif
    /* Asked again with the link already up -- a retry after the catalog
       failed, not the wifi -- there is nothing to bring up. */
    if (g_net.connected) {
        int state = 0;
        if (sceNetApctlGetState(&state) >= 0 && state == 4) return 0;
        net_down();
    }
    phase("wifi");
    if (sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON) < 0) return -1;
    if (sceUtilityLoadNetModule(PSP_NET_MODULE_INET) < 0) {
        sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
        return -2;
    }

    if (sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024) < 0) goto fail;
    g_net.net = 1;
    if (sceNetInetInit() < 0) goto fail;
    g_net.inet = 1;
    if (sceNetResolverInit() < 0) goto fail;
    g_net.resolver = 1;
    if (sceNetApctlInit(0x1600, 42) < 0) goto fail;
    g_net.apctl = 1;

    /* Connection profile 1, the first one configured on the console. */
    phase("access point");
    if (sceNetApctlConnect(1) < 0) goto fail;
    g_net.connected = 1;

    unsigned start = now_ms();
    for (;;) {
        int state = 0;
        if (sceNetApctlGetState(&state) < 0) goto fail;
        if (state == 4) { phase("ip"); return 0; }   /* got an IP */
        if (expired(start, CONNECT_TIMEOUT_MS)) goto fail;
        sceKernelDelayThread(50 * 1000);
    }

fail:
    net_down();
    return -3;
}

/* The PSP resolver rather than getaddrinfo: newlib's lookup path yields
   "Trying 0.0.0.0" here, so it is not to be trusted. */
static int resolve(const char *host, struct in_addr *out) {
    static char buf[1024];
    int rid = -1;
    if (sceNetResolverCreate(&rid, buf, sizeof(buf)) < 0) return -1;
    int rc = sceNetResolverStartNtoA(rid, host, out, 2 * 1000 * 1000, 5);
    sceNetResolverDelete(rid);
    return rc < 0 ? -2 : 0;
}

/* PSPSDK declares these as returning size_t even though they report failure as
   a negative value, so the cast back to int is deliberate and load-bearing. */
static int psp_recv(int fd, void *buf, int len) {
    return (int)sceNetInetRecv(fd, buf, (size_t)len, 0);
}

static int psp_send(int fd, const void *buf, int len) {
    return (int)sceNetInetSend(fd, buf, (size_t)len, 0);
}

/* sceNetInetSelect hangs on this stack, so waiting is a short sleep. */
static void wait_socket(int ms) {
    sceKernelDelayThread((unsigned)ms * 1000);
}

/* Read once per connection, just before wolfSSL builds this session's RNG, so
   that what goes in reaches this handshake and not merely the next. The
   current draw moves with whatever the CPU and the radio are doing and has a
   noisy converter beneath it; voltage and temperature drift slowly and are
   worth a good deal less. How long the connect took is the network's answer,
   not ours. None of it is counted -- see entropy_stir. */
static void stir_power(unsigned connect_ms) {
    struct {
        int volt, elec, temp, life;
        unsigned connect_ms;
    } p;
    p.volt = scePowerGetBatteryVolt();
    p.elec = scePowerGetBatteryElec();
    p.temp = scePowerGetBatteryTemp();
    p.life = scePowerGetBatteryLifeTime();
    p.connect_ms = connect_ms;
    entropy_stir(&p, sizeof(p));
}

/* ---------------------------------------------------------------- pacing */

/* The test rig only. An emulator borrows the host's network, which is an
   order of magnitude past what a PSP-1004's 802.11b radio and its own TCP
   stack ever managed -- a download that takes half a minute on the hardware
   is over before the progress bar has moved. ms0:/PSPDX.SLOW holds a rate in
   kilobytes a second, or nothing for the measured rate of a 1004, and the
   receive path is held to it. A PSP nobody has put that file on reads
   nothing here and is paced by its radio, as it should be.

   The clamp is on arriving bytes rather than on the socket, so it shapes
   every fetch the client makes: catalog, icons, films and packages alike. */
#define PSP_1004_KBPS 180

static unsigned g_paced_kbps;           /* 0 until asked, then 0 = no limit */
static unsigned g_pace_since, g_pace_bytes;

static void pace_begin(void) {
    if (!g_paced_kbps) {
        char text[16];
        int fd = sceIoOpen(storage_path("PSP/PSPDX/DEBUG/PSPDX.SLOW"), PSP_O_RDONLY, 0777);
        if (fd < 0) { g_paced_kbps = ~0u; return; }
        int n = sceIoRead(fd, text, sizeof(text) - 1);
        sceIoClose(fd);
        text[n > 0 ? n : 0] = '\0';
        unsigned rate = (unsigned)atoi(text);
        g_paced_kbps = rate ? rate : PSP_1004_KBPS;
        logline("network paced to %u KB/s, as a PSP-1004", g_paced_kbps);
    }
    g_pace_since = now_ms();
    g_pace_bytes = 0;
}

static void pace(int n) {
    if (g_paced_kbps == ~0u || n <= 0) return;
    g_pace_bytes += (unsigned)n;
    unsigned due = g_pace_bytes / g_paced_kbps;          /* ms the radio needs */
    unsigned spent = now_ms() - g_pace_since;
    if (due > spent) sceKernelDelayThread((due - spent) * 1000);
}

/* ------------------------------------------------------------------- tls */

static int io_recv(WOLFSSL *ssl, char *buf, int sz, void *ctx) {
    (void)ssl;
    if (sz <= 0) return 0;
    int fd = *(int *)ctx;

    int n = psp_recv(fd, buf, sz);
    if (n > 0) {
        pace(n);
        /* When a packet lands is decided by the radio, the access point's
           scheduling and the path across the internet, none of which this
           device has a say in. wait_socket polls on a fixed sleep, which
           coarsens the arrival time, so this is worth about a bit; the
           timestamp is folded in by entropy_stir itself. */
        entropy_stir(&n, sizeof(n));
        return n;
    }
    if (n == 0) return WOLFSSL_CBIO_ERR_CONN_CLOSE;

    int e = sceNetInetGetErrno();
    if (e == ECONNRESET) return WOLFSSL_CBIO_ERR_CONN_RST;
    if (e == ETIMEDOUT) return WOLFSSL_CBIO_ERR_TIMEOUT;
    /* EINTR is documented to map to CBIO_ERR_ISR, but returning WANT_READ hands
       control back to the caller, where a deadline governs the retry. Retrying
       inside the callback has no bound and hangs the handshake. */
    if (e == EAGAIN || e == EWOULDBLOCK || e == EINTR)
        return WOLFSSL_CBIO_ERR_WANT_READ;
    return WOLFSSL_CBIO_ERR_GENERAL;
}

static int io_send(WOLFSSL *ssl, char *buf, int sz, void *ctx) {
    (void)ssl;
    if (sz <= 0) return 0;
    int fd = *(int *)ctx;

    int n = psp_send(fd, buf, sz);
    if (n >= 0) return n;

    int e = sceNetInetGetErrno();
    if (e == EPIPE || e == ECONNRESET) return WOLFSSL_CBIO_ERR_CONN_RST;
    if (e == EAGAIN || e == EWOULDBLOCK || e == EINTR)
        return WOLFSSL_CBIO_ERR_WANT_WRITE;
    return WOLFSSL_CBIO_ERR_GENERAL;
}

/* PSP newlib has no memmem. */
static const char *mem_find(const char *hay, size_t hlen,
                            const char *needle, size_t nlen) {
    if (nlen == 0 || hlen < nlen) return NULL;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nlen) == 0)
            return hay + i;
    }
    return NULL;
}

/* Case-insensitive header lookup within the head. Returns the value start. */
static const char *header(const char *head, size_t len, const char *name) {
    size_t nlen = strlen(name);
    const char *p = head, *end = head + len;
    while (p < end) {
        const char *eol = mem_find(p, (size_t)(end - p), "\r\n", 2);
        if (!eol) eol = end;
        if ((size_t)(eol - p) > nlen && p[nlen] == ':' &&
            strncasecmp(p, name, nlen) == 0) {
            const char *v = p + nlen + 1;
            while (v < eol && *v == ' ') v++;
            return v;
        }
        p = eol + 2;
    }
    return NULL;
}

static int connection_closes(const char *value, const char *end) {
    if (!value || !end) return 0;
    for (const char *p = value; p + 5 <= end; p++)
        if (!strncasecmp(p, "close", 5) &&
            (p == value || p[-1] == ' ' || p[-1] == ',') &&
            (p + 5 == end || p[5] == ' ' || p[5] == ','))
            return 1;
    return 0;
}

/* ------------------------------------------------------------------- url */

/* A GitHub release download redirects to a signed URL well over 512 bytes. */
struct url { char host[128]; char path[1600]; unsigned short port; };

static int url_parse(const char *s, struct url *u) {
    if (strncmp(s, "https://", 8) != 0) { logline("url: not https: %.40s", s); return -1; }
    s += 8;
    const char *slash = strchr(s, '/');
    size_t hl = slash ? (size_t)(slash - s) : strlen(s);
    if (hl == 0 || hl >= sizeof(u->host)) { logline("url: bad host"); return -1; }
    memcpy(u->host, s, hl);
    u->host[hl] = '\0';
    /* host:port, for a server that is not on 443 -- a test one, mostly. */
    u->port = PORT;
    char *colon = strchr(u->host, ':');
    if (colon) {
        *colon = '\0';
        unsigned long port = strtoul(colon + 1, NULL, 10);
        if (port == 0 || port > 65535) { logline("url: bad port"); return -1; }
        u->port = (unsigned short)port;
    }
    if (!slash) { strcpy(u->path, "/"); return 0; }
    if (strlen(slash) >= sizeof(u->path)) { logline("url: path too long"); return -1; }
    strcpy(u->path, slash);
    return 0;
}

/* Location may be absolute or a path on the same host. */
static int url_resolve(const struct url *base, const char *loc, size_t loclen,
                       struct url *out) {
    char tmp[1800];
    if (loclen >= sizeof(tmp)) return -1;
    memcpy(tmp, loc, loclen);
    tmp[loclen] = '\0';
    if (tmp[0] == '/') {
        *out = *base;
        if (strlen(tmp) >= sizeof(out->path)) return -1;
        strcpy(out->path, tmp);
        return 0;
    }
    if (strncmp(tmp, "https://", 8) == 0) return url_parse(tmp, out);
    if (strncmp(tmp, "http://", 7) == 0) {
        /* Refusing rather than following: a redirect down to plain HTTP is
           where a downgrade would happen, and no host here needs it. */
        logline("url: refusing redirect to http");
        return -1;
    }

    /* A bare relative reference, resolved against the base directory. */
    *out = *base;
    const char *slash = strrchr(base->path, '/');
    size_t dir = slash ? (size_t)(slash - base->path) + 1 : 1;
    if (dir + strlen(tmp) >= sizeof(out->path)) return -1;
    memcpy(out->path, base->path, dir);
    strcpy(out->path + dir, tmp);
    return 0;
}

/* The day this file was compiled, as yyyymmdd, from the "Mmm dd yyyy" the
   compiler hands out. It is the one date the client knows to be in the past
   whatever the console's clock says. */
static long build_day(void) {
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *d = __DATE__;
    long mon = 0;
    for (int i = 0; i < 12; i++)
        if (!strncmp(d, months + i * 3, 3)) mon = i + 1;
    long day = (d[4] == ' ' ? 0 : (d[4] - '0') * 10) + (d[5] - '0');
    return atol(d + 7) * 10000 + mon * 100 + day;
}

/* True when the certificate's notAfter lies before the build: it had run out
   before this client existed, and no clock can make it current again. An
   unreadable date counts as run out -- the waiver below is for a clock the
   client distrusts, not for a date it cannot read. */
static int expired_before_build(WOLFSSL_X509_STORE_CTX *store) {
    WOLFSSL_X509 *c = wolfSSL_X509_STORE_CTX_get_current_cert(store);
    WOLFSSL_ASN1_TIME *t = c ? wolfSSL_X509_get_notAfter(c) : NULL;
    struct tm tm;
    if (!t || wolfSSL_ASN1_TIME_to_tm(t, &tm) != WOLFSSL_SUCCESS) {
        logline("cert notAfter unreadable at depth %d", store->error_depth);
        return 1;
    }
    long after = (tm.tm_year + 1900L) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
    if (after >= build_day()) return 0;
    logline("cert at depth %d ran out %ld, before this build (%ld)",
            store->error_depth, after, build_day());
    return 1;
}

/* The one check that has to be waived: the console's clock. The PSP's RTC is
   user-settable and resets to 2000 when the battery dies, so a correct chain
   would be rejected as not-yet-valid on a large share of real consoles. Every
   other verification failure -- unknown issuer, bad signature, wrong host --
   still fails the handshake. This trades expiry for the ability to run at all;
   revocation was never checked on a device with no clock anyway.

   The waiver has a floor. A certificate that had already run out on the day
   the client was built is refused whichever way the clock is wrong: without
   that, a key leaked from any certificate ever issued would open every
   console for ever, and a client this old is due an update anyway. */
static int verify_ignoring_dates(int preverify, WOLFSSL_X509_STORE_CTX *store) {
    if (preverify) return 1;
    if (store->error == ASN_BEFORE_DATE_E || store->error == ASN_AFTER_DATE_E) {
        if (expired_before_build(store)) return 0;
        logline("cert date ignored: the console clock is not trustworthy");
        return 1;
    }
    {
        WOLFSSL_X509 *c = wolfSSL_X509_STORE_CTX_get_current_cert(store);
        char *sub = c ? wolfSSL_X509_get_subjectCN(c) : NULL;
        char iss[48] = "?";
        if (c) wolfSSL_X509_NAME_oneline(wolfSSL_X509_get_issuer_name(c), iss, sizeof(iss));
        /* ASN_NO_SIGNER_E means a CA we do not carry, not an attack: rebuild
           the bundle with tools/make-ca-bundle.py and this host works again. */
        logline("cert %d at depth %d: %.14s from %.24s", store->error,
                store->error_depth, sub ? sub : "?", iss);
    }
    return 0;
}

/* ---------------------------------------------------------------- request */

/* The catalog alternates raw.githubusercontent.com and api.github.com for
   each app. One idle slot would close the first connection every time the
   second host is contacted. These slots are used by the existing serialized
   network work: sync and installs quiesce the media thread. */
#define IDLE_SLOTS 3
#define IDLE_MS 30000
struct connection {
    int sock;
    WOLFSSL_CTX *ctx;
    WOLFSSL *ssl;
    char host[128];
    unsigned short port;
    unsigned idle_at;
};
static struct connection *g_idle[IDLE_SLOTS];

static void close_connection(struct connection *c, int graceful) {
    if (!c) return;
    if (c->ssl) {
        if (graceful) wolfSSL_shutdown(c->ssl);
        wolfSSL_free(c->ssl);
    }
    if (c->ctx) wolfSSL_CTX_free(c->ctx);
    if (c->sock >= 0) sceNetInetClose(c->sock);
    free(c);
}

static void close_idle(void) {
    for (int i = 0; i < IDLE_SLOTS; i++) {
        close_connection(g_idle[i], 0);
        g_idle[i] = NULL;
    }
}

static struct connection *take_idle(const struct url *u) {
    for (int i = 0; i < IDLE_SLOTS; i++) {
        struct connection *c = g_idle[i];
        if (!c) continue;
        if (expired(c->idle_at, IDLE_MS)) {
            close_connection(c, 0);
            g_idle[i] = NULL;
        } else if (c->port == u->port && !strcmp(c->host, u->host)) {
            g_idle[i] = NULL;
            return c;
        }
    }
    return NULL;
}

static void save_idle(struct connection *c) {
    int slot = -1;
    for (int i = 0; i < IDLE_SLOTS; i++)
        if (!g_idle[i]) { slot = i; break; }
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < IDLE_SLOTS; i++)
            if ((unsigned)(now_ms() - g_idle[i]->idle_at) >
                (unsigned)(now_ms() - g_idle[slot]->idle_at))
                slot = i;
        close_connection(g_idle[slot], 0);
    }
    c->idle_at = now_ms();
    g_idle[slot] = c;
}

/* One HTTP request, possibly over an idle connection. Fills head[] and streams
   the body. Returns: 0 complete, 1 truncated, <0 failed before the body.
   On a 3xx with Location, *redirect is filled and 2 is returned. */
static int one_request(const struct url *u, https_sink sink, void *sink_ctx,
                       https_progress progress, void *progress_ctx,
                       struct https_result *res, struct url *redirect, int *stale) {
    int sock = -1, rc, ret = -1;
    WOLFSSL_CTX *ctx = NULL;
    WOLFSSL *ssl = NULL;
    struct connection *conn = take_idle(u);
    int reused = conn != NULL;
    int can_keep = 0;
    static char buf[16 * 1024];
    static char head[HEAD_MAX];
    size_t headlen = 0;

    *stale = 0;
    res->status = 0;
    res->body_len = 0;
    res->content_length = 0;
    res->truncated = 0;
    if (conn) {
        sock = conn->sock;
        ctx = conn->ctx;
        ssl = conn->ssl;
        res->handshake_ms = 0;
        logline("https: reuse %s", u->host);
        goto request;
    }
    struct in_addr ip;
    phase("dns");
    if (resolve(u->host, &ip) < 0) { logline("dns failed: %s", u->host); return -1; }
    phase("connect");

    conn = calloc(1, sizeof(*conn));
    if (!conn) return -1;
    conn->sock = -1;
    snprintf(conn->host, sizeof(conn->host), "%s", u->host);
    conn->port = u->port;

    sock = sceNetInetSocket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { logline("socket failed"); ret = -2; goto out; }
    conn->sock = sock;

    /* No portable O_NONBLOCK here, and SO_NONBLOCK / SO_ERROR are not in the
       headers -- using them picks up constants from elsewhere and configures
       the wrong option. The stack behaves as non-blocking (recv reports
       EAGAIN), which is what the IO callbacks are written for. */
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(u->port);
    sa.sin_addr = ip;

    /* A non-blocking connect returns at once with EINPROGRESS. With no
       SO_ERROR and no usable select, the way to learn that it finished is
       to ask again: the stack answers EALREADY while it is still at it and
       EISCONN (or 0) once the connection stands. */
    unsigned start = now_ms();
    if (sceNetInetConnect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        int e = sceNetInetGetErrno();
        if (e != EINPROGRESS && e != EALREADY && e != EWOULDBLOCK) {
            logline("connect failed errno=%d", e);
            goto out;
        }
        for (;;) {
            sceKernelDelayThread(20 * 1000);
            if (sceNetInetConnect(sock, (struct sockaddr *)&sa, sizeof(sa)) == 0) break;
            e = sceNetInetGetErrno();
            if (e == EISCONN) break;
            if (e != EINPROGRESS && e != EALREADY && e != EWOULDBLOCK) {
                logline("connect failed errno=%d", e);
                goto out;
            }
            if (expired(start, CONNECT_TIMEOUT_MS)) { logline("connect timeout"); goto out; }
        }
    }
    stir_power(now_ms() - start);

    if (!g_wolf_ready) {
        int irc = wolfSSL_Init();
        if (irc != WOLFSSL_SUCCESS) {
            logline("wolfssl %s init=%d", wolfSSL_lib_version(), irc);
            goto out;
        }
        g_wolf_ready = 1;
    }

    ctx = wolfSSL_CTX_new(wolfTLSv1_3_client_method());
    conn->ctx = ctx;
    if (!ctx) { logline("no TLS 1.3 in this build"); goto out; }
    if (g_suites && wolfSSL_CTX_set_cipher_list(ctx, g_suites) != WOLFSSL_SUCCESS)
        logline("cipher list rejected: %s", g_suites);

    /* Nothing here is signed, so the chain is the only thing standing between a
       hostile access point and an EBOOT of its choosing: it would only have to
       serve its own catalog, its own manifest, and a ZIP whose sha256 matches
       the hash in that manifest. The roots are compiled in -- Sony's store is
       from 2007 and expired long ago. */
    if (wolfSSL_CTX_load_verify_buffer(ctx, (const unsigned char *)PSPDX_CA_PEM,
                                       (long)sizeof(PSPDX_CA_PEM) - 1,
                                       WOLFSSL_FILETYPE_PEM) != WOLFSSL_SUCCESS) {
        logline("CA bundle rejected");
        goto out;
    }
    wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_PEER, verify_ignoring_dates);
    wolfSSL_CTX_SetIORecv(ctx, io_recv);
    wolfSSL_CTX_SetIOSend(ctx, io_send);

    /* X25519 costs a fraction of P-256 on a core with no crypto hardware, and
       offering its key share up front avoids a HelloRetryRequest, which would
       be an entire extra round trip. */
    static int groups[] = { WOLFSSL_ECC_X25519, WOLFSSL_ECC_SECP256R1 };
    if (wolfSSL_CTX_set_groups(ctx, groups, 2) != WOLFSSL_SUCCESS)
        logline("x25519 unavailable, using default groups");

    ssl = wolfSSL_new(ctx);
    conn->ssl = ssl;
    if (!ssl) { logline("wolfSSL_new failed"); goto out; }
    wolfSSL_SetIOReadCtx(ssl, &conn->sock);
    wolfSSL_SetIOWriteCtx(ssl, &conn->sock);
    if (wolfSSL_UseSNI(ssl, WOLFSSL_SNI_HOST_NAME, u->host,
                       (unsigned short)strlen(u->host)) != WOLFSSL_SUCCESS)
        logline("SNI rejected");
    /* Without this a valid certificate for any other host would pass. */
    if (wolfSSL_check_domain_name(ssl, u->host) != WOLFSSL_SUCCESS) {
        logline("cannot pin domain name");
        goto out;
    }
    if (wolfSSL_UseKeyShare(ssl, WOLFSSL_ECC_X25519) != WOLFSSL_SUCCESS)
        logline("x25519 key share unavailable");

    phase("tls handshake");
    start = now_ms();
    while ((rc = wolfSSL_connect(ssl)) != WOLFSSL_SUCCESS) {
        int e = wolfSSL_get_error(ssl, rc);
        if (e != WOLFSSL_ERROR_WANT_READ && e != WOLFSSL_ERROR_WANT_WRITE) {
            char msg[80];
            wolfSSL_ERR_error_string((unsigned long)e, msg);
            logline("handshake failed %d: %s", e, msg);
            goto out;
        }
        if (expired(start, HANDSHAKE_TIMEOUT_MS)) { logline("handshake timeout"); goto out; }
        wait_socket(1);
    }
    res->handshake_ms = now_ms() - start;
    {
        const char *group = wolfSSL_get_curve_name(ssl);
        const char *cipher = wolfSSL_get_cipher(ssl);
        logline("%s %s %s %u ms", u->host, cipher,
                group ? group : "?", res->handshake_ms);
        /* The same four facts the log gets, kept for the info panel: what
           was negotiated is only knowable here, while the session is open. */
        snprintf(g_last.host, sizeof(g_last.host), "%s", u->host);
        snprintf(g_last.cipher, sizeof(g_last.cipher), "%s", cipher ? cipher : "?");
        snprintf(g_last.group, sizeof(g_last.group), "%s", group ? group : "?");
        g_last.handshake_ms = res->handshake_ms;
    }

request:
    phase("request");
    pace_begin();
    int reqlen = snprintf(buf, sizeof(buf),
                          "GET %s HTTP/1.1\r\n"
                          "Host: %s\r\n"
                          "User-Agent: pspdx/0.0\r\n"
                          "Connection: keep-alive\r\n\r\n", u->path, u->host);
    if (reqlen <= 0 || reqlen >= (int)sizeof(buf)) { logline("request too long"); goto out; }

    start = now_ms();
    for (int sent = 0; sent < reqlen; ) {
        rc = wolfSSL_write(ssl, buf + sent, reqlen - sent);
        if (rc > 0) { sent += rc; continue; }
        int e = wolfSSL_get_error(ssl, rc);
        if (e != WOLFSSL_ERROR_WANT_READ && e != WOLFSSL_ERROR_WANT_WRITE) {
            logline("write failed %d", e);
            if (reused) *stale = 1;
            goto out;
        }
        if (expired(start, STALL_TIMEOUT_MS)) { logline("write timeout"); goto out; }
        wait_socket(1);
    }

    /* Read until the head is complete, then hand the rest to the sink. */
    const char *body_start = NULL;
    const char *leftover = NULL;
    size_t leftover_len = 0;
    size_t want = 0;
    int have_length = 0, chunked = 0;
    start = now_ms();
    for (;;) {
        rc = wolfSSL_read(ssl, buf, (int)sizeof(buf));
        if (rc > 0) {
            start = now_ms();
            const char *data = buf;
            size_t len = (size_t)rc;

            if (!body_start) {
                /* Only what fits is copied; a read that carries the head plus
                   megabytes of body is normal and must not be refused. */
                size_t room = sizeof(head) - headlen;
                size_t take = len < room ? len : room;
                memcpy(head + headlen, data, take);
                headlen += take;
                const char *sep = mem_find(head, headlen, "\r\n\r\n", 4);
                if (!sep) {
                    if (headlen == sizeof(head)) { logline("http: head too large"); goto out; }
                    continue;
                }
                leftover = data + take;
                leftover_len = len - take;

                size_t hl = (size_t)(sep - head) + 4;
                if (sscanf(head, "HTTP/%*d.%*d %ld", &res->status) != 1) {
                    logline("http: bad status line");
                    goto out;
                }
                const char *cl = header(head, hl, "Content-Length");
                if (cl) { want = (size_t)strtoul(cl, NULL, 10); have_length = 1; }
                res->content_length = want;
                const char *te = header(head, hl, "Transfer-Encoding");
                if (te && strncasecmp(te, "chunked", 7) == 0) chunked = 1;
                const char *connection = header(head, hl, "Connection");
                const char *connection_end = connection
                    ? mem_find(connection, (size_t)(head + hl - connection), "\r\n", 2) : NULL;
                can_keep = have_length && !chunked &&
                    !strncmp(head, "HTTP/1.1 ", 9) &&
                    !connection_closes(connection, connection_end);

                if (res->status >= 300 && res->status < 400) {
                    const char *loc = header(head, hl, "Location");
                    if (loc) {
                        const char *eol = mem_find(loc, (size_t)(head + hl - loc), "\r\n", 2);
                        if (eol && url_resolve(u, loc, (size_t)(eol - loc), redirect) == 0) {
                            logline("http %ld -> %s", res->status, redirect->host);
                            ret = 2;
                            goto out;
                        }
                    }
                }
                if (chunked) {
                    /* GitHub serves everything we ask for with a length;
                       a chunk decoder is not worth its bytes until it is not. */
                    logline("http: chunked not supported");
                    goto out;
                }
                logline("http %ld, %lu bytes announced", res->status, (unsigned long)want);
                phase("download");
                if (progress) progress(progress_ctx, 0, want);

                /* Whatever followed the head in this read is body. */
                body_start = head + hl;
                data = body_start;
                len = headlen - hl;
                ret = 1;                                 /* body has begun */
                if (len == 0 && leftover_len == 0) {
                    if (have_length && want == 0) { ret = 0; goto out; }
                    continue;
                }
            }

            /* Two pieces on the read that completed the head: the tail of the
               buffer it was copied into, then what did not fit. */
            for (int piece = 0; piece < 2; piece++) {
                if (piece == 1) {
                    if (leftover_len == 0) break;
                    data = leftover;
                    len = leftover_len;
                    leftover_len = 0;
                }
                if (len == 0) continue;

                /* Anything past Content-Length is not part of this message. */
                if (have_length && res->body_len + len > want) {
                    logline("http: %lu bytes past content-length, ignored",
                            (unsigned long)(res->body_len + len - want));
                    len = want - res->body_len;
                }
                if (len && sink && sink(sink_ctx, data, len) != 0) {
                    logline("sink aborted");
                    goto out;
                }
                res->body_len += len;
                if (progress) progress(progress_ctx, res->body_len, want);
                if (g_abort) { logline("http: aborted"); goto out; }
                if (have_length && res->body_len >= want) { ret = 0; goto out; }
            }
            continue;
        }

        int e = wolfSSL_get_error(ssl, rc);
        if (e == WOLFSSL_ERROR_NONE || e == WOLFSSL_ERROR_ZERO_RETURN) {
            /* Clean close: complete unless a length says otherwise. */
            if (body_start && (!have_length || res->body_len >= want)) ret = 0;
            else if (!body_start) {
                logline("http: closed before head");
                if (reused) *stale = 1;
            }
            goto out;
        }
        if (e != WOLFSSL_ERROR_WANT_READ && e != WOLFSSL_ERROR_WANT_WRITE) {
            /* A reset after the body has arrived is common enough to tolerate,
               but it must not be reported as a clean read. */
            logline("read error %d after %lu bytes", e, (unsigned long)res->body_len);
            if (body_start && have_length && res->body_len >= want) ret = 0;
            goto out;
        }
        if (expired(start, STALL_TIMEOUT_MS)) { logline("read stalled"); goto out; }
        wait_socket(1);
    }

out:
    if (ret == 1) res->truncated = 1;
    if (reused && ret < 0 && !res->body_len && !headlen)
        *stale = 1;
    if (ret == 0 && can_keep) save_idle(conn);
    else close_connection(conn, ret == 0 || ret == 2);
    return ret;
}

int https_get(const char *url, https_sink sink, void *sink_ctx,
              https_progress progress, void *progress_ctx,
              struct https_result *out) {
    struct url u, next;
    struct https_result res;
    memset(&res, 0, sizeof(res));
    if (out) *out = res;                      /* callers log it either way */
    if (url_parse(url, &u) < 0) return -1;
    g_abort = 0;

    for (res.redirects = 0; ; res.redirects++) {
        int stale = 0;
        int rc = one_request(&u, sink, sink_ctx, progress, progress_ctx, &res, &next, &stale);
        if (stale && !res.body_len) {
            /* A server may close an idle connection just before our next GET.
               Retrying is safe only before any response body reached the sink. */
            rc = one_request(&u, sink, sink_ctx, progress, progress_ctx, &res, &next, &stale);
        }
        if (rc != 2) {
            strncpy(res.host, u.host, sizeof(res.host) - 1);
            if (out) *out = res;
            return rc;
        }
        if (res.redirects >= MAX_REDIRECTS) {
            logline("too many redirects");
            strncpy(res.host, u.host, sizeof(res.host) - 1);
            if (out) *out = res;
            return -3;
        }
        u = next;
    }
}
