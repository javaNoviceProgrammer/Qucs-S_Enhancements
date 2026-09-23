# Qucs-S Enhancements

[![CI](https://github.com/javaNoviceProgrammer/Qucs-S_Enhancements/actions/workflows/ci.yml/badge.svg)](https://github.com/javaNoviceProgrammer/Qucs-S_Enhancements/actions/workflows/ci.yml)
[![Release](https://github.com/javaNoviceProgrammer/Qucs-S_Enhancements/actions/workflows/release.yml/badge.svg)](https://github.com/javaNoviceProgrammer/Qucs-S_Enhancements/actions/workflows/release.yml)

A hardened and extended build of [Qucs-S](https://github.com/ra3xdh/qucs_s),
the Qt circuit simulator front-end for ngspice, Xyce and SPICE OPUS. It
starts from the upstream 26.1.1 source, fixes the crash classes that source
had, and adds the workflow features listed below — with a headless test
suite, sanitizer builds and fuzzers behind every change. The work is done
with Claude Code, reviewed and driven by a human.

Ready-made bundles for macOS, Linux and Windows are on the
[Releases page](https://github.com/javaNoviceProgrammer/Qucs-S_Enhancements/releases).
This build calls itself **26.1.2** (`qucs-s-26.1.1/VERSION`; the source
directory keeps the name of the upstream version it started from). Documents
it saves say `<Qucs Schematic 26.1.2>`; upstream 26.1.1 asks before opening a
file from a newer version unless *Load documents from future versions* is on.

- [ARCHITECTURE.md](ARCHITECTURE.md) — how the upstream code base is put together
- [ENHANCEMENT_PROPOSAL.md](ENHANCEMENT_PROPOSAL.md) — crash root causes, the plan, and what has been done
- `qucs-s-26.1.1/` — the vendored upstream source (ra3xdh/qucs_s 26.1.1) with the changes applied

## Downloads

Every release carries one bundle per platform, built by the
[Release workflow](.github/workflows/release.yml) from a single Qt version:

| Platform | Bundle | Notes |
|---|---|---|
| macOS, Apple Silicon | `qucs-s-<ver>-macos-apple-silicon.dmg` | ad-hoc signed, not notarised: on first launch right-click the app and choose *Open*, or `xattr -d com.apple.quarantine <app>` |
| macOS, Intel | `qucs-s-<ver>-macos-intel.dmg` | same |
| Linux x86_64 | `qucs-s-<ver>-linux-intel.AppImage` | `chmod +x` and run; needs FUSE 2 (`libfuse2`) or `--appimage-extract` |
| Linux arm64 | `qucs-s-<ver>-linux-arm.AppImage` | same; needs glibc 2.38 or newer (Ubuntu 24.04, Debian 13) |
| Windows x64 | `...-windows-intel.zip` and `...-windows-intel-setup.exe` | |
| Windows arm64 | `...-windows-arm.zip` and `...-windows-arm-setup.exe` | |

No bundle includes ngspice: install it separately (`brew install ngspice`,
`apt install ngspice`, the Windows archive from
[ngspice.sourceforge.io](https://ngspice.sourceforge.io/download.html), …)
and point *Application Settings → Locations → Ngspice* at it if it is not
on `PATH` — on Windows at `ngspice_con.exe`. Without it the bundled
qucsator is the default simulator.

The rolling **`continuous`** pre-release is replaced on every manual run of
the workflow and follows `main` (its bundles carry the commit too:
`qucs-s-<ver>-<sha>-...`); tagged releases (`v*`) are permanent. A
`SHA256SUMS.txt` accompanies each release. Bundles live only on the Releases
page — nothing built is committed to this repository (`bin/` is git-ignored;
`scripts/fetch-binaries.sh` downloads a release into it).

## What is different from upstream

- **Crash hardening** (WS1 of the proposal): the dataset and schematic
  loaders and the simulator-output parsers survive damaged input, the app
  keeps autosave copies and writes a crash report with a backtrace, and the
  next start offers to restore what was open.
- **Verilog-A "Build All"**: right-click the *Verilog-A* row in the
  Content panel to compile every `.va` file of the project with OpenVAF (the
  executable set under *Application Settings → Locations → OpenVAF Path*),
  one after the other, with the compiler output in the message dock and a
  pass/fail tally at the end. Files with unsaved changes are compiled as last
  saved, after a confirmation.
- **Content panel sees the whole project**: files in subdirectories of the
  project (at any depth, hidden directories excluded) are listed under
  their category, either as `sub/dir/name.ext` rows or as sub-trees with
  one folder row per directory — right-click the empty area of the panel,
  *Toggle hierarchy search view*, to switch; the choice is remembered.
  The folder rows are plain; *Application Settings → Settings → Folder
  icons in the Content panel* puts a folder icon on them.
  Open, copy, rename, delete, drag and subcircuit insertion work on them in
  both listings. A new *Osdi* category lists the compiled `.osdi` models;
  ngspice loads all of them, wherever they are in the project, and Build
  All compiles the `.va` files wherever they are. Two more categories sit
  between *SPICE* and *Others*: *Python* (`.py`, `.pyw`; they open in the
  text editor) and *Images* (`.png`, `.jpg`/`.jpeg`, `.svg`, `.gif`,
  `.bmp`, `.tif`, `.webp` and the other formats Qt reads; they open with
  the system's viewer). Files in *Scratch* stay under *Scratch*.
- **A Scratch folder per project, a subfolder per schematic**: the
  temporary files of a simulation (netlist, the raw simulator output such
  as `spice4qucs.ac1.plot`, log) go to `Scratch/<schematic>/` inside the
  open project — `Scratch/amp/` for `amp.sch`, `Scratch/sub/amp/` for
  `sub/amp.sch` — instead of the user's cache directory, and stay there
  until the next run of that schematic, which updates the same folder
  (upstream's release builds deleted the raw output right after
  converting it, and every run overwrote the last). *Simulation → Show
  Last Netlist / Show Last Messages* open the files of the schematic in
  front (with a netlist in front, of the schematic simulated last), and
  *Simulation → Generate Netlist* writes the schematic's SPICE netlist
  there as a run would — `spice4qucs.cir` — without simulating, runs
  the schematic check first, and opens the file. The
  folder is created with the project (or when an older project is opened)
  and the Content panel lists its contents under a *Scratch* category
  below *Others*. Headless runs (`-n`, `--run`) keep using the simulator
  work directory from the settings, flat.
- **Text editor defaults and file types**: `.cir`, `.ckt` and `.sp` files
  open in the built-in text editor like `.va` and the other Qucs text
  documents; plain-text formats (`.txt`, `.py`, `.md`, `.json`, `.csv`,
  `.sh`, C/C++ sources, SPICE `.lib/.inc/.mod`, ngspice `.plot` files, the
  per-simulator datasets, ... — see `QucsApp::textFileSuffixes()`) open in
  the text editor from the settings, the built-in one by default. Under
  *Application Settings → File Types* a suffix can be registered with the
  program `qucs-editor` (there is a button for it) to open it in the
  built-in editor; a suffix registered with any program takes precedence
  over the defaults, and a program path may contain slashes (upstream cut
  it at the first one).
- **Embedded simulation console**: a simulation runs in a *Simulation*
  dock at the bottom of the window (tabbed with the build messages) instead
  of the modal "Simulate with external simulator" dialog, so the schematic
  stays usable while ngspice/Xyce work. The dock has the simulator's
  output, a status list, a progress bar, *Stop*, *Save netlist* and
  *Clear*; it comes up with the first simulation and can be shown or hidden
  from *View → Simulation Console*. A second Simulate while one is running
  is refused (the console says so); closing the schematic being simulated
  stops its run (upstream #235). *Simulation → Simulators Settings →
  Simulation console* offers two alternatives: the same console in a
  separate window that does not block the application either, or the
  legacy window — the dialog of earlier versions, which opens with every
  simulation, blocks until closed and stops a simulation still running
  when it is closed. The simulation toolbar (simulator choice, Simulate,
  Tune, …) starts a second row of toolbars.
- **Terminal and Python Shell docks** (*View → Terminal*, *View → Python
  Shell*): a shell — `$SHELL` as a login shell on macOS/Linux, PowerShell
  on Windows — and a Python interpreter, each in a dock next to the
  simulation console, started when the dock is first shown. On Unix the
  program runs on a pseudo-terminal, so prompts, echo and Ctrl-C work as
  in a terminal window; the output is shown as plain text (escape
  sequences dropped, carriage returns and backspaces applied), so
  full-screen programs are not for it. A command line below the output
  with a history (Up/Down), *Interrupt*, *Restart*, *Clear* and *Project
  dir* (a `cd` / `os.chdir` to the open project). The interpreter is the
  one under *Application Settings → Locations → Python Path*, or `python3`
  on `PATH` when that is empty.
- **The environment of your shell, even when started from the Finder or
  the Dock**: a desktop start gets a bare environment (`PATH` without
  Homebrew or `~/bin`, none of your exports), so simulators were not
  found and variables like `SPICE_LIB_DIR` were missing. At start Qucs-S
  now asks your login shell (`$SHELL -l -i`, so both the profile and the
  rc file count; fish and zsh-that-starts-fish included) for its
  environment and brings in what the process lacks — `PATH` is merged in
  the shell's order, with anything given on the command line kept in
  front; what the process already has is never replaced. It reports what
  it added on stderr. `QUCS_NO_SHELL_ENV=1` turns it off. Windows starts
  already carry the user's environment.
- **Content panel keeps itself current**: every few seconds it looks at
  the project's files and, only when one came, went or changed — saved by
  Qucs, written by a script in the Terminal dock, copied in by hand —
  lists them again (never while a simulation is writing its scratch
  files, under an open menu, or during a drag). *Application Settings →
  Settings* has the interval (default 3 s) and a checkbox to turn it off;
  *Refresh* on the right-click menu of the panel's empty area does it on
  the spot.
- **Editor panes** (*View → Panes*): documents side by side, up to a 2×2
  grid — a schematic next to its netlist, two schematics to compare.
  *Split Right* (Ctrl+\) and *Split Down* (Ctrl+Shift+\) open a new,
  empty pane; a pane's thin coloured top edge marks the active one, which
  is where files open (from the Content panel, the menus, or a drop) and
  which follows clicks and keyboard focus. *Move Document to Next Pane*
  (also on a tab's context menu; with a single pane it splits first),
  *Next Pane* (Ctrl+`) and *Close Pane* (its documents go to a
  neighbour); a pane whose last document is closed goes by itself. Save
  All, Close All and Find span every pane.
- **Theme** (*Application Settings → Appearance → Theme*): *System*,
  *Dark* or *Light* for the application's windows, menus, docks and
  dialogs. On macOS and Windows Qt asks the platform for the appearance,
  so the native controls, title bars and menus follow; elsewhere (Linux,
  and any platform that does not answer) a dark or light palette is put
  on the application. *System* takes the platform's colours again and
  follows them when they change. The schematic's own colours — document
  background, grid — stay the settings above it on the same tab, and the
  text editor is black on white in either theme.
- **Check Schematic** (*Simulation → Check Schematic*, F10): an electrical
  rule check before the simulator sees the circuit — component pins and
  wire ends connected to nothing (a wire end carrying a label is a named
  net, not a problem), two components of one name, no ground, no
  simulation block, and what the simulator in use would drop from the
  netlist (a component not available for it, one without a SPICE model,
  an implicit equation-defined device, a winding without its core) —
  listed on a *Problems* tab of the message dock with
  error/warning icons; a click on a row selects the component and centres
  the schematic on the place. Every simulation runs the check first and
  brings the tab up when there are errors (the run goes ahead anyway; the
  simulator has the last word). Subcircuits (schematics with ports) are
  not asked for a ground or a simulation. *Check Schematic and
  Subcircuits* runs it on the schematic in front and on every subcircuit
  it uses, at any depth (open documents as they are, the others from
  disk); a subcircuit's finding names its file and a click opens it
  there. A *Hierarchy and Netlist* toolbar to the right of the simulation
  toolbar holds *Go into Subcircuit*, *Pop out*, that check, *Generate
  Netlist* and *Save netlist*.
- **Net highlighting**: select a wire and its whole electrical net lights
  up — every wire and node reached through junctions, through wire labels
  of the same name (a `Vout` here joins a `Vout` there) and through ground
  symbols — as an orange glow under the wires, for as long as the
  selection lasts. Several selected wires light their nets together.
  Exports and prints never show it.
- **Diagram legend**: every graph diagram (Cartesian, polar, Smith, 3D, …)
  can show a legend — a sample of each graph's line (colour, thickness,
  style or symbol) with its variable — in a corner of its choice:
  *Edit Diagram Properties → Properties → Legend*. Off by default; the
  position is saved with the diagram, and files without it load as before
  (upstream #1719).
- **Dashed and dotted graphs look dashed and dotted** (upstream #1723):
  the pattern runs on along the whole curve. Upstream started it again at
  every data point, so a graph whose points were closer together than a
  dash - most simulations - came out solid. Also drawn now: a graph of
  two samples, the first curve of a sweep with two samples a step, and the
  last segment of a curve coming back inside a diagram with a fixed
  range - all of which upstream silently dropped.
- **`.OPTIONS` keeps every line, however it is written**: each line of
  the editor becomes a `.OPTION` line of the netlist, and a line may now
  be an option with no value at all (`noopiter`, `keepopinfo` and the
  other ngspice flags were dropped without a word), several options at
  once (`gmin=1e-10 reltol=1e-4` became one option with a nonsense
  value), or written out as a netlist line would be (`.option
  method=gear`). Before that, the first option of a `.OPTIONS`
  section was dropped from the ngspice netlist (or taken for the Xyce
  option package) once the section had been edited — the netlister and
  the file format take the first property for the Xyce package, and the
  equation editor rebuilt the properties from its lines without it. The
  package now has a field of its own in the properties dialog, the
  netlister finds it by name, and a schematic saved with the problem
  loads right (the misplaced option is put back).
- **A value says what it means as you type it**: under the property
  table of a component's dialog, a line reads the value being edited —
  `10 kOhm = 10000 → netlist: 10K`, an expression, a parameter's name,
  a list — with what the SPICE netlist will carry. Where SPICE and Qucs
  would read two different numbers it says so and the value turns
  amber: `10 Mohm` (Qucs: 10 mega; SPICE sees `10MOHM`, 10 milli),
  `10 meg` (the other way round), `1 KOhm` (K is no prefix to Qucs),
  and the European `4k7` or `2R2`, whose digits after the prefix are
  not read at all. The property's description is the tooltip of its
  name and its value.
- **Subcircuit properties in one table** (upstream #1285): *Edit
  Subcircuit Properties* (double-click the symbol's name text in symbol
  mode) shows the prefix and one table of parameters — Show, Name,
  Default, Type, Description — edited in the cells, as the component
  dialog is, instead of a read-only list with edit fields and an extra
  Apply-to-row step under it. Add gives a new row a name of its own and
  starts typing it; Remove takes the selected rows; Move Up/Down set the
  order (that of the symbol and the netlist); the type is picked from
  real, integer and string or typed. OK and Apply check the table first
  — a name missing, given twice or "File", or a character the file
  cannot hold — and show what is wrong at its cell; Apply writes into
  the symbol and keeps the dialog open.
- **One grid setting for every schematic**: *Application Settings →
  Appearance → Schematic grid* — *As each schematic says* (the
  default: each file keeps its own, as before), *Always hidden* or
  *Always shown*. The override applies to every open schematic at
  once, leaves the files as they are (they still carry their own flag,
  for upstream Qucs-S and for anyone else), and elements still snap to
  the grid; data displays keep their own. While it is on, *View → Show
  Grid* (Alt+G) turns it over for all schematics instead of changing
  the current file.
- **Schematics on dark paper**: symbols, wires and texts are drawn in
  fixed colours meant for light paper — dark blue above all — and
  vanished on a dark background colour. They now fit the paper they are
  drawn on: on dark paper a colour that would not show gets the
  lightness it lacks and keeps its hue (dark blue becomes light blue,
  black light grey, dark red pink), a colour that shows is left alone,
  and a diagram is drawn as a light card, as it prints. On light paper,
  and in every print and export, nothing changes. *Application
  Settings → Appearance → Dark schematic paper in the dark theme*
  (off by default) gives the canvas dark paper whenever the theme is
  dark, and follows the system when it switches; a dark *Document
  Background Color* of your own works the same way.
- **DC bias labels find their own place** (upstream #1692): after
  *Calculate DC bias* every value used to be drawn at the same fixed
  offset from its node, whatever was there — on the symbol, across a
  wire, on the next value. Each label now goes beside its node where it
  covers the least: first the corner it always had (upper left for a
  voltage, upper right for a current), then the other corners and
  sides, then a step further out with a thin leader line back to the
  node; never on another value while any other place is left, and the
  labels with the least room choose first. Values, units and colours
  are as before. Over the 242 shipped schematics with every bias value
  shown, labels on other labels went from 436 to 2, labels on a symbol
  or its text from 4135 to 2200, and labels a wire runs through from
  3950 to 478.
- **The operating point of every device** (upstream #789): after
  *Calculate DC bias* with ngspice, rest the mouse on a transistor,
  diode or Verilog-A (OSDI) device to see what it is doing — `ic`,
  `vbe`, `gm`, `gpi`, `cpi` of a BJT; `id`, `vgs`, `vth`, `vdsat`, `gm`,
  `gds` and the capacitances of a MOSFET; a diode's `cd`; whatever an
  OSDI model reports — with units, zeros left out. *View → Operating
  Point* opens the full list in the message dock: one row per
  component (transistors first, then resistors, capacitors and sources;
  a subcircuit's devices under it), every parameter ngspice gives, a
  filter (`gm` shows every device's gm, `T1` all of T1), *Copy* for a
  spreadsheet, and a click that selects the component. Nothing extra to
  set up: the DC bias run asks ngspice for it (`show all`).
- **Find in a schematic, and find and replace across the project**:
  *Edit → Find* (Ctrl+F) in a schematic opens a find bar under the
  pane — type a component's name, a net label or a value and the first
  match is selected and centred (names first: `R1` finds R1 before
  R10), Enter and Shift+Enter step through the rest, Escape closes it;
  a net label found lights up its whole net. It used to be disabled in
  schematics. *Edit → Replace…* (F7) is a new Find and Replace for
  component property values, in this schematic, in every open one or
  in every schematic of the project: narrow it to a type of component,
  to names that fit a wildcard (`R?`) and to one property, match case,
  whole values or a regular expression (with `\1` in the
  replacement), see every hit with the value it would get, untick the
  ones to keep, and double-click one to see it. An open schematic takes
  the change as one undo step; a project schematic that is not open is
  opened, changed and left for you to save — nothing is written behind
  your back. With nothing to find, a property is set whatever its value
  (`Temp` of every resistor to `-273.15`), which is what the old
  *Replace* dialog did for the schematic in front.
- **A schematic opens whichever simulator is selected** (upstream
  #1468): only the components of the selected simulator were known to
  the loader, so with ngspice selected a schematic holding a
  Qucsator-only part (a microstrip tee, an external transient block, …)
  did not open — the GUI offered to put an empty subcircuit in the
  part's place, which the next save wrote over the original — and with
  Qucsator selected a SPICE one did not. Of the 247 shipped examples,
  ngspice refused 30, Xyce 32, SPICE OPUS 31 and Qucsator 93; now every
  one opens under every simulator. Parts the selected simulator cannot
  take are drawn in grey and named by *Check Schematic* before a run,
  and an undo after switching simulators keeps them (it reloads the
  document from its own text, and used to lose them the same way). The
  component panel still offers only the selected simulator's parts —
  now also in the *equations* group, which used to offer the SPICE
  equation blocks to Qucsator and `.CSPARAM` to Xyce.
- **Component netlists audited**: every built-in component was netlisted
  in every flavour and run through ngspice
  ([docs/bug_hunts/](docs/bug_hunts/README.md)). Fixed: the 4-terminal
  transmission line paired the wrong pins as a port; the symmetric
  transformer applied T1 and T2 to the wrong windings; the 3 mutual
  inductors netlisted k12 for K13; I(TRNOISE) used RTSAM for all three RTS
  times; the digital source and the time-controlled switch played their
  pattern once instead of repeating it; the VDMOS card carried a `Temp`
  that overrode the circuit temperature, and `RQ=0 VQ=0` that switched
  quasi-saturation on and broke the operating point; a relay with the
  default `Ron = 0` had no operating point; the Xyce JFET card contained
  `UseGlobTemp`; the Xyce transient sensitivity had its `.TRAN` arguments
  in the wrong order; XSPICE-based components were offered for Xyce. And
  values follow Qucs notation now: a bare `10M` is 10 mega (SPICE read it
  as milli), `10 cm` is 0.1, `-1 MOhm` keeps its sign, `2*Rload` is braced
  so the simulator evaluates it. Properties the dialog showed but the
  SPICE netlist ignored either reach it (the diode's ISR/NR and Cp, the
  JFET's N/XTI/BETATCE, the MOSFET's NRD/NRS and Rg, a transmission
  line's Alpha as an LTRA, the potentiometer's contact resistance, error
  terms and tapers) or are hidden under a SPICE simulator as Qucsator's
  (the DC block's settings, the AC block's Noise, the delay of the
  controlled sources, …), the way the transient block's already were.
  Found on the way: the macOS bundle shipped without the SPICE
  subcircuits of the transformer, relay, switch, coax line and magnetic
  core (`share/qucs-s/spicelibrary`), and looked for its resources in the
  wrong place when kept under a directory named `bin`; both fixed, and
  the `simulate` smoke suite now runs circuits that include those files.
- **Making a subcircuit's symbol**: the instances of a subcircuit write
  the name of every pin inside the symbol — the name the netlist gives
  that pin — so a box no longer says only `sub`. *File → Symbol* holds
  the rest: *Recreate Circuit Symbol* throws the drawing away and lays
  the ports out around a fresh box, wide enough for the names it now
  carries (there was no way to redraw it once a symbol existed);
  *Pin Order…* says which pin is the first argument of the `.SUBCKT` and
  the first pin of every instance; *Save Symbol As…* writes the symbol
  into a `.sym` of its own and *Load Symbol…* takes one back, keeping
  this schematic's ports and moving each to where that symbol puts the
  port of its number. A *polyline* painting (open, or closed and filled)
  joins the line, arc, ellipse and rectangle — click a corner at a time,
  click the last one again to finish or the first one to close it — and,
  like every other drawing, it reaches the instances of the subcircuit
  and turns and mirrors with them. Under *Application Settings →
  Settings*, *Pin names in subcircuit symbols* (on) and *Pin directions
  in subcircuit symbols* (off): with the second, a mark on each pin says
  which way it points — from the type of the port it stands for — and a
  symbol drawn anew puts the inputs on the left and the outputs on the
  right. Fixed on the way: a schematic exported to PNG from the command
  line (`-p`) was painted over uninitialised memory, so its background
  was whatever had been in it.
- **A subcircuit's pin, its net and its symbol carry one name**: the
  netlist calls a pin of a `.SUBCKT` after the net the port sits on, so
  that is what the symbol now writes beside the pin — the label of the
  net if it has one, instead of the port's refdes. And a port on a net
  with no label of its own lends the net its name, so the pin reaches
  the netlist as `in` or `P2` rather than as `_net7`
  (`.Def:singleOPV P1 P2 P3 P4 P6`, where it used to be
  `.Def:singleOPV _net4 _net0 _net9 _net3 _net7`). A name that some
  other net already answers to is never borrowed — that would join two
  nets that are not connected — and *Check Schematic* says so when it
  happens.
- **Images, and images on a symbol**: an image is placed from the
  *paintings* group of the component panel, pasted from the clipboard, or
  dropped on the schematic from a file manager, and it can now be in any
  format Qt has a reader for — SVG and SVGZ included, next to PNG, JPEG,
  BMP, GIF, TIFF and WebP. The bytes of the file travel inside the
  document: the picture keeps working when the file it came from is
  gone, an SVG stays a vector and is redrawn sharp at every zoom instead
  of being frozen into pixels, and a photograph keeps its JPEG
  compression instead of being re-encoded as a much larger PNG. An image
  put on a subcircuit's symbol reaches the instances of that subcircuit:
  it is drawn as the background of the symbol and turns and mirrors with
  it, like the lines and arcs around it — before, it was dropped when
  the symbol was read, so the instance showed the symbol without its
  picture (or the plain `sub` box, if the picture was all there was).
  Fixed on the way: turning an image that had been read back from a
  document emptied it and left a file that could not be opened at all, a
  square image could not be turned, a mirrored one did not mirror, and a
  freshly placed or loaded one turned about the origin of the schematic
  instead of about itself.
- **Cursor-key moves are undoable and cancellable**: moving the selection
  with the arrow keys marks the document modified, is one undo step for the
  whole sequence, and Escape takes it back while it is the latest change
  (upstream #1525; upstream recorded neither).
- **Drag and drop from the Content panel**: drag one or more files onto the
  document area (a schematic, a text document, the tab bar) to open them —
  schematics, data displays and symbols in their views, Verilog-A and other
  text files in the text editor, anything else the way a double-click would.
  Files dragged in from a file manager open the same way.
- **Documents open from the system** (upstream #973): double-click a
  schematic (`.sch`), data display (`.dpl`) or symbol (`.sym`) in the
  Finder, the Explorer or a Linux file manager, or drop it on the Dock icon,
  and it opens in Qucs-S — in the window already running, on macOS.
  *Open With* offers Qucs-S for SPICE netlists (`.cir`, `.ckt`, `.sp`) and
  Verilog-A (`.va`) on macOS too. On the command line `qucs-s FILE...`
  opens documents and `qucs-s NAME_prj` opens a project; `-i` is not
  needed. Qucs-S registers itself for these types without taking any of
  them over from another program: macOS's *Default* handler for `.sch` and
  `.sym` and owner of `.dpl`; on Windows the installer lists it under
  *Open with*; on Linux the desktop entry and the MIME types
  (`share/mime/packages/qucs-s.xml`, telling a Qucs `.sch` from another
  program's by its first line) do the same.

The detailed record — root causes, what each change does and how it is
tested — is in [ENHANCEMENT_PROPOSAL.md](ENHANCEMENT_PROPOSAL.md).

## Building from source

Build dependencies on every platform: a C++20 compiler (GCC 12, Clang 15 or newer), CMake ≥ 3.16, Ninja,
Qt 6 with the Svg, Xml, PrintSupport, Charts and Tools (Linguist) modules,
flex, bison ≥ 3, gperf and dos2unix. ngspice is a run-time dependency only.
The source tree is `qucs-s-26.1.1/`; every recipe below configures it into a
sibling build directory (`build*/` is git-ignored) and stamps the commit into
the About dialog with `-DGIT`.

### macOS (Homebrew)

```bash
brew install cmake ninja qt bison flex gperf dos2unix ngspice
```

Homebrew's bison must shadow the one in Xcode:

```bash
export PATH="$(brew --prefix bison)/bin:$PATH"
cmake -S qucs-s-26.1.1 -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix qt)" -DGIT="$(git rev-parse --short HEAD)"
cmake --build build --parallel
```

The app is then at `build/qucs/qucs-s.app`, but it still points at the
Homebrew Qt and has no libraries, examples or tool apps inside. To get a
self-contained bundle you can run anywhere (and a `.dmg`):

```bash
scripts/package-macos.sh build bin/macos/apple-silicon
```

This is the same script the Release workflow uses. The bundle also carries
Qt's offscreen platform plugin, so its binary works headless too — the smoke
suites run against it unchanged. If Homebrew upgrades Qt underneath an
existing build directory, rebuild from scratch: the new headers carry old
timestamps, and an incremental build mixes objects of two Qt versions.

### Linux

Debian / Ubuntu (22.04 or newer):

```bash
sudo apt-get install build-essential git cmake ninja-build flex bison gperf dos2unix ngspice \
    qt6-base-dev qt6-tools-dev qt6-tools-dev-tools qt6-l10n-tools linguist-qt6 \
    libqt6svg6-dev qt6-charts-dev libqt6opengl6-dev libglx-dev libgl1-mesa-dev libcups2-dev
```

Fedora:

```bash
sudo dnf install gcc-c++ git cmake ninja-build flex bison gperf dos2unix ngspice \
    qt6-qtbase-devel qt6-qtsvg-devel qt6-qttools-devel qt6-qtcharts-devel
```

Arch:

```bash
sudo pacman -S base-devel git cmake ninja flex bison gperf dos2unix ngspice qt6-base qt6-svg qt6-charts qt6-tools
```

Then:

```bash
cmake -S qucs-s-26.1.1 -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGIT="$(git rev-parse --short HEAD)"
cmake --build build --parallel
build/qucs/qucs-s
```

To install (`/usr/local` by default; the tool apps, examples and library go
with it):

```bash
sudo cmake --build build --target install
```

If your distribution's Qt is older than 6.5, or you want exactly what the
releases use, fetch Qt 6.10.3 with [aqtinstall](https://github.com/miurahr/aqtinstall)
and point CMake at it:

```bash
pip install aqtinstall
aqt install-qt linux desktop 6.10.3 linux_gcc_64 -m qtcharts -O "$HOME/Qt"
cmake -S qucs-s-26.1.1 -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$HOME/Qt/6.10.3/gcc_64"
```

The AppImage the Release workflow publishes is made from an install into
`AppDir/usr` with [linuxdeploy](https://github.com/linuxdeploy/linuxdeploy)
and its Qt plugin; the exact steps are in the `linux` job of
[release.yml](.github/workflows/release.yml).

### Windows (MSYS2)

The releases are built with [MSYS2](https://www.msys2.org/) in the UCRT64
environment (CLANGARM64 on arm64), which is also the simplest way to build
locally. In a *MSYS2 UCRT64* shell:

```bash
pacman -S --needed git bison flex dos2unix zip \
    mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-gperf \
    mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-qt6-tools mingw-w64-ucrt-x86_64-qt6-svg mingw-w64-ucrt-x86_64-qt6-charts
```

Clone with LF line endings (`git config --global core.autocrlf false` before
cloning — the flex/bison sources and the smoke scripts need it), then:

```bash
cmake -S qucs-s-26.1.1 -B qucs-s-26.1.1/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PWD/qucs-s-26.1.1/build/qucs-suite" -DGIT="$(git rev-parse --short HEAD)"
cmake --build qucs-s-26.1.1/build --parallel
cmake --build qucs-s-26.1.1/build --target install
```

`qucs-suite/bin/qucs-s.exe` runs from the MSYS2 shell as is. To make the
`bin/` directory self-contained (runnable from Explorer, or to zip it), copy
the Qt and runtime DLLs next to the executables:

```bash
cd qucs-s-26.1.1/build/qucs-suite/bin
for exe in qucs-s qucs-sactivefilter qucs-sattenuator qucs-sfilter qucs-spowercombining qucs-strans qucs-sspar-viewer rxcalc; do
  windeployqt.exe "$exe.exe" --svg --no-translations --no-system-d3d-compiler --no-network
done
for f in $(ldd qucs-s.exe | awk '($3 ~ /\/ucrt64\/bin\//) {print $3}'); do case "$(basename "$f")" in Qt6*) ;; *) cp -f "$f" .;; esac; done
```

Install [ngspice for Windows](https://ngspice.sourceforge.io/download.html)
and set its path under *Application Settings → Locations*. The installer
(`-setup.exe`) is produced with [Inno Setup](https://jrsoftware.org/isinfo.php)
from `qucs-s-26.1.1/contrib/InnoSetup/qucs.iss`; the `windows` job of
[release.yml](.github/workflows/release.yml) has the exact commands,
including where it puts a bundled ngspice.

### Tests, smoke suites and fuzzers

The same checks CI runs, against an ASan/UBSan build (macOS shown; on Linux
drop `-DCMAKE_PREFIX_PATH` and use `build-asan/qucs/qucs-s` as the binary):

```bash
cmake -S qucs-s-26.1.1 -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH="$(brew --prefix qt)" -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
scripts/ci/smoke-test.sh hostile build-asan/qucs/qucs-s.app/Contents/MacOS/qucs-s qucs-s-26.1.1/examples /tmp/smoke
```

Unit tests live in `qucs-s-26.1.1/qucs/tests/` (QtTest, headless). They keep
their settings in a file under their temporary directory
(`tests/isolated_settings.h`), so a test run never touches your own Qucs-S
preferences; the application offers the same for trying a build out:
`QUCS_SETTINGS_DIR=<dir> qucs-s` keeps that run's settings in
`<dir>/qucs/qucs_s.ini`, and its crash reports and autosave copies under
`<dir>` as well — a trial run that is killed leaves nothing for your own
next start to report or offer.

`scripts/ci/smoke-test.sh` has three suites — `load` (render every ngspice
example), `simulate` (netlist → ngspice → dataset → render; needs `ngspice` on
`PATH`) and `hostile` (render a fixture against damaged datasets). Everything
runs headless through the CLI modes of `qucs-s`; no window is opened.

`scripts/ci/fuzz-sch.py` mutates the example schematics (truncation, dropped
fields, unbalanced quotes, absurd numbers, missing terminators, random bytes)
and pushes every mutant through the netlister (`-n`) and the renderer (`-p`).
With `--extra <dir>` it also takes schematics that have a dataset next to
them — the `simulate` suite's work directory — and damages the dataset one
time in six. A mutant may be rejected, but must never crash, hang or trip a
sanitizer. Runs are seeded and reproducible; each finding is kept as a
directory with the mutant, its dataset and the log:

```bash
python3 scripts/ci/fuzz-sch.py build-asan/qucs/qucs-s.app/Contents/MacOS/qucs-s qucs-s-26.1.1/examples /tmp/fuzz --count 500 --seed 7 --extra /tmp/smoke/simulate/work
```

`scripts/ci/fuzz-simout.py` does the same to the simulator's output: the
`simulate` suite (with a Debug build) stashes the real ngspice files of every
circuit it ran under `<work>/<circuit>/simout/`, and the fuzzer damages one
of them per mutant and runs `qucs/tests/simout_harness`, which converts them
to a Qucs dataset exactly as the GUI does after a simulation:

```bash
python3 scripts/ci/fuzz-simout.py build-asan/qucs/tests/simout_harness /tmp/smoke/simulate/work /tmp/fuzz-simout --count 500 --seed 7
```

`qucs/tests/test_netlist_audit` places every built-in component on a
schematic and checks that it netlists in every flavour without a crash and
survives a save/load and a properties-dialog Apply unchanged. With
`QUCS_NETLIST_AUDIT=<dir>` it also writes the surveys (every component's
netlists, and every property marked to show which ones the SPICE netlist
ignores), and `scripts/netlist-audit.sh <build> <out>` runs the resulting
decks through ngspice. What that turned up is written up in
[docs/bug_hunts/](docs/bug_hunts/README.md).

## Crash reports and recovery

If the app dies, a report is written to the application-data directory
(`~/Library/Application Support/qucs-s/crash-reports/` on macOS,
`~/.local/share/qucs-s/crash-reports/` on Linux, `%LOCALAPPDATA%\qucs-s\crash-reports\`
on Windows) with the version, commit, signal, a backtrace and the last log
lines, and modified documents are autosaved. The next start says so and
offers to restore them. Modified documents are autosaved every two minutes
anyway (`AutosaveInterval` in the settings file, seconds; 0 disables).
Please attach the report when filing a crash.

## Continuous integration and releases

| Workflow | Trigger | What it does |
|---|---|---|
| [CI](.github/workflows/ci.yml) | every push / PR | Linux Debug build with ASan + UBSan, the unit tests, the `load`, `simulate` and `hostile` smoke suites, then 150 fuzzed schematics/datasets and 150 fuzzed simulator outputs; a macOS Release build with the unit tests; a Windows (MSYS2 ucrt64) Release build. Logs, renders and any fuzz findings are uploaded as a workflow artifact (kept 14 days). |
| [Release](.github/workflows/release.yml) | manual (*Actions → Release → Run workflow*) or a `v*` tag | Builds the bundles of the table above on GitHub's runners and publishes them, with checksums, as a GitHub Release. |

The `hostile` suite is the regression guard for the dataset-loader crashes
fixed in WS1.1; it is blocking.

Nothing either workflow builds goes into git: CI keeps logs as short-lived
artifacts, and the Release workflow uploads the bundles as release assets
only. A manual run publishes to the rolling `continuous` pre-release
(replaced each time); a tag push (`git tag -a v26.1.2 && git push origin v26.1.2`)
publishes a permanent release named after the tag, with the annotated tag's
message as its notes and the generated change list below it. All platforms, the ARM
ones included, are built by default; deselect any in the dispatch form, or
set the repository variable `BUILD_ARM=false` to leave ARM out of tag builds.

| Platform | Runner |
|---|---|
| macOS Apple Silicon | `macos-15` |
| macOS Intel | `macos-15-intel`, newest Xcode on the image (its default Apple clang 17 crashes on one source file for x86_64) |
| Linux x86_64 | `ubuntu-22.04` (AppImage runs on anything as new as that) |
| Linux arm64 | `ubuntu-24.04-arm` (Qt's arm64 binaries need glibc 2.38, so the AppImage does too) |
| Windows x64 | `windows-2022`, MSYS2 ucrt64 |
| Windows arm64 | `windows-11-arm`, MSYS2 clangarm64 |

To pull a release's bundles into the git-ignored `bin/<os>/<arch>/`
directories (checksums verified; needs the GitHub CLI):

```bash
scripts/fetch-binaries.sh              # latest "continuous" pre-release
scripts/fetch-binaries.sh v26.1.2      # a tagged release
scripts/fetch-binaries.sh --run 12345  # artifacts of one workflow run
```

## Reporting problems

Open an issue with the platform, the bundle name (or commit), what you did,
and — for a crash — the report from the directory above. Problems that exist
in upstream Qucs-S as well are best reported [there](https://github.com/ra3xdh/qucs_s/issues)
too; this repository tracks upstream and sends fixes back where they apply.

## License

Qucs-S is © its authors and released under the GNU General Public License,
version 2 or (at your option) any later version — see
[`qucs-s-26.1.1/COPYING`](qucs-s-26.1.1/COPYING). The changes in this
repository are released under the GNU General Public License, version 3
([`LICENSE`](LICENSE)), as the upstream "or any later version" terms allow;
the combined work is therefore distributed under GPL-3.0.
