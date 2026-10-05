# Kitchen Sink, 2007

A homebrew app for iPhone OS 1.0 (1A543a) on the emulated M68 that shows off the interesting controls 1.0's
UIKit has, one screen each. It is built the way Hello and Tilt are (`../hello-2007`, docs/m68/sideload.md):
no SDK, fragile-ABI Objective-C linked against 1.0's own frameworks, and sideloaded into `/Applications`.

The class list comes from 1.0 itself, not from a later SDK. `objc1dump.py` reads the `__OBJC` metadata of 1.0's
UIKit binary, the way class-dump did in 2007: 193 classes with their ivars, instance sizes, selectors and type
encodings. `uikit1ks.h` declares what the app uses, with the argument types 1.0 has. For example,
`-[UIProgressBar setProgress:]` takes a double, and `-[UITextField setTextFont:]` takes an object. The control
event masks come from `-[UIControl mouseDown:/mouseUp:]`: 1 is mouse down, 0x40 is up inside and 0x80 is up
outside.

```
ARMV6_SDK=<iPhoneOS3.1.3.sdk> ./build.sh ROOT [OUT]          # OUT/Kitchen.app; ROOT = the 1.0 rootfs copy
imgtools/sideload1x.py --nand DEV/nand --overlay OVL OUT/Kitchen.app
./tour.py --device DEV --overlay OVL --qemu QEMU --out DIR [--frames]   # screenshots, contact sheet, GIF
python3 objc1dump.py ROOT/System/Library/Frameworks/UIKit.framework/UIKit [CLASS...]
```

The bundle is `Kitchen.app`, because 1.0 labels the home-screen icon with the bundle's directory name, and
"KitchenSink" truncates. `art.py` (Pillow) draws every image the app ships: the icon, row icons, button
backgrounds, the star, tab icons and the map. Nothing of Apple's is copied. The app links against 1.0's
frameworks on the device.

Screenshots, a contact sheet and `tour.gif` (the tour at 2x speed) from `tour.py` on 2026-10-05 are in
`~/Developer/qemu-ios-files/m68/guestdev/kitchen-sink/`.

## Screens and the 1.0 classes they use

The shell is a `UINavigationBar` (with `UINavigationItem`s and back buttons) over a `UITransitionView`. The root
list is a `UITable` with a `UITableColumn` and `UIImageAndTextTableCell`s with disclosures. Picking a row pushes
an item and slides the screen in; Back slides it out. Every screen ends with a `UITextLabel` footer naming its
classes.

| Screen | Classes | Interaction |
|---|---|---|
| Buttons | `UIThreePartButton` (three-slice backgrounds, pressed image), `UIPushButton` (image per state, selected), `UIValueButton` (label, value, disclosure), `UICheckbox` | every button reports to a status label |
| Switches & Sliders | `UISwitchControl` (normal and `setAlternateColors:` orange), `UISliderControl` (value readout; tick marks), `UISegmentedControl` (styles 0, 1, 2), `UIPageIndicator` | live readout of every value |
| Text & Keyboard | `UITextField` (border, placeholder, clear button), `UISearchField`, `UITextView` (editable, WebKit-backed), `UIKeyboard` | type on the keyboard |
| Alerts & Sheets | `UIAlertSheet` as an alert, as an action sheet with a destructive button (`presentSheetInView:`), and with a text field (`addTextFieldWithValue:label:`); `UIProgressHUD` | buttons report their index |
| Progress | `UIProgressIndicator` (styles 0-2, on dark and light), `UIProgressBar` (styles 0 and 1), driven by `NSTimer` | animates |
| Pickers | `UIPickerView` (two columns from `pickerView:titleForRow:inColumn:`), `UIDatePicker` (modes 0-3: time, date, both, countdown), `UISegmentedControl` | the selection is read back live |
| Scroller & Images | `UIScroller` (four-way rubber banding, indicators), `UIImageView`, `UIImage applicationImageNamed:` | drag around a 720x1000 map |
| Preferences Table | `UIPreferencesTable` with grouped `UIPreferencesControlTableCell` (switches), `UIPreferencesTextTableCell` (text, secure), value cells with disclosures, a radio group of checkmarks | tap a radio row |
| Section List | `UISectionList` with its index, `UITable`, `UISimpleTableCell` | the index jumps |
| Transitions | `UITransitionView`, styles 1-9 | Next Transition cycles them |
| Animator | `UIAnimator` with `UIAlphaAnimation`, `UIRotationAnimation`, `UIFrameAnimation` (ease curve) | Fade, Spin, Move |
| Navigation Bars | `UINavigationBar` bar styles 0, 1, 2; `showLeftButton:withStyle:rightButton:withStyle:` (normal, red, blue); a prompt | bar buttons cycle the button styles |
| Button Bar | `UIButtonBar` (1.0's tab bar: item dictionaries, button groups, selection, badge) | tabs switch and clear the badge |
| Web View | `UIWebView` (`loadHTMLString:baseURL:`, viewport, CSS, tables) | scrolls |
| Labels & Callouts | `UITextLabel` (wrapping, embossed shadow), `UIDateLabel` (live clock), `UIGradientBar`, `UIBox` (rounded), `UICalloutView` (anchored, as Maps' pins) | the clock ticks |

Foundation 1.0 is used as well: `NSString`, `NSArray`, `NSDictionary`, `NSNumber`, `NSTimer`, `NSDate`,
`NSNotification` (`tableRowSelected:`) and `performSelector:withObject:afterDelay:`.

## In 1.0 but not shown, and why

- `UICoverFlowLayer`: a LayerKit (`LKLayer`) layer, not a view. The iPod app feeds it album art through an
  image manager and benchmark hooks. Hosting a layer and providing that protocol was more than this pass did.
- `UIScrubberControl`: the iPod/YouTube playback scrubber. It needs a media duration and a scrub delegate.
  The slider screen shows its `UISliderControl` base.
- `UIFontChooser`: Notes' internal font picker. It drives a shared singleton, not a standalone control.
- `UICompletionTable`, `UIFormAssistant`, `UIAutocorrect*`, `UITextLoupe`, `UIKeyboardImpl`: internals of the
  keyboard, MobileSafari's forms and autocorrection. Typing on the Text screen brings some of them up
  indirectly. They have no app-facing API.
- Row deletion in `UITable` (`enableRowDeletion:`, `UIRemoveControl`): exists, but not added to the
  list screens yet.
- `UIZoomAnimation` and pinch zoom in `UIScroller` and `UIWebView`: zooming is a two-finger gesture. The emulator's
  host input is a single touch, so there's nothing to drive it with.
- `SBSuspensionWindow`, `UISuspendInfo`, `UIViewHeartbeat`, `UIAnimator` internals, `WebLayer`, `UITiledView`,
  `UIThreadSafeNode`, `UITouchDiagnosticsLayer`: plumbing, with nothing to show by themselves.

## Notes

- Subclassing needs real instance sizes (fragile ABI). Only `UIApplication` is subclassed (12 bytes, from
  `uikit1.h`). Every other class is used through messages, so its ivars need not be declared.
- `UIThreePartButton` autosizes to its title unless told `setAutosizesToFit:NO`. `UIValueButton` shows its
  left-hand text with `setLabel:`. `UISwitchControl` and `UISliderControl` are registered for all events, and
  the handler reads `value`.
- `tour.py` boots a copy of the overlay. The tour ends by stopping QEMU, which leaves the guest unclean, and an
  overlay booted that way hangs at the logo the next time.
