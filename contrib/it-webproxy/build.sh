#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# Static OpenSSL is bundled into the helper; users install no extra runtime.
OPENSSL_PREFIX=${OPENSSL_PREFIX:-"$HERE/../../../qemu-ios-deps12"}
[ -f "$OPENSSL_PREFIX/lib/libssl.a" ] && [ -f "$OPENSSL_PREFIX/lib/libcrypto.a" ] || {
    echo "Set OPENSSL_PREFIX to a compatible static OpenSSL build" >&2; exit 1;
}
WEATHER_OBJECT=$(mktemp "${TMPDIR:-/tmp}/itweather.XXXXXX")
trap 'rm -f "$WEATHER_OBJECT"' EXIT HUP INT TERM
cc -O2 -fobjc-arc -Wall -Wextra -Werror -Wno-deprecated-declarations ${CFLAGS:-} \
    -c "$HERE/weather.m" -o "$WEATHER_OBJECT"
cc -O2 -Wall -Wextra -Werror -Wno-deprecated-declarations ${CFLAGS:-} \
    -DHAVE_OPENSSL -DHAVE_WEATHER -I"$OPENSSL_PREFIX/include" "$HERE/itwebproxy.c" \
    "$OPENSSL_PREFIX/lib/libssl.a" "$OPENSSL_PREFIX/lib/libcrypto.a" "$WEATHER_OBJECT" -framework Foundation -lcurl -o "$HERE/itwebproxy"
