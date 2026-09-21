/*
 * The simulation console (#235): a simulation runs in a dock instead of a
 * modal dialog. The dock appears with the run, the application stays
 * responsive, Stop ends the run, a second run is refused while the first
 * is going, and closing the simulated document takes the run down safely.
 * A script stands in for the simulator so that timing is under control;
 * the last case uses the real ngspice when it is installed.
 */
#include <QtTest>
#include <QTemporaryDir>
#include <QDockWidget>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardPaths>

#include "config.h"
#include "qucs.h"
#include "schematic.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "simulationconsole.h"
#include "extsimkernels/simulationrun.h"
#include "extsimkernels/spicecompat.h"

namespace {
struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

QStringList statusLines(SimulationConsole* c)
{
    QStringList lines;
    for (int i = 0; i < c->statusLog()->count(); ++i) lines << c->statusLog()->item(i)->text();
    return lines;
}

QPushButton* button(QWidget* w, const QString& text)
{
    for (QPushButton* b : w->findChildren<QPushButton*>())
        if (b->text() == text) return b;
    return nullptr;
}
} // namespace

class TestSimulationConsole : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString sch;
    QString fakeSimulator;

    QString write(const QString& path, const QString& text, bool executable = false)
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return {};
        f.write(text.toUtf8());
        f.close();
        if (executable) f.setPermissions(f.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeUser);
        return path;
    }

    // A fresh copy of the example (its dataset is removed with it).
    void resetSchematic()
    {
        QFile::remove(sch);
        QFile::remove(dir.filePath("RCL_resonance.dat.ngspice"));
        QVERIFY(QFile::copy(QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/RF/Miscellaneous/RCL_resonance.sch"), sch));
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.maxUndo = 20;
        QucsSettings.firstRun = false;   // else QucsApp goes looking for ngspice and resets its path
        QucsSettings.XyceExecutable = "xyce";
        QucsSettings.SpiceOpusExecutable = "spiceopus";
        QucsSettings.S4Qworkdir = dir.filePath("work");
        QucsSettings.tempFilesDir.setPath(dir.filePath("work"));
        QVERIFY(QDir().mkpath(dir.filePath("work")));
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        sch = dir.filePath("RCL_resonance.sch");
        // Pretends to simulate for a few seconds and succeeds without output.
        fakeSimulator = write(dir.filePath("slow-ngspice.sh"),
            "#!/bin/sh\necho \"fake ngspice: $*\"\nsleep 3\necho \"done\"\nexit 0\n", true);
    }

    void theDockAppearsWithTheRunAndTheAppStaysResponsive()
    {
        resetSchematic();
        QucsSettings.NgspiceExecutable = fakeSimulator;
        QucsApp app(false);
        MainGuard guard(&app);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        QVERIFY(app.gotoPage(sch));
        SimulationConsole* console = app.simulationConsole();
        QVERIFY(console != nullptr);
        QVERIFY(!console->dock()->isVisible());
        QVERIFY(!console->isRunning());

        QVERIFY(QMetaObject::invokeMethod(&app, "slotSimulateWithSpice"));
        QVERIFY(console->dock()->isVisible());           // brought up with the run
        QVERIFY(console->isRunning());
        QVERIFY(button(console, "Stop")->isEnabled());
        // Not modal: the event loop is ours while the simulator works.
        QTRY_VERIFY(console->console()->toPlainText().contains("fake ngspice"));
        QVERIFY(console->isRunning());
        QVERIFY(app.isEnabled());

        QTRY_VERIFY_WITH_TIMEOUT(!console->isRunning(), 15000);
        QTRY_VERIFY(console->currentRun() == nullptr);   // freed once handled
        const QStringList status = statusLines(console);
        QVERIFY2(status.filter("Simulation started").size() == 1, qPrintable(status.join(" | ")));
        QVERIFY2(status.filter("successful").size() == 1, qPrintable(status.join(" | ")));
        QVERIFY(console->console()->toPlainText().contains("Simulation finished"));
        QVERIFY(!button(console, "Stop")->isEnabled());
        QVERIFY(QFileInfo::exists(dir.filePath("work/log.txt")));   // the log, as before
        // For a look at the dock: QUCS_TEST_GRAB=<dir> saves a picture of it.
        const QString grabDir = qEnvironmentVariable("QUCS_TEST_GRAB");
        if (!grabDir.isEmpty()) {
            console->dock()->resize(900, 260);
            QTest::qWait(50);
            console->dock()->grab().save(grabDir + "/simulation-console.png");
        }
        app.closeAllFiles();
    }

    void aSecondRunIsRefusedWhileOneIsGoing()
    {
        resetSchematic();
        QucsSettings.NgspiceExecutable = fakeSimulator;
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(sch));
        SimulationConsole* console = app.simulationConsole();
        QVERIFY(QMetaObject::invokeMethod(&app, "slotSimulateWithSpice"));
        SimulationRun* first = console->currentRun();
        QVERIFY(first != nullptr);
        QVERIFY(QMetaObject::invokeMethod(&app, "slotSimulateWithSpice"));
        QCOMPARE(console->currentRun(), first);          // still the first run
        QVERIFY(statusLines(console).last().contains("already running"));
        QTRY_VERIFY_WITH_TIMEOUT(!console->isRunning(), 15000);
        QCOMPARE(statusLines(console).filter("Simulation started").size(), 1);
        app.closeAllFiles();
    }

    void stopEndsTheRun()
    {
        resetSchematic();
        QucsSettings.NgspiceExecutable = fakeSimulator;
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(sch));
        SimulationConsole* console = app.simulationConsole();
        QVERIFY(QMetaObject::invokeMethod(&app, "slotSimulateWithSpice"));
        QTRY_VERIFY(console->console()->toPlainText().contains("fake ngspice"));
        button(console, "Stop")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!console->isRunning(), 5000);   // not the 3 s the script takes
        QVERIFY(statusLines(console).join("\n").contains("stopped"));
        QVERIFY(!QFileInfo::exists(dir.filePath("RCL_resonance.dat.ngspice")));
        QTRY_VERIFY(console->currentRun() == nullptr);
        // ...and the console is free for the next one.
        QVERIFY(QMetaObject::invokeMethod(&app, "slotSimulateWithSpice"));
        QVERIFY(console->isRunning());
        button(console, "Stop")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!console->isRunning(), 5000);
        app.closeAllFiles();
    }

    void closingTheDocumentTakesTheRunDown()
    {
        resetSchematic();
        QucsSettings.NgspiceExecutable = fakeSimulator;
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(sch));
        SimulationConsole* console = app.simulationConsole();
        QVERIFY(QMetaObject::invokeMethod(&app, "slotSimulateWithSpice"));
        QTRY_VERIFY(console->console()->toPlainText().contains("fake ngspice"));
        QVERIFY(app.closeAllFiles());                     // the document is unmodified: no prompt
        QVERIFY(console->currentRun() == nullptr || console->currentRun()->schematic() == nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(!console->isRunning(), 5000);
        QTRY_VERIFY(console->currentRun() == nullptr);
        QVERIFY(statusLines(console).join("\n").contains("closed during the simulation"));
        QVERIFY(!QFileInfo::exists(dir.filePath("RCL_resonance.dat.ngspice")));
    }

    void aFailedStartIsReportedAndOver()
    {
        resetSchematic();
        // A file that exists (so the app lists the simulator) but cannot be
        // executed: the process fails to start.
        QucsSettings.NgspiceExecutable = write(dir.filePath("not-a-simulator"), "just text\n", false);
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(sch));
        SimulationConsole* console = app.simulationConsole();
        QVERIFY(QMetaObject::invokeMethod(&app, "slotSimulateWithSpice"));
        QTRY_VERIFY_WITH_TIMEOUT(!console->isRunning(), 5000);
        QVERIFY(statusLines(console).join("\n").contains("Failed to start"));
        QTRY_VERIFY(console->currentRun() == nullptr);
        app.closeAllFiles();
    }

    void theRealSimulatorWritesTheDataset()
    {
        const QString ngspice = QStandardPaths::findExecutable("ngspice");
        if (ngspice.isEmpty()) QSKIP("ngspice is not installed");
        resetSchematic();
        QucsSettings.NgspiceExecutable = ngspice;
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(sch));
        QVERIFY2(QDir(misc::scratchDir()).exists(), qPrintable(misc::scratchDir()));
        QVERIFY2(QFileInfo(QucsSettings.NgspiceExecutable).isExecutable(), qPrintable(QucsSettings.NgspiceExecutable));
        SimulationConsole* console = app.simulationConsole();
        QVERIFY(QMetaObject::invokeMethod(&app, "slotSimulateWithSpice"));
        QTRY_VERIFY_WITH_TIMEOUT(!console->isRunning(), 60000);
        // "successful", or "finished" after warnings ngspice may print.
        QVERIFY2(statusLines(console).join("\n").contains("Now place diagram"),
                 qPrintable(statusLines(console).join(" | ") + "\nconsole: " + console->console()->toPlainText()));
        QTRY_VERIFY(QFileInfo::exists(dir.filePath("RCL_resonance.dat.ngspice")));
        QVERIFY(QFileInfo(dir.filePath("RCL_resonance.dat.ngspice")).size() > 1000);
        app.closeAllFiles();
    }
};

int main(int argc, char** argv)
{
    int one = 1;   // QucsApp opens every argument as a document
    QApplication app(one, argv);
    TestSimulationConsole test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_simulation_console.moc"
