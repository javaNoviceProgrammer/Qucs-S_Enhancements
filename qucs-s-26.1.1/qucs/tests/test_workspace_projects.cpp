/*
 * The Projects panel's menu: Switch Workspace, Import Project (a project
 * folder copied into the workspace), Link Project (a link to it in the
 * workspace, nothing copied, and it acts as a project there), Unlink
 * Project (the link of the row right-clicked goes), Open Project (the
 * project right-clicked) and Close Project.
 * Deleting or unlinking a linked project removes the link, never the
 * project it leads to. And the workspace changed in the settings is the
 * one the panel lists.
 */
#include <QtTest>
#include <QDialog>
#include <QInputDialog>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>

#ifndef Q_OS_WIN
#include <sys/stat.h>
#endif

#include "config.h"
#include "qucs.h"
#include "schematic.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "settings.h"
#include "workspace.h"
#include "dialogs/qucssettingsdialog.h"
#include "dialogs/savedialog.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

using namespace qucs_s::workspace;

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

const QByteArray kSchematic =
    "<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n</Properties>\n<Symbol>\n</Symbol>\n"
    "<Components>\n</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n";

bool write(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

QByteArray read(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// Runs fn and answers the dialog it brings up with answer(dialog).
void answering(const std::function<void()>& fn, const std::function<bool(QWidget*)>& answer)
{
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&] {
        if (QWidget* modal = QApplication::activeModalWidget())
            if (answer(modal)) timer.stop();
    });
    timer.start(20);
    fn();
}

bool clickYes(QWidget* w)
{
    auto* box = qobject_cast<QMessageBox*>(w);
    if (box == nullptr) return false;
    // (A box with no Yes - a message where a question was meant - closed:
    // the test fails on what came of it, rather than crashing.)
    if (QAbstractButton* yes = box->button(QMessageBox::Yes)) yes->click();
    else box->accept();
    return true;
}

// The Projects panel's menu, right-clicked at \a pos: each entry's object
// name and whether it can be chosen. With \a choose, that entry is chosen
// as the keyboard would (what it asks is up to the caller).
QList<QPair<QString, bool>> projectsMenu(QucsApp& app, const QPoint& pos, const QString& choose = QString())
{
    QList<QPair<QString, bool>> shown;
    // The menu is a popup, not modal: look for it while it is open.
    QTimer::singleShot(50, [&] {
        for (QWidget* w : QApplication::topLevelWidgets())
            if (auto* menu = qobject_cast<QMenu*>(w); menu && menu->isVisible()) {
                QAction* chosen = nullptr;
                for (QAction* a : menu->actions()) {
                    if (a->isSeparator()) continue;
                    shown << qMakePair(a->objectName(), a->isEnabled());
                    if (!choose.isEmpty() && a->objectName() == choose) chosen = a;
                }
                // (One that cannot be chosen: Return on it left the menu
                // open, and the suite waited for ever - on CI.)
                if (chosen != nullptr && !chosen->isEnabled()) {
                    qWarning().noquote() << choose << "cannot be chosen there: the menu is closed";
                    chosen = nullptr;
                }
                if (chosen == nullptr) {
                    menu->close();
                } else {
                    menu->setActiveAction(chosen);
                    QTest::keyClick(menu, Qt::Key_Return);
                }
                return;
            }
    });
    emit app.projectsView()->customContextMenuRequested(pos);
    return shown;
}

bool canChoose(const QList<QPair<QString, bool>>& menu, const QString& name)
{
    for (const auto& [entry, enabled] : menu)
        if (entry == name) return enabled;
    return false;
}

// The Projects panel's row of \a folder, invalid while it has not read
// its folder. (Taken when it is used: the panel's model sorts again as it
// learns more of its files, and an index kept goes stale.)
QModelIndex indexOf(QucsApp& app, const QString& folder)
{
    QListView* view = app.projectsView();
    for (int r = 0; r < view->model()->rowCount(view->rootIndex()); ++r) {
        const QModelIndex index = view->model()->index(r, 0, view->rootIndex());
        if (index.data().toString() == folder) return index;
    }
    return QModelIndex();
}

// Where the Projects panel shows \a folder (once it has read its folder).
QPoint rowOf(QucsApp& app, const QString& folder)
{
    const QModelIndex index = indexOf(app, folder);
    return index.isValid() ? app.projectsView()->visualRect(index).center() : QPoint(-1, -1);
}

} // namespace

class TestWorkspaceProjects : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString workspace;   // the workspace of each test
    QString source;      // a project outside it: amp_prj

    // A fresh workspace and a project elsewhere with a schematic, a
    // subfolder, a hidden file and (not on Windows) a link inside.
    void fresh(const QString& tag)
    {
        workspace = dir.filePath(tag + "/workspace");
        source = dir.filePath(tag + "/elsewhere/amp_prj");
        QVERIFY(QDir().mkpath(workspace));
        QVERIFY(write(source + "/amp.sch", kSchematic));
        QVERIFY(write(source + "/sub/lib.sch", kSchematic));
        QVERIFY(write(source + "/.hidden", "h"));
#ifndef Q_OS_WIN
        QVERIFY(QFile::link("amp.sch", source + "/same.sch"));   // relative
#endif
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
    }

    void importCopiesTheProject()
    {
        fresh("import");
        const Result r = importProject(source, workspace);
        QVERIFY2(r.status == Result::Done, qPrintable(r.message));
        QCOMPARE(r.path, QDir(workspace).absoluteFilePath("amp_prj"));
        QVERIFY(!isLink(r.path));
        QCOMPARE(read(r.path + "/amp.sch"), kSchematic);
        QCOMPARE(read(r.path + "/sub/lib.sch"), kSchematic);
        QCOMPARE(read(r.path + "/.hidden"), QByteArray("h"));
#ifndef Q_OS_WIN
        QVERIFY(isLink(r.path + "/same.sch"));   // a link as a link, still pointing next door
        QCOMPARE(QFileInfo(r.path + "/same.sch").canonicalFilePath(), QFileInfo(r.path + "/amp.sch").canonicalFilePath());
#endif
        // A copy: the source is untouched, and a change to one is not the other's.
        QVERIFY(write(r.path + "/amp.sch", "changed"));
        QCOMPARE(read(source + "/amp.sch"), kSchematic);

        // Again: the workspace has it; another name is free.
        const Result again = importProject(source, workspace);
        QCOMPARE(again.status, Result::Exists);
        QCOMPARE(again.path, r.path);
        QCOMPARE(freeName(workspace, "amp_prj"), QStringLiteral("amp_2_prj"));
        const Result renamed = importProject(source, workspace, "amp_2_prj");
        QVERIFY2(renamed.status == Result::Done, qPrintable(renamed.message));
        QCOMPARE(freeName(workspace, "amp_prj"), QStringLiteral("amp_3_prj"));
    }

    void whatCannotComeIn()
    {
        fresh("refused");
        const auto invalid = [](const Result& r) { return r.status == Result::Invalid && !r.message.isEmpty(); };
        QVERIFY(invalid(check(dir.filePath("refused/nothing_prj"), workspace)));      // missing
        QVERIFY(write(dir.filePath("refused/file_prj"), "x"));
        QVERIFY(invalid(check(dir.filePath("refused/file_prj"), workspace)));         // a file
        QVERIFY(QDir().mkpath(dir.filePath("refused/elsewhere/plain")));
        QVERIFY(invalid(check(dir.filePath("refused/elsewhere/plain"), workspace)));  // no _prj
        QVERIFY(invalid(check(source, workspace, "amp")));                           // a name without it
        QVERIFY(invalid(check(source, workspace, "a/b_prj")));
        QVERIFY(invalid(check(source, dir.filePath("refused/no-workspace"))));
        // In the workspace already.
        QVERIFY(QDir().mkpath(workspace + "/here_prj"));
        QVERIFY(invalid(check(workspace + "/here_prj", workspace)));
        // A project holding the workspace would copy into itself.
        const QString outer = dir.filePath("refused/outer_prj");
        QVERIFY(QDir().mkpath(outer + "/ws"));
        QVERIFY(invalid(check(outer, outer + "/ws")));
        QVERIFY(invalid(importProject(outer, outer + "/ws")));
        QVERIFY(!QFileInfo::exists(outer + "/ws/outer_prj"));
        QCOMPARE(check(source, workspace).status, Result::Done);
    }

    // A folder as the macOS dialog gives it, "…/amp_prj/", or typed with
    // "//" or "./" in it: the same folder, and the same project.
    void aFolderWrittenAnyWay()
    {
        fresh("spelled");
        const QString slash = QDir::toNativeSeparators(source) + QDir::separator();
        const Result copied = importProject(slash, workspace);
        QVERIFY2(copied.status == Result::Done, qPrintable(copied.message));
        QCOMPARE(copied.path, QDir(workspace).absoluteFilePath("amp_prj"));
        QCOMPARE(read(copied.path + "/amp.sch"), kSchematic);

        const Result linked = linkProject(source + "//", workspace, "amp_link_prj");
        QVERIFY2(linked.status == Result::Done, qPrintable(linked.message));
        QVERIFY(isLink(linked.path));
        QCOMPARE(QFileInfo(linkTarget(linked.path)).canonicalFilePath(), QFileInfo(source).canonicalFilePath());

        // The workspace written with a slash too: the path has none twice.
        const Result dotted = importProject(source + "/./", workspace + "/", "amp_dot_prj");
        QVERIFY2(dotted.status == Result::Done, qPrintable(dotted.message));
        QCOMPARE(dotted.path, QDir(workspace).absoluteFilePath("amp_dot_prj"));

        // What was refused is refused for what it is, not for the slash.
        const Result there = check(copied.path + "/", workspace);
        QCOMPARE(there.status, Result::Invalid);
        QVERIFY2(there.message.contains("in the workspace already"), qPrintable(there.message));
        QVERIFY2(there.message.endsWith("amp_prj is in the workspace already."), qPrintable(there.message));
        QCOMPARE(check(slash, workspace).status, Result::Exists);

        // From the application, as the menu's folder dialog hands it over.
        fresh("spelled-app");
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.switchWorkspace(workspace));
        QCOMPARE(app.bringProjectIn(source + "/", false), QDir(workspace).absoluteFilePath("amp_prj"));
        QString asked;
        QString linkedApp;
        answering([&] { linkedApp = app.bringProjectIn(source + "/", true); },
                  [&](QWidget* w) {
                      auto* input = qobject_cast<QInputDialog*>(w);
                      if (input == nullptr) return false;
                      asked = input->textValue();
                      input->accept();
                      return true;
                  });
        QCOMPARE(asked, QStringLiteral("amp_2"));   // offered after the folder's name, not ""
        QCOMPARE(linkedApp, QDir(workspace).absoluteFilePath("amp_2_prj"));
        QVERIFY(isLink(linkedApp));
    }

    void linkLinksTheProject()
    {
        fresh("link");
        const Result r = linkProject(source, workspace);
        QVERIFY2(r.status == Result::Done, qPrintable(r.message));
        QCOMPARE(r.path, QDir(workspace).absoluteFilePath("amp_prj"));
        QVERIFY(isLink(r.path));
        QCOMPARE(QFileInfo(linkTarget(r.path)).canonicalFilePath(), QFileInfo(source).canonicalFilePath());
        QVERIFY(QFileInfo(r.path).isDir());
        // Nothing copied: one project, seen from two places.
        QCOMPARE(read(r.path + "/sub/lib.sch"), kSchematic);
        QVERIFY(write(r.path + "/amp.sch", "through the link"));
        QCOMPARE(read(source + "/amp.sch"), QByteArray("through the link"));
        QVERIFY(write(source + "/new.sch", kSchematic));
        QVERIFY(QFileInfo::exists(r.path + "/new.sch"));

        QCOMPARE(linkProject(source, workspace).status, Result::Exists);
        // The link itself is in the workspace already.
        QCOMPARE(linkProject(r.path, workspace, "again_prj").status, Result::Invalid);
        QCOMPARE(importProject(r.path, workspace, "again_prj").status, Result::Invalid);
        QVERIFY(!isLink(source));
        QVERIFY(linkTarget(source).isEmpty());
    }

    // The link goes, the project stays; a real folder is no link to remove.
    void removingALinkLeavesTheProject()
    {
        fresh("unlink");
        const Result r = linkProject(source, workspace);
        QVERIFY2(r.status == Result::Done, qPrintable(r.message));
        QString error;
        QVERIFY2(removeLink(r.path, &error), qPrintable(error));
        QVERIFY(!QFileInfo::exists(r.path) && !isLink(r.path));
        QCOMPARE(read(source + "/amp.sch"), kSchematic);
        QCOMPARE(read(source + "/sub/lib.sch"), kSchematic);

        QVERIFY(!removeLink(source, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(QFileInfo::exists(source + "/amp.sch"));
    }

    // The menu of the Projects panel, and the same actions in the Project
    // menu - but Unlink Project and Open Project, of the row right-clicked:
    // chosen only on a linked project, and on a project not open. Close
    // Project only with a project open (the Project menu's own is as it
    // was).
    void theMenu()
    {
        fresh("menu");
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.switchWorkspace(workspace));
        QCOMPARE(app.projectsView()->contextMenuPolicy(), Qt::CustomContextMenu);
        const QString link = app.bringProjectIn(source, true);
        QVERIFY(isLink(link));
        QVERIFY(QDir().mkpath(workspace + "/real_prj"));
        QVERIFY(QDir().mkpath(workspace + "/plain"));   // a folder, no project
        QTRY_VERIFY(rowOf(app, "real_prj").x() >= 0 && rowOf(app, "amp_prj").x() >= 0 && rowOf(app, "plain").x() >= 0);

        const auto onLink = projectsMenu(app, rowOf(app, "amp_prj"));
        QStringList shown;
        for (const auto& entry : onLink) shown << entry.first;
        QCOMPARE(shown, QStringList({"projSwitchWorkspace", "projImport", "projLink", "projUnlink", "projOpenRow", "projClose"}));
        QVERIFY(canChoose(onLink, "projUnlink"));
        QVERIFY(canChoose(onLink, "projOpenRow"));
        QVERIFY(!canChoose(onLink, "projClose"));   // no project open
        QVERIFY(canChoose(onLink, "projLink"));
        const auto onReal = projectsMenu(app, rowOf(app, "real_prj"));
        QVERIFY(!canChoose(onReal, "projUnlink"));
        QVERIFY(canChoose(onReal, "projOpenRow"));
        QVERIFY(!canChoose(projectsMenu(app, rowOf(app, "plain")), "projOpenRow"));
        if (rowOf(app, "..").x() >= 0) QVERIFY(!canChoose(projectsMenu(app, rowOf(app, "..")), "projOpenRow"));
        const QRect below = app.projectsView()->viewport()->rect();
        const auto onNothing = projectsMenu(app, QPoint(5, below.bottom() - 2));   // on no row
        QVERIFY(!canChoose(onNothing, "projUnlink"));
        QVERIFY(!canChoose(onNothing, "projOpenRow"));
        auto* close = app.findChild<QAction*>("projClose");
        QVERIFY(close != nullptr && close->isEnabled());   // the Project menu's, as it was

        // Chosen: the project right-clicked opens - not again once open.
        projectsMenu(app, rowOf(app, "real_prj"), "projOpenRow");
        QTRY_COMPARE(app.ProjName, QStringLiteral("real"));
        QCOMPARE(QDir::cleanPath(QucsSettings.QucsWorkDir.absolutePath()), QDir::cleanPath(workspace + "/real_prj"));
        const auto open = projectsMenu(app, rowOf(app, "real_prj"));
        QVERIFY(canChoose(open, "projClose"));
        QVERIFY(!canChoose(open, "projUnlink"));
        QVERIFY(!canChoose(open, "projOpenRow"));
        QVERIFY(canChoose(projectsMenu(app, rowOf(app, "amp_prj")), "projOpenRow"));   // another one
        // Close Project chosen: the open project closes.
        projectsMenu(app, rowOf(app, "amp_prj"), "projClose");
        QTRY_VERIFY(app.ProjName.isEmpty());
        QVERIFY(close->isEnabled());
        // A linked project opened from its row.
        projectsMenu(app, rowOf(app, "amp_prj"), "projOpenRow");
        QTRY_COMPARE(app.ProjName, QStringLiteral("amp"));
        app.slotMenuProjClose();

        for (const char* name : {"projSwitchWorkspace", "projImport", "projLink", "projClose"}) {
            auto* action = app.findChild<QAction*>(name);
            QVERIFY(action != nullptr);
            bool inMenuBar = false;
            for (QAction* top : app.menuBar()->actions())
                if (top->menu() != nullptr && top->menu()->actions().contains(action)) inMenuBar = true;
            QVERIFY2(inMenuBar, name);
        }
        QVERIFY(app.findChild<QAction*>("projUnlink") == nullptr);   // (the menu's own, gone with it)
        QVERIFY(app.findChild<QAction*>("projOpenRow") == nullptr);
    }

    // Unlink Project on a linked project's row: asked, the link goes, the
    // project stays where it is; No leaves it. A folder that is no link is
    // not unlinked.
    void unlinkingAProject()
    {
        fresh("unlink-menu");
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.switchWorkspace(workspace));
        const QString link = app.bringProjectIn(source, true);
        QVERIFY(isLink(link));
        QTRY_VERIFY(rowOf(app, "amp_prj").x() >= 0);

        QString question, details;
        const auto answer = [&](QMessageBox::StandardButton button) {
            return [&, button](QWidget* w) {
                auto* box = qobject_cast<QMessageBox*>(w);
                if (box == nullptr) return false;
                question = box->text();
                details = box->informativeText();
                box->button(button)->click();
                return true;
            };
        };
        answering([&] { projectsMenu(app, rowOf(app, "amp_prj"), "projUnlink"); }, answer(QMessageBox::No));
        QVERIFY2(question.contains("Remove the link") && question.contains("stay where they are")
                     && question.contains(QDir::toNativeSeparators(QFileInfo(source).canonicalFilePath())),
                 qPrintable(question));
        QVERIFY2(details.contains("Link Project brings it back") && !details.contains("open project"), qPrintable(details));
        QVERIFY(isLink(link));

        QTRY_VERIFY(canChoose(projectsMenu(app, rowOf(app, "amp_prj")), "projUnlink"));
        answering([&] { projectsMenu(app, rowOf(app, "amp_prj"), "projUnlink"); }, answer(QMessageBox::Yes));
        QVERIFY(!QFileInfo::exists(link) && !isLink(link));
        QCOMPARE(read(source + "/amp.sch"), kSchematic);
        QCOMPARE(read(source + "/sub/lib.sch"), kSchematic);
        QCOMPARE(rowOf(app, "amp_prj"), QPoint(-1, -1));   // gone from the panel at once (the watcher took seconds)

        // A real project: no link to remove, nothing removed.
        QVERIFY(QDir().mkpath(workspace + "/real_prj"));
        QString said;
        bool unlinked = true;
        answering([&] { unlinked = app.unlinkProject(workspace + "/real_prj"); },
                  [&](QWidget* w) {
                      auto* box = qobject_cast<QMessageBox*>(w);
                      if (box == nullptr) return false;
                      said = box->text();
                      box->accept();
                      return true;
                  });
        QVERIFY(!unlinked);
        QVERIFY2(said.contains("is not linked into the workspace"), qPrintable(said));
        QVERIFY(QFileInfo(workspace + "/real_prj").isDir());
    }

    // The open project unlinked: it closes first - not when a document's
    // unsaved changes are kept - then the link goes. Documents open through
    // a link close with it; one with unsaved changes stops it.
    void unlinkingAnOpenProject()
    {
        fresh("unlink-open");
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.switchWorkspace(workspace));
        const QString link = app.bringProjectIn(source, true);
        QVERIFY(isLink(link));
        app.openProject(link);
        QCOMPARE(app.ProjName, QStringLiteral("amp"));
        QVERIFY(app.gotoPage(link + "/amp.sch"));
        app.currentSchematic()->setDocChanged(true);

        // Yes, then the changes kept (the save dialog cancelled): not closed,
        // not unlinked.
        QString details;
        int asked = 0;
        bool unlinked = true;
        answering([&] { unlinked = app.unlinkProject(link); },
                  [&](QWidget* w) {
                      if (auto* box = qobject_cast<QMessageBox*>(w)) {
                          details = box->informativeText();
                          ++asked;
                          return !clickYes(w);
                      }
                      if (auto* save = qobject_cast<SaveDialog*>(w)) {
                          ++asked;
                          static_cast<QDialog*>(save)->reject();   // (its Cancel: closing aborted)
                          return true;
                      }
                      return false;
                  });
        QVERIFY(!unlinked);
        QCOMPARE(asked, 2);
        QVERIFY2(details.contains("It is the open project: it closes first."), qPrintable(details));
        QCOMPARE(app.ProjName, QStringLiteral("amp"));
        QVERIFY(isLink(link));

        for (QucsDoc* doc : app.allDocuments()) doc->setDocChanged(false);
        answering([&] { unlinked = app.unlinkProject(link); }, clickYes);
        QVERIFY(unlinked);
        QVERIFY(app.ProjName.isEmpty());
        QVERIFY(!isLink(link) && !QFileInfo::exists(link));
        QCOMPARE(read(source + "/amp.sch"), kSchematic);

        // Linked again, a document opened through it but not the project.
        const QString again = app.bringProjectIn(source, true);
        QCOMPARE(again, link);
        QVERIFY(app.gotoPage(link + "/sub/lib.sch"));
        app.currentSchematic()->setDocChanged(true);
        QString said;
        answering([&] { unlinked = app.unlinkProject(link); },
                  [&](QWidget* w) {
                      auto* box = qobject_cast<QMessageBox*>(w);
                      if (box == nullptr) return false;
                      said = box->text();
                      box->accept();
                      return true;
                  });
        QVERIFY(!unlinked);
        QVERIFY2(said.contains("lib.sch is open from it with unsaved changes"), qPrintable(said));
        QVERIFY(isLink(link));
        app.currentSchematic()->setDocChanged(false);
        details.clear();
        answering([&] { unlinked = app.unlinkProject(link + "/"); },   // (as a folder dialog gives it)
                  [&](QWidget* w) {
                      if (auto* box = qobject_cast<QMessageBox*>(w)) details = box->informativeText();
                      return clickYes(w);
                  });
        QVERIFY(unlinked);
        QVERIFY2(details.contains("Documents open from it close: lib.sch."), qPrintable(details));
        for (QucsDoc* doc : app.allDocuments()) QVERIFY2(!doc->getDocName().startsWith(link), qPrintable(doc->getDocName()));
        QVERIFY(!isLink(link));
        QCOMPARE(read(source + "/sub/lib.sch"), kSchematic);
    }

#ifndef Q_OS_WIN
    // A linked project whose folder is gone (moved, deleted, on a drive
    // not mounted) is listed, greyed - it was not, so it could not be
    // unlinked while its name stayed taken. Unlinked from its row; a
    // project linked under its name replaces it when the user says so.
    void aLinkThatLeadsNowhere()
    {
        fresh("dangling");
        const QString gone = workspace + "/gone_prj";
        QVERIFY(QFile::link(dir.filePath("dangling/nowhere_prj"), gone));
        QVERIFY(isDanglingLink(gone) && !isDanglingLink(workspace));
        QVERIFY(::mkfifo(QFile::encodeName(workspace + "/pipe").constData(), 0600) == 0);   // (no project: not listed)
        QVERIFY(QDir().mkpath(workspace + "/real_prj"));
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.switchWorkspace(workspace));
        QTRY_VERIFY(rowOf(app, "gone_prj").x() >= 0 && rowOf(app, "real_prj").x() >= 0);
        QCOMPARE(rowOf(app, "pipe"), QPoint(-1, -1));

        const QString tip = indexOf(app, "gone_prj").data(Qt::ToolTipRole).toString();
        QVERIFY2(tip.contains("which is not there now"), qPrintable(tip));
        QCOMPARE(indexOf(app, "gone_prj").data(Qt::ForegroundRole).value<QBrush>().color(),
                 QApplication::palette().color(QPalette::Disabled, QPalette::Text));
        const auto menu = projectsMenu(app, rowOf(app, "gone_prj"));
        QVERIFY(canChoose(menu, "projUnlink"));
        QVERIFY(!canChoose(menu, "projOpenRow"));
        // A double-click opens nothing (as a folder it listed nothing).
        const QString listed = QucsSettings.projsDir.absolutePath();
        emit app.projectsView()->doubleClicked(indexOf(app, "gone_prj"));
        QCOMPARE(QucsSettings.projsDir.absolutePath(), listed);
        QVERIFY(app.ProjName.isEmpty());

        // Its name: why it is taken, and replaced when Yes.
        const QString another = dir.filePath("dangling/elsewhere/gone_prj");
        QVERIFY(write(another + "/new.sch", kSchematic));
        const Result r = check(another, workspace);
        QCOMPARE(r.status, Result::Exists);
        QVERIFY2(r.message.contains("which is not there now") && r.message.contains("Unlink Project"), qPrintable(r.message));
        QString question;
        QString path = "unset";
        bool named = false;
        answering([&] { path = app.bringProjectIn(another, true); },
                  [&](QWidget* w) {
                      if (auto* box = qobject_cast<QMessageBox*>(w); box != nullptr && box->objectName() == "replaceDanglingLink") {
                          question = box->text();
                          box->button(QMessageBox::No)->click();
                          return false;   // (then another name is asked)
                      }
                      if (auto* name = qobject_cast<QInputDialog*>(w)) {
                          named = true;
                          name->reject();
                          return true;
                      }
                      return false;
                  });
        QVERIFY2(question.contains("which is not there now"), qPrintable(question));
        QVERIFY(named && path.isEmpty());   // (No: another name asked, and none given)
        QVERIFY(isDanglingLink(gone));
        answering([&] { path = app.bringProjectIn(another, true); }, clickYes);
        QCOMPARE(path, gone);
        QVERIFY(isLink(gone) && !isDanglingLink(gone));
        QCOMPARE(read(gone + "/new.sch"), kSchematic);

        // Unlinked from its row.
        QVERIFY(QDir(another).removeRecursively());
        QVERIFY(isDanglingLink(gone));
        answering([&] { projectsMenu(app, rowOf(app, "gone_prj"), "projUnlink"); }, clickYes);
        QVERIFY(!isLink(gone));
        QCOMPARE(rowOf(app, "gone_prj"), QPoint(-1, -1));
        QVERIFY(QFile::remove(workspace + "/pipe"));
    }
#endif

    // The workspace and the open project, each by another spelling (a link
    // to the folder they are in): the open project's row offers no Open
    // Project (it reopened it, every document closed), and unlinking finds
    // the project open and the documents open through the link.
    void theSameFolderSpelledAnotherWay()
    {
        fresh("aliased");
        const QString alias = dir.filePath("alias");
        QVERIFY(QFile::link(dir.filePath("aliased"), alias));
        const QString through = alias + "/workspace";   // the workspace as the panel lists it
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.switchWorkspace(through));
        QVERIFY(QDir().mkpath(workspace + "/real_prj"));
        const QString link = app.bringProjectIn(source, true);
        QCOMPARE(link, through + "/amp_prj");
        QTRY_VERIFY(rowOf(app, "real_prj").x() >= 0 && rowOf(app, "amp_prj").x() >= 0);

        app.openProject(workspace + "/real_prj");   // by its own path
        QCOMPARE(app.ProjName, QStringLiteral("real"));
        QVERIFY(!canChoose(projectsMenu(app, rowOf(app, "real_prj")), "projOpenRow"));
        QVERIFY(canChoose(projectsMenu(app, rowOf(app, "amp_prj")), "projOpenRow"));
        app.slotMenuProjClose();

        // The linked project opened by its own spelling, a document of it
        // by the other; unlinked by the panel's.
        app.openProject(workspace + "/amp_prj");
        QCOMPARE(app.ProjName, QStringLiteral("amp"));
        QVERIFY(!canChoose(projectsMenu(app, rowOf(app, "amp_prj")), "projOpenRow"));
        QString details;
        bool unlinked = false;
        answering([&] { unlinked = app.unlinkProject(link); },
                  [&](QWidget* w) {
                      if (auto* box = qobject_cast<QMessageBox*>(w)) details = box->informativeText();
                      return clickYes(w);
                  });
        QVERIFY(unlinked);
        QVERIFY2(details.contains("It is the open project: it closes first."), qPrintable(details));
        QVERIFY(app.ProjName.isEmpty());

        QCOMPARE(app.bringProjectIn(source, true), link);
        QVERIFY(app.gotoPage(workspace + "/amp_prj/sub/lib.sch"));
        QVERIFY(app.gotoPage(source + "/amp.sch"));   // (its own folder, not through the link)
        details.clear();
        answering([&] { unlinked = app.unlinkProject(link); },
                  [&](QWidget* w) {
                      if (auto* box = qobject_cast<QMessageBox*>(w)) details = box->informativeText();
                      return clickYes(w);
                  });
        QVERIFY(unlinked);
        QVERIFY2(details.contains("Documents open from it close: lib.sch."), qPrintable(details));
        bool stays = false;
        for (QucsDoc* doc : app.allDocuments()) {
            QVERIFY2(!doc->getDocName().endsWith("lib.sch"), qPrintable(doc->getDocName()));
            stays = stays || doc->getDocName().endsWith("amp.sch");
        }
        // (A document of the project's own folder, not through the link,
        // stays: the link going leaves its path as it was.)
        QVERIFY(stays);
        QCOMPARE(read(source + "/sub/lib.sch"), kSchematic);
    }

    void switchingTheWorkspace()
    {
        fresh("switch");
        QucsApp app(false);
        MainGuard guard(&app);
        const QString other = dir.filePath("switch/other workspace");   // not there yet
        QVERIFY(QDir().mkpath(workspace + "/open_prj"));
        app.openProject(workspace + "/open_prj");
        QCOMPARE(app.ProjName, QStringLiteral("open"));

        QVERIFY(app.switchWorkspace(other));
        QVERIFY(QFileInfo(other).isDir());
        QCOMPARE(QucsSettings.qucsWorkspaceDir.absolutePath(), other);
        QCOMPARE(QucsSettings.projsDir.absolutePath(), other);
        QVERIFY(app.ProjName.isEmpty());   // the old workspace's project is closed
        QCOMPARE(_settings::Get().item<QString>("QucsHomeDir"), QFileInfo(other).canonicalFilePath());
        // The panel lists it.
        QCOMPARE(app.projectsView()->rootIndex().data(QFileSystemModel::FilePathRole).toString(), other);
    }

    // Import and Link from the application: into the workspace, selected in
    // the panel; a linked project in italics with where it is.
    void importAndLinkFromTheApplication()
    {
        fresh("app");
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.switchWorkspace(workspace));

        const QString copied = app.bringProjectIn(source, false);
        QCOMPARE(copied, QDir(workspace).absoluteFilePath("amp_prj"));
        QTRY_COMPARE(app.projectsView()->currentIndex().data().toString(), QStringLiteral("amp_prj"));
        QVERIFY(!app.projectsView()->currentIndex().data(Qt::FontRole).value<QFont>().italic());

        // Linked under another name: the workspace has an amp_prj - the
        // name asked for comes in.
        QString asked;
        QString linked;
        answering([&] { linked = app.bringProjectIn(source, true); },
                  [&](QWidget* w) {
                      auto* input = qobject_cast<QInputDialog*>(w);
                      if (input == nullptr) return false;
                      asked = input->textValue();
                      input->setTextValue("amp_linked");
                      input->accept();
                      return true;
                  });
        QCOMPARE(asked, QStringLiteral("amp_2"));   // the free name offered
        QCOMPARE(linked, QDir(workspace).absoluteFilePath("amp_linked_prj"));
        QVERIFY(isLink(linked));
        QTRY_COMPARE(app.projectsView()->currentIndex().data().toString(), QStringLiteral("amp_linked_prj"));
        const QModelIndex row = app.projectsView()->currentIndex();
        QVERIFY(row.data(Qt::FontRole).value<QFont>().italic());
        QVERIFY2(row.data(Qt::ToolTipRole).toString().contains(QDir::toNativeSeparators(QFileInfo(source).canonicalFilePath())),
                 qPrintable(row.data(Qt::ToolTipRole).toString()));

        // Cancelled at the name: nothing comes in.
        answering([&] { QVERIFY(app.bringProjectIn(source, true).isEmpty()); },
                  [](QWidget* w) {
                      auto* input = qobject_cast<QInputDialog*>(w);
                      if (input != nullptr) input->reject();
                      return input != nullptr;
                  });
        // Not a project: said, nothing comes in.
        QString said;
        answering([&] { QVERIFY(app.bringProjectIn(dir.filePath("app/elsewhere"), false).isEmpty()); },
                  [&](QWidget* w) {
                      auto* box = qobject_cast<QMessageBox*>(w);
                      if (box == nullptr) return false;
                      said = box->text();
                      box->accept();
                      return true;
                  });
        QVERIFY2(said.contains("_prj"), qPrintable(said));
    }

    // Deleting a linked project from the workspace removes the link only.
    void deletingALinkedProject()
    {
        fresh("delete");
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.switchWorkspace(workspace));
        const QString link = app.bringProjectIn(source, true);
        QVERIFY(isLink(link));
        QString question;
        bool deleted = false;
        answering([&] { deleted = app.deleteProject(link); },
                  [&](QWidget* w) {
                      if (auto* box = qobject_cast<QMessageBox*>(w)) question = box->text();
                      return clickYes(w);
                  });
        QVERIFY(deleted);
        QVERIFY2(question.contains("Remove the link") && question.contains("stay where they are"), qPrintable(question));
        QVERIFY(!QFileInfo::exists(link) && !isLink(link));
        QCOMPARE(read(source + "/amp.sch"), kSchematic);
        QCOMPARE(read(source + "/sub/lib.sch"), kSchematic);
    }

    // Project > Delete Project takes the folder from the dialog, which on
    // macOS ends in "/". A link read through that slash is the folder it
    // leads to: the question has to be about the link all the same, and
    // Yes must take the link, never the project's files.
    void deletingALinkedProjectGivenWithASlash()
    {
        fresh("delete-slash");
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.switchWorkspace(workspace));
        const QString link = app.bringProjectIn(source, true);
        QVERIFY(isLink(link));
        QVERIFY(isLink(link + "/"));
        QCOMPARE(QFileInfo(linkTarget(link + "/")).canonicalFilePath(), QFileInfo(source).canonicalFilePath());
        QString question;
        bool deleted = false;
        answering([&] { deleted = app.deleteProject(link + "/"); },
                  [&](QWidget* w) {
                      if (auto* box = qobject_cast<QMessageBox*>(w)) question += box->text() + "\n";
                      return clickYes(w);
                  });
        QVERIFY(deleted);
        QVERIFY2(question.contains("Remove the link") && !question.contains("destroy"), qPrintable(question));
        QVERIFY(!QFileInfo::exists(link) && !isLink(link));
        QCOMPARE(read(source + "/amp.sch"), kSchematic);
        QCOMPARE(read(source + "/sub/lib.sch"), kSchematic);
    }

    // A linked project is a project: it opens, lists its files, its
    // documents are saved where it is, its Scratch folder is made there.
    void aLinkedProjectIsAProject()
    {
        fresh("use");
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.switchWorkspace(workspace));
        const QString link = app.bringProjectIn(source, true);
        QVERIFY(isLink(link));

        app.openProject(link);
        QCOMPARE(app.ProjName, QStringLiteral("amp"));
        QCOMPARE(QucsSettings.QucsWorkDir.absolutePath(), link);
        const QStringList files = misc::projectFiles(QucsSettings.QucsWorkDir, {"*.sch"});
        QVERIFY2(files.contains("amp.sch") && files.contains("sub/lib.sch"), qPrintable(files.join(", ")));
        QVERIFY2(QFileInfo(source + "/Scratch").isDir(), "the project's Scratch folder, made on opening");

        QVERIFY(app.gotoPage(link + "/amp.sch"));
        Schematic* doc = app.currentSchematic();
        QVERIFY(doc != nullptr);
        doc->setChanged(true);
        QVERIFY(doc->save() >= 0);
        QVERIFY(read(source + "/amp.sch").startsWith("<Qucs Schematic"));
        QVERIFY(!isLink(source + "/amp.sch"));   // saved in place, not replaced by anything
        doc->setChanged(false);
        app.closeAllFiles();
        app.slotMenuProjClose();
        QVERIFY(isLink(link));
    }

    // The workspace changed in the settings: the panel lists the new one.
    void theSettingsDialogSwitchesToo()
    {
        fresh("settings");
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.switchWorkspace(workspace));
        const QString other = dir.filePath("settings/from the dialog");
        QucsSettingsDialog dlg(&app);
        QLineEdit* home = nullptr;
        for (QLineEdit* e : dlg.findChildren<QLineEdit*>())
            if (e->text() == QFileInfo(workspace).canonicalFilePath()) home = e;
        QVERIFY(home != nullptr);
        home->setText(other);
        QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
        QVERIFY(QFileInfo(other).isDir());
        QCOMPARE(QucsSettings.projsDir.absolutePath(), other);
        QCOMPARE(app.projectsView()->rootIndex().data(QFileSystemModel::FilePathRole).toString(), other);
    }
};

QTEST_MAIN(TestWorkspaceProjects)
#include "test_workspace_projects.moc"
