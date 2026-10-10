/*
 * fem_model.cpp - a multiphysics model: its node kinds, its tree, its file
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_model.h"

#include <QCoreApplication>
#include <QHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <functional>

namespace qucs_s::fem {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::fem::Model", text);
}

const QString FormatName = QStringLiteral("qucs-s multiphysics 1");

} // namespace

QString levelName(Level level)
{
    switch (level) {
    case Level::Domain: return QStringLiteral("domain");
    case Level::Boundary: return QStringLiteral("boundary");
    case Level::Point: return QStringLiteral("point");
    case Level::None: break;
    }
    return {};
}

Level levelOf(const QString& name)
{
    if (name.startsWith(QLatin1String("domain"))) return Level::Domain;
    if (name.startsWith(QLatin1String("boundar"))) return Level::Boundary;
    if (name.startsWith(QLatin1String("point"))) return Level::Point;
    return Level::None;
}

Dim PropertyDef::dim() const
{
    if (unit == QLatin1String("length")) return Dim::length();
    if (unit.isEmpty() || unit == QLatin1String("angle")) return Dim();
    const std::optional<Unit> u = parseUnit(unit);
    return u ? u->dim : Dim();
}

const PropertyDef* NodeKind::property(const QString& key) const
{
    for (const PropertyDef& p : properties)
        if (p.key == key) return &p;
    return nullptr;
}

// ------------------------------------------------------------------ The kinds

namespace {

using P = PropertyDef;

P expr(const QString& key, const QString& label, const QString& unit, const QString& def, const QString& tip = {})
{
    P p;
    p.key = key;
    p.label = label;
    p.kind = P::Expression;
    p.unit = unit;
    p.defaultValue = def;
    p.tooltip = tip;
    return p;
}

P spatial(const QString& key, const QString& label, const QString& unit, const QString& def, const QString& tip = {})
{
    P p = expr(key, label, unit, def, tip);
    p.spatial = true;
    return p;
}

P pair(const QString& key, const QString& label, const QString& unit, const QString& x, const QString& y,
       const QString& tip = {})
{
    P p;
    p.key = key;
    p.label = label;
    p.kind = P::Pair;
    p.unit = unit;
    p.defaultValue = QJsonArray{x, y};
    p.tooltip = tip;
    return p;
}

P choice(const QString& key, const QString& label, const QStringList& choices, const QStringList& labels,
         const QString& def, const QString& tip = {})
{
    P p;
    p.key = key;
    p.label = label;
    p.kind = P::Choice;
    p.choices = choices;
    p.choiceLabels = labels;
    p.defaultValue = def;
    p.tooltip = tip;
    return p;
}

P flag(const QString& key, const QString& label, bool def, const QString& tip = {})
{
    P p;
    p.key = key;
    p.label = label;
    p.kind = P::Bool;
    p.defaultValue = def;
    p.tooltip = tip;
    return p;
}

P integer(const QString& key, const QString& label, int def, const QString& tip = {})
{
    P p;
    p.key = key;
    p.label = label;
    p.kind = P::Integer;
    p.defaultValue = def;
    p.tooltip = tip;
    return p;
}

P text(const QString& key, const QString& label, const QString& def, const QString& tip = {})
{
    P p;
    p.key = key;
    p.label = label;
    p.kind = P::Text;
    p.defaultValue = def;
    p.tooltip = tip;
    return p;
}

P selectionProp(const QJsonObject& def = QJsonObject{{QStringLiteral("all"), true}}, Level level = Level::None)
{
    P p;
    p.key = QStringLiteral("selection");
    p.label = tr("Selection");
    p.kind = P::Selection;
    p.defaultValue = def;
    p.level = level;
    return p;
}

P objects(const QString& key, const QString& label, const QString& tip = {})
{
    P p;
    p.key = key;
    p.label = label;
    p.kind = P::Objects;
    p.defaultValue = QJsonArray();
    p.tooltip = tip;
    return p;
}

P shownIf(P p, const QString& condition)
{
    p.showIf = condition;
    return p;
}

QList<NodeKind> makeKinds()
{
    QList<NodeKind> k;
    const QJsonObject none{{QStringLiteral("numbers"), QJsonArray()}};
    auto add = [&k](NodeKind kind) { k.append(std::move(kind)); };

    // The tree's own parts.
    add({QStringLiteral("model"), tr("Model"), QString(), QStringLiteral("model"), Level::None,
         {text(QStringLiteral("description"), tr("Description"), QString())},
         {QStringLiteral("study")}, {}, true, true, QString(), QString()});
    add({QStringLiteral("definitions"), tr("Global Definitions"), QString(), QStringLiteral("definitions"), Level::None, {},
         {QStringLiteral("analytic"), QStringLiteral("interpolation")}, {}, true, true, QString(),
         tr("The parameters and functions every part of the model may use.")});
    {
        P rows;
        rows.key = QStringLiteral("rows");
        rows.label = tr("Parameters");
        rows.kind = P::Rows;
        rows.defaultValue = QJsonArray();
        add({QStringLiteral("parameters"), tr("Parameters"), QString(), QStringLiteral("parameters"), Level::None, {rows}, {},
             {}, true, true, QString(),
             tr("Named values, each an expression of those above it: w = 3[mm]. Geometry, materials and physics use them; "
                "a sweep changes them.")});
    }
    add({QStringLiteral("analytic"), tr("Analytic Function"), QStringLiteral("function"), QStringLiteral("function"), Level::None,
         {text(QStringLiteral("name"), tr("Function name"), QStringLiteral("fn")),
          text(QStringLiteral("arguments"), tr("Arguments"), QStringLiteral("x"), tr("Its arguments' names, split by commas")),
          text(QStringLiteral("expression"), tr("Expression"), QStringLiteral("x"), tr("Of the arguments and the parameters")),
          text(QStringLiteral("argunits"), tr("Arguments' units"), QString(), tr("One per argument, split by commas: K, m...")),
          text(QStringLiteral("unit"), tr("Function's unit"), QString())},
         {}, {}, false, false, QStringLiteral("an"), tr("A function of its arguments: sigmaT(T) = sigma0/(1+alpha*(T-T0)).")});
    {
        P rows;
        rows.key = QStringLiteral("data");
        rows.label = tr("Table (x, f(x))");
        rows.kind = P::Rows;
        rows.defaultValue = QJsonArray();
        add({QStringLiteral("interpolation"), tr("Interpolation"), QStringLiteral("function"), QStringLiteral("function"), Level::None,
             {text(QStringLiteral("name"), tr("Function name"), QStringLiteral("int")), rows,
              text(QStringLiteral("argunit"), tr("Argument's unit"), QString()),
              text(QStringLiteral("unit"), tr("Function's unit"), QString())},
             {}, {}, false, false, QStringLiteral("ip"),
             tr("A function of one argument given by a table: linear between its points, constant beyond them.")});
    }
    add({QStringLiteral("component"), tr("Component (2D)"), QString(), QStringLiteral("component"), Level::None,
         {choice(QStringLiteral("space"), tr("Space"), {QStringLiteral("2D"), QStringLiteral("axisymmetric")},
                 {tr("2D, in the plane"), tr("2D axisymmetric: about the y axis (r, z)")}, QStringLiteral("2D"),
                 tr("Axisymmetric: x is the distance r from the axis, y is z along it; what is integrated is integrated "
                    "round the axis (2πr)")),
          choice(QStringLiteral("unit"), tr("Length unit"),
                 {QStringLiteral("m"), QStringLiteral("cm"), QStringLiteral("mm"), QStringLiteral("um"), QStringLiteral("nm"),
                  QStringLiteral("in"), QStringLiteral("mil")},
                 {tr("m"), tr("cm"), tr("mm"), tr("µm"), tr("nm"), tr("in"), tr("mil")}, QStringLiteral("mm"),
                 tr("The geometry's unit: a plain number in it is in this unit")),
          shownIf(expr(QStringLiteral("thickness"), tr("Out-of-plane thickness"), QStringLiteral("m"), QStringLiteral("1[m]"),
                       tr("What a charge, a current, a power or a force of the 2D model is for: per this length out of the plane")),
                  QStringLiteral("space=2D"))},
         {QStringLiteral("electrostatics"), QStringLiteral("currents"), QStringLiteral("heat"), QStringLiteral("solid")}, {}, true, true,
         QString(), tr("The model's 2D space: its geometry, materials, physics and mesh.")});
    add({QStringLiteral("selections"), tr("Definitions"), QString(), QStringLiteral("definitions"), Level::None, {},
         {QStringLiteral("selection")}, {}, true, true, QString(), tr("Named selections, which the physics may use.")});
    add({QStringLiteral("selection"), tr("Selection"), QStringLiteral("selection"), QStringLiteral("selection"), Level::Domain,
         {choice(QStringLiteral("level"), tr("Level"), {QStringLiteral("domain"), QStringLiteral("boundary"), QStringLiteral("point")},
                 {tr("Domains"), tr("Boundaries"), tr("Points")}, QStringLiteral("domain")),
          selectionProp(none)},
         {}, {}, false, false, QStringLiteral("sel"), tr("A named selection: {\"named\": \"its name\"} in a rule.")});

    // Geometry.
    const QStringList geometryFeatures = {
        QStringLiteral("rectangle"), QStringLiteral("circle"), QStringLiteral("ellipse"), QStringLiteral("polygon"),
        QStringLiteral("point"), QStringLiteral("move"), QStringLiteral("rotate"), QStringLiteral("scale"),
        QStringLiteral("mirror"), QStringLiteral("array"), QStringLiteral("union"), QStringLiteral("difference"),
        QStringLiteral("intersection"), QStringLiteral("import")};
    add({QStringLiteral("geometry"), tr("Geometry"), QString(), QStringLiteral("geometry"), Level::None, {}, geometryFeatures,
         {QStringLiteral("form union")}, true, true, QString(),
         tr("A sequence of features, built in order: each makes objects of the parameters. Form Union makes them one: "
            "the domains, boundaries and points the physics are on.")});
    const P rotation = expr(QStringLiteral("rotation"), tr("Rotation"), QStringLiteral("angle"), QStringLiteral("0"),
                            tr("Degrees, counter-clockwise (or [rad])"));
    add({QStringLiteral("rectangle"), tr("Rectangle"), QStringLiteral("geometry"), QStringLiteral("rectangle"), Level::None,
         {choice(QStringLiteral("base"), tr("Position at"), {QStringLiteral("corner"), QStringLiteral("center")},
                 {tr("Corner"), tr("Center")}, QStringLiteral("corner")),
          pair(QStringLiteral("position"), tr("Position"), QStringLiteral("length"), QStringLiteral("0"), QStringLiteral("0")),
          pair(QStringLiteral("size"), tr("Width, height"), QStringLiteral("length"), QStringLiteral("1"), QStringLiteral("1")),
          rotation},
         {}, {}, false, false, QStringLiteral("r"), QString()});
    add({QStringLiteral("circle"), tr("Circle"), QStringLiteral("geometry"), QStringLiteral("circle"), Level::None,
         {pair(QStringLiteral("center"), tr("Center"), QStringLiteral("length"), QStringLiteral("0"), QStringLiteral("0")),
          expr(QStringLiteral("radius"), tr("Radius"), QStringLiteral("length"), QStringLiteral("1")),
          expr(QStringLiteral("sector"), tr("Sector angle"), QStringLiteral("angle"), QStringLiteral("360"),
               tr("Degrees: 360 a whole circle")),
          rotation},
         {}, {}, false, false, QStringLiteral("c"), QString()});
    add({QStringLiteral("ellipse"), tr("Ellipse"), QStringLiteral("geometry"), QStringLiteral("ellipse"), Level::None,
         {pair(QStringLiteral("center"), tr("Center"), QStringLiteral("length"), QStringLiteral("0"), QStringLiteral("0")),
          pair(QStringLiteral("semiaxes"), tr("Semiaxes"), QStringLiteral("length"), QStringLiteral("2"), QStringLiteral("1")),
          rotation},
         {}, {}, false, false, QStringLiteral("e"), QString()});
    {
        P points;
        points.key = QStringLiteral("points");
        points.label = tr("Corners (x, y)");
        points.kind = P::Points;
        points.unit = QStringLiteral("length");
        points.defaultValue = QJsonArray{QJsonArray{QStringLiteral("0"), QStringLiteral("0")},
                                         QJsonArray{QStringLiteral("1"), QStringLiteral("0")},
                                         QJsonArray{QStringLiteral("0"), QStringLiteral("1")}};
        add({QStringLiteral("polygon"), tr("Polygon"), QStringLiteral("geometry"), QStringLiteral("polygon"), Level::None,
             {points, flag(QStringLiteral("closed"), tr("Closed"), true, tr("Closed: a solid; open: a polyline, its segments boundaries"))},
             {}, {}, false, false, QStringLiteral("pol"), QString()});
    }
    add({QStringLiteral("point"), tr("Point"), QStringLiteral("geometry"), QStringLiteral("point"), Level::None,
         {pair(QStringLiteral("position"), tr("Position"), QStringLiteral("length"), QStringLiteral("0"), QStringLiteral("0"))},
         {}, {}, false, false, QStringLiteral("pt"), tr("A point of the geometry: for a point source, or a value there.")});
    const P input = objects(QStringLiteral("input"), tr("Input objects"));
    const P keep = flag(QStringLiteral("keep"), tr("Keep input objects"), false);
    add({QStringLiteral("move"), tr("Move"), QStringLiteral("geometry"), QStringLiteral("move"), Level::None,
         {input, pair(QStringLiteral("displacement"), tr("Displacement"), QStringLiteral("length"), QStringLiteral("0"), QStringLiteral("0")),
          shownIf(keep, QString())},
         {}, {}, false, false, QStringLiteral("mov"), tr("Moves objects (a copy, its input kept).")});
    add({QStringLiteral("rotate"), tr("Rotate"), QStringLiteral("geometry"), QStringLiteral("rotate"), Level::None,
         {input, expr(QStringLiteral("angle"), tr("Angle"), QStringLiteral("angle"), QStringLiteral("90"), tr("Degrees, counter-clockwise")),
          pair(QStringLiteral("center"), tr("About"), QStringLiteral("length"), QStringLiteral("0"), QStringLiteral("0")), keep},
         {}, {}, false, false, QStringLiteral("rot"), QString()});
    add({QStringLiteral("scale"), tr("Scale"), QStringLiteral("geometry"), QStringLiteral("scale"), Level::None,
         {input, pair(QStringLiteral("factor"), tr("Factor (x, y)"), QString(), QStringLiteral("1"), QStringLiteral("1")),
          pair(QStringLiteral("center"), tr("About"), QStringLiteral("length"), QStringLiteral("0"), QStringLiteral("0")), keep},
         {}, {}, false, false, QStringLiteral("sca"), QString()});
    add({QStringLiteral("mirror"), tr("Mirror"), QStringLiteral("geometry"), QStringLiteral("mirror"), Level::None,
         {input, pair(QStringLiteral("point"), tr("Point on the axis"), QStringLiteral("length"), QStringLiteral("0"), QStringLiteral("0")),
          pair(QStringLiteral("normal"), tr("Axis's normal"), QString(), QStringLiteral("1"), QStringLiteral("0")), keep},
         {}, {}, false, false, QStringLiteral("mir"), QString()});
    add({QStringLiteral("array"), tr("Array"), QStringLiteral("geometry"), QStringLiteral("array"), Level::None,
         {input, pair(QStringLiteral("count"), tr("Copies (x, y)"), QString(), QStringLiteral("2"), QStringLiteral("1")),
          pair(QStringLiteral("displacement"), tr("Displacement"), QStringLiteral("length"), QStringLiteral("1"), QStringLiteral("0"))},
         {}, {}, false, false, QStringLiteral("arr"), tr("Copies of objects in rows and columns.")});
    add({QStringLiteral("union"), tr("Union"), QStringLiteral("geometry"), QStringLiteral("union"), Level::None,
         {input, flag(QStringLiteral("interior"), tr("Keep interior boundaries"), false)},
         {}, {}, false, false, QStringLiteral("uni"), tr("Objects made one.")});
    add({QStringLiteral("difference"), tr("Difference"), QStringLiteral("geometry"), QStringLiteral("difference"), Level::None,
         {input, objects(QStringLiteral("tools"), tr("Objects to subtract")), keep},
         {}, {}, false, false, QStringLiteral("dif"), tr("Objects with others cut out of them.")});
    add({QStringLiteral("intersection"), tr("Intersection"), QStringLiteral("geometry"), QStringLiteral("intersection"), Level::None,
         {input, keep}, {}, {}, false, false, QStringLiteral("isc"), tr("What objects have in common.")});
    {
        P file;
        file.key = QStringLiteral("file");
        file.label = tr("File");
        file.kind = P::File;
        file.defaultValue = QString();
        file.tooltip = tr("A DXF or SVG drawing, its path relative to the model's folder");
        add({QStringLiteral("import"), tr("Import"), QStringLiteral("geometry"), QStringLiteral("import"), Level::None,
             {file,
              choice(QStringLiteral("format"), tr("Format"), {QStringLiteral("auto"), QStringLiteral("dxf"), QStringLiteral("svg")},
                     {tr("By its suffix"), tr("DXF (AutoCAD)"), tr("SVG")}, QStringLiteral("auto")),
              shownIf(text(QStringLiteral("layers"), tr("Layers"), QString(), tr("A DXF's layers to read, split by commas (empty: all)")),
                      QStringLiteral("format!=svg")),
              choice(QStringLiteral("unit"), tr("The file's unit"),
                     {QStringLiteral("auto"), QStringLiteral("m"), QStringLiteral("cm"), QStringLiteral("mm"), QStringLiteral("um"),
                      QStringLiteral("nm"), QStringLiteral("in"), QStringLiteral("mil"), QStringLiteral("px")},
                     {tr("As it says (else the geometry's)"), tr("m"), tr("cm"), tr("mm"), tr("µm"), tr("nm"), tr("in"), tr("mil"),
                      tr("px (1/96 in)")},
                     QStringLiteral("auto")),
              expr(QStringLiteral("scale"), tr("Scale"), QString(), QStringLiteral("1"), tr("Its coordinates times this")),
              pair(QStringLiteral("position"), tr("Displacement"), QStringLiteral("length"), QStringLiteral("0"), QStringLiteral("0")),
              choice(QStringLiteral("curves"), tr("Closed curves"), {QStringLiteral("solids"), QStringLiteral("curves")},
                     {tr("Solids (inner ones holes)"), tr("Curves only")}, QStringLiteral("solids"))},
             {}, {}, false, false, QStringLiteral("imp"),
             tr("A drawing's lines, arcs, circles, ellipses, polylines and splines (DXF), or its paths, rectangles, circles, "
                "ellipses, polygons and lines (SVG): its closed curves solids - one inside another a hole - the others "
                "curves. Read again when the file changes.")});
    }
    add({QStringLiteral("form union"), tr("Form Union"), QStringLiteral("geometry"), QStringLiteral("formunion"), Level::None,
         {expr(QStringLiteral("tolerance"), tr("Repair tolerance"), QString(), QStringLiteral("1e-6"),
               tr("Relative to the geometry's size: points nearer than this are one"))},
         {}, {}, true, true, QStringLiteral("fin"),
         tr("Every object's edges, split where they cross: the domains they close, the boundaries and points.")});

    // Materials.
    add({QStringLiteral("materials"), tr("Materials"), QString(), QStringLiteral("materials"), Level::None, {},
         {QStringLiteral("material")}, {}, true, true, QString(),
         tr("Each material on its domains; a later one overrides an earlier on the domains they share.")});
    add({QStringLiteral("material"), tr("Material"), QStringLiteral("material"), QStringLiteral("material"), Level::Domain,
         {selectionProp(none), text(QStringLiteral("library"), tr("From the library"), QString()),
          spatial(QStringLiteral("epsilonr"), tr("Relative permittivity εr"), QString(), QString()),
          spatial(QStringLiteral("sigma"), tr("Electrical conductivity σ"), QStringLiteral("S/m"), QString()),
          spatial(QStringLiteral("k"), tr("Thermal conductivity k"), QStringLiteral("W/(m*K)"), QString()),
          spatial(QStringLiteral("rho"), tr("Density ρ"), QStringLiteral("kg/m^3"), QString()),
          spatial(QStringLiteral("Cp"), tr("Heat capacity Cp"), QStringLiteral("J/(kg*K)"), QString()),
          spatial(QStringLiteral("mur"), tr("Relative permeability µr"), QString(), QString()),
          spatial(QStringLiteral("E"), tr("Young's modulus E"), QStringLiteral("Pa"), QString()),
          spatial(QStringLiteral("nu"), tr("Poisson's ratio ν"), QString(), QString()),
          spatial(QStringLiteral("alpha"), tr("Thermal expansion α"), QStringLiteral("1/K"), QString()),
          spatial(QStringLiteral("n"), tr("Refractive index n"), QString(), QString())},
         {}, {}, false, false, QStringLiteral("mat"),
         tr("Its properties, each an expression (of T too, for a temperature-dependent one). Those empty are not defined.")});

    // Electrostatics.
    const P source = choice(QStringLiteral("source"), tr("From"), {QStringLiteral("material"), QStringLiteral("user")},
                            {tr("Material"), tr("User defined")}, QStringLiteral("material"));
    add({QStringLiteral("electrostatics"), tr("Electrostatics"), QStringLiteral("physics"), QStringLiteral("es"), Level::Domain,
         {selectionProp(),
          choice(QStringLiteral("order"), tr("Element order"), {QStringLiteral("1"), QStringLiteral("2")}, {tr("Linear"), tr("Quadratic")},
                 QStringLiteral("2"))},
         {QStringLiteral("electrostatics/charge"), QStringLiteral("electrostatics/spacecharge"), QStringLiteral("electrostatics/zerocharge"),
          QStringLiteral("electrostatics/ground"), QStringLiteral("electrostatics/potential"), QStringLiteral("electrostatics/terminal"),
          QStringLiteral("electrostatics/surfacecharge"), QStringLiteral("electrostatics/floating")},
         {QStringLiteral("electrostatics/charge"), QStringLiteral("electrostatics/zerocharge")}, false, false, QStringLiteral("es"),
         tr("The electric potential V of charges at rest: -∇·(ε0 εr ∇V) = ρ. Terminals give a capacitance matrix.")});
    add({QStringLiteral("electrostatics/charge"), tr("Charge Conservation"), QStringLiteral("es"), QStringLiteral("domainfeature"),
         Level::Domain,
         {selectionProp(), source,
          shownIf(spatial(QStringLiteral("epsilonr"), tr("Relative permittivity εr"), QString(), QStringLiteral("1")),
                  QStringLiteral("source=user"))},
         {}, {}, false, false, QStringLiteral("ccn"), tr("D = ε0 εr E on its domains, εr from their material or as given.")});
    add({QStringLiteral("electrostatics/spacecharge"), tr("Space Charge Density"), QStringLiteral("es"), QStringLiteral("domainfeature"),
         Level::Domain, {selectionProp(none), spatial(QStringLiteral("rho"), tr("Space charge density ρv"), QStringLiteral("C/m^3"), QStringLiteral("0"))},
         {}, {}, false, false, QStringLiteral("scd"), QString()});
    add({QStringLiteral("electrostatics/zerocharge"), tr("Zero Charge"), QStringLiteral("es"), QStringLiteral("boundaryfeature"),
         Level::Boundary, {selectionProp()}, {}, {}, false, false, QStringLiteral("zc"),
         tr("No charge on the boundary: n·D = 0. Where no other condition is.")});
    add({QStringLiteral("electrostatics/ground"), tr("Ground"), QStringLiteral("es"), QStringLiteral("ground"), Level::Boundary,
         {selectionProp(none)}, {}, {}, false, false, QStringLiteral("gnd"), tr("V = 0.")});
    add({QStringLiteral("electrostatics/potential"), tr("Electric Potential"), QStringLiteral("es"), QStringLiteral("boundaryfeature"),
         Level::Boundary, {selectionProp(none), spatial(QStringLiteral("V0"), tr("Electric potential V0"), QStringLiteral("V"), QStringLiteral("1[V]"))},
         {}, {}, false, false, QStringLiteral("pot"), QString()});
    add({QStringLiteral("electrostatics/terminal"), tr("Terminal"), QStringLiteral("es"), QStringLiteral("terminal"), Level::Boundary,
         {selectionProp(none), text(QStringLiteral("name"), tr("Terminal name"), QStringLiteral("1")),
          expr(QStringLiteral("V0"), tr("Voltage"), QStringLiteral("V"), QStringLiteral("1[V]"))},
         {}, {}, false, false, QStringLiteral("term"),
         tr("A conductor at a voltage. Its charge is es.Q_<name>; with terminals, each at 1 V in turn gives the "
            "capacitance matrix es.C<i><j> (es.C_i_j).")});
    add({QStringLiteral("electrostatics/surfacecharge"), tr("Surface Charge Density"), QStringLiteral("es"),
         QStringLiteral("boundaryfeature"), Level::Boundary,
         {selectionProp(none), spatial(QStringLiteral("rhos"), tr("Surface charge density ρs"), QStringLiteral("C/m^2"), QStringLiteral("0"))},
         {}, {}, false, false, QStringLiteral("sc"), QString()});
    add({QStringLiteral("electrostatics/floating"), tr("Floating Potential"), QStringLiteral("es"), QStringLiteral("terminal"),
         Level::Boundary,
         {selectionProp(none), expr(QStringLiteral("Q0"), tr("Charge"), QStringLiteral("C"), QStringLiteral("0"),
                                    tr("Over the out-of-plane thickness"))},
         {}, {}, false, false, QStringLiteral("fp"), tr("A conductor not connected: one potential, of the charge given.")});

    // Electric currents.
    add({QStringLiteral("currents"), tr("Electric Currents"), QStringLiteral("physics"), QStringLiteral("ec"), Level::Domain,
         {selectionProp(),
          choice(QStringLiteral("order"), tr("Element order"), {QStringLiteral("1"), QStringLiteral("2")}, {tr("Linear"), tr("Quadratic")},
                 QStringLiteral("2"))},
         {QStringLiteral("currents/conservation"), QStringLiteral("currents/source"), QStringLiteral("currents/insulation"),
          QStringLiteral("currents/ground"), QStringLiteral("currents/potential"), QStringLiteral("currents/terminal"),
          QStringLiteral("currents/normalcurrent"), QStringLiteral("currents/floating")},
         {QStringLiteral("currents/conservation"), QStringLiteral("currents/insulation")}, false, false, QStringLiteral("ec"),
         tr("Steady currents in conductors: -∇·(σ ∇V) = Qj. Terminals give a conductance matrix and resistances.")});
    add({QStringLiteral("currents/conservation"), tr("Current Conservation"), QStringLiteral("ec"), QStringLiteral("domainfeature"),
         Level::Domain,
         {selectionProp(), source,
          shownIf(spatial(QStringLiteral("sigma"), tr("Electrical conductivity σ"), QStringLiteral("S/m"), QStringLiteral("1[S/m]")),
                  QStringLiteral("source=user"))},
         {}, {}, false, false, QStringLiteral("cuc"), tr("J = σ E on its domains, σ from their material or as given.")});
    add({QStringLiteral("currents/source"), tr("Current Source"), QStringLiteral("ec"), QStringLiteral("domainfeature"), Level::Domain,
         {selectionProp(none), spatial(QStringLiteral("Qj"), tr("Current source Qj"), QStringLiteral("A/m^3"), QStringLiteral("0"))},
         {}, {}, false, false, QStringLiteral("cs"), QString()});
    add({QStringLiteral("currents/insulation"), tr("Electric Insulation"), QStringLiteral("ec"), QStringLiteral("boundaryfeature"),
         Level::Boundary, {selectionProp()}, {}, {}, false, false, QStringLiteral("ei"),
         tr("No current through the boundary: n·J = 0. Where no other condition is.")});
    add({QStringLiteral("currents/ground"), tr("Ground"), QStringLiteral("ec"), QStringLiteral("ground"), Level::Boundary,
         {selectionProp(none)}, {}, {}, false, false, QStringLiteral("gnd"), tr("V = 0.")});
    add({QStringLiteral("currents/potential"), tr("Electric Potential"), QStringLiteral("ec"), QStringLiteral("boundaryfeature"),
         Level::Boundary, {selectionProp(none), spatial(QStringLiteral("V0"), tr("Electric potential V0"), QStringLiteral("V"), QStringLiteral("1[V]"))},
         {}, {}, false, false, QStringLiteral("pot"), QString()});
    add({QStringLiteral("currents/terminal"), tr("Terminal"), QStringLiteral("ec"), QStringLiteral("terminal"), Level::Boundary,
         {selectionProp(none), text(QStringLiteral("name"), tr("Terminal name"), QStringLiteral("1")),
          choice(QStringLiteral("drive"), tr("Driven by"), {QStringLiteral("voltage"), QStringLiteral("current")},
                 {tr("Voltage"), tr("Current")}, QStringLiteral("voltage")),
          shownIf(expr(QStringLiteral("V0"), tr("Voltage"), QStringLiteral("V"), QStringLiteral("1[V]")), QStringLiteral("drive=voltage")),
          shownIf(expr(QStringLiteral("I0"), tr("Current"), QStringLiteral("A"), QStringLiteral("1[A]"),
                       tr("Into the domain, over the out-of-plane thickness")),
                  QStringLiteral("drive=current"))},
         {}, {}, false, false, QStringLiteral("term"),
         tr("A contact at a voltage, or fed a current. Its current is ec.I_<name>; with terminals, each at 1 V in turn "
            "gives the conductance matrix ec.G<i><j> and, of two, the resistance ec.R.")});
    add({QStringLiteral("currents/normalcurrent"), tr("Normal Current Density"), QStringLiteral("ec"), QStringLiteral("boundaryfeature"),
         Level::Boundary,
         {selectionProp(none), spatial(QStringLiteral("Jn"), tr("Normal current density (inward)"), QStringLiteral("A/m^2"), QStringLiteral("0"))},
         {}, {}, false, false, QStringLiteral("ncd"), QString()});
    add({QStringLiteral("currents/floating"), tr("Floating Potential"), QStringLiteral("ec"), QStringLiteral("terminal"),
         Level::Boundary,
         {selectionProp(none), expr(QStringLiteral("I0"), tr("Current"), QStringLiteral("A"), QStringLiteral("0"))},
         {}, {}, false, false, QStringLiteral("fp"), tr("A contact of one potential, fed the current given (none: a floating one).")});

    // Heat transfer.
    add({QStringLiteral("heat"), tr("Heat Transfer in Solids"), QStringLiteral("physics"), QStringLiteral("ht"), Level::Domain,
         {selectionProp(),
          choice(QStringLiteral("order"), tr("Element order"), {QStringLiteral("1"), QStringLiteral("2")}, {tr("Linear"), tr("Quadratic")},
                 QStringLiteral("2"))},
         {QStringLiteral("heat/solid"), QStringLiteral("heat/init"), QStringLiteral("heat/source"), QStringLiteral("heat/insulation"),
          QStringLiteral("heat/temperature"), QStringLiteral("heat/flux"), QStringLiteral("heat/radiation"), QStringLiteral("heat/linesource")},
         {QStringLiteral("heat/solid"), QStringLiteral("heat/insulation"), QStringLiteral("heat/init")}, false, false, QStringLiteral("ht"),
         tr("The temperature T of conduction: ρ Cp ∂T/∂t - ∇·(k ∇T) = Q (at steady state without its first term).")});
    add({QStringLiteral("heat/solid"), tr("Solid"), QStringLiteral("ht"), QStringLiteral("domainfeature"), Level::Domain,
         {selectionProp(), source,
          shownIf(spatial(QStringLiteral("k"), tr("Thermal conductivity k"), QStringLiteral("W/(m*K)"), QStringLiteral("1[W/(m*K)]")),
                  QStringLiteral("source=user")),
          shownIf(spatial(QStringLiteral("rho"), tr("Density ρ"), QStringLiteral("kg/m^3"), QStringLiteral("1000[kg/m^3]")),
                  QStringLiteral("source=user")),
          shownIf(spatial(QStringLiteral("Cp"), tr("Heat capacity Cp"), QStringLiteral("J/(kg*K)"), QStringLiteral("1000[J/(kg*K)]")),
                  QStringLiteral("source=user"))},
         {}, {}, false, false, QStringLiteral("sld"),
         tr("q = -k ∇T on its domains; k (and for a study in time ρ and Cp) from their material or as given.")});
    add({QStringLiteral("heat/init"), tr("Initial Values"), QStringLiteral("ht"), QStringLiteral("domainfeature"), Level::Domain,
         {selectionProp(), spatial(QStringLiteral("T0"), tr("Temperature T"), QStringLiteral("K"), QStringLiteral("293.15[K]"),
                                   tr("Of x and y: where a study in time starts, and Newton's first guess"))},
         {}, {}, false, false, QStringLiteral("init"), tr("The temperature at the start.")});
    add({QStringLiteral("heat/radiation"), tr("Surface-to-Ambient Radiation"), QStringLiteral("ht"), QStringLiteral("boundaryfeature"),
         Level::Boundary,
         {selectionProp(none), spatial(QStringLiteral("epsilon"), tr("Surface emissivity ε"), QString(), QStringLiteral("0.9")),
          spatial(QStringLiteral("Tamb"), tr("Ambient temperature"), QStringLiteral("K"), QStringLiteral("293.15[K]"))},
         {}, {}, false, false, QStringLiteral("rad"),
         tr("Heat radiated to surroundings at Tamb: -n·q = εσ(Tamb⁴ - T⁴). It makes the problem nonlinear: solved by Newton's "
            "method.")});
    add({QStringLiteral("heat/source"), tr("Heat Source"), QStringLiteral("ht"), QStringLiteral("domainfeature"), Level::Domain,
         {selectionProp(none),
          choice(QStringLiteral("kind"), tr("Given as"), {QStringLiteral("density"), QStringLiteral("power")},
                 {tr("Power density"), tr("Total power")}, QStringLiteral("density")),
          shownIf(spatial(QStringLiteral("Q0"), tr("Heat source Q0"), QStringLiteral("W/m^3"), QStringLiteral("0")), QStringLiteral("kind=density")),
          shownIf(expr(QStringLiteral("P0"), tr("Power"), QStringLiteral("W"), QStringLiteral("1[W]"),
                       tr("Over its domains and the out-of-plane thickness")),
                  QStringLiteral("kind=power"))},
         {}, {}, false, false, QStringLiteral("hs"), QString()});
    add({QStringLiteral("heat/insulation"), tr("Thermal Insulation"), QStringLiteral("ht"), QStringLiteral("boundaryfeature"),
         Level::Boundary, {selectionProp()}, {}, {}, false, false, QStringLiteral("ins"),
         tr("No heat through the boundary: -n·q = 0. Where no other condition is.")});
    add({QStringLiteral("heat/temperature"), tr("Temperature"), QStringLiteral("ht"), QStringLiteral("temperature"), Level::Boundary,
         {selectionProp(none), spatial(QStringLiteral("T0"), tr("Temperature T0"), QStringLiteral("K"), QStringLiteral("293.15[K]"))},
         {}, {}, false, false, QStringLiteral("temp"), QString()});
    add({QStringLiteral("heat/flux"), tr("Heat Flux"), QStringLiteral("ht"), QStringLiteral("boundaryfeature"), Level::Boundary,
         {selectionProp(none),
          choice(QStringLiteral("kind"), tr("Flux"), {QStringLiteral("general"), QStringLiteral("convective")},
                 {tr("General inward flux"), tr("Convective: h (Text - T)")}, QStringLiteral("convective")),
          shownIf(spatial(QStringLiteral("q0"), tr("Inward heat flux q0"), QStringLiteral("W/m^2"), QStringLiteral("0")), QStringLiteral("kind=general")),
          shownIf(spatial(QStringLiteral("h"), tr("Heat transfer coefficient h"), QStringLiteral("W/(m^2*K)"), QStringLiteral("10[W/(m^2*K)]")),
                  QStringLiteral("kind=convective")),
          shownIf(spatial(QStringLiteral("Text"), tr("External temperature Text"), QStringLiteral("K"), QStringLiteral("293.15[K]")),
                  QStringLiteral("kind=convective"))},
         {}, {}, false, false, QStringLiteral("hf"), QString()});
    add({QStringLiteral("heat/linesource"), tr("Boundary Heat Source"), QStringLiteral("ht"), QStringLiteral("boundaryfeature"),
         Level::Boundary,
         {selectionProp(none), spatial(QStringLiteral("Qb"), tr("Heat source per area Qb"), QStringLiteral("W/m^2"), QStringLiteral("0"))},
         {}, {}, false, false, QStringLiteral("bhs"), tr("Heat made on a boundary: a thin heater.")});

    // Solid mechanics.
    add({QStringLiteral("solid"), tr("Solid Mechanics"), QStringLiteral("physics"), QStringLiteral("solid"), Level::Domain,
         {selectionProp(),
          shownIf(choice(QStringLiteral("model"), tr("2D approximation"), {QStringLiteral("planestrain"), QStringLiteral("planestress")},
                         {tr("Plane strain (long in z: no strain out of the plane)"), tr("Plane stress (thin in z: no stress out of the plane)")},
                         QStringLiteral("planestrain")),
                  QString()),
          choice(QStringLiteral("order"), tr("Element order"), {QStringLiteral("1"), QStringLiteral("2")}, {tr("Linear"), tr("Quadratic")},
                 QStringLiteral("2"))},
         {QStringLiteral("solid/elastic"), QStringLiteral("solid/free"), QStringLiteral("solid/fixed"), QStringLiteral("solid/displacement"),
          QStringLiteral("solid/roller"), QStringLiteral("solid/load"), QStringLiteral("solid/bodyload")},
         {QStringLiteral("solid/elastic"), QStringLiteral("solid/free")}, false, false, QStringLiteral("solid"),
         tr("The displacement (u, v) of a linear elastic solid: -∇·σ = f, σ = D (ε - ε0), ε0 a thermal strain. In the plane, "
            "in plane strain or plane stress; about an axis, axisymmetric (the component's space).")});
    add({QStringLiteral("solid/elastic"), tr("Linear Elastic Material"), QStringLiteral("solid"), QStringLiteral("domainfeature"), Level::Domain,
         {selectionProp(), source,
          shownIf(spatial(QStringLiteral("E"), tr("Young's modulus E"), QStringLiteral("Pa"), QStringLiteral("200[GPa]")), QStringLiteral("source=user")),
          shownIf(spatial(QStringLiteral("nu"), tr("Poisson's ratio ν"), QString(), QStringLiteral("0.3")), QStringLiteral("source=user")),
          flag(QStringLiteral("thermal"), tr("Thermal expansion"), false, tr("A strain α (T - Tref), T from a heat physics or as given")),
          shownIf(spatial(QStringLiteral("alpha"), tr("Coefficient of thermal expansion α"), QStringLiteral("1/K"), QStringLiteral("1e-5[1/K]")),
                  QStringLiteral("source=user")),
          shownIf(spatial(QStringLiteral("T"), tr("Temperature"), QStringLiteral("K"), QStringLiteral("T"),
                          tr("An expression: T (a heat physics' field), ht.T, or a value")),
                  QStringLiteral("thermal=true")),
          shownIf(spatial(QStringLiteral("Tref"), tr("Strain-free temperature Tref"), QStringLiteral("K"), QStringLiteral("293.15[K]")),
                  QStringLiteral("thermal=true"))},
         {}, {}, false, false, QStringLiteral("lemm"), tr("Hooke's law on its domains: E and ν (and α) from their material or as given.")});
    add({QStringLiteral("solid/free"), tr("Free"), QStringLiteral("solid"), QStringLiteral("boundaryfeature"), Level::Boundary,
         {selectionProp()}, {}, {}, false, false, QStringLiteral("free"), tr("No load, no constraint: where no other condition is.")});
    add({QStringLiteral("solid/fixed"), tr("Fixed Constraint"), QStringLiteral("solid"), QStringLiteral("ground"), Level::Boundary,
         {selectionProp(none)}, {}, {}, false, false, QStringLiteral("fix"), tr("u = v = 0.")});
    add({QStringLiteral("solid/displacement"), tr("Prescribed Displacement"), QStringLiteral("solid"), QStringLiteral("boundaryfeature"),
         Level::Boundary,
         {selectionProp(none), flag(QStringLiteral("x"), tr("Prescribed in x (r)"), true),
          shownIf(spatial(QStringLiteral("u0"), tr("Displacement u0"), QStringLiteral("m"), QStringLiteral("0[m]")), QStringLiteral("x=true")),
          flag(QStringLiteral("y"), tr("Prescribed in y (z)"), true),
          shownIf(spatial(QStringLiteral("v0"), tr("Displacement v0"), QStringLiteral("m"), QStringLiteral("0[m]")), QStringLiteral("y=true"))},
         {}, {}, false, false, QStringLiteral("disp"), tr("A displacement given, in x, in y or both: of x, y and t.")});
    add({QStringLiteral("solid/roller"), tr("Roller"), QStringLiteral("solid"), QStringLiteral("boundaryfeature"), Level::Boundary,
         {selectionProp(none)}, {}, {}, false, false, QStringLiteral("rol"),
         tr("No displacement across the boundary, free along it: a symmetry plane.")});
    add({QStringLiteral("solid/load"), tr("Boundary Load"), QStringLiteral("solid"), QStringLiteral("boundaryfeature"), Level::Boundary,
         {selectionProp(none),
          choice(QStringLiteral("kind"), tr("Load"), {QStringLiteral("force"), QStringLiteral("pressure")},
                 {tr("Force per area (Fx, Fy)"), tr("Pressure (pushing in)")}, QStringLiteral("force")),
          shownIf(spatial(QStringLiteral("Fx"), tr("Fx"), QStringLiteral("N/m^2"), QStringLiteral("0[N/m^2]")), QStringLiteral("kind=force")),
          shownIf(spatial(QStringLiteral("Fy"), tr("Fy"), QStringLiteral("N/m^2"), QStringLiteral("0[N/m^2]")), QStringLiteral("kind=force")),
          shownIf(spatial(QStringLiteral("p"), tr("Pressure p"), QStringLiteral("Pa"), QStringLiteral("0[Pa]")), QStringLiteral("kind=pressure"))},
         {}, {}, false, false, QStringLiteral("bndl"), tr("A force on the boundary, per area (per length times the thickness).")});
    add({QStringLiteral("solid/bodyload"), tr("Body Load"), QStringLiteral("solid"), QStringLiteral("domainfeature"), Level::Domain,
         {selectionProp(none), spatial(QStringLiteral("fx"), tr("fx"), QStringLiteral("N/m^3"), QStringLiteral("0[N/m^3]")),
          spatial(QStringLiteral("fy"), tr("fy"), QStringLiteral("N/m^3"), QStringLiteral("0[N/m^3]"),
                  tr("Its weight: -rho*g_const, with rho a parameter or a value"))},
         {}, {}, false, false, QStringLiteral("bl"), tr("A force per volume: weight, an inertial force.")});

    // Mesh.
    const QStringList presets = {QStringLiteral("extremely fine"), QStringLiteral("extra fine"), QStringLiteral("finer"),
                                 QStringLiteral("fine"), QStringLiteral("normal"), QStringLiteral("coarse"),
                                 QStringLiteral("coarser"), QStringLiteral("extra coarse"), QStringLiteral("extremely coarse")};
    const QStringList presetLabels = {tr("Extremely fine"), tr("Extra fine"), tr("Finer"), tr("Fine"), tr("Normal"),
                                      tr("Coarse"), tr("Coarser"), tr("Extra coarse"), tr("Extremely coarse")};
    add({QStringLiteral("mesh"), tr("Mesh"), QString(), QStringLiteral("mesh"), Level::None,
         {choice(QStringLiteral("size"), tr("Element size"), presets, presetLabels, QStringLiteral("normal")),
          flag(QStringLiteral("custom"), tr("Customize"), false),
          shownIf(expr(QStringLiteral("hmax"), tr("Maximum element size"), QStringLiteral("length"), QString(),
                       tr("Empty: the preset's")), QStringLiteral("custom=true")),
          shownIf(expr(QStringLiteral("hmin"), tr("Minimum element size"), QStringLiteral("length"), QString()), QStringLiteral("custom=true")),
          shownIf(expr(QStringLiteral("growth"), tr("Maximum element growth rate"), QString(), QString()), QStringLiteral("custom=true")),
          shownIf(expr(QStringLiteral("curvature"), tr("Curvature factor"), QString(), QString(),
                       tr("An element on a curve at most this times its radius")), QStringLiteral("custom=true")),
          shownIf(expr(QStringLiteral("narrow"), tr("Resolution of narrow regions"), QString(), QString(),
                       tr("Elements across a gap between boundaries, at least")), QStringLiteral("custom=true"))},
         {QStringLiteral("mesh/size"), QStringLiteral("mesh/distribution")}, {}, true, true, QString(),
         tr("Triangles, Delaunay, refined till their angles are good and their sizes as asked.")});
    add({QStringLiteral("mesh/size"), tr("Size"), QStringLiteral("mesh"), QStringLiteral("meshsize"), Level::Domain,
         {choice(QStringLiteral("level"), tr("Level"), {QStringLiteral("domain"), QStringLiteral("boundary"), QStringLiteral("point")},
                 {tr("Domains"), tr("Boundaries"), tr("Points")}, QStringLiteral("domain")),
          selectionProp(none), expr(QStringLiteral("hmax"), tr("Maximum element size"), QStringLiteral("length"), QStringLiteral("0.1"))},
         {}, {}, false, false, QStringLiteral("size"), tr("Smaller elements where it says.")});
    add({QStringLiteral("mesh/distribution"), tr("Distribution"), QStringLiteral("mesh"), QStringLiteral("meshdistribution"),
         Level::Boundary, {selectionProp(none), integer(QStringLiteral("count"), tr("Number of elements"), 10)},
         {}, {}, false, false, QStringLiteral("dis"), tr("A number of elements along boundaries.")});

    // Studies.
    add({QStringLiteral("study"), tr("Study"), QStringLiteral("study"), QStringLiteral("study"), Level::None, {},
         {QStringLiteral("stationary"), QStringLiteral("transient"), QStringLiteral("sweep")}, {QStringLiteral("stationary")}, false, false,
         QStringLiteral("std"), tr("What is solved, and how: its steps in order (for each point of its sweep).")});
    {
        P physics;
        physics.key = QStringLiteral("physics");
        physics.label = tr("Physics solved");
        physics.kind = P::Physics;
        physics.defaultValue = QJsonArray();
        physics.tooltip = tr("None chosen: all of them");
        const P solver = choice(QStringLiteral("solver"), tr("Linear solver"), {QStringLiteral("auto"), QStringLiteral("direct"), QStringLiteral("iterative")},
                                {tr("Automatic"), tr("Direct (sparse Cholesky, LU)"), tr("Iterative (conjugate gradients)")}, QStringLiteral("auto"));
        const P nonlinear = choice(QStringLiteral("nonlinear"), tr("Nonlinear method"),
                                   {QStringLiteral("auto"), QStringLiteral("newton"), QStringLiteral("picard")},
                                   {tr("Automatic (Newton within a physics, in turn between them)"), tr("Newton"),
                                    tr("Picard: each physics in turn, again and again")},
                                   QStringLiteral("auto"),
                                   tr("A physics that reads its own field (k(T), radiation): Newton's method converges in a few "
                                      "steps; physics that read each other's are solved in turn till they agree"));
        add({QStringLiteral("stationary"), tr("Stationary"), QStringLiteral("step"), QStringLiteral("stationary"), Level::None,
             {physics, solver, nonlinear,
              flag(QStringLiteral("sweep"), tr("Terminal sweep (capacitance, conductance matrices)"), true),
              expr(QStringLiteral("tolerance"), tr("Relative tolerance"), QString(), QStringLiteral("1e-6"),
                   tr("Of the nonlinear iterations, and of an iterative solver")),
              integer(QStringLiteral("maxiter"), tr("Maximum iterations"), 50,
                      tr("Of the nonlinear iterations: a material that depends on a field"))},
             {}, {}, false, false, QStringLiteral("stat"), tr("The fields at rest: the steady state.")});
        add({QStringLiteral("transient"), tr("Time Dependent"), QStringLiteral("step"), QStringLiteral("transient"), Level::None,
             {physics,
              text(QStringLiteral("times"), tr("Output times"), QStringLiteral("range(0, 0.1, 1)"),
                   tr("In seconds, or with a unit: range(start, step, stop), linspace(start, stop, count), or values split by "
                      "commas - range(0[s], 10[ms], 1[s])")),
              choice(QStringLiteral("method"), tr("Method"), {QStringLiteral("bdf2"), QStringLiteral("euler")},
                     {tr("BDF of order 2"), tr("Backward Euler (order 1)")}, QStringLiteral("bdf2")),
              choice(QStringLiteral("stepping"), tr("Steps"), {QStringLiteral("adaptive"), QStringLiteral("fixed")},
                     {tr("Adaptive: as the tolerance asks"), tr("Fixed")}, QStringLiteral("adaptive")),
              expr(QStringLiteral("rtol"), tr("Relative tolerance"), QString(), QStringLiteral("0.001"),
                   tr("Of each step's error, against how much the fields have changed")),
              expr(QStringLiteral("dt"), tr("Time step"), QStringLiteral("s"), QString(),
                   tr("Fixed: the step; adaptive: the first one (empty: a thousandth of the first output interval)")),
              solver, nonlinear,
              expr(QStringLiteral("tolerance"), tr("Nonlinear tolerance"), QString(), QStringLiteral("1e-6"),
                   tr("Of a step's nonlinear iterations")),
              integer(QStringLiteral("maxiter"), tr("Maximum iterations"), 25, tr("Of a step's nonlinear iterations"))},
             {}, {}, false, false, QStringLiteral("time"),
             tr("The fields in time: heat with its ρ Cp ∂T/∂t, the other physics at rest at each step, from the initial "
                "values; kept at the output times.")});
        P rows;
        rows.key = QStringLiteral("parameters");
        rows.label = tr("Parameters swept");
        rows.kind = P::Rows;
        rows.choiceLabels = {tr("Parameter"), tr("Values"), tr("Unit")};
        rows.defaultValue = QJsonArray();
        rows.tooltip = tr("Each a parameter's values: 1, 2, 5 or range(start, step, stop) or linspace(start, stop, count); "
                          "plain numbers in the unit given");
        add({QStringLiteral("sweep"), tr("Parametric Sweep"), QStringLiteral("step"), QStringLiteral("sweep"), Level::None,
             {rows,
              choice(QStringLiteral("combination"), tr("Sweep type"), {QStringLiteral("all"), QStringLiteral("specified")},
                     {tr("All combinations"), tr("Specified combinations (the n-th values together)")}, QStringLiteral("all"))},
             {}, {}, false, true, QStringLiteral("param"),
             tr("The study's steps solved again for each value of its parameters - the geometry and the mesh made again "
                "where they change. Results show one point of it, or all of them along a 1D plot.")});
    }

    // Results.
    add({QStringLiteral("results"), tr("Results"), QString(), QStringLiteral("results"), Level::None, {},
         {QStringLiteral("plotgroup"), QStringLiteral("plotgroup1d"), QStringLiteral("cutline"), QStringLiteral("cutpoint")},
         {QStringLiteral("derived")}, true, true, QString(), QString()});
    {
        P study;
        study.key = QStringLiteral("study");
        study.label = tr("Study");
        study.kind = P::Study;
        study.defaultValue = QString();
        study.tooltip = tr("Empty: the first");
        P point;
        point.key = QStringLiteral("point");
        point.label = tr("Parameter value");
        point.kind = P::SweepPoint;
        point.defaultValue = 0;
        point.tooltip = tr("Of a study with a sweep: which of its solutions (0: the last)");
        P time;
        time.key = QStringLiteral("time");
        time.label = tr("Time");
        time.kind = P::Instant;
        time.defaultValue = QString();
        time.tooltip = tr("Of a study in time: the output time shown (empty: the last)");
        add({QStringLiteral("plotgroup"), tr("2D Plot Group"), QStringLiteral("results"), QStringLiteral("plotgroup"), Level::None,
             {study, point, time, text(QStringLiteral("title"), tr("Title"), QString())},
             {QStringLiteral("surface"), QStringLiteral("contour"), QStringLiteral("arrow"), QStringLiteral("meshplot"), QStringLiteral("deformation")},
             {}, false, false, QStringLiteral("pg"), tr("Plots shown together in the Graphics view.")});
        add({QStringLiteral("deformation"), tr("Deformation"), QStringLiteral("plot"), QStringLiteral("deformation"), Level::None,
             {spatial(QStringLiteral("x"), tr("x component"), QStringLiteral("m"), QStringLiteral("solid.u")),
              spatial(QStringLiteral("y"), tr("y component"), QStringLiteral("m"), QStringLiteral("solid.v")),
              choice(QStringLiteral("scaling"), tr("Scale"), {QStringLiteral("auto"), QStringLiteral("manual")},
                     {tr("Automatic: the largest a tenth of the model"), tr("Manual")}, QStringLiteral("auto")),
              shownIf(expr(QStringLiteral("scale"), tr("Scale factor"), QString(), QStringLiteral("1")), QStringLiteral("scaling=manual"))},
             {}, {}, false, true, QStringLiteral("def"),
             tr("The plot group's plots drawn where the solid has moved, its displacement scaled to be seen.")});
        P dataset;
        dataset.key = QStringLiteral("data");
        dataset.label = tr("Dataset");
        dataset.kind = P::Dataset;
        dataset.defaultValue = QString();
        dataset.tooltip = tr("A cut line (a line graph) or cut point (a point graph) of the results");
        const P solutions = choice(QStringLiteral("solutions"), tr("Solutions"), {QStringLiteral("all"), QStringLiteral("selected")},
                                   {tr("All: each output time, each parameter value"), tr("The plot group's time and parameter value")},
                                   QStringLiteral("all"));
        add({QStringLiteral("cutline"), tr("Cut Line 2D"), QStringLiteral("dataset"), QStringLiteral("cutline"), Level::None,
             {pair(QStringLiteral("start"), tr("Start"), QStringLiteral("length"), QStringLiteral("0"), QStringLiteral("0")),
              pair(QStringLiteral("end"), tr("End"), QStringLiteral("length"), QStringLiteral("1"), QStringLiteral("0")),
              integer(QStringLiteral("points"), tr("Points"), 200, tr("Where it is evaluated, evenly along it"))},
             {}, {}, false, false, QStringLiteral("cln"), tr("A line across the model: what a Line Graph shows its values along.")});
        P coordinates0;
        coordinates0.key = QStringLiteral("coordinates");
        coordinates0.label = tr("Coordinates (x, y)");
        coordinates0.kind = P::Points;
        coordinates0.unit = QStringLiteral("length");
        QJsonArray origin0;
        origin0.append(QJsonArray{QStringLiteral("0"), QStringLiteral("0")});
        coordinates0.defaultValue = origin0;
        add({QStringLiteral("cutpoint"), tr("Cut Point 2D"), QStringLiteral("dataset"), QStringLiteral("cutpoint"), Level::None, {coordinates0},
             {}, {}, false, false, QStringLiteral("cpt"), tr("Points of the model: what a Point Graph shows values at, in time or along a sweep.")});
        add({QStringLiteral("plotgroup1d"), tr("1D Plot Group"), QStringLiteral("results"), QStringLiteral("plotgroup1d"), Level::None,
             {study, point, time, text(QStringLiteral("title"), tr("Title"), QString()),
              choice(QStringLiteral("diagram"), tr("Diagram"), {QStringLiteral("rect"), QStringLiteral("tab")},
                     {tr("Cartesian"), tr("Table")}, QStringLiteral("rect"))},
             {QStringLiteral("linegraph"), QStringLiteral("globalgraph"), QStringLiteral("pointgraph")}, {}, false, false,
             QStringLiteral("pg1d"),
             tr("Graphs of the solution - along a cut line, at points or of global values over time or a sweep - written as a "
                "Qucs dataset (model_tag.dat beside the model) and shown in a data display, with its markers and exports.")});
        const P unit1 = text(QStringLiteral("unit"), tr("Unit"), QString(), tr("Shown in this unit: mV, degC, kV/mm... (empty: SI)"));
        add({QStringLiteral("linegraph"), tr("Line Graph"), QStringLiteral("graph"), QStringLiteral("linegraph"), Level::None,
             {dataset, spatial(QStringLiteral("expression"), tr("Expression"), QString(), QStringLiteral("V")), unit1,
              choice(QStringLiteral("xaxis"), tr("x-axis"), {QStringLiteral("arc"), QStringLiteral("x"), QStringLiteral("y")},
                     {tr("The distance along the line"), tr("x"), tr("y")}, QStringLiteral("arc")),
              solutions},
             {}, {}, false, false, QStringLiteral("lngr"), tr("An expression along a cut line: a curve for each solution.")});
        add({QStringLiteral("globalgraph"), tr("Global"), QStringLiteral("graph"), QStringLiteral("globalgraph"), Level::None,
             {text(QStringLiteral("expression"), tr("Expressions"), QStringLiteral("ht.Tmax"),
                   tr("Split by ';': ht.Tmax; ec.P... - or a Derived Values node's tag: lp1 (each of its values), lp1.Z0 (one)")),
              unit1},
             {}, {}, false, false, QStringLiteral("glbg"),
             tr("Global values - or a Derived Values node's - over the output times, or the sweep's parameter.")});
        add({QStringLiteral("pointgraph"), tr("Point Graph"), QStringLiteral("graph"), QStringLiteral("pointgraph"), Level::None,
             {dataset, spatial(QStringLiteral("expression"), tr("Expression"), QString(), QStringLiteral("T")), unit1},
             {}, {}, false, false, QStringLiteral("ptgr"), tr("An expression at a cut point's points over the output times, or the sweep's parameter.")});
        const QStringList tables = {QStringLiteral("rainbow"), QStringLiteral("viridis"), QStringLiteral("thermal"),
                                    QStringLiteral("coolwarm"), QStringLiteral("gray")};
        const QStringList tableLabels = {tr("Rainbow"), tr("Viridis"), tr("Thermal"), tr("Cool to warm"), tr("Gray")};
        const P unit = text(QStringLiteral("unit"), tr("Unit"), QString(), tr("Shown in this unit: mV, degC, kV/mm... (empty: SI)"));
        add({QStringLiteral("surface"), tr("Surface"), QStringLiteral("plot"), QStringLiteral("surface"), Level::None,
             {spatial(QStringLiteral("expression"), tr("Expression"), QString(), QStringLiteral("V")), unit,
              choice(QStringLiteral("colors"), tr("Color table"), tables, tableLabels, QStringLiteral("rainbow")),
              choice(QStringLiteral("range"), tr("Range"), {QStringLiteral("auto"), QStringLiteral("manual")}, {tr("Automatic"), tr("Manual")},
                     QStringLiteral("auto")),
              shownIf(expr(QStringLiteral("min"), tr("Minimum"), QString(), QStringLiteral("0")), QStringLiteral("range=manual")),
              shownIf(expr(QStringLiteral("max"), tr("Maximum"), QString(), QStringLiteral("1")), QStringLiteral("range=manual"))},
             {}, {}, false, false, QStringLiteral("surf"), tr("An expression in colour over the domains.")});
        add({QStringLiteral("contour"), tr("Contour"), QStringLiteral("plot"), QStringLiteral("contour"), Level::None,
             {spatial(QStringLiteral("expression"), tr("Expression"), QString(), QStringLiteral("V")), unit,
              integer(QStringLiteral("levels"), tr("Levels"), 20),
              choice(QStringLiteral("colors"), tr("Color"), QStringList{QStringLiteral("single")} + tables,
                     QStringList{tr("One color")} + tableLabels, QStringLiteral("single")),
              shownIf(text(QStringLiteral("color"), tr("Line color"), QStringLiteral("black")), QStringLiteral("colors=single"))},
             {}, {}, false, false, QStringLiteral("con"), tr("Lines where an expression has each of a number of values.")});
        add({QStringLiteral("arrow"), tr("Arrow Surface"), QStringLiteral("plot"), QStringLiteral("arrow"), Level::None,
             {spatial(QStringLiteral("x"), tr("x component"), QString(), QStringLiteral("es.Ex")),
              spatial(QStringLiteral("y"), tr("y component"), QString(), QStringLiteral("es.Ey")),
              pair(QStringLiteral("points"), tr("Arrows (x, y)"), QString(), QStringLiteral("20"), QStringLiteral("20")),
              choice(QStringLiteral("scaling"), tr("Length"), {QStringLiteral("proportional"), QStringLiteral("normalized")},
                     {tr("Proportional"), tr("Normalized")}, QStringLiteral("proportional")),
              text(QStringLiteral("color"), tr("Color"), QStringLiteral("black"))},
             {}, {}, false, false, QStringLiteral("arw"), tr("A vector field as arrows on a grid.")});
        add({QStringLiteral("meshplot"), tr("Mesh"), QStringLiteral("plot"), QStringLiteral("meshplot"), Level::None,
             {flag(QStringLiteral("quality"), tr("Colored by quality"), false)}, {}, {}, false, false, QStringLiteral("mp"),
             tr("The mesh's triangles.")});
        add({QStringLiteral("derived"), tr("Derived Values"), QString(), QStringLiteral("derived"), Level::None, {},
             {QStringLiteral("global"), QStringLiteral("integral"), QStringLiteral("average"), QStringLiteral("maximum"),
              QStringLiteral("minimum"), QStringLiteral("pointeval"), QStringLiteral("lineparams")},
             {}, true, true, QString(), tr("Numbers of the solution: integrals, averages, a capacitance, a resistance.")});
        add({QStringLiteral("global"), tr("Global Evaluation"), QStringLiteral("derived"), QStringLiteral("global"), Level::None,
             {study, text(QStringLiteral("expression"), tr("Expressions"), QStringLiteral("es.C11"),
                          tr("Split by ';': es.C11; ec.R; ht.Rth...")),
              unit, solutions},
             {}, {}, false, false, QStringLiteral("gev"), tr("Values of the whole model: a capacitance matrix, a resistance.")});
        const P level = choice(QStringLiteral("level"), tr("Over"), {QStringLiteral("domain"), QStringLiteral("boundary")},
                               {tr("Domains"), tr("Boundaries")}, QStringLiteral("domain"));
        const QStringList over = {QStringLiteral("integral"), QStringLiteral("average"), QStringLiteral("maximum"), QStringLiteral("minimum")};
        const QStringList overTitles = {tr("Integration"), tr("Average"), tr("Maximum"), tr("Minimum")};
        const QStringList overPrefix = {QStringLiteral("int"), QStringLiteral("av"), QStringLiteral("max"), QStringLiteral("min")};
        for (int i = 0; i < over.size(); ++i)
            add({over[i], overTitles[i], QStringLiteral("derived"), over[i], Level::Domain,
                 {study, level, selectionProp(), spatial(QStringLiteral("expression"), tr("Expression"), QString(), QStringLiteral("1")), unit,
                  flag(QStringLiteral("revolved"), tr("Round the axis (2πr), about one"), true,
                       tr("An axisymmetric model's integral: of the volume (surface) it sweeps round the axis, not of the plane's area (length)")),
                  solutions},
                 {}, {}, false, false, overPrefix[i], QString()});
        P coordinates;
        coordinates.key = QStringLiteral("coordinates");
        coordinates.label = tr("Coordinates (x, y)");
        coordinates.kind = P::Points;
        coordinates.unit = QStringLiteral("length");
        QJsonArray origin;   // (appended: a braced list of one list is a copy to some compilers)
        origin.append(QJsonArray{QStringLiteral("0"), QStringLiteral("0")});
        coordinates.defaultValue = origin;
        coordinates.showIf = QStringLiteral("where=coordinates");
        add({QStringLiteral("pointeval"), tr("Point Evaluation"), QStringLiteral("derived"), QStringLiteral("pointeval"), Level::Point,
             {study,
              choice(QStringLiteral("where"), tr("At"), {QStringLiteral("points"), QStringLiteral("coordinates")},
                     {tr("Points of the geometry"), tr("Coordinates")}, QStringLiteral("coordinates")),
              shownIf(selectionProp(none), QStringLiteral("where=points")), coordinates,
              spatial(QStringLiteral("expression"), tr("Expression"), QString(), QStringLiteral("V")), unit, solutions},
             {}, {}, false, false, QStringLiteral("pev"), QString()});
        P terminal;
        terminal.key = QStringLiteral("terminal");
        terminal.label = tr("Signal terminal");
        terminal.kind = P::Terminal;
        terminal.defaultValue = QStringLiteral("1");
        P physics;
        physics.key = QStringLiteral("physics");
        physics.label = tr("Physics");
        physics.kind = P::Physics;
        physics.defaultValue = QJsonArray{QStringLiteral("es")};
        add({QStringLiteral("lineparams"), tr("Line Parameters (quasi-TEM)"), QStringLiteral("derived"), QStringLiteral("lineparams"),
             Level::None, {study, physics, terminal}, {}, {}, false, false, QStringLiteral("lp"),
             tr("A transmission line's cross-section: its capacitance C and, with every εr 1, C0 - so εeff = C/C0, "
                "Z0 = 1/(c0 √(C C0)) and L = 1/(c0² C0) per metre. The signal terminal at 1 V, the others and the "
                "grounds at 0 V.")});
    }
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

QStringList physicsTypes()
{
    return {QStringLiteral("electrostatics"), QStringLiteral("currents"), QStringLiteral("heat"), QStringLiteral("solid")};
}

// ------------------------------------------------------------------ Node

QString Node::name() const
{
    if (!label.isEmpty()) return label;
    const NodeKind* k = kind();
    return k ? k->title : type;
}

QJsonValue Node::value(const QString& key) const
{
    auto it = props.constFind(key);
    if (it != props.constEnd() && !it->isUndefined() && !it->isNull()) return *it;
    if (const NodeKind* k = kind())
        if (const PropertyDef* p = k->property(key)) return p->defaultValue;
    return {};
}

QString Node::text(const QString& key) const
{
    const QJsonValue v = value(key);
    if (v.isDouble()) return formatNumber(v.toDouble(), 15);
    if (v.isBool()) return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    return v.toString();
}

bool Node::flag(const QString& key) const
{
    const QJsonValue v = value(key);
    if (v.isString()) return v.toString() == QLatin1String("true") || v.toString() == QLatin1String("1");
    return v.toBool();
}

int Node::integer(const QString& key) const
{
    const QJsonValue v = value(key);
    if (v.isString()) return v.toString().toInt();
    return v.toInt();
}

QStringList Node::pair(const QString& key) const
{
    const QJsonArray a = value(key).toArray();
    QStringList out;
    for (const QJsonValue& v : a) out << (v.isDouble() ? formatNumber(v.toDouble(), 15) : v.toString());
    while (out.size() < 2) out << QStringLiteral("0");
    return out;
}

QStringList Node::list(const QString& key) const
{
    const QJsonValue v = value(key);
    QStringList out;
    if (v.isString()) {
        for (const QString& s : v.toString().split(QLatin1Char(','), Qt::SkipEmptyParts)) out << s.trimmed();
        return out;
    }
    for (const QJsonValue& e : v.toArray()) out << e.toString();
    return out;
}

Node* Node::find(const QString& t)
{
    if (tag == t) return this;
    for (Node& c : children)
        if (Node* f = c.find(t)) return f;
    return nullptr;
}

const Node* Node::find(const QString& t) const
{
    return const_cast<Node*>(this)->find(t);
}

Node* Node::child(const QString& t)
{
    for (Node& c : children)
        if (c.type == t) return &c;
    return nullptr;
}

const Node* Node::child(const QString& t) const
{
    return const_cast<Node*>(this)->child(t);
}

// ------------------------------------------------------------------ Model

Model::Model()
{
    root.type = QStringLiteral("model");
    root.tag = QStringLiteral("model");
    normalise();
}

Model Model::blank()
{
    Model m;
    Node study = m.make(QStringLiteral("study"));
    m.add(QStringLiteral("model"), study);
    return m;
}

Node& Model::definitions()
{
    return *root.child(QStringLiteral("definitions"));
}

Node& Model::component()
{
    return *root.child(QStringLiteral("component"));
}

const Node& Model::component() const
{
    return *root.child(QStringLiteral("component"));
}

Node& Model::results()
{
    return *root.child(QStringLiteral("results"));
}

const Node& Model::results() const
{
    return *root.child(QStringLiteral("results"));
}

QList<Model::Parameter> Model::parameters() const
{
    QList<Parameter> out;
    const Node* defs = root.child(QStringLiteral("definitions"));
    const Node* params = defs ? defs->child(QStringLiteral("parameters")) : nullptr;
    if (!params) return out;
    for (const QJsonValue& v : params->value(QStringLiteral("rows")).toArray()) {
        const QJsonObject o = v.toObject();
        out.append({o.value(QStringLiteral("name")).toString(), o.value(QStringLiteral("expression")).toString(),
                    o.value(QStringLiteral("description")).toString()});
    }
    return out;
}

void Model::setParameters(const QList<Parameter>& parameters)
{
    QJsonArray rows;
    for (const Parameter& p : parameters) {
        QJsonObject o{{QStringLiteral("name"), p.name}, {QStringLiteral("expression"), p.expression}};
        if (!p.description.isEmpty()) o.insert(QStringLiteral("description"), p.description);
        rows.append(o);
    }
    definitions().child(QStringLiteral("parameters"))->props.insert(QStringLiteral("rows"), rows);
}

std::vector<const Node*> Model::physics() const
{
    std::vector<const Node*> out;
    for (const Node& c : component().children)
        if (physicsTypes().contains(c.type)) out.push_back(&c);
    return out;
}

std::vector<const Node*> Model::studies() const
{
    std::vector<const Node*> out;
    for (const Node& c : root.children)
        if (c.type == QLatin1String("study")) out.push_back(&c);
    return out;
}

Node* Model::find(const QString& tag)
{
    return root.find(tag);
}

const Node* Model::find(const QString& tag) const
{
    return root.find(tag);
}

namespace {

Node* parentIn(Node& node, const QString& tag, int* index)
{
    for (int i = 0; i < int(node.children.size()); ++i) {
        if (node.children[std::size_t(i)].tag == tag) {
            if (index) *index = i;
            return &node;
        }
        if (Node* p = parentIn(node.children[std::size_t(i)], tag, index)) return p;
    }
    return nullptr;
}

void collectTags(const Node& node, QSet<QString>& tags)
{
    tags.insert(node.tag);
    for (const Node& c : node.children) collectTags(c, tags);
}

} // namespace

Node* Model::parentOf(const QString& tag, int* index)
{
    return parentIn(root, tag, index);
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
    if (!k) return n;
    if (k->group == QLatin1String("physics")) {
        QSet<QString> tags;
        collectTags(root, tags);
        n.tag = k->prefix;
        for (int i = 2; tags.contains(n.tag); ++i) n.tag = k->prefix + QString::number(i);
    } else {
        n.tag = newTag(k->prefix);
    }
    if (k->group == QLatin1String("geometry")) n.label = n.tag;   // the object's name
    if (type == QLatin1String("study") || type == QLatin1String("plotgroup") || type == QLatin1String("plotgroup1d")) {
        // Study 1, 2D Plot Group 2: numbered as COMSOL's.
        int count = 1;
        for (const Node& c : root.children) count += c.type == type ? 1 : 0;
        if (type != QLatin1String("study"))
            for (const Node& c : root.child(QStringLiteral("results"))->children) count += c.type == type ? 1 : 0;
        n.label = k->title + QLatin1Char(' ') + QString::number(count);
    }
    for (const QString& d : k->defaults) {
        Node c;
        c.type = d;
        const NodeKind* ck = nodeKind(d);
        c.tag = (ck ? ck->prefix : QStringLiteral("n")) + QLatin1Char('_') + n.tag;
        c.isDefault = k->group == QLatin1String("physics");
        n.children.push_back(c);
    }
    return n;
}

Node* Model::add(const QString& parentTag, Node node, int index, QString* error)
{
    Node* parent = find(parentTag);
    if (!parent) {
        if (error) *error = tr("there is no node %1").arg(parentTag);
        return nullptr;
    }
    const NodeKind* pk = parent->kind();
    const NodeKind* k = node.kind();
    if (!k) {
        if (error) *error = tr("%1 is not a kind of node").arg(node.type);
        return nullptr;
    }
    if (!pk || !pk->children.contains(node.type)) {
        if (error) *error = tr("a %1 may not go under %2").arg(k->title, parent->name());
        return nullptr;
    }
    if (k->unique && parent->child(node.type)) {
        if (error) *error = tr("%1 has a %2 already").arg(parent->name(), k->title);
        return nullptr;
    }
    // Unique tags, the node's and its children's.
    QSet<QString> tags;
    collectTags(root, tags);
    std::function<void(Node&)> retag = [&](Node& n) {
        const NodeKind* nk = n.kind();
        if (n.tag.isEmpty() || tags.contains(n.tag)) {
            const QString prefix = nk && !nk->prefix.isEmpty() ? nk->prefix : QStringLiteral("n");
            QString t = n.tag.isEmpty() ? prefix + QLatin1Char('1') : n.tag;
            for (int i = 1; tags.contains(t); ++i) t = prefix + QString::number(i);
            n.tag = t;
        }
        tags.insert(n.tag);
        for (Node& c : n.children) retag(c);
    };
    retag(node);
    // A geometry object's name, unique among the objects.
    if (k->group == QLatin1String("geometry") && node.type != QLatin1String("form union")) {
        const QStringList names = objectNames();
        if (node.label.isEmpty() || names.contains(node.label)) {
            QString name = node.label.isEmpty() ? node.tag : node.label;
            for (int i = 2; names.contains(name); ++i) name = (node.label.isEmpty() ? node.tag : node.label) + QString::number(i);
            node.label = name;
        }
    }
    // A terminal's name: the next number among its physics' terminals.
    if (k->icon == QLatin1String("terminal") && node.type.endsWith(QLatin1String("/terminal"))
        && !node.props.contains(QStringLiteral("name"))) {
        QStringList used;
        for (const Node& c : parent->children)
            if (c.type == node.type) used << c.text(QStringLiteral("name"));
        int i = 1;
        while (used.contains(QString::number(i))) ++i;
        node.props.insert(QStringLiteral("name"), QString::number(i));
    }
    std::vector<Node>& list = parent->children;
    int at = index < 0 || index > int(list.size()) ? int(list.size()) : index;
    // The tree's order kept: Form Union last of the geometry, the mesh after
    // the physics, the studies before the results, Derived Values last of them.
    const QString last = parent->type == QLatin1String("geometry")    ? QStringLiteral("form union")
                         : parent->type == QLatin1String("component") ? QStringLiteral("mesh")
                         : parent->type == QLatin1String("model")     ? QStringLiteral("results")
                         : parent->type == QLatin1String("results")   ? QStringLiteral("derived")
                                                                      : QString();
    if (!last.isEmpty())
        for (int i = 0; i < int(list.size()); ++i)
            if (list[std::size_t(i)].type == last && at > i) at = i;
    // A study's sweep before its steps.
    if (node.type == QLatin1String("sweep")) at = 0;
    list.insert(list.begin() + at, std::move(node));
    return &list[std::size_t(at)];
}

bool Model::remove(const QString& tag, QString* error)
{
    int index = -1;
    Node* parent = parentOf(tag, &index);
    if (!parent) {
        if (error) *error = tr("there is no node %1").arg(tag);
        return false;
    }
    const Node& node = parent->children[std::size_t(index)];
    const NodeKind* k = node.kind();
    if ((k && k->fixed) || node.isDefault) {
        if (error) *error = tr("%1 is a part of the model: it may be disabled, not deleted").arg(node.name());
        return false;
    }
    parent->children.erase(parent->children.begin() + index);
    return true;
}

namespace {

void renameIn(QJsonValue& value, const QString& from, const QString& to)
{
    if (value.isObject()) {
        QJsonObject o = value.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) {
            QJsonValue v = it.value();
            renameIn(v, from, to);
            it.value() = v;
        }
        value = o;
    } else if (value.isArray()) {
        QJsonArray a = value.toArray();
        for (int i = 0; i < a.size(); ++i) {
            QJsonValue v = a.at(i);
            if (v.isString() && v.toString() == from) v = to;
            else renameIn(v, from, to);
            a[i] = v;
        }
        value = a;
    }
}

void renameNode(Node& node, const QString& from, const QString& to)
{
    for (const QString& key : {QStringLiteral("selection"), QStringLiteral("input"), QStringLiteral("tools")}) {
        if (!node.props.contains(key)) continue;
        QJsonValue v = node.props.value(key);
        renameIn(v, from, to);
        node.props.insert(key, v);
    }
    for (Node& c : node.children) renameNode(c, from, to);
}

void namesIn(const Node& geometry, const QString& before, QStringList& out)
{
    for (const Node& f : geometry.children) {
        if (!before.isEmpty() && f.tag == before) return;
        if (f.type == QLatin1String("form union") || f.label.isEmpty()) continue;
        if (!out.contains(f.label)) out << f.label;
    }
}

} // namespace

void Model::renameObject(const QString& from, const QString& to)
{
    if (from == to || from.isEmpty()) return;
    renameNode(root, from, to);
}

QStringList Model::objectNames(const QString& beforeTag) const
{
    QStringList out;
    namesIn(geometry(), beforeTag, out);
    return out;
}

// ------------------------------------------------------------------ The file

namespace {

/// A child's type as the file writes it under its parent: "terminal"
/// under an electrostatics, not "electrostatics/terminal".
QString shortType(const Node& parent, const QString& type)
{
    const QString prefix = parent.type + QLatin1Char('/');
    if (type.startsWith(prefix)) return type.mid(prefix.size());
    if (parent.type == QLatin1String("mesh") && type.startsWith(QLatin1String("mesh/"))) return type.mid(5);
    return type;
}

QString longType(const QString& parentType, const QString& type)
{
    if (nodeKind(parentType + QLatin1Char('/') + type)) return parentType + QLatin1Char('/') + type;
    return type;
}

QJsonObject nodeObject(const Node& parent, const Node& node, const QString& childKey = QStringLiteral("features"))
{
    QJsonObject o;
    o.insert(QStringLiteral("type"), shortType(parent, node.type));
    o.insert(QStringLiteral("tag"), node.tag);
    if (!node.label.isEmpty()) o.insert(QStringLiteral("label"), node.label);
    if (!node.enabled) o.insert(QStringLiteral("enabled"), false);
    if (node.isDefault) o.insert(QStringLiteral("default"), true);
    for (auto it = node.props.begin(); it != node.props.end(); ++it) o.insert(it.key(), it.value());
    if (!node.children.empty()) {
        QJsonArray children;
        for (const Node& c : node.children) children.append(nodeObject(node, c));
        o.insert(childKey, children);
    }
    return o;
}

const QStringList Reserved = {QStringLiteral("type"), QStringLiteral("tag"), QStringLiteral("label"),
                              QStringLiteral("enabled"), QStringLiteral("default"), QStringLiteral("features"),
                              QStringLiteral("steps"), QStringLiteral("plots")};

std::optional<Node> nodeFrom(const QString& parentType, const QJsonValue& value, QStringList& problems,
                             const QString& childKey = QStringLiteral("features"), const QString& defaultType = {})
{
    if (!value.isObject()) {
        problems << tr("a node that is not an object");
        return std::nullopt;
    }
    const QJsonObject o = value.toObject();
    Node n;
    n.type = longType(parentType, o.value(QStringLiteral("type")).toString(defaultType));
    // "name" is a geometry object's label too, as the proposal writes it.
    n.tag = o.value(QStringLiteral("tag")).toString();
    n.label = o.value(QStringLiteral("label")).toString();
    n.enabled = o.value(QStringLiteral("enabled")).toBool(true);
    n.isDefault = o.value(QStringLiteral("default")).toBool(false);
    const NodeKind* k = nodeKind(n.type);
    if (!k) problems << tr("an unknown kind of node, %1 (kept as it is)").arg(n.type);
    if (k && k->group == QLatin1String("geometry") && n.label.isEmpty() && o.contains(QStringLiteral("name")))
        n.label = o.value(QStringLiteral("name")).toString();
    for (auto it = o.begin(); it != o.end(); ++it) {
        if (Reserved.contains(it.key())) continue;
        if (k && k->group == QLatin1String("geometry") && it.key() == QLatin1String("name")) continue;
        n.props.insert(it.key(), it.value());
    }
    for (const QJsonValue& c : o.value(childKey).toArray())
        if (std::optional<Node> child = nodeFrom(n.type, c, problems)) n.children.push_back(*child);
    return n;
}

/// A selection rule as the proposal writes some: {"bottom of": [...]} is
/// {"of": [...], "side": "bottom"}.
void normaliseRule(QJsonObject& rule)
{
    for (const char* side : {"bottom", "top", "left", "right"}) {
        const QString key = QString::fromLatin1(side) + QStringLiteral(" of");
        if (rule.contains(key)) {
            rule.insert(QStringLiteral("of"), rule.value(key));
            rule.insert(QStringLiteral("side"), QString::fromLatin1(side));
            rule.remove(key);
        }
    }
}

void normaliseSelections(Node& node)
{
    // "domains"/"boundaries"/"points" as the proposal writes a selection.
    for (const char* key : {"domains", "boundaries", "points"}) {
        const QString k = QString::fromLatin1(key);
        if (node.props.contains(k) && node.props.value(k).isObject() && !node.props.contains(QStringLiteral("selection"))) {
            node.props.insert(QStringLiteral("selection"), node.props.value(k));
            node.props.remove(k);
        }
    }
    if (node.props.contains(QStringLiteral("selection"))) {
        QJsonValue v = node.props.value(QStringLiteral("selection"));
        if (v.isArray()) v = QJsonObject{{QStringLiteral("numbers"), v}};   // [1, 2]: those numbers
        else if (v.isString() && v.toString() == QLatin1String("all")) v = QJsonObject{{QStringLiteral("all"), true}};
        QJsonObject rule = v.toObject();
        normaliseRule(rule);
        node.props.insert(QStringLiteral("selection"), rule);
    }
    for (Node& c : node.children) normaliseSelections(c);
}

} // namespace

void Model::normalise()
{
    auto ensure = [](Node& parent, const QString& type, int at = -1) -> Node& {
        if (Node* n = parent.child(type)) return *n;
        Node n;
        n.type = type;
        n.tag = type == QLatin1String("component") ? QStringLiteral("comp1") : type;
        auto pos = at < 0 || at > int(parent.children.size()) ? parent.children.end() : parent.children.begin() + at;
        parent.children.insert(pos, n);
        return *parent.child(type);
    };
    Node& defs = ensure(root, QStringLiteral("definitions"), 0);
    ensure(defs, QStringLiteral("parameters"), 0);
    Node& comp = ensure(root, QStringLiteral("component"), 1);
    ensure(comp, QStringLiteral("selections"), 0);
    ensure(comp, QStringLiteral("geometry"), 1);
    ensure(comp, QStringLiteral("materials"), 2);
    // The mesh after the physics.
    if (!comp.child(QStringLiteral("mesh"))) ensure(comp, QStringLiteral("mesh"));
    else {
        auto it = std::find_if(comp.children.begin(), comp.children.end(), [](const Node& n) { return n.type == QLatin1String("mesh"); });
        Node mesh = *it;
        comp.children.erase(it);
        comp.children.push_back(mesh);
    }
    // Form Union last of the geometry.
    {
        Node& geometry = *comp.child(QStringLiteral("geometry"));
        auto it = std::find_if(geometry.children.begin(), geometry.children.end(),
                               [](const Node& n) { return n.type == QLatin1String("form union"); });
        Node fin;
        if (it != geometry.children.end()) {
            fin = *it;
            geometry.children.erase(it);
        } else {
            fin.type = QStringLiteral("form union");
            fin.tag = QStringLiteral("fin");
        }
        geometry.children.push_back(fin);
    }
    // Results after the studies, Derived Values in them.
    {
        auto it = std::find_if(root.children.begin(), root.children.end(), [](const Node& n) { return n.type == QLatin1String("results"); });
        Node results;
        if (it != root.children.end()) {
            results = *it;
            root.children.erase(it);
        } else {
            results.type = QStringLiteral("results");
            results.tag = QStringLiteral("results");
        }
        root.children.push_back(results);
        Node& r = root.children.back();
        auto d = std::find_if(r.children.begin(), r.children.end(), [](const Node& n) { return n.type == QLatin1String("derived"); });
        Node derived;
        if (d != r.children.end()) {
            derived = *d;
            r.children.erase(d);
        } else {
            derived.type = QStringLiteral("derived");
            derived.tag = QStringLiteral("derived");
        }
        r.children.push_back(derived);
    }
    // Every node a tag, each its own.
    QSet<QString> tags;
    std::function<void(Node&)> retag = [&](Node& n) {
        if (n.tag.isEmpty() || tags.contains(n.tag)) {
            const NodeKind* k = n.kind();
            const QString prefix = k && !k->prefix.isEmpty() ? k->prefix : QStringLiteral("n");
            QString t = prefix + QLatin1Char('1');
            for (int i = 1; tags.contains(t); ++i) t = prefix + QString::number(i);
            n.tag = t;
        }
        tags.insert(n.tag);
        for (Node& c : n.children) retag(c);
    };
    retag(root);
    normaliseSelections(root);
    // A physics has its default features (Charge Conservation, Zero
    // Charge...), on all, first: a file written by hand may leave them out.
    for (Node& p : root.child(QStringLiteral("component"))->children) {
        const NodeKind* k = p.kind();
        if (!k || k->group != QLatin1String("physics")) continue;
        int at = 0;
        for (const QString& d : k->defaults) {
            auto it = std::find_if(p.children.begin(), p.children.end(), [&](const Node& c) { return c.type == d && c.isDefault; });
            if (it != p.children.end()) {
                // (The next one made after it: the defaults in their order.)
                at = std::max(at, int(it - p.children.begin()) + 1);
                continue;
            }
            Node c;
            c.type = d;
            const NodeKind* ck = nodeKind(d);
            QString t = (ck ? ck->prefix : QStringLiteral("n")) + QLatin1Char('_') + p.tag;
            for (int i = 2; tags.contains(t); ++i) t = (ck ? ck->prefix : QStringLiteral("n")) + QLatin1Char('_') + p.tag + QString::number(i);
            tags.insert(t);
            c.tag = t;
            c.isDefault = true;
            p.children.insert(p.children.begin() + at++, c);
        }
    }
}

std::optional<Model> Model::fromJson(const QByteArray& json, QString* error)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (doc.isNull() || !doc.isObject()) {
        if (error) *error = tr("not a model (JSON: %1)").arg(parseError.errorString());
        return std::nullopt;
    }
    const QJsonObject o = doc.object();
    const QString format = o.value(QStringLiteral("format")).toString();
    if (!format.startsWith(QLatin1String("qucs-s multiphysics"))) {
        if (error) *error = tr("not a Qucs-S multiphysics model: its \"format\" is not \"%1\"").arg(FormatName);
        return std::nullopt;
    }
    Model m;
    m.root.children.clear();
    m.root.label = o.value(QStringLiteral("title")).toString();
    if (o.contains(QStringLiteral("description"))) m.root.props.insert(QStringLiteral("description"), o.value(QStringLiteral("description")));
    QStringList problems;

    Node defs;
    defs.type = QStringLiteral("definitions");
    defs.tag = QStringLiteral("definitions");
    Node params;
    params.type = QStringLiteral("parameters");
    params.tag = QStringLiteral("parameters");
    params.props.insert(QStringLiteral("rows"), o.value(QStringLiteral("parameters")).toArray());
    defs.children.push_back(params);
    for (const QJsonValue& f : o.value(QStringLiteral("functions")).toArray())
        if (std::optional<Node> n = nodeFrom(QStringLiteral("definitions"), f, problems)) defs.children.push_back(*n);
    m.root.children.push_back(defs);

    const QJsonArray components = o.value(QStringLiteral("components")).toArray();
    if (components.size() > 1) problems << tr("only the first of %1 components is read").arg(components.size());
    const QJsonObject c = components.isEmpty() ? QJsonObject() : components.first().toObject();
    Node comp;
    comp.type = QStringLiteral("component");
    comp.tag = c.value(QStringLiteral("tag")).toString(QStringLiteral("comp1"));
    comp.label = c.value(QStringLiteral("label")).toString();
    for (const char* key : {"unit", "thickness"})
        if (c.contains(QString::fromLatin1(key))) comp.props.insert(QString::fromLatin1(key), c.value(QString::fromLatin1(key)));
    if (c.contains(QStringLiteral("space"))) {
        // "2D", or about an axis: "axisymmetric" ("2D axisymmetric", "2Daxi").
        const QString space = c.value(QStringLiteral("space")).toString().trimmed().toLower();
        if (space.contains(QLatin1String("axi"))) comp.props.insert(QStringLiteral("space"), QStringLiteral("axisymmetric"));
        else if (space != QLatin1String("2d")) problems << tr("the space %1 is not built yet: 2D and 2D axisymmetric are").arg(space);
    }
    Node selections;
    selections.type = QStringLiteral("selections");
    selections.tag = QStringLiteral("selections");
    for (const QJsonValue& s : c.value(QStringLiteral("selections")).toArray())
        if (std::optional<Node> n = nodeFrom(QStringLiteral("selections"), s, problems, QStringLiteral("features"), QStringLiteral("selection"))) {
            selections.children.push_back(*n);
        }
    comp.children.push_back(selections);
    Node geometry;
    geometry.type = QStringLiteral("geometry");
    geometry.tag = QStringLiteral("geom1");
    for (const QJsonValue& g : c.value(QStringLiteral("geometry")).toArray())
        if (std::optional<Node> n = nodeFrom(QStringLiteral("geometry"), g, problems)) geometry.children.push_back(*n);
    comp.children.push_back(geometry);
    Node materials;
    materials.type = QStringLiteral("materials");
    materials.tag = QStringLiteral("materials");
    for (const QJsonValue& mat : c.value(QStringLiteral("materials")).toArray())
        if (std::optional<Node> n = nodeFrom(QStringLiteral("materials"), mat, problems, QStringLiteral("features"), QStringLiteral("material"))) {
            // {"material": "FR-4"} names its library entry (and the node).
            if (n->props.contains(QStringLiteral("material")) && !n->props.contains(QStringLiteral("library"))) {
                n->props.insert(QStringLiteral("library"), n->props.value(QStringLiteral("material")));
                n->props.remove(QStringLiteral("material"));
            }
            if (n->label.isEmpty()) n->label = n->props.value(QStringLiteral("library")).toString();
            materials.children.push_back(*n);
        }
    comp.children.push_back(materials);
    for (const QJsonValue& ph : c.value(QStringLiteral("physics")).toArray())
        if (std::optional<Node> n = nodeFrom(QStringLiteral("component"), ph, problems)) comp.children.push_back(*n);
    Node mesh;
    mesh.type = QStringLiteral("mesh");
    mesh.tag = QStringLiteral("mesh1");
    {
        const QJsonObject mo = c.value(QStringLiteral("mesh")).toObject();
        for (auto it = mo.begin(); it != mo.end(); ++it)
            if (!Reserved.contains(it.key())) mesh.props.insert(it.key(), it.value());
        if (mo.contains(QStringLiteral("tag"))) mesh.tag = mo.value(QStringLiteral("tag")).toString();
        for (const QJsonValue& f : mo.value(QStringLiteral("features")).toArray())
            if (std::optional<Node> n = nodeFrom(QStringLiteral("mesh"), f, problems)) mesh.children.push_back(*n);
    }
    comp.children.push_back(mesh);
    m.root.children.push_back(comp);

    for (const QJsonValue& s : o.value(QStringLiteral("studies")).toArray()) {
        std::optional<Node> n = nodeFrom(QStringLiteral("model"), s, problems, QStringLiteral("steps"), QStringLiteral("study"));
        if (!n) continue;
        m.root.children.push_back(*n);
    }
    Node results;
    results.type = QStringLiteral("results");
    results.tag = QStringLiteral("results");
    const QJsonObject ro = o.value(QStringLiteral("results")).toObject();
    for (const QJsonValue& p : ro.value(QStringLiteral("plots")).toArray()) {
        std::optional<Node> n = nodeFrom(QStringLiteral("results"), p, problems, QStringLiteral("plots"), QStringLiteral("plotgroup"));
        if (!n) continue;
        results.children.push_back(*n);
    }
    Node derived;
    derived.type = QStringLiteral("derived");
    derived.tag = QStringLiteral("derived");
    for (const QJsonValue& d : ro.value(QStringLiteral("derived")).toArray())
        if (std::optional<Node> n = nodeFrom(QStringLiteral("derived"), d, problems)) derived.children.push_back(*n);
    results.children.push_back(derived);
    m.root.children.push_back(results);

    m.normalise();
    if (error) *error = problems.join(QLatin1String("; "));
    return m;
}

QJsonObject Model::toObject() const
{
    QJsonObject o;
    o.insert(QStringLiteral("format"), FormatName);
    if (!root.label.isEmpty()) o.insert(QStringLiteral("title"), root.label);
    if (root.props.contains(QStringLiteral("description")))
        o.insert(QStringLiteral("description"), root.props.value(QStringLiteral("description")));
    const Node* defs = root.child(QStringLiteral("definitions"));
    o.insert(QStringLiteral("parameters"), defs->child(QStringLiteral("parameters"))->value(QStringLiteral("rows")).toArray());
    QJsonArray functions;
    for (const Node& f : defs->children)
        if (f.type != QLatin1String("parameters")) functions.append(nodeObject(*defs, f));
    if (!functions.isEmpty()) o.insert(QStringLiteral("functions"), functions);

    const Node& comp = component();
    QJsonObject c;
    c.insert(QStringLiteral("tag"), comp.tag);
    if (!comp.label.isEmpty()) c.insert(QStringLiteral("label"), comp.label);
    c.insert(QStringLiteral("space"), QStringLiteral("2D"));
    for (auto it = comp.props.begin(); it != comp.props.end(); ++it) c.insert(it.key(), it.value());
    if (!c.contains(QStringLiteral("unit"))) c.insert(QStringLiteral("unit"), comp.text(QStringLiteral("unit")));
    QJsonArray selections, geometry, materials, physics;
    for (const Node& s : comp.child(QStringLiteral("selections"))->children)
        selections.append(nodeObject(*comp.child(QStringLiteral("selections")), s));
    for (const Node& g : this->geometry().children) geometry.append(nodeObject(this->geometry(), g));
    for (const Node& mat : this->materials().children) materials.append(nodeObject(this->materials(), mat));
    for (const Node& ph : comp.children)
        if (physicsTypes().contains(ph.type) || (!ph.kind() && ph.type != QLatin1String("mesh"))) physics.append(nodeObject(comp, ph));
    if (!selections.isEmpty()) c.insert(QStringLiteral("selections"), selections);
    c.insert(QStringLiteral("geometry"), geometry);
    c.insert(QStringLiteral("materials"), materials);
    c.insert(QStringLiteral("physics"), physics);
    {
        const Node& m = mesh();
        QJsonObject mo;
        mo.insert(QStringLiteral("tag"), m.tag);
        for (auto it = m.props.begin(); it != m.props.end(); ++it) mo.insert(it.key(), it.value());
        QJsonArray features;
        for (const Node& f : m.children) features.append(nodeObject(m, f));
        if (!features.isEmpty()) mo.insert(QStringLiteral("features"), features);
        c.insert(QStringLiteral("mesh"), mo);
    }
    o.insert(QStringLiteral("components"), QJsonArray{c});

    QJsonArray studies;
    for (const Node& s : root.children)
        if (s.type == QLatin1String("study")) studies.append(nodeObject(root, s, QStringLiteral("steps")));
    o.insert(QStringLiteral("studies"), studies);
    QJsonObject r;
    QJsonArray plots, derived;
    for (const Node& p : results().children) {
        if (p.type == QLatin1String("derived")) {
            for (const Node& d : p.children) derived.append(nodeObject(p, d));
        } else {
            plots.append(nodeObject(results(), p, QStringLiteral("plots")));
        }
    }
    r.insert(QStringLiteral("plots"), plots);
    r.insert(QStringLiteral("derived"), derived);
    o.insert(QStringLiteral("results"), r);
    return o;
}

QByteArray Model::toJson() const
{
    return QJsonDocument(toObject()).toJson(QJsonDocument::Indented);
}

// ------------------------------------------------------------------ Parameters

std::shared_ptr<ParameterScope> evaluateParameters(const Model& model, const QHash<QString, QString>& overrides)
{
    auto ps = std::make_shared<ParameterScope>();
    static const QRegularExpression identifier(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
    // The functions first: a parameter may use one.
    const Node* defs = model.root.child(QStringLiteral("definitions"));
    for (const Node& f : defs->children) {
        if (!f.enabled) continue;
        const QString name = f.text(QStringLiteral("name")).trimmed();
        if (f.type == QLatin1String("analytic")) {
            Scope::Function fn;
            for (const QString& a : f.text(QStringLiteral("arguments")).split(QLatin1Char(','), Qt::SkipEmptyParts)) fn.arguments << a.trimmed();
            const QStringList units = f.text(QStringLiteral("argunits")).split(QLatin1Char(','));
            for (int i = 0; i < fn.arguments.size(); ++i) {
                const std::optional<Unit> u = parseUnit(i < units.size() ? units[i].trimmed() : QString());
                fn.argumentDims << (u ? u->dim : Dim());
            }
            fn.body = f.text(QStringLiteral("expression"));
            const std::optional<Unit> u = parseUnit(f.text(QStringLiteral("unit")));
            fn.dim = u ? u->dim : Dim();
            ps->scope.setFunction(name, fn);
        } else if (f.type == QLatin1String("interpolation")) {
            auto table = std::make_shared<Expression::Table>();
            const std::optional<Unit> xu = parseUnit(f.text(QStringLiteral("argunit")));
            const std::optional<Unit> yu = parseUnit(f.text(QStringLiteral("unit")));
            std::vector<std::pair<double, double>> points;
            for (const QJsonValue& row : f.value(QStringLiteral("data")).toArray()) {
                const QJsonArray r = row.toArray();
                if (r.size() < 2) continue;
                auto number = [](const QJsonValue& v) { return v.isDouble() ? v.toDouble() : v.toString().toDouble(); };
                points.emplace_back(number(r.at(0)) * (xu ? xu->scale : 1) + (xu ? xu->offset : 0),
                                    number(r.at(1)) * (yu ? yu->scale : 1) + (yu ? yu->offset : 0));
            }
            std::sort(points.begin(), points.end());
            for (const auto& p : points) {
                table->x.push_back(p.first);
                table->y.push_back(p.second);
            }
            Scope::Function fn;
            fn.arguments << QStringLiteral("x");
            fn.argumentDims << (xu ? xu->dim : Dim());
            fn.table = table;
            fn.dim = yu ? yu->dim : Dim();
            ps->scope.setFunction(name, fn);
        }
    }
    for (const Model::Parameter& p : model.parameters()) {
        const QString name = p.name.trimmed();
        if (name.isEmpty()) continue;
        if (!identifier.match(name).hasMatch()) {
            ps->errors.insert(name, tr("'%1' is not a name: letters, digits and _, not a digit first").arg(name));
            continue;
        }
        const QString text = overrides.contains(name) ? overrides.value(name) : p.expression;
        const Expression e = Expression::compile(text, ps->scope);
        if (!e.isValid()) {
            ps->errors.insert(name, e.error());
            continue;
        }
        if (!e.isConstant()) {
            ps->errors.insert(name, tr("a parameter must be a constant"));
            continue;
        }
        if (!e.warnings().isEmpty()) ps->warnings.insert(name, e.warnings().join(QLatin1String("; ")));
        ps->scope.setConstant(name, e.constant(), e.dim());
        ps->values.insert(name, e.constant());
        ps->dims.insert(name, e.dim());
    }
    return ps;
}

} // namespace qucs_s::fem
