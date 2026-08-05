/*
 * QEMU Cocoa CG display driver
 *
 * Copyright (c) 2008 Mike Kronenberg
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <crt_externs.h>

#include "qemu/help-texts.h"
#include "qemu-main.h"
#include "ui/clipboard.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/kbd-state.h"
#include "system/system.h"
#include "system/runstate.h"
#include "system/runstate-action.h"
#include "system/cpu-throttle.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-block.h"
#include "qapi/qapi-commands-control.h"
#include "qapi/qapi-commands-machine.h"
#include "qapi/qapi-commands-misc.h"
#include "system/blockdev.h"
#include "hw/qdev-core.h"
#include "qemu-version.h"
#include "qemu/cutils.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include <Carbon/Carbon.h>
#include "hw/core/cpu.h"

#ifndef MAC_OS_VERSION_14_0
#define MAC_OS_VERSION_14_0 140000
#endif

//#define DEBUG

#ifdef DEBUG
#define COCOA_DEBUG(...)  { (void) fprintf (stdout, __VA_ARGS__); }
#else
#define COCOA_DEBUG(...)  ((void) 0)
#endif

#define cgrect(nsrect) (*(CGRect *)&(nsrect))

#define UC_CTRL_KEY "\xe2\x8c\x83"
#define UC_ALT_KEY "\xe2\x8c\xa5"

typedef struct {
    int width;
    int height;
} QEMUScreen;

@class QemuCocoaPasteboardTypeOwner;

static void cocoa_update(DisplayChangeListener *dcl,
                         int x, int y, int w, int h);

static void cocoa_switch(DisplayChangeListener *dcl,
                         DisplaySurface *surface);

static void cocoa_refresh(DisplayChangeListener *dcl);
static void cocoa_mouse_set(DisplayChangeListener *dcl, int x, int y, bool on);
static void cocoa_cursor_define(DisplayChangeListener *dcl, QEMUCursor *cursor);

static const DisplayChangeListenerOps dcl_ops = {
    .dpy_name          = "cocoa",
    .dpy_gfx_update = cocoa_update,
    .dpy_gfx_switch = cocoa_switch,
    .dpy_refresh = cocoa_refresh,
    .dpy_mouse_set = cocoa_mouse_set,
    .dpy_cursor_define = cocoa_cursor_define,
};
static DisplayChangeListener dcl = {
    .ops = &dcl_ops,
};
static QKbdState *kbd;
static int cursor_hide = 1;
static int left_command_key_enabled = 1;
static bool swap_opt_cmd;

static CGInterpolationQuality zoom_interpolation = kCGInterpolationNone;
static NSTextField *pauseLabel;

static bool allow_events;

static NSInteger cbchangecount = -1;
static QemuClipboardInfo *cbinfo;
static QemuEvent cbevent;
static QemuCocoaPasteboardTypeOwner *cbowner;

// Utility functions to run specified code block with the BQL held
typedef void (^CodeBlock)(void);
typedef bool (^BoolCodeBlock)(void);

static void with_bql(CodeBlock block)
{
    bool locked = bql_locked();
    if (!locked) {
        bql_lock();
    }
    block();
    if (!locked) {
        bql_unlock();
    }
}

static bool bool_with_bql(BoolCodeBlock block)
{
    bool locked = bql_locked();
    bool val;

    if (!locked) {
        bql_lock();
    }
    val = block();
    if (!locked) {
        bql_unlock();
    }
    return val;
}

// Mac to QKeyCode conversion
static const int mac_to_qkeycode_map[] = {
    [kVK_ANSI_A] = Q_KEY_CODE_A,
    [kVK_ANSI_B] = Q_KEY_CODE_B,
    [kVK_ANSI_C] = Q_KEY_CODE_C,
    [kVK_ANSI_D] = Q_KEY_CODE_D,
    [kVK_ANSI_E] = Q_KEY_CODE_E,
    [kVK_ANSI_F] = Q_KEY_CODE_F,
    [kVK_ANSI_G] = Q_KEY_CODE_G,
    [kVK_ANSI_H] = Q_KEY_CODE_H,
    [kVK_ANSI_I] = Q_KEY_CODE_I,
    [kVK_ANSI_J] = Q_KEY_CODE_J,
    [kVK_ANSI_K] = Q_KEY_CODE_K,
    [kVK_ANSI_L] = Q_KEY_CODE_L,
    [kVK_ANSI_M] = Q_KEY_CODE_M,
    [kVK_ANSI_N] = Q_KEY_CODE_N,
    [kVK_ANSI_O] = Q_KEY_CODE_O,
    [kVK_ANSI_P] = Q_KEY_CODE_P,
    [kVK_ANSI_Q] = Q_KEY_CODE_Q,
    [kVK_ANSI_R] = Q_KEY_CODE_R,
    [kVK_ANSI_S] = Q_KEY_CODE_S,
    [kVK_ANSI_T] = Q_KEY_CODE_T,
    [kVK_ANSI_U] = Q_KEY_CODE_U,
    [kVK_ANSI_V] = Q_KEY_CODE_V,
    [kVK_ANSI_W] = Q_KEY_CODE_W,
    [kVK_ANSI_X] = Q_KEY_CODE_X,
    [kVK_ANSI_Y] = Q_KEY_CODE_Y,
    [kVK_ANSI_Z] = Q_KEY_CODE_Z,

    [kVK_ANSI_0] = Q_KEY_CODE_0,
    [kVK_ANSI_1] = Q_KEY_CODE_1,
    [kVK_ANSI_2] = Q_KEY_CODE_2,
    [kVK_ANSI_3] = Q_KEY_CODE_3,
    [kVK_ANSI_4] = Q_KEY_CODE_4,
    [kVK_ANSI_5] = Q_KEY_CODE_5,
    [kVK_ANSI_6] = Q_KEY_CODE_6,
    [kVK_ANSI_7] = Q_KEY_CODE_7,
    [kVK_ANSI_8] = Q_KEY_CODE_8,
    [kVK_ANSI_9] = Q_KEY_CODE_9,

    [kVK_ANSI_Grave] = Q_KEY_CODE_GRAVE_ACCENT,
    [kVK_ANSI_Minus] = Q_KEY_CODE_MINUS,
    [kVK_ANSI_Equal] = Q_KEY_CODE_EQUAL,
    [kVK_Delete] = Q_KEY_CODE_BACKSPACE,
    [kVK_CapsLock] = Q_KEY_CODE_CAPS_LOCK,
    [kVK_Tab] = Q_KEY_CODE_TAB,
    [kVK_Return] = Q_KEY_CODE_RET,
    [kVK_ANSI_LeftBracket] = Q_KEY_CODE_BRACKET_LEFT,
    [kVK_ANSI_RightBracket] = Q_KEY_CODE_BRACKET_RIGHT,
    [kVK_ANSI_Backslash] = Q_KEY_CODE_BACKSLASH,
    [kVK_ANSI_Semicolon] = Q_KEY_CODE_SEMICOLON,
    [kVK_ANSI_Quote] = Q_KEY_CODE_APOSTROPHE,
    [kVK_ANSI_Comma] = Q_KEY_CODE_COMMA,
    [kVK_ANSI_Period] = Q_KEY_CODE_DOT,
    [kVK_ANSI_Slash] = Q_KEY_CODE_SLASH,
    [kVK_Space] = Q_KEY_CODE_SPC,

    [kVK_ANSI_Keypad0] = Q_KEY_CODE_KP_0,
    [kVK_ANSI_Keypad1] = Q_KEY_CODE_KP_1,
    [kVK_ANSI_Keypad2] = Q_KEY_CODE_KP_2,
    [kVK_ANSI_Keypad3] = Q_KEY_CODE_KP_3,
    [kVK_ANSI_Keypad4] = Q_KEY_CODE_KP_4,
    [kVK_ANSI_Keypad5] = Q_KEY_CODE_KP_5,
    [kVK_ANSI_Keypad6] = Q_KEY_CODE_KP_6,
    [kVK_ANSI_Keypad7] = Q_KEY_CODE_KP_7,
    [kVK_ANSI_Keypad8] = Q_KEY_CODE_KP_8,
    [kVK_ANSI_Keypad9] = Q_KEY_CODE_KP_9,
    [kVK_ANSI_KeypadDecimal] = Q_KEY_CODE_KP_DECIMAL,
    [kVK_ANSI_KeypadEnter] = Q_KEY_CODE_KP_ENTER,
    [kVK_ANSI_KeypadPlus] = Q_KEY_CODE_KP_ADD,
    [kVK_ANSI_KeypadMinus] = Q_KEY_CODE_KP_SUBTRACT,
    [kVK_ANSI_KeypadMultiply] = Q_KEY_CODE_KP_MULTIPLY,
    [kVK_ANSI_KeypadDivide] = Q_KEY_CODE_KP_DIVIDE,
    [kVK_ANSI_KeypadEquals] = Q_KEY_CODE_KP_EQUALS,
    [kVK_ANSI_KeypadClear] = Q_KEY_CODE_NUM_LOCK,

    [kVK_UpArrow] = Q_KEY_CODE_UP,
    [kVK_DownArrow] = Q_KEY_CODE_DOWN,
    [kVK_LeftArrow] = Q_KEY_CODE_LEFT,
    [kVK_RightArrow] = Q_KEY_CODE_RIGHT,

    [kVK_Help] = Q_KEY_CODE_INSERT,
    [kVK_Home] = Q_KEY_CODE_HOME,
    [kVK_PageUp] = Q_KEY_CODE_PGUP,
    [kVK_PageDown] = Q_KEY_CODE_PGDN,
    [kVK_End] = Q_KEY_CODE_END,
    [kVK_ForwardDelete] = Q_KEY_CODE_DELETE,

    [kVK_Escape] = Q_KEY_CODE_ESC,

    /* The Power key can't be used directly because the operating system uses
     * it. This key can be emulated by using it in place of another key such as
     * F1. Don't forget to disable the real key binding.
     */
    /* [kVK_F1] = Q_KEY_CODE_POWER, */

    [kVK_F1] = Q_KEY_CODE_F1,
    [kVK_F2] = Q_KEY_CODE_F2,
    [kVK_F3] = Q_KEY_CODE_F3,
    [kVK_F4] = Q_KEY_CODE_F4,
    [kVK_F5] = Q_KEY_CODE_F5,
    [kVK_F6] = Q_KEY_CODE_F6,
    [kVK_F7] = Q_KEY_CODE_F7,
    [kVK_F8] = Q_KEY_CODE_F8,
    [kVK_F9] = Q_KEY_CODE_F9,
    [kVK_F10] = Q_KEY_CODE_F10,
    [kVK_F11] = Q_KEY_CODE_F11,
    [kVK_F12] = Q_KEY_CODE_F12,
    [kVK_F13] = Q_KEY_CODE_PRINT,
    [kVK_F14] = Q_KEY_CODE_SCROLL_LOCK,
    [kVK_F15] = Q_KEY_CODE_PAUSE,

    // JIS keyboards only
    [kVK_JIS_Yen] = Q_KEY_CODE_YEN,
    [kVK_JIS_Underscore] = Q_KEY_CODE_RO,
    [kVK_JIS_KeypadComma] = Q_KEY_CODE_KP_COMMA,
    [kVK_JIS_Eisu] = Q_KEY_CODE_MUHENKAN,
    [kVK_JIS_Kana] = Q_KEY_CODE_HENKAN,

    /*
     * The eject and volume keys can't be used here because they are handled at
     * a lower level than what an Application can see.
     */
};

static int cocoa_keycode_to_qemu(int keycode)
{
    if (ARRAY_SIZE(mac_to_qkeycode_map) <= keycode) {
        error_report("(cocoa) warning unknown keycode 0x%x", keycode);
        return 0;
    }
    return mac_to_qkeycode_map[keycode];
}

/* Displays an alert dialog box with the specified message */
static void QEMU_Alert(NSString *message)
{
    NSAlert *alert;
    alert = [NSAlert new];
    [alert setMessageText: message];
    [alert runModal];
}

/* Handles any errors that happen with a device transaction */
static void handleAnyDeviceErrors(Error * err)
{
    if (err) {
        QEMU_Alert([NSString stringWithCString: error_get_pretty(err)
                                      encoding: NSASCIIStringEncoding]);
        error_free(err);
    }
}

/*
 ------------------------------------------------------
    QemuCocoaView
 ------------------------------------------------------
*/
@interface QemuCocoaView : NSView
{
    QEMUScreen screen;
    pixman_image_t *pixman_image;
    /* The state surrounding mouse grabbing is potentially confusing.
     * isAbsoluteEnabled tracks qemu_input_is_absolute() [ie "is the emulated
     *   pointing device an absolute-position one?"], but is only updated on
     *   next refresh.
     * isMouseGrabbed tracks whether GUI events are directed to the guest;
     *   it controls whether special keys like Cmd get sent to the guest,
     *   and whether we capture the mouse when in non-absolute mode.
     */
    BOOL isMouseGrabbed;
    BOOL isAbsoluteEnabled;
    CFMachPortRef eventsTap;
    CGColorSpaceRef colorspace;
    CALayer *cursorLayer;
    QEMUCursor *cursor;
    int mouseX;
    int mouseY;
    bool mouseOn;
    /*
     * Option-drag pinch. A single host pointer cannot describe two contacts, so
     * Option synthesises a second one; see -updatePinch: for the arithmetic.
     * Points are kept in guest framebuffer coordinates, not window ones, so the
     * mirror is about the middle of the *screen* whatever the window's size.
     */
    BOOL pinchDown;
    BOOL leftButtonDown;
    BOOL haveMousePoint;
    NSPoint mousePoint;
    NSPoint prevMousePoint;
    NSPoint pinchPoint;
    NSUInteger previousModifierFlags;
    /*
     * Show taps. Drawn over the scanout in -drawRect:, never into the guest's
     * framebuffer -- so screendumps and the regression harness, which assert on
     * pixel values, cannot see any of it.
     */
    BOOL showTaps;
    NSRect tapRects[2];
    int tapCount;
    /* The dots are a preview -- Option is held but nothing is touching yet. */
    BOOL tapPreview;
}
- (void) switchSurface:(pixman_image_t *)image;
- (void) grabMouse;
- (void) ungrabMouse;
- (void) setFullGrab:(id)sender;
- (void) handleMonitorInput:(NSEvent *)event;
- (bool) handleEvent:(NSEvent *)event;
- (bool) handleEventLocked:(NSEvent *)event;
- (void) notifyMouseModeChange;
- (void) copyScreenToPasteboard;
- (void) installIPA:(NSString *)path;
- (void) refreshTapMarkers;
- (void) drawTapMarkers:(CGContextRef)ctx;
- (void) toggleShowTaps:(id)sender;
- (BOOL) showTaps;
- (void) updatePinch:(NSUInteger)modifiers;
- (void) modifiersChanged:(NSUInteger)modifiers;
- (BOOL) isMouseGrabbed;
- (QEMUScreen) gscreen;
- (void) raiseAllKeys;
@end

QemuCocoaView *cocoaView;

static CGEventRef handleTapEvent(CGEventTapProxy proxy, CGEventType type, CGEventRef cgEvent, void *userInfo)
{
    QemuCocoaView *view = userInfo;
    NSEvent *event = [NSEvent eventWithCGEvent:cgEvent];
    if ([view isMouseGrabbed] && [view handleEvent:event]) {
        COCOA_DEBUG("Global events tap: qemu handled the event, capturing!\n");
        return NULL;
    }
    COCOA_DEBUG("Global events tap: qemu did not handle the event, letting it through...\n");

    return cgEvent;
}

@implementation QemuCocoaView
- (id)initWithFrame:(NSRect)frameRect
{
    COCOA_DEBUG("QemuCocoaView: initWithFrame\n");

    self = [super initWithFrame:frameRect];
    if (self) {

        NSTrackingAreaOptions options = NSTrackingActiveInKeyWindow |
                                        NSTrackingMouseEnteredAndExited |
                                        NSTrackingMouseMoved |
                                        NSTrackingInVisibleRect;

        NSTrackingArea *trackingArea =
            [[NSTrackingArea alloc] initWithRect:CGRectZero
                                         options:options
                                           owner:self
                                        userInfo:nil];

        [self addTrackingArea:trackingArea];
        [trackingArea release];
        /* Drop an .ipa on the window to install it. */
        [self registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
        /*
         * On by default: it cannot reach a screendump, and an Option-drag whose
         * second finger is invisible is a gesture you perform blind.
         */
        showTaps = YES;
        screen.width = frameRect.size.width;
        screen.height = frameRect.size.height;
        colorspace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
#if MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_VERSION_14_0
        [self setClipsToBounds:YES];
#endif
        [self setWantsLayer:YES];
        cursorLayer = [[CALayer alloc] init];
        [cursorLayer setAnchorPoint:CGPointMake(0, 1)];
        [cursorLayer setAutoresizingMask:kCALayerMaxXMargin |
                                         kCALayerMinYMargin];
        [[self layer] addSublayer:cursorLayer];

    }
    return self;
}

- (void) dealloc
{
    COCOA_DEBUG("QemuCocoaView: dealloc\n");

    if (pixman_image) {
        pixman_image_unref(pixman_image);
    }

    if (eventsTap) {
        CFRelease(eventsTap);
    }

    CGColorSpaceRelease(colorspace);
    [cursorLayer release];
    cursor_unref(cursor);
    [super dealloc];
}

- (BOOL) isOpaque
{
    return YES;
}

- (void) viewDidMoveToWindow
{
    [self resizeWindow];
}

- (void) selectConsoleLocked:(unsigned int)index
{
    QemuConsole *con = qemu_console_lookup_by_index(index);
    if (!con) {
        return;
    }

    unregister_displaychangelistener(&dcl);
    qkbd_state_switch_console(kbd, con);
    dcl.con = con;
    register_displaychangelistener(&dcl);
    [self notifyMouseModeChange];
    [self updateUIInfo];
}

- (void) hideCursor
{
    if (!cursor_hide) {
        return;
    }
    [NSCursor hide];
}

- (void) unhideCursor
{
    if (!cursor_hide) {
        return;
    }
    [NSCursor unhide];
}

- (void)setMouseX:(int)x y:(int)y on:(bool)on
{
    CGPoint position;

    mouseX = x;
    mouseY = y;
    mouseOn = on;

    position.x = mouseX;
    position.y = screen.height - mouseY;

    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    [cursorLayer setPosition:position];
    [cursorLayer setHidden:!mouseOn];
    [CATransaction commit];
}

- (void)setCursor:(QEMUCursor *)given_cursor
{
    CGDataProviderRef provider;
    CGImageRef image;
    CGRect bounds = CGRectZero;

    cursor_unref(cursor);
    cursor = given_cursor;

    if (!cursor) {
        return;
    }

    cursor_ref(cursor);

    bounds.size.width = cursor->width;
    bounds.size.height = cursor->height;

    provider = CGDataProviderCreateWithData(
        NULL,
        cursor->data,
        cursor->width * cursor->height * 4,
        NULL
    );

    image = CGImageCreate(
        cursor->width, //width
        cursor->height, //height
        8, //bitsPerComponent
        32, //bitsPerPixel
        cursor->width * 4, //bytesPerRow
        colorspace, //colorspace
        kCGBitmapByteOrder32Little | kCGImageAlphaFirst, //bitmapInfo
        provider, //provider
        NULL, //decode
        0, //interpolate
        kCGRenderingIntentDefault //intent
    );

    CGDataProviderRelease(provider);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    [cursorLayer setBounds:bounds];
    [cursorLayer setContents:(id)image];
    [CATransaction commit];
    CGImageRelease(image);
}

- (void) drawRect:(NSRect) rect
{
    COCOA_DEBUG("QemuCocoaView: drawRect\n");

    // get CoreGraphic context
    CGContextRef viewContextRef = [[NSGraphicsContext currentContext] CGContext];

    CGContextSetInterpolationQuality (viewContextRef, zoom_interpolation);
    CGContextSetShouldAntialias (viewContextRef, NO);

    // draw screen bitmap directly to Core Graphics context
    if (!pixman_image) {
        // Draw request before any guest device has set up a framebuffer:
        // just draw an opaque black rectangle
        CGContextSetRGBFillColor(viewContextRef, 0, 0, 0, 1.0);
        CGContextFillRect(viewContextRef, NSRectToCGRect(rect));
    } else {
        int w = pixman_image_get_width(pixman_image);
        int h = pixman_image_get_height(pixman_image);
        int bitsPerPixel = PIXMAN_FORMAT_BPP(pixman_image_get_format(pixman_image));
        int stride = pixman_image_get_stride(pixman_image);
        CGDataProviderRef dataProviderRef = CGDataProviderCreateWithData(
            NULL,
            pixman_image_get_data(pixman_image),
            stride * h,
            NULL
        );
        CGImageRef imageRef = CGImageCreate(
            w, //width
            h, //height
            DIV_ROUND_UP(bitsPerPixel, 8) * 2, //bitsPerComponent
            bitsPerPixel, //bitsPerPixel
            stride, //bytesPerRow
            colorspace, //colorspace
            kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst, //bitmapInfo
            dataProviderRef, //provider
            NULL, //decode
            0, //interpolate
            kCGRenderingIntentDefault //intent
        );
        // selective drawing code (draws only dirty rectangles) (OS X >= 10.4)
        const NSRect *rectList;
        NSInteger rectCount;
        int i;
        CGImageRef clipImageRef;
        CGRect clipRect;

        [self getRectsBeingDrawn:&rectList count:&rectCount];
        for (i = 0; i < rectCount; i++) {
            clipRect = rectList[i];
            clipRect.origin.y = (float)h - (clipRect.origin.y + clipRect.size.height);
            clipImageRef = CGImageCreateWithImageInRect(
                                                        imageRef,
                                                        clipRect
                                                        );
            CGContextDrawImage (viewContextRef, cgrect(rectList[i]), clipImageRef);
            CGImageRelease (clipImageRef);
        }
        CGImageRelease (imageRef);
        CGDataProviderRelease(dataProviderRef);
    }

    [self drawTapMarkers:viewContextRef];
}

/*
 * Show taps -- grey dots where the contacts are.
 *
 * The iPhone Simulator did this in a separate borderless window with
 * ignoresMouseEvents set, purely so the dots could never be a hit-test
 * participant. We have no compositing layer above the guest framebuffer, so
 * that does not transfer; drawing here instead, AFTER the scanout has been
 * blitted and into the view's own context, gets the same property for free.
 * The guest's framebuffer is never touched, which matters: screendumps and the
 * regression harness assert on those pixel values and must not see a dot.
 *
 * The second finger of an Option-drag is the reason this exists at all. It is
 * invisible otherwise, which makes a pinch something you do blind.
 */
- (void) drawTapMarkers:(CGContextRef)ctx
{
    int i;

    if (!showTaps) {
        return;
    }
    CGContextSetShouldAntialias(ctx, YES);
    for (i = 0; i < tapCount; i++) {
        CGRect r = NSRectToCGRect(tapRects[i]);

        /*
         * A preview is drawn as an outline: it has to read as "this is where
         * the fingers will land", not as "the screen is being touched", or
         * aiming a pinch and performing one look the same.
         */
        if (tapPreview) {
            CGContextSetRGBFillColor(ctx, 0.75, 0.75, 0.75, 0.12);
            CGContextFillEllipseInRect(ctx, r);
            CGContextSetRGBStrokeColor(ctx, 0.95, 0.95, 0.95, 0.55);
        } else {
            CGContextSetRGBFillColor(ctx, 0.75, 0.75, 0.75, 0.45);
            CGContextFillEllipseInRect(ctx, r);
            CGContextSetRGBStrokeColor(ctx, 0.95, 0.95, 0.95, 0.8);
        }
        CGContextSetLineWidth(ctx, 1.5);
        CGContextStrokeEllipseInRect(ctx, CGRectInset(r, 0.75, 0.75));
    }
    CGContextSetShouldAntialias(ctx, NO);
}

#define TAP_MARKER_RADIUS 13.0

/*
 * Recompute where the dots are and dirty both the old and the new positions.
 * Only the new ones is not enough: a static guest redraws nothing on its own,
 * so the previous dot would stay on screen and the markers would smear into a
 * trail behind the finger.
 */
- (void) refreshTapMarkers
{
    NSRect old[2];
    int oldCount = tapCount, i;
    NSPoint pts[2];
    int n = 0;

    if (!showTaps) {
        return;
    }
    memcpy(old, tapRects, sizeof(old));
    tapPreview = FALSE;

    if (leftButtonDown && haveMousePoint) {
        pts[n++] = mousePoint;
    }
    if (pinchDown) {
        pts[n++] = pinchPoint;
    }

    /*
     * Nothing is touching, but Option is held: show where the two contacts
     * WOULD be. Holding Option is how you aim a pinch, and doing that blind --
     * press first, then discover the second finger landed somewhere useless --
     * is the thing that made the gesture hard to place. No touch is injected
     * here; these are markers only, and pressing is still what starts the
     * contact.
     */
    if (n == 0 && haveMousePoint && isAbsoluteEnabled && isMouseGrabbed &&
        (previousModifierFlags & NSEventModifierFlagOption)) {
        NSPoint partner = [self pinchPartnerFor:previousModifierFlags];

        if (partner.x >= 0 && partner.y >= 0 &&
            partner.x < screen.width && partner.y < screen.height) {
            pts[n++] = mousePoint;
            pts[n++] = partner;
            tapPreview = TRUE;
        }
    }
    for (i = 0; i < n; i++) {
        /* Guest coordinates have y downwards; the view's origin is bottom left. */
        tapRects[i] = NSMakeRect(pts[i].x - TAP_MARKER_RADIUS,
                                 (screen.height - pts[i].y) - TAP_MARKER_RADIUS,
                                 TAP_MARKER_RADIUS * 2, TAP_MARKER_RADIUS * 2);
    }
    tapCount = n;

    for (i = 0; i < oldCount; i++) {
        [self setNeedsDisplayInRect:old[i]];
    }
    for (i = 0; i < n; i++) {
        [self setNeedsDisplayInRect:tapRects[i]];
    }
}

- (BOOL) showTaps { return showTaps; }

- (void) toggleShowTaps:(id)sender
{
    showTaps = !showTaps;
    [sender setState:showTaps ? NSControlStateValueOn : NSControlStateValueOff];
    if (!showTaps) {
        int i;
        for (i = 0; i < tapCount; i++) {
            [self setNeedsDisplayInRect:tapRects[i]];
        }
        tapCount = 0;
    } else {
        [self refreshTapMarkers];
    }
}

- (NSSize)fixAspectRatio:(NSSize)max
{
    NSSize scaled;
    NSSize fixed;

    scaled.width = screen.width * max.height;
    scaled.height = screen.height * max.width;

    /*
     * Here screen is our guest's output size, and max is the size of the
     * largest possible area of the screen we can display on.
     * We want to scale up (screen.width x screen.height) by either:
     *   1) max.height / screen.height
     *   2) max.width / screen.width
     * With the first scale factor the scale will result in an output height of
     * max.height (i.e. we will fill the whole height of the available screen
     * space and have black bars left and right) and with the second scale
     * factor the scaling will result in an output width of max.width (i.e. we
     * fill the whole width of the available screen space and have black bars
     * top and bottom). We need to pick whichever keeps the whole of the guest
     * output on the screen, which is to say the smaller of the two scale
     * factors.
     * To avoid doing more division than strictly necessary, instead of directly
     * comparing scale factors 1 and 2 we instead calculate and compare those
     * two scale factors multiplied by (screen.height * screen.width).
     */
    if (scaled.width < scaled.height) {
        fixed.width = scaled.width / screen.height;
        fixed.height = max.height;
    } else {
        fixed.width = max.width;
        fixed.height = scaled.height / screen.width;
    }

    return fixed;
}

- (NSSize) screenSafeAreaSize
{
    NSSize size = [[[self window] screen] frame].size;
    NSEdgeInsets insets = [[[self window] screen] safeAreaInsets];
    size.width -= insets.left + insets.right;
    size.height -= insets.top + insets.bottom;
    return size;
}

- (void) resizeWindow
{
    [[self window] setContentAspectRatio:NSMakeSize(screen.width, screen.height)];

    if (!([[self window] styleMask] & NSWindowStyleMaskResizable)) {
        [[self window] setContentSize:NSMakeSize(screen.width, screen.height)];
        [[self window] center];
    } else if ([[self window] styleMask] & NSWindowStyleMaskFullScreen) {
        [[self window] setContentSize:[self fixAspectRatio:[self screenSafeAreaSize]]];
        [[self window] center];
    } else {
        [[self window] setContentSize:[self fixAspectRatio:[self frame].size]];
    }
}

- (void) updateBounds
{
    [self setBoundsSize:NSMakeSize(screen.width, screen.height)];
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

- (void) updateUIInfoLocked
{
    /* Must be called with the BQL, i.e. via updateUIInfo */
    NSSize frameSize;
    QemuUIInfo info;

    if (!qemu_console_is_graphic(dcl.con)) {
        return;
    }

    if ([self window]) {
        NSDictionary *description = [[[self window] screen] deviceDescription];
        CGDirectDisplayID display = [[description objectForKey:@"NSScreenNumber"] unsignedIntValue];
        NSSize screenSize = [[[self window] screen] frame].size;
        CGSize screenPhysicalSize = CGDisplayScreenSize(display);
        bool isFullscreen = ([[self window] styleMask] & NSWindowStyleMaskFullScreen) != 0;
        CVDisplayLinkRef displayLink;

        frameSize = isFullscreen ? [self screenSafeAreaSize] : [self frame].size;

        if (!CVDisplayLinkCreateWithCGDisplay(display, &displayLink)) {
            CVTime period = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(displayLink);
            CVDisplayLinkRelease(displayLink);
            if (!(period.flags & kCVTimeIsIndefinite)) {
                update_displaychangelistener(&dcl,
                                             1000 * period.timeValue / period.timeScale);
                info.refresh_rate = (int64_t)1000 * period.timeScale / period.timeValue;
            }
        }

        info.width_mm = frameSize.width / screenSize.width * screenPhysicalSize.width;
        info.height_mm = frameSize.height / screenSize.height * screenPhysicalSize.height;
    } else {
        frameSize = [self frame].size;
        info.width_mm = 0;
        info.height_mm = 0;
    }

    info.xoff = 0;
    info.yoff = 0;
    info.width = frameSize.width;
    info.height = frameSize.height;

    dpy_set_ui_info(dcl.con, &info, TRUE);
}

#pragma clang diagnostic pop

- (void) updateUIInfo
{
    if (!allow_events) {
        /*
         * Don't try to tell QEMU about UI information in the application
         * startup phase -- we haven't yet registered dcl with the QEMU UI
         * layer.
         * When cocoa_display_init() does register the dcl, the UI layer
         * will call cocoa_switch(), which will call updateUIInfo, so
         * we don't lose any information here.
         */
        return;
    }

    with_bql(^{
        [self updateUIInfoLocked];
    });
}

- (void) switchSurface:(pixman_image_t *)image
{
    COCOA_DEBUG("QemuCocoaView: switchSurface\n");

    int w = pixman_image_get_width(image);
    int h = pixman_image_get_height(image);

    if (w != screen.width || h != screen.height) {
        // Resize before we trigger the redraw, or we'll redraw at the wrong size
        COCOA_DEBUG("switchSurface: new size %d x %d\n", w, h);
        screen.width = w;
        screen.height = h;
        [self resizeWindow];
        [self updateBounds];
    }

    // update screenBuffer
    if (pixman_image) {
        pixman_image_unref(pixman_image);
    }

    pixman_image = image;
}

/*
 * Copy Screen -- the iPhone Simulator's Cmd+Ctrl+C, which put the screen on the
 * pasteboard. Until now the only way to get a picture out of here was a QMP
 * screendump to a file, followed by brightening it by hand before it was
 * legible enough to look at.
 *
 * The brightening is dealt with at the source rather than afterwards: the panel
 * scales every channel by the backlight the guest programmed, and once a pixel
 * has been written at a tenth of its value the information is gone -- scaling
 * the result back up recovers banding, not detail. So the frame is re-rendered
 * with the backlight held at full for the grab, and re-rendered again as it
 * really looks before returning. Both happen inside one main-thread event, so
 * no draw can run between them and the window never flashes.
 */
- (void) copyScreenToPasteboard
{
    __block CGImageRef imageRef = NULL;

    with_bql(^{
        qemu_display_set_capture_exposure(true);
        graphic_hw_update(dcl.con);
        qemu_display_set_capture_exposure(false);

        if (pixman_image) {
            int w = pixman_image_get_width(pixman_image);
            int h = pixman_image_get_height(pixman_image);
            int bpp = PIXMAN_FORMAT_BPP(pixman_image_get_format(pixman_image));
            int stride = pixman_image_get_stride(pixman_image);
            CGDataProviderRef provider = CGDataProviderCreateWithData(
                NULL, pixman_image_get_data(pixman_image), stride * h, NULL);

            imageRef = CGImageCreate(w, h, DIV_ROUND_UP(bpp, 8) * 2, bpp,
                                     stride, colorspace,
                                     kCGBitmapByteOrder32Little |
                                     kCGImageAlphaNoneSkipFirst,
                                     provider, NULL, 0,
                                     kCGRenderingIntentDefault);
            CGDataProviderRelease(provider);
        }
    });

    if (imageRef) {
        /*
         * The provider hands out the live surface, so the bytes are copied out
         * here -- while the exposure render is still what is in it -- rather
         * than being read later from whatever the guest has drawn since.
         */
        NSBitmapImageRep *rep =
            [[[NSBitmapImageRep alloc] initWithCGImage:imageRef] autorelease];
        NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG
                                        properties:@{}];
        NSImage *img = [[[NSImage alloc] initWithData:png] autorelease];
        NSPasteboard *pb = [NSPasteboard generalPasteboard];

        [pb clearContents];
        [pb writeObjects:@[img]];
        CGImageRelease(imageRef);
    }

    /* Put back what the panel actually looks like. */
    with_bql(^{
        graphic_hw_update(dcl.con);
    });
}

/*
 * Drag and drop an .ipa on the window to install it.
 *
 * The iPhone Simulator never had this, and could not have: it had no installd
 * and no AFC, so "installing" there was only a path in a launch config. We have
 * both, and the whole install already works over USB from a terminal -- so all
 * that is missing is the gesture.
 *
 * The work is handed to imgtools/install-ipa.sh rather than done here, because
 * every part of it (finding the right usbmuxd, the DTSDKName / cryptid /
 * OpenGLES pre-flight, ideviceinstaller) is testable from a terminal and a drop
 * handler is not.
 *
 * The failure that matters is usbmuxd not being up: QEMU dials OUT to it when
 * the guest's USB core comes up, and if nothing was listening it gives up for
 * the rest of that boot. That has to be reported loudly and by name -- silently
 * doing nothing here would look exactly like a drop that missed.
 */
- (NSString *) installerPath
{
    const char *env = getenv("IT_INSTALL_IPA");

    if (env) {
        return [NSString stringWithUTF8String:env];
    }
    /*
     * For a plain executable -bundlePath is the directory holding it, which is
     * build/, so the tree's own script is one level up.
     */
    return [[[NSBundle mainBundle] bundlePath]
            stringByAppendingPathComponent:@"../imgtools/install-ipa.sh"];
}

- (void) installIPA:(NSString *)path
{
    NSString *tool = [self installerPath];

    if (![[NSFileManager defaultManager] isExecutableFileAtPath:tool]) {
        NSAlert *a = [[[NSAlert alloc] init] autorelease];
        [a setMessageText:@"Cannot install: the installer script is missing"];
        [a setInformativeText:[NSString stringWithFormat:
            @"Looked for %@.\n\nSet IT_INSTALL_IPA to its path if the tree is "
            @"somewhere else.", tool]];
        [a runModal];
        return;
    }

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        NSTask *task = [[NSTask alloc] init];
        NSPipe *pipe = [NSPipe pipe];

        [task setLaunchPath:tool];
        [task setArguments:@[path]];
        [task setStandardOutput:pipe];
        [task setStandardError:pipe];

        NSData *out = nil;
        int status = -1;
        @try {
            [task launch];
            out = [[pipe fileHandleForReading] readDataToEndOfFile];
            [task waitUntilExit];
            status = [task terminationStatus];
        } @catch (NSException *e) {
            out = [[e reason] dataUsingEncoding:NSUTF8StringEncoding];
        }
        NSString *text = [[[NSString alloc] initWithData:out
                            encoding:NSUTF8StringEncoding] autorelease];
        [task release];

        dispatch_async(dispatch_get_main_queue(), ^{
            NSAlert *a = [[[NSAlert alloc] init] autorelease];
            [a setMessageText:status == 0
                ? [NSString stringWithFormat:@"Installed %@",
                   [path lastPathComponent]]
                : [NSString stringWithFormat:@"Could not install %@",
                   [path lastPathComponent]]];
            /* The script's own diagnosis, verbatim -- it is the useful part. */
            [a setInformativeText:text ?: @"(no output)"];
            [a runModal];
        });
    });
}

- (NSDragOperation) draggingEntered:(id <NSDraggingInfo>)sender
{
    for (NSURL *u in [[sender draggingPasteboard]
                      readObjectsForClasses:@[[NSURL class]]
                      options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}]) {
        if ([[[u pathExtension] lowercaseString] isEqualToString:@"ipa"]) {
            return NSDragOperationCopy;
        }
    }
    return NSDragOperationNone;
}

- (BOOL) performDragOperation:(id <NSDraggingInfo>)sender
{
    for (NSURL *u in [[sender draggingPasteboard]
                      readObjectsForClasses:@[[NSURL class]]
                      options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}]) {
        if ([[[u pathExtension] lowercaseString] isEqualToString:@"ipa"]) {
            [self installIPA:[u path]];
            return YES;
        }
    }
    return NO;
}

- (void) setFullGrab:(id)sender
{
    COCOA_DEBUG("QemuCocoaView: setFullGrab\n");

    CGEventMask mask = CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp) | CGEventMaskBit(kCGEventFlagsChanged);
    eventsTap = CGEventTapCreate(kCGHIDEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault,
                                 mask, handleTapEvent, self);
    if (!eventsTap) {
        warn_report("Could not create event tap, system key combos will not be captured.\n");
        return;
    } else {
        COCOA_DEBUG("Global events tap created! Will capture system key combos.\n");
    }

    CFRunLoopRef runLoop = CFRunLoopGetCurrent();
    if (!runLoop) {
        warn_report("Could not obtain current CF RunLoop, system key combos will not be captured.\n");
        return;
    }

    CFRunLoopSourceRef tapEventsSrc = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, eventsTap, 0);
    if (!tapEventsSrc ) {
        warn_report("Could not obtain current CF RunLoop, system key combos will not be captured.\n");
        return;
    }

    CFRunLoopAddSource(runLoop, tapEventsSrc, kCFRunLoopDefaultMode);
    CFRelease(tapEventsSrc);
}

- (void) toggleKey: (int)keycode {
    qkbd_state_key_event(kbd, keycode, !qkbd_state_key_get(kbd, keycode));
}

// Does the work of sending input to the monitor
- (void) handleMonitorInput:(NSEvent *)event
{
    int keysym = 0;
    int control_key = 0;

    // if the control key is down
    if ([event modifierFlags] & NSEventModifierFlagControl) {
        control_key = 1;
    }

    /* translates Macintosh keycodes to QEMU's keysym */

    static const int without_control_translation[] = {
        [0 ... 0xff] = 0,   // invalid key

        [kVK_UpArrow]       = QEMU_KEY_UP,
        [kVK_DownArrow]     = QEMU_KEY_DOWN,
        [kVK_RightArrow]    = QEMU_KEY_RIGHT,
        [kVK_LeftArrow]     = QEMU_KEY_LEFT,
        [kVK_Home]          = QEMU_KEY_HOME,
        [kVK_End]           = QEMU_KEY_END,
        [kVK_PageUp]        = QEMU_KEY_PAGEUP,
        [kVK_PageDown]      = QEMU_KEY_PAGEDOWN,
        [kVK_ForwardDelete] = QEMU_KEY_DELETE,
        [kVK_Delete]        = QEMU_KEY_BACKSPACE,
    };

    static const int with_control_translation[] = {
        [0 ... 0xff] = 0,   // invalid key

        [kVK_UpArrow]       = QEMU_KEY_CTRL_UP,
        [kVK_DownArrow]     = QEMU_KEY_CTRL_DOWN,
        [kVK_RightArrow]    = QEMU_KEY_CTRL_RIGHT,
        [kVK_LeftArrow]     = QEMU_KEY_CTRL_LEFT,
        [kVK_Home]          = QEMU_KEY_CTRL_HOME,
        [kVK_End]           = QEMU_KEY_CTRL_END,
        [kVK_PageUp]        = QEMU_KEY_CTRL_PAGEUP,
        [kVK_PageDown]      = QEMU_KEY_CTRL_PAGEDOWN,
    };

    if (control_key != 0) { /* If the control key is being used */
        if ([event keyCode] < ARRAY_SIZE(with_control_translation)) {
            keysym = with_control_translation[[event keyCode]];
        }
    } else {
        if ([event keyCode] < ARRAY_SIZE(without_control_translation)) {
            keysym = without_control_translation[[event keyCode]];
        }
    }

    // if not a key that needs translating
    if (keysym == 0) {
        NSString *ks = [event characters];
        if ([ks length] > 0) {
            keysym = [ks characterAtIndex:0];
        }
    }

    if (keysym) {
        QemuTextConsole *con = QEMU_TEXT_CONSOLE(dcl.con);
        qemu_text_console_put_keysym(con, keysym);
    }
}

- (bool) handleEvent:(NSEvent *)event
{
    return bool_with_bql(^{
        return [self handleEventLocked:event];
    });
}

- (bool) handleEventLocked:(NSEvent *)event
{
    /* Return true if we handled the event, false if it should be given to OSX */
    COCOA_DEBUG("QemuCocoaView: handleEvent\n");
    InputButton button;
    int keycode = 0;
    NSUInteger modifiers = [event modifierFlags];

    /*
     * Check -[NSEvent modifierFlags] here.
     *
     * There is a NSEventType for an event notifying the change of
     * -[NSEvent modifierFlags], NSEventTypeFlagsChanged but these operations
     * are performed for any events because a modifier state may change while
     * the application is inactive (i.e. no events fire) and we don't want to
     * wait for another modifier state change to detect such a change.
     *
     * NSEventModifierFlagCapsLock requires a special treatment. The other flags
     * are handled in similar manners.
     *
     * NSEventModifierFlagCapsLock
     * ---------------------------
     *
     * If CapsLock state is changed, "up" and "down" events will be fired in
     * sequence, effectively updates CapsLock state on the guest.
     *
     * The other flags
     * ---------------
     *
     * If a flag is not set, fire "up" events for all keys which correspond to
     * the flag. Note that "down" events are not fired here because the flags
     * checked here do not tell what exact keys are down.
     *
     * If one of the keys corresponding to a flag is down, we rely on
     * -[NSEvent keyCode] of an event whose -[NSEvent type] is
     * NSEventTypeFlagsChanged to know the exact key which is down, which has
     * the following two downsides:
     * - It does not work when the application is inactive as described above.
     * - It malfactions *after* the modifier state is changed while the
     *   application is inactive. It is because -[NSEvent keyCode] does not tell
     *   if the key is up or down, and requires to infer the current state from
     *   the previous state. It is still possible to fix such a malfanction by
     *   completely leaving your hands from the keyboard, which hopefully makes
     *   this implementation usable enough.
     */
    if (!!(modifiers & NSEventModifierFlagCapsLock) !=
        qkbd_state_modifier_get(kbd, QKBD_MOD_CAPSLOCK)) {
        qkbd_state_key_event(kbd, Q_KEY_CODE_CAPS_LOCK, true);
        qkbd_state_key_event(kbd, Q_KEY_CODE_CAPS_LOCK, false);
    }

    if (!(modifiers & NSEventModifierFlagShift)) {
        qkbd_state_key_event(kbd, Q_KEY_CODE_SHIFT, false);
        qkbd_state_key_event(kbd, Q_KEY_CODE_SHIFT_R, false);
    }
    if (!(modifiers & NSEventModifierFlagControl)) {
        qkbd_state_key_event(kbd, Q_KEY_CODE_CTRL, false);
        qkbd_state_key_event(kbd, Q_KEY_CODE_CTRL_R, false);
    }
    if (!(modifiers & NSEventModifierFlagOption)) {
        if (swap_opt_cmd) {
            qkbd_state_key_event(kbd, Q_KEY_CODE_META_L, false);
            qkbd_state_key_event(kbd, Q_KEY_CODE_META_R, false);
        } else {
            qkbd_state_key_event(kbd, Q_KEY_CODE_ALT, false);
            qkbd_state_key_event(kbd, Q_KEY_CODE_ALT_R, false);
        }
    }
    if (!(modifiers & NSEventModifierFlagCommand)) {
        if (swap_opt_cmd) {
            qkbd_state_key_event(kbd, Q_KEY_CODE_ALT, false);
            qkbd_state_key_event(kbd, Q_KEY_CODE_ALT_R, false);
        } else {
            qkbd_state_key_event(kbd, Q_KEY_CODE_META_L, false);
            qkbd_state_key_event(kbd, Q_KEY_CODE_META_R, false);
        }
    }

    /*
     * Option/Shift going down or up must move the synthesised second finger by
     * itself -- letting go of Option is how you lift it, and that comes with no
     * pointer motion at all.
     */
    [self modifiersChanged:modifiers];

    switch ([event type]) {
        case NSEventTypeFlagsChanged:
            switch ([event keyCode]) {
                case kVK_Shift:
                    if (!!(modifiers & NSEventModifierFlagShift)) {
                        [self toggleKey:Q_KEY_CODE_SHIFT];
                    }
                    break;

                case kVK_RightShift:
                    if (!!(modifiers & NSEventModifierFlagShift)) {
                        [self toggleKey:Q_KEY_CODE_SHIFT_R];
                    }
                    break;

                case kVK_Control:
                    if (!!(modifiers & NSEventModifierFlagControl)) {
                        [self toggleKey:Q_KEY_CODE_CTRL];
                    }
                    break;

                case kVK_RightControl:
                    if (!!(modifiers & NSEventModifierFlagControl)) {
                        [self toggleKey:Q_KEY_CODE_CTRL_R];
                    }
                    break;

                case kVK_Option:
                    if (!!(modifiers & NSEventModifierFlagOption)) {
                        if (swap_opt_cmd) {
                            [self toggleKey:Q_KEY_CODE_META_L];
                        } else {
                            [self toggleKey:Q_KEY_CODE_ALT];
                        }
                    }
                    break;

                case kVK_RightOption:
                    if (!!(modifiers & NSEventModifierFlagOption)) {
                        if (swap_opt_cmd) {
                            [self toggleKey:Q_KEY_CODE_META_R];
                        } else {
                            [self toggleKey:Q_KEY_CODE_ALT_R];
                        }
                    }
                    break;

                /* Don't pass command key changes to guest unless mouse is grabbed */
                case kVK_Command:
                    if (isMouseGrabbed &&
                        !!(modifiers & NSEventModifierFlagCommand) &&
                        left_command_key_enabled) {
                        if (swap_opt_cmd) {
                            [self toggleKey:Q_KEY_CODE_ALT];
                        } else {
                            [self toggleKey:Q_KEY_CODE_META_L];
                        }
                    }
                    break;

                case kVK_RightCommand:
                    if (isMouseGrabbed &&
                        !!(modifiers & NSEventModifierFlagCommand)) {
                        if (swap_opt_cmd) {
                            [self toggleKey:Q_KEY_CODE_ALT_R];
                        } else {
                            [self toggleKey:Q_KEY_CODE_META_R];
                        }
                    }
                    break;
            }
            return true;
        case NSEventTypeKeyDown:
            keycode = cocoa_keycode_to_qemu([event keyCode]);

            // forward command key combos to the host UI unless the mouse is grabbed
            if (!isMouseGrabbed && ([event modifierFlags] & NSEventModifierFlagCommand)) {
                return false;
            }

            // default

            // handle control + alt Key Combos (ctrl+alt+[1..9,g] is reserved for QEMU)
            if (([event modifierFlags] & NSEventModifierFlagControl) && ([event modifierFlags] & NSEventModifierFlagOption)) {
                NSString *keychar = [event charactersIgnoringModifiers];
                if ([keychar length] == 1) {
                    char key = [keychar characterAtIndex:0];
                    switch (key) {

                        // enable graphic console
                        case '1' ... '9':
                            [self selectConsoleLocked:key - '0' - 1]; /* ascii math */
                            return true;

                        // release the mouse grab
                        case 'g':
                            [self ungrabMouse];
                            return true;
                    }
                }
            }

            if (qemu_console_is_graphic(dcl.con)) {
                qkbd_state_key_event(kbd, keycode, true);
            } else {
                [self handleMonitorInput: event];
            }
            return true;
        case NSEventTypeKeyUp:
            keycode = cocoa_keycode_to_qemu([event keyCode]);

            // don't pass the guest a spurious key-up if we treated this
            // command-key combo as a host UI action
            if (!isMouseGrabbed && ([event modifierFlags] & NSEventModifierFlagCommand)) {
                return true;
            }

            if (qemu_console_is_graphic(dcl.con)) {
                qkbd_state_key_event(kbd, keycode, false);
            }
            return true;
        case NSEventTypeScrollWheel:
            /*
             * Send wheel events to the guest regardless of window focus.
             * This is in-line with standard Mac OS X UI behaviour.
             */

            /* Determine if this is a scroll up or scroll down event */
            if ([event deltaY] != 0) {
                button = ([event deltaY] > 0) ?
                    INPUT_BUTTON_WHEEL_UP : INPUT_BUTTON_WHEEL_DOWN;
            } else if ([event deltaX] != 0) {
                button = ([event deltaX] > 0) ?
                    INPUT_BUTTON_WHEEL_LEFT : INPUT_BUTTON_WHEEL_RIGHT;
            } else {
                /*
                 * We shouldn't have got a scroll event when deltaY and delta Y
                 * are zero, hence no harm in dropping the event
                 */
                return true;
            }

            qemu_input_queue_btn(dcl.con, button, true);
            qemu_input_event_sync();
            qemu_input_queue_btn(dcl.con, button, false);
            qemu_input_event_sync();

            return true;
        default:
            return false;
    }
}

/*
 * The second finger of an Option-drag, driven from one host pointer.
 *
 * This is the iPhone Simulator's idiom, kept deliberately identical because it
 * is the one people already have in their fingers:
 *
 *   Option        the second contact is the pointer reflected through the
 *                 CENTRE of the screen -- (2cx - x, 2cy - y). A symmetric
 *                 pinch: dragging towards the middle closes it, away opens it.
 *   Option+Shift  the pair translates rigidly. The offset between the two
 *                 contacts is frozen and carried onto each new pointer
 *                 position, so the spread you set with Option alone slides
 *                 across the screen without changing.
 *
 * A modifier change on its own must re-emit -- that is why previousModifierFlags
 * exists, and why this is called from the flags-changed path with no motion at
 * all. Releasing Option has to LIFT the second finger; if it only stopped
 * updating it, the guest would be left holding a contact that no pointer
 * movement can ever end.
 *
 * Contacts are pushed as QEMU multi-touch events on slot 1. Slot 0 stays on the
 * ordinary pointer path, which the digitizer already maps to finger 0, so
 * one-finger behaviour is untouched by everything here.
 *
 * mousePoint/pinchPoint are guest framebuffer coordinates. Callers update
 * mousePoint (and prevMousePoint) first; this only decides what finger 1 does.
 */
/*
 * Where finger 1 goes for the current pointer position. Shared by the real
 * contact and by the hover preview, so the dot you line up before pressing is
 * by construction the dot you get when you press.
 */
- (NSPoint) pinchPartnerFor:(NSUInteger)modifiers
{
    NSPoint p;

    if (pinchDown && (modifiers & NSEventModifierFlagShift)) {
        /* Rigid translation: carry the existing separation to the new point. */
        p.x = mousePoint.x + (pinchPoint.x - prevMousePoint.x);
        p.y = mousePoint.y + (pinchPoint.y - prevMousePoint.y);
    } else {
        p.x = screen.width - mousePoint.x;
        p.y = screen.height - mousePoint.y;
    }
    return p;
}

- (void) updatePinch:(NSUInteger)modifiers
{
    BOOL wantsPinch = (modifiers & NSEventModifierFlagOption) != 0 &&
                      leftButtonDown && haveMousePoint && isAbsoluteEnabled;
    NSPoint p;

    if (!wantsPinch) {
        if (pinchDown) {
            qemu_input_queue_mtt(dcl.con, INPUT_MULTI_TOUCH_TYPE_END, 1, 1);
            qemu_input_event_sync();
            pinchDown = FALSE;
        }
        [self refreshTapMarkers];
        return;
    }

    p = [self pinchPartnerFor:modifiers];

    /*
     * Leaving the panel is a transition, not a stuck touch. The mirrored finger
     * runs off the edge long before the real one does, and the digitizer would
     * end the contact anyway -- doing it here as well keeps the host's idea of
     * what is down in step with the guest's, so the contact can be started
     * again cleanly if the pointer brings it back on screen.
     */
    if (p.x < 0 || p.y < 0 || p.x >= screen.width || p.y >= screen.height) {
        if (pinchDown) {
            qemu_input_queue_mtt(dcl.con, INPUT_MULTI_TOUCH_TYPE_END, 1, 1);
            qemu_input_event_sync();
            pinchDown = FALSE;
        }
        [self refreshTapMarkers];
        return;
    }

    qemu_input_queue_mtt_abs(dcl.con, INPUT_AXIS_X, p.x, 0, screen.width, 1, 1);
    qemu_input_queue_mtt_abs(dcl.con, INPUT_AXIS_Y, p.y, 0, screen.height, 1, 1);
    qemu_input_queue_mtt(dcl.con,
                         pinchDown ? INPUT_MULTI_TOUCH_TYPE_UPDATE
                                   : INPUT_MULTI_TOUCH_TYPE_BEGIN, 1, 1);
    qemu_input_event_sync();

    pinchPoint = p;
    pinchDown = TRUE;
    [self refreshTapMarkers];
}

/* Re-emit on a modifier change alone, with the pointer exactly where it was. */
- (void) modifiersChanged:(NSUInteger)modifiers
{
    NSUInteger interesting = NSEventModifierFlagOption | NSEventModifierFlagShift;

    if ((modifiers & interesting) == (previousModifierFlags & interesting)) {
        return;
    }
    previousModifierFlags = modifiers;
    prevMousePoint = mousePoint;
    [self updatePinch:modifiers];
}

- (void) handleMouseEvent:(NSEvent *)event button:(InputButton)button down:(bool)down
{
    if (!isMouseGrabbed) {
        return;
    }

    if (button == INPUT_BUTTON_LEFT) {
        leftButtonDown = down;
    }

    with_bql(^{
        qemu_input_queue_btn(dcl.con, button, down);
    });

    [self handleMouseEvent:event];
}

- (void) handleMouseEvent:(NSEvent *)event
{
    if (!isMouseGrabbed) {
        return;
    }

    with_bql(^{
        if (isAbsoluteEnabled) {
            CGFloat d = (CGFloat)screen.height / [self frame].size.height;
            NSPoint p = [event locationInWindow];

            /* Note that the origin for Cocoa mouse coords is bottom left, not top left. */
            qemu_input_queue_abs(dcl.con, INPUT_AXIS_X, p.x * d, 0, screen.width);
            qemu_input_queue_abs(dcl.con, INPUT_AXIS_Y, screen.height - p.y * d, 0, screen.height);

            prevMousePoint = haveMousePoint ? mousePoint
                                            : NSMakePoint(p.x * d, screen.height - p.y * d);
            mousePoint = NSMakePoint(p.x * d, screen.height - p.y * d);
            haveMousePoint = TRUE;
        } else {
            qemu_input_queue_rel(dcl.con, INPUT_AXIS_X, [event deltaX]);
            qemu_input_queue_rel(dcl.con, INPUT_AXIS_Y, [event deltaY]);
        }

        qemu_input_event_sync();

        /*
         * After the pointer, so finger 0's new position is already in the frame
         * finger 1's update produces. The other order makes finger 0 lag by one
         * event whenever the digitizer coalesces to its report rate.
         */
        previousModifierFlags = [event modifierFlags];
        [self updatePinch:previousModifierFlags];
    });

    /* -updatePinch: only redraws when finger 1 moves; finger 0 moved too. */
    [self refreshTapMarkers];
}

- (void) mouseExited:(NSEvent *)event
{
    if (isAbsoluteEnabled && isMouseGrabbed) {
        [self ungrabMouse];
    }
}

- (void) mouseEntered:(NSEvent *)event
{
    if (isAbsoluteEnabled && !isMouseGrabbed) {
        [self grabMouse];
    }
}

- (void) mouseMoved:(NSEvent *)event
{
    [self handleMouseEvent:event];
}

- (void) mouseDown:(NSEvent *)event
{
    [self handleMouseEvent:event button:INPUT_BUTTON_LEFT down:true];
}

- (void) rightMouseDown:(NSEvent *)event
{
    [self handleMouseEvent:event button:INPUT_BUTTON_RIGHT down:true];
}

- (void) otherMouseDown:(NSEvent *)event
{
    [self handleMouseEvent:event button:INPUT_BUTTON_MIDDLE down:true];
}

- (void) mouseDragged:(NSEvent *)event
{
    [self handleMouseEvent:event];
}

- (void) rightMouseDragged:(NSEvent *)event
{
    [self handleMouseEvent:event];
}

- (void) otherMouseDragged:(NSEvent *)event
{
    [self handleMouseEvent:event];
}

- (void) mouseUp:(NSEvent *)event
{
    if (!isMouseGrabbed) {
        [self grabMouse];
    }

    [self handleMouseEvent:event button:INPUT_BUTTON_LEFT down:false];
}

- (void) rightMouseUp:(NSEvent *)event
{
    [self handleMouseEvent:event button:INPUT_BUTTON_RIGHT down:false];
}

- (void) otherMouseUp:(NSEvent *)event
{
    [self handleMouseEvent:event button:INPUT_BUTTON_MIDDLE down:false];
}

- (void) grabMouse
{
    COCOA_DEBUG("QemuCocoaView: grabMouse\n");

    if (qemu_name)
        [[self window] setTitle:[NSString stringWithFormat:@"QEMU %s - (Press  " UC_CTRL_KEY " " UC_ALT_KEY " G  to release Mouse)", qemu_name]];
    else
        [[self window] setTitle:@"QEMU - (Press  " UC_CTRL_KEY " " UC_ALT_KEY " G  to release Mouse)"];
    [self hideCursor];
    CGAssociateMouseAndMouseCursorPosition(isAbsoluteEnabled);
    isMouseGrabbed = TRUE; // while isMouseGrabbed = TRUE, QemuCocoaApp sends all events to [cocoaView handleEvent:]
}

- (void) ungrabMouse
{
    COCOA_DEBUG("QemuCocoaView: ungrabMouse\n");

    if (qemu_name)
        [[self window] setTitle:[NSString stringWithFormat:@"QEMU %s", qemu_name]];
    else
        [[self window] setTitle:@"QEMU"];
    [self unhideCursor];
    CGAssociateMouseAndMouseCursorPosition(TRUE);
    isMouseGrabbed = FALSE;
    [self raiseAllButtons];
}

- (void) notifyMouseModeChange {
    bool tIsAbsoluteEnabled = bool_with_bql(^{
        return qemu_input_is_absolute(dcl.con);
    });

    if (tIsAbsoluteEnabled == isAbsoluteEnabled) {
        return;
    }

    isAbsoluteEnabled = tIsAbsoluteEnabled;

    if (isMouseGrabbed) {
        if (isAbsoluteEnabled) {
            [self ungrabMouse];
        } else {
            CGAssociateMouseAndMouseCursorPosition(isAbsoluteEnabled);
        }
    }
}
- (BOOL) isMouseGrabbed {return isMouseGrabbed;}
- (QEMUScreen) gscreen {return screen;}

/*
 * Makes the target think all down keys are being released.
 * This prevents a stuck key problem, since we will not see
 * key up events for those keys after we have lost focus.
 */
- (void) raiseAllKeys
{
    with_bql(^{
        qkbd_state_lift_all_keys(kbd);
    });
}

- (void) raiseAllButtons
{
    with_bql(^{
        qemu_input_queue_btn(dcl.con, INPUT_BUTTON_LEFT, false);
        qemu_input_queue_btn(dcl.con, INPUT_BUTTON_RIGHT, false);
        qemu_input_queue_btn(dcl.con, INPUT_BUTTON_MIDDLE, false);
        /*
         * Losing the grab is the other way an Option-drag can end. The pointer
         * is gone, so nothing else will ever lift finger 1.
         */
        leftButtonDown = FALSE;
        [self updatePinch:0];
    });
}
@end



/*
 ------------------------------------------------------
    QemuCocoaAppController
 ------------------------------------------------------
*/
@interface QemuCocoaAppController : NSObject
                                       <NSWindowDelegate, NSApplicationDelegate>
{
}
- (void)doToggleFullScreen:(id)sender;
- (void)showQEMUDoc:(id)sender;
- (void)zoomToFit:(id) sender;
- (void)displayConsole:(id)sender;
- (void)pauseQEMU:(id)sender;
- (void)resumeQEMU:(id)sender;
- (void)displayPause;
- (void)removePause;
- (void)restartQEMU:(id)sender;
- (void)powerDownQEMU:(id)sender;
- (void)pasteTextToGuest:(id)sender;
- (void)ejectDeviceMedia:(id)sender;
- (void)changeDeviceMedia:(id)sender;
- (BOOL)verifyQuit;
- (void)openDocumentation:(NSString *)filename;
- (IBAction) do_about_menu_item: (id) sender;
- (void)adjustSpeed:(id)sender;
- (void)copyScreen:(id)sender;
- (void)deviceButton:(id)sender;
- (void)installApp:(id)sender;
- (void)openTerminal:(id)sender;
- (void)eraseDevice:(id)sender;
@end

@implementation QemuCocoaAppController
- (id) init
{
    NSWindow *window;

    COCOA_DEBUG("QemuCocoaAppController: init\n");

    self = [super init];
    if (self) {

        // create a view and add it to the window
        cocoaView = [[QemuCocoaView alloc] initWithFrame:NSMakeRect(0.0, 0.0, 640.0, 480.0)];
        if(!cocoaView) {
            error_report("(cocoa) can't create a view");
            exit(1);
        }

        // create a window
        window = [[NSWindow alloc] initWithContentRect:[cocoaView frame]
            styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskClosable
            backing:NSBackingStoreBuffered defer:NO];
        if(!window) {
            error_report("(cocoa) can't create window");
            exit(1);
        }
        [window setAcceptsMouseMovedEvents:YES];
        [window setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];
        [window setTitle:qemu_name ? [NSString stringWithFormat:@"QEMU %s", qemu_name] : @"QEMU"];
        [window setContentView:cocoaView];
        [window makeKeyAndOrderFront:self];
        [window center];
        [window setDelegate: self];

        /* Used for displaying pause on the screen */
        pauseLabel = [NSTextField new];
        [pauseLabel setBezeled:YES];
        [pauseLabel setDrawsBackground:YES];
        [pauseLabel setBackgroundColor: [NSColor whiteColor]];
        [pauseLabel setEditable:NO];
        [pauseLabel setSelectable:NO];
        [pauseLabel setStringValue: @"Paused"];
        [pauseLabel setFont: [NSFont fontWithName: @"Helvetica" size: 90]];
        [pauseLabel setTextColor: [NSColor blackColor]];
        [pauseLabel sizeToFit];
    }
    return self;
}

- (void) dealloc
{
    COCOA_DEBUG("QemuCocoaAppController: dealloc\n");

    [cocoaView release];
    [cbowner release];
    cbowner = nil;

    [super dealloc];
}

- (void)applicationDidFinishLaunching: (NSNotification *) note
{
    COCOA_DEBUG("QemuCocoaAppController: applicationDidFinishLaunching\n");
    allow_events = true;
}

- (void)applicationWillTerminate:(NSNotification *)aNotification
{
    COCOA_DEBUG("QemuCocoaAppController: applicationWillTerminate\n");

    with_bql(^{
        shutdown_action = SHUTDOWN_ACTION_POWEROFF;
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
    });

    /*
     * Sleep here, because returning will cause OSX to kill us
     * immediately; the QEMU main loop will handle the shutdown
     * request and terminate the process.
     */
    [NSThread sleepForTimeInterval:INFINITY];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)theApplication
{
    return YES;
}

- (NSApplicationTerminateReply)applicationShouldTerminate:
                                                         (NSApplication *)sender
{
    COCOA_DEBUG("QemuCocoaAppController: applicationShouldTerminate\n");
    return [self verifyQuit];
}

- (void)windowDidChangeScreen:(NSNotification *)notification
{
    [cocoaView updateUIInfo];
}

- (void)windowDidEnterFullScreen:(NSNotification *)notification
{
    [cocoaView grabMouse];
}

- (void)windowDidExitFullScreen:(NSNotification *)notification
{
    [cocoaView resizeWindow];
    [cocoaView ungrabMouse];
}

- (void)windowDidResize:(NSNotification *)notification
{
    [cocoaView updateBounds];
    [cocoaView updateUIInfo];
}

/* Called when the user clicks on a window's close button */
- (BOOL)windowShouldClose:(id)sender
{
    COCOA_DEBUG("QemuCocoaAppController: windowShouldClose\n");
    [NSApp terminate: sender];
    /* If the user allows the application to quit then the call to
     * NSApp terminate will never return. If we get here then the user
     * cancelled the quit, so we should return NO to not permit the
     * closing of this window.
     */
    return NO;
}

- (NSApplicationPresentationOptions) window:(NSWindow *)window
                                     willUseFullScreenPresentationOptions:(NSApplicationPresentationOptions)proposedOptions;

{
    return (proposedOptions & ~(NSApplicationPresentationAutoHideDock | NSApplicationPresentationAutoHideMenuBar)) |
           NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar;
}

/*
 * Called when QEMU goes into the background. Note that
 * [-NSWindowDelegate windowDidResignKey:] is used here instead of
 * [-NSApplicationDelegate applicationWillResignActive:] because it cannot
 * detect that the window loses focus when the deck is clicked on macOS 13.2.1.
 */
- (void) windowDidResignKey: (NSNotification *)aNotification
{
    COCOA_DEBUG("%s\n", __func__);
    [cocoaView ungrabMouse];
    [cocoaView raiseAllKeys];
}

/* We abstract the method called by the Enter Fullscreen menu item
 * because Mac OS 10.7 and higher disables it. This is because of the
 * menu item's old selector's name toggleFullScreen:
 */
- (void) doToggleFullScreen:(id)sender
{
    [[cocoaView window] toggleFullScreen:sender];
}

- (void) setFullGrab:(id)sender
{
    COCOA_DEBUG("QemuCocoaAppController: setFullGrab\n");

    [cocoaView setFullGrab:sender];
}

/* Tries to find then open the specified filename */
- (void) openDocumentation: (NSString *) filename
{
    /* Where to look for local files */
    NSString *path_array[] = {@"../share/doc/qemu/", @"../doc/qemu/", @"docs/"};
    NSString *full_file_path;
    NSURL *full_file_url;

    /* iterate thru the possible paths until the file is found */
    int index;
    for (index = 0; index < ARRAY_SIZE(path_array); index++) {
        full_file_path = [[NSBundle mainBundle] executablePath];
        full_file_path = [full_file_path stringByDeletingLastPathComponent];
        full_file_path = [NSString stringWithFormat: @"%@/%@%@", full_file_path,
                          path_array[index], filename];
        full_file_url = [NSURL fileURLWithPath: full_file_path
                                   isDirectory: false];
        if ([[NSWorkspace sharedWorkspace] openURL: full_file_url] == YES) {
            return;
        }
    }

    /* If none of the paths opened a file */
    NSBeep();
    QEMU_Alert(@"Failed to open file");
}

- (void)showQEMUDoc:(id)sender
{
    COCOA_DEBUG("QemuCocoaAppController: showQEMUDoc\n");

    [self openDocumentation: @"index.html"];
}

/* Stretches video to fit host monitor size */
- (void)zoomToFit:(id) sender
{
    NSWindowStyleMask styleMask = [[cocoaView window] styleMask] ^ NSWindowStyleMaskResizable;

    [[cocoaView window] setStyleMask:styleMask];
    [sender setState:styleMask & NSWindowStyleMaskResizable ? NSControlStateValueOn : NSControlStateValueOff];
    [cocoaView resizeWindow];
}

- (void)toggleZoomInterpolation:(id) sender
{
    if (zoom_interpolation == kCGInterpolationNone) {
        zoom_interpolation = kCGInterpolationLow;
        [sender setState: NSControlStateValueOn];
    } else {
        zoom_interpolation = kCGInterpolationNone;
        [sender setState: NSControlStateValueOff];
    }
}

/* Displays the console on the screen */
- (void)displayConsole:(id)sender
{
    with_bql(^{
        [cocoaView selectConsoleLocked:[sender tag]];
    });
}

/* Pause the guest */
- (void)pauseQEMU:(id)sender
{
    with_bql(^{
        qmp_stop(NULL);
    });
    [sender setEnabled: NO];
    [[[sender menu] itemWithTitle: @"Resume"] setEnabled: YES];
    [self displayPause];
}

/* Resume running the guest operating system */
- (void)resumeQEMU:(id) sender
{
    with_bql(^{
        qmp_cont(NULL);
    });
    [sender setEnabled: NO];
    [[[sender menu] itemWithTitle: @"Pause"] setEnabled: YES];
    [self removePause];
}

/* Displays the word pause on the screen */
- (void)displayPause
{
    /* Coordinates have to be calculated each time because the window can change its size */
    int xCoord, yCoord, width, height;
    xCoord = ([cocoaView frame].size.width - [pauseLabel frame].size.width)/2;
    yCoord = [cocoaView frame].size.height - [pauseLabel frame].size.height - ([pauseLabel frame].size.height * .5);
    width = [pauseLabel frame].size.width;
    height = [pauseLabel frame].size.height;
    [pauseLabel setFrame: NSMakeRect(xCoord, yCoord, width, height)];
    [cocoaView addSubview: pauseLabel];
}

/* Removes the word pause from the screen */
- (void)removePause
{
    [pauseLabel removeFromSuperview];
}

/* Restarts QEMU */
- (void)restartQEMU:(id)sender
{
    with_bql(^{
        qmp_system_reset(NULL);
    });
}

/* Powers down QEMU */
- (void)powerDownQEMU:(id)sender
{
    with_bql(^{
        qmp_system_powerdown(NULL);
    });
}

/*
 * Hand the host clipboard's text to the guest.
 *
 * This is deliberately an explicit gesture rather than a background sync, the
 * way the iPhone Simulator's Edit > Paste Text was: the guest pasteboard is a
 * single slot the user is about to paste from, and silently replacing it every
 * time something is copied on the Mac would be a surprise.
 *
 * It goes through the machine's "pasteboard" property rather than a direct
 * call, so this and the QMP path (qom-set) are the same code -- and headless
 * runs, which have no clipboard peer at all, are not a second implementation.
 */
- (void)pasteTextToGuest:(id)sender
{
    NSString *text = [[NSPasteboard generalPasteboard]
                      stringForType:NSPasteboardTypeString];
    if (text == nil) {
        NSBeep();
        return;
    }

    const char *utf8 = [text UTF8String];
    __block Error *err = NULL;
    with_bql(^{
        Object *machine = qdev_get_machine();
        if (machine && object_property_find(machine, "pasteboard")) {
            object_property_set_str(machine, "pasteboard", utf8, &err);
        } else {
            error_setg(&err, "this machine has no guest pasteboard");
        }
    });
    if (err) {
        QEMU_Alert([NSString stringWithUTF8String: error_get_pretty(err)]);
        error_free(err);
    }
}

/* Ejects the media.
 * Uses sender's tag to figure out the device to eject.
 */
- (void)ejectDeviceMedia:(id)sender
{
    NSString * drive;
    drive = [sender representedObject];
    if(drive == nil) {
        NSBeep();
        QEMU_Alert(@"Failed to find drive to eject!");
        return;
    }

    __block Error *err = NULL;
    with_bql(^{
        qmp_eject([drive cStringUsingEncoding: NSASCIIStringEncoding],
                  NULL, false, false, &err);
    });
    handleAnyDeviceErrors(err);
}

/* Displays a dialog box asking the user to select an image file to load.
 * Uses sender's represented object value to figure out which drive to use.
 */
- (void)changeDeviceMedia:(id)sender
{
    /* Find the drive name */
    NSString * drive;
    drive = [sender representedObject];
    if(drive == nil) {
        NSBeep();
        QEMU_Alert(@"Could not find drive!");
        return;
    }

    /* Display the file open dialog */
    NSOpenPanel * openPanel;
    openPanel = [NSOpenPanel openPanel];
    [openPanel setCanChooseFiles: YES];
    [openPanel setAllowsMultipleSelection: NO];
    if([openPanel runModal] == NSModalResponseOK) {
        NSString * file = [[[openPanel URLs] objectAtIndex: 0] path];
        if(file == nil) {
            NSBeep();
            QEMU_Alert(@"Failed to convert URL to file path!");
            return;
        }

        __block Error *err = NULL;
        with_bql(^{
            qmp_blockdev_change_medium([drive cStringUsingEncoding:
                                                  NSASCIIStringEncoding],
                                       NULL,
                                       [file cStringUsingEncoding:
                                                 NSASCIIStringEncoding],
                                       "raw",
                                       true, false,
                                       false, 0,
                                       &err);
        });
        handleAnyDeviceErrors(err);
    }
}

/* Verifies if the user really wants to quit */
- (BOOL)verifyQuit
{
    NSAlert *alert = [NSAlert new];
    [alert autorelease];
    [alert setMessageText: @"Are you sure you want to quit QEMU?"];
    [alert addButtonWithTitle: @"Cancel"];
    [alert addButtonWithTitle: @"Quit"];
    if([alert runModal] == NSAlertSecondButtonReturn) {
        return YES;
    } else {
        return NO;
    }
}

/* The action method for the About menu item */
- (IBAction) do_about_menu_item: (id) sender
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    char *icon_path_c = get_relocated_path(CONFIG_QEMU_ICONDIR "/hicolor/512x512/apps/qemu.png");
    NSString *icon_path = [NSString stringWithUTF8String:icon_path_c];
    g_free(icon_path_c);
    NSImage *icon = [[NSImage alloc] initWithContentsOfFile:icon_path];
    NSString *version = @"QEMU emulator version " QEMU_FULL_VERSION;
    NSString *copyright = @QEMU_COPYRIGHT;
    NSDictionary *options;
    if (icon) {
        options = @{
            NSAboutPanelOptionApplicationIcon : icon,
            NSAboutPanelOptionApplicationVersion : version,
            @"Copyright" : copyright,
        };
        [icon release];
    } else {
        options = @{
            NSAboutPanelOptionApplicationVersion : version,
            @"Copyright" : copyright,
        };
    }
    [NSApp orderFrontStandardAboutPanelWithOptions:options];
    [pool release];
}

/* Edit ▸ Copy Screen. Apple's key equivalent, kept exactly. */
- (void)copyScreen:(id)sender
{
    [cocoaView copyScreenToPasteboard];
}

/*
 * Device menu tags. The hardware buttons already exist as Command-gated key
 * chords (see ipod_touch_kbd_button), and the menu drives that same path rather
 * than a second one into the GPIOs -- so anything true of the keyboard shortcut
 * stays true of the menu item, including the release-always guarantee.
 */
enum {
    IT_BTN_HOME = 1,
    IT_BTN_POWER,
    IT_BTN_VOLUP,
    IT_BTN_VOLDOWN,
    IT_BTN_ROTATE_CCW,
    IT_BTN_ROTATE_CW,
    IT_BTN_SHAKE,
};

/*
 * A real press is not instantaneous and the guest can tell. SpringBoard
 * distinguishes a Home tap from a hold, and the volume keys ramp for as long as
 * the GPIO stays asserted, so the release is scheduled rather than sent in the
 * same breath as the press.
 */
#define IT_BTN_HOLD_MS 250

static void it_send_chord(int key, bool shift, bool down)
{
    with_bql(^{
        if (down) {
            qkbd_state_key_event(kbd, Q_KEY_CODE_META_L, true);
            if (shift) {
                qkbd_state_key_event(kbd, Q_KEY_CODE_SHIFT, true);
            }
            qkbd_state_key_event(kbd, key, true);
        } else {
            qkbd_state_key_event(kbd, key, false);
            if (shift) {
                qkbd_state_key_event(kbd, Q_KEY_CODE_SHIFT, false);
            }
            qkbd_state_key_event(kbd, Q_KEY_CODE_META_L, false);
        }
    });
}

- (void)deviceButton:(id)sender
{
    int key;
    bool shift = false;
    bool momentary = true;

    switch ([sender tag]) {
    case IT_BTN_HOME:       key = Q_KEY_CODE_H; shift = true; break;
    case IT_BTN_POWER:      key = Q_KEY_CODE_L; break;
    case IT_BTN_VOLUP:      key = Q_KEY_CODE_EQUAL; break;
    case IT_BTN_VOLDOWN:    key = Q_KEY_CODE_MINUS; break;
    /* Rotation is edge-triggered on the press; there is nothing to release. */
    case IT_BTN_ROTATE_CCW: key = Q_KEY_CODE_LEFT;  momentary = false; break;
    case IT_BTN_ROTATE_CW:  key = Q_KEY_CODE_RIGHT; momentary = false; break;
    case IT_BTN_SHAKE: {
        __block Error *err = NULL;
        with_bql(^{
            Object *machine = qdev_get_machine();
            if (machine && object_property_find(machine, "accel-shake")) {
                object_property_set_bool(machine, "accel-shake", true, &err);
            } else {
                error_setg(&err, "this machine has no accelerometer");
            }
        });
        if (err) {
            QEMU_Alert([NSString stringWithUTF8String: error_get_pretty(err)]);
            error_free(err);
        }
        return;
    }
    default:
        return;
    }

    it_send_chord(key, shift, true);
    if (!momentary) {
        it_send_chord(key, shift, false);
        return;
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                 (int64_t)(IT_BTN_HOLD_MS * NSEC_PER_MSEC)),
                   dispatch_get_main_queue(), ^{
        it_send_chord(key, shift, false);
    });
}

/*
 * Device ▸ Install App… -- the same handler a dropped .ipa takes. Dragging onto
 * the window is not discoverable, and someone who downloaded a prebuilt app has
 * no terminal open to be told about it in.
 */
- (void)installApp:(id)sender
{
    NSOpenPanel *panel = [NSOpenPanel openPanel];

    [panel setAllowedFileTypes:@[@"ipa"]];
    [panel setAllowsMultipleSelection:NO];
    [panel setMessage:@"Choose a decrypted .ipa to install on the device."];
    if ([panel runModal] != NSModalResponseOK) {
        return;
    }
    [cocoaView installIPA:[[panel URL] path]];
}

/*
 * Device ▸ Erase All Content and Settings…
 *
 * Everything the guest has ever written lives in the copy-on-write overlay, so
 * a factory reset is deleting one directory -- but NOT from here. The overlay's
 * page files are open and being written to by the running machine, and removing
 * them underneath it corrupts whatever is in flight. So this drops a marker in
 * the overlay and quits; the runner sees the marker on its next start and wipes
 * before QEMU exists to care. Same reason the marker is not simply "wipe on
 * exit": a crash or a kill -9 would skip it, and the device would come back
 * with the state the user just asked to be rid of.
 */
- (void)eraseDevice:(id)sender
{
    __block char *overlay = NULL;
    NSAlert *alert;

    with_bql(^{
        Object *machine = qdev_get_machine();
        if (machine && object_property_find(machine, "nandrw")) {
            overlay = object_property_get_str(machine, "nandrw", NULL);
        }
    });

    if (overlay == NULL || *overlay == '\0') {
        g_free(overlay);
        QEMU_Alert(@"This device has no writable overlay, so there is nothing "
                   @"to erase -- it already starts clean every time.");
        return;
    }

    alert = [[[NSAlert alloc] init] autorelease];
    [alert setMessageText:@"Erase all content and settings?"];
    [alert setInformativeText:@"Every app you installed and every setting you "
                              @"changed will be deleted, and the device will "
                              @"return to how it shipped.\n\nThe emulator will "
                              @"quit; the erase happens the next time you open "
                              @"it."];
    [alert addButtonWithTitle:@"Cancel"];
    [alert addButtonWithTitle:@"Erase"];
    [alert setAlertStyle:NSAlertStyleCritical];
    if ([alert runModal] != NSAlertSecondButtonReturn) {
        g_free(overlay);
        return;
    }

    NSString *marker = [[NSString stringWithUTF8String:overlay]
                        stringByAppendingPathComponent:@".erase-on-next-start"];
    g_free(overlay);

    if (![@"" writeToFile:marker atomically:YES
                 encoding:NSUTF8StringEncoding error:NULL]) {
        QEMU_Alert([NSString stringWithFormat:
            @"Could not mark the device for erase: %@ is not writable.", marker]);
        return;
    }
    with_bql(^{
        qmp_quit(NULL);
    });
}

/*
 * Device ▸ Open Terminal -- a root shell on the guest, over USB.
 *
 * The script is the deliverable, not this method: it is runnable from a
 * terminal, so the ordering it has to get right (usbmuxd, then iproxy, then
 * ssh) can be debugged without going through the menu. All that happens here is
 * that its refusals become a dialog, because someone running a prebuilt app has
 * no stderr to read them on.
 */
- (void)openTerminal:(id)sender
{
    const char *env = getenv("IT_SSH_TERMINAL");
    NSString *tool = env ? [NSString stringWithUTF8String:env]
                         : [[[NSBundle mainBundle] bundlePath]
                            stringByAppendingPathComponent:@"../contrib/it-ssh-terminal.sh"];

    if (![[NSFileManager defaultManager] isExecutableFileAtPath:tool]) {
        QEMU_Alert([NSString stringWithFormat:
            @"Cannot open a terminal: %@ is missing.\n\nSet IT_SSH_TERMINAL to "
            @"its path if the tree is somewhere else.", tool]);
        return;
    }

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        NSTask *task = [[NSTask alloc] init];
        NSPipe *pipe = [NSPipe pipe];
        NSData *out = nil;
        int status = -1;

        [task setLaunchPath:tool];
        [task setStandardError:pipe];
        [task setStandardOutput:[NSFileHandle fileHandleWithNullDevice]];
        @try {
            [task launch];
            out = [[pipe fileHandleForReading] readDataToEndOfFile];
            [task waitUntilExit];
            status = [task terminationStatus];
        } @catch (NSException *e) {
            out = [[e reason] dataUsingEncoding:NSUTF8StringEncoding];
        }
        NSString *text = [[[NSString alloc] initWithData:out
                            encoding:NSUTF8StringEncoding] autorelease];
        [task release];

        if (status != 0) {
            dispatch_async(dispatch_get_main_queue(), ^{
                QEMU_Alert([NSString stringWithFormat:@"Could not open a "
                            @"terminal on the device.\n\n%@",
                            [text length] ? text : @"(no output)"]);
            });
        }
    });
}

/* Used by the Speed menu items */
- (void)adjustSpeed:(id)sender
{
    int throttle_pct; /* throttle percentage */
    NSMenu *menu;

    menu = [sender menu];
    if (menu != nil)
    {
        /* Unselect the currently selected item */
        for (NSMenuItem *item in [menu itemArray]) {
            if (item.state == NSControlStateValueOn) {
                [item setState: NSControlStateValueOff];
                break;
            }
        }
    }

    // check the menu item
    [sender setState: NSControlStateValueOn];

    // get the throttle percentage
    throttle_pct = [sender tag];

    with_bql(^{
        cpu_throttle_set(throttle_pct);
    });
    COCOA_DEBUG("cpu throttling at %d%c\n", cpu_throttle_get_percentage(), '%');
}

@end

@interface QemuApplication : NSApplication
@end

@implementation QemuApplication
- (void)sendEvent:(NSEvent *)event
{
    COCOA_DEBUG("QemuApplication: sendEvent\n");
    if (![cocoaView handleEvent:event]) {
        [super sendEvent: event];
    }
}
@end

static void create_initial_menus(void)
{
    // Add menus
    NSMenu      *menu;
    NSMenuItem  *menuItem;

    [NSApp setMainMenu:[[NSMenu alloc] init]];
    [NSApp setServicesMenu:[[NSMenu alloc] initWithTitle:@"Services"]];

    // Application menu
    menu = [[NSMenu alloc] initWithTitle:@""];
    [menu addItemWithTitle:@"About QEMU" action:@selector(do_about_menu_item:) keyEquivalent:@""]; // About QEMU
    [menu addItem:[NSMenuItem separatorItem]]; //Separator
    menuItem = [menu addItemWithTitle:@"Services" action:nil keyEquivalent:@""];
    [menuItem setSubmenu:[NSApp servicesMenu]];
    [menu addItem:[NSMenuItem separatorItem]];
    [menu addItemWithTitle:@"Hide QEMU" action:@selector(hide:) keyEquivalent:@"h"]; //Hide QEMU
    menuItem = (NSMenuItem *)[menu addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@"h"]; // Hide Others
    [menuItem setKeyEquivalentModifierMask:(NSEventModifierFlagOption|NSEventModifierFlagCommand)];
    [menu addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""]; // Show All
    [menu addItem:[NSMenuItem separatorItem]]; //Separator
    [menu addItemWithTitle:@"Quit QEMU" action:@selector(terminate:) keyEquivalent:@"q"];
    menuItem = [[NSMenuItem alloc] initWithTitle:@"Apple" action:nil keyEquivalent:@""];
    [menuItem setSubmenu:menu];
    [[NSApp mainMenu] addItem:menuItem];
    [NSApp performSelector:@selector(setAppleMenu:) withObject:menu]; // Workaround (this method is private since 10.4+)

    // Machine menu
    menu = [[NSMenu alloc] initWithTitle: @"Machine"];
    [menu setAutoenablesItems: NO];
    [menu addItem: [[[NSMenuItem alloc] initWithTitle: @"Pause" action: @selector(pauseQEMU:) keyEquivalent: @""] autorelease]];
    menuItem = [[[NSMenuItem alloc] initWithTitle: @"Resume" action: @selector(resumeQEMU:) keyEquivalent: @""] autorelease];
    [menu addItem: menuItem];
    [menuItem setEnabled: NO];
    [menu addItem: [NSMenuItem separatorItem]];
    [menu addItem: [[[NSMenuItem alloc] initWithTitle: @"Reset" action: @selector(restartQEMU:) keyEquivalent: @""] autorelease]];
    [menu addItem: [[[NSMenuItem alloc] initWithTitle: @"Power Down" action: @selector(powerDownQEMU:) keyEquivalent: @""] autorelease]];
    menuItem = [[[NSMenuItem alloc] initWithTitle: @"Machine" action:nil keyEquivalent:@""] autorelease];
    [menuItem setSubmenu:menu];
    [[NSApp mainMenu] addItem:menuItem];

    /*
     * Edit menu. Cmd+Ctrl+C and Cmd+Ctrl+V rather than the plain Cmd forms, on
     * purpose: those are the iPhone Simulator's shortcuts for the same two
     * operations, and leaving plain Cmd+C/Cmd+V alone means they still reach
     * the guest as key events.
     */
    menu = [[NSMenu alloc] initWithTitle:@"Edit"];
    menuItem = [[[NSMenuItem alloc] initWithTitle:@"Copy Screen" action:@selector(copyScreen:) keyEquivalent:@"c"] autorelease];
    [menuItem setKeyEquivalentModifierMask:(NSEventModifierFlagCommand|NSEventModifierFlagControl)];
    [menu addItem: menuItem];
    menuItem = [[[NSMenuItem alloc] initWithTitle:@"Paste Text to Guest" action:@selector(pasteTextToGuest:) keyEquivalent:@"v"] autorelease];
    [menuItem setKeyEquivalentModifierMask:(NSEventModifierFlagControl|NSEventModifierFlagCommand)];
    [menu addItem: menuItem];
    menuItem = [[[NSMenuItem alloc] initWithTitle:@"Edit" action:nil keyEquivalent:@""] autorelease];
    [menuItem setSubmenu:menu];
    [[NSApp mainMenu] addItem:menuItem];

    /*
     * Device menu -- the hardware the emulated iPod has and a Mac does not.
     * Every item here was previously a Command chord you had to be told about,
     * which is no use to someone running a prebuilt app with no docs open.
     */
    menu = [[NSMenu alloc] initWithTitle:@"Device"];
    struct { const char *title; int tag; const char *key; NSEventModifierFlags mods; } it_buttons[] = {
        { "Home",            IT_BTN_HOME,       "h", NSEventModifierFlagCommand | NSEventModifierFlagShift },
        { "Lock",            IT_BTN_POWER,      "l", NSEventModifierFlagCommand },
        { NULL, 0, NULL, 0 },
        { "Volume Up",       IT_BTN_VOLUP,      "=", NSEventModifierFlagCommand },
        { "Volume Down",     IT_BTN_VOLDOWN,    "-", NSEventModifierFlagCommand },
        { NULL, 0, NULL, 0 },
        { "Rotate Left",     IT_BTN_ROTATE_CCW, "",  0 },
        { "Rotate Right",    IT_BTN_ROTATE_CW,  "",  0 },
        { "Shake",           IT_BTN_SHAKE,      "",  0 },
    };
    for (size_t i = 0; i < ARRAY_SIZE(it_buttons); i++) {
        if (it_buttons[i].title == NULL) {
            [menu addItem: [NSMenuItem separatorItem]];
            continue;
        }
        menuItem = [[[NSMenuItem alloc]
                     initWithTitle: [NSString stringWithUTF8String: it_buttons[i].title]
                     action: @selector(deviceButton:)
                     keyEquivalent: [NSString stringWithUTF8String: it_buttons[i].key]] autorelease];
        if (it_buttons[i].mods) {
            [menuItem setKeyEquivalentModifierMask: it_buttons[i].mods];
        }
        [menuItem setTag: it_buttons[i].tag];
        [menu addItem: menuItem];
    }
    [menu addItem: [NSMenuItem separatorItem]];
    [menu addItem: [[[NSMenuItem alloc] initWithTitle:@"Install App…"
                     action:@selector(installApp:) keyEquivalent:@""] autorelease]];
    [menu addItem: [[[NSMenuItem alloc] initWithTitle:@"Open Terminal"
                     action:@selector(openTerminal:) keyEquivalent:@""] autorelease]];
    [menu addItem: [NSMenuItem separatorItem]];
    [menu addItem: [[[NSMenuItem alloc] initWithTitle:@"Erase All Content and Settings…"
                     action:@selector(eraseDevice:) keyEquivalent:@""] autorelease]];
    menuItem = [[[NSMenuItem alloc] initWithTitle:@"Device" action:nil keyEquivalent:@""] autorelease];
    [menuItem setSubmenu:menu];
    [[NSApp mainMenu] addItem:menuItem];

    // View menu
    menu = [[NSMenu alloc] initWithTitle:@"View"];
    [menu addItem: [[[NSMenuItem alloc] initWithTitle:@"Enter Fullscreen" action:@selector(doToggleFullScreen:) keyEquivalent:@"f"] autorelease]]; // Fullscreen
    menuItem = [[[NSMenuItem alloc] initWithTitle:@"Zoom To Fit" action:@selector(zoomToFit:) keyEquivalent:@""] autorelease];
    [menuItem setState: [[cocoaView window] styleMask] & NSWindowStyleMaskResizable ? NSControlStateValueOn : NSControlStateValueOff];
    [menu addItem: menuItem];
    menuItem = [[[NSMenuItem alloc] initWithTitle:@"Zoom Interpolation" action:@selector(toggleZoomInterpolation:) keyEquivalent:@""] autorelease];
    [menuItem setState: zoom_interpolation == kCGInterpolationLow ? NSControlStateValueOn : NSControlStateValueOff];
    [menu addItem: menuItem];
    menuItem = [[[NSMenuItem alloc] initWithTitle:@"Show Taps" action:@selector(toggleShowTaps:) keyEquivalent:@""] autorelease];
    [menuItem setTarget: cocoaView];
    [menuItem setState: [cocoaView showTaps] ? NSControlStateValueOn : NSControlStateValueOff];
    [menu addItem: menuItem];
    menuItem = [[[NSMenuItem alloc] initWithTitle:@"View" action:nil keyEquivalent:@""] autorelease];
    [menuItem setSubmenu:menu];
    [[NSApp mainMenu] addItem:menuItem];

    // Speed menu
    menu = [[NSMenu alloc] initWithTitle:@"Speed"];

    // Add the rest of the Speed menu items
    int p, percentage, throttle_pct;
    for (p = 10; p >= 0; p--)
    {
        percentage = p * 10 > 1 ? p * 10 : 1; // prevent a 0% menu item

        menuItem = [[[NSMenuItem alloc]
                   initWithTitle: [NSString stringWithFormat: @"%d%%", percentage] action:@selector(adjustSpeed:) keyEquivalent:@""] autorelease];

        if (percentage == 100) {
            [menuItem setState: NSControlStateValueOn];
        }

        /* Calculate the throttle percentage */
        throttle_pct = -1 * percentage + 100;

        [menuItem setTag: throttle_pct];
        [menu addItem: menuItem];
    }
    menuItem = [[[NSMenuItem alloc] initWithTitle:@"Speed" action:nil keyEquivalent:@""] autorelease];
    [menuItem setSubmenu:menu];
    [[NSApp mainMenu] addItem:menuItem];

    // Window menu
    menu = [[NSMenu alloc] initWithTitle:@"Window"];
    [menu addItem: [[[NSMenuItem alloc] initWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"] autorelease]]; // Miniaturize
    menuItem = [[[NSMenuItem alloc] initWithTitle:@"Window" action:nil keyEquivalent:@""] autorelease];
    [menuItem setSubmenu:menu];
    [[NSApp mainMenu] addItem:menuItem];
    [NSApp setWindowsMenu:menu];

    // Help menu
    menu = [[NSMenu alloc] initWithTitle:@"Help"];
    [menu addItem: [[[NSMenuItem alloc] initWithTitle:@"QEMU Documentation" action:@selector(showQEMUDoc:) keyEquivalent:@"?"] autorelease]]; // QEMU Help
    menuItem = [[[NSMenuItem alloc] initWithTitle:@"Window" action:nil keyEquivalent:@""] autorelease];
    [menuItem setSubmenu:menu];
    [[NSApp mainMenu] addItem:menuItem];
}

/* Returns a name for a given console */
static NSString * getConsoleName(QemuConsole * console)
{
    g_autofree char *label = qemu_console_get_label(console);

    return [NSString stringWithUTF8String:label];
}

/* Add an entry to the View menu for each console */
static void add_console_menu_entries(void)
{
    NSMenu *menu;
    NSMenuItem *menuItem;
    int index = 0;

    menu = [[[NSApp mainMenu] itemWithTitle:@"View"] submenu];

    [menu addItem:[NSMenuItem separatorItem]];

    while (qemu_console_lookup_by_index(index) != NULL) {
        menuItem = [[[NSMenuItem alloc] initWithTitle: getConsoleName(qemu_console_lookup_by_index(index))
                                               action: @selector(displayConsole:) keyEquivalent: @""] autorelease];
        [menuItem setTag: index];
        [menu addItem: menuItem];
        index++;
    }
}

/* Make menu items for all removable devices.
 * Each device is given an 'Eject' and 'Change' menu item.
 */
static void addRemovableDevicesMenuItems(void)
{
    NSMenu *menu;
    NSMenuItem *menuItem;
    BlockInfoList *currentDevice, *pointerToFree;
    NSString *deviceName;

    currentDevice = qmp_query_block(NULL);
    pointerToFree = currentDevice;

    menu = [[[NSApp mainMenu] itemWithTitle:@"Machine"] submenu];

    // Add a separator between related groups of menu items
    [menu addItem:[NSMenuItem separatorItem]];

    // Set the attributes to the "Removable Media" menu item
    NSString *titleString = @"Removable Media";
    NSMutableAttributedString *attString=[[NSMutableAttributedString alloc] initWithString:titleString];
    NSColor *newColor = [NSColor blackColor];
    NSFontManager *fontManager = [NSFontManager sharedFontManager];
    NSFont *font = [fontManager fontWithFamily:@"Helvetica"
                                          traits:NSBoldFontMask|NSItalicFontMask
                                          weight:0
                                            size:14];
    [attString addAttribute:NSFontAttributeName value:font range:NSMakeRange(0, [titleString length])];
    [attString addAttribute:NSForegroundColorAttributeName value:newColor range:NSMakeRange(0, [titleString length])];
    [attString addAttribute:NSUnderlineStyleAttributeName value:[NSNumber numberWithInt: 1] range:NSMakeRange(0, [titleString length])];

    // Add the "Removable Media" menu item
    menuItem = [NSMenuItem new];
    [menuItem setAttributedTitle: attString];
    [menuItem setEnabled: NO];
    [menu addItem: menuItem];

    /* Loop through all the block devices in the emulator */
    while (currentDevice) {
        deviceName = [[NSString stringWithFormat: @"%s", currentDevice->value->device] retain];

        if(currentDevice->value->removable) {
            menuItem = [[NSMenuItem alloc] initWithTitle: [NSString stringWithFormat: @"Change %s...", currentDevice->value->device]
                                                  action: @selector(changeDeviceMedia:)
                                           keyEquivalent: @""];
            [menu addItem: menuItem];
            [menuItem setRepresentedObject: deviceName];
            [menuItem autorelease];

            menuItem = [[NSMenuItem alloc] initWithTitle: [NSString stringWithFormat: @"Eject %s", currentDevice->value->device]
                                                  action: @selector(ejectDeviceMedia:)
                                           keyEquivalent: @""];
            [menu addItem: menuItem];
            [menuItem setRepresentedObject: deviceName];
            [menuItem autorelease];
        }
        currentDevice = currentDevice->next;
    }
    qapi_free_BlockInfoList(pointerToFree);
}

static void cocoa_mouse_mode_change_notify(Notifier *notifier, void *data)
{
    dispatch_async(dispatch_get_main_queue(), ^{
        [cocoaView notifyMouseModeChange];
    });
}

static Notifier mouse_mode_change_notifier = {
    .notify = cocoa_mouse_mode_change_notify
};

@interface QemuCocoaPasteboardTypeOwner : NSObject<NSPasteboardTypeOwner>
@end

@implementation QemuCocoaPasteboardTypeOwner

- (void)pasteboard:(NSPasteboard *)sender provideDataForType:(NSPasteboardType)type
{
    if (type != NSPasteboardTypeString) {
        return;
    }

    with_bql(^{
        QemuClipboardInfo *info = qemu_clipboard_info_ref(cbinfo);
        qemu_event_reset(&cbevent);
        qemu_clipboard_request(info, QEMU_CLIPBOARD_TYPE_TEXT);

        while (info == cbinfo &&
               info->types[QEMU_CLIPBOARD_TYPE_TEXT].available &&
               info->types[QEMU_CLIPBOARD_TYPE_TEXT].data == NULL) {
            bql_unlock();
            qemu_event_wait(&cbevent);
            bql_lock();
        }

        if (info == cbinfo) {
            NSData *data = [[NSData alloc] initWithBytes:info->types[QEMU_CLIPBOARD_TYPE_TEXT].data
                                           length:info->types[QEMU_CLIPBOARD_TYPE_TEXT].size];
            [sender setData:data forType:NSPasteboardTypeString];
            [data release];
        }

        qemu_clipboard_info_unref(info);
    });
}

@end

static void cocoa_clipboard_notify(Notifier *notifier, void *data);
static void cocoa_clipboard_request(QemuClipboardInfo *info,
                                    QemuClipboardType type);

static QemuClipboardPeer cbpeer = {
    .name = "cocoa",
    .notifier = { .notify = cocoa_clipboard_notify },
    .request = cocoa_clipboard_request
};

static void cocoa_clipboard_update_info(QemuClipboardInfo *info)
{
    if (info->owner == &cbpeer || info->selection != QEMU_CLIPBOARD_SELECTION_CLIPBOARD) {
        return;
    }

    if (info != cbinfo) {
        NSAutoreleasePool * pool = [[NSAutoreleasePool alloc] init];
        qemu_clipboard_info_unref(cbinfo);
        cbinfo = qemu_clipboard_info_ref(info);
        cbchangecount = [[NSPasteboard generalPasteboard] declareTypes:@[NSPasteboardTypeString] owner:cbowner];
        [pool release];
    }

    qemu_event_set(&cbevent);
}

static void cocoa_clipboard_notify(Notifier *notifier, void *data)
{
    QemuClipboardNotify *notify = data;

    switch (notify->type) {
    case QEMU_CLIPBOARD_UPDATE_INFO:
        cocoa_clipboard_update_info(notify->info);
        return;
    case QEMU_CLIPBOARD_RESET_SERIAL:
        /* ignore */
        return;
    }
}

static void cocoa_clipboard_request(QemuClipboardInfo *info,
                                    QemuClipboardType type)
{
    NSAutoreleasePool *pool;
    NSData *text;

    switch (type) {
    case QEMU_CLIPBOARD_TYPE_TEXT:
        pool = [[NSAutoreleasePool alloc] init];
        text = [[NSPasteboard generalPasteboard] dataForType:NSPasteboardTypeString];
        if (text) {
            qemu_clipboard_set_data(&cbpeer, info, type,
                                    [text length], [text bytes], true);
        }
        [pool release];
        break;
    default:
        break;
    }
}

static int cocoa_main(void)
{
    COCOA_DEBUG("Main thread: entering OSX run loop\n");
    [NSApp run];
    COCOA_DEBUG("Main thread: left OSX run loop, which should never happen\n");

    abort();
}



#pragma mark qemu
static void cocoa_update(DisplayChangeListener *dcl,
                         int x, int y, int w, int h)
{
    COCOA_DEBUG("qemu_cocoa: cocoa_update\n");

    dispatch_async(dispatch_get_main_queue(), ^{
        NSRect rect = NSMakeRect(x, [cocoaView gscreen].height - y - h, w, h);
        [cocoaView setNeedsDisplayInRect:rect];
    });
}

static void cocoa_switch(DisplayChangeListener *dcl,
                         DisplaySurface *surface)
{
    pixman_image_t *image = surface->image;

    COCOA_DEBUG("qemu_cocoa: cocoa_switch\n");

    // The DisplaySurface will be freed as soon as this callback returns.
    // We take a reference to the underlying pixman image here so it does
    // not disappear from under our feet; the switchSurface method will
    // deref the old image when it is done with it.
    pixman_image_ref(image);

    dispatch_async(dispatch_get_main_queue(), ^{
        [cocoaView switchSurface:image];
    });
}

static void cocoa_refresh(DisplayChangeListener *dcl)
{
    NSAutoreleasePool * pool = [[NSAutoreleasePool alloc] init];

    COCOA_DEBUG("qemu_cocoa: cocoa_refresh\n");
    graphic_hw_update(dcl->con);

    if (cbchangecount != [[NSPasteboard generalPasteboard] changeCount]) {
        qemu_clipboard_info_unref(cbinfo);
        cbinfo = qemu_clipboard_info_new(&cbpeer, QEMU_CLIPBOARD_SELECTION_CLIPBOARD);
        if ([[NSPasteboard generalPasteboard] availableTypeFromArray:@[NSPasteboardTypeString]]) {
            cbinfo->types[QEMU_CLIPBOARD_TYPE_TEXT].available = true;
        }
        qemu_clipboard_update(cbinfo);
        cbchangecount = [[NSPasteboard generalPasteboard] changeCount];
        qemu_event_set(&cbevent);
    }

    [pool release];
}

static void cocoa_mouse_set(DisplayChangeListener *dcl, int x, int y, bool on)
{
    dispatch_async(dispatch_get_main_queue(), ^{
        [cocoaView setMouseX:x y:y on:on];
    });
}

static void cocoa_cursor_define(DisplayChangeListener *dcl, QEMUCursor *cursor)
{
    dispatch_async(dispatch_get_main_queue(), ^{
        BQL_LOCK_GUARD();
        [cocoaView setCursor:qemu_console_get_cursor(dcl->con)];
    });
}

static void cocoa_display_init(DisplayState *ds, DisplayOptions *opts)
{
    NSAutoreleasePool * pool = [[NSAutoreleasePool alloc] init];

    COCOA_DEBUG("qemu_cocoa: cocoa_display_init\n");

    // Pull this console process up to being a fully-fledged graphical
    // app with a menubar and Dock icon
    ProcessSerialNumber psn = { 0, kCurrentProcess };
    TransformProcessType(&psn, kProcessTransformToForegroundApplication);

    [QemuApplication sharedApplication];

    // Create an Application controller
    QemuCocoaAppController *controller = [[QemuCocoaAppController alloc] init];
    [NSApp setDelegate:controller];

    /* if fullscreen mode is to be used */
    if (opts->has_full_screen && opts->full_screen) {
        [[cocoaView window] toggleFullScreen: nil];
    }
    if (opts->u.cocoa.has_full_grab && opts->u.cocoa.full_grab) {
        [controller setFullGrab: nil];
    }

    if (opts->has_show_cursor && opts->show_cursor) {
        cursor_hide = 0;
    }
    if (opts->u.cocoa.has_swap_opt_cmd) {
        swap_opt_cmd = opts->u.cocoa.swap_opt_cmd;
    }

    if (opts->u.cocoa.has_left_command_key && !opts->u.cocoa.left_command_key) {
        left_command_key_enabled = 0;
    }

    if (opts->u.cocoa.has_zoom_to_fit && opts->u.cocoa.zoom_to_fit) {
        [cocoaView window].styleMask |= NSWindowStyleMaskResizable;
    }

    if (opts->u.cocoa.has_zoom_interpolation && opts->u.cocoa.zoom_interpolation) {
        zoom_interpolation = kCGInterpolationLow;
    }

    create_initial_menus();
    /*
     * Create the menu entries which depend on QEMU state (for consoles
     * and removable devices). These make calls back into QEMU functions,
     * which is OK because at this point we know that the second thread
     * holds the BQL and is synchronously waiting for us to
     * finish.
     */
    add_console_menu_entries();
    addRemovableDevicesMenuItems();

    dcl.con = qemu_console_lookup_default();
    kbd = qkbd_state_init(dcl.con);

    // register vga output callbacks
    register_displaychangelistener(&dcl);
    qemu_add_mouse_mode_change_notifier(&mouse_mode_change_notifier);
    [cocoaView notifyMouseModeChange];
    [cocoaView updateUIInfo];

    qemu_event_init(&cbevent, false);
    cbowner = [[QemuCocoaPasteboardTypeOwner alloc] init];
    qemu_clipboard_peer_register(&cbpeer);

    [pool release];

    /*
     * The Cocoa UI will run the NSApplication runloop on the main thread
     * rather than the default Core Foundation one.
     */
    qemu_main = cocoa_main;
}

static QemuDisplay qemu_display_cocoa = {
    .type       = DISPLAY_TYPE_COCOA,
    .init       = cocoa_display_init,
};

static void register_cocoa(void)
{
    qemu_display_register(&qemu_display_cocoa);
}

type_init(register_cocoa);
