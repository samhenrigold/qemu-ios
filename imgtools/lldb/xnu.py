"""lldb commands for a guest kernel behind QEMU's gdbstub (docs/guest-debug.md).

    (lldb) target create --arch armv6-apple-ios KERNEL.macho      # armv7-apple-ios for the iPad
    (lldb) gdb-remote 127.0.0.1:PORT                               # QEMU -gdb tcp:127.0.0.1:PORT
    (lldb) command script import imgtools/lldb/xnu.py
    (lldb) xnu-procs                    every process: pid, name, proc, task, pmap, its TTBR0
    (lldb) xnu-current                  the process the CPU is in (kernel or user mode)
    (lldb) xnu-images --sysroot DIR [--pid N]
                                        a process's dyld images (the current one by default), added
                                        to lldb at their load addresses from DIR, a host copy of the
                                        guest's root filesystem (userland symbols)
    (lldb) xnu-break NAME ADDR|SYMBOL   a breakpoint that stops only in the process called NAME
    (lldb) xnu-offsets [key=value ...]  the struct offsets in use; set them by hand for a stripped kernel

Struct offsets come from the kernel itself, not from tables: the accessors xnu exports
(get_threadtask, get_bsdtask_info, get_task_map, get_task_pmap, proc_pid) are one or two loads each
and are read out of their disassembly; p_comm is found by its "kernel_task" in kernproc, the
pmap's translation table by TTBR1 in kernel_pmap. A stripped kernel (3.2+) has no accessor names:
pass the offsets with xnu-offsets (docs/guest-debug.md has the ones measured).

Userland stops are attributed by TTBR0: with TTBCR.N > 0 the low addresses translate through the
current process's own table, so the process whose pmap holds that table is the one running.
"""

import re
import struct

import lldb

OFF = {}          # thread_task, task_proc, task_map, map_pmap, proc_pid, proc_comm, pmap_ttb_pa
SYMS = {}
TTB_CACHE = {}  # TTBR0 (masked) -> (proc, pid, name)


def _target():
    return lldb.debugger.GetSelectedTarget()


def _sym(name):
    if name in SYMS:
        return SYMS[name]
    t = _target()
    k = t.GetModuleAtIndex(0)
    if k.IsValid() and k.GetObjectFileHeaderAddress().GetLoadAddress(t) == lldb.LLDB_INVALID_ADDRESS:
        # lldb finds the kernel itself only when it stops in it; these kernels are never slid
        t.SetModuleLoadAddress(k, 0)
    ctx = t.FindSymbols(name)
    addr = None
    for i in range(ctx.GetSize()):
        a = ctx.GetContextAtIndex(i).GetSymbol().GetStartAddress().GetLoadAddress(_target())
        if a != lldb.LLDB_INVALID_ADDRESS:
            addr = a
            break
    if addr is not None:          # not cached until the kernel's load address is known
        SYMS[name] = addr
    return addr


def _packet(text):
    ret = lldb.SBCommandReturnObject()
    lldb.debugger.GetCommandInterpreter().HandleCommand('process plugin packet send "%s"' % text, ret)
    m = re.search(r"response: (\S*)", ret.GetOutput() or "")
    return m.group(1) if m else ""


def _phys(pa, n):
    """Physical memory through QEMU's gdbstub (qqemu.PhyMemMode), lldb's cache bypassed."""
    _packet("Qqemu.PhyMemMode:1")
    try:
        out = b""
        while len(out) < n:
            k = min(0x400, n - len(out))
            r = _packet("m%x,%x" % (pa + len(out), k))
            if len(r) != 2 * k:
                return None
            out += bytes.fromhex(r)
        return out
    finally:
        _packet("Qqemu.PhyMemMode:0")


def vtop(va, ttb=None):
    """ARMv6/v7 short-descriptor walk. ttb: a process's table (pmap), else TTBR0/TTBR1 by TTBCR.N."""
    if ttb is None:
        n = (_reg("TTBCR") or 0) & 7
        low = n and va < (1 << (32 - n))
        ttb = (_reg("TTBR0_EL1") if (low or not n) else _reg("TTBR1_EL1")) or 0
    l1 = _phys((ttb & ~0x7F) + (va >> 20) * 4, 4)
    if not l1:
        return None
    e = struct.unpack("<I", l1)[0]
    if e & 3 == 2:
        return (e & 0xFF000000) | (va & 0xFFFFFF) if e & (1 << 18) else (e & 0xFFF00000) | (va & 0xFFFFF)
    if e & 3 != 1:
        return None
    l2 = _phys((e & ~0x3FF) + ((va >> 12) & 0xFF) * 4, 4)
    e2 = struct.unpack("<I", l2)[0] if l2 else 0
    if e2 & 3 == 1:
        return (e2 & 0xFFFF0000) | (va & 0xFFFF)
    if e2 & 2:
        return (e2 & 0xFFFFF000) | (va & 0xFFF)
    return None


def _mem(addr, n, ttb=None):
    """Virtual memory: lldb's read, or a page walk when the gdbstub's own translation refuses (kernel
    memory from a user-mode stop: QEMU translates debug reads with the current privilege) or when
    another process's table is named."""
    if ttb is None:
        err = lldb.SBError()
        b = _target().GetProcess().ReadMemory(addr, n, err)
        if err.Success():
            return b
    out = b""
    while len(out) < n:
        va = addr + len(out)
        k = min(n - len(out), 0x1000 - (va & 0xFFF))
        pa = vtop(va, ttb)
        b = _phys(pa, k) if pa is not None else None
        if b is None:
            return None
        out += b
    return out


def _u32(addr, ttb=None):
    b = _mem(addr, 4, ttb) if addr else None
    return struct.unpack("<I", b)[0] if b else None


def _cstr(addr, n=32, ttb=None):
    b = _mem(addr, n, ttb)
    return b.split(b"\0")[0].decode("latin1") if b else "?"


def _reg(name):
    fr = _target().GetProcess().GetSelectedThread().GetFrameAtIndex(0)
    v = fr.FindRegister(name)
    if not v.IsValid():
        return None
    return v.GetValueAsUnsigned()


def _loads(fn, limit=12):
    """The offsets an accessor loads through, in order: `ldr rD, [rB, #imm]` and `ldr rD, [rB, rI]`
    with rI built by movs/lsls (the 1.x Thumb idiom), until its return."""
    addr = _sym(fn)
    if addr is None:
        return []
    t = _target()
    code = _mem(addr & ~1, limit * 4)
    if not code:
        return []
    insts = t.GetInstructions(lldb.SBAddress(addr, t), code)
    regs, out = {}, []
    for i in range(insts.GetSize()):
        ins = insts.GetInstructionAtIndex(i)
        m, ops = ins.GetMnemonic(t), ins.GetOperands(t).replace(" ", "")
        if m.startswith("bx") or m.startswith("pop"):
            break
        if m in ("movs", "mov") and re.fullmatch(r"r\d+,#0x[0-9a-f]+|r\d+,#\d+", ops):
            r, v = ops.split(",#")
            regs[r] = int(v, 0)
        elif m in ("lsls", "lsl") and re.fullmatch(r"(r\d+),(r\d+),#(\w+)", ops):
            d, s, n = re.fullmatch(r"(r\d+),(r\d+),#(\w+)", ops).groups()
            if s in regs:
                regs[d] = regs[s] << int(n, 0)
        elif m in ("ldr", "ldr.w"):
            mm = re.fullmatch(r"r\d+,\[r\d+(?:,#(-?\w+))?\]", ops)
            mi = re.fullmatch(r"r\d+,\[r\d+,(r\d+)\]", ops)
            if mm:
                out.append(int(mm.group(1), 0) if mm.group(1) else 0)
            elif mi and mi.group(1) in regs:
                out.append(regs[mi.group(1)])
    return out


def _current_thread_reg():
    """1.x keeps the current thread in r9 (current_thread is `mov r0, r9`); later kernels use TPIDRPRW."""
    addr = _sym("current_thread")
    if addr is not None:
        t = _target()
        ins = t.GetInstructions(lldb.SBAddress(addr, t), _mem(addr & ~1, 4) or b"").GetInstructionAtIndex(0)
        if ins.GetMnemonic(t) == "mov" and ins.GetOperands(t).replace(" ", "").endswith("r9"):
            return "r9"
    return "TPIDRPRW"


def _find_word(base, n, want, mask=0xFFFFFFFF):
    b = _mem(base, n)
    if not b:
        return None
    for o in range(0, n - 3, 4):
        if struct.unpack_from("<I", b, o)[0] & mask == want & mask:
            return o
    return None


def _calibrate():
    if OFF.get("_done"):
        return
    # (key, accessor, which load); later rows are fallbacks for kernels that export fewer names (7B500
    # exports current_task but not get_threadtask, and no get_task_pmap)
    for key, fn, idx in (("thread_task", "get_threadtask", 0), ("thread_task", "current_task", 0),
                         ("task_proc", "get_bsdtask_info", 0), ("task_map", "get_task_map", 0),
                         ("map_pmap", "get_task_pmap", 1), ("proc_pid", "proc_pid", 0)):
        if key not in OFF:
            l = _loads(fn)
            if len(l) > idx:
                OFF[key] = l[idx]
    kt, km = _sym("kernel_task"), _sym("kernel_pmap")
    if "map_pmap" not in OFF and "task_map" in OFF and kt is not None and km is not None:
        # the kernel's own vm_map points at the kernel pmap
        kmap = _u32(_u32(kt) + OFF["task_map"])
        o = _find_word(kmap, 0x80, _u32(km)) if kmap else None
        if o is not None:
            OFF["map_pmap"] = o
    kp = _sym("kernproc")
    if "proc_comm" not in OFF and kp is not None:
        p = _u32(kp)
        b = _mem(p, 0x400) if p else None
        if b and b"kernel_task\0" in b:
            OFF["proc_comm"] = b.index(b"kernel_task\0")
    ttbr1 = _reg("TTBR1_EL1") or _reg("TTBR1")
    if "pmap_ttb_pa" not in OFF and km is not None and ttbr1:
        # kernel_pmap is a pointer to the structure (3.x) or, possibly, the structure itself: the pointer
        # first, since the words around a pointer variable can match by accident (3.1.3: +0x28)
        for base in (_u32(km), km):
            o = _find_word(base, 0x40, ttbr1, 0xFFFFC000) if base else None
            if o is not None:
                OFF["pmap_ttb_pa"] = o
                break
    OFF["current"] = _current_thread_reg()
    OFF["_done"] = all(k in OFF for k in ("thread_task", "task_proc", "proc_pid", "proc_comm"))


def procs():
    """[(proc, pid, name, task, pmap, ttb_pa)] from allproc (p_list.le_next is the proc's first word)."""
    _calibrate()
    head = _allproc()
    if head is None or not OFF.get("_done"):
        return []
    out, p, seen = [], _u32(head), set()
    task_off = _task_of_proc_offset()
    while p and p not in seen and len(out) < 512:
        seen.add(p)
        task = _u32(p + task_off) if task_off is not None else None
        pmap = ttb = None
        if task and "task_map" in OFF and "map_pmap" in OFF:
            m = _u32(task + OFF["task_map"])
            pmap = _u32(m + OFF["map_pmap"]) if m else None
            if pmap and "pmap_ttb_pa" in OFF:
                ttb = _u32(pmap + OFF["pmap_ttb_pa"])
        out.append((p, _u32(p + OFF["proc_pid"]), _cstr(p + OFF["proc_comm"], 17), task, pmap, ttb))
        p = _u32(p)
    return out


def _allproc():
    """&allproc, or (not exported, 7B500) found from kernproc: kernproc is the list's last entry, and
    p_list.le_prev leads back through each proc (le_next is a proc's first word) to the list head, the
    first link that lies in the kernel's own data rather than in a proc."""
    head = _sym("allproc")
    if head is not None or "allproc" in OFF:
        return head if head is not None else OFF["allproc"]
    kp = _sym("kernproc")
    if kp is None:
        return None
    t = _target()
    data = [t.GetModuleAtIndex(0).FindSection("__DATA")]
    lo = data[0].GetLoadAddress(t) if data[0].IsValid() else None
    hi = lo + data[0].GetByteSize() if lo is not None else None
    start = p = _u32(kp)
    for _ in range(512):
        q = _u32(p + 4) if p else None
        if not q or _u32(q) != p:
            return None
        if lo is not None and lo <= q < hi and q != start:
            OFF["allproc"] = q
            return q
        p = q
    return None


def _task_of_proc_offset():
    """proc->task: the word in kernproc that points at kernel_task (found, not assumed)."""
    if "proc_task" in OFF:
        return OFF["proc_task"]
    kp, kt = _sym("kernproc"), _sym("kernel_task")
    if kp is None or kt is None:
        return None
    o = _find_word(_u32(kp), 0x400, _u32(kt))
    if o is not None:
        OFF["proc_task"] = o
    return o


def current():
    """(proc, pid, name, how) for the process the CPU is running."""
    _calibrate()
    if not OFF.get("_done"):
        return None, None, None, "offsets unknown (xnu-offsets)"
    cpsr = _reg("cpsr") or 0
    user = (cpsr & 0x1F) == 0x10
    if not user:
        th = _reg(OFF["current"]) if OFF["current"] == "r9" else (_reg("TPIDRPRW") or 0) & ~0x3
        task = _u32(th + OFF["thread_task"]) if th else None
        p = _u32(task + OFF["task_proc"]) if task else None
        if p:
            return p, _u32(p + OFF["proc_pid"]), _cstr(p + OFF["proc_comm"], 17), "thread 0x%x" % th
    ttbr0 = (_reg("TTBR0_EL1") or _reg("TTBR0") or 0) & 0xFFFFF000
    # A filtered breakpoint in a shared library is hit by every process, so the table->process map is
    # cached and the process list (dozens of physical reads) walked again only for a table not seen yet.
    # A recycled table is caught by the pid read back from the cached proc.
    hit = TTB_CACHE.get(ttbr0)
    if hit and _u32(hit[0] + OFF["proc_pid"]) == hit[1]:
        return hit + ("TTBR0 0x%x" % ttbr0,)
    for p, pid, name, task, pmap, ttb in procs():
        if ttb is not None and pid != 0:
            TTB_CACHE[ttb & 0xFFFFF000] = (p, pid, name)
    hit = TTB_CACHE.get(ttbr0)
    if hit:
        return hit + ("TTBR0 0x%x" % ttbr0,)
    return None, None, None, "unknown (TTBR0 0x%x)" % ttbr0


def _out(result, s):
    result.AppendMessage(s)


def cmd_procs(debugger, command, ctx, result, _):
    rows = procs()
    if not rows:
        result.SetError("no allproc / offsets (stripped kernel? see xnu-offsets)")
        return
    _out(result, "%5s %-17s %-10s %-10s %-10s %s" % ("pid", "name", "proc", "task", "pmap", "ttb"))
    for p, pid, name, task, pmap, ttb in rows:
        _out(result, "%5d %-17s 0x%08x 0x%08x 0x%08x %s" % (pid or 0, name, p, task or 0, pmap or 0,
                                                        "0x%08x" % ttb if ttb else "-"))


def cmd_current(debugger, command, ctx, result, _):
    p, pid, name, how = current()
    mode = "user" if ((_reg("cpsr") or 0) & 0x1F) == 0x10 else "kernel"
    if p is None:
        result.SetError("cannot tell: %s" % how)
        return
    _out(result, "%s mode, pid %d %s (proc 0x%x, %s)" % (mode, pid, name, p, how))


def _parse_symtab(path):
    """{name: value} from a Mach-O's LC_SYMTAB (1.x binaries carry LC_PREBIND_CKSUM, which llvm's
    tools refuse; this does not care)."""
    b = open(path, "rb").read()
    if struct.unpack_from("<I", b, 0)[0] != 0xFEEDFACE:
        return {}
    ncmds = struct.unpack_from("<I", b, 16)[0]
    off, syms = 28, {}
    for _ in range(ncmds):
        cmd, sz = struct.unpack_from("<II", b, off)
        if cmd == 2:
            symoff, nsyms, stroff, _s = struct.unpack_from("<IIII", b, off + 8)
            for i in range(nsyms):
                strx, typ, sect, desc, val = struct.unpack_from("<IBBHI", b, symoff + 12 * i)
                if typ & 0x0E == 0x0E:
                    e = b.index(b"\0", stroff + strx)
                    syms[b[stroff + strx:e].decode("latin1")] = val
        off += sz
    return syms


def _text_vmaddr(path):
    b = open(path, "rb").read(0x4000)
    ncmds, off = struct.unpack_from("<I", b, 16)[0], 28
    for _ in range(ncmds):
        cmd, sz = struct.unpack_from("<II", b, off)
        if cmd == 1 and b[off + 8:off + 24].split(b"\0")[0] == b"__TEXT":
            return struct.unpack_from("<I", b, off + 24)[0]
        off += sz
    return None


def images(sysroot, ttb=None):
    """[(load address, path)] from dyld's dyld_all_image_infos in the current address space.
    1.x-2.x dyld sits unslid at its preferred address, so the symbol's value is where it is."""
    import os
    syms = _parse_symtab(os.path.join(sysroot, "usr/lib/dyld"))
    a = syms.get("_dyld_all_image_infos")
    if a is None:
        return None
    count, arr = _u32(a + 4, ttb), _u32(a + 8, ttb)
    out = []
    for i in range(min(count or 0, 1024)):
        load, pathp = _u32(arr + 12 * i, ttb), _u32(arr + 12 * i + 4, ttb)
        out.append((load, _cstr(pathp, 256, ttb) if pathp else "?"))
    return out


def cmd_images(debugger, command, ctx, result, _):
    import os
    import shlex
    args = shlex.split(command)
    sysroot = args[args.index("--sysroot") + 1] if "--sysroot" in args else None
    if sysroot is None:
        result.SetError("usage: xnu-images --sysroot DIR [--pid N] (DIR: a host copy of the guest's root)")
        return
    ttb = None
    if "--pid" in args:     # another process's images, read through its own translation table
        pid = int(args[args.index("--pid") + 1])
        ttb = next((r[5] for r in procs() if r[1] == pid), None)
        if ttb is None:
            result.SetError("no process %d with a known translation table" % pid)
            return
    imgs = images(sysroot, ttb)
    if imgs is None:
        result.SetError("no _dyld_all_image_infos in %s/usr/lib/dyld" % sysroot)
        return
    t = _target()
    have = {t.GetModuleAtIndex(i).GetFileSpec().fullpath for i in range(t.GetNumModules())}
    for load, path in imgs:
        host = os.path.join(sysroot, path.lstrip("/"))
        note = ""
        if os.path.exists(host) and host not in have:
            vm = _text_vmaddr(host)
            m = t.AddModule(host, None, None)
            if m.IsValid() and vm is not None:
                for i in range(m.GetNumSections()):     # per segment: a slide can be negative
                    sec = m.GetSectionAtIndex(i)
                    if sec.GetName() != "__PAGEZERO":
                        t.SetSectionLoadAddress(sec, (sec.GetFileAddress() + load - vm) & 0xFFFFFFFF)
                note = "  (added, slide %#x)" % (load - vm)
        elif not os.path.exists(host):
            note = "  (not in sysroot)"
        _out(result, "0x%08x %s%s" % (load, path, note))


def _break_callback(frame, bp_loc, extra, internal_dict):
    want = extra.GetValueForKey("process").GetStringValue(64)
    p, pid, name, how = current()
    return name == want


def cmd_break(debugger, command, ctx, result, _):
    import shlex
    args = shlex.split(command)
    if len(args) != 2:
        result.SetError("usage: xnu-break PROCESS-NAME ADDRESS|SYMBOL")
        return
    t = _target()
    try:
        bp = t.BreakpointCreateByAddress(int(args[1], 0))
    except ValueError:
        bp = t.BreakpointCreateByName(args[1])
    extra = lldb.SBStructuredData()
    extra.SetFromJSON('{"process": "%s"}' % args[0])
    bp.SetScriptCallbackFunction("xnu._break_callback", extra)
    _out(result, "breakpoint %d: %s, stops only in %s (%d locations)" % (bp.GetID(), args[1], args[0],
                                                                         bp.GetNumLocations()))


def cmd_offsets(debugger, command, ctx, result, _):
    for kv in command.split():
        k, v = kv.split("=")
        OFF[k] = int(v, 0) if k != "current" else v
    OFF.pop("_done", None)
    _calibrate()
    _task_of_proc_offset()
    _out(result, " ".join("%s=%s" % (k, hex(v) if isinstance(v, int) else v)
                          for k, v in sorted(OFF.items()) if not k.startswith("_")))


def __lldb_init_module(debugger, internal_dict):
    for name, fn in (("xnu-procs", "cmd_procs"), ("xnu-current", "cmd_current"), ("xnu-images", "cmd_images"),
                     ("xnu-break", "cmd_break"), ("xnu-offsets", "cmd_offsets")):
        debugger.HandleCommand("command script add -o -f xnu.%s %s" % (fn, name))
