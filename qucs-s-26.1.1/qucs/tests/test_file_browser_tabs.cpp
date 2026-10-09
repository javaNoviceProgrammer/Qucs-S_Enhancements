/*
 * The File Browser's tabs: a folder opened in a new tab (its menu's Open in
 * New Tab, a middle click - behind -, Command or Ctrl with the
 * double-click, the Options menu's New Tab); each tab with its own folder,
 * steps back and forward, view, filter, selection and Tree folders open;
 * the tab bar shown while there are two or more, its tabs closed (their
 * ✕, a middle click, its menu), moved; the tabs kept for the next start.
 */
#include <QtTest>
#include <QAction>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTreeView>

#include "filebrowser.h"
#include "isolated_settings.h"
#include "main.h"

namespace {

QAction* actionNamed(QMenu* menu, const QString& name)
{
    for (QAction* a : menu->actions())
        if (a->objectName() == name) return a;
    return nullptr;
}

QStringList actionTexts(QMenu* menu)
{
    QStringList out;
    for (QAction* a : menu->actions())
        if (!a->isSeparator()) out << a->text();
    return out;
}

} // namespace

class TestFileBrowserTabs : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString top;

    static void touch(const QString& path)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
    }
    static bool settled(FileBrowser& fb, const QString& oneOf)
    {
        return QTest::qWaitFor([&] { return fb.shownNames().contains(oneOf); }, 10000);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        top = QFileInfo(dir.path()).canonicalFilePath();
        touch(top + "/alpha/a1.sch");
        touch(top + "/alpha/a2.txt");
        touch(top + "/alpha/deep/d.sch");
        touch(top + "/beta/b1.sch");
        touch(top + "/beta/sub/s.sch");
        touch(top + "/gamma/g.txt");
    }

    // One tab: no bar. Another: the bar, each tab named by its folder.
    void aFolderOpensInANewTab()
    {
        FileBrowser fb;
        fb.resize(300, 600);
        fb.show();
        fb.setView(FileBrowser::View::List);
        fb.setLocation(top + "/alpha");
        QVERIFY(settled(fb, "a1.sch"));
        QCOMPARE(fb.tabCount(), 1);
        QVERIFY(!fb.tabBar()->isVisible());
        QCOMPARE(fb.tabLocations(), QStringList({top + "/alpha"}));

        // The folder's menu: Open, then Open in New Tab; a file's has none.
        QMenu* menu = fb.contextMenuFor(top + "/beta");
        QStringList texts = actionTexts(menu);
        QCOMPARE(texts.mid(0, 2), QStringList({"Open", "Open in New Tab"}));
        QSignalSpy moved(&fb, &FileBrowser::locationChanged);
        actionNamed(menu, "fbOpenInNewTab")->trigger();
        delete menu;
        QCOMPARE(fb.tabCount(), 2);
        QCOMPARE(fb.currentTab(), 1);
        QCOMPARE(fb.location(), top + "/beta");
        QCOMPARE(moved.count(), 1);
        QVERIFY(fb.tabBar()->isVisible());
        QCOMPARE(fb.tabBar()->count(), 2);
        QCOMPARE(fb.tabBar()->tabText(0), QStringLiteral("alpha"));
        QCOMPARE(fb.tabBar()->tabText(1), QStringLiteral("beta"));
        QCOMPARE(fb.tabBar()->tabToolTip(1), QDir::toNativeSeparators(top + "/beta"));
        QCOMPARE(fb.tabBar()->currentIndex(), 1);
        QVERIFY(settled(fb, "b1.sch"));
        QVERIFY(!fb.canGoBack());   // a tab's own steps: none yet
        menu = fb.contextMenuFor(top + "/beta/b1.sch");
        QVERIFY(actionNamed(menu, "fbOpenInNewTab") == nullptr);
        QVERIFY(!actionTexts(menu).contains("Open in New Tab"));
        delete menu;

        // Clicked: the other comes to the front, as it was.
        fb.tabBar()->setCurrentIndex(0);
        QCOMPARE(fb.currentTab(), 0);
        QCOMPARE(fb.location(), top + "/alpha");
        QVERIFY(settled(fb, "a1.sch"));
        // A tab's title follows its folder.
        fb.setLocation(top + "/alpha/deep");
        QCOMPARE(fb.tabBar()->tabText(0), QStringLiteral("deep"));
        QCOMPARE(fb.tabLocations(), QStringList({top + "/alpha/deep", top + "/beta"}));
        // The Options menu's New Tab: the folder shown, after this tab.
        auto* options = fb.findChild<QAction*>("fbNewTab");
        QVERIFY(options != nullptr);
        options->trigger();
        QCOMPARE(fb.tabCount(), 3);
        QCOMPARE(fb.currentTab(), 1);
        QCOMPARE(fb.tabLocations(), QStringList({top + "/alpha/deep", top + "/alpha/deep", top + "/beta"}));
        // A file given: its folder, the file selected.
        QCOMPARE(fb.openInNewTab(top + "/gamma/g.txt"), 2);
        QCOMPARE(fb.location(), top + "/gamma");
        QVERIFY(settled(fb, "g.txt"));
        QTRY_COMPARE(fb.selectedPath(), top + "/gamma/g.txt");
        QCOMPARE(fb.openInNewTab(top + "/none"), -1);
        QCOMPARE(fb.tabCount(), 4);
    }

    // Each tab its own: steps back and forward, view, filter, selection,
    // the Tree's folders open.
    void eachTabKeepsItsOwnState()
    {
        FileBrowser fb;
        fb.resize(300, 600);
        fb.show();
        while (fb.tabCount() > 1) fb.closeTab(fb.tabCount() - 1);
        fb.setView(FileBrowser::View::List);
        fb.setFilterText(QString());
        fb.setLocation(top + "/alpha");
        QVERIFY(settled(fb, "a1.sch"));
        fb.activate(top + "/alpha/deep");
        QCOMPARE(fb.location(), top + "/alpha/deep");
        QVERIFY(fb.canGoBack());

        fb.openInNewTab(top + "/beta");
        QVERIFY(!fb.canGoBack() && !fb.canGoForward());
        fb.setView(FileBrowser::View::Details);
        fb.setFilterText("b1");
        QVERIFY(settled(fb, "b1.sch"));
        QTRY_COMPARE(fb.shownNames(), QStringList({"b1.sch"}));
        fb.selectPath(top + "/beta/b1.sch");
        QCOMPARE(fb.selectedPath(), top + "/beta/b1.sch");

        fb.setCurrentTab(0);
        QCOMPARE(fb.location(), top + "/alpha/deep");
        QCOMPARE(fb.view(), FileBrowser::View::List);
        QVERIFY(fb.filterEdit()->text().isEmpty());
        QVERIFY(fb.canGoBack());
        QVERIFY(settled(fb, "d.sch"));
        fb.back();
        QCOMPARE(fb.location(), top + "/alpha");
        QCOMPARE(fb.selectedPath(), top + "/alpha/deep");   // where it came from
        QVERIFY(fb.canGoForward());

        fb.setCurrentTab(1);
        QCOMPARE(fb.location(), top + "/beta");
        QCOMPARE(fb.view(), FileBrowser::View::Details);
        QCOMPARE(fb.filterEdit()->text(), QStringLiteral("b1"));
        QTRY_COMPARE(fb.shownNames(), QStringList({"b1.sch"}));
        QTRY_COMPARE(fb.selectedPath(), top + "/beta/b1.sch");
        QVERIFY(!fb.canGoBack());

        // The Tree: the folders open in each tab.
        fb.setFilterText(QString());
        fb.setView(FileBrowser::View::Tree);
        QVERIFY(settled(fb, "sub"));
        auto* tree = qobject_cast<QTreeView*>(fb.currentView());
        QVERIFY(tree != nullptr);
        tree->expand(fb.indexOf(top + "/beta/sub"));
        QVERIFY(tree->isExpanded(fb.indexOf(top + "/beta/sub")));
        fb.setCurrentTab(0);
        QCOMPARE(fb.view(), FileBrowser::View::List);
        fb.setView(FileBrowser::View::Tree);
        QVERIFY(settled(fb, "deep"));
        QVERIFY(!tree->isExpanded(fb.indexOf(top + "/alpha/deep")));
        fb.setCurrentTab(1);
        QCOMPARE(fb.view(), FileBrowser::View::Tree);
        QTRY_VERIFY(tree->isExpanded(fb.indexOf(top + "/beta/sub")));
        fb.setCurrentTab(0);
        QTRY_VERIFY(!tree->isExpanded(fb.indexOf(top + "/beta/sub")));
        fb.setView(FileBrowser::View::List);
    }

    // A middle click on a folder: a tab behind; Command (Ctrl) with a
    // double-click: a tab in front. On a tab: closed.
    void clicksOpenAndCloseTabs()
    {
        FileBrowser fb;
        fb.resize(300, 600);
        fb.show();
        QVERIFY(QTest::qWaitForWindowExposed(&fb));
        while (fb.tabCount() > 1) fb.closeTab(fb.tabCount() - 1);
        fb.setView(FileBrowser::View::List);
        fb.setFilterText(QString());
        fb.setLocation(top);
        QVERIFY(settled(fb, "gamma"));
        QAbstractItemView* view = fb.currentView();
        const QRect beta = view->visualRect(fb.indexOf(top + "/beta"));
        QVERIFY(beta.isValid());
        QTest::mouseClick(view->viewport(), Qt::MiddleButton, Qt::NoModifier, beta.center());
        QCOMPARE(fb.tabCount(), 2);
        QCOMPARE(fb.currentTab(), 0);   // behind
        QCOMPARE(fb.location(), top);
        QCOMPARE(fb.tabLocations().at(1), top + "/beta");
        // A file: nothing.
        QVERIFY(QDir().exists(top + "/gamma"));
        fb.setLocation(top + "/gamma");
        QVERIFY(settled(fb, "g.txt"));
        const QRect file = view->visualRect(fb.indexOf(top + "/gamma/g.txt"));
        QTest::mouseClick(view->viewport(), Qt::MiddleButton, Qt::NoModifier, file.center());
        QCOMPARE(fb.tabCount(), 2);
        fb.back();
        QVERIFY(settled(fb, "alpha"));

        // Command (Ctrl) and a double-click: in front.
        const QRect alpha = view->visualRect(fb.indexOf(top + "/alpha"));
        QTest::mouseDClick(view->viewport(), Qt::LeftButton, Qt::ControlModifier, alpha.center());
        QTRY_COMPARE(fb.tabCount(), 3);
        QCOMPARE(fb.currentTab(), 1);
        QCOMPARE(fb.location(), top + "/alpha");
        QCOMPARE(fb.tabLocations(), QStringList({top, top + "/alpha", top + "/beta"}));
        // A double-click alone: entered, in this tab.
        QVERIFY(settled(fb, "deep"));
        emit view->activated(fb.indexOf(top + "/alpha/deep"));
        QCOMPARE(fb.location(), top + "/alpha/deep");
        QCOMPARE(fb.tabCount(), 3);

        // A middle click on a tab closes it; closing the one in front
        // brings the next.
        QTabBar* bar = fb.tabBar();
        QTest::mouseClick(bar, Qt::MiddleButton, Qt::NoModifier, bar->tabRect(2).center());
        QCOMPARE(fb.tabCount(), 2);
        QCOMPARE(fb.tabLocations(), QStringList({top, top + "/alpha/deep"}));
        QCOMPARE(fb.currentTab(), 1);
        fb.closeTab(1);   // in front: the one before comes
        QCOMPARE(fb.tabCount(), 1);
        QCOMPARE(fb.location(), top);
        QVERIFY(!bar->isVisible());
        fb.closeTab(0);   // the last stays
        QCOMPARE(fb.tabCount(), 1);
        // Its ✕.
        fb.openInNewTab(top + "/beta");
        emit bar->tabCloseRequested(0);
        QCOMPARE(fb.tabLocations(), QStringList({top + "/beta"}));
        QCOMPARE(fb.location(), top + "/beta");
    }

    // A tab's menu: a new one, close it, the others, those to its right.
    // Tabs moved: their states move with them.
    void theTabMenuAndMovedTabs()
    {
        FileBrowser fb;
        fb.resize(400, 600);
        fb.show();
        while (fb.tabCount() > 1) fb.closeTab(fb.tabCount() - 1);
        fb.setView(FileBrowser::View::List);
        fb.setLocation(top + "/alpha");
        fb.openInNewTab(top + "/beta");
        fb.openInNewTab(top + "/gamma");
        fb.setView(FileBrowser::View::Icons);   // gamma's
        QCOMPARE(fb.tabLocations(), QStringList({top + "/alpha", top + "/beta", top + "/gamma"}));

        // Moved: gamma first; still in front, its view kept.
        fb.tabBar()->moveTab(2, 0);
        QCOMPARE(fb.tabLocations(), QStringList({top + "/gamma", top + "/alpha", top + "/beta"}));
        QCOMPARE(fb.currentTab(), 0);
        QCOMPARE(fb.location(), top + "/gamma");
        fb.setCurrentTab(2);
        QCOMPARE(fb.location(), top + "/beta");
        QCOMPARE(fb.view(), FileBrowser::View::List);
        fb.setCurrentTab(0);
        QCOMPARE(fb.view(), FileBrowser::View::Icons);
        fb.setView(FileBrowser::View::List);

        QMenu* menu = fb.tabMenuFor(1);
        QVERIFY(actionNamed(menu, "fbCloseTab")->isEnabled());
        QVERIFY(actionNamed(menu, "fbCloseTabsRight")->isEnabled());
        actionNamed(menu, "fbCloseTabsRight")->trigger();
        delete menu;
        QCOMPARE(fb.tabLocations(), QStringList({top + "/gamma", top + "/alpha"}));
        menu = fb.tabMenuFor(1);
        QVERIFY(!actionNamed(menu, "fbCloseTabsRight")->isEnabled());
        actionNamed(menu, "fbCloseOtherTabs")->trigger();
        delete menu;
        QCOMPARE(fb.tabLocations(), QStringList({top + "/alpha"}));
        QCOMPARE(fb.location(), top + "/alpha");
        menu = fb.tabMenuFor(0);
        QVERIFY(!actionNamed(menu, "fbCloseTab")->isEnabled());
        QVERIFY(!actionNamed(menu, "fbCloseOtherTabs")->isEnabled());
        QVERIFY(actionTexts(menu).contains("New Tab"));
        delete menu;

        // The Recent view: a file shown in a new tab, in its folder.
        fb.setRecentFiles({top + "/beta/b1.sch"});
        fb.setView(FileBrowser::View::Recent);
        QCOMPARE(fb.tabBar()->tabText(0), QStringLiteral("Recent Documents"));
        menu = fb.contextMenuFor(top + "/beta/b1.sch");
        QAction* show = actionNamed(menu, "fbShowInNewTab");
        QVERIFY(show != nullptr);
        show->trigger();
        delete menu;
        QCOMPARE(fb.tabCount(), 2);
        QCOMPARE(fb.location(), top + "/beta");
        QCOMPARE(fb.view(), FileBrowser::View::List);   // the file system's view
        QVERIFY(settled(fb, "b1.sch"));
        QTRY_COMPARE(fb.selectedPath(), top + "/beta/b1.sch");
        fb.setCurrentTab(0);
        QCOMPARE(fb.view(), FileBrowser::View::Recent);
        fb.setView(FileBrowser::View::List);
    }

    // A tab's folder deleted while it is behind: the nearest folder above
    // it, when it comes to the front.
    void aTabWhoseFolderIsGone()
    {
        FileBrowser fb;
        while (fb.tabCount() > 1) fb.closeTab(fb.tabCount() - 1);
        fb.setView(FileBrowser::View::List);
        fb.setLocation(top + "/alpha");
        QVERIFY(QDir().mkpath(top + "/brief/inner"));
        fb.openInNewTab(top + "/brief/inner");
        fb.setCurrentTab(0);
        QVERIFY(QDir(top + "/brief").removeRecursively());
        fb.setCurrentTab(1);
        QCOMPARE(fb.location(), top);
        QCOMPARE(fb.tabLocations(), QStringList({top + "/alpha", top}));
        QCOMPARE(fb.tabBar()->tabText(1), QFileInfo(top).fileName());
        fb.setCurrentTab(0);
        QCOMPARE(fb.location(), top + "/alpha");
        fb.closeTab(1);
    }

    // The tabs kept for the next start: their folders, their views, the
    // one in front - a tab whose folder is gone left out.
    void theTabsAreKept()
    {
        {
            FileBrowser fb;
            while (fb.tabCount() > 1) fb.closeTab(fb.tabCount() - 1);
            fb.setView(FileBrowser::View::List);
            fb.setLocation(top + "/alpha");
            fb.openInNewTab(top + "/beta");
            fb.setView(FileBrowser::View::Details);
            QVERIFY(QDir().mkpath(top + "/doomed"));
            fb.openInNewTab(top + "/doomed");
            fb.openInNewTab(top + "/gamma");
            fb.setView(FileBrowser::View::Columns);
            fb.setCurrentTab(1);   // beta in front
        }
        QVERIFY(QDir(top + "/doomed").removeRecursively());
        {
            FileBrowser fb;
            QCOMPARE(fb.tabLocations(), QStringList({top + "/alpha", top + "/beta", top + "/gamma"}));
            QCOMPARE(fb.currentTab(), 1);
            QCOMPARE(fb.location(), top + "/beta");
            QCOMPARE(fb.view(), FileBrowser::View::Details);
            QVERIFY(!fb.tabBar()->isHidden());
            fb.setCurrentTab(2);
            QCOMPARE(fb.view(), FileBrowser::View::Columns);
            fb.setCurrentTab(0);
            QCOMPARE(fb.view(), FileBrowser::View::List);
            while (fb.tabCount() > 1) fb.closeTab(fb.tabCount() - 1);
        }
        {
            FileBrowser fb;
            QCOMPARE(fb.tabCount(), 1);
            QCOMPARE(fb.location(), top + "/alpha");
            QVERIFY(fb.tabBar()->isHidden());
            fb.openInNewTab(top + "/beta");
        }
        // A run given a workspace of its own: one tab, as it starts.
        QucsSettings.workspaceOfRun = top + "/gamma";
        {
            FileBrowser fb;
            QCOMPARE(fb.tabCount(), 1);
            fb.openInNewTab(top + "/alpha");
        }
        QucsSettings.workspaceOfRun.clear();
        {
            FileBrowser fb;   // (the run's tabs were not kept)
            QCOMPARE(fb.tabLocations(), QStringList({top + "/alpha", top + "/beta"}));
        }
    }
};

QTEST_MAIN(TestFileBrowserTabs)
#include "test_file_browser_tabs.moc"
