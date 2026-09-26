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
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
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

// More of what a tool is asked than its subject: a command, a file's
// content, an edit. \a scale: how many lines of it, from the permission
// card's few (1) up.
QString detailOf(const QString& tool, const QJsonObject& input, int scale = 1)
{
    const auto str = [&input](const char* key) { return input.value(QLatin1String(key)).toString(); };
    if (tool == QLatin1String("Bash") || tool == QLatin1String("PowerShell")) {
        const QString description = str("description");
        return firstLines(str("command"), 12 * scale) + (description.isEmpty() ? QString() : QStringLiteral("\n# ") + description);
    }
    if (tool == QLatin1String("Write")) return firstLines(str("content"), 14 * scale);
    if (tool == QLatin1String("Read") || tool == QLatin1String("Glob") || tool == QLatin1String("LS")) return {};
    if (tool == QLatin1String("Grep")) {
        QStringList parts{str("pattern")};
        if (!str("path").isEmpty()) parts << tr("in %1").arg(str("path"));
        if (!str("glob").isEmpty()) parts << tr("files %1").arg(str("glob"));
        return parts.join(QLatin1Char(' '));
    }
    if (tool == QLatin1String("Task") || tool == QLatin1String("Agent")) return firstLines(str("prompt"), 6 * scale);
    if (tool == QLatin1String("Edit")) {
        const auto marked = [scale](const QString& text, QChar mark) {
            QStringList lines = firstLines(text, 7 * scale).split(QLatin1Char('\n'));
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
    if (tool == QLatin1String("NotebookEdit")) return firstLines(str("new_source"), 10 * scale);
    return firstLines(QString::fromUtf8(QJsonDocument(input).toJson(QJsonDocument::Indented)), 12 * scale);
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
    if (!options.mcpConfig.isEmpty()) args << QStringLiteral("--mcp-config") << options.mcpConfig;
    if (!options.allowedTools.isEmpty())
        args << QStringLiteral("--allowedTools") << options.allowedTools.join(QLatin1Char(','));
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
        "unless it has unsaved changes of its own: say which files you changed. The panel typesets "
        "TeX math between $...$ (inline) and $$...$$ (display): write formulas that way, not as code. "
        "The mcp__qucs__ tools drive the Qucs-S window the user sees: open, show, save and close documents, "
        "read and change the schematic in front (components, wires, labels, properties), take a screenshot of it, "
        "use menu actions and answer their dialogs, run a simulation. When the user asks for something to be "
        "done in Qucs-S, or about what is open in it, use them (load them with ToolSearch); prefer them to editing "
        "the file of a schematic that is open.");
}

bool isAskMode(const QString& mode)
{
    return mode.isEmpty() || mode == QLatin1String("default") || mode == QLatin1String("manual");
}

QString modelName(const QString& id)
{
    QString name = id.trimmed();
    if (name.isEmpty()) return {};
    if (name.startsWith(QLatin1String("claude-"))) name = name.mid(7);
    name.remove(QRegularExpression(QStringLiteral("-\\d{8}$")));
    name.remove(QRegularExpression(QStringLiteral("\\[.*\\]$")));
    QStringList parts = name.split(QLatin1Char('-'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) return id;
    QString family = parts.takeFirst();
    family[0] = family[0].toUpper();
    return parts.isEmpty() ? family : family + QLatin1Char(' ') + parts.join(QLatin1Char('.'));
}

namespace {

// "claude-opus-5[1m]", "claude-haiku-4-5-20251001": claude-opus-5,
// claude-haiku-4-5 - the model, whatever its context or its date.
QString baseModel(const QString& id)
{
    QString base = id.trimmed();
    base.remove(QRegularExpression(QStringLiteral("\\[.*\\]$")));
    base.remove(QRegularExpression(QStringLiteral("-\\d{8}$")));
    return base;
}

// The newest of each family, for a program that does not offer them (its
// list has aliases, which stand for what its version thought newest).
const struct {
    const char* id;
    const char* name;
    bool autoMode;
} kNewestModels[] = {
    {"claude-opus-5-5", "Opus 5.5", true},
    {"claude-fable-5-1", "Fable 5.1", true},
    {"claude-sonnet-5", "Sonnet 5", true},
    {"claude-haiku-4-5", "Haiku 4.5", false},
};

const char* const kModelsRequest = "qucs-models";

} // namespace

QList<ModelChoice> modelChoices(const QJsonArray& listed)
{
    // "Opus 5 with 1M context · Best for everyday, complex tasks"
    const QString dot = QStringLiteral(" \u00b7 ");
    QList<ModelChoice> choices;
    const auto has = [&choices](const QString& value) {
        return std::any_of(choices.cbegin(), choices.cend(), [&value](const ModelChoice& c) { return c.value == value; });
    };
    for (const QJsonValue& v : listed) {
        const QJsonObject o = v.toObject();
        ModelChoice c;
        c.value = o.value(QLatin1String("value")).toString().trimmed();
        if (c.value == QLatin1String("default")) c.value.clear();
        if (has(c.value)) continue;
        c.resolved = o.value(QLatin1String("resolvedModel")).toString();
        c.autoMode = o.value(QLatin1String("supportsAutoMode")).toBool();
        c.listed = true;
        const QString about = o.value(QLatin1String("description")).toString().trimmed();
        const QString what = about.contains(dot) ? about.section(dot, 0, 0).trimmed() : QString();
        c.description = about.contains(dot) ? about.section(dot, 1).trimmed() : about;
        if (c.value.isEmpty()) c.name = what.isEmpty() ? tr("Default") : tr("Default (%1)").arg(what);
        else if (!what.isEmpty()) c.name = what;
        else c.name = modelName(c.resolved.isEmpty() ? c.value : c.resolved);   // "Custom model"
        choices << c;
    }
    if (!has(QString())) {
        ModelChoice c;
        c.name = tr("Default");
        c.description = tr("The model Claude Code chooses");
        c.autoMode = true;
        choices.prepend(c);
    }
    for (const auto& newest : kNewestModels) {
        const QString id = QString::fromLatin1(newest.id);
        if (std::any_of(choices.cbegin(), choices.cend(), [&id](const ModelChoice& c) {
                return c.value == id || (!c.resolved.isEmpty() && baseModel(c.resolved) == id);
            }))
            continue;
        ModelChoice c;
        c.value = id;
        c.name = QString::fromLatin1(newest.name);
        c.description = id;
        c.resolved = id;
        c.autoMode = newest.autoMode;
        choices << c;
    }
    return choices;
}

// ----------------------------------------------------------------------
ModelQuery::ModelQuery(QObject* parent) : QObject(parent), a_timeout(new QTimer(this))
{
    a_timeout->setSingleShot(true);
    connect(a_timeout, &QTimer::timeout, this, [this] {
        if (a_process != nullptr) a_process->kill();   // and finish() as it ends
    });
}

ModelQuery::~ModelQuery()
{
    if (a_process != nullptr) {
        a_process->disconnect(this);
        a_process->kill();
        a_process->waitForFinished(1000);
    }
}

void ModelQuery::start(const QString& program, const QString& dir)
{
    if (a_process != nullptr || program.isEmpty()) return;
    auto* process = new QProcess(this);
    process->setProgram(program);
    process->setArguments({QStringLiteral("-p"), QStringLiteral("--input-format"), QStringLiteral("stream-json"),
                           QStringLiteral("--output-format"), QStringLiteral("stream-json"), QStringLiteral("--verbose")});
    process->setWorkingDirectory(!dir.isEmpty() && QFileInfo(dir).isDir() ? dir : QDir::homePath());
    process->setStandardErrorFile(QProcess::nullDevice());
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
        a_output += process->readAllStandardOutput();
        if (a_output.size() > (8 << 20)) process->kill();   // not what was asked for
    });
    connect(process, &QProcess::finished, this, &ModelQuery::finish);
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finish();
    });
    a_process = process;
    a_output.clear();
    process->start();
    const QJsonObject request{{QStringLiteral("type"), QStringLiteral("control_request")},
                              {QStringLiteral("request_id"), QString::fromLatin1(kModelsRequest)},
                              {QStringLiteral("request"), QJsonObject{{QStringLiteral("subtype"), QStringLiteral("initialize")}}}};
    process->write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    process->closeWriteChannel();   // it answers, and ends
    a_timeout->start(30000);
}

void ModelQuery::finish()
{
    QProcess* process = a_process;
    if (process == nullptr) return;
    a_process = nullptr;
    a_timeout->stop();
    a_output += process->readAllStandardOutput();
    process->disconnect(this);
    process->deleteLater();
    QJsonArray models;
    for (const QByteArray& line : a_output.split('\n')) {
        const QJsonObject m = QJsonDocument::fromJson(line.trimmed()).object();
        if (m.value(QLatin1String("type")).toString() != QLatin1String("control_response")) continue;
        const QJsonObject response = m.value(QLatin1String("response")).toObject();
        if (response.value(QLatin1String("request_id")).toString() != QLatin1String(kModelsRequest)) continue;
        models = response.value(QLatin1String("response")).toObject().value(QLatin1String("models")).toArray();
    }
    a_output.clear();
    emit finished(models);
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
    a_toolsAllowed = false;
    a_modelInUse.clear();
    a_conversationCost = 0.0;
    if (a_state != State::NotFound) setState(State::Off);
}

void Session::resume(const QString& sessionId)
{
    if (isRunning()) stop();
    a_sessionId = sessionId;
}

void Session::setToolHost(ToolHost* host)
{
    a_host = host;
}

QJsonObject Session::forDocument(const QString& tool, const QJsonObject& input) const
{
    return a_host != nullptr && !a_document.isEmpty() ? a_host->forDocument(tool, input, a_document) : input;
}

QString Session::hostTool(const QString& tool) const
{
    if (a_host == nullptr) return {};
    const QString prefix = QStringLiteral("mcp__") + a_host->serverName() + QStringLiteral("__");
    return tool.startsWith(prefix) ? tool.mid(prefix.size()) : QString();
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
    Options options;
    options.permissionMode = a_mode;
    options.model = a_model;
    options.resume = a_sessionId;
    options.appendSystemPrompt = a_systemPrompt;
    if (a_host != nullptr) {
        // The host's tools, as an "sdk" MCP server: their messages come over
        // this stream. Those that only look need no asking.
        const QString name = a_host->serverName();
        // Always loaded: with its tools deferred behind the program's tool
        // search, Claude spent a turn finding them before the first use.
        options.mcpConfig = QString::fromUtf8(QJsonDocument(QJsonObject{
            {QStringLiteral("mcpServers"),
             QJsonObject{{name, QJsonObject{{QStringLiteral("type"), QStringLiteral("sdk")},
                                            {QStringLiteral("name"), name},
                                            {QStringLiteral("alwaysLoad"), true}}}}}})
                                                  .toJson(QJsonDocument::Compact));
        for (const QString& tool : a_host->readOnlyTools())
            options.allowedTools << QStringLiteral("mcp__") + name + QStringLiteral("__") + tool;
    }
    process->setArguments(arguments(options));
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
    a_resumedFrom = a_sessionId;
    a_initSeen = false;
    a_resumeFailed = false;
    a_stopping = false;
    a_reportedCost = 0.0;
    a_modeInUse.clear();
    setState(State::Starting);
    process->start();
    if (a_host != nullptr) {
        // As the SDKs begin: the servers this host answers for.
        static int count = 0;
        write({{QStringLiteral("type"), QStringLiteral("control_request")},
               {QStringLiteral("request_id"), QStringLiteral("qucs-init-%1").arg(++count)},
               {QStringLiteral("request"), QJsonObject{{QStringLiteral("subtype"), QStringLiteral("initialize")},
                                                       {QStringLiteral("sdkMcpServers"), QJsonArray{a_host->serverName()}}}}});
    }
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
    a_lastPrompt = prompt;
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

void Session::allowRequest(const QString& id, const QJsonObject& input)
{
    write({{QStringLiteral("type"), QStringLiteral("control_response")},
           {QStringLiteral("response"),
            QJsonObject{{QStringLiteral("subtype"), QStringLiteral("success")},
                        {QStringLiteral("request_id"), id},
                        {QStringLiteral("response"), QJsonObject{{QStringLiteral("behavior"), QStringLiteral("allow")},
                                                                 {QStringLiteral("updatedInput"), input}}}}}});
}

void Session::answer(const QString& id, bool allow, bool allowEdits, bool allowTools)
{
    if (!a_pending.contains(id)) return;
    const PermissionRequest request = a_pending.take(id);
    if (allow && allowTools && request.canAllowTools) {
        // The host's tools from now on, and those already asked about.
        a_toolsAllowed = true;
        const QStringList ids = a_pending.keys();
        for (const QString& other : ids) {
            if (!a_pending.value(other).canAllowTools) continue;
            allowRequest(other, a_pending.take(other).input);
            emit permissionWithdrawn(other);
        }
    }
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
            a_modeInUse = a_mode;
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
    a_toolsAllowed = false;
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

    // A conversation it no longer has (its files are kept for a while):
    // the prompt goes to a new one.
    if (a_resumeFailed) {
        a_resumeFailed = false;
        if (QString::fromUtf8(a_stderr).contains(QLatin1String("No conversation found"))) {
            const QString prompt = a_lastPrompt;
            endTurn();
            a_sessionId.clear();
            a_stderr.clear();
            emit notice(tr("Claude Code no longer has the conversation this one continued: Claude begins afresh, "
                           "without what was said before."));
            if (!prompt.isEmpty() && send(prompt)) return;
            setState(State::Off);
            return;
        }
    }

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
        a_initSeen = true;
        a_sessionId = m.value(QLatin1String("session_id")).toString();
        // Its commands, less those only its terminal has.
        QStringList commands;
        const QJsonArray terminal = m.value(QLatin1String("terminal_slash_commands")).toArray();
        for (const QJsonValue& v : m.value(QLatin1String("slash_commands")).toArray())
            if (!terminal.contains(v) && !v.toString().startsWith(QLatin1String("__"))) commands << v.toString();
        if (!commands.isEmpty()) a_slashCommands = commands;
        a_modelInUse = m.value(QLatin1String("model")).toString();
        a_version = m.value(QLatin1String("claude_code_version")).toString();
        // Not every model has every mode (Haiku has no auto mode): the
        // program falls back to asking, and says so only here.
        const QString mode = m.value(QLatin1String("permissionMode")).toString();
        const bool told = !a_modeInUse.isEmpty();
        a_modeInUse = mode;
        emit sessionStarted(a_sessionId, a_modelInUse);
        if (!told && !mode.isEmpty() && mode != a_mode && !(isAskMode(mode) && isAskMode(a_mode))) {
            const QString model = modelName(a_modelInUse);
            if (a_mode == QLatin1String("auto") && isAskMode(mode))
                emit notice(model.isEmpty() ? tr("Auto mode is not available here: Claude asks before it acts.")
                                            : tr("Auto mode is not available with %1: Claude asks before it acts.").arg(model));
            else
                emit notice(tr("Claude Code works in the %1 mode, not %2.").arg(isAskMode(mode) ? tr("asking") : mode,
                                                                                    isAskMode(a_mode) ? tr("asking") : a_mode));
        }
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
            const QString own = hostTool(tool);
            emit toolStarted(id, tool, own.isEmpty() ? toolSubject(tool, input, a_workDir) : a_host->subjectOf(own, forDocument(own, input)),
                             detailOf(tool, input, 4));
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
    if (subtype == QLatin1String("mcp_message")) {
        handleMcpMessage(id, request);
        return;
    }
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
    if (const QString tool = hostTool(p.tool); !tool.isEmpty()) {
        // The host's own tools: those that look, and all of them once
        // allowed (or where edits need no asking), without a question;
        // the others may be allowed all at once.
        const bool editsFree = a_mode == QLatin1String("acceptEdits") || a_mode == QLatin1String("auto")
                               || a_mode == QLatin1String("bypassPermissions");
        if (a_toolsAllowed || editsFree || a_host->readOnlyTools().contains(tool)) {
            allowRequest(id, p.input);
            return;
        }
        p.action = a_host->actionOf(tool);
        p.subject = a_host->subjectOf(tool, forDocument(tool, p.input));
        p.canAllowTools = true;
    }
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

// The host's MCP server, answered here: the program's messages to it come
// as control requests, its answers go back as control responses.
void Session::handleMcpMessage(const QString& requestId, const QJsonObject& request)
{
    const QJsonObject message = request.value(QLatin1String("message")).toObject();
    const QJsonValue messageId = message.value(QLatin1String("id"));
    const QString method = message.value(QLatin1String("method")).toString();
    const QString server = request.value(QLatin1String("server_name")).toString();
    const auto reply = [this, requestId, messageId](const QJsonObject& body, bool error) {
        QJsonObject rpc{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                        {QStringLiteral("id"), messageId.isUndefined() || messageId.isNull() ? QJsonValue(0) : messageId}};
        rpc.insert(error ? QStringLiteral("error") : QStringLiteral("result"), body);
        write({{QStringLiteral("type"), QStringLiteral("control_response")},
               {QStringLiteral("response"),
                QJsonObject{{QStringLiteral("subtype"), QStringLiteral("success")},
                            {QStringLiteral("request_id"), requestId},
                            {QStringLiteral("response"), QJsonObject{{QStringLiteral("mcp_response"), rpc}}}}}});
    };
    const auto failure = [](int code, const QString& text) {
        return QJsonObject{{QStringLiteral("code"), code}, {QStringLiteral("message"), text}};
    };
    if (a_host == nullptr || server != a_host->serverName()) {
        reply(failure(-32601, QStringLiteral("No server %1 here").arg(server)), true);
        return;
    }
    if (method == QLatin1String("initialize")) {
        const QJsonObject params = message.value(QLatin1String("params")).toObject();
        QJsonObject result{{QStringLiteral("protocolVersion"),
                            params.value(QLatin1String("protocolVersion")).toString(QStringLiteral("2025-06-18"))},
                           {QStringLiteral("capabilities"), QJsonObject{{QStringLiteral("tools"), QJsonObject()}}},
                           {QStringLiteral("serverInfo"), QJsonObject{{QStringLiteral("name"), a_host->serverName()},
                                                                      {QStringLiteral("version"), QStringLiteral("1")}}}};
        const QString instructions = a_host->instructions();
        if (!instructions.isEmpty()) result.insert(QStringLiteral("instructions"), instructions);
        reply(result, false);
    } else if (method.startsWith(QLatin1String("notifications/")) || method == QLatin1String("ping")) {
        reply(QJsonObject(), false);
    } else if (method == QLatin1String("tools/list")) {
        reply({{QStringLiteral("tools"), a_host->tools()}}, false);
    } else if (method == QLatin1String("tools/call")) {
        const QJsonObject params = message.value(QLatin1String("params")).toObject();
        const QString tool = params.value(QLatin1String("name")).toString();
        const QJsonObject arguments = params.value(QLatin1String("arguments")).toObject();
        // Not from within the reading of the output: a tool may open a
        // dialog (an event loop of its own) or take its time.
        QPointer<Session> self(this);
        const QProcess* process = a_process;
        QTimer::singleShot(0, this, [self, process, tool, arguments, reply] {
            if (!self || self->a_host == nullptr) return;
            // (The document pinned now: the conversation's when it runs.)
            self->a_host->callToolFor(self->a_caller, tool, self->forDocument(tool, arguments), [self, process, reply](const QJsonObject& result) {
                // (For the program that asked: not one started since.)
                if (self && self->a_process == process) reply(result, false);
            });
        });
    } else {
        reply(failure(-32601, QStringLiteral("Method not found: %1").arg(method)), true);
    }
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
    // Not started at all, continuing one it does not have: said when it
    // ends (processFinished()), and the prompt sent to a new one.
    if (!a_initSeen && !a_resumedFrom.isEmpty() && !r.ok && r.turns == 0) {
        a_resumeFailed = true;
        return;
    }
    if (!sessionId.isEmpty()) a_sessionId = sessionId;

    endTurn();
    if (r.ok || r.stopped) {
        setState(State::Ready);
    } else {
        setState(State::Failed, failureOf(r.subtype, r.text));
    }
    emit turnFinished(r);
    if (a_restartAfterTurn) {
        // A mode or model changed during the turn: the next prompt starts
        // the program with it.
        a_restartAfterTurn = false;
        stop();
    }
}

} // namespace qucs_s::claude
