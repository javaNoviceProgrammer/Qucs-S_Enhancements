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
#include <gdstk/gdstk.hpp>
#include <gdstk/polygon.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QMap>

#include <algorithm>
#include <cmath>
#include <functional>

namespace qucs_s::rf25 {

namespace {

QString tr(const char* text, const char* disambiguation = nullptr, int n = -1)
{
    return QCoreApplication::translate("qucs_s::rf25", text, disambiguation, n);
}

/// The integer grid the boolean operations work on: the coordinates are
/// micrometres, and a nanometre is finer than any layout's database unit.
constexpr double kScaling = 1000.0;

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
    // (A Vec2's x and y are in an anonymous struct in a union: assigned,
    // not braced, which no brace count initializes without a warning.)
    for (const QPointF& point : polygon) {
        gdstk::Vec2 v;
        v.x = point.x();
        v.y = point.y();
        p.point_array.append(v);
    }
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

/// gdstk's boolean "or" of \a shapes alone; \a ok false where it failed.
static QList<QPolygonF> mergedOf(const QList<QPolygonF>& shapes, bool* ok)
{
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
    *ok = code == gdstk::ErrorCode::NoError;
    return *ok ? joined : shapes;
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

bool touchAlongAnEdge(const QPolygonF& a, const QPolygonF& b, double tol)
{
    const QRectF ra = a.boundingRect(), rb = b.boundingRect();
    if (!ra.adjusted(-tol, -tol, tol, tol).intersects(rb.adjusted(-tol, -tol, tol, tol))) return false;
    const int na = int(a.size()), nb = int(b.size());
    for (int i = 0; i < na; ++i) {
        const QPointF p = a.at(i), q = a.at((i + 1) % na);
        const QPointF d = q - p;
        const double len = std::hypot(d.x(), d.y());
        if (!(len > tol)) continue;
        const QPointF unit = d / len, normal(-unit.y(), unit.x());
        for (int j = 0; j < nb; ++j) {
            const QPointF u = b.at(j) - p, w = b.at((j + 1) % nb) - p;
            // On the same line, within tol of it at both ends...
            if (std::abs(u.x() * normal.x() + u.y() * normal.y()) > tol
                || std::abs(w.x() * normal.x() + w.y() * normal.y()) > tol)
                continue;
            // ... and sharing more than tol of it.
            const double tu = u.x() * unit.x() + u.y() * unit.y(), tw = w.x() * unit.x() + w.y() * unit.y();
            if (std::min(len, std::max(tu, tw)) - std::max(0.0, std::min(tu, tw)) > tol) return true;
        }
    }
    return false;
}

/// \a shapes joined where they touch or overlap: gdstk's boolean "or", and
/// then the pieces it left touching along an edge joined. Clipper's union
/// leaves two pieces that meet along part of an edge apart in some sets -
/// one in sixty of random sets of touching rectangles, a
/// low-pass filter among them, whose centre stub then lost its line in the
/// full-wave solve with no word said (B1 of the open items of 8 November).
/// Merging again joined only six in seven; each group of pieces that touch
/// along an edge is closed instead - grown by the union's own resolution,
/// united (now they overlap, which Clipper does join) and shrunk back by
/// as much -, which moves nothing by more than that resolution, a
/// nanometre. \a joined, given, counts the pieces so joined.
QList<QPolygonF> unionOf(const QList<QPolygonF>& shapes, int* joined)
{
    if (joined != nullptr) *joined = 0;
    if (shapes.size() < 2) return shapes;
    bool ok = false;
    QList<QPolygonF> out = mergedOf(shapes, &ok);
    if (!ok) return shapes;
    const double step = 1.0 / kScaling;
    for (int round = 0; round < 4 && out.size() > 1; ++round) {
        // Which pieces touch which along an edge, in groups.
        const int n = int(out.size());
        QList<int> parent(n);
        for (int i = 0; i < n; ++i) parent[i] = i;
        const std::function<int(int)> rootOf = [&parent, &rootOf](int i) {
            return parent.at(i) == i ? i : (parent[i] = rootOf(parent.at(i)));
        };
        bool any = false;
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j)
                if (rootOf(i) != rootOf(j) && touchAlongAnEdge(out.at(i), out.at(j), step / 2)) {
                    parent[rootOf(j)] = rootOf(i);
                    any = true;
                }
        if (!any) break;
        QMap<int, QList<QPolygonF>> groups;
        for (int i = 0; i < n; ++i) groups[rootOf(i)] << out.at(i);
        QList<QPolygonF> next;
        for (const QList<QPolygonF>& group : std::as_const(groups)) {
            if (group.size() < 2) {
                next += group;
                continue;
            }
            const QList<QPolygonF> closed = offsetOf(offsetOf(group, step), -step);
            if (closed.isEmpty() || closed.size() >= group.size()) {
                next += group;
                continue;
            }
            if (joined != nullptr) *joined += int(group.size() - closed.size());
            next += closed;
        }
        if (next.size() >= out.size()) break;
        out = next;
    }
    return out;
}

QList<QPolygonF> clipTo(const QList<QPolygonF>& shapes, const QPolygonF& within)
{
    if (shapes.isEmpty() || within.size() < 3) return {};
    std::vector<gdstk::Polygon> owned;
    owned.reserve(shapes.size());
    gdstk::Array<gdstk::Polygon*> in{};
    for (const QPolygonF& s : shapes) {
        owned.push_back(gdstkOf(s));
        in.append(&owned.back());
    }
    gdstk::Polygon box = gdstkOf(within);
    gdstk::Array<gdstk::Polygon*> out{};
    const gdstk::ErrorCode code = gdstk::boolean(in, box, gdstk::Operation::And, kScaling, out);
    QList<QPolygonF> kept;
    if (code == gdstk::ErrorCode::NoError)
        for (uint64_t i = 0; i < out.count; ++i) kept << polygonOf(*out[i]);
    freeAll(out);
    in.clear();
    box.clear();
    for (gdstk::Polygon& p : owned) p.clear();
    return kept;
}

namespace {

/// gdstk's boolean \a op of \a a with \a b; \a ok false where it failed.
QList<QPolygonF> booleanOf(const QList<QPolygonF>& a, const QList<QPolygonF>& b, gdstk::Operation op, bool* ok)
{
    std::vector<gdstk::Polygon> owned;
    owned.reserve(a.size() + b.size());
    gdstk::Array<gdstk::Polygon*> in1{}, in2{};
    for (const QPolygonF& s : a) {
        owned.push_back(gdstkOf(s));
        in1.append(&owned.back());
    }
    for (const QPolygonF& s : b) {
        owned.push_back(gdstkOf(s));
        in2.append(&owned.back());
    }
    gdstk::Array<gdstk::Polygon*> out{};
    const gdstk::ErrorCode code = gdstk::boolean(in1, in2, op, kScaling, out);
    QList<QPolygonF> result;
    if (code == gdstk::ErrorCode::NoError)
        for (uint64_t i = 0; i < out.count; ++i) result << polygonOf(*out[i]);
    freeAll(out);
    in1.clear();
    in2.clear();
    for (gdstk::Polygon& p : owned) p.clear();
    *ok = code == gdstk::ErrorCode::NoError;
    return result;
}

} // namespace

QList<QPolygonF> differenceOf(const QList<QPolygonF>& shapes, const QList<QPolygonF>& less)
{
    if (shapes.isEmpty()) return {};
    if (less.isEmpty()) return shapes;
    bool ok = false;
    const QList<QPolygonF> out = booleanOf(shapes, less, gdstk::Operation::Not, &ok);
    return ok ? out : QList<QPolygonF>();
}

namespace {

/// The model's parameters as a scope their expressions are read in.
fem::Scope scopeOf(const QList<fem::Model::Parameter>& parameters)
{
    fem::Scope scope(&fem::Scope::builtins());
    for (const fem::Model::Parameter& p : parameters) {
        const fem::Expression e = fem::Expression::compile(p.expression, scope);
        if (e.isValid() && e.isConstant()) scope.setConstant(p.name, e.constant(), e.dim());
    }
    return scope;
}

} // namespace

Cutout cutoutOf(const Model& model, QStringList* problems)
{
    Cutout out;
    const Node* node = model.layout().child(QStringLiteral("rf_cutout"));
    if (node == nullptr || !flagIn(*node, QStringLiteral("apply"), true)) return out;
    const fem::Scope scope = scopeOf(model.parameters());
    const Stack stack = model.stack();
    const auto say = [problems](const QString& text) {
        if (problems != nullptr) *problems << text;
    };
    for (const Node& r : node->children) {
        if (!r.enabled || r.type != QLatin1String("rf_region")) continue;
        const QString name = nameOf(r);
        // A length of it in µm, or nothing and why.
        bool readable = true;
        const auto length = [&](const QJsonValue& v, const QString& what) {
            const QString text = v.isDouble() ? QString::number(v.toDouble(), 'g', 17) : v.toString();
            const fem::Expression e = fem::Expression::compile(text.trimmed().isEmpty() ? QStringLiteral("0") : text, scope);
            if (!e.isValid() || !e.isConstant()) {
                if (readable)
                    say(tr("Cutout: %1's %2 is not a number (%3).")
                            .arg(name, what, e.isValid() ? tr("it is not a constant") : e.error()));
                readable = false;
                return 0.0;
            }
            return e.constant() / 1e-6;
        };
        const auto lengthOf = [&](const char* key) {
            return length(valueOf(r, QLatin1String(key)), QLatin1String(key));
        };
        CutRegion region;
        region.name = name;
        region.keep = textOf(r, QStringLiteral("action")) != QLatin1String("remove");
        region.layers = namesOf(r, QStringLiteral("layers"));
        const QString shape = textOf(r, QStringLiteral("shape"));
        if (shape == QLatin1String("polygon")) {
            const QJsonArray rows = valueOf(r, QStringLiteral("points")).toArray();
            for (int i = 0; i < rows.size(); ++i) {
                const QJsonArray cells = rows.at(i).toArray();
                const double x = length(cells.at(0), tr("corner %1's x").arg(i + 1));
                const double y = length(cells.at(1), tr("corner %1's y").arg(i + 1));
                if (region.outline.isEmpty() || region.outline.last() != QPointF(x, y)) region.outline << QPointF(x, y);
            }
            while (region.outline.size() > 1 && region.outline.last() == region.outline.first()) region.outline.removeLast();
            if (readable && region.outline.size() < 3) {
                say(tr("Cutout: %1 is a polygon of %2 corner(s): it wants three at least.")
                        .arg(name).arg(region.outline.size()));
                continue;
            }
        } else if (shape == QLatin1String("circle")) {
            const QPointF centre(lengthOf("x"), lengthOf("y"));
            const double radius = lengthOf("radius");
            if (readable && !(radius > 0)) {
                say(tr("Cutout: %1's radius is not a length above zero.").arg(name));
                continue;
            }
            // Its sides no further than a nanometre in from the circle.
            const double sagitta = 1e-3;
            const int sides = radius > 0 ? std::clamp(int(std::ceil(M_PI / std::sqrt(2 * sagitta / radius))), 32, 1024) : 0;
            for (int i = 0; i < sides; ++i) {
                const double a = 2 * M_PI * i / sides;
                region.outline << centre + radius * QPointF(std::cos(a), std::sin(a));
            }
        } else {
            const QPointF a(lengthOf("x0"), lengthOf("y0"));
            const QPointF b(lengthOf("x1"), lengthOf("y1"));
            region.outline = QPolygonF(QRectF(a, b).normalized());
            region.outline.removeLast();   // (QPolygonF of a rectangle closes its ring)
        }
        if (!readable) continue;
        if (!(areaOf(region.outline) > 0)) {
            say(tr("Cutout: %1 has no area.").arg(name));
            continue;
        }
        bool named = true;
        for (const QString& layer : std::as_const(region.layers))
            if (stack.named(layer) == nullptr) {
                say(tr("Cutout: %1 cuts %2, which is no layer of the stack.").arg(name, layer));
                named = false;
            }
        if (!named) continue;
        out.regions << region;
    }
    return out;
}

QList<OwnShape> ownShapesOf(const Model& model, QStringList* problems)
{
    QList<OwnShape> out;
    const Node* node = model.layout().child(QStringLiteral("rf_shapes"));
    if (node == nullptr) return out;
    const fem::Scope scope = scopeOf(model.parameters());
    const Stack stack = model.stack();
    const auto say = [problems](const QString& text) {
        if (problems != nullptr) *problems << text;
    };
    for (const Node& n : node->children) {
        if (!n.enabled || n.type != QLatin1String("rf_shape")) continue;
        const QString name = nameOf(n);
        // A length of it in µm, or nothing and why (the first thing wrong).
        bool readable = true;
        const auto length = [&](const QJsonValue& v, const QString& what) {
            const QString text = v.isDouble() ? QString::number(v.toDouble(), 'g', 17) : v.toString();
            const fem::Expression e = fem::Expression::compile(text.trimmed().isEmpty() ? QStringLiteral("0") : text, scope);
            if (!e.isValid() || !e.isConstant() || !std::isfinite(e.constant())) {
                if (readable)
                    say(tr("Shapes: %1's %2 is not a number (%3).")
                            .arg(name, what, !e.isValid() ? e.error() : tr("it is not a constant")));
                readable = false;
                return 0.0;
            }
            return e.constant() / 1e-6;
        };
        const auto lengthOf = [&](const char* key, const QString& what) { return length(valueOf(n, QLatin1String(key)), what); };
        OwnShape shape;
        shape.tag = n.tag;
        shape.name = name;
        shape.layer = textOf(n, QStringLiteral("layer")).trimmed();
        const Layer* layer = stack.named(shape.layer);
        if (layer == nullptr) {
            say(shape.layer.isEmpty() ? tr("Shapes: %1 is on no layer: name a metal or a via of the stack.").arg(name)
                                      : tr("Shapes: %1 is on %2, which is no layer of the stack.").arg(name, shape.layer));
            continue;
        }
        shape.layer = layer->name;
        if (layer->kind != Layer::Metal && layer->kind != Layer::Via && layer->kind != Layer::Dielectric && layer->kind != Layer::Substrate) {
            say(tr("Shapes: %1 is on %2, which is no metal, via, dielectric or substrate: a ground plane has no shapes.").arg(name, layer->name));
            continue;
        }
        const QString kind = textOf(n, QStringLiteral("shape"));
        const auto pointsOf = [&](const char* what) {
            QPolygonF points;
            const QJsonArray rows = valueOf(n, QStringLiteral("points")).toArray();
            for (int i = 0; i < rows.size(); ++i) {
                const QJsonArray cells = rows.at(i).toArray();
                const double x = length(cells.at(0), tr("%1 %2's x").arg(QLatin1String(what)).arg(i + 1));
                const double y = length(cells.at(1), tr("%1 %2's y").arg(QLatin1String(what)).arg(i + 1));
                if (points.isEmpty() || points.last() != QPointF(x, y)) points << QPointF(x, y);
            }
            return points;
        };
        if (kind == QLatin1String("polygon")) {
            shape.outline = pointsOf("corner");
            while (shape.outline.size() > 1 && shape.outline.last() == shape.outline.first()) shape.outline.removeLast();
            if (readable && shape.outline.size() < 3) {
                say(tr("Shapes: %1 is a polygon of %2 corner(s): it wants three at least.").arg(name).arg(shape.outline.size()));
                continue;
            }
        } else if (kind == QLatin1String("path")) {
            const QPolygonF spine = pointsOf("point");
            const double width = lengthOf("path_width", tr("width"));
            if (!readable) continue;
            if (spine.size() < 2) {
                say(tr("Shapes: %1 is a path of %2 point(s): it wants two at least.").arg(name).arg(spine.size()));
                continue;
            }
            if (!(width > 0)) {
                say(tr("Shapes: %1's width is not a length above zero.").arg(name));
                continue;
            }
            // Outlined as gdstk outlines a path, as the tab's Draw writes one.
            const QString ends = textOf(n, QStringLiteral("ends"));
            const auto vec = [](QPointF p) {
                gdstk::Vec2 v;
                v.x = p.x();
                v.y = p.y();
                return v;
            };
            gdstk::FlexPath path = {};
            path.num_elements = 1;
            path.elements = static_cast<gdstk::FlexPathElement*>(gdstk::allocate_clear(sizeof(gdstk::FlexPathElement)));
            path.init(vec(spine.first()), width, 0, 1e-3 * width, 0);
            path.simple_path = true;
            path.elements[0].end_type = ends == QLatin1String("round")      ? gdstk::EndType::Round
                                        : ends == QLatin1String("extended") ? gdstk::EndType::HalfWidth
                                                                            : gdstk::EndType::Flush;
            gdstk::Array<gdstk::Vec2> rest = {};
            for (int i = 1; i < spine.size(); ++i) rest.append(vec(spine.at(i)));
            path.segment(rest, nullptr, nullptr, false);
            rest.clear();
            gdstk::Array<gdstk::Polygon*> outlines = {};
            path.to_polygons(false, 0, outlines);
            for (uint64_t i = 0; i < outlines.count; ++i) {
                gdstk::Polygon* poly = outlines[i];
                QPolygonF outline;
                for (uint64_t k = 0; k < poly->point_array.count; ++k) outline << QPointF(poly->point_array[k].x, poly->point_array[k].y);
                if (outline.size() >= 3) {
                    OwnShape piece = shape;
                    piece.outline = outline;
                    out << piece;
                }
                poly->clear();
                gdstk::free_allocation(poly);
            }
            outlines.clear();
            path.clear();
            continue;
        } else if (kind == QLatin1String("via")) {
            const QPointF centre(lengthOf("x", QStringLiteral("x")), lengthOf("y", QStringLiteral("y")));
            const double side = lengthOf("size", tr("side"));
            if (!readable) continue;
            if (!(side > 0)) {
                say(tr("Shapes: %1's side is not a length above zero.").arg(name));
                continue;
            }
            shape.outline = QPolygonF(QRectF(centre - QPointF(side / 2, side / 2), QSizeF(side, side)));
            shape.outline.removeLast();
        } else {
            const QPointF at(lengthOf("x", QStringLiteral("x")), lengthOf("y", QStringLiteral("y")));
            const double w = lengthOf("width", tr("width")), h = lengthOf("height", tr("height"));
            if (!readable) continue;
            if (!(w > 0) || !(h > 0)) {
                say(tr("Shapes: %1's width and height are to be lengths above zero (%2 by %3 µm).").arg(name).arg(w).arg(h));
                continue;
            }
            const QPointF corner = textOf(n, QStringLiteral("anchor")) == QLatin1String("centre") ? at - QPointF(w / 2, h / 2) : at;
            shape.outline = QPolygonF(QRectF(corner, QSizeF(w, h)));
            shape.outline.removeLast();
        }
        if (!readable) continue;
        if (!(std::abs(areaOf(shape.outline)) > 0)) {
            say(tr("Shapes: %1 has no area.").arg(name));
            continue;
        }
        out << shape;
    }
    return out;
}

QList<Dimension> dimensionsOf(const Model& model, QStringList* problems)
{
    QList<Dimension> out;
    const fem::Scope scope = scopeOf(model.parameters());
    const Stack stack = model.stack();
    const auto say = [problems](const QString& text) {
        if (problems != nullptr) *problems << text;
    };
    for (const Node& n : model.layout().children) {
        if (!n.enabled || n.type != QLatin1String("rf_dimension")) continue;
        Dimension d;
        d.tag = n.tag;
        d.name = nameOf(n);
        bool readable = true;
        const auto number = [&](const char* key, double unit, const QString& what) {
            const QJsonValue v = valueOf(n, QLatin1String(key));
            const QString text = v.isDouble() ? QString::number(v.toDouble(), 'g', 17) : v.toString();
            const fem::Expression e = fem::Expression::compile(text.trimmed().isEmpty() ? QStringLiteral("0") : text, scope);
            if (!e.isValid() || !e.isConstant() || !std::isfinite(e.constant())) {
                if (readable)
                    say(tr("Dimensions: %1's %2 is not a number (%3).").arg(d.name, what, !e.isValid() ? e.error() : tr("it is not a constant")));
                readable = false;
                return 0.0;
            }
            return e.constant() / unit;
        };
        const QPointF a(number("x0", 1e-6, QStringLiteral("x0")), number("y0", 1e-6, QStringLiteral("y0")));
        const QPointF b(number("x1", 1e-6, QStringLiteral("x1")), number("y1", 1e-6, QStringLiteral("y1")));
        const double by = number("offset", 1e-6, tr("offset"));
        const QString direction = textOf(n, QStringLiteral("direction"));
        double angle = direction == QLatin1String("-x") ? 180 : direction == QLatin1String("+y") ? 90 : direction == QLatin1String("-y") ? 270 : 0;
        if (direction == QLatin1String("angle")) angle = number("angle", 1, tr("angle"));
        if (!readable) continue;
        d.box = QRectF(a, b).normalized();
        d.layer = textOf(n, QStringLiteral("layer")).trimmed();
        const Layer* layer = stack.named(d.layer);
        if (layer == nullptr) {
            say(d.layer.isEmpty() ? tr("Dimensions: %1 names no layer.").arg(d.name)
                                  : tr("Dimensions: %1 moves shapes of %2, which is no layer of the stack.").arg(d.name, d.layer));
            continue;
        }
        d.layer = layer->name;
        const double radians = angle * M_PI / 180;
        // (Along an axis exactly: a cosine of 90 degrees is not quite nought.)
        const double c = direction == QLatin1String("angle") ? std::cos(radians) : std::round(std::cos(radians));
        const double s = direction == QLatin1String("angle") ? std::sin(radians) : std::round(std::sin(radians));
        d.move = QPointF(c, s) * by;
        d.offset = textOf(n, QStringLiteral("offset"));
        out << d;
    }
    return out;
}

namespace {

/// Whether segments a-b and c-d meet.
bool segmentsMeet(QPointF a, QPointF b, QPointF c, QPointF d)
{
    const auto cross = [](QPointF o, QPointF p, QPointF q) { return (p.x() - o.x()) * (q.y() - o.y()) - (p.y() - o.y()) * (q.x() - o.x()); };
    const auto on = [](QPointF p, QPointF q, QPointF r) {
        return std::min(p.x(), r.x()) <= q.x() && q.x() <= std::max(p.x(), r.x()) && std::min(p.y(), r.y()) <= q.y() && q.y() <= std::max(p.y(), r.y());
    };
    const double d1 = cross(c, d, a), d2 = cross(c, d, b), d3 = cross(a, b, c), d4 = cross(a, b, d);
    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0))) return true;
    return (d1 == 0 && on(c, a, d)) || (d2 == 0 && on(c, b, d)) || (d3 == 0 && on(a, c, b)) || (d4 == 0 && on(a, d, b));
}

/// Whether \a ring's edges cross or touch, but for neighbours at their corner.
bool crossesItself(const QPolygonF& ring)
{
    const int n = int(ring.size());
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j) {
            if (j == i + 1 || (i == 0 && j == n - 1)) continue;
            if (segmentsMeet(ring.at(i), ring.at((i + 1) % n), ring.at(j), ring.at((j + 1) % n))) return true;
        }
    return false;
}

} // namespace

bool moveCorners(QPolygonF& outline, const QString& layer, const QList<Dimension>& dimensions, QString* why, QVector<int>* moved)
{
    QPolygonF ring = outline;
    if (ring.size() > 1 && ring.first() == ring.last()) ring.removeLast();
    bool any = false;
    QString by;
    for (int k = 0; k < dimensions.size(); ++k) {
        const Dimension& d = dimensions.at(k);
        if (d.layer != layer) continue;
        // (Its edges in it: a box drawn along an edge holds it, to a part in a
        //  billion of its size.)
        const double slack = 1e-9 * std::max({1.0, std::abs(d.box.width()), std::abs(d.box.height())});
        const QRectF box = d.box.adjusted(-slack, -slack, slack, slack);
        int count = 0;
        for (QPointF& p : ring)
            if (box.contains(p)) {
                p += d.move;
                ++count;
            }
        if (moved != nullptr && k < moved->size()) (*moved)[k] += count;
        if (count > 0) {
            any = true;
            by = d.name;
        }
    }
    if (!any) return true;
    if (crossesItself(ring) || !(std::abs(areaOf(ring)) > 0)) {
        if (why != nullptr)
            *why = tr("Dimensions: %1 would make a shape of %2 at (%3, %4) µm cross itself: it is left as it was.")
                       .arg(by, layer)
                       .arg(outline.boundingRect().center().x(), 0, 'g', 6)
                       .arg(outline.boundingRect().center().y(), 0, 'g', 6);
        return false;
    }
    outline = ring;
    return true;
}

QList<QPolygonF> cutOf(const QList<QPolygonF>& shapes, const Cutout& cutout, const QString& layer, int* cut, int* masked)
{
    if (cut != nullptr) *cut = 0;
    if (masked != nullptr) *masked = 0;
    QList<QPolygonF> keep, remove;
    QList<QRectF> keepBoxes, removeBoxes;
    for (const CutRegion& r : cutout.regions) {
        if (!r.cuts(layer)) continue;
        (r.keep ? keep : remove) << r.outline;
        (r.keep ? keepBoxes : removeBoxes) << r.outline.boundingRect();
    }
    if (keep.isEmpty() && remove.isEmpty()) return shapes;
    // (The regions that keep are one operand of the boolean, and so their
    //  union: a shape across two of them is kept whole - whichever way each
    //  is wound, since gdstk turns every polygon anticlockwise before its
    //  boolean.)
    const auto near = [](const QRectF& box, const QList<QRectF>& boxes) {
        for (const QRectF& b : boxes)
            if (box.intersects(b)) return true;
        return false;
    };
    QList<QPolygonF> out;
    out.reserve(shapes.size());
    for (const QPolygonF& s : shapes) {
        // QRectF::intersects() is false for a box of no width: a shape
        // drawn as a line (healed away later) is tested by its points.
        const QRectF box = s.boundingRect().adjusted(-1e-9, -1e-9, 1e-9, 1e-9);
        QList<QPolygonF> part{s};
        bool touched = false;
        if (!keep.isEmpty()) {
            if (!near(box, keepBoxes)) {
                if (masked != nullptr) ++*masked;
                continue;
            }
            bool ok = false;
            part = booleanOf(part, keep, gdstk::Operation::And, &ok);
            if (!ok) part = {s};
            touched = true;
        }
        if (!remove.isEmpty() && !part.isEmpty() && near(box, removeBoxes)) {
            bool ok = false;
            const QList<QPolygonF> left = booleanOf(part, remove, gdstk::Operation::Not, &ok);
            if (ok) part = left;
            touched = true;
        }
        if (!touched) {
            out << s;
            continue;
        }
        if (part.isEmpty()) {
            if (masked != nullptr) ++*masked;
            continue;
        }
        // A shape the regions hold whole is handed on as it was drawn - its
        // points and their order -, not as the boolean redrew it.
        double after = 0;
        for (const QPolygonF& p : std::as_const(part)) after += areaOf(p);
        // (Within what the boolean's grid moves it by: half a step at each
        //  side, along its whole boundary.)
        const double before = areaOf(s);
        if (part.size() == 1 && std::abs(after - before) <= perimeterOf(s) / kScaling) {
            out << s;
            continue;
        }
        if (cut != nullptr) ++*cut;
        out += part;
    }
    return out;
}

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
    if (!how.cutout.isEmpty())
        out.report.lines << tr("The cutout's %1 region(s) applied first.").arg(how.cutout.regions.size());
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
        // A dielectric's or the substrate's shapes are where it is: its
        // extent, beyond which it is something else (rf25_dielectric.h) -
        // not a conductor, and not processed as one.
        if (of->kind == Layer::Dielectric || of->kind == Layer::Substrate) {
            const QList<QPolygonF> joined = unionOf(shapes);
            out.extents[layer] += joined;
            out.report.lines << tr("%1: %2 shape(s) give it its extent - there only inside them, eps_r %3 beyond.")
                                    .arg(layer).arg(shapes.size()).arg(of->beyond);
            continue;
        }
        const bool isFill = fillLayers.contains(layer);
        if (isFill && how.removeFill) {
            out.report.fill += int(shapes.size());
            continue;
        }

        // The Cutout first: what it masks is never merged, healed or
        // meshed, which on a whole chip's layout is most of the work.
        if (!how.cutout.isEmpty()) {
            int cut = 0, masked = 0;
            shapes = cutOf(shapes, how.cutout, layer, &cut, &masked);
            out.report.cut += cut;
            out.report.masked += masked;
            if (cut > 0 || masked > 0)
                out.report.lines << tr("%1: the cutout cut %2 shape(s) along its regions' edges and masked %3 other(s).")
                                        .arg(layer).arg(cut).arg(masked);
            if (shapes.isEmpty()) continue;
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
            int touching = 0;
            shapes = unionOf(shapes, &touching);
            if (shapes.size() != before) {
                out.report.mergedFrom += before;
                out.report.mergedTo += int(shapes.size());
                out.report.lines << tr("%1: merged %2 shapes into %3.").arg(layer).arg(before).arg(shapes.size());
            }
            if (touching > 0)
                out.report.lines << tr("%1: %2 piece(s) the union had left touching along an edge joined to what they "
                                       "touch.").arg(layer).arg(touching);
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
        out.problems << (out.report.masked > 0
                             ? tr("The cutout's regions keep none of the shapes: nothing is left to solve on. Their lengths "
                                  "are the layout's coordinates, in metres: 30[um].")
                             : tr("Every shape was dropped: nothing is left to solve on."));
    return out;
}

Processed processRead(const Model& model, const layout::Layout& read, const QString& called)
{
    Processed out;
    // The cell: the one named, else the one a view would show.
    const QString wanted = model.layout().props.value(QLatin1String("cell")).toString().trimmed();
    const int cell = wanted.isEmpty() ? read.mainCell() : read.cellIndex(wanted);
    if (cell < 0) {
        out.problems << tr("%1 has no cell %2.").arg(called, wanted);
        return out;
    }

    const LayerMap map = layerMapOf(model);
    // The Dimensions: the file's corners they hold moved as the parameters say.
    QStringList moving;
    const QList<Dimension> dimensions = dimensionsOf(model, &moving);
    QVector<int> moved(dimensions.size(), 0);
    // Every shape of the cell and of what it places, by the stack layer
    // the map sends it to.
    QMap<QString, QList<QPolygonF>> byLayer;
    int ignored = 0;
    layout::Query query;
    query.cell = cell;
    query.labels = false;
    layout::search(read, query, [&](const layout::Found& found) {
        if (found.isText() || found.shape < 0) return true;
        const layout::LayerInfo& info = read.layers.at(read.cells.at(found.cell).shapes.at(found.shape).layer);
        const QString key = QStringLiteral("%1/%2").arg(info.key.layer).arg(info.key.datatype);
        const QString to = map.toLayer.value(key);
        if (to.isEmpty()) {
            ++ignored;
            return true;
        }
        QPolygonF outline = layout::outlineOf(read, found);
        if (!dimensions.isEmpty() && outline.size() >= 3) {
            QString why;
            if (!moveCorners(outline, to, dimensions, &why, &moved) && !moving.contains(why)) moving << why;
        }
        if (outline.size() >= 3) byLayer[to] << outline;
        return true;
    });

    QStringList said;
    // The model's own shapes, on their layers beside the file's.
    int own = 0;
    for (const OwnShape& s : ownShapesOf(model, &said)) {
        byLayer[s.layer] << s.outline;
        ++own;
    }
    Processing how = processingOf(*model.layout().child(QStringLiteral("rf_processing")), model.parameters(), &said);
    how.cutout = cutoutOf(model, &said);
    out = processShapes(byLayer, map.fill, how, model.stack(), 1e-6);
    if (own > 0) out.report.lines << tr("%n shape(s) of the model's own (Shapes) added to the layout's.", nullptr, own);
    for (int k = 0; k < dimensions.size(); ++k)
        out.report.lines << (moved.at(k) > 0 ? tr("%1 moved %n corner(s) of %2 by %3 µm.", nullptr, moved.at(k))
                                                   .arg(dimensions.at(k).name, dimensions.at(k).layer)
                                                   .arg(std::hypot(dimensions.at(k).move.x(), dimensions.at(k).move.y()), 0, 'g', 6)
                                             : tr("%1 moves no corner: its box holds none of %2's shapes of the layout file.")
                                                   .arg(dimensions.at(k).name, dimensions.at(k).layer));
    out.problems += moving;
    out.report.ignored = ignored;
    if (ignored > 0)
        out.report.lines << tr("%1 shape(s) are on layers the map does not name.").arg(ignored);
    out.problems += map.problems;
    out.problems += said;
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
    return processRead(model, *read, QFileInfo(path).fileName());
}

} // namespace qucs_s::rf25
