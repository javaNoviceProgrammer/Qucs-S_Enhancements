/*
 * What a property value says (valuereading.h): a number with its prefix
 * and unit, an expression, a name, a list or text; what the SPICE netlist
 * gets; and a warning where SPICE and Qucs would read two different
 * numbers or a digit follows the prefix ("4k7") - and the line under the
 * component dialog's table that shows it while a value is typed.
 */
#include <QtTest>
#include <QLabel>
#include <QLineEdit>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>

#include "config.h"
#include "qucs.h"
#include "schematic.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "valuereading.h"
#include "components/component.h"
#include "components/componentdialog.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

using qucs_s::units::Reading;

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

QString kindName(Reading::Kind k)
{
    switch (k) {
    case Reading::Empty: return "empty";
    case Reading::Number: return "number";
    case Reading::Expression: return "expression";
    case Reading::Name: return "name";
    case Reading::List: return "list";
    case Reading::Text: return "text";
    }
    return "?";
}

} // namespace

class TestValueReading : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

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

    void aValueIsRead_data()
    {
        QTest::addColumn<QString>("value");
        QTest::addColumn<QString>("kind");
        QTest::addColumn<double>("number");
        QTest::addColumn<QString>("unit");
        QTest::addColumn<QString>("spice");
        QTest::addColumn<bool>("warned");

        QTest::newRow("prefix") << "10k" << "number" << 1e4 << "" << "10K" << false;
        QTest::newRow("prefix and unit") << "10 kOhm" << "number" << 1e4 << "Ohm" << "10K" << false;
        QTest::newRow("nano farad") << "4.7 nF" << "number" << 4.7e-9 << "F" << "4.7N" << false;
        QTest::newRow("mega") << "10M" << "number" << 1e7 << "" << "10MEG" << false;
        QTest::newRow("mega ohm") << "10 MOhm" << "number" << 1e7 << "Ohm" << "10MEG" << false;
        QTest::newRow("SPICE's Meg") << "10Meg" << "number" << 1e7 << "" << "10MEG" << false;
        QTest::newRow("centimetre") << "10 cm" << "number" << 0.1 << "m" << "10E-2" << false;
        QTest::newRow("exponent") << "1e-3" << "number" << 1e-3 << "" << "1E-3" << false;
        QTest::newRow("negative volts") << "-5 V" << "number" << -5.0 << "V" << "-5" << false;
        QTest::newRow("plain") << "26.85" << "number" << 26.85 << "" << "26.85" << false;
        QTest::newRow("dBm") << "0 dBm" << "number" << 0.0 << "dBm" << "0" << false;
        // Where SPICE and Qucs part ways.
        QTest::newRow("ohm in lower case") << "10 Mohm" << "number" << 1e7 << "ohm" << "10MOHM" << true;
        QTest::newRow("meg in lower case") << "10 meg" << "number" << 1e-2 << "" << "10MEG" << true;
        QTest::newRow("K is no prefix to Qucs") << "1 KOhm" << "number" << 1.0 << "KOhm" << "1K" << true;
        QTest::newRow("4k7") << "4k7" << "number" << 4e3 << "" << "4K7" << true;
        QTest::newRow("2R2") << "2R2" << "number" << 2.0 << "" << "2R2" << true;
        // Not numbers.
        QTest::newRow("expression") << "2*R1" << "expression" << 0.0 << "" << "{2*R1}" << false;
        QTest::newRow("braced") << "{x+1}" << "expression" << 0.0 << "" << "{X+1}" << false;
        QTest::newRow("name") << "Rload" << "name" << 0.0 << "" << "{RLOAD}" << false;
        QTest::newRow("model name") << "2N2222" << "text" << 0.0 << "" << "2N2222" << false;
        QTest::newRow("list") << "0 0 1u 5" << "list" << 0.0 << "" << "001U5" << false;
        QTest::newRow("nothing") << "" << "empty" << 0.0 << "" << "" << false;
    }

    void aValueIsRead()
    {
        QFETCH(QString, value);
        QFETCH(QString, kind);
        QFETCH(double, number);
        QFETCH(QString, unit);
        QFETCH(QString, spice);
        QFETCH(bool, warned);
        const Reading r = qucs_s::units::read(value);
        QCOMPARE(kindName(r.kind), kind);
        QCOMPARE(r.spice, spice);
        QCOMPARE(!r.warning.isEmpty(), warned);
        if (r.kind == Reading::Number) {
            QCOMPARE(r.value, number);
            QCOMPARE(r.unit, unit);
        }
    }

    void theWarningSaysWhatEachReads()
    {
        const Reading ohm = qucs_s::units::read("10 Mohm");
        QVERIFY2(ohm.warning.contains("10 m") && ohm.warning.contains("10 M"), qPrintable(ohm.warning));
        const Reading european = qucs_s::units::read("4k7");
        QVERIFY2(european.warning.contains("4.7k"), qPrintable(european.warning));
        // The line for the dialog: the number, the netlist, the warning.
        QCOMPARE(qucs_s::units::read("10 kOhm").describe(true), QStringLiteral("10 kOhm = 10000  →  netlist: 10K"));
        QCOMPARE(qucs_s::units::read("10 kOhm").describe(false), QStringLiteral("10 kOhm = 10000"));
        QVERIFY(qucs_s::units::read("10 Mohm").describe(true).contains("\n⚠ "));
        QCOMPARE(qucs_s::units::read("2N2222").describe(true), QStringLiteral("text"));
    }

    void spiceReadsANumberItsOwnWay()
    {
        bool ok = false;
        QCOMPARE(qucs_s::units::spiceNumber("10MEG", &ok), 1e7);
        QVERIFY(ok);
        QCOMPARE(qucs_s::units::spiceNumber("10M"), 1e-2);
        QCOMPARE(qucs_s::units::spiceNumber("10MOHM"), 1e-2);   // the letters after the factor are ignored
        QCOMPARE(qucs_s::units::spiceNumber("1k"), 1e3);
        QCOMPARE(qucs_s::units::spiceNumber("5U"), 5e-6);
        QCOMPARE(qucs_s::units::spiceNumber("2MIL"), 2 * 25.4e-6);
        QCOMPARE(qucs_s::units::spiceNumber("3.3"), 3.3);
        qucs_s::units::spiceNumber("R1", &ok);
        QVERIFY(!ok);
    }

    void engineeringNotation()
    {
        QCOMPARE(qucs_s::units::engineering(1e4, "Ohm"), QStringLiteral("10 kOhm"));
        QCOMPARE(qucs_s::units::engineering(4.7e-9, "F"), QStringLiteral("4.7 nF"));
        QCOMPARE(qucs_s::units::engineering(26.85), QStringLiteral("26.85"));
        QCOMPARE(qucs_s::units::engineering(5, "V"), QStringLiteral("5 V"));
    }

    // In the component dialog: the line under the table follows the value
    // being typed, the field turns amber on a warning, and the name and
    // the value carry the description as their tooltip.
    void theDialogReadsTheValueBeingTyped()
    {
        const QString sch = dir.filePath("r.sch");
        {
            QFile f(sch);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n"
                    "  <R R1 1 100 100 13 -26 0 1 \"10k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
                    "</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
        }
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(sch, false, false));
        Schematic* doc = app.currentSchematic();
        Component* r1 = doc->a_DocComps.front();
        {   // a child of the schematic: gone before the schematic is
        ComponentDialog dlg(r1, doc);
        QTableWidget* table = dlg.propertiesTable();
        QLabel* reading = dlg.valueReadingLabel();
        QVERIFY(reading != nullptr);

        // The resistance's field.
        int row = -1;
        for (int r = 0; r < table->rowCount(); ++r)
            if (table->item(r, 0) && table->item(r, 0)->text() == "R") row = r;
        QVERIFY(row >= 0);
        auto* edit = qobject_cast<QLineEdit*>(table->indexWidget(table->model()->index(row, 1)));
        QVERIFY(edit != nullptr);
        QCOMPARE(edit->text(), QStringLiteral("10k"));
        QVERIFY(!table->item(row, 0)->toolTip().isEmpty());
        QCOMPARE(table->item(row, 0)->toolTip(), table->item(row, 3)->text());
        const QColor normal = edit->palette().color(QPalette::Text);

        edit->setText("10 Mohm");
        QVERIFY2(reading->text().startsWith("R: "), qPrintable(reading->text()));
        QVERIFY2(reading->text().contains("SPICE reads"), qPrintable(reading->text()));
        QVERIFY(edit->palette().color(QPalette::Text) != normal);   // marked
        QVERIFY(edit->toolTip().contains("SPICE reads"));

        edit->setText("2.2 kOhm");
        QVERIFY2(reading->text().contains("netlist: 2.2K"), qPrintable(reading->text()));
        QVERIFY(!reading->text().contains("⚠"));
        QCOMPARE(edit->palette().color(QPalette::Text), normal);

        // Qucsator has no SPICE netlist to speak of.
        QucsSettings.DefaultSimulator = spicecompat::simQucsator;
        edit->setText("3.3 kOhm");
        QVERIFY(!reading->text().contains("netlist"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        }
        doc->setChanged(false);
        app.closeAllFiles();
    }
};

QTEST_MAIN(TestValueReading)
#include "test_value_reading.moc"
