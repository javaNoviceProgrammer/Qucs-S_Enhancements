/*
 * Documents the system hands over (upstream #973): named on the command
 * line (qucs-s FILE..., a project directory), given by a desktop's file
 * manager (paths or file: URLs), sent by the Finder or the Dock while the
 * application runs (QFileOpenEvent) - held until there is a window and no
 * modal dialog. And the files that make the systems offer Qucs-S for
 * schematics, data displays and symbols: the bundle's Info.plist, the
 * desktop entry and its MIME types, the Windows installer.
 */
#include <QtTest>
#include <QDialog>
#include <QDirIterator>
#include <QFileOpenEvent>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QXmlStreamReader>

#include "config.h"
#include "qucs.h"
#include "schematic.h"
#include "textdoc.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "systemopen.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

using qucs_s::systemopen::localPath;
using qucs_s::systemopen::Receiver;

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

const QByteArray kSchematic =
    "<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n</Properties>\n<Symbol>\n</Symbol>\n"
    "<Components>\n</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n";

QString write(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return {};
    f.write(bytes);
    return QFileInfo(path).absoluteFilePath();
}

// The text of a file, lines ending in "\n" (the installer script's end in
// "\r\n").
QString read(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).replace("\r\n", "\n") : QString();
}

} // namespace

class TestSystemOpen : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

    // Runs fn with a message box expected: returns the box's text.
    static QString withMessageBox(const std::function<void()>& fn)
    {
        QString text;
        QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, [&] {
            for (QWidget* w : QApplication::topLevelWidgets())
                if (auto* box = qobject_cast<QMessageBox*>(w); box && box->isVisible()) {
                    text = box->text();
                    box->accept();
                }
        });
        timer.start(10);
        fn();
        return text;
    }

    static int documents(QucsApp& app) { return app.allDocuments().size(); }

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

    void aPathOrAFileUrl()
    {
        const QString cwd = QDir::currentPath();
        QDir::setCurrent(dir.path());
        // (The current directory as the system has it: /private/var on macOS.)
        QCOMPARE(localPath("amp.sch"), QDir::current().absoluteFilePath("amp.sch"));
        QCOMPARE(localPath("sub/../amp.sch"), QDir::current().absoluteFilePath("amp.sch"));
        QCOMPARE(localPath(QUrl::fromLocalFile(dir.filePath("a b.sch")).toString()), dir.filePath("a b.sch"));
        const QString encoded = QUrl::fromLocalFile(dir.filePath("a b.sch")).toString(QUrl::FullyEncoded);
        QVERIFY(encoded.contains("a%20b.sch"));
        QCOMPARE(localPath(encoded), dir.filePath("a b.sch"));
        QCOMPARE(localPath(dir.filePath("x.dpl")), dir.filePath("x.dpl"));
        QVERIFY(localPath("https://example.org/amp.sch").isEmpty());
        QVERIFY(localPath("").isEmpty());
        QDir::setCurrent(cwd);
    }

    // Files by path and by URL, relative to the current directory too; the
    // untitled placeholder goes; a document already open is not opened
    // again.
    void documentsOpen()
    {
        const QString amp = write(dir.filePath("docs/amp.sch"), kSchematic);
        const QString plots = write(dir.filePath("docs/amp.dpl"), kSchematic);
        const QString net = write(dir.filePath("docs/amp.cir"), "* netlist\n.end\n");
        QucsApp app(false);
        MainGuard guard(&app);
        QCOMPARE(documents(app), 1);   // the untitled placeholder

        QCOMPARE(app.openFromSystem({amp, QUrl::fromLocalFile(plots).toString()}), 2);
        QVERIFY(app.findDoc(amp) != nullptr);
        QVERIFY(app.findDoc(plots) != nullptr);
        QCOMPARE(documents(app), 2);
        QCOMPARE(QucsSettings.QucsWorkDir.absolutePath(), QFileInfo(amp).absolutePath());

        const QString cwd = QDir::currentPath();
        QDir::setCurrent(dir.filePath("docs"));
        QCOMPARE(app.openFromSystem({"amp.cir", "amp.sch"}), 2);
        QDir::setCurrent(cwd);
        // amp.sch once: the relative name is the same file, if not the same
        // string (/private/var on macOS).
        QCOMPARE(documents(app), 3);
        QCOMPARE(app.getDoc()->getDocName(), amp);   // the last named is in front
        QucsDoc* cir = nullptr;
        for (QucsDoc* d : app.allDocuments())
            if (QFileInfo(d->getDocName()).fileName() == "amp.cir") cir = d;
        QVERIFY(cir != nullptr);
        QCOMPARE(QFileInfo(cir->getDocName()).canonicalFilePath(), QFileInfo(net).canonicalFilePath());
        QVERIFY(qobject_cast<TextDoc*>(QucsApp::documentWidget(cir)) != nullptr);
        app.closeAllFiles();
    }

    // A project directory opens as the project, before the documents; a
    // document outside it leaves the project's directory the working one.
    void aProjectDirectory()
    {
        const QString project = QDir(dir.filePath("demo_prj")).absolutePath();
        const QString inside = write(project + "/demo.sch", kSchematic);
        const QString outside = write(dir.filePath("elsewhere/other.sch"), kSchematic);
        QucsApp app(false);
        MainGuard guard(&app);

        QCOMPARE(app.openFromSystem({inside, project}), 2);
        QCOMPARE(app.ProjName, QStringLiteral("demo"));
        QVERIFY(app.findDoc(inside) != nullptr);
        QCOMPARE(documents(app), 1);

        QCOMPARE(app.openFromSystem({outside}), 1);
        QCOMPARE(app.ProjName, QStringLiteral("demo"));
        QCOMPARE(QucsSettings.QucsWorkDir.absolutePath(), project);
        app.closeAllFiles();
    }

    // What cannot be opened is said, once, and the rest opens.
    void whatCannotBeOpened()
    {
        const QString amp = write(dir.filePath("docs/amp.sch"), kSchematic);
        QDir().mkpath(dir.filePath("plain"));
        QucsApp app(false);
        MainGuard guard(&app);
        int opened = -1;
        const QString text = withMessageBox([&] {
            opened = app.openFromSystem({dir.filePath("missing.sch"), dir.filePath("plain"), amp,
                                         "https://example.org/x.sch"});
        });
        QCOMPARE(opened, 1);
        QVERIFY(app.findDoc(amp) != nullptr);
        QVERIFY2(text.contains("missing.sch") && text.contains("plain") && text.contains("https://example.org/x.sch"),
                 qPrintable(text));
        app.closeAllFiles();
    }

    // The Finder's and the Dock's requests: kept until there is a target,
    // handed over as one batch, never under a modal dialog.
    void openEventsAreKeptAndHandedOver()
    {
        Receiver receiver;
        QList<QStringList> batches;

        QFileOpenEvent first(dir.filePath("one.sch"));
        QFileOpenEvent second(QUrl::fromLocalFile(dir.filePath("two.sch")));
        QVERIFY(QCoreApplication::sendEvent(qApp, &first));
        QVERIFY(QCoreApplication::sendEvent(qApp, &second));
        QTest::qWait(20);
        QCOMPARE(receiver.pending(), QStringList({dir.filePath("one.sch"), dir.filePath("two.sch")}));

        receiver.setTarget([&](const QStringList& files) { batches << files; });
        QTRY_COMPARE(batches.size(), 1);
        QCOMPARE(batches.first().size(), 2);
        QVERIFY(receiver.pending().isEmpty());

        // Under a modal dialog: after it.
        QDialog modal;
        modal.setModal(true);
        modal.show();
        QTRY_VERIFY(QApplication::activeModalWidget() == &modal);
        QFileOpenEvent third(dir.filePath("three.sch"));
        QCoreApplication::sendEvent(qApp, &third);
        QTest::qWait(300);
        QCOMPARE(batches.size(), 1);
        modal.close();
        QTRY_COMPARE(batches.size(), 2);
        QCOMPARE(batches.last(), QStringList({dir.filePath("three.sch")}));

        // Every other event reaches its object.
        struct Recorder : QObject {
            int users = 0;
            bool event(QEvent* e) override
            {
                if (e->type() != QEvent::User) return QObject::event(e);
                ++users;
                return true;
            }
        } target;
        QEvent other(QEvent::User);
        QCoreApplication::sendEvent(&target, &other);
        QCOMPARE(target.users, 1);
    }

    void anOpenEventOpensTheDocument()
    {
        const QString amp = write(dir.filePath("finder/amp.sch"), kSchematic);
        QucsApp app(false);
        MainGuard guard(&app);
        Receiver receiver;
        receiver.setTarget([&app](const QStringList& files) { app.openFromSystem(files); });
        QFileOpenEvent request(amp);
        QCoreApplication::sendEvent(qApp, &request);
        QTRY_VERIFY(app.findDoc(amp) != nullptr);
        QCOMPARE(documents(app), 1);
        app.closeAllFiles();
    }

#ifdef QUCS_BUNDLE_INFO_PLIST
    // The bundle declares the document types (and CMake filled in the rest
    // of its template).
    void theBundleDeclaresTheDocuments()
    {
        QVERIFY2(QFileInfo::exists(QUCS_BUNDLE_INFO_PLIST), QUCS_BUNDLE_INFO_PLIST);
        QSettings plist(QUCS_BUNDLE_INFO_PLIST, QSettings::NativeFormat);
        QCOMPARE(plist.value("CFBundleExecutable").toString(), QStringLiteral("qucs-s"));
        QCOMPARE(plist.value("CFBundleIconFile").toString(), QStringLiteral("qucs.icns"));

        QMap<QString, QString> extensionOf;   // UTI -> extension
        for (const QVariant& v : plist.value("UTExportedTypeDeclarations").toList()) {
            const QVariantMap type = v.toMap();
            extensionOf[type["UTTypeIdentifier"].toString()] =
                type["UTTypeTagSpecification"].toMap()["public.filename-extension"].toList().value(0).toString();
        }
        QMap<QString, QString> rankOf;        // extension -> handler rank
        for (const QVariant& v : plist.value("CFBundleDocumentTypes").toList()) {
            const QVariantMap type = v.toMap();
            QCOMPARE(type["CFBundleTypeRole"].toString(), QStringLiteral("Editor"));
            for (const QVariant& uti : type["LSItemContentTypes"].toList())
                rankOf[extensionOf.value(uti.toString(), "?")] = type["LSHandlerRank"].toString();
            for (const QVariant& ext : type["CFBundleTypeExtensions"].toList())
                rankOf[ext.toString()] = type["LSHandlerRank"].toString();
        }
        QCOMPARE(rankOf.value("sch"), QStringLiteral("Default"));
        QCOMPARE(rankOf.value("dpl"), QStringLiteral("Owner"));
        QCOMPARE(rankOf.value("sym"), QStringLiteral("Default"));
        QCOMPARE(rankOf.value("cir"), QStringLiteral("Alternate"));
        QCOMPARE(rankOf.value("va"), QStringLiteral("Alternate"));
        QVERIFY(!rankOf.contains("?"));
    }
#endif

    // The desktop entry names the types and takes local files; the MIME
    // package defines each by its extension and, where others use it too,
    // by the first line every Qucs schematic, data display and symbol has.
    void theDesktopKnowsTheTypes()
    {
        const QString desktop = read(QUCS_SOURCE_DIR "/qucs/qucs-s.desktop");
        QVERIFY(desktop.contains("\nExec=qucs-s %F\n"));
        QStringList types;
        for (const QString& line : desktop.split('\n'))
            if (line.startsWith("MimeType="))
                types = line.mid(9).split(';', Qt::SkipEmptyParts);
        QCOMPARE(types.size(), 3);

        QFile xml(QUCS_SOURCE_DIR "/qucs/qucs-s-mime.xml");
        QVERIFY(xml.open(QIODevice::ReadOnly));
        QXmlStreamReader reader(&xml);
        QMap<QString, QString> globOf, magicOf;
        QString type;
        while (!reader.atEnd()) {
            if (!reader.readNextStartElement()) continue;
            if (reader.name() == u"mime-type") type = reader.attributes().value("type").toString();
            else if (reader.name() == u"glob") globOf[type] = reader.attributes().value("pattern").toString();
            else if (reader.name() == u"match") magicOf[type] = reader.attributes().value("value").toString();
        }
        QVERIFY2(!reader.hasError(), qPrintable(reader.errorString()));
        for (const QString& t : types) QVERIFY2(globOf.contains(t), qPrintable(t));
        QCOMPARE(globOf.value("application/x-qucs-schematic"), QStringLiteral("*.sch"));
        QCOMPARE(globOf.value("application/x-qucs-data-display"), QStringLiteral("*.dpl"));
        QCOMPARE(globOf.value("application/x-qucs-symbol"), QStringLiteral("*.sym"));

        // The first line is what the files really start with.
        QStringList samples;
        QDirIterator it(QUCS_SOURCE_DIR "/library", {"*.sym"}, QDir::Files, QDirIterator::Subdirectories);
        if (it.hasNext()) samples << it.next();
        QDirIterator sch(QUCS_EXAMPLES_DIR, {"*.sch"}, QDir::Files, QDirIterator::Subdirectories);
        if (sch.hasNext()) samples << sch.next();
        QCOMPARE(samples.size(), 2);
        for (const QString& f : samples) {
            const QString magic = f.endsWith(".sym") ? magicOf.value("application/x-qucs-symbol")
                                                     : magicOf.value("application/x-qucs-schematic");
            QVERIFY2(!magic.isEmpty() && read(f).startsWith(magic), qPrintable(f));
        }
    }

    // The installer registers qucs-s.exe "FILE" for the three types,
    // alongside whatever else handles them.
    void theWindowsInstallerRegistersTheTypes()
    {
        const QString iss = read(QUCS_SOURCE_DIR "/contrib/InnoSetup/qucs.iss");
        QVERIFY(iss.contains("\nChangesAssociations=yes\n"));
        for (const auto& [ext, progId] : std::initializer_list<std::pair<QString, QString>>{
                 {".sch", "QucsS.Schematic"}, {".dpl", "QucsS.DataDisplay"}, {".sym", "QucsS.Symbol"}}) {
            QVERIFY2(iss.contains(QStringLiteral("Subkey: \"Software\\Classes\\%1\\OpenWithProgids\"; ValueType: string; "
                                                 "ValueName: \"%2\"").arg(ext, progId)), qPrintable(ext));
            // (Windows' own "%1" is the file.)
            const QString command = "Subkey: \"Software\\Classes\\" + progId + "\\shell\\open\\command\"; "
                                    "ValueType: string; ValueName: \"\"; "
                                    "ValueData: \"\"\"{app}\\bin\\qucs-s.exe\"\" \"\"%1\"\"\"";
            QVERIFY2(iss.contains(command), qPrintable(command));
            QVERIFY2(iss.contains(QStringLiteral("SupportedTypes\"; ValueType: string; ValueName: \"%1\"").arg(ext)),
                     qPrintable(ext));
            // Never the extension's own default: that stays another program's.
            QVERIFY(!iss.contains(QStringLiteral("Subkey: \"Software\\Classes\\%1\"; ").arg(ext)));
        }
    }
};

QTEST_MAIN(TestSystemOpen)
#include "test_system_open.moc"
