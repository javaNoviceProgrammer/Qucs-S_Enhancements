/*
 * test_rf25_hfss.cpp - an RF 2.5D model written for HFSS™ 3D Layout and read
 * back from it (rf25/rf25_hfss.h), a project read without any Ansys® software
 * (rf25/rf25_edb.h), and a solve's S beside a Touchstone file's.
 *
 * No Electronics Desktop runs here. A script is parsed by Python, then run
 * against a stand-in that records what it asks and answers as the
 * documentation says the real one does; the control file is read by an XML reader, and
 * back by the import. An edb.def is written here by a small writer of the
 * format as public samples have it, since no file of Ansys may be kept in
 * this repository (scripts/ci/check-third-party.py).
 */
#include <QtTest>

#include "dataimport.h"
#include "layout.h"
#include "rf25/rf25_edb.h"
#include "rf25/rf25_fullwave.h"
#include "rf25/rf25_hfss.h"
#include "rf25/rf25_layout.h"
#include "rf25/rf25_model.h"
#include "rf25/rf25_sparams.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QXmlStreamReader>

#include <cmath>
#include <complex>
#include <cstring>
#include <limits>

using namespace qucs_s::rf25;
using Cx = std::complex<double>;

namespace {

/// Under the sanitizers, or asked to go easy.
bool slowBuild()
{
    if (qEnvironmentVariableIsSet("QUCS_TEST_SLOW")) return true;
#if defined(__SANITIZE_ADDRESS__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
    return true;
#endif
#endif
    return false;
}

/// A model of JSON text, its tree's children as given; the parts it does not
/// give are filled in by the reader.
Model modelOf(const QString& children)
{
    const QString json = QStringLiteral("{\"format\": \"qucs-s rf 2.5d 1\", \"tree\": {\"type\": \"rf_model\", \"children\": [%1]}}").arg(children);
    QString why;
    const std::optional<Model> m = Model::fromJson(json.toUtf8(), &why);
    if (!m) qFatal("the test's model does not read: %s", qPrintable(why));
    return *m;
}

const char* const kStudy = R"({"type": "rf_study", "tag": "study", "properties": {"kind": "full_wave", "scale": "linear", "from": "1[GHz]", "to": "40[GHz]", "points": "40"}})";

/// A microstrip's stack: a plane, a substrate of 3.12 and 0.002, a metal.
QString microstripStack(const QString& ground = QStringLiteral("perfect"))
{
    return QStringLiteral(R"({"type": "rf_stack", "tag": "stack", "children": [
        {"type": "rf_ground", "tag": "g", "label": "GND", "properties": {"kind": "%1", "sigma": "5.8e7"}},
        {"type": "rf_substrate", "tag": "s", "label": "Si", "properties": {"thickness": "500[um]", "epsr": "11.7", "sigma": "6.5[S/m]"}},
        {"type": "rf_dielectric", "tag": "d", "label": "PI", "properties": {"thickness": "20[um]", "epsr": "3.12", "tand": "0.002"}},
        {"type": "rf_metal", "tag": "m", "label": "M1", "properties": {"thickness": "1.5[um]", "sigma": "4.1e7", "roughness": "0.2[um]"}}]})")
        .arg(ground);
}

Conductor box(const QString& layer, double x0, double y0, double x1, double y1)
{
    Conductor c;
    c.layer = layer;
    c.outline = QPolygonF(QList<QPointF>{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}});
    return c;
}

/// A coplanar line along x, \a l µm long: a signal 62 wide and grounds of 300.
QList<Conductor> coplanar(double l)
{
    const QString m = QStringLiteral("M1");
    return {box(m, 0, -31, l, 31), box(m, 0, 40, l, 340), box(m, 0, -340, l, -40)};
}

QString portsOf(const QString& ports)
{
    return QStringLiteral(R"({"type": "rf_ports", "tag": "ports", "children": [%1]})").arg(ports);
}

/// The line of Python \a script that assigns \a name, as text.
bool python(QString* where)
{
    *where = QStandardPaths::findExecutable(QStringLiteral("python3"));
    return !where->isEmpty();
}

/// A stand-in for Electronics Desktop: whatever is asked of it is recorded,
/// and it answers what the documentation says. Recorded to \a out as JSON.
const char* const kStandIn = R"PY(
import json, os, sys, types
calls = []
class Point:
    def __init__(self, x, y):
        self.x, self.y = x, y
    def GetX(self): return self.x
    def GetY(self): return self.y
class Polygon:
    def __init__(self, points): self.points = points
    def GetPoints(self):
        how = os.environ.get("RF25_READ", "")
        points = self.points
        if how == "rotated": points = points[1:] + points[:1]
        if how == "closed": points = points + points[:1]
        if how == "moved": points = [(x + 1e-3, y) for x, y in points]
        return [Point(x, y) for x, y in points]
class Object:
    def __init__(self, name): self._name = name
    def __getattr__(self, attribute):
        def call(*arguments):
            calls.append([self._name + "." + attribute, list(arguments)])
            return self.answer(attribute, arguments)
        return call
    def answer(self, attribute, arguments):
        return None
polygons = {}
ports = []
UNITS = 1e-6
class Editor(Object):
    def answer(self, attribute, arguments):
        if attribute == "GetStackupLayerNames": return ["TOP", "BOTTOM"] if not stack else [l for l in stack]
        if attribute == "AddStackupLayer":
            a = arguments[0]
            stack.append(a[a.index("Name:=") + 1])
            return None
        if attribute == "CreatePolygon":
            g = arguments[0][arguments[0].index("polyGeometry:=") + 1]
            xs = [g[k + 1] * UNITS for k in range(len(g)) if g[k] == "x:="]
            ys = [g[k + 1] * UNITS for k in range(len(g)) if g[k] == "y:="]
            polygons[g[g.index("Name:=") + 1]] = list(zip(xs, ys))
            return g[g.index("Name:=") + 1]
        if attribute == "GetPolygon": return Polygon(polygons[arguments[0]])
        if attribute in ("CreateEdgePort", "CreateCircuitPort"):
            ports.append("Port%d" % (len(ports) + 1))
            return ports[-1]
        return None
stack = []
class Excitations(Object):
    def answer(self, attribute, arguments):
        if attribute == "GetAllPortsList": return list(ports)
        if attribute == "Rename": ports[ports.index(arguments[0])] = arguments[1]
        return None
class Design(Object):
    def answer(self, attribute, arguments):
        if attribute == "SetActiveEditor": return Editor("oEditor")
        if attribute == "GetModule": return Excitations("oExcitations") if arguments[0] == "Excitations" else Object("oSetups")
        return None
class Definitions(Object):
    def answer(self, attribute, arguments):
        if attribute == "GetManager": return Object("oPadstacks")
        return None
class Project(Object):
    def answer(self, attribute, arguments):
        if attribute == "GetDefinitionManager": return Definitions("oDefinitions")
        if attribute == "SetActiveDesign": return Design("oDesign")
        return None
class Desktop(Object):
    def answer(self, attribute, arguments):
        if attribute == "NewProject": return Project("oProject")
        if attribute == "AddMessage" and arguments[2]: print("trouble: " + arguments[3])
        return None
module = types.ModuleType("ScriptEnv")
def initialize(name):
    calls.append(["ScriptEnv.Initialize", [name]])
    import builtins
    builtins.oDesktop = Desktop("oDesktop")
module.Initialize = initialize
sys.modules["ScriptEnv"] = module
script, out = sys.argv[1], sys.argv[2]
sys.argv = [script]
source = open(script, encoding="utf-8").read()
scope = {"__file__": script, "__name__": "__main__"}
if len(sys.argv) > 0 and os.environ.get("RF25_ARG"):
    scope["ScriptArgument"] = os.environ["RF25_ARG"]
exec(compile(source, script, "exec"), scope)
json.dump(calls, open(out, "w"))
)PY";

struct Ran {
    QList<QJsonArray> calls;
    QString output;
    bool ok = false;
    /// Those of \a name, in order: each its arguments.
    QList<QJsonArray> of(const QString& name) const
    {
        QList<QJsonArray> out;
        for (const QJsonArray& c : calls)
            if (c.at(0).toString() == name) out << c.at(1).toArray();
        return out;
    }
};

/// What follows \a key in a recorded array ("Name:=", value).
QJsonValue valueAfter(const QJsonArray& a, const QString& key)
{
    for (qsizetype k = 0; k + 1 < a.size(); ++k)
        if (a.at(k).isString() && a.at(k).toString() == key) return a.at(k + 1);
    return {};
}

/// \a script run against the stand-in, in \a folder. \a polygonsRead says how
/// a polygon reads back: "rotated", "closed", "moved", or as it was written.
Ran runScript(const QString& script, const QString& folder, const QString& argument = {}, const QString& polygonsRead = {})
{
    Ran out;
    QString exe;
    if (!python(&exe)) return out;
    const QString standIn = QDir(folder).filePath(QStringLiteral("standin.py"));
    const QString answer = QDir(folder).filePath(QStringLiteral("calls.json"));
    {
        QFile f(standIn);
        f.open(QIODevice::WriteOnly);
        f.write(kStandIn);
    }
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!argument.isEmpty()) env.insert(QStringLiteral("RF25_ARG"), argument);
    if (!polygonsRead.isEmpty()) env.insert(QStringLiteral("RF25_READ"), polygonsRead);
    p.setProcessEnvironment(env);
    p.setWorkingDirectory(folder);
    p.start(exe, {QStringLiteral("-B"), QStringLiteral("-I"), standIn, script, answer});
    p.waitForFinished(120000);
    out.output = QString::fromUtf8(p.readAllStandardOutput() + p.readAllStandardError());
    out.ok = p.exitCode() == 0;
    QFile f(answer);
    if (f.open(QIODevice::ReadOnly))
        for (const QJsonValue& v : QJsonDocument::fromJson(f.readAll()).array()) out.calls << v.toArray();
    return out;
}

/// Python's own parser's word on \a script.
QString parsed(const QString& script)
{
    QString exe;
    if (!python(&exe)) return QStringLiteral("no python3");
    QProcess p;
    p.start(exe, {QStringLiteral("-B"), QStringLiteral("-I"), QStringLiteral("-c"),
                  QStringLiteral("import ast,sys; ast.parse(open(sys.argv[1], encoding='utf-8').read())"), script});
    p.waitForFinished(60000);
    return p.exitCode() == 0 ? QString() : QString::fromUtf8(p.readAllStandardError());
}

/// An export written to \a folder as model.py, model.xml, model.gds.
QString written(const HfssExport& e, const QString& folder)
{
    const QString path = QDir(folder).filePath(QStringLiteral("model.py"));
    QString why;
    QStringList files;
    if (!writeHfssExport(e, path, &files, &why)) qWarning("%s", qPrintable(why));
    return path;
}

// ------------------------------------------------- A small edb.def, written here

class EdbWriter
{
public:
    QByteArray bytes;
    void i(qint32 v) { bytes.append(reinterpret_cast<const char*>(&v), 4); }
    void d(double v) { bytes.append(reinterpret_cast<const char*>(&v), 8); }
    void by(int v) { bytes.append(char(v)); }
    void s(const QByteArray& text)
    {
        i(qint32(text.size()));
        bytes.append(text);
    }
    void value(double v, const QByteArray& expression = {})
    {
        d(v);
        s(expression);
    }
    /// The start of a record: its classes' chain down to class 5, whose id and
    /// two lists follow.
    void base(std::initializer_list<int> chain, int id, const QByteArray& name, int net, int parent, int kind)
    {
        for (const int c : chain) i(c);
        i(5);
        i(id);
        if (name.isEmpty()) {
            i(0);
            i(-1);
        } else {
            // As a shape keeps its name: three lists of arrays.
            i(3);
            i(6);
            i(0);
            i(7);
            i(0);
            i(7);
            i(1);
            i(7);
            i(2);
            i(1);
            i(1);
            i(4);
            s(name);
        }
        i(0);
        i(-1);
        i(net);
        i(parent);
        i(kind);
    }
};

/// The class table a version of the format has, as far as is written here.
QList<QPair<int, QList<int>>> edbClasses()
{
    return {{5, {1, 6, 6}},
            {7, {5, 4, 1, 1, 6, 1}},
            {8, {5, 4, 4}},
            {10, {5, 1, 1, 1}},
            {11, {5, 1, 0, 1, 1}},
            {12, {5, 1, 3, 3, 3, 3, 3, 3}},
            {13, {5, 3, 3, 3}},
            {14, {5, 1, 1, 1, 3, 3, 5, 5, 0}},
            {15, {5, 5}},
            {19, {5, 1, 3, 3, 3, 3, 4, 1, 4, 0, 0, 6, 4}},
            {29, {5, 4, 1, 4, 0, 0, 0, 0, 1, 6, 1, 4, 4, 4, 1, 4, 1, 4, 1, 4, 0, 0}},
            {30, {5, 6}},
            {32, {5, 1, 4, 3}},
            {36, {0, 1, 6}},
            {39, {1, 2, 2, 1, 1}}};
}

void writeShapeHead(EdbWriter& w, int cls, int id, const QByteArray& name, int net, int layer, int owner = -1)
{
    w.base({cls, 11, 10}, id, name, net, -1, 4);
    w.i(layer);
    w.by(0);
    w.i(0);
    w.i(owner);
}

void writePoints(EdbWriter& w, const QList<double>& xy, bool closed)
{
    w.i(36);
    w.by(closed ? 1 : 0);
    w.i(2);
    w.i(qint32(xy.size()));
    w.i(2);
    for (const double v : xy) w.d(v);
}

/// A terminal's fields under class 29, with what a port keeps.
void writeTerminal(EdbWriter& w, std::initializer_list<int> chain, int id, const QByteArray& name, int net, int layer, const QByteArray& impedance,
                   int reference, const QByteArray& hfss, const QByteArray& excitation)
{
    w.base(chain, id, {}, net, -1, 0);
    w.s(name);
    w.i(layer);
    w.s({});
    w.by(0);
    w.by(0);
    w.by(0);
    w.by(0);
    w.i(0);
    if (hfss.isEmpty()) {
        w.i(0);
        w.i(-1);
    } else {
        w.i(2);
        w.i(4);
        w.s("HFSS");
        w.s(hfss);
    }
    w.i(reference);
    w.s(impedance);
    w.s({});
    w.s({});
    w.i(-1);
    w.s({});
    w.i(0);
    w.s(excitation);
    w.i(0);
    w.s({});
    w.by(0);
    w.by(0);
}

void writeBlock(EdbWriter& w, const QByteArray& text)
{
    w.s(text);
}

/// What every small edb.def starts with: its header, its classes, the project,
/// its padstack's layers \a padLayers.
void writeEdbHead(EdbWriter& w, const QByteArray& padLayers = "\t\t\t\t\t$begin 'lgm'\n\t\t\t\t\t\tlay='TOP'\n\t\t\t\t\t$end 'lgm'\n\t\t\t\t\t$begin 'lgm'\n\t\t\t\t\t\tlay='BOT'\n\t\t\t\t\t$end 'lgm'\n")
{
    const QByteArray header = "$begin 'Hdr'\n\tVersion='10.6'\n\tLastUpdateTimeStamp=1\n\tEncrypted=false\n$end 'Hdr'\n";
    w.by(0);
    w.s(header);
    const auto classes = edbClasses();
    w.i(-1);
    w.i(qint32(classes.size()));
    for (const auto& [id, types] : classes) {
        w.i(id);
        w.i(qint32(types.size()));
        int f = 0;
        for (const int t : types) {
            w.i(f++);
            w.i(t);
        }
    }
    w.i(-1);
    w.i(1);
    w.i(0);
    writeBlock(w, "$begin 'EDB'\n\tSource=''\n\t$begin 'PropList'\n\t\tVariableProp(n='$w', v='0.4mm', vt=0, oi=0, ds='')\n\t$end 'PropList'\n"
                  "\t$begin 'pds'\n\t\t$begin 'pd'\n\t\t\tid=7\n\t\t\tln=''\n\t\t\t$begin 'psd'\n\t\t\t\tnam='GVIA'\n\t\t\t\tmat='copper'\n\t\t\t\tplt='100'\n"
                  "\t\t\t\t$begin 'pds'\n" + padLayers + "\t\t\t\t$end 'pds'\n"
                  "\t\t\t\thle(shp='Cir', Szs('0.3mm'), X='0', Y='0', R='0deg')\n\t\t\t\thRg='UTL'\n\t\t\t$end 'psd'\n\t\t$end 'pd'\n\t$end 'pds'\n"
                  "\t$begin 'Materials'\n"
                  "\t\t$begin 'copper'\n\t\t\tpermittivity='1'\n\t\t\tconductivity='58000000'\n\t\t$end 'copper'\n"
                  "\t\t$begin 'fr4'\n\t\t\tpermittivity='4.4'\n\t\t\tconductivity='0'\n\t\t\tdielectric_loss_tangent='0.02'\n\t\t$end 'fr4'\n"
                  "\t\t$begin 'air'\n\t\t\tpermittivity='1.0006'\n\t\t$end 'air'\n"
                  "\t$end 'Materials'\n$end 'EDB'\n");
}

/// A coplanar line as a project has it: on a board of two layers, a
/// rectangle for a plane, a line, a polygon with a void and an arc, a via, an
/// edge port and a point port, and a setup with its sweep. \a stack is a stack
/// in place of the board's (the shapes stand on the layers of ids 0 and 2),
/// \a moreShapes records to add to its shapes, \a edgePortKeeps what its edge
/// port keeps by the solver's name, \a padLayers its padstack's layers, \a viaNet
/// its via's net, \a viaRotation its turn and \a viaHole the number it keeps for a
/// hole.
QByteArray edbFile(const QByteArray& extraClassForAStop = {}, const QByteArray& stack = {}, const QList<QByteArray>& moreShapes = {},
                   const QByteArray& edgePortKeeps = "HFSS('HFSS Type'='Wave', Orientation='Vertical')", const QByteArray& padLayers = {}, int viaNet = 0,
                   double viaRotation = 0, double viaHole = 0.0003)
{
    EdbWriter w;
    if (padLayers.isEmpty()) writeEdbHead(w);
    else writeEdbHead(w, padLayers);
    w.i(1);
    w.i(0);
    w.i(5);
    w.i(1);
    w.i(1);
    w.i(5);
    w.i(2);
    w.s("layout0");
    w.i(1);
    w.i(5);
    w.i(3);
    writeBlock(w, "$begin ''\n\tid=0\n\tdn='Board'\n\t$begin 'PropList'\n\t$end 'PropList'\n\t$begin 'SimSetups'\n"
                  "\t\tSimSetup(id=2, n='HFSS Setup 1', t=0, v='$begin \\'SimSetupInfo\\'\\\n\\\t$begin \\'SingleFrequencyDataList\\'\\\n\\\t\\\tAdaptiveFrequency=\\'8GHz\\'\\\n\\\t$end \\'SingleFrequencyDataList\\'\\\n"
                  "\\\t$begin \\'SweepData\\'\\\n\\\t\\\tSweepName=\\'Sweep 1\\'\\\n\\\t\\\tFrequencyString=\\'LINC 1GHz 10GHz 10\\'\\\n\\\t$end \\'SweepData\\'\\\n$end \\'SimSetupInfo\\'\\\n')\n"
                  "\t$end 'SimSetups'\n$end ''\n");
    w.i(3);
    w.i(6);
    w.i(0);
    w.i(7);
    w.i(0);
    w.i(7);
    w.i(0);
    w.i(7);
    writeBlock(w, !stack.isEmpty() ? stack
                                   : QByteArray("$begin ''\n\tMode='Laminate'\n"
                                                "\tSLayer(Layer(N='TOP', ID=2, T='signal', Zones()), SubL(Th='50um', LElev='1.05mm', R='0um', Mat='copper', FilMat='air'), UseR=false)\n"
                                                "\tSLayer(Layer(N='D1', ID=1, T='dielectric', Zones()), SubL(Th='1mm', LElev='0.05mm', R='0um', Mat='fr4'), UseR=false)\n"
                                                "\tSLayer(Layer(N='BOT', ID=0, T='signal', Zones()), SubL(Th='50um', LElev='0mm', R='0um', Mat='copper', FilMat='air'), UseR=false)\n$end ''\n"));
    w.i(1);
    w.i(5);
    w.i(6);
    w.i(0);
    writeBlock(w, "$begin ''\n\tdef=7\n\tfl=0\n\ttl=2\n$end ''\n");
    // Its records.
    w.i(4);
    w.i(0);
    // The nets.
    w.i(2);
    w.i(5);
    for (const auto& [id, name] : QList<QPair<int, QByteArray>>{{0, "gnd"}, {1, "sig"}}) {
        w.i(7);
        w.i(5);
        w.i(id);
        w.i(0);
        w.i(-1);
        w.i(0);
        w.i(-1);
        w.s(name);
        w.i(0);
        w.i(0);
        w.i(0);
        w.i(-1);
        w.i(-1);
    }
    w.i(0);
    w.i(5);
    w.i(0);
    w.i(5);
    // The shapes, and an edge among them as a terminal names it.
    if (!extraClassForAStop.isEmpty()) {
        w.i(1);
        w.i(5);
        w.bytes.append(extraClassForAStop);
    } else {
        w.i(5 + qint32(moreShapes.size()));
        w.i(5);
        // The plane, by its corners, 20 by 6 mm.
        writeShapeHead(w, 12, 10, "plane", 0, 0);
        w.i(2);
        w.value(0);
        w.value(-0.003);
        w.value(0.02, "$l");
        w.value(0.003);
        w.value(0);
        w.value(0);
        // The line, from nought to 20 mm, its width by an expression.
        writeShapeHead(w, 14, 11, "line_1", 1, 2);
        w.i(1);
        w.i(1);
        w.i(1);
        w.value(0.0004, "$w");
        w.value(0.6);
        writePoints(w, {0, 0, 0.02, 0}, false);
        writePoints(w, {}, false);
        w.by(1);
        // A pad with a void and an arc: a square with one side bowed out.
        writeShapeHead(w, 15, 12, "pad", 1, 2);
        const double big = std::numeric_limits<double>::max();
        writePoints(w, {0.021, -0.001, 0.023, -0.001, 0.023, 0.001, 0.0005, big, 0.021, 0.001}, true);
        writeShapeHead(w, 15, 13, "pad void", -1, 2, 12);
        writePoints(w, {0.0215, -0.0002, 0.0225, -0.0002, 0.0225, 0.0002, 0.0215, 0.0002}, true);
        // An edge of the line's, 0 mm along it.
        w.i(39);
        w.i(23);
        w.d(0);
        w.d(0);
        w.i(3);
        w.i(11);
        for (const QByteArray& shape : moreShapes) w.bytes.append(shape);
    }
    // The vias.
    w.i(1);
    w.i(5);
    w.i(19);
    w.i(10);
    w.i(5);
    w.i(30);
    w.i(0);
    w.i(-1);
    w.i(0);
    w.i(-1);
    w.i(viaNet);
    w.i(-1);
    w.i(4);
    w.i(0);
    w.value(0.001);
    w.value(0.00075);
    w.value(viaRotation);
    w.value(viaHole);
    w.s({});
    w.i(-1);
    w.s({});
    w.by(0);
    w.by(0);
    w.i(0);
    w.i(-1);
    w.s({});
    w.i(0);
    w.i(5);
    w.i(0);
    w.i(5);
    w.i(0);
    w.i(5);
    // The terminals: an edge port, and a point port.
    w.i(2);
    w.i(5);
    writeTerminal(w, {30, 29, 10}, 40, "P1", 1, 0, "50ohm", -1, edgePortKeeps,
                  "PortExcitationProps(DoDeembed=true, DoDeembedLength='1mm', DoRenormalize=true, RenormalizionZ0='75ohm + 0i ohm')");
    w.i(1);
    w.i(1);
    w.i(23);
    writeTerminal(w, {32, 29, 10}, 41, "Inner port", 1, 2, "50", -1, {}, "PortExcitationProps(DoDeembed=false, DoRenormalize=false)");
    w.i(2);
    w.s("f(x='22mm', y='0.5mm', r='0deg', s='1', m=false)");
    w.value(0);
    for (int k = 0; k < 3; ++k) {
        w.i(0);
        w.i(5);
    }
    w.i(-1);
    w.i(0);
    return w.bytes;
}

/// A rectangle's record by its centre, its width and its height (m), turned
/// by \a turned radians.
QByteArray centredRectangle(int id, const QByteArray& name, int layer, double x, double y, double width, double height, double turned = 0, double corner = 0, int net = 1)
{
    EdbWriter w;
    writeShapeHead(w, 12, id, name, net, layer);
    w.i(1);
    w.value(x);
    w.value(y);
    w.value(width);
    w.value(height);
    w.value(corner);
    w.value(turned);
    return w.bytes;
}

QByteArray circle(int id, const QByteArray& name, int layer, double x, double y, double radius)
{
    EdbWriter w;
    writeShapeHead(w, 13, id, name, 1, layer);
    w.value(x);
    w.value(y);
    w.value(radius);
    return w.bytes;
}

/// A file of nothing but cells of these names, each its stack and no records.
QByteArray edbCells(const QStringList& names)
{
    EdbWriter w;
    writeEdbHead(w);
    int id = 0;
    for (const QString& name : names) {
        writeBlock(w, QStringLiteral("$begin ''\n\tid=%1\n\tdn='%2'\n$end ''\n").arg(id++).arg(name).toUtf8());
        writeBlock(w, "$begin ''\n\tMode='Laminate'\n\tSLayer(Layer(N='TOP', ID=2, T='signal', Zones()), SubL(Th='50um', LElev='0mm', R='0um', Mat='copper'), UseR=false)\n$end ''\n");
        w.i(4);
        w.i(0);
        w.i(0);
        w.i(5);
    }
    return w.bytes;
}

} // namespace

class TestRf25Hfss : public QObject
{
    Q_OBJECT

private slots:
    // ------------------------------------------------------------ The export

    // A microstrip: the stack, the materials, a shape, a port, the setup and
    // the sweep, as the commands say them.
    void aMicrostripsScriptIsWhatTheGuideSays()
    {
        const Model model = modelOf(microstripStack() + QLatin1Char(',')
                                    + portsOf(QStringLiteral(R"({"type": "rf_edge_port", "tag": "p1", "label": "P1", "properties": {"conductor": "M1", "x": "0", "y": "0", "facing": "-x"}},
                                                               {"type": "rf_edge_port", "tag": "p2", "label": "P2", "properties": {"conductor": "M1", "x": "1000[um]", "y": "0", "facing": "+x"}})"))
                                    + QLatin1Char(',') + QLatin1String(kStudy));
        const HfssExport e = hfssExportOf(model, {box(QStringLiteral("M1"), 0, -31, 1000, 31)}, HfssOptions(), QStringLiteral("line"), QStringLiteral("Qucs-S test"));
        QVERIFY2(e.ok(), qPrintable(e.problems.join(QLatin1Char(' '))));
        QCOMPARE(e.ports, 2);
        QCOMPARE(e.polygons, 1);
        QCOMPARE(e.sweepData, QStringLiteral("LINC 1000000000Hz 40000000000Hz 40"));
        QVERIFY(std::abs(e.adaptive - 40e9) < 1);
        // (A quarter wave at the adaptive frequency.)
        QVERIFY(std::abs(e.airUsed - 299792458.0 / 40e9 / 4) < 1e-9);

        QTemporaryDir dir;
        const QString script = written(e, dir.path());
        if (const QString why = parsed(script); why == QLatin1String("no python3")) QSKIP("no python3 here");
        else QVERIFY2(why.isEmpty(), qPrintable(why));
        // Marks, once each at the top; Python 2 has none of what 3 alone reads.
        const QString text = e.script;
        QVERIFY(text.startsWith(QStringLiteral("# -*- coding: utf-8 -*-\n")));
        QVERIFY(text.contains(QStringLiteral("Ansys\u00ae Electronics Desktop")));
        QVERIFY(text.contains(QStringLiteral("HFSS\u2122 3D Layout")));
        QVERIFY(!text.contains(QRegularExpression(QStringLiteral("\\bf\""))));
        QVERIFY(!text.contains(QLatin1String("nonlocal")));

        const Ran ran = runScript(script, dir.path());
        QVERIFY2(ran.ok, qPrintable(ran.output));
        QVERIFY2(!ran.output.contains(QLatin1String("trouble")), qPrintable(ran.output));
        // The design: a project, the kind of design, its name.
        QCOMPARE(ran.of(QStringLiteral("oProject.InsertDesign")).first().at(0).toString(), QStringLiteral("HFSS 3D Layout Design"));
        QCOMPARE(ran.of(QStringLiteral("oDesign.SetActiveEditor")).first().at(0).toString(), QStringLiteral("Layout"));
        // The materials: each layer's own, with its numbers.
        QHash<QString, QJsonArray> materials;
        for (const QJsonArray& a : ran.of(QStringLiteral("oDefinitions.AddMaterial"))) materials.insert(a.at(0).toArray().at(0).toString(), a.at(0).toArray());
        QVERIFY(materials.contains(QStringLiteral("NAME:rf25_Si")));
        QCOMPARE(valueAfter(materials.value(QStringLiteral("NAME:rf25_Si")), QStringLiteral("permittivity:=")).toString(), QStringLiteral("11.7"));
        QCOMPARE(valueAfter(materials.value(QStringLiteral("NAME:rf25_Si")), QStringLiteral("conductivity:=")).toString(), QStringLiteral("6.5"));
        QCOMPARE(valueAfter(materials.value(QStringLiteral("NAME:rf25_PI")), QStringLiteral("dielectric_loss_tangent:=")).toString(), QStringLiteral("0.002"));
        QCOMPARE(valueAfter(materials.value(QStringLiteral("NAME:rf25_M1")), QStringLiteral("conductivity:=")).toString(), QStringLiteral("41000000"));
        // A perfect plane's conductivity is what Electronics Desktop takes as perfect.
        QCOMPARE(valueAfter(materials.value(QStringLiteral("NAME:rf25_GND")), QStringLiteral("conductivity:=")).toString(), QStringLiteral("1e+30"));

        // The stack from the bottom up, each on the one below; the metal in the
        // air above it, at the dielectrics' top.
        const QList<QJsonArray> layers = ran.of(QStringLiteral("oEditor.AddStackupLayer"));
        QCOMPARE(layers.size(), 4);
        const auto layerSays = [&layers](int k, const QString& key) { return valueAfter(layers.at(k).at(0).toArray(), key); };
        const auto subSays = [&layers](int k, const QString& key) {
            for (const QJsonValue& v : layers.at(k).at(0).toArray())
                if (v.isArray() && v.toArray().at(0).toString() == QLatin1String("NAME:Sublayer")) return valueAfter(v.toArray(), key);
            return QJsonValue();
        };
        QCOMPARE(layerSays(0, QStringLiteral("Name:=")).toString(), QStringLiteral("GND"));
        QCOMPARE(layerSays(0, QStringLiteral("Type:=")).toString(), QStringLiteral("signal"));
        QCOMPARE(subSays(0, QStringLiteral("Thickness:=")).toString(), QStringLiteral("0um"));
        QCOMPARE(layerSays(1, QStringLiteral("Name:=")).toString(), QStringLiteral("Si"));
        QCOMPARE(subSays(1, QStringLiteral("LowerElevation:=")).toString(), QStringLiteral("0um"));
        QCOMPARE(subSays(1, QStringLiteral("Thickness:=")).toString(), QStringLiteral("500um"));
        QCOMPARE(subSays(2, QStringLiteral("LowerElevation:=")).toString(), QStringLiteral("500um"));
        QCOMPARE(layerSays(3, QStringLiteral("Name:=")).toString(), QStringLiteral("M1"));
        QCOMPARE(subSays(3, QStringLiteral("LowerElevation:=")).toString(), QStringLiteral("520um"));
        QCOMPARE(subSays(3, QStringLiteral("Thickness:=")).toString(), QStringLiteral("1.5um"));
        QCOMPARE(subSays(3, QStringLiteral("FillMaterial:=")).toString(), QStringLiteral("rf25_air"));
        QCOMPARE(valueAfter(layers.at(3).at(0).toArray(), QStringLiteral("RMdl:=")).toString(), QStringLiteral("Groisse"));

        // The plane over the whole, five heights past the shape.
        const QList<QJsonArray> planes = ran.of(QStringLiteral("oEditor.CreateRectangle"));
        QCOMPARE(planes.size(), 1);
        const QJsonArray rectangle = valueAfter(planes.first().at(0).toArray(), QStringLiteral("rectGeometry:=")).toArray();
        QCOMPARE(valueAfter(rectangle, QStringLiteral("LayerName:=")).toString(), QStringLiteral("GND"));
        QCOMPARE(valueAfter(rectangle, QStringLiteral("Ax:=")).toString(), QStringLiteral("-2607.5um"));

        // The polygon: polyGeometry, as the guide does not print it.
        const QList<QJsonArray> polygons = ran.of(QStringLiteral("oEditor.CreatePolygon"));
        QCOMPARE(polygons.size(), 1);
        const QJsonArray geometry = valueAfter(polygons.first().at(0).toArray(), QStringLiteral("polyGeometry:=")).toArray();
        QCOMPARE(valueAfter(geometry, QStringLiteral("LayerName:=")).toString(), QStringLiteral("M1"));
        QCOMPARE(valueAfter(geometry, QStringLiteral("n:=")).toInt(), 4);
        QCOMPARE(valueAfter(geometry, QStringLiteral("U:=")).toString(), QStringLiteral("um"));

        // The ports: each on the edge it faces, by its number in the polygon
        // written counter-clockwise from (0, -31): -x is edge 3, +x edge 1.
        const QList<QJsonArray> ports = ran.of(QStringLiteral("oEditor.CreateEdgePort"));
        QCOMPARE(ports.size(), 2);
        const auto edgeOf = [](const QJsonArray& contents) { return valueAfter(valueAfter(contents, QStringLiteral("edge:=")).toArray(), QStringLiteral("edge:=")).toInt(); };
        QCOMPARE(edgeOf(ports.at(0).at(0).toArray()), 3);
        QCOMPARE(edgeOf(ports.at(1).at(0).toArray()), 1);
        QCOMPARE(valueAfter(ports.at(0).at(0).toArray(), QStringLiteral("circuit:=")).toBool(), false);
        // Named as the model names them, in its order.
        const QList<QJsonArray> renames = ran.of(QStringLiteral("oExcitations.Rename"));
        QCOMPARE(renames.at(0).at(1).toString(), QStringLiteral("P1"));
        QCOMPARE(renames.at(1).at(1).toString(), QStringLiteral("P2"));
        // A port that is not calibrated: a gap port, not calibrated either.
        QStringList properties;
        for (const QJsonArray& a : ran.of(QStringLiteral("oEditor.ChangeProperty"))) {
            const QJsonArray tab = a.at(0).toArray().at(1).toArray();
            if (tab.at(0).toString() != QLatin1String("NAME:EM Design")) continue;
            const QJsonArray prop = tab.at(2).toArray().at(1).toArray();
            properties << tab.at(1).toArray().at(1).toString() + QLatin1Char(' ') + prop.at(0).toString().mid(5) + QLatin1Char('=') + prop.at(2).toString();
        }
        QVERIFY2(properties.contains(QStringLiteral("Excitations:P1 HFSS Type=Gap")), qPrintable(properties.join(QLatin1Char('|'))));
        QVERIFY(properties.contains(QStringLiteral("Excitations:P1 PortSolver=false")));
        QVERIFY(properties.contains(QStringLiteral("Excitations:P2 Renormalize Impedence=50ohm + 0i ohm")));

        // The setup and its sweep.
        const QJsonArray setup = ran.of(QStringLiteral("oSetups.Add")).first().at(0).toArray();
        QCOMPARE(setup.at(0).toString(), QStringLiteral("NAME:HFSS Setup 1"));
        QCOMPARE(valueAfter(setup, QStringLiteral("SolveSetupType:=")).toString(), QStringLiteral("HFSS"));
        QCOMPARE(valueAfter(setup, QStringLiteral("Frequency:=")).toString(), QStringLiteral("40GHz"));
        const QList<QJsonArray> sweeps = ran.of(QStringLiteral("oSetups.AddSweep"));
        QCOMPARE(sweeps.first().at(0).toString(), QStringLiteral("HFSS Setup 1"));
        const QJsonArray sweep = sweeps.first().at(1).toArray();
        QCOMPARE(valueAfter(sweep, QStringLiteral("FreqSweepType:=")).toString(), QStringLiteral("kInterpolating"));
        bool data = false;
        for (const QJsonValue& v : sweep)
            if (v.isArray() && v.toArray().at(0).toString() == QLatin1String("NAME:Sweeps"))
                data = valueAfter(v.toArray(), QStringLiteral("Data:=")).toString() == QLatin1String("LINC 1000000000Hz 40000000000Hz 40");
        QVERIFY(data);
        // The extents, cut at the plane; the project saved beside it; what came
        // of it kept for whoever runs it.
        const QJsonArray extents = ran.of(QStringLiteral("oDesign.EditHfssExtents")).first().at(0).toArray();
        QCOMPARE(valueAfter(extents, QStringLiteral("TruncAtGnd:=")).toBool(), true);
        QVERIFY(ran.of(QStringLiteral("oProject.SaveAs")).first().at(0).toString().endsWith(QLatin1String("/line.aedt")));
        QVERIFY(QFileInfo::exists(QDir(dir.path()).filePath(QStringLiteral("line_aedt_report.txt"))));
        // Not solved unless asked; asked, solved and written.
        QVERIFY(ran.of(QStringLiteral("oDesign.Analyze")).isEmpty());
        const Ran solved = runScript(script, dir.path(), QStringLiteral("solve"));
        QVERIFY2(solved.ok, qPrintable(solved.output));
        QCOMPARE(solved.of(QStringLiteral("oDesign.Analyze")).size(), 1);
        const QJsonArray exported = solved.of(QStringLiteral("oDesign.ExportNetworkData")).first();
        QVERIFY(exported.at(3).toString().endsWith(QLatin1String("/line_hfss.s2p")));
        QCOMPARE(exported.at(1).toArray().at(0).toString(), QStringLiteral("HFSS Setup 1:Sweep 1"));
    }

    // A dielectric over the metal fills its room, as the solve's two faces have
    // it; a lossy plane has a skin's depth; a stack with nothing below is cut at
    // nothing and has its dielectric past the shapes.
    void whatFillsAMetalAndWhatClosesTheStack()
    {
        const QString stack = QStringLiteral(R"({"type": "rf_stack", "tag": "stack", "children": [
            {"type": "rf_ground", "tag": "g", "label": "GND", "properties": {"kind": "open"}},
            {"type": "rf_substrate", "tag": "s", "label": "Sub", "properties": {"thickness": "100[um]", "epsr": "9.8"}},
            {"type": "rf_metal", "tag": "m1", "label": "M1", "properties": {"thickness": "1[um]", "sigma": "3e7"}},
            {"type": "rf_dielectric", "tag": "d", "label": "Ox", "properties": {"thickness": "2[um]", "epsr": "4"}},
            {"type": "rf_metal", "tag": "m2", "label": "M2", "properties": {"thickness": "3[um]", "sigma": "4e7"}}]})");
        const Model model = modelOf(stack + QLatin1Char(',') + portsOf(QStringLiteral(R"({"type": "rf_edge_port", "tag": "p", "label": "P1", "properties": {"conductor": "M2", "x": "0", "y": "0", "facing": "-x", "return": "beside"}})"))
                                    + QLatin1Char(',') + QLatin1String(kStudy));
        HfssOptions how;
        how.margin = 100e-6;
        how.air = 500e-6;
        how.adaptive = 10e9;
        how.discrete = true;
        how.thinMetal = 5e-6;
        QList<Conductor> shapes = coplanar(200);
        for (Conductor& c : shapes) c.layer = QStringLiteral("M2");
        shapes << box(QStringLiteral("M1"), 0, -100, 200, 100);
        const HfssExport e = hfssExportOf(model, shapes, how, QStringLiteral("two"));
        QVERIFY2(e.ok(), qPrintable(e.problems.join(QLatin1Char(' '))));
        QTemporaryDir dir;
        const Ran ran = runScript(written(e, dir.path()), dir.path());
        if (!ran.ok && ran.calls.isEmpty() && ran.output.isEmpty()) QSKIP("no python3 here");
        QVERIFY2(ran.ok, qPrintable(ran.output));
        const QList<QJsonArray> layers = ran.of(QStringLiteral("oEditor.AddStackupLayer"));
        QStringList said;
        for (const QJsonArray& a : layers) {
            const QJsonArray layer = a.at(0).toArray();
            QJsonArray sub;
            for (const QJsonValue& v : layer)
                if (v.isArray() && v.toArray().at(0).toString() == QLatin1String("NAME:Sublayer")) sub = v.toArray();
            said << valueAfter(layer, QStringLiteral("Name:=")).toString() + QLatin1Char('@') + valueAfter(sub, QStringLiteral("LowerElevation:=")).toString() + QLatin1Char('+')
                        + valueAfter(sub, QStringLiteral("Thickness:=")).toString() + QLatin1Char(' ') + valueAfter(sub, QStringLiteral("FillMaterial:=")).toString();
        }
        QCOMPARE(said, QStringList({QStringLiteral("Sub@0um+100um "), QStringLiteral("M1@100um+1um rf25_Ox"), QStringLiteral("Ox@101um+2um "), QStringLiteral("M2@103um+3um rf25_air")}));
        // No plane: nothing truncated, the dielectric past the shapes by the margin.
        const QJsonArray extents = ran.of(QStringLiteral("oDesign.EditHfssExtents")).first().at(0).toArray();
        QCOMPARE(valueAfter(extents, QStringLiteral("TruncAtGnd:=")).toBool(), false);
        QCOMPARE(valueAfter(valueAfter(extents, QStringLiteral("DielExt:=")).toArray(), QStringLiteral("Ext:=")).toString(), QStringLiteral("100um"));
        QCOMPARE(valueAfter(valueAfter(extents, QStringLiteral("AirPosZExt:=")).toArray(), QStringLiteral("Ext:=")).toString(), QStringLiteral("500um"));
        QVERIFY(ran.of(QStringLiteral("oEditor.CreateRectangle")).isEmpty());
        // A discrete sweep, thin metals as sheets, its adaptive frequency as asked.
        QCOMPARE(valueAfter(ran.of(QStringLiteral("oSetups.AddSweep")).first().at(1).toArray(), QStringLiteral("FreqSweepType:=")).toString(), QStringLiteral("kDiscrete"));
        const QJsonArray setup = ran.of(QStringLiteral("oSetups.Add")).first().at(0).toArray();
        QCOMPARE(valueAfter(setup, QStringLiteral("Frequency:=")).toString(), QStringLiteral("10GHz"));
        QJsonArray adaptiveData;
        for (const QJsonValue& v : setup)
            if (v.isArray() && v.toArray().at(0).toString() == QLatin1String("NAME:AdaptiveSettings"))
                for (const QJsonValue& list : v.toArray())
                    if (list.isArray() && list.toArray().at(0).toString() == QLatin1String("NAME:SingleFrequencyDataList")) adaptiveData = list.toArray().at(1).toArray();
        QCOMPARE(valueAfter(adaptiveData, QStringLiteral("AdaptiveFrequency:=")).toString(), QStringLiteral("10GHz"));
        QCOMPARE(valueAfter(adaptiveData, QStringLiteral("MaxDelta:=")).toString(), QStringLiteral("0.02"));
        QCOMPARE(valueAfter(adaptiveData, QStringLiteral("MaxPasses:=")).toInt(), 20);
        bool thin = false;
        for (const QJsonValue& v : setup)
            if (v.isArray() && v.toArray().at(0).toString() == QLatin1String("NAME:AdvancedSettings"))
                thin = valueAfter(v.toArray(), QStringLiteral("ZeroMetalLayerThickness:=")).toString() == QLatin1String("5um");
        QVERIFY(thin);
        // A coplanar port: its signal's edge, and each ground's on the line of it.
        const QList<QJsonArray> references = ran.of(QStringLiteral("oEditor.AddRefPort"));
        QCOMPARE(references.size(), 1);
        QCOMPARE(references.first().at(0).toArray().at(0).toString(), QStringLiteral("P1"));
        const QJsonArray contents = references.first().at(1).toArray();
        QStringList primitives;
        for (qsizetype k = 0; k + 1 < contents.size(); ++k)
            if (contents.at(k).toString() == QLatin1String("edge:=")) primitives << valueAfter(contents.at(k + 1).toArray(), QStringLiteral("prim:=")).toString();
        primitives.sort();
        QCOMPARE(primitives, QStringList({QStringLiteral("M2_2"), QStringLiteral("M2_3")}));
    }

    // A plane of a metal or of an impedance, and a lid: each written as what
    // gives the same thing there, and said. A log sweep's points each its own.
    void aPlaneOrALidIsWhatGivesTheSameThere()
    {
        const auto stackOver = [](const QString& ground, const QString& cover = {}) {
            return QStringLiteral(R"({"type": "rf_stack", "tag": "stack", "children": [
                {"type": "rf_ground", "tag": "g", "label": "GND", "properties": %1},
                {"type": "rf_substrate", "tag": "s", "label": "Sub", "properties": {"thickness": "100[um]", "epsr": "9.8"}},
                {"type": "rf_metal", "tag": "m", "label": "M1", "properties": {"thickness": "2[um]", "sigma": "3e7"}}%2]})").arg(ground, cover);
        };
        const QString port = portsOf(QStringLiteral(R"({"type": "rf_edge_port", "tag": "p", "label": "P1", "properties": {"conductor": "M1", "x": "0", "y": "0", "facing": "-x"}})"));
        const auto exportOf = [&port](const QString& stack, const char* study = kStudy) {
            return hfssExportOf(modelOf(stack + QLatin1Char(',') + port + QLatin1Char(',') + QLatin1String(study)), {box(QStringLiteral("M1"), 0, -31, 1000, 31)});
        };
        QTemporaryDir dir;
        const auto ranOf = [&dir](const HfssExport& e) { return runScript(written(e, dir.path()), dir.path()); };
        const auto layerOf = [](const Ran& ran, const QString& name) {
            for (const QJsonArray& a : ran.of(QStringLiteral("oEditor.AddStackupLayer"))) {
                const QJsonArray layer = a.at(0).toArray();
                if (valueAfter(layer, QStringLiteral("Name:=")).toString() != name) continue;
                for (const QJsonValue& v : layer)
                    if (v.isArray() && v.toArray().at(0).toString() == QLatin1String("NAME:Sublayer")) return v.toArray();
            }
            return QJsonArray();
        };
        const auto conductivityOf = [](const Ran& ran, const QString& material) {
            for (const QJsonArray& a : ran.of(QStringLiteral("oDefinitions.AddMaterial")))
                if (a.at(0).toArray().at(0).toString() == QStringLiteral("NAME:") + material) return valueAfter(a.at(0).toArray(), QStringLiteral("conductivity:=")).toString().toDouble();
            return -1.0;
        };

        // A metal: five skin depths at the bottom of the sweep, of its conductivity.
        const HfssExport metal = exportOf(stackOver(QStringLiteral(R"({"kind": "metal", "sigma": "1e7"})")));
        QVERIFY2(metal.ok(), qPrintable(metal.problems.join(QLatin1Char(' '))));
        const Ran metalRan = ranOf(metal);
        if (!metalRan.ok && metalRan.calls.isEmpty() && metalRan.output.isEmpty()) QSKIP("no python3 here");
        QVERIFY2(metalRan.ok, qPrintable(metalRan.output));
        const double skin = std::sqrt(1 / (M_PI * 1e9 * 4e-7 * M_PI * 1e7));
        const QJsonArray plane = layerOf(metalRan, QStringLiteral("GND"));
        QVERIFY(std::abs(valueAfter(plane, QStringLiteral("Thickness:=")).toString().chopped(2).toDouble() - 5 * skin * 1e6) < 1e-6);
        QCOMPARE(conductivityOf(metalRan, QStringLiteral("rf25_GND")), 1e7);
        QVERIFY(metal.notes.join(QLatin1Char('\n')).contains(QLatin1String("five skin depths")));
        // What stands on it, as high as it is thick.
        QVERIFY(std::abs(valueAfter(layerOf(metalRan, QStringLiteral("Sub")), QStringLiteral("LowerElevation:=")).toString().chopped(2).toDouble() - 5 * skin * 1e6) < 1e-6);

        // An impedance: a sheet 1 µm thick of the conductivity that gives it.
        const HfssExport sheet = exportOf(stackOver(QStringLiteral(R"({"kind": "impedance", "impedance": "0.5"})")));
        QVERIFY2(sheet.ok(), qPrintable(sheet.problems.join(QLatin1Char(' '))));
        const Ran sheetRan = ranOf(sheet);
        QVERIFY2(sheetRan.ok, qPrintable(sheetRan.output));
        QCOMPARE(valueAfter(layerOf(sheetRan, QStringLiteral("GND")), QStringLiteral("Thickness:=")).toString(), QStringLiteral("1um"));
        QVERIFY(std::abs(conductivityOf(sheetRan, QStringLiteral("rf25_GND")) - 1 / (0.5 * 1e-6)) < 1e-3);
        QVERIFY(sheet.notes.join(QLatin1Char('\n')).contains(QLatin1String("a surface impedance of 0.5 Ohm")));

        // A lid: the air up to it, a plane past the shapes, and said to have no
        // answer here. (RF 2.5D refuses to solve one, not to write it.)
        const HfssExport lid = exportOf(stackOver(QStringLiteral(R"({"kind": "perfect"})"), QStringLiteral(R"(, {"type": "rf_cover", "tag": "c", "label": "Lid", "properties": {"kind": "perfect", "height": "300[um]"}})")));
        QVERIFY2(lid.ok(), qPrintable(lid.problems.join(QLatin1Char(' '))));
        const Ran lidRan = ranOf(lid);
        QVERIFY2(lidRan.ok, qPrintable(lidRan.output));
        QCOMPARE(lid.planes, 2);
        QVERIFY(std::abs(valueAfter(layerOf(lidRan, QStringLiteral("Lid_gap")), QStringLiteral("Thickness:=")).toString().chopped(2).toDouble() - 300) < 1e-6);
        QVERIFY(std::abs(valueAfter(layerOf(lidRan, QStringLiteral("Lid")), QStringLiteral("LowerElevation:=")).toString().chopped(2).toDouble() - 402) < 1e-6);
        QCOMPARE(ranOf(lid).of(QStringLiteral("oEditor.CreateRectangle")).size(), 2);
        QVERIFY(lid.notes.join(QLatin1Char('\n')).contains(QLatin1String("Lid: a metal lid, which RF 2.5D does not solve yet")));

        // A log sweep: each of its points, as a linear step cannot say them.
        const HfssExport logSweep = exportOf(stackOver(QStringLiteral(R"({"kind": "perfect"})")),
                                             R"({"type": "rf_study", "tag": "study", "properties": {"kind": "full_wave", "scale": "log", "from": "1[GHz]", "to": "100[GHz]", "points": "3"}})");
        QVERIFY2(logSweep.ok(), qPrintable(logSweep.problems.join(QLatin1Char(' '))));
        QCOMPARE(logSweep.sweepData, QStringLiteral("SINGLE 1000000000Hz SINGLE 10000000000Hz SINGLE 100000000000Hz"));
    }

    // Every kind of port and what is said of it where the two solvers differ:
    // wave for a calibrated one with its de-embedding, a circuit port with its
    // returns for a port that names them, a point for an internal one.
    void everyKindOfPort()
    {
        const QString ports = QStringLiteral(R"(
            {"type": "rf_terminal", "tag": "t1", "label": "T1", "properties": {"metal": "M1", "x": "0", "y": "60[um]", "width": "20[um]", "height": "20[um]"}},
            {"type": "rf_terminal", "tag": "t2", "label": "T2", "properties": {"metal": "M1", "x": "0", "y": "-60[um]", "face": "-x"}},
            {"type": "rf_edge_port", "tag": "p1", "label": "P1", "properties": {"conductor": "M1", "x": "0", "y": "0", "facing": "-x", "calibrate": true, "deembed": "50[um]", "impedance": "75"}},
            {"type": "rf_edge_port", "tag": "p2", "label": "P2", "properties": {"conductor": "M1", "x": "200[um]", "y": "0", "facing": "+x", "return": "terminals", "returns": ["T1", "T2"]}},
            {"type": "rf_internal_port", "tag": "p3", "label": "P3", "properties": {"x": "100[um]", "y": "0", "to": "M1", "from": "ground", "width": "10[um]", "height": "10[um]"}},
            {"type": "rf_internal_port", "tag": "p4", "label": "P4", "properties": {"x": "100[um]", "y": "10[um]", "to": "M1", "return": "terminals", "returns": ["T1"]}})");
        const Model model = modelOf(microstripStack() + QLatin1Char(',') + portsOf(ports) + QLatin1Char(',') + QLatin1String(kStudy));
        const HfssExport e = hfssExportOf(model, coplanar(200));
        QVERIFY2(e.ok(), qPrintable(e.problems.join(QLatin1Char(' '))));
        QCOMPARE(e.ports, 4);
        QTemporaryDir dir;
        const Ran ran = runScript(written(e, dir.path()), dir.path());
        if (!ran.ok && ran.calls.isEmpty() && ran.output.isEmpty()) QSKIP("no python3 here");
        QVERIFY2(ran.ok, qPrintable(ran.output));
        QVERIFY2(!ran.output.contains(QLatin1String("trouble")), qPrintable(ran.output));
        QCOMPARE(ran.of(QStringLiteral("oEditor.CreateEdgePort")).size(), 3);
        QCOMPARE(ran.of(QStringLiteral("oEditor.CreateCircuitPort")).size(), 1);
        // The point port: its metal against the ground, at its point.
        const QJsonArray location = ran.of(QStringLiteral("oEditor.CreateCircuitPort")).first().at(0).toArray();
        QCOMPARE(valueAfter(location, QStringLiteral("PosLayer:=")).toString(), QStringLiteral("M1"));
        QCOMPARE(valueAfter(location, QStringLiteral("NegLayer:=")).toString(), QStringLiteral("GND"));
        QCOMPARE(valueAfter(location, QStringLiteral("X0:=")).toString(), QStringLiteral("100um"));
        // The wave port, de-embedded, into its own reference.
        QStringList properties;
        for (const QJsonArray& a : ran.of(QStringLiteral("oEditor.ChangeProperty"))) {
            const QJsonArray tab = a.at(0).toArray().at(1).toArray();
            if (tab.at(0).toString() != QLatin1String("NAME:EM Design")) continue;
            const QJsonArray prop = tab.at(2).toArray().at(1).toArray();
            properties << tab.at(1).toArray().at(1).toString() + QLatin1Char(' ') + prop.at(0).toString().mid(5) + QLatin1Char('=') + prop.at(2).toString();
        }
        QVERIFY2(properties.contains(QStringLiteral("Excitations:P1 HFSS Type=Wave")), qPrintable(properties.join(QLatin1Char('|'))));
        QVERIFY(properties.contains(QStringLiteral("Excitations:P1 Deembed Distance=50um")));
        QVERIFY(properties.contains(QStringLiteral("Excitations:P1 Renormalize Impedence=75ohm + 0i ohm")));
        // Those that name their returns: circuit ports, each return an edge, tied.
        const QList<QJsonArray> tied = ran.of(QStringLiteral("oEditor.AddCircuitRefPort"));
        QCOMPARE(tied.size(), 2);
        QCOMPARE(tied.at(0).at(0).toArray().at(0).toString(), QStringLiteral("P2"));
        int edges = 0;
        const QJsonArray contents = tied.at(0).at(1).toArray();
        for (const QJsonValue& v : contents) edges += v.toString() == QLatin1String("edge:=");
        QCOMPARE(edges, 2);
        QCOMPARE(valueAfter(ran.of(QStringLiteral("oEditor.CreateEdgePort")).at(1).at(0).toArray(), QStringLiteral("circuit:=")).toBool(), true);
        // And each difference said, in the answer and in the script's head.
        const QString notes = e.notes.join(QLatin1Char('\n'));
        QVERIFY2(notes.contains(QLatin1String("P2: its returns are the edges nearest each terminal")), qPrintable(notes));
        QVERIFY(notes.contains(QLatin1String("P3: RF 2.5D drives the cells of a rectangle")));
        QVERIFY(notes.contains(QLatin1String("P4: its own cells")));
        QVERIFY(e.script.contains(QLatin1String("#   - P3: RF 2.5D drives the cells of a rectangle")));
    }

    // A port's edge goes by the number Electronics Desktop reads its polygon
    // with: where the first point is another, where it closes the outline, and
    // where no edge is where it was written, which is said.
    void anEdgeIsNumberedAsItsPolygonReadsBack()
    {
        const QString ports = QStringLiteral(R"(
            {"type": "rf_edge_port", "tag": "p1", "label": "P1", "properties": {"conductor": "M1", "x": "0", "y": "0", "facing": "-x"}})");
        const Model model = modelOf(microstripStack() + QLatin1Char(',') + portsOf(ports) + QLatin1Char(',') + QLatin1String(kStudy));
        const HfssExport e = hfssExportOf(model, {box(QStringLiteral("M1"), 0, -31, 1000, 31)});
        QVERIFY2(e.ok(), qPrintable(e.problems.join(QLatin1Char(' '))));
        QTemporaryDir dir;
        const QString script = written(e, dir.path());
        const auto edgeRun = [&script, &dir](const QString& read, QString* output) {
            const Ran ran = runScript(script, dir.path(), {}, read);
            *output = ran.output;
            if (!ran.ok) return -2;
            const QList<QJsonArray> made = ran.of(QStringLiteral("oEditor.CreateEdgePort"));
            if (made.size() != 1) return -3;
            return valueAfter(valueAfter(made.first().at(0).toArray(), QStringLiteral("edge:=")).toArray(), QStringLiteral("edge:=")).toInt(-4);
        };
        QString output;
        int edge = edgeRun({}, &output);
        if (edge == -2 && output.isEmpty()) QSKIP("no python3 here");
        // (From (0, 31) to (0, -31): the fourth side of the box as written.)
        QCOMPARE(edge, 3);
        QVERIFY2(output.isEmpty(), qPrintable(output));
        QCOMPARE(edgeRun(QStringLiteral("closed"), &output), 3);
        QVERIFY2(output.isEmpty(), qPrintable(output));
        QCOMPARE(edgeRun(QStringLiteral("rotated"), &output), 2);
        QVERIFY2(output.isEmpty(), qPrintable(output));
        QCOMPARE(edgeRun(QStringLiteral("moved"), &output), 3);
        QVERIFY2(output.contains(QLatin1String("trouble: RF 2.5D: ")) && output.contains(QLatin1String("no edge of it reads back")), qPrintable(output));

        // An outline given clockwise is written counter-clockwise, as the numbers
        // the edges go by are of that.
        Conductor clockwise;
        clockwise.layer = QStringLiteral("M1");
        clockwise.outline = QPolygonF(QList<QPointF>{{0, -31}, {0, 31}, {1000, 31}, {1000, -31}});
        const HfssExport other = hfssExportOf(model, {clockwise});
        QVERIFY2(other.ok(), qPrintable(other.problems.join(QLatin1Char(' '))));
        const Ran ran = runScript(written(other, dir.path()), dir.path());
        QVERIFY2(ran.ok, qPrintable(ran.output));
        const QJsonArray geometry = valueAfter(ran.of(QStringLiteral("oEditor.CreatePolygon")).first().at(0).toArray(), QStringLiteral("polyGeometry:=")).toArray();
        QPolygonF drawn;
        QList<double> xs, ys;
        for (qsizetype k = 0; k + 1 < geometry.size(); ++k) {
            if (geometry.at(k).toString() == QLatin1String("x:=")) xs << geometry.at(k + 1).toDouble();
            if (geometry.at(k).toString() == QLatin1String("y:=")) ys << geometry.at(k + 1).toDouble();
        }
        QCOMPARE(xs.size(), 4);
        QCOMPARE(ys.size(), 4);
        double doubled = 0;
        for (int k = 0; k < 4; ++k) doubled += xs.at(k) * ys.at((k + 1) % 4) - xs.at((k + 1) % 4) * ys.at(k);
        QVERIFY2(doubled > 0, qPrintable(QString::number(doubled)));
    }

    // A hole kept as a keyhole, as gdstk's union leaves it, is a void of its
    // own: one outline counter-clockwise, its hole, and its port on the outline.
    void aKeyholeIsAVoid()
    {
        const QString m = QStringLiteral("M1");
        const QList<QPolygonF> joined = unionOf({QPolygonF(QList<QPointF>{{0, 0}, {100, 0}, {100, 40}, {0, 40}}), QPolygonF(QList<QPointF>{{0, 60}, {100, 60}, {100, 100}, {0, 100}}),
                                                 QPolygonF(QList<QPointF>{{0, 0}, {40, 0}, {40, 100}, {0, 100}}), QPolygonF(QList<QPointF>{{60, 0}, {100, 0}, {100, 100}, {60, 100}})});
        QCOMPARE(joined.size(), 1);
        Conductor ring;
        ring.layer = m;
        ring.outline = joined.first();
        const Model model = modelOf(microstripStack() + QLatin1Char(',')
                                    + portsOf(QStringLiteral(R"({"type": "rf_edge_port", "tag": "p", "label": "P1", "properties": {"conductor": "M1", "x": "0", "y": "50[um]", "facing": "-x"}})"))
                                    + QLatin1Char(',') + QLatin1String(kStudy));
        const HfssExport e = hfssExportOf(model, {ring});
        QVERIFY2(e.ok(), qPrintable(e.problems.join(QLatin1Char(' '))));
        QCOMPARE(e.polygons, 1);
        QCOMPARE(e.voids, 1);
        QTemporaryDir dir;
        const Ran ran = runScript(written(e, dir.path()), dir.path());
        if (!ran.ok && ran.calls.isEmpty() && ran.output.isEmpty()) QSKIP("no python3 here");
        QVERIFY2(ran.ok, qPrintable(ran.output));
        QCOMPARE(ran.of(QStringLiteral("oEditor.CreatePolygon")).size(), 1);
        const QJsonArray void0 = ran.of(QStringLiteral("oEditor.CreatePolygonVoid")).first().at(0).toArray();
        QCOMPARE(valueAfter(void0, QStringLiteral("owner:=")).toString(), QStringLiteral("M1_1"));
        const QJsonArray geometry = valueAfter(void0, QStringLiteral("poly voidGeometry:=")).toArray();
        QCOMPARE(valueAfter(geometry, QStringLiteral("n:=")).toInt(), 4);
        // The GDSII file has the keyhole: it is how the format holds a hole.
        QCOMPARE(e.gdsShapes.first().second.size(), ring.outline.size());

        // Several: as a plane with its via's clearances comes out of the union, and
        // a slit from one hole on to the next. Each a void, none left in the outline.
        QList<QPolygonF> strips;
        for (int k = 0; k <= 4; ++k) strips << QPolygonF(QList<QPointF>{{k * 40.0, 0}, {k * 40.0 + 10, 0}, {k * 40.0 + 10, 50}, {k * 40.0, 50}});
        strips << QPolygonF(QList<QPointF>{{0, 0}, {170, 0}, {170, 10}, {0, 10}}) << QPolygonF(QList<QPointF>{{0, 40}, {170, 40}, {170, 50}, {0, 50}});
        const QList<QPolygonF> grid = unionOf(strips);
        QCOMPARE(grid.size(), 1);
        Conductor plane;
        plane.layer = m;
        plane.outline = grid.first();
        const HfssExport four = hfssExportOf(model, {plane});
        QVERIFY2(four.ok(), qPrintable(four.problems.join(QLatin1Char(' '))));
        QCOMPARE(four.voids, 4);
        const Ran ranFour = runScript(written(four, dir.path()), dir.path());
        QVERIFY2(ranFour.ok, qPrintable(ranFour.output));
        QCOMPARE(ran.of(QStringLiteral("oEditor.CreatePolygon")).size(), 1);
        // (Its four corners, and where a slit met it, in line.)
        QVERIFY(valueAfter(valueAfter(ranFour.of(QStringLiteral("oEditor.CreatePolygon")).first().at(0).toArray(), QStringLiteral("polyGeometry:=")).toArray(), QStringLiteral("n:=")).toInt() <= 5);
        for (const QJsonArray& a : ranFour.of(QStringLiteral("oEditor.CreatePolygonVoid")))
            QCOMPARE(valueAfter(valueAfter(a.at(0).toArray(), QStringLiteral("poly voidGeometry:=")).toArray(), QStringLiteral("n:=")).toInt(), 4);

        // A slit from a hole to the next, by hand, whatever a union does today.
        Conductor chained;
        chained.layer = m;
        chained.outline = QPolygonF(QList<QPointF>{{0, 0}, {100, 0}, {100, 100}, {0, 100}, {0, 50}, {20, 50}, {20, 70}, {40, 70}, {40, 50}, {60, 50}, {60, 70}, {80, 70}, {80, 50}, {60, 50}, {40, 50}, {20, 50}, {0, 50}});
        const HfssExport two = hfssExportOf(model, {chained});
        QVERIFY2(two.ok(), qPrintable(two.problems.join(QLatin1Char(' '))));
        QCOMPARE(two.voids, 2);

        // Two lobes on a slit, both the same way round, are no hole: as it is. (The
        // smaller taken for a hole would take metal away.)
        Conductor bridged;
        bridged.layer = m;
        bridged.outline = QPolygonF(QList<QPointF>{{0, 0}, {10, 0}, {10, 5}, {20, 5}, {20, 0}, {30, 0}, {30, 10}, {20, 10}, {20, 5}, {10, 5}, {10, 10}, {0, 10}});
        const HfssExport lobes = hfssExportOf(model, {bridged});
        QVERIFY2(lobes.ok(), qPrintable(lobes.problems.join(QLatin1Char(' '))));
        QCOMPARE(lobes.voids, 0);
        QCOMPARE(lobes.polygons, 1);
    }

    // What stands in a void of a plane, within its outline, is no part of it: a
    // via's pad in its clearance has the via's net, and the plane its own.
    void whatStandsInAVoidIsNoPartOfIt()
    {
        const QString stack = QStringLiteral(R"({"type": "rf_stack", "tag": "stack", "children": [
            {"type": "rf_ground", "tag": "g", "label": "GND", "properties": {"kind": "open"}},
            {"type": "rf_substrate", "tag": "s", "label": "Sub", "properties": {"thickness": "100[um]", "epsr": "9.8"}},
            {"type": "rf_metal", "tag": "m1", "label": "M1", "properties": {"thickness": "1[um]", "sigma": "3e7"}},
            {"type": "rf_dielectric", "tag": "d", "label": "Ox", "properties": {"thickness": "2[um]", "epsr": "4"}},
            {"type": "rf_metal", "tag": "m2", "label": "M2", "properties": {"thickness": "3[um]", "sigma": "4e7"}},
            {"type": "rf_via", "tag": "v1", "label": "V1", "properties": {"from": "M1", "to": "M2", "sigma": "2e7"}}]})");
        const Model model = modelOf(stack + QLatin1Char(',') + portsOf(QStringLiteral(R"({"type": "rf_edge_port", "tag": "p", "label": "P1", "properties": {"conductor": "M2", "x": "0", "y": "0", "facing": "-x"}})"))
                                    + QLatin1Char(',') + QLatin1String(kStudy));
        // A plane on M1 with a clearance, keyholed as the union leaves it; in it the
        // pad; on the pad the via; on the via the line on M2.
        const QList<QPolygonF> ring = unionOf({QPolygonF(QList<QPointF>{{0, 0}, {200, 0}, {200, 40}, {0, 40}}), QPolygonF(QList<QPointF>{{0, 60}, {200, 60}, {200, 100}, {0, 100}}),
                                               QPolygonF(QList<QPointF>{{0, 0}, {90, 0}, {90, 100}, {0, 100}}), QPolygonF(QList<QPointF>{{110, 0}, {200, 0}, {200, 100}, {110, 100}})});
        QCOMPARE(ring.size(), 1);
        Conductor plane;
        plane.layer = QStringLiteral("M1");
        plane.outline = ring.first();
        const QList<Conductor> shapes{plane, box(QStringLiteral("M1"), 95, 45, 105, 55), box(QStringLiteral("V1"), 97, 47, 103, 53), box(QStringLiteral("M2"), 0, 45, 105, 55)};
        const HfssExport e = hfssExportOf(model, shapes);
        QVERIFY2(e.ok(), qPrintable(e.problems.join(QLatin1Char(' '))));
        QCOMPARE(e.voids, 1);
        QTemporaryDir dir;
        const Ran ran = runScript(written(e, dir.path()), dir.path());
        if (!ran.ok && ran.calls.isEmpty() && ran.output.isEmpty()) QSKIP("no python3 here");
        QVERIFY2(ran.ok, qPrintable(ran.output));
        QHash<QString, QStringList> nets;
        for (const QJsonArray& a : ran.of(QStringLiteral("oEditor.ChangeProperty"))) {
            const QJsonArray tab = a.at(0).toArray().at(1).toArray();
            if (tab.at(0).toString() != QLatin1String("NAME:BaseElementTab")) continue;
            const QJsonArray servers = tab.at(1).toArray();
            for (qsizetype k = 1; k < servers.size(); ++k) nets[tab.at(2).toArray().at(1).toArray().at(2).toString()] << servers.at(k).toString();
        }
        QStringList sizes;
        for (const QStringList& members : std::as_const(nets)) sizes << QString::number(members.size());
        sizes.sort();
        // (The plane alone; the pad, the via and the line.)
        QCOMPARE(sizes, QStringList({QStringLiteral("1"), QStringLiteral("3")}));
    }

    // A via: a rectangle as it is, anything else a circle of its area; nets by
    // what touches and what a via joins; a via down to the plane.
    void viasAndNets()
    {
        const QString stack = QStringLiteral(R"({"type": "rf_stack", "tag": "stack", "children": [
            {"type": "rf_ground", "tag": "g", "label": "GND", "properties": {"kind": "perfect"}},
            {"type": "rf_substrate", "tag": "s", "label": "Sub", "properties": {"thickness": "100[um]", "epsr": "9.8"}},
            {"type": "rf_metal", "tag": "m1", "label": "M1", "properties": {"thickness": "1[um]", "sigma": "3e7"}},
            {"type": "rf_dielectric", "tag": "d", "label": "Ox", "properties": {"thickness": "2[um]", "epsr": "4"}},
            {"type": "rf_metal", "tag": "m2", "label": "M2", "properties": {"thickness": "3[um]", "sigma": "4e7"}},
            {"type": "rf_via", "tag": "v1", "label": "V1", "properties": {"from": "M1", "to": "M2", "sigma": "2e7"}},
            {"type": "rf_via", "tag": "v0", "label": "V0", "properties": {"from": "ground", "to": "M1", "sigma": "1e7"}}]})");
        const Model model = modelOf(stack + QLatin1Char(',') + portsOf(QStringLiteral(R"({"type": "rf_edge_port", "tag": "p", "label": "P1", "properties": {"conductor": "M2", "x": "0", "y": "0", "facing": "-x"}})"))
                                    + QLatin1Char(',') + QLatin1String(kStudy));
        const QString m1 = QStringLiteral("M1"), m2 = QStringLiteral("M2");
        Conductor octagon;
        octagon.layer = QStringLiteral("V1");
        for (int k = 0; k < 8; ++k) octagon.outline << QPointF(150 + 4 * std::cos(k * M_PI / 4), 4 * std::sin(k * M_PI / 4));
        const QList<Conductor> shapes{box(m2, 0, -5, 100, 5),   box(m2, 100, -5, 200, 5),            // (butted: one net)
                                      box(m1, 140, -10, 160, 10), box(m1, 0, 100, 50, 150),          // (the second floats)
                                      octagon,                  box(QStringLiteral("V0"), 145, -2, 149, 2),
                                      box(QStringLiteral("V0"), 145, 4, 149, 8)};                    // (its like: one padstack for the two)
        const HfssExport e = hfssExportOf(model, shapes);
        QVERIFY2(e.ok(), qPrintable(e.problems.join(QLatin1Char(' '))));
        QCOMPARE(e.vias, 3);
        // In the GDSII file each via on a layer of its own, past the metals'.
        QSet<int> octagonLayers, boxLayers, metalLayers;
        for (const auto& [number, polygon] : e.gdsShapes) {
            const QRectF bound = polygon.boundingRect();
            if (polygon.size() == 8) octagonLayers << number;
            else if (std::abs(bound.width() - 4) < 1e-9 && std::abs(bound.height() - 4) < 1e-9) boxLayers << number;
            else metalLayers << number;
        }
        QCOMPARE(octagonLayers.size(), 1);
        QCOMPARE(boxLayers.size(), 1);
        QVERIFY(octagonLayers != boxLayers);
        QVERIFY(!metalLayers.intersects(octagonLayers));
        QVERIFY(!metalLayers.intersects(boxLayers));
        QVERIFY(e.notes.join(QLatin1Char('\n')).contains(QLatin1String("1 via shape(s) that are no rectangles")));
        QTemporaryDir dir;
        const Ran ran = runScript(written(e, dir.path()), dir.path());
        if (!ran.ok && ran.calls.isEmpty() && ran.output.isEmpty()) QSKIP("no python3 here");
        QVERIFY2(ran.ok, qPrintable(ran.output));
        const QList<QJsonArray> padstacks = ran.of(QStringLiteral("oPadstacks.Add"));
        QCOMPARE(padstacks.size(), 2);
        QJsonArray psd = padstacks.first().at(0).toArray();
        QJsonArray body;
        for (const QJsonValue& v : psd)
            if (v.isArray() && v.toArray().at(0).toString() == QLatin1String("NAME:psd")) body = v.toArray();
        const QJsonArray hole = valueAfter(body, QStringLiteral("hle:=")).toArray();
        QCOMPARE(valueAfter(hole, QStringLiteral("shp:=")).toString(), QStringLiteral("Cir"));
        // The octagon's area: 2 sqrt(2) 4^2; a circle of it.
        const double diameter = 2 * std::sqrt(2 * M_SQRT2 * 16 / M_PI);
        QVERIFY(std::abs(valueAfter(hole, QStringLiteral("Szs:=")).toArray().at(0).toString().chopped(2).toDouble() - diameter) < 1e-6);
        QCOMPARE(valueAfter(body, QStringLiteral("plt:=")).toString(), QStringLiteral("100"));
        QJsonArray psd2 = padstacks.at(1).at(0).toArray();
        QJsonArray body2;
        for (const QJsonValue& v : psd2)
            if (v.isArray() && v.toArray().at(0).toString() == QLatin1String("NAME:psd")) body2 = v.toArray();
        QCOMPARE(valueAfter(valueAfter(body2, QStringLiteral("hle:=")).toArray(), QStringLiteral("shp:=")).toString(), QStringLiteral("Rct"));
        const QList<QJsonArray> vias = ran.of(QStringLiteral("oEditor.CreateVia"));
        QCOMPARE(vias.size(), 3);
        QCOMPARE(valueAfter(vias.at(0).at(0).toArray(), QStringLiteral("highest_layer:=")).toString(), m2);
        QCOMPARE(valueAfter(vias.at(0).at(0).toArray(), QStringLiteral("lowest_layer:=")).toString(), m1);
        QCOMPARE(valueAfter(vias.at(1).at(0).toArray(), QStringLiteral("lowest_layer:=")).toString(), QStringLiteral("GND"));
        // The nets: the two butted, the via, the pad and the plane are one, GND;
        // the floating shape its own.
        QHash<QString, QStringList> nets;
        for (const QJsonArray& a : ran.of(QStringLiteral("oEditor.ChangeProperty"))) {
            const QJsonArray tab = a.at(0).toArray().at(1).toArray();
            if (tab.at(0).toString() != QLatin1String("NAME:BaseElementTab")) continue;
            const QJsonArray servers = tab.at(1).toArray();
            QCOMPARE(tab.at(2).toArray().at(1).toArray().at(0).toString(), QStringLiteral("NAME:Net"));
            const QString net = tab.at(2).toArray().at(1).toArray().at(2).toString();
            for (qsizetype k = 1; k < servers.size(); ++k) nets[net] << servers.at(k).toString();
        }
        QCOMPARE(nets.size(), 2);
        QVERIFY(nets.contains(QStringLiteral("GND")));
        QCOMPARE(nets.value(QStringLiteral("GND")).size(), 7);   // (the plane, two lines, a pad, three vias)
    }

    // ------------------------------------------------------------ The control file

    // Well formed, as Ansys writes one; its stack and ports as the model says.
    void theControlFileIsWhatTheHelpSays()
    {
        const Model model = modelOf(microstripStack() + QLatin1Char(',')
                                    + portsOf(QStringLiteral(R"({"type": "rf_edge_port", "tag": "p1", "label": "P1", "properties": {"conductor": "M1", "x": "0", "y": "0", "facing": "-x", "impedance": "75"}},
                                                               {"type": "rf_edge_port", "tag": "p2", "label": "P2", "properties": {"conductor": "M1", "x": "1000[um]", "y": "0", "facing": "+x", "return": "beside"}})"))
                                    + QLatin1Char(',') + QLatin1String(kStudy));
        const HfssExport e = hfssExportOf(model, coplanar(1000), HfssOptions(), QStringLiteral("cpw"));
        QVERIFY2(e.ok(), qPrintable(e.problems.join(QLatin1Char(' '))));
        QStringList dielectrics, conductors;
        QStringList ports;
        QXmlStreamReader xml(e.control);
        QString within;
        QStringList path;
        QString sweep;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement()) {
                const QString name = xml.name().toString();
                const QXmlStreamAttributes a = xml.attributes();
                if (name == QLatin1String("Layer") && path.last() == QLatin1String("Dielectrics"))
                    dielectrics << a.value(QLatin1String("Name")).toString() + QLatin1Char('=') + a.value(QLatin1String("Thickness")).toString();
                if (name == QLatin1String("Layer") && path.last() == QLatin1String("Layers"))
                    conductors << a.value(QLatin1String("Name")).toString() + QLatin1Char(' ') + a.value(QLatin1String("TargetLayer")).toString() + QLatin1Char(' ') + a.value(QLatin1String("Elevation")).toString()
                                      + QLatin1Char(' ') + a.value(QLatin1String("Thickness")).toString() + QLatin1Char(' ') + a.value(QLatin1String("Type")).toString();
                if (name == QLatin1String("CircuitPortPt"))
                    ports << a.value(QLatin1String("Name")).toString() + QLatin1Char(' ') + a.value(QLatin1String("x1")).toString() + QLatin1Char(' ') + a.value(QLatin1String("Layer1")).toString() + QLatin1Char(' ')
                                 + a.value(QLatin1String("y2")).toString() + QLatin1Char(' ') + a.value(QLatin1String("Layer2")).toString() + QLatin1Char(' ') + a.value(QLatin1String("Z0")).toString();
                path << name;
            } else if (xml.isEndElement()) {
                path.removeLast();
            }
        }
        QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));
        // Top first, a metal's room among them.
        QCOMPARE(dielectrics, QStringList({QStringLiteral("M1_fill=1.5"), QStringLiteral("PI=20"), QStringLiteral("Si=500")}));
        QCOMPARE(conductors, QStringList({QStringLiteral("2 M1 520 1.5 conductor"), QStringLiteral("1 GND 0 0 conductor")}));
        QCOMPARE(ports.size(), 2);
        QCOMPARE(ports.at(0), QStringLiteral("P1 0 M1 0 GND 75"));   // (a point on its edge, over the plane)
        QVERIFY2(ports.at(1).endsWith(QLatin1String("M1 50")), qPrintable(ports.at(1)));   // (a return's edge on its own layer)
        // (The middle of that edge: its point on the signal's, the other on a ground's.)
        QVERIFY2(QRegularExpression(QStringLiteral("Name=\"P2\" x1=\"1000\" y1=\"0\" Layer1=\"M1\" x2=\"1000\" y2=\"-?190\" Layer2=\"M1\"")).match(e.control).hasMatch(), qPrintable(e.control));
        QVERIFY(e.control.contains(QLatin1String("<Start>1000000000Hz</Start>")));
        QVERIFY(e.control.contains(QLatin1String("<Step>1000000000Hz</Step>")));
        QVERIFY(e.control.contains(QLatin1String("AirboxTruncateAtGround=\"true\"")));
        // The GDSII file: every shape on its layer, the plane past them.
        QTemporaryDir dir;
        const QString path2 = written(e, dir.path());
        QString why;
        const auto read = qucs_s::layout::read(QFileInfo(path2).absoluteDir().filePath(QStringLiteral("model.gds")), &why);
        QVERIFY2(read != nullptr, qPrintable(why));
        QCOMPARE(read->shapeCount, quint64(4));
        QVERIFY(read->layerIndex(qucs_s::layout::LayerKey{1, 0}) >= 0);
        QVERIFY(read->layerIndex(qucs_s::layout::LayerKey{2, 0}) >= 0);
    }

    // A model that cannot be written says why, nothing half-written.
    void whatCannotBeWrittenIsSaid()
    {
        const Model empty = modelOf(microstripStack() + QLatin1Char(',') + portsOf(QString()) + QLatin1Char(',') + QLatin1String(kStudy));
        const HfssExport none = hfssExportOf(empty, {});
        QVERIFY(!none.ok());
        QVERIFY(none.problems.join(QLatin1Char(' ')).contains(QLatin1String("no shapes")));
        HfssOptions bad;
        bad.portType = QStringLiteral("coax");
        const HfssExport wrongType = hfssExportOf(empty, coplanar(100), bad);
        QVERIFY(wrongType.problems.join(QLatin1Char(' ')).contains(QLatin1String("coax")));
        const HfssExport noPort = hfssExportOf(empty, coplanar(100));
        QVERIFY(noPort.problems.join(QLatin1Char(' ')).contains(QLatin1String("No port")));
        QString why;
        QVERIFY(!writeHfssExport(noPort, QStringLiteral("/nowhere/x.py"), nullptr, &why));
        QVERIFY(!why.isEmpty());
        // A port on no edge is left out and said, and the rest still written.
        const Model oneOff = modelOf(microstripStack() + QLatin1Char(',')
                                     + portsOf(QStringLiteral(R"({"type": "rf_edge_port", "tag": "p1", "label": "P1", "properties": {"conductor": "M1", "x": "0", "y": "0", "facing": "-x"}},
                                                                {"type": "rf_edge_port", "tag": "p2", "label": "P2", "properties": {"conductor": "M9", "x": "0", "y": "0", "facing": "+x"}})"))
                                     + QLatin1Char(',') + QLatin1String(kStudy));
        const HfssExport partly = hfssExportOf(oneOff, coplanar(100));
        QCOMPARE(partly.ports, 1);
        QVERIFY(partly.notes.join(QLatin1Char('\n')).contains(QLatin1String("P2 is not written")));
        // A return with no edge to stand for it: said, of an edge port and of an
        // internal one, which are written all the same.
        const Model noReturn = modelOf(microstripStack() + QLatin1Char(',')
                                       + portsOf(QStringLiteral(R"({"type": "rf_terminal", "tag": "t", "label": "T1", "properties": {"metal": "M9", "x": "0", "y": "0", "face": "-x"}},
                                                                  {"type": "rf_edge_port", "tag": "p1", "label": "P1", "properties": {"conductor": "M1", "x": "0", "y": "0", "facing": "-x", "return": "terminals", "returns": ["T1"]}},
                                                                  {"type": "rf_internal_port", "tag": "p2", "label": "P2", "properties": {"x": "50[um]", "y": "0", "to": "M1", "return": "terminals", "returns": ["T1"]}})"))
                                       + QLatin1Char(',') + QLatin1String(kStudy));
        const HfssExport lost = hfssExportOf(noReturn, coplanar(100));
        QCOMPARE(lost.ports, 2);
        const QString lostNotes = lost.notes.join(QLatin1Char('\n'));
        QVERIFY2(lostNotes.contains(QLatin1String("P1: its return T1 has no edge to reference")), qPrintable(lostNotes));
        QVERIFY2(lostNotes.contains(QLatin1String("P2: its return T1 has no edge to reference")), qPrintable(lostNotes));
    }

    // ------------------------------------------------------------ The round trip

    // Export, import: the same model, its layout file kept where it is there;
    // its shapes as solved where it is not, from the GDSII file beside the script
    // or from the script itself.
    void exportThenImportIsTheSameModel()
    {
        QTemporaryDir dir;
        const QString layoutFile = QDir(dir.path()).filePath(QStringLiteral("layout.gds"));
        QList<QPair<int, QPolygonF>> given;
        for (const Conductor& c : coplanar(1000)) given << qMakePair(1, c.outline);
        QString why;
        QVERIFY2(writeGdsOf(given, layoutFile, QStringLiteral("TOP"), &why), qPrintable(why));
        const QString layoutNode = QStringLiteral(R"({"type": "rf_layout", "tag": "layout", "properties": {"file": "layout.gds"}, "children": [
            {"type": "rf_layer_map", "tag": "layer_map", "properties": {"rows": [["1", "0", "M1"]]}}, {"type": "rf_processing", "tag": "processing", "properties": {"snap": true}}]})");
        const QString tree = microstripStack() + QLatin1Char(',') + layoutNode + QLatin1Char(',')
                             + portsOf(QStringLiteral(R"({"type": "rf_return", "tag": "g1", "label": "G1", "properties": {"x": "100[um]", "y": "100[um]"}},
                                                        {"type": "rf_edge_port", "tag": "p1", "label": "P1", "properties": {"conductor": "M1", "x": "0", "y": "0", "facing": "-x", "return": "beside", "calibrate": true}},
                                                        {"type": "rf_edge_port", "tag": "p2", "label": "P2", "properties": {"conductor": "M1", "x": "1000[um]", "y": "0", "facing": "+x", "return": "beside", "pair": "P1"}})"))
                             + QLatin1Char(',') + QLatin1String(kStudy);
        const Model model = modelOf(tree);
        const HfssExport e = hfssExportOf(model, coplanar(1000), HfssOptions(), QStringLiteral("cpw"));
        QVERIFY2(e.ok(), qPrintable(e.problems.join(QLatin1Char(' '))));
        const QString script = written(e, dir.path());

        // The file named is there.
        HfssImport back = hfssImportOf(script, {}, &why);
        QVERIFY2(back.model.has_value(), qPrintable(why));
        QCOMPARE(back.kind, QStringLiteral("exported script"));
        QCOMPARE(back.layoutFile, QFileInfo(layoutFile).absoluteFilePath());
        QCOMPARE(back.model->toObject(), model.toObject());
        QCOMPARE(back.ports, 2);
        QCOMPARE(back.polygons, 3);
        QCOMPARE(back.vias, 0);
        QVERIFY(std::abs(back.to - 40e9) < 1);
        // Written as a model beside it, it opens as the same one.
        const QString modelPath = QDir(dir.path()).filePath(QStringLiteral("back.qrf25"));
        QStringList files;
        QVERIFY2(writeHfssImport(back, modelPath, &files, &why), qPrintable(why));
        QFile saved(modelPath);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const std::optional<Model> reopened = Model::fromJson(saved.readAll(), &why);
        QVERIFY(reopened.has_value());
        QCOMPARE(reopened->toObject(), model.toObject());

        // Taken elsewhere, the script finds the file named in the folder it kept.
        {
            HfssOptions how;
            how.modelFolder = dir.path();
            const HfssExport kept = hfssExportOf(model, coplanar(1000), how, QStringLiteral("cpw"));
            QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("elsewhere")));
            const QString moved = QDir(dir.path()).filePath(QStringLiteral("elsewhere/moved.py"));
            QFile f(moved);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(kept.script.toUtf8());
            f.close();
            const HfssImport found = hfssImportOf(moved, {}, &why);
            QVERIFY2(found.model.has_value(), qPrintable(why));
            QCOMPARE(found.layoutFile, QFileInfo(layoutFile).absoluteFilePath());
            QVERIFY(found.notes.join(QLatin1Char(' ')).isEmpty());
        }

        // The file named gone: from the GDSII file beside the script, nothing
        // done to the shapes, which are as they were solved.
        QVERIFY(QFile::remove(layoutFile));
        back = hfssImportOf(script, {}, &why);
        QVERIFY2(back.model.has_value(), qPrintable(why));
        QCOMPARE(QFileInfo(back.layoutFile).fileName(), QStringLiteral("model.gds"));
        QVERIFY(back.notes.join(QLatin1Char(' ')).contains(QLatin1String("layout.gds is not there")));
        QVERIFY(!flagOf(*back.model->layout().child(QStringLiteral("rf_processing")), QStringLiteral("merge")));
        // The plane is no shape of the model's: its map sends only the metal.
        QJsonArray rows = back.model->layout().child(QStringLiteral("rf_layer_map"))->props.value(QLatin1String("rows")).toArray();
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.at(0).toArray().at(2).toString(), QStringLiteral("M1"));
        QCOMPARE(rows.at(0).toArray().at(0).toString(), QStringLiteral("2"));
        // That read as the model reads it: its three shapes on M1, and only those.
        const QString beside = QDir(dir.path()).filePath(QStringLiteral("again.qrf25"));
        QVERIFY2(writeHfssImport(back, beside, nullptr, &why), qPrintable(why));
        QFile againFile(beside);
        QVERIFY(againFile.open(QIODevice::ReadOnly));
        const std::optional<Model> again = Model::fromJson(againFile.readAll(), &why);
        QVERIFY(again.has_value());
        const Processed shapes = processLayout(*again, dir.path(), &why);
        QCOMPARE(shapes.conductors.size(), 3);
        // (In µm, as they were written: 62 and two of 300 by 1000.)
        double areas = 0;
        for (const Conductor& c : shapes.conductors) areas += c.area();
        QVERIFY2(std::abs(areas - 662000) < 1, qPrintable(QString::number(areas)));

        // The GDSII file gone too: from the script itself.
        QVERIFY(QFile::remove(QDir(dir.path()).filePath(QStringLiteral("model.gds"))));
        back = hfssImportOf(script, {}, &why);
        QVERIFY2(back.model.has_value(), qPrintable(why));
        QVERIFY(back.layoutFile.isEmpty());
        QCOMPARE(back.shapes.size(), 4);   // (three, and the plane the map leaves out)
        const QString own = QDir(dir.path()).filePath(QStringLiteral("own.qrf25"));
        files.clear();
        QVERIFY2(writeHfssImport(back, own, &files, &why), qPrintable(why));
        QCOMPARE(files.size(), 2);
        QVERIFY(QFileInfo::exists(QDir(dir.path()).filePath(QStringLiteral("own.gds"))));
    }

    // A script that is none of ours, a line that does not read, a format not
    // known, a file not there: said.
    void whatCannotBeImportedIsSaid()
    {
        QTemporaryDir dir;
        const auto file = [&dir](const QString& name, const QByteArray& text) {
            const QString path = QDir(dir.path()).filePath(name);
            QFile f(path);
            f.open(QIODevice::WriteOnly);
            f.write(text);
            return path;
        };
        QString why;
        QVERIFY(!hfssImportOf(file(QStringLiteral("a.py"), "print('hello')\n"), {}, &why).model);
        QVERIFY2(why.contains(QLatin1String("has no \"#qucs-s\" line")), qPrintable(why));
        QVERIFY(!hfssImportOf(file(QStringLiteral("b.py"), "#qucs-s {not json\n"), {}, &why).model);
        QVERIFY(why.contains(QLatin1String("does not read")));
        QVERIFY(!hfssImportOf(file(QStringLiteral("c.py"), "#qucs-s {\"format\": \"later\"}\n"), {}, &why).model);
        QVERIFY(why.contains(QLatin1String("later")));
        QVERIFY(!hfssImportOf(QDir(dir.path()).filePath(QStringLiteral("none.py")), {}, &why).model);
        QVERIFY(why.contains(QLatin1String("not there")));
        QVERIFY(!hfssImportOf(file(QStringLiteral("d.txt"), "x"), {}, &why).model);
        QVERIFY(why.contains(QLatin1String(".aedt")));
        QVERIFY(!hfssImportOf(file(QStringLiteral("e.xml"), "<a><b></a>"), {}, &why).model);
        QVERIFY(why.contains(QLatin1String("does not read as XML")));
        QVERIFY(!hfssImportOf(file(QStringLiteral("f.xml"), "<a/>"), {}, &why).model);
        QVERIFY(why.contains(QLatin1String("no control file")));
        // A project of no 3D Layout design, of none beside it, of nothing.
        QVERIFY(!hfssImportOf(file(QStringLiteral("g.aedt"), "$begin 'AnsoftProject'\n$end 'AnsoftProject'\n"), {}, &why).model);
        QVERIFY(why.contains(QLatin1String("no HFSS 3D Layout design")));
        QVERIFY(!hfssImportOf(file(QStringLiteral("h.aedt"), "$begin 'PlanarEMCircuit'\n\tName='D'\n$end 'PlanarEMCircuit'\n"), {}, &why).model);
        QVERIFY(why.contains(QLatin1String(".aedb")));
        QVERIFY(!hfssImportOf(file(QStringLiteral("edb.def"), "not one"), {}, &why).model);
        QVERIFY(why.contains(QLatin1String("no edb.def")));
    }

    // A control file as Ansys has them: dielectrics whole, metals among them, a
    // via layer between two; its shapes kept in the GDSII file beside it.
    void aControlFileReadsAsAModel()
    {
        QTemporaryDir dir;
        const QString control = QDir(dir.path()).filePath(QStringLiteral("chip.xml"));
        {
            QFile f(control);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\" ?>\n"
            "<c:Control xmlns:c=\"http://www.ansys.com/control\" schemaVersion=\"1.0\">\n"
            "  <Stackup schemaVersion=\"1.0\">\n"
            "    <Materials>\n"
            "      <Material Name=\"Al\"><Conductivity><Double>3e7</Double></Conductivity></Material>\n"
            "      <Material Name=\"W\"><Conductivity><Double>1e7</Double></Conductivity></Material>\n"
            "      <Material Name=\"SiO2\"><Permittivity><Double>4.1</Double></Permittivity><DielectricLossTangent><Double>0.001</Double></DielectricLossTangent></Material>\n"
            "      <Material Name=\"sub\"><Permittivity><Double>11.7</Double></Permittivity><Conductivity><Double>10</Double></Conductivity></Material>\n"
            "    </Materials>\n"
            "    <ELayers LengthUnit=\"um\">\n"
            "      <Dielectrics>\n"
            "        <Layer Material=\"SiO2\" Name=\"top_ox\" Thickness=\"2\"/>\n"
            "        <Layer Material=\"SiO2\" Name=\"mid_ox\" Thickness=\"3\"/>\n"
            "        <Layer Material=\"SiO2\" Name=\"low_ox\" Thickness=\"1\"/>\n"
            "        <Layer Material=\"sub\" Name=\"subst\" Thickness=\"200\"/>\n"
            "      </Dielectrics>\n"
            "      <Layers>\n"
            "        <Layer Elevation=\"204.5\" GDSDataType=\"0\" Name=\"9\" TargetLayer=\"Top\" Material=\"Al\" Thickness=\"1\" Type=\"conductor\"/>\n"
            "        <Layer Elevation=\"201\" GDSDataType=\"2\" Name=\"5\" TargetLayer=\"Low\" Material=\"Al\" Thickness=\"0.5\" Type=\"conductor\"/>\n"
            "        <Layer Name=\"50\" Type=\"outline\"/>\n"
            "      </Layers>\n"
            "      <Vias>\n"
            "        <Layer GDSDataType=\"0\" Material=\"W\" Name=\"7\" SolveInside=\"false\" StartLayer=\"Top\" StopLayer=\"Low\" TargetLayer=\"Via1\"/>\n"
            "      </Vias>\n"
            "    </ELayers>\n"
            "  </Stackup>\n"
            "  <Boundaries LengthUnit=\"um\">\n"
            "    <CircuitPortPt Name=\"In\" x1=\"0\" y1=\"5\" Layer1=\"Top\" x2=\"0\" y2=\"5\" Layer2=\"Low\" Z0=\"75\"/>\n"
            "    <Extents Type=\"bbox\" DielType=\"bbox\" DielHorizFactor=\"10um\" AirboxHorizFactor=\"0\" AirboxVertFactorPos=\"350um\" AirboxVertFactorNeg=\"350um\" UseRadiationBoundary=\"True\"/>\n"
            "  </Boundaries>\n"
            "  <SimulationSetups>\n"
            "    <HFSSSetup schemaVersion=\"1.0\" Name=\"S\">\n"
            "      <HFSSSimulationSettings><HFSSAdaptiveSettings><AdaptiveSettings><SingleFrequencyDataList><AdaptiveFrequencyData><AdaptiveFrequency>5GHz</AdaptiveFrequency></AdaptiveFrequencyData></SingleFrequencyDataList></AdaptiveSettings></HFSSAdaptiveSettings></HFSSSimulationSettings>\n"
            "      <HFSSSweepDataList><FreqSweep><Interpolating><LinearStep><Start>500mhz</Start><Stop>10ghz</Stop><Step>500mhz</Step></LinearStep></Interpolating></FreqSweep></HFSSSweepDataList>\n"
            "    </HFSSSetup>\n"
            "  </SimulationSetups>\n"
            "</c:Control>\n");
        }
        QString why;
        // Its shapes: a GDSII file of its name, with a layer the control file does not map.
        QList<QPair<int, QPolygonF>> shapes{qMakePair(9, box(QString(), 0, 0, 100, 10).outline), qMakePair(77, box(QString(), 0, 0, 5, 5).outline)};
        QVERIFY(writeGdsOf(shapes, QDir(dir.path()).filePath(QStringLiteral("chip.gds")), QStringLiteral("TOP"), &why));
        const HfssImport read = hfssImportOf(control, {}, &why);
        QVERIFY2(read.model.has_value(), qPrintable(why));
        QCOMPARE(read.kind, QStringLiteral("control file"));
        QCOMPARE(QFileInfo(read.layoutFile).fileName(), QStringLiteral("chip.gds"));
        const Stack s = read.model->stack();
        QStringList names;
        for (const Layer& l : s.layers) names << l.name;
        // The substrate, the oxide up to Low, Low, the oxides up to Top, the via
        // between its two metals, Top in its oxide, and what is over it.
        QCOMPARE(names, QStringList({QStringLiteral("GND"), QStringLiteral("subst"), QStringLiteral("low_ox"), QStringLiteral("Low"), QStringLiteral("mid_ox"), QStringLiteral("top_ox"), QStringLiteral("Via1"), QStringLiteral("Top"), QStringLiteral("top_ox_2")}));
        QVERIFY2(s.ok(), qPrintable(s.problems.join(QLatin1Char(' '))));
        const Layer* sub = s.named(QStringLiteral("subst"));
        QCOMPARE(sub->kind, Layer::Substrate);
        QVERIFY(std::abs(sub->thickness - 200e-6) < 1e-12);
        QCOMPARE(sub->epsr, 11.7);
        QCOMPARE(sub->sigma, 10.0);
        const Layer* low = s.named(QStringLiteral("Low"));
        QVERIFY(std::abs(low->thickness - 0.5e-6) < 1e-12);
        QCOMPARE(low->sigma, 3e7);
        QVERIFY(std::abs(s.named(QStringLiteral("mid_ox"))->thickness - 2.5e-6) < 1e-12);   // (from 201.5 up to 204)
        const Layer* via = s.named(QStringLiteral("Via1"));
        QCOMPARE(via->from, QStringLiteral("Low"));
        QCOMPARE(via->to, QStringLiteral("Top"));
        QCOMPARE(via->sigma, 1e7);
        // Its map, its port and its sweep.
        const QJsonArray rows = read.model->layout().child(QStringLiteral("rf_layer_map"))->props.value(QLatin1String("rows")).toArray();
        QCOMPARE(rows.size(), 3);
        QCOMPARE(rows.at(1).toArray().at(1).toString(), QStringLiteral("2"));
        QCOMPARE(rows.at(2).toArray().at(2).toString(), QStringLiteral("Via1"));
        QCOMPARE(read.ports, 1);
        const Node* port = &read.model->ports().children.front();
        QCOMPARE(port->type, QStringLiteral("rf_internal_port"));
        QCOMPARE(textOf(*port, QStringLiteral("to")), QStringLiteral("Top"));
        QCOMPARE(textOf(*port, QStringLiteral("from")), QStringLiteral("Low"));
        QVERIFY(std::abs(numberOf(*port, QStringLiteral("y"), *read.model, 0) - 5e-6) < 1e-12);
        QCOMPARE(numberOf(*port, QStringLiteral("impedance"), *read.model, 0), 75.0);
        QCOMPARE(read.points, 20);
        QVERIFY(std::abs(read.from - 0.5e9) < 1);
        QVERIFY(std::abs(read.to - 10e9) < 1);
        // Nothing wrong with it that it should not say.
        QVERIFY2(read.problems.isEmpty(), qPrintable(read.problems.join(QLatin1Char(' '))));
    }

    // An export's control file read back: the same stack in z, and a lid-less
    // plane its ground.
    void aSweepFromNoughtBeginsAtItsFirstStep()
    {
        QTemporaryDir dir;
        const QString control = QDir(dir.path()).filePath(QStringLiteral("dc.xml"));
        {
            QFile f(control);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\" ?>\n"
                    "<c:Control xmlns:c=\"http://www.ansys.com/control\" schemaVersion=\"1.0\">\n"
                    "  <SimulationSetups><HFSSSetup Name=\"S\"><HFSSSweepDataList><FreqSweep><Interpolating><LinearStep>"
                    "<Start>0Hz</Start><Stop>10GHz</Stop><Step>1GHz</Step></LinearStep></Interpolating></FreqSweep></HFSSSweepDataList></HFSSSetup></SimulationSetups>\n"
                    "</c:Control>\n");
        }
        QString why;
        const HfssImport read = hfssImportOf(control, {}, &why);
        QVERIFY2(read.model.has_value(), qPrintable(why));
        QCOMPARE(read.from, 1e9);
        QCOMPARE(read.points, 10);
        QVERIFY2(read.notes.join(QLatin1Char(' ')).contains(QLatin1String("Its sweep begins at nought")), qPrintable(read.notes.join(QLatin1Char(' '))));
    }

    void anExportsControlFileReadsBack()
    {
        const Model model = modelOf(microstripStack() + QLatin1Char(',')
                                    + portsOf(QStringLiteral(R"({"type": "rf_edge_port", "tag": "p1", "label": "P1", "properties": {"conductor": "M1", "x": "0", "y": "0", "facing": "-x"}})"))
                                    + QLatin1Char(',') + QLatin1String(kStudy));
        const HfssExport e = hfssExportOf(model, {box(QStringLiteral("M1"), 0, -31, 1000, 31)});
        QVERIFY(e.ok());
        QTemporaryDir dir;
        const QString script = written(e, dir.path());
        QString why;
        const HfssImport back = hfssImportOf(QFileInfo(script).absoluteDir().filePath(QStringLiteral("model.xml")), {}, &why);
        QVERIFY2(back.model.has_value(), qPrintable(why));
        const Stack s = back.model->stack();
        QStringList names;
        for (const Layer& l : s.layers) names << l.name;
        // (What fills M1's room is its room: gone.)
        QCOMPARE(names, QStringList({QStringLiteral("GND"), QStringLiteral("Si"), QStringLiteral("PI"), QStringLiteral("M1")}));
        QVERIFY(std::abs(s.named(QStringLiteral("Si"))->thickness - 500e-6) < 1e-12);
        QCOMPARE(s.named(QStringLiteral("Si"))->sigma, 6.5);
        QCOMPARE(s.named(QStringLiteral("PI"))->tand, 0.002);
        QCOMPARE(s.named(QStringLiteral("M1"))->sigma, 4.1e7);
    }

    // ------------------------------------------------------------ A project

    // An edb.def as a project keeps it: the stack, the shapes by their kinds, a
    // void and an arc, the via, the ports and the sweep.
    void aProjectReadsWithoutAnsysSoftware()
    {
        QTemporaryDir dir;
        const QString aedb = QDir(dir.path()).filePath(QStringLiteral("board.aedb"));
        QVERIFY(QDir().mkpath(aedb));
        {
            QFile f(QDir(aedb).filePath(QStringLiteral("edb.def")));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(edbFile());
        }
        // The reader by itself.
        EdbDesign design;
        QString why;
        QVERIFY2(readEdb(edbFile(), {}, &design, &why), qPrintable(why));
        QCOMPARE(design.version, QStringLiteral("10.6"));
        QCOMPARE(design.name, QStringLiteral("Board"));
        QCOMPARE(design.mode, QStringLiteral("Laminate"));
        QCOMPARE(design.layers.size(), 3);
        QCOMPARE(design.nets.value(1), QStringLiteral("sig"));
        QCOMPARE(design.materials.value(QStringLiteral("fr4")).epsr, 4.4);
        QCOMPARE(design.shapes.size(), 4);
        QCOMPARE(design.shapes.at(0).kind, QStringLiteral("rectangle"));
        QCOMPARE(design.shapes.at(0).outline.boundingRect(), QRectF(QPointF(0, -3000), QPointF(20000, 3000)));
        QCOMPARE(design.shapes.at(1).kind, QStringLiteral("path"));
        QCOMPARE(design.shapes.at(1).name, QStringLiteral("line_1"));
        QVERIFY(std::abs(design.shapes.at(1).outline.boundingRect().height() - 400) < 1e-6);
        // The bowed side: 0.5 mm of arc over a chord of 2 mm, past x = 23 mm is it
        // not: on the left of the way the outline goes.
        const QRectF pad = design.shapes.at(2).outline.boundingRect();
        QVERIFY2(std::abs(pad.top() - -1000) < 1e-6 && std::abs(pad.bottom() - 1500) < 1e-6, qPrintable(QStringLiteral("%1 %2").arg(pad.top()).arg(pad.bottom())));
        QCOMPARE(design.shapes.at(3).owner, 12);
        QCOMPARE(design.vias.size(), 1);
        QCOMPARE(design.vias.first().padstack, QStringLiteral("GVIA"));
        QVERIFY(std::abs(design.vias.first().at.y() - 750) < 1e-9);
        QCOMPARE(design.padstacks.value(QStringLiteral("GVIA")).layers, QStringList({QStringLiteral("TOP"), QStringLiteral("BOT")}));
        QCOMPARE(design.ports.size(), 2);
        const EdbPort edge = design.ports.at(0);
        QCOMPARE(edge.kind, QStringLiteral("edge"));
        QCOMPARE(edge.primitive, 11);
        QCOMPARE(edge.hfssType, QStringLiteral("Wave"));
        QCOMPARE(edge.impedance, 75.0);
        QVERIFY(std::abs(edge.deembed - 1e-3) < 1e-12);
        const EdbPort point = design.ports.at(1);
        QCOMPARE(point.kind, QStringLiteral("point"));
        QVERIFY(std::abs(point.at.x() - 22000) < 1e-6);
        QCOMPARE(design.sweeps.size(), 1);
        QCOMPARE(design.sweeps.first().data, QStringLiteral("LINC 1GHz 10GHz 10"));
        QVERIFY(std::abs(design.sweeps.first().adaptive - 8e9) < 1);

        // As a model.
        const HfssImport read = hfssImportOf(aedb, {}, &why);
        QVERIFY2(read.model.has_value(), qPrintable(why));
        QCOMPARE(read.kind, QStringLiteral("project"));
        const Stack s = read.model->stack();
        QStringList names;
        for (const Layer& l : s.layers) names << l.name;
        // BOT covers less than the whole here (the pad and its port lie past it),
        // so it stays a metal; the via between the two.
        QCOMPARE(names, QStringList({QStringLiteral("GND"), QStringLiteral("BOT"), QStringLiteral("D1"), QStringLiteral("V_BOT_TOP"), QStringLiteral("TOP")}));
        QVERIFY(std::abs(s.named(QStringLiteral("D1"))->thickness - 1e-3) < 1e-12);
        QCOMPARE(s.named(QStringLiteral("D1"))->epsr, 4.4);
        QCOMPARE(s.named(QStringLiteral("TOP"))->sigma, 5.8e7);
        // The sweep, the ports.
        QCOMPARE(read.points, 10);
        QVERIFY(std::abs(read.to - 10e9) < 1);
        const QList<double> study = studySweep(*read.model, &why);
        QCOMPARE(study.size(), 10);
        QVERIFY(std::abs(study.first() - 1e9) < 1);
        QVERIFY(std::abs(study.last() - 10e9) < 1);
        QCOMPARE(read.ports, 2);
        const Node& p1 = read.model->ports().children.at(0);
        QCOMPARE(p1.type, QStringLiteral("rf_edge_port"));
        QCOMPARE(textOf(p1, QStringLiteral("conductor")), QStringLiteral("TOP"));
        QCOMPARE(textOf(p1, QStringLiteral("facing")), QStringLiteral("-x"));
        QVERIFY(flagOf(p1, QStringLiteral("calibrate")));
        QCOMPARE(numberOf(p1, QStringLiteral("impedance"), *read.model, 0), 75.0);
        QVERIFY(std::abs(numberOf(p1, QStringLiteral("deembed"), *read.model, 0) - 1e-3) < 1e-12);
        const Node& p2 = read.model->ports().children.at(1);
        QCOMPARE(p2.type, QStringLiteral("rf_internal_port"));
        QCOMPARE(p2.label, QStringLiteral("Inner_port"));
        QCOMPARE(textOf(p2, QStringLiteral("to")), QStringLiteral("TOP"));
        // It says what it is read from.
        QVERIFY(read.notes.join(QLatin1Char(' ')).contains(QLatin1String("no one documents")));

        // Written, the model opens and reads its shapes: the plane on BOT, the
        // line, the pad with its hole, and the via's hole and where it lands.
        const QString modelPath = QDir(dir.path()).filePath(QStringLiteral("board.qrf25"));
        QStringList files;
        QVERIFY2(writeHfssImport(read, modelPath, &files, &why), qPrintable(why));
        QFile saved(modelPath);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const std::optional<Model> opened = Model::fromJson(saved.readAll(), &why);
        QVERIFY(opened.has_value());
        const Processed shapes = processLayout(*opened, dir.path(), &why);
        QVERIFY2(shapes.problems.isEmpty(), qPrintable(shapes.problems.join(QLatin1Char(' '))));
        const QMap<QString, int> counts = shapes.byLayer();
        QCOMPARE(counts.value(QStringLiteral("BOT")), 1);
        // (And where the via lands on TOP, off the line: its barrel's width, as it
        // has no pad to say otherwise. On BOT it is within the plane.)
        QCOMPARE(counts.value(QStringLiteral("TOP")), 3);
        QCOMPARE(counts.value(QStringLiteral("V_BOT_TOP")), 1);
        // The pad's hole took out of its area: 2 x 2 mm and the bow, less 1 x 0.4.
        double padArea = 0;
        for (const Conductor& c : shapes.conductors)
            if (c.layer == QLatin1String("TOP") && c.outline.boundingRect().left() > 20500) padArea = c.area();
        const double bow = 2.0 / 3 * 2000 * 500;   // (a parabola's, near enough for a shallow arc)
        QVERIFY2(std::abs(padArea - (2000.0 * 2000 + bow - 1000.0 * 400)) < 0.03 * (2000.0 * 2000), qPrintable(QString::number(padArea)));
    }

    // Metals and via layers one on another, as a process's stack has them, in a
    // dielectric that goes round them as an Overlap stack's does: metal and via
    // take turns, one via layer of no height, each where the project has it, and
    // the dielectric what they leave. (sky130's stack, as a sample has it.)
    void aRunOfSignalLayersTakesTurnsMetalAndVia()
    {
        const auto stackWith = [](const QByteArray& m2Elevation) {
            return QByteArray("$begin ''\n\tMode='Overlap'\n"
                              "\tSLayer(Layer(N='OX', ID=9, T='dielectric', Zones()), SubL(Th='5um', LElev='100um', R='0um', Mat='fr4'), UseR=false)\n"
                              "\tSLayer(Layer(N='M3', ID=2, T='signal', Zones()), SubL(Th='1um', LElev='3um', R='0um', Mat='copper'), UseR=false)\n"
                              "\tSLayer(Layer(N='V2', ID=5, T='signal', Zones()), SubL(Th='0', LElev='3um', R='0um', Mat='copper', FilMat='fr4'), UseR=false)\n"
                              "\tSLayer(Layer(N='M2', ID=6, T='signal', Zones()), SubL(Th='1um', LElev='") + m2Elevation + QByteArray("', R='0um', Mat='copper'), UseR=false)\n"
                              "\tSLayer(Layer(N='V1', ID=7, T='signal', Zones()), SubL(Th='0.5um', LElev='1.5um', R='0um', Mat='copper', FilMat='air'), UseR=false)\n"
                              "\tSLayer(Layer(N='M1', ID=0, T='signal', Zones()), SubL(Th='0.5um', LElev='1um', R='0um', Mat='copper'), UseR=false)\n"
                              "\tSLayer(Layer(N='SUB', ID=10, T='dielectric', Zones()), SubL(Th='100um', LElev='0um', R='0um', Mat='fr4'), UseR=false)\n$end ''\n");
        };
        const QList<QByteArray> more{centredRectangle(50, "boxed", 7, 1e-3, 0, 10e-6, 20e-6), circle(51, "round", 6, 5e-3, 0, 100e-6)};
        QTemporaryDir dir;
        const QString aedb = QDir(dir.path()).filePath(QStringLiteral("process.aedb"));
        QVERIFY(QDir().mkpath(aedb));
        const auto writeFile = [&aedb](const QByteArray& bytes) {
            QFile f(QDir(aedb).filePath(QStringLiteral("edb.def")));
            return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
        };
        QVERIFY(writeFile(edbFile({}, stackWith("2um"), more)));
        QString why;
        const HfssImport read = hfssImportOf(aedb, {}, &why);
        QVERIFY2(read.model.has_value(), qPrintable(why));
        const Stack s = read.model->stack();
        QStringList names;
        for (const Layer& l : s.layers) names << l.name;
        QCOMPARE(names, QStringList({QStringLiteral("GND"), QStringLiteral("SUB"), QStringLiteral("M1"), QStringLiteral("SUB_2"), QStringLiteral("V_M1_M2"), QStringLiteral("M2"),
                                     QStringLiteral("V_M2_M3"), QStringLiteral("M3"), QStringLiteral("SUB_3"), QStringLiteral("OX")}));
        // (A via layer of no height leaves its metals touching, which a stack here
        // may not: the model says so, and it is for the user to give it room.)
        QCOMPARE(s.problems, QStringList(QStringLiteral("M2 and M3 touch: there is no dielectric between them.")));
        QVERIFY2(read.problems.join(QLatin1Char(' ')).contains(QLatin1String("M2 and M3 touch")), qPrintable(read.problems.join(QLatin1Char(' '))));
        const auto at = [&s](const char* name, double z0, double z1) {
            const Layer* l = s.named(QLatin1String(name));
            return l != nullptr && std::abs(l->z0 - z0 * 1e-6) < 1e-12 && std::abs(l->z1 - z1 * 1e-6) < 1e-12;
        };
        QVERIFY(at("SUB", 0, 1));
        QVERIFY(at("M1", 1, 1.5));
        QVERIFY(at("SUB_2", 1.5, 2));
        QVERIFY(at("M2", 2, 3));
        QVERIFY(at("M3", 3, 4));
        QVERIFY(at("SUB_3", 4, 100));
        QVERIFY(at("OX", 100, 105));
        QVERIFY2(!read.notes.join(QLatin1Char(' ')).contains(QLatin1String("overlap")), qPrintable(read.notes.join(QLatin1Char('\n'))));
        QCOMPARE(s.named(QStringLiteral("V_M1_M2"))->kind, Layer::Via);

        // The rectangle by its centre on the via layer, the circle on the metal.
        EdbDesign design;
        QFile back(QDir(aedb).filePath(QStringLiteral("edb.def")));
        QVERIFY(back.open(QIODevice::ReadOnly));
        QVERIFY2(readEdb(back.readAll(), {}, &design, &why), qPrintable(why));
        const auto shapeNamed = [&design](const char* name) {
            for (const EdbShape& shape : std::as_const(design.shapes))
                if (shape.name == QLatin1String(name)) return shape;
            return EdbShape{};
        };
        const EdbShape boxed = shapeNamed("boxed");
        QCOMPARE(boxed.kind, QStringLiteral("rectangle"));
        QCOMPARE(boxed.layer, 7);
        QVERIFY2(std::abs(boxed.outline.boundingRect().left() - 995) < 1e-6 && std::abs(boxed.outline.boundingRect().bottom() - 10) < 1e-6 && std::abs(boxed.outline.boundingRect().width() - 10) < 1e-6,
                 "a rectangle by its centre");
        const EdbShape round = shapeNamed("round");
        QCOMPARE(round.kind, QStringLiteral("circle"));
        Conductor asCut;
        asCut.outline = round.outline;
        QVERIFY(std::abs(asCut.area() - M_PI * 100 * 100) < 0.01 * M_PI * 100 * 100);
        QVERIFY(std::abs(round.outline.boundingRect().center().x() - 5000) < 1e-6);

        // Written and read as a model: what is on each. (A via through all, as
        // its definition says: a hole between each two.)
        const QString modelPath = QDir(dir.path()).filePath(QStringLiteral("process.qrf25"));
        QVERIFY2(writeHfssImport(read, modelPath, nullptr, &why), qPrintable(why));
        QFile saved(modelPath);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const std::optional<Model> opened = Model::fromJson(saved.readAll(), &why);
        QVERIFY(opened.has_value());
        const QMap<QString, int> counts = processLayout(*opened, dir.path(), &why).byLayer();
        QCOMPARE(counts.value(QStringLiteral("M2")), 2);   // (the circle, and where the via passes)
        QCOMPARE(counts.value(QStringLiteral("V_M1_M2")), 2);
        QCOMPARE(counts.value(QStringLiteral("V_M2_M3")), 1);

        // Metals that overlap in height have no stack here that is the project's:
        // it is said.
        QVERIFY(writeFile(edbFile({}, stackWith("1.25um"), more)));
        const HfssImport overlapped = hfssImportOf(aedb, {}, &why);
        QVERIFY2(overlapped.model.has_value(), qPrintable(why));
        QVERIFY2(overlapped.notes.join(QLatin1Char(' ')).contains(QLatin1String("overlap(s) what is below")), qPrintable(overlapped.notes.join(QLatin1Char('\n'))));

        // A rounded rectangle's corners are square here, said once however many.
        EdbDesign rounded;
        QVERIFY2(readEdb(edbFile({}, {}, {centredRectangle(60, "r1", 2, 0, 0, 1e-4, 1e-4, 0, 1e-5), centredRectangle(61, "r2", 2, 2e-3, 0, 1e-4, 1e-4, 0, 1e-5)}), {}, &rounded, &why), qPrintable(why));
        QCOMPARE(rounded.notes.filter(QStringLiteral("rounded corners")).size(), 1);
        // A rectangle turned: a quarter turn swaps its sides.
        EdbDesign turned;
        QVERIFY2(readEdb(edbFile({}, {}, {centredRectangle(62, "t", 2, 0, 0, 2e-4, 1e-4, M_PI / 2)}), {}, &turned, &why), qPrintable(why));
        const QRectF quarter = turned.shapes.last().outline.boundingRect();
        QVERIFY2(std::abs(quarter.width() - 100) < 1e-6 && std::abs(quarter.height() - 200) < 1e-6, qPrintable(QStringLiteral("%1 by %2").arg(quarter.width()).arg(quarter.height())));
    }

    // The lowest layer is the ground plane where it covers the design with
    // nothing cut from it, and a metal otherwise.
    void aPlaneIsTheLayerThatCoversTheDesignWithNoVoid()
    {
        QTemporaryDir dir;
        const QString aedb = QDir(dir.path()).filePath(QStringLiteral("board.aedb"));
        QVERIFY(QDir().mkpath(aedb));
        const auto readWith = [&aedb](const QList<QByteArray>& more, QString* why) {
            QFile f(QDir(aedb).filePath(QStringLiteral("edb.def")));
            if (!f.open(QIODevice::WriteOnly) || f.write(edbFile({}, {}, more)) <= 0) qFatal("edb.def could not be written");
            f.close();
            return hfssImportOf(aedb, {}, why);
        };
        QString why;
        // On BOT (0), 30 by 10 mm about the design: all of it.
        const QByteArray covering = centredRectangle(70, "all", 0, 12e-3, 0, 30e-3, 10e-3);
        HfssImport whole = readWith({covering}, &why);
        QVERIFY2(whole.model.has_value(), qPrintable(why));
        QCOMPARE(whole.layers.first(), QStringLiteral("BOT (the ground plane)"));
        QVERIFY(whole.notes.join(QLatin1Char(' ')).contains(QLatin1String("BOT covers the whole design")));
        QVERIFY(whole.model->stack().named(QStringLiteral("BOT")) != nullptr);
        QCOMPARE(whole.model->stack().named(QStringLiteral("BOT"))->kind, Layer::Ground);
        // A void in it: a metal, its void cut out.
        EdbWriter voided;
        writeShapeHead(voided, 15, 71, "all void", -1, 0, 70);
        writePoints(voided, {0.0, -0.001, 0.001, -0.001, 0.001, 0.001, 0.0, 0.001}, true);
        HfssImport cut = readWith({covering, voided.bytes}, &why);
        QVERIFY2(cut.model.has_value(), qPrintable(why));
        QCOMPARE(cut.layers.first(), QStringLiteral("BOT"));
        QVERIFY(!cut.notes.join(QLatin1Char(' ')).contains(QLatin1String("BOT covers the whole design")));
        QCOMPARE(cut.model->stack().named(QStringLiteral("BOT"))->kind, Layer::Metal);
    }

    // A via through metal of another net: its anti-pad clears it, a pad of its own
    // in the clearance, so that the signal is no short to the plane - which a
    // padstack does with the plane's outline whole. Of the via's own net: no
    // clearance, and it lands on the metal. By the layers' names, or by a padstack's
    // own in the stack's order.
    void aViaClearsMetalOfAnotherNet()
    {
        const QByteArray stack = "$begin ''\n\tMode='Laminate'\n"
            "\tSLayer(Layer(N='TOP', ID=2, T='signal', Zones()), SubL(Th='10um', LElev='2.02mm', R='0um', Mat='copper', FilMat='air'), UseR=false)\n"
            "\tSLayer(Layer(N='D2', ID=3, T='dielectric', Zones()), SubL(Th='1mm', LElev='1.02mm', R='0um', Mat='fr4'), UseR=false)\n"
            "\tSLayer(Layer(N='MID', ID=4, T='signal', Zones()), SubL(Th='10um', LElev='1.01mm', R='0um', Mat='copper', FilMat='fr4'), UseR=false)\n"
            "\tSLayer(Layer(N='D1', ID=1, T='dielectric', Zones()), SubL(Th='1mm', LElev='0.01mm', R='0um', Mat='fr4'), UseR=false)\n"
            "\tSLayer(Layer(N='BOT', ID=0, T='signal', Zones()), SubL(Th='10um', LElev='0mm', R='0um', Mat='copper', FilMat='air'), UseR=false)\n$end ''\n";
        const auto lgm = [](const char* layer, const char* pad, const char* anti) {
            return QByteArray("\t\t\t\t\t$begin 'lgm'\n\t\t\t\t\t\tlay='") + layer + "'\n\t\t\t\t\t\tid=1\n\t\t\t\t\t\tpad(shp='Cir', Szs('" + pad + "'), X='0', Y='0', R='0')\n\t\t\t\t\t\tant(shp='Cir', Szs('" + anti
                   + "'), X='0', Y='0', R='0')\n\t\t\t\t\t\tthm(shp='No', Szs(), X='0', Y='0', R='0')\n\t\t\t\t\t$end 'lgm'\n";
        };
        // The plane on MID, of the net of the plane on BOT; the via from TOP to BOT.
        const QByteArray midPlane = centredRectangle(80, "mid", 4, 10e-3, 0, 25e-3, 8e-3, 0, 0, 0);
        QTemporaryDir dir;
        const auto modelOfBoard = [&dir, &stack, &midPlane](const char* folder, const QByteArray& padLayers, int viaNet, QString* why, HfssImport* read) {
            const QString aedb = QDir(dir.path()).filePath(QLatin1String(folder) + QStringLiteral(".aedb"));
            QDir().mkpath(aedb);
            {
                QFile f(QDir(aedb).filePath(QStringLiteral("edb.def")));
                if (!f.open(QIODevice::WriteOnly) || f.write(edbFile({}, stack, {midPlane}, "HFSS('HFSS Type'='Gap')", padLayers, viaNet)) <= 0) qFatal("edb.def could not be written");
            }
            *read = hfssImportOf(aedb, {}, why);
            if (!read->model) return Processed{};
            const QString modelPath = QDir(dir.path()).filePath(QLatin1String(folder) + QStringLiteral(".qrf25"));
            if (!writeHfssImport(*read, modelPath, nullptr, why)) return Processed{};
            QFile saved(modelPath);
            saved.open(QIODevice::ReadOnly);
            const std::optional<Model> opened = Model::fromJson(saved.readAll(), why);
            return opened ? processLayout(*opened, dir.path(), why) : Processed{};
        };
        const auto areaOn = [](const Processed& shapes, const QString& layer) {
            double total = 0;
            for (const Conductor& c : shapes.conductors)
                if (c.layer == layer) total += c.area();
            return total;
        };
        const double planeArea = 25000.0 * 8000;
        const auto circle = [](double diameter) { return M_PI * diameter * diameter / 4; };

        // Of the signal's net (1), the plane's another (0): cleared, its pad within.
        const QByteArray byName = lgm("TOP", "500um", "800um") + lgm("MID", "500um", "800um") + lgm("BOT", "500um", "800um");
        QString why;
        HfssImport read;
        Processed shapes = modelOfBoard("signal", byName, 1, &why, &read);
        QVERIFY2(read.model.has_value(), qPrintable(why));
        QVERIFY2(read.notes.join(QLatin1Char(' ')).contains(QLatin1String("anti-pad(s) cut from metal of another net")), qPrintable(read.notes.join(QLatin1Char('\n'))));
        // (BOT's rectangle and the pad's island on MID's clearance.)
        QVERIFY2(std::abs(areaOn(shapes, QStringLiteral("MID")) - (planeArea - circle(800) + circle(500))) < 0.001 * circle(500),
                 qPrintable(QStringLiteral("%1 for %2").arg(areaOn(shapes, QStringLiteral("MID"))).arg(planeArea - circle(800) + circle(500))));
        QCOMPARE(shapes.byLayer().value(QStringLiteral("MID")), 2);
        QCOMPARE(shapes.byLayer().value(QStringLiteral("V_TOP_MID")) + shapes.byLayer().value(QStringLiteral("V_MID_TOP")), 1);

        // Of the plane's net: it lands on it, nothing cleared.
        shapes = modelOfBoard("ground", byName, 0, &why, &read);
        QVERIFY2(read.model.has_value(), qPrintable(why));
        QVERIFY(!read.notes.join(QLatin1Char(' ')).contains(QLatin1String("anti-pad(s) cut")));
        QVERIFY2(std::abs(areaOn(shapes, QStringLiteral("MID")) - planeArea) < 0.001 * circle(500), qPrintable(QString::number(areaOn(shapes, QStringLiteral("MID")))));
        QCOMPARE(shapes.byLayer().value(QStringLiteral("MID")), 1);

        // Layers of its own names, upper first, as an imported padstack's are: the
        // second is the middle of the span. (Its top's anti-pad is larger, to tell.)
        const QByteArray ofItsOwn = lgm("s1", "500um", "900um") + lgm("s2", "600um", "1mm") + lgm("s3", "700um", "900um");
        shapes = modelOfBoard("generic", ofItsOwn, 1, &why, &read);
        QVERIFY2(read.model.has_value(), qPrintable(why));
        QVERIFY2(std::abs(areaOn(shapes, QStringLiteral("MID")) - (planeArea - circle(1000) + circle(600))) < 0.001 * circle(600), qPrintable(QString::number(areaOn(shapes, QStringLiteral("MID")))));
        // (The last is the lowest: its pad of 700 µm in its clearance of 900, in BOT's
        // plane of 20 by 6 mm, which is the signal's net's other.)
        QVERIFY2(std::abs(areaOn(shapes, QStringLiteral("BOT")) - (20000.0 * 6000 - circle(900) + circle(700))) < 0.001 * circle(700), qPrintable(QString::number(areaOn(shapes, QStringLiteral("BOT")))));

        // A bottom plane whole but for a clearance is no plane without end.
        const QByteArray covering = centredRectangle(81, "all", 0, 12e-3, 0, 40e-3, 12e-3, 0, 0, 0);
        {
            const QString aedb = QDir(dir.path()).filePath(QStringLiteral("cleared.aedb"));
            QVERIFY(QDir().mkpath(aedb));
            {
                QFile f(QDir(aedb).filePath(QStringLiteral("edb.def")));
                QVERIFY(f.open(QIODevice::WriteOnly));
                f.write(edbFile({}, {}, {covering}, "HFSS('HFSS Type'='Gap')", lgm("TOP", "500um", "800um") + lgm("BOT", "500um", "800um"), 1));
            }
            const HfssImport cleared = hfssImportOf(aedb, {}, &why);
            QVERIFY2(cleared.model.has_value(), qPrintable(why));
            QCOMPARE(cleared.layers.first(), QStringLiteral("BOT"));
        }

        // On a metal of no other shape there, a square of the pad: turned with its
        // via. And from the padstack's hole, whatever number the via keeps for one
        // (0.3 mm in the padstack, 1 mm in the via: a clearance of 0.8 would not hold it).
        {
            const QByteArray rectangles = "\t\t\t\t\t$begin 'lgm'\n\t\t\t\t\t\tlay='TOP'\n\t\t\t\t\t\tpad(shp='Rct', Szs('2mm', '1mm'), X='0', Y='0', R='0')\n\t\t\t\t\t\tant(shp='No', Szs(), X='0', Y='0', R='0')\n\t\t\t\t\t$end 'lgm'\n"
                                          + lgm("MID", "500um", "800um") + lgm("BOT", "500um", "800um");
            const QString aedb = QDir(dir.path()).filePath(QStringLiteral("turned.aedb"));
            QVERIFY(QDir().mkpath(aedb));
            {
                QFile f(QDir(aedb).filePath(QStringLiteral("edb.def")));
                QVERIFY(f.open(QIODevice::WriteOnly));
                f.write(edbFile({}, stack, {midPlane}, "HFSS('HFSS Type'='Gap')", rectangles, 1, M_PI / 2, 1e-3));
            }
            const HfssImport turned = hfssImportOf(aedb, {}, &why);
            QVERIFY2(turned.model.has_value(), qPrintable(why));
            bool found = false, barrels = false;
            for (const auto& [number, polygon] : turned.shapes) {
                const QRectF bound = polygon.boundingRect();
                // (The via at (1000, 750) µm: 1 mm wide and 2 mm high, turned.)
                if (polygon.size() == 4 && std::abs(bound.center().x() - 1000) < 1e-6 && std::abs(bound.center().y() - 750) < 1e-6 && std::abs(bound.width() - 1000) < 1e-6 && std::abs(bound.height() - 2000) < 1e-6) found = true;
                // (A barrel of 0.3 mm: its area, on sixteen sides.)
                Conductor c;
                c.outline = polygon;
                if (polygon.size() == 16 && std::abs(c.area() - circle(300)) < 0.001 * circle(300)) barrels = true;
                QVERIFY2(!(polygon.size() == 16 && std::abs(c.area() - circle(1000)) < 0.1 * circle(1000)), "a barrel of the number the via keeps");
            }
            QVERIFY(found);
            QVERIFY(barrels);
        }

        // A pad of a shape it cannot draw is said, and the barrel stands for it.
        const QByteArray oval = "\t\t\t\t\t$begin 'lgm'\n\t\t\t\t\t\tlay='TOP'\n\t\t\t\t\t\tpad(shp='Ov', Szs('1mm', '2mm', '0.5mm'), X='0', Y='0', R='0')\n\t\t\t\t\t\tant(shp='No', Szs(), X='0', Y='0', R='0')\n\t\t\t\t\t$end 'lgm'\n"
                                + lgm("MID", "500um", "800um") + lgm("BOT", "500um", "800um");
        shapes = modelOfBoard("oval", oval, 1, &why, &read);
        QVERIFY2(read.model.has_value(), qPrintable(why));
        QVERIFY2(read.notes.join(QLatin1Char(' ')).contains(QLatin1String("1 pad(s) of shapes other than")), qPrintable(read.notes.join(QLatin1Char('\n'))));
    }

    // What a port's reference comes to: the layer it is on, beside it; a metal, a
    // terminal under its point, as the model has a return through one; the ground
    // plane, the plane; nothing named, the plane too, and said where there is none.
    void aPortsReferenceIsWhereItReturnsThrough()
    {
        QTemporaryDir dir;
        const QString aedb = QDir(dir.path()).filePath(QStringLiteral("board.aedb"));
        QVERIFY(QDir().mkpath(aedb));
        const auto readWith = [&aedb](const QByteArray& keeps, const QList<QByteArray>& more, QString* why) {
            QFile f(QDir(aedb).filePath(QStringLiteral("edb.def")));
            if (!f.open(QIODevice::WriteOnly) || f.write(edbFile({}, {}, more, keeps)) <= 0) qFatal("edb.def could not be written");
            f.close();
            return hfssImportOf(aedb, {}, why);
        };
        const auto portOf = [](const HfssImport& read) -> const Node* {
            for (const Node& n : read.model->ports().children)
                if (n.label == QLatin1String("P1")) return &n;
            return nullptr;
        };
        QString why;
        // A metal: its terminal under the point, and no calibration, which a port
        // that names its returns cannot have.
        const QByteArray onBot = "HFSS('HFSS Type'='Wave', ReferenceName='BOT:gnd:plane')";
        const HfssImport metal = readWith(onBot, {}, &why);
        QVERIFY2(metal.model.has_value(), qPrintable(why));
        const Node* p1 = portOf(metal);
        QVERIFY(p1 != nullptr);
        QCOMPARE(textOf(*p1, QStringLiteral("return")), QStringLiteral("terminals"));
        QCOMPARE(namesOf(*p1, QStringLiteral("returns")), QStringList(QStringLiteral("P1_ref")));
        QVERIFY(!flagOf(*p1, QStringLiteral("calibrate")));
        const Node* terminal = nullptr;
        for (const Node& n : metal.model->ports().children)
            if (n.type == QLatin1String("rf_terminal")) terminal = &n;
        QVERIFY(terminal != nullptr);
        QCOMPARE(terminal->label, QStringLiteral("P1_ref"));
        QCOMPARE(textOf(*terminal, QStringLiteral("metal")), QStringLiteral("BOT"));
        QCOMPARE(numberOf(*terminal, QStringLiteral("x"), *metal.model, -1), numberOf(*p1, QStringLiteral("x"), *metal.model, -2));
        QCOMPARE(numberOf(*terminal, QStringLiteral("y"), *metal.model, -1), numberOf(*p1, QStringLiteral("y"), *metal.model, -2));
        QVERIFY2(metal.notes.join(QLatin1Char(' ')).contains(QLatin1String("through a terminal under its point (P1_ref)")), qPrintable(metal.notes.join(QLatin1Char('\n'))));
        // (The terminal is no port: S has the two.)
        QCOMPARE(metal.ports, 2);
        QVERIFY2(metal.problems.isEmpty(), qPrintable(metal.problems.join(QLatin1Char(' '))));
        // The same layer, with BOT the ground plane: the plane, calibrated.
        const QByteArray covering = centredRectangle(70, "all", 0, 12e-3, 0, 30e-3, 10e-3);
        const HfssImport planed = readWith(onBot, {covering}, &why);
        QVERIFY2(planed.model.has_value(), qPrintable(why));
        p1 = portOf(planed);
        QCOMPARE(textOf(*p1, QStringLiteral("return")), QStringLiteral("ground"));
        QVERIFY(flagOf(*p1, QStringLiteral("calibrate")));
        // On its own layer: beside it.
        const HfssImport own = readWith("HFSS('HFSS Type'='Gap', ReferenceName='TOP:gnd:pad')", {}, &why);
        QVERIFY2(own.model.has_value(), qPrintable(why));
        p1 = portOf(own);
        QCOMPARE(textOf(*p1, QStringLiteral("return")), QStringLiteral("beside"));
        QVERIFY(!flagOf(*p1, QStringLiteral("calibrate")));
        // Nothing named, and no plane: the ground, and what to do about it said.
        const HfssImport none = readWith("HFSS('HFSS Type'='Gap')", {}, &why);
        QVERIFY2(none.model.has_value(), qPrintable(why));
        p1 = portOf(none);
        QCOMPARE(textOf(*p1, QStringLiteral("return")), QStringLiteral("ground"));
        QVERIFY2(none.notes.join(QLatin1Char(' ')).contains(QLatin1String("through a ground plane that is not there")), qPrintable(none.notes.join(QLatin1Char('\n'))));
    }

    // Asked for nothing by name, it takes the cell with the most shapes, and a
    // symbol's footprint, which a project keeps as a cell too, only for want of
    // any other.
    void aFootprintIsNoDesign()
    {
        const QString footprint = QStringLiteral("_symbolFootprint_61092558_4db12850");
        EdbDesign design;
        QString why;
        QVERIFY2(readEdb(edbCells({footprint, QStringLiteral("EMDesign1")}), {}, &design, &why), qPrintable(why));
        QCOMPARE(design.name, QStringLiteral("EMDesign1"));
        design = {};
        QVERIFY2(readEdb(edbCells({QStringLiteral("EMDesign1"), footprint}), {}, &design, &why), qPrintable(why));
        QCOMPARE(design.name, QStringLiteral("EMDesign1"));
        design = {};
        QVERIFY2(readEdb(edbCells({footprint}), {}, &design, &why), qPrintable(why));
        QCOMPARE(design.name, footprint);
        design = {};
        QVERIFY2(readEdb(edbCells({footprint, QStringLiteral("EMDesign1")}), footprint, &design, &why), qPrintable(why));
        QCOMPARE(design.name, footprint);
    }

    // Stopped by what it does not know, it says so; the stack it has already.
    void aRecordItDoesNotKnowStopsItAndIsSaid()
    {
        // A record of a class the table has not.
        QByteArray stray(4, '\0');
        const qint32 unknown = 999;
        std::memcpy(stray.data(), &unknown, 4);
        EdbDesign design;
        QString why;
        QVERIFY(!readEdb(edbFile(stray), {}, &design, &why));
        QVERIFY2(why.contains(QLatin1String("class 999")), qPrintable(why));
        QCOMPARE(design.layers.size(), 3);
        QVERIFY(design.shapes.isEmpty());
        // As a project: a model with its stack, and what is missing said.
        QTemporaryDir dir;
        const QString aedb = QDir(dir.path()).filePath(QStringLiteral("board.aedb"));
        QVERIFY(QDir().mkpath(aedb));
        {
            QFile f(QDir(aedb).filePath(QStringLiteral("edb.def")));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(edbFile(stray));
        }
        const HfssImport read = hfssImportOf(aedb, {}, &why);
        QVERIFY2(read.model.has_value(), qPrintable(why));
        QVERIFY(read.notes.join(QLatin1Char(' ')).contains(QLatin1String("class 999")));
        QVERIFY(read.notes.join(QLatin1Char(' ')).contains(QLatin1String("export a GDSII file")));
        QVERIFY(read.shapes.isEmpty());
        // Encrypted: refused, and why.
        QByteArray encrypted = edbFile();
        encrypted.replace("Encrypted=false", "Encrypted=true ");
        QVERIFY(!readEdb(encrypted, {}, &design, &why));
        QVERIFY(why.contains(QLatin1String("encrypted")));
        // Cut short: stopped at the end, no shape half-read.
        EdbDesign cut;
        const QByteArray whole = edbFile();
        QVERIFY(!readEdb(whole.left(whole.size() - 400), {}, &cut, &why));
        QVERIFY(cut.shapes.isEmpty());
    }

    // A file damaged anywhere is read to where it stops, or to its end: whatever
    // it comes to, nothing is read past the file, and what is handed on holds only
    // places that are numbers, on a layout that can be drawn.
    void aFileDamagedAnywhereIsNeverMisread()
    {
        const QByteArray whole = edbFile();
        // (Every place in Release; QUCS_EDB_EVERY asks for that under the
        // sanitizers too, where each read takes ten times as long and more.)
        const qsizetype every = slowBuild() && !qEnvironmentVariableIsSet("QUCS_EDB_EVERY") ? 8 : 1;
        const auto wholeShapes = [](const EdbDesign& design) {
            for (const EdbShape& s : design.shapes)
                for (const QPointF& q : s.outline)
                    if (!std::isfinite(q.x()) || !std::isfinite(q.y()) || std::abs(q.x()) >= 1e9 || std::abs(q.y()) >= 1e9) return false;
            for (const EdbVia& v : design.vias)
                if (!std::isfinite(v.at.x()) || !std::isfinite(v.at.y())) return false;
            return true;
        };
        int refused = 0, read = 0;
        const auto tryOn = [&](const QByteArray& bytes) {
            EdbDesign design;
            QString why;
            (readEdb(bytes, {}, &design, &why) ? read : refused) += 1;
            return wholeShapes(design);
        };
        // Each number, as what a damaged one might be.
        const QList<qint32> numbers{-1, 0x7fffffff, 1000000, 0};
        for (qsizetype at = 0; at + 4 <= whole.size(); at += 4 * every)
            for (const qint32 n : numbers) {
                QByteArray damaged = whole;
                std::memcpy(damaged.data() + at, &n, 4);
                QVERIFY2(tryOn(damaged), qPrintable(QStringLiteral("number %1 at %2").arg(n).arg(at)));
            }
        // Each double of the records, as no number and as too far off. (A byte
        // off the grid of fours, too, as they are not all on it.)
        const qsizetype records = whole.indexOf("def=7");
        QVERIFY(records > 0);
        for (qsizetype at = records; at + 8 <= whole.size(); at += every)
            for (const double v : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), 1e30}) {
                QByteArray damaged = whole;
                std::memcpy(damaged.data() + at, &v, 8);
                QVERIFY2(tryOn(damaged), qPrintable(QStringLiteral("%1 at %2").arg(v).arg(at)));
            }
        // Cut short anywhere.
        for (qsizetype size = 0; size < whole.size(); size += every) QVERIFY(tryOn(whole.left(size)));
        qInfo() << read << "read," << refused << "refused";
        QVERIFY(refused > 0 && read > 0);
        // And on to a model and its shapes on disk, what the rest makes of it.
        QTemporaryDir dir;
        const QString aedb = QDir(dir.path()).filePath(QStringLiteral("damaged.aedb"));
        QVERIFY(QDir().mkpath(aedb));
        for (qsizetype at = 0; at + 4 <= whole.size(); at += 8 * every) {
            QByteArray damaged = whole;
            const qint32 n = at % 16 == 0 ? 0x7fffffff : -1;
            std::memcpy(damaged.data() + at, &n, 4);
            {
                QFile f(QDir(aedb).filePath(QStringLiteral("edb.def")));
                QVERIFY(f.open(QIODevice::WriteOnly));
                f.write(damaged);
            }
            QString why;
            const HfssImport project = hfssImportOf(aedb, {}, &why);
            if (!project.model) {
                QVERIFY2(!why.isEmpty(), qPrintable(QStringLiteral("nothing said of the damage at %1").arg(at)));
                continue;
            }
            QVERIFY2(writeHfssImport(project, QDir(dir.path()).filePath(QStringLiteral("damaged.qrf25")), nullptr, &why), qPrintable(why));
        }
    }

    // The .aedt beside it: the design's name, and a port's or a sweep's where
    // edb.def has none; alone, it says what is missing.
    void theAedtBesideIt()
    {
        const QString text = QStringLiteral(R"($begin 'AnsoftProject'
	$begin 'Definitions'
		$begin 'Materials'
			$begin 'ro4350'
				permittivity='3.66'
				dielectric_loss_tangent='0.0037'
			$end 'ro4350'
		$end 'Materials'
	$end 'Definitions'
	$begin 'PlanarEMCircuit'
		Name='MyDesign'
		$begin 'Port'
			$begin 'Data'
				$begin 'P1'
					DataId='Port0'
					$begin 'Properties'
						'Renormalize Impedence'='75ohm + 0i ohm'
						Renormalize='true'
						'Deembed Distance'='2mm'
						Deembed='true'
						'HFSS Type'='Wave'
					$end 'Properties'
					Type='EdgePort'
				$end 'P1'
			$end 'Data'
		$end 'Port'
		$begin 'Setup'
			$begin 'Data'
				$begin 'HFSS Setup 1'
					$begin 'Properties'
						Enable='true'
					$end 'Properties'
					Frequency='12GHz'
					$begin 'Data'
						$begin 'Sweep 1'
							$begin 'Sweeps'
								Variable='Sweep 1'
								Data='LIN 1GHz 3GHz 0.5GHz'
							$end 'Sweeps'
						$end 'Sweep 1'
					$end 'Data'
				$end 'HFSS Setup 1'
			$end 'Data'
		$end 'Setup'
	$end 'PlanarEMCircuit'
$end 'AnsoftProject'
)");
        const AedtProject project = readAedt(text);
        QVERIFY(project.found);
        QCOMPARE(project.design, QStringLiteral("MyDesign"));
        QCOMPARE(project.materials.value(QStringLiteral("ro4350")).epsr, 3.66);
        QCOMPARE(project.portOrder, QStringList({QStringLiteral("P1")}));
        QCOMPARE(project.ports.value(QStringLiteral("P1")).value(QStringLiteral("HFSS Type")), QStringLiteral("Wave"));
        QCOMPARE(project.ports.value(QStringLiteral("P1")).value(QStringLiteral("Deembed Distance")), QStringLiteral("2mm"));
        QCOMPARE(project.sweeps.size(), 1);
        QCOMPARE(project.sweeps.first().data, QStringLiteral("LIN 1GHz 3GHz 0.5GHz"));
        QVERIFY(std::abs(project.sweeps.first().adaptive - 12e9) < 1);
        // With an edb.def beside it whose sweep it would have to give: the .aedt's
        // port settings over the file's, by the port's name.
        QTemporaryDir dir;
        const QString aedt = QDir(dir.path()).filePath(QStringLiteral("board.aedt"));
        {
            QFile f(aedt);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(text.toLatin1());
        }
        const QString aedb = QDir(dir.path()).filePath(QStringLiteral("board.aedb"));
        QVERIFY(QDir().mkpath(aedb));
        {
            QFile f(QDir(aedb).filePath(QStringLiteral("edb.def")));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(edbFile());
        }
        QString why;
        const HfssImport read = hfssImportOf(aedt, {}, &why);
        QVERIFY2(read.model.has_value(), qPrintable(why));
        const Node& p1 = read.model->ports().children.at(0);
        QVERIFY(std::abs(numberOf(p1, QStringLiteral("deembed"), *read.model, 0) - 2e-3) < 1e-12);
        QCOMPARE(numberOf(p1, QStringLiteral("impedance"), *read.model, 0), 75.0);
    }

    // The public samples the format was read from, where a folder of them is
    // named: none is kept here (scripts/ci/check-third-party.py), so CI has none.
    // Each reads to its end, and is said; what it holds is checked by hand in
    // docs/feature_gaps/2026-11-04-rf25-hfss-3d-layout.md.
    void thePublicSamplesReadToTheirEnds()
    {
        const QString folder = qEnvironmentVariable("QUCS_EDB_SAMPLES");
        if (folder.isEmpty()) QSKIP("QUCS_EDB_SAMPLES names no folder of edb.def samples");
        const QStringList names = QDir(folder).entryList({QStringLiteral("*.def")}, QDir::Files);
        QVERIFY(!names.isEmpty());
        for (const QString& name : names) {
            QFile f(QDir(folder).filePath(name));
            QVERIFY(f.open(QIODevice::ReadOnly));
            EdbDesign d;
            QString why;
            const bool whole = readEdb(f.readAll(), {}, &d, &why);
            QMap<QString, int> kinds;
            int voids = 0;
            for (const EdbShape& s : std::as_const(d.shapes)) {
                ++kinds[s.kind];
                voids += s.owner >= 0;
            }
            QStringList counted;
            for (auto it = kinds.constBegin(); it != kinds.constEnd(); ++it) counted << QStringLiteral("%1 %2").arg(it.value()).arg(it.key());
            int ports = 0;
            for (const EdbPort& p : std::as_const(d.ports)) ports += p.isPort;
            qInfo().noquote() << name << "version" << d.version << "cell" << d.name << "layers" << d.layers.size() << "nets" << d.nets.size() << counted.join(QStringLiteral(", ")) << "voids" << voids
                              << "vias" << d.vias.size() << "terminals" << d.ports.size() << "ports" << ports << "sweeps" << d.sweeps.size() << "unread" << d.unread << (whole ? QString() : why);
            QVERIFY2(whole, qPrintable(name + QStringLiteral(": ") + why));
            for (const QString& note : std::as_const(d.notes)) qInfo().noquote() << "   " << note;
            // And as a model.
            const QString path = f.fileName();
            HfssImport read = hfssImportOf(path, {}, &why);
            QVERIFY2(read.model.has_value(), qPrintable(name + QStringLiteral(": ") + why));
            qInfo().noquote() << "    stack:" << read.layers.join(QStringLiteral(" / ")) << "| shapes" << read.shapes.size() << "vias" << read.vias << "ports" << read.ports << "sweep" << read.from << read.to << read.points;
            for (const QString& problem : std::as_const(read.problems)) qInfo().noquote() << "    problem:" << problem;
            // What this one is known to be: its ports on the lines they drive.
            if (name.startsWith(QLatin1String("diff_via"))) {
                QHash<QString, const Node*> byName;
                for (const Node& port : read.model->ports().children) byName.insert(port.label, &port);
                QCOMPARE(textOf(*byName.value(QStringLiteral("Port1_T1")), QStringLiteral("conductor")), QStringLiteral("Top_L1"));
                QCOMPARE(textOf(*byName.value(QStringLiteral("Port1_T1")), QStringLiteral("facing")), QStringLiteral("-y"));
                QCOMPARE(textOf(*byName.value(QStringLiteral("Port3_T1")), QStringLiteral("conductor")), QStringLiteral("Inner_Layer_3_L10"));
                QCOMPARE(textOf(*byName.value(QStringLiteral("Port3_T1")), QStringLiteral("facing")), QStringLiteral("+y"));
            }
            // (To look at: each as a model, where a folder for them is named.)
            if (const QString out = qEnvironmentVariable("QUCS_EDB_OUT"); !out.isEmpty()) {
                QVERIFY(writeHfssImport(read, QDir(out).filePath(QFileInfo(name).completeBaseName() + QStringLiteral(".qrf25")), nullptr, &why));
                for (const Node& port : read.model->ports().children)
                    qInfo().noquote() << "    port" << port.label << port.type << QJsonDocument(port.props).toJson(QJsonDocument::Compact);
            }
        }
    }

    void lengthsAsAProjectWritesThem()
    {
        const QHash<QString, double> variables{{QStringLiteral("$w"), 0.4e-3}, {QStringLiteral("gap"), 0.3e-3}};
        const auto length = [&variables](const QString& text) {
            double v = -1;
            return lengthOf(text, variables, &v) ? v : -1.0;
        };
        QCOMPARE(length(QStringLiteral("0.5mm")), 0.5e-3);
        QCOMPARE(length(QStringLiteral("2um")), 2e-6);
        QVERIFY(std::abs(length(QStringLiteral("1mil")) - 25.4e-6) < 1e-15);
        QCOMPARE(length(QStringLiteral("1e-05")), 1e-5);
        QCOMPARE(length(QStringLiteral("0meter")), 0.0);
        QVERIFY(std::abs(length(QStringLiteral("$w/2+gap+0.25mm")) - 0.75e-3) < 1e-12);
        QCOMPARE(length(QStringLiteral("what")), -1.0);
        QCOMPARE(length(QStringLiteral("3furlong")), -1.0);
    }

    // ------------------------------------------------------------ Comparing S

    // The same S, written and read back: nothing apart; a port's S changed: by
    // how much, where; the other reference and the order as the file has them.
    void aSolvesSBesideATouchstoneFile()
    {
        FullWave solved;
        solved.portNames = QStringList{QStringLiteral("P1"), QStringLiteral("P2")};
        solved.references = QList<double>{50, 50};
        for (int k = 1; k <= 5; ++k) {
            solved.frequencies << k * 1e9;
            const Cx series(5, k * 10.0), shunt(0, -500.0 / k);
            QList<QList<Cx>> z{{series + shunt, shunt}, {shunt, series + shunt}};
            solved.z << z;
            solved.s << scatteringOf(z, 50);
        }
        QTemporaryDir dir;
        const QString touchstone = QDir(dir.path()).filePath(QStringLiteral("hfss.s2p"));
        {
            QFile f(touchstone);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(touchstoneOf(solved, 50).toUtf8());
        }
        SCompared same = compareWithTouchstone(solved, touchstone);
        QVERIFY2(same.ok(), qPrintable(same.problem));
        QCOMPARE(same.frequencies.size(), 5);
        QCOMPARE(same.terms.size(), 4);
        QVERIFY2(same.worst < 1e-9, qPrintable(QString::number(same.worst)));
        QCOMPARE(same.reference, 50.0);
        QCOMPARE(same.interpolated, 0);
        // A file of the same S into 75 Ohm, MA, GHz, as HFSS writes one: the
        // solve's read into 75 too, nothing apart.
        const QString into75 = QDir(dir.path()).filePath(QStringLiteral("hfss75.s2p"));
        {
            QFile f(into75);
            QVERIFY(f.open(QIODevice::WriteOnly));
            QString text = QStringLiteral("! comment\n# GHz S MA R 75\n");
            for (int k = 0; k < 5; ++k) {
                const auto s = scatteringOf(solved.z.at(k), 75);
                text += QString::number(solved.frequencies.at(k) / 1e9);
                // (Touchstone 1's two-port order: 11 21 12 22.)
                for (const auto& [i, j] : QList<QPair<int, int>>{{0, 0}, {1, 0}, {0, 1}, {1, 1}})
                    text += QStringLiteral(" %1 %2").arg(std::abs(s.at(i).at(j)), 0, 'g', 17).arg(std::arg(s.at(i).at(j)) * 180 / M_PI, 0, 'g', 17);
                text += QLatin1Char('\n');
            }
            f.write(text.toUtf8());
        }
        const SCompared other = compareWithTouchstone(solved, into75);
        QVERIFY2(other.ok(), qPrintable(other.problem));
        QCOMPARE(other.reference, 75.0);
        QVERIFY2(other.worst < 1e-9, qPrintable(QString::number(other.worst)));
        QVERIFY(other.notes.join(QLatin1Char(' ')).contains(QLatin1String("75 Ohm")));
        // One term off at one frequency: by that, there, and only that.
        const QString off = QDir(dir.path()).filePath(QStringLiteral("off.s2p"));
        {
            QFile f(off);
            QVERIFY(f.open(QIODevice::WriteOnly));
            QString text = QStringLiteral("# Hz S RI R 50\n");
            for (int k = 0; k < 5; ++k) {
                auto s = solved.s.at(k);
                if (k == 2) s[1][0] += Cx(0.01, 0);
                text += QString::number(solved.frequencies.at(k), 'g', 17);
                for (const auto& [i, j] : QList<QPair<int, int>>{{0, 0}, {1, 0}, {0, 1}, {1, 1}})
                    text += QStringLiteral(" %1 %2").arg(s.at(i).at(j).real(), 0, 'g', 17).arg(s.at(i).at(j).imag(), 0, 'g', 17);
                text += QLatin1Char('\n');
            }
            f.write(text.toUtf8());
        }
        const SCompared apart = compareWithTouchstone(solved, off);
        QVERIFY2(apart.ok(), qPrintable(apart.problem));
        QVERIFY(std::abs(apart.worst - 0.01) < 1e-9);
        QCOMPARE(apart.worstAt, 3e9);
        QCOMPARE(apart.worstI, 2);
        QCOMPARE(apart.worstJ, 1);
        // (Its rms over the five.)
        for (const SCompared::Term& t : apart.terms)
            if (t.i == 2 && t.j == 1) QVERIFY2(std::abs(t.rms - 0.01 / std::sqrt(5.0)) < 1e-9, qPrintable(QString::number(t.rms)));
        // The ports in another order: S21 now S12 of the other's.
        const SCompared swapped = compareWithTouchstone(solved, off, {2, 1});
        QVERIFY2(swapped.ok(), qPrintable(swapped.problem));
        QCOMPARE(swapped.worstI, 1);
        QCOMPARE(swapped.worstJ, 2);
        // A file on a coarser grid: read between its points in magnitude and angle.
        const QString coarse = QDir(dir.path()).filePath(QStringLiteral("coarse.s2p"));
        {
            QFile f(coarse);
            QVERIFY(f.open(QIODevice::WriteOnly));
            QString text = QStringLiteral("# Hz S RI R 50\n");
            for (const int k : {0, 2, 4}) {
                const auto& s = solved.s.at(k);
                text += QString::number(solved.frequencies.at(k), 'g', 17);
                for (const auto& [i, j] : QList<QPair<int, int>>{{0, 0}, {1, 0}, {0, 1}, {1, 1}})
                    text += QStringLiteral(" %1 %2").arg(s.at(i).at(j).real(), 0, 'g', 17).arg(s.at(i).at(j).imag(), 0, 'g', 17);
                text += QLatin1Char('\n');
            }
            f.write(text.toUtf8());
        }
        const SCompared between = compareWithTouchstone(solved, coarse);
        QVERIFY2(between.ok(), qPrintable(between.problem));
        QCOMPARE(between.interpolated, 2);
        QCOMPARE(between.frequencies.size(), 5);
        // (At 2 GHz, half way between the file's points at 1 and 3 in both.)
        {
            const Cx a = solved.s.at(0).at(1).at(0), b = solved.s.at(2).at(1).at(0);
            const Cx expected = std::polar((std::abs(a) + std::abs(b)) / 2, (std::arg(a) + std::arg(b)) / 2);
            const Cx read = between.theirs.at(1).at(1).at(0);
            QVERIFY2(std::abs(read - expected) < 1e-9, qPrintable(QStringLiteral("%1 apart").arg(std::abs(read - expected))));
            QVERIFY2(std::abs((a + b) / 2.0 - expected) > 1e-5, "the two ways of reading between are the same here: this shows nothing");
        }
        // Part of the range, as much as there is, and said.
        const QString part = QDir(dir.path()).filePath(QStringLiteral("part.s2p"));
        {
            QFile f(part);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QByteArrayLiteral("# GHz S RI R 50\n"
                                      "2 0 0 0 0 0 0 0 0\n"
                                      "3 0 0 0 0 0 0 0 0\n"));
        }
        const SCompared some = compareWithTouchstone(solved, part);
        QVERIFY2(some.ok(), qPrintable(some.problem));
        QCOMPARE(some.frequencies.size(), 2);
        QVERIFY(some.notes.join(QLatin1Char(' ')).contains(QLatin1String("3 of the solve's 5")));
        // The table of them: a line a frequency, four columns a term.
        const QString table = comparisonTable(apart, QStringLiteral("RF 2.5D"), QStringLiteral("off.s2p"));
        const QStringList lines = table.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QCOMPARE(lines.size(), 7);
        QCOMPARE(lines.at(1).split(QLatin1Char(',')).size(), 17);
        QVERIFY(lines.at(1).contains(QLatin1String("S2_1_file_dB")));

        // What cannot be compared.
        QVERIFY(!compareWithTouchstone(FullWave(), touchstone).ok());
        const QString above = QDir(dir.path()).filePath(QStringLiteral("above.s2p"));
        {
            QFile f(above);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QByteArrayLiteral("# GHz S RI R 50\n20 0 0 0 0 0 0 0 0\n30 0 0 0 0 0 0 0 0\n"));
        }
        const SCompared apartInFrequency = compareWithTouchstone(solved, above);
        QVERIFY2(apartInFrequency.problem.contains(QLatin1String("do not meet")), qPrintable(apartInFrequency.problem));
        QVERIFY(!compareWithTouchstone(solved, QDir(dir.path()).filePath(QStringLiteral("none.s2p"))).ok());
        const QString three = QDir(dir.path()).filePath(QStringLiteral("three.s3p"));
        {
            QFile f(three);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QByteArrayLiteral("# Hz S RI R 50\n1e9 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n"));
        }
        const SCompared more = compareWithTouchstone(solved, three);
        QVERIFY(more.problem.contains(QLatin1String("3 port(s)")));
        // (Which two of the three are ours, it can be told.)
        QVERIFY(compareWithTouchstone(solved, three, {1, 3}).ok());
        QVERIFY(!compareWithTouchstone(solved, three, {1, 4}).ok());
        QVERIFY(!compareWithTouchstone(solved, three, {1}).ok());
        const QString y = QDir(dir.path()).filePath(QStringLiteral("y.s2p"));
        {
            QFile f(y);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QByteArrayLiteral("# Hz Y RI R 50\n1e9 0 0 0 0 0 0 0 0\n"));
        }
        QVERIFY(compareWithTouchstone(solved, y).problem.contains(QLatin1String("Y parameters")));
        const QString txt = QDir(dir.path()).filePath(QStringLiteral("notes.txt"));
        QVERIFY(compareWithTouchstone(solved, txt).problem.contains(QLatin1String("no Touchstone")));
    }
};

QTEST_GUILESS_MAIN(TestRf25Hfss)
#include "test_rf25_hfss.moc"
