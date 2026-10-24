# RF 2.5D: what the solver would be given, shown in the panel and drawn in the view

`2026-10-24-rf25-mesh.md` called itself the last piece of phase A, and it was the last of its machinery;
this is the round that connects it. That round built the cells and their cost, and
`2026-10-24-rf25-layout-processing.md` the layer map and the processing steps before them - both of them
library code with tests behind them and nothing in the window calling either. Neither `processLayout()` nor `meshOf()` was
reached from the GUI: the panel showed the processing as settings with no report, and the view drew the
layout file and nothing made of it. This is the wiring, and it is what the proposal asks for in so many
words - "the panel should show the unknown count before solving", and, of the mesh, "it is drawn in the
view", which the Mesh node's own help text has promised since phase A began.

## What it is now

### Processed without reading the file twice

`processLayout()` read the layout file itself. A tab has already read it - on a thread of its own, so that
a large GDSII never holds the window - and reading it again to process it would hold the window for as
long as that first read took. So the file read is split off:

```cpp
Processed processRead(const Model& model, const layout::Layout& read, const QString& called = {});
Processed processLayout(const Model& model, const QString& folder, QString* error = nullptr);
```

`processRead()` is the whole of it - the cell the model names, the map applied, the steps run - over a
layout already in memory; `processLayout()` is the read and then that. The tab calls the first, Claude's
tools and the tests can call either, and a test holds the two to the same shapes, the same report and the
same problems over the real example layout.

### Made in the background, whenever it changes

`RF25Doc` gained a second piece of background work beside the layout read:

- **`prepare()`** copies the model, holds the layout as read, and on a thread of its own runs
  `processRead()` and then `meshOf()` with the settings of the `rf_mesh` node and the top of the study's
  sweep. The model is copied and the layout held by a `shared_ptr`, so an edit while it runs changes
  neither under it.
- **`schedulePrepare()`** coalesces a burst: a largest cell typed digit by digit is one run, not one a
  digit. A read that has just finished does not wait those 300 ms.
- **One run at a time.** A model that changed while a run went on is noted and made again when that run
  ends, so a slow layout leaves one run waiting rather than one an edit. A layout read again lets the run
  go: its shapes were of the layout as it was.
- **`waitForPrepared()`** starts a run that is still waiting its moment, because a test and a tool ask
  what the model says now, not what it said 300 ms ago.

`isBusy()` is now true while either piece runs, and `preparedChanged()` says when there is something new
to show. `preparedRuns()` counts the runs that have ended; it is there for the tests, which need to tell a
run that happened from a result that was already in place.

### Said where it is about

- **The tab's notes** list what each step did under *What the layout gives the solver*, and what the two
  of them cannot do under *What stands in the way of a solve* - the processing's problems and the mesh's,
  which are not the model's own "what is missing".
- **The tab's status line** carries the count that the proposal asks for - `110 cell(s), 108 unknowns;
  a dense matrix of 0.2 MB` on the example layout - beside the layers and the stack's height. While a run
  goes on it says so.
- **The panel** shows it under the very node that decides it: the steps under **Processing**, the cells
  and their cost under **Mesh**, and what the map let through and left out under **Layout** - as a report
  (`rfNodeReport`) above the node's problems, in the same shape the panel already used for what is
  missing. A node that decides none of it has no report, and the run ending refreshes whichever node is
  shown.

### Drawn over the layout

`LayoutView` gained a small, general overlay API, because the view is where the transform lives and the
RF 2.5D tab is not the only thing that may one day make something of a layout:

```cpp
struct Overlay { QList<QPolygonF> shapes; QColor line; double width; QString name; };
void setOverlays(const QList<Overlay>& overlays);
```

Outlines only, and no more fields than the two callers use: a first cut carried a fill colour and a pen
style that nothing set, which the paint then had to ask about on every shape. Three lines put either back
when something wants one.

The polygons are in the µm of the cell shown, which is what both `outlineOf()` and the processing already
speak, so nothing is converted. The view knows nothing of what they mean: it draws them over the layout's
own picture, in the order given, leaving its cache, its stats, its selection and its rulers alone.

They are drawn every frame, unlike the layout, so three things keep a mesh of tens of thousands of cells
cheap: only shapes whose bounds meet the visible region are drawn, a shape under two pixels across is
drawn as a point rather than a polygon of its own, and the count stops at `MostOverlayShapes` (40,000)
with the rest counted as left out rather than silently dropped. The GPU canvas paints through this very
`paintFrame()`, so the overlays go over its frame as they go over the CPU's picture - which is how the code
stands, not something a test has tried.

`RF25Doc` builds one overlay a layer from the processed conductors and one a layer from the mesh cells,
behind two toolbar buttons - **Processed Shapes** (on) and **Mesh** (off) - each remembered for the next
model opened.

## The colours, which took a second look

The first attempt drew the processed outline in the layer's colour lightened and the cells in it
darkened. A picture of the example layout showed why that is wrong: the layout's own shape underneath is
drawn *filled in that very colour*, and a tint of a fill is no line at all. So:

- The **processed outline** is the layer's colour told against the canvas - darkened on a light one,
  lightened on a dark one - which is a line over the fill it lies on, and still that layer's colour.
- The **cells** are the canvas's own ink at 190 alpha, because a cell is geometry and not metal: that
  reads over any layer's colour in either theme, and lets a fine mesh divide a shape without blacking it
  out.
- The canvas going light or dark takes both again (`colorsChanged`), which the first attempt did not: the
  colours were computed once and kept the old theme's.

## Tested

| Test | What it holds to |
|---|---|
| `aLayoutInMemoryIsProcessedWithoutReadingItAgain` (layout) | `processRead()` and `processLayout()` give the same conductors, outline for outline, the same report and the same problems over the real example layout; one layer of three mapped leaving shapes counted as left out; a cell the file has not is said of the name it was given |
| `theShapesAndTheMeshAreMadeOfTheLayoutRead` (doc) | A map that names nothing giving no conductors and an empty mesh said; layers 1 and 11 sent to M1 and M2 giving conductors on both and cells with unknowns and a cost; every report line in the notes; the cells and the cost in the status line; the report under Processing, Mesh and Layout, and under no other node; a read of the layout on its own making them again with no edit behind it; a layout file gone taking the shapes, the mesh, the overlays and the report with it |
| `theMeshIsMadeAgainWithoutReadingTheLayoutAgain` (doc) | A largest cell of 20 µm giving smaller cells and more of them with the layout not read again; two cells to a wavelength refused and said in the notes and under the Mesh node; the Mesh node left where it is across an edit and its report brought up to date by the run ending; the processing turned off leaving no merge in the report |
| `theProcessedShapesAndTheMeshAreDrawnOverTheLayout` (doc) | One overlay a layer holding that layer's conductors in that layer's colour; the cells' overlays holding every cell; either turned off leaving the other; the outline darker than its fill on a light canvas and lighter on a dark one, taken again when the theme changes; the cells in the canvas's ink; every shape drawn when the view is fitted and none when it looks elsewhere |
| `overlaysAreDrawnOverTheLayoutWithinTheirBudget` (layout viewer) | Three overlay shapes in sight drawn and one far away not, in the frame `setOverlays()` asks for rather than one the test forces or one left in the queue; the layout's picture neither changed nor drawn again under them (`renders()` still); 40,050 shapes drawn to the budget with 50 counted as left out; cleared, the layout alone |

## Broken on purpose

`docs/feature_gaps/2026-10-24-rf25-panel-and-view/breaks.py` holds twenty-five: the cell the model names
ignored, the shapes left out going uncounted, the overlays not drawn, a shape out of sight drawn anyway,
the budget made no limit, `setOverlays` not repainting, the overlays drawn into the cached picture,
nothing made after a read, nothing made after an edit, a waiting run never started for a test, the model
that changed during a run not made again, a layout read again keeping the shapes of the one before, the
notes and the status line each going quiet, what stands in the way not said, the panel's report renamed,
the report given to every node, the panel keeping the run before's report, either overlay drawn unasked,
the mesh's overlay holding the conductors, the outline taking the fill's own colour, the cells taking a
layer's colour instead of the ink, and the canvas's theme ignored in both directions.

## What the breaks taught

Six of the twenty-five went uncaught on the first run, three of those were still uncaught after the first
round of fixes, and every one of the nine was a test that could not see its break rather than code that
did not need it. Three of them are the same mistake in three clothes - **the test did the very thing the
code is there to do**:

- The repaint break was invisible because the test called `picture()`, which paints whatever `update()`
  did or did not ask for. The test now lets the event loop deliver the frame `setOverlays()` asked for and
  reads the count before forcing anything. (That break also named the wrong test at first: the one that
  could see it is in the layout viewer's own suite, not the RF 2.5D tab's. A break is only as good as the
  test it names.) Even then it went uncaught once more, and for a third reason: a paint event the earlier
  `fit()` had asked for was still in the queue, so *some* frame arrived whether `setOverlays()` asked for
  one or not. The test drains the queue first, and only then is what follows waiting on `setOverlays()`
  alone. A test that waits on an event loop has to own what is in it.
- The stale-panel break was invisible because the test chose the Mesh node anew, which rebuilds the form
  by itself. It now leaves the node where it is across an edit, which is where a user's cursor would be.
- The two `prepare()` calls that follow a layout read were invisible because the *edit* that reached them
  had already scheduled a run, and that run did the same work. Only **Read the Layout Again** reaches them
  with nothing scheduled behind it, so the test presses that path twice: once on a layout that is there,
  and once on a layout removed from under the tab.

**A result that was already there looks like a result.** Even on the Reread path, the first of those two
could not be caught: the shapes of the read before were still in place, and `processed()` being non-null
said nothing about whether this read had made anything. `preparedRuns()`, a count of runs that have ended,
is what tells one from the other; it exists for the tests and is what they now hold to.

**Two things broken the same way compare equal.** The shapes-left-out count was held only against
`processLayout()`'s, and the break zeroed both, since one calls the other. The test now maps one layer of
the three and holds the count itself to be above zero. This is the same trap as the first break written in
this round, of `outlineColor()`: the test compared the overlay's colour against `doc->outlineColor(...)`,
so both sides moved together - a tautology dressed as an assertion. It holds to the property now, that the
outline is darker than its fill on a light canvas and lighter on a dark one, and writing that break is what
showed the theme change leaving the old colours in place.

**A cache is not seen in what it draws, only in how often.** Drawing the overlays into the layout's own
picture gives the same shapes and the same stats; `renders()`, the count of pictures the CPU has drawn, is
what shows it. The test now holds that count still across two frames in which only overlays change.

## Not done

- **No mesh is kept with the document.** It is made again from the layout whenever the model changes and
  thrown away with the tab. For phase A, over the shapes a mapped layout holds, that is cheap enough; a
  solver will want it beside the model, as the multiphysics document keeps its own, and
  `2026-10-24-rf25-mesh.md` already notes it.
- **Every edit of the model makes it again, not only the edits that change it.** The processing and the
  mesh read the stack, the layout with its map and its steps, the mesh node, the study's top frequency
  and the parameters - but a port renamed or an export added starts a run too, because nothing compares
  what a run would read against what the last one read. The 300 ms coalescing hides it on the layouts
  this was tried on; a fingerprint of those parts would settle it, at the price of a stale mesh wherever
  the fingerprint misses an input, which is a worse failure than a wasted second. It was left for its own
  round rather than bolted on to this one.
- **The processing is not cancellable.** The layout read checks a flag and stops; a union over a
  hundred thousand polygons does not, so a run on a very large layout is abandoned rather than stopped -
  its thread finishes and its result is dropped. The mesh's own refusals bound the meshing, not the
  processing.
- **Nothing is drawn of the stack.** The cells are drawn in x and y; which layer a cell is on is said by
  its colour alone. The stack view beside the canvas still shows the layers, not where the cells sit in
  them.
- **Nothing has drawn them with the GPU canvas.** It paints through the same `paintFrame()`, so they
  should be there; the viewers are CPU by default (`8d85227`) and the overlay tests run on the CPU path.
  Worth a look when the GPU canvas is next exercised.
- **The "being made" states are not covered by a test.** The panel says *Being made…* under the three
  nodes, and the status line says the shapes and the mesh are being made, while a run goes on; a run over
  the layouts tried here ends in milliseconds, so there is no moment a test can hold to, and no break was
  written for them. Nothing has seen them: they are read from the code, and they are the one piece of this
  round with neither a test nor a look behind it.
- **The overlays carry no legend.** Each has a name - "M1, processed", "M1, the cells" - which nothing
  shows yet; the layout viewer's own layer list would be the place for it.
- **There are no `rf25_*` tools.** Claude cannot ask for the unknown count: the panel shows it, the tests
  read it, and the tools of the RF 2.5D model are still unwritten. That and an example `.qrf25` are what
  remain of phase A's conveniences before phase B's quasi-static extraction.
