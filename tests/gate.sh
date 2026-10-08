#!/bin/bash
# The one gate for this tree.
#
#   tests/gate.sh --quick    host checks, JOBS at a time (default 4): every tests/slice/*.c (tests/slice/run.sh),
#                            every tests/*/*-test.sh, and the GL name table check
#   tests/gate.sh --models   the device qtests (tests/qtest/{ipod,ipad1,s5l8920}-*-test) and test-ios-baseband
#                            from the build next to QEMU
#   tests/gate.sh --all      both
#
# QEMU=... selects the emulator (default build/qemu-system-arm); the qtests come from the same build directory.
# Boots of prepared devices are LightTouchMac's (tests/sessions: `sessions single` and the Release plan's prepare
# matrix), not this gate's.
# One line per check, PASS/FAIL with seconds; exit 1 if anything FAILs. Logs under OUT (default mktemp).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TIER="${1:---quick}"
case "$TIER" in --quick|--models|--all) ;; *) sed -n '2,13p' "$0"; exit 2 ;; esac
export QEMU="${QEMU:-$ROOT/build/qemu-system-arm}"
export OUT="${OUT:-$(mktemp -d /tmp/gate.XXXXXX)}"
JOBS="${JOBS:-4}"
export TIMEOUT="$(command -v timeout || true)"   # coreutils; without it a hung check hangs the gate
cd "$ROOT"
mkdir -p "$OUT" && : > "$OUT/results" || exit 2

# NAME CMD...: one check, its own log, one line in $OUT/results (whole line appended at once).
run1() {
    local name=$1 t0=$SECONDS log state; shift
    log="$OUT/${name//[\/ ]/_}.log"
    if ${TIMEOUT:+$TIMEOUT 900} "$@" > "$log" 2>&1; then state=PASS; else state=FAIL; fi
    printf '%-5s %5ds  %s\n' "$state" $((SECONDS - t0)) "$name" >> "$OUT/results"
}
export -f run1

if [ "$TIER" != --models ]; then
    { ls tests/slice/*.c tests/*/*-test.sh; echo contrib/gles-public/gligen.sh; } |
        xargs -P "$JOBS" -I{} bash -c 'case "$1" in *.c) run1 "$1" tests/slice/run.sh "$1" ;; *) run1 "$1" bash "$1" ;; esac' _ {}
fi
if [ "$TIER" != --quick ]; then
    BUILD="$(dirname "$QEMU")"
    for binary in "$BUILD"/tests/qtest/{ipod,ipad1,s5l8920}-*-test "$BUILD/tests/unit/test-ios-baseband"; do
        if [ -x "$QEMU" ] && [ -x "$binary" ]; then
            run1 "${binary#"$BUILD"/}" env QTEST_QEMU_BINARY="$QEMU" "$binary"
        else
            printf 'FAIL      -  %s (build qemu-system-arm and the tests next to it, or set QEMU=)\n' "${binary#"$BUILD"/}" >> "$OUT/results"
        fi
    done
fi

echo "== $TIER"
sort -k3 "$OUT/results"
printf '%d passed, %d failed; logs in %s\n' "$(grep -c '^PASS' "$OUT/results")" "$(grep -c '^FAIL' "$OUT/results")" "$OUT"
! grep -q '^FAIL' "$OUT/results"
