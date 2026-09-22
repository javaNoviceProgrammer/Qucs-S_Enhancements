/*
 * A schematic opens whatever simulator is selected (upstream #1468).
 *
 * Module::registerComponent() used to put a component into the hash the
 * loader reads only when it was meant for the simulator in use, so under
 * ngspice a schematic holding a qucsator-only part did not load headless
 * and in the GUI offered to replace the part by an empty subcircuit - which
 * the next save wrote over the original. Undo after a simulator switch went
 * the same way, since it reloads the document from its own text. Now every
 * component loads and only the palette follows the simulator.
 */
#include <QtTest>
#include <QTemporaryDir>
#include <QDirIterator>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTimer>

#include "config.h"
#include "qucs.h"
#include "schematic.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "erc.h"
#include "components/component.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

const QList<int> kSimulators = {spicecompat::simNgspice, spicecompat::simXyce,
                                spicecompat::simSpiceOpus, spicecompat::simQucsator};

bool forSimulator(const Component* c, int simulator) { return (c->Simulator & simulator) == simulator; }

void useSimulator(int simulator)
{
    QucsSettings.DefaultSimulator = simulator;
    Module::registerModules();   // unregisters first
}

// Everything the palette offers under the simulator in use, instantiated.
QList<Component*> paletteComponents()
{
    QList<Component*> out;
    for (const QString& cat : Category::getCategories()) {
        for (Module* m : Category::getModules(cat)) {
            if (m->info == nullptr) continue;
            QString name;
            char* file = nullptr;
            Element* e = m->info(name, file, true);
            if (auto* c = dynamic_cast<Component*>(e)) out << c;
            else delete e;
        }
    }
    return out;
}

} // namespace

class TestSimulatorModes : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    // A saved line per model, from the palette of every simulator.
    QMap<QString, QString> lines;
    QMap<QString, int> simulatorOf;   // the component's Simulator mask

    // A model offered by one simulator's palette and by no palette of the
    // other: the part a schematic for that simulator cannot do without.
    QString onlyFor(int simulator) const
    {
        for (auto it = simulatorOf.cbegin(); it != simulatorOf.cend(); ++it)
            if (it.value() == simulator && !it.key().startsWith('.')) return it.key();
        return QString();
    }

    // The <Components> section of a schematic file (the header's view
    // depends on whether the document was ever laid out).
    static QByteArray components(const QByteArray& file)
    {
        const int from = file.indexOf("<Components>");
        const int to = file.indexOf("</Components>");
        return from < 0 || to < from ? QByteArray() : file.mid(from, to - from);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.maxUndo = 20;
        QucsSettings.firstRun = false;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        for (int simulator : kSimulators) {
            useSimulator(simulator);
            for (Component* c : paletteComponents()) {
                if (!lines.contains(c->Model)) {
                    lines.insert(c->Model, c->save());
                    simulatorOf.insert(c->Model, c->Simulator);
                }
                delete c;
            }
        }
        QVERIFY(lines.size() > 100);
        QVERIFY2(!onlyFor(spicecompat::simQucsator).isEmpty(), "no qucsator-only component to test with");
        QVERIFY2(!onlyFor(spicecompat::simNgspice).isEmpty(), "no ngspice-only component to test with");
    }

    void cleanupTestCase() { Module::unregisterModules(); }

    // The palette is still the simulator's own.
    void thePaletteOffersTheSimulatorsPartsOnly()
    {
        for (int simulator : kSimulators) {
            useSimulator(simulator);
            QStringList foreign;
            for (Component* c : paletteComponents()) {
                if (!forSimulator(c, simulator)) foreign << c->Model;
                delete c;
            }
            QVERIFY2(foreign.isEmpty(), qPrintable(foreign.join(", ")));
            QVERIFY(!Module::Unlisted.isEmpty());   // there is always another simulator's part
        }
    }

    // Every component of every palette loads under every simulator, and
    // comes back as it was saved.
    void everyComponentLoadsUnderEverySimulator()
    {
        Schematic doc(nullptr, QString());
        for (int simulator : kSimulators) {
            useSimulator(simulator);
            QStringList failed;
            for (auto it = lines.cbegin(); it != lines.cend(); ++it) {
                QString line = it.value();
                Component* c = getComponentFromName(line, &doc);
                if (c == nullptr) {
                    failed << it.key();
                    continue;
                }
                if (c->Model != it.key()) failed << it.key() + " read as " + c->Model;
                else if (c->save() != it.value()) failed << it.key() + ": " + c->save() + " != " + it.value();
                delete c;
            }
            QVERIFY2(failed.isEmpty(), qPrintable(spicecompat::getDefaultSimulatorName(simulator) + ": "
                                                  + failed.join("\n")));
        }
    }

    // Of two classes that share a model name, the one meant for the
    // simulator in use reads it - as when only that one was registered.
    void aSharedModelNameReadsTheSimulatorsOwnClass()
    {
        for (int simulator : kSimulators) {
            useSimulator(simulator);
            QStringList wrong;
            Schematic doc(nullptr, QString());
            for (Component* c : paletteComponents()) {
                QString line = c->save();
                std::unique_ptr<Component> read(getComponentFromName(line, &doc));
                if (!read || !forSimulator(read.get(), simulator)) wrong << c->Model;
                delete c;
            }
            QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(", ")));
        }
    }

    // The case of the report: a schematic saved with a qucsator-only part,
    // opened with ngspice selected, keeps the part; the check names it; the
    // file saved again is the file that was opened.
    void aSchematicForAnotherSimulatorOpensWhole()
    {
        const QString model = onlyFor(spicecompat::simQucsator);
        const QString path = dir.filePath("qucsator.sch");

        useSimulator(spicecompat::simQucsator);
        {
            Schematic doc(nullptr, path);
            QString line = lines.value(model);
            Component* c = getComponentFromName(line, &doc);
            QVERIFY(c != nullptr);
            doc.insertRawComponent(c);
            QVERIFY(doc.save() >= 0);
        }
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray saved = components(file.readAll());
        file.close();
        QVERIFY(saved.contains(model.toUtf8()));

        useSimulator(spicecompat::simNgspice);
        {
            Schematic doc(nullptr, path);
            QVERIFY(doc.load());
            QCOMPARE(doc.a_DocComps.size(), std::size_t(1));
            Component* c = doc.a_DocComps.front();
            QCOMPARE(c->Model, model);
            QVERIFY(!forSimulator(c, spicecompat::simNgspice));

            bool named = false;
            for (const auto& issue : qucs_s::erc::check(&doc))
                named = named || (issue.severity == qucs_s::erc::Severity::Error && issue.component == c->Name
                                  && issue.message.contains("not available for"));
            QVERIFY2(named, "the check does not report the part the simulator cannot take");

            QVERIFY(doc.save() >= 0);
        }
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(components(file.readAll()), saved);
    }

    // Every shipped example opens under every simulator: before, under
    // ngspice the qucsator examples did not, and under qucsator the SPICE
    // ones did not.
    void everyExampleOpensUnderEverySimulator()
    {
        QStringList files;
        QDirIterator it(QStringLiteral(QUCS_EXAMPLES_DIR), {"*.sch"}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) files << it.next();
        files.sort();
        QVERIFY(files.size() > 200);
        // Its "tunnel" device exists only once the user has loaded the
        // module through Tools > Load Verilog-A module (tunnel_*.json).
        files.removeAll(QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/OpenVAF/Tunnel_Ngspice_prj/tunn.sch"));
        for (int simulator : kSimulators) {
            useSimulator(simulator);
            QStringList failed;
            for (const QString& path : files) {
                Schematic doc(nullptr, path);
                if (!doc.load()) failed << QDir(QStringLiteral(QUCS_EXAMPLES_DIR)).relativeFilePath(path);
            }
            QVERIFY2(failed.isEmpty(), qPrintable(spicecompat::getDefaultSimulatorName(simulator) + ": "
                                                  + failed.join("\n")));
        }
    }

    // In the application the loader asked, for each part it did not know,
    // whether to put an empty subcircuit in its place. A qucsator example
    // opened with ngspice selected now just opens.
    void theApplicationOpensItWithoutAsking()
    {
        useSimulator(spicecompat::simNgspice);
        QucsApp app(false);
        MainGuard guard(&app);
        QStringList asked;
        QTimer watch;
        connect(&watch, &QTimer::timeout, [&asked] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                asked << box->text();
                box->done(QMessageBox::No);
            }
        });
        watch.start(20);
        const QString path = QStringLiteral(QUCS_EXAMPLES_DIR "/qucsator/RF/Transmission Lines/microstrip.sch");
        QVERIFY(app.gotoPage(path));
        watch.stop();
        QVERIFY2(asked.isEmpty(), qPrintable(asked.join("\n")));

        Schematic* doc = app.currentSchematic();
        QVERIFY(doc != nullptr);
        QCOMPARE(QFileInfo(doc->getDocName()).fileName(), QStringLiteral("microstrip.sch"));
        const auto tee = std::find_if(doc->a_DocComps.begin(), doc->a_DocComps.end(),
                                      [](const Component* c) { return c->Model == QLatin1String("MTEE"); });
        QVERIFY(tee != doc->a_DocComps.end());
        QVERIFY(!forSimulator(*tee, spicecompat::simNgspice));   // drawn in grey, named by the check
        app.closeAllFiles();
    }

    // Undo rebuilds the document from its own text: after a switch of the
    // simulator the parts of the other one must survive it.
    void undoAfterASimulatorSwitchKeepsTheParts()
    {
        const QString model = onlyFor(spicecompat::simNgspice);
        useSimulator(spicecompat::simNgspice);
        Schematic doc(nullptr, QString());
        QString line = lines.value(model);
        Component* c = getComponentFromName(line, &doc);
        QVERIFY(c != nullptr);
        doc.insertRawComponent(c);
        doc.setChanged(true, true);   // the state to go back to

        useSimulator(spicecompat::simQucsator);
        doc.a_DocComps.front()->isSelected = true;
        QVERIFY(doc.deleteElements());   // records the undo entry itself
        QCOMPARE(doc.a_DocComps.size(), std::size_t(0));

        QVERIFY(doc.undo());
        QCOMPARE(doc.a_DocComps.size(), std::size_t(1));
        QCOMPARE(doc.a_DocComps.front()->Model, model);
    }
};

QTEST_MAIN(TestSimulatorModes)
#include "test_simulator_modes.moc"
