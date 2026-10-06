#include "network/usbnet.h"
#include <stdio.h>
#include <stdlib.h>
/* The kernel's side of the cable on the host: KERNEL says there is a bridge,
   LOADED that the firmware loaded the module already, and a load is noted
   in the file LOADED_NOTE names, a path a line, for the test to read. */
static int loaded;
int usbnet_kernel(void) { return getenv("KERNEL") != NULL; }
int usbnet_loaded(void) { return usbnet_kernel() && (loaded || getenv("LOADED")); }
int usbnet_load(const char *path) {
    if (!usbnet_kernel() || getenv("LOAD_FAILS"))
        return -1;
    FILE *f = getenv("LOADED_NOTE") && !usbnet_loaded() ? fopen(getenv("LOADED_NOTE"), "a") : NULL;
    if (f) {
        fprintf(f, "%s\n", path);
        fclose(f);
    }
    loaded = 1;
    return 0;
}
