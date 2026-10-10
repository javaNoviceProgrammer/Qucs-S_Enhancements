/*
 * gitrepo.cpp - git for the File Browser, the Git menu, the status bar and
 *               Claude's git tools
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "gitrepo.h"

#include "gitstatus.h"
#include "misc.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QMutex>
#include <QMutexLocker>
#include <QPromise>
#include <QRegularExpression>
#include <QThreadPool>
#include <QTimer>

#include <algorithm>
#include <memory>

namespace qucs_s::git {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("Git", text);
}

// The read-only guards of a repository, for a while: they take a git of
// their own to find.
QStringList guardsOf(const QString& root)
{
    struct Kept {
        QStringList guards;
        qint64 at = 0;
    };
    static QMutex mutex;
    static QHash<QString, Kept> kept;
    static QElapsedTimer clock;
    QMutexLocker lock(&mutex);
    if (!clock.isValid()) clock.start();
    auto it = kept.find(root);
    if (it != kept.end() && clock.elapsed() - it->at < 30000) return it->guards;
    lock.unlock();
    const QStringList guards = readOnlyGuards(root);
    lock.relock();
    kept.insert(root, {guards, clock.elapsed()});
    return guards;
}

// Lines of git's output.
QStringList linesOf(const QString& text)
{
    return text.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts);
}

// \a paths from the top folder, for after "--".
QStringList relative(const QString& root, const QStringList& paths)
{
    QStringList out;
    for (const QString& p : paths) {
        bool inside = false;
        QString rel = QFileInfo(p).isAbsolute() ? relativePath(root, p, &inside) : QDir::cleanPath(p);
        if (QFileInfo(p).isAbsolute() && !inside) continue;
        if (rel.isEmpty() || rel == QLatin1String(".")) rel = QStringLiteral(".");
        if (rel.endsWith(QLatin1Char('/'))) rel.chop(1);
        out << rel;
    }
    return out;
}

// A ref, a remote, a URL or a name given - by a dialog, by Claude: one
// that begins with '-' git would take for an option ("--upload-pack=...",
// "--output=..."), and is refused.
bool optionLike(const QString& s)
{
    return s.startsWith(QLatin1Char('-'));
}

Result failure(const QString& why)
{
    Result r;
    r.exitCode = 1;
    r.err = why;
    return r;
}

Result optionRefused(const QString& s)
{
    return failure(tr("fatal: %1 is not a name git takes here (it begins with '-')").arg(s));
}

// Paths given, none of them in the repository: refused (none would be
// everything).
Result outside()
{
    return failure(tr("fatal: the paths given are not in the repository"));
}

bool hasHead(const QString& root)
{
    return run(root, {QStringLiteral("rev-parse"), QStringLiteral("--verify"), QStringLiteral("-q"), QStringLiteral("HEAD")}, true, 15000).ok();
}

QStringList withPaths(QStringList args, const QStringList& paths)
{
    args << QStringLiteral("--") << paths;
    return args;
}

// \a work run on the thread pool; its result in the future.
template <typename T, typename F>
QFuture<T> runAside(F work)
{
    auto promise = std::make_shared<QPromise<T>>();
    QFuture<T> future = promise->future();
    promise->start();
    QThreadPool::globalInstance()->start([promise, work] {
        promise->addResult(work());
        promise->finish();
    });
    return future;
}

qint64 nowMs()
{
    static QElapsedTimer clock;
    if (!clock.isValid()) clock.start();
    return clock.elapsed();
}

} // namespace

// ----------------------------------------------------------------------

QString Result::error() const
{
    // Paths git would not stage, being ignored: which, not the last line.
    if (const qsizetype at = err.indexOf(QLatin1String("ignored by one of your .gitignore files")); at >= 0) {
        QStringList names;
        for (const QString& l : linesOf(err.mid(at)).mid(1)) {
            if (l.startsWith(QLatin1String("hint:"))) break;
            names << l.trimmed();
        }
        return tr("in .gitignore, so not staged: %1 (an ignored file is staged only once git follows it)")
            .arg(names.join(QStringLiteral(", ")));
    }
    QStringList said;
    for (const QString& l : linesOf(err + QLatin1Char('\n') + out)) {
        const QString t = l.trimmed();
        if (t.startsWith(QLatin1String("fatal:")) || t.startsWith(QLatin1String("error:")) || t.startsWith(QLatin1String("CONFLICT"))
            || t.startsWith(QLatin1String("Automatic merge failed")) || t.startsWith(QLatin1String("! [")))
            said << t;
    }
    if (said.isEmpty()) {
        for (const QString& text : {err, out}) {
            QStringList lines;
            for (const QString& l : linesOf(text))
                if (!l.trimmed().startsWith(QLatin1String("hint:"))) lines << l.trimmed();
            if (!lines.isEmpty()) {
                said << lines.last();
                break;
            }
        }
    }
    if (said.isEmpty())
        return exitCode < 0 ? tr("git did not run to its end") : tr("git ended with exit code %1").arg(exitCode);
    return said.join(QLatin1Char('\n'));
}

QProcessEnvironment environment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    env.insert(QStringLiteral("GCM_INTERACTIVE"), QStringLiteral("never"));
    env.insert(QStringLiteral("GIT_EDITOR"), QStringLiteral(":"));
    env.insert(QStringLiteral("GIT_SEQUENCE_EDITOR"), QStringLiteral(":"));
    env.insert(QStringLiteral("GIT_MERGE_AUTOEDIT"), QStringLiteral("no"));
    env.insert(QStringLiteral("NO_COLOR"), QStringLiteral("1"));
    forgetRepositoryVariables(env);   // (Qucs-S started from a git hook)
    return env;
}

namespace {

// What git wrote, as bytes (a file's: not read as text).
struct Raw {
    int exitCode = -1;
    QByteArray out;
    QString err;
};

Raw runRaw(const QString& dir, const QStringList& args, bool readOnly, int timeoutMs, const QByteArray& input = {});

} // namespace

Result run(const QString& dir, const QStringList& args, bool readOnly, int timeoutMs, const QByteArray& input)
{
    const Raw raw = runRaw(dir, args, readOnly, timeoutMs, input);
    Result r;
    r.exitCode = raw.exitCode;
    r.out = QString::fromUtf8(raw.out);
    r.err = raw.err;
    return r;
}

namespace {

Raw runRaw(const QString& dir, const QStringList& args, bool readOnly, int timeoutMs, const QByteArray& input)
{
    Raw r;
    const QString git = program();
    if (git.isEmpty()) {
        r.err = tr("git is not installed");
        return r;
    }
    QProcess p;
    QProcessEnvironment env = environment();
    QStringList all;
    if (readOnly) {
        env.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));
        const QString root = topLevel(dir);
        for (const QString& c : guardsOf(root.isEmpty() ? dir : root)) all << QStringLiteral("-c") << c;
    }
    all << args;
    p.setProcessEnvironment(env);
    p.setWorkingDirectory(dir);
    p.start(git, all);
    if (!p.waitForStarted(5000)) {
        r.err = tr("git could not be started");
        return r;
    }
    if (!input.isEmpty()) p.write(input);
    p.closeWriteChannel();
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(2000);
        r.err = tr("git took too long, and was stopped");
        return r;
    }
    r.exitCode = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    r.out = p.readAllStandardOutput();
    r.err = QString::fromUtf8(p.readAllStandardError());
    return r;
}

} // namespace

QString topLevel(const QString& path)
{
    if (path.isEmpty()) return {};
    const QFileInfo info(path);
    QString dir = QDir::cleanPath(info.isDir() ? info.absoluteFilePath() : info.absolutePath());
    QStringList ceilings;
    for (const QString& c : qEnvironmentVariable("GIT_CEILING_DIRECTORIES").split(QDir::listSeparator(), Qt::SkipEmptyParts))
        ceilings << QDir::cleanPath(c);
    for (int depth = 0; depth < 256 && !dir.isEmpty(); ++depth) {
        if (QFileInfo::exists(dir + QStringLiteral("/.git"))) return dir;
        const QString parent = QFileInfo(dir).absolutePath();
        if (parent == dir || ceilings.contains(parent)) break;
        dir = QDir::cleanPath(parent);
    }
    return {};
}

QString relativePath(const QString& root, const QString& path, bool* inside)
{
    const auto done = [inside](bool in, const QString& rel) {
        if (inside != nullptr) *inside = in;
        return rel;
    };
    const QString r = QDir::cleanPath(root), p = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (p == r) return done(true, {});
    if (p.startsWith(r + QLatin1Char('/'))) return done(true, p.mid(r.size() + 1));
    // Spelt otherwise (macOS' /var for /private/var, a link): by the files.
    const QString cr = QFileInfo(r).canonicalFilePath();
    QString cp = QFileInfo(p).canonicalFilePath();
    if (cp.isEmpty()) {   // (not there: a file deleted)
        const QString folder = QFileInfo(QFileInfo(p).absolutePath()).canonicalFilePath();
        if (!folder.isEmpty()) cp = folder + QLatin1Char('/') + QFileInfo(p).fileName();
    }
    if (cr.isEmpty() || cp.isEmpty()) return done(false, {});
    if (cp == cr) return done(true, {});
    if (cp.startsWith(cr + QLatin1Char('/'))) return done(true, cp.mid(cr.size() + 1));
    return done(false, {});
}

// ----------------------------------------------------------------------
// Entries and repositories

QString Entry::letter() const
{
    if (conflicted) return QStringLiteral("!");
    if (ignored) return QStringLiteral("I");
    if (untracked) return QStringLiteral("U");
    const QChar c = unstaged != QLatin1Char('.') ? unstaged : staged;
    if (c == QLatin1Char('T')) return QStringLiteral("M");
    return QString(c);
}

QString Entry::describe() const
{
    if (conflicted) return tr("In conflict");
    if (ignored) return tr("Ignored");
    if (untracked) return isFolder() ? tr("Untracked folder") : tr("Untracked");
    const auto what = [](QChar c) {
        switch (c.unicode()) {
        case 'M': return tr("Modified");
        case 'T': return tr("Type changed");
        case 'A': return tr("Added");
        case 'D': return tr("Deleted");
        case 'R': return tr("Renamed");
        case 'C': return tr("Copied");
        default: return QString();
        }
    };
    if (hasStaged() && unstaged != QLatin1Char('.'))
        return tr("%1, staged, and %2 since").arg(what(staged), what(unstaged).toLower());
    if (hasStaged()) return tr("%1, staged").arg(what(staged));
    return tr("%1, not staged").arg(what(unstaged));
}

void Repository::index()
{
    a_byPath.clear();
    a_changedFolders.clear();
    for (int i = 0; i < entries.size(); ++i) {
        const Entry& e = entries.at(i);
        a_byPath.insert(e.path, i);
        if (e.ignored) continue;
        QString folder = e.isFolder() ? e.path.chopped(1) : e.path.section(QLatin1Char('/'), 0, -2);
        if (!e.isFolder() && !e.path.contains(QLatin1Char('/'))) folder.clear();
        for (;;) {
            if (a_changedFolders.contains(folder)) break;
            a_changedFolders.insert(folder);
            if (folder.isEmpty()) break;
            folder = folder.contains(QLatin1Char('/')) ? folder.section(QLatin1Char('/'), 0, -2) : QString();
        }
    }
}

const Entry* Repository::entryOf(const QString& path) const
{
    QString p = path;
    if (p.endsWith(QLatin1Char('/'))) p.chop(1);
    if (auto it = a_byPath.find(p); it != a_byPath.end()) return &entries.at(*it);
    if (auto it = a_byPath.find(p + QLatin1Char('/')); it != a_byPath.end()) return &entries.at(*it);
    // In an untracked or ignored folder.
    for (QString folder = p.section(QLatin1Char('/'), 0, -2); p.contains(QLatin1Char('/')) && !folder.isEmpty();
         folder = folder.contains(QLatin1Char('/')) ? folder.section(QLatin1Char('/'), 0, -2) : QString()) {
        if (auto it = a_byPath.find(folder + QLatin1Char('/')); it != a_byPath.end()) return &entries.at(*it);
        if (!folder.contains(QLatin1Char('/'))) break;
    }
    return nullptr;
}

bool Repository::changedIn(const QString& path) const
{
    QString p = path;
    if (p.endsWith(QLatin1Char('/'))) p.chop(1);
    return a_changedFolders.contains(p);
}

int Repository::changedCount() const
{
    return int(std::count_if(entries.cbegin(), entries.cend(), [](const Entry& e) { return !e.ignored; }));
}

int Repository::stagedCount() const
{
    return int(std::count_if(entries.cbegin(), entries.cend(), [](const Entry& e) { return e.hasStaged(); }));
}

int Repository::conflictCount() const
{
    return int(std::count_if(entries.cbegin(), entries.cend(), [](const Entry& e) { return e.conflicted; }));
}

QString Repository::branchText() const
{
    QString text = !branch.isEmpty() ? branch : head.isEmpty() ? tr("no branch") : tr("detached at %1").arg(head);
    if (head.isEmpty() && !branch.isEmpty()) text += QLatin1Char(' ') + tr("(no commit yet)");
    if (ahead > 0) text += QStringLiteral(" ↑%1").arg(ahead);
    if (behind > 0) text += QStringLiteral(" ↓%1").arg(behind);
    return text;
}

Repository read(const QString& path)
{
    Repository r;
    const QString top = topLevel(path);
    if (top.isEmpty() || program().isEmpty()) return r;
    const Result where = run(top, {QStringLiteral("rev-parse"), QStringLiteral("--absolute-git-dir")}, true, 15000);
    if (!where.ok()) return r;
    r.root = top;
    r.gitDir = QDir::cleanPath(QDir::fromNativeSeparators(where.out.trimmed()));
    QStringList args{QStringLiteral("status"), QStringLiteral("--porcelain=v2"), QStringLiteral("-z"), QStringLiteral("--branch"),
                     QStringLiteral("--untracked-files=normal")};
    Result st = run(top, args + QStringList{QStringLiteral("--ignored=matching")}, true, 30000);
    if (!st.ok()) st = run(top, args, true, 30000);   // (a git before --ignored=matching)
    if (!st.ok()) return r;
    r.valid = true;
    r.read = QDateTime::currentDateTime();
    const QStringList tokens = st.out.split(QLatin1Char('\0'));
    for (qsizetype i = 0; i < tokens.size(); ++i) {
        const QString& t = tokens.at(i);
        if (t.isEmpty()) continue;
        if (t.startsWith(QLatin1String("# branch.oid "))) {
            const QString oid = t.mid(13);
            r.head = oid == QLatin1String("(initial)") ? QString() : oid.left(7);
        } else if (t.startsWith(QLatin1String("# branch.head "))) {
            const QString head = t.mid(14);
            r.branch = head == QLatin1String("(detached)") ? QString() : head;
        } else if (t.startsWith(QLatin1String("# branch.upstream "))) {
            r.upstream = t.mid(18);
        } else if (t.startsWith(QLatin1String("# branch.ab "))) {
            const QStringList ab = t.mid(12).split(QLatin1Char(' '));
            if (ab.size() == 2) {
                r.ahead = ab.at(0).mid(1).toInt();
                r.behind = ab.at(1).mid(1).toInt();
            }
        } else if (t.startsWith(QLatin1String("1 ")) || t.startsWith(QLatin1String("2 ")) || t.startsWith(QLatin1String("u "))) {
            Entry e;
            const QString xy = t.section(QLatin1Char(' '), 1, 1);
            if (xy.size() == 2) {
                e.staged = xy.at(0);
                e.unstaged = xy.at(1);
            }
            if (t.at(0) == QLatin1Char('1')) e.path = t.section(QLatin1Char(' '), 8);
            else if (t.at(0) == QLatin1Char('2')) {
                e.path = t.section(QLatin1Char(' '), 9);
                if (i + 1 < tokens.size()) e.from = tokens.at(++i);
            } else {
                e.path = t.section(QLatin1Char(' '), 10);
                e.conflicted = true;
            }
            r.entries << e;
        } else if (t.startsWith(QLatin1String("? ")) || t.startsWith(QLatin1String("! "))) {
            Entry e;
            e.path = t.mid(2);
            (t.at(0) == QLatin1Char('?') ? e.untracked : e.ignored) = true;
            r.entries << e;
        }
    }
    const QDir git(r.gitDir);
    if (git.exists(QStringLiteral("MERGE_HEAD"))) r.operation = QStringLiteral("merge");
    else if (git.exists(QStringLiteral("rebase-merge")) || git.exists(QStringLiteral("rebase-apply"))) r.operation = QStringLiteral("rebase");
    else if (git.exists(QStringLiteral("CHERRY_PICK_HEAD"))) r.operation = QStringLiteral("cherry-pick");
    else if (git.exists(QStringLiteral("REVERT_HEAD"))) r.operation = QStringLiteral("revert");
    else if (git.exists(QStringLiteral("BISECT_LOG"))) r.operation = QStringLiteral("bisect");
    r.index();
    return r;
}

// ----------------------------------------------------------------------
// What is done with it

Result stage(const QString& root, const QStringList& paths)
{
    if (paths.isEmpty()) return run(root, {QStringLiteral("add"), QStringLiteral("-A")});
    const QStringList rel = relative(root, paths);
    if (rel.isEmpty()) return outside();
    return run(root, withPaths({QStringLiteral("add"), QStringLiteral("-A")}, rel));
}

Result unstage(const QString& root, const QStringList& paths)
{
    const QStringList rel = paths.isEmpty() ? QStringList{QStringLiteral(".")} : relative(root, paths);
    if (rel.isEmpty()) return outside();
    if (!hasHead(root))   // (before the first commit: nothing to go back to)
        return run(root, withPaths({QStringLiteral("rm"), QStringLiteral("--cached"), QStringLiteral("-r"), QStringLiteral("-q")}, rel));
    if (paths.isEmpty()) return run(root, {QStringLiteral("reset"), QStringLiteral("-q")});
    return run(root, withPaths({QStringLiteral("reset"), QStringLiteral("-q"), QStringLiteral("HEAD")}, rel));
}

Result discard(const QString& root, const QStringList& paths, QStringList* trashed)
{
    Result r;
    r.exitCode = 0;
    const Repository repo = read(root);
    if (!repo.valid) {
        r.exitCode = 1;
        r.err = tr("fatal: not in a git repository");
        return r;
    }
    const bool head = !repo.head.isEmpty();
    // The entries the paths take in: their own, those in a folder given.
    QList<Entry> chosen;
    const QStringList rel = paths.isEmpty() ? QStringList{QString()} : relative(root, paths);
    if (rel.isEmpty()) return outside();
    for (const Entry& e : repo.entries) {
        if (e.ignored) continue;
        for (QString p : rel) {
            if (p == QLatin1String(".")) p.clear();
            const QString bare = e.isFolder() ? e.path.chopped(1) : e.path;
            if (p.isEmpty() || bare == p || bare.startsWith(p + QLatin1Char('/')) || (e.isFolder() && p.startsWith(e.path))) {
                chosen << e;
                break;
            }
        }
    }
    QStringList restore, forget, toTrash;
    for (const Entry& e : chosen) {
        if (e.untracked) {
            // In an untracked folder: the path given (a file in it), else the folder.
            QString target = e.path;
            if (e.isFolder()) {
                target.chop(1);
                for (const QString& p : rel)
                    if (!p.isEmpty() && p != QLatin1String(".") && p.startsWith(e.path)) target = p;
            }
            toTrash << target;
        } else if (!head || e.staged == QLatin1Char('A') || e.staged == QLatin1Char('C') || e.staged == QLatin1Char('R')) {
            forget << e.path;   // not in HEAD: out of the index, then to the trash
            toTrash << e.path;
            if (e.staged == QLatin1Char('R') && !e.from.isEmpty()) restore << e.from;
        } else {
            restore << e.path;
        }
    }
    if (!forget.isEmpty()) {
        const Result f = run(root, withPaths({QStringLiteral("rm"), QStringLiteral("--cached"), QStringLiteral("-q"), QStringLiteral("-f"),
                                              QStringLiteral("-r")}, forget));
        if (!f.ok()) return f;
    }
    if (!restore.isEmpty()) {
        const Result c = run(root, withPaths({QStringLiteral("checkout"), QStringLiteral("HEAD")}, restore));
        if (!c.ok()) return c;
    }
    QStringList failed;
    for (const QString& p : std::as_const(toTrash)) {
        const QString path = QDir(root).filePath(p);
        if (!QFileInfo::exists(path) && !QFileInfo(path).isSymLink()) continue;
        QString where;
        if (misc::moveToTrash(path, &where)) {
            if (trashed != nullptr) *trashed << where;
        } else {
            failed << p;
        }
    }
    if (!failed.isEmpty()) {
        r.exitCode = 1;
        r.err = tr("error: could not be moved to the trash: %1").arg(failed.join(QStringLiteral(", ")));
    }
    return r;
}

Result commit(const QString& root, const QString& message, bool amend, const QStringList& only)
{
    QStringList args{QStringLiteral("commit"), QStringLiteral("-F"), QStringLiteral("-")};
    if (amend) args << QStringLiteral("--amend");
    if (!only.isEmpty()) {
        const QStringList rel = relative(root, only);
        if (rel.isEmpty()) return outside();
        const Result added = run(root, withPaths({QStringLiteral("add"), QStringLiteral("-A")}, rel));
        if (!added.ok()) return added;
        args = withPaths(args, rel);
    }
    return run(root, args, false, 300000, message.toUtf8());
}

QString lastMessage(const QString& root)
{
    const Result r = run(root, {QStringLiteral("log"), QStringLiteral("-1"), QStringLiteral("--format=%B")}, true, 15000);
    return r.ok() ? r.out.trimmed() : QString();
}

Result untrack(const QString& root, const QStringList& paths)
{
    const QStringList rel = relative(root, paths);
    if (rel.isEmpty()) return outside();
    return run(root, withPaths({QStringLiteral("rm"), QStringLiteral("--cached"), QStringLiteral("-r"), QStringLiteral("-q")}, rel));
}

bool ignore(const QString& root, const QString& path, QString* why)
{
    bool inside = false;
    QString rel = QFileInfo(path).isAbsolute() ? relativePath(root, path, &inside) : QDir::cleanPath(path);
    if ((QFileInfo(path).isAbsolute() && !inside) || rel.isEmpty() || rel == QLatin1String(".")) {
        if (why != nullptr) *why = tr("it is not a file of the repository");
        return false;
    }
    // A pattern for it alone: from the top folder, its special characters
    // taken as they are.
    QString pattern;
    for (const QChar c : std::as_const(rel)) {
        if (QStringLiteral("*?[]!#\\").contains(c)) pattern += QLatin1Char('\\');
        pattern += c;
    }
    pattern = QLatin1Char('/') + pattern;
    if (QFileInfo(QDir(root).filePath(rel)).isDir()) pattern += QLatin1Char('/');
    QFile file(QDir(root).filePath(QStringLiteral(".gitignore")));
    QByteArray text;
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly)) {
            if (why != nullptr) *why = file.errorString();
            return false;
        }
        text = file.readAll();
        file.close();
        for (const QByteArray& l : text.split('\n'))
            if (QString::fromUtf8(l).trimmed() == pattern) return true;
    }
    if (!file.open(QIODevice::Append)) {
        if (why != nullptr) *why = file.errorString();
        return false;
    }
    QByteArray add;
    if (!text.isEmpty() && !text.endsWith('\n')) add += '\n';
    add += pattern.toUtf8() + '\n';
    if (file.write(add) != add.size()) {
        if (why != nullptr) *why = file.errorString();
        return false;
    }
    return true;
}

Result init(const QString& folder)
{
    return run(folder, {QStringLiteral("init")});
}

// ----------------------------------------------------------------------
// Branches, tags, remotes

QList<Branch> branches(const QString& root)
{
    QList<Branch> out;
    const Result r = run(root,
                         {QStringLiteral("for-each-ref"),
                          QStringLiteral("--format=%(refname)%1f%(refname:short)%1f%(objectname:short)%1f%(upstream:short)%1f"
                                         "%(upstream:track,nobracket)%1f%(HEAD)%1f%(committerdate:iso-strict)%1f%(contents:subject)%1e"),
                          QStringLiteral("refs/heads"), QStringLiteral("refs/remotes")},
                         true, 15000);
    if (!r.ok()) return out;
    for (const QString& record : r.out.split(QChar(0x1e), Qt::SkipEmptyParts)) {
        const QStringList f = record.trimmed().split(QChar(0x1f));
        if (f.size() < 8) continue;
        if (f.at(0).startsWith(QLatin1String("refs/remotes/")) && f.at(0).endsWith(QLatin1String("/HEAD"))) continue;
        Branch b;
        b.remote = f.at(0).startsWith(QLatin1String("refs/remotes/"));
        b.name = f.at(1);
        b.commit = f.at(2);
        b.upstream = f.at(3);
        static const QRegularExpression ahead(QStringLiteral("ahead (\\d+)")), behind(QStringLiteral("behind (\\d+)"));
        if (const auto m = ahead.match(f.at(4)); m.hasMatch()) b.ahead = m.captured(1).toInt();
        if (const auto m = behind.match(f.at(4)); m.hasMatch()) b.behind = m.captured(1).toInt();
        b.current = f.at(5).trimmed() == QLatin1String("*");
        b.date = QDateTime::fromString(f.at(6), Qt::ISODate);
        b.subject = f.at(7);
        out << b;
    }
    std::stable_sort(out.begin(), out.end(), [](const Branch& a, const Branch& b) { return !a.remote && b.remote; });
    return out;
}

QStringList tags(const QString& root)
{
    const Result r = run(root, {QStringLiteral("tag"), QStringLiteral("--sort=-creatordate")}, true, 15000);
    return r.ok() ? linesOf(r.out) : QStringList();
}

Result switchTo(const QString& root, const QString& branch)
{
    if (optionLike(branch)) return optionRefused(branch);
    // A remote's branch: a local one that follows it (made when there is none).
    for (const Branch& b : branches(root)) {
        if (!b.remote || b.name != branch) continue;
        const QString local = branch.section(QLatin1Char('/'), 1);
        for (const Branch& l : branches(root))
            if (!l.remote && l.name == local) return run(root, {QStringLiteral("checkout"), local, QStringLiteral("--")});
        return run(root, {QStringLiteral("checkout"), QStringLiteral("-b"), local, QStringLiteral("--track"), branch});
    }
    return run(root, {QStringLiteral("checkout"), branch, QStringLiteral("--")});
}

Result createBranch(const QString& root, const QString& name, const QString& start, bool checkOut)
{
    if (optionLike(name) || optionLike(start)) return optionRefused(optionLike(name) ? name : start);
    QStringList args = checkOut ? QStringList{QStringLiteral("checkout"), QStringLiteral("-b"), name}
                                : QStringList{QStringLiteral("branch"), name};
    if (!start.isEmpty()) args << start;
    return run(root, args);
}

Result deleteBranch(const QString& root, const QString& name, bool force)
{
    if (optionLike(name)) return optionRefused(name);
    return run(root, {QStringLiteral("branch"), force ? QStringLiteral("-D") : QStringLiteral("-d"), name});
}

Result renameBranch(const QString& root, const QString& from, const QString& to)
{
    if (optionLike(from) || optionLike(to)) return optionRefused(optionLike(from) ? from : to);
    return run(root, {QStringLiteral("branch"), QStringLiteral("-m"), from, to});
}

Result merge(const QString& root, const QString& ref)
{
    if (optionLike(ref)) return optionRefused(ref);
    return run(root, {QStringLiteral("merge"), QStringLiteral("--no-edit"), ref}, false, 300000);
}

Result abort(const QString& root, const QString& operation)
{
    if (operation == QLatin1String("bisect")) return run(root, {QStringLiteral("bisect"), QStringLiteral("reset")});
    if (operation == QLatin1String("merge") || operation == QLatin1String("rebase") || operation == QLatin1String("cherry-pick")
        || operation == QLatin1String("revert"))
        return run(root, {operation, QStringLiteral("--abort")});
    Result r;
    r.exitCode = 1;
    r.err = tr("error: nothing is under way to abort");
    return r;
}

Result createTag(const QString& root, const QString& name, const QString& message, const QString& commit)
{
    if (optionLike(name) || optionLike(commit)) return optionRefused(optionLike(name) ? name : commit);
    QStringList args{QStringLiteral("tag")};
    if (!message.isEmpty()) args << QStringLiteral("-a") << QStringLiteral("-F") << QStringLiteral("-");
    args << name;
    if (!commit.isEmpty()) args << commit;
    return run(root, args, false, 60000, message.toUtf8());
}

Result deleteTag(const QString& root, const QString& name)
{
    if (optionLike(name)) return optionRefused(name);
    return run(root, {QStringLiteral("tag"), QStringLiteral("-d"), name});
}

QList<Remote> remotes(const QString& root)
{
    QList<Remote> out;
    const Result r = run(root, {QStringLiteral("remote"), QStringLiteral("-v")}, true, 15000);
    if (!r.ok()) return out;
    for (const QString& l : linesOf(r.out)) {
        static const QRegularExpression line(QStringLiteral("^(\\S+)\\s+(.*)\\s+\\((fetch|push)\\)$"));
        const auto m = line.match(l.trimmed());
        if (!m.hasMatch()) continue;
        auto it = std::find_if(out.begin(), out.end(), [&](const Remote& x) { return x.name == m.captured(1); });
        if (it == out.end()) {
            out << Remote{m.captured(1), {}, {}};
            it = out.end() - 1;
        }
        (m.captured(3) == QLatin1String("fetch") ? it->fetchUrl : it->pushUrl) = m.captured(2).trimmed();
    }
    return out;
}

Result addRemote(const QString& root, const QString& name, const QString& url)
{
    if (optionLike(name) || optionLike(url)) return optionRefused(optionLike(name) ? name : url);
    return run(root, {QStringLiteral("remote"), QStringLiteral("add"), name, url});
}

Result removeRemote(const QString& root, const QString& name)
{
    if (optionLike(name)) return optionRefused(name);
    return run(root, {QStringLiteral("remote"), QStringLiteral("remove"), name});
}

QStringList fetchArgs(const QString& remote)
{
    if (optionLike(remote)) return {};
    return {QStringLiteral("fetch"), QStringLiteral("--prune"), QStringLiteral("--progress"),
            remote.isEmpty() ? QStringLiteral("--all") : remote};
}

QStringList pullArgs()
{
    return {QStringLiteral("pull"), QStringLiteral("--no-edit"), QStringLiteral("--progress")};
}

QStringList pushArgs(const Repository& repo, QString* why, bool tags)
{
    if (repo.branch.isEmpty()) {
        if (why != nullptr) *why = tr("HEAD is detached: there is no branch to push");
        return {};
    }
    QStringList args{QStringLiteral("push"), QStringLiteral("--progress")};
    if (tags) args << QStringLiteral("--follow-tags");
    if (!repo.upstream.isEmpty()) return args;
    const QList<Remote> all = remotes(repo.root);
    if (all.isEmpty()) {
        if (why != nullptr) *why = tr("the repository has no remote to push to (Git > Remotes... adds one)");
        return {};
    }
    QString remote = all.first().name;
    for (const Remote& r : all)
        if (r.name == QLatin1String("origin")) remote = r.name;
    return args << QStringLiteral("-u") << remote << repo.branch;
}

QStringList cloneArgs(const QString& url, const QString& folder)
{
    return {QStringLiteral("clone"), QStringLiteral("--progress"), QStringLiteral("--"), url, folder};
}

// ----------------------------------------------------------------------
// Stashes

QList<Stash> stashes(const QString& root)
{
    QList<Stash> out;
    const Result r = run(root, {QStringLiteral("stash"), QStringLiteral("list"), QStringLiteral("--format=%gd%x1f%gs%x1f%cI")}, true, 15000);
    if (!r.ok()) return out;
    for (const QString& l : linesOf(r.out)) {
        const QStringList f = l.split(QChar(0x1f));
        if (f.size() < 3) continue;
        static const QRegularExpression index(QStringLiteral("\\{(\\d+)\\}"));
        const auto m = index.match(f.at(0));
        if (!m.hasMatch()) continue;
        out << Stash{m.captured(1).toInt(), f.at(1), QDateTime::fromString(f.at(2), Qt::ISODate)};
    }
    return out;
}

Result stash(const QString& root, const QString& message, bool untracked)
{
    QStringList args{QStringLiteral("stash"), QStringLiteral("push")};
    if (untracked) args << QStringLiteral("--include-untracked");
    if (!message.isEmpty()) args << QStringLiteral("-m") << message;
    return run(root, args);
}

Result applyStash(const QString& root, int index, bool pop)
{
    return run(root, {QStringLiteral("stash"), pop ? QStringLiteral("pop") : QStringLiteral("apply"), QStringLiteral("stash@{%1}").arg(index)});
}

Result dropStash(const QString& root, int index)
{
    return run(root, {QStringLiteral("stash"), QStringLiteral("drop"), QStringLiteral("stash@{%1}").arg(index)});
}

QString showStash(const QString& root, int index)
{
    const Result r = run(root, {QStringLiteral("stash"), QStringLiteral("show"), QStringLiteral("-p"), QStringLiteral("--no-ext-diff"),
                                QStringLiteral("--no-textconv"), QStringLiteral("stash@{%1}").arg(index)},
                         true, 30000);
    return r.ok() ? r.out : r.error();
}

// ----------------------------------------------------------------------
// History

namespace {

// A commit a line: its fields between unit separators, the refs' names
// whole (refs/heads/..., refs/remotes/...: a branch "feature/x" is not a
// remote's).
const QString kCommitFormat = QStringLiteral("--format=%H%x1f%h%x1f%an%x1f%ae%x1f%aI%x1f%s%x1f%D%x1f%P%x1f%cn%x1f%ce%x1f%cI%x1e");

// %D's names whole, as git log --decorate=full gives them: each with its
// kind, and as the short names git gives without it ("HEAD -> main",
// "origin/main", "tag: v1.0").
void readRefs(const QString& decoration, Commit& c)
{
    const QString heads = QStringLiteral("refs/heads/"), remotes = QStringLiteral("refs/remotes/"), tags = QStringLiteral("refs/tags/");
    for (QString item : decoration.split(QStringLiteral(", "), Qt::SkipEmptyParts)) {
        item = item.trimmed();
        bool current = false;
        if (item.startsWith(QLatin1String("HEAD -> "))) {
            current = true;
            item = item.mid(8);
        }
        Ref ref;
        QString shortName;
        if (item == QLatin1String("HEAD")) {
            ref.kind = Ref::Head;
            ref.name = shortName = item;
        } else if (item.startsWith(QLatin1String("tag: "))) {
            ref.kind = Ref::Tag;
            ref.name = item.mid(5).startsWith(tags) ? item.mid(5 + tags.size()) : item.mid(5);
            shortName = QStringLiteral("tag: ") + ref.name;
        } else if (item.startsWith(heads)) {
            ref.kind = Ref::Branch;
            ref.name = shortName = item.mid(heads.size());
        } else if (item.startsWith(remotes)) {
            ref.kind = Ref::Remote;
            ref.name = shortName = item.mid(remotes.size());
        } else {
            shortName = item.startsWith(QLatin1String("refs/")) ? item.mid(5) : item;
            ref.name.clear();   // (the stash, notes: no label)
        }
        ref.current = current;
        c.refs << (current ? QStringLiteral("HEAD -> ") + shortName : shortName);
        // (A remote's HEAD names its default branch: no label of its own.)
        if (!ref.name.isEmpty() && !(ref.kind == Ref::Remote && ref.name.endsWith(QLatin1String("/HEAD")))) c.refList << ref;
    }
}

QList<Commit> readCommits(const QString& text)
{
    QList<Commit> out;
    for (const QString& record : text.split(QChar(0x1e), Qt::SkipEmptyParts)) {
        const QStringList f = record.trimmed().split(QChar(0x1f));
        if (f.size() < 11) continue;
        Commit c;
        c.hash = f.at(0);
        c.shortHash = f.at(1);
        c.author = f.at(2);
        c.email = f.at(3);
        c.date = QDateTime::fromString(f.at(4), Qt::ISODate);
        c.subject = f.at(5);
        readRefs(f.at(6), c);
        c.parents = f.at(7).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        c.committer = f.at(8);
        c.committerEmail = f.at(9);
        c.committed = QDateTime::fromString(f.at(10), Qt::ISODate);
        out << c;
    }
    return out;
}

} // namespace

QList<Commit> log(const QString& root, const QString& path, int max, const QString& ref, int skip)
{
    QList<Commit> out;
    QStringList args{QStringLiteral("log"), QStringLiteral("--decorate=full"), kCommitFormat, QStringLiteral("-n"),
                     QString::number(std::max(1, max))};
    if (skip > 0) args << QStringLiteral("--skip=%1").arg(skip);
    if (optionLike(ref)) return out;
    if (!ref.isEmpty()) args << ref;
    if (!path.isEmpty()) {
        const QStringList rel = relative(root, {path});
        if (rel.isEmpty()) return out;
        // The top folder is the whole repository: no path (with one, git
        // drops a merge whose tree is one side's - a conflict resolved
        // with theirs).
        if (rel.first() != QLatin1String(".")) {
            if (!QFileInfo(QDir(root).filePath(rel.first())).isDir()) args << QStringLiteral("--follow");
            args << QStringLiteral("--") << rel;
        }
    }
    const Result r = run(root, args, true, 30000);
    return r.ok() ? readCommits(r.out) : out;
}

QList<Commit> history(const QString& root, const HistoryQuery& query)
{
    QStringList args{QStringLiteral("log"), QStringLiteral("--decorate=full"), kCommitFormat, QStringLiteral("--date-order"),
                     QStringLiteral("-n"), QString::number(std::max(1, query.max))};
    if (query.skip > 0) args << QStringLiteral("--skip=%1").arg(query.skip);
    if (query.firstParent) args << QStringLiteral("--first-parent");
    switch (query.scope) {
    case HistoryQuery::All:
        args << QStringLiteral("--branches");
        if (query.remotes) args << QStringLiteral("--remotes");
        if (query.tags) args << QStringLiteral("--tags");
        args << QStringLiteral("HEAD");
        break;
    case HistoryQuery::Current: args << QStringLiteral("HEAD"); break;
    case HistoryQuery::OneRef:
        if (query.ref.isEmpty() || optionLike(query.ref)) return {};
        args << query.ref;
        break;
    }
    if (!query.path.isEmpty()) {
        const QStringList rel = relative(root, {query.path});
        if (rel.isEmpty()) return {};
        if (rel.first() != QLatin1String(".")) {
            if (!QFileInfo(QDir(root).filePath(rel.first())).isDir()) args << QStringLiteral("--follow");
            args << QStringLiteral("--") << rel;
        }
    }
    const Result r = run(root, args, true, 60000);
    return r.ok() ? readCommits(r.out) : QList<Commit>();
}

namespace {

// What diff-tree compares \a commit with: its first parent (a merge's:
// what it brought into its branch - "-m --first-parent" gives one diff a
// parent), a first commit with nothing.
QStringList againstFirstParent(const QString& root, const QString& commit)
{
    const Result parent = run(root, {QStringLiteral("rev-parse"), QStringLiteral("--verify"), QStringLiteral("-q"), commit + QStringLiteral("^1")},
                              true, 15000);
    if (parent.ok() && !parent.out.trimmed().isEmpty()) return {parent.out.trimmed(), commit};
    return {QStringLiteral("--root"), commit};
}

} // namespace

QList<ChangedFile> changedFiles(const QString& root, const QString& commit)
{
    if (commit.isEmpty() || optionLike(commit)) return {};
    // NUL between the names: any name read whole.
    const QStringList trees = againstFirstParent(root, commit);
    const QStringList common{QStringLiteral("diff-tree"), QStringLiteral("-r"), QStringLiteral("-M"), QStringLiteral("--no-commit-id"),
                             QStringLiteral("-z")};
    const Result names = run(root, common + QStringList{QStringLiteral("--name-status")} + trees, true, 30000);
    if (!names.ok()) return {};
    QList<ChangedFile> out;
    QHash<QString, int> byPath;
    const QStringList n = names.out.split(QChar(0), Qt::KeepEmptyParts);
    for (int i = 0; i + 1 < n.size();) {
        const QString status = n.at(i);
        if (status.isEmpty()) {
            ++i;
            continue;
        }
        ChangedFile f;
        f.status = status.at(0);
        if ((f.status == QLatin1Char('R') || f.status == QLatin1Char('C')) && i + 2 < n.size()) {
            f.from = n.at(i + 1);
            f.path = n.at(i + 2);
            i += 3;
        } else {
            f.path = n.at(i + 1);
            i += 2;
        }
        byPath.insert(f.path, int(out.size()));
        out << f;
    }
    // The lines: "added<TAB>removed<TAB>path", a rename's names after it.
    const Result counts = run(root, common + QStringList{QStringLiteral("--numstat")} + trees, true, 30000);
    const QStringList c = counts.out.split(QChar(0), Qt::KeepEmptyParts);
    for (int i = 0; i < c.size(); ++i) {
        const QStringList parts = c.at(i).split(QLatin1Char('\t'));
        if (parts.size() < 3) continue;
        QString path = parts.at(2);
        if (path.isEmpty() && i + 2 < c.size()) {   // a rename: its names follow
            path = c.at(i + 2);
            i += 2;
        }
        const int at = byPath.value(path, -1);
        if (at < 0) continue;
        bool okA = false, okR = false;
        const int added = parts.at(0).toInt(&okA), removed = parts.at(1).toInt(&okR);
        out[at].added = okA ? added : -1;
        out[at].removed = okR ? removed : -1;
    }
    return out;
}

QString commitMessage(const QString& root, const QString& commit)
{
    if (commit.isEmpty() || optionLike(commit)) return {};
    const Result r = run(root, {QStringLiteral("show"), QStringLiteral("-s"), QStringLiteral("--format=%B"), commit}, true, 15000);
    return r.ok() ? r.out.trimmed() : QString();
}

QString commitDiff(const QString& root, const QString& commit, const QString& path)
{
    if (commit.isEmpty() || optionLike(commit)) return optionRefused(commit).error();
    QStringList args = QStringList{QStringLiteral("diff-tree"), QStringLiteral("-p"), QStringLiteral("-M"), QStringLiteral("--no-commit-id"),
                                   QStringLiteral("--no-color")}
                       + againstFirstParent(root, commit);
    if (!path.isEmpty()) {
        const QStringList rel = relative(root, {path});
        if (rel.isEmpty()) return {};
        args << QStringLiteral("--") << rel;
    }
    const Result r = run(root, args, true, 30000);
    return r.ok() ? r.out : r.error();
}

QByteArray fileAt(const QString& root, const QString& commit, const QString& path, bool* ok)
{
    if (ok != nullptr) *ok = false;
    if (commit.isEmpty() || optionLike(commit) || path.isEmpty()) return {};
    const QStringList rel = relative(root, {path});
    if (rel.isEmpty() || rel.first() == QLatin1String(".")) return {};
    const Raw r = runRaw(root, {QStringLiteral("cat-file"), QStringLiteral("blob"), commit + QLatin1Char(':') + rel.first()}, true, 30000);
    if (r.exitCode != 0) return {};
    if (ok != nullptr) *ok = true;
    return r.out;
}

QString show(const QString& root, const QString& commit, const QString& path)
{
    if (optionLike(commit)) return optionRefused(commit).error();
    QStringList args{QStringLiteral("show"), QStringLiteral("--stat"), QStringLiteral("--patch"), QStringLiteral("--format=fuller"),
                     QStringLiteral("--no-ext-diff"), QStringLiteral("--no-textconv"), commit};
    if (!path.isEmpty()) args << QStringLiteral("--") << relative(root, {path});
    const Result r = run(root, args, true, 30000);
    return r.ok() ? r.out : r.error();
}

QString diff(const QString& root, const QString& path, DiffOf of)
{
    const QStringList common{QStringLiteral("--no-ext-diff"), QStringLiteral("--no-textconv"), QStringLiteral("--no-color")};
    const Repository repo = read(root);
    const QStringList rel = path.isEmpty() ? QStringList() : relative(root, {path});
    if (!path.isEmpty() && rel.isEmpty()) return {};
    // An untracked file: all of it added.
    const auto untrackedDiff = [&](const QString& file) {
        const Result r = run(root, QStringList{QStringLiteral("diff"), QStringLiteral("--no-index")} + common
                                       + QStringList{QStringLiteral("--"), QStringLiteral("/dev/null"), file},
                             true, 30000);
        return r.out;   // (exit code 1: they differ)
    };
    if (!rel.isEmpty() && of != DiffOf::Staged) {
        const Entry* e = repo.entryOf(rel.first());
        if (e != nullptr && e->untracked && !QFileInfo(QDir(root).filePath(rel.first())).isDir()) return untrackedDiff(rel.first());
    }
    QStringList args{QStringLiteral("diff")};
    args << common;
    const bool head = !repo.head.isEmpty();
    if (of == DiffOf::Staged || (of == DiffOf::Head && !head)) args << QStringLiteral("--cached");
    else if (of == DiffOf::Head) args << QStringLiteral("HEAD");
    if (!rel.isEmpty()) args << QStringLiteral("--") << rel;
    const Result r = run(root, args, true, 60000);
    QString text = r.ok() ? r.out : r.error();
    if (of == DiffOf::Head && !head && r.ok()) {   // (before the first commit: what is not staged too)
        QStringList unstaged{QStringLiteral("diff")};
        unstaged << common;
        if (!rel.isEmpty()) unstaged << QStringLiteral("--") << rel;
        text += run(root, unstaged, true, 60000).out;
    }
    // And the untracked files in it, all added.
    if (of != DiffOf::Staged) {
        QStringList others{QStringLiteral("ls-files"), QStringLiteral("--others"), QStringLiteral("--exclude-standard")};
        if (!rel.isEmpty()) others << QStringLiteral("--") << rel;
        const Result o = run(root, others, true, 30000);
        int n = 0;
        for (const QString& f : linesOf(o.out)) {
            if (++n > 200) break;
            text += untrackedDiff(f);
        }
    }
    return text;
}

QList<BlameLine> blame(const QString& root, const QString& path)
{
    QList<BlameLine> out;
    const QStringList rel = relative(root, {path});
    if (rel.isEmpty()) return out;
    const Result r = run(root, {QStringLiteral("blame"), QStringLiteral("--porcelain"), QStringLiteral("--no-textconv"),
                                QStringLiteral("--"), rel.first()},
                         true, 60000);
    if (!r.ok()) return out;
    struct Info {
        QString author, summary;
        QDateTime date;
    };
    QHash<QString, Info> infos;
    BlameLine current;
    QString hash;
    for (const QString& l : r.out.split(QLatin1Char('\n'))) {
        if (l.startsWith(QLatin1Char('\t'))) {
            current.text = l.mid(1);
            const Info info = infos.value(hash);
            current.hash = hash;
            current.shortHash = hash.left(7);
            current.author = info.author;
            current.summary = info.summary;
            current.date = info.date;
            current.uncommitted = hash.startsWith(QLatin1String("0000000"));
            out << current;
            continue;
        }
        static const QRegularExpression header(QStringLiteral("^([0-9a-f]{40,64}) \\d+ (\\d+)"));
        if (const auto m = header.match(l); m.hasMatch()) {
            hash = m.captured(1);
            current = BlameLine();
            current.line = m.captured(2).toInt();
            continue;
        }
        Info& info = infos[hash];
        if (l.startsWith(QLatin1String("author "))) info.author = l.mid(7);
        else if (l.startsWith(QLatin1String("author-time "))) info.date = QDateTime::fromSecsSinceEpoch(l.mid(12).toLongLong());
        else if (l.startsWith(QLatin1String("summary "))) info.summary = l.mid(8);
    }
    return out;
}

Result checkOutCommit(const QString& root, const QString& commit)
{
    if (optionLike(commit)) return optionRefused(commit);
    return run(root, {QStringLiteral("checkout"), QStringLiteral("--detach"), commit});
}

Result revert(const QString& root, const QString& commit)
{
    if (optionLike(commit)) return optionRefused(commit);
    return run(root, {QStringLiteral("revert"), QStringLiteral("--no-edit"), commit}, false, 300000);
}

Result cherryPick(const QString& root, const QString& commit)
{
    if (optionLike(commit)) return optionRefused(commit);
    return run(root, {QStringLiteral("cherry-pick"), commit}, false, 300000);
}

Result reset(const QString& root, const QString& commit, const QString& mode)
{
    if (optionLike(commit)) return optionRefused(commit);
    if (mode != QLatin1String("soft") && mode != QLatin1String("mixed") && mode != QLatin1String("hard")) {
        Result r;
        r.exitCode = 1;
        r.err = tr("error: a reset is soft, mixed or hard");
        return r;
    }
    return run(root, {QStringLiteral("reset"), QStringLiteral("--") + mode, commit});
}

// ----------------------------------------------------------------------
// Conflicts

bool hasConflictMarkers(const QString& path)
{
    QFile file(path);
    if (file.size() > (qint64(64) << 20) || !file.open(QIODevice::ReadOnly)) return false;
    // ("<<<<<<< ours", "=======", ">>>>>>> theirs" in that order; diff3's
    // "||||||| base" between the first two.)
    int stage = 0;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine();
        if (stage == 0 && line.startsWith("<<<<<<<") && (line.size() == 7 || line.at(7) == ' ' || line.at(7) == '\n' || line.at(7) == '\r'))
            stage = 1;
        else if (stage == 1 && (line == "=======\n" || line == "=======\r\n" || line == "======="))
            stage = 2;
        else if (stage == 2 && line.startsWith(">>>>>>>") && (line.size() == 7 || line.at(7) == ' ' || line.at(7) == '\n' || line.at(7) == '\r'))
            return true;
    }
    return false;
}

ConflictVersions conflictVersions(const QString& root, const QString& path)
{
    ConflictVersions v;
    const QStringList rel = relative(root, {path});
    if (rel.isEmpty()) return v;
    const Result stages = run(root, {QStringLiteral("ls-files"), QStringLiteral("-u"), QStringLiteral("--"), rel.first()}, true, 15000);
    if (!stages.ok()) return v;
    // "mode hash stage<TAB>path": 1 the base, 2 ours, 3 theirs.
    for (const QString& l : linesOf(stages.out)) {
        const int stage = l.section(QLatin1Char('\t'), 0, 0).section(QLatin1Char(' '), 2, 2).toInt();
        if (stage < 1 || stage > 3) continue;
        v.inConflict = true;
        const Result text = run(root, {QStringLiteral("show"), QStringLiteral(":%1:%2").arg(stage).arg(rel.first())}, true, 30000);
        if (!text.ok()) continue;
        (stage == 1 ? v.base : stage == 2 ? v.ours : v.theirs) = text.out;
    }
    return v;
}

Result resolve(const QString& root, const QString& path, const QString& side)
{
    if (side != QLatin1String("ours") && side != QLatin1String("theirs"))
        return failure(tr("error: a conflict is resolved with ours or theirs"));
    const QStringList rel = relative(root, {path});
    if (rel.isEmpty() || rel.first() == QLatin1String(".")) return outside();
    const ConflictVersions v = conflictVersions(root, path);
    if (!v.inConflict) return failure(tr("error: %1 is not in conflict").arg(rel.first()));
    // That side deleted it: deleted.
    if (!(side == QLatin1String("ours") ? v.ours : v.theirs).has_value())
        return run(root, withPaths({QStringLiteral("rm"), QStringLiteral("-q"), QStringLiteral("--ignore-unmatch")}, rel));
    const Result taken = run(root, withPaths({QStringLiteral("checkout"), QStringLiteral("--") + side}, rel));
    if (!taken.ok()) return taken;
    return run(root, withPaths({QStringLiteral("add"), QStringLiteral("-f")}, rel));
}

// ----------------------------------------------------------------------
// Tracker

Tracker* Tracker::instance()
{
    static QPointer<Tracker> one;
    if (one.isNull()) one = new Tracker(QCoreApplication::instance());
    return one;
}

Tracker::Tracker(QObject* parent)
    : QObject(parent), a_watcher(new QFileSystemWatcher(this)), a_soon(new QTimer(this)), a_every(new QTimer(this))
{
    a_soon->setSingleShot(true);
    a_soon->setInterval(150);
    connect(a_soon, &QTimer::timeout, this, &Tracker::readNext);
    // A git command (here or elsewhere): HEAD, the index, a merge's files.
    const auto changedAt = [this](const QString& path) {
        for (auto it = a_repos.cbegin(); it != a_repos.cend(); ++it)
            if (!it->gitDir.isEmpty() && (path == it->gitDir || path.startsWith(it->gitDir + QLatin1Char('/')))) readSoon(it.key());
    };
    connect(a_watcher, &QFileSystemWatcher::fileChanged, this, changedAt);
    connect(a_watcher, &QFileSystemWatcher::directoryChanged, this, changedAt);
    // A file edited elsewhere: those looked at lately, every few seconds -
    // one slow to read (a home folder that is a repository) less often.
    a_every->setInterval(5000);
    connect(a_every, &QTimer::timeout, this, [this] {
        a_roots.clear();   // (a repository made elsewhere: noticed)
        const qint64 now = nowMs();
        for (auto it = a_used.cbegin(); it != a_used.cend(); ++it) {
            if (now - it.value() >= 30000) continue;
            const qint64 every = std::max<qint64>(4500, 20 * a_cost.value(it.key()));
            if (now - a_polled.value(it.key(), 0) < every) continue;
            a_polled.insert(it.key(), now);
            readSoon(it.key());
        }
    });
    a_every->start();
}

QString Tracker::rootOf(const QString& path)
{
    // (Asked as the views draw: the folders' .git looked for once a while.)
    if (const auto it = a_roots.constFind(path); it != a_roots.cend()) return *it;
    if (a_roots.size() > 20000) a_roots.clear();
    const QString root = topLevel(path);
    a_roots.insert(path, root);
    return root;
}

const Repository* Tracker::repositoryOf(const QString& path)
{
    const QString root = rootOf(path);
    if (root.isEmpty()) return nullptr;
    a_used.insert(root, nowMs());
    const auto it = a_repos.constFind(root);
    if (it == a_repos.cend()) {
        readSoon(root);
        return nullptr;
    }
    return it->valid ? &*it : nullptr;
}

const Entry* Tracker::entryOf(const QString& path)
{
    const Repository* repo = repositoryOf(path);
    if (repo == nullptr) return nullptr;
    bool inside = false;
    const QString rel = relativePath(repo->root, path, &inside);
    return inside && !rel.isEmpty() ? repo->entryOf(rel) : nullptr;
}

void Tracker::refresh(const QString& path)
{
    a_roots.clear();   // (a repository made, or taken away)
    if (path.isEmpty()) {
        for (auto it = a_repos.cbegin(); it != a_repos.cend(); ++it) readSoon(it.key());
        return;
    }
    const QString root = rootOf(path);
    if (!root.isEmpty()) readSoon(root);
}

const Repository* Tracker::readNow(const QString& path)
{
    a_roots.clear();
    const QString root = rootOf(path);
    if (root.isEmpty()) return nullptr;
    a_used.insert(root, nowMs());
    const qint64 started = nowMs();
    Repository repo = read(root);
    a_cost.insert(root, nowMs() - started);
    store(root, repo);
    const auto it = a_repos.constFind(root);
    return it != a_repos.cend() && it->valid ? &*it : nullptr;
}

const Repository* Tracker::fresh(const QString& path, int maxAgeMs)
{
    const QString root = rootOf(path);
    if (root.isEmpty()) return nullptr;
    const auto it = a_repos.constFind(root);
    if (it != a_repos.cend() && it->read.isValid() && it->read.msecsTo(QDateTime::currentDateTime()) < maxAgeMs
        && !a_waiting.contains(root) && !a_reading.contains(root)) {
        a_used.insert(root, nowMs());
        return it->valid ? &*it : nullptr;
    }
    return readNow(path);
}

void Tracker::readSoon(const QString& root)
{
    a_waiting.insert(root);
    if (!a_soon->isActive()) a_soon->start();
}

void Tracker::readNext()
{
    const QSet<QString> waiting = a_waiting;
    for (const QString& root : waiting) {
        if (a_reading.contains(root)) continue;   // (again once this read ends)
        a_waiting.remove(root);
        a_reading.insert(root);
        auto* watcher = new QFutureWatcher<Repository>(this);
        const qint64 started = nowMs();
        connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, root, started] {
            watcher->deleteLater();
            a_reading.remove(root);
            a_cost.insert(root, nowMs() - started);
            store(root, watcher->result());
            if (a_waiting.contains(root) && !a_soon->isActive()) a_soon->start();
        });
        watcher->setFuture(runAside<Repository>([root] { return read(root); }));
    }
}

namespace {

// Whether two reads of a repository found it as it was.
bool sameState(const Repository& a, const Repository& b)
{
    if (a.valid != b.valid || a.root != b.root || a.branch != b.branch || a.head != b.head || a.upstream != b.upstream
        || a.ahead != b.ahead || a.behind != b.behind || a.operation != b.operation || a.entries.size() != b.entries.size())
        return false;
    for (qsizetype i = 0; i < a.entries.size(); ++i) {
        const Entry& x = a.entries.at(i);
        const Entry& y = b.entries.at(i);
        if (x.path != y.path || x.from != y.from || x.staged != y.staged || x.unstaged != y.unstaged || x.untracked != y.untracked
            || x.ignored != y.ignored || x.conflicted != y.conflicted)
            return false;
    }
    return true;
}

} // namespace

void Tracker::store(const QString& root, const Repository& repo)
{
    const auto was = a_repos.constFind(root);
    const bool same = was != a_repos.cend() && sameState(*was, repo);
    a_repos.insert(root, repo);
    if (repo.valid) watch(repo);
    // (Read again by the clock, as it was: nothing drawn again.)
    if (!same) emit changed(root);
}

void Tracker::watch(const Repository& repo)
{
    // (Written anew - git replaces the index - a file's watch ends: set again.)
    QStringList paths;
    for (const QString& name : {QStringLiteral("HEAD"), QStringLiteral("index")}) {
        const QString p = repo.gitDir + QLatin1Char('/') + name;
        if (QFileInfo::exists(p) && !a_watcher->files().contains(p)) paths << p;
    }
    for (const QString& d : {repo.gitDir, repo.gitDir + QStringLiteral("/refs/heads")})
        if (QFileInfo(d).isDir() && !a_watcher->directories().contains(d)) paths << d;
    if (!paths.isEmpty()) a_watcher->addPaths(paths);
}

// ----------------------------------------------------------------------
// Job

Job::Job(const QString& dir, const QStringList& args, QObject* parent, const QByteArray& input)
    : QObject(parent), a_dir(dir), a_args(args), a_input(input)
{
}

Job::~Job()
{
    if (a_process != nullptr) {
        a_process->disconnect(this);
        a_process->kill();
        a_process->waitForFinished(2000);
        delete a_process;
    }
}

bool Job::isRunning() const
{
    return a_process != nullptr && a_process->state() != QProcess::NotRunning;
}

void Job::start()
{
    const QString git = program();
    if (git.isEmpty()) {
        a_result.err = tr("git is not installed");
        QTimer::singleShot(0, this, [this] { emit finished(a_result); });
        return;
    }
    a_process = new QProcess;
    a_process->setProcessEnvironment(environment());
    a_process->setWorkingDirectory(a_dir);
    connect(a_process, &QProcess::readyReadStandardError, this, [this] {
        const QByteArray chunk = a_process->readAllStandardError();
        a_err += chunk;
        const QStringList lines = QString::fromUtf8(chunk).split(QRegularExpression(QStringLiteral("[\r\n]")), Qt::SkipEmptyParts);
        if (!lines.isEmpty()) emit progress(lines.last().trimmed());
    });
    connect(a_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        a_result.exitCode = a_cancelled || status != QProcess::NormalExit ? -1 : code;
        a_result.out = QString::fromUtf8(a_process->readAllStandardOutput());
        a_err += a_process->readAllStandardError();
        a_result.err = QString::fromUtf8(a_err);
        if (a_cancelled) a_result.err += QLatin1Char('\n') + tr("Stopped.");
        a_process->deleteLater();
        a_process = nullptr;
        emit finished(a_result);
    });
    connect(a_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        a_result.err = tr("git could not be started");
        a_process->deleteLater();
        a_process = nullptr;
        emit finished(a_result);
    });
    a_process->start(git, a_args);
    if (!a_input.isEmpty()) a_process->write(a_input);
    a_process->closeWriteChannel();
}

void Job::cancel()
{
    if (!isRunning()) return;
    a_cancelled = true;
    a_process->kill();
}

} // namespace qucs_s::git
