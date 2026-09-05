#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cc -O2 -Wall -Wextra -Werror -Wno-deprecated-declarations ${CFLAGS:-} "$HERE/itwebproxy.c" -lcurl -o "$HERE/itwebproxy"
