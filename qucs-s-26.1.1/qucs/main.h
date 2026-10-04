/***************************************************************************
                                  main.h
                                 --------
    begin                : Mon May 24  2004
    copyright            : (C) 2003 by Michael Margraf
    email                : michael.margraf@alumni.tu-berlin.de
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/
/*!
 * \file main.h
 * \brief Definitions and declarations for the main application.
 */

#ifndef QUCS_MAIN_H
#define QUCS_MAIN_H

#include <QFont>
#include <QColor>
#include <QMap>
#include <QStringList>
#include <QDir>
#include <QtGlobal>
#include <QMessageLogContext>


class QucsApp;
class Component;
class VersionTriplet;

static const double pi = 3.1415926535897932384626433832795029;  /* pi   */

/// A category of the Content panel the user added: its name and the file
/// name patterns it lists ("*.s2p, *.s4p").
struct ContentCategory {
  QString name;
  QString patterns;
  bool operator==(const ContentCategory&) const = default;
};

struct tQucsSettings {
  int DefaultSimulator;

  QFont font;
  QFont appFont;
  QFont textFont;
  QFont sysDefaultFont;
  float largeFontSize;
  QColor BGColor;      // background color of view area
  QString Language;

  // syntax highlighting
  QColor Comment, String, Integer, Real, Character, Type,
    Attribute, Directive, Task;

  unsigned int maxUndo;    // size of undo stack
  QString Editor;
  QString Qucsator;
  QString QucsatorDir;
  QString QucsatorVar;
  QString Qucsconv;
  QString BinDir;
  QString LangDir;
  QString LibDir;
  QString SpiceLibDir;
  QString OctaveDir;  // m-files location
  QString ExamplesDir;
  QString DocDir;

  unsigned int NodeWiring;
  QDir QucsWorkDir;

  // A dir for user projects and libraries. See also https://github.com/ra3xdh/qucs_s/issues/145
  QDir qucsWorkspaceDir;
  // The workspace --workspace gave this run: its own, not the settings' -
  // nor saved as theirs. Empty when none was given.
  QString workspaceOfRun;

  // This is the dir where all temporary or intermediate data should be stored.
  // Consider a data "temporary" if its used only once or it makes sense only
  // through out a single app run or a shorter period of time.
  // Don't make any assumptions about the lifetime of contents in this dir,
  // think that everything placed in here is deleted when app is terminated.
  QDir tempFilesDir;
  QDir projsDir; // current user projects subdirectory
  QDir AdmsXmlBinDir;  // dir of admsXml executable
  QDir AscoBinDir;     // dir of asco executable
  QString OpenVAFExecutable;
  QString NgspiceExecutable;  // Executables of external simulators
  QString XyceExecutable;
  QString XyceParExecutable;
  QString SpiceOpusExecutable;
  QString S4Qworkdir;
  unsigned int NProcs; // Number of processors for Xyce
  QString OctaveExecutable; // OctaveExecutable location
  QString PythonExecutable; // the Python shell dock's interpreter; empty: python3 on PATH
  QString QucsOctave; // OUCS_OCTAVE variable
  QString RFLayoutExecutable;
  bool ResolveSpicePrefix;

  // registered filename extensions with program to open the file
  QStringList FileTypes;

  // List of extensions used for spice files
  QStringList spiceExtensions;

  unsigned int numRecentDocs;
  QStringList RecentDocs;

  QStringList RecentProjects;
  // A project is any folder (in the workspace, or opened as one), not only
  // one named NAME_prj: qucs_s::workspace (workspace.h) says which are.
  bool AnyFolderIsProject = false;
  // The workspace brought back at the start as it was at the last close
  // (Application Settings > Workspace, workspacesession.h): the project,
  // the documents in their panes, the panels and toolbars - each as said
  // below, all of them only when RestoreWorkspace is on. The window's size
  // and place are kept apart from them.
  bool RestoreWorkspace = true;
  bool RestoreProject = true;
  bool RestoreDocuments = true;
  bool RestorePanels = true;
  bool RestoreWindowGeometry = true;

  bool IgnoreFutureVersion;
  bool GraphAntiAliasing;
  bool TextAntiAliasing;

  bool hasDarkTheme;
  bool fullTraceName;
  bool alwaysPrefixDataset;
  bool ContentTreeView;   // Content panel: subdirectories as sub-trees (true, the default) or "dir/name" rows (false: Flat)
  bool ContentFolderIcons = false;   // Content panel: a folder icon on the sub-trees' folder rows
  bool ContentAutoRefresh = true;   // Content panel: look for files that came or went, every ...
  int ContentRefreshSeconds = 3;    // ... this many seconds
  // A file's name in a document's tab and in the Claude Code panel: cut
  // after this many characters, "…" for the rest, its extension whole
  // (misc::shownFileName()); 0, never cut.
  int FileNameCap = 50;
  // Content panel: the file name patterns of the categories the user
  // changed, by category key (ProjectView::patterns() has the defaults).
  QMap<QString, QString> ContentPatterns;
  // Content panel: the categories the user added, in their order - after
  // the built-in ones, before Others (ProjectView::categories()).
  QList<ContentCategory> ContentUserCategories;
  // The text editor's syntax highlighting: the formats the user changed
  // ("python/Keyword" -> "#00007f bold"), and the language chosen for a
  // suffix ("inc" -> "spice"); qucs_s::syntax (syntax.h) has the defaults.
  QMap<QString, QString> SyntaxFormats;
  QMap<QString, QString> SyntaxForSuffix;
  // Symbols: write the name of each pin of a subcircuit inside its
  // symbol, and mark which way the pin points (in, out, inout).
  bool ShowPinNames = true;
  bool ShowPinDirections = false;
  // Create Library: the Verilog-A sources (.va) the subcircuits use go
  // into the library, beside its other files; compiled where it is used.
  bool EmbedVerilogAInLibraries = true;
  // Saving a text document also writes its Document Settings into a file
  // beside it (name.cfg). Off: only when they hold something (a document
  // whose settings were set) or changed; a file of defaults says nothing.
  bool WriteTextDocSettings = true;
  bool PaperFollowsTheme = false;   // the schematic paper (and grid) is the theme's
  int GridMode = 0;   // the grid of the schematics: 0 as each says, 1 always hidden, 2 always shown
  // Where the simulator's output goes: the Simulation dock, a window of
  // its own, or the legacy window that blocks the application until closed.
  enum SimConsoleHost { SimConsoleDock = 0, SimConsoleWindow = 1, SimConsoleLegacyWindow = 2 };
  int SimulationConsoleHost = SimConsoleDock;
  // A circuit must have a ground symbol to be simulated (the simulators
  // refuse it, Check Schematic calls it an error); off, nothing is said
  // of a missing one and the simulator decides.
  bool RequireGround = true;
  // What a simulation runs besides the simulator - a System command part,
  // ngspice's shell, the Octave script after it - is looked for: Check
  // Schematic warns of each, and Claude's simulate and tune refuse a
  // schematic with one unless asked (erc::commandsRun). Off by default.
  bool CheckCommands = false;
  // An ngspice netlist includes ngspice_mathfunc.inc, when the installation
  // has it (share/qucs-s/xspice_cmlib/include): limexp, step and stp, which
  // expressions written for Qucsator use and ngspice has not. On by
  // default; Simulator Settings > Netlist.
  bool NgspiceMathFuncs = true;
  // An ngspice or Xyce run whose results are larger than DatasetTextLimitMB
  // keeps them binary (datasetfile.h): smaller, every digit, quick to read.
  bool DatasetBinary = true;
  int DatasetTextLimitMB = 10;
  // The application's colours: the system's (0), dark (1) or light (2) -
  // qucs_s::apptheme::Theme.
  int Theme = 0;
  // The toolbars stay where they are: they cannot be dragged elsewhere
  // (View > Toolbars > Lock Toolbars).
  bool LockToolbars = false;

  // Folders of component libraries besides the installed ones and the
  // workspace's user_lib (Settings > Locations > Library Search Paths):
  // each a section of the Libraries panel, and searched for a library a
  // placed part names that is not where it was.
  QStringList LibraryPaths;

  bool firstRun;
};

extern tQucsSettings QucsSettings;  // extern because nearly everywhere used
extern QucsApp *QucsMain;  // the Qucs application itself
extern QString lastDir;    // to remember last directory for several dialogs
extern QStringList qucsPathList;
extern VersionTriplet QucsVersion;

bool loadSettings();
bool saveApplSettings();
void qucsMessageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg);
/// Qt's hint on macOS that a missing font family made it list the other
/// names of every font, for the generic "sans-serif" the texts of the
/// icons ask for: qucsMessageOutput leaves it out.
bool isGenericFontFamilyHint(const QMessageLogContext &context, const QString &msg);

#endif // ifndef QUCS_MAIN_H
