// Print SpringBoard's icon layout as an XML plist on stdout (pages -> icons with
// bundle ids, in on-screen order), via lockdown's stock springboardservices.
// "sbicons set" instead reads an XML icon state on stdin and applies it.
//
//   cc -o sbicons sbicons.c $(pkg-config --cflags --libs libimobiledevice-1.0)
//   USBMUXD_SOCKET_ADDRESS=127.0.0.1:PORT ./sbicons [set]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>
#include <libimobiledevice/sbservices.h>

int main(int argc, char **argv)
{
    idevice_t dev = NULL;
    lockdownd_client_t ld = NULL;
    lockdownd_service_descriptor_t svc = NULL;
    sbservices_client_t sb = NULL;
    plist_t state = NULL;
    char *xml = NULL;
    uint32_t len = 0;
    int rc = 1;

    if (idevice_new(&dev, NULL) != IDEVICE_E_SUCCESS)
        goto out;
    if (lockdownd_client_new_with_handshake(dev, &ld, "sbicons") != LOCKDOWN_E_SUCCESS)
        goto out;
    if (lockdownd_start_service(ld, "com.apple.springboardservices", &svc) != LOCKDOWN_E_SUCCESS)
        goto out;
    if (sbservices_client_new(dev, svc, &sb) != SBSERVICES_E_SUCCESS)
        goto out;
    if (argc > 1 && !strcmp(argv[1], "set")) {
        static char buf[1 << 20];
        size_t n = fread(buf, 1, sizeof buf, stdin);
        plist_from_xml(buf, (uint32_t)n, &state);
        if (state && sbservices_set_icon_state(sb, state) == SBSERVICES_E_SUCCESS)
            rc = 0;
        goto out;
    }
    if (sbservices_get_icon_state(sb, &state, NULL) != SBSERVICES_E_SUCCESS || !state)
        goto out;
    plist_to_xml(state, &xml, &len);
    fwrite(xml, 1, len, stdout);
    rc = 0;
out:
    if (rc)
        fprintf(stderr, "sbicons: could not read icon state\n");
    free(xml);
    if (state) plist_free(state);
    if (sb) sbservices_client_free(sb);
    if (svc) lockdownd_service_descriptor_free(svc);
    if (ld) lockdownd_client_free(ld);
    if (dev) idevice_free(dev);
    return rc;
}
