/*
 * claudegitbar.h - the Claude Code dock's bar of the git repository the
 *                  project (or the conversation's folder) is in
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_CLAUDEGITBAR_H
#define QUCS_CLAUDEGITBAR_H

#include "gitstatus.h"

#include <QDialog>
#include <QFrame>
#include <QPointer>

class QLabel;
class QListWidget;
class QMenu;
class QPlainTextEdit;
class QTimer;
class QToolButton;

/*!
 * Above the prompt, when the folder the dock is about is in a git
 * repository: the repository's folder, the branch, the lines changed
 * ("+2,096 −3", which open the changes), and Create PR - or, once the
 * branch has one (gh tells), PR #12, which opens it. Its menu: a draft
 * pull request, Commit Changes, Push Branch, Show Changes, Refresh. What
 * changes files or pushes is asked of Claude, a prompt in the
 * conversation (promptRequested), with the permissions it has. ✕ puts the
 * bar away until the repository or the branch changes.
 *
 * Looked at when the folder changes, after each turn, when Qucs-S comes to
 * the front and every few seconds while it is shown; git runs off the GUI
 * thread. ⋯ > Show Git Status turns the bar off (and on) in every
 * conversation, kept in the settings.
 */
class ClaudeGitBar : public QFrame
{
    Q_OBJECT

public:
    explicit ClaudeGitBar(QWidget* parent = nullptr);
    ~ClaudeGitBar() override;

    /// Whether the bars show the repository (⋯ > Show Git Status; on at
    /// first).
    static bool isOn();
    static void setOn(bool on);

    /// The folder the bar is about; looked at again when it changes.
    void setDirectory(const QString& dir);
    QString directory() const { return a_dir; }
    /// Looks at the repository again - now, or once the look under way
    /// ends; \a askGitHub: the branch's pull request too.
    void refresh(bool askGitHub = false);
    /// A turn is under way: nothing to ask of Claude now.
    void setBusy(bool busy);

    const qucs_s::git::Status& status() const { return a_status; }
    const qucs_s::git::PullRequest& pullRequest() const { return a_pr; }
    /// Whether git (or gh) is being asked.
    bool isLooking() const { return a_looking || a_askingGitHub; }
    /// Put away with ✕ (until the repository or the branch changes).
    bool isDismissed() const;

    /// What the buttons ask of Claude.
    QString createPrPrompt(bool draft) const;
    QString updatePrPrompt() const;
    QString commitPrompt() const;
    QString pushPrompt() const;

    // For the tests.
    QLabel* folderLabel() const { return a_folder; }
    QLabel* branchLabel() const { return a_branch; }
    QLabel* statLabel() const { return a_stat; }
    QToolButton* prButton() const { return a_prButton; }
    QToolButton* closeButton() const { return a_close; }
    QMenu* prMenu() const { return a_menu; }
    QDialog* changesDialog() const { return a_changes; }

public slots:
    /// The changes, file by file, with their diffs.
    void showChanges();

signals:
    /// A prompt for Claude (Create PR, Commit Changes...).
    void promptRequested(const QString& prompt);
    /// A changed file to open in Qucs-S.
    void openFileRequested(const QString& path);
    /// Looked at: what the bar shows is up to date.
    void updated();

protected:
    void changeEvent(QEvent* event) override;

private:
    void look();
    void askGitHub();
    void display(const qucs_s::git::Status& status);
    void updateBar();
    void updateVisibility();
    void fillMenu();
    void restyle();
    /// The palette of the theme it is drawn in.
    QPalette themePalette() const;
    void ask(const QString& prompt);
    QString dismissKey() const;
    QString where() const;

    QString a_dir;
    qucs_s::git::Status a_status;
    qucs_s::git::PullRequest a_pr;
    QString a_prFor;           // root and branch the pull request was asked for
    bool a_looking = false;
    bool a_again = false;      // looked at again once the look under way ends
    bool a_askAgain = false;   // and GitHub asked
    bool a_askingGitHub = false;
    int a_generation = 0;      // the folder's: an older look's result is dropped
    bool a_busy = false;
    QString a_dismissed;       // dismissKey() when ✕ was pressed

    QLabel* a_folder;
    QLabel* a_branch;
    QLabel* a_stat;
    QToolButton* a_prButton;
    QMenu* a_menu;
    QToolButton* a_close;
    QTimer* a_timer;
    QPointer<QDialog> a_changes;
};

/// The changes of a repository, file by file: the list, and the diff of
/// the file chosen (or of all), coloured.
class GitChangesDialog : public QDialog
{
    Q_OBJECT

public:
    GitChangesDialog(const qucs_s::git::Status& status, QWidget* parent = nullptr);

    void setStatus(const qucs_s::git::Status& status);
    QListWidget* files() const { return a_files; }
    QPlainTextEdit* diffView() const { return a_diff; }

signals:
    void openFileRequested(const QString& path);

private:
    void showDiff();

    qucs_s::git::Status a_status;
    QLabel* a_summary;
    QListWidget* a_files;
    QPlainTextEdit* a_diff;
};

#endif
