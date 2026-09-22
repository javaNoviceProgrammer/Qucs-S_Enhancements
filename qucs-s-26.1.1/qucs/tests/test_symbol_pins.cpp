/*
 * The pin of a subcircuit and the net behind it carry one name: the
 * symbol writes beside the pin what the netlist calls that pin of the
 * .SUBCKT, and a net that has no label of its own is named after the
 * port that sits on it.
 */
#include <QtTest>
#include <QTemporaryDir>

#include "config.h"
#include "erc.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "node.h"
#include "schematic.h"
#include "components/component.h"
#include "extsimkernels/ngspice.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

namespace {

// A subcircuit: three ports on three nets, wired through two resistors.
// `labels` gives the label of each of the three wires the ports sit on,
// an empty string for a net that carries none.
QString subcircuit(const QStringList& labels, const QStringList& portNames = {})
{
    auto name = [&](int i) {
        return i < portNames.size() && !portNames.at(i).isEmpty() ? portNames.at(i)
                                                                  : QStringLiteral("P%1").arg(i + 1);
    };
    auto wire = [&](int x1, int y1, int x2, int y2, int i) {
        const QString label = i < labels.size() ? labels.at(i) : QString();
        return label.isEmpty()
                   ? QStringLiteral("  <%1 %2 %3 %4 \"\" 0 0 0 \"\">\n").arg(x1).arg(y1).arg(x2).arg(y2)
                   : QStringLiteral("  <%1 %2 %3 %4 \"%5\" %6 %7 0 \"\">\n")
                         .arg(x1).arg(y1).arg(x2).arg(y2).arg(label).arg(x1 + 20).arg(y1 - 30);
    };

    return "<Qucs Schematic " PACKAGE_VERSION ">\n"
           "<Properties>\n</Properties>\n"
           "<Symbol>\n</Symbol>\n"
           "<Components>\n"
         + QStringLiteral("  <Port %1 1 100 100 -23 12 0 0 \"1\" 1 \"analog\" 0>\n").arg(name(0))
         + QStringLiteral("  <Port %1 1 400 100 4 -42 0 2 \"2\" 1 \"analog\" 0>\n").arg(name(1))
         + QStringLiteral("  <Port %1 1 100 200 -23 12 0 0 \"3\" 1 \"analog\" 0>\n").arg(name(2))
         + "  <R R1 1 250 100 -26 15 0 0 \"1 kOhm\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
           "  <R R2 1 250 200 -26 15 0 0 \"2 kOhm\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
           "</Components>\n"
           "<Wires>\n"
         + wire(100, 100, 220, 100, 0)
         + wire(280, 100, 400, 100, 1)
         + wire(100, 200, 220, 200, 2)
         + "  <280 200 400 200 \"\" 0 0 0 \"\">\n"
           "  <400 100 400 200 \"\" 0 0 0 \"\">\n"
           "</Wires>\n"
           "<Diagrams>\n</Diagrams>\n"
           "<Paintings>\n</Paintings>\n";
}

// A schematic that places one subcircuit and asks for an operating point.
QString parentOf(const QString& subFile)
{
    return "<Qucs Schematic " PACKAGE_VERSION ">\n"
           "<Properties>\n</Properties>\n"
           "<Symbol>\n</Symbol>\n"
           "<Components>\n"
         + QStringLiteral("  <Sub SUB1 1 300 200 -26 40 0 0 \"%1\" 1>\n").arg(subFile)
         + "  <GND * 1 300 300 0 0 0 0>\n"
           "  <.DC DC1 1 500 200 0 40 0 0 \"26.85\" 0 \"0.001\" 0 \"1 pA\" 0 \"1 uV\" 0 \"no\" 0 "
           "\"150\" 0 \"no\" 0 \"none\" 0 \"CroutLU\" 0 \"no\" 0>\n"
           "</Components>\n"
           "<Wires>\n</Wires>\n"
           "<Diagrams>\n</Diagrams>\n"
           "<Paintings>\n</Paintings>\n";
}

// The names beside the pins of the <Symbol> block, in port order.
QStringList symbolPinNames(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};

    QMap<int, QString> byNumber;
    QTextStream stream(&file);
    bool inSymbol = false;
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        if (line == "<Symbol>") { inSymbol = true; continue; }
        if (line == "</Symbol>") break;
        if (!inSymbol || !line.startsWith("<.PortSym ")) continue;

        // <.PortSym cx cy number angle name>
        const QString body = line.mid(1, line.length() - 2);
        byNumber.insert(body.section(' ', 3, 3).toInt(), body.section(' ', 5).trimmed());
    }
    return QStringList(byNumber.begin(), byNumber.end());
}

} // namespace

class TestSymbolPins : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

    QString write(const QString& name, const QString& text)
    {
        const QString path = dir.filePath(name);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) return QString();
        QTextStream(&file) << text;
        return path;
    }

    // Saves the subcircuit (which fills in its symbol) and returns the
    // names its symbol writes beside the pins.
    QStringList pinsOf(const QString& name, const QStringList& labels,
                       const QStringList& portNames = {})
    {
        const QString path = write(name, subcircuit(labels, portNames));
        if (path.isEmpty()) return {};

        auto* doc = new Schematic(nullptr, path);
        if (!doc->load()) { delete doc; return {}; }
        doc->save();
        delete doc;
        return symbolPinNames(path);
    }

    // The .SUBCKT line of the netlist of a schematic that places `subFile`.
    QString subcktLine(const QString& name, const QString& subFile)
    {
        const QString path = write(name, parentOf(subFile));
        if (path.isEmpty()) return QString();

        auto* doc = new Schematic(nullptr, path);
        if (!doc->load()) { delete doc; return QString(); }

        const QString netlist = dir.filePath(name + ".cir");
        Ngspice ngspice(doc);
        ngspice.SaveNetlist(netlist, false);
        delete doc;

        QFile file(netlist);
        if (!file.open(QIODevice::ReadOnly)) return QString();
        QTextStream stream(&file);
        while (!stream.atEnd()) {
            const QString line = stream.readLine();
            if (line.startsWith(".SUBCKT")) return line.simplified();
        }
        return QString();
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
        QucsSettings.QucsWorkDir.setPath(dir.path());
        QucsSettings.S4Qworkdir = dir.filePath("scratch");   // no netlister mkpath warnings
        Module::registerModules();
        QVERIFY(QucsMain == nullptr);   // headless: no dialogs
    }

    // ---- the symbol writes the name of the net ----------------------

    void theSymbolPinTakesTheLabelOfItsNet()
    {
        QCOMPARE(pinsOf("labelled.sch", {"in", "out", "bias"}),
                 (QStringList{"in", "out", "bias"}));
    }

    void aNetWithoutALabelLendsThePortName()
    {
        QCOMPARE(pinsOf("mixed.sch", {"in", "", ""}),
                 (QStringList{"in", "P2", "P3"}));
    }

    void aRenamedPortRenamesItsPin()
    {
        QCOMPARE(pinsOf("renamed.sch", {"", "", ""}, {"vin", "vout", "vbias"}),
                 (QStringList{"vin", "vout", "vbias"}));
    }

    // The label does not have to be on the wire the port touches.
    void aLabelAnywhereOnTheNetNamesThePin()
    {
        // The second port is on the net that the wire 280,200 - 400,200
        // belongs to; label that one instead of the port's own wire.
        QString text = subcircuit({"", "", ""});
        text.replace("  <280 200 400 200 \"\" 0 0 0 \"\">",
                     "  <280 200 400 200 \"far\" 300 170 0 \"\">");
        const QString path = write("faraway.sch", text);
        QVERIFY(!path.isEmpty());

        auto* doc = new Schematic(nullptr, path);
        QVERIFY(doc->load());
        doc->save();
        delete doc;

        // That wire is on the second port's net, through the corner; the
        // label reaches the pin although the port does not touch it.
        QCOMPARE(symbolPinNames(path), (QStringList{"P1", "far", "P3"}));
    }

    // ---- and the netlist calls the pin the same thing ----------------

    void theNetlistNamesThePinsTheSameWay()
    {
        QCOMPARE(pinsOf("netlisted.sch", {"in", "", ""}),
                 (QStringList{"in", "P2", "P3"}));
        QCOMPARE(subcktLine("nettop.sch", "netlisted.sch"),
                 QStringLiteral(".SUBCKT netlisted in P2 P3"));
    }

    void everyNetWithoutALabelReachesTheNetlistNamed()
    {
        QCOMPARE(pinsOf("plain.sch", {"", "", ""}), (QStringList{"P1", "P2", "P3"}));
        QCOMPARE(subcktLine("plaintop.sch", "plain.sch"),
                 QStringLiteral(".SUBCKT plain P1 P2 P3"));
    }

    void renamedPortsReachTheNetlistToo()
    {
        QCOMPARE(pinsOf("named.sch", {"", "", ""}, {"vin", "vout", "vbias"}),
                 (QStringList{"vin", "vout", "vbias"}));
        QCOMPARE(subcktLine("namedtop.sch", "named.sch"),
                 QStringLiteral(".SUBCKT named vin vout vbias"));
    }

    // ---- a name already taken is never borrowed ---------------------

    // Naming the net after the port would join it to the net that
    // already answers to that name, so the port's net keeps a generated
    // name and the check says so.
    void aPortNameThatIsAlreadyALabelIsLeftAlone()
    {
        QString text = subcircuit({"P2", "", ""});   // the FIRST net is labelled P2
        const QString path = write("clash.sch", text);
        QVERIFY(!path.isEmpty());

        const QString parent = write("clashtop.sch", parentOf("clash.sch"));
        QVERIFY(!parent.isEmpty());

        auto* doc = new Schematic(nullptr, parent);
        QVERIFY(doc->load());
        const QString netlist = dir.filePath("clash.cir");
        Ngspice ngspice(doc);
        ngspice.SaveNetlist(netlist, false);
        delete doc;

        QFile file(netlist);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QString subckt;
        QTextStream stream(&file);
        while (!stream.atEnd()) {
            const QString line = stream.readLine();
            if (line.startsWith(".SUBCKT")) { subckt = line.simplified(); break; }
        }
        const QStringList pins = subckt.split(' ');
        QCOMPARE(pins.size(), 5);
        QCOMPARE(pins.at(2), QStringLiteral("P2"));      // the labelled net
        QVERIFY2(pins.at(3) != QStringLiteral("P2"), qPrintable(subckt));  // not joined to it
        QCOMPARE(pins.at(4), QStringLiteral("P3"));

        // And the check warns that this pin will not be called P2.
        auto* sub = new Schematic(nullptr, path);
        QVERIFY(sub->load());
        const QList<qucs_s::erc::Issue> issues = qucs_s::erc::check(sub);
        QStringList messages;
        for (const auto& issue : issues) messages << issue.message;
        QVERIFY2(!messages.filter("generated name").isEmpty(), qPrintable(messages.join(" | ")));
        delete sub;
    }

    // A subcircuit whose ports are all on labelled nets has nothing to
    // say about names.
    void aTidySubcircuitHasNoNameWarning()
    {
        const QString path = write("tidy.sch", subcircuit({"in", "out", "bias"}));
        QVERIFY(!path.isEmpty());

        auto* doc = new Schematic(nullptr, path);
        QVERIFY(doc->load());
        QStringList messages;
        for (const auto& issue : qucs_s::erc::check(doc)) messages << issue.message;
        QVERIFY2(messages.filter("generated name").isEmpty(), qPrintable(messages.join(" | ")));
        delete doc;
    }
};

int main(int argc, char** argv)
{
    int one = 1;
    QApplication app(one, argv);
    TestSymbolPins test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_symbol_pins.moc"
