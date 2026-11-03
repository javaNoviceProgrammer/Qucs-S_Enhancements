/*
 * fem3_geometry.cpp - a 3D multiphysics model's geometry: its layered solids
 *                     made one, its domains, faces, edges and points, and
 *                     the selections of them
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem3_geometry.h"
#include "fem_import.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>

#include <clipper/clipper.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <numeric>
#include <set>

namespace qucs_s::fem {

namespace {

QString tr(const char* text, const char* disambiguation = nullptr, int n = -1)
{
    return QCoreApplication::translate("qucs_s::fem::Geometry3", text, disambiguation, n);
}

/// Indices joined into sets (union-find).
class Joins
{
public:
    explicit Joins(std::size_t n) : a_parent(n)
    {
        std::iota(a_parent.begin(), a_parent.end(), std::size_t(0));
    }
    std::size_t root(std::size_t i)
    {
        while (a_parent[i] != i) {
            a_parent[i] = a_parent[a_parent[i]];
            i = a_parent[i];
        }
        return i;
    }
    void join(std::size_t a, std::size_t b)
    {
        a = root(a);
        b = root(b);
        if (a != b) a_parent[std::max(a, b)] = std::min(a, b);
    }

private:
    std::vector<std::size_t> a_parent;
};

double cross2(QPointF a, QPointF b)
{
    return a.x() * b.y() - a.y() * b.x();
}

double norm2(QPointF a)
{
    return std::hypot(a.x(), a.y());
}

/// Lowest first, then from the left, then from the front (within \a eps); the
/// middles break a tie.
bool lowerFirst(const Box3& a, const Vec3& ma, const Box3& b, const Vec3& mb, double eps)
{
    const double ka[6] = {a.z0, a.x0, a.y0, ma.z, ma.x, ma.y};
    const double kb[6] = {b.z0, b.x0, b.y0, mb.z, mb.x, mb.y};
    for (int i = 0; i < 6; ++i)
        if (std::abs(ka[i] - kb[i]) > eps) return ka[i] < kb[i];
    return false;
}

/// A solid's piece: a region of the plan (an object of its Form Union) from
/// level k0 to level k1.
struct Piece {
    int region = -1;
    double z0 = 0, z1 = 0;
    int k0 = 0, k1 = 0;
    QString tag;   ///< the feature that made it
};

/// A solid as its features make it: pieces, or a boolean of solids.
struct Solid {
    enum Op { Pieces, Union, Difference, Intersection };
    QString name, group, tag;
    bool layer = false;   ///< a Layer Stack's row
    Op op = Pieces;
    std::vector<int> pieces;
    std::vector<Solid> inputs, tools;

    bool holds(const std::function<bool(int)>& held) const
    {
        switch (op) {
        case Pieces:
            for (int p : pieces)
                if (held(p)) return true;
            return false;
        case Union:
            for (const Solid& s : inputs)
                if (s.holds(held)) return true;
            return false;
        case Intersection:
            for (const Solid& s : inputs)
                if (!s.holds(held)) return false;
            return !inputs.empty();
        case Difference: {
            bool in = false;
            for (const Solid& s : inputs) in = in || s.holds(held);
            if (!in) return false;
            for (const Solid& t : tools)
                if (t.holds(held)) return false;
            return true;
        }
        }
        return false;
    }
    void collect(std::set<int>& out) const
    {
        for (int p : pieces) out.insert(p);
        for (const Solid& s : inputs) s.collect(out);
        for (const Solid& s : tools) s.collect(out);
    }
};

/// A Layer Stack waiting for its window: made once every other solid is.
struct PendingStack {
    QString tag, name;
    struct Row {
        QString name, layer;
        double z0 = 0, z1 = 0;
        int solid = -1;   ///< its solid's index in the solids made
        int piece = -1;
    };
    std::vector<Row> rows;
    bool autoWindow = true;
    double margin = 0;            ///< the plan's unit
    QRectF window;                ///< a box's (the plan's unit)
    LayoutShapes shapes;          ///< its layout's layers, µm
    double layoutScale = 1;       ///< the plan's units in a µm
};

class Builder3
{
public:
    Builder3(const Model& model, const ParameterScope& parameters) : a_model(model), a_ps(parameters)
    {
        const QString unit = model.component().text(QStringLiteral("unit"));
        const std::optional<Unit> u = parseUnit(unit);
        a_scale = u && u->dim == Dim::length() ? u->scale : 1e-3;
        a_unitName = unit;
    }

    GeometryBuild3 build(const QString& upTo)
    {
        GeometryBuild3 out;
        double tolerance = 1e-6;
        QString finTag = QStringLiteral("fin");
        for (const Node& f : a_model.geometry().children) {
            if (f.type == QLatin1String("form union")) {
                finTag = f.tag;
                if (f.enabled) {
                    tolerance = number(f.text(QStringLiteral("tolerance")), 1e-6);
                    if (!a_error.isEmpty() || !(tolerance > 0)) tolerance = 1e-6;
                    a_error.clear();
                }
                break;
            }
            if (!f.enabled) continue;
            a_error.clear();
            a_warning.clear();
            a_tag = f.tag;
            feature(f);
            for (const QString& file : a_files)
                if (!out.files.contains(file)) out.files << file;
            a_files.clear();
            if (!a_warning.isEmpty()) out.warnings.insert(f.tag, a_warning);
            if (!a_error.isEmpty()) {
                out.errors.insert(f.tag, a_error);
                out.failedAt = f.tag;
                break;
            }
            // (Up to a feature of an extrude's sketch: the extrude whole.)
            if (!upTo.isEmpty() && (f.tag == upTo || f.find(upTo))) break;
        }
        // The stacks' windows, once every other solid is made.
        if (out.failedAt.isEmpty())
            for (PendingStack& s : a_stacks) {
                a_error.clear();
                a_warning.clear();
                a_tag = s.tag;
                finishStack(s);
                if (!a_warning.isEmpty())
                    out.warnings.insert(s.tag, out.warnings.contains(s.tag) ? out.warnings.value(s.tag) + QStringLiteral("; ") + a_warning : a_warning);
                if (!a_error.isEmpty()) {
                    out.errors.insert(s.tag, a_error);
                    out.failedAt = s.tag;
                    break;
                }
            }
        a_error.clear();
        a_warning.clear();
        // (A build stopped at a feature: what was made before it shown - a
        // stack's rows left without their regions not.)
        if (!out.failedAt.isEmpty())
            a_solids.erase(std::remove_if(a_solids.begin(), a_solids.end(),
                                          [this](const Solid& solid) {
                                              std::set<int> pieces;
                                              solid.collect(pieces);
                                              for (int p : pieces)
                                                  if (a_pieces[std::size_t(p)].region < 0) return true;
                                              return false;
                                          }),
                           a_solids.end());
        QString why, failedAt;
        out.topology = formUnion(tolerance, &why, &failedAt);
        if (!out.topology) {
            const QString tag = failedAt.isEmpty() ? finTag : failedAt;
            if (!out.errors.contains(tag)) out.errors.insert(tag, why);
            if (out.failedAt.isEmpty()) out.failedAt = tag;
        } else if (!why.isEmpty()) {
            const QString tag = failedAt.isEmpty() ? finTag : failedAt;
            out.warnings.insert(tag, out.warnings.contains(tag) ? out.warnings.value(tag) + QStringLiteral("; ") + why : why);
        }
        return out;
    }

private:
    // --- Values ----------------------------------------------------------
    double evaluate(const QString& text, double lengthUnit, bool lengths, const QString& what)
    {
        Expression::Options options;
        if (lengths) {
            options.lengthUnit = lengthUnit;
            options.numbersAreLengths = true;
        }
        const Expression e = Expression::compile(text, a_ps.scope, options);
        if (!e.isValid() || !e.isConstant()) {
            fail(tr("%1: %2").arg(what.isEmpty() ? text : what, e.isValid() ? tr("not a constant") : e.error()));
            return 0;
        }
        const double v = e.constant();
        if (!std::isfinite(v)) {
            fail(tr("%1 is not a finite number").arg(what.isEmpty() ? text : what));
            return 0;
        }
        return v;
    }
    /// A length in the geometry's unit.
    double length(const QString& text, const QString& what) { return evaluate(text, a_scale, true, what); }
    /// A length in µm (a plain number), in the geometry's unit.
    double micrometres(const QString& text, const QString& what) { return evaluate(text, 1e-6, true, what) * 1e-6 / a_scale; }
    double number(const QString& text, double fallback)
    {
        if (text.trimmed().isEmpty()) return fallback;
        return evaluate(text, 1, false, QString());
    }
    void fail(const QString& why)
    {
        if (a_error.isEmpty()) a_error = why;
    }
    void warn(const QString& why)
    {
        a_warning = a_warning.isEmpty() ? why : a_warning + QStringLiteral("; ") + why;
    }

    // --- The features -----------------------------------------------------
    void feature(const Node& f)
    {
        const QString& t = f.type;
        if (t == QLatin1String("box") || t == QLatin1String("cylinder")) primitive(f);
        else if (t == QLatin1String("extrude")) extrude(f);
        else if (t == QLatin1String("stack")) stack(f);
        else if (t == QLatin1String("union") || t == QLatin1String("difference") || t == QLatin1String("intersection")) boolean(f);
        else {
            const NodeKind* k = f.kind();
            if (k && (k->spaces & Spaces::Plane) && k->group == QLatin1String("geometry"))
                fail(tr("a %1 is of the plan: in a 3D model it goes in an Extrude's sketch").arg(k->title));
            else
                fail(tr("a %1 is not a 3D model's feature").arg(k ? k->title : t));
        }
    }

    /// A 2D model of \a features, this model's unit, parameters, folder and
    /// Form Union's tolerance: what a sketch, a box's footprint is built in.
    Model planModel(const std::vector<Node>& features) const
    {
        Model m;
        if (const Node* defs = a_model.root.child(QStringLiteral("definitions"))) *m.root.child(QStringLiteral("definitions")) = *defs;
        m.component().props.insert(QStringLiteral("unit"), a_model.component().text(QStringLiteral("unit")));
        m.fileName = a_model.fileName;
        Node& g = m.geometry();
        Node fin = g.children.back();
        if (const Node* own = a_model.geometry().child(QStringLiteral("form union"))) fin.props = own->props;
        g.children.clear();
        for (const Node& f : features) g.children.push_back(f);
        g.children.push_back(fin);
        return m;
    }

    /// The objects \a features make in the plan, not made one; null and the
    /// feature's error said where one fails.
    std::shared_ptr<Topology> planObjects(const std::vector<Node>& features)
    {
        const GeometryBuild b = buildObjects(planModel(features), a_ps);
        for (const QString& file : b.files)
            if (!a_files.contains(file)) a_files << file;
        for (auto it = b.warnings.begin(); it != b.warnings.end(); ++it) warn(it.value());
        if (!b.failedAt.isEmpty() || !b.topology) {
            const QString why = b.errors.value(b.failedAt);
            const Node* bad = nullptr;
            for (const Node& f : features)
                if (f.tag == b.failedAt) bad = &f;
            fail(bad && bad->tag != a_tag ? tr("%1: %2").arg(bad->name(), why) : why);
            return nullptr;
        }
        return b.topology;
    }

    /// \a o, its curves in \a curves, a region of the plan: its index.
    int addRegion(const GObject& o, const std::vector<Curve>& curves)
    {
        QHash<int, int> moved;
        auto curveOf = [&](int c) {
            if (c < 0) return c;
            auto it = moved.constFind(c);
            if (it != moved.cend()) return *it;
            a_curves.push_back(curves[std::size_t(c)]);
            moved.insert(c, int(a_curves.size()) - 1);
            return int(a_curves.size()) - 1;
        };
        GObject r;
        r.name = o.name;
        for (const auto& loop : o.loops) {
            std::vector<GEdge> l;
            for (const GEdge& e : loop) l.push_back(GEdge{e.a, e.b, curveOf(e.curve), e.ta, e.tb});
            r.loops.push_back(std::move(l));
        }
        a_regions.push_back(std::move(r));
        return int(a_regions.size()) - 1;
    }
    /// A loop of straight lines through \a corners (counter-clockwise: an
    /// outline; clockwise: a hole), a region's.
    std::vector<GEdge> lineLoop(const std::vector<QPointF>& corners)
    {
        std::vector<GEdge> loop;
        for (std::size_t i = 0; i < corners.size(); ++i) {
            const QPointF a = corners[i], b = corners[(i + 1) % corners.size()];
            if (a == b) continue;
            Curve c;
            c.kind = Curve::Line;
            c.c = a;
            c.a = b - a;
            a_curves.push_back(c);
            loop.push_back(GEdge{a, b, int(a_curves.size()) - 1, 0, 1});
        }
        return loop;
    }
    int addPiece(int region, double z0, double z1)
    {
        Piece p;
        p.region = region;
        p.z0 = z0;
        p.z1 = z1;
        p.tag = a_tag;
        a_pieces.push_back(p);
        return int(a_pieces.size()) - 1;
    }
    /// \a s made: refused where a solid of its name is (a solid's name one
    /// solid's).
    bool addSolid(Solid s)
    {
        for (const Solid& o : a_solids)
            if (o.name == s.name) {
                fail(tr("there is a solid %1 already: each solid's name its own").arg(s.name));
                return false;
            }
        a_solids.push_back(std::move(s));
        return true;
    }
    /// A bottom and a height, the geometry's unit.
    bool heights(const Node& f, double* z0, double* z1)
    {
        const double z = length(f.text(QStringLiteral("z")), tr("its bottom z"));
        const double h = length(f.text(QStringLiteral("height")), tr("its height"));
        if (!a_error.isEmpty()) return false;
        if (!(h > 0)) {
            fail(tr("its height must be more than 0"));
            return false;
        }
        *z0 = z;
        *z1 = z + h;
        return true;
    }

    /// A box or a cylinder: a rectangle or a circle of the plan, extruded.
    void primitive(const Node& f)
    {
        double z0 = 0, z1 = 0;
        if (!heights(f, &z0, &z1)) return;
        Node footprint;
        footprint.tag = f.tag;
        footprint.label = f.label.isEmpty() ? f.tag : f.label;
        if (f.type == QLatin1String("box")) {
            footprint.type = QStringLiteral("rectangle");
            for (const char* key : {"base", "position", "size", "rotation"})
                if (f.props.contains(QString::fromLatin1(key))) footprint.props.insert(QString::fromLatin1(key), f.props.value(QString::fromLatin1(key)));
        } else {
            footprint.type = QStringLiteral("circle");
            for (const char* key : {"center", "radius"})
                if (f.props.contains(QString::fromLatin1(key))) footprint.props.insert(QString::fromLatin1(key), f.props.value(QString::fromLatin1(key)));
        }
        const std::shared_ptr<Topology> t = planObjects({footprint});
        if (!t) return;
        if (t->objects.size() != 1 || !t->objects.front().solid()) return fail(tr("its footprint is not a solid"));
        Solid s;
        s.name = footprint.label;
        s.group = s.name;
        s.tag = f.tag;
        s.pieces.push_back(addPiece(addRegion(t->objects.front(), t->curves), z0, z1));
        addSolid(std::move(s));
    }

    /// A sketch extruded: each object it ends with a solid of its name.
    void extrude(const Node& f)
    {
        double z0 = 0, z1 = 0;
        if (!heights(f, &z0, &z1)) return;
        std::vector<Node> sketch;
        for (const Node& c : f.children)
            if (c.enabled) sketch.push_back(c);
        if (sketch.empty()) return fail(tr("its sketch is empty: add rectangles, circles, polygons... to it"));
        const std::shared_ptr<Topology> t = planObjects(sketch);
        if (!t) return;
        const QString group = f.label.isEmpty() ? f.tag : f.label;
        int made = 0, open = 0;
        for (const GObject& o : t->objects) {
            if (!o.solid()) {
                ++open;
                continue;
            }
            Solid s;
            s.name = o.name;
            s.group = group;
            s.tag = f.tag;
            s.pieces.push_back(addPiece(addRegion(o, t->curves), z0, z1));
            if (!addSolid(std::move(s))) return;
            ++made;
        }
        if (open > 0) warn(tr("%n of its sketch's objects not solid (an open curve, a point) left out: a 3D model's are solids", nullptr, open));
        if (made == 0) fail(tr("its sketch makes no solid: closed shapes are extruded"));
    }

    /// A Layer Stack: its rows read, its window and layout's layers made once
    /// every other solid is.
    void stack(const Node& f)
    {
        PendingStack s;
        s.tag = f.tag;
        s.name = f.label.isEmpty() ? f.tag : f.label;
        QStringList layers;
        for (const QJsonValue& v : f.value(QStringLiteral("stack")).toArray()) {
            const QJsonArray r = v.toArray();
            auto cell = [&](int i) { return r.at(i).isDouble() ? formatNumber(r.at(i).toDouble(), 15) : r.at(i).toString().trimmed(); };
            PendingStack::Row row;
            row.name = cell(0);
            row.layer = cell(1);
            if (row.name.isEmpty()) return fail(tr("a row of the layer stack has no name"));
            const double z = micrometres(cell(2).isEmpty() ? QStringLiteral("0") : cell(2), tr("%1's z").arg(row.name));
            const double thickness = micrometres(cell(3).isEmpty() ? QStringLiteral("0") : cell(3), tr("%1's thickness").arg(row.name));
            if (!a_error.isEmpty()) return;
            if (!(thickness > 0)) return fail(tr("%1: its thickness must be more than 0").arg(row.name));
            row.z0 = z;
            row.z1 = z + thickness;
            if (!row.layer.isEmpty()) {
                LayoutLayer key;
                bool any = false;
                if (!parseLayoutLayer(row.layer, &key, &any)) return fail(tr("%1: %2 is not a layer (as 1/0)").arg(row.name, row.layer));
                if (any) return fail(tr("%1: a layer and its datatype, as 1/0 (not %2)").arg(row.name, row.layer));
                row.layer = key.text();
                if (!layers.contains(row.layer)) layers << row.layer;
            }
            s.rows.push_back(row);
        }
        if (s.rows.empty()) return fail(tr("the layer stack is empty: give its rows (name, layer, z, thickness)"));
        s.autoWindow = f.text(QStringLiteral("window")) != QLatin1String("box");
        if (s.autoWindow) {
            s.margin = micrometres(f.text(QStringLiteral("margin")).trimmed().isEmpty() ? QStringLiteral("0") : f.text(QStringLiteral("margin")),
                                   tr("its margin"));
            if (!a_error.isEmpty()) return;
            if (s.margin < 0) return fail(tr("its margin must not be less than 0"));
        } else {
            const QStringList a = f.pair(QStringLiteral("from")), b = f.pair(QStringLiteral("to"));
            const QPointF p0(micrometres(a.value(0), tr("the window's x")), micrometres(a.value(1), tr("the window's y")));
            const QPointF p1(micrometres(b.value(0), tr("the window's x")), micrometres(b.value(1), tr("the window's y")));
            if (!a_error.isEmpty()) return;
            s.window = QRectF(p0, p1).normalized();
            if (!(s.window.width() > 0) || !(s.window.height() > 0)) return fail(tr("its window has no area"));
        }
        s.layoutScale = 1e-6 / a_scale;
        if (!layers.isEmpty()) {
            const QString given = f.text(QStringLiteral("file")).trimmed();
            if (given.isEmpty()) return fail(tr("rows name layers (%1): give it a layout").arg(layers.join(QStringLiteral(", "))));
            QString path = given;
            if (QFileInfo(given).isRelative()) {
                if (a_model.fileName.isEmpty())
                    return fail(tr("%1 is relative to the model's folder: save the model first, or give its whole path").arg(given));
                path = QFileInfo(a_model.fileName).absoluteDir().absoluteFilePath(given);
            }
            a_files << QFileInfo(path).absoluteFilePath();
            QRectF window;
            if (!s.autoWindow) {
                const double um = s.layoutScale;
                window = QRectF(s.window.topLeft() / um, s.window.bottomRight() / um).adjusted(-1e-3, -1e-3, 1e-3, 1e-3);
            }
            s.shapes = readLayoutShapes(path, f.text(QStringLiteral("cell")), layers.join(QStringLiteral(", ")), window);
            if (!s.shapes.error.isEmpty()) return fail(s.shapes.error);
            for (const QString& w : s.shapes.warnings) warn(w);
        }
        // Its rows' solids now, in order; their pieces' regions once the
        // window is known.
        for (PendingStack::Row& row : s.rows) {
            Solid solid;
            solid.name = row.name;
            solid.group = s.name;
            solid.tag = f.tag;
            solid.layer = true;
            row.piece = addPiece(-1, row.z0, row.z1);
            solid.pieces.push_back(row.piece);
            if (!addSolid(std::move(solid))) return;
        }
        a_stacks.push_back(std::move(s));
    }

    /// A stack's window and rows' regions.
    void finishStack(PendingStack& s)
    {
        const double um = s.layoutScale;
        QRectF window = s.window;
        if (s.autoWindow) {
            // Round every other solid and its layout's shapes. (Corners, not
            // QRectF::united: a rectangle of no size is null to it, and
            // united with one the other.)
            double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
            auto round = [&](QPointF p) {
                x0 = std::min(x0, p.x());
                y0 = std::min(y0, p.y());
                x1 = std::max(x1, p.x());
                y1 = std::max(y1, p.y());
            };
            for (const GObject& r : a_regions)
                for (const auto& loop : r.loops)
                    for (const GEdge& e : loop) {
                        round(e.a);
                        round(e.b);
                    }
            for (const LayoutShapes::Layer& l : s.shapes.layers)
                for (const auto& loop : l.loops)
                    for (QPointF p : loop) round(p * um);
            if (!(x0 <= x1) || !(x1 - x0 + 2 * s.margin > 0) || !(y1 - y0 + 2 * s.margin > 0))
                return fail(tr("its window is round the other solids and its layout's shapes, and there are none: give it a box"));
            window = QRectF(QPointF(x0 - s.margin, y0 - s.margin), QPointF(x1 + s.margin, y1 + s.margin));
        }
        const int windowRegion = [&] {
            GObject o;
            o.name = s.name;
            o.loops.push_back(lineLoop({window.topLeft(), QPointF(window.right(), window.top()), window.bottomRight(),
                                        QPointF(window.left(), window.bottom())}));
            a_regions.push_back(std::move(o));
            return int(a_regions.size()) - 1;
        }();
        for (const PendingStack::Row& row : s.rows) {
            Piece& piece = a_pieces[std::size_t(row.piece)];
            if (row.layer.isEmpty()) {
                piece.region = windowRegion;
                continue;
            }
            LayoutLayer key;
            parseLayoutLayer(row.layer, &key);
            const LayoutShapes::Layer* l = s.shapes.layer(key);
            GObject o = l ? clipped(l->loops, um, window) : GObject();
            if (!o.solid()) {
                warn(tr("%1: the layout draws nothing of layer %2 in its window - the row left out").arg(row.name, row.layer));
                piece.region = -1;   // (holds no cell)
                continue;
            }
            o.name = row.name;
            a_regions.push_back(std::move(o));
            piece.region = int(a_regions.size()) - 1;
        }
        // (A row of nothing left out, unless a boolean has used it.)
        a_solids.erase(std::remove_if(a_solids.begin(), a_solids.end(),
                                      [&](const Solid& solid) {
                                          return solid.layer && solid.group == s.name && solid.op == Solid::Pieces && solid.pieces.size() == 1
                                                 && a_pieces[std::size_t(solid.pieces.front())].region < 0;
                                      }),
                       a_solids.end());
    }

    /// \a loops (µm; times \a scale the plan's unit) where they are in
    /// \a window: their insides, outlines counter-clockwise and holes
    /// clockwise.
    GObject clipped(const std::vector<std::vector<QPointF>>& loops, double scale, const QRectF& window)
    {
        double extent = std::max({1.0, std::abs(window.left()), std::abs(window.right()), std::abs(window.top()), std::abs(window.bottom())});
        for (const auto& loop : loops)
            for (QPointF p : loop) extent = std::max({extent, std::abs(p.x() * scale), std::abs(p.y() * scale)});
        const double k = std::pow(2.0, std::floor(std::log2(1e12 / extent)));
        ClipperLib::Paths subject, clip, result;
        for (const auto& loop : loops) {
            ClipperLib::Path path;
            for (QPointF p : loop)
                path.emplace_back(ClipperLib::cInt(std::llround(p.x() * scale * k)), ClipperLib::cInt(std::llround(p.y() * scale * k)));
            subject.push_back(path);
        }
        clip.push_back({{ClipperLib::cInt(std::llround(window.left() * k)), ClipperLib::cInt(std::llround(window.top() * k))},
                        {ClipperLib::cInt(std::llround(window.right() * k)), ClipperLib::cInt(std::llround(window.top() * k))},
                        {ClipperLib::cInt(std::llround(window.right() * k)), ClipperLib::cInt(std::llround(window.bottom() * k))},
                        {ClipperLib::cInt(std::llround(window.left() * k)), ClipperLib::cInt(std::llround(window.bottom() * k))}});
        ClipperLib::Clipper c;
        c.AddPaths(subject, ClipperLib::ptSubject, true);
        c.AddPaths(clip, ClipperLib::ptClip, true);
        c.Execute(ClipperLib::ctIntersection, result, ClipperLib::pftNonZero, ClipperLib::pftNonZero);
        ClipperLib::CleanPolygons(result, 1.0);
        GObject o;
        for (const ClipperLib::Path& path : result) {
            if (path.size() < 3) continue;
            std::vector<QPointF> corners;
            for (const ClipperLib::IntPoint& p : path) corners.emplace_back(double(p.X) / k, double(p.Y) / k);
            std::vector<GEdge> loop = lineLoop(corners);
            if (loop.size() >= 3) o.loops.push_back(std::move(loop));
        }
        return o;
    }

    /// The solids \a key names (an extrude's or a stack's name all of its;
    /// an array's its copies), taken out of those made where \a take.
    std::vector<Solid> inputs(const Node& f, const QString& key, bool take)
    {
        std::vector<Solid> out;
        const QStringList names = f.list(key);
        if (names.isEmpty()) {
            fail(key == QLatin1String("tools") ? tr("no solids to subtract") : tr("no input solids"));
            return out;
        }
        std::vector<int> picked;
        for (const QString& n : names) {
            bool any = false;
            for (int i = 0; i < int(a_solids.size()); ++i)
                if (a_solids[std::size_t(i)].name == n) {
                    picked.push_back(i);
                    any = true;
                }
            if (!any)
                for (int i = 0; i < int(a_solids.size()); ++i)
                    if (a_solids[std::size_t(i)].group == n) {
                        picked.push_back(i);
                        any = true;
                    }
            if (!any) {
                QStringList names;
                for (const Solid& s : a_solids) names << s.name;
                fail(tr("there is no solid %1 here (the solids: %2)").arg(n, names.isEmpty() ? tr("none") : names.join(QStringLiteral(", "))));
                return {};
            }
        }
        std::sort(picked.begin(), picked.end());
        picked.erase(std::unique(picked.begin(), picked.end()), picked.end());
        for (int i : picked) out.push_back(a_solids[std::size_t(i)]);
        if (take)
            for (auto it = picked.rbegin(); it != picked.rend(); ++it) a_solids.erase(a_solids.begin() + *it);
        return out;
    }
    bool flagOf(const Node& f, const QString& key)
    {
        QString why;
        const bool on = flagIn(f, key, a_ps.scope, &why);
        if (!why.isEmpty()) fail(why);
        return on;
    }

    /// A union, a difference or an intersection of solids: one solid.
    void boolean(const Node& f)
    {
        const bool keep = flagOf(f, QStringLiteral("keep"));
        if (!a_error.isEmpty()) return;
        const bool unite = f.type == QLatin1String("union");
        std::vector<Solid> in = inputs(f, QStringLiteral("input"), !keep || unite);
        if (!a_error.isEmpty()) return;
        std::vector<Solid> tools;
        if (f.type == QLatin1String("difference")) {
            tools = inputs(f, QStringLiteral("tools"), !keep);
            if (!a_error.isEmpty()) return;
        }
        if (unite && flagOf(f, QStringLiteral("interior")))
            warn(tr("its inputs' faces between them are not kept in a 3D model (yet): made one"));
        Solid s;
        s.name = f.label.isEmpty() ? f.tag : f.label;
        s.group = s.name;
        s.tag = f.tag;
        s.op = unite ? Solid::Union : f.type == QLatin1String("difference") ? Solid::Difference : Solid::Intersection;
        s.inputs = std::move(in);
        s.tools = std::move(tools);
        addSolid(std::move(s));
    }

    // --- Form Union -------------------------------------------------------
    std::shared_ptr<Topology3> formUnion(double tolerance, QString* why, QString* failedAt);

    const Model& a_model;
    const ParameterScope& a_ps;
    double a_scale = 1e-3;
    QString a_unitName;
    QString a_tag;
    QString a_error, a_warning;
    QStringList a_files;
    std::vector<Curve> a_curves;
    std::vector<GObject> a_regions;
    std::vector<Piece> a_pieces;
    std::vector<Solid> a_solids;
    std::vector<PendingStack> a_stacks;
};

/// Whether plan boundaries \a a and \a b lie on one curve: one, or two lines
/// on one line, or two arcs of one circle.
bool oneSurface(const Topology& plan, int a, int b, double eps)
{
    const int ca = plan.boundaries[std::size_t(a)].curve, cb = plan.boundaries[std::size_t(b)].curve;
    if (ca < 0 || cb < 0) return false;
    if (ca == cb) return true;
    const Curve& p = plan.curves[std::size_t(ca)];
    const Curve& q = plan.curves[std::size_t(cb)];
    if (p.kind == Curve::Line && q.kind == Curve::Line) {
        const double lp = norm2(p.a), lq = norm2(q.a);
        if (!(lp > 0) || !(lq > 0)) return false;
        return std::abs(cross2(p.a, q.a)) <= 1e-9 * lp * lq && std::abs(cross2(p.a, q.c - p.c)) <= eps * lp;
    }
    if (p.kind == Curve::Arc && q.kind == Curve::Arc) {
        auto circle = [](const Curve& c) {
            const double ra = norm2(c.a), rb = norm2(c.b);
            return std::abs(ra - rb) <= 1e-9 * ra && std::abs(c.a.x() * c.b.x() + c.a.y() * c.b.y()) <= 1e-9 * ra * rb;
        };
        return circle(p) && circle(q) && norm2(p.c - q.c) <= eps && std::abs(norm2(p.a) - norm2(q.a)) <= eps;
    }
    return false;
}

std::shared_ptr<Topology3> Builder3::formUnion(double tolerance, QString* why, QString* failedAt)
{
    auto t = std::make_shared<Topology3>();
    t->unitScale = a_scale;
    t->unitName = a_unitName;
    if (a_solids.empty()) {
        *why = tr("the geometry has no solids");
        auto plan = std::make_shared<Topology>();
        plan->unitScale = a_scale;
        plan->unitName = a_unitName;
        t->plan = plan;
        return t;
    }
    // The pieces the solids are made of, and where they begin and end.
    std::set<int> used;
    for (const Solid& s : a_solids) s.collect(used);
    QRectF planBox;
    double zLow = INFINITY, zHigh = -INFINITY;
    for (int p : used) {
        const Piece& piece = a_pieces[std::size_t(p)];
        if (piece.region >= 0) planBox = planBox.isNull() ? a_regions[std::size_t(piece.region)].bounds() : planBox.united(a_regions[std::size_t(piece.region)].bounds());
        zLow = std::min(zLow, piece.z0);
        zHigh = std::max(zHigh, piece.z1);
    }
    const double diag = std::max(std::sqrt(planBox.width() * planBox.width() + planBox.height() * planBox.height()
                                           + (zHigh - zLow) * (zHigh - zLow)),
                                 1e-300);
    const double eps = tolerance * diag;
    // Levels: every bottom and top, those nearer than the tolerance one.
    std::vector<double> zs;
    for (int p : used) {
        zs.push_back(a_pieces[std::size_t(p)].z0);
        zs.push_back(a_pieces[std::size_t(p)].z1);
    }
    std::sort(zs.begin(), zs.end());
    for (double z : zs)
        if (t->levels.empty() || z - t->levels.back() > eps) t->levels.push_back(z);
    auto levelOf = [&](double z) {
        int best = 0;
        for (int i = 1; i < int(t->levels.size()); ++i)
            if (std::abs(t->levels[std::size_t(i)] - z) < std::abs(t->levels[std::size_t(best)] - z)) best = i;
        return best;
    };
    QStringList thin;
    for (int p : used) {
        Piece& piece = a_pieces[std::size_t(p)];
        piece.k0 = levelOf(piece.z0);
        piece.k1 = levelOf(piece.z1);
        if (piece.k1 <= piece.k0) thin << piece.tag;
    }
    if (!thin.isEmpty()) {
        *failedAt = thin.front();
        *why = tr("its height is less than the union's tolerance, %1 %2 (%3 of the geometry's extent): a smaller tolerance "
                  "(Form Union's), or a taller solid")
                   .arg(formatNumber(eps, 3), a_unitName, formatNumber(tolerance, 3));
        return nullptr;
    }
    const int K = t->slabs();
    if (K < 1) {
        *why = tr("the solids have no height");
        return nullptr;
    }
    // The plan: every region's outline made one.
    {
        QString said;
        std::shared_ptr<Topology> plan = qucs_s::fem::formUnion(a_regions, a_curves, a_scale, a_unitName, tolerance, &said);
        if (!plan) {
            *why = said;
            return nullptr;
        }
        if (!said.isEmpty() && why->isEmpty()) *why = said;
        t->plan = plan;
    }
    const Topology& plan = *t->plan;
    const int D = int(plan.domains.size()), B = int(plan.boundaries.size()), P = int(plan.points.size());
    const int L = int(t->levels.size());
    // The solids, kept in the order made.
    const int S = int(a_solids.size());
    for (const Solid& s : a_solids) {
        t->objectNames << s.name;
        t->objectGroups << s.group;
        t->objectLayers.push_back(s.layer ? 1 : 0);
    }
    // Each cell's solids: those whose pieces hold it, as their booleans say.
    std::vector<int> cellSig(std::size_t(D) * std::size_t(K), -1);
    std::map<std::vector<int>, int> sigIndex;
    std::vector<std::vector<int>> sigs;
    t->objectVolume.assign(std::size_t(S), 0);
    std::vector<double> thickness(static_cast<std::size_t>(K));
    for (int k = 0; k < K; ++k) thickness[std::size_t(k)] = t->levels[std::size_t(k) + 1] - t->levels[std::size_t(k)];
    for (int d = 0; d < D; ++d) {
        const QSet<int>& regions = plan.domains[std::size_t(d)].objects;
        for (int k = 0; k < K; ++k) {
            const auto held = [&](int p) {
                const Piece& piece = a_pieces[std::size_t(p)];
                return piece.region >= 0 && piece.k0 <= k && k < piece.k1 && regions.contains(piece.region);
            };
            std::vector<int> sig;
            for (int s = 0; s < S; ++s)
                if (a_solids[std::size_t(s)].holds(held)) {
                    sig.push_back(s);
                    t->objectVolume[std::size_t(s)] += plan.domains[std::size_t(d)].area * thickness[std::size_t(k)];
                }
            if (sig.empty()) continue;
            auto it = sigIndex.find(sig);
            if (it == sigIndex.end()) {
                it = sigIndex.emplace(sig, int(sigs.size())).first;
                sigs.push_back(sig);
            }
            cellSig[std::size_t(d) * std::size_t(K) + std::size_t(k)] = it->second;
        }
    }
    // A solid that holds nothing: a boolean's empty result, said.
    for (int s = 0; s < S; ++s)
        if (!(t->objectVolume[std::size_t(s)] > 0)) {
            *failedAt = a_solids[std::size_t(s)].tag;
            *why = a_solids[std::size_t(s)].op == Solid::Pieces ? tr("%1 holds nothing: covered by nothing of the plan").arg(a_solids[std::size_t(s)].name)
                                                                 : tr("nothing is left of %1: the result is empty").arg(a_solids[std::size_t(s)].name);
            return nullptr;
        }
    auto sigOf = [&](int d, int k) {
        return d < 0 || k < 0 || k >= K ? -1 : cellSig[std::size_t(d) * std::size_t(K) + std::size_t(k)];
    };
    // Domains: the cells held by the same solids that touch.
    Joins cells(std::size_t(D) * std::size_t(K));
    for (int d = 0; d < D; ++d)
        for (int k = 0; k + 1 < K; ++k) {
            const int a = sigOf(d, k);
            if (a >= 0 && a == sigOf(d, k + 1)) cells.join(std::size_t(d) * K + k, std::size_t(d) * K + k + 1);
        }
    for (int b = 0; b < B; ++b) {
        const Topology::Boundary& bd = plan.boundaries[std::size_t(b)];
        if (bd.up < 0 || bd.down < 0) continue;
        for (int k = 0; k < K; ++k) {
            const int a = sigOf(bd.up, k);
            if (a >= 0 && a == sigOf(bd.down, k)) cells.join(std::size_t(bd.up) * K + k, std::size_t(bd.down) * K + k);
        }
    }
    std::vector<Topology3::Domain> found;
    std::map<std::size_t, int> rootDomain;
    std::vector<int> cellFound(std::size_t(D) * std::size_t(K), -1);
    std::vector<double> bestCell;
    for (int d = 0; d < D; ++d)
        for (int k = 0; k < K; ++k) {
            const int sig = sigOf(d, k);
            if (sig < 0) continue;
            const std::size_t r = cells.root(std::size_t(d) * K + k);
            auto it = rootDomain.find(r);
            if (it == rootDomain.end()) {
                it = rootDomain.emplace(r, int(found.size())).first;
                Topology3::Domain dm;
                for (int s : sigs[std::size_t(sig)]) dm.objects.insert(s);
                // Its owner: the smallest solid that holds it (of two
                // alike, the later).
                double ownerVolume = INFINITY;
                for (int s : sigs[std::size_t(sig)]) {
                    const double v = t->objectVolume[std::size_t(s)];
                    if (v <= ownerVolume * (1 + 1e-12)) {
                        ownerVolume = v;
                        dm.owner = s;
                    }
                }
                found.push_back(dm);
                bestCell.push_back(-1);
            }
            Topology3::Domain& dm = found[std::size_t(it->second)];
            cellFound[std::size_t(d) * K + k] = it->second;
            dm.cells.emplace_back(d, k);
            const Topology::Domain& pd = plan.domains[std::size_t(d)];
            const double v = pd.area * thickness[std::size_t(k)];
            dm.volume += v;
            dm.bounds.add(Vec3{pd.bounds.left(), pd.bounds.top(), t->levels[std::size_t(k)]});
            dm.bounds.add(Vec3{pd.bounds.right(), pd.bounds.bottom(), t->levels[std::size_t(k) + 1]});
            if (v > bestCell[std::size_t(it->second)]) {
                bestCell[std::size_t(it->second)] = v;
                dm.inside = Vec3{pd.inside.x(), pd.inside.y(), (t->levels[std::size_t(k)] + t->levels[std::size_t(k) + 1]) / 2};
            }
        }
    // Numbered from below, then from the left, then from the front.
    {
        std::vector<int> order(found.size());
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            return lowerFirst(found[std::size_t(a)].bounds, found[std::size_t(a)].inside, found[std::size_t(b)].bounds,
                              found[std::size_t(b)].inside, eps);
        });
        std::vector<int> number(found.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            number[std::size_t(order[i])] = int(i);
            t->domains.push_back(found[std::size_t(order[i])]);
        }
        t->cellDomain.assign(std::size_t(D) * std::size_t(K), -1);
        for (std::size_t c = 0; c < cellFound.size(); ++c)
            if (cellFound[c] >= 0) t->cellDomain[c] = number[std::size_t(cellFound[c])];
        for (const Topology3::Domain& dm : t->domains) t->bounds.add(dm.bounds);
    }
    auto dom = [&](int d, int k) { return d < 0 || k < 0 || k >= K ? -1 : t->cellDomain[std::size_t(d) * std::size_t(K) + std::size_t(k)]; };

    // Faces: walls (b, k) and floors (d, i) between two domains (or one and
    // the outside), joined on one surface where the same domains meet.
    const std::size_t W = std::size_t(B) * std::size_t(K);
    const std::size_t F = std::size_t(D) * std::size_t(L);
    std::vector<std::pair<int, int>> atomSides(W + F, {-1, -1});
    std::vector<char> isAtom(W + F, 0);
    for (int b = 0; b < B; ++b) {
        const Topology::Boundary& bd = plan.boundaries[std::size_t(b)];
        for (int k = 0; k < K; ++k) {
            const int u = dom(bd.up, k), w = dom(bd.down, k);
            if (u == w) continue;
            isAtom[std::size_t(b) * K + k] = 1;
            atomSides[std::size_t(b) * K + k] = {u, w};
        }
    }
    for (int d = 0; d < D; ++d)
        for (int i = 0; i < L; ++i) {
            const int lo = dom(d, i - 1), hi = dom(d, i);
            if (lo == hi) continue;
            isAtom[W + std::size_t(d) * L + i] = 1;
            atomSides[W + std::size_t(d) * L + i] = {lo, hi};
        }
    Joins atoms(W + F);
    for (int b = 0; b < B; ++b)
        for (int k = 0; k + 1 < K; ++k) {
            const std::size_t a = std::size_t(b) * K + k;
            if (isAtom[a] && isAtom[a + 1] && atomSides[a] == atomSides[a + 1]) atoms.join(a, a + 1);
        }
    // (Walls of one surface across a plan point: each boundary there, and
    // whether it arrives at the point or leaves it.)
    std::vector<int> pointOfVertex(plan.vertices.size(), -1);
    for (int p = 0; p < P; ++p) pointOfVertex[std::size_t(plan.points[std::size_t(p)].vertex)] = p;
    struct End {
        int boundary;
        bool arrives;
    };
    std::vector<std::vector<End>> ends(static_cast<std::size_t>(P));
    for (int b = 0; b < B; ++b) {
        const Topology::Boundary& bd = plan.boundaries[std::size_t(b)];
        const int ps = pointOfVertex[std::size_t(bd.start)], pe = pointOfVertex[std::size_t(bd.end)];
        if (ps >= 0) ends[std::size_t(ps)].push_back({b, false});
        if (pe >= 0) ends[std::size_t(pe)].push_back({b, true});
    }
    for (int p = 0; p < P; ++p) {
        const std::vector<End>& at = ends[std::size_t(p)];
        for (std::size_t x = 0; x < at.size(); ++x)
            for (std::size_t y = x + 1; y < at.size(); ++y) {
                if (at[x].boundary == at[y].boundary) continue;
                if (!oneSurface(plan, at[x].boundary, at[y].boundary, eps)) continue;
                const bool turned = at[x].arrives == at[y].arrives;
                for (int k = 0; k < K; ++k) {
                    const std::size_t a = std::size_t(at[x].boundary) * K + k, c = std::size_t(at[y].boundary) * K + k;
                    if (!isAtom[a] || !isAtom[c]) continue;
                    const std::pair<int, int> sa = atomSides[a];
                    const std::pair<int, int> sc = turned ? std::pair<int, int>(atomSides[c].second, atomSides[c].first) : atomSides[c];
                    if (sa == sc) atoms.join(a, c);
                }
            }
    }
    for (int b = 0; b < B; ++b) {
        const Topology::Boundary& bd = plan.boundaries[std::size_t(b)];
        if (bd.up < 0 || bd.down < 0) continue;
        for (int i = 0; i < L; ++i) {
            const std::size_t a = W + std::size_t(bd.up) * L + i, c = W + std::size_t(bd.down) * L + i;
            if (isAtom[a] && isAtom[c] && atomSides[a] == atomSides[c]) atoms.join(a, c);
        }
    }
    std::vector<Topology3::Face> faces;
    std::vector<int> atomFound(W + F, -1);
    {
        std::map<std::size_t, int> rootFace;
        std::vector<double> bestAtom;
        for (std::size_t a = 0; a < W + F; ++a) {
            if (!isAtom[a]) continue;
            const std::size_t r = atoms.root(a);
            auto it = rootFace.find(r);
            const bool wall = a < W;
            if (it == rootFace.end()) {
                it = rootFace.emplace(r, int(faces.size())).first;
                Topology3::Face f;
                f.back = atomSides[a].first;
                f.front = atomSides[a].second;
                f.floor = !wall;
                if (wall) {
                    const int b = int(a / std::size_t(K));
                    const Topology::Boundary& bd = plan.boundaries[std::size_t(b)];
                    f.curve = bd.curve;
                    f.planar = bd.curve >= 0 && plan.curves[std::size_t(bd.curve)].kind == Curve::Line;
                    if (f.planar) {
                        const std::vector<QPointF> line = bd.polyline(plan);
                        const QPointF along = line.back() - line.front();
                        const double n = norm2(along);
                        // (Right of the boundary as walked: from up into down.)
                        f.normal = n > 0 ? Vec3{along.y() / n, -along.x() / n, 0} : Vec3{};
                    }
                } else {
                    f.planar = true;
                    f.normal = Vec3{0, 0, 1};
                }
                faces.push_back(f);
                bestAtom.push_back(-1);
            }
            Topology3::Face& f = faces[std::size_t(it->second)];
            atomFound[a] = it->second;
            double area = 0;
            Vec3 middle;
            if (wall) {
                const int b = int(a / std::size_t(K)), k = int(a % std::size_t(K));
                f.atoms.emplace_back(b, k);
                const Topology::Boundary& bd = plan.boundaries[std::size_t(b)];
                area = bd.length * thickness[std::size_t(k)];
                const std::vector<QPointF> line = bd.polyline(plan);
                for (QPointF q : line) {
                    f.bounds.add(Vec3{q.x(), q.y(), t->levels[std::size_t(k)]});
                    f.bounds.add(Vec3{q.x(), q.y(), t->levels[std::size_t(k) + 1]});
                }
                // (Its middle segment's middle: where the plan's normalAt is.)
                const std::size_t ms = (line.size() - 1) / 2;
                const QPointF mid = ms + 1 < line.size() ? (line[ms] + line[ms + 1]) / 2 : line[ms];
                middle = Vec3{mid.x(), mid.y(), (t->levels[std::size_t(k)] + t->levels[std::size_t(k) + 1]) / 2};
            } else {
                const int d = int((a - W) / std::size_t(L)), i = int((a - W) % std::size_t(L));
                f.atoms.emplace_back(d, i);
                const Topology::Domain& pd = plan.domains[std::size_t(d)];
                area = pd.area;
                f.bounds.add(Vec3{pd.bounds.left(), pd.bounds.top(), t->levels[std::size_t(i)]});
                f.bounds.add(Vec3{pd.bounds.right(), pd.bounds.bottom(), t->levels[std::size_t(i)]});
                middle = Vec3{pd.inside.x(), pd.inside.y(), t->levels[std::size_t(i)]};
            }
            f.area += area;
            if (area > bestAtom[std::size_t(it->second)]) {
                bestAtom[std::size_t(it->second)] = area;
                f.middle = middle;
                f.middleAtom = int(f.atoms.size()) - 1;
            }
        }
    }
    // Each face's solids: those holding one side of it alone.
    auto objectsOf = [&](int d) { return d >= 0 ? t->domains[std::size_t(d)].objects : QSet<int>(); };
    for (Topology3::Face& f : faces) {
        const QSet<int> a = objectsOf(f.back), b = objectsOf(f.front);
        QSet<int> only = a;
        only.subtract(b);
        QSet<int> other = b;
        other.subtract(a);
        f.objects = only.unite(other);
    }
    std::vector<int> faceNumber(faces.size());
    {
        std::vector<int> order(faces.size());
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            return lowerFirst(faces[std::size_t(a)].bounds, faces[std::size_t(a)].middle, faces[std::size_t(b)].bounds,
                              faces[std::size_t(b)].middle, eps);
        });
        for (std::size_t i = 0; i < order.size(); ++i) {
            faceNumber[std::size_t(order[i])] = int(i);
            t->faces.push_back(faces[std::size_t(order[i])]);
        }
        t->wallFace.assign(W, -1);
        t->floorFace.assign(F, -1);
        for (std::size_t a = 0; a < W; ++a)
            if (atomFound[a] >= 0) t->wallFace[a] = faceNumber[std::size_t(atomFound[a])];
        for (std::size_t a = 0; a < F; ++a)
            if (atomFound[W + a] >= 0) t->floorFace[a] = faceNumber[std::size_t(atomFound[W + a])];
    }
    auto wall = [&](int b, int k) { return b < 0 || k < 0 || k >= K ? -1 : t->wallFace[std::size_t(b) * K + k]; };
    auto floorOf = [&](int d, int i) { return d < 0 || i < 0 || i >= L ? -1 : t->floorFace[std::size_t(d) * L + i]; };

    // Edges: rises (p, k) and rims (b, i) where two faces or more meet,
    // chained between corners where they do not go on alike.
    const std::size_t R = std::size_t(P) * std::size_t(K);
    const std::size_t M = std::size_t(B) * std::size_t(L);
    std::vector<std::vector<int>> edgeFaces(R + M);
    auto distinct = [](std::vector<int> v) {
        v.erase(std::remove(v.begin(), v.end(), -1), v.end());
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        return v;
    };
    for (int p = 0; p < P; ++p)
        for (int k = 0; k < K; ++k) {
            std::vector<int> v;
            for (int b : plan.points[std::size_t(p)].boundaries) v.push_back(wall(b, k));
            v = distinct(v);
            if (v.size() >= 2) edgeFaces[std::size_t(p) * K + k] = v;
        }
    for (int b = 0; b < B; ++b) {
        const Topology::Boundary& bd = plan.boundaries[std::size_t(b)];
        for (int i = 0; i < L; ++i) {
            std::vector<int> v = distinct({wall(b, i - 1), wall(b, i), floorOf(bd.up, i), floorOf(bd.down, i)});
            if (v.size() >= 2) edgeFaces[R + std::size_t(b) * L + i] = v;
        }
    }
    // The corners each edge atom joins: (plan point, level) as p * L + i.
    auto atomEnds = [&](std::size_t a) -> std::pair<int, int> {
        if (a < R) {
            const int p = int(a / std::size_t(K)), k = int(a % std::size_t(K));
            return {p * L + k, p * L + k + 1};
        }
        const int b = int((a - R) / std::size_t(L)), i = int((a - R) % std::size_t(L));
        const Topology::Boundary& bd = plan.boundaries[std::size_t(b)];
        return {pointOfVertex[std::size_t(bd.start)] * L + i, pointOfVertex[std::size_t(bd.end)] * L + i};
    };
    std::vector<std::vector<std::size_t>> around(std::size_t(P) * std::size_t(L));
    for (std::size_t a = 0; a < R + M; ++a) {
        if (edgeFaces[a].empty()) continue;
        const auto [c0, c1] = atomEnds(a);
        if (c0 >= 0) around[std::size_t(c0)].push_back(a);
        if (c1 >= 0) around[std::size_t(c1)].push_back(a);
    }
    // A corner breaks edges where its atoms are not two going on alike: two
    // rises of the same faces, two rims of the same faces on one curve.
    std::vector<char> isBreak(around.size(), 0);
    for (std::size_t c = 0; c < around.size(); ++c) {
        const auto& ar = around[c];
        if (ar.empty()) continue;
        if (ar.size() != 2) {
            isBreak[c] = 1;
            continue;
        }
        const std::size_t a = ar[0], b = ar[1];
        bool alike = edgeFaces[a] == edgeFaces[b] && (a < R) == (b < R);
        if (alike && a >= R) {
            const int ba = int((a - R) / std::size_t(L)), bb = int((b - R) / std::size_t(L));
            alike = ba == bb || oneSurface(plan, ba, bb, eps);
        }
        if (!alike) isBreak[c] = 1;
    }
    std::vector<int> atomEdge(R + M, -1);
    std::vector<Topology3::Edge> edges;
    std::vector<std::pair<int, int>> edgeCorners;
    auto otherEnd = [&](std::size_t a, int corner) {
        const auto [c0, c1] = atomEnds(a);
        return c0 == corner ? c1 : c0;
    };
    for (std::size_t first = 0; first < R + M; ++first) {
        if (edgeFaces[first].empty() || atomEdge[first] >= 0) continue;
        // Back to where its chain begins, then along it.
        std::size_t a = first;
        int at = atomEnds(a).first;
        for (std::size_t guard = 0; guard < R + M; ++guard) {
            if (at < 0 || isBreak[std::size_t(at)]) break;
            const auto& ar = around[std::size_t(at)];
            const std::size_t prev = ar[0] == a ? ar[1] : ar[0];
            if (prev == first) {   // a closed loop with no corner on it
                isBreak[std::size_t(at)] = 1;
                break;
            }
            a = prev;
            at = otherEnd(a, at);
        }
        Topology3::Edge e;
        e.rise = a < R;
        e.faces = edgeFaces[a];
        const int start = at;
        int cur = at;
        for (std::size_t guard = 0; guard <= R + M; ++guard) {
            atomEdge[a] = int(edges.size());
            if (a < R) e.atoms.emplace_back(int(a / std::size_t(K)), int(a % std::size_t(K)));
            else e.atoms.emplace_back(int((a - R) / std::size_t(L)), int((a - R) % std::size_t(L)));
            cur = otherEnd(a, cur);
            if (cur < 0 || isBreak[std::size_t(cur)]) break;
            const auto& ar = around[std::size_t(cur)];
            const std::size_t next = ar[0] == a ? ar[1] : ar[0];
            if (atomEdge[next] >= 0) break;
            a = next;
        }
        edgeCorners.emplace_back(start, cur);
        if (start >= 0) isBreak[std::size_t(start)] = 1;
        if (cur >= 0) isBreak[std::size_t(cur)] = 1;
        edges.push_back(std::move(e));
    }
    // Their lengths, bounds, middles; their solids.
    for (Topology3::Edge& e : edges) {
        double best = -1;
        for (const auto& [x, y] : e.atoms) {
            double len = 0;
            Vec3 mid;
            if (e.rise) {
                const QPointF q = plan.vertices[std::size_t(plan.points[std::size_t(x)].vertex)];
                len = thickness[std::size_t(y)];
                e.bounds.add(Vec3{q.x(), q.y(), t->levels[std::size_t(y)]});
                e.bounds.add(Vec3{q.x(), q.y(), t->levels[std::size_t(y) + 1]});
                mid = Vec3{q.x(), q.y(), (t->levels[std::size_t(y)] + t->levels[std::size_t(y) + 1]) / 2};
            } else {
                const Topology::Boundary& bd = plan.boundaries[std::size_t(x)];
                len = bd.length;
                const std::vector<QPointF> line = bd.polyline(plan);
                for (QPointF q : line) e.bounds.add(Vec3{q.x(), q.y(), t->levels[std::size_t(y)]});
                const QPointF m = line[line.size() / 2];
                mid = Vec3{m.x(), m.y(), t->levels[std::size_t(y)]};
            }
            e.length += len;
            if (len > best) {
                best = len;
                e.middle = mid;
            }
        }
        for (int f : e.faces) e.objects.unite(t->faces[std::size_t(f)].objects);
    }
    std::vector<int> edgeNumber(edges.size());
    {
        std::vector<int> order(edges.size());
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            return lowerFirst(edges[std::size_t(a)].bounds, edges[std::size_t(a)].middle, edges[std::size_t(b)].bounds,
                              edges[std::size_t(b)].middle, eps);
        });
        for (std::size_t i = 0; i < order.size(); ++i) {
            edgeNumber[std::size_t(order[i])] = int(i);
            t->edges.push_back(edges[std::size_t(order[i])]);
        }
        t->riseEdge.assign(R, -1);
        t->rimEdge.assign(M, -1);
        for (std::size_t a = 0; a < R; ++a)
            if (atomEdge[a] >= 0) t->riseEdge[a] = edgeNumber[std::size_t(atomEdge[a])];
        for (std::size_t a = 0; a < M; ++a)
            if (atomEdge[R + a] >= 0) t->rimEdge[a] = edgeNumber[std::size_t(atomEdge[R + a])];
    }
    // Points: the corners edges end at.
    std::vector<int> corners;
    for (const auto& [s, e] : edgeCorners) {
        if (s >= 0) corners.push_back(s);
        if (e >= 0) corners.push_back(e);
    }
    std::sort(corners.begin(), corners.end());
    corners.erase(std::unique(corners.begin(), corners.end()), corners.end());
    auto cornerPosition = [&](int c) {
        const int p = c / L, i = c % L;
        const QPointF q = plan.vertices[std::size_t(plan.points[std::size_t(p)].vertex)];
        return Vec3{q.x(), q.y(), t->levels[std::size_t(i)]};
    };
    std::stable_sort(corners.begin(), corners.end(), [&](int a, int b) {
        const Vec3 pa = cornerPosition(a), pb = cornerPosition(b);
        if (std::abs(pa.z - pb.z) > eps) return pa.z < pb.z;
        if (std::abs(pa.x - pb.x) > eps) return pa.x < pb.x;
        return pa.y < pb.y;
    });
    t->cornerPoint.assign(std::size_t(P) * std::size_t(L), -1);
    for (int c : corners) {
        Topology3::Point pt;
        pt.planPoint = c / L;
        pt.level = c % L;
        pt.position = cornerPosition(c);
        t->cornerPoint[std::size_t(c)] = int(t->points.size());
        t->points.push_back(pt);
    }
    for (std::size_t i = 0; i < edges.size(); ++i) {
        Topology3::Edge& e = t->edges[std::size_t(edgeNumber[i])];
        e.start = edgeCorners[i].first >= 0 ? t->cornerPoint[std::size_t(edgeCorners[i].first)] : -1;
        e.end = edgeCorners[i].second >= 0 ? t->cornerPoint[std::size_t(edgeCorners[i].second)] : -1;
        for (int c : {e.start, e.end}) {
            if (c < 0) continue;
            Topology3::Point& pt = t->points[std::size_t(c)];
            if (std::find(pt.edges.begin(), pt.edges.end(), edgeNumber[i]) == pt.edges.end()) pt.edges.push_back(edgeNumber[i]);
            pt.objects.unite(e.objects);
        }
    }
    return t;
}

} // namespace

// ------------------------------------------------------------------ Boxes

void Box3::add(const Vec3& p)
{
    x0 = std::min(x0, p.x);
    y0 = std::min(y0, p.y);
    z0 = std::min(z0, p.z);
    x1 = std::max(x1, p.x);
    y1 = std::max(y1, p.y);
    z1 = std::max(z1, p.z);
}

void Box3::add(const Box3& b)
{
    if (b.isEmpty()) return;
    add(Vec3{b.x0, b.y0, b.z0});
    add(Vec3{b.x1, b.y1, b.z1});
}

bool Box3::contains(const Box3& b, double tolerance) const
{
    return !b.isEmpty() && b.x0 >= x0 - tolerance && b.x1 <= x1 + tolerance && b.y0 >= y0 - tolerance && b.y1 <= y1 + tolerance
           && b.z0 >= z0 - tolerance && b.z1 <= z1 + tolerance;
}

// ------------------------------------------------------------------ Topology3

int Topology3::cell(int planDomain, int slab) const
{
    const int K = slabs();
    if (planDomain < 0 || slab < 0 || slab >= K || std::size_t(planDomain) * std::size_t(K) + std::size_t(slab) >= cellDomain.size()) return -1;
    return cellDomain[std::size_t(planDomain) * std::size_t(K) + std::size_t(slab)];
}

int Topology3::slabAt(double z) const
{
    if (levels.size() < 2 || !(z >= levels.front()) || !(z <= levels.back())) return -1;
    const auto it = std::upper_bound(levels.begin(), levels.end(), z);
    return std::clamp(int(it - levels.begin()) - 1, 0, slabs() - 1);
}

int Topology3::domainAt(const Vec3& p) const
{
    if (!plan) return -1;
    return cell(plan->domainAt(QPointF(p.x, p.y)), slabAt(p.z));
}

std::vector<std::array<Vec3, 3>> Topology3::faceTriangles(int face) const
{
    std::vector<std::array<Vec3, 3>> out;
    if (face < 0 || face >= int(faces.size()) || !plan) return out;
    const Face& f = faces[std::size_t(face)];
    const int K = slabs();
    for (const auto& [x, y] : f.atoms) {
        if (f.floor) {
            const double z = levels[std::size_t(y)];
            for (const std::array<int, 3>& tri : plan->domains[std::size_t(x)].triangles) {
                const QPointF a = plan->vertices[std::size_t(tri[0])], b = plan->vertices[std::size_t(tri[1])], c = plan->vertices[std::size_t(tri[2])];
                out.push_back({Vec3{a.x(), a.y(), z}, Vec3{b.x(), b.y(), z}, Vec3{c.x(), c.y(), z}});
            }
            continue;
        }
        const Topology::Boundary& bd = plan->boundaries[std::size_t(x)];
        const double z0 = levels[std::size_t(y)], z1 = levels[std::size_t(y) + 1];
        // Seen from its front: the boundary's right is its front unless the
        // face runs the other way here.
        const int up = bd.up >= 0 && y < K ? cellDomain[std::size_t(bd.up) * std::size_t(K) + std::size_t(y)] : -1;
        const bool turned = up != f.back;
        const std::vector<QPointF> line = bd.polyline(*plan);
        for (std::size_t i = 0; i + 1 < line.size(); ++i) {
            const Vec3 a0{line[i].x(), line[i].y(), z0}, b0{line[i + 1].x(), line[i + 1].y(), z0};
            const Vec3 b1{line[i + 1].x(), line[i + 1].y(), z1}, a1{line[i].x(), line[i].y(), z1};
            if (!turned) {
                out.push_back({a0, b0, b1});
                out.push_back({a0, b1, a1});
            } else {
                out.push_back({a0, b1, b0});
                out.push_back({a0, a1, b1});
            }
        }
    }
    return out;
}

std::vector<Vec3> Topology3::edgePolyline(int edge) const
{
    std::vector<Vec3> out;
    if (edge < 0 || edge >= int(edges.size()) || !plan) return out;
    const Edge& e = edges[std::size_t(edge)];
    if (e.atoms.empty()) return out;
    if (e.rise) {
        const QPointF q = plan->vertices[std::size_t(plan->points[std::size_t(e.atoms.front().first)].vertex)];
        // From its start: up or down.
        std::vector<int> ks;
        for (const auto& [p, k] : e.atoms) ks.push_back(k);
        const bool down = ks.size() > 1 ? ks[1] < ks[0] : (e.start >= 0 && points[std::size_t(e.start)].level > ks[0]);
        if (!down) {
            out.push_back(Vec3{q.x(), q.y(), levels[std::size_t(ks.front())]});
            for (int k : ks) out.push_back(Vec3{q.x(), q.y(), levels[std::size_t(k) + 1]});
        } else {
            out.push_back(Vec3{q.x(), q.y(), levels[std::size_t(ks.front()) + 1]});
            for (int k : ks) out.push_back(Vec3{q.x(), q.y(), levels[std::size_t(k)]});
        }
        return out;
    }
    // Along its rims from its start's plan vertex.
    int at = e.start >= 0 ? plan->points[std::size_t(points[std::size_t(e.start)].planPoint)].vertex : -1;
    for (const auto& [b, i] : e.atoms) {
        const Topology::Boundary& bd = plan->boundaries[std::size_t(b)];
        std::vector<QPointF> line = bd.polyline(*plan);
        const bool reversed = at >= 0 ? bd.start != at : false;
        if (reversed) std::reverse(line.begin(), line.end());
        at = reversed ? bd.start : bd.end;
        const double z = levels[std::size_t(i)];
        for (std::size_t k = out.empty() ? 0 : 1; k < line.size(); ++k) out.push_back(Vec3{line[k].x(), line[k].y(), z});
    }
    return out;
}

Vec3 Topology3::normalAt(int face, int domain) const
{
    if (face < 0 || face >= int(faces.size()) || !plan) return {};
    const Face& f = faces[std::size_t(face)];
    const double sign = domain == f.front ? -1 : 1;
    if (f.planar) return f.normal * sign;
    // A curved wall: the plan boundary's normal at its middle (on the atom
    // its middle is on).
    const auto& [b, k] = f.atoms[std::size_t(std::clamp(f.middleAtom, 0, int(f.atoms.size()) - 1))];
    const Topology::Boundary& bd = plan->boundaries[std::size_t(b)];
    const int K = slabs();
    const int up = bd.up >= 0 ? cellDomain[std::size_t(bd.up) * std::size_t(K) + std::size_t(k)] : -1;
    const int planDomain = up == domain ? bd.up : bd.down;
    if (planDomain < 0) {
        const QPointF n = plan->normalAt(b, bd.up >= 0 ? bd.up : bd.down);
        return Vec3{-n.x(), -n.y(), 0};
    }
    const QPointF n = plan->normalAt(b, planDomain);
    return Vec3{n.x(), n.y(), 0};
}

GeometryBuild3 buildGeometry3(const Model& model, const ParameterScope& parameters, const QString& upTo)
{
    Builder3 builder(model, parameters);
    return builder.build(upTo);
}

// ------------------------------------------------------------------ Selections

int count3(const Topology3& t, Level level)
{
    switch (level) {
    case Level::Domain: return int(t.domains.size());
    case Level::Boundary: return int(t.faces.size());
    case Level::Edge: return int(t.edges.size());
    case Level::Point: return int(t.points.size());
    case Level::None: break;
    }
    return 0;
}

namespace {

/// The solids \a names name: by name, else by their feature's (an extrude's,
/// a stack's), else an array's copies (n_1, n_2...); names of none in
/// \a unknown.
QSet<int> solidIndices(const Topology3& t, const QJsonValue& names, QStringList& unknown)
{
    QSet<int> out;
    QStringList list;
    if (names.isString()) list << names.toString();
    for (const QJsonValue& v : names.toArray()) list << v.toString();
    for (const QString& n : list) {
        bool any = false;
        for (int i = 0; i < t.objectNames.size(); ++i)
            if (t.objectNames.at(i) == n) {
                out.insert(i);
                any = true;
            }
        if (!any)
            for (int i = 0; i < t.objectGroups.size(); ++i)
                if (t.objectGroups.at(i) == n) {
                    out.insert(i);
                    any = true;
                }
        if (!any && !n.isEmpty())
            for (int i = 0; i < t.objectNames.size(); ++i) {
                const QString& o = t.objectNames.at(i);
                if (o.size() < n.size() + 2 || !o.startsWith(n) || o.at(n.size()) != QLatin1Char('_')) continue;
                bool digits = true;
                for (qsizetype k = n.size() + 1; k < o.size() && digits; ++k) digits = o.at(k).isDigit();
                if (digits) {
                    out.insert(i);
                    any = true;
                }
            }
        if (!any) unknown << n;
    }
    return out;
}

bool insideBox3(const Topology3& t, Level level, int i, const Box3& box)
{
    switch (level) {
    case Level::Domain: return box.contains(t.domains[std::size_t(i)].bounds);
    case Level::Boundary: return box.contains(t.faces[std::size_t(i)].bounds);
    case Level::Edge: return box.contains(t.edges[std::size_t(i)].bounds);
    case Level::Point: {
        Box3 b;
        b.add(t.points[std::size_t(i)].position);
        return box.contains(b);
    }
    case Level::None: break;
    }
    return false;
}

/// Faces to edges and points: those of the faces, the edges' ends.
QSet<int> edgesOfFaces(const Topology3& t, const QSet<int>& faces)
{
    QSet<int> out;
    for (int e = 0; e < int(t.edges.size()); ++e)
        for (int f : t.edges[std::size_t(e)].faces)
            if (faces.contains(f)) {
                out.insert(e);
                break;
            }
    return out;
}

QSet<int> pointsOfEdges(const Topology3& t, const QSet<int>& edges)
{
    QSet<int> out;
    for (int e : edges) {
        const Topology3::Edge& ed = t.edges[std::size_t(e)];
        if (ed.start >= 0) out.insert(ed.start);
        if (ed.end >= 0) out.insert(ed.end);
    }
    return out;
}

/// Faces taken to the \a level asked: themselves, their edges, their points.
QSet<int> fromFaces(const Topology3& t, Level level, const QSet<int>& faces)
{
    if (level == Level::Boundary) return faces;
    const QSet<int> edges = edgesOfFaces(t, faces);
    if (level == Level::Edge) return edges;
    return pointsOfEdges(t, edges);
}

double num(const QJsonValue& v)
{
    return v.isDouble() ? v.toDouble() : v.toString().toDouble();
}

QSet<int> resolve3(const Topology3& t, const Model& model, Level level, const QJsonObject& rule, QStringList& problems, int depth)
{
    QSet<int> out;
    if (depth > 16) {
        problems << tr("the selections name each other in a circle");
        return out;
    }
    const int n = count3(t, level);
    const double eps = 1e-9 * std::max(1.0, t.bounds.diagonal());
    QStringList unknown;
    QString taken;
    if (rule.value(QStringLiteral("all")).toBool()) {
        taken = QStringLiteral("all");
        for (int i = 0; i < n; ++i) out.insert(i);
    } else if (rule.contains(QStringLiteral("numbers"))) {
        taken = QStringLiteral("numbers");
        for (const QJsonValue& v : rule.value(QStringLiteral("numbers")).toArray()) {
            const int k = v.toInt() - 1;
            if (k >= 0 && k < n) out.insert(k);
            else problems << tr("there is no %1 %2").arg(level == Level::Boundary ? tr("face") : levelName(level)).arg(v.toInt());
        }
    } else if (rule.contains(QStringLiteral("named"))) {
        taken = QStringLiteral("named");
        const QString name = rule.value(QStringLiteral("named")).toString();
        const Node* found = nullptr;
        if (const Node* sels = model.component().child(QStringLiteral("selections")))
            for (const Node& s : sels->children)
                if ((s.label == name || s.tag == name) && s.enabled) found = &s;
        if (!found) problems << tr("there is no selection %1").arg(name);
        else if (levelOf(found->text(QStringLiteral("level"))) != level)
            problems << tr("the selection %1 is of %2, not %3").arg(name, levelNames(levelOf(found->text(QStringLiteral("level")))), levelNames(level));
        else
            out = resolve3(t, model, level, found->selection(), problems, depth + 1);
    } else if (rule.contains(QStringLiteral("box"))) {
        taken = QStringLiteral("box");
        const QJsonArray b = rule.value(QStringLiteral("box")).toArray();
        if (b.size() != 6) {
            problems << tr("a 3D model's box is six numbers: [x0, y0, z0, x1, y1, z1]");
        } else {
            Box3 box;
            box.add(Vec3{num(b.at(0)), num(b.at(1)), num(b.at(2))});
            box.add(Vec3{num(b.at(3)), num(b.at(4)), num(b.at(5))});
            for (int i = 0; i < n; ++i) {
                Box3 grown = box;
                grown.add(Vec3{box.x0 - eps, box.y0 - eps, box.z0 - eps});
                grown.add(Vec3{box.x1 + eps, box.y1 + eps, box.z1 + eps});
                if (insideBox3(t, level, i, grown)) out.insert(i);
            }
        }
    } else if (level == Level::Domain && (rule.contains(QStringLiteral("objects")) || rule.contains(QStringLiteral("only"))
                                          || rule.contains(QStringLiteral("layer")))) {
        const bool only = rule.contains(QStringLiteral("only"));
        const bool layer = !only && !rule.contains(QStringLiteral("objects"));
        taken = only ? QStringLiteral("only") : layer ? QStringLiteral("layer") : QStringLiteral("objects");
        QSet<int> objs = solidIndices(t, rule.value(taken), unknown);
        if (layer) {
            for (int s : QSet<int>(objs))
                if (t.objectLayers.size() > std::size_t(s) && !t.objectLayers[std::size_t(s)]) {
                    problems << tr("%1 is not a layer of a stack").arg(t.objectNames.at(s));
                    objs.remove(s);
                }
        }
        for (int i = 0; i < n; ++i) {
            const Topology3::Domain& d = t.domains[std::size_t(i)];
            if (only ? objs.contains(d.owner) : d.objects.intersects(objs)) out.insert(i);
        }
    } else if (level != Level::Domain && (rule.contains(QStringLiteral("of")) || rule.contains(QStringLiteral("layer")))) {
        const bool layer = !rule.contains(QStringLiteral("of"));
        taken = layer ? QStringLiteral("layer") : QStringLiteral("of");
        QSet<int> objs = solidIndices(t, rule.value(taken), unknown);
        if (layer)
            for (int s : QSet<int>(objs))
                if (t.objectLayers.size() > std::size_t(s) && !t.objectLayers[std::size_t(s)]) {
                    problems << tr("%1 is not a layer of a stack").arg(t.objectNames.at(s));
                    objs.remove(s);
                }
        QStringList sides;
        const QJsonValue sideValue = rule.value(QStringLiteral("side"));
        if (sideValue.isArray())
            for (const QJsonValue& v : sideValue.toArray()) sides << v.toString();
        else if (!sideValue.toString().isEmpty())
            sides << sideValue.toString();
        std::vector<Vec3> dirs;
        for (const QString& side : sides) {
            if (side == QLatin1String("left")) dirs.push_back({-1, 0, 0});
            else if (side == QLatin1String("right")) dirs.push_back({1, 0, 0});
            else if (side == QLatin1String("front")) dirs.push_back({0, -1, 0});
            else if (side == QLatin1String("back")) dirs.push_back({0, 1, 0});
            else if (side == QLatin1String("bottom")) dirs.push_back({0, 0, -1});
            else if (side == QLatin1String("top")) dirs.push_back({0, 0, 1});
            else problems << tr("a side is left, right, front, back, bottom or top, not %1").arg(side);
        }
        QSet<int> faces;
        for (int f = 0; f < int(t.faces.size()); ++f) {
            const Topology3::Face& fc = t.faces[std::size_t(f)];
            if (!fc.objects.intersects(objs)) continue;
            if (!sides.isEmpty()) {
                // Facing a side: the normal out of the solids' domain.
                auto held = [&](int d) { return d >= 0 && t.domains[std::size_t(d)].objects.intersects(objs); };
                const bool backIn = held(fc.back), frontIn = held(fc.front);
                if (backIn == frontIn) continue;
                const Vec3 nrm = t.normalAt(f, backIn ? fc.back : fc.front);
                bool facing = false;
                for (const Vec3& dir : dirs) facing = facing || nrm.dot(dir) >= 0.7;
                if (!facing) continue;
            }
            faces.insert(f);
        }
        out = fromFaces(t, level, faces);
        if (level == Level::Point && sides.isEmpty())
            for (int p = 0; p < int(t.points.size()); ++p)
                if (t.points[std::size_t(p)].objects.intersects(objs)) out.insert(p);
    } else if (level == Level::Boundary && rule.contains(QStringLiteral("between"))) {
        taken = QStringLiteral("between");
        const QJsonArray pair = rule.value(QStringLiteral("between")).toArray();
        const QSet<int> a = solidIndices(t, pair.at(0), unknown), b = solidIndices(t, pair.at(1), unknown);
        auto owner = [&](int d) { return d >= 0 ? t.domains[std::size_t(d)].owner : -1; };
        for (int i = 0; i < n; ++i) {
            const Topology3::Face& f = t.faces[std::size_t(i)];
            const int u = owner(f.back), d = owner(f.front);
            if ((a.contains(u) && b.contains(d)) || (a.contains(d) && b.contains(u))) out.insert(i);
        }
    } else if (level == Level::Boundary && (rule.value(QStringLiteral("exterior")).toBool() || rule.value(QStringLiteral("interior")).toBool())) {
        const bool exterior = rule.value(QStringLiteral("exterior")).toBool();
        taken = exterior ? QStringLiteral("exterior") : QStringLiteral("interior");
        for (int i = 0; i < n; ++i) {
            const Topology3::Face& f = t.faces[std::size_t(i)];
            if (exterior ? f.exterior() : f.back >= 0 && f.front >= 0) out.insert(i);
        }
    } else if (rule.contains(QStringLiteral("adjacent")) && level != Level::Domain) {
        taken = QStringLiteral("adjacent");
        const QSet<int> domains = resolve3(t, model, Level::Domain, rule.value(QStringLiteral("adjacent")).toObject(), problems, depth + 1);
        QSet<int> faces;
        for (int i = 0; i < int(t.faces.size()); ++i) {
            const Topology3::Face& f = t.faces[std::size_t(i)];
            if (domains.contains(f.back) || domains.contains(f.front)) faces.insert(i);
        }
        out = fromFaces(t, level, faces);
    } else if (level == Level::Boundary && rule.contains(QStringLiteral("normal"))) {
        taken = QStringLiteral("normal");
        const QJsonArray v = rule.value(QStringLiteral("normal")).toArray();
        const Vec3 dir = Vec3{num(v.at(0)), num(v.at(1)), num(v.at(2))}.normalized();
        if (v.size() != 3 || !(dir.norm() > 0)) {
            problems << tr("a normal is three numbers, not all 0: [0, 0, 1]");
        } else {
            for (int i = 0; i < n; ++i) {
                const Topology3::Face& f = t.faces[std::size_t(i)];
                if (!f.planar) continue;
                if (f.exterior()) {
                    const Vec3 out_ = t.normalAt(i, f.back >= 0 ? f.back : f.front);
                    if (out_.dot(dir) >= 1 - 1e-9) out.insert(i);
                } else if (std::abs(f.normal.dot(dir)) >= 1 - 1e-9) {
                    out.insert(i);
                }
            }
        }
    } else if (level != Level::Domain && rule.contains(QStringLiteral("plane"))) {
        taken = QStringLiteral("plane");
        const QJsonObject plane = rule.value(QStringLiteral("plane")).toObject();
        if (plane.size() != 1 || !(plane.contains(QStringLiteral("x")) || plane.contains(QStringLiteral("y")) || plane.contains(QStringLiteral("z")))) {
            problems << tr("a plane is one coordinate: {\"z\": 3}, {\"x\": 0} or {\"y\": 0}");
        } else {
            const QString axis = plane.keys().front();
            const double at = num(plane.value(axis));
            auto onPlane = [&](const Box3& b) {
                if (b.isEmpty()) return false;
                const double lo = axis == QLatin1String("x") ? b.x0 : axis == QLatin1String("y") ? b.y0 : b.z0;
                const double hi = axis == QLatin1String("x") ? b.x1 : axis == QLatin1String("y") ? b.y1 : b.z1;
                return std::abs(lo - at) <= eps && std::abs(hi - at) <= eps;
            };
            for (int i = 0; i < n; ++i) {
                Box3 b;
                if (level == Level::Boundary) b = t.faces[std::size_t(i)].bounds;
                else if (level == Level::Edge) b = t.edges[std::size_t(i)].bounds;
                else b.add(t.points[std::size_t(i)].position);
                if (onPlane(b)) out.insert(i);
            }
        }
    } else if (!rule.isEmpty() && !(rule.size() == 1 && rule.contains(QStringLiteral("except")))) {
        problems << tr("a rule for %1 is all, numbers, %2box or named")
                        .arg(level == Level::Boundary ? tr("faces") : levelNames(level),
                             level == Level::Domain   ? QStringLiteral("objects, only, layer, ")
                             : level == Level::Boundary ? QStringLiteral("of (with a side), layer, between, exterior, interior, adjacent, normal, plane, ")
                                                        : QStringLiteral("of (with a side), layer, adjacent, plane, "));
    }
    if (!unknown.isEmpty()) problems << tr("there is no solid %1").arg(unknown.join(QStringLiteral(", ")));
    if (!taken.isEmpty()) {
        static const QStringList rules{QStringLiteral("all"),      QStringLiteral("numbers"),  QStringLiteral("named"),
                                       QStringLiteral("box"),      QStringLiteral("objects"),  QStringLiteral("only"),
                                       QStringLiteral("layer"),    QStringLiteral("of"),       QStringLiteral("between"),
                                       QStringLiteral("exterior"), QStringLiteral("interior"), QStringLiteral("adjacent"),
                                       QStringLiteral("normal"),   QStringLiteral("plane")};
        QStringList over;
        for (const QString& key : rules) {
            if (key == taken || !rule.contains(key)) continue;
            if ((key == QLatin1String("exterior") || key == QLatin1String("interior") || key == QLatin1String("all")) && !rule.value(key).toBool())
                continue;
            over << key;
        }
        if (taken != QLatin1String("of") && taken != QLatin1String("layer") && rule.contains(QStringLiteral("side"))) over << QStringLiteral("side");
        if (!over.isEmpty())
            problems << tr("%1 was read, and %2 of the same selection left out: one rule to a selection ('except' takes another)")
                            .arg(taken, over.join(QLatin1String(", ")));
    }
    if (rule.contains(QStringLiteral("except")))
        out.subtract(resolve3(t, model, level, rule.value(QStringLiteral("except")).toObject(), problems, depth + 1));
    return out;
}

QString names(const QJsonValue& v)
{
    QStringList list;
    if (v.isString()) list << v.toString();
    for (const QJsonValue& e : v.toArray()) list << e.toString();
    return list.join(QLatin1String(", "));
}

} // namespace

QVector<int> resolveSelection3(const Topology3& topology, const Model& model, Level level, const QJsonObject& rule, QString* error)
{
    QStringList problems;
    const QSet<int> set = resolve3(topology, model, level, rule, problems, 0);
    QVector<int> out(set.begin(), set.end());
    std::sort(out.begin(), out.end());
    if (error) *error = problems.join(QLatin1String("; "));
    return out;
}

QString describeSelection3(const QJsonObject& rule, Level level)
{
    const QString plural = level == Level::Domain ? tr("domains") : level == Level::Boundary ? tr("faces") : level == Level::Edge ? tr("edges") : tr("points");
    QString text;
    if (rule.value(QStringLiteral("all")).toBool()) text = tr("all %1").arg(plural);
    else if (rule.contains(QStringLiteral("numbers"))) {
        QStringList n;
        for (const QJsonValue& v : rule.value(QStringLiteral("numbers")).toArray()) n << QString::number(v.toInt());
        text = n.isEmpty() ? tr("no %1").arg(plural) : tr("%1 %2").arg(plural, n.join(QLatin1String(", ")));
    } else if (rule.contains(QStringLiteral("named"))) text = tr("the selection %1").arg(rule.value(QStringLiteral("named")).toString());
    else if (rule.contains(QStringLiteral("objects"))) text = tr("the %1 in %2").arg(plural, names(rule.value(QStringLiteral("objects"))));
    else if (rule.contains(QStringLiteral("only"))) text = tr("the %1 of %2 alone").arg(plural, names(rule.value(QStringLiteral("only"))));
    else if (rule.contains(QStringLiteral("layer"))) text = tr("the %1 of layer %2").arg(plural, names(rule.value(QStringLiteral("layer"))));
    else if (rule.contains(QStringLiteral("of"))) {
        text = tr("the %1 of %2").arg(plural, names(rule.value(QStringLiteral("of"))));
        if (rule.contains(QStringLiteral("side"))) {
            const QJsonValue side = rule.value(QStringLiteral("side"));
            text += tr(" (%1)").arg(side.isArray() ? names(side) : side.toString());
        }
    } else if (rule.contains(QStringLiteral("between"))) {
        const QJsonArray p = rule.value(QStringLiteral("between")).toArray();
        text = tr("the %1 between %2 and %3").arg(plural, names(p.at(0)), names(p.at(1)));
    } else if (rule.value(QStringLiteral("exterior")).toBool()) text = tr("the exterior %1").arg(plural);
    else if (rule.value(QStringLiteral("interior")).toBool()) text = tr("the interior %1").arg(plural);
    else if (rule.contains(QStringLiteral("adjacent")))
        text = tr("the %1 of %2").arg(plural, describeSelection3(rule.value(QStringLiteral("adjacent")).toObject(), Level::Domain));
    else if (rule.contains(QStringLiteral("normal"))) {
        QStringList v;
        for (const QJsonValue& c : rule.value(QStringLiteral("normal")).toArray()) v << formatNumber(num(c), 6);
        text = tr("the %1 facing (%2)").arg(plural, v.join(QStringLiteral(", ")));
    } else if (rule.contains(QStringLiteral("plane"))) {
        const QJsonObject plane = rule.value(QStringLiteral("plane")).toObject();
        const QString axis = plane.isEmpty() ? QStringLiteral("z") : plane.keys().front();
        text = tr("the %1 in the plane %2 = %3").arg(plural, axis, formatNumber(num(plane.value(axis)), 6));
    } else if (rule.contains(QStringLiteral("box"))) text = tr("the %1 in a box").arg(plural);
    else text = tr("no %1").arg(plural);
    if (rule.contains(QStringLiteral("except")))
        text += tr(", except %1").arg(describeSelection3(rule.value(QStringLiteral("except")).toObject(), level));
    return text;
}

} // namespace qucs_s::fem
