# Qucs-S Enhancements
**Owner: Dr. Meisam Bahadori**

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
This build calls itself **26.1.5** (`qucs-s-26.1.1/VERSION`; the source
directory keeps the name of the upstream version it started from). Documents
it saves say `<Qucs Schematic 26.1.5>`; upstream 26.1.1 asks before opening a
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
- **Verilog-A "Build All" and "Compile"**: right-click the *Verilog-A* row
  in the Content panel to compile every `.va` file of the project with
  OpenVAF (the executable set under *Application Settings → Locations →
  OpenVAF Path*), or a `.va` file's row for *Compile*: that file alone —
  or, with several `.va` files selected, *Compile N Files*. They run one
  after the other, the compiler output in the message dock, brought to
  the front over the simulation console, terminal or Python shell sharing
  the bottom of the window, and a pass/fail tally at the end. Open files
  with unsaved changes are saved first (*Save and Compile*) or compiled
  as last saved (*Compile as Saved*).
- **Verilog-A components keep their parameters' descriptions**: a
  component made from a Verilog-A module (draw its symbol, save it, *Load
  Verilog-A module*) shows each parameter's `desc` as written, with its
  `units` in brackets ("Resistance at the nominal temperature [Ohm]").
  Upstream took every space out of the component file ("Resistance at
  the nominal temperature" became "Resistanceatthenominaltemperature"),
  a quote in any
  description left the component with no parameters at all, a string
  parameter's default came out as garbage, and a symbol in a project
  subfolder was not found. The parameters come from the library OpenVAF
  built (OSDI 0.3 or 0.4, the module of the file's name) while it is as
  new as the source, and from the source itself otherwise.
- **Content panel sees the whole project**: files in subdirectories of the
  project (at any depth; hidden directories, `node_modules`,
  `__pycache__`, `venv` and CMake build trees excluded; at most 20,000
  files and folders looked at, the panel's header saying so when a folder
  holds more) are listed under their category, as sub-trees with one
  folder row per directory (*Sub-trees per folder*, the default) or as
  `sub/dir/name.ext` rows (*Flat*) — right-click the empty area of the
  panel, *Toggle hierarchy search view*, to switch; the choice is
  remembered.
  The folder rows are plain; *Application Settings → Settings → Folder
  icons in the Content panel* puts a folder icon on them.
  Open, copy, rename, delete, drag and subcircuit insertion work on them in
  both listings. A new *Osdi* category lists the compiled `.osdi` models;
  ngspice loads the ones a simulation uses, wherever they are in the
  project (see below), and Build All compiles the `.va` files wherever
  they are. Two more categories sit
  between *SPICE* and *Others*: *Python* (`.py`, `.pyw`; they open in the
  text editor) and *Images* (`.png`, `.jpg`/`.jpeg`, `.svg`, `.gif`,
  `.bmp`, `.tif`, `.webp` and the other formats Qt reads; they open with
  the system's viewer). Files in *Scratch* stay under *Scratch*.
- **Content panel filter**: above the panel, the File Browser's *Filter
  by name* box. It shows only the files whose names hold what is typed,
  or in which it finds a match as a regular expression (`^amp`,
  `\.sch$`, `amp|filter`, `r\d+`), whatever its case - the name as the
  row has it, so a folder's name (`docs`) finds the files in it, or the
  file's own (`^amp` finds `models/amp.sch`) -, in their categories and
  folders, opened; the header says how many it found. Text that is no
  regular expression (`*.sch`) is taken as typed, and a warning in the
  box says why. A row it hides is no longer selected, so no menu or drag
  takes it along. It stays as the panel refreshes; cleared, every row is
  back and the categories are open as they were. The **Projects panel**
  has the same box, under its New, Open and Delete buttons: the projects
  and folders whose names hold what is typed or match it (`..` always,
  to go back up). The project chosen stays chosen while it is listed;
  one it leaves out is no longer the one Open and Delete act on. It
  stays as you go into a folder.
- **Which project is open, at a glance**: in the Projects panel a green
  dot at the right end of the open project's row, a grey one on the
  other projects, none on a folder that is no project; the tooltip says
  which. A linked project shows open whether it was opened through its
  link or by the folder it leads to, and a long name ends before the dot.
- **Long file names cut** (*Application Settings → Appearance → Cut long
  file names after*, 50 characters unless set; *Never* shows them
  whole): in a document's tab and in the Claude Code panel's pin and
  document chip, a file's name longer than that ends in "…", its
  extension always shown whole (`a_very_long_schematic_name_that_goes_on_and_on_pas….sch`).
  Characters are counted as read: an accented letter or an emoji is one,
  and never cut in two. The tab's tooltip gives the whole path; a change
  of the setting retitles the open tabs at once. Claude's tools still
  name each document whole, and take the cut name too. Found on the
  way: an `&` in a file's name was taken for a shortcut's mark on its
  tab ("R&D.sch" showed "RD.sch").
- **Content panel categories are yours to set**: a *Text* category, right
  before *Others*, lists the `.txt` files. *Application Settings →
  Contents* has a row per category, its name and the patterns of the
  files it lists: extensions (`*.txt`, `.txt` or `txt`) or names with
  wildcards (`notes*.md`), separated by commas — `*.txt, *.md, *.log`
  under *Text*, say. A file is listed under the first category from the
  top that matches it (a `.sch` only when it is a schematic); *Others*
  has `*`, whatever no other category took, and without it such files are
  not listed; *Scratch* lists the files of the Scratch folder that match
  its own. *Restore Default Patterns* puts the defaults back, and only
  the categories changed are saved. **Categories of your own**: the tab's
  *Your categories* table adds them (*Add Category*), names them, gives
  them patterns (`*.s2p, *.s4p` for Touchstone files, say), puts them in
  order and removes them; they are listed after *Text*, before *Others*,
  and are kept with the settings (exported and imported with them too).
- **ngspice loads only the Verilog-A models a circuit uses**: the netlist
  loaded (`pre_osdi`) every `.osdi` of the project, whatever the circuit
  used — with a few libraries of compiled models in the project, every
  simulation loaded all of them, and two builds of one module clashed.
  Now the device types of the netlist's `.model` cards — those of the
  schematic, its subcircuits and the libraries and include files they
  bring in, followed into the files those include — are matched against
  the modules each library defines, and only those libraries are
  loaded: one for each module (a library already loaded, else the one
  with the most of what is needed, else the most recently built; a
  netlist comment says which was left out). A library that Qucs-S
  cannot load itself (built for another architecture than the one it
  runs on) is searched for the module's name. The DC bias
  (*Calculate DC bias*) now loads them too; its netlist loaded none, so
  it failed on a circuit with a Verilog-A device.
- **Verilog-A is compiled when a simulation needs it**: before an ngspice
  simulation (or the DC bias) the Verilog-A sources of the project that
  define a module the circuit uses are compiled with OpenVAF when their
  library (`NAME.osdi` beside `NAME.va`) is missing or older than the
  source or a file it `` `include``s — edit a model, press F2, and the
  simulation runs the new one. The compiler's output goes to the
  simulation console; a source that does not compile stops the run with
  OpenVAF's error instead of simulating the old library. Without an
  OpenVAF path in the settings the simulation runs with what there is
  and the status log says which library is out of date. A source open
  with unsaved changes is compiled as saved, and the log says so. *Check
  Schematic* warns about a Verilog-A component whose module is in no
  library and no source of the project.
- **Libraries that bring their Verilog-A**: *Tools → Create Library*
  copies the Verilog-A sources (`.va`) its subcircuits use into the
  library's folder (`user_lib/NAME/`, beside `NAME.lib`), listed with the
  component: those that define the modules their `.model` cards name, and
  the files they `` `include `` (from the project, and from libraries whose
  components the subcircuits use). Compiled models (`.osdi`) are not
  embedded: each runs on one platform only. A circuit that uses the
  library — in any project, or none — has the source compiled with OpenVAF
  beside it before its first simulation, and again when the source is
  newer or the model was built on another platform (a Linux `.osdi` on a
  Mac, an x86-64 one for an Arm ngspice, judged by the ngspice program
  itself), and loads the model. A module the project has only compiled,
  with no source, gets a warning when the library is made. *Application
  Settings → Settings → Embed Verilog-A files in exported libraries* (on
  by default) turns it off: the library then holds the subcircuits and
  their symbols only, as before. Share the library as `NAME.lib` with its
  folder; older versions of Qucs-S read it and ignore the Verilog-A files.
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
  below *Others*. A folder not made for Qucs-S that is opened as a
  project (*Any folder is a project*: a git checkout, a folder of one's
  own) gets nothing written into it: its Scratch folder is in the cache
  directory instead. Headless runs (`-n`, `--run`) keep using the
  simulator work directory from the settings, flat.
- **What an ngspice netlist includes** (*Simulation → Simulators Settings
  → Netlist*): *Include ngspice_mathfunc.inc (limexp, step, stp)*, on by
  default. On, each ngspice netlist begins with an `.INCLUDE` of that
  file of the installation (`share/qucs-s/xspice_cmlib/include`): it
  defines `limexp(x)`, `step(x)` and `stp(x)`, functions of expressions
  written for Qucsator, which ngspice has not. Off, the line is left out:
  the netlist names no file of the installation, and an expression that
  uses them fails under ngspice. The tab shows the file, or that this
  installation has none (the line is then left out either way).
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
- **Text documents' settings files, optional**: saving a text document in
  the built-in editor also writes `name.cfg` beside it (`notes.txt.cfg`)
  with its *File → Document Settings*: the simulation duration, module
  and libraries of a VHDL or Verilog file, the symbol icon, descriptions
  and device type of a Verilog-A file. Other files have no use for it.
  *Application Settings → Settings → Write a settings file (.cfg) beside
  each text document* (on by default, as upstream) turns that off. A
  file is then written only for a document whose Document Settings were
  set or changed, so none is lost, and the files already there are left
  alone. *Default Values* in that dialog now restores the defaults of
  *Flexible wires* and *Embed Verilog-A files* too: it put back their
  stored values.
- **Syntax highlighting in the text editor**: a file is highlighted as the
  language of its suffix — C/C++, JSON, Markdown, Octave/MATLAB, Python,
  Qucs netlists, shell scripts, SPICE (`.cir`, `.sp`, `.lib`, `.inc`,
  `.mod`, ...), Verilog and SystemVerilog, Verilog-A, VHDL and XML (the
  first four were all it had, Octave with the wrong comment mark).
  Keywords, types, built-ins, numbers, directives (SPICE's dot commands,
  the preprocessor, decorators), strings and comments, those over several
  lines too (`/* */`, `"""`, `<!-- -->`, fenced code), and a `#` in a
  string is not a comment. *Comment/Uncomment* uses each language's mark:
  it comments the lines selected — or, when every one is a comment
  already, uncomments them — and is one step to undo. A text file is read
  as its bytes are (UTF-8, UTF-16 or UTF-32 by its byte order mark, else
  Windows-1252, as vendor model libraries often are) and saved the same
  way, its line ends too; a character its encoding has no bytes for is
  asked about (the file is then saved as UTF-8). Undo right after opening
  leaves the file as it was.
  A button in the status bar names the language of the text document in
  front; its menu chooses another, kept for every file of that suffix
  (open, opened later, after a restart) until *Back to the Default*. A
  document without a suffix keeps its choice for itself. *Application
  Settings → Source Code Editor* lists the languages with a radio button
  each; the one chosen shows its styles — a colour, bold and italic each,
  under the language's own names ("Dot command", "Decorator") — on a
  preview of the language. OK or Apply saves the changes of every
  language, and the open documents take them at once; only the styles
  changed are saved. (The tab's nine colour buttons, which nothing ever
  used, are gone.)
- **Embedded simulation console**: a simulation runs in a *Simulation*
  dock at the bottom of the window (tabbed with the build messages) instead
  of the modal "Simulate with external simulator" dialog, so the schematic
  stays usable while ngspice/Xyce work. The dock has the simulator's
  output, a status list, a progress bar, *Stop*, *Save netlist* and
  *Clear* (*Stop* and *Clear* are also *Simulation → Stop Simulation* and
  *Clear Simulation Console*); it comes up with the first simulation and
  can be shown or hidden from *View → Simulation Console*. A second Simulate while one is running
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
- **Claude Code dock** (*View → Claude Code*, or the *Claude* chip in the
  status bar): a conversation with [Claude Code](https://claude.com/claude-code)
  beside the schematic. Claude works in the workspace folder (*Application
  Settings → Locations*) unless another is chosen in the dock, and the
  document in front goes along with a prompt when its chip is on. The reply
  is drawn as it is written (Markdown, code on a shade, TeX math between
  `$...$` and `$$...$$` typeset: fractions, roots, sums and integrals with
  their limits, matrices, cases, aligned equations, Greek, units), the
  tools Claude uses in a row fold into one line ("Ran 3 commands, read
  amp.sch") that opens on a click - each tool on its whole command and
  what it gave - and each turn ends with its time and, when chosen, what
  it took: the tokens of the prompt and of the conversation so far (a
  tooltip tells input, output and the cache read and written) and their
  cost (*⋯ → Show Usage*: each of the four on or off, for every
  conversation, its exports and `/status`; all off by default).
  *New* (or *+*
  by the tabs) opens another conversation in a tab of its own, with its
  own Claude Code session; a tab shows what its conversation is about and
  how it stands, and closing it ends that session. *Rename…* in a tab's
  right-click menu (or a double click on it) names the conversation in
  place - the name its tab, the status bar and an export use - and
  *Reset Name* gives it back its first prompt. A conversation can be
  **pinned to a schematic** - your choice, off by default: the pin
  beside the document chip pins the schematic in front, *⋯ → Pin to a
  Schematic* any open one. Pinned, the conversation's prompts name that
  schematic (not the tab in front), Claude's Qucs-S tools act on it when
  given no document - so two conversations can work on two schematics
  side by side, whichever tab is in front - and a menu action Claude uses
  brings it to the front first. The chip turns into a filled pin with
  the schematic's name (a click unpins it), the tab's tool tip says it,
  and a *Save As* of the schematic keeps it pinned. *⋯ → Export
  Conversation* saves the whole conversation - every prompt, reply and
  tool with its input and output - as a **PDF** (drawn as in the dock,
  on paper, math typeset), **Markdown** (the replies as Claude wrote
  them) or **plain text**. *Include Tool Details* in the same menu (on
  by default, and remembered) decides whether the contents of the boxes
  that fold go along: off, a row of tools is the one line that sums it
  up ("✓ Ran 3 commands, read amp.sch"), as the dock shows it folded, and
  the export is the chat itself. What Claude may do is chosen in the dock's menu: *Ask Before
  Acting* (a card asks before a command runs or a file changes — *Allow*,
  *Allow All Edits* for the rest of that conversation alone, *Deny*), *Accept
  Edits*, *Auto* (Claude acts without asking and a safety check stops
  risky actions; not every model has it, and the menu says which do not),
  *Plan Only* or *Bypass Permissions*. So is the model: the models the
  installed `claude` offers, by what they are (*Fable 5.1*, *Opus 5 with
  1M context*), the newest of each family it does not offer yet (*Opus
  5.5* — which needs a recent Claude Code), or any other by its name. The
  header shows the model and the mode. A schematic, text, spreadsheet,
  PDF or ZIP archive Claude changed that is open without unsaved changes
  is loaded again (a spreadsheet keeps its sheet and cell); when it is
  not, as it has unsaved changes, Claude's next Qucs-S tool result says
  so, whatever changed the file (Write, Edit, Bash); Save asks
  before writing over a file that another program changed since it was
  loaded. The
  status bar chip says what Claude is doing — thinking, the tool it runs,
  *needs you* when it waits for an answer, and how many other
  conversations are at work — shows or hides the dock, and leads to a
  conversation that waits behind another.
  **Claude drives the window**: Qucs-S offers Claude tools of its own
  (an MCP server inside Qucs-S, served over the same stream as the
  conversation; Claude Code is started with
  `CLAUDE_CODE_MAX_MCP_DESCRIPTION_LENGTH` as long as the server's
  instructions, so that none of them is cut - unless you set it
  yourself), so that it can open, show, save and close documents,
  read the schematic in front (its parts, their pins' places, or its
  `.sch` text) and change it as you watch - place components, set their
  properties, move, turn and rename them (their wires follow, the
  circuit kept as it was), draw wires pin to pin (routed around what
  they must not touch: a wire never joins a net it was not meant to), label
  nets, delete, replace whole sections from text, undo and redo, each
  change one step of Edit > Undo - take a screenshot of it to see what it
  did, use any menu action and fill in and answer the dialog it opens (a
  file dialog too: one Claude opens is Qt's, which it fills in with a
  path, where the system's panel would wait for you; those you open stay
  the system's), and run a simulation and read its outcome. The rest of
  the window is read and used the same way (`get_ui`, `set_ui`): a dock
  or one of its panels - Content, Projects, Components, Problems, the
  Tuner, the simulation console's log -, a toolbar, the status bar, the
  documents' tabs - their fields, buttons, sliders and lists, and the
  views of files and parts row by row (selected, opened with a double
  click, a tree's folders opened). `context_menu` opens the right-click
  menu you would get on a part, a diagram, a file, a project or a tab,
  lists it and chooses from it (a row a panel's filter hides is said to
  be). The Claude Code panel - its prompts, permissions and settings, and
  its chip in the status bar - stays yours, and Claude reads the consoles
  but types into them only with the `console` tool: one line into the
  Octave, Python Shell or Terminal dock, shown there as you would type
  it, you asked about each line - also where Claude acts on its own (it
  runs with your rights, outside Claude Code's rules for commands) -, and what it
  printed (no banner, no echo of the line, no prompt) until its prompt
  came back - or "still running", stopped with
  Ctrl-C when you allow that too. `get_settings` and `set_settings` read
  and set Application Settings, Simulators Settings, CDL Settings and a
  document's own by typed keys ("Tab/Label": a text, a choice and its
  choices, a number and its range): set through the dialog's own OK, so
  what the window does after it is done, each change told with what it
  was (set back with that) and what it is now; Claude Code's own
  settings are refused. **It reads the results as
  numbers, not only as pictures**: `get_dataset` gives a dataset's
  variables and, for those asked for, their statistics (min, max and
  where, mean, RMS), samples over a range, values at given times or
  frequencies, and measurements on the full data — rise and fall time,
  overshoot, settling time, period, frequency, duty cycle, crossings of a
  level, −3 dB bandwidth — each curve of a parameter sweep on its own.
  `simulate` says it succeeded only when the simulator ran to its end,
  reported no error and exited with 0 (it looks for `name.dat.ngspice`,
  `.dat.xyce`, … — no longer for Qucsator's `name.dat` whatever the
  simulator); its errors and warnings come each with the netlist line
  they name, the part of the schematic (a device inside a subcircuit is
  its subcircuit's part) and the node, and it says which traces of the
  diagrams show no data and why; with `keep_as` it keeps a copy of the
  run's dataset under a name, to read later or to plot beside the next
  run (`ngspice/run1:tran.v(out)`). **A long run in the background**:
  `simulate` with `background` answers at once with the run's id, while
  Claude goes on - a Monte Carlo, a long transient; `simulation_status`
  says how it goes and gives its outcome once it has ended (one JSON, a
  stopped run told as stopped, not crashed), `stop_simulation` stops it
  (or the run going, the user's too), and a run past its `timeout` goes
  on the same way. A schematic `simulate` names that is not open is
  opened first, and said. **Waiting rather than
  asking again and again**: `wait_for` returns when a run has ended (its
  outcome), a dialog has come up, a document has been changed (by whom)
  or a file written - and what the user edits while Claude waits is the
  user's. `get_netlist` gives the netlist a
  simulation would run, or the last one run; when the netlister gives up
  (a part with no model, a subcircuit or library it cannot read) it says
  why - it gave a title line alone, and the next netlist then left the
  part out without a word. **Diagrams by name**:
  `add_diagram`, `edit_diagram`, `add_trace`, `edit_trace` (and `delete`
  for diagrams and traces) take named fields — type, place and size,
  each axis's label, log scale, limits and step, grid, legend, theme
  (the colours of its parts: a preset — light, dark, no_background — and
  each part's); each
  trace's variable (`v(out)` or `out` is found in the dataset and given
  its simulator's and analysis's name), color, thickness, style, axis,
  markers — instead of the positional fields of a `<Rect …>` line, and
  every trace reads its data at once; replacing the `<Diagrams>` section
  as text now does too, and `reload_data` (or *Simulation > Reload
  Simulation Data*) reads the datasets again. `rename_net` renames a net
  together with the traces, data displays and equations that name its
  voltage; taking a label away warns of the traces it leaves without a
  net. `describe_component_type` tells a type's properties in their
  order with defaults, units and meaning, the netlist line it makes, and
  the traps (a `Vpulse` is one pulse under SPICE; `Vrect` repeats).
  **Results it can trust**: a curve written as complex numbers with no
  imaginary part — a Nutmeg equation's `db(...)` — is read as real, its
  sign kept (as a magnitude, −52.8 dB rose to +52.8 dB and its bandwidth
  was 27 times too wide); each variable says its units (dB, V, A,
  degrees — from its name, or the equation that defines it, which it
  quotes), and `bandwidth` is 3 dB below the peak of a curve in dB, the
  peak over √2 of a magnitude, and refused on a signed curve that is
  neither. A DC simulation's **operating point** is read as one: each
  node's value and, with ngspice, each device's quantities — id, gm,
  vgs, … — under its component (T1 for ngspice's `jt1`), written into the
  dataset by the run itself; a DC bias run's files are removed before
  every run, so none is read a day later. **What changed under it**:
  every tool's result says what changed since Claude's last call that it
  did not change — the user's edits, another conversation's, a
  simulation the user ran, documents opened or closed — and `get_state`
  gives each document's revision (it counts every edit, undo and reload)
  and who made the last edit, and names the open project. **Markers**: `add_marker` places one at an
  x, the peak, 3 dB below it or a crossing of a level (the exact point
  found, the marker on the nearest sample), with its label, precision,
  format, indicator and colours; `edit_marker` and `delete_marker`; and
  `get_schematic` lists them. `get_schematic` lists the properties not at
  their type's default (or those shown, or all), the parts named or in a
  region, 200 at most with what is left out, the paintings and the
  document's settings; `set_schematic` says what it read of each section
  it replaced, and `describe_format` explains every field of a `.sch`
  line. `export_netlist` writes a SPICE or CDL netlist to a file (the
  menu's dialogs cannot be answered); `get_state` lists the panes and
  `move_to_pane` puts a document beside another; `screenshot` takes the
  document on white paper (`paper`), the canvas as the user sees it in
  the theme (`screen`), or the whole window with each dialog over it.
  **Drawing on it**: `add_painting` and `edit_painting` draw and change
  texts, arrows, lines, boxes, text boxes (a callout pointing at what it
  is about), tables, dimensions and TeX formulas by their named fields,
  and a subcircuit's symbol with the same tools (the document is switched
  to show its symbol, as *Edit Circuit Symbol* does). Each change is one
  step to undo. `get_schematic` lists every painting by its fields, and
  `delete` and `select` take paintings by number. **The measurements an
  amplifier session is after**: `thd` on a transient, which gives each
  harmonic's amplitude over the last whole periods of a stated
  fundamental, as ngspice's `.four` does; and on a loop gain, `gain`,
  `phase_margin` and `gain_margin`. `list_documents` lists the files of
  the open project (else the workspace) or a folder without a shell:
  kinds, sizes, times, newest first, and which dataset is which
  schematic's. `get_schematic`'s
  `overview` tells a 24,000-part schematic in 721 bytes. `export_image`
  writes a picture of a schematic, or of one diagram alone, as PNG, SVG,
  PDF and more, without the export dialog. `set_simulator` chooses the
  simulator, so two engines compare in four calls. An `atomic` batch is
  all or nothing, and one that stops half-way says how many changes stay
  (`undo` takes `steps`). `simulate` says when the schematic was changed
  while it ran.
  **Wiring Claude can trust**: `check_schematic` checks the circuit, and
  the summary counts the problems it finds. It reports pins or wire ends
  on another net's wire that are not joined, parts on no ground, nets with
  no DC path, and crossings without a junction; *Check Schematic* in the
  menu gains the same checks. A wire drawn across another net's wire, or
  a part placed on one, says so at once. `simulate` with `operating_point`
  gives a transient-only schematic's operating point in one call: node
  voltages, and each transistor's gm, ic and more, with re, rπ, β and ro
  worked out. `edit_component` hides or shows a part's properties and
  moves its text, with no need to rewrite its line. `move` moves a group
  with its wiring. `create_subcircuit` turns parts into a subcircuit, and
  a subcircuit's parameters are read and set on its symbol. `add_analysis`
  adds an AC, transient, DC or sweep analysis with its plot. `get_dataset`
  reads expressions such as `v(out)/v(in)` and compares a run with a kept
  one. `set_schematic` refuses a component line with a value too many,
  which would have shifted every property after it.
  **Design tools**: `tune` sets a part's value, simulates and measures,
  over and over, until a number comes out right: sweep RE until the
  emitter sits at 5 V, or C until the peaking is 1 dB. It takes a few runs
  in one call, and the value found is one step to undo - set only when
  it gives the target; a value with no unit (`330`) stays one. The search
  draws its secant on the value or its logarithm, and the measurement or
  its logarithm, whichever foretells the runs best (a divider's voltage
  over its source, or a bandwidth over a capacitance: three runs), and a
  value of a list given is set as it is written (`3 kOhm`). `build_verilog_a`
  compiles a `.va` now, with each error's line and column.
  `describe_component_type` gives a Verilog-A module's parameters and a
  `.model` card. `find_library_component` finds a part by its values (an
  NPN with Bf near 200) in the libraries and the project's SPICE models,
  and `read_pdf` reads a datasheet's text. `import_netlist` makes a
  schematic of a SPICE netlist, and `make_symbol` draws a subcircuit's
  symbol with its pins on four sides. `ngspice_commands` tells Claude
  which commands ngspice has: all 165, each in a line by category (the
  analyses, the RF set, measurements, output, statistics and optimization,
  breakpoints, the `.control` language), with how Qucs-S writes them (a
  Nutmeg script, a NutmegEq, the simulation blocks). One command in full
  gives its syntax, an example that runs and ngspice's own help line; a
  search finds commands by what they do (stability, Touchstone). It asks
  the ngspice of the settings for its own list, so the answer marks what
  that ngspice lacks - a stock one has none of the enhanced build's
  commands - and names any it has besides. `new_project`, `open_project`,
  `copy_document` (a schematic with its datasets and data display),
  `clean_scratch`, `rename_file` (a file or folder renamed or moved, the
  documents open from it following) and `trash_file` (to the system's
  trash, the documents open from it closing; asked about every time) tend
  the files; `undo` with `files` renames back, and takes back from the
  trash. `get_text` and `edit_text` read and edit a text tab - a
  netlist, a `.va`, a script - as it is in the window, the user's unsaved
  edits kept: the edits are one step of the tab's undo, and refused when
  the user typed since the revision Claude read. `goto_line` shows a line.
  `build_verilog_a` marks its errors and warnings in the open `.va` tab as
  the editor shows them to you: a wavy line, a dot in the line numbers'
  margin, the message on the line. `read_help` finds what this build's
  help says on a topic - each menu action's own help, the component
  types, the examples - and where the online manual is. **The last
  resort**: `send_input` clicks, drags, double-clicks and types on the
  canvas (in the schematic's coordinates) or a panel (in its pixels, which
  `get_ui` gives each control) as your mouse and keyboard would - a key
  that is a shortcut sets off its action -, with a picture after; asked
  about each time, and never on a console, on the Claude Code panel, or
  with a key that would quit Qucs-S. `new_document` opens a schematic's data
  display for a report's plots. Equation blocks, Monte Carlo records and
  specs, and hidden text are set by name, so `set_schematic` is rarely
  needed. Diagrams have a title that moves with them. A marker can sit
  3 dB below 0 dB or the DC value, not only below the peak. `get_dataset`
  measures a spectrum (`fft`), an eye (`eye`: the bit period, when not
  given, the Tbit of the V(PRBS) source the signal comes from - as the run
  the data is of gave it - or told from its crossings; PAM4's three eyes
  with `levels` 4, or without `levels` when the source is coded PAM4) and a Monte
  Carlo family's `distribution`, gives a table across a sweep, and reads `.csv` and
  `.xlsx` results, and says when a dataset is stale: the last run failed
  after it, or the circuit changed since (the netlist a run would be
  given now is not the one it ran; where that netlist is not at hand - a
  copy not simulated yet - an edit after it, and that no more can be
  told). A preview is no edit. A ring's frequency is measured at the
  value it settles at. `simulate` runs Check Schematic first - what it
  finds comes first in the answer, and a failed run's errors begin with
  a part's pin connected to nothing - and takes a simulator for one run;
  a Qucsator run is waited for too. A simulation
  writes its dataset where the schematic's `DataSet` says, where the
  diagrams read it - not after the file's name, which differs in a copy
  made outside Qucs-S; `open_document` points such a copy out, and
  `own_data_names` names its dataset and data display after it. What the user
  changes between Claude's calls is told part by part (R2: R 47k → 67k),
  and `undo_history` tells the steps to undo in words. `"selection": true`
  takes what the user selected. A document is found by its file's name
  alone, and relative paths are taken from the open project. A
  schematic, text, spreadsheet or archive changed on disk by another
  program is loaded again - also when it changed just after it was saved
  or opened - and a PDF follows its file even through a delete and a new
  version later. Claude is told such a change as its file's, not as the
  user's, and one its own Write or Edit made as its own.
  `replace_component` puts a part of another type in one's place - a
  built-in OpAmp for a subcircuit - its pins mapped by number or name to
  the old pins' nets, turned, mirrored and placed so no wire runs across
  it, in one step to undo. An `.OPTIONS` block takes flags (`noinit`). A
  symbol's port can show its instances a label beside the pin instead of
  the net's name (`+`, `-`, or nothing; also by a double-click in the
  symbol editor). The schematic's tools switch a document back from its
  symbol themselves. `describe_component_type "Verilog-A"` gives a module
  that OpenVAF compiles as it is, with the rules it wants, and for any
  type the hidden properties that still go into the netlist (an OpAmp's
  `Umax`). `save_document` of a subcircuit says which open schematics'
  instances took its new symbol and which pins moved off their wires.
  `get_schematic` gives the box of each shown text of a part, and
  `open_document` says when a line has more values than its type.
  `set_schematic` also takes parts and wires as JSON - each property by
  its name, checked against the type, so a value too many cannot shift
  the rest - and `get_schematic` gives that form (`"format": "json"`).
  A value that does not fit its property - a word where a number
  belongs that no equation block defines, a word that is not one of the
  property's choices (`european`, `US`) - is told, for `.sch` text too,
  where a value left out or two swapped once went unnoticed. A number
  mistyped - digits with letters that are no scale and unit, `1kk`,
  `10uu` - is refused, where SPICE would have read `1k`; so are a
  type's traps (a `Vpulse` is one pulse, an `OpAmp` clips at `Umax`) as
  the part is placed or edited. An equation block's answer lists its
  equations as they are after the change. `arrange` lays a whole
  schematic out again for a person to read: parts in columns by signal
  flow, two-pin parts turned as schematics show them (in series lying
  down, to ground standing up), a ground symbol back on each piece that
  had one, every wire drawn again by the router - every net compared
  before and after, one step to undo. It takes about a third of a second
  for a 2,000-part RLC ladder, and with `wire_labels` it draws wires where
  only labels joined a net (an imported netlist's labels on every pin).
  A tool refuses an argument it does not take, and names the one meant
  (`rotaton`: "Meant rotation?"), instead of leaving it out - inside an
  argument too (a `set_schematic` part's, a wire's, a trace's); a script
  in CI (`scripts/ci/check-tool-arguments.py`) checks that every argument
  a tool reads is in its schema, and the other way round. A name that is
  no file's (`"`, `{}`, `NUL`, `.`) is refused by the tools that write
  one. Saving a subcircuit whose new symbol moves an instance's pins
  joins each pin to its net again - its label goes along, a wire that
  ended on it is drawn on to it, a port numbered anew is followed by its
  name - and compares the parent's nets before and after. Check Schematic
  says when a library part or a subcircuit could not be loaded (a box
  without pins), not only that the wires to it end on nothing.
  `create_subcircuit` of parts in a row (a divider's R1 and R2 on one
  wire, a capacitor outside on the wire between them) keeps every net:
  that wire goes with the group, each port's net is joined by a label,
  the instance goes where none of its pins meets another net, and the
  parent's nets are compared after - the change undone if one differs.
  For drawings a person reads: `add_component` and `edit_component` put
  a part beside another (`near`: Rf below U1, 80 apart - slid along
  that side off whatever is there, and the answer says where it went),
  `connect` goes round a side or through points given (`side`, `via`),
  `set_label` puts the label's text where it is asked (`text_at`), and
  `arrange` draws only the wiring again with the parts where they are
  (`keep_places`), puts an op-amp's feedback part below or above it
  with its pin under the output and the part from its input to ground
  (Rg) standing under it (`feedback`), and joins the supplies by
  labels, VCC up and VEE down (`supplies`). Wires go round every part's
  symbol - a ground's too - leaving a pin straight out of its own
  symbol, and where none of the usual shapes is clear, a way is found a
  grid step at a time; a label goes on the longest stretch of its net
  where its text has room; `straighten` nudges a part up to four grid
  steps, clear of the others, so the two pins of a wire between two
  parts line up and it runs straight - only pins that face each other,
  each leaving its symbol toward the other as its stub goes. A library part's pins have
  the names its model gives them (an op-amp's INN, INP, OUT, VCC, VEE;
  an LED's C and A; the transistor-level uA741's, AD825's and LM3886's
  too): `connect` takes `U1.inp`, `replace_component` maps them by name
  - and refuses to map named pins by number to a part without those
  names, listing both parts' pins and the side each is on, until
  `pins` says which is which - and the netlist map shows them beside
  the numbers. A
  library part that is one component with the library's values (a
  varactor's diode, a transistor's model) is placed as that component,
  as the library panel does, and `find_library_component` says what it
  is placed as. `batch` and `simulate` answer briefly when
  asked (`brief`: a line a call; no ngspice log), `simulate` sets this
  run's measurements beside a kept run's with the change (`compare`),
  `tune` sets two to four parts together for as many targets (`knobs`,
  `targets`: Rf and Rg for a gain and an input resistance), keeps other
  measurements within bounds while it moves the values (`hold`: a gain
  of 11 while the bandwidth stays above 50 kHz - a value that breaks one
  is told, not set) and gives every measurement before and after
  (`compare`), and `undo`
  takes back the files the last calls wrote (`files`: a subcircuit
  made, a file written over), not only the edits. Check Schematic warns
  of a part whose supply pins nothing powers, and of a supply the wrong
  way round (a VCC pin below ground, a VEE pin above it, with the value
  that turns it); a DC source below ground on a net nothing names as a
  negative supply is a note. `find_library_component` says how each part fared
  when every part of the libraries was run under ngspice - placed alone,
  each pin to ground through 1 MΩ, its operating point - and `tested`
  lists only the parts that passed; `scripts/ci/test-library-parts.py`
  makes that run, each night in CI (`library.yml`), and fails when a
  part that passed fails. An op-amp, a transistor, a FET or a diode
  that passes is then put in a bench of its kind - a follower and a gain
  of 11 on ±15 V, two bias points, |Vgs| 10 V, about 1 mA forward - and
  its numbers checked against a range: a model that runs and does the
  wrong thing (a diode's Is written 1.3 A) fails, and is not `tested`.
  `describe_part` gives a library part in one call: its pins with their
  names, sides and roles (input, output, supply), what its model is
  (one component, a macromodel, transistor level) and how it fared, each
  bench number with its unit and what it is to be (5.504 V, expected
  5.5 V). `create_subcircuit` names a port after the pin it came from (a
  741's INN, VCC), and `make_symbol` sides ports by their names (inputs
  left, outputs right), so `arrange` finds the output of a subcircuit's
  box by name. The server's instructions end with the way of working
  that eight rounds of review arrived at (describe the part, wire it by
  name, arrange, check with its subcircuits, judge by numbers, `tune`
  with `hold`); `scripts/mcp-e2e-scenarios.py` runs eight end-to-end
  scenarios through `--mcp-server`, the 741 bench among them. Library models keep a value's unit when it is
  written with a space (`C="30 pF"`, once 30 farads), and a library
  `Idc` flows as a schematic's does (a 741 macromodel's output sat at
  the negative rail). `connect` takes `"ground"` at one
  end: a ground symbol of the pin's own. A ground has no name: every
  tool that takes a part's name takes its ref from `get_schematic`
  (`GND#2`) - `move`, `delete`, `select`, `edit_component` - and a read of
  some parts names the nets as a read of all does. The tools that change
  a schematic take `"preview": true`: what the change would do, and
  nothing done (a `batch` too) - no file either: `create_subcircuit`'s is
  put back. `add_analysis`, `add_diagram` and `add_trace` plot an
  expression (`db(v(out))`) through a NutmegEq beside the analysis, and
  an expression that does not read is refused before anything is added.
  `simulate` and `tune` save an
  untitled schematic in the scratch folder first, and a new document
  replaces the untitled one nothing was done in. Every field of every
  tool says what it is in the schema, and every tool takes `max_chars`:
  a longer answer comes back with its biggest lists and texts halved
  ("… 40 more") and a `trimmed` field that says what was cut. A `batch`, a script, `tune` and a
  preview run alone: a call that comes meanwhile - sent before the last
  was answered, or another conversation's - waits its turn, so no
  rollback takes it away, and a batch's calls stay with its document. No
  message box holds a tool call up (under `--mcp-server` no one could
  answer it): it is closed with its safe button, and what it said goes
  into the answer. The JSON form sets back every shipped example as it
  was, netlist line for netlist line - parts mirrored and turned, sweeps'
  records, subcircuit parameters, library parts, a label's `.IC`. Net
  and part names are checked as the dialogs check them (no net called
  `gnd` or `net2`), and `export_netlist` does not write over a document. `diff` compares a schematic with its
  file as saved, with a step of its history or with another file.
  `get_netlist` with `"map": true` ties each netlist line to its part and
  each node to its pins, and lists the parts whose pins have no names,
  each pin with the side it is on. `import_netlist` takes a `title`. `run_script` runs a short JavaScript program
  that calls the tools (`qucs.call("add_component", {...})`) with loops
  and conditions between the calls, in one turn (its result the last
  expression, or a `return`) - with `"atomic": true` every schematic is
  put back when it fails. The schematic's text, its
  netlist, the netlist map, the dataset, the window's state and the
  summary of ngspice's commands are also MCP resources, read and subscribed to (told when they change, also by
  another program writing the file). Before a file is written over or
  unsaved changes are discarded, Qucs-S asks you in the dock, with a
  button for each answer. Tools that only look are
  used without asking; the first change asks, and *Allow Qucs-S Control*
  lets the rest of the conversation go on without asking (as do the
  *Accept Edits*, *Auto* and *Bypass* modes). What cannot be undone -
  a file deleted or written over, unsaved changes discarded - is asked
  about every time, but in *Auto* and *Bypass*. Each tool carries MCP's
  annotations (read-only, destructive, idempotent). The system's own file and
  print dialogs are not driven: Claude opens and saves by file name.
  - **Quick**: the 22 tools most sessions use are loaded in every turn,
    with a summary each (about 5.6k tokens with their schemas);
    `describe_tool` gives a tool's full description. The others are
    found by Claude Code's tool search when a task needs them and load
    with their full description. The descriptions are written in plain
    English. Answers are compact JSON, and structured
    (`structuredContent`); `batch` runs many
    calls in one - place and wire a circuit, set properties, add a
    diagram and its traces - each change still one step to undo; a menu
    action answers as soon as it is over (not half a second later).
    While a dialog waits for an answer (the user's, or one Claude
    opened), the tools that change a document wait too - the dialog
    holds on to what it edits - and so does loading a file Claude
    changed.
  - **Without the window**: `qucs-s --mcp-server` serves the same tools
    over stdio, with no window on screen, for any MCP client - Claude
    Code in a terminal, a batch job, a CI check of schematics, or
    subagents each working on a copy:

    ```bash
    claude mcp add qucs -- qucs-s --mcp-server
    ```

    (From the macOS disk image, the program is
    `/Applications/qucs-s.app/Contents/MacOS/qucs-s`.) Schematics named
    after the option are opened. It ends when its
    input does; unsaved changes are left unsaved (it says so on stderr).
    A dialog that opens is read with `get_dialog` and answered with
    `set_dialog`. Questions go to the client's user when the client can
    ask them (MCP elicitation); otherwise writing over a file needs
    `"replace": true`. The tools read and write wherever the user who
    runs it can - a netlist exported to `/tmp`, the files of any folder
    listed - as a desktop program does: give `--mcp-server` only to
    clients you would let use your files. `--workspace DIR` gives the run
    a workspace of its own (relative paths, new projects and the scratch
    folder go there) instead of the settings' - nor is it saved as
    theirs.

    Claude Code keeps 2,048 characters of a server's instructions unless
    `CLAUDE_CODE_MAX_MCP_DESCRIPTION_LENGTH` says more (from Claude Code
    2.1.280), and Qucs-S's run to about 6,800: the part it keeps says
    that the resource `qucs://instructions` holds them whole. To give
    Claude all of them at once, set it in the environment Claude Code
    starts in (the dock does so itself):

    ```bash
    export CLAUDE_CODE_MAX_MCP_DESCRIPTION_LENGTH=8000
    ```
- **Conversations kept, and gone on with**: every conversation is kept
  as it goes; those open when Qucs-S closes come back when it opens
  again - what was said, their names, folders and pinned schematics -
  and the next prompt continues the same Claude Code session
  (`--resume`; if Claude Code no longer has it, Claude begins afresh and
  the dock says so). *⋯ → Reopen Conversations at Start* turns this off.
  `/resume` (or *⋯ → Resume a Conversation…*) lists the kept ones and
  Claude Code's own sessions of the folder - from a terminal, say - to
  search and go on with (brought back from Claude Code's session file);
  `/resume <session id>` goes on with one at once.
- **Git status above the prompt**, as in the Claude desktop app. When the
  open project (or the folder chosen for Claude, else the workspace) is
  in a git repository, a bar above the prompt shows:
  - the repository's folder and the branch (↑2 ↓1 for commits to push
    and to pull);
  - the lines changed, e.g. **+2,096 −3**, counted from where the branch
    left the remote's default branch, committed or not, new files
    included (on the default branch itself, from its upstream). Click
    the numbers to see the changes file by file, with coloured diffs;
    double-click a file to open it;
  - **Create PR**, which asks Claude to commit, push the branch (on a new
    one if you are on the default branch) and open the pull request with
    `gh`. Once the branch has an open pull request (asked of `gh`, when
    installed), the button shows **PR #12** and opens it. The menu next
    to it has *Create Draft Pull Request*, *Update Pull Request*, *Commit
    Changes*, *Push Branch*, *Show Changes…*, *Refresh* and *Copy Branch
    Name*. What changes files or pushes goes to Claude as a prompt in the
    conversation, with the permissions it has.
  - **✕** hides the bar until the repository or branch changes.
  The bar refreshes when the folder changes, after each turn, when
  Qucs-S comes to the front, and every 10 seconds while it is shown.
  git runs in the background, never blocking the window, and never runs
  what a repository's own configuration names (a file system monitor,
  filters, text conversions): a project downloaded with its `.git` runs
  nothing by being opened. *⋯ → Show Git
  Status* turns the bar off (or on) for every conversation; it is on by
  default.
- **Slash commands** in the prompt: typing `/` lists them (↑↓ to choose,
  Tab to complete, Enter to run). The dock's own: `/clear` (`/new`),
  `/resume`, `/quit` (`/exit`), `/help`, `/model [name]`,
  `/permissions [ask|edits|auto|plan|bypass]`, `/rename <name>`,
  `/export [pdf|md|txt]`, `/status`, `/pin`, `/unpin`; Claude Code's -
  `/compact`, `/context`, `/cost`, `/init`, `/review`, your skills… as it
  lists them - are sent to it as typed, without the note of the open
  document that would have become their arguments.
  It needs the `claude` program installed and signed in once in a terminal;
  it is found on `PATH` or where its installers put it, or chosen under
  the dock's *⋯* menu. It runs `claude -p` with stream-json both ways, so
  the conversation carries on across prompts and after a restart
  (`--resume`).
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
  the project's files — on another thread, the window never waiting — and,
  only when one came, went or changed — saved by
  Qucs, written by a script in the Terminal dock, copied in by hand —
  lists them again (never while a simulation is writing its scratch
  files, under an open menu, or during a drag). *Application Settings →
  Settings* has the interval (default 3 s) and a checkbox to turn it off;
  *Refresh* on the right-click menu of the panel's empty area does it on
  the spot.
- **File Browser** (a tab of the left panel): the file system, from the
  workspace, in the view you choose — *Tree* (folders open in place),
  *List* (a double-click enters a folder), *Icons*, *Details* (size, kind
  and date, sorted by any), *Columns* (a column for each folder and a
  preview of the file selected, as Finder's) or *Recent Documents*. Every
  file has an icon for its kind — schematics, symbols, data displays,
  datasets, netlists and libraries, Verilog-A and HDL sources, compiled
  models, S-parameters, scripts, text, images, archives — its tag and
  colour on a page, drawn for the light or dark theme, and folders come
  first, in natural order (R2 before R10). Back, forward, up, and places
  (the workspace, the open project, the examples, home, the volumes); the
  path as buttons, or typed (Ctrl+Shift+G); a filter by name (text or a
  regular expression, as the Content panel's), and *Show Only Qucs-S
  Files*. A double-click opens a file as the Content panel does; its menu
  opens it with the system, shows it in the file manager, copies its
  path, renames it in place, makes a folder or a new ZIP archive (*New
  Zip…*, below) or moves it to the trash (documents open from it close;
  not while one has unsaved changes). The folder, the view and the
  options are kept for the next start.
  **Drag and drop**: select one or several entries (⌘/Ctrl-click,
  Shift-click) and drag them.
  - **Where they go:** onto a folder's row, into that folder. Beside the
    rows, into the folder shown. Onto a button of the path, into that
    parent. A column of the Columns view takes the folder it shows.
  - **Move or copy:** within one disk they are moved. With Option (Ctrl on
    Windows and Linux) they are copied, and they are also copied to
    another disk. Command (Shift) forces a move.
  - **From other programs:** files dragged in from the Finder or the
    Explorer are copied in.
  - **To the document area:** entries dragged there open.
  - **While dragging:** the target folder is highlighted. Holding the drag
    over a folder for a moment opens it, and near an edge the view
    scrolls.
  - **Same name:** if the folder already has an item with that name, you
    choose *Replace* (the old one goes to the trash), *Keep Both*
    (`name 2`), *Skip* or *Stop*, and you can apply the choice to the
    rest. An item that holds the one being moved is never replaced. A
    copy into its own folder is named `name copy`.
  - **Not allowed:** moving a folder into itself, or moving the workspace
    or the open project.
  - **Open documents:** documents open from moved or renamed files or
    folders follow them; their tabs and names update.
- **PDF viewer**: a PDF document — a datasheet, an application note, a
  report written by a script — opens in a tab of its own, from the File
  Browser, the Content panel, *File → Open*, a drop, or Claude's
  `open_document`. Its pages one under the other, drawn sharp at any zoom
  (in tiles, only those in sight); the toolbar has the page (typed, or
  with the arrows), zoom in and out, *Fit Width*, *Fit Page* and set
  sizes, a find bar with the matches marked and gone through (*Edit →
  Find*), and a menu to open it with the system's viewer, show it in the
  file manager or copy its path. The sidebar shows the pages small and the
  document's outline. Text is selected with a drag (a word with a double
  click, the page with *Edit → Select All*) and copied; links are
  followed as a Markdown file's are (below); Ctrl+wheel or a pinch
  zooms, the middle button pans; *View
  All*, *View 1:1* and the zoom commands work on it. When its file is
  written again it is read again where you were. *Save As* copies it;
  *Print* prints its pages. Built with Qt's PDF module (all the release
  bundles have it); without it PDFs open in the system's viewer as before.
- **Tuner in a dock**: *Simulation → Tune* shows the tuner in a dock of
  the main window — at the bottom beside the simulation output the first
  time, where you left it after that, or floated as a window of its own —
  not in a separate window; its sliders scroll when the dock is small.
  Closing the dock stops tuning, as its *Close* button does. Esc in the
  tuner closes it; elsewhere Esc is the window's again (docked, the two
  were ambiguous and neither worked). Closing no longer asks whether to
  keep values nobody changed (a value without a unit prefix, as a
  resistor's `30`, was taken for changed).
- **Markdown viewer and editor**: a `.md` file opens in a tab of its own
  (from anywhere a file opens, whatever text editor the settings name) —
  its text, highlighted and edited as any text document (undo, find and
  replace, save), and the text rendered: GitHub's Markdown (headings,
  lists, task lists, tables, code on a shade, links, images beside the
  file) and TeX math between dollars, typeset. A bar at the top chooses
  *Edit* (the text), *Split* (the text and the rendering side by side, a
  handle sharing the width; the rendering scrolls with the text) or
  *Preview* (the rendering alone); the choice is kept for the next file.
  The rendering follows the edits; a link to a heading scrolls to it, one
  to a Qucs-S document (a schematic, a text, a spreadsheet, a PDF) opens
  it in a tab, and web pages and mail open in the browser or the mail
  program. Nothing else is handed to the system to open — a program
  shipped with a project would run: another file is only shown in the
  Finder or the Explorer once you say so, and a link of another kind
  opens only once you agree, the link named in the question.
- **CSV and Excel workbooks**: `.csv`, `.tsv` and `.xlsx` files open in a
  table, read and written by Qucs-S itself (no Excel, no library). Above
  the table, the cell in front — its name, its value or formula — is
  edited, or type in the table; edits are undone and redone (*Edit →
  Undo*); cells are cut, copied and pasted as tab-separated text (to and
  from other spreadsheets), a value pasted into a range fills it, Delete
  clears them. A CSV file keeps its delimiter (detected: `,` `;` tab
  `|`), quotes, encoding (UTF-8 with or without a byte order mark,
  UTF-16 — Excel's *Unicode Text* — or else Windows-1252; a character it
  has no bytes for makes it UTF-8, once asked) and line ends; its rows and
  columns are inserted and deleted.
  A workbook shows its sheets in tabs, its column widths and merged
  cells, numbers, dates (by their styles), booleans, errors, and formulas
  by their values (their text above the table; a formula filled down
  shows in every cell it fills). Saved, it keeps all but
  the cells changed as they were — formatting, charts, names, the other
  sheets — and Excel calculates its formulas again when it opens it
  (Qucs-S does not calculate them: a changed one shows its formula until
  then). Rows and columns are not inserted into a workbook (its formulas
  would not follow). *Save As* writes CSV, TSV or a workbook (a CSV
  file's numbers as numbers; a workbook's sheet in front as CSV). The old
  binary `.xls` is not read: save it as `.xlsx`. A workbook is opened up
  to Excel's limits (1,048,576 rows, 16,384 columns) and 2,000,000 cells,
  its files at most 256 MB unpacked — a small file that unpacks to
  gigabytes is refused; styles set on far rows and columns take no room.
- **ZIP archives, viewed and edited** (as Eclipse's zip editor): a `.zip`
  opens in a tab of its own — its files and folders in a tree (or a list
  of paths), each with its size, packed size, ratio, time, packing and
  CRC-32, filtered by name - text or a regular expression, on the path
  or the name at its end (*Edit → Find*). *Open* (a double-click) opens
  a file in Qucs-S as a copy: saved there, it is in the archive again.
  *Add Files…*, *Add Folder…*, files dropped on a row or pasted from a
  file manager go into the folder selected (a name there already asked
  about); *New Folder*, *Rename…* (F2), *Delete*; *Extract…* and *Extract
  All…* write entries into a folder you choose, and entries dragged out
  land as files. Every change is a step to undo; *Save* writes the
  archive, the files not changed copied as they were packed. Nothing in
  an archive is run (what Qucs-S does not open is only extracted), and
  nothing is extracted outside the folder chosen. **A new archive**: the
  File Browser's *New Zip…* (its right-click menus and its ⋯ menu) opens
  an empty one, *untitled.zip*; files and folders dropped on it or added
  go in, and *Save* asks where, offering the folder the menu was of and
  a name free there (`Archive.zip`, `Archive 2.zip`).
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
- **The workspace as it was, at the next start** (*Application Settings →
  Workspace*): when Qucs-S closes it keeps what is open, and opens it
  again at the next start - the project; the documents, each in the
  pane it was in, in its order, with the one in front, the panes split
  and sized as they were; the panels and toolbars (which are shown,
  where, how big, the left dock's page); the window's size and place;
  and the Claude Code conversations. Each part can be turned off, or the
  whole. It is kept with each autosave too, so a crash loses little of
  it - and after a run that did not end cleanly Qucs-S asks before it
  opens the workspace again, in case what it opens brought it down; a
  document whose autosaved copy is offered is left for that offer (and
  opened from its file when the offer is declined). A file no longer
  there is left out and the status bar says so; a project named on the
  command line opens instead of the kept one. *Forget It* on the tab
  starts the next run afresh. The tab also holds the workspace folder
  and *Any folder is a project*, which were under *Locations*.
- **Toolbars locked in place** (*View → Toolbars → Lock Toolbars*, the
  menu of a right click on the toolbars, or *Application Settings →
  Appearance*): the toolbars lose their handles and cannot be dragged to
  another place or off the window by accident; one left floating comes
  back to the window. Unlock them to arrange them. The lock is kept
  between sessions (off by default), *View → Toolbars* also shows or
  hides each toolbar, and the lock can be given a shortcut
  (*View.LockToolbars*).
- **Settings exported and imported** (*File → Export Settings…*, *File →
  Import Settings…*): every setting goes into one JSON file, to share or
  keep. That covers the application's and the simulators' settings, the
  theme, the editor's highlighting, the Content panel's categories, the
  shortcuts and Claude Code's model and permissions. The session's state
  stays out: where the windows were, the recent files, the last folders
  and the models the Claude program on this computer offers. The file
  is readable (`"Qucs-S settings": 1`, who made it, on what, when, then
  the settings key by key) and the same on every platform, where the
  store itself is a plist, the registry or an INI file. An import asks
  first. It says where it came from, whether the workspace moves and
  which programs Qucs-S would run with it. It saves the current settings
  into a backup (the last ten are kept, in the application's data folder
  under `settings-backups`; importing one takes them back). Then it
  replaces them and applies them at once: the theme and style, the
  paper, the grid, the toolbars, the editors, the simulators, the
  shortcuts, the Claude Code panels and the workspace. The language, the
  fonts and flexible wires take effect at the next start, and it says so.
  What it leaves as this computer has it, and says so:
  - programs and folders of the other computer that are not on this one
    (a Windows path on a Mac, someone else's home folder);
  - folders searched for subcircuits that are not here;
  - Claude's *Bypass Permissions*, which is chosen in the panel after a
    warning, not brought in by a file.

  Found on the way: the shortcuts set in the Shortcut Manager were saved
  but never loaded again, so they were lost at the next start, and one
  set back to its default stayed saved. The PDF viewer's sidebar was
  kept outside the application's settings. On a Mac, *Edit → Delete*'s
  key was the chord "E, Ctrl+G" (two standard keys' numbers read as keys
  upstream), so neither Delete key deleted anything on the canvas: the
  key labelled delete and forward Delete both do now, and a key set in
  the Shortcut Manager takes their place.
- **Theme** (*Application Settings → Appearance → Theme*, or *View →
  Theme* to switch at once): *System*, *Dark* or *Light* — the
  platform's own look — or one of ten designed themes that look the same
  on every platform: *Daylight*, *Paper* (the warm cream of classic
  Qucs), *Solarized Light* and *Catppuccin Latte*; *Graphite*, *Nord*,
  *Dracula*, *One Dark*, *Solarized Dark* and *Catppuccin Mocha*.
  - The platform's themes: on macOS and Windows Qt asks the platform for
    the appearance, so the native controls, title bars and menus follow;
    elsewhere (Linux, and any platform that does not answer) a dark or
    light palette is put on the application. *System* takes the
    platform's colours again and follows them when they change. They
    draw with the *App Style* above.
  - A designed theme draws with Fusion under a style of its own: flat,
    rounded buttons, fields, check boxes and radio buttons in the
    palette's colours (a colour-picker button still shows its colour),
    and a style sheet for the tool bars, docks, tabs (an accent line
    under the current one), menus, slim scroll bars and the status bar.
    Every theme's text meets WCAG contrast on its backgrounds. The
    window frames and the platform's dialogs follow its darkness.
  - The component list and the text editor take the theme's colours. On
    a dark theme the component and tool bar icons, drawn in dark blue
    for white, are inked like the schematic (see *Schematics on dark
    paper*), and so are the editor's syntax colours.
  - *Schematic paper and grid from the theme* (same tab, off by default)
    puts each designed theme's own paper and grid under the schematic —
    Nord's slate, Solarized's cream, Paper's classic cream — instead of
    the document background and grid colours above; under the platform's
    themes it is the dark paper in *Dark*. Prints and exports stay on
    white.
  Under the platform's themes the text editor is black on white, as
  before.
- **Status bar**: on the left, what the tool in hand does and the keys it
  takes — *Double-click to edit · ⌘-click to add · drag on empty space to
  select*, *Click to place ground · right-click to rotate · Esc to stop*,
  the wiring route and how to change it. On the right, chips that show
  while they have something to say; when the window is narrow the least
  important give way first:
  - the diagram value under the cursor, each axis by its variable with
    its unit (`time 1.5 ms · out.Vt 1.235 V`; on a histogram the value
    and its count; in the diagram's own notation when it has one); the
    selection (one component by its name
    and value, `R1: R = 50`; a click zooms to it); the cursor position
    (line and column in the text editor); the grid (a click shows or
    hides it); the zoom (a menu: fit, the selection, 25–400 %);
  - the electrical rule check of the schematic in front, run again a
    moment after each edit — *No problems*, *1 error, 3 warnings*, the
    first few in its tool tip; a click lists them on the *Problems* tab
    and goes to the first; the last simulation — its seconds while it
    runs, then *Simulated in 0.42 s*, *2 warnings*, *Simulation failed*
    or *stopped*; a click shows its output — in place of the blinking
    "Warnings in last simulation" label; the simulator with its version
    (*Ngspice 46*), whose menu switches simulators as the tool bar's
    list does;
  - whether the document is saved (*Saved 5 min ago*, *Unsaved ·
    autosaved just now*, *Not saved*; a click saves it) and the theme
    (its swatch; a click opens *View → Theme*).
  The green, amber and red marks are fitted to the bar in every theme.
- **Check Schematic** (*Simulation → Check Schematic*, F10): an electrical
  rule check before the simulator sees the circuit — component pins and
  wire ends connected to nothing (a wire end carrying a label is a named
  net, not a problem), two components of one name, no ground, no
  simulation block, and what the simulator in use would drop from the
  netlist (a component not available for it, one without a SPICE model,
  an implicit equation-defined device, a winding without its core, a
  SPICE library part whose library is not found or does not define it) —
  listed on a *Problems* tab of the message dock with
  error/warning icons; a click on a row selects the component and centres
  the schematic on the place. ngspice and Xyce read names without regard
  to case, and so does the check for them: labels `Out` and `out` are one
  net (told, when the wires keep them apart), and `r1` beside `R1` is a
  name used twice. A part switched to shorted joins its pins, as its
  netlist does. It also reads what the parts do, as far as the drawing
  tells it: voltage sources in parallel or shorted by a wire (errors: the
  operating point fails), an inductor across a source, a negative
  capacitance, an AC analysis with no AC source, a NutmegEq's `v(node)` of
  no net (warnings). Every simulation runs the check first and
  brings the tab up when there are errors (the run goes ahead anyway; the
  simulator has the last word). Subcircuits (schematics with ports) are
  not asked for a ground or a simulation. A ground symbol is required by
  default; *Simulation → Simulators Settings → Before a simulation → A
  schematic must have a ground symbol* turns that off: the circuit is
  simulated as it is (node 0 from a net named `0` or a component that
  brings it) and the check says nothing of it. A ground symbol switched
  off is no ground, for the check and the simulators alike. Changing the
  setting checks again: the status bar, and the *Problems* tab as it was
  last filled (this schematic, or its subcircuits too). *Check Schematic and
  Subcircuits* runs it on the schematic in front and on every subcircuit
  it uses, at any depth (open documents as they are, the others from
  disk); a subcircuit's finding names its file and a click opens it
  there. A *Hierarchy and Netlist* toolbar to the right of the simulation
  toolbar holds *Go into Subcircuit*, *Pop out*, the two checks — a
  yellow check mark for this schematic only (*Check Schematic*, F10),
  then the green one for it and its subcircuits — *Generate Netlist* and
  *Save netlist*.
- **Net highlighting**: select a wire and its whole electrical net lights
  up — every wire and node reached through junctions, through wire labels
  of the same name (a `Vout` here joins a `Vout` there) and through ground
  symbols — as an orange glow under the wires, for as long as the
  selection lasts. Several selected wires light their nets together.
  Exports and prints never show it.
- **Marker colours** (double-click a marker): *Text color* and
  *Background color* each *Automatic* - dark text on the paper the
  diagram is drawn on, the light card it is in the dark theme (the label
  was a dark box around black text there) - or any colour, the
  background with transparency too; *Reset* goes back to automatic, and
  *Transparent background* is in sight again (the buttons covered it).
  Saved with the marker, as is its indicator (off, square or triangle),
  which was lost on reading; files of before load as they did.
- **Diagram legend**: every graph diagram (Cartesian, polar, Smith, 3D, …)
  can show a legend — a sample of each graph's line (colour, thickness,
  style or symbol) with its variable — in a corner of its choice:
  *Edit Diagram Properties → Properties → Legend*. Off by default; the
  position is saved with the diagram, and files without it load as before
  (upstream #1719).
- **Diagram themes**: *Edit Diagram Properties → Theme* sets the colour of
  each part of a diagram — its *Background* (under all of it: frame,
  numbers, labels, title), the *Plot area* inside the frame (the circle
  of a polar or Smith chart), the *Frame* (a table's rules), the *Grid*
  (moved here from *Properties*), each axis (its ticks, numbers and
  label), the *Title*, a table's *Text*, and the legend's *Background*,
  *Border* and *Text* (a histogram's statistics box too). Each part is
  *Automatic* or a colour of your own, with transparency; *Reset* goes
  back. Automatic is how diagrams have always been drawn — nothing under
  them on light paper, a white card on dark paper — every part fitted to
  the background it is on: choose a dark background alone and the frame,
  numbers and legend turn light, dark traces lighter. *Theme* starts
  from *Automatic*, *Light* (white on any canvas), *Dark*, *No background*
  (the canvas shows through, the parts fitted to it) or your own default,
  and says *Custom* once you change a colour. A preview shows the diagram
  in the colours as you choose them, on its schematic's canvas. *Save as
  Default for New Diagrams* gives the diagrams you place from then on
  those colours. The colours are saved with the diagram, after its title;
  a diagram whose parts are all automatic is saved as before, and older
  versions read the file as they did. Claude's `add_diagram` and
  `edit_diagram` take a `theme` too.
- **Data in and out of a diagram**: *Edit Diagram Properties → Import*
  reads data files — CSV, TSV, Excel (.xlsx), text in columns, NumPy
  (.npy, .npz), Touchstone, Qucs-S datasets — into datasets of their own
  beside the schematic, to plot next to a simulation's (a trace
  `name:variable`); Claude's `import_data` does the same. *Export* writes
  a dataset's variables to a file for another program: check them (*All*,
  *None*, or *The Diagram's Traces*), choose the format — CSV, TSV, an
  Excel workbook, text in columns, NumPy arrays (.npz) or a Qucs-S
  dataset — and *Export…*. Variables over the same sweep make a table, a
  row for each point, the independent variables first (a parameter
  sweep's repeated as they go round); a complex one takes two columns —
  real and imaginary parts, magnitude and phase, or dB and phase. CSV,
  TSV and text hold one table; a workbook has a sheet for each sweep;
  NumPy has an array for each variable, shaped by what it is over, complex
  ones complex. The tab shows the Data tab's dataset until you choose
  another, and says what would be written before you do. Claude's
  `export_data` writes the same files: a diagram's traces (or some of
  them), or a dataset's variables - by name, as a trace names them
  (`ngspice/run1:v(out)`, `m:gain`), or as an expression
  (`db(ac.v(out)/ac.v(in))`) - so "export these curves as a CSV" is one
  call. As in the tab, the file's suffix gives the format, and neither
  the dataset nor a file a dataset was imported from is written over.
- **Auto colors and point markers for the curves of a sweep**: a graph
  whose variable was swept - a parameter sweep, NgSweep, a Monte Carlo
  family - draws a curve for each value; with *auto* ticked next to the
  graph's *Color* in the diagram dialog, each curve has a color of its
  own, from an eight-color palette in a fixed order (kept apart for
  colour-blind readers too), every curve in the graph's own line style.
  *Marker* puts a symbol on the data points - on every point of a sparse
  curve, some 40 pixels apart along a dense one, each curve's starting a
  little further on so curves that lie on each other still show theirs:
  one shape for all (circle, square, triangle, diamond, triangle down,
  cross, plus), or *auto*, a shape for each curve from the seven in turn
  - with auto colors no two curves alike before the 57th. The legend -
  switched on with either - names every curve by its values (`r1=2.2k`)
  with its line and marker. The auto graphs of a diagram share the
  sequence, each going on where the last left off. In Cartesian, polar,
  Smith and locus diagrams; saved with the graph, and older versions read
  the graph as before.
- **Histogram diagram** (*diagrams → Histogram*): each graph's values -
  a Monte Carlo's samples, or every point of any variable - counted into
  bins and drawn as bars in the graph's colour, several graphs over each
  other on the same bins. The bins cover the values or the x axis'
  manual limits, their number automatic (by the spread of the values,
  Freedman-Diaconis) or set; the height a count, a percentage or a
  probability density. The normal distribution of the same mean and
  deviation can be drawn over the bars, a box gives each graph's number,
  mean and deviation, and a lower and an upper limit are drawn as lines
  with the share of values between them - for a Monte Carlo, the yield.
  Grid, notation, legend, zoom and the cursor readout are the Cartesian
  diagram's; the settings are in its *Properties* tab.
- **A pseudo-random bit sequence source** (*sources → V(PRBS)*, for
  ngspice): ngspice's `PRBS(v1 v2 tbit td tr tf order seed)` - the bits
  of a maximal-length shift register of 2 to 31 stages (PRBS7, PRBS15,
  PRBS31 as a pattern generator sends them) between *U1* and *U2*, a bit
  each *Tbit*, from *Td* on, each edge *Tr* or *Tf* long (0: the time
  step) - or, with *Coding* PAM4, the same register two bits a symbol,
  Gray coded on four levels from U1 to U2 (order 13 is IEEE 802.3's
  PRBS13Q). *Seed* left empty starts the register all ones. No DC value:
  the operating point is U1, where the bits start. Needs an ngspice built
  with the PRBS source (Ngspice-OpenVAF-Enhancements). *Check Schematic*
  gives, as errors, the values ngspice refuses (a Tbit of 0, an Order
  outside 2 to 31, a Seed whose register bits are all zero, an edge
  longer than a bit) and warns of a Coding other than NRZ and PAM4.
- **Eye diagram** (*diagrams → Eye Diagram*), as a sampling oscilloscope
  or Cadence ViVA shows one: each graph - a received data signal, NRZ or
  PAM4 - cut into windows a few unit intervals (UI) long, starting a UI
  apart, and laid over each other, the eye's centre in the middle. Drawn
  as a density - how many traces pass each point, from blue to red, as
  ngspice's `pyplot -eye` draws it - or as the traces themselves, fainter
  the more there are (many bits are drawn as quickly as the density). The
  UI is given; or, on a schematic (or its data display), the *Tbit* of
  the V(PRBS) source each trace comes from - the one nearest its node,
  never through ground, said beside it ("UI 100 ps, V1's Tbit"), as the
  run the data is of gave it (the run's netlist is kept): a Tbit changed
  since is said, the data folded at the bits it was made of. Traces of
  sources of different Tbits are each folded at their own, the time
  across it then in UI. With no source, the UI is told from where the
  first graph crosses its threshold (its crossings' times fitted against
  their UI's number, so the fold does not drift). *Levels* "as the PRBS
  source is coded" (the default) measures a trace of a PAM4 source on its
  four levels, else two. The settling at the start can be left out
  (*From*). Beside it, what is measured on each graph, as
  ngspice's `eye` command measures it: the UI, the eye's height (the
  lowest 1 less the highest 0 at the centre) and width (a UI less the
  crossings' spread), the jitter, rms and peak to peak, the levels and Q
  - for PAM4 each of the three eyes, its thresholds halfway between the
  four levels. A mask - a hexagon at each eye's centre, its width in UI
  and its height - says how many UIs go through it, drawn red when any
  does. The threshold, the centre, the height and width and the mask are
  marked in the eye. The cursor readout gives the time into the window in
  UI too. Claude's `add_diagram` and `edit_diagram` set it all by name
  (`eye`), and `get_schematic` lists what was measured on each trace.
  *examples/ngspice/NGspice features/PRBS_eye_diagram.sch* sends a
  10 Gb/s PRBS7 with noise through an RC channel.
- **Six number notations for a diagram's axes** (*Properties* of a
  diagram, *Number notation* and *Decimal places*): automatic (what
  "scientific" was: decimal, with an exponent for large and small
  numbers), **decimal** (neither exponent nor prefix: 250000, 0.000025),
  scientific (2.5e5), scientific with a power of ten (2.5×10⁵),
  engineering with SI prefixes (250k, as before) and engineering with an
  exponent that is a multiple of three (250e3). *Decimal places* is auto
  (as many as each number needs; decimal labels as many as the grid
  step needs, so they line up: 0.00, 0.25, 0.50) or a fixed number. The
  cursor readout in the status bar follows the diagram, and so does a
  marker unless it has a notation of its own (its dialog's *Number
  format*: as the diagram's axes, or any of the six, its *Precision* the
  places after the point). The notation reaches every number in the
  marker's box: its position, a complex value as real/imaginary or
  magnitude/angle, a value in dB and a Smith chart's impedance (the
  complex values and the impedance were always written in the automatic
  way). `edit_diagram` and `edit_marker` set both. Saved with the diagram
  and the marker; older versions read a notation of their own as
  automatic, and a marker's as the diagram's. Fixed on the way: a diagram without graphs (or whose data
  had not changed) showed a new diagram's axes — 0 to 1, engineering —
  after the schematic was opened, instead of its own.
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
  and a diagram is drawn as a light card, as it prints (unless its
  *Theme* gives it a background of its own). On light paper,
  and in every print and export, nothing changes. *Application
  Settings → Appearance → Schematic paper and grid from the theme*
  (off by default) gives the canvas dark paper whenever the theme is
  dark — a designed theme's own — and follows the system when it
  switches; a dark *Document Background Color* of your own works the
  same way.
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
- **Optimization with ngspice** (upstream #1327): the *Optimization*
  component is no longer qucsator-only. Place it beside an ngspice
  simulation, list its variables (initial value, bounds, a linear or
  logarithmic scale, integers or an E3 ... E192 series) and its goals
  (results of the simulation by name, e.g. a Nutmeg equation's
  `passband = vecmin(gain[0,20])`: at least, at most, equal to a value,
  as small or as large as possible, or just shown), and press Simulate.
  Qucs-S runs differential evolution itself: every candidate is one
  ngspice run, as many at a time as the machine has cores; the console
  shows the start and every few generations the best values and each
  goal met or not. The best values become the component's initial
  values (undoable), and the best point is simulated once more so the
  diagrams show it. A variable can be an equation's or `.PARAM`'s, or
  named only in a component's value (`Rx`, as the qucsator examples
  do); a goal over a sweep counts by its worst point. The run stops
  after the generations of its settings, when the population has
  converged, or as soon as every goal is met when all are limits;
  *Stop* keeps the best so far. Example: *NGspice features →
  LC_lowpass_optimization* chooses the three E24 values of a 50 Ω
  low-pass (0.5 dB to 1 MHz, 30 dB down from 3.16 MHz) in about two
  seconds.
- **NgOpt: ngspice's own optimizer as a component**: for ngspice builds
  that have the `optimize` command (the
  [Ngspice_OpenVAF_Enhancements](https://github.com/javaNoviceProgrammer/Ngspice_OpenVAF_Enhancements)
  fork), *simulations → ngspice optimize* is a form for it. Its knobs are
  an equation's or `.PARAM`'s variables (*Add Equation Variables* adds
  them all), device instances (`R1`, `@m1[w]`) or model parameters
  (`@dmod[is]`), each with a range. The objective is an ngspice
  expression to minimize after an analysis, or targets to fit by least
  squares, each after its own analysis (a simulation component of the
  schematic, or an ngspice command such as `ac lin 1 1meg 1meg`). The
  method is differential evolution, particle swarm, simulated
  annealing, Nelder-Mead or Levenberg-Marquardt, with iterations,
  tolerance, population and seed. The dialog shows the command it
  writes. Press Simulate: ngspice optimizes first, in one process, and
  the schematic's simulations then run at the optimum; the status log
  has ngspice's verdict and the values found become the knobs' initial
  values. Example: *NGspice features → LC_lowpass_ngopt* fits the
  low-pass to a Butterworth response in 39 evaluations.
- **NgMonteCarlo and NgCorners: ngspice's own statistical loops as
  components**: for the same ngspice builds, which have the `montecarlo`
  and `corners` commands. *simulations → ngspice Monte Carlo* runs an
  analysis (a simulation component of the schematic, or an ngspice
  command) for N samples - plain or Latin hypercube, with a seed -
  drawing the circuit's random values anew each time (`agauss()`,
  `aunif()` in a `.PARAM` or a component's value, and optionally the
  Verilog-A models' declared statistics). It records the values you
  name: a number per sample lands in the dataset against the sample (a
  Histogram diagram shows its distribution), and a waveform as a family
  of curves, one per sample. Specs with limits give a yield and
  its 95% confidence interval in the status log and the dataset.
  *simulations → ngspice corners* runs the analysis at every process
  corner the Verilog-A models declare (`(* corner="ss=+10%, ff=-10%" *)`),
  or at the ones listed, the nominal first: the values you name per
  corner, and the voltages and currents at every corner as families,
  or instead a Monte Carlo with a yield at each corner. The status log
  names the corners with their values. Everything is under the
  component's name in the dataset (`ngmontecarlo1.gain`,
  `ngcorners1.v(out)`). Example: *NGspice features →
  RC_lowpass_montecarlo*: 200 samples of an RC low-pass with 5% parts,
  the family of gain curves, the histogram of the corner frequency with
  its normal fit and the ±10% limits, and the yield.
- **NgSweep: ngspice's own parametric sweep as a component**: for
  ngspice builds with the `sweep` command. *simulations → ngspice sweep*
  changes a parameter - a component (`R1`, `V1`), an instance or model
  parameter (`@m1[w]`, `@dmod[is]`), a `.param` (an equation's
  variable) or `temp`; ngspice tells which - over a linear, logarithmic
  or listed set of values, and runs an analysis (a simulation component
  of the schematic, switched off if only its sweep is wanted, or an
  ngspice command) at each. Every value's voltages and currents over
  frequency, time or a dc sweep come into the dataset as a family of
  curves (`ngsweep1.v(out)` against `ngsweep1.frequency` and
  `ngsweep1.r1`); after an `op`, the voltages and currents against the
  parameter. Up to three more parameters swept around the first make a
  curve for every combination, and named values (`maximum(vdb(out))`)
  are recorded at every point. The dialog shows the command it writes;
  the status log what was swept and ngspice's warnings. Example:
  *NGspice features → RC_lowpass_ngsweep*: an RC low-pass for five
  values of R1, the curves in auto colors, and the corner frequency
  against R1.
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
  was whatever had been in it; and a new schematic saved with *Save As*
  did not know its folder until it was opened again, so a subcircuit or
  a SPICE library beside it was not found (the subcircuit had no pins).
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
- **More paintings**: the *paintings* group of the component panel has
  thirteen new entries, each with its own icon:
  - **Rounded Rectangle** (outline or filled): a block of a block
    diagram, with a corner radius you choose.
  - **Triangle**, **Regular Polygon** and **Star**:
    - fitted to the box you draw;
    - the triangle points right, as an amplifier does;
    - the number of sides or points, the star's inner radius and where
      the first corner points can all be set.
  - **Brace**: a curly brace, square bracket or parenthesis, to group
    parts of a schematic.
  - **Waveform**: a few cycles of sine, square, triangle, sawtooth,
    pulse (with its width) or damped sine, with an optional line through
    the middle. Use it to mark a source or a signal.
  - **Text Box**, **Note** and **Callout**:
    - text wrapped inside a box with round corners, with its size,
      colour, bold and alignment;
    - a Note is yellow and a Callout has a pointer to what it is about,
      whose tip you drag by its handle.
  - **Table**:
    - rows and columns of text, with an optional header row in its own
      colour;
    - the cells are edited in a grid in its dialog.
  - **Dimension**:
    - click two points to measure the distance between them, drawn with
      extension lines and arrows, ticks or dots;
    - the middle handle drags the dimension line off the points;
    - the label is the length times a scale with a unit (e.g. "12.5 mm"),
      or a text of your own.
  - **Formula**:
    - TeX maths typeset on the schematic, e.g.
      `f_c = \frac{1}{2\pi RC}`: fractions, roots, sums and integrals with
      limits, matrices, Greek;
    - redrawn sharp at any zoom, and its handle scales it.

  The box shapes are drawn like a rectangle: click one corner, then the
  other (a single click gives a default size). Corner handles resize them.
  They rotate in quarter turns and mirror, and all of them work in copy
  and paste, undo, printing and export. Each opens a properties dialog
  with a live preview. Drawn in a subcircuit's symbol, they reach every
  instance: the shapes become polylines, text boxes and tables become
  texts, and a formula becomes an image. The Claude Code tools know
  their line formats.
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
- **Workspace, import and link from the Projects panel**: right-click
  the *Projects* panel (or use the *Project* menu, which has all but
  *Unlink Project* and *Open Project*, actions on the row right-clicked -
  its own *Open Project...* asks for the folder):
  - *Switch Workspace...* chooses another folder as the workspace; the
    panel lists its projects, and the choice is kept (the same as
    *Application Settings → Locations*, which now also updates the
    panel — it used to keep listing the old workspace until a restart).
  - *Import Project...* copies a project folder (`NAME_prj`) from
    anywhere into the workspace.
  - *Link Project...* puts a link to a project folder elsewhere into the
    workspace: nothing is copied, and the project is listed (in italics,
    with its real place as the tooltip), opened, edited and simulated as
    any other; its files and Scratch folder stay where they are.
    *Delete* on a linked project removes the link only — the project
    itself is not touched. (A symbolic link; on Windows a junction when
    symbolic links need Developer Mode.)
  - *Unlink Project*, on a linked project's row, removes its link from
    the workspace after asking; the project's files stay where they are,
    and *Link Project...* brings it back. The open project closes first
    (asking about unsaved changes), and documents opened through the link
    close with it - however their paths are spelled (through a link of
    the workspace's, /tmp for /private/tmp).
  - A linked project whose folder is gone (moved, deleted, on a drive not
    mounted) is listed greyed, its old place as the tooltip, so that it
    can be unlinked; linking a project of its name offers to replace it.
  - *Open Project*, on a project's row, opens that project as a
    double-click does (not the one open already).
  - *Close Project* closes the open project, as in the *Project* menu
    (in the panel's menu only while a project is open).
  When the workspace already has a project of that name, you are asked
  for another one.
- **Any folder can be a project**: by default a project is a folder
  whose name ends in `_prj`. With *Application Settings → Workspace →
  Projects → Any folder is a project* on, a folder of any name is one:
  - every folder of the workspace is listed and opens as a project
    (except `user_lib`, which holds the user libraries, and hidden
    folders);
  - *Import Project...* and *Link Project...* take any folder and keep its
    name (a folder with a name already in the workspace is offered
    `name_2`, not `name_2_prj`);
  - *Open Project*, a folder dropped on the window and `qucs-s FOLDER`
    open any folder as the project - not the workspace folder itself,
    nor the home folder - and write nothing into it (its Scratch folder
    is in the cache directory);
  - *New Project* names the folder exactly as typed.
  The project's name is its folder's name, with a `_prj` ending dropped.
  *Delete Project* moves the folder to the trash; its question names the
  folder and what is in it, and says so of a folder that is a project
  only by this option.
  Folders named `NAME_prj` are always projects. Other Qucs-S
  installations still recognise only those, so a workspace of plain
  folders shows no projects there. The option is off by default, and
  turning it on or off updates the Projects panel and the File Browser
  straight away.
- **Export to SVG, PDF, EPS, JPEG and more, without Inkscape** (*File →
  Export as image...*, or *Export...* on the context menu of the empty
  canvas; *Export Diagram...* on a diagram for the diagram alone):
  PNG, JPEG, BMP, TIFF, WebP, SVG, PDF, EPS and *PDF + LaTeX*, all
  written by Qucs-S itself. Before, PNG and JPEG were the only formats
  that worked everywhere: SVG came out at the wrong scale in a corner of
  its canvas, with its text in a font no other program has (macOS's
  system font), and PDF, EPS and PDF + LaTeX ran Inkscape with options
  Inkscape 1.x no longer takes — an error without Inkscape. Now:
  - a vector file has the size of the drawing (a unit of the schematic is
    1/96 inch): an SVG with its `viewBox`, a PDF page cropped to the
    drawing with its fonts embedded, an EPS from a PostScript writer of
    its own (Qt 6 has none);
  - *Text as outlines* for SVG and PDF (always for EPS): the file looks
    the same everywhere; without it an SVG names a font the reader has;
  - *PDF + LaTeX* writes `NAME.pdf` without text and `NAME.pdf_tex` with
    it, for LaTeX to set in the document's font: `\input{NAME.pdf_tex}`,
    `\def\svgwidth{\columnwidth}` for its width; the text is as large
    as in the drawing (`\def\qucsdocumentfont{}` keeps the document's
    size), turned text stays turned, a text with `$` is taken as LaTeX;
  - images at any scale or resolution (the dpi is written into the file),
    JPEG and WebP quality;
  - colour, grayscale or black and white (lines and text black, fills
    white unless dark), on white paper or a transparent background;
  - the whole document or the selection, the file named after the
    document, and the choices kept for the next time.
  *Copy to Clipboard* in the dialog, and *Edit → Copy as Image* (also in
  the context menu), put the selection — or everything — on the
  clipboard as an image, an SVG and a PDF, for another program to paste.
  On the command line `qucs-s -p -i FILE.sch -o OUT.ext` takes the same
  formats by the extension, `--dpi` for an image's resolution and
  `--color BW`; a PDF is the size of the drawing unless `--page` or
  `--orin` asks for a page.
- **Large schematics stay responsive**: an RC ladder of 24,000
  components took 170 ms to repaint, whatever part of it was on show, so
  a selection rectangle, a drag or a wire being drawn moved at six frames
  a second. Now the canvas draws only what reaches the part being
  painted (a corner at 1:1: 6 ms); zoomed out so far that a line of text
  would be under four pixels tall it leaves out component names and
  values, symbol texts and pin names, which no one could read (prints and
  exports keep them; 175 ms to 69 ms for the whole ladder); and a gesture
  draws the rest of the schematic once and, at each step, only what it
  drags or draws over it: a step of a selection rectangle, a drag, a wire
  or a symbol being placed takes 4 to 13 ms. The preview of the wires a
  drop will mend is planned from the nodes something has left, not from
  the whole schematic at every step.
- **View All with diagrams on the schematic** shows all of it, filling
  the window. A marker's bounds were in its diagram's own coordinates, so
  each marker added a box near the origin and View All zoomed out to
  show it, the schematic small in a corner (a selection rectangle there
  also selected markers far away). A diagram's title, and the numbers of
  its axes on the top and right edges, were left out of its bounds - a
  title wider than its plot was cut off; Print, Zoom to Selection and the
  image export use the same bounds. A Smith chart's drawing counted as the
  circles its grid arcs are parts of, many times its size, and its white
  card in the dark theme was as large.

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
next start to report or offer. `QUCS_TRASH_DIR=<dir>` does the same for
the trash: what Qucs-S moves to the trash (the File Browser, Delete
Project, Claude's `trash_file`, `clean_scratch` and `import_data`'s
`remove`) goes into `<dir>` instead of the system's. The tests set it, so a
test run puts nothing in your trash.

`scripts/ci/smoke-test.sh` has three suites — `load` (render every ngspice
example, and one in every export format), `simulate` (netlist → ngspice → dataset → render; needs `ngspice` on
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

`qucs/tests/test_gui_monkey` does it to the main window: a seeded random
walk over copies of the ngspice examples — clicks, drags (some interrupted
by an undo or a delete), double clicks, the wheel and keys in every mouse
mode, the menu and toolbar commands, tabs, symbol view and hierarchy,
placing components, closing and reopening — where every dialog that comes
up is cancelled or filled with odd values and accepted. It asserts nothing
but that the application survives; a watchdog turns a step that never
ends into an abort with the stack, and the last steps are printed with any
failure. CTest runs a short walk; a stress run takes more steps and seeds,
and a seed replays the same walk on every platform:

```bash
QT_QPA_PLATFORM=offscreen QUCS_MONKEY_SEED=42 QUCS_MONKEY_STEPS=2000 build-asan/qucs/tests/test_gui_monkey
seq 1 100 | xargs -P 10 -I{} sh -c 'QT_QPA_PLATFORM=offscreen QUCS_MONKEY_SEED={} QUCS_MONKEY_STEPS=2000 build-asan/qucs/tests/test_gui_monkey > /tmp/monkey-{}.log 2>&1 || echo "seed {} failed"'
```

What a walk finds gets a test of its own in `qucs/tests/test_stress_findings`,
since the walk goes elsewhere after any change.

`qucs/tests/test_large_schematics` times loading, rotating, undo, deleting
and dragging on chains of 2,000 and 8,000 components and fails when
four times the elements take ten times as long: these were quadratic until
the node and wire lookups by place (`qucs/conductor_index.h`), and a
16,000-component schematic stopped for nine seconds after each edit.
`qucs/tests/test_canvas_drawing` paints the canvas of the examples (every
symbol of the symbol galleries) in parts and checks each looks as in a
paint of the whole, checks that a step of a gesture shows what a whole
paint would, also after an edit or a selection made from elsewhere, and
times a corner and a gesture's step against a whole paint of 16,000
components.

`qucs/tests/test_claude_code` drives the Claude Code dock's session and
the dock itself with a shell script that answers as `claude` does (stream
events, a permission request, a result), and the GUI monkey has one
(`qucs/tests/monkey_claude.sh`) that also dies now and then. No test
reaches a `claude` installed on the machine: `QUCS_CLAUDE`, which names
the program over the settings, points nowhere for them. The same script
answers the dock's question for the models on offer, and the test checks
the menu it builds and which models have auto mode. It also checks the
tools folding into a line and opening, the conversations in tabs (what
their tabs say, which one the status bar leads to, closing them), and
the math: found in Markdown but not in code or money, typeset, and
1,500 random scraps of TeX set without harm (the test runs under the
address sanitizer too), and the host's tool server over the stream: the
handshake, tools/list and tools/call answered, a tool that looks used
without asking, one that changes asked about, and none after *Allow
Qucs-S Control*. `qucs/tests/test_qucs_control` uses Claude's tools on
the application itself: a schematic built part by part (placed, wired
pin to pin, labelled, changed, turned, moved, deleted, each one step to
undo), read back as a summary and as text and replaced from text (and
left alone, without a message box, when the text does not read), a
picture of it, documents saved, closed and opened, menu actions used,
Document Settings opened by its action and filled in and answered from
within its own event loop, and a simulation waited for. The GUI
monkey's fake claude calls the tools too, some with arguments no one
would give (coordinates of two billion, junk .sch text, pins that do not
exist).

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
(replaced each time); a tag push (`git tag -a v26.1.5 && git push origin v26.1.5`)
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
scripts/fetch-binaries.sh v26.1.5      # a tagged release
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
