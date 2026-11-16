#!/bin/bash
# Exercise the real gate in a tiny fake checkout: incomplete and stale model builds must never pass.
set -euo pipefail
SRC="$(cd "$(dirname "$0")/../.." && pwd)"
scratch="${LTM_SCRATCH_ROOT:-$HOME/Developer/ltm-scratch.noindex}"
mkdir -p "$scratch"
T="$(mktemp -d "$scratch/gate-test.XXXXXX")"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/build/meson-info" "$T/tests/qtest" "$T/scripts" "$T/build/tests/qtest" "$T/build/tests/unit"
cp "$SRC/tests/gate.sh" "$T/tests/gate.sh"
printf '{"directories":{"source":"%s"}}\n' "$T" > "$T/build/meson-info/meson-info.json"
for model in ipod-first ipad1-second s5l8920-third; do touch "$T/tests/qtest/$model-test.c"; done
# A fake incremental builder records the targets and can refuse, or leave one target missing.
cat > "$T/scripts/ccninja" <<'BUILDER'
#!/bin/bash
printf '%s\n' "$@" > "$PWD/build-args"
[ "${BUILD_FAIL:-0}" = 0 ] || exit 1
shift 2
for target in "$@"; do
    [[ "$target" != "${MISSING:-}" ]] || continue
    printf '#!/bin/sh\nexit %s\n' "${MODEL_FAIL:-0}" > "build/$target"
    chmod +x "build/$target"
done
BUILDER
chmod +x "$T/scripts/ccninja"
gate() { OUT="$T/out" QEMU="$T/build/qemu-system-arm" bash "$T/tests/gate.sh" "$@" > "$T/result" 2>&1; }
fail() { echo "FAIL: $*"; cat "$T/result"; exit 1; }
gate --models || fail 'complete build refused'
grep -qx tests/qtest/ipad1-second-test "$T/build-args" || fail 'unbuilt test not requested'
grep -q '4 passed, 0 failed' "$T/result" || fail 'not every model ran'
rm "$T/build-args"
printf '{"directories":{"source":"/another-checkout"}}\n' > "$T/build/meson-info/meson-info.json"
gate --models && fail 'another checkout tested instead of these sources'
[ ! -f "$T/build-args" ] || fail 'another checkout was rebuilt in place'
printf '{"directories":{"source":"%s"}}\n' "$T" > "$T/build/meson-info/meson-info.json"
BUILD_FAIL=1 gate --models && fail 'failed rebuild passed using stale executables'
rm "$T/build/tests/qtest/ipad1-second-test"
MISSING=tests/qtest/ipad1-second-test gate --models && fail 'missing model passed'
MODEL_FAIL=1 gate --models && fail 'model failure passed'
JOBS=0 gate --quick && fail 'unbounded dispatch accepted'
# A failed xargs dispatch must propagate even if another result says PASS.
mkdir -p "$T/bin" "$T/tests/slice" "$T/tests/host" "$T/contrib/gles-public"
touch "$T/tests/slice/host.c" "$T/tests/host/one-test.sh" "$T/contrib/gles-public/gligen.sh"
printf '#!/bin/sh\nprintf "PASS 0s fake\\n" >> "$OUT/results"\nexit 1\n' > "$T/bin/xargs"
chmod +x "$T/bin/xargs"
PATH="$T/bin:$PATH" gate --quick && fail 'failed dispatch passed'
# The host tier runs its host shell test, but never a marked emulator test; --all runs each once.
rm "$T/bin/xargs"
cat > "$T/tests/slice/run.sh" <<'HOST'
#!/bin/sh
exit 0
HOST
chmod +x "$T/tests/slice/run.sh"
printf '#!/bin/sh\necho host >> "%s/ran"\n' "$T" > "$T/tests/host/one-test.sh"
printf '#!/bin/sh\n# GATE: models\necho model >> "%s/ran"\n' "$T" > "$T/tests/host/emulator-test.sh"
gate --quick || fail 'host-only tier failed'
[ "$(cat "$T/ran")" = host ] || fail 'quick ran an emulator check'
: > "$T/ran"
gate --all || fail 'all tier failed'
[ "$(sort "$T/ran" | tr '\n' ' ')" = 'host model ' ] || fail 'all duplicated or omitted a shell check'
echo 'PASS: gate builds every model source, rejects stale/missing/failed models, invalid jobs and failed dispatch'
