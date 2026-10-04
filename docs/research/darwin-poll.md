# Darwin nanosecond poll deadlines

Darwin lacks the ppoll path used by QEMU on other POSIX hosts. Its previous GLib fallback rounded each finite timeout up to whole milliseconds, including sub-millisecond timer deadlines. The Darwin path now uses a per-call kqueue wait with the requested timespec, retaining g_poll as the authority for returned descriptor masks and counts. It falls back for unsupported interests/resources/filter errors, preserves EINTR, and leaves zero/infinite waits unchanged. No guest clock source or timer frequency changes.

The actual-source native sanitizer test exercises descriptors, duplicates, EOF, invalid descriptors, signals and fallbacks, and checks exact100000ns forwarding. Its negative control rejects millisecond rounding deterministically; measured latency is recorded without a load-dependent pass threshold.

Untraced disposable native2.1.1 passes13/13 including warm reset, clean cold persistence and fsck. A matching3.1.3 pair changes only the private util timer object, with all other link inputs hashed unchanged: baseline and candidate both pass8/8, including two guest-confirmed shutdowns, filesystem consistency, rendering and audio. `/private/tmp/ltm-next-poll-pair` contains the exact artifact/hash receipts; native results are in `ltm-next-poll-{baseline,candidate}-ios313` and `ltm-next-poll-ordinary-ios211` under `/private/tmp`.

Diagnostic LCD-traced2.1.1 passes8/8 separately. This does not qualify a guest timer rate correction or establish a comparative animation improvement. The known10MHz/6MHz timer mismatch remains pending controlled cadence measurements. Current universal/Intel and supported-old-host runtime qualification remain separate from these arm64 local controls.
