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

QList<Net> netsOf(const Mesh& mesh, const QList<Conductor>& conductors)
{
    // Which conductor each cell was cut from: the one on its layer that
    // holds its centre. The processing has already joined what touches, so
    // a conductor is a net.
    QList<Net> nets;
    QHash<int, int> ofConductor;   // conductor index to net index
    for (int i = 0; i < conductors.size(); ++i) {
        Net n;
        n.layer = conductors.at(i).layer;
        n.area = conductors.at(i).area();
        int sameLayer = 0;
        for (int j = 0; j < i; ++j)
            if (conductors.at(j).layer == n.layer) ++sameLayer;
        n.name = sameLayer == 0 ? n.layer : QStringLiteral("%1 #%2").arg(n.layer).arg(sameLayer + 1);
        ofConductor.insert(i, nets.size());
        nets << n;
    }
    for (int c = 0; c < mesh.cells.size(); ++c) {
        const MeshCell& cell = mesh.cells.at(c);
        int best = -1;
        for (int i = 0; i < conductors.size(); ++i) {
            if (conductors.at(i).layer != cell.layer) continue;
            if (conductors.at(i).outline.containsPoint(cell.centre, Qt::OddEvenFill)) {
                best = i;
                break;
            }
        }
        if (best < 0)
            for (int i = 0; i < conductors.size() && best < 0; ++i)
                if (conductors.at(i).layer == cell.layer && conductors.at(i).outline.boundingRect().contains(cell.centre)) best = i;
        if (best >= 0) nets[ofConductor.value(best)].cells << c;
    }
    // A conductor the mesh gave no cell is no net at all.
    QList<Net> kept;
    for (const Net& n : std::as_const(nets))
        if (!n.cells.isEmpty()) kept << n;
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

/// The potential coefficients of \a cells in \a green: P(i,j) is the
/// potential at cell i's centre from a unit charge spread over cell j.
Eigen::MatrixXd coefficients(const QList<MeshCell>& cells, const QList<int>& which, const Green& green, double unit)
{
    const int n = int(which.size());
    Eigen::MatrixXd p(n, n);
    const double fourPiEps = 4 * M_PI * green.epsilon();
    // The images, with the charge itself as the first of them: every term
    // is a charge a/(4 pi eps sqrt(rho^2 + z^2)), and a rectangle's
    // integral of each has a closed form.
    QList<Green::Image> terms{Green::Image{1.0, 0.0}};
    terms += green.images();
    for (int i = 0; i < n; ++i) {
        const MeshCell& ci = cells.at(which.at(i));
        const QPointF at = ci.centre * unit;
        for (int j = 0; j < n; ++j) {
            const MeshCell& cj = cells.at(which.at(j));
            const QRectF box = cj.outline.boundingRect();
            const QRectF rect(box.left() * unit, box.top() * unit, box.width() * unit, box.height() * unit);
            const double side = std::sqrt(cj.area) * unit;
            const double distance = std::hypot(at.x() - rect.center().x(), at.y() - rect.center().y());
            if (i == j || distance < kNearWidths * side) {
                // Near, or the cell itself: every term over the rectangle,
                // in closed form.
                double sum = 0;
                for (const Green::Image& t : std::as_const(terms)) sum += t.a * rectanglePotential(rect, at, t.z);
                p(i, j) = sum / fourPiEps;
            } else {
                // Far: the charge at the cell's centre, which the Green's
                // function answers in one call.
                p(i, j) = green.potential(distance);
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
    const QList<Net> nets = netsOf(mesh, conductors);
    if (nets.isEmpty()) {
        out.problems << tr("No cell of the mesh belongs to a conductor.");
        return out;
    }
    // One metal layer: the Green's function has no term yet for a charge
    // on one layer and a point on another, so a model that needs one is
    // refused rather than answered wrongly.
    QStringList layers;
    for (const Net& n : nets)
        if (!layers.contains(n.layer)) layers << n.layer;
    if (layers.size() > 1) {
        out.problems << tr("The cells are on %1 layers (%2). This build solves one metal layer at a time: the potential of a "
                           "charge on one layer at a point on another is the next round's work.")
                            .arg(layers.size()).arg(layers.join(QStringLiteral(", ")));
        return out;
    }
    const Layer* metal = stack.named(layers.first());
    if (metal == nullptr) {
        out.problems << tr("The stack has no layer %1.").arg(layers.first());
        return out;
    }
    if (metal->kind != Layer::Metal) {
        out.problems << tr("%1 is not a metal of the stack: only a metal carries the charge this solves for.").arg(metal->name);
        return out;
    }

    const Layers layers0 = layersOf(stack);
    if (!layers0.ok()) {
        out.problems += layers0.problems;
        return out;
    }
    // Where the charge sits: the metal rests on the dielectric below it,
    // and the metals take no room in the slabs, so its height is the top
    // of the dielectrics under it.
    double z = 0;
    for (const Layer& l : stack.layers) {
        if (l.tag == metal->tag) break;
        if (l.kind == Layer::Dielectric || l.kind == Layer::Substrate) z += l.thickness;
    }
    const Green green(layers0, z);
    if (!green.ok()) {
        out.problems += green.problems();
        return out;
    }
    // A stack that reflects nearly everything has an image series that
    // does not die away, and the expansion is then not the whole Green's
    // function. Better to say so than to answer with a capacitance that is
    // quietly short of the truth.
    constexpr double kMostLost = 1e-3;
    if (green.truncated() > kMostLost) {
        out.problems << tr("This stack reflects too strongly for its images to be enumerated: %1 of the reflection was left "
                           "out, and a capacitance from them would be short of the truth. It is the permittivities' own "
                           "ratios that do it.").arg(green.truncated(), 0, 'g', 3);
        return out;
    }
    // And the same stack with every dielectric vacuum, for C0.
    Layers vacuum = layers0;
    for (Layers::Slab& s : vacuum.slabs) s.epsr = 1;
    vacuum.epsrAbove = vacuum.epsrBelow = 1;
    const Green bare(vacuum, z);
    if (!bare.ok()) {
        out.problems += bare.problems();
        return out;
    }

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

    const auto solve = [&](const Green& g, QList<QList<double>>* matrix) {
        const Eigen::MatrixXd p = coefficients(mesh.cells, which, g, micrometre);
        const Eigen::PartialPivLU<Eigen::MatrixXd> lu(p);
        matrix->clear();
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
        }
        return p.diagonal().maxCoeff() / std::max(1e-300, p.diagonal().minCoeff());
    };

    out.conditionHint = solve(green, &out.maxwell);
    QList<QList<double>> bareMaxwell;
    solve(bare, &bareMaxwell);

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

    out.lines << tr("%1 cell(s) on %2 conductor(s) of %3, as %4 unknowns.")
                     .arg(mesh.cells.size()).arg(nets.size()).arg(metal->name).arg(n);
    out.lines << tr("The charge sits %1 µm up the stack, in %2.")
                     .arg(z * 1e6, 0, 'g', 4)
                     .arg(layers0.slabs.at(std::max(0, layers0.slabAt(z))).name);
    out.lines << tr("%1 image(s) of the stack; %2 of the reflection was left out of them.")
                     .arg(green.images().size()).arg(green.truncated(), 0, 'g', 2);
    for (int i = 0; i < out.names.size(); ++i)
        out.lines << tr("%1: %2 F to everything else.").arg(out.names.at(i)).arg(out.selfCapacitance(i), 0, 'g', 4);
    return out;
}

LineParameters lineParameters(const Extraction& extraction, int index, double length)
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
    return out;
}

} // namespace qucs_s::rf25
