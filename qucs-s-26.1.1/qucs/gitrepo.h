/*
 * gitrepo.h - git for the File Browser, the Git menu, the status bar and
 *             Claude's git tools: a repository's files as git sees them,
 *             and what is done with it - staged, committed, branched,
 *             merged, fetched, pulled and pushed, stashed, tagged - its
 *             history and its blame
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_GITREPO_H
#define QUCS_GITREPO_H

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QString>
#include <QStringList>

class QFileSystemWatcher;
class QTimer;

namespace qucs_s::git {

/// What git said: its exit code (-1: it did not run, or it was stopped),
/// its output and its errors.
struct Result {
    int exitCode = -1;
    QString out;
    QString err;
    bool ok() const { return exitCode == 0; }
    /// What went wrong, for a message: git's error lines (its hints left
    /// out), else its exit code.
    QString error() const;
};

/// The environment git runs in: no prompt (a password asked where there is
/// no terminal would wait for ever), no editor (its own messages taken as
/// they are), no colours.
QProcessEnvironment environment();

/// Runs git in \a dir with \a args, \a input on its standard input. \a
/// readOnly: it only looks - nothing the repository's own configuration
/// names is run (readOnlyGuards()), and it takes no lock it can do
/// without. Waits up to \a timeoutMs (then stopped: exit code -1).
Result run(const QString& dir, const QStringList& args, bool readOnly = false, int timeoutMs = 60000,
           const QByteArray& input = {});

/// The top folder of the work tree \a path is in (a file's: its folder's),
/// or empty. Looks for .git upwards: runs nothing.
QString topLevel(const QString& path);
/// \a path from \a root, "/" between folders - empty for the root itself
/// and for a path out of it (\a inside false then).
QString relativePath(const QString& root, const QString& path, bool* inside = nullptr);

/// A file as git sees it (git status): what is staged of it (the index
/// against HEAD) and what is not (the work tree against the index), in
/// git's letters - M modified, A added, D deleted, R renamed, C copied, T
/// its type changed, '.' nothing.
struct Entry {
    QString path;          ///< from the top folder; a folder untracked or ignored whole ends in "/"
    QString from;          ///< a rename's or a copy's former name
    QChar staged = QLatin1Char('.');
    QChar unstaged = QLatin1Char('.');
    bool untracked = false;
    bool ignored = false;
    bool conflicted = false;

    bool hasStaged() const { return !conflicted && staged != QLatin1Char('.'); }
    bool hasUnstaged() const { return untracked || conflicted || unstaged != QLatin1Char('.'); }
    bool isFolder() const { return path.endsWith(QLatin1Char('/')); }
    /// One letter, as an editor shows it: M, A, D, R, U (untracked), ! (in
    /// conflict), I (ignored).
    QString letter() const;
    /// "Modified, staged", "Untracked", "In conflict"...
    QString describe() const;
};

/// What a repository stands at.
struct Repository {
    bool valid = false;
    QString root;          ///< the work tree's top folder
    QString gitDir;        ///< its .git folder
    QString branch;        ///< checked out; empty when HEAD is detached
    QString head;          ///< HEAD's commit, short; empty before the first commit
    QString upstream;      ///< "origin/main", or empty
    int ahead = 0;         ///< commits the upstream has not
    int behind = 0;        ///< and it has
    QString operation;     ///< a merge, rebase, cherry-pick, revert or bisect under way ("merge"...), or empty
    QList<Entry> entries;  ///< changed, untracked and ignored
    QDateTime read;        ///< when it was read

    /// The entry of \a path (from the top folder): its own, or that of the
    /// untracked or ignored folder it is in; null when it is as committed.
    const Entry* entryOf(const QString& path) const;
    /// Whether anything in the folder \a path (from the top folder; the
    /// root: "") changed - untracked files too, ignored ones not.
    bool changedIn(const QString& path) const;
    /// The changed files (untracked too), those staged, those in conflict.
    int changedCount() const;
    int stagedCount() const;
    int conflictCount() const;
    /// "main ↑1 ↓2", "detached at 1a2b3c4", "main (no commit yet)".
    QString branchText() const;
    /// Builds the lookups (read() does).
    void index();

private:
    QHash<QString, int> a_byPath;
    QSet<QString> a_changedFolders;
};

/// The repository \a path is in, as it stands (valid false outside one,
/// or without git). It only looks; runs git: best not on the GUI thread.
Repository read(const QString& path);

// ----------------------------------------------------------------------
// What is done with it. Paths: absolute, or from the top folder - paths
// given, none of them in the repository, are refused. A ref, a name or a
// URL that begins with '-' is refused (git would take it for an option).

/// Stages \a paths (all changes, new and deleted files too; everything
/// when none).
Result stage(const QString& root, const QStringList& paths);
/// Takes \a paths out of what is staged (everything when none).
Result unstage(const QString& root, const QStringList& paths);
/// Throws their changes away: a file git knows is put back as HEAD has
/// it; one it does not (untracked, or only added) goes to the trash - \a
/// trashed says where. Everything when \a paths is empty.
Result discard(const QString& root, const QStringList& paths, QStringList* trashed = nullptr);
/// Commits what is staged with \a message - \a only those paths, staged
/// first, when given; \a amend: in place of the last commit.
Result commit(const QString& root, const QString& message, bool amend = false, const QStringList& only = {});
/// The last commit's message (for an amend).
QString lastMessage(const QString& root);
/// Git stops following \a paths (git rm --cached): the files stay.
Result untrack(const QString& root, const QStringList& paths);
/// \a path in the top folder's .gitignore ("/build/", "/run.dat"): added,
/// or there already. False and why when it cannot be.
bool ignore(const QString& root, const QString& path, QString* why = nullptr);
/// A new repository in \a folder.
Result init(const QString& folder);

/// A branch, local or a remote's.
struct Branch {
    QString name;          ///< "main", "origin/main"
    QString commit;        ///< short
    QString upstream;      ///< a local one's
    int ahead = 0;
    int behind = 0;
    bool current = false;
    bool remote = false;
    QString subject;       ///< its last commit's
    QDateTime date;
};
/// The local branches, then the remotes' (no origin/HEAD).
QList<Branch> branches(const QString& root);
/// The tags, newest first.
QStringList tags(const QString& root);
/// Checks \a branch out: a remote one ("origin/feature") as a local branch
/// that follows it (made, or the one there already).
Result switchTo(const QString& root, const QString& branch);
Result createBranch(const QString& root, const QString& name, const QString& start = {}, bool checkOut = true);
Result deleteBranch(const QString& root, const QString& name, bool force = false);
Result renameBranch(const QString& root, const QString& from, const QString& to);
/// Merges \a ref into the branch checked out.
Result merge(const QString& root, const QString& ref);
/// Gives up the merge, rebase, cherry-pick or revert under way.
Result abort(const QString& root, const QString& operation);
Result createTag(const QString& root, const QString& name, const QString& message = {}, const QString& commit = {});
Result deleteTag(const QString& root, const QString& name);

struct Remote {
    QString name;
    QString fetchUrl;
    QString pushUrl;
};
QList<Remote> remotes(const QString& root);
Result addRemote(const QString& root, const QString& name, const QString& url);
Result removeRemote(const QString& root, const QString& name);

/// What goes over the network - long: a Job runs it. Fetch from \a remote
/// (every one when empty; none - empty - for a name that begins with '-'),
/// pruning; pull the branch's upstream; push the
/// branch - to its upstream, else to the only remote or origin, made its
/// upstream (empty and why when there is no remote, or no branch); clone.
QStringList fetchArgs(const QString& remote = {});
QStringList pullArgs();
QStringList pushArgs(const Repository& repo, QString* why = nullptr, bool tags = false);
QStringList cloneArgs(const QString& url, const QString& folder);

struct Stash {
    int index = 0;
    QString message;
    QDateTime date;
};
QList<Stash> stashes(const QString& root);
/// The changes put aside (untracked files too with \a untracked).
Result stash(const QString& root, const QString& message = {}, bool untracked = true);
/// Stash \a index's changes back (\a pop: and it dropped).
Result applyStash(const QString& root, int index, bool pop);
Result dropStash(const QString& root, int index);
/// Stash \a index as a diff.
QString showStash(const QString& root, int index);

// ----------------------------------------------------------------------
// Its history.

struct Commit {
    QString hash;
    QString shortHash;
    QString author;
    QString email;
    QDateTime date;
    QString subject;
    QStringList refs;      ///< "HEAD -> main", "origin/main", "tag: v1.0"
    QStringList parents;
};
/// The commits of \a ref (HEAD when empty) - those that changed \a path
/// when given (a file followed through renames) -, newest first.
QList<Commit> log(const QString& root, const QString& path = {}, int max = 200, const QString& ref = {}, int skip = 0);
/// A commit: what git show says - its message, its files, its diff (of
/// \a path alone when given).
QString show(const QString& root, const QString& commit, const QString& path = {});

enum class DiffOf {
    Unstaged,   ///< the work tree against the index
    Staged,     ///< the index against HEAD
    Head        ///< the work tree against HEAD: all of it
};
/// The changes of \a path (every file when empty) as a unified diff; an
/// untracked file's as all added.
QString diff(const QString& root, const QString& path = {}, DiffOf of = DiffOf::Head);

struct BlameLine {
    int line = 0;
    QString hash;
    QString shortHash;
    QString author;
    QDateTime date;
    QString summary;
    QString text;
    bool uncommitted = false;
};
QList<BlameLine> blame(const QString& root, const QString& path);

/// Checks \a commit out alone (no branch: HEAD detached).
Result checkOutCommit(const QString& root, const QString& commit);
/// Makes a commit that undoes \a commit.
Result revert(const QString& root, const QString& commit);
Result cherryPick(const QString& root, const QString& commit);
/// Moves the branch to \a commit: "soft" (the changes kept staged),
/// "mixed" (kept, not staged) or "hard" (thrown away).
Result reset(const QString& root, const QString& commit, const QString& mode);

// ----------------------------------------------------------------------

/*!
 * The repositories the windows look at, read again when they change: a
 * command of git's (HEAD, the index), a file saved, every few seconds
 * while one was looked at lately. Read on the thread pool; changed() says
 * when one was read anew.
 */
class Tracker : public QObject
{
    Q_OBJECT

public:
    static Tracker* instance();

    /// What the repository \a path is in stands at, as last read; null
    /// when \a path is in none, or it has not been read yet (it is read,
    /// and changed() comes).
    const Repository* repositoryOf(const QString& path);
    /// The entry of \a path (null: as committed, or in no repository).
    const Entry* entryOf(const QString& path);
    /// Reads it again soon: the repository \a path is in (every one known
    /// when empty).
    void refresh(const QString& path = {});
    /// Reads it again now, and waits (after a command, in the tests).
    const Repository* readNow(const QString& path);
    /// As last read when that was less than \a maxAgeMs ago (a menu that
    /// opens: no git run while it waits), else read now.
    const Repository* fresh(const QString& path, int maxAgeMs = 3000);
    /// Whether one is being read.
    bool isReading() const { return !a_reading.isEmpty(); }

signals:
    /// The repository of \a root was read anew and stands otherwise than
    /// it did (it may be none any more).
    void changed(const QString& root);

private:
    explicit Tracker(QObject* parent);
    QString rootOf(const QString& path);
    void readSoon(const QString& root);
    void readNext();
    void store(const QString& root, const Repository& repo);
    void watch(const Repository& repo);

    QHash<QString, Repository> a_repos;
    QHash<QString, QString> a_roots;    // a path's repository (empty: none), as last looked for
    QHash<QString, qint64> a_used;      // when each was last asked for (ms)
    QHash<QString, qint64> a_cost;      // how long each took to read (ms)
    QHash<QString, qint64> a_polled;    // when each was last read for the clock (ms)
    QSet<QString> a_reading;
    QSet<QString> a_waiting;            // to be read (again)
    QFileSystemWatcher* a_watcher;
    QTimer* a_soon;
    QTimer* a_every;
};

/*!
 * A git command run without waiting for it - what goes over the network,
 * a commit (its hooks take their time): its progress as git writes it, its
 * end. Stopped by cancel().
 */
class Job : public QObject
{
    Q_OBJECT

public:
    Job(const QString& dir, const QStringList& args, QObject* parent = nullptr, const QByteArray& input = {});
    ~Job() override;
    void start();
    void cancel();
    bool isRunning() const;
    const Result& result() const { return a_result; }
    QStringList arguments() const { return a_args; }

signals:
    /// The line git is at ("Receiving objects:  45% (450/1000)").
    void progress(const QString& line);
    void finished(const qucs_s::git::Result& result);

private:
    QString a_dir;
    QStringList a_args;
    QByteArray a_input;
    QPointer<QProcess> a_process;
    Result a_result;
    QByteArray a_err;
    bool a_cancelled = false;
};

} // namespace qucs_s::git

#endif // QUCS_GITREPO_H
