"""Answer the emulator's request to bless its JIT buffer, from LLDB.

From iOS 26, TXM will not let a process mark its own pages executable -- only
an attached debugger can, because only the debugger holds
com.apple.private.cs.debugger. QEMU therefore maps its code buffer executable
up front and then traps with `brk #0x69`, x0 = address, x1 = length, asking us
to touch the range. That write is what makes the kernel record the mapping as
a legitimate debug region; without it, the first jump into generated code dies
with EXC_BAD_ACCESS (code=50).

Load it from the project's lldbinit:

    command script import ~/Developer/qemu-ios/contrib/ios-app/lldb_jit.py

The module name has to stay import-safe (underscore, not hyphen) or the
stop-hook registration below cannot name it.
"""

import lldb

BRK_JIT = 0x69

# AArch64 BRK #imm16 is 1101 0100 001 imm16 00000.
BRK_OPCODE_MASK = 0xFFE0001F
BRK_OPCODE = 0xD4200000


def _is_jit_brk(process, pc):
    """True if the instruction at pc is exactly `brk #0x69`."""
    err = lldb.SBError()
    raw = process.ReadMemory(pc, 4, err)
    if not err.Success() or len(raw) != 4:
        return False
    word = int.from_bytes(raw, "little")
    if (word & BRK_OPCODE_MASK) != BRK_OPCODE:
        return False
    return ((word >> 5) & 0xFFFF) == BRK_JIT


def _bless(frame, stream):
    process = frame.GetThread().GetProcess()
    addr = frame.FindRegister("x0").GetValueAsUnsigned()
    length = frame.FindRegister("x1").GetValueAsUnsigned()

    # Reading and writing the range back unchanged is the entire point: the
    # contents do not matter, the fact that the DEBUGGER wrote them does.
    # Chunked so a large buffer is not one enormous packet.
    err = lldb.SBError()
    chunk = 1 << 20
    done = 0
    while done < length:
        n = min(chunk, length - done)
        data = process.ReadMemory(addr + done, n, err)
        if not err.Success():
            stream.Print(f"[jit] read at {addr + done:#x} failed: {err}\n")
            return False
        process.WriteMemory(addr + done, data, err)
        if not err.Success():
            stream.Print(f"[jit] write at {addr + done:#x} failed: {err}\n")
            return False
        done += n

    # Step over the trap, or we stop on it again immediately.
    frame.SetPC(frame.GetPC() + 4)
    stream.Print(f"[jit] blessed {length >> 20} MiB at {addr:#x}\n")
    return True


class JITRegionHook:
    """Stop hook: handle_stop returning False lets the process run on."""

    def __init__(self, target, extra_args, internal_dict):
        pass

    def handle_stop(self, exe_ctx, stream):
        thread = exe_ctx.GetThread()
        if not thread or not thread.IsValid():
            return True
        if thread.GetStopReason() != lldb.eStopReasonException:
            return True

        frame = thread.GetFrameAtIndex(0)
        if not frame or not frame.IsValid():
            return True
        if not _is_jit_brk(exe_ctx.GetProcess(), frame.GetPC()):
            return True                 # someone else's exception; really stop

        return not _bless(frame, stream)


def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand(
        "target stop-hook add -P {}.JITRegionHook".format(__name__)
    )
    print("[jit] JIT-region stop hook installed")
