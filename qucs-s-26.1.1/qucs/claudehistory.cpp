/*
 * claudehistory.cpp - the Claude Code dock's conversations kept on disk, and
 *                     Claude Code's own sessions to continue
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "claudehistory.h"

#include "settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QHash>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace qucs_s::claude::history {

namespace {

QString indexPath()
{
    return QDir(directory()).filePath(QStringLiteral("index.json"));
}

QJsonObject readJson(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

bool writeJson(const QString& path, const QJsonObject& o)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    return f.commit();
}

// An id of ours: a file name, nothing that climbs out of the folder.
bool validId(const QString& id)
{
    if (id.isEmpty() || id.size() > 80) return false;
    return std::all_of(id.cbegin(), id.cend(), [](QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('-'); });
}

Summary summaryOf(const QJsonObject& o)
{
    Summary s;
    s.id = o.value(QLatin1String("id")).toString();
    s.title = o.value(QLatin1String("title")).toString();
    s.sessionId = o.value(QLatin1String("sessionId")).toString();
    s.folder = o.value(QLatin1String("folder")).toString();
    s.updated = QDateTime::fromString(o.value(QLatin1String("updated")).toString(), Qt::ISODateWithMs);
    return s;
}

// A user's prompt as Claude Code keeps it: its text, or empty for what is
// not one (a command's output, a tool's result, what it adds itself).
QString promptOf(const QJsonObject& line)
{
    if (line.value(QLatin1String("type")).toString() != QLatin1String("user")) return {};
    if (line.value(QLatin1String("isMeta")).toBool() || line.value(QLatin1String("isSidechain")).toBool()) return {};
    const QJsonValue content = line.value(QLatin1String("message")).toObject().value(QLatin1String("content"));
    QString text;
    if (content.isString()) {
        text = content.toString();
    } else {
        for (const QJsonValue& v : content.toArray()) {
            const QJsonObject part = v.toObject();
            if (part.value(QLatin1String("type")).toString() == QLatin1String("text"))
                text += part.value(QLatin1String("text")).toString();
        }
    }
    text = text.trimmed();
    if (text.startsWith(QLatin1Char('<')) || text.startsWith(QLatin1String("Caveat:"))) return {};
    return text;
}

} // namespace

QString directory()
{
    const QString forced = qEnvironmentVariable("QUCS_CLAUDE_HISTORY");
    if (!forced.isEmpty()) return forced;
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("claude-conversations"));
}

QList<Summary> saved()
{
    QList<Summary> list;
    for (const QJsonValue& v : readJson(indexPath()).value(QLatin1String("conversations")).toArray()) {
        Summary s = summaryOf(v.toObject());
        if (validId(s.id) && QFileInfo::exists(QDir(directory()).filePath(s.id + QStringLiteral(".json")))) list << s;
    }
    std::stable_sort(list.begin(), list.end(), [](const Summary& a, const Summary& b) { return a.updated > b.updated; });
    return list;
}

QJsonObject load(const QString& id)
{
    if (!validId(id)) return {};
    return readJson(QDir(directory()).filePath(id + QStringLiteral(".json")));
}

bool save(const QString& id, const QJsonObject& conversation, int keep)
{
    if (!validId(id)) return false;
    QJsonObject saved = conversation;
    const QString updated = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    saved.insert(QStringLiteral("id"), id);
    saved.insert(QStringLiteral("updated"), updated);
    if (!writeJson(QDir(directory()).filePath(id + QStringLiteral(".json")), saved)) return false;

    QJsonObject index = readJson(indexPath());
    QJsonArray entries;
    const QJsonObject entry{{QStringLiteral("id"), id},
                            {QStringLiteral("title"), conversation.value(QLatin1String("title"))},
                            {QStringLiteral("sessionId"), conversation.value(QLatin1String("sessionId"))},
                            {QStringLiteral("folder"), conversation.value(QLatin1String("folder"))},
                            {QStringLiteral("updated"), updated}};
    entries.append(entry);
    for (const QJsonValue& v : index.value(QLatin1String("conversations")).toArray())
        if (v.toObject().value(QLatin1String("id")).toString() != id) entries.append(v);
    // The latest kept - and those open, however old.
    const QJsonArray open = index.value(QLatin1String("open")).toArray();
    QJsonArray kept;
    for (const QJsonValue& v : entries) {
        const QString each = v.toObject().value(QLatin1String("id")).toString();
        if (kept.size() < keep || open.contains(QJsonValue(each))) kept.append(v);
        else if (validId(each)) QFile::remove(QDir(directory()).filePath(each + QStringLiteral(".json")));
    }
    index.insert(QStringLiteral("conversations"), kept);
    return writeJson(indexPath(), index);
}

void remove(const QString& id)
{
    if (!validId(id)) return;
    QFile::remove(QDir(directory()).filePath(id + QStringLiteral(".json")));
    QJsonObject index = readJson(indexPath());
    QJsonArray entries;
    for (const QJsonValue& v : index.value(QLatin1String("conversations")).toArray())
        if (v.toObject().value(QLatin1String("id")).toString() != id) entries.append(v);
    index.insert(QStringLiteral("conversations"), entries);
    QJsonArray open;
    for (const QJsonValue& v : index.value(QLatin1String("open")).toArray())
        if (v.toString() != id) open.append(v);
    index.insert(QStringLiteral("open"), open);
    writeJson(indexPath(), index);
}

bool reopenAtStart()
{
    return QucsSettingsFile().value(QStringLiteral("ClaudeCode/reopenConversations"), true).toBool();
}

void setReopenAtStart(bool on)
{
    QucsSettingsFile().setValue(QStringLiteral("ClaudeCode/reopenConversations"), on);
}

QStringList open(QString* current)
{
    const QJsonObject index = readJson(indexPath());
    QStringList ids;
    for (const QJsonValue& v : index.value(QLatin1String("open")).toArray())
        if (validId(v.toString())) ids << v.toString();
    if (current != nullptr) *current = index.value(QLatin1String("current")).toString();
    return ids;
}

void setOpen(const QStringList& ids, const QString& current)
{
    QJsonObject index = readJson(indexPath());
    if (QJsonArray::fromStringList(ids) == index.value(QLatin1String("open")).toArray()
        && current == index.value(QLatin1String("current")).toString())
        return;
    index.insert(QStringLiteral("open"), QJsonArray::fromStringList(ids));
    index.insert(QStringLiteral("current"), current);
    writeJson(indexPath(), index);
}

// ----------------------------------------------------------------------
// Claude Code's own.

QString claudeDirectory()
{
    const QString forced = qEnvironmentVariable("CLAUDE_CONFIG_DIR");
    return forced.isEmpty() ? QDir::home().filePath(QStringLiteral(".claude")) : forced;
}

Summary claudeSessionSummary(const QString& file)
{
    Summary s;
    s.claudeFile = file;
    s.sessionId = QFileInfo(file).completeBaseName();
    s.id = s.sessionId;
    s.updated = QFileInfo(file).lastModified().toUTC();
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return s;
    // Its head for the first prompt and the folder; its tail for a title
    // it was given since (/rename) - not the whole of a long session.
    const QByteArray head = f.read(256 * 1024);
    for (const QByteArray& line : head.split('\n')) {
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        if (o.isEmpty()) continue;
        if (s.folder.isEmpty()) s.folder = o.value(QLatin1String("cwd")).toString();
        if (s.title.isEmpty()) s.title = promptOf(o).simplified().left(120);
        if (!s.folder.isEmpty() && !s.title.isEmpty()) break;
    }
    const qint64 size = f.size();
    if (size > 0) {
        f.seek(std::max<qint64>(0, size - 64 * 1024));
        const QList<QByteArray> tail = f.readAll().split('\n');
        for (auto it = tail.crbegin(); it != tail.crend(); ++it) {
            if (!it->contains("\"custom-title\"")) continue;
            const QString title = QJsonDocument::fromJson(*it).object().value(QLatin1String("customTitle")).toString();
            if (!title.isEmpty()) {
                s.title = title;
                break;
            }
        }
    }
    return s;
}

QList<Summary> claudeSessions(const QString& folder, int limit)
{
    // Claude Code names a folder's sessions' folder by its path, every
    // character not a letter or a digit a dash - the path as the system
    // gives its working directory, links resolved (/tmp is /private/tmp on
    // macOS; a workspace may be a link to another disk). Looked for under
    // that and under the path as spelled, the newest first.
    const QFileInfo info(folder);
    QStringList names;
    for (const QString& path : {info.canonicalFilePath(), info.absoluteFilePath()}) {
        if (path.isEmpty()) continue;
        QString name = QDir::cleanPath(path);
        for (QChar& c : name)
            if (!(c.isLetterOrNumber() && c.unicode() < 128)) c = QLatin1Char('-');
        if (!names.contains(name)) names << name;
    }
    QFileInfoList files;
    for (const QString& name : std::as_const(names)) {
        const QDir dir(QDir(claudeDirectory()).filePath(QStringLiteral("projects/") + name));
        files += dir.entryInfoList({QStringLiteral("*.jsonl")}, QDir::Files, QDir::Time);
    }
    std::stable_sort(files.begin(), files.end(), [](const QFileInfo& a, const QFileInfo& b) { return a.lastModified() > b.lastModified(); });
    QList<Summary> list;
    for (const QFileInfo& info : std::as_const(files)) {
        if (list.size() >= limit) break;
        Summary s = claudeSessionSummary(info.absoluteFilePath());
        if (!s.title.isEmpty()) list << s;   // (one never prompted: nothing to continue)
    }
    return list;
}

QString claudeSessionFile(const QString& sessionId)
{
    if (!validId(sessionId)) return {};
    const QDir projects(QDir(claudeDirectory()).filePath(QStringLiteral("projects")));
    for (const QString& folder : projects.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString file = projects.filePath(folder + QLatin1Char('/') + sessionId + QStringLiteral(".jsonl"));
        if (QFileInfo::exists(file)) return file;
    }
    return {};
}

SessionPoints sessionPoints(const QString& file)
{
    SessionPoints points;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return points;
    static const QRegularExpression typedCommand(QStringLiteral("<command-name>([^<]*)</command-name>"));
    static const QRegularExpression typedArguments(QStringLiteral("<command-args>([^<]*)</command-args>"),
                                                   QRegularExpression::DotMatchesEverythingOption);
    // Each entry of the conversation (a message, an attachment...): whether
    // it is a message - the user's or Claude's - and its parent. The one
    // before a prompt is its nearest parent that is a message.
    QHash<QString, QPair<bool, QString>> chain;
    const auto before = [&chain](QString uuid) {
        for (int guard = 0; !uuid.isEmpty() && guard < 1000000; ++guard) {
            const auto it = chain.constFind(uuid);
            if (it == chain.cend()) return QString();
            if (it->first) return uuid;
            uuid = it->second;
        }
        return QString();
    };
    while (!f.atEnd()) {
        const QJsonObject o = QJsonDocument::fromJson(f.readLine()).object();
        const QString uuid = o.value(QLatin1String("uuid")).toString();
        if (uuid.isEmpty() || o.value(QLatin1String("isSidechain")).toBool()) continue;   // (a subagent's)
        const QString type = o.value(QLatin1String("type")).toString();
        const bool message = type == QLatin1String("user") || type == QLatin1String("assistant");
        const QString parent = o.value(QLatin1String("parentUuid")).toString();
        chain.insert(uuid, {message, parent});
        if (message) points.last = uuid;
        if (type != QLatin1String("user") || o.value(QLatin1String("isMeta")).toBool()) continue;
        // A prompt as the dock shows it (importClaudeSession()): its text; a
        // command as typed; not a tool's result, not what Claude Code adds.
        const QJsonValue content = o.value(QLatin1String("message")).toObject().value(QLatin1String("content"));
        QString text = content.toString();
        for (const QJsonValue& v : content.toArray())
            if (v.toObject().value(QLatin1String("type")).toString() == QLatin1String("text"))
                text += v.toObject().value(QLatin1String("text")).toString();
        text = text.trimmed();
        if (const auto m = typedCommand.match(text); m.hasMatch())
            text = (m.captured(1).trimmed() + QLatin1Char(' ') + typedArguments.match(text).captured(1).trimmed()).trimmed();
        else if (text.startsWith(QLatin1Char('<')) || text.startsWith(QLatin1String("Caveat:")))
            continue;
        if (!text.isEmpty()) points.prompts << qMakePair(text, before(parent));
    }
    return points;
}

} // namespace qucs_s::claude::history
