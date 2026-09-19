/*
 * The schematic loader on damaged input. Found by scripts/ci/fuzz-sch.py;
 * each case is one class of file the loader used to abort on (Debug) or
 * read out of bounds on (Release), or to block on with a modal dialog
 * when there was nobody to click it.
 */
#include <QtTest>
#include <QTemporaryDir>

#include "config.h"
#include "schematic.h"
#include "wire.h"
#include "diagrams/diagram.h"
#include "components/component.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "extsimkernels/spicecompat.h"

class TestLoader : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

    QString write(const QString& name, const QString& text)
    {
        const QString path = dir.filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return {};
        f.write(text.toUtf8());
        return path;
    }

    static QString header()
    {
        return "<Qucs Schematic " PACKAGE_VERSION ">\n"
               "<Properties>\n  <View=0,0,800,600,1,0,0>\n  <Grid=10,10,1>\n"
               "  <DataSet=x.dat>\n  <DataDisplay=x.dpl>\n  <OpenDisplay=0>\n"
               "</Properties>\n<Symbol>\n</Symbol>\n";
    }
    static QString resistor()
    {
        return "  <R R1 1 100 100 15 -26 0 1 \"1 kOhm\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n";
    }

    // Loads and returns the result; the point is that it returns at all.
    bool load(const QString& path, Schematic** out = nullptr)
    {
        auto* sch = new Schematic(nullptr, path);
        const bool ok = sch->load();
        if (out) *out = sch; else delete sch;
        return ok;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        QucsSettings.DefaultSimulator = spicecompat::simNotSpecified;
        QucsSettings.maxUndo = 20;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        QVERIFY(QucsMain == nullptr);   // headless: errors must not open dialogs
    }

    void versionTripletToleratesAnything_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("parsed");
        QTest::newRow("full")      << "26.1.1"      << "26.1.1";
        QTest::newRow("two")       << "26.1"        << "26.1.0";
        QTest::newRow("one")       << "26"          << "26.0.0";
        QTest::newRow("empty")     << ""            << "0.0.0";
        QTest::newRow("garbage")   << "abc"         << "0.0.0";
        QTest::newRow("spaces")    << " 1.2.3 "     << "1.2.3";
        QTest::newRow("too-many")  << "1.2.3.4.5"   << "1.2.3";
        QTest::newRow("negative")  << "-1.-2.-3"    << "0.0.0";
        QTest::newRow("dots")      << "..."         << "0.0.0";
    }

    void versionTripletToleratesAnything()
    {
        QFETCH(QString, text);
        QFETCH(QString, parsed);
        QCOMPARE(VersionTriplet(text).toString(), parsed);
    }

    void shortVersionHeaderDoesNotAbort()
    {
        const QString path = write("short-version.sch",
            "<Qucs Schematic 26.1>\n<Components>\n" + resistor() + "</Components>\n");
        Schematic* sch = nullptr;
        QVERIFY(load(path, &sch));                // 26.1 <= current: opens
        QCOMPARE(sch->a_DocComps.size(), std::size_t(1));
        delete sch;
        load(write("garbage-version.sch",           // reads as 0.0.0: opens too
            "<Qucs Schematic abc>\n<Components>\n</Components>\n"));
    }

    void blankAndBareBracketLinesInsideSections()
    {
        // An empty line, a lone "<" and a lone "</" used to be indexed
        // at [0] and [1] before anyone checked the length.
        for (const char* junk : {"\n", "<\n", "</\n", " \n"}) {
            const QString path = write("junk.sch",
                header() + "<Components>\n" + junk + resistor() + junk + "</Components>\n"
                + "<Wires>\n" + junk + "</Wires>\n<Diagrams>\n" + junk + "</Diagrams>\n"
                + "<Paintings>\n" + junk + "</Paintings>\n");
            load(path);   // any result, no abort
        }
    }

    void unclosedSectionsAreRejectedWithoutBlocking_data()
    {
        QTest::addColumn<QString>("body");
        QTest::newRow("components") << "<Components>\n" + resistor();
        QTest::newRow("wires")      << "<Wires>\n  <100 100 100 200 \"\" 0 0 0 \"\">\n";
        QTest::newRow("diagrams")   << "<Diagrams>\n";
        QTest::newRow("paintings")  << "<Paintings>\n";
        QTest::newRow("properties") << "<Properties>\n  <Grid=10,10,1>\n";
        QTest::newRow("symbol")     << "<Symbol>\n";
    }

    void unclosedSectionsAreRejectedWithoutBlocking()
    {
        QFETCH(QString, body);
        const QString path = write("unclosed.sch", "<Qucs Schematic " PACKAGE_VERSION ">\n" + body);
        QVERIFY(!load(path));   // returns instead of showing a modal error box
    }

    void formatErrorsAreRejectedWithoutBlocking_data()
    {
        QTest::addColumn<QString>("body");
        QTest::newRow("wrong-type")        << "<Nonsense>\n</Nonsense>\n";
        QTest::newRow("unknown-component") << "<Components>\n  <NoSuchDevice X1 1 0 0 0 0 0 0>\n</Components>\n";
        QTest::newRow("bad-component")     << "<Components>\n  <R R1 x>\n</Components>\n";
        QTest::newRow("bad-wire")          << "<Wires>\n  <1 2 3>\n</Wires>\n";
        QTest::newRow("unknown-diagram")   << "<Diagrams>\n  <Nope 0 0 1 1>\n</Diagrams>\n";
        QTest::newRow("bad-diagram")       << "<Diagrams>\n  <Rect x>\n</Diagrams>\n";
        QTest::newRow("marker-no-graph")   << "<Diagrams>\n  <Rect 0 0 100 100 3 #c0c0c0 1 00 1 -40 20 85 0 0 2 10 0 0 0.002 0.01 315 0 225 1 0 0 \"\" \"\" \"\">\n  <Mkr 1 0 0 3 0 0>\n  </Rect>\n</Diagrams>\n";
        QTest::newRow("unknown-painting")  << "<Paintings>\n  <Nope 0 0>\n</Paintings>\n";
        QTest::newRow("painting-delims")   << "<Paintings>\n  Line 0 0 1 1\n</Paintings>\n";
        QTest::newRow("bad-property")      << "<Properties>\n  <Nope=1>\n</Properties>\n";
        QTest::newRow("property-delims")   << "<Properties>\n  Grid=10,10,1\n</Properties>\n";
    }

    void formatErrorsAreRejectedWithoutBlocking()
    {
        QFETCH(QString, body);
        const QString path = write("format.sch", "<Qucs Schematic " PACKAGE_VERSION ">\n" + body);
        load(path);   // rejected or tolerated; never a dialog, never a crash
    }

    // Property values are read with at(0) all over the component classes;
    // an emptied value ("") in a file must not index past the end. Then the
    // line-level damage the fuzzer produces.
    void damagedComponentLinesDoNotCrash_data()
    {
        QTest::addColumn<QString>("line");
        QTest::newRow("mosfet-type")   << "<MOSFET T1 1 0 0 8 -26 0 0 \"\" 0 \"\" 0 \"\" 0>";
        QTest::newRow("mosfet-sub")    << "<_MOSFET T2 1 0 0 8 -26 0 0 \"\" 0 \"\" 0 \"\" 0 \"\" 0>";
        QTest::newRow("jfet-type")     << "<JFET T3 1 0 0 8 -26 0 0 \"\" 0 \"\" 0>";
        QTest::newRow("diode-symbol")  << "<Diode D1 1 0 0 -26 13 0 0 \"1e-15 A\" 0 \"1\" 0 \"10 fF\" 0 \"0.5\" 0 \"0.7 V\" 0 \"0.5\" 0 \"0.0 fF\" 0 \"0.0\" 0 \"2.0\" 0 \"0.0 Ohm\" 0 \"0.0 ps\" 0 \"0\" 0 \"0.0\" 0 \"1.0\" 0 \"1.0\" 0 \"0\" 0 \"1.0\" 0 \"1.0\" 0 \"0\" 0 \"0.0\" 0 \"0.0\" 0 \"3.0\" 0 \"1.11\" 0 \"26.85\" 0 \"3.0\" 0 \"1.0\" 0 \"\" 0 \"\" 0>";
        QTest::newRow("capacitor-sym") << "<C C1 1 0 0 17 -26 0 1 \"1 pF\" 1 \"\" 0 \"\" 0 \"\" 0>";
        QTest::newRow("buf-symbol")    << "<Buf Y1 1 0 0 -26 20 0 0 \"1\" 0 \"1 V\" 0 \"0\" 0 \"\" 0>";
        QTest::newRow("inv-symbol")    << "<Inv Y2 1 0 0 -26 20 0 0 \"1\" 0 \"1 V\" 0 \"0\" 0 \"\" 0>";
        QTest::newRow("subport-dir")   << "<Port P1 1 0 0 -23 12 0 0 \"1\" 1 \"\" 0 \"\" 0 \"\" 0>";
        QTest::newRow("mstee-sym")     << "<MTEE MS1 1 0 0 -26 20 0 0 \"Subst1\" 0 \"1 mm\" 0 \"1 mm\" 0 \"1 mm\" 0 \"\" 0 \"\" 0 \"\" 0>";
        QTest::newRow("mscross-sym")   << "<MCROSS MS2 1 0 0 -26 20 0 0 \"Subst1\" 0 \"1 mm\" 0 \"1 mm\" 0 \"1 mm\" 0 \"1 mm\" 0 \"\" 0 \"\" 0>";
        // The display flag after a value is " 1 " or " 0 "; with one quote
        // missing the sections shift and the flag field can be empty.
        QTest::newRow("unbalanced")    << "<R R1 1 100 100 15 -26 0 1 \"1 kOhm\" 1 \"26.85 0 \"0.0\" 0>";
        QTest::newRow("quote-at-end")  << "<R R1 1 100 100 15 -26 0 1 \"1 kOhm\" 1 \">";
        QTest::newRow("just-brackets") << "<>";
        // The Diode's old-file compatibility shift stepped an iterator
        // before begin() when the line carried no value at all.
        QTest::newRow("diode-no-props")  << "<Diode D1 1 0 0 -26 13 0 0>";
        QTest::newRow("diode-one-quote") << "<Diode D1 1 0 0 -26 13 0 0 \"1e-15 A>";
        QTest::newRow("diode-one-value") << "<Diode D1 1 0 0 -26 13 0 0 \"1e-15 A\" 1>";
        QTest::newRow("and-no-props")    << "<AND Y1 1 0 0 -26 20 0 0>";
        QTest::newRow("buf-no-props")    << "<Buf Y1 1 0 0 -26 20 0 0>";
        // The rotation field was used as a loop bound: 2^31 rotate() calls.
        QTest::newRow("huge-rotation") << "<R R1 1 100 100 15 -26 0 2147483647 \"1 kOhm\" 1>";
        QTest::newRow("neg-rotation")  << "<R R1 1 100 100 15 -26 0 -2147483648 \"1 kOhm\" 1>";
        QTest::newRow("single-char")   << "<";
    }

    void damagedComponentLinesDoNotCrash()
    {
        QFETCH(QString, line);
        const QString path = write("props.sch",
            header() + "<Components>\n  " + line + "\n</Components>\n");
        load(path);   // any result, no abort
    }

    // Coordinates near INT_MAX / INT_MIN made the bounding-box and
    // print-margin arithmetic overflow (Qt 6.11 asserts on it in Debug).
    void absurdCoordinatesAreClamped()
    {
        const QString path = write("coords.sch", header() +
            "<Components>\n"
            "  <R R1 1 2147483647 -2147483648 15 -26 0 1 \"1 kOhm\" 1>\n"
            "  <GND * 1 -2147483648 2147483647 0 0 0 0>\n"
            "</Components>\n<Wires>\n"
            "  <-2147483648 -2147483648 2147483647 2147483647 \"lbl\" 2147483647 -2147483648 2147483647 \"\">\n"
            "</Wires>\n<Diagrams>\n"
            "  <Rect 2147483647 2147483647 2147483647 2147483647 3 #c0c0c0 1 00 1 -40 20 85 0 0 2 10 0 0 0.002 0.01 315 0 225 1 0 0 \"\" \"\" \"\">\n"
            "  </Rect>\n"
            "</Diagrams>\n<Paintings>\n"
            "  <Line -2147483648 2147483647 2147483647 -2147483648 #000000 0 1>\n"
            "  <Rectangle 2147483647 2147483647 2147483647 2147483647 #000000 0 1 #c0c0c0 1 0>\n"
            "  <Text 2147483647 -2147483648 12 #000000 0 \"t\">\n"
            "</Paintings>\n");
        Schematic* sch = nullptr;
        QVERIFY(load(path, &sch));
        for (Component* c : sch->a_DocComps) {
            QVERIFY(std::abs(c->cx) <= misc::MaxCoordinate);
            QVERIFY(std::abs(c->cy) <= misc::MaxCoordinate);
        }
        for (Wire* w : sch->a_DocWires) {
            QVERIFY(std::abs(w->x1) <= misc::MaxCoordinate);
            QVERIFY(std::abs(w->y2) <= misc::MaxCoordinate);
        }
        for (Diagram* d : sch->a_DocDiags) {
            QVERIFY(std::abs(d->cx) <= misc::MaxCoordinate);
            QVERIFY(std::abs(d->x2) <= misc::MaxCoordinate);
        }
        const QRect all = sch->allBoundingRect();      // must not overflow
        QVERIFY(all.width() >= 0 && all.height() >= 0);
        QVERIFY(std::abs(all.left()) <= 2 * misc::MaxCoordinate + 100);
        delete sch;
    }

    // setOnGrid() divides by the grid size.
    void gridSizeNeverReachesZero()
    {
        Schematic* sch = nullptr;
        QVERIFY(load(write("grid0.sch",
            "<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n  <Grid=0,-5,1>\n</Properties>\n"), &sch));
        QCOMPARE(sch->getGridX(), 1);
        QCOMPARE(sch->getGridY(), 1);
        sch->setGridX(0);
        QCOMPARE(sch->getGridX(), 1);
        int x = 17, y = -23;
        sch->setOnGrid(x, y);   // would be SIGFPE with a zero grid
        delete sch;
    }

    void wrongDocumentTypeAndMissingFile()
    {
        QVERIFY(!load(write("not-a-schematic.sch", "<Qucs Dataset 26.1.1>\n")));
        QVERIFY(!load(dir.filePath("does-not-exist.sch")));
        QVERIFY(load(write("empty.sch", "")));    // an empty file is an empty document
    }
};

int main(int argc, char** argv)
{
    int one = 1;
    QApplication app(one, argv);
    TestLoader test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_loader.moc"
