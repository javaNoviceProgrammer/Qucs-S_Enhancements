/*
 * rf25_hfss.cpp - an RF 2.5D model written for HFSS™ 3D Layout: an Ansys®
 *                 Electronics Desktop script, and a GDSII file with its XML
 *                 control file; and a solve's S beside a Touchstone file's
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The commands are those of the HFSS 3D Layout Scripting Guide (2025 R2), read
 * against PyAEDT (MIT), which drives them every day: the guide has
 * CreatePolygon take "lineGeometry:=" where it takes "polyGeometry:=", and
 * shows Add only with a near-field setup; the names of an HFSS setup's
 * properties are those of a real .aedt. The control file is that of the HFSS 3D
 * Layout Help, chapter 5, as Ansys writes it itself (pyaedt/gds/Model.xml of
 * github.com/ansys/example-data). No Electronics Desktop has run any of this:
 * docs/feature_gaps/2026-11-04-rf25-hfss-3d-layout.md says what to look at first
 * when one does.
 */
#include "rf25_hfss.h"

#include "dataimport.h"
#include "layout.h"
#include "multiphysics/fem_import.h"
#include "rf25_inductance.h"
#include "rf25_sparams.h"

#include <gdstk/gdstk.hpp>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace qucs_s::rf25 {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::rf25", text);
}

constexpr double kC0 = 299792458.0;

/// A number as the files write it: enough digits for the layout's grid.
QString num(double value)
{
    if (value == 0) return QStringLiteral("0");
    return QString::number(value, 'g', 12);
}

/// \a value m in µm.
QString um(double metres)
{
    return num(metres * 1e6);
}

/// \a text as a Python string: ASCII, and nothing that closes it.
QString py(const QString& text)
{
    QString out = QStringLiteral("\"");
    for (const QChar c : text) {
        if (c == QLatin1Char('"') || c == QLatin1Char('\\')) out += QLatin1Char('\\');
        out += c.unicode() < 128 && c.isPrint() ? QString(c) : QStringLiteral("_");
    }
    return out + QLatin1Char('"');
}

/// \a text as XML's attribute or text.
QString xml(const QString& text)
{
    return text.toHtmlEscaped();
}

/// \a wanted as a name Electronics Desktop takes, none of \a used: letters,
/// digits and _, beginning with a letter. Added to \a used.
QString safeName(const QString& wanted, QSet<QString>* used)
{
    QString name;
    for (const QChar c : wanted) name += c.unicode() < 128 && (c.isLetterOrNumber() || c == QLatin1Char('_')) ? c : QLatin1Char('_');
    if (name.isEmpty() || !name.at(0).isLetter()) name.prepend(QLatin1Char('L'));
    QString unique = name;
    for (int k = 2; used->contains(unique.toLower()); ++k) unique = QStringLiteral("%1_%2").arg(name).arg(k);
    used->insert(unique.toLower());
    return unique;
}

double signedArea(const QPolygonF& p)
{
    double a = 0;
    for (int i = 0, n = int(p.size()); i < n; ++i) {
        const QPointF& u = p.at(i);
        const QPointF& v = p.at((i + 1) % n);
        a += u.x() * v.y() - v.x() * u.y();
    }
    return a / 2;
}

/// \a p with no point repeated at its end or beside itself.
QPolygonF cleaned(const QPolygonF& p)
{
    QPolygonF out;
    for (const QPointF& q : p)
        if (out.isEmpty() || out.last() != q) out << q;
    while (out.size() > 1 && out.first() == out.last()) out.removeLast();
    return out;
}

QPolygonF counterClockwise(QPolygonF p)
{
    if (signedArea(p) < 0) std::reverse(p.begin(), p.end());
    return p;
}

/*!
 * A polygon that holds its holes by a keyhole - a slit there and back, what
 * the union of the processing (gdstk's) gives - as its outline and its holes.
 * Electronics Desktop has voids of its own, and a slit of no width is what a
 * polygon of its should not have.
 */
void openKeyholes(const QPolygonF& polygon, QPolygonF* outline, QList<QPolygonF>* holes)
{
    // Split at each slit, whichever loop it is in: a slit can join a hole to the
    // next hole, as a plane's several are, and not only to the outline.
    QList<QPolygonF> loops, open{cleaned(polygon)};
    while (!open.isEmpty()) {
        const QPolygonF p = open.takeLast();
        const int n = int(p.size());
        bool split = false;
        for (int i = 0; i < n && !split; ++i) {
            const QPointF& a = p.at(i);
            const QPointF& b = p.at((i + 1) % n);
            // The same edge the other way round, further on.
            for (int j = i + 1; j < n && !split; ++j) {
                if (p.at(j) != b || p.at((j + 1) % n) != a) continue;
                QPolygonF inner, outer;
                for (int k = i + 1; k < j; ++k) inner << p.at(k);
                for (int k = j + 1; k < n; ++k) outer << p.at(k);
                for (int k = 0; k <= i; ++k) outer << p.at(k);
                for (const QPolygonF& part : {cleaned(inner), cleaned(outer)})
                    if (part.size() >= 3 && std::abs(signedArea(part)) > 0) open << part;
                split = true;
            }
        }
        if (!split) loops << p;
    }
    // The largest is the outline, and a hole runs the other way round. A loop that
    // runs the same way is a lobe on a seam, no hole: such a polygon stays as it is.
    int largest = 0;
    for (int k = 1; k < loops.size(); ++k)
        if (std::abs(signedArea(loops.at(k))) > std::abs(signedArea(loops.at(largest)))) largest = k;
    if (loops.isEmpty()) {
        *outline = cleaned(polygon);
        return;
    }
    for (int k = 0; k < loops.size(); ++k)
        if (k != largest && (signedArea(loops.at(k)) > 0) == (signedArea(loops.at(largest)) > 0)) {
            *outline = cleaned(polygon);
            return;
        }
    *outline = loops.at(largest);
    for (int k = 0; k < loops.size(); ++k)
        if (k != largest) holes->append(loops.at(k));
}

/// Whether \a shape lies wholly in one of \a holes: what stands in a void of a
/// polygon is no part of it, though within its outline.
bool inAHole(const QList<QPolygonF>& holes, const QPolygonF& shape)
{
    for (const QPolygonF& hole : holes) {
        bool all = !shape.isEmpty();
        for (const QPointF& q : shape) all = all && hole.containsPoint(q, Qt::OddEvenFill);
        if (all) return true;
    }
    return false;
}

/// A layer of the stack as Electronics Desktop has it.
struct HLayer {
    QString name;          ///< its name there
    QString of;            ///< the model's
    bool signal = false;
    double z0 = 0, thickness = 0;   ///< m
    QString material, fill;
    double roughness = 0;  ///< m, rms
    int number = 0;        ///< its layer in the GDSII file (0: it has no shapes)
};

struct HMaterial {
    QString name;
    double epsr = 1, tand = 0, sigma = 0;
};

/// Colours as Electronics Desktop says them: bbggrr.
int bgr(const QColor& c)
{
    return c.red() | (c.green() << 8) | (c.blue() << 16);
}

/// A union-find over the shapes, for their nets.
class Sets
{
public:
    explicit Sets(int count) : a_up(count)
    {
        std::iota(a_up.begin(), a_up.end(), 0);
    }
    int find(int k)
    {
        while (a_up[k] != k) {
            a_up[k] = a_up[a_up[k]];
            k = a_up[k];
        }
        return k;
    }
    void join(int a, int b) { a_up[find(a)] = find(b); }

private:
    std::vector<int> a_up;
};

bool overlapping(const QPolygonF& a, const QRectF& boxA, const QPolygonF& b, const QRectF& boxB)
{
    // (A touch counts: a line butted against a pad is one conductor.)
    constexpr double kTouch = 1e-4;
    if (!boxA.adjusted(-kTouch, -kTouch, kTouch, kTouch).intersects(boxB)) return false;
    for (const QPointF& p : a)
        if (b.containsPoint(p, Qt::OddEvenFill)) return true;
    for (const QPointF& p : b)
        if (a.containsPoint(p, Qt::OddEvenFill)) return true;
    // Crossing, or lying along each other, with no corner inside.
    for (int i = 0, n = int(a.size()); i < n; ++i) {
        const QLineF ea(a.at(i), a.at((i + 1) % n));
        for (int j = 0, m = int(b.size()); j < m; ++j) {
            const QLineF eb(b.at(j), b.at((j + 1) % m));
            if (ea.intersects(eb, nullptr) == QLineF::BoundedIntersection) return true;
            // Collinear and overlapping (parallel for QLineF::intersects).
            const QPointF d = ea.p2() - ea.p1();
            const double len = std::hypot(d.x(), d.y());
            if (!(len > 0)) continue;
            const auto off = [&ea, &d, len](const QPointF& q) {
                const QPointF r = q - ea.p1();
                return std::abs(r.x() * d.y() - r.y() * d.x()) / len;
            };
            if (off(eb.p1()) > kTouch || off(eb.p2()) > kTouch) continue;
            const auto along = [&ea, &d, len](const QPointF& q) {
                const QPointF r = q - ea.p1();
                return (r.x() * d.x() + r.y() * d.y()) / len;
            };
            const double s0 = std::min(along(eb.p1()), along(eb.p2())), s1 = std::max(along(eb.p1()), along(eb.p2()));
            if (s0 <= len + kTouch && s1 >= -kTouch) return true;
        }
    }
    return false;
}

/// The script's own part, which is the same for every model: what the data
/// above it says, done in Electronics Desktop. Python 2.7 (IronPython) and 3
/// alike.
const char* const kEngine = R"PY(
# ---------------------------------------------------------------------------
# What follows is the same in every export: the data above, built.
# ---------------------------------------------------------------------------
import os

try:
    import ScriptEnv
    ScriptEnv.Initialize("Ansoft.ElectronicsDesktop")
except ImportError:
    pass  # (Electronics Desktop running the script has given it oDesktop itself)

try:
    HERE = os.path.dirname(os.path.abspath(__file__))
except NameError:
    HERE = oDesktop.GetProjectDirectory()

said = []
trouble = []


def say(text, severity=0):
    said.append(text)
    if severity:
        trouble.append(text)
    try:
        oDesktop.AddMessage("", "", severity, "RF 2.5D: " + text)
    except Exception:
        pass


def attempt(what, command, *arguments):
    """command(*arguments); what went wrong said, and the rest still built."""
    try:
        return command(*arguments)
    except Exception as why:
        say("%s: %s" % (what, why), 1)
        return None


def length(value):
    return "%.12gum" % value


try:
    oDesktop.RestoreWindow()
except Exception:
    pass  # (No window, as with -ng.)
oProject = oDesktop.NewProject()
oDefinitions = oProject.GetDefinitionManager()
try:
    oProject.InsertDesign("HFSS 3D Layout Design", NAME, "", "")
except Exception:
    oProject.InsertDesign("HFSS3D", NAME, "", "")
oDesign = oProject.SetActiveDesign(NAME)
oEditor = oDesign.SetActiveEditor("Layout")
oExcitations = oDesign.GetModule("Excitations")
oSetups = oDesign.GetModule("SolveSetups")

# The materials: one of each layer's own, so that nothing comes of a library's.
for name, permittivity, tangent, conductivity in MATERIALS:
    attempt("material " + name, oDefinitions.AddMaterial,
            ["NAME:" + name,
             "CoordinateSystemType:=", "Cartesian",
             "BulkOrSurfaceType:=", 1,
             ["NAME:PhysicsTypes", "set:=", ["Electromagnetic"]],
             "permittivity:=", str(permittivity),
             "permeability:=", "1",
             "conductivity:=", str(conductivity),
             "dielectric_loss_tangent:=", str(tangent),
             "magnetic_loss_tangent:=", "0"])

# The stack. (A new design may have layers of its own.)
try:
    for name in list(oEditor.GetStackupLayerNames()):
        oEditor.RemoveLayer(name)
except Exception:
    pass
for name, kind, elevation, thickness, material, fill, colour, roughness in LAYERS:
    layer = ["NAME:stackup layer",
             "Name:=", name,
             "Type:=", kind,
             "Top Bottom:=", "neither",
             "Color:=", colour,
             "Transparency:=", 60,
             "Pattern:=", 1,
             "VisFlag:=", 127,
             "Locked:=", False,
             "DrawOverride:=", 0,
             "Zones:=", []]
    sublayer = ["NAME:Sublayer",
                "Thickness:=", length(thickness),
                "LowerElevation:=", length(elevation),
                "Roughness:=", 0,
                "BotRoughness:=", 0,
                "SideRoughness:=", 0,
                "Material:=", material]
    if kind == "signal":
        sublayer += ["FillMaterial:=", fill]
    layer.append(sublayer)
    if kind == "signal" and roughness > 0:
        rough = length(roughness)
        layer += ["UseR:=", True,
                  "RMdl:=", "Groisse", "NR:=", rough, "HRatio:=", "2.9",
                  "BRMdl:=", "Groisse", "BNR:=", rough, "BHRatio:=", "2.9",
                  "SRMdl:=", "Groisse", "SNR:=", rough, "SHRatio:=", "2.9"]
    attempt("layer " + name, oEditor.AddStackupLayer, layer)

# The metal: planes first, then the shapes and their voids.
for name, layer, net, x0, y0, x1, y1 in PLANES:
    attempt("plane " + name, oEditor.CreateRectangle,
            ["NAME:Contents",
             "rectGeometry:=",
             ["Name:=", name, "LayerName:=", layer, "lw:=", "0",
              "Ax:=", length(x0), "Ay:=", length(y0), "Bx:=", length(x1), "By:=", length(y1),
              "cr:=", "0um", "ang:=", "0deg"]])


def geometry(name, layer, points):
    out = ["Name:=", name, "LayerName:=", layer, "lw:=", "0", "n:=", len(points) // 2, "U:=", "um"]
    for k in range(0, len(points), 2):
        out += ["x:=", points[k], "y:=", points[k + 1]]
    return out


for name, layer, net, points, voids in POLYGONS:
    attempt("polygon " + name, oEditor.CreatePolygon,
            ["NAME:Contents", "polyGeometry:=", geometry(name, layer, points)])
    for k, void in enumerate(voids):
        attempt("void of " + name, oEditor.CreatePolygonVoid,
                ["NAME:Contents", "owner:=", name,
                 "poly voidGeometry:=", geometry("%s_void%d" % (name, k), layer, void)])

# The vias: a padstack for each shape, its hole the drawn shape's.
oPadstacks = oDefinitions.GetManager("Padstack")
for name, shape, sizes, material, upper, lower in PADSTACKS:
    nothing = ["shp:=", "No", "Szs:=", [], "X:=", "0um", "Y:=", "0um", "R:=", "0deg"]
    pad = ["shp:=", shape, "Szs:=", sizes, "X:=", "0um", "Y:=", "0um", "R:=", "0deg"]
    pads = ["NAME:pds"]
    for number, layer in enumerate((upper, lower)):
        pads.append(["NAME:lgm", "lay:=", layer, "id:=", number + 1, "pad:=", pad, "ant:=", nothing, "thm:=", nothing,
                     "X:=", "0um", "Y:=", "0um", "dir:=", "No"])
    attempt("padstack " + name, oPadstacks.Add,
            ["NAME:" + name,
             "ModTime:=", 0,
             "Library:=", "",
             "ModSinceLib:=", False,
             "LibLocation:=", "Project",
             ["NAME:psd", "nam:=", name, "lib:=", "", "mat:=", material, "plt:=", "100",
              pads,
              "hle:=", pad,
              "hRg:=", "UTL",
              "sbsh:=", "None",
              "sbpl:=", "abv",
              "sbr:=", "0um",
              "sb2:=", "0um",
              "sbn:=", ""],
             "ppl:=", []])
for name, padstack, x, y, upper, lower, net in VIAS:
    attempt("via " + name, oEditor.CreateVia,
            ["NAME:Contents",
             "name:=", name,
             "ReferencedPadstack:=", padstack,
             "vposition:=", ["x:=", length(x), "y:=", length(y)],
             "vrotation:=", ["0deg"],
             "overrides hole:=", False,
             "hole diameter:=", ["0um"],
             "Pin:=", False,
             "highest_layer:=", upper,
             "lowest_layer:=", lower])

# The nets: what touches, and what a via joins, is one conductor.
nets = {}
for name, layer, net, x0, y0, x1, y1 in PLANES:
    nets.setdefault(net, []).append(name)
for name, layer, net, points, voids in POLYGONS:
    nets.setdefault(net, []).append(name)
for name, padstack, x, y, upper, lower, net in VIAS:
    nets.setdefault(net, []).append(name)
for net in sorted(nets):
    attempt("net " + net, oEditor.ChangeProperty,
            ["NAME:AllTabs",
             ["NAME:BaseElementTab",
              ["NAME:PropServers"] + nets[net],
              ["NAME:ChangedProps", ["NAME:Net", "Value:=", net]]]])


# How far the shapes reach, and how far the air past them.
attempt("the extents", oDesign.EditHfssExtents,
        ["NAME:HfssExportInfo",
         "DielExtentType:=", "BboxExtent",
         "DielExt:=", ["Ext:=", length(DIELECTRIC_PAST), "Dim:=", True],
         "HonorUserDiel:=", False,
         "ExtentType:=", "BboxExtent",
         "TruncAtGnd:=", TRUNCATE_AT_GROUND,
         "AirHorExt:=", ["Ext:=", length(AIR_PAST), "Dim:=", True],
         "AirPosZExt:=", ["Ext:=", length(AIR_PAST), "Dim:=", True],
         "AirNegZExt:=", ["Ext:=", length(AIR_PAST), "Dim:=", True],
         "UseStackupForZExtFact:=", False,
         "OpenRegionType:=", "Radiation",
         "UseRadBound:=", True])


def edge_number(primitive, number, ends):
    """The number Electronics Desktop gives the edge of primitive between the
    two points of ends: number, as it was written, unless what it reads back
    says otherwise. Its points read back are in metres."""
    if ends is None:
        return number
    try:
        points = [(p.GetX() * 1e6, p.GetY() * 1e6) for p in oEditor.GetPolygon(primitive).GetPoints()]
    except Exception:
        return number
    near = 1e-3
    for k in range(len(points)):
        a, b = points[k], points[(k + 1) % len(points)]
        forth = abs(a[0] - ends[0]) + abs(a[1] - ends[1]) + abs(b[0] - ends[2]) + abs(b[1] - ends[3])
        back = abs(b[0] - ends[0]) + abs(b[1] - ends[1]) + abs(a[0] - ends[2]) + abs(a[1] - ends[3])
        if min(forth, back) < near:
            if k != number:
                say("%s: its edge %d is read back as %d" % (primitive, number, k))
            return k
    say("%s: no edge of it reads back where edge %d was written" % (primitive, number), 1)
    return number


def edges_of(listed):
    out = ["NAME:Contents"]
    for primitive, number, ends in listed:
        out += ["edge:=", ["et:=", "pe", "prim:=", primitive, "edge:=", edge_number(primitive, number, ends)]]
    return out


def set_property(port, name, value):
    attempt("%s: %s" % (port, name), oEditor.ChangeProperty,
            ["NAME:AllTabs",
             ["NAME:EM Design",
              ["NAME:PropServers", "Excitations:" + port],
              ["NAME:ChangedProps", ["NAME:" + name, "Value:=", value]]]])


# The ports, in the order of the S matrix.
for port in PORTS:
    before = list(oExcitations.GetAllPortsList())
    if port["kind"] == "point":
        attempt("port " + port["name"], oEditor.CreateCircuitPort,
                ["NAME:Location",
                 "PosLayer:=", port["positive"][0], "X0:=", length(port["positive"][1]), "Y0:=", length(port["positive"][2]),
                 "NegLayer:=", port["negative"][0], "X1:=", length(port["negative"][1]), "Y1:=", length(port["negative"][2])])
    else:
        contents = edges_of(port["edges"]) + ["circuit:=", port["kind"] == "circuit", "btype:=", 0]
        attempt("port " + port["name"], oEditor.CreateEdgePort, contents)
    made = [p for p in oExcitations.GetAllPortsList() if p not in before]
    if not made:
        say("port %s was not made" % port["name"], 1)
        continue
    name = made[0]
    if name != port["name"]:
        try:
            oExcitations.Rename(name, port["name"])
            name = port["name"]
        except Exception:
            say("port %s is called %s" % (port["name"], name))
    if port["references"]:
        if port["kind"] == "circuit":
            attempt(name + ": its returns", oEditor.AddCircuitRefPort, [name], edges_of(port["references"]))
        else:
            attempt(name + ": its returns", oEditor.AddRefPort, [name], edges_of(port["references"]))
    if port["type"]:
        set_property(name, "HFSS Type", port["type"])
    set_property(name, "PortSolver", port["solver"])
    set_property(name, "Renormalize", "true")
    set_property(name, "Renormalize Impedence", "%.12gohm + 0i ohm" % port["impedance"])
    if port["deembed"] != 0:
        set_property(name, "Deembed", "true")
        set_property(name, "Deembed Distance", length(port["deembed"]))

# The setup and its sweep. (The names and the values are the setup's own, as a
# project keeps them: the adaptive frequency, its Delta S and passes are ours.)
attempt("the setup", oSetups.Add,
        ["NAME:" + SETUP,
         ["NAME:Properties", "Enable:=", "true"],
         "CustomSetup:=", False,
         "AutoSetup:=", False,
         "SliderType:=", "Balanced",
         "SolveSetupType:=", "HFSS",
         "PercentRefinementPerPass:=", 30,
         "MinNumberOfPasses:=", 1,
         "MinNumberOfConvergedPasses:=", 1,
         "UseDefaultLambda:=", True,
         "UseMaxRefinement:=", False,
         "MaxRefinement:=", 1000000,
         "SaveAdaptiveCurrents:=", False,
         "SaveLastAdaptiveRadFields:=", False,
         "ProdMajVerID:=", -1,
         "ProjDesignSetup:=", "",
         "ProdMinVerID:=", -1,
         "Refine:=", False,
         "Frequency:=", ADAPTIVE,
         "LambdaRefine:=", True,
         "MeshSizeFactor:=", 1.5,
         "QualityRefine:=", True,
         "MinAngle:=", "15deg",
         "UniformityRefine:=", False,
         "MaxRatio:=", 2,
         "Smooth:=", False,
         "SmoothingPasses:=", 5,
         "UseEdgeMesh:=", False,
         "UseEdgeMeshAbsLength:=", False,
         "EdgeMeshRatio:=", 0.1,
         "EdgeMeshAbsLength:=", "1000mm",
         "LayerProjectThickness:=", "0meter",
         "UseDefeature:=", True,
         "UseDefeatureAbsLength:=", False,
         "DefeatureRatio:=", 1e-06,
         "DefeatureAbsLength:=", "0mm",
         "DoAdaptive:=", True,
         ["NAME:AdvancedSettings",
          "AccuracyLevel:=", 2,
          "GapPortCalibration:=", True,
          "ReferenceLengthRatio:=", 0.25,
          "RefineAreaRatio:=", 4,
          "DRCOn:=", False,
          "ZeroMetalLayerThickness:=", THIN_METAL,
          "ThinDielectric:=", 0,
          "SolverType:=", "Direct Solver",
          "CausalMaterials:=", True],
         ["NAME:AdaptiveSettings",
          "DoAdaptive:=", True,
          "SaveFields:=", False,
          "SaveRadFieldsOnly:=", False,
          "MaxRefinePerPass:=", 30,
          "MinPasses:=", 1,
          "MinConvergedPasses:=", 1,
          "AdaptType:=", "kSingle",
          "Basic:=", True,
          ["NAME:SingleFrequencyDataList",
           ["NAME:AdaptiveFrequencyData",
            "AdaptiveFrequency:=", ADAPTIVE,
            "MaxDelta:=", str(MAX_DELTA),
            "MaxPasses:=", MAX_PASSES,
            "Expressions:=", []]],
          ["NAME:BroadbandFrequencyDataList"],
          ["NAME:MultiFrequencyDataList"]],
         ["NAME:Data"],
         ["NAME:MeshOps"]])
attempt("the sweep", oSetups.AddSweep, SETUP,
        ["NAME:" + SWEEP,
         ["NAME:Properties", "Enable:=", "true"],
         ["NAME:Sweeps", "Variable:=", SWEEP, "Data:=", SWEEP_DATA, "OffsetF1:=", False, "Synchronize:=", 0],
         "GenerateSurfaceCurrent:=", False,
         "SaveRadFieldsOnly:=", False,
         "ZoSelected:=", False,
         "SAbsError:=", 0.005,
         "ZoPercentError:=", 1,
         "GenerateStateSpace:=", False,
         "EnforcePassivity:=", False,
         "PassivityTolerance:=", 0.0001,
         "UseQ3DForDC:=", False,
         "ResimulateDC:=", False,
         "MaxSolutions:=", 250,
         "InterpUseSMatrix:=", True,
         "InterpUsePortImpedance:=", True,
         "InterpUsePropConst:=", True,
         "InterpUseFullBasis:=", True,
         "AdvDCExtrapolation:=", False,
         "MinSolvedFreq:=", "0.01GHz",
         "AutoSMatOnlySolve:=", True,
         "MinFreqSMatrixOnlySolve:=", "1MHz",
         "CustomFrequencyString:=", "",
         "AllEntries:=", False,
         "AllDiagEntries:=", False,
         "AllOffDiagEntries:=", False,
         "MagMinThreshold:=", 0.01,
         "FreqSweepType:=", SWEEP_TYPE])

# Kept as a project beside this script.
project = os.path.join(HERE, NAME + ".aedt").replace("\\", "/")
try:
    oProject.SaveAs(project, True, "ef_overwrite", ["NAME:OverrideActions"])
except Exception:
    attempt("saving the project", oProject.SaveAs, project, True)

# What came of it, to look at first when something is not as it should be.
say("%d layers, %d shapes, %d vias, %d ports" % (len(LAYERS), len(POLYGONS) + len(PLANES), len(VIAS), len(PORTS)))
attempt("reading the layers back", lambda: say("layers: " + ", ".join(oEditor.GetStackupLayerNames())))
attempt("reading the ports back", lambda: say("ports: " + ", ".join(oExcitations.GetAllPortsList())))

# "solve" among the script's arguments: solved, its S written beside it.
try:
    arguments = ScriptArgument
except NameError:
    arguments = ""
if "solve" in str(arguments):
    attempt("the solve", oDesign.Analyze, SETUP)
    written = os.path.join(HERE, "%s_hfss.s%dp" % (NAME, len(PORTS))).replace("\\", "/")
    attempt("writing " + written, oDesign.ExportNetworkData,
            "", [SETUP + ":" + SWEEP], 3, written, ["all"], RENORMALIZE > 0, max(RENORMALIZE, 50), "S", -1, 0, 15, True, True, False)
    attempt("saving the project", oProject.Save)

with open(os.path.join(HERE, NAME + "_aedt_report.txt"), "w") as report:
    report.write("\n".join(said) + "\n")
    if trouble:
        report.write("\n%d thing(s) went wrong; each is said above.\n" % len(trouble))
)PY";

} // namespace

HfssExport hfssExportOf(const Model& model, const QList<Conductor>& conductors, const HfssOptions& options, const QString& name,
                        const QString& madeBy, const QList<bool>& returns)
{
    HfssExport out;
    out.name = name.isEmpty() ? QStringLiteral("RF25") : name;
    {
        QSet<QString> none;
        out.name = safeName(out.name, &none);
    }
    const Stack stack = model.stack();
    if (!stack.ok()) out.problems << stack.problems;
    QString why;
    const QList<double> sweep = studySweep(model, &why);
    if (sweep.isEmpty()) out.problems << (why.isEmpty() ? tr("The study asks for no frequency.") : why);
    if (conductors.isEmpty()) out.problems << tr("The model has no shapes to write: its layout has not been read, or nothing of it is mapped to the stack.");
    if (options.portType != QLatin1String("auto") && options.portType != QLatin1String("wave") && options.portType != QLatin1String("gap"))
        out.problems << tr("The port type is \"auto\", \"wave\" or \"gap\", not \"%1\".").arg(options.portType);
    if (!(options.maxDelta > 0)) out.problems << tr("The adaptive Delta S is a number above nought.");
    if (options.maxPasses < 1) out.problems << tr("The adaptive passes are at least one.");
    if (!out.problems.isEmpty()) return out;

    const double top = sweep.last();
    out.adaptive = options.adaptive > 0 ? options.adaptive : top;
    const bool wholeNumber = std::abs(out.adaptive / 1e9 - std::round(out.adaptive / 1e9)) < 1e-9;
    const QString adaptive = wholeNumber ? QStringLiteral("%1GHz").arg(num(out.adaptive / 1e9)) : QStringLiteral("%1Hz").arg(num(out.adaptive));

    // ---- The stack, from the bottom up ----
    QSet<QString> layerNames, materialNames;
    QList<HLayer> layers;
    QList<HMaterial> materials;
    const auto addMaterial = [&materials, &materialNames](const QString& wanted, double epsr, double tand, double sigma) {
        HMaterial m;
        m.name = safeName(QStringLiteral("rf25_") + wanted, &materialNames);
        m.epsr = epsr;
        m.tand = tand;
        m.sigma = sigma;
        materials << m;
        return m.name;
    };
    const QString air = addMaterial(QStringLiteral("air"), 1, 0, 0);

    // The height first: what a margin and a quarter wave are judged by.
    double height = 0;
    for (const Layer& l : stack.layers)
        if (l.kind != Layer::Ground && l.kind != Layer::Via && l.kind != Layer::Cover) height += l.thickness;

    int number = 0;
    double z = 0;
    QString groundLayer;
    bool plane = false;
    QString lidLayer;
    for (const Layer& l : stack.layers) {
        if (l.kind == Layer::Ground && l.variant != QLatin1String("open")) {
            HLayer g;
            g.name = safeName(l.name.isEmpty() ? QStringLiteral("GND") : l.name, &layerNames);
            g.of = l.name;
            g.signal = true;
            g.number = ++number;
            g.fill = air;
            if (l.variant == QLatin1String("metal") && l.sigma > 0) {
                // A skin depth's few at the bottom of the sweep: what a plane
                // of that conductivity is, seen from above.
                const double delta = std::sqrt(1 / (M_PI * sweep.first() * 4e-7 * M_PI * l.sigma));
                g.thickness = 5 * delta;
                g.material = addMaterial(g.name, 1, 0, l.sigma);
                out.notes << tr("%1: a metal plane, %2 µm thick here, five skin depths at the bottom of the sweep.").arg(g.name, um(g.thickness));
            } else if (l.variant == QLatin1String("impedance") && l.sigma > 0) {
                g.thickness = 1e-6;
                g.material = addMaterial(g.name, 1, 0, 1 / (l.sigma * g.thickness));
                out.notes << tr("%1: a surface impedance of %2 Ohm per square, here a sheet 1 µm thick of the conductivity that gives it.")
                                 .arg(g.name, num(l.sigma));
            } else {
                // (Electronics Desktop takes a conductivity from 1e30 as perfect, as its own pec is.)
                g.thickness = 0;
                g.material = addMaterial(g.name, 1, 0, 1e30);
            }
            g.z0 = z;
            z += g.thickness;
            groundLayer = g.name;
            plane = true;
            layers << g;
        }
    }
    QHash<int, QString> materialOf;   // a dielectric's, by its place in the stack
    QHash<QString, int> fillFrom;     // a metal's fill: the dielectric of that place
    for (int i = 0; i < stack.layers.size(); ++i) {
        const Layer& l = stack.layers.at(i);
        if (l.kind == Layer::Substrate || l.kind == Layer::Dielectric) {
            HLayer d;
            d.name = safeName(l.name, &layerNames);
            d.of = l.name;
            d.z0 = z;
            d.thickness = l.thickness;
            d.material = addMaterial(d.name, l.epsr, l.tand, l.kind == Layer::Substrate ? l.sigma : 0);
            materialOf.insert(i, d.material);
            z += l.thickness;
            layers << d;
        } else if (l.kind == Layer::Metal) {
            HLayer m;
            m.name = safeName(l.name, &layerNames);
            m.of = l.name;
            m.signal = true;
            m.z0 = z;
            m.thickness = l.thickness;
            m.number = ++number;
            m.material = addMaterial(m.name, 1, 0, l.sigma);
            m.roughness = l.roughness;
            // What is beside it, as the solve's two faces have it: the first
            // dielectric above it short of another metal, else the air.
            m.fill = air;
            for (int j = i + 1; j < stack.layers.size(); ++j) {
                const Layer& above = stack.layers.at(j);
                if (above.kind == Layer::Metal || above.kind == Layer::Cover) break;
                if (above.kind != Layer::Dielectric) continue;
                fillFrom.insert(m.name, j);
                break;
            }
            z += l.thickness;
            layers << m;
        } else if (l.kind == Layer::Cover && l.variant == QLatin1String("perfect")) {
            HLayer gap;
            gap.name = safeName(QStringLiteral("%1_gap").arg(l.name), &layerNames);
            gap.of = l.name;
            gap.z0 = z;
            gap.thickness = l.thickness;
            gap.material = air;
            z += l.thickness;
            layers << gap;
            HLayer lid;
            lid.name = safeName(l.name, &layerNames);
            lid.of = l.name;
            lid.signal = true;
            lid.z0 = z;
            lid.number = ++number;
            lid.fill = air;
            lid.material = addMaterial(lid.name, 1, 0, 1e30);
            lidLayer = lid.name;
            layers << lid;
            out.notes << tr("%1: a metal lid, which RF 2.5D does not solve yet: this model has no answer of its own to compare with.").arg(l.name);
        }
    }
    // (The dielectric above a metal comes after it.)
    for (HLayer& l : layers)
        if (l.signal && fillFrom.contains(l.name)) l.fill = materialOf.value(fillFrom.value(l.name), air);
    {
        bool used = false;
        for (const HLayer& l : std::as_const(layers)) used = used || l.fill == air || l.material == air;
        if (!used) materials.removeFirst();
    }
    const auto layerOf = [&layers](const QString& of) -> const HLayer* {
        for (const HLayer& l : layers)
            if (l.of == of && l.signal) return &l;
        return nullptr;
    };

    // ---- The shapes ----
    // Vias apart; a metal's shapes with their keyholes opened.
    QList<HfssPolygon> polygons;
    QList<int> polygonFrom;   // each polygon's conductor
    QSet<QString> shapeNames;
    QList<int> viaConductors;
    for (int k = 0; k < conductors.size(); ++k) {
        const Conductor& c = conductors.at(k);
        const Layer* l = stack.named(c.layer);
        if (l == nullptr) continue;
        if (l->kind == Layer::Via) {
            viaConductors << k;
            continue;
        }
        const HLayer* on = layerOf(c.layer);
        if (on == nullptr) continue;
        HfssPolygon p;
        p.layer = on->name;
        p.name = safeName(QStringLiteral("%1_%2").arg(on->name).arg(polygons.size() + 1), &shapeNames);
        QPolygonF outline;
        openKeyholes(c.outline, &outline, &p.holes);
        p.outline = counterClockwise(outline);
        if (p.outline.size() < 3) continue;
        polygons << p;
        polygonFrom << k;
    }
    out.polygons = int(polygons.size());
    for (const HfssPolygon& p : std::as_const(polygons)) out.voids += int(p.holes.size());

    // The box of everything: what the margin is past.
    QRectF box;
    for (const Conductor& c : conductors) box = box.isNull() ? c.outline.boundingRect() : box.united(c.outline.boundingRect());
    out.marginUsed = options.margin >= 0 ? options.margin : 5 * std::max(height, 1e-9);
    out.airUsed = options.air >= 0 ? options.air : kC0 / out.adaptive / 4;
    const double m = out.marginUsed * 1e6;
    const QRectF outer = box.adjusted(-m, -m, m, m);

    // ---- The vias ----
    struct HVia {
        QString name, padstack, upper, lower;
        QPointF at;
        int from = -1;     // its conductor
    };
    struct HPadstack {
        QString name, shape, material, upper, lower;
        QStringList sizes;
    };
    QList<HVia> vias;
    QList<HPadstack> padstacks;
    QMap<QString, QString> viaMaterials;
    int rounded = 0;
    for (const int k : std::as_const(viaConductors)) {
        const Conductor& c = conductors.at(k);
        const Layer* v = stack.named(c.layer);
        if (v == nullptr) continue;
        const bool fromGround = stack.isGround(v->from), toGround = stack.isGround(v->to);
        const HLayer* a = fromGround ? nullptr : layerOf(v->from);
        const HLayer* b = toGround ? nullptr : layerOf(v->to);
        const QString lowerName = fromGround ? groundLayer : a != nullptr ? a->name : QString();
        const QString upperName = toGround ? groundLayer : b != nullptr ? b->name : QString();
        if (lowerName.isEmpty() || upperName.isEmpty()) {
            out.notes << tr("A shape of the via %1 is not written: it joins a metal or a plane that is not there.").arg(v->name);
            continue;
        }
        if (!viaMaterials.contains(v->name)) viaMaterials.insert(v->name, addMaterial(QStringLiteral("via_") + v->name, 1, 0, v->sigma));
        // A rectangle as it is; anything else a circle of its area, at its middle.
        const QPolygonF outline = cleaned(c.outline);
        const QRectF bound = outline.boundingRect();
        const double area = std::abs(signedArea(outline));
        const bool rectangle = std::abs(area - bound.width() * bound.height()) <= 1e-6 * bound.width() * bound.height();
        HVia via;
        via.upper = upperName;
        via.lower = lowerName;
        via.from = k;
        HPadstack ps;
        ps.material = viaMaterials.value(v->name);
        ps.upper = upperName;
        ps.lower = lowerName;
        if (rectangle) {
            ps.shape = QStringLiteral("Rct");
            ps.sizes = QStringList{QStringLiteral("%1um").arg(num(bound.width())), QStringLiteral("%1um").arg(num(bound.height()))};
            via.at = bound.center();
        } else {
            ps.shape = QStringLiteral("Cir");
            ps.sizes = QStringList{QStringLiteral("%1um").arg(num(2 * std::sqrt(area / M_PI)))};
            ++rounded;
            // Its centroid.
            double cx = 0, cy = 0;
            for (int i = 0, n = int(outline.size()); i < n; ++i) {
                const QPointF& u = outline.at(i);
                const QPointF& w = outline.at((i + 1) % n);
                const double cross = u.x() * w.y() - w.x() * u.y();
                cx += (u.x() + w.x()) * cross;
                cy += (u.y() + w.y()) * cross;
            }
            const double signedTotal = signedArea(outline);
            via.at = std::abs(signedTotal) > 0 ? QPointF(cx / (6 * signedTotal), cy / (6 * signedTotal)) : bound.center();
        }
        const QString key = QStringLiteral("%1 %2 %3 %4 %5").arg(ps.shape, ps.sizes.join(QLatin1Char('x')), ps.material, ps.upper, ps.lower);
        int found = -1;
        for (int i = 0; i < padstacks.size() && found < 0; ++i) {
            const HPadstack& o = padstacks.at(i);
            if (QStringLiteral("%1 %2 %3 %4 %5").arg(o.shape, o.sizes.join(QLatin1Char('x')), o.material, o.upper, o.lower) == key) found = i;
        }
        if (found < 0) {
            ps.name = QStringLiteral("rf25_via_%1").arg(padstacks.size() + 1);
            padstacks << ps;
            found = int(padstacks.size()) - 1;
        }
        via.padstack = padstacks.at(found).name;
        via.name = safeName(QStringLiteral("via_%1").arg(vias.size() + 1), &shapeNames);
        vias << via;
    }
    out.vias = int(vias.size());
    if (rounded > 0)
        out.notes << tr("%1 via shape(s) that are no rectangles are written as circles of their area: a padstack's hole is "
                        "a shape of its own, and GDSII + XML keeps them as they are.").arg(rounded);

    // ---- The nets: what touches, and what a via joins ----
    const int nPoly = int(polygons.size()), nVia = int(vias.size());
    Sets sets(nPoly + nVia + 2);
    const int groundSet = nPoly + nVia, lidSet = groundSet + 1;
    QList<QRectF> boxes;
    boxes.reserve(nPoly);
    for (const HfssPolygon& p : std::as_const(polygons)) boxes << p.outline.boundingRect();
    for (int a = 0; a < nPoly; ++a)
        for (int b = a + 1; b < nPoly; ++b)
            if (polygons.at(a).layer == polygons.at(b).layer && overlapping(polygons.at(a).outline, boxes.at(a), polygons.at(b).outline, boxes.at(b))
                && !inAHole(polygons.at(a).holes, polygons.at(b).outline) && !inAHole(polygons.at(b).holes, polygons.at(a).outline))
                sets.join(a, b);
    for (int v = 0; v < nVia; ++v) {
        const HVia& via = vias.at(v);
        const QPolygonF shape = cleaned(conductors.at(via.from).outline);
        const QRectF bound = shape.boundingRect();
        if (via.lower == groundLayer || via.upper == groundLayer) sets.join(nPoly + v, groundSet);
        for (int a = 0; a < nPoly; ++a) {
            if (polygons.at(a).layer != via.upper && polygons.at(a).layer != via.lower) continue;
            if (overlapping(polygons.at(a).outline, boxes.at(a), shape, bound) && !inAHole(polygons.at(a).holes, shape)) sets.join(a, nPoly + v);
        }
    }
    QHash<int, QString> netNames;
    netNames.insert(sets.find(groundSet), QStringLiteral("GND"));
    netNames.insert(sets.find(lidSet), QStringLiteral("LID"));
    const auto netOf = [&sets, &netNames](int k) {
        const int root = sets.find(k);
        if (!netNames.contains(root)) netNames.insert(root, QStringLiteral("N%1").arg(netNames.size() - 1));
        return netNames.value(root);
    };
    for (int a = 0; a < nPoly; ++a) polygons[a].net = netOf(a);
    QStringList viaNets;
    for (int v = 0; v < nVia; ++v) viaNets << netOf(nPoly + v);

    // ---- The ports ----
    // Which polygon and which of its edges lies along a line.
    QList<Conductor> outlines;
    QList<bool> outlineReturns;
    for (int a = 0; a < nPoly; ++a) {
        Conductor c;
        c.layer = conductors.at(polygonFrom.at(a)).layer;
        c.outline = polygons.at(a).outline;
        outlines << c;
        outlineReturns << returns.value(polygonFrom.at(a), true);
    }
    struct Edge {
        int polygon = -1, number = -1;
        QLineF line;
    };
    const auto edgeAlong = [&polygons, nPoly](const QLineF& line) {
        Edge best;
        for (int a = 0; a < nPoly && best.polygon < 0; ++a) {
            const QPolygonF& o = polygons.at(a).outline;
            for (int i = 0, n = int(o.size()); i < n; ++i) {
                const QPointF& u = o.at(i);
                const QPointF& w = o.at((i + 1) % n);
                if ((u == line.p1() && w == line.p2()) || (u == line.p2() && w == line.p1())) {
                    best.polygon = a;
                    best.number = i;
                    best.line = QLineF(u, w);
                    break;
                }
            }
        }
        return best;
    };
    // The edges on a stretch of a line beside a face, on its layer.
    const auto edgesOn = [&polygons, &conductors, &polygonFrom, nPoly](const QLineF& stretch, const QString& layer) {
        QList<Edge> found;
        const double len = stretch.length();
        if (!(len > 0)) return found;
        const QPointF unit = (stretch.p2() - stretch.p1()) / len, normal(-unit.y(), unit.x());
        constexpr double kNear = 1e-6;
        for (int a = 0; a < nPoly; ++a) {
            if (conductors.at(polygonFrom.at(a)).layer != layer) continue;
            const QPolygonF& o = polygons.at(a).outline;
            for (int i = 0, n = int(o.size()); i < n; ++i) {
                const QPointF u = o.at(i) - stretch.p1(), w = o.at((i + 1) % n) - stretch.p1();
                if (std::abs(u.x() * normal.x() + u.y() * normal.y()) > kNear * std::max(1.0, len)
                    || std::abs(w.x() * normal.x() + w.y() * normal.y()) > kNear * std::max(1.0, len))
                    continue;
                const double s0 = u.x() * unit.x() + u.y() * unit.y(), s1 = w.x() * unit.x() + w.y() * unit.y();
                const double middle = (s0 + s1) / 2;
                if (middle < -kNear || middle > len + kNear) continue;
                Edge e;
                e.polygon = a;
                e.number = i;
                e.line = QLineF(o.at(i), o.at((i + 1) % n));
                found << e;
            }
        }
        return found;
    };
    // The port-kind nodes by name, for a return that names one.
    QHash<QString, const Node*> byName;
    for (const Node& p : model.ports().children)
        if (p.enabled) byName.insert(portName(p), &p);
    const auto pointOf = [&model](const Node& node) {
        return QPointF(numberOf(node, QStringLiteral("x"), model, 0), numberOf(node, QStringLiteral("y"), model, 0)) / 1e-6;
    };
    const auto faceOfWay = [](const QString& way) {
        if (way == QLatin1String("+x")) return QPointF(1, 0);
        if (way == QLatin1String("-x")) return QPointF(-1, 0);
        if (way == QLatin1String("+y")) return QPointF(0, 1);
        if (way == QLatin1String("-y")) return QPointF(0, -1);
        return QPointF();
    };
    // The edge a return's terminal stands for: its face, else the edge of the
    // shape under its point nearest that point.
    const auto edgeOfTerminal = [&](const Node& t, Edge* e) {
        const QString metal = t.type == QLatin1String("rf_terminal")      ? textOf(t, QStringLiteral("metal")).trimmed()
                              : t.type == QLatin1String("rf_edge_port") ? textOf(t, QStringLiteral("conductor")).trimmed()
                                                                         : textOf(t, QStringLiteral("to")).trimmed();
        QLineF line;
        const QPointF way = faceOfWay(t.type == QLatin1String("rf_terminal") ? textOf(t, QStringLiteral("face")) : textOf(t, QStringLiteral("facing")));
        if (!faceEdge(outlines, pointOf(t), metal, way, &line)) return false;
        *e = edgeAlong(line);
        return e->polygon >= 0;
    };
    QSet<QString> portNames;
    QStringList portLines;
    QStringList xmlPorts;
    for (const Node& p : model.ports().children) {
        if (!p.enabled) continue;
        const bool edgePort = p.type == QLatin1String("rf_edge_port");
        if (!edgePort && p.type != QLatin1String("rf_internal_port")) continue;
        const QString own = portName(p);
        const QString name = safeName(own, &portNames);
        const double impedance = numberOf(p, QStringLiteral("impedance"), model, 50);
        const bool named = textOf(p, QStringLiteral("return")) == QLatin1String("terminals");
        if (!textOf(p, QStringLiteral("pair")).trimmed().isEmpty())
            out.notes << tr("%1: its differential pair is not set up in HFSS: compare the ports' S, whose modes follow from it, "
                            "or pair them there (Differential Pairs).").arg(own);
        QString kind, type, sayIt;
        QString solver = QStringLiteral("true");
        QList<Edge> edges, references;
        QString positive, negative;   // (of a point port)
        QPointF at = pointOf(p), negativeAt = at;
        double deembed = 0;
        Edge main;
        if (edgePort) {
            const QString metal = textOf(p, QStringLiteral("conductor")).trimmed();
            const QPointF way = faceOfWay(textOf(p, QStringLiteral("facing")));
            QLineF face;
            QPointF outward;
            QString onLayer;
            if (!faceEdge(outlines, at, metal, way, &face, &outward, &onLayer) || (main = edgeAlong(face)).polygon < 0) {
                out.notes << tr("%1 is not written: no edge of a shape on %2 near its point faces the way it says.").arg(own, metal);
                continue;
            }
            edges << main;
            const QString layerName = polygons.at(main.polygon).layer;
            const bool calibrate = flagOf(p, QStringLiteral("calibrate"));
            deembed = numberOf(p, QStringLiteral("deembed"), model, 0);
            const bool wave = options.portType == QLatin1String("wave")
                              || (options.portType == QLatin1String("auto") && (calibrate || deembed != 0));
            const QString returnKind = textOf(p, QStringLiteral("return"));
            if (returnKind == QLatin1String("beside")) {
                const QList<bool>* marks = returns.isEmpty() ? nullptr : &outlineReturns;
                const std::array<QLineF, 2> beside = besideEdges(outlines, face, outward, onLayer, marks);
                for (const QLineF& stretch : beside) references += edgesOn(stretch, onLayer);
                if (references.isEmpty())
                    out.notes << tr("%1 returns through the conductors beside it, and there are none on the line of its edge.").arg(own);
            } else if (named) {
                for (const QString& r : namesOf(p, QStringLiteral("returns"))) {
                    const Node* other = byName.value(r, nullptr);
                    Edge e;
                    if (other != nullptr && edgeOfTerminal(*other, &e)) references << e;
                    else out.notes << tr("%1: its return %2 has no edge to reference.").arg(own, r);
                }
            }
            if (named) {
                // As RF 2.5D has it: a source between its own cells and its
                // returns', with no path of its own - a circuit port's.
                kind = QStringLiteral("circuit");
                sayIt = tr("%1: circuit port on an edge of %2, its returns the %3 edge(s) of its terminals, tied").arg(name, layerName).arg(references.size());
                out.notes << tr("%1: its returns are the edges nearest each terminal, not the cells under them: a probe's tips land "
                                "on a pad's area, a circuit port's terminal on an edge.").arg(own);
                solver = QStringLiteral("false");
            } else {
                kind = QStringLiteral("edge");
                type = wave ? QStringLiteral("Wave") : QStringLiteral("Gap");
                solver = calibrate || wave ? QStringLiteral("true") : QStringLiteral("false");
                sayIt = references.isEmpty()
                            ? tr("%1: %2 port on an edge of %3, referenced to the nearest plane").arg(name, type.toLower(), layerName)
                            : tr("%1: %2 port on an edge of %3, referenced to %4 edge(s) beside it").arg(name, type.toLower(), layerName).arg(references.size());
                if (calibrate && !wave)
                    out.notes << tr("%1: RF 2.5D takes its own discontinuity out by strips of its feed; here a gap port with its port solver on.").arg(own);
            }
            if (deembed != 0) {
                if (textOf(p, QStringLiteral("along")) == QLatin1String("line"))
                    out.notes << tr("%1: moved %2 µm along a line RF 2.5D is given (%3 Ohm, eps %4); HFSS moves it along the line its port solves.")
                                     .arg(own, um(deembed), num(numberOf(p, QStringLiteral("line_z0"), model, 50)), num(numberOf(p, QStringLiteral("line_eps"), model, 1)));
                sayIt += tr(", de-embedded %1 µm").arg(um(deembed));
            }
        } else {
            // A point, and a metal above another or the ground under it.
            const QString to = textOf(p, QStringLiteral("to")).trimmed();
            QString from = textOf(p, QStringLiteral("from")).trimmed();
            const HLayer* upper = layerOf(to);
            if (upper == nullptr) {
                out.notes << tr("%1 is not written: its To metal %2 is no metal of the stack.").arg(own, to);
                continue;
            }
            positive = upper->name;
            if (named) {
                const QStringList listed = namesOf(p, QStringLiteral("returns"));
                // The source on an edge of its own pad; the returns, each the
                // nearest edge of its terminal, tied.
                QLineF face;
                if (!faceEdge(outlines, at, to, QPointF(), &face) || (main = edgeAlong(face)).polygon < 0) {
                    out.notes << tr("%1 is not written: there is no shape of %2 under its point.").arg(own, to);
                    continue;
                }
                for (const QString& r : listed) {
                    const Node* other = byName.value(r, nullptr);
                    Edge e;
                    if (other != nullptr && edgeOfTerminal(*other, &e)) references << e;
                    else out.notes << tr("%1: its return %2 has no edge to reference.").arg(own, r);
                }
                edges << main;
                kind = QStringLiteral("circuit");
                solver = QStringLiteral("false");
                sayIt = tr("%1: circuit port on the edge of %2 nearest its point, its returns the %3 edge(s) of its terminals, tied").arg(name, positive).arg(references.size());
                out.notes << tr("%1: its own cells and its returns' are edges here, each the one nearest the point: a circuit port's "
                                "terminal is an edge or a point, not an area.").arg(own);
            } else {
                kind = QStringLiteral("point");
                solver = QStringLiteral("false");
                const HLayer* lower = from.isEmpty() || stack.isGround(from) ? nullptr : layerOf(from);
                negative = lower != nullptr ? lower->name : groundLayer;
                if (negative.isEmpty()) {
                    out.notes << tr("%1 is not written: it returns through a ground plane the stack has not.").arg(own);
                    continue;
                }
                sayIt = tr("%1: circuit port at a point, %2 against %3").arg(name, positive, negative);
                const double w = numberOf(p, QStringLiteral("width"), model, 0), h = numberOf(p, QStringLiteral("height"), model, 0);
                if (w > 0 || h > 0)
                    out.notes << tr("%1: RF 2.5D drives the cells of a rectangle %2 by %3 µm about its point; a circuit port here drives its point.")
                                     .arg(own, um(w), um(h));
            }
        }
        // As a line of the script.
        const auto edgeText = [&polygons](const QList<Edge>& list) {
            QStringList items;
            for (const Edge& e : list)
                items << QStringLiteral("(%1, %2, (%3, %4, %5, %6))")
                             .arg(py(polygons.at(e.polygon).name)).arg(e.number)
                             .arg(num(e.line.x1()), num(e.line.y1()), num(e.line.x2()), num(e.line.y2()));
            return QStringLiteral("[%1]").arg(items.join(QStringLiteral(", ")));
        };
        QString line = QStringLiteral("    {\"name\": %1, \"kind\": %2, \"type\": %3, \"solver\": %4, \"impedance\": %5, \"deembed\": %6,\n     ")
                           .arg(py(name), py(kind), py(type), py(solver), num(impedance), um(deembed));
        if (kind == QLatin1String("point")) {
            line += QStringLiteral("\"positive\": (%1, %2, %3), \"negative\": (%4, %5, %6), \"edges\": [], \"references\": []},")
                        .arg(py(positive), num(at.x()), num(at.y()), py(negative), num(negativeAt.x()), num(negativeAt.y()));
        } else {
            line += QStringLiteral("\"edges\": %1, \"references\": %2},").arg(edgeText(edges), edgeText(references));
        }
        portLines << line;
        out.portSaid << sayIt;
        ++out.ports;
        // The control file has points alone: a point on the edge, against a
        // point on the plane under it or on the nearest return.
        QPointF x1 = at, x2 = at;
        QString layer1 = positive, layer2 = negative;
        if (kind != QLatin1String("point")) {
            x1 = main.line.center();
            layer1 = polygons.at(main.polygon).layer;
            if (!references.isEmpty()) {
                x2 = references.first().line.center();
                layer2 = polygons.at(references.first().polygon).layer;
            } else {
                x2 = x1;
                layer2 = groundLayer;
            }
        }
        if (layer2.isEmpty())
            out.notes << tr("%1 is not in the control file: a circuit port there wants a second point, and it returns through nothing beside it or under it.").arg(own);
        else
            xmlPorts << QStringLiteral("    <CircuitPortPt Name=\"%1\" x1=\"%2\" y1=\"%3\" Layer1=\"%4\" x2=\"%5\" y2=\"%6\" Layer2=\"%7\" Z0=\"%8\"/>")
                            .arg(xml(name), num(x1.x()), num(x1.y()), xml(layer1), num(x2.x()), num(x2.y()), xml(layer2), num(impedance));
    }
    if (out.ports == 0) {
        out.problems << tr("No port could be written: the model's ports are on no edge or metal that is there.");
        return out;
    }
    bool sameReference = true;
    double firstReference = -1;
    for (const Node& p : model.ports().children) {
        if (!p.enabled || (p.type != QLatin1String("rf_edge_port") && p.type != QLatin1String("rf_internal_port"))) continue;
        const double z0 = numberOf(p, QStringLiteral("impedance"), model, 50);
        if (firstReference < 0) firstReference = z0;
        sameReference = sameReference && z0 == firstReference;
    }

    // ---- The sweep ----
    const Node* studyNode = model.root.child(QStringLiteral("rf_study"));
    const bool logarithmic = studyNode != nullptr && textOf(*studyNode, QStringLiteral("scale")) == QLatin1String("log");
    if (studyNode != nullptr && flagOf(*studyNode, QStringLiteral("adaptive")))
        out.notes << tr("The study adds points where S changes: its points as it asks for them are written.");
    if (sweep.size() == 1) {
        out.sweepData = QStringLiteral("SINGLE %1Hz").arg(num(sweep.first()));
    } else if (logarithmic) {
        QStringList each;
        for (const double f : sweep) each << QStringLiteral("SINGLE %1Hz").arg(num(f));
        out.sweepData = each.join(QLatin1Char(' '));
    } else {
        out.sweepData = QStringLiteral("LINC %1Hz %2Hz %3").arg(num(sweep.first()), num(sweep.last())).arg(sweep.size());
    }
    out.setup = QStringLiteral("HFSS Setup 1");
    out.sweep = QStringLiteral("Sweep 1");

    // ---- Where the two solvers differ, whatever the model ----
    out.notes << tr("RF 2.5D has the stack without end across the plane; here the dielectrics%1 stop %2 µm past the shapes and "
                    "a radiation boundary stands %3 µm past that: make them larger if S moves with them.")
                     .arg(plane ? tr(" and the plane") : QString(), um(out.marginUsed), um(out.airUsed));
    bool sheets = false, rough = false;
    for (const Layer& l : stack.layers) {
        sheets = sheets || (l.kind == Layer::Metal && !l.twoSheets);
        rough = rough || (l.kind == Layer::Metal && l.roughness > 0);
    }
    if (sheets)
        out.notes << (options.thinMetal > 0
                          ? tr("Metals thinner than %1 µm are modeled here as zero thickness, as RF 2.5D solves them as sheets.").arg(um(options.thinMetal))
                          : tr("A metal RF 2.5D solves as a sheet has its thickness in its loss alone, and the dielectrics above it "
                               "keep their heights over its bottom; here it has its thickness in the field, what is above it that much "
                               "higher. Its Solved as \"faces\" solves the same geometry."));
    if (rough)
        out.notes << tr("Roughness: RF 2.5D multiplies the surface resistance by Hammerstad's factor of the rms height; here Groisse's model of the same height.");
    bool viaFill = false;
    for (const Node& n : model.stackNode().children)
        viaFill = viaFill || (n.type == QLatin1String("rf_via") && numberOf(n, QStringLiteral("fill"), model, 1) != 1);
    if (viaFill) out.notes << tr("A via's Filled fraction is not written: its hole is solid.");

    out.layers = int(layers.size());
    out.materials = int(materials.size());
    if (plane) ++out.planes;
    if (!lidLayer.isEmpty()) ++out.planes;

    // ---- The GDSII file's shapes ----
    QHash<QString, int> gdsLayer;
    for (HLayer& l : layers) {
        if (l.signal) gdsLayer.insert(l.name, l.number);
    }
    int viaNumber = number;
    QHash<QString, int> viaGds;
    for (const int k : std::as_const(viaConductors)) {
        const QString& via = conductors.at(k).layer;
        if (!viaGds.contains(via)) viaGds.insert(via, ++viaNumber);
    }
    // (The file has the keyholes: it is how GDSII holds a hole.)
    for (int k = 0; k < conductors.size(); ++k) {
        const Conductor& c = conductors.at(k);
        if (viaGds.contains(c.layer)) {
            out.gdsShapes << qMakePair(viaGds.value(c.layer), c.outline);
            continue;
        }
        const HLayer* on = layerOf(c.layer);
        if (on != nullptr) out.gdsShapes << qMakePair(on->number, c.outline);
    }
    const QPolygonF outerPolygon(outer);
    if (plane) out.gdsShapes << qMakePair(gdsLayer.value(groundLayer), outerPolygon);
    if (!lidLayer.isEmpty()) out.gdsShapes << qMakePair(gdsLayer.value(lidLayer), outerPolygon);

    // ---- The script ----
    QString s;
    s += QStringLiteral("# -*- coding: utf-8 -*-\n");
    s += QStringLiteral("# An Ansys\u00ae Electronics Desktop script: builds the HFSS\u2122 3D Layout design %1 of an RF 2.5D model.\n").arg(out.name);
    s += QStringLiteral("# Written by %1 on %2.\n").arg(madeBy.isEmpty() ? QStringLiteral("Qucs-S") : madeBy, QDateTime::currentDateTime().toString(Qt::ISODate));
    s += QStringLiteral("#\n");
    s += QStringLiteral("# Tools > Run Script, or with no window:\n");
    s += QStringLiteral("#   ansysedt -RunScriptAndExit <this file>\n");
    s += QStringLiteral("# It keeps the project beside it as %1.aedt, and what came of it in %1_aedt_report.txt. With the\n").arg(out.name);
    s += QStringLiteral("# argument \"solve\" (the Run Script window's, or -ScriptArgs solve) it solves it too, and writes %1_hfss.s%2p:\n").arg(out.name).arg(out.ports);
    s += QStringLiteral("# RF 2.5D > Compare With Touchstone reads that beside its own solve.\n");
    s += QStringLiteral("#\n");
    s += QStringLiteral("# This script has not been run in Electronics Desktop by whoever wrote it: its report says what went wrong, if\n");
    s += QStringLiteral("# anything. The GDSII file and the XML control file of its name are a second way in (File > Import > GDSII).\n");
    s += QStringLiteral("#\n");
    s += QStringLiteral("# Where this is not the same as what RF 2.5D solves:\n");
    for (const QString& note : std::as_const(out.notes)) s += QStringLiteral("#   - %1\n").arg(QString(note).replace(QLatin1Char('\n'), QLatin1Char(' ')));
    s += QStringLiteral("#\n");
    // For Qucs-S: the model itself, and its shapes where its file is not there.
    {
        QJsonObject carried;
        carried.insert(QStringLiteral("format"), QStringLiteral("qucs-s rf 2.5d hfss 1"));
        carried.insert(QStringLiteral("model"), model.toObject());
        if (!options.modelFolder.isEmpty()) carried.insert(QStringLiteral("folder"), options.modelFolder);
        QJsonArray shapes;
        for (const auto& [gds, polygon] : std::as_const(out.gdsShapes)) {
            QJsonArray points;
            for (const QPointF& q : polygon) {
                points.append(q.x());
                points.append(q.y());
            }
            shapes.append(QJsonObject{{QStringLiteral("gds"), gds}, {QStringLiteral("points"), points}});
        }
        carried.insert(QStringLiteral("shapes"), shapes);
        QJsonArray map;
        for (const HLayer& l : std::as_const(layers))
            if (l.signal && l.number > 0) {
                QJsonArray row;
                row.append(QString::number(l.number));
                row.append(QStringLiteral("0"));
                row.append(l.of);
                map.append(row);
            }
        for (auto it = viaGds.constBegin(); it != viaGds.constEnd(); ++it) {
            QJsonArray row;
            row.append(QString::number(it.value()));
            row.append(QStringLiteral("0"));
            row.append(it.key());
            map.append(row);
        }
        carried.insert(QStringLiteral("map"), map);
        s += QStringLiteral("# For Qucs-S: the model itself, which RF 2.5D > Import reads back; Electronics Desktop passes over it.\n");
        s += QStringLiteral("#qucs-s %1\n").arg(QString::fromUtf8(QJsonDocument(carried).toJson(QJsonDocument::Compact)));
    }
    s += QStringLiteral("\nNAME = %1\n").arg(py(out.name));
    s += QStringLiteral("SETUP = %1\nSWEEP = %2\n").arg(py(out.setup), py(out.sweep));
    s += QStringLiteral("ADAPTIVE = %1\nMAX_DELTA = %2\nMAX_PASSES = %3\n").arg(py(adaptive), num(options.maxDelta)).arg(options.maxPasses);
    s += QStringLiteral("SWEEP_DATA = %1\nSWEEP_TYPE = %2\n").arg(py(out.sweepData), py(options.discrete ? QStringLiteral("kDiscrete") : QStringLiteral("kInterpolating")));
    s += QStringLiteral("THIN_METAL = %1\n").arg(options.thinMetal > 0 ? py(um(options.thinMetal) + QStringLiteral("um")) : QStringLiteral("0"));
    s += QStringLiteral("RENORMALIZE = %1\n").arg(sameReference && firstReference > 0 ? num(firstReference) : QStringLiteral("0"));
    s += QStringLiteral("# µm throughout, as the layout's own units.\n");
    s += QStringLiteral("DIELECTRIC_PAST = %1\nAIR_PAST = %2\nTRUNCATE_AT_GROUND = %3\n").arg(plane ? QStringLiteral("0") : um(out.marginUsed), um(out.airUsed), plane ? QStringLiteral("True") : QStringLiteral("False"));
    s += QStringLiteral("\n# name, permittivity, loss tangent, conductivity (S/m)\nMATERIALS = [\n");
    for (const HMaterial& m : std::as_const(materials))
        s += QStringLiteral("    (%1, %2, %3, %4),\n").arg(py(m.name), num(m.epsr), num(m.tand), num(m.sigma));
    s += QStringLiteral("]\n\n# name, type, lower elevation, thickness, material, fill, colour (bbggrr), roughness (rms): from the bottom up\nLAYERS = [\n");
    for (const HLayer& l : std::as_const(layers))
        s += QStringLiteral("    (%1, %2, %3, %4, %5, %6, %7, %8),\n")
                 .arg(py(l.name), py(l.signal ? QStringLiteral("signal") : QStringLiteral("dielectric")), um(l.z0), um(l.thickness), py(l.material), py(l.fill))
                 .arg(bgr(layout::paletteStyle(l.number > 0 ? l.number : 8).fill))
                 .arg(um(l.roughness));
    s += QStringLiteral("]\n\n# name, layer, net, the corners of a rectangle\nPLANES = [\n");
    if (plane)
        s += QStringLiteral("    (%1, %2, \"GND\", %3, %4, %5, %6),\n").arg(py(groundLayer + QStringLiteral("_plane")), py(groundLayer), num(outer.left()), num(outer.top()), num(outer.right()), num(outer.bottom()));
    if (!lidLayer.isEmpty())
        s += QStringLiteral("    (%1, %2, \"LID\", %3, %4, %5, %6),\n").arg(py(lidLayer + QStringLiteral("_plane")), py(lidLayer), num(outer.left()), num(outer.top()), num(outer.right()), num(outer.bottom()));
    s += QStringLiteral("]\n\n# name, layer, net, its outline (x0, y0, x1, y1, ...) counter-clockwise, its voids\nPOLYGONS = [\n");
    const auto coordinates = [](const QPolygonF& p) {
        QStringList values;
        values.reserve(2 * int(p.size()));
        for (const QPointF& q : p) {
            values << num(q.x());
            values << num(q.y());
        }
        return QStringLiteral("[%1]").arg(values.join(QStringLiteral(", ")));
    };
    for (const HfssPolygon& p : std::as_const(polygons)) {
        QStringList voids;
        for (const QPolygonF& h : p.holes) voids << coordinates(h);
        s += QStringLiteral("    (%1, %2, %3, %4, [%5]),\n").arg(py(p.name), py(p.layer), py(p.net), coordinates(p.outline), voids.join(QStringLiteral(", ")));
    }
    s += QStringLiteral("]\n\n# name, hole's shape, its sizes, its material, the layers it joins\nPADSTACKS = [\n");
    for (const HPadstack& ps : std::as_const(padstacks)) {
        QStringList sizes;
        for (const QString& size : ps.sizes) sizes << py(size);
        s += QStringLiteral("    (%1, %2, [%3], %4, %5, %6),\n").arg(py(ps.name), py(ps.shape), sizes.join(QStringLiteral(", ")), py(ps.material), py(ps.upper), py(ps.lower));
    }
    s += QStringLiteral("]\n\n# name, padstack, x, y, upper layer, lower layer, net\nVIAS = [\n");
    for (int v = 0; v < nVia; ++v) {
        const HVia& via = vias.at(v);
        s += QStringLiteral("    (%1, %2, %3, %4, %5, %6, %7),\n").arg(py(via.name), py(via.padstack), num(via.at.x()), num(via.at.y()), py(via.upper), py(via.lower), py(viaNets.at(v)));
    }
    s += QStringLiteral("]\n\n# In the order of the S matrix. An edge is (primitive, its number, its ends).\nPORTS = [\n");
    s += portLines.join(QLatin1Char('\n'));
    s += QStringLiteral("\n]\n");
    s += QString::fromUtf8(kEngine);
    out.script = s;

    // ---- The control file ----
    QString x;
    x += QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\" ?>\n");
    x += QStringLiteral("<!-- The control file of the GDSII file of its name, for File > Import > GDSII in Ansys\u00ae Electronics Desktop: the HFSS\u2122 3D Layout\n"
                        "     design %1 of an RF 2.5D model, written by %2. Its ports are circuit ports, which is all this way in has: the\n"
                        "     script of its name makes the edge ports. -->\n")
             .arg(out.name, madeBy.isEmpty() ? QStringLiteral("Qucs-S") : madeBy);
    x += QStringLiteral("<c:Control xmlns:c=\"http://www.ansys.com/control\" schemaVersion=\"1.0\">\n");
    x += QStringLiteral("  <Stackup schemaVersion=\"1.0\">\n    <Materials>\n");
    for (const HMaterial& m : std::as_const(materials)) {
        x += QStringLiteral("      <Material Name=\"%1\">\n").arg(xml(m.name));
        x += QStringLiteral("        <Permittivity><Double>%1</Double></Permittivity>\n").arg(num(m.epsr));
        x += QStringLiteral("        <Permeability><Double>1</Double></Permeability>\n");
        x += QStringLiteral("        <Conductivity><Double>%1</Double></Conductivity>\n").arg(num(m.sigma));
        x += QStringLiteral("        <DielectricLossTangent><Double>%1</Double></DielectricLossTangent>\n").arg(num(m.tand));
        x += QStringLiteral("      </Material>\n");
    }
    x += QStringLiteral("    </Materials>\n    <ELayers LengthUnit=\"um\">\n      <Dielectrics>\n");
    // The dielectrics whole from top to bottom, as a control file has them, a
    // metal's room among them in what fills it.
    QStringList dielectrics, conductorsXml, viasXml;
    for (const HLayer& l : std::as_const(layers)) {
        const QString colour = layout::paletteStyle(l.number > 0 ? l.number : 8).fill.name();
        if (!l.signal) {
            dielectrics.prepend(QStringLiteral("        <Layer Color=\"%1\" Material=\"%2\" Name=\"%3\" Thickness=\"%4\"/>\n")
                                    .arg(colour, xml(l.material), xml(l.name), um(l.thickness)));
            continue;
        }
        if (l.thickness > 0)
            dielectrics.prepend(QStringLiteral("        <Layer Color=\"%1\" Material=\"%2\" Name=\"%3_fill\" Thickness=\"%4\"/>\n")
                                    .arg(colour, xml(l.fill.isEmpty() ? air : l.fill), xml(l.name), um(l.thickness)));
        conductorsXml.prepend(QStringLiteral("        <Layer Color=\"%1\" Elevation=\"%2\" GDSDataType=\"0\" Name=\"%3\" TargetLayer=\"%4\" Material=\"%5\" Thickness=\"%6\" Type=\"conductor\"/>\n")
                                  .arg(colour, um(l.z0)).arg(l.number).arg(xml(l.name), xml(l.material), um(l.thickness)));
    }
    x += dielectrics.join(QString());
    x += QStringLiteral("      </Dielectrics>\n      <Layers>\n");
    x += conductorsXml.join(QString());
    x += QStringLiteral("      </Layers>\n");
    if (!viaGds.isEmpty()) {
        x += QStringLiteral("      <Vias>\n");
        QSet<QString> viasWritten;
        for (const int k : std::as_const(viaConductors)) {
            const QString viaName = conductors.at(k).layer;
            if (viasWritten.contains(viaName)) continue;
            viasWritten.insert(viaName);
            const Layer* v = stack.named(viaName);
            const bool fromGround = stack.isGround(v->from), toGround = stack.isGround(v->to);
            const HLayer* a = fromGround ? nullptr : layerOf(v->from);
            const HLayer* b = toGround ? nullptr : layerOf(v->to);
            const QString start = fromGround ? groundLayer : a != nullptr ? a->name : QString();
            const QString stop = toGround ? groundLayer : b != nullptr ? b->name : QString();
            x += QStringLiteral("        <Layer Color=\"%1\" GDSDataType=\"0\" Material=\"%2\" Name=\"%3\" SolveInside=\"false\" StartLayer=\"%4\" StopLayer=\"%5\" TargetLayer=\"%6\"/>\n")
                     .arg(layout::paletteStyle(viaGds.value(viaName)).fill.name(), xml(viaMaterials.value(viaName))).arg(viaGds.value(viaName))
                     .arg(xml(start), xml(stop), xml(safeName(viaName, &layerNames)));
        }
        x += QStringLiteral("      </Vias>\n");
    }
    x += QStringLiteral("    </ELayers>\n  </Stackup>\n");
    x += QStringLiteral("  <Boundaries LengthUnit=\"um\">\n");
    x += xmlPorts.join(QLatin1Char('\n'));
    if (!xmlPorts.isEmpty()) x += QLatin1Char('\n');
    x += QStringLiteral("    <Extents Type=\"bbox\" DielType=\"bbox\" DielHorizFactor=\"%1um\" AirboxHorizFactor=\"%2um\" AirboxVertFactorPos=\"%2um\" AirboxVertFactorNeg=\"%2um\" UseRadiationBoundary=\"True\" DielHonorPrimitives=\"false\" AirboxTruncateAtGround=\"%3\"/>\n")
             .arg(plane ? QStringLiteral("0") : um(out.marginUsed), um(out.airUsed), plane ? QStringLiteral("true") : QStringLiteral("false"));
    x += QStringLiteral("  </Boundaries>\n");
    x += QStringLiteral("  <SimulationSetups>\n    <HFSSSetup schemaVersion=\"1.0\" Name=\"%1\">\n      <HFSSSimulationSettings>\n").arg(xml(out.setup));
    x += QStringLiteral("        <SaveFields>False</SaveFields>\n        <SaveRadFieldsOnly>False</SaveRadFieldsOnly>\n");
    x += QStringLiteral("        <HFSSAdaptiveSettings>\n          <MaxRefinePerPass>30</MaxRefinePerPass>\n          <MinPasses>1</MinPasses>\n          <MinConvergedPasses>1</MinConvergedPasses>\n");
    x += QStringLiteral("          <AdaptiveSettings>\n            <SingleFrequencyDataList>\n              <AdaptiveFrequencyData>\n                <AdaptiveFrequency>%1</AdaptiveFrequency>\n                <MaxDelta>%2</MaxDelta>\n                <MaxPasses>%3</MaxPasses>\n              </AdaptiveFrequencyData>\n            </SingleFrequencyDataList>\n          </AdaptiveSettings>\n        </HFSSAdaptiveSettings>\n")
             .arg(adaptive, num(options.maxDelta)).arg(options.maxPasses);
    if (options.thinMetal > 0)
        x += QStringLiteral("        <HFSSModellingSettings>\n          <ZeroMetalLayerThickness>%1um</ZeroMetalLayerThickness>\n        </HFSSModellingSettings>\n").arg(um(options.thinMetal));
    x += QStringLiteral("      </HFSSSimulationSettings>\n      <HFSSSweepDataList>\n        <FreqSweep>\n          <Name>%1</Name>\n          <Enabled>True</Enabled>\n          <UseQ3DForDC>False</UseQ3DForDC>\n").arg(xml(out.sweep));
    const QString sweepKind = options.discrete ? QStringLiteral("Discrete") : QStringLiteral("Interpolating");
    x += QStringLiteral("          <%1>\n").arg(sweepKind);
    if (sweep.size() > 1 && !logarithmic) {
        x += QStringLiteral("            <LinearStep>\n              <Start>%1Hz</Start>\n              <Stop>%2Hz</Stop>\n              <Step>%3Hz</Step>\n            </LinearStep>\n")
                 .arg(num(sweep.first()), num(sweep.last()), num((sweep.last() - sweep.first()) / (sweep.size() - 1)));
    } else {
        // (One frequency, or a log sweep: as a step from the first to the
        // last at the smallest, the control file having no list.)
        const double step = sweep.size() > 1 ? sweep.at(1) - sweep.first() : 1;
        x += QStringLiteral("            <LinearStep>\n              <Start>%1Hz</Start>\n              <Stop>%2Hz</Stop>\n              <Step>%3Hz</Step>\n            </LinearStep>\n")
                 .arg(num(sweep.first()), num(sweep.last()), num(step));
    }
    x += QStringLiteral("          </%1>\n        </FreqSweep>\n      </HFSSSweepDataList>\n    </HFSSSetup>\n  </SimulationSetups>\n").arg(sweepKind);
    x += QStringLiteral("  <ImportOptions Flatten=\"true\" GDSIIConvertPolygonToCircles=\"false\" ImportDummyNet=\"true\"/>\n");
    x += QStringLiteral("</c:Control>\n");
    out.control = x;
    return out;
}

bool writeGdsOf(const QList<QPair<int, QPolygonF>>& shapes, const QString& path, const QString& cell, QString* error)
{
    // gdstk tells what it does to one FILE* of its own: one at a time.
    QMutexLocker lock(&fem::gdstkLock());
    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        if (error != nullptr) *error = tr("No temporary folder to write a GDSII file in.");
        return false;
    }
    // (By a name of its own: gdstk opens with fopen, which on Windows reads
    // the local code page, not the letters a folder may have.)
    const QString written = scratch.filePath(QStringLiteral("shapes.gds"));
    gdstk::Library library = {};
    const QByteArray libraryName = QFileInfo(path).completeBaseName().toLatin1();
    library.init(libraryName.isEmpty() ? "RF25" : libraryName.constData(), 1e-6, 1e-9);
    auto* top = static_cast<gdstk::Cell*>(gdstk::allocate_clear(sizeof(gdstk::Cell)));
    const QByteArray cellName = cell.isEmpty() ? QByteArrayLiteral("TOP") : cell.toLatin1();
    top->init(cellName.constData());
    library.cell_array.append(top);
    for (const auto& [layer, polygon] : shapes) {
        auto* p = static_cast<gdstk::Polygon*>(gdstk::allocate_clear(sizeof(gdstk::Polygon)));
        p->tag = gdstk::make_tag(uint32_t(layer), 0);
        p->point_array.ensure_slots(uint64_t(polygon.size()));
        for (const QPointF& q : polygon) p->point_array.append(gdstk::Vec2{{q.x(), q.y()}});
        top->polygon_array.append(p);
    }
    const gdstk::ErrorCode code = library.write_gds(QFile::encodeName(written).constData(), 0, nullptr);
    library.free_all();
    if (code != gdstk::ErrorCode::NoError) {
        if (error != nullptr) *error = tr("%1 could not be written as GDSII.").arg(QDir::toNativeSeparators(path));
        return false;
    }
    QFile::remove(path);
    if (!QFile::copy(written, path)) {
        if (error != nullptr) *error = tr("%1 could not be written.").arg(QDir::toNativeSeparators(path));
        return false;
    }
    return true;
}

bool writeHfssExport(const HfssExport& written, const QString& scriptPath, QStringList* files, QString* error)
{
    if (!written.ok()) {
        if (error != nullptr) *error = written.problems.join(QLatin1Char(' '));
        return false;
    }
    const QFileInfo info(scriptPath);
    const QString stem = info.absoluteDir().filePath(info.completeBaseName());
    const auto put = [error, files](const QString& path, const QString& text) {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(text.toUtf8()) < 0 || !file.commit()) {
            if (error != nullptr) *error = tr("%1 could not be written: %2.").arg(QDir::toNativeSeparators(path), file.errorString());
            return false;
        }
        if (files != nullptr) *files << path;
        return true;
    };
    if (!put(info.absoluteFilePath(), written.script)) return false;
    if (!put(stem + QStringLiteral(".xml"), written.control)) return false;
    if (!writeGdsOf(written.gdsShapes, stem + QStringLiteral(".gds"), written.name, error)) return false;
    if (files != nullptr) *files << stem + QStringLiteral(".gds");
    return true;
}

// ------------------------------------------------------------ Comparing S

namespace {

/// The references a Touchstone file writes its S into: one, or each port's
/// (version 2's [Reference]).
QList<double> referencesIn(const QString& path, int ports, QString* said)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    double reference = 50;
    QList<double> each;
    bool seen = false, expecting = false;
    while (!file.atEnd()) {
        QString line = QString::fromUtf8(file.readLine());
        if (const qsizetype bang = line.indexOf(QLatin1Char('!')); bang >= 0) line.truncate(bang);
        line = line.trimmed();
        if (line.isEmpty()) continue;
        if (expecting) {
            for (const QString& w : line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts))
                each << w.toDouble();
            expecting = each.size() < ports;
            continue;
        }
        if (line.startsWith(QLatin1Char('#')) && !seen) {
            seen = true;
            const QStringList words = line.mid(1).simplified().toUpper().split(QLatin1Char(' '), Qt::SkipEmptyParts);
            for (int i = 0; i + 1 < words.size(); ++i)
                if (words.at(i) == QLatin1String("R")) reference = words.at(i + 1).toDouble();
            continue;
        }
        if (line.startsWith(QLatin1Char('['))) {
            const QString keyword = line.section(QLatin1Char(']'), 0, 0).mid(1).trimmed().toLower();
            if (keyword == QLatin1String("reference")) {
                for (const QString& w : line.section(QLatin1Char(']'), 1).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts))
                    each << w.toDouble();
                expecting = each.size() < ports;
            }
        } else if (seen) {
            break;   // (the numbers: all that says anything is before them)
        }
    }
    if (each.size() == ports) {
        if (said != nullptr) *said = tr("each port into its own reference, as its file says");
        return each;
    }
    if (said != nullptr) *said = tr("every port into %1 Ohm, as its file says").arg(reference);
    return QList<double>(ports, reference);
}

} // namespace

SCompared compareWithTouchstone(const FullWave& solved, const QString& path, const QList<int>& order)
{
    SCompared out;
    if (!solved.ok() || solved.z.isEmpty()) {
        out.problem = tr("There is no full-wave solve to compare it with: solve the model first.");
        return out;
    }
    dataimport::Data data;
    QString why;
    if (dataimport::formatOf(path) != dataimport::Format::Touchstone) {
        out.problem = tr("%1 is no Touchstone file (.sNp).").arg(QFileInfo(path).fileName());
        return out;
    }
    if (!dataimport::read(path, dataimport::Options(), &data, &why)) {
        out.problem = why;
        return out;
    }
    QList<double> frequency;
    QHash<QString, const dataset::Variable*> values;
    int ports = 0;
    static const QRegularExpression term(QStringLiteral("^([SYZHG])\\[(\\d+),(\\d+)\\]$"));
    QString parameter;
    for (const dataset::Variable& v : std::as_const(data.variables)) {
        if (v.name == QLatin1String("frequency")) {
            frequency = QList<double>(v.re.begin(), v.re.end());
            continue;
        }
        const QRegularExpressionMatch m = term.match(v.name);
        if (!m.hasMatch()) continue;
        parameter = m.captured(1);
        ports = std::max(ports, m.captured(2).toInt());
        values.insert(m.captured(2) + QLatin1Char(',') + m.captured(3), &v);
    }
    if (parameter != QLatin1String("S")) {
        out.problem = tr("%1 holds %2 parameters: S is what is compared.").arg(QFileInfo(path).fileName(), parameter);
        return out;
    }
    const int np = int(solved.portNames.size());
    QList<int> map = order;
    if (map.isEmpty()) {
        map.resize(np);
        std::iota(map.begin(), map.end(), 1);
    }
    if (ports != np && order.isEmpty()) {
        out.problem = tr("%1 has %2 port(s), the solve %3: give their order.").arg(QFileInfo(path).fileName()).arg(ports).arg(np);
        return out;
    }
    if (map.size() != np) {
        out.problem = tr("The order names %1 port(s), the solve has %2.").arg(map.size()).arg(np);
        return out;
    }
    for (const int k : std::as_const(map))
        if (k < 1 || k > ports) {
            out.problem = tr("The order names port %1, and %2 has %3.").arg(k).arg(QFileInfo(path).fileName()).arg(ports);
            return out;
        }
    if (frequency.isEmpty()) {
        out.problem = tr("%1 holds no frequency.").arg(QFileInfo(path).fileName());
        return out;
    }
    out.ports = np;
    QString saidReference;
    const QList<double> fileReferences = referencesIn(path, ports, &saidReference);
    QList<double> references;
    for (const int k : std::as_const(map)) references << fileReferences.value(k - 1, 50);
    bool same = true;
    for (const double r : std::as_const(references)) same = same && r == references.first();
    out.reference = same ? references.first() : 0;
    out.notes << tr("Compared %1.").arg(saidReference);

    // At each frequency of the solve the file reaches: between its points, in
    // magnitude and in angle, which a phase's rotation asks for.
    const auto valueAt = [&](const dataset::Variable* v, int below, double t) {
        const Complex a(v->re.at(below), v->im.value(below));
        if (t <= 0) return a;
        const Complex b(v->re.at(below + 1), v->im.value(below + 1));
        double turn = std::arg(b) - std::arg(a);
        while (turn > M_PI) turn -= 2 * M_PI;
        while (turn < -M_PI) turn += 2 * M_PI;
        return std::polar(std::abs(a) + t * (std::abs(b) - std::abs(a)), std::arg(a) + t * turn);
    };
    constexpr double kSame = 1e-9;
    for (int k = 0; k < solved.frequencies.size(); ++k) {
        const double f = solved.frequencies.at(k);
        if (f < frequency.first() * (1 - kSame) || f > frequency.last() * (1 + kSame)) continue;
        int below = int(std::upper_bound(frequency.begin(), frequency.end(), f) - frequency.begin()) - 1;
        below = std::clamp(below, 0, int(frequency.size()) - 1);
        double t = 0;
        if (std::abs(frequency.at(below) - f) > kSame * f && below + 1 < frequency.size()) {
            t = (f - frequency.at(below)) / (frequency.at(below + 1) - frequency.at(below));
            ++out.interpolated;
        }
        QList<QList<Complex>> theirs;
        for (int i = 0; i < np; ++i) {
            QList<Complex> row;
            for (int j = 0; j < np; ++j) {
                const QString key = QStringLiteral("%1,%2").arg(map.at(i)).arg(map.at(j));
                const dataset::Variable* v = values.value(key, nullptr);
                row << (v != nullptr ? valueAt(v, below, t) : Complex(0, 0));
            }
            theirs << row;
        }
        out.frequencies << f;
        out.theirs << theirs;
        out.ours << scatteringOf(solved.z.at(k), references);
    }
    if (out.frequencies.isEmpty()) {
        out.problem = tr("The file's frequencies (%1 to %2 Hz) and the solve's do not meet.").arg(frequency.first()).arg(frequency.last());
        return out;
    }
    if (out.frequencies.size() < solved.frequencies.size())
        out.notes << tr("%1 of the solve's %2 frequencies lie outside the file's: left out.")
                         .arg(solved.frequencies.size() - out.frequencies.size()).arg(solved.frequencies.size());
    if (out.interpolated > 0)
        out.notes << tr("%1 frequencies are read between the file's points, in magnitude and angle.").arg(out.interpolated);
    for (const QString& note : solved.warnings) out.notes << tr("The solve says: %1").arg(note);

    const auto db = [](const Complex& s) { return 20 * std::log10(std::max(std::abs(s), 1e-30)); };
    for (int i = 0; i < np; ++i)
        for (int j = 0; j < np; ++j) {
            SCompared::Term t;
            t.i = i + 1;
            t.j = j + 1;
            double sum = 0;
            for (int k = 0; k < out.frequencies.size(); ++k) {
                const Complex a = out.ours.at(k).at(i).at(j), b = out.theirs.at(k).at(i).at(j);
                const double apart = std::abs(a - b);
                sum += apart * apart;
                if (apart >= t.worst) {
                    t.worst = apart;
                    t.worstAt = out.frequencies.at(k);
                }
                if (db(a) > -60 || db(b) > -60) t.dbWorst = std::max(t.dbWorst, std::abs(db(a) - db(b)));
                if (db(a) > -40 && db(b) > -40) {
                    double turn = std::arg(a) - std::arg(b);
                    while (turn > M_PI) turn -= 2 * M_PI;
                    while (turn < -M_PI) turn += 2 * M_PI;
                    t.degWorst = std::max(t.degWorst, std::abs(turn) * 180 / M_PI);
                }
            }
            t.rms = std::sqrt(sum / double(out.frequencies.size()));
            out.terms << t;
            if (t.worst >= out.worst) {
                out.worst = t.worst;
                out.worstAt = t.worstAt;
                out.worstI = t.i;
                out.worstJ = t.j;
            }
        }
    return out;
}

QString comparisonTable(const SCompared& compared, const QString& solvedBy, const QString& theirs)
{
    QString out;
    out += QStringLiteral("# S of %1 beside %2, %3 frequencies; magnitude in dB, angle in degrees\n")
               .arg(solvedBy, theirs).arg(compared.frequencies.size());
    QStringList header{QStringLiteral("frequency_Hz")};
    for (const SCompared::Term& t : compared.terms) {
        const QString sij = QStringLiteral("S%1_%2").arg(t.i).arg(t.j);
        header << sij + QStringLiteral("_dB") << sij + QStringLiteral("_deg") << sij + QStringLiteral("_file_dB") << sij + QStringLiteral("_file_deg");
    }
    out += header.join(QLatin1Char(',')) + QLatin1Char('\n');
    for (int k = 0; k < compared.frequencies.size(); ++k) {
        QStringList row{QString::number(compared.frequencies.at(k), 'g', 12)};
        for (const SCompared::Term& t : compared.terms) {
            const Complex a = compared.ours.at(k).at(t.i - 1).at(t.j - 1), b = compared.theirs.at(k).at(t.i - 1).at(t.j - 1);
            row << QString::number(20 * std::log10(std::max(std::abs(a), 1e-30)), 'f', 5) << QString::number(std::arg(a) * 180 / M_PI, 'f', 4)
                << QString::number(20 * std::log10(std::max(std::abs(b), 1e-30)), 'f', 5) << QString::number(std::arg(b) * 180 / M_PI, 'f', 4);
        }
        out += row.join(QLatin1Char(',')) + QLatin1Char('\n');
    }
    return out;
}

} // namespace qucs_s::rf25
