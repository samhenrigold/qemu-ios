# macOS QEMU USB transport for libirecovery

This builds a private copy of upstream libirecovery with a Unix socket transport.
Unmodified `irecovery` and `idevicerestore` executables retain upstream upload,
DFU CRC, recovery command and restore logic. The transport library is modified;
this does **not** make the guest appear as a native macOS USB device.

Dependencies: clang, Python 3, curl, pkg-config, libusb and libimobiledevice-glue.

```
contrib/libirecovery-qemu/build.sh /tmp/libirecovery-qemu
python3 imgtools/usb_recovery_bridge.py --socket /tmp/ipad-usb.sock --port 23592
```

Start QEMU separately with `usb-tcp-addr=127.0.0.1:23592`. Then:

```
IRECV_QEMU_SOCKET=/tmp/ipad-usb.sock \
DYLD_LIBRARY_PATH=/tmp/libirecovery-qemu irecovery -q
```

With `IRECV_QEMU_SOCKET` set, recovery opens and hotplug events use only this
socket. Failure to connect does not fall back to physical USB. `idevicerestore`
should additionally select the synthetic device's ECID with `-i`. Its normal-mode
libimobiledevice/usbmux transport is separate and is not replaced by this adapter.

The build pins upstream revision `93c117c29b1f6669bc4ceca8b84e1df06449fe33`.
Upstream libirecovery is LGPL-2.1; its copyright and license notices are preserved.
The generated source remains in the output directory for review/rebuilding.

Validated: querying real-ROM DFU; stock `irecovery -f` uploading unmodified iBSS;
querying the resulting recovery device; stock `idevicerestore -c -z` reaching
the restore ramdisk. Pass `--usbmuxd HOST:PORT` to the Python bridge to hand the
restore-kernel USB link to usbmuxd-qemu. See `tests/ipad1/restore-smoke.py` for
the complete isolated runner.
