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
// The potential at \a at (m) of a unit charge spread over \a rect (m):
// near, or the cell itself, the terms worth it over the rectangle in
// closed form, the rest - far enough that their field hardly changes over
// a cell - at its centre; far, the charge at the cell's centre, which the
// Green's function answers from its table in one step. \a side is the
// wider of the two cells, so that a pair is near or far whichever way
// round it is asked: the edge refinement leaves cells of two sizes, and
// judging by the charged one alone made the matrix asymmetric where they
// met.
double cellPotential(const QPointF& at, const QRectF& rect, const Green& g, double side, bool self)
{
    const double distance = std::hypot(at.x() - rect.center().x(), at.y() - rect.center().y());
    if (self || distance < kNearWidths * side) {
        const QList<Green::Image> terms = g.strongestTerms(kMostNear);
        double sum = 0;
        for (const Green::Image& t : terms) sum += t.a * rectanglePotential(rect, at, t.z);
        return sum / (4 * M_PI * g.epsilon()) + g.beyondStrongest(distance, int(terms.size()));
    }
    return g.potential(distance);
}

QRectF scaled(const QRectF& box, double unit)
{
    return QRectF(box.left() * unit, box.top() * unit, box.width() * unit, box.height() * unit);
}

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
            const double side = std::max(std::sqrt(ci.area), std::sqrt(cj.area)) * unit;
            p(i, j) = cellPotential(at, scaled(cj.outline.boundingRect(), unit), greenOf(ci.layer, cj.layer), side, i == j);
        }
    }
    return p;
}

// The antiderivative in u of ln sqrt(u^2 + d^2): a straight segment's even
// charge, in 2D, integrated along it.
double lnAntiderivative(double u, double d)
{
    const double r2 = u * u + d * d;
    double v = r2 > 0 ? 0.5 * u * std::log(r2) - u : -u;
    if (d != 0) v += d * std::atan(u / d);
    return v;
}

/// A face of a conductor's cross-section in 2D, m: from a to b along it,
/// horizontal at height z = at, or vertical at x = at.
struct Face2D {
    int net = 0;
    bool horizontal = true;
    double a = 0, b = 0, at = 0;
};

/// A conductor's cross-section: a rectangle, its faces' grid across (the
/// mesh's own lines, m) and its bottom and top, m.
struct Box2D {
    int net = 0;
    QList<double> across;
    double z0 = 0, z1 = 0;
    int up = 2;   ///< cells up each wall
};

// The potential at (x, z) of a unit charge per metre spread evenly over a
// face, over a perfect ground at z = 0 - its image at -z.
double facePotential(double x, double z, const Face2D& f)
{
    const auto integral = [&f, x, z](bool image) {
        if (f.horizontal) {
            const double d = z - (image ? -f.at : f.at);
            return lnAntiderivative(f.b - x, d) - lnAntiderivative(f.a - x, d);
        }
        const double d = x - f.at;
        const double a = image ? -f.b : f.a, b = image ? -f.a : f.b;
        return lnAntiderivative(b - z, d) - lnAntiderivative(a - z, d);
    };
    return (integral(true) - integral(false)) / (2 * M_PI * kEps0 * (f.b - f.a));
}

// L per metre of the conductors \a boxes over a perfect ground in vacuum,
// C0^-1/c^2, by a 2D method of moments in closed form: every face of each
// rectangle - its bottom, its top and its two walls - in cells, an even
// charge on each, matched at its middle. \a recede is a net whose every
// face moves \a dn into its metal, or -2 for the ground moving \a dn away
// below everything, or -1 for neither.
Eigen::MatrixXd inductance2D(const QList<Box2D>& boxes, int nets, int recede, double dn)
{
    QList<Face2D> faces;
    for (const Box2D& box : boxes) {
        QList<double> across = box.across;
        double z0 = box.z0, z1 = box.z1;
        if (recede == box.net) {
            const double x0 = across.first(), x1 = across.last();
            for (double& x : across) x = x0 + dn + (x - x0) * (x1 - x0 - 2 * dn) / (x1 - x0);
            z0 += dn;
            z1 -= dn;
        } else if (recede == -2) {
            z0 += dn;
            z1 += dn;
        }
        // The walls' grid as the faces': the outermost half as tall.
        QList<double> zs{z0};
        const double step = (z1 - z0) / box.up;
        zs << z0 + step / 2;
        for (int i = 1; i < box.up; ++i) zs << z0 + i * step;
        zs << z1 - step / 2 << z1;
        for (int i = 0; i + 1 < across.size(); ++i) {
            faces << Face2D{box.net, true, across.at(i), across.at(i + 1), z0};
            faces << Face2D{box.net, true, across.at(i), across.at(i + 1), z1};
        }
        for (int i = 0; i + 1 < zs.size(); ++i) {
            faces << Face2D{box.net, false, zs.at(i), zs.at(i + 1), across.first()};
            faces << Face2D{box.net, false, zs.at(i), zs.at(i + 1), across.last()};
        }
    }
    const int k = int(faces.size());
    Eigen::MatrixXd p(k, k);
    for (int i = 0; i < k; ++i) {
        const Face2D& f = faces.at(i);
        const double middle = (f.a + f.b) / 2;
        const double x = f.horizontal ? middle : f.at, z = f.horizontal ? f.at : middle;
        for (int j = 0; j < k; ++j) p(i, j) = facePotential(x, z, faces.at(j));
    }
    const Eigen::PartialPivLU<Eigen::MatrixXd> lu(p);
    Eigen::MatrixXd c = Eigen::MatrixXd::Zero(nets, nets);
    for (int e = 0; e < nets; ++e) {
        Eigen::VectorXd v = Eigen::VectorXd::Zero(k);
        for (int i = 0; i < k; ++i) v(i) = faces.at(i).net == e ? 1.0 : 0.0;
        const Eigen::VectorXd q = lu.solve(v);
        for (int i = 0; i < k; ++i) c(faces.at(i).net, e) += q(i);
    }
    return c.inverse() / (kLightSpeed * kLightSpeed);
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
    if (n > kMostCharges) {
        out.problems << tr("%1 cells is more than this build will solve a dense matrix for (%2 at most): ask for fewer cells "
                           "across a conductor, or a larger largest cell.")
                            .arg(n).arg(kMostCharges);
        return out;
    }

    // Which net each unknown belongs to, in the order they were gathered.
    QList<int> netOf;
    netOf.reserve(n);
    for (int i = 0; i < nets.size(); ++i)
        for (int k = 0; k < nets.at(i).cells.size(); ++k) netOf << i;

    // The line's per-metre values are of its middle cross-section, as if
    // the line ran on for ever: each cell there a strip the line's whole
    // length, tiled by copies of itself end to end, and the charge per
    // metre on those strips solved for. Its two open ends' fringing is then
    // not in it at all. Dividing the whole charge by the length put both
    // ends' fringing into every value "per metre" - a 50 Ohm line came out
    // 7 % low, a coupled pair's even mode 9 % (the phase B check of 24
    // October). Nor is the middle of the line enough, nor the difference of
    // the line and its half: in vacuum what the ends do falls off as a power
    // of the distance, not at once, and both left the C0 of a 1000 µm line
    // over 100 µm a per cent high.
    QRectF all;
    for (const int k : std::as_const(which)) all = all.united(mesh.cells.at(k).outline.boundingRect());
    const bool alongX = all.width() >= all.height();
    const double lineFrom = alongX ? all.left() : all.top();
    const double lineLength = alongX ? all.width() : all.height();   // (the layout's units)
    const double middle = lineFrom + lineLength / 2;
    const double eps = 1e-6 * std::max(1.0, lineLength);
    const auto alongOf = [alongX](const QRectF& b) { return alongX ? std::pair(b.left(), b.right()) : std::pair(b.top(), b.bottom()); };
    const auto acrossOf = [alongX](const QRectF& b) { return alongX ? std::pair(b.top(), b.bottom()) : std::pair(b.left(), b.right()); };
    // Each net's cells at a place along the line, across it: which layer
    // and from where to where.
    const auto sectionAt = [&](double at) {
        QList<QList<std::tuple<QString, double, double>>> section(nets.size());
        for (int k = 0; k < n; ++k) {
            const QRectF b = mesh.cells.at(which.at(k)).outline.boundingRect();
            const auto [s0, s1] = alongOf(b);
            if (!(s0 <= at && at < s1)) continue;
            const auto [t0, t1] = acrossOf(b);
            section[netOf.at(k)] << std::make_tuple(mesh.cells.at(which.at(k)).layer, t0, t1);
        }
        for (auto& cells : section) std::sort(cells.begin(), cells.end());
        return section;
    };
    // A uniform line: every net there and the same across a quarter, half
    // and three quarters of the way along, of rectangles, and nowhere wider
    // than its middle - an L's other arm is at none of the three. A pad, a
    // stub, a taper or a bend is no line, and its per-metre values are the
    // whole over its length, said as such.
    QList<int> column;   // (into `which')
    bool uniform = lineLength > 0 && !nets.isEmpty();
    if (uniform) {
        const auto middleSection = sectionAt(middle);
        for (const double at : {lineFrom + lineLength / 4, lineFrom + 3 * lineLength / 4}) {
            const auto there = sectionAt(at);
            for (int net = 0; net < nets.size() && uniform; ++net) {
                const auto& a = middleSection.at(net);
                const auto& b = there.at(net);
                if (a.isEmpty() || a.size() != b.size()) uniform = false;
                for (int i = 0; i < a.size() && uniform; ++i)
                    if (std::get<0>(a.at(i)) != std::get<0>(b.at(i)) || std::abs(std::get<1>(a.at(i)) - std::get<1>(b.at(i))) > eps
                        || std::abs(std::get<2>(a.at(i)) - std::get<2>(b.at(i))) > eps)
                        uniform = false;
            }
        }
        // (Each net's own breadth, of all its cells, against its middle's.)
        for (int net = 0; net < nets.size() && uniform; ++net) {
            double lo = 0, hi = 0, midLo = 0, midHi = 0;
            bool first = true;
            for (int k = 0; k < n; ++k) {
                if (netOf.at(k) != net) continue;
                const auto [t0, t1] = acrossOf(mesh.cells.at(which.at(k)).outline.boundingRect());
                lo = first ? t0 : std::min(lo, t0);
                hi = first ? t1 : std::max(hi, t1);
                first = false;
            }
            const auto& mid = middleSection.at(net);
            for (int i = 0; i < mid.size(); ++i) {
                midLo = i == 0 ? std::get<1>(mid.at(i)) : std::min(midLo, std::get<1>(mid.at(i)));
                midHi = i == 0 ? std::get<2>(mid.at(i)) : std::max(midHi, std::get<2>(mid.at(i)));
            }
            if (first || mid.isEmpty() || std::abs(lo - midLo) > eps || std::abs(hi - midHi) > eps) uniform = false;
        }
        for (int k = 0; k < n && uniform; ++k) {
            const MeshCell& cell = mesh.cells.at(which.at(k));
            const QRectF b = cell.outline.boundingRect();
            const auto [s0, s1] = alongOf(b);
            if (!(s0 <= middle && middle < s1)) continue;
            if (std::abs(b.width() * b.height() - cell.area) > 1e-6 * std::max(1e-30, cell.area)) uniform = false;
            column << k;
        }
    }
    // How far along the copies are summed: a hundred times the larger of
    // the stack's height and the cross-section's span. Over a ground what a
    // strip does falls as the cube of the distance, so what is left beyond
    // is a part in ten thousand; with no ground it falls as the distance
    // itself, and a line's capacitance per metre to infinity has no value -
    // it falls as the log of the length - so its values are of that much
    // line, which is said.
    double reach = 0;   // m
    if (uniform) {
        double lo = 0, hi = 0, top = 0;
        for (int i = 0; i < column.size(); ++i) {
            const MeshCell& cell = mesh.cells.at(which.at(column.at(i)));
            const auto [t0, t1] = acrossOf(cell.outline.boundingRect());
            lo = i == 0 ? t0 : std::min(lo, t0);
            hi = i == 0 ? t1 : std::max(hi, t1);
            top = std::max(top, height.value(cell.layer));
        }
        reach = 100 * std::max((hi - lo) * micrometre, top);
    }
    // The cross-section's own Maxwell matrix per metre, and how each net's
    // charge crowds across it: R per metre is the surface resistance times
    // the sum of each strip's charge squared over its width, over the
    // whole charge squared - the current running where the charge sits.
    const auto crossSection = [&](const std::function<const Green&(const QString&, const QString&)>& greenOf,
                                  QList<QList<double>>* matrix, QList<double>* crowd) {
        const int m = int(column.size());
        Eigen::MatrixXd p(m, m);
        for (int a = 0; a < m; ++a) {
            const MeshCell& ca = mesh.cells.at(which.at(column.at(a)));
            const QPointF at = ca.centre * micrometre;
            for (int b = 0; b < m; ++b) {
                const MeshCell& cb = mesh.cells.at(which.at(column.at(b)));
                const Green& g = greenOf(ca.layer, cb.layer);
                const QRectF rect = scaled(cb.outline.boundingRect(), micrometre);
                const double delta = alongX ? rect.width() : rect.height();
                const double side = std::max(std::sqrt(ca.area), std::sqrt(cb.area)) * micrometre;
                const int copies = int(std::ceil(reach / delta));
                double sum = 0;
                for (int k = -copies; k <= copies; ++k) {
                    const QRectF copy = rect.translated(alongX ? k * delta : 0.0, alongX ? 0.0 : k * delta);
                    sum += cellPotential(at, copy, g, side, a == b && k == 0);
                }
                // (A charge per metre on the strip puts delta of it on each copy.)
                p(a, b) = sum * delta;
            }
        }
        const Eigen::PartialPivLU<Eigen::MatrixXd> lu(p);
        matrix->clear();
        if (crowd != nullptr) crowd->clear();
        for (int net = 0; net < nets.size(); ++net) {
            Eigen::VectorXd v = Eigen::VectorXd::Zero(m);
            for (int a = 0; a < m; ++a) v(a) = netOf.at(column.at(a)) == net ? 1.0 : 0.0;
            const Eigen::VectorXd q = lu.solve(v);   // C/m on each strip
            QList<double> row(nets.size(), 0.0);
            for (int a = 0; a < m; ++a) row[netOf.at(column.at(a))] += q(a);
            matrix->append(row);
            if (crowd != nullptr) {
                double square = 0, total = 0;
                for (int a = 0; a < m; ++a) {
                    if (netOf.at(column.at(a)) != net) continue;
                    const auto [t0, t1] = acrossOf(mesh.cells.at(which.at(column.at(a))).outline.boundingRect());
                    const double width = (t1 - t0) * micrometre;
                    if (width > 0) square += q(a) * q(a) / width;
                    total += q(a);
                }
                crowd->append(total != 0 ? square / (total * total) : 0.0);
            }
        }
    };

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
    solve(fromVacuum, &out.maxwell0);
    out.crowding = crowding;
    out.grounded = layers0.groundBelow;
    if (uniform) {
        crossSection(fromStack, &out.perMetre.maxwell, &out.perMetre.crowding);
        crossSection(fromVacuum, &out.perMetre.maxwell0, nullptr);
    }
    // The ground's kind, for its share of the loss.
    out.groundKind = QStringLiteral("open");
    for (const Layer& l : stack.layers)
        if (l.kind == Layer::Ground) {
            out.groundKind = l.variant.isEmpty() ? QStringLiteral("perfect") : l.variant;
            out.groundValue = l.sigma;   // (the impedance's ohms, or the metal's conductivity)
        }

    // R by Wheeler's incremental-inductance rule, R = (Rs/mu0) dL/dn, on
    // the cross-section's own rectangles: every face of a conductor - its
    // bottom, its top, its walls - receding into it, and the ground
    // receding below them all. The crowding of a zero-thickness sheet's
    // charge diverges at its edges, so R from it grew without end as the
    // mesh was refined (the phase B check's F4: +10.5 % from 5 µm cells to
    // 2.5); the thickness is what stops that, and L - which is all the rule
    // asks of a geometry - is the vacuum's whatever the dielectrics, so a
    // 2D solve of the rectangles in closed form over the ground's image is
    // the whole of it. (Measured in a separate solve of the whole
    // perimeter, Wheeler's rule on Hammerstad and Jensen's thick air line
    // is within 0.4 to 1.2 % of it; two sheets without their walls were 4
    // to 7 % low.)
    if (uniform && out.grounded) {
        QMap<QPair<int, QString>, QList<std::pair<double, double>>> spans;
        for (const int a : std::as_const(column)) {
            const MeshCell& cell = mesh.cells.at(which.at(a));
            spans[{netOf.at(a), cell.layer}] << acrossOf(cell.outline.boundingRect());
        }
        QList<Box2D> boxes;
        QList<double> conductance(nets.size(), 0.0);   // S of a metre of each net at direct current
        QStringList thin;
        double finest = 0, thinnest = 0;
        for (auto it = spans.begin(); it != spans.end(); ++it) {
            const Layer* metal = stack.named(it.key().second);
            const double t = metal != nullptr ? metal->thickness : 0.0;
            if (!(t > 0)) {
                if (!thin.contains(it.key().second)) thin << it.key().second;
                continue;
            }
            QList<std::pair<double, double>> cells = it.value();
            std::sort(cells.begin(), cells.end());
            const auto close = [&](Box2D box) {
                if (box.across.size() < 2) return;
                // The corners are where the field crowds: the outermost
                // cells halved until they are no wider than half the
                // metal's thickness, which the 2D solve can afford and the
                // mesh's own cells across need not be.
                while (box.across.at(1) - box.across.at(0) > t / 2)
                    box.across.insert(1, (box.across.at(0) + box.across.at(1)) / 2);
                while (box.across.at(box.across.size() - 1) - box.across.at(box.across.size() - 2) > t / 2)
                    box.across.insert(box.across.size() - 1,
                                      (box.across.at(box.across.size() - 2) + box.across.last()) / 2);
                box.net = it.key().first;
                box.z0 = height.value(it.key().second);
                box.z1 = box.z0 + t;
                for (int i = 0; i + 1 < box.across.size(); ++i) {
                    const double w = box.across.at(i + 1) - box.across.at(i);
                    if (finest <= 0 || w < finest) finest = w;
                }
                conductance[box.net] += metal->sigma * (box.across.last() - box.across.first()) * t;
                boxes << box;
            };
            Box2D box;
            for (const auto& [t0, t1] : cells) {
                // (Across in the layout's units, the box's lines in metres.)
                if (!box.across.isEmpty() && std::abs(t0 - box.across.last() / micrometre) > eps) {
                    close(box);
                    box = Box2D();
                }
                if (box.across.isEmpty()) box.across << t0 * micrometre;
                box.across << t1 * micrometre;
            }
            close(box);
            if (thinnest <= 0 || t < thinnest) thinnest = t;
        }
        // Up each wall as finely as the finest cell across: the walls' grid
        // as the faces'.
        for (Box2D& box : boxes) box.up = std::clamp(int(std::ceil((box.z1 - box.z0) / std::max(1e-30, finest))), 2, 16);
        if (thin.isEmpty() && !boxes.isEmpty()) {
            const int count = int(nets.size());
            const double dn = 1e-4 * std::min(thinnest, finest);
            constexpr double mu0 = 4e-7 * M_PI;
            const auto factor = [&](int recede) {
                const Eigen::MatrixXd d =
                    (inductance2D(boxes, count, recede, dn) - inductance2D(boxes, count, recede, -dn)) / (2 * dn * mu0);
                QList<QList<double>> m;
                for (int i = 0; i < count; ++i) {
                    QList<double> row;
                    for (int j = 0; j < count; ++j) row << 0.5 * (d(i, j) + d(j, i));
                    m << row;
                }
                return m;
            };
            for (int k = 0; k < count; ++k) out.perMetre.wheeler << factor(k);
            out.perMetre.wheelerGround = factor(-2);
            for (int i = 0; i < count; ++i) out.perMetre.dcResistance << (conductance.at(i) > 0 ? 1.0 / conductance.at(i) : 0.0);
            QStringList each;
            for (int i = 0; i < count; ++i)
                each << tr("%1's faces %2/m").arg(nets.at(i).name).arg(out.perMetre.wheeler.at(i).at(i).at(i), 0, 'g', 4);
            out.lines << tr("R per metre by Wheeler's rule on the cross-section's own rectangles, the metal's thickness and "
                            "walls in them: every face of a conductor receding into it, and the ground below them all, "
                            "in a 2D solve of their L. Per ohm of surface resistance, %1, and the ground %2/m.")
                             .arg(each.join(QStringLiteral(", ")))
                             .arg(out.perMetre.wheelerGround.at(0).at(0), 0, 'g', 4);
        } else if (!thin.isEmpty()) {
            out.lines << tr("%1 has no thickness: R is from how the charge crowds on a sheet of none, which grows without "
                            "end as the mesh is refined. Give the metal its thickness, and R is Wheeler's.")
                             .arg(thin.join(QStringLiteral(", ")));
        }
    } else if (uniform) {
        out.lines << tr("With no ground, R is from how the charge crowds on a sheet, which grows as the mesh is refined: "
                        "Wheeler's rule wants the ground the current returns through.");
    }

    // Each dielectric's share of the field, for the loss it adds. The
    // energy a layer holds is eps dC/deps, so G = w sum(tand_k eps_k
    // dC/deps_k) - and dC/deps is taken by solving again with that layer
    // a little different, which is a solve per lossy layer and none at
    // all for a stack that says no loss.
    out.dielectricLoss.clear();
    for (int i = 0; i < nets.size(); ++i) out.dielectricLoss << QList<double>(nets.size(), 0.0);
    out.perMetre.dielectricLoss.clear();
    if (uniform)
        for (int i = 0; i < nets.size(); ++i) out.perMetre.dielectricLoss << QList<double>(nets.size(), 0.0);
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
            QList<QList<double>> nudgedMaxwell, nudgedPerMetre;
            solve(fromNudged, &nudgedMaxwell);
            // (And of the cross-section, for G per metre.)
            if (uniform) crossSection(fromNudged, &nudgedPerMetre, nullptr);
            // The whole matrix's derivative, not one number of it: a
            // layer's loss couples two conductors as well as loading one,
            // and coupled lines need that term.
            for (int i = 0; i < nets.size(); ++i)
                for (int j = 0; j < nets.size(); ++j) {
                    const double dCdEps = (nudgedMaxwell.at(i).at(j) - out.maxwell.at(i).at(j)) / step;
                    out.dielectricLoss[i][j] += tand * was * dCdEps;
                    // (And per metre, of the cross-section.)
                    if (uniform && i < nudgedPerMetre.size() && j < nudgedPerMetre.at(i).size())
                        out.perMetre.dielectricLoss[i][j] +=
                            tand * was * (nudgedPerMetre.at(i).at(j) - out.perMetre.maxwell.at(i).at(j)) / step;
                }
        }
    }
    // What was done for the per-metre values, said.
    {
        Extraction::PerMetre& pm = out.perMetre;
        pm.fromMiddle = uniform && pm.maxwell.size() == nets.size() && pm.maxwell0.size() == nets.size();
        pm.length = lineLength * micrometre;
        pm.reach = pm.fromMiddle ? reach : 0;
        if (pm.fromMiddle) {
            pm.said = tr("Per metre of the line's middle cross-section, as if it ran on for ever: its two open ends' "
                         "fringing is not in it.");
            if (!out.grounded && nets.size() == 1)
                pm.said = tr("One conductor and no ground: no line, so no values per metre.");
            else if (!out.grounded)
                pm.said += tr(" With no ground, a line's capacitance to infinity has no value per metre - it falls as "
                              "the log of the length - so these are of %1 mm of it; between its conductors they hold.")
                               .arg(2 * reach * 1e3, 0, 'g', 3);
        } else {
            pm.maxwell.clear();
            pm.maxwell0.clear();
            pm.dielectricLoss.clear();
            pm.crowding.clear();
            pm.said = tr("Not a uniform line - a net does not run its whole length - so its per-metre values are the "
                         "whole over its length, the ends' fringing in them.");
        }
        out.lines << pm.said;
    }
    // One net's own loss tangent, for a single line: its row of the loss
    // matrix over its row of the capacitance - of the line's middle where
    // that was taken, which is what its C per metre is of too.
    {
        const bool middle = out.perMetre.fromMiddle;
        const QList<QList<double>>& cap = middle ? out.perMetre.maxwell : out.maxwell;
        const QList<QList<double>>& loss = middle ? out.perMetre.dielectricLoss : out.dielectricLoss;
        for (int i = 0; i < nets.size(); ++i) {
            double here = 0, lost = 0;
            for (int j = 0; j < nets.size(); ++j) {
                here += cap.at(i).at(j);
                lost += loss.at(i).at(j);
            }
            out.lossTangent << (here != 0 ? lost / here : 0.0);
        }
    }
    // The metal each net is on: its conductivity and thickness, for the
    // surface resistance at a frequency.
    for (const Net& net : nets) {
        const Layer* metal = net.layers.isEmpty() ? nullptr : stack.named(net.layers.first());
        out.sigma << (metal != nullptr ? metal->sigma : 0.0);
        out.thickness << (metal != nullptr ? metal->thickness : 0.0);
        out.roughness << (metal != nullptr ? metal->roughness : 0.0);
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
    out.c0 = mutualOf(out.maxwell0);
    for (const Net& net : nets) out.names << net.name;

    // (The charge's own count, as the mesh's line gives it - "unknowns"
    // there are a full-wave solve's edges, a second number the phase B
    // check found beside this one under the same word.)
    const double megabytes = 8.0 * double(n) * double(n) / (1024 * 1024);
    out.lines << tr("%1 cell(s) on %2 net(s) over %3: a charge on each, a dense matrix of %4 to solve.")
                     .arg(mesh.cells.size()).arg(nets.size()).arg(layers.join(QStringLiteral(", ")))
                     .arg(megabytes >= 1024 ? tr("%1 GB").arg(megabytes / 1024, 0, 'f', 1) : tr("%1 MB").arg(megabytes, 0, 'f', 1));
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

double roughnessFactor(double rms, double sigma, double frequency)
{
    if (!(rms > 0) || !(sigma > 0) || !(frequency > 0)) return 1;
    constexpr double mu0 = 4e-7 * M_PI;
    const double depth = std::sqrt(2.0 / (2 * M_PI * frequency * mu0 * sigma));
    const double ratio = rms / depth;
    return 1 + 2 / M_PI * std::atan(1.4 * ratio * ratio);
}

double groundSurfaceResistance(const Extraction& extraction, double frequency)
{
    if (extraction.groundKind == QLatin1String("impedance")) return std::max(0.0, extraction.groundValue);
    if (extraction.groundKind == QLatin1String("metal") && extraction.groundValue > 0 && frequency > 0) {
        constexpr double mu0 = 4e-7 * M_PI;
        return std::sqrt(M_PI * frequency * mu0 / extraction.groundValue);
    }
    return 0;   // (a perfect plane, or none)
}

QList<QList<double>> resistancePerMetre(const Extraction& extraction, double frequency, QList<QList<double>>* ground)
{
    const Extraction::PerMetre& pm = extraction.perMetre;
    const int n = int(extraction.names.size());
    if (ground != nullptr) ground->clear();
    if (!pm.fromMiddle || int(pm.wheeler.size()) != n || int(pm.wheelerGround.size()) != n
        || int(pm.dcResistance.size()) != n || !(frequency > 0))
        return {};
    QList<QList<double>> r(n, QList<double>(n, 0.0)), g(n, QList<double>(n, 0.0));
    // Each metal's slab surface resistance times its own factor: a slab's
    // and not a half space's, so that a metal thin beside its skin depth
    // loses as the slab it is.
    for (int k = 0; k < n; ++k) {
        const double rs = surfaceImpedance(extraction.sigma.value(k), extraction.thickness.value(k), frequency).real()
                          * roughnessFactor(extraction.roughness.value(k), extraction.sigma.value(k), frequency);
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) r[i][j] += rs * pm.wheeler.at(k).at(i).at(j);
    }
    const double rsg = groundSurfaceResistance(extraction, frequency);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            g[i][j] = rsg * pm.wheelerGround.at(i).at(j);
            r[i][j] += g[i][j];
        }
    // Wheeler's rule is of a current on the faces, a skin depth deep; at
    // direct current it runs through the whole metal, and a net's own
    // resistance is never less than that.
    for (int i = 0; i < n; ++i) r[i][i] = std::max(r[i][i], pm.dcResistance.at(i) + g[i][i]);
    if (ground != nullptr) *ground = g;
    return r;
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
    // A lone conductor with no ground is no line: its charge has nothing
    // to return through, so it has a capacitance - to infinity - and no
    // Z0, eps_eff or S parameters (the phase B check of 24 October).
    if (!extraction.grounded && extraction.c.size() == 1) {
        out.problems << tr("One conductor and no ground is no line: its current has nothing to return through. It has "
                           "a capacitance, to infinity, but no Z0, eps_eff or S parameters - put a ground plane under "
                           "the stack, or draw the conductor it returns through.");
        return out;
    }
    // From the line's cross-section, where that was taken: its own per metre,
    // the ends' fringing left out, whatever length is asked for - which is
    // then the length of the two-port alone. Else the whole over the
    // length, ends and all.
    const Extraction::PerMetre& pm = extraction.perMetre;
    const bool middle = pm.fromMiddle && index < pm.maxwell.size() && index < pm.maxwell0.size();
    if (middle) {
        double held = 0, bare = 0;
        for (int j = 0; j < pm.maxwell.at(index).size(); ++j) held += pm.maxwell.at(index).at(j);
        for (int j = 0; j < pm.maxwell0.at(index).size(); ++j) bare += pm.maxwell0.at(index).at(j);
        out.c = held;
        out.c0 = bare;
    } else {
        out.c = extraction.selfCapacitance(index) / length;
        double bare = 0;
        for (int j = 0; j < extraction.c0.at(index).size(); ++j) bare += extraction.c0.at(index).at(j);
        out.c0 = bare / length;
    }
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
    // (Its roughness's share in it: the node said it raised the loss, and
    // until the phase B check's round nothing read it.)
    out.roughnessFactor = roughnessFactor(extraction.roughness.value(index), sigma, frequency);
    out.surfaceResistance = zs.real() * out.roughnessFactor;
    if (sigma > 0) {
        constexpr double mu0 = 4e-7 * M_PI;
        out.skinDepth = std::sqrt(2.0 / (omega * mu0 * sigma));
    }
    // Rs times the length times how the charge crowds: for a line of
    // width W whose current runs evenly that is Rs/W, and where it
    // crowds at the edges it is more.
    out.r = middle && index < pm.crowding.size() ? out.surfaceResistance * pm.crowding.at(index)
                                                 : out.surfaceResistance * length * extraction.crowding.value(index);
    // By Wheeler's rule where it was taken: the metal's faces and the
    // ground's, of the thick cross-section, which converges as the mesh is
    // refined where the crowding of a sheet does not.
    QList<QList<double>> groundPart;
    const QList<QList<double>> wheeler = resistancePerMetre(extraction, frequency, &groundPart);
    if (index < wheeler.size()) {
        out.r = wheeler.at(index).at(index);
        out.rGround = groundPart.at(index).at(index);
        out.rDc = pm.dcResistance.value(index);
    }
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
