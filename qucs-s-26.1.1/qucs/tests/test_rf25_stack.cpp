/*
 * test_rf25_stack.cpp - an RF 2.5D model's stack written as an HFSS™ 3D Layout
 * XML stackup and as an EMX process file, and read back from either
 * (rf25/rf25_stackfile.h).
 *
 * Neither Electronics Desktop nor EMX runs here. What each file is was read
 * from each format's documentation; the process files below are written
 * for this test.
 * QUCS_EMX_SAMPLES may name a folder of real process files, which are read
 * where it is set (none is kept in this repository).
 */
#include <QtTest>

#include "rf25/rf25_hfss.h"
#include "rf25/rf25_model.h"
#include "rf25/rf25_stackfile.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QXmlStreamReader>

#include <algorithm>

#include <cmath>

using namespace qucs_s::rf25;

namespace {

Model modelOf(const QString& children)
{
    const QString json = QStringLiteral("{\"format\": \"qucs-s rf 2.5d 1\", \"tree\": {\"type\": \"rf_model\", \"children\": [%1]}}").arg(children);
    QString why;
    const std::optional<Model> m = Model::fromJson(json.toUtf8(), &why);
    if (!m) qFatal("the test's model does not read: %s", qPrintable(why));
    return *m;
}

/// A two-metal process: a plane, silicon, an oxide, M1, an oxide with a via
/// up to M2, M2 and a passivation; its layer map M1 1/0 and 1/20, M2 2/0, V1 3/0.
QString twoMetals(const QString& ground = QStringLiteral(R"({"type": "rf_ground", "tag": "g", "label": "GND", "properties": {"kind": "perfect"}})"),
                  const QString& cover = {})
{
    return QStringLiteral(R"({"type": "rf_stack", "tag": "stack", "children": [
        %1,
        {"type": "rf_substrate", "tag": "s", "label": "Si", "properties": {"thickness": "300[um]", "epsr": "11.9", "tand": "0.001", "sigma": "10"}},
        {"type": "rf_dielectric", "tag": "d1", "label": "Ox1", "properties": {"thickness": "2[um]", "epsr": "4.1", "tand": "0.002"}},
        {"type": "rf_metal", "tag": "m1", "label": "M1", "properties": {"thickness": "0.5[um]", "sigma": "3.3e7"}},
        {"type": "rf_dielectric", "tag": "d2", "label": "Ox2", "properties": {"thickness": "1[um]", "epsr": "4.2", "tand": "0"}},
        {"type": "rf_via", "tag": "v1", "label": "V1", "properties": {"from": "M1", "to": "M2", "sigma": "2e7"}},
        {"type": "rf_metal", "tag": "m2", "label": "M2", "properties": {"thickness": "2[um]", "sigma": "4e7"}},
        {"type": "rf_dielectric", "tag": "d3", "label": "Pass", "properties": {"thickness": "1.5[um]", "epsr": "7", "tand": "0.01"}}%2]},
      {"type": "rf_layout", "tag": "layout", "children": [
        {"type": "rf_layer_map", "tag": "layer_map", "properties": {"rows": [["1", "0", "M1"], ["1", "20", "M1"], ["2", "", "M2"], ["3", "0", "V1"], ["9", "0", ""]]}}]},
      {"type": "rf_ports", "tag": "ports", "children": [
        {"type": "rf_internal_port", "tag": "p1", "label": "P1", "properties": {"from": "ground", "to": "M2"}}]},
      {"type": "rf_study", "tag": "study", "properties": {"kind": "full_wave", "scale": "linear", "from": "2[GHz]", "to": "20[GHz]", "points": "10"}})")
        .arg(ground, cover.isEmpty() ? QString() : QStringLiteral(",\n        ") + cover);
}

/// The stack as one line a layer, for comparing two: kind, name, thickness
/// in nm, and what it is made of.
QStringList said(const Stack& stack)
{
    QStringList out;
    for (const Layer& l : stack.layers) {
        QString line;
        switch (l.kind) {
        case Layer::Ground: line = QStringLiteral("ground %1 %2").arg(l.variant).arg(l.variant == QLatin1String("metal") ? l.sigma : 0); break;
        case Layer::Substrate: line = QStringLiteral("substrate %1 %2 %3 %4").arg(l.epsr).arg(l.tand).arg(l.sigma).arg(std::lround(l.thickness * 1e9)); break;
        case Layer::Dielectric: line = QStringLiteral("dielectric %1 %2 %3").arg(l.epsr).arg(l.tand).arg(std::lround(l.thickness * 1e9)); break;
        case Layer::Metal: line = QStringLiteral("metal %1 %2 %3").arg(l.name).arg(l.sigma).arg(std::lround(l.thickness * 1e9)); break;
        case Layer::Via: line = QStringLiteral("via %1 %2 %3 %4").arg(l.name, l.from, l.to).arg(l.sigma); break;
        case Layer::Cover: line = QStringLiteral("cover %1 %2").arg(l.variant).arg(std::lround(l.thickness * 1e9)); break;
        }
        out << line;
    }
    return out;
}

Model readBack(const QString& text, StackFormat format, StackRead* read = nullptr, const Model& into = Model::blank())
{
    QString why;
    const std::optional<StackRead> r = stackTextRead(text.toUtf8(), format, QStringLiteral("test"), &why);
    if (!r) qFatal("it does not read back: %s", qPrintable(why));
    if (read != nullptr) *read = *r;
    return withStackOf(into, *r);
}

QStringList rowsOf(const Model& m)
{
    QStringList out;
    for (const QJsonValue& v : m.layout().child(QStringLiteral("rf_layer_map"))->props.value(QLatin1String("rows")).toArray()) {
        const QJsonArray row = v.toArray();
        out << QStringLiteral("%1/%2 %3").arg(row.at(0).toVariant().toString(), row.at(1).toVariant().toString(), row.at(2).toString());
    }
    out.sort();
    return out;
}

/// The XML's layers: "dielectric name material thickness" top first, then
/// "conductor name elevation thickness material fill", then the vias.
QStringList xmlLayers(const QString& text, QHash<QString, double>* sigmaOf = nullptr)
{
    QStringList out;
    QXmlStreamReader x(text);
    QStringList within;
    QString material, property;
    while (!x.atEnd()) {
        x.readNext();
        if (x.isStartElement()) {
            const QString name = x.name().toString();
            const QXmlStreamAttributes a = x.attributes();
            const QString parent = within.value(within.size() - 1);
            within << name;
            if (name == QLatin1String("Material")) material = a.value(QLatin1String("Name")).toString();
            if (name == QLatin1String("Conductivity")) property = name;
            if (name == QLatin1String("Double") && property == QLatin1String("Conductivity") && sigmaOf != nullptr)
                sigmaOf->insert(material, x.readElementText().toDouble()), within.removeLast(), property.clear();
            if (name != QLatin1String("Layer")) continue;
            if (parent == QLatin1String("Dielectrics"))
                out << QStringLiteral("dielectric %1 %2 %3").arg(a.value(QLatin1String("Name")), a.value(QLatin1String("Material")), a.value(QLatin1String("Thickness")));
            else if (parent == QLatin1String("Layers"))
                out << QStringLiteral("conductor %1 %2 %3 %4 %5").arg(a.value(QLatin1String("Name")), a.value(QLatin1String("Elevation")), a.value(QLatin1String("Thickness")),
                                                                    a.value(QLatin1String("Material")), a.value(QLatin1String("FillMaterial")));
            else if (parent == QLatin1String("Vias"))
                out << QStringLiteral("via %1 %2 %3 %4").arg(a.value(QLatin1String("Name")), a.value(QLatin1String("StartLayer")), a.value(QLatin1String("StopLayer")),
                                                           a.value(QLatin1String("Material")));
        } else if (x.isEndElement() && !within.isEmpty()) {
            within.removeLast();
        }
    }
    if (x.hasError()) qFatal("the stackup is no XML: %s", qPrintable(x.errorString()));
    return out;
}

/// A process file of this test's own, with what the format's grammar has:
/// units by statement and by context, sheet resistance and resistivity,
/// offsets and a position, a conductor through two layers, a region by
/// difference, a via to the backside, attributes.
const char* const kProcess = R"(# A test's process
assume microns
assume ohm-cm
define nwell = L5T0
define poly = L7T0+L7T1
define met1 = L9T0
define met2 = grow(L11T0, 0.02um)
define met3 = L13T0
define cont = L8T0-poly
define pcont = L8T0*poly
define v12 = L10T0
define v23 = L12T0
define tsv = L20T0
layer 400 11.9 1 20 name bulk          # 20 ohm-cm: 5 S/m
layer 6 4.0 tan delta 0.001
    conductor 0.25 80 ohm/sq nwell
    offset 0.5
    conductor 0.2 4e4 S/m poly color red
    offset 1
    conductor 0.6 3e7 S/m met1
    position 3.5
    conductor 0.9 3e7 S/m met2 bias 0.01 um
layer 1.5 3.8
    conductor 2 3.5e7 S/m met3 name top
layer 1 6.5 tan delta 0.003
layer infinity 1
via met1 nwell 2e5 S/m cont       # (the upper first: from the lower all the same)
via poly met1 2e5 S/m pcont
via met1 met2 1.5e7 S/m v12
via met2 top 1.5e7 S/m v23
via nwell backside 1e7 S/m tsv
capacitor met1 met2 met1*met2
)";

} // namespace

class TestRf25Stack : public QObject
{
    Q_OBJECT

private slots:
    void aStackIsWrittenAsAnHfssStackup();
    void anHfssStackupReadsBackTheSameStack();
    void groundsAndLidsGoThereAndBack();
    void aStackIsWrittenAsAnEmxProcess();
    void anEmxProcessReadsBackTheSameStack();
    void anEmxProcessIsReadAsItsGrammarSays();
    void variationsUnitsFlipsAndHiddenParts();
    void aStackupOfAnotherHandReads();
    void theControlFileImportKeepsEachPermittivity();
    void whatCannotBeReadIsRefused();
    void aFileCutAnywhereNeverFaults();
    void theStackReplacesTheModelsAlone();
    void whatNeitherFileSaysOfAMetalIsKeptByName();
    void realProcessFilesRead();
};

void TestRf25Stack::aStackIsWrittenAsAnHfssStackup()
{
    const Model m = modelOf(twoMetals());
    const StackWritten w = hfssStackupOf(m, QStringLiteral("two"), QStringLiteral("the test"));
    QVERIFY2(w.ok(), qPrintable(w.problems.join(QLatin1Char('\n'))));
    QVERIFY(w.text.startsWith(QLatin1String("<?xml")));
    QVERIFY(w.text.contains(QLatin1String("<c:Control xmlns:c=\"http://www.ansys.com/control\" schemaVersion=\"1.0\">")));
    QVERIFY(w.text.contains(QLatin1String("two")) && w.text.contains(QLatin1String("the test")));
    QHash<QString, double> sigma;
    const QStringList layers = xmlLayers(w.text, &sigma);
    // Top first: the passivation, M2's room of it, the oxide, M1's room of the
    // oxide above it, the first oxide, the silicon; the conductors with their
    // elevations in µm; the via from M2 (StartLayer, the upper) down to M1.
    QCOMPARE(layers, (QStringList{
        QStringLiteral("dielectric Pass rf25_Pass 1.5"), QStringLiteral("dielectric M2_fill rf25_Pass 2"),
        QStringLiteral("dielectric Ox2 rf25_Ox2 1"), QStringLiteral("dielectric M1_fill rf25_Ox2 0.5"),
        QStringLiteral("dielectric Ox1 rf25_Ox1 2"), QStringLiteral("dielectric Si rf25_Si 300"),
        QStringLiteral("conductor M2 303.5 2 rf25_M2 rf25_Pass"), QStringLiteral("conductor M1 302 0.5 rf25_M1 rf25_Ox2"),
        QStringLiteral("conductor GND 0 0 rf25_GND rf25_air"), QStringLiteral("via V1 M2 M1 rf25_V1")}));
    QCOMPARE(sigma.value(QStringLiteral("rf25_GND")), 1e30);
    QCOMPARE(sigma.value(QStringLiteral("rf25_Si")), 10.0);
    QCOMPARE(sigma.value(QStringLiteral("rf25_M2")), 4e7);
    QCOMPARE(sigma.value(QStringLiteral("rf25_V1")), 2e7);
    QCOMPARE(w.vias, 1);
    QCOMPARE(w.mapped, 4);
    QVERIFY(w.text.contains(QLatin1String("<DielectricLossTangent><Double>0.01</Double>")));

    // A stack that cannot be read is not written.
    Model broken = m;
    broken.find(QStringLiteral("m1"))->props.insert(QStringLiteral("thickness"), QStringLiteral("nonsense"));
    QVERIFY(!hfssStackupOf(broken).ok());
    QVERIFY(!emxProcessOf(broken).ok());
}

void TestRf25Stack::anHfssStackupReadsBackTheSameStack()
{
    const Model m = modelOf(twoMetals());
    StackRead read;
    const Model back = readBack(hfssStackupOf(m).text, StackFormat::Hfss, &read);
    QCOMPARE(said(back.stack()), said(m.stack()));
    QVERIFY2(back.stack().ok(), qPrintable(back.stack().problems.join(QLatin1Char('\n'))));
    QCOMPARE(read.vias, 1);
    // Its names are its layers', not GDSII numbers: no layer map comes of it.
    QVERIFY(read.rows.isEmpty());
    QVERIFY(read.notes.join(QLatin1Char(' ')).contains(QLatin1String("at the bottom of every dielectric")));
}

void TestRf25Stack::groundsAndLidsGoThereAndBack()
{
    const QString metal = QStringLiteral(R"({"type": "rf_ground", "tag": "g", "label": "Plane", "properties": {"kind": "metal", "sigma": "5.8e7"}})");
    const QString open = QStringLiteral(R"({"type": "rf_ground", "tag": "g", "label": "GND", "properties": {"kind": "open"}})");
    const QString lid = QStringLiteral(R"({"type": "rf_cover", "tag": "c", "label": "Lid", "properties": {"kind": "perfect", "height": "100[um]"}})");
    for (const QString& ground : {metal, open}) {
        const Model m = modelOf(twoMetals(ground, lid));
        for (const StackFormat f : {StackFormat::Hfss, StackFormat::Emx}) {
            const StackWritten w = f == StackFormat::Hfss ? hfssStackupOf(m) : emxProcessOf(m);
            QVERIFY(w.ok());
            QStringList expected = said(m.stack());
            // EMX's plane is perfect, whatever the model's was.
            if (f == StackFormat::Emx && ground == metal) expected.first() = QStringLiteral("ground perfect 0");
            const Model back = readBack(w.text, f);
            QStringList got = said(back.stack());
            QCOMPARE(got, expected);
        }
    }
    // The metal plane in HFSS: five skin depths at the bottom of the sweep.
    const StackWritten w = hfssStackupOf(modelOf(twoMetals(metal)));
    const double delta = std::sqrt(1 / (M_PI * 2e9 * 4e-7 * M_PI * 5.8e7));
    QVERIFY(w.text.contains(QStringLiteral("Name=\"Plane\" Thickness=\"%1\"").arg(QString::number(5 * delta * 1e6, 'g', 12))));
    // A surface impedance is a sheet of what gives it, and back a metal plane.
    const Model impedance = modelOf(twoMetals(QStringLiteral(R"({"type": "rf_ground", "tag": "g", "label": "GND", "properties": {"kind": "impedance", "impedance": "0.02"}})")));
    const Stack back = readBack(hfssStackupOf(impedance).text, StackFormat::Hfss).stack();
    QCOMPARE(back.layers.first().variant, QStringLiteral("metal"));
    QVERIFY(std::abs(back.layers.first().sigma - 1 / (0.02 * 1e-6)) < 1e-3);
    // EMX says the open ground's air, and the lid's copper.
    const StackWritten e = emxProcessOf(modelOf(twoMetals(open, lid)));
    QVERIFY(e.text.contains(QLatin1String("name rf25_open_ground")));
    QVERIFY(e.text.contains(QLatin1String("layer 100 1 name Lid_gap")));
    QVERIFY(e.text.contains(QLatin1String("layer infinity 1 conductivity 5.8e7 name Lid")));
    QVERIFY(!e.text.contains(QLatin1String("name air")));
}

void TestRf25Stack::aStackIsWrittenAsAnEmxProcess()
{
    const Model m = modelOf(twoMetals());
    const StackWritten w = emxProcessOf(m, QStringLiteral("two"), QStringLiteral("the test"));
    QVERIFY(w.ok());
    QStringList lines;
    for (const QString& line : w.text.split(QLatin1Char('\n'))) {
        const QString bare = line.section(QLatin1Char('#'), 0, 0).trimmed();
        if (!bare.isEmpty()) lines << bare;
    }
    QCOMPARE(lines, (QStringList{
        QStringLiteral("assume microns"),
        QStringLiteral("define M1 = L1T0+L1T20"), QStringLiteral("define V1 = L3T0"), QStringLiteral("define M2 = L2T0"),
        QStringLiteral("layer 300 11.9 1 10 name Si tan delta 0.001"),
        QStringLiteral("layer 2 4.1 name Ox1 tan delta 0.002"),
        QStringLiteral("layer 0.5 4.2 name M1_room"), QStringLiteral("conductor 0.5 33000000 M1"),
        QStringLiteral("layer 1 4.2 name Ox2"),
        QStringLiteral("layer 2 7 name M2_room tan delta 0.01"), QStringLiteral("conductor 2 40000000 M2"),
        QStringLiteral("layer 1.5 7 name Pass tan delta 0.01"),
        QStringLiteral("layer infinity 1 name air"),
        QStringLiteral("via M1 M2 20000000 V1")}));
    QCOMPARE(w.mapped, 4);
    QCOMPARE(w.vias, 1);

    // Names EMX cannot have; a metal the map names not; a via to the ground.
    Model odd = m;
    odd.renameLayer(QStringLiteral("M1"), QStringLiteral("1st metal"));
    odd.find(QStringLiteral("m1"))->label = QStringLiteral("1st metal");
    odd.renameLayer(QStringLiteral("M2"), QStringLiteral("via"));
    odd.find(QStringLiteral("m2"))->label = QStringLiteral("via");
    QJsonArray onlyTheVia;
    onlyTheVia.append(QJsonArray{"3", "0", "V1"});
    odd.layout().child(QStringLiteral("rf_layer_map"))->props.insert(QStringLiteral("rows"), onlyTheVia);
    Node down = odd.make(QStringLiteral("rf_via"));
    down.label = QStringLiteral("TSV");
    down.props.insert(QStringLiteral("from"), QStringLiteral("ground"));
    down.props.insert(QStringLiteral("to"), QStringLiteral("1st metal"));
    down.props.insert(QStringLiteral("fill"), QStringLiteral("0.5"));
    down.props.insert(QStringLiteral("sigma"), QStringLiteral("1e7"));
    QVERIFY(odd.add(QStringLiteral("stack"), down, 2) != nullptr);
    const StackWritten o = emxProcessOf(odd);
    QVERIFY2(o.ok(), qPrintable(o.problems.join(QLatin1Char('\n'))));
    QVERIFY(o.text.contains(QLatin1String("conductor 0.5 33000000 x1st_metal")));
    QVERIFY(o.text.contains(QLatin1String("conductor 2 40000000 via_")));
    QVERIFY(o.text.contains(QLatin1String("# define x1st_metal = L?T?")));
    QVERIFY(o.text.contains(QLatin1String("via backside x1st_metal 5000000 TSV")));
    const QString notes = o.notes.join(QLatin1Char('\n'));
    QVERIFY(notes.contains(QLatin1String("1st metal is x1st_metal")));
    QVERIFY(notes.contains(QLatin1String("no row of the layer map names them")));
    QVERIFY(notes.contains(QLatin1String("0.5 metal")));
}

void TestRf25Stack::anEmxProcessReadsBackTheSameStack()
{
    const Model m = modelOf(twoMetals());
    StackRead read;
    const Model back = readBack(emxProcessOf(m).text, StackFormat::Emx, &read);
    QCOMPARE(said(back.stack()), said(m.stack()));
    QVERIFY2(back.stack().ok(), qPrintable(back.stack().problems.join(QLatin1Char('\n'))));
    // The layer map comes back from the defines (the row of no layer is none).
    QCOMPARE(rowsOf(back), (QStringList{QStringLiteral("1/0 M1"), QStringLiteral("1/20 M1"), QStringLiteral("2/0 M2"), QStringLiteral("3/0 V1")}));
}

void TestRf25Stack::anEmxProcessIsReadAsItsGrammarSays()
{
    StackRead read;
    const Model back = readBack(QString::fromLatin1(kProcess), StackFormat::Emx, &read);
    const QStringList got = said(back.stack());
    // In µm from the plane: bulk 0-400; the 6 µm layer 400-406 holds nwell
    // 400-400.25, poly 400.5-400.7, met1 401.5-402.1, met2 403.5-404.4; the
    // 1.5 µm one 406-407.5 holds top 406-408, which reaches 0.5 µm into the
    // 6.5 one (407.5-408.5). Each metal cuts the dielectric it is in.
    // Each via goes just under the metal above it: the one from the plane
    // under nwell, those up to met1 under met1.
    QCOMPARE(got, (QStringList{
        QStringLiteral("ground perfect 0"),
        QStringLiteral("substrate 11.9 0 5 400000"),
        QStringLiteral("via tsv ground nwell 1e+07"),
        QStringLiteral("metal nwell %1 250").arg(1 / (80 * 0.25e-6)),
        QStringLiteral("dielectric 4 0.001 250"),
        QStringLiteral("metal poly 40000 200"),
        QStringLiteral("dielectric 4 0.001 800"),
        QStringLiteral("via cont nwell met1 200000"),
        QStringLiteral("via pcont poly met1 200000"),
        QStringLiteral("metal met1 3e+07 600"),
        QStringLiteral("dielectric 4 0.001 1400"),
        QStringLiteral("via v12 met1 met2 1.5e+07"),
        QStringLiteral("metal met2 3e+07 900"),
        QStringLiteral("dielectric 4 0.001 1600"),
        QStringLiteral("via v23 met2 top 1.5e+07"),
        QStringLiteral("metal top 3.5e+07 2000"),
        QStringLiteral("dielectric 6.5 0.003 500")}));
    // (cont joins nwell to met1 past poly: the stack says so, as it would of
    //  any via that is not between neighbours.)
    QVERIFY(back.stack().problems.join(QLatin1Char(' ')).contains(QLatin1String("The via cont joins nwell and met1, which are not neighbours")));
    QCOMPARE(read.vias, 5);
    QCOMPARE(rowsOf(back), (QStringList{QStringLiteral("10/0 v12"), QStringLiteral("11/0 met2"), QStringLiteral("12/0 v23"), QStringLiteral("13/0 top"),
                                        QStringLiteral("20/0 tsv"), QStringLiteral("5/0 nwell"), QStringLiteral("7/0 poly"), QStringLiteral("7/1 poly"),
                                        QStringLiteral("8/0 cont"), QStringLiteral("9/0 met1")}));
    const QString notes = read.notes.join(QLatin1Char('\n'));
    QVERIFY2(notes.contains(QLatin1String("capacitor statements are left out")), qPrintable(notes));
    QVERIFY(notes.contains(QLatin1String("bias is not done")));
    QVERIFY(notes.contains(QLatin1String("Sheet resistances")));
    // grow(L11T0, ...) and L8T0-poly are more than a union; L8T0*poly's L8T0 is cont's already.
    QVERIFY2(notes.contains(QLatin1String("met2 (\"grow(L11T0,0.02 um)\", mapped from L11T0)")), qPrintable(notes));
    QVERIFY(notes.contains(QLatin1String("cont (\"L8T0-poly\", mapped from L8T0)")));
    QVERIFY(notes.contains(QLatin1String("L8T0 is drawn for more than one layer")));
}

void TestRf25Stack::variationsUnitsFlipsAndHiddenParts()
{
    const char* const text = R"(
assume kA                       # all contexts
assume region microns
assume ohm-cm
assume ohms/sq                  # after ohm-cm, which would undo it for a conductor
define shvar = random 3*sigma 10% limit 2
corner low -1
define thick = 2*5                 # 10 kA: 1 um
define m1 = L1T0
define m2 = L2T0
define vv = L3T0
layer 2500 11.9 1 sqrt(400)+(random sigma 5) name sub   # 20 ohm-cm
begin flip
layer 10 4.1 name below
layer thick 7 tan delta 0.002 0.004 name above
    conductor 5 0.04 -/+ 0.01*shvar +/- 0.3*deltaT m1
end flip
layer 3 um 3.9                  # (a unit of its own)
    position 10
    conductor 10 0.02 m2
<<< an encrypted part
layer 1 2
>>>
layer infinity 1 conductivity 1/(5.8e5)
via m1 m2 { 0.07 => 3.2 ohms/via, 0.12 => 0.9 ohms/via, 1.2e7 S/m } vv name V12
)";
    StackRead read;
    const Model back = readBack(QString::fromLatin1(text), StackFormat::Emx, &read);
    const QStringList got = said(back.stack());
    // The flip puts "above" (1 µm, holding m1 at its old bottom, 0.5 µm) under
    // "below" (1 µm): m1 is at 250.5-251 µm, its top at what was the bottom.
    // m2 1 µm thick at 1 µm into the 3 µm layer; the conducting layer without
    // end a lid on the top, with nothing between.
    QCOMPARE(got, (QStringList{
        QStringLiteral("ground perfect 0"),
        QStringLiteral("substrate 11.9 0 5 250000"),
        QStringLiteral("dielectric 7 0.002 500"),
        QStringLiteral("metal m1 5e+07 500"),
        QStringLiteral("dielectric 4.1 0 1000"),
        QStringLiteral("dielectric 3.9 0 1000"),
        QStringLiteral("via V12 m1 m2 1.2e+07"),
        QStringLiteral("metal m2 5e+07 1000"),
        QStringLiteral("dielectric 3.9 0 1000"),
        QStringLiteral("cover perfect 0")}));
    const QString notes = read.notes.join(QLatin1Char('\n'));
    QVERIFY(notes.contains(QLatin1String("random variables are taken as nought")));
    QVERIFY(notes.contains(QLatin1String("+/- and -/+ variations")));
    QVERIFY(notes.contains(QLatin1String("flipped layers")));
    QVERIFY(notes.contains(QLatin1String("magnetic loss tangent")));
    QVERIFY(notes.contains(QLatin1String("1 encrypted part")));
    QVERIFY(notes.contains(QLatin1String("conducts (58000000 S/m)")));
    QVERIFY(notes.contains(QLatin1String("does not solve yet")));

    // A via's resistance a via is not a conductivity; a name defined nowhere
    // is nought only where part of the file cannot be read.
    StackRead other;
    readBack(QStringLiteral("assume microns\nassume ohms/via\ndefine a = L1T0\ndefine b = L2T0\nlayer 10 4\nconductor 1 3e7 a\noffset 3\n"
                            "conductor 1 3e7 b\nlayer 5 4 tan delta lossy\nlayer infinity 1\nvia a b 0.6 L5T0\n<<<x>>>\n"),
             StackFormat::Emx, &other);
    const QString said2 = other.notes.join(QLatin1Char('\n'));
    QVERIFY(said2.contains(QLatin1String("0.6 Ohm a via")));
    QVERIFY(said2.contains(QLatin1String("lossy is defined nowhere that can be read")));
}

void TestRf25Stack::aStackupOfAnotherHandReads()
{
    // As documented examples have them: a GDSII control file's layers by
    // stream number, two streams to one target, a via between two, no
    // dielectrics above the top metal; mm as its unit.
    const char* const control = R"(<?xml version="1.0" encoding="UTF-8" standalone="no" ?>
<c:Control xmlns:c="http://www.ansys.com/control" schemaVersion="1.0">
<Stackup schemaVersion="1.0">
<Materials>
<Material Name="FR4"><Permittivity><Double>4.4</Double></Permittivity><DielectricLossTangent><Double>0.02</Double></DielectricLossTangent></Material>
<Material Name="copper"><Conductivity><Double>58000000</Double></Conductivity></Material>
</Materials>
<ELayers LengthUnit="mm">
<Dielectrics>
<Layer Color="#008000" Material="FR4" Name="D2" Thickness="0.2"/>
<Layer Color="#008000" Material="FR4" Name="D1" Thickness="0.8"/>
</Dielectrics>
<Layers>
<Layer Color="#008000" Elevation="1" Material="copper" Name="30" GDSDataType="0" TargetLayer="Top" Thickness="0.035" Type="conductor"/>
<Layer Color="#008000" Elevation="1" Material="copper" Name="31" GDSDataType="0" TargetLayer="Top" Thickness="0.035" Type="conductor"/>
<Layer Color="#008000" Elevation="0.8" Material="copper" Name="40" TargetLayer="Inner" Thickness="0.035" Type="conductor"/>
<Layer Color="#008000" Elevation="0" Material="copper" Name="55" GDSDataType="0" TargetLayer="Bottom" Thickness="0.035" Type="conductor"/>
<Layer Color="#353981" Name="Outline" Type="outline"/>
</Layers>
<Vias>
<Layer Color="#bd00bd" Material="copper" Name="32" GDSDataType="0" TargetLayer="via" StartLayer="Top" StopLayer="Inner"/>
</Vias>
</ELayers>
</Stackup>
<ImportOptions Flatten="true"/>
</c:Control>
)";
    StackRead read;
    const Model back = readBack(QString::fromLatin1(control), StackFormat::Hfss, &read);
    // Bottom, at nought and under every dielectric, is the plane; Top stands
    // on the dielectrics; Inner cuts D1 at 0.8 mm. The ground is no metal and
    // has no row of its own.
    QCOMPARE(said(back.stack()), (QStringList{
        QStringLiteral("ground metal 5.8e+07"),
        QStringLiteral("substrate 4.4 0.02 0 765000"),
        QStringLiteral("metal Inner 5.8e+07 35000"),
        QStringLiteral("dielectric 4.4 0.02 165000"),
        QStringLiteral("via via Inner Top 5.8e+07"),
        QStringLiteral("metal Top 5.8e+07 35000")}));
    QCOMPARE(rowsOf(back), (QStringList{QStringLiteral("30/0 Top"), QStringLiteral("31/0 Top"), QStringLiteral("32/0 via"), QStringLiteral("40/0 Inner")}));
    QCOMPARE(read.vias, 1);
}

// The import of a control file (rf25_hfss_import's) read a <Permeability>
// written after <Permittivity> as the permittivity: every dielectric of 1, as
// rf25_hfss_export's own control file has them both.
void TestRf25Stack::theControlFileImportKeepsEachPermittivity()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("two.xml"));
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QVERIFY(f.write(hfssStackupOf(modelOf(twoMetals())).text.toUtf8()) > 0);
    }
    QString why;
    const HfssImport read = hfssImportOf(path, {}, &why);
    QVERIFY2(read.model, qPrintable(why));
    QStringList epsr;
    // (Its own four: with no GDSII file to tell the plane by, that route
    //  keeps the plane a metal, with a gap of its own over it.)
    for (const Layer& l : read.model->stack().layers)
        if ((l.kind == Layer::Substrate || l.kind == Layer::Dielectric) && QStringList({QStringLiteral("Si"), QStringLiteral("Ox1"), QStringLiteral("Ox2"), QStringLiteral("Pass")}).contains(l.name))
            epsr << QString::number(l.epsr);
    QCOMPARE(epsr, (QStringList{QStringLiteral("11.9"), QStringLiteral("4.1"), QStringLiteral("4.2"), QStringLiteral("7")}));
}

void TestRf25Stack::whatCannotBeReadIsRefused()
{
    const auto refused = [](const QByteArray& text, StackFormat f, const char* why) {
        QString error;
        const std::optional<StackRead> r = stackTextRead(text, f, QStringLiteral("bad"), &error);
        if (r) return QStringLiteral("read");
        return error.contains(QLatin1String(why)) ? QString() : error;
    };
    QCOMPARE(refused("layer\n", StackFormat::Emx, "a number wanted"), QString());
    QCOMPARE(refused("frobnicate 1 2\nlayer 1 2\n", StackFormat::Emx, "begins no statement"), QString());
    QCOMPARE(refused("layer infinity 1\nlayer 2 3\n", StackFormat::Emx, "above the one without end"), QString());
    QCOMPARE(refused("begin flip\nlayer 1 4\n", StackFormat::Emx, "no \"end flip\""), QString());
    QCOMPARE(refused("end flip\n", StackFormat::Emx, "no \"begin flip\""), QString());
    QCOMPARE(refused("layer 1 4 sideways\n", StackFormat::Emx, "sideways is defined nowhere"), QString());
    QCOMPARE(refused("layer 1 4 1 0 frobs\n", StackFormat::Emx, "is not read in a layer"), QString());
    QCOMPARE(refused("conductor 1 2 m\n", StackFormat::Emx, "before any layer"), QString());
    QCOMPARE(refused("assume parsecs\n", StackFormat::Emx, "no unit"), QString());
    QCOMPARE(refused("define a = a + 1\nlayer a 4\n", StackFormat::Emx, "defined by itself"), QString());
    QCOMPARE(refused("layer 1 4 1 3 ohm/sq\n", StackFormat::Emx, "only a conductor's"), QString());
    QCOMPARE(refused("assume ohms/sq\nlayer 1 4\nconductor 0 5 m\n", StackFormat::Emx, "no thickness"), QString());
    QCOMPARE(refused("# nothing\n", StackFormat::Emx, "no layer"), QString());
    QCOMPARE(refused("<c:Control xmlns:c=\"http://www.ansys.com/control\"><Stackup/></c:Control>", StackFormat::Hfss, "has no layers"), QString());
    QCOMPARE(refused("<Stackup><Layers/></Stackup>", StackFormat::Hfss, "no control file"), QString());
    QCOMPARE(refused("<c:Control", StackFormat::Hfss, "does not read as XML"), QString());

    // A file by its suffix, else by what it begins with.
    QTemporaryDir dir;
    const auto write = [&dir](const QString& name, const QByteArray& text) {
        QFile f(dir.filePath(name));
        if (!f.open(QIODevice::WriteOnly) || f.write(text) != text.size()) qFatal("not written");
        return f.fileName();
    };
    QVERIFY(stackFileRead(write(QStringLiteral("a.proc"), "layer 1 4\nconductor 1 1e7 L1T0\nlayer infinity 1\n")));
    QVERIFY(stackFileRead(write(QStringLiteral("a.txt"), "layer 1 4\nconductor 1 1e7 L1T0\nlayer infinity 1\n")));
    const QString xml = write(QStringLiteral("b.stack"), hfssStackupOf(modelOf(twoMetals())).text.toUtf8());
    const std::optional<StackRead> byContent = stackFileRead(xml);
    QVERIFY(byContent);
    QCOMPARE(byContent->format, StackFormat::Hfss);
    QString why;
    QVERIFY(!stackFileRead(dir.filePath(QStringLiteral("none.proc")), &why));
    QVERIFY(why.contains(QLatin1String("could not be read")));
    QCOMPARE(stackFormatOf(QStringLiteral("x.XML")), std::optional<StackFormat>(StackFormat::Hfss));
    QCOMPARE(stackFormatOf(QStringLiteral("x.proc")), std::optional<StackFormat>(StackFormat::Emx));
    QVERIFY(!stackFormatOf(QStringLiteral("x.py")));
}

void TestRf25Stack::aFileCutAnywhereNeverFaults()
{
    const QByteArray emx(kProcess);
    const QByteArray hfss = hfssStackupOf(modelOf(twoMetals())).text.toUtf8();
    int read = 0;
    for (int n = 0; n <= emx.size(); ++n) {
        QString why;
        const std::optional<StackRead> r = stackTextRead(emx.left(n), StackFormat::Emx, QStringLiteral("cut"), &why);
        read += r ? 1 : 0;
        if (!r) QVERIFY(!why.isEmpty());
    }
    for (int n = 0; n <= hfss.size(); n += 7) {
        QString why;
        const std::optional<StackRead> r = stackTextRead(hfss.left(n), StackFormat::Hfss, QStringLiteral("cut"), &why);
        if (!r) QVERIFY(!why.isEmpty());
    }
    QVERIFY(read > 0);
}

void TestRf25Stack::theStackReplacesTheModelsAlone()
{
    const Model m = modelOf(twoMetals());
    // A process of one metal, called Top, on layer 2: M2's row goes, 1/x stays
    // for M1 (no longer there: said), the port on M2 is said.
    const char* const one = "assume microns\ndefine Top = L2T0\nlayer 100 3.5\nlayer 1 3.5\nconductor 1 5.8e7 Top\nlayer infinity 1\n";
    StackRead read;
    QString why;
    const std::optional<StackRead> r = stackTextRead(QByteArray(one), StackFormat::Emx, QStringLiteral("one"), &why);
    QVERIFY(r);
    QStringList notes;
    const Model back = withStackOf(m, *r, &notes);
    QCOMPARE(said(back.stack()), (QStringList{QStringLiteral("ground perfect 0"), QStringLiteral("substrate 3.5 0 0 100000"), QStringLiteral("metal Top 5.8e+07 1000")}));
    QCOMPARE(rowsOf(back), (QStringList{QStringLiteral("1/0 M1"), QStringLiteral("1/20 M1"), QStringLiteral("2/0 Top"), QStringLiteral("3/0 V1"), QStringLiteral("9/0 ")}));
    QVERIFY2(notes.join(QLatin1Char('\n')).contains(QLatin1String("M2")), qPrintable(notes.join(QLatin1Char('\n'))));
    // The rest of the tree as it was, and every tag its own.
    QCOMPARE(back.ports().children.size(), m.ports().children.size());
    QSet<QString> tags;
    std::function<void(const Node&)> walk = [&](const Node& n) {
        QVERIFY2(!tags.contains(n.tag), qPrintable(n.tag));
        tags.insert(n.tag);
        for (const Node& c : n.children) walk(c);
    };
    walk(back.root);
}

// The check of 9 November (qucs-reviews/RF2p5D/2026-11-09-claude-rf25-stack-
// roundtrip.md): S1, a metal solved as its two faces came back a sheet, unsaid;
// S2, EMX's renames of a dielectric and of the plane went unsaid. The
// recheck: a metal with no namesake came in a sheet and smooth, unsaid.
void TestRf25Stack::whatNeitherFileSaysOfAMetalIsKeptByName()
{
    Model m = modelOf(twoMetals());
    m.find(QStringLiteral("m2"))->props.insert(QStringLiteral("model"), QStringLiteral("faces"));
    m.find(QStringLiteral("m2"))->props.insert(QStringLiteral("roughness"), QStringLiteral("0.2[um]"));
    m.renameLayer(QStringLiteral("M2"), QStringLiteral("M2 thick"));
    m.find(QStringLiteral("m2"))->label = QStringLiteral("M2 thick");
    m.find(QStringLiteral("d1"))->label = QStringLiteral("Si oxide");
    m.find(QStringLiteral("g"))->label = QStringLiteral("Plane");
    for (const StackFormat f : {StackFormat::Hfss, StackFormat::Emx}) {
        const StackWritten w = f == StackFormat::Hfss ? hfssStackupOf(m) : emxProcessOf(m);
        QVERIFY2(w.ok(), qPrintable(w.problems.join(QLatin1Char('\n'))));
        const QString said = w.notes.join(QLatin1Char('\n'));
        QVERIFY2(said.contains(QLatin1String("M2 thick solved as their two faces")), qPrintable(said));
        QVERIFY(said.contains(QLatin1String("roughness")));
        if (f == StackFormat::Emx) {
            QVERIFY2(said.contains(QLatin1String("M2 thick is M2_thick")), qPrintable(said));
            QVERIFY2(said.contains(QLatin1String("Si oxide is Si_oxide")), qPrintable(said));
            QVERIFY2(said.contains(QLatin1String("Plane is EMX's plane")), qPrintable(said));
        }
        // Into this model: kept (by EMX's spelling of the name too), and said.
        QString why;
        const std::optional<StackRead> r = stackTextRead(w.text.toUtf8(), f, QStringLiteral("back"), &why);
        QVERIFY2(r, qPrintable(why));
        QStringList notes;
        const Model back = withStackOf(m, *r, &notes);
        // (Each stack kept while its layer is read: stack() makes a new one.)
        const Stack backStack = back.stack();
        const Layer* metal = backStack.named(f == StackFormat::Hfss ? QStringLiteral("M2 thick") : QStringLiteral("M2_thick"));
        QVERIFY(metal != nullptr);
        QVERIFY(metal->twoSheets);
        QVERIFY(std::abs(metal->roughness - 0.2e-6) < 1e-15);
        const QString kept = notes.join(QLatin1Char('\n'));
        QVERIFY2(kept.contains(QLatin1String("still solved as their two faces")) && kept.contains(QLatin1String("keep the roughness")), qPrintable(kept));
        // Into a model with no such metal: a sheet, smooth, and said, for
        // every metal that had no namesake to keep them from.
        notes.clear();
        const Model fresh = withStackOf(Model::blank(), *r, &notes);
        const Stack freshStack = fresh.stack();
        const Layer* other = freshStack.named(metal->name);
        QVERIFY(other != nullptr);
        QVERIFY(!other->twoSheets);
        QCOMPARE(other->roughness, 0.0);
        const QString freshSaid = notes.join(QLatin1Char('\n'));
        QVERIFY(!freshSaid.contains(QLatin1String("two faces")));
        // A blank model's own M1, a sheet, has M1's namesake: only M2 is said.
        QVERIFY2(freshSaid.contains(QStringLiteral("\n%1 came in as a sheet with no roughness").arg(metal->name)) || freshSaid.startsWith(QStringLiteral("%1 came in as a sheet with no roughness").arg(metal->name)), qPrintable(freshSaid));
        QVERIFY2(!freshSaid.contains(QLatin1String("M1 came in")) && !freshSaid.contains(QLatin1String("M1,")), qPrintable(freshSaid));
        // Into a model whose stack has no metal at all: both said, together.
        Model bare = Model::blank();
        bare.stackNode().children.erase(std::remove_if(bare.stackNode().children.begin(), bare.stackNode().children.end(),
                                                       [](const Node& n) { return n.type == QLatin1String("rf_metal"); }),
                                        bare.stackNode().children.end());
        notes.clear();
        withStackOf(bare, *r, &notes);
        QVERIFY2(notes.join(QLatin1Char('\n')).contains(QStringLiteral("M1, %1 came in as sheets with no roughness").arg(metal->name)), qPrintable(notes.join(QLatin1Char('\n'))));
        // M1, a sheet before, a sheet still, and not said; nor is a metal kept.
        QVERIFY(!back.stack().named(QStringLiteral("M1"))->twoSheets);
        QVERIFY(!kept.contains(QLatin1String("M1 still")));
        QVERIFY2(!kept.contains(QLatin1String("came in as sheets")), qPrintable(kept));
    }
}

void TestRf25Stack::realProcessFilesRead()
{
    const QString folder = qEnvironmentVariable("QUCS_EMX_SAMPLES");
    if (folder.isEmpty()) QSKIP("QUCS_EMX_SAMPLES names no folder of process files");
    QDirIterator it(folder, {QStringLiteral("*.proc")}, QDir::Files, QDirIterator::Subdirectories);
    int files = 0;
    while (it.hasNext()) {
        const QString path = it.next();
        QString why;
        const std::optional<StackRead> r = stackFileRead(path, &why);
        QVERIFY2(r, qPrintable(why));
        const Model back = withStackOf(Model::blank(), *r);
        int metals = 0;
        for (const Layer& l : back.stack().layers) metals += l.kind == Layer::Metal ? 1 : 0;
        QVERIFY2(metals > 0, qPrintable(path));
        qInfo().noquote() << QFileInfo(path).fileName() << r->layers.size() << "layers," << r->vias << "vias," << r->rows.size() << "rows";
        if (qEnvironmentVariableIsSet("QUCS_EMX_SAMPLES_SAY"))
            qInfo().noquote() << said(back.stack()).join(QLatin1Char('\n')) << "\n" << r->notes.join(QLatin1Char('\n')) << "\n"
                              << back.stack().problems.join(QLatin1Char('\n'));
        // And out again, as EMX, the same stack.
        if (back.stack().ok()) {
            const std::optional<StackRead> out = stackTextRead(emxProcessOf(back).text.toUtf8(), StackFormat::Emx, QStringLiteral("again"));
            QVERIFY(out);
            const Model again = withStackOf(Model::blank(), *out);
            QCOMPARE(said(again.stack()), said(back.stack()));
        }
        ++files;
    }
    QVERIFY(files > 0);
}

QTEST_GUILESS_MAIN(TestRf25Stack)
#include "test_rf25_stack.moc"
