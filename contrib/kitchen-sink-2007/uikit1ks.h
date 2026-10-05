/*
 * The rest of iPhone OS 1.0's UIKit that the kitchen sink uses, on top of ../hello-2007/uikit1.h. Written by
 * hand from the firmware's own class metadata (objc1dump.py on 1.0's UIKit, the way class-dump headers were):
 * the selectors and their argument types are 1.0's, not a later SDK's. Classes the app only messages are
 * declared without ivars; the fragile ABI needs real instance sizes only for classes it subclasses.
 */
#ifndef UIKIT1KS_H
#define UIKIT1KS_H
#include "../hello-2007/uikit1.h"

typedef struct { CGRect left, middle, right; } UIThreePartSlices;
typedef struct { CGFloat a, b, c, d, tx, ty; } CGAffineTransform;
extern GSFontRef GSFontCreateWithName(const char *, int, CGFloat);
extern void *memset(void *, int, unsigned long);
extern float sinf(float), cosf(float);

/* Foundation 1.0 */
@interface NSObject (KS)
- (id)retain;
- (void)release;
- (id)autorelease;
- (void)performSelector:(SEL)sel withObject:(id)o afterDelay:(double)delay;
@end
@interface NSString (KS)
+ (id)stringWithCString:(const char *)s;
@end
@interface NSArray (KS)
+ (id)arrayWithObjects:(id)first, ...;
- (id)objectAtIndex:(unsigned)i;
- (unsigned)count;
@end
@interface NSDictionary : NSObject
+ (id)dictionaryWithObjectsAndKeys:(id)first, ...;
@end
@interface NSNumber : NSObject
+ (id)numberWithInt:(int)i;
+ (id)numberWithFloat:(float)f;
@end
@interface NSTimer : NSObject
+ (id)scheduledTimerWithTimeInterval:(double)t target:(id)target selector:(SEL)sel userInfo:(id)info repeats:(BOOL)r;
- (void)invalidate;
@end
@interface NSDate : NSObject
+ (id)date;
+ (id)dateWithTimeIntervalSinceNow:(double)t;
@end
@interface NSNotification : NSObject
- (id)object;
@end
extern id kUIButtonBarButtonAction, kUIButtonBarButtonInfo, kUIButtonBarButtonSelectedInfo, kUIButtonBarButtonStyle,
    kUIButtonBarButtonTag, kUIButtonBarButtonTarget, kUIButtonBarButtonTitle, kUIButtonBarButtonType;

/* UIView's categories (geometry, hierarchy) */
@interface UIView (KS)
- (CGRect)frame;
- (CGRect)bounds;
- (void)setFrame:(CGRect)f;
- (void)removeFromSuperview;
- (void)setAlpha:(float)a;
- (void)setEnabled:(BOOL)e;
- (id)superview;
- (void)setTapDelegate:(id)d;
- (int)tag;
- (void)setTransform:(CGAffineTransform)t;
@end
@interface UIWindow (KS)
- (id)contentView;
@end
@interface UITextLabel (KS)
- (void)setWrapsText:(BOOL)w;
- (void)setShadowColor:(CGColorRef)c;
- (void)setShadowOffset:(CGSize)s;
- (void)sizeToFit;
@end
@interface UINavigationItem (KS)
- (void)setBackButtonTitle:(id)t;
@end
@interface UINavigationBar (KS)
- (void)setBarStyle:(int)s;
- (void)setPrompt:(id)p;
- (void)popNavigationItem;
- (void)showLeftButton:(id)l withStyle:(int)ls rightButton:(id)r withStyle:(int)rs;
- (void)hideButtons;
- (id)topItem;
@end
@interface UIAlertSheet (KS)
- (id)initWithFrame:(CGRect)f;
- (void)setTitle:(id)t;
- (id)addButtonWithTitle:(id)t;
- (void)setDestructiveButton:(id)b;
- (void)setAlertSheetStyle:(int)s;
- (void)presentSheetInView:(UIView *)v;
- (void)setDelegate:(id)d;
- (id)addTextFieldWithValue:(id)v label:(id)l;
- (id)textField;
- (id)buttons;
- (void)setDimsBackground:(BOOL)d;
@end

/* Images */
@interface UIImage : NSObject
+ (id)applicationImageNamed:(id)name;
+ (id)imageNamed:(id)name;
+ (id)imageAtPath:(id)path;
- (CGSize)size;
@end
@interface UIImageView : UIView
- (id)initWithImage:(UIImage *)i;
- (void)setImage:(UIImage *)i;
@end

/* Controls */
@interface UIControl : UIView
- (void)addTarget:(id)t action:(SEL)a forEvents:(int)mask;
@end
#define kUIMouseDown 1
#define kUIMouseUpInside 0x40
#define kUIAllEvents 0xFFFFFFF
@interface UIPushButton : UIControl
- (id)initWithTitle:(id)t autosizesToFit:(BOOL)a;
- (void)setTitle:(id)t;
- (void)setTitleFont:(GSFontRef)f;
- (void)setTitleColor:(CGColorRef)c forState:(unsigned)state;
- (void)setBackground:(UIImage *)i forState:(unsigned)state;
- (void)setImage:(UIImage *)i forState:(unsigned)state;
- (void)setDrawsShadow:(BOOL)d;
- (void)setShowPressFeedback:(BOOL)f;
- (void)setStretchBackground:(BOOL)s;
- (void)setDrawContentsCentered:(BOOL)c;
- (void)setAutosizesToFit:(BOOL)a;
- (void)setSelected:(BOOL)s;
- (BOOL)isSelected;
@end
@interface UIThreePartButton : UIPushButton
- (void)setBackgroundImage:(UIImage *)i;
- (void)setPressedBackgroundImage:(UIImage *)i;
- (void)setBackgroundSlices:(UIThreePartSlices)s;
@end
@interface UIValueButton : UIThreePartButton
- (id)initWithTitle:(id)t;
- (void)setValue:(id)v;
- (void)setLabel:(id)l;
- (void)setShowsDisclosure:(BOOL)s;
- (void)sizeToFit;
@end
@interface UICheckbox : UIControl
- (id)initWithTitle:(id)t;
- (void)setChecked:(BOOL)c;
- (BOOL)isChecked;
@end
@interface UISliderControl : UIControl
- (void)setMinValue:(float)v;
- (void)setMaxValue:(float)v;
- (void)setValue:(float)v;
- (float)value;
- (void)setShowValue:(BOOL)s;
- (void)setContinuous:(BOOL)c;
- (void)setNumberOfTickMarks:(int)n;
- (void)setValue:(float)v animated:(BOOL)a;
@end
@interface UISwitchControl : UISliderControl
- (void)setAlternateColors:(BOOL)a;
@end
@interface UIScrubberControl : UISliderControl
- (id)initWithFrame:(CGRect)f maxTrackWidth:(float)w showTimes:(BOOL)t showKnob:(BOOL)k;
- (void)setDuration:(double)d;
@end
@interface UISegmentedControl : UIView
- (id)initWithFrame:(CGRect)f withStyle:(int)style withItems:(NSArray *)items;
- (void)setDelegate:(id)d;
- (void)selectSegment:(int)s;
- (int)selectedSegment;
- (void)setMomentaryClick:(BOOL)m;
@end
@interface UIPageIndicator : UIControl
- (void)setPageCount:(int)n;
- (void)setCurrentPage:(int)n;
- (int)currentPage;
@end

/* Text */
@interface UITextField : UIControl
- (void)setText:(id)t;
- (id)text;
- (void)setPlaceholder:(id)p;
- (void)setBorderStyle:(int)s;
- (void)setFont:(GSFontRef)f;
- (void)setPaddingTop:(float)t paddingLeft:(float)l;
- (void)setTextCentersVertically:(BOOL)c;
- (void)setClearButtonStyle:(int)s;
- (void)setSecure:(BOOL)s;
- (void)setLabel:(id)l;
- (BOOL)becomeFirstResponder;
- (BOOL)resignFirstResponder;
- (void)setDelegate:(id)d;
@end
@interface UISearchField : UITextField
@end
@interface UIScroller : UIView
- (void)setContentSize:(CGSize)s;
- (void)setAllowsRubberBanding:(BOOL)a;
- (void)setAllowsFourWayRubberBanding:(BOOL)a;
- (void)setShowScrollerIndicators:(BOOL)s;
- (void)setScrollerIndicatorStyle:(int)s;
- (void)setOffset:(CGPoint)p;
- (void)setDelegate:(id)d;
- (void)scrollPointVisibleAtTopLeft:(CGPoint)p animated:(BOOL)a;
@end
@interface UITextView : UIScroller
- (void)setText:(id)t;
- (void)setTextSize:(float)s;
- (void)setEditable:(BOOL)e;
- (BOOL)becomeFirstResponder;
- (void)setTextColor:(CGColorRef)c;
@end
@interface UIKeyboard : UIView
+ (CGSize)defaultSize;
- (id)initWithDefaultSize;
@end

/* Tables */
@interface UITableColumn : NSObject
- (id)initWithTitle:(id)t identifier:(id)i width:(float)w;
@end
@interface UITable : UIScroller
- (void)addTableColumn:(UITableColumn *)c;
- (void)setDataSource:(id)d;
- (void)setDelegate:(id)d;
- (void)reloadData;
- (int)selectedRow;
- (void)setRowHeight:(float)h;
- (void)setSeparatorStyle:(int)s;
- (void)selectRow:(int)r byExtendingSelection:(BOOL)e withFade:(BOOL)f;
- (id)cellAtRow:(int)r column:(int)c;
@end
@interface UITableCell : UIView
- (void)setShowDisclosure:(BOOL)s;
- (void)setDisclosureStyle:(int)s;
- (void)setSelected:(BOOL)s withFade:(BOOL)f;
- (void)setDisclosureClickable:(BOOL)c;
@end
@interface UIImageAndTextTableCell : UITableCell
- (void)setTitle:(id)t;
- (void)setImage:(UIImage *)i;
@end
@interface UISimpleTableCell : UITableCell
- (void)setTitle:(id)t;
- (void)setIcon:(UIImage *)i;
- (void)setTitleColor:(CGColorRef)c;
@end
@interface UIPreferencesTable : UITable
@end
@interface UIPreferencesTableCell : UIImageAndTextTableCell
- (void)setValue:(id)v;
- (void)setChecked:(BOOL)c;
- (void)setCheckStyle:(int)s;
- (void)setUsesBlueDisclosureCircle:(BOOL)b;
@end
@interface UIPreferencesControlTableCell : UIPreferencesTableCell
- (void)setControl:(UIView *)c;
@end
@interface UIPreferencesTextTableCell : UIPreferencesTableCell
- (void)setPlaceHolderValue:(id)v;
- (id)textField;
@end
@interface UISectionList : UIView
- (id)initWithFrame:(CGRect)f showSectionIndex:(BOOL)s;
- (void)setDataSource:(id)d;
- (UITable *)table;
- (void)reloadData;
- (void)setShouldHideHeaderInShortLists:(BOOL)h;
@end

/* Pickers */
@interface UIPickerView : UIView
+ (CGSize)defaultSize;
- (void)setDelegate:(id)d;
- (void)selectRow:(int)r inColumn:(int)c animated:(BOOL)a;
- (int)selectedRowForColumn:(int)c;
- (void)reloadData;
- (void)setSoundsEnabled:(BOOL)s;
@end
@interface UIDatePicker : UIPickerView
- (void)setDatePickerMode:(int)m;
- (void)setDate:(id)d;
- (id)date;
@end

/* Progress */
@interface UIProgressIndicator : UIView
+ (CGSize)defaultSizeForStyle:(int)s;
- (void)setStyle:(int)s;
- (void)startAnimation;
- (void)stopAnimation;
@end
@interface UIProgressBar : UIView
- (void)setProgress:(double)p;
- (void)setStyle:(int)s;
@end
@interface UIProgressHUD : UIView
- (id)initWithWindow:(UIWindow *)w;
- (void)setText:(id)t;
- (void)show:(BOOL)s;
- (void)done;
@end

/* Transitions and animation */
@interface UITransitionView : UIView
- (BOOL)transition:(int)style toView:(UIView *)v;
- (void)setDelegate:(id)d;
@end
@interface UIAnimation : NSObject
- (id)initWithTarget:(id)t;
- (void)setAnimationCurve:(int)c;
- (void)setDelegate:(id)d;
@end
@interface UIAlphaAnimation : UIAnimation
- (void)setStartAlpha:(float)a;
- (void)setEndAlpha:(float)a;
@end
@interface UIRotationAnimation : UIAnimation
- (void)setStartRotationAngle:(float)a;
- (void)setEndRotationAngle:(float)a;
@end
@interface UIFrameAnimation : UIAnimation
- (void)setStartFrame:(CGRect)f;
- (void)setEndFrame:(CGRect)f;
@end
@interface UIAnimator : NSObject
+ (id)sharedAnimator;
- (void)addAnimation:(UIAnimation *)a withDuration:(double)d start:(BOOL)s;
@end

/* Bars, web, misc */
@interface UIButtonBar : UIView
+ (float)defaultHeight;
- (id)initInView:(UIView *)v withFrame:(CGRect)f withItemList:(NSArray *)items;
- (void)registerButtonGroup:(int)g withButtons:(int *)tags withCount:(int)n;
- (void)showButtonGroup:(int)g withDuration:(double)d;
- (void)showSelectionForButton:(int)tag;
- (void)setBarStyle:(int)s;
- (void)setBadgeValue:(id)v forButton:(int)tag;
- (void)setDelegate:(id)d;
@end
@interface UIWebView : UIView
- (void)loadHTMLString:(id)html baseURL:(id)url;
- (void)setDrawsBackground:(BOOL)d;
- (void)setAutoresizes:(BOOL)a;
@end
@interface UICalloutView : UIControl
- (void)setTitle:(id)t;
- (void)setSubtitle:(id)t;
- (void)setAnchorPoint:(CGPoint)p boundaryRect:(CGRect)r animate:(BOOL)a;
- (void)addTarget:(id)t action:(SEL)a;
@end
@interface UIDateLabel : UITextLabel
- (void)setDate:(id)d;
@end
@interface UIGradientBar : UIView
@end
@interface UIBox : UIView
- (void)setRoundedCorners:(int)c;
- (void)setContentView:(UIView *)v;
@end

#endif
