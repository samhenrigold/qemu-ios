# Common armv6 guest helper ABI

The production export builds it_agent, it_typein, sblaunch, sbdlicon and the
armv6 it_prefs with the existing LEGACY_LINK=1 toolchain. This uses classic
Mach-O relocations, reserves r9 for 2.x's thread pointer, and initializes old
libSystem through the existing crt1old startup. No second helper implementation
or user-selectable helper slot is introduced. The iPad's armv7 build is unchanged.

The n72-ios2 and n72-ios30 packages now include the agent, launcher and install
placeholder tool. They still omit media/proxy/status tools whose older API
contracts have not been qualified. N45's 1.x package remains GL-only.

Two separate failures were reproduced and corrected:

- An unsigned it_typein executable page makes stock 2.x reject SpringBoard's
  injected library. 3.0 also remained at the Apple logo despite working lockdown.
  The production recipe now signs the dylib; package assembly refuses an
  unsigned injected typein hook. The unsigned runs are retained at
  `/private/tmp/ltm-n72-211-legacy13-session` and
  `/private/tmp/ltm-n72-30-legacy13-session`.
- 5F138 has no SBSLaunchApplicationWithIdentifier. Its stock
  SBLaunchApplication MIG stub consumes the server port, a byte flag and a
  bounded UTF-8 string. The agent and sblaunch share the exported-API selector
  in contrib/it-agent/sbs-launch.h. Later firmware uses its existing CFString
  API. Guest instructions and Mach message layouts are not patched or rebuilt.

Native tests use the stock installation service, a real iOS 2-compatible
PAC-MAN Lite IPA (com.namconetworks.pacmanlite), and a private writable overlay.
Actual frontmost bundle identity is required: installation alone is insufficient.
The single-session harness can now exercise `--launch --reboot`, checking the
AFC marker byte for byte, installed-app persistence, generated identity, Home,
automatic activation and guest-confirmed power-off on both boots.

- 2.1.1 / 5F138: **18/18 PASS**, signed production export; evidence
  `/private/tmp/ltm-n72-211-legacy13-signed-session`.
- 3.1.3 / 7E18: **18/18 PASS**, common legacy helpers; evidence
  `/private/tmp/ltm-n72-313-legacy13-session`.
- 3.0 / 7A341: **18/18 PASS**, signed production export; evidence
  `/private/tmp/ltm-n72-30-legacy13-signed-session`.
- 4.2.1 / 8C148: **18/18 PASS**, signed production export; evidence
  `/private/tmp/ltm-n72-421-legacy13-signed-session`.

These tests establish agent readiness, foreground/lock queries and application
launch. They do not qualify text injection, clipboard, download placeholders,
orientation or developer SSH on every older firmware. Package update/rollback
and torn-install tests pass under ASan/UBSan independently of native boot.
Guest serial 13 / 1.1.11 carries this ABI and signature correction.
