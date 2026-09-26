/*
 * claudehistory.h - the Claude Code dock's conversations kept on disk, and
 *                   Claude Code's own sessions to continue
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_CLAUDEHISTORY_H
#define QUCS_CLAUDEHISTORY_H

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

/*!
 * The conversations of the Claude Code dock, kept as they go - a file each
 * (what was said, its title and name, its folder, the schematic it was
 * pinned to, Claude Code's session to continue) and an index of them and
 * of those open when Qucs-S was closed - so that they come back when it
 * opens again, and any of them can be continued (/resume). Claude Code
 * keeps its own sessions too (~/.claude/projects): those of a folder are
 * listed to continue as well.
 */
namespace qucs_s::claude::history {

/// A conversation to continue, as the list of them shows it.
struct Summary {
    QString id;              ///< Qucs-S's (a file here), or Claude Code's session
    QString title;           ///< its name, else its first prompt
    QString sessionId;       ///< Claude Code's, to continue it
    QString folder;          ///< where Claude worked
    QDateTime updated;
    QString claudeFile;      ///< one of Claude Code's own: its session file
};

/// Where they are kept: QUCS_CLAUDE_HISTORY when set (the tests), else
/// "claude-conversations" in the application's data folder.
QString directory();

/// Those kept, the latest first.
QList<Summary> saved();
/// One kept (as it was saved), or an empty object.
QJsonObject load(const QString& id);
/// Keeps \a conversation as \a id: its "title", "sessionId", "folder" go
/// to the index too. Only the latest \a keep are kept (not those open).
bool save(const QString& id, const QJsonObject& conversation, int keep = 100);
void remove(const QString& id);

/// Whether those open when Qucs-S closed come back when it opens again
/// (on at first).
bool reopenAtStart();
void setReopenAtStart(bool on);

/// Those open when last said (in their order), and the one in front.
QStringList open(QString* current = nullptr);
void setOpen(const QStringList& ids, const QString& current);

/// Claude Code's folder: CLAUDE_CONFIG_DIR, else ~/.claude.
QString claudeDirectory();
/// Claude Code's sessions of \a folder, the latest first (at most
/// \a limit): their titles (/rename) or first prompts.
QList<Summary> claudeSessions(const QString& folder, int limit = 50);
/// The file of Claude Code's session \a sessionId, whatever its folder,
/// or empty.
QString claudeSessionFile(const QString& sessionId);
/// The first prompt (or title) and folder of a session file.
Summary claudeSessionSummary(const QString& file);

} // namespace qucs_s::claude::history

#endif // QUCS_CLAUDEHISTORY_H
