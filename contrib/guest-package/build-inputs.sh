#!/bin/bash
# The actual guest sources and external build inputs, independent of the emulator's HEAD.
#   build-inputs.sh               {"inputs": {path: sha256}, "build_context": {sdk, tools, clang_headers}} (sorted keys)
#   build-inputs.sh --components  the contrib components guest-package/build.sh copies
#   build-inputs.sh sources ROOT  the inputs object for the checkout at ROOT
#   build-inputs.sh tree DIR      {path, sha256, files}: a directory's contents, modes and inventory
# export-guest-artifacts.sh runs it before and after the build and refuses output whose inputs changed.
set -euo pipefail
export LC_ALL=C
COMPONENTS="armv6-toolchain it-gles gles-public it-agent it-instprogress it-media it-proxy it-status it-halt
it-orientation ipad1-guest appsync it-boot it-pasteboard it-seal it-prefs it-keybag it-heading it-gyro
it-cctest it-gltest it-msmquiet guest-package"

# NUL-separated paths on stdin -> one JSON object {path relative to $1: sha256}
hashes() {
    set -e   # bash clears -e in command substitutions
    xargs -0 shasum -a 256 -- | jq -R --arg root "$1/" \
        '(.[0:64]) as $h | (.[66:] | ltrimstr($root)) as $p | {($p): $h}' | jq -S -s 'add // {}'
}

sources() {
    set -e
    local root="$1" c f
    {
        for c in $COMPONENTS; do
            [ -d "$root/contrib/$c" ] || continue
            find "$root/contrib/$c" -type f ! -path '*/__pycache__/*' | while IFS= read -r f; do
                case "${f##*/}" in .*|*.md|*.o|*.py|*.pyc|*.cclog|*.ldlog|*.itpack|gles_stubs.h) continue ;; esac
                # built Mach-Os (thin, fat, 64-bit) are outputs, not inputs
                case "$(od -An -tx1 -N4 "$f" | tr -d ' \n')" in cefaedfe|cafebabe|cffaedfe) continue ;; esac
                printf '%s\0' "$f"
            done
        done
        # the tests guest-package/build.sh copies and it-boot/build.sh runs; the headers the app compiles against
        for f in "$root"/tests/guest-package/* "$root"/contrib/ios-app/*.h "$root"/contrib/macos-app/*.h; do
            [ -f "$f" ] && [ "${f%.py}" = "$f" ] && printf '%s\0' "$f"
        done
        for f in include/hw/arm/guest-services/gles-names.h contrib/guest-package/VERSION \
                 contrib/export-guest-artifacts.sh contrib/macos-app/entitlements.plist; do
            [ -f "$root/$f" ] || { echo "build-inputs: missing $root/$f" >&2; exit 1; }
            printf '%s\0' "$root/$f"
        done
    } | sort -zu | hashes "$root"
}

# A directory's contents (name, sha256, executable bits) and inventory, without embedding thousands of paths.
# Files and symlinks to files count; symlinked directories are not followed.
tree() {
    set -e
    local dir list
    dir="$(cd "$1" 2>/dev/null && pwd -P)" || { echo "build-inputs: empty build input: $1" >&2; exit 1; }
    list="$(cd "$dir" && find . \( -type f -o \( -type l -exec test -f {} \; \) \) -print | sed 's|^\./||' | sort)"
    [ -n "$list" ] || { echo "build-inputs: empty build input: $1" >&2; exit 1; }
    paste <(cd "$dir" && tr '\n' '\0' <<<"$list" | xargs -0 shasum -a 256 -- | cut -c1-64) \
          <(cd "$dir" && tr '\n' '\0' <<<"$list" | xargs -0 stat -L -f '%Lp' --) <(printf '%s\n' "$list") |
        awk -F'\t' '{ m = substr("000" $2, length($2) + 1); x = 0
                      for (i = 1; i <= 3; i++) x = x * 8 + substr(m, i, 1) % 2
                      printf "%s\t%s\t%d\n", $3, $1, x * 1 }' |
        shasum -a 256 | cut -c1-64 |
        jq -R -S --arg path "$dir" --argjson files "$(wc -l <<<"$list")" '{path: $path, sha256: ., files: $files}'
}

tool() {   # NAME PATH -> {NAME: {path, sha256}}
    set -e
    local p
    p="$(realpath "$2")"
    jq -n -S --arg n "$1" --arg p "$p" --arg h "$(shasum -a 256 "$p" | cut -c1-64)" '{($n): {path: $p, sha256: $h}}'
}

context() {
    set -e
    local armv6="${ARMV6_SDK:-$HOME/Developer/ipod2g-re/OldSDK/iPhoneOS3.1.3.sdk}"
    local ipad="${IPAD_SDK:-$HOME/Developer/qemu-ios-files/ipad1/sdk/x-iPhoneSDK3_2_2/Payload/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS3.2.sdk}"
    local clang name path tools=()
    clang="$(xcrun --find clang)"
    for name in clang ld nm; do
        tools+=("$(tool "$name" "$(xcrun --find "$name")")")
    done
    for name in lipo file ldid; do
        path="$(command -v "$( [ "$name" = ldid ] && echo "${LDID:-ldid}" || echo "$name")")" ||
            { echo "build-inputs: guest build tool not found: $name" >&2; exit 1; }
        tools+=("$(tool "$name" "$path")")
    done
    local a i t h
    a="$(tree "$armv6")"; i="$(tree "$ipad")"; h="$(tree "$("$clang" -print-resource-dir)/include")"
    t="$(printf '%s\n' "${tools[@]}" | jq -s add)"
    jq -n -S --argjson armv6 "$a" --argjson ipad "$i" --argjson tools "$t" --argjson headers "$h" \
        '{sdk: {armv6: $armv6, ipad: $ipad}, tools: $tools, clang_headers: $headers}'
}

case "${1:-}" in
--components) echo $COMPONENTS ;;
sources) sources "$(cd "$2" && pwd)" ;;
tree) tree "$2" ;;
"") root="$(cd "$(dirname "$0")/../.." && pwd)"
    inputs="$(sources "$root")"; context="$(context)"
    jq -n -S --argjson inputs "$inputs" --argjson context "$context" \
        '{inputs: $inputs, build_context: $context}' ;;
*) echo "usage: build-inputs.sh [--components | sources ROOT | tree DIR]" >&2; exit 2 ;;
esac
