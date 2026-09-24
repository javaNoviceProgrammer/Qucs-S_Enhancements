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
#include <QRegularExpression>
#include <algorithm>

#include "config.h"
#include "qucs.h"
#include "schematic.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "messagedock.h"
#include "simulationconsole.h"
#include "textdoc.h"
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include "projectView.h"
#include "extsimkernels/spicecompat.h"
#include "extsimkernels/ngspice.h"
#include "isolated_settings.h"
#include "settings.h"
#include "dialogs/qucssettingsdialog.h"
#include <QCheckBox>
#include <QSpinBox>

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
            // the module's name as a string, as a library holds it
            "printf '\\000%s\\000' \"$(basename \"${1%.va}\")\" > \"${1%.va}.osdi\"\n", true);
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
        QVERIFY(models->icon().isNull());   // a plain row unless QucsSettings.ContentFolderIcons

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

        // Rows get no such menu: a category other than Verilog-A shows none,
        // a folder shows none (it is not a file either), a file shows the
        // file menu without it.
        QStandardItemModel* m = view->model();
        const QModelIndex others = m->index(ProjectView::Others, 0);
        view->scrollTo(others);
        const QPoint onOthers = view->visualRect(others).center();
        QCOMPARE(view->indexAt(onOthers), others);
        QVERIFY(QMetaObject::invokeMethod(&app, "slotShowContentMenu", Q_ARG(QPoint, onOthers)));
        QVERIFY(!menuShowing(&app, viewMenu->menuAction()));
        QVERIFY(!anyMenuShowing(&app));

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

        // The output moves the cursor of the dock's text a line at a time:
        // no warning a line (the dock printed the line's number).
        QTest::failOnWarning(QRegularExpression(QStringLiteral("^\\d+$")));
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

        // ngspice gets the compiled models the netlist uses, wherever in
        // the project they are - and a circuit without them none.
        app.ProjName = "vatest";
        QucsSettings.S4Qworkdir = dir.filePath("kernel");
        const auto netlistOf = [&](const QString& file) {
            Schematic sch(nullptr, file);
            if (!sch.load()) return QString();
            Ngspice kernel(&sch);
            kernel.setWorkdir(dir.filePath("kernel"));
            kernel.SaveNetlist(dir.filePath("kernel/net.cir"), false);
            QFile net(dir.filePath("kernel/net.cir"));
            return net.open(QIODevice::ReadOnly) ? QString::fromUtf8(net.readAll()) : QString();
        };
        write(project + "/uses.sch",
              "<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n"
              "  <R R1 1 250 100 -26 15 0 0 \"1 kOhm\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
              "  <GND * 1 280 100 0 0 0 0>\n"
              "  <GND * 1 220 100 0 0 0 0>\n"
              "  <SpiceModel SpiceModel1 1 120 300 -27 16 0 0 \".model m1 good\" 1 \".model m2 DEEP\" 1 \"Line_3=\" 0>\n"
              "</Components>\n");
        QString netlist = netlistOf(project + "/uses.sch");
        QVERIFY2(netlist.contains("pre_osdi '" + project + "/good.osdi'"), qPrintable(netlist));
        QVERIFY2(netlist.contains("pre_osdi '" + project + "/models/deep.osdi'"), qPrintable(netlist));
        netlist = netlistOf(QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/RF/Miscellaneous/RCL_resonance.sch"));
        QVERIFY2(netlist.contains(".control") && !netlist.contains("pre_osdi"), qPrintable(netlist));
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

    // Every few seconds the panel looks at the project's files and lists
    // them again when one came, went or changed - at the interval from
    // the settings, or not at all when turned off there. Refresh, by hand,
    // is on the panel's own menu (the empty area) and nowhere else.
    void filesAppearByThemselvesAtTheSetInterval()
    {
        TreeViewGuard mode;
        QucsSettings.ContentTreeView = false;
        struct RefreshGuard {
            bool on = QucsSettings.ContentAutoRefresh;
            int seconds = QucsSettings.ContentRefreshSeconds;
            ~RefreshGuard() { QucsSettings.ContentAutoRefresh = on; QucsSettings.ContentRefreshSeconds = seconds; }
        } refreshGuard;
        QucsSettings.ContentAutoRefresh = true;
        QucsSettings.ContentRefreshSeconds = 1;
        QucsApp app(false);
        MainGuard guard(&app);
        ProjectView* view = app.projectView();
        view->setProjPath(project);
        QVERIFY(view->autoRefreshEnabled());
        QCOMPARE(children(view, ProjectView::VerilogA), QStringList({"broken.va", "good.va", "models/deep.va"}));

        // A file written by something else - the Terminal dock, a script.
        write(project + "/late.va", "module late(p, n);\nendmodule\n");
        QTRY_VERIFY_WITH_TIMEOUT(children(view, ProjectView::VerilogA).contains("late.va"), 5000);
        // A new directory with a file in it.
        QVERIFY(QDir().mkpath(project + "/extra"));
        write(project + "/extra/x.va", "module x(p, n);\nendmodule\n");
        QTRY_VERIFY_WITH_TIMEOUT(children(view, ProjectView::VerilogA).contains("extra/x.va"), 5000);
        // Gone again.
        QVERIFY(QFile::remove(project + "/late.va"));
        QTRY_VERIFY_WITH_TIMEOUT(!children(view, ProjectView::VerilogA).contains("late.va"), 5000);
        // Nothing changed: the panel is left alone (the listing keeps its
        // signature; a rebuild would produce fresh items).
        QStandardItem* before = view->model()->item(ProjectView::VerilogA, 0);
        QTest::qWait(2500);
        QCOMPARE(view->model()->item(ProjectView::VerilogA, 0), before);

        // Turned off: a new file is not noticed...
        QucsSettings.ContentAutoRefresh = false;
        view->applyRefreshSettings();
        QVERIFY(!view->autoRefreshEnabled());
        write(project + "/unseen.va", "module unseen(p, n);\nendmodule\n");
        QTest::qWait(2500);
        QVERIFY(!children(view, ProjectView::VerilogA).contains("unseen.va"));
        // ...until Refresh, which is on the panel menu only.
        QAction* refresh = nullptr;
        for (QAction* a : app.findChildren<QAction*>())
            if (a->text() == "Refresh") { refresh = a; break; }
        QVERIFY(refresh != nullptr);
        int holders = 0;
        for (QMenu* m : app.findChildren<QMenu*>())
            if (m->actions().contains(refresh)) ++holders;
        QCOMPARE(holders, 1);
        QVERIFY(!menuWithAction(&app, "Open")->actions().contains(refresh));
        QVERIFY(!menuWithAction(&app, "Build All")->actions().contains(refresh));
        QMenu* panelMenu = menuWithAction(&app, "Refresh");
        QVERIFY(panelMenu->actions().contains(menuWithAction(&app, "Sub-trees per folder")->menuAction()));
        refresh->trigger();
        QVERIFY(children(view, ProjectView::VerilogA).contains("unseen.va"));

        // Back on, with a longer interval: the timer follows.
        QucsSettings.ContentAutoRefresh = true;
        QucsSettings.ContentRefreshSeconds = 60;
        view->applyRefreshSettings();
        QVERIFY(view->autoRefreshEnabled());

        QVERIFY(QFile::remove(project + "/unseen.va"));
        QVERIFY(QFile::remove(project + "/extra/x.va"));
        QVERIFY(QDir(project + "/extra").removeRecursively());
        refresh->trigger();
        QCOMPARE(children(view, ProjectView::VerilogA), QStringList({"broken.va", "good.va", "models/deep.va"}));
    }

    void theSettingsDialogHasTheRefreshControls()
    {
        struct RefreshGuard {
            bool on = QucsSettings.ContentAutoRefresh;
            int seconds = QucsSettings.ContentRefreshSeconds;
            ~RefreshGuard() { QucsSettings.ContentAutoRefresh = on; QucsSettings.ContentRefreshSeconds = seconds; }
        } refreshGuard;
        QucsSettings.ContentAutoRefresh = true;
        QucsSettings.ContentRefreshSeconds = 3;
        QucsApp app(false);
        MainGuard guard(&app);
        QucsSettingsDialog dlg(&app);
        QCheckBox* on = nullptr;
        QSpinBox* seconds = nullptr;
        for (QCheckBox* c : dlg.findChildren<QCheckBox*>())
            if (c->toolTip().contains("listed again")) on = c;   // the auto-refresh switch
        for (QSpinBox* sp : dlg.findChildren<QSpinBox*>())
            if (sp->toolTip().contains("project's files are looked at")) seconds = sp;
        QVERIFY(on != nullptr && seconds != nullptr);
        QVERIFY(on->isChecked());
        QCOMPARE(seconds->value(), 3);
        QVERIFY(seconds->isEnabled());
        on->setChecked(false);
        QVERIFY(!seconds->isEnabled());              // nothing to set an interval for
        on->setChecked(true);
        seconds->setValue(15);
        QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
        QVERIFY(QucsSettings.ContentAutoRefresh);
        QCOMPARE(QucsSettings.ContentRefreshSeconds, 15);
        QCOMPARE(_settings::Get().item<int>("ContentRefreshSeconds"), 15);   // saved
        app.projectView()->applyRefreshSettings();   // as QucsApp does after the dialog
        QVERIFY(app.projectView()->autoRefreshEnabled());
    }

    // "Compile" on a .va file of the panel: OpenVAF on that file alone -
    // or on the .va files selected with it; not on other files.
    void compileOnAVaFile()
    {
        QucsSettings.OpenVAFExecutable = fakeCompiler();
        QucsSettings.QucsWorkDir.setPath(project);
        const QString calls = dir.filePath("calls.log");
        QFile::remove(calls);
        QFile::remove(project + "/good.osdi");

        QucsApp app(false);
        MainGuard guard(&app);
        ProjectView* view = app.projectView();
        view->setProjPath(project);
        app.show();
        view->expandAll();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        QMenu* fileMenu = menuWithAction(&app, "Open");
        QVERIFY(fileMenu != nullptr);
        QAction* compile = nullptr;
        for (QAction* a : fileMenu->actions())
            if (a->text() == "Compile") compile = a;
        QVERIFY(compile != nullptr);

        QStandardItemModel* m = view->model();
        // A build lists the project again (the new .osdi files): the rows
        // are made anew, so they are looked up each time.
        const auto vaRow = [&](const QString& name) {
            const QModelIndex va = m->index(ProjectView::VerilogA, 0);
            for (int i = 0; i < m->rowCount(va); ++i)
                if (view->filePath(m->index(i, 0, va)) == name) return m->index(i, 0, va);
            return QModelIndex();
        };
        QVERIFY(vaRow("good.va").isValid() && vaRow("models/deep.va").isValid());
        const auto menuOn = [&](const QModelIndex& row) {
            for (QMenu* menu : app.findChildren<QMenu*>()) menu->hide();
            return QMetaObject::invokeMethod(&app, "slotShowContentMenu",
                                             Q_ARG(QPoint, view->visualRect(row).center()));
        };
        const auto compiled = [&] {
            QFile f(calls);
            return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).trimmed().split('\n') : QStringList();
        };

        // Not on a schematic.
        QVERIFY(menuOn(m->index(0, 0, m->index(ProjectView::Schematics, 0))));
        QVERIFY(fileMenu->isVisible());
        QVERIFY(!compile->isVisible());

        // On good.va: that file.
        view->selectionModel()->clear();
        QVERIFY(menuOn(vaRow("good.va")));
        QVERIFY(compile->isVisible() && compile->isEnabled());
        QCOMPARE(compile->text(), QString("Compile"));
        compile->trigger();
        fileMenu->hide();
        QTRY_VERIFY_WITH_TIMEOUT(app.messages()->admsOutput->toPlainText().contains("Done:"), 15000);
        QString log = app.messages()->admsOutput->toPlainText();
        QVERIFY2(log.startsWith("Compiling " + QDir::toNativeSeparators(project + "/good.va")), qPrintable(log));
        QVERIFY2(log.contains("compiled " + project + "/good.va"), qPrintable(log));
        QCOMPARE(compiled(), QStringList({project + "/good.va"}));
        QVERIFY(QFileInfo::exists(project + "/good.osdi"));
        QVERIFY(!app.messages()->admsOutput->visibleRegion().isEmpty());   // the output in front

        // Two .va files selected: both, the clicked one among them.
        QFile::remove(calls);
        view->expandAll();
        view->selectionModel()->select(vaRow("good.va"), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        view->selectionModel()->select(vaRow("models/deep.va"), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        QVERIFY(menuOn(vaRow("models/deep.va")));
        QCOMPARE(compile->text(), QString("Compile 2 Files"));
        compile->trigger();
        fileMenu->hide();
        QTRY_VERIFY_WITH_TIMEOUT(app.messages()->admsOutput->toPlainText().contains("Done:"), 15000);
        QCOMPARE(compiled(), QStringList({project + "/good.va", project + "/models/deep.va"}));
        // A row outside the selection: that one alone.
        view->expandAll();
        QVERIFY(menuOn(vaRow("broken.va")));
        QCOMPARE(compile->text(), QString("Compile"));
        for (QMenu* menu : app.findChildren<QMenu*>()) menu->hide();
    }

    // A .va file open with changes: saved first when asked, then compiled.
    void compileSavesTheOpenFileFirst()
    {
        QucsSettings.OpenVAFExecutable = fakeCompiler();
        QucsSettings.QucsWorkDir.setPath(project);
        QFile::remove(dir.filePath("calls.log"));
        QucsApp app(false);
        MainGuard guard(&app);
        ProjectView* view = app.projectView();
        view->setProjPath(project);
        app.show();
        view->expandAll();
        QVERIFY(QTest::qWaitForWindowExposed(&app));

        QVERIFY(app.gotoPage(project + "/good.va", false, false));
        auto* doc = qobject_cast<TextDoc*>(app.DocumentTab->currentWidget());
        QVERIFY(doc != nullptr);
        doc->moveCursor(QTextCursor::End);
        doc->insertPlainText("// edited\n");
        QVERIFY(doc->getDocChanged());

        const QModelIndex good = view->model()->index(1, 0, view->model()->index(ProjectView::VerilogA, 0));
        view->selectionModel()->clear();
        QVERIFY(QMetaObject::invokeMethod(&app, "slotShowContentMenu", Q_ARG(QPoint, view->visualRect(good).center())));
        QMenu* fileMenu = menuWithAction(&app, "Open");
        QAction* compile = nullptr;
        for (QAction* a : fileMenu->actions())
            if (a->text() == "Compile") compile = a;
        QVERIFY(compile != nullptr);
        fileMenu->hide();
        // The question: "Save and Compile".
        bool asked = false;
        QTimer answer;
        connect(&answer, &QTimer::timeout, this, [&] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (box == nullptr) return;
            for (QAbstractButton* b : box->buttons())
                if (b->text() == "Save and Compile") { asked = true; b->click(); }
        });
        answer.start(20);
        compile->trigger();
        answer.stop();
        QVERIFY(asked);
        QVERIFY(!doc->getDocChanged());
        QFile saved(project + "/good.va");
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QVERIFY(QString::fromUtf8(saved.readAll()).contains("// edited"));
        QTRY_VERIFY_WITH_TIMEOUT(app.messages()->admsOutput->toPlainText().contains("Done:"), 15000);
    }

    // The message dock shares the bottom of the window with the simulation
    // console, the terminal and the Python shell; a build brings its
    // output to the front - the dock's tab and, inside it, the output's.
    void theBuildOutputComesToTheFront()
    {
        QucsSettings.OpenVAFExecutable = fakeCompiler();
        QucsSettings.QucsWorkDir.setPath(project);
        QucsApp app(false);
        MainGuard guard(&app);
        app.projectView()->setProjPath(project);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        MessageDock* messages = app.messages();
        messages->builderTabs->setCurrentWidget(messages->problems);   // a check was shown last
        messages->msgDock->show();
        app.simulationConsole()->showConsole();                        // then a simulation
        // A dock whose tab is not in front stays "visible", moved out of
        // sight: what is seen has a visible region.
        const auto inFront = [](QWidget* w) { return w->isVisible() && !w->visibleRegion().isEmpty(); };
        QCoreApplication::processEvents();
        QVERIFY(app.tabifiedDockWidgets(messages->msgDock).contains(app.simulationConsole()->dock()));
        QTRY_VERIFY(inFront(app.simulationConsole()));
        QVERIFY(!inFront(messages->admsOutput));

        QVERIFY(QMetaObject::invokeMethod(&app, "slotCMenuBuildAllVerilogA"));
        QTRY_VERIFY(inFront(messages->admsOutput));
        QVERIFY(!inFront(app.simulationConsole()));
        QTRY_VERIFY_WITH_TIMEOUT(messages->admsOutput->toPlainText().contains("Done:"), 15000);
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
