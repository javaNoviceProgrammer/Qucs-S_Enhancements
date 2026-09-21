/*
 * Editor panes: documents side by side in up to a 2x2 grid. Splitting,
 * where documents open, moving them between panes, panes going when
 * empty, the active pane following clicks and focus, and the whole-
 * application operations (close all, save all, find) spanning every pane.
 */
#include <QtTest>
#include <QTemporaryDir>
#include <QAction>
#include <QMenu>
#include <QMenuBar>
#include <QSplitter>
#include <QStandardPaths>
#include <QTabBar>

#include "config.h"
#include "qucs.h"
#include "schematic.h"
#include "textdoc.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

namespace {
struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

QStringList tabTitles(ContextMenuTabWidget* pane)
{
    QStringList titles;
    for (int i = 0; i < pane->count(); ++i) titles << pane->tabText(i);
    return titles;
}

// A submenu of the menu bar: the top-level menus are not the window's
// children, so they are followed from the menu bar's actions.
QMenu* menuTitled(QMainWindow* window, const QString& top, const QString& title)
{
    for (QAction* a : window->menuBar()->actions()) {
        if (a->menu() == nullptr || a->menu()->title() != top) continue;
        for (QAction* sub : a->menu()->actions())
            if (sub->menu() != nullptr && sub->menu()->title() == title) return sub->menu();
    }
    return nullptr;
}
} // namespace

class TestPanes : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString schA, schB, schC, textD;

    // gotoPage(..., false, false): the copies keep the example's dataset
    // name, which gotoPage() would otherwise offer to rename in a dialog.
    QString write(const QString& path, const QString& text)
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return {};
        f.write(text.toUtf8());
        return path;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.firstRun = false;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.XyceExecutable = "xyce";
        QucsSettings.SpiceOpusExecutable = "spiceopus";
        QucsSettings.S4Qworkdir = dir.filePath("work");
        QucsSettings.tempFilesDir.setPath(dir.filePath("work"));
        QVERIFY(QDir().mkpath(dir.filePath("work")));
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        const QString example = QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/RF/Miscellaneous/RCL_resonance.sch");
        schA = dir.filePath("a.sch");
        schB = dir.filePath("b.sch");
        schC = dir.filePath("c.sch");
        QVERIFY(QFile::copy(example, schA));
        QVERIFY(QFile::copy(example, schB));
        QVERIFY(QFile::copy(example, schC));
        textD = write(dir.filePath("d.cir"), "* a netlist\nR1 1 0 1k\n.end\n");
    }

    void oneToBeginWithUpToFour()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        QCOMPARE(app.panes().size(), 1);
        ContextMenuTabWidget* first = app.activePane();
        QVERIFY(first != nullptr);
        QCOMPARE(app.DocumentTab, first);
        QCOMPARE(first->count(), 1);                    // the untitled document
        QVERIFY(app.canSplitRight());
        QVERIFY(app.canSplitDown());
        QVERIFY(!app.canClosePane());

        app.slotSplitPaneRight();
        QCOMPARE(app.panes().size(), 2);
        QVERIFY(app.activePane() != first);              // the new pane is the active one
        QCOMPARE(app.activePane(), app.panes().at(1));
        QCOMPARE(app.activePane()->count(), 1);          // with an untitled document, like the first
        QVERIFY(app.getDoc()->getDocName().isEmpty());
        QVERIFY(!app.canSplitRight());                   // the row is full
        QVERIFY(app.canSplitDown());
        QVERIFY(app.canClosePane());

        app.slotSplitPaneDown();
        QCOMPARE(app.panes().size(), 3);
        QCOMPARE(app.activePane(), app.panes().at(2));   // the new row, below
        QVERIFY(app.canSplitRight());
        QVERIFY(!app.canSplitDown());
        app.slotSplitPaneRight();
        QCOMPARE(app.panes().size(), 4);
        QVERIFY(!app.canSplitRight());
        QVERIFY(!app.canSplitDown());
        app.slotSplitPaneRight();                        // no-ops now
        app.slotSplitPaneDown();
        QCOMPARE(app.panes().size(), 4);
        // Two rows of two.
        auto* area = qobject_cast<QSplitter*>(app.centralWidget());
        QVERIFY(area != nullptr);
        QCOMPARE(area->count(), 2);
        for (int r = 0; r < 2; ++r) {
            auto* row = qobject_cast<QSplitter*>(area->widget(r));
            QVERIFY(row != nullptr);
            QCOMPARE(row->count(), 2);
        }
        // For a look at it: QUCS_TEST_GRAB=<dir> saves a picture of the
        // window with a document in each pane.
        const QString grabDir = qEnvironmentVariable("QUCS_TEST_GRAB");
        if (!grabDir.isEmpty()) {
            app.show();
            QVERIFY(QTest::qWaitForWindowExposed(&app));
            app.resize(1200, 760);
            QVERIFY(app.gotoPage(textD, false, false));
            app.setActivePane(app.panes().at(0));
            QVERIFY(app.gotoPage(schA, false, false));
            app.setActivePane(app.panes().at(1));
            QVERIFY(app.gotoPage(schB, false, false));
            app.setActivePane(app.panes().at(2));
            QVERIFY(app.gotoPage(schC, false, false));
            QTest::qWait(150);
            app.grab().save(grabDir + "/panes-2x2.png");
        }
        QVERIFY(app.closeAllFiles());
    }

    void documentsOpenInTheActivePane()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        ContextMenuTabWidget* left = app.activePane();
        app.slotSplitPaneRight();
        ContextMenuTabWidget* right = app.activePane();
        QVERIFY(app.gotoPage(schA, false, false));
        QCOMPARE(app.paneOf(QucsApp::documentWidget(app.getDoc())), right);
        QCOMPARE(tabTitles(right), QStringList{"a.sch"});   // the placeholder went
        QCOMPARE(tabTitles(left), QStringList{"untitled"});
        app.setActivePane(left);
        QVERIFY(app.gotoPage(textD, false, false));
        QCOMPARE(tabTitles(left), QStringList{"d.cir"});
        QCOMPARE(app.allDocuments().size(), 2);
        // A document that is open in another pane: that pane becomes
        // active and shows it, rather than a second copy opening.
        QVERIFY(app.gotoPage(schA, false, false));
        QCOMPARE(app.activePane(), right);
        QCOMPARE(app.allDocuments().size(), 2);
        int pos = -1;
        QVERIFY(app.findDoc(schA, &pos) != nullptr);
        QCOMPARE(pos, 0);
        QVERIFY(app.findTextDoc(textD) != nullptr);
        QVERIFY(app.findTextDoc(schA) == nullptr);      // a schematic, not a text document
        QVERIFY(app.closeAllFiles());
    }

    void aPaneGoesWithItsLastDocument()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        ContextMenuTabWidget* left = app.activePane();
        app.slotSplitPaneRight();
        ContextMenuTabWidget* right = app.activePane();
        QVERIFY(app.gotoPage(schA, false, false));
        QCOMPARE(app.paneOf(QucsApp::documentWidget(app.getDoc())), right);
        app.slotFileClose();                            // the only document of the right pane
        QCOMPARE(app.panes().size(), 1);
        QCOMPARE(app.activePane(), left);
        QCOMPARE(tabTitles(left), QStringList{"untitled"});
        // The last pane never goes: closing its last document leaves an untitled one.
        app.slotFileClose();
        QCOMPARE(app.panes().size(), 1);
        QCOMPARE(left->count(), 1);
        QVERIFY(app.closeAllFiles());
    }

    void aDocumentMovesToTheNextPane()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        ContextMenuTabWidget* left = app.activePane();
        QVERIFY(app.gotoPage(schA, false, false));
        QVERIFY(app.gotoPage(schB, false, false));
        QCOMPARE(tabTitles(left), (QStringList{"a.sch", "b.sch"}));
        app.getDoc()->setDocChanged(true);              // b.sch, the current one
        // One pane: moving splits first, and the document replaces the
        // new pane's placeholder.
        app.slotMoveDocumentToNextPane();
        QCOMPARE(app.panes().size(), 2);
        ContextMenuTabWidget* right = app.panes().at(1);
        QCOMPARE(app.activePane(), right);
        QCOMPARE(tabTitles(right), QStringList{"b.sch"});
        QCOMPARE(tabTitles(left), QStringList{"a.sch"});
        QVERIFY(app.getDoc()->getDocChanged());         // still marked modified
        QCOMPARE(app.paneOf(QucsApp::documentWidget(app.findDoc(schB))), right);
        // ...and back: the emptied pane goes.
        app.slotMoveDocumentToNextPane();
        QCOMPARE(app.panes().size(), 1);
        QCOMPARE(app.activePane(), left);
        QCOMPARE(tabTitles(left), (QStringList{"a.sch", "b.sch"}));
        app.getDoc()->setDocChanged(false);
        QVERIFY(app.closeAllFiles());
    }

    void closingAPaneHandsItsDocumentsToANeighbour()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        ContextMenuTabWidget* left = app.activePane();
        QVERIFY(app.gotoPage(schA, false, false));
        app.slotSplitPaneRight();
        QVERIFY(app.gotoPage(schB, false, false));
        QVERIFY(app.gotoPage(schC, false, false));
        QCOMPARE(app.panes().size(), 2);
        app.slotClosePane();
        QCOMPARE(app.panes().size(), 1);
        QCOMPARE(app.activePane(), left);
        QCOMPARE(tabTitles(left), (QStringList{"a.sch", "b.sch", "c.sch"}));
        QCOMPARE(app.allDocuments().size(), 3);
        // A pane holding only its placeholder closes without a trace.
        app.slotSplitPaneDown();
        QCOMPARE(app.panes().size(), 2);
        app.slotClosePane();
        QCOMPARE(app.panes().size(), 1);
        QCOMPARE(tabTitles(left), (QStringList{"a.sch", "b.sch", "c.sch"}));
        QVERIFY(app.closeAllFiles());
    }

    void closeAllAndSaveAllSpanEveryPane()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(schA, false, false));
        app.slotSplitPaneRight();
        QVERIFY(app.gotoPage(schB, false, false));
        app.slotSplitPaneDown();
        QVERIFY(app.gotoPage(schC, false, false));
        QCOMPARE(app.panes().size(), 3);
        QCOMPARE(app.allDocuments().size(), 3);
        for (QucsDoc* doc : app.allDocuments()) doc->setDocChanged(true);
        app.slotFileSaveAll();
        for (QucsDoc* doc : app.allDocuments()) QVERIFY(!doc->getDocChanged());
        QCOMPARE(app.allDocuments().size(), 3);
        // Close all: every document, and the panes left empty.
        QVERIFY(app.closeAllFiles());
        QCOMPARE(app.allDocuments().size(), 0);
        QCOMPARE(app.panes().size(), 1);
        QCOMPARE(app.activePane(), app.panes().first());
    }

    void closeAllButThisKeepsThatPane()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        ContextMenuTabWidget* left = app.activePane();
        QVERIFY(app.gotoPage(schA, false, false));
        app.slotSplitPaneRight();
        ContextMenuTabWidget* right = app.activePane();
        QVERIFY(app.gotoPage(schB, false, false));
        QVERIFY(app.gotoPage(schC, false, false));
        QVERIFY(app.closeAllFiles(right->indexOf(QucsApp::documentWidget(app.findDoc(schC)))));
        QCOMPARE(app.panes().size(), 1);
        QCOMPARE(app.activePane(), right);
        QVERIFY(!app.panes().contains(left));
        QCOMPARE(tabTitles(right), QStringList{"c.sch"});
        QVERIFY(app.closeAllFiles());
    }

    void clicksAndFocusChooseTheActivePane()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        app.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&app));
        ContextMenuTabWidget* left = app.activePane();
        QVERIFY(app.gotoPage(schA, false, false));
        app.slotSplitPaneRight();
        ContextMenuTabWidget* right = app.activePane();
        QVERIFY(app.gotoPage(schB, false, false));
        QCOMPARE(app.activePane(), right);
        // Focus in the left document.
        left->currentWidget()->setFocus();
        QTRY_COMPARE(app.activePane(), left);
        QVERIFY(app.currentSchematic() != nullptr);
        QCOMPARE(app.currentSchematic()->getDocName(), QDir::toNativeSeparators(schA));
        // A click on the right pane's tabs.
        QTest::mouseClick(right->tabBar(), Qt::LeftButton, Qt::NoModifier, right->tabBar()->tabRect(0).center());
        QTRY_COMPARE(app.activePane(), right);
        QCOMPARE(app.currentSchematic()->getDocName(), QDir::toNativeSeparators(schB));
        // Next Pane cycles.
        app.slotNextPane();
        QCOMPARE(app.activePane(), left);
        app.slotNextPane();
        QCOMPARE(app.activePane(), right);
        // Files dropped on a document open in its pane.
        app.openDroppedFiles({textD}, left->currentWidget());
        QTRY_VERIFY(app.findDoc(textD) != nullptr);
        QCOMPARE(app.paneOf(QucsApp::documentWidget(app.findDoc(textD))), left);
        QCOMPARE(app.activePane(), left);
        QVERIFY(app.closeAllFiles());
    }

    void theViewMenuHasThePaneActions()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        QMenu* panes = menuTitled(&app, "&View", "&Panes");
        QVERIFY(panes != nullptr);
        QStringList texts;
        for (QAction* a : panes->actions())
            if (!a->isSeparator()) texts << a->text();
        QCOMPARE(texts, (QStringList{"Split &Right", "Split &Down", "&Close Pane",
                                     "&Move Document to Next Pane", "&Next Pane"}));
        QAction* closePane = panes->actions().at(2);
        QAction* splitRight = panes->actions().at(0);
        QVERIFY(!closePane->isEnabled());                // one pane
        QVERIFY(splitRight->isEnabled());
        splitRight->trigger();
        QCOMPARE(app.panes().size(), 2);
        QVERIFY(closePane->isEnabled());
        QVERIFY(!splitRight->isEnabled());               // the row is full
        closePane->trigger();
        QCOMPARE(app.panes().size(), 1);
        QVERIFY(!closePane->isEnabled());
        QVERIFY(app.closeAllFiles());
    }
};

int main(int argc, char** argv)
{
    int one = 1;   // QucsApp opens every argument as a document
    QApplication app(one, argv);
    TestPanes test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_panes.moc"
