/*
 * rf25_inductance.cpp - the partial inductances of a layout, and the
 *                       inductance of the loop a current makes of them
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "rf25_inductance.h"

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

constexpr double kMu0 = 4e-7 * M_PI;
/// Beyond this many runs and cells a dense solve is not set up.
constexpr int kMostRuns = 3000;
/// Two runs further apart than this many of their widths have their
/// transverse distance taken between their middles; nearer than it, it is
/// averaged across both widths.
constexpr double kNearWidths = 6.0;

/// Four-point Gauss-Legendre on [-1/2, 1/2]: the nodes and the weights,
/// which sum to one.
constexpr double kGaussAt[4] = {-0.43056815579702629, -0.16999052179242813, 0.16999052179242813, 0.43056815579702629};
constexpr double kGaussWeight[4] = {0.17392742256872693, 0.32607257743127307, 0.32607257743127307, 0.17392742256872693};

/// s asinh(s/rho) - sqrt(s^2 + rho^2): the second antiderivative of
/// 1/sqrt(s^2 + rho^2), and an even function of s.
double antiderivative(double s, double rho)
{
    return s * std::asinh(s / rho) - std::sqrt(s * s + rho * rho);
}

/*!
 * The double line integral of 1/r over two parallel runs, m: one from 0
 * to \a la along the axis, the other from \a d to \a d + \a lb, their
 * axes \a rho apart.
 *
 * Four terms of the antiderivative, which is exact.
 */
double parallelPair(double la, double lb, double d, double rho)
{
    return antiderivative(d + lb, rho) - antiderivative(d + lb - la, rho) - antiderivative(d, rho) + antiderivative(d - la, rho);
}

struct Vector {
    double x = 0, y = 0, z = 0;
    double dot(const Vector& o) const { return x * o.x + y * o.y + z * o.z; }
    double norm() const { return std::sqrt(dot(*this)); }
    Vector operator-(const Vector& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vector operator+(const Vector& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vector operator*(double s) const { return {x * s, y * s, z * s}; }
};

Vector directionOf(const Filament& f)
{
    Vector d{f.to.x() - f.from.x(), f.to.y() - f.from.y(), f.z1 - f.z0};
    const double n = d.norm();
    return n > 0 ? d * (1.0 / n) : Vector{0, 0, 0};
}

Vector startOf(const Filament& f) { return {f.from.x(), f.from.y(), f.z0}; }

/*!
 * The direction a run's width lies along: perpendicular to the run and,
 * for a run in the plane, in the plane.
 *
 * A via is a run in z whose cross-section is two-dimensional; one
 * direction of it is taken, which is the same approximation the thin
 * metal of the rest of the solver makes and is small beside a via's own
 * short length.
 */
Vector widthOf(const Filament& f)
{
    const Vector d = directionOf(f);
    if (std::abs(d.z) > 0.5) return {1, 0, 0};
    return {-d.y, d.x, 0};
}

} // namespace

double Filament::length() const
{
    const double dx = to.x() - from.x(), dy = to.y() - from.y(), dz = z1 - z0;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void Filament::direction(double* x, double* y, double* z) const
{
    const Vector d = directionOf(*this);
    if (x != nullptr) *x = d.x;
    if (y != nullptr) *y = d.y;
    if (z != nullptr) *z = d.z;
}

void Filament::middle(double* x, double* y, double* z) const
{
    if (x != nullptr) *x = 0.5 * (from.x() + to.x());
    if (y != nullptr) *y = 0.5 * (from.y() + to.y());
    if (z != nullptr) *z = 0.5 * (z0 + z1);
}

double Filament::resistance() const
{
    const double area = width * thickness;
    if (!(sigma > 0) || !(area > 0)) return 0;
    return length() / (sigma * area);
}

double selfDistance(double width)
{
    if (!(width > 0)) return 0;
    return width * std::exp(-1.5);
}

double partialInductance(const Filament& a, const Filament& b)
{
    const double la = a.length(), lb = b.length();
    if (!(la > 0) || !(lb > 0)) return 0;
    const Vector da = directionOf(a), db = directionOf(b);
    const double along = da.dot(db);
    // Orthogonal runs put no flux through one another, which is what
    // makes an axis-aligned layout cheap: only x with x and y with y.
    if (std::abs(along) < 1e-9) return 0;
    const double sign = along > 0 ? 1.0 : -1.0;

    // Both runs measured along a's own direction: a spans [0, la], and b
    // spans [d, d + lb] whichever way round it is drawn.
    const Vector origin = startOf(a);
    const Vector bFrom = startOf(b) - origin;
    const Vector bTo = (startOf(b) + db * lb) - origin;
    const double sFrom = bFrom.dot(da), sTo = bTo.dot(da);
    const double d = std::min(sFrom, sTo);

    // What is left of the offset once the along-the-axis part is taken
    // out: the distance between the two axes.
    const Vector offset = bFrom - da * sFrom;
    const double apart = offset.norm();

    const bool same = &a == &b;
    const double widest = std::max(a.width, b.width);
    // The same run, or two runs on the same axis - consecutive pieces of
    // one strip - have no distance between their axes, so the distance
    // that stands in for it is the cross-section's own geometric mean.
    // It is what makes a strip cut into pieces add back up to the whole.
    if (same || apart <= 1e-9 * std::max(widest, la)) {
        const double rho = selfDistance(widest);
        if (!(rho > 0)) return 0;
        return sign * kMu0 / (4 * M_PI) * parallelPair(la, lb, d, rho);
    }
    // Far enough apart that the widths do not matter: between the axes.
    if (apart > kNearWidths * widest || !(a.width > 0) || !(b.width > 0))
        return sign * kMu0 / (4 * M_PI) * parallelPair(la, lb, d, apart);

    // Near: the distance averaged across both widths, which is where a
    // neighbour's own breadth is most of the answer.
    const Vector wa = widthOf(a), wb = widthOf(b);
    double sum = 0;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            const Vector here = offset + wa * (kGaussAt[i] * a.width) - wb * (kGaussAt[j] * b.width);
            // (Perpendicular to the axis already, both widths being so.)
            const double rho = std::max(here.norm(), 1e-12 * widest);
            sum += kGaussWeight[i] * kGaussWeight[j] * parallelPair(la, lb, d, rho);
        }
    return sign * kMu0 / (4 * M_PI) * sum;
}

QList<QList<double>> partialInductances(const QList<Filament>& runs)
{
    QList<QList<double>> out;
    const int n = int(runs.size());
    for (int i = 0; i < n; ++i) out << QList<double>(n, 0.0);
    for (int i = 0; i < n; ++i) {
        out[i][i] = partialInductance(runs.at(i), runs.at(i));
        for (int j = i + 1; j < n; ++j) {
            const double m = partialInductance(runs.at(i), runs.at(j));
            out[i][j] = m;
            out[j][i] = m;
        }
    }
    return out;
}

double loopInductance(const QList<Filament>& loop)
{
    double sum = 0;
    for (int i = 0; i < loop.size(); ++i)
        for (int j = 0; j < loop.size(); ++j) sum += partialInductance(loop.at(i), loop.at(j));
    return sum;
}

namespace {

/// A key two cells sharing an edge agree on, whichever way round they
/// have it: the midpoint, rounded to a nanometre of the layout's units.
QString edgeKey(const QPointF& a, const QPointF& b)
{
    const QPointF middle = (a + b) / 2;
    return QStringLiteral("%1,%2").arg(std::llround(middle.x() * 1e3)).arg(std::llround(middle.y() * 1e3));
}

/// How high metal \a metal sits in \a stack, m; -1 where it is no metal
/// of it. (The same walk the charge extraction makes.)
double heightOf(const Stack& stack, const QString& metal)
{
    double z = 0;
    for (const Layer& l : stack.layers) {
        if (l.name == metal || l.tag == metal) return l.isConductor() ? z : -1;
        if (l.kind == Layer::Dielectric || l.kind == Layer::Substrate) z += l.thickness;
    }
    return -1;
}

} // namespace

QList<Filament> runsOf(const Mesh& mesh, const QList<Conductor>& conductors, const Stack& stack, double micrometre,
                       QList<QPair<int, int>>* ends)
{
    Q_UNUSED(conductors);
    QList<Filament> runs;
    if (ends != nullptr) ends->clear();

    // The pairs of cells that share an edge: where current flows from one
    // to the other, which is what the mesh has been counting all along.
    QHash<QString, QList<int>> shared;
    for (int c = 0; c < mesh.cells.size(); ++c) {
        const MeshCell& cell = mesh.cells.at(c);
        for (int i = 0, n = int(cell.outline.size()); i < n; ++i)
            shared[cell.layer + QLatin1Char('|') + edgeKey(cell.outline.at(i), cell.outline.at((i + 1) % n))].append(c);
    }
    QMap<QString, double> height;
    for (const QString& layer : mesh.byLayer().keys()) height.insert(layer, heightOf(stack, layer));

    for (auto it = shared.constBegin(); it != shared.constEnd(); ++it) {
        if (it.value().size() != 2) continue;
        const int a = it.value().at(0), b = it.value().at(1);
        const MeshCell& ca = mesh.cells.at(a);
        const MeshCell& cb = mesh.cells.at(b);
        const double z = height.value(ca.layer, -1);
        if (!(z >= 0)) continue;
        // Its width is the edge the two cells share; its length is from
        // one centre to the other.
        double edge = 0;
        for (int i = 0, n = int(ca.outline.size()); i < n; ++i) {
            const QPointF p = ca.outline.at(i), q = ca.outline.at((i + 1) % n);
            if (edgeKey(p, q) != it.key().section(QLatin1Char('|'), 1)) continue;
            edge = std::hypot(q.x() - p.x(), q.y() - p.y());
            break;
        }
        if (!(edge > 0)) continue;
        const Layer* metal = stack.named(ca.layer);
        Filament f;
        f.from = ca.centre * micrometre;
        f.to = cb.centre * micrometre;
        f.z0 = f.z1 = z;
        f.width = edge * micrometre;
        f.thickness = metal != nullptr ? metal->thickness : 0.0;
        f.sigma = metal != nullptr ? metal->sigma : 0.0;
        if (!(f.length() > 0)) continue;
        runs << f;
        if (ends != nullptr) ends->append({a, b});
    }

    // A via joins a cell of one metal to a cell of another: a run in z,
    // which puts no flux through anything in the plane.
    for (int v = 0; v < mesh.cells.size(); ++v) {
        const MeshCell& cell = mesh.cells.at(v);
        const Layer* via = stack.named(cell.layer);
        if (via == nullptr || via->kind != Layer::Via) continue;
        const double za = heightOf(stack, via->from), zb = heightOf(stack, via->to);
        if (!(za >= 0) || !(zb >= 0) || za == zb) continue;
        const auto nearest = [&mesh, &cell](const QString& layer) {
            int best = -1;
            double closest = 0;
            for (int c = 0; c < mesh.cells.size(); ++c) {
                if (mesh.cells.at(c).layer != layer) continue;
                const QPointF d = mesh.cells.at(c).centre - cell.centre;
                const double far = d.x() * d.x() + d.y() * d.y();
                if (best < 0 || far < closest) {
                    best = c;
                    closest = far;
                }
            }
            return best;
        };
        const int a = nearest(via->from), b = nearest(via->to);
        if (a < 0 || b < 0) continue;
        Filament f;
        f.from = f.to = cell.centre * micrometre;
        f.z0 = std::min(za, zb);
        f.z1 = std::max(za, zb);
        // A via's cross-section is what it covers; one side of it stands
        // for its breadth.
        f.width = std::sqrt(std::max(0.0, cell.area)) * micrometre;
        f.thickness = f.width;
        f.sigma = via->sigma;
        if (!(f.length() > 0) || !(f.width > 0)) continue;
        runs << f;
        if (ends != nullptr) ends->append({za < zb ? a : b, za < zb ? b : a});
    }
    return runs;
}

Inductance inductanceOf(const Mesh& mesh, const QList<Conductor>& conductors, const Stack& stack, QPointF from, QPointF to,
                        double micrometre, double frequency)
{
    Inductance out;
    if (mesh.cells.isEmpty()) {
        out.problems << tr("There are no cells to find an inductance on: mesh the layout first.");
        return out;
    }
    QList<QPair<int, int>> ends;
    out.runs = runsOf(mesh, conductors, stack, micrometre, &ends);
    out.branches = int(out.runs.size());
    if (out.runs.isEmpty()) {
        out.problems << tr("No two cells of the mesh share an edge, so there is nowhere for a current to flow. Mesh the "
                           "layout more finely.");
        return out;
    }

    // The cells the runs join, numbered as they are met.
    QMap<int, int> nodeOf;
    for (const QPair<int, int>& e : std::as_const(ends)) {
        if (!nodeOf.contains(e.first)) nodeOf.insert(e.first, nodeOf.size());
        if (!nodeOf.contains(e.second)) nodeOf.insert(e.second, nodeOf.size());
    }
    out.nodes = int(nodeOf.size());
    if (out.branches + out.nodes > kMostRuns) {
        out.problems << tr("%1 runs of current over %2 cells is more than this build will set up a dense matrix for "
                           "(%3 in all). Mesh the layout more coarsely: fewer cells across a conductor (the Mesh "
                           "node's, ten by default), or a larger largest cell.")
                            .arg(out.branches).arg(out.nodes).arg(kMostRuns);
        return out;
    }

    // Where the current goes in and comes out: the cells nearest the two
    // points asked for, of those a run reaches.
    const auto nearest = [&mesh, &nodeOf](QPointF where) {
        int best = -1;
        double closest = 0;
        for (auto it = nodeOf.constBegin(); it != nodeOf.constEnd(); ++it) {
            const QPointF d = mesh.cells.at(it.key()).centre - where;
            const double far = d.x() * d.x() + d.y() * d.y();
            if (best < 0 || far < closest) {
                best = it.key();
                closest = far;
            }
        }
        return best;
    };
    /*
     * A port drives the whole face of the conductor it is on, not the one
     * cell nearest it. Which face is not a guess: the point lies on an
     * edge of a processed shape, and the cells that have a side on that
     * same edge are the face.
     *
     * It matters more than it sounds. Fed at a point, a current must
     * spread out from it, and that spreading resistance is real but is
     * not the bar's - a bar two cells wide came out a sixth too
     * resistive, and the answer moved as the mesh was refined, which is
     * the one thing a solver must not do.
     */
    const auto faceAt = [&mesh, &conductors, &nodeOf, &nearest](QPointF where, QPointF* onLine = nullptr,
                                                                 QPointF* alongLine = nullptr) {
        QList<int> cells;
        if (onLine != nullptr) *onLine = where;
        if (alongLine != nullptr) *alongLine = QPointF(0, 0);
        // The outline edge the point is on, over every shape.
        double closest = 0;
        QPointF p, q;
        QString layer;
        for (const Conductor& c : conductors)
            for (int i = 0, n = int(c.outline.size()); i < n; ++i) {
                const QPointF a = c.outline.at(i), b = c.outline.at((i + 1) % n);
                const QPointF d = b - a;
                const double len2 = d.x() * d.x() + d.y() * d.y();
                if (!(len2 > 0)) continue;
                double t = ((where.x() - a.x()) * d.x() + (where.y() - a.y()) * d.y()) / len2;
                t = std::clamp(t, 0.0, 1.0);
                const QPointF on = a + d * t;
                const double far = std::hypot(where.x() - on.x(), where.y() - on.y());
                if (layer.isEmpty() || far < closest) {
                    closest = far;
                    p = a;
                    q = b;
                    layer = c.layer;
                }
            }
        if (layer.isEmpty()) return cells;
        const QPointF along = q - p;
        const double len = std::hypot(along.x(), along.y());
        if (!(len > 0)) return cells;
        const QPointF unit = along / len;
        const QPointF normal(-unit.y(), unit.x());
        if (onLine != nullptr) *onLine = p;
        if (alongLine != nullptr) *alongLine = unit;
        // A cell is on that face where one of its own sides lies along
        // the same line and within the edge's own extent.
        const double tol = 1e-6 * std::max(1.0, len);
        for (auto it = nodeOf.constBegin(); it != nodeOf.constEnd(); ++it) {
            const MeshCell& cell = mesh.cells.at(it.key());
            if (cell.layer != layer) continue;
            for (int i = 0, n = int(cell.outline.size()); i < n; ++i) {
                const QPointF a = cell.outline.at(i), b = cell.outline.at((i + 1) % n);
                const double offA = (a.x() - p.x()) * normal.x() + (a.y() - p.y()) * normal.y();
                const double offB = (b.x() - p.x()) * normal.x() + (b.y() - p.y()) * normal.y();
                if (std::abs(offA) > tol || std::abs(offB) > tol) continue;
                const double tA = (a.x() - p.x()) * unit.x() + (a.y() - p.y()) * unit.y();
                const double tB = (b.x() - p.x()) * unit.x() + (b.y() - p.y()) * unit.y();
                if (std::max(tA, tB) < -tol || std::min(tA, tB) > len + tol) continue;
                cells << it.key();
                break;
            }
        }
        // A point that is on no face of anything - inside a shape, or off
        // the layout - drives the one cell nearest it.
        if (cells.isEmpty()) {
            const int one = nearest(where);
            if (one >= 0) cells << one;
        }
        return cells;
    };
    QPointF sourceOn, sourceAlong, sinkOn, sinkAlong;
    const QList<int> sourceFace = faceAt(from, &sourceOn, &sourceAlong);
    const QList<int> sinkFace = faceAt(to, &sinkOn, &sinkAlong);
    const int source = sourceFace.isEmpty() ? -1 : sourceFace.first();
    const int sink = sinkFace.isEmpty() ? -1 : sinkFace.first();
    // (Kept for the stubs below: how wide the cell is across the way the
    //  current comes into it.)
    const auto acrossOf = [&mesh](int cell, QPointF along) {
        const QRectF box = mesh.cells.at(cell).outline.boundingRect();
        const double n = std::hypot(along.x(), along.y());
        if (!(n > 0)) return std::min(box.width(), box.height());
        // The extent across the direction: the box's own side that the
        // direction does not run along.
        return std::abs(along.x()) > std::abs(along.y()) ? box.height() : box.width();
    };
    if (source < 0 || sink < 0) {
        out.problems << tr("Neither end of the loop landed on a cell that a current can reach.");
        return out;
    }
    // The two faces must be different ones: a current that goes in and
    // comes out at the same place makes no loop.
    //
    // Different, not disjoint. Two faces that meet at a corner share the
    // cell in that corner - a current going in at the left face and out
    // at the bottom one turns through a right angle, which is a loop like
    // any other - so it is the faces that are compared, not their cells.
    {
        QList<int> a = sourceFace, b = sinkFace;
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        if (a == b) {
            out.problems << tr("Both ends of the loop are on the same face of the layout, at the cell at %1, %2 µm: "
                               "put them apart, or mesh the layout more finely.")
                                .arg(mesh.cells.at(a.first()).centre.x(), 0, 'g', 4)
                                .arg(mesh.cells.at(a.first()).centre.y(), 0, 'g', 4);
            return out;
        }
    }

    // The two stubs: from where the current goes in to the first cell's
    // centre, and from the last cell's centre to where it comes out.
    // Without them the solve is of a path a half cell short at each end,
    // which on a bar of ten cells is a tenth of its resistance.
    const auto stub = [&](int cell, QPointF outside, QPointF on, QPointF unit, bool inwards) {
        const QPointF centre = mesh.cells.at(cell).centre;
        // Where this cell's own centre meets the face: a face of several
        // cells is then driven across its breadth, each cell fed straight
        // in, rather than every cell fed from one place on it.
        QPointF at = outside;
        if (unit != QPointF(0, 0)) {
            const double t = (centre.x() - on.x()) * unit.x() + (centre.y() - on.y()) * unit.y();
            at = on + unit * t;
        }
        const QPointF along = centre - at;
        Filament f;
        f.from = (inwards ? at : centre) * micrometre;
        f.to = (inwards ? centre : at) * micrometre;
        f.z0 = f.z1 = heightOf(stack, mesh.cells.at(cell).layer);
        f.width = acrossOf(cell, along) * micrometre;
        const Layer* m = stack.named(mesh.cells.at(cell).layer);
        f.thickness = m != nullptr ? m->thickness : 0.0;
        f.sigma = m != nullptr ? m->sigma : 0.0;
        return f;
    };
    const int portIn = out.nodes, portOut = out.nodes + 1;
    int stubs = 0;
    for (const int cell : sourceFace) {
        const Filament a = stub(cell, from, sourceOn, sourceAlong, true);
        if (!(a.length() > 0) || !(a.width > 0)) continue;
        out.runs << a;
        ends.append({portIn, cell});
        ++stubs;
    }
    for (const int cell : sinkFace) {
        const Filament b = stub(cell, to, sinkOn, sinkAlong, false);
        if (!(b.length() > 0) || !(b.width > 0)) continue;
        out.runs << b;
        ends.append({cell, portOut});
        ++stubs;
    }
    out.branches = int(out.runs.size());
    // The ports are nodes of their own where they have a stub; where they
    // have none the current goes straight in at the cell.
    QMap<int, int> portNode;
    for (const QPair<int, int>& e : std::as_const(ends)) {
        for (const int which : {e.first, e.second})
            if ((which == portIn || which == portOut) && !nodeOf.contains(which)) nodeOf.insert(which, nodeOf.size());
    }
    Q_UNUSED(portNode);
    out.nodes = int(nodeOf.size());
    const int inAt = nodeOf.contains(portIn) ? portIn : source;
    const int outAt = nodeOf.contains(portOut) ? portOut : sink;
    out.lines << tr("The current goes in across %1 cell(s) of one face and out across %2 of another, by %3 stub(s), so "
                    "that the path is the whole of it and the face is driven across its breadth.")
                     .arg(sourceFace.size()).arg(sinkFace.size()).arg(stubs);

    const int nb = out.branches, nn = out.nodes;
    const QList<QList<double>> lp = partialInductances(out.runs);

    // Of every current that obeys Kirchhoff's law and carries the
    // terminal current, the one that flows is the one of least magnetic
    // energy. Minimising 1/2 I'LI subject to AI = s is a saddle-point
    // system in the currents and the nodes' potentials:
    //
    //     [ L   A' ] [ I ]   [ 0 ]
    //     [ A   0  ] [ u ] = [ s ]
    //
    // One node's row is dropped, since the injections sum to nothing and
    // the potentials are only fixed up to a constant.
    const int drop = nodeOf.value(outAt);
    const int rows = nb + nn - 1;
    Eigen::MatrixXd m = Eigen::MatrixXd::Zero(rows, rows);
    Eigen::VectorXd rhs = Eigen::VectorXd::Zero(rows);
    for (int i = 0; i < nb; ++i)
        for (int j = 0; j < nb; ++j) m(i, j) = lp.at(i).at(j);
    const auto row = [drop, nb](int node) { return node < drop ? nb + node : nb + node - 1; };
    for (int b = 0; b < nb; ++b) {
        const int a = nodeOf.value(ends.at(b).first), c = nodeOf.value(ends.at(b).second);
        if (a != drop) {
            m(row(a), b) += 1;
            m(b, row(a)) += 1;
        }
        if (c != drop) {
            m(row(c), b) -= 1;
            m(b, row(c)) -= 1;
        }
    }
    const int in = nodeOf.value(inAt);
    if (in == drop) {
        out.problems << tr("The current would go in and out at the same node.");
        return out;
    }
    rhs(row(in)) = 1.0;   // one amp in, and out at the node that was dropped

    const Eigen::FullPivLU<Eigen::MatrixXd> lu(m);
    if (!lu.isInvertible()) {
        out.problems << tr("The runs of current do not make a path from one end of the loop to the other: the layout is "
                           "in pieces that nothing joins.");
        return out;
    }
    const Eigen::VectorXd answer = lu.solve(rhs);

    // The energy the current carries is the inductance.
    double energy = 0, loss = 0;
    out.current.clear();
    for (int i = 0; i < nb; ++i) {
        out.current << answer(i);
        loss += out.runs.at(i).resistance() * answer(i) * answer(i);
        for (int j = 0; j < nb; ++j) energy += lp.at(i).at(j) * answer(i) * answer(j);
    }
    if (!(energy > 0) || !std::isfinite(energy)) {
        out.problems << tr("The loop's inductance came out as %1 H, which is none.").arg(energy, 0, 'g', 3);
        return out;
    }
    out.l = energy;        // (one amp, so the energy is the inductance)

    // At direct current the resistance divides the current, not the
    // inductance: the network's nodal solve, G u = s with G = A R^-1 A',
    // and the current R^-1 A' u. Its loss is the resistance at direct
    // current and its energy the inductance there. (The least-energy
    // current's loss, given as that until the phase B check of 24
    // October, counted its crowding as resistance: 29 % high on a bar ten
    // cells across.) Where a run says no resistance there is no such
    // current, and the least-energy one's loss is all there is.
    bool resistive = nb > 0;
    for (const Filament& f : std::as_const(out.runs))
        if (!(f.resistance() > 0)) resistive = false;
    out.lDc = energy;
    out.rDc = loss;
    if (resistive) {
        const int m = nn - 1;
        const auto at = [drop](int node) { return node < drop ? node : node - 1; };
        Eigen::MatrixXd g = Eigen::MatrixXd::Zero(m, m);
        for (int b = 0; b < nb; ++b) {
            const double y = 1.0 / out.runs.at(b).resistance();
            const int a = nodeOf.value(ends.at(b).first), c = nodeOf.value(ends.at(b).second);
            if (a != drop) g(at(a), at(a)) += y;
            if (c != drop) g(at(c), at(c)) += y;
            if (a != drop && c != drop) {
                g(at(a), at(c)) -= y;
                g(at(c), at(a)) -= y;
            }
        }
        Eigen::VectorXd s = Eigen::VectorXd::Zero(m);
        s(at(in)) = 1.0;
        const Eigen::LDLT<Eigen::MatrixXd> ldlt(g);
        const Eigen::VectorXd u = ldlt.solve(s);
        if (ldlt.info() == Eigen::Success && u.allFinite()) {
            QList<double> dc;
            for (int b = 0; b < nb; ++b) {
                const int a = nodeOf.value(ends.at(b).first), c = nodeOf.value(ends.at(b).second);
                const double ua = a == drop ? 0.0 : u(at(a)), uc = c == drop ? 0.0 : u(at(c));
                dc << (ua - uc) / out.runs.at(b).resistance();
            }
            double dcLoss = 0, dcEnergy = 0;
            for (int i = 0; i < nb; ++i) {
                dcLoss += out.runs.at(i).resistance() * dc.at(i) * dc.at(i);
                for (int j = 0; j < nb; ++j) dcEnergy += lp.at(i).at(j) * dc.at(i) * dc.at(j);
            }
            if (dcLoss > 0 && dcEnergy > 0) {
                out.rDc = dcLoss;
                out.lDc = dcEnergy;
            }
        }
    }
    out.frequency = frequency;
    if (frequency > 0 && out.rDc > 0) out.q = 2 * M_PI * frequency * energy / out.rDc;

    double longest = 0;
    for (const Filament& f : std::as_const(out.runs)) longest = std::max(longest, f.length());
    out.lines << tr("%1 run(s) of current over %2 cell(s); the longest is %3 µm.")
                     .arg(nb).arg(nn).arg(longest * 1e6, 0, 'g', 3);
    out.lines << tr("In at the cell at %1, %2 µm and out at %3, %4 µm.")
                     .arg(mesh.cells.at(source).centre.x(), 0, 'g', 4)
                     .arg(mesh.cells.at(source).centre.y(), 0, 'g', 4)
                     .arg(mesh.cells.at(sink).centre.x(), 0, 'g', 4)
                     .arg(mesh.cells.at(sink).centre.y(), 0, 'g', 4);
    out.lines << tr("The loop is %1 nH where its inductance outweighs its resistance, %2 nH and %3 Ohm at direct "
                    "current.")
                     .arg(out.l * 1e9, 0, 'g', 4).arg(out.lDc * 1e9, 0, 'g', 4).arg(out.rDc, 0, 'g', 4);
    if (out.q > 0) out.lines << tr("Q is %1 at %2 GHz, of that inductance over the resistance at direct current.")
                                    .arg(out.q, 0, 'g', 3).arg(frequency / 1e9, 0, 'g', 3);
    return out;
}

} // namespace qucs_s::rf25
