/*
 * Tilt, 2007 -- a ball that rolls with the phone. The accelerometer and Core Graphics on iPhone OS 1.0:
 * the UIApplication subclass overrides acceleratedInX:Y:Z:, which is all UIKit wants to start sending raw
 * accelerometer events (_requestAccelerometerEventsIfNeeded checks for the override), and a UIView draws
 * the level in drawRect: through UICurrentContext(). Same toolchain as Hello (build.sh, uikit1.h).
 */
#include "uikit1.h"

extern float sqrtf(float);

#define BALL 34.0f       /* diameter */
#define RANGE 120.0f     /* points of travel for 1 g */

@interface TiltView : UIView {
@public
    float x, y, z;       /* low-passed acceleration, g */
}
@end

@implementation TiltView

- (void)drawRect:(CGRect)rect
{
    CGContextRef c = UICurrentContext();
    CGFloat cx = rect.size.width / 2, cy = 40 + (rect.size.height - 40) / 2;
    float tilt = sqrtf(x * x + y * y);
    int level = tilt < 0.05f;

    CGContextSetRGBFillColor(c, 0.07, 0.09, 0.12, 1);
    CGContextFillRect(c, rect);

    /* the target: rings every 30 points and a crosshair */
    CGContextSetLineWidth(c, 2);
    for (int r = 1; r <= 4; r++) {
        CGFloat d = r * 60.0f;
        CGContextSetRGBStrokeColor(c, 0.30, 0.38, 0.48, 1);
        CGContextStrokeEllipseInRect(c, (CGRect){ { cx - d / 2, cy - d / 2 }, { d, d } });
    }
    CGContextMoveToPoint(c, cx - 130, cy);
    CGContextAddLineToPoint(c, cx + 130, cy);
    CGContextMoveToPoint(c, cx, cy - 130);
    CGContextAddLineToPoint(c, cx, cy + 130);
    CGContextStrokePath(c);

    /* the ball rolls downhill: screen x follows +x, screen y follows -y (y is up the device) */
    CGFloat bx = cx + x * RANGE, by = cy - y * RANGE;
    CGFloat lim = 125 - BALL / 2;
    if (bx < cx - lim) bx = cx - lim;
    if (bx > cx + lim) bx = cx + lim;
    if (by < cy - lim) by = cy - lim;
    if (by > cy + lim) by = cy + lim;
    CGContextSetRGBFillColor(c, 0, 0, 0, 0.35);                                     /* shadow */
    CGContextFillEllipseInRect(c, (CGRect){ { bx - BALL / 2 + 3, by - BALL / 2 + 4 }, { BALL, BALL } });
    if (level) {
        CGContextSetRGBFillColor(c, 0.30, 0.85, 0.40, 1);
    } else {
        CGContextSetRGBFillColor(c, 0.95, 0.55, 0.15, 1);
    }
    CGContextFillEllipseInRect(c, (CGRect){ { bx - BALL / 2, by - BALL / 2 }, { BALL, BALL } });
    CGContextSetRGBFillColor(c, 1, 1, 1, 0.45);                                     /* highlight */
    CGContextFillEllipseInRect(c, (CGRect){ { bx - BALL / 4, by - BALL / 3 }, { BALL / 3, BALL / 4 } });
}

@end

@interface TiltApp : UIApplication {
    TiltView *view;
    UITextLabel *readout;
    int samples;
}
@end

@implementation TiltApp

- (void)applicationDidFinishLaunching:(id)unused
{
    CGRect rect = [UIHardware fullScreenApplicationContentRect];
    rect.origin.x = rect.origin.y = 0;
    UIWindow *window = [[UIWindow alloc] initWithContentRect:rect];
    CGColorRef ink = rgb(0.07, 0.09, 0.12);

    view = [[TiltView alloc] initWithFrame:rect];
    view->z = -1;
    [view addSubview:label((CGRect){ { 0, 8 }, { rect.size.width, 28 } }, @"Tilt, 2007", 22, rgb(1, 1, 1), ink)];
    readout = label((CGRect){ { 0, rect.size.height - 34 }, { rect.size.width, 24 } }, @"Tilt the phone.", 16,
                    rgb(0.72, 0.78, 0.88), ink);
    [view addSubview:readout];

    [window orderFront:self];
    [window makeKey:self];
    [window _setHidden:NO];
    [window setContentView:view];
}

- (void)acceleratedInX:(float)ax Y:(float)ay Z:(float)az
{
    const float k = 0.3f;                   /* low-pass: the sensor jitters a few hundredths of a g */
    view->x += k * (ax - view->x);
    view->y += k * (ay - view->y);
    view->z += k * (az - view->z);
    [view setNeedsDisplay];
    if (++samples % 4 == 0) {
        [readout setText:[NSString stringWithFormat:@"x %+.2f   y %+.2f   z %+.2f", view->x, view->y, view->z]];
    }
}

@end

int main(int argc, char **argv)
{
    run_library_initializers();
    [[NSAutoreleasePool alloc] init];
    return UIApplicationMain(argc, argv, [TiltApp class]);
}
