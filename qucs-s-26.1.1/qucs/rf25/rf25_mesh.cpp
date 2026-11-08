/*
 * rf25_mesh.cpp - the conductors divided into cells, and what they cost
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "rf25_mesh.h"

#include "multiphysics/fem_expr.h"

#include <QCoreApplication>
#include <QHash>
#include <QSet>

#include <algorithm>
#include <numeric>
#include <array>
#include <cmath>
#include <limits>

namespace qucs_s::rf25 {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::rf25", text);
}

/// How many unknowns this build will set up a dense matrix for. Beyond it
/// the matrix alone is more memory than a desktop has, and the proposal
/// asks for a refusal rather than a machine that fills its swap.
constexpr int kMostUnknowns = 60000;
/// And held compressed: the full-wave solve's own bound.
constexpr int kMostCompressedUnknowns = 2000000;

constexpr double kLightSpeed = 299792458.0;

QJsonValue propertyOf(const Node& node, const QString& key)
{
    if (node.props.contains(key)) return node.props.value(key);
    if (const NodeKind* k = nodeKind(node.type))
        if (const PropertyDef* p = k->property(key)) return p->defaultValue;
    return {};
}

QString textOf(const Node& node, const QString& key)
{
    const QJsonValue v = propertyOf(node, key);
    return v.isString() ? v.toString() : v.isDouble() ? QString::number(v.toDouble()) : QString();
}

fem::Scope scopeOf(const QList<fem::Model::Parameter>& parameters)
{
    fem::Scope scope(&fem::Scope::builtins());
    for (const fem::Model::Parameter& p : parameters) {
        const fem::Expression e = fem::Expression::compile(p.expression, scope);
        if (e.isValid() && e.isConstant()) scope.setConstant(p.name, e.constant(), e.dim());
    }
    return scope;
}

double numberIn(const Node& node, const QString& key, const fem::Scope& scope, double fallback,
                const QString& where, QStringList* problems)
{
    const QString text = textOf(node, key);
    if (text.trimmed().isEmpty()) return fallback;
    const fem::Expression e = fem::Expression::compile(text, scope);
    if (!e.isValid() || !e.isConstant()) {
        if (problems != nullptr)
            *problems << tr("%1: %2 is not a number (%3).")
                             .arg(where, key, e.isValid() ? tr("it is not a constant") : e.error());
        return fallback;
    }
    return e.constant();
}

/// Where a side of \a length from \a from is cut: spaced at most \a step
/// apart, evenly, and the first space halved where \a gradeLow says, the
/// last where \a gradeHigh does. A side shorter than a step is one space
/// (two where graded).
/// How fine a conductor's outermost cells are made where it faces another
/// across a gap: no wider than this share of the gap (meshOf()).
constexpr double kGapShare = 0.25;
/// A metal's thickness, as a share of the narrowest gap beside it, from
/// which the side walls' missing capacitance is said (meshOf()).
constexpr double kThickBesideGap = 0.25;
/// A metal solved as its two faces whose thickness is under this share of
/// its narrowest conductor's width is solved as a sheet (thickened()).
constexpr double kFacesApart = 0.02;

QList<double> gridLines(double from, double length, double step, bool gradeLow, bool gradeHigh, double finestLow = 0,
                        double finestHigh = 0)
{
    QList<double> at;
    const int n = step > 0 ? std::max(1, int(std::ceil(length / step - 1e-9))) : 1;
    const double d = length / n;
    at << from;
    // A graded end's outermost space halved, and halved again while it is
    // wider than the finest asked there (a narrow gap it faces).
    if (gradeLow) {
        at << from + d / 2;
        for (double w = d / 2; finestLow > 0 && w > finestLow && w > 1e-6 * length; w /= 2) at << from + w / 2;
    }
    for (int i = 1; i < n; ++i) at << from + i * d;
    if (gradeHigh) {
        at << from + length - d / 2;
        for (double w = d / 2; finestHigh > 0 && w > finestHigh && w > 1e-6 * length; w /= 2) at << from + length - w / 2;
    }
    at << from + length;
    std::sort(at.begin(), at.end());
    // (One space graded at both ends is cut once, in its middle.)
    QList<double> out;
    for (const double x : std::as_const(at))
        if (out.isEmpty() || x - out.last() > 1e-9 * std::max(1.0, std::abs(length))) out << x;
    if (out.size() < 2) out = {from, from + length};
    return out;
}

/// \a outline cut into rectangles, where every side of it runs along x or
/// y: slabs between its corners' heights - across y, or across x, whichever
/// gives fewer, the broader where both give as many - each slab's spans
/// inside the outline, a span carried on into the next slab where that has
/// it too. So a tee is its line and its stub, an L its two arms, a bend its
/// legs. Empty where a side is slanted.
QList<QRectF> rectanglesOf(const QPolygonF& outline)
{
    QList<QPointF> points(outline.begin(), outline.end());
    if (points.size() > 1 && points.first() == points.last()) points.removeLast();
    const int m = int(points.size());
    if (m < 4) return {};
    const QRectF bounds = outline.boundingRect();
    const double tol = 1e-9 * std::max(1.0, std::max(bounds.width(), bounds.height()));
    for (int i = 0; i < m; ++i) {
        const QPointF a = points.at(i), b = points.at((i + 1) % m);
        if (std::abs(b.x() - a.x()) > tol && std::abs(b.y() - a.y()) > tol) return {};
    }
    const auto slabs = [&points, m, tol](bool acrossY) {
        QList<double> cuts;
        for (const QPointF& v : std::as_const(points)) cuts << (acrossY ? v.y() : v.x());
        std::sort(cuts.begin(), cuts.end());
        QList<double> unique;
        for (const double c : std::as_const(cuts))
            if (unique.isEmpty() || c - unique.last() > tol) unique << c;
        QList<QRectF> done, open;
        for (int s = 0; s + 1 < unique.size(); ++s) {
            const double c0 = unique.at(s), c1 = unique.at(s + 1), mid = (c0 + c1) / 2;
            // Where the outline's sides across the slab cross its middle:
            // inside between each odd crossing and the next.
            QList<double> crossings;
            for (int i = 0; i < m; ++i) {
                const QPointF a = points.at(i), b = points.at((i + 1) % m);
                const double a0 = acrossY ? a.y() : a.x(), b0 = acrossY ? b.y() : b.x();
                if (std::abs(b0 - a0) <= tol) continue;
                if (mid > std::min(a0, b0) && mid < std::max(a0, b0)) crossings << (acrossY ? a.x() : a.y());
            }
            std::sort(crossings.begin(), crossings.end());
            QList<QRectF> next;
            for (int k = 0; k + 1 < crossings.size(); k += 2) {
                const double u0 = crossings.at(k), u1 = crossings.at(k + 1);
                if (u1 - u0 <= tol) continue;
                int carried = -1;
                for (int o = 0; o < open.size() && carried < 0; ++o) {
                    const QRectF& r = open.at(o);
                    const double r0 = acrossY ? r.left() : r.top(), r1 = acrossY ? r.right() : r.bottom();
                    if (std::abs(r0 - u0) <= tol && std::abs(r1 - u1) <= tol) carried = o;
                }
                if (carried >= 0) {
                    QRectF r = open.takeAt(carried);
                    if (acrossY) r.setBottom(c1);
                    else r.setRight(c1);
                    next << r;
                } else {
                    next << (acrossY ? QRectF(QPointF(u0, c0), QPointF(u1, c1)) : QRectF(QPointF(c0, u0), QPointF(c1, u1)));
                }
            }
            done << open;
            open = next;
        }
        done << open;
        return done;
    };
    const QList<QRectF> byY = slabs(true), byX = slabs(false);
    const auto narrowest = [](const QList<QRectF>& rs) {
        double n = std::numeric_limits<double>::infinity();
        for (const QRectF& r : rs) n = std::min(n, std::min(r.width(), r.height()));
        return n;
    };
    const QList<QRectF>& chosen = byY.size() < byX.size() || (byY.size() == byX.size() && narrowest(byY) >= narrowest(byX))
                                      ? byY
                                      : byX;
    // (Their areas add up to the outline's, or it was no simple outline.)
    double sum = 0;
    for (const QRectF& r : chosen) sum += r.width() * r.height();
    if (std::abs(sum - areaOf(outline)) > 1e-6 * std::max(1e-30, areaOf(outline))) return {};
    return chosen;
}

/// A piece of a conductor cut between its corners (piecesOf()): a
/// rectangle, or a trapezoid along a slanted side.
struct Slab {
    QPolygonF outline;
    QRectF box;
    bool rectangle = false;
    /// How wide it is: a rectangle's short side; a trapezoid's mean width
    /// across its slab, or the slab's own depth where that is less.
    double feature = 0;
};

/*!
 * \a outline - a side of it slanted (rectanglesOf() takes it where none
 * is) - cut into slabs between its corners' coordinates, across x or across
 * y: the way its slanted sides run least along the cuts - so that each is
 * in a trapezoid as thin as it can be - then the way of fewer pieces. Each
 * slab's spans inside the outline are a rectangle where both its sides run
 * straight across the slab, carried on into the next slab where that has
 * the same span, and a trapezoid else. So a line drawn inside a pad with
 * tapers is that line, a rectangle, with its pads' rectangles and its
 * tapers' trapezoids beside it, and its grounds alike; meshed as each
 * piece's own width, joined edge to edge, the line's cross-section the line
 * drawn alone's. Sized by the conductor's 2 area / perimeter whole, the line
 * inside the polygon was meshed unlike the line drawn alone - its eps_eff
 * 0.6-0.8 % over it -, and a filter's 0.7 mm line in a polygon called 2694
 * µm wide had one or two cells across it (B4 of the open items of 8
 * November). Empty where the pieces' areas do not add up to the outline's
 * (it crosses itself, or is no simple outline).
 */
QList<Slab> piecesOf(const QPolygonF& outline)
{
    QList<QPointF> points(outline.begin(), outline.end());
    if (points.size() > 1 && points.first() == points.last()) points.removeLast();
    const int m = int(points.size());
    if (m < 3) return {};
    const QRectF bounds = outline.boundingRect();
    const double tol = 1e-9 * std::max(1.0, std::max(bounds.width(), bounds.height()));
    const auto cut = [&points, m, tol](bool acrossY) {
        // (u: along the slab; c: across it, the corners' coordinates.)
        const auto cOf = [acrossY](QPointF p) { return acrossY ? p.y() : p.x(); };
        const auto uOf = [acrossY](QPointF p) { return acrossY ? p.x() : p.y(); };
        QList<double> cuts;
        for (const QPointF& v : std::as_const(points)) cuts << cOf(v);
        std::sort(cuts.begin(), cuts.end());
        QList<double> unique;
        for (const double c : std::as_const(cuts))
            if (unique.isEmpty() || c - unique.last() > tol) unique << c;
        struct Span {
            double u0[2], u1[2];   // its two sides at its low and its high c
            double c0, c1;
        };
        QList<Slab> done;
        QList<Span> open;
        const auto finish = [&done, acrossY, tol](const Span& sp) {
            Slab slab;
            const auto at = [acrossY](double u, double c) { return acrossY ? QPointF(u, c) : QPointF(c, u); };
            slab.outline = QPolygonF({at(sp.u0[0], sp.c0), at(sp.u1[0], sp.c0), at(sp.u1[1], sp.c1), at(sp.u0[1], sp.c1)});
            slab.box = slab.outline.boundingRect();
            slab.rectangle = std::abs(sp.u0[0] - sp.u0[1]) <= tol && std::abs(sp.u1[0] - sp.u1[1]) <= tol;
            const double depth = sp.c1 - sp.c0, wide = 0.5 * ((sp.u1[0] - sp.u0[0]) + (sp.u1[1] - sp.u0[1]));
            slab.feature = slab.rectangle ? std::min(depth, wide) : std::min(depth, wide);
            done << slab;
        };
        for (int s = 0; s + 1 < unique.size(); ++s) {
            const double c0 = unique.at(s), c1 = unique.at(s + 1), mid = (c0 + c1) / 2;
            QList<std::array<double, 3>> crossings;   // (u at mid, at c0, at c1)
            for (int i = 0; i < m; ++i) {
                const QPointF a = points.at(i), b = points.at((i + 1) % m);
                const double ca = cOf(a), cb = cOf(b);
                if (std::abs(cb - ca) <= tol || mid <= std::min(ca, cb) || mid >= std::max(ca, cb)) continue;
                const auto uAt = [&](double c) { return uOf(a) + (uOf(b) - uOf(a)) * (c - ca) / (cb - ca); };
                crossings.push_back({uAt(mid), uAt(c0), uAt(c1)});
            }
            std::sort(crossings.begin(), crossings.end(), [](const auto& x, const auto& y) { return x[0] < y[0]; });
            QList<Span> next;
            for (int k = 0; k + 1 < crossings.size(); k += 2) {
                const auto& lo = crossings.at(k);
                const auto& hi = crossings.at(k + 1);
                if (hi[0] - lo[0] <= tol) continue;
                Span sp{{lo[1], lo[2]}, {hi[1], hi[2]}, c0, c1};
                const bool straight = std::abs(lo[1] - lo[2]) <= tol && std::abs(hi[1] - hi[2]) <= tol;
                int carried = -1;
                for (int o = 0; o < open.size() && carried < 0 && straight; ++o) {
                    const Span& was = open.at(o);
                    if (std::abs(was.u0[0] - was.u0[1]) <= tol && std::abs(was.u1[0] - was.u1[1]) <= tol
                        && std::abs(was.u0[1] - lo[1]) <= tol && std::abs(was.u1[1] - hi[1]) <= tol && std::abs(was.c1 - c0) <= tol)
                        carried = o;
                }
                if (carried >= 0) {
                    sp = open.takeAt(carried);
                    sp.c1 = c1;
                }
                next << sp;
            }
            for (const Span& sp : std::as_const(open)) finish(sp);
            open = next;
        }
        for (const Span& sp : std::as_const(open)) finish(sp);
        return done;
    };
    // How far the slanted sides run along the cuts of each way: across y,
    // their extent in x; across x, in y.
    double runX = 0, runY = 0;
    for (int i = 0; i < m; ++i) {
        const QPointF a = points.at(i), b = points.at((i + 1) % m);
        if (std::abs(a.x() - b.x()) <= tol || std::abs(a.y() - b.y()) <= tol) continue;
        runX += std::abs(a.x() - b.x());
        runY += std::abs(a.y() - b.y());
    }
    const QList<Slab> byY = cut(true), byX = cut(false);
    const bool acrossY = runX < runY - tol || (std::abs(runX - runY) <= tol && byY.size() <= byX.size());
    const QList<Slab>& chosen = acrossY ? byY : byX;
    double sum = 0;
    for (const Slab& p : chosen) sum += areaOf(p.outline);
    if (chosen.isEmpty() || std::abs(sum - areaOf(outline)) > 1e-6 * std::max(1e-30, areaOf(outline))) return {};
    return chosen;
}

/// The lines of \a mine and of \a theirs within [\a lo, \a hi] each made
/// the other's too: where two pieces meet along that stretch, so that
/// every cell of one there shares a whole edge with a cell of the other.
/// Whether \a mine or \a theirs gained any.
bool shareLines(QList<double>& mine, QList<double>& theirs, double lo, double hi, double tol, bool ends = true)
{
    // (\a ends false: the lines strictly inside alone, not the stretch's
    //  ends - where a slanted side's part meets another, an end at a slanted
    //  corner is no line of either.)
    const double in = ends ? -tol : tol;
    QList<double> both;
    for (const double x : std::as_const(mine))
        if (x >= lo + in && x <= hi - in) both << x;
    for (const double x : std::as_const(theirs))
        if (x >= lo + in && x <= hi - in) both << x;
    if (ends) both << lo << hi;
    bool gained = false;
    for (QList<double>* lines : {&mine, &theirs})
        for (const double x : std::as_const(both)) {
            bool known = false;
            for (const double y : std::as_const(*lines)) known = known || std::abs(x - y) <= tol;
            if (known) continue;
            lines->insert(std::lower_bound(lines->begin(), lines->end(), x), x);
            gained = true;
        }
    return gained;
}

/// A key that two cells sharing an edge agree on, whichever way round they
/// have it: the midpoint, rounded to a nanometre.
QString edgeKey(const QPointF& a, const QPointF& b)
{
    const QPointF middle = (a + b) / 2;
    return QStringLiteral("%1,%2").arg(std::llround(middle.x() * 1e3)).arg(std::llround(middle.y() * 1e3));
}

} // namespace

QMap<QString, int> Mesh::byLayer() const
{
    QMap<QString, int> counts;
    for (const MeshCell& c : cells) ++counts[c.layer];
    return counts;
}

MeshSettings meshSettingsOf(const Node& node, const QList<fem::Model::Parameter>& parameters, QStringList* problems)
{
    const fem::Scope scope = scopeOf(parameters);
    MeshSettings how;
    const QJsonValue per = propertyOf(node, QStringLiteral("per_wavelength"));
    if (per.isDouble()) how.perWavelength = per.toInt();
    else if (per.isString() && !per.toString().trimmed().isEmpty()) how.perWavelength = per.toString().toInt();
    const QJsonValue skin = propertyOf(node, QStringLiteral("skin"));
    if (skin.isDouble()) how.skin = skin.toInt();
    const QJsonValue edges = propertyOf(node, QStringLiteral("edges"));
    how.edges = edges.isBool() ? edges.toBool() : how.edges;
    how.maximum = numberIn(node, QStringLiteral("maximum"), scope, 0, tr("Mesh"), problems);
    const QJsonValue across = propertyOf(node, QStringLiteral("cells_across"));
    if (across.isDouble()) how.cellsAcross = across.toInt();
    else if (across.isString() && !across.toString().trimmed().isEmpty()) how.cellsAcross = across.toString().toInt();
    how.stretch = numberIn(node, QStringLiteral("stretch"), scope, how.stretch, tr("Mesh"), problems);
    const QJsonValue most = propertyOf(node, QStringLiteral("most_cells"));
    if (most.isDouble()) how.most = most.toInt();
    else if (most.isString() && !most.toString().trimmed().isEmpty()) how.most = most.toString().toInt();
    if (how.most < 0) {
        if (problems != nullptr) *problems << tr("Mesh: the most cells cannot be fewer than none (0: the solver's own).");
        how.most = 0;
    }
    if (how.cellsAcross < 0) {
        if (problems != nullptr) *problems << tr("Mesh: the cells across a conductor cannot be fewer than none.");
        how.cellsAcross = 0;
    }
    if (how.stretch < 1) {
        if (problems != nullptr) *problems << tr("Mesh: a strip's cells cannot be shorter along it than across (a stretch of 1 at least).");
        how.stretch = 1;
    }
    if (how.perWavelength < 4) {
        if (problems != nullptr)
            *problems << tr("Mesh: %1 cells to a wavelength is too few to carry a current (4 at least).")
                             .arg(how.perWavelength);
        how.perWavelength = std::max(how.perWavelength, 4);
    }
    if (how.maximum < 0) {
        if (problems != nullptr) *problems << tr("Mesh: the largest cell is a length above zero, or nothing.");
        how.maximum = 0;
    }
    if (how.skin < 0) {
        if (problems != nullptr) *problems << tr("Mesh: the layers through the skin depth cannot be fewer than none.");
        how.skin = 0;
    }
    return how;
}

int MeshSettings::mostCells() const
{
    return most > 0 ? most : (compressed ? kMostCompressedCells : kMostCharges);
}

MeshCost costOf(int unknowns, bool compressed)
{
    MeshCost cost;
    cost.unknowns = unknowns;
    if (compressed) {
        // Held compressed: some 20 kB an unknown, its low-rank blocks and its
        // near ones, as the solve measured them from 20 000 unknowns to
        // 310 000 (16 to 19 kB).
        cost.matrixBytes = 2e4 * double(unknowns);
        cost.tooBig = unknowns > kMostCompressedUnknowns;
        const double mb = cost.matrixBytes / (1024 * 1024);
        cost.said = mb >= 1024 ? tr("%1 unknowns; held compressed, some %2 GB").arg(unknowns).arg(mb / 1024, 0, 'f', 1)
                               : tr("%1 unknowns; held compressed, some %2 MB").arg(unknowns).arg(mb, 0, 'f', 1);
        if (cost.tooBig)
            cost.said += tr(" - more than this build will set up (%1 unknowns at most). Merge the vias, drop the fill, or "
                            "mesh more coarsely.").arg(kMostCompressedUnknowns);
        return cost;
    }
    // A dense complex matrix: 16 bytes a term, every term kept (the
    // matrix of a mixed-potential formulation is not symmetric).
    cost.matrixBytes = 16.0 * double(unknowns) * double(unknowns);
    cost.tooBig = unknowns > kMostUnknowns;
    const double mb = cost.matrixBytes / (1024 * 1024);
    cost.said = mb >= 1024 ? tr("%1 unknowns; a dense matrix of %2 GB")
                                 .arg(unknowns).arg(mb / 1024, 0, 'f', 1)
                           : tr("%1 unknowns; a dense matrix of %2 MB").arg(unknowns).arg(mb, 0, 'f', 1);
    if (cost.tooBig)
        cost.said += tr(" - more than this build will set up (%1 unknowns at most). Merge the vias, drop the fill, or "
                        "mesh more coarsely.").arg(kMostUnknowns);
    return cost;
}

double topFrequency(const Model& model)
{
    const Node* study = model.root.child(QStringLiteral("rf_study"));
    if (study == nullptr) return 0;
    const fem::Scope scope = scopeOf(model.parameters());
    const double from = numberIn(*study, QStringLiteral("from"), scope, 0, tr("Study"), nullptr);
    const double to = numberIn(*study, QStringLiteral("to"), scope, 0, tr("Study"), nullptr);
    return std::max(from, to);
}

QString chargeCostOf(int cells)
{
    // A dense real matrix: 8 bytes a term.
    const double mb = 8.0 * double(cells) * double(cells) / (1024 * 1024);
    QString said = mb >= 1024 ? tr("%1 to solve the charge on, a dense matrix of %2 GB").arg(cells).arg(mb / 1024, 0, 'f', 1)
                              : tr("%1 to solve the charge on, a dense matrix of %2 MB").arg(cells).arg(mb, 0, 'f', 1);
    if (cells > kMostCharges)
        said += tr(" - more than this build will solve (%1 at most): ask for fewer cells across a conductor, or a larger "
                   "largest cell").arg(kMostCharges);
    return said;
}

Mesh meshOf(const QList<Conductor>& conductors, const MeshSettings& how, const Stack& stack, double top,
            double micrometre)
{
    Mesh mesh;
    const double perUnit = micrometre > 0 ? micrometre : 1e-6;
    if (conductors.isEmpty()) {
        mesh.problems << tr("There are no shapes to mesh.");
        return mesh;
    }

    // The cell size: a wavelength at the highest frequency, in the fastest
    // dielectric of the stack, divided by the cells asked for - and never
    // larger than the largest cell the user allows.
    double epsr = 1;
    for (const Layer& l : stack.layers)
        if (l.kind == Layer::Dielectric || l.kind == Layer::Substrate) epsr = std::max(epsr, l.epsr);
    double size = 0;   // µm
    if (top > 0) {
        const double wavelength = kLightSpeed / (top * std::sqrt(epsr));   // m
        size = wavelength / how.perWavelength / perUnit;
        mesh.lines << tr("A wavelength at %1 GHz in %2 is %3 µm; %4 cells to it.")
                          .arg(top / 1e9, 0, 'g', 3).arg(QStringLiteral("eps_r %1").arg(epsr, 0, 'g', 3))
                          .arg(wavelength / 1e-6, 0, 'g', 3).arg(how.perWavelength);
    }
    if (how.maximum > 0) {
        const double most = how.maximum / perUnit;
        if (size <= 0 || most < size) {
            if (size > 0)
                mesh.lines << tr("The largest cell allowed (%1 µm) is smaller than that.").arg(most, 0, 'g', 3);
            size = most;
        }
    }
    if (size <= 0) {
        mesh.problems << tr("Nothing says how large a cell may be: give the study a frequency, or the mesh a largest "
                            "cell.");
        return mesh;
    }
    mesh.cellSize = size;

    // Each conductor cut against a grid of its own: lines spaced to put at
    // least cellsAcross cells across its narrowest part, a straight
    // strip's stretched along it, and - where the edges are refined - the
    // outermost space of each side halved, which is where its charge
    // crowds. A grid of lines rather than cells divided again keeps every
    // pair of neighbours sharing a whole edge (a quartered cell met its
    // undivided neighbour along half of one, so the inductance's runs of
    // current, which follow shared edges, lost the conductor's rim).
    //
    // A conductor whose every side runs along x or y is cut into the
    // rectangles it is made of first, and each is meshed as a strip of its
    // own: a tee merged into one outline was a grid of the whole, square
    // cells sized by twice its area over its perimeter, and 1120 cells at
    // four across where its two strips are 312 (the phase C check of 1
    // November, F1). Where two rectangles meet - of one conductor, or two
    // shapes left unmerged - each takes the other's lines along the stretch
    // they share, so that every cell there shares a whole edge with one
    // across it: meshed apart, a stub met its line through one run of
    // current, where two lines happened to coincide. And a side is graded
    // only where it is the conductor's edge, not where it meets another.
    struct Piece {
        int conductor = -1;
        QRectF box;
        bool rectangle = false;        // a rectangle, of a conductor or cut from one
        bool grade[4] = {true, true, true, true};   // its sides at low x, high x, low y, high y
        double gap[4] = {0, 0, 0, 0};                // how far across each side another conductor faces it; 0: none
        QList<double> xs, ys;
        /// A part of its conductor that is no rectangle (piecesOf()): its own
        /// outline, cells clipped to it, and its width; empty, and nought,
        /// for its conductor whole.
        QPolygonF outline;
        double feature = 0;
    };
    QList<Piece> pieces;
    int cutInto = 0, cutFrom = 0, slantedFrom = 0, slantedRectangles = 0, slantedParts = 0;
    for (int c = 0; c < conductors.size(); ++c) {
        const QPolygonF& outline = conductors.at(c).outline;
        const QRectF bounds = outline.boundingRect();
        const double area = areaOf(outline);
        const double box = bounds.width() * bounds.height();
        QList<QRectF> boxes;
        if (box > 0 && std::abs(area - box) <= 1e-6 * box) boxes << bounds;
        else boxes = rectanglesOf(outline);
        if (boxes.size() > 1) {
            ++cutFrom;
            cutInto += int(boxes.size());
        }
        if (boxes.isEmpty()) {
            // A side slanted: its rectangles and the parts of it in the boxes
            // between its corners (piecesOf()), else the conductor whole.
            const QList<Slab> slabs = piecesOf(outline);
            if (!slabs.isEmpty()) {
                ++slantedFrom;
                for (const Slab& slab : slabs) {
                    Piece piece;
                    piece.conductor = c;
                    piece.box = slab.box;
                    piece.rectangle = slab.rectangle;
                    if (!slab.rectangle) {
                        piece.outline = slab.outline;
                        piece.feature = slab.feature;
                    }
                    pieces << piece;
                    ++(slab.rectangle ? slantedRectangles : slantedParts);
                }
                continue;
            }
            Piece whole;
            whole.conductor = c;
            whole.box = bounds;
            pieces << whole;
            continue;
        }
        for (const QRectF& b : std::as_const(boxes)) {
            Piece piece;
            piece.conductor = c;
            piece.box = b;
            piece.rectangle = true;
            pieces << piece;
        }
    }
    // Which rectangles meet which, along which stretch; and a side wholly
    // met by others is no edge.
    struct Meeting {
        int a, b;
        bool alongX;   // they meet along a line of constant y, so share their x lines
        double lo, hi;
    };
    QList<Meeting> meetings;
    {
        QRectF all;
        for (const Piece& p : std::as_const(pieces)) all = all.united(p.box);
        const double tol = 1e-9 * std::max(1.0, std::max(all.width(), all.height()));
        QList<std::array<double, 4>> met(pieces.size(), {0, 0, 0, 0});
        for (int a = 0; a < pieces.size(); ++a) {
            if (!pieces.at(a).rectangle) continue;
            for (int b = a + 1; b < pieces.size(); ++b) {
                if (!pieces.at(b).rectangle) continue;
                if (conductors.at(pieces.at(a).conductor).layer != conductors.at(pieces.at(b).conductor).layer) continue;
                const QRectF& A = pieces.at(a).box;
                const QRectF& B = pieces.at(b).box;
                const double xLo = std::max(A.left(), B.left()), xHi = std::min(A.right(), B.right());
                const double yLo = std::max(A.top(), B.top()), yHi = std::min(A.bottom(), B.bottom());
                if (xHi - xLo > tol && (std::abs(A.bottom() - B.top()) <= tol || std::abs(A.top() - B.bottom()) <= tol)) {
                    meetings << Meeting{a, b, true, xLo, xHi};
                    const bool aBelow = std::abs(A.bottom() - B.top()) <= tol;
                    met[a][aBelow ? 3 : 2] += xHi - xLo;
                    met[b][aBelow ? 2 : 3] += xHi - xLo;
                }
                if (yHi - yLo > tol && (std::abs(A.right() - B.left()) <= tol || std::abs(A.left() - B.right()) <= tol)) {
                    meetings << Meeting{a, b, false, yLo, yHi};
                    const bool aLeft = std::abs(A.right() - B.left()) <= tol;
                    met[a][aLeft ? 1 : 0] += yHi - yLo;
                    met[b][aLeft ? 0 : 1] += yHi - yLo;
                }
            }
        }
        for (int a = 0; a < pieces.size(); ++a) {
            const QRectF& A = pieces.at(a).box;
            const double side[4] = {A.height(), A.height(), A.width(), A.width()};
            for (int k = 0; k < 4; ++k) pieces[a].grade[k] = met.at(a).at(k) < side[k] - tol;
        }
        // A piece that is no rectangle - its conductor whole, a side of it
        // slanted - meets another along the stretches of their outlines that
        // run along x or y on one line: those lines shared too, so that its
        // clipped cells there share whole edges with the other's. Left out,
        // two touching pieces the union had not joined met through no run of
        // current at all where one had a slanted side (B1 of the open items
        // of 8 November). (Its sides stay graded as they were.)
        const auto outlineOf = [&pieces, &conductors](int i) {
            const Piece& p = pieces.at(i);
            return p.rectangle ? QPolygonF(p.box) : !p.outline.isEmpty() ? p.outline : conductors.at(p.conductor).outline;
        };
        for (int a = 0; a < pieces.size(); ++a)
            for (int b = a + 1; b < pieces.size(); ++b) {
                if (pieces.at(a).rectangle && pieces.at(b).rectangle) continue;
                if (conductors.at(pieces.at(a).conductor).layer != conductors.at(pieces.at(b).conductor).layer) continue;
                if (!pieces.at(a).box.adjusted(-tol, -tol, tol, tol).intersects(pieces.at(b).box)) continue;
                const QPolygonF A = outlineOf(a), B = outlineOf(b);
                for (int i = 0, na = int(A.size()); i < na; ++i) {
                    const QPointF p = A.at(i), q = A.at((i + 1) % na);
                    const bool alongX = std::abs(q.y() - p.y()) <= tol, alongY = std::abs(q.x() - p.x()) <= tol;
                    if (alongX && alongY) continue;
                    if (!alongX && !alongY) {
                        // A slanted stretch shared: both pieces cut at the same
                        // points along it - every line of either that crosses
                        // it, along x and along y, made both's -, so that each
                        // cell clipped by it on one side meets one on the
                        // other along the same piece of it. Met in part only,
                        // two pieces touching along a slant were joined where
                        // their grids happened to agree.
                        const QPointF d = q - p;
                        const double len = std::hypot(d.x(), d.y());
                        const QPointF unit = d / len, normal(-unit.y(), unit.x());
                        for (int j = 0, nb = int(B.size()); j < nb; ++j) {
                            const QPointF u = B.at(j) - p, w = B.at((j + 1) % nb) - p;
                            if (std::abs(u.x() * normal.x() + u.y() * normal.y()) > tol
                                || std::abs(w.x() * normal.x() + w.y() * normal.y()) > tol)
                                continue;
                            const double tu = u.x() * unit.x() + u.y() * unit.y(), tw = w.x() * unit.x() + w.y() * unit.y();
                            const double t0 = std::max(0.0, std::min(tu, tw)), t1 = std::min(len, std::max(tu, tw));
                            if (t1 - t0 <= tol) continue;
                            const QPointF e0 = p + unit * t0, e1 = p + unit * t1;
                            meetings << Meeting{a, b, true, std::min(e0.x(), e1.x()), std::max(e0.x(), e1.x())};
                            meetings << Meeting{a, b, false, std::min(e0.y(), e1.y()), std::max(e0.y(), e1.y())};
                        }
                        continue;
                    }
                    for (int j = 0, nb = int(B.size()); j < nb; ++j) {
                        const QPointF u = B.at(j), w = B.at((j + 1) % nb);
                        if (alongX ? (std::abs(u.y() - p.y()) > tol || std::abs(w.y() - p.y()) > tol)
                                   : (std::abs(u.x() - p.x()) > tol || std::abs(w.x() - p.x()) > tol))
                            continue;
                        const double lo = std::max(alongX ? std::min(p.x(), q.x()) : std::min(p.y(), q.y()),
                                                   alongX ? std::min(u.x(), w.x()) : std::min(u.y(), w.y()));
                        const double hi = std::min(alongX ? std::max(p.x(), q.x()) : std::max(p.y(), q.y()),
                                                   alongX ? std::max(u.x(), w.x()) : std::max(u.y(), w.y()));
                        if (hi - lo > tol) meetings << Meeting{a, b, alongX, lo, hi};
                    }
                }
            }
    }
    if (cutFrom > 0)
        mesh.lines << tr("%1 shape(s) cut into the %2 rectangle(s) they are made of, each meshed as a strip of its own; "
                         "where two rectangles meet, their cells meet edge to edge.")
                          .arg(cutFrom).arg(cutInto);
    if (slantedFrom > 0)
        mesh.lines << tr("%1 shape(s) with slanted sides cut at their corners into %2 rectangle(s), each meshed as a strip "
                         "of its own, and %3 part(s) along the slanted sides, each meshed by its own width; where two "
                         "meet, their cells meet edge to edge.")
                          .arg(slantedFrom).arg(slantedRectangles).arg(slantedParts);
    // Where each side faces another conductor of its layer, and how far
    // across: the charge crowds on both edges of a narrow gap, and halving
    // the outermost cells once - the edges' grading - leaves a gap of 6 µm
    // between conductors of 50 and 150 µm with one cell of 3 µm on one side
    // and of 9 on the other. A coplanar line so on thin-film lithium niobate
    // came out 10 % under its capacitance in vacuum, 12 % under its own
    // conformal mapping (the review of 3 November); its edge cells halved
    // down to a quarter of the gap it is a per cent from either, and the
    // cells across hardly matter. The current crowds there too: a measured
    // coplanar line on alumina, 25 µm gaps beside a 50 µm signal and its
    // 270 µm grounds, the grounds' outermost cells 45 µm wide, lost 19 %
    // under its 2D eddy-current solve in a full-wave solve, and 1 % under at
    // 10 GHz to 9 % over at 60 on the graded cells. (Pieces swept in order
    // of their left edges: only those within twice the largest cell can face
    // one within reach of its grading.)
    if (how.edges && how.gaps) {
        QList<int> order(pieces.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&pieces](int a, int b) { return pieces.at(a).box.left() < pieces.at(b).box.left(); });
        const double reach = size / (2 * kGapShare);
        for (int oi = 0; oi < order.size(); ++oi) {
            Piece& p = pieces[order.at(oi)];
            const QRectF& A = p.box;
            const QString& layer = conductors.at(p.conductor).layer;
            for (int oj = oi + 1; oj < order.size(); ++oj) {
                Piece& q = pieces[order.at(oj)];
                const QRectF& B = q.box;
                if (B.left() > A.right() + reach) break;
                if (conductors.at(q.conductor).layer != layer) continue;
                const auto keep = [](double* at, double g) {
                    if (g > 0 && (*at <= 0 || g < *at)) *at = g;
                };
                const bool acrossY0 = B.bottom() > A.top() && B.top() < A.bottom();   // they share some y: face across x
                const bool acrossX0 = B.right() > A.left() && B.left() < A.right();   // they share some x: face across y
                // (Two pieces of one conductor with that conductor between
                //  them - a line and its own pad past its taper - face no gap.)
                const auto filled = [&](bool alongX) {
                    if (p.conductor != q.conductor) return false;
                    const QPointF mid = alongX ? QPointF(0.5 * (std::max(A.left(), B.left()) + std::min(A.right(), B.right())),
                                                         0.5 * (std::min(A.bottom(), B.bottom()) + std::max(A.top(), B.top())))
                                               : QPointF(0.5 * (std::min(A.right(), B.right()) + std::max(A.left(), B.left())),
                                                         0.5 * (std::max(A.top(), B.top()) + std::min(A.bottom(), B.bottom())));
                    return conductors.at(p.conductor).outline.containsPoint(mid, Qt::OddEvenFill);
                };
                const bool acrossY = acrossY0 && !filled(false);
                const bool acrossX = acrossX0 && !filled(true);
                if (acrossY && B.left() >= A.right()) {
                    keep(&p.gap[1], B.left() - A.right());
                    keep(&q.gap[0], B.left() - A.right());
                }
                if (acrossY && A.left() >= B.right()) {
                    keep(&p.gap[0], A.left() - B.right());
                    keep(&q.gap[1], A.left() - B.right());
                }
                if (acrossX && B.top() >= A.bottom()) {
                    keep(&p.gap[3], B.top() - A.bottom());
                    keep(&q.gap[2], B.top() - A.bottom());
                }
                if (acrossX && A.top() >= B.bottom()) {
                    keep(&p.gap[2], A.top() - B.bottom());
                    keep(&q.gap[3], A.top() - B.bottom());
                }
            }
        }
        for (Piece& p : pieces)
            for (int k = 0; k < 4; ++k)
                if (!p.grade[k] || p.gap[k] >= reach) p.gap[k] = 0;
    }
    double narrowest = 0, finest = 0;
    int strips = 0;
    // The conductors cut with \a perFeature cells across each: false where
    // one of them would take more cells than a mesh may hold, \a many then
    // how many it would have taken.
    // As many cells as the solver it is for holds, before fewer are put
    // across a conductor; and a piece of more than four times that, or of
    // the old bound of a dense full-wave matrix where that is more, not cut
    // at all.
    const int most = how.mostCells();
    const double pieceMost = std::max(double(kMostUnknowns), double(most));
    int facing = 0;
    QList<int> cellConductor;   // (each cell's conductor, for the check of touching pieces below)
    const auto cut = [&mesh, &pieces, &meetings, &conductors, &how, &narrowest, &finest, &strips, &facing, &cellConductor,
                      size, pieceMost](int perFeature, double* many) {
        mesh.cells.clear();
        cellConductor.clear();
        narrowest = finest = 0;
        strips = 0;
        facing = 0;
        for (Piece& piece : pieces) {
            const Conductor& conductor = conductors.at(piece.conductor);
            const QRectF bounds = piece.box;
            const double narrow = std::min(bounds.width(), bounds.height());
            // How wide the conductor is where it is narrowest: a rectangle's
            // short side, else twice its area over its perimeter - which is
            // a trace's width for a trace of any shape, a spiral's included.
            const QPolygonF& shape = piece.outline.isEmpty() ? conductor.outline : piece.outline;
            const double area = piece.rectangle ? bounds.width() * bounds.height() : areaOf(shape);
            const double perimeter = piece.rectangle ? 0 : perimeterOf(shape);
            const double feature = piece.rectangle ? narrow
                                   : piece.feature > 0 ? piece.feature
                                                       : (perimeter > 0 ? 2 * area / perimeter : narrow);
            double across = size;
            if (perFeature > 0 && feature > 0) across = std::min(across, feature / perFeature);
            const bool strip = piece.rectangle && std::max(bounds.width(), bounds.height()) >= 3 * narrow;
            const bool alongX = bounds.width() >= bounds.height();
            double along = across;
            if (strip && how.stretch > 1) along = std::min(size, how.stretch * across);
            const double stepX = strip ? (alongX ? along : across) : across;
            const double stepY = strip ? (alongX ? across : along) : across;
            // (Counted before the lines are made: a fine grid over a large
            // layout would be millions of them.)
            const double squares = std::ceil(bounds.width() / stepX) * std::ceil(bounds.height() / stepY);
            if (squares > 4.0 * pieceMost) {
                *many = squares;
                return false;
            }
            const double* gap = piece.gap;
            piece.xs = gridLines(bounds.left(), bounds.width(), stepX, how.edges && piece.grade[0], how.edges && piece.grade[1],
                                 kGapShare * gap[0], kGapShare * gap[1]);
            piece.ys = gridLines(bounds.top(), bounds.height(), stepY, how.edges && piece.grade[2], how.edges && piece.grade[3],
                                 kGapShare * gap[2], kGapShare * gap[3]);
            // (The sides graded past the edges' one halving, for the line
            //  that says so: those whose quarter gap is narrower than it.)
            for (int k = 0; k < 4 && how.edges; ++k) {
                const double span = k < 2 ? bounds.width() : bounds.height(), step = k < 2 ? stepX : stepY;
                const double outermost = span / std::max(1, int(std::ceil(span / step - 1e-9))) / 2;
                if (gap[k] > 0 && kGapShare * gap[k] < outermost) ++facing;
            }
            if (narrowest <= 0 || feature < narrowest) narrowest = feature;
            if (finest <= 0 || across < finest) finest = across;
            if (strip) ++strips;
        }
        // Each meeting's lines made both pieces', until none gains any: a
        // line one takes may lie where it meets a third.
        QRectF all;
        for (const Piece& p : std::as_const(pieces)) all = all.united(p.box);
        const double tol = 1e-9 * std::max(1.0, std::max(all.width(), all.height()));
        const auto shareAll = [&pieces, &meetings, tol](bool rectanglesOnly) {
            bool gained = !meetings.isEmpty();
            for (int round = 0; gained && round < 64; ++round) {
                gained = false;
                for (const Meeting& m : std::as_const(meetings)) {
                    Piece& a = pieces[m.a];
                    Piece& b = pieces[m.b];
                    const bool rectangles = a.rectangle && b.rectangle;
                    if (rectanglesOnly && !rectangles) continue;
                    gained = (m.alongX ? shareLines(a.xs, b.xs, m.lo, m.hi, tol, rectangles)
                                       : shareLines(a.ys, b.ys, m.lo, m.hi, tol, rectangles))
                             || gained;
                }
            }
        };
        shareAll(true);
        // Where a slanted side's trapezoid meets the rest, the lines go one
        // way: from the straight strips - a line, which a polygon's line
        // must be meshed as when drawn alone - to the trapezoids beside
        // them, and from those to the other rectangles (a pad), each piece
        // taking its stronger neighbours' lines where it meets them in place
        // of its own. Shared both ways, a pad's lines came through its taper
        // into the line across its width (B4 of the open items of 8
        // November). Then shared both ways, so that cells meet edge to edge
        // whatever was left.
        bool parts = false;
        for (const Piece& p : std::as_const(pieces)) parts = parts || (!p.rectangle && !p.outline.isEmpty());
        if (parts) {
            const auto isStrip = [](const Piece& p) {
                const double lo = std::min(p.box.width(), p.box.height()), hi = std::max(p.box.width(), p.box.height());
                return p.rectangle && hi >= 3 * lo;
            };
            const auto rank = [&isStrip](const Piece& p) { return isStrip(p) ? 0 : !p.rectangle ? 1 : 2; };
            const auto widthOf = [](const Piece& p) {
                return p.rectangle ? std::min(p.box.width(), p.box.height()) : p.feature;
            };
            QList<int> order(pieces.size());
            std::iota(order.begin(), order.end(), 0);
            std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
                const Piece& p = pieces.at(a);
                const Piece& q = pieces.at(b);
                if (rank(p) != rank(q)) return rank(p) < rank(q);
                if (rank(p) == 0) return std::max(p.box.width(), p.box.height()) > std::max(q.box.width(), q.box.height());
                return widthOf(p) < widthOf(q);
            });
            QList<int> place(pieces.size());
            for (int k = 0; k < order.size(); ++k) place[order.at(k)] = k;
            // Each piece's stretches already taken from a stronger one, each way.
            QList<QList<std::pair<double, double>>> lockedX(pieces.size()), lockedY(pieces.size());
            const auto inside = [tol](const QList<std::pair<double, double>>& locks, double x) {
                for (const auto& [lo, hi] : locks)
                    if (x > lo + tol && x < hi - tol) return true;
                return false;
            };
            for (const int p : std::as_const(order)) {
                // Its meetings with stronger pieces, the strongest first.
                QList<const Meeting*> mine;
                for (const Meeting& m : std::as_const(meetings)) {
                    const int q = m.a == p ? m.b : m.b == p ? m.a : -1;
                    if (q < 0 || place.at(q) > place.at(p)) continue;
                    if (pieces.at(p).rectangle && pieces.at(q).rectangle) continue;
                    mine << &m;
                }
                std::stable_sort(mine.begin(), mine.end(), [&](const Meeting* x, const Meeting* y) {
                    return place.at(x->a == p ? x->b : x->a) < place.at(y->a == p ? y->b : y->a);
                });
                for (const Meeting* m : std::as_const(mine)) {
                    const int q = m->a == p ? m->b : m->a;
                    QList<double>& lines = m->alongX ? pieces[p].xs : pieces[p].ys;
                    const QList<double>& theirs = m->alongX ? pieces.at(q).xs : pieces.at(q).ys;
                    QList<std::pair<double, double>>& locks = m->alongX ? lockedX[p] : lockedY[p];
                    QList<double> next;
                    for (const double x : std::as_const(lines))
                        if (x <= m->lo + tol || x >= m->hi - tol || inside(locks, x)) next << x;
                    for (const double x : theirs)
                        if (x > m->lo + tol && x < m->hi - tol && !inside(locks, x)) next << x;
                    std::sort(next.begin(), next.end());
                    lines.clear();
                    for (const double x : std::as_const(next))
                        if (lines.isEmpty() || x - lines.last() > tol) lines << x;
                    locks << std::make_pair(m->lo, m->hi);
                }
            }
        }
        shareAll(false);
        for (const Piece& piece : std::as_const(pieces)) {
            const Conductor& conductor = conductors.at(piece.conductor);
            const QList<double>& xs = piece.xs;
            const QList<double>& ys = piece.ys;
            if (double(xs.size() - 1) * double(ys.size() - 1) > 4.0 * pieceMost) {
                *many = double(xs.size() - 1) * double(ys.size() - 1);
                return false;
            }
            for (int i = 0; i + 1 < xs.size(); ++i)
                for (int j = 0; j + 1 < ys.size(); ++j) {
                    const QRectF square(QPointF(xs.at(i), ys.at(j)), QPointF(xs.at(i + 1), ys.at(j + 1)));
                    const double full = square.width() * square.height();
                    const QList<QPolygonF> cut = piece.rectangle ? QList<QPolygonF>{QPolygonF(square)}
                                                                 : clipTo({piece.outline.isEmpty() ? conductor.outline : piece.outline},
                                                                          QPolygonF(square));
                    for (const QPolygonF& part : cut) {
                        const double a = piece.rectangle ? full : areaOf(part);
                        if (a <= 0) continue;
                        MeshCell cell;
                        cell.layer = conductor.layer;
                        cell.outline = part;
                        cell.centre = part.boundingRect().center();
                        cell.area = a;
                        // On the edge where the cut took something off, or
                        // where the square touches the conductor's bounds.
                        cell.onEdge = std::abs(a - full) > full * 1e-6 || i == 0 || j == 0 || i + 2 == xs.size()
                                      || j + 2 == ys.size();
                        mesh.cells << cell;
                        cellConductor << piece.conductor;
                    }
                }
        }
        *many = double(mesh.cells.size());
        return true;
    };
    // As many across as asked, or as many as the solver will take: a large
    // layout at ten across a narrow trace is tens of thousands of cells,
    // and refusing it outright left a user nothing to solve (the heater
    // example came out at 72 024). Fewer across is said, with why.
    int perFeature = how.cellsAcross;
    double many = 0;
    bool fits = cut(perFeature, &many) && many <= most;
    while (!fits && perFeature > 1) {
        const double was = many;
        const int fewer = std::max(1, int(std::floor(perFeature * std::sqrt(double(most) / std::max(1.0, many)))));
        const int tried = std::min(perFeature - 1, fewer);
        double now = 0;
        const bool cutOk = cut(tried, &now);
        // (Where the largest cell sets the size and not the cells across,
        // fewer across changes nothing: no use going on.)
        if (cutOk && now >= was) {
            perFeature = tried;
            many = now;
            break;
        }
        perFeature = tried;
        many = now;
        fits = cutOk && many <= most;
    }
    if (!fits && many > 4.0 * pieceMost) {
        mesh.cells.clear();
        mesh.problems << tr("The layout would take %1 cells of %2 µm even at one across a conductor: ask for a larger "
                            "largest cell, or a lower top frequency.")
                             .arg(many, 0, 'g', 3).arg(size, 0, 'g', 3);
        return mesh;
    }
    if (perFeature < how.cellsAcross && fits)
        mesh.lines << tr("%1 cells across a conductor would be more than the %2 this build solves: %3 across instead.")
                          .arg(how.cellsAcross).arg(most).arg(perFeature);
    else if (perFeature < how.cellsAcross)
        mesh.lines << tr("Fewer cells across a conductor do not bring the mesh within the %1 this build solves: the "
                         "largest cell, or the wavelength, sets the size there - ask for a larger largest cell.")
                          .arg(most);
    if (perFeature > 0 && narrowest > 0)
        mesh.lines << tr("%1 cell(s) across the narrowest conductor (%2 µm across): cells of %3 µm there%4.")
                          .arg(perFeature).arg(narrowest, 0, 'g', 4).arg(finest, 0, 'g', 3)
                          .arg(strips > 0 ? tr(", up to %1 times as long along a straight strip").arg(how.stretch, 0, 'g', 3)
                                          : QString());
    if (how.edges) mesh.lines << tr("The outermost cells of each conductor's sides are half as wide, where its charge "
                                    "crowds.");
    mesh.facing = facing;
    if (facing > 0)
        mesh.lines << tr("%1 side(s) face another conductor across a gap: their outermost cells are halved until they are no "
                         "wider than a quarter of it, where the charge and the current crowd on both edges.")
                          .arg(facing);

    // Conductors of one layer that touch along an edge are one conductor -
    // the union joins them - and a current crosses from one to the other
    // only where a cell of each shares a whole edge. Where none does (a
    // stretch of slanted edge, or the merge turned off), the solve has them
    // apart, which is said, naming them: it said nothing, and a filter's
    // stub was simply not there (B1 of the open items of 8 November).
    if (cellConductor.size() == mesh.cells.size()) {
        QRectF all;
        for (const Conductor& c : conductors) all = all.united(c.outline.boundingRect());
        const double tol = 1e-6 * std::max(1.0, std::max(all.width(), all.height()));
        QHash<QString, QList<int>> byEdge;   // conductors whose cells have that edge
        for (int c = 0; c < mesh.cells.size(); ++c) {
            const QPolygonF& o = mesh.cells.at(c).outline;
            for (int i = 0, n = int(o.size()); i < n; ++i) {
                QList<int>& who = byEdge[mesh.cells.at(c).layer + QLatin1Char('|') + edgeKey(o.at(i), o.at((i + 1) % n))];
                if (!who.contains(cellConductor.at(c))) who << cellConductor.at(c);
            }
        }
        QSet<QPair<int, int>> joined;
        for (const QList<int>& who : std::as_const(byEdge))
            for (int i = 0; i < who.size(); ++i)
                for (int j = i + 1; j < who.size(); ++j) joined.insert(qMakePair(std::min(who.at(i), who.at(j)), std::max(who.at(i), who.at(j))));
        const auto said = [&conductors](int c) {
            const QPointF m = conductors.at(c).outline.boundingRect().center();
            return tr("%1's piece about %2, %3 µm").arg(conductors.at(c).layer).arg(m.x(), 0, 'g', 6).arg(m.y(), 0, 'g', 6);
        };
        for (int a = 0; a < conductors.size(); ++a)
            for (int b = a + 1; b < conductors.size(); ++b) {
                if (conductors.at(a).layer != conductors.at(b).layer || joined.contains(qMakePair(a, b))) continue;
                if (!touchAlongAnEdge(conductors.at(a).outline, conductors.at(b).outline, tol)) continue;
                const QString w = tr("%1 and %2 touch along an edge, but no cell of one shares an edge with a cell of the "
                                     "other, so no current crosses between them here: the solve has them apart. Draw them "
                                     "as one shape, or turn the layer's merge on.")
                                      .arg(said(a), said(b));
                mesh.warnings << w;
                mesh.lines << w;
            }
    }

    // A metal thick beside the gaps it faces: the capacitance across a
    // narrow gap is much of it in the field between the side walls, which
    // neither model holds - a sheet has none, and the two faces' walls carry
    // no charge (rf25_static.h) -, so the answer reads low. On a comb of 1
    // µm metal in 1 µm gaps it was 19 % under Palace, two sheets with no
    // walls giving 0.82 of solid metal; 3 µm in 50 µm gaps, as rf25_fullwave.h
    // validates, is a few per cent. Said where the thickness is a quarter of
    // the narrowest gap or more (B2 of the open items of 8 November: nothing
    // was said).
    {
        QMap<QString, double> narrowestGap;   // (µm, by layer: between pieces facing each other across x or y)
        for (int a = 0; a < pieces.size(); ++a)
            for (int b = a + 1; b < pieces.size(); ++b) {
                const QString& layer = conductors.at(pieces.at(a).conductor).layer;
                if (conductors.at(pieces.at(b).conductor).layer != layer) continue;
                const QRectF& A = pieces.at(a).box;
                const QRectF& B = pieces.at(b).box;
                double gap = -1;
                if (B.bottom() > A.top() && B.top() < A.bottom())   // sharing some y: across x
                    gap = std::max(B.left() - A.right(), A.left() - B.right());
                if (B.right() > A.left() && B.left() < A.right()) {   // sharing some x: across y
                    const double g = std::max(B.top() - A.bottom(), A.top() - B.bottom());
                    gap = gap > 0 && g > 0 ? std::min(gap, g) : std::max(gap, g);
                }
                if (gap <= 1e-9 * size) continue;
                if (!narrowestGap.contains(layer) || gap < narrowestGap.value(layer)) narrowestGap.insert(layer, gap);
            }
        for (auto it = narrowestGap.constBegin(); it != narrowestGap.constEnd(); ++it) {
            const Layer* metal = stack.named(it.key());
            if (metal == nullptr || metal->kind != Layer::Metal || !(metal->thickness > 0)) continue;
            const double t = metal->thickness / perUnit, gap = it.value();
            if (t < kThickBesideGap * gap) continue;
            // (Solved as its two faces, the walls' charge is added as
            //  capacitance between facing edges - wallGapsOf() - from a 2D
            //  solve of straight walls, which is still an approximation.)
            const QString w = metal->twoSheets
                                  ? tr("%1 is %2 µm thick beside gaps as narrow as %3 µm: the field between its side walls "
                                       "is in this answer as capacitances between facing edge cells, from a 2D solve of "
                                       "straight walls - its corners, finger tips and slanted walls are taken as straight.")
                                        .arg(it.key()).arg(t, 0, 'g', 3).arg(gap, 0, 'g', 3)
                                  : tr("%1 is %2 µm thick beside gaps as narrow as %3 µm: the field between its side walls "
                                       "is not in this answer - a sheet has no walls - so the capacitance across its gaps "
                                       "reads low, by some 20 % where it is as thick as the gap is wide. Solved as its two "
                                       "faces (the metal's Solved as), they are in it.")
                                        .arg(it.key()).arg(t, 0, 'g', 3).arg(gap, 0, 'g', 3);
            mesh.warnings << w;
            mesh.lines << w;
        }
    }

    // The unknowns: an edge two cells of a layer share is where current
    // flows from one to the other, and is one unknown of a rooftop basis.
    QHash<QString, int> shared;
    for (const MeshCell& cell : std::as_const(mesh.cells))
        for (int i = 0, n = cell.outline.size(); i < n; ++i)
            ++shared[cell.layer + QLatin1Char('|') + edgeKey(cell.outline.at(i), cell.outline.at((i + 1) % n))];
    // A metal solved as its two faces is its cells twice, its edges twice,
    // and a wall up from each cell along its conductor's edge (thickened()).
    const auto twoFaces = [&stack](const QString& layer) {
        const Layer* l = stack.named(layer);
        return l != nullptr && l->kind == Layer::Metal && l->twoSheets && l->thickness > 0;
    };
    int unknowns = 0, charges = int(mesh.cells.size());
    for (auto it = shared.constBegin(); it != shared.constEnd(); ++it)
        if (it.value() >= 2) unknowns += twoFaces(it.key().section(QLatin1Char('|'), 0, 0)) ? 2 : 1;
    QStringList faced;
    for (const MeshCell& cell : std::as_const(mesh.cells))
        if (twoFaces(cell.layer)) {
            ++charges;
            if (cell.onEdge) ++unknowns;
            if (!faced.contains(cell.layer)) faced << cell.layer;
        }
    mesh.charges = charges;
    mesh.cost = how.compressed ? costOf(unknowns + charges, true) : costOf(unknowns);
    // (For a full-wave solve held compressed, its cells and runs together
    //  are its unknowns, and the charge solve's dense matrix is no one's.)
    if (how.compressed)
        mesh.lines << tr("%1 cell(s) on %2 layer(s), and the %3 edge(s) between them: a full-wave solve's %4 unknowns, %5.")
                          .arg(charges).arg(mesh.byLayer().size()).arg(unknowns).arg(unknowns + charges)
                          .arg(costOf(unknowns + charges, true).said.section(QLatin1String("; "), 1));
    else
        mesh.lines << tr("%1 cell(s) on %2 layer(s): %3. The %4 edge(s) between them are a full-wave solve's unknowns: %5.")
                          .arg(charges).arg(mesh.byLayer().size()).arg(chargeCostOf(charges))
                          .arg(unknowns).arg(mesh.cost.said);
    if (!faced.isEmpty())
        mesh.lines << tr("%1 solved as its two faces: its cells and their edges twice, and a wall up from each cell along "
                         "its edges, in those counts.")
                          .arg(faced.join(QStringLiteral(", ")));
    if (mesh.cost.tooBig)
        mesh.problems << mesh.cost.said;
    // (The mesh's own cells: a metal's top face is the solver's to refuse,
    //  which a calibration strip too long for it is cut short for.)
    if (!how.compressed && mesh.cells.size() > kMostCharges) mesh.problems << chargeCostOf(int(mesh.cells.size()));
    if (mesh.cells.isEmpty()) mesh.problems << tr("The mesh came out empty.");
    return mesh;
}


bool Thickened::made(const QString& layer) const
{
    const Layer* l = stack.named(layer);
    return l != nullptr && (l->face == Layer::Top || l->face == Layer::Walls);
}

Thickened thickened(const Stack& stack, const QList<Conductor>& conductors, const Mesh& mesh)
{
    Thickened out;
    out.stack = stack;
    out.conductors = conductors;
    out.mesh = mesh;
    out.cells = int(mesh.cells.size());
    QStringList metals;
    for (const Layer& l : stack.layers)
        if (l.kind == Layer::Metal && l.twoSheets && l.thickness > 0 && l.face == Layer::Whole) metals << l.name;
    // A film - a metal under a fiftieth of its narrowest conductor's width
    // (twice its area over its perimeter) - is one sheet, its thickness in
    // its loss: two faces that close are a pair of sheets whose charges
    // nearly the same potentials cannot tell apart - 0.01 µm on a comb's 1
    // µm fingers took GMRES 2492 iterations at six across, and the reviewers'
    // twelve-finger comb never converged, where the sheet it is in the limit
    // solves in 22 (B2 of the open items of 8 November) -, and what they add
    // to it is under a per cent.
    for (qsizetype i = metals.size() - 1; i >= 0; --i) {
        double narrowest = 0;
        for (const Conductor& c : conductors) {
            if (c.layer != metals.at(i)) continue;
            const double around = perimeterOf(c.outline), width = around > 0 ? 2 * areaOf(c.outline) / around : 0;
            if (width > 0 && (narrowest <= 0 || width < narrowest)) narrowest = width;
        }
        const Layer* l = stack.named(metals.at(i));
        const double t = l->thickness * 1e6;   // µm, as the mesh
        if (narrowest > 0 && t < kFacesApart * narrowest) {
            out.said << tr("%1 is solved as a sheet, its thickness in its loss: %2 µm thick on conductors as narrow as %3 "
                           "µm, it is a film, its two faces as good as one.")
                            .arg(metals.at(i)).arg(t, 0, 'g', 3).arg(narrowest, 0, 'g', 3);
            for (Layer& layer : out.stack.layers)
                if (layer.name == metals.at(i)) layer.twoSheets = false;
            metals.removeAt(i);
        }
    }
    if (metals.isEmpty()) return out;
    out.any = true;

    // A name no layer of the stack has, for what is made.
    const auto unused = [&stack](const QString& wanted) {
        QString name = wanted;
        for (int k = 2; stack.named(name) != nullptr; ++k) name = QStringLiteral("%1 %2").arg(wanted).arg(k);
        return name;
    };
    QHash<QString, QString> topOf, wallsOf;   // (by the metal's name)
    QList<Layer> layers;
    for (int i = 0; i < stack.layers.size(); ++i) {
        Layer metal = stack.layers.at(i);
        if (!metals.contains(metal.name)) {
            layers << metal;
            continue;
        }
        metal.face = Layer::Bottom;
        metal.faceOf = metal.name;
        layers << metal;
        // The room its thickness takes, filled with what is over it: the
        // first dielectric above it short of another metal, else the air.
        Layer room;
        room.kind = Layer::Dielectric;
        room.name = unused(tr("%1 (beside)").arg(metal.name));
        room.thickness = metal.thickness;
        room.z0 = metal.z0;
        room.z1 = metal.z1;
        QString filled = tr("the air");
        for (int j = i + 1; j < stack.layers.size(); ++j) {
            const Layer& above = stack.layers.at(j);
            if (above.kind == Layer::Metal || above.kind == Layer::Cover) break;
            if (above.kind != Layer::Dielectric) continue;
            room.epsr = above.epsr;
            room.tand = above.tand;
            filled = above.name;
            break;
        }
        room.faceOf = metal.name;
        layers << room;
        Layer top = metal;
        top.tag.clear();
        top.name = unused(tr("%1 (top)").arg(metal.name));
        top.face = Layer::Top;
        top.z0 = top.z1 = metal.z1;
        layers << top;
        Layer walls;
        walls.kind = Layer::Via;
        walls.name = unused(tr("%1 (walls)").arg(metal.name));
        walls.from = metal.name;
        walls.to = top.name;
        walls.sigma = metal.sigma;
        walls.face = Layer::Walls;
        walls.faceOf = metal.name;
        walls.z0 = metal.z0;
        walls.z1 = metal.z1;
        layers << walls;
        topOf.insert(metal.name, top.name);
        wallsOf.insert(metal.name, walls.name);
        out.said << tr("%1 is solved as its two faces: a sheet at its bottom and one at its top, %2 µm over it, with %3 "
                       "between them beside its walls, which join the two along its edges.")
                        .arg(metal.name).arg(metal.thickness * 1e6, 0, 'g', 4).arg(filled);
    }
    // A via up from such a metal leaves its top face; one down from it, or
    // up to it from below, its bottom. (Above by its place in the stack,
    // which is its place in z: a stack made by hand has no z of its own.)
    const auto place = [&stack](const Layer* l) { return l == nullptr ? -1 : int(l - stack.layers.constData()); };
    for (Layer& via : layers) {
        if (via.kind != Layer::Via || via.face != Layer::Whole) continue;
        const Layer* from = stack.named(via.from);
        const Layer* to = stack.named(via.to);
        const auto upper = [&stack, &place](const Layer* a, const Layer* b, const QString& bName) {
            return a != nullptr && b != nullptr && !stack.isGround(bName) && place(b) > place(a);
        };
        if (from != nullptr && topOf.contains(from->name) && upper(from, to, via.to)) via.from = topOf.value(from->name);
        else if (to != nullptr && topOf.contains(to->name) && upper(to, from, via.from)) via.to = topOf.value(to->name);
    }
    out.stack.layers = layers;

    // The shapes: each on its top face too, and its walls the shape over
    // both, which is what joins the two into one conductor.
    for (const Conductor& c : conductors) {
        const Layer* metal = stack.named(c.layer);
        if (metal == nullptr || !topOf.contains(metal->name)) continue;
        Conductor top = c;
        top.layer = topOf.value(metal->name);
        out.conductors << top;
        Conductor walls = c;
        walls.layer = wallsOf.value(metal->name);
        out.conductors << walls;
    }

    // The cells: each of a two-sheet metal's on its top face too, and a
    // wall along each side that is its conductor's edge - a strip a tenth
    // of the cell deep inside that side, which lands on the cell and on its
    // top. Its outline is only where it stands: the walls carry the
    // current between the faces, and their own impedance is nothing beside
    // the rest.
    for (int c = 0; c < out.cells; ++c) {
        const MeshCell& cell = mesh.cells.at(c);
        const Layer* metal = stack.named(cell.layer);
        if (metal == nullptr || !topOf.contains(metal->name)) continue;
        MeshCell top = cell;
        top.layer = topOf.value(metal->name);
        out.topOf.insert(c, int(out.mesh.cells.size()));
        out.bottomOf.insert(int(out.mesh.cells.size()), c);
        out.mesh.cells << top;
    }
    for (int c = 0; c < out.cells; ++c) {
        const MeshCell& cell = mesh.cells.at(c);
        const Layer* metal = stack.named(cell.layer);
        if (metal == nullptr || !topOf.contains(metal->name) || !cell.onEdge) continue;
        const QPolygonF& outline = cell.outline;
        const QRectF box = outline.boundingRect();
        const double size = std::max(box.width(), box.height());
        for (int i = 0, n = int(outline.size()); i < n; ++i) {
            const QPointF a = outline.at(i), b = outline.at((i + 1) % n), d = b - a;
            const double length = std::hypot(d.x(), d.y());
            if (!(length > 0)) continue;
            QPointF outward(d.y() / length, -d.x() / length);
            const QPointF middle = (a + b) / 2;
            if (outline.containsPoint(middle + outward * (1e-3 * length), Qt::OddEvenFill)) outward = -outward;
            const QPointF probe = middle + outward * (1e-4 * size);
            bool covered = false;
            for (const Conductor& shape : conductors)
                if (shape.layer == cell.layer && shape.outline.boundingRect().contains(probe)
                    && shape.outline.containsPoint(probe, Qt::OddEvenFill)) {
                    covered = true;
                    break;
                }
            if (covered) continue;
            double deep = 0;   // (how far the cell reaches in from that side)
            for (const QPointF& p : outline) deep = std::max(deep, -((p - a).x() * outward.x() + (p - a).y() * outward.y()));
            const QPointF in = -outward * (0.1 * deep);
            MeshCell wall;
            wall.layer = wallsOf.value(metal->name);
            wall.outline = QPolygonF({a, b, b + in, a + in});
            wall.centre = middle + in / 2;
            wall.area = length * 0.1 * deep;
            wall.onEdge = true;
            out.wallOn.insert(int(out.mesh.cells.size()), c);
            out.mesh.cells << wall;
            ++out.walls;
        }
    }
    return out;
}


double wallExcess(double tau)
{
    // (t / S, and the excess capacitance per length over eps: the 2D
    //  solve's at S/32 and S/64, extrapolated linearly in the grid.)
    static const double points[][2] = {{0.0625, 0.0080}, {0.125, 0.0203}, {0.25, 0.0492}, {0.5, 0.1258},
                                       {1.0, 0.3489},    {2.0, 0.9806},   {4.0, 2.60}};
    constexpr int n = int(sizeof(points) / sizeof(points[0]));
    if (!(tau >= 1.0 / 32)) return 0;
    if (tau <= points[0][0]) return points[0][1] * std::pow(tau / points[0][0], 2);   // (as tau squared, in the limit)
    if (tau >= points[n - 1][0])
        return points[n - 1][1] + (tau - points[n - 1][0]) * (points[n - 1][1] - points[n - 2][1]) / (points[n - 1][0] - points[n - 2][0]);
    int i = 0;
    while (points[i + 1][0] < tau) ++i;
    const double f = std::log(tau / points[i][0]) / std::log(points[i + 1][0] / points[i][0]);
    return std::exp(std::log(points[i][1]) + f * (std::log(points[i + 1][1]) - std::log(points[i][1])));
}

QList<WallGap> wallGapsOf(const Thickened& thick, double micrometre)
{
    QList<WallGap> out;
    if (!thick.any || thick.wallOn.isEmpty()) return out;
    const double perUnit = micrometre > 0 ? micrometre : 1e-6;
    // Each wall: its metal, its side's line and extent, and which way it
    // faces (0 +x, 1 -x, 2 +y, 3 -y).
    struct Wall {
        int cell, on;
        QString metal;
        int facing;
        double line, lo, hi;
    };
    QHash<QString, QList<Wall>> byMetal;
    for (auto it = thick.wallOn.constBegin(); it != thick.wallOn.constEnd(); ++it) {
        const MeshCell& wall = thick.mesh.cells.at(it.key());
        if (wall.outline.size() < 4) continue;
        const QPointF a = wall.outline.at(0), b = wall.outline.at(1), in = wall.outline.at(2) - wall.outline.at(1);
        const QPointF d = b - a;
        const double len = std::hypot(d.x(), d.y());
        if (!(len > 0)) continue;
        const Layer* layer = thick.stack.named(wall.layer);
        if (layer == nullptr) continue;
        Wall w{it.key(), it.value(), layer->faceOf, -1, 0, 0, 0};
        if (std::abs(d.y()) <= 1e-9 * len) {   // along x: facing +y or -y
            w.facing = in.y() < 0 ? 2 : 3;
            w.line = a.y();
            w.lo = std::min(a.x(), b.x());
            w.hi = std::max(a.x(), b.x());
        } else if (std::abs(d.x()) <= 1e-9 * len) {   // along y: facing +x or -x
            w.facing = in.x() < 0 ? 0 : 1;
            w.line = a.x();
            w.lo = std::min(a.y(), b.y());
            w.hi = std::max(a.y(), b.y());
        } else {
            continue;   // (a slanted wall: none)
        }
        byMetal[w.metal] << w;
    }
    for (auto it = byMetal.constBegin(); it != byMetal.constEnd(); ++it) {
        const Layer* metal = thick.stack.named(it.key());
        if (metal == nullptr || !(metal->thickness > 0)) continue;
        const double t = metal->thickness / perUnit;   // mesh units
        // What fills the room beside the walls: the layer made for it.
        double epsr = 1;
        for (const Layer& l : thick.stack.layers)
            if (l.kind == Layer::Dielectric && l.faceOf == it.key()) epsr = l.epsr;
        const QList<Wall>& walls = it.value();
        for (const int axis : {0, 2}) {
            // Walls facing + on this axis, each with the nearest facing -.
            QList<const Wall*> plus, minus;
            for (const Wall& w : walls) {
                if (w.facing == axis) plus << &w;
                if (w.facing == axis + 1) minus << &w;
            }
            std::sort(minus.begin(), minus.end(), [](const Wall* x, const Wall* y) { return x->line < y->line; });
            for (const Wall* p : std::as_const(plus)) {
                const auto first = std::lower_bound(minus.begin(), minus.end(), p->line + 1e-9 * t,
                                                    [](const Wall* w, double v) { return w->line < v; });
                double nearest = -1;
                for (auto m = first; m != minus.end() && (*m)->line - p->line <= 16 * t; ++m) {
                    if (nearest >= 0 && (*m)->line > nearest + 1e-9 * t) break;
                    const double overlap = std::min(p->hi, (*m)->hi) - std::max(p->lo, (*m)->lo);
                    if (!(overlap > 1e-9 * t)) continue;
                    nearest = (*m)->line;
                    const double gap = (*m)->line - p->line;
                    const double excess = wallExcess(t / gap);
                    if (!(excess > 0)) continue;
                    WallGap g;
                    g.a = p->on;
                    g.b = (*m)->on;
                    g.gap = gap * perUnit;
                    g.length = overlap * perUnit;
                    g.capacitance = 8.8541878128e-12 * epsr * excess * g.length;   // (eps0)
                    g.epsr = epsr;
                    g.alongX = axis == 2;
                    g.lo = std::max(p->lo, (*m)->lo);
                    g.hi = std::min(p->hi, (*m)->hi);
                    out << g;
                }
            }
        }
    }
    return out;
}

} // namespace qucs_s::rf25
