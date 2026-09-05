#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# Static OpenSSL is bundled into the helper; users install no extra runtime.
OPENSSL_PREFIX=${OPENSSL_PREFIX:-"$HERE/../../../qemu-ios-deps12"}
[ -f "$OPENSSL_PREFIX/lib/libssl.a" ] && [ -f "$OPENSSL_PREFIX/lib/libcrypto.a" ] || {
    echo "Set OPENSSL_PREFIX to a compatible static OpenSSL build" >&2; exit 1;
}
cc -O2 -Wall -Wextra -Werror -Wno-deprecated-declarations ${CFLAGS:-} \
    -DHAVE_OPENSSL -I"$OPENSSL_PREFIX/include" "$HERE/itwebproxy.c" \
    "$OPENSSL_PREFIX/lib/libssl.a" "$OPENSSL_PREFIX/lib/libcrypto.a" -lcurl -o "$HERE/itwebproxy"
