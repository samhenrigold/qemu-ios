# N72 real-ROM DFU, stock iBSS, and ECID fuses

October 1 continuation, on the isolated fidelity candidate. No physical USB
access or firmware instruction changes are involved.

A private all-FF NOR, real `bootrom_240_4`, source NAND opened with a private
overlay, and `usb-attached=on` make stock SecureROM enumerate PID `1227`.
The existing `usb_recovery_bridge.py` and private libirecovery transport are
sufficient: unmodified host `irecovery --query` identifies CPID 8720, n72ap,
iPod2,1, DFU and SRTG iBoot-240.4. Stock `irecovery -f` uploads the **unaltered**
5F138 `Firmware/dfu/iBSS.n72ap.RELEASE.dfu`, SHA-256
`7e8a98711a6b5650bed89ba38c71b820f4eb2423e0cd72f54b33d075e5127f16`.
The guest transitions to PID `1281`/Recovery; its console identifies Apple
`iBSS for n72ap`, BUILD_TAG `iBoot-385.22`, RELEASE. This image has no KBAG.

The first private validator incorrectly required SRTG in recovery's descriptor.
That field is absent there. The retained corrected run judges the actual PID,
mode and console, rather than inventing a descriptor field.

## One measured hardware gap

The original model returned zero at both CHIPID +0xC and +0x10, so stock ROM
and iBSS advertised ECID zero. `idevicerestore -i 0x0` rejected its CLI input;
without the selector, it did not discover this zero-ECID target. Stock ROM's
ECID routine reads the two fuse words and rearranges their bits as follows:

```
ECID[41:21] = word3[20:0]
ECID[20:16] = word3[25:21]
ECID[15:8]  = word4[9:2]
ECID[7:2]   = word3[31:26]
ECID[1:0]   = word4[1:0]
```

The model now accepts read-only `word3` and `word4` properties, leaving their
zero defaults and existing production/security/chip-revision fuses unchanged.
Fuse values survive reset; guest stores cannot change them. This supplies the
hardware input that guest code reads, not a fabricated USB identity. Configure
these dotted device types with QEMU's structured syntax:

```
-global driver=ipodtouch.chipid,property=word3,value=0xd6e54321
-global driver=ipodtouch.chipid,property=word4,value=0x2a7
```

Stock ROM then advertises ECID `0x000000a86437a9d7`, exactly the decoded fuse
value. Stock `idevicerestore -i 0xa86437a9d7` discovers DFU and identifies
n72ap/iPod2,1. Its next stop with the untouched 2.1.1 IPSW is **host firmware
suitability validation** after reading Restore.plist, before any restore
ramdisk or flash writes. Do not describe this as a NAND or USB failure or a
completed restore. The same nonzero identity is checked again across the stock
iBSS upload/re-enumeration in a separate private run.

Two production-board qtests cover unchanged defaults and programmed read-only
fuses across reset. The full default iPod regression must pass after this
single hardware change. Both test inputs and outputs are retained, including
the first qtest's incorrect shorthand global-property syntax.

## Boundaries

This establishes DFU/recovery and host target selection on 5F138. Automatic
per-device N72 ECID provisioning, host support for this older Restore.plist,
iBEC/restore-ramdisk boot, a truly blank physical NAND restore, and cold stock
GPU execution remain open. Existing generated devices retain their old fuse
inputs. The existing K48 restore runner is the reusable transport reference;
this investigation does not introduce another production restore engine.

Evidence roots:
`/private/tmp/ltm-n72-realrom-dfu`,
`/private/tmp/ltm-n72-realrom-ibss2`,
`/private/tmp/ltm-n72-realrom-stock-restore-fuses`,
`/private/tmp/ltm-n72-realrom-ibss-fuses`,
`/private/tmp/ltm-n72-ecid-qtest2.log`,
`/private/tmp/ltm-n72-ecid-models.log`, and
`/private/tmp/ltm-n72-ecid-default-regress.log`.
