/*
 * pythonrun.cpp - a Python script run as a program of its own, its output
 *                 in a console (the Python toolbar's Run) - or debugged
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "pythonrun.h"

#include "main.h"
#include "settings.h"
#include "processconsole.h"
#include "pythondoc.h"

#include <QDir>
#include <QCompleter>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QKeyEvent>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSplitter>
#include <QStringListModel>
#include <QTabWidget>
#include <QTextBlock>
#include <QTextCursor>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <functional>

#ifndef Q_OS_WIN
#include <signal.h>
#endif

namespace {

// A traceback's place: File "/path/x.py", line 3
const QRegularExpression& placePattern()
{
    static const QRegularExpression pattern(QStringLiteral("File \"([^\"]+)\", line (\\d+)"));
    return pattern;
}

} // namespace

PythonRunConsole::PythonRunConsole(QWidget* parent)
    : QWidget(parent), a_decoder(QStringDecoder::Utf8)
{
    a_output = new QPlainTextEdit(this);
    a_output->setObjectName(QStringLiteral("pythonRunOutput"));
    a_output->setReadOnly(true);
    a_output->setUndoRedoEnabled(false);
    a_output->setMaximumBlockCount(kMostLines);
    a_output->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    // Fixed-width, as a terminal's: a traceback's marks under their code.
    QFont font = QucsSettings.textFont;
    if (!QFontInfo(font).fixedPitch()) font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    a_output->setFont(font);
    a_output->viewport()->installEventFilter(this);
    a_output->viewport()->setMouseTracking(true);

    a_status = new QLabel(tr("Run a Python script with Run on the Python toolbar (F2)."), this);
    a_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    a_stop = new QPushButton(tr("Stop"), this);
    a_stop->setObjectName(QStringLiteral("pythonRunStop"));
    a_stop->setEnabled(false);
    a_stop->setToolTip(tr("Ends the script that is running."));
    a_clear = new QPushButton(tr("Clear"), this);
    a_clear->setToolTip(tr("Clears the output."));
    a_interrupt = new QPushButton(tr("Interrupt"), this);
    a_interrupt->setObjectName(QStringLiteral("pythonRunInterrupt"));
    a_interrupt->setEnabled(false);
    a_interrupt->setVisible(canInterrupt());
    a_interrupt->setToolTip(tr("A KeyboardInterrupt in the script, as Ctrl+C in a terminal: in a long call too; debugged, it "
                               "stops where it was"));
    connect(a_interrupt, &QPushButton::clicked, this, &PythonRunConsole::interrupt);
    connect(a_stop, &QPushButton::clicked, this, &PythonRunConsole::stop);
    connect(a_clear, &QPushButton::clicked, this, &PythonRunConsole::clear);

    auto* row = new QHBoxLayout;
    row->setContentsMargins(4, 0, 4, 2);
    row->addWidget(a_status, 1);
    row->addWidget(a_interrupt);
    row->addWidget(a_stop);
    row->addWidget(a_clear);

    // What the script reads: a line typed, Return sends it.
    a_input = new QLineEdit(this);
    a_input->setObjectName(QStringLiteral("pythonRunInput"));
    a_input->setPlaceholderText(tr("Input for the script: type a line, Return sends it (Ctrl+D: the end of its input)"));
    a_input->setEnabled(false);
    a_input->installEventFilter(this);
    connect(a_input, &QLineEdit::returnPressed, this, [this] {
        const QString text = a_input->text();
        if (sendInput(text)) a_input->clear();
    });
    a_endInput = new QPushButton(tr("End Input"), this);
    a_endInput->setObjectName(QStringLiteral("pythonRunEndInput"));
    a_endInput->setToolTip(tr("The script's input ended (Ctrl+D): input() and sys.stdin.read() read the end of the file"));
    a_endInput->setEnabled(false);
    connect(a_endInput, &QPushButton::clicked, this, &PythonRunConsole::endInput);
    auto* inputRow = new QHBoxLayout;
    inputRow->setContentsMargins(4, 0, 4, 0);
    inputRow->addWidget(a_input, 1);
    inputRow->addWidget(a_endInput);

    // The debugger's panel, beside the output while a debug run goes: its
    // buttons, the call stack, the variables, a line to evaluate.
    a_debugPanel = new QWidget(this);
    a_debugPanel->setObjectName(QStringLiteral("pythonDebugPanel"));
    auto* buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 0, 0, 0);
    const auto button = [&](QToolButton*& b, const char* name, const QString& text, const QString& tip, const QString& icon,
                            void (PythonRunConsole::*slot)()) {
        b = new QToolButton(a_debugPanel);
        b->setObjectName(QLatin1String(name));
        b->setText(text);
        b->setToolTip(tip);
        b->setIcon(QIcon(icon));
        b->setToolButtonStyle(Qt::ToolButtonIconOnly);   // (the panel is narrow; the tip says it)
        b->setAutoRaise(true);
        b->setEnabled(false);
        connect(b, &QToolButton::clicked, this, slot);
        buttons->addWidget(b);
    };
    button(a_continue, "pythonDebugContinue", tr("Continue"), tr("Continue (Ctrl+F2): on to the next breakpoint"),
           QStringLiteral(":/bitmaps/svg/python_continue.svg"), &PythonRunConsole::continueRun);
    button(a_pause, "pythonDebugPause", tr("Pause"), tr("Pause (Ctrl+F2): stopped at the next line of the script's"),
           QStringLiteral(":/bitmaps/svg/python_pause.svg"), &PythonRunConsole::pause);
    button(a_stepOver, "pythonDebugStepOver", tr("Step Over"), tr("Step Over (Ctrl+F10): to the next line, a call on this one made whole"),
           QStringLiteral(":/bitmaps/svg/python_stepover.svg"), &PythonRunConsole::stepOver);
    button(a_stepInto, "pythonDebugStepInto", tr("Step Into"), tr("Step Into (Ctrl+F11): into the call on the line"),
           QStringLiteral(":/bitmaps/svg/python_stepinto.svg"), &PythonRunConsole::stepInto);
    button(a_stepOut, "pythonDebugStepOut", tr("Step Out"), tr("Step Out (Ctrl+Shift+F11): out of the function, to the line that called it"),
           QStringLiteral(":/bitmaps/svg/python_stepout.svg"), &PythonRunConsole::stepOut);
    buttons->addStretch(1);
    a_stackView = new QListWidget(a_debugPanel);
    a_stackView->setObjectName(QStringLiteral("pythonDebugStack"));
    a_stackView->setToolTip(tr("The calls it is in, the innermost first: one chosen, its variables and its line"));
    connect(a_stackView, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0 && row != a_frame) selectFrame(row);
    });
    // The variables of the frame, and the watch list: a tab each, their
    // values opened alike (a value's insides, View as Table, Show in a Data
    // Display).
    const auto valueTree = [this](QTreeWidget*& tree, const char* name, const QString& first) {
        tree = new QTreeWidget(a_debugPanel);
        tree->setObjectName(QLatin1String(name));
        tree->setHeaderLabels({first, tr("Type"), tr("Value")});
        tree->setUniformRowHeights(true);
        tree->header()->setStretchLastSection(true);
        connect(tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) {
            const int handle = item->data(0, Qt::UserRole).toInt();
            if (item->childCount() > 0 || handle < 0 || !a_paused) return;
            a_opening.insert(handle, item);
            command({{QStringLiteral("command"), QStringLiteral("children")}, {QStringLiteral("handle"), handle}});
        });
        tree->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int column) {
            if (item->treeWidget() == a_watchView && column == 0 && item->parent() == nullptr) {
                editWatch(a_watchView->indexOfTopLevelItem(item));
                return;
            }
            viewAsTable(item);
        });
        connect(tree, &QTreeWidget::customContextMenuRequested, this, [this, tree](const QPoint& at) {
            QTreeWidgetItem* item = tree->itemAt(at);
            if (item == nullptr) return;
            QMenu menu(this);
            QAction* table = menu.addAction(tr("View as Table"));
            table->setObjectName(QStringLiteral("pythonDebugViewAsTable"));
            table->setEnabled(item->data(1, Qt::UserRole).toBool() && a_paused);
            QAction* display = menu.addAction(tr("Show in a Data Display"));
            display->setObjectName(QStringLiteral("pythonDebugShowInDisplay"));
            display->setEnabled(item->data(1, Qt::UserRole).toBool() && a_paused);
            QAction* edit = nullptr;
            QAction* remove = nullptr;
            if (tree == a_watchView && item->parent() == nullptr) {
                menu.addSeparator();
                edit = menu.addAction(tr("Edit Expression"));
                remove = menu.addAction(tr("Remove Expression"));
            }
            QAction* chosen = menu.exec(tree->viewport()->mapToGlobal(at));
            if (chosen == table) viewAsTable(item);
            else if (chosen == display) showInDisplay(item);
            else if (chosen != nullptr && chosen == edit) editWatch(a_watchView->indexOfTopLevelItem(item));
            else if (chosen != nullptr && chosen == remove) removeWatch(a_watchView->indexOfTopLevelItem(item));
        });
    };
    valueTree(a_variables, "pythonDebugVariables", tr("Name"));
    valueTree(a_watchView, "pythonDebugWatch", tr("Expression"));
    a_watchView->setToolTip(tr("Expressions evaluated in the frame each time it stops: one added below, edited with a "
                               "double-click, removed with Delete"));
    a_watchView->installEventFilter(this);
    a_watchAdd = new QLineEdit(a_debugPanel);
    a_watchAdd->setObjectName(QStringLiteral("pythonDebugWatchAdd"));
    a_watchAdd->setPlaceholderText(tr("Add an expression to watch"));
    connect(a_watchAdd, &QLineEdit::returnPressed, this, [this] {
        const QString expression = a_watchAdd->text().trimmed();
        if (expression.isEmpty()) return;
        addWatch(expression);
        a_watchAdd->clear();
    });
    a_watches = _settings::Get().item<QStringList>("PythonWatches");
    auto* watchPage = new QWidget(a_debugPanel);
    auto* watchLayout = new QVBoxLayout(watchPage);
    watchLayout->setContentsMargins(0, 0, 0, 0);
    watchLayout->setSpacing(2);
    watchLayout->addWidget(a_watchView, 1);
    watchLayout->addWidget(a_watchAdd);
    a_valueTabs = new QTabWidget(a_debugPanel);
    a_valueTabs->setObjectName(QStringLiteral("pythonDebugValueTabs"));
    a_valueTabs->addTab(a_variables, tr("Variables"));
    a_valueTabs->addTab(watchPage, tr("Watch"));
    showWatches({});

    // The line to evaluate in: names completed from the frame, the earlier
    // lines with Up and Down.
    a_evaluate = new QLineEdit(a_debugPanel);
    a_evaluate->setObjectName(QStringLiteral("pythonDebugEvaluate"));
    a_evaluate->setPlaceholderText(tr("Evaluate in the frame: an expression, or a statement (Up: the one before)"));
    a_evaluate->setEnabled(false);
    a_evaluate->installEventFilter(this);
    a_completions = new QStringListModel(this);
    a_evaluateCompleter = new QCompleter(a_completions, this);
    a_evaluateCompleter->setCompletionMode(QCompleter::PopupCompletion);
    a_evaluateCompleter->setCaseSensitivity(Qt::CaseSensitive);
    a_evaluateCompleter->setModelSorting(QCompleter::CaseSensitivelySortedModel);
    a_evaluateCompleter->setMaxVisibleItems(12);
    a_evaluateCompleter->popup()->setObjectName(QStringLiteral("pythonDebugCompletions"));
    a_evaluate->setCompleter(a_evaluateCompleter);
    a_completeDelay = new QTimer(this);
    a_completeDelay->setSingleShot(true);
    a_completeDelay->setInterval(120);
    connect(a_completeDelay, &QTimer::timeout, this, [this] { askCompletions(); });
    connect(a_evaluate, &QLineEdit::textEdited, this, [this](const QString& text) {
        if (!text.isEmpty() && (text.back().isLetterOrNumber() || text.back() == QLatin1Char('_') || text.back() == QLatin1Char('.')))
            a_completeDelay->start();
    });
    connect(a_evaluate, &QLineEdit::returnPressed, this, [this] {
        if (a_evaluateCompleter->popup()->isVisible()) return;
        const QString expression = a_evaluate->text().trimmed();
        if (expression.isEmpty()) return;
        a_history.removeAll(expression);
        a_history.append(expression);
        while (a_history.size() > 100) a_history.removeFirst();
        a_historyAt = int(a_history.size());
        evaluate(expression);
        a_evaluate->clear();
    });
    auto* panel = new QVBoxLayout(a_debugPanel);
    panel->setContentsMargins(4, 2, 4, 2);
    panel->setSpacing(2);
    panel->addLayout(buttons);
    panel->addWidget(new QLabel(tr("Call stack"), a_debugPanel));
    panel->addWidget(a_stackView, 1);
    panel->addWidget(a_valueTabs, 3);
    panel->addWidget(a_evaluate);
    a_debugPanel->hide();

    a_split = new QSplitter(Qt::Horizontal, this);
    a_split->addWidget(a_output);
    a_split->addWidget(a_debugPanel);
    a_split->setStretchFactor(0, 3);
    a_split->setStretchFactor(1, 2);
    a_split->setChildrenCollapsible(false);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(a_split, 1);
    layout->addLayout(inputRow);
    layout->addLayout(row);
}

PythonRunConsole::~PythonRunConsole()
{
    if (a_process != nullptr) {
        disconnect(a_process, nullptr, this, nullptr);
        a_process->kill();
        a_process->waitForFinished(2000);
    }
}

bool PythonRunConsole::isRunning() const
{
    return a_process != nullptr;
}

QString PythonRunConsole::outputText() const
{
    return a_output->toPlainText();
}

bool PythonRunConsole::run(const QString& interpreter, const QString& script)
{
    const qucs_s::python::RunPlan plan = qucs_s::python::runPlanFor(script);
    return start(interpreter, script, QStringList{QStringLiteral("-u"), QFileInfo(script).absoluteFilePath()} + plan.arguments, false, plan);
}

bool PythonRunConsole::debug(const QString& interpreter, const QString& script, const QHash<QString, QList<int>>& breakpoints)
{
    QHash<QString, QList<qucs_s::python::Breakpoint>> specs;
    for (auto it = breakpoints.cbegin(); it != breakpoints.cend(); ++it)
        for (const int line : it.value()) {
            qucs_s::python::Breakpoint b;
            b.line = line;
            specs[it.key()].append(b);
        }
    return debug(interpreter, script, specs, false);
}

bool PythonRunConsole::debug(const QString& interpreter, const QString& script,
                             const QHash<QString, QList<qucs_s::python::Breakpoint>>& breakpoints, bool raised,
                             const std::pair<QString, int>& runTo)
{
    const qucs_s::python::RunPlan plan = qucs_s::python::runPlanFor(script);
    if (!start(interpreter, script,
               QStringList{QStringLiteral("-u"), QStringLiteral("-c"), qucs_s::python::debuggerProgram(), QFileInfo(script).absoluteFilePath()}
                   + plan.arguments,
               true, plan))
        return false;
    QJsonArray list;
    for (auto it = breakpoints.cbegin(); it != breakpoints.cend(); ++it)
        for (const qucs_s::python::Breakpoint& b : it.value()) {
            QJsonObject o = qucs_s::python::toJson(b);
            o.insert(QStringLiteral("file"), it.key());
            list.append(o);
        }
    QJsonObject first{{QStringLiteral("command"), QStringLiteral("start")}, {QStringLiteral("breakpoints"), list},
                      {QStringLiteral("raised"), raised}, {QStringLiteral("library"), a_library}};
    if (!runTo.first.isEmpty())
        first.insert(QStringLiteral("run_to"), QJsonObject{{QStringLiteral("file"), runTo.first}, {QStringLiteral("line"), runTo.second}});
    command(first);
    return true;
}

bool PythonRunConsole::start(const QString& interpreter, const QString& script, const QStringList& arguments, bool debugging,
                             const qucs_s::python::RunPlan& plan)
{
    if (a_process != nullptr) {   // the one going, ended at once
        disconnect(a_process, nullptr, this, nullptr);
        a_process->kill();
        a_process->waitForFinished(2000);
        a_process->deleteLater();
        a_process = nullptr;
    }
    if (a_debugging) {   // (a debug run ended so: said, as one that ends)
        a_debugging = false;
        setPaused(false);
        a_debugPanel->hide();
        emit debuggingChanged();
    }
    clear();
    a_script = script;
    a_interpreter = interpreter;
    a_exitCode = -1;
    a_stopped = false;
    a_decoder.resetState();
    const QFileInfo info(script);
    QString said = QFileInfo(interpreter).fileName() + QLatin1Char(' ') + info.fileName();
    for (const QString& a : plan.arguments)   // (its arguments as a shell would take them)
        said += QLatin1Char(' ') + (a.isEmpty() || a.contains(QRegularExpression(QStringLiteral("[\\s'\"\\\\$]"))) ? ProcessConsole::quotedForShell(a) : a);
    note(tr("%1   (in %2)").arg(said, QDir::toNativeSeparators(plan.folder.isEmpty() ? info.absolutePath() : plan.folder)));
    a_interrupted = false;
    if (!plan.failure.isEmpty()) {
        note(plan.failure);
        setStatus(tr("%1 was not run: its Run Settings (Simulation > Python > Run Settings...)").arg(info.fileName()));
        emit finished(-1);
        return false;
    }

    a_process = new QProcess(this);
    // Debugged: the script's output on standard output, the debugger's
    // word on standard error (the script's own error output goes with its
    // output).
    a_process->setProcessChannelMode(debugging ? QProcess::SeparateChannels : QProcess::MergedChannels);
    QProcessEnvironment environment = plan.environment.isEmpty() ? qucs_s::python::scriptEnvironment() : plan.environment;   // (the qucs module on its path)
    environment.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    a_process->setProcessEnvironment(environment);
    a_process->setWorkingDirectory(plan.folder.isEmpty() ? info.absolutePath() : plan.folder);
    connect(a_process, &QProcess::readyReadStandardOutput, this, &PythonRunConsole::readOutput);
    if (debugging) connect(a_process, &QProcess::readyReadStandardError, this, &PythonRunConsole::readEvents);
    connect(a_process, &QProcess::finished, this, &PythonRunConsole::runFinished);
    a_events.clear();
    a_stack.clear();
    a_frame = 0;
    a_reason.clear();
    a_exception.clear();
    a_stackView->clear();
    a_variables->clear();
    a_opening.clear();
    a_clock.start();
    a_process->start(interpreter, arguments);
    if (!a_process->waitForStarted(10000)) {
        note(tr("%1 could not be started: %2").arg(interpreter, a_process->errorString()));
        setStatus(tr("%1 was not run.").arg(info.fileName()));
        disconnect(a_process, nullptr, this, nullptr);
        a_process->deleteLater();
        a_process = nullptr;
        emit finished(-1);
        return false;
    }
    // Its input: typed below the output (debugged: the debugger's pipe,
    // through its commands).
    setInputOpen(true);
    a_pausing = false;
    a_stop->setEnabled(true);
    a_debugging = debugging;
    setPaused(false);
    a_debugPanel->setVisible(debugging);
    a_evaluate->setEnabled(false);
    setStatus(debugging ? tr("Debugging %1...").arg(info.fileName()) : tr("Running %1...").arg(info.fileName()));
    emit started();
    if (debugging) emit debuggingChanged();
    return true;
}

void PythonRunConsole::stop()
{
    if (a_process == nullptr) return;
    a_stopped = true;
#ifdef Q_OS_WIN
    a_process->kill();   // (a console program takes no polite request)
#else
    a_process->terminate();
    QPointer<QProcess> process = a_process;
    QTimer::singleShot(2000, this, [process] {
        if (process != nullptr && process->state() != QProcess::NotRunning) process->kill();
    });
#endif
}

void PythonRunConsole::clear()
{
    a_output->clear();
    a_lineOpen = false;
    a_overwrite = false;
}

void PythonRunConsole::readOutput()
{
    if (a_process == nullptr) return;
    const QByteArray bytes = a_process->readAllStandardOutput();
    if (!bytes.isEmpty()) append(a_decoder.decode(bytes));
}

void PythonRunConsole::runFinished()
{
    if (a_process == nullptr) return;
    readOutput();
    if (a_debugging) readEvents();
    if (a_lineOpen) endLine();
    QProcess* process = a_process;
    a_process = nullptr;
    disconnect(process, nullptr, this, nullptr);
    process->deleteLater();
    const QString seconds = QString::number(a_clock.elapsed() / 1000.0, 'f', 2);
    const QString name = QFileInfo(a_script).fileName();
    if (a_interrupted && !a_stopped) {
        // (A KeyboardInterrupt no one catches ends Python by SIGINT itself.)
        const bool normal = process->exitStatus() == QProcess::NormalExit;
        a_exitCode = normal ? process->exitCode() : -1;
        note(normal ? tr("Interrupted: exit code %1 after %2 s.").arg(a_exitCode).arg(seconds) : tr("Interrupted after %1 s.").arg(seconds));
        setStatus(tr("%1 was interrupted after %2 s.").arg(name, seconds));
    } else if (a_stopped) {
        a_exitCode = -1;
        note(tr("Stopped after %1 s.").arg(seconds));
        setStatus(tr("%1 was stopped after %2 s.").arg(name, seconds));
    } else if (process->exitStatus() != QProcess::NormalExit) {
        a_exitCode = -1;
        note(tr("Ended abnormally after %1 s.").arg(seconds));
        setStatus(tr("%1 ended abnormally after %2 s.").arg(name, seconds));
    } else {
        a_exitCode = process->exitCode();
        note(tr("Exit code %1 after %2 s.").arg(a_exitCode).arg(seconds));
        setStatus(tr("%1: exit code %2 after %3 s.").arg(name).arg(a_exitCode).arg(seconds));
    }
    a_stop->setEnabled(false);
    a_interrupt->setEnabled(false);
    setInputOpen(false);
    a_pausing = false;
    const bool debugged = a_debugging;
    a_debugging = false;
    setPaused(false);
    a_debugPanel->hide();
    a_stack.clear();
    a_stackView->clear();
    a_variables->clear();
    a_opening.clear();
    emit finished(a_exitCode);
    if (debugged) emit debuggingChanged();
}

// ----------------------------------------------------------------------
// The debugger

void PythonRunConsole::command(const QJsonObject& command)
{
    if (a_process == nullptr || !a_debugging) return;
    a_process->write(QJsonDocument(command).toJson(QJsonDocument::Compact) + '\n');
}

void PythonRunConsole::readEvents()
{
    if (a_process == nullptr) return;
    a_events += a_process->readAllStandardError();
    qsizetype end;
    while ((end = a_events.indexOf('\n')) >= 0) {
        const QByteArray line = a_events.left(end);
        a_events.remove(0, end + 1);
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject() && doc.object().contains(QStringLiteral("event"))) {
            handleEvent(doc.object());
        } else if (!line.trimmed().isEmpty()) {
            append(a_decoder.decode(line + '\n'));   // (Python's own word, before the debugger had the channel)
        }
    }
}

void PythonRunConsole::setPaused(bool paused)
{
    a_paused = paused;
    for (QToolButton* b : {a_continue, a_stepOver, a_stepInto, a_stepOut}) b->setEnabled(paused);
    a_pause->setEnabled(a_debugging && !paused && a_process != nullptr);
    // (Stopped in the debugger: an interrupt would land in it, not the script.)
    a_interrupt->setEnabled(a_process != nullptr && !paused);
    a_continue->setVisible(paused || !a_debugging);
    a_pause->setVisible(!a_continue->isVisible());
    a_evaluate->setEnabled(paused);
}

void PythonRunConsole::setInputOpen(bool open)
{
    a_inputOpen = open;
    a_input->setEnabled(open);
    a_endInput->setEnabled(open);
    if (!open) a_input->clear();
}

bool PythonRunConsole::sendInput(const QString& text)
{
    if (a_process == nullptr || !a_inputOpen) return false;
    // Echoed after what it wrote last (its prompt), as a terminal has it.
    QTextCursor cursor(a_output->document());
    cursor.movePosition(QTextCursor::End);
    QTextCharFormat typed;
    typed.setFontWeight(QFont::Bold);
    if (a_overwrite) a_overwrite = false;
    cursor.insertText(text, typed);
    a_lineOpen = true;
    endLine();
    a_output->verticalScrollBar()->setValue(a_output->verticalScrollBar()->maximum());
    if (a_debugging) command({{QStringLiteral("command"), QStringLiteral("input")}, {QStringLiteral("text"), text + QLatin1Char('\n')}});
    else a_process->write((text + QLatin1Char('\n')).toUtf8());
    return true;
}

void PythonRunConsole::endInput()
{
    if (a_process == nullptr || !a_inputOpen) return;
    if (a_debugging) command({{QStringLiteral("command"), QStringLiteral("eof")}});
    else a_process->closeWriteChannel();
    setInputOpen(false);
    note(tr("(The end of its input.)"));
}

void PythonRunConsole::pause()
{
    if (!a_debugging || a_paused || a_process == nullptr) return;
    a_pausing = true;
    setStatus(tr("Pausing %1... (at its next line: one in a long call stops after it)").arg(QFileInfo(a_script).fileName()));
    command({{QStringLiteral("command"), QStringLiteral("pause")}});
}

void PythonRunConsole::runTo(const QString& file, int line)
{
    if (!a_paused) return;
    command({{QStringLiteral("command"), QStringLiteral("run_to")}, {QStringLiteral("file"), file}, {QStringLiteral("line"), line}});
}

void PythonRunConsole::setBreakOnRaised(bool on)
{
    command({{QStringLiteral("command"), QStringLiteral("raised")}, {QStringLiteral("on"), on}});
}

void PythonRunConsole::setDebugLibrary(bool on)
{
    a_library = on;
    command({{QStringLiteral("command"), QStringLiteral("library")}, {QStringLiteral("on"), on}});
}

bool PythonRunConsole::canInterrupt()
{
#ifdef Q_OS_WIN
    return false;   // (a console's Ctrl+C reaches no process started so)
#else
    return true;
#endif
}

void PythonRunConsole::interrupt()
{
    if (a_process == nullptr || a_paused || !canInterrupt()) return;
#ifndef Q_OS_WIN
    if (::kill(pid_t(a_process->processId()), SIGINT) != 0) return;
    a_interrupted = true;
    a_pausing = false;
    setStatus(tr("Interrupting %1 (KeyboardInterrupt)...").arg(QFileInfo(a_script).fileName()));
#endif
}

int PythonRunConsole::inspect(const QString& expression)
{
    if (!a_paused) return 0;
    const int id = ++a_requests;
    command({{QStringLiteral("command"), QStringLiteral("inspect")}, {QStringLiteral("expression"), expression},
             {QStringLiteral("frame"), a_frame}, {QStringLiteral("id"), id}});
    return id;
}

int PythonRunConsole::requestData(int handle, const QString& expression, const QJsonObject& view)
{
    if (!a_paused) return 0;
    const int id = ++a_requests;
    QJsonObject c = view;
    c.insert(QStringLiteral("command"), QStringLiteral("data"));
    c.insert(QStringLiteral("frame"), a_frame);
    c.insert(QStringLiteral("id"), id);
    if (handle >= 0) c.insert(QStringLiteral("handle"), handle);
    else c.insert(QStringLiteral("expression"), expression);
    command(c);
    return id;
}

void PythonRunConsole::handleEvent(const QJsonObject& event)
{
    const QString kind = event.value(QStringLiteral("event")).toString();
    if (kind == QLatin1String("stopped")) {
        readOutput();   // (what it printed before it stopped, first)
        a_stack.clear();
        for (const QJsonValue& v : event.value(QStringLiteral("stack")).toArray()) {
            const QJsonObject f = v.toObject();
            a_stack.append({f.value(QStringLiteral("file")).toString(), f.value(QStringLiteral("line")).toInt(),
                            f.value(QStringLiteral("function")).toString(), f.value(QStringLiteral("library")).toBool(),
                            f.value(QStringLiteral("first")).toInt()});
        }
        a_frame = 0;
        a_reason = event.value(QStringLiteral("reason")).toString();
        a_exception = event.value(QStringLiteral("exception")).toString();
        {
            const QSignalBlocker block(a_stackView);
            a_stackView->clear();
            for (const Frame& f : std::as_const(a_stack)) {
                auto* item = new QListWidgetItem(tr("%1   %2, line %3").arg(f.function, QFileInfo(f.file).fileName()).arg(f.line), a_stackView);
                item->setToolTip(QDir::toNativeSeparators(f.file));
            }
            a_stackView->setCurrentRow(0);
        }
        a_variables->clear();
        a_opening.clear();
        fillVariables(nullptr, event.value(QStringLiteral("variables")).toArray());
        a_pausing = false;
        setPaused(true);
        refreshWatches();
        if (!a_stack.isEmpty()) {
            const Frame& top = a_stack.first();
            const QString where = tr("%1, line %2").arg(QFileInfo(top.file).fileName()).arg(top.line);
            QString said = tr("Paused at %1.").arg(where);
            if (a_reason == QLatin1String("exception")) said = tr("Stopped by %1 at %2.").arg(a_exception, where);
            else if (a_reason == QLatin1String("raised")) said = tr("%1 raised at %2 (Continue: on, as it goes).").arg(a_exception, where);
            else if (a_reason == QLatin1String("breakpoint")) said = tr("Paused at a breakpoint: %1.").arg(where);
            setStatus(said);
            emit locationShown(top.file, top.line, true);
        }
        emit paused();
        emit debuggingChanged();
        emit variablesShown();
    } else if (kind == QLatin1String("running")) {
        setPaused(false);
        a_stack.clear();
        {
            const QSignalBlocker block(a_stackView);
            a_stackView->clear();
        }
        a_variables->clear();
        a_opening.clear();
        a_frameValues.clear();
        showWatches({});
        setStatus(tr("Debugging %1...").arg(QFileInfo(a_script).fileName()));
        emit resumed();
        emit debuggingChanged();
    } else if (kind == QLatin1String("variables")) {
        if (event.value(QStringLiteral("frame")).toInt() != a_frame) return;
        a_variables->clear();
        a_opening.clear();
        fillVariables(nullptr, event.value(QStringLiteral("variables")).toArray());
        refreshWatches();
        emit variablesShown();
    } else if (kind == QLatin1String("children")) {
        QTreeWidgetItem* item = a_opening.take(event.value(QStringLiteral("handle")).toInt());
        if (item != nullptr) fillVariables(item, event.value(QStringLiteral("items")).toArray());
        emit variablesShown();
    } else if (kind == QLatin1String("evaluated")) {
        const QString expression = event.value(QStringLiteral("expression")).toString();
        if (event.contains(QStringLiteral("error"))) {
            a_lastEvaluated = event.value(QStringLiteral("error")).toString();
        } else {
            const QJsonObject value = event.value(QStringLiteral("value")).toObject();
            a_lastEvaluated = value.isEmpty() ? tr("(done)") : value.value(QStringLiteral("value")).toString();
        }
        note(QStringLiteral(">>> %1").arg(expression));
        append(a_lastEvaluated + QLatin1Char('\n'));
        emit evaluated();
    } else if (kind == QLatin1String("inspected")) {
        const QJsonObject value = event.value(QStringLiteral("value")).toObject();
        emit inspected(event.value(QStringLiteral("id")).toInt(), event.value(QStringLiteral("expression")).toString(),
                       value.isEmpty() ? QString() : value.value(QStringLiteral("value")).toString(),
                       event.value(QStringLiteral("error")).toString());
    } else if (kind == QLatin1String("data")) {
        emit dataArrived(event.value(QStringLiteral("id")).toInt(), event);
    } else if (kind == QLatin1String("watches")) {
        if (event.value(QStringLiteral("frame")).toInt() != a_frame) return;
        showWatches(event.value(QStringLiteral("values")).toArray());
    } else if (kind == QLatin1String("completions")) {
        if (event.value(QStringLiteral("id")).toInt() != a_completeRequest) return;   // (overtaken)
        const QString text = a_evaluate->text();
        const QString before = text.left(event.value(QStringLiteral("start")).toInt());
        QStringList lines;
        for (const QJsonValue& v : event.value(QStringLiteral("items")).toArray()) lines << before + v.toString();
        lines.sort(Qt::CaseSensitive);
        a_completions->setStringList(lines);
        if (!lines.isEmpty() && a_evaluate->hasFocus()) {
            a_evaluateCompleter->setCompletionPrefix(text);
            if (a_evaluateCompleter->completionCount() > 0 && !(a_evaluateCompleter->completionCount() == 1 && a_evaluateCompleter->currentCompletion() == text))
                a_evaluateCompleter->complete();
        }
        emit completionsShown();
    } else if (kind == QLatin1String("displayed")) {
        if (event.value(QStringLiteral("id")).toInt() != a_displayRequest) return;
        emit displayed(event.value(QStringLiteral("path")).toString(), event.value(QStringLiteral("error")).toString());
    }
}

void PythonRunConsole::fillVariables(QTreeWidgetItem* parent, const QJsonArray& items)
{
    if (parent == nullptr) a_frameValues.clear();
    for (const QJsonValue& v : items) {
        const QJsonObject o = v.toObject();
        auto* item = parent != nullptr ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(a_variables);
        const QString value = o.value(QStringLiteral("value")).toString();
        item->setText(0, o.value(QStringLiteral("name")).toString());
        item->setText(1, o.value(QStringLiteral("type")).toString());
        item->setText(2, value);
        item->setToolTip(2, value);
        const int handle = o.value(QStringLiteral("handle")).toInt(-1);
        item->setData(0, Qt::UserRole, handle);
        if (parent == nullptr) a_frameValues.insert(item->text(0), value);   // (inline values)
        item->setData(1, Qt::UserRole, o.value(QStringLiteral("table")).toBool());   // (View as Table)
        if (o.value(QStringLiteral("table")).toBool()) item->setToolTip(0, tr("Double-click: the value as a table"));
        if (handle >= 0) item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    }
    if (parent != nullptr && parent->childCount() == 0) parent->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicator);
}

void PythonRunConsole::viewAsTable(QTreeWidgetItem* item)
{
    if (item == nullptr || !item->data(1, Qt::UserRole).toBool() || !a_paused) return;
    QString name = item->text(0);
    for (QTreeWidgetItem* up = item->parent(); up != nullptr; up = up->parent()) name.prepend(up->text(0) + QLatin1Char(' '));
    emit tableRequested(item->data(0, Qt::UserRole).toInt(), name);
}

void PythonRunConsole::showInDisplay(QTreeWidgetItem* item)
{
    if (item == nullptr || !a_paused) return;
    const int handle = item->data(0, Qt::UserRole).toInt();
    a_displayRequest = ++a_requests;
    QJsonObject c{{QStringLiteral("command"), QStringLiteral("display")}, {QStringLiteral("frame"), a_frame},
                  {QStringLiteral("name"), item->text(0)}, {QStringLiteral("id"), a_displayRequest}};
    if (handle >= 0) c.insert(QStringLiteral("handle"), handle);
    else c.insert(QStringLiteral("expression"), item->text(0));
    command(c);
}

void PythonRunConsole::addWatch(const QString& expression)
{
    const QString e = expression.trimmed();
    if (e.isEmpty() || a_watches.contains(e)) return;
    a_watches.append(e);
    saveWatches();
    if (a_paused) refreshWatches();
    else showWatches({});
}

void PythonRunConsole::removeWatch(int index)
{
    if (index < 0 || index >= a_watches.size()) return;
    a_watches.removeAt(index);
    saveWatches();
    if (a_paused) refreshWatches();
    else showWatches({});
}

void PythonRunConsole::editWatch(int index)
{
    if (index < 0 || index >= a_watches.size()) return;
    bool ok = false;
    const QString e = QInputDialog::getText(this, tr("Watch"), tr("Expression:"), QLineEdit::Normal, a_watches.at(index), &ok).trimmed();
    if (!ok) return;
    if (e.isEmpty()) {
        removeWatch(index);
        return;
    }
    a_watches[index] = e;
    a_watches.removeDuplicates();
    saveWatches();
    if (a_paused) refreshWatches();
    else showWatches({});
}

void PythonRunConsole::saveWatches()
{
    _settings::Get().setItem<QStringList>("PythonWatches", a_watches);
}

void PythonRunConsole::refreshWatches()
{
    if (!a_paused || a_watches.isEmpty()) {
        showWatches({});
        return;
    }
    command({{QStringLiteral("command"), QStringLiteral("watch")}, {QStringLiteral("expressions"), QJsonArray::fromStringList(a_watches)},
             {QStringLiteral("frame"), a_frame}});
}

void PythonRunConsole::showWatches(const QJsonArray& values)
{
    a_watchView->clear();
    for (int k = 0; k < a_watches.size(); ++k) {
        const QJsonObject o = values.at(k).toObject();
        auto* item = new QTreeWidgetItem(a_watchView);
        item->setText(0, a_watches.at(k));
        item->setData(0, Qt::UserRole, -1);
        if (o.contains(QStringLiteral("error"))) {
            item->setText(2, o.value(QStringLiteral("error")).toString());
            item->setForeground(2, palette().placeholderText());
            continue;
        }
        if (o.isEmpty()) continue;   // (not stopped: the expression alone)
        const QString value = o.value(QStringLiteral("value")).toString();
        item->setText(1, o.value(QStringLiteral("type")).toString());
        item->setText(2, value);
        item->setToolTip(2, value);
        const int handle = o.value(QStringLiteral("handle")).toInt(-1);
        item->setData(0, Qt::UserRole, handle);
        item->setData(1, Qt::UserRole, o.value(QStringLiteral("table")).toBool());
        if (handle >= 0) item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    }
    a_valueTabs->setTabText(1, a_watches.isEmpty() ? tr("Watch") : tr("Watch (%1)").arg(a_watches.size()));
    emit watchesShown();
}

QStringList PythonRunConsole::watchRows() const
{
    QStringList rows;
    for (int k = 0; k < a_watchView->topLevelItemCount(); ++k) {
        const QTreeWidgetItem* item = a_watchView->topLevelItem(k);
        rows << (item->text(1).isEmpty() && !item->text(2).isEmpty()
                     ? QStringLiteral("%1: %2").arg(item->text(0), item->text(2))
                     : QStringLiteral("%1: %2 = %3").arg(item->text(0), item->text(1), item->text(2)));
    }
    return rows;
}

QStringList PythonRunConsole::evaluateCompletions() const
{
    return a_completions->stringList();
}

void PythonRunConsole::askCompletions()
{
    if (!a_paused) return;
    a_completeRequest = ++a_requests;
    command({{QStringLiteral("command"), QStringLiteral("complete")}, {QStringLiteral("text"), a_evaluate->text()},
             {QStringLiteral("frame"), a_frame}, {QStringLiteral("id"), a_completeRequest}});
}

QStringList PythonRunConsole::variableRows() const
{
    QStringList rows;
    const std::function<void(QTreeWidgetItem*, int)> walk = [&](QTreeWidgetItem* item, int depth) {
        rows << QStringLiteral("%1%2: %3 = %4").arg(QString(depth * 2, QLatin1Char(' ')), item->text(0), item->text(1), item->text(2));
        for (int k = 0; k < item->childCount(); ++k) walk(item->child(k), depth + 1);
    };
    for (int k = 0; k < a_variables->topLevelItemCount(); ++k) walk(a_variables->topLevelItem(k), 0);
    return rows;
}

void PythonRunConsole::continueRun()
{
    if (a_paused) command({{QStringLiteral("command"), QStringLiteral("continue")}});
}

void PythonRunConsole::stepOver()
{
    if (a_paused) command({{QStringLiteral("command"), QStringLiteral("next")}});
}

void PythonRunConsole::stepInto()
{
    if (a_paused) command({{QStringLiteral("command"), QStringLiteral("step")}});
}

void PythonRunConsole::stepOut()
{
    if (a_paused) command({{QStringLiteral("command"), QStringLiteral("return")}});
}

void PythonRunConsole::selectFrame(int index)
{
    if (!a_paused || index < 0 || index >= a_stack.size()) return;
    a_frame = index;
    {
        const QSignalBlocker block(a_stackView);
        a_stackView->setCurrentRow(index);
    }
    command({{QStringLiteral("command"), QStringLiteral("frame")}, {QStringLiteral("index"), index}});
    emit locationShown(a_stack.at(index).file, a_stack.at(index).line, index == 0);
}

void PythonRunConsole::evaluate(const QString& expression)
{
    if (!a_paused) return;
    command({{QStringLiteral("command"), QStringLiteral("evaluate")}, {QStringLiteral("expression"), expression},
             {QStringLiteral("frame"), a_frame}});
}

void PythonRunConsole::setBreakpoints(const QString& file, const QList<int>& lines)
{
    QJsonArray list;
    for (const int line : lines) list.append(line);
    command({{QStringLiteral("command"), QStringLiteral("breakpoints")}, {QStringLiteral("file"), file}, {QStringLiteral("lines"), list}});
}

void PythonRunConsole::setBreakpoints(const QString& file, const QList<qucs_s::python::Breakpoint>& breakpoints)
{
    QJsonArray list;
    for (const qucs_s::python::Breakpoint& b : breakpoints) list.append(qucs_s::python::toJson(b));
    command({{QStringLiteral("command"), QStringLiteral("breakpoints")}, {QStringLiteral("file"), file}, {QStringLiteral("lines"), list}});
}

void PythonRunConsole::append(const QString& text)
{
    QTextCursor cursor(a_output->document());
    cursor.movePosition(QTextCursor::End);
    QString pending;
    const auto flush = [&] {
        if (pending.isEmpty()) return;
        if (a_overwrite) {   // after a carriage return: the line begins again
            cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
            cursor.removeSelectedText();
            a_overwrite = false;
        }
        cursor.insertText(pending, QTextCharFormat());
        pending.clear();
        a_lineOpen = true;
    };
    for (qsizetype k = 0; k < text.size(); ++k) {
        const QChar c = text.at(k);
        if (c == QLatin1Char('\r')) {
            flush();
            if (k + 1 < text.size() && text.at(k + 1) == QLatin1Char('\n')) continue;   // CR LF: the LF ends it
            a_overwrite = true;
        } else if (c == QLatin1Char('\n')) {
            flush();
            a_overwrite = false;
            endLine();
            cursor.movePosition(QTextCursor::End);
        } else {
            pending += c;
        }
    }
    flush();
    a_output->verticalScrollBar()->setValue(a_output->verticalScrollBar()->maximum());
}

void PythonRunConsole::endLine()
{
    QTextDocument* document = a_output->document();
    QTextBlock block = document->lastBlock();
    // A traceback's place: a link to it, when the file is there.
    const QRegularExpressionMatch m = placePattern().match(block.text());
    if (m.hasMatch()) {
        QString file = m.captured(1);
        if (QFileInfo(file).isRelative() && !a_script.isEmpty()) file = QFileInfo(a_script).absoluteDir().filePath(file);
        if (QFileInfo(file).isFile()) {
            QTextCursor link(block);
            link.setPosition(block.position() + m.capturedStart(0));
            link.setPosition(block.position() + m.capturedEnd(0), QTextCursor::KeepAnchor);
            QTextCharFormat format;
            format.setAnchor(true);
            format.setAnchorHref(QStringLiteral("%1#%2").arg(QFileInfo(file).absoluteFilePath(), m.captured(2)));
            format.setForeground(palette().link());
            format.setFontUnderline(true);
            link.mergeCharFormat(format);
        }
    }
    QTextCursor cursor(document);
    cursor.movePosition(QTextCursor::End);
    cursor.insertBlock(QTextBlockFormat(), QTextCharFormat());
    a_lineOpen = false;
}

void PythonRunConsole::note(const QString& text)
{
    if (a_lineOpen) endLine();
    QTextCursor cursor(a_output->document());
    cursor.movePosition(QTextCursor::End);
    QTextCharFormat format;
    format.setForeground(palette().placeholderText());
    format.setFontItalic(true);
    cursor.insertText(text, format);
    cursor.insertBlock(QTextBlockFormat(), QTextCharFormat());
    a_output->verticalScrollBar()->setValue(a_output->verticalScrollBar()->maximum());
}

void PythonRunConsole::setStatus(const QString& text)
{
    a_status->setText(text);
}

bool PythonRunConsole::eventFilter(QObject* watched, QEvent* event)
{
    // The evaluate line's history: Up, Down (its list of names not shown).
    if (watched == a_evaluate && event->type() == QEvent::KeyPress && !a_evaluateCompleter->popup()->isVisible()) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if ((key->key() == Qt::Key_Up || key->key() == Qt::Key_Down) && !a_history.isEmpty()) {
            a_historyAt = std::clamp(a_historyAt + (key->key() == Qt::Key_Up ? -1 : 1), 0, int(a_history.size()));
            a_evaluate->setText(a_historyAt < a_history.size() ? a_history.at(a_historyAt) : QString());
            return true;
        }
    }
    // Delete on a watch: it goes (the list's key, not the window's Delete).
    if (watched == a_watchView && (event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride)) {
        const auto* key = static_cast<QKeyEvent*>(event);
        QTreeWidgetItem* item = a_watchView->currentItem();
        if ((key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace) && item != nullptr && item->parent() == nullptr) {
            if (event->type() == QEvent::ShortcutOverride) event->accept();
            else removeWatch(a_watchView->indexOfTopLevelItem(item));
            return true;
        }
    }
    // Ctrl+D in the input line: the end of the script's input (the line's,
    // not the window's shortcut).
    if (watched == a_input && (event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride)) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_D && (key->modifiers() & (Qt::ControlModifier | Qt::MetaModifier))) {
            if (event->type() == QEvent::ShortcutOverride) event->accept();
            else endInput();
            return true;
        }
    }
    if (watched == a_output->viewport()) {
        if (event->type() == QEvent::MouseMove) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            a_output->viewport()->setCursor(a_output->anchorAt(mouse->position().toPoint()).isEmpty() ? Qt::IBeamCursor : Qt::PointingHandCursor);
        } else if (event->type() == QEvent::MouseButtonRelease) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            const QString href = a_output->anchorAt(mouse->position().toPoint());
            if (mouse->button() == Qt::LeftButton && !href.isEmpty() && !a_output->textCursor().hasSelection()) {
                const qsizetype hash = href.lastIndexOf(QLatin1Char('#'));
                emit locationRequested(href.left(hash), href.mid(hash + 1).toInt());
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}
