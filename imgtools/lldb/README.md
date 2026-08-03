# lldb against the emulated iPod touch 2G (iOS 3.1.3, 7E18)

**Working.** A modern host `lldb` attaches to a process running inside the
emulated guest, reads registers, resolves symbols out of the dyld shared cache,
sets a breakpoint and hits it.  Measured 2026-08-03 with `lldb-2103.0.25.1`.

    (lldb) breakpoint set --address 0x2631a
    Breakpoint 1: where = bash`... + 370, address = 0x0002631a
    (lldb) continue
    Process 628 stopped
    * thread #1, stop reason = EXC_BREAKPOINT (code=EXC_ARM_BREAKPOINT, subcode=0x0)
           frame #0: 0x0002631a bash`... + 370

## The pieces

1. **A 3.1.3-matched armv6 `debugserver`.**  `Packages/DeveloperDiskImage.pkg`
   on the iPhone SDK 3.1.3 DMG carries a `DeviceSupport/3.1.3 (7E18)/
   DeveloperDiskImage.dmg` -- an exact-build match, not the 2.1 one the old
   notes describe.  `DeveloperDiskImage-3.1.3-7E18.tgz` here is its `usr` and
   `Library` trees, ready to untar into the guest's `/Developer`.
   `debugserver` needs `/Developer/usr/lib/libdebugnub.dylib` and
   `/Developer/Library/PrivateFrameworks/ARMDisassembler.framework`, so untar
   the whole thing -- it is only 14 MB.

2. **lockdownd spawns it; no image-mount handshake.**  lockdownd reads
   `/Developer/Library/Lockdown/ServiceAgents/*.plist` at startup, so
   `killall lockdownd` after the untar is enough to publish
   `com.apple.debugserver`.  **Replace the shipped agent plist with
   `com.apple.debugserver.root.plist`** (identical but `UserName` `root`
   instead of `mobile`) or you can only attach to `mobile`-owned processes.

3. **`lldb_shim.py`** -- a GDB-remote shim.  Modern lldb cannot drive a 2009
   debugserver directly: see "What the shim fixes" below.

## Recipe

    # emulator + usbmuxd, then over SSH (see ../ssh/README.md):
    scp DeveloperDiskImage-3.1.3-7E18.tgz root@device:/tmp/
    ssh root@device 'cd /Developer && tar xzf /tmp/DeveloperDiskImage-3.1.3-7E18.tgz'
    scp com.apple.debugserver.root.plist \
        root@device:/Developer/Library/Lockdown/ServiceAgents/com.apple.debugserver.plist
    ssh root@device 'killall lockdownd'

    # host side, three processes:
    USBMUXD_SOCKET_ADDRESS=$SOCK idevicedebugserverproxy $DSPORT &
    python3 lldb_shim.py --device-port $DSPORT --listen $SHIMPORT --attach-pid $PID &
    lldb -o "target create --arch armv6 <a host copy of the guest binary>" \
         -o "gdb-remote 127.0.0.1:$SHIMPORT"

`--attach-pid` matters: the shim attaches before lldb connects, so lldb's
opening `?`/`qProcessInfo` find a live stopped process.  Without it lldb
reports "Process 1 was reported ... but no stop reply packet was received".

Optional but strongly recommended -- give lldb the shared cache on disk, or it
reads ~96 MB of it out of guest memory one 512-byte `m` packet at a time
(measured: 37,000 packets, about five minutes, and long enough that SpringBoard
is killed by the watchdog while stopped):

    ~/Library/Developer/Xcode/iOS DeviceSupport/3.1.3 (7E18)/Symbols/
        System/Library/Caches/com.apple.dyld/dyld_shared_cache_armv6
        usr/lib/dyld

(both copied straight out of a mounted `rootfs313.dmg`).

## What the shim fixes -- all measured

* The device answers an **empty packet** to `qSupported`, `qHostInfo`,
  `qProcessInfo`, `qRegisterInfo`, `vCont?`, `qVAttachOrWaitSupported`,
  `QThreadSuffixSupported`, `jThreadsInfo`.  lldb is left with no arch,
  pointer size, byte order or register layout.  The shim answers them
  (armv6 / ios / little / ptrsize 4).
* `qfThreadInfo` answers **`OK`**, not a thread list.  lldb cannot parse that
  and hangs.  The shim answers `m<tid>` using the tid from the stop reply.
* **Register layout.**  `g` is 300 bytes in the classic *GDB* ARM numbering,
  not lldb's:  r0-r15 at 0, f0-f7 (12 bytes each, legacy FPA) at 64, fps at
  160, cpsr at 164, s0-s31 at 168, fpscr at 296.  The stop reply's register
  numbers (`00`-`0f`, `19`) confirm it.  The shim synthesises `qRegisterInfo`
  from exactly that layout, so no target-definition file is needed.
* **Do not put the device side in no-ack mode.**  It replies `OK` to
  `QStartNoAckMode` and then never answers another packet -- the shim acks the
  device always, and only speaks no-ack to lldb.
* `Hc-1` ("all threads") is rewritten to `Hc<tid>`.

## Traps

* **debugserver serves exactly one connection and then exits**, killing the
  process it was attached to.  Always send `D` before dropping the link, or
  SpringBoard (and anything else you attached to) is killed and respawns.
* A **failed** `vAttach` also ends the session -- and the pid you looked up
  earlier is usually stale by then, because the previous session killed it.
  Re-read the pid immediately before each attach.
* `launchctl list` is the only process listing on the guest; there is no `ps`.
* Attaching to SpringBoard works, but leaving it stopped for minutes gets it
  killed.  Fix the shared-cache path above before trying to work in it.
* The MAC gate is already open: `IT_AMFI_ALLOW_TASKPORT=1` patches
  `mac_proc_check_get_task{,_name}`.  Without it `vAttach` cannot work.
  The old `get-task-allow` story in the notes is wrong and dead.
