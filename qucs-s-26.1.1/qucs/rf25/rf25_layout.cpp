/*
 * rf25_layout.cpp - the layer map applied and the processing steps run
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "rf25_layout.h"

#include "layout.h"
#include "multiphysics/fem_expr.h"

#include <gdstk/clipper_tools.hpp>
#include <gdstk/polygon.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>

#include <algorithm>
#include <cmath>

namespace qucs_s::rf25 {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::rf25", text);
}

/// The integer grid the boolean operations work on: the coordinates are
/// micrometres, and a nanometre is finer than any layout's database unit.
constexpr double kScaling = 1000.0;

double areaOf(const QPolygonF& polygon)
{
    double twice = 0;
    for (int i = 0, n = polygon.size(); i < n; ++i) {
        const QPointF& a = polygon.at(i);
        const QPointF& b = polygon.at((i + 1) % n);
        twice += a.x() * b.y() - b.x() * a.y();
    }
    return std::abs(twice) / 2;
}

double perimeterOf(const QPolygonF& polygon)
{
    double length = 0;
    for (int i = 0, n = polygon.size(); i < n; ++i) {
        const QPointF d = polygon.at((i + 1) % n) - polygon.at(i);
        length += std::hypot(d.x(), d.y());
    }
    return length;
}

/// A polygon gdstk can work on. Its points are copied; free() releases them.
gdstk::Polygon gdstkOf(const QPolygonF& polygon)
{
    gdstk::Polygon p{};
    p.point_array.capacity = 0;
    p.point_array.count = 0;
    p.point_array.items = nullptr;
    for (const QPointF& point : polygon) p.point_array.append(gdstk::Vec2{point.x(), point.y()});
    return p;
}

QPolygonF polygonOf(const gdstk::Polygon& p)
{
    QPolygonF out;
    out.reserve(int(p.point_array.count));
    for (uint64_t i = 0; i < p.point_array.count; ++i)
        out << QPointF(p.point_array[i].x, p.point_array[i].y);
    return out;
}

void freeAll(gdstk::Array<gdstk::Polygon*>& polygons)
{
    for (uint64_t i = 0; i < polygons.count; ++i) {
        polygons[i]->clear();
        gdstk::free_allocation(polygons[i]);
    }
    polygons.clear();
}

/// \a shapes joined where they touch or overlap (gdstk's boolean "or").
QList<QPolygonF> unionOf(const QList<QPolygonF>& shapes)
{
    if (shapes.size() < 2) return shapes;
    std::vector<gdstk::Polygon> owned;
    owned.reserve(shapes.size());
    gdstk::Array<gdstk::Polygon*> in{};
    for (const QPolygonF& s : shapes) {
        owned.push_back(gdstkOf(s));
        in.append(&owned.back());
    }
    gdstk::Array<gdstk::Polygon*> out{};
    const gdstk::ErrorCode code = gdstk::merge(in, kScaling, out);
    QList<QPolygonF> joined;
    if (code == gdstk::ErrorCode::NoError)
        for (uint64_t i = 0; i < out.count; ++i) joined << polygonOf(*out[i]);
    freeAll(out);
    in.clear();
    for (gdstk::Polygon& p : owned) p.clear();
    return code == gdstk::ErrorCode::NoError ? joined : shapes;
}

/// \a shapes grown (or, for a negative distance, shrunk) by \a distance µm.
QList<QPolygonF> offsetOf(const QList<QPolygonF>& shapes, double distance)
{
    std::vector<gdstk::Polygon> owned;
    owned.reserve(shapes.size());
    gdstk::Array<gdstk::Polygon*> in{};
    for (const QPolygonF& s : shapes) {
        owned.push_back(gdstkOf(s));
        in.append(&owned.back());
    }
    gdstk::Array<gdstk::Polygon*> out{};
    const gdstk::ErrorCode code =
        gdstk::offset(in, distance, gdstk::OffsetJoin::Miter, 2, kScaling, true, out);
    QList<QPolygonF> moved;
    if (code == gdstk::ErrorCode::NoError)
        for (uint64_t i = 0; i < out.count; ++i) moved << polygonOf(*out[i]);
    freeAll(out);
    in.clear();
    for (gdstk::Polygon& p : owned) p.clear();
    return code == gdstk::ErrorCode::NoError ? moved : shapes;
}

/// How far apart two polygons are, by their bounding boxes: 0 where the
/// boxes meet. (A box is enough for clustering vias, which are convex and
/// small; two shapes whose boxes overlap are joined by the union anyway.)
double gapBetween(const QRectF& a, const QRectF& b)
{
    const double dx = std::max({a.left() - b.right(), b.left() - a.right(), 0.0});
    const double dy = std::max({a.top() - b.bottom(), b.top() - a.bottom(), 0.0});
    return std::hypot(dx, dy);
}

/// The smaller side of a polygon's bounding box - how a sliver is told
/// from a shape, which is exact for a rectangle and an over-estimate for
/// anything turned at an angle (said in the write-up).
double narrowness(const QPolygonF& polygon)
{
    const QRectF box = polygon.boundingRect();
    return std::min(box.width(), box.height());
}

double numberIn(const Node& node, const QString& key, const fem::Scope& scope, double fallback,
                QStringList* problems)
{
    const QJsonValue v = node.props.contains(key) ? node.props.value(key)
                       : nodeKind(node.type) != nullptr && nodeKind(node.type)->property(key) != nullptr
                             ? nodeKind(node.type)->property(key)->defaultValue
                             : QJsonValue();
    const QString text = v.isString() ? v.toString() : v.isDouble() ? QString::number(v.toDouble()) : QString();
    if (text.trimmed().isEmpty()) return fallback;
    const fem::Expression e = fem::Expression::compile(text, scope);
    if (!e.isValid() || !e.isConstant()) {
        if (problems != nullptr)
            *problems << tr("Processing: %1 is not a number (%2).").arg(key, e.isValid() ? tr("it is not a constant") : e.error());
        return fallback;
    }
    return e.constant();
}

bool flagIn(const Node& node, const QString& key, bool fallback)
{
    const QJsonValue v = node.props.contains(key) ? node.props.value(key)
                       : nodeKind(node.type) != nullptr && nodeKind(node.type)->property(key) != nullptr
                             ? nodeKind(node.type)->property(key)->defaultValue
                             : QJsonValue();
    if (v.isBool()) return v.toBool();
    if (v.isDouble()) return v.toDouble() != 0;
    if (v.isString()) return v.toString().compare(QLatin1String("true"), Qt::CaseInsensitive) == 0 || v.toString() == QLatin1String("1");
    return fallback;
}

} // namespace

double Conductor::area() const
{
    return areaOf(outline);
}

QString LayoutReport::summary() const
{
    return lines.join(QStringLiteral("; "));
}

QMap<QString, int> Processed::byLayer() const
{
    QMap<QString, int> counts;
    for (const Conductor& c : conductors) ++counts[c.layer];
    return counts;
}

Processing processingOf(const Node& node, const QList<fem::Model::Parameter>& parameters, QStringList* problems)
{
    fem::Scope scope(&fem::Scope::builtins());
    for (const fem::Model::Parameter& p : parameters) {
        const fem::Expression e = fem::Expression::compile(p.expression, scope);
        if (e.isValid() && e.isConstant()) scope.setConstant(p.name, e.constant(), e.dim());
    }
    Processing how;
    how.merge = flagIn(node, QStringLiteral("merge"), how.merge);
    how.mergeVias = flagIn(node, QStringLiteral("merge_vias"), how.mergeVias);
    how.keepArea = flagIn(node, QStringLiteral("keep_area"), how.keepArea);
    how.dropSlivers = flagIn(node, QStringLiteral("drop_slivers"), how.dropSlivers);
    how.snap = flagIn(node, QStringLiteral("snap"), how.snap);
    how.heal = flagIn(node, QStringLiteral("heal"), how.heal);
    how.removeFill = flagIn(node, QStringLiteral("remove_fill"), how.removeFill);
    how.viaDistance = numberIn(node, QStringLiteral("via_distance"), scope, how.viaDistance, problems);
    how.sliverWidth = numberIn(node, QStringLiteral("sliver_width"), scope, how.sliverWidth, problems);
    how.grid = numberIn(node, QStringLiteral("grid"), scope, how.grid, problems);
    if (how.viaDistance < 0 && problems != nullptr) *problems << tr("Processing: the via distance is below zero.");
    if (how.sliverWidth < 0 && problems != nullptr) *problems << tr("Processing: the sliver width is below zero.");
    if (how.grid <= 0 && how.snap && problems != nullptr) *problems << tr("Processing: the grid is not a length above zero.");
    return how;
}

LayerMap layerMapOf(const Model& model)
{
    LayerMap map;
    const Node* node = model.layout().child(QStringLiteral("rf_layer_map"));
    if (node == nullptr) return map;
    const Stack stack = model.stack();
    for (const QJsonValue& row : node->props.value(QLatin1String("rows")).toArray()) {
        const QJsonArray cells = row.toArray();
        const QString layer = cells.size() > 0 ? cells.at(0).toString().trimmed() : QString();
        const QString datatype = cells.size() > 1 ? cells.at(1).toString().trimmed() : QString();
        const QString to = cells.size() > 2 ? cells.at(2).toString().trimmed() : QString();
        const QString note = cells.size() > 3 ? cells.at(3).toString().trimmed() : QString();
        if (layer.isEmpty() || to.isEmpty()) continue;
        const QString key = QStringLiteral("%1/%2").arg(layer, datatype.isEmpty() ? QStringLiteral("0") : datatype);
        if (map.toLayer.contains(key))
            map.problems << tr("The layer map sends %1 to two layers of the stack (%2 and %3).")
                                .arg(key, map.toLayer.value(key), to);
        map.toLayer.insert(key, to);
        if (stack.named(to) == nullptr)
            map.problems << tr("The layer map sends %1 to %2, which is no layer of the stack.").arg(key, to);
        if (note.compare(QLatin1String("fill"), Qt::CaseInsensitive) == 0) map.fill.insert(to);
    }
    return map;
}

Processed processShapes(const QMap<QString, QList<QPolygonF>>& byLayer, const QSet<QString>& fillLayers,
                        const Processing& how, const Stack& stack, double micrometre)
{
    Processed out;
    // The settings are metres; the polygons are the layout's own units.
    const double perUnit = micrometre > 0 ? micrometre : 1e-6;
    const double viaDistance = how.viaDistance / perUnit;
    const double sliverWidth = how.sliverWidth / perUnit;
    const double grid = how.grid / perUnit;

    for (auto it = byLayer.constBegin(); it != byLayer.constEnd(); ++it) {
        const QString& layer = it.key();
        QList<QPolygonF> shapes = it.value();
        if (shapes.isEmpty()) continue;
        out.report.read += int(shapes.size());
        const Layer* of = stack.named(layer);
        if (of == nullptr) {
            out.problems << tr("%1 has shapes but is no layer of the stack.").arg(layer);
            continue;
        }
        const bool isFill = fillLayers.contains(layer);
        if (isFill && how.removeFill) {
            out.report.fill += int(shapes.size());
            continue;
        }

        // Heal first: a shape that crosses itself confuses every step
        // after it, and the union that heals it is the cheapest fix.
        if (how.heal) {
            QList<QPolygonF> healed;
            for (const QPolygonF& s : std::as_const(shapes)) {
                // A point repeated along the way is damage; the point that
                // closes the ring is how a polygon is written, and is
                // taken off without a word (QPolygonF of a rectangle has
                // one, so every shape would otherwise read as healed).
                QPolygonF tidy;
                int repeated = 0;
                for (const QPointF& p : s) {
                    if (tidy.isEmpty() || p != tidy.last()) tidy << p;
                    else ++repeated;
                }
                while (tidy.size() > 1 && tidy.last() == tidy.first()) tidy.removeLast();
                // United with itself, which is what undoes a crossing or a
                // zero-width neck.
                QList<QPolygonF> one = unionOf({tidy, tidy});
                if (one.isEmpty()) one = {tidy};
                if (repeated > 0 || one.size() != 1 || one.first().size() != tidy.size()) ++out.report.healed;
                healed += one;
            }
            shapes = healed;
        }

        // Snap to the grid, and drop what that leaves with no area.
        if (how.snap && grid > 0) {
            QList<QPolygonF> snapped;
            for (const QPolygonF& s : std::as_const(shapes)) {
                QPolygonF on;
                bool moved = false;
                for (const QPointF& p : s) {
                    const QPointF to(std::round(p.x() / grid) * grid, std::round(p.y() / grid) * grid);
                    moved = moved || to != p;
                    if (on.isEmpty() || on.last() != to) on << to;
                }
                if (moved) ++out.report.snapped;
                if (on.size() >= 3 && areaOf(on) > 0) snapped << on;
                else ++out.report.slivers;
            }
            shapes = snapped;
        }

        // A via layer's own step first: the vias of a farm are joined by
        // how close they are, not by touching.
        const bool vias = of->kind == Layer::Via;
        if (vias && how.mergeVias && shapes.size() > 1 && viaDistance > 0) {
            out.report.viasFrom += int(shapes.size());
            double wanted = 0;
            for (const QPolygonF& s : std::as_const(shapes)) wanted += areaOf(s);
            // A union joins what overlaps, and the vias of a farm do not
            // touch: each is grown by half the distance first, the union
            // then bridges every gap below it, and the result is shrunk
            // back by the same half. (A closing, and the only way a
            // distance can decide what joins what.)
            const QList<QPolygonF> grown = offsetOf(shapes, viaDistance / 2);
            QList<QPolygonF> one = offsetOf(unionOf(grown), -viaDistance / 2);
            if (one.isEmpty()) one = shapes;
            double after = 0;
            for (const QPolygonF& s : std::as_const(one)) after += areaOf(s);
            // Keep the metal's area: each step takes off (extra
            // area)/(perimeter), which overshoots on a shape with corners,
            // so it is repeated until it is within a per cent or it stops
            // helping.
            for (int step = 0; how.keepArea && step < 5 && after > wanted * 1.01 && !one.isEmpty(); ++step) {
                double around = 0;
                for (const QPolygonF& s : std::as_const(one)) around += perimeterOf(s);
                if (around <= 0) break;
                const QList<QPolygonF> smaller = offsetOf(one, -(after - wanted) / around);
                double shrunk = 0;
                for (const QPolygonF& s : std::as_const(smaller)) shrunk += areaOf(s);
                if (smaller.isEmpty() || shrunk < wanted * 0.98 || shrunk >= after) break;
                one = smaller;
                after = shrunk;
            }
            out.report.viasTo += int(one.size());
            out.report.areaKept = wanted > 0 ? after / wanted : 1;
            if (one.size() < shapes.size())
                out.report.lines << tr("%1: merged %2 vias into %3 shape(s)%4.")
                                        .arg(layer).arg(shapes.size()).arg(one.size())
                                        .arg(how.keepArea ? tr(", keeping %1% of their area")
                                                                .arg(std::round(out.report.areaKept * 1000) / 10)
                                                          : QString());
            shapes = one;
        } else if (how.merge && shapes.size() > 1) {
            // Every other layer: the metal of a layout is drawn as many
            // rectangles, and the solver wants one conductor.
            const int before = int(shapes.size());
            shapes = unionOf(shapes);
            if (shapes.size() != before) {
                out.report.mergedFrom += before;
                out.report.mergedTo += int(shapes.size());
                out.report.lines << tr("%1: merged %2 shapes into %3.").arg(layer).arg(before).arg(shapes.size());
            }
        }

        // Slivers: fill, assist shapes and the crumbs a boolean leaves.
        if (how.dropSlivers && sliverWidth > 0) {
            QList<QPolygonF> kept;
            int dropped = 0;
            for (const QPolygonF& s : std::as_const(shapes)) {
                if (narrowness(s) < sliverWidth || areaOf(s) < sliverWidth * sliverWidth) ++dropped;
                else kept << s;
            }
            if (dropped > 0) {
                out.report.slivers += dropped;
                out.report.lines << tr("%1: dropped %2 shape(s) thinner than %3 µm.")
                                        .arg(layer).arg(dropped).arg(sliverWidth);
            }
            shapes = kept;
        }

        for (const QPolygonF& s : std::as_const(shapes)) {
            Conductor c;
            c.layer = layer;
            c.outline = s;
            c.fill = isFill;
            out.conductors << c;
        }
    }

    if (how.snap && out.report.snapped > 0)
        out.report.lines << tr("Snapped %1 shape(s) to a %2 µm grid.").arg(out.report.snapped).arg(grid);
    if (out.report.healed > 0) out.report.lines << tr("Healed %1 shape(s).").arg(out.report.healed);
    if (out.report.fill > 0) out.report.lines << tr("Dropped %1 dummy fill shape(s).").arg(out.report.fill);
    if (out.report.ignored > 0)
        out.report.lines << tr("%1 shape(s) are on layers the map does not name.").arg(out.report.ignored);
    if (out.conductors.isEmpty() && out.report.read > 0)
        out.problems << tr("Every shape was dropped: nothing is left to solve on.");
    return out;
}

Processed processLayout(const Model& model, const QString& folder, QString* error)
{
    Processed out;
    const QString named = model.layout().props.value(QLatin1String("file")).toString().trimmed();
    if (named.isEmpty()) {
        out.problems << tr("The layout names no file: there are no shapes to solve on.");
        return out;
    }
    const QString path = QFileInfo(named).isAbsolute() ? named : QDir(folder).absoluteFilePath(named);
    if (!QFileInfo(path).isFile()) {
        if (error != nullptr) *error = tr("There is no file %1.").arg(QDir::toNativeSeparators(path));
        out.problems << tr("There is no file %1.").arg(QDir::toNativeSeparators(path));
        return out;
    }
    QString why;
    const std::shared_ptr<layout::Layout> read = layout::read(path, &why);
    if (!read) {
        if (error != nullptr) *error = why;
        out.problems << why;
        return out;
    }
    // The cell: the one named, else the one a view would show.
    const QString wanted = model.layout().props.value(QLatin1String("cell")).toString().trimmed();
    const int cell = wanted.isEmpty() ? read->mainCell() : read->cellIndex(wanted);
    if (cell < 0) {
        out.problems << tr("%1 has no cell %2.").arg(QFileInfo(path).fileName(), wanted);
        return out;
    }

    const LayerMap map = layerMapOf(model);
    out.problems += map.problems;
    // Every shape of the cell and of what it places, by the stack layer
    // the map sends it to.
    QMap<QString, QList<QPolygonF>> byLayer;
    int ignored = 0;
    layout::Query query;
    query.cell = cell;
    query.labels = false;
    layout::search(*read, query, [&](const layout::Found& found) {
        if (found.isText() || found.shape < 0) return true;
        const layout::LayerInfo& info = read->layers.at(read->cells.at(found.cell).shapes.at(found.shape).layer);
        const QString key = QStringLiteral("%1/%2").arg(info.key.layer).arg(info.key.datatype);
        const QString to = map.toLayer.value(key);
        if (to.isEmpty()) {
            ++ignored;
            return true;
        }
        const QPolygonF outline = layout::outlineOf(*read, found);
        if (outline.size() >= 3) byLayer[to] << outline;
        return true;
    });

    const Stack stack = model.stack();
    QStringList said;
    const Processing how = processingOf(*model.layout().child(QStringLiteral("rf_processing")), model.parameters(), &said);
    out = processShapes(byLayer, map.fill, how, stack, 1e-6);
    out.report.ignored = ignored;
    if (ignored > 0)
        out.report.lines << tr("%1 shape(s) are on layers the map does not name.").arg(ignored);
    out.problems += map.problems;
    out.problems += said;
    return out;
}

} // namespace qucs_s::rf25
