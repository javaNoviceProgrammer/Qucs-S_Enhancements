/*
 * qucscontrol_git.cpp - Claude's git: what the File Browser's Git menu, the
 *                       Git menu and the status bar do - the repository's
 *                       state, its changes, history and blame; staging,
 *                       committing, branches, the remotes, stashes, tags
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "qucscontrol.h"
#include "qucscontrol_p.h"

#include "gitrepo.h"
#include "filebrowser.h"
#include "gitstatus.h"
#include "gitui.h"
#include "main.h"
#include "qucs.h"
#include "qucsdoc.h"
#include "schematic.h"
#include "schematicdiff.h"
#include "settings.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>

using namespace qucs_s::control;
namespace git = qucs_s::git;

namespace {

// Long texts cut, and said so.
constexpr int kMostText = 150000;

QString cut(const QString& text)
{
    if (text.size() <= kMostText) return text;
    return text.left(kMostText) + QLatin1Char('\n') + tr("[... cut: %1 more characters - give 'path' for less]").arg(text.size() - kMostText);
}

QString native(const QString& path)
{
    return QDir::toNativeSeparators(path);
}

QString what(QChar c)
{
    switch (c.unicode()) {
    case 'M': return QStringLiteral("modified");
    case 'T': return QStringLiteral("type changed");
    case 'A': return QStringLiteral("added");
    case 'D': return QStringLiteral("deleted");
    case 'R': return QStringLiteral("renamed");
    case 'C': return QStringLiteral("copied");
    default: return QString();
    }
}

// The files a tool names: relative to \a root when given (the repository
// its 'path' named), else as \a resolve takes a path.
QStringList pathsOf(const QJsonValue& value, const QString& root, const std::function<QString(const QString&)>& resolve)
{
    QStringList out;
    for (const QJsonValue& v : value.toArray()) {
        const QString p = v.toString().trimmed();
        if (p.isEmpty()) continue;
        out << (!root.isEmpty() && QFileInfo(p).isRelative() ? QDir::cleanPath(QDir(root).filePath(p)) : resolve(p));
    }
    return out;
}

QJsonObject commitJson(const git::Commit& c)
{
    QJsonObject o{{QStringLiteral("hash"), c.hash},
                  {QStringLiteral("short"), c.shortHash},
                  {QStringLiteral("author"), QStringLiteral("%1 <%2>").arg(c.author, c.email)},
                  {QStringLiteral("date"), c.date.toString(Qt::ISODate)},
                  {QStringLiteral("subject"), c.subject}};
    if (!c.refs.isEmpty()) o.insert(QStringLiteral("refs"), QJsonArray::fromStringList(c.refs));
    if (c.parents.size() > 1) o.insert(QStringLiteral("merge"), true);
    return o;
}

} // namespace

QString QucsControl::gitPath(const QString& path) const
{
    const QString given = path.trimmed();
    if (given.isEmpty() || QFileInfo(given).isAbsolute()) return given.isEmpty() ? given : QDir::cleanPath(given);
    // From the open project's folder, the workspace, the folder of the
    // document in front, the File Browser's: the first where it is - or,
    // for one not there yet (a file to ignore before it is made), the first
    // in a repository.
    QStringList bases{QucsSettings.QucsWorkDir.absolutePath(), QucsSettings.qucsWorkspaceDir.absolutePath()};
    if (const QString file = a_app->gitFile(); !file.isEmpty()) bases << QFileInfo(file).absolutePath();
    if (const FileBrowser* fb = a_app->fileBrowserPanel(); fb != nullptr && !fb->location().isEmpty()) bases << fb->location();
    QStringList candidates;
    for (const QString& base : std::as_const(bases))
        if (const QString c = QDir::cleanPath(QDir(base).filePath(given)); !candidates.contains(c)) candidates << c;
    for (const QString& c : std::as_const(candidates))
        if (QFileInfo::exists(c)) return c;
    for (const QString& c : std::as_const(candidates))
        if (!git::topLevel(c).isEmpty()) return c;
    return absolute(given);
}

QString QucsControl::gitRootOf(const QJsonObject& args, QStringList* paths, QString* error)
{
    const QString path = args.value(QLatin1String("path")).toString().trimmed();
    const auto resolve = [this](const QString& p) { return gitPath(p); };
    if (!path.isEmpty()) {
        const QString root = gitRootOf(path, error);
        *paths = pathsOf(args.value(QLatin1String("paths")), root, resolve);
        return root;
    }
    *paths = pathsOf(args.value(QLatin1String("paths")), {}, resolve);
    return gitRootOf(paths->value(0), error);
}

QString QucsControl::gitRootOf(const QString& path, QString* error)
{
    if (git::program().isEmpty()) {
        *error = tr("git is not installed (nor where Homebrew puts it): Qucs-S's git needs it.");
        return {};
    }
    const QString where = !path.trimmed().isEmpty() ? gitPath(path) : a_app->gitFolder();
    const QString root = git::topLevel(where);
    if (root.isEmpty())
        *error = tr("%1 is in no git repository: git_init makes one, git_clone brings one.").arg(native(where));
    return root;
}

QJsonObject QucsControl::gitState(const QString& root)
{
    QJsonObject o;
    const git::Repository* found = git::Tracker::instance()->readNow(root);
    if (found == nullptr) return o;
    const git::Repository& r = *found;
    o.insert(QStringLiteral("repository"), native(r.root));
    if (!r.branch.isEmpty()) o.insert(QStringLiteral("branch"), r.branch);
    else o.insert(QStringLiteral("branch"), QJsonValue(QJsonValue::Null));
    o.insert(QStringLiteral("head"), r.head.isEmpty() ? tr("none: no commit yet") : r.head);
    if (r.branch.isEmpty() && !r.head.isEmpty()) o.insert(QStringLiteral("detached"), true);
    if (!r.upstream.isEmpty()) {
        o.insert(QStringLiteral("upstream"), r.upstream);
        o.insert(QStringLiteral("ahead"), r.ahead);
        o.insert(QStringLiteral("behind"), r.behind);
    }
    if (!r.operation.isEmpty()) o.insert(QStringLiteral("under way"), r.operation);
    QJsonArray staged, unstaged, untracked, conflicts;
    for (const git::Entry& e : r.entries) {
        if (e.ignored) continue;
        if (e.conflicted) {
            conflicts.append(e.path);
            continue;
        }
        if (e.untracked) {
            untracked.append(e.path);
            continue;
        }
        if (e.hasStaged()) {
            QJsonObject s{{QStringLiteral("path"), e.path}, {QStringLiteral("state"), what(e.staged)}};
            if (!e.from.isEmpty()) s.insert(QStringLiteral("from"), e.from);
            staged.append(s);
        }
        if (e.unstaged != QLatin1Char('.')) unstaged.append(QJsonObject{{QStringLiteral("path"), e.path}, {QStringLiteral("state"), what(e.unstaged)}});
    }
    o.insert(QStringLiteral("staged"), staged);
    o.insert(QStringLiteral("not staged"), unstaged);
    o.insert(QStringLiteral("untracked"), untracked);
    if (!conflicts.isEmpty()) {
        o.insert(QStringLiteral("conflicts"), conflicts);
        QJsonArray kept;
        for (const QJsonValue& c : std::as_const(conflicts))
            if (const QString file = QDir(r.root).filePath(c.toString()); a_app->keptInConflict(file)) kept.append(native(file));
        if (!kept.isEmpty())
            o.insert(QStringLiteral("kept in their tabs"),
                     QJsonObject{{QStringLiteral("files"), kept},
                                 {QStringLiteral("why"), tr("git's conflict marks are in them, which no schematic reads: each tab "
                                                           "shows the version from before, and is not saved over the file unless "
                                                           "save_document gets 'replace'. git_resolve takes a side or shows both.")}});
    }
    if (const int n = int(git::stashes(root).size()); n > 0) o.insert(QStringLiteral("stashes"), n);
    if (r.changedCount() == 0 && r.operation.isEmpty()) o.insert(QStringLiteral("clean"), true);
    return o;
}

void QucsControl::reloadGitChanged(const QString& root, QJsonObject* answer)
{
    // (The window's watch would, a moment later: the next tool reads them.)
    QJsonArray loaded, kept, failed, unsaved;
    for (QucsDoc* doc : a_app->allDocuments()) {
        const QString file = doc->getDocName();
        if (file.isEmpty()) continue;
        bool inside = false;
        git::relativePath(root, file, &inside);
        const QFileInfo info(file);
        if (!inside || !info.exists() || (doc->getLastSaved().isValid() && info.lastModified() <= doc->getLastSaved())) continue;
        const QString name = native(file);
        if (doc->getDocChanged()) {
            unsaved.append(name);
            continue;
        }
        const quint64 editor = QucsDoc::editor();
        QucsDoc::setEditor(QucsDoc::kOnDisk);
        const QucsApp::Reloaded how = a_app->reloadFromDisk(doc);
        QucsDoc::setEditor(editor);
        if (how == QucsApp::Reloaded::Yes) loaded.append(name);
        else if (how == QucsApp::Reloaded::Failed) failed.append(name);
        else if (how == QucsApp::Reloaded::InConflict || a_app->keptInConflict(file)) kept.append(name);
    }
    if (answer == nullptr) return;
    if (!loaded.isEmpty()) answer->insert(QStringLiteral("loaded again"), loaded);
    if (!kept.isEmpty())
        answer->insert(QStringLiteral("kept in their tabs"),
                       QJsonObject{{QStringLiteral("files"), kept},
                                   {QStringLiteral("why"), tr("git's conflict marks are in them, which no schematic reads: each tab "
                                                             "shows the version from before, and is not saved over the file "
                                                             "unless save_document gets 'replace'. git_resolve takes a side or "
                                                             "shows both.")}});
    if (!failed.isEmpty()) answer->insert(QStringLiteral("could not be read again"), failed);
    if (!unsaved.isEmpty()) answer->insert(QStringLiteral("not loaded again: unsaved changes"), unsaved);
}

// ----------------------------------------------------------------------
// What only looks

QJsonObject QucsControl::gitStatus(const QJsonObject& args)
{
    const QString path = args.value(QLatin1String("path")).toString().trimmed();
    QString error;
    const QString root = gitRootOf(path, &error);
    if (root.isEmpty()) return errorResult(error);
    QJsonObject state = gitState(root);
    const QString file = !path.isEmpty() ? gitPath(path) : a_app->gitFile();
    if (!file.isEmpty()) {
        bool inside = false;
        const QString rel = git::relativePath(root, file, &inside);
        if (inside && !rel.isEmpty()) {
            const git::Entry* e = git::Tracker::instance()->entryOf(file);
            state.insert(QStringLiteral("path"), QJsonObject{{QStringLiteral("path"), rel},
                                                            {QStringLiteral("state"), e == nullptr ? tr("as committed") : e->describe()}});
        }
    }
    return jsonResult(state);
}

QJsonObject QucsControl::gitDiff(const QJsonObject& args)
{
    const QString path = args.value(QLatin1String("path")).toString().trimmed();
    const QString of = args.value(QLatin1String("of")).toString(QStringLiteral("head"));
    if (of != QLatin1String("head") && of != QLatin1String("staged") && of != QLatin1String("unstaged"))
        return errorResult(tr("'of' is head, staged or unstaged."));
    QString error;
    const QString root = gitRootOf(path, &error);
    if (root.isEmpty()) return errorResult(error);
    const QString text = git::diff(root, path.isEmpty() ? QString() : gitPath(path),
                                   of == QLatin1String("staged") ? git::DiffOf::Staged
                                   : of == QLatin1String("unstaged") ? git::DiffOf::Unstaged : git::DiffOf::Head);
    if (text.trimmed().isEmpty()) return textResult(tr("No changes (%1).").arg(of));
    // A schematic's lines move and its view changes: what changed in it,
    // part by part, before git's lines.
    const QString parts = git::schematicChangesOf(root, path.isEmpty() ? QString() : gitPath(path),
                                                  of == QLatin1String("staged") ? git::DiffOf::Staged
                                                  : of == QLatin1String("unstaged") ? git::DiffOf::Unstaged : git::DiffOf::Head);
    return textResult(cut(parts.isEmpty() ? text : parts + QLatin1Char('\n') + text));
}

QJsonObject QucsControl::gitLog(const QJsonObject& args)
{
    const QString path = args.value(QLatin1String("path")).toString().trimmed();
    QString error;
    const QString root = gitRootOf(path, &error);
    if (root.isEmpty()) return errorResult(error);
    const int max = std::clamp(args.value(QLatin1String("max")).toInt(50), 1, 1000);
    const int skip = std::max(0, args.value(QLatin1String("skip")).toInt(0));
    const QString ref = args.value(QLatin1String("ref")).toString().trimmed();
    if (ref.startsWith(QLatin1Char('-'))) return errorResult(tr("%1 is no ref (it begins with '-').").arg(ref));
    QJsonArray commits;
    for (const git::Commit& c : git::log(root, path.isEmpty() ? QString() : gitPath(path), max, ref, skip)) commits.append(commitJson(c));
    QJsonObject o{{QStringLiteral("repository"), native(root)}, {QStringLiteral("commits"), commits}};
    if (commits.size() == max) o.insert(QStringLiteral("more"), tr("there may be more: 'skip' %1").arg(skip + max));
    return jsonResult(o);
}

QJsonObject QucsControl::gitShow(const QJsonObject& args)
{
    const QString commit = args.value(QLatin1String("commit")).toString().trimmed();
    const QString path = args.value(QLatin1String("path")).toString().trimmed();
    if (commit.isEmpty()) return errorResult(tr("Which commit? ('commit': a hash, a branch, HEAD~1)"));
    if (commit.startsWith(QLatin1Char('-'))) return errorResult(tr("%1 is no commit (it begins with '-').").arg(commit));
    QString error;
    const QString root = gitRootOf(path, &error);
    if (root.isEmpty()) return errorResult(error);
    if (!git::run(root, {QStringLiteral("rev-parse"), QStringLiteral("--verify"), QStringLiteral("-q"), commit + QStringLiteral("^{commit}")}, true, 15000).ok())
        return errorResult(tr("%1 is no commit of %2.").arg(commit, native(root)));
    const QString parts = git::schematicChangesIn(root, commit, path.isEmpty() ? QString() : gitPath(path));
    const QString text = git::show(root, commit, path.isEmpty() ? QString() : gitPath(path));
    return textResult(cut(parts.isEmpty() ? text : parts + QLatin1Char('\n') + text));
}

QJsonObject QucsControl::gitBlame(const QJsonObject& args)
{
    const QString path = args.value(QLatin1String("path")).toString().trimmed();
    if (path.isEmpty()) return errorResult(tr("Which file? ('path')"));
    QString error;
    const QString root = gitRootOf(path, &error);
    if (root.isEmpty()) return errorResult(error);
    const QString file = gitPath(path);
    if (!QFileInfo(file).isFile()) return errorResult(tr("There is no file %1.").arg(native(file)));
    const int from = std::max(1, args.value(QLatin1String("from")).toInt(1));
    const int to = args.value(QLatin1String("to")).toInt(0);
    const QList<git::BlameLine> all = git::blame(root, file);
    if (all.isEmpty()) return errorResult(tr("git has no blame of %1 (not committed yet, or not followed).").arg(native(file)));
    QJsonArray lines;
    for (const git::BlameLine& l : all) {
        if (l.line < from || (to > 0 && l.line > to)) continue;
        if (lines.size() >= 2000) break;
        QJsonObject o{{QStringLiteral("line"), l.line}, {QStringLiteral("text"), l.text}};
        if (l.uncommitted) {
            o.insert(QStringLiteral("commit"), QStringLiteral("not committed"));
        } else {
            o.insert(QStringLiteral("commit"), l.shortHash);
            o.insert(QStringLiteral("author"), l.author);
            o.insert(QStringLiteral("date"), l.date.toString(Qt::ISODate));
            o.insert(QStringLiteral("summary"), l.summary);
        }
        lines.append(o);
    }
    return jsonResult(QJsonObject{{QStringLiteral("path"), native(file)}, {QStringLiteral("lines"), lines}});
}

// ----------------------------------------------------------------------
// What changes

QJsonObject QucsControl::gitStage(const QJsonObject& args)
{
    QStringList paths;
    QString error;
    const QString root = gitRootOf(args, &paths, &error);
    const bool all = args.value(QLatin1String("all")).toBool();
    if (paths.isEmpty() && !all) return errorResult(tr("Which files? ('paths', or 'all': every change)"));
    if (root.isEmpty()) return errorResult(error);
    const git::Result r = git::stage(root, all ? QStringList() : paths);
    if (!r.ok()) return errorResult(tr("git could not stage them: %1").arg(r.error()));
    return jsonResult(gitState(root));
}

QJsonObject QucsControl::gitUnstage(const QJsonObject& args)
{
    QStringList paths;
    QString error;
    const QString root = gitRootOf(args, &paths, &error);
    const bool all = args.value(QLatin1String("all")).toBool();
    if (paths.isEmpty() && !all) return errorResult(tr("Which files? ('paths', or 'all': everything staged)"));
    if (root.isEmpty()) return errorResult(error);
    const git::Result r = git::unstage(root, all ? QStringList() : paths);
    if (!r.ok()) return errorResult(tr("git could not unstage them: %1").arg(r.error()));
    return jsonResult(gitState(root));
}

QJsonObject QucsControl::gitDiscard(const QJsonObject& args)
{
    QStringList paths;
    QString error;
    const QString root = gitRootOf(args, &paths, &error);
    const bool all = args.value(QLatin1String("all")).toBool();
    if (paths.isEmpty() && !all) return errorResult(tr("Which files? ('paths', or 'all': every change)"));
    if (root.isEmpty()) return errorResult(error);
    QStringList trashed;
    const git::Result r = git::discard(root, all ? QStringList() : paths, &trashed);
    if (!r.ok()) return errorResult(tr("git could not discard them: %1").arg(r.error()));
    QJsonObject o = gitState(root);
    reloadGitChanged(root, &o);
    if (!trashed.isEmpty()) {
        QJsonArray where;
        for (const QString& t : std::as_const(trashed)) where.append(native(t));
        o.insert(QStringLiteral("to the trash"), where);
    }
    return jsonResult(o);
}

void QucsControl::gitJob(const QString& root, const QStringList& args, int timeoutMs, const QByteArray& input,
                         const std::function<QJsonObject(const git::Result&)>& then, const Done& done)
{
    auto* job = new git::Job(root, args, this, input);
    QPointer<git::Job> guard(job);
    QTimer::singleShot(timeoutMs, job, [guard] {
        if (!guard.isNull()) guard->cancel();
    });
    connect(job, &git::Job::finished, this, [job, then, done](const git::Result& r) {
        job->deleteLater();
        done(then(r));
    });
    job->start();
}

void QucsControl::gitCommit(const QJsonObject& args, const Done& done)
{
    const QString message = args.value(QLatin1String("message")).toString().trimmed();
    const bool amend = args.value(QLatin1String("amend")).toBool();
    const bool push = args.value(QLatin1String("push")).toBool();
    if (message.isEmpty()) {
        done(errorResult(tr("A commit needs a 'message'.")));
        return;
    }
    QStringList paths;
    QString error;
    const QString root = gitRootOf(args, &paths, &error);
    if (root.isEmpty()) {
        done(errorResult(error));
        return;
    }
    QStringList commitArgs{QStringLiteral("commit"), QStringLiteral("-F"), QStringLiteral("-")};
    if (amend) commitArgs << QStringLiteral("--amend");
    if (!paths.isEmpty()) {
        const git::Result added = git::stage(root, paths);
        if (!added.ok()) {
            done(errorResult(tr("git could not stage them: %1").arg(added.error())));
            return;
        }
        commitArgs << QStringLiteral("--");
        for (const QString& p : paths) commitArgs << git::relativePath(root, p);
    } else if (!amend) {
        const git::Repository* repo = git::Tracker::instance()->readNow(root);
        // (A merge's commit records it, nothing staged or not: each file
        // resolved as the branch had it.)
        if (repo != nullptr && repo->stagedCount() == 0 && repo->operation != QLatin1String("merge")) {
            done(errorResult(tr("Nothing is staged: git_stage the files first, or give them as 'paths'.")));
            return;
        }
    }
    gitJob(root, commitArgs, 300000, message.toUtf8(), [this, root, push, done](const git::Result& r) -> QJsonObject {
        if (!r.ok()) return errorResult(tr("git could not commit: %1").arg(r.error()));
        const QString hash = git::run(root, {QStringLiteral("rev-parse"), QStringLiteral("--short"), QStringLiteral("HEAD")}, true, 15000).out.trimmed();
        QJsonObject o = gitState(root);
        o.insert(QStringLiteral("commit"), hash);
        if (!push) return jsonResult(o);
        // Then pushed: answered once that is done.
        const git::Repository repo = git::read(root);
        QString why;
        const QStringList pushArgs = git::pushArgs(repo, &why);
        if (pushArgs.isEmpty()) {
            o.insert(QStringLiteral("not pushed"), why);
            return jsonResult(o);
        }
        gitJob(root, pushArgs, 300000, {}, [this, root, hash](const git::Result& p) -> QJsonObject {
            QJsonObject state = gitState(root);
            state.insert(QStringLiteral("commit"), hash);
            if (!p.ok()) state.insert(QStringLiteral("not pushed"), p.error());
            else state.insert(QStringLiteral("pushed"), true);
            return jsonResult(state);
        }, done);
        return QJsonObject{{QStringLiteral("pending"), true}};
    }, [done](const QJsonObject& answer) {
        if (!answer.value(QLatin1String("pending")).toBool()) done(answer);
    });
}

QJsonObject QucsControl::gitBranch(const QJsonObject& args)
{
    const QString action = args.value(QLatin1String("action")).toString(QStringLiteral("list"));
    const QString name = args.value(QLatin1String("name")).toString().trimmed();
    QString error;
    const QString root = gitRootOf(args.value(QLatin1String("path")).toString(), &error);
    if (root.isEmpty()) return errorResult(error);
    if (action == QLatin1String("list")) {
        QJsonArray local, remote;
        for (const git::Branch& b : git::branches(root)) {
            QJsonObject o{{QStringLiteral("name"), b.name}, {QStringLiteral("commit"), b.commit}, {QStringLiteral("subject"), b.subject},
                          {QStringLiteral("date"), b.date.toString(Qt::ISODate)}};
            if (b.current) o.insert(QStringLiteral("current"), true);
            if (!b.upstream.isEmpty()) {
                o.insert(QStringLiteral("upstream"), b.upstream);
                o.insert(QStringLiteral("ahead"), b.ahead);
                o.insert(QStringLiteral("behind"), b.behind);
            }
            (b.remote ? remote : local).append(o);
        }
        return jsonResult(QJsonObject{{QStringLiteral("repository"), native(root)}, {QStringLiteral("local"), local}, {QStringLiteral("remote"), remote}});
    }
    if (name.isEmpty() && action != QLatin1String("rename")) return errorResult(tr("Which branch? ('name')"));
    git::Result r;
    if (action == QLatin1String("create")) {
        r = git::createBranch(root, name, args.value(QLatin1String("start")).toString().trimmed(),
                              args.value(QLatin1String("switch")).toBool(true));
    } else if (action == QLatin1String("switch")) {
        r = git::switchTo(root, name);
        if (!r.ok() && args.value(QLatin1String("stash")).toBool()
            && (r.err.contains(QLatin1String("would be overwritten")) || r.err.contains(QLatin1String("commit your changes")))) {
            const git::Result stashed = git::stash(root, tr("Before switching to %1").arg(name));
            if (!stashed.ok()) return errorResult(tr("git could not stash the changes: %1").arg(stashed.error()));
            r = git::switchTo(root, name);
        }
    } else if (action == QLatin1String("rename")) {
        const QString to = args.value(QLatin1String("to")).toString().trimmed();
        if (to.isEmpty()) return errorResult(tr("Its new name? ('to')"));
        const QString from = name.isEmpty() ? git::read(root).branch : name;
        if (from.isEmpty()) return errorResult(tr("No branch is checked out (HEAD is detached): give the one to rename as 'name'."));
        r = git::renameBranch(root, from, to);
    } else if (action == QLatin1String("delete")) {
        r = git::deleteBranch(root, name, args.value(QLatin1String("force")).toBool());
        if (!r.ok() && r.err.contains(QLatin1String("not fully merged")))
            return errorResult(tr("%1 has commits no other branch has - they would be lost: 'force' deletes it all the same.").arg(name));
    } else if (action == QLatin1String("merge")) {
        r = git::merge(root, name);
        if (!r.ok() && (r.out.contains(QLatin1String("CONFLICT")) || r.err.contains(QLatin1String("CONFLICT")))) {
            QJsonObject o = gitState(root);
            o.insert(QStringLiteral("merge"), tr("conflicts: resolve the files in 'conflicts' - git_resolve takes a side whole or "
                                                 "shows both; an edited file is marked resolved with git_stage - then git_commit; "
                                                 "or git_abort"));
            o.insert(QStringLiteral("git said"), r.error());
            reloadGitChanged(root, &o);
            return jsonResult(o);
        }
    } else {
        return errorResult(tr("'action' is list, create, switch, rename, delete or merge."));
    }
    if (!r.ok()) return errorResult(tr("git could not %1: %2").arg(action, r.error()));
    QJsonObject o = gitState(root);
    if (action == QLatin1String("merge") && !r.out.trimmed().isEmpty()) o.insert(QStringLiteral("git said"), r.out.trimmed().section(QLatin1Char('\n'), 0, 2));
    if (action == QLatin1String("switch") || action == QLatin1String("merge")) reloadGitChanged(root, &o);
    return jsonResult(o);
}

void QucsControl::gitRemote(const QJsonObject& args, const Done& done)
{
    const QString action = args.value(QLatin1String("action")).toString(QStringLiteral("list"));
    const int timeout = std::clamp(args.value(QLatin1String("timeout")).toInt(300), 5, 1800) * 1000;
    QString error;
    const QString root = gitRootOf(args.value(QLatin1String("path")).toString(), &error);
    if (root.isEmpty()) {
        done(errorResult(error));
        return;
    }
    if (action == QLatin1String("list")) {
        QJsonArray list;
        for (const git::Remote& r : git::remotes(root))
            list.append(QJsonObject{{QStringLiteral("name"), r.name}, {QStringLiteral("fetch"), r.fetchUrl}, {QStringLiteral("push"), r.pushUrl}});
        done(jsonResult(QJsonObject{{QStringLiteral("repository"), native(root)}, {QStringLiteral("remotes"), list}}));
        return;
    }
    if (action == QLatin1String("add") || action == QLatin1String("remove")) {
        const QString name = args.value(QLatin1String("name")).toString().trimmed();
        const QString url = args.value(QLatin1String("url")).toString().trimmed();
        if (name.isEmpty() || (action == QLatin1String("add") && url.isEmpty())) {
            done(errorResult(action == QLatin1String("add") ? tr("A remote needs a 'name' and a 'url'.") : tr("Which remote? ('name')")));
            return;
        }
        const git::Result r = action == QLatin1String("add") ? git::addRemote(root, name, url) : git::removeRemote(root, name);
        done(r.ok() ? jsonResult(gitState(root)) : errorResult(tr("git could not %1 the remote: %2").arg(action, r.error())));
        return;
    }
    QStringList network;
    if (action == QLatin1String("fetch")) {
        const QString remote = args.value(QLatin1String("remote")).toString().trimmed();
        network = git::fetchArgs(remote);
        if (network.isEmpty()) {
            done(errorResult(tr("%1 is no remote's name (it begins with '-').").arg(remote)));
            return;
        }
    } else if (action == QLatin1String("pull")) {
        network = git::pullArgs();
    } else if (action == QLatin1String("push")) {
        QString why;
        network = git::pushArgs(git::read(root), &why, args.value(QLatin1String("tags")).toBool());
        if (network.isEmpty()) {
            done(errorResult(why));
            return;
        }
    } else {
        done(errorResult(tr("'action' is list, fetch, pull, push, add or remove.")));
        return;
    }
    gitJob(root, network, timeout, {}, [this, root, action](const git::Result& r) -> QJsonObject {
        if (!r.ok()) {
            if (r.exitCode < 0) return errorResult(tr("git's %1 took too long, and was stopped ('timeout' gives it more).").arg(action));
            return errorResult(tr("git could not %1: %2").arg(action, r.error()));
        }
        QJsonObject o = gitState(root);
        o.insert(action, QStringLiteral("done"));
        if (action == QLatin1String("pull")) reloadGitChanged(root, &o);
        return jsonResult(o);
    }, done);
}

QJsonObject QucsControl::gitStash(const QJsonObject& args)
{
    const QString action = args.value(QLatin1String("action")).toString(QStringLiteral("list"));
    const int index = std::max(0, args.value(QLatin1String("index")).toInt(0));
    QString error;
    const QString root = gitRootOf(args.value(QLatin1String("path")).toString(), &error);
    if (root.isEmpty()) return errorResult(error);
    if (action == QLatin1String("list")) {
        QJsonArray list;
        for (const git::Stash& s : git::stashes(root))
            list.append(QJsonObject{{QStringLiteral("index"), s.index}, {QStringLiteral("message"), s.message}, {QStringLiteral("date"), s.date.toString(Qt::ISODate)}});
        return jsonResult(QJsonObject{{QStringLiteral("repository"), native(root)}, {QStringLiteral("stashes"), list}});
    }
    if (action == QLatin1String("show")) return textResult(cut(git::showStash(root, index)));
    git::Result r;
    if (action == QLatin1String("push")) {
        r = git::stash(root, args.value(QLatin1String("message")).toString().trimmed(), args.value(QLatin1String("untracked")).toBool(true));
    } else if (action == QLatin1String("apply") || action == QLatin1String("pop")) {
        r = git::applyStash(root, index, action == QLatin1String("pop"));
    } else if (action == QLatin1String("drop")) {
        r = git::dropStash(root, index);
    } else {
        return errorResult(tr("'action' is list, push, apply, pop, show or drop."));
    }
    if (!r.ok() && (action == QLatin1String("apply") || action == QLatin1String("pop"))
        && (r.out.contains(QLatin1String("CONFLICT")) || r.err.contains(QLatin1String("CONFLICT")))) {
        QJsonObject o = gitState(root);
        o.insert(QStringLiteral("stash"), tr("brought back in conflict (and kept: drop it once resolved) - git_resolve takes a side "
                                             "or shows both; an edited file is marked resolved with git_stage"));
        o.insert(QStringLiteral("git said"), r.error());
        reloadGitChanged(root, &o);
        return jsonResult(o);
    }
    if (!r.ok()) return errorResult(tr("git could not %1 the stash: %2").arg(action, r.error()));
    QJsonObject o = gitState(root);
    if (r.out.contains(QLatin1String("No local changes to save"))) o.insert(QStringLiteral("stash"), tr("nothing to stash: no changes"));
    if (action != QLatin1String("drop")) reloadGitChanged(root, &o);
    return jsonResult(o);
}

QJsonObject QucsControl::gitTag(const QJsonObject& args)
{
    const QString action = args.value(QLatin1String("action")).toString(QStringLiteral("list"));
    const QString name = args.value(QLatin1String("name")).toString().trimmed();
    QString error;
    const QString root = gitRootOf(args.value(QLatin1String("path")).toString(), &error);
    if (root.isEmpty()) return errorResult(error);
    if (action == QLatin1String("list"))
        return jsonResult(QJsonObject{{QStringLiteral("repository"), native(root)}, {QStringLiteral("tags"), QJsonArray::fromStringList(git::tags(root))}});
    if (name.isEmpty()) return errorResult(tr("Which tag? ('name')"));
    git::Result r;
    if (action == QLatin1String("create"))
        r = git::createTag(root, name, args.value(QLatin1String("message")).toString().trimmed(), args.value(QLatin1String("commit")).toString().trimmed());
    else if (action == QLatin1String("delete"))
        r = git::deleteTag(root, name);
    else
        return errorResult(tr("'action' is list, create or delete."));
    if (!r.ok()) return errorResult(tr("git could not %1 the tag: %2").arg(action, r.error()));
    return jsonResult(QJsonObject{{QStringLiteral("repository"), native(root)}, {QStringLiteral("tags"), QJsonArray::fromStringList(git::tags(root))}});
}

QJsonObject QucsControl::gitCommitAction(const QJsonObject& args)
{
    const QString commit = args.value(QLatin1String("commit")).toString().trimmed();
    const QString action = args.value(QLatin1String("action")).toString();
    const QString mode = args.value(QLatin1String("mode")).toString(QStringLiteral("mixed"));
    if (commit.isEmpty()) return errorResult(tr("Which commit? ('commit')"));
    QString error;
    const QString root = gitRootOf(args.value(QLatin1String("path")).toString(), &error);
    if (root.isEmpty()) return errorResult(error);
    git::Result r;
    if (action == QLatin1String("revert")) r = git::revert(root, commit);
    else if (action == QLatin1String("cherry_pick")) r = git::cherryPick(root, commit);
    else if (action == QLatin1String("check_out")) r = git::checkOutCommit(root, commit);
    else if (action == QLatin1String("reset")) r = git::reset(root, commit, mode);
    else return errorResult(tr("'action' is revert, cherry_pick, check_out or reset."));
    if (!r.ok()) {
        if (r.out.contains(QLatin1String("CONFLICT")) || r.err.contains(QLatin1String("CONFLICT")) || r.err.contains(QLatin1String("conflict"))) {
            QJsonObject o = gitState(root);
            o.insert(action, tr("conflicts: resolve the files in 'conflicts' - git_resolve takes a side whole or shows both; an "
                                "edited file is marked resolved with git_stage - then git_commit; or git_abort"));
            o.insert(QStringLiteral("git said"), r.error());
            reloadGitChanged(root, &o);
            return jsonResult(o);
        }
        reloadGitChanged(root, nullptr);
        return errorResult(tr("git could not %1: %2").arg(action, r.error()));
    }
    QJsonObject o = gitState(root);
    reloadGitChanged(root, &o);
    return jsonResult(o);
}

QJsonObject QucsControl::gitAbort(const QJsonObject& args)
{
    QString error;
    const QString root = gitRootOf(args.value(QLatin1String("path")).toString(), &error);
    if (root.isEmpty()) return errorResult(error);
    const git::Repository repo = git::read(root);
    if (repo.operation.isEmpty()) return errorResult(tr("Nothing is under way in %1 to give up.").arg(native(root)));
    const git::Result r = git::abort(root, repo.operation);
    if (!r.ok()) return errorResult(tr("git could not give the %1 up: %2").arg(repo.operation, r.error()));
    QJsonObject o = gitState(root);
    reloadGitChanged(root, &o);
    return jsonResult(o);
}

QJsonObject QucsControl::gitResolve(const QJsonObject& args)
{
    const QString path = args.value(QLatin1String("path")).toString().trimmed();
    const QString action = args.value(QLatin1String("action")).toString(QStringLiteral("show"));
    if (path.isEmpty()) return errorResult(tr("Which file in conflict? ('path')"));
    if (action != QLatin1String("show") && action != QLatin1String("take_ours") && action != QLatin1String("take_theirs")
        && action != QLatin1String("open"))
        return errorResult(tr("'action' is show, take_ours, take_theirs or open."));
    QString error;
    const QString root = gitRootOf(path, &error);
    if (root.isEmpty()) return errorResult(error);
    const QString file = gitPath(path);
    const git::ConflictVersions v = git::conflictVersions(root, file);
    if (!v.inConflict) return errorResult(tr("%1 is not in conflict (git_status lists the files that are).").arg(native(file)));
    if (action == QLatin1String("show")) {
        const auto text = [](const std::optional<QString>& t) { return t.has_value() ? QJsonValue(cut(*t)) : QJsonValue(QJsonValue::Null); };
        QJsonObject o{{QStringLiteral("path"), native(file)},
                      {QStringLiteral("mine"), text(v.ours)},
                      {QStringLiteral("theirs"), text(v.theirs)},
                      {QStringLiteral("base"), text(v.base)},
                      {QStringLiteral("marks in the file"), git::hasConflictMarkers(file)}};
        if (git::isSchematicFile(file)) o.insert(QStringLiteral("changes"), git::schematicConflictChanges(v));
        if (a_app->keptInConflict(file)) o.insert(QStringLiteral("tab"), tr("open, showing mine (the version from before)"));
        return jsonResult(o);
    }
    if (action == QLatin1String("open")) {
        const QStringList opened = git::Commands::instance()->openConflictVersions(root, file);
        if (opened.isEmpty()) return errorResult(tr("The versions of %1 could not be written.").arg(native(file)));
        QJsonArray list;
        for (const QString& o : opened) list.append(native(o));
        return jsonResult(QJsonObject{{QStringLiteral("opened"), list},
                                      {QStringLiteral("note"), tr("written beside it, untracked: delete them once it is resolved")}});
    }
    const git::Result r = git::resolve(root, file, action == QLatin1String("take_ours") ? QStringLiteral("ours") : QStringLiteral("theirs"));
    if (!r.ok()) return errorResult(tr("git could not resolve it: %1").arg(r.error()));
    QJsonObject o = gitState(root);
    reloadGitChanged(root, &o);
    return jsonResult(o);
}

QJsonObject QucsControl::gitInit(const QJsonObject& args)
{
    if (git::program().isEmpty()) return errorResult(tr("git is not installed (nor where Homebrew puts it): Qucs-S's git needs it."));
    const QString given = args.value(QLatin1String("path")).toString().trimmed();
    QString folder = given.isEmpty() ? QString() : gitPath(given);
    if (folder.isEmpty() && !a_app->ProjName.isEmpty()) folder = QucsSettings.QucsWorkDir.absolutePath();
    if (folder.isEmpty()) return errorResult(tr("Which folder? ('path'; the open project's when one is open)"));
    if (!QFileInfo(folder).isDir()) return errorResult(tr("There is no folder %1.").arg(native(folder)));
    if (const QString root = git::topLevel(folder); root == QDir::cleanPath(folder))
        return errorResult(tr("%1 is a git repository already.").arg(native(folder)));
    const git::Result r = git::init(folder);
    if (!r.ok()) return errorResult(tr("git could not make a repository: %1").arg(r.error()));
    git::Tracker::instance()->refresh(folder);
    return jsonResult(gitState(folder));
}

void QucsControl::gitClone(const QJsonObject& args, const Done& done)
{
    if (git::program().isEmpty()) {
        done(errorResult(tr("git is not installed (nor where Homebrew puts it): Qucs-S's git needs it.")));
        return;
    }
    const QString url = args.value(QLatin1String("url")).toString().trimmed();
    if (url.isEmpty() || url.startsWith(QLatin1Char('-'))) {
        done(errorResult(url.isEmpty() ? tr("Which repository? ('url')") : tr("%1 is no repository's URL (it begins with '-').").arg(url)));
        return;
    }
    QString target = args.value(QLatin1String("path")).toString().trimmed();
    if (target.isEmpty()) {
        QString name = url.section(QLatin1Char('/'), -1).section(QLatin1Char(':'), -1);
        if (name.endsWith(QLatin1String(".git"))) name.chop(4);
        target = QucsSettings.qucsWorkspaceDir.filePath(name.isEmpty() ? QStringLiteral("repository") : name);
    } else {
        target = absolute(target);
    }
    target = QDir::cleanPath(target);
    if (QFileInfo::exists(target) && !QDir(target).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) {
        done(errorResult(tr("%1 is there already, and not empty: give another 'path'.").arg(native(target))));
        return;
    }
    const QString parent = QFileInfo(target).absolutePath();
    if (!QDir().mkpath(parent)) {
        done(errorResult(tr("The folder %1 cannot be made.").arg(native(parent))));
        return;
    }
    const int timeout = std::clamp(args.value(QLatin1String("timeout")).toInt(600), 5, 3600) * 1000;
    gitJob(parent, git::cloneArgs(url, target), timeout, {}, [this, target](const git::Result& r) -> QJsonObject {
        if (!r.ok()) {
            if (r.exitCode < 0) return errorResult(tr("The clone took too long, and was stopped ('timeout' gives it more)."));
            return errorResult(tr("git could not clone it: %1").arg(r.error()));
        }
        git::Tracker::instance()->refresh(target);
        QJsonObject o = gitState(target);
        o.insert(QStringLiteral("cloned into"), native(target));
        return jsonResult(o);
    }, done);
}

QJsonObject QucsControl::gitIgnore(const QJsonObject& args)
{
    const QString path = args.value(QLatin1String("path")).toString().trimmed();
    if (path.isEmpty()) return errorResult(tr("Which file or folder? ('path')"));
    QString error;
    const QString root = gitRootOf(path, &error);
    if (root.isEmpty()) return errorResult(error);
    const QString file = gitPath(path);
    if (!git::ignore(root, file, &error)) return errorResult(tr("It could not be added to .gitignore: %1").arg(error));
    if (args.value(QLatin1String("untrack")).toBool()) {
        const bool tracked = git::run(root, {QStringLiteral("ls-files"), QStringLiteral("--error-unmatch"), QStringLiteral("--"),
                                             git::relativePath(root, file)},
                                      true, 15000)
                                 .ok();
        if (tracked) {
            const git::Result r = git::untrack(root, {file});
            if (!r.ok()) return errorResult(tr("It is in .gitignore; git could not stop following it: %1").arg(r.error()));
        }
    }
    return jsonResult(gitState(root));
}
