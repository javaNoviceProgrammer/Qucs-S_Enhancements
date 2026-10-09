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
#include "settings.h"

#include <QAction>
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

  pythonFormatAction = new QAction(tr("Format Document"), this);
  pythonFormatAction->setObjectName(QStringLiteral("pythonFormat"));
  pythonFormatAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::ALT | Qt::Key_F));
  pythonFormatAction->setToolTip(tr("The script formatted by ruff (or black) installed for its Python - one edit, "
                                    "Undo takes it back"));
  connect(pythonFormatAction, &QAction::triggered, this, &QucsApp::slotPythonFormat);
  pythonFixAction = new QAction(tr("Fix Problems"), this);
  pythonFixAction->setObjectName(QStringLiteral("pythonFix"));
  pythonFixAction->setToolTip(tr("What ruff can fix of what it finds (imports not used and more) fixed - one edit, "
                                 "Undo takes it back"));
  connect(pythonFixAction, &QAction::triggered, this, &QucsApp::slotPythonFix);

  // The editor's keys work in a script alone (connectPythonDoc()): not
  // Shift+Return in the Python Shell's line, nor Alt+Left in another text.
  for (QAction *a : {pythonRunSelectionAction, pythonRunCellAction, pythonRunCellAdvanceAction, pythonDefinitionAction,
                     pythonBackAction, pythonBreakpointAction, pythonFormatAction, pythonSignatureAction})
    a->setShortcutContext(Qt::WidgetShortcut);

  // The same in Simulation > Python, above the simulators' settings: for
  // the keyboard, and for Claude's trigger_action (it runs menu actions).
  auto *pythonMenu = new QMenu(tr("&Python"), simMenu);
  pythonMenu->addAction(pythonRunAction);
  pythonMenu->addAction(pythonDebugAction);
  pythonMenu->addAction(pythonStopAction);
  pythonMenu->addAction(pythonShellAction);
  pythonMenu->addSeparator();
  pythonMenu->addAction(pythonRunSelectionAction);
  pythonMenu->addAction(pythonRunCellAction);
  pythonMenu->addAction(pythonRunCellAdvanceAction);
  pythonMenu->addSeparator();
  pythonMenu->addAction(pythonContinueAction);
  pythonMenu->addAction(pythonStepOverAction);
  pythonMenu->addAction(pythonStepIntoAction);
  pythonMenu->addAction(pythonStepOutAction);
  pythonMenu->addAction(pythonBreakpointAction);
  pythonMenu->addSeparator();
  pythonMenu->addAction(pythonCheckAction);
  pythonMenu->addAction(pythonLineEndsAction);
  pythonMenu->addAction(pythonFormatAction);
  pythonMenu->addAction(pythonFixAction);
  pythonMenu->addSeparator();
  pythonMenu->addAction(pythonCompleteAction);
  pythonMenu->addAction(pythonSignatureAction);
  pythonMenu->addAction(pythonAsYouTypeAction);
  pythonMenu->addSeparator();
  pythonMenu->addAction(pythonDefinitionAction);
  pythonMenu->addAction(pythonBackAction);
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
                     pythonRunCellAction, pythonRunCellAdvanceAction})
    a->setEnabled(py != nullptr);
  pythonFormatAction->setEnabled(editable);
  pythonFixAction->setEnabled(editable);
  pythonBackAction->setEnabled(!a_pythonBack.isEmpty());
  // (Debug and Continue share Ctrl+F2: one of them enabled at a time.)
  const bool debugging = pythonRun != nullptr && pythonRun->isDebugging();
  const bool paused = debugging && pythonRun->isPaused();
  pythonDebugAction->setEnabled(py != nullptr && !debugging);
  for (QAction *a : {pythonContinueAction, pythonStepOverAction, pythonStepIntoAction, pythonStepOutAction}) a->setEnabled(paused);
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

bool QucsApp::debugPython(PythonDoc *py)
{
  if (py == nullptr) return false;
  if ((py->getDocName().isEmpty() || py->getDocChanged()) && !saveFile(py)) return false;
  // The breakpoints of every script open.
  QHash<QString, QList<int>> breakpoints;
  for (QucsDoc *doc : allDocuments())
    if (auto *script = dynamic_cast<PythonDoc *>(doc); script != nullptr && !script->getDocName().isEmpty())
      if (const QList<int> lines = script->breakpoints(); !lines.isEmpty())
        breakpoints.insert(QFileInfo(script->getDocName()).absoluteFilePath(), lines);
  pythonRunDock->show();
  pythonRunDock->raise();
  const bool started = pythonRun->debug(py->interpreter(), py->getDocName(), breakpoints);
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
  }
  for (QucsDoc *doc : allDocuments())
    if (auto *script = dynamic_cast<PythonDoc *>(doc))
      script->setExecutionLine(script == shown ? line : 0, top);
}

void QucsApp::connectPythonDoc(PythonDoc *py)
{
  py->addActions({pythonRunSelectionAction, pythonRunCellAction, pythonRunCellAdvanceAction, pythonDefinitionAction,
                  pythonBackAction, pythonBreakpointAction, pythonFormatAction, pythonSignatureAction});
  connect(py, &PythonDoc::checkFinished, this, [this, py] { pythonChecked(py); });
  connect(py, &PythonDoc::definitionAnswered, this, [this, py] { pythonDefinitionFound(py); });
  connect(py, &PythonDoc::formatted, this, [this](const QString &said) { statusBar()->showMessage(said, 8000); });
  // A breakpoint set or taken away while it is debugged: the debugger told.
  connect(py, &PythonDoc::breakpointsChanged, this, [this, py] {
    if (pythonRun != nullptr && pythonRun->isDebugging() && !py->getDocName().isEmpty())
      pythonRun->setBreakpoints(QFileInfo(py->getDocName()).absoluteFilePath(), py->breakpoints());
  });
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
