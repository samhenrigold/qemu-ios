/*
 * Hello, 2007 -- an iPhone OS 1.0 app the way the 2007 toolchain wrote them: no SDK, UIKit a private
 * framework, the interfaces below declared by hand from the firmware's own class metadata (as class-dump
 * headers were), a UIApplication subclass handed to UIApplicationMain. Built and linked against 1.0's own
 * frameworks by build.sh; docs/m68/sideload.md.
 */
typedef float CGFloat;
typedef struct { CGFloat x, y; } CGPoint;
typedef struct { CGFloat width, height; } CGSize;
typedef struct { CGPoint origin; CGSize size; } CGRect;
typedef struct CGColor *CGColorRef;
typedef struct CGColorSpace *CGColorSpaceRef;
typedef struct __GSFont *GSFontRef;
typedef signed char BOOL;
typedef struct objc_class *Class;
typedef struct objc_selector *SEL;
#define YES ((BOOL)1)
#define NO ((BOOL)0)
#define nil ((id)0)

extern CGColorSpaceRef CGColorSpaceCreateDeviceRGB(void);
extern CGColorRef CGColorCreate(CGColorSpaceRef, const CGFloat *);
extern GSFontRef GSFontCreateWithName(const char *, int traits, CGFloat size);
extern int UIApplicationMain(int, char **, Class);
extern int printf(const char *, ...);

@interface NSObject { Class isa; }
+ (id)alloc;
+ (Class)class;
- (id)init;
@end
@interface NSString : NSObject
+ (id)stringWithFormat:(id)fmt, ...;
@end
@interface NSArray : NSObject
+ (id)arrayWithObject:(id)o;
@end
@interface NSAutoreleasePool : NSObject
@end
@interface UIResponder : NSObject
@end
/*
 * The fragile ABI places a subclass's ivars right after its superclass's, at offsets fixed at compile
 * time, so a class we subclass is declared at its real instance size (1.0's class metadata: UIView 28
 * bytes, UIApplication 12, both including isa); class-dump headers carried the ivars themselves.
 */
@interface UIView : UIResponder {
    char uiview_ivars[24];
}
- (id)initWithFrame:(CGRect)frame;
- (void)addSubview:(UIView *)view;
- (void)setBackgroundColor:(CGColorRef)color;
@end
@interface UIWindow : UIView
- (id)initWithContentRect:(CGRect)rect;
- (void)orderFront:(id)sender;
- (void)makeKey:(id)sender;
- (void)_setHidden:(BOOL)hidden;
- (void)setContentView:(UIView *)view;
@end
@interface UIHardware : NSObject
+ (CGRect)fullScreenApplicationContentRect;
@end
@interface UITextLabel : UIView
- (void)setText:(id)text;
- (void)setFont:(GSFontRef)font;
- (void)setColor:(CGColorRef)color;
- (void)setCentersHorizontally:(BOOL)centers;
@end
@interface UINavigationItem : NSObject
- (id)initWithTitle:(id)title;
@end
@interface UINavigationBar : UIView
- (void)setDelegate:(id)delegate;
- (void)pushNavigationItem:(UINavigationItem *)item;
- (void)showButtonsWithLeftTitle:(id)left rightTitle:(id)right leftBack:(BOOL)back;
@end
@interface UIAlertSheet : UIView
- (id)initWithTitle:(id)title buttons:(NSArray *)buttons defaultButtonIndex:(int)index delegate:(id)delegate
            context:(id)context;
- (void)setBodyText:(id)text;
- (void)popupAlertAnimated:(BOOL)animated;
- (void)dismiss;
@end
@interface UIApplication : UIResponder {
    char uiapplication_ivars[8];
}
@end

static CGColorRef rgb(CGFloat r, CGFloat g, CGFloat b)
{
    CGFloat c[4] = { r, g, b, 1 };
    return CGColorCreate(CGColorSpaceCreateDeviceRGB(), c);
}

static UITextLabel *label(CGRect frame, id text, CGFloat size, CGColorRef color, CGColorRef background)
{
    UITextLabel *l = [[UITextLabel alloc] initWithFrame:frame];
    [l setText:text];
    [l setFont:GSFontCreateWithName("Helvetica", size >= 30 ? 2 : 0, size)];   /* 2: bold */
    [l setColor:color];
    [l setBackgroundColor:background];
    [l setCentersHorizontally:YES];
    return l;
}

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
- (void)mouseUp:(struct __GSEvent *)event
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

/*
 * 1.x's dyld (the Tiger one) defers library initializers -- libobjc's among them, which registers the
 * runtime's image callback -- until the executable's crt1 asks for them, and hands crt1 its lookup
 * function through the image's __DATA,__dyld section. crt1old.c (shared with plain-C helpers that need no
 * initializers) does not, so an Objective-C program does it here; without it every class and selector
 * reference stays unfixed and the first message crashes.
 */
__attribute__((section("__DATA,__dyld"), used)) static struct {
    void *lazy_binding_helper;
    int (*lookup)(const char *name, void **address);
} dyld_glue;

int main(int argc, char **argv)
{
    void (*initializers)(void) = 0;
    if (dyld_glue.lookup) {
        dyld_glue.lookup("__dyld_make_delayed_module_initializer_calls", (void **)&initializers);
    }
    if (initializers) {
        initializers();
    }
    [[NSAutoreleasePool alloc] init];
    return UIApplicationMain(argc, argv, [HelloApp class]);
}
