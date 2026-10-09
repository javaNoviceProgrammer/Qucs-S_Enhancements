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
#include <QFileDialog>
#include <QFileInfo>
#include <QMenu>
#include <QStatusBar>
#include <QTextBlock>
#include <QDir>

#include <algorithm>
#include <QToolBar>

bool QucsApp::isPythonFile(const QString &name)
{
  const QString suffix = QFileInfo(name).suffix().toLower();
  return suffix == QLatin1String("py") || suffix == QLatin1String("pyw");
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

  pythonShellAction = new QAction(QIcon(QStringLiteral(":/bitmaps/svg/python_shell.svg")), tr("Run in Shell"), this);
  pythonShellAction->setObjectName(QStringLiteral("pythonRunInShell"));
  pythonShellAction->setToolTip(tr("Run in Shell: the script saved and run in the Python Shell (the Python of "
                                   "Application Settings), in its folder - its variables are there after it, to "
                                   "look at"));
  connect(pythonShellAction, &QAction::triggered, this, &QucsApp::slotPythonRunInShell);
  pythonToolbar->addAction(pythonShellAction);

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

  // The same in Simulation > Python, above the simulators' settings: for
  // the keyboard, and for Claude's trigger_action (it runs menu actions).
  auto *pythonMenu = new QMenu(tr("&Python"), simMenu);
  pythonMenu->addAction(pythonRunAction);
  pythonMenu->addAction(pythonStopAction);
  pythonMenu->addAction(pythonShellAction);
  pythonMenu->addSeparator();
  pythonMenu->addAction(pythonCheckAction);
  pythonMenu->addAction(pythonLineEndsAction);
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

void QucsApp::slotPythonRun() { runPython(currentPythonDoc()); }

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
