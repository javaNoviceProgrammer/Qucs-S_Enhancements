/*
 * A ZIP archive in a tab (zipdoc.h), as Eclipse's zip editor has one: its
 * files and folders listed with sizes, packing and checksums, in a tree or
 * a list, filtered; files and folders added (a name taken asked about),
 * folders made, entries renamed and deleted - each a step to undo - and
 * the archive saved, the files not changed copied as they were packed;
 * extracted, never out of the folder chosen; a file opened in Qucs-S as a
 * copy, followed: saved, it is in the archive again; nothing run.
 */
#include <QtTest>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QRandomGenerator>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeView>
#include <QUndoStack>
#include <QtEndian>

#include "config.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "qucs.h"
#include "zipdoc.h"
#include "zipfile.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

using namespace qucs_s;

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

bool write(const QString& path, const QByteArray& bytes)
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

const QByteArray kSchematic = "<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n</Properties>\n<Components>\n</Components>\n";

// An archive as another program makes it: a folder, a text deflated, a
// schematic, an image stored, times and a comment of its own.
QByteArray sampleArchive()
{
    QList<zip::Part> parts;
    const auto part = [&](const QString& name, const QByteArray& data, const QDateTime& when) {
        zip::Part p;
        p.item.name = name;
        p.item.modified = when;
        p.data = data;
        parts << p;
    };
    const QDateTime when(QDate(2024, 5, 1), QTime(13, 30, 10));
    part("docs/", {}, when);
    part("docs/notes.txt", QByteArray("notes about the amplifier\n").repeated(50), when);
    part("amp.sch", kSchematic, when.addDays(1));
    QByteArray noise(256, Qt::Uninitialized);   // (packs no smaller: stored)
    quint32 x = 1926;
    for (char& c : noise) c = char((x = x * 1103515245u + 12345u) >> 24);
    part("docs/logo.png", QByteArray("\x89PNG\r\n\x1a\n") + noise, when);
    part("tool.exe", "MZ this would run", when);
    return zip::write(parts, "made for the test");
}

} // namespace

class TestZipDoc : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

    ZipDoc* current(QucsApp& app) { return qobject_cast<ZipDoc*>(app.DocumentTab->currentWidget()); }

    // The names of the rows under \a parent (the top when invalid).
    static QStringList shown(ZipDoc* doc, const QModelIndex& parent = QModelIndex())
    {
        QStringList names;
        for (int r = 0; r < doc->model()->rowCount(parent); ++r) names << doc->model()->index(r, 0, parent).data().toString();
        return names;
    }

    static QModelIndex rowNamed(ZipDoc* doc, const QString& name, const QModelIndex& parent = QModelIndex())
    {
        for (int r = 0; r < doc->model()->rowCount(parent); ++r) {
            const QModelIndex i = doc->model()->index(r, 0, parent);
            if (i.data(ZipDoc::NameRole).toString() == name) return i;
            const QModelIndex inside = rowNamed(doc, name, i);
            if (inside.isValid()) return inside;
        }
        return {};
    }

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
        Module::registerModules();
    }

    // The archive's directory read without inflating; a file extracted on
    // its own; packed bytes copied as they are keep the archive as it was:
    // times, a Unix mode, comments.
    void theArchiveLayer()
    {
        const QByteArray archive = sampleArchive();
        QString why;
        const QList<zip::Item> items = zip::list(archive, &why);
        QVERIFY2(why.isEmpty(), qPrintable(why));
        QCOMPARE(items.size(), 5);
        QVERIFY(items.at(0).folder());
        QCOMPARE(items.at(1).name, QString("docs/notes.txt"));
        QCOMPARE(items.at(1).method, quint16(8));
        QVERIFY(items.at(1).packed < items.at(1).size);
        QCOMPARE(items.at(1).modified, QDateTime(QDate(2024, 5, 1), QTime(13, 30, 10)));
        QCOMPARE(items.at(3).method, quint16(0));   // not smaller deflated: stored
        QCOMPARE(zip::comment(archive), QString("made for the test"));
        bool ok = false;
        QCOMPARE(zip::extract(archive, items.at(2), &why, &ok), kSchematic);
        QVERIFY(ok);

        // Copied as they are, with a mode and a comment of their own.
        QList<zip::Part> copy;
        for (const zip::Item& item : items) {
            zip::Part p;
            p.item = item;
            p.copied = !item.folder();
            p.raw = zip::packedData(archive, item);
            copy << p;
        }
        copy[2].item.madeBy = quint16((3 << 8) | 20);
        copy[2].item.externalAttributes = 0100755u << 16;
        copy[2].item.comment = "the amplifier";
        const QByteArray again = zip::write(copy, "made for the test");
        const QList<zip::Item> read = zip::list(again, &why);
        QCOMPARE(read.size(), 5);
        for (int i = 0; i < 5; ++i) {
            QCOMPARE(read.at(i).name, items.at(i).name);
            QCOMPARE(read.at(i).crc, items.at(i).crc);
            QCOMPARE(read.at(i).packed, items.at(i).packed);
            QCOMPARE(zip::packedData(again, read.at(i)), zip::packedData(archive, items.at(i)));
        }
        QCOMPARE(read.at(2).externalAttributes >> 16, 0100755u);
        QCOMPARE(read.at(2).comment, QString("the amplifier"));
        QCOMPARE(zip::read(again).at(0).data, QByteArray("notes about the amplifier\n").repeated(50));   // (folders are not entries)
    }

    // Opened in a tab from anywhere a file opens: a tree of its folders,
    // each row with its size, packed size, ratio, time, packing, checksum;
    // a list of paths; filtered by name.
    void itOpensInATab()
    {
        const QString file = dir.filePath("sample.zip");
        QVERIFY(write(file, sampleArchive()));
        QVERIFY(QucsApp::isArchiveFile("x/y.ZIP"));
        QVERIFY(!QucsApp::isArchiveFile("x/y.xlsx"));
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(file));
        ZipDoc* doc = current(app);
        QVERIFY(doc != nullptr);
        QVERIFY(QucsApp::isArchiveDocument(doc));
        QCOMPARE(doc->names(), QStringList({"docs/", "docs/notes.txt", "amp.sch", "docs/logo.png", "tool.exe"}));
        QCOMPARE(shown(doc), QStringList({"docs", "amp.sch", "tool.exe"}));   // folders first
        const QModelIndex docs = rowNamed(doc, "docs/");
        QCOMPARE(shown(doc, docs), QStringList({"logo.png", "notes.txt"}));
        const QModelIndex notes = rowNamed(doc, "docs/notes.txt");
        const auto cell = [&](const QModelIndex& i, int column) { return i.sibling(i.row(), column).data().toString(); };
        QCOMPARE(cell(notes, 1), QLocale().toString(1300));
        QVERIFY(cell(notes, 3).endsWith("%"));
        QCOMPARE(cell(notes, 4), QString("2024-05-01 13:30"));
        QCOMPARE(cell(notes, 5), QString("Deflated"));
        QCOMPARE(cell(notes, 6).size(), 8);
        QCOMPARE(cell(rowNamed(doc, "docs/logo.png"), 5), QString("Stored"));
        QVERIFY(!cell(docs, 1).isEmpty());   // a folder: what is in it
        QVERIFY2(doc->statusLabel()->text().startsWith("4 files, 1 folder"), qPrintable(doc->statusLabel()->text()));
        QVERIFY(doc->statusLabel()->toolTip().contains("made for the test"));

        doc->setTreeMode(false);
        QCOMPARE(shown(doc), QStringList({"docs/", "amp.sch", "docs/logo.png", "docs/notes.txt", "tool.exe"}));
        doc->setFilter("NOTES");
        QCOMPARE(shown(doc), QStringList({"docs/notes.txt"}));
        doc->setTreeMode(true);
        QCOMPARE(shown(doc), QStringList({"docs"}));
        QCOMPARE(shown(doc, rowNamed(doc, "docs/")), QStringList({"notes.txt"}));
        doc->setFilter(QString());
        QVERIFY(!doc->getDocChanged());
        app.closeAllFiles();
    }

    // Files and folders added (a name taken refused, skipped or replaced),
    // a folder made, entries renamed and deleted: each one step to undo.
    // Saved: the archive written again, the files not changed copied as
    // they were packed, the comment kept.
    void itIsEditedAndSaved()
    {
        const QString file = dir.filePath("edited.zip");
        const QByteArray original = sampleArchive();
        QVERIFY(write(file, original));
        const QString outside = dir.filePath("outside");
        QVERIFY(write(outside + "/filter.sch", kSchematic));
        QVERIFY(write(outside + "/notes.txt", "new notes\n"));
        QVERIFY(write(outside + "/models/diode.lib", ".model D1 D(Is=1e-14)\n"));
        QVERIFY(write(outside + "/models/empty/.keep", ""));

        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(file));
        ZipDoc* doc = current(app);
        QVERIFY(doc != nullptr);

        QString why;
        QCOMPARE(doc->add({outside + "/filter.sch"}, "", ZipDoc::Clash::Refuse, &why), QStringList({"filter.sch"}));
        QVERIFY(doc->getDocChanged());
        QCOMPARE(doc->clashes({outside + "/notes.txt"}, "docs/"), QStringList({"docs/notes.txt"}));
        QVERIFY(doc->add({outside + "/notes.txt"}, "docs/", ZipDoc::Clash::Refuse, &why).isEmpty());
        QVERIFY2(why.contains("docs/notes.txt"), qPrintable(why));
        QVERIFY(doc->add({outside + "/notes.txt"}, "docs/", ZipDoc::Clash::Skip, &why).isEmpty());
        QCOMPARE(doc->add({outside + "/notes.txt"}, "docs", ZipDoc::Clash::Replace, &why), QStringList({"docs/notes.txt"}));
        QCOMPARE(doc->contents("docs/notes.txt"), QByteArray("new notes\n"));
        const QStringList added = doc->add({outside + "/models"}, "docs/", ZipDoc::Clash::Refuse, &why);
        QVERIFY2(added.contains("docs/models/") && added.contains("docs/models/diode.lib") && added.contains("docs/models/empty/.keep"),
                 qPrintable(added.join(' ')));
        QVERIFY(doc->makeFolder("", "results", &why));
        QVERIFY(!doc->makeFolder("", "results", &why));   // there
        QVERIFY(!doc->makeFolder("", "../up", &why));
        QVERIFY(doc->rename("amp.sch", "amplifier.sch", &why));
        QVERIFY(!doc->rename("filter.sch", "amplifier.sch", &why));   // taken
        QVERIFY(!doc->rename("filter.sch", "../x.sch", &why));
        QVERIFY(doc->rename("docs/", "documents", &why));
        QVERIFY(doc->names().contains("documents/models/diode.lib"));
        doc->remove({"tool.exe", "documents/logo.png"});
        QVERIFY(!doc->names().contains("tool.exe"));

        // Each a step to undo.
        const QStringList now = doc->names();
        doc->undo();
        QVERIFY(doc->names().contains("tool.exe"));
        doc->undo();
        QVERIFY(doc->names().contains("docs/notes.txt"));
        doc->redo();
        doc->redo();
        QCOMPARE(doc->names(), now);
        QCOMPARE(doc->undoStack()->count(), 7);   // (what was refused or skipped: no step)

        QCOMPARE(doc->save(), 0);
        QVERIFY(!doc->getDocChanged());
        const QByteArray saved = read(file);
        QCOMPARE(zip::comment(saved), QString("made for the test"));
        const QList<zip::Entry> entries = zip::read(saved, &why);
        QVERIFY2(!entries.isEmpty(), qPrintable(why));
        QMap<QString, QByteArray> byName;
        for (const zip::Entry& e : entries) byName.insert(e.name, e.data);
        QCOMPARE(byName.value("amplifier.sch"), kSchematic);
        QCOMPARE(byName.value("filter.sch"), kSchematic);
        QCOMPARE(byName.value("documents/notes.txt"), QByteArray("new notes\n"));
        QCOMPARE(byName.value("documents/models/diode.lib"), QByteArray(".model D1 D(Is=1e-14)\n"));
        QVERIFY(!byName.contains("tool.exe") && !byName.contains("documents/logo.png"));
        QVERIFY(zip::list(saved).size() > 0);
        bool results = false;
        for (const zip::Item& item : zip::list(saved)) results = results || item.name == "results/";
        QVERIFY(results);
        // The schematic, renamed, not changed: its packed bytes as they were.
        zip::Item before, after;
        for (const zip::Item& item : zip::list(original)) if (item.name == "amp.sch") before = item;
        for (const zip::Item& item : zip::list(saved)) if (item.name == "amplifier.sch") after = item;
        QCOMPARE(zip::packedData(saved, after), zip::packedData(original, before));
        QCOMPARE(after.modified, before.modified);

        // Saved under another name.
        const QString other = dir.filePath("copy of edited.zip");
        QVERIFY(app.saveDocumentAs(doc, other));
        QCOMPARE(zip::read(read(other)).size(), entries.size());
        app.closeAllFiles();
    }

    // Extracted into a folder: every entry, or those selected (a folder
    // with all in it); a file there already left unless it is to be
    // overwritten; a name that would leave the folder ("../x", "/x")
    // refused - nothing written outside.
    void itIsExtracted()
    {
        QList<zip::Part> parts;
        for (const char* name : {"a/one.txt", "a/b/two.txt", "../evil.txt", "/abs.txt", "c/../../evil2.txt", "C:/drive.txt"}) {
            zip::Part p;
            p.item.name = QString::fromLatin1(name);
            p.data = QByteArray(name);
            parts << p;
        }
        const QString file = dir.filePath("slip.zip");
        QVERIFY(write(file, zip::write(parts)));
        ZipDoc doc(nullptr, file);
        QVERIFY(doc.load());
        const QString into = dir.filePath("extracted/here");
        QString why;
        QStringList written = doc.extract({}, into, false, &why);
        QCOMPARE(written.size(), 2);
        QCOMPARE(read(into + "/a/one.txt"), QByteArray("a/one.txt"));
        QCOMPARE(read(into + "/a/b/two.txt"), QByteArray("a/b/two.txt"));
        QVERIFY2(why.contains("../evil.txt") && why.contains("/abs.txt") && why.contains("evil2.txt") && why.contains("drive.txt"),
                 qPrintable(why));
        QVERIFY(!QFileInfo::exists(dir.filePath("extracted/evil.txt")));
        QVERIFY(!QFileInfo::exists(dir.filePath("evil2.txt")));
        QVERIFY(!QFileInfo::exists("/abs.txt"));

        QVERIFY(write(into + "/a/one.txt", "mine"));
        written = doc.extract({"a/"}, into, false, &why);
        QCOMPARE(read(into + "/a/one.txt"), QByteArray("mine"));   // left
        QVERIFY(why.contains("there already"));
        written = doc.extract({"a/one.txt"}, into, true, &why);
        QCOMPARE(read(into + "/a/one.txt"), QByteArray("a/one.txt"));
    }

    // A file opened in Qucs-S: a copy, followed - saved there, it is in the
    // archive again (to be saved with it); a program is not opened (nothing
    // of an archive is run); an encrypted file, or one bigger than is read,
    // listed and kept but not opened.
    void aFileOpensAsACopyAndComesBack()
    {
        const QByteArray archive = sampleArchive();
        // The last file marked encrypted, as a password-protected one is.
        QByteArray locked = archive;
        const qsizetype dirStart = locked.lastIndexOf(QByteArray("PK\x01\x02", 4));   // (its directory's last: tool.exe)
        quint16 flags = qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(locked.constData()) + dirStart + 8);
        qToLittleEndian<quint16>(flags | 1, reinterpret_cast<uchar*>(locked.data()) + dirStart + 8);
        const QString file = dir.filePath("open.zip");
        QVERIFY(write(file, locked));

        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(file));
        ZipDoc* doc = current(app);
        QVERIFY(doc != nullptr);
        QVERIFY(doc->entries().last().item.encrypted());
        QString why;
        QVERIFY(doc->openEntry("tool.exe", &why).isEmpty());
        QVERIFY2(why.contains("not a document Qucs-S opens"), qPrintable(why));
        bool ok = true;
        doc->contents("tool.exe", &why, &ok);
        QVERIFY(!ok && why.contains("encrypted"));

        const QString copy = doc->openEntry("docs/notes.txt", &why);
        QVERIFY2(!copy.isEmpty(), qPrintable(why));
        QCOMPARE(read(copy), QByteArray("notes about the amplifier\n").repeated(50));
        QVERIFY(app.getDoc()->getDocName() == copy);   // in front, in a tab
        QVERIFY(!doc->getDocChanged());
        QVERIFY(write(copy, "notes, edited in Qucs-S\n"));   // as its tab saves it
        QTRY_COMPARE_WITH_TIMEOUT(doc->contents("docs/notes.txt"), QByteArray("notes, edited in Qucs-S\n"), 5000);
        QVERIFY(doc->getDocChanged());
        QCOMPARE(doc->undoStack()->undoText(), QString("Change notes.txt"));
        QCOMPARE(doc->openEntry("docs/notes.txt", &why), copy);   // the same copy

        // Saved: the encrypted file copied as it was packed.
        QCOMPARE(doc->save(), 0);
        const QList<zip::Item> saved = zip::list(read(file));
        QVERIFY(saved.last().encrypted());
        QCOMPARE(zip::packedData(read(file), saved.last()), zip::packedData(locked, zip::list(locked).last()));
        for (QucsDoc* d : app.allDocuments()) d->setDocChanged(false);
        app.closeAllFiles();
    }

    // Closed with changes not saved (Discard): no crash - its undo stack,
    // clearing, said so to a status line already gone.
    void closedWithChangesNotSaved()
    {
        const QString file = dir.filePath("discard.zip");
        QVERIFY(write(file, sampleArchive()));
        const QString extra = dir.filePath("extra.txt");
        QVERIFY(write(extra, "x"));
        {
            ZipDoc doc(nullptr, file);
            QVERIFY(doc.load());
            QCOMPARE(doc.add({extra}, "", ZipDoc::Clash::Refuse).size(), 1);
            QVERIFY(doc.getDocChanged());
        }
        QCOMPARE(zip::list(read(file)).size(), 5);   // (not written)
    }

    // Any bytes - an archive damaged anywhere - listed, read and opened
    // without a crash (and never inflated past what they say). Seeded.
    void damagedArchivesAreSafe()
    {
        const QByteArray good = sampleArchive();
        QRandomGenerator random(927);
        const QString file = dir.filePath("damaged.zip");
        for (int n = 0; n < 1500; ++n) {
            QByteArray bytes = good;
            const int changes = 1 + int(random.bounded(8));
            for (int k = 0; k < changes; ++k) bytes[random.bounded(int(bytes.size()))] = char(random.bounded(256));
            if (random.bounded(10) == 0) bytes.truncate(random.bounded(int(bytes.size())));
            QString why;
            for (const zip::Item& item : zip::list(bytes, &why)) {
                bool ok = false;
                const QByteArray data = zip::extract(bytes, item, &why, &ok);
                QVERIFY(!ok || quint32(data.size()) == item.size);
            }
            zip::read(bytes, &why);
            if (n % 50 == 0) {
                QVERIFY(write(file, bytes));
                ZipDoc doc(nullptr, file);
                misc::ErrorCapture quiet;   // (its complaints kept, not shown)
                if (doc.load()) doc.archive();
            }
        }
    }

    // Files dropped from anywhere go into the folder under them.
    void filesDroppedAreAdded()
    {
        const QString file = dir.filePath("drop.zip");
        QVERIFY(write(file, sampleArchive()));
        const QString dropped = dir.filePath("dropped/readme.md");
        QVERIFY(write(dropped, "# Read me\n"));
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(1000, 700);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        QVERIFY(app.gotoPage(file));
        ZipDoc* doc = current(app);
        QVERIFY(doc != nullptr);
        QWidget* viewport = doc->view()->viewport();
        const QPoint onDocs = doc->view()->visualRect(rowNamed(doc, "docs/")).center();
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(dropped)});
        QDragEnterEvent enter(onDocs, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(viewport, &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent drop(onDocs, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(viewport, &drop);
        QVERIFY(drop.isAccepted());
        QTRY_VERIFY_WITH_TIMEOUT(doc->names().contains("docs/readme.md"), 3000);
        QCOMPARE(doc->contents("docs/readme.md"), QByteArray("# Read me\n"));
        doc->setDocChanged(false);
        app.closeAllFiles();
    }
};

QTEST_MAIN(TestZipDoc)
#include "test_zip_doc.moc"
