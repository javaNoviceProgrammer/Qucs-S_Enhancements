/*
 * The electrical rule check (erc.h): unconnected pins and wire ends,
 * duplicate names, no ground, no simulation - and the Problems tab that
 * lists them, with a click that shows the place.
 */
#include <QtTest>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QDirIterator>
#include <QLabel>
#include <QTimer>
#include <QRegularExpression>

#include "config.h"
#include "qucs.h"
#include "schematic.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "erc.h"
#include "messagedock.h"
#include "simulationconsole.h"
#include "components/component.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

using namespace qucs_s::erc;

namespace {
struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

QStringList messages(const QList<Issue>& issues)
{
    QStringList out;
    for (const Issue& i : issues) out << (i.severity == Severity::Error ? "E " : "W ") + i.message;
    return out;
}

QAction* menuAction(QucsApp* app, const QString& menuTitle, const QString& text)
{
    for (QAction* m : app->menuBar()->actions()) {
        if (m->text().remove('&') != menuTitle || m->menu() == nullptr) continue;
        for (QAction* a : m->menu()->actions())
            if (a->text().remove('&') == text) return a;
    }
    return nullptr;
}
} // namespace

class TestErc : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString broken, fine;

    static void write(const QString& path, const QByteArray& bytes)
    {
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) f.write(bytes);
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
        // R1 twice; R2 with pin 2 open; a wire with a loose end at (500,300)
        // and one whose loose end carries a label (fine); no ground, no
        // simulation block.
        broken = dir.filePath("broken.sch");
        write(broken,
            "<Qucs Schematic " PACKAGE_VERSION ">\n"
            "<Components>\n"
            "  <R R1 1 100 100 15 -26 0 1 \"30\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
            "  <R R1 1 200 100 15 -26 0 1 \"30\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
            "  <R R2 1 300 100 15 -26 0 1 \"30\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
            "</Components>\n"
            "<Wires>\n"
            "  <100 70 200 70 \"\" 0 0 0 \"\">\n"      // R1 top to R1' top
            "  <100 130 200 130 \"\" 0 0 0 \"\">\n"    // their bottoms
            "  <200 130 300 130 \"\" 0 0 0 \"\">\n"    // on to R2's pin 2? no: R2's bottom is at (300,130)
            "  <500 200 500 300 \"\" 0 0 0 \"\">\n"    // loose end at (500,300), other end loose too
            "  <600 200 600 300 \"net1\" 620 250 50 \"\">\n"   // named stub: not a problem
            "</Wires>\n"
            "<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
        fine = dir.filePath("RCL_resonance.sch");
        QVERIFY(QFile::copy(QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/RF/Miscellaneous/RCL_resonance.sch"), fine));
    }

    void theChecksFindEachKindOfProblem()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(broken, false, false));
        Schematic* doc = app.currentSchematic();
        QVERIFY(doc != nullptr);
        const QList<Issue> issues = check(doc);
        const QStringList got = messages(issues);
        QVERIFY2(got.contains("E R1: the name is used twice (also at 100, 100)"), qPrintable(got.join(" | ")));
        QVERIFY2(got.contains("E no ground: the circuit has no reference node"), qPrintable(got.join(" | ")));
        QCOMPARE(got.filter("W R2: pin ").size(), 1);   // one open pin (the other is wired)
        QVERIFY2(got.contains("W the wire end at 500, 200 is connected to nothing"), qPrintable(got.join(" | ")));
        QVERIFY2(got.contains("W the wire end at 500, 300 is connected to nothing"), qPrintable(got.join(" | ")));
        QVERIFY2(got.contains("W no simulation: no .AC, .TR, .DC, .SP, ... block"), qPrintable(got.join(" | ")));
        // The named stub and the wired pins are fine.
        QVERIFY2(!got.join("|").contains("600,"), qPrintable(got.join(" | ")));
        QVERIFY2(got.filter("W R1: pin ").isEmpty(), qPrintable(got.join(" | ")));
        QCOMPARE(errorCount(issues), 2);
        // Errors first.
        QCOMPARE(issues.first().severity, Severity::Error);
        QCOMPARE(issues.last().severity, Severity::Warning);
        // The duplicate points at the second R1, and names it.
        for (const Issue& i : issues)
            if (i.message.startsWith("R1: the name")) { QCOMPARE(i.where, QPoint(200, 100)); QCOMPARE(i.component, QString("R1")); }
    }

    void aGoodCircuitHasNoProblems()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(fine));
        Schematic* doc = app.currentSchematic();
        QVERIFY(doc != nullptr);
        const QList<Issue> issues = check(doc);
        QVERIFY2(issues.isEmpty(), qPrintable(messages(issues).join(" | ")));
        QCOMPARE(check(nullptr).size(), 0);
    }

    void theMenuActionListsThemAndAClickShowsThePlace()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(1000, 700);
        app.show();
        QVERIFY(app.gotoPage(broken, false, false));
        Schematic* doc = app.currentSchematic();
        QVERIFY(doc != nullptr);
        MessageDock* dock = app.messages();
        QVERIFY(!dock->msgDock->isVisible());

        QAction* action = menuAction(&app, "Simulation", "Check Schematic");
        QVERIFY(action != nullptr);
        QCOMPARE(action->shortcut(), QKeySequence(Qt::Key_F10));
        action->trigger();
        QVERIFY(dock->msgDock->isVisible());                      // brought up
        QCOMPARE(dock->builderTabs->currentWidget(), dock->problems);
        QCOMPARE(dock->problems->count(), dock->issues().size());
        QVERIFY(dock->problems->count() >= 6);
        QVERIFY(dock->builderTabs->tabText(2).startsWith("Problems ("));
        QCOMPARE(dock->problemsDocument(), doc);

        // Click the duplicate-name row: the second R1 is selected, the
        // view centred on it.
        int row = -1;
        for (int i = 0; i < dock->issues().size(); ++i)
            if (dock->issues().at(i).message.startsWith("R1: the name")) row = i;
        QVERIFY(row >= 0);
        doc->showAll();
        dock->problems->setCurrentRow(row);
        emit dock->problems->itemClicked(dock->problems->item(row));
        int selected = 0;
        Component* chosen = nullptr;
        for (Component* c : doc->a_DocComps) if (c->isSelected) { ++selected; chosen = c; }
        QCOMPARE(selected, 1);
        QCOMPARE(QPoint(chosen->cx, chosen->cy), QPoint(200, 100));
        const QPoint centre = doc->viewportToModel(doc->viewport()->rect().center());
        QVERIFY2((centre - QPoint(200, 100)).manhattanLength() < 8, qPrintable(QString("%1,%2").arg(centre.x()).arg(centre.y())));

        // A wire-end row selects nothing and centres on the end.
        for (int i = 0; i < dock->issues().size(); ++i)
            if (dock->issues().at(i).message.contains("500, 300")) row = i;
        dock->problems->setCurrentRow(row);
        emit dock->problems->itemClicked(dock->problems->item(row));
        selected = 0;
        for (Component* c : doc->a_DocComps) if (c->isSelected) ++selected;
        QCOMPARE(selected, 0);
        const QPoint centre2 = doc->viewportToModel(doc->viewport()->rect().center());
        QVERIFY2((centre2 - QPoint(500, 300)).manhattanLength() < 8, qPrintable(QString("%1,%2").arg(centre2.x()).arg(centre2.y())));

        // A clean schematic: the tab says so, no error icon.
        QVERIFY(app.gotoPage(fine));
        menuAction(&app, "Simulation", "Check Schematic")->trigger();
        QCOMPARE(dock->issues().size(), 0);
        QCOMPARE(dock->problems->count(), 1);
        QCOMPARE(dock->problems->item(0)->text(), QString("No problems found."));
        QCOMPARE(dock->builderTabs->tabText(2), QString("Problems"));
    }

    // QUCS_ERC_SURVEY=1: run the check over every shipped example and
    // print what it says (to see that it does not cry wolf).
    void surveyOfTheExamples()
    {
        if (qEnvironmentVariableIsEmpty("QUCS_ERC_SURVEY")) QSKIP("set QUCS_ERC_SURVEY=1 for the survey");
        // The library components of the examples need the shipped library.
        QucsSettings.LibDir = QFileInfo(QStringLiteral(QUCS_EXAMPLES_DIR)).dir().filePath("library") + "/";
        QucsApp app(false);
        MainGuard guard(&app);
        QDirIterator it(QStringLiteral(QUCS_EXAMPLES_DIR), {"*.sch"}, QDir::Files, QDirIterator::Subdirectories);
        int files = 0, withErrors = 0, withWarnings = 0;
        QHash<QString, int> kinds;
        while (it.hasNext()) {
            const QString f = it.next();
            if (qEnvironmentVariableIsSet("QUCS_ERC_SURVEY_TRACE")) qWarning() << "loading" << f;
            QTimer::singleShot(3000, &app, [] {
                for (QWidget* w : QApplication::topLevelWidgets())
                    if (w->isVisible() && w->isModal()) {
                        qWarning() << "MODAL:" << w->metaObject()->className() << w->windowTitle();
                        for (QLabel* l : w->findChildren<QLabel*>()) qWarning() << "  text:" << l->text().left(200);
                        w->close();
                    }
            });
            if (!app.gotoPage(f, false, false)) continue;
            ++files;
            const QList<Issue> issues = check(app.currentSchematic());
            if (errorCount(issues) > 0) ++withErrors;
            if (issues.size() > errorCount(issues)) ++withWarnings;
            for (const Issue& i : issues) {
                QString kind = i.message;
                kind.replace(QRegularExpression("[0-9]+"), "N");
                kind.replace(QRegularExpression("^[A-Za-z_]+N?:"), "X:");
                ++kinds[(i.severity == Severity::Error ? "E " : "W ") + kind];
            }
            if (!issues.isEmpty()) qWarning() << f.mid(QString(QUCS_EXAMPLES_DIR).size()) << issues.size() << messages(issues).mid(0, 4);
            app.closeAllFiles();
        }
        qWarning() << "files" << files << "with errors" << withErrors << "with warnings" << withWarnings;
        for (auto k = kinds.begin(); k != kinds.end(); ++k) qWarning() << k.value() << k.key();
    }

    void aSimulationRunsTheCheckFirst()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        app.show();
        QVERIFY(app.gotoPage(broken, false, false));
        MessageDock* dock = app.messages();
        QVERIFY(!dock->msgDock->isVisible());
        // The simulator is a shell that exits at once; the check comes
        // before it and, with errors, brings the Problems tab up.
        QVERIFY(QMetaObject::invokeMethod(&app, "slotSimulateWithSpice"));
        QVERIFY(dock->msgDock->isVisible());
        QCOMPARE(dock->builderTabs->currentWidget(), dock->problems);
        QVERIFY(errorCount(dock->issues()) >= 2);
        QTRY_VERIFY_WITH_TIMEOUT(!app.simulationConsole()->isRunning(), 15000);
    }
};

int main(int argc, char** argv)
{
    int one = 1;   // QucsApp opens every argument as a document
    QApplication app(one, argv);
    TestErc test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_erc.moc"
