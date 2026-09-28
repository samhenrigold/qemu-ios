#!/usr/bin/env python3
"""A/B app launch pass: the same IPAs on two (or more) base NANDs, in parallel.

    tests/ipod/app_ab.py --out DIR --nand old=PATH --nand new=PATH IPA...
    tests/ipod/app_ab.py --out DIR --table          # print the markdown table so far

Each (image, IPA) pair is one `regress.py --checks boot,appinstall,applaunch
--launch-stages` run on its own overlay (screens at 5/20 s, foreground + lit at
30 s, crash reports and installd's registry pulled), all started at once with
disjoint port ranges so their usbmuxd/QEMU/QMP/iproxy ports cannot collide.
A pair whose results.json already exists is skipped, so batches can be resumed.
"""
import argparse, json, os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "../.."))
sys.path.insert(0, HERE)
from regress import ipa_bundle_id  # noqa: E402


def verdict(res):
    for name in ("boot", "appinstall", "applaunch"):
        r = res.get(name)
        if not r or r.get("ok") is not True:
            return "FAIL %s: %s" % (name, (r or {}).get("detail", "not run"))
    return "PASS"


def table(out):
    rows = {}
    for img in sorted(os.listdir(out)):
        d = os.path.join(out, img)
        if not os.path.isdir(d):
            continue
        for run in sorted(os.listdir(d)):
            p = os.path.join(d, run, "results.json")
            if os.path.exists(p):
                rows.setdefault(run, {})[img] = verdict(json.load(open(p)))
    imgs = sorted({i for r in rows.values() for i in r})
    print("| app | " + " | ".join(imgs) + " |")
    print("|---|" + "---|" * len(imgs))
    for run, r in sorted(rows.items()):
        print("| %s | %s |" % (run, " | ".join(r.get(i, "-").replace("|", "/")[:160] for i in imgs)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--nand", action="append", default=[], help="name=path")
    ap.add_argument("--qemu", default=os.path.join(ROOT, "build/qemu-system-arm"))
    ap.add_argument("--deadline", type=int, default=560, help="seconds before every run is killed")
    ap.add_argument("--table", action="store_true")
    ap.add_argument("ipas", nargs="*")
    a = ap.parse_args()
    if a.table:
        return table(a.out)
    jobs = []
    for spec in a.nand:
        name, path = spec.split("=", 1)
        for ipa in a.ipas:
            tag = re.sub(r"[^A-Za-z0-9_.-]", "_", ipa_bundle_id(ipa))
            run = os.path.join(a.out, name, tag)
            if os.path.exists(os.path.join(run, "results.json")):
                continue
            k = len(jobs)
            ports = ["--qemu-port-lo", str(1600 + 10 * k), "--qemu-port-hi", str(1609 + 10 * k),
                     "--mux-port-lo", str(27400 + 10 * k), "--mux-port-hi", str(27409 + 10 * k),
                     "--qmp-port-lo", str(28200 + 10 * k), "--qmp-port-hi", str(28209 + 10 * k),
                     "--proxy-port-lo", str(28600 + 20 * k), "--proxy-port-hi", str(28619 + 20 * k)]
            os.makedirs(os.path.dirname(run), exist_ok=True)
            log = open(run + ".log", "w")
            p = subprocess.Popen([sys.executable, os.path.join(HERE, "regress.py"), "--qemu", a.qemu,
                                  "--base-nand", path, "--ipa", ipa, "--out", run,
                                  "--checks", "boot,appinstall,applaunch", "--launch-stages",
                                  "--boot-timeout", "400", "--install-timeout", "240"] + ports,
                                 stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
            jobs.append((p, name, tag))
    end = time.time() + a.deadline
    for p, name, tag in jobs:
        try:
            p.wait(timeout=max(1, end - time.time()))
        except subprocess.TimeoutExpired:
            # regress.py has no SIGTERM cleanup and starts QEMU/usbmuxd in their own
            # sessions, so kill its whole descendant tree (our own PIDs only).
            ps = subprocess.run(["ps", "-Ao", "pid=,ppid="], capture_output=True, text=True).stdout
            kids, tree = {}, [p.pid]
            for line in ps.split("\n"):
                if line.strip():
                    c, par = map(int, line.split())
                    kids.setdefault(par, []).append(c)
            for pid in tree:
                tree += kids.get(pid, [])
            for pid in reversed(tree):
                try:
                    os.kill(pid, 9)
                except OSError:
                    pass
            p.wait()
            print("TIMEOUT %s %s" % (name, tag))
    table(a.out)


if __name__ == "__main__":
    main()
