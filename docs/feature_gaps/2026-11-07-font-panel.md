# The font panel's choice lost on closing it (Application Settings > Appearance)

*7 November 2026, on top of `e723e56`. Apple M2 Ultra, macOS 27.2, Qt 6.12.0 from Homebrew. CI and the releases use
Qt 6.10.3, on which none of this has run (see "What could not be verified").*

The user: "Looks like the font selector doesn't give the settings to Qucs settings window when I close it. For example,
if I set the font size to 14 and close the font selector window, the font size in the qucs gui is still at 10."

## The cause

`QFontDialog::getFont` takes closing macOS's font panel for Cancel, and on macOS 27 closing is the only way out of it.

Qt's Cocoa helper (`qcocoafontdialoghelper.mm`) makes OK and Cancel buttons and puts them in the panel's content view
with the panel's old contents inside. On macOS 27 the panel's contents come from a split view controller, and the
buttons never appear: Qt's wrapper is made, and is nowhere in the window. Qt does not know that, and its
`windowShouldClose:` ends the panel with `NSModalResponseCancel`, so `getFont` says `ok == false` and returns the font
it was given. The panel's size box said 14 and the selected font was Menlo 14 all the while.

All three buttons (Schematic, Application, Text document font) did the same. Nothing was wrong in how the settings are
saved: they saved what the button had.

Qt's helper is the same in 6.10.3 and 6.12.0 but for one comment line, by the sources. That is a reading, not a run.

## The fix

`chooseFont()` in `qucssettingsdialog.cpp`, for all three buttons:

- **On macOS the panel opens with `QFontDialog::NoButtons`,** Qt's form for a live dialog: its contents are left as
  they are, and it tells each change by `currentFontChanged`. What it told last is taken when it closes.
- **A font is taken only from the second telling on.** The panel tells once more as it closes, whatever was done, and
  that alone is no choice. Taking it would have been worse than the bug:

  | the setting | opened and closed, nothing chosen, the panel tells |
  |---|---|
  | `.AppleSystemUIFont,13` | Academy Engraved LET 13 |
  | a font not on the machine, 9 pt | Academy Engraved LET 9 |
  | Menlo 10.5 | Menlo 11 |
  | Menlo 10 | Menlo 10, Regular |

- **The system's font.** Schematic and Application fonts are `.AppleSystemUIFont` by default, which is not in the
  panel's list: Qt asks for it by name, gets nothing, and the panel shows the first font of the list, Academy Engraved
  LET. With the choice now kept, a new size there would have made the application's font Academy Engraved. It now opens
  on Helvetica Neue, and a new size or style on that keeps the system's font. A family picked from the list is taken.
- **The plain face's style name is dropped** ("Regular", "Plain"). With the name kept a later bold stays the Regular
  face (`QFontInfo::styleName`, tried), and the button reads `Menlo,14`, as the setting did.
- **Cancel.** The panel has no Cancel, and the settings window's own is what undoes it: nothing is kept until its Apply
  or OK. Esc in the panel now closes it as the red button does, and keeps the choice. Before, it dropped it, as
  closing did.
- Qt's own dialog (other platforms, or macOS while the tools hold the native dialogs back, `AA_DontUseNativeDialogs`)
  keeps its OK and Cancel, through `getFont` as before.

## What was seen on the real panel

The real Qucs-S, its settings window and the real `NSFontPanel`, on the display, in One Dark as the user has it and an
own settings folder. The shell here has neither Accessibility nor Screen Recording (`AXIsProcessTrusted` and
`CGPreflightScreenCaptureAccess` both 0), so a library put in with `DYLD_INSERT_LIBRARIES`
([paneldriver.mm](2026-11-07-font-panel/paneldriver.mm), [drive.sh](2026-11-07-font-panel/drive.sh)) posts AppKit events
to `NSApp`: a click in the size box, Delete, the digits, Return, a click on the red button. Those go through the panel's
own event handling; they are not a person's hand, and no pointer moved.

Before, on `e723e56`, Text document font, 14 typed, red button:

```
settings window open; button 2 reads 'Menlo,10'
panel[typed] selected=Menlo-Regular 14 sizeBox='14' Qt's buttons=made in the window=no
after the panel: panel visible=0, button 2 reads 'Menlo,10'
```

After:

| done in the panel | closed by | then | the button | saved |
|---|---|---|---|---|
| Text document font: 14 typed | red button | Apply | `Menlo,14` | `Menlo,14` |
| Application font: 14 typed | red button | Apply | `.AppleSystemUIFont,14` | `.AppleSystemUIFont,14` |
| Schematic font: 14 typed | red button | Apply | `.AppleSystemUIFont,14` | `.AppleSystemUIFont,14` |
| Text: 14 typed | red button | settings' **Cancel** | `Menlo,14` | `Menlo,10` |
| Text: 14 typed | Esc | OK | `Menlo,14` | `Menlo,14` |
| Text: the size box's list, a click in it | red button | Apply | `Menlo,24` | `Menlo,24` |
| Text: Bold clicked | red button | Apply | `Menlo,10,Bold` | Menlo 10 Bold |
| Text: a family clicked | red button | Apply | `Luminari,10` | `Luminari,10` |
| Application font: nothing | red button | Apply | `.AppleSystemUIFont,13` | `.AppleSystemUIFont,13` |
| Text: nothing | red button | Apply | `Menlo,10` | `Menlo,10` |

- Started again on the settings of the first row, the button reads `Menlo,14` and the panel opens on Menlo 14.
- Return in the panel, with the size already taken, leaves it open.
- As the label says ("set after reload"), a font chosen applies at the next start. That is as it was, and not changed.

## Tests

`tests/test_font_settings.cpp`: each button's choice reaches it and Apply saves it, the settings' Cancel drops it, closing
with nothing chosen changes nothing (with the panel's closing word played), the system's font through a new size and to
another family, Qt's own dialog keeping its buttons. On macOS it plays what the panel was seen to do: it cannot run the
panel, which needs a display, and offscreen `QFontDialog` is Qt's own. Elsewhere it uses that dialog's OK and Cancel.

| test | Release | ASan/UBSan (Debug; no leak check) |
|---|---|---|
| `test_font_settings` | 9 passed | 9 passed |
| `test_grid_setting` | 7 passed | 7 passed |
| `test_app_theme` | 19 passed, `gallery` skipped (no `QUCS_TEST_GRAB`) | the same |
| `test_settings_io` | 15 passed | 15 passed |

The whole suite was not run. `test_gui_monkey`, which opens these dialogs, passed in Release with seeds 7 and 8, 800 steps
each. The four lints in `scripts/ci/` are green.

The test was checked by breaking the fix: `getFont` back on macOS fails 5 of its 9 (the three buttons, Cancel, the
system's font); every telling taken fails `closingWithNothingChosenChangesNothing`; the system's font not kept fails
`theSystemFontKeepsItselfThroughANewSize`.

## What could not be verified

- **The size box the user saw empty: not reproduced, cause not found.** In the user's picture the box beside Menlo is
  blank. In every drive here it held its value, on opening and after, in the system theme and in One Dark, with Qt's
  buttons and without: `stringValue` 10, 13 or 14, the label not hidden, and drawn in the panel's own picture
  (`cacheDisplayInRect`). That picture is the view's drawing, not what the window server composites, and Screen
  Recording would be needed to see that. It did not stop the choice: the value is there and a typed or listed size
  takes. It may be a second fault, or go with the wrapper this fix no longer makes; only the user's eye can say.
- **Qt 6.10.3,** which CI and the releases use: not run. The helper's source is the same.
- **Older macOS:** not run. Where Qt's OK and Cancel do show, `NoButtons` removes them there too, and the panel closes
  and keeps the choice as here, the settings' Cancel still undoing it. That was chosen over asking the system's version,
  since where the panel changed is not known here.
- **A person's hand.** No real pointer or keyboard was used.
- **Fractions.** Qt's helper rounds what the panel says (`qRound`), so a 12.5 typed is 13.

## For the next Qt

"From the second telling on" rests on `windowShouldClose:` telling once as it closes, which is not documented. If Qt
stops, a single change would be lost again, as it was, with no harm to the setting. The test cannot see that; a drive of
the real panel can, and is worth repeating at a Qt or macOS upgrade.
