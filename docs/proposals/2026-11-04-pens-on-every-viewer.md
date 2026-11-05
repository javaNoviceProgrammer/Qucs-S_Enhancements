# Proposal: pens on every viewer, with a Pens toolbar, and Claude reading what they draw

*4 November 2026, after Qucs-S 27.0.6 (main at d8b5942). A written plan only; nothing here is built. The
user parked the work: "actually, let's park this and just write up the implement plan. We'll get to this at
some point in the future."*

*Every `file:line` is as of d8b5942, and a bare `:n` is in the file named just before it. Paths are under
`qucs-s-26.1.1/qucs/` unless they say otherwise. The facts were read from the code. No probe was run, so whatever reading cannot settle is under [Open questions
and risks](#open-questions-and-risks), not asserted. Remarks about Qt's own source are from memory and say so:
it is not on this machine.*

## The goal

The user's request of 4 November, in their words:

> "I love the pen marker panel that we added to the gds layout viewer and pptx viewer so much that I think we
> should create a dedicated toolbar for it and make it work in any viewer (gds viewer, pptx viewer, markdown
> viewer, pdf viewer, multiphysics viewer, multiphysics 3D viewer, RF 2.5D viewer, ...) so that I can do
> markings in any viewer and claude can see them."

So: mark up whatever a tab shows, with one toolbar, and have Claude read those marks without a picture, as it
already does on layouts and slides.

Read [the pen-markers note](../feature_gaps/2026-11-02-pen-markers.md) first. It says what the pens do, how a
stroke is read, how they are tested and what was left out. This plan continues it.

## In short

- **One pen, one toolbar.** A new `Pens` toolbar in the main window holds the tools, colours, breadth, undo and
  clearing. It acts on the document in front. The layout's and the slide's own pen buttons stay, in step with
  it both ways.
- **One small interface per document.** `QucsDoc::penSurface()` returns a `markup::PenSurface`: the drawing, the
  part shown, pixel to measure and back, and the reading. The toolbar, the prompts and every Claude tool go
  through it and stop naming `LayoutDoc` and `PptxDoc`.
- **One shared input.** An event filter takes the left drag while the pen is up and paints after the view has
  painted. This plan recommends it over the transparent child widget first proposed; the reasons are in
  [The shared input](#3-the-shared-input-a-filter-rather-than-a-child-widget).
- **Layouts and slides keep their own mouse code.** They are wrapped, not rewritten, so their tests stand.
- **Each viewer says only what it knows.** A PDF has words but no figures, a picture has nothing but where the
  stroke is, a model has its domains and plotted values. Each reading is honest about that.
- **Eleven phases, each mergeable alone.** The work can stop after any of them.

## Terms

- **Part:** what a stroke is drawn on inside a document: a slide, a cell, a page. A stroke shows only with its
  part. `Stroke::on` holds it as a string (`markup.h:58`).
- **Measures:** the units a stroke's points are held in, so that it pans and zooms with what is under it: a
  slide's points, a layout's µm.
- **Reading:** one stroke as JSON: how it was drawn (a ring, a line, an arrow...) and what it marks. A *sentence*
  is the same for a prompt (`markup.cpp:733`).
- **The pen is up:** taken up, so that a left drag draws. **Down:** the viewer has its mouse back.

## Decisions, and whose they are

**The user's own**

1. **The layout's and the slide's pen buttons stay.** The user's words, answering "the new toolbar replaces the
   pen buttons on the layout and slide viewers": "No, let's also keep them". So the toolbar goes beside them.
   They must never disagree: pressing either takes the pen up or down and the other shows it at once, and the
   pen in hand, its colour and breadth, and the undo and clear states stay in step both ways. `penButton()` and
   the tests using it keep working unchanged.

   Other viewers get the toolbar only, unless a reason turns up. The user asked to keep these two, not to add
   more. (That last sentence is how the coordinating session read their answer.)

**Recommended to the user, seen and not objected to, not explicitly confirmed.** Confirm these when the work is
picked up:

2. **One pen.** Taken up, it stays up as tabs change, until Escape puts it down.
3. **While it is up a left drag draws.** The middle button and the wheel still go to the view, so it pans and
   zooms as it does today.
4. **Drawings stay with the tab.** They are not saved, as today.
5. **In 3D a stroke is pinned to the surface under it** and turns with the model.

**This plan's own, put to nobody yet.** Each can be changed without touching the rest:

6. A shared event filter rather than a child widget (the reasons are in the design).
7. A new `pen` tool for taking the pen up and clearing on any kind; `read_drawings` stays read-only.
8. While the pen is up, Ctrl+Z is the pen's on every kind, and nothing falls through to the document's own Undo.
9. A stroke on flowing text (Markdown, a text file) is pinned whole to one character and keeps its shape.
10. Multiphysics parts follow what is shown: geometry, mesh, each plot group, each process step.
11. Layout and slide mouse code stays. Replacing it with the shared input is left out.
12. Exports stay as they are today: `export_image` and File > Export carry no strokes (a slide's already carry
    none, and the pen-markers note says why), while print and `screenshot` do. See open question 9.

## Where the brief was wrong or short

The first brief for this work came from reading headers and grep hits. Checked against the code:

| The brief said | The code says |
|---|---|
| A transparent child over the viewer's viewport | **It would not survive.** `gpu::setGpuViewport` deletes the old viewport (`gpurendering.cpp:177`), and its children with it. Its comment already says filters are the owner's to install again (`gpurendering.h:67`). `send_input` also sends straight to a schematic's canvas, a layout's view and slides, and to `root->childAt(at)` otherwise (`qucscontrol_input.cpp:491`), and the existing pen tests send to the viewport (`tests/test_pptx_doc.cpp:2215`), so on those paths a child would never see them. See [the shared input](#3-the-shared-input-a-filter-rather-than-a-child-widget). |
| "Unlike the viewers, the window's Undo is live" on a schematic | **It is live on seven kinds.** Only PDF, picture, layout and slides turn it off (`pdfdoc.cpp:2056`, `imagedoc.cpp:1676`, `layoutdoc.cpp:2713`, `pptxdoc.cpp:1644`). RF 2.5D (`rf25doc.cpp:899`), 2D and 3D models (`multiphysicsdoc.cpp:637`, `multiphysics3ddoc.cpp:486`), sheets (`sheetdoc.cpp:486`), archives (`zipdoc.cpp:387`), text (`textdoc.cpp:287`) and schematics (`schematic.cpp:497`) leave it on while they have a step to undo. On those, the window's Undo takes Ctrl+Z before the view sees it: on an RF 2.5D model with the pen up, it would undo a model edit, not the stroke. The pen-markers note's "Ctrl+Z reaches both views as it is" holds only for the read-only kinds. |
| `PainterSurface` and `GpuSurface` both apply to the PDF | **`GpuSurface` has fills and images only** (`pdfdoc.cpp:272-283`, `rhicanvas.h:157`), so there are no lines. Strokes reach that path as an image. |
| PDF mapping by `pageAt`, `pageRect`, `pixelsPerPoint` | **No public way back from points to pixels.** `toViewport(int, QPointF)` and `pagePoints(int)` are private (`pdfdoc.h:207-209`), and `pageAt` gives -1 off a page (`pdfdoc.cpp:553-563`). |
| A `grab()` sees an overlay but a CPU `picture()` does not | **True of some only.** `GraphicsView::picture()` paints directly (`multiphysicsview.cpp:524`), and so does `View3D::picture()` (`view3d.cpp:332`). `LayoutView::picture()` is itself a grab (`layoutdoc.cpp:973-975`). |
| Schematic mapping by `viewportToContents` | **The model mapping is another pair:** `modelToViewport` and `viewportToModel` (`schematic.h:281`, `:287`), on whole numbers, and `viewportToModelF` (`schematic.h:289`) takes a whole pixel. A stroke needs a double-precision way back, which is new. |
| A face id per triangle in 3D | **Only on boundary faces.** Slices and isosurfaces have id -1 (`multiphysics3ddoc.cpp:1546`, `:1581`, `:1681`). |
| Middle button and wheel reach the view, in every viewer | **On the 3D view that leaves no way to turn the model.** Its left drag turns it and its middle button pans (`view3d.cpp:399-408`). See open question 7. |
| Markdown: pin a stroke to the text block | **Text cursors cannot hold it.** `renderNow()` rebuilds the whole `QTextDocument` a moment after each edit (`markdowndoc.cpp:1063-1091`), so positions and cursors die. See [Markdown](#markdown-rendering). |
| `read_drawings` entries at 678, 1023, 1214, 1377, 3872, 4178 of `qucscontrol.cpp` | **Three more:** `kReadOnly` (`qucscontrol.cpp:1114`), `whileADialogWaits` (`:5192`) and the dispatch (`:5340`). |
| The tests keep working | **Three must change.** `tests/test_toolbar_lock.cpp:113` and `:206` count 7 toolbars. `tests/test_pptx_doc.cpp:2791` expects the refusal "not a layout or a presentation", on a text file, which the [text phase](#text-and-code) ends. |

Also, `send_input`'s description is 2,003 characters against a test's cap of 2,048 (`tests/test_qucs_control.cpp:289-291`,
`kDescriptionCap`), and `show_layout`'s is 2,038. Neither has room to list new kinds, so they must be rewritten, not
extended.

## What is there today

- **`markup.{h,cpp}`** has `Pen`, `Stroke`, `Drawing` with undo, `paint()`, `begin()`, the gesture reading and
  `readPage()`, and `PenButton`. The pen in hand (tool, colour, breadth) is already global: `currentPen()` and
  `setCurrentPen()` keep it in the settings, and `Pens::changed` tells everyone (`markup.h:183-194`). **Whether the
  pen is up is not global:** each view holds it.
- **The layout.** `LayoutView` has `Mode::Draw` beside Select, Ruler, PickPoint and PickBox (`layoutdoc.h:134`). It
  has its own mouse code (`layoutdoc.cpp:1375-1391`, `:1414-1426`, `:1479-1490`), paints in `paintFrame`
  (`layoutdoc.cpp:1135-1139`), which the CPU and the GPU's canvas share, and reads in `readDrawing`
  (`layoutdoc.cpp:824`). `LayoutDoc` joins button and mode at `layoutdoc.cpp:1939-1955`.
- **The slides.** `SlideView` holds `a_pen` (`pptxdoc.cpp:224`), its own mouse code (`pptxdoc.cpp:559-630`) and paint
  (`pptxdoc.cpp:485-494`). `PptxDoc::readDrawing` reads (`pptxdoc.cpp:1435`), and the button is joined at
  `pptxdoc.cpp:1085-1099`.
- **Escape and Ctrl+Z.** Each view accepts Escape on `ShortcutOverride` (`layoutdoc.cpp:1327-1337`, `pptxdoc.cpp:643-655`),
  because the window has its own Escape (`qucs_init.cpp:612-615`), which ends a schematic's mode (`qucs_actions.cpp:519`).
- **Claude.** `QucsControl::readDrawings` and `insertDrawings` (`qucscontrol_pptx.cpp:408`, `:418`),
  `QucsApp::drawingsOf` (`qucs.cpp:558`), `inEveryTurn` (`qucscontrol.cpp:3867-3875`), `send_input`'s `layout` and `slide`
  targets (`qucscontrol_input.cpp:248-309`). The first three, and that part of `send_input`, name the two classes outright.
- **The prompt's telling** is already kind-agnostic (`claudecodepanel.cpp:2284-2292`): it asks by file and known
  revision, so it needs no change.

## The design

### 1. The pen is up or down once: `Pens`

`Pens` (`markup.h:187`) gains the one state. It is not kept in the settings, so a new session starts with the pen down.

```cpp
class Pens : public QObject
{
    Q_OBJECT
public:
    static Pens* instance();
    bool up() const { return a_up; }
    void setUp(bool up);              // upChanged only on a change
signals:
    void changed();                   // as now: the pen in hand
    void upChanged(bool up);
private:
    bool a_up = false;
};
```

`QucsApp`'s destructor puts it down, so a test that makes a window per function (`test_pptx_doc`) starts clean even
after a failure.

`PenButton` (`markup.h:203`) gains an explicit bond:

```cpp
/// Shows Pens::up(), and its press or a pick from its menu sets it. A button
/// never bound keeps its own checked state.
void followPens();
```

It must be explicit. `thePenIsPickedFromItsMenuAndKept` (`tests/test_pptx_doc.cpp:2126`) builds two free buttons, picks
from them and leaves one checked. If every button followed `Pens`, that would leave the pen up, and the next function's
`penButton()->click()` (`tests/test_pptx_doc.cpp:2231`) would put it *down*, failing. Free buttons stay as they are.

The joins in `LayoutDoc` (`layoutdoc.cpp:1939-1955`) and `PptxDoc` (`pptxdoc.cpp:1085-1092`) become:

- `a_penButton->followPens()`.
- `Pens::upChanged` sets the view: `up` gives `setMode(Draw)`; down gives `setMode(Select)` only if the mode is Draw, so
  a Ruler just pressed is left alone. The focus goes to the view only if this document is in front.
- `modeChanged` keeps its blocked `setChecked` and adds: a mode other than Draw, while `Pens::up()`, puts the pen down.
  That is how Escape and the Ruler button reach the toolbar and every other tab.

`LayoutView::setMode` already lets a stroke being drawn go (`layoutdoc.cpp:573-597`).

### 2. One surface per document: `markup::PenSurface`

It is not called `Surface` because `PageView::Surface` already means something else in `pdfdoc.cpp:208`.

```cpp
namespace qucs_s::markup {

class PenSurface : public QObject
{
    Q_OBJECT
public:
    /// \a drawing: a view's own (LayoutView's, SlideView's); null: one kept here.
    explicit PenSurface(QObject* parent, Drawing* drawing = nullptr);

    // What a document says of itself.
    virtual QString partWord() const = 0;                     // "Page", "Cell", "Slide": "Clear the Drawings on This %1"
    virtual QString partShown() const = 0;                    // in front; empty: nothing can be drawn on now
    virtual QString partSaid(const QString& part) const = 0;  // "page 3", "cell TOP", "the plot Temperature"
    virtual QString measures() const = 0;                     // read_drawings': "points from the page's top left"
    virtual QString unit() const = 0;                         // a sentence's: "points", "µm"
    virtual QJsonArray read(bool points) const = 0;           // each reading naming its part
    virtual QString partOf(const QJsonObject& reading) const = 0;   // send_input's 'drawn'

    // Where it is drawn, in the view's pixels.
    virtual QList<QWidget*> views() const = 0;                // what the shared input watches
    virtual QWidget* viewOf(const QString& part) const = 0;
    virtual QString partAt(QWidget* view, QPointF pixel) const = 0;   // empty: a press there draws nothing
    virtual QStringList partsShown(QWidget* view) const = 0;  // in sight now in that view (pages: several)
    virtual QPointF toMeasure(const QString& part, QPointF pixel) const = 0;
    virtual QPointF toPixel(const QString& part, QPointF measure) const = 0;
    virtual double pixelsPerUnit(const QString& part) const = 0;
    virtual QRectF clipOf(const QString& part) const { return {}; }   // drawn no further; empty: no limit
    virtual bool showPart(const QString& part, const QRectF& measures) = 0;   // in sight, the zoom kept (send_input)

    // Those that do not hold still under the pen: 3D, flowing text.
    virtual void pin(Stroke& stroke) const {}                 // fills stroke.pins as it ends
    virtual QPolygonF shownPoints(const Stroke& stroke) const;   // pixels now; default toPixel() of each; empty: not shown

    // A view with its own mouse code and keys (layout, slides): no shared input for it.
    virtual bool drawsItself() const { return false; }

    // Shared; a wrapper of a view's own drawing overrides the three that change it.
    const Drawing& drawing() const;
    virtual void add(const Stroke& stroke);
    virtual bool undo();
    virtual bool clear(const QString& part = QString());      // empty: all
    void paintOn(QPainter& p, QWidget* view) const;           // the filter, a picture(), a print
    QString sentences(int most = 30) const;                   // a line a reading, for a prompt
signals:
    void changed();                                           // the drawing, or the part shown
};

} // namespace qucs_s::markup
```

`QucsDoc` gains one virtual beside `becomeCurrent` (`qucsdoc.h:44`). Null means no pen.

```cpp
virtual qucs_s::markup::PenSurface* penSurface() { return nullptr; }
```

**`Stroke`** gains one field. It is empty on every flat viewer, so the 2D ones carry nothing.

```cpp
/// What a surface that does not hold still keeps beside its points: a 3D view's
/// x, y, z and face of each point; a flowing text's character and where it was.
QList<double> pins;
```

**The rest of `markup`:**

- `Target::kind` is already a free string (`markup.h:151`) and `refOf` uses it as a key (`markup.cpp:359`). So "line",
  "heading", "domain" and "port" read at once, and `said()`'s default branch words them (`markup.cpp:487-489`).
- `alongText` assumes y down (`markup.cpp:429-450`). Only y-down surfaces have words, so it holds. A model's y is up, but
  its targets have no words.
- `Drawing::eraseAt` works in measures (`markup.cpp:208`). 3D and pinned text need it in pixels: add
  `eraseWhere(part, std::function<bool(const Stroke&)> hit)`, the shared input testing `shownPoints()`.
- `sentencesOf(readings, bool slides, most)` (`markup.cpp:755`) stays for its four test callers
  (`tests/test_layout_doc.cpp:1310`, `tests/test_pptx_doc.cpp:2513`, `:2525`, `:2537`). Add an overload that takes a
  function saying where each reading is, and the unit. `PenSurface::sentences()` uses it.
- `sentenceOf`'s `fields` table (`markup.cpp:743`) gains `value`, for a plotted value at a stroke's tip.

### 3. The shared input: a filter rather than a child widget

The first brief proposed a transparent child over each viewport, and an overlay is what the user sees. Reading the code
speaks against building it as a child widget:

1. **It would be deleted.** `setGpuViewport` deletes the viewport with its children (`gpurendering.cpp:177`).
2. **It would miss events on some paths.** `send_input` sends to a schematic's canvas, a layout's view and slides themselves,
   and to `root->childAt(at)` otherwise (`qucscontrol_input.cpp:491`), so a child is found on the other kinds only if it does
   not let mouse events through. Tests written as the pen tests are (`tests/test_pptx_doc.cpp:2215`) send to the viewport.
3. **It would be moved.** `QWidget::scroll()` moves children, per Qt's documentation as remembered. `Q3ScrollView` calls
   `clipper()->scroll` (`qt3_compat/q3scrollview.cpp:2130`), and Qt's text and item views do the same, also from memory.
4. **It would have to be stacked** above the GPU canvases, which are children of `LayoutView` and `GraphicsView`.

The tree already has the other pattern: `QuoteBars` is a filter that sends the paint on, then paints over it
(`markdowndoc.cpp:513-527`), and its bars show in a `grab()`. So:

```cpp
class PenInput : public QObject
{
public:
    /// \a view's mouse taken while the pen is up, and (unless it paints the strokes
    /// itself) its paint followed by the strokes shown there.
    PenInput(PenSurface* surface, QWidget* view, bool paints = true);
    /// A viewport made again (setGpuViewport): the filter on the new one.
    void reinstall(QWidget* view);
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
};
```

| Event | While the pen is up | While it is down |
|---|---|---|
| Left press or double click | Begins a stroke or erases, if `partAt` names a part; taken either way. A double click is two presses, as on a slide (`pptxdoc.cpp:634`). | Passed on |
| Move with a stroke going | Takes a point at `LeastStep` (`markup.h:116`), updates its dirty rectangle; taken | Passed on |
| Move with no button | Passed on (the status line's readouts), then the cross cursor put back, since views set cursors in moves (`pdfdoc.cpp:1213`) | Passed on |
| Left release | Ends the stroke, `pin()`, `add()`; taken | Passed on |
| Middle and right buttons, wheel, context menu, gestures | Passed on: decision 3. Another button during a stroke changes nothing, as on a layout (`layoutdoc.cpp:1372`). | Passed on |
| Paint | With strokes in sight: sent on under an inside flag, then `paintOn()` in a new painter, clipped to the event's rectangle; taken. Without any: passed on untouched. Never on a `QRhiWidget`, which does not paint with `QPainter` (`rhicanvas.cpp:715`). | Same |

The cursor is saved when the pen goes up and put back when it goes down.

**Where the paint filter is not used.** Each of these has one painting path that a filter cannot reach, or a line there is
simpler:

- `LayoutView` and `GraphicsView` paint through `paintFrame`, shared by the CPU, the GPU's canvas and (for the graph
  view) `picture()`. Their `paintEvent` returns early under the GPU (`layoutdoc.cpp:505-510`, `multiphysicsview.cpp:540-543`). One
  line in `paintFrame` covers all of them, as the layout already does.
- `View3D`: one line in `paintOverlay` (`view3d.cpp:282`) covers `paintEvent` and `picture()` (`view3d.cpp:279`, `:340`).
- The two GPU viewports, PDF and Markdown, draw a `Canvas2D` of fills and images. Strokes are drawn into a transparent
  image of the viewport's size at its ratio, kept until the drawing or the view changes, and `drawImage`'d last
  (`pdfdoc.cpp:294-297`, `markdowndoc.cpp:996-998`).

**Filter order.** Filters run last installed first (`rhicanvas.h:209-211`), so `PenInput` is installed after a view's own.
Owners already reinstall theirs after a GPU swap (`markdowndoc.cpp:1002-1004`); `PageView::setGpu` (`pdfdoc.cpp:289`) and
`MarkdownDoc::setPreviewGpu` call `reinstall` there too.

### 4. The keys: `PenKeys`

An application-wide event filter, installed only while the pen is up. It claims Escape and `QKeySequence::Undo` on
`ShortcutOverride`, then handles them on `KeyPress`:

- **Escape:** a stroke being drawn is dropped; else the pen goes down.
- **Ctrl+Z:** `surface->undo()` on the document in front, whether or not there is anything to undo (decision 8).

It claims neither when a dialog or popup is up, when the focus is outside the document in front (the Claude Code panel's
field, a dock), or when the focus is a field inside it (a find bar). There the key is that field's, or the window's. It
leaves surfaces with `drawsItself()` alone, so the layout's "one thing at a time" order on Escape (`layoutdoc.cpp:1572-1592`)
stands.

One addition for those: `LayoutView::event` (`layoutdoc.cpp:1327`) also accepts Undo in `Mode::Draw`. On a read-only layout
that changes nothing, as the pen-markers note found. RF 2.5D draws through the same view with a live Undo, and there it is what
keeps a stroke's Ctrl+Z from the model.

`send_input` asks the field first through `ShortcutOverride` (`qucscontrol_input.cpp:592-597`), so an application filter
sees its keys too. Whether a real Cmd+Z takes this path on macOS's menu bar is open question 4.

### 5. The toolbar

`pensToolbar`, a `markup::PensToolBar : QToolBar` in `markup.{h,cpp}`:

- **Where:** made in `QucsApp::initToolBar` (`qucs_init.cpp:1438`) after `hierarchyToolbar`, so on the second row
  (`addToolBarBreak`, `:1486`) and before Python's (`:1511`). Object name beside the others (`:1518`). Listed in `toolbars()`
  (`:1530`), so the Toolbars menu (`:1520`) and the lock (`:1534`) apply at once.
- **Holds, in order:** pen, highlighter, eraser (checked: the pen up with that tool; pressing the one in hand puts it down);
  colour, a drop-down of the eight and Other Colour…; breadth, a drop-down of fine, medium and bold; undo; clear this part;
  clear all. That is eight buttons. Each colour as a button of its own would make seventeen, on a row Python's toolbar shares.
- **Shares `PenButton`'s logic.** `fillMenu` and `updateMenu` (`markup.cpp:837-927`) become a small `PenActions` that both
  use. The pen in hand already follows `Pens::changed`.
- **Follows the document in front.** `QucsApp::updatePensToolbar()` is called from `slotChangeView` beside
  `updatePythonToolbar()` (`qucs.cpp:3841`), before its early return (`qucs.cpp:3853`). It joins the surface's `changed` to the
  undo and clear states, and lets go of the last one's. Split panes come for free: `setActivePane` calls `slotChangeView`
  (`qucs_panes.cpp:206-217`).
- **Greyed** where `penSurface()` is null or `partShown()` is empty. A pen taken up stays up there, shown checked and greyed.
- **Actions have object names**, so `list_actions` and `trigger_action` reach them (`qucscontrol.cpp:12090`), and `get_ui`
  lists the bar without a change (`qucscontrol_ui.cpp:203`).
- **No new shortcut,** so as not to add an ambiguous one.

### What each changes in existing code

| File | Change |
|---|---|
| `markup.{h,cpp}` | `Pens::up`, `PenButton::followPens`, `PenSurface`, `PenInput`, `PenKeys`, `PensToolBar`, `PenActions`, `Stroke::pins`, `Drawing::eraseWhere`, the `sentencesOf` overload |
| `qucsdoc.h` | `penSurface()` |
| `qucs.h`, `qucs_init.cpp`, `qucs.cpp` | The toolbar, `updatePensToolbar`, `drawingsOf` through surfaces, the pen put down at the end |
| `layoutdoc.*`, `pptxdoc.*` | A wrapper surface each; the joins above. Their pen code stays |
| `pdfdoc.*`, `imagedoc.*`, `rf25doc.*`, `multiphysicsview.*`, `multiphysicsdoc.*`, `view3d.*`, `multiphysics3ddoc.*`, `markdowndoc.*`, `schematic.*`, `graphicsexport.*`, `textdoc.*`, `sheetdoc.*`, `zipdoc.*`, `xmldoc.*` | A surface each, phase by phase; see each viewer |
| `qucscontrol*.cpp`, `qucscontrol.h` | [Claude's side](#claudes-side) |
| `tests/test_toolbar_lock.cpp` | 7 toolbars become 8 (`:113`, `:206`) |

## Each viewer

Each lists its part and measures, what its reading says and cannot say, what stands in its way, which tool carries the
readings, its `send_input` target, and its test suite. Every reading also has what all have: stroke number, gesture, tool,
colour, box, and with `points` its simplified points (`markup.cpp:641`).

Which window Undo is live was checked per document's `becomeCurrent` (table in
[where the brief was wrong](#where-the-brief-was-wrong-or-short)).

### GDS layout (`LayoutDoc`) and slides (`PptxDoc`): wrapped

- **Change:** a wrapper surface over `view()->drawing()`, `drawingPart()`, `readDrawing()`, `drawsItself() = true`; the joins
  above. The own buttons stay (decision 1).
- **Tools:** `show_layout`, `show_pptx`, `read_pptx`'s tab, `read_drawings`, as now; `send_input`'s `layout` and `slide`.
- **Suites:** `test_layout_doc`, `test_pptx_doc`, unchanged but for new functions.

### PDF (`PdfDoc`, `PageView`)

- **Part and measures:** the page's number from 1, as a slide's. Points from its top left, by `pageAt` (`pdfdoc.h:131`) and
  `toViewport` (private today, `pdfdoc.h:209`; make it public), scale `pixelsPerPoint()` (`pdfdoc.cpp:331`). `clipOf` is
  `pageRect` (`pdfdoc.h:128`), as a slide draws no further than its edge. Several pages can be in sight.
- **Reading says:** targets per page from `QPdfDocument`: `getAllText(page).bounds()` gives runs (`pdfdoc.cpp:689-691`), and
  `getSelectionAtIndex` a word's box. Lines grouped into paragraphs by gaps (open question 6). The usual gestures: words a ring
  holds, words underlined, what an arrow points at.
- **Cannot say:** what is in a figure, plot or table's structure, since nothing here asks pdfium for those and as remembered
  `QPdfDocument` has no such API. A scanned page has no words at all. Both read as gesture and box; the answer says "no text
  under it", and `screenshot`'s `page` shows it.
- **In the way:**
  - *Mouse:* left selects, follows a link, or pans beside a page (`pdfdoc.cpp:1154-1185`); a double click selects a word
    (`:1235`). All are taken by the filter only while up.
  - *Cost:* every text call rereads the page (3 ms at 3,000 runs, 12 ms at 12,000). Make a page's targets once, keep them as
    `a_pageTexts` keeps its runs (`pdfdoc.cpp:684`), and drop them on `reload` (`:1736`).
  - *Print and pictures:* `print` draws a page image (`pdfdoc.cpp:2085-2114`); strokes go after it, scaled. `PdfDoc::render`
    (`:1951`) gains strokes and serves `screenshot`'s `page` and `region` (`qucscontrol.cpp:11522-11541`). The free
    `pdf::renderPage` (`pdfdoc.cpp:1956`) stays bare: `read_pdf`'s `image` reads a file perhaps not open (`qucscontrol_design.cpp:1611`).
    A tab's `screenshot` is a grab (`qucscontrol.cpp:11615`).
  - *GPU:* the swap deletes the viewport, so `reinstall` after it; strokes as an image in the lambda (`pdfdoc.cpp:294-297`).
  - *Keys:* Undo off. Escape clears a selection (`pdfdoc.cpp:1256`) with no override; with the pen up `PenKeys` takes it.
  - *Modes:* none.
- **Tools:** `show_pdf` (`qucscontrol_design.cpp:1728`) gives readings. `send_input`: `target` `page`, a new `page` beside
  `slide`, points from its top left.
- **Suite:** `test_pdf_viewer` (only with Qt's PDF module, `tests/CMakeLists.txt:380`). On the display,
  `everyCommandLeavesItWhole` can hang in the native save panel, an older fault.

### Picture (`ImageDoc`, `ImageView`)

- **Part and measures:** the frame from 1 (`setFrame`, `imagedoc.cpp:1610`). The picture's pixels as in the file, not turned:
  `toPicture`, `toViewport` (`imagedoc.cpp:453`, `:458`), so a stroke turns with `setTurns` (`:589`). `pixelsPerUnit` is `zoom()`.
- **Reading says:** where the stroke is, its gesture and box, on which frame.
- **Cannot say:** anything of what the picture shows. The answer says so, rather than implying more (`read_drawings` gives a
  note: "what a picture shows is not known: screenshot shows it").
- **In the way:**
  - *Mouse:* left pans or, with Shift or the selection mode, selects (`imagedoc.cpp:843-863`); a double click toggles the zoom
    (`:903`).
  - *Print and pictures:* `print`'s world transform is the picture's pixels (`imagedoc.cpp:1854-1886`): strokes go after
    `drawImage`, before `restore`. `picture()` (`imagedoc.cpp:1709`), and so Copy as Image, gains them, turned as shown, and cut to the
    selection when it is. A `screenshot` is a grab.
  - *GPU:* none: not among the four (`gpurendering.h:31`).
  - *Keys:* Undo off. `ImageView::event` claims Escape only with a selection (`imagedoc.cpp:951-961`).
  - *Modes:* the selection mode is left alone and works again when the pen goes down. Reload with a new size can leave a stroke
    off the picture; it is kept.
- **Tools:** no show tool: the `pen` tool and `read_drawings`. `send_input`: `target` `picture`, its pixels.
- **Suite:** `test_image_doc`.

### RF 2.5D (`RF25Doc`): the layout and the cross-section

- **Part and measures:** on the layout, as on any: cell name, µm, y up (`drawingPart`, `layoutdoc.cpp:618`). `RF25Doc::view()`
  already has `Mode::Draw` and `readDrawing`; nothing takes the pen up. On `StackView`, one part (`stack`), in the widget's
  pixels as fractions of its size, since the picture is not to scale: thick layers share the room by thickness with a floor
  under each (`rf25doc.cpp:365-368`).
- **Reading says:** the layout's shapes by layer, texts and placements, as now, plus **ports and terminals** by name from the
  model (`a_model.ports()`, `rf25doc.cpp:1885`) and the overlays' labels (`LayoutView::Overlay::Label`, `layoutdoc.h:194`). On
  the cross-section, the layers it crosses by name (`Stack::layers`).
- **Cannot say:** a height in µm on the cross-section. A value on a current plot. What the mesh and processed overlays add:
  cells are not named.
- **In the way:**
  - *Modes:* the fight the brief named is real. `rf25doc.cpp:744` only ends placing on `Select`, so `setMode(Draw)` would leave
    `a_placing` and its button checked. Taking the pen up must `setPlacing(None)` first; `setPlacing` to a tool (`rf25doc.cpp:1435-1454`,
    called from `:684`, `:694`) puts the pen down first; `rf25doc.cpp:744` handles any mode but the placing's own.
  - *Mouse:* the layout's own. `StackView`'s click chooses a layer (`rf25doc.cpp:458`): filter it, paint a line in `paintEvent`
    (`:423`).
  - *Print and pictures:* `print` is `a_view->picture()`, a grab (`rf25doc.cpp:887-889`), so strokes are in. The cross-section is
    not printed today.
  - *GPU:* through `paintFrame`.
  - *Keys:* **Undo live** (`rf25doc.cpp:896-901`): Ctrl+Z must be claimed (`LayoutView::keyPressEvent` at `layoutdoc.cpp:1596`
    never sees it while the model has a step, and `slotEditUndo` undoes the model, `qucs_actions.cpp:852`). Escape already goes to
    the view (`layoutdoc.cpp:1333-1334`).
- **Tools:** `show_layout`: take `pen` and `drawings` out of the refusal at `qucscontrol_layout.cpp:672` and read them in the RF
  branch. `send_input`'s `layout` is `LayoutDoc` only (`qucscontrol_input.cpp:252-259`): extend it; add `stack`.
- **Suite:** `test_rf25_doc`; no solve is needed.
- Mark in the pen-markers note what this supersedes: its "An RF 2.5D model's own layout has no pen".

### Multiphysics 2D (`MultiphysicsDoc`, `GraphicsView`)

- **Part and measures:** what is shown: `geometry`, `mesh`, `plot:<tag>` (`shownPlotGroup`, `multiphysicsdoc.h:219`), and
  `step:<tag>` while a process step is shown (`a_viewPartial`, `multiphysicsdoc.h:371`). A stroke on a plot says which, by its
  name. The model's units, y up (`toModel`, `toPixel`, `multiphysicsview.cpp:341`, `:346`), `scale()`.
- **Reading says:** domains, boundaries and points, numbered as `fem_describe` numbers them and named by their objects
  (`Topology::domainAt`, `boundaryAt`, `pointAt`, `multiphysics/fem_geometry.h:136-139`; `objectNames`, as the status line does at
  `multiphysicsdoc.cpp:3572-3574`). On results, the plotted value at a dot, an arrow's tip or a corner's point: split the
  evaluation out of `pointerMoved` (`multiphysicsdoc.cpp:3575-3598`) to share it. It takes the first enabled surface plot.
- **Cannot say:** a value on a contour-only or arrow-only plot. Which mesh cell, or which ray. A value when no solution is held
  (solutions are not kept with the `.qfem`, so after reopening the plot must be solved again). After a re-solve the value is the
  new solution's, not the one the user looked at.
- **In the way:**
  - *Mouse:* left pans or picks (`multiphysicsview.cpp:442-482`), hover redraws (`:463-469`). Taken by the filter while up; a
    pick level (`setPickLevel`) is untouched, working again when the pen is down.
  - *Print and pictures:* one line in `paintFrame` (`multiphysicsview.cpp:548`), after what it paints and before `a_gl = nullptr`. That covers
    `picture()` (`:524`), `print` (`multiphysicsdoc.cpp:626`) and Save Image (`:3603`).
  - *GPU:* through the same `paintFrame` on `GpuCanvas`; the filter's Paint branch is off here.
  - *Keys:* **Undo live** (`multiphysicsdoc.cpp:634`). Its arrow keys and F stay the view's.
  - A 1D plot group is not here: it opens a data display (`qucscontrol.cpp:969`), which is the [schematic's](#schematic-symbol-data-display) surface.
- **Tools:** `fem_plot` (`qucscontrol_fem.cpp:2831`, in every turn with a model, `qucscontrol.cpp:1263`). `send_input`: `target`
  `model`, its units, y up.
- **Suites:** `test_multiphysics_view` for the view, `test_multiphysics_doc` for readings.

### Markdown rendering

`MarkdownDoc` **is** a `TextDoc` (`markdowndoc.h:192`) with `preview()`, a `QTextBrowser` (`:209`), and modes Edit, Split, Preview.
This phase is the rendering; the source is the [text](#text-and-code) phase's. In Edit mode, before that phase, it is greyed.

- **Part and measures:** one part, `rendering`. Pixels of the laid-out text from its top left, as laid out now, y down. They
  change with the width, and the answer says "as laid out now".
- **Pinning (decision 9):** a stroke is pinned whole to the character nearest its middle. `points` hold its shape at drawing;
  `pins` hold that character's position and its rectangle then. Shown, it moves by how far the character moved, keeping its
  shape, so its gesture never changes. On an image's character it also scales with the image (`fitImages`, `markdowndoc.h:77`).
  Pinning each point instead was rejected: a ring's points lie between lines, near characters a rewrap sends far apart, so it
  would grow spikes.
- **When the source is edited in Split mode:** `renderNow()` rebuilds everything (`markdowndoc.cpp:1063-1091`), so before it each
  stroke remembers its block's text, index and offset. After it, a block with the same text nearest the old index takes it. Else
  the block with the longest common start and end, with the offset moved by the edit. Else the block nearest the old index, and
  the reading says `"its text changed"`. A re-render for an image or a palette finds the same text, so nothing moves.
- **Reading says:** targets from `QTextLayout`, block by block: paragraph, heading, code, table cell, image (by its file), each
  with word boxes. Geometry as `QuoteBars::bars` gets it (`markdowndoc.cpp:489-503`).
- **Cannot say:** what is inside an image, or a formula's parts (a typeset object: "a formula"). A long underline past a rewrap
  sticks out rather than following the words, and reads what it lies on now.
- **In the way:**
  - *Mouse:* left selects and follows links; the viewport tracks the mouse (`markdowndoc.cpp:1004`).
  - *Print and pictures:* File > Print prints the **source** (`printerwriter.cpp:106-109`), so no strokes: left out. A
    `screenshot` is a grab of the tab, with them.
  - *GPU:* `setPreviewGpu` (`markdowndoc.cpp:986`) remakes the viewport; `reinstall` beside `QuoteBars::install` (`:1002`). Strokes as an image
    last in `PreviewTiles::draw` (`:595`), after the bars (`:662-663`).
  - *Keys:* Undo live on the source, whose editor keeps the focus: `PenKeys` claims it.
- **Tools:** `get_text` (`qucscontrol_text.cpp:69`) carries the readings. `send_input`: `target` `rendering`, its pixels.
- **Suite:** `test_markdown_doc`; run on the display too (it found colour faults there before).

### Multiphysics 3D (`Multiphysics3DDoc`, `View3D`)

- **Part and measures:** what is shown (`Show`, `multiphysics3ddoc.h:162`): `geometry`, `mesh`, `solution`, `plot:<tag>`
  (`plotGroup()`, `:173`). `pins` hold each point's x, y, z and face. `points` hold the shape in the screen's plane at drawing, in
  model units, so a ring's gesture is judged as drawn, whatever the turn since.
- **New on `View3D`:**

  ```cpp
  struct Hit { bool onModel = false; qucs_s::fem::Vec3 point; int id = -1; };
  /// The frontmost triangle at a pixel, from depth and ids kept until invalidate().
  Hit hitAt(QPointF pixel);
  QPointF toPixel(const qucs_s::fem::Vec3& point) const;
  /// Not behind a triangle, not cut away.
  bool seen(const qucs_s::fem::Vec3& point);
  ```

  `render` keeps its depth local (`view3d.cpp:163`) and hands back ids only (`:223`). `idAt` renders the whole scene per call
  (`:359-369`), which a 200-point stroke cannot afford: keep both buffers until `invalidate()` (`:151`). `project` is private
  (`view3d.h:127`).
- **A point off the model** goes in a plane facing the camera through the stroke's last hit, or the model's centre.
- **When its far side turns away:** what is behind the model is **hidden**, as an edge there is (`view3d.cpp:224-234`), and
  shows again when turned back. A point beyond a cut is hidden as a line is (`:237-242`). Rebuilding the geometry leaves a stroke
  where it was in space.
- **Reading says:** the faces under its points, as face number and the solids they bound (`Topology3::Face::objects`,
  `multiphysics/fem3_geometry.h:111`; `objectNames`). What a ring holds is sampled from the ids **when it is drawn**, since only then
  is its camera known, and kept in `pins`. The plotted value at a dot or tip, by `evaluateAt3`
  (`multiphysics/fem3_results.h:104`).
- **Cannot say:** which face, on a slice or an isosurface (id -1: `multiphysics3ddoc.cpp:1546`, `:1581`, `:1681`): it reads "on a
  slice", its point and value. Which element. Whether a mark seen from one side was meant for the solid behind.
- **In the way:**
  - *Mouse:* left turns; right, middle and Shift-left pan (`view3d.cpp:399-408`). **With the pen up nothing turns it** but the preset
    buttons (`multiphysics3ddoc.cpp:310`) or `fem_plot`'s `azimuth`: open question 7.
  - *Print and pictures:* `print` is `picture(2)` (`multiphysics3ddoc.cpp:475-477`); `picture` has `paintOverlay` (`view3d.cpp:340`).
    Buffers at the screen's ratio serve a picture at another, mapped by logical pixel.
  - *GPU:* none: this view is CPU only at d8b5942 (`view3d.h:2`).
  - *Keys:* **Undo live** (`multiphysics3ddoc.cpp:483`).
- **Tools:** `fem_plot`'s 3D branch, `fem3Plot` (`qucscontrol_fem.cpp:3726`). `send_input`: `target` `view3d`, `click` of [x, y, z]. `xyOf`
  takes two numbers (`qucscontrol_input.cpp:107`), so this target needs its own; a point is projected, and refused if hidden.
- **Suite:** `test_multiphysics3d_doc`.

### Schematic, symbol, data display

`Schematic`, a `Q3ScrollView`, for `.sch`, `.sym` and `.dpl` (`qucscontrol.cpp:2346-2363`).

- **Part and measures:** `schematic` or `symbol` by `getSymbolMode()` (`schematic.h:427`), since a document holds both drawings.
  Model units, y down. New: a double-precision model-to-pixel, from `a_Scale` and `a_ViewX1`, `a_ViewY1` as `modelArea` uses them
  (`schematic.cpp:782-787`).
- **Reading says:** parts by name (`a_DocComps`), wires with their net's label if it has one (`netLabelOf`), labels, a diagram by its
  number (as `get_schematic` numbers it, `d->boundingRect()`) with its graphs' variables. At a dot or tip on a rectangular diagram,
  the axes' values, by the status line's own `status::readout` (`schematic.cpp:1393-1395`).
- **Cannot say:** an unlabelled wire's name beyond its net's number. A value on a non-rectangular diagram. A curve by itself
  (an arrow at a curve points at the diagram; its point's values say where).
- **In the way:**
  - *Mouse:* `contentsMousePressEvent` etc. (`schematic.cpp:1444`). The left button never reaches them, so the `MousePressAction` in
    hand is **untouched**: wiring or a part in hand carries on when the pen goes down. No `setChanged` is called, so it is never
    marked changed. Moves with no button still pass, so a part in hand follows the pointer. A component's text editor stays open
    unless `slotHideEdit` is called at the press (`schematic.cpp:1448`).
  - *Paint cost:* a plain update redraws the scene (`schematic.cpp:733-755`). A stroke's steps should use `Gesture::Drawing`
    (`schematic.h:652`), which paints from the held scene (`schematic.cpp:863-889`). Check on `test_large_schematics`.
  - *Print and pictures:* `print` (`schematic.cpp:1514`) and `paintSchToViewpainter` (`:1595`) gain strokes. **The default `screenshot` is "paper"**, by
    `graphicsexport::image` (`qucscontrol.cpp:11606-11611`, `graphicsexport.cpp:328`), so `Options` needs a strokes flag or Claude's
    default picture would not show them. `export_image` and File > Export do not set it (decision 12).
  - *GPU:* none.
  - *Keys:* **Undo live**, and Escape would call `slotEscape` (`qucs_actions.cpp:519-529`), which ends the mode. `PenKeys` must claim
    both; test by pressing them through the window's shortcuts after activating the window.
  - *Modes:* see *Mouse*. Hover, tooltips and the status line stay.
- **Tools:** `get_schematic` (`qucscontrol.cpp:6526`). `send_input`: `canvas` exists (`qucscontrol_input.cpp:238`); give it the `drawn`
  answer.
- **Suite:** `test_canvas_drawing`; `test_mouse_wheel` for the wheel under the pen; `test_large_schematics` for cost.

### Text and code

`TextDoc`, a `QPlainTextEdit`, and with it `PythonDoc` and `XmlDoc`'s text.

- **Part and measures:** one part, `text`. Pinned as in Markdown, but the document is edited in place, so
  `QTextDocument::contentsChange` moves the stored positions and no rematching is needed. Reported as line and column.
  No wrap by default (`textdoc.cpp:85`); Markdown's source wraps (`markdowndoc.cpp:772`); XML's follows its mode (`xmldoc.cpp:437`).
- **Reading says:** lines by number with their text, words on them, with the same gestures as a page.
- **Cannot say:** meaning: an arrow from a word to a word says only those.
- **In the way:**
  - *Mouse:* left selects. The line-number margin is its own widget, and is not drawn on.
  - *Print and pictures:* `QPlainTextEdit::print` (`printerwriter.cpp:109`) paginates by itself: left out. A `screenshot` is a grab.
  - *GPU:* none.
  - *Keys:* **Undo live.** While up, typing still goes to the text and Ctrl+Z is the pen's. Folded lines hide their strokes.
- **Tools:** `get_text`. `send_input`: `target` `text`, `click` of [line, column].
- **Suite:** `test_text_editing`.

### Spreadsheet (`SheetDoc`)

- **Part and measures:** the sheet's name. Pinned whole to the cell under its middle, by row, column and offset, scaled by `a_zoom`
  (`sheetdoc.cpp:494`). Positions by the headers' `sectionPosition`, which do not depend on the scroll.
- **Reading says:** cells named `B3` (`columnName`, `spreadsheet.h:133`), their shown values.
- **Cannot say:** what they mean; a chart in a workbook.
- **In the way:**
  - *Mouse:* left selects.
  - *Print and pictures:* no `print` at all (the `QucsDoc` default). A `screenshot` is a grab.
  - *Keys:* **Undo live** (`sheetdoc.cpp:483`). A Delete filter on the view stays (`:745`).
  - *Modes:* inserting rows or columns (`ShiftCommand`, `:82`) must move the pinned cells, or a stroke stays at its old row number.
- **Tools:** no sheet tool exists: `read_drawings`. `send_input`: `target` `sheet`, a new `cell`.
- **Suite:** `test_sheet_doc`.

### Archive and XML trees

- **Part and measures:** one part each, pinned to a row: in an archive by its entry's name (`NameRole`, `zipdoc.h:134`), because
  `rebuild()` clears the model (`zipdoc.cpp:792`) and persistent indexes die with it; in XML by the node's path, since the text is parsed
  again after each edit. A stroke is not drawn while its row is folded away or filtered out.
- **Reading says:** the entries or nodes it marks, by name or path.
- **Cannot say:** anything inside an entry.
- **In the way:**
  - *Mouse:* a left drag starts a drag out of the archive (`zipdoc.cpp:136`); the filter is before it while up. The archive's own
    filter on the viewport (`zipdoc.cpp:215`) passes the rest.
  - *Print and pictures:* none.
  - *Keys:* **Undo live** (`zipdoc.cpp:384`, and `TextDoc`'s for XML).
- **Tools:** `read_drawings`; the `xml` tool too (`qucscontrol_xml.cpp:48`). `send_input`: `target` `tree`, a new `row`.
- **Suites:** `test_zip_doc`, `test_xml_doc`.

### Left out of "any viewer"

The slide show and the thumbnails (as the pen-markers note left them); the docks and panels (the file browser, messages, consoles,
the Claude Code panel), which are not documents; a slide's notes pane.

## Claude's side

Every place, as of d8b5942:

| Place | Change |
|---|---|
| `QucsApp::drawingsOf` (`qucs.cpp:558-572`) | Through `penSurface()`: revision and `sentences()`. Knowing the revision, nothing is read, as now |
| The provider's comment (`qucs.cpp:934`) | Any document |
| `claudecodepanel.cpp:2284-2292` | None: it asks by file. An untitled document has no name to ask by (`qucs.cpp:930-933`): open question 11 |
| `readDrawings` (`qucscontrol_pptx.cpp:418-454`) | Over `penSurface()`. `measures` from the surface. The refusal becomes "...has no pen", with `tests/test_pptx_doc.cpp:2791` changed to match, on a kind that still has none. The nothing-drawn message (`qucscontrol_pptx.cpp:452`) |
| `insertDrawings` (`qucscontrol_pptx.cpp:408`) | Already kind-agnostic: called from each carrier |
| `inEveryTurn` (`qucscontrol.cpp:3867-3875`) | One loop over `penSurface()`, not the two classes |
| `read_drawings` in `kTools` (`qucscontrol.cpp:679`) | Rewritten for every kind; say "`measures` says each one's units". Its 1,332 characters may grow to 2,048 |
| `kActions` (`:1023`), `kSummaries` (`:1214`), `kSearchHints` (`:1377`) | Not "layouts and slides". Hints gain "pdf markdown picture multiphysics rf schematic" and so on |
| `kReadOnly` (`:1114`), `whileADialogWaits` (`:5192`), the dispatch (`:5340`), the subject (`:4178`) | Unchanged for `read_drawings`; the new tool joins the three that apply |
| New tool `pen` | `kTools`; `kActions` (a test needs "Qucs-S" in it, `tests/test_qucs_control.cpp:275-277`); `kSummaries`; `kSearchHints`; `kAutonomous` (`qucscontrol.cpp:3966`) and `kAdditive` (`:1123`), as `show_layout`; the dispatch; the subject; **not** `kReadOnly` |
| `show_layout` (`qucscontrol.cpp:639`, `:646`) | Description says RF 2.5D's `pen` and `drawings` work. 2,038 characters: trim first |
| `show_pptx` (`:669`, `:677`) | As it is |
| `show_pdf` (`:661`), `fem_plot` (`:969`), `get_text` (`:616`), `get_schematic` (`:148`), `screenshot` (`:296`) | Each says it gives the readings; `screenshot` that it shows strokes |
| `qucscontrol_layout.cpp:597-601`, `:816`, `:868-869` | Existing `pen` and `drawings` stay, going through `Pens` as the button does |
| `qucscontrol_layout.cpp:672` | `pen`, `drawings` out of the refusal loop |
| `qucscontrol_pptx.cpp:481-482`, `:539-543`, `:679-684` | Stay |
| `send_input` schema (`qucscontrol.cpp:343-356`) | `target` and `click` list new kinds. 2,003 characters of 2,048: **rewrite**, e.g. "a document's view ('target' as `read_drawings`' `measures` says)". New arguments: `page`, `cell`, `row`, each with a description (`theToolsAreListed` requires one) |
| `send_input` body (`qucscontrol_input.cpp:248-309`, `:339-378`, `:647-654`, `:705-721`, `:726-745`) | The `layoutView`, `slideView` branches become one over `surface->viewOf`, `toPixel`, `showPart`, `partOf`. Layout and slide tests must not notice. `laidOut` before mapping (`:91`), and map again as events go (the `mapped` lambda), as now |
| `shownInView` (`qucscontrol.cpp:11390`) | Optional: what parts are in sight |
| `scripts/ci/check-tool-arguments.py` | Should need no change: keep keys literal at the call. One `for (const char* key : {...})` loop per function, and no `args` handed to a helper with the key as a parameter, or it reports every key unread |

**The `pen` tool:**

```json
{"name": "pen",
 "inputSchema": {"type": "object", "properties": {
   "up": {"type": "boolean", "description": "The user's pen taken up (a drag draws on what the tab shows, in any tab) or put down"},
   "path": {"type": "string", "description": "The document: its file or its tab's title; the one in front when not given"},
   "clear": {"type": "string", "enum": ["part", "all"], "description": "What the pen drew taken away: on the part shown, or all of it"}}}}
```

It answers whether the pen is up and the document's readings without points. The tools check a schema's type before a tool runs but not
its enum, as the pen-markers note found, so `clear` is checked by the tool. `show_layout`'s and `show_pptx`'s `pen` stay.

## Order of work

Each phase is mergeable alone: nothing later is needed for it to be right, and greyed is an honest state for what is not yet done. The
sizes are rough and relative: **S** a few hundred lines with tests, **M** about a thousand, **L** two thousand or more.

**What done means, each phase, for what it touches:** drawn on with real input. A stroke stays on what it marks through pan, zoom, scroll,
part change, reflow and rotation; it is in prints and pictures where the viewer has them; undo, clear and eraser work. Tests with real
events, readings checked by what they say, not only that they exist. GPU paths checked on the display where they apply, what was not
exercised said plainly. `breaks.py`: every break caught at the assertion meant for it, tests sharpened where one got through. A write-up in
`docs/feature_gaps/`, in the manner of the pen-markers note, with "Not done". Changed tests pass in Release and `build-asan`; the full ctest;
the four lints (`scripts/ci/check-tool-arguments.py`, `check-json-nesting.py`, `check-third-party.py`, `check-shaders.py`). Named files staged.
A DMG from a clean detached worktree, driven through MCP: open the kind, draw through `send_input`, read with `read_drawings`.

| # | Phase | Size | Done, besides the above |
|---|---|---|---|
| 1 | **One pen and the toolbar**, on layouts and slides | M | `Pens::up`, the button and toolbar in step both ways with real presses, `PenSurface` wrappers, Claude's tables over surfaces, the `pen` tool. Others greyed. The user gets the toolbar |
| 2 | **Shared input and keys, and the PDF** | L | `PenInput`, `PenKeys`, `PdfSurface`, word targets, `show_pdf`, `send_input` `page`, GPU path on the display |
| 3 | **Picture** | S | The smallest second proof on a scroll area, and of turning. The brief had it later; it is cheap, and it tests the mapping |
| 4 | **RF 2.5D:** layout, then cross-section | M | Placing never fights the pen; Ctrl+Z is the stroke's, never the model's; ports named. Mark the old note superseded |
| 5 | **Multiphysics 2D** | M | Parts by what is shown, a value at a tip, on the GPU canvas too |
| 6 | **Markdown rendering** | L | Pin, rewrap, re-render after an edit, word targets, GPU tiles |
| 7 | **Multiphysics 3D** | L | Hit buffers, pinned points, hidden and cut, faces and values, the question of turning settled |
| 8 | **Schematic, symbol, data display** | L | Modes and "changed" undisturbed, keys pressed through the window, paint cost held, `screenshot`'s paper |
| 9 | **Text and code** | M | Pinning through edits; decide then if it is worth it |
| 10 | **Spreadsheet** | M | Pinned cells; decide then if it is worth it |
| 11 | **Archive and XML trees** | S each | Rows by name or path; decide then if worth it |

Phases 2 to 8 can be taken in another order, once 1 and 2 are in. "Any viewer" was the request, so 9 to 11 should be done if sound. If one
is not worth it, the write-up says which and why, rather than quietly leaving it out.

## Tests and breaks, phase by phase

The bar is the pen-markers note's table: real events, pixels read at their positions, a stroke's breadth measured either side of it, readings by what they say.
Function names here are suggestions, each in its viewer's suite. `breaks.py` goes in `docs/feature_gaps/<date>-pens-.../`, one list per phase.

| Phase | Tests | Breaks at least |
|---|---|---|
| 1 | `thePenButtonAndTheToolbarAgree` in `test_layout_doc` and `test_pptx_doc`, **with real presses both ways on both viewers**: button up, toolbar shows it; toolbar down, button shows it; a colour picked in either checked in the other; a stroke enables undo in both; toolbar's undo and clear change the menu's state, and the menu's change the toolbar's; a tab change keeps it up, Escape puts it down everywhere; Ruler puts it down; panes followed. `theToolsSeeAnySurface` in `test_qucs_control`. `test_toolbar_lock`'s 8. `test_long_lines` audit | **The two falling out of step, each way:** button not following `upChanged`; toolbar not following; a `modeChanged` that leaves the pen up; toolbar's undo acting on the last document; state not refreshed at a tab change. A free button publishing (caught by `aPenDrawsOnTheSlide`). `inEveryTurn` and `drawingsOf` missing a surface; the `pen` tool's enum |
| 2 | `aPenDrawsOnThePage`: on its page, several in sight, clipped at the edge, no selection or link followed while up, a double click two dots, middle pans, wheel zooms, Ctrl+Z and Escape pressed, eraser, print, `screenshot` `page`, kept across reload. `whatThePenDrewOnAPageIsRead`. `theRawInputDrawsOnAPage`. `aPenDrawsOnThePageOnTheGpu`, QSKIP offscreen, run on the display | Stroke on the wrong page; a filter not reinstalled after the GPU swap; paint not sent on; word boxes off by a page; print without strokes; a `PenKeys` claiming a find bar's Ctrl+Z; one not claiming Escape |
| 3 | `aPenDrawsOnThePicture`: turned with it, on its frame, print and copy, selection mode back after | Measures turned twice; wrong frame; print's transform |
| 4 | `aPenDrawsOnAnRf25Model`: pen and placing exclude each other both ways, model undo untouched by Ctrl+Z pressed, `show_layout` lifts its refusal; `aPenDrawsOnTheCrossSection` | Placing left checked; Ctrl+Z undoing the model; ports not read; cross-section's layer off by one |
| 5 | `aPenDrawsOnTheGraphicsView`: parts by plot, hidden on another, `picture()` has it; reading names domains, a value at a dot and at an arrow's tip | One part for all plots; value at the start, not the tip; stroke missing from `picture()` |
| 6 | `aStrokeFollowsItsText`: at two widths, on the same word; after an edit above it, it moved with it; word edited, reading says so; image rescaled. GPU on the display | Pin not updated; text matched by index alone; image not scaled; stroke's offset applied twice |
| 7 | `aStrokeIsPinnedInThreeD`: stays on its point through turn, zoom and pan by pixels; hidden turned away; cut away; ring's faces; value at a tip; off the model in its plane | Depth test off; point not unprojected; clip ignored; faces read by today's camera |
| 8 | `aPenLeavesASchematicAlone`: **Ctrl+Z and Escape pressed through the window's shortcuts**; the mode in hand and "changed" untouched; reading names parts and a diagram; `screenshot` shows it; `export_image` does not; symbol mode has its own | Window's Undo taking the key; `slotEscape` running; `setChanged` called; default screenshot without strokes |
| 9 to 11 | `aStrokeFollowsItsLine`, `aStrokeStaysOnItsCell`, `aStrokeStaysOnItsRow`: through edits, inserts, folding, filtering | Positions not moved by `contentsChange`; row kept by index |

Every phase: `QT_SCALE_FACTOR=2` once on what paints (`reference-qt-painting-costs`).

## Traps that apply

These come from the first brief and the project's notes (on this machine: `~/.claude/projects/-Users-meisam-git-Qucs-S-Enhancements/memory/`,
named here in case they are still there). They cost time before.

**Writing code**

- A `[&]` lambda of 15 lines or more crashes GCC on CI Linux: capture explicitly.
- No new identifiers `near`, `far` or `small`, because of `windows.h`. Existing code has some (`markup.cpp:213`, `:526`): don't copy it.
- CI has Qt 6.10.3 and this Mac 6.11.2: check new Qt APIs against v6.10.3 headers.
- Build one-element `QJsonArray`s with `append` (AppleClang 17 flattens the braces).
- A non-ASCII `QLatin1String` never matches: use `QStringLiteral` for anything with `µm`.
- `QJsonValue::toInt` hands back its default for a string or fraction: judge a `page` first, as `slide` does (`qucscontrol_input.cpp:294-302`).
- Round caps wider than a pixel go through `QStroker`, about six times slower, and `paint()` uses them (`markup.cpp:259`). Watch a long drawing on each frame.
- A label or tool button's text reaches the window's least width (`reference-qt-layout-widths`). Check the toolbar with `test_long_lines`.
- No vendor possessives in the UI: a lint checks it.

**Testing**

- `QCOMPARE(double, 0)` is fuzzy: `QVERIFY(x == 0)`.
- A failing `QTRY` waits three times over, past QtTest's 300 s watchdog: `QTest::qWaitFor` for long waits.
- Offscreen, `QTest::mouseDClick` misses a scroll area's viewport: send press, release, double click and release by hand.
- A test of a key must press it through the window's shortcuts, after `activateWindow` and `qWaitForWindowActive`, or a fault hides.
- GPU tests must `gpu::setWanted(viewer, true)` first and run on the display. On the display, read grabs at their ratio and compare colours in 8 bits.
- A QRhiWidget is constructed parentless, then `setParent`; `renderFailed` can be transient; `gpu::shownFrame` is what the screen shows.
- In `test_qucs_control`, close what you opened with `closeUnder` or `close_document`: `closeAllFiles` waits 60 s mid-suite.
- The sanitized build is what CI's Linux runs, and Qt's asserts fire there. Run changed tests in `build-asan` first.
- Timing ratios mean nothing under ASan: judge costs in Release only.

**Breaks and tools**

- A break run restores source, not the build: rebuild before trusting a result. A killed one leaves its edit: check the originals.
- A failed ninja leaves stale tests that still pass. A `breaks.py` needs its `sys.exit(main())` guarded, or it runs on load.
- The Bash tool's `grep` is a function that can silently find nothing: `/usr/bin/grep` or `git grep`. zsh does not word-split `$var`, and it mangles `$sha:r`.
- Headless runs set `QUCS_CACHE_DIR`, `S4Q_workdir` for old builds; packaged runs an isolated `HOME` and `QUCS_SETTINGS_DIR`. `python3 -B -I`. Kill Qucs-S by PID only.
- Stage named files: `qucs-s-26.1.1/examples/ngspice/NGspice features/LC_lowpass_optimization.pdf` must never enter a commit or DMG. Old DMGs go to `~/.Trash` by literal path. Use a worktree if another session has edits in main's checkout.

## Open questions and risks

Reading cannot settle these; each needs a probe, or the user.

1. **Does painting after the view's paint work on every CPU viewport?** It does on a `QTextBrowser`'s, where `QuoteBars` proves it. `QPlainTextEdit`, `QTableView`, `QTreeView`, `Q3ScrollView`, `PageView` and `ImageView` are unproved. Probe each at its phase's start, including a `grab()` and `render()` of it.
2. **On the GPU, is a strokes image good enough on a real display?** Uploading one on each stroke step, at Retina size. Alternatively a child over a `QRhiWidget` viewport: does it composite as hoped on a real display, and at what frame cost? As remembered, Qt documents widgets over `QOpenGLWidget` as working, but nothing here has tried it. If the image is too slow, use a child on the GPU path alone, made again after each swap. Offscreen cannot answer either: there is no RHI there.
3. **An old saved window state.** Does `restoreState` (`qucs_workspace.cpp:493`; version 1, `workspacesession.h:69`) leave a toolbar it does not know on the row `initToolBar` put it? Check with a real saved state; Python's toolbar came in the same way, but that was not traced.
4. **Cmd+Z on macOS's menu bar.** The window's Undo is in the Edit menu. Does a real Cmd+Z come through `ShortcutOverride` to an application filter before the menu's key equivalent? `sheetdoc.cpp:750` and `xmldoc.cpp:984` claim other keys that way, but neither claims Undo. Tests go through `QTest` or `send_input`, not the native menu, so this needs a human hand on the display, in phase 4 at the latest, where a wrong answer undoes a model edit.
5. **The cost of word boxes on a dense PDF page.** Each `getSelectionAtIndex` rereads the page. One option is to ask only for lines near a stroke. Measure before choosing.
6. **Paragraphs in a PDF** are a guess from line gaps: columns, footnotes and tables will fool it. Can a reading by line be enough?
7. **Turning a 3D model with the pen up.** Decision 3 leaves left for drawing and middle for pan, so nothing turns it but the presets. Choices: right drag turns while the pen is up (it pans today, and so does middle); or Shift-left; or accept it. This is the user's call, and it should be asked with decision 5.
8. **How much rigid pinning disappoints.** A highlighter across a line that later rewraps sticks out. Only use will tell. Pinning by word runs is the fallback.
9. **Should exports carry strokes?** Decision 12 follows the slides: print and `screenshot` yes, `export_image` and File > Export no. A user marking a schematic for a colleague may want them. The user's call.
10. **Does the user notice the pen is up?** One pen across tabs means a schematic can seem dead to clicks. The cross cursor and toolbar say so; a status bar hint (`initStatusBar`, `qucs_init.cpp:1559`) may be wanted too.
11. **Untitled documents.** A prompt reaches drawings by file name (`qucs.cpp:930-933`), and a new untitled schematic has none, so it is told nothing, though `read_drawings` finds it by its tab's title. Tell by document pointer, or accept it?
12. **Undo stays two stacks.** A stroke and a schematic edit undo separately, and the pen up decides which Ctrl+Z means. Decision 8 is simple but may surprise someone typing in a text file with the pen up.
13. **`Mode::Draw` in hidden tabs.** With one pen, hidden layouts sit in Draw while it is up, which is harmless as read, but a hidden `LayoutView` that loads or fits should be watched.
14. **A hover in the schematic.** Moves with no button pass through to `MouseMoveAction`, which is what keeps a part following the pointer. Check that no action does more on a move than it shows.

## What is left out

- **Saving drawings** (decision 4). They live with the tab: nothing is written to a `.pptx`, a sidecar or a project.
- **Handwriting** is not read, as today.
- **The layout's and slide's own mouse code** is not replaced. If `PenInput` proves itself on every other viewer, moving them to it would be a clean later step, with their tests as the check.
- **Pen buttons on other viewers** (decision 1).
- **Text printing with strokes**, Markdown's rendering in print, and a sheet's print (none exists).
- **The slide show, thumbnails, docks and panels.**
- **A Pens shortcut:** none is added.
