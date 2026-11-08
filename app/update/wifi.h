#ifndef PSPDX_WIFI_H
#define PSPDX_WIFI_H
/* The console's own network settings, used without the system's dialog:
   for Auto Connect, which joins one of them in the background. */
#include <stddef.h>

#define WIFI_PROFILES 10

/* The name a connection has in the system's settings (1 to 10), into out.
   0, or -1 when there is no connection under that number. */
int wifi_profile_name(int profile, char *out, size_t size);
/* The first connection there is after profile (0 for the first of all), or
   0 when there is none after it. */
int wifi_profile_next(int profile);
/* Worker: the radio up and the connection joined, waiting up to twenty
   seconds for an address. 0 joined, -1 not. Afterwards https_net_connect
   adopts the link. */
int wifi_join(int profile);
#endif
