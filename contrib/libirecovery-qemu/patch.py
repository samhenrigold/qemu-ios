#!/usr/bin/env python3
"""Add a socket transport to pinned upstream libirecovery; retain its protocol."""
from pathlib import Path
import sys
source = Path(sys.argv[1])
s = source.read_text()
pos = s.index('static int check_context(')
s = s[:pos] + Path(__file__).with_name('transport.inc').read_text() + '\n' + s[pos:]
branches = {
 'irecv_open_with_ecid': 'return qopen(pclient, ecid);',
 'irecv_usb_control_transfer': 'return qcontrol(bm_request_type, b_request, w_value, w_index, data, w_length, timeout);',
 'irecv_usb_bulk_transfer': 'return qbulk(endpoint, data, length, transferred, timeout);',
 'irecv_usb_interrupt_transfer': 'return qbulk(endpoint, data, length, transferred, timeout);',
 'irecv_usb_set_configuration': 'return qcontrol(0, 9, configuration, 0, NULL, 0, 1000) < 0 ? IRECV_E_USB_CONFIGURATION : IRECV_E_SUCCESS;',
 'irecv_usb_set_interface': 'client->usb_interface = usb_interface; client->usb_alt_interface = usb_alt_interface; return usb_interface ? (qcontrol(1, 11, usb_alt_interface, usb_interface, NULL, 0, 1000) < 0 ? IRECV_E_USB_INTERFACE : IRECV_E_SUCCESS) : IRECV_E_SUCCESS;',
 'irecv_reset': 'return qrpc(4, NULL, 0, NULL, 0) < 0 ? IRECV_E_UNKNOWN_ERROR : IRECV_E_SUCCESS;',
 'irecv_device_event_subscribe': 'return qsubscribe(context, callback, user_data);',
 'irecv_device_event_unsubscribe': 'return qunsubscribe(context);',
 'irecv_cleanup': 'if (client) client->handle = NULL;',
}
import re
for name, body in branches.items():
    pattern = r'\n(?:static )?(?:IRECV_API )?(?:irecv_error_t|int) ' + name + r'\([^;]*?\)\n\{'
    match = re.search(pattern, s)
    if not match:
        raise SystemExit('missing upstream function ' + name)
    pos = match.end()
    s = s[:pos] + '\n    if (getenv("IRECV_QEMU_SOCKET")) { ' + body + ' }\n' + s[pos:]
source.write_text(s)
