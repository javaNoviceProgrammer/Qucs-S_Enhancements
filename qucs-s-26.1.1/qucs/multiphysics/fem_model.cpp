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

/// A B-H curve: rows of B and H.
P bhRows()
{
    P p;
    p.key = QStringLiteral("BH");
    p.label = tr("B-H curve");
    p.kind = P::Rows;
    p.choiceLabels = {tr("B (T)"), tr("H (A/m)")};
    p.defaultValue = QJsonArray();
    p.tooltip = tr("A soft magnetic material's curve: B in T and H in A/m (or with units) from (0, 0), rising; "
                   "beyond its last point H grows as in vacuum");
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
         {QStringLiteral("electrostatics"), QStringLiteral("currents"), QStringLiteral("heat"), QStringLiteral("solid"),
          QStringLiteral("magnetic"), QStringLiteral("modes"), QStringLiteral("waves"), QStringLiteral("semiconductor"), QStringLiteral("rays"),
          QStringLiteral("quantum"), QStringLiteral("pde")},
         {}, true, true, QString(), tr("The model's 2D space: its geometry, materials, physics and mesh.")});
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
        QStringLiteral("intersection"), QStringLiteral("import"), QStringLiteral("crosssection")};
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
        file.tooltip = tr("A DXF or SVG drawing, or a GDSII or OASIS layout, its path relative to the model's folder");
        add({QStringLiteral("import"), tr("Import"), QStringLiteral("geometry"), QStringLiteral("import"), Level::None,
             {file,
              choice(QStringLiteral("format"), tr("Format"),
                     {QStringLiteral("auto"), QStringLiteral("dxf"), QStringLiteral("svg"), QStringLiteral("gds"), QStringLiteral("oasis")},
                     {tr("By its suffix"), tr("DXF (AutoCAD)"), tr("SVG"), tr("GDSII layout"), tr("OASIS layout")}, QStringLiteral("auto")),
              shownIf(text(QStringLiteral("layers"), tr("Layers"), QString(),
                           tr("A DXF's layers to read, split by commas; a layout's as layer/datatype: 1/0, 2/0, 3/* (empty: all)")),
                      QStringLiteral("format!=svg")),
              shownIf(text(QStringLiteral("cell"), tr("Cell"), QString(), tr("A layout's cell, flattened (empty: its largest top cell)")),
                      QStringLiteral("format!=svg&format!=dxf")),
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
                "curves. A layout's cell seen from above (GDSII, OASIS; in µm): an object for each layer, name_layer_datatype, "
                "its shapes made one. Read again when the file changes.")});
        P layout = file;
        layout.tooltip = tr("A GDSII or OASIS layout, its path relative to the model's folder");
        P stack;
        stack.key = QStringLiteral("stack");
        stack.label = tr("Layer stack");
        stack.kind = P::Rows;
        stack.choiceLabels = {tr("Name"), tr("Layer"), tr("z (µm)"), tr("Thickness (µm)")};
        stack.defaultValue = QJsonArray();
        stack.tooltip = tr("Each row an object of that name: where its layer (as 1/0) is under the cut, from z to z + thickness; "
                           "no layer: all along the cut (a substrate, an oxide, air). µm, or with units");
        add({QStringLiteral("crosssection"), tr("Layout Cross-Section"), QStringLiteral("geometry"), QStringLiteral("crosssection"), Level::None,
             {layout, text(QStringLiteral("cell"), tr("Cell"), QString(), tr("Flattened (empty: its largest top cell)")),
              pair(QStringLiteral("start"), tr("Cut from (x, y)"), QString(), QStringLiteral("0"), QStringLiteral("0"),
                   tr("In the layout's coordinates: µm, or with units")),
              pair(QStringLiteral("end"), tr("Cut to (x, y)"), QString(), QStringLiteral("10"), QStringLiteral("0")), stack,
              choice(QStringLiteral("origin"), tr("x = 0 at"), {QStringLiteral("start"), QStringLiteral("center")},
                     {tr("The cut's start"), tr("Its middle")}, QStringLiteral("start")),
              pair(QStringLiteral("position"), tr("Displacement"), QStringLiteral("length"), QStringLiteral("0"), QStringLiteral("0"))},
             {}, {}, false, false, QStringLiteral("xs"),
             tr("A slice of a layout along a cut line: x along the cut, y its layer stack's heights - each row of the stack a "
                "rectangle where its layer lies under the cut (a row without a layer: all along it). Its objects are named as the "
                "rows, for the materials and the physics. Read again when the layout changes.")});
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
          spatial(QStringLiteral("tand"), tr("Loss tangent tan δ"), QString(), QString(),
                  tr("Of a dielectric: a line's conductance per metre G = ω C tan δ")),
          spatial(QStringLiteral("mur"), tr("Relative permeability µr"), QString(), QString()),
          bhRows(),
          expr(QStringLiteral("Br"), tr("Remanent flux density Br"), QStringLiteral("T"), QString(), tr("Of a permanent magnet")),
          spatial(QStringLiteral("E"), tr("Young's modulus E"), QStringLiteral("Pa"), QString()),
          spatial(QStringLiteral("nu"), tr("Poisson's ratio ν"), QString(), QString()),
          spatial(QStringLiteral("alpha"), tr("Thermal expansion α"), QStringLiteral("1/K"), QString()),
          spatial(QStringLiteral("n"), tr("Refractive index n"), QString(), QString()),
          spatial(QStringLiteral("kappa"), tr("Extinction coefficient κ"), QString(), QString(), tr("The refractive index's imaginary part: n - jκ")),
          expr(QStringLiteral("dndT"), tr("Thermo-optic coefficient dn/dT"), QStringLiteral("1/K"), QString()),
          expr(QStringLiteral("Eg"), tr("Band gap Eg"), QStringLiteral("eV"), QString(),
               tr("A semiconductor's (an insulator has none). Given, it is the gap every physics uses, over its compound's - at "
                  "300 K: quantum wells at another temperature move it as the compound's moves. With a compound, the valence band "
                  "stays at the compound's VBO, so a gap given moves the conduction band (give VBO too to move the other)")),
          expr(QStringLiteral("lambdag"), tr("Gap wavelength λg"), QStringLiteral("m"), QString(),
               tr("Eg = hc/λg when Eg is not given: a quaternary named by its gap's wavelength (\"1.3Q\": 1.3[um]) - its masses "
                  "and deformation potentials still its compound's")),
          expr(QStringLiteral("chi"), tr("Electron affinity χ"), QStringLiteral("eV"), QString()),
          expr(QStringLiteral("Nc"), tr("Effective density of states Nc"), QStringLiteral("1/m^3"), QString()),
          expr(QStringLiteral("Nv"), tr("Effective density of states Nv"), QStringLiteral("1/m^3"), QString()),
          expr(QStringLiteral("mun"), tr("Electron mobility µn"), QStringLiteral("m^2/(V*s)"), QString()),
          expr(QStringLiteral("mup"), tr("Hole mobility µp"), QStringLiteral("m^2/(V*s)"), QString()),
          expr(QStringLiteral("taun"), tr("Electron lifetime τn"), QStringLiteral("s"), QString()),
          expr(QStringLiteral("taup"), tr("Hole lifetime τp"), QStringLiteral("s"), QString()),
          expr(QStringLiteral("Egd"), tr("Direct gap Egd"), QStringLiteral("eV"), QString(),
               tr("Of an indirect semiconductor: its direct gap, where light is absorbed strongly (germanium's 0.80 eV)")),
          expr(QStringLiteral("mr"), tr("Direct gap's reduced mass µ (m0)"), QString(), QString(), tr("The Franz-Keldysh effect's")),
          expr(QStringLiteral("Afk"), tr("Direct gap's strength A"), QStringLiteral("1/m"), QString(),
               tr("α = A √((ħω - Egd)/1 eV) (1 eV/ħω) without a field")),
          text(QStringLiteral("compound"), tr("III-V compound"), QString(),
               tr("GaAs, AlAs, InAs, InP, GaP, GaSb, AlSb, InSb or an alloy of them - AlGaAs, InGaAs, InAlAs, GaInP, GaAsP, InAsP, "
                  "GaAsSb, InGaAsP, AlGaInAs: its band parameters at its composition, for quantum wells and the semiconductor; "
                  "each property given on the material (or its library entry) is used over the compound's, in every physics")),
          choice(QStringLiteral("bands"), tr("Band gaps"), {QStringLiteral("vurgaftman"), QStringLiteral("measured")},
                 {tr("Vurgaftman et al. (2001)"), tr("Fitted to gaps measured at 300 K")}, QStringLiteral("vurgaftman"),
                 tr("Vurgaftman's: 0 K values and Varshni's law, 9-16 meV off at 300 K in the InP family (InP 1.353 eV, "
                    "In0.53Ga0.47As 0.737). The fit: InP 1.344, In0.53Ga0.47As 0.750, In0.52Al0.48As 1.46, InGaAsP on InP's "
                    "lattice within 7 meV of Nahory's, the 1.3 µm AlGaInAs 0.954 - Varshni's slope kept; the masses, offsets and "
                    "deformation potentials Vurgaftman's either way")),
          expr(QStringLiteral("x"), tr("Composition x"), QString(), QString(),
               tr("An alloy's: AlGaAs, Al's share; InGaAs, InAlAs, In's; GaInP, Ga's; GaAsP, InAsP, GaAsSb, As's; InGaAsP "
                  "(In1-x Gax Asy P1-y), Ga's; AlGaInAs (Alx Gay In1-x-y As), Al's")),
          expr(QStringLiteral("y"), tr("Composition y"), QString(), QString(), tr("InGaAsP, As's share; AlGaInAs, Ga's")),
          expr(QStringLiteral("me"), tr("Electron effective mass (m0)"), QString(), QString()),
          expr(QStringLiteral("gamma1"), tr("Luttinger parameter γ1"), QString(), QString(),
               tr("The holes' masses: heavy 1/(γ1 - 2γ2) along the growth axis, 1/(γ1 + γ2) across it; light 1/(γ1 + 2γ2), 1/(γ1 - γ2)")),
          expr(QStringLiteral("gamma2"), tr("Luttinger parameter γ2"), QString(), QString()),
          expr(QStringLiteral("gamma3"), tr("Luttinger parameter γ3"), QString(), QString()),
          expr(QStringLiteral("VBO"), tr("Valence band offset"), QStringLiteral("eV"), QString(),
               tr("The valence band's top on one scale for all (Vurgaftman's: InSb's at 0); without it, the affinity's rule")),
          expr(QStringLiteral("Ep"), tr("Kane energy Ep"), QStringLiteral("eV"), QString(), tr("The interband momentum's: 2 P²/m0")),
          expr(QStringLiteral("Delta"), tr("Split-off energy Δ"), QStringLiteral("eV"), QString()),
          expr(QStringLiteral("a0"), tr("Lattice constant"), QStringLiteral("m"), QString()),
          expr(QStringLiteral("ac"), tr("Conduction band deformation potential ac"), QStringLiteral("eV"), QString()),
          expr(QStringLiteral("av"), tr("Valence band deformation potential av"), QStringLiteral("eV"), QString(),
               tr("Chuang's sign: the gap opens by (ac - av) times the volume's relative change")),
          expr(QStringLiteral("b"), tr("Shear deformation potential b"), QStringLiteral("eV"), QString()),
          expr(QStringLiteral("C11"), tr("Elastic constant C11"), QStringLiteral("Pa"), QString()),
          expr(QStringLiteral("C12"), tr("Elastic constant C12"), QStringLiteral("Pa"), QString())},
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

    // Magnetic fields.
    add({QStringLiteral("magnetic"), tr("Magnetic Fields"), QStringLiteral("physics"), QStringLiteral("mf"), Level::Domain,
         {selectionProp(),
          choice(QStringLiteral("order"), tr("Element order"), {QStringLiteral("1"), QStringLiteral("2")}, {tr("Linear"), tr("Quadratic")},
                 QStringLiteral("2"))},
         {QStringLiteral("magnetic/ampere"), QStringLiteral("magnetic/current"), QStringLiteral("magnetic/coil"),
          QStringLiteral("magnetic/magnet"), QStringLiteral("magnetic/insulation"), QStringLiteral("magnetic/potential"),
          QStringLiteral("magnetic/pmc"), QStringLiteral("magnetic/surfacecurrent")},
         {QStringLiteral("magnetic/ampere"), QStringLiteral("magnetic/insulation")}, false, false, QStringLiteral("mf"),
         tr("The magnetic vector potential A out of the plane (round the axis): ∇×(µ⁻¹ ∇×A) = J, with B-H curves at rest "
            "and eddy currents (jωσA) in the frequency domain. Coils give an inductance or impedance matrix.")});
    add({QStringLiteral("magnetic/ampere"), tr("Ampère's Law"), QStringLiteral("mf"), QStringLiteral("domainfeature"), Level::Domain,
         {selectionProp(),
          choice(QStringLiteral("constitutive"), tr("Magnetization model"), {QStringLiteral("mur"), QStringLiteral("bh")},
                 {tr("Relative permeability"), tr("B-H curve")}, QStringLiteral("mur"),
                 tr("A B-H curve is solved by Newton's method; at rest only")),
          source,
          shownIf(spatial(QStringLiteral("mur"), tr("Relative permeability µr"), QString(), QStringLiteral("1")),
                  QStringLiteral("source=user&constitutive=mur")),
          shownIf(bhRows(), QStringLiteral("source=user&constitutive=bh")),
          shownIf(spatial(QStringLiteral("sigma"), tr("Electrical conductivity σ"), QStringLiteral("S/m"), QStringLiteral("0[S/m]")),
                  QStringLiteral("source=user"))},
         {}, {}, false, false, QStringLiteral("al"),
         tr("H = B/(µ0 µr), or by a B-H curve; J = σ E where it conducts (eddy currents in the frequency domain). From "
            "their material or as given.")});
    add({QStringLiteral("magnetic/current"), tr("External Current Density"), QStringLiteral("mf"), QStringLiteral("domainfeature"),
         Level::Domain,
         {selectionProp(none), spatial(QStringLiteral("Je"), tr("Current density Je"), QStringLiteral("A/m^2"), QStringLiteral("0[A/m^2]"),
                                       tr("Out of the plane (round the axis)"))},
         {}, {}, false, false, QStringLiteral("ecd"), tr("A current density given: out of the plane, or round the axis.")});
    add({QStringLiteral("magnetic/coil"), tr("Coil"), QStringLiteral("mf"), QStringLiteral("coil"), Level::Domain,
         {selectionProp(none), text(QStringLiteral("name"), tr("Coil name"), QStringLiteral("1")),
          choice(QStringLiteral("conductor"), tr("Conductor model"), {QStringLiteral("single"), QStringLiteral("multi")},
                 {tr("Single conductor"), tr("Homogenized multi-turn")}, QStringLiteral("single"),
                 tr("A single conductor: its current spread as its voltage drives it (the skin effect in the frequency domain); "
                    "turns of thin wire: evenly, no eddy currents")),
          expr(QStringLiteral("I0"), tr("Coil current"), QStringLiteral("A"), QStringLiteral("1[A]"),
               tr("Its amplitude in the frequency domain; out of the plane (round the axis)")),
          shownIf(expr(QStringLiteral("N"), tr("Number of turns"), QString(), QStringLiteral("10")), QStringLiteral("conductor=multi"))},
         {}, {}, false, false, QStringLiteral("coil"),
         tr("A conductor fed a current, its voltage found: mf.V_<name>, mf.R_<name>, mf.L_<name>; with coils swept, the "
            "inductance (impedance) matrix mf.L_<i>_<j>, mf.R_<i>_<j>.")});
    add({QStringLiteral("magnetic/magnet"), tr("Magnetization"), QStringLiteral("mf"), QStringLiteral("magnet"), Level::Domain,
         {selectionProp(none), source,
          shownIf(expr(QStringLiteral("Br"), tr("Remanent flux density Br"), QStringLiteral("T"), QStringLiteral("1[T]")),
                  QStringLiteral("source=user")),
          pair(QStringLiteral("direction"), tr("Direction (x, y)"), QString(), QStringLiteral("0"), QStringLiteral("1"),
               tr("Its magnetization's direction: (r, z) about the axis"))},
         {}, {}, false, false, QStringLiteral("mag"),
         tr("A permanent magnet: B = µ0 µr H + Br, µr (its recoil permeability) from Ampère's Law.")});
    add({QStringLiteral("magnetic/insulation"), tr("Magnetic Insulation"), QStringLiteral("mf"), QStringLiteral("ground"), Level::Boundary,
         {selectionProp()}, {}, {}, false, false, QStringLiteral("mi"),
         tr("A = 0: no flux crosses it (a perfect conductor). On the physics' outer boundaries.")});
    add({QStringLiteral("magnetic/potential"), tr("Magnetic Potential"), QStringLiteral("mf"), QStringLiteral("boundaryfeature"),
         Level::Boundary,
         {selectionProp(none), spatial(QStringLiteral("A0"), tr("Magnetic vector potential A0"), QStringLiteral("Wb/m"), QStringLiteral("0[Wb/m]"))},
         {}, {}, false, false, QStringLiteral("mp"), tr("A given: the flux between two boundaries is the difference of their A.")});
    add({QStringLiteral("magnetic/pmc"), tr("Perfect Magnetic Conductor"), QStringLiteral("mf"), QStringLiteral("boundaryfeature"),
         Level::Boundary, {selectionProp(none)}, {}, {}, false, false, QStringLiteral("pmc"),
         tr("n × H = 0: the field meets it at right angles (a symmetry plane of the currents).")});
    add({QStringLiteral("magnetic/surfacecurrent"), tr("Surface Current Density"), QStringLiteral("mf"), QStringLiteral("boundaryfeature"),
         Level::Boundary,
         {selectionProp(none), spatial(QStringLiteral("Js"), tr("Surface current density Js"), QStringLiteral("A/m"), QStringLiteral("0[A/m]"))},
         {}, {}, false, false, QStringLiteral("scd"), tr("A current sheet on the boundary, out of the plane.")});

    // Electromagnetic waves: a cross-section's modes, waves in the plane.
    auto waveEquation = [&](const QString& physics, const QString& title) {
        const QString p = physics + QLatin1Char('/');
        add({p + QStringLiteral("wave"), title, physics == QLatin1String("modes") ? QStringLiteral("emw") : QStringLiteral("ewfd"),
             QStringLiteral("domainfeature"), Level::Domain,
             {selectionProp(),
              choice(QStringLiteral("constitutive"), tr("Electric displacement"), {QStringLiteral("n"), QStringLiteral("epsilonr")},
                     {tr("Refractive index (n - jκ)"), tr("Relative permittivity (loss tangent, conductivity)")}, QStringLiteral("n")),
              source,
              shownIf(choice(QStringLiteral("anisotropy"), tr("Anisotropy"), {QStringLiteral("isotropic"), QStringLiteral("diagonal")},
                             {tr("Isotropic"), tr("Diagonal (x, y, z)")}, QStringLiteral("isotropic")),
                      QStringLiteral("source=user")),
              shownIf(spatial(QStringLiteral("n"), tr("Refractive index n"), QString(), QStringLiteral("1")),
                      QStringLiteral("source=user&constitutive=n&anisotropy=isotropic")),
              shownIf(spatial(QStringLiteral("nx"), tr("nx"), QString(), QStringLiteral("1")), QStringLiteral("source=user&constitutive=n&anisotropy=diagonal")),
              shownIf(spatial(QStringLiteral("ny"), tr("ny"), QString(), QStringLiteral("1")), QStringLiteral("source=user&constitutive=n&anisotropy=diagonal")),
              shownIf(spatial(QStringLiteral("nz"), tr("nz"), QString(), QStringLiteral("1")), QStringLiteral("source=user&constitutive=n&anisotropy=diagonal")),
              shownIf(spatial(QStringLiteral("kappa"), tr("Extinction coefficient κ"), QString(), QStringLiteral("0")),
                      QStringLiteral("source=user&constitutive=n")),
              shownIf(spatial(QStringLiteral("epsilonr"), tr("Relative permittivity εr"), QString(), QStringLiteral("1")),
                      QStringLiteral("source=user&constitutive=epsilonr&anisotropy=isotropic")),
              shownIf(spatial(QStringLiteral("epsxx"), tr("εxx"), QString(), QStringLiteral("1")),
                      QStringLiteral("source=user&constitutive=epsilonr&anisotropy=diagonal")),
              shownIf(spatial(QStringLiteral("epsyy"), tr("εyy"), QString(), QStringLiteral("1")),
                      QStringLiteral("source=user&constitutive=epsilonr&anisotropy=diagonal")),
              shownIf(spatial(QStringLiteral("epszz"), tr("εzz"), QString(), QStringLiteral("1")),
                      QStringLiteral("source=user&constitutive=epsilonr&anisotropy=diagonal")),
              shownIf(spatial(QStringLiteral("tand"), tr("Loss tangent tan δ"), QString(), QStringLiteral("0")),
                      QStringLiteral("source=user&constitutive=epsilonr")),
              shownIf(spatial(QStringLiteral("sigma"), tr("Electrical conductivity σ"), QStringLiteral("S/m"), QStringLiteral("0[S/m]")),
                      QStringLiteral("source=user&constitutive=epsilonr")),
              shownIf(spatial(QStringLiteral("mur"), tr("Relative permeability µr"), QString(), QStringLiteral("1")), QStringLiteral("source=user")),
              flag(QStringLiteral("thermooptic"), tr("Thermo-optic"), false,
                   tr("n + dn/dT (T - Tref): T from a heat physics (as T) or as given")),
              shownIf(expr(QStringLiteral("dndT"), tr("Thermo-optic coefficient dn/dT"), QStringLiteral("1/K"), QStringLiteral("1.86e-4[1/K]")),
                      QStringLiteral("thermooptic=true&source=user")),
              shownIf(spatial(QStringLiteral("T"), tr("Temperature"), QStringLiteral("K"), QStringLiteral("T")), QStringLiteral("thermooptic=true")),
              shownIf(expr(QStringLiteral("Tref"), tr("Reference temperature"), QStringLiteral("K"), QStringLiteral("293.15[K]")),
                      QStringLiteral("thermooptic=true")),
              flag(QStringLiteral("plasma"), tr("Plasma dispersion (free carriers)"), false,
                   tr("Silicon's free carriers: Δn = -(8.8e-22 N + 8.5e-18 P^0.8), Δα = 8.5e-18 N + 6.0e-18 P (cm⁻³, cm⁻¹; Soref and "
                      "Bennett's at 1550 nm, as λ²) - N and P from a semiconductor physics")),
              shownIf(spatial(QStringLiteral("Ncarriers"), tr("Electron concentration"), QStringLiteral("1/m^3"), QStringLiteral("semi.n")),
                      QStringLiteral("plasma=true")),
              shownIf(spatial(QStringLiteral("Pcarriers"), tr("Hole concentration"), QStringLiteral("1/m^3"), QStringLiteral("semi.p")),
                      QStringLiteral("plasma=true")),
              flag(QStringLiteral("electroabsorption"), tr("Direct-gap absorption (Franz-Keldysh)"), false,
                   tr("κ + αλ/4π, α a direct gap's: A √((ħω - Eg)/1 eV) (1 eV/ħω) without a field, and under one the Franz-Keldysh "
                      "effect - A (ħθ/1 eV)^½ π (Ai'(β)² - β Ai(β)²) (1 eV/ħω), β = (Eg - ħω)/ħθ, ħθ = (q²ħ²F²/2µ)^⅓: a tail below "
                      "the gap, an electroabsorption modulator's; germanium's absorption at 1550 nm, a photodiode's")),
              shownIf(choice(QStringLiteral("easource"), tr("Gap, mass and strength"), {QStringLiteral("material"), QStringLiteral("user")},
                             {tr("From the material (Egd, mr, Afk)"), tr("User defined")}, QStringLiteral("material")),
                      QStringLiteral("electroabsorption=true")),
              shownIf(expr(QStringLiteral("Egd"), tr("Direct gap Eg"), QStringLiteral("eV"), QStringLiteral("0.80[eV]"),
                           tr("Germanium's 0.80 eV, 0.773 eV strained on silicon; each % of silicon in germanium about +33 meV")),
                      QStringLiteral("electroabsorption=true&easource=user")),
              shownIf(expr(QStringLiteral("mr"), tr("Reduced mass µ (m0)"), QString(), QStringLiteral("0.03"),
                           tr("The electron's and hole's at the direct gap: 1/µ = 1/me + 1/mh")),
                      QStringLiteral("electroabsorption=true&easource=user")),
              shownIf(expr(QStringLiteral("Afk"), tr("Edge's strength A"), QStringLiteral("1/m"), QStringLiteral("2e4[1/cm]"),
                           tr("α = A √((ħω - Eg)/1 eV) (1 eV/ħω) above the gap without a field")),
                      QStringLiteral("electroabsorption=true&easource=user")),
              shownIf(spatial(QStringLiteral("Fea"), tr("Electric field"), QStringLiteral("V/m"), QStringLiteral("semi.normE"),
                              tr("A semiconductor physics' (semi.normE), or as given")),
                      QStringLiteral("electroabsorption=true")),
              shownIf(expr(QStringLiteral("Ebroad"), tr("Broadening σ"), QStringLiteral("eV"), QStringLiteral("0[eV]"),
                           tr("The gap spread as a Gaussian's: a tail below the gap even without a field, as a real edge has "
                              "(germanium's some meV); the ripples above it smoothed")),
                      QStringLiteral("electroabsorption=true"))},
             {}, {}, false, false, QStringLiteral("wee"),
             tr("ε from a refractive index n - jκ (its square) or a permittivity εr (1 - j tan δ) - jσ/ωε0; µr; anisotropic along x, "
                "y and z; warmed: n + dn/dT (T - Tref); free carriers' plasma dispersion; a direct gap's absorption and the "
                "Franz-Keldysh effect on it. From their material or as given.")});
        add({p + QStringLiteral("pml"), tr("Perfectly Matched Layer"), physics == QLatin1String("modes") ? QStringLiteral("emw") : QStringLiteral("ewfd"),
             QStringLiteral("pml"), Level::Domain, {selectionProp(none)}, {}, {}, false, false, QStringLiteral("pml"),
             tr("Domains beside, above or round the others that take the waves in without reflecting them: their coordinates "
                "stretched into the complex plane, more the deeper.")});
        add({p + QStringLiteral("pec"), tr("Perfect Electric Conductor"), physics == QLatin1String("modes") ? QStringLiteral("emw") : QStringLiteral("ewfd"),
             QStringLiteral("ground"), Level::Boundary, {selectionProp()}, {}, {}, false, false, QStringLiteral("pec"),
             tr("n × E = 0. On the outside by default.")});
        add({p + QStringLiteral("pmc"), tr("Perfect Magnetic Conductor"), physics == QLatin1String("modes") ? QStringLiteral("emw") : QStringLiteral("ewfd"),
             QStringLiteral("boundaryfeature"), Level::Boundary, {selectionProp(none)}, {}, {}, false, false, QStringLiteral("pmc"),
             tr("n × H = 0: a plane of symmetry of the fields.")});
    };
    add({QStringLiteral("modes"), tr("Electromagnetic Waves, Mode Analysis"), QStringLiteral("physics"), QStringLiteral("emw"), Level::Domain,
         {selectionProp(),
          choice(QStringLiteral("order"), tr("Element order"), {QStringLiteral("1"), QStringLiteral("2")},
                 {tr("Linear (Whitney's edges)"), tr("Quadratic")}, QStringLiteral("2"))},
         {QStringLiteral("modes/wave"), QStringLiteral("modes/pml"), QStringLiteral("modes/pec"), QStringLiteral("modes/pmc")},
         {QStringLiteral("modes/wave"), QStringLiteral("modes/pec")}, false, false, QStringLiteral("emw"),
         tr("The modes of a waveguide's cross-section - the fields E(x, y) e^-jβz along it, full-vector (Et on Nédélec's edge "
            "elements, Ez on Lagrange's) - their effective index β/k0 and loss, at a frequency or a wavelength.")});
    waveEquation(QStringLiteral("modes"), tr("Wave Equation, Electric"));
    add({QStringLiteral("waves"), tr("Electromagnetic Waves, Frequency Domain"), QStringLiteral("physics"), QStringLiteral("ewfd"), Level::Domain,
         {selectionProp(),
          choice(QStringLiteral("polarization"), tr("Field out of the plane"), {QStringLiteral("ez"), QStringLiteral("hz")},
                 {tr("Electric (Ez: E across the plane - TE of a parallel-plate guide)"), tr("Magnetic (Hz: E in the plane)")},
                 QStringLiteral("ez")),
          choice(QStringLiteral("order"), tr("Element order"), {QStringLiteral("1"), QStringLiteral("2")}, {tr("Linear"), tr("Quadratic")},
                 QStringLiteral("2"))},
         {QStringLiteral("waves/wave"), QStringLiteral("waves/pml"), QStringLiteral("waves/pec"), QStringLiteral("waves/pmc"),
          QStringLiteral("waves/scattering"), QStringLiteral("waves/port")},
         {QStringLiteral("waves/wave"), QStringLiteral("waves/pec")}, false, false, QStringLiteral("ewfd"),
         tr("Waves in the plane at each frequency, Ez or Hz: -∇·(P ∇u) - k0² q u = 0. Ports give the S-parameters "
            "(ewfd.S21, ewfd.S21dB); scattering boundaries and PMLs let waves out.")});
    waveEquation(QStringLiteral("waves"), tr("Wave Equation, Electric"));
    add({QStringLiteral("waves/scattering"), tr("Scattering Boundary Condition"), QStringLiteral("ewfd"), QStringLiteral("boundaryfeature"),
         Level::Boundary,
         {selectionProp(none),
          choice(QStringLiteral("incident"), tr("Incident field"), {QStringLiteral("none"), QStringLiteral("plane")},
                 {tr("None: the waves leave"), tr("A plane wave, coming in")}, QStringLiteral("none")),
          shownIf(expr(QStringLiteral("E0"), tr("Its amplitude (Ez or Hz)"), QString(), QStringLiteral("1")), QStringLiteral("incident=plane")),
          shownIf(expr(QStringLiteral("angle"), tr("Its direction"), QStringLiteral("angle"), QStringLiteral("0"),
                       tr("Degrees from the x axis: where it goes")),
                  QStringLiteral("incident=plane"))},
         {}, {}, false, false, QStringLiteral("sctr"),
         tr("A boundary waves pass out of as into free space (first order: best at right angles), and a plane wave may come in by.")});
    add({QStringLiteral("waves/port"), tr("Port"), QStringLiteral("ewfd"), QStringLiteral("terminal"), Level::Boundary,
         {selectionProp(none), text(QStringLiteral("name"), tr("Port name"), QStringLiteral("1")),
          flag(QStringLiteral("excite"), tr("Fed"), true, tr("Swept: each port fed in turn, the others matched - the S-matrix"))},
         {}, {}, false, false, QStringLiteral("port"),
         tr("A port across a waveguide: its first mode (found on the port's line) fed at 1 W, and taken out matched; the "
            "S-parameters ewfd.S<i><j> (real and imaginary parts), ewfd.S<i><j>dB.")});

    // Semiconductors.
    add({QStringLiteral("semiconductor"), tr("Semiconductor"), QStringLiteral("physics"), QStringLiteral("semi"), Level::Domain,
         {selectionProp(),
          expr(QStringLiteral("T"), tr("Temperature"), QStringLiteral("K"), QStringLiteral("300[K]"), tr("The lattice's, the same everywhere")),
          spatial(QStringLiteral("TL"), tr("Lattice temperature for the mobilities"), QStringLiteral("K"), QString(),
                  tr("Where it varies - a heat physics' ht.T, its source semi.H: self-heating. The mobilities (TL/300 K)^-ζ there (each "
                     "material model's ζ); the carriers' statistics at T. Empty: none")) },
         {QStringLiteral("semiconductor/model"), QStringLiteral("semiconductor/doping"), QStringLiteral("semiconductor/srh"),
          QStringLiteral("semiconductor/auger"), QStringLiteral("semiconductor/radiative"), QStringLiteral("semiconductor/generation"),
          QStringLiteral("semiconductor/optical"), QStringLiteral("semiconductor/impact"), QStringLiteral("semiconductor/btbt"),
          QStringLiteral("semiconductor/quantum"),
          QStringLiteral("semiconductor/insulation"),
          QStringLiteral("semiconductor/ohmic"),
          QStringLiteral("semiconductor/schottky"), QStringLiteral("semiconductor/gate"), QStringLiteral("semiconductor/charge"),
          QStringLiteral("semiconductor/traps"), QStringLiteral("semiconductor/fn")},
         {QStringLiteral("semiconductor/model"), QStringLiteral("semiconductor/insulation")}, false, false, QStringLiteral("semi"),
         tr("Drift and diffusion of electrons and holes: Poisson's equation for the potential V, continuity for the carriers n and "
            "p (Boltzmann's statistics, or Fermi-Dirac's; band gap narrowing, dopants ionized in part, if the material model asks), "
            "their currents Scharfetter and Gummel's along each edge of linear triangles - across heterojunctions too, the bands' "
            "offsets by Anderson's rule (χ, Eg). Mobilities by doping, at an insulator's surface (Lombardi) and saturated, warmed "
            "by a lattice temperature TL; recombination, impact ionization, band-to-band tunnelling, interface traps, light's "
            "pairs; the carriers' heat semi.H for a heat physics; a quantum wells physics' states for its carriers "
            "(Schrödinger-Poisson). Domains whose material has no band gap are insulators: the potential alone. Solved by "
            "Newton's method from equilibrium, the contacts' voltages raised in steps (a sweep's next point from the last); at "
            "rest, in time, or small-signal in a frequency step (the contacts' conductance and capacitance matrices; lit, a "
            "photodiode's response).")});
    add({QStringLiteral("semiconductor/model"), tr("Semiconductor Material Model"), QStringLiteral("semi"), QStringLiteral("domainfeature"),
         Level::Domain,
         {selectionProp(), source,
          shownIf(expr(QStringLiteral("epsilonr"), tr("Relative permittivity εr"), QString(), QStringLiteral("11.7")), QStringLiteral("source=user")),
          shownIf(expr(QStringLiteral("Eg"), tr("Band gap Eg"), QStringLiteral("eV"), QStringLiteral("1.12[eV]")), QStringLiteral("source=user")),
          shownIf(expr(QStringLiteral("chi"), tr("Electron affinity χ"), QStringLiteral("eV"), QStringLiteral("4.05[eV]")), QStringLiteral("source=user")),
          shownIf(expr(QStringLiteral("Nc"), tr("Conduction band's effective density of states Nc"), QStringLiteral("1/m^3"),
                       QStringLiteral("2.8e19[1/cm^3]")),
                  QStringLiteral("source=user")),
          shownIf(expr(QStringLiteral("Nv"), tr("Valence band's effective density of states Nv"), QStringLiteral("1/m^3"),
                       QStringLiteral("1.04e19[1/cm^3]")),
                  QStringLiteral("source=user")),
          shownIf(expr(QStringLiteral("mun"), tr("Electron mobility µn"), QStringLiteral("m^2/(V*s)"), QStringLiteral("1400[cm^2/(V*s)]")),
                  QStringLiteral("source=user")),
          shownIf(expr(QStringLiteral("mup"), tr("Hole mobility µp"), QStringLiteral("m^2/(V*s)"), QStringLiteral("450[cm^2/(V*s)]")),
                  QStringLiteral("source=user")),
          choice(QStringLiteral("mobility"), tr("Mobility"), {QStringLiteral("constant"), QStringLiteral("doping")},
                 {tr("Constant (the lattice's)"), tr("By doping (Caughey and Thomas)")}, QStringLiteral("constant"),
                 tr("By doping: µmin + (µ - µmin)/(1 + (N/Nref)^α), N the donors and acceptors together")),
          shownIf(expr(QStringLiteral("munmin"), tr("Electrons' µmin"), QStringLiteral("m^2/(V*s)"), QStringLiteral("68.5[cm^2/(V*s)]")),
                  QStringLiteral("mobility=doping")),
          shownIf(expr(QStringLiteral("nrefn"), tr("Electrons' Nref"), QStringLiteral("1/m^3"), QStringLiteral("9.2e16[1/cm^3]")), QStringLiteral("mobility=doping")),
          shownIf(expr(QStringLiteral("alphan"), tr("Electrons' α"), QString(), QStringLiteral("0.711")), QStringLiteral("mobility=doping")),
          shownIf(expr(QStringLiteral("mupmin"), tr("Holes' µmin"), QStringLiteral("m^2/(V*s)"), QStringLiteral("44.9[cm^2/(V*s)]")),
                  QStringLiteral("mobility=doping")),
          shownIf(expr(QStringLiteral("nrefp"), tr("Holes' Nref"), QStringLiteral("1/m^3"), QStringLiteral("2.23e17[1/cm^3]")), QStringLiteral("mobility=doping")),
          shownIf(expr(QStringLiteral("alphap"), tr("Holes' α"), QString(), QStringLiteral("0.719")), QStringLiteral("mobility=doping")),
          expr(QStringLiteral("zetan"), tr("Electrons' mobility's temperature exponent ζ"), QString(), QStringLiteral("2.5"),
               tr("µ(TL) = µ(300 K) (TL/300 K)^-ζ where the physics' lattice temperature TL is given")),
          expr(QStringLiteral("zetap"), tr("Holes' mobility's temperature exponent ζ"), QString(), QStringLiteral("2.2")),
          flag(QStringLiteral("saturation"), tr("High-field velocity saturation"), false,
               tr("µ / (1 + (µF/vsat)^β)^(1/β), β 2 for electrons and 1 for holes; F the force along each carrier's current or "
                  "the whole field, each triangle's")),
          shownIf(expr(QStringLiteral("vsatn"), tr("Electrons' saturation velocity"), QStringLiteral("m/s"), QStringLiteral("1e7[cm/s]")),
                  QStringLiteral("saturation=true")),
          shownIf(expr(QStringLiteral("vsatp"), tr("Holes' saturation velocity"), QStringLiteral("m/s"), QStringLiteral("8e6[cm/s]")),
                  QStringLiteral("saturation=true")),
          shownIf(choice(QStringLiteral("driving"), tr("Driving force"), {QStringLiteral("current"), QStringLiteral("field")},
                         {tr("Along the current"), tr("The whole field |E|")}, QStringLiteral("current"),
                         tr("Along the current: each carrier's quasi-Fermi potential's gradient, J/(q n µ) - the field along its "
                            "current where it drifts (E·J/|J|), nought at equilibrium, no more than |E| - so a MOSFET's gate field "
                            "across its channel heats nothing. The whole field: as it was, right where field and current lie along "
                            "each other (a diode, a bar)")),
                  QStringLiteral("saturation=true")),
          flag(QStringLiteral("surface"), tr("Surface scattering (Lombardi)"), false,
               tr("At an insulator's surface - a MOSFET's channel - acoustic phonons and the surface's roughness scatter the "
                  "carriers by the field across it: 1/µ = 1/µbulk + D/µac + D/µsr, µac = B/E⊥ + C (N/1 cm⁻³)^λ / (T E⊥^⅓), µsr = "
                  "δ/E⊥², E⊥ the field across the nearest interface, D = e^(-d/lcrit) its distance's. Silicon's (Lombardi et "
                  "al. 1988) by default")),
          shownIf(expr(QStringLiteral("Bn"), tr("Electrons' B"), QStringLiteral("m/s"), QStringLiteral("4.75e7[cm/s]")), QStringLiteral("surface=true")),
          shownIf(expr(QStringLiteral("Cn"), tr("Electrons' C (cm, V, s, K)"), QString(), QStringLiteral("1.74e5"),
                       tr("In cm²/(V s) K (V/cm)^⅓: C/T 580 at 300 K")),
                  QStringLiteral("surface=true")),
          shownIf(expr(QStringLiteral("lambdan"), tr("Electrons' λ"), QString(), QStringLiteral("0.125")), QStringLiteral("surface=true")),
          shownIf(expr(QStringLiteral("deltan"), tr("Electrons' δ"), QStringLiteral("V/s"), QStringLiteral("5.82e14[V/s]")), QStringLiteral("surface=true")),
          shownIf(expr(QStringLiteral("Bp"), tr("Holes' B"), QStringLiteral("m/s"), QStringLiteral("9.925e6[cm/s]")), QStringLiteral("surface=true")),
          shownIf(expr(QStringLiteral("Cp"), tr("Holes' C (cm, V, s, K)"), QString(), QStringLiteral("8.842e5")), QStringLiteral("surface=true")),
          shownIf(expr(QStringLiteral("lambdap"), tr("Holes' λ"), QString(), QStringLiteral("0.0317")), QStringLiteral("surface=true")),
          shownIf(expr(QStringLiteral("deltap"), tr("Holes' δ"), QStringLiteral("V/s"), QStringLiteral("2.0546e14[V/s]")), QStringLiteral("surface=true")),
          shownIf(expr(QStringLiteral("lcrit"), tr("Depth lcrit"), QStringLiteral("m"), QStringLiteral("10[nm]"),
                       tr("How far from the interface the surface's scattering reaches: e^(-d/lcrit)")),
                  QStringLiteral("surface=true")),
          flag(QStringLiteral("fermi"), tr("Fermi-Dirac statistics"), false,
               tr("The carriers as Fermi-Dirac's, n = Nc ℱ1/2((Efn - Ec)/kT), not Boltzmann's Nc e^((Efn - Ec)/kT): fewer "
                  "where the Fermi level nears or enters a band (silicon's 1e20 cm⁻³ 0.30 of Boltzmann's np), their diffusion "
                  "the generalized Einstein relation's - degenerate contacts, emitters, a HEMT's channel")),
          flag(QStringLiteral("bgn"), tr("Band gap narrowing (Slotboom)"), false,
               tr("Heavy doping closes the gap: ΔEg = E (ln(N/Nref) + √(ln²(N/Nref) + C)), N the donors and acceptors - ni "
                  "e^(ΔEg/2kT), the bands' edges each ΔEg/2 nearer the intrinsic level (a doping's gradient a field of its own "
                  "on the minority carriers). Silicon's (Slotboom and de Graaff, Klaassen's values) by default")),
          shownIf(expr(QStringLiteral("Ebgn"), tr("Narrowing's energy E"), QStringLiteral("eV"), QStringLiteral("6.92[meV]")),
                  QStringLiteral("bgn=true")),
          shownIf(expr(QStringLiteral("Nbgn"), tr("Narrowing's Nref"), QStringLiteral("1/m^3"), QStringLiteral("1.3e17[1/cm^3]")),
                  QStringLiteral("bgn=true")),
          shownIf(expr(QStringLiteral("Cbgn"), tr("Narrowing's C"), QString(), QStringLiteral("0.5")), QStringLiteral("bgn=true")),
          flag(QStringLiteral("incomplete"), tr("Incomplete ionization"), false,
               tr("The dopants ionized in part: Nd⁺ = Nd/(1 + gd n/n1), n1 = Nc e^(-ΔEd/kT), Na⁻ = Na/(1 + ga p/p1), p1 = Nv "
                  "e^(-ΔEa/kT), in Poisson's equation at once (no capture in time). Silicon's phosphorus and boron by default")),
          shownIf(expr(QStringLiteral("Ed"), tr("Donors' level below Ec, ΔEd"), QStringLiteral("eV"), QStringLiteral("45[meV]")),
                  QStringLiteral("incomplete=true")),
          shownIf(expr(QStringLiteral("gd"), tr("Donors' degeneracy gd"), QString(), QStringLiteral("2")), QStringLiteral("incomplete=true")),
          shownIf(expr(QStringLiteral("Ea"), tr("Acceptors' level above Ev, ΔEa"), QStringLiteral("eV"), QStringLiteral("45[meV]")),
                  QStringLiteral("incomplete=true")),
          shownIf(expr(QStringLiteral("ga"), tr("Acceptors' degeneracy ga"), QString(), QStringLiteral("4")), QStringLiteral("incomplete=true"))},
         {}, {}, false, false, QStringLiteral("smm"),
         tr("A semiconductor's band gap, electron affinity, effective densities of states and mobilities, from its material or as "
            "given; ni = √(Nc Nv) exp(-Eg/2kT).")});
    add({QStringLiteral("semiconductor/doping"), tr("Analytic Doping Model"), QStringLiteral("semi"), QStringLiteral("domainfeature"),
         Level::Domain,
         {selectionProp(none),
          choice(QStringLiteral("dopant"), tr("Impurity"), {QStringLiteral("acceptor"), QStringLiteral("donor")},
                 {tr("Acceptors (p-type)"), tr("Donors (n-type)")}, QStringLiteral("acceptor")),
          choice(QStringLiteral("profile"), tr("Profile"),
                 {QStringLiteral("uniform"), QStringLiteral("box"), QStringLiteral("implant"), QStringLiteral("expression")},
                 {tr("Uniform"), tr("A box, Gaussian outside it"), tr("An implant, annealed"), tr("User defined")}, QStringLiteral("uniform")),
          shownIf(expr(QStringLiteral("dose"), tr("Dose"), QStringLiteral("1/m^2"), QStringLiteral("1e14[1/cm^2]")), QStringLiteral("profile=implant")),
          shownIf(expr(QStringLiteral("Rp"), tr("Projected range Rp"), QStringLiteral("m"), QStringLiteral("0.1[um]"),
                       tr("A range table's (SRIM's) for the ion, its energy and the target")),
                  QStringLiteral("profile=implant")),
          shownIf(expr(QStringLiteral("dRp"), tr("Straggle ΔRp"), QStringLiteral("m"), QStringLiteral("0.03[um]")), QStringLiteral("profile=implant")),
          shownIf(expr(QStringLiteral("dRl"), tr("Lateral straggle"), QStringLiteral("m"), QStringLiteral("0.03[um]")), QStringLiteral("profile=implant")),
          shownIf(expr(QStringLiteral("surface"), tr("Surface at y"), QStringLiteral("length"), QStringLiteral("0"),
                       tr("The ions enter here, downward (-y)")),
                  QStringLiteral("profile=implant")),
          shownIf(expr(QStringLiteral("x1"), tr("Mask's window from x"), QStringLiteral("length"), QString(), tr("Empty: no mask")),
                  QStringLiteral("profile=implant")),
          shownIf(expr(QStringLiteral("x2"), tr("Mask's window to x"), QStringLiteral("length"), QString()), QStringLiteral("profile=implant")),
          shownIf(expr(QStringLiteral("D0"), tr("Diffusivity's D0"), QStringLiteral("m^2/s"), QStringLiteral("0.76[cm^2/s]"),
                       tr("D = D0 e^(-Ea/kT), the dopant's intrinsic diffusivity: boron's (Fair) by default")),
                  QStringLiteral("profile=implant")),
          shownIf(expr(QStringLiteral("Ea"), tr("Diffusivity's Ea"), QStringLiteral("eV"), QStringLiteral("3.46[eV]")), QStringLiteral("profile=implant")),
          shownIf(expr(QStringLiteral("Ta"), tr("Anneal temperature"), QStringLiteral("K"), QStringLiteral("1273.15[K]")), QStringLiteral("profile=implant")),
          shownIf(expr(QStringLiteral("ta"), tr("Anneal time"), QStringLiteral("s"), QStringLiteral("0[s]"), tr("Nought: as implanted")),
                  QStringLiteral("profile=implant")),
          shownIf(expr(QStringLiteral("N0"), tr("Concentration N0"), QStringLiteral("1/m^3"), QStringLiteral("1e16[1/cm^3]"),
                       tr("A box's inside")),
                  QStringLiteral("profile=uniform|profile=box")),
          shownIf(pair(QStringLiteral("position"), tr("Box's corner"), QStringLiteral("length"), QStringLiteral("0"), QStringLiteral("0")),
                  QStringLiteral("profile=box")),
          shownIf(pair(QStringLiteral("size"), tr("Box's width, height"), QStringLiteral("length"), QStringLiteral("1"), QStringLiteral("1")),
                  QStringLiteral("profile=box")),
          shownIf(expr(QStringLiteral("dj"), tr("Junction depth"), QStringLiteral("m"), QStringLiteral("0.5[um]"),
                       tr("How far outside the box it falls to the background")),
                  QStringLiteral("profile=box")),
          shownIf(expr(QStringLiteral("Nb"), tr("Background concentration"), QStringLiteral("1/m^3"), QStringLiteral("1e15[1/cm^3]")),
                  QStringLiteral("profile=box")),
          shownIf(spatial(QStringLiteral("N"), tr("Concentration N"), QStringLiteral("1/m^3"), QStringLiteral("1e16[1/cm^3]"),
                          tr("Of x and y")),
                  QStringLiteral("profile=expression"))},
         {}, {}, false, false, QStringLiteral("dop"),
         tr("Donors or acceptors on its domains: uniform, a box with a Gaussian fall outside it (N0 exp(-(d/σ)²), the "
            "background reached at the junction depth), an implant - a Gaussian of its dose about its range below the surface, "
            "its mask's window's edges spread by its lateral straggle, annealed as diffusion widens it: σ² = ΔRp² + 2 D t, the "
            "surface reflecting (its image), the dose kept; exact while D is the intrinsic one, not a high concentration's -, or "
            "an expression of x and y (another physics' field: a diffusion solved in time, a PDE's u). Dopings add.")});
    add({QStringLiteral("semiconductor/srh"), tr("Trap-Assisted Recombination"), QStringLiteral("semi"), QStringLiteral("domainfeature"),
         Level::Domain,
         {selectionProp(), source,
          shownIf(expr(QStringLiteral("taun"), tr("Electron lifetime τn"), QStringLiteral("s"), QStringLiteral("10[us]")), QStringLiteral("source=user")),
          shownIf(expr(QStringLiteral("taup"), tr("Hole lifetime τp"), QStringLiteral("s"), QStringLiteral("10[us]")), QStringLiteral("source=user")),
          expr(QStringLiteral("Et"), tr("Trap level above the intrinsic level"), QStringLiteral("eV"), QStringLiteral("0[eV]"))},
         {}, {}, false, false, QStringLiteral("srh"),
         tr("Shockley-Read-Hall: R = (np - ni²) / (τp (n + n1) + τn (p + p1)).")});
    add({QStringLiteral("semiconductor/auger"), tr("Auger Recombination"), QStringLiteral("semi"), QStringLiteral("domainfeature"), Level::Domain,
         {selectionProp(none), expr(QStringLiteral("Cn"), tr("Electrons' coefficient Cn"), QStringLiteral("m^6/s"), QStringLiteral("2.8e-31[cm^6/s]")),
          expr(QStringLiteral("Cp"), tr("Holes' coefficient Cp"), QStringLiteral("m^6/s"), QStringLiteral("9.9e-32[cm^6/s]"))},
         {}, {}, false, false, QStringLiteral("aug"), tr("R = (Cn n + Cp p)(np - ni²): heavy doping, high injection.")});
    add({QStringLiteral("semiconductor/radiative"), tr("Direct Recombination"), QStringLiteral("semi"), QStringLiteral("domainfeature"),
         Level::Domain,
         {selectionProp(none), expr(QStringLiteral("B"), tr("Coefficient B"), QStringLiteral("m^3/s"), QStringLiteral("1.1e-14[cm^3/s]"))},
         {}, {}, false, false, QStringLiteral("rad"), tr("Band to band: R = B (np - ni²) (GaAs about 7e-10 cm³/s).")});
    {
        const QString user = QStringLiteral("model=user");
        const QString perCm = QStringLiteral("1/m"), fieldUnit = QStringLiteral("V/m");
        add({QStringLiteral("semiconductor/impact"), tr("Impact Ionization"), QStringLiteral("semi"), QStringLiteral("domainfeature"),
             Level::Domain,
             {selectionProp(none),
              choice(QStringLiteral("model"), tr("Model"), {QStringLiteral("silicon"), QStringLiteral("user")},
                     {tr("Silicon (van Overstraeten and de Man)"), tr("User-defined (Chynoweth's)")}, QStringLiteral("silicon")),
              flag(QStringLiteral("currents"), tr("In the carriers' continuity"), true,
                   tr("Its pairs in the equations - avalanche multiplication, an APD's gain, up to breakdown; off: the ionization "
                      "integrals alone (a breakdown voltage swept to, the currents as they are without it)")),
              shownIf(expr(QStringLiteral("an"), tr("Electrons' a"), perCm, QStringLiteral("7.03e5[1/cm]")), user),
              shownIf(expr(QStringLiteral("bn"), tr("Electrons' b"), fieldUnit, QStringLiteral("1.231e6[V/cm]")), user),
              shownIf(expr(QStringLiteral("ap"), tr("Holes' a"), perCm, QStringLiteral("1.582e6[1/cm]")), user),
              shownIf(expr(QStringLiteral("bp"), tr("Holes' b"), fieldUnit, QStringLiteral("2.036e6[V/cm]")), user),
              shownIf(expr(QStringLiteral("an2"), tr("Electrons' a above E0"), perCm, QStringLiteral("7.03e5[1/cm]")), user),
              shownIf(expr(QStringLiteral("bn2"), tr("Electrons' b above E0"), fieldUnit, QStringLiteral("1.231e6[V/cm]")), user),
              shownIf(expr(QStringLiteral("ap2"), tr("Holes' a above E0"), perCm, QStringLiteral("6.71e5[1/cm]")), user),
              shownIf(expr(QStringLiteral("bp2"), tr("Holes' b above E0"), fieldUnit, QStringLiteral("1.693e6[V/cm]")), user),
              shownIf(expr(QStringLiteral("E0"), tr("The ranges' field E0"), fieldUnit, QStringLiteral("4e5[V/cm]")), user),
              shownIf(expr(QStringLiteral("betan"), tr("Electrons' exponent β"), QString(), QStringLiteral("1")), user),
              shownIf(expr(QStringLiteral("betap"), tr("Holes' exponent β"), QString(), QStringLiteral("1")), user)},
             {}, {}, false, false, QStringLiteral("ii"),
             tr("Carriers a field accelerates make pairs: G = (αn |Jn| + αp |Jp|)/q, α = a e^(-(b/E)^β) (Chynoweth's; silicon's "
                "van Overstraeten and de Man's, its holes' two ranges about 400 kV/cm), E each triangle's field, Jn and Jp its "
                "currents - avalanche multiplication, an APD's gain. The ionization integrals along the field line through the highest field, "
                "semi.In and semi.Ip (electrons', holes': breakdown where they reach 1) and their multiplications semi.Mn, "
                "semi.Mp, the field semi.Emax: a breakdown voltage found by sweeping toward it, its currents in or not.")});
    }
    add({QStringLiteral("semiconductor/btbt"), tr("Band-to-Band Tunnelling"), QStringLiteral("semi"), QStringLiteral("domainfeature"),
         Level::Domain,
         {selectionProp(none),
          choice(QStringLiteral("kind"), tr("Sign and size"), {QStringLiteral("hurkx"), QStringLiteral("generation")},
                 {tr("Hurkx's (n p - ni²)/((n + ni)(p + ni))"), tr("Generation alone")}, QStringLiteral("hurkx"),
                 tr("Hurkx's: nought at equilibrium, recombination forward - but small wherever either carrier is above ni, a "
                    "junction's carriers spilling into its field (an i layer's edges some 10 nm at 0.6 MV/cm). Generation alone: "
                    "A E^γ e^(-B/E) wherever the field is, a reversed tunnel junction's - not at equilibrium nought")),
          expr(QStringLiteral("A"), tr("A (cm^-1/2 V^-5/2 s^-1 for γ 2.5)"), QString(), QStringLiteral("4e14"),
               tr("In 1/(cm³ s) per (V/cm)^γ: silicon's 4e14 (Hurkx and Klaassen)")),
          expr(QStringLiteral("B"), tr("B"), QStringLiteral("V/m"), QStringLiteral("1.9e7[V/cm]")),
          expr(QStringLiteral("gamma"), tr("Exponent γ"), QString(), QStringLiteral("2.5"), tr("2.5 phonon-assisted (an indirect gap), 2 direct"))},
         {}, {}, false, false, QStringLiteral("bbt"),
         tr("Electrons tunnelling from the valence band to the conduction band where the field is strong - a Zener diode's "
            "breakdown, a MOSFET's gate-induced drain leakage, a tunnel FET: Hurkx's R = A E^γ e^(-B/E) (n p - ni²)/((n + ni)(p "
            "+ ni)), generation where the carriers are fewer than at equilibrium, each triangle's field. Local: the field at a "
            "point stands for the tunnelling path's.")});
    add({QStringLiteral("semiconductor/quantum"), tr("Schrödinger-Poisson"), QStringLiteral("semi"), QStringLiteral("domainfeature"),
         Level::Domain,
         {selectionProp(none), flag(QStringLiteral("electrons"), tr("Electrons"), true), flag(QStringLiteral("holes"), tr("Holes"), false),
          shownIf(spatial(QStringLiteral("nq"), tr("Electrons in the states"), QStringLiteral("1/m^3"), QStringLiteral("qw.n"),
                          tr("A quantum wells physics' electrons in its states (qw.n, its Fermi level semi.Efn - semi.Ec)")),
                  QStringLiteral("electrons=true")),
          shownIf(spatial(QStringLiteral("pq"), tr("Holes in the states"), QStringLiteral("1/m^3"), QStringLiteral("qw.p"),
                          tr("Its holes (qw.p, their Fermi level semi.Ev - semi.Efp)")),
                  QStringLiteral("holes=true"))},
         {}, {}, false, false, QStringLiteral("sp"),
         tr("The carriers here as quantum states have them - an inversion layer's, a 2DEG's, a well's: a Quantum States step "
            "with Self-consistent solves the semiconductor and the states in turn, each node's carriers moved to the states' by a "
            "quantum potential in its band, Vt ln(n_states/n), until it rests - Poisson's equation, the currents and the contacts "
            "with it.")});
    add({QStringLiteral("semiconductor/generation"), tr("User-Defined Generation"), QStringLiteral("semi"), QStringLiteral("domainfeature"),
         Level::Domain,
         {selectionProp(none), spatial(QStringLiteral("G"), tr("Generation rate G"), QStringLiteral("1/(m^3*s)"), QStringLiteral("0"),
                                       tr("Pairs made per volume and time: of x, y, t, other physics' fields (light absorbed)"))},
         {}, {}, false, false, QStringLiteral("gen"), tr("Electron-hole pairs made: optical generation, say.")});
    add({QStringLiteral("semiconductor/optical"), tr("Optical Generation"), QStringLiteral("semi"), QStringLiteral("domainfeature"),
         Level::Domain,
         {selectionProp(none), text(QStringLiteral("mode"), tr("Mode analysis"), QStringLiteral("emw"), tr("The Electromagnetic Waves, Mode Analysis whose mode is fed")),
          integer(QStringLiteral("number"), tr("Mode number"), 1,
                  tr("Which of the mode step's modes is fed: 1 the highest effective index, as emw.mode numbers them (the step "
                     "leaves it selected; it must find that many)")),
          expr(QStringLiteral("P0"), tr("Power fed"), QStringLiteral("W"), QStringLiteral("1[mW]"), tr("The mode's at the device's start; of t in time (a pulse)")),
          expr(QStringLiteral("eta"), tr("Internal quantum efficiency η"), QString(), QStringLiteral("1"), tr("Pairs made per photon absorbed across the gap")),
          expr(QStringLiteral("RL"), tr("Load resistance"), QStringLiteral("Ohm"), QStringLiteral("50[Ohm]"),
               tr("A frequency step's response through it: semi.HopRL_<contact>, the device's capacitance charged through it"))},
         {}, {}, false, false, QStringLiteral("opg"),
         tr("A waveguide photodiode's light: a mode fed P0, solved before (a Mode Analysis step) - each photon it loses across the gap "
            "(emw.Qgap: the media's κ and a direct gap's, not the free carriers') a pair, its power falling along the device as "
            "e^(-2αz), the mean over its length (the thickness). semi.Gop pairs per second - never more than the photons the mode "
            "loses -, semi.Iop = q Gop, semi.Pin; in a frequency step semi.Hop_<contact> - each contact's photocurrent per pair, the "
            "light modulated - and semi.HopRL_<contact> through the load. The cross-section's one mode is taken to carry all of P0: "
            "how a waveguide's light divides among a section's modes (germanium butt-coupled to silicon) is a question for a 3D "
            "or mode-expansion model, and responsivity can differ by several percent from one mode to another - the mode number "
            "is an assumption to state.")});
    add({QStringLiteral("semiconductor/insulation"), tr("Insulation"), QStringLiteral("semi"), QStringLiteral("boundaryfeature"), Level::Boundary,
         {selectionProp()}, {}, {}, false, false, QStringLiteral("ins"),
         tr("No current and no charge through the boundary. Where no other condition is.")});
    add({QStringLiteral("semiconductor/ohmic"), tr("Metal Contact"), QStringLiteral("semi"), QStringLiteral("terminal"), Level::Boundary,
         {selectionProp(none), text(QStringLiteral("name"), tr("Contact name"), QStringLiteral("1")),
          expr(QStringLiteral("V0"), tr("Voltage"), QStringLiteral("V"), QStringLiteral("0[V]"), tr("A parameter to sweep it; of t in time"))},
         {}, {}, false, false, QStringLiteral("mc"),
         tr("An ohmic contact: the carriers at equilibrium and charge neutrality, the quasi-Fermi levels at -q V0. Its current "
            "semi.I_<name> (into the device), its charge semi.Q_<name>.")});
    add({QStringLiteral("semiconductor/schottky"), tr("Schottky Contact"), QStringLiteral("semi"), QStringLiteral("terminal"), Level::Boundary,
         {selectionProp(none), text(QStringLiteral("name"), tr("Contact name"), QStringLiteral("1")),
          expr(QStringLiteral("V0"), tr("Voltage"), QStringLiteral("V"), QStringLiteral("0[V]")),
          expr(QStringLiteral("phiB"), tr("Barrier height"), QStringLiteral("eV"), QStringLiteral("0.8[eV]"), tr("The metal's work function less χ")),
          expr(QStringLiteral("vn"), tr("Electrons' recombination velocity"), QStringLiteral("m/s"), QStringLiteral("1e7[cm/s]"),
               tr("A* T² / (q Nc): Richardson's")),
          expr(QStringLiteral("vp"), tr("Holes' recombination velocity"), QStringLiteral("m/s"), QStringLiteral("1e7[cm/s]"))},
         {}, {}, false, false, QStringLiteral("sc"), tr("A metal on the semiconductor with a barrier: thermionic emission over it.")});
    add({QStringLiteral("semiconductor/gate"), tr("Gate Contact"), QStringLiteral("semi"), QStringLiteral("terminal"), Level::Boundary,
         {selectionProp(none), text(QStringLiteral("name"), tr("Contact name"), QStringLiteral("g")),
          expr(QStringLiteral("V0"), tr("Voltage"), QStringLiteral("V"), QStringLiteral("0[V]")),
          expr(QStringLiteral("phim"), tr("Metal work function"), QStringLiteral("eV"), QStringLiteral("4.05[eV]"),
               tr("n⁺ polysilicon 4.05 eV, aluminium 4.1 eV, p⁺ polysilicon 5.17 eV"))},
         {}, {}, false, false, QStringLiteral("gc"),
         tr("A metal on an insulator (the gate oxide drawn as a domain): its potential V0 less the work function's difference. "
            "Its charge semi.Q_<name>; small-signal, its capacitance.")});
    add({QStringLiteral("semiconductor/charge"), tr("Surface Charge Density"), QStringLiteral("semi"), QStringLiteral("boundaryfeature"),
         Level::Boundary,
         {selectionProp(none), spatial(QStringLiteral("rhos"), tr("Surface charge density"), QStringLiteral("C/m^2"), QStringLiteral("0"),
                                       tr("Fixed charge at an interface: an oxide's, q times 1e10 to 1e11 per cm²"))},
         {}, {}, false, false, QStringLiteral("scd"), tr("Charge fixed on a boundary.")});
    add({QStringLiteral("semiconductor/fn"), tr("Fowler-Nordheim Tunnelling"), QStringLiteral("semi"), QStringLiteral("boundaryfeature"),
         Level::Boundary,
         {selectionProp(none),
          expr(QStringLiteral("phiB"), tr("Barrier φB"), QStringLiteral("eV"), QStringLiteral("3.1[eV]"), tr("Silicon's conduction band to silicon dioxide's")),
          expr(QStringLiteral("mox"), tr("Mass in the insulator (m0)"), QString(), QStringLiteral("0.42"))},
         {}, {}, false, false, QStringLiteral("fn"),
         tr("Electrons tunnelling through an insulator in a strong field - a gate oxide's leakage, a flash cell's writing: J = A "
            "E² e^(-B/E), A = q³ m0/(8π h φB m_ox), B = 8π √(2 m_ox) φB^(3/2)/(3 q h) (Lenzlinger and Snow; SiO₂ on silicon about "
            "1.2e-6 A/V² and 240 MV/cm), E the insulator's field across the boundary: semi.Ifn, its current after the solve - not "
            "taken from the carriers (a leakage far below the device's own currents).")});
    {
        const QString level = QStringLiteral("distribution=level"), spread = QStringLiteral("distribution=uniform");
        add({QStringLiteral("semiconductor/traps"), tr("Interface Traps"), QStringLiteral("semi"), QStringLiteral("boundaryfeature"),
             Level::Boundary,
             {selectionProp(none),
              choice(QStringLiteral("kind"), tr("Kind"), {QStringLiteral("acceptor"), QStringLiteral("donor")},
                     {tr("Acceptor-like (negative when filled)"), tr("Donor-like (positive when empty)")}, QStringLiteral("acceptor")),
              choice(QStringLiteral("distribution"), tr("In energy"), {QStringLiteral("level"), QStringLiteral("uniform")},
                     {tr("A single level"), tr("Uniform over a range (Dit)")}, QStringLiteral("uniform")),
              shownIf(expr(QStringLiteral("Nit"), tr("Density Nit"), QStringLiteral("1/m^2"), QStringLiteral("1e11[1/cm^2]")), level),
              shownIf(expr(QStringLiteral("Et"), tr("Level above the intrinsic one, Et - Ei"), QStringLiteral("eV"), QStringLiteral("0[eV]")), level),
              shownIf(expr(QStringLiteral("Dit"), tr("Density per energy Dit"), QStringLiteral("1/(m^2*J)"), QStringLiteral("1e11[1/(cm^2*eV)]")), spread),
              shownIf(expr(QStringLiteral("Elow"), tr("From, above the intrinsic level"), QStringLiteral("eV"), QStringLiteral("-0.5[eV]")), spread),
              shownIf(expr(QStringLiteral("Ehigh"), tr("To, above the intrinsic level"), QStringLiteral("eV"), QStringLiteral("0.5[eV]")), spread),
              shownIf(integer(QStringLiteral("levels"), tr("Levels"), 64, tr("The range in this many levels, each Dit ΔE")), spread),
              expr(QStringLiteral("sigman"), tr("Electrons' capture cross section σn"), QStringLiteral("m^2"), QStringLiteral("1e-16[cm^2]")),
              expr(QStringLiteral("sigmap"), tr("Holes' capture cross section σp"), QStringLiteral("m^2"), QStringLiteral("1e-16[cm^2]")),
              expr(QStringLiteral("vth"), tr("Thermal velocity"), QStringLiteral("m/s"), QStringLiteral("1e7[cm/s]"))},
             {}, {}, false, false, QStringLiteral("trp"),
             tr("Traps at an interface - an oxide's: each level filled as Shockley, Read and Hall have it, f = (cn n + cp p1)/(cn "
                "(n + n1) + cp (p + p1)), cn = σn vth, n1 = ni e^((Et - Ei)/kT); its charge -q Nit f (acceptor-like) or q Nit (1 - f) "
                "(donor-like), and the pairs it recombines, Nit cn cp (n p - ni²)/(cn (n + n1) + cp (p + p1)). At rest the C-V curve "
                "stretched out; small-signal each level follows with its time, 1/(cn (n + n1) + cp (p + p1)) - the conductance "
                "method's peak. In time the occupancy follows at once (a slow trap's own state not kept).")});
    }

    // Geometrical optics.
    add({QStringLiteral("rays"), tr("Geometrical Optics"), QStringLiteral("physics"), QStringLiteral("gop"), Level::Domain,
         {selectionProp(),
          integer(QStringLiteral("maxinteractions"), tr("Most boundaries a ray meets"), 100),
          expr(QStringLiteral("threshold"), tr("Power threshold"), QString(), QStringLiteral("1e-4"),
               tr("A ray ends below this part of its release's power")),
          flag(QStringLiteral("reflections"), tr("Release reflected rays"), true,
               tr("At each refracting boundary a reflected ray too, Fresnel's part of the power")),
          integer(QStringLiteral("maxrays"), tr("Most rays traced from a release"), 20000)},
         {QStringLiteral("rays/medium"), QStringLiteral("rays/point"), QStringLiteral("rays/beam"), QStringLiteral("rays/boundary"),
          QStringLiteral("rays/mirror"), QStringLiteral("rays/absorber"), QStringLiteral("rays/coating"), QStringLiteral("rays/detector")},
         {QStringLiteral("rays/medium")}, false, false, QStringLiteral("gop"),
         tr("Rays traced through the domains, a Stationary step tracing them: straight where the refractive index is uniform, "
            "bent by the ray equation where it varies; refracted by Snell's law and split by Fresnel's coefficients at each "
            "boundary between media, wholly reflected past the critical angle; absorbed as they go where n - jκ has a κ, that "
            "power gop.Q per volume (a heat source). They stop on the physics' outside.")});
    add({QStringLiteral("rays/medium"), tr("Medium Properties"), QStringLiteral("gop"), QStringLiteral("domainfeature"), Level::Domain,
         {selectionProp(), source,
          shownIf(spatial(QStringLiteral("n"), tr("Refractive index n"), QString(), QStringLiteral("1.5"),
                          tr("Of x and y (a graded index), of lambda0 (dispersion), of another physics' field (T: a thermal lens)")),
                  QStringLiteral("source=user")),
          shownIf(spatial(QStringLiteral("kappa"), tr("Extinction coefficient κ"), QString(), QStringLiteral("0"),
                          tr("Its absorption: α = 4πκ/λ")),
                  QStringLiteral("source=user"))},
         {}, {}, false, false, QStringLiteral("mp"), tr("The refractive index the rays see: from the material, or as given.")});
    const P wavelengths = text(QStringLiteral("lambda0"), tr("Wavelengths"), QStringLiteral("550[nm]"),
                               tr("In vacuum; several split by commas, each released"));
    const P raysCount = integer(QStringLiteral("count"), tr("Rays"), 21);
    const P raysPower = expr(QStringLiteral("P0"), tr("Power"), QStringLiteral("W"), QStringLiteral("1[W]"),
                             tr("Of all its rays, each wavelength's"));
    const P polarization = choice(QStringLiteral("polarization"), tr("Polarization"),
                                  {QStringLiteral("unpolarized"), QStringLiteral("s"), QStringLiteral("p")},
                                  {tr("Unpolarized"), tr("s (E out of the plane)"), tr("p (E in the plane)")}, QStringLiteral("unpolarized"));
    add({QStringLiteral("rays/point"), tr("Release from Point"), QStringLiteral("gop"), QStringLiteral("point"), Level::None,
         {pair(QStringLiteral("position"), tr("Position"), QStringLiteral("length"), QStringLiteral("0"), QStringLiteral("0")),
          expr(QStringLiteral("angle"), tr("Direction"), QStringLiteral("angle"), QStringLiteral("0"), tr("Degrees from the x axis")),
          expr(QStringLiteral("spread"), tr("Half-angle"), QStringLiteral("angle"), QStringLiteral("10"),
               tr("Degrees each side of the direction (180: all round)")),
          raysCount, raysPower, wavelengths, polarization},
         {}, {}, false, false, QStringLiteral("relp"), tr("A fan of rays from a point, evenly in angle.")});
    add({QStringLiteral("rays/beam"), tr("Release Beam"), QStringLiteral("gop"), QStringLiteral("beam"), Level::None,
         {pair(QStringLiteral("start"), tr("From"), QStringLiteral("length"), QStringLiteral("0"), QStringLiteral("-1")),
          pair(QStringLiteral("end"), tr("To"), QStringLiteral("length"), QStringLiteral("0"), QStringLiteral("1")),
          expr(QStringLiteral("angle"), tr("Direction"), QStringLiteral("angle"), QStringLiteral("0"), tr("Degrees from the x axis")),
          choice(QStringLiteral("profile"), tr("Intensity"), {QStringLiteral("uniform"), QStringLiteral("gaussian")},
                 {tr("Uniform"), tr("Gaussian")}, QStringLiteral("uniform")),
          shownIf(expr(QStringLiteral("waist"), tr("Beam radius w"), QStringLiteral("m"), QStringLiteral("1[mm]"),
                       tr("Its intensity exp(-2r²/w²) about the line's middle")),
                  QStringLiteral("profile=gaussian")),
          raysCount, raysPower, wavelengths, polarization},
         {}, {}, false, false, QStringLiteral("relb"), tr("Parallel rays released along a line: a collimated beam.")});
    add({QStringLiteral("rays/boundary"), tr("Release from Boundary"), QStringLiteral("gop"), QStringLiteral("boundaryfeature"), Level::Boundary,
         {selectionProp(none), raysCount, raysPower, wavelengths, polarization},
         {}, {}, false, false, QStringLiteral("relbd"), tr("Rays released evenly along boundaries, normal to them, into the physics.")});
    add({QStringLiteral("rays/mirror"), tr("Mirror"), QStringLiteral("gop"), QStringLiteral("boundaryfeature"), Level::Boundary,
         {selectionProp(none), expr(QStringLiteral("R"), tr("Reflectance"), QString(), QStringLiteral("1"), tr("The rest absorbed"))},
         {}, {}, false, false, QStringLiteral("mir"), tr("Specular reflection.")});
    add({QStringLiteral("rays/absorber"), tr("Absorber"), QStringLiteral("gop"), QStringLiteral("boundaryfeature"), Level::Boundary,
         {selectionProp(none)}, {}, {}, false, false, QStringLiteral("abs"), tr("A wall that stops the rays.")});
    add({QStringLiteral("rays/coating"), tr("Anti-Reflection Coating"), QStringLiteral("gop"), QStringLiteral("boundaryfeature"), Level::Boundary,
         {selectionProp(none)}, {}, {}, false, false, QStringLiteral("arc"),
         tr("Refraction without reflection (but past the critical angle).")});
    add({QStringLiteral("rays/detector"), tr("Detector"), QStringLiteral("gop"), QStringLiteral("terminal"), Level::Boundary,
         {selectionProp(none), text(QStringLiteral("name"), tr("Detector name"), QStringLiteral("1")),
          flag(QStringLiteral("stop"), tr("Stops the rays"), false)},
         {}, {}, false, false, QStringLiteral("det"),
         tr("Counts the rays that cross it: gop.N_<name>, their power gop.P_<name>, centroid gop.x_<name>, gop.y_<name>, rms "
            "spot gop.rms_<name>, mean optical path gop.opl_<name>.")});

    // Quantum wells.
    add({QStringLiteral("quantum"), tr("Quantum Wells (Schrödinger)"), QStringLiteral("physics"), QStringLiteral("qw"), Level::Domain,
         {selectionProp(),
          choice(QStringLiteral("space"), tr("Solved"), {QStringLiteral("1d"), QStringLiteral("2d")},
                 {tr("1D: along the growth axis, through the layers"), tr("2D: over the cross-section (wires; dots about an axis)")},
                 QStringLiteral("1d"),
                 tr("1D: a line across the layers, quadratic elements of its own, the states free in the plane of the layers; 2D: "
                    "the mesh's, the states free along z")),
          choice(QStringLiteral("axis"), tr("Growth axis"), {QStringLiteral("y"), QStringLiteral("x")}, {tr("y"), tr("x")}, QStringLiteral("y"),
                 tr("The layers stack along it: the holes' masses along it and across it differ, the field F is along it")),
          shownIf(expr(QStringLiteral("at"), tr("Line across the layers at"), QStringLiteral("length"), QString(),
                       tr("x (y, the axis x) where the line runs; empty: the middle of the physics' domains")),
                  QStringLiteral("space=1d")),
          shownIf(expr(QStringLiteral("h1d"), tr("Element size along the line"), QStringLiteral("m"), QStringLiteral("0.1[nm]"),
                       tr("Quadratic elements, their energies' error as h⁴: 0.1 nm finds a 10 nm well's first states to 1 µeV, 0.5 nm "
                          "to some 0.2 meV - fine for stacks hundreds of nm thick. A plain number is metres (0.1[nm])")),
                  QStringLiteral("space=1d")),
          shownIf(choice(QStringLiteral("order"), tr("Element order"), {QStringLiteral("1"), QStringLiteral("2")}, {tr("Linear"), tr("Quadratic")},
                         QStringLiteral("2")),
                  QStringLiteral("space=2d")),
          flag(QStringLiteral("electrons"), tr("Electrons"), true), flag(QStringLiteral("heavy"), tr("Heavy holes"), true),
          flag(QStringLiteral("light"), tr("Light holes"), true),
          expr(QStringLiteral("T"), tr("Temperature"), QStringLiteral("K"), QStringLiteral("300[K]"),
               tr("Of the band gaps (Varshni's) and of the carriers in a spectrum")),
          text(QStringLiteral("substrate"), tr("Substrate (strain)"), QString(),
               tr("The layers take its lattice in their plane (pseudomorphic): a material (Gallium arsenide, Indium phosphide), a "
                  "compound (GaAs) or a lattice constant (0.58697[nm]); empty: no strain")),
          expr(QStringLiteral("F"), tr("Electric field along the axis"), QStringLiteral("V/m"), QStringLiteral("0[V/m]"),
               tr("Uniform: the quantum-confined Stark effect (100 kV/cm is 1e7 V/m); its potential 0 at the physics' middle")),
          spatial(QStringLiteral("V"), tr("Electric potential"), QStringLiteral("V"), QStringLiteral("0"),
                  tr("Another physics' (semi.V, es.V): the bands move by -q V")),
          spatial(QStringLiteral("etan"), tr("Electrons' Fermi level above the band, Efn - Ec"), QStringLiteral("J"), QString(),
                  tr("Where the electrons' quasi-Fermi level is: a semiconductor's semi.Efn - semi.Ec. The states found filled to it give "
                     "qw.n - a Schrödinger-Poisson loop's. Empty: none counted")),
          spatial(QStringLiteral("etap"), tr("Holes' Fermi level below the band, Ev - Efp"), QStringLiteral("J"), QString(),
                  tr("A semiconductor's semi.Ev - semi.Efp: qw.p, the holes in the states found. Empty: none counted")),
          choice(QStringLiteral("valence"), tr("Valence bands"), {QStringLiteral("parabolic"), QStringLiteral("kp4"), QStringLiteral("kp6")},
                 {tr("Parabolic heavy and light holes"), tr("k·p 4x4 (Luttinger-Kohn)"), tr("k·p 6x6 (and the split-off band)")},
                 QStringLiteral("parabolic"),
                 tr("A spectrum's holes (1D wells): each parabolic in the plane, or the k·p bands - heavy and light holes mixed as "
                    "the in-plane k grows (γ1, γ2, γ3; the 6x6 the split-off band too, Δ), solved at each k: the gain's width and "
                    "peak, the TE/TM ratio of strained wells. A Valence Bands step gives their dispersion")),
          choice(QStringLiteral("inplane"), tr("In-plane k"), {QStringLiteral("axial"), QStringLiteral("100"), QStringLiteral("110"), QStringLiteral("average")},
                 {tr("Axial approximation"), tr("Along [100]"), tr("Along [110]"), tr("Averaged over directions")}, QStringLiteral("axial"),
                 tr("The k·p bands' in-plane k: the axial approximation (the plane isotropic, R's γ2 and γ3 their mean - Chuang's, "
                    "Lumerical's), along [100] or [110] taken for every direction, or the warped bands averaged over the plane's "
                    "directions (four, 0 to 45°: four times the work)")),
          expr(QStringLiteral("drawn"), tr("Waves drawn"), QStringLiteral("eV"), QStringLiteral("50[meV]"),
               tr("qw.wave_e and the others: a state's energy with its |ψ|² on it, its largest this - a band diagram's picture"))},
         {QStringLiteral("quantum/material"), QStringLiteral("quantum/zero"), QStringLiteral("quantum/flux")},
         {QStringLiteral("quantum/material"), QStringLiteral("quantum/zero")}, false, false, QStringLiteral("qw"),
         tr("The effective-mass Schrödinger equation of electrons, heavy holes and light holes, BenDaniel and Duke's at each "
            "heterojunction; the bands from the materials' compounds (their alloys' at their composition), strained on a "
            "substrate, tilted by a field or another physics' potential. A Quantum States step finds each carrier's lowest "
            "states, an Optical Spectrum step their absorption, or gain with carriers in the wells.")});
    add({QStringLiteral("quantum/material"), tr("Quantum Well Material"), QStringLiteral("qw"), QStringLiteral("domainfeature"), Level::Domain,
         {selectionProp(), source,
          shownIf(spatial(QStringLiteral("Ec"), tr("Conduction band edge Ec"), QStringLiteral("eV"), QStringLiteral("0[eV]"), tr("Of x and y")),
                  QStringLiteral("source=user")),
          shownIf(spatial(QStringLiteral("Ev"), tr("Valence band edge Ev"), QStringLiteral("eV"), QStringLiteral("-1.5[eV]"),
                          tr("The heavy and light holes' both")),
                  QStringLiteral("source=user")),
          shownIf(expr(QStringLiteral("me"), tr("Electron mass (m0)"), QString(), QStringLiteral("0.067")), QStringLiteral("source=user")),
          shownIf(expr(QStringLiteral("mhh"), tr("Heavy hole mass along the axis (m0)"), QString(), QStringLiteral("0.35")), QStringLiteral("source=user")),
          shownIf(expr(QStringLiteral("mlh"), tr("Light hole mass along the axis (m0)"), QString(), QStringLiteral("0.09")), QStringLiteral("source=user")),
          shownIf(expr(QStringLiteral("mhhp"), tr("Heavy hole mass across the axis (m0)"), QString(), QString(), tr("Empty: as along it")),
                  QStringLiteral("source=user")),
          shownIf(expr(QStringLiteral("mlhp"), tr("Light hole mass across the axis (m0)"), QString(), QString(), tr("Empty: as along it")),
                  QStringLiteral("source=user")),
          shownIf(expr(QStringLiteral("Ep"), tr("Kane energy Ep"), QStringLiteral("eV"), QStringLiteral("25[eV]")), QStringLiteral("source=user")),
          shownIf(expr(QStringLiteral("nr"), tr("Refractive index"), QString(), QStringLiteral("3.5")), QStringLiteral("source=user")),
          shownIf(expr(QStringLiteral("epsilonr"), tr("Static permittivity εr"), QString(), QString(),
                       tr("For the excitons' binding. Empty: nr²")),
                  QStringLiteral("source=user")),
          shownIf(expr(QStringLiteral("gamma3"), tr("Luttinger's γ3"), QString(), QString(),
                       tr("For the k·p valence bands (γ1 and γ2 those the masses along the axis make). Empty: γ2")),
                  QStringLiteral("source=user")),
          shownIf(expr(QStringLiteral("Delta"), tr("Split-off energy Δ"), QStringLiteral("eV"), QString(), tr("For the 6x6 k·p bands")),
                  QStringLiteral("source=user")),
          text(QStringLiteral("valleys"), tr("Electron valleys"), QString(),
               tr("Si(100), Si(110), Si(111) (Δ valleys, ml 0.916, mt 0.190), Ge(100), Ge(110), Ge(111) (L valleys, ml 1.59, mt 0.0815) - "
                  "each followed by ml mt for other masses -, or sets of your own: g mz md [dE eV]; ... Each set a ladder of states "
                  "of its own (mz along the axis, md its density of states in the plane, g its valleys), their carriers summed. "
                  "Empty: one valley, me (a direct gap's)"))},
         {}, {}, false, false, QStringLiteral("qwm"),
         tr("The bands and masses: from the material - its compound (GaAs, InGaAs at x...), or its Eg, VBO (or χ), me, γ1 and γ2 - "
            "or as given.")});
    add({QStringLiteral("quantum/zero"), tr("Zero Wavefunction"), QStringLiteral("qw"), QStringLiteral("boundaryfeature"), Level::Boundary,
         {selectionProp()}, {}, {}, false, false, QStringLiteral("zw"),
         tr("ψ = 0 where the physics ends: hard walls - its outer layers barriers thick enough. In 1D, at both ends of the line. "
            "A state within a few meV of the barriers' top reaches the wall and is raised by it (a 5 nm well's e2, 2 meV below "
            "the top, by 0.1 meV with 30 nm barriers): thicker barriers for those")});
    add({QStringLiteral("quantum/flux"), tr("Zero Flux"), QStringLiteral("qw"), QStringLiteral("boundaryfeature"), Level::Boundary,
         {selectionProp(none)}, {}, {}, false, false, QStringLiteral("zf"),
         tr("∂ψ/∂n = 0 on the boundaries chosen (2D): a wide layer's sides, the states free across them.")});

    // A PDE of the user's own.
    {
        auto spatialPair = [](const QString& key, const QString& label, const QString& tip) {
            P p = pair(key, label, QString(), QStringLiteral("0"), QStringLiteral("0"), tip);
            p.spatial = true;
            return p;
        };
        const QString each = tr("Several variables: one for each, split by ';' (one alone: all alike)");
        add({QStringLiteral("pde"), tr("PDE (Coefficient and General Form)"), QStringLiteral("physics"), QStringLiteral("pde"), Level::Domain,
             {selectionProp(),
              text(QStringLiteral("variables"), tr("Dependent variables"), QStringLiteral("u"),
                   tr("Their names, split by commas (u, v): each with its gradient ux, uy (and the physics' tag before them, c.u, "
                      "c.ux), along a line us")),
              text(QStringLiteral("unit"), tr("Their unit"), QString(), tr("K, V, m... (empty: none)")),
              choice(QStringLiteral("space"), tr("Solved"), {QStringLiteral("2d"), QStringLiteral("1d")},
                     {tr("2D: on the mesh's domains"), tr("1D: along a line across them (an ODE)")}, QStringLiteral("2d"),
                     tr("1D: a line along x (or y) through the domains, quadratic elements of its own; the conditions where it meets "
                        "the boundaries chosen")),
              shownIf(choice(QStringLiteral("axis"), tr("Line along"), {QStringLiteral("x"), QStringLiteral("y")}, {tr("x"), tr("y")}, QStringLiteral("x")),
                      QStringLiteral("space=1d")),
              shownIf(expr(QStringLiteral("at"), tr("Line through"), QStringLiteral("length"), QString(),
                           tr("y (x for a line along y) where it runs; empty: the middle of the physics' domains")),
                      QStringLiteral("space=1d")),
              shownIf(integer(QStringLiteral("elements"), tr("Elements along the line"), 200, tr("Quadratic; each domain it crosses at least one")),
                      QStringLiteral("space=1d")),
              shownIf(choice(QStringLiteral("order"), tr("Element order"), {QStringLiteral("1"), QStringLiteral("2")}, {tr("Linear"), tr("Quadratic")},
                             QStringLiteral("2")),
                      QStringLiteral("space=2d"))},
             {QStringLiteral("pde/coefficient"), QStringLiteral("pde/general"), QStringLiteral("pde/init"), QStringLiteral("pde/dirichlet"),
              QStringLiteral("pde/flux"), QStringLiteral("pde/zeroflux")},
             {QStringLiteral("pde/coefficient"), QStringLiteral("pde/zeroflux"), QStringLiteral("pde/init")}, false, false, QStringLiteral("c"),
             tr("An equation of your own, ea ∂²u/∂t² + da ∂u/∂t + ∇·Γ = F, for one or several variables: in the coefficient form "
                "(Γ = -c ∇u - α u + γ, F = f - β·∇u - a u) or the general one (Γ and F as you write them); its coefficients any "
                "expressions of x, y, t, the variables and their gradients, other physics' fields - nonlinear, by Newton's "
                "method. On the mesh (2D, about an axis too) or along a line (1D: an ODE). Dirichlet, flux (Robin) and zero "
                "flux conditions; at rest, in time or its eigenvalues.")});
        add({QStringLiteral("pde/coefficient"), tr("Coefficient Form PDE"), QStringLiteral("pde"), QStringLiteral("domainfeature"), Level::Domain,
             {selectionProp(),
              spatial(QStringLiteral("c"), tr("Diffusion coefficient c"), QString(), QStringLiteral("1"), each),
              choice(QStringLiteral("diffusion"), tr("Diffusion"), {QStringLiteral("isotropic"), QStringLiteral("anisotropic")},
                     {tr("Isotropic"), tr("Anisotropic (a matrix)")}, QStringLiteral("isotropic")),
              shownIf(spatial(QStringLiteral("cxx"), tr("c, xx"), QString(), QStringLiteral("1")), QStringLiteral("diffusion=anisotropic")),
              shownIf(spatial(QStringLiteral("cxy"), tr("c, xy"), QString(), QStringLiteral("0")), QStringLiteral("diffusion=anisotropic")),
              shownIf(spatial(QStringLiteral("cyx"), tr("c, yx"), QString(), QStringLiteral("0")), QStringLiteral("diffusion=anisotropic")),
              shownIf(spatial(QStringLiteral("cyy"), tr("c, yy"), QString(), QStringLiteral("1")), QStringLiteral("diffusion=anisotropic")),
              spatial(QStringLiteral("a"), tr("Absorption coefficient a"), QString(), QStringLiteral("0"), each),
              spatial(QStringLiteral("f"), tr("Source term f"), QString(), QStringLiteral("1"), each),
              spatial(QStringLiteral("ea"), tr("Mass coefficient ea"), QString(), QStringLiteral("0"), tr("Of ∂²u/∂t²: a wave equation")),
              spatial(QStringLiteral("da"), tr("Damping or mass coefficient da"), QString(), QStringLiteral("1"),
                      tr("Of ∂u/∂t: a diffusion's; an eigenproblem's mass")),
              spatialPair(QStringLiteral("alpha"), tr("Conservative flux convection α"), each),
              spatialPair(QStringLiteral("beta"), tr("Convection coefficient β"), each),
              spatialPair(QStringLiteral("gamma"), tr("Conservative flux source γ"), each)},
             {}, {}, false, false, QStringLiteral("cfeq"),
             tr("ea ∂²u/∂t² + da ∂u/∂t + ∇·(-c ∇u - α u + γ) + β·∇u + a u = f. Each coefficient may read x, y, t, the variables "
                "and their gradients: nonlinear.")});
        add({QStringLiteral("pde/general"), tr("General Form PDE"), QStringLiteral("pde"), QStringLiteral("domainfeature"), Level::Domain,
             {selectionProp(),
              spatial(QStringLiteral("Gx"), tr("Conservative flux Γ, x"), QString(), QStringLiteral("-ux"), each),
              spatial(QStringLiteral("Gy"), tr("Conservative flux Γ, y"), QString(), QStringLiteral("-uy"), each),
              spatial(QStringLiteral("F"), tr("Source term F"), QString(), QStringLiteral("1"), each),
              spatial(QStringLiteral("ea"), tr("Mass coefficient ea"), QString(), QStringLiteral("0")),
              spatial(QStringLiteral("da"), tr("Damping or mass coefficient da"), QString(), QStringLiteral("1"))},
             {}, {}, false, false, QStringLiteral("gfeq"),
             tr("ea ∂²u/∂t² + da ∂u/∂t + ∇·Γ = F, Γ and F as you write them (along a line, Γ's component along it).")});
        add({QStringLiteral("pde/init"), tr("Initial Values"), QStringLiteral("pde"), QStringLiteral("domainfeature"), Level::Domain,
             {selectionProp(), spatial(QStringLiteral("u0"), tr("Initial value"), QString(), QStringLiteral("0"), each),
              spatial(QStringLiteral("ut0"), tr("Initial time derivative"), QString(), QStringLiteral("0"), each)},
             {}, {}, false, false, QStringLiteral("init"), tr("Where a step in time starts, and Newton's method's first guess.")});
        add({QStringLiteral("pde/dirichlet"), tr("Dirichlet Boundary Condition"), QStringLiteral("pde"), QStringLiteral("boundaryfeature"),
             Level::Boundary,
             {selectionProp(none), spatial(QStringLiteral("r"), tr("Prescribed value r"), QString(), QStringLiteral("0"),
                                           tr("u = r; several variables: one for each split by ';', an item left empty that variable free")),
              text(QStringLiteral("which"), tr("Variables constrained"), QString(), tr("Their names (empty: each that has an r)"))},
             {}, {}, false, false, QStringLiteral("dir"), tr("u = r on its boundaries (along a line: where the line meets them).")});
        add({QStringLiteral("pde/flux"), tr("Flux/Source"), QStringLiteral("pde"), QStringLiteral("boundaryfeature"), Level::Boundary,
             {selectionProp(none), spatial(QStringLiteral("g"), tr("Boundary flux or source g"), QString(), QStringLiteral("0"), each),
              spatial(QStringLiteral("q"), tr("Boundary absorption q"), QString(), QStringLiteral("0"), each)},
             {}, {}, false, false, QStringLiteral("flux"),
             tr("-n·Γ = g - q u: a flux in, or (q) a Robin condition; g and q may read the variables.")});
        add({QStringLiteral("pde/zeroflux"), tr("Zero Flux"), QStringLiteral("pde"), QStringLiteral("boundaryfeature"), Level::Boundary,
             {selectionProp()}, {}, {}, false, false, QStringLiteral("zfl"), tr("-n·Γ = 0: where no other condition is.")});
    }

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
         {QStringLiteral("stationary"), QStringLiteral("transient"), QStringLiteral("frequency"), QStringLiteral("mode"), QStringLiteral("states"),
          QStringLiteral("spectrum"), QStringLiteral("bands"), QStringLiteral("eigenvalue"), QStringLiteral("sweep")},
         {QStringLiteral("stationary")}, false, false,
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
        // Adaptive mesh refinement: at rest, or by the last frequency.
        auto adaptive = [&](const QString& tooltip) { return flag(QStringLiteral("adaptive"), tr("Adaptive mesh refinement"), false, tooltip); };
        const P adaptSteps = shownIf(integer(QStringLiteral("adaptsteps"), tr("Refinements"), 3), QStringLiteral("adaptive=true"));
        P adaptPhysics;
        adaptPhysics.key = QStringLiteral("adaptphysics");
        adaptPhysics.label = tr("Refined by the error of");
        adaptPhysics.kind = P::Physics;
        adaptPhysics.defaultValue = QJsonArray();
        adaptPhysics.tooltip = tr("None chosen: the first physics solved");
        adaptPhysics.showIf = QStringLiteral("adaptive=true");
        // What a step solves before its modes, states or eigenvalues: its
        // physics at rest, by Newton's method.
        const P before = integer(QStringLiteral("maxiter"), tr("Maximum iterations"), 50,
                                 tr("Of the physics at rest it solves first (Newton's method, and in turn between them)"));
        add({QStringLiteral("stationary"), tr("Stationary"), QStringLiteral("step"), QStringLiteral("stationary"), Level::None,
             {physics, solver, nonlinear,
              flag(QStringLiteral("sweep"), tr("Terminal and coil sweep (capacitance, conductance, inductance matrices)"), true),
              expr(QStringLiteral("tolerance"), tr("Relative tolerance"), QString(), QStringLiteral("1e-6"),
                   tr("Of the nonlinear iterations, and of an iterative solver")),
              integer(QStringLiteral("maxiter"), tr("Maximum iterations"), 50,
                      tr("Of the nonlinear iterations: a material that depends on a field")),
              adaptive(tr("Solved, the mesh made finer where the error is largest, solved again: the study's last solution on the finest")),
              adaptSteps, adaptPhysics},
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
             tr("The fields in time, from the initial values (a study's step before: its solution): heat with its ρ Cp ∂T/∂t, "
                "semiconductors' carriers with ∂n/∂t and ∂p/∂t - a contact's current with its displacement current, contacts' "
                "voltages and Optical Generation's power of t (switching, a diode's reverse recovery, a photodiode's pulse "
                "response) -, PDEs with their mass; the other physics at rest at each step. Kept at the output times.")});
        add({QStringLiteral("frequency"), tr("Frequency Domain"), QStringLiteral("step"), QStringLiteral("frequency"), Level::None,
             {physics,
              text(QStringLiteral("frequencies"), tr("Frequencies"), QStringLiteral("logspace(1[kHz], 1[GHz], 31)"),
                   tr("In Hz, or with a unit: logspace(start, stop, count) - evenly in the logarithm -, range(start, step, stop), "
                      "linspace(start, stop, count) or values split by commas")),
              flag(QStringLiteral("sweep"), tr("Coil sweep (impedance matrix)"), true), solver, nonlinear,
              expr(QStringLiteral("tolerance"), tr("Relative tolerance"), QString(), QStringLiteral("1e-6"),
                   tr("Of the physics solved in turn at each frequency")),
              integer(QStringLiteral("maxiter"), tr("Maximum iterations"), 50),
              adaptive(tr("Solved at each frequency, the mesh made finer where the last frequency's error is largest, all solved "
                          "again")),
              adaptSteps, adaptPhysics},
             {}, {}, false, false, QStringLiteral("freq"),
             tr("The fields at each frequency: magnetic fields with their eddy currents, waves in the plane (complex: their "
                "real and imaginary parts), the other physics at rest at each; kept at each frequency.")});
        add({QStringLiteral("mode"), tr("Mode Analysis"), QStringLiteral("step"), QStringLiteral("mode"), Level::None,
             {physics,
              text(QStringLiteral("frequency"), tr("Frequency or wavelength"), QStringLiteral("1550[nm]"),
                   tr("In Hz, or a free-space wavelength in m (1550[nm]); several as range(...), linspace(...), logspace(...) or "
                      "values split by commas - a dispersion")),
              integer(QStringLiteral("modes"), tr("Modes"), 4, tr("How many, nearest the effective index below")),
              expr(QStringLiteral("neff"), tr("Search about the effective index"), QString(), QString(),
                   tr("Empty: the largest refractive index of the model")),
              flag(QStringLiteral("follow"), tr("Follow the mode"), true,
                   tr("One mode asked for: each frequency after the first, and each point of a sweep, searches about the mode "
                      "found before, not the index above - a wavelength sweep stays on its branch rather than taking whichever "
                      "mode is nearest the index. (Several: the set nearest the index at each, or those most in the domains kept.)")),
              [&] {
                  P track = selectionProp(QJsonObject{{QStringLiteral("numbers"), QJsonArray()}}, Level::Domain);
                  track.key = QStringLiteral("track");
                  track.label = tr("Keep the modes most in");
                  track.tooltip = tr("Domains: of the modes found about the index (more than asked), those with most of their |E|² "
                                     "there - the core's, not a contact's or a substrate's; their share emw.Gamma. None: the nearest the index");
                  return track;
              }(),
              choice(QStringLiteral("polarization"), tr("Polarization"), {QStringLiteral("any"), QStringLiteral("te"), QStringLiteral("tm")},
                     {tr("Any"), tr("TE (E along x)"), tr("TM (E along y)")}, QStringLiteral("any"),
                     tr("Those of one polarization alone - by emw.TEfraction, of the transverse |E|² the part along x: TE at 1/2 "
                        "or more. A metal or magnetic wall's lateral modes (a 2 µm strip's TE01 above a slab's TM0) left out")),
              expr(QStringLiteral("tolerance"), tr("Relative tolerance"), QString(), QStringLiteral("1e-6"),
                   tr("Of the physics solved before at each frequency")),
              before, nonlinear},
             {}, {}, false, false, QStringLiteral("mode"),
             tr("A cross-section's modes at each frequency: each a solution, as an output time is - its effective index, "
                "loss, fields. Physics at rest in it (heat for a thermo-optic index) are solved first.")});
        add({QStringLiteral("eigenvalue"), tr("Eigenvalue"), QStringLiteral("step"), QStringLiteral("eigenvalue"), Level::None,
             {physics, integer(QStringLiteral("count"), tr("Eigenvalues"), 6, tr("Those nearest the shift")),
              expr(QStringLiteral("shift"), tr("Search about"), QString(), QStringLiteral("0")),
              expr(QStringLiteral("tolerance"), tr("Relative tolerance"), QString(), QStringLiteral("1e-6"),
                   tr("Of the physics solved before")),
              before, nonlinear},
             {}, {}, false, false, QStringLiteral("eig"),
             tr("A PDE's eigenvalues λ - -λ da u + ∇·Γ = F linearized about where it is, its Dirichlet conditions 0 - and their "
                "eigenfunctions: each a solution, as a mode is.")});
        add({QStringLiteral("states"), tr("Quantum States"), QStringLiteral("step"), QStringLiteral("states"), Level::None,
             {physics, integer(QStringLiteral("states"), tr("States of each carrier"), 4, tr("The lowest: electrons upward, holes downward")),
              expr(QStringLiteral("tolerance"), tr("Relative tolerance"), QString(), QStringLiteral("1e-6"),
                   tr("Of the physics solved before (a semiconductor's potential)")),
              flag(QStringLiteral("consistent"), tr("Self-consistent (Schrödinger-Poisson)"), false,
                   tr("The semiconductor and the states in turn until its Schrödinger-Poisson regions' carriers are the states'")),
              shownIf(integer(QStringLiteral("iterations"), tr("Most turns"), 60), QStringLiteral("consistent=true")),
              shownIf(expr(QStringLiteral("sptolerance"), tr("Its quantum potential's last change (Vt)"), QString(), QStringLiteral("1e-4")),
                      QStringLiteral("consistent=true")),
              before, nonlinear},
             {}, {}, false, false, QStringLiteral("qs"),
             tr("A quantum wells physics' states: each a solution, as a mode is - each carrier's energy and wavefunction; the "
                "transitions' energies and overlaps. Physics at rest in it (a semiconductor's potential) are solved first.")});
        add({QStringLiteral("bands"), tr("Valence Bands"), QStringLiteral("step"), QStringLiteral("spectrum"), Level::None,
             {physics,
              expr(QStringLiteral("kmax"), tr("Largest in-plane k"), QStringLiteral("1/m"), QStringLiteral("1[1/nm]"),
                   tr("The wave vector in the wells' plane, from 0 (along x: the in-plane warping left out)")),
              integer(QStringLiteral("points"), tr("Values of k"), 41),
              integer(QStringLiteral("states"), tr("States"), 4, tr("The highest valence states (Kramers' pairs both: twice as many)")),
              expr(QStringLiteral("tolerance"), tr("Relative tolerance"), QString(), QStringLiteral("1e-6")), before, nonlinear},
             {}, {}, false, false, QStringLiteral("bnd"),
             tr("A quantum wells physics' k·p valence bands (its Valence bands 4x4 or 6x6) along the wave vector in the wells' "
                "plane: each value of k a solution, qw.k, the states' energies qw.Ev1, qw.Ev2... - the subbands' dispersion, the "
                "heavy and light holes mixed away from k = 0.")});
        add({QStringLiteral("spectrum"), tr("Optical Spectrum"), QStringLiteral("step"), QStringLiteral("spectrum"), Level::None,
             {physics,
              text(QStringLiteral("energies"), tr("Photon energies or wavelengths"), QStringLiteral("linspace(1.4[eV], 1.8[eV], 201)"),
                   tr("In eV, or free-space wavelengths in m (range(1.5[um], 1[nm], 1.6[um])), or Hz: linspace, range or values split "
                      "by commas")),
              integer(QStringLiteral("states"), tr("States of each carrier"), 6,
                      tr("How many states of each carrier are solved and enter the spectrum (with k·p valence bands, twice as many "
                         "of those: Kramers' pairs). With a Lorentzian line each higher state's tail reaches under the gain's peak, "
                         "which then depends on it - a 10 nm GaAs well's TE peak 394 /cm with 2, 304 with 4 (Lumerical's MQW keeps 2 "
                         "conduction and 4 valence subbands: 2 here); the log warns when transitions more than 5 half widths above the "
                         "peak take more than 10 % off it. A Gaussian or a sech line hardly depends on it")),
              integer(QStringLiteral("kpoints"), tr("k points (k·p)"), 121,
                      tr("The k·p valence bands' in-plane k from 0 past the highest photon and the holes' reach, at this many values: "
                         "fewer, quicker - each interval's occupations and the line's tail are integrated within it")),
              expr(QStringLiteral("linewidth"), tr("Broadening"), QStringLiteral("eV"), QStringLiteral("6.6[meV]"),
                   tr("The line's half width at half maximum: a Lorentzian's ħ/τ, 6.6 meV for 0.1 ps")),
              expr(QStringLiteral("dlinewidth"), tr("Broadening's growth with the field"), QString(), QString(),
                   tr("Added to the broadening per field across the wells (qw.Fw, their band edge's slope): 1[meV]/(1e7[V/m]) - "
                      "an electroabsorption edge broadens as the field pulls the states apart. Empty: none")),
              choice(QStringLiteral("continuum"), tr("Continuum"), {QStringLiteral("states"), QStringLiteral("bound")},
                     {tr("The states found (a box's)"), tr("Bound states', and the bulk absorption")}, QStringLiteral("states"),
                     tr("The states found above the barriers are a box's - the line's ends hard walls -, moving with a field: a "
                        "Kramers-Kronig index from them moves too. Bound: the transitions of states confined by their own well's "
                        "barriers alone (in a field too: below its barriers' edges, not the line's far ends'), their 2D steps fading "
                        "above the barriers' gap into the wells' own 3D absorption, the barriers' 3D absorption over their thickness "
                        "(per the wells' thickness, as the states' was): far enough above, the absorption the bulk's, the same in any "
                        "field - Δn between fields converges as the window widens")),
              shownIf(expr(QStringLiteral("fade"), tr("Fade above the barriers' gap"), QStringLiteral("eV"), QStringLiteral("0.1[eV]"),
                           tr("Over this above the barriers' gap the bound states' 2D steps give way to the wells' 3D absorption (cos²)")),
                      QStringLiteral("continuum=bound")),
              choice(QStringLiteral("lineshape"), tr("Line shape"), {QStringLiteral("lorentzian"), QStringLiteral("gaussian"), QStringLiteral("sech")},
                     {tr("Lorentzian"), tr("Gaussian"), tr("Hyperbolic secant")}, QStringLiteral("lorentzian"),
                     tr("Each transition's broadening. With carriers a Lorentzian's slow tails move the gain's zero below the "
                        "quasi-Fermi levels' separation (some 10 meV at 6.6 meV) and part the emission from the gain, and the gain's peak "
                        "depends on how many states the step takes: for gain spectra a Gaussian or a sech. A Lorentzian's tails absorb far below the edge (an electroabsorption "
                        "modulator's loss at 0 V); a Gaussian's (inhomogeneous: the wells' widths and alloy) and a sech's do not")),
              flag(QStringLiteral("excitons"), tr("Excitons"), false,
                   tr("Each transition's 1s exciton - its binding energy variational over the states' own envelopes (a field "
                      "pulling them apart weakens it: the quantum-confined Stark effect's), its line 2 |F(0)|² strong (both spin "
                      "pairs) - and the continuum above it raised by the 2D Sommerfeld factor (2 at the edge). Absorption only: with "
                      "carriers they are screened, left out")),
              shownIf(choice(QStringLiteral("excitonmodel"), tr("Exciton"), {QStringLiteral("two"), QStringLiteral("bastard")},
                             {tr("Two parameters (λ, γ)"), tr("Bastard's (λ)")}, QStringLiteral("two"),
                             tr("Its trial function: e^(-√(ρ² + γ²(ze - zh)²)/λ), the electron and hole drawn together along the axis "
                                "too (Gerlach et al.'s; Lumerical's variational one) - within 2 % of the exact binding and its line "
                                "near the exact's in wells 2 to 20 nm wide -, or Bastard's e^(-ρ/λ) (γ 0: a narrow well's; |F(0)|² = "
                                "2/(πλ²)), its line weaker in wide wells")),
                      QStringLiteral("excitons=true")),
              shownIf(expr(QStringLiteral("epsr"), tr("Excitons' permittivity εr"), QString(), QString(),
                           tr("Static. Empty: the wells' material's (its epsilonr, else its compound's; a Quantum Well Material's "
                              "own, else nr²)")),
                      QStringLiteral("excitons=true")),
              expr(QStringLiteral("density"), tr("Carriers in the wells"), QStringLiteral("1/m^3"), QStringLiteral("0[1/cm^3]"),
                   tr("Electrons and holes alike, per volume of the wells (the domains of the smallest gap): gain where their "
                      "quasi-Fermi levels are far enough apart; 0, the wells' absorption")),
              expr(QStringLiteral("nr"), tr("Refractive index"), QString(), QString(), tr("Empty: the wells' material's")),
              expr(QStringLiteral("tolerance"), tr("Relative tolerance"), QString(), QStringLiteral("1e-6")), before, nonlinear},
             {}, {}, false, false, QStringLiteral("spec"),
             tr("A quantum wells physics' absorption coefficient - TE (in the layers' plane) and TM - or gain over photon "
                "energies: its states' transitions, k conserved, each broadened; each point a solution, as a frequency is. Each "
                "transition's reduced mass is of its two states' own masses in the plane - each the inverse mass averaged over "
                "its |ψ|² across the layers, not the well's alone (a state reaching into the barriers is heavier there).")});
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
         {QStringLiteral("derived"), QStringLiteral("exports")}, true, true, QString(), QString()});
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
        P frequency;
        frequency.key = QStringLiteral("frequency");
        frequency.label = tr("Frequency");
        frequency.kind = P::Instant;
        frequency.unit = QStringLiteral("Hz");
        frequency.defaultValue = QString();
        frequency.tooltip = tr("Of a study in the frequency domain: the frequency shown, or its wavelength (empty: the last)");
        add({QStringLiteral("plotgroup"), tr("2D Plot Group"), QStringLiteral("results"), QStringLiteral("plotgroup"), Level::None,
             {study, point, time, frequency,
              integer(QStringLiteral("mode"), tr("Mode"), 1, tr("Of a mode analysis: which mode, from 1 (by effective index)")),
              text(QStringLiteral("title"), tr("Title"), QString())},
             {QStringLiteral("surface"), QStringLiteral("contour"), QStringLiteral("arrow"), QStringLiteral("meshplot"), QStringLiteral("deformation"),
              QStringLiteral("rayplot")},
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
             {study, point, time, frequency, text(QStringLiteral("title"), tr("Title"), QString()),
              choice(QStringLiteral("diagram"), tr("Diagram"), {QStringLiteral("rect"), QStringLiteral("tab")},
                     {tr("Cartesian"), tr("Table")}, QStringLiteral("rect")),
              // (A global's or a point's graph: what is along x - automatic: a
              // sweep's parameter (of two of several values the last listed, a
              // curve per the other's) before a frequency step's frequencies, the
              // modes last; a transient's times, a spectrum before the sweep.)
              choice(QStringLiteral("against"), tr("Plot against"), {QStringLiteral("auto"), QStringLiteral("sweep"), QStringLiteral("steps")},
                     {tr("Automatic"), tr("The sweep's parameters"), tr("The times, frequencies or modes")}, QStringLiteral("auto"))},
             {QStringLiteral("linegraph"), QStringLiteral("globalgraph"), QStringLiteral("pointgraph")}, {}, false, false,
             QStringLiteral("pg1d"),
             tr("Graphs of the solution - along a cut line, at points or of global values over time or a sweep - written as a "
                "Qucs dataset (model_tag.dat beside the model) and shown in a data display, with its markers and exports.")});
        const P unit1 = text(QStringLiteral("unit"), tr("Unit"), QString(),
                             tr("Shown in this unit: mV, degC, kV/mm, dB/cm for emw.loss... (empty: SI, or a global's own: dB/m)"));
        add({QStringLiteral("linegraph"), tr("Line Graph"), QStringLiteral("graph"), QStringLiteral("linegraph"), Level::None,
             {dataset, spatial(QStringLiteral("expression"), tr("Expression"), QString(), QStringLiteral("V")), unit1,
              choice(QStringLiteral("xaxis"), tr("x-axis"), {QStringLiteral("arc"), QStringLiteral("x"), QStringLiteral("y")},
                     {tr("The distance along the line"), tr("x"), tr("y")}, QStringLiteral("arc")),
              solutions},
             {}, {}, false, false, QStringLiteral("lngr"), tr("An expression along a cut line: a curve for each solution.")});
        add({QStringLiteral("globalgraph"), tr("Global"), QStringLiteral("graph"), QStringLiteral("globalgraph"), Level::None,
             {text(QStringLiteral("expression"), tr("Expressions"), QStringLiteral("ht.Tmax"),
                   tr("Split by ';': ht.Tmax; ec.P... - or a Derived Values node's tag: lp1 (each of its values), lp1.Z0 (one); each named "
                      "if wanted: loss_dBcm = emw.loss/100")),
              unit1},
             {}, {}, false, false, QStringLiteral("glbg"),
             tr("Global values - or a Derived Values node's - over the output times, or the sweep's parameters.")});
        add({QStringLiteral("pointgraph"), tr("Point Graph"), QStringLiteral("graph"), QStringLiteral("pointgraph"), Level::None,
             {dataset, spatial(QStringLiteral("expression"), tr("Expression"), QString(), QStringLiteral("T")), unit1},
             {}, {}, false, false, QStringLiteral("ptgr"), tr("An expression at a cut point's points over the output times, or the sweep's parameters.")});
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
        add({QStringLiteral("rayplot"), tr("Ray Trajectories"), QStringLiteral("plot"), QStringLiteral("rayplot"), Level::None,
             {text(QStringLiteral("physics"), tr("Of the physics"), QString(), tr("Its tag (empty: the first geometrical optics)")),
              choice(QStringLiteral("by"), tr("Color by"),
                     {QStringLiteral("wavelength"), QStringLiteral("power"), QStringLiteral("release"), QStringLiteral("single")},
                     {tr("Wavelength (its color)"), tr("Power"), tr("Release"), tr("One color")}, QStringLiteral("wavelength")),
              shownIf(choice(QStringLiteral("colors"), tr("Color table"), tables, tableLabels, QStringLiteral("rainbow")),
                      QStringLiteral("by=power|by=release")),
              shownIf(text(QStringLiteral("color"), tr("Color"), QStringLiteral("red")), QStringLiteral("by=single"))},
             {}, {}, false, false, QStringLiteral("ray"), tr("The rays' paths, as traced.")});
        add({QStringLiteral("derived"), tr("Derived Values"), QString(), QStringLiteral("derived"), Level::None, {},
             {QStringLiteral("global"), QStringLiteral("integral"), QStringLiteral("average"), QStringLiteral("maximum"),
              QStringLiteral("minimum"), QStringLiteral("pointeval"), QStringLiteral("lineparams"), QStringLiteral("confinement")},
             {}, true, true, QString(), tr("Numbers of the solution: integrals, averages, a capacitance, a resistance.")});
        add({QStringLiteral("global"), tr("Global Evaluation"), QStringLiteral("derived"), QStringLiteral("global"), Level::None,
             {study, text(QStringLiteral("expression"), tr("Expressions"), QStringLiteral("es.C11"),
                          tr("Split by ';': es.C11; ec.R; ht.Rth... - each named if wanted: C_pF = es.C11/1[pF]")),
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
        {
            P modes;
            modes.key = QStringLiteral("physics");
            modes.label = tr("Mode analysis");
            modes.kind = P::Physics;
            modes.defaultValue = QJsonArray{QStringLiteral("emw")};
            add({QStringLiteral("confinement"), tr("Confinement Factor"), QStringLiteral("derived"), QStringLiteral("confinement"), Level::Domain,
                 {study, selectionProp(), modes,
                  choice(QStringLiteral("kind"), tr("Of"), {QStringLiteral("e2"), QStringLiteral("power"), QStringLiteral("index")},
                         {tr("|E|²: its share in the domains"), tr("Power: Sz's share"), tr("Index: ∂neff/∂n there (modal gain's)")},
                         QStringLiteral("e2"),
                         tr("|E|²: ∫ |E|² there over ∫ |E|² - the usual Γ. Power: ∫ Sz there over ∫ Sz. Index: (c ε0/2) ∫ n |E|² "
                            "there over ∫ Sz - how much neff moves for n moved there, a modal gain's Γ (above 1 in high contrast: "
                            "Visser et al.); a Δn there moves neff by Γ Δn, to the first order")),
                  solutions},
                 {}, {}, false, false, QStringLiteral("conf"),
                 tr("A mode's share of the domains chosen, for each mode solved: Γ without integrating cut lines (a TM mode's "
                    "normal E jumps at an index step: a line samples one side of it).")});
        }
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
        P magnetic;
        magnetic.key = QStringLiteral("magnetic");
        magnetic.label = tr("Magnetic fields");
        magnetic.kind = P::Physics;
        magnetic.defaultValue = QJsonArray{QStringLiteral("mf")};
        magnetic.showIf = QStringLiteral("method=rlgc");
        add({QStringLiteral("lineparams"), tr("Line Parameters"), QStringLiteral("derived"), QStringLiteral("lineparams"),
             Level::None,
             {study,
              choice(QStringLiteral("method"), tr("Method"), {QStringLiteral("quasitem"), QStringLiteral("rlgc")},
                     {tr("Quasi-TEM, from electrostatics alone"), tr("R, L, G, C over frequency: electrostatics and magnetic fields")},
                     QStringLiteral("quasitem")),
              physics, terminal, magnetic,
              shownIf(text(QStringLiteral("conductor"), tr("Signal conductor (coil)"), QStringLiteral("1"),
                           tr("The magnetic fields' coil that carries the line's current")),
                      QStringLiteral("method=rlgc")),
              shownIf(text(QStringLiteral("return"), tr("Return conductor (coil)"), QString(),
                           tr("The coil the current comes back in (empty: the boundaries at A = 0, a perfect ground)")),
                      QStringLiteral("method=rlgc")),
              solutions},
             {}, {}, false, false, QStringLiteral("lp"),
             tr("A transmission line's cross-section, per metre. Quasi-TEM: its capacitance C and, with every εr 1, C0 - so "
                "εeff = C/C0, Z0 = 1/(c0 √(C C0)) and L = 1/(c0² C0). R, L, G, C: C from electrostatics, G = ω C tan δ "
                "(+ σ) from its dielectrics' losses, R and L from the magnetic fields' coils in the frequency domain (the "
                "skin effect, internal inductance) - Z0, the attenuation α and β at each frequency.")});

        // Exports.
        add({QStringLiteral("exports"), tr("Export"), QString(), QStringLiteral("exports"), Level::None, {},
             {QStringLiteral("touchstone"), QStringLiteral("spice"), QStringLiteral("qucspart")}, {}, true, true, QString(),
             tr("Results as files and parts of circuits: a line's S-parameters (Touchstone), SPICE subcircuits, a Qucs "
                "subcircuit - each written beside the model (model_tag.s2p, .cir, .sch).")});
        P line;
        line.key = QStringLiteral("line");
        line.label = tr("Line");
        line.kind = P::Reference;
        line.choices = {QStringLiteral("lineparams")};
        line.defaultValue = QStringLiteral("lp1");
        line.tooltip = tr("A Line Parameters node of method \"R, L, G, C over frequency\"");
        const P length = expr(QStringLiteral("length"), tr("Line length"), QStringLiteral("m"), QStringLiteral("10[mm]"));
        P file;
        file.key = QStringLiteral("file");
        file.label = tr("File");
        file.kind = P::File;
        file.defaultValue = QString();
        file.tooltip = tr("Where it is written, relative to the model's folder (empty: model_tag beside it)");
        const P name = text(QStringLiteral("name"), tr("Subcircuit name"), QString(), tr("Empty: the node's tag"));
        P portsPhysics;
        portsPhysics.key = QStringLiteral("physics");
        portsPhysics.label = tr("Physics");
        portsPhysics.kind = P::Physics;
        portsPhysics.defaultValue = QJsonArray{QStringLiteral("ewfd")};
        portsPhysics.showIf = QStringLiteral("of=ports");
        add({QStringLiteral("touchstone"), tr("Touchstone File"), QStringLiteral("export"), QStringLiteral("touchstone"), Level::None,
             {study, point,
              choice(QStringLiteral("of"), tr("Of"), {QStringLiteral("line"), QStringLiteral("ports")},
                     {tr("A transmission line of a length"), tr("Waves' ports (their S-matrix)")}, QStringLiteral("line")),
              shownIf(line, QStringLiteral("of=line")), shownIf(length, QStringLiteral("of=line")),
              shownIf(expr(QStringLiteral("zref"), tr("Reference impedance"), QStringLiteral("ohm"), QStringLiteral("50[ohm]")), QStringLiteral("of=line")),
              portsPhysics, file},
             {}, {}, false, false, QStringLiteral("ts"),
             tr("A line of its length as a two-port: its S-parameters at each frequency of the study, a Touchstone file (.s2p) "
                "for a schematic's S-parameter file part.")});
        P physicsMatrix;
        physicsMatrix.key = QStringLiteral("physics");
        physicsMatrix.label = tr("Physics");
        physicsMatrix.kind = P::Physics;
        physicsMatrix.defaultValue = QJsonArray{QStringLiteral("es")};
        physicsMatrix.showIf = QStringLiteral("what=matrix");
        add({QStringLiteral("spice"), tr("SPICE Subcircuit"), QStringLiteral("export"), QStringLiteral("spice"), Level::None,
             {study, point, frequency,
              choice(QStringLiteral("what"), tr("Of"), {QStringLiteral("line"), QStringLiteral("matrix")},
                     {tr("A transmission line"), tr("Terminals' matrix: capacitances, conductances, inductances")}, QStringLiteral("line")),
              shownIf(line, QStringLiteral("what=line")), shownIf(length, QStringLiteral("what=line")),
              shownIf(choice(QStringLiteral("model"), tr("Line model"), {QStringLiteral("ladder"), QStringLiteral("ltra")},
                             {tr("Sections of R, L, G, C"), tr("A lossy line (LTRA)")}, QStringLiteral("ladder")),
                      QStringLiteral("what=line")),
              shownIf(integer(QStringLiteral("sections"), tr("Sections"), 20, tr("Enough that each is short against a wavelength")),
                      QStringLiteral("what=line&model=ladder")),
              physicsMatrix, name, file},
             {}, {}, false, false, QStringLiteral("sp"),
             tr("A SPICE subcircuit (.cir): a line as sections of R, L, G, C or an LTRA line at a frequency, or a physics' "
                "terminals as capacitors (es), resistors (ec) or coupled inductors (mf).")});
        add({QStringLiteral("qucspart"), tr("Qucs Subcircuit (RLCG)"), QStringLiteral("export"), QStringLiteral("qucspart"), Level::None,
             {study, point, frequency, line, length, name, file},
             {}, {}, false, false, QStringLiteral("qp"),
             tr("A Qucs subcircuit (.sch): an RLCG part of the line's values at a frequency and its length between ports 1 "
                "and 2 - a part for a schematic (Subcircuit), or copied to paste.")});
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
    return {QStringLiteral("electrostatics"), QStringLiteral("currents"), QStringLiteral("heat"), QStringLiteral("solid"),
            QStringLiteral("magnetic"), QStringLiteral("modes"), QStringLiteral("waves"), QStringLiteral("semiconductor"), QStringLiteral("rays"),
            QStringLiteral("quantum"), QStringLiteral("pde")};
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
        // Study 1, 2D Plot Group 2: numbered.
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
    // (Results: before Derived Values and Export.)
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

void Model::normalise(QStringList* notes)
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
        // Derived Values, then Export, last of them.
        for (const QString& type : {QStringLiteral("derived"), QStringLiteral("exports")}) {
            auto d = std::find_if(r.children.begin(), r.children.end(), [&](const Node& n) { return n.type == type; });
            Node fixed;
            if (d != r.children.end()) {
                fixed = *d;
                r.children.erase(d);
            } else {
                fixed.type = type;
                fixed.tag = type;
            }
            r.children.push_back(fixed);
        }
    }
    // Every node a tag, each its own.
    QSet<QString> tags;
    std::function<void(Node&)> retag = [&](Node& n) {
        if (n.tag.isEmpty() || tags.contains(n.tag)) {
            const NodeKind* k = n.kind();
            const QString prefix = k && !k->prefix.isEmpty() ? k->prefix : QStringLiteral("n");
            QString t = prefix + QLatin1Char('1');
            for (int i = 1; tags.contains(t); ++i) t = prefix + QString::number(i);
            // (A tag of the file's taken by a node before it - a plot
            // group's the same as a study's: said, with the one it has now.)
            if (notes && !n.tag.isEmpty())
                *notes << tr("%1's tag %2 is another node's already: it is %3 here").arg(n.label.isEmpty() ? n.type : n.label, n.tag, t);
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
    Node exports;
    exports.type = QStringLiteral("exports");
    exports.tag = QStringLiteral("exports");
    for (const QJsonValue& e : ro.value(QStringLiteral("exports")).toArray())
        if (std::optional<Node> n = nodeFrom(QStringLiteral("exports"), e, problems)) exports.children.push_back(*n);
    results.children.push_back(exports);
    m.root.children.push_back(results);

    m.normalise(&problems);
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
    QJsonArray plots, derived, exports;
    for (const Node& p : results().children) {
        if (p.type == QLatin1String("derived")) {
            for (const Node& d : p.children) derived.append(nodeObject(p, d));
        } else if (p.type == QLatin1String("exports")) {
            for (const Node& e : p.children) exports.append(nodeObject(p, e));
        } else {
            plots.append(nodeObject(results(), p, QStringLiteral("plots")));
        }
    }
    r.insert(QStringLiteral("plots"), plots);
    r.insert(QStringLiteral("derived"), derived);
    if (!exports.isEmpty()) r.insert(QStringLiteral("exports"), exports);
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
