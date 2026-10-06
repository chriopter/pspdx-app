#ifndef PSPDX_NETWORK_USBNET_H
#define PSPDX_NETWORK_USBNET_H

/* Network over the USB cable (pspkit-usbnet): what only the kernel can say
 * or do about its module, usbnet.prx. Nothing here decides whether the
 * cable is wanted, and nothing is loaded at start by itself: session/cable.h
 * asks, installs the plugin the release carries, and calls this.
 *
 * With the module loaded, a connection "Hi-Speed USB", made once in the
 * system's connection dialog, connects over the cable to the gateway on a
 * PC. */

/* Whether the kernel can be asked at all: custom firmware with its bridge.
   0 on an emulator, where nothing here does anything. */
int usbnet_kernel(void);
/* Whether the module is loaded, by the firmware as a plugin or by PSPDX. */
int usbnet_loaded(void);
/* Loads and starts the usbnet.prx at path for this session, unless the
   module is there already: 0 when it is loaded afterwards. */
int usbnet_load(const char *path);
/* Whether the gateway on the PC answers: asked of the module, which looks
   for up to ms milliseconds, taking USB for it as a connection would, and
   returns as soon as it knows. 1 it is there, 0 it is not (or the cable is
   out, or USB storage has the port), -1 the module cannot say: none loaded,
   or one from before it had its device "usbnet:". Blocks; nothing is drawn
   meanwhile, so it wants a thread of its own. */
int usbnet_probe(unsigned ms);

#endif
