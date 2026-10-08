/* The maintained Harness PCM ownership doubles (contrib/it-harness/test-pcm.c, with its own main) under
 * ASan/UBSan; no SDK needed. Guest waveform qualification is separate. Included rather than sliced so that
 * test-pcm.c finds pcm_audio.h beside it.
 *
 * CFLAGS -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function
 */
#include "../../contrib/it-harness/test-pcm.c"
