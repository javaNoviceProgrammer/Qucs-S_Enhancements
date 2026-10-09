/*
 * qucs_git.cpp - the Git menu: on the repository of the document in front
 *                (else of the File Browser's folder, the project's) - the
 *                commit, its changes, the file's, the history, branches,
 *                stashes, tags, the network; a repository made or cloned
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "qucs.h"

#include "filebrowser.h"
#include "gitui.h"
#include "main.h"
#include "misc.h"
#include "qucsdoc.h"
#include "settings.h"

#include <QAction>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>
#include <QTabWidget>

using qucs_s::git::Commands;
using qucs_s::git::Tracker;

QString QucsApp::gitFile()
{
  QucsDoc *doc = DocumentTab != nullptr && DocumentTab->count() > 0 ? getDoc() : nullptr;
  if (doc == nullptr || doc->getDocName().isEmpty() || !QFileInfo(doc->getDocName()).isFile()) return {};
  return QFileInfo(doc->getDocName()).absoluteFilePath();
}

QString QucsApp::gitFolder()
{
  if (const QString file = gitFile(); !file.isEmpty()) return QFileInfo(file).absolutePath();
  if (fileBrowser != nullptr && QFileInfo(fileBrowser->location()).isDir()) return fileBrowser->location();
  if (!ProjName.isEmpty()) return QucsSettings.QucsWorkDir.absolutePath();
  return QucsSettings.qucsWorkspaceDir.absolutePath();
}

QString QucsApp::gitRoot()
{
  return qucs_s::git::topLevel(gitFolder());
}

void QucsApp::initGitMenu()
{
  gitMenu = new QMenu(tr("&Git"), this);
  gitMenu->setObjectName(QStringLiteral("gitMenu"));
  gitMenu->setToolTipsVisible(true);
  Commands *git = Commands::instance();
  // Each on the repository found as it is chosen: none, and it says how
  // to make one.
  const auto onRoot = [this](const std::function<void(const QString &)> &work) {
    return [this, work] {
      const QString root = gitRoot();
      if (root.isEmpty()) {
        misc::reportError(tr("%1 is in no git repository: Git > Create Repository... makes one, Git > Clone Repository... "
                             "brings one.").arg(QDir::toNativeSeparators(gitFolder())));
        return;
      }
      work(root);
    };
  };
  const auto onFile = [this](const std::function<void(const QString &, const QString &)> &work) {
    return [this, work] {
      const QString file = gitFile();
      const QString root = qucs_s::git::topLevel(file);
      if (file.isEmpty() || root.isEmpty()) {
        misc::reportError(tr("The document in front is no file of a git repository."));
        return;
      }
      work(root, file);
    };
  };
  const auto add = [this](const char *name, const QString &text, const std::function<void()> &work, const QString &tip = {}) {
    QAction *a = gitMenu->addAction(text, this, work);
    a->setObjectName(QLatin1String(name));
    if (!tip.isEmpty()) a->setToolTip(tip);
    return a;
  };
  add("gitCommit", tr("Commit..."), onRoot([git](const QString &r) { git->commit(r); }),
      tr("What is staged and what is not, each file's changes, the message: commit (and push)"));
  add("gitShowAllChanges", tr("Show All Changes"), onRoot([git](const QString &r) { git->showDiff(r, {}, qucs_s::git::DiffOf::Head); }));
  gitMenu->addSeparator();
  add("gitFileChanges", tr("Show Changes in This File"),
      onFile([git](const QString &r, const QString &f) { git->showDiff(r, f, qucs_s::git::DiffOf::Head); }));
  add("gitStageFile", tr("Stage This File"), onFile([git](const QString &r, const QString &f) { git->stage(r, {f}); }));
  add("gitUnstageFile", tr("Unstage This File"), onFile([git](const QString &r, const QString &f) { git->unstage(r, {f}); }));
  add("gitDiscardFile", tr("Discard Changes in This File..."), onFile([git](const QString &r, const QString &f) { git->discard(r, {f}); }));
  add("gitFileHistory", tr("History of This File"), onFile([git](const QString &r, const QString &f) { git->showHistory(r, f); }));
  add("gitBlame", tr("Blame This File"), onFile([git](const QString &r, const QString &f) { git->showBlame(r, f); }),
      tr("Who last changed each line, and in which commit"));
  gitMenu->addSeparator();
  add("gitStageAll", tr("Stage All"), onRoot([git](const QString &r) { git->stage(r, {}); }));
  add("gitUnstageAll", tr("Unstage All"), onRoot([git](const QString &r) { git->unstage(r, {}); }));
  add("gitDiscardAll", tr("Discard All Changes..."), onRoot([git](const QString &r) { git->discard(r, {}); }));
  gitMenu->addSeparator();
  add("gitHistory", tr("History"), onRoot([git](const QString &r) { git->showHistory(r); }),
      tr("The commits, newest first: check one out, branch or tag there, revert, cherry-pick, reset"));
  gitMenu->addSeparator();
  add("gitFetch", tr("Fetch"), onRoot([git](const QString &r) { git->fetch(r); }), tr("What the remotes have, without changing your files"));
  add("gitPull", tr("Pull"), onRoot([git](const QString &r) { git->pull(r); }), tr("The branch's upstream merged into it"));
  add("gitPush", tr("Push"), onRoot([git](const QString &r) { git->push(r); }),
      tr("The branch's commits to its upstream (the remote's branch made its upstream the first time)"));
  gitMenu->addSeparator();
  QMenu *branches = gitMenu->addMenu(tr("Switch to Branch"));
  branches->setObjectName(QStringLiteral("gitSwitchMenu"));
  connect(branches, &QMenu::aboutToShow, this, [this, branches] {
    branches->clear();
    if (const QString root = gitRoot(); !root.isEmpty()) Commands::instance()->fillBranchMenu(branches, root);
  });
  add("gitNewBranch", tr("New Branch..."), onRoot([git](const QString &r) { git->newBranch(r); }));
  add("gitRenameBranch", tr("Rename Branch..."), onRoot([git](const QString &r) { git->renameBranch(r); }));
  add("gitDeleteBranch", tr("Delete Branch..."), onRoot([git](const QString &r) { git->deleteBranch(r); }));
  add("gitMerge", tr("Merge into the Current Branch..."), onRoot([git](const QString &r) { git->mergeInto(r); }));
  gitMenu->addSeparator();
  add("gitStash", tr("Stash Changes..."), onRoot([git](const QString &r) { git->stashChanges(r); }),
      tr("Every change put aside, the files back to the last commit: Stashes brings them back"));
  QMenu *stashes = gitMenu->addMenu(tr("Stashes"));
  stashes->setObjectName(QStringLiteral("gitStashesMenu"));
  connect(stashes, &QMenu::aboutToShow, this, [this, stashes] {
    stashes->clear();
    if (const QString root = gitRoot(); !root.isEmpty()) Commands::instance()->fillStashMenu(stashes, root);
  });
  add("gitNewTag", tr("New Tag..."), onRoot([git](const QString &r) { git->newTag(r); }));
  add("gitAbort", tr("Abort the Merge"), onRoot([git](const QString &r) { git->abortOperation(r); }));
  gitMenu->addSeparator();
  add("gitIgnoreFile", tr("Add This File to .gitignore"), onFile([git](const QString &r, const QString &f) { git->ignore(r, f); }));
  add("gitEditIgnore", tr("Edit .gitignore"), onRoot([this](const QString &r) {
        const QString file = QDir(r).filePath(QStringLiteral(".gitignore"));
        if (!QFileInfo::exists(file)) {
          QFile made(file);
          if (!made.open(QIODevice::WriteOnly)) {
            misc::reportError(tr("%1 cannot be made: %2").arg(QDir::toNativeSeparators(file), made.errorString()));
            return;
          }
        }
        openAsText(file);
      }));
  add("gitRemotes", tr("Remotes..."), onRoot([git](const QString &r) { git->editRemotes(r); }));
  gitMenu->addSeparator();
  add("gitInit", tr("Create Repository..."), [this, git] { git->createRepository(gitFolder()); },
      tr("The folder of the document in front (else the File Browser's, the project's) made a git repository"));
  add("gitClone", tr("Clone Repository..."), [this, git] {
        git->cloneRepository(fileBrowser != nullptr && QFileInfo(fileBrowser->location()).isDir()
                                 ? fileBrowser->location() : QucsSettings.qucsWorkspaceDir.absolutePath());
      }, tr("A repository from a URL, into the File Browser's folder"));
  add("gitRefresh", tr("Refresh Git Status"), [] { Tracker::instance()->refresh(); });
  connect(gitMenu, &QMenu::aboutToShow, this, &QucsApp::updateGitMenu);

  // Left of Help.
  const QList<QAction *> bar = menuBar()->actions();
  qsizetype at = bar.indexOf(helpMenu->menuAction());
  if (at > 0 && bar.at(at - 1)->isSeparator()) --at;
  if (at >= 0) menuBar()->insertMenu(bar.at(at), gitMenu);
  else menuBar()->addMenu(gitMenu);

  // What the commands ask of the window.
  connect(git, &Commands::openRequested, this, [this](const QString &path) { gotoPage(path); });
  connect(git, &Commands::showFolderRequested, this, [this](const QString &path) {
    if (fileBrowser == nullptr) return;
    fileBrowser->setLocation(path);
    if (TabView != nullptr) TabView->setCurrentWidget(fileBrowser);
  });
  connect(git, &Commands::message, this, [this](const QString &text) { statusBar()->showMessage(text, 5000); });
  updateGitMenu();
}

void QucsApp::updateGitMenu()
{
  if (gitMenu == nullptr) return;
  const QString file = gitFile();
  const QString root = gitRoot();
  const qucs_s::git::Repository *repo = root.isEmpty() ? nullptr : Tracker::instance()->fresh(root);
  const qucs_s::git::Repository r = repo != nullptr ? *repo : qucs_s::git::Repository();
  const bool inRepo = r.valid;
  const bool fileIn = inRepo && !file.isEmpty() && qucs_s::git::topLevel(file) == root;
  const qucs_s::git::Entry *e = fileIn ? r.entryOf(qucs_s::git::relativePath(root, file)) : nullptr;
  const bool fileChanged = e != nullptr && !e->ignored;
  const bool head = inRepo && !r.head.isEmpty();
  const auto set = [this](const char *name, bool on) {
    if (QAction *a = gitMenu->findChild<QAction *>(QLatin1String(name))) a->setEnabled(on);
  };
  for (QAction *a : gitMenu->actions())
    if (a->menu() != nullptr) a->setEnabled(inRepo);
  set("gitCommit", inRepo);
  set("gitShowAllChanges", inRepo && r.changedCount() > 0);
  set("gitFileChanges", fileChanged);
  set("gitStageFile", fileChanged && e->hasUnstaged());
  set("gitUnstageFile", fileChanged && e->hasStaged());
  set("gitDiscardFile", fileChanged);
  set("gitFileHistory", fileIn && head && (e == nullptr || !e->untracked));
  set("gitBlame", fileIn && head && (e == nullptr || (!e->untracked && !e->ignored)));
  set("gitStageAll", inRepo && r.changedCount() > r.stagedCount());
  set("gitUnstageAll", inRepo && r.stagedCount() > 0);
  set("gitDiscardAll", inRepo && r.changedCount() > 0);
  set("gitHistory", head);
  for (const char *name : {"gitFetch", "gitPull", "gitPush", "gitNewBranch", "gitRenameBranch", "gitDeleteBranch", "gitMerge",
                           "gitNewTag", "gitRemotes", "gitEditIgnore"})
    set(name, inRepo);
  set("gitNewBranch", head);
  set("gitNewTag", head);
  set("gitStash", head && r.changedCount() > 0);
  set("gitIgnoreFile", fileIn && (e == nullptr || !e->ignored));
  if (QAction *a = gitMenu->findChild<QAction *>(QStringLiteral("gitAbort"))) {
    a->setEnabled(inRepo && !r.operation.isEmpty());
    a->setText(r.operation.isEmpty() ? tr("Abort the Merge") : tr("Abort the %1").arg(r.operation));
  }
  set("gitInit", !inRepo);
}
