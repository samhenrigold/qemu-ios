/*
 * What the 2007 apps here (Hello.m, Tilt.m) declare of iPhone OS 1.0: there was no SDK, so these are
 * written by hand from the firmware's own class metadata, as class-dump headers were. Only what the apps
 * call. Built against 1.0's frameworks by build.sh; docs/m68/sideload.md.
 */
#ifndef UIKIT1_H
#define UIKIT1_H
typedef float CGFloat;
typedef struct { CGFloat x, y; } CGPoint;
typedef struct { CGFloat width, height; } CGSize;
typedef struct { CGPoint origin; CGSize size; } CGRect;
typedef struct CGColor *CGColorRef;
typedef struct CGColorSpace *CGColorSpaceRef;
typedef struct __GSFont *GSFontRef;
typedef struct __GSEvent *GSEventRef;
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
typedef struct CGContext *CGContextRef;
extern CGContextRef UICurrentContext(void);
extern void CGContextSetRGBFillColor(CGContextRef, CGFloat, CGFloat, CGFloat, CGFloat);
extern void CGContextSetRGBStrokeColor(CGContextRef, CGFloat, CGFloat, CGFloat, CGFloat);
extern void CGContextFillRect(CGContextRef, CGRect);
extern void CGContextFillEllipseInRect(CGContextRef, CGRect);
extern void CGContextStrokeEllipseInRect(CGContextRef, CGRect);
extern void CGContextSetLineWidth(CGContextRef, CGFloat);
extern void CGContextMoveToPoint(CGContextRef, CGFloat, CGFloat);
extern void CGContextAddLineToPoint(CGContextRef, CGFloat, CGFloat);
extern void CGContextStrokePath(CGContextRef);

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
- (void)setNeedsDisplay;
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

static inline CGColorRef rgb(CGFloat r, CGFloat g, CGFloat b)
{
    CGFloat c[4] = { r, g, b, 1 };
    return CGColorCreate(CGColorSpaceCreateDeviceRGB(), c);
}

static inline UITextLabel *label(CGRect frame, id text, CGFloat size, CGColorRef color, CGColorRef background)
{
    UITextLabel *l = [[UITextLabel alloc] initWithFrame:frame];
    [l setText:text];
    [l setFont:GSFontCreateWithName("Helvetica", size >= 30 ? 2 : 0, size)];   /* 2: bold */
    [l setColor:color];
    [l setBackgroundColor:background];
    [l setCentersHorizontally:YES];
    return l;
}

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

static inline void run_library_initializers(void)
{
    void (*initializers)(void) = 0;
    if (dyld_glue.lookup) {
        dyld_glue.lookup("__dyld_make_delayed_module_initializer_calls", (void **)&initializers);
    }
    if (initializers) {
        initializers();
    }
}

#endif
