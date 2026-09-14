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
static unsigned operations;
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
int sceIoOpen(const char *p, int flags, int mode) { return open(p, flags, mode); }
int sceIoClose(int f) { return close(f); }
int sceIoRead(int f, void *b, size_t n) { return read(f, b, n); }
int sceIoWrite(int f, const void *b, size_t n) {
    const char *fail = getenv("WRITE_FAIL");
    if (fail)
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
    if (access(to, F_OK) == 0)
        return -1;
    int rc = rename(from, to);
    tick();
    return rc;
}
int sceIoRemove(const char *p) {
    int rc = unlink(p);
    tick();
    return rc;
}
int sceIoRmdir(const char *p) {
    int rc = rmdir(p);
    tick();
    return rc;
}
int sceIoMkdir(const char *p, int mode) {
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
    tick();
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
unsigned now_ms(void) { return 0; }
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
void https_abort(void) {}
enum https_outcome https_get(const char *url, https_sink sink, void *ctx, https_progress cb, void *pc,
              struct https_result *r) {
    (void)cb;
    (void)pc;
    struct https_result ignored;
    if (!r) r = &ignored;
    memset(r, 0, sizeof(*r));
    FILE *requests = fopen("requests.log", "a");
    fprintf(requests, "%s\n", url);
    fclose(requests);
    const char *path = NULL;
    if (getenv("OFFLINE"))
        return -1;
    if (strstr(url, "download.zip"))
        path = getenv("ZIP_FILE");
    else if (strstr(url, "catalog.json")) {
        if (!getenv("CATALOG_DOWN"))
            path = "catalog.json";
    } else if (strstr(url, "catalog.txt")) {
        if (!getenv("LIST_DOWN"))
            path = "catalog.txt";
    } else if (strstr(url, "/.pspdx"))
        path = "manifest.json";
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
    while ((n = fread(b, 1, sizeof(b), f))) {
        if (sink && sink(ctx, b, n) != 0) {
            rc = HTTPS_TRUNCATED;
            break;
        }
        r->body_len += n;
        if (cb) cb(pc, r->body_len, 0);
    }
    if (ferror(f)) rc = HTTPS_TRUNCATED;
    r->truncated = rc == HTTPS_TRUNCATED;
    fclose(f);
    return rc;
}
