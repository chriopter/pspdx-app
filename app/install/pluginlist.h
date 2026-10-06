#ifndef PSPDX_PLUGINLIST_H
#define PSPDX_PLUGINLIST_H

#include <stddef.h>

/* PLUGINS.TXT, the list ARK-4 loads plugins by, read as bytes in memory:
   nothing here touches the stick, and nothing here makes a new list. What
   comes out is at most one small write for the list as it is: bytes to put
   at one place in it, of the length that is there already, or a line to
   put after its end. The list never shrinks and no line moves.

   A line is

       <run level>, <path>, <on or off>

   and is taken apart the way ARK's own loader does it
   (core/systemctrl/src/plugin.c): any byte below a space ends a line, a
   line that begins with //, ; or # is a comment, the first two commas part
   the fields, the third field ends at another comma or at a comment, and
   spaces around a field are not part of it. on, 1, true and enabled turn a
   plugin on, in any case; every other word turns it off. A path with no
   device in it is beside the list, and paths are told apart without regard
   to case, as the stick tells its files apart.

   path is the plugin's whole path, ms0:/seplugins/usbnet/usbnet.prx.

   Of the lines that name a path only one can be PSPDX's own, and only that
   one is ever written: the line PSPDX added, by its bytes. It is

       always, <path>, on          (written with a space after it)
       always, <path>, off

   or what ARK's two plugin managers make of it when they write the list
   out again, the same with no space after on. Anything else that names
   the path -- another run level, another spelling, a comment after it -- is
   somebody's, and so are two lines that both look like PSPDX's. */

/* Whether ARK loads the plugin, by the last line that names it: 1 on, 0
   off, -1 when no line names it. */
int pluginlist_state(const char *text, size_t len, const char *path);

/* One write: n bytes of bytes at at. */
#define PLUGINLIST_BYTES 96
struct pluginlist_write {
    size_t at, n;
    char bytes[PLUGINLIST_BYTES];
};

/* The line for a plugin, to go after the list's end: turned on, with the
   line end the list uses, and one before it where the list does not end in
   one. 0, or -1 for a path a line cannot hold. */
int pluginlist_add(const char *text, size_t len, const char *path, struct pluginlist_write *w);

/* PSPDX's own line turned on or off: the bytes of its last field, the same
   number of them. 1 with the write, 0 when the line says so already, -1
   when the list has no line that is PSPDX's own. */
int pluginlist_switch(const char *text, size_t len, const char *path, int on,
                      struct pluginlist_write *w);

/* PSPDX's own line taken out: spaces over its bytes, its line end left, so
   that an empty line stays where it was. 1 with the write, -1 when the list
   has no line that is PSPDX's own. */
int pluginlist_blank(const char *text, size_t len, const char *path, struct pluginlist_write *w);

#endif
