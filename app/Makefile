TARGET = pspdx
OBJS = main.o gui/screen.o gui/entropy_screen.o gui/gfx.o gui/font.o \
       gui/image.o gui/lattice.o gui/title.o gui/preview.o gui/icons.o gui/marks.o gui/shell.o gui/files_view.o gui/osk.o logic/entropy.o network/https.o network/bench.o \
       update/catalog.o update/sources.o update/pspdx.o update/inbox.o update/assets.o update/sync.o \
       install/install.o install/state.o util/storage.o util/files.o session/view.o \
       install/zipread.o audio/audio.o audio/synth.o audio/music.o \
       audio/cues.o video/mp4.o video/psmf.o video/player.o util/runtime.o util/pbp.o

WOLFSSL_PREFIX = $(CURDIR)/wolfssl-psp/prefix

# Which build this is: the tag if the checkout is on one, the commit if it is
# not. It is shown in the info band and written into the record PSPDX keeps
# about itself, and that record's version is held against the catalog's, which
# is the release tag with its "v" taken off -- so the "v" comes off here too,
# or the two strings would never agree and the client would offer itself an
# update for ever. The Docker build mounts app/ alone and sees no .git, so it
# falls back to "dev"; dev/release passes the version in from the host.
PSPDX_DESCRIBE := $(shell git describe --tags --always 2>/dev/null || echo dev)
PSPDX_VERSION ?= $(PSPDX_DESCRIBE:v%=%)

# -I. only: every local include names its directory, so the layer a header
# comes from is visible at the include line.
# -MMD writes a .d next to every .o so that a header change rebuilds what
# includes it; build.mak on its own only looks at the .c.
# EXTRA_CFLAGS is for a test build: tools/localcat points the client at a
# catalog on the host and trusts the host's throwaway CA through it.
CFLAGS = -O2 -G0 -Wall -I. -I$(WOLFSSL_PREFIX)/include -MMD -MP $(EXTRA_CFLAGS)
ASFLAGS = $(CFLAGS)

LIBDIR = $(WOLFSSL_PREFIX)/lib
# intraFont draws through the GE, so it has to come before -lpspgu; libpng
# leans on the zlib that is already here for the package archives.
LIBS = -lwolfssl -lcjson -lintrafont -lpng -lz -lm \
       -lpspgu -lpspgum -lpspmpeg -lpspaudio -lpspsemaphore_660 -lpsppower \
       -lpspnet_resolver -lpspnet_apctl -lpspnet_inet -lpspnet \
       -lpsputility -lpspdebug -lpspdisplay -lpspge -lpspctrl -lpspatrac3

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = PSPDX
# The picture the XMB shows beside the title, and the one the catalog lists
# the client under: the same file, 144x80, as every PSP package carries.
PSP_EBOOT_ICON = assets/icon0.png
# The rest of what Sony put in a PBP, and what the catalog takes out of one:
# the picture on the card, a six-second film, the tune under the card.
PSP_EBOOT_PIC1 = assets/pic1.png
PSP_EBOOT_ICON1 = assets/icon1.pmf
PSP_EBOOT_SND0 = assets/snd0.at3

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

# After build.mak, so its all: stays the default goal.
-include $(OBJS:.o=.d)

# The version reaches the code through a generated header rather than a -D:
# a define changes nothing make can see, so a new version never recompiled
# the files that carried the old one. The header is rewritten only when the
# string differs, and the .d files make what includes it follow.
util/version.h: FORCE
	@printf '#define PSPDX_VERSION "%s"\n' '$(PSPDX_VERSION)' > $@.tmp; \
	if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; echo "version $(PSPDX_VERSION)"; fi
FORCE:
$(OBJS): | util/version.h
