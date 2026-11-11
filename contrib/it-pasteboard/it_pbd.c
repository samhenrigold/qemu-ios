/*
 * it_pbd -- the host<->guest clipboard and nothing else: it_agent's pasteboard
 * code (contrib/it-agent/it_agent.c, which has the why and THE TRAP) with its own
 * main. it_agent carries the clipboard since guest package serial 2; this is the
 * standalone binary older images and the iPad's bake still install.
 *
 * Build: ./build.sh (see ../armv6-toolchain/README.md).
 */
#define IT_AGENT_CLIPBOARD_ONLY
#include "../it-agent/it_agent.c"

int main(void)
{
    return clipboard_main();
}
