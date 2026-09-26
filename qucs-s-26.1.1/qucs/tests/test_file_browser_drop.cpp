/*
 * The File Browser's drag and drop: entries dropped on a folder (a row,
 * the folder shown, a button of the path) move there - or are copied, with
 * the copy key, from another program, to another disk; a copy into its
 * own folder is "name copy"; a name the folder has already is asked about;
 * a folder cannot go into itself, the workspace and the open project stay
 * put; a folder held under a drag opens; documents open from what moved
 * follow it.
 */
#include <QtTest>
#include <QCheckBox>
#include <QListView>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeView>

#include "config.h"
#include "filebrowser.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "qucs.h"
#include "qucsdoc.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

bool write(const QString& path, const QByteArray& bytes = "x")
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

QByteArray read(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// Runs fn and answers the dialog it brings up with answer(dialog).
void answering(const std::function<void()>& fn, const std::function<bool(QWidget*)>& answer)
{
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&] {
        if (QWidget* modal = QApplication::activeModalWidget())
            if (answer(modal)) timer.stop();
    });
    timer.start(20);
    fn();
}

// Answers the question about a name with the button \a name (and the box
// for the rest, when \a rest).
std::function<bool(QWidget*)> clash(const char* name, bool rest = false, QString* asked = nullptr)
{
    return [name, rest, asked](QWidget* w) {
        auto* box = qobject_cast<QMessageBox*>(w);
        if (box == nullptr || box->objectName() != "fbClash") return false;
        if (asked != nullptr) *asked = box->text();
        if (rest) box->findChild<QCheckBox*>("fbClashRest")->setChecked(true);
        box->findChild<QPushButton*>(name)->click();
        return true;
    };
}

#ifdef Q_OS_MACOS
constexpr Qt::KeyboardModifier kCopyKey = Qt::AltModifier;
constexpr Qt::KeyboardModifier kMoveKey = Qt::ControlModifier;
#else
constexpr Qt::KeyboardModifier kCopyKey = Qt::ControlModifier;
constexpr Qt::KeyboardModifier kMoveKey = Qt::ShiftModifier;
#endif

// A drag entering \a on at \a pos and dropped there (Qt hands the drop to
// the widget that took the drag's entry): whether it was taken, and as what.
std::pair<bool, Qt::DropAction> dropOn(QWidget* on, const QPoint& pos, const QMimeData* mime, Qt::KeyboardModifiers keys)
{
    const Qt::DropActions actions = Qt::CopyAction | Qt::MoveAction;
    QDragEnterEvent enter(pos, actions, mime, Qt::LeftButton, keys);
    QCoreApplication::sendEvent(on, &enter);
    QDropEvent drop(pos, actions, mime, Qt::LeftButton, keys);
    QCoreApplication::sendEvent(on, &drop);
    return {drop.isAccepted(), drop.dropAction()};
}

QMimeData* urls(const QStringList& paths)
{
    auto* mime = new QMimeData;
    QList<QUrl> list;
    for (const QString& p : paths) list << QUrl::fromLocalFile(p);
    mime->setUrls(list);
    return mime;
}

} // namespace

class TestFileBrowserDrop : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString top;   // canonical

    // A fresh folder \a tag: a/ (with inner.txt), b/ (with deep/x.txt),
    // one.sch, two.txt.
    QString fresh(const QString& tag)
    {
        const QString here = top + "/" + tag;
        write(here + "/a/inner.txt", "inner");
        write(here + "/b/deep/x.txt", "x");
        write(here + "/one.sch", "one");
        write(here + "/two.txt", "two");
        return here;
    }

    static bool shows(FileBrowser& fb, const QString& name)
    {
        return QTest::qWaitFor([&] { return fb.shownNames().contains(name); }, 10000);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        top = QFileInfo(dir.path()).canonicalFilePath();
#ifdef Q_OS_LINUX
        qputenv("XDG_DATA_HOME", QFile::encodeName(top + "/xdg"));   // the trash in here, not the user's
#endif
    }

    void whatADropDoes()
    {
        const QString here = fresh("rules");
        const QStringList one{here + "/one.sch"};
        QCOMPARE(FileBrowser::dropAction(true, Qt::NoModifier, one, here + "/a"), Qt::MoveAction);
        QCOMPARE(FileBrowser::dropAction(false, Qt::NoModifier, one, here + "/a"), Qt::CopyAction);   // from the Finder
        QCOMPARE(FileBrowser::dropAction(true, kCopyKey, one, here + "/a"), Qt::CopyAction);
        QCOMPARE(FileBrowser::dropAction(false, kMoveKey, one, here + "/a"), Qt::MoveAction);

        FileBrowser fb;
        fb.setHomePath(here + "/b");   // the workspace
        fb.setProjectPath(here + "/b/deep");
        QVERIFY(fb.refusal(one, here + "/a", Qt::MoveAction).isEmpty());
        QVERIFY(!fb.refusal({here + "/a"}, here + "/a", Qt::MoveAction).isEmpty());             // into itself
        QVERIFY(!fb.refusal({here + "/a"}, here + "/a/sub", Qt::CopyAction).isEmpty());         // into what is in it
        QVERIFY(!fb.refusal({here + "/b"}, here + "/a", Qt::MoveAction).isEmpty());             // the workspace
        QVERIFY(!fb.refusal({here + "/b/deep"}, here + "/a", Qt::MoveAction).isEmpty());        // the open project
        QVERIFY(fb.refusal({here + "/b/deep"}, here + "/a", Qt::CopyAction).isEmpty());         // (a copy of it may go)
        QVERIFY(fb.refusal({here + "/b/deep/x.txt"}, here + "/a", Qt::MoveAction).isEmpty());  // what is in it may move
        QVERIFY(!fb.refusal(one, here, Qt::MoveAction).isEmpty());                              // there already
        QVERIFY(fb.refusal(one, here, Qt::CopyAction).isEmpty());                               // (a copy beside it)
        QVERIFY(!fb.refusal(one, here + "/two.txt", Qt::MoveAction).isEmpty());                 // no folder
    }

    void movedAndCopied()
    {
        const QString here = fresh("transfer");
        FileBrowser fb;
        QSignalSpy moved(&fb, &FileBrowser::moved);

        QCOMPARE(fb.transfer({here + "/one.sch", here + "/b"}, here + "/a", Qt::MoveAction),
                 QStringList({here + "/a/one.sch", here + "/a/b"}));
        QCOMPARE(read(here + "/a/one.sch"), QByteArray("one"));
        QCOMPARE(read(here + "/a/b/deep/x.txt"), QByteArray("x"));
        QVERIFY(!QFileInfo::exists(here + "/one.sch") && !QFileInfo::exists(here + "/b"));
        QCOMPARE(moved.size(), 1);
        QCOMPARE(moved.at(0).at(0).toStringList(), QStringList({here + "/one.sch", here + "/b"}));
        QCOMPARE(moved.at(0).at(1).toStringList(), QStringList({here + "/a/one.sch", here + "/a/b"}));

        // Copied: the original stays; beside itself, "name copy".
        QCOMPARE(fb.transfer({here + "/two.txt"}, here + "/a", Qt::CopyAction), QStringList({here + "/a/two.txt"}));
        QCOMPARE(read(here + "/two.txt"), QByteArray("two"));
        QCOMPARE(fb.transfer({here + "/two.txt"}, here, Qt::CopyAction), QStringList({here + "/two copy.txt"}));
        QCOMPARE(fb.transfer({here + "/two.txt"}, here, Qt::CopyAction), QStringList({here + "/two copy 2.txt"}));
        QCOMPARE(fb.transfer({here + "/a/b"}, here + "/a", Qt::CopyAction), QStringList({here + "/a/b copy"}));
        QCOMPARE(read(here + "/a/b copy/deep/x.txt"), QByteArray("x"));
        QCOMPARE(moved.size(), 1);   // (copies move nothing)

        // Refused: nothing happens (said in a box).
        answering([&] { QVERIFY(fb.transfer({here + "/a"}, here + "/a/b", Qt::MoveAction).isEmpty()); },
                  [](QWidget* w) {
                      auto* box = qobject_cast<QMessageBox*>(w);
                      if (box != nullptr) box->accept();
                      return box != nullptr;
                  });
        QVERIFY(QFileInfo(here + "/a/inner.txt").exists());
    }

    // A name the folder has already: keep both, skip, stop - for the rest too.
    void aNameTakenAlready()
    {
        const QString here = fresh("clash");
        write(here + "/c/two.txt", "old");
        write(here + "/c/one.sch", "old");
        FileBrowser fb;

        QString asked;
        QStringList done;
        answering([&] { done = fb.transfer({here + "/two.txt"}, here + "/c", Qt::MoveAction); },
                  clash("fbClashKeepBoth", false, &asked));
        QVERIFY2(asked.contains("two.txt") && asked.contains("“c”"), qPrintable(asked));
        QCOMPARE(done, QStringList({here + "/c/two 2.txt"}));
        QCOMPARE(read(here + "/c/two.txt"), QByteArray("old"));
        QCOMPARE(read(here + "/c/two 2.txt"), QByteArray("two"));

        write(here + "/two.txt", "two again");
        answering([&] { done = fb.transfer({here + "/two.txt"}, here + "/c", Qt::CopyAction); }, clash("fbClashSkip"));
        QVERIFY(done.isEmpty());
        QCOMPARE(read(here + "/c/two.txt"), QByteArray("old"));

        // Stop: the rest are left too.
        answering([&] { done = fb.transfer({here + "/one.sch", here + "/two.txt"}, here + "/c", Qt::MoveAction); },
                  clash("fbClashStop"));
        QVERIFY(done.isEmpty());
        QVERIFY(QFileInfo::exists(here + "/one.sch") && QFileInfo::exists(here + "/two.txt"));

        // Keep both, for the rest too: asked once.
        int questions = 0;
        answering([&] { done = fb.transfer({here + "/one.sch", here + "/two.txt"}, here + "/c", Qt::CopyAction); },
                  [&](QWidget* w) {
                      if (!clash("fbClashKeepBoth", true)(w)) return false;
                      ++questions;
                      return true;
                  });
        QCOMPARE(questions, 1);
        QCOMPARE(done, QStringList({here + "/c/one 2.sch", here + "/c/two 3.txt"}));

#ifdef Q_OS_LINUX
        // Replace: the one there goes to the trash (here, not the user's).
        answering([&] { done = fb.transfer({here + "/two.txt"}, here + "/c", Qt::MoveAction); }, clash("fbClashReplace"));
        QCOMPARE(done, QStringList({here + "/c/two.txt"}));
        QCOMPARE(read(here + "/c/two.txt"), QByteArray("two again"));
#endif
    }

    // Dropped on the views: on a folder's row, into it; beside the rows,
    // into the folder shown; lit up where it would go.
    void droppedOnTheView()
    {
        const QString here = fresh("view");
        const QString elsewhere = top + "/elsewhere";
        write(elsewhere + "/from finder.txt", "finder");
        FileBrowser fb;
        fb.resize(420, 520);
        fb.show();
        fb.setView(FileBrowser::View::List);
        fb.setLocation(here);
        QVERIFY(shows(fb, "one.sch"));
        auto* view = fb.currentView();
        QWidget* viewport = view->viewport();
        QVERIFY(viewport->acceptDrops());

        const QModelIndex a = fb.indexOf(here + "/a");
        QVERIFY(a.isValid());
        const QPoint onA = view->visualRect(a).center();
        const QPoint below(viewport->width() / 2, viewport->height() - 10);
        QRect area;
        QCOMPARE(fb.dropTarget(view, onA, &area), here + "/a");
        QVERIFY(area.contains(onA) && area.height() < viewport->height() / 2);
        QCOMPARE(fb.dropTarget(view, below, &area), here);
        QCOMPARE(area, viewport->rect());
        const QModelIndex file = fb.indexOf(here + "/one.sch");
        QCOMPARE(fb.dropTarget(view, view->visualRect(file).center()), here);   // on a file: its folder

        // From another program (no source here): copied, lit up.
        std::unique_ptr<QMimeData> mime(urls({elsewhere + "/from finder.txt"}));
        QDragEnterEvent enter(onA, Qt::CopyAction | Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(viewport, &enter);
        QVERIFY(enter.isAccepted());
        QDragMoveEvent move(onA, Qt::CopyAction | Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(viewport, &move);
        QVERIFY(move.isAccepted());
        QCOMPARE(move.dropAction(), Qt::CopyAction);
        QVERIFY(fb.dropHighlight() != nullptr && fb.dropHighlight()->isVisible());
        QCOMPARE(fb.dropHighlight()->parentWidget(), viewport);
        QVERIFY(fb.dropHighlight()->geometry().contains(onA));
        QDropEvent drop(onA, Qt::CopyAction | Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(viewport, &drop);
        QVERIFY(drop.isAccepted());
        QVERIFY(!fb.dropHighlight()->isVisible());
        QTRY_COMPARE(read(here + "/a/from finder.txt"), QByteArray("finder"));
        QVERIFY(QFileInfo::exists(elsewhere + "/from finder.txt"));

        // With the move key: moved.
        std::unique_ptr<QMimeData> moving(urls({elsewhere + "/from finder.txt"}));
        const auto moveDrop = dropOn(viewport, below, moving.get(), kMoveKey);
        QVERIFY(moveDrop.first);
        QCOMPARE(moveDrop.second, Qt::MoveAction);
        QTRY_VERIFY(!QFileInfo::exists(elsewhere + "/from finder.txt"));
        QCOMPARE(read(here + "/from finder.txt"), QByteArray("finder"));

        // Moved where it is: not allowed.
        std::unique_ptr<QMimeData> same(urls({here + "/two.txt"}));
        QDragEnterEvent sameEnter(below, Qt::CopyAction | Qt::MoveAction, same.get(), Qt::LeftButton, kMoveKey);
        QCoreApplication::sendEvent(viewport, &sameEnter);
        QDragMoveEvent nowhere(below, Qt::CopyAction | Qt::MoveAction, same.get(), Qt::LeftButton, kMoveKey);
        QCoreApplication::sendEvent(viewport, &nowhere);
        QVERIFY(!nowhere.isAccepted());
        QVERIFY(!fb.dropHighlight()->isVisible());
        QVERIFY(!dropOn(viewport, below, same.get(), kMoveKey).first);
        QVERIFY(QFileInfo::exists(here + "/two.txt"));

        // Not files: not taken.
        QMimeData text;
        text.setText("words");
        QDragEnterEvent words(onA, Qt::CopyAction, &text, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(viewport, &words);
        QVERIFY(!words.isAccepted());

        // On a button of the path: into that folder.
        QToolButton* parentCrumb = nullptr;
        for (QToolButton* b : fb.crumbs())
            if (b->property("path").toString() == top) parentCrumb = b;
        QVERIFY(parentCrumb != nullptr && parentCrumb->acceptDrops());
        std::unique_ptr<QMimeData> up(urls({here + "/two.txt"}));
        QVERIFY(dropOn(parentCrumb, parentCrumb->rect().center(), up.get(), kMoveKey).first);
        QTRY_COMPARE(read(top + "/two.txt"), QByteArray("two"));
        QVERIFY(!QFileInfo::exists(here + "/two.txt"));

        // The Details and Icons views take drops as the list does; the
        // tree too, a file in an opened folder going into that folder.
        for (FileBrowser::View v : {FileBrowser::View::Details, FileBrowser::View::Icons, FileBrowser::View::Tree}) {
            fb.setView(v);
            QVERIFY(fb.currentView()->viewport()->acceptDrops());
            QTRY_VERIFY(fb.indexOf(here + "/a").isValid());
            QCOMPARE(fb.dropTarget(fb.currentView(), fb.currentView()->visualRect(fb.indexOf(here + "/a")).center()),
                     here + "/a");
        }
        auto* tree = static_cast<QTreeView*>(fb.currentView());
        tree->expand(fb.indexOf(here + "/a"));
        QTRY_VERIFY(fb.indexOf(here + "/a/inner.txt").isValid() && tree->visualRect(fb.indexOf(here + "/a/inner.txt")).isValid());
        QCOMPARE(fb.dropTarget(tree, tree->visualRect(fb.indexOf(here + "/a/inner.txt")).center()), here + "/a");

        // The Columns view: each column takes drops, into its folder or the
        // folder under the drop.
        fb.setView(FileBrowser::View::Columns);
        QListView* column = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
                                     for (QListView* c : fb.currentView()->findChildren<QListView*>())
                                         if (c->isVisible() && fb.pathOf(c->rootIndex()) == here) column = c;
                                     return column != nullptr && column->model()->rowCount(column->rootIndex()) > 0;
                                 }()),
                                 10000);
        QVERIFY(column->viewport()->acceptDrops());
        QModelIndex aRow;
        for (int r = 0; r < column->model()->rowCount(column->rootIndex()); ++r) {
            const QModelIndex i = column->model()->index(r, 0, column->rootIndex());
            if (fb.pathOf(i) == here + "/a") aRow = i;
        }
        QVERIFY(aRow.isValid());
        QCOMPARE(fb.dropTarget(column, column->visualRect(aRow).center()), here + "/a");
        QCOMPARE(fb.dropTarget(column, QPoint(5, column->viewport()->height() - 5)), here);
    }

    // A folder held under a drag opens.
    void aFolderHeldOpens()
    {
        const QString here = fresh("spring");
        write(top + "/spring-source.txt", "s");
        FileBrowser fb;
        fb.resize(420, 520);
        fb.show();
        fb.setView(FileBrowser::View::List);
        fb.setLocation(here);
        QVERIFY(shows(fb, "a"));
        auto* view = fb.currentView();
        const QPoint onA = view->visualRect(fb.indexOf(here + "/a")).center();
        std::unique_ptr<QMimeData> mime(urls({top + "/spring-source.txt"}));
        QDragEnterEvent enter(onA, Qt::CopyAction | Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(view->viewport(), &enter);
        QDragMoveEvent move(onA, Qt::CopyAction | Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(view->viewport(), &move);
        QTRY_COMPARE_WITH_TIMEOUT(fb.location(), here + "/a", 3000);
        QDragLeaveEvent leave;
        QCoreApplication::sendEvent(fb.currentView()->viewport(), &leave);
    }

    // Documents open from what moved follow it: their names and tabs.
    void openDocumentsFollow()
    {
        const QString here = fresh("docs");
        write(here + "/a/notes.txt", "notes");
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(here + "/two.txt"));
        QVERIFY(app.gotoPage(here + "/a/notes.txt"));
        const auto named = [&](const QString& path) {
            for (QucsDoc* d : app.allDocuments())
                if (d->getDocName() == path) return d;
            return static_cast<QucsDoc*>(nullptr);
        };
        QucsDoc* two = named(here + "/two.txt");
        QucsDoc* notes = named(here + "/a/notes.txt");
        QVERIFY(two != nullptr && notes != nullptr);

        FileBrowser* fb = app.fileBrowserPanel();
        QVERIFY(!fb->transfer({here + "/two.txt"}, here + "/b", Qt::MoveAction).isEmpty());
        QCOMPARE(two->getDocName(), here + "/b/two.txt");
        QVERIFY(!fb->transfer({here + "/a"}, here + "/b", Qt::MoveAction).isEmpty());   // a folder, with what is open in it
        QCOMPARE(notes->getDocName(), here + "/b/a/notes.txt");
        QWidget* w = QucsApp::documentWidget(notes);
        QCOMPARE(app.paneOf(w)->tabText(app.paneOf(w)->indexOf(w)), QStringLiteral("notes.txt"));
        app.closeAllFiles();
    }
};

QTEST_MAIN(TestFileBrowserDrop)
#include "test_file_browser_drop.moc"
