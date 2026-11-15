/*
 * rf25_bondwire.cpp - RF 2.5D: bond wires made of air slabs, metal layers
 * and vias (rf25_bondwire.h).
 */
#include "rf25_bondwire.h"
#include "rf25_sparams.h"

#include <QCoreApplication>
#include <QJsonValue>

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
        w.pieces = int(std::lround(number("pieces", 3)));
        w.ball = textOf(n, QStringLiteral("profile")).trimmed() == QLatin1String("ball");
        if (w.ball) w.knee = number("knee", 0.15);
        {
            const QJsonValue fit = n.props.value(QLatin1String("fit"));
            w.fit = fit.isBool() ? fit.toBool() : fit.isString() ? fit.toString() != QLatin1String("false") : true;
        }
        if (!all || w.fromMetal.isEmpty() || w.toMetal.isEmpty()) {
            if (problems != nullptr)
                *problems << tr("Shapes: the bond wire %1 is not read: its metals are named, and its numbers numbers.").arg(w.name);
            continue;
        }
        out << w;
    }
    return out;
}

namespace {

/// A profile's height at \a u along its span (0 to 1): an arc's, or a ball
/// bond's loop.
double profileAt(bool ball, double knee, double zA, double zB, double height, double u)
{
    if (!ball) return zA * (1 - u) + zB * u + 4 * height * u * (1 - u);
    const double top = zA + height;
    return u <= knee ? top : top + (zB - top) * (u - knee) / (1 - knee);
}

QList<double> levelsOf(bool ball, double knee, double zA, double zB, double height, int pieces)
{
    QList<double> out;
    // Gauss's rule, sixteen points a piece, on ln z(u) - a ball bond's piece
    // over its knee in two parts.
    static const double x[8] = {0.0950125098376374, 0.2816035507792589, 0.4580167776572274, 0.6178762444026438,
                                0.7554044083550030, 0.8656312023878318, 0.9445750230732326, 0.9894009349916499};
    static const double w[8] = {0.1894506104550685, 0.1826034150449236, 0.1691565193950025, 0.1495959888165767,
                                0.1246289712555339, 0.0951585116824928, 0.0622535239386479, 0.0271524594117541};
    const auto meanOver = [&](double u0, double u1) {
        double mean = 0;
        for (int k = 0; k < 8; ++k)
            for (const double sign : {-1.0, 1.0}) {
                const double u = 0.5 * (u0 + u1) + 0.5 * (u1 - u0) * sign * x[k];
                mean += 0.5 * w[k] * std::log(profileAt(ball, knee, zA, zB, height, u));
            }
        return mean;
    };
    for (int i = 0; i < pieces; ++i) {
        const double u0 = double(i) / pieces, u1 = double(i + 1) / pieces;
        if (ball && knee > u0 && knee < u1)
            out << std::exp(((knee - u0) * meanOver(u0, knee) + (u1 - knee) * meanOver(knee, u1)) / (u1 - u0));
        else
            out << std::exp(meanOver(u0, u1));
    }
    return out;
}

/// The staircase of \a levels from \a zA to \a zB along \a span: up from the
/// first pad, each run, the steps between them, down to the second.
QList<QPointF> staircase(const QList<double>& levels, double zA, double zB, double span)
{
    QList<QPointF> out{QPointF(0, zA)};
    const int n = int(levels.size());
    for (int i = 0; i < n; ++i) out << QPointF(span * i / n, levels.at(i)) << QPointF(span * (i + 1) / n, levels.at(i));
    out << QPointF(span, zB);
    return out;
}

} // namespace

QList<double> bondWireLevels(double zA, double zB, double height, int pieces)
{
    return levelsOf(false, 0, zA, zB, height, pieces);
}

QList<double> bondWireLevels(const BondWire& wire, double zA, double zB)
{
    return levelsOf(wire.ball, wire.knee, zA, zB, wire.height, wire.pieces);
}

QList<QPointF> bondWirePath(const BondWire& wire, double zA, double zB, double span)
{
    if (wire.ball) {
        const double top = zA + wire.height;
        return {QPointF(0, zA), QPointF(0, top), QPointF(wire.knee * span, top), QPointF(span, zB)};
    }
    QList<QPointF> out;
    const int n = 64;
    for (int i = 0; i <= n; ++i) {
        const double u = double(i) / n;
        out << QPointF(u * span, profileAt(false, 0, zA, zB, wire.height, u));
    }
    return out;
}

double wireLoopInductance(const QList<QPointF>& path, double radius, bool ground)
{
    // The path's parts, none longer than the radius or a two-hundredth of
    // the whole.
    double whole = 0;
    for (int i = 0; i + 1 < path.size(); ++i) whole += std::hypot(path.at(i + 1).x() - path.at(i).x(), path.at(i + 1).y() - path.at(i).y());
    const double longest = std::max(radius, whole / 200);
    struct Part {
        QPointF a, t;    // its start and its run
    };
    std::vector<Part> parts;
    for (int i = 0; i + 1 < path.size(); ++i) {
        const QPointF d = path.at(i + 1) - path.at(i);
        const double length = std::hypot(d.x(), d.y());
        if (!(length > 0)) continue;
        const int n = std::max(1, int(std::ceil(length / longest)));
        for (int k = 0; k < n; ++k) parts.push_back(Part{path.at(i) + d * (double(k) / n), d / n});
    }
    static const double gx[4] = {0.0694318442029737, 0.3300094782075719, 0.6699905217924281, 0.9305681557970263};
    static const double gw[4] = {0.1739274225687269, 0.3260725774312731, 0.3260725774312731, 0.1739274225687269};
    const double a2 = radius * radius;
    double sum = 0;
    for (const Part& p : parts)
        for (const Part& q : parts) {
            const double dot = p.t.x() * q.t.x() + p.t.y() * q.t.y();
            // (The image's current: its horizontal part reversed, its vertical
            //  part as it is, below the ground.)
            const double image = -p.t.x() * q.t.x() + p.t.y() * q.t.y();
            if (dot == 0 && (!ground || image == 0)) continue;
            double own = 0, mirrored = 0;
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j) {
                    const double px = p.a.x() + gx[i] * p.t.x(), pz = p.a.y() + gx[i] * p.t.y();
                    const double qx = q.a.x() + gx[j] * q.t.x(), qz = q.a.y() + gx[j] * q.t.y();
                    const double dx = px - qx, dz = pz - qz, wz = pz + qz;
                    own += gw[i] * gw[j] / std::sqrt(dx * dx + dz * dz + a2);
                    if (ground) mirrored += gw[i] * gw[j] / std::sqrt(dx * dx + wz * wz);
                }
            sum += dot * own + (ground ? image * mirrored : 0);
        }
    return 1e-7 * sum;
}

QStringList bondWiresNotWritten(const Model& model, const QString& where)
{
    QStringList out;
    for (const BondWire& w : bondWiresOf(model))
        out << tr("The bond wire %1 is not in %2: the solve makes it of runs and legs on layers of its own, which are no layers of "
                  "the board. Draw it there as a wire %3 µm across from (%4, %5) on %6 to (%7, %8) on %9, its loop %10 µm over its pads.")
                   .arg(w.name, where).arg(w.diameter * 1e6, 0, 'g', 4)
                   .arg(w.from.x(), 0, 'g', 6).arg(w.from.y(), 0, 'g', 6).arg(w.fromMetal)
                   .arg(w.to.x(), 0, 'g', 6).arg(w.to.y(), 0, 'g', 6).arg(w.toMetal)
                   .arg(w.height * 1e6, 0, 'g', 4);
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
        QString wire;
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
        if (wire.ball && !(wire.knee >= 0 && wire.knee < 1)) {
            out.problems << tr("%1's loop runs flat over its ball for a share of its span from nought to below one, not %2.")
                                .arg(wire.name).arg(wire.knee);
            return out;
        }
        const double a = wire.diameter / 2;
        const double span = std::hypot(wire.to.x() - wire.from.x(), wire.to.y() - wire.from.y()) * 1e-6;
        QList<double> z = bondWireLevels(wire, zA, zB);
        // Fitted: its runs' heights over the chord scaled, one factor, until
        // the staircase's inductance over the ground is the profile's (the
        // secant on ln L, from the unfitted staircase and one a tenth lower).
        double scale = 1, profileL = 0, unfittedL = 0, fittedL = 0;
        const bool grounded = std::any_of(stack.layers.cbegin(), stack.layers.cend(), [](const Layer& l) {
            return l.kind == Layer::Ground && l.variant != QLatin1String("open");
        });
        if (wire.fit && span > 0) {
            QList<double> chord;
            for (int i = 0; i < z.size(); ++i) chord << zA + (zB - zA) * (i + 0.5) / z.size();
            const auto scaled = [&](double k) {
                QList<double> h;
                for (int i = 0; i < z.size(); ++i) h << chord.at(i) + k * (z.at(i) - chord.at(i));
                return h;
            };
            const auto inductance = [&](double k) { return wireLoopInductance(staircase(scaled(k), zA, zB, span), a, grounded); };
            profileL = wireLoopInductance(bondWirePath(wire, zA, zB, span), a, grounded);
            double k0 = 1, k1 = 0.9, f0 = std::log((unfittedL = inductance(k0)) / profileL), f1 = std::log(inductance(k1) / profileL);
            for (int it = 0; it < 30 && std::abs(f1) > 1e-6 && f1 != f0; ++it) {
                const double k2 = std::clamp(k1 - f1 * (k1 - k0) / (f1 - f0), 0.05, 4.0);
                k0 = k1;
                f0 = f1;
                k1 = k2;
                f1 = std::log(inductance(k1) / profileL);
            }
            if (std::abs(f1) <= 1e-4) {
                scale = k1;
                z = scaled(k1);
                fittedL = profileL * std::exp(f1);
            }
        }
        for (const double h : z)
            if (h <= top * (1 + 1e-12)) {
                out.problems << tr("%1 runs through the stack's dielectrics (a piece of it at %2 µm, their top at %3 µm): its pads "
                                   "are on a buried metal, or its loop is too low.")
                                    .arg(wire.name).arg(h * 1e6, 0, 'g', 4).arg(top * 1e6, 0, 'g', 4);
                return out;
            }
        // (Its strips e^(3/2) a wide - a sheet's geometric mean distance, its
        //  current spread across it, w e^(-3/2), the round wire's a - their
        //  metal w/pi thick at sigma pi^2 a^2/w^2: a round wire's R per length
        //  both at direct current and with the skin thin. Its legs squares
        //  a/0.44705 across, a's geometric mean distance, at sigma pi^2 a^2 /
        //  (4 s^2): the round wire's R with the skin thin.)
        const double width = std::exp(1.5) * a, thickness = width / M_PI, sigma = wire.sigma * M_PI * M_PI * a * a / (width * width);
        const double side = a / 0.44705, legSigma = wire.sigma * M_PI * M_PI * a * a / (4 * side * side);
        QStringList names;
        for (int i = 0; i < z.size(); ++i) {
            // (Pieces as high as each other, one after another, are one run on
            //  one metal, with no leg between: a via of no length is nothing to
            //  solve, and two strips over each other are not one.)
            const bool same = i > 0 && std::abs(z.at(i) - z.at(i - 1)) <= 1e-9 * z.at(i);
            const QString name = same ? names.last() : QStringLiteral("%1_%2").arg(wire.name).arg(i + 1);
            if (!same) levels << Level{z.at(i), name, sigma, thickness, wire.name};
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
            run.outline = stripBetween(p0, p1, width * 1e6, side * 1e6 / 2);
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
            via.sigma = legSigma;
            via.standsFor = wire.name;
            vias << via;
            const double across = side * 1e6;
            Conductor square;
            square.layer = via.name;
            square.outline = QPolygonF(QRectF(at.x() - across / 2, at.y() - across / 2, across, across));
            out.conductors << square;
        };
        leg(wire.fromMetal, names.first(), wire.from, 0);
        for (int i = 0; i + 1 < names.size(); ++i)
            if (names.at(i) != names.at(i + 1))
                leg(names.at(i), names.at(i + 1), wire.from + (wire.to - wire.from) * (double(i + 1) / z.size()), i + 1);
        leg(names.last(), wire.toMetal, wire.to, int(names.size()));
        out.lines << tr("%1: a bond wire %2 µm across from %3 to %4, %5 %6 µm over its pads, made of %7 run(s) at %8 µm and "
                        "their legs.")
                         .arg(wire.name).arg(wire.diameter * 1e6, 0, 'g', 3).arg(wire.fromMetal, wire.toMetal)
                         .arg(wire.ball ? tr("a ball bond's loop") : tr("its arc"))
                         .arg(wire.height * 1e6, 0, 'g', 4).arg(z.size())
                         .arg([&z] {
                             QStringList s;
                             for (const double h : z) s << QString::number(h * 1e6, 'g', 4);
                             return s.join(QStringLiteral(", "));
                         }());
        if (fittedL > 0)
            out.lines << tr("%1's runs fitted: their heights over the chord times %2, so that its staircase's inductance over the "
                            "ground, %3 nH unfitted, is its profile's, %4 nH.")
                             .arg(wire.name).arg(scale, 0, 'f', 3).arg(unfittedL * 1e9, 0, 'g', 4).arg(profileL * 1e9, 0, 'g', 4);
        else if (wire.fit && span > 0)
            out.lines << tr("%1's runs could not be fitted to its profile's inductance: they are at its mean ln heights.").arg(wire.name);
        if (z.size() > 4)
            out.lines << tr("%1 is cut into %2 pieces: each step between its runs adds an error of the mesh's to its inductance, "
                            "some 2 to 5 pH a step at 25 µm cells, and less or more as the cells are; up to four pieces, its "
                            "inductance is its profile's within 3 %.")
                             .arg(wire.name).arg(z.size());
        if (wire.sigma > 0)
            out.lines << tr("%1's legs are the round wire's resistance with the skin thin; at direct current, 4/pi of it, and its "
                            "runs' ends run on over them: its resistance there is a little high.")
                             .arg(wire.name);
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
            air.standsFor = lv.wire;
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
        metal.standsFor = lv.wire;
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
