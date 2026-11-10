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

// Lines of git's output.
QStringList linesOf(const QString& text)
{
    return text.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts);
}

// (To Qt a path that begins with ':' is a resource's, so absolute: a file's
// name here.)
bool isAbsolutePath(const QString& p)
{
    return !p.startsWith(QLatin1Char(':')) && QFileInfo(p).isAbsolute();
}

// \a paths from the top folder, for after "--".
QStringList relative(const QString& root, const QStringList& paths)
{
    QStringList out;
    for (const QString& p : paths) {
        bool inside = false;
        QString rel = isAbsolutePath(p) ? relativePath(root, p, &inside) : QDir::cleanPath(p);
        if (isAbsolutePath(p) && !inside) continue;
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
    args << QStringLiteral("--") << literalPaths(paths);
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

QStringList literalPaths(const QStringList& paths)
{
    QStringList out;
    for (const QString& p : paths) out << (p == QLatin1String(".") ? p : QStringLiteral(":(literal)") + p);
    return out;
}

QString Result::error() const
{
    // (What we write before a path, in what git says back of it.)
    const QString err = QString(this->err).remove(QStringLiteral(":(literal)"));
    const QString out = QString(this->out).remove(QStringLiteral(":(literal)"));
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
    bool goesOn = false;   // (a "fatal:" of several lines, whole: the rest says what to do)
    for (const QString& l : linesOf(err)) {
        const QString t = l.trimmed();
        if (t.startsWith(QLatin1String("fatal:")) || t.startsWith(QLatin1String("error:")) || t.startsWith(QLatin1String("CONFLICT"))
            || t.startsWith(QLatin1String("Automatic merge failed")) || t.startsWith(QLatin1String("! ["))) {
            said << t;
            goesOn = t.startsWith(QLatin1String("fatal:"));
        } else if (goesOn && !t.startsWith(QLatin1String("hint:"))) {
            said << t;
        } else {
            goesOn = false;
        }
    }
    for (const QString& l : linesOf(out)) {
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
    plainEnvironment(env);   // (Qucs-S started from a git hook; paths taken as they are)
    return env;
}

namespace {

// A git stopped: asked first, so that it lets its locks go and takes back a
// folder half made (SIGKILL leaves index.lock, and every git after it fails
// until someone removes it); then at once.
constexpr int kAskedToStopMs = 1500;

void stop(QProcess& p)
{
#ifndef Q_OS_WIN
    p.terminate();   // (Windows' would only ask a window to close)
    if (p.waitForFinished(kAskedToStopMs)) return;
#endif
    p.kill();
    p.waitForFinished(2000);
}

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
        lookingEnvironment(env);
        const QString root = topLevel(dir);
        if (!applyGuards(readOnlyGuards(root.isEmpty() ? dir : root), all, env)) {
            r.err = tr("git was not run: it could not be asked in time which filters this repository's own configuration "
                       "sets, and looking could have started one");
            return r;
        }
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
        stop(p);
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
        // (A folder named .git with no HEAD is no repository: git goes on up,
        // and its paths would not be from this folder.)
        const QFileInfo dotGit(dir + QStringLiteral("/.git"));
        if (dotGit.exists()
            && (!dotGit.isDir()
                || (QFileInfo::exists(dotGit.filePath() + QStringLiteral("/HEAD")) && QFileInfo::exists(dotGit.filePath() + QStringLiteral("/objects"))
                    && QFileInfo::exists(dotGit.filePath() + QStringLiteral("/refs")))))
            return dir;
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

namespace {

// A path as the lookups have it. On macOS one name has two spellings, its
// accents composed or not: the file system takes either, Qt gives one, and
// git the other where core.precomposeunicode is off.
QString lookupForm(const QString& path)
{
#ifdef Q_OS_MACOS
    return path.normalized(QString::NormalizationForm_C);
#else
    return path;
#endif
}

} // namespace

void Repository::index()
{
    a_byPath.clear();
    a_changedFolders.clear();
    for (int i = 0; i < entries.size(); ++i) {
        const Entry& e = entries.at(i);
        const QString path = lookupForm(e.path);
        a_byPath.insert(path, i);
        if (e.ignored) continue;
        QString folder = e.isFolder() ? path.chopped(1) : path.section(QLatin1Char('/'), 0, -2);
        if (!e.isFolder() && !path.contains(QLatin1Char('/'))) folder.clear();
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
    QString p = lookupForm(path);
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
    QString p = lookupForm(path);
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

void readStatus(const QString& porcelain, Repository& r)
{
    const QStringList tokens = porcelain.split(QLatin1Char('\0'));
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
    r.index();
}

namespace {

// The git folder of the work tree \a top, read from what is there: a .git
// folder, or a .git file (a linked worktree's, a submodule's) that says where.
// A program started for it was half of every read's programs. A link, or
// anything odd: git says.
QString gitFolderOf(const QString& top)
{
    const QFileInfo dotGit(fileIn(top, QStringLiteral(".git")));
    if (!dotGit.isSymLink()) {
        if (dotGit.isDir()) return QDir::cleanPath(dotGit.filePath());
        QFile file(dotGit.filePath());
        if (dotGit.isFile() && dotGit.size() < 8192 && file.open(QIODevice::ReadOnly)) {
            const QString text = QString::fromUtf8(file.readAll()).trimmed();
            if (text.startsWith(QLatin1String("gitdir: ")) && !text.contains(QLatin1Char('\n'))) {
                const QString where = text.mid(8);
                return QDir::cleanPath(QFileInfo(where).isAbsolute() ? where : fileIn(top, where));
            }
        }
    }
    const Result asked = run(top, {QStringLiteral("rev-parse"), QStringLiteral("--absolute-git-dir")}, true, 15000);
    return asked.ok() ? QDir::cleanPath(QDir::fromNativeSeparators(asked.out.trimmed())) : QString();
}

} // namespace

Repository read(const QString& path)
{
    Repository r;
    const QString top = topLevel(path);
    if (top.isEmpty() || program().isEmpty()) return r;
    r.gitDir = gitFolderOf(top);
    if (r.gitDir.isEmpty()) return r;
    r.root = top;
    // (A submodule's insides not looked at: a look there would run what its
    // own configuration names, which the guards here do not reach.)
    QStringList args{QStringLiteral("status"), QStringLiteral("--porcelain=v2"), QStringLiteral("-z"), QStringLiteral("--branch"),
                     QStringLiteral("--untracked-files=normal"), QStringLiteral("--ignore-submodules=dirty")};
    Result st = run(top, args + QStringList{QStringLiteral("--ignored=matching")}, true, 30000);
    // (A git before --ignored=matching says so: asked again. One stopped at its
    // limit, by a pipe named .gitignore, would only wait as long again.)
    if (!st.ok() && st.exitCode != -1) st = run(top, args, true, 30000);
    if (!st.ok()) return r;
    r.valid = true;
    r.read = QDateTime::currentDateTime();
    readStatus(st.out, r);
    const QDir git(r.gitDir);
    // (A rebase first: stopped at a merge it has MERGE_HEAD too, stopped at a
    // pick CHERRY_PICK_HEAD. rebase-apply is git am's as well, and rebase
    // --abort refuses that.)
    if (git.exists(QStringLiteral("rebase-apply/applying"))) r.operation = QStringLiteral("am");
    else if (git.exists(QStringLiteral("rebase-merge")) || git.exists(QStringLiteral("rebase-apply"))) r.operation = QStringLiteral("rebase");
    else if (git.exists(QStringLiteral("MERGE_HEAD"))) r.operation = QStringLiteral("merge");
    else if (git.exists(QStringLiteral("CHERRY_PICK_HEAD"))) r.operation = QStringLiteral("cherry-pick");
    else if (git.exists(QStringLiteral("REVERT_HEAD"))) r.operation = QStringLiteral("revert");
    else if (git.exists(QStringLiteral("BISECT_LOG"))) r.operation = QStringLiteral("bisect");
    return r;
}

// ----------------------------------------------------------------------
// What is done with it

namespace {

// Whether the last commit holds \a rel.
bool inHead(const QString& root, const QString& rel)
{
    return run(root, {QStringLiteral("cat-file"), QStringLiteral("-e"), QStringLiteral("HEAD:") + rel}, true, 15000).ok();
}

// git add -A of \a rel, but for a file gone from the disk and the index and
// in the last commit: its deletion is staged already, and git would say it
// matches nothing. A name that is nowhere goes to git, which says so.
Result addAll(const QString& root, const QStringList& rel)
{
    QStringList some;
    for (const QString& p : rel) {
        const QFileInfo info(fileIn(root, p));
        if (info.exists() || info.isSymLink() || p == QLatin1String(".")) {
            some << p;
            continue;
        }
        const Result known = run(root, {QStringLiteral("ls-files"), QStringLiteral("--"), p}, true, 15000);
        if (!known.ok() || !known.out.trimmed().isEmpty() || !inHead(root, p)) some << p;
    }
    if (some.isEmpty()) {
        Result nothing;
        nothing.exitCode = 0;
        return nothing;
    }
    return run(root, withPaths({QStringLiteral("add"), QStringLiteral("-A")}, some));
}

} // namespace

Result stage(const QString& root, const QStringList& paths)
{
    if (paths.isEmpty()) return run(root, {QStringLiteral("add"), QStringLiteral("-A")});
    const QStringList rel = relative(root, paths);
    if (rel.isEmpty()) return outside();
    return addAll(root, rel);
}

QStringList withRenameSources(const QString& root, const QStringList& paths)
{
    const auto named = [&paths](const QString& path) {
        const QString form = lookupForm(path);
        for (const QString& p : paths) {
            const QString given = lookupForm(p);
            if (form == given || form.startsWith(given + QLatin1Char('/'))) return true;
        }
        return false;
    };
    QStringList out = paths;
    for (const Entry& e : read(root).entries) {
        if (e.staged != QLatin1Char('R') || e.from.isEmpty()) continue;
        if (named(e.from) && !out.contains(e.path)) out << e.path;
        if (named(e.path) && !out.contains(e.from)) {
            const QFileInfo former(fileIn(root, e.from));
            if (!former.exists() && !former.isSymLink()) out << e.from;
        }
    }
    return out;
}

Result unstage(const QString& root, const QStringList& paths)
{
    QStringList rel = paths.isEmpty() ? QStringList{QStringLiteral(".")} : relative(root, paths);
    if (rel.isEmpty()) return outside();
    for (QString& p : rel) p = spelledOnDisk(root, p);
    const bool head = hasHead(root);
    // A name that is no file: said. (git resets a path it does not know in silence.)
    if (!paths.isEmpty()) {
        for (const QString& p : std::as_const(rel)) {
            if (p == QLatin1String(".") || (head && inHead(root, p))) continue;
            const Result known = run(root, {QStringLiteral("ls-files"), QStringLiteral("--"), p}, true, 15000);
            if (known.ok() && known.out.trimmed().isEmpty())
                return failure(QStringLiteral("error: pathspec '%1' did not match any file(s) known to git").arg(p));
        }
        rel = withRenameSources(root, rel);
    }
    // (Before the first commit: nothing to go back to. -f: edited since it was
    // staged, git would refuse - and the file on the disk stays. --ignore-unmatch:
    // nothing staged at all is no fault.)
    if (!head)
        return run(root, withPaths({QStringLiteral("rm"), QStringLiteral("--cached"), QStringLiteral("-r"), QStringLiteral("-q"), QStringLiteral("-f"),
                                    QStringLiteral("--ignore-unmatch")}, rel));
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
    QStringList rel = paths.isEmpty() ? QStringList{QString()} : relative(root, paths);
    if (rel.isEmpty()) return outside();
    for (QString& p : rel) p = spelledOnDisk(root, p);
    QSet<QString> taken;   // the names given that took in an entry
    for (const Entry& e : repo.entries) {
        if (e.ignored) continue;
        bool in = false;
        for (const QString& given : rel) {
            // (Compared in one spelling of their accents: a name that matches
            // none would be left as it is, and said done.)
            const QString p = given == QLatin1String(".") ? QString() : lookupForm(given);
            const QString path = lookupForm(e.path);
            const QString bare = e.isFolder() ? path.chopped(1) : path;
            // (A name inside an untracked folder is not known from the folder's
            // entry: it has to be there. A rename is also its former name.)
            const bool within = e.isFolder() && p.startsWith(path);
            if (p.isEmpty() || bare == p || bare.startsWith(p + QLatin1Char('/')) || within || (!e.from.isEmpty() && lookupForm(e.from) == p)) {
                if (!within || QFileInfo::exists(fileIn(root, given))) taken.insert(given);
                in = true;
            }
        }
        if (in) chosen << e;
    }
    // A name that is no file - not changed, not on the disk, not committed: a
    // slip, said, as git would, before anything is thrown away. (A file as
    // committed has nothing to discard, and that is no fault.)
    if (!paths.isEmpty()) {
        for (const QString& p : rel) {
            const QFileInfo info(fileIn(root, p));
            if (p == QLatin1String(".") || taken.contains(p) || info.exists() || info.isSymLink() || (head && inHead(root, p))) continue;
            return failure(QStringLiteral("error: pathspec '%1' did not match any file(s) known to git").arg(p));
        }
    }
    QStringList restore, forget, toTrash;
    for (const Entry& e : chosen) {
        if (e.untracked) {
            // In an untracked folder: the paths given (files in it), else the folder.
            QStringList targets;
            if (e.isFolder()) {
                // (The folder, or one around it, given too: all of it.)
                const QString folder = lookupForm(e.path);
                bool whole = false;
                for (const QString& p : rel) {
                    const QString given = lookupForm(p);
                    if (p.isEmpty() || p == QLatin1String(".") || folder.startsWith(given + QLatin1Char('/'))) whole = true;
                    else if (given.startsWith(folder)) targets << p;
                }
                if (whole || targets.isEmpty()) targets = {e.path.chopped(1)};
            } else {
                targets << e.path;
            }
            toTrash << targets;
        } else if (e.conflicted ? !inHead(root, e.path)   // (HEAD's alone decides: AA is in it, DU not)
                                : (!head || e.staged == QLatin1Char('A') || e.staged == QLatin1Char('C') || e.staged == QLatin1Char('R'))) {
            forget << e.path;   // not in HEAD: out of the index, then to the trash
            toTrash << e.path;
            if (e.staged == QLatin1Char('R') && !e.from.isEmpty()) restore << e.from;
        } else {
            restore << e.path;
        }
    }
    // (Out of the index with a file of that name on the disk - Stop Tracking,
    // Keep the File - it is two entries, deleted and untracked: it goes back
    // as HEAD has it, and that is not then taken to the trash.)
    for (const QString& p : std::as_const(restore)) toTrash.removeAll(p);
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
        const QString path = fileIn(root, p);
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
        const QStringList rel = withRenameSources(root, relative(root, only));
        if (rel.isEmpty()) return outside();
        const Result added = addAll(root, rel);
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
    QString rel = isAbsolutePath(path) ? relativePath(root, path, &inside) : QDir::cleanPath(path);
    if ((isAbsolutePath(path) && !inside) || rel.isEmpty() || rel == QLatin1String(".") || rel == QLatin1String("..")
        || rel.startsWith(QLatin1String("../"))) {
        if (why != nullptr) *why = tr("it is not a file of the repository");
        return false;
    }
    // (A line in the name would be a line of .gitignore of its own, and that
    // line a pattern: "x\n*" everything. git has no way to write one.)
    if (rel.contains(QLatin1Char('\n')) || rel.contains(QLatin1Char('\r'))) {
        if (why != nullptr) *why = tr(".gitignore cannot name a file with a line break in its name");
        return false;
    }
    // A pattern for it alone: from the top folder, its special characters
    // taken as they are, and spaces at its end kept (git drops them, unless
    // written so).
    // (A link to a folder is no folder to git: a pattern with "/" would not take it.)
    const QFileInfo what(fileIn(root, rel));
    const bool folder = what.isDir() && !what.isSymLink();
    const auto patternOf = [folder](const QString& name) {
        qsizetype textEnd = name.size();
        while (textEnd > 0 && name.at(textEnd - 1) == QLatin1Char(' ')) --textEnd;
        QString pattern(QLatin1Char('/'));
        for (qsizetype i = 0; i < name.size(); ++i) {
            const QChar c = name.at(i);
            if (QStringLiteral("*?[]!#\\").contains(c) || i >= textEnd) pattern += QLatin1Char('\\');
            pattern += c;
        }
        return folder ? pattern + QLatin1Char('/') : pattern;
    };
    QStringList patterns{patternOf(rel)};
#ifdef Q_OS_MACOS
    // (git composes a name's accents as it reads the folder - but not one with
    // a character beyond 16 bits, an emoji: either spelling can be the one it
    // compares, and a pattern matches only its own.)
    patterns = {patternOf(rel.normalized(QString::NormalizationForm_C))};
    if (const QString other = patternOf(rel.normalized(QString::NormalizationForm_D)); other != patterns.first()) patterns << other;
#endif
    // (A pipe of that name would wait for ever. A link: written through it the
    // line would go outside the repository, and git does not read such a file.)
    const QFileInfo ignoreFile(fileIn(root, QStringLiteral(".gitignore")));
    if (ignoreFile.isSymLink() || (ignoreFile.exists() && !ignoreFile.isFile())) {
        if (why != nullptr) *why = tr(".gitignore here is not a plain file");
        return false;
    }
    QFile file(QDir(root).filePath(QStringLiteral(".gitignore")));
    QByteArray text;
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly)) {
            if (why != nullptr) *why = file.errorString();
            return false;
        }
        text = file.readAll();
        file.close();
        for (const QByteArray& l : text.split('\n')) {
            const QString line = QString::fromUtf8(l);
            patterns.removeAll(line);
            patterns.removeAll(line.trimmed());
        }
    }
    if (patterns.isEmpty()) return true;
    if (!file.open(QIODevice::Append)) {
        if (why != nullptr) *why = file.errorString();
        return false;
    }
    QByteArray add;
    if (!text.isEmpty() && !text.endsWith('\n')) add += '\n';
    add += patterns.join(QLatin1Char('\n')).toUtf8() + '\n';
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

QList<Branch> readBranches(const QString& text)
{
    QList<Branch> out;
    // (Each field ends in a NUL, which no commit holds: a subject may hold
    // anything else.)
    const QStringList f = text.split(QChar(0));
    for (qsizetype i = 0; i + 8 < f.size(); i += 8) {
        const QString refname = f.at(i).trimmed();
        if (refname.startsWith(QLatin1String("refs/remotes/")) && refname.endsWith(QLatin1String("/HEAD"))) continue;
        Branch b;
        b.remote = refname.startsWith(QLatin1String("refs/remotes/"));
        b.name = f.at(i + 1);
        b.commit = f.at(i + 2);
        b.upstream = f.at(i + 3);
        static const QRegularExpression ahead(QStringLiteral("ahead (\\d+)")), behind(QStringLiteral("behind (\\d+)"));
        if (const auto m = ahead.match(f.at(i + 4)); m.hasMatch()) b.ahead = m.captured(1).toInt();
        if (const auto m = behind.match(f.at(i + 4)); m.hasMatch()) b.behind = m.captured(1).toInt();
        b.current = f.at(i + 5).trimmed() == QLatin1String("*");
        b.date = QDateTime::fromString(f.at(i + 6), Qt::ISODate);
        b.subject = f.at(i + 7);
        out << b;
    }
    std::stable_sort(out.begin(), out.end(), [](const Branch& a, const Branch& b) { return !a.remote && b.remote; });
    return out;
}

QList<Branch> branches(const QString& root)
{
    const Result r = run(root,
                         {QStringLiteral("for-each-ref"),
                          QStringLiteral("--format=%(refname)%00%(refname:short)%00%(objectname:short)%00%(upstream:short)%00"
                                         "%(upstream:track,nobracket)%00%(HEAD)%00%(committerdate:iso-strict)%00%(contents:subject)%00"),
                          QStringLiteral("refs/heads"), QStringLiteral("refs/remotes")},
                         true, 15000);
    return r.ok() ? readBranches(r.out) : QList<Branch>();
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
        // (Past the remote's name, which may hold a '/' too.)
        QString remoteName = branch.section(QLatin1Char('/'), 0, 0);
        for (const Remote& r : remotes(root))
            if (r.name.size() > remoteName.size() && branch.startsWith(r.name + QLatin1Char('/'))) remoteName = r.name;
        const QString local = branch.mid(remoteName.size() + 1);
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

QStringList worktreeArgs(const QString& folder, const QString& branch, bool create, const QString& start, bool track)
{
    if (folder.isEmpty() || branch.isEmpty() || optionLike(branch) || optionLike(start)) return {};
    QStringList args{QStringLiteral("worktree"), QStringLiteral("add")};
    if (create && track) args << QStringLiteral("--track");
    if (create) args << QStringLiteral("-b") << branch;
    args << QStringLiteral("--") << folder;
    if (!create) args << branch;
    else if (!start.isEmpty()) args << start;
    return args;
}

QString worktreeFolder(const QString& root, const QString& branch, const QString& in)
{
    const QFileInfo top(QDir::cleanPath(root));
    QString leaf = branch.trimmed();
    leaf.replace(QLatin1Char('/'), QLatin1Char('-')).replace(QLatin1Char('\\'), QLatin1Char('-'));
    const QString name = leaf.isEmpty() ? top.fileName() : top.fileName() + QLatin1Char('-') + leaf;
    // (By hand, not QDir::filePath: a name beginning with ':' would be a resource.)
    QString beside = in.isEmpty() ? top.absolutePath() : QDir::cleanPath(in);
    if (!beside.endsWith(QLatin1Char('/'))) beside += QLatin1Char('/');
    QString target = beside + name;
    for (int n = 2; QFileInfo::exists(target) || QFileInfo(target).isSymLink(); ++n) target = beside + name + QLatin1Char('-') + QString::number(n);
    return target;
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
    return run(root, mergeArgs(ref), false, 300000);
}

QStringList mergeArgs(const QString& ref)
{
    if (optionLike(ref)) return {};
    return {QStringLiteral("merge"), QStringLiteral("--no-edit"), ref};
}

QString folderOfUrl(const QString& url)
{
    QString name = url;
    while (name.endsWith(QLatin1Char('/'))) name.chop(1);
    name = name.section(QLatin1Char('/'), -1).section(QLatin1Char(':'), -1);
    if (name.endsWith(QLatin1String(".git"))) name.chop(4);
    return name.isEmpty() ? QStringLiteral("repository") : name;
}

QString spelledOnDisk(const QString& root, const QString& rel)
{
    if (rel.isEmpty() || rel == QLatin1String(".")) return rel;
    const QFileInfo info(fileIn(root, rel));
    if (!info.exists() || info.isSymLink()) return rel;
    const QString found = relativePath(root, info.canonicalFilePath());
    return found.size() == rel.size() && found != rel && found.compare(rel, Qt::CaseInsensitive) == 0 ? found : rel;
}

Result abort(const QString& root, const QString& operation)
{
    if (operation == QLatin1String("bisect")) return run(root, {QStringLiteral("bisect"), QStringLiteral("reset")});
    if (operation == QLatin1String("merge") || operation == QLatin1String("rebase") || operation == QLatin1String("am")
        || operation == QLatin1String("cherry-pick") || operation == QLatin1String("revert"))
        return run(root, {operation, QStringLiteral("--abort")});
    Result r;
    r.exitCode = 1;
    r.err = tr("error: nothing is under way to abort");
    return r;
}

Result continueOperation(const QString& root, const QString& operation, bool skip)
{
    if (operation != QLatin1String("rebase") && operation != QLatin1String("am") && operation != QLatin1String("cherry-pick")
        && operation != QLatin1String("revert") && !(operation == QLatin1String("merge") && !skip))
        return failure(tr("error: there is nothing to go on with here"));
    return run(root, {operation, skip ? QStringLiteral("--skip") : QStringLiteral("--continue")}, false, 300000);
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

QList<Remote> readRemotes(const QString& text)
{
    QList<Remote> out;
    for (const QString& l : linesOf(text)) {
        // (A partial clone's fetch line ends in its filter: "(fetch) [blob:none]".)
        static const QRegularExpression line(QStringLiteral("^(\\S+)\\s+(.*)\\s+\\((fetch|push)\\)( \\[.*\\])?$"));
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

QList<Remote> remotes(const QString& root)
{
    const Result r = run(root, {QStringLiteral("remote"), QStringLiteral("-v")}, true, 15000);
    return r.ok() ? readRemotes(r.out) : QList<Remote>();
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

QStringList pullArgs(const QString& root)
{
    QStringList args{QStringLiteral("pull"), QStringLiteral("--no-edit"), QStringLiteral("--progress")};
    if (root.isEmpty()) return args;
    const auto isSet = [&root](const QString& key) { return run(root, {QStringLiteral("config"), QStringLiteral("--get"), key}, true, 15000).ok(); };
    const QString branch = run(root, {QStringLiteral("symbolic-ref"), QStringLiteral("--short"), QStringLiteral("-q"), QStringLiteral("HEAD")}, true, 15000).out.trimmed();
    if (!isSet(QStringLiteral("pull.rebase")) && !isSet(QStringLiteral("pull.ff"))
        && (branch.isEmpty() || !isSet(QStringLiteral("branch.%1.rebase").arg(branch))))
        args << QStringLiteral("--no-rebase");
    return args;
}

QStringList pushArgs(const Repository& repo, QString* why, bool tags)
{
    if (repo.branch.isEmpty()) {
        if (why != nullptr) *why = tr("HEAD is detached: there is no branch to push");
        return {};
    }
    // (Names from the repository: one that begins with '-' git would take for
    // an option, --force or --receive-pack=...)
    if (optionLike(repo.branch)) {
        if (why != nullptr) *why = tr("%1 is not a name git takes here (it begins with '-')").arg(repo.branch);
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
    if (optionLike(remote)) {
        if (why != nullptr) *why = tr("%1 is not a name git takes here (it begins with '-')").arg(remote);
        return {};
    }
    return args << QStringLiteral("-u") << remote << repo.branch;
}

QStringList cloneArgs(const QString& url, const QString& folder)
{
    return {QStringLiteral("clone"), QStringLiteral("--progress"), QStringLiteral("--"), url, folder};
}

// ----------------------------------------------------------------------
// Stashes

QList<Stash> readStashes(const QString& text)
{
    QList<Stash> out;
    const QStringList f = text.split(QChar(0));
    for (qsizetype i = 0; i + 3 < f.size(); i += 3) {
        static const QRegularExpression index(QStringLiteral("\\{(\\d+)\\}"));
        const auto m = index.match(f.at(i));
        if (!m.hasMatch()) continue;
        out << Stash{m.captured(1).toInt(), f.at(i + 1), QDateTime::fromString(f.at(i + 2), Qt::ISODate)};
    }
    return out;
}

QList<Stash> stashes(const QString& root)
{
    const Result r = run(root, {QStringLiteral("stash"), QStringLiteral("list"), QStringLiteral("--format=%gd%x00%gs%x00%cI%x00")}, true, 15000);
    return r.ok() ? readStashes(r.out) : QList<Stash>();
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
    // (New files too, which a stash holds by default: from git 2.32.)
    QStringList args{QStringLiteral("stash"), QStringLiteral("show"), QStringLiteral("-p"), QStringLiteral("--no-ext-diff"),
                     QStringLiteral("--no-textconv"), QStringLiteral("stash@{%1}").arg(index)};
    QStringList withNew = args;
    withNew.insert(3, QStringLiteral("--include-untracked"));
    Result r = run(root, withNew, true, 30000);
    if (!r.ok()) r = run(root, args, true, 30000);
    return r.ok() ? r.out : r.error();
}

// ----------------------------------------------------------------------
// History

namespace {

// A commit's fields, each ending in a NUL - the one character no commit can
// hold: a name or a subject written by hand may hold any other, and would
// make a row of its own -, the refs' names whole (refs/heads/...,
// refs/remotes/...: a branch "feature/x" is not a remote's).
const QString kCommitFormat = QStringLiteral("--format=%H%x00%h%x00%an%x00%ae%x00%aI%x00%s%x00%D%x00%P%x00%cn%x00%ce%x00%cI%x00");
constexpr int kCommitFields = 11;

bool isHash(const QString& s)
{
    if (s.size() != 40 && s.size() != 64) return false;
    return std::all_of(s.cbegin(), s.cend(), [](QChar c) { return (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c >= QLatin1Char('a') && c <= QLatin1Char('f')); });
}

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

} // namespace

QList<Commit> readCommits(const QString& text)
{
    QList<Commit> out;
    const QStringList f = text.split(QChar(0));
    for (qsizetype i = 0; i + kCommitFields < f.size(); i += kCommitFields) {
        Commit c;
        // (A line between one commit's end and the next.)
        c.hash = f.at(i).trimmed();
        if (!isHash(c.hash)) continue;
        c.shortHash = f.at(i + 1);
        c.author = f.at(i + 2);
        c.email = f.at(i + 3);
        c.date = QDateTime::fromString(f.at(i + 4), Qt::ISODate);
        c.subject = f.at(i + 5);
        readRefs(f.at(i + 6), c);
        for (const QString& p : f.at(i + 7).split(QLatin1Char(' '), Qt::SkipEmptyParts))
            if (isHash(p)) c.parents << p;
        c.committer = f.at(i + 8);
        c.committerEmail = f.at(i + 9);
        c.committed = QDateTime::fromString(f.at(i + 10), Qt::ISODate);
        out << c;
    }
    return out;
}

QList<Commit> log(const QString& root, const QString& path, int max, const QString& ref, int skip)
{
    QList<Commit> out;
    QStringList args{QStringLiteral("log"), QStringLiteral("--decorate=full"), kCommitFormat, QStringLiteral("-n"),
                     QString::number(std::max(1, max))};
    if (skip > 0) args << QStringLiteral("--skip=%1").arg(skip);
    if (optionLike(ref)) return out;
    if (!ref.isEmpty()) args << ref;
    QStringList limit{QStringLiteral("--")};   // (a file named as the ref: the ref, not "both")
    if (!path.isEmpty()) {
        const QStringList rel = relative(root, {path});
        if (rel.isEmpty()) return out;
        // The top folder is the whole repository: no path (with one, git
        // drops a merge whose tree is one side's - a conflict resolved
        // with theirs).
        if (rel.first() != QLatin1String(".")) {
            if (!QFileInfo(fileIn(root, rel.first())).isDir()) args << QStringLiteral("--follow");
            limit << rel;
        }
    }
    args << limit;
    const Result r = run(root, args, true, 30000);
    return r.ok() ? readCommits(r.out) : out;
}

QList<Commit> history(const QString& root, const HistoryQuery& query)
{
    // (--ignore-missing: on a branch with no commit yet, HEAD is no name, and
    // the other branches' commits are still there.)
    QStringList args{QStringLiteral("log"), QStringLiteral("--decorate=full"), kCommitFormat, QStringLiteral("--date-order"),
                     QStringLiteral("--ignore-missing"), QStringLiteral("-n"), QString::number(std::max(1, query.max))};
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
    QStringList limit{QStringLiteral("--")};
    if (!query.path.isEmpty()) {
        const QStringList rel = relative(root, {query.path});
        if (rel.isEmpty()) return {};
        if (rel.first() != QLatin1String(".")) {
            if (!QFileInfo(fileIn(root, rel.first())).isDir()) args << QStringLiteral("--follow");
            limit << rel;
        }
    }
    args << limit;
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

// What a commit changed, once asked: the History, on a commit chosen, asks
// for its files, a schematic's parts and its diff, and a commit by its full
// hash never changes.
struct CommitMemory {
    QMutex mutex;
    QHash<QString, QList<ChangedFile>> files;
};

CommitMemory& commitMemory()
{
    static CommitMemory memory;
    return memory;
}

} // namespace

QList<ChangedFile> readChangedFiles(const QString& namesOut, const QString& countsOut)
{
    QList<ChangedFile> out;
    QHash<QString, int> byPath;
    const QStringList n = namesOut.split(QChar(0), Qt::KeepEmptyParts);
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
    // The lines: "added<TAB>removed<TAB>path" (the path may hold a tab), a
    // rename's names after it.
    const QStringList c = countsOut.split(QChar(0), Qt::KeepEmptyParts);
    for (int i = 0; i < c.size(); ++i) {
        const qsizetype tab1 = c.at(i).indexOf(QLatin1Char('\t'));
        const qsizetype tab2 = tab1 < 0 ? -1 : c.at(i).indexOf(QLatin1Char('\t'), tab1 + 1);
        if (tab2 < 0) continue;
        QString path = c.at(i).mid(tab2 + 1);
        const QString addedText = c.at(i).left(tab1), removedText = c.at(i).mid(tab1 + 1, tab2 - tab1 - 1);
        if (path.isEmpty() && i + 2 < c.size()) {   // a rename: its names follow
            path = c.at(i + 2);
            i += 2;
        }
        const int at = byPath.value(path, -1);
        if (at < 0) continue;
        bool okA = false, okR = false;
        const int added = addedText.toInt(&okA), removed = removedText.toInt(&okR);
        out[at].added = okA ? added : -1;
        out[at].removed = okR ? removed : -1;
    }
    return out;
}

QList<ChangedFile> changedFiles(const QString& root, const QString& commit)
{
    if (commit.isEmpty() || optionLike(commit)) return {};
    const bool lasting = isHash(commit);
    const QString key = root + QLatin1Char('\n') + commit;
    if (lasting) {
        CommitMemory& memory = commitMemory();
        const QMutexLocker lock(&memory.mutex);
        if (const auto it = memory.files.constFind(key); it != memory.files.cend()) return *it;
    }
    // NUL between the names: any name read whole.
    const QStringList trees = againstFirstParent(root, commit);
    const QStringList common{QStringLiteral("diff-tree"), QStringLiteral("-r"), QStringLiteral("-M"), QStringLiteral("--no-commit-id"),
                             QStringLiteral("-z")};
    const Result names = run(root, common + QStringList{QStringLiteral("--name-status")} + trees, true, 30000);
    if (!names.ok()) return {};
    const Result counts = run(root, common + QStringList{QStringLiteral("--numstat")} + trees, true, 30000);
    const QList<ChangedFile> found = readChangedFiles(names.out, counts.out);
    if (lasting && counts.ok()) {
        CommitMemory& memory = commitMemory();
        const QMutexLocker lock(&memory.mutex);
        if (memory.files.size() >= 256) memory.files.clear();
        memory.files.insert(key, found);
    }
    return found;
}

QString commitMessage(const QString& root, const QString& commit)
{
    if (commit.isEmpty() || optionLike(commit)) return {};
    const Result r = run(root, {QStringLiteral("show"), QStringLiteral("-s"), QStringLiteral("--format=%B"), commit, QStringLiteral("--")}, true, 15000);
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

QList<std::optional<QByteArray>> readBlobs(const QByteArray& out, int count)
{
    // "<hash> <type> <size>\n<its bytes>\n", or "<name> missing\n".
    QList<std::optional<QByteArray>> found;
    qsizetype at = 0;
    while (found.size() < count) {
        const qsizetype end = out.indexOf('\n', at);
        if (end < 0) break;
        const QList<QByteArray> head = out.mid(at, end - at).split(' ');
        at = end + 1;
        bool sized = false;
        const qlonglong size = head.size() >= 3 ? head.last().toLongLong(&sized) : 0;
        if (!sized || size < 0 || size > out.size() - at) {
            found << std::nullopt;   // (a line of git's that names nothing)
            continue;
        }
        found << (head.at(1) == "blob" ? std::optional<QByteArray>(out.mid(at, size)) : std::nullopt);
        at += size + 1;
    }
    return found;
}

QList<std::optional<QByteArray>> blobs(const QString& root, const QStringList& specs)
{
    if (specs.isEmpty()) return {};
    // (One name a line: one with a line in it asked apart.)
    QByteArray input;
    QList<int> together;
    for (int i = 0; i < specs.size(); ++i)
        if (!specs.at(i).contains(QLatin1Char('\n'))) {
            input += specs.at(i).toUtf8() + '\n';
            together << i;
        }
    QList<std::optional<QByteArray>> out(specs.size());
    if (!input.isEmpty()) {
        const Raw r = runRaw(root, {QStringLiteral("cat-file"), QStringLiteral("--batch")}, true, 60000, input);
        if (r.exitCode != 0) return {};
        const QList<std::optional<QByteArray>> read = readBlobs(r.out, int(together.size()));
        for (qsizetype k = 0; k < read.size(); ++k) out[together.at(k)] = read.at(k);
    }
    for (int i = 0; i < specs.size(); ++i)
        if (specs.at(i).contains(QLatin1Char('\n'))) {
            const Raw r = runRaw(root, {QStringLiteral("cat-file"), QStringLiteral("blob"), specs.at(i)}, true, 30000);
            if (r.exitCode == 0) out[i] = r.out;
        }
    return out;
}

QString show(const QString& root, const QString& commit, const QString& path)
{
    if (optionLike(commit)) return optionRefused(commit).error();
    QStringList args{QStringLiteral("show"), QStringLiteral("--stat"), QStringLiteral("--patch"), QStringLiteral("--format=fuller"),
                     QStringLiteral("--no-ext-diff"), QStringLiteral("--no-textconv"), commit};
    // (The top folder is the whole repository: no path, as in log().)
    const QStringList rel = path.isEmpty() ? QStringList() : relative(root, {path});
    const bool limited = !rel.isEmpty() && rel.first() != QLatin1String(".");
    args << QStringLiteral("--");   // (a file named as the commit: the commit, not "both")
    if (limited) args << rel;
    const Result r = run(root, args, true, 30000);
    if (!r.ok()) return r.error();
    // A commit that changed nothing there: git says nothing at all with a path
    // given. Who made it and why still come back.
    if (limited && r.out.trimmed().isEmpty()) {
        const Result alone = run(root, {QStringLiteral("show"), QStringLiteral("-s"), QStringLiteral("--format=fuller"), commit, QStringLiteral("--")}, true, 30000);
        return alone.ok() ? alone.out : alone.error();
    }
    return r.out;
}

namespace {

// An untracked file as git's diff shows it, all added - read here: a git
// program for each cost 200 of them on a tree of many new files, and the
// History's row of the changes not committed waited seconds. A link is its
// own line; what it points at is not read.
QString newFileDiff(const QString& root, const QString& rel)
{
    constexpr qint64 kShownSize = qint64(32) << 20;
    const QFileInfo info(fileIn(root, rel));
    QString head = QStringLiteral("diff --git a/%1 b/%1\n").arg(rel);
    if (info.isSymLink())
        return head + QStringLiteral("new file mode 120000\n--- /dev/null\n+++ b/%1\n@@ -0,0 +1 @@\n+%2\n\\ No newline at end of file\n").arg(rel, info.symLinkTarget());
    if (!info.isFile()) return {};
    head += QStringLiteral("new file mode %1\n").arg(info.isExecutable() ? QStringLiteral("100755") : QStringLiteral("100644"));
    QFile file(info.filePath());
    if (info.size() > kShownSize || !file.open(QIODevice::ReadOnly)) return head + QStringLiteral("Binary files /dev/null and b/%1 differ\n").arg(rel);
    const QByteArray bytes = file.readAll();
    if (bytes.isEmpty()) return head;
    // (As git tells a binary file.)
    if (bytes.left(8000).contains('\0')) return head + QStringLiteral("Binary files /dev/null and b/%1 differ\n").arg(rel);
    const QString body = QString::fromUtf8(bytes);
    QString text = head + QStringLiteral("--- /dev/null\n+++ b/%1\n").arg(rel);
    const bool lastHasEnd = body.endsWith(QLatin1Char('\n'));
    QStringList lines = body.split(QLatin1Char('\n'));
    if (lastHasEnd) lines.removeLast();
    text += lines.size() == 1 ? QStringLiteral("@@ -0,0 +1 @@\n") : QStringLiteral("@@ -0,0 +1,%1 @@\n").arg(lines.size());
    text.reserve(text.size() + body.size() + lines.size() + 40);
    for (const QString& l : std::as_const(lines)) {
        text += QLatin1Char('+');
        text += l;
        text += QLatin1Char('\n');
    }
    if (!lastHasEnd) text += QStringLiteral("\\ No newline at end of file\n");
    return text;
}

} // namespace

QString diff(const QString& root, const QString& path, DiffOf of)
{
    const QStringList common{QStringLiteral("--no-ext-diff"), QStringLiteral("--no-textconv"), QStringLiteral("--no-color")};
    const QStringList rel = path.isEmpty() ? QStringList() : relative(root, {path});
    if (!path.isEmpty() && rel.isEmpty()) return {};
    // The untracked files in it, each all added. (-z: each name as it is.
    // Without it a name with an accent, a quote, a tab comes back quoted, and
    // no such file is found.) git's lines need no status: a click on a file
    // waited for a whole one, its walk of the tree.
    QStringList untracked;
    if (of != DiffOf::Staged) {
        QStringList others{QStringLiteral("ls-files"), QStringLiteral("--others"), QStringLiteral("--exclude-standard"), QStringLiteral("-z")};
        if (!rel.isEmpty()) others << QStringLiteral("--") << rel;
        untracked = run(root, others, true, 30000).out.split(QChar(0), Qt::SkipEmptyParts);
        // A file that is one: nothing else to ask.
        if (!rel.isEmpty() && untracked.size() == 1 && QDir::cleanPath(untracked.first()) == QDir::cleanPath(rel.first())) return newFileDiff(root, untracked.first());
    }
    QStringList args{QStringLiteral("diff")};
    args << common;
    const bool head = of != DiffOf::Head || hasHead(root);
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
    constexpr int kNewFilesShown = 200;
    for (int n = 0; n < untracked.size() && n < kNewFilesShown; ++n) text += newFileDiff(root, untracked.at(n));
    if (untracked.size() > kNewFilesShown) text += tr("(%1 more new files are not shown.)\n").arg(untracked.size() - kNewFilesShown);
    return text;
}

QList<BlameLine> readBlame(const QString& porcelain)
{
    QList<BlameLine> out;
    struct Info {
        QString author, summary;
        QDateTime date;
    };
    QHash<QString, Info> infos;
    BlameLine current;
    QString hash;
    for (const QString& l : porcelain.split(QLatin1Char('\n'))) {
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

QList<BlameLine> blame(const QString& root, const QString& path)
{
    const QStringList rel = relative(root, {path});
    if (rel.isEmpty()) return {};
    const Result r = run(root, {QStringLiteral("blame"), QStringLiteral("--porcelain"), QStringLiteral("--no-textconv"),
                                QStringLiteral("--"), rel.first()},
                         true, 60000);
    return r.ok() ? readBlame(r.out) : QList<BlameLine>();
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
    return run(root, {QStringLiteral("reset"), QStringLiteral("--") + mode, commit, QStringLiteral("--")});
}

// ----------------------------------------------------------------------
// Conflicts

bool hasConflictMarkers(const QString& path)
{
    // (A file: a pipe, or a link to a device, would never end.)
    const QFileInfo info(path);
    if (!info.isFile() || info.size() > (qint64(64) << 20)) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
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
    // Every file in conflict, and that one found among them as it is spelt: a
    // path given to git would have to match git's own spelling of its accents.
    const Result stages = run(root, {QStringLiteral("ls-files"), QStringLiteral("-u"), QStringLiteral("-z")}, true, 15000);
    if (!stages.ok()) return v;
    const QString wanted = lookupForm(spelledOnDisk(root, rel.first()));
    // "mode hash stage<TAB>path": 1 the base, 2 ours, 3 theirs.
    for (const QString& l : stages.out.split(QChar(0), Qt::SkipEmptyParts)) {
        const QString name = l.section(QLatin1Char('\t'), 1);
        if (lookupForm(name) != wanted) continue;
        const int stage = l.section(QLatin1Char('\t'), 0, 0).section(QLatin1Char(' '), 2, 2).toInt();
        if (stage < 1 || stage > 3) continue;
        v.inConflict = true;
        v.name = name;
        const Result text = run(root, {QStringLiteral("show"), QStringLiteral(":%1:%2").arg(stage).arg(name)}, true, 30000);
        if (!text.ok()) {
            v.incomplete = true;
            continue;
        }
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
    const QStringList held{v.name};
    // (A side not read is not a side that deleted it: taken so, the file would go.)
    if (v.incomplete) return failure(tr("error: git could not read the versions of %1").arg(rel.first()));
    // That side deleted it: deleted.
    if (!(side == QLatin1String("ours") ? v.ours : v.theirs).has_value())
        return run(root, withPaths({QStringLiteral("rm"), QStringLiteral("-q"), QStringLiteral("--ignore-unmatch")}, held));
    const Result taken = run(root, withPaths({QStringLiteral("checkout"), QStringLiteral("--") + side}, held));
    if (!taken.ok()) return taken;
    return run(root, withPaths({QStringLiteral("add"), QStringLiteral("-f")}, held));
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
    a_readDirect.insert(root, ++a_turn);
    // (What asked for a read before this one has its answer: another would be
    // the same status, and fresh() would not trust this one until it ended.)
    a_waiting.remove(root);
    store(root, repo);
    const auto it = a_repos.constFind(root);
    return it != a_repos.cend() && it->valid ? &*it : nullptr;
}

const Repository* Tracker::fresh(const QString& path, int maxAgeMs)
{
    QString root = rootOf(path);
    // (None, as last looked for: looked for again, since this is asked as a
    // menu opens - a repository made elsewhere a moment ago was off in the
    // Git menu for up to 5 s: the recheck of New Worktree, 8 November 2026.)
    if (root.isEmpty()) {
        a_roots.remove(path);
        root = rootOf(path);
    }
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
        const quint64 turn = ++a_turn;
        connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, root, started, turn] {
            watcher->deleteLater();
            a_reading.remove(root);
            a_cost.insert(root, nowMs() - started);
            // (Read now since this one began, after a commit: this older
            // answer would put what that did out of sight.)
            if (a_readDirect.value(root) < turn) store(root, watcher->result());
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
        stop(*a_process);
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
        if (a_process == nullptr) return;
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
        // (Said from the event loop, as above: this can be inside start().)
        QTimer::singleShot(0, this, [this] { emit finished(a_result); });
    });
    a_process->start(git, a_args);
    // (Gone already when it could not start: its folder is not there.)
    if (a_process == nullptr) return;
    if (!a_input.isEmpty()) a_process->write(a_input);
    a_process->closeWriteChannel();
}

void Job::cancel()
{
    if (!isRunning()) return;
    a_cancelled = true;
#ifdef Q_OS_WIN
    a_process->kill();
#else
    // (See stop().)
    a_process->terminate();
    QTimer::singleShot(kAskedToStopMs, this, [this] {
        if (isRunning()) a_process->kill();
    });
#endif
}

} // namespace qucs_s::git
