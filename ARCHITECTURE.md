# Qucs-S 26.1.1 — Architecture Overview

This document describes the architecture of the vendored Qucs-S source tree in
`qucs-s-26.1.1/`. It was written after reading the build files, core class
headers, and key implementation paths of the upstream release (ra3xdh/qucs_s,
tag 26.1.1). Paths below are relative to the repository root.

---

## 1. Repository layout

| Path | What it is |
|---|---|
| `qucs-s-26.1.1/` | Vendored upstream source, with the three git submodules already checked out |
| `bin/{macos,linux,windows}/{intel,arm,...}` | Placeholder dirs for prebuilt binaries |
| `README.md`, `LICENSE` | Wrapper repo files |

## 2. Big picture

Qucs-S is **a Qt6 schematic-capture front end that delegates all numerical work
to external simulator processes**. It is a fork of the original Qucs (2003,
Michael Margraf) with the built-in simulator ripped out and replaced by
pluggable SPICE backends. The repo is a monorepo of ~10 executables built by
one CMake tree:

```
qucs-suite (top CMakeLists.txt)
├── qucs/                 → qucs-s          the main GUI (~200k lines incl. components)
├── qucs-filter/          → qucsfilter      passive filter synthesis
├── qucs-activefilter/    → qucsactivefilter
├── qucs-attenuator/      → qucsattenuator
├── qucs-transcalc/       → qucstrans       transmission-line calculator
├── qucs-powercombining/  → qucspowercombining
├── qucs-s-spar-viewer/   → (submodule)     S-parameter viewer
├── rxcalc/               → (submodule)     RF receiver calculator
├── qucsator_rf/          → (submodule)     the legacy Qucs simulator kernel (~40k lines C++)
├── library/              → *.lib component libraries (data only)
├── translations/         → Qt .ts files
└── examples/             → ngspice/, xyce/, qucsator/ example schematics
```

The satellite tools are **independent `QMainWindow` apps** with no code
dependency on `qucs/`; `qucs-s` launches them as child processes via
`launchTool()` in `qucs-s-26.1.1/qucs/qucs_actions.cpp` (`QProcess::start`,
killed on exit via the `signalKillEmAll` signal).

**Runtime dependencies (not built here):** ngspice, Xyce, SpiceOpus, Octave,
OpenVAF — all discovered by path in settings.

## 3. Build system

- CMake ≥ 3.10, **C++20**, Qt6 (Core/Gui/Widgets/Svg/SvgWidgets/Xml/PrintSupport;
  QtCharts for the spar-viewer).
- `CMAKE_AUTOMOC/AUTOUIC/AUTORCC` on. Release builds pass `-w` (all warnings
  off), Debug passes `-Wall -Wextra`.
- `config.h.cmake` → `config.h` injects `PACKAGE_VERSION`, `QUCS_NAME`, `GIT`.
- `qucs/` links against static sub-libraries:
  `components diagrams dialogs geometry paintings extsimkernels spicecomponents magnetics qt3_compat`.
- No test suite for the GUI (`enable_testing()` is present but only
  `geometry/test_geometry.cpp` and `qucsator_rf` have tests).

## 4. The core GUI (`qucs/`) — layered view

### 4.1 Process entry and global state

`qucs-s-26.1.1/qucs/main.cpp` does more than start the GUI:

- Populates the **global `tQucsSettings QucsSettings`** struct
  (`qucs-s-26.1.1/qucs/main.h`) — fonts, colours, paths to every external
  executable, `DefaultSimulator` bitmask, workspace dirs. This struct is
  `extern`'d and read everywhere.
- A second, newer settings layer `settingsManager`
  (`qucs-s-26.1.1/qucs/settings.h`, a `QSettings` subclass with defaults +
  legacy-key aliases, accessed via the `QucsSingleton<>` template). The two
  coexist — tech debt.
- `QCommandLineParser` gives **headless modes**: `-n` netlist export
  (Qucsator / `--ngspice` / `--xyce` / `--cdl`), `-p` print to file, `--icons`,
  `--doc`, `--list-entries`, `--run`. These instantiate `Schematic` without
  showing a window.
- Otherwise: `QucsMain = new QucsApp(...)` (another global).

### 4.2 Application shell — `QucsApp`

`QucsApp : QMainWindow` (`qucs-s-26.1.1/qucs/qucs.h`) is a 4,200-line
god-class split across three translation units by convention (the header has
comments marking which methods live where):

| File | Responsibility |
|---|---|
| `qucs/qucs.cpp` | Window layout (`initView`), project/file management, document tabs, component palette, library tree, **simulation dispatch** (`slotSimulate`, `slotSimulateWithSpice`, `slotAfterSimulation`, `slotAfterSpiceSimulation`, `slotDCbias`) |
| `qucs/qucs_init.cpp` | `initActions/initMenuBar/initToolBar/initStatusBar` — creates ~100 `QAction*` public members |
| `qucs/qucs_actions.cpp` | Slot implementations for edit operations (rotate/mirror/cut/paste/align…), launching satellite tools, export |

Layout: central `ContextMenuTabWidget DocumentTab` holding one `Schematic` or
`TextDoc` per tab; a left `QDockWidget` with a `QTabWidget` (Projects list /
Content `ProjectView` / Components palette `CompChoose`+`CompComps` / Libraries
`libTreeWidget`); a bottom `OctaveWindow` dock; a `MessageDock`. A
`QComboBox simulatorsCombobox` in the toolbar switches
`QucsSettings.DefaultSimulator` — which **re-registers the whole component
palette** (see 4.5).

### 4.3 Document model

```
QucsDoc  (plain C++ base: name, dataset, dataDisplay, script, scale, dirty flag)
 ├── Schematic : Q3ScrollView, QucsDoc     (.sch / .dpl / .sym)
 └── TextDoc   : QPlainTextEdit, QucsDoc   (.vhdl / .v / .va / .m / .oct / .cir …)
```

`Q3ScrollView` is a **vendored Qt3 compatibility widget**
(`qucs-s-26.1.1/qucs/qt3_compat/`) — the canvas never migrated to
`QGraphicsView`. All painting is immediate-mode `QPainter` in
`Schematic::drawContents`.

`Schematic` (586-line header, `qucs-s-26.1.1/qucs/schematic.h`) is split across
four `.cpp` files by concern:

| File | Concern |
|---|---|
| `qucs/schematic.cpp` | View: zoom, scroll, grid, frame, `drawContents`, event routing to `MouseActions`, undo/redo stack driving |
| `qucs/schematic_render.cpp` | The three coordinate systems (viewport ↔ contents ↔ model) and `renderModel()` — well documented at the top of the file |
| `qucs/schematic_element.cpp` | **Topology mutation**: create/find/merge nodes, insert/delete components and wires, split wires, selection, alignment, wire optimisation, `heal*` |
| `qucs/schematic_file.cpp` | Serialisation (`saveDocument/loadDocument`), clipboard, **undo** (`createUndoString/rebuild`), and **netlist generation** (`prepareNetlist/createNetlist/giveNodeNames/throughAllComps`) |

The document state is five `std::list<T*>` of raw owning pointers:
`a_DocComps, a_DocWires, a_DocNodes, a_DocDiags, a_DocPaints`, plus
`a_SymbolPaints`. Pointer aliases (`a_Components` etc.) are swapped to point at
either the schematic lists or the symbol list when `a_symbolMode` toggles —
that's how "edit subcircuit symbol" mode works.

**Undo is snapshot-based**: every change serialises the whole document to a
`QString` (same format as the `.sch` file) into `a_undoAction`, and undo calls
`rebuild()` which re-parses it. Simple, but O(document) per edit and it's why
`QucsSettings.maxUndo` exists.

### 4.4 Element hierarchy

Everything on the canvas derives from `Element`
(`qucs-s-26.1.1/qucs/element.h`): a plain struct with `cx,cy` (centre),
`x1,y1,x2,y2` (relative bounds), `Type` bitmask (`isComponent`, `isWire`,
`isDiagram`…), `isSelected`, and virtual
`rotate/mirrorX/mirrorY/moveCenter/boundingRect`.

```
Element
├── Component                     (components/component.h)
│   ├── MultiViewComponent        symbol regenerated from properties (e.g. Resistor EU/US)
│   │   └── GateComponent         digital gates
│   ├── Subcircuit, Lib, SpiceLibComp, VerilogA …
│   └── ~170 concrete components in components/ + ~70 in spicecomponents/
├── Conductor                     owns an optional unique_ptr<WireLabel>
│   ├── Node                      junction; lists of Wire* and Component* (non-owning)
│   └── Wire                      Port1/Port2 → Node*
├── Diagram (diagrams/)           Rect, Polar, Smith, Tab, Truth, Timing, 3D… own a list of Graph
├── Painting (paintings/)         Line, Arrow, Rect, Ellipse, Text, Image, PortSymbol, ID
├── Marker                        cursor on a Graph
└── WireLabel
```

A `Component` holds its symbol as lists of `qucs::DrawingPrimitive`
(`Line/Arc/Rect/Ellips/Polyline/Text`), a `QList<Port*>` (each `Port` has a
`Node* Connection`), and a `QList<Property*>`. `Property`
(`qucs-s-26.1.1/qucs/components/property.h`) has a fluent `Builder` and —
crucially for Qucs-S — a `spicecompat::Simulator` bitmask saying which
backends honour it.

**Connectivity graph**:
`Component.Ports[i].Connection → Node ← Wire.Port1/Port2`. Nodes are shared; a
`Node` with `conn_count()==0` is an orphan and gets garbage-collected by the
`disconnect*`/`delete*` functions.

### 4.5 Component registry — `Module` / `Category`

`qucs-s-26.1.1/qucs/module.cpp` builds the palette at startup with macros like
`REGISTER_LUMPED_1(Resistor)`. Each component class exposes a static
`info(QString& name, char*& bitmap, bool getNewOne)` factory;
`registerComponent` instantiates it once, checks
`c->Simulator & QucsSettings.DefaultSimulator`, renders a 128×128 icon, and
stores it in `Module::Modules` (keyed by `Model` string, e.g. `"R"`) and in a
`Category`. `Module::getComponent("R")` and `getComponentFromName(line)` are
the factories used by the file loader. Switching simulator calls
`unregisterModules()` + `registerModules()`, which is why the palette differs
between ngspice and Qucsator modes. Verilog-A user modules are registered
dynamically into `vaComponents`.

### 4.6 Interaction — `MouseActions`

`qucs-s-26.1.1/qucs/mouseactions.h` is a **function-pointer state machine**.
`QucsApp` holds four pointers (`MousePressAction`, `MouseMoveAction`,
`MouseReleaseAction`, `MouseDoubleClickAction`);
`Schematic::contentsMousePressEvent` calls
`(view->*App->MousePressAction)(this, event, x, y)`. Each tool (select, wire,
paste, rotate, marker, zoom…) is a set of `MPress*/MMove*/MRelease*` methods,
and toolbar actions swap the pointers. Temporary feedback while dragging uses
`Schematic::PostPaintEvent`.

Wire topology helpers live beside it: `qucs-s-26.1.1/qucs/wire_planner.h`
(orthogonal routing strategies: two-step XY/YX, three-step, straight) and
`qucs-s-26.1.1/qucs/healer.h` (a pimpl'd planner that emits `AbstractAction`s
against a `SchematicMutator` interface to reconnect wires after a move/rotate —
the newest, most "modern C++" part of the codebase).

### 4.7 Simulation backends — `extsimkernels/`

This is the layer that makes Qucs-S "S":

```
AbstractSpiceKernel : QObject            (abstractspicekernel.h, 1.8k lines)
├── Ngspice                              (ngspice.cpp)   also drives SpiceOpus
└── Xyce                                 (xyce.cpp)
```

Responsibilities of the base: pre-flight checks (ground present, a simulation
present, no SPICE-incompatible components), **netlist assembly**
(`startNetlist` walks `a_DocComps` and calls
`Component::getSpiceNetlist(dialect)` on each, emitting
`.FUNC`/`.IC`/`.PARAM`/components/`.MODEL` in order), `QProcess` lifecycle, and
a family of `parse*Output` methods that read ngspice raw/ASCII, Xyce
`.prn`/STD, Fourier, noise, PZ, sensitivity, HB outputs and **merge them into
one Qucs `.dat` dataset** (`convertToQucsData`) so the existing Qucs diagram
code can plot them unchanged.

`ExternSimDialog` (`qucs-s-26.1.1/qucs/extsimkernels/externsimdialog.h`) is the
modal console that owns one `Ngspice` and one `Xyce` instance and picks by
`QucsSettings.DefaultSimulator`.

`qucs-s-26.1.1/qucs/extsimkernels/spicecompat.h` holds the `Simulator` bitmask
enum (`simNgspice|simXyce|simSpiceOpus|simQucsator`), `SpiceDialect`
(`SPICEDefault|SPICEXyce|CDL`), and the Qucs→SPICE value/node/function
normalisers. Also here: `qucs2spice`, `s2spice` (S-param → subckt),
`verilogawriter`, `CdlNetlistWriter`, and the Xspice code-model build glue
(`xspice/`).

**The legacy Qucsator path** is separate: `QucsApp::slotSimulate` →
`SimMessage` dialog (`qucs-s-26.1.1/qucs/dialogs/simmessage.h`) →
`Schematic::createNetlist` writing Qucs's own netlist syntax via
`Component::netlist()` → runs the `qucsator_rf` binary. Digital (VHDL/Verilog
via FreeHDL/Icarus) also goes through `SimMessage`.

So every `Component` can emit up to five netlist flavours: `netlist()`
(Qucsator), `spice_netlist(dialect)`, `cdl_netlist()`, `va_code()`,
`vhdlCode()/verilogCode()`. The `Resistor` in
`qucs-s-26.1.1/qucs/components/resistor.cpp` is the canonical minimal example.

### 4.8 Results display

Datasets are the `<Qucs Dataset>` text format. `Diagram`
(`qucs-s-26.1.1/qucs/diagrams/diagram.h`) owns `Graph`s (each with a variable
expression, colour, style), loads data via `loadGraphData`, and each subclass
implements `calcCoordinate` for its projection (rect, log, polar, Smith).
`Marker` snaps a cursor to a `Graph`. `.dpl` data-display pages are just
schematics containing only diagrams and paintings.

### 4.9 Other subsystems

- `dialogs/` — ~25 QDialogs (settings, component properties, library builder,
  **tuner** for live parameter sweeps, matching, sweep).
- `magnetics/` — Jiles-Atherton core model + winding component.
- `geometry/` — small header-only concept/shape helpers (C++20 concepts), the
  only unit-tested part.
- `octave/` + `octave_window.cpp` — embedded Octave console; `.m`
  post-processing scripts.
- `python/` — a dataset parser and CLI wrapper (not built into the app).
- `third_party/osdi` — OpenVAF OSDI headers for compiled Verilog-A.
- `symbolwidget.cpp`, `qucslib_common.h` — library browser / `.lib` parser.
- `qucsshortcutmanager.*` — user-remappable shortcuts (recent addition).

## 5. File formats (all plain text, all hand-parsed)

| Ext | Format |
|---|---|
| `.sch` / `.dpl` / `.sym` | `<Qucs Schematic ver>` with `<Properties>`, `<Symbol>`, `<Components>`, `<Wires>`, `<Diagrams>`, `<Paintings>` sections; components are one-line `<Model Name active cx cy tx ty mirror rot "val" show …>`. Undo/clipboard reuse this exact format. |
| `.dat` | `<Qucs Dataset>` — `<indep name n>` / `<dep name indep>` blocks |
| `.lib` | `<Qucs Library ver "name">` with `<Component>` entries wrapping a `<Model>` line in `.sch` syntax, plus optional SPICE/Verilog-A/symbol sections |
| `.net` / `.cir` | Generated netlist (Qucsator syntax or SPICE) |

## 6. End-to-end simulation flow (ngspice case)

1. User hits F2 → `QucsApp::slotSimulate` → `DefaultSimulator != Qucsator` →
   `slotSimulateWithSpice`.
2. `ExternSimDialog` constructed for the current `Schematic`; picks `Ngspice`.
3. `Ngspice::slotSimulate`: checks → `SaveNetlist("spice4qucs.cir")` →
   `prepareSpiceNetlist` (which calls `Schematic::prepareNetlist` to assign
   node names via `giveNodeNames`/`throughAllNodes`, propagating labels and
   `gnd`) → `startNetlist` → per-simulation blocks with `.control` scripts
   writing one output file per analysis.
4. `QProcess` runs ngspice in `tempFilesDir`; stdout streamed to the console.
5. `slotFinished` → `convertToQucsData` parses every output file into one
   `.dat` next to the schematic.
6. Signal `simulated` → `QucsApp::slotAfterSpiceSimulation` → opens/reloads
   the `.dpl`, `Schematic::reloadGraphs()`, runs post-sim Octave script or
   `CMD` components, notifies the tuner if in tuning mode.

## 7. Things worth knowing before enhancing it

- **Global mutable state everywhere**: `QucsSettings`, `QucsMain`,
  `Module::Modules`, `Category::Categories`, `lastDir`. Most classes reach for
  `QucsMain->…` directly rather than being injected.
- **Two settings systems** (`tQucsSettings` struct vs
  `settingsManager`/`QSettings`) mid-migration.
- **Canvas is not `QGraphicsView`** — it's a Qt3 `Q3ScrollView` port with
  manual hit-testing (`getSelected(x,y)` per element) and full repaints. Any
  modern canvas feature (hover, per-item caching, GPU) means working in this
  layer.
- **Ownership**: raw pointers in `std::list` throughout the document model;
  only `WireLabel` (via `Conductor`) and the healer use smart pointers.
- **Large files**: `qucs.cpp` 4.2k, `schematic_element.cpp` 2.9k,
  `mouseactions.cpp` 2k, `abstractspicekernel.cpp` 1.8k lines. Header comments
  tell you which `.cpp` a method lives in.
- **Adding a component** = new class with `info()` factory + `createSymbol()`
  + the netlist overrides you need, then one `REGISTER_*` line in `module.cpp`
  and an icon in `bitmaps/`. Component visibility per simulator is controlled
  by the `Simulator` bitmask on the component and its properties.
- **Adding a backend** = subclass `AbstractSpiceKernel`, add a
  `spicecompat::Simulator` bit, wire it into `ExternSimDialog` and the
  simulator combo.
- No GUI tests; `.clang-format` and `.clangd` exist but Release builds compile
  with warnings suppressed.
