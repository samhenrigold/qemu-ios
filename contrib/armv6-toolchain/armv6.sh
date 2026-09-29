#!/bin/bash
# Build armv6 Mach-O binaries for iOS 3.1.3 with a stock modern macOS toolchain.
#
# Source this and use cc6/link6; or run it directly to build pl0trap, the
# QEMU_CALL-from-user-mode probe.
#
# See README.md for why each step is here. The short version: clang still
# compiles armv6 fine, ld refuses to link it, and everything after the link is
# undoing things that did not exist in 2010.
#
#   cc6   <src> <obj> [compiler flags...]
#   link6 -bundle|-dylib|-execute <out> <objs/flags...>
#
# GUEST_ARCH=armv7 retargets the same pipeline at the iPad 1 (Cortex-A8, iOS
# 3.2.2): real armv7 code, no subtype round-trip, cpusubtype 9. Point ARMV6_SDK
# at the 3.2 SDK then. mkold.py's LC_MAIN->LC_UNIXTHREAD rewrite is what lets
# these be executables at all on 3.2 dyld (docs/ipad1/guest-services.md).
#
# LEGACY_LINK=1 makes link6 emit what 2.x dyld takes as well: a non-PIE link
# whose LC_DYLD_INFO_ONLY mkold.py --legacy proves redundant and drops; executables
# also get crt1old.c, the start routine 1.x libSystem needs (it does not initialize itself).
set -eu

GUEST_ARCH="${GUEST_ARCH:-armv6}"
ARMV6_SDK="${ARMV6_SDK:-/Users/shg/Downloads/OldSDK/iPhoneOS3.1.3.sdk}"
ARMV6_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

cc6() {
    # -marm because clang defaults to Thumb for this target and would emit
    # Thumb-2, which the ARM1176 cannot execute. armv6 also keeps movw/movt out
    # of the output; constants go through the literal pool instead.
    #
    # LEGACY_LINK=1 (code for the 2.x dyld) also keeps r9 out of the register allocator: 2.x's
    # ABI reserves it as the thread pointer (2.x libSystem's pthread_getspecific is
    # `add r0, r9, r0, lsl #2; ldr r0, [r0, #0x48]`), where 3.0+ made it an ordinary register.
    # Code that uses it breaks TSD, pthread_once and errno for itself and every caller it
    # returns to. It also force-includes legacy.h: stat/readdir as 1.x's libSystem has them.
    rm -f "$2"
    fixed=()
    [ "${LEGACY_LINK:-0}" = 1 ] && fixed=(-ffixed-r9 -include "$ARMV6_HERE/legacy.h")
    if ! xcrun clang -target $GUEST_ARCH-apple-ios5.0 -marm -O1 -fno-stack-protector ${fixed[@]+"${fixed[@]}"} \
        -fno-builtin -nostdinc -isystem "$ARMV6_SDK/usr/include" "${@:3}" \
        -c "$1" -o "$2" >"$2.cclog" 2>&1; then
        cat "$2.cclog" >&2
        rm -f "$2" "$2.cclog"
        return 1
    fi
    grep -v 'incompatible-sysroot' "$2.cclog" >&2 || true
    rm -f "$2.cclog"
    # ld rejects -arch armv6 outright, so present the object as armv7 and put
    # the subtype back after linking.
    [ "$GUEST_ARCH" = armv7 ] || python3 "$ARMV6_HERE/subtype.py" "$2" 9 >/dev/null
}

link6() {
    kind="$1"; out="$2"; shift 2
    # The 3.1.3 SDK's own libSystem stub is fat and has a real armv7 slice; the
    # modern iOS SDK dropped armv7 entirely and has nothing to link against.
    #
    # The filtering below drops one expected warning, but ld's exit status is
    # checked rather than swallowed. An earlier version piped straight into
    # `grep ... || true`, which hid a genuine link error ("-framework OpenGLES
    # ... built for 'unknown'" is fatal, unlike the same mismatch on -lSystem)
    # and surfaced it as a confusing "no such file" from mkold.py two steps
    # later. A failed link must fail here.
    rm -f "$out"
    legacy=()
    [ "${LEGACY_LINK:-0}" = 1 ] && legacy=(-no_pie)
    # a legacy executable enters at crt1old.c's _start, which does what 1.x's crt1 did
    if [ "${LEGACY_LINK:-0}" = 1 ] && [ "$kind" = -execute ]; then
        cc6 "$ARMV6_HERE/crt1old.c" "$out.crt1old.o" || return 1
        legacy+=(-e _start "$out.crt1old.o")
    fi
    if ! xcrun ld -arch armv7 "$kind" ${legacy[@]+"${legacy[@]}"} -platform_version ios 9.0 9.0 \
            -no_function_starts -no_data_in_code_info -no_uuid \
            -syslibroot "$ARMV6_SDK" -L"$ARMV6_SDK/usr/lib" -lSystem \
            "$@" -o "$out" 2>"$out.ldlog"; then
        echo "link6: ld failed for $out" >&2
        cat "$out.ldlog" >&2
        rm -f "$out.ldlog" "$out.crt1old.o"
        return 1
    fi
    grep -v "built for 'unknown'" "$out.ldlog" >&2 || true
    rm -f "$out.ldlog" "$out.crt1old.o"
    python3 "$ARMV6_HERE/mkold.py" "$out" --subtype "$([ "$GUEST_ARCH" = armv7 ] && echo 9 || echo 6)" \
        $([ "${LEGACY_LINK:-0}" = 1 ] && echo --legacy)
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    OUT="${1:-$ARMV6_HERE/pl0trap}"
    cc6 "$ARMV6_HERE/pl0trap.c" "$ARMV6_HERE/pl0trap.o"
    link6 -execute "$OUT" "$ARMV6_HERE/pl0trap.o"
    rm -f "$ARMV6_HERE/pl0trap.o"
    file "$OUT"
fi
