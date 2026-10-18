/*
 * The File Browser's drag and drop: entries dropped on a folder (a row,
 * the folder shown, a button of the path) move there - or are copied, with
 * the copy key, from another program, to another disk; a copy into its
 * own folder is "name copy"; a name the folder has already is asked about;
 * a folder cannot go into itself, the workspace and the open project stay
 * put; a folder held under a drag opens; documents open from what moved
 * follow it.
 */
#include <QContextMenuEvent>
#include <QTimer>
#include <QtTest>
#include <QAction>
#include <QClipboard>
#include <QFileSystemModel>
#include <QMenu>
#include <QSignalSpy>
#include <QCheckBox>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

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

// Runs fn and answers the dialog it brings up with answer(dialog). Any
// other box (one more after it: a transfer reporting a failure, say) is
// closed and fails the test - rather than waiting for an answer until the
// test is killed.
void answering(const std::function<void()>& fn, const std::function<bool(QWidget*)>& answer)
{
    bool answered = false;
    QStringList unexpected;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&] {
        QWidget* modal = QApplication::activeModalWidget();
        if (modal == nullptr) return;
        if (!answered && answer(modal)) {
            answered = true;
            return;
        }
        if (auto* box = qobject_cast<QMessageBox*>(modal)) {
            unexpected << (box->text() + QLatin1Char(' ') + box->informativeText()).trimmed();
            box->done(QDialog::Rejected);
        }
    });
    timer.start(20);
    fn();
    if (!unexpected.isEmpty())
        QTest::qFail(qPrintable(QStringLiteral("unexpected box: ") + unexpected.join(QStringLiteral(" | "))), __FILE__, __LINE__);
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
        // (What goes to the trash goes into the settings' folder here, not
        // into the user's trash: QUCS_TRASH_DIR, useIsolatedSettings.)
    }

    // Files compared as files, not names: another case where the file
    // system has none, a path through a link. A copy over a file writes
    // beside it first: one that fails leaves the file there (the helpers
    // under PDF Save As, Claude's simulate keep_as and the library
    // dialog; bug hunt 2026-09-26, A1).
    void filesComparedAndCopiedSafely()
    {
        const QString here = top + "/same";
        write(here + "/report.pdf", "the report");
        write(here + "/other.pdf", "another");
        QVERIFY(misc::isSameFile(here + "/report.pdf", here + "/./report.pdf"));
        QVERIFY(!misc::isSameFile(here + "/report.pdf", here + "/other.pdf"));
        QVERIFY(!misc::isSameFile(here + "/report.pdf", here + "/missing.pdf"));
        QVERIFY(!misc::isSameFile(QString(), QString()));
        if (QFileInfo::exists(here + "/Report.pdf"))   // (macOS, Windows)
            QVERIFY(misc::isSameFile(here + "/report.pdf", here + "/Report.pdf"));
#ifndef Q_OS_WIN
        QVERIFY(QFile::link(here, top + "/samelink"));
        QVERIFY(misc::isSameFile(here + "/report.pdf", top + "/samelink/report.pdf"));
        QVERIFY(misc::copyFileOver(here + "/report.pdf", top + "/samelink/report.pdf"));   // itself: nothing done
        QCOMPARE(read(here + "/report.pdf"), QByteArray("the report"));
#endif
        QString why;
        QVERIFY(misc::copyFileOver(here + "/report.pdf", here + "/other.pdf", &why));
        QCOMPARE(read(here + "/other.pdf"), QByteArray("the report"));
        QVERIFY(misc::copyFileOver(here + "/report.pdf", here + "/new.pdf"));
        QCOMPARE(read(here + "/new.pdf"), QByteArray("the report"));
        write(here + "/other.pdf", "another");
        QVERIFY(!misc::copyFileOver(here + "/missing.pdf", here + "/other.pdf", &why));
        QVERIFY(why.contains("missing.pdf"));
        QCOMPARE(read(here + "/other.pdf"), QByteArray("another"));   // left as it was
        QCOMPARE(QDir(here).entryList(QDir::Files).size(), 3);         // nothing left beside it
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

        // Replace: the one there goes to the trash (here, not the user's).
        answering([&] { done = fb.transfer({here + "/two.txt"}, here + "/c", Qt::MoveAction); }, clash("fbClashReplace"));
        QCOMPARE(done, QStringList({here + "/c/two.txt"}));
        QCOMPARE(read(here + "/c/two.txt"), QByteArray("two again"));
        QVERIFY(QFileInfo::exists(qEnvironmentVariable("QUCS_TRASH_DIR") + "/two.txt"));
    }

    // Replace is not offered for the one there when it holds the one moved
    // or copied: W/x/x onto W, where W/x is, sent W/x - the one moved and
    // all beside it - to the trash, and the move then failed (bug hunt
    // 2026-09-26, A2). Kept both, or skipped.
    void aFolderHoldingTheOneMovedIsNotReplaced()
    {
        const QString here = top + "/holds";
        write(here + "/x/other.txt", "also precious");
        write(here + "/x/x/data.txt", "precious");
        FileBrowser fb;
        bool offered = true;
        QString informative;
        const auto skipIt = [&](QWidget* w) {
            auto* box = qobject_cast<QMessageBox*>(w);
            if (box == nullptr || box->objectName() != "fbClash") return false;
            offered = box->findChild<QPushButton*>("fbClashReplace") != nullptr;
            informative = box->informativeText();
            box->findChild<QPushButton*>("fbClashSkip")->click();
            return true;
        };
        QStringList done;
        for (const Qt::DropAction action : {Qt::MoveAction, Qt::CopyAction}) {
            offered = true;
            answering([&] { done = fb.transfer({here + "/x/x"}, here, action); }, skipIt);
            QVERIFY(!offered);
            QVERIFY2(informative.contains("holds"), qPrintable(informative));
            QVERIFY(done.isEmpty());
            QCOMPARE(read(here + "/x/x/data.txt"), QByteArray("precious"));
            QCOMPARE(read(here + "/x/other.txt"), QByteArray("also precious"));
        }
        // Replace chosen for the rest (the trash here, not the user's):
        // asked again for the one that holds the source, without Replace.
        write(here + "/z/z/deep.txt", "deep");
        write(here + "/one.txt", "one there");
        write(here + "/z/one.txt", "one moved");
        int boxes = 0;
        offered = true;
        QTimer timer;
        connect(&timer, &QTimer::timeout, this, [&] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (box == nullptr || box->objectName() != "fbClash" || !box->isVisible()) return;
            if (++boxes == 1) {
                box->findChild<QCheckBox*>("fbClashRest")->setChecked(true);
                box->findChild<QPushButton*>("fbClashReplace")->click();
            } else {
                offered = box->findChild<QPushButton*>("fbClashReplace") != nullptr;
                box->findChild<QPushButton*>("fbClashSkip")->click();
            }
        });
        timer.start(20);
        done = fb.transfer({here + "/z/one.txt", here + "/z/z"}, here, Qt::MoveAction);
        timer.stop();
        QCOMPARE(boxes, 2);
        QVERIFY(!offered);
        QCOMPARE(done, QStringList({here + "/one.txt"}));
        QCOMPARE(read(here + "/one.txt"), QByteArray("one moved"));
        QCOMPARE(read(here + "/z/z/deep.txt"), QByteArray("deep"));
        // Keep Both: beside it, all kept.
        answering([&] { done = fb.transfer({here + "/x/x"}, here, Qt::MoveAction); }, clash("fbClashKeepBoth"));
        QCOMPARE(done, QStringList({here + "/x 2"}));
        QCOMPARE(read(here + "/x 2/data.txt"), QByteArray("precious"));
        QCOMPARE(read(here + "/x/other.txt"), QByteArray("also precious"));
#ifndef Q_OS_WIN
        // Through a link: the same folder, found so.
        write(here + "/y/y/deep.txt", "deep");
        QVERIFY(QFile::link(here + "/y", top + "/ylink"));
        offered = true;
        answering([&] { done = fb.transfer({top + "/ylink/y"}, here, Qt::MoveAction); }, skipIt);
        QVERIFY(!offered);
        QCOMPARE(read(here + "/y/y/deep.txt"), QByteArray("deep"));
#endif
    }

    // Renamed as a name: another case of its own is a rename (it was
    // refused on macOS and Windows), a path ("../x", "sub/x") is refused
    // (it moved the file); renamed by name or in place, moved() says so,
    // for open documents to follow (bug hunt 2026-09-26, A9, A10).
    void renamedAsAName()
    {
        const QString here = top + "/renames";
        write(here + "/proj/amp.sch", "amp");
        write(here + "/proj/filter.sch", "filter");
        FileBrowser fb;
        QSignalSpy moved(&fb, &FileBrowser::moved);
        QVERIFY(fb.renameEntry(here + "/proj/amp.sch", "amp2.sch").isEmpty());
        QCOMPARE(moved.count(), 1);
        QCOMPARE(moved.last().at(0).toStringList(), QStringList{here + "/proj/amp.sch"});
        QCOMPARE(moved.last().at(1).toStringList(), QStringList{here + "/proj/amp2.sch"});
        QCOMPARE(read(here + "/proj/amp2.sch"), QByteArray("amp"));

        QVERIFY(fb.renameEntry(here + "/proj/amp2.sch", "Amp2.sch").isEmpty());
        QVERIFY(QDir(here + "/proj").entryList(QDir::Files).contains("Amp2.sch"));
        QVERIFY(!QDir(here + "/proj").entryList(QDir::Files).contains("amp2.sch"));
        QCOMPARE(moved.count(), 2);

        for (const QString bad : {"../escaped.sch", "sub/x.sch", "..", ".", "/abs.sch"})
            QVERIFY2(!fb.renameEntry(here + "/proj/filter.sch", bad).isEmpty(), qPrintable(bad));
        QVERIFY(QFileInfo::exists(here + "/proj/filter.sch"));
        QVERIFY(!QFileInfo::exists(here + "/escaped.sch"));
        QVERIFY(!fb.renameEntry(here + "/proj/filter.sch", "Amp2.sch").isEmpty());   // taken
        QCOMPARE(read(here + "/proj/Amp2.sch"), QByteArray("amp"));
        QCOMPARE(moved.count(), 2);

        QVERIFY(fb.renameEntry(here + "/proj", "proj2").isEmpty());   // a folder
        QCOMPARE(moved.last().at(1).toStringList(), QStringList{here + "/proj2"});

        // In place, as the view's editor does it.
        fb.setView(FileBrowser::View::List);
        fb.setLocation(here + "/proj2");
        QVERIFY(shows(fb, "filter.sch"));
        QFileSystemModel* model = fb.fileModel();
        QVERIFY(model->setData(model->index(here + "/proj2/filter.sch"), QStringLiteral("filter2.sch")));
        QCOMPARE(moved.count(), 4);
        QCOMPARE(moved.last().at(0).toStringList(), QStringList{here + "/proj2/filter.sch"});
        QCOMPARE(moved.last().at(1).toStringList(), QStringList{here + "/proj2/filter2.sch"});
    }

    // Copy and Paste: the entries chosen on the clipboard as files (their
    // paths as text, for what takes text), pasted into a folder as a
    // copying drop puts them there - the originals stay, a name taken is
    // asked about, into their own folder "name copy". From an entry's menu
    // (into a folder, beside a file) and the folder's; Paste only when the
    // clipboard has files.
    void copiedAndPasted()
    {
        const QString here = fresh("clip");
        FileBrowser fb;
        fb.resize(400, 600);
        fb.setView(FileBrowser::View::List);
        fb.setLocation(here);
        QVERIFY(shows(fb, "two.txt"));

        fb.copyEntries({here + "/one.sch", here + "/a"});
        const QMimeData* mime = QApplication::clipboard()->mimeData();
        QCOMPARE(mime->urls(), QList<QUrl>({QUrl::fromLocalFile(here + "/one.sch"), QUrl::fromLocalFile(here + "/a")}));
        QCOMPARE(mime->text(), QDir::toNativeSeparators(here + "/one.sch") + "\n" + QDir::toNativeSeparators(here + "/a"));
        QCOMPARE(FileBrowser::clipboardFiles(), QStringList({here + "/one.sch", here + "/a"}));

        // Into another folder: copied, a folder with what is in it.
        QCOMPARE(fb.pasteInto(here + "/b"), QStringList({here + "/b/one.sch", here + "/b/a"}));
        QCOMPARE(read(here + "/b/one.sch"), QByteArray("one"));
        QCOMPARE(read(here + "/b/a/inner.txt"), QByteArray("inner"));
        QCOMPARE(read(here + "/one.sch"), QByteArray("one"));
        QCOMPARE(read(here + "/a/inner.txt"), QByteArray("inner"));
        // There again: the names taken, asked about.
        QString asked;
        QStringList went;
        answering([&] { went = fb.pasteInto(here + "/b"); }, clash("fbClashKeepBoth", true, &asked));
        QVERIFY2(asked.contains("one.sch") && asked.contains("“b”"), qPrintable(asked));
        QCOMPARE(went, QStringList({here + "/b/one 2.sch", here + "/b/a 2"}));
        // Into their own folder: beside themselves.
        QCOMPARE(fb.pasteInto(here), QStringList({here + "/one copy.sch", here + "/a copy"}));
        QCOMPARE(read(here + "/a copy/inner.txt"), QByteArray("inner"));

        // The menus. A file: Copy, and Paste beside it.
        fb.copyEntries({here + "/two.txt"});
        QMenu* menu = fb.contextMenuFor(here + "/one.sch");
        QAction* copy = menu->findChild<QAction*>("fbCopy");
        QAction* paste = menu->findChild<QAction*>("fbPaste");
        QVERIFY(copy != nullptr && paste != nullptr);
        QCOMPARE(copy->text(), QStringLiteral("Copy"));
        QCOMPARE(paste->text(), QStringLiteral("Paste"));
        QVERIFY(paste->isEnabled());
        copy->trigger();
        QCOMPARE(FileBrowser::clipboardFiles(), QStringList({here + "/one.sch"}));
        delete menu;
        // A folder: Paste into it.
        menu = fb.contextMenuFor(here + "/b/deep");
        paste = menu->findChild<QAction*>("fbPaste");
        QCOMPARE(paste->text(), QStringLiteral("Paste into “deep”"));
        paste->trigger();
        QCOMPARE(read(here + "/b/deep/one.sch"), QByteArray("one"));
        delete menu;
        // The folder shown: Paste into it.
        menu = fb.contextMenuFor(QString());
        paste = menu->findChild<QAction*>("fbPaste");
        QVERIFY(paste != nullptr && menu->findChild<QAction*>("fbCopy") == nullptr);
        delete menu;
        // Nothing to paste: text on the clipboard, or a file gone.
        QApplication::clipboard()->setText(here + "/two.txt");
        QVERIFY(FileBrowser::clipboardFiles().isEmpty());
        menu = fb.contextMenuFor(here + "/one.sch");
        QVERIFY(!menu->findChild<QAction*>("fbPaste")->isEnabled());
        delete menu;
        QVERIFY(fb.pasteInto(here + "/b").isEmpty());
        write(here + "/gone.txt");
        fb.copyEntries({here + "/gone.txt"});
        QVERIFY(QFile::remove(here + "/gone.txt"));
        QVERIFY(FileBrowser::clipboardFiles().isEmpty());
        // Several chosen: Copy takes them all.
        fb.selectPath(here + "/one.sch");
        fb.currentView()->selectionModel()->select(fb.indexOf(here + "/two.txt"), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        menu = fb.contextMenuFor(here + "/two.txt");
        copy = menu->findChild<QAction*>("fbCopy");
        QCOMPARE(copy->text(), QStringLiteral("Copy 2 Items"));
        copy->trigger();
        QCOMPARE(FileBrowser::clipboardFiles().size(), 2);
        delete menu;
    }

    // Their keys, in a view of it, before the window's Edit > Copy and
    // Paste: the entries chosen copied; pasted into the folder shown, or
    // in the Tree the folder chosen (the chosen file's).
    void copiedAndPastedByKeys()
    {
        const QString here = fresh("keys");
        QWidget window;
        auto* layout = new QVBoxLayout(&window);
        auto* fb = new FileBrowser;
        layout->addWidget(fb);
        auto* editCopy = new QAction(&window);
        editCopy->setShortcut(QKeySequence::Copy);
        auto* editPaste = new QAction(&window);
        editPaste->setShortcut(QKeySequence::Paste);
        window.addActions({editCopy, editPaste});
        QSignalSpy copies(editCopy, &QAction::triggered), pastes(editPaste, &QAction::triggered);
        window.resize(400, 600);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        fb->setView(FileBrowser::View::List);
        fb->setLocation(here);
        QVERIFY(shows(*fb, "two.txt"));
        fb->selectPath(here + "/two.txt");
        fb->currentView()->setFocus();
        QVERIFY(fb->hasEditFocus());
        QTest::keySequence(fb->currentView(), QKeySequence::Copy);
        QCOMPARE(FileBrowser::clipboardFiles(), QStringList({here + "/two.txt"}));
        // Into the folder shown, whatever is chosen (a folder too).
        fb->selectPath(here + "/a");
        QTest::keySequence(fb->currentView(), QKeySequence::Paste);
        QTRY_VERIFY(QFile::exists(here + "/two copy.txt"));
        QVERIFY(!QFile::exists(here + "/a/two.txt"));
        // The Tree: into the folder chosen, or the chosen file's.
        fb->setView(FileBrowser::View::Tree);
        QVERIFY(shows(*fb, "b"));
        static_cast<QTreeView*>(fb->currentView())->expand(fb->indexOf(here + "/b"));
        QTRY_VERIFY(fb->indexOf(here + "/b/deep").isValid());
        fb->selectPath(here + "/b/deep");
        fb->currentView()->setFocus();
        QTest::keySequence(fb->currentView(), QKeySequence::Paste);
        QTRY_VERIFY(QFile::exists(here + "/b/deep/two.txt"));
        fb->selectPath(here + "/a/inner.txt");
        if (!fb->indexOf(here + "/a/inner.txt").isValid()) {
            static_cast<QTreeView*>(fb->currentView())->expand(fb->indexOf(here + "/a"));
            QTRY_VERIFY(fb->indexOf(here + "/a/inner.txt").isValid());
            fb->selectPath(here + "/a/inner.txt");
        }
        QTest::keySequence(fb->currentView(), QKeySequence::Paste);
        QTRY_VERIFY(QFile::exists(here + "/a/two.txt"));
        QCOMPARE(copies.count(), 0);
        QCOMPARE(pastes.count(), 0);
        // Elsewhere (the filter's text) the keys are not its own.
        fb->filterEdit()->setFocus();
        QVERIFY(!fb->hasEditFocus());
    }

    // Dropped on the views: on a folder's row, into it; beside the rows,
    // into the folder shown; lit up where it would go.
    // A right click on one of several entries chosen keeps them all chosen,
    // its menu for them all: Copy N Items, Copy N Paths, Move N Items to
    // Trash. (The menu made the one clicked the current entry, which chose
    // it alone: Copy copied it alone.) On an entry not chosen: that one
    // alone. In the List, Tree and Details views.
    void aRightClickKeepsTheChosen()
    {
        const QString here = fresh("rightclick");
        QWidget window;
        auto* layout = new QVBoxLayout(&window);
        auto* fb = new FileBrowser;
        layout->addWidget(fb);
        window.resize(500, 600);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const QStringList both{here + "/one.sch", here + "/two.txt"};
        for (const FileBrowser::View view : {FileBrowser::View::List, FileBrowser::View::Tree, FileBrowser::View::Details}) {
            fb->setView(view);
            fb->setLocation(here);
            QVERIFY(shows(*fb, "two.txt"));
            QAbstractItemView* v = fb->currentView();
            // The menu at a right click on \a path: its items (by name),
            // its Copy chosen.
            const auto rightClick = [fb, v](const QString& path) {
                QStringList items;
                const QPoint at = v->visualRect(fb->indexOf(path)).center();
                QTest::mousePress(v->viewport(), Qt::RightButton, Qt::NoModifier, at);
                QTest::mouseRelease(v->viewport(), Qt::RightButton, Qt::NoModifier, at);
                QTimer::singleShot(0, fb, [fb, &items] {
                    const QList<QMenu*> menus = fb->findChildren<QMenu*>(QStringLiteral("fbContextMenu"));
                    if (menus.isEmpty()) return;
                    QMenu* menu = menus.last();
                    for (const char* name : {"fbCopy", "fbCopyPath", "fbTrash"})
                        if (auto* a = menu->findChild<QAction*>(QLatin1String(name))) items << a->text();
                    if (auto* copy = menu->findChild<QAction*>(QStringLiteral("fbCopy"))) copy->trigger();
                    menu->close();
                });
                QContextMenuEvent menuEvent(QContextMenuEvent::Mouse, at, v->viewport()->mapToGlobal(at));
                QCoreApplication::sendEvent(v->viewport(), &menuEvent);
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                return items;
            };
            fb->selectPath(both[0]);
            v->selectionModel()->select(fb->indexOf(both[1]), QItemSelectionModel::Select | QItemSelectionModel::Rows);
            QCOMPARE(fb->selectedPaths().size(), 2);
            QApplication::clipboard()->setText(QStringLiteral("before"));
            const QStringList items = rightClick(both[1]);
            QCOMPARE(items, QStringList({QStringLiteral("Copy 2 Items"), QStringLiteral("Copy 2 Paths"), QStringLiteral("Move 2 Items to Trash…")}));
            QStringList chosen = fb->selectedPaths();
            chosen.sort();
            QCOMPARE(chosen, both);
            QStringList copied = FileBrowser::clipboardFiles();
            copied.sort();
            QCOMPARE(copied, both);
            // One not chosen: it alone.
            const QStringList one = rightClick(here + "/a");
            QCOMPARE(one, QStringList({QStringLiteral("Copy"), QStringLiteral("Copy Path"), QStringLiteral("Move to Trash…")}));
            QCOMPARE(fb->selectedPaths(), QStringList({here + "/a"}));
            QCOMPARE(FileBrowser::clipboardFiles(), QStringList({here + "/a"}));
        }
        // Copy 2 Paths: a line each.
        fb->selectPath(both[0]);
        fb->currentView()->selectionModel()->select(fb->indexOf(both[1]), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        QMenu* menu = fb->contextMenuFor(both[1]);
        menu->findChild<QAction*>(QStringLiteral("fbCopyPath"))->trigger();
        QStringList lines = QApplication::clipboard()->text().split(QLatin1Char('\n'));
        lines.sort();
        QCOMPARE(lines, QStringList({QDir::toNativeSeparators(both[0]), QDir::toNativeSeparators(both[1])}));
        // Move 2 Items to Trash: asked about both (No: nothing).
        QString asked;
        answering([&] { menu->findChild<QAction*>(QStringLiteral("fbTrash"))->trigger(); },
                  [&](QWidget* w) {
                      auto* box = qobject_cast<QMessageBox*>(w);
                      if (box == nullptr || box->button(QMessageBox::No) == nullptr) return false;
                      asked = box->text();
                      box->button(QMessageBox::No)->click();
                      return true;
                  });
        QVERIFY2(asked.contains("2 items") && asked.contains("one.sch") && asked.contains("two.txt"), qPrintable(asked));
        QVERIFY(QFileInfo::exists(both[0]) && QFileInfo::exists(both[1]));
        delete menu;
        // Both to the trash, asked once (one inside a folder chosen with it
        // goes with the folder).
        answering([&] { fb->moveToTrash(QStringList{both[0], both[1], here + "/b", here + "/b/deep"}); },
                  [&](QWidget* w) {
                      auto* box = qobject_cast<QMessageBox*>(w);
                      if (box == nullptr || box->button(QMessageBox::Yes) == nullptr) return false;
                      asked = box->text();
                      box->button(QMessageBox::Yes)->click();
                      return true;
                  });
        QVERIFY2(asked.contains("3 items") && asked.contains("one.sch") && asked.contains("two.txt"), qPrintable(asked));
        for (const QString& name : {"one.sch", "two.txt", "b"}) {
            QVERIFY(!QFileInfo::exists(here + "/" + name));
            QVERIFY(QFileInfo::exists(qEnvironmentVariable("QUCS_TRASH_DIR") + "/" + name));
        }
        QVERIFY(QFileInfo::exists(qEnvironmentVariable("QUCS_TRASH_DIR") + "/b/deep/x.txt"));
    }

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

    // Renamed in the File Browser, the documents open from it follow (they
    // stayed on the old name, and the next save wrote it again); moved to
    // the trash, they close - not while one has unsaved changes (it would
    // have been saved where it was, in place of what was trashed).
    void openDocumentsFollowARenameAndATrash()
    {
        const QString here = top + "/renamedocs";
        write(here + "/proj/amp.txt", "amp");
        write(here + "/proj/notes.txt", "notes");
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(here + "/proj/amp.txt"));
        QVERIFY(app.gotoPage(here + "/proj/notes.txt"));
        const auto named = [&](const QString& path) {
            for (QucsDoc* d : app.allDocuments())
                if (d->getDocName() == path) return d;
            return static_cast<QucsDoc*>(nullptr);
        };
        QucsDoc* amp = named(here + "/proj/amp.txt");
        QucsDoc* notes = named(here + "/proj/notes.txt");
        QVERIFY(amp != nullptr && notes != nullptr);

        FileBrowser* fb = app.fileBrowserPanel();
        QVERIFY(fb->renameEntry(here + "/proj/amp.txt", "amp2.txt").isEmpty());
        QCOMPARE(amp->getDocName(), here + "/proj/amp2.txt");
        QWidget* w = QucsApp::documentWidget(amp);
        QCOMPARE(app.paneOf(w)->tabText(app.paneOf(w)->indexOf(w)), QStringLiteral("amp2.txt"));
        QVERIFY(fb->renameEntry(here + "/proj", "proj2").isEmpty());
        QCOMPARE(notes->getDocName(), here + "/proj2/notes.txt");
        QCOMPARE(amp->getDocName(), here + "/proj2/amp2.txt");

        // One with unsaved changes: the trash refused, nothing asked.
        notes->setDocChanged(true);
        QString said;
        answering([&] { fb->moveToTrash(here + "/proj2"); },
                  [&](QWidget* w) {
                      auto* box = qobject_cast<QMessageBox*>(w);
                      if (box == nullptr || box->button(QMessageBox::Ok) == nullptr) return false;   // (a question: fails the test)
                      said = box->text();
                      box->button(QMessageBox::Ok)->click();
                      return true;
                  });
        QVERIFY2(said.contains("notes.txt") && said.contains("unsaved"), qPrintable(said));
        QVERIFY(QFileInfo::exists(here + "/proj2/notes.txt"));
        notes->setDocChanged(false);

        // Asked, naming the documents that close; No: nothing happens.
        answering([&] { fb->moveToTrash(here + "/proj2"); },
                  [&](QWidget* w) {
                      auto* box = qobject_cast<QMessageBox*>(w);
                      if (box == nullptr || box->button(QMessageBox::No) == nullptr) return false;
                      said = box->text();
                      box->button(QMessageBox::No)->click();
                      return true;
                  });
        QVERIFY2(said.contains("amp2.txt") && said.contains("notes.txt") && said.contains("close"), qPrintable(said));
        QVERIFY(QFileInfo::exists(here + "/proj2/notes.txt"));
        QVERIFY(named(here + "/proj2/notes.txt") != nullptr);
        // Yes (the trash here, not the user's): gone, their tabs closed.
        answering([&] { fb->moveToTrash(here + "/proj2"); },
                  [&](QWidget* w) {
                      auto* box = qobject_cast<QMessageBox*>(w);
                      if (box == nullptr || box->button(QMessageBox::Yes) == nullptr) return false;
                      box->button(QMessageBox::Yes)->click();
                      return true;
                  });
        QVERIFY(!QFileInfo::exists(here + "/proj2"));
        QVERIFY(QFileInfo::exists(qEnvironmentVariable("QUCS_TRASH_DIR") + "/proj2/notes.txt"));
        QVERIFY(named(here + "/proj2/notes.txt") == nullptr);
        QVERIFY(named(here + "/proj2/amp2.txt") == nullptr);
        app.closeAllFiles();
    }
};

QTEST_MAIN(TestFileBrowserDrop)
#include "test_file_browser_drop.moc"
