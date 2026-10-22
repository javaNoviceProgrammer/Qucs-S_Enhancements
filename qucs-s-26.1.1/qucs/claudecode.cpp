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
#include "mcpserver.h"
#include "main.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
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
    // A sign-in that failed or ran out: Claude Code's own words say /login,
    // which the dock (a session in print mode) cannot run - how, added.
    static const QRegularExpression signIn(QStringLiteral("authenticat|/login|\\blog ?in\\b|api[ -]?key|oauth|\\b401\\b|unauthori[sz]ed|credential"),
                                           QRegularExpression::CaseInsensitiveOption);
    if (signIn.match(text).hasMatch())
        return oneLine(text, 200) + QLatin1Char(' ')
               + tr("To sign in again, run claude in a terminal and type /login there (the dock cannot run /login), then send "
                    "the message again.");
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

QByteArray imageForClaude(const QString& path, QString* mediaType, QString* why)
{
    constexpr qint64 kMostBytes = 3932160;   // 3.75 MB: 5 MB in base64
    constexpr int kMostSide = 2000;
    QImageReader reader(path);
    reader.setAutoTransform(true);   // (a photo's orientation)
    const QByteArray format = reader.format().toLower();
    const QSize size = reader.size();
    if (!reader.canRead()) {
        if (why != nullptr) *why = tr("%1 is no image that reads (%2)").arg(QFileInfo(path).fileName(), reader.errorString());
        return {};
    }
    static const QHash<QByteArray, QString> types{{"png", QStringLiteral("image/png")},
                                                  {"jpeg", QStringLiteral("image/jpeg")},
                                                  {"jpg", QStringLiteral("image/jpeg")},
                                                  {"gif", QStringLiteral("image/gif")},
                                                  {"webp", QStringLiteral("image/webp")}};
    // As it is: a type Claude takes, small enough, upright (a JPEG's
    // orientation turned by its reader is written again).
    if (types.contains(format) && QFileInfo(path).size() <= kMostBytes && size.isValid() && size.width() <= kMostSide
        && size.height() <= kMostSide && reader.transformation() == QImageIOHandler::TransformationNone) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            *mediaType = types.value(format);
            return f.readAll();
        }
    }
    QImage image = reader.read();
    if (image.isNull()) {
        if (why != nullptr) *why = tr("%1 is no image that reads (%2)").arg(QFileInfo(path).fileName(), reader.errorString());
        return {};
    }
    if (image.width() > kMostSide || image.height() > kMostSide)
        image = image.scaled(kMostSide, kMostSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    for (int pass = 0; pass < 6; ++pass) {
        for (const char* as : {"PNG", "JPEG"}) {
            QByteArray bytes;
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::WriteOnly);
            const QImage written = qstrcmp(as, "JPEG") == 0 ? image.convertToFormat(QImage::Format_RGB32) : image;
            if (!written.save(&buffer, as, qstrcmp(as, "JPEG") == 0 ? 88 : -1)) continue;
            if (bytes.size() <= kMostBytes) {
                *mediaType = qstrcmp(as, "JPEG") == 0 ? QStringLiteral("image/jpeg") : QStringLiteral("image/png");
                return bytes;
            }
        }
        image = image.scaled(image.size() * 0.7, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    if (why != nullptr) *why = tr("%1 is too large to send").arg(QFileInfo(path).fileName());
    return {};
}

QJsonValue promptContent(const QString& prompt, const QStringList& images)
{
    if (images.isEmpty()) return prompt;
    QJsonArray blocks;
    for (const QString& path : images) {
        QString type;
        const QByteArray bytes = imageForClaude(path, &type);
        if (bytes.isEmpty()) continue;
        blocks.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("image")},
                                  {QStringLiteral("source"), QJsonObject{{QStringLiteral("type"), QStringLiteral("base64")},
                                                                         {QStringLiteral("media_type"), type},
                                                                         {QStringLiteral("data"), QString::fromLatin1(bytes.toBase64())}}}});
    }
    if (!prompt.trimmed().isEmpty() || blocks.isEmpty())
        blocks.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), prompt}});
    return blocks;
}

namespace {
// A whole number a message gives, 0 to \a most (a double past qint64's
// range converted is undefined).
qint64 wholeOf(const QJsonValue& value, double most)
{
    const double v = value.toDouble();
    return std::isfinite(v) && v > 0 ? qint64(std::min(v, most)) : 0;
}
} // namespace

qint64 tokenCount(const QJsonValue& value)
{
    return wholeOf(value, 1e15);
}

TokenUsage& TokenUsage::operator+=(const TokenUsage& other)
{
    static constexpr qint64 most = 1'000'000'000'000'000'000;
    const auto add = [](qint64& a, qint64 b) { a = std::min(most, std::clamp<qint64>(a, 0, most) + std::clamp<qint64>(b, 0, most)); };
    add(input, other.input);
    add(output, other.output);
    add(cacheRead, other.cacheRead);
    add(cacheWrite, other.cacheWrite);
    return *this;
}

TokenUsage tokensOf(const QJsonObject& result, bool* running)
{
    const auto count = [](const QJsonObject& o, const char* name) { return tokenCount(o.value(QLatin1String(name))); };
    TokenUsage tokens;
    const QJsonObject models = result.value(QLatin1String("modelUsage")).toObject();
    if (!models.isEmpty()) {
        for (auto it = models.begin(); it != models.end(); ++it) {
            const QJsonObject m = it.value().toObject();
            tokens += TokenUsage{count(m, "inputTokens"), count(m, "outputTokens"), count(m, "cacheReadInputTokens"),
                                 count(m, "cacheCreationInputTokens")};
        }
        if (running != nullptr) *running = true;
        return tokens;
    }
    const QJsonObject usage = result.value(QLatin1String("usage")).toObject();
    tokens.input = count(usage, "input_tokens");
    tokens.output = count(usage, "output_tokens");
    tokens.cacheRead = count(usage, "cache_read_input_tokens");
    tokens.cacheWrite = count(usage, "cache_creation_input_tokens");
    if (running != nullptr) *running = false;
    return tokens;
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
    if (!options.resume.isEmpty()) {
        args << QStringLiteral("--resume") << options.resume;
        if (!options.resumeAt.isEmpty()) args << QStringLiteral("--resume-session-at") << options.resumeAt;
        if (options.fork) args << QStringLiteral("--fork-session");
    }
    if (!options.appendSystemPrompt.isEmpty())
        args << QStringLiteral("--append-system-prompt") << options.appendSystemPrompt;
    if (!options.mcpConfig.isEmpty()) args << QStringLiteral("--mcp-config") << options.mcpConfig;
    if (!options.allowedTools.isEmpty())
        args << QStringLiteral("--allowedTools") << options.allowedTools.join(QLatin1Char(','));
    // Auto-compact as the dock says, over the user's own settings.
    args << QStringLiteral("--settings")
         << QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("autoCompactEnabled"), options.autoCompact}})
                                  .toJson(QJsonDocument::Compact));
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
    const QString projects = QucsSettings.AnyFolderIsProject
                                 ? QStringLiteral("a project is a folder - any folder of the workspace, or one "
                                                  "opened as a project, not only one named NAME_prj")
                                 : QStringLiteral("a project is a folder whose name ends in _prj");
    return QStringLiteral(
        "You are running inside the Claude Code panel of Qucs-S, a circuit simulator with a "
        "schematic editor. The user sees your replies in that panel, rendered as Markdown. "
        "Qucs-S keeps schematics in .sch files (its own text format: <Components>, <Wires>, "
        "<Diagrams> sections), symbols in .sym, data displays in .dpl and simulation results "
        "in .dat datasets; ") + projects + QStringLiteral(". It netlists for "
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

int longestDescription(const ToolHost& host)
{
    int longest = int(host.instructions().size());
    for (const QJsonValue& tool : host.tools())
        longest = std::max(longest, int(tool.toObject().value(QLatin1String("description")).toString().size()));
    return longest;
}

QProcessEnvironment claudeEnvironment(const QProcessEnvironment& base, const ToolHost* host)
{
    static const QString name = QStringLiteral("CLAUDE_CODE_MAX_MCP_DESCRIPTION_LENGTH");
    QProcessEnvironment env = base;
    if (host == nullptr || env.contains(name)) return env;
    // The server's instructions run past the default: cut there, Claude
    // would not be told how the tools work best.
    const int longest = longestDescription(*host);
    if (longest > kDescriptionCap) env.insert(name, QString::number(longest));
    return env;
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
    a_lastMessageId.clear();
    a_rewindAt.clear();
    a_toolsAllowed = false;
    a_modelInUse.clear();
    a_conversationCost = 0.0;
    a_conversationTokens = {};
    if (a_state != State::NotFound) setState(State::Off);
}

void Session::resume(const QString& sessionId)
{
    if (isRunning()) stop();
    a_sessionId = sessionId;
    a_lastMessageId.clear();   // (not known until said: setLastMessageId())
    a_rewindAt.clear();
}

void Session::rewind(const QString& sessionId, const QString& messageId)
{
    // The program holds the conversation as it was: it ends, and the next
    // prompt starts it at the message before the one edited.
    stop();
    endTurn();
    a_rewoundSession = a_sessionId;
    a_rewoundFrom = a_lastMessageId;
    if (sessionId.isEmpty() || messageId.isEmpty()) {
        a_sessionId.clear();   // (the first prompt: nothing said before it)
        a_rewindAt.clear();
    } else {
        a_sessionId = sessionId;
        a_rewindAt = messageId;
    }
    a_lastMessageId = messageId;
    setState(a_program.isEmpty() ? State::NotFound : State::Off);
}

void Session::setConversationTotals(const TokenUsage& tokens, double costUsd)
{
    a_conversationTokens = tokens;
    a_conversationCost = std::max(0.0, costUsd);
}

void Session::setToolHost(ToolHost* host)
{
    a_host = host;
    delete a_mcp;
    a_mcp = nullptr;
    if (host == nullptr) return;
    a_mcp = new mcp::Server(host, this);
    a_mcp->setCaller(a_caller);
    // A call: not from within the reading of the output (a tool may open a
    // dialog - an event loop of its own - or take its time), for this
    // conversation (what changed since its last call, the document it is
    // pinned to when it runs), answered to the program that asked, not
    // one started since.
    QPointer<Session> self(this);
    a_mcp->setCall([self](const QString& tool, const QJsonObject& arguments, std::function<void(const QJsonObject&)> done) {
        if (!self) return;
        const QProcess* process = self->a_process;
        QTimer::singleShot(0, self, [self, process, tool, arguments, done] {
            if (!self || self->a_host == nullptr) return;
            self->a_host->callToolFor(self->a_caller, tool, self->forDocument(tool, arguments), [self, process, done](const QJsonObject& result) {
                if (self && self->a_process == process) done(result);
            });
        });
    });
    // The server's own messages (a resource changed, a question for the
    // user) go to the program as it sends its own: control requests.
    a_mcp->setSend([self](const QJsonObject& message) {
        if (!self || self->a_process == nullptr || self->a_host == nullptr) return;
        static int count = 0;
        self->write({{QStringLiteral("type"), QStringLiteral("control_request")},
                     {QStringLiteral("request_id"), QStringLiteral("qucs-mcp-%1").arg(++count)},
                     {QStringLiteral("request"), QJsonObject{{QStringLiteral("subtype"), QStringLiteral("mcp_message")},
                                                             {QStringLiteral("server_name"), self->a_host->serverName()},
                                                             {QStringLiteral("message"), message}}}});
    });
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
    // Asking is "default" to every version. The one it works in, it says.
    a_modeInUse.clear();
    if (mode == QLatin1String("bypassPermissions")) {
        // Only from its start (it refuses it as it runs).
        if (a_busy) a_restartAfterTurn = true;
        else stop();
        return;
    }
    askProgram(QStringLiteral("set_permission_mode"),
               {{QStringLiteral("mode"), isAskMode(mode) ? QStringLiteral("default") : mode}});
}

void Session::setModel(const QString& model)
{
    if (model == a_model) return;
    a_model = model;
    if (!isRunning()) return;
    // Which it is ("opus" is a model of its version), the next turn says.
    a_modelInUse.clear();
    askProgram(QStringLiteral("set_model"),
               {{QStringLiteral("model"), model.isEmpty() ? QStringLiteral("default") : model}});
}

void Session::setAutoCompact(bool on)
{
    if (on == a_autoCompact) return;
    a_autoCompact = on;
    if (!isRunning()) return;
    askProgram(QStringLiteral("apply_flag_settings"),
               {{QStringLiteral("settings"), QJsonObject{{QStringLiteral("autoCompactEnabled"), on}}}});
}

void Session::askProgram(const QString& subtype, const QJsonObject& fields)
{
    static int count = 0;
    const QString id = QStringLiteral("qucs-change-%1").arg(++count);
    QJsonObject request = fields;
    request.insert(QStringLiteral("subtype"), subtype);
    a_asked.insert(id, subtype);
    write({{QStringLiteral("type"), QStringLiteral("control_request")},
           {QStringLiteral("request_id"), id},
           {QStringLiteral("request"), request}});
}

void Session::handleControlResponse(const QJsonObject& m)
{
    const QJsonObject response = m.value(QLatin1String("response")).toObject();
    // A tool asked to go to the background (askBackground()): sent, or -
    // not a task yet - asked again when it is one (handleSystem()), or not.
    const QString requestId = response.value(QLatin1String("request_id")).toString();
    if (const QString toolUse = a_backgroundAsked.take(requestId); !toolUse.isEmpty()) {
        const bool ok = response.value(QLatin1String("subtype")).toString() == QLatin1String("success");
        // (Final only when asked of the task: a refusal of a request made
        // before it was one - the task said between the asking and the
        // answer - was taken for final, and not asked again: the hunt of
        // 21 October, B3.)
        const bool ofTask = a_backgroundAskedOfTask.remove(requestId);
        if (ok && response.value(QLatin1String("response")).toObject().value(QLatin1String("backgrounded")).toBool()) return;
        if (ok && a_running.contains(toolUse) && !ofTask) {
            if (a_taskOf.contains(toolUse)) askBackground(toolUse);   // (a task now: again)
            return;                                                   // (its task to come)
        }
        a_backgroundWanted.remove(toolUse);
        const QString error = response.value(QLatin1String("error")).toString().trimmed();
        emit notice(error.isEmpty() ? tr("%1 could not be sent to the background.").arg(a_tools.value(toolUse, tr("The tool")))
                                    : tr("%1 could not be sent to the background: %2").arg(a_tools.value(toolUse, tr("The tool")), oneLine(error, 200)));
        return;
    }
    const QString subtype = a_asked.take(response.value(QLatin1String("request_id")).toString());
    if (subtype.isEmpty()) return;
    if (response.value(QLatin1String("subtype")).toString() == QLatin1String("success")) {
        // The mode it now works in, which newer versions say.
        const QString mode = response.value(QLatin1String("response")).toObject().value(QLatin1String("mode")).toString();
        if (subtype == QLatin1String("set_permission_mode") && !mode.isEmpty()) {
            a_modeInUse = mode;
            reportMode(mode);
            emit modeInUseChanged();
        }
        return;
    }
    // Refused (a version without it, a model it does not know): it is
    // started with it, once the turn is over. What is wrong with a model
    // is said ("Model 'x' not found").
    const QString error = response.value(QLatin1String("error")).toString().trimmed();
    if (subtype == QLatin1String("set_model") && !error.isEmpty()) emit notice(oneLine(error, 300));
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
    options.autoCompact = a_autoCompact;
    options.resume = a_sessionId;
    // A prompt edited: the conversation up to the message before it, as a
    // new session (until the program says it started on it).
    options.resumeAt = a_rewindAt;
    options.fork = !a_rewindAt.isEmpty();
    options.appendSystemPrompt = a_systemPrompt;
    if (a_host != nullptr) {
        // The host's tools, as an "sdk" MCP server: their messages come over
        // this stream. Those that only look need no asking.
        const QString name = a_host->serverName();
        // Its tools say which are in every turn ("anthropic/alwaysLoad":
        // those most sessions use) and which the program's tool search
        // finds when a task needs them - the server as a whole is not
        // loaded in full (that made every turn carry all of them).
        options.mcpConfig = QString::fromUtf8(QJsonDocument(QJsonObject{
            {QStringLiteral("mcpServers"),
             QJsonObject{{name, QJsonObject{{QStringLiteral("type"), QStringLiteral("sdk")}, {QStringLiteral("name"), name}}}}}})
                                                  .toJson(QJsonDocument::Compact));
        for (const QString& tool : a_host->readOnlyTools())
            options.allowedTools << QStringLiteral("mcp__") + name + QStringLiteral("__") + tool;
    }
    process->setArguments(arguments(options));
    process->setProcessEnvironment(claudeEnvironment(QProcessEnvironment::systemEnvironment(), a_host));
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
    a_reportedTokens = {};
    a_modeInUse.clear();
    a_asked.clear();
    a_restartAfterTurn = false;
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

void Session::answerElicitation(const QString& id, const QString& action, const QJsonObject& content)
{
    QJsonObject answer{{QStringLiteral("action"), action}};
    if (action == QLatin1String("accept")) answer.insert(QStringLiteral("content"), content);
    write({{QStringLiteral("type"), QStringLiteral("control_response")},
           {QStringLiteral("response"), QJsonObject{{QStringLiteral("subtype"), QStringLiteral("success")},
                                                    {QStringLiteral("request_id"), id},
                                                    {QStringLiteral("response"), answer}}}});
}

bool Session::send(const QString& prompt, const QStringList& images)
{
    if (a_busy || (prompt.trimmed().isEmpty() && images.isEmpty())) return false;
    return sendContent(promptContent(prompt, images), prompt);
}

bool Session::sendContent(const QJsonValue& content, const QString& prompt)
{
    if (a_program.isEmpty()) {
        setState(State::NotFound);
        return false;
    }
    if (!isRunning()) {
        start();
        if (!isRunning()) return false;
    }
    beginTurn();
    a_lastPrompt = prompt;
    write({{QStringLiteral("type"), QStringLiteral("user")},
           {QStringLiteral("message"), QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                                   {QStringLiteral("content"), content}}},
           {QStringLiteral("parent_tool_use_id"), QJsonValue::Null},
           {QStringLiteral("session_id"), a_sessionId}});
    setState(State::Thinking);
    return true;
}

void Session::beginTurn()
{
    a_busy = true;
    a_interrupted = false;
    a_turnClock.start();
    a_tools.clear();
    a_running.clear();
    a_editedFiles.clear();
    a_changedFiles.clear();
    a_streamed.clear();
    a_streaming = false;
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

namespace {
// The program's own tools it runs on in the background when asked
// (background_tasks): commands, agents.
bool backgroundable(const QString& tool)
{
    return tool == QLatin1String("Bash") || tool == QLatin1String("PowerShell") || tool == QLatin1String("Agent")
           || tool == QLatin1String("Task");
}
} // namespace

bool Session::canSendToBackground() const
{
    if (!a_busy || !isRunning()) return false;
    for (const QString& id : a_running) {
        const auto call = a_hostCalls.constFind(id);
        if (call != a_hostCalls.cend() ? !call->background : backgroundable(a_tools.value(id)) && !a_backgroundWanted.contains(id))
            return true;
    }
    return false;
}

int Session::sendToBackground()
{
    if (!canSendToBackground()) return 0;
    int sent = 0;
    for (const QString& id : std::as_const(a_running)) {
        const auto call = a_hostCalls.find(id);
        if (call != a_hostCalls.end()) {
            if (call->background) continue;
            // Answered now; its own answer, when it comes, a message.
            call->background = true;
            const QString text = tr("The user sent this call to the background: it goes on in Qucs-S, and its result comes "
                                    "in a message of its own when it is done. Do not call it again meanwhile; go on with "
                                    "what does not need it, or end your turn.");
            call->reply({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("id"), call->rpcId},
                         {QStringLiteral("result"),
                          QJsonObject{{QStringLiteral("content"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                                                                          {QStringLiteral("text"), text}}}}}}});
            ++sent;
        } else if (backgroundable(a_tools.value(id)) && !a_backgroundWanted.contains(id)) {
            a_backgroundWanted.insert(id);
            askBackground(id);
            ++sent;
        }
    }
    return sent;
}

bool Session::asksBackground(const QString& id) const
{
    for (const QString& asked : a_backgroundAsked)
        if (asked == id) return true;
    return false;
}

void Session::askBackground(const QString& id)
{
    static int count = 0;
    const QString request = QStringLiteral("qucs-background-%1").arg(++count);
    a_backgroundAsked.insert(request, id);
    if (a_taskOf.contains(id)) a_backgroundAskedOfTask.insert(request);
    write({{QStringLiteral("type"), QStringLiteral("control_request")},
           {QStringLiteral("request_id"), request},
           {QStringLiteral("request"), QJsonObject{{QStringLiteral("subtype"), QStringLiteral("background_tasks")},
                                                   {QStringLiteral("tool_use_id"), id}}}});
}

void Session::hostCallDone(const HostCall& call, const QJsonObject& rpc)
{
    const QJsonObject result = rpc.value(QLatin1String("result")).toObject();
    const bool error = rpc.contains(QLatin1String("error"));
    const bool failed = error || result.value(QLatin1String("isError")).toBool();
    const QJsonArray content = result.value(QLatin1String("content")).toArray();
    const QString output = error ? rpc.value(QLatin1String("error")).toObject().value(QLatin1String("message")).toString() : textOf(content);
    // Its text, and its images as the program takes a prompt's.
    QJsonArray blocks{QJsonObject{
        {QStringLiteral("type"), QStringLiteral("text")},
        {QStringLiteral("text"), (failed ? tr("(Qucs-S: %1, which the user sent to the background, has failed. What it said:)")
                                         : tr("(Qucs-S: %1, which the user sent to the background, has finished. Its result:)"))
                                         .arg(call.tool)
                                     + QStringLiteral("\n\n") + output}}};
    for (const QJsonValue& v : content) {
        const QJsonObject block = v.toObject();
        if (block.value(QLatin1String("type")).toString() != QLatin1String("image")) continue;
        blocks.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("image")},
                                  {QStringLiteral("source"), QJsonObject{{QStringLiteral("type"), QStringLiteral("base64")},
                                                                         {QStringLiteral("media_type"), block.value(QLatin1String("mimeType"))},
                                                                         {QStringLiteral("data"), block.value(QLatin1String("data"))}}}});
    }
    bool told = false;
    if (isRunning()) {
        if (a_busy) {
            // In a turn: the program reads it when it can.
            write({{QStringLiteral("type"), QStringLiteral("user")},
                   {QStringLiteral("message"), QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), blocks}}},
                   {QStringLiteral("parent_tool_use_id"), QJsonValue::Null},
                   {QStringLiteral("session_id"), a_sessionId}});
            told = true;
        } else {
            told = sendContent(blocks, QString());
        }
    }
    emit backgroundFinished(call.tool, failed, output, told);
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
    a_lastMessageId.clear();
    a_rewindAt.clear();
    a_conversationCost = 0.0;
    a_conversationTokens = {};
    a_modelInUse.clear();
    a_toolsAllowed = false;
    setState(a_program.isEmpty() ? State::NotFound : State::Off);
}

void Session::endTurn()
{
    a_busy = false;
    a_stopTimer->stop();
    a_streaming = false;
    a_running.clear();
    a_backgroundWanted.clear();
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
        // A rewind to a message the conversation has not: said, and the
        // conversation as it was (the prompt not sent).
        if (!a_rewindAt.isEmpty() && QString::fromUtf8(a_stderr).contains(QLatin1String("No message found"))) {
            const QString why = oneLine(QString::fromUtf8(a_stderr).trimmed(), 200);
            endTurn();
            a_rewindAt.clear();
            a_sessionId = a_rewoundSession;
            a_lastMessageId = a_rewoundFrom;
            a_stderr.clear();
            setState(State::Off);
            emit rewindFailed(why);
            return;
        }
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

    // The host's calls sent to the background: they go on, but their
    // answers have no program to go to.
    for (auto it = a_hostCalls.cbegin(); it != a_hostCalls.cend(); ++it)
        if (it->background)
            emit notice(tr("%1, sent to the background, goes on in Qucs-S, but Claude will not be given its result: "
                           "Claude Code has ended.").arg(it->tool));
    a_hostCalls.clear();
    a_backgroundAsked.clear();
    a_backgroundAskedOfTask.clear();
    a_tasks.clear();
    a_taskOf.clear();
    a_backgroundTasks.clear();
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
    // A turn the program begins by itself - a task sent to the background
    // has ended, and it goes on -: as busy as one of the user's (Stop stops
    // it, a prompt waits).
    if (!a_busy && isRunning() && (type == QLatin1String("stream_event") || type == QLatin1String("assistant")) && !fromSubagent(m)) {
        beginTurn();
        setState(State::Thinking);
    }
    if (type == QLatin1String("system")) handleSystem(m);
    else if (type == QLatin1String("stream_event")) handleStreamEvent(m);
    else if (type == QLatin1String("assistant")) handleAssistant(m);
    else if (type == QLatin1String("user")) handleUser(m);
    else if (type == QLatin1String("control_request")) handleControlRequest(m);
    else if (type == QLatin1String("control_response")) handleControlResponse(m);
    else if (type == QLatin1String("control_cancel_request")) {
        const QString id = m.value(QLatin1String("request_id")).toString();
        if (a_pending.remove(id) > 0) emit permissionWithdrawn(id);
    } else if (type == QLatin1String("result")) handleResult(m);
    else if (type == QLatin1String("rate_limit_event")) {
        const QJsonObject info = m.value(QLatin1String("rate_limit_info")).toObject();
        const QString status = info.value(QLatin1String("status")).toString();
        if (!status.isEmpty() && status != QLatin1String("allowed") && status != QLatin1String("allowed_warning")) {
            const qint64 resets = wholeOf(info.value(QLatin1String("resetsAt")), 1e11);
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
        a_rewindAt.clear();   // (gone back: the new session goes on from here)
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
        if (!told) reportMode(mode);
        if (a_busy && a_state == State::Starting) setState(State::Thinking);
    } else if (subtype == QLatin1String("permission_denied")) {
        const QString message = m.value(QLatin1String("message")).toString();
        emit notice(message.isEmpty() ? tr("%1 was not allowed.").arg(m.value(QLatin1String("tool_name")).toString())
                                      : message);
    } else if (subtype == QLatin1String("status")) {
        if (a_busy && a_state == State::Starting) setState(State::Thinking);
    } else if (subtype == QLatin1String("task_started")) {
        // A command or an agent the program runs as a task (a moment after
        // it starts): sent to the background before it was one, asked again.
        const QString task = m.value(QLatin1String("task_id")).toString();
        const QString toolUse = m.value(QLatin1String("tool_use_id")).toString();
        a_tasks.insert(task, m.value(QLatin1String("description")).toString());
        if (!toolUse.isEmpty()) a_taskOf.insert(toolUse, task);
        if (m.value(QLatin1String("is_backgrounded")).toBool()) a_backgroundTasks.insert(task);
        else if (a_backgroundWanted.contains(toolUse) && !asksBackground(toolUse)) askBackground(toolUse);
    } else if (subtype == QLatin1String("task_updated")) {
        const QString task = m.value(QLatin1String("task_id")).toString();
        if (m.value(QLatin1String("patch")).toObject().value(QLatin1String("is_backgrounded")).toBool()) a_backgroundTasks.insert(task);
    } else if (subtype == QLatin1String("task_notification")) {
        // One in the background has ended: said (the program goes on with
        // a turn of its own).
        const QString task = m.value(QLatin1String("task_id")).toString();
        const QString what = a_tasks.take(task);
        if (!a_backgroundTasks.remove(task)) return;
        const QString status = m.value(QLatin1String("status")).toString();
        const QString name = what.isEmpty() ? tr("A task") : QStringLiteral("\u201C%1\u201D").arg(oneLine(what, 80));
        emit notice(status == QLatin1String("completed") ? tr("%1, sent to the background, has finished.").arg(name)
                    : status == QLatin1String("failed")  ? tr("%1, sent to the background, has failed.").arg(name)
                                                         : tr("%1, sent to the background, has ended (%2).").arg(name, status));
    }
}

void Session::reportMode(const QString& inUse)
{
    if (inUse.isEmpty() || inUse == a_mode || (isAskMode(inUse) && isAskMode(a_mode))) return;
    const QString model = modelName(a_modelInUse.isEmpty() ? a_model : a_modelInUse);
    if (a_mode == QLatin1String("auto") && isAskMode(inUse))
        emit notice(model.isEmpty() ? tr("Auto mode is not available here: Claude asks before it acts.")
                                    : tr("Auto mode is not available with %1: Claude asks before it acts.").arg(model));
    else
        emit notice(tr("Claude Code works in the %1 mode, not %2.").arg(isAskMode(inUse) ? tr("asking") : inUse,
                                                                            isAskMode(a_mode) ? tr("asking") : a_mode));
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
    if (!subagent && m.value(QLatin1String("uuid")).isString()) a_lastMessageId = m.value(QLatin1String("uuid")).toString();
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
            a_running.insert(id);
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
    if (m.value(QLatin1String("uuid")).isString()) a_lastMessageId = m.value(QLatin1String("uuid")).toString();
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
        a_running.remove(id);
        a_backgroundWanted.remove(id);
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
    if (subtype == QLatin1String("elicitation")) {
        // A question for the user from an MCP server (Qucs-S's own asks
        // before a file is written over): the dock shows it.
        emit elicitationRequested(id, request.value(QLatin1String("mcp_server_name")).toString(),
                                  request.value(QLatin1String("message")).toString(),
                                  request.value(QLatin1String("requested_schema")).toObject());
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
        const bool autonomous = a_mode == QLatin1String("auto") || a_mode == QLatin1String("bypassPermissions");
        const bool editsFree = autonomous || a_mode == QLatin1String("acceptEdits");
        // A use that cannot be undone (files deleted or written over,
        // unsaved changes discarded) is asked about all the same, but where
        // Claude acts on its own.
        const bool lasting = a_host->irreversible(tool, forDocument(tool, p.input));
        // (One asked about every time: also where Claude acts on its own.)
        const QString always = a_host->askedEachTime(tool, forDocument(tool, p.input));
        const bool unasked = !always.isEmpty() ? a_mode == QLatin1String("bypassPermissions")
                                            : (lasting ? autonomous : (a_toolsAllowed || editsFree));
        if (a_host->readOnlyTools().contains(tool) || unasked) {
            allowRequest(id, p.input);
            return;
        }
        p.action = a_host->actionOf(tool);
        p.subject = a_host->subjectOf(tool, forDocument(tool, p.input));
        p.canAllowTools = !lasting && always.isEmpty();
        if (!always.isEmpty()) p.detail = always;
        else if (lasting) p.detail = tr("This cannot be undone: files are deleted or written over, or unsaved changes discarded.");
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

// The host's MCP server (mcpserver.h), answered here: the program's
// messages to it come as control requests, its answers go back as control
// responses.
void Session::handleMcpMessage(const QString& requestId, const QJsonObject& request)
{
    const QJsonObject message = request.value(QLatin1String("message")).toObject();
    const QString server = request.value(QLatin1String("server_name")).toString();
    const auto reply = [this, requestId](const QJsonObject& rpc) {
        write({{QStringLiteral("type"), QStringLiteral("control_response")},
               {QStringLiteral("response"),
                QJsonObject{{QStringLiteral("subtype"), QStringLiteral("success")},
                            {QStringLiteral("request_id"), requestId},
                            {QStringLiteral("response"), QJsonObject{{QStringLiteral("mcp_response"), rpc}}}}}});
    };
    if (a_host == nullptr || a_mcp == nullptr || server != a_host->serverName()) {
        const QJsonValue id = message.value(QLatin1String("id"));
        reply({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
               {QStringLiteral("id"), id.isUndefined() || id.isNull() ? QJsonValue(0) : id},
               {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), -32601},
                                                     {QStringLiteral("message"), QStringLiteral("No server %1 here").arg(server)}}}});
        return;
    }
    // A call of a tool, by the tool use it answers: kept until it is
    // answered - sent to the background, answered at once
    // (sendToBackground()), and its own answer then a message to Claude.
    const QString toolUse = message.value(QLatin1String("method")).toString() == QLatin1String("tools/call")
                                ? message.value(QLatin1String("params")).toObject().value(QLatin1String("_meta")).toObject()
                                      .value(QLatin1String("claudecode/toolUseId")).toString()
                                : QString();
    if (toolUse.isEmpty()) {
        a_mcp->handle(message, reply);
        return;
    }
    a_hostCalls.insert(toolUse, {message.value(QLatin1String("id")),
                                 message.value(QLatin1String("params")).toObject().value(QLatin1String("name")).toString(), reply});
    QPointer<Session> self(this);
    a_mcp->handle(message, [self, toolUse, reply](const QJsonObject& rpc) {
        if (!self) return;
        const auto it = self->a_hostCalls.find(toolUse);
        if (it == self->a_hostCalls.end() || !it->background) {
            if (it != self->a_hostCalls.end()) self->a_hostCalls.erase(it);
            reply(rpc);
            return;
        }
        const HostCall call = *it;
        self->a_hostCalls.erase(it);
        self->hostCallDone(call, rpc);
    });
}

void Session::handleResult(const QJsonObject& m)
{
    TurnResult r;
    r.subtype = m.value(QLatin1String("subtype")).toString();
    r.ok = !m.value(QLatin1String("is_error")).toBool() && r.subtype == QLatin1String("success");
    r.text = m.value(QLatin1String("result")).toString();
    r.durationMs = wholeOf(m.value(QLatin1String("duration_ms")), 1e13);
    // The program reports what it has cost since it started: the turn's
    // is what was added. (Not a sum past any conversation's: taken as
    // what was reported before.)
    double reported = m.value(QLatin1String("total_cost_usd")).toDouble();
    if (!(reported >= 0 && reported < 1e9)) reported = a_reportedCost;
    r.costUsd = std::max(0.0, reported - a_reportedCost);
    a_reportedCost = std::max(a_reportedCost, reported);
    a_conversationCost += r.costUsd;
    r.conversationCostUsd = a_conversationCost;
    // Its tokens likewise, each kind (a result with none, a failure's,
    // takes nothing away).
    bool running = false;
    const TokenUsage reportedTokens = tokensOf(m, &running);
    if (running) {
        const auto added = [](qint64 now, qint64& before) {
            const qint64 turn = std::max<qint64>(0, now - before);
            before = std::max(before, now);
            return turn;
        };
        r.tokens.input = added(reportedTokens.input, a_reportedTokens.input);
        r.tokens.output = added(reportedTokens.output, a_reportedTokens.output);
        r.tokens.cacheRead = added(reportedTokens.cacheRead, a_reportedTokens.cacheRead);
        r.tokens.cacheWrite = added(reportedTokens.cacheWrite, a_reportedTokens.cacheWrite);
    } else {
        r.tokens = reportedTokens;
    }
    a_conversationTokens += r.tokens;
    r.conversationTokens = a_conversationTokens;
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
        // A mode or model it would not take during the turn: the next
        // prompt starts the program with it.
        a_restartAfterTurn = false;
        stop();
    }
}

} // namespace qucs_s::claude
