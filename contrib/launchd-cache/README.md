# Launchd service-cache metadata

iOS 8 release launchd takes System daemon jobs from the `__xpcd_cache`
section of `xpcd_cache.dylib`, which is normally inside the dyld shared
cache. Editing or adding a daemon plist alone does not update that cache.

`build.sh OUTPUT` builds a signed armv7 metadata template using the same
toolchain as the other guest helpers. Set `ARMV6_SDK` and `LDID` as usual.
Copy the result into the helpers directory as `launchd-cache-template`.

FirmwareKit's `LaunchdCache.refresh` retains the firmware's own service
metadata and mirrors only the job definitions changed by shared preparation.
It fills the reserved section, updates its length and signature, and installs
the result at the original dylib path. The existing dyld override switch
selects it, as with OpenGLES. No launchd instructions or dyld-cache bytes
are patched. Firmware without this cache is left on its existing path.

