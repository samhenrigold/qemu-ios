#!/bin/bash
# The one gate for this tree.
#
#   tests/gate.sh --quick   host only, no emulator: every tests/ipod/test_*.py, tests/ipad1/test_*.py and
#                           tests/guest-package/test_*.py that does not launch qemu-system-arm, plus
#                           contrib/guest-package/mkpkg.py selfcheck; JOBS at a time (default 4)
#   tests/gate.sh --full    quick, then tests/ipod/run-regression.sh and tests/ipad1/regress.py (default tiers),
#                           one suite at a time
#   tests/gate.sh --fresh   full, then tests/ipod/fresh-device.sh and tests/ipad1/fresh-device.sh (defaults)
#
# Unit tests that launch the emulator (the *_guest.py acceptance runs, the *_snapshot.py and paused-machine
# QOM checks: any test naming qemu-system-arm) are SKIP in every tier; run them by hand with a built emulator
# and a NAND. So are tests that take their inputs (a NAND, a movie, a capture) on the command line.
# The harnesses keep their own input defaults (~/Developer/qemu-ios-files, the usbmuxd forks,
# repro/default-iboot). The one shared input is the emulator: QEMU=... (default build/qemu-system-arm, the
# README's build dir); --fresh's iPad script has no override and always uses build/qemu-system-arm.
# Checks listed in KNOWN below fail on today's tree for the reason given; they run and report XFAIL (or XPASS
# once they pass again), and neither fails the gate. Delete the line when the check is fixed.
# One line per check, PASS/FAIL/SKIP/XFAIL/XPASS with seconds; exit 1 if anything FAILs. Logs under OUT
# (default mktemp).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TIER="${1:---quick}"
case "$TIER" in --quick|--full|--fresh) ;; *) sed -n '2,20p' "$0"; exit 2 ;; esac
export QEMU="${QEMU:-$ROOT/build/qemu-system-arm}"
export OUT="${OUT:-$(mktemp -d /tmp/gate.XXXXXX)}"
JOBS="${JOBS:-4}"
export TIMEOUT="$(command -v timeout || true)"   # coreutils; without it a hung test hangs the gate
# The H.264 unit checks link libavcodec through pkg-config and need the patched FFmpeg the emulator was
# configured against (scripts/configure-patched-ffmpeg leaves its pkg-config dir in the build dir).
[ -d "$ROOT/build/ffmpeg-pkgconfig" ] && export PKG_CONFIG_PATH="$ROOT/build/ffmpeg-pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
cd "$ROOT"
mkdir -p "$OUT" && : > "$OUT/results" || exit 2

# Failing on today's tree, each for a known reason (none since 2026-09-29: the 12 stale C-slice/mock checks were repaired).
export KNOWN='
'
known_reason() { printf '%s\n' "$KNOWN" | awk -v n="$1" '$1 == n { $1 = ""; sub(/^ +/, ""); print }'; }
export -f known_reason

# NAME CMD...: one check, its own log, one line in $OUT/results (whole line appended at once).
run1() {
    local name=$1 t0=$SECONDS log state why; shift
    log="$OUT/${name//[\/ ]/_}.log"
    if ${TIMEOUT:+$TIMEOUT 900} "$@" > "$log" 2>&1; then state=PASS; else state=FAIL; fi
    why=$(known_reason "$name")
    if [ "$state" = FAIL ] && head -1 "$log" | grep -q '^usage:'; then
        state=SKIP; why="takes inputs on the command line: $(head -1 "$log")"
    elif [ -n "$why" ]; then
        if [ "$state" = FAIL ]; then state=XFAIL; else state=XPASS; fi
    fi
    printf '%-5s %5ds  %s%s\n' "$state" $((SECONDS - t0)) "$name" "${why:+  ($why)}" >> "$OUT/results"
}
skip() { printf 'SKIP      -  %s  (%s)\n' "$1" "$2" >> "$OUT/results"; }
export -f run1

# --- quick: host-only unit checks, in parallel
{
    for t in tests/ipod/test_*.py tests/ipad1/test_*.py tests/guest-package/test_*.py; do
        if grep -q qemu-system-arm "$t"; then skip "$t" "launches qemu-system-arm: run by hand with a built emulator and a NAND"
        elif grep -q 'sys.exit(__doc__)' "$t"; then skip "$t" "takes inputs on the command line: see its docstring"
        else echo "$t"; fi
    done
    echo selfcheck
} | xargs -P "$JOBS" -I{} bash -c '
    case "$1" in
        selfcheck) run1 "contrib/guest-package/mkpkg.py selfcheck" python3 contrib/guest-package/mkpkg.py selfcheck ;;
        *) run1 "$1" python3 "$1" ;;
    esac' _ {}

# --- full and fresh: the emulator suites, one at a time; each prints its own PASS/FAIL/SKIP lines
suite() {   # NAME CMD...
    local name=$1; shift
    echo "== $name"
    run1 "$name" "$@"
    grep -E '^(PASS|FAIL|SKIP|XFAIL)\b' "$OUT/${name//[\/ ]/_}.log" | sed 's/^/     /'
}
if [ "$TIER" != --quick ]; then
    if [ -x "$QEMU" ]; then
        # --stage-gles-shim: the gles check runs this tree's guest shim against this tree's host, the pair
        # the gate is judging. The shipping image's baked shim is older (its gles verdict is the image's,
        # not the tree's) and is replaced at the main-merge image swap (docs/ipod/nand-current-new-verification.md).
        suite "tests/ipod/run-regression.sh" tests/ipod/run-regression.sh --qemu "$QEMU" --stage-gles-shim --out "$OUT/ipod-regress"
        suite "tests/ipad1/regress.py" python3 tests/ipad1/regress.py --qemu "$QEMU" --out "$OUT/ipad1-regress"
    else
        skip "tests/ipod/run-regression.sh" "no emulator at $QEMU: build it or set QEMU="
        skip "tests/ipad1/regress.py" "no emulator at $QEMU: build it or set QEMU="
    fi
fi
if [ "$TIER" = --fresh ]; then
    suite "tests/ipod/fresh-device.sh" tests/ipod/fresh-device.sh
    suite "tests/ipad1/fresh-device.sh" tests/ipad1/fresh-device.sh
fi

echo "== $TIER"
sort -k3 "$OUT/results"
printf '%d passed, %d failed, %d skipped, %d known failing, %d passing again; logs in %s\n' \
    "$(grep -c '^PASS' "$OUT/results")" "$(grep -c '^FAIL' "$OUT/results")" "$(grep -c '^SKIP' "$OUT/results")" \
    "$(grep -c '^XFAIL' "$OUT/results")" "$(grep -c '^XPASS' "$OUT/results")" "$OUT"
! grep -q '^FAIL' "$OUT/results"
