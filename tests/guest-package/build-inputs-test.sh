#!/bin/bash
# contrib/guest-package/build-inputs.sh: new sources and same-size SDK changes invalidate reuse. Exit 0 = pass.
set -euo pipefail
BI="$(cd "$(dirname "$0")/../.." && pwd)/contrib/guest-package/build-inputs.sh"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
fail() { echo "FAIL line ${BASH_LINENO[0]}: $*" >&2; exit 1; }
put() { mkdir -p "$(dirname "$T/$1")"; printf '%s' "${2:-input}" > "$T/$1"; }
src() { "$BI" sources "$T"; }
tree() { "$BI" tree "$T/SDK"; }
for f in include/hw/arm/guest-services/gles-names.h contrib/guest-package/VERSION \
         contrib/export-guest-artifacts.sh contrib/macos-app/entitlements.plist; do
    put "$f"
done

# a new nested source is a dependency, generated Mach-Os, objects, generated stubs and caches are not
before="$(src)"
put contrib/it-agent/data/new-input.dat
printf '\xce\xfa\xed\xfecompiled' > "$T/contrib/it-agent/it_agent"
put contrib/it-agent/agent.o
put contrib/it-gles/gles_stubs.h
put contrib/it-agent/__pycache__/cached.pyc
put contrib/it-agent/.hidden
after="$(src)"
added="$(jq -rn --argjson a "$after" --argjson b "$before" '($a | keys) - ($b | keys) | .[]')"
[ "$added" = contrib/it-agent/data/new-input.dat ] || fail "added: $added"
rm "$T/contrib/it-agent/data/new-input.dat"
[ "$(src)" = "$before" ] || fail "removing the source did not restore the inputs"

# exported headers, entitlements and the validation recipes are inputs; Python files no longer are
for f in contrib/ios-app/api.h contrib/macos-app/api.h tests/guest-package/format-test.sh; do
    put "$f"
    src | jq -e --arg f "$f" 'has($f)' >/dev/null || fail "$f is not an input"
done
put tests/guest-package/old_test.py
src | jq -e 'has("tests/guest-package/old_test.py") | not' >/dev/null || fail "a .py file is an input"
before="$(src)"
put contrib/macos-app/entitlements.plist changed
[ "$(src)" != "$before" ] || fail "an entitlements change went unseen"

# an SDK change is detected even when size and mtime are preserved
put SDK/usr/include/header.h old
before="$(tree)"
touch -r "$T/SDK/usr/include/header.h" "$T/stamp"
printf new > "$T/SDK/usr/include/header.h"
touch -r "$T/stamp" "$T/SDK/usr/include/header.h"
[ "$(tree)" != "$before" ] || fail "a same-size, same-mtime SDK change went unseen"
rm -rf "$T/SDK"

# the SDK's inventory and executable modes are dependencies
put SDK/tool
before="$(tree)"
chmod 755 "$T/SDK/tool"
[ "$(tree)" != "$before" ] || fail "a mode change went unseen"
put SDK/additional-header.h
[ "$(tree | jq .files)" = 2 ] || fail "inventory"
rm "$T/SDK/tool"
[ "$(tree | jq .files)" = 1 ] || fail "inventory after a deletion"
if "$BI" tree "$T/absent" 2>"$T/err"; then fail "an absent SDK was accepted"; fi
grep -q 'empty build input' "$T/err" || fail "$(cat "$T/err")"
mkdir "$T/empty"
if "$BI" tree "$T/empty" 2>/dev/null; then fail "an empty SDK was accepted"; fi
echo "PASS: build inputs: nested sources, generated files skipped, headers and recipes, SDK content, modes, inventory"
