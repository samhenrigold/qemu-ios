#!/usr/bin/env python3
"""it_boot (contrib/it-boot) on the host under ASan/UBSan, against a fake qc().

The fake host is a directory: `offer` is what QC_PKG_OFFER serves and each
payload is served by index from serve/<the path its offer line names>, as the real
host does. `silent` makes every call fail, `corrupt` names an index whose
bytes get flipped, `fail_at` an "index offset" where reads start failing.
Reports and launchctl calls are appended to files for the checks.
"""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
HOOKS = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static char fake_dir[1024];
static void fake_path(char *out, const char *name) { snprintf(out, 1024, "%s/%s", fake_dir, name); }
static int fake_exists(const char *name) { char p[1024]; fake_path(p, name); return access(p, F_OK) == 0; }
static const char *root_dir(void) { static char p[1024]; fake_path(p, "root"); return p; }
static const char *sys_root(void) { static char p[1024]; fake_path(p, "sys"); return p; }
static int sha_disabled(void) { return fake_exists("nosha"); }
static void os_build(char *out, size_t n) {
    char p[1024]; fake_path(p, "build"); FILE *f = fopen(p, "r");
    snprintf(out, n, "7E18");
    if (f) { if (!fgets(out, (int)n, f)) out[0] = 0; out[strcspn(out, "\n")] = 0; fclose(f); }
}
static int launchctl(const char *verb, const char *arg) {
    char p[1024]; fake_path(p, "launchctl"); FILE *f = fopen(p, "a");
    fprintf(f, "%s %s\n", verb, arg); fclose(f); return 0;
}
static uint64_t fake_ticks;
static uint64_t fake_mach_absolute_time(void) { return fake_ticks; }
static kern_return_t fake_mach_timebase_info(mach_timebase_info_t info) {
    info->numer = 1; info->denom = 1; return 0;
}
static time_t fake_time(time_t *out) {
    time_t value = fake_ticks ? 2000000000 : 1000000000;
    if (out) *out = value;
    return value;
}
#define mach_absolute_time fake_mach_absolute_time
#define mach_timebase_info fake_mach_timebase_info
#define time fake_time
static char offer_buf[65536];
static long offer_len = -1;
static long fake_load(char *buf, size_t cap, const char *name) {
    char p[1024]; fake_path(p, name); FILE *f = fopen(p, "rb");
    if (!f) return -1;
    long n = (long)fread(buf, 1, cap, f); fclose(f); buf[n] = 0; return n;
}
/* index -> the path its offer line names, the way hw/arm/guest-package.c maps it */
static int fake_index_path(unsigned idx, char *out) {
    char copy[65536]; memcpy(copy, offer_buf, (size_t)offer_len); copy[offer_len] = 0;
    char *save = NULL;
    for (char *l = strtok_r(copy, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        char kind[16], path[512]; unsigned i;
        if (sscanf(l, "%15s %u %511s", kind, &i, path) == 3 && i == idx &&
            (!strcmp(kind, "file") || !strcmp(kind, "job") || !strcmp(kind, "hook"))) {
            snprintf(out, 1024, "%s/serve/%s", fake_dir, path); return 0;
        }
    }
    return -1;
}
static int64_t qc(uint32_t op, void *buf, uint32_t off, uint32_t len, uint64_t token) {
    assert(len <= 1024);
    if (fake_exists("silent")) return -1;
    if (op == 0x170) {
        if (off == 0) offer_len = fake_load(offer_buf, sizeof(offer_buf) - 1, "offer");
        if (offer_len < 0) return -1;
        if (!len) return offer_len;
        if (off >= offer_len) return 0;
        long n = offer_len - off < (long)len ? offer_len - off : (long)len;
        memcpy(buf, offer_buf + off, (size_t)n); return n;
    }
    if (op == 0x171) {
        if (len) {
            char step[64];
            uint64_t elapsed = fake_load(step, sizeof(step) - 1, "tick_step") > 0 ? strtoull(step, NULL, 10) : 1;
            fake_ticks += fake_exists("slow") ? UINT64_C(6000000000) : elapsed;
        }
        char p[1024]; static char data[1 << 20];
        if (offer_len < 0 || fake_index_path((unsigned)token, p)) return -1;
        FILE *f = fopen(p, "rb"); if (!f) return -1;
        long size = (long)fread(data, 1, sizeof(data), f); fclose(f);
        if (!len) return size;
        char spec[64]; unsigned fi, fo;
        if (fake_load(spec, sizeof(spec) - 1, "fail_at") > 0 && sscanf(spec, "%u %u", &fi, &fo) == 2 &&
            fi == token && off >= fo) return -1;
        if (off >= size) return 0;
        long n = size - off < (long)len ? size - off : (long)len;
        memcpy(buf, data + off, (size_t)n);
        if (fake_load(spec, sizeof(spec) - 1, "corrupt") > 0 && (unsigned)atoi(spec) == token && off == 0)
            ((char *)buf)[0] ^= 0x55;
        return n;
    }
    if (op == 0x172) {
        char p[1024]; fake_path(p, "reports"); FILE *f = fopen(p, "a");
        fprintf(f, "%llu %d %.*s\n", (unsigned long long)token, (int32_t)off, (int)len, (char *)buf);
        fclose(f); return 0;
    }
    return -1;
}
'''
MAIN = r'''
#include "it_boot.c"
int main(int argc, char **argv) {
    assert(argc == 2);
    snprintf(fake_dir, sizeof(fake_dir), "%s", argv[1]);
    (void)fake_time;
    struct offer maximum = {0}; struct pull_clock clock;
    maximum.nent = MAX_ENT;
    for (int i = 0; i < MAX_ENT; i++) maximum.e[i].size = FILE_MAX;
    assert(pull_clock_start(&clock, &maximum) == 0);
    assert(clock.seconds == PULL_MAX_SECONDS);
    printf("%d\n", it_boot_run());
    return 0;
}
'''

MBX = "/System/Library/Frameworks/OpenGLES.framework/MBXGLEngine.bundle/MBXGLEngine"
TYPEIN = "/usr/lib/it_typein.dylib"


class Device:
    def __init__(self, base, exe):
        self.d, self.exe = Path(base), exe
        (self.d / "root/pkgs").mkdir(parents=True)
        (self.d / "sys").mkdir()

    def rel(self, p):
        return self.d / p.lstrip("/")

    def package(self, serial, files, jobs=(), hooks=(), build="7E18"):
        """Serve a package from the fake host: files/jobs {path: bytes}, hooks [(path, bytes, target, respring)]."""
        lines, idx = ["ltpkg 1", "build " + build, "serial %d %d.0" % (serial, serial)], 0
        src = self.d / "serve"
        for kind, items in (("file", [(p, b, None, 0) for p, b in files.items()]),
                            ("job", [(p, b, None, 0) for p, b in dict(jobs).items()]),
                            ("hook", list(hooks))):
            for path, data, target, respring in items:
                (src / path).parent.mkdir(parents=True, exist_ok=True)
                (src / path).write_bytes(data)
                line = "%s %d %s %s %d %s" % (kind, idx, path, "644" if kind == "job" else "755",
                                                    len(data), hashlib.sha256(data).hexdigest())
                if target:
                    line += " " + target + (" respring" if respring else "")
                lines.append(line)
                idx += 1
        return "\n".join(lines) + "\n"

    def offer(self, text, verdicts=()):
        text = text.replace("\n", "\n" + "".join("verdict %s %d\n" % v for v in verdicts), 1) if verdicts else text
        (self.d / "offer").write_text(text)

    def bake_seed(self, serial, files, jobs, hooks):
        """What P4's preparer will do: the seed package, current, state, hook targets and .baked copies."""
        text = self.package(serial, files, jobs, hooks)
        pkg = self.d / "root/pkgs" / str(serial)
        for path, data in list(files.items()) + list(dict(jobs).items()):
            (pkg / path).parent.mkdir(parents=True, exist_ok=True)
            (pkg / path).write_bytes(data)
            (pkg / path).chmod(0o644 if path.startswith("jobs/") else 0o755)
        for path, data, target, _ in hooks:
            (pkg / path).parent.mkdir(parents=True, exist_ok=True)
            (pkg / path).write_bytes(data)
            (pkg / path).chmod(0o755)
            for t in (target, target + ".baked"):
                self.rel("sys" + t).parent.mkdir(parents=True, exist_ok=True)
                self.rel("sys" + t).write_bytes(data)
                self.rel("sys" + t).chmod(0o755)
        (pkg / "offer").write_text(text)
        os.symlink("pkgs/%d" % serial, self.d / "root/current")
        (self.d / "root/state").write_text("seed %d\n" % serial)

    def boot(self, **flags):
        for name in ("silent", "corrupt", "fail_at", "nosha", "build", "slow", "tick_step"):
            (self.d / name).unlink(missing_ok=True)
        for name, value in flags.items():
            (self.d / name).write_text(str(value))
        for log in ("reports", "launchctl"):
            (self.d / log).unlink(missing_ok=True)
        out = subprocess.run([self.exe, str(self.d)], check=True, capture_output=True, text=True)
        self.stderr = out.stderr
        return int(out.stdout)

    def current(self):
        return int(os.readlink(self.d / "root/current").split("/")[1])

    def reports(self):
        p = self.d / "reports"
        return [l.split(" ", 2) for l in p.read_text().splitlines()] if p.exists() else []

    def launchctl(self):
        p = self.d / "launchctl"
        return p.read_text().splitlines() if p.exists() else []

    def state(self):
        return (self.d / "root/state").read_text()

    def hook(self, target):
        return self.rel("sys" + target).read_bytes()


def seeded(base, exe):
    dev = Device(base, exe)
    dev.bake_seed(10, {"bin/it_agent": b"agent v10"}, {"jobs/com.qemu.it-agent.plist": b"<plist>10</plist>"},
                  [("hooks/MBXGLEngine", b"shim v10", MBX, 1)])
    return dev


def pkg(dev, serial, shim=None, extra=b"", build="7E18"):
    hooks = [("hooks/MBXGLEngine", shim, MBX, 1)] if shim else []
    hooks.append(("hooks/it_typein.dylib", b"typein %d" % serial, TYPEIN, 0))
    return dev.package(serial, {"bin/it_agent": b"agent v%d" % serial + extra},
                       {"jobs/com.qemu.it-agent.plist": b"<plist>%d</plist>" % serial}, hooks, build)


def main():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / "hooks.h").write_text(HOOKS)
        (tmp / "main.c").write_text(MAIN)
        exe = str(tmp / "it_boot_host")
        subprocess.run(["clang", "-g", "-Wall", "-Wextra", "-Wno-unused-result", "-Werror",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        '-DIT_BOOT_TEST="%s"' % (tmp / "hooks.h"), "-I" + str(root / "contrib/it-boot"),
                        str(tmp / "main.c"), "-o", exe], check=True)
        n = 0

        def case():
            nonlocal n
            n += 1
            return seeded(tmp / ("dev%d" % n), exe)

        # silent host: current unchanged, its jobs loaded, no tries counted, nothing reported
        dev = case()
        dev.offer(pkg(dev, 11, b"shim v11"))
        assert dev.boot(silent=1) == 0 and dev.current() == 10 and not dev.reports()
        assert dev.launchctl() == ["load %s/root/pkgs/10/jobs/com.qemu.it-agent.plist" % dev.d]
        assert "tries 0" in dev.state()

        # a good install: staged, flipped, hooks applied with .baked kept, jobs swapped, one respring, reported
        assert dev.boot() == 1 and dev.current() == 11
        assert (dev.d / "root/pkgs/11/bin/it_agent").read_bytes() == b"agent v11"
        assert dev.hook(MBX) == b"shim v11" and dev.hook(MBX + ".baked") == b"shim v10"
        assert dev.hook(TYPEIN) == b"typein 11" and not dev.rel("sys" + TYPEIN + ".baked").exists()
        assert dev.launchctl() == ["unload %s/root/pkgs/10/jobs/com.qemu.it-agent.plist" % dev.d,
                                   "load %s/root/pkgs/11/jobs/com.qemu.it-agent.plist" % dev.d,
                                   "stop com.apple.SpringBoard"], dev.launchctl()
        assert dev.reports()[0][:2] == ["11", "1"] and "prev 10" in dev.reports()[0][2]
        assert (os.stat(dev.d / "root/pkgs/11/bin/it_agent").st_mode & 0o777) == 0o755

        # Wall-clock synchronization jumps a billion seconds during payload reads.
        # It cannot expire the monotonic budget, but genuinely elapsed time can.
        timed = case()
        timed.offer(pkg(timed, 11, extra=b"x" * 4096))
        assert timed.boot() == 1 and timed.current() == 11
        timed = case()
        timed.offer(pkg(timed, 11, extra=b"x" * 4096))
        assert timed.boot(slow=1) < 0 and timed.current() == 10
        assert not (timed.d / "root/pkgs/11").exists()
        assert any(int(r[1]) < 0 for r in timed.reports())

        # A larger valid transfer takes >10 simulated seconds and must receive
        # its size allowance, without extending the clock on each progress call.
        sized = case()
        sized.offer(pkg(sized, 11, extra=b"x" * (128 * 1024)))
        assert sized.boot(tick_step=80000000) == 1 and sized.current() == 11
        capped = case()
        capped.offer(pkg(capped, 11, extra=b"x" * (128 * 1024)))
        assert capped.boot(tick_step=500000000) < 0 and capped.current() == 10

        # the same offer again: nothing moves, no respring, jobs reloaded for this boot
        assert dev.boot() == 0 and dev.current() == 11 and "stop com.apple.SpringBoard" not in dev.launchctl()

        # host judges it good: stays across verdict-less boots
        dev.offer(pkg(dev, 11, b"shim v11"), [("good", 11)])
        assert dev.boot() == 0 and "good 11" in dev.state()
        dev.offer(pkg(dev, 11, b"shim v11"))
        for _ in range(3):
            assert dev.boot() == 0 and dev.current() == 11

        # bad verdict on a later package: back to the previous one, hooks and jobs follow, never re-installed
        dev.offer(pkg(dev, 12, b"shim v12"))
        assert dev.boot() == 1 and dev.current() == 12 and dev.hook(MBX) == b"shim v12"
        dev.offer(pkg(dev, 12, b"shim v12"), [("bad", 12)])
        assert dev.boot() == 3 and dev.current() == 11 and dev.hook(MBX) == b"shim v11"
        assert "unload %s/root/pkgs/12/jobs/com.qemu.it-agent.plist" % dev.d in dev.launchctl()
        assert "stop com.apple.SpringBoard" in dev.launchctl()
        assert dev.reports()[0][:2] == ["11", "3"]
        assert dev.boot() == 5 and dev.current() == 11 and dev.reports()[0][:2] == ["11", "5"]

        # no verdict: two boots, then back to the previous package
        dev.offer(pkg(dev, 13, b"shim v13"))
        assert dev.boot() == 1 and dev.current() == 13
        assert dev.boot() == 0 and dev.current() == 13
        assert dev.boot() == 4 and dev.current() == 11
        assert dev.boot() == 5 and dev.current() == 11

        # serial 0: safe mode on the seed, stock hooks restored from .baked
        dev.offer("ltpkg 1\nbuild 7E18\nserial 0\n")
        assert dev.boot() == 2 and dev.current() == 10 and dev.hook(MBX) == b"shim v10"
        assert dev.reports()[0][:2] == ["10", "2"]
        # and back to a package still on disk without fetching it
        dev.offer("ltpkg 1\nbuild 7E18\nserial 11\n")
        assert dev.boot() == 2 and dev.current() == 11 and dev.hook(MBX) == b"shim v11"
        # a verdict for the package being switched to counts at once
        dev.offer("ltpkg 1\nbuild 7E18\nserial 0\n")
        assert dev.boot() == 2 and dev.current() == 10
        dev.offer("ltpkg 1\nbuild 7E18\nserial 11\n", [("good", 11)])
        assert dev.boot() == 2 and dev.current() == 11 and "tries 0" in dev.state() and "good 11" in dev.state()

        # a hook target the seed never had .baked: created from the stock file on first override
        dev = case()
        dev.rel("sys" + TYPEIN).parent.mkdir(parents=True, exist_ok=True)
        dev.rel("sys" + TYPEIN).write_bytes(b"stock typein")
        dev.rel("sys" + TYPEIN).chmod(0o755)
        dev.offer(pkg(dev, 11))
        assert dev.boot() == 1 and dev.hook(TYPEIN) == b"typein 11"
        assert dev.hook(TYPEIN + ".baked") == b"stock typein"
        assert "stop com.apple.SpringBoard" not in dev.launchctl()   # not a respring hook
        dev.offer("ltpkg 1\nserial 0\n")
        assert dev.boot() == 2 and dev.hook(TYPEIN) == b"stock typein"

        # bad hash: nothing installed, current unchanged, the failure reported
        dev = case()
        dev.offer(pkg(dev, 11, b"shim v11"))
        assert dev.boot(corrupt=0) == -94 and dev.current() == 10    # -EBADMSG (Darwin)
        assert not (dev.d / "root/pkgs/11").exists() and not (dev.d / "root/pkgs/11.tmp").exists()
        assert dev.hook(MBX) == b"shim v10" and dev.reports()[0][:2] == ["10", "-94"]
        # size-only fallback where CommonCrypto is missing: the same bytes now pass
        assert dev.boot(corrupt=0, nosha=1) == 1 and dev.current() == 11

        # torn installs: a transfer dying halfway, stale staging dirs, a truncated package on disk
        dev = case()
        dev.offer(pkg(dev, 11, b"shim v11", extra=b"x" * 5000))
        rc = dev.boot(fail_at="0 2048")
        assert rc == -5 and dev.current() == 10, (rc, dev.stderr)   # -EIO
        assert not (dev.d / "root/pkgs/11.tmp").exists()
        (dev.d / "root/pkgs/11.tmp").mkdir()
        (dev.d / "root/pkgs/11.tmp/junk").write_text("x")
        os.symlink("pkgs/999", dev.d / "root/current.lt-new")
        (dev.d / "root/pkgs/11").mkdir()
        (dev.d / "root/pkgs/11/offer").write_text((dev.d / "offer").read_text())
        assert dev.boot() == 1 and dev.current() == 11
        assert len((dev.d / "root/pkgs/11/bin/it_agent").read_bytes()) == 5009
        assert not (dev.d / "root/pkgs/11.tmp").exists() and not os.path.lexists(dev.d / "root/current.lt-new")
        # current pointing at a package that is gone: back on the seed
        os.unlink(dev.d / "root/current")
        os.symlink("pkgs/77", dev.d / "root/current")
        assert dev.boot(silent=1) == 0 and dev.current() == 10

        # an offer for another firmware build, and malformed offers, change nothing
        dev = case()
        dev.offer(pkg(dev, 11, b"shim v11", build="8C148"))
        assert dev.boot() == -8 and dev.current() == 10                   # -ENOEXEC
        for bad in ("garbage\n", "ltpkg 1\nserial 11\nfile 0 ../../etc/passwd 755 1 " + "0" * 64 + "\n",
                    "ltpkg 1\nserial 11\nhook 0 h 755 1 %s relative/target\n" % ("0" * 64),
                    "ltpkg 1\nserial x\n"):
            dev.offer(bad)
            assert dev.boot() == -22 and dev.current() == 10, bad         # -EINVAL
        assert not any(p.name != "10" for p in (dev.d / "root/pkgs").iterdir())

        # an image without a seed package (legacy): installs, and bad has nowhere to go but stays reported
        dev = Device(tmp / "bare", exe)
        dev.offer(pkg(dev, 11))
        assert dev.boot() == 1 and dev.current() == 11 and "seed -1" in dev.state()
        dev.offer(pkg(dev, 11), [("bad", 11)])
        rc = dev.boot()
        assert rc == -2 and dev.current() == 11, (rc, dev.state())         # -ENOENT: nothing to revert to

        # the preparers' seed (mkpkg.seed): it_boot takes it as is, nothing to change, no respring
        sys.path.insert(0, str(root / "contrib/guest-package"))
        import mkpkg
        dev = Device.__new__(Device)
        dev.d, dev.exe = tmp / "seed", exe
        vol = dev.d / "sys"
        (vol / "System/Library/CoreServices").mkdir(parents=True)
        (vol / "System/Library/LaunchDaemons").mkdir()
        (vol / "System/Library/CoreServices/SystemVersion.plist").write_bytes(
            mkpkg.plistlib.dumps({"ProductBuildVersion": "7E18"}))
        dev.rel("sys" + MBX).parent.mkdir(parents=True)
        dev.rel("sys" + MBX).write_bytes(b"stock mbx")
        payload = (("bin/it_agent", "755", b"agent"), ("jobs/j.plist", "644", b"<j/>"),
                   ("hooks/MBXGLEngine", "755", b"shim"), ("hooks/it_typein.dylib", "755", b"t"))
        manifest = {"serial": 7, "version": "7.0", "requires": {"builds": ["7E18"]}, "jobs": ["jobs/j.plist"],
                    "files": [{"name": n, "mode": md, "size": len(b), "sha256": hashlib.sha256(b).hexdigest()}
                              for n, md, b in payload],
                    "hooks": [{"file": "hooks/MBXGLEngine", "target": MBX, "respring": True},
                              {"file": "hooks/it_typein.dylib", "target": TYPEIN, "respring": True}]}
        mkpkg.pack([("f/manifest.json", json.dumps(manifest).encode())] + [("f/" + n, b) for n, _, b in payload]
                   + [("loader/it_boot", b"loader"), ("loader/com.qemu.it-boot.plist", b"<plist/>"),
                      ("g/manifest.json", json.dumps(dict(manifest, requires={"builds": ["8C148"]})).encode())],
                   str(tmp / "t.itpack"))
        (vol / "System/Library/LaunchDaemons/j.plist").write_bytes(b"baked job")
        made, rec = mkpkg.seed(str(vol), str(tmp / "t.itpack"), gles=False)
        assert rec["seed"] == 7 and rec["family"] == "f" and rec["hooks"] == [] and rec["jobs"] == ["j.plist"]
        assert not os.path.lexists(vol / "System/Library/LaunchDaemons/j.plist")    # the package's job
        assert dev.hook(MBX) == b"stock mbx"      # no shim installed: no GL hook; typein's target is absent
        assert "hooks/MBXGLEngine" not in (vol / "usr/local/lighttouch/pkgs/7/offer").read_text()
        shutil.rmtree(vol / "usr")
        (vol / "System/Library/LaunchDaemons/com.qemu.it-boot.plist").unlink()
        dev.rel("sys" + TYPEIN).parent.mkdir(parents=True)
        dev.rel("sys" + TYPEIN).write_bytes(b"old typein")
        made, rec = mkpkg.seed(str(vol), str(tmp / "t.itpack"), gles=True)
        assert rec["hooks"] == [MBX, TYPEIN] and "usr/local/lighttouch/current" in made
        # .baked keeps what the volume had; the target gets the package's bytes
        assert dev.hook(MBX) == b"shim" and dev.hook(MBX + ".baked") == b"stock mbx"
        assert dev.hook(TYPEIN) == b"t" and dev.hook(TYPEIN + ".baked") == b"old typein"
        assert (vol / "usr/local/bin/it_boot").read_bytes() == b"loader"
        os.symlink("sys/usr/local/lighttouch", dev.d / "root")
        dev.offer((vol / "usr/local/lighttouch/pkgs/7/offer").read_text())
        assert dev.boot() == 0 and dev.current() == 7 and "hook" not in dev.stderr, dev.stderr
        assert dev.launchctl() == ["load %s/root/pkgs/7/jobs/j.plist" % dev.d], dev.launchctl()
        assert dev.reports()[0][:2] == ["7", "0"] and "seed 7" in dev.reports()[0][2]
        assert dev.boot(silent=1) == 0 and dev.launchctl() == ["load %s/root/pkgs/7/jobs/j.plist" % dev.d]

    print("PASS: silent host, install, good/bad verdicts, no-verdict retries, safe mode, .baked hooks, "
          "bad hash, size-only fallback, torn installs, wrong build, malformed offers, the preparers' seed")


if __name__ == "__main__":
    sys.exit(main())
