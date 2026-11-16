/***************************************************************************
                               rf25_export.cpp
                              -----------------
 *  An RF 2.5D model's shapes and stack as a GDSII file, a DXF drawing and a
 *  3D model of Multiphysics 3D (rf25_export.h).
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 ***************************************************************************/

#include "rf25_export.h"

#include "multiphysics/fem_import.h"
#include "rf25_hfss.h"
#include "rf25_stackfile.h"

#include <QCoreApplication>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <array>
#include <cmath>

namespace qucs_s::rf25 {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::rf25", text);
}

/// A length in m as µm, as a Layer Stack's row takes it.
QString um(double metres)
{
    const double v = metres * 1e6;
    return QString::number(std::abs(v) < 1e-12 ? 0.0 : v, 'g', 12);
}

QString number(double v)
{
    return QString::number(v, 'g', 12);
}

} // namespace

QRectF LayoutExport::bounds() const
{
    double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    for (const ExportedShape& s : shapes)
        for (const QPointF& p : s.outline) {
            x0 = std::min(x0, p.x());
            y0 = std::min(y0, p.y());
            x1 = std::max(x1, p.x());
            y1 = std::max(y1, p.y());
        }
    if (!(x0 <= x1) || !(y0 <= y1)) return {};
    return QRectF(QPointF(x0, y0), QPointF(x1, y1));
}

LayoutExport layoutExportOf(const Model& model, const Processed& processed)
{
    LayoutExport out;
    const Stack stack = model.stack();
    // Each layer of the stack to the lowest of the file's layers the map
    // sends there.
    const LayerMap map = layerMapOf(model);
    QMap<QString, fem::LayoutLayer> lowest;
    int top = 0;
    for (auto it = map.toLayer.constBegin(); it != map.toLayer.constEnd(); ++it) {
        fem::LayoutLayer key;
        if (!fem::parseLayoutLayer(it.key(), &key)) continue;
        top = std::max(top, int(key.layer));
        const auto at = lowest.constFind(it.value());
        if (at == lowest.constEnd() || key < *at) lowest.insert(it.value(), key);
    }
    // The shapes, in the stack's order: metals, vias and dielectrics alike.
    QMap<QString, QList<QPolygonF>> byLayer;
    int fill = 0;
    for (const Conductor& c : processed.conductors) {
        if (c.outline.size() < 3) continue;
        byLayer[c.layer] << c.outline;
        fill += c.fill ? 1 : 0;
    }
    for (auto it = processed.extents.constBegin(); it != processed.extents.constEnd(); ++it)
        for (const QPolygonF& p : it.value())
            if (p.size() >= 3) byLayer[it.key()] << p;
    if (byLayer.isEmpty()) {
        out.problems << tr("There are no shapes to write: the layout and the model's own shapes leave none on a layer of the stack.");
        return out;
    }
    QStringList order;
    for (const Layer& l : stack.layers)
        if (byLayer.contains(l.name)) order << l.name;
    for (auto it = byLayer.constBegin(); it != byLayer.constEnd(); ++it)
        if (!order.contains(it.key())) order << it.key();
    QStringList numbered;
    for (const QString& layer : std::as_const(order)) {
        std::pair<int, int> n;
        if (const auto at = lowest.constFind(layer); at != lowest.constEnd()) {
            n = {int(at->layer), int(at->datatype)};
        } else {
            n = {++top, 0};
            numbered << tr("%1 on %2/0").arg(layer).arg(n.first);
        }
        out.numbers.insert(layer, n);
        out.layers << layer;
        for (const QPolygonF& p : byLayer.value(layer)) out.shapes << ExportedShape{layer, n.first, n.second, p};
    }
    if (!numbered.isEmpty())
        out.notes << tr("The layer map gives no layer of the file to %1, so each was written on the next number free: %2.")
                         .arg(numbered.size() == 1 ? tr("one layer") : tr("%1 layers").arg(numbered.size()), numbered.join(QStringLiteral(", ")));
    if (fill > 0)
        out.notes << QCoreApplication::translate("qucs_s::rf25", "%n shape(s) of dummy fill written with the others, on their layers.", nullptr, fill);
    return out;
}

bool writeLayoutGds(const LayoutExport& shapes, const QString& path, const QString& cell, QString* error)
{
    QList<GdsShape> list;
    list.reserve(shapes.shapes.size());
    for (const ExportedShape& s : shapes.shapes) list << GdsShape{s.number, s.datatype, s.outline};
    return writeGdsShapes(list, path, cell, error);
}

QString dxfLayerName(const QString& name)
{
    QString out;
    for (const QChar c : name) {
        const bool kept = (c.unicode() < 128 && c.isLetterOrNumber()) || c == QLatin1Char('_') || c == QLatin1Char('-') || c == QLatin1Char('$');
        out += kept ? c : QLatin1Char('_');
    }
    if (out.isEmpty()) out = QStringLiteral("_");
    return out.left(31);
}

namespace {

/// The nearest of AutoCAD's first nine colours to \a colour.
int aciOf(const QColor& colour)
{
    if (!colour.isValid()) return 7;
    static const std::array<std::pair<int, QColor>, 9> aci{{{1, QColor(255, 0, 0)},
                                                            {2, QColor(255, 255, 0)},
                                                            {3, QColor(0, 255, 0)},
                                                            {4, QColor(0, 255, 255)},
                                                            {5, QColor(0, 0, 255)},
                                                            {6, QColor(255, 0, 255)},
                                                            {7, QColor(255, 255, 255)},
                                                            {8, QColor(128, 128, 128)},
                                                            {9, QColor(192, 192, 192)}}};
    int best = 7;
    double nearest = INFINITY;
    for (const auto& [index, c] : aci) {
        const double d = std::pow(c.red() - colour.red(), 2) + std::pow(c.green() - colour.green(), 2) + std::pow(c.blue() - colour.blue(), 2);
        if (d < nearest) {
            nearest = d;
            best = index;
        }
    }
    return best;
}

} // namespace

QByteArray dxfOf(const LayoutExport& shapes, const QMap<QString, QColor>& colours, const QString& madeBy)
{
    QString s;
    const auto group = [&s](int code, const QString& value) {
        s += QString::number(code);
        s += QLatin1Char('\n');
        s += value;
        s += QLatin1Char('\n');
    };
    if (!madeBy.isEmpty()) group(999, madeBy);
    group(999, QStringLiteral("An RF 2.5D model's shapes as processed, in um: each a closed polyline on its layer of the stack"));
    group(0, QStringLiteral("SECTION"));
    group(2, QStringLiteral("HEADER"));
    group(9, QStringLiteral("$ACADVER"));
    group(1, QStringLiteral("AC1009"));
    group(9, QStringLiteral("$INSUNITS"));
    group(70, QStringLiteral("13"));
    group(0, QStringLiteral("ENDSEC"));
    // Each layer once, by a name of its own (two names made alike are told
    // apart by a number).
    QHash<QString, QString> nameOf;
    QSet<QString> taken;
    for (const QString& layer : shapes.layers) {
        QString name = dxfLayerName(layer);
        for (int n = 2; taken.contains(name.toUpper()); ++n) name = dxfLayerName(layer).left(27) + QStringLiteral("_%1").arg(n);
        taken.insert(name.toUpper());
        nameOf.insert(layer, name);
    }
    group(0, QStringLiteral("SECTION"));
    group(2, QStringLiteral("TABLES"));
    group(0, QStringLiteral("TABLE"));
    group(2, QStringLiteral("LAYER"));
    group(70, QString::number(shapes.layers.size()));
    for (const QString& layer : shapes.layers) {
        group(0, QStringLiteral("LAYER"));
        group(2, nameOf.value(layer));
        group(70, QStringLiteral("0"));
        group(62, QString::number(aciOf(colours.value(layer))));
        group(6, QStringLiteral("CONTINUOUS"));
    }
    group(0, QStringLiteral("ENDTAB"));
    group(0, QStringLiteral("ENDSEC"));
    group(0, QStringLiteral("SECTION"));
    group(2, QStringLiteral("ENTITIES"));
    for (const ExportedShape& shape : shapes.shapes) {
        QPolygonF p = shape.outline;
        // (Closed by its flag: a last corner on the first is dropped.)
        if (p.size() > 3 && p.first() == p.last()) p.removeLast();
        const QString layer = nameOf.value(shape.layer, dxfLayerName(shape.layer));
        group(0, QStringLiteral("POLYLINE"));
        group(8, layer);
        group(66, QStringLiteral("1"));
        group(10, QStringLiteral("0.0"));
        group(20, QStringLiteral("0.0"));
        group(30, QStringLiteral("0.0"));
        group(70, QStringLiteral("1"));
        for (const QPointF& q : std::as_const(p)) {
            group(0, QStringLiteral("VERTEX"));
            group(8, layer);
            group(10, number(q.x()));
            group(20, number(q.y()));
            group(30, QStringLiteral("0.0"));
        }
        group(0, QStringLiteral("SEQEND"));
        group(8, layer);
    }
    group(0, QStringLiteral("ENDSEC"));
    group(0, QStringLiteral("EOF"));
    return s.toUtf8();
}

namespace {

/// A material of the 3D model: its values as the material node takes them.
struct Material {
    QString label;
    QString epsilonr = QStringLiteral("1");
    double tand = 0, sigma = 0, mur = 1;
    QStringList rows;        ///< the stack's rows it is on
    QString visibility;
};

QJsonObject materialJson(const Material& m)
{
    QJsonObject o{{QStringLiteral("type"), QStringLiteral("material")},
                  {QStringLiteral("label"), m.label},
                  {QStringLiteral("epsilonr"), m.epsilonr},
                  // ("only": the domains each row's solid holds as the smallest
                  //  round them - a metal's in its room are the metal's, not
                  //  the fill's, so that a framed dielectric frames no metal.)
                  {QStringLiteral("selection"), QJsonObject{{QStringLiteral("only"), QJsonArray::fromStringList(m.rows)}}}};
    if (m.tand > 0) o.insert(QStringLiteral("tand"), number(m.tand));
    if (m.sigma > 0) o.insert(QStringLiteral("sigma"), number(m.sigma) + QStringLiteral("[S/m]"));
    if (m.mur != 1) o.insert(QStringLiteral("mur"), number(m.mur));
    if (!m.visibility.isEmpty()) o.insert(QStringLiteral("visibility"), m.visibility);
    return o;
}

/// A dielectric's or the substrate's permittivity as the material node
/// takes it: one value, or three along x, y and z where it is not the same
/// every way.
QString permittivityOf(const Layer& l)
{
    const double ex = l.averaged() ? l.epsx : l.epsr, ey = l.averaged() ? l.epsy : l.epsr;
    const double ez = l.epsz > 0 ? l.epsz : l.epsr;
    if (ex == ey && ey == ez) return number(l.epsr);
    return QStringLiteral("%1; %2; %3").arg(number(ex), number(ey), number(ez));
}

constexpr double kPerfect = 1e12;   ///< S/m: a perfect conductor's, as written

} // namespace

Mp3dExport mp3dOf(const Model& model, const LayoutExport& shapes, const QString& gdsFile, const QString& title, double margin,
                  const QString& madeBy)
{
    Mp3dExport out;
    const Stack stack = model.stack();
    if (!stack.ok()) {
        out.problems << tr("The stack cannot be drawn up as it stands: %1").arg(stack.problems.join(QLatin1Char(' ')));
        return out;
    }
    if (!shapes.ok()) {
        out.problems << shapes.problems;
        return out;
    }
    const QHash<QString, QString> hidden = layerVisibility(model);
    // The window: round the shapes, or a lid's side walls.
    QRectF window;   // µm
    for (const Layer& l : stack.layers)
        if (l.kind == Layer::Cover && l.walls && l.box.isValid())
            window = QRectF(l.box.topLeft() * 1e6, l.box.bottomRight() * 1e6).normalized();
    if (window.isNull()) {
        const QRectF b = shapes.bounds();
        if (b.isNull()) {
            out.problems << tr("There are no shapes to draw up.");
            return out;
        }
        const double side = std::max(b.width(), b.height());
        const double m = std::isfinite(margin) && margin >= 0 ? margin * 1e6 : 0.1 * side;
        window = b.adjusted(-m, -m, m, m);
        if (!(window.width() > 0) || !(window.height() > 0)) window = b.adjusted(-1, -1, 1, 1);
    } else {
        out.notes << tr("The window is the lid's side walls' box; the walls themselves are not drawn.");
    }

    QJsonArray rows;
    QList<Material> dielectrics, conductors;
    QHash<QString, int> dielectricOf;   // a dielectric's name to its material
    const auto row = [&rows](const QString& name, const QString& layer, double z0, double thickness) {
        rows.append(QJsonArray{name, layer, um(z0), um(thickness)});
    };
    const auto layerText = [&shapes](const QString& name) {
        const auto n = shapes.numbers.value(name);
        return QStringLiteral("%1/%2").arg(n.first).arg(n.second);
    };
    double thinnest = INFINITY;
    for (const Layer& l : stack.layers)
        if (l.kind == Layer::Metal && l.thickness > 0) thinnest = std::min(thinnest, l.thickness);
    if (!std::isfinite(thinnest)) thinnest = 1e-6;
    QStringList noShapes, perfect;
    bool dispersive = false, withExtents = false;
    for (int i = 0; i < stack.layers.size(); ++i) {
        const Layer& l = stack.layers.at(i);
        switch (l.kind) {
        case Layer::Substrate:
        case Layer::Dielectric: {
            row(l.name, QString(), l.z0, l.thickness);
            dispersive = dispersive || !l.dispersion.isEmpty();
            withExtents = withExtents || shapes.numbers.contains(l.name);
            // (Made already for a metal under it, whose room it fills.)
            if (dielectricOf.contains(l.name)) {
                dielectrics[dielectricOf.value(l.name)].rows << l.name;
                break;
            }
            Material m;
            m.label = l.name;
            m.epsilonr = permittivityOf(l);
            m.tand = l.tand;
            m.sigma = l.sigma;
            m.mur = l.mur;
            m.rows << l.name;
            m.visibility = hidden.value(l.name);
            dielectricOf.insert(l.name, int(dielectrics.size()));
            dielectrics << m;
            break;
        }
        case Layer::Metal: {
            // The room it takes, filled with the dielectric over it (as an
            // HFSS stackup's fill has it), and its shapes in it.
            const QString room = l.name + QStringLiteral(" fill");
            row(room, QString(), l.z0, l.thickness);
            // (The dielectric over it comes after it in the stack, so it is
            //  made here, its own slab joining it when it comes.)
            const Layer* above = fillAbove(stack, i);
            if (above != nullptr) {
                Material m;
                m.label = above->name;
                m.epsilonr = permittivityOf(*above);
                m.tand = above->tand;
                m.sigma = above->sigma;
                m.mur = above->mur;
                m.rows << room;
                m.visibility = hidden.value(above->name);
                dielectricOf.insert(above->name, int(dielectrics.size()));
                dielectrics << m;
            } else {
                Material m;
                m.label = room;
                m.rows << room;
                m.visibility = QStringLiteral("frame");
                dielectrics << m;
            }
            if (!shapes.numbers.contains(l.name)) {
                noShapes << l.name;
                break;
            }
            row(l.name, layerText(l.name), l.z0, l.thickness);
            Material m;
            m.label = l.name;
            m.sigma = l.sigma > 0 ? l.sigma : kPerfect;
            m.rows << l.name;
            m.visibility = hidden.value(l.name);
            conductors << m;
            break;
        }
        case Layer::Via: {
            if (!shapes.numbers.contains(l.name)) {
                noShapes << l.name;
                break;
            }
            // Through the room between its two metals (the ground: from
            // nought).
            const auto bounds = [&stack](const QString& name) -> std::pair<double, double> {
                if (stack.isGround(name)) return {0.0, 0.0};
                const Layer* m = stack.named(name);
                return m != nullptr ? std::pair<double, double>{m->z0, m->z1} : std::pair<double, double>{NAN, NAN};
            };
            const auto [a0, a1] = bounds(l.from);
            const auto [b0, b1] = bounds(l.to);
            const double z0 = std::min(a1, b1), z1 = std::max(a0, b0);
            if (!(z1 > z0)) {
                out.notes << tr("%1 joins metals with no room between them: it is not drawn.").arg(l.name);
                break;
            }
            row(l.name, layerText(l.name), z0, z1 - z0);
            Material m;
            m.label = l.name;
            m.sigma = l.sigma > 0 ? l.sigma : kPerfect;
            m.rows << l.name;
            m.visibility = hidden.value(l.name);
            conductors << m;
            break;
        }
        case Layer::Ground: {
            if (l.variant == QLatin1String("open")) break;
            row(l.name, QString(), -thinnest, thinnest);
            Material m;
            m.label = l.name;
            const bool lossy = (l.variant == QLatin1String("metal") || l.variant == QLatin1String("impedance")) && l.sigma > 0;
            m.sigma = lossy ? l.sigma : kPerfect;
            if (!lossy) perfect << l.name;
            m.rows << l.name;
            m.visibility = hidden.value(l.name);
            conductors << m;
            out.notes << tr("The ground plane, a sheet in the 2.5D stack, is drawn under it as thick as the thinnest metal (%1 µm).")
                             .arg(um(thinnest));
            break;
        }
        case Layer::Cover: {
            row(l.name, QString(), l.z0, l.thickness);
            Material air;
            air.label = l.name;
            air.rows << l.name;
            // (The air drawn as its frame: shut, it hides the stack under it.)
            air.visibility = hidden.value(l.name, QStringLiteral("frame"));
            dielectrics << air;
            if (l.variant == QLatin1String("perfect")) {
                const QString lid = l.name + QStringLiteral(" lid");
                row(lid, QString(), l.z1, thinnest);
                Material m;
                m.label = lid;
                m.sigma = kPerfect;
                m.rows << lid;
                m.visibility = hidden.value(l.name);
                conductors << m;
                perfect << lid;
            }
            break;
        }
        }
    }
    for (const Layer& l : stack.layers)
        if ((l.kind == Layer::Metal || l.kind == Layer::Via) && !(l.sigma > 0)) perfect << l.name;
    if (!noShapes.isEmpty())
        out.notes << tr("No shapes on %1: nothing of it drawn up.").arg(noShapes.join(QStringLiteral(", ")));
    if (!perfect.isEmpty())
        out.notes << tr("%1 perfect, written with a conductivity of 1e12 S/m.").arg(perfect.join(QStringLiteral(", ")));
    if (dispersive) out.notes << tr("A dispersive dielectric is written with its permittivity and loss tangent as given, at their frequency.");
    if (withExtents)
        out.notes << tr("A dielectric's own shapes are in the GDSII file, but the dielectric is drawn as one slab across the window.");
    {
        QStringList wires;
        for (const Node& n : model.layout().children)
            if (n.type == QLatin1String("rf_shapes"))
                for (const Node& c : n.children)
                    if (c.type == QLatin1String("rf_bondwire") && c.enabled) wires << nameOf(c);
        if (!wires.isEmpty()) out.notes << tr("Bond wires are not drawn: %1.").arg(wires.join(QStringLiteral(", ")));
    }
    if (!model.ports().children.empty()) out.notes << tr("The ports are not drawn: a 3D model has its own.");

    QJsonArray materials;
    for (const Material& m : std::as_const(dielectrics)) materials.append(materialJson(m));
    for (const Material& m : std::as_const(conductors)) materials.append(materialJson(m));
    QJsonObject stackNode{{QStringLiteral("type"), QStringLiteral("stack")},
                          {QStringLiteral("label"), QStringLiteral("Stack")},
                          {QStringLiteral("window"), QStringLiteral("box")},
                          {QStringLiteral("from"), QJsonArray{number(window.left()), number(window.top())}},
                          {QStringLiteral("to"), QJsonArray{number(window.right()), number(window.bottom())}},
                          {QStringLiteral("stack"), rows}};
    if (!shapes.numbers.isEmpty()) {
        stackNode.insert(QStringLiteral("file"), gdsFile);
        stackNode.insert(QStringLiteral("cell"), QStringLiteral("TOP"));
    }
    const QJsonObject component{{QStringLiteral("unit"), QStringLiteral("um")},
                                {QStringLiteral("space"), QStringLiteral("3D")},
                                {QStringLiteral("geometry"), QJsonArray{stackNode, QJsonObject{{QStringLiteral("type"), QStringLiteral("form union")}}}},
                                {QStringLiteral("materials"), materials},
                                {QStringLiteral("physics"), QJsonArray()},
                                {QStringLiteral("mesh"), QJsonObject{{QStringLiteral("size"), QStringLiteral("normal")}}}};
    QString description = tr("Made of the RF 2.5D model %1: its stack drawn up as a Layer Stack, each metal and via from its shapes as "
                             "processed, read from %2.")
                              .arg(title, gdsFile);
    if (!madeBy.isEmpty()) description += QLatin1Char(' ') + tr("Written by %1.").arg(madeBy);
    const QJsonObject root{{QStringLiteral("format"), QStringLiteral("qucs-s multiphysics 3d 1")},
                           {QStringLiteral("title"), title},
                           {QStringLiteral("description"), description},
                           {QStringLiteral("parameters"), QJsonArray()},
                           {QStringLiteral("components"), QJsonArray() << component},
                           {QStringLiteral("studies"), QJsonArray()},
                           {QStringLiteral("results"), QJsonObject{{QStringLiteral("plots"), QJsonArray()}, {QStringLiteral("derived"), QJsonArray()}}}};
    out.json = QJsonDocument(root).toJson(QJsonDocument::Indented);
    return out;
}

} // namespace qucs_s::rf25
