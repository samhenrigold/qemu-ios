# Offline hardware-codec reference

Build with `bash contrib/it-audio/build.sh /tmp/it-audio-offline`. This uses the
existing ARMv6 toolchain and the iPhoneOS 3.1.3 SDK (`ARMV6_SDK` overrides its
local default). It dynamically loads the device's AudioToolbox APIs.

In a fresh temporary directory on a disposable emulator or reference device,
place the executable and a short supported audio file named `input.m4a`.
Run the executable from that directory. It writes interleaved signed 16-bit
stereo PCM at 44,100 Hz to `output.pcm`. Copy the result back and remove the
temporary directory. No app installation or SpringBoard restart is required.

The probe requires hardware decoding and successfully enables **offline mode
before starting the queue**. Its session category permits the output queue,
but samples go to the PCM file rather than speakers. It sizes and primes the
decode buffer for the entire file; the default 0.75-second buffer is inadequate
for a fast offline comparison. Inputs are limited to 30 seconds, 4,096 packets
and 16 MiB of encoded allocation. Nonzero API statuses fail the probe.

By default it uses AudioFile's stream description. For an implicit-SBR AAC
file, `IT_AUDIO_HE=1 ./it-audio-offline` requests the corresponding HE-AAC
description instead, retaining the core's channel count. This compares the LC
and HE hardware programs with the same input. It is a diagnostic override,
not automatic format detection.

The executable exits explicitly because the ARMv6 launcher has no C runtime
entry wrapper to return to. Queue disposal and session deactivation happen on
success; process exit releases them after a failed check.
