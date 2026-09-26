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
    // too, in the Tree only the files; "only Qucs-S files" leaves the
    // schematics, datasets, netlists, sources and S-parameters, and the
    // folders.
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
    }
};

QTEST_MAIN(TestFileBrowser)
#include "test_file_browser.moc"
