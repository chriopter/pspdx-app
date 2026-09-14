#!/bin/sh
# Builds wolfSSL for the PSP into ./prefix. Run inside the pspdev container:
#   docker run --rm -v "$PWD:/src" -w /src pspdev/pspdev:latest sh wolfssl-psp/build.sh
#
# Two departures from the stock pspdev package, both build flags -- the source
# is untouched upstream:
#
#   CUSTOM_RAND_     Makes wc_GenerateSeed() a wrapper around a function the
#   GENERATE_SEED    application provides, so the /dev/urandom branch -- which
#                    the PSP does not have, and which makes wolfCrypt_Init()
#                    die with WC_INIT_E (-228) -- is never compiled in.
#                    The runtime hook wc_SetSeed_Cb() cannot be used for this:
#                    wolfSSL_Init() overwrites it with wc_GenerateSeed again
#                    (src/ssl.c:3224 in 5.9.2) before anything of ours can
#                    take effect.
#
#   ALT_CERT_        Trust the chain as soon as a certificate in it is one we
#   CHAINS           already hold, instead of insisting that every certificate
#                    above it also verifies. Public CAs cross-sign: GitHub
#                    Pages sends a chain that ends in a Let's Encrypt root
#                    cross-signed by ISRG, and verifying that last signature
#                    fails with ASN_SIG_CONFIRM_E (-155) even though ISRG Root
#                    X1 is right there in our bundle and already anchored the
#                    intermediate below it.
#
#   SP_INT_BITS      The big-integer backend sizes itself from what is compiled
#   4096             in, and with only RSA and no large FFDHE parameters it
#                    settles on 3072 bits (sp_int.h, the "must be SP math all"
#                    branch). Every RSA-4096 signature then fails to verify with
#                    ASN_SIG_CONFIRM_E (-155) -- including the one ISRG Root X1
#                    puts on the chain GitHub Pages serves. A size limit that
#                    presents itself as a forged certificate is the worst kind.
#                    WOLFSSL_SP_4096 alone does not do it: that branch is only
#                    consulted when SP RSA is built, which it is not here.
#
#   WOLFSSL_USER_IO  The application supplies the socket callbacks -- https.c
#                    installs its own on every context, since the SDK's BSD
#                    wrappers do not work -- so wolfSSL's built-in EmbedSend
#                    and EmbedReceive are left out. From 5.9 that is also
#                    what keeps the IPv6 alt-name matcher out, which wants a
#                    struct sockaddr_in6 the PSP headers do not have. IPv4
#                    alt names (the local test CA's IP:127.0.0.1) are matched
#                    as strings and are not affected.
#
#   CURVE25519 &c.   X25519 costs a fraction of P-256 on a core with no crypto
#                    hardware -- measured here, five key exchanges: 154 ms
#                    against 1032 ms. Ed25519 is for package signatures,
#                    ChaCha20-Poly1305 for bulk without AES acceleration.
set -e

VER=5.9.2
# The pin. A tag is a pointer and can be moved or deleted; only the hash says
# which bytes we actually built against. Computed here from the tarball the
# release page serves, not copied from a web page.
SHA=2f4ef3d4fd387a9b3191d36a6316d69116c46ff69bb9583b6c82b36d7b8ca114
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/prefix"

WORK="$HERE/work"
mkdir -p "$WORK" && cd "$WORK"
TARBALL="v${VER}-stable.tar.gz"
[ -f "$TARBALL" ] || wget -q "https://github.com/wolfSSL/wolfssl/archive/refs/tags/$TARBALL"

echo "$SHA  $TARBALL" | sha256sum -c - || {
    echo "wolfssl tarball does not match the pinned hash -- refusing to build" >&2
    exit 1
}
rm -rf "wolfssl-${VER}-stable"
tar xf "v${VER}-stable.tar.gz"
cd "wolfssl-${VER}-stable"

# The departures above, as the compiler sees them. One list, because the
# client has to be shown the same one: see the end of this script.
DEFS="NO_WRITEV NO_DEV_RANDOM WOLFSSL_USER_IO SP_INT_BITS=4096 WOLFSSL_ALT_CERT_CHAINS CUSTOM_RAND_GENERATE_SEED=psprandom_seed_raw"
CFLAGS="${EXTRA_CFLAGS:-} -include $HERE/psprandom_decl.h"
for d in $DEFS; do CFLAGS="$CFLAGS -D$d"; done

mkdir -p build && cd build
CFLAGS="$CFLAGS" \
  cmake -Wno-dev \
    -DCMAKE_TOOLCHAIN_FILE="$PSPDEV/psp/share/pspdev.cmake" \
    -DCMAKE_INSTALL_PREFIX="$OUT" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DWOLFSSL_CRYPT_TESTS=OFF -DWOLFSSL_EXAMPLES=OFF \
    -DWOLFSSL_CURL=ON \
    -DWOLFSSL_CURVE25519=yes -DWOLFSSL_ED25519=yes \
    -DWOLFSSL_CHACHA=yes -DWOLFSSL_POLY1305=yes \
    -DWARNING_C_FLAGS=-w .. >/dev/null
make -j"$(nproc)"
make install >/dev/null 2>&1

# options.h is the library's account of how it was built, and the client
# compiles against it. Up to 5.7 cmake copied every -D from CFLAGS into it;
# from 5.9 it is filled in from a fixed template of known options, and ours
# fall through. Without SP_INT_BITS there the client would size big integers
# differently from the library it links, so the list goes in by hand, before
# the closing extern "C".
OPTS="$OUT/include/wolfssl/options.h"
FRAG=""
for d in $DEFS; do
    FRAG="$FRAG#undef  ${d%%=*}
#define $(echo "$d" | tr '=' ' ')
"
done
awk -v frag="$FRAG" '
    { line[NR] = $0; if ($0 ~ /^#ifdef __cplusplus/) last = NR }
    END { for (i = 1; i <= NR; i++) { if (i == last) print frag; print line[i] } }
' "$OPTS" > "$OPTS.tmp" && mv "$OPTS.tmp" "$OPTS"

echo "=== flags im ergebnis ==="
for d in WOLFSSL_TLS13 HAVE_CURVE25519 HAVE_ED25519 HAVE_CHACHA OPENSSL_EXTRA WOLFSSL_ALT_CERT_CHAINS SP_INT_BITS WOLFSSL_USER_IO; do
  printf '%-18s ' "$d"
  grep -qE "^#define $d" "$OUT/include/wolfssl/options.h" && echo AN || echo aus
done
