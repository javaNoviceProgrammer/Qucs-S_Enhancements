/*
 * Editor panes: documents side by side in up to a 2x2 grid. Splitting,
 * where documents open, moving them between panes, panes going when
 * empty, the active pane following clicks and focus, and the whole-
 * application operations (close all, save all, find) spanning every pane.
 */
#include <QtTest>
#include <QTemporaryDir>
#include <QAbstractButton>
#include <QAction>
#include <QDockWidget>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QMenu>
#include <QMenuBar>
#include <QSplitter>
#include <QStandardPaths>
#include <QTabBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>

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

// The panels docked in the window and shown.
QList<QDockWidget*> dockedAndShown(QMainWindow* window)
{
    QList<QDockWidget*> out;
    for (QDockWidget* dock : window->findChildren<QDockWidget*>())
        if (window->dockWidgetArea(dock) != Qt::NoDockWidgetArea && !dock->isFloating() && !dock->isHidden())
            out << dock;
    return out;
}

// Sizes the same but for a pixel or two (a layout's rounding).
bool nearly(const QList<int>& a, const QList<int>& b)
{
    if (a.size() != b.size()) return false;
    for (int i = 0; i < a.size(); ++i)
        if (qAbs(a.at(i) - b.at(i)) > 2) return false;
    return true;
}

// Waits until the panels' sizes hold still (a panel lays out its contents
// a moment after it is shown; slower under the sanitizers).
void settle(QMainWindow* window)
{
    auto sizes = [window] {
        QList<QSize> out;
        for (QDockWidget* dock : window->findChildren<QDockWidget*>()) out << dock->size();
        return out;
    };
    window->layout()->invalidate();   // (its contents' minimum grew meanwhile)
    window->layout()->activate();
    QList<QSize> last = sizes();
    for (int i = 0; i < 40; ++i) {
        QTest::qWait(100);
        const QList<QSize> now = sizes();
        if (now == last) return;
        last = now;
    }
}

QString sizesOf(const QList<int>& a, const QList<int>& b)
{
    auto text = [](const QList<int>& l) {
        QStringList t;
        for (int v : l) t << QString::number(v);
        return t.join(',');
    };
    return text(a) + " vs " + text(b);
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

    // As above, in a window with the focus in the document, closed by its
    // tab's button. The focus moved to the other pane's document as the
    // tab went, made that pane the active one, and the close then looked
    // there for a pane left empty: the empty one stayed, with no tab.
    void aPaneGoesWithItsLastDocumentHavingTheFocus()
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
        left->currentWidget()->setFocus();
        QTRY_COMPARE(app.activePane(), left);
        QAbstractButton* close = nullptr;
        for (auto side : {QTabBar::LeftSide, QTabBar::RightSide})
            if (auto* b = qobject_cast<QAbstractButton*>(left->tabBar()->tabButton(0, side))) close = b;
        QVERIFY(close != nullptr);
        QTest::mouseClick(close, Qt::LeftButton);
        QCOMPARE(app.panes().size(), 1);
        QCOMPARE(app.activePane(), right);
        QCOMPARE(tabTitles(right), QStringList{"b.sch"});
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
                                     "&Move Document to Next Pane", "&Next Pane", "Ma&ximize Document"}));
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

    // A double-click on a tab: its pane fills the window, the other panes
    // and the docked panels hidden, the toolbars and the status bar left;
    // a second one puts it all back, the panes at their sizes, the panels
    // where they were, the one in front of a tab group in front again.
    void aDoubleClickedTabFillsTheWindow()
    {
        QTest::failOnWarning(QRegularExpression("Invariant violated"));
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(1200, 800);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        ContextMenuTabWidget* left = app.activePane();
        QVERIFY(app.gotoPage(schA, false, false));
        app.slotSplitPaneRight();
        ContextMenuTabWidget* right = app.activePane();
        QVERIFY(app.gotoPage(schB, false, false));
        app.slotSplitPaneDown();
        ContextMenuTabWidget* below = app.activePane();
        QVERIFY(app.gotoPage(schC, false, false));
        auto* area = qobject_cast<QSplitter*>(app.centralWidget());
        auto* top = qobject_cast<QSplitter*>(area->widget(0));
        QVERIFY(area != nullptr && top != nullptr);
        area->setSizes({420, 180});
        top->setSizes({300, 500});
        // Panels on three sides; two of them in one tab group, the second
        // in front. (On the right one of fixed contents: the Claude Code
        // panel's grows as it learns of the project's git, which can come
        // while the document is maximized.)
        app.terminalDockWidget()->show();
        app.pythonDockWidget()->show();
        app.tabifyDockWidget(app.terminalDockWidget(), app.pythonDockWidget());
        app.pythonDockWidget()->raise();
        app.claudeDockWidget()->hide();
        auto* side = new QDockWidget("Side Panel", &app);
        side->setObjectName("SidePanel");
        side->setWidget(new QLabel("fixed", side));
        app.addDockWidget(Qt::RightDockWidgetArea, side);
        side->show();
        settle(&app);
        const QList<QDockWidget*> shown = dockedAndShown(&app);
        QVERIFY2(shown.size() >= 4, qPrintable(QString::number(shown.size())));
        QHash<QDockWidget*, QSize> dockSizes;
        for (QDockWidget* dock : shown) dockSizes[dock] = dock->size();
        QList<QToolBar*> bars;
        for (QToolBar* bar : app.toolbars())
            if (!bar->isHidden()) bars << bar;
        QVERIFY(!bars.isEmpty());
        const QList<int> rows = area->sizes(), columns = top->sizes();
        const int widthBefore = right->width();
        QVERIFY(!app.isDocumentMaximized());
        QVERIFY(!app.maximizeDocumentAction()->isChecked());

        QTest::mouseDClick(right->tabBar(), Qt::LeftButton, Qt::NoModifier, right->tabBar()->tabRect(0).center());
        QVERIFY(app.isDocumentMaximized());
        QCOMPARE(app.activePane(), right);
        QVERIFY(app.maximizeDocumentAction()->isChecked());
        QVERIFY(dockedAndShown(&app).isEmpty());
        QVERIFY(!left->isVisibleTo(&app));
        QVERIFY(!below->isVisibleTo(&app));
        QVERIFY(right->isVisibleTo(&app));
        for (QToolBar* bar : bars) QVERIFY2(!bar->isHidden(), qPrintable(bar->windowTitle()));
        QVERIFY(!app.statusBar()->isHidden());
        QVERIFY(!app.menuBar()->isHidden());
        // It fills the window, and the way back is in sight.
        QTRY_VERIFY(right->width() > widthBefore + 300);
        QTRY_VERIFY(right->width() >= app.centralWidget()->width() - 4);
        auto* button = qobject_cast<QToolButton*>(right->cornerWidget(Qt::TopRightCorner));
        QVERIFY(button != nullptr);
        QCOMPARE(button->objectName(), QStringLiteral("restorePanels"));
        QVERIFY(button->isVisibleTo(&app));
        QVERIFY2(button->toolTip().contains(app.maximizeDocumentAction()->shortcut().toString(QKeySequence::NativeText)),
                 qPrintable(button->toolTip()));
        QVERIFY(app.statusBar()->currentMessage().contains("Restore Panels"));
        // For a look at it: QUCS_TEST_GRAB=<dir>.
        const QString grabDir = qEnvironmentVariable("QUCS_TEST_GRAB");
        if (!grabDir.isEmpty()) {
            QTest::qWait(150);
            app.grab().save(grabDir + "/document-maximized.png");
        }
        // Another tab of the pane in front: still maximized.
        QVERIFY(app.gotoPage(textD, false, false));
        QCOMPARE(app.paneOf(QucsApp::documentWidget(app.findDoc(textD))), right);
        QVERIFY(app.isDocumentMaximized());

        QTest::mouseDClick(right->tabBar(), Qt::LeftButton, Qt::NoModifier, right->tabBar()->tabRect(0).center());
        QVERIFY(!app.isDocumentMaximized());
        QVERIFY(!app.maximizeDocumentAction()->isChecked());
        QCOMPARE(app.activePane(), right);
        QTRY_VERIFY(right->cornerWidget(Qt::TopRightCorner) == nullptr);
        QVERIFY(left->isVisibleTo(&app) && below->isVisibleTo(&app) && right->isVisibleTo(&app));
        QCOMPARE(dockedAndShown(&app).size(), shown.size());
        for (QDockWidget* dock : shown) QVERIFY2(!dock->isHidden(), qPrintable(dock->windowTitle()));
        QTRY_VERIFY2(nearly(area->sizes(), rows), qPrintable(sizesOf(area->sizes(), rows)));
        QTRY_VERIFY2(nearly(top->sizes(), columns), qPrintable(sizesOf(top->sizes(), columns)));
        for (QDockWidget* dock : shown)
            QTRY_VERIFY2(nearly({dock->width(), dock->height()}, {dockSizes[dock].width(), dockSizes[dock].height()}),
                         qPrintable(dock->windowTitle() + ": " + sizesOf({dock->width(), dock->height()},
                                                                         {dockSizes[dock].width(), dockSizes[dock].height()})));
        // The tab group as it was: Python in front.
        QTRY_VERIFY(!app.pythonDockWidget()->visibleRegion().isEmpty());
        QVERIFY(app.terminalDockWidget()->visibleRegion().isEmpty());
        if (!grabDir.isEmpty()) {
            QTest::qWait(150);
            app.grab().save(grabDir + "/document-restored.png");
        }
        QVERIFY(app.closeAllFiles());
    }

    // View > Panes > Maximize Document, and the button in the pane's corner;
    // a panel that came up in the meantime (as a simulation's console does)
    // stays when the others come back, and a toolbar hidden then stays hidden.
    void theMenuAndTheButtonTurnItOnAndOff()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(1100, 760);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        QVERIFY(app.gotoPage(schA, false, false));
        auto* mainDock = app.findChild<QDockWidget*>(QStringLiteral("MainDock"));
        QVERIFY(mainDock != nullptr);
        mainDock->show();
        // The terminal in a tab group with the Python shell, which is in
        // front; the terminal hidden.
        app.pythonDockWidget()->show();
        app.terminalDockWidget()->show();
        app.tabifyDockWidget(app.pythonDockWidget(), app.terminalDockWidget());
        app.terminalDockWidget()->hide();
        app.pythonDockWidget()->raise();
        QMenu* panes = menuTitled(&app, "&View", "&Panes");
        QVERIFY(panes != nullptr && panes->actions().contains(app.maximizeDocumentAction()));

        app.maximizeDocumentAction()->trigger();
        QVERIFY(app.isDocumentMaximized());
        QVERIFY(mainDock->isHidden());
        app.terminalDockWidget()->show();             // came up in the meantime
        QToolBar* bar = app.toolbars().constFirst();
        QVERIFY(!bar->isHidden());
        bar->hide();
        auto* button = qobject_cast<QToolButton*>(app.activePane()->cornerWidget(Qt::TopRightCorner));
        QVERIFY(button != nullptr);
        QTest::mouseClick(button, Qt::LeftButton);
        QVERIFY(!app.isDocumentMaximized());
        QVERIFY(!mainDock->isHidden());
        QVERIFY(!app.terminalDockWidget()->isHidden());
        QVERIFY(!app.pythonDockWidget()->isHidden());
        // The terminal, which came up last, in front of its group.
        QTRY_VERIFY(!app.terminalDockWidget()->visibleRegion().isEmpty());
        QVERIFY(app.pythonDockWidget()->visibleRegion().isEmpty());
        QVERIFY(bar->isHidden());
        bar->show();

        // The menu's action turns it off too, and a call does nothing more
        // a second time.
        app.maximizeDocumentAction()->trigger();
        app.setDocumentMaximized(true);
        QVERIFY(app.isDocumentMaximized());
        QVERIFY(mainDock->isHidden() && app.terminalDockWidget()->isHidden() && app.pythonDockWidget()->isHidden());
        app.maximizeDocumentAction()->trigger();
        QVERIFY(!app.isDocumentMaximized());
        app.setDocumentMaximized(false);
        QVERIFY(!mainDock->isHidden() && !app.terminalDockWidget()->isHidden());

        // In the tab's menu, checked while maximized.
        app.setDocumentMaximized(true);
        QStringList texts;
        bool checked = false;
        QTimer::singleShot(0, this, [&] {
            if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
                for (QAction* a : menu->actions())
                    if (!a->isSeparator()) texts << a->text();
                checked = menu->actions().contains(app.maximizeDocumentAction()) && app.maximizeDocumentAction()->isChecked();
                menu->close();
            }
        });
        app.activePane()->showContextMenu(app.activePane()->tabBar()->tabRect(0).center());
        QVERIFY2(texts.contains("Ma&ximize Document"), qPrintable(texts.join(" | ")));
        QVERIFY(checked);
        app.setDocumentMaximized(false);
        QVERIFY(app.closeAllFiles());
    }

    // What changes the panes shows them all again first: a split, a pane
    // closed, a document moved, a hidden pane made the active one (a
    // document in it brought to the front). So does closing the maximized
    // pane's last document, which would leave nothing in it.
    void paneChangesBringTheLayoutBack()
    {
        // (A document opened in a pane just shown again fitted itself to
        // no size: renderModel()'s checks said so.)
        QTest::failOnWarning(QRegularExpression("Invariant violated"));
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(1100, 760);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        ContextMenuTabWidget* left = app.activePane();
        QVERIFY(app.gotoPage(schA, false, false));
        app.slotSplitPaneRight();
        ContextMenuTabWidget* right = app.activePane();
        QVERIFY(app.gotoPage(schB, false, false));

        // A split.
        app.setActivePane(left);
        app.setDocumentMaximized(true);
        QVERIFY(!right->isVisibleTo(&app));
        app.slotSplitPaneDown();
        QVERIFY(!app.isDocumentMaximized());
        QCOMPARE(app.panes().size(), 3);
        for (ContextMenuTabWidget* pane : app.panes()) QVERIFY(pane->isVisibleTo(&app));
        // A pane closed (the new one, its placeholder only).
        app.setDocumentMaximized(true);
        app.slotClosePane();
        QVERIFY(!app.isDocumentMaximized());
        QCOMPARE(app.panes().size(), 2);
        QVERIFY(left->isVisibleTo(&app) && right->isVisibleTo(&app));
        // A document in a hidden pane brought to the front.
        app.setActivePane(left);
        app.setDocumentMaximized(true);
        app.showDocument(QucsApp::documentWidget(app.findDoc(schB)));
        QVERIFY(!app.isDocumentMaximized());
        QCOMPARE(app.activePane(), right);
        QVERIFY(left->isVisibleTo(&app));
        // A document moved to the next pane.
        app.setActivePane(left);
        QVERIFY(left->width() > 100 && left->height() > 100);   // sized at once
        QVERIFY(app.gotoPage(schC, false, false));
        app.setDocumentMaximized(true);
        app.slotMoveDocumentToNextPane();
        QVERIFY(!app.isDocumentMaximized());
        QCOMPARE(app.paneOf(QucsApp::documentWidget(app.findDoc(schC))), right);
        QVERIFY(left->isVisibleTo(&app) && right->isVisibleTo(&app));
        // The maximized pane's last document closed: the pane goes, and the
        // other comes back.
        app.setActivePane(left);
        QCOMPARE(left->count(), 1);
        app.setDocumentMaximized(true);
        app.slotFileClose(0);
        QVERIFY(!app.isDocumentMaximized());
        QCOMPARE(app.panes().size(), 1);
        QVERIFY(right->isVisibleTo(&app));
        QVERIFY(QPointer<ContextMenuTabWidget>(right)->cornerWidget(Qt::TopRightCorner) == nullptr);
        // One pane: its last document closed, an untitled takes its place,
        // and the panels are back.
        auto* mainDock = app.findChild<QDockWidget*>(QStringLiteral("MainDock"));
        mainDock->show();
        QVERIFY(app.closeAllFiles());
        QVERIFY(app.gotoPage(schA, false, false));
        QCOMPARE(app.activePane()->count(), 1);
        app.setDocumentMaximized(true);
        QVERIFY(mainDock->isHidden());
        app.slotFileClose(0);
        QVERIFY(!app.isDocumentMaximized());
        QVERIFY(!mainDock->isHidden());
        QCOMPARE(app.activePane()->count(), 1);
        QVERIFY(app.closeAllFiles());
    }

    // The editor of a property on the canvas lives in the document's
    // viewport while it is open. Closing one document takes it out first;
    // Close All and Close all left/right/but current deleted the documents
    // without, the editor went with the canvas, and the next use of
    // QucsApp::editText read freed memory (a GUI-monkey walk).
    void closingDocumentsLeavesThePropertyEditor()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        for (int round = 0; round < 2; ++round) {
            QVERIFY(app.gotoPage(schA, false, false));
            QVERIFY(app.gotoPage(schB, false, false));
            auto* doc = dynamic_cast<Schematic*>(app.getDoc());
            QVERIFY(doc != nullptr);
            // what editing a property on the canvas does with it
            app.editText->setParent(doc->viewport());
            app.editText->show();
            const QPointer<QLineEdit> editor(app.editText);

            if (round == 0) {
                QVERIFY(app.closeAllLeft(app.activePane()->indexOf(QucsApp::documentWidget(doc)) + 1));
            } else {
                QVERIFY(app.closeAllFiles());
            }
            QVERIFY(!editor.isNull());
            QCOMPARE(editor->parent(), static_cast<QObject*>(&app));
            QVERIFY(editor->isHidden());
        }
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
