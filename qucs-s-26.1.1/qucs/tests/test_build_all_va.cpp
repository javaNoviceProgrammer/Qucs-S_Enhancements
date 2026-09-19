/*
 * "Build All..." on the Verilog-A row of the project tree: every .va file
 * of the project goes through the OpenVAF executable from the settings,
 * one after the other, with the output in the message dock. A fake
 * compiler script stands in for OpenVAF.
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
        write(project + "/other.sch", "<Qucs Schematic " PACKAGE_VERSION ">\n");
    }

    void buildsEveryFileAndTalliesFailures()
    {
        QucsSettings.OpenVAFExecutable = fakeCompiler();
        QucsSettings.QucsWorkDir.setPath(project);

        QucsApp app(false);
        QucsMain = &app;
        app.projectView()->setProjPath(project);
        app.projectView()->setExpanded(app.projectView()->model()->index(ProjectView::VerilogA, 0), true);
        QCOMPARE(children(app.projectView(), ProjectView::VerilogA), QStringList({"broken.va", "good.va"}));

        QVERIFY(QMetaObject::invokeMethod(&app, "slotCMenuBuildAllVerilogA"));
        QTRY_VERIFY_WITH_TIMEOUT(app.messages()->admsOutput->toPlainText().contains("Done:"), 15000);

        const QString log = app.messages()->admsOutput->toPlainText();
        QVERIFY2(log.contains("Building 2 Verilog-A file(s)"), qPrintable(log));
        QVERIFY2(log.contains("compiled " + project + "/good.va"), qPrintable(log));
        QVERIFY2(log.contains("error: cannot parse"), qPrintable(log));
        QVERIFY2(log.contains("exited with code 1"), qPrintable(log));
        QVERIFY2(log.contains("Done: 1 of 2 compiled, 1 failed."), qPrintable(log));

        // Both were compiled, in name order, from the project directory.
        QFile calls(dir.filePath("calls.log"));
        QVERIFY(calls.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(calls.readAll()).trimmed().split('\n'),
                 QStringList({project + "/broken.va", project + "/good.va"}));
        QVERIFY(QFileInfo::exists(project + "/good.osdi"));
        QVERIFY(!QFileInfo::exists(project + "/broken.osdi"));

        // The tree was refreshed: the new .osdi shows up under "Others", and
        // the row the user was working in stayed open.
        QVERIFY(children(app.projectView(), ProjectView::Others).contains("good.osdi"));
        QVERIFY(app.projectView()->isExpanded(app.projectView()->model()->index(ProjectView::VerilogA, 0)));
        QucsMain = nullptr;
    }

    // Right-clicking the "Verilog-A" row pops up a menu with "Build All...";
    // a file row still gets the file menu, and other category rows nothing.
    void contextMenuOnTheCategoryRow()
    {
        QucsApp app(false);
        QucsMain = &app;
        app.projectView()->setProjPath(project);
        app.show();
        ProjectView* view = app.projectView();
        view->expandAll();
        QVERIFY(QTest::qWaitForWindowExposed(&app));

        QMenu* buildMenu = nullptr;
        for (QMenu* m : app.findChildren<QMenu*>())
            for (QAction* a : m->actions())
                if (a->text() == "Build All...") buildMenu = m;
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
        QucsMain = nullptr;
    }

    void categoryLookup()
    {
        QucsApp app(false);
        QucsMain = &app;
        app.projectView()->setProjPath(project);
        QStandardItemModel* m = app.projectView()->model();
        const QModelIndex va = m->index(ProjectView::VerilogA, 0);
        QCOMPARE(app.projectView()->categoryOf(va), int(ProjectView::VerilogA));
        QCOMPARE(app.projectView()->categoryOf(m->index(0, 0, va)), int(ProjectView::VerilogA));   // a file
        QCOMPARE(app.projectView()->categoryOf(QModelIndex()), -1);
        QucsMain = nullptr;
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
