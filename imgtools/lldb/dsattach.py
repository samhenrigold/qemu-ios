# Apple's debugserver in the guest (from a mounted DeveloperDiskImage), driven from lldb:
#   DSPORT=<idevicedebugserverproxy port> DSPID=<pid> [DSEXE=<host copy of the app binary>] [DSWAKE=<file>] \
#       lldb -b -o 'command script import dsattach.py'
# Connects, attaches to DSPID, breaks where the main thread's blocked mach_msg returns (the run loop's next wake),
# prints the backtrace, deletes the breakpoint, continues and detaches. DSWAKE: a file written after the first
# continue, for the caller to wake the run loop (a touch). Lines are prefixed "ds:" for the caller to parse.
import os, time, lldb


def _wait(p, states, secs):
    end = time.time() + secs
    while time.time() < end and p.GetState() not in states:
        time.sleep(0.2)
    return p.GetState()


def __lldb_init_module(dbg, _):
    say = lambda *a: print("ds:", *a, flush=True)
    state = lambda p: lldb.SBDebugger.StateAsCString(p.GetState())
    ci, r = dbg.GetCommandInterpreter(), lldb.SBCommandReturnObject()
    # Libraries come from the guest's memory over USB (no host shared cache for these builds): read only what a
    # backtrace needs.
    ci.HandleCommand("settings set target.memory-module-load-level minimal", r)
    dbg.SetAsync(True)
    target = dbg.CreateTarget(os.environ.get("DSEXE", ""))
    err = lldb.SBError()
    target.ConnectRemote(dbg.GetListener(), "connect://127.0.0.1:" + os.environ["DSPORT"], "gdb-remote", err)
    say("connect", "ok" if err.Success() else err.GetCString())
    if not err.Success():
        return
    time.sleep(2)
    dbg.SetAsync(False)
    # by pid: lldb resolves -n through the platform, and 4.x-7.x debugserver has no qfProcessInfo
    ci.HandleCommand("process attach -p " + os.environ["DSPID"], r)
    p = dbg.GetSelectedTarget().GetProcess()
    say("attach", "ok" if r.Succeeded() else (r.GetError() or "").strip(), "pid", p.GetProcessID(),
        "state", state(p), "threads", p.GetNumThreads())
    if not r.Succeeded():
        return
    ret = p.GetThreadAtIndex(0).GetFrameAtIndex(1).GetPC()
    bp = dbg.GetSelectedTarget().BreakpointCreateByAddress(ret)
    say("breakpoint", "0x%x" % ret, "locations", bp.GetNumLocations())
    p.Continue()
    if os.environ.get("DSWAKE"):
        time.sleep(3)
        open(os.environ["DSWAKE"], "w").write("wake")
    st = _wait(p, (lldb.eStateStopped, lldb.eStateExited), 60)
    th = p.GetSelectedThread()
    say("stop", lldb.SBDebugger.StateAsCString(st), th.GetStopDescription(100))
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
