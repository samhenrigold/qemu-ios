/* it_msmquiet tells the USB "not supported" notice by its text: the key's localization itself (3.2 to 5.x), or, as
 * 7.x's DeviceOMatic raises it, that text behind the device's name ("QEMU USB Keyboard: The connected device is not
 * supported."), whether the localization is the sentence or the format the name goes into. Another notice with the
 * same title, a text that only shares a few last characters, and a value that isn't a string are not it.
 * 7.1.2 showed the named notice at every boot, over whatever was on screen, until its OK was tapped.
 *
 * SLICE:shim contrib/it-msmquiet/it_msmquiet.c file
 * CFLAGS -framework CoreFoundation -Wno-deprecated-declarations
 */
#include <assert.h>
#include <stdio.h>
#undef NULL   /* the shim builds with -nostdinc and spells out its own */
#include "shim.h"

extern CFTypeRef CFNumberCreate(CFAllocatorRef allocator, long type, const void *value);

int main(void)
{
    CFStringRef sentence = CFSTR("The connected USB device is not supported.");
    CFStringRef format = CFSTR("%@: The connected device is not supported.");
    int seven = 7;
    CFTypeRef number = CFNumberCreate(NULL, 9 /* kCFNumberIntType */, &seven);

    assert(says(sentence, sentence));                                              /* 4.x and 5.x: the text itself */
    assert(says(CFSTR("QEMU USB Keyboard: The connected device is not supported."), format));   /* 7.x */
    assert(says(CFSTR("QEMU USB Keyboard: The connected USB device is not supported."), sentence));
    assert(!says(CFSTR("The connected USB device requires too much power."), sentence));
    assert(!says(CFSTR("QEMU USB Keyboard: The connected device needs too much power."), format));
    assert(!says(CFSTR("Camera import finished."), CFSTR("%@ finished.")));         /* too short a tail to tell by */
    assert(!says(number, sentence) && !says(number, format));
    assert(is_unsupported(CFSTR("UNSUPPORTED_FAILURE")) && is_unsupported(CFSTR("UNSUPPORTED_FAILURE_BODY")));
    assert(!is_unsupported(CFSTR("POWER_HUNGRY_FAILURE")) && !is_unsupported(NULL));
    puts("PASS: the notice by its text, bare or behind the device's name; other notices pass");
    return 0;
}
