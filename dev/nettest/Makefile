TARGET = nettest
OBJS = main.o ../../app/network/https.o ../../app/util/runtime.o ../../app/logic/entropy.o

WOLFSSL_PREFIX = $(CURDIR)/../../app/wolfssl-psp/prefix
# testca.h is forced in first and defines ca_certs.h's guard: the test
# trusts its own CA and nothing else.
CFLAGS = -O2 -G0 -Wall -I../../app -I$(WOLFSSL_PREFIX)/include -include testca.h
ASFLAGS = $(CFLAGS)
LIBDIR = $(WOLFSSL_PREFIX)/lib
LIBS = -lwolfssl -lpspnet_resolver -lpspnet_apctl -lpspnet_inet -lpspnet \
       -lpsputility -lpsprtc -lpsppower
EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = nettest
PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
