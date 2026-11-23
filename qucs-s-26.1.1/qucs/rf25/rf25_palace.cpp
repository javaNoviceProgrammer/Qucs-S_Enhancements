/***************************************************************************
                               rf25_palace.cpp
                              -----------------
 *  An RF 2.5D model as a project of Palace: its mesh and its configuration
 *  (rf25_palace.h).
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 ***************************************************************************/

#include "rf25_palace.h"

#include "multiphysics/fem3_mesh.h"
#include "multiphysics/fem_geometry.h"
#include "multiphysics/fem_mesh.h"
#include "rf25_inductance.h"
#include "rf25_sparams.h"
#include "rf25_stackfile.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace qucs_s::rf25 {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("Rf25Palace", text);
}

constexpr double kInfinity = std::numeric_limits<double>::infinity();
constexpr double kLight = 299792458.0;
constexpr double kGrowth = 1.3;   ///< how fast the elements grow away from an edge or a level

QString um(double v)
{
    return QString::number(v, 'g', 6);
}

/// A rectangle (or a strip) of a port: vertical - above the plan segment
/// a-b from z0 up to z1 - or level - the plan quadrilateral quad at z. Its
/// direction from the port's plus to its minus.
struct PortElement {
    bool vertical = true;
    QPointF a, b;
    double z0 = 0, z1 = 0;
    QPolygonF quad;
    double z = 0;
    std::array<double, 3> direction{{0, 0, -1}};
    int attribute = 0;
    double area = 0;          ///< µm², as drawn
};

struct PortWritten {
    QString name;
    int index = 0;
    bool driven = true;
    double r = 0, l = 0, c = 0;
    QList<PortElement> elements;
};

/// A boundary of the mesh: its attribute, what it is, its name.
struct BoundaryGroup {
    QString name;
    QString kind;             ///< "pec", "conductivity", "impedance", "absorbing", "port"
    double value = 0;         ///< a conductivity's (S/m), an impedance's (Ohm/sq)
    double thickness = 0;     ///< µm: a sheet's conductor's
};

struct FaceKey {
    int a, b, c;
    bool operator<(const FaceKey& o) const
    {
        if (a != o.a) return a < o.a;
        if (b != o.b) return b < o.b;
        return c < o.c;
    }
    bool operator==(const FaceKey& o) const { return a == o.a && b == o.b && c == o.c; }
};

FaceKey keyOf(int x, int y, int z)
{
    int v[3] = {x, y, z};
    std::sort(v, v + 3);
    return {v[0], v[1], v[2]};
}

std::vector<fem::GEdge> lineLoop(const QPolygonF& corners, std::vector<fem::Curve>& curves)
{
    std::vector<fem::GEdge> loop;
    const int n = int(corners.size()) - (corners.size() > 1 && corners.first() == corners.last() ? 1 : 0);
    for (int i = 0; i < n; ++i) {
        const QPointF a = corners.at(i), b = corners.at((i + 1) % n);
        if (a == b) continue;
        fem::Curve c;
        c.c = a;
        c.a = b - a;
        curves.push_back(c);
        loop.push_back({a, b, int(curves.size()) - 1, 0, 1});
    }
    return loop;
}

double signedArea(const QPolygonF& p)
{
    double s = 0;
    for (int i = 0, n = int(p.size()); i < n; ++i) {
        const QPointF& u = p.at(i);
        const QPointF& w = p.at((i + 1) % n);
        s += u.x() * w.y() - w.x() * u.y();
    }
    return s / 2;
}

double perimeter(const QPolygonF& p)
{
    double s = 0;
    for (int i = 0, n = int(p.size()); i < n; ++i) s += QLineF(p.at(i), p.at((i + 1) % n)).length();
    return s;
}

/// A direction as Palace takes it: a keyword along an axis, else three numbers.
QJsonValue directionJson(const std::array<double, 3>& d)
{
    static const char* const names[3][2] = {{"+X", "-X"}, {"+Y", "-Y"}, {"+Z", "-Z"}};
    for (int k = 0; k < 3; ++k) {
        const double o1 = d[std::size_t((k + 1) % 3)], o2 = d[std::size_t((k + 2) % 3)];
        if (std::abs(o1) < 1e-12 && std::abs(o2) < 1e-12 && std::abs(std::abs(d[std::size_t(k)]) - 1) < 1e-12)
            return QString::fromLatin1(names[k][d[std::size_t(k)] > 0 ? 0 : 1]);
    }
    return QJsonArray{d[0], d[1], d[2]};
}

/// A layer's or a port's name as a physical group's in MSH: a double quote
/// would end it, and a line break the line, so each is made harmless.
std::string physicalName(QString name)
{
    name.replace(QLatin1Char('"'), QLatin1Char('\''));
    name.replace(QLatin1Char('\n'), QLatin1Char(' '));
    name.replace(QLatin1Char('\r'), QLatin1Char(' '));
    return name.toStdString();
}

void append(std::string& out, double v)
{
    char buf[40];
    const int n = std::snprintf(buf, sizeof buf, "%.17g", v);
    out.append(buf, std::size_t(n > 0 ? n : 0));
}

void append(std::string& out, long long v)
{
    char buf[24];
    const int n = std::snprintf(buf, sizeof buf, "%lld", v);
    out.append(buf, std::size_t(n > 0 ? n : 0));
}

} // namespace

PalaceExport palaceExportOf(const Model& model, const QList<Conductor>& given, const PalaceOptions& options, const QString& name,
                            const QString& madeBy)
{
    // (A merged via farm's footprint as its metal: this solver takes a shape
    //  as solid - asSolid().)
    const QList<Conductor> conductors = asSolid(given);

    PalaceExport out;
    // (A merged via farm: written as its metal thinned in place, which a
    //  solver of solid shapes takes as a block - its resistance, not the
    //  vias' spread over their footprint as RF 2.5D solves them. Said.)
    if (const QString said = mergedFarmsSaid(given, conductors); !said.isEmpty()) out.notes << said;
    out.name = name.trimmed().isEmpty() ? QStringLiteral("rf25") : name.trimmed();
    const Stack stack = model.stack();
    if (!stack.ok()) out.problems << tr("The stack cannot be written as it stands: %1").arg(stack.problems.join(QLatin1Char(' ')));
    QString why;
    const QList<double> sweep = studySweep(model, &why);
    if (sweep.isEmpty()) out.problems << (why.isEmpty() ? tr("The study asks for no frequency.") : why);
    if (conductors.isEmpty())
        out.problems << tr("The model has no shapes to write: its layout has not been read, or nothing of it is mapped to the stack.");
    if (const Node* processing = model.layout().child(QStringLiteral("rf_processing"));
        processing != nullptr && processingOf(*processing, model.parameters(), nullptr).symmetryAxis != 0)
        out.problems << tr("The layout's Processing keeps only the half beyond its plane of symmetry: set its Symmetry to none to "
                           "export the whole.");
    if (options.order < 1 || options.order > 4) out.problems << tr("The elements' order is 1 to 4, not %1.").arg(options.order);
    if (options.sides != QLatin1String("perfect") && options.sides != QLatin1String("absorbing"))
        out.problems << tr("The box's sides are \"perfect\" or \"absorbing\", not \"%1\".").arg(options.sides);
    if (options.solver != QLatin1String("ams") && options.solver != QLatin1String("default"))
        out.problems << tr("The linear solver is \"ams\" or \"default\", not \"%1\".").arg(options.solver);
    if (!out.problems.isEmpty()) return out;

    constexpr double toUm = 1e6;
    // ---- The stack's layers by name, and the shapes on each ----
    QHash<QString, int> layerIndex;
    for (int i = 0; i < stack.layers.size(); ++i) layerIndex.insert(stack.layers.at(i).name, i);
    struct Shape {
        int layer = -1;
        QPolygonF outline;
    };
    QList<Shape> shapes;
    QStringList offStack;
    QRectF bounds;
    double narrowest = kInfinity;
    for (const Conductor& c : conductors) {
        const int li = layerIndex.value(c.layer, -1);
        if (li < 0 || (stack.layers.at(li).kind != Layer::Metal && stack.layers.at(li).kind != Layer::Via)) {
            if (!offStack.contains(c.layer)) offStack << c.layer;
            continue;
        }
        QPolygonF o = c.outline;
        if (o.size() > 1 && o.first() == o.last()) o.removeLast();
        if (o.size() < 3 || !(std::abs(signedArea(o)) > 0)) continue;
        if (signedArea(o) < 0) std::reverse(o.begin(), o.end());
        shapes << Shape{li, o};
        bounds = bounds.isNull() ? o.boundingRect() : bounds.united(o.boundingRect());
        if (stack.layers.at(li).kind == Layer::Metal) narrowest = std::min(narrowest, 2 * std::abs(signedArea(o)) / perimeter(o));
    }
    if (!offStack.isEmpty())
        out.notes << tr("Shapes on %1, no metal or via of the stack, are not written.").arg(offStack.join(QStringLiteral(", ")));
    if (shapes.isEmpty()) {
        out.problems << tr("There are no shapes on the stack's metals and vias to write.");
        return out;
    }

    // ---- Sizes (µm) ----
    const double top = sweep.last();
    const double lambda0 = kLight / top * toUm;
    double slowest = 1;
    double stackTop = 0;
    for (const Layer& l : stack.layers) {
        if (l.kind == Layer::Substrate || l.kind == Layer::Dielectric)
            slowest = std::max(slowest, std::max({l.epsr, l.epsz, l.averaged() ? std::max(l.epsx, l.epsy) : 0.0}) * std::max(1.0, l.mur));
        if (l.kind != Layer::Cover && l.kind != Layer::Ground && l.kind != Layer::Via) stackTop = std::max(stackTop, l.z1 * toUm);
    }
    const double side = std::max(bounds.width(), bounds.height());
    out.largestUsed = options.largest > 0 ? options.largest * toUm : std::min(lambda0 / (10 * std::sqrt(slowest)), std::max(side, stackTop));
    out.edgesUsed = options.edges > 0 ? options.edges * toUm
                                      : std::min(std::isfinite(narrowest) ? narrowest / 4 : side / 40, out.largestUsed / 2);
    out.edgesUsed = std::min(out.edgesUsed, out.largestUsed);

    // ---- The box: the window in the plan, the floor and the ceiling ----
    QRectF window;
    bool walls = false;
    const Layer* lid = nullptr;
    const Layer* ground = nullptr;
    for (const Layer& l : stack.layers) {
        if (l.kind == Layer::Cover) {
            if (l.variant == QLatin1String("perfect")) lid = &l;
            if (l.walls && l.box.isValid()) {
                window = QRectF(l.box.topLeft() * toUm, l.box.bottomRight() * toUm).normalized();
                walls = true;
            }
        }
        if (l.kind == Layer::Ground) ground = &l;
    }
    const bool openGround = ground == nullptr || ground->variant == QLatin1String("open");
    if (walls) {
        if (!window.adjusted(-1e-9 * window.width(), -1e-9 * window.height(), 1e-9 * window.width(), 1e-9 * window.height()).contains(bounds)) {
            out.problems << tr("Some shapes reach past the lid's side walls: draw them inside the box, or make the box larger.");
            return out;
        }
        out.notes << tr("The box is the lid's side walls, perfect conductors.");
    } else {
        // (Perfect sides five heights of the stack out. Absorbing sides a
        //  quarter wave out at the top of the sweep, five heights at least: an
        //  absorbing side near a line takes its field along it as a resistive
        //  wall would - 1.3 mm off a microstrip on 254 µm of alumina 27 dB/m
        //  more than its copper's 8.5 at 10 GHz, 3.7 mm off still 4.)
        const double heights = 5 * std::max(stackTop, out.edgesUsed);
        out.marginUsed = options.margin >= 0                     ? options.margin * toUm
                         : options.sides == QLatin1String("absorbing") ? std::max(lambda0 / 4, heights)
                                                                       : heights;
        if (!(out.marginUsed > 0)) out.marginUsed = std::max(side / 10, out.edgesUsed);
        window = bounds.adjusted(-out.marginUsed, -out.marginUsed, out.marginUsed, out.marginUsed);
    }
    const double extent = std::max({window.width(), window.height(), stackTop, 1e-3});
    const double tol = 1e-9 * extent;
    // The ceiling: the lid's top, else the air above the stack's top.
    double ceiling = 0, floor = 0;
    if (lid != nullptr) {
        ceiling = lid->z1 * toUm;
        if (!(ceiling > stackTop + tol)) {
            out.problems << tr("The metal lid has no air under it: give the Cover a height.");
            return out;
        }
    } else {
        double coverTop = stackTop;
        for (const Layer& l : stack.layers)
            if (l.kind == Layer::Cover) coverTop = std::max(coverTop, l.z1 * toUm);
        out.airUsed = options.air > 0 ? options.air * toUm : lambda0 / 4;
        ceiling = std::max(coverTop, stackTop + out.airUsed);
        out.airUsed = ceiling - stackTop;
    }
    if (openGround) floor = -(options.air > 0 ? options.air * toUm : std::max(out.airUsed, lambda0 / 4));

    // A metal's top as written: its bottom, where it is a sheet.
    const auto topOf = [&](const Layer& l) { return (options.sheets ? l.z0 : l.z0 + l.thickness) * toUm; };

    // ---- The ports, resolved to rectangles ----
    QList<Conductor> outlines;   // (µm, for faceEdge and besideEdges)
    for (const Shape& s : std::as_const(shapes)) outlines << Conductor{stack.layers.at(s.layer).name, s.outline, false};
    const auto containing = [&](const QString& layer, QPointF p) {
        for (const Shape& s : std::as_const(shapes))
            if (stack.layers.at(s.layer).name == layer && s.outline.containsPoint(p, Qt::OddEvenFill)) return true;
        return false;
    };
    // The top of the conductor under a point, below height z (µm): the
    // highest metal there, else the ground (0), else none (NaN).
    const auto returnUnder = [&](QPointF p, double z, QString* what) {
        double best = -kInfinity;
        for (const Shape& s : std::as_const(shapes)) {
            const Layer& l = stack.layers.at(s.layer);
            if (l.kind != Layer::Metal) continue;
            const double t = topOf(l);
            if (t > z - tol || t <= best) continue;
            if (!s.outline.containsPoint(p, Qt::OddEvenFill)) continue;
            best = t;
            *what = l.name;
        }
        if (std::isfinite(best)) return best;
        if (!openGround) {
            *what = ground->name;
            return 0.0;
        }
        return std::numeric_limits<double>::quiet_NaN();
    };
    const auto zOf = [&](const QString& metal, bool upper) {
        const Layer* l = stack.named(metal);
        return l == nullptr ? std::numeric_limits<double>::quiet_NaN() : upper ? topOf(*l) : l->z0 * toUm;
    };
    QList<PortWritten> ports;
    int nextAttribute = 1;
    QList<BoundaryGroup> groups;
    const auto addGroup = [&](const QString& groupName, const QString& kind, double value = 0, double thickness = 0) {
        groups << BoundaryGroup{groupName, kind, value, thickness};
        return nextAttribute++;
    };
    int measuring = 0;
    QList<const Node*> portNodes, lumpedNodes;
    for (const Node& p : model.ports().children) {
        if (!p.enabled) continue;
        if (p.type == QLatin1String("rf_edge_port")) portNodes << &p;
        else if (p.type == QLatin1String("rf_internal_port"))
            (textOf(p, QStringLiteral("element")) == QLatin1String("lumped") ? lumpedNodes : portNodes) << &p;
    }
    const auto facingOf = [](const QString& way) {
        if (way == QLatin1String("+x")) return QPointF(1, 0);
        if (way == QLatin1String("-x")) return QPointF(-1, 0);
        if (way == QLatin1String("+y")) return QPointF(0, 1);
        if (way == QLatin1String("-y")) return QPointF(0, -1);
        return QPointF();
    };
    for (const Node* node : portNodes + lumpedNodes) {
        const Node& p = *node;
        const QString own = portName(p);
        const bool edge = p.type == QLatin1String("rf_edge_port");
        const bool lumpedElement = !edge && textOf(p, QStringLiteral("element")) == QLatin1String("lumped");
        QPointF at = QPointF(numberOf(p, QStringLiteral("x"), model, 0), numberOf(p, QStringLiteral("y"), model, 0)) * toUm;
        PortWritten w;
        w.name = own;
        w.driven = !lumpedElement;
        const QString returnKind = textOf(p, QStringLiteral("return"));
        if (returnKind == QLatin1String("terminals")) {
            out.notes << tr("%1 returns through the terminals it names: it is not written - a lumped port in Palace returns across "
                            "its own rectangle.").arg(own);
            continue;
        }
        if (!textOf(p, QStringLiteral("pair")).trimmed().isEmpty())
            out.notes << tr("%1: its differential pair is not written - Palace gives single-ended S, from which the modes follow.").arg(own);
        if (lumpedElement) {
            const double r = numberOf(p, QStringLiteral("r"), model, 0), l = numberOf(p, QStringLiteral("l"), model, 0),
                         c = numberOf(p, QStringLiteral("c"), model, 0);
            const int given = (r > 0) + (l > 0) + (c > 0);
            if (given == 0 || (given > 1 && textOf(p, QStringLiteral("topology")) != QLatin1String("parallel"))) {
                out.notes << tr("%1 is a lumped element Palace cannot take - its R, L and C %2 - : it is not written.")
                                 .arg(own, given == 0 ? tr("all nought") : tr("in series"));
                continue;
            }
            w.r = r;
            w.l = l;
            w.c = c;
        } else {
            w.r = numberOf(p, QStringLiteral("impedance"), model, 50);
            if (!(w.r > 0)) {
                out.notes << tr("%1's reference impedance is none: it is not written.").arg(own);
                continue;
            }
            if (edge && (flagOf(p, QStringLiteral("calibrate")) || numberOf(p, QStringLiteral("deembed"), model, 0) != 0))
                out.notes << tr("%1: its reference plane is at its rectangle - its own discontinuity is in Palace's S, and it is not "
                                "moved along its line.").arg(own);
        }
        if (edge) {
            const QString metal = textOf(p, QStringLiteral("conductor")).trimmed();
            const Layer* ml = stack.named(metal);
            QLineF face;
            QPointF outward;
            QString onLayer;
            // (Taken onto the edge it was drawn on, as the solve takes it: a
            //  port on a circle's edge was "not written", on no conductor.)
            {
                double moved = 0;
                at = ontoNearestEdge(outlines, at, metal, options.portReach, &moved);
                if (moved > 0) out.notes << tr("%1 is %2 µm off the processed outline it was drawn on: written on it.").arg(own).arg(moved, 0, 'g', 2);
            }
            // (On the edge, as the solve has it: portPlace(). The nearest
            //  edge however far was written.)
            const QPointF way = facingOf(textOf(p, QStringLiteral("facing")));
            const PortPlace place = portPlace(outlines, at, metal, way);
            if (ml == nullptr || ml->kind != Layer::Metal || place != PortPlace::OnEdge
                || !faceEdge(outlines, at, metal, way, &face, &outward, &onLayer)) {
                out.notes << (ml != nullptr && ml->kind == Layer::Metal && place != PortPlace::OnEdge
                                  ? tr("%1 is not written: %2.").arg(own, portPlaceSaid(place, metal))
                                  : tr("%1 is not written: no edge of a shape on %2 near its point faces the way it says.").arg(own, metal));
                continue;
            }
            // (Its conductor's whole side, as the solver's face is.)
            face = wholeSideOf(outlines, face, onLayer);
            const double zMetal = ml->z0 * toUm;
            if (returnKind == QLatin1String("beside")) {
                const std::array<QLineF, 2> beside = besideEdges(outlines, face, outward, onLayer);
                const double len = face.length();
                const QPointF u = (face.p2() - face.p1()) / len;
                for (const QLineF& s : beside) {
                    if (s.isNull()) continue;
                    const double s1 = QPointF::dotProduct(s.p1() - face.p1(), u), s2 = QPointF::dotProduct(s.p2() - face.p1(), u);
                    QPointF from, to;
                    if (std::min(s1, s2) >= len - tol) {
                        from = face.p2();
                        to = face.p1() + u * std::min(s1, s2);
                    } else if (std::max(s1, s2) <= tol) {
                        from = face.p1();
                        to = face.p1() + u * std::max(s1, s2);
                    } else {
                        continue;
                    }
                    const double gap = QLineF(from, to).length();
                    if (!(gap > tol)) continue;
                    const double depth = gap;
                    PortElement e;
                    e.vertical = false;
                    e.z = zMetal;
                    e.quad = QPolygonF({from, to, to - outward * depth, from - outward * depth});
                    if (signedArea(e.quad) < 0) std::reverse(e.quad.begin(), e.quad.end());
                    const QPointF d = (to - from) / gap;
                    e.direction = {{d.x(), d.y(), 0}};
                    e.area = gap * depth;
                    w.elements << e;
                }
                if (w.elements.isEmpty()) {
                    out.notes << tr("%1 returns through the conductors beside it, and there are none on the line of its edge: it is not "
                                    "written.").arg(own);
                    continue;
                }
                out.portSaid << tr("%1: lumped port %2 of %3 elements across its gaps to the conductors beside it, in the plane of %4")
                                    .arg(own).arg(measuring + 1).arg(w.elements.size()).arg(metal);
            } else {
                // Down from its face to the ground, or to the metal under it.
                const QPointF inside = (face.p1() + face.p2()) / 2 - outward * std::max(tol, 1e-6 * face.length());
                QString under;
                const double z0 = returnUnder(inside, zMetal, &under);
                if (!std::isfinite(z0) || !(zMetal > z0 + tol)) {
                    out.notes << tr("%1 is not written: there is no ground or metal under its face to return to.").arg(own);
                    continue;
                }
                PortElement e;
                e.vertical = true;
                e.a = face.p1();
                e.b = face.p2();
                e.z0 = z0;
                e.z1 = zMetal;
                e.direction = {{0, 0, -1}};
                e.area = face.length() * (zMetal - z0);
                w.elements << e;
                out.portSaid << tr("%1: lumped port %2 under its face on %3, down to %4 (%5 by %6 µm)")
                                    .arg(own).arg(measuring + 1).arg(metal, under, um(face.length()), um(zMetal - z0));
            }
        } else {
            const QString to = textOf(p, QStringLiteral("to")).trimmed();
            const QString from = textOf(p, QStringLiteral("from")).trimmed();
            const Layer* tl = stack.named(to);
            if (tl == nullptr || tl->kind != Layer::Metal) {
                out.notes << tr("%1 is not written: its To metal %2 is no metal of the stack.").arg(own, to);
                continue;
            }
            const bool toGround = from.isEmpty() || stack.isGround(from);
            // From the lower metal's top (the ground's nought) up to the upper
            // metal's bottom, whichever of its two is the lower; its plus, the
            // To metal, above or below.
            double z0 = 0, z1 = zOf(to, false);
            bool toAbove = true;
            if (toGround) {
                if (openGround) {
                    out.notes << tr("%1 is not written: it returns to the ground, and the ground is open.").arg(own);
                    continue;
                }
            } else {
                const Layer* fl = stack.named(from);
                if (fl == nullptr || fl->kind != Layer::Metal) {
                    out.notes << tr("%1 is not written: its From metal %2 is no metal of the stack.").arg(own, from);
                    continue;
                }
                toAbove = tl->z0 > fl->z0;
                z0 = toAbove ? zOf(from, true) : zOf(to, true);
                z1 = toAbove ? zOf(to, false) : zOf(from, false);
            }
            if (!(z1 > z0 + tol)) {
                out.notes << tr("%1 is not written: its two metals have no room between them.").arg(own);
                continue;
            }
            const double wx = numberOf(p, QStringLiteral("width"), model, 0) * toUm, hy = numberOf(p, QStringLiteral("height"), model, 0) * toUm;
            const bool alongX = wx >= hy;
            const double span = std::max(wx, hy) > 0 ? std::max(wx, hy) : out.edgesUsed;
            PortElement e;
            e.vertical = true;
            e.a = alongX ? at - QPointF(span / 2, 0) : at - QPointF(0, span / 2);
            e.b = alongX ? at + QPointF(span / 2, 0) : at + QPointF(0, span / 2);
            if (!containing(to, e.a) || !containing(to, e.b) || (!toGround && (!containing(from, e.a) || !containing(from, e.b)))) {
                out.notes << tr("%1 is not written: its %2 µm across its point is not all over %3.")
                                 .arg(own, um(span), toGround ? to : tr("both its metals"));
                continue;
            }
            e.z0 = z0;
            e.z1 = z1;
            // (From its To metal, its plus, down to its From.)
            e.direction = {{0, 0, toAbove ? -1.0 : 1.0}};
            e.area = span * (z1 - z0);
            w.elements << e;
            const QString fromName = toGround ? (ground != nullptr ? ground->name : QStringLiteral("ground")) : toAbove ? from : to;
            const QString toName = toAbove ? to : from;
            const QString across = alongX ? QStringLiteral("x") : QStringLiteral("y");
            out.portSaid << (lumpedElement ? tr("%1: lumped element, an undriven lumped port, from %2 up to %3, %4 µm across %5")
                                                 .arg(own, fromName, toName, um(span), across)
                                           : tr("%1: lumped port %2 from %3 up to %4, %5 µm across %6")
                                                 .arg(own)
                                                 .arg(measuring + 1)
                                                 .arg(fromName, toName, um(span), across));
        }
        if (lumpedElement) {
            ++out.lumped;
        } else {
            ++measuring;
            w.index = measuring;
        }
        for (int i = 0; i < w.elements.size(); ++i)
            w.elements[i].attribute = addGroup(w.elements.size() > 1 ? QStringLiteral("port %1 %2").arg(own).arg(i + 1)
                                                                     : QStringLiteral("port %1").arg(own),
                                               QStringLiteral("port"));
        ports << w;
    }
    // (Lumped elements numbered after the measuring ports.)
    {
        int next = measuring;
        for (PortWritten& w : ports)
            if (!w.driven) w.index = ++next;
    }
    out.ports = measuring;
    if (measuring == 0) {
        out.problems << tr("No port could be written: the model's ports are on no edge or metal that is there.");
        return out;
    }

    // ---- The plan: the box, every shape's outline, every port's edge ----
    std::vector<fem::GObject> objects;
    std::vector<fem::Curve> curves;
    {
        fem::GObject box;
        box.name = QStringLiteral("box");
        box.loops.push_back(lineLoop(QPolygonF(window), curves));
        objects.push_back(box);
    }
    const int firstShape = int(objects.size());
    for (int i = 0; i < shapes.size(); ++i) {
        fem::GObject o;
        o.name = QStringLiteral("shape%1").arg(i + 1);
        o.loops.push_back(lineLoop(shapes.at(i).outline, curves));
        objects.push_back(o);
    }
    const int firstPort = int(objects.size());
    for (const PortWritten& w : std::as_const(ports))
        for (const PortElement& e : w.elements) {
            fem::GObject o;
            o.name = QStringLiteral("port%1").arg(e.attribute);
            if (e.vertical) {
                fem::Curve c;
                c.c = e.a;
                c.a = e.b - e.a;
                curves.push_back(c);
                o.open.push_back({e.a, e.b, int(curves.size()) - 1, 0, 1});
            } else {
                o.loops.push_back(lineLoop(e.quad, curves));
            }
            objects.push_back(o);
        }
    QString said;
    const std::shared_ptr<fem::Topology> plan = fem::formUnion(objects, curves, 1e-6, QStringLiteral("um"), 1e-9, &said);
    if (!plan) {
        out.problems << tr("The plan could not be made one: %1").arg(said);
        return out;
    }
    if (!said.isEmpty()) out.notes << tr("The plan: %1").arg(said);
    // Each plan domain's metals and vias (the stack's index of each).
    std::vector<std::vector<int>> domainLayers(plan->domains.size());
    for (std::size_t d = 0; d < plan->domains.size(); ++d)
        for (const int o : plan->domains[d].objects)
            if (o >= firstShape && o < firstPort) {
                const int li = shapes.at(o - firstShape).layer;
                if (std::find(domainLayers[d].begin(), domainLayers[d].end(), li) == domainLayers[d].end()) domainLayers[d].push_back(li);
            }

    // ---- The levels and their divisions in z ----
    std::vector<double> levels{floor, ceiling};
    std::vector<double> fine;   // levels a metal or the ground stands on
    const auto addLevel = [&levels](double z) { levels.push_back(z); };
    if (openGround) addLevel(0);
    else fine.push_back(0);
    for (const Layer& l : stack.layers) {
        if (l.kind == Layer::Substrate || l.kind == Layer::Dielectric || (l.kind == Layer::Cover && lid == nullptr)) {
            addLevel(l.z0 * toUm);
            addLevel(l.z1 * toUm);
        } else if (l.kind == Layer::Metal) {
            addLevel(l.z0 * toUm);
            addLevel((l.z0 + l.thickness) * toUm);
            fine.push_back(l.z0 * toUm);
            fine.push_back((l.z0 + l.thickness) * toUm);
        }
    }
    for (const PortWritten& w : std::as_const(ports))
        for (const PortElement& e : w.elements) {
            if (e.vertical) {
                addLevel(e.z0);
                addLevel(e.z1);
            } else {
                addLevel(e.z);
            }
        }
    // A via's room: between its two metals (the ground at nought).
    QHash<int, std::pair<double, double>> viaRoom;
    for (int i = 0; i < stack.layers.size(); ++i) {
        const Layer& l = stack.layers.at(i);
        if (l.kind != Layer::Via) continue;
        const auto bounds2 = [&](const QString& metal) -> std::pair<double, double> {
            if (stack.isGround(metal)) return {0.0, 0.0};
            const Layer* m = stack.named(metal);
            return m != nullptr ? std::pair<double, double>{m->z0 * toUm, topOf(*m)} : std::pair<double, double>{NAN, NAN};
        };
        const auto [a0, a1] = bounds2(l.from);
        const auto [b0, b1] = bounds2(l.to);
        const double z0 = std::min(a1, b1), z1 = std::max(a0, b0);
        if (!(z1 > z0 + tol)) continue;
        viaRoom.insert(i, {z0, z1});
        addLevel(z0);
        addLevel(z1);
    }
    std::sort(levels.begin(), levels.end());
    {
        std::vector<double> kept;
        for (const double z : levels)
            if (z >= floor - tol && z <= ceiling + tol && (kept.empty() || z - kept.back() > tol)) kept.push_back(z);
        levels = kept;
    }
    const int K = int(levels.size()) - 1;
    if (K < 1) {
        out.problems << tr("The stack has no height to mesh.");
        return out;
    }
    std::vector<double> hLevel(levels.size(), kInfinity);
    for (std::size_t i = 0; i < levels.size(); ++i)
        for (const double z : fine)
            if (std::abs(levels[i] - z) <= tol) hLevel[i] = out.edgesUsed;
    const std::vector<std::vector<double>> cuts =
        fem::sweptDivisions(levels, std::vector<double>(std::size_t(K), out.largestUsed), std::vector<int>(std::size_t(K), 0), hLevel, kGrowth);
    std::vector<double> heights;
    for (int k = 0; k < K; ++k) {
        heights.push_back(levels[std::size_t(k)]);
        for (const double z : cuts[std::size_t(k)]) heights.push_back(z);
    }
    heights.push_back(levels.back());
    const int H = int(heights.size());
    const auto heightIndex = [&](double z) {
        const auto it = std::lower_bound(heights.begin(), heights.end(), z - tol);
        return it != heights.end() && std::abs(*it - z) <= tol ? int(it - heights.begin()) : -1;
    };
    // (Refused before the plan is meshed where even elements of the largest
    //  size everywhere would be too many: a plan of 1e8 triangles took
    //  minutes and gigabytes to make, only to be refused.)
    const auto tooMany = [&](long long tetrahedra) {
        out.problems << tr("The mesh would have %1 tetrahedra or more, past the %2 written at most: give it larger elements (its "
                           "largest element, the elements at the edges) or a smaller margin and air.")
                            .arg(tetrahedra)
                            .arg(kPalaceMostTetrahedra);
    };
    {
        const double fewest = 2.3 * window.width() * window.height() / (out.largestUsed * out.largestUsed);
        if (3 * fewest * (H - 1) > kPalaceMostTetrahedra) {
            tooMany((long long)(3 * fewest * (H - 1)));
            return out;
        }
    }
    // ---- The plan meshed ----
    fem::MeshSizes sizes;
    sizes.hmax = out.largestUsed;
    sizes.hmin = out.edgesUsed / 8;
    sizes.growth = kGrowth;
    sizes.domainMax.assign(plan->domains.size(), kInfinity);
    sizes.boundaryMax.assign(plan->boundaries.size(), kInfinity);
    sizes.pointMax.assign(plan->points.size(), kInfinity);
    sizes.boundaryCount.assign(plan->boundaries.size(), 0);
    for (std::size_t b = 0; b < plan->boundaries.size(); ++b)
        for (const int o : plan->boundaries[b].objects)
            if (o >= firstShape) {
                sizes.boundaryMax[b] = out.edgesUsed;
                break;
            }
    const fem::MeshBuild pm = fem::buildMesh(plan, sizes);
    if (!pm.mesh) {
        out.problems << tr("The plan could not be meshed: %1").arg(pm.error);
        return out;
    }
    const fem::Mesh& m2 = *pm.mesh;
    const int nPlan = int(m2.nodes.size());

    {
        const long long tetrahedra = 3LL * (long long)m2.triangles.size() * (H - 1);
        if (tetrahedra > kPalaceMostTetrahedra) {
            tooMany(tetrahedra);
            return out;
        }
    }

    // ---- The materials: each slab's medium ----
    // (Its dielectric: the slab's own, a metal's room the dielectric over it,
    //  the cover and what is past the stack air.)
    struct Medium {
        QString name;
        const Layer* layer = nullptr;   // null: air
    };
    QList<Medium> media{{QStringLiteral("air"), nullptr}};
    const auto mediumAt = [&](double z) {
        const Layer* found = nullptr;
        for (int i = 0; i < stack.layers.size() && found == nullptr; ++i) {
            const Layer& l = stack.layers.at(i);
            if (!(l.z0 * toUm < z && z < l.z1 * toUm)) continue;
            if (l.kind == Layer::Substrate || l.kind == Layer::Dielectric) found = &l;
            else if (l.kind == Layer::Metal) {
                found = fillAbove(stack, i);
                if (found == nullptr) break;
            } else if (l.kind == Layer::Cover) {
                break;
            }
        }
        for (int i = 0; i < media.size(); ++i)
            if (media.at(i).layer == found) return i + 1;
        media << Medium{found->name, found};
        return int(media.size());
    };
    // Each division's medium, and what is cut out of it (the stack's index
    // of the metal or via, by plan domain).
    std::vector<int> divisionMedium(std::size_t(H - 1));
    std::vector<double> divisionMiddle(std::size_t(H - 1));
    for (int j = 0; j + 1 < H; ++j) {
        divisionMiddle[std::size_t(j)] = (heights[std::size_t(j)] + heights[std::size_t(j) + 1]) / 2;
        divisionMedium[std::size_t(j)] = mediumAt(divisionMiddle[std::size_t(j)]);
    }
    const auto solidAt = [&](int domain, double z) {
        for (const int li : domainLayers[std::size_t(domain)]) {
            const Layer& l = stack.layers.at(li);
            if (l.kind == Layer::Metal && !options.sheets && l.thickness > 0 && l.z0 * toUm < z && z < (l.z0 + l.thickness) * toUm) return li;
            if (l.kind == Layer::Via && viaRoom.contains(li) && viaRoom.value(li).first < z && z < viaRoom.value(li).second) return li;
        }
        return -1;
    };

    // ---- The prisms split into tetrahedra ----
    // (A node is plan node p at height h: h * nPlan + p. Every quadrilateral
    //  face split along the diagonal through its lowest-numbered node, so
    //  that the two prisms either side of it split it alike.)
    struct Tet {
        std::array<int, 4> v;
        int attribute;   // above nought a medium; below, -(the stack's index of the conductor cut out) - 1
    };
    std::vector<Tet> tets;
    tets.reserve(std::size_t(m2.triangles.size()) * std::size_t(H - 1) * 3);
    for (int j = 0; j + 1 < H; ++j) {
        for (std::size_t t = 0; t < m2.triangles.size(); ++t) {
            const std::array<int, 3>& tri = m2.triangles[t];
            const int solid = solidAt(m2.domain[t], divisionMiddle[std::size_t(j)]);
            const int attribute = solid >= 0 ? -solid - 1 : divisionMedium[std::size_t(j)];
            std::array<int, 6> v{{j * nPlan + tri[0], j * nPlan + tri[1], j * nPlan + tri[2], (j + 1) * nPlan + tri[0],
                                  (j + 1) * nPlan + tri[1], (j + 1) * nPlan + tri[2]}};
            int least = 0;
            for (int i = 1; i < 6; ++i)
                if (v[std::size_t(i)] < v[std::size_t(least)]) least = i;
            const int r = least % 3;
            const bool flip = least >= 3;
            std::array<int, 6> u{};
            for (int i = 0; i < 3; ++i) {
                u[std::size_t(i)] = v[std::size_t((i + r) % 3 + (flip ? 3 : 0))];
                u[std::size_t(3 + i)] = v[std::size_t((i + r) % 3 + (flip ? 0 : 3))];
            }
            if (std::min(u[1], u[5]) < std::min(u[2], u[4])) {
                tets.push_back({{u[0], u[1], u[2], u[5]}, attribute});
                tets.push_back({{u[0], u[1], u[5], u[4]}, attribute});
            } else {
                tets.push_back({{u[0], u[1], u[2], u[4]}, attribute});
                tets.push_back({{u[0], u[4], u[2], u[5]}, attribute});
            }
            tets.push_back({{u[0], u[4], u[5], u[3]}, attribute});
        }
    }
    const auto position = [&](int g) {
        const QPointF& q = m2.nodes[std::size_t(g % nPlan)];
        return std::array<double, 3>{{q.x(), q.y(), heights[std::size_t(g / nPlan)]}};
    };

    // ---- The boundaries ----
    // Conductors' surfaces by the stack's index; the box's sides, floor, ceiling.
    QHash<int, int> conductorGroup;
    const auto conductorAttribute = [&](int li) {
        if (conductorGroup.contains(li)) return conductorGroup.value(li);
        const Layer& l = stack.layers.at(li);
        const bool lossy = l.sigma > 0 && l.thickness > 0;
        // (A sheet's loss is of its thickness: both its faces on one sheet.)
        const bool sheet = options.sheets && l.kind == Layer::Metal;
        const int a = addGroup(l.name, lossy ? QStringLiteral("conductivity") : QStringLiteral("pec"), lossy ? l.sigma : 0,
                               lossy && sheet ? l.thickness * toUm : 0);
        conductorGroup.insert(li, a);
        return a;
    };
    int absorbing = 0, floorGroup = 0, ceilingGroup = 0, wallGroup = 0;
    const auto absorbingAttribute = [&]() {
        if (absorbing == 0) absorbing = addGroup(QStringLiteral("absorbing"), QStringLiteral("absorbing"));
        return absorbing;
    };
    if (!openGround) {
        // (A surface impedance: its value as the ground's node gives it.)
        double rs = 0;
        if (ground->variant == QLatin1String("impedance"))
            for (const Node& n : model.stackNode().children)
                if (n.type == QLatin1String("rf_ground") && n.enabled) rs = numberOf(n, QStringLiteral("impedance"), model, 0);
        if (ground->variant == QLatin1String("metal") && ground->sigma > 0)
            floorGroup = addGroup(ground->name, QStringLiteral("conductivity"), ground->sigma);
        else if (rs > 0)
            floorGroup = addGroup(ground->name, QStringLiteral("impedance"), rs);
        else
            floorGroup = addGroup(ground->name, QStringLiteral("pec"));
    }
    if (lid != nullptr) ceilingGroup = addGroup(lid->name, QStringLiteral("pec"));
    if (walls) wallGroup = addGroup(QStringLiteral("walls"), QStringLiteral("pec"));
    else if (options.sides == QLatin1String("perfect")) wallGroup = addGroup(QStringLiteral("sides"), QStringLiteral("pec"));

    struct Boundary {
        FaceKey key;
        std::array<int, 3> v;
        int attribute;
    };
    std::vector<Boundary> boundaries;
    {
        struct Entry {
            FaceKey key;
            std::array<int, 3> v;
            int tet;
        };
        std::vector<Entry> faces;
        faces.reserve(tets.size() * 4);
        static const int local[4][3] = {{1, 2, 3}, {0, 3, 2}, {0, 1, 3}, {0, 2, 1}};
        for (std::size_t t = 0; t < tets.size(); ++t)
            for (const auto& f : local) {
                const std::array<int, 3> v{{tets[t].v[std::size_t(f[0])], tets[t].v[std::size_t(f[1])], tets[t].v[std::size_t(f[2])]}};
                faces.push_back({keyOf(v[0], v[1], v[2]), v, int(t)});
            }
        std::sort(faces.begin(), faces.end(), [](const Entry& x, const Entry& y) { return x.key < y.key; });
        // A face on a port's rectangle: that port element's attribute, or 0.
        const auto portOf = [&](const std::array<int, 3>& v) {
            const std::array<double, 3> p0 = position(v[0]), p1 = position(v[1]), p2 = position(v[2]);
            for (const PortWritten& w : std::as_const(ports))
                for (const PortElement& e : w.elements) {
                    bool on = true;
                    for (const auto& p : {p0, p1, p2}) {
                        if (e.vertical) {
                            const QLineF line(e.a, e.b);
                            const double len = line.length();
                            const QPointF d = (e.b - e.a) / len;
                            const QPointF q(p[0] - e.a.x(), p[1] - e.a.y());
                            const double along = q.x() * d.x() + q.y() * d.y(), across = q.x() * d.y() - q.y() * d.x();
                            on = std::abs(across) <= tol && along >= -tol && along <= len + tol && p[2] >= e.z0 - tol && p[2] <= e.z1 + tol;
                        } else {
                            on = std::abs(p[2] - e.z) <= tol
                                 && (e.quad.containsPoint(QPointF(p[0], p[1]), Qt::OddEvenFill)
                                     || [&] {
                                            for (int i = 0, n = int(e.quad.size()); i < n; ++i) {
                                                const QLineF s(e.quad.at(i), e.quad.at((i + 1) % n));
                                                const double len = s.length();
                                                const QPointF d = (s.p2() - s.p1()) / len;
                                                const QPointF q(p[0] - s.p1().x(), p[1] - s.p1().y());
                                                const double along = q.x() * d.x() + q.y() * d.y(), across = q.x() * d.y() - q.y() * d.x();
                                                if (std::abs(across) <= tol && along >= -tol && along <= len + tol) return true;
                                            }
                                            return false;
                                        }());
                        }
                        if (!on) break;
                    }
                    if (on) return e.attribute;
                }
            return 0;
        };
        std::size_t i = 0;
        while (i < faces.size()) {
            std::size_t j = i + 1;
            while (j < faces.size() && faces[j].key == faces[i].key) ++j;
            const Entry& first = faces[i];
            if (j - i == 1) {
                const Tet& t = tets[std::size_t(first.tet)];
                if (t.attribute > 0) {
                    // On the box: its floor, its ceiling or a side.
                    const std::array<double, 3> p0 = position(first.v[0]), p1 = position(first.v[1]), p2 = position(first.v[2]);
                    const auto all = [&](int k, double value) {
                        return std::abs(p0[std::size_t(k)] - value) <= tol && std::abs(p1[std::size_t(k)] - value) <= tol
                               && std::abs(p2[std::size_t(k)] - value) <= tol;
                    };
                    int a = 0;
                    if (all(2, floor)) a = openGround ? absorbingAttribute() : floorGroup;
                    else if (all(2, ceiling)) a = lid != nullptr ? ceilingGroup : absorbingAttribute();
                    else if (all(0, window.left()) || all(0, window.right()) || all(1, window.top()) || all(1, window.bottom()))
                        a = wallGroup > 0 ? wallGroup : absorbingAttribute();
                    if (const int port = portOf(first.v); port > 0) a = port;
                    if (a > 0) boundaries.push_back({first.key, first.v, a});
                }
            } else {
                const Tet& t1 = tets[std::size_t(first.tet)];
                const Tet& t2 = tets[std::size_t(faces[i + 1].tet)];
                if ((t1.attribute > 0) != (t2.attribute > 0)) {
                    // A conductor's surface, facing out of it.
                    const Entry& kept = t1.attribute > 0 ? faces[i] : faces[i + 1];
                    const int li = -(t1.attribute > 0 ? t2.attribute : t1.attribute) - 1;
                    boundaries.push_back({kept.key, kept.v, conductorAttribute(li)});
                } else if (t1.attribute > 0) {
                    if (const int port = portOf(first.v); port > 0) boundaries.push_back({first.key, first.v, port});
                }
            }
            i = j;
        }
    }
    // The sheets: each metal at its bottom where its shapes are.
    {
        std::vector<FaceKey> taken;
        taken.reserve(boundaries.size());
        for (const Boundary& b : boundaries) taken.push_back(b.key);
        std::sort(taken.begin(), taken.end());
        for (std::size_t t = 0; t < m2.triangles.size(); ++t)
            for (const int li : domainLayers[std::size_t(m2.domain[t])]) {
                const Layer& l = stack.layers.at(li);
                if (l.kind != Layer::Metal || (!options.sheets && l.thickness > 0)) continue;
                const int h = heightIndex(l.z0 * toUm);
                if (h < 0) continue;
                const std::array<int, 3>& tri = m2.triangles[t];
                const std::array<int, 3> v{{h * nPlan + tri[0], h * nPlan + tri[1], h * nPlan + tri[2]}};
                const FaceKey key = keyOf(v[0], v[1], v[2]);
                if (std::binary_search(taken.begin(), taken.end(), key)) continue;
                boundaries.push_back({key, v, conductorAttribute(li)});
            }
    }
    // Each port element's area as meshed, against its rectangle's.
    for (const PortWritten& w : std::as_const(ports))
        for (const PortElement& e : w.elements) {
            double area = 0;
            for (const Boundary& b : boundaries) {
                if (b.attribute != e.attribute) continue;
                const std::array<double, 3> p0 = position(b.v[0]), p1 = position(b.v[1]), p2 = position(b.v[2]);
                const std::array<double, 3> u{{p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]}}, v{{p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]}};
                const double cx = u[1] * v[2] - u[2] * v[1], cy = u[2] * v[0] - u[0] * v[2], cz = u[0] * v[1] - u[1] * v[0];
                area += std::sqrt(cx * cx + cy * cy + cz * cz) / 2;
            }
            if (std::abs(area - e.area) > 1e-6 * std::max(e.area, 1e-12)) {
                out.problems << tr("%1's rectangle came out of the mesh %2 µm² of %3: it crosses a conductor or the box's side.")
                                    .arg(w.name, um(area), um(e.area));
                return out;
            }
        }

    // ---- Nodes in use, numbered from 1; tetrahedra turned positive ----
    std::vector<int> number(std::size_t(nPlan) * std::size_t(H), 0);
    for (const Tet& t : tets)
        if (t.attribute > 0)
            for (const int g : t.v) number[std::size_t(g)] = 1;
    for (const Boundary& b : boundaries)
        for (const int g : b.v) number[std::size_t(g)] = 1;
    int nodes = 0;
    for (int& n : number)
        if (n != 0) n = ++nodes;
    std::string msh;
    msh.reserve(std::size_t(nodes) * 60 + tets.size() * 40);
    msh += "$MeshFormat\n2.2 0 8\n$EndMeshFormat\n$PhysicalNames\n";
    append(msh, (long long)(media.size() + groups.size()));
    msh += '\n';
    for (int i = 0; i < media.size(); ++i) {
        msh += "3 ";
        append(msh, (long long)(i + 1));
        msh += " \"" + physicalName(media.at(i).name) + "\"\n";
    }
    for (int i = 0; i < groups.size(); ++i) {
        msh += "2 ";
        append(msh, (long long)(i + 1));
        msh += " \"" + physicalName(groups.at(i).name) + "\"\n";
    }
    msh += "$EndPhysicalNames\n$Nodes\n";
    append(msh, (long long)nodes);
    msh += '\n';
    for (std::size_t g = 0; g < number.size(); ++g) {
        if (number[g] == 0) continue;
        const std::array<double, 3> p = position(int(g));
        append(msh, (long long)number[g]);
        for (const double c : p) {
            msh += ' ';
            append(msh, c);
        }
        msh += '\n';
    }
    msh += "$EndNodes\n$Elements\n";
    long long elements = 0;
    for (const Tet& t : tets) elements += t.attribute > 0;
    out.tetrahedra = int(elements);
    out.triangles = int(boundaries.size());
    elements += (long long)boundaries.size();
    append(msh, elements);
    msh += '\n';
    long long id = 0;
    for (const Boundary& b : boundaries) {
        msh += std::to_string(++id) + " 2 2 " + std::to_string(b.attribute) + ' ' + std::to_string(b.attribute);
        for (const int g : b.v) msh += ' ' + std::to_string(number[std::size_t(g)]);
        msh += '\n';
    }
    double volume = 0;
    for (const Tet& t : tets) {
        if (t.attribute <= 0) continue;
        std::array<int, 4> v = t.v;
        const std::array<double, 3> a = position(v[0]), b = position(v[1]), c = position(v[2]), d = position(v[3]);
        const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
        const double vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
        const double wx = d[0] - a[0], wy = d[1] - a[1], wz = d[2] - a[2];
        const double det = ux * (vy * wz - vz * wy) - uy * (vx * wz - vz * wx) + uz * (vx * wy - vy * wx);
        // (The lowest node of a prism is on its floor, so the split above
        //  turns each one positive already: kept as a guard.)
        if (det < 0) std::swap(v[2], v[3]);
        volume += std::abs(det) / 6;
        msh += std::to_string(++id) + " 4 2 " + std::to_string(t.attribute) + ' ' + std::to_string(t.attribute);
        for (const int g : v) msh += ' ' + std::to_string(number[std::size_t(g)]);
        msh += '\n';
    }
    msh += "$EndElements\n";
    out.mesh = QByteArray::fromStdString(msh);
    out.nodes = nodes;
    out.materials = int(media.size());

    // ---- The configuration ----
    QJsonArray materials;
    for (int i = 0; i < media.size(); ++i) {
        const Layer* l = media.at(i).layer;
        QJsonObject m{{QStringLiteral("Attributes"), QJsonArray{i + 1}}};
        if (l == nullptr) {
            m.insert(QStringLiteral("Permittivity"), 1.0);
            m.insert(QStringLiteral("Permeability"), 1.0);
        } else {
            const double ex = l->averaged() ? l->epsx : l->epsr, ey = l->averaged() ? l->epsy : l->epsr;
            const double ez = l->epsz > 0 ? l->epsz : l->epsr;
            if (ex == ey && ey == ez) m.insert(QStringLiteral("Permittivity"), l->epsr);
            else m.insert(QStringLiteral("Permittivity"), QJsonArray{ex, ey, ez});
            m.insert(QStringLiteral("Permeability"), l->mur);
            if (l->tand > 0) m.insert(QStringLiteral("LossTan"), l->tand);
            if (l->sigma > 0) m.insert(QStringLiteral("Conductivity"), l->sigma);
        }
        materials.append(m);
    }
    QJsonArray pec, absorbingList;
    QJsonArray conductivity, impedance, lumpedPorts;
    for (int i = 0; i < groups.size(); ++i) {
        const BoundaryGroup& g = groups.at(i);
        const int a = i + 1;
        if (g.kind == QLatin1String("pec")) pec.append(a);
        else if (g.kind == QLatin1String("absorbing")) absorbingList.append(a);
        else if (g.kind == QLatin1String("conductivity")) {
            QJsonObject o{{QStringLiteral("Attributes"), QJsonArray{a}}, {QStringLiteral("Conductivity"), g.value}};
            if (g.thickness > 0) o.insert(QStringLiteral("Thickness"), g.thickness);
            conductivity.append(o);
        }
        else if (g.kind == QLatin1String("impedance"))
            impedance.append(QJsonObject{{QStringLiteral("Attributes"), QJsonArray{a}}, {QStringLiteral("Rs"), g.value}});
    }
    QList<PortWritten> ordered = ports;
    std::sort(ordered.begin(), ordered.end(), [](const PortWritten& x, const PortWritten& y) { return x.index < y.index; });
    for (const PortWritten& w : std::as_const(ordered)) {
        QJsonObject port{{QStringLiteral("Index"), w.index}};
        if (w.r > 0) port.insert(QStringLiteral("R"), w.r);
        if (w.l > 0) port.insert(QStringLiteral("L"), w.l);
        if (w.c > 0) port.insert(QStringLiteral("C"), w.c);
        if (w.driven) port.insert(QStringLiteral("Excitation"), w.index);
        if (w.elements.size() == 1) {
            port.insert(QStringLiteral("Attributes"), QJsonArray{w.elements.first().attribute});
            port.insert(QStringLiteral("Direction"), directionJson(w.elements.first().direction));
        } else {
            QJsonArray elements2;
            for (const PortElement& e : w.elements)
                elements2.append(QJsonObject{{QStringLiteral("Attributes"), QJsonArray{e.attribute}},
                                             {QStringLiteral("Direction"), directionJson(e.direction)}});
            port.insert(QStringLiteral("Elements"), elements2);
        }
        lumpedPorts.append(port);
    }
    QJsonObject boundariesJson;
    if (!pec.isEmpty()) boundariesJson.insert(QStringLiteral("PEC"), QJsonObject{{QStringLiteral("Attributes"), pec}});
    if (!conductivity.isEmpty()) boundariesJson.insert(QStringLiteral("Conductivity"), conductivity);
    if (!impedance.isEmpty()) boundariesJson.insert(QStringLiteral("Impedance"), impedance);
    if (!absorbingList.isEmpty())
        boundariesJson.insert(QStringLiteral("Absorbing"), QJsonObject{{QStringLiteral("Attributes"), absorbingList}, {QStringLiteral("Order"), 1}});
    boundariesJson.insert(QStringLiteral("LumpedPort"), lumpedPorts);

    const Node* studyNode = model.root.child(QStringLiteral("rf_study"));
    const bool logarithmic = studyNode != nullptr && textOf(*studyNode, QStringLiteral("scale")) == QLatin1String("log");
    QJsonArray samples;
    const double f0 = sweep.first() / 1e9, f1 = sweep.last() / 1e9;
    if (sweep.size() == 1)
        samples.append(QJsonObject{{QStringLiteral("Type"), QStringLiteral("Point")}, {QStringLiteral("Freq"), QJsonArray{f0}}});
    else
        samples.append(QJsonObject{{QStringLiteral("Type"), logarithmic ? QStringLiteral("Log") : QStringLiteral("Linear")},
                                   {QStringLiteral("MinFreq"), f0},
                                   {QStringLiteral("MaxFreq"), f1},
                                   {QStringLiteral("NSample"), int(sweep.size())}});
    QJsonObject driven{{QStringLiteral("Samples"), samples}, {QStringLiteral("Save"), QJsonArray()}};
    if (options.adaptiveTolerance > 0 && sweep.size() > 2) driven.insert(QStringLiteral("AdaptiveTol"), options.adaptiveTolerance);
    const QJsonObject linear{{QStringLiteral("Type"), options.solver == QLatin1String("ams") ? QStringLiteral("AMS") : QStringLiteral("Default")},
                             {QStringLiteral("KSPType"), QStringLiteral("GMRES")},
                             {QStringLiteral("Tol"), 1.0e-6},
                             {QStringLiteral("MaxIts"), 1000}};
    const QJsonObject root{
        {QStringLiteral("Problem"),
         QJsonObject{{QStringLiteral("Type"), QStringLiteral("Driven")}, {QStringLiteral("Verbose"), 2}, {QStringLiteral("Output"), QStringLiteral("postpro_") + out.name}}},
        {QStringLiteral("Model"), QJsonObject{{QStringLiteral("Mesh"), out.name + QStringLiteral(".msh")}, {QStringLiteral("L0"), 1.0e-6}}},
        {QStringLiteral("Domains"), QJsonObject{{QStringLiteral("Materials"), materials}}},
        {QStringLiteral("Boundaries"), boundariesJson},
        {QStringLiteral("Solver"),
         QJsonObject{{QStringLiteral("Order"), options.order}, {QStringLiteral("Device"), QStringLiteral("CPU")}, {QStringLiteral("Driven"), driven}, {QStringLiteral("Linear"), linear}}}};
    // (Palace strips comments before it reads the rest.)
    QByteArray head = QStringLiteral("// Palace driven solve of the RF 2.5D model %1%2. Run it in this folder: palace -np 4 %1.json\n")
                          .arg(out.name, madeBy.isEmpty() ? QString() : tr(", written by %1").arg(madeBy))
                          .toUtf8();
    out.config = head + QJsonDocument(root).toJson(QJsonDocument::Indented);
    out.volume = volume;
    out.floor = floor;
    out.ceiling = ceiling;
    out.box = window;

    // ---- Where it is not what RF 2.5D solves ----
    out.notes.prepend(tr("A mesh of %1 tetrahedra on %2 nodes (elements %3 µm at the edges, %4 µm at the largest); run it with "
                         "\"palace -np 4 %5.json\" in its folder - Palace reads the mesh from there and writes its S into "
                         "postpro_%5/port-S.csv.")
                          .arg(out.tetrahedra)
                          .arg(out.nodes)
                          .arg(um(out.edgesUsed), um(out.largestUsed), out.name));
    if (!walls)
        out.notes << tr("RF 2.5D has the stack without end across the plane; here it stops %1 µm past the shapes at %2%3: make "
                        "it larger if S moves with it.")
                         .arg(um(out.marginUsed),
                              options.sides == QLatin1String("perfect") ? tr("perfectly conducting sides") : tr("an absorbing boundary"),
                              lid == nullptr ? tr(", and the air above at %1 µm to an absorbing boundary").arg(um(out.airUsed)) : QString());
    if (stack.dispersive())
        out.notes << tr("Djordjevic-Sarkar dispersion is not written: Palace takes a permittivity and a loss tangent alone, written "
                        "as given at their frequency.");
    for (const Layer& l : stack.layers)
        if ((l.kind == Layer::Substrate || l.kind == Layer::Dielectric) && l.mtand > 0) {
            out.notes << tr("%1's magnetic loss tangent is not written: Palace has none.").arg(l.name);
            break;
        }
    for (const Layer& l : stack.layers)
        if ((l.kind == Layer::Metal || l.kind == Layer::Via) && l.roughness > 0) {
            out.notes << tr("The metals' roughness is not written: Palace's conductor is smooth.");
            break;
        }
    if (options.sheets)
        out.notes << tr("Each metal is a sheet at its bottom, as RF 2.5D solves a sheet: its thickness is in its loss alone, and its "
                        "room is the dielectric over it.");
    {
        QStringList wires;
        for (const Node& n : model.layout().children)
            if (n.type == QLatin1String("rf_shapes"))
                for (const Node& c : n.children)
                    if (c.type == QLatin1String("rf_bondwire") && c.enabled) wires << nameOf(c);
        if (!wires.isEmpty()) out.notes << tr("Bond wires are not written: %1.").arg(wires.join(QStringLiteral(", ")));
    }
    if (out.lumped > 0)
        out.notes << tr("Palace's port-S.csv has a row for each lumped element too, after the measuring ports': it is the element's "
                        "own, not a port to measure at.");
    return out;
}

bool writePalaceExport(const PalaceExport& written, const QString& configPath, QStringList* files, QString* error)
{
    const QFileInfo info(configPath);
    const QString meshPath = info.absoluteDir().filePath(written.name + QStringLiteral(".msh"));
    const auto put = [error](const QString& where, const QByteArray& bytes) {
        QSaveFile file(where);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            if (error != nullptr)
                *error = QCoreApplication::translate("Rf25Palace", "%1 could not be written: %2.").arg(QDir::toNativeSeparators(where), file.errorString());
            return false;
        }
        return true;
    };
    // (The mesh first: a configuration is never left naming a mesh not there.)
    if (!put(meshPath, written.mesh) || !put(info.absoluteFilePath(), written.config)) return false;
    if (files != nullptr) *files = QStringList{info.absoluteFilePath(), meshPath};
    return true;
}

} // namespace qucs_s::rf25
