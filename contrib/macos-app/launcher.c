/*
 * The app bundle's main executable.
 *
 * This was a shell script until notarization was on the table. A bundle whose
 * CFBundleExecutable is a script cannot carry the hardened runtime -- the
 * runtime is a property of a Mach-O's code signature, and a script has no code
 * signature -- so the notary service rejects it. A few lines of C that exec the
 * same script fixes that without moving any logic out of the script, where it
 * can still be read and run by hand.
 *
 * Everything it sets is a path into the bundle, except the state directory:
 * the bundle is read-only and signed, so the runner's writable areas live in
 * Application Support instead.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <sys/stat.h>
#include <mach-o/dyld.h>

static void setenv_fmt(const char *key, const char *fmt, const char *a)
{
    char buf[4096];
    snprintf(buf, sizeof(buf), fmt, a);
    setenv(key, buf, 1);
}

int main(int argc, char **argv)
{
    char exec_path[4096];
    uint32_t size = sizeof(exec_path);
    char res[4096], state[4096], runner[4096];
    const char *home = getenv("HOME");

    if (_NSGetExecutablePath(exec_path, &size) != 0 || home == NULL) {
        fprintf(stderr, "iPod touch: cannot locate the app bundle\n");
        return 1;
    }
    /* .../Contents/MacOS/iPod touch -> .../Contents/Resources */
    snprintf(res, sizeof(res), "%s/../Resources", dirname(exec_path));

    snprintf(state, sizeof(state), "%s/Library/Application Support/iPod touch", home);
    mkdir(state, 0755);

    setenv("IPOD_FILES", state, 1);
    setenv_fmt("QEMU",            "%s/../MacOS/qemu-system-arm", res);
    setenv_fmt("MUXD",            "%s/tools/usbmuxd", res);
    /*
     * usbmuxd's config directory, which holds the lockdown pairing records.
     * Without this the runner falls back to ~/Developer/usbmuxd-qemu/run/conf
     * and silently creates that tree in the home directory of someone who has
     * never heard of the project -- and puts device state outside Application
     * Support, so "delete that folder to factory reset" stops being true.
     */
    setenv_fmt("MUXD_CONF", "%s/usbmuxd-conf", state);
    setenv_fmt("IT_INSTALL_IPA",  "%s/tools/install-ipa.sh", res);
    setenv_fmt("IT_SSH_TERMINAL", "%s/tools/it-ssh-terminal.sh", res);
    setenv_fmt("IT_APP_RESOURCES", "%s", res);
    /*
     * Everything the scripts would otherwise need python3 for. A clean macOS
     * has none -- /usr/bin/python3 is a Command Line Tools shim that pops an
     * install dialog -- and the first-run NAND unpack went through it, so
     * without this the app cannot start on the machine it is meant for.
     */
    setenv_fmt("IT_HELPER", "%s/tools/ipod-helper", res);

    /*
     * libimobiledevice's tools (iproxy, ideviceinstaller, ideviceinfo,
     * idevice_id) are bundled into Resources/tools by build-app.sh, so a
     * fresh Mac needs nothing extra for Install App... or Open Terminal.
     * The bundle directory comes first on PATH; the Homebrew locations
     * after it are only a fallback for a build that skipped bundling them
     * (no libimobiledevice on the build machine), and both features still
     * report their own absence clearly if nothing resolves.
     */
    setenv_fmt("PATH", "%s/tools:/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin", res);

    snprintf(runner, sizeof(runner), "%s/tools/stage-and-run.sh", res);

    char **args = calloc(argc + 2, sizeof(char *));
    args[0] = runner;
    for (int i = 1; i < argc; i++) {
        args[i] = argv[i];
    }
    execv(runner, args);
    fprintf(stderr, "iPod touch: cannot run %s: %s\n", runner, strerror(errno));
    return 1;
}
