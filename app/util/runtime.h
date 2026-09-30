#ifndef PSPDX_RUNTIME_H
#define PSPDX_RUNTIME_H

#include <stddef.h>

/* Which build this is, as a string: git describe, handed down by the Makefile.
   Everything that prints a version or writes one down takes it from here, so
   the band, the log and the record on the stick cannot disagree. A build from
   outside a checkout has no answer and says so. */
/* Written by the Makefile from git describe, or "dev" where there is no
   git to ask. */
#include "util/version.h"
#ifndef PSPDX_VERSION
#define PSPDX_VERSION "dev"
#endif

void logline(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_dump(void);

/* The same, from a thread of its own below everything else: a frame does
   not wait for the stick. */
void log_dump_later(void);
int log_count(void);
/* Copies a terminated line so concurrent logging cannot overwrite it. */
void log_at(int index, char *out, size_t size);

unsigned now_ms(void);
unsigned now_us(void);
int expired(unsigned start, unsigned budget_ms);

/* Seconds since 1970 by the console's clock. Not time(): on a PSP-1000
   under ARK newlib's time() came back as the seconds since midnight, so
   anything kept "for a week" or "a day old" was wrong by the date. */
unsigned wall_time(void);

#endif
