#!/bin/bash
# Run one slice check: a C test that compiles production functions cut out of hw/ sources next to its own
# stubs, under ASan/UBSan, with no QEMU build.
#
#   tests/slice/run.sh tests/slice/NAME.c
#
# Directives, one per line anywhere in the file (normally its first comment):
#   SLICE[:GROUP] PATH fn NAME...          each function's definition (the first that is not a prototype)
#   SLICE[:GROUP] PATH typedef NAME...     `typedef struct|enum|union NAME ... } NAME;`
#   SLICE[:GROUP] PATH define ERE          every `#define` line whose name matches ^(ERE)
#   SLICE[:GROUP] PATH range START | END   the text from START up to (not including) END; \n is a newline
#   SLICE[:GROUP] PATH file                the whole file
#   CFLAGS ...                             extra compiler flags; PKG NAME... adds pkg-config --cflags --libs ($ROOT is the tree in both)
#   VARIANT ...                            build and run once per VARIANT line, with these flags added
# The slices go, in directive order, into GROUP.h (default slice.h) for the test to #include. PATH is relative to
# the tree. The check passes when it builds and every run exits 0 (UBSan halts on the first report).
set -euo pipefail -f   # -f: directive words are never globs
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TEST="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/slice.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# cut FILE KIND ARGS...: one slice to stdout, or fail naming what is missing
cut() {
    local file="$ROOT/$1" kind="$2"; shift 2
    [ -f "$file" ] || { echo "slice: no $file" >&2; return 1; }
    case "$kind" in
        file) cat "$file" ;;
        define) grep -E "^#define[[:space:]]+($*)" "$file" || { echo "slice: no #define ^($*) in $file" >&2; return 1; } ;;
        fn|typedef) for name in "$@"; do
                awk -v name="$name" -v kind="$kind" '
                    function start(line) {
                        if (kind == "typedef")
                            return line ~ ("^typedef (struct|enum|union) " name "([^A-Za-z0-9_]|$)")
                        return line ~ /^[A-Za-z_]/ && line ~ ("(^|[^A-Za-z0-9_])" name "\\(")
                    }
                    !inside && start($0) { inside = 1; text = ""; body = 0 }
                    inside {
                        text = text $0 "\n"
                        if (kind == "fn" && !body) {
                            if ($0 ~ /\{/) body = 1
                            else if ($0 ~ /;[[:space:]]*$/) { inside = 0; next }   # a prototype
                        }
                        if ((kind == "fn" && $0 ~ /^}/) || (kind == "typedef" && $0 ~ ("^}[[:space:]]*" name "[[:space:]]*;"))) {
                            printf "%s", text; found = 1; exit
                        }
                    }
                    END { exit !found }' "$file" || { echo "slice: no $kind $name in $file" >&2; return 1; }
            done ;;
        range) awk -v spec="$*" '
                BEGIN { RS = "\001"; i = index(spec, " | "); a = substr(spec, 1, i - 1); b = substr(spec, i + 3)
                        gsub(/\\n/, "\n", a); gsub(/\\n/, "\n", b) }
                { s = index($0, a); if (!s || !i) exit 1; rest = substr($0, s); e = index(rest, b); if (!e) exit 1
                  printf "%s", substr(rest, 1, e - 1); found = 1 }
                END { exit !found }' "$file" || { echo "slice: no range $* in $file" >&2; return 1; } ;;
        *) echo "slice: unknown kind $kind" >&2; return 1 ;;
    esac
}

flags=() variants=()
: > "$TMP/slice.h"
while IFS= read -r line; do
    line="${line#"${line%%[![:space:]*/]*}"}"   # the directive may sit in a comment
    case "$line" in
        SLICE\ *|SLICE:*)
            read -r head path kind rest <<<"$line"
            group="${head#SLICE}"; group="${group#:}"
            { echo "// $path $kind $rest"; cut "$path" "$kind" $rest; echo; } >> "$TMP/${group:-slice}.h" ;;
        CFLAGS\ *) read -r -a more <<<"${line#CFLAGS }"; flags+=("${more[@]//\$ROOT/$ROOT}") ;;
        PKG\ *) line="${line#PKG }"; read -r -a more <<<"$(pkg-config --cflags --libs ${line//\$ROOT/$ROOT})"; flags+=("${more[@]}") ;;
        VARIANT\ *|VARIANT) variants+=("${line#VARIANT}") ;;
    esac
done < "$TEST"
[ ${#variants[@]} -gt 0 ] || variants=("")
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"
n=0
for variant in "${variants[@]}"; do
    read -r -a extra <<<"$variant"
    n=$((n + 1))
    xcrun clang -std=gnu11 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
        -Wno-unused-function -Wno-comment -I"$TMP" -I"$ROOT/tests/slice" ${flags[@]+"${flags[@]}"} ${extra[@]+"${extra[@]}"} \
        "$TEST" -o "$TMP/check$n"
    (cd "$TMP" && "$TMP/check$n")
done
