/*
 * The Projects panel's menu: Switch Workspace, Import Project (a project
 * folder copied into the workspace), Link Project (a link to it in the
 * workspace, nothing copied, and it acts as a project there). Deleting a
 * linked project removes the link, never the project it leads to. And
 * the workspace changed in the settings is the one the panel lists.
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

#include "config.h"
#include "qucs.h"
#include "schematic.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "settings.h"
#include "workspace.h"
#include "dialogs/qucssettingsdialog.h"
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
    box->button(QMessageBox::Yes)->click();
    return true;
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

    // The menu of the Projects panel, and the same actions in the Project menu.
    void theMenu()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        QCOMPARE(app.projectsView()->contextMenuPolicy(), Qt::CustomContextMenu);
        QStringList shown;
        // The menu is a popup, not modal: look for it while it is open.
        QTimer::singleShot(50, [&] {
            for (QWidget* w : QApplication::topLevelWidgets())
                if (auto* menu = qobject_cast<QMenu*>(w); menu && menu->isVisible()) {
                    for (QAction* a : menu->actions())
                        if (!a->isSeparator()) shown << a->objectName();
                    menu->close();
                }
        });
        emit app.projectsView()->customContextMenuRequested(QPoint(5, 5));
        QCOMPARE(shown, QStringList({"projSwitchWorkspace", "projImport", "projLink"}));
        for (const char* name : {"projSwitchWorkspace", "projImport", "projLink"}) {
            auto* action = app.findChild<QAction*>(name);
            QVERIFY(action != nullptr);
            bool inMenuBar = false;
            for (QAction* top : app.menuBar()->actions())
                if (top->menu() != nullptr && top->menu()->actions().contains(action)) inMenuBar = true;
            QVERIFY2(inMenuBar, name);
        }
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
