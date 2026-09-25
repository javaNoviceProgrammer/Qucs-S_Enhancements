/*
 * claudecode.cpp - a Claude Code session: the claude program run headless,
 *                  its conversation over stream-json both ways
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "claudecode.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>

namespace qucs_s::claude {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("ClaudeCode", text);
}

bool isFileTool(const QString& tool)
{
    return tool == QLatin1String("Write") || tool == QLatin1String("Edit") || tool == QLatin1String("MultiEdit")
           || tool == QLatin1String("NotebookEdit");
}

QString fileOf(const QJsonObject& input)
{
    const QString file = input.value(QLatin1String("file_path")).toString();
    return file.isEmpty() ? input.value(QLatin1String("notebook_path")).toString() : file;
}

// The first line, and not more than a line's worth of it.
QString oneLine(const QString& text, int max = 160)
{
    QString line = text.trimmed().section(QLatin1Char('\n'), 0, 0).trimmed();
    if (text.trimmed().contains(QLatin1Char('\n'))) line += QStringLiteral(" …");
    if (line.size() > max) line = line.left(max - 1) + QChar(0x2026);
    return line;
}

// At most \a lines lines of \a text.
QString firstLines(const QString& text, int lines)
{
    const QStringList all = text.split(QLatin1Char('\n'));
    if (all.size() <= lines) return text;
    return all.mid(0, lines).join(QLatin1Char('\n'))
           + tr("\n… (%1 more lines)").arg(all.size() - lines);
}

// A tool result's content: a string, or text blocks.
QString textOf(const QJsonValue& content)
{
    if (content.isString()) return content.toString();
    QStringList parts;
    for (const QJsonValue& v : content.toArray()) {
        const QJsonObject block = v.toObject();
        if (block.value(QLatin1String("type")).toString() == QLatin1String("text"))
            parts << block.value(QLatin1String("text")).toString();
    }
    return parts.join(QLatin1Char('\n'));
}

bool fromSubagent(const QJsonObject& m)
{
    const QJsonValue parent = m.value(QLatin1String("parent_tool_use_id"));
    return parent.isString() && !parent.toString().isEmpty();
}

QString actionOf(const QString& tool)
{
    if (tool == QLatin1String("Bash") || tool == QLatin1String("PowerShell")) return tr("run a command");
    if (tool == QLatin1String("Write")) return tr("write a file");
    if (tool == QLatin1String("Edit") || tool == QLatin1String("MultiEdit")) return tr("edit a file");
    if (tool == QLatin1String("NotebookEdit")) return tr("edit a notebook");
    if (tool == QLatin1String("WebFetch")) return tr("fetch a web page");
    if (tool == QLatin1String("WebSearch")) return tr("search the web");
    if (tool == QLatin1String("Read")) return tr("read a file");
    return tr("use %1").arg(tool);
}

QString detailOf(const QString& tool, const QJsonObject& input)
{
    const auto str = [&input](const char* key) { return input.value(QLatin1String(key)).toString(); };
    if (tool == QLatin1String("Bash") || tool == QLatin1String("PowerShell")) {
        const QString description = str("description");
        return firstLines(str("command"), 12) + (description.isEmpty() ? QString() : QStringLiteral("\n# ") + description);
    }
    if (tool == QLatin1String("Write")) return firstLines(str("content"), 14);
    if (tool == QLatin1String("Edit")) {
        const auto marked = [](const QString& text, QChar mark) {
            QStringList lines = firstLines(text, 7).split(QLatin1Char('\n'));
            for (QString& line : lines) line.prepend(mark + QLatin1Char(' '));
            return lines.join(QLatin1Char('\n'));
        };
        return marked(str("old_string"), QLatin1Char('-')) + QLatin1Char('\n') + marked(str("new_string"), QLatin1Char('+'));
    }
    if (tool == QLatin1String("MultiEdit")) {
        const int n = input.value(QLatin1String("edits")).toArray().size();
        return n == 1 ? tr("1 change") : tr("%1 changes").arg(n);
    }
    if (tool == QLatin1String("WebFetch")) return str("prompt");
    if (tool == QLatin1String("NotebookEdit")) return firstLines(str("new_source"), 10);
    return firstLines(QString::fromUtf8(QJsonDocument(input).toJson(QJsonDocument::Indented)), 12);
}

QString failureOf(const QString& subtype, const QString& text)
{
    if (subtype == QLatin1String("error_max_turns")) return tr("Claude stopped: the turn took too many steps");
    if (subtype == QLatin1String("error_during_execution")) return tr("The turn was stopped");
    if (!text.trimmed().isEmpty()) return oneLine(text, 200);
    return tr("The turn failed (%1)").arg(subtype);
}

} // namespace

// ----------------------------------------------------------------------
QString stateText(State state)
{
    switch (state) {
    case State::NotFound: return tr("not installed");
    case State::Off: return tr("off");
    case State::Starting: return tr("starting");
    case State::Thinking: return tr("thinking");
    case State::Working: return tr("working");
    case State::Waiting: return tr("needs you");
    case State::Ready: return tr("ready");
    case State::Failed: return tr("failed");
    }
    return {};
}

QStringList arguments(const Options& options)
{
    QStringList args = {QStringLiteral("-p"),
                        QStringLiteral("--input-format"), QStringLiteral("stream-json"),
                        QStringLiteral("--output-format"), QStringLiteral("stream-json"),
                        QStringLiteral("--verbose"),
                        QStringLiteral("--include-partial-messages"),
                        // The permission prompts come to us, and the dock asks.
                        QStringLiteral("--permission-prompt-tool"), QStringLiteral("stdio")};
    // Asking is the program's own default: not named, which works with
    // the names of older versions ("default") and newer ("manual").
    if (!options.permissionMode.isEmpty())
        args << QStringLiteral("--permission-mode") << options.permissionMode;
    if (!options.model.isEmpty()) args << QStringLiteral("--model") << options.model;
    if (!options.resume.isEmpty()) args << QStringLiteral("--resume") << options.resume;
    if (!options.appendSystemPrompt.isEmpty())
        args << QStringLiteral("--append-system-prompt") << options.appendSystemPrompt;
    return args;
}

QString findProgram(const QString& configured, const QString& home)
{
    const auto executable = [](const QString& path) {
        const QFileInfo info(path);
        return info.isFile() && info.isExecutable();
    };
    const QString wanted = configured.trimmed();
    if (!wanted.isEmpty()) {
        if (QFileInfo(wanted).isAbsolute())
            return executable(wanted) ? QFileInfo(wanted).absoluteFilePath() : QString();
        return QStandardPaths::findExecutable(wanted);
    }
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("claude"));
    if (!onPath.isEmpty()) return onPath;

    // Where the installers put it, for a PATH that does not say.
    const QString h = home.isEmpty() ? QDir::homePath() : home;
#ifdef Q_OS_WIN
    const QStringList candidates = {h + QStringLiteral("/.local/bin/claude.exe"),
                                    h + QStringLiteral("/.claude/local/claude.exe"),
                                    h + QStringLiteral("/AppData/Roaming/npm/claude.cmd")};
#else
    const QStringList candidates = {h + QStringLiteral("/.local/bin/claude"),
                                    h + QStringLiteral("/.claude/local/claude"),
                                    h + QStringLiteral("/.npm-global/bin/claude"),
                                    h + QStringLiteral("/.bun/bin/claude"),
                                    QStringLiteral("/opt/homebrew/bin/claude"),
                                    QStringLiteral("/usr/local/bin/claude"),
                                    QStringLiteral("/usr/bin/claude")};
#endif
    for (const QString& candidate : candidates)
        if (executable(candidate)) return candidate;
    return {};
}

QString toolSubject(const QString& tool, const QJsonObject& input, const QString& workDir)
{
    const auto str = [&input](const char* key) { return input.value(QLatin1String(key)).toString(); };
    const auto relative = [&workDir](const QString& path) {
        if (workDir.isEmpty() || path.isEmpty() || !QFileInfo(path).isAbsolute()) return path;
        const QString rel = QDir(workDir).relativeFilePath(path);
        return rel.startsWith(QLatin1String("..")) ? path : rel;
    };
    QString subject;
    if (tool == QLatin1String("Bash") || tool == QLatin1String("PowerShell")) subject = str("command");
    else if (isFileTool(tool) || tool == QLatin1String("Read")) subject = relative(fileOf(input));
    else if (tool == QLatin1String("Glob")) subject = str("pattern");
    else if (tool == QLatin1String("Grep")) {
        subject = str("pattern");
        if (!str("path").isEmpty()) subject += tr("  in %1").arg(relative(str("path")));
    } else if (tool == QLatin1String("LS")) subject = relative(str("path"));
    else if (tool == QLatin1String("WebFetch")) subject = str("url");
    else if (tool == QLatin1String("WebSearch")) subject = str("query");
    else if (tool == QLatin1String("Task") || tool == QLatin1String("Agent")) subject = str("description");
    else if (tool == QLatin1String("TodoWrite")) subject = tr("the to-do list");
    else if (!input.isEmpty()) subject = QString::fromUtf8(QJsonDocument(input).toJson(QJsonDocument::Compact));
    return oneLine(subject);
}

QString qucsSystemPrompt()
{
    return QStringLiteral(
        "You are running inside the Claude Code panel of Qucs-S, a circuit simulator with a "
        "schematic editor. The user sees your replies in that panel, rendered as Markdown. "
        "Qucs-S keeps schematics in .sch files (its own text format: <Components>, <Wires>, "
        "<Diagrams> sections), symbols in .sym, data displays in .dpl and simulation results "
        "in .dat datasets; a project is a folder whose name ends in _prj. It netlists for "
        "ngspice, Xyce and Qucsator. When the user refers to \"this schematic\" or \"the "
        "open document\", the prompt names the file. Qucs-S reloads a document you change "
        "unless it has unsaved changes of its own: say which files you changed.");
}

// ----------------------------------------------------------------------
Session::Session(QObject* parent) : QObject(parent), a_stopTimer(new QTimer(this))
{
    a_stopTimer->setSingleShot(true);
    // An interrupted turn that does not end: the program goes.
    connect(a_stopTimer, &QTimer::timeout, this, [this] {
        if (a_busy) stop();
    });
    a_systemPrompt = qucsSystemPrompt();
}

Session::~Session()
{
    if (a_process != nullptr) {
        a_process->disconnect(this);
        a_process->kill();
        a_process->waitForFinished(1000);
    }
}

void Session::setProgram(const QString& program)
{
    a_program = program;
    if (a_program.isEmpty() && !isRunning()) setState(State::NotFound);
    else if (!a_program.isEmpty() && a_state == State::NotFound) setState(State::Off);
}

void Session::setWorkingDirectory(const QString& dir)
{
    const QString clean = QDir::cleanPath(dir);
    if (clean == a_workDir) return;
    stop();
    a_workDir = clean;
    a_sessionId.clear();   // a conversation belongs to its directory
    a_modelInUse.clear();
    a_conversationCost = 0.0;
    if (a_state != State::NotFound) setState(State::Off);
}

void Session::setPermissionMode(const QString& mode)
{
    if (mode == a_mode) return;
    a_mode = mode;
    if (!isRunning()) return;
    if (a_busy) a_restartAfterTurn = true;
    else stop();
}

void Session::setModel(const QString& model)
{
    if (model == a_model) return;
    a_model = model;
    if (!isRunning()) return;
    if (a_busy) a_restartAfterTurn = true;
    else stop();
}

bool Session::isBusy() const
{
    return a_busy;
}

bool Session::isRunning() const
{
    return a_process != nullptr && a_process->state() != QProcess::NotRunning;
}

qint64 Session::turnElapsed() const
{
    return a_busy && a_turnClock.isValid() ? a_turnClock.elapsed() : 0;
}

void Session::setState(State state, const QString& detail)
{
    if (state == a_state && detail == a_detail) return;
    a_state = state;
    a_detail = detail;
    emit stateChanged(state, detail);
}

// ----------------------------------------------------------------------
void Session::start()
{
    if (!a_workDir.isEmpty() && !QFileInfo(a_workDir).isDir()) {
        const QString message = tr("The folder Claude works in, %1, does not exist.").arg(QDir::toNativeSeparators(a_workDir));
        setState(State::Failed, message);
        emit failed(message);
        return;
    }
    auto* process = new QProcess(this);
    process->setProgram(a_program);
    process->setArguments(arguments({a_mode, a_model, a_sessionId, a_systemPrompt}));
    if (!a_workDir.isEmpty()) process->setWorkingDirectory(a_workDir);
    connect(process, &QProcess::readyReadStandardOutput, this, &Session::readOutput);
    connect(process, &QProcess::readyReadStandardError, this, [this, process] {
        a_stderr += process->readAllStandardError();
        if (a_stderr.size() > 4096) a_stderr = a_stderr.right(4096);
    });
    connect(process, &QProcess::finished, this, [this](int exitCode) { processFinished(exitCode); });
    a_process = process;
    a_buffer.clear();
    a_stderr.clear();
    a_stopping = false;
    a_reportedCost = 0.0;
    setState(State::Starting);
    process->start();
    if (!process->waitForStarted(5000)) {
        const QString message = tr("Claude Code could not be started (%1): %2")
                                    .arg(QDir::toNativeSeparators(a_program), process->errorString());
        process->disconnect(this);
        process->deleteLater();
        a_process = nullptr;
        setState(State::Failed, message);
        emit failed(message);
    }
}

void Session::write(const QJsonObject& message)
{
    if (!isRunning()) return;
    a_process->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}

bool Session::send(const QString& prompt)
{
    if (a_busy || prompt.trimmed().isEmpty()) return false;
    if (a_program.isEmpty()) {
        setState(State::NotFound);
        return false;
    }
    if (!isRunning()) {
        start();
        if (!isRunning()) return false;
    }
    a_busy = true;
    a_interrupted = false;
    a_turnClock.start();
    a_tools.clear();
    a_editedFiles.clear();
    a_changedFiles.clear();
    a_streamed.clear();
    a_streaming = false;
    write({{QStringLiteral("type"), QStringLiteral("user")},
           {QStringLiteral("message"), QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                                   {QStringLiteral("content"), prompt}}},
           {QStringLiteral("parent_tool_use_id"), QJsonValue::Null},
           {QStringLiteral("session_id"), a_sessionId}});
    setState(State::Thinking);
    return true;
}

void Session::answer(const QString& id, bool allow, bool allowEdits)
{
    if (!a_pending.contains(id)) return;
    const PermissionRequest request = a_pending.take(id);
    QJsonObject response;
    if (allow) {
        response = {{QStringLiteral("behavior"), QStringLiteral("allow")},
                    {QStringLiteral("updatedInput"), request.input}};
        if (allowEdits) {
            // As the program suggested: edits need no asking for the rest
            // of the session.
            response.insert(QStringLiteral("updatedPermissions"),
                            QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("setMode")},
                                                   {QStringLiteral("mode"), QStringLiteral("acceptEdits")},
                                                   {QStringLiteral("destination"), QStringLiteral("session")}}});
            a_mode = QStringLiteral("acceptEdits");
            emit permissionModeChanged(a_mode);
        }
    } else {
        response = {{QStringLiteral("behavior"), QStringLiteral("deny")},
                    {QStringLiteral("message"), tr("The user did not allow this.")}};
    }
    write({{QStringLiteral("type"), QStringLiteral("control_response")},
           {QStringLiteral("response"), QJsonObject{{QStringLiteral("subtype"), QStringLiteral("success")},
                                                    {QStringLiteral("request_id"), id},
                                                    {QStringLiteral("response"), response}}}});
    if (!a_busy) return;
    if (!a_pending.isEmpty()) setState(State::Waiting, a_pending.cbegin().value().tool);
    else if (allow) setState(State::Working, request.tool);
    else setState(State::Thinking);
}

void Session::withdrawRequests()
{
    const QStringList ids = a_pending.keys();
    a_pending.clear();
    for (const QString& id : ids) emit permissionWithdrawn(id);
}

void Session::interrupt()
{
    if (!a_busy) return;
    if (!isRunning()) {
        endTurn();
        return;
    }
    withdrawRequests();
    a_interrupted = true;
    static int count = 0;
    write({{QStringLiteral("type"), QStringLiteral("control_request")},
           {QStringLiteral("request_id"), QStringLiteral("qucs-interrupt-%1").arg(++count)},
           {QStringLiteral("request"), QJsonObject{{QStringLiteral("subtype"), QStringLiteral("interrupt")}}}});
    a_stopTimer->start(4000);
}

void Session::stop()
{
    QProcess* process = a_process;
    if (process == nullptr) {
        if (a_busy) endTurn();
        return;
    }
    a_stopping = true;
    process->closeWriteChannel();   // the end of its input: the program ends
    if (process->state() != QProcess::NotRunning && !process->waitForFinished(1500)) {
        process->terminate();
        if (!process->waitForFinished(1500)) {
            process->kill();
            process->waitForFinished(1000);
        }
    }
    // processFinished() has run (the finished signal).
}

void Session::reset()
{
    stop();
    endTurn();   // and any permission request asked without a program
    a_sessionId.clear();
    a_conversationCost = 0.0;
    a_modelInUse.clear();
    setState(a_program.isEmpty() ? State::NotFound : State::Off);
}

void Session::endTurn()
{
    a_busy = false;
    a_stopTimer->stop();
    a_streaming = false;
    withdrawRequests();
}

// ----------------------------------------------------------------------
void Session::readOutput()
{
    if (a_process == nullptr) return;
    a_buffer += a_process->readAllStandardOutput();
    qsizetype newline;
    while ((newline = a_buffer.indexOf('\n')) >= 0) {
        const QByteArray line = a_buffer.left(newline);
        a_buffer.remove(0, newline + 1);
        handleLine(line);
    }
}

void Session::processFinished(int exitCode)
{
    QProcess* process = a_process;
    if (process == nullptr) return;
    // What it wrote last.
    a_buffer += process->readAllStandardOutput();
    a_stderr += process->readAllStandardError();
    a_process = nullptr;
    process->deleteLater();
    const QByteArray rest = a_buffer;
    a_buffer.clear();
    for (const QByteArray& line : rest.split('\n')) handleLine(line);

    const bool wasBusy = a_busy;
    const bool stopping = a_stopping;
    a_stopping = false;
    endTurn();
    if (stopping || (!wasBusy && exitCode == 0)) {
        if (a_state == State::Starting || a_state == State::Thinking || a_state == State::Working
            || a_state == State::Waiting)
            setState(State::Off);
        return;
    }
    QString message = tr("Claude Code ended unexpectedly (exit code %1).").arg(exitCode);
    const QString err = QString::fromUtf8(a_stderr).trimmed();
    if (!err.isEmpty()) message += QLatin1Char('\n') + firstLines(err, 12);
    setState(State::Failed, oneLine(err.isEmpty() ? message : err, 200));
    emit failed(message);
}

void Session::handleLine(const QByteArray& line)
{
    const QByteArray text = line.trimmed();
    if (text.isEmpty()) return;
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(text, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        // Not the protocol: a warning the program printed.
        emit notice(oneLine(QString::fromUtf8(text), 300));
        return;
    }
    const QJsonObject m = doc.object();
    const QString type = m.value(QLatin1String("type")).toString();
    if (type == QLatin1String("system")) handleSystem(m);
    else if (type == QLatin1String("stream_event")) handleStreamEvent(m);
    else if (type == QLatin1String("assistant")) handleAssistant(m);
    else if (type == QLatin1String("user")) handleUser(m);
    else if (type == QLatin1String("control_request")) handleControlRequest(m);
    else if (type == QLatin1String("control_cancel_request")) {
        const QString id = m.value(QLatin1String("request_id")).toString();
        if (a_pending.remove(id) > 0) emit permissionWithdrawn(id);
    } else if (type == QLatin1String("result")) handleResult(m);
    else if (type == QLatin1String("rate_limit_event")) {
        const QJsonObject info = m.value(QLatin1String("rate_limit_info")).toObject();
        const QString status = info.value(QLatin1String("status")).toString();
        if (!status.isEmpty() && status != QLatin1String("allowed") && status != QLatin1String("allowed_warning")) {
            const qint64 resets = qint64(info.value(QLatin1String("resetsAt")).toDouble());
            emit notice(resets > 0 ? tr("Usage limit reached; it resets at %1.")
                                         .arg(QLocale().toString(QDateTime::fromSecsSinceEpoch(resets).time(),
                                                                 QLocale::ShortFormat))
                                   : tr("Usage limit reached."));
        }
    }
}

void Session::handleSystem(const QJsonObject& m)
{
    const QString subtype = m.value(QLatin1String("subtype")).toString();
    if (subtype == QLatin1String("init")) {
        a_sessionId = m.value(QLatin1String("session_id")).toString();
        a_modelInUse = m.value(QLatin1String("model")).toString();
        a_version = m.value(QLatin1String("claude_code_version")).toString();
        emit sessionStarted(a_sessionId, a_modelInUse);
        if (a_busy && a_state == State::Starting) setState(State::Thinking);
    } else if (subtype == QLatin1String("permission_denied")) {
        const QString message = m.value(QLatin1String("message")).toString();
        emit notice(message.isEmpty() ? tr("%1 was not allowed.").arg(m.value(QLatin1String("tool_name")).toString())
                                      : message);
    } else if (subtype == QLatin1String("status")) {
        if (a_busy && a_state == State::Starting) setState(State::Thinking);
    }
}

void Session::handleStreamEvent(const QJsonObject& m)
{
    if (fromSubagent(m)) return;
    const QJsonObject event = m.value(QLatin1String("event")).toObject();
    const QString type = event.value(QLatin1String("type")).toString();
    if (type == QLatin1String("content_block_start")) {
        const QJsonObject block = event.value(QLatin1String("content_block")).toObject();
        const QString kind = block.value(QLatin1String("type")).toString();
        if (kind == QLatin1String("text")) {
            a_streamed.clear();
            a_streaming = true;
            if (a_busy && a_state != State::Thinking) setState(State::Thinking);
        } else if (kind == QLatin1String("tool_use") && a_busy) {
            setState(State::Working, block.value(QLatin1String("name")).toString());
        }
    } else if (type == QLatin1String("content_block_delta")) {
        const QJsonObject delta = event.value(QLatin1String("delta")).toObject();
        if (a_streaming && delta.value(QLatin1String("type")).toString() == QLatin1String("text_delta")) {
            a_streamed += delta.value(QLatin1String("text")).toString();
            emit replyStreamed(a_streamed);
        }
    }
}

void Session::handleAssistant(const QJsonObject& m)
{
    const bool subagent = fromSubagent(m);
    const QJsonArray content = m.value(QLatin1String("message")).toObject().value(QLatin1String("content")).toArray();
    for (const QJsonValue& v : content) {
        const QJsonObject block = v.toObject();
        const QString kind = block.value(QLatin1String("type")).toString();
        if (subagent) {
            // An agent Claude started: only which tool it uses, for the state.
            if (kind == QLatin1String("tool_use") && a_busy)
                setState(State::Working, block.value(QLatin1String("name")).toString());
            continue;
        }
        if (kind == QLatin1String("text")) {
            a_streaming = false;
            a_streamed.clear();
            const QString text = block.value(QLatin1String("text")).toString();
            if (!text.trimmed().isEmpty()) emit replyFinished(text);
        } else if (kind == QLatin1String("tool_use")) {
            const QString id = block.value(QLatin1String("id")).toString();
            const QString tool = block.value(QLatin1String("name")).toString();
            const QJsonObject input = block.value(QLatin1String("input")).toObject();
            a_tools.insert(id, tool);
            if (isFileTool(tool)) a_editedFiles.insert(id, fileOf(input));
            emit toolStarted(id, tool, toolSubject(tool, input, a_workDir));
            if (a_busy) setState(State::Working, tool);
        }
    }
}

void Session::handleUser(const QJsonObject& m)
{
    if (fromSubagent(m)) return;
    const QJsonValue content = m.value(QLatin1String("message")).toObject().value(QLatin1String("content"));
    for (const QJsonValue& v : content.toArray()) {
        const QJsonObject block = v.toObject();
        if (block.value(QLatin1String("type")).toString() != QLatin1String("tool_result")) continue;
        const QString id = block.value(QLatin1String("tool_use_id")).toString();
        const bool failed = block.value(QLatin1String("is_error")).toBool();
        if (!failed && a_editedFiles.contains(id)) {
            const QString file = QDir::cleanPath(a_editedFiles.value(id));
            if (!file.isEmpty() && !a_changedFiles.contains(file)) a_changedFiles << file;
        }
        a_editedFiles.remove(id);
        emit toolFinished(id, failed, textOf(block.value(QLatin1String("content"))));
    }
    if (a_busy && a_state == State::Working && a_pending.isEmpty()) setState(State::Thinking);
}

void Session::handleControlRequest(const QJsonObject& m)
{
    const QString id = m.value(QLatin1String("request_id")).toString();
    const QJsonObject request = m.value(QLatin1String("request")).toObject();
    const QString subtype = request.value(QLatin1String("subtype")).toString();
    if (subtype != QLatin1String("can_use_tool")) {
        // Nothing else is asked of this host.
        write({{QStringLiteral("type"), QStringLiteral("control_response")},
               {QStringLiteral("response"), QJsonObject{{QStringLiteral("subtype"), QStringLiteral("error")},
                                                        {QStringLiteral("request_id"), id},
                                                        {QStringLiteral("error"), QStringLiteral("Not supported by Qucs-S")}}}});
        return;
    }
    PermissionRequest p;
    p.id = id;
    p.tool = request.value(QLatin1String("tool_name")).toString();
    p.input = request.value(QLatin1String("input")).toObject();
    p.action = actionOf(p.tool);
    p.subject = toolSubject(p.tool, p.input, a_workDir);
    p.detail = detailOf(p.tool, p.input);
    for (const QJsonValue& s : request.value(QLatin1String("permission_suggestions")).toArray()) {
        const QJsonObject suggestion = s.toObject();
        if (suggestion.value(QLatin1String("type")).toString() == QLatin1String("setMode")
            && suggestion.value(QLatin1String("mode")).toString() == QLatin1String("acceptEdits"))
            p.canAllowEdits = true;
    }
    a_pending.insert(id, p);
    setState(State::Waiting, p.tool);
    emit permissionRequested(p);
}

void Session::handleResult(const QJsonObject& m)
{
    TurnResult r;
    r.subtype = m.value(QLatin1String("subtype")).toString();
    r.ok = !m.value(QLatin1String("is_error")).toBool() && r.subtype == QLatin1String("success");
    r.text = m.value(QLatin1String("result")).toString();
    r.durationMs = qint64(m.value(QLatin1String("duration_ms")).toDouble());
    // The program reports what it has cost since it started: the turn's
    // is what was added.
    const double reported = m.value(QLatin1String("total_cost_usd")).toDouble();
    r.costUsd = std::max(0.0, reported - a_reportedCost);
    a_reportedCost = std::max(a_reportedCost, reported);
    a_conversationCost += r.costUsd;
    r.conversationCostUsd = a_conversationCost;
    r.turns = m.value(QLatin1String("num_turns")).toInt();
    r.denials = int(m.value(QLatin1String("permission_denials")).toArray().size());
    r.changedFiles = a_changedFiles;
    r.stopped = a_interrupted && !r.ok;
    const QString sessionId = m.value(QLatin1String("session_id")).toString();
    if (!sessionId.isEmpty()) a_sessionId = sessionId;

    endTurn();
    if (r.ok || r.stopped) setState(State::Ready);
    else setState(State::Failed, failureOf(r.subtype, r.text));
    emit turnFinished(r);
    if (a_restartAfterTurn) {
        // A mode or model changed during the turn: the next prompt starts
        // the program with it.
        a_restartAfterTurn = false;
        stop();
    }
}

} // namespace qucs_s::claude
