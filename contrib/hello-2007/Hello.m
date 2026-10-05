/*
 * Hello, 2007 -- an iPhone OS 1.0 app the way the 2007 toolchain wrote them: no SDK, UIKit a private
 * framework, its interfaces declared by hand from the firmware's own class metadata (uikit1.h, as class-dump
 * headers were), a UIApplication subclass handed to UIApplicationMain. Built and linked against 1.0's own
 * frameworks by build.sh; docs/m68/sideload.md.
 */
#include "uikit1.h"

@interface HelloApp : UIApplication {
    UITextLabel *counter;
    int hellos;
}
- (void)sayHello;
@end

/* Taps anywhere on the background say hello too (1.0's responders take GSEvents, mouseDown:/mouseUp:). */
@interface HelloView : UIView {
@public
    HelloApp *app;
}
@end

@implementation HelloView
- (void)mouseUp:(GSEventRef)event
{
    [app sayHello];
}
@end

@implementation HelloApp

- (void)applicationDidFinishLaunching:(id)unused
{
    CGRect rect = [UIHardware fullScreenApplicationContentRect];
    rect.origin.x = rect.origin.y = 0;
    UIWindow *window = [[UIWindow alloc] initWithContentRect:rect];
    CGColorRef blue = rgb(0.09, 0.16, 0.32), white = rgb(1, 1, 1), grey = rgb(0.72, 0.78, 0.88);

    HelloView *main = [[HelloView alloc] initWithFrame:rect];
    main->app = self;
    [main setBackgroundColor:blue];

    UINavigationBar *bar = [[UINavigationBar alloc] initWithFrame:(CGRect){ { 0, 0 }, { rect.size.width, 44 } }];
    [bar pushNavigationItem:[[UINavigationItem alloc] initWithTitle:@"Hello, 2007"]];
    [bar showButtonsWithLeftTitle:nil rightTitle:@"Hello" leftBack:NO];
    [bar setDelegate:self];
    [main addSubview:bar];

    [main addSubview:label((CGRect){ { 0, 130 }, { rect.size.width, 50 } }, @"Hello, 2007!", 36, white, blue)];
    [main addSubview:label((CGRect){ { 0, 190 }, { rect.size.width, 24 } }, @"iPhone OS 1.0, no SDK required", 16,
                           grey, blue)];
    counter = label((CGRect){ { 0, 300 }, { rect.size.width, 24 } }, @"Tap Hello, or anywhere.", 18, white, blue);
    [main addSubview:counter];

    [window orderFront:self];
    [window makeKey:self];
    [window _setHidden:NO];
    [window setContentView:main];
    printf("Hello, 2007: launched\n");
}

- (void)navigationBar:(UINavigationBar *)bar buttonClicked:(int)button
{
    [self sayHello];
}

- (void)sayHello
{
    hellos++;
    [counter setText:[NSString stringWithFormat:@"Said hello %d time%s.", hellos, hellos == 1 ? "" : "s"]];
    UIAlertSheet *sheet = [[UIAlertSheet alloc] initWithTitle:@"Hello from 2007"
                                                      buttons:[NSArray arrayWithObject:@"OK"]
                                           defaultButtonIndex:0 delegate:self context:nil];
    [sheet setBodyText:@"This app was built for iPhone OS 1.0 without an SDK, as the jailbreak community did."];
    [sheet popupAlertAnimated:YES];
}

- (void)alertSheet:(UIAlertSheet *)sheet buttonClicked:(int)button
{
    [sheet dismiss];
}

@end

int main(int argc, char **argv)
{
    run_library_initializers();
    [[NSAutoreleasePool alloc] init];
    return UIApplicationMain(argc, argv, [HelloApp class]);
}
