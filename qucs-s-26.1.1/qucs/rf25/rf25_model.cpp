/*
 * rf25_model.cpp - an RF 2.5D model: the tree, its kinds, and the stack
 *                  read out of it
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "rf25_model.h"

#include "multiphysics/fem_expr.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <functional>

namespace qucs_s::rf25 {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::rf25", text);
}

using P = PropertyDef;

P expr(const QString& key, const QString& label, const QString& unit, const QString& value, const QString& tip = {})
{
    P p;
    p.key = key;
    p.label = label;
    p.kind = P::Expression;
    p.unit = unit;
    p.defaultValue = value;
    p.tooltip = tip;
    return p;
}

P text(const QString& key, const QString& label, const QString& value, const QString& tip = {})
{
    P p;
    p.key = key;
    p.label = label;
    p.kind = P::Text;
    p.defaultValue = value;
    p.tooltip = tip;
    return p;
}

P choice(const QString& key, const QString& label, const QStringList& choices, const QStringList& labels,
         const QString& value, const QString& tip = {})
{
    P p;
    p.key = key;
    p.label = label;
    p.kind = P::Choice;
    p.choices = choices;
    p.choiceLabels = labels;
    p.defaultValue = value;
    p.tooltip = tip;
    return p;
}

P boolean(const QString& key, const QString& label, bool value, const QString& tip = {})
{
    P p;
    p.key = key;
    p.label = label;
    p.kind = P::Bool;
    p.defaultValue = value;
    p.tooltip = tip;
    return p;
}

P integer(const QString& key, const QString& label, int value, const QString& tip = {})
{
    P p;
    p.key = key;
    p.label = label;
    p.kind = P::Integer;
    p.defaultValue = value;
    p.tooltip = tip;
    return p;
}

/// \a p shown only where \a condition holds ("snap=true"): the panel asks
/// fem::propertyVisible() of it.
P shownIf(P p, const char* condition)
{
    p.showIf = QLatin1String(condition);
    return p;
}

P rows(const QString& key, const QString& label, const QStringList& columns, const QString& tip = {})
{
    P p;
    p.key = key;
    p.label = label;
    p.kind = P::Rows;
    p.defaultValue = QJsonArray();
    p.choiceLabels = columns;
    p.tooltip = tip;
    return p;
}

P file(const QString& key, const QString& label, const QString& tip = {})
{
    P p;
    p.key = key;
    p.label = label;
    p.kind = P::File;
    p.defaultValue = QString();
    p.tooltip = tip;
    return p;
}

QList<NodeKind> makeKinds()
{
    QList<NodeKind> k;
    const auto add = [&k](NodeKind kind) { k.append(std::move(kind)); };
    constexpr fem::Level none = fem::Level::None;

    add({QStringLiteral("rf_model"), tr("RF 2.5D Model"), QString(), QStringLiteral("model"), none,
         {text(QStringLiteral("description"), tr("Description"), QString())},
         {QStringLiteral("rf_parameters"), QStringLiteral("rf_stack"), QStringLiteral("rf_layout"),
          QStringLiteral("rf_ports"), QStringLiteral("rf_mesh"), QStringLiteral("rf_study"),
          QStringLiteral("rf_results"), QStringLiteral("rf_exports")},
         {}, true, true, QString(),
         tr("A planar structure: a stack in z, a layout in x and y, ports on its metals, and a frequency sweep that "
            "gives S parameters.")});

    add({QStringLiteral("rf_parameters"), tr("Parameters"), QString(), QStringLiteral("parameters"), none,
         {rows(QStringLiteral("rows"), tr("Parameters"), {tr("Name"), tr("Expression"), tr("Description")})},
         {}, {}, true, true, QString(),
         tr("Named values, each an expression of those above it: w = 10[um]. The stack, the layout and the sweep use "
            "them.")});

    // ---- the stack: the z direction ----
    add({QStringLiteral("rf_stack"), tr("Stack"), QString(), QStringLiteral("stack"), none, {},
         {QStringLiteral("rf_substrate"), QStringLiteral("rf_dielectric"), QStringLiteral("rf_metal"),
          QStringLiteral("rf_via"), QStringLiteral("rf_ground"), QStringLiteral("rf_cover")},
         {}, true, true, QString(),
         tr("The layers from the bottom up: the substrate, the dielectrics and metals over it, the vias between two "
            "metals, and what closes it at either end. Their order in the tree is their order in z - all but the "
            "ground's, which is a plane under the substrate wherever it is put.")});
    add({QStringLiteral("rf_substrate"), tr("Substrate"), QStringLiteral("stack"), QStringLiteral("layer"), none,
         {expr(QStringLiteral("thickness"), tr("Thickness"), QStringLiteral("m"), QStringLiteral("300[um]")),
          expr(QStringLiteral("epsr"), tr("Relative permittivity"), QString(), QStringLiteral("11.9")),
          expr(QStringLiteral("tand"), tr("Loss tangent"), QString(), QStringLiteral("0")),
          expr(QStringLiteral("sigma"), tr("Conductivity"), QStringLiteral("S/m"), QStringLiteral("0"),
               tr("A lossy silicon substrate: 10[S/m] for 10 Ohm cm. Zero for an insulating one."))},
         {}, {}, false, true, QStringLiteral("sub"),
         tr("The bottom layer, below every metal. One at most, and first in the stack.")});
    add({QStringLiteral("rf_dielectric"), tr("Dielectric"), QStringLiteral("stack"), QStringLiteral("layer"), none,
         {expr(QStringLiteral("thickness"), tr("Thickness"), QStringLiteral("m"), QStringLiteral("1[um]")),
          expr(QStringLiteral("epsr"), tr("Relative permittivity"), QString(), QStringLiteral("4.1")),
          expr(QStringLiteral("tand"), tr("Loss tangent"), QString(), QStringLiteral("0.002"))},
         {}, {}, false, false, QStringLiteral("d"),
         tr("An insulating layer: an oxide between two metals, a passivation over the top metal.")});
    add({QStringLiteral("rf_metal"), tr("Metal"), QStringLiteral("stack"), QStringLiteral("conductor"), none,
         {expr(QStringLiteral("thickness"), tr("Thickness"), QStringLiteral("m"), QStringLiteral("0.6[um]")),
          expr(QStringLiteral("sigma"), tr("Conductivity"), QStringLiteral("S/m"), QStringLiteral("3.3e7")),
          expr(QStringLiteral("roughness"), tr("Roughness"), QStringLiteral("m"), QStringLiteral("0"),
               tr("The rms roughness of its surface; it raises the loss at high frequency."))},
         {}, {}, false, false, QStringLiteral("m"),
         tr("A metal layer. The layout's polygons on the layers mapped to it are its conductors; its name (the label) "
            "is what the layer map and the ports call it.")});
    add({QStringLiteral("rf_via"), tr("Via"), QStringLiteral("stack"), QStringLiteral("via"), none,
         {text(QStringLiteral("from"), tr("From metal"), QString(), tr("The metal below, by its name")),
          text(QStringLiteral("to"), tr("To metal"), QString(), tr("The metal above, by its name")),
          expr(QStringLiteral("sigma"), tr("Conductivity"), QStringLiteral("S/m"), QStringLiteral("2.1e7")),
          expr(QStringLiteral("fill"), tr("Filled fraction"), QString(), QStringLiteral("1"),
               tr("How much of the drawn shape is metal (a tungsten plug in a wider opening: below 1)."))},
         {}, {}, false, false, QStringLiteral("v"),
         tr("The metal that joins two layers through the dielectric between them. Its two metals must be neighbours "
            "in the stack.")});
    add({QStringLiteral("rf_ground"), tr("Ground"), QStringLiteral("stack"), QStringLiteral("conductor"), none,
         {choice(QStringLiteral("kind"), tr("Below the stack"),
                 {QStringLiteral("perfect"), QStringLiteral("impedance"), QStringLiteral("open")},
                 {tr("A perfect plane"), tr("A surface impedance"), tr("Open (no plane)")}, QStringLiteral("perfect")),
          shownIf(expr(QStringLiteral("impedance"), tr("Surface impedance"), QStringLiteral("Ohm"), QStringLiteral("0"),
                       tr("Of the plane, per square")),
                  "kind=impedance")},
         {}, {}, false, true, QStringLiteral("gnd"),
         tr("What closes the stack at the bottom: a plane under the substrate, wherever it is in the tree. One at "
            "most. Open is no plane at all - the substrate on air.")});
    add({QStringLiteral("rf_cover"), tr("Cover"), QStringLiteral("stack"), QStringLiteral("conductor"), none,
         {expr(QStringLiteral("height"), tr("Height above the top"), QStringLiteral("m"), QStringLiteral("1[mm]")),
          choice(QStringLiteral("kind"), tr("Above the stack"),
                 {QStringLiteral("open"), QStringLiteral("perfect")},
                 {tr("Open (air above)"), tr("A metal lid")}, QStringLiteral("open"))},
         {}, {}, false, true, QStringLiteral("cov"),
         tr("What closes the stack at the top: air without end, or a lid at a height.")});

    // ---- the layout: x and y ----
    add({QStringLiteral("rf_layout"), tr("Layout"), QString(), QStringLiteral("layout"), none,
         {file(QStringLiteral("file"), tr("GDSII or OASIS file"), tr("Relative to the model's folder")),
          text(QStringLiteral("cell"), tr("Cell"), QString(), tr("The cell to solve; the top cell when empty")),
          expr(QStringLiteral("unit"), tr("Unit of its coordinates"), QStringLiteral("m"), QStringLiteral("1[um]"),
               tr("What one unit of the file means, where the file itself does not say"))},
         {QStringLiteral("rf_layer_map"), QStringLiteral("rf_processing")},
         {QStringLiteral("rf_layer_map"), QStringLiteral("rf_processing")}, true, true, QString(),
         tr("The shapes in x and y, read with gdstk as get_layout and find_shapes read them.")});
    add({QStringLiteral("rf_layer_map"), tr("Layer map"), QString(), QStringLiteral("map"), none,
         {rows(QStringLiteral("rows"), tr("Layers"), {tr("Layer"), tr("Datatype"), tr("Stack layer")},
               tr("One row per GDS layer and datatype: the metal or via of the stack it belongs to, or nothing to "
                  "leave it out."))},
         {}, {}, true, true, QString(),
         tr("Which layer of the file is which layer of the stack. A layer not named here is left out of the solve.")});
    add({QStringLiteral("rf_processing"), tr("Processing"), QString(), QStringLiteral("processing"), none,
         {boolean(QStringLiteral("merge"), tr("Merge touching shapes"), true),
          boolean(QStringLiteral("merge_vias"), tr("Merge nearby vias"), true),
          shownIf(expr(QStringLiteral("via_distance"), tr("Vias closer than"), QStringLiteral("m"), QStringLiteral("1[um]")),
                  "merge_vias=true"),
          shownIf(boolean(QStringLiteral("keep_area"), tr("Keep the merged area"), true,
                          tr("Shrinks the union so that the metal it stands for has the area the vias had")),
                  "merge_vias=true"),
          boolean(QStringLiteral("drop_slivers"), tr("Drop slivers"), true),
          shownIf(expr(QStringLiteral("sliver_width"), tr("Thinner than"), QStringLiteral("m"), QStringLiteral("0.1[um]")),
                  "drop_slivers=true"),
          boolean(QStringLiteral("snap"), tr("Snap to a grid"), false),
          shownIf(expr(QStringLiteral("grid"), tr("Grid"), QStringLiteral("m"), QStringLiteral("5[nm]")), "snap=true"),
          boolean(QStringLiteral("heal"), tr("Heal polygons"), true,
                  tr("Self-intersections, repeated points and zero-width necks")),
          boolean(QStringLiteral("remove_fill"), tr("Remove dummy fill"), false)},
         {}, {}, true, true, QString(),
         tr("What is done to the shapes before they are meshed. Each step says what it changed, and the processed "
            "layout is drawn over the original.")});

    // ---- the ports ----
    add({QStringLiteral("rf_ports"), tr("Ports"), QString(), QStringLiteral("ports"), none, {},
         {QStringLiteral("rf_edge_port"), QStringLiteral("rf_internal_port")}, {}, true, true, QString(),
         tr("Where the structure is driven. Their order is the order of the S matrix.")});
    add({QStringLiteral("rf_edge_port"), tr("Edge Port"), QStringLiteral("port"), QStringLiteral("port"), none,
         {text(QStringLiteral("conductor"), tr("On metal"), QString(), tr("A metal of the stack, by its name")),
          expr(QStringLiteral("x"), tr("At x"), QStringLiteral("m"), QStringLiteral("0")),
          expr(QStringLiteral("y"), tr("At y"), QStringLiteral("m"), QStringLiteral("0")),
          choice(QStringLiteral("facing"), tr("Facing"),
                 {QStringLiteral("auto"), QStringLiteral("+x"), QStringLiteral("-x"), QStringLiteral("+y"), QStringLiteral("-y")},
                 {tr("Outwards"), QStringLiteral("+x"), QStringLiteral("-x"), QStringLiteral("+y"), QStringLiteral("-y")},
                 QStringLiteral("auto")),
          expr(QStringLiteral("impedance"), tr("Reference impedance"), QStringLiteral("Ohm"), QStringLiteral("50")),
          expr(QStringLiteral("deembed"), tr("De-embed"), QStringLiteral("m"), QStringLiteral("0"),
               tr("How far the reference plane is moved in along the line"))},
         {}, {}, false, false, QStringLiteral("p"),
         tr("A port on the edge of a polygon, driven against the ground below it.")});
    add({QStringLiteral("rf_internal_port"), tr("Internal Port"), QStringLiteral("port"), QStringLiteral("port"), none,
         {expr(QStringLiteral("x"), tr("At x"), QStringLiteral("m"), QStringLiteral("0")),
          expr(QStringLiteral("y"), tr("At y"), QStringLiteral("m"), QStringLiteral("0")),
          text(QStringLiteral("from"), tr("From metal"), QString(), tr("A metal of the stack, or \"ground\"")),
          text(QStringLiteral("to"), tr("To metal"), QString()),
          expr(QStringLiteral("impedance"), tr("Reference impedance"), QStringLiteral("Ohm"), QStringLiteral("50"))},
         {}, {}, false, false, QStringLiteral("p"),
         tr("A port between two metals at a point, or between a metal and the ground.")});

    // ---- the mesh, the study, the results ----
    add({QStringLiteral("rf_mesh"), tr("Mesh"), QString(), QStringLiteral("mesh"), none,
         {integer(QStringLiteral("per_wavelength"), tr("Cells per wavelength"), 20),
          expr(QStringLiteral("maximum"), tr("Largest cell"), QStringLiteral("m"), QStringLiteral("0")),
          boolean(QStringLiteral("edges"), tr("Refine at the edges"), true,
                  tr("The current crowds at a conductor's edge; cells there are smaller")),
          integer(QStringLiteral("skin"), tr("Layers through the skin depth"), 0,
                  tr("0: the surface impedance alone, which is right above a few skin depths"))},
         {}, {}, true, true, QString(), tr("How finely the metals are divided. It is drawn in the view.")});
    add({QStringLiteral("rf_study"), tr("Study"), QString(), QStringLiteral("study"), none,
         {choice(QStringLiteral("kind"), tr("Solver"),
                 {QStringLiteral("quasi_static"), QStringLiteral("full_wave")},
                 {tr("Quasi-static (RLGC)"), tr("Full wave (MPIE)")}, QStringLiteral("quasi_static")),
          expr(QStringLiteral("from"), tr("From"), QStringLiteral("Hz"), QStringLiteral("0.1[GHz]")),
          expr(QStringLiteral("to"), tr("To"), QStringLiteral("Hz"), QStringLiteral("40[GHz]")),
          integer(QStringLiteral("points"), tr("Points"), 41),
          choice(QStringLiteral("scale"), tr("Spacing"), {QStringLiteral("linear"), QStringLiteral("log")},
                 {tr("Linear"), tr("Logarithmic")}, QStringLiteral("linear")),
          boolean(QStringLiteral("adaptive"), tr("Add points where it changes"), false)},
         {}, {}, true, true, QStringLiteral("st"),
         tr("The frequencies to solve at. The quasi-static solver reads them for a line's R and G and for its "
            "S-parameters; the full-wave one is not built yet.")});
    add({QStringLiteral("rf_results"), tr("Results"), QString(), QStringLiteral("results"), none, {}, {}, {}, true, true,
         QString(), tr("What the solve found: each net's capacitance, each as a line with its RLGC and S parameters, "
                       "and the frequency above which a quasi-static answer is no longer the whole story.")});
    add({QStringLiteral("rf_exports"), tr("Exports"), QString(), QStringLiteral("exports"), none, {},
         {QStringLiteral("rf_touchstone")}, {}, true, true, QString(), tr("What is written out of a solution.")});
    add({QStringLiteral("rf_touchstone"), tr("Touchstone"), QStringLiteral("export"), QStringLiteral("export"), none,
         {file(QStringLiteral("file"), tr("File")),
          choice(QStringLiteral("of"), tr("Of"), {QStringLiteral("line"), QStringLiteral("coupled")},
                 {tr("One net, as a two-port"), tr("Every net, as coupled lines")}, QStringLiteral("line")),
          text(QStringLiteral("net"), tr("Net"), QString(),
               tr("Which conductor, by the name the solve gave it (empty: the first)")),
          expr(QStringLiteral("length"), tr("Length"), QStringLiteral("m"), QString(),
               tr("How long a piece of line to write (empty: the length that was solved)")),
          expr(QStringLiteral("impedance"), tr("Reference impedance"), QStringLiteral("Ohm"), QStringLiteral("50"))},
         {}, {}, false, false, QStringLiteral("ts"),
         tr("An .sNp file of the S parameters, at the study's own frequencies. Coupled lines are written as a "
            "2N-port: the near ends first, then the far ends.")});
    return k;
}

const QList<NodeKind>& kinds()
{
    static const QList<NodeKind> all = makeKinds();
    return all;
}

const QHash<QString, const NodeKind*>& kindIndex()
{
    static const QHash<QString, const NodeKind*> index = [] {
        QHash<QString, const NodeKind*> h;
        for (const NodeKind& k : kinds()) h.insert(k.type, &k);
        return h;
    }();
    return index;
}

/// \a text evaluated in \a scope: its value, or nothing and why.
std::optional<double> numberIn(const QString& text, const fem::Scope& scope, QString* why)
{
    if (text.trimmed().isEmpty()) return 0.0;
    const fem::Expression e = fem::Expression::compile(text, scope);
    if (!e.isValid()) {
        if (why != nullptr) *why = e.error();
        return std::nullopt;
    }
    if (!e.isConstant()) {
        if (why != nullptr) *why = tr("it is not a constant here");
        return std::nullopt;
    }
    return e.constant();
}

Layer::Kind kindOfLayer(const QString& type)
{
    if (type == QLatin1String("rf_substrate")) return Layer::Substrate;
    if (type == QLatin1String("rf_metal")) return Layer::Metal;
    if (type == QLatin1String("rf_via")) return Layer::Via;
    if (type == QLatin1String("rf_ground")) return Layer::Ground;
    if (type == QLatin1String("rf_cover")) return Layer::Cover;
    return Layer::Dielectric;
}

QJsonObject nodeToJson(const Node& node)
{
    QJsonObject o{{QStringLiteral("type"), node.type}};
    if (!node.tag.isEmpty()) o.insert(QStringLiteral("tag"), node.tag);
    if (!node.label.isEmpty()) o.insert(QStringLiteral("label"), node.label);
    if (!node.enabled) o.insert(QStringLiteral("enabled"), false);
    if (!node.props.isEmpty()) o.insert(QStringLiteral("properties"), node.props);
    if (!node.children.empty()) {
        QJsonArray children;
        for (const Node& c : node.children) children.append(nodeToJson(c));
        o.insert(QStringLiteral("children"), children);
    }
    return o;
}

/*!
 * A node read from \a o, and what in it was not read in \a notes.
 *
 * A key a node does not have, and a property its kind does not have, were
 * passed over without a word: a file that wrote "props" for "properties"
 * loaded with every value at its default and the model said nothing was
 * missing (the phase A check of 24 October). They are still not read -
 * guessing what was meant is worse than saying - but they are said.
 */
Node nodeFromJson(const QJsonObject& o, QStringList* notes)
{
    Node n;
    n.type = o.value(QLatin1String("type")).toString();
    n.tag = o.value(QLatin1String("tag")).toString();
    n.label = o.value(QLatin1String("label")).toString();
    n.enabled = o.value(QLatin1String("enabled")).toBool(true);
    n.props = o.value(QLatin1String("properties")).toObject();
    const NodeKind* kind = nodeKind(n.type);
    const QString who = !n.label.isEmpty() ? n.label : !n.tag.isEmpty() ? n.tag : n.type;
    if (notes != nullptr) {
        static const QStringList keys{QStringLiteral("type"), QStringLiteral("tag"), QStringLiteral("label"),
                                      QStringLiteral("enabled"), QStringLiteral("properties"), QStringLiteral("children")};
        for (auto it = o.constBegin(); it != o.constEnd(); ++it) {
            if (keys.contains(it.key())) continue;
            *notes << (it.key() == QLatin1String("props")
                           ? QCoreApplication::translate("qucs_s::rf25", "%1: \"props\" is not read - a node's values are "
                                                                         "under \"properties\", so they were left at their "
                                                                         "defaults").arg(who)
                           : QCoreApplication::translate("qucs_s::rf25", "%1: \"%2\" is no key of a node, and was not read")
                                 .arg(who, it.key()));
        }
        // (Only of a kind this build knows: an unknown kind is said where
        //  the stack or the tree meets it.)
        if (kind != nullptr)
            for (auto it = n.props.constBegin(); it != n.props.constEnd(); ++it)
                if (kind->property(it.key()) == nullptr)
                    *notes << QCoreApplication::translate("qucs_s::rf25", "%1: \"%2\" is no property of a %3, and was "
                                                                          "not read")
                                  .arg(who, it.key(), kind->title);
    }
    for (const QJsonValue& c : o.value(QLatin1String("children")).toArray())
        if (c.isObject()) n.children.push_back(nodeFromJson(c.toObject(), notes));
    return n;
}

void collectTags(const Node& node, QSet<QString>& tags)
{
    if (!node.tag.isEmpty()) tags.insert(node.tag);
    for (const Node& c : node.children) collectTags(c, tags);
}

/// A child of \a type, made if it is not there (the fixed parts of the
/// tree, which a file of an older build may not have).
Node& child(Node& parent, const QString& type)
{
    if (Node* found = parent.child(type)) return *found;
    Node made;
    made.type = type;
    parent.children.push_back(made);
    return parent.children.back();
}

} // namespace

const NodeKind* nodeKind(const QString& type)
{
    return kindIndex().value(type, nullptr);
}

QList<const NodeKind*> nodeKinds()
{
    QList<const NodeKind*> list;
    for (const NodeKind& k : kinds()) list << &k;
    return list;
}

const NodeKind* kindOf(const Node& node)
{
    return nodeKind(node.type);
}

QJsonValue valueOf(const Node& node, const QString& key)
{
    if (node.props.contains(key)) return node.props.value(key);
    if (const NodeKind* k = nodeKind(node.type))
        if (const PropertyDef* p = k->property(key)) return p->defaultValue;
    return {};
}

QString textOf(const Node& node, const QString& key)
{
    const QJsonValue v = valueOf(node, key);
    if (v.isString()) return v.toString();
    if (v.isDouble()) return QString::number(v.toDouble());
    if (v.isBool()) return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    return {};
}

bool flagOf(const Node& node, const QString& key)
{
    const QJsonValue v = valueOf(node, key);
    return v.isBool() ? v.toBool() : v.isDouble() ? v.toDouble() != 0 : v.toString().compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
}

int integerOf(const Node& node, const QString& key)
{
    const QJsonValue v = valueOf(node, key);
    // (A whole number given as a text - a file written by hand - counts.)
    return v.isDouble() ? int(std::lround(v.toDouble())) : int(std::lround(v.toString().toDouble()));
}

QString nameOf(const Node& node)
{
    if (!node.label.isEmpty()) return node.label;
    const NodeKind* k = nodeKind(node.type);
    return k != nullptr ? k->title : node.type;
}

QString formatName()
{
    return QStringLiteral("qucs-s rf 2.5d 1");
}

QStringList Stack::conductors() const
{
    QStringList names;
    for (const Layer& l : layers)
        if (l.kind == Layer::Metal || l.kind == Layer::Ground)
            names << (l.name.isEmpty() ? l.tag : l.name);
    return names;
}

const Layer* Stack::named(const QString& name) const
{
    for (const Layer& l : layers)
        if (!name.isEmpty() && (l.name == name || l.tag == name)) return &l;
    return nullptr;
}

Model::Model()
{
    root.type = QStringLiteral("rf_model");
}

Model Model::blank()
{
    Model m;
    for (const char* type : {"rf_parameters", "rf_stack", "rf_layout", "rf_ports", "rf_mesh", "rf_study",
                             "rf_results", "rf_exports"})
        child(m.root, QLatin1String(type));
    // The layout's own fixed children.
    child(m.layout(), QStringLiteral("rf_layer_map"));
    child(m.layout(), QStringLiteral("rf_processing"));
    // A stack one can see at once: a substrate, an oxide and one metal.
    Node& stack = m.stackNode();
    Node substrate;
    substrate.type = QStringLiteral("rf_substrate");
    substrate.tag = QStringLiteral("sub1");
    substrate.label = tr("Substrate");
    Node oxide;
    oxide.type = QStringLiteral("rf_dielectric");
    oxide.tag = QStringLiteral("d1");
    oxide.label = QStringLiteral("ILD1");
    Node metal;
    metal.type = QStringLiteral("rf_metal");
    metal.tag = QStringLiteral("m1");
    metal.label = QStringLiteral("M1");
    stack.children.push_back(substrate);
    stack.children.push_back(oxide);
    stack.children.push_back(metal);
    m.normalise();
    return m;
}

Node& Model::stackNode() { return child(root, QStringLiteral("rf_stack")); }
const Node& Model::stackNode() const { return *root.child(QStringLiteral("rf_stack")); }
Node& Model::layout() { return child(root, QStringLiteral("rf_layout")); }
const Node& Model::layout() const { return *root.child(QStringLiteral("rf_layout")); }
Node& Model::ports() { return child(root, QStringLiteral("rf_ports")); }
const Node& Model::ports() const { return *root.child(QStringLiteral("rf_ports")); }
Node& Model::mesh() { return child(root, QStringLiteral("rf_mesh")); }
const Node& Model::mesh() const { return *root.child(QStringLiteral("rf_mesh")); }

QJsonObject Model::toObject() const
{
    return QJsonObject{{QStringLiteral("format"), formatName()}, {QStringLiteral("tree"), nodeToJson(root)}};
}

QByteArray Model::toJson() const
{
    return QJsonDocument(toObject()).toJson(QJsonDocument::Indented);
}

std::optional<Model> Model::fromJson(const QByteArray& json, QString* error)
{
    QJsonParseError parsed{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &parsed);
    if (document.isNull() || !document.isObject()) {
        if (error != nullptr)
            *error = tr("It does not read as JSON: %1").arg(parsed.errorString());
        return std::nullopt;
    }
    const QJsonObject o = document.object();
    const QString format = o.value(QLatin1String("format")).toString();
    if (format != formatName()) {
        if (error != nullptr)
            *error = format.isEmpty() ? tr("It does not say what it is: an RF 2.5D model begins \"format\": \"%1\".").arg(formatName())
                                      : tr("%1 is not an RF 2.5D model (%2).").arg(format, formatName());
        return std::nullopt;
    }
    const QJsonObject tree = o.value(QLatin1String("tree")).toObject();
    if (tree.value(QLatin1String("type")).toString() != QLatin1String("rf_model")) {
        if (error != nullptr) *error = tr("Its tree is not rooted in an RF 2.5D model.");
        return std::nullopt;
    }
    Model m;
    QStringList notes;
    m.root = nodeFromJson(tree, &notes);
    // The fixed parts, for a file written by a build that had fewer.
    for (const char* type : {"rf_parameters", "rf_stack", "rf_layout", "rf_ports", "rf_mesh", "rf_study",
                             "rf_results", "rf_exports"})
        child(m.root, QLatin1String(type));
    child(m.layout(), QStringLiteral("rf_layer_map"));
    child(m.layout(), QStringLiteral("rf_processing"));
    m.normalise();
    // What was not read, as the read's own notes (the multiphysics reader's
    // way too): the tab says them where it says what it read.
    if (error != nullptr) {
        if (notes.size() > 8) {
            const int more = int(notes.size()) - 8;
            notes = notes.mid(0, 8);
            notes << tr("and %1 more").arg(more);
        }
        *error = notes.join(QStringLiteral("; "));
    }
    return m;
}

QList<fem::Model::Parameter> Model::parameters() const
{
    QList<fem::Model::Parameter> list;
    const Node* node = root.child(QStringLiteral("rf_parameters"));
    if (node == nullptr) return list;
    for (const QJsonValue& row : node->props.value(QLatin1String("rows")).toArray()) {
        const QJsonArray cells = row.toArray();
        if (cells.isEmpty()) continue;
        fem::Model::Parameter p;
        p.name = cells.at(0).toString().trimmed();
        p.expression = cells.size() > 1 ? cells.at(1).toString().trimmed() : QString();
        p.description = cells.size() > 2 ? cells.at(2).toString() : QString();
        if (!p.name.isEmpty()) list << p;
    }
    return list;
}

Stack Model::stack() const
{
    return stackOf(stackNode(), parameters());
}

Stack stackOf(const Node& node, const QList<fem::Model::Parameter>& parameters)
{
    Stack out;
    // The parameters first, each an expression of those above it.
    fem::Scope scope(&fem::Scope::builtins());
    for (const fem::Model::Parameter& p : parameters) {
        const fem::Expression e = fem::Expression::compile(p.expression, scope);
        if (!e.isValid() || !e.isConstant()) {
            out.problems << QCoreApplication::translate("qucs_s::rf25", "The parameter %1 is not a number here: %2")
                                .arg(p.name, e.isValid() ? tr("it is not a constant") : e.error());
            continue;
        }
        scope.setConstant(p.name, e.constant(), e.dim());
    }

    // Every enabled child, in the order the tree has them - and, for each,
    // the properties that would not read, so that the checks below do not
    // say the same thing again of the value they were left at (a bad
    // thickness was "not a number" and then "no thickness": one mistake,
    // two things missing - the phase A check of 24 October).
    QList<QSet<QString>> unread;
    for (const Node& c : node.children) {
        if (!c.enabled) continue;
        const NodeKind* kind = nodeKind(c.type);
        if (kind == nullptr) {
            out.problems << tr("The stack holds a %1, which this build does not know.").arg(c.type);
            continue;
        }
        Layer layer;
        layer.kind = kindOfLayer(c.type);
        layer.tag = c.tag;
        layer.name = c.label.isEmpty() ? c.tag : c.label;
        const QString where = layer.name.isEmpty() ? kind->title : layer.name;
        QSet<QString> failed;
        const auto read = [&](const QString& key, double* into) {
            QString why;
            if (const std::optional<double> v = numberIn(textOf(c, key), scope, &why)) {
                *into = *v;
                return true;
            }
            out.problems << tr("%1: %2 is not a number (%3).").arg(where, key, why);
            failed.insert(key);
            return false;
        };
        switch (layer.kind) {
        case Layer::Substrate:
            read(QStringLiteral("thickness"), &layer.thickness);
            read(QStringLiteral("epsr"), &layer.epsr);
            read(QStringLiteral("tand"), &layer.tand);
            read(QStringLiteral("sigma"), &layer.sigma);
            break;
        case Layer::Dielectric:
            read(QStringLiteral("thickness"), &layer.thickness);
            read(QStringLiteral("epsr"), &layer.epsr);
            read(QStringLiteral("tand"), &layer.tand);
            break;
        case Layer::Metal:
            read(QStringLiteral("thickness"), &layer.thickness);
            read(QStringLiteral("sigma"), &layer.sigma);
            read(QStringLiteral("roughness"), &layer.roughness);
            break;
        case Layer::Via:
            layer.from = textOf(c, QStringLiteral("from")).trimmed();
            layer.to = textOf(c, QStringLiteral("to")).trimmed();
            read(QStringLiteral("sigma"), &layer.sigma);
            break;
        case Layer::Ground:
            layer.variant = textOf(c, QStringLiteral("kind"));
            if (layer.variant == QLatin1String("impedance"))
                read(QStringLiteral("impedance"), &layer.sigma);   // (per square; not a conductivity)
            break;
        case Layer::Cover:
            layer.variant = textOf(c, QStringLiteral("kind"));
            read(QStringLiteral("height"), &layer.thickness);
            break;
        }
        out.layers << layer;
        unread << failed;
    }

    // Where each layer sits: the thicknesses from the bottom up. A via
    // takes the room of the dielectric between its metals, so it adds
    // nothing of its own. The ground is a plane under everything, at
    // nought, wherever the tree has it: it has no place in the order to
    // get wrong. (It was put at the running height, so one listed after
    // the passivation sat on top of it - the phase A check of 24 October.)
    double z = 0;
    for (Layer& l : out.layers) {
        if (l.kind == Layer::Ground) {
            l.z0 = l.z1 = 0;
            continue;
        }
        if (l.kind == Layer::Via) {
            l.z0 = l.z1 = z;
            continue;
        }
        l.z0 = z;
        l.z1 = z + l.thickness;
        z = l.z1;
    }

    // What is wrong with it (the proposal's checks).
    const auto name = [](const Layer& l) { return l.name.isEmpty() ? l.tag : l.name; };
    // The first and the last layer that has a place in z: the ground has
    // none, so a substrate after it is still the first, and a cover
    // before it still the last.
    int firstPlaced = -1, lastPlaced = -1;
    for (int i = 0; i < out.layers.size(); ++i) {
        if (out.layers.at(i).kind == Layer::Ground) continue;
        if (firstPlaced < 0) firstPlaced = i;
        lastPlaced = i;
    }
    int substrates = 0, grounds = 0, covers = 0, metals = 0;
    for (int i = 0; i < out.layers.size(); ++i) {
        const Layer& l = out.layers.at(i);
        // (Said already where it would not read.)
        const auto read = [&unread, i](const char* key) { return !unread.at(i).contains(QLatin1String(key)); };
        switch (l.kind) {
        case Layer::Substrate: ++substrates; break;
        case Layer::Ground: ++grounds; break;
        case Layer::Cover: ++covers; break;
        case Layer::Metal: ++metals; break;
        default: break;
        }
        if (l.kind == Layer::Substrate && i != firstPlaced)
            out.problems << tr("The substrate is not the first layer: it lies below every metal.");
        if (l.kind == Layer::Cover && i != lastPlaced)
            out.problems << tr("The cover is not the last layer: it closes the stack at the top.");
        if ((l.kind == Layer::Substrate || l.kind == Layer::Dielectric || l.kind == Layer::Metal) && !(l.thickness > 0)
            && read("thickness"))
            out.problems << tr("%1 has no thickness.").arg(name(l));
        if ((l.kind == Layer::Dielectric || l.kind == Layer::Substrate) && l.epsr < 1 && read("epsr"))
            out.problems << tr("%1 has a relative permittivity below 1 (%2).").arg(name(l)).arg(l.epsr);
        if ((l.kind == Layer::Dielectric || l.kind == Layer::Substrate) && l.tand < 0 && read("tand"))
            out.problems << tr("%1 has a loss tangent below zero.").arg(name(l));
        if (l.kind == Layer::Metal && !(l.sigma > 0) && read("sigma"))
            out.problems << tr("%1 has no conductivity.").arg(name(l));
        if (l.kind == Layer::Via && !(l.sigma > 0) && read("sigma"))
            out.problems << tr("The via %1 has no conductivity.").arg(name(l));
        if (l.kind == Layer::Metal && l.roughness < 0 && read("roughness"))
            out.problems << tr("%1 has a roughness below zero.").arg(name(l));
        // A via joins the metals it names, and they must be neighbours:
        // nothing but dielectric between them.
        if (l.kind == Layer::Via) {
            const auto metalAt = [&](const QString& named) {
                for (int j = 0; j < out.layers.size(); ++j)
                    if (out.layers.at(j).kind == Layer::Metal && name(out.layers.at(j)) == named) return j;
                return -1;
            };
            const int a = metalAt(l.from), b = metalAt(l.to);
            if (l.from.isEmpty() || l.to.isEmpty())
                out.problems << tr("The via %1 does not say which metals it joins.").arg(name(l));
            else if (a < 0 || b < 0)
                out.problems << tr("The via %1 names %2, which is no metal of the stack.")
                                    .arg(name(l), a < 0 ? l.from : l.to);
            else {
                int between = 0;
                for (int j = std::min(a, b) + 1; j < std::max(a, b); ++j)
                    if (out.layers.at(j).kind == Layer::Metal) ++between;
                if (between > 0)
                    out.problems << tr("The via %1 joins %2 and %3, which are not neighbours: %4 metal(s) lie between "
                                       "them.").arg(name(l), l.from, l.to).arg(between);
            }
        }
    }
    if (substrates > 1) out.problems << tr("The stack has more than one substrate.");
    if (grounds > 1) out.problems << tr("The stack has more than one ground.");
    if (covers > 1) out.problems << tr("The stack has more than one cover.");
    if (metals == 0) out.problems << tr("The stack has no metal: there is nothing to solve on.");
    // Two metals with nothing insulating between them are one conductor,
    // which is almost always a dielectric left out by mistake. It is asked
    // of the z the layers were placed at, not of their places in the list,
    // because a via between them takes no room of its own.
    const Layer* below = nullptr;
    for (const Layer& l : std::as_const(out.layers)) {
        if (l.kind != Layer::Metal) continue;
        if (below != nullptr && l.z0 <= below->z1)
            out.problems << tr("%1 and %2 touch: there is no dielectric between them.").arg(name(*below), name(l));
        below = &l;
    }
    return out;
}

QStringList Model::problems() const
{
    const Stack s = stack();
    QStringList said = s.problems;
    // The layout: a file, and a map that names layers of the stack.
    if (textOf(layout(), QStringLiteral("file")).trimmed().isEmpty())
        said << tr("The layout names no file: there are no shapes to solve on.");
    const QJsonArray map = layout().child(QStringLiteral("rf_layer_map")) != nullptr
                               ? layout().child(QStringLiteral("rf_layer_map"))->props.value(QLatin1String("rows")).toArray()
                               : QJsonArray();
    int mapped = 0;
    for (const QJsonValue& row : map) {
        const QJsonArray cells = row.toArray();
        const QString to = cells.size() > 2 ? cells.at(2).toString().trimmed() : QString();
        if (to.isEmpty()) continue;
        ++mapped;
        if (s.named(to) == nullptr)
            said << tr("The layer map sends a layer to %1, which is no layer of the stack.").arg(to);
    }
    if (mapped == 0 && !map.isEmpty())
        said << tr("The layer map sends no layer of the file to the stack.");
    // The ports: each names conductors that are there.
    for (const Node& p : ports().children) {
        if (!p.enabled) continue;
        const QString who = p.label.isEmpty() ? p.tag : p.label;
        for (const QString& key : {QStringLiteral("conductor"), QStringLiteral("from"), QStringLiteral("to")}) {
            const QString named = textOf(p, key).trimmed();
            if (named.isEmpty() || named.compare(QLatin1String("ground"), Qt::CaseInsensitive) == 0) continue;
            if (s.named(named) == nullptr)
                said << tr("The port %1 names %2, which is no layer of the stack.").arg(who, named);
        }
    }
    if (ports().children.empty())
        said << tr("The model has no port: there is nothing to drive it with.");
    return said;
}

// ------------------------------------------------------------------ The tree

void Model::normalise()
{
    QSet<QString> tags;
    collectTags(root, tags);
    // A part of the tree itself is named by its kind ("rf_stack" is
    // "stack"): the same tag in every model, so that a tool or a session
    // can name it. The nodes a user adds are tagged by their prefix.
    std::function<void(Node&)> walk = [&](Node& n) {
        const NodeKind* k = nodeKind(n.type);
        if (n.tag.isEmpty() && k != nullptr && k->fixed) {
            const QString stem = n.type.startsWith(QLatin1String("rf_")) ? n.type.mid(3) : n.type;
            QString tag = stem;
            for (int i = 2; tags.contains(tag); ++i) tag = stem + QString::number(i);
            n.tag = tag;
            tags.insert(tag);
        }
        for (Node& c : n.children) walk(c);
    };
    walk(root);
    // A tag two nodes share would find the wrong one: the second gets one
    // of its own (a file written by hand, or copied together).
    tags.clear();
    std::function<void(Node&)> unique = [&](Node& n) {
        if (!n.tag.isEmpty() && tags.contains(n.tag)) {
            const NodeKind* k = nodeKind(n.type);
            const QString prefix = k != nullptr && !k->prefix.isEmpty() ? k->prefix : QStringLiteral("n");
            n.tag = newTag(prefix);
        }
        if (!n.tag.isEmpty()) tags.insert(n.tag);
        for (Node& c : n.children) unique(c);
    };
    unique(root);
}

Node* Model::find(const QString& tag)
{
    return root.find(tag);
}

const Node* Model::find(const QString& tag) const
{
    return root.find(tag);
}

Node* Model::parentOf(const QString& tag, int* index)
{
    std::function<Node*(Node&)> walk = [&](Node& parent) -> Node* {
        for (std::size_t i = 0; i < parent.children.size(); ++i) {
            if (parent.children[i].tag == tag) {
                if (index != nullptr) *index = int(i);
                return &parent;
            }
            if (Node* found = walk(parent.children[i])) return found;
        }
        return nullptr;
    };
    if (tag.isEmpty()) return nullptr;
    return walk(root);
}

QString Model::newTag(const QString& prefix) const
{
    QSet<QString> tags;
    collectTags(root, tags);
    const QString p = prefix.isEmpty() ? QStringLiteral("n") : prefix;
    for (int i = 1;; ++i) {
        const QString t = p + QString::number(i);
        if (!tags.contains(t)) return t;
    }
}

Node Model::make(const QString& type) const
{
    Node n;
    n.type = type;
    const NodeKind* k = nodeKind(type);
    if (k == nullptr) return n;
    n.tag = newTag(k->prefix);
    // A name of its own. A layer is named by the layer map, the vias and
    // the ports, so one made without a name could be named by nothing: the
    // metals are M1, M2..., the dielectrics ILD1..., the ports P1....
    const auto numbered = [this, &type](const QString& stem) {
        int count = 0;
        std::function<void(const Node&)> walk = [&](const Node& node) {
            count += node.type == type ? 1 : 0;
            for (const Node& c : node.children) walk(c);
        };
        walk(root);
        return stem + QString::number(count + 1);
    };
    if (type == QLatin1String("rf_metal")) n.label = numbered(QStringLiteral("M"));
    else if (type == QLatin1String("rf_dielectric")) n.label = numbered(QStringLiteral("ILD"));
    else if (type == QLatin1String("rf_via")) n.label = numbered(QStringLiteral("V"));
    else if (type == QLatin1String("rf_edge_port") || type == QLatin1String("rf_internal_port"))
        n.label = numbered(QStringLiteral("P"));
    else if (!k->fixed) n.label = k->title;
    for (const QString& d : k->defaults) {
        Node c;
        c.type = d;
        n.children.push_back(c);
    }
    return n;
}

Node* Model::add(const QString& parentTag, Node node, int index, QString* error)
{
    const auto no = [error](const QString& why) -> Node* {
        if (error != nullptr) *error = why;
        return nullptr;
    };
    Node* parent = find(parentTag);
    if (parent == nullptr) return no(tr("There is no node %1.").arg(parentTag));
    const NodeKind* pk = nodeKind(parent->type);
    const NodeKind* k = nodeKind(node.type);
    if (k == nullptr) return no(tr("%1 is not a kind of node.").arg(node.type));
    if (pk == nullptr || !pk->children.contains(node.type))
        return no(tr("A %1 may not go under %2.").arg(k->title, nameOf(*parent)));
    if (k->unique && parent->child(node.type) != nullptr)
        return no(tr("%1 has a %2 already.").arg(nameOf(*parent), k->title));
    // Unique tags, the node's and its children's.
    QSet<QString> tags;
    collectTags(root, tags);
    std::function<void(Node&)> retag = [&](Node& n) {
        const NodeKind* nk = nodeKind(n.type);
        if (n.tag.isEmpty() || tags.contains(n.tag)) {
            const QString prefix = nk != nullptr && !nk->prefix.isEmpty() ? nk->prefix : QStringLiteral("n");
            QString t = n.tag.isEmpty() ? prefix + QLatin1Char('1') : n.tag;
            for (int i = 1; tags.contains(t); ++i) t = prefix + QString::number(i);
            n.tag = t;
        }
        tags.insert(n.tag);
        for (Node& c : n.children) retag(c);
    };
    retag(node);
    std::vector<Node>& list = parent->children;
    int at = index < 0 || index > int(list.size()) ? int(list.size()) : index;
    if (index < 0 && parent->type == QLatin1String("rf_stack")) {
        // The order of the tree is the order in z, from the bottom up, so
        // a layer added goes where it belongs: the ground first (it is
        // under everything, and stackOf() puts it there wherever it is),
        // the substrate next, the cover last, and the rest before the
        // cover. (A ground was put at the end, after a cover added before
        // it - the phase A check of 24 October.)
        const bool groundFirst = !list.empty() && list.front().type == QLatin1String("rf_ground");
        if (node.type == QLatin1String("rf_ground")) {
            at = 0;
        } else if (node.type == QLatin1String("rf_substrate")) {
            at = groundFirst ? 1 : 0;
        } else if (node.type != QLatin1String("rf_cover")) {
            while (at > 0 && list[std::size_t(at - 1)].type == QLatin1String("rf_cover")) --at;
        }
    }
    list.insert(list.begin() + at, std::move(node));
    return &list[std::size_t(at)];
}

bool Model::remove(const QString& tag, QString* error)
{
    int index = -1;
    Node* parent = parentOf(tag, &index);
    if (parent == nullptr) {
        if (error != nullptr) *error = tr("There is no node %1.").arg(tag);
        return false;
    }
    const NodeKind* k = nodeKind(parent->children[std::size_t(index)].type);
    if (k != nullptr && k->fixed) {
        if (error != nullptr) *error = tr("%1 is a part of the tree itself: it is not deleted.").arg(k->title);
        return false;
    }
    parent->children.erase(parent->children.begin() + index);
    return true;
}

void Model::renameLayer(const QString& from, const QString& to)
{
    if (from.isEmpty() || to.isEmpty() || from == to) return;
    const auto rename = [&from, &to](Node& node, const QString& key) {
        if (textOf(node, key).trimmed() == from) node.props.insert(key, to);
    };
    for (Node& l : stackNode().children) {
        rename(l, QStringLiteral("from"));
        rename(l, QStringLiteral("to"));
    }
    for (Node& p : ports().children) {
        rename(p, QStringLiteral("conductor"));
        rename(p, QStringLiteral("from"));
        rename(p, QStringLiteral("to"));
    }
    if (Node* map = layout().child(QStringLiteral("rf_layer_map"))) {
        const QJsonArray rows = map->props.value(QLatin1String("rows")).toArray();
        QJsonArray out;
        bool changed = false;
        for (const QJsonValue& row : rows) {
            QJsonArray cells = row.toArray();
            if (cells.size() > 2 && cells.at(2).toString().trimmed() == from) {
                cells.replace(2, to);
                changed = true;
            }
            out.append(cells);
        }
        if (changed) map->props.insert(QStringLiteral("rows"), out);
    }
}

} // namespace qucs_s::rf25
