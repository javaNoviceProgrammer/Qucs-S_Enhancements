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

#include <algorithm>
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
QList<double> gridLines(double from, double length, double step, bool gradeLow, bool gradeHigh)
{
    QList<double> at;
    const int n = step > 0 ? std::max(1, int(std::ceil(length / step - 1e-9))) : 1;
    const double d = length / n;
    at << from;
    if (gradeLow) at << from + d / 2;
    for (int i = 1; i < n; ++i) at << from + i * d;
    if (gradeHigh) at << from + length - d / 2;
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

/// The lines of \a mine and of \a theirs within [\a lo, \a hi] each made
/// the other's too: where two pieces meet along that stretch, so that
/// every cell of one there shares a whole edge with a cell of the other.
/// Whether \a mine or \a theirs gained any.
bool shareLines(QList<double>& mine, QList<double>& theirs, double lo, double hi, double tol)
{
    QList<double> both;
    for (const double x : std::as_const(mine))
        if (x >= lo - tol && x <= hi + tol) both << x;
    for (const double x : std::as_const(theirs))
        if (x >= lo - tol && x <= hi + tol) both << x;
    both << lo << hi;
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

MeshCost costOf(int unknowns)
{
    MeshCost cost;
    cost.unknowns = unknowns;
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
        QList<double> xs, ys;
    };
    QList<Piece> pieces;
    int cutInto = 0, cutFrom = 0;
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
    }
    if (cutFrom > 0)
        mesh.lines << tr("%1 shape(s) cut into the %2 rectangle(s) they are made of, each meshed as a strip of its own; "
                         "where two rectangles meet, their cells meet edge to edge.")
                          .arg(cutFrom).arg(cutInto);
    double narrowest = 0, finest = 0;
    int strips = 0;
    // The conductors cut with \a perFeature cells across each: false where
    // one of them would take more cells than a mesh may hold, \a many then
    // how many it would have taken.
    const auto cut = [&mesh, &pieces, &meetings, &conductors, &how, &narrowest, &finest, &strips, size](int perFeature,
                                                                                                         double* many) {
        mesh.cells.clear();
        narrowest = finest = 0;
        strips = 0;
        for (Piece& piece : pieces) {
            const Conductor& conductor = conductors.at(piece.conductor);
            const QRectF bounds = piece.box;
            const double narrow = std::min(bounds.width(), bounds.height());
            // How wide the conductor is where it is narrowest: a rectangle's
            // short side, else twice its area over its perimeter - which is
            // a trace's width for a trace of any shape, a spiral's included.
            const double area = piece.rectangle ? bounds.width() * bounds.height() : areaOf(conductor.outline);
            const double perimeter = piece.rectangle ? 0 : perimeterOf(conductor.outline);
            const double feature = piece.rectangle ? narrow : (perimeter > 0 ? 2 * area / perimeter : narrow);
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
            if (squares > 4.0 * kMostUnknowns) {
                *many = squares;
                return false;
            }
            piece.xs = gridLines(bounds.left(), bounds.width(), stepX, how.edges && piece.grade[0], how.edges && piece.grade[1]);
            piece.ys = gridLines(bounds.top(), bounds.height(), stepY, how.edges && piece.grade[2], how.edges && piece.grade[3]);
            if (narrowest <= 0 || feature < narrowest) narrowest = feature;
            if (finest <= 0 || across < finest) finest = across;
            if (strip) ++strips;
        }
        // Each meeting's lines made both pieces', until none gains any: a
        // line one takes may lie where it meets a third.
        QRectF all;
        for (const Piece& p : std::as_const(pieces)) all = all.united(p.box);
        const double tol = 1e-9 * std::max(1.0, std::max(all.width(), all.height()));
        bool gained = !meetings.isEmpty();
        for (int round = 0; gained && round < 64; ++round) {
            gained = false;
            for (const Meeting& m : std::as_const(meetings)) {
                Piece& a = pieces[m.a];
                Piece& b = pieces[m.b];
                gained = (m.alongX ? shareLines(a.xs, b.xs, m.lo, m.hi, tol) : shareLines(a.ys, b.ys, m.lo, m.hi, tol)) || gained;
            }
        }
        for (const Piece& piece : std::as_const(pieces)) {
            const Conductor& conductor = conductors.at(piece.conductor);
            const QList<double>& xs = piece.xs;
            const QList<double>& ys = piece.ys;
            if (double(xs.size() - 1) * double(ys.size() - 1) > 4.0 * kMostUnknowns) {
                *many = double(xs.size() - 1) * double(ys.size() - 1);
                return false;
            }
            for (int i = 0; i + 1 < xs.size(); ++i)
                for (int j = 0; j + 1 < ys.size(); ++j) {
                    const QRectF square(QPointF(xs.at(i), ys.at(j)), QPointF(xs.at(i + 1), ys.at(j + 1)));
                    const double full = square.width() * square.height();
                    const QList<QPolygonF> cut = piece.rectangle ? QList<QPolygonF>{QPolygonF(square)}
                                                                 : clipTo({conductor.outline}, QPolygonF(square));
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
    bool fits = cut(perFeature, &many) && many <= kMostCharges;
    while (!fits && perFeature > 1) {
        const double was = many;
        const int fewer = std::max(1, int(std::floor(perFeature * std::sqrt(double(kMostCharges) / std::max(1.0, many)))));
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
        fits = cutOk && many <= kMostCharges;
    }
    if (!fits && many > 4.0 * kMostUnknowns) {
        mesh.cells.clear();
        mesh.problems << tr("The layout would take %1 cells of %2 µm even at one across a conductor: ask for a larger "
                            "largest cell, or a lower top frequency.")
                             .arg(many, 0, 'g', 3).arg(size, 0, 'g', 3);
        return mesh;
    }
    if (perFeature < how.cellsAcross && fits)
        mesh.lines << tr("%1 cells across a conductor would be more than the %2 this build solves: %3 across instead.")
                          .arg(how.cellsAcross).arg(kMostCharges).arg(perFeature);
    else if (perFeature < how.cellsAcross)
        mesh.lines << tr("Fewer cells across a conductor do not bring the mesh within the %1 this build solves: the "
                         "largest cell, or the wavelength, sets the size there - ask for a larger largest cell.")
                          .arg(kMostCharges);
    if (perFeature > 0 && narrowest > 0)
        mesh.lines << tr("%1 cell(s) across the narrowest conductor (%2 µm across): cells of %3 µm there%4.")
                          .arg(perFeature).arg(narrowest, 0, 'g', 4).arg(finest, 0, 'g', 3)
                          .arg(strips > 0 ? tr(", up to %1 times as long along a straight strip").arg(how.stretch, 0, 'g', 3)
                                          : QString());
    if (how.edges) mesh.lines << tr("The outermost cells of each conductor's sides are half as wide, where its charge "
                                    "crowds.");

    // The unknowns: an edge two cells of a layer share is where current
    // flows from one to the other, and is one unknown of a rooftop basis.
    QHash<QString, int> shared;
    for (const MeshCell& cell : std::as_const(mesh.cells))
        for (int i = 0, n = cell.outline.size(); i < n; ++i)
            ++shared[cell.layer + QLatin1Char('|') + edgeKey(cell.outline.at(i), cell.outline.at((i + 1) % n))];
    int unknowns = 0;
    for (auto it = shared.constBegin(); it != shared.constEnd(); ++it)
        if (it.value() >= 2) ++unknowns;
    mesh.cost = costOf(unknowns);
    mesh.lines << tr("%1 cell(s) on %2 layer(s): %3. The %4 edge(s) between them are a full-wave solve's unknowns: %5.")
                      .arg(mesh.cells.size()).arg(mesh.byLayer().size()).arg(chargeCostOf(int(mesh.cells.size())))
                      .arg(unknowns).arg(mesh.cost.said);
    if (mesh.cost.tooBig)
        mesh.problems << mesh.cost.said;
    if (mesh.cells.size() > kMostCharges) mesh.problems << chargeCostOf(int(mesh.cells.size()));
    if (mesh.cells.isEmpty()) mesh.problems << tr("The mesh came out empty.");
    return mesh;
}

} // namespace qucs_s::rf25
