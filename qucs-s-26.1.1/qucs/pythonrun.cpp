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
#include "pythondoc.h"

#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontInfo>
#include <QHBoxLayout>
#include <QHeaderView>
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
#include <QTextBlock>
#include <QTextCursor>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <functional>

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
    connect(a_stop, &QPushButton::clicked, this, &PythonRunConsole::stop);
    connect(a_clear, &QPushButton::clicked, this, &PythonRunConsole::clear);

    auto* row = new QHBoxLayout;
    row->setContentsMargins(4, 0, 4, 2);
    row->addWidget(a_status, 1);
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
    a_variables = new QTreeWidget(a_debugPanel);
    a_variables->setObjectName(QStringLiteral("pythonDebugVariables"));
    a_variables->setHeaderLabels({tr("Name"), tr("Type"), tr("Value")});
    a_variables->setUniformRowHeights(true);
    a_variables->header()->setStretchLastSection(true);
    connect(a_variables, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) {
        const int handle = item->data(0, Qt::UserRole).toInt();
        if (item->childCount() > 0 || handle < 0 || !a_paused) return;
        a_opening.insert(handle, item);
        command({{QStringLiteral("command"), QStringLiteral("children")}, {QStringLiteral("handle"), handle}});
    });
    // An array, a list, a DataFrame: View as Table (its menu, a double-click).
    a_variables->setContextMenuPolicy(Qt::CustomContextMenu);
    const auto asTable = [this](QTreeWidgetItem* item) {
        if (item == nullptr || !item->data(1, Qt::UserRole).toBool() || !a_paused) return;
        QString name = item->text(0);
        for (QTreeWidgetItem* up = item->parent(); up != nullptr; up = up->parent()) name.prepend(up->text(0) + QLatin1Char(' '));
        emit tableRequested(item->data(0, Qt::UserRole).toInt(), name);
    };
    connect(a_variables, &QTreeWidget::itemDoubleClicked, this, [asTable](QTreeWidgetItem* item) { asTable(item); });
    connect(a_variables, &QTreeWidget::customContextMenuRequested, this, [this, asTable](const QPoint& at) {
        QTreeWidgetItem* item = a_variables->itemAt(at);
        if (item == nullptr) return;
        QMenu menu(this);
        QAction* table = menu.addAction(tr("View as Table"));
        table->setObjectName(QStringLiteral("pythonDebugViewAsTable"));
        table->setEnabled(item->data(1, Qt::UserRole).toBool() && a_paused);
        if (menu.exec(a_variables->viewport()->mapToGlobal(at)) == table) asTable(item);
    });
    a_evaluate = new QLineEdit(a_debugPanel);
    a_evaluate->setObjectName(QStringLiteral("pythonDebugEvaluate"));
    a_evaluate->setPlaceholderText(tr("Evaluate in the frame: an expression, or a statement"));
    a_evaluate->setEnabled(false);
    connect(a_evaluate, &QLineEdit::returnPressed, this, [this] {
        const QString expression = a_evaluate->text().trimmed();
        if (expression.isEmpty()) return;
        evaluate(expression);
        a_evaluate->clear();
    });
    auto* panel = new QVBoxLayout(a_debugPanel);
    panel->setContentsMargins(4, 2, 4, 2);
    panel->setSpacing(2);
    panel->addLayout(buttons);
    panel->addWidget(new QLabel(tr("Call stack"), a_debugPanel));
    panel->addWidget(a_stackView, 1);
    panel->addWidget(new QLabel(tr("Variables"), a_debugPanel));
    panel->addWidget(a_variables, 3);
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
    return start(interpreter, script, {QStringLiteral("-u"), QFileInfo(script).absoluteFilePath()}, false);
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
    if (!start(interpreter, script,
               {QStringLiteral("-u"), QStringLiteral("-c"), qucs_s::python::debuggerProgram(), QFileInfo(script).absoluteFilePath()}, true))
        return false;
    QJsonArray list;
    for (auto it = breakpoints.cbegin(); it != breakpoints.cend(); ++it)
        for (const qucs_s::python::Breakpoint& b : it.value()) {
            QJsonObject o = qucs_s::python::toJson(b);
            o.insert(QStringLiteral("file"), it.key());
            list.append(o);
        }
    QJsonObject first{{QStringLiteral("command"), QStringLiteral("start")}, {QStringLiteral("breakpoints"), list},
                      {QStringLiteral("raised"), raised}};
    if (!runTo.first.isEmpty())
        first.insert(QStringLiteral("run_to"), QJsonObject{{QStringLiteral("file"), runTo.first}, {QStringLiteral("line"), runTo.second}});
    command(first);
    return true;
}

bool PythonRunConsole::start(const QString& interpreter, const QString& script, const QStringList& arguments, bool debugging)
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
    note(tr("%1 %2   (in %3)").arg(QFileInfo(interpreter).fileName(), info.fileName(), QDir::toNativeSeparators(info.absolutePath())));

    a_process = new QProcess(this);
    // Debugged: the script's output on standard output, the debugger's
    // word on standard error (the script's own error output goes with its
    // output).
    a_process->setProcessChannelMode(debugging ? QProcess::SeparateChannels : QProcess::MergedChannels);
    QProcessEnvironment environment = qucs_s::python::scriptEnvironment();   // (the qucs module on its path)
    environment.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    a_process->setProcessEnvironment(environment);
    a_process->setWorkingDirectory(info.absolutePath());
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
    if (a_stopped) {
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

int PythonRunConsole::inspect(const QString& expression)
{
    if (!a_paused) return 0;
    const int id = ++a_requests;
    command({{QStringLiteral("command"), QStringLiteral("inspect")}, {QStringLiteral("expression"), expression},
             {QStringLiteral("frame"), a_frame}, {QStringLiteral("id"), id}});
    return id;
}

int PythonRunConsole::requestData(int handle, const QString& expression, int start, int count)
{
    if (!a_paused) return 0;
    const int id = ++a_requests;
    QJsonObject c{{QStringLiteral("command"), QStringLiteral("data")}, {QStringLiteral("frame"), a_frame},
                  {QStringLiteral("start"), start}, {QStringLiteral("count"), count}, {QStringLiteral("id"), id}};
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
                            f.value(QStringLiteral("function")).toString()});
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
        setStatus(tr("Debugging %1...").arg(QFileInfo(a_script).fileName()));
        emit resumed();
        emit debuggingChanged();
    } else if (kind == QLatin1String("variables")) {
        if (event.value(QStringLiteral("frame")).toInt() != a_frame) return;
        a_variables->clear();
        a_opening.clear();
        fillVariables(nullptr, event.value(QStringLiteral("variables")).toArray());
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
    }
}

void PythonRunConsole::fillVariables(QTreeWidgetItem* parent, const QJsonArray& items)
{
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
        item->setData(1, Qt::UserRole, o.value(QStringLiteral("table")).toBool());   // (View as Table)
        if (o.value(QStringLiteral("table")).toBool()) item->setToolTip(0, tr("Double-click: the value as a table"));
        if (handle >= 0) item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    }
    if (parent != nullptr && parent->childCount() == 0) parent->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicator);
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
