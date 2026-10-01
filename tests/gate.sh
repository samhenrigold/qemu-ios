#!/bin/bash
# The one gate for this tree.
#
#   tests/gate.sh --quick   registered host checks, JOBS at a time (default 4)
#   tests/gate.sh --models  registered production device qtests, no guest inputs
#   tests/gate.sh --full    quick, models, then prepared-device suites
#   tests/gate.sh --fresh   full, then stock-IPSW preparation suites
#
# tests/gate-registry.json declares tiers and prerequisites. Unregistered tests
# fail every tier before checks run. Manual input-dependent checks report SKIP.
# The harnesses keep their own input defaults (~/Developer/qemu-ios-files, the usbmuxd forks,
# repro/default-iboot). The one shared input is the emulator: QEMU=... (default build/qemu-system-arm, the
# README's build dir), which --fresh's scripts use too.
# Checks listed in KNOWN below fail on today's tree for the reason given; they run and report XFAIL (or XPASS
# once they pass again), and neither fails the gate. Delete the line when the check is fixed.
# One line per check, PASS/FAIL/SKIP/XFAIL/XPASS with seconds; exit 1 if anything FAILs. Logs under OUT
# (default mktemp).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TIER="${1:---quick}"
case "$TIER" in --quick|--models|--full|--fresh) ;; *) sed -n '2,20p' "$0"; exit 2 ;; esac
export QEMU="${QEMU:-$ROOT/build/qemu-system-arm}"
export OUT="${OUT:-$(mktemp -d /tmp/gate.XXXXXX)}"
JOBS="${JOBS:-4}"
export TIMEOUT="$(command -v timeout || true)"   # coreutils; without it a hung test hangs the gate
# The H.264 unit checks link libavcodec through pkg-config and need the patched FFmpeg the emulator was
# configured against (scripts/configure-patched-ffmpeg leaves its pkg-config dir in the build dir).
FFMPEG_PKGCONFIG="$(dirname "$QEMU")/ffmpeg-pkgconfig"
[ -d "$FFMPEG_PKGCONFIG" ] && export PKG_CONFIG_PATH="$FFMPEG_PKGCONFIG${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
cd "$ROOT"
mkdir -p "$OUT" && : > "$OUT/results" || exit 2
python3 tests/gate_registry.py --quick-plan > "$OUT/quick-plan" || exit 1

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
    if [ -n "$why" ]; then
        if [ "$state" = FAIL ]; then state=XFAIL; else state=XPASS; fi
    fi
    printf '%-5s %5ds  %s%s\n' "$state" $((SECONDS - t0)) "$name" "${why:+  ($why)}" >> "$OUT/results"
}
skip() { printf 'SKIP      -  %s  (%s)\n' "$1" "$2" >> "$OUT/results"; }
export -f run1

# --- quick: host-only unit checks, in parallel
if [ "$TIER" != --models ]; then
{
    while IFS=$'\t' read -r tier name reason; do
        if [ "$tier" = manual ]; then skip "$name" "$reason"
        elif [ "$name" = "contrib/guest-package/mkpkg.py selfcheck" ]; then echo selfcheck
        else echo "$name"; fi
    done < "$OUT/quick-plan"
} | xargs -P "$JOBS" -I{} bash -c '
    case "$1" in
        selfcheck) run1 "contrib/guest-package/mkpkg.py selfcheck" python3 contrib/guest-package/mkpkg.py selfcheck ;;
        *) run1 "$1" python3 "$1" ;;
    esac' _ {}
fi

# --- full and fresh: the emulator suites, one at a time; each prints its own PASS/FAIL/SKIP lines
suite() {   # NAME CMD...
    local name=$1; shift
    echo "== $name"
    run1 "$name" "$@"
    grep -E '^(PASS|FAIL|SKIP|XFAIL)\b' "$OUT/${name//[\/ ]/_}.log" | sed 's/^/     /'
}
# Explicit built-emulator tests: libqtest drives the real MMIO/IRQ/timer model.
# Missing build prerequisites fail this tier rather than silently skipping it.
if [ "$TIER" = --models ] || [ "$TIER" = --full ] || [ "$TIER" = --fresh ]; then
    while IFS=$'\t' read -r tier registered reason; do
        model="${registered#qtest/}"
        case "$model" in
            ipad1-pmgr) binary="${QTEST_BINARY:-$(dirname "$QEMU")/tests/qtest/$model-test}" ;;
            ipad1-h2fmi) binary="${QTEST_H2FMI_BINARY:-$(dirname "$QEMU")/tests/qtest/$model-test}" ;;
            ipad1-cdma) binary="${QTEST_CDMA_BINARY:-$(dirname "$QEMU")/tests/qtest/$model-test}" ;;
            *) binary="$(dirname "$QEMU")/tests/qtest/$model-test" ;;
        esac
        if [ -x "$QEMU" ] && [ -x "$binary" ]; then
            suite "qtest/$model" env QTEST_QEMU_BINARY="$QEMU" "$binary"
        else
            printf 'FAIL      -  qtest/%s (build qemu-system-arm and tests/qtest/%s-test; QEMU and QTEST_BINARY/QTEST_H2FMI_BINARY/QTEST_CDMA_BINARY select them)\n' "$model" "$model" >> "$OUT/results"
        fi
    done < <(python3 tests/gate_registry.py --tier-plan models)
fi
if [ "$TIER" = --full ] || [ "$TIER" = --fresh ]; then
    while IFS=$'\t' read -r tier name reason; do
        if [ ! -x "$QEMU" ]; then
            skip "$name" "no emulator at $QEMU: build it or set QEMU="
            continue
        fi
        case "$name" in
            tests/ipod/run-regression.sh)
                # Judge this tree's guest shim against this tree's host.
                suite "$name" "$name" --qemu "$QEMU" --stage-gles-shim --out "$OUT/ipod-regress" ;;
            tests/ipad1/regress.py)
                suite "$name" python3 "$name" --qemu "$QEMU" --out "$OUT/ipad1-regress" ;;
            tests/ipad1/jank.py)
                suite "$name" python3 "$name" --gate --qemu "$QEMU" --out "$OUT/ipad1-jank" ;;
            *) printf 'FAIL      -  %s (no full-tier runner registered)\n' "$name" >> "$OUT/results" ;;
        esac
    done < <(python3 tests/gate_registry.py --tier-plan full)
fi
if [ "$TIER" = --fresh ]; then
    while IFS=$'\t' read -r tier name reason; do
        case "$name" in
            tests/ipod/fresh-device.sh) ipsw="${IPOD_IPSW:-}"; variable=IPOD_IPSW; entry=n72ap-7E18; output=fresh-ipod ;;
            tests/ipad1/fresh-device.sh) ipsw="${IPAD_IPSW:-}"; variable=IPAD_IPSW; entry=k48ap-7B500; output=fresh-ipad ;;
            *) printf 'FAIL      -  %s (no fresh-tier runner registered)\n' "$name" >> "$OUT/results"; continue ;;
        esac
        if [ -n "$ipsw" ]; then suite "$name" "$name" "$ipsw" "$OUT/$output"
        else skip "$name" "set $variable to the stock IPSW of ENTRY (default $entry)"; fi
    done < <(python3 tests/gate_registry.py --tier-plan fresh)
fi

echo "== $TIER"
sort -k3 "$OUT/results"
printf '%d passed, %d failed, %d skipped, %d known failing, %d passing again; logs in %s\n' \
    "$(grep -c '^PASS' "$OUT/results")" "$(grep -c '^FAIL' "$OUT/results")" "$(grep -c '^SKIP' "$OUT/results")" \
    "$(grep -c '^XFAIL' "$OUT/results")" "$(grep -c '^XPASS' "$OUT/results")" "$OUT"
! grep -q '^FAIL' "$OUT/results"
