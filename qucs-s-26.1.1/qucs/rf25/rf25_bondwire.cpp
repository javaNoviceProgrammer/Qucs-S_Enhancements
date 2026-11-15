/*
 * rf25_bondwire.cpp - RF 2.5D: bond wires made of air slabs, metal layers
 * and vias (rf25_bondwire.h).
 */
#include "rf25_bondwire.h"
#include "rf25_sparams.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>

namespace qucs_s::rf25 {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("RF25BondWire", text);
}

/// A metal's height over the ground, m (the metals take no room), or -1.
double metalHeight(const Stack& stack, const QString& metal)
{
    double z = 0;
    for (const Layer& l : stack.layers) {
        if (l.kind == Layer::Metal && (l.name == metal || l.tag == metal)) return z;
        if (l.kind == Layer::Dielectric || l.kind == Layer::Substrate) z += l.thickness;
    }
    return -1;
}

/// A strip \a width (µm) wide from \a a to \a b (µm), its ends run on by
/// \a over (µm) each.
QPolygonF stripBetween(QPointF a, QPointF b, double width, double over)
{
    const QPointF d = b - a;
    const double length = std::hypot(d.x(), d.y());
    const QPointF t = length > 0 ? d / length : QPointF(1, 0), n(-t.y(), t.x());
    const QPointF p0 = a - over * t, p1 = b + over * t;
    return QPolygonF({p0 + 0.5 * width * n, p1 + 0.5 * width * n, p1 - 0.5 * width * n, p0 - 0.5 * width * n});
}

} // namespace

QList<BondWire> bondWiresOf(const Model& model, QStringList* problems)
{
    QList<BondWire> out;
    // (A model without a layout - a stack's alone - has no wires: layout()
    //  is not there to be read.)
    const Node* layout = model.root.child(QStringLiteral("rf_layout"));
    const Node* shapes = layout != nullptr ? layout->child(QStringLiteral("rf_shapes")) : nullptr;
    if (shapes == nullptr) return out;
    for (const Node& n : shapes->children) {
        if (!n.enabled || n.type != QLatin1String("rf_bondwire")) continue;
        BondWire w;
        w.name = n.label.isEmpty() ? n.tag : n.label;
        w.fromMetal = textOf(n, QStringLiteral("from_metal")).trimmed();
        w.toMetal = textOf(n, QStringLiteral("to_metal")).trimmed();
        bool ok = true, all = true;
        const auto number = [&](const char* key, double fallback) {
            const double v = numberOf(n, QLatin1String(key), model, fallback, &ok);
            all = all && ok;
            return v;
        };
        w.from = QPointF(number("x0", 0), number("y0", 0)) / 1e-6;
        w.to = QPointF(number("x1", 0), number("y1", 0)) / 1e-6;
        w.height = number("height", 150e-6);
        w.diameter = number("diameter", 25e-6);
        w.sigma = number("sigma", 4.1e7);
        w.pieces = int(std::lround(number("pieces", 5)));
        if (!all || w.fromMetal.isEmpty() || w.toMetal.isEmpty()) {
            if (problems != nullptr)
                *problems << tr("Shapes: the bond wire %1 is not read: its metals are named, and its numbers numbers.").arg(w.name);
            continue;
        }
        out << w;
    }
    return out;
}

QList<double> bondWireLevels(double zA, double zB, double height, int pieces)
{
    QList<double> out;
    // Gauss's rule, sixteen points a piece, on ln z(u).
    static const double x[8] = {0.0950125098376374, 0.2816035507792589, 0.4580167776572274, 0.6178762444026438,
                                0.7554044083550030, 0.8656312023878318, 0.9445750230732326, 0.9894009349916499};
    static const double w[8] = {0.1894506104550685, 0.1826034150449236, 0.1691565193950025, 0.1495959888165767,
                                0.1246289712555339, 0.0951585116824928, 0.0622535239386479, 0.0271524594117541};
    for (int i = 0; i < pieces; ++i) {
        const double u0 = double(i) / pieces, u1 = double(i + 1) / pieces;
        double mean = 0;
        for (int k = 0; k < 8; ++k)
            for (const double sign : {-1.0, 1.0}) {
                const double u = 0.5 * (u0 + u1) + 0.5 * (u1 - u0) * sign * x[k];
                mean += 0.5 * w[k] * std::log(zA * (1 - u) + zB * u + 4 * height * u * (1 - u));
            }
        out << std::exp(mean);
    }
    return out;
}

BondWired withBondWires(const Stack& stack, const QList<Conductor>& conductors, const QList<BondWire>& wires)
{
    BondWired out;
    out.stack = stack;
    out.conductors = conductors;
    if (wires.isEmpty()) return out;
    // Where the stack's dielectrics end, and its cover.
    double top = 0;
    int coverAt = -1;
    for (int i = 0; i < stack.layers.size(); ++i) {
        const Layer& l = stack.layers.at(i);
        if (l.kind == Layer::Dielectric || l.kind == Layer::Substrate) top += l.thickness;
        if (l.kind == Layer::Cover) coverAt = i;
    }
    struct Level {
        double z;
        QString metal;
        double sigma, thickness;
    };
    QList<Level> levels;
    QList<Layer> vias;
    for (const BondWire& wire : wires) {
        const double zA = metalHeight(stack, wire.fromMetal), zB = metalHeight(stack, wire.toMetal);
        if (zA < 0 || zB < 0) {
            out.problems << tr("%1 lands on no metal of the stack: its pads are on %2 and %3.").arg(wire.name, wire.fromMetal, wire.toMetal);
            return out;
        }
        if (!(wire.diameter > 0) || !(wire.height > 0) || wire.pieces < 1) {
            out.problems << tr("%1 has no diameter, no height or no pieces.").arg(wire.name);
            return out;
        }
        const QList<double> z = bondWireLevels(zA, zB, wire.height, wire.pieces);
        for (const double h : z)
            if (h <= top * (1 + 1e-12)) {
                out.problems << tr("%1 runs through the stack's dielectrics (a piece of it at %2 µm, their top at %3 µm): its pads "
                                   "are on a buried metal, or its loop is too low.")
                                    .arg(wire.name).arg(h * 1e6, 0, 'g', 4).arg(top * 1e6, 0, 'g', 4);
                return out;
            }
        const double a = wire.diameter / 2;
        // (Its strips' metal: 4a/pi thick, sigma (pi/4)^2 - a round wire's R
        //  per length both at direct current and with the skin thin.)
        const double thickness = 4 * a / M_PI, sigma = wire.sigma * M_PI * M_PI / 16;
        QStringList names;
        for (int i = 0; i < z.size(); ++i) {
            // (Pieces as high as each other, one after another, are one run on
            //  one metal, with no leg between: a via of no length is nothing to
            //  solve, and two strips over each other are not one.)
            const bool same = i > 0 && std::abs(z.at(i) - z.at(i - 1)) <= 1e-9 * z.at(i);
            const QString name = same ? names.last() : QStringLiteral("%1_%2").arg(wire.name).arg(i + 1);
            if (!same) levels << Level{z.at(i), name, sigma, thickness};
            names << name;
        }
        for (int i = 0; i < z.size();) {
            int j = i;
            while (j + 1 < z.size() && names.at(j + 1) == names.at(i)) ++j;
            // Its run: along the span over its pieces, run on over the legs
            // at its ends.
            const QPointF p0 = wire.from + (wire.to - wire.from) * (double(i) / z.size());
            const QPointF p1 = wire.from + (wire.to - wire.from) * (double(j + 1) / z.size());
            Conductor run;
            run.layer = names.at(i);
            run.outline = stripBetween(p0, p1, 4 * a * 1e6, 0.871 * wire.diameter * 1e6 / 2);
            out.conductors << run;
            i = j + 1;
        }
        // Its legs: up from each pad, and between the pieces where they step.
        const auto leg = [&](const QString& from, const QString& to, QPointF at, int index) {
            Layer via;
            via.kind = Layer::Via;
            via.name = QStringLiteral("%1_v%2").arg(wire.name).arg(index);
            via.tag = via.name.toLower();
            via.from = from;
            via.to = to;
            via.sigma = wire.sigma;
            vias << via;
            const double side = 0.871 * wire.diameter * 1e6;
            Conductor square;
            square.layer = via.name;
            square.outline = QPolygonF(QRectF(at.x() - side / 2, at.y() - side / 2, side, side));
            out.conductors << square;
        };
        leg(wire.fromMetal, names.first(), wire.from, 0);
        for (int i = 0; i + 1 < names.size(); ++i)
            if (names.at(i) != names.at(i + 1))
                leg(names.at(i), names.at(i + 1), wire.from + (wire.to - wire.from) * (double(i + 1) / z.size()), i + 1);
        leg(names.last(), wire.toMetal, wire.to, int(names.size()));
        out.lines << tr("%1: a bond wire %2 µm across from %3 to %4, its arc %5 µm over its pads made of %6 run(s) at %7 µm and "
                        "their legs.")
                         .arg(wire.name).arg(wire.diameter * 1e6, 0, 'g', 3).arg(wire.fromMetal, wire.toMetal)
                         .arg(wire.height * 1e6, 0, 'g', 4).arg(z.size())
                         .arg([&z] {
                             QStringList s;
                             for (const double h : z) s << QString::number(h * 1e6, 'g', 4);
                             return s.join(QStringLiteral(", "));
                         }());
    }
    // The levels from the bottom up, each an air slab to it and its metal;
    // under a cover, its gap the less.
    std::sort(levels.begin(), levels.end(), [](const Level& p, const Level& q) { return p.z < q.z; });
    QList<Layer> added;
    double z = top;
    for (const Level& lv : std::as_const(levels)) {
        if (lv.z > z * (1 + 1e-12)) {
            Layer air;
            air.kind = Layer::Dielectric;
            air.name = QStringLiteral("%1_air").arg(lv.metal);
            air.tag = air.name.toLower();
            air.thickness = lv.z - z;
            air.epsr = 1;
            added << air;
            z = lv.z;
        }
        Layer metal;
        metal.kind = Layer::Metal;
        metal.name = lv.metal;
        metal.tag = lv.metal.toLower();
        metal.thickness = lv.thickness;
        metal.sigma = lv.sigma;
        metal.roundWire = true;
        added << metal;
    }
    if (coverAt >= 0) {
        Layer& cover = out.stack.layers[coverAt];
        if (cover.variant == QLatin1String("perfect")) {
            if (z - top >= cover.thickness) {
                out.problems << tr("A bond wire reaches the lid: its loop is higher than the cover's gap.");
                return out;
            }
            cover.thickness -= z - top;
        }
        // (Before the cover, which closes the stack: the vias too.)
        added << vias;
        for (int i = 0; i < added.size(); ++i) out.stack.layers.insert(coverAt + i, added.at(i));
    } else {
        out.stack.layers << added << vias;
    }
    return out;
}

} // namespace qucs_s::rf25
