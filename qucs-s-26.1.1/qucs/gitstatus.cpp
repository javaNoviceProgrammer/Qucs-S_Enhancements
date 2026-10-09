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

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
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

// Runs \a program in \a dir. Git takes no lock it can do without (Claude
// may be committing meanwhile) and asks nothing. \a config: settings for
// every git it runs, itself or in a program it starts (gh's), as
// "key=value" pairs.
Output run(const QString& program, const QString& dir, const QStringList& args, int timeoutMs = 10000,
           const QStringList& config = {})
{
    Output o;
    if (program.isEmpty()) return o;
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    forgetRepositoryVariables(env);
    env.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));
    env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    env.insert(QStringLiteral("GH_PROMPT_DISABLED"), QStringLiteral("1"));
    env.insert(QStringLiteral("GH_NO_UPDATE_NOTIFIER"), QStringLiteral("1"));
    env.insert(QStringLiteral("NO_COLOR"), QStringLiteral("1"));
    if (!config.isEmpty()) {   // (git 2.31 and later)
        env.insert(QStringLiteral("GIT_CONFIG_COUNT"), QString::number(config.size()));
        for (qsizetype i = 0; i < config.size(); ++i) {
            env.insert(QStringLiteral("GIT_CONFIG_KEY_%1").arg(i), config.at(i).section(QLatin1Char('='), 0, 0));
            env.insert(QStringLiteral("GIT_CONFIG_VALUE_%1").arg(i), config.at(i).section(QLatin1Char('='), 1));
        }
    }
    p.setProcessEnvironment(env);
    p.setWorkingDirectory(dir);
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start(program, args);
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
    QStringList config{QStringLiteral("core.fsmonitor=false"), QStringLiteral("core.hooksPath=/dev/null")};
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

// \a config as git's -c options.
QStringList options(const QStringList& config)
{
    QStringList args;
    for (const QString& c : config) args << QStringLiteral("-c") << c;
    return args;
}

Output git(const QString& dir, const QStringList& args, const QStringList& guard)
{
    return run(program(), dir, options(guard) + args);
}

QString line(const Output& o)
{
    return o.exitCode == 0 ? QString::fromUtf8(o.out).trimmed() : QString();
}

bool refExists(const QString& root, const QString& ref, const QStringList& guard)
{
    return git(root, {QStringLiteral("rev-parse"), QStringLiteral("--verify"), QStringLiteral("-q"), ref}, guard).exitCode == 0;
}

// An untracked file's lines, or binary.
FileChange untrackedFile(const QString& root, const QString& path)
{
    FileChange f;
    f.path = path;
    f.untracked = true;
    QFile file(QDir(root).filePath(path));
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
    return guards(root);
}

void forgetRepositoryVariables(QProcessEnvironment& env)
{
    for (const char* name : {"GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY", "GIT_ALTERNATE_OBJECT_DIRECTORIES",
                             "GIT_COMMON_DIR", "GIT_NAMESPACE", "GIT_PREFIX"})
        env.remove(QLatin1String(name));
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
    const QString top = line(git(dir, {QStringLiteral("rev-parse"), QStringLiteral("--show-toplevel")},
                                 {QStringLiteral("core.fsmonitor=false"), QStringLiteral("core.hooksPath=/dev/null")}));
    if (top.isEmpty()) return s;
    s.repository = true;
    s.root = QDir::cleanPath(QDir::fromNativeSeparators(top));
    const QString& root = s.root;
    const QStringList guard = guards(root);
    const auto ask = [&](const QStringList& args) { return git(root, args, guard); };

    s.branch = line(ask({QStringLiteral("symbolic-ref"), QStringLiteral("--short"), QStringLiteral("-q"), QStringLiteral("HEAD")}));
    s.head = line(ask({QStringLiteral("rev-parse"), QStringLiteral("--short"), QStringLiteral("-q"), QStringLiteral("--verify"), QStringLiteral("HEAD")}));
    s.hasRemote = !line(ask({QStringLiteral("remote")})).isEmpty();

    // The default branch: the remote's (origin/HEAD), else a main or master.
    QString remoteDefault = line(ask({QStringLiteral("symbolic-ref"), QStringLiteral("--short"), QStringLiteral("-q"),
                                            QStringLiteral("refs/remotes/origin/HEAD")}));
    if (remoteDefault.isEmpty()) {
        for (const QString& name : {QStringLiteral("main"), QStringLiteral("master")})
            if (refExists(root, QStringLiteral("refs/remotes/origin/") + name, guard)) {
                remoteDefault = QStringLiteral("origin/") + name;
                break;
            }
    }
    if (!remoteDefault.isEmpty()) {
        s.defaultBranch = remoteDefault.section(QLatin1Char('/'), 1);
    } else {
        for (const QString& name : {QStringLiteral("main"), QStringLiteral("master")})
            if (refExists(root, QStringLiteral("refs/heads/") + name, guard)) {
                s.defaultBranch = name;
                break;
            }
    }

    if (!s.branch.isEmpty()) {
        s.upstream = line(ask({QStringLiteral("rev-parse"), QStringLiteral("--abbrev-ref"), QStringLiteral("-q"),
                                     QStringLiteral("--symbolic-full-name"), QStringLiteral("@{upstream}")}));
        if (!s.upstream.isEmpty()) {
            const QStringList counts = line(ask({QStringLiteral("rev-list"), QStringLiteral("--left-right"),
                                                       QStringLiteral("--count"), QStringLiteral("HEAD...@{upstream}")}))
                                           .split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            if (counts.size() == 2) {
                s.ahead = counts.at(0).toInt();
                s.behind = counts.at(1).toInt();
            }
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
            s.baseCommit = line(ask({QStringLiteral("rev-parse"), QStringLiteral("HEAD")}));
        }
    }

    // What changed, tracked: from the base to the files as they are.
    const Output numstat = ask({QStringLiteral("-c"), QStringLiteral("core.quotepath=off"), QStringLiteral("diff"),
                                      QStringLiteral("--numstat"), QStringLiteral("--no-renames"), QStringLiteral("-z"),
                                      QStringLiteral("--no-ext-diff"), QStringLiteral("--no-textconv"),
                                      QStringLiteral("--ignore-submodules=dirty"), s.baseCommit, QStringLiteral("--")});
    if (numstat.exitCode == 0) {
        for (const QByteArray& record : numstat.out.split('\0')) {
            const QList<QByteArray> fields = record.split('\t');
            if (fields.size() < 3) continue;
            FileChange f;
            f.path = QString::fromUtf8(record.mid(fields.at(0).size() + fields.at(1).size() + 2));
            f.binary = fields.at(0) == "-";
            f.added = f.binary ? 0 : fields.at(0).toInt();
            f.removed = f.binary ? 0 : fields.at(1).toInt();
            s.changes << f;
        }
    }
    // And the new files git does not know yet.
    const Output others = ask({QStringLiteral("ls-files"), QStringLiteral("--others"), QStringLiteral("--exclude-standard"),
                                     QStringLiteral("-z")});
    if (others.exitCode == 0) {
        int counted = 0;
        for (const QByteArray& name : others.out.split('\0')) {
            if (name.isEmpty()) continue;
            const QString path = QString::fromUtf8(name);
            if (++counted > kCountedFiles) {
                FileChange f;
                f.path = path;
                f.untracked = true;
                f.binary = true;
                s.changes << f;
            } else {
                s.changes << untrackedFile(root, path);
            }
        }
    }
    std::sort(s.changes.begin(), s.changes.end(), [](const FileChange& a, const FileChange& b) { return a.path < b.path; });
    // Anything not committed?
    const Output porcelain = ask({QStringLiteral("status"), QStringLiteral("--porcelain"), QStringLiteral("-z"),
                                  QStringLiteral("--ignore-submodules=dirty")});
    s.dirty = porcelain.exitCode == 0 && !porcelain.out.isEmpty();
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
        const Output o = git(s.root, args, guards(s.root));
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
        QFile file(QDir(s.root).filePath(f.path));
        if (!file.open(QIODevice::ReadOnly)) continue;
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
    if (gh.isEmpty() || root.isEmpty() || branch.isEmpty()) return pr;
    // (gh asks git about the repository: guarded too.)
    const Output o = run(gh, root,
                         {QStringLiteral("pr"), QStringLiteral("view"), branch, QStringLiteral("--json"),
                          QStringLiteral("number,url,title,state,isDraft")},
                         20000, guards(root));
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
