#!/bin/bash
# it_boot (contrib/it-boot) on the host under ASan/UBSan, against a fake qc() (it-boot-test-host.h).
# Each device is a directory: root/ is /usr/local/lighttouch, sys/ the system volume, and the files beside them
# are the fake host (offer, serve/, the flags boot writes) and its logs (reports, launchctl). Exit 0 = pass.
# contrib/it-boot/build.sh runs it.
set -euo pipefail
umask 022
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
EXE="$T/it_boot_host"
clang -g -Wall -Wextra -Wno-unused-result -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -DIT_BOOT_TEST="\"$HERE/it-boot-test-host.h\"" -I"$ROOT/contrib/it-boot" "$HERE/it-boot-test.c" -o "$EXE"

MBX=/System/Library/Frameworks/OpenGLES.framework/MBXGLEngine.bundle/MBXGLEngine
TYPEIN=/usr/lib/it_typein.dylib
Z64=0000000000000000000000000000000000000000000000000000000000000000

fail() { echo "FAIL (lines ${BASH_LINENO[*]}): $*" >&2; exit 1; }
eq() { [ "$1" = "$2" ] || fail "got '$1', want '$2'"; }
neg() { [ "$1" -lt 0 ] || fail "got $1, want < 0"; }
bytes() { cmp -s "$1" <(printf '%s' "$2") || fail "$1: '$(head -c 80 "$1" 2>/dev/null)', want '$2'"; }
gone() { if [ -e "$1" ] || [ -L "$1" ]; then fail "$1 exists"; fi; }
has() { grep -qF -- "$2" <<<"$1" || fail "'$2' not in: $1"; }
lacks() { if grep -qF -- "$2" <<<"$1"; then fail "'$2' in: $1"; fi; }
xs() { head -c "$1" /dev/zero | tr '\0' x; }

# the current device: D; its views
dev() { D="$T/$1"; mkdir -p "$D/root/pkgs" "$D/sys"; }
s() { echo "$D/sys$1"; }
cur() { local l; l="$(readlink "$D/root/current")"; echo "${l#pkgs/}"; }
lc() { cat "$D/launchctl" 2>/dev/null || true; }
state() { cat "$D/root/state"; }
r0() { head -1 "$D/reports" 2>/dev/null | cut -d' ' -f1,2; }
r0text() { head -1 "$D/reports" | cut -d' ' -f3-; }
load() { echo "load $D/root/pkgs/$1/jobs/${2:-com.qemu.it-agent.plist}"; }
unload() { echo "unload $D/root/pkgs/$1/jobs/${2:-com.qemu.it-agent.plist}"; }

# a package the fake host serves: pkg SERIAL [BUILD], then add KIND PATH DATA [TARGET RESPRING]; OFFER is its offer
pkg() {
    OFFER="ltpkg 1"$'\n'"build ${2:-7E18}"$'\n'"serial $1 $1.0"$'\n'
    IDX=0
}
add() {
    local f="$D/serve/$2" mode=755 line
    mkdir -p "$(dirname "$f")"
    printf '%s' "$3" > "$f"
    if [ "$1" = job ]; then mode=644; fi
    line="$1 $IDX $2 $mode $(stat -f %z "$f") $(shasum -a 256 "$f" | cut -c1-64)"
    if [ -n "${4:-}" ]; then
        line+=" $4"
        if [ "$5" = 1 ]; then line+=" respring"; fi
    fi
    OFFER+="$line"$'\n'
    IDX=$((IDX + 1))
}
# the usual package: the agent, its job, the MBX shim (if given) and the typein hook
std() {   # SERIAL [SHIM] [EXTRA] [BUILD]
    pkg "$1" "${4:-7E18}"
    add file bin/it_agent "agent v$1${3:-}"
    add job jobs/com.qemu.it-agent.plist "<plist>$1</plist>"
    if [ -n "${2:-}" ]; then add hook hooks/MBXGLEngine "$2" "$MBX" 1; fi
    add hook hooks/it_typein.dylib "typein $1" "$TYPEIN" 0
}
offer() {   # [VERDICT...]: OFFER, with "verdict V" lines after its first
    local v ins=""
    for v in "$@"; do ins+="verdict $v"$'\n'; done
    printf '%s\n%s%s' "${OFFER%%$'\n'*}" "$ins" "${OFFER#*$'\n'}" > "$D/offer"
}
boot() {   # [FLAG=VALUE...] -> RC
    local f
    for f in silent corrupt fail_at nosha build slow tick_step readonly reports launchctl; do rm -f "$D/$f"; done
    for f in "$@"; do printf '%s' "${f#*=}" > "$D/${f%%=*}"; done
    RC="$("$EXE" "$D" 2>"$D.stderr")" || fail "it_boot_host failed: $(cat "$D.stderr")"
}
put() {   # PATH DATA MODE
    mkdir -p "$(dirname "$1")"
    printf '%s' "$2" > "$1"
    chmod "$3" "$1"
}
# a prepared device: seed package 10 (agent, job, MBX shim), current, state, the hook target and its .baked
seeded() {
    dev "$1"
    pkg 10
    add file bin/it_agent "agent v10"
    add job jobs/com.qemu.it-agent.plist "<plist>10</plist>"
    add hook hooks/MBXGLEngine "shim v10" "$MBX" 1
    put "$D/root/pkgs/10/bin/it_agent" "agent v10" 755
    put "$D/root/pkgs/10/jobs/com.qemu.it-agent.plist" "<plist>10</plist>" 644
    put "$D/root/pkgs/10/hooks/MBXGLEngine" "shim v10" 755
    put "$(s "$MBX")" "shim v10" 755
    put "$(s "$MBX.baked")" "shim v10" 755
    printf '%s' "$OFFER" > "$D/root/pkgs/10/offer"
    ln -s pkgs/10 "$D/root/current"
    printf 'seed 10\n' > "$D/root/state"
}
snap() { (cd "$D/root" && find . | sort && cat state); }

# 7.x read-only root: the baked package's jobs load, the offer is reported as read-only, nothing is written
seeded ro
std 11 "shim v11"; offer
before="$(snap)"
boot readonly=1; eq "$RC" 6; eq "$(cur)" 10; bytes "$(s "$MBX")" "shim v10"
eq "$(lc)" "$(load 10)"
eq "$(r0)" "10 6"; has "$(r0text)" "read-only root"
eq "$(snap)" "$before"

# silent host: current unchanged, its jobs loaded, no tries counted, nothing reported
seeded dev; DEV="$D"
std 11 "shim v11"; offer
boot silent=1; eq "$RC" 0; eq "$(cur)" 10; gone "$D/reports"
eq "$(lc)" "$(load 10)"
has "$(state)" "tries 0"

# a good install: staged, flipped, hooks applied with .baked kept, jobs swapped, one respring, reported
boot; eq "$RC" 1; eq "$(cur)" 11
bytes "$D/root/pkgs/11/bin/it_agent" "agent v11"
bytes "$(s "$MBX")" "shim v11"; bytes "$(s "$MBX.baked")" "shim v10"
bytes "$(s "$TYPEIN")" "typein 11"; gone "$(s "$TYPEIN.baked")"
eq "$(lc)" "$(unload 10)"$'\n'"$(load 11)"$'\n'"stop com.apple.SpringBoard"
eq "$(r0)" "11 1"; has "$(r0text)" "prev 10"
eq "$(stat -f %Lp "$D/root/pkgs/11/bin/it_agent")" 755

# Wall-clock synchronization jumps a billion seconds during payload reads.
# It cannot expire the monotonic budget, but genuinely elapsed time can.
seeded timed1
std 11 "" "$(xs 4096)"; offer
boot; eq "$RC" 1; eq "$(cur)" 11
seeded timed2
std 11 "" "$(xs 4096)"; offer
boot slow=1; neg "$RC"; eq "$(cur)" 10
gone "$D/root/pkgs/11"
awk '$2 < 0 { found = 1 } END { exit !found }' "$D/reports" || fail "no failure reported"

# A larger valid transfer takes >10 simulated seconds and must receive
# its size allowance, without extending the clock on each progress call.
seeded sized
std 11 "" "$(xs 131072)"; offer
boot tick_step=80000000; eq "$RC" 1; eq "$(cur)" 11
seeded capped
std 11 "" "$(xs 131072)"; offer
boot tick_step=500000000; neg "$RC"; eq "$(cur)" 10

# the same offer again: nothing moves, no respring, jobs reloaded for this boot
D="$DEV"
boot; eq "$RC" 0; eq "$(cur)" 11; lacks "$(lc)" "stop com.apple.SpringBoard"

# host judges it good: stays across verdict-less boots
std 11 "shim v11"; offer "good 11"
boot; eq "$RC" 0; has "$(state)" "good 11"
std 11 "shim v11"; offer
for _ in 1 2 3; do boot; eq "$RC" 0; eq "$(cur)" 11; done

# bad verdict on a later package: back to the previous one, hooks and jobs follow, never re-installed
std 12 "shim v12"; offer
boot; eq "$RC" 1; eq "$(cur)" 12; bytes "$(s "$MBX")" "shim v12"
std 12 "shim v12"; offer "bad 12"
boot; eq "$RC" 3; eq "$(cur)" 11; bytes "$(s "$MBX")" "shim v11"
has "$(lc)" "$(unload 12)"
has "$(lc)" "stop com.apple.SpringBoard"
eq "$(r0)" "11 3"
boot; eq "$RC" 5; eq "$(cur)" 11; eq "$(r0)" "11 5"

# no verdict: two boots, then back to the previous package
std 13 "shim v13"; offer
boot; eq "$RC" 1; eq "$(cur)" 13
boot; eq "$RC" 0; eq "$(cur)" 13
boot; eq "$RC" 4; eq "$(cur)" 11
boot; eq "$RC" 5; eq "$(cur)" 11

# serial 0: safe mode on the seed, stock hooks restored from .baked
OFFER=$'ltpkg 1\nbuild 7E18\nserial 0\n'; offer
boot; eq "$RC" 2; eq "$(cur)" 10; bytes "$(s "$MBX")" "shim v10"
eq "$(r0)" "10 2"
# and back to a package still on disk without fetching it
OFFER=$'ltpkg 1\nbuild 7E18\nserial 11\n'; offer
boot; eq "$RC" 2; eq "$(cur)" 11; bytes "$(s "$MBX")" "shim v11"
# a verdict for the package being switched to counts at once
OFFER=$'ltpkg 1\nbuild 7E18\nserial 0\n'; offer
boot; eq "$RC" 2; eq "$(cur)" 10
OFFER=$'ltpkg 1\nbuild 7E18\nserial 11\n'; offer "good 11"
boot; eq "$RC" 2; eq "$(cur)" 11; has "$(state)" "tries 0"; has "$(state)" "good 11"

# a hook target the seed never had .baked: created from the stock file on first override
seeded typein
put "$(s "$TYPEIN")" "stock typein" 755
std 11; offer
boot; eq "$RC" 1; bytes "$(s "$TYPEIN")" "typein 11"
bytes "$(s "$TYPEIN.baked")" "stock typein"
lacks "$(lc)" "stop com.apple.SpringBoard"   # not a respring hook
OFFER=$'ltpkg 1\nserial 0\n'; offer
boot; eq "$RC" 2; bytes "$(s "$TYPEIN")" "stock typein"

# bad hash: nothing installed, current unchanged, the failure reported
seeded badhash
std 11 "shim v11"; offer
boot corrupt=0; eq "$RC" -94; eq "$(cur)" 10   # -EBADMSG (Darwin)
gone "$D/root/pkgs/11"; gone "$D/root/pkgs/11.tmp"
bytes "$(s "$MBX")" "shim v10"; eq "$(r0)" "10 -94"
# size-only fallback where CommonCrypto is missing: the same bytes now pass
boot corrupt=0 nosha=1; eq "$RC" 1; eq "$(cur)" 11

# torn installs: a transfer dying halfway, stale staging dirs, a truncated package on disk
seeded torn
std 11 "shim v11" "$(xs 5000)"; offer
boot fail_at="0 2048"; eq "$RC" -5; eq "$(cur)" 10   # -EIO
gone "$D/root/pkgs/11.tmp"
mkdir "$D/root/pkgs/11.tmp"
printf x > "$D/root/pkgs/11.tmp/junk"
ln -s pkgs/999 "$D/root/current.lt-new"
mkdir "$D/root/pkgs/11"
cp "$D/offer" "$D/root/pkgs/11/offer"
boot; eq "$RC" 1; eq "$(cur)" 11
eq "$(stat -f %z "$D/root/pkgs/11/bin/it_agent")" 5009
gone "$D/root/pkgs/11.tmp"; gone "$D/root/current.lt-new"
# current pointing at a package that is gone: back on the seed
rm "$D/root/current"
ln -s pkgs/77 "$D/root/current"
boot silent=1; eq "$RC" 0; eq "$(cur)" 10

# an offer for another firmware build, and malformed offers, change nothing
seeded wrongbuild
std 11 "shim v11" "" 8C148; offer
boot; eq "$RC" -8; eq "$(cur)" 10   # -ENOEXEC
for bad in $'garbage\n' $'ltpkg 1\nserial 11\nfile 0 ../../etc/passwd 755 1 '"$Z64"$'\n' \
           $'ltpkg 1\nserial 11\nhook 0 h 755 1 '"$Z64"$' relative/target\n' $'ltpkg 1\nserial x\n'; do
    printf '%s' "$bad" > "$D/offer"
    boot; eq "$RC" -22; eq "$(cur)" 10   # -EINVAL
done
eq "$(ls "$D/root/pkgs")" 10

# an image without a seed package (legacy): installs, and bad has nowhere to go but stays reported
dev bare
std 11; offer
boot; eq "$RC" 1; eq "$(cur)" 11; has "$(state)" "seed -1"
std 11; offer "bad 11"
boot; eq "$RC" -2; eq "$(cur)" 11   # -ENOENT: nothing to revert to

# a device the preparer seeded (FirmwareKit's GuestPackage.seed): seed package 7 (agent, job, MBX and typein
# hooks) with its offer, current, the state naming the installed hooks, each target with the package's bytes
# and its .baked keeping what the volume had, the loader and its job
D="$T/seed"; SEED="$D"; V="$D/sys"
put "$V/System/Library/CoreServices/SystemVersion.plist" "<plist><dict><key>ProductBuildVersion</key><string>7E18</string></dict></plist>" 644
put "$V/usr/local/bin/it_boot" loader 755
put "$V/System/Library/LaunchDaemons/com.qemu.it-boot.plist" "<plist/>" 644
pkg 7
add file bin/it_agent agent
add job jobs/j.plist "<j/>"
add hook hooks/MBXGLEngine shim "$MBX" 1
add hook hooks/it_typein.dylib t "$TYPEIN" 0
L="$V/usr/local/lighttouch"
put "$L/pkgs/7/bin/it_agent" agent 755
put "$L/pkgs/7/jobs/j.plist" "<j/>" 644
put "$L/pkgs/7/hooks/MBXGLEngine" shim 755
put "$L/pkgs/7/hooks/it_typein.dylib" t 755
put "$L/pkgs/7/offer" "$OFFER" 644
ln -s pkgs/7 "$L/current"
put "$L/state" "seed 7"$'\n'"hook 1 $MBX"$'\n'"hook 0 $TYPEIN"$'\n' 644
put "$(s "$MBX.baked")" "stock mbx" 644
put "$(s "$MBX")" shim 755
put "$(s "$TYPEIN.baked")" "old typein" 644
put "$(s "$TYPEIN")" t 755
ln -s sys/usr/local/lighttouch "$D/root"
copy() { cp -RP "$SEED" "$T/$1"; D="$T/$1"; }

# A different first offer must restore seeded hooks without a same-offer
# boot first teaching the loader about them. Exercise the real loader.
copy first-offer-removes-hooks
pkg 8; add file bin/it_agent "no hooks"; offer
boot; eq "$RC" 1; eq "$(cur)" 8
bytes "$(s "$MBX")" "stock mbx"      # first offer forgot the seeded MBX hook
bytes "$(s "$TYPEIN")" "old typein"  # first offer forgot the seeded typein hook
bytes "$(s "$MBX.baked")" "stock mbx"
bytes "$(s "$TYPEIN.baked")" "old typein"
eq "$(stat -f %Lp "$(s "$MBX")")" "$(stat -f %Lp "$(s "$MBX.baked")")"
has "$(lc)" "stop com.apple.SpringBoard"
# Removing only a non-respring hook restores it without restarting UI.
copy first-offer-removes-nonrespring
pkg 8; add file bin/it_agent "new agent"; add hook hooks/MBXGLEngine shim "$MBX" 1; offer
boot; eq "$RC" 1; bytes "$(s "$TYPEIN")" "old typein"
bytes "$(s "$MBX")" shim
lacks "$(lc)" "stop com.apple.SpringBoard"
# A cache-only original has no on-disk bytes. The preparer's explicit
# absence marker must remove the override on the very first no-hook offer.
copy absent-original
rm "$(s "$MBX.baked")"
put "$(s "$MBX.baked-absent")" "" 644
pkg 8; add file bin/it_agent "no override"; offer
boot; eq "$RC" 1
gone "$(s "$MBX")"   # cache-only stock remained shadowed by frontend
bytes "$(s "$MBX.baked-absent")" ""
has "$(lc)" "stop com.apple.SpringBoard"
# Reinstall preserves original absence. Bad verdict returns to the
# no-hook package and removes the override again, not a fake .baked file.
pkg 9; add file bin/it_agent override; add hook hooks/MBXGLEngine "new frontend" "$MBX" 1; offer "good 8"
boot; eq "$RC" 1; bytes "$(s "$MBX")" "new frontend"
gone "$(s "$MBX.baked")"
offer "bad 9"
boot; eq "$RC" 3; eq "$(cur)" 8
gone "$(s "$MBX")"
bytes "$(s "$MBX.baked-absent")" ""
# Conflicting provenance is rejected without changing the current file.
copy conflicting-backups
put "$(s "$MBX.baked-absent")" "" 644
pkg 8; add file bin/it_agent "no hooks"; offer
boot; eq "$RC" 1; bytes "$(s "$MBX")" shim
has "$(cat "$D.stderr")" "conflicting or invalid backup"

# Hooks introduced dynamically have the same absent-original contract.
dev dynamic-absent
pkg 1; add hook hooks/typein dynamic "$TYPEIN" 0; offer
boot; eq "$RC" 1; bytes "$(s "$TYPEIN")" dynamic
bytes "$(s "$TYPEIN.baked-absent")" ""
gone "$(s "$TYPEIN.baked")"
pkg 2; offer "good 1"
boot; eq "$RC" 1; gone "$(s "$TYPEIN")"
lacks "$(lc)" "stop com.apple.SpringBoard"
dev dangling-original
mkdir -p "$(dirname "$(s "$TYPEIN")")"
ln -s missing-original "$(s "$TYPEIN")"
pkg 1; add hook hooks/typein new "$TYPEIN" 1; offer
boot; eq "$RC" 1; [ -L "$(s "$TYPEIN")" ] || fail "the dangling original was replaced"
eq "$(readlink "$(s "$TYPEIN")")" missing-original
gone "$(s "$TYPEIN.baked-absent")"
has "$(cat "$D.stderr")" "cannot preserve original"
lacks "$(lc)" "stop com.apple.SpringBoard"
copy malformed-absence
rm "$(s "$MBX.baked")"
put "$(s "$MBX.baked-absent")" "not empty" 644
pkg 8; add file bin/it_agent "no hooks"; offer
boot; eq "$RC" 1; bytes "$(s "$MBX")" shim
has "$(cat "$D.stderr")" "conflicting or invalid backup"

dev failed-dynamic-copy
mkdir -p "$(s "$MBX")"
pkg 1; add hook hooks/engine frontend "$MBX" 1; offer
boot; eq "$RC" 1; [ -d "$(s "$MBX")" ] || fail "the directory at the target was replaced"
gone "$(s "$MBX.baked-absent")"
has "$(cat "$D.stderr")" "cannot keep"
lacks "$(lc)" "stop com.apple.SpringBoard"

# the seeded device offered its own seed: it_boot takes it as is, nothing to change, no respring
D="$SEED"
cp "$L/pkgs/7/offer" "$D/offer"
boot; eq "$RC" 0; eq "$(cur)" 7; lacks "$(cat "$D.stderr")" hook
eq "$(lc)" "$(load 7 j.plist)"
eq "$(r0)" "7 0"; has "$(r0text)" "seed 7"
boot silent=1; eq "$RC" 0; eq "$(lc)" "$(load 7 j.plist)"

echo "PASS: silent host, install, good/bad verdicts, no-verdict retries, safe mode, .baked hooks," \
     "bad hash, size-only fallback, torn installs, wrong build, malformed offers," \
     "seed first-offer hook removal, filtered/failed hooks, genuine cached absence," \
     "dynamic absence removal/reinstall/rollback, conflicting backups and dangling originals"
