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

#include <QList>
#include <QWidget>

#include <functional>

class ClaudeCodePanel;
class QTabWidget;
class QToolButton;

/*!
 * The conversations of the Claude Code dock, a tab each: New (in a
 * conversation's header, or + by the tabs) opens one beside the others,
 * each with its own Claude Code session, so that one can think while
 * another waits. A tab says what its conversation is about (its first
 * prompt) and, by its mark, how it stands; closing the tab ends its
 * session (asked first when Claude is at work in it), and the last one
 * closed leaves a new one.
 *
 * A conversation that asks for permission comes forward when the dock is
 * hidden or it is the one in front; one behind others marks its tab and
 * the status bar, which leads to it.
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
    void refreshDocument();
    /// A note in the conversation whose files are being loaded again
    /// (while filesChanged is handled), else in the one in front.
    void addNote(const QString& text);

public slots:
    void focusComposer();

signals:
    void filesChanged(const QStringList& files);
    void openFileRequested(const QString& path);
    /// A session's state changed, or conversations came, went or changed
    /// places.
    void stateChanged();

protected:
    void changeEvent(QEvent* event) override;

private:
    ClaudeCodePanel* addPanel(ClaudeCodePanel* like);
    void updateTab(ClaudeCodePanel* panel);
    void permissionAsked(ClaudeCodePanel* panel);
    void restyle();

    QTabWidget* a_tabs;
    QToolButton* a_plus;
    QString a_defaultDir;
    std::function<QString()> a_document;
    ClaudeCodePanel* a_reporting = nullptr;   // whose files are loaded again
};

#endif // QUCS_CLAUDECODETABS_H
