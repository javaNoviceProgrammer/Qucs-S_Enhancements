/*
 * qucs_python.cpp - the Python toolbar and the console of a script's run:
 *                   QucsApp's side of the Python editor (pythondoc.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "qucs.h"

#include "main.h"
#include "messagedock.h"
#include "processconsole.h"
#include "pythondoc.h"
#include "pythonrun.h"
#include "pythonviews.h"
#include "qucscontrol.h"
#include "schematic.h"
#include "diagrams/diagram.h"
#include "diagrams/graph.h"
#include "settings.h"

#include <QAction>
#include <QActionGroup>
#include <QInputDialog>
#include <QJsonArray>
#include <QComboBox>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QStatusBar>
#include <QTextBlock>
#include <QDir>

#include <algorithm>
#include <tuple>
#include <utility>
#include <QToolBar>

bool QucsApp::isPythonFile(const QString &name)
{
  const QString suffix = QFileInfo(name).suffix().toLower();
  // (.pyi: a stub - jedi's Go to Definition of a builtin is to one.)
  return suffix == QLatin1String("py") || suffix == QLatin1String("pyw") || suffix == QLatin1String("pyi");
}

PythonDoc *QucsApp::currentPythonDoc() const
{
  return DocumentTab != nullptr ? qobject_cast<PythonDoc *>(DocumentTab->currentWidget()) : nullptr;
}

// ----------------------------------------------------------
// The toolbar, made with the others (initToolBar()): shown while a Python
// script is in front - unless it was hidden from the Toolbars menu, which
// is kept.
void QucsApp::initPythonToolbar()
{
  pythonToolbar = new QToolBar(tr("Python"));
  pythonToolbar->setObjectName(QStringLiteral("pythonToolbar"));
  addToolBar(pythonToolbar);

  pythonInterpreters = new QComboBox(pythonToolbar);
  pythonInterpreters->setObjectName(QStringLiteral("pythonInterpreters"));
  pythonInterpreters->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  pythonInterpreters->setToolTip(tr("The Python the script runs and is checked with: a virtual environment beside "
                                    "it or in the project, the one Application Settings name, those on the PATH - "
                                    "or another, chosen with Browse."));
  connect(pythonInterpreters, &QComboBox::activated, this, &QucsApp::slotPythonInterpreterChosen);
  pythonToolbar->addWidget(pythonInterpreters);

  pythonRunAction = new QAction(QIcon(QStringLiteral(":/bitmaps/svg/python_run.svg")), tr("Run"), this);
  pythonRunAction->setObjectName(QStringLiteral("pythonRun"));
  // (F2 is Simulate's, which runs a Python script in front: a second
  // action of the key would leave it to neither.)
  pythonRunAction->setToolTip(tr("Run (F2): the script saved and run with this Python in its folder, its output "
                                 "in the Python Run console"));
  connect(pythonRunAction, &QAction::triggered, this, &QucsApp::slotPythonRun);
  pythonToolbar->addAction(pythonRunAction);

  pythonStopAction = new QAction(QIcon(QStringLiteral(":/bitmaps/svg/python_stop.svg")), tr("Stop"), this);
  pythonStopAction->setObjectName(QStringLiteral("pythonStop"));
  pythonStopAction->setToolTip(tr("Stop: the script that is running ended"));
  pythonStopAction->setEnabled(false);
  connect(pythonStopAction, &QAction::triggered, this, &QucsApp::slotPythonStop);
  pythonToolbar->addAction(pythonStopAction);

  pythonDebugAction = new QAction(QIcon(QStringLiteral(":/bitmaps/svg/python_debug.svg")), tr("Debug"), this);
  pythonDebugAction->setObjectName(QStringLiteral("pythonDebug"));
  // (Ctrl+F2 Continue's too, which is enabled while Debug is not.)
  pythonDebugAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_F2));
  pythonDebugAction->setToolTip(tr("Debug (Ctrl+F2): the script saved and run under the debugger - stopped at its "
                                   "breakpoints (a click in the line numbers' margin) and where an exception no one "
                                   "catches is raised"));
  connect(pythonDebugAction, &QAction::triggered, this, &QucsApp::slotPythonDebug);
  pythonToolbar->insertAction(pythonStopAction, pythonDebugAction);   // (Run, Debug, Stop)

  pythonShellAction = new QAction(QIcon(QStringLiteral(":/bitmaps/svg/python_shell.svg")), tr("Run in Shell"), this);
  pythonShellAction->setObjectName(QStringLiteral("pythonRunInShell"));
  pythonShellAction->setToolTip(tr("Run in Shell: the script saved and run in the Python Shell (the Python of "
                                   "Application Settings), in its folder - its variables are there after it, to "
                                   "look at"));
  connect(pythonShellAction, &QAction::triggered, this, &QucsApp::slotPythonRunInShell);
  pythonToolbar->addAction(pythonShellAction);

  // The debugger's steps, while it is stopped.
  pythonToolbar->addSeparator();
  // (Each to slotPythonDebugStep(), which tells them apart: no lambda in a
  // lambda calling through a pointer to a member - GCC's UBSan takes the
  // inner one's captured this for the closure.)
  const auto debugAction = [this](QAction *&action, const char *name, const QString &text, const QString &icon,
                                  const QKeySequence &key, const QString &tip) {
    action = new QAction(QIcon(icon), text, this);
    action->setObjectName(QLatin1String(name));
    action->setShortcut(key);
    action->setToolTip(tip);
    action->setEnabled(false);
    connect(action, &QAction::triggered, this, &QucsApp::slotPythonDebugStep);
    pythonToolbar->addAction(action);
  };
  debugAction(pythonContinueAction, "pythonContinue", tr("Continue"), QStringLiteral(":/bitmaps/svg/python_continue.svg"),
              QKeySequence(Qt::CTRL | Qt::Key_F2), tr("Continue (Ctrl+F2): on to the next breakpoint"));
  debugAction(pythonStepOverAction, "pythonStepOver", tr("Step Over"), QStringLiteral(":/bitmaps/svg/python_stepover.svg"),
              QKeySequence(Qt::CTRL | Qt::Key_F10), tr("Step Over (Ctrl+F10): to the next line, a call on this one made whole"));
  debugAction(pythonStepIntoAction, "pythonStepInto", tr("Step Into"), QStringLiteral(":/bitmaps/svg/python_stepinto.svg"),
              QKeySequence(Qt::CTRL | Qt::Key_F11), tr("Step Into (Ctrl+F11): into the call on the line - one of the script's, "
                                                          "not Python's library"));
  debugAction(pythonStepOutAction, "pythonStepOut", tr("Step Out"), QStringLiteral(":/bitmaps/svg/python_stepout.svg"),
              QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F11), tr("Step Out (Ctrl+Shift+F11): out of the function, to the line "
                                                                     "that called it"));
  // Pause shares Ctrl+F2 with Debug and Continue: one of the three enabled
  // at a time (not debugging, running, stopped).
  pythonPauseAction = new QAction(QIcon(QStringLiteral(":/bitmaps/svg/python_pause.svg")), tr("Pause"), this);
  pythonPauseAction->setObjectName(QStringLiteral("pythonPause"));
  pythonPauseAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_F2));
  pythonPauseAction->setToolTip(tr("Pause (Ctrl+F2): the script debugged stopped at its next line - one in a long call "
                                   "of Python's library stops after it"));
  pythonPauseAction->setEnabled(false);
  connect(pythonPauseAction, &QAction::triggered, this, [this] {
    if (pythonRun != nullptr) pythonRun->pause();
    updatePythonToolbar();
  });
  pythonToolbar->insertAction(pythonStepOverAction, pythonPauseAction);
  pythonRunToCursorAction =
      new QAction(QIcon(QStringLiteral(":/bitmaps/svg/python_runtocursor.svg")), tr("Run to Cursor"), this);
  pythonRunToCursorAction->setObjectName(QStringLiteral("pythonRunToCursor"));
  pythonRunToCursorAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F10));
  pythonRunToCursorAction->setToolTip(tr("Run to Cursor (Ctrl+Shift+F10): debugged on to the cursor's line - once, a "
                                         "breakpoint before it stopping it first; debugging begins when it has not"));
  connect(pythonRunToCursorAction, &QAction::triggered, this, &QucsApp::slotPythonRunToCursor);
  pythonToolbar->addAction(pythonRunToCursorAction);
  pythonRaisedAction = new QAction(tr("Break on Raised Exceptions"), this);
  pythonRaisedAction->setObjectName(QStringLiteral("pythonBreakOnRaised"));
  pythonRaisedAction->setCheckable(true);
  pythonRaisedAction->setChecked(_settings::Get().item<bool>("PythonBreakOnRaised"));
  pythonRaisedAction->setToolTip(tr("The debugger stops where an exception is raised in the script's code, caught or "
                                    "not (StopIteration aside) - not only where one no one catches ends it"));
  connect(pythonRaisedAction, &QAction::toggled, this, [this](bool on) {
    _settings::Get().setItem<bool>("PythonBreakOnRaised", on);
    if (pythonRun != nullptr && pythonRun->isDebugging()) pythonRun->setBreakOnRaised(on);
  });
  pythonLibraryAction = new QAction(tr("Debug Library Code"), this);
  pythonLibraryAction->setObjectName(QStringLiteral("pythonDebugLibrary"));
  pythonLibraryAction->setCheckable(true);
  pythonLibraryAction->setChecked(_settings::Get().item<bool>("PythonDebugLibraryCode"));
  pythonLibraryAction->setToolTip(tr("Step Into goes into Python's library and installed packages too (opened read-only), "
                                     "not over them; their frames are stopped in like the script's"));
  connect(pythonLibraryAction, &QAction::toggled, this, [this](bool on) {
    _settings::Get().setItem<bool>("PythonDebugLibraryCode", on);
    if (pythonRun != nullptr) pythonRun->setDebugLibrary(on);
  });
  pythonInterruptAction = new QAction(tr("Interrupt"), this);
  pythonInterruptAction->setObjectName(QStringLiteral("pythonInterrupt"));
  pythonInterruptAction->setToolTip(tr("A KeyboardInterrupt in the script that is running, as Ctrl+C in a terminal - in a "
                                       "long call too, where Pause waits; debugged, it stops where it was"));
  pythonInterruptAction->setEnabled(false);
  pythonInterruptAction->setVisible(PythonRunConsole::canInterrupt());
  connect(pythonInterruptAction, &QAction::triggered, this, [this] {
    if (pythonRun != nullptr) pythonRun->interrupt();
  });
  pythonRunSettingsAction = new QAction(tr("Run Settings..."), this);
  pythonRunSettingsAction->setObjectName(QStringLiteral("pythonRunSettings"));
  pythonRunSettingsAction->setToolTip(tr("The script's arguments, working folder, environment variables and .env file, "
                                         "for Run and Debug"));
  connect(pythonRunSettingsAction, &QAction::triggered, this, &QucsApp::slotPythonRunSettings);

  pythonEditBreakpointAction = new QAction(tr("Edit Breakpoint..."), this);
  pythonEditBreakpointAction->setObjectName(QStringLiteral("pythonEditBreakpoint"));
  pythonEditBreakpointAction->setToolTip(tr("The breakpoint at the cursor's line - its condition, hits, a logpoint's "
                                            "message - edited (one added when there is none); a right-click in the line "
                                            "numbers' margin does it too"));
  connect(pythonEditBreakpointAction, &QAction::triggered, this, &QucsApp::slotPythonEditBreakpoint);

  pythonBreakpointAction = new QAction(tr("Toggle Breakpoint"), this);
  pythonBreakpointAction->setObjectName(QStringLiteral("pythonBreakpoint"));
  pythonBreakpointAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_F9));
  pythonBreakpointAction->setToolTip(tr("A breakpoint set at the cursor's line, or taken away (a click in the line "
                                        "numbers' margin does it too)"));
  connect(pythonBreakpointAction, &QAction::triggered, this, &QucsApp::slotPythonToggleBreakpoint);

  // Lines run in the Python Shell, in its variables: the selection or the
  // line, the cell (# %%).
  pythonRunSelectionAction = new QAction(tr("Run Selection or Line"), this);
  pythonRunSelectionAction->setObjectName(QStringLiteral("pythonRunSelection"));
  pythonRunSelectionAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Return));
  pythonRunSelectionAction->setToolTip(tr("The lines selected - or the cursor's line, and on to the next - run in the "
                                          "Python Shell, in its variables"));
  connect(pythonRunSelectionAction, &QAction::triggered, this, &QucsApp::slotPythonRunSelection);
  pythonRunCellAction = new QAction(tr("Run Cell"), this);
  pythonRunCellAction->setObjectName(QStringLiteral("pythonRunCell"));
  pythonRunCellAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
  pythonRunCellAction->setToolTip(tr("The cell the cursor is in - from a line # %% to the next - run in the Python "
                                     "Shell, in its variables"));
  connect(pythonRunCellAction, &QAction::triggered, this, [this] { slotPythonRunCell(false); });
  pythonRunCellAdvanceAction = new QAction(tr("Run Cell and Advance"), this);
  pythonRunCellAdvanceAction->setObjectName(QStringLiteral("pythonRunCellAdvance"));
  pythonRunCellAdvanceAction->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Return));
  pythonRunCellAdvanceAction->setToolTip(tr("The cell run in the Python Shell, and the cursor on to the next"));
  connect(pythonRunCellAdvanceAction, &QAction::triggered, this, [this] { slotPythonRunCell(true); });

  pythonToolbar->addSeparator();
  pythonCheckAction = new QAction(QIcon(QStringLiteral(":/bitmaps/svg/python_check.svg")), tr("Check"), this);
  pythonCheckAction->setObjectName(QStringLiteral("pythonCheck"));
  connect(pythonCheckAction, &QAction::triggered, this, &QucsApp::slotPythonCheck);
  pythonToolbar->addAction(pythonCheckAction);

  pythonLineEndsAction =
      new QAction(QIcon(QStringLiteral(":/bitmaps/svg/python_lineends.svg")), tr("Messages at Line Ends"), this);
  pythonLineEndsAction->setObjectName(QStringLiteral("pythonMessagesAtLineEnds"));
  pythonLineEndsAction->setCheckable(true);
  pythonLineEndsAction->setChecked(PythonDoc::messagesAtLineEnds());
  pythonLineEndsAction->setToolTip(tr("Messages at Line Ends: each line's first error or warning written after its "
                                      "text as well, faintly"));
  connect(pythonLineEndsAction, &QAction::toggled, this, &QucsApp::slotPythonLineEnds);
  pythonToolbar->addAction(pythonLineEndsAction);

  // Completion: asked for (Ctrl+Space - the Control key on a Mac, the
  // Command key's being Spotlight's), and as one types.
  pythonCompleteAction = new QAction(tr("Show Completions"), this);
  pythonCompleteAction->setObjectName(QStringLiteral("pythonComplete"));
#ifdef Q_OS_MACOS
  pythonCompleteAction->setShortcuts({QKeySequence(Qt::META | Qt::Key_Space), QKeySequence(Qt::CTRL | Qt::Key_Space)});
#else
  pythonCompleteAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Space));
#endif
  connect(pythonCompleteAction, &QAction::triggered, this, &QucsApp::slotPythonComplete);
  pythonAsYouTypeAction = new QAction(tr("Complete as You Type"), this);
  pythonAsYouTypeAction->setObjectName(QStringLiteral("pythonCompleteAsYouType"));
  pythonAsYouTypeAction->setCheckable(true);
  pythonAsYouTypeAction->setChecked(PythonDoc::completeAsYouType());
  pythonAsYouTypeAction->setToolTip(tr("The words that complete a name offered as you type it: after two letters, or a "
                                       "dot. Off: when asked for (Show Completions)."));
  connect(pythonAsYouTypeAction, &QAction::toggled, this, [](bool on) { PythonDoc::setCompleteAsYouType(on); });
  pythonSignatureAction = new QAction(tr("Show Signature"), this);
  pythonSignatureAction->setObjectName(QStringLiteral("pythonSignature"));
#ifdef Q_OS_MACOS
  pythonSignatureAction->setShortcuts({QKeySequence(Qt::META | Qt::SHIFT | Qt::Key_Space), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Space)});
#else
  pythonSignatureAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Space));
#endif
  pythonSignatureAction->setToolTip(tr("The parameters of the call the cursor is in, above it (shown as you type a "
                                       "bracket or a comma, too)"));
  connect(pythonSignatureAction, &QAction::triggered, this, &QucsApp::slotPythonSignature);

  pythonDefinitionAction = new QAction(tr("Go to Definition"), this);
  pythonDefinitionAction->setObjectName(QStringLiteral("pythonDefinition"));
  pythonDefinitionAction->setShortcut(QKeySequence(Qt::Key_F12));
  pythonDefinitionAction->setToolTip(tr("Where the name at the cursor is defined (F12, or Ctrl+click on it): in the "
                                        "script, a module beside it, or Python's library (shown read-only)"));
  connect(pythonDefinitionAction, &QAction::triggered, this, &QucsApp::slotPythonDefinition);
  pythonBackAction = new QAction(tr("Go Back"), this);
  pythonBackAction->setObjectName(QStringLiteral("pythonBack"));
#ifdef Q_OS_MACOS
  pythonBackAction->setShortcut(QKeySequence(Qt::META | Qt::Key_Minus));
#else
  pythonBackAction->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Left));
#endif
  pythonBackAction->setToolTip(tr("Back where Go to Definition left"));
  connect(pythonBackAction, &QAction::triggered, this, &QucsApp::slotPythonBack);

  pythonReferencesAction = new QAction(tr("Find All References"), this);
  pythonReferencesAction->setObjectName(QStringLiteral("pythonReferences"));
  pythonReferencesAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F12));
  pythonReferencesAction->setToolTip(tr("Where the name at the cursor is used, and defined, listed on the References tab "
                                        "(Shift+F12): in the script - and the modules beside it with jedi"));
  connect(pythonReferencesAction, &QAction::triggered, this, &QucsApp::slotPythonReferences);
  pythonRenameAction = new QAction(tr("Rename Symbol..."), this);
  pythonRenameAction->setObjectName(QStringLiteral("pythonRename"));
  pythonRenameAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F2));
  pythonRenameAction->setToolTip(tr("The name at the cursor renamed wherever it is that name (Shift+F2) - a function's "
                                    "local alone, a module's everywhere it is used; with jedi in the modules beside it "
                                    "too, and attributes. Undo takes it back."));
  connect(pythonRenameAction, &QAction::triggered, this, &QucsApp::slotPythonRename);
  pythonSymbolsAction = new QAction(tr("Go to Symbol..."), this);
  pythonSymbolsAction->setObjectName(QStringLiteral("pythonSymbols"));
  pythonSymbolsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_F12));
  pythonSymbolsAction->setToolTip(tr("A class, a function, a method or a variable of the script - or of the scripts "
                                     "beside it - picked by typing its letters (Ctrl+F12)"));
  connect(pythonSymbolsAction, &QAction::triggered, this, &QucsApp::slotPythonSymbols);

  // Folding.
  const auto foldAction = [this](QAction *&action, const char *name, const QString &text, const QString &tip,
                                 const QList<QKeySequence> &keys) {
    action = new QAction(text, this);
    action->setObjectName(QLatin1String(name));
    action->setShortcuts(keys);
    action->setToolTip(tip);
  };
  foldAction(pythonFoldAction, "pythonFold", tr("Fold"), tr("The block the cursor is in folded away: a function, a class, "
                                                            "a loop, a cell (Ctrl+Shift+[)"),
             {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_BracketLeft), QKeySequence(Qt::CTRL | Qt::Key_BraceLeft)});
  foldAction(pythonUnfoldAction, "pythonUnfold", tr("Unfold"), tr("The fold at the cursor opened (Ctrl+Shift+])"),
             {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_BracketRight), QKeySequence(Qt::CTRL | Qt::Key_BraceRight)});
  foldAction(pythonFoldAllAction, "pythonFoldAll", tr("Fold All"), tr("Every function, class and cell folded"), {});
  foldAction(pythonUnfoldAllAction, "pythonUnfoldAll", tr("Unfold All"), tr("Every fold opened"), {});
  connect(pythonFoldAction, &QAction::triggered, this, [this] {
    if (PythonDoc *py = currentPythonDoc())
      if (const int line = py->foldAround(py->textCursor().blockNumber() + 1, true); line > 0) py->fold(line);
  });
  connect(pythonUnfoldAction, &QAction::triggered, this, [this] {
    if (PythonDoc *py = currentPythonDoc())
      if (const int line = py->foldAround(py->textCursor().blockNumber() + 1, false); line > 0) py->unfold(line);
  });
  connect(pythonFoldAllAction, &QAction::triggered, this, [this] {
    if (PythonDoc *py = currentPythonDoc()) py->foldAll();
  });
  connect(pythonUnfoldAllAction, &QAction::triggered, this, [this] {
    if (PythonDoc *py = currentPythonDoc()) py->unfoldAll();
  });
  pythonAutoCloseAction = new QAction(tr("Close Brackets and Quotes"), this);
  pythonAutoCloseAction->setObjectName(QStringLiteral("pythonAutoClose"));
  pythonAutoCloseAction->setCheckable(true);
  pythonAutoCloseAction->setChecked(PythonDoc::autoClose());
  pythonAutoCloseAction->setToolTip(tr("A bracket or a quote typed closed at once, the cursor between them; a selection "
                                       "wrapped in them; a closing one typed over the one there"));
  connect(pythonAutoCloseAction, &QAction::toggled, this, [](bool on) { PythonDoc::setAutoClose(on); });

  // Matplotlib's figures in the Python Plots pane.
  pythonInlinePlotsAction = new QAction(tr("Plots in Qucs-S"), this);
  pythonInlinePlotsAction->setObjectName(QStringLiteral("pythonInlinePlots"));
  pythonInlinePlotsAction->setCheckable(true);
  pythonInlinePlotsAction->setChecked(qucs_s::python::inlinePlots());
  pythonInlinePlotsAction->setToolTip(tr("matplotlib's figures shown in the Python Plots pane, not windows of their own - "
                                         "for scripts run and debugged from now on, and the Python Shell once it starts again"));
  connect(pythonInlinePlotsAction, &QAction::toggled, this, [this](bool on) {
    qucs_s::python::setInlinePlots(on);
    updateConsolePrograms();   // (the Python Shell's, when it starts again)
  });

  // The type checker run after each check.
  pythonTypeCheckers = new QActionGroup(this);
  pythonTypeCheckers->setExclusive(true);
  const QString checker = PythonDoc::typeChecker();
  for (const auto &[key, text] : {std::pair<QString, QString>{QStringLiteral("auto"), tr("Automatic (mypy, else pyright)")},
                                  {QStringLiteral("mypy"), tr("mypy")},
                                  {QStringLiteral("pyright"), tr("pyright")},
                                  {QStringLiteral("off"), tr("Off")}}) {
    QAction *a = pythonTypeCheckers->addAction(text);
    a->setObjectName(QStringLiteral("pythonTypeChecker_") + key);
    a->setData(key);
    a->setCheckable(true);
    a->setChecked(key == checker);
  }
  connect(pythonTypeCheckers, &QActionGroup::triggered, this, [this](QAction *a) {
    PythonDoc::setTypeChecker(a->data().toString());
    updatePythonToolbar();
  });

  pythonFormatAction = new QAction(tr("Format Document"), this);
  pythonFormatAction->setObjectName(QStringLiteral("pythonFormat"));
  pythonFormatAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::ALT | Qt::Key_F));
  pythonFormatAction->setToolTip(tr("The script formatted by ruff (or black) installed for its Python - one edit, "
                                    "Undo takes it back"));
  connect(pythonFormatAction, &QAction::triggered, this, &QucsApp::slotPythonFormat);
  pythonImportsAction = new QAction(tr("Organize Imports"), this);
  pythonImportsAction->setObjectName(QStringLiteral("pythonOrganizeImports"));
  pythonImportsAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::ALT | Qt::Key_O));
  pythonImportsAction->setToolTip(tr("The imports sorted and grouped by ruff (its isort rules), else isort, installed for "
                                     "the script's Python (Shift+Alt+O) - one edit, Undo takes it back"));
  connect(pythonImportsAction, &QAction::triggered, this, [this] {
    if (PythonDoc *py = currentPythonDoc()) py->organizeImports();
  });
  pythonFormatSelectionAction = new QAction(tr("Format Selection"), this);
  pythonFormatSelectionAction->setObjectName(QStringLiteral("pythonFormatSelection"));
  pythonFormatSelectionAction->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_F));
  pythonFormatSelectionAction->setToolTip(tr("The lines selected - or the cursor's - formatted by ruff (or black), the rest "
                                             "left as it is (Ctrl+Alt+F)"));
  connect(pythonFormatSelectionAction, &QAction::triggered, this, [this] {
    if (PythonDoc *py = currentPythonDoc()) py->formatSelection();
  });
  pythonFormatOnSaveAction = new QAction(tr("Format on Save"), this);
  pythonFormatOnSaveAction->setObjectName(QStringLiteral("pythonFormatOnSave"));
  pythonFormatOnSaveAction->setCheckable(true);
  pythonFormatOnSaveAction->setChecked(PythonDoc::formatOnSave());
  pythonFormatOnSaveAction->setToolTip(tr("A script formatted (ruff, or black) each time it is saved"));
  connect(pythonFormatOnSaveAction, &QAction::toggled, this, [](bool on) { PythonDoc::setFormatOnSave(on); });
  pythonQuickFixAction = new QAction(tr("Quick Fix..."), this);
  pythonQuickFixAction->setObjectName(QStringLiteral("pythonQuickFix"));
  pythonQuickFixAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::ALT | Qt::Key_Return));
  pythonQuickFixAction->setToolTip(tr("The fixes of the problems on the cursor's line (Alt+Shift+Return, or the light "
                                      "bulb in the margin): ruff's fix, an import for a name not defined, each ignored on "
                                      "the line"));
  connect(pythonQuickFixAction, &QAction::triggered, this, &QucsApp::slotPythonQuickFix);

  pythonFixAction = new QAction(tr("Fix Problems"), this);
  pythonFixAction->setObjectName(QStringLiteral("pythonFix"));
  pythonFixAction->setToolTip(tr("What ruff can fix of what it finds (imports not used and more) fixed - one edit, "
                                 "Undo takes it back"));
  connect(pythonFixAction, &QAction::triggered, this, &QucsApp::slotPythonFix);

  // The editor's keys work in a script alone (connectPythonDoc()): not
  // Shift+Return in the Python Shell's line, nor Alt+Left in another text.
  for (QAction *a : {pythonRunSelectionAction, pythonRunCellAction, pythonRunCellAdvanceAction, pythonDefinitionAction,
                     pythonBackAction, pythonBreakpointAction, pythonFormatAction, pythonSignatureAction, pythonReferencesAction,
                     pythonRenameAction, pythonSymbolsAction, pythonFoldAction, pythonUnfoldAction, pythonRunToCursorAction,
                     pythonQuickFixAction, pythonImportsAction, pythonFormatSelectionAction})
    a->setShortcutContext(Qt::WidgetShortcut);

  // The same in Simulation > Python, above the simulators' settings: for
  // the keyboard, and for Claude's trigger_action (it runs menu actions).
  auto *pythonMenu = new QMenu(tr("&Python"), simMenu);
  pythonMenu->addAction(pythonRunAction);
  pythonMenu->addAction(pythonDebugAction);
  pythonMenu->addAction(pythonStopAction);
  pythonMenu->addAction(pythonInterruptAction);
  pythonMenu->addAction(pythonShellAction);
  pythonMenu->addAction(pythonRunSettingsAction);
  pythonMenu->addSeparator();
  pythonMenu->addAction(pythonRunSelectionAction);
  pythonMenu->addAction(pythonRunCellAction);
  pythonMenu->addAction(pythonRunCellAdvanceAction);
  pythonMenu->addSeparator();
  pythonMenu->addAction(pythonContinueAction);
  pythonMenu->addAction(pythonPauseAction);
  pythonMenu->addAction(pythonStepOverAction);
  pythonMenu->addAction(pythonStepIntoAction);
  pythonMenu->addAction(pythonStepOutAction);
  pythonMenu->addAction(pythonRunToCursorAction);
  pythonMenu->addAction(pythonBreakpointAction);
  pythonMenu->addAction(pythonEditBreakpointAction);
  pythonMenu->addAction(pythonRaisedAction);
  pythonMenu->addAction(pythonLibraryAction);
  pythonMenu->addSeparator();
  pythonMenu->addAction(pythonCheckAction);
  pythonMenu->addAction(pythonLineEndsAction);
  QMenu *types = pythonMenu->addMenu(tr("Type Checker"));
  types->setObjectName(QStringLiteral("pythonTypeCheckerMenu"));
  types->addActions(pythonTypeCheckers->actions());
  pythonMenu->addAction(pythonQuickFixAction);
  pythonMenu->addAction(pythonFormatAction);
  pythonMenu->addAction(pythonFormatSelectionAction);
  pythonMenu->addAction(pythonImportsAction);
  pythonMenu->addAction(pythonFixAction);
  pythonMenu->addAction(pythonFormatOnSaveAction);
  pythonMenu->addSeparator();
  pythonMenu->addAction(pythonCompleteAction);
  pythonMenu->addAction(pythonSignatureAction);
  pythonMenu->addAction(pythonAsYouTypeAction);
  pythonMenu->addAction(pythonAutoCloseAction);
  pythonMenu->addSeparator();
  pythonMenu->addAction(pythonDefinitionAction);
  pythonMenu->addAction(pythonBackAction);
  pythonMenu->addAction(pythonReferencesAction);
  pythonMenu->addAction(pythonRenameAction);
  pythonMenu->addAction(pythonSymbolsAction);
  pythonMenu->addSeparator();
  pythonMenu->addAction(pythonFoldAction);
  pythonMenu->addAction(pythonUnfoldAction);
  pythonMenu->addAction(pythonFoldAllAction);
  pythonMenu->addAction(pythonUnfoldAllAction);
  pythonMenu->addSeparator();
  pythonMenu->addAction(pythonInlinePlotsAction);
  // Python Plots and Python Variables shown (their docks: initPythonConsole()).
  pythonToolbar->addSeparator();
  for (auto [dock, icon, name, tip] :
       {std::tuple<QDockWidget *, const char *, const char *, QString>{
            a_pythonPlotsDock, ":/bitmaps/svg/python_plots.svg", "pythonShowPlots",
            tr("Python Plots: the figures of matplotlib a script draws (plt.show())")},
        {a_pythonVariablesDock, ":/bitmaps/svg/python_variables.svg", "pythonShowVariables",
         tr("Python Variables: the Python Shell's variables, an array or a list shown as a table")}}) {
    if (dock == nullptr) continue;
    QAction *toggle = dock->toggleViewAction();
    toggle->setIcon(QIcon(QLatin1String(icon)));
    toggle->setObjectName(QLatin1String(name));
    toggle->setToolTip(tip);
    pythonToolbar->addAction(toggle);
    pythonMenu->addAction(toggle);
  }
  a_pythonMenu = pythonMenu;
  simMenu->insertMenu(simSettings, pythonMenu);
  simMenu->insertSeparator(simSettings);

  // Hidden or shown from the Toolbars menu (or the toolbars' own menu):
  // kept, for the next Python script in front.
  connect(pythonToolbar->toggleViewAction(), &QAction::triggered, this,
          [](bool on) { _settings::Get().setItem<bool>("PythonToolbar", on); });
  pythonToolbar->hide();
}

// ----------------------------------------------------------
// The console of a script's run: a dock beside the Python Shell (initView()).
void QucsApp::initPythonConsole()
{
  pythonRun = new PythonRunConsole;
  pythonRunDock = new QDockWidget(tr("Python Run"), this);
  pythonRunDock->setObjectName(QStringLiteral("PythonRunDock"));
  pythonRunDock->setWidget(pythonRun);
  pythonRunDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
  addDockWidget(Qt::BottomDockWidgetArea, pythonRunDock);
  tabifyDockWidget(pythonDock, pythonRunDock);
  pythonRunDock->hide();
  connect(pythonRun, &PythonRunConsole::started, this, &QucsApp::updatePythonToolbar);
  connect(pythonRun, &PythonRunConsole::finished, this, &QucsApp::updatePythonToolbar);
  connect(pythonRun, &PythonRunConsole::debuggingChanged, this, [this] {
    if (!pythonRun->isPaused()) showPythonExecution(QString(), 0, true);   // (running, or ended: no line)
    updatePythonToolbar();
  });
  connect(pythonRun, &PythonRunConsole::locationShown, this, &QucsApp::showPythonExecution);
  connect(pythonRun, &PythonRunConsole::locationRequested, this,
          [this](const QString &file, int line) { showTextPlace(nullptr, file, line, 0); });
  connect(messageDock, &MessageDock::lineRequested, this,
          [this](QWidget *document, int line, int column) { showTextPlace(document, QString(), line, column); });
  connect(messageDock, &MessageDock::placeRequested, this,
          [this](const QString &file, int line, int column) { showTextPlace(nullptr, file, line, column); });

  // The figures scripts draw, and the Python Shell's variables: docks at
  // the right, shown as a figure comes - or from the Python toolbar.
  a_pythonPlots = new PythonPlotsPane;
  a_pythonPlotsDock = new QDockWidget(tr("Python Plots"), this);
  a_pythonPlotsDock->setObjectName(QStringLiteral("PythonPlotsDock"));
  a_pythonPlotsDock->setWidget(a_pythonPlots);
  addDockWidget(Qt::RightDockWidgetArea, a_pythonPlotsDock);
  a_pythonPlotsDock->hide();
  a_pythonVariables = new PythonVariablesPane;
  a_pythonVariablesDock = new QDockWidget(tr("Python Variables"), this);
  a_pythonVariablesDock->setObjectName(QStringLiteral("PythonVariablesDock"));
  a_pythonVariablesDock->setWidget(a_pythonVariables);
  addDockWidget(Qt::RightDockWidgetArea, a_pythonVariablesDock);
  tabifyDockWidget(a_pythonPlotsDock, a_pythonVariablesDock);
  a_pythonVariablesDock->hide();
  // (Their actions on the Python toolbar and in its menu: initPythonToolbar(),
  // which comes after.)

  a_pythonExchange = new qucs_s::python::Exchange(this);
  connect(a_pythonExchange, &qucs_s::python::Exchange::plotArrived, this, [this](const QString &image, const QJsonObject &about) {
    a_pythonPlots->addPlot(image, about);
    if (!a_pythonPlotsDock->isVisible()) a_pythonPlotsDock->show();
    a_pythonPlotsDock->raise();
  });
  connect(a_pythonExchange, &qucs_s::python::Exchange::variablesChanged, a_pythonVariables, &PythonVariablesPane::setVariables);
  connect(a_pythonExchange, &qucs_s::python::Exchange::shellAnswered, this, [this](const QJsonObject &answer) {
    const QString kind = answer.value(QStringLiteral("kind")).toString();
    if (kind == QLatin1String("variables")) {
      a_pythonVariables->setVariables(answer.value(QStringLiteral("variables")).toArray());
    } else if (kind == QLatin1String("table")) {
      if (PythonDataViewer *viewer = a_shellTables.take(answer.value(QStringLiteral("id")).toInt())) viewer->answer(answer);
    }
  });
  connect(a_pythonExchange, &qucs_s::python::Exchange::displayRequested, this, [this](const QJsonObject &request) {
    const QString why = showPythonDisplay(request);
    if (!why.isEmpty()) statusBar()->showMessage(why, 10000);
  });
  connect(a_pythonVariables, &PythonVariablesPane::tableRequested, this, &QucsApp::viewShellTable);
  connect(a_pythonVariables, &PythonVariablesPane::refreshRequested, this,
          [this] { a_pythonExchange->askShell({{QStringLiteral("kind"), QStringLiteral("variables")}}); });
  connect(pythonRun, &PythonRunConsole::finished, a_pythonExchange, &qucs_s::python::Exchange::scan);
  // The shell ended (or started again): its variables gone.
  connect(pythonShell, &ProcessConsole::finished, a_pythonVariables, [this] { a_pythonVariables->setVariables({}); });

  // The debugger's: a value as a table, a value under the mouse.
  connect(pythonRun, &PythonRunConsole::tableRequested, this, [this](int handle, const QString &name) {
    auto *viewer = new PythonDataViewer(name, nullptr, this);
    viewer->model()->setFetch([this, handle, made = QPointer<PythonDataViewer>(viewer)](int start, int count) {
      if (made.isNull()) return;
      if (const int id = pythonRun->requestData(handle, QString(), start, count); id > 0) a_debugTables.insert(id, made);
      else made->answer({{QStringLiteral("error"), tr("The script went on: its values are no longer there.")}});
    });
    viewer->show();
    viewer->model()->fetchFirst();   // (the first rows - and its shape)
  });
  connect(pythonRun, &PythonRunConsole::dataArrived, this, [this](int id, const QJsonObject &table) {
    if (PythonDataViewer *viewer = a_debugTables.take(id)) viewer->answer(table);
  });
  connect(pythonRun, &PythonRunConsole::inspected, this,
          [this](int id, const QString &expression, const QString &value, const QString &error) {
            if (PythonDoc *py = a_pythonValues.take(id))
              py->showValue(expression, error.isEmpty() ? QStringLiteral("%1 = %2").arg(expression, value) : QString());
          });
}

PythonDataViewer *QucsApp::viewShellTable(const QString &expression)
{
  auto *viewer = new PythonDataViewer(expression, nullptr, this);
  if (!pythonShell->isRunning()) {
    viewer->answer({{QStringLiteral("error"), tr("The Python Shell is not running: its variables are gone.")}});
    viewer->show();
    return viewer;
  }
  viewer->model()->setFetch([this, expression, made = QPointer<PythonDataViewer>(viewer)](int start, int count) {
    if (made.isNull()) return;
    const int id = a_pythonExchange->askShell({{QStringLiteral("kind"), QStringLiteral("table")},
                                               {QStringLiteral("expression"), expression},
                                               {QStringLiteral("start"), start},
                                               {QStringLiteral("count"), count}});
    if (id > 0) a_shellTables.insert(id, made);
  });
  viewer->show();
  viewer->model()->fetchFirst();
  return viewer;
}

void QucsApp::updatePythonToolbar()
{
  if (pythonToolbar == nullptr) return;
  PythonDoc *py = currentPythonDoc();
  if (py == nullptr) pythonToolbar->hide();
  else if (_settings::Get().item<bool>("PythonToolbar")) pythonToolbar->show();

  {
    const QSignalBlocker block(pythonInterpreters);
    pythonInterpreters->clear();
    if (py != nullptr) {
      const QString project = !ProjName.isEmpty() ? QucsSettings.QucsWorkDir.absolutePath() : QString();
      QList<qucs_s::python::Interpreter> found = qucs_s::python::interpretersFor(
          py->getDocName(), project, _settings::Get().item<QStringList>("PythonInterpreters"));
      const QString current = py->interpreter();
      if (std::none_of(found.cbegin(), found.cend(), [&](const qucs_s::python::Interpreter &i) { return i.path == current; }))
        found.prepend({current, tr("%1 (chosen)").arg(QFileInfo(current).fileName())});
      for (const qucs_s::python::Interpreter &i : std::as_const(found)) {
        pythonInterpreters->addItem(i.label, i.path);
        pythonInterpreters->setItemData(pythonInterpreters->count() - 1, QDir::toNativeSeparators(i.path), Qt::ToolTipRole);
      }
      pythonInterpreters->addItem(tr("Browse..."), QString());
      pythonInterpreters->setCurrentIndex(pythonInterpreters->findData(current));
    }
    pythonInterpreters->setEnabled(py != nullptr);
  }
  pythonRunAction->setEnabled(py != nullptr);
  pythonShellAction->setEnabled(py != nullptr);
  pythonCheckAction->setEnabled(py != nullptr);
  pythonCompleteAction->setEnabled(py != nullptr);
  pythonCompleteAction->setToolTip(tr("Show Completions: the words that complete the name at the cursor") +
                                   (py != nullptr ? QStringLiteral("\n") + py->completedBy() : QString()));
  const bool editable = py != nullptr && !py->isReadOnly();
  for (QAction *a : {pythonSignatureAction, pythonDefinitionAction, pythonBreakpointAction, pythonRunSelectionAction,
                     pythonRunCellAction, pythonRunCellAdvanceAction, pythonReferencesAction, pythonSymbolsAction,
                     pythonEditBreakpointAction, pythonFoldAction, pythonUnfoldAction, pythonFoldAllAction, pythonUnfoldAllAction})
    a->setEnabled(py != nullptr);
  pythonFormatAction->setEnabled(editable);
  pythonFixAction->setEnabled(editable);
  for (QAction *a : {pythonQuickFixAction, pythonImportsAction, pythonFormatSelectionAction}) a->setEnabled(editable);
  pythonRunSettingsAction->setEnabled(py != nullptr && !py->getDocName().isEmpty());
  if (py != nullptr && !py->getDocName().isEmpty()) {   // (the Run button says how it runs)
    const qucs_s::python::RunSettings how = qucs_s::python::runSettingsFor(py->getDocName());
    QString tip = tr("Run (F2): the script saved and run with this Python in its folder, its output in the Python Run console");
    if (!how.arguments.isEmpty()) tip += QLatin1Char('\n') + tr("Arguments: %1").arg(how.arguments);
    if (!how.folder.isEmpty()) tip += QLatin1Char('\n') + tr("Working folder: %1").arg(how.folder);
    if (!how.environment.isEmpty() || !how.envFile.isEmpty()) tip += QLatin1Char('\n') + tr("With its environment (Run Settings)");
    pythonRunAction->setToolTip(tip);
  }
  pythonRenameAction->setEnabled(editable);
  pythonBackAction->setEnabled(!a_pythonBack.isEmpty());
  // (Debug, Pause and Continue share Ctrl+F2: one of them enabled at a
  // time.)
  const bool debugging = pythonRun != nullptr && pythonRun->isDebugging();
  const bool paused = debugging && pythonRun->isPaused();
  pythonDebugAction->setEnabled(py != nullptr && !debugging);
  for (QAction *a : {pythonContinueAction, pythonStepOverAction, pythonStepIntoAction, pythonStepOutAction}) a->setEnabled(paused);
  pythonPauseAction->setEnabled(debugging && !paused && !pythonRun->pausing());
  pythonInterruptAction->setEnabled(pythonRun != nullptr && pythonRun->isRunning() && !paused && PythonRunConsole::canInterrupt());
  pythonRunToCursorAction->setEnabled(py != nullptr && !py->getDocName().isEmpty() && (!debugging || paused));
  // Pause in Continue's place while it runs (on the toolbar and in the
  // menu; the one hidden has no key).
  pythonPauseAction->setVisible(debugging && !paused);
  pythonContinueAction->setVisible(!debugging || paused);
  pythonStopAction->setEnabled(pythonRun != nullptr && pythonRun->isRunning());
  pythonCheckAction->setToolTip(tr("Check: the script checked now, and the Problems tab brought up") +
                                (py != nullptr ? QStringLiteral("\n") + py->checkedBy() : QString()));
}

void QucsApp::pythonChecked(PythonDoc *py)
{
  const bool front = py == currentPythonDoc();
  if (front || messageDock->textProblemsDocument() == py) {
    messageDock->showTextProblems(py, py->diagnostics(), py->checkedBy(), front && a_pythonCheckRaises, py->lastCheck().failure);
    if (front) a_pythonCheckRaises = false;
  }
  // No Python to check with: said once, not after each edit.
  if (!py->lastCheck().failure.isEmpty() && !a_pythonFailureSaid) {
    a_pythonFailureSaid = true;
    statusBar()->showMessage(py->lastCheck().failure, 10000);
  }
  if (front) updatePythonToolbar();
}

bool QucsApp::runPython(PythonDoc *py)
{
  if (py == nullptr) return false;
  if ((py->getDocName().isEmpty() || py->getDocChanged()) && !saveFile(py)) return false;
  pythonRunDock->show();
  pythonRunDock->raise();
  const bool ran = pythonRun->run(py->interpreter(), py->getDocName());
  updatePythonToolbar();
  return ran;
}

bool QucsApp::runPythonInShell(PythonDoc *py)
{
  if (py == nullptr) return false;
  if ((py->getDocName().isEmpty() || py->getDocChanged()) && !saveFile(py)) return false;
  const QFileInfo info(py->getDocName());
  // In its folder, as Run runs it, the folder first on sys.path; its own
  // file name; its bytes compiled, so its coding line is read as Python
  // reads one. The helpers' names are gone before it runs.
  const QString folder = ProcessConsole::quotedForPython(info.absolutePath());
  const QString file = ProcessConsole::quotedForPython(info.absoluteFilePath());
  const QString line =
      QStringLiteral("import os as _qucs_os, sys as _qucs_sys; _qucs_os.chdir(%1); "
                     "_qucs_sys.path.insert(0, %1) if %1 not in _qucs_sys.path else None; del _qucs_os, _qucs_sys; "
                     "__file__ = %2; exec(compile(open(__file__, 'rb').read(), __file__, 'exec'))")
          .arg(folder, file);
  pythonDock->show();
  pythonDock->raise();
  pythonShell->sendLine(line);
  return true;
}

bool QucsApp::runPythonLines(PythonDoc *py, int firstLine, const QString &code, const QString &what)
{
  if (py == nullptr || code.trimmed().isEmpty()) return false;
  // What the shell runs, written for it (qucs_s::python::moduleFolder()'s
  // _qucs_shell.py reads it, and takes it away): the shell's line stays
  // short, as typed.
  const QString folder = qucs_s::python::moduleFolder();
  if (folder.isEmpty()) return false;
  const QString path = QDir(folder).filePath(QStringLiteral("jobs/%1.json").arg(++a_pythonJobs));
  const QFileInfo script(py->getDocName());
  const QJsonObject job{{QStringLiteral("file"), py->getDocName().isEmpty() ? QString() : script.absoluteFilePath()},
                        {QStringLiteral("folder"), py->getDocName().isEmpty() ? QString() : script.absolutePath()},
                        {QStringLiteral("line"), firstLine},
                        {QStringLiteral("code"), code}};
  QFile out(path);
  if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(QJsonDocument(job).toJson(QJsonDocument::Compact)) < 0)
    return false;
  out.close();
  pythonDock->show();
  pythonDock->raise();
  pythonShell->sendLine(QStringLiteral("__import__('_qucs_shell').run(globals(), %1)  # %2")
                            .arg(ProcessConsole::quotedForPython(path), what));
  return true;
}

bool QucsApp::debugPython(PythonDoc *py, int runToLine)
{
  if (py == nullptr) return false;
  if ((py->getDocName().isEmpty() || py->getDocChanged()) && !saveFile(py)) return false;
  // The breakpoints of every script open, with their conditions, hits and
  // messages.
  QHash<QString, QList<qucs_s::python::Breakpoint>> breakpoints;
  for (QucsDoc *doc : allDocuments())
    if (auto *script = dynamic_cast<PythonDoc *>(doc); script != nullptr && !script->getDocName().isEmpty())
      if (const QList<qucs_s::python::Breakpoint> list = script->breakpointList(); !list.isEmpty())
        breakpoints.insert(QFileInfo(script->getDocName()).absoluteFilePath(), list);
  pythonRunDock->show();
  pythonRunDock->raise();
  const std::pair<QString, int> runTo =
      runToLine > 0 ? std::pair<QString, int>{QFileInfo(py->getDocName()).absoluteFilePath(), runToLine} : std::pair<QString, int>{};
  pythonRun->setDebugLibrary(pythonLibraryAction->isChecked());
  const bool started = pythonRun->debug(py->interpreter(), py->getDocName(), breakpoints, pythonRaisedAction->isChecked(), runTo);
  updatePythonToolbar();
  return started;
}

void QucsApp::showPythonExecution(const QString &file, int line, bool top)
{
  // The script stopped in, in front, at its line; the line marked there
  // and nowhere else.
  PythonDoc *shown = nullptr;
  if (!file.isEmpty() && line > 0) {
    showTextPlace(nullptr, file, line, 0);
    shown = currentPythonDoc();
    if (shown != nullptr && QFileInfo(shown->getDocName()).canonicalFilePath() != QFileInfo(file).canonicalFilePath())
      shown = nullptr;
    // Python's own, or a package's (Debug Library Code): read-only.
    if (shown != nullptr && pythonRun != nullptr && !shown->isLibraryFile())
      for (const PythonRunConsole::Frame &f : pythonRun->stack())
        if (f.library && f.file == file) {
          shown->setLibraryFile(true);
          break;
        }
  }
  for (QucsDoc *doc : allDocuments())
    if (auto *script = dynamic_cast<PythonDoc *>(doc))
      script->setExecutionLine(script == shown ? line : 0, top);
}

void QucsApp::connectPythonDoc(PythonDoc *py)
{
  py->addActions({pythonRunSelectionAction, pythonRunCellAction, pythonRunCellAdvanceAction, pythonDefinitionAction,
                  pythonBackAction, pythonBreakpointAction, pythonFormatAction, pythonSignatureAction, pythonReferencesAction,
                  pythonRenameAction, pythonSymbolsAction, pythonFoldAction, pythonUnfoldAction, pythonRunToCursorAction,
                  pythonQuickFixAction, pythonImportsAction, pythonFormatSelectionAction});
  connect(py, &PythonDoc::checkFinished, this, [this, py] { pythonChecked(py); });
  connect(py, &PythonDoc::typeCheckFinished, this, [this, py] { pythonChecked(py); });
  connect(py, &PythonDoc::definitionAnswered, this, [this, py] { pythonDefinitionFound(py); });
  connect(py, &PythonDoc::formatted, this, [this](const QString &said) { statusBar()->showMessage(said, 8000); });
  // A breakpoint set, changed or taken away while it is debugged: the
  // debugger told.
  connect(py, &PythonDoc::breakpointsChanged, this, [this, py] {
    if (pythonRun != nullptr && pythonRun->isDebugging() && !py->getDocName().isEmpty())
      pythonRun->setBreakpoints(QFileInfo(py->getDocName()).absoluteFilePath(), py->breakpointList());
  });
  connect(py, &PythonDoc::referencesAnswered, this, [this, py] {
    const qucs_s::python::Answer &found = py->lastReferences();
    if (!found.valid || found.references.isEmpty()) {
      statusBar()->showMessage(tr("No references found for the name at the cursor."), 6000);
      return;
    }
    QList<MessageDock::Reference> list;
    for (const qucs_s::python::Reference &r : found.references)
      list.append({r.file, r.line, r.column, r.end, r.text, r.definition});
    QString scope;
    if (found.scope == QLatin1String("function")) scope = tr(" - a local of its function");
    else if (found.scope == QLatin1String("attribute")) scope = tr(" - every attribute of this name in the script (with jedi: of its object alone)");
    else if (found.scope == QLatin1String("module") || found.scope == QLatin1String("class")) scope = tr(" - in this script (with jedi: the modules beside it too)");
    const QString places = found.references.size() == 1 ? tr("1 place") : tr("%1 places").arg(found.references.size());
    messageDock->showReferences(tr("%1: %2%3").arg(found.name, places, scope), py, list);
  });
  connect(py, &PythonDoc::renameAnswered, this, [this, py] {
    const qucs_s::python::Answer &done = py->lastRename();
    QString said;
    if (!done.valid) said = tr("There is no name at the cursor to rename.");
    else if (!done.refusal.isEmpty()) said = done.refusal;
    else {
      const QString others = applyPythonRename(py);
      const QString places = done.count == 1 ? tr("1 place") : tr("%1 places").arg(done.count);
      said = others.isEmpty() ? tr("%1 renamed: %2 (Undo takes it back).").arg(done.name, places) : others;
    }
    statusBar()->showMessage(said, 10000);
  });
  // While the debugger is stopped: the value of a name the mouse rests on.
  py->setValueLookup([this, doc = QPointer<PythonDoc>(py)](const QString &expression) {
    if (pythonRun == nullptr || !pythonRun->isPaused() || doc.isNull()) return false;
    const int id = pythonRun->inspect(expression);
    if (id == 0) return false;
    a_pythonValues.insert(id, doc);
    return true;
  });
}

QString QucsApp::applyPythonRename(PythonDoc *py)
{
  // The script's own change is made (PythonDoc); the other files' here:
  // opened, each one edit, left unsaved - unless one open has changes not
  // saved, which jedi did not see (refused before any is changed).
  const qucs_s::python::Answer &done = py->lastRename();
  QList<std::pair<QString, QString>> others;
  for (const qucs_s::python::FileChange &c : done.changes)
    if (!c.file.isEmpty()) others.append({QFileInfo(c.file).absoluteFilePath(), c.text});
  if (others.isEmpty()) return {};
  for (const auto &[file, text] : std::as_const(others)) {
    int at = 0;
    if (QucsDoc *open = findDoc(file, &at); open != nullptr && open->getDocChanged())
      return tr("%1 has changes not saved: save it, then rename again (the script's own places were renamed).")
          .arg(QFileInfo(file).fileName());
  }
  QStringList names;
  for (const auto &[file, text] : std::as_const(others)) {
    if (!gotoPage(file)) continue;
    auto *doc = qobject_cast<TextDoc *>(DocumentTab->currentWidget());
    if (doc == nullptr) continue;
    QTextCursor all(doc->document());
    all.beginEditBlock();
    all.select(QTextCursor::Document);
    all.insertText(text);
    all.endEditBlock();
    names << QFileInfo(file).fileName();
  }
  showDocument(py);
  const QString places = done.count == 1 ? tr("1 place") : tr("%1 places").arg(done.count);
  return tr("%1 renamed: %2 - in %3 too, open and not saved (Undo in each takes it back).")
      .arg(done.name, places, names.join(QStringLiteral(", ")));
}

void QucsApp::pythonDefinitionFound(PythonDoc *py)
{
  const qucs_s::python::Place place = py->lastDefinition();
  if (!place.valid) {
    statusBar()->showMessage(tr("No definition found for the name at the cursor."), 6000);
    return;
  }
  if (place.builtin) {
    statusBar()->showMessage(tr("%1 is built into Python: it has no source to show.").arg(place.name), 6000);
    return;
  }
  // Back here with Go Back.
  a_pythonBack.append({QPointer<TextDoc>(py), py->textCursor().position()});
  while (a_pythonBack.size() > 50) a_pythonBack.removeFirst();
  if (place.file.isEmpty()) {
    showTextPlace(py, QString(), place.line, place.column + 1);
  } else {
    showTextPlace(nullptr, place.file, place.line, place.column + 1);
    if (auto *there = currentPythonDoc(); there != nullptr && place.library && there != py) there->setLibraryFile(true);
  }
  updatePythonToolbar();
}

void QucsApp::slotPythonRun() { runPython(currentPythonDoc()); }

void QucsApp::slotPythonDebug() { debugPython(currentPythonDoc()); }

void QucsApp::slotPythonDebugStep()
{
  if (pythonRun == nullptr) return;
  const QObject *from = sender();
  if (from == pythonContinueAction) pythonRun->continueRun();
  else if (from == pythonStepOverAction) pythonRun->stepOver();
  else if (from == pythonStepIntoAction) pythonRun->stepInto();
  else if (from == pythonStepOutAction) pythonRun->stepOut();
}

void QucsApp::slotPythonRunSelection()
{
  PythonDoc *py = currentPythonDoc();
  if (py == nullptr) return;
  QTextCursor cursor = py->textCursor();
  const QString name = py->getDocName().isEmpty() ? tr("the script") : QFileInfo(py->getDocName()).fileName();
  if (cursor.hasSelection()) {
    const int first = py->document()->findBlock(cursor.selectionStart()).blockNumber() + 1;
    const int last = py->document()->findBlock(cursor.selectionEnd()).blockNumber() + 1;
    QString code = cursor.selectedText();
    code.replace(QChar::ParagraphSeparator, QLatin1Char('\n')).replace(QChar::LineSeparator, QLatin1Char('\n'));
    runPythonLines(py, first, code, first == last ? tr("%1, line %2").arg(name).arg(first) : tr("%1, lines %2-%3").arg(name).arg(first).arg(last));
    return;
  }
  // The line - and on to the next, as one goes through a script line by line.
  const int line = cursor.blockNumber() + 1;
  runPythonLines(py, line, cursor.block().text(), tr("%1, line %2").arg(name).arg(line));
  if (cursor.movePosition(QTextCursor::NextBlock)) py->setTextCursor(cursor);
}

void QucsApp::slotPythonRunCell(bool advance)
{
  PythonDoc *py = currentPythonDoc();
  if (py == nullptr) return;
  const auto [first, last] = py->currentCell();
  QStringList lines;
  for (int k = first; k <= last; ++k) lines << py->document()->findBlockByNumber(k - 1).text();
  const QString name = py->getDocName().isEmpty() ? tr("the script") : QFileInfo(py->getDocName()).fileName();
  runPythonLines(py, first, lines.join(QLatin1Char('\n')), tr("%1, cell of lines %2-%3").arg(name).arg(first).arg(last));
  if (!advance) return;
  // On to the next cell's first line (past its # %%), or the end.
  QTextBlock next = py->document()->findBlockByNumber(last);   // (the line after the cell: the next one's marker)
  if (next.isValid() && qucs_s::python::isCellMarker(next.text()) && next.next().isValid()) next = next.next();
  QTextCursor cursor(py->document());
  if (next.isValid()) cursor.setPosition(next.position());
  else cursor.movePosition(QTextCursor::End);
  py->setTextCursor(cursor);
  py->ensureCursorVisible();
}

void QucsApp::slotPythonToggleBreakpoint()
{
  if (PythonDoc *py = currentPythonDoc()) py->toggleBreakpoint(py->textCursor().blockNumber() + 1);
}

void QucsApp::slotPythonSignature()
{
  if (PythonDoc *py = currentPythonDoc()) py->showSignature(true);
}

void QucsApp::slotPythonDefinition()
{
  if (PythonDoc *py = currentPythonDoc()) py->goToDefinition();
}

void QucsApp::slotPythonBack()
{
  while (!a_pythonBack.isEmpty()) {
    const auto [doc, position] = a_pythonBack.takeLast();
    if (doc.isNull()) continue;   // (closed since)
    showDocument(doc);
    QTextCursor cursor(doc->document());
    cursor.setPosition(std::min(position, doc->document()->characterCount() - 1));
    doc->setTextCursor(cursor);
    doc->centerCursor();
    doc->setFocus();
    break;
  }
  updatePythonToolbar();
}

void QucsApp::slotPythonReferences()
{
  if (PythonDoc *py = currentPythonDoc()) py->findReferences();
}

void QucsApp::slotPythonRename()
{
  PythonDoc *py = currentPythonDoc();
  if (py == nullptr || py->isReadOnly()) return;
  const QString name = py->nameAtCursor();
  if (name.isEmpty()) {
    statusBar()->showMessage(tr("There is no name at the cursor to rename."), 6000);
    return;
  }
  bool ok = false;
  const QString to = QInputDialog::getText(this, tr("Rename Symbol"), tr("%1 renamed:").arg(name), QLineEdit::Normal, name, &ok).trimmed();
  if (!ok || to.isEmpty() || to == name) return;
  py->renameSymbol(to);
}

void QucsApp::slotPythonSymbols()
{
  PythonDoc *py = currentPythonDoc();
  if (py == nullptr) return;
  // The script's names, then those of the scripts beside it (as open, or
  // read), the folder's first 200.
  QList<PythonSymbolPicker::Entry> entries;
  for (const qucs_s::python::Symbol &s : qucs_s::python::symbolsOf(py->toPlainText())) entries.append({QString(), s});
  if (!py->getDocName().isEmpty()) {
    const QFileInfo self(py->getDocName());
    const QFileInfoList beside = QDir(self.absolutePath()).entryInfoList({QStringLiteral("*.py")}, QDir::Files, QDir::Name);
    int read = 0;
    for (const QFileInfo &file : beside) {
      if (file.absoluteFilePath() == self.absoluteFilePath() || ++read > 200) continue;
      QString text;
      int at = 0;
      if (auto *open = dynamic_cast<TextDoc *>(findDoc(file.absoluteFilePath(), &at))) text = open->toPlainText();
      else if (QFile f(file.absoluteFilePath()); file.size() < 2 * 1024 * 1024 && f.open(QIODevice::ReadOnly)) text = QString::fromUtf8(f.readAll());
      for (const qucs_s::python::Symbol &s : qucs_s::python::symbolsOf(text)) entries.append({file.absoluteFilePath(), s});
    }
  }
  auto *picker = new PythonSymbolPicker(entries, py);
  const int width = std::min(std::max(420, py->width() / 2), py->width() - 20);
  picker->resize(width, std::min(420, py->height() - 40));
  picker->move(py->mapToGlobal(QPoint((py->width() - width) / 2, 30)));
  connect(picker, &PythonSymbolPicker::chosen, this, [this, doc = QPointer<PythonDoc>(py)](const QString &file, int line, int column) {
    if (doc.isNull()) return;
    a_pythonBack.append({QPointer<TextDoc>(doc.data()), doc->textCursor().position()});   // (Go Back comes here)
    if (file.isEmpty()) showTextPlace(doc, QString(), line, column + 1);
    else showTextPlace(nullptr, file, line, column + 1);
    updatePythonToolbar();
  });
  picker->show();
  picker->filterLine()->setFocus();
}

void QucsApp::slotPythonRunToCursor()
{
  PythonDoc *py = currentPythonDoc();
  if (py == nullptr || py->getDocName().isEmpty()) return;
  const int line = py->textCursor().blockNumber() + 1;
  if (pythonRun != nullptr && pythonRun->isDebugging()) {
    if (pythonRun->isPaused()) pythonRun->runTo(QFileInfo(py->getDocName()).absoluteFilePath(), line);
    return;
  }
  debugPython(py, line);
}

void QucsApp::slotPythonRunSettings()
{
  PythonDoc *py = currentPythonDoc();
  if (py == nullptr || py->getDocName().isEmpty()) return;
  qucs_s::python::RunSettings settings = qucs_s::python::runSettingsFor(py->getDocName());
  if (!qucs_s::python::editRunSettingsDialog(this, py->getDocName(), &settings)) return;
  qucs_s::python::setRunSettingsFor(py->getDocName(), settings);
  updatePythonToolbar();
}

void QucsApp::slotPythonQuickFix()
{
  PythonDoc *py = currentPythonDoc();
  if (py == nullptr) return;
  const QRect at = py->cursorRect();
  py->quickFix(py->textCursor().blockNumber() + 1, py->viewport()->mapToGlobal(at.bottomLeft()));
}

void QucsApp::slotPythonEditBreakpoint()
{
  if (PythonDoc *py = currentPythonDoc()) py->editBreakpoint(py->textCursor().blockNumber() + 1, 0);
}

QString QucsApp::showPythonDisplay(const QJsonObject &request)
{
  const QString display = QFileInfo(request.value(QStringLiteral("display")).toString()).absoluteFilePath();
  const QString dataset = request.value(QStringLiteral("dataset")).toString();
  if (!display.endsWith(QLatin1String(".dpl"), Qt::CaseInsensitive) || !QFileInfo(dataset).isFile())
    return tr("qucs.display(): there is no dataset %1 to show.").arg(QDir::toNativeSeparators(dataset));
  // None yet: a data display of that dataset, made.
  if (!QFileInfo::exists(display)) {
    QFile file(display);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
      return tr("qucs.display(): %1 could not be written (%2).").arg(QDir::toNativeSeparators(display), file.errorString());
    file.write(QStringLiteral("<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n  <DataSet=%1>\n  <DataDisplay=%2>\n"
                              "  <OpenDisplay=0>\n</Properties>\n<Symbol>\n</Symbol>\n<Components>\n</Components>\n<Wires>\n"
                              "</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n")
                   .arg(QFileInfo(dataset).fileName(), QFileInfo(display).fileName())
                   .toUtf8());
  }
  if (!gotoPage(display)) return tr("qucs.display(): %1 could not be opened.").arg(QDir::toNativeSeparators(display));
  // (Its data, written anew, read again: gotoPage() has it become current.)
  auto *shown = qobject_cast<Schematic *>(DocumentTab->currentWidget());
  if (shown == nullptr) return tr("qucs.display(): %1 is no data display.").arg(QDir::toNativeSeparators(display));
  // A diagram showing these traces already: shown again, no other added.
  QStringList traces;
  for (const QJsonValue &v : request.value(QStringLiteral("traces")).toArray()) traces << v.toString();
  QStringList sorted = traces;
  sorted.sort();
  for (Diagram *d : shown->a_DocDiags) {
    QStringList vars;
    for (Graph *g : d->Graphs) vars << g->Var;
    vars.sort();
    if (vars == sorted) return {};
  }
  if (a_control == nullptr || traces.isEmpty()) return {};
  // (Of a dataset of no simulator's: qucsator/ has add_diagram read the
  // display's own dataset, name.dat, by the variables' names alone.)
  QJsonArray named;
  for (const QString &t : std::as_const(traces)) named.append(QStringLiteral("qucsator/") + t);
  QJsonObject args{{QStringLiteral("path"), display},
                   {QStringLiteral("type"), request.value(QStringLiteral("type")).toString(QStringLiteral("rect"))},
                   {QStringLiteral("traces"), named}};
  if (const QString title = request.value(QStringLiteral("title")).toString(); !title.isEmpty()) args.insert(QStringLiteral("title"), title);
  a_control->callTool(QStringLiteral("add_diagram"), args, [this](const QJsonObject &result) {
    if (result.value(QLatin1String("isError")).toBool())
      statusBar()->showMessage(tr("qucs.display(): %1").arg(result.value(QStringLiteral("content")).toArray().at(0).toObject()
                                                               .value(QStringLiteral("text")).toString()),
                               10000);
  });
  return {};
}

void QucsApp::slotPythonFormat()
{
  if (PythonDoc *py = currentPythonDoc()) py->format();
}

void QucsApp::slotPythonFix()
{
  if (PythonDoc *py = currentPythonDoc()) py->fixProblems();
}

void QucsApp::slotPythonStop()
{
  if (pythonRun != nullptr) pythonRun->stop();
}

void QucsApp::slotPythonRunInShell() { runPythonInShell(currentPythonDoc()); }

void QucsApp::slotPythonCheck()
{
  PythonDoc *py = currentPythonDoc();
  if (py == nullptr) return;
  a_pythonCheckRaises = true;
  py->checkNow();
  updatePythonToolbar();
}

void QucsApp::slotPythonInterpreterChosen(int index)
{
  PythonDoc *py = currentPythonDoc();
  if (py == nullptr || index < 0) return;
  QString path = pythonInterpreters->itemData(index).toString();
  if (path.isEmpty()) {   // Browse...
    path = QFileDialog::getOpenFileName(this, tr("Choose a Python interpreter"), QFileInfo(py->interpreter()).absolutePath());
    if (path.isEmpty()) {
      updatePythonToolbar();   // (the one it had, shown again)
      return;
    }
    QStringList chosen = _settings::Get().item<QStringList>("PythonInterpreters");
    if (!chosen.contains(path)) {
      chosen.append(path);
      _settings::Get().setItem<QStringList>("PythonInterpreters", chosen);
    }
  }
  py->setInterpreter(path);
  updatePythonToolbar();
}

void QucsApp::slotPythonLineEnds(bool on)
{
  PythonDoc::setMessagesAtLineEnds(on);
}

void QucsApp::slotPythonComplete()
{
  if (PythonDoc *py = currentPythonDoc()) py->complete(true);
}

void QucsApp::showTextPlace(QWidget *document, const QString &path, int line, int column)
{
  auto *text = qobject_cast<TextDoc *>(document);
  if (text == nullptr && !path.isEmpty()) {
    if (!gotoPage(path)) return;
    text = qobject_cast<TextDoc *>(DocumentTab->currentWidget());
  }
  if (text == nullptr) return;
  showDocument(text);
  const QTextBlock block = text->document()->findBlockByNumber(std::max(0, line - 1));
  if (!block.isValid()) return;
  QTextCursor cursor(block);
  if (column > 1) cursor.setPosition(block.position() + std::min(column - 1, std::max(0, block.length() - 1)));
  text->setTextCursor(cursor);
  text->centerCursor();
  text->setFocus();
}
