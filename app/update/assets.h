#ifndef PSPDX_ASSETS_H
#define PSPDX_ASSETS_H

#include <stddef.h>

/* What a catalog entry links to: its icon, a still, a moving one, and a
   sound. Every fetch costs a TLS handshake, and on a PSP over 802.11b that
   is the expensive part, so an asset is fetched once and then read off the
   stick under PSP/PSPDX/cache for the life of the installation.

   The cache is consulted before the URL, so what is on the stick is shown
   even when the catalog no longer links it -- and a file planted there by
   hand is shown too, which is how the test rig feeds the player. */

enum asset_kind {
    ASSET_ICON,         /* PNG, the bundle's 144x80 ICON0 */
    ASSET_SHOT,         /* PNG, at most 480x272 */
    ASSET_VIDEO,        /* an ICON1.PMF, or an H.264 baseline MP4 the client wraps */
    ASSET_SOUND         /* SND0.AT3: ATRAC3, a short loop */
};

/* Returns a pointer into a buffer owned here, valid until the next call --
   one asset is being looked at at a time, so there is no reason to hold
   two. url may be empty, in which case only the cache is tried. Returns
   NULL and logs if there is nothing to show. */
const void *asset_fetch(enum asset_kind kind, const char *id, const char *url,
                        int cached_only, size_t *len);

#endif
