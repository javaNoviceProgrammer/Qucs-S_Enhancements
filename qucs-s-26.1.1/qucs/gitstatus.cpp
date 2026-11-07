/*
 * gitstatus.cpp - the git repository a folder is in
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "gitstatus.h"

#include "gitrepo.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QHash>
#include <QLocale>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>

namespace qucs_s::git {

namespace {

// git's tree of no files: what a repository without a commit is compared to.
const QString kEmptyTree = QStringLiteral("4b825dc642cb6eb9a060e54bf8d69288fbee4904");

// An untracked file counted line by line up to this size; past it, or
// holding a NUL byte, it counts as binary.
constexpr qint64 kCountedSize = 4 * 1024 * 1024;
// And so many of them at most.
constexpr int kCountedFiles = 2000;

QString findTool(const char* env, const QString& name)
{
    if (qEnvironmentVariableIsSet(env)) {
        const QString named = qEnvironmentVariable(env);
        return QFileInfo(named).isExecutable() ? named : QString();
    }
    QString found = QStandardPaths::findExecutable(name);
    if (found.isEmpty())
        found = QStandardPaths::findExecutable(name, {QStringLiteral("/opt/homebrew/bin"), QStringLiteral("/usr/local/bin")});
    return found;
}

struct Output {
    int exitCode = -1;
    QByteArray out;
};

// "key=value" apart: the value is after the last '.' (the driver's name,
// in the key, may hold a '=').
qsizetype equalsOf(const QString& setting)
{
    return setting.indexOf(QLatin1Char('='), std::max<qsizetype>(0, setting.lastIndexOf(QLatin1Char('.'))));
}

// A setting for every git \a env reaches (git 2.31 and later).
void addConfig(QProcessEnvironment& env, const QString& key, const QString& value)
{
    const int n = env.value(QStringLiteral("GIT_CONFIG_COUNT")).toInt();
    env.insert(QStringLiteral("GIT_CONFIG_KEY_%1").arg(n), key);
    env.insert(QStringLiteral("GIT_CONFIG_VALUE_%1").arg(n), value);
    env.insert(QStringLiteral("GIT_CONFIG_COUNT"), QString::number(n + 1));
}

// Runs \a program in \a dir, only looking, asking nothing. \a config:
// settings for every git it runs, itself or in a program it starts (gh's),
// as "key=value" pairs.
Output run(const QString& program, const QString& dir, const QStringList& args, int timeoutMs = 10000,
           const QStringList& config = {}, const QStringList& guard = {})
{
    Output o;
    if (program.isEmpty()) return o;
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    plainEnvironment(env);
    lookingEnvironment(env);
    env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    env.insert(QStringLiteral("GH_PROMPT_DISABLED"), QStringLiteral("1"));
    env.insert(QStringLiteral("GH_NO_UPDATE_NOTIFIER"), QStringLiteral("1"));
    env.insert(QStringLiteral("NO_COLOR"), QStringLiteral("1"));
    for (const QString& c : config) {
        const qsizetype at = equalsOf(c);
        addConfig(env, c.left(at), c.mid(at + 1));
    }
    QStringList all;
    applyGuards(guard, all, env);
    all << args;
    p.setProcessEnvironment(env);
    p.setWorkingDirectory(dir);
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start(program, all);
    if (!p.waitForStarted(5000)) return o;
    p.closeWriteChannel();
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(1000);
        return o;
    }
    o.exitCode = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    o.out = p.readAllStandardOutput();
    return o;
}

// What a repository's own configuration could make git run while Qucs-S
// only asks it how things stand - a file system monitor (core.fsmonitor),
// filters (filter.<driver>.clean, .process, run on a changed file), hooks:
// never. A project downloaded or unpacked with its .git is the user's own,
// so git's safe.directory does not stop that. As "key=value" settings,
// for -c (text conversions and external diffs: --no-textconv and
// --no-ext-diff where they apply; submodules not entered). Filters are
// named by the configuration: the drivers the repository's own sets are
// unset - the user's own (git-lfs's, in the global configuration) kept.
QStringList guards(const QString& root)
{
    // (log.showSignature: a signed commit's check runs gpg.program, and its
    // lines come before the log's own.)
    QStringList config{QStringLiteral("core.fsmonitor=false"), QStringLiteral("core.hooksPath=/dev/null"),
                       QStringLiteral("log.showSignature=false"), QStringLiteral("i18n.logOutputEncoding=UTF-8")};
    const QString pattern = QStringLiteral("^filter\\..*\\.(clean|smudge|process|required)$");
    QStringList keys;
    // "scope<tab>key" lines, the key whole (a driver's name may hold spaces).
    Output o = run(program(), root,
                   {QStringLiteral("-c"), config.at(0), QStringLiteral("config"), QStringLiteral("--show-scope"),
                    QStringLiteral("--includes"), QStringLiteral("--name-only"), QStringLiteral("--get-regexp"), pattern});
    if (o.exitCode == 0) {
        for (const QByteArray& l : o.out.split('\n')) {
            const QString entry = QString::fromUtf8(l);
            const QString scope = entry.section(QLatin1Char('\t'), 0, 0);
            if (scope != QLatin1String("global") && scope != QLatin1String("system"))
                keys << entry.section(QLatin1Char('\t'), 1);
        }
    } else if (o.exitCode != 1) {   // (1: none) - a git before --show-scope (2.26): the repository's file
        o = run(program(), root,
                {QStringLiteral("config"), QStringLiteral("--local"), QStringLiteral("--includes"), QStringLiteral("--name-only"),
                 QStringLiteral("--get-regexp"), pattern});
        for (const QByteArray& l : o.out.split('\n')) keys << QString::fromUtf8(l);
    }
    QStringList drivers;
    for (const QString& key : std::as_const(keys)) {
        const qsizetype first = key.indexOf(QLatin1Char('.')), last = key.lastIndexOf(QLatin1Char('.'));
        if (first < 0 || last <= first) continue;
        const QString driver = key.mid(first + 1, last - first - 1);
        if (!drivers.contains(driver)) drivers << driver;
    }
    for (const QString& driver : std::as_const(drivers))
        for (const char* setting : {"clean=", "smudge=", "process=", "required=false"})
            config << QStringLiteral("filter.%1.%2").arg(driver, QLatin1String(setting));
    return config;
}

Output git(const QString& dir, const QStringList& args, const QStringList& guard)
{
    return run(program(), dir, args, 10000, {}, guard);
}

QString line(const Output& o)
{
    return o.exitCode == 0 ? QString::fromUtf8(o.out).trimmed() : QString();
}

// An untracked file's lines, or binary. A link is its own line, as git has
// it: what it points at may be anywhere, a pipe that never ends or a device
// that has no end (git lists no pipe itself).
FileChange untrackedFile(const QString& root, const QString& path)
{
    FileChange f;
    f.path = path;
    f.untracked = true;
    const QFileInfo info(fileIn(root, path));
    if (info.isSymLink()) {
        f.added = 1;
        return f;
    }
    if (!info.isFile()) {
        f.binary = true;
        return f;
    }
    QFile file(info.filePath());
    if (file.size() > kCountedSize || !file.open(QIODevice::ReadOnly)) {
        f.binary = true;
        return f;
    }
    const QByteArray bytes = file.readAll();
    if (bytes.left(8000).contains('\0')) {
        f.binary = true;
        return f;
    }
    f.added = int(bytes.count('\n')) + (!bytes.isEmpty() && !bytes.endsWith('\n') ? 1 : 0);
    return f;
}

} // namespace

int Status::added() const
{
    int n = 0;
    for (const FileChange& f : changes) n += f.added;
    return n;
}

int Status::removed() const
{
    int n = 0;
    for (const FileChange& f : changes) n += f.removed;
    return n;
}

bool Status::operator==(const Status& o) const
{
    if (repository != o.repository || root != o.root || branch != o.branch || head != o.head || upstream != o.upstream
        || ahead != o.ahead || behind != o.behind || defaultBranch != o.defaultBranch || hasRemote != o.hasRemote
        || base != o.base || baseCommit != o.baseCommit || dirty != o.dirty || changes.size() != o.changes.size())
        return false;
    for (qsizetype i = 0; i < changes.size(); ++i) {
        const FileChange &a = changes.at(i), &b = o.changes.at(i);
        if (a.path != b.path || a.added != b.added || a.removed != b.removed || a.binary != b.binary
            || a.untracked != b.untracked)
            return false;
    }
    return true;
}

QStringList readOnlyGuards(const QString& root)
{
    // For a while: they take a git of their own to find.
    struct Kept {
        QStringList guards;
        qint64 at = 0;
    };
    static QMutex mutex;
    static QHash<QString, Kept> kept;
    static QElapsedTimer clock;
    QMutexLocker lock(&mutex);
    if (!clock.isValid()) clock.start();
    const auto it = kept.constFind(root);
    if (it != kept.cend() && clock.elapsed() - it->at < 30000) return it->guards;
    lock.unlock();
    const QStringList found = guards(root);
    lock.relock();
    kept.insert(root, {found, clock.elapsed()});
    return found;
}

void forgetRepositoryVariables(QProcessEnvironment& env)
{
    for (const char* name : {"GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY", "GIT_ALTERNATE_OBJECT_DIRECTORIES",
                             "GIT_COMMON_DIR", "GIT_NAMESPACE", "GIT_PREFIX"})
        env.remove(QLatin1String(name));
}

void plainEnvironment(QProcessEnvironment& env)
{
    forgetRepositoryVariables(env);
    // (Any of these beside a literal path and git stops.)
    for (const char* name : {"GIT_GLOB_PATHSPECS", "GIT_NOGLOB_PATHSPECS", "GIT_ICASE_PATHSPECS", "GIT_LITERAL_PATHSPECS"})
        env.remove(QLatin1String(name));
    addConfig(env, QStringLiteral("color.ui"), QStringLiteral("false"));
}

void lookingEnvironment(QProcessEnvironment& env)
{
    env.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));
    env.insert(QStringLiteral("GIT_NO_LAZY_FETCH"), QStringLiteral("1"));
    env.insert(QStringLiteral("GIT_LITERAL_PATHSPECS"), QStringLiteral("1"));
}

void applyGuards(const QStringList& guards, QStringList& args, QProcessEnvironment& env)
{
    for (const QString& g : guards) {
        const qsizetype at = equalsOf(g);
        if (g.first(at).contains(QLatin1Char('='))) addConfig(env, g.left(at), g.mid(at + 1));
        else args << QStringLiteral("-c") << g;
    }
}

QString fileIn(const QString& root, const QString& path)
{
    return root.endsWith(QLatin1Char('/')) ? root + path : root + QLatin1Char('/') + path;
}

QList<FileChange> readNumstat(const QByteArray& out)
{
    QList<FileChange> changes;
    for (const QByteArray& record : out.split('\0')) {
        const QList<QByteArray> fields = record.split('\t');
        if (fields.size() < 3) continue;
        FileChange f;
        f.path = QString::fromUtf8(record.mid(fields.at(0).size() + fields.at(1).size() + 2));
        f.binary = fields.at(0) == "-";
        f.added = f.binary ? 0 : fields.at(0).toInt();
        f.removed = f.binary ? 0 : fields.at(1).toInt();
        changes << f;
    }
    return changes;
}

QString program()
{
    if (qEnvironmentVariableIsSet("QUCS_GIT")) return findTool("QUCS_GIT", QStringLiteral("git"));
    static const QString found = [] {
        const QString git = findTool("QUCS_GIT", QStringLiteral("git"));
#ifdef Q_OS_MACOS
        // /usr/bin/git only hands over to the command line tools - and,
        // without them, asks in a dialog to install them.
        if (QFileInfo(git).canonicalFilePath() == QLatin1String("/usr/bin/git")
            && run(QStringLiteral("/usr/bin/xcode-select"), QDir::homePath(), {QStringLiteral("-p")}, 5000).exitCode != 0)
            return QString();
#endif
        return git;
    }();
    return found;
}

Status status(const QString& dir)
{
    Status s;
    if (dir.isEmpty() || !QFileInfo(dir).isDir() || program().isEmpty()) return s;
    const QString root = topLevel(dir);
    if (root.isEmpty()) return s;
    const QStringList guard = readOnlyGuards(root);
    const auto ask = [&](const QStringList& args) { return git(root, args, guard); };

    // Where it stands, what is not committed and which files are new, in one
    // look: a program for each of these took 14 on a repository with a remote,
    // and listing the new files a second walk of its folders. (Not a work tree
    // - a .git that leads nowhere, a bare one - and it fails.)
    const Output looked = ask({QStringLiteral("status"), QStringLiteral("--porcelain=v2"), QStringLiteral("-z"), QStringLiteral("--branch"),
                               QStringLiteral("--untracked-files=all"), QStringLiteral("--ignore-submodules=dirty")});
    if (looked.exitCode != 0) return s;
    s.repository = true;
    s.root = QDir::cleanPath(QDir::fromNativeSeparators(root));
    Repository r;
    readStatus(QString::fromUtf8(looked.out), r);
    s.branch = r.branch;
    s.head = r.head;
    s.upstream = r.upstream;
    s.ahead = r.ahead;
    s.behind = r.behind;
    s.dirty = !r.entries.isEmpty();
    s.hasRemote = !line(ask({QStringLiteral("remote")})).isEmpty();

    // The default branch: the remote's (origin/HEAD), else a main or master.
    // Each that exists, with what a symbolic one points at.
    QHash<QString, QString> known;
    const Output refs = ask({QStringLiteral("for-each-ref"), QStringLiteral("--format=%(refname) %(symref:short)"), QStringLiteral("refs/remotes/origin/HEAD"),
                             QStringLiteral("refs/remotes/origin/main"), QStringLiteral("refs/remotes/origin/master"), QStringLiteral("refs/heads/main"),
                             QStringLiteral("refs/heads/master")});
    if (refs.exitCode == 0)
        for (const QString& l : QString::fromUtf8(refs.out).split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            known.insert(l.section(QLatin1Char(' '), 0, 0), l.section(QLatin1Char(' '), 1));
    QString remoteDefault = known.value(QStringLiteral("refs/remotes/origin/HEAD"));
    if (remoteDefault.isEmpty()) {
        for (const QString& name : {QStringLiteral("main"), QStringLiteral("master")})
            if (known.contains(QStringLiteral("refs/remotes/origin/") + name)) {
                remoteDefault = QStringLiteral("origin/") + name;
                break;
            }
    }
    if (!remoteDefault.isEmpty()) {
        s.defaultBranch = remoteDefault.section(QLatin1Char('/'), 1);
    } else {
        for (const QString& name : {QStringLiteral("main"), QStringLiteral("master")})
            if (known.contains(QStringLiteral("refs/heads/") + name)) {
                s.defaultBranch = name;
                break;
            }
    }

    // Where the changes are counted from.
    if (s.head.isEmpty()) {
        s.base = QStringLiteral("HEAD");
        s.baseCommit = kEmptyTree;
    } else {
        QString against;
        if (s.onDefaultBranch()) against = s.upstream;
        else if (!remoteDefault.isEmpty()) against = remoteDefault;
        else if (!s.defaultBranch.isEmpty() && s.branch != s.defaultBranch) against = s.defaultBranch;
        else against = s.upstream;
        if (!against.isEmpty()) {
            const QString fork = line(ask({QStringLiteral("merge-base"), QStringLiteral("HEAD"), against}));
            if (!fork.isEmpty()) {
                s.base = against;
                s.baseCommit = fork;
            }
        }
        if (s.baseCommit.isEmpty()) {
            s.base = QStringLiteral("HEAD");
            // (status said it in full.)
            s.baseCommit = QString::fromUtf8(looked.out.mid(looked.out.indexOf("# branch.oid ") + 13, 64)).section(QChar(0), 0, 0);
        }
    }

    // What changed, tracked: from the base to the files as they are.
    const Output numstat = ask({QStringLiteral("-c"), QStringLiteral("core.quotepath=off"), QStringLiteral("diff"),
                                      QStringLiteral("--numstat"), QStringLiteral("--no-renames"), QStringLiteral("-z"),
                                      QStringLiteral("--no-ext-diff"), QStringLiteral("--no-textconv"),
                                      QStringLiteral("--ignore-submodules=dirty"), s.baseCommit, QStringLiteral("--")});
    if (numstat.exitCode == 0) s.changes = readNumstat(numstat.out);
    // And the new files git does not know yet.
    int counted = 0;
    for (const Entry& e : r.entries) {
        if (!e.untracked) continue;
        if (++counted > kCountedFiles) {
            FileChange f;
            f.path = e.path;
            f.untracked = true;
            f.binary = true;
            s.changes << f;
        } else {
            s.changes << untrackedFile(root, e.path);
        }
    }
    std::sort(s.changes.begin(), s.changes.end(), [](const FileChange& a, const FileChange& b) { return a.path < b.path; });
    return s;
}

QString diff(const Status& s, const QString& path)
{
    if (!s.repository) return QString();
    QString text;
    bool untrackedOnly = false;
    for (const FileChange& f : s.changes)
        if (!path.isEmpty() && f.path == path) untrackedOnly = f.untracked;
    if (!untrackedOnly) {
        QStringList args{QStringLiteral("-c"), QStringLiteral("core.quotepath=off"), QStringLiteral("diff"),
                         QStringLiteral("--no-color"), QStringLiteral("--no-ext-diff"), QStringLiteral("--no-textconv"),
                         QStringLiteral("--ignore-submodules=dirty"), QStringLiteral("--no-renames"),
                         s.baseCommit, QStringLiteral("--")};
        if (!path.isEmpty()) args << path;
        const Output o = git(s.root, args, readOnlyGuards(s.root));
        if (o.exitCode == 0) text = QString::fromUtf8(o.out);
    }
    // Untracked files: all their lines added.
    for (const FileChange& f : s.changes) {
        if (!f.untracked || (!path.isEmpty() && f.path != path)) continue;
        if (!text.isEmpty() && !text.endsWith(QLatin1Char('\n'))) text += QLatin1Char('\n');
        text += QStringLiteral("diff --git a/%1 b/%1\nnew file\n").arg(f.path);
        if (f.binary) {
            text += QStringLiteral("Binary file %1 (new)\n").arg(f.path);
            continue;
        }
        const QFileInfo info(fileIn(s.root, f.path));
        // (As git shows a link: the place it points at, a line.)
        if (info.isSymLink()) {
            text += QStringLiteral("--- /dev/null\n+++ b/%1\n@@ -0,0 +1 @@\n+%2\n").arg(f.path, info.symLinkTarget());
            continue;
        }
        QFile file(info.filePath());
        if (!info.isFile() || !file.open(QIODevice::ReadOnly)) continue;
        QString body = QString::fromUtf8(file.readAll());
        if (body.endsWith(QLatin1Char('\n'))) body.chop(1);
        const QStringList lines = body.isEmpty() ? QStringList() : body.split(QLatin1Char('\n'));
        text += QStringLiteral("--- /dev/null\n+++ b/%1\n").arg(f.path);
        if (!lines.isEmpty()) {
            text += QStringLiteral("@@ -0,0 +1,%1 @@\n").arg(lines.size());
            for (const QString& l : lines) text += QLatin1Char('+') + l + QLatin1Char('\n');
        }
    }
    return text;
}

QString statText(int added, int removed)
{
    const QLocale locale;
    return QStringLiteral("+%1 −%2").arg(locale.toString(added), locale.toString(removed));
}

QString ghProgram()
{
    return findTool("QUCS_GH", QStringLiteral("gh"));
}

PullRequest pullRequest(const QString& root, const QString& branch)
{
    PullRequest pr;
    const QString gh = ghProgram();
    // (A name that begins with '-' gh would take for an option: a repository's
    // HEAD can name such a branch.)
    if (gh.isEmpty() || root.isEmpty() || branch.isEmpty() || branch.startsWith(QLatin1Char('-'))) return pr;
    // (gh asks git about the repository: guarded too.)
    const Output o = run(gh, root,
                         {QStringLiteral("pr"), QStringLiteral("view"), branch, QStringLiteral("--json"),
                          QStringLiteral("number,url,title,state,isDraft")},
                         20000, readOnlyGuards(root));
    if (o.exitCode != 0) return pr;
    const QJsonObject json = QJsonDocument::fromJson(o.out).object();
    pr.number = json.value(QStringLiteral("number")).toInt();
    pr.url = json.value(QStringLiteral("url")).toString();
    const QString scheme = QUrl(pr.url).scheme().toLower();
    if (scheme != QLatin1String("https") && scheme != QLatin1String("http")) pr.url.clear();   // (opened with the system: a web page alone)
    pr.title = json.value(QStringLiteral("title")).toString();
    pr.state = json.value(QStringLiteral("state")).toString();
    pr.draft = json.value(QStringLiteral("isDraft")).toBool();
    return pr;
}

} // namespace qucs_s::git
