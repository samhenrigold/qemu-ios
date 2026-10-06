# Apple's debugserver in the guest (from a mounted DeveloperDiskImage), driven from lldb:
#   DSPORT=<idevicedebugserverproxy port> DSPID=<pid> [DSWAKE=<file>] \
#       [DSSYSROOT=<host libraries>] \
#       lldb -b -o 'command script import dsattach.py'
# Connects, attaches to DSPID, breaks where the main thread's blocked mach_msg returns (the run loop's next wake),
# prints the backtrace, deletes the breakpoint, continues and detaches. DSWAKE: a file written after the first
# continue, for the caller to wake the run loop (a touch). Lines are prefixed "ds:" for the caller to parse.
import os, re, time, lldb


def _wait(p, states, secs):
    end = time.time() + secs
    while time.time() < end and p.GetState() not in states:
        time.sleep(0.2)
    return p.GetState()


def __lldb_init_module(dbg, _):
    say = lambda *a: print("ds:", *a, flush=True)
    state = lambda p: lldb.SBDebugger.StateAsCString(p.GetState())
    ci, r = dbg.GetCommandInterpreter(), lldb.SBCommandReturnObject()
    # Libraries: from DSSYSROOT (the build's shared cache through dsc_extract.py, plus usr/lib/dyld), through target
    # search paths and matched by UUID (`platform select remote-ios --sysroot` alone left lldb reading them from
    # memory). The app itself, and without DSSYSROOT everything, is read from guest memory over USB: then only what
    # a backtrace needs (about 100 s to attach instead of about 12 s).
    sysroot = os.environ.get("DSSYSROOT", "").rstrip("/")
    if not sysroot:
        ci.HandleCommand("settings set target.memory-module-load-level minimal", r)
    dbg.SetAsync(True)
    if os.environ.get("DSLOG"):
        ci.HandleCommand("log enable -f %s gdb-remote packets" % os.environ["DSLOG"], r)
    target = dbg.CreateTargetWithFileAndTargetTriple("", "armv7-apple-ios")
    for top in ("/System", "/usr") if sysroot else ():
        ci.HandleCommand("target modules search-paths add %s %s%s" % (top, sysroot, top), r)
    err = lldb.SBError()
    target.ConnectRemote(dbg.GetListener(), "connect://127.0.0.1:" + os.environ["DSPORT"], "gdb-remote", err)
    say("connect", "ok" if err.Success() else err.GetCString())
    if not err.Success():
        return
    time.sleep(2)
    dbg.SetAsync(False)
    # The 4.x/5.x debugservers do not know qRegisterInfo (an empty reply; lldb then has no registers): give lldb
    # their layout. 6.x answers it, 7.x answers an error until it has a process.
    ci.HandleCommand("process plugin packet send qRegisterInfo0", r)
    say("qRegisterInfo0", (r.GetOutput() or "").strip().replace("\n", " | "))
    if re.search(r"response: *$", r.GetOutput() or "", re.M):
        ci.HandleCommand("settings set plugin.process.gdb-remote.target-definition-file " +
                         os.path.join(os.path.dirname(os.path.abspath(__file__)), "arm_gdb_regs.py"), r)
        say("registers", "arm_gdb_regs.py", "ok" if r.Succeeded() else (r.GetError() or "").strip())
    # by pid: lldb resolves -n through the platform, and 4.x-7.x debugserver has no qfProcessInfo
    t0 = time.time()
    ci.HandleCommand("process attach -p " + os.environ["DSPID"], r)
    p = dbg.GetSelectedTarget().GetProcess()
    say("attach", "ok" if r.Succeeded() else (r.GetError() or "").strip(), "pid", p.GetProcessID(),
        "state", state(p), "threads", p.GetNumThreads(), "secs %.0f" % (time.time() - t0))
    if not r.Succeeded():
        return
    dbg.SetAsync(True)    # a synchronous Continue would block until the next stop
    for i in range(target.GetNumModules()):
        m = target.GetModuleAtIndex(i)
        if m.GetFileSpec().GetFilename() in ("UIKit", "CoreFoundation", "libobjc.A.dylib"):
            say("module", m.GetFileSpec().GetFilename(), "symbols", m.GetNumSymbols(), "from", m.GetFileSpec())
    for c in ("thread list", "register read pc lr sp cpsr", "bt 4"):
        ci.HandleCommand(c, r)
        say("diag", c, (r.GetOutput() or r.GetError() or "").replace("\n", " | "))
    ret = p.GetThreadAtIndex(0).GetFrameAtIndex(1).GetPC()
    bp = dbg.GetSelectedTarget().BreakpointCreateByAddress(ret)
    say("breakpoint", "0x%x" % ret, "locations", bp.GetNumLocations())
    p.Continue()
    if os.environ.get("DSWAKE"):
        time.sleep(3)
        open(os.environ["DSWAKE"], "w").write("wake")
    st = _wait(p, (lldb.eStateStopped, lldb.eStateExited), 60)
    th = p.GetSelectedThread()
    # 4.2's debugserver reports its breakpoint trap with subcode 0, which lldb does not take for a breakpoint (it
    # prints EXC_BREAKPOINT): the pc decides
    pc = th.GetFrameAtIndex(0).GetPC()
    say("stop", lldb.SBDebugger.StateAsCString(st), "at-breakpoint" if pc == ret else "pc 0x%x" % pc,
        th.GetStopDescription(100))
    for i in range(min(10, th.GetNumFrames())):
        say("frame", i, th.GetFrameAtIndex(i))
    dbg.GetSelectedTarget().BreakpointDelete(bp.GetID())
    p.Continue()
    time.sleep(3)
    say("continue", state(p))
    p.Stop()
    _wait(p, (lldb.eStateStopped, lldb.eStateExited), 30)
    e = p.Detach()
    say("detach", "ok" if e.Success() else e.GetCString(), state(p))
