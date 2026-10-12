/*
 * The File Browser panel (filebrowser.h): files known by kind, with their
 * names and icons; a folder listed folders first, in natural order, the
 * hidden files hidden; folders entered and left, back, forward, up, the
 * path's buttons; each view showing the folder - the Tree opening folders
 * in place, the Columns view a column each and a file's preview; filters;
 * the recent documents; folders made, menus; its lists keeping their keys
 * from the window's shortcuts; and in the application, a tab of the left
 * dock that opens a schematic and follows the recent documents.
 */
#include <QtTest>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QColumnView>
#include <QHeaderView>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QShortcut>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include "config.h"
#include "extsimkernels/spicecompat.h"
#include "filebrowser.h"
#include "isolated_settings.h"
#include "namefilter.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "qucs.h"
#include "schematic.h"

using qucs_s::files::IconProvider;
using qucs_s::files::kindOf;

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

QImage imageOf(const QIcon& icon, int side)
{
    return icon.pixmap(QSize(side, side), 1.0).toImage();
}

} // namespace

class TestFileBrowser : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString top;    // the temporary folder, canonical
    QString root;   // top/amplifier_prj

    static void touch(const QString& path)
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
    }
    // The names shown once \a oneOf is among them (the file system's model
    // loads a folder in its own thread).
    static QStringList settled(FileBrowser& fb, const QString& oneOf)
    {
        static_cast<void>(QTest::qWaitFor([&] { return fb.shownNames().contains(oneOf); }, 10000));
        return fb.shownNames();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        top = QFileInfo(dir.path()).canonicalFilePath();
        root = top + "/amplifier_prj";
        QVERIFY(QDir().mkpath(root + "/models"));
        QVERIFY(QDir().mkpath(root + "/scratch"));
        QVERIFY(QDir().mkpath(top + "/filters_prj"));
        for (const char* f : {"amp.sch", "amp.dpl", "amp.dat", "amp.dat.ngspice", "opamp.sym", "bjt.va", "netlist.cir",
                              "notes.txt", "plot.png", "sparams.s2p", "R10.sch", "R2.sch", "mystery.xyz", ".hidden.txt"})
            touch(root + "/" + f);
        touch(root + "/models/inner.sch");
    }

    // What a file is, by its suffix: a name, a tag, whether Qucs-S makes
    // or reads it; icons that tell the kinds apart, at any size, for a light
    // and a dark theme.
    void filesAreKnownByKind()
    {
        const auto kind = [this](const QString& name) { return kindOf(QFileInfo(root + "/" + name)); };
        QCOMPARE(kind("amp.sch").name, QStringLiteral("Qucs-S schematic"));
        QCOMPARE(kind("amp.sch").tag, QStringLiteral("SCH"));
        QVERIFY(kind("amp.sch").qucs);
        QCOMPARE(kind("amp.dat.ngspice").name, QStringLiteral("Qucs-S dataset"));
        QCOMPARE(kind("sparams.s2p").name, QStringLiteral("Touchstone S-parameters"));
        QCOMPARE(kind("sparams.s2p").tag, QStringLiteral("S2P"));
        QVERIFY(kind("bjt.va").qucs);
        QVERIFY(kind("netlist.cir").qucs);
        QVERIFY(!kind("notes.txt").qucs);
        QCOMPARE(kind("mystery.xyz").name, QStringLiteral("XYZ file"));
        QCOMPARE(kind("mystery.xyz").tag, QStringLiteral("XYZ"));
        QCOMPARE(kindOf(QFileInfo(root + "/models")).name, QStringLiteral("Folder"));
        QCOMPARE(kindOf(QFileInfo(top + "/filters_prj")).name, QStringLiteral("Qucs-S project"));

        IconProvider icons;
        QCOMPARE(icons.type(QFileInfo(root + "/amp.sch")), QStringLiteral("Qucs-S schematic"));
        const auto icon = [&](const QString& path, int side) { return imageOf(icons.icon(QFileInfo(path)), side); };
        for (int side : {16, 32, 64}) {
            const QImage sch = icon(root + "/amp.sch", side);
            QVERIFY(!sch.isNull());
            QCOMPARE(sch.width(), side);
            QVERIFY(sch != icon(root + "/notes.txt", side));
            QVERIFY(sch != icon(root + "/models", side));
            QVERIFY(icon(root + "/models", side) != icon(top + "/filters_prj", side));
        }
        QCOMPARE(icon(root + "/amp.sch", 32), icon(root + "/R2.sch", 32));   // one kind, one icon
        // A dark theme: the pages go dark.
        const QPalette light = QApplication::palette();
        QPalette dark = light;
        dark.setColor(QPalette::Base, QColor(0x1e, 0x1e, 0x1e));
        dark.setColor(QPalette::Text, QColor(0xe0, 0xe0, 0xe0));
        const QImage before = icon(root + "/amp.sch", 32);
        QApplication::setPalette(dark);
        const QImage after = icon(root + "/amp.sch", 32);
        QApplication::setPalette(light);
        QVERIFY(before != after);
    }

    // A folder: its folders first, then its files in natural order (R2
    // before R10); hidden files when asked for; the count under it.
    void aFolderIsListed()
    {
        FileBrowser fb;
        fb.resize(300, 600);
        fb.setView(FileBrowser::View::List);
        fb.setLocation(root);
        const QStringList names = settled(fb, "sparams.s2p");
        QCOMPARE(names.mid(0, 2), QStringList({"models", "scratch"}));
        QVERIFY(names.indexOf("R2.sch") < names.indexOf("R10.sch"));
        QVERIFY(names.indexOf("amp.sch") < names.indexOf("bjt.va"));
        QVERIFY(!names.contains(".hidden.txt"));
        QTRY_VERIFY(fb.statusLabel()->text().startsWith("2 folders, 13 files"));
        fb.setShowHidden(true);
        QTRY_VERIFY_WITH_TIMEOUT(fb.shownNames().contains(".hidden.txt"), 10000);
        fb.setShowHidden(false);
        QTRY_VERIFY(!fb.shownNames().contains(".hidden.txt"));
    }

    // A double-click: a folder entered, a file opened. Back, forward and
    // up, each landing on where one came from; the path's buttons.
    void foldersAreEnteredAndLeft()
    {
        FileBrowser fb;
        fb.resize(300, 600);
        fb.show();
        fb.setView(FileBrowser::View::List);
        fb.setLocation(root);
        settled(fb, "amp.sch");
        QSignalSpy opened(&fb, &FileBrowser::openRequested);
        QSignalSpy moved(&fb, &FileBrowser::locationChanged);

        fb.activate(root + "/models");
        QCOMPARE(fb.location(), root + "/models");
        QCOMPARE(moved.count(), 1);
        QCOMPARE(settled(fb, "inner.sch"), QStringList({"inner.sch"}));
        QVERIFY(fb.canGoBack());
        QVERIFY(!fb.canGoForward());
        fb.back();
        QCOMPARE(fb.location(), root);
        QCOMPARE(fb.selectedPath(), root + "/models");
        QVERIFY(fb.canGoForward());
        fb.forward();
        QCOMPARE(fb.location(), root + "/models");
        fb.up();
        QCOMPARE(fb.location(), root);
        QCOMPARE(fb.selectedPath(), root + "/models");

        fb.activate(root + "/amp.sch");
        QCOMPARE(opened.count(), 1);
        QCOMPARE(opened.first().first().toString(), root + "/amp.sch");
        QCOMPARE(fb.location(), root);

        // A double-click in the view does the same.
        settled(fb, "scratch");
        emit fb.currentView()->activated(fb.indexOf(root + "/scratch"));
        QCOMPARE(fb.location(), root + "/scratch");

        // The path: a button for each folder down to this one; one clicked
        // goes there.
        const QList<QToolButton*> crumbs = fb.crumbs();
        QVERIFY(crumbs.size() >= 2);
        QCOMPARE(crumbs.last()->text(), QStringLiteral("scratch"));
        QCOMPARE(crumbs.at(crumbs.size() - 2)->text(), QStringLiteral("amplifier_prj"));
        crumbs.at(crumbs.size() - 2)->click();
        QCOMPARE(fb.location(), root);

        // A file given: its folder, the file selected.
        fb.setLocation(root + "/models/inner.sch");
        QCOMPARE(fb.location(), root + "/models");
        settled(fb, "inner.sch");
        fb.selectPath(root + "/models/inner.sch");
        QCOMPARE(fb.selectedPath(), root + "/models/inner.sch");
    }

    // Every view shows the folder. The Tree opens a folder in place; Icons
    // are large; Details has size, kind and date; the Columns view a column
    // for the folder and a file's preview. The view and the folder are kept
    // for the next time.
    void everyViewShowsTheFolder()
    {
        {
            FileBrowser fb;
            fb.resize(320, 600);
            fb.show();
            fb.setLocation(root);
            for (const auto view : {FileBrowser::View::Tree, FileBrowser::View::List, FileBrowser::View::Icons,
                                    FileBrowser::View::Details, FileBrowser::View::Columns}) {
                fb.setView(view);
                QCOMPARE(fb.view(), view);
                QCOMPARE(fb.pathOf(fb.currentView()->rootIndex()), root);
                const QStringList names = settled(fb, "amp.sch");
                QCOMPARE(names.first(), QStringLiteral("models"));
            }

            fb.setView(FileBrowser::View::Tree);
            auto* tree = qobject_cast<QTreeView*>(fb.currentView());
            QVERIFY(tree != nullptr);
            fb.activate(root + "/models");
            QCOMPARE(fb.location(), root);   // opened in place
            QVERIFY(tree->isExpanded(fb.indexOf(root + "/models")));
            QTRY_VERIFY_WITH_TIMEOUT(fb.indexOf(root + "/models/inner.sch").isValid(), 10000);
            fb.selectPath(root + "/models/inner.sch");
            QCOMPARE(fb.selectedPath(), root + "/models/inner.sch");
            fb.activate(root + "/models");
            QVERIFY(!tree->isExpanded(fb.indexOf(root + "/models")));

            fb.setView(FileBrowser::View::Icons);
            auto* icons = qobject_cast<QListView*>(fb.currentView());
            QVERIFY(icons != nullptr);
            QCOMPARE(icons->viewMode(), QListView::IconMode);
            QVERIFY(icons->iconSize().width() >= 48);

            fb.setView(FileBrowser::View::Details);
            auto* details = qobject_cast<QTreeView*>(fb.currentView());
            QVERIFY(details != nullptr);
            QVERIFY(!details->isHeaderHidden());
            QCOMPARE(details->header()->count(), 4);
            const QModelIndex sch = fb.indexOf(root + "/amp.sch");
            QCOMPARE(sch.sibling(sch.row(), 2).data().toString(), QStringLiteral("Qucs-S schematic"));
            // Sorted by kind: folders still first.
            details->sortByColumn(2, Qt::AscendingOrder);
            QTRY_COMPARE(fb.shownNames().first(), QStringLiteral("models"));
            details->sortByColumn(0, Qt::AscendingOrder);

            fb.setView(FileBrowser::View::Columns);
            auto* columns = qobject_cast<QColumnView*>(fb.currentView());
            QVERIFY(columns != nullptr);
            // Its first column is the folder, not a preview of nothing.
            QTRY_VERIFY_WITH_TIMEOUT(
                [&] {
                    for (auto* column : columns->findChildren<QAbstractItemView*>())
                        if (column->isVisible() && column->model() != nullptr && fb.pathOf(column->rootIndex()) == root) return true;
                    return false;
                }(),
                10000);
            fb.selectPath(root + "/amp.sch");
            QTRY_VERIFY(fb.findChild<QLabel*>("fbPreviewFacts")->text().contains("Qucs-S schematic"));
            QCOMPARE(fb.findChild<QLabel*>("fbPreviewName")->text(), QStringLiteral("amp.sch"));
            fb.setView(FileBrowser::View::Details);
        }
        FileBrowser again;
        QCOMPARE(again.view(), FileBrowser::View::Details);
        QCOMPARE(again.location(), root);
    }

    // The Columns view on a folder with nothing in it: Qt's shows its
    // preview column alone, and a current entry left from the folder
    // before, or the keys' (Right with none went to the top of the file
    // system), crashed it once its scroll's animation ended. The cursor
    // stays in the folder shown.
    void theColumnsViewTakesAnEmptyFolder()
    {
        const QString empty = top + "/nothing_here";
        QVERIFY(QDir().mkpath(empty));
        FileBrowser fb;
        fb.resize(320, 600);
        fb.show();
        fb.setView(FileBrowser::View::Columns);
        auto* columns = qobject_cast<QColumnView*>(fb.currentView());
        QVERIFY(columns != nullptr);
        fb.setLocation(root);
        settled(fb, "amp.sch");
        QTRY_VERIFY_WITH_TIMEOUT(fb.indexOf(root + "/models/inner.sch").isValid(), 10000);

        // An entry deep in the folder, scrolled to, and then the empty
        // folder: none current there.
        columns->setCurrentIndex(fb.indexOf(root + "/models"));
        columns->setCurrentIndex(fb.indexOf(root + "/models/inner.sch"));
        fb.setLocation(empty);
        QVERIFY(!columns->currentIndex().isValid());
        QTest::qWait(500);   // (the scroll's animation ends)
        // Keys move nothing there.
        for (const auto key : {Qt::Key_Right, Qt::Key_Down, Qt::Key_Left, Qt::Key_Up, Qt::Key_End, Qt::Key_Home}) {
            QTest::keyClick(columns, key);
            QVERIFY(!columns->currentIndex().isValid());
        }
        QTest::qWait(500);
        fb.selectPath(empty);
        QVERIFY(!columns->currentIndex().isValid());

        // With entries and none current, Right goes to the first of them,
        // not out of the folder.
        fb.setLocation(root);
        QCOMPARE(settled(fb, "amp.sch").first(), QStringLiteral("models"));
        columns->setCurrentIndex(QModelIndex());
        QTest::keyClick(columns, Qt::Key_Right);
        QCOMPARE(fb.pathOf(columns->currentIndex()), root + "/models");
        QTest::qWait(500);
    }

    // The name typed filters the entries - in the flat views the folders
    // too, in the Tree only the files -, as text or as a regular
    // expression; "only Qucs-S files" leaves the schematics, datasets,
    // netlists, sources and S-parameters, and the folders.
    void theEntriesAreFiltered()
    {
        FileBrowser fb;
        fb.resize(300, 600);
        fb.setView(FileBrowser::View::List);
        fb.setLocation(root);
        settled(fb, "amp.sch");
        fb.setFilterText("AMP");   // (anywhere in the name)
        QTRY_COMPARE(fb.shownNames(), QStringList({"amp.dat", "amp.dat.ngspice", "amp.dpl", "amp.sch", "opamp.sym"}));
        QTRY_VERIFY(fb.statusLabel()->text().contains("filtered"));
        fb.setView(FileBrowser::View::Tree);
        QTRY_COMPARE(fb.shownNames(), QStringList({"models", "scratch", "amp.dat", "amp.dat.ngspice", "amp.dpl", "amp.sch", "opamp.sym"}));
        fb.setView(FileBrowser::View::List);
        // A regular expression, whatever the case: the folders too.
        fb.setFilterText("^amp\\.");
        QTRY_COMPARE(fb.shownNames(), QStringList({"amp.dat", "amp.dat.ngspice", "amp.dpl", "amp.sch"}));
        fb.setFilterText("^r\\d+\\.sch$");
        QTRY_COMPARE(fb.shownNames(), QStringList({"R2.sch", "R10.sch"}));
        fb.setFilterText("\\.(va|cir)$|^mod");
        QTRY_COMPARE(fb.shownNames(), QStringList({"models", "bjt.va", "netlist.cir"}));
        fb.setView(FileBrowser::View::Tree);
        QTRY_COMPARE(fb.shownNames(), QStringList({"models", "scratch", "bjt.va", "netlist.cir"}));
        fb.setView(FileBrowser::View::List);
        // No regular expression: the text as typed, and the box says why.
        QAction* warning = fb.filterEdit()->findChild<QAction*>("nameFilterNoRegex");
        QVERIFY(warning != nullptr);
        QVERIFY(!warning->isVisible());
        QVERIFY(fb.filterEdit()->toolTip().contains("regular expression"));
        fb.setFilterText("*.sch");
        QTRY_COMPARE(fb.shownNames(), QStringList());
        QVERIFY(warning->isVisible());
        QVERIFY2(warning->toolTip().startsWith("Not a regular expression (") && warning->toolTip().contains("at character 1"),
                 qPrintable(warning->toolTip()));
        QCOMPARE(fb.filterEdit()->toolTip(), warning->toolTip());
        fb.setFilterText("amp\\.sch");
        QTRY_COMPARE(fb.shownNames(), QStringList({"amp.sch"}));
        QVERIFY(!warning->isVisible());
        fb.setFilterText(QString());
        fb.setQucsFilesOnly(true);
        QTRY_VERIFY(!fb.shownNames().contains("notes.txt"));
        const QStringList names = fb.shownNames();
        for (const char* shown : {"models", "scratch", "amp.sch", "amp.dat.ngspice", "bjt.va", "netlist.cir", "sparams.s2p", "opamp.sym"})
            QVERIFY2(names.contains(shown), shown);
        for (const char* hidden : {"plot.png", "mystery.xyz"}) QVERIFY2(!names.contains(hidden), hidden);
        fb.setQucsFilesOnly(false);
        QTRY_VERIFY(fb.shownNames().contains("notes.txt"));
    }

    // What a "Filter by name" box finds: a name that holds the text, or in
    // which the text finds a match as a regular expression, whatever the
    // case; a text that is no regular expression is only the text. A
    // path: it, or the name at its end.
    void aNameIsFoundByTextOrRegularExpression()
    {
        using qucs_s::files::NameFilter;
        QVERIFY(NameFilter().isEmpty() && NameFilter().matches("anything") && NameFilter().matchesPath("a/b"));
        QVERIFY(NameFilter("AMP").matches("opamp.sym"));
        QVERIFY(NameFilter("^amp").matches("Amp.sch"));
        QVERIFY(!NameFilter("^amp").matches("opamp.sym"));
        QVERIFY(NameFilter("\\.sch$").matches("amp.SCH"));
        QVERIFY(!NameFilter("\\.sch$").matches("amp.sch.bak"));
        QVERIFY(NameFilter("amp|filter").matches("filter_prj"));
        QVERIFY(NameFilter("r\\d{2}").matches("R10.sch") && !NameFilter("r\\d{2}").matches("R2.sch"));
        QVERIFY(NameFilter("^ÄRGER").matches("ärger.sch"));   // (a case beyond ASCII)
        // Text a regular expression reads otherwise: found as typed too.
        QVERIFY(NameFilter("file(1)").matches("file(1).sch"));
        QVERIFY(NameFilter("a.c").matches("a.c") && NameFilter("a.c").matches("abc"));
        QVERIFY(NameFilter("[draft]").matches("amp [draft].sch"));
        // No regular expression: only the text, and why.
        const NameFilter glob("*.sch");
        QVERIFY(!glob.error().isEmpty());
        QVERIFY2(glob.error().endsWith("at character 1"), qPrintable(glob.error()));
        QVERIFY(!glob.matches("amp.sch"));
        QVERIFY(glob.matches("all*.sch.txt"));
        QVERIFY(!NameFilter("amp(").matches("amp.sch") && NameFilter("amp(").matches("amp(2).sch"));
        QVERIFY2(NameFilter("amp(").error().endsWith("at character 4"), qPrintable(NameFilter("amp(").error()));
        QVERIFY2(NameFilter("x{3,1}").error().endsWith("at character 5"), qPrintable(NameFilter("x{3,1}").error()));
        QVERIFY(NameFilter("^amp").error().isEmpty() && NameFilter().error().isEmpty());
        // A path: it, or its last name (a folder's ends in '/').
        QVERIFY(NameFilter("^amp").matchesPath("models/amp.sch"));
        QVERIFY(NameFilter("^models/").matchesPath("models/amp.sch"));
        QVERIFY(NameFilter("^docs$").matchesPath("docs/"));
        QVERIFY(NameFilter("^docs$").matchesPath("a/docs/"));
        QVERIFY(!NameFilter("^models$").matchesPath("models/amp.sch"));
        QVERIFY(!NameFilter("^x").matchesPath("models/amp.sch"));
    }

    // The Recent view: the documents opened last that are still there; a
    // double-click opens one; a folder asked for goes back to the files.
    void recentDocumentsAreListed()
    {
        FileBrowser fb;
        fb.resize(300, 600);
        fb.setView(FileBrowser::View::List);
        fb.setRecentFiles({root + "/amp.sch", root + "/gone.sch", root + "/bjt.va"});
        fb.setView(FileBrowser::View::Recent);
        QCOMPARE(fb.shownNames(), QStringList({"amp.sch", "bjt.va"}));
        QVERIFY(fb.statusLabel()->text().startsWith("2 recent"));
        QSignalSpy opened(&fb, &FileBrowser::openRequested);
        emit fb.currentView()->activated(fb.indexOf(root + "/bjt.va"));
        QCOMPARE(opened.count(), 1);
        QCOMPARE(opened.first().first().toString(), root + "/bjt.va");
        fb.setFilterText("amp");
        QCOMPARE(fb.shownNames(), QStringList({"amp.sch"}));
        fb.setFilterText("\\.VA$");
        QCOMPARE(fb.shownNames(), QStringList({"bjt.va"}));
        fb.setFilterText(QString());
        fb.setLocation(root + "/models");
        QCOMPARE(fb.view(), FileBrowser::View::List);
        QCOMPARE(fb.location(), root + "/models");
    }

    // Folders made with the next free name; the menus of a file, a folder
    // and of nothing; a path copied; names edited in place.
    void entriesAreMadeAndTheirMenusOffered()
    {
        FileBrowser fb;
        fb.setView(FileBrowser::View::List);
        fb.setLocation(root);
        const QString made = fb.createFolder(root + "/scratch");
        QCOMPARE(made, root + "/scratch/New Folder");
        QVERIFY(QFileInfo(made).isDir());
        QCOMPARE(fb.createFolder(root + "/scratch"), root + "/scratch/New Folder 2");
        QVERIFY(fb.createFolder(root + "/no such folder").isEmpty());
        QDir(root + "/scratch/New Folder").removeRecursively();
        QDir(root + "/scratch/New Folder 2").removeRecursively();

        const auto texts = [](QMenu* menu) {
            QStringList all;
            for (QAction* a : menu->actions())
                if (!a->isSeparator()) all << a->text();
            delete menu;
            return all;
        };
        const QStringList file = texts(fb.contextMenuFor(root + "/amp.sch"));
        for (const char* action : {"Open", "Open with the System's Application", "Copy Path", "Rename…", "Move to Trash…"})
            QVERIFY2(file.contains(action), action);
        const QStringList folder = texts(fb.contextMenuFor(root + "/models"));
        QVERIFY(folder.contains("Open"));
        QVERIFY(folder.contains("New Folder…"));
        QVERIFY(!folder.contains("Open with the System's Application"));
        QVERIFY(texts(fb.contextMenuFor(QString())).contains("New Folder…"));

        QMenu* menu = fb.contextMenuFor(root + "/amp.sch");
        for (QAction* a : menu->actions())
            if (a->text() == "Copy Path") a->trigger();
        delete menu;
        QCOMPARE(QApplication::clipboard()->text(), QDir::toNativeSeparators(root + "/amp.sch"));

        // New Zip…, in every menu: a new archive asked for, to go in the
        // folder the menu is of - a file's, the folder's, the one shown.
        QSignalSpy asked(&fb, &FileBrowser::newArchiveRequested);
        const auto choose = [](QMenu* m, const QString& text) {
            int found = 0;
            for (QAction* a : m->actions())
                if (a->text() == text) {
                    a->trigger();
                    ++found;
                }
            return found;
        };
        for (const QString& on : {root + "/amp.sch", root + "/models", QString()}) {
            QMenu* m = fb.contextMenuFor(on);
            QCOMPARE(choose(m, "New Zip…"), 1);
            delete m;
        }
        auto* options = fb.findChild<QMenu*>("fbOptions");
        QVERIFY(options != nullptr);
        QCOMPARE(choose(options, "New Zip…"), 1);
        QCOMPARE(asked.count(), 4);
        QCOMPARE(asked.at(0).first().toString(), root);
        QCOMPARE(asked.at(1).first().toString(), root + "/models");
        QCOMPARE(asked.at(2).first().toString(), fb.location());
        QCOMPARE(asked.at(3).first().toString(), fb.location());
        QCOMPARE(fb.location(), root);
        QVERIFY(!fb.fileModel()->isReadOnly());   // (a name is edited in place)
    }

    // A list keeps its keys: the window's shortcut on Down (as Qucs-S's for
    // a diagram's marker) does not take them. Backspace goes up.
    void theListsKeepTheirKeys()
    {
        QWidget window;
        auto* layout = new QVBoxLayout(&window);
        auto* fb = new FileBrowser;
        layout->addWidget(fb);
        auto* marker = new QShortcut(QKeySequence(Qt::Key_Down), &window);
        QSignalSpy fired(marker, &QShortcut::activated);
        window.resize(300, 600);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        fb->setView(FileBrowser::View::List);
        fb->setLocation(root);
        settled(*fb, "amp.sch");
        fb->selectPath(root + "/models");
        fb->currentView()->setFocus();
        QTest::keyClick(fb->currentView(), Qt::Key_Down);
        QCOMPARE(fired.count(), 0);
        QCOMPARE(fb->selectedPath(), root + "/scratch");
        QTest::keyClick(fb->currentView(), Qt::Key_Backspace);
        QCOMPARE(fb->location(), top);
    }

    // What changes on the disk is shown as it changes (QFileSystemModel's
    // watcher, FSEvents on macOS): a file made, renamed, removed, a folder
    // made, a file's new size, a change in a folder open in the Tree; after
    // going away and back, under another tab, from the folder above - the
    // folder named as it is or through a link (/var on macOS is
    // /private/var).
    void changesOnTheDiskAreShown_data()
    {
        QTest::addColumn<bool>("linked");
        QTest::newRow("as it is") << false;
        QTest::newRow("through a link") << true;
    }
    void changesOnTheDiskAreShown()
    {
        QFETCH(bool, linked);
        const QString live = linked ? dir.path() + "/linked" : top + "/live";
        QVERIFY(QDir().mkpath(live + "/sub"));
        touch(live + "/a.txt");
        touch(live + "/sub/inner.txt");
        FileBrowser fb;
        fb.resize(400, 600);
        fb.setView(FileBrowser::View::Details);
        fb.setLocation(live);
        settled(fb, "a.txt");
        const auto shown = [&](const QString& name) { return fb.shownNames().contains(name); };
        touch(live + "/new.txt");
        QTRY_VERIFY_WITH_TIMEOUT(shown("new.txt"), 5000);
        QVERIFY(QFile::rename(live + "/new.txt", live + "/renamed.txt"));
        QTRY_VERIFY_WITH_TIMEOUT(shown("renamed.txt") && !shown("new.txt"), 5000);
        QVERIFY(QFile::remove(live + "/renamed.txt"));
        QTRY_VERIFY_WITH_TIMEOUT(!shown("renamed.txt"), 5000);
        QVERIFY(QDir().mkpath(live + "/made"));
        QTRY_VERIFY_WITH_TIMEOUT(shown("made"), 5000);
        {
            QFile f(live + "/a.txt");
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Append));
            f.write(QByteArray(5000, 'y'));
        }
        QTRY_VERIFY_WITH_TIMEOUT(fb.fileModel()->size(fb.fileModel()->index(live + "/a.txt")) == 5001, 5000);
        fb.setView(FileBrowser::View::Tree);
        static_cast<QTreeView*>(fb.currentView())->expand(fb.indexOf(live + "/sub"));
        const auto inSub = [&] { return fb.fileModel()->rowCount(fb.fileModel()->index(live + "/sub")); };
        QTRY_COMPARE_WITH_TIMEOUT(inSub(), 1, 5000);
        touch(live + "/sub/second.txt");
        QTRY_COMPARE_WITH_TIMEOUT(inSub(), 2, 5000);
        QVERIFY(QFile::remove(live + "/sub/second.txt"));
        QTRY_COMPARE_WITH_TIMEOUT(inSub(), 1, 5000);
        // Away and back.
        fb.setView(FileBrowser::View::Details);
        fb.setLocation(root);
        settled(fb, "amp.sch");
        touch(live + "/while_away.txt");
        QTest::qWait(700);
        fb.setLocation(live);
        QTRY_VERIFY_WITH_TIMEOUT(shown("while_away.txt"), 5000);
        touch(live + "/after_back.txt");
        QTRY_VERIFY_WITH_TIMEOUT(shown("after_back.txt"), 5000);
        // Another tab in front, then this one again.
        fb.openInNewTab(root);
        settled(fb, "amp.sch");
        touch(live + "/other_tab.txt");
        QTest::qWait(700);
        fb.setCurrentTab(0);
        QTRY_VERIFY_WITH_TIMEOUT(shown("other_tab.txt"), 5000);
        touch(live + "/after_tab.txt");
        QTRY_VERIFY_WITH_TIMEOUT(shown("after_tab.txt"), 5000);
        // From the folder above.
        fb.setLocation(top);
        settled(fb, linked ? "linked" : "live");
        touch(live + "/sub/from_above.txt");
        QTest::qWait(700);
        fb.setLocation(live + "/sub");
        QTRY_VERIFY_WITH_TIMEOUT(shown("from_above.txt"), 5000);
    }

    // The folder shown taken out of the model while it is there: a listing
    // of the folder above, gathered before it was made, coming after it was
    // entered - or the folder removed and made again (a git checkout). It
    // is shown again, and what changes in it; the views showed the top of
    // the file system ("/") from then on. Gone for good: the nearest folder
    // above it still there.
    void theFolderShownTakenAwayAndBack()
    {
        const QString parent = top + "/away";
        const QString here = parent + "/here";
        QVERIFY(QDir().mkpath(here));
        touch(here + "/a.txt");
        for (FileBrowser::View view : {FileBrowser::View::List, FileBrowser::View::Tree, FileBrowser::View::Details}) {
            FileBrowser fb;
            fb.resize(400, 600);
            fb.setView(view);
            // (The folder above seen first: its listing comes again.)
            fb.setLocation(parent);
            settled(fb, "here");
            fb.setLocation(here);
            settled(fb, "a.txt");
            // Made again the moment the model takes it away.
            QFileSystemModel* model = fb.fileModel();
            bool remade = false;
            const QMetaObject::Connection c = connect(model, &QAbstractItemModel::rowsAboutToBeRemoved, this,
                                                      [&](const QModelIndex& p, int first, int last) {
                for (int r = first; r <= last; ++r)
                    if (model->filePath(model->index(r, 0, p)) == here) {
                        QVERIFY(QDir().mkpath(here));
                        touch(here + "/a.txt");
                        remade = true;
                    }
            });
            QVERIFY(QDir(here).removeRecursively());
            QTRY_VERIFY_WITH_TIMEOUT(remade, 5000);
            disconnect(c);
            QCOMPARE(fb.location(), here);
            QTRY_VERIFY_WITH_TIMEOUT(fb.currentView()->rootIndex().isValid(), 5000);
            QCOMPARE(fb.pathOf(fb.currentView()->rootIndex()), here);
            settled(fb, "a.txt");
            touch(here + "/new.txt");
            QTRY_VERIFY_WITH_TIMEOUT(fb.shownNames().contains("new.txt"), 5000);
            QVERIFY(QFile::remove(here + "/new.txt"));
        }
        // Removed, and made again a moment after the model saw it go (a
        // script, a checkout): kept, not left for the folder above.
        {
            FileBrowser fb;
            fb.resize(400, 600);
            fb.setView(FileBrowser::View::List);
            fb.setLocation(parent);
            settled(fb, "here");
            fb.setLocation(here);
            settled(fb, "a.txt");
            QFileSystemModel* model = fb.fileModel();
            bool seen = false, remade = false;
            const QMetaObject::Connection c = connect(model, &QAbstractItemModel::rowsAboutToBeRemoved, this,
                                                      [&](const QModelIndex& p, int first, int last) {
                bool it = model->filePath(p) == here;
                for (int r = first; r <= last; ++r) it = it || model->filePath(model->index(r, 0, p)) == here;
                if (!it || seen) return;
                seen = true;
                QTimer::singleShot(200, this, [&] {
                    QVERIFY(QDir().mkpath(here));
                    touch(here + "/a.txt");
                    remade = true;
                });
            });
            QVERIFY(QDir(here).removeRecursively());
            QTRY_VERIFY_WITH_TIMEOUT(remade, 5000);
            disconnect(c);
            QTest::qWait(800);
            QCOMPARE(fb.location(), here);
            QTRY_VERIFY_WITH_TIMEOUT(fb.currentView()->rootIndex().isValid(), 5000);
            settled(fb, "a.txt");
        }
        // Gone for good: the folder above, shown - the model watching it,
        // or only the folder gone and what was in it.
        for (bool above : {true, false}) {
            QVERIFY(QDir().mkpath(here));
            touch(here + "/a.txt");
            FileBrowser fb;
            fb.resize(400, 600);
            fb.setView(FileBrowser::View::List);
            if (above) {
                fb.setLocation(parent);
                settled(fb, "here");
            }
            fb.setLocation(here);
            settled(fb, "a.txt");
            QVERIFY(QDir(here).removeRecursively());
            QTRY_COMPARE_WITH_TIMEOUT(fb.location(), parent, 5000);
            QVERIFY(fb.currentView()->rootIndex().isValid());
            QTRY_VERIFY_WITH_TIMEOUT(!fb.shownNames().contains("here"), 5000);
        }
    }

    // In the application: the left dock's File Browser tab, from the
    // workspace; a schematic it opens is opened as from the Content tab,
    // and then among its recent documents.
    void theApplicationHasIt()
    {
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.firstRun = false;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.XyceExecutable = "xyce";
        QucsSettings.SpiceOpusExecutable = "spiceopus";
        QucsSettings.S4Qworkdir = dir.filePath("s4q");
        QucsSettings.tempFilesDir.setPath(dir.filePath("s4q"));
        QDir().mkpath(dir.filePath("s4q"));
        QucsSettings.qucsWorkspaceDir.setPath(top);
        QucsSettings.QucsWorkDir.setPath(top);
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        // (Under its own name: its dataset's and display's are in it.)
        const QString file = root + "/RCL_resonance.sch";
        QVERIFY(QFile::copy(QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/RF/Miscellaneous/RCL_resonance.sch"), file));
        QFile::setPermissions(file, QFile::ReadOwner | QFile::WriteOwner);

        QucsApp app(false);
        MainGuard guard(&app);
        FileBrowser* fb = app.fileBrowserPanel();
        QVERIFY(fb != nullptr);
        QCOMPARE(fb->homePath(), top);
        bool tabbed = false;
        for (QTabWidget* tabs : app.findChildren<QTabWidget*>())
            for (int i = 0; i < tabs->count(); ++i)
                if (tabs->widget(i) == fb && tabs->tabText(i) == QStringLiteral("File Browser")) tabbed = true;
        QVERIFY(tabbed);

        emit fb->openRequested(file);
        auto* doc = dynamic_cast<Schematic*>(app.getDoc());
        QVERIFY(doc != nullptr);
        QCOMPARE(QFileInfo(doc->getDocName()).canonicalFilePath(), file);
        fb->setView(FileBrowser::View::Recent);
        QVERIFY(fb->shownNames().contains("RCL_resonance.sch"));
        fb->setView(FileBrowser::View::List);
        doc->setChanged(false);
        app.closeAllFiles();
        QFile::remove(file);

        // A folder made and entered at once: shown, and what changes in it
        // (a listing of the workspace gathered before it was made took it
        // out of the model, and the views showed "/" from then on).
        const QString live = top + "/inapp";
        QVERIFY(QDir().mkpath(live));
        touch(live + "/a.txt");
        app.show();
        fb->setLocation(live);
        QCOMPARE(settled(*fb, "a.txt"), QStringList({"a.txt"}));
        touch(live + "/new.txt");
        QTRY_VERIFY_WITH_TIMEOUT(fb->shownNames().contains("new.txt"), 5000);
        QVERIFY(QFile::remove(live + "/new.txt"));
        QTRY_VERIFY_WITH_TIMEOUT(!fb->shownNames().contains("new.txt"), 5000);

        // Edit > Copy and Paste while it has the focus: its files.
        QApplication::clipboard()->clear();
        fb->selectPath(live + "/a.txt");
        fb->currentView()->setFocus();
        QVERIFY(fb->hasEditFocus());
        app.editCopy->trigger();
        QCOMPARE(FileBrowser::clipboardFiles(), QStringList({live + "/a.txt"}));
        app.editPaste->trigger();
        QTRY_VERIFY(QFile::exists(live + "/a copy.txt"));
        QVERIFY(!app.editPaste->isChecked());
    }
};

QTEST_MAIN(TestFileBrowser)
#include "test_file_browser.moc"
