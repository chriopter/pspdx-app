#include "pspkit-https/https.h"
#include "pspiofilemgr.h"
#include <dirent.h>
#include <errno.h>
#include <openssl/sha.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* The PSP's file calls on the host, with what a test needs to make a stick
   misbehave:

     fault (argument, or FAULT=n)  the n-th mutating call ends the process: a power cut
     FAILAT=n                      the n-th mutating call fails and the run goes on
     STICK_BYTES=n                 a stick of n bytes of files: a write past it fails,
                                   and the free-space call counts what is left
     RO_MATCH=s                    a file whose path holds s is read-only: it cannot be removed
     WRITE_FAIL                    every write fails
     DEVCTL_FAIL                   the stick does not say how much is free
     NET_DROP_AFTER=n              network stops after at least n body bytes

   and a network that answers from files in the working directory; URL_MAP
   (below) names any answer a test wants beside the fixed ones. */
static unsigned operations, failable;
static long fault;
static DIR *dirs[64];
void host_fault(long n) {
    fault = n;
    operations = 0;
}
unsigned host_operations(void) { return operations; }
static void tick(void) {
    operations++;
    if (fault > 0 && operations == (unsigned)fault)
        _exit(77);
}
static int fails(void) {
    const char *at = getenv("FAILAT");
    return at && ++failable == (unsigned)atol(at);
}
static long long tree_bytes(const char *path) {
    struct stat st;
    if (lstat(path, &st) < 0)
        return 0;
    if (!S_ISDIR(st.st_mode))
        return st.st_size;
    long long sum = 0;
    DIR *d = opendir(path);
    for (struct dirent *e; d && (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char sub[4096];
        snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
        sum += tree_bytes(sub);
    }
    if (d)
        closedir(d);
    return sum;
}
/* -1 for a stick without a size. */
static long long room_left(void) {
    const char *size = getenv("STICK_BYTES");
    return size ? atoll(size) - tree_bytes("ms0:") - tree_bytes("ef0:") : -1;
}
int sceIoOpen(const char *p, int flags, int mode) {
    if ((flags & (O_WRONLY | O_RDWR | O_CREAT)) && fails())
        return -1;
    return open(p, flags, mode);
}
int sceIoClose(int f) { return close(f); }
int sceIoRead(int f, void *b, size_t n) { return read(f, b, n); }
int sceIoWrite(int f, const void *b, size_t n) {
    if (getenv("WRITE_FAIL") || fails())
        return -1;
    long long left = room_left();
    if (left >= 0 && (long long)n > left)
        return -1;
    int rc = write(f, b, n);
    tick();
    return rc;
}
SceOff sceIoLseek(int f, SceOff off, int mode) { return lseek(f, off, mode); }
int sceIoLseek32(int f, int off, int mode) { return lseek(f, off, mode); }
int sceIoRename(const char *from, const char *name) {
    /* Match PSP same-directory semantics, including rejecting existing targets. */
    char to[1024];
    snprintf(to, sizeof(to), "%s", from);
    char *slash = strrchr(to, '/');
    if (slash)
        slash[1] = 0;
    else
        to[0] = 0;
    const char *base = strrchr(name, '/');
    strcat(to, base ? base + 1 : name);
    if (access(to, F_OK) == 0 || fails())
        return -1;
    int rc = rename(from, to);
    tick();
    return rc;
}
int sceIoRemove(const char *p) {
    const char *ro = getenv("RO_MATCH");
    if (fails() || (ro && *ro && strstr(p, ro)))
        return -1;
    int rc = unlink(p);
    tick();
    return rc;
}
int sceIoRmdir(const char *p) {
    if (fails())
        return -1;
    int rc = rmdir(p);
    tick();
    return rc;
}
int sceIoMkdir(const char *p, int mode) {
    if (fails())
        return -1;
    int rc = mkdir(p, mode);
    tick();
    return rc;
}
int sceIoGetstat(const char *p, SceIoStat *out) {
    struct stat s;
    if (stat(p, &s) < 0)
        return -1;
    out->st_mode = s.st_mode;
    out->st_size = s.st_size;
    return 0;
}
int sceIoSync(const char *p, int mode) {
    (void)p;
    (void)mode;
    if (fails())
        return -1;
    tick();
    return 0;
}
int sceIoDevctl(const char *dev, unsigned int cmd, void *in, int inlen, void *out, int outlen) {
    (void)dev;
    (void)inlen;
    (void)out;
    (void)outlen;
    struct ms_info {
        unsigned max_clusters, free_clusters, max_sectors, sector_size, sector_count;
    } **info = in;
    if (cmd != 0x02425818 || !info || !*info || getenv("DEVCTL_FAIL"))
        return -1;
    const char *free_on = getenv(!strcmp(dev, "ef0:") ? "EF_FREE" : "MS_FREE");
    long long left = free_on ? atoll(free_on) : room_left();
    /* One byte a cluster, so a test counts in bytes; a stick with no size set
       has a gigabyte free. */
    (*info)->sector_size = (*info)->sector_count = 1;
    (*info)->free_clusters = left < 0 ? 1u << 30 : left > 0 ? (unsigned)left : 0;
    (*info)->max_clusters = getenv("STICK_BYTES") ? (unsigned)atoll(getenv("STICK_BYTES")) : 1u << 30;
    return 0;
}
int sceIoDopen(const char *p) {
    for (int i = 0; i < 64; i++)
        if (!dirs[i]) {
            dirs[i] = opendir(p);
            return dirs[i] ? i : -1;
        }
    return -1;
}
int sceIoDclose(int n) {
    int rc = closedir(dirs[n]);
    dirs[n] = NULL;
    return rc;
}
int sceIoDread(int n, SceIoDirent *out) {
    struct dirent *d = readdir(dirs[n]);
    if (!d)
        return 0;
    snprintf(out->d_name, sizeof(out->d_name), "%s", d->d_name);
    struct stat st;
    fstatat(dirfd(dirs[n]), d->d_name, &st, 0);
    out->d_stat.st_mode = st.st_mode;
    return 1;
}
int sceKernelUtilsSha1Digest(unsigned char *b, size_t n, unsigned char *out) {
    SHA1(b, n, out);
    return 0;
}
/* A clock that stands still, or with CLOCK_STEP_MS moves that far every time
   it is read: what a server that sends a byte now and then looks like. */
unsigned now_ms(void) {
    static unsigned now;
    const char *step = getenv("CLOCK_STEP_MS");
    return step ? (now += (unsigned)atol(step)) : 0;
}
void logline(const char *fmt, ...) {
    if (!getenv("VERBOSE"))
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
int https_net_connect(void) { return getenv("OFFLINE") ? -1 : 0; }
static int net_aborted;
void https_abort(void) { net_aborted = 1; }
static int accept_gzip;
void https_set_accept_gzip(int on) { accept_gzip = on != 0; }
/* The library's bound on a whole get, held against the clock above between
   pieces: FAILED before the body, TRUNCATED after, as the library says. */
static unsigned time_limit;
void https_set_time_limit(unsigned seconds) { time_limit = seconds; }
/* URL_MAP names a file of lines "<part of a URL> <file> [status] [host]": the
   first line whose part the URL holds answers it, from the file ("-" for no
   body), with the status (200 with a file, 404 without) and as though the
   last redirect had ended at the host. */
static int mapped(const char *url, const char **path, struct https_result *r) {
    static char file[1024];
    const char *map = getenv("URL_MAP");
    FILE *f = map ? fopen(map, "r") : NULL;
    char line[2048], part[1024], host[128];
    int hit = 0;
    while (f && !hit && fgets(line, sizeof(line), f)) {
        long status = 0;
        host[0] = 0;
        if (sscanf(line, "%1023s %1023s %ld %127s", part, file, &status, host) < 2 ||
            !strstr(url, part))
            continue;
        hit = 1;
        *path = strcmp(file, "-") ? file : NULL;
        r->status = status ? status : *path ? 200 : 404;
        if (host[0])
            snprintf(r->host, sizeof(r->host), "%s", host);
    }
    if (f)
        fclose(f);
    return hit;
}
enum https_outcome https_get(const char *url, https_sink sink, void *ctx, https_progress cb, void *pc,
              struct https_result *r) {
    net_aborted = 0;
    struct https_result ignored;
    if (!r) r = &ignored;
    memset(r, 0, sizeof(*r));
    FILE *requests = fopen("requests.log", "a");
    fprintf(requests, "%s\n", url);
    fclose(requests);
    /* What asked for gzip, apart from requests.log, whose lines tests hold
       to exactly. A ZIP asked for compressed would be a bug, so it ends the
       run whatever the test was about. */
    if (accept_gzip) {
        FILE *asked = fopen("gzip.log", "a");
        fprintf(asked, "%s\n", url);
        fclose(asked);
        if (strstr(url, ".zip"))
            _exit(3);
    }
    const char *path = NULL;
    if (getenv("OFFLINE"))
        return -1;
    unsigned start = now_ms();
    /* The host the request went to, as the library reports the last one. */
    const char *host = strstr(url, "://");
    host = host ? host + 3 : url;
    snprintf(r->host, sizeof(r->host), "%.*s", (int)strcspn(host, "/?#"), host);
    /* One host that does not answer, while the rest do. */
    const char *down = getenv("DOWN_HOST");
    if (down && *down && strstr(url, down)) {
        r->status = 404;
        return -1;
    }
    if (mapped(url, &path, r)) {
        if (!path || r->status != 200)
            return -1;
    } else if (strstr(url, "download.zip"))
        path = getenv("ZIP_FILE");
    else if (strstr(url, "second/catalog.json") && getenv("SECOND_CATALOG"))
        /* A second catalog beside the first, for what one source's answer
           does to the other's. */
        path = getenv("SECOND_CATALOG");
    else if (strstr(url, "catalog.json")) {
        /* Like GitHub Pages: gzip only to a client that asks, and named in
           the header unless the test wants a server that forgets to. */
        if (getenv("CATALOG_DOWN"))
            ;
        else if (accept_gzip && access("catalog.json.gz", F_OK) == 0) {
            path = "catalog.json.gz";
            if (!getenv("GZIP_UNNAMED"))
                strcpy(r->content_encoding, "gzip");
        } else
            path = "catalog.json";
    } else if (strstr(url, "catalog.txt")) {
        if (!getenv("LIST_DOWN"))
            path = "catalog.txt";
    } else if (strstr(url, "/.pspdx"))
        path = "manifest.json";
    else if (strstr(url, "/releases/tags/") && access("release-tag.json", F_OK) == 0)
        /* The release a tag names, beside the one GitHub calls latest. */
        path = "release-tag.json";
    else if (strstr(url, "/releases/"))
        path = "release.json";
    else if (strstr(url, "api.github.com/repos/"))
        path = "repo.json";
    if (!path) {
        r->status = 404;
        return -1;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        r->status = 404;
        return -1;
    }
    r->status = 200;
    char b[4096];
    size_t n;
    enum https_outcome rc = HTTPS_COMPLETE;
    /* A network stack may hand over a piece of nothing before the first byte. */
    if (getenv("EMPTY_FIRST_PIECE") && sink && sink(ctx, b, 0) != 0)
        rc = HTTPS_TRUNCATED;
    while ((n = fread(b, 1, sizeof(b), f))) {
        if (time_limit && now_ms() - start > time_limit * 1000u) {
            fclose(f);
            r->truncated = r->body_len > 0;
            return r->body_len ? HTTPS_TRUNCATED : HTTPS_FAILED;
        }
        if (sink && sink(ctx, b, n) != 0) {
            rc = HTTPS_TRUNCATED;
            break;
        }
        r->body_len += n;
        if (cb) cb(pc, r->body_len, 0);
        if (net_aborted) { rc = HTTPS_TRUNCATED; break; }
        const char *drop = getenv("NET_DROP_AFTER");
        if (drop && r->body_len >= strtoul(drop, NULL, 10)) {
            rc = HTTPS_TRUNCATED;
            break;
        }
    }
    if (ferror(f)) rc = HTTPS_TRUNCATED;
    r->truncated = rc == HTTPS_TRUNCATED;
    fclose(f);
    return rc;
}
