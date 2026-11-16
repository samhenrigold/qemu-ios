#!/bin/bash
# The one gate for this tree.
#
#   tests/gate.sh --quick    host checks, JOBS at a time (default 4): every tests/slice/*.c (tests/slice/run.sh),
#                            host tests/*/*-test.sh, and the GL name table check
#   tests/gate.sh --models   the device qtests (tests/qtest/{ipod,ipad1,s5l8920}-*-test) and test-ios-baseband
#                            from the build next to QEMU, and shell checks marked # GATE: models
#   tests/gate.sh --all      both
#
# QEMU=... selects the emulator (default build/qemu-system-arm); the qtests come from the same build directory.
# Boots of prepared devices are LightTouchMac's (tests/sessions: `sessions single` and the Release plan's prepare
# matrix), not this gate's.
# One line per check, PASS/FAIL with seconds; exit 1 if anything FAILs. Logs under OUT (default a fresh
# qemu-gate.* under LTM_SCRATCH_ROOT or ~/Developer/ltm-scratch.noindex). Models are built incrementally first.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
TIER="${1:---quick}"
case "$TIER" in --quick|--models|--all) ;; *) sed -n '2,13p' "$0"; exit 2 ;; esac
export QEMU="${QEMU:-$ROOT/build/qemu-system-arm}"
JOBS="${JOBS:-4}"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo 'JOBS must be a positive integer' >&2; exit 2; }
if [ -z "${OUT:-}" ]; then
    scratch="${LTM_SCRATCH_ROOT:-$HOME/Developer/ltm-scratch.noindex}"
    mkdir -p "$scratch" || exit 2
    OUT="$(mktemp -d "$scratch/qemu-gate.XXXXXX")" || exit 2
fi
export OUT
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

# Shell checks declare when they need a built emulator; --quick never starts QEMU. Keep discovery in one
# place so --all runs each check once, after any needed rebuild.
host_checks=() model_checks=()
for check in tests/*/*-test.sh; do
    [ -f "$check" ] || continue
    if grep -q '^# GATE: models$' "$check"; then model_checks+=("$check"); else host_checks+=("$check"); fi
done
shell_checks() {
    printf '%s\n' "$@" | xargs -P "$JOBS" -I{} bash -c 'run1 "$1" bash "$1"' _ {} \
        || printf 'FAIL      -  shell check dispatch\n' >> "$OUT/results"
}
if [ "$TIER" != --models ]; then
    printf '%s\n' tests/slice/*.c |
        xargs -P "$JOBS" -I{} bash -c 'run1 "$1" tests/slice/run.sh "$1"' _ {} \
        || printf 'FAIL      -  slice check dispatch\n' >> "$OUT/results"
    shell_checks "${host_checks[@]}" contrib/gles-public/gligen.sh
fi
if [ "$TIER" != --quick ]; then
    BUILD="$(dirname "$QEMU")"
    # Discover from source, not the subset of binaries somebody happened to build. Bring the emulator and
    # every model check up to date together; otherwise an old binary can pass against changed device code.
    models=()
    for source in tests/qtest/{ipod,ipad1,s5l8920}-*-test.c; do
        [ -f "$source" ] || continue
        models+=("${source%.c}")
    done
    models+=(tests/unit/test-ios-baseband)
    run_build_log="$OUT/models-build.log"
    build_source="$(python3 -I - "$BUILD/meson-info/meson-info.json" 2> "$run_build_log" <<'SOURCE'
import json, os, sys
try:
    with open(sys.argv[1]) as f:
        print(os.path.realpath(json.load(f)["directories"]["source"]))
except (OSError, KeyError, ValueError) as e:
    print(e, file=sys.stderr)
    sys.exit(1)
SOURCE
)" || build_source=""
    if [ "$build_source" != "$ROOT" ]; then
        printf 'FAIL      -  model build is not configured from this checkout (see models-build.log)\n' >> "$OUT/results"
    elif [ "${#models[@]}" -le 1 ]; then
        printf 'FAIL      -  no device qtest sources found\n' >> "$OUT/results"
    elif ! scripts/ccninja -C "$BUILD" qemu-system-arm "${models[@]}" > "$run_build_log" 2>&1; then
        printf 'FAIL      -  model build (see models-build.log; configure the build next to QEMU=)\n' >> "$OUT/results"
    else
        if [ "${#model_checks[@]}" -gt 0 ]; then shell_checks "${model_checks[@]}"; fi
        for model in "${models[@]}"; do
            binary="$BUILD/$model"
            if [ -x "$QEMU" ] && [ -x "$binary" ]; then
                run1 "$model" env QTEST_QEMU_BINARY="$QEMU" "$binary"
            else
                printf 'FAIL      -  %s (missing after model build)\n' "$model" >> "$OUT/results"
            fi
        done
    fi
fi

echo "== $TIER"
sort -k3 "$OUT/results"
printf '%d passed, %d failed; logs in %s\n' "$(grep -c '^PASS' "$OUT/results")" "$(grep -c '^FAIL' "$OUT/results")" "$OUT"
grep -q '^PASS' "$OUT/results" && ! grep -q '^FAIL' "$OUT/results"
