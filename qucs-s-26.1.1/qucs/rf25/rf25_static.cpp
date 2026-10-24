/*
 * rf25_static.cpp - the quasi-static extraction of an RF 2.5D model
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "rf25_static.h"

#include <QCoreApplication>
#include <QHash>

#include <functional>

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>

namespace qucs_s::rf25 {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::rf25", text);
}

/// How many cell widths away a pair has to be before the cell's charge may
/// be taken as sitting at its centre. Within it the rectangle's own closed
/// form is used instead.
constexpr double kNearWidths = 4.0;

/// The antiderivative of 1/sqrt(x^2 + y^2 + z^2) in x and y: its mixed
/// second derivative is the kernel, so a rectangle's integral is this at
/// its four corners with alternating signs.
double rectangleTerm(double x, double y, double z)
{
    const double r = std::sqrt(x * x + y * y + z * z);
    double out = 0;
    // x asinh(y / sqrt(x^2 + z^2)), which is zero when x is.
    const double dx = std::sqrt(x * x + z * z), dy = std::sqrt(y * y + z * z);
    if (dx > 0) out += x * std::asinh(y / dx);
    if (dy > 0) out += y * std::asinh(x / dy);
    // The out-of-plane term, which vanishes in the plane.
    if (z != 0 && r > 0 && x != 0 && y != 0) out -= z * std::atan(x * y / (z * r));
    return out;
}

} // namespace

double rectanglePotential(const QRectF& rect, const QPointF& point, double z)
{
    // The rectangle's corners, relative to the point.
    const double x1 = rect.left() - point.x(), x2 = rect.right() - point.x();
    const double y1 = rect.top() - point.y(), y2 = rect.bottom() - point.y();
    const double integral = rectangleTerm(x2, y2, z) - rectangleTerm(x1, y2, z) - rectangleTerm(x2, y1, z)
                            + rectangleTerm(x1, y1, z);
    const double area = rect.width() * rect.height();
    return area > 0 ? integral / area : 0;
}

/// Where a metal of \a stack sits once the metals are collapsed to
/// sheets: the top of the dielectrics below it. Negative when the stack
/// has no such metal.
double heightOf(const Stack& stack, const QString& metal)
{
    double z = 0;
    for (const Layer& l : stack.layers) {
        if (l.name == metal || l.tag == metal) return l.kind == Layer::Metal ? z : -1;
        if (l.kind == Layer::Dielectric || l.kind == Layer::Substrate) z += l.thickness;
    }
    return -1;
}

QList<Net> netsOf(const Mesh& mesh, const QList<Conductor>& conductors, const Stack& stack)
{
    // Each conductor its own net to begin with, then the vias join them.
    QList<int> parent;
    for (int i = 0; i < conductors.size(); ++i) parent << i;
    const std::function<int(int)> rootOf = [&parent, &rootOf](int i) {
        return parent.at(i) == i ? i : (parent[i] = rootOf(parent.at(i)));
    };
    const auto join = [&](int a, int b) {
        const int ra = rootOf(a), rb = rootOf(b);
        if (ra != rb) parent[rb] = ra;
    };
    // A via joins what it lands on: the conductors of the two metals it
    // names, where it overlaps them in x and y.
    for (int v = 0; v < conductors.size(); ++v) {
        const Layer* via = stack.named(conductors.at(v).layer);
        if (via == nullptr || via->kind != Layer::Via) continue;
        const QRectF where = conductors.at(v).outline.boundingRect();
        for (const QString& end : {via->from, via->to}) {
            for (int c = 0; c < conductors.size(); ++c) {
                if (conductors.at(c).layer != end) continue;
                if (!conductors.at(c).outline.boundingRect().intersects(where)) continue;
                if (unionOf({conductors.at(c).outline, conductors.at(v).outline}).size() > 1) continue;
                join(v, c);
            }
        }
    }
    // The nets, in the order their first conductor appears.
    QMap<int, int> indexOf;
    QList<Net> nets;
    for (int i = 0; i < conductors.size(); ++i) {
        const int r = rootOf(i);
        if (!indexOf.contains(r)) {
            indexOf.insert(r, nets.size());
            nets << Net();
        }
        Net& n = nets[indexOf.value(r)];
        const Layer* layer = stack.named(conductors.at(i).layer);
        // A via is a wire between two sheets: it joins them and carries no
        // charge of its own.
        if (layer != nullptr && layer->kind == Layer::Via) continue;
        if (!n.layers.contains(conductors.at(i).layer)) n.layers << conductors.at(i).layer;
        n.area += conductors.at(i).area();
    }
    // Their cells: the conductor a cell was cut from decides its net.
    for (int c = 0; c < mesh.cells.size(); ++c) {
        const MeshCell& cell = mesh.cells.at(c);
        const Layer* layer = stack.named(cell.layer);
        if (layer != nullptr && layer->kind == Layer::Via) continue;
        int best = -1;
        for (int i = 0; i < conductors.size() && best < 0; ++i) {
            if (conductors.at(i).layer != cell.layer) continue;
            if (conductors.at(i).outline.containsPoint(cell.centre, Qt::OddEvenFill)) best = i;
        }
        if (best < 0)
            for (int i = 0; i < conductors.size() && best < 0; ++i)
                if (conductors.at(i).layer == cell.layer && conductors.at(i).outline.boundingRect().contains(cell.centre))
                    best = i;
        if (best >= 0) nets[indexOf.value(rootOf(best))].cells << c;
    }
    // Named for what they are made of, and numbered where a layer has
    // more than one.
    QList<Net> kept;
    QHash<QString, int> seen;
    for (Net& n : nets) {
        if (n.cells.isEmpty() || n.layers.isEmpty()) continue;
        const QString of = n.layers.join(QLatin1Char('+'));
        const int which = ++seen[of];
        n.name = which == 1 ? of : QStringLiteral("%1 #%2").arg(of).arg(which);
        kept << n;
    }
    return kept;
}

double Extraction::selfCapacitance(int i) const
{
    if (i < 0 || i >= c.size()) return 0;
    double sum = 0;
    for (int j = 0; j < c.at(i).size(); ++j) sum += c.at(i).at(j);
    return sum;
}

namespace {

/// How many of a Green's images are integrated over a cell rather than
/// taken at its centre. A deep stack runs to thousands, and the ones past
/// the strongest few dozen are so far away that their field hardly
/// changes across a cell.
constexpr int kMostNear = 64;

/// The potential coefficients of \a which (indices into \a cells):
/// P(i, j) is the potential at cell i's centre of a unit charge spread
/// over cell j. \a greenOf gives the Green's function for a point on one
/// layer and a charge on another.
Eigen::MatrixXd coefficients(const QList<MeshCell>& cells, const QList<int>& which,
                             const std::function<const Green&(const QString&, const QString&)>& greenOf, double unit)
{
    const int n = int(which.size());
    Eigen::MatrixXd p(n, n);
    for (int i = 0; i < n; ++i) {
        const MeshCell& ci = cells.at(which.at(i));
        const QPointF at = ci.centre * unit;
        for (int j = 0; j < n; ++j) {
            const MeshCell& cj = cells.at(which.at(j));
            const Green& g = greenOf(ci.layer, cj.layer);
            const QRectF box = cj.outline.boundingRect();
            const QRectF rect(box.left() * unit, box.top() * unit, box.width() * unit, box.height() * unit);
            // The wider of the two cells decides, so that a pair is near
            // or far whichever way round it is asked: the edge refinement
            // leaves cells of two sizes, and judging by the charged one
            // alone made the matrix asymmetric where they met.
            const double side = std::max(std::sqrt(ci.area), std::sqrt(cj.area)) * unit;
            const double distance = std::hypot(at.x() - rect.center().x(), at.y() - rect.center().y());
            if (i == j || distance < kNearWidths * side) {
                // Near, or the cell itself: the terms worth it over the
                // rectangle in closed form, the rest - far enough that
                // their field hardly changes over a cell - at its centre.
                const QList<Green::Image> terms = g.strongestTerms(kMostNear);
                double sum = 0;
                for (const Green::Image& t : terms) sum += t.a * rectanglePotential(rect, at, t.z);
                p(i, j) = sum / (4 * M_PI * g.epsilon()) + g.beyondStrongest(distance, int(terms.size()));
            } else {
                // Far: the charge at the cell's centre, which the Green's
                // function answers from its table in one step.
                p(i, j) = g.potential(distance);
            }
        }
    }
    return p;
}

} // namespace

Extraction extract(const Mesh& mesh, const QList<Conductor>& conductors, const Stack& stack, double micrometre)
{
    Extraction out;
    if (mesh.cells.isEmpty()) {
        out.problems << tr("There are no cells to solve on: mesh the layout first.");
        return out;
    }
    const QList<Net> nets = netsOf(mesh, conductors, stack);
    if (nets.isEmpty()) {
        out.problems << tr("No cell of the mesh belongs to a conductor of the stack.");
        return out;
    }
    // Every metal the cells are on, and where each sits once the metals
    // are collapsed to sheets.
    QStringList layers;
    for (const Net& n : nets)
        for (const QString& l : n.layers)
            if (!layers.contains(l)) layers << l;
    QMap<QString, double> height;
    for (const QString& name : std::as_const(layers)) {
        const Layer* metal = stack.named(name);
        if (metal == nullptr) {
            out.problems << tr("The stack has no layer %1.").arg(name);
            return out;
        }
        if (metal->kind != Layer::Metal) {
            out.problems << tr("%1 is not a metal of the stack: only a metal carries the charge this solves for.")
                                .arg(metal->name);
            return out;
        }
        height.insert(name, heightOf(stack, name));
    }

    const Layers layers0 = layersOf(stack);
    if (!layers0.ok()) {
        out.problems += layers0.problems;
        return out;
    }
    // The same stack with every dielectric vacuum, for C0.
    Layers vacuum = layers0;
    for (Layers::Slab& slab : vacuum.slabs) slab.epsr = 1;
    vacuum.epsrAbove = vacuum.epsrBelow = 1;
    // A Green's function for each pair of metals - the potential at a
    // point on one of a charge on the other - built once and shared by
    // every pair of cells on them. They are symmetric, so a pair is
    // wanted one way round only.
    // One for each unordered pair: the Green's function is symmetric, and
    // building it once both ways round would give two tables over distance
    // whose interpolation differs a little - which would make the
    // capacitance matrix asymmetric for no physical reason.
    const auto keyOf = [](const QString& a, const QString& b) {
        return a <= b ? a + QLatin1Char('|') + b : b + QLatin1Char('|') + a;
    };
    QMap<QString, Green> greens, bareGreens;
    for (const QString& a : std::as_const(layers))
        for (const QString& b : std::as_const(layers)) {
            const QString key = keyOf(a, b);
            if (greens.contains(key)) continue;
            greens.insert(key, Green(layers0, height.value(a), height.value(b)));
            bareGreens.insert(key, Green(vacuum, height.value(a), height.value(b)));
            if (!greens.value(key).ok()) {
                out.problems += greens.value(key).problems();
                return out;
            }
        }
    // (By reference to the maps themselves: a helper that took one as a
    // parameter and captured that would hand out a reference to its own
    // argument, which is gone by the time the matrix is filled.)
    const auto fromStack = [&greens, &keyOf](const QString& a, const QString& b) -> const Green& {
        const auto it = greens.constFind(keyOf(a, b));
        return it != greens.constEnd() ? it.value() : greens.constBegin().value();
    };
    const auto fromVacuum = [&bareGreens, &keyOf](const QString& a, const QString& b) -> const Green& {
        const auto it = bareGreens.constFind(keyOf(a, b));
        return it != bareGreens.constEnd() ? it.value() : bareGreens.constBegin().value();
    };

    QList<int> which;
    for (const Net& n : nets) which += n.cells;
    const int n = int(which.size());
    out.unknowns = n;
    if (n > 4000) {
        out.problems << tr("%1 cells is more than this build will solve a dense matrix for (4000 at most): mesh more coarsely.")
                            .arg(n);
        return out;
    }

    // Which net each unknown belongs to, in the order they were gathered.
    QList<int> netOf;
    netOf.reserve(n);
    for (int i = 0; i < nets.size(); ++i)
        for (int k = 0; k < nets.at(i).cells.size(); ++k) netOf << i;

    QList<double> crowding;
    const auto solve = [&](const std::function<const Green&(const QString&, const QString&)>& greenOf,
                           QList<QList<double>>* matrix, QList<double>* crowd = nullptr) {
        const Eigen::MatrixXd p = coefficients(mesh.cells, which, greenOf, micrometre);
        const Eigen::PartialPivLU<Eigen::MatrixXd> lu(p);
        matrix->clear();
        if (crowd != nullptr) crowd->clear();
        for (int net = 0; net < nets.size(); ++net) {
            Eigen::VectorXd v = Eigen::VectorXd::Zero(n);
            for (int i = 0; i < n; ++i) v(i) = netOf.at(i) == net ? 1.0 : 0.0;
            const Eigen::VectorXd q = lu.solve(v);
            QList<double> column;
            for (int other = 0; other < nets.size(); ++other) {
                double sum = 0;
                for (int i = 0; i < n; ++i)
                    if (netOf.at(i) == other) sum += q(i);
                column << sum;
            }
            matrix->append(column);
            if (crowd != nullptr) {
                // How the charge crowds on the net held at a volt: what
                // its resistance per metre needs beyond Rs.
                double square = 0, total = 0;
                for (int i = 0; i < n; ++i) {
                    if (netOf.at(i) != net) continue;
                    const double area = mesh.cells.at(which.at(i)).area * micrometre * micrometre;
                    if (area > 0) square += q(i) * q(i) / area;
                    total += q(i);
                }
                crowd->append(total != 0 ? square / (total * total) : 0.0);
            }
        }
        return p.diagonal().maxCoeff() / std::max(1e-300, p.diagonal().minCoeff());
    };

    out.conditionHint = solve(fromStack, &out.maxwell, &crowding);
    QList<QList<double>> bareMaxwell;
    solve(fromVacuum, &bareMaxwell);
    out.crowding = crowding;

    // Each dielectric's share of the field, for the loss it adds. The
    // energy a layer holds is eps dC/deps, so G = w sum(tand_k eps_k
    // dC/deps_k) - and dC/deps is taken by solving again with that layer
    // a little different, which is a solve per lossy layer and none at
    // all for a stack that says no loss.
    QList<double> loss;
    for (int i = 0; i < nets.size(); ++i) loss << 0.0;
    bool lossy = false;
    for (const Layer& l : stack.layers)
        if ((l.kind == Layer::Dielectric || l.kind == Layer::Substrate) && l.tand > 0) lossy = true;
    if (lossy) {
        for (int k = 0; k < layers0.slabs.size(); ++k) {
            // The model's own layer behind this slab, for its tan delta.
            double tand = 0;
            for (const Layer& l : stack.layers)
                if ((l.kind == Layer::Dielectric || l.kind == Layer::Substrate) && l.name == layers0.slabs.at(k).name)
                    tand = l.tand;
            if (!(tand > 0)) continue;
            Layers nudged = layers0;
            const double was = nudged.slabs[k].epsr;
            const double step = was * 1e-3;
            nudged.slabs[k].epsr = was + step;
            QMap<QString, Green> some;
            bool built = true;
            for (const QString& a : std::as_const(layers))
                for (const QString& b : std::as_const(layers)) {
                    const QString key = keyOf(a, b);
                    if (some.contains(key)) continue;
                    some.insert(key, Green(nudged, height.value(a), height.value(b)));
                    built = built && some.value(key).ok();
                }
            if (!built) continue;
            const auto fromNudged = [&some, &keyOf](const QString& a, const QString& b) -> const Green& {
                const auto it = some.constFind(keyOf(a, b));
                return it != some.constEnd() ? it.value() : some.constBegin().value();
            };
            QList<QList<double>> nudgedMaxwell;
            solve(fromNudged, &nudgedMaxwell);
            for (int i = 0; i < nets.size(); ++i) {
                double here = 0, there = 0;
                for (int j = 0; j < nets.size(); ++j) {
                    here += out.maxwell.at(i).at(j);
                    there += nudgedMaxwell.at(i).at(j);
                }
                const double dCdEps = (there - here) / step;
                if (here != 0) loss[i] += tand * was * dCdEps / here;
            }
        }
    }
    out.lossTangent = loss;
    // The metal each net is on: its conductivity and thickness, for the
    // surface resistance at a frequency.
    for (const Net& net : nets) {
        const Layer* metal = net.layers.isEmpty() ? nullptr : stack.named(net.layers.first());
        out.sigma << (metal != nullptr ? metal->sigma : 0.0);
        out.thickness << (metal != nullptr ? metal->thickness : 0.0);
    }

    // The short-circuit (Maxwell) matrix as the mutual one a circuit uses:
    // the diagonal is what a conductor has to everything else, and an
    // off-diagonal term is the capacitance between two of them, which is
    // the negative of the Maxwell term.
    const auto mutualOf = [&nets](const QList<QList<double>>& m) {
        QList<QList<double>> out;
        for (int i = 0; i < nets.size(); ++i) {
            QList<double> row;
            for (int j = 0; j < nets.size(); ++j) {
                double sum = 0;
                for (int k = 0; k < nets.size(); ++k) sum += m.at(i).at(k);
                row << (i == j ? sum : -m.at(i).at(j));
            }
            out << row;
        }
        return out;
    };
    out.c = mutualOf(out.maxwell);
    out.c0 = mutualOf(bareMaxwell);
    for (const Net& net : nets) out.names << net.name;

    out.lines << tr("%1 cell(s) on %2 net(s) over %3, as %4 unknowns.")
                     .arg(mesh.cells.size()).arg(nets.size()).arg(layers.join(QStringLiteral(", "))).arg(n);
    for (const QString& name : std::as_const(layers))
        out.lines << tr("%1 sits %2 µm up the stack.").arg(name).arg(height.value(name) * 1e6, 0, 'g', 4);
    const Green& first = greens[keyOf(layers.first(), layers.first())];
    out.lines << tr("%1 image(s) of the stack; %2 of the reflection coarsened into one of them, %3 of it left out.")
                     .arg(first.images().size()).arg(first.coarsened(), 0, 'g', 2).arg(first.truncated(), 0, 'g', 2);
    for (int i = 0; i < out.names.size(); ++i)
        out.lines << tr("%1: %2 F to everything else.").arg(out.names.at(i)).arg(out.selfCapacitance(i), 0, 'g', 4);
    return out;
}

std::complex<double> surfaceImpedance(double sigma, double t, double frequency)
{
    if (!(sigma > 0) || !(t > 0) || !(frequency > 0)) return {0, 0};
    constexpr double mu0 = 4e-7 * M_PI;
    const double omega = 2 * M_PI * frequency;
    const double depth = std::sqrt(2.0 / (omega * mu0 * sigma));
    const std::complex<double> k{1.0 / depth, 1.0 / depth};   // (1 + j)/delta
    const std::complex<double> kt = k * t;
    // coth, written so that a thick metal (where cosh and sinh both run
    // away) comes out as the 1 it tends to rather than as a ratio of two
    // infinities.
    std::complex<double> coth;
    if (std::abs(kt.real()) > 30) coth = 1.0;
    else coth = std::cosh(kt) / std::sinh(kt);
    return k / sigma * coth;
}

LineParameters lineParameters(const Extraction& extraction, int index, double length, double frequency)
{
    LineParameters out;
    out.length = length;
    if (!extraction.ok()) {
        out.problems = extraction.problems.isEmpty() ? QStringList{tr("Nothing was extracted.")} : extraction.problems;
        return out;
    }
    if (index < 0 || index >= extraction.c.size()) {
        out.problems << tr("There is no conductor %1 in the extraction.").arg(index);
        return out;
    }
    if (!(length > 0)) {
        out.problems << tr("A line's parameters are per metre: say how long the piece that was solved is.");
        return out;
    }
    out.c = extraction.selfCapacitance(index) / length;
    double bare = 0;
    for (int j = 0; j < extraction.c0.at(index).size(); ++j) bare += extraction.c0.at(index).at(j);
    out.c0 = bare / length;
    if (!(out.c > 0) || !(out.c0 > 0)) {
        out.problems << tr("Its capacitance came out as %1 F/m and %2 F/m in vacuum: neither can be nothing or less.")
                            .arg(out.c, 0, 'g', 3).arg(out.c0, 0, 'g', 3);
        return out;
    }
    // The relations the multiphysics engine's lineparams uses, so that a
    // structure solved both ways gives the same numbers.
    out.epsilonEff = out.c / out.c0;
    out.z0 = 1.0 / (kLightSpeed * std::sqrt(out.c * out.c0));
    out.l = 1.0 / (kLightSpeed * kLightSpeed * out.c0);
    out.nEff = std::sqrt(out.epsilonEff);
    out.velocity = kLightSpeed / out.nEff;
    if (!(frequency > 0)) return out;

    // The rest of RLGC, which needs a frequency: the metal's loss and the
    // dielectrics'.
    out.frequency = frequency;
    const double omega = 2 * M_PI * frequency;
    const double sigma = extraction.sigma.value(index), t = extraction.thickness.value(index);
    const std::complex<double> zs = surfaceImpedance(sigma, t, frequency);
    out.surfaceResistance = zs.real();
    if (sigma > 0) {
        constexpr double mu0 = 4e-7 * M_PI;
        out.skinDepth = std::sqrt(2.0 / (omega * mu0 * sigma));
    }
    // Rs times the length times how the charge crowds: for a line of
    // width W whose current runs evenly that is Rs/W, and where it
    // crowds at the edges it is more.
    out.r = out.surfaceResistance * length * extraction.crowding.value(index);
    // And the dielectrics', by the share of the field each holds.
    out.g = omega * out.c * extraction.lossTangent.value(index);
    if (!(out.r >= 0) || !(out.g >= 0)) {
        out.problems << tr("Its resistance came out as %1 Ohm/m and its conductance as %2 S/m: neither can be less than "
                           "nothing.").arg(out.r, 0, 'g', 3).arg(out.g, 0, 'g', 3);
        return out;
    }
    // What a line of that R, L, G and C does: its characteristic
    // impedance, its propagation and the loss along it.
    const std::complex<double> series{out.r, omega * out.l};
    const std::complex<double> shunt{out.g, omega * out.c};
    if (std::abs(shunt) > 0) out.zc = std::sqrt(series / shunt);
    out.gamma = std::sqrt(series * shunt);
    // Nepers to decibels, and the root taken on the side that loses
    // rather than gains.
    if (out.gamma.real() < 0) out.gamma = -out.gamma;
    out.alphaDb = out.gamma.real() * 20.0 / std::log(10.0);
    return out;
}

} // namespace qucs_s::rf25
