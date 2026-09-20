/*
 * Autosave, session recovery and the crash handler (WS1.7).
 *
 * The crash handler is exercised for real: this binary re-runs itself with
 * --crash-segv / --crash-throw, which install the handler, register an
 * emergency callback that writes a marker file, and then crash. The parent
 * checks the report and the marker.
 */
#include <QtTest>
#include <QProcess>
#include <QTemporaryDir>

#include "config.h"
#include "qucs.h"
#include "schematic.h"
#include "textdoc.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "autosave.h"
#include "crashhandler.h"
#include "extsimkernels/spicecompat.h"

namespace autosave = qucs_s::autosave;
namespace crash = qucs_s::crash;

static const char* kExample = "ngspice/RF/Miscellaneous/RCL_resonance.sch";

class TestAutosave : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString example(const QString& rel) const
    {
        return QStringLiteral(QUCS_EXAMPLES_DIR) + QStringLiteral("/") + rel;
    }
    QString copyExample(const QString& name) const
    {
        const QString dst = dir.filePath(name);
        QFile::remove(dst);
        QFile::copy(example(kExample), dst);
        return dst;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        QucsSettings.DefaultSimulator = spicecompat::simNotSpecified;
        QucsSettings.maxUndo = 20;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        autosave::setDirectory(dir.filePath("autosave"));
        crash::setReportDirectory(dir.filePath("crash-reports"));
    }

    void cleanup()
    {
        autosave::clear();
    }

    // ---- QucsDoc::writeTo -------------------------------------------------
    void schematicWritesItselfElsewhere()
    {
        Schematic sch(nullptr, example(kExample));
        QVERIFY(sch.load());
        const QString copy = dir.filePath("copy.sch");
        QVERIFY(sch.writeTo(copy));
        QVERIFY(!sch.getDocChanged());                    // untouched
        QCOMPARE(sch.getDocName(), example(kExample));    // untouched

        Schematic again(nullptr, copy);
        QVERIFY(again.load());
        QCOMPARE(again.a_DocComps.size(), sch.a_DocComps.size());
        QCOMPARE(again.a_DocWires.size(), sch.a_DocWires.size());
        QCOMPARE(again.a_DocDiags.size(), sch.a_DocDiags.size());
    }

    void textDocumentWritesItselfElsewhere()
    {
        TextDoc text(nullptr, QString());
        text.setPlainText("module m(a, b);\nendmodule\n");
        const QString copy = dir.filePath("copy.va");
        QVERIFY(text.writeTo(copy));
        QFile f(copy);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(f.readAll()), QStringLiteral("module m(a, b);\nendmodule\n"));
    }

    // ---- the autosave store ------------------------------------------------
    void namedDocumentRoundTrip()
    {
        const QString path = copyExample("named.sch");
        Schematic sch(nullptr, path);
        QVERIFY(sch.load());
        sch.setChanged(true, false);

        const QString copy = autosave::write(&sch);
        QVERIFY(!copy.isEmpty());
        QVERIFY(QFileInfo::exists(copy));
        QVERIFY(copy.endsWith(".sch"));
        QVERIFY(!QFileInfo::exists(copy + ".part"));      // atomic rename

        const auto pending = autosave::pending();
        QCOMPARE(pending.size(), 1);
        QCOMPARE(pending.first().original, QDir::cleanPath(path));
        QVERIFY(!pending.first().untitled);
        QVERIFY(pending.first().schematic);
        QCOMPARE(pending.first().path, copy);

        // Writing again replaces, not duplicates.
        QCOMPARE(autosave::write(&sch), copy);
        QCOMPARE(autosave::pending().size(), 1);

        autosave::remove(path);
        QVERIFY(autosave::pending().isEmpty());
    }

    void untitledDocumentsAreKeptApart()
    {
        Schematic a(nullptr, QString());
        Schematic b(nullptr, QString());
        TextDoc t(nullptr, QString());
        t.setPlainText("x");
        QVERIFY(!autosave::write(&a, 0).isEmpty());
        QVERIFY(!autosave::write(&b, 1).isEmpty());
        QVERIFY(!autosave::write(&t, 2).isEmpty());
        auto pending = autosave::pending();
        QCOMPARE(pending.size(), 3);
        int text = 0;
        for (const auto& e : pending) { QVERIFY(e.untitled); QVERIFY(e.original.isEmpty()); if (!e.schematic) ++text; }
        QCOMPARE(text, 1);
        autosave::removeUntitled(1, true);
        QCOMPARE(autosave::pending().size(), 2);
        autosave::removeUntitled(2, false);
        QCOMPARE(autosave::pending().size(), 1);
    }

    void orphanedSidecarsAreDropped()
    {
        const QString path = copyExample("orphan.sch");
        Schematic sch(nullptr, path);
        QVERIFY(sch.load());
        const QString copy = autosave::write(&sch);
        QVERIFY(QFile::remove(copy));                     // copy lost, sidecar left
        QVERIFY(autosave::pending().isEmpty());
        QCOMPARE(QDir(autosave::directory()).entryList(QDir::Files).size(), 0);
    }

    // ---- application integration ------------------------------------------
    void applicationAutosavesAndRestores()
    {
        const QString path = copyExample("RCL_resonance.sch");   // name must match its dataset names
        std::size_t componentsAfterEdit = 0;
        {
            QucsApp app(false);
            QucsMain = &app;
            QVERIFY(app.gotoPage(path));
            Schematic* sch = app.currentSchematic();
            QVERIFY(sch != nullptr);
            QCOMPARE(app.autosaveAll(), 0);                 // nothing modified yet

            // Edit: delete the first component.
            sch->a_DocComps.front()->isSelected = true;
            QVERIFY(sch->deleteElements());
            QVERIFY(sch->getDocChanged());
            componentsAfterEdit = sch->a_DocComps.size();

            QCOMPARE(app.autosaveAll(), 1);
            QCOMPARE(autosave::pending().size(), 1);
            QCOMPARE(autosave::pending().first().original, QDir::cleanPath(path));

            // Saving makes the copy obsolete...
            app.slotFileSave();
            QVERIFY(!sch->getDocChanged());
            QVERIFY(autosave::pending().isEmpty());
            // ...and the file on disk carries the edit.
            Schematic check(nullptr, path);
            QVERIFY(check.load());
            QCOMPARE(check.a_DocComps.size(), componentsAfterEdit);

            // Edit again and leave the copy behind, as a crash would.
            sch->a_DocComps.front()->isSelected = true;
            QVERIFY(sch->deleteElements());
            componentsAfterEdit = sch->a_DocComps.size();
            QCOMPARE(app.autosaveAll(true), 1);
            // Discard on close so the app does not prompt; the copy must
            // survive that only if we do not close - so do not close.
            QucsMain = nullptr;
        }
        QCOMPARE(autosave::pending().size(), 1);

        // "Next start": restore without the prompt.
        {
            QucsApp app(false);
            QucsMain = &app;
            app.restoreAutosaved(autosave::pending());
            Schematic* sch = app.currentSchematic();
            QVERIFY(sch != nullptr);
            QCOMPARE(sch->getDocName(), QDir::cleanPath(path));   // identity restored
            QVERIFY(sch->getDocChanged());                        // shown as modified
            QCOMPARE(sch->a_DocComps.size(), componentsAfterEdit); // content from the copy
            // The document on disk still has the older state.
            Schematic disk(nullptr, path);
            QVERIFY(disk.load());
            QCOMPARE(disk.a_DocComps.size(), componentsAfterEdit + 1);
            sch->setDocChanged(false);   // no prompt on teardown
            QucsMain = nullptr;
        }
    }

    // ---- crash handler -------------------------------------------------------
    void sessionMarkerDetectsUncleanExit()
    {
        QVERIFY(!crash::markSessionStart());   // fresh
        crash::markSessionEnd();
        QVERIFY(!crash::markSessionStart());   // clean previous exit
        QVERIFY(crash::markSessionStart());    // no markSessionEnd() in between
        crash::markSessionEnd();
    }

    void crashWritesReportAndRunsEmergencyCallback_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QString>("expected");
        QTest::newRow("segv")  << "--crash-segv"  << "SIGSEGV";
        QTest::newRow("throw") << "--crash-throw" << "boom";
    }

    void crashWritesReportAndRunsEmergencyCallback()
    {
        QFETCH(QString, mode);
        QFETCH(QString, expected);
        const QString reports = dir.filePath("child-reports-" + mode.mid(2));
        const QString marker = dir.filePath("marker-" + mode.mid(2));

        QProcess child;
        child.setProgram(QCoreApplication::applicationFilePath());
        child.setArguments({mode, reports, marker});
        child.start();
        QVERIFY(child.waitForFinished(30000));
        QVERIFY2(child.exitStatus() == QProcess::CrashExit || child.exitCode() != 0,
                 "the child was expected to die");

        const QFileInfoList written = QDir(reports).entryInfoList({"crash-*.txt"}, QDir::Files);
        QCOMPARE(written.size(), 1);
        QFile f(written.first().absoluteFilePath());
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString report = QString::fromUtf8(f.readAll());
        QVERIFY2(report.contains("qucs-s crash report"), qPrintable(report));
        QVERIFY2(report.contains(expected), qPrintable(report));
        QVERIFY2(report.contains("Backtrace"), qPrintable(report));
        QVERIFY2(report.contains("warning: last words"), qPrintable(report));  // ring buffer
#if defined(__APPLE__) || defined(__linux__)
        QVERIFY2(report.contains("qucs_s") || report.contains("test_autosave"), qPrintable(report));
#endif
        QVERIFY2(QFileInfo::exists(marker), "emergency callback did not run");
    }
};

// Child modes crash on purpose after installing the handler.
static int crashChild(const QString& mode, const QString& reports, const QString& marker)
{
    crash::setReportDirectory(reports);
    crash::install([marker] {
        QFile f(marker);
        if (f.open(QIODevice::WriteOnly)) f.write("emergency\n");
    });
    crash::noteMessage(QtWarningMsg, "last words");
    if (mode == "--crash-throw")
        throw std::runtime_error("boom");
    // An unmapped, non-null address: a null dereference is what UBSan's
    // null check reports (and, with halt_on_error, exits on) before the CPU
    // ever faults, so the handler under test would never run.
    volatile int* p = reinterpret_cast<volatile int*>(0x10);
    return *p;   // SIGSEGV
}

int main(int argc, char** argv)
{
    if (argc == 4 && QString(argv[1]).startsWith("--crash-"))
        return crashChild(argv[1], argv[2], argv[3]);
    int one = 1;   // QucsApp opens every argument as a document
    QApplication app(one, argv);
    TestAutosave test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_autosave.moc"
