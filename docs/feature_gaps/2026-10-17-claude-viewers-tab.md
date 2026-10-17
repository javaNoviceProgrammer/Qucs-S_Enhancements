# The viewers drawn by the CPU; the GPU an opt-in on a Viewers tab

The user (2026-10-17): *"The gds viewer is also very laggy when turning a layer on or off. Maybe we should set the default for all these viewers back to CPU rendering and use GPU as a opt-in setting that user chooses in the settings. Add a new tab called "Viewers" to qucs settings GUI and add option for GPU rendering for pdf viewer, gds/layout viewer, markdown viewer, ... so that the user can choose."*

## A layer shown or hidden: the GPU's lag

**Why.** On the GPU the layout viewer's buffers were keyed on the set of layers shown. Showing or hiding one layer flattened the whole cell again: every shape tessellated, the non-convex ones by constrained Delaunay triangulation, and all of it uploaded. On 8,000 curves of 96 points (half non-convex C's), a layer shown or hidden took 394 ms, the whole rebuild. On a large chip it takes seconds.

**Now.**
- A layer hidden keeps its buffers and is not drawn; its texts are not drawn either.
- Shown again, it is drawn from them.
- A layer hidden from the start is flattened alone, and its buffers added, when it is first shown.
- The whole cell is made again only when the cell, the depth or the texts change, or when it is past its budget.
- On the same layout:
  - a layer hidden 4 ms, shown 4 ms;
  - one shown for the first time 128 ms (its 2,000 C's alone);
  - on the CPU, a hide 33 ms (the picture drawn again).

## The CPU by default, the GPU chosen

- **Settings… > Viewers** (a new tab): *Drawn by the GPU (OpenGL)*. A box for each viewer:
  - the PDF viewer;
  - the Markdown preview;
  - the layout viewer (GDS, OASIS);
  - the multiphysics Graphics view.

  Each is off unless chosen. Its tooltip says what the GPU does there. The status says whether the GPU can draw here (OpenGL 3.3, QUCS_GPU=0, a build without OpenGL). The choices left the Appearance tab.
- **Kept under new keys** (`Viewers/pdfGpu`, `Viewers/markdownGpu`, `Viewers/layoutGpu`, `Viewers/multiphysicsGpu`). The old ones (`Rendering/…`, `Multiphysics/gpu`) were on by default and are not read. So the CPU is everyone's default now, whatever an earlier version kept.
- **The Graphics view** reads and sets the same key in its own menu and in Multiphysics > Settings…. Before, it read the old key on its own, also on by default.

## Tests

- **test_layout_doc:**
  - `theViewersSettings` (was `theDrawingSettings`):
    - every viewer on the CPU, the old keys set to the GPU and not read;
    - the Viewers tab and its four boxes, unchecked, with the status; nothing in Appearance now;
    - all chosen and applied: kept under `Viewers/`, the open viewers (a layout, a PDF, a Markdown preview, a Graphics view) drawn so;
    - shown again as chosen, Cancel keeping nothing, QUCS_GPU=0 over a choice;
    - back to the defaults: the CPU.
  - `aLayerShownOrHiddenAtOnce` (new, on the GPU):
    - 8,000 curves with a layer hidden from the start;
    - a layer hidden and shown again: no buffers made, its colour gone from the framebuffer and back;
    - the first one shown: one layer added, all 8,000 shapes, none twice;
    - a sample's text layer hidden: its texts not drawn;
    - the times logged, a whole rebuild among them.
  - `theGpuDrawsAsTheCpu`: a layer hidden keeps its buffers (it was "made again"); the GPU chosen for it first.
- **test_pdf_viewer `theGpuDrawsAsTheCpu`:** the CPU's until chosen.
- **test_markdown_doc:** the new key.

## Breaks

Eight changes broken on purpose (one of them run on both platforms): nine runs, all caught.

- **The GPU by default again:** theViewersSettings; on the display, the PDF viewer's too.
- **The old key read:** theViewersSettings.
- **The Viewers tab not kept:** theViewersSettings.
- **The canvas made again whole:** aLayerShownOrHiddenAtOnce, theGpuDrawsAsTheCpu.
- **A hidden layer's texts drawn:** aLayerShownOrHiddenAtOnce.
- **Every shown layer flattened again when one is added:** aLayerShownOrHiddenAtOnce (14,000 shapes).
- **The Graphics view on the old key:** theViewersSettings, on the display.
- **Eigen's LU given another pattern** (2026-10-17-claude-ci-21ec256.md).

## Runs

- **The Release suite (offscreen):** 127 of 127.
- **On the display:**
  - test_layout_doc 30 of 30, twice;
  - test_markdown_doc; test_multiphysics_doc 57 of 57;
  - test_pdf_viewer `theGpuDrawsAsTheCpu`.

  A third run of test_layout_doc came while a sanitized solve and a build loaded the machine. In it, `aLayoutWrittenAgainIsReadAgainWhereItWas` found its view's centre a unit lower after the reload, x the same: a pan, as a wheel or a trackpad over its window gives. It passed alone three times and whole twice.
- **Under the sanitizers (offscreen):** all pass, no report:
  - whole files: test_layout_doc, test_pdf_viewer, test_markdown_doc, test_multiphysics_doc, test_python_tools, test_diagram_legend, test_fem_sparse, test_fem_resonator (366 s), test_fem_nitrides (731 s);
  - the changed functions of test_fem_semiconductor and test_fem_modulator (TE 0.9977).
- **The lints pass.**
