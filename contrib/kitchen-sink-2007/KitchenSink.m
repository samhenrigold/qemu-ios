/*
 * Kitchen Sink, 2007 -- every interesting control iPhone OS 1.0's UIKit has, one screen each, for the
 * emulated M68 (docs/m68/sideload.md). There was no SDK: the interfaces come from 1.0's own class metadata
 * (objc1dump.py -> uikit1ks.h), the app links against 1.0's frameworks, and it is built and sideloaded the
 * way Hello and Tilt are (build.sh here, imgtools/sideload1x.py). README.md lists the classes per screen.
 *
 * Layout: a UINavigationBar over a UITransitionView. The root is a UITable of screens; picking one pushes a
 * UINavigationItem and slides the screen in, and the bar's back button slides back. Every screen ends in a
 * footer naming the classes it shows.
 */
#include "uikit1ks.h"

#define W 320.0f
#define H 416.0f          /* below the status bar and the navigation bar */
#define FOOTER 30.0f

enum { S_BUTTONS, S_CONTROLS, S_TEXT, S_ALERTS, S_PROGRESS, S_PICKERS, S_SCROLL, S_PREFS, S_SECTIONS,
       S_TRANSITIONS, S_ANIMATION, S_NAVBARS, S_BUTTONBAR, S_WEB, S_LABELS, S_COUNT };

static const char *screen_title[S_COUNT] = {
    "Buttons", "Switches & Sliders", "Text & Keyboard", "Alerts & Sheets", "Progress", "Pickers",
    "Scroller & Images", "Preferences Table", "Section List", "Transitions", "Animator", "Navigation Bars",
    "Button Bar", "Web View", "Labels & Callouts",
};
static const char *screen_classes[S_COUNT] = {
    "UIThreePartButton, UIPushButton, UIValueButton, UICheckbox",
    "UISwitchControl, UISliderControl, UISegmentedControl, UIPageIndicator",
    "UITextField, UISearchField, UITextView, UIKeyboard",
    "UIAlertSheet (alert, sheet, text field), UIProgressHUD",
    "UIProgressIndicator, UIProgressBar, NSTimer",
    "UIPickerView, UIDatePicker, UISegmentedControl",
    "UIScroller, UIImageView, UIImage",
    "UIPreferencesTable and its Control, Text and value cells",
    "UISectionList, UITable, UISimpleTableCell",
    "UITransitionView (styles 0-9)",
    "UIAnimator: UIAlphaAnimation, UIRotationAnimation, UIFrameAnimation",
    "UINavigationBar styles 0-2, prompts, button styles",
    "UIButtonBar (the tab bar), badges",
    "UIWebView",
    "UITextLabel, UIDateLabel, UIGradientBar, UICalloutView, UIBox",
};

static CGRect R(float x, float y, float w, float h) { return (CGRect){ { x, y }, { w, h } }; }
static id S(const char *s) { return [NSString stringWithCString:s]; }

static UITextLabel *text(CGRect f, id s, float size, int bold, CGColorRef color, CGColorRef bg, BOOL center)
{
    UITextLabel *l = [[UITextLabel alloc] initWithFrame:f];
    [l setText:s];
    [l setFont:GSFontCreateWithName("Helvetica", bold ? 2 : 0, size)];
    [l setColor:color];
    [l setBackgroundColor:bg];
    [l setCentersHorizontally:center];
    return l;
}

@interface KSApp : UIApplication {
    UIWindow *window;
    UINavigationBar *nav;
    UITransitionView *stage;
    UITable *list;
    UIView *screen;                       /* the screen on stage, or nil at the root */
    int current;
    NSTimer *timer;
    CGColorRef ink, paper, dim, clear, white, accent;
    /* per-screen state */
    UITextLabel *status;
    id a, b, c, d, e;                     /* the screen's live controls */
    float progress;
    int style, page;
    UIPreferencesTableCell *prefCells[5][4];
    int radio;
    UIProgressHUD *hud;
}
@end

@implementation KSApp

/* ---- shell ---- */

- (UIView *)blank:(CGColorRef)bg
{
    UIView *v = [[UIView alloc] initWithFrame:R(0, 0, W, H)];
    [v setBackgroundColor:bg];
    return v;
}

- (void)footer:(UIView *)v
{
    UITextLabel *l = text(R(0, H - FOOTER, W, FOOTER), S(screen_classes[current]), 11, 0, rgb(0.92, 0.94, 0.97),
                          rgb(0.16, 0.19, 0.24), YES);
    [l setWrapsText:YES];
    [v addSubview:l];
}

- (UIThreePartButton *)button:(const char *)title color:(const char *)color frame:(CGRect)f action:(SEL)sel
{
    UIThreePartButton *bt = [[UIThreePartButton alloc] initWithFrame:f];
    [bt setAutosizesToFit:NO];
    [bt setTitle:S(title)];
    [bt setTitleFont:GSFontCreateWithName("Helvetica", 2, 17)];
    [bt setTitleColor:white forState:0];
    [bt setBackgroundImage:[UIImage applicationImageNamed:[NSString stringWithFormat:@"Button-%s.png", color]]];
    [bt setPressedBackgroundImage:[UIImage applicationImageNamed:[NSString stringWithFormat:@"Button-%s-Pressed.png",
                                                                                            color]]];
    UIThreePartSlices sl = { R(0, 0, 14, 44), R(14, 0, 2, 44), R(16, 0, 14, 44) };
    [bt setBackgroundSlices:sl];
    [bt setDrawsShadow:YES];
    [bt setFrame:f];
    if (sel) {
        [bt addTarget:self action:sel forEvents:kUIMouseUpInside];
    }
    return bt;
}

- (UITextLabel *)status:(UIView *)v at:(float)y text:(const char *)s
{
    status = text(R(10, y, W - 20, 24), S(s), 15, 0, ink, clear, YES);
    [v addSubview:status];
    return status;
}

- (void)applicationDidFinishLaunching:(id)unused
{
    CGRect rect = [UIHardware fullScreenApplicationContentRect];
    rect.origin.x = rect.origin.y = 0;
    ink = rgb(0.13, 0.15, 0.19);
    paper = rgb(0.86, 0.88, 0.92);
    dim = rgb(0.45, 0.5, 0.58);
    white = rgb(1, 1, 1);
    accent = rgb(0.16, 0.36, 0.72);
    CGFloat z[4] = { 0, 0, 0, 0 };
    clear = CGColorCreate(CGColorSpaceCreateDeviceRGB(), z);

    window = [[UIWindow alloc] initWithContentRect:rect];
    UIView *main = [[UIView alloc] initWithFrame:rect];
    [main setBackgroundColor:ink];

    nav = [[UINavigationBar alloc] initWithFrame:R(0, 0, W, 44)];
    UINavigationItem *root = [[UINavigationItem alloc] initWithTitle:@"Kitchen Sink"];
    [root setBackButtonTitle:@"Sink"];
    [nav pushNavigationItem:root];
    [nav setDelegate:self];
    [main addSubview:nav];

    stage = [[UITransitionView alloc] initWithFrame:R(0, 44, W, H)];
    [main addSubview:stage];

    list = [[UITable alloc] initWithFrame:R(0, 0, W, H)];
    [list addTableColumn:[[UITableColumn alloc] initWithTitle:@"Screens" identifier:@"screens" width:W]];
    [list setRowHeight:46];
    [list setSeparatorStyle:1];
    [list setDataSource:self];
    [list setDelegate:self];
    [list reloadData];
    [stage transition:0 toView:list];

    [window orderFront:self];
    [window makeKey:self];
    [window _setHidden:NO];
    [window setContentView:main];
    printf("Kitchen Sink, 2007: launched\n");
}

/* The root table, the section list's table and the preferences table all come through here. */
- (int)numberOfRowsInTable:(UITable *)t
{
    if (t == list) {
        return S_COUNT;
    }
    return 26 * 3;                        /* the section list: three names per letter */
}

- (UITableCell *)table:(UITable *)t cellForRow:(int)row column:(UITableColumn *)col
{
    if (t == list) {
        UIImageAndTextTableCell *cell = [[UIImageAndTextTableCell alloc] init];
        [cell setTitle:S(screen_title[row])];
        [cell setImage:[UIImage applicationImageNamed:[NSString stringWithFormat:@"Row%d.png", row]]];
        [cell setShowDisclosure:YES];
        return [cell autorelease];
    }
    static const char *given[] = { "Alex", "Blair", "Casey", "Drew", "Emery", "Finley", "Gray", "Harper", "Indy",
                                   "Jules", "Kai", "Lee", "Morgan", "Noel", "Oakley", "Parker", "Quinn", "Reese",
                                   "Sage", "Taylor", "Uma", "Val", "Wren", "Xan", "Yael", "Zion" };
    static const char *family[] = { "Abbott", "Baker", "Chen" };
    UISimpleTableCell *cell = [[UISimpleTableCell alloc] init];
    [cell setTitle:[NSString stringWithFormat:@"%s %s", given[row / 3 % 26], family[row % 3]]];
    return [cell autorelease];
}

- (void)tableRowSelected:(NSNotification *)n
{
    UITable *t = [n object];
    if (t == list) {
        int row = [list selectedRow];
        if (row >= 0 && row < S_COUNT && !screen) {
            [self show:row];
        }
    } else if (t == (id)a && current == S_PREFS) {
        [self prefsSelected];
    } else if (current == S_SECTIONS) {
        [status setText:[NSString stringWithFormat:@"Row %d", [t selectedRow]]];
    }
}

- (void)show:(int)i
{
    current = i;
    status = nil;
    a = b = c = d = e = nil;
    UINavigationItem *item = [[UINavigationItem alloc] initWithTitle:S(screen_title[i])];
    [nav pushNavigationItem:item];
    switch (i) {
    case S_BUTTONS: screen = [self buttons]; break;
    case S_CONTROLS: screen = [self controls]; break;
    case S_TEXT: screen = [self textScreen]; break;
    case S_ALERTS: screen = [self alerts]; break;
    case S_PROGRESS: screen = [self progressScreen]; break;
    case S_PICKERS: screen = [self pickers]; break;
    case S_SCROLL: screen = [self scroller]; break;
    case S_PREFS: screen = [self prefs]; break;
    case S_SECTIONS: screen = [self sections]; break;
    case S_TRANSITIONS: screen = [self transitions]; break;
    case S_ANIMATION: screen = [self animation]; break;
    case S_NAVBARS: screen = [self navbars]; break;
    case S_BUTTONBAR: screen = [self buttonbar]; break;
    case S_WEB: screen = [self web]; break;
    default: screen = [self labels]; break;
    }
    [self footer:screen];
    [stage transition:1 toView:screen];
}

- (void)navigationBar:(UINavigationBar *)bar poppedItem:(UINavigationItem *)item
{
    if (bar != nav || !screen) {
        return;
    }
    [timer invalidate];
    timer = nil;
    if (hud) {
        [hud show:NO];
        [hud removeFromSuperview];
        hud = nil;
    }
    [stage transition:2 toView:list];
    screen = nil;
    [list selectRow:-1 byExtendingSelection:NO withFade:YES];
}

- (void)navigationBar:(UINavigationBar *)bar buttonClicked:(int)button
{
    if (bar == nav) {
        return;
    }
    /* the Navigation Bars screen's own bars: 0 is the right button, 1 the left */
    style = (style + 1) % 3;
    [(UINavigationBar *)a showLeftButton:@"Left" withStyle:style rightButton:@"Right" withStyle:(style + 1) % 3];
    [status setText:[NSString stringWithFormat:@"%s button; styles now %d / %d", button ? "Left" : "Right", style,
                                                (style + 1) % 3]];
}

/* ---- Buttons ---- */

- (UIView *)buttons
{
    UIView *v = [self blank:paper];
    [v addSubview:text(R(0, 12, W, 20), @"UIThreePartButton", 13, 1, dim, clear, YES)];
    [v addSubview:[self button:"Go" color:"Green" frame:R(20, 36, 130, 44) action:@selector(tapGo)]];
    [v addSubview:[self button:"Delete" color:"Red" frame:R(170, 36, 130, 44) action:@selector(tapDelete)]];
    [v addSubview:[self button:"Cancel" color:"Gray" frame:R(20, 88, 280, 44) action:@selector(tapCancel)]];

    [v addSubview:text(R(0, 144, W, 20), @"UIPushButton with an image", 13, 1, dim, clear, YES)];
    UIPushButton *star = [[UIPushButton alloc] initWithTitle:@"Favorite" autosizesToFit:NO];
    [star setFrame:R(40, 166, 240, 40)];
    [star setDrawContentsCentered:YES];
    [star setImage:[UIImage applicationImageNamed:@"Star.png"] forState:0];
    [star setImage:[UIImage applicationImageNamed:@"StarOn.png"] forState:4];
    [star setTitleFont:GSFontCreateWithName("Helvetica", 2, 18)];
    [star setTitleColor:accent forState:0];
    [star setShowPressFeedback:YES];
    [star addTarget:self action:@selector(tapStar:) forEvents:kUIMouseUpInside];
    [v addSubview:star];
    a = star;

    [v addSubview:text(R(0, 216, W, 20), @"UIValueButton and UICheckbox", 13, 1, dim, clear, YES)];
    UIValueButton *val = [[UIValueButton alloc] initWithTitle:@"Ringtone"];
    [val setLabel:@"Ringtone"];
    [val setFrame:R(20, 238, 280, 44)];
    [val setValue:@"Marimba"];
    [val setShowsDisclosure:YES];
    [val addTarget:self action:@selector(tapValue:) forEvents:kUIMouseUpInside];
    [v addSubview:val];
    b = val;
    UICheckbox *check = [[UICheckbox alloc] initWithTitle:@"Remember me"];
    [check setFrame:R(20, 292, 280, 32)];
    [check addTarget:self action:@selector(tapCheck:) forEvents:kUIMouseUpInside];
    [v addSubview:check];
    c = check;
    [self status:v at:H - FOOTER - 52 text:"Tap a button."];
    return v;
}

- (void)tapGo { [status setText:@"Go!"]; }
- (void)tapDelete { [status setText:@"Deleted (not really)."]; }
- (void)tapCancel { [status setText:@"Cancelled."]; }
- (void)tapStar:(UIPushButton *)s
{
    [s setSelected:![s isSelected]];
    [status setText:[s isSelected] ? @"Favorited." : @"Not a favorite."];
}
- (void)tapValue:(id)s
{
    static const char *tones[] = { "Marimba", "Alarm", "Ascending", "Bark", "Bell Tower", "Blues", "Boing" };
    page = (page + 1) % 7;
    [(UIValueButton *)b setValue:S(tones[page])];
    [status setText:[NSString stringWithFormat:@"Ringtone: %s", tones[page]]];
}
- (void)tapCheck:(UICheckbox *)s
{
    [s setChecked:![s isChecked]];
    [status setText:[s isChecked] ? @"Will remember." : @"Won't remember."];
}

/* ---- Switches & Sliders ---- */

- (UIView *)controls
{
    UIView *v = [self blank:paper];
    [v addSubview:text(R(20, 18, 140, 24), @"Wi-Fi", 17, 1, ink, clear, NO)];
    UISwitchControl *sw = [[UISwitchControl alloc] initWithFrame:R(206, 12, 94, 28)];
    [sw setValue:1];
    [sw addTarget:self action:@selector(controlChanged:) forEvents:kUIAllEvents];
    [v addSubview:sw];
    a = sw;
    [v addSubview:text(R(20, 60, 160, 24), @"Airplane Mode", 17, 1, ink, clear, NO)];
    UISwitchControl *alt = [[UISwitchControl alloc] initWithFrame:R(206, 54, 94, 28)];
    [alt setAlternateColors:YES];
    [alt addTarget:self action:@selector(controlChanged:) forEvents:kUIAllEvents];
    [v addSubview:alt];
    b = alt;

    UISliderControl *sl = [[UISliderControl alloc] initWithFrame:R(20, 100, 280, 28)];
    [sl setMinValue:0];
    [sl setMaxValue:100];
    [sl setValue:40];
    [sl setShowValue:YES];
    [sl setContinuous:YES];
    [sl addTarget:self action:@selector(controlChanged:) forEvents:kUIAllEvents];
    [v addSubview:sl];
    c = sl;
    UISliderControl *ticks = [[UISliderControl alloc] initWithFrame:R(20, 140, 280, 28)];
    [ticks setMinValue:0];
    [ticks setMaxValue:4];
    [ticks setNumberOfTickMarks:5];
    [ticks setValue:2];
    [ticks addTarget:self action:@selector(controlChanged:) forEvents:kUIAllEvents];
    [v addSubview:ticks];

    float y = 186;
    for (int st = 0; st < 3; st++) {
        UISegmentedControl *seg = [[UISegmentedControl alloc]
            initWithFrame:R(20, y, 280, st == 2 ? 30 : 44) withStyle:st
                withItems:[NSArray arrayWithObjects:@"Map", @"Satellite", @"Hybrid", nil]];
        [seg selectSegment:st];
        [seg setDelegate:self];
        [v addSubview:seg];
        y += st == 2 ? 40 : 52;
    }
    UIPageIndicator *pages = [[UIPageIndicator alloc] initWithFrame:R(0, y, W, 20)];
    [pages setPageCount:5];
    [pages setCurrentPage:0];
    [pages addTarget:self action:@selector(pageTapped:) forEvents:kUIMouseUpInside];
    [v addSubview:pages];
    d = pages;
    [self status:v at:H - FOOTER - 28 text:"Wi-Fi on, airplane off, slider 40"];
    return v;
}

- (void)controlChanged:(UISliderControl *)s
{
    [status setText:[NSString stringWithFormat:@"Wi-Fi %s, airplane %s, slider %d",
                                               [(UISwitchControl *)a value] > 0.5f ? "on" : "off",
                                               [(UISwitchControl *)b value] > 0.5f ? "on" : "off",
                                               (int)([(UISliderControl *)c value] + 0.5f)]];
}

- (void)segmentedControl:(UISegmentedControl *)seg selectedSegmentChanged:(int)i
{
    if (current == S_PICKERS) {
        [self pickerMode:i];
        return;
    }
    static const char *names[] = { "Map", "Satellite", "Hybrid" };
    [status setText:[NSString stringWithFormat:@"Segment: %s", names[i % 3]]];
}

- (void)pageTapped:(UIPageIndicator *)p
{
    page = (page + 1) % 5;
    [p setCurrentPage:page];
    [status setText:[NSString stringWithFormat:@"Page %d of 5", page + 1]];
}

/* ---- Text & Keyboard ---- */

- (UIView *)textScreen
{
    UIView *v = [self blank:paper];
    UITextField *field = [[UITextField alloc] initWithFrame:R(14, 10, 292, 32)];
    [field setBorderStyle:1];
    [field setPlaceholder:@"Name"];
    [field setFont:GSFontCreateWithName("Helvetica", 0, 18)];
    [field setPaddingTop:6 paddingLeft:8];
    [field setClearButtonStyle:2];
    [v addSubview:field];
    a = field;
    UISearchField *search = [[UISearchField alloc] initWithFrame:R(14, 50, 292, 30)];
    [search setPlaceholder:@"Search"];
    [search setFont:GSFontCreateWithName("Helvetica", 0, 16)];
    [search setPaddingTop:5 paddingLeft:26];
    [v addSubview:search];
    UITextView *tv = [[UITextView alloc] initWithFrame:R(14, 88, 292, 72)];
    [tv setText:@"A UITextView: a scrolling, editable WebKit-backed text area. Tap here and type."];
    [tv setTextSize:15];
    [tv setEditable:YES];
    [v addSubview:tv];
    CGSize ks = [UIKeyboard defaultSize];
    UIKeyboard *kb = [[UIKeyboard alloc] initWithFrame:R(0, H - FOOTER - ks.height, ks.width, ks.height)];
    [v addSubview:kb];
    [field becomeFirstResponder];
    return v;
}

/* ---- Alerts & Sheets ---- */

- (UIView *)alerts
{
    UIView *v = [self blank:paper];
    [v addSubview:[self button:"Alert" color:"Gray" frame:R(20, 20, 280, 44) action:@selector(showAlert)]];
    [v addSubview:[self button:"Action Sheet" color:"Gray" frame:R(20, 74, 280, 44) action:@selector(showSheet)]];
    [v addSubview:[self button:"Alert with Text Field" color:"Gray" frame:R(20, 128, 280, 44)
                         action:@selector(showFieldAlert)]];
    [v addSubview:[self button:"Progress HUD" color:"Green" frame:R(20, 182, 280, 44) action:@selector(showHUD)]];
    [self status:v at:250 text:"Pick one."];
    return v;
}

- (void)showAlert
{
    UIAlertSheet *s = [[UIAlertSheet alloc] initWithTitle:@"Software Update"
                                                  buttons:[NSArray arrayWithObjects:@"Later", @"Install", nil]
                                       defaultButtonIndex:1 delegate:self context:nil];
    [s setBodyText:@"iPhone OS 1.0.1 is available. This is a UIAlertSheet in its alert style."];
    [s popupAlertAnimated:YES];
}

- (void)showSheet
{
    UIAlertSheet *s = [[UIAlertSheet alloc] initWithFrame:R(0, 240, W, 240)];
    [s setTitle:@"Delete this photo?"];
    [s setDestructiveButton:[s addButtonWithTitle:@"Delete Photo"]];
    [s addButtonWithTitle:@"Cancel"];
    [s setDelegate:self];
    [s presentSheetInView:screen];
}

- (void)showFieldAlert
{
    UIAlertSheet *s = [[UIAlertSheet alloc] initWithTitle:@"Join Network"
                                                  buttons:[NSArray arrayWithObjects:@"Cancel", @"Join", nil]
                                       defaultButtonIndex:1 delegate:self context:nil];
    [s setBodyText:@"Enter the password for \"qemu-ios\"."];
    [s addTextFieldWithValue:@"" label:@"Password"];
    [s popupAlertAnimated:YES];
}

- (void)alertSheet:(UIAlertSheet *)s buttonClicked:(int)button
{
    [status setText:[NSString stringWithFormat:@"Button %d", button]];
    [s dismiss];
}

- (void)showHUD
{
    hud = [[UIProgressHUD alloc] initWithWindow:window];
    [hud setText:@"Syncing..."];
    [hud show:YES];
    [screen addSubview:hud];
    [self performSelector:@selector(hideHUD) withObject:nil afterDelay:3.0];
}

- (void)hideHUD
{
    if (hud) {
        [hud done];
        [hud setText:@"Done"];
        [self performSelector:@selector(dropHUD) withObject:nil afterDelay:1.0];
    }
}

- (void)dropHUD
{
    [hud show:NO];
    [hud removeFromSuperview];
    hud = nil;
}

/* ---- Progress ---- */

- (UIView *)progressScreen
{
    UIView *v = [self blank:paper];
    UIView *dark = [[UIView alloc] initWithFrame:R(0, 0, W, 100)];
    [dark setBackgroundColor:ink];
    [v addSubview:dark];
    for (int st = 0; st < 3; st++) {
        CGSize sz = [UIProgressIndicator defaultSizeForStyle:st];
        UIProgressIndicator *p = [[UIProgressIndicator alloc]
            initWithFrame:R(60 + st * 90 - sz.width / 2, (st == 1 ? 150 : 50) - sz.height / 2, sz.width, sz.height)];
        [p setStyle:st];
        [p startAnimation];
        [st == 1 ? v : dark addSubview:p];
        [(st == 1 ? v : dark) addSubview:text(R(st * 90 + 15, st == 1 ? 172 : 76, 90, 18),
                                              [NSString stringWithFormat:@"style %d", st], 12, 0,
                                              st == 1 ? dim : white, clear, YES)];
    }
    UIProgressBar *bar0 = [[UIProgressBar alloc] initWithFrame:R(20, 220, 280, 10)];
    [bar0 setStyle:0];
    [v addSubview:bar0];
    a = bar0;
    UIProgressBar *bar1 = [[UIProgressBar alloc] initWithFrame:R(20, 250, 280, 10)];
    [bar1 setStyle:1];
    [v addSubview:bar1];
    b = bar1;
    [self status:v at:280 text:"Downloading..."];
    progress = 0;
    timer = [NSTimer scheduledTimerWithTimeInterval:0.1 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
    return v;
}

- (void)tick:(NSTimer *)t
{
    if (current == S_PICKERS) {
        [self pickerTick];
        return;
    }
    if (current == S_LABELS) {
        [(UIDateLabel *)a setDate:[NSDate date]];
        return;
    }
    progress += 0.01f;
    if (progress > 1.0f) {
        progress = 0;
    }
    [(UIProgressBar *)a setProgress:progress];
    [(UIProgressBar *)b setProgress:1.0 - progress];
    [status setText:[NSString stringWithFormat:@"Downloading... %d%%", (int)(progress * 100)]];
}

/* ---- Pickers ---- */

- (UIView *)pickers
{
    UIView *v = [self blank:paper];
    UISegmentedControl *seg = [[UISegmentedControl alloc]
        initWithFrame:R(10, 8, 300, 30) withStyle:2
            withItems:[NSArray arrayWithObjects:@"Picker", @"Time", @"Date", @"Both", @"Timer", nil]];
    [seg selectSegment:0];
    [seg setDelegate:self];
    [v addSubview:seg];
    CGSize ps = [UIPickerView defaultSize];
    UIPickerView *pk = [[UIPickerView alloc] initWithFrame:R(0, 46, ps.width, ps.height)];
    [pk setDelegate:self];
    [pk setSoundsEnabled:NO];
    [v addSubview:pk];
    [pk selectRow:3 inColumn:0 animated:NO];
    [pk selectRow:1 inColumn:1 animated:NO];
    a = pk;
    UIDatePicker *dp = [[UIDatePicker alloc] initWithFrame:R(0, 46, ps.width, ps.height)];
    [dp setDate:[NSDate date]];
    b = dp;
    c = v;
    [self status:v at:46 + ps.height + 10 text:""];
    timer = [NSTimer scheduledTimerWithTimeInterval:0.3 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
    return v;
}

static const char *fruit[] = { "Apple", "Banana", "Cherry", "Grape", "Lemon", "Mango", "Orange", "Peach", "Pear",
                               "Plum" };
static const char *colors[] = { "Red", "Green", "Blue", "Yellow", "Purple" };

- (int)numberOfColumnsInPickerView:(UIPickerView *)p { return 2; }
- (int)pickerView:(UIPickerView *)p numberOfRowsInColumn:(int)col { return col ? 5 : 10; }
- (id)pickerView:(UIPickerView *)p titleForRow:(int)row inColumn:(int)col
{
    return S(col ? colors[row % 5] : fruit[row % 10]);
}
- (float)pickerView:(UIPickerView *)p tableWidthForColumn:(int)col { return col ? 120 : 170; }

- (void)pickerMode:(int)i
{
    UIPickerView *pk = a;
    UIDatePicker *dp = b;
    if (i == 0) {
        [dp removeFromSuperview];
        [(UIView *)c addSubview:pk];
    } else {
        [pk removeFromSuperview];
        [dp setDatePickerMode:i - 1];
        [(UIView *)c addSubview:dp];
    }
    style = i;
}

- (void)pickerTick
{
    if (style == 0) {
        [status setText:[NSString stringWithFormat:@"%s %s", colors[[(UIPickerView *)a selectedRowForColumn:1] % 5],
                                                   fruit[[(UIPickerView *)a selectedRowForColumn:0] % 10]]];
    } else {
        [status setText:[NSString stringWithFormat:@"UIDatePicker mode %d", style - 1]];
    }
}

/* ---- Scroller & Images ---- */

- (UIView *)scroller
{
    UIView *v = [self blank:ink];
    UIScroller *sc = [[UIScroller alloc] initWithFrame:R(0, 0, W, H - FOOTER)];
    UIImage *img = [UIImage applicationImageNamed:@"Map.png"];
    UIImageView *iv = [[UIImageView alloc] initWithImage:img];
    [sc setContentSize:[img size]];
    [sc addSubview:iv];
    [sc setAllowsRubberBanding:YES];
    [sc setAllowsFourWayRubberBanding:YES];
    [sc setShowScrollerIndicators:YES];
    [sc setOffset:(CGPoint){ 160, 200 }];
    [v addSubview:sc];
    return v;
}

/* ---- Preferences Table ---- */

static const char *pref_groups[] = { "Network", "Account", "Sounds", "Auto-Lock" };
static const int pref_rows[] = { 2, 2, 2, 4 };

- (UIView *)prefs
{
    UIView *v = [self blank:paper];
    memset(prefCells, 0, sizeof prefCells);
    radio = 1;
    UIPreferencesTable *t = [[UIPreferencesTable alloc] initWithFrame:R(0, 0, W, H - FOOTER)];
    [t setDataSource:self];
    [t setDelegate:self];
    [t reloadData];
    [v addSubview:t];
    a = t;
    return v;
}

- (int)numberOfGroupsInPreferencesTable:(UIPreferencesTable *)t { return 4; }
- (int)preferencesTable:(UIPreferencesTable *)t numberOfRowsInGroup:(int)g { return pref_rows[g]; }
- (id)preferencesTable:(UIPreferencesTable *)t titleForGroup:(int)g { return S(pref_groups[g]); }
- (float)preferencesTable:(UIPreferencesTable *)t heightForRow:(int)r inGroup:(int)g withProposedHeight:(float)h
{
    return h;
}

- (id)preferencesTable:(UIPreferencesTable *)t cellForRow:(int)r inGroup:(int)g
{
    if (prefCells[g][r]) {
        return prefCells[g][r];
    }
    UIPreferencesTableCell *cell;
    if (g == 0) {
        UIPreferencesControlTableCell *cc = [[UIPreferencesControlTableCell alloc] init];
        [cc setTitle:r ? @"Bluetooth" : @"Wi-Fi"];
        UISwitchControl *sw = [[UISwitchControl alloc] initWithFrame:R(206, 9, 94, 28)];
        [sw setValue:r ? 0 : 1];
        [cc setControl:sw];
        cell = cc;
    } else if (g == 1) {
        UIPreferencesTextTableCell *tc = [[UIPreferencesTextTableCell alloc] init];
        [tc setTitle:r ? @"Password" : @"Name"];
        [tc setPlaceHolderValue:r ? @"Required" : @"Steve"];
        if (r) {
            [[tc textField] setSecure:YES];
        }
        cell = tc;
    } else if (g == 2) {
        cell = [[UIPreferencesTableCell alloc] init];
        [cell setTitle:r ? @"Ringtone" : @"New Voicemail"];
        [cell setValue:r ? @"Marimba" : @"Tri-tone"];
        [cell setShowDisclosure:YES];
    } else {
        static const char *mins[] = { "1 Minute", "2 Minutes", "5 Minutes", "Never" };
        cell = [[UIPreferencesTableCell alloc] init];
        [cell setTitle:S(mins[r])];
        [cell setChecked:r == radio];
    }
    prefCells[g][r] = cell;
    return cell;
}

- (void)prefsSelected
{
    UIPreferencesTable *t = a;
    int row = [t selectedRow];
    /* table rows count each group's title as a row: groups 0-2 take rows 0-8, Auto-Lock's title row 9 */
    int r = row - 10;
    if (r >= 0 && r < 4) {
        [prefCells[3][radio] setChecked:NO];
        radio = r;
        [prefCells[3][radio] setChecked:YES];
    }
    [[t cellAtRow:row column:0] setSelected:NO withFade:YES];
}

/* ---- Section List ---- */

- (UIView *)sections
{
    UIView *v = [self blank:white];
    UISectionList *sl = [[UISectionList alloc] initWithFrame:R(0, 0, W, H - FOOTER - 26) showSectionIndex:YES];
    [sl setDataSource:self];
    UITable *t = [sl table];
    [t addTableColumn:[[UITableColumn alloc] initWithTitle:@"Names" identifier:@"names" width:W]];
    [t setSeparatorStyle:1];
    [t setDataSource:self];
    [t setDelegate:self];
    [sl reloadData];
    [v addSubview:sl];
    [self status:v at:H - FOOTER - 25 text:"Contacts-style, with the index"];
    return v;
}

- (int)numberOfSectionsInSectionList:(UISectionList *)l { return 26; }
- (id)sectionList:(UISectionList *)l titleForSection:(int)s { return [NSString stringWithFormat:@"%c", 'A' + s]; }
- (int)sectionList:(UISectionList *)l rowForSection:(int)s { return s * 3; }

/* ---- Transitions ---- */

- (UIView *)card:(int)n
{
    static const float tints[][3] = { { 0.85, 0.3, 0.25 }, { 0.2, 0.6, 0.35 }, { 0.2, 0.4, 0.8 }, { 0.75, 0.55, 0.1 } };
    const float *c3 = tints[n % 4];
    UIView *card = [[UIView alloc] initWithFrame:R(0, 0, 280, 220)];
    [card setBackgroundColor:rgb(c3[0], c3[1], c3[2])];
    [card addSubview:text(R(0, 80, 280, 50), [NSString stringWithFormat:@"Card %d", n + 1], 36, 1, white, clear, YES)];
    return card;
}

- (UIView *)transitions
{
    UIView *v = [self blank:paper];
    UITransitionView *tv = [[UITransitionView alloc] initWithFrame:R(20, 16, 280, 220)];
    [v addSubview:tv];
    [tv transition:0 toView:[self card:0]];
    a = tv;
    style = 0;
    page = 0;
    [v addSubview:[self button:"Next Transition" color:"Green" frame:R(20, 252, 280, 44)
                         action:@selector(nextTransition)]];
    [self status:v at:306 text:"Styles 1-9 slide, push, flip and fade"];
    return v;
}

- (void)nextTransition
{
    style = style % 9 + 1;
    page++;
    [(UITransitionView *)a transition:style toView:[self card:page]];
    [status setText:[NSString stringWithFormat:@"transition:%d toView:", style]];
}

/* ---- Animator ---- */

- (UIView *)animation
{
    UIView *v = [self blank:paper];
    UIView *box = [[UIView alloc] initWithFrame:R(110, 40, 100, 100)];
    [box setBackgroundColor:accent];
    [box addSubview:text(R(0, 36, 100, 28), @"1.0", 24, 1, white, clear, YES)];
    [v addSubview:box];
    a = box;
    [v addSubview:[self button:"Fade" color:"Gray" frame:R(20, 200, 86, 44) action:@selector(fade)]];
    [v addSubview:[self button:"Spin" color:"Gray" frame:R(117, 200, 86, 44) action:@selector(spin)]];
    [v addSubview:[self button:"Move" color:"Gray" frame:R(214, 200, 86, 44) action:@selector(move)]];
    [self status:v at:260 text:"UIAnimator runs UIAnimations."];
    return v;
}

- (void)fade
{
    UIAlphaAnimation *an = [[UIAlphaAnimation alloc] initWithTarget:a];
    [an setStartAlpha:page ? 0.15f : 1];
    [an setEndAlpha:page ? 1 : 0.15f];
    page = !page;
    [[UIAnimator sharedAnimator] addAnimation:an withDuration:0.8 start:YES];
    [status setText:@"UIAlphaAnimation"];
}

- (void)spin
{
    UIRotationAnimation *an = [[UIRotationAnimation alloc] initWithTarget:a];
    [an setStartRotationAngle:0];
    [an setEndRotationAngle:6.2831853f];   /* a full turn, so Move starts from upright */
    [[UIAnimator sharedAnimator] addAnimation:an withDuration:1.0 start:YES];
    [status setText:@"UIRotationAnimation"];
}

- (void)move
{
    CGAffineTransform upright = { 1, 0, 0, 1, 0, 0 };
    [(UIView *)a setTransform:upright];  /* a finished spin can leave a sliver of rotation */
    CGRect f = [(UIView *)a frame];
    UIFrameAnimation *an = [[UIFrameAnimation alloc] initWithTarget:a];
    [an setStartFrame:f];
    f.origin.x = f.origin.x > 100 ? 20 : 200;
    f.origin.y = f.origin.y > 30 ? 20 : 60;
    [an setEndFrame:f];
    [an setAnimationCurve:1];
    [[UIAnimator sharedAnimator] addAnimation:an withDuration:0.6 start:YES];
    [status setText:@"UIFrameAnimation"];
}

/* ---- Navigation Bars ---- */

- (UIView *)navbars
{
    UIView *v = [self blank:paper];
    float y = 14;
    for (int st = 0; st < 3; st++) {
        UINavigationBar *nb = [[UINavigationBar alloc] initWithFrame:R(0, y, W, 44)];
        [nb setBarStyle:st];
        [nb pushNavigationItem:[[UINavigationItem alloc] initWithTitle:[NSString stringWithFormat:@"barStyle %d", st]]];
        [nb showLeftButton:@"Left" withStyle:0 rightButton:@"Right" withStyle:st];
        [nb setDelegate:self];
        [v addSubview:nb];
        if (st == 0) {
            a = nb;
        }
        y += 58;
    }
    UINavigationBar *pr = [[UINavigationBar alloc] initWithFrame:R(0, y, W, 74)];
    [pr setPrompt:@"A prompt, above the title"];
    [pr pushNavigationItem:[[UINavigationItem alloc] initWithTitle:@"With Prompt"]];
    [pr showButtonsWithLeftTitle:@"Back" rightTitle:@"Done" leftBack:YES];
    [pr setDelegate:self];
    [v addSubview:pr];
    [self status:v at:y + 86 text:"Tap a bar button to cycle its styles."];
    return v;
}

/* ---- Button Bar ---- */

- (UIView *)buttonbar
{
    UIView *v = [self blank:paper];
    static const char *names[] = { "Favorites", "Recents", "Contacts", "Keypad", "Voicemail" };
    id items[5];
    for (int i = 0; i < 5; i++) {
        items[i] = [NSDictionary dictionaryWithObjectsAndKeys:
            @"buttonBarItemTapped:", kUIButtonBarButtonAction,
            [NSString stringWithFormat:@"Tab%d.png", i], kUIButtonBarButtonInfo,
            [NSString stringWithFormat:@"Tab%dSelected.png", i], kUIButtonBarButtonSelectedInfo,
            [NSNumber numberWithInt:i + 1], kUIButtonBarButtonTag,
            self, kUIButtonBarButtonTarget,
            S(names[i]), kUIButtonBarButtonTitle,
            @"0", kUIButtonBarButtonType, nil];
    }
    float bh = [UIButtonBar defaultHeight];
    UIButtonBar *bar = [[UIButtonBar alloc] initInView:v withFrame:R(0, H - FOOTER - bh, W, bh)
                                          withItemList:[NSArray arrayWithObjects:items[0], items[1], items[2],
                                                                                  items[3], items[4], nil]];
    int tags[5] = { 1, 2, 3, 4, 5 };
    [bar registerButtonGroup:0 withButtons:tags withCount:5];
    [bar showButtonGroup:0 withDuration:0];
    [bar setBarStyle:1];
    [bar showSelectionForButton:1];
    [bar setBadgeValue:@"3" forButton:5];
    [v addSubview:bar];
    a = bar;
    b = text(R(0, 120, W, 40), @"Favorites", 30, 1, ink, clear, YES);
    [v addSubview:b];
    [self status:v at:170 text:"Tap a button in the bar."];
    return v;
}

- (void)buttonBarItemTapped:(id)button
{
    static const char *names[] = { "Favorites", "Recents", "Contacts", "Keypad", "Voicemail" };
    int tag = [button tag];
    if (tag >= 1 && tag <= 5) {
        [(UIButtonBar *)a showSelectionForButton:tag];
        [(UITextLabel *)b setText:S(names[tag - 1])];
        if (tag == 5) {
            [(UIButtonBar *)a setBadgeValue:nil forButton:5];
        }
    }
}

/* ---- Web View ---- */

- (UIView *)web
{
    UIView *v = [self blank:white];
    UIWebView *wv = [[UIWebView alloc] initWithFrame:R(0, 0, W, H - FOOTER)];
    [wv setAutoresizes:YES];
    [wv loadHTMLString:@"<html><head><meta name='viewport' content='width=320'></head><body style='font-family:Helvetica;margin:14px;background:#f4f6fa'>"
                        "<h2 style='color:#28508f;margin:4px 0'>UIWebView</h2>"
                        "<p>This is <b>WebKit</b> inside a 1.0 app, the same engine as MobileSafari, "
                        "loaded from a string with <code>loadHTMLString:baseURL:</code>.</p>"
                        "<ul><li>CSS <span style='color:#c33'>colors</span> and "
                        "<span style='font-style:italic'>styles</span></li><li>lists and tables</li></ul>"
                        "<table border=1 cellpadding=4 style='border-collapse:collapse'>"
                        "<tr><th>Device</th><th>OS</th></tr><tr><td>iPhone</td><td>1.0 (1A543a)</td></tr>"
                        "</table><p style='color:#888'>Pinch is not modelled; scroll with a drag.</p>"
                        "</body></html>"
                baseURL:nil];
    [v addSubview:wv];
    return v;
}

/* ---- Labels & Callouts ---- */

- (UIView *)labels
{
    UIView *v = [self blank:paper];
    UIGradientBar *gb = [[UIGradientBar alloc] initWithFrame:R(0, 0, W, 44)];
    [v addSubview:gb];
    [v addSubview:text(R(0, 10, W, 24), @"UIGradientBar", 17, 1, white, clear, YES)];

    UITextLabel *big = text(R(10, 54, 300, 40), @"Embossed", 32, 1, rgb(0.25, 0.3, 0.4), clear, YES);
    [big setShadowColor:white];
    [big setShadowOffset:(CGSize){ 0, 1 }];
    [v addSubview:big];
    UITextLabel *wrap = text(R(20, 96, 280, 54), @"A UITextLabel that wraps its text onto as many lines as "
                                                  "the frame allows, as 1.0's alerts do.",
                             14, 0, ink, clear, YES);
    [wrap setWrapsText:YES];
    [v addSubview:wrap];

    UIDateLabel *date = [[UIDateLabel alloc] initWithFrame:R(20, 158, 280, 24)];
    [date setDate:[NSDate date]];
    [date setColor:accent];
    [date setBackgroundColor:clear];
    [date sizeToFit];
    CGRect df = [date frame];
    df.origin.x = (W - df.size.width) / 2;
    [date setFrame:df];
    [v addSubview:date];
    a = date;

    UIBox *box = [[UIBox alloc] initWithFrame:R(40, 196, 240, 60)];
    [box setRoundedCorners:15];
    [box setBackgroundColor:rgb(0.2, 0.55, 0.35)];
    [v addSubview:box];
    [v addSubview:text(R(40, 214, 240, 24), @"UIBox, rounded", 17, 1, white, clear, YES)];

    UICalloutView *co = [[UICalloutView alloc] initWithFrame:R(0, 0, 200, 60)];
    [co setTitle:@"Apple Inc."];
    [co setSubtitle:@"1 Infinite Loop, Cupertino"];
    [v addSubview:co];
    [co setAnchorPoint:(CGPoint){ 160, 336 } boundaryRect:R(0, 260, W, 120) animate:YES];
    UIView *pin = [[UIView alloc] initWithFrame:R(156, 336, 8, 8)];
    [pin setBackgroundColor:rgb(0.85, 0.15, 0.15)];
    [v addSubview:pin];
    timer = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
    return v;
}

@end

int main(int argc, char **argv)
{
    run_library_initializers();
    [[NSAutoreleasePool alloc] init];
    return UIApplicationMain(argc, argv, [KSApp class]);
}
