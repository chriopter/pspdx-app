/* The plugin the program carries, for the tests: what a release compiles in
   is here two files a test writes, carried/usbnet.prx and carried/usbnet.txt
   (tag=, zip= a line each), so that a test can carry any release, a broken
   note, or nothing. Never a file on the stick: beside the EBOOT nothing is
   the plugin's. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session/carried.h"

static int hex(const char *s, unsigned char out[32]) {
    if (strlen(s) != 64) return -1;
    for (int i = 0; i < 32; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1 || strspn(s + 2 * i, "0123456789abcdef") < 2) return -1;
        out[i] = (unsigned char)v;
    }
    return 0;
}

int carried_plugin(const unsigned char **prx, size_t *len, const char **tag,
                   const unsigned char **zip_sha256) {
    static unsigned char data[1 << 16], zip[32];
    static char note[512], name[64];
    FILE *f = fopen("carried/usbnet.prx", "rb");
    if (!f) return -1;
    *len = fread(data, 1, sizeof(data), f);
    fclose(f);
    f = fopen("carried/usbnet.txt", "rb");
    if (!f) return -1;
    note[fread(note, 1, sizeof(note) - 1, f)] = 0;
    fclose(f);
    char hash[80] = "";
    name[0] = 0;
    for (char *line = strtok(note, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        if (!strncmp(line, "tag=", 4)) snprintf(name, sizeof(name), "%s", line + 4);
        if (!strncmp(line, "zip=", 4)) snprintf(hash, sizeof(hash), "%s", line + 4);
    }
    if (!name[0] || hex(hash, zip) < 0) return -1;
    *prx = data;
    *tag = name;
    *zip_sha256 = zip;
    return 0;
}
