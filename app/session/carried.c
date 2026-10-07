#include "session/carried.h"

#ifdef PSPDX_USBNET_CARRIED
#include "session/usbnet_carried.h"

int carried_plugin(const unsigned char **prx, size_t *len, const char **tag,
                   const unsigned char **zip_sha256) {
    *prx = usbnet_carried_prx;
    *len = sizeof(usbnet_carried_prx);
    *tag = usbnet_carried_tag;
    *zip_sha256 = usbnet_carried_zip;
    return 0;
}
#else
int carried_plugin(const unsigned char **prx, size_t *len, const char **tag,
                   const unsigned char **zip_sha256) {
    (void)prx;
    (void)len;
    (void)tag;
    (void)zip_sha256;
    return -1;
}
#endif
