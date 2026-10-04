# Retired guest libc translator substitutions

The candidate removes `target/arm/tcg/it-hle.c`, its header/helper declarations,
and the interception in ARM instruction translation. This deletes 477 lines.
The old optional implementation matched fixed iOS 3.1.3 libc virtual addresses
and instruction words, then counted or replaced memcpy/memmove, memset and
bzero on the host. These are guest functions, not hardware behavior. The
`IT_HLE`, `IT_HLE_COUNT`, `IT_HLE_STATS` and address override controls are retired.
Normal upstream ARM instruction translation now always executes their bodies.
No speedup or new hardware capability is claimed.

The required default native iPod regression passes 8/8 on a freshly prepared
7E18 base: boot, fsck, two-boot file persistence, install, confirmed foreground
launch, live GLES, shell-free agent and stereo audio. The same translator also
runs the iPad 7B500 developer offer, with authenticated stock SSH and byte-exact
SFTP using the updated GNU shell. Both use private overlays; the bases remain
unchanged. This does not remove the separately declared guest GL/activation
additions or their hypercall transport.

Evidence:

- `/private/tmp/ltm-retire-tcg-hle-build.log`
- `/private/tmp/ltm-retire-tcg-hle-default-regress.log`
- `/private/tmp/ltm-developer-v2-k48-native`
