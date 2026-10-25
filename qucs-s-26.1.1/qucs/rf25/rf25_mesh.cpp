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
#include <cmath>

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
/// apart, evenly, and - where \a graded - the first and the last space
/// halved. A side shorter than a step is one space (two where graded).
QList<double> gridLines(double from, double length, double step, bool graded)
{
    QList<double> at;
    const int n = step > 0 ? std::max(1, int(std::ceil(length / step - 1e-9))) : 1;
    const double d = length / n;
    at << from;
    if (graded) at << from + d / 2;
    for (int i = 1; i < n; ++i) at << from + i * d;
    if (graded) at << from + length - d / 2;
    at << from + length;
    std::sort(at.begin(), at.end());
    // (One space graded at both ends is cut once, in its middle.)
    QList<double> out;
    for (const double x : std::as_const(at))
        if (out.isEmpty() || x - out.last() > 1e-9 * std::max(1.0, std::abs(length))) out << x;
    if (out.size() < 2) out = {from, from + length};
    return out;
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
    double narrowest = 0, finest = 0;
    int strips = 0;
    // The conductors cut with \a perFeature cells across each: false where
    // one of them would take more cells than a mesh may hold, \a many then
    // how many it would have taken.
    const auto cut = [&](int perFeature, double* many) {
        mesh.cells.clear();
        narrowest = finest = 0;
        strips = 0;
        for (const Conductor& conductor : conductors) {
            const QRectF bounds = conductor.outline.boundingRect();
            const double area = areaOf(conductor.outline);
            const double box = bounds.width() * bounds.height();
            const bool rectangle = box > 0 && std::abs(area - box) <= 1e-6 * box;
            const double narrow = std::min(bounds.width(), bounds.height());
            // How wide the conductor is where it is narrowest: a rectangle's
            // short side, else twice its area over its perimeter - which is
            // a trace's width for a trace of any shape, a spiral's included.
            const double perimeter = perimeterOf(conductor.outline);
            const double feature = rectangle ? narrow : (perimeter > 0 ? 2 * area / perimeter : narrow);
            double across = size;
            if (perFeature > 0 && feature > 0) across = std::min(across, feature / perFeature);
            const bool strip = rectangle && std::max(bounds.width(), bounds.height()) >= 3 * narrow;
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
            const QList<double> xs = gridLines(bounds.left(), bounds.width(), stepX, how.edges);
            const QList<double> ys = gridLines(bounds.top(), bounds.height(), stepY, how.edges);
            if (narrowest <= 0 || feature < narrowest) narrowest = feature;
            if (finest <= 0 || across < finest) finest = across;
            if (strip) ++strips;
            for (int i = 0; i + 1 < xs.size(); ++i)
                for (int j = 0; j + 1 < ys.size(); ++j) {
                    const QRectF square(QPointF(xs.at(i), ys.at(j)), QPointF(xs.at(i + 1), ys.at(j + 1)));
                    const double full = square.width() * square.height();
                    const QList<QPolygonF> pieces = rectangle ? QList<QPolygonF>{QPolygonF(square)}
                                                              : clipTo({conductor.outline}, QPolygonF(square));
                    for (const QPolygonF& piece : pieces) {
                        const double a = rectangle ? full : areaOf(piece);
                        if (a <= 0) continue;
                        MeshCell cell;
                        cell.layer = conductor.layer;
                        cell.outline = piece;
                        cell.centre = piece.boundingRect().center();
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
