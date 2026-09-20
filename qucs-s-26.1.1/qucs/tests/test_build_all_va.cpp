/*
 * The Content panel and "Build All" on its Verilog-A row: the panel lists
 * the project's files from any subdirectory, .osdi files under their own
 * category; Build All puts every .va file of the project through the
 * OpenVAF executable from the settings, one after the other, with the
 * output in the message dock. A fake compiler script stands in for OpenVAF.
 */
#include <QtTest>
#include <QTemporaryDir>
#include <QPlainTextEdit>
#include <QMenu>

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

// QucsMain must not outlive the QucsApp of a test that fails half-way.
struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

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
        QStringList names;
        QStandardItem* cat = view->model()->item(category, 0);
        for (int i = 0; cat && i < cat->rowCount(); ++i)
            names << cat->child(i, 0)->text();
        return names;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.maxUndo = 20;
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
        QTRY_VERIFY(buildMenu->isVisible());
        buildMenu->hide();

        const QModelIndex sch = m->index(ProjectView::Schematics, 0);
        const QPoint onOther = view->visualRect(sch).center();
        QVERIFY(QMetaObject::invokeMethod(&app, "slotShowContentMenu", Q_ARG(QPoint, onOther)));
        QTest::qWait(100);
        QVERIFY(!buildMenu->isVisible());

        const QPoint onFile = view->visualRect(m->index(0, 0, va)).center();
        QVERIFY(QMetaObject::invokeMethod(&app, "slotShowContentMenu", Q_ARG(QPoint, onFile)));
        QTest::qWait(100);
        QVERIFY(!buildMenu->isVisible());
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
};

int main(int argc, char** argv)
{
    int one = 1;   // QucsApp opens every argument as a document
    QApplication app(one, argv);
    TestBuildAllVerilogA test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_build_all_va.moc"
