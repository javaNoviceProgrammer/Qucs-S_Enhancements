/*
 * claudecodetabs.h - the Claude Code dock's conversations, a tab each
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_CLAUDECODETABS_H
#define QUCS_CLAUDECODETABS_H

#include "claudehistory.h"

#include <QList>
#include <QPointer>
#include <QSet>
#include <QWidget>

#include <functional>

class ClaudeCodePanel;
namespace qucs_s::claude {
class ToolHost;
}
class QDialog;
class QLineEdit;
class QMenu;
class QTimer;
class QTabWidget;
class QToolButton;

/*!
 * The conversations of the Claude Code dock, a tab each: New (in a
 * conversation's header, or + by the tabs) opens one beside the others,
 * each with its own Claude Code session, so that one can think while
 * another waits. A tab says what its conversation is about (its first
 * prompt, or the name it was given: Rename in the tab's menu, or a double
 * click on it) and, by its mark, how it stands; closing the tab ends its
 * session (asked first when Claude is at work in it), and the last one
 * closed leaves a new one.
 *
 * A conversation that asks for permission comes forward when the dock is
 * hidden or it is the one in front; one behind others marks its tab and
 * the status bar, which leads to it.
 *
 * The conversations are kept as they go (claudehistory.h): those open when
 * Qucs-S closes come back when it opens again, each going on where it was,
 * and any kept one - or one of Claude Code's own sessions of the folder -
 * is gone on with from /resume.
 */
class ClaudeCodeTabs : public QWidget
{
    Q_OBJECT

public:
    explicit ClaudeCodeTabs(QWidget* parent = nullptr);
    ~ClaudeCodeTabs() override;

    /// The conversation in front.
    ClaudeCodePanel* current() const;
    QList<ClaudeCodePanel*> panels() const;
    int count() const;
    QTabWidget* tabWidget() const { return a_tabs; }
    QToolButton* newTabButton() const { return a_plus; }

    /// A conversation in a tab of its own, in front; in the folder of the
    /// one that was in front.
    ClaudeCodePanel* newConversation();
    /// Closes \a panel's tab and ends its session (with \a ask, asked
    /// first when Claude is at work in it). False: it stays.
    bool closeConversation(ClaudeCodePanel* panel, bool ask = true);
    void showConversation(ClaudeCodePanel* panel);
    /// Names \a panel's conversation where its tab is: an editor over the
    /// tab, Enter (or a click elsewhere) to keep the name, Esc to leave it
    /// as it was; emptied, the first prompt names it again.
    void renameConversation(ClaudeCodePanel* panel);
    /// The editor of a rename under way, or null.
    QLineEdit* renameEditor() const;
    /// The menu of the tab at \a index, as a right click on it opens it:
    /// Rename, Reset Name, Close. The caller's to delete.
    QMenu* tabMenu(int index);
    /// The conversations open when Qucs-S last closed, brought back (when
    /// ⋯ > Reopen Conversations at Start is on): each as it was, going on
    /// where it was. False when there were none.
    bool restoreConversations();
    /// Keeps every conversation that changed, and which are open, now.
    void saveConversations();
    /// The conversations to go on with: those the dock kept, then Claude
    /// Code's own sessions of \a folder not among them.
    QList<qucs_s::claude::history::Summary> resumable(const QString& folder) const;
    /// Goes on with \a conversation - in front if it is open already, else
    /// in the tab in front when that is empty, else in a new one. Null
    /// when it cannot be read.
    ClaudeCodePanel* resume(const qucs_s::claude::history::Summary& conversation);
    /// /resume: \a query naming one (its session's id, or its kept id)
    /// goes on with it at once; else the list of them to choose from,
    /// \a query in its filter.
    void showResume(ClaudeCodePanel* from, const QString& query);
    /// The list /resume shows, for \a folder, \a query in its filter (not
    /// shown: showResume() runs it).
    QDialog* resumeDialog(const QString& folder, const QString& query);

    /// A conversation waiting for the user's permission (the one in
    /// front, if it is), or null.
    ClaudeCodePanel* needingAttention() const;
    /// What the status bar tells of: one waiting for permission, else the
    /// one in front at work, else one at work, else the one in front.
    ClaudeCodePanel* mostUrgent() const;

    /// As ClaudeCodePanel's, for every conversation.
    void setDefaultDirectory(const QString& dir);
    QString defaultDirectory() const { return a_defaultDir; }
    void setDocumentProvider(std::function<QString()> provider);
    void setSchematicsProvider(std::function<QStringList()> provider);
    /// A document was saved under another name (Save As): a conversation
    /// pinned to \a from is pinned to \a to.
    void documentRenamed(const QString& from, const QString& to);
    /// The application's tools, offered in every conversation (not owned).
    void setToolHost(qucs_s::claude::ToolHost* host);
    void refreshDocument();
    /// A note in the conversation whose files are being loaded again
    /// (while filesChanged is handled), else in the one in front.
    void addNote(const QString& text);

public slots:
    void focusComposer();

signals:
    void filesChanged(const QStringList& files);
    void openFileRequested(const QString& path);
    /// A session's state changed, or conversations came, went, changed
    /// places or were named.
    void stateChanged();

protected:
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    ClaudeCodePanel* addPanel(ClaudeCodePanel* like);
    /// Ends the rename under way: the name kept (\a keep) or not, and the
    /// composer given the keyboard back (\a refocus) when the editor had it.
    void finishRenaming(bool keep, bool refocus);
    /// Where the rename's editor goes: over its tab's text, the tab bar's
    /// width at most.
    QRect renameRect() const;
    void placeRenameEditor();
    void updateTab(ClaudeCodePanel* panel);
    void permissionAsked(ClaudeCodePanel* panel);
    /// Keeps \a panel's conversation (a new id for it when it has none).
    void saveConversation(ClaudeCodePanel* panel);
    /// Which conversations are open, and the one in front.
    void saveOpen();
    void restyle();

    QTabWidget* a_tabs;
    QToolButton* a_plus;
    QString a_defaultDir;
    std::function<QString()> a_document;
    std::function<QStringList()> a_schematics;
    qucs_s::claude::ToolHost* a_host = nullptr;
    ClaudeCodePanel* a_reporting = nullptr;   // whose files are loaded again
    QSet<ClaudeCodePanel*> a_unsaved;         // changed since kept
    QTimer* a_saveTimer = nullptr;            // keeps them a moment later
    QPointer<QLineEdit> a_renameEditor;       // a rename under way
    QPointer<ClaudeCodePanel> a_renaming;     // of this one
};

#endif // QUCS_CLAUDECODETABS_H
