# Spike: n72ap-7E18 (iPod touch 2G, iPhone OS 3.1.3) in a browser through WebAssembly

Date: 2026-10-13. Host: this Mac (M4 Max, 16 cores, macOS 27). qemu-ios 6d16ef4f3f (QEMU 11.0.3 plus upstream
merges through 2026-08-04). Spike branch `wasm-spike` in the worktree ~/Developer/qemu-ios-wasm-spike, commit
e67073595d (not pushed). Patch: [wasm-spike.diff](wasm-spike.diff). Scripts: [wasm-spike/](wasm-spike/).

## Answer

**It runs, but too slowly to use: no-go on the TCI path.** The wasm build boots 3.1.3 to the lock screen under Node
and in Chrome. It also resumes a state saved by the native build and shows Home in a Chrome canvas
(wasm-spike/browser-home.png) about 1 s after the page loads. But it runs about 21-23 guest MIPS. Native TCG runs
about 570, so wasm is about 25x slower, and that is about 4% of the real device's speed. A cold boot takes 5 minutes
instead of 13 s. Native scrolling at about 22 fps would drop below 1 fps.

## Numbers

Guest MIPS was measured as instructions per wall second, with `-icount shift=0,sleep=off` on all four builds. The
stretch measured is the boot's 1e9 to 5e9 instructions, which is CPU-bound because sleep=off skips idle time. "Lit"
means the screen's mean luma first passed 0.1, which happens at the lock screen. All runs started from the same
fresh overlay of the gate base ~/Developer/qemu-ios-files/m68/ltm-1x/n72-7E18. They were headless and silent
(`-audio none`). Load average was 3-25 during the runs.

| build | guest MIPS | boot to lit (icount run) | boot to lit (plain run) |
|---|---|---|---|
| native TCG (same device set as wasm) | 571 | 13 s | 13-14 s |
| native TCI | 87 | 78 s | - |
| wasm TCI, Node 26 | 18 / 21 (two runs) | 356 s / 327 s | - |
| wasm TCI, headless Chrome 155 | 23 | 278 s | - |

Interpreting TCI in wasm costs about 4x more than interpreting it natively. TCI itself costs 6.5x against TCG.

Resume used a state saved natively at Home and loaded with `-incoming file:`:

| | native | wasm, Node | wasm, Chrome |
|---|---|---|---|
| save (QMP `stop` + `migrate file:`) | 0.4-0.7 s, 104 MB (44 MB gzip -1) | - | - |
| process start to resumed Home on screen | under 1 s | under 1.5 s | page load to module ready 0.87 s, Home drawn at the first 1 s probe tick |

Sizes:
- qemu-system-arm.wasm: 58.1 MB (16.0 MB gzip -9, 11.6 MB brotli -9). The JS glue is 0.44 MB.
- The native binary for comparison is 33 MB.
- The NAND base is 869 MB in 111,294 page files.
- A cold boot to the lock screen reads 16,251 existing pages, 70 MB with the boot files. A resumed Home session
  read 113 pages in 40 s.
- The resumed browser session's manifest was 459 files, 108 MB, most of it the state.

## What it took (steps; each disable is a configure flag or a few lines)

1. Install the toolchain: `brew install emscripten` (6.0.12). Docker has no daemon here, so the upstream
   dockerfile (tests/docker/dockerfiles/emsdk-wasm64-cross.docker) was followed by hand.
2. `wasm-spike/deps.sh` builds zlib, libffi, pixman, glib 2.84 (with its pcre2 fallback) and OpenSSL 3.5's
   libcrypto (no-asm, linux-generic64) for wasm64 into ~/Developer/wasm-spike-deps/target. One trap: zlib's
   configure picks Apple's libtool on macOS, so it needs `AR=emar ARFLAGS=rcs`.
3. `wasm-spike/build-wasm.sh SRC`: `--cpu=wasm64 --static --enable-tcg-interpreter --target-list=arm-softmmu`
   with `--disable-tools/docs/slirp/sdl/cocoa/coreaudio/capstone/fuse`. These flags are also needed:
   - `--cross-prefix=em --cc=emcc --cxx=em++`
   - `PKG_CONFIG_LIBDIR` set to the deps' pkgconfig, otherwise Homebrew's curl and zstd leak in
   - `EMSDK_PYTHON`, because ninja runs emar by path with Xcode's Python 3.9
   - after configure, a build.ninja edit adding `ENV` to EXPORTED_RUNTIME_METHODS and `-lnodefs.js`, so the
     Node runner can pass environment variables and mount /Users

   Upstream's emscripten section adds `-sPROXY_TO_PTHREAD -sASYNCIFY -sTOTAL_MEMORY=2GB`, so the page must be
   cross-origin isolated (COOP/COEP).
4. Source patches (the diff, +26/-17 lines plus the probe):
   - **EMSCRIPTEN macro**: Emscripten 6 defines only `__EMSCRIPTEN__`, but QEMU 11.0 tests `EMSCRIPTEN`. Fixed in
     configure (host detection) and in a three-line alias in osdep.h.
   - **GLES bridge**: `--disable-opengl` now drops gles-host.c on macOS too, as on a host without GL. This was
     needed for state compatibility, because gles-host registers a vmstate section that the wasm build (which uses
     the stub) doesn't have.
   - **libcrypto link order**: libcrypto comes from pkg-config in hw/arm/meson.build. wasm-ld needs it after the
     objects; native builds keep `-lcrypto`.
   - **OpenSSL**: no change to the AES and PKE models. At runtime the boot needs both: iBoot really verifies every
     img3 (RSA through PKE, with forge-sigcheck off) and decrypts with AES. So libcrypto was built for wasm rather
     than stubbed. It is a 6.6 MB archive, and the static link keeps only what the two models call. Swapping AES to QEMU's crypto/aes.h
     would also need a CBC loop, and PKE would need a bignum library: not worth it.
   - **FFmpeg (IT_HAVE_AVCODEC)**: without it, ipod_touch_amc.c didn't compile (`amc_local` sat inside the avcodec
     block), its realize failed ("AMC decoding requires a build with libavcodec support"), and its post_load
     refused a decode-mode state. Now it warns, and decoded audio is silent. FFmpeg also adds h264 vmstate fields,
     so the native comparison build is configured without it (`build-native.sh` filters libav*.pc out of
     pkg-config). VideoToolbox code is already `__APPLE__`-only.
   - **slirp**: disabled (`--disable-slirp`). The machine runs without Wi-Fi's netdev.
   - **spike-probe.c**: env-gated and inert unless SPIKE_PROBE is set. It prints elapsed time, icount and screen
     luma each second, writes the screen as a PPM, quits on time, and `SPIKE_CONT` runs a state that loaded
     paused. QEMU now keeps the source's paused runstate across migration, so a Stop-style save loads paused and
     the helper would have to `cont` after it.
5. Node: `run.sh NAME SECS -- node run-node.mjs build-wasm/qemu-system-arm.mjs [-- qemu args]`. The argv is the
   same as BootRecipe.iPod's: `-M iPod-Touch,boot-args=…,direct-iboot=…,direct-llb=,bootrom=…,nand=…,nor=…,
   nor-rw=…,nandrw=…,gid-blobs=… -m 128M -display none -no-shutdown`, plus `IT_TVOUT_READY=1`. There is no
   guest-package, so the GLES bridge is never offered.
6. State: `save-home.py NAME build-native/qemu-system-arm` boots natively, waits for the lock screen, slides to
   unlock over QMP `input-send-event`, then runs `stop` and `migrate file:`. The resulting state loads in the wasm
   build under Node and in Chrome. The device set must match: no FFmpeg and no GLES bridge on both sides.
7. Browser: `serve.mjs BUILD RUN OPENLOG` plus `page.html` plus `chrome.sh SECS`. A localhost server with COOP and
   COEP serves a manifest: the argv, plus the files to preload into MEMFS at their Mac paths. The NAND pages listed
   are those a Node run opened (`SPIKE_OPENLOG`). The page draws the probe's PPM to a canvas each second and posts
   the canvas back. It was tested in headless Chrome only, which supports Memory64. Safari would need the
   `--wasm64-32bit-address-limit` build, which wasn't tried. Only pages from the openlog are preloaded, so a page
   the guest reads later in the browser would read as erased. A real product would need lazy page fetching or a
   packed NAND image.

## The wasm TCG backend (qemu-wasm)

- Not upstream, and not in 11.1 either. This tree carries upstream through 2026-08-04 (the 11.1 cycle) and has no
  tcg/wasm. Upstream took only the TCI port: Emscripten in 2025, wasm64 guests in the 11.0 cycle (January 2026).
- Kohei Tokunaga's April 2025 series had a backend that emits Wasm modules at runtime. Reviewers asked him to split
  it and land TCI first, and the backend has not been merged since.
- Rebasing to 11.1 therefore buys nothing for speed. The backend would have to be ported out of tree from the
  qemu-wasm fork onto 11.0.x, a fork based on an older QEMU. Its speedup on a 32-bit ARM guest wasn't measured
  here.
- For scale: even a 10x gain over TCI would give about 200 MIPS. That is about 35% of native TCG, roughly the
  native TCI-to-TCG gap.
- 2007.mobile reaches usable speed with its own interpreter plus precompiled guest code, which is not QEMU's
  architecture.

## Graphics (question 4)

3.1.3 draws without the GLES host bridge. With no guest package the bridge is never offered, and this native build
has no gles-host at all (`--disable-opengl`). Even so, SpringBoard drew the lock screen (luma 0.24) and, after the
unlock slide, Home (luma 0.28). Home is visible in browser-home.png from the Chrome canvas: Home was drawn natively
before the save and kept running in wasm. The lock screen was drawn entirely by the wasm build, cold, under both
Node and Chrome. The spike doesn't depend on the MBX model another agent is adapting: what 3.1.3 needed is already
in tree.

## Recommendation

**No-go for building on TCI.** About 22 guest MIPS (4% of the device, 25x slower than native) is a demo, not an
emulator. If a browser version is still wanted, the path is:

1. Port the qemu-wasm TCG backend onto 11.0.x out of tree, as a measured spike first: does an ARM guest reach
   200+ MIPS? Don't rebase to 11.1 first, because it doesn't have the backend.
2. Ship resume-from-state instead of a cold boot: about 44 MB of gzip'd state plus lazy NAND pages, instead of
   870 MB.
3. Use a wasm32-addressed build (`--wasm64-32bit-address-limit`) for Safari.

Without step 1 none of the rest matters.

Artifacts (scratch, not in any repo):
- ~/Developer/wasm-spike-deps: the wasm deps and logs (0.5 GB)
- ~/Developer/wasm-spike-run: run directories, probe logs, states and screenshots (0.4 GB)
- ~/Developer/qemu-ios-wasm-spike: the build-wasm, build-native and build-native-tci build directories
