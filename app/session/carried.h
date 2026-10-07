#ifndef PSPDX_CARRIED_H
#define PSPDX_CARRIED_H

#include <stddef.h>

/* The cable's plugin as the program carries it: pspkit-usbnet's usbnet.prx
   of the release dev/pack found newest, compiled in (dev/embed usbnet
   writes session/usbnet_carried.h). 0 with its bytes, the tag of the
   release it is of and the SHA-256 of that release's zip, by which a
   catalog lists the release; -1 in a build made without it, which then
   offers no cable. */
int carried_plugin(const unsigned char **prx, size_t *len, const char **tag,
                   const unsigned char **zip_sha256);

#endif
