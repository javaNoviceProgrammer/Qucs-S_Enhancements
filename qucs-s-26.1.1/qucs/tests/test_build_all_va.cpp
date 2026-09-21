/*
 * The Content panel and "Build All" on its Verilog-A row: the panel lists
 * the project's files from any subdirectory, .osdi files under their own
 * category; Build All puts every .va file of the project through the
 * OpenVAF executable from the settings, one after the other, with the
 * output in the message dock. A fake compiler script stands in for OpenVAF.
 */
#include <QtTest>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QPlainTextEdit>
#include <QMenu>
#include <algorithm>

#include "config.h"
#include "qucs.h"
#include "schematic.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "messagedock.h"
#include "projectView.h"
#include "extsimkernels/spicecompat.h"
#include "extsimkernels/ngspice.h"
#include "isolated_settings.h"

// QucsMain must not outlive the QucsApp of a test that fails half-way.
struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

// The listing mode is global; put it back whatever happens.
struct TreeViewGuard {
    bool saved = QucsSettings.ContentTreeView;
    ~TreeViewGuard() { QucsSettings.ContentTreeView = saved; }
};

// Whether a menu holding this action is popped up right now. Checked
// synchronously: popup() shows the menu at once, and on the offscreen
// platform a popup does not survive the next round of event processing.
static bool menuShowing(QObject* root, QAction* action)
{
    for (QMenu* m : root->findChildren<QMenu*>())
        if (m->isVisible() && m->actions().contains(action)) return true;
    return false;
}

static bool anyMenuShowing(QObject* root)
{
    for (QMenu* m : root->findChildren<QMenu*>())
        if (m->isVisible()) return true;
    return false;
}

class TestBuildAllVerilogA : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString project;

    QString write(const QString& path, const QString& text, bool executable = false)
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return {};
        f.write(text.toUtf8());
        f.close();
        if (executable)
            f.setPermissions(f.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeUser);
        return path;
    }

    // A stand-in for openvaf: writes <name>.osdi next to the input and
    // succeeds, unless the file mentions "broken", in which case it prints
    // an error and fails. Records every invocation in calls.log.
    QString fakeCompiler()
    {
        return write(dir.filePath("fake-openvaf.sh"),
            "#!/bin/sh\n"
            "echo \"$1\" >> \"" + dir.filePath("calls.log") + "\"\n"
            "if grep -q broken \"$1\"; then echo \"error: cannot parse $1\"; exit 1; fi\n"
            "echo \"compiled $1\"\n"
            "touch \"${1%.va}.osdi\"\n", true);
    }

    static QStringList children(ProjectView* view, int category)
    {
        return childrenOf(view->model()->item(category, 0));
    }

    static QStringList childrenOf(QStandardItem* parent)
    {
        QStringList names;
        for (int i = 0; parent && i < parent->rowCount(); ++i)
            names << parent->child(i, 0)->text();
        return names;
    }

    // The row named `name` under `parent` (a folder or a file).
    static QStandardItem* row(QStandardItem* parent, const QString& name)
    {
        for (int i = 0; parent && i < parent->rowCount(); ++i)
            if (parent->child(i, 0)->text() == name) return parent->child(i, 0);
        return nullptr;
    }

    // The QMenu holding an action with this text.
    static QMenu* menuWithAction(QObject* root, const QString& text)
    {
        for (QMenu* m : root->findChildren<QMenu*>())
            for (QAction* a : m->actions())
                if (a->text() == text) return m;
        return nullptr;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        // QucsApp lists the simulators it can find and puts up a modal
        // error box when there is none: name one that exists.
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QucsSettings.ContentTreeView = false;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();

        project = dir.filePath("vatest_prj");
        QVERIFY(QDir().mkpath(project));
        write(project + "/good.va", "module good(p, n);\nendmodule\n");
        write(project + "/broken.va", "module broken(p, n);\n// broken\nendmodule\n");
        write(project + "/other.sch", "<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n</Components>\n");
        // Files in subdirectories belong to the project too; hidden
        // directories do not.
        QVERIFY(QDir().mkpath(project + "/models/nested"));
        QVERIFY(QDir().mkpath(project + "/.hidden"));
        write(project + "/models/deep.va", "module deep(p, n);\nendmodule\n");
        write(project + "/models/nested/notes.txt", "notes\n");
        write(project + "/models/nested/sub.sch", "<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n"
              "<Port P1 1 0 0 0 0 0 0 \"1\" 1 \"analog\" 0>\n<Port P2 1 0 0 0 0 0 0 \"2\" 1 \"analog\" 0>\n</Components>\n");
        write(project + "/.hidden/secret.va", "module secret(p, n);\nendmodule\n");
    }

    // The tree shows files from every level of the project as paths
    // relative to it, root files first.
    void listsTheWholeProjectTree()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        ProjectView* view = app.projectView();
        view->setProjPath(project);
        QVERIFY(view->isExpanded(view->model()->index(ProjectView::Schematics, 0)));   // open on arrival
        QVERIFY(!view->isExpanded(view->model()->index(ProjectView::VerilogA, 0)));
        QCOMPARE(children(view, ProjectView::VerilogA), QStringList({"broken.va", "good.va", "models/deep.va"}));
        QCOMPARE(children(view, ProjectView::Schematics), QStringList({"other.sch", "models/nested/sub.sch"}));
        // The subcircuit in the subdirectory got its port count.
        QStandardItem* schematics = view->model()->item(ProjectView::Schematics, 0);
        QCOMPARE(schematics->child(1, 1)->text(), QString("2-port"));
        QVERIFY(schematics->child(0, 1) == nullptr || schematics->child(0, 1)->text().isEmpty());
        QCOMPARE(children(view, ProjectView::Others), QStringList({"models/nested/notes.txt"}));
        QCOMPARE(children(view, ProjectView::Osdi), QStringList());
        for (int cat = 0; cat < view->model()->rowCount(); ++cat)
            for (const QString& name : children(view, cat))
                QVERIFY2(!name.contains("secret"), qPrintable(name));
    }

    // The same project with folders as sub-trees: the rows show bare
    // names, every file row still knows its project-relative path, and
    // the consumers' lookups (category, file path, subcircuit list, drag
    // URLs) see through the folder rows.
    void treeViewShowsFoldersAsSubtrees()
    {
        TreeViewGuard mode;
        QucsSettings.ContentTreeView = true;
        QucsApp app(false);
        MainGuard guard(&app);
        ProjectView* view = app.projectView();
        view->setProjPath(project);
        QStandardItemModel* m = view->model();

        QStandardItem* va = m->item(ProjectView::VerilogA, 0);
        QCOMPARE(childrenOf(va), QStringList({"broken.va", "good.va", "models"}));
        QStandardItem* models = row(va, "models");
        QVERIFY(models != nullptr);
        QCOMPARE(childrenOf(models), QStringList({"deep.va"}));
        QVERIFY(!(models->flags() & Qt::ItemIsSelectable));               // a folder row, like a category
        QVERIFY(!(models->flags() & Qt::ItemIsDragEnabled));
        QVERIFY(!models->icon().isNull());

        const QModelIndex deep = models->child(0, 0)->index();
        QCOMPARE(view->filePath(deep), QString("models/deep.va"));
        QCOMPARE(view->filePath(models->index()), QString());
        QCOMPARE(view->filePath(va->index()), QString());
        QVERIFY(view->isFile(deep));
        QVERIFY(!view->isFile(models->index()));
        QCOMPARE(view->categoryOf(deep), int(ProjectView::VerilogA));
        QCOMPARE(view->categoryOf(models->index()), int(ProjectView::VerilogA));

        // Two levels down, with the note beside the file.
        QStandardItem* sch = m->item(ProjectView::Schematics, 0);
        QCOMPARE(childrenOf(sch), QStringList({"other.sch", "models"}));
        QStandardItem* nested = row(row(sch, "models"), "nested");
        QVERIFY(nested != nullptr);
        QCOMPARE(childrenOf(nested), QStringList({"sub.sch"}));
        QCOMPARE(nested->child(0, 1)->text(), QString("2-port"));
        QCOMPARE(view->filePath(nested->child(0, 0)->index()), QString("models/nested/sub.sch"));
        QCOMPARE(view->exportSchematic(), QStringList({"models/nested/sub.sch"}));

        view->selectionModel()->select(deep, QItemSelectionModel::Select | QItemSelectionModel::Rows);
        view->selectionModel()->select(models->index(), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        QCOMPARE(view->selectedFileUrls(), QList<QUrl>({QUrl::fromLocalFile(project + "/models/deep.va")}));

        // Expanded folders survive a refresh (as after a build or a save).
        view->setExpanded(va->index(), true);
        view->setExpanded(models->index(), true);
        view->refresh();
        QStandardItem* modelsAgain = row(m->item(ProjectView::VerilogA, 0), "models");
        QVERIFY(modelsAgain != nullptr);
        QVERIFY(view->isExpanded(m->item(ProjectView::VerilogA, 0)->index()));
        QVERIFY(view->isExpanded(modelsAgain->index()));
        QVERIFY(!view->isExpanded(m->item(ProjectView::Others, 0)->index()));

        // Back to the flat listing: the same files, "dir/name" rows again.
        view->setTreeView(false);
        QVERIFY(!QucsSettings.ContentTreeView);
        QCOMPARE(children(view, ProjectView::VerilogA), QStringList({"broken.va", "good.va", "models/deep.va"}));
        QCOMPARE(view->exportSchematic(), QStringList({"models/nested/sub.sch"}));
    }

    // The panel's context menu on the empty area (only there) offers the
    // two listings; picking one switches the tree and the setting.
    void contextMenuTogglesTheView()
    {
        TreeViewGuard mode;
        QucsSettings.ContentTreeView = false;
        QucsApp app(false);
        MainGuard guard(&app);
        app.projectView()->setProjPath(project);
        app.show();
        ProjectView* view = app.projectView();
        view->expandAll();
        QVERIFY(QTest::qWaitForWindowExposed(&app));

        QMenu* viewMenu = menuWithAction(&app, "Sub-trees per folder");
        QVERIFY(viewMenu != nullptr);
        QCOMPARE(viewMenu->title(), QString("Toggle hierarchy search view"));
        QAction* flat = viewMenu->actions().at(0);
        QAction* tree = viewMenu->actions().at(1);
        QVERIFY(flat->isCheckable() && tree->isCheckable());
        // ...as a sub-menu of the panel menu only: not of the file menu, not
        // of the Verilog-A menu.
        int holders = 0;
        for (QMenu* m : app.findChildren<QMenu*>())
            if (m->actions().contains(viewMenu->menuAction())) ++holders;
        QCOMPARE(holders, 1);
        QVERIFY(!menuWithAction(&app, "Open")->actions().contains(viewMenu->menuAction()));
        QVERIFY(!menuWithAction(&app, "Build All")->actions().contains(viewMenu->menuAction()));

        // The empty area below the rows.
        const QPoint empty(10, view->viewport()->height() - 2);
        QVERIFY(!view->indexAt(empty).isValid());
        QVERIFY(QMetaObject::invokeMethod(&app, "slotShowContentMenu", Q_ARG(QPoint, empty)));
        QVERIFY(menuShowing(&app, viewMenu->menuAction()));
        QVERIFY(flat->isChecked());
        QVERIFY(!tree->isChecked());
        for (QMenu* menu : app.findChildren<QMenu*>()) menu->hide();

        tree->trigger();
        QVERIFY(QucsSettings.ContentTreeView);
        QVERIFY(row(view->model()->item(ProjectView::VerilogA, 0), "models") != nullptr);

        // Rows get no such menu: a category other than Verilog-A shows the
        // category menu (Refresh only), a folder the same (it is not a file
        // either), a file shows the file menu without it.
        QStandardItemModel* m = view->model();
        const QModelIndex others = m->index(ProjectView::Others, 0);
        view->scrollTo(others);
        const QPoint onOthers = view->visualRect(others).center();
        QCOMPARE(view->indexAt(onOthers), others);
        QVERIFY(QMetaObject::invokeMethod(&app, "slotShowContentMenu", Q_ARG(QPoint, onOthers)));
        QVERIFY(!menuShowing(&app, viewMenu->menuAction()));
        QVERIFY(anyMenuShowing(&app));
        for (QMenu* menu : app.findChildren<QMenu*>()) {
            if (!menu->isVisible()) continue;
            QStringList texts;
            for (QAction* a : menu->actions()) texts << a->text();
            QCOMPARE(texts, QStringList{"Refresh"});
        }
        for (QMenu* menu : app.findChildren<QMenu*>()) menu->hide();

        const QModelIndex folder = row(m->item(ProjectView::VerilogA, 0), "models")->index();
        view->scrollTo(folder);
        const QPoint onFolder = view->visualRect(folder).center();
        QCOMPARE(view->indexAt(onFolder), folder);
        QVERIFY(QMetaObject::invokeMethod(&app, "slotShowContentMenu", Q_ARG(QPoint, onFolder)));
        QVERIFY(!menuShowing(&app, viewMenu->menuAction()));
        QMenu* fileMenu = menuWithAction(&app, "Open");
        QVERIFY(fileMenu != nullptr);
        QVERIFY(!fileMenu->isVisible());   // a folder is not a file: no Open/Rename/Delete

        const QModelIndex deep = row(folder.model() == m ? m->itemFromIndex(folder) : nullptr, "deep.va")->index();
        view->scrollTo(deep);
        const QPoint onFile = view->visualRect(deep).center();
        QCOMPARE(view->indexAt(onFile), deep);
        QVERIFY(QMetaObject::invokeMethod(&app, "slotShowContentMenu", Q_ARG(QPoint, onFile)));
        QVERIFY(fileMenu->isVisible());
        QVERIFY(!menuShowing(&app, viewMenu->menuAction()));
        for (QMenu* menu : app.findChildren<QMenu*>()) menu->hide();
        // The mode entries reflect the current listing when the panel menu opens.
        view->collapseAll();
        view->scrollToTop();
        QCoreApplication::processEvents();
        const QPoint emptyAgain(10, view->viewport()->height() - 2);
        QVERIFY2(!view->indexAt(emptyAgain).isValid(),
                 qPrintable(QString("row at the bottom: %1 (viewport %2 px)").arg(view->indexAt(emptyAgain).data().toString()).arg(view->viewport()->height())));
        QVERIFY(QMetaObject::invokeMethod(&app, "slotShowContentMenu", Q_ARG(QPoint, emptyAgain)));
        QVERIFY(menuShowing(&app, viewMenu->menuAction()));
        QVERIFY(tree->isChecked());
        QVERIFY(!flat->isChecked());
        for (QMenu* menu : app.findChildren<QMenu*>()) menu->hide();

        flat->trigger();
        QVERIFY(!QucsSettings.ContentTreeView);
        QCOMPARE(children(view, ProjectView::VerilogA), QStringList({"broken.va", "good.va", "models/deep.va"}));
    }

    void projectFilesHelper()
    {
        const QDir root(project);
        QCOMPARE(misc::projectFiles(root, {"*.va"}), QStringList({"broken.va", "good.va", "models/deep.va"}));
        QCOMPARE(misc::projectFiles(root, {"*.txt"}), QStringList({"models/nested/notes.txt"}));
        QCOMPARE(misc::projectFiles(root, {"*.none"}), QStringList());
        QCOMPARE(misc::projectFiles(QDir(dir.filePath("does-not-exist"))), QStringList());
        // Root files first, then directory by directory, names case-insensitively.
        QVERIFY(QDir().mkpath(dir.filePath("order/b/x")));
        QVERIFY(QDir().mkpath(dir.filePath("order/A")));
        for (const QString& f : {"order/Zeta.txt", "order/alpha.txt", "order/b/x/deep.txt", "order/b/one.txt", "order/A/two.txt"})
            write(dir.filePath(f), "x");
        QCOMPARE(misc::projectFiles(QDir(dir.filePath("order"))),
                 QStringList({"alpha.txt", "Zeta.txt", "A/two.txt", "b/one.txt", "b/x/deep.txt"}));
    }

    // A subcircuit inserted from the panel refers to its file relative to
    // the project; the netlister resolves that from any schematic.
    void relativeProjectPathsResolve()
    {
        QucsSettings.QucsWorkDir.setPath(project);
        QCOMPARE(misc::properAbsFileName("models/nested/sub.sch"), QFileInfo(project + "/models/nested/sub.sch").canonicalFilePath());
        QCOMPARE(misc::properAbsFileName("other.sch"), QFileInfo(project + "/other.sch").canonicalFilePath());
        QCOMPARE(misc::properAbsFileName("nowhere/missing.sch"), QString("nowhere/missing.sch"));
    }

    void buildsEveryFileAndTalliesFailures()
    {
        QucsSettings.OpenVAFExecutable = fakeCompiler();
        QucsSettings.QucsWorkDir.setPath(project);

        QucsApp app(false);
        MainGuard guard(&app);
        app.projectView()->setProjPath(project);
        app.projectView()->setExpanded(app.projectView()->model()->index(ProjectView::VerilogA, 0), true);
        QCOMPARE(children(app.projectView(), ProjectView::VerilogA), QStringList({"broken.va", "good.va", "models/deep.va"}));

        QVERIFY(QMetaObject::invokeMethod(&app, "slotCMenuBuildAllVerilogA"));
        QTRY_VERIFY_WITH_TIMEOUT(app.messages()->admsOutput->toPlainText().contains("Done:"), 15000);

        const QString log = app.messages()->admsOutput->toPlainText();
        QVERIFY2(log.contains("Building 3 Verilog-A file(s)"), qPrintable(log));
        QVERIFY2(log.contains("compiled " + project + "/good.va"), qPrintable(log));
        QVERIFY2(log.contains("compiled " + project + "/models/deep.va"), qPrintable(log));
        QVERIFY2(log.contains("error: cannot parse"), qPrintable(log));
        QVERIFY2(log.contains("exited with code 1"), qPrintable(log));
        QVERIFY2(log.contains("Done: 2 of 3 compiled, 1 failed."), qPrintable(log));

        // All three were compiled, in the panel's order, the one in the
        // subdirectory included.
        QFile calls(dir.filePath("calls.log"));
        QVERIFY(calls.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(calls.readAll()).trimmed().split('\n'),
                 QStringList({project + "/broken.va", project + "/good.va", project + "/models/deep.va"}));
        QVERIFY(QFileInfo::exists(project + "/good.osdi"));
        QVERIFY(QFileInfo::exists(project + "/models/deep.osdi"));
        QVERIFY(!QFileInfo::exists(project + "/broken.osdi"));
        QVERIFY(!QFileInfo::exists(project + "/.hidden/secret.osdi"));

        // The tree was refreshed: the new .osdi files show up under "Osdi"
        // (not "Others"), and the row the user was working in stayed open.
        QCOMPARE(children(app.projectView(), ProjectView::Osdi), QStringList({"good.osdi", "models/deep.osdi"}));
        QVERIFY(!children(app.projectView(), ProjectView::Others).contains("good.osdi"));
        QVERIFY(app.projectView()->isExpanded(app.projectView()->model()->index(ProjectView::VerilogA, 0)));

        // ngspice gets every compiled model of the project, wherever it is.
        app.ProjName = "vatest";
        QucsSettings.S4Qworkdir = dir.filePath("kernel");
        Schematic sch(nullptr, QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/RF/Miscellaneous/RCL_resonance.sch"));
        QVERIFY(sch.load());
        Ngspice kernel(&sch);
        kernel.setWorkdir(dir.filePath("kernel"));
        kernel.SaveNetlist(dir.filePath("kernel/net.cir"), false);
        QFile net(dir.filePath("kernel/net.cir"));
        QVERIFY(net.open(QIODevice::ReadOnly));
        const QString netlist = QString::fromUtf8(net.readAll());
        QVERIFY2(netlist.contains("pre_osdi '" + project + "/good.osdi'"), qPrintable(netlist));
        QVERIFY2(netlist.contains("pre_osdi '" + project + "/models/deep.osdi'"), qPrintable(netlist));
    }

    // Right-clicking the "Verilog-A" row pops up a menu with "Build All";
    // a file row still gets the file menu, and other category rows nothing.
    void contextMenuOnTheCategoryRow()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        app.projectView()->setProjPath(project);
        app.show();
        ProjectView* view = app.projectView();
        view->expandAll();
        QVERIFY(QTest::qWaitForWindowExposed(&app));

        QMenu* buildMenu = nullptr;
        for (QMenu* m : app.findChildren<QMenu*>())
            for (QAction* a : m->actions())
                if (a->text() == "Build All") buildMenu = m;
        QVERIFY(buildMenu != nullptr);

        QStandardItemModel* m = view->model();
        const QModelIndex va = m->index(ProjectView::VerilogA, 0);
        // customContextMenuRequested() carries viewport coordinates.
        const QPoint onCategory = view->visualRect(va).center();
        QVERIFY(QMetaObject::invokeMethod(&app, "slotShowContentMenu", Q_ARG(QPoint, onCategory)));
        QVERIFY(buildMenu->isVisible());
        buildMenu->hide();

        const QModelIndex sch = m->index(ProjectView::Schematics, 0);
        const QPoint onOther = view->visualRect(sch).center();
        QVERIFY(QMetaObject::invokeMethod(&app, "slotShowContentMenu", Q_ARG(QPoint, onOther)));
        QVERIFY(!buildMenu->isVisible());
        for (QMenu* menu : app.findChildren<QMenu*>()) menu->hide();

        const QPoint onFile = view->visualRect(m->index(0, 0, va)).center();
        QVERIFY(QMetaObject::invokeMethod(&app, "slotShowContentMenu", Q_ARG(QPoint, onFile)));
        QVERIFY(!buildMenu->isVisible());
        QVERIFY(menuWithAction(&app, "Open")->isVisible());   // the file menu
        for (QMenu* menu : app.findChildren<QMenu*>()) menu->hide();
    }

    void categoryLookup()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        app.projectView()->setProjPath(project);
        QStandardItemModel* m = app.projectView()->model();
        const QModelIndex va = m->index(ProjectView::VerilogA, 0);
        QCOMPARE(app.projectView()->categoryOf(va), int(ProjectView::VerilogA));
        QCOMPARE(app.projectView()->categoryOf(m->index(0, 0, va)), int(ProjectView::VerilogA));   // a file
        QCOMPARE(app.projectView()->categoryOf(QModelIndex()), -1);
    }

    // The panel lists files that appear, go or move by themselves - it
    // watches the project's directories - and Refresh is on every one of
    // its menus for when it should not have to.
    void filesAppearByThemselvesAndRefreshIsOnEveryMenu()
    {
        TreeViewGuard mode;
        QucsSettings.ContentTreeView = false;
        QucsApp app(false);
        MainGuard guard(&app);
        ProjectView* view = app.projectView();
        view->setProjPath(project);
        QCOMPARE(children(view, ProjectView::VerilogA), QStringList({"broken.va", "good.va", "models/deep.va"}));
        // The project directory and its subdirectories are watched; hidden
        // ones are not.
        const QStringList watched = view->watchedDirectories();
        QVERIFY2(watched.contains(project), qPrintable(watched.join(", ")));
        QVERIFY(watched.contains(project + "/models"));
        QVERIFY(watched.contains(project + "/models/nested"));
        QVERIFY(!watched.contains(project + "/.hidden"));

        // A file written by something else - the Terminal dock, a script.
        write(project + "/late.va", "module late(p, n);\nendmodule\n");
        QTRY_VERIFY_WITH_TIMEOUT(children(view, ProjectView::VerilogA).contains("late.va"), 5000);
        // A new directory, then a file in it: the directory is watched as
        // soon as it is listed, so the file is noticed too.
        QVERIFY(QDir().mkpath(project + "/extra"));
        QTRY_VERIFY_WITH_TIMEOUT(view->watchedDirectories().contains(project + "/extra"), 5000);
        write(project + "/extra/x.va", "module x(p, n);\nendmodule\n");
        QTRY_VERIFY_WITH_TIMEOUT(children(view, ProjectView::VerilogA).contains("extra/x.va"), 5000);
        // Gone again.
        QVERIFY(QFile::remove(project + "/late.va"));
        QTRY_VERIFY_WITH_TIMEOUT(!children(view, ProjectView::VerilogA).contains("late.va"), 5000);

        // Refresh, by hand: on the panel's menu, the file menu, the
        // Verilog-A menu and the category menu.
        QAction* refresh = nullptr;
        for (QAction* a : app.findChildren<QAction*>())
            if (a->text() == "Refresh") { refresh = a; break; }
        QVERIFY(refresh != nullptr);
        int holders = 0;
        for (QMenu* m : app.findChildren<QMenu*>())
            if (m->actions().contains(refresh)) ++holders;
        QCOMPARE(holders, 4);
        QVERIFY(menuWithAction(&app, "Open")->actions().contains(refresh));
        QVERIFY(menuWithAction(&app, "Build All")->actions().contains(refresh));
        QVERIFY(menuWithAction(&app, "Sub-trees per folder") != nullptr);
        // Removing a file with the watcher's refresh not yet due: Refresh
        // lists the project as it is now.
        QVERIFY(QFile::remove(project + "/extra/x.va"));
        QVERIFY(QDir(project + "/extra").removeRecursively());
        refresh->trigger();
        QCOMPARE(children(view, ProjectView::VerilogA), QStringList({"broken.va", "good.va", "models/deep.va"}));
        QTRY_VERIFY_WITH_TIMEOUT(!view->watchedDirectories().contains(project + "/extra"), 5000);
    }
};

int main(int argc, char** argv)
{
    int one = 1;   // QucsApp opens every argument as a document
    QApplication app(one, argv);
    TestBuildAllVerilogA test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_build_all_va.moc"
