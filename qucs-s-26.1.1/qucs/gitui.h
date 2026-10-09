/*
 * gitui.h - git in the windows: the commit dialog (what is staged and what
 *           is not, a file's changes, the message), the history, the blame,
 *           a diff; and what the File Browser's, the Git menu's and the
 *           status bar's git commands do
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_GITUI_H
#define QUCS_GITUI_H

#include "gitrepo.h"

#include <QDialog>
#include <QPointer>
#include <QSyntaxHighlighter>

#include <functional>

class QCheckBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;

namespace qucs_s::git {

/// The colour of a file's name in the File Browser and the lists: changed
/// (amber), staged (green), new (green), in conflict (red), ignored
/// (grey) - for a light or a dark theme. Invalid: as it is.
QColor colourOf(const Entry& entry, const QPalette& palette);
/// A folder with changes in it.
QColor changedFolderColour(const QPalette& palette);
/// A small badge with the entry's letter, in its colour.
QIcon badgeOf(const Entry& entry, const QPalette& palette);

/// Colours a unified diff: what is added, removed, the hunks, the headers.
class DiffHighlighter : public QSyntaxHighlighter
{
public:
    explicit DiffHighlighter(QTextDocument* document);
    void highlightBlock(const QString& text) override;
};

/// A diff, a commit, a stash: read in a window of its own.
class TextDialog : public QDialog
{
    Q_OBJECT

public:
    TextDialog(const QString& title, const QString& text, QWidget* parent = nullptr);
    QPlainTextEdit* view() const { return a_view; }

private:
    QPlainTextEdit* a_view;
};

/*!
 * Commit - as Eclipse's Staging view: the changes not staged and those
 * staged, each file's changes beside them, the message. Files go from one
 * list to the other (Stage, Unstage, their All, a double-click opens one);
 * a change is thrown away (Discard...); the commit amends the last one
 * when asked, and is pushed when asked.
 */
class CommitDialog : public QDialog
{
    Q_OBJECT

public:
    CommitDialog(const QString& root, QWidget* parent = nullptr);

    QString root() const { return a_root; }
    /// The lists read again.
    void reload();
    /// Commits: false when there was nothing to commit, no message, or it
    /// failed (said). \a push: then pushed.
    void commitNow(bool push);

    // For the tests.
    QListWidget* unstagedList() const { return a_unstaged; }
    QListWidget* stagedList() const { return a_staged; }
    QPlainTextEdit* messageEdit() const { return a_message; }
    QPlainTextEdit* diffView() const { return a_diff; }
    QCheckBox* amendBox() const { return a_amend; }
    QPushButton* commitButton() const { return a_commit; }
    QLabel* headerLabel() const { return a_header; }

public slots:
    void stageSelected();
    void unstageSelected();
    void stageAll();
    void unstageAll();
    void discardSelected();

signals:
    /// Committed (\a hash short); \a push asked.
    void committed(const QString& hash, bool push);
    void openRequested(const QString& path);

private:
    void showDiffOf(QListWidgetItem* item, bool staged);
    QStringList selectedPaths(QListWidget* list) const;
    void updateButtons();

    QString a_root;
    QLabel* a_header;
    QListWidget* a_unstaged;
    QListWidget* a_staged;
    QPlainTextEdit* a_diff;
    QPlainTextEdit* a_message;
    QCheckBox* a_amend;
    QPushButton* a_commit;
    QPushButton* a_commitPush;
    QPushButton* a_stage;
    QPushButton* a_unstage;
    QPushButton* a_discard;
    QPointer<Job> a_job;
};

/*!
 * The history of a repository, or of a file: its commits, newest first -
 * their refs, their author and date -, the commit chosen with its changes.
 * A commit's menu: copy its hash, check it out, a branch or a tag there,
 * revert or cherry-pick it, reset the branch to it.
 */
class HistoryDialog : public QDialog
{
    Q_OBJECT

public:
    HistoryDialog(const QString& root, const QString& path = {}, QWidget* parent = nullptr);
    QTreeWidget* commits() const { return a_commits; }
    QPlainTextEdit* details() const { return a_details; }
    /// The commit menu of row \a row.
    QMenu* menuFor(int row);
    void reload();

private:
    void loadMore();

    QString a_root;
    QString a_path;
    QTreeWidget* a_commits;
    QPlainTextEdit* a_details;
    QPushButton* a_more;
    int a_loaded = 0;
};

/// Who last changed each line of a file, and in which commit.
class BlameDialog : public QDialog
{
    Q_OBJECT

public:
    BlameDialog(const QString& root, const QString& path, QWidget* parent = nullptr);
    QTreeWidget* lines() const { return a_lines; }

private:
    QString a_root;
    QTreeWidget* a_lines;
};

/*!
 * What the git commands do, wherever they are chosen - the File Browser's
 * Git menu, the Git menu, the status bar's: on a repository and some of
 * its files, asking what needs to be asked (a name, whether to throw
 * changes away), saying what git said when it failed. What goes over the
 * network, and a commit, run without waiting, their progress shown.
 */
class Commands : public QObject
{
    Q_OBJECT

public:
    static Commands* instance();

    /// The Git menu of File Browser entries: \a paths.
    void fillEntryMenu(QMenu* menu, const QStringList& paths);
    /// The menu of a document's file (the status bar's): what it is in,
    /// what to do with it, and with its repository.
    void fillFileMenu(QMenu* menu, const QString& file);
    /// The branches to switch to (a remote's in a menu of their own).
    void fillBranchMenu(QMenu* menu, const QString& root);
    /// The stashes, each with its commands.
    void fillStashMenu(QMenu* menu, const QString& root);

    void commit(const QString& root);
    void stage(const QString& root, const QStringList& paths);
    void unstage(const QString& root, const QStringList& paths);
    /// Asks first.
    void discard(const QString& root, const QStringList& paths);
    void showDiff(const QString& root, const QString& path = {}, DiffOf of = DiffOf::Head);
    void showHistory(const QString& root, const QString& path = {});
    void showBlame(const QString& root, const QString& path);
    void ignore(const QString& root, const QString& path);
    void untrack(const QString& root, const QStringList& paths);

    void fetch(const QString& root);
    void pull(const QString& root);
    void push(const QString& root);
    void newBranch(const QString& root, const QString& start = {});
    void switchBranch(const QString& root, const QString& branch);
    void renameBranch(const QString& root);
    void deleteBranch(const QString& root);
    void mergeInto(const QString& root);
    void stashChanges(const QString& root);
    void applyStash(const QString& root, int index, bool pop);
    void dropStash(const QString& root, int index);
    void showStash(const QString& root, int index);
    void newTag(const QString& root, const QString& commit = {});
    void abortOperation(const QString& root);
    void editRemotes(const QString& root);
    void createRepository(const QString& folder);
    void cloneRepository(const QString& folder);

    /// A command that runs without being waited for (fetch, pull, push,
    /// clone, a commit): its progress shown, then \a then.
    void runJob(const QString& dir, const QStringList& args, const QString& title,
                const std::function<void(const Result&)>& then = {}, const QByteArray& input = {});
    /// The one running, if any (the tests wait for it).
    Job* runningJob() const { return a_job; }
    /// The windows it opened lately (the tests close them).
    QList<QPointer<QDialog>> openWindows() const { return a_windows; }

signals:
    /// A file to open in Qucs-S.
    void openRequested(const QString& path);
    /// A folder to show in the File Browser (a repository cloned).
    void showFolderRequested(const QString& path);
    /// What happened, for the status bar.
    void message(const QString& text);

private:
    explicit Commands(QObject* parent);
    QWidget* window() const;
    /// \a result's failure said (misc::reportError), titled \a what; true
    /// when it succeeded.
    bool report(const Result& result, const QString& what);
    void done(const QString& root, const QString& said);
    void keep(QDialog* dialog);

    QPointer<Job> a_job;
    QList<QPointer<QDialog>> a_windows;
};

} // namespace qucs_s::git

#endif // QUCS_GITUI_H
