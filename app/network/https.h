#ifndef PSPDX_HTTPS_H
#define PSPDX_HTTPS_H

#include <stddef.h>

int net_up(void);
void net_down(void);

typedef int (*https_sink)(void *ctx, const void *data, size_t len);
typedef void (*https_progress)(void *ctx, size_t done, size_t total);

struct https_result {
    long status;
    size_t body_len;
    size_t content_length;
    int truncated;
    unsigned handshake_ms;
    int redirects;
    char host[128];
};

/* What the last handshake agreed on. The client shows it, so the strings are
   wolfSSL's own spelling and empty until something has connected. */
struct https_info {
    char host[128];
    char cipher[48];
    char group[24];
    unsigned handshake_ms;
};

const struct https_info *https_last(void);

/* The cipher suites to offer first, in wolfSSL's spelling, or NULL for the
   library's own order. Applies to connections made after the call. */
void https_prefer(const char *suites);

/* What the stack is doing at this moment -- "dns", "tls handshake",
   "download" -- for a status line. */
const char *https_phase(void);

/* Ends the request in progress at its next piece of body; meant for the
   progress callback. The connection is closed rather than kept. */
void https_abort(void);

/* Closes the connections kept for reuse. The next request handshakes
   again -- which is the point when the seed behind it has just changed. */
void https_close_idle(void);

/* A handshake refused on a doubt a person can settle -- a certificate that
   had run out, or one from an issuer this client does not carry -- rather
   than on a proof of tampering. The doubt waits here for the main thread
   to ask about it; https_doubt_take hands it over once and clears it.
   https_trust_anyway makes every later handshake take such a chain, for
   the rest of the run. */
enum https_doubt { HTTPS_DOUBT_NONE, HTTPS_DOUBT_EXPIRED, HTTPS_DOUBT_ISSUER };
enum https_doubt https_doubt_take(char *host, size_t size);
void https_trust_anyway(void);

int https_get(const char *url, https_sink sink, void *sink_ctx,
              https_progress progress, void *progress_ctx,
              struct https_result *out);

#endif
