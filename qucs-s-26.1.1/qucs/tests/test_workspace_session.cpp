/*
 * The workspace kept from one run to the next (workspacesession.h,
 * Application Settings > Workspace): what is open when the window closes
 * - the project, the documents pane by pane with the one in front, the
 * panes' split, the docks - opened again by the next window; each part
 * as the settings say, none when they say none; a file gone left out;
 * after a crash, asked first. The settings' Workspace tab sets it all.
 */
#include <QtTest>
#include <QApplication>
#include <QCheckBox>
#include <QDockWidget>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>

#include "config.h"
#include "qucs.h"
#include "qucsdoc.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "settings.h"
#include "claudehistory.h"
#include "workspacesession.h"
#include "dialogs/qucssettingsdialog.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

namespace session = qucs_s::session;

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

// The settings as they are by default, and the workspace kept by no one,
// after each test.
struct Restored {
    ~Restored()
    {
        QucsSettings.RestoreWorkspace = QucsSettings.RestoreProject = QucsSettings.RestoreDocuments = true;
        QucsSettings.RestorePanels = QucsSettings.RestoreWindowGeometry = true;
        QucsApp::setWorkspaceKept(false);
        session::forget();
    }
};

void write(const QString& file, const QByteArray& text)
{
    QDir().mkpath(QFileInfo(file).absolutePath());
    QFile f(file);
    if (f.open(QIODevice::WriteOnly)) f.write(text);
}

QByteArray schematic()
{
    return "<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n</Components>\n<Wires>\n</Wires>\n"
           "<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n";
}

// Each pane's documents (their file names), row by row.
QList<QStringList> documentsByPane(QucsApp& app)
{
    QList<QStringList> out;
    for (ContextMenuTabWidget* pane : app.panes()) {
        QStringList names;
        for (int i = 0; i < pane->count(); ++i)
            if (QucsDoc* doc = QucsApp::docIn(pane->widget(i)))
                names << (doc->getDocName().isEmpty() ? QStringLiteral("untitled") : QFileInfo(doc->getDocName()).fileName());
        out << names;
    }
    return out;
}

QString currentOf(ContextMenuTabWidget* pane)
{
    QucsDoc* doc = QucsApp::docIn(pane->currentWidget());
    return doc != nullptr ? QFileInfo(doc->getDocName()).fileName() : QString();
}

} // namespace

class TestWorkspaceSession : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString project;

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
        QucsSettings.qucsWorkspaceDir.setPath(dir.filePath("workspace"));
        QucsSettings.QucsWorkDir = QucsSettings.qucsWorkspaceDir;
        QDir().mkpath(dir.filePath("workspace"));
        Module::registerModules();
        project = dir.filePath("workspace/amp_prj");
        for (const char* name : {"a.sch", "b.sch", "d.sch", "e.sch"}) write(project + "/" + name, schematic());
        write(project + "/c.txt", "notes\n");
    }

    // A project, four documents in three panes (two in the first row, one
    // below), the one in front of each, the active pane, a dock hidden:
    // kept at the close, and all of it as it was in the next window.
    void aWorkspaceIsKeptAndBroughtBack()
    {
        Restored restored;
        QucsApp::setWorkspaceKept(true);
        session::Workspace was;
        {
            QucsApp app(false);
            MainGuard main(&app);
            app.resize(1200, 800);
            app.show();
            QVERIFY(QTest::qWaitForWindowExposed(&app));
            app.openProject(project);
            QVERIFY(app.gotoPage(project + "/a.sch", false, false));
            QVERIFY(app.gotoPage(project + "/b.sch", false, false));
            QVERIFY(QMetaObject::invokeMethod(&app, "slotSplitPaneRight"));
            QVERIFY(app.gotoPage(project + "/c.txt", false, false));
            QVERIFY(QMetaObject::invokeMethod(&app, "slotSplitPaneDown"));
            QVERIFY(app.gotoPage(project + "/d.sch", false, false));
            QCOMPARE(app.panes().size(), 3);
            app.panes().at(0)->setCurrentIndex(0);   // a.sch in front there
            app.setActivePane(app.panes().at(1));
            app.findChild<QDockWidget*>(QStringLiteral("MainDock"))->hide();
            // Kept as the window closes.
            QCloseEvent close;
            QApplication::sendEvent(&app, &close);
            QVERIFY(close.isAccepted());
            was = session::saved();
        }
        QCOMPARE(QDir(was.project).dirName(), QStringLiteral("amp_prj"));
        QCOMPARE(was.documentCount(), 4);
        QVERIFY2(was.summary().contains("project amp_prj") && was.summary().contains("4 documents in 3 panes"), qPrintable(was.summary()));

        QucsApp app(false);
        MainGuard main(&app);
        app.resize(1200, 800);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        QVERIFY(app.restoreWorkspace().isEmpty());
        QCOMPARE(app.ProjName, QStringLiteral("amp"));
        QCOMPARE(documentsByPane(app), (QList<QStringList>{{"a.sch", "b.sch"}, {"c.txt"}, {"d.sch"}}));
        QCOMPARE(app.paneCell(app.panes().at(1)), QPoint(1, 0));
        QCOMPARE(app.paneCell(app.panes().at(2)), QPoint(0, 1));
        QCOMPARE(currentOf(app.panes().at(0)), QStringLiteral("a.sch"));
        QCOMPARE(app.activePane(), app.panes().at(1));
        QVERIFY(!app.findChild<QDockWidget*>(QStringLiteral("MainDock"))->isVisible());
        QVERIFY2(app.statusBar()->currentMessage().contains("4 documents"), qPrintable(app.statusBar()->currentMessage()));
        app.closeAllFiles();
    }

    // The window closed with a document maximized (a tab double-clicked):
    // kept are the panels and the panes' split from before, so the next
    // window does not open with them all hidden.
    void aMaximizedDocumentKeepsTheLayoutFromBefore()
    {
        Restored restored;
        QucsApp::setWorkspaceKept(true);
        QByteArray before;
        QList<int> columns;
        {
            QucsApp app(false);
            MainGuard main(&app);
            app.resize(1200, 800);
            app.show();
            QVERIFY(QTest::qWaitForWindowExposed(&app));
            QVERIFY(app.gotoPage(project + "/a.sch", false, false));
            QVERIFY(QMetaObject::invokeMethod(&app, "slotSplitPaneRight"));
            QVERIFY(app.gotoPage(project + "/b.sch", false, false));
            auto* row = qobject_cast<QSplitter*>(qobject_cast<QSplitter*>(app.centralWidget())->widget(0));
            QVERIFY(row != nullptr);
            row->setSizes({300, 600});
            app.findChild<QDockWidget*>(QStringLiteral("MainDock"))->show();
            app.terminalDockWidget()->show();
            QTest::qWait(50);
            columns = row->sizes();
            before = app.saveState(session::kWindowStateVersion);
            app.setDocumentMaximized(true);
            QVERIFY(app.findChild<QDockWidget*>(QStringLiteral("MainDock"))->isHidden());
            QTRY_COMPARE(row->sizes().value(0), 0);   // laid out: the other pane hidden
            QCloseEvent close;
            QApplication::sendEvent(&app, &close);
            QVERIFY(close.isAccepted());
        }
        QCOMPARE(session::savedWindowState(), before);
        const session::Workspace was = session::saved();
        QCOMPARE(was.panes.size(), 2);
        QCOMPARE(was.columnSizes.value(0), columns);

        QucsApp app(false);
        MainGuard main(&app);
        app.resize(1200, 800);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        QVERIFY(app.restoreWorkspace().isEmpty());
        QVERIFY(!app.isDocumentMaximized());
        QCOMPARE(app.panes().size(), 2);
        for (ContextMenuTabWidget* pane : app.panes()) QVERIFY(pane->isVisibleTo(&app));
        QVERIFY(!app.findChild<QDockWidget*>(QStringLiteral("MainDock"))->isHidden());
        QVERIFY(!app.terminalDockWidget()->isHidden());
        // A workspace restored while a document is maximized (its panels
        // only): all shown first, then as kept - the terminal, hidden before
        // maximizing, as the workspace has it.
        app.terminalDockWidget()->hide();
        app.setDocumentMaximized(true);
        QucsSettings.RestoreProject = QucsSettings.RestoreDocuments = false;
        app.restoreWorkspace();
        QucsSettings.RestoreProject = QucsSettings.RestoreDocuments = true;
        QVERIFY(!app.isDocumentMaximized());
        QVERIFY(app.activePane()->cornerWidget(Qt::TopRightCorner) == nullptr);
        QVERIFY(!app.findChild<QDockWidget*>(QStringLiteral("MainDock"))->isHidden());
        QVERIFY(!app.terminalDockWidget()->isHidden());
        // Close all but this, maximized: it stays so, the hidden pane gone,
        // and the sizes kept are the one pane's.
        app.setDocumentMaximized(true);
        QVERIFY(app.closeAllFiles(app.activePane()->currentIndex()));
        QVERIFY(app.isDocumentMaximized());
        QCOMPARE(app.panes().size(), 1);
        app.saveWorkspace();
        const session::Workspace one = session::saved();
        QCOMPARE(one.columnSizes.size(), 1);
        QCOMPARE(one.columnSizes.value(0).size(), 1);
        QVERIFY(one.columnSizes.value(0).value(0) > 0);
        // Close All: nothing is left to fill the window, the panels are back.
        QVERIFY(app.closeAllFiles());
        QVERIFY(!app.isDocumentMaximized());
        QVERIFY(!app.findChild<QDockWidget*>(QStringLiteral("MainDock"))->isHidden());
    }

    // Each part as the settings say: the project alone; the documents
    // alone; nothing with the whole off - and nothing kept then.
    void theSettingsSayWhatComesBack()
    {
        Restored restored;
        QucsApp::setWorkspaceKept(true);
        {
            QucsApp app(false);
            MainGuard main(&app);
            app.openProject(project);
            QVERIFY(app.gotoPage(project + "/a.sch", false, false));
            app.saveWorkspace();
            app.closeAllFiles();
        }
        {
            QucsSettings.RestoreDocuments = false;
            QucsApp app(false);
            MainGuard main(&app);
            app.restoreWorkspace();
            QCOMPARE(app.ProjName, QStringLiteral("amp"));
            QCOMPARE(documentsByPane(app), (QList<QStringList>{{"untitled"}}));
            app.closeAllFiles();
            QucsSettings.RestoreDocuments = true;
        }
        {
            QucsSettings.RestoreProject = false;
            QucsApp app(false);
            MainGuard main(&app);
            app.restoreWorkspace();
            QVERIFY(app.ProjName.isEmpty());
            QCOMPARE(documentsByPane(app), (QList<QStringList>{{"a.sch"}}));
            app.closeAllFiles();
            QucsSettings.RestoreProject = true;
        }
        {
            QucsSettings.RestoreWorkspace = false;
            QucsApp app(false);
            MainGuard main(&app);
            app.restoreWorkspace();
            QVERIFY(app.ProjName.isEmpty());
            QCOMPARE(documentsByPane(app), (QList<QStringList>{{"untitled"}}));
            // Off: nothing kept.
            session::forget();
            QVERIFY(app.gotoPage(project + "/b.sch", false, false));
            app.saveWorkspace();
            QVERIFY(session::saved().isEmpty());
            app.closeAllFiles();
        }
        // No window keeps its workspace but the application's (tests, the
        // MCP server).
        QucsSettings.RestoreWorkspace = true;
        QucsApp::setWorkspaceKept(false);
        QucsApp app(false);
        MainGuard main(&app);
        QVERIFY(app.gotoPage(project + "/b.sch", false, false));
        app.saveWorkspace();
        QVERIFY(session::saved().isEmpty());
        app.closeAllFiles();
    }

    // A file gone is left out, and said; one with an autosaved copy to
    // offer (after a crash) is held back and returned; a project gone is
    // not opened, and no message box waits. A project named on the command
    // line opens instead of the kept project and documents.
    void whatIsGoneOrHeldBackIsLeftOut()
    {
        Restored restored;
        QucsApp::setWorkspaceKept(true);
        {
            QucsApp app(false);
            MainGuard main(&app);
            app.openProject(project);
            for (const char* name : {"a.sch", "b.sch", "e.sch"}) QVERIFY(app.gotoPage(project + "/" + name, false, false));
            app.saveWorkspace();
            app.closeAllFiles();
        }
        QVERIFY(QFile::rename(project + "/e.sch", project + "/e.sch.away"));
        {
            QucsApp app(false);
            MainGuard main(&app);
            const QStringList held = app.restoreWorkspace(false, {QFileInfo(project + "/b.sch").canonicalFilePath()});
            QCOMPARE(held.size(), 1);
            QVERIFY(held.first().endsWith("b.sch"));
            QCOMPARE(documentsByPane(app), (QList<QStringList>{{"a.sch"}}));
            QVERIFY2(app.statusBar()->currentMessage().contains("not e.sch (no longer there)"), qPrintable(app.statusBar()->currentMessage()));
            app.closeAllFiles();
        }
        QVERIFY(QFile::rename(project + "/e.sch.away", project + "/e.sch"));
        {
            QucsApp app(false);
            MainGuard main(&app);
            app.restoreWorkspace(/*projectGiven=*/true);
            QVERIFY(app.ProjName.isEmpty());
            QCOMPARE(documentsByPane(app), (QList<QStringList>{{"untitled"}}));
            app.closeAllFiles();
        }
        // The project's folder gone: not opened, no box.
        session::Workspace gone = session::saved();
        gone.project = dir.filePath("workspace/gone_prj");
        session::save(gone, {});
        {
            QucsApp app(false);
            MainGuard main(&app);
            QTimer watch;
            bool boxed = false;
            connect(&watch, &QTimer::timeout, this, [&] {
                if (QWidget* modal = QApplication::activeModalWidget()) {
                    boxed = true;
                    modal->close();
                }
            });
            watch.start(20);
            app.restoreWorkspace();
            watch.stop();
            QVERIFY(!boxed);
            QVERIFY(app.ProjName.isEmpty());
            QVERIFY2(app.statusBar()->currentMessage().contains("not project gone_prj"), qPrintable(app.statusBar()->currentMessage()));
            app.closeAllFiles();
        }
    }

    // After a run that did not end cleanly, asked first: No opens neither
    // the project nor the documents.
    void afterACrashItAsksFirst()
    {
        Restored restored;
        QucsApp::setWorkspaceKept(true);
        {
            QucsApp app(false);
            MainGuard main(&app);
            app.openProject(project);
            QVERIFY(app.gotoPage(project + "/a.sch", false, false));
            app.saveWorkspace();
            app.closeAllFiles();
        }
        QucsApp app(false);
        MainGuard main(&app);
        QString asked;
        QTimer::singleShot(50, this, [&] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                asked = box->text();
                box->button(QMessageBox::No)->click();
            }
        });
        app.restoreWorkspace(false, {}, /*askFirst=*/true);
        QVERIFY2(asked.contains("did not exit cleanly") && asked.contains("project amp_prj"), qPrintable(asked));
        QVERIFY(app.ProjName.isEmpty());
        QCOMPARE(documentsByPane(app), (QList<QStringList>{{"untitled"}}));
        app.closeAllFiles();
    }

    // What reads back of a kept workspace: nothing of another version, no
    // pane outside the 2 x 2 grid, no size that is no number.
    void aKeptWorkspaceReadsBackSafely()
    {
        session::Workspace w;
        w.project = "/p/amp_prj";
        w.panes << session::Pane{0, 0, {"/p/a.sch", "/p/a.sch", "/p/b.sch"}, "/p/b.sch"} << session::Pane{0, 1, {"/p/c.txt"}, {}};
        w.rowSizes = {400};
        w.columnSizes = {{300, 500}};
        const session::Workspace back = session::Workspace::fromJson(w.toJson());
        QCOMPARE(back.project, w.project);
        QCOMPARE(back.panes.size(), 2);
        QCOMPARE(back.panes.at(0).documents, (QStringList{"/p/a.sch", "/p/b.sch"}));   // (once each)
        QCOMPARE(back.columnSizes, w.columnSizes);
        QJsonObject odd = w.toJson();
        QJsonArray panes = odd.value("panes").toArray();
        QJsonObject far = panes.at(1).toObject();
        far.insert("row", 5);
        panes.replace(1, far);
        odd.insert("panes", panes);
        odd.insert("row sizes", QJsonArray{"x"});
        const session::Workspace read = session::Workspace::fromJson(odd);
        QCOMPARE(read.panes.size(), 1);
        QVERIFY(read.rowSizes.isEmpty());
        odd.insert("version", 7);
        QVERIFY(session::Workspace::fromJson(odd).isEmpty());
    }

    // Application Settings > Workspace: the workspace folder and what a
    // project is are there; the parts of the workspace are set, and off
    // when the whole is; turned off, what was kept is forgotten; Forget It
    // forgets at once; the Claude Code dock's conversations too.
    void theWorkspaceTabSetsItAll()
    {
        Restored restored;
        QucsApp app(false);
        MainGuard main(&app);
        QucsSettingsDialog dlg(&app);
        auto* tabs = dlg.findChild<QTabWidget*>();
        QVERIFY(tabs != nullptr);
        auto* tab = dlg.findChild<QWidget*>(QStringLiteral("workspaceTab"));
        QVERIFY(tab != nullptr);
        QCOMPARE(tabs->tabText(tabs->indexOf(tab)), QStringLiteral("Workspace"));
        for (const char* name : {"workspaceFolder", "anyFolderIsProject", "restoreWorkspace", "restoreProject", "restoreDocuments",
                                 "restorePanels", "restoreWindowGeometry", "reopenConversations", "forgetWorkspace"})
            QVERIFY2(dlg.findChild<QWidget*>(QLatin1String(name)) != nullptr && tab->isAncestorOf(dlg.findChild<QWidget*>(QLatin1String(name))), name);
        QCOMPARE(dlg.findChild<QLineEdit*>(QStringLiteral("workspaceFolder"))->text(), QucsSettings.qucsWorkspaceDir.canonicalPath());
        auto* whole = dlg.findChild<QCheckBox*>(QStringLiteral("restoreWorkspace"));
        auto* documents = dlg.findChild<QCheckBox*>(QStringLiteral("restoreDocuments"));
        auto* geometry = dlg.findChild<QCheckBox*>(QStringLiteral("restoreWindowGeometry"));
        auto* conversations = dlg.findChild<QCheckBox*>(QStringLiteral("reopenConversations"));
        auto* kept = dlg.findChild<QLabel*>(QStringLiteral("keptWorkspace"));
        QVERIFY(whole->isChecked() && documents->isChecked() && documents->isEnabled());
        QCOMPARE(kept->text(), QStringLiteral("Nothing is kept now."));

        documents->setChecked(false);
        geometry->setChecked(false);
        conversations->setChecked(false);
        QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
        QVERIFY(!QucsSettings.RestoreDocuments && QucsSettings.RestoreWorkspace && !QucsSettings.RestoreWindowGeometry);
        QVERIFY(!_settings::Get().item<bool>("RestoreDocuments"));
        QVERIFY(!qucs_s::claude::history::reopenAtStart());

        // Something kept: said; off, forgotten.
        QucsApp::setWorkspaceKept(true);
        QVERIFY(app.gotoPage(project + "/a.sch", false, false));
        app.saveWorkspace();
        {
            QucsSettingsDialog again(&app);
            QVERIFY(!again.findChild<QCheckBox*>(QStringLiteral("restoreDocuments"))->isChecked());
            QVERIFY2(again.findChild<QLabel*>(QStringLiteral("keptWorkspace"))->text().startsWith("Kept: 1 document"),
                     qPrintable(again.findChild<QLabel*>(QStringLiteral("keptWorkspace"))->text()));
            again.findChild<QPushButton*>(QStringLiteral("forgetWorkspace"))->click();
            QVERIFY(session::saved().isEmpty());
            QCOMPARE(again.findChild<QLabel*>(QStringLiteral("keptWorkspace"))->text(), QStringLiteral("Nothing is kept now."));
            QVERIFY(!again.findChild<QPushButton*>(QStringLiteral("forgetWorkspace"))->isEnabled());
        }
        app.saveWorkspace();
        QVERIFY(!session::saved().isEmpty());
        whole->setChecked(false);
        QVERIFY(!documents->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
        QVERIFY(!QucsSettings.RestoreWorkspace);
        QVERIFY(session::saved().isEmpty());

        // The defaults: all of it on.
        QVERIFY(QMetaObject::invokeMethod(&dlg, "slotDefaultValues"));
        QVERIFY(whole->isChecked() && documents->isChecked() && geometry->isChecked() && conversations->isChecked());
        QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
        QVERIFY(QucsSettings.RestoreWorkspace && QucsSettings.RestoreDocuments && qucs_s::claude::history::reopenAtStart());
        app.closeAllFiles();
    }
};

QTEST_MAIN(TestWorkspaceSession)
#include "test_workspace_session.moc"
