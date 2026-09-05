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
set -eu

ARMV6_SDK="${ARMV6_SDK:-/Users/shg/Downloads/OldSDK/iPhoneOS3.1.3.sdk}"
ARMV6_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

cc6() {
    # -marm because clang defaults to Thumb for this target and would emit
    # Thumb-2, which the ARM1176 cannot execute. armv6 also keeps movw/movt out
    # of the output; constants go through the literal pool instead.
    rm -f "$2"
    if ! xcrun clang -target armv6-apple-ios5.0 -marm -O1 -fno-stack-protector \
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
    python3 "$ARMV6_HERE/subtype.py" "$2" 9 >/dev/null
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
    if ! xcrun ld -arch armv7 "$kind" -platform_version ios 9.0 9.0 \
            -no_function_starts -no_data_in_code_info -no_uuid \
            -syslibroot "$ARMV6_SDK" -L"$ARMV6_SDK/usr/lib" -lSystem \
            "$@" -o "$out" 2>"$out.ldlog"; then
        echo "link6: ld failed for $out" >&2
        cat "$out.ldlog" >&2
        rm -f "$out.ldlog"
        return 1
    fi
    grep -v "built for 'unknown'" "$out.ldlog" >&2 || true
    rm -f "$out.ldlog"
    python3 "$ARMV6_HERE/mkold.py" "$out"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    OUT="${1:-$ARMV6_HERE/pl0trap}"
    cc6 "$ARMV6_HERE/pl0trap.c" "$ARMV6_HERE/pl0trap.o"
    link6 -execute "$OUT" "$ARMV6_HERE/pl0trap.o"
    rm -f "$ARMV6_HERE/pl0trap.o"
    file "$OUT"
fi
