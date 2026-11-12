# RF 2.5D's layout: drawn by the GPU, and a Ruler

The request: give the RF 2.5D layout a GPU setting in its own Settings…, as Multiphysics and Multiphysics 3D have one,
and put a Ruler button on it like the layout viewer's, to measure the layout.

## What was built

- **The GPU, its own setting.** An RF 2.5D model's layout is drawn by the layout viewer's canvas (`LayoutView`), which
  already had a GPU path (`layoutviewgpu.cpp`). Until now it followed the layout viewer's setting
  (`Viewers/layoutGpu`). Now it follows a setting of its own, `Viewers/rf25Gpu` (`gpu::Viewer::RF25`). Like the others,
  the CPU draws it unless the GPU is chosen.
  - `LayoutView` takes the viewer whose setting it follows, `LayoutView(parent, viewer)` or `setGpuViewer()`. It is the
    layout viewer's unless told otherwise. RF 2.5D's tab gives it `RF25` when it builds the view, so no GPU canvas is
    made from the layout viewer's setting and then dropped.
  - **RF 2.5D > Settings…** has a *Layout view* group with *Draw with the GPU*. A line under it says what draws the
    layout: the GPU and its backend, the CPU, or what keeps the GPU from drawing here (`QUCS_GPU=0`, an offscreen or
    remote display, a build without Qt's RHI). The choice is kept on OK, not on Cancel. Every open model's view is
    drawn that way at once.
  - **Settings… > Viewers** has the same setting as an *RF 2.5D layout* box.
  - The processed shapes, the mesh, the currents, the ports and what is being drawn are overlays. They are drawn over
    the GPU's layers as over the CPU's, the same way as in the layout viewer. Pictures and exports are the CPU's either
    way.
- **The Ruler.** A *Ruler* button sits on the tab's toolbar after *Read the Layout Again*.
  - While it is taken up, a drag over the layout measures. The ends snap to the shapes' corners and edges, and Shift
    keeps the ruler across or up.
  - Each ruler is labelled with its length on the canvas. The status line gives the last one's length, dx, dy and ends,
    followed by the pointer's x and y while the pointer moves.
  - Escape first takes back the ruler being drawn, then clears the rulers, then puts the Ruler down. Putting it down
    with its button keeps the rulers, as in a layout's tab.
  - The Ruler, the port tools, the drawing tools and the pen work one at a time. Taking up any of them puts the others
    down. If the view is put in the Ruler's mode some other way, the port tool in hand is put down and the view stays
    measuring.
  - The rulers stay when the layout is shown again in place: after a shape is drawn, or a map or colour changes. They
    go when another layout is read or the layout file is read again, which also fits the view.

## Not done

- Claude's tools (`qucscontrol_rf25.cpp`) cannot add or read RF 2.5D rulers. The layout viewer's tools are untouched.
- No context menu on the RF 2.5D view to clear the rulers: Escape does it.

## Files

- `qucs/gpurendering.{h,cpp}`: `Viewer::RF25`, its key and name, and the window readied for it.
- `qucs/layoutdoc.{h,cpp}`: `LayoutView`'s viewer: the constructor's argument, `setGpuViewer()`, `gpuViewer()`.
- `qucs/rf25settings.{h,cpp}`: the *Layout view* group, `gpuStatus()`, the setting kept on OK.
- `qucs/dialogs/qucssettingsdialog.{h,cpp}`: the sixth box of the Viewers tab.
- `qucs/rf25doc.{h,cpp}`: the view made with `RF25`; the Ruler button, its place among the tools, `rulerSaid()`, and
  the rulers kept when the layout is shown again in place.
- `qucs/qucs_rf25.cpp`: the Settings item's status tip.
- `qucs/tests/test_rf25_doc.cpp`: `theRulerMeasuresTheLayout`, `theLayoutIsDrawnByTheGpuAsItsOwnSettingSays`.

## Checked

- Both tests pass offscreen. There the GPU is unavailable, so the setting's status line names the reason.
- Both also pass on the Cocoa platform, where Metal is available. There the model's view is drawn by the GPU when its
  own setting is on, and by the CPU when only the layout viewer's setting is on.
- `test_rf25_doc`, `test_layout_doc`, `test_view3d` and `test_qucs_control` pass under ctest, and so do the four lints.
- The ruler test fails when the restored mode is taken out: a port tool put down would leave the view in Select.
