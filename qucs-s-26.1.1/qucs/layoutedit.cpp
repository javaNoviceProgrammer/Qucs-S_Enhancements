/*
 * layoutedit.cpp - a layout changed in place, and what changed between
 *                  two reads of one (layoutedit.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "layoutedit.h"

#include "layoutrouter/router_gds.h"
#include "strokeshape.h"

#include <gdstk/gdstk.hpp>

#include <QFileInfo>
#include <QHash>
#include <QLineF>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <functional>

namespace qucs_s::layout {

namespace {

using markup::numberSaid;
using markup::pointSaid;

enum class Kind { Polygon, Path, Shape, Text, Placement };

bool kindNamed(const QString& name, Kind* kind)
{
    static const QHash<QString, Kind> kinds{{QStringLiteral("polygon"), Kind::Polygon}, {QStringLiteral("path"), Kind::Path},
                                            {QStringLiteral("shape"), Kind::Shape},     {QStringLiteral("text"), Kind::Text},
                                            {QStringLiteral("placement"), Kind::Placement}};
    if (!kinds.contains(name)) return false;
    *kind = kinds.value(name);
    return true;
}

/// Where an element is in its cell: which of its arrays, and where there.
enum class Holder { Polygons, FlexPaths, RobustPaths, Labels, References };

struct Element {
    Holder array = Holder::Polygons;
    uint64_t index = 0;
    double area = 0;    // µm²
    QRectF box;         // µm
    double distance = 0;
};

/// The edits' context: the library, the cell changed, and µm to the
/// library's user unit.
struct Edit {
    gdstk::Library* lib = nullptr;
    gdstk::Cell* cell = nullptr;
    double scale = 1;   // user units a µm

    gdstk::Vec2 user(QPointF p) const { return gdstk::Vec2{p.x() * scale, p.y() * scale}; }
    QPointF um(gdstk::Vec2 v) const { return QPointF(v.x / scale, v.y / scale); }
    QRectF box(gdstk::Vec2 min, gdstk::Vec2 max) const { return QRectF(um(min), um(max)).normalized(); }
};

bool layerOf(const QJsonValue& v, uint32_t* layer, uint32_t* type, QString* why)
{
    *type = 0;
    if (v.isDouble() && v.toDouble() >= 0 && v.toDouble() <= 1e9 && v.toDouble() == std::floor(v.toDouble())) {
        *layer = uint32_t(v.toDouble());
        return true;
    }
    const QStringList parts = v.toString().trimmed().split(QLatin1Char('/'));
    bool okLayer = false, okType = parts.size() == 1;
    if (parts.size() <= 2) {
        const qulonglong l = parts.at(0).toULongLong(&okLayer);
        okLayer = okLayer && l <= 0xffffffffULL;
        *layer = uint32_t(l);
        if (parts.size() == 2) {
            const qulonglong t = parts.at(1).toULongLong(&okType);
            okType = okType && t <= 0xffffffffULL;
            *type = uint32_t(t);
        }
    }
    if (okLayer && okType) return true;
    *why = QObject::tr("a 'layer' is \"layer/datatype\" (\"2/0\") or a layer's number");
    return false;
}

bool pointOf(const QJsonValue& v, QPointF* out)
{
    const QJsonArray xy = v.toArray();
    if (xy.size() != 2 || !xy.at(0).isDouble() || !xy.at(1).isDouble()) return false;
    const double x = xy.at(0).toDouble(), y = xy.at(1).toDouble();
    if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 1e9 || std::abs(y) > 1e9) return false;
    *out = QPointF(x, y);
    return true;
}

bool pointsOf(const QJsonValue& v, int least, QPolygonF* out)
{
    const QJsonArray a = v.toArray();
    if (!v.isArray() || a.size() < least || a.size() > 100000) return false;
    for (const QJsonValue& p : a) {
        QPointF q;
        if (!pointOf(p, &q)) return false;
        *out << q;
    }
    return true;
}

QString tagSaid(gdstk::Tag tag)
{
    return QStringLiteral("%1/%2").arg(gdstk::get_layer(tag)).arg(gdstk::get_type(tag));
}

QString boxSaid(const QRectF& b)
{
    return QStringLiteral("%1 to %2").arg(pointSaid(b.topLeft()), pointSaid(b.bottomRight()));
}

/// A path's polygons, for a test of a point and its box and area.
template <typename P>
void pathShape(const Edit& e, P* path, const std::function<void(const gdstk::Polygon&)>& each)
{
    gdstk::Array<gdstk::Polygon*> polys = {};
    path->to_polygons(false, 0, polys);
    for (uint64_t i = 0; i < polys.count; ++i) {
        each(*polys[i]);
        polys[i]->clear();
        gdstk::free_allocation(polys[i]);
    }
    polys.clear();
    (void)e;
}

gdstk::Tag pathTag(const gdstk::FlexPath* p) { return p->num_elements > 0 ? p->elements[0].tag : 0; }
gdstk::Tag pathTag(const gdstk::RobustPath* p) { return p->num_elements > 0 ? p->elements[0].tag : 0; }

/// Whether \a outer (the cell placed) places \a inner, itself or below.
bool places(const gdstk::Cell* outer, const gdstk::Cell* inner, QSet<const gdstk::Cell*>& seen)
{
    if (outer == inner) return true;
    if (seen.contains(outer)) return false;
    seen.insert(outer);
    for (uint64_t i = 0; i < outer->reference_array.count; ++i) {
        const gdstk::Reference* r = outer->reference_array[i];
        if (r->type == gdstk::ReferenceType::Cell && r->cell != nullptr && places(r->cell, inner, seen)) return true;
    }
    return false;
}

QString referenceName(const gdstk::Reference* r)
{
    if (r->type == gdstk::ReferenceType::Cell && r->cell != nullptr) return QString::fromUtf8(r->cell->name);
    if (r->type == gdstk::ReferenceType::RawCell && r->rawcell != nullptr) return QString::fromUtf8(r->rawcell->name);
    return QString::fromUtf8(r->name);
}

bool repeated(const gdstk::Repetition& r) { return r.type != gdstk::RepetitionType::None; }

class Editor
{
public:
    explicit Editor(Edit e) : a_e(e) {}

    /// One edit applied; false, and why, when it cannot be.
    bool apply(const QJsonObject& edit, QString* done, QString* why);

private:
    bool pick(Kind kind, const QJsonObject& e, bool one, QList<Element>* out, QString* why);
    gdstk::FlexPath* newPath(const QPolygonF& points, double width, gdstk::EndType ends, gdstk::Tag tag) const;
    void remove(const Element& el);
    QString describe(const Element& el) const;
    bool endsOf(const QJsonValue& v, gdstk::EndType* ends, QString* why) const;

    Edit a_e;
};

bool Editor::endsOf(const QJsonValue& v, gdstk::EndType* ends, QString* why) const
{
    const QString s = v.toString(QStringLiteral("flush"));
    if (s == QLatin1String("flush")) *ends = gdstk::EndType::Flush;
    else if (s == QLatin1String("round")) *ends = gdstk::EndType::Round;
    else if (s == QLatin1String("extended")) *ends = gdstk::EndType::HalfWidth;
    else {
        *why = QObject::tr("a path's 'ends' are flush, round or extended (by half its width)");
        return false;
    }
    return true;
}

gdstk::FlexPath* Editor::newPath(const QPolygonF& points, double width, gdstk::EndType ends, gdstk::Tag tag) const
{
    auto* p = static_cast<gdstk::FlexPath*>(gdstk::allocate_clear(sizeof(gdstk::FlexPath)));
    p->num_elements = 1;
    p->elements = static_cast<gdstk::FlexPathElement*>(gdstk::allocate_clear(sizeof(gdstk::FlexPathElement)));
    p->init(a_e.user(points.first()), width * a_e.scale, 0, 0.01 * a_e.scale, tag);
    p->simple_path = true;
    p->scale_width = true;
    p->elements[0].end_type = ends;
    gdstk::Array<gdstk::Vec2> rest = {};
    for (int i = 1; i < points.size(); ++i) rest.append(a_e.user(points.at(i)));
    p->segment(rest, nullptr, nullptr, false);
    rest.clear();
    return p;
}

QString Editor::describe(const Element& el) const
{
    gdstk::Cell* c = a_e.cell;
    switch (el.array) {
    case Holder::Polygons: return QObject::tr("the polygon on %1 (its box %2)").arg(tagSaid(c->polygon_array[el.index]->tag), boxSaid(el.box));
    case Holder::FlexPaths: return QObject::tr("the path on %1 (its box %2)").arg(tagSaid(pathTag(c->flexpath_array[el.index])), boxSaid(el.box));
    case Holder::RobustPaths: return QObject::tr("the path on %1 (its box %2)").arg(tagSaid(pathTag(c->robustpath_array[el.index])), boxSaid(el.box));
    case Holder::Labels: {
        const gdstk::Label* l = c->label_array[el.index];
        return QObject::tr("the text %1 on %2 at %3").arg(QString::fromUtf8(l->text), tagSaid(l->tag), pointSaid(a_e.um(l->origin)));
    }
    case Holder::References: {
        const gdstk::Reference* r = c->reference_array[el.index];
        return QObject::tr("the placement of %1 at %2").arg(referenceName(r), pointSaid(a_e.um(r->origin)));
    }
    }
    return QString();
}

void Editor::remove(const Element& el)
{
    gdstk::Cell* c = a_e.cell;
    switch (el.array) {
    case Holder::Polygons:
        c->polygon_array[el.index]->clear();
        gdstk::free_allocation(c->polygon_array[el.index]);
        c->polygon_array.remove(el.index);
        break;
    case Holder::FlexPaths:
        c->flexpath_array[el.index]->clear();
        gdstk::free_allocation(c->flexpath_array[el.index]);
        c->flexpath_array.remove(el.index);
        break;
    case Holder::RobustPaths:
        c->robustpath_array[el.index]->clear();
        gdstk::free_allocation(c->robustpath_array[el.index]);
        c->robustpath_array.remove(el.index);
        break;
    case Holder::Labels:
        c->label_array[el.index]->clear();
        gdstk::free_allocation(c->label_array[el.index]);
        c->label_array.remove(el.index);
        break;
    case Holder::References:
        c->reference_array[el.index]->clear();
        gdstk::free_allocation(c->reference_array[el.index]);
        c->reference_array.remove(el.index);
        break;
    }
}

bool Editor::pick(Kind kind, const QJsonObject& e, bool one, QList<Element>* out, QString* why)
{
    gdstk::Cell* c = a_e.cell;
    const bool hasAt = e.contains(QLatin1String("at")), hasBox = e.contains(QLatin1String("box"));
    QPointF at;
    QRectF box;
    if (hasAt && !pointOf(e.value(QLatin1String("at")), &at)) {
        *why = QObject::tr("'at' is [x, y] in µm");
        return false;
    }
    if (hasBox) {
        const QJsonArray b = e.value(QLatin1String("box")).toArray();
        QPointF p, q;
        if (b.size() != 4 || !pointOf(QJsonArray{b.at(0), b.at(1)}, &p) || !pointOf(QJsonArray{b.at(2), b.at(3)}, &q)) {
            *why = QObject::tr("'box' is [x0, y0, x1, y1] in µm");
            return false;
        }
        box = QRectF(p, q).normalized();
    }
    const bool byText = kind == Kind::Text && e.contains(QLatin1String("text"));
    const bool byCell = kind == Kind::Placement && e.contains(QLatin1String("of"));
    if (hasAt == hasBox && !(byText || byCell) ) {
        *why = hasAt ? QObject::tr("'at' or 'box' picks what is changed, not both")
                     : QObject::tr("what is changed is picked by 'at' [x, y] (the smallest there) or 'box' [x0, y0, x1, y1] (those wholly inside)");
        return false;
    }
    if (hasAt && hasBox) {
        *why = QObject::tr("'at' or 'box' picks what is changed, not both");
        return false;
    }
    bool layered = false;
    uint32_t layer = 0, type = 0;
    if (e.contains(QLatin1String("layer"))) {
        if (!layerOf(e.value(QLatin1String("layer")), &layer, &type, why)) return false;
        layered = true;
    } else if (kind != Kind::Placement && kind != Kind::Text) {
        *why = QObject::tr("a shape is picked on its 'layer'");
        return false;
    }
    const gdstk::Tag tag = gdstk::make_tag(layer, type);
    const QString text = e.value(QLatin1String("text")).toString();
    const QString of = e.value(QLatin1String("of")).toString();
    QList<Element> found;
    const auto consider = [&](Element el, const std::function<bool(QPointF)>& contains) {
        if (hasBox && !box.contains(el.box)) return;
        if (hasAt && !contains(at)) return;
        found << el;
    };
    if (kind == Kind::Polygon || kind == Kind::Shape)
        for (uint64_t i = 0; i < c->polygon_array.count; ++i) {
            gdstk::Polygon* p = c->polygon_array[i];
            if (p->tag != tag) continue;
            gdstk::Vec2 lo, hi;
            p->bounding_box(lo, hi);
            Element el{Holder::Polygons, i, std::abs(p->area()) / (a_e.scale * a_e.scale), a_e.box(lo, hi)};
            consider(el, [&](QPointF q) { return p->contain(a_e.user(q)); });
        }
    if (kind == Kind::Path || kind == Kind::Shape) {
        const auto paths = [&](auto& array, Holder which) {
            for (uint64_t i = 0; i < array.count; ++i) {
                auto* p = array[i];
                if (pathTag(p) != tag) continue;
                Element el{which, i, 0, QRectF()};
                bool in = false;
                pathShape(a_e, p, [&](const gdstk::Polygon& poly) {
                    gdstk::Vec2 lo, hi;
                    poly.bounding_box(lo, hi);
                    el.box = el.box.isNull() ? a_e.box(lo, hi) : el.box.united(a_e.box(lo, hi));
                    el.area += std::abs(poly.area()) / (a_e.scale * a_e.scale);
                    if (hasAt && poly.contain(a_e.user(at))) in = true;
                });
                consider(el, [&](QPointF) { return in; });
            }
        };
        paths(c->flexpath_array, Holder::FlexPaths);
        paths(c->robustpath_array, Holder::RobustPaths);
    }
    if (kind == Kind::Text)
        for (uint64_t i = 0; i < c->label_array.count; ++i) {
            gdstk::Label* l = c->label_array[i];
            if (layered && l->tag != tag) continue;
            if (byText && QString::fromUtf8(l->text) != text) continue;
            const QPointF origin = a_e.um(l->origin);
            Element el{Holder::Labels, i, 0, QRectF(origin, origin)};
            el.distance = hasAt ? QLineF(origin, at).length() : 0;
            if (hasBox && !box.contains(origin)) continue;
            if (hasAt && el.distance > 1.0) continue;
            found << el;
        }
    if (kind == Kind::Placement)
        for (uint64_t i = 0; i < c->reference_array.count; ++i) {
            gdstk::Reference* r = c->reference_array[i];
            if (byCell && referenceName(r) != of) continue;
            gdstk::Vec2 lo, hi;
            r->bounding_box(lo, hi);
            Element el{Holder::References, i, 0, a_e.box(lo, hi)};
            el.area = el.box.width() * el.box.height();
            consider(el, [&](QPointF q) { return el.box.contains(q) || QLineF(q, a_e.um(r->origin)).length() <= 1e-3; });
        }
    if (found.isEmpty()) {
        QString what = kind == Kind::Polygon ? QObject::tr("polygon") : kind == Kind::Path ? QObject::tr("path") : kind == Kind::Shape ? QObject::tr("shape")
                       : kind == Kind::Text ? QObject::tr("text") : QObject::tr("placement");
        if (layered) what += QObject::tr(" on %1").arg(tagSaid(tag));
        if (byText) what += QObject::tr(" saying %1").arg(text);
        if (byCell) what += QObject::tr(" of %1").arg(of);
        *why = QObject::tr("%1 has no %2 %3 (its own elements, in its own µm: a shape of a cell it places is that cell's to change)")
                   .arg(QString::fromUtf8(c->name), what,
                        hasAt ? QObject::tr("at %1").arg(pointSaid(at)) : hasBox ? QObject::tr("wholly inside %1").arg(boxSaid(box)) : QString());
        return false;
    }
    // At a point, the smallest there (a text, the nearest); in a box, all.
    if (hasAt || one) {
        std::stable_sort(found.begin(), found.end(), [](const Element& a, const Element& b) {
            return a.distance != b.distance ? a.distance < b.distance : a.area < b.area;
        });
        if (one && !hasAt && found.size() > 1) {
            *why = QObject::tr("%1 things are picked; one is replaced or moved at a time - 'at' picks the smallest at a point").arg(found.size());
            return false;
        }
        found = found.mid(0, 1);
    }
    for (const Element& el : std::as_const(found)) {
        const bool rep = el.array == Holder::Polygons      ? repeated(c->polygon_array[el.index]->repetition)
                         : el.array == Holder::FlexPaths   ? repeated(c->flexpath_array[el.index]->repetition)
                         : el.array == Holder::RobustPaths ? repeated(c->robustpath_array[el.index]->repetition)
                         : el.array == Holder::Labels      ? repeated(c->label_array[el.index]->repetition)
                                                          : repeated(c->reference_array[el.index]->repetition);
        if (rep) {
            *why = QObject::tr("%1 is repeated (an array): edit_layout changes single elements").arg(describe(el));
            return false;
        }
    }
    *out = found;
    return true;
}

bool Editor::apply(const QJsonObject& edit, QString* done, QString* why)
{
    static const QSet<QString> known{QStringLiteral("add"),      QStringLiteral("delete"),   QStringLiteral("replace"), QStringLiteral("move"),
                                     QStringLiteral("layer"),    QStringLiteral("points"),   QStringLiteral("width"),   QStringLiteral("ends"),
                                     QStringLiteral("text"),     QStringLiteral("at"),       QStringLiteral("of"),      QStringLiteral("rotation"),
                                     QStringLiteral("mirrored"), QStringLiteral("magnification"), QStringLiteral("box"), QStringLiteral("by"),
                                     QStringLiteral("layer_to"), QStringLiteral("text_to"),  QStringLiteral("at_to")};
    for (auto it = edit.begin(); it != edit.end(); ++it)
        if (!known.contains(it.key())) {
            *why = QObject::tr("'%1' is not a key of an edit").arg(it.key());
            return false;
        }
    QString op;
    for (const char* name : {"add", "delete", "replace", "move"})
        if (edit.contains(QLatin1String(name))) {
            if (!op.isEmpty()) {
                *why = QObject::tr("an edit is one of add, delete, replace or move");
                return false;
            }
            op = QLatin1String(name);
        }
    Kind kind = Kind::Shape;
    if (op.isEmpty() || !kindNamed(edit.value(op).toString(), &kind)) {
        *why = QObject::tr("an edit is {\"add\" | \"delete\" | \"replace\" | \"move\": polygon, path, shape, text or placement, ...}");
        return false;
    }
    // Only the keys an edit of its kind reads.
    const auto only = [&](std::initializer_list<const char*> keys) {
        QSet<QString> allowed{op};
        for (const char* k : keys) allowed.insert(QLatin1String(k));
        for (auto it = edit.begin(); it != edit.end(); ++it)
            if (!allowed.contains(it.key())) {
                *why = QObject::tr("'%1' is not of %2 %3").arg(it.key(), op, edit.value(op).toString());
                return false;
            }
        return true;
    };
    gdstk::Cell* c = a_e.cell;
    const QString cellName = QString::fromUtf8(c->name);
    uint32_t layer = 0, type = 0;
    if (op == QLatin1String("add")) {
        if (kind == Kind::Shape) {
            *why = QObject::tr("what is added is a polygon, a path, a text or a placement");
            return false;
        }
        if (kind != Kind::Placement && !layerOf(edit.value(QLatin1String("layer")), &layer, &type, why)) return false;
        const gdstk::Tag tag = gdstk::make_tag(layer, type);
        QPolygonF points;
        switch (kind) {
        case Kind::Polygon: {
            if (!only({"layer", "points"})) return false;
            if (!pointsOf(edit.value(QLatin1String("points")), 3, &points)) {
                *why = QObject::tr("a polygon's 'points' are [[x, y], ...] in µm, 3 at least");
                return false;
            }
            auto* p = static_cast<gdstk::Polygon*>(gdstk::allocate_clear(sizeof(gdstk::Polygon)));
            p->tag = tag;
            for (const QPointF& q : std::as_const(points)) p->point_array.append(a_e.user(q));
            if (!(std::abs(p->area()) > 0)) {
                p->clear();
                gdstk::free_allocation(p);
                *why = QObject::tr("the polygon's points enclose nothing");
                return false;
            }
            c->polygon_array.append(p);
            *done = QObject::tr("added a polygon on %1 to %2: %3 points, its box %4").arg(tagSaid(tag), cellName).arg(points.size()).arg(boxSaid(points.boundingRect()));
            return true;
        }
        case Kind::Path: {
            if (!only({"layer", "points", "width", "ends"})) return false;
            const double width = edit.value(QLatin1String("width")).toDouble(-1);
            gdstk::EndType ends;
            if (!pointsOf(edit.value(QLatin1String("points")), 2, &points)) {
                *why = QObject::tr("a path's 'points' are its spine, [[x, y], ...] in µm, 2 at least");
                return false;
            }
            if (!(width > 0) || width > 1e6) {
                *why = QObject::tr("a path has a 'width' in µm, more than 0");
                return false;
            }
            if (!endsOf(edit.value(QLatin1String("ends")), &ends, why)) return false;
            c->flexpath_array.append(newPath(points, width, ends, tag));
            *done = QObject::tr("added a path on %1 to %2: %3 points from %4 to %5, %6 µm wide")
                        .arg(tagSaid(tag), cellName)
                        .arg(points.size())
                        .arg(pointSaid(points.first()), pointSaid(points.last()), numberSaid(width));
            return true;
        }
        case Kind::Text: {
            if (!only({"layer", "text", "at"})) return false;
            QPointF at;
            const QString words = edit.value(QLatin1String("text")).toString();
            if (!pointOf(edit.value(QLatin1String("at")), &at) || words.isEmpty() || words.size() > 1000) {
                *why = QObject::tr("a text has its 'text' and 'at' [x, y] in µm");
                return false;
            }
            auto* l = static_cast<gdstk::Label*>(gdstk::allocate_clear(sizeof(gdstk::Label)));
            l->init(words.toUtf8().constData());
            l->origin = a_e.user(at);
            l->tag = tag;
            c->label_array.append(l);
            *done = QObject::tr("added the text %1 on %2 to %3 at %4").arg(words, tagSaid(tag), cellName, pointSaid(at));
            return true;
        }
        case Kind::Placement: {
            if (!only({"of", "at", "rotation", "mirrored", "magnification"})) return false;
            QPointF at;
            const QString of = edit.value(QLatin1String("of")).toString();
            gdstk::Cell* placed = a_e.lib->get_cell(of.toUtf8().constData());
            if (placed == nullptr) {
                *why = QObject::tr("the layout has no cell %1 to place").arg(of);
                return false;
            }
            QSet<const gdstk::Cell*> seen;
            if (places(placed, c, seen)) {
                *why = QObject::tr("%1 placed in %2 would place %2 in itself").arg(of, cellName);
                return false;
            }
            if (!pointOf(edit.value(QLatin1String("at")), &at)) {
                *why = QObject::tr("a placement has 'at' [x, y] in µm");
                return false;
            }
            const double magnification = edit.value(QLatin1String("magnification")).toDouble(1);
            if (!(magnification > 0) || magnification > 1e6) {
                *why = QObject::tr("a placement's 'magnification' is more than 0");
                return false;
            }
            auto* r = static_cast<gdstk::Reference*>(gdstk::allocate_clear(sizeof(gdstk::Reference)));
            r->init(placed);
            r->origin = a_e.user(at);
            r->rotation = edit.value(QLatin1String("rotation")).toDouble(0) * M_PI / 180;
            r->x_reflection = edit.value(QLatin1String("mirrored")).toBool(false);
            r->magnification = magnification;
            c->reference_array.append(r);
            *done = QObject::tr("placed %1 in %2 at %3").arg(of, cellName, pointSaid(at));
            return true;
        }
        case Kind::Shape: break;
        }
        return false;
    }

    // What is deleted, replaced or moved: picked.
    QList<Element> picked;
    if (op == QLatin1String("delete")) {
        if (!only({"layer", "at", "box", "text", "of"})) return false;
        if (!pick(kind, edit, false, &picked, why)) return false;
        QStringList said;
        for (const Element& el : std::as_const(picked)) said << describe(el);
        // (Each array's latest first, so the others keep their places.)
        std::sort(picked.begin(), picked.end(), [](const Element& a, const Element& b) { return a.array != b.array ? a.array < b.array : a.index > b.index; });
        for (const Element& el : std::as_const(picked)) remove(el);
        *done = QObject::tr("deleted from %1: %2").arg(cellName, said.join(QStringLiteral("; ")));
        return true;
    }
    if (op == QLatin1String("move")) {
        if (!only({"layer", "at", "box", "text", "of", "by"})) return false;
        QPointF by;
        if (!pointOf(edit.value(QLatin1String("by")), &by)) {
            *why = QObject::tr("a move is 'by' [dx, dy] in µm");
            return false;
        }
        if (!pick(kind, edit, true, &picked, why)) return false;
        const Element& el = picked.first();
        const QString what = describe(el);
        const gdstk::Vec2 d = a_e.user(by);
        switch (el.array) {
        case Holder::Polygons: c->polygon_array[el.index]->translate(d); break;
        case Holder::FlexPaths: c->flexpath_array[el.index]->translate(d); break;
        case Holder::RobustPaths: c->robustpath_array[el.index]->translate(d); break;
        case Holder::Labels: c->label_array[el.index]->origin += d; break;
        case Holder::References: c->reference_array[el.index]->origin += d; break;
        }
        *done = QObject::tr("moved %1 in %2 by %3").arg(what, cellName, pointSaid(by));
        return true;
    }
    // Replaced: the one picked, as given and else as it was.
    if (!pick(kind, edit, true, &picked, why)) return false;
    const Element el = picked.first();
    const QString was = describe(el);
    bool newLayer = false;
    if (edit.contains(QLatin1String("layer_to"))) {
        if (!layerOf(edit.value(QLatin1String("layer_to")), &layer, &type, why)) return false;
        newLayer = true;
    }
    switch (el.array) {
    case Holder::Polygons: {
        if (!only({"layer", "at", "box", "points", "layer_to"})) return false;
        gdstk::Polygon* p = c->polygon_array[el.index];
        QPolygonF points;
        if (edit.contains(QLatin1String("points"))) {
            if (!pointsOf(edit.value(QLatin1String("points")), 3, &points)) {
                *why = QObject::tr("a polygon's 'points' are [[x, y], ...] in µm, 3 at least");
                return false;
            }
            gdstk::Polygon trial = {};
            for (const QPointF& q : std::as_const(points)) trial.point_array.append(a_e.user(q));
            const bool encloses = std::abs(trial.area()) > 0;
            trial.clear();
            if (!encloses) {
                *why = QObject::tr("the polygon's points enclose nothing");
                return false;
            }
            p->point_array.clear();
            for (const QPointF& q : std::as_const(points)) p->point_array.append(a_e.user(q));
        }
        if (newLayer) p->tag = gdstk::make_tag(layer, type);
        break;
    }
    case Holder::FlexPaths: {
        if (!only({"layer", "at", "box", "points", "width", "ends", "layer_to"})) return false;
        gdstk::FlexPath* p = c->flexpath_array[el.index];
        QPolygonF points;
        if (edit.contains(QLatin1String("points"))) {
            if (!pointsOf(edit.value(QLatin1String("points")), 2, &points)) {
                *why = QObject::tr("a path's 'points' are its spine, [[x, y], ...] in µm, 2 at least");
                return false;
            }
        } else {
            for (uint64_t i = 0; i < p->spine.point_array.count; ++i) points << a_e.um(p->spine.point_array[i]);
        }
        double width = p->elements[0].half_width_and_offset.count > 0 ? 2 * p->elements[0].half_width_and_offset[0].u / a_e.scale : 0;
        if (edit.contains(QLatin1String("width"))) width = edit.value(QLatin1String("width")).toDouble(-1);
        if (!(width > 0) || width > 1e6) {
            *why = QObject::tr("a path has a 'width' in µm, more than 0");
            return false;
        }
        gdstk::EndType ends = p->elements[0].end_type;
        if (edit.contains(QLatin1String("ends")) && !endsOf(edit.value(QLatin1String("ends")), &ends, why)) return false;
        if (p->num_elements != 1 || points.size() < 2) {
            *why = QObject::tr("%1 is a path of several lines: edit_layout replaces simple paths").arg(was);
            return false;
        }
        const gdstk::Tag tag = newLayer ? gdstk::make_tag(layer, type) : pathTag(p);
        gdstk::FlexPath* made = newPath(points, width, ends, tag);
        made->properties = p->properties;   // (kept, and not freed with the old)
        p->properties = nullptr;
        p->clear();
        gdstk::free_allocation(p);
        c->flexpath_array[el.index] = made;
        break;
    }
    case Holder::RobustPaths:
        *why = QObject::tr("%1 is a path whose width varies along it: edit_layout replaces simple paths (delete it and add one)").arg(was);
        return false;
    case Holder::Labels: {
        if (!only({"layer", "at", "box", "text", "text_to", "at_to", "layer_to"})) return false;
        gdstk::Label* l = c->label_array[el.index];
        if (edit.contains(QLatin1String("text_to"))) {
            const QString words = edit.value(QLatin1String("text_to")).toString();
            if (words.isEmpty() || words.size() > 1000) {
                *why = QObject::tr("'text_to' is the text's new words");
                return false;
            }
            gdstk::free_allocation(l->text);
            l->text = gdstk::copy_string(words.toUtf8().constData(), nullptr);
        }
        if (edit.contains(QLatin1String("at_to"))) {
            QPointF to;
            if (!pointOf(edit.value(QLatin1String("at_to")), &to)) {
                *why = QObject::tr("'at_to' is [x, y] in µm");
                return false;
            }
            l->origin = a_e.user(to);
        }
        if (newLayer) l->tag = gdstk::make_tag(layer, type);
        break;
    }
    case Holder::References: {
        if (!only({"at", "box", "of", "at_to", "rotation", "mirrored", "magnification"})) return false;
        gdstk::Reference* r = c->reference_array[el.index];
        if (edit.contains(QLatin1String("at_to"))) {
            QPointF to;
            if (!pointOf(edit.value(QLatin1String("at_to")), &to)) {
                *why = QObject::tr("'at_to' is [x, y] in µm");
                return false;
            }
            r->origin = a_e.user(to);
        }
        if (edit.contains(QLatin1String("rotation"))) r->rotation = edit.value(QLatin1String("rotation")).toDouble() * M_PI / 180;
        if (edit.contains(QLatin1String("mirrored"))) r->x_reflection = edit.value(QLatin1String("mirrored")).toBool();
        if (edit.contains(QLatin1String("magnification"))) {
            const double m = edit.value(QLatin1String("magnification")).toDouble(-1);
            if (!(m > 0) || m > 1e6) {
                *why = QObject::tr("a placement's 'magnification' is more than 0");
                return false;
            }
            r->magnification = m;
        }
        break;
    }
    }
    *done = QObject::tr("replaced %1 in %2").arg(was, cellName);
    return true;
}

} // namespace

EditResult editLayout(const QString& file, const QString& cellName, const QJsonArray& edits)
{
    EditResult result;
    if (!router::isWritableLayout(file)) {
        result.error = QObject::tr("edit_layout writes a .gds or an .oas; %1 is neither (a .gds.gz can be saved as a .gds first)").arg(QFileInfo(file).fileName());
        return result;
    }
    if (edits.isEmpty() || edits.size() > 10000) {
        result.error = QObject::tr("'edits' is a list of 1 to 10000 edits");
        return result;
    }
    gdstk::Library lib = {};
    if (!router::readLibrary(file, &lib, &result.error)) return result;
    gdstk::Cell* cell = nullptr;
    if (!cellName.isEmpty()) {
        cell = lib.get_cell(cellName.toUtf8().constData());
        if (cell == nullptr) {
            lib.free_all();
            result.error = QObject::tr("%1 has no cell %2").arg(QFileInfo(file).fileName(), cellName);
            return result;
        }
    } else {
        gdstk::Array<gdstk::Cell*> tops = {};
        gdstk::Array<gdstk::RawCell*> raw = {};
        lib.top_level(tops, raw);
        uint64_t most = 0;
        for (uint64_t i = 0; i < tops.count; ++i) {
            const uint64_t size = tops[i]->polygon_array.count + tops[i]->flexpath_array.count + tops[i]->reference_array.count;
            if (cell == nullptr || size > most) {
                cell = tops[i];
                most = size;
            }
        }
        tops.clear();
        raw.clear();
        if (cell == nullptr) {
            lib.free_all();
            result.error = QObject::tr("%1 has no cell").arg(QFileInfo(file).fileName());
            return result;
        }
    }
    result.cell = QString::fromUtf8(cell->name);
    Editor editor(Edit{&lib, cell, 1e-6 / lib.unit});
    for (int i = 0; i < edits.size(); ++i) {
        QString done, why;
        if (!edits.at(i).isObject() || !editor.apply(edits.at(i).toObject(), &done, &why)) {
            lib.free_all();
            result.error = QObject::tr("Edit %1: %2. Nothing was changed.").arg(i + 1).arg(why.isEmpty() ? QObject::tr("an edit is an object") : why);
            result.done = {};
            return result;
        }
        result.done.append(done);
    }
    QString why;
    const bool written = router::writeLibrary(lib, file, &why);
    lib.free_all();
    if (!written) {
        result.error = why;
        result.done = {};
        return result;
    }
    result.ok = true;
    return result;
}

// ----------------------------------------------------------------------
// What changed

namespace {

/// Each shape of a cell, by what it is - its layer, kind, width, outline
/// on the database's grid, copies -, with its box.
QHash<QString, QList<QRectF>> shapesOf(const Layout& layout, const Cell& cell)
{
    QHash<QString, QList<QRectF>> out;
    const double dbu = layout.dbu > 0 ? layout.dbu : 0.001;
    for (const Shape& s : cell.shapes) {
        QString key = layout.layers.at(s.layer).key.text() + (s.kind == ShapeKind::Path ? QStringLiteral(" path ") : QStringLiteral(" polygon "));
        if (s.path >= 0) key += QString::number(std::llround(cell.paths.at(s.path).width / dbu)) + QLatin1Char(' ');
        for (quint32 k = 0; k < s.count; ++k) {
            const QPointF p = cell.points.at(s.first + k);
            key += QStringLiteral("%1,%2 ").arg(std::llround(p.x() / dbu)).arg(std::llround(p.y() / dbu));
        }
        if (s.repetition >= 0) key += QStringLiteral("x%1").arg(cell.repetitions.at(s.repetition).count());
        out[key] << s.bounds;
    }
    return out;
}

QJsonArray boxJson(const QRectF& b)
{
    const auto r = [](double v) { return std::round(v * 1000) / 1000 + 0.0; };
    return {r(b.left()), r(b.top()), r(b.right()), r(b.bottom())};
}

} // namespace

QJsonObject differences(const Layout& before, const Layout& after, int most)
{
    QJsonObject out;
    QJsonArray added, gone;
    for (const Cell& c : after.cells)
        if (before.cellIndex(c.name) < 0) added.append(c.name);
    for (const Cell& c : before.cells)
        if (after.cellIndex(c.name) < 0) gone.append(c.name);
    if (!added.isEmpty()) out.insert(QStringLiteral("cells added"), added);
    if (!gone.isEmpty()) out.insert(QStringLiteral("cells taken away"), gone);
    QJsonObject cells;
    const double dbu = after.dbu > 0 ? after.dbu : 0.001;
    for (const Cell& c : after.cells) {
        const int b = before.cellIndex(c.name);
        if (b < 0) continue;
        const Cell& was = before.cells.at(b);
        QJsonObject changed;
        // The shapes, by layer.
        const QHash<QString, QList<QRectF>> now = shapesOf(after, c), then = shapesOf(before, was);
        QMap<QString, QJsonObject> layers;
        const auto count = [&](const QHash<QString, QList<QRectF>>& a, const QHash<QString, QList<QRectF>>& b, const QString& what) {
            for (auto it = a.begin(); it != a.end(); ++it) {
                const int more = int(it.value().size()) - int(b.value(it.key()).size());
                if (more <= 0) continue;
                const QString layer = it.key().section(QLatin1Char(' '), 0, 0);
                QJsonObject& o = layers[layer];
                o.insert(what, o.value(what).toInt() + more);
                QJsonArray boxes = o.value(what + QStringLiteral(" boxes")).toArray();
                for (int k = 0; k < more && boxes.size() < most; ++k) boxes.append(boxJson(it.value().at(k)));
                o.insert(what + QStringLiteral(" boxes"), boxes);
            }
        };
        count(now, then, QStringLiteral("added"));
        count(then, now, QStringLiteral("taken away"));
        if (!layers.isEmpty()) {
            QJsonObject shapes;
            for (auto it = layers.begin(); it != layers.end(); ++it) shapes.insert(it.key(), it.value());
            changed.insert(QStringLiteral("shapes"), shapes);
        }
        // The texts and the placements, by what and where.
        const auto list = [&](const Cell& cell, const Layout& layout, bool texts) {
            QHash<QString, int> out;
            if (texts)
                for (const Label& l : cell.labels)
                    ++out[QStringLiteral("%1 on %2 at %3").arg(l.text, layout.layers.at(l.layer).key.text(),
                                                               pointSaid(QPointF(std::round(l.origin.x() / dbu) * dbu, std::round(l.origin.y() / dbu) * dbu)))];
            else
                for (const Instance& i : cell.instances)
                    ++out[QStringLiteral("%1 at %2%3%4").arg(i.cell >= 0 ? layout.cells.at(i.cell).name : QStringLiteral("?"),
                                                             pointSaid(QPointF(std::round(i.origin.x() / dbu) * dbu, std::round(i.origin.y() / dbu) * dbu)),
                                                             i.rotation != 0 ? QStringLiteral(", turned %1°").arg(numberSaid(i.rotation)) : QString(),
                                                             i.mirrored ? QStringLiteral(", mirrored") : QString())];
            return out;
        };
        for (const bool texts : {true, false}) {
            const QHash<QString, int> a = list(c, after, texts), z = list(was, before, texts);
            QJsonArray more, fewer;
            for (auto it = a.begin(); it != a.end(); ++it)
                for (int k = z.value(it.key()); k < it.value() && more.size() < most; ++k) more.append(it.key());
            for (auto it = z.begin(); it != z.end(); ++it)
                for (int k = a.value(it.key()); k < it.value() && fewer.size() < most; ++k) fewer.append(it.key());
            QJsonObject o;
            if (!more.isEmpty()) o.insert(QStringLiteral("added"), more);
            if (!fewer.isEmpty()) o.insert(QStringLiteral("taken away"), fewer);
            if (!o.isEmpty()) changed.insert(texts ? QStringLiteral("texts") : QStringLiteral("placements"), o);
        }
        if (!changed.isEmpty()) cells.insert(c.name, changed);
    }
    if (!cells.isEmpty()) out.insert(QStringLiteral("cells"), cells);
    return out;
}

} // namespace qucs_s::layout
