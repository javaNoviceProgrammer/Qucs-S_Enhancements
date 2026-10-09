/*
 * gitstatus.h - the git repository a folder is in: its branch, what
 *               changed and by how many lines, the pull request of the
 *               branch (GitHub's gh)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_GITSTATUS_H
#define QUCS_GITSTATUS_H

#include <QList>
#include <QString>

class QProcessEnvironment;

namespace qucs_s::git {

/// A file changed since the base (Status::base): its lines added and
/// removed.
struct FileChange {
    QString path;          ///< from the repository's top folder, "/" between folders
    int added = 0;
    int removed = 0;
    bool binary = false;   ///< no lines to count
    bool untracked = false;   ///< new, not known to git yet
};

/*!
 * What the repository a folder is in stands at. The changes are counted
 * from where the branch left the default one - the work a pull request
 * would propose, committed or not: on another branch from its merge base
 * with the remote's default branch (origin/main), on the default branch
 * from its upstream (the commits not pushed), else from HEAD; untracked
 * files count as added, all their lines.
 */
struct Status {
    bool repository = false;   ///< the folder is in a git work tree
    QString root;              ///< the work tree's top folder
    QString branch;            ///< the branch checked out; empty when HEAD is detached
    QString head;              ///< HEAD's commit, short; empty before the first commit
    QString upstream;          ///< the branch's upstream ("origin/feature"), or empty
    int ahead = 0;             ///< commits the upstream does not have
    int behind = 0;            ///< and it has
    QString defaultBranch;     ///< "main": the remote's default, else main or master
    bool hasRemote = false;
    QString base;              ///< what the changes are counted from: "origin/main", "HEAD"
    QString baseCommit;        ///< and its commit (the empty tree before a first one)
    QList<FileChange> changes;
    bool dirty = false;        ///< changes not committed (new files too)

    int added() const;
    int removed() const;
    bool onDefaultBranch() const { return !branch.isEmpty() && branch == defaultBranch; }
    bool operator==(const Status& other) const;
    bool operator!=(const Status& other) const { return !(*this == other); }
};

/// The git program: QUCS_GIT in the environment when set, else git on the
/// PATH (or where Homebrew puts it). Empty when there is none - on macOS
/// too when /usr/bin/git would only ask to install the command line tools.
QString program();

/// The settings that keep git from running what a repository's own
/// configuration names - a file system monitor, its filters, its hooks -
/// while Qucs-S only looks at it (status, diff, log, blame): "key=value"
/// pairs, for -c. The user's own filters (git-lfs's) are kept.
QStringList readOnlyGuards(const QString& root);

/// \a env without what points git at one repository whatever the folder
/// (GIT_DIR, GIT_WORK_TREE, GIT_INDEX_FILE...: Qucs-S started from a git
/// hook): each path's own found.
void forgetRepositoryVariables(QProcessEnvironment& env);

/// Where \a dir stands (a Status whose repository is false outside a work
/// tree, or without git). Runs git: best not on the GUI thread.
Status status(const QString& dir);

/// The changes of \a status as a unified diff - of the file \a path (from
/// the top folder) alone when given; an untracked file as all added.
QString diff(const Status& status, const QString& path = QString());

/// "+2,096 −3".
QString statText(int added, int removed);

/// A pull request of a branch.
struct PullRequest {
    int number = 0;        ///< 0: none
    QString url;
    QString title;
    QString state;         ///< OPEN, CLOSED, MERGED
    bool draft = false;
};

/// GitHub's gh: QUCS_GH in the environment when set, else gh on the PATH
/// (or where Homebrew puts it); empty when there is none.
QString ghProgram();

/// The pull request of \a branch in the repository \a root (asks GitHub,
/// through gh); none without gh, a remote, or one.
PullRequest pullRequest(const QString& root, const QString& branch);

} // namespace qucs_s::git

#endif
