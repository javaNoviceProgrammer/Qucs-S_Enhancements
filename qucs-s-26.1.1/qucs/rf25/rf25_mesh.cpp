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

    // Each conductor cut against a grid of that size. A conductor smaller
    // than a cell is one cell, whatever the grid says.
    for (const Conductor& conductor : conductors) {
        // A conductor smaller than a cell needs no special case: the grid
        // below is one square, cut to its outline, which is the conductor
        // itself.
        const QRectF bounds = conductor.outline.boundingRect();
        const int across = std::max(1, int(std::ceil(bounds.width() / size)));
        const int up = std::max(1, int(std::ceil(bounds.height() / size)));
        // A conductor that would need more cells than the whole mesh may
        // hold is not cut at all: the count below says why.
        if (double(across) * double(up) > 4.0 * kMostUnknowns) {
            mesh.problems << tr("%1 would take %2 cells of %3 µm: give the mesh a larger cell, or the study a lower "
                                "frequency.").arg(conductor.layer).arg(double(across) * double(up), 0, 'g', 3)
                                 .arg(size, 0, 'g', 3);
            return mesh;
        }
        const double dx = bounds.width() / across, dy = bounds.height() / up;
        for (int i = 0; i < across; ++i)
            for (int j = 0; j < up; ++j) {
                const QRectF square(bounds.left() + i * dx, bounds.top() + j * dy, dx, dy);
                const QList<QPolygonF> pieces = clipTo({conductor.outline}, QPolygonF(square));
                for (const QPolygonF& piece : pieces) {
                    const double area = areaOf(piece);
                    if (area <= 0) continue;
                    MeshCell cell;
                    cell.layer = conductor.layer;
                    cell.outline = piece;
                    cell.centre = piece.boundingRect().center();
                    cell.area = area;
                    // On the edge where the cut took something off, or
                    // where the square touches the conductor's bounds.
                    cell.onEdge = std::abs(area - dx * dy) > dx * dy * 1e-6
                                  || i == 0 || j == 0 || i == across - 1 || j == up - 1;
                    mesh.cells << cell;
                }
            }
    }

    // Where the current crowds: the cells on a boundary divided once more.
    if (how.edges) {
        QList<MeshCell> refined;
        int divided = 0;
        for (const MeshCell& cell : std::as_const(mesh.cells)) {
            const QRectF box = cell.outline.boundingRect();
            if (!cell.onEdge || box.width() <= size / 4 || box.height() <= size / 4) {
                refined << cell;
                continue;
            }
            ++divided;
            for (int i = 0; i < 2; ++i)
                for (int j = 0; j < 2; ++j) {
                    const QRectF quarter(box.left() + i * box.width() / 2, box.top() + j * box.height() / 2,
                                         box.width() / 2, box.height() / 2);
                    for (const QPolygonF& piece : clipTo({cell.outline}, QPolygonF(quarter))) {
                        const double area = areaOf(piece);
                        if (area <= 0) continue;
                        MeshCell part = cell;
                        part.outline = piece;
                        part.centre = piece.boundingRect().center();
                        part.area = area;
                        refined << part;
                    }
                }
        }
        if (divided > 0) {
            mesh.lines << tr("Divided %1 cell(s) again at the conductors' edges.").arg(divided);
            mesh.cells = refined;
        }
    }

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
    mesh.lines << tr("%1 cell(s) on %2 layer(s); %3.")
                      .arg(mesh.cells.size()).arg(mesh.byLayer().size()).arg(mesh.cost.said);
    if (mesh.cost.tooBig)
        mesh.problems << mesh.cost.said;
    if (mesh.cells.isEmpty()) mesh.problems << tr("The mesh came out empty.");
    return mesh;
}

} // namespace qucs_s::rf25
