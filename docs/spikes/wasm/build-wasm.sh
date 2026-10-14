#!/bin/bash
# Configures and builds qemu-system-arm (arm-softmmu, TCI) for wasm64 with Homebrew's emcc
# against deps.sh's libraries. usage: build-wasm.sh QEMU_SRC [BUILD_DIR] [extra configure args]
set -euo pipefail
SRC=$1; B=${2:-$SRC/build-wasm}; shift; [ $# -gt 0 ] && shift
export EMSDK_PYTHON=/opt/homebrew/opt/python@3.12/libexec/bin/python3  # emar is run by path, not via the brew wrapper
DEPS=${DEPS:-$HOME/Developer/wasm-spike-deps/target}
export PKG_CONFIG="pkg-config --static" PKG_CONFIG_LIBDIR=$DEPS/lib/pkgconfig PKG_CONFIG_PATH=$DEPS/lib/pkgconfig EM_PKG_CONFIG_PATH=$DEPS/lib/pkgconfig CPATH=$DEPS/include
export CFLAGS="-O3 -pthread -DWASM_BIGINT" LDFLAGS="-sWASM_BIGINT -sASYNCIFY=1 -L$DEPS/lib"
mkdir -p "$B"; cd "$B"
[ -f build.ninja ] || emconfigure "$SRC/configure" --python=/opt/homebrew/opt/python@3.12/libexec/bin/python3 \
  --cross-prefix=em --cc=emcc --cxx=em++ --static --cpu=wasm64 --target-list=arm-softmmu --enable-tcg-interpreter --disable-tools --disable-docs \
  --disable-slirp --disable-sdl --disable-cocoa --disable-coreaudio --disable-capstone --disable-fuse \
  --extra-cflags="-I$DEPS/include" "$@"
# The runner (run-node.mjs) mounts the Mac's /Users with NODEFS and passes environment variables through ENV.
grep -q 'TTY,FS,ENV' build.ninja || sed -i '' 's|-sEXPORTED_RUNTIME_METHODS=addFunction,removeFunction,TTY,FS|&,ENV -lnodefs.js|' build.ninja
nice ninja -j8 qemu-system-arm.js
ln -sf qemu-system-arm.js qemu-system-arm.mjs
ls -la qemu-system-arm*
