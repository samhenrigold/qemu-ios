#!/bin/bash
# Builds qemu-ios's wasm64 host dependencies with Homebrew's emcc, following
# qemu's tests/docker/dockerfiles/emsdk-wasm64-cross.docker (no Docker), plus
# OpenSSL's libcrypto for the AES/PKE models. Output: $ROOT/target.
set -euo pipefail
ROOT=${ROOT:-$HOME/Developer/wasm-spike-deps}
export TARGET=$ROOT/target SRC=$ROOT/src
mkdir -p "$TARGET/lib" "$SRC"
export CPATH="$TARGET/include" PKG_CONFIG_PATH="$TARGET/lib/pkgconfig" EM_PKG_CONFIG_PATH="$TARGET/lib/pkgconfig"
export CFLAGS="-O3 -pthread -DWASM_BIGINT -sMEMORY64=1" CXXFLAGS="-O3 -pthread -DWASM_BIGINT -sMEMORY64=1"
export LDFLAGS="-sWASM_BIGINT -sASYNCIFY=1 -L$TARGET/lib -sMEMORY64=1"
J=$(sysctl -n hw.ncpu)
q() { printf "'%s', " $1 | sed 's/, $//'; }
cat > "$ROOT/cross.meson" <<EOT
[host_machine]
system = 'emscripten'
cpu_family = 'wasm64'
cpu = 'wasm64'
endian = 'little'
[binaries]
c = 'emcc'
cpp = 'em++'
ar = 'emar'
ranlib = 'emranlib'
pkgconfig = ['pkg-config', '--static']
[built-in options]
c_args = [$(q "$CFLAGS -Wno-incompatible-function-pointer-types")]
cpp_args = [$(q "$CFLAGS")]
c_link_args = [$(q "$LDFLAGS")]
cpp_link_args = [$(q "$LDFLAGS")]
EOT
fetch() {
  [ -d "$SRC/$1" ] && return
  mkdir -p "$SRC/$1"
  curl -Ls "$2" -o "$SRC/$1.tar"
  tar xf "$SRC/$1.tar" -C "$SRC/$1" --strip-components=1
}

if [ ! -f "$TARGET/lib/libz.a" ]; then
  fetch zlib https://zlib.net/fossils/zlib-1.3.1.tar.gz
  (cd "$SRC/zlib" && emconfigure ./configure --prefix="$TARGET" --static && emmake make install -j$J AR=emar ARFLAGS=rcs)
fi
if [ ! -f "$TARGET/include/ffi.h" ]; then
  fetch libffi https://github.com/libffi/libffi/releases/download/v3.5.2/libffi-3.5.2.tar.gz
  (cd "$SRC/libffi" && emconfigure ./configure --host=wasm64-unknown-linux \
    --prefix="$TARGET" --enable-static --disable-shared --disable-dependency-tracking --disable-builddir \
    --disable-multi-os-directory --disable-raw-api --disable-docs && emmake make install SUBDIRS=include -j$J)
fi
if [ ! -f "$TARGET/lib/libpixman-1.a" ]; then
  fetch pixman https://cairographics.org/releases/pixman-0.44.2.tar.gz
  (cd "$SRC/pixman" && meson setup _build --prefix="$TARGET" --cross-file="$ROOT/cross.meson" --default-library=static \
    --buildtype=release -Dtests=disabled -Ddemos=disabled && meson install -C _build)
fi
if [ ! -f "$TARGET/lib/libglib-2.0.a" ]; then
  printf '#include <netdb.h>\nint res_query(const char *n,int c,int t,unsigned char *d,int l){h_errno=HOST_NOT_FOUND;return -1;}\n' > "$ROOT/res_query.c"
  emcc $CFLAGS -c "$ROOT/res_query.c" -o "$ROOT/res_query.o"
  emar rcs "$TARGET/lib/libresolv.a" "$ROOT/res_query.o"
  fetch glib https://download.gnome.org/sources/glib/2.84/glib-2.84.0.tar.xz
  (cd "$SRC/glib" && meson setup _build --prefix="$TARGET" --cross-file="$ROOT/cross.meson" --default-library=static \
    --buildtype=release --force-fallback-for=pcre2 -Dselinux=disabled -Dxattr=false -Dlibmount=disabled -Dnls=disabled \
    -Dtests=false -Dglib_debug=disabled -Dglib_assert=false -Dglib_checks=false \
    && sed -i '' -E '/#define HAVE_POSIX_SPAWN 1/d;/#define HAVE_PTHREAD_GETNAME_NP 1/d' _build/config.h \
    && meson install -C _build)
fi
if [ ! -f "$TARGET/lib/libcrypto.a" ]; then
  fetch openssl https://github.com/openssl/openssl/releases/download/openssl-3.5.4/openssl-3.5.4.tar.gz
  (cd "$SRC/openssl" && ./Configure linux-generic64 no-asm no-threads no-shared no-dso no-engine no-tests no-apps \
    no-sock no-ui-console no-afalgeng no-async no-docs --prefix="$TARGET" --libdir=lib CC=emcc AR=emar RANLIB=emranlib \
    CFLAGS="$CFLAGS" && make -j$J build_libs && make install_dev)
fi
echo deps ok
