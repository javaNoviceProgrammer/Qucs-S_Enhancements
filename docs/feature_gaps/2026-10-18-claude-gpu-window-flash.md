# The window flashed as a model opened, with the GPU chosen

The user (2026-10-18): *"when I open the qucs app and then open a qfem project, if the GPU option is active, the window GUI quickly restarts and then opens the qfem project. Why is that?"*

## Why

Qt's own doing, since Qt 6.4. A window of widgets is made with a plain (raster) surface when none of its widgets draws through Qt's RHI. When the first RHI widget is put into a window that is already shown, Qt makes the window's native window again with Metal's surface (Direct3D's, OpenGL's elsewhere). It destroys the native window, creates a new one, and puts back the state, position and visibility (`QWidget::setParent` → `QWidgetPrivate::recreate()`). On screen, the whole window goes and comes back.

The Graphics view on the GPU is such a widget (`LayeredCanvas`), and so are the PDF and Markdown viewports and the layout viewer's canvas. So the first of them, as a model opened, remade the window. Qt's log shows it (`QT_LOGGING_RULES=qt.widgets.painting.debug=true`), at the moment the model's view joins the window:

```
Recreating QWidgetWindow(…, name="QucsAppClassWindow") with current type QSurface::RasterSurface to support QSurface::MetalSurface
```

With the GPU not chosen, no RHI widget is made and nothing is remade.

## Now

Qt picks a widget window's surface when the native window is first made, from the RHI widgets in the window at that moment (`QWidgetWindow`'s constructor), hidden ones included. So when a viewer will draw with the GPU (`qucs_s::gpu::used()`: chosen in Settings… > Viewers, and nothing in the way), `QucsApp`'s constructor puts a hidden RHI widget in the main window before it is first shown (`qucs_s::gpu::readyWindow`). The window is then made for the RHI from the start, and the viewers join it as they open.

The same run as above, opening `vcsel_laser.qfem` from the command line with the Graphics view on the GPU:
- **Before:** the "Recreating" line, as the model opens.
- **Now:** the window evaluates to flushing with the RHI from its creation, and as the model opens there is no "Recreating".

**Still remade once:** when the GPU is chosen for a viewer during a session (Settings… > Viewers), the first viewer on the GPU in that session remakes the window, as before. From the next start on, the window is made ready.

**The cost:** with the GPU chosen, the main window is composed through the GPU from the start rather than from the first GPU viewer on. That is how it already was once a GPU viewer had been opened.

## Test

**test_markdown_doc `aGpuViewerLeavesTheWindowAsItWas` (new),** on the display (it skips where the RHI has no backend, such as CI's offscreen display). The Markdown preview on the GPU is opened in a new main window, and the test watches the window's native window (a `QPointer` to its `QWindow`, which Qt deletes when it remakes it):
- **The GPU chosen:** the window holds its hidden RHI widget, the native window is the same after the preview opens.
- **Not chosen:** no such widget, and the preview on the GPU still has Qt remake the window. This is Qt's behaviour, kept in the test as its control.

Three changes broken on purpose, all caught:
- the window not readied (QucsApp not calling it);
- its RHI widget shown (it would cover a part of the window);
- the window readied whatever is chosen.

test_markdown_doc passes 21 of 22 on the display (the one skip as before) and 17 offscreen (5 GPU functions skipped). The new function passes under the sanitizers on the display, with no report.
