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
#include <map>
#include <set>
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

/// A Choice that takes a value in its place too.
P orValue(P p)
{
    p.orValue = true;
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
        P grid;
        grid.key = QStringLiteral("grid");
        grid.label = tr("Table (x, y, f(x, y))");
        grid.kind = P::Rows;
        grid.choiceLabels = {QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("f")};
        grid.defaultValue = QJsonArray();
        grid.tooltip = tr("Every point of a rectilinear grid: each x with each y");
        add({QStringLiteral("interpolation"), tr("Interpolation"), QStringLiteral("function"), QStringLiteral("function"), Level::None,
             {text(QStringLiteral("name"), tr("Function name"), QStringLiteral("int")),
              choice(QStringLiteral("dimension"), tr("Arguments"), {QStringLiteral("1"), QStringLiteral("2")},
                     {tr("One: f(x)"), tr("Two: f(x, y) on a grid")}, QStringLiteral("1")),
              shownIf(rows, QStringLiteral("dimension=1")), shownIf(grid, QStringLiteral("dimension=2")),
              text(QStringLiteral("argunit"), tr("Arguments' units"), QString(), tr("Two split by a comma for f(x, y): um, um")),
              text(QStringLiteral("unit"), tr("Function's unit"), QString())},
             {}, {}, false, false, QStringLiteral("ip"),
             tr("A function given by a table: of one argument, linear between its points; of two, on a grid of every x with "
                "every y, bilinear between them (an imported optical generation map); constant beyond them.")});
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
          QStringLiteral("magnetic"), QStringLiteral("modes"), QStringLiteral("waves"), QStringLiteral("resonator"), QStringLiteral("semiconductor"),
          QStringLiteral("rays"), QStringLiteral("quantum"), QStringLiteral("pde")},
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
    // (A tensor of a crystal: in its own axes, turned into the model's by
    // its orientation.)
    const QString tensor = tr("One value, or a tensor in the crystal's axes split by ';': three along them (x; y; z), six symmetric "
                              "(xx; yy; zz; yz; xz; xy), nine row by row");
    const QString alongAxes = tr("One value, or three along the crystal's axes split by ';' (x; y; z: an ordinary and an extraordinary "
                                 "ray's no; no; ne)");
    add({QStringLiteral("material"), tr("Material"), QStringLiteral("material"), QStringLiteral("material"), Level::Domain,
         {selectionProp(none), text(QStringLiteral("library"), tr("From the library"), QString()),
          choice(QStringLiteral("orientation"), tr("Crystal orientation"), {QStringLiteral("none"), QStringLiteral("cut"), QStringLiteral("euler")},
                 {tr("Its axes the model's"), tr("A wafer's cut"), tr("Euler's angles")}, QStringLiteral("none"),
                 tr("Where an anisotropic crystal's axes lie in the model (x across, y up - a wafer's normal -, z out of the plane, along a "
                    "waveguide): its tensors - permittivity, conductivities, index, stiffness, the piezoelectric, electro-optic and "
                    "photoelastic ones - turned into the model's axes in every physics")),
          shownIf(choice(QStringLiteral("cut"), tr("Crystal axis up (y)"), {QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("z")},
                         {tr("x: x-cut"), tr("y: y-cut"), tr("z: z-cut")}, QStringLiteral("x"), tr("The crystal's axis along the wafer's normal")),
                  QStringLiteral("orientation=cut")),
          shownIf(choice(QStringLiteral("along"), tr("Crystal axis along z"), {QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("z")},
                         {tr("x: x-propagating"), tr("y: y-propagating"), tr("z: z-propagating")}, QStringLiteral("y"),
                         tr("The crystal's axis along the waveguide, out of the plane; the third lies along x (x-cut, y-propagating "
                            "lithium niobate: its z, the optic axis, across)")),
                  QStringLiteral("orientation=cut")),
          shownIf(expr(QStringLiteral("euler_phi"), tr("Euler angle φ"), QStringLiteral("angle"), QStringLiteral("0"),
                       tr("z-x-z: the crystal turned φ about z, then θ about its new x, then ψ about its new z (degrees, or [rad])")),
                  QStringLiteral("orientation=euler")),
          shownIf(expr(QStringLiteral("euler_theta"), tr("Euler angle θ"), QStringLiteral("angle"), QStringLiteral("0")), QStringLiteral("orientation=euler")),
          shownIf(expr(QStringLiteral("euler_psi"), tr("Euler angle ψ"), QStringLiteral("angle"), QStringLiteral("0")), QStringLiteral("orientation=euler")),
          spatial(QStringLiteral("epsilonr"), tr("Relative permittivity εr"), QString(), QString(), tensor),
          spatial(QStringLiteral("sigma"), tr("Electrical conductivity σ"), QStringLiteral("S/m"), QString(), tensor),
          spatial(QStringLiteral("k"), tr("Thermal conductivity k"), QStringLiteral("W/(m*K)"), QString(), tensor),
          spatial(QStringLiteral("rho"), tr("Density ρ"), QStringLiteral("kg/m^3"), QString()),
          spatial(QStringLiteral("Cp"), tr("Heat capacity Cp"), QStringLiteral("J/(kg*K)"), QString()),
          spatial(QStringLiteral("tand"), tr("Loss tangent tan δ"), QString(), QString(),
                  tr("Of a dielectric: a line's conductance per metre G = ω C tan δ. %1").arg(alongAxes)),
          spatial(QStringLiteral("mur"), tr("Relative permeability µr"), QString(), QString(), tensor),
          bhRows(),
          expr(QStringLiteral("Br"), tr("Remanent flux density Br"), QStringLiteral("T"), QString(), tr("Of a permanent magnet")),
          spatial(QStringLiteral("E"), tr("Young's modulus E"), QStringLiteral("Pa"), QString()),
          spatial(QStringLiteral("nu"), tr("Poisson's ratio ν"), QString(), QString()),
          text(QStringLiteral("C"), tr("Stiffness Cij"), QString(),
               tr("An anisotropic solid's, in Voigt's notation and the crystal's axes, by entries split by ';': C11=198[GPa]; "
                  "C12=55[GPa]; C13=65[GPa]; C33=227[GPa]; C44=60[GPa]; ... (each Cij named once, Cji alike; those not named "
                  "nought). Given, it is what solid mechanics uses over E and ν")),
          spatial(QStringLiteral("alpha"), tr("Thermal expansion α"), QStringLiteral("1/K"), QString(), tensor),
          text(QStringLiteral("e"), tr("Piezoelectric stress constants eij"), QString(),
               tr("C/m², Voigt's, the crystal's axes: e15=3.7[C/m^2]; e22=2.4[C/m^2]; e31=0.2[C/m^2]; e33=1.3[C/m^2]; ... - D = e S + "
                  "ε E, σ = C S - eᵀ E (each eij named, the rest nought; a crystal's symmetry gives each that is the same: e16 = "
                  "-e22 too)")),
          text(QStringLiteral("d"), tr("Piezoelectric strain constants dij"), QString(),
               tr("C/N (pC/N), Voigt's, the crystal's axes: d15=69.2[pC/N]; d16=-41.6[pC/N]; ... - S = sᴱ σ + dᵀ E, a datasheet's "
                  "form; without e, e = d C (its stiffness Cij needed)")),
          spatial(QStringLiteral("n"), tr("Refractive index n"), QString(), QString(), alongAxes),
          spatial(QStringLiteral("kappa"), tr("Extinction coefficient κ"), QString(), QString(),
                  tr("The refractive index's imaginary part: n - jκ. %1").arg(alongAxes)),
          expr(QStringLiteral("dndT"), tr("Thermo-optic coefficient dn/dT"), QStringLiteral("1/K"), QString(), alongAxes),
          text(QStringLiteral("r"), tr("Electro-optic coefficients rij"), QString(),
               tr("Pockels', m/V (pm/V), Voigt's, the crystal's axes: Δ(1/ε)_i = Σ r_ij E_j - r13=9.6[pm/V]; r22=6.8[pm/V]; "
                  "r33=30.9[pm/V]; r51=32.6[pm/V]; ... (each named; the same by symmetry too: r23, r12 = -r22, r42)")),
          text(QStringLiteral("p"), tr("Photoelastic coefficients pij"), QString(),
               tr("Pockels', Voigt's, the crystal's axes: Δ(1/ε)_i = Σ p_ij S_j (S an engineering strain) - p11=-0.094; "
                  "p12=0.017; p44=-0.051 (silicon's; p22, p13, p55... the same by symmetry, named too; pji is pij where it is not named)")),
          expr(QStringLiteral("g"), tr("Gyration g"), QString(), QString(),
               tr("A magneto-optic material's, magnetized: ε's off-diagonal j g along the magnetization (Ce:YIG's about 0.009 at "
                  "1550 nm) - a wave feature's Magneto-optic option")),
          expr(QStringLiteral("Eg"), tr("Band gap Eg"), QStringLiteral("eV"), QString(),
               tr("A semiconductor's (an insulator has none). Given, it is the gap every physics uses, over its compound's - at "
                  "300 K: quantum wells at another temperature move it as the compound's moves. With a compound, the valence band "
                  "stays at the compound's VBO, so a gap given moves the conduction band (give VBO too to move the other)")),
          expr(QStringLiteral("lambdag"), tr("Gap wavelength λg"), QStringLiteral("m"), QString(),
               tr("Eg = hc/λg when Eg is not given: a quaternary named by its gap's wavelength (\"1.3Q\": 1.3[um]) - its masses "
                  "and deformation potentials still its compound's at the composition given; or, its composition solved for "
                  "the gap (Composition), all of them of that composition")),
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
          choice(QStringLiteral("composition"), tr("Composition"), {QStringLiteral("given"), QStringLiteral("gap")},
                 {tr("As given (x, y)"), tr("Solved for its gap λg (a quaternary on a lattice)")}, QStringLiteral("given"),
                 tr("Solved: the alloy whose gap at 300 K is hc/λg in its band set - a ternary's x; a quaternary's x and y on "
                    "the lattice below (InGaAsP along As and P, AlGaInAs and AlGaInP along Al and Ga) -, its gap, masses, "
                    "offsets and deformation potentials all of that composition (the 1.3Q's of the 300 K fit: In0.72 Ga0.28 As0.60 P0.40)")),
          shownIf(text(QStringLiteral("lattice"), tr("Lattice-matched to"), QStringLiteral("InP"), tr("A binary: InP, GaAs...")),
                  QStringLiteral("composition=gap")),
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
    // (A scalar physics' coefficient as given: one, or a tensor in the
    // plane - its xx, yy and xy, the model's axes.)
    auto planeTensor = [](const QString& key, const QString& symbol, const QString& unit, const QString& def) {
        const QString user = QStringLiteral("source=user&anisotropy!=isotropic");
        return QList<P>{
            shownIf(choice(QStringLiteral("anisotropy"), tr("Anisotropy"), {QStringLiteral("isotropic"), QStringLiteral("diagonal"), QStringLiteral("symmetric")},
                           {tr("Isotropic"), tr("Diagonal (x, y)"), tr("Symmetric (xx, yy, xy)")}, QStringLiteral("isotropic"),
                           tr("In the model's axes (r and z about an axis); a material's tensor is its own, turned by its orientation")),
                    QStringLiteral("source=user")),
            shownIf(spatial(key + QStringLiteral("xx"), symbol + QStringLiteral(", xx"), unit, def), user),
            shownIf(spatial(key + QStringLiteral("yy"), symbol + QStringLiteral(", yy"), unit, def), user),
            shownIf(spatial(key + QStringLiteral("xy"), symbol + QStringLiteral(", xy"), unit, QStringLiteral("0")),
                    QStringLiteral("source=user&anisotropy=symmetric"))};
    };
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
         QList<P>{selectionProp(), source,
                  shownIf(spatial(QStringLiteral("epsilonr"), tr("Relative permittivity εr"), QString(), QStringLiteral("1")),
                          QStringLiteral("source=user&anisotropy=isotropic"))}
             + planeTensor(QStringLiteral("eps"), tr("εr"), QString(), QStringLiteral("1")),
         {}, {}, false, false, QStringLiteral("ccn"),
         tr("D = ε0 εr E on its domains, εr from their material or as given - a tensor too: an anisotropic crystal's (lithium "
            "niobate, sapphire, quartz), its loss tangent along its axes too.")});
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
         QList<P>{selectionProp(), source,
                  shownIf(spatial(QStringLiteral("sigma"), tr("Electrical conductivity σ"), QStringLiteral("S/m"), QStringLiteral("1[S/m]")),
                          QStringLiteral("source=user&anisotropy=isotropic"))}
             + planeTensor(QStringLiteral("sigma"), tr("σ"), QStringLiteral("S/m"), QStringLiteral("1[S/m]")),
         {}, {}, false, false, QStringLiteral("cuc"), tr("J = σ E on its domains, σ from their material or as given - a tensor too.")});
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
         QList<P>{selectionProp(), source,
          shownIf(spatial(QStringLiteral("k"), tr("Thermal conductivity k"), QStringLiteral("W/(m*K)"), QStringLiteral("1[W/(m*K)]")),
                  QStringLiteral("source=user&anisotropy=isotropic")),
          shownIf(spatial(QStringLiteral("rho"), tr("Density ρ"), QStringLiteral("kg/m^3"), QStringLiteral("1000[kg/m^3]")),
                  QStringLiteral("source=user")),
          shownIf(spatial(QStringLiteral("Cp"), tr("Heat capacity Cp"), QStringLiteral("J/(kg*K)"), QStringLiteral("1000[J/(kg*K)]")),
                  QStringLiteral("source=user"))}
             + planeTensor(QStringLiteral("k"), tr("k"), QStringLiteral("W/(m*K)"), QStringLiteral("1[W/(m*K)]")),
         {}, {}, false, false, QStringLiteral("sld"),
         tr("q = -k ∇T on its domains; k (and for a study in time ρ and Cp) from their material or as given - k a tensor too: a "
            "laminate's in its plane and across it, a crystal's along its axes.")});
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
          QStringLiteral("solid/roller"), QStringLiteral("solid/load"), QStringLiteral("solid/bodyload"), QStringLiteral("solid/piezo")},
         {QStringLiteral("solid/elastic"), QStringLiteral("solid/free")}, false, false, QStringLiteral("solid"),
         tr("The displacement (u, v) of a linear elastic solid: -∇·σ = f, σ = D (ε - ε0), ε0 a thermal strain. In the plane, "
            "in plane strain or plane stress; about an axis, axisymmetric (the component's space).")});
    add({QStringLiteral("solid/elastic"), tr("Linear Elastic Material"), QStringLiteral("solid"), QStringLiteral("domainfeature"), Level::Domain,
         {selectionProp(), source,
          shownIf(choice(QStringLiteral("anisotropy"), tr("Anisotropy"), {QStringLiteral("isotropic"), QStringLiteral("anisotropic")},
                         {tr("Isotropic (E, ν)"), tr("Anisotropic (Cij)")}, QStringLiteral("isotropic"),
                         tr("In the model's axes; a material's stiffness Cij is its crystal's, turned by its orientation")),
                  QStringLiteral("source=user")),
          shownIf(spatial(QStringLiteral("E"), tr("Young's modulus E"), QStringLiteral("Pa"), QStringLiteral("200[GPa]")),
                  QStringLiteral("source=user&anisotropy=isotropic")),
          shownIf(spatial(QStringLiteral("nu"), tr("Poisson's ratio ν"), QString(), QStringLiteral("0.3")), QStringLiteral("source=user&anisotropy=isotropic")),
          shownIf(text(QStringLiteral("C"), tr("Stiffness Cij"), QStringLiteral("C11=165.7[GPa]; C12=63.9[GPa]; C13=63.9[GPa]; C22=165.7[GPa]; C23=63.9[GPa]; C33=165.7[GPa]; C44=79.6[GPa]; C55=79.6[GPa]; C66=79.6[GPa]"),
                       tr("Voigt's, the model's axes (xx, yy, zz, yz, xz, xy), entries split by ';' - silicon's along its cube's axes "
                          "by default")),
                  QStringLiteral("source=user&anisotropy=anisotropic")),
          flag(QStringLiteral("thermal"), tr("Thermal expansion"), false, tr("A strain α (T - Tref), T from a heat physics or as given")),
          shownIf(spatial(QStringLiteral("alpha"), tr("Coefficient of thermal expansion α"), QStringLiteral("1/K"), QStringLiteral("1e-5[1/K]"),
                          tr("One, or a tensor: x; y; z (or six, symmetric)")),
                  QStringLiteral("source=user")),
          shownIf(spatial(QStringLiteral("T"), tr("Temperature"), QStringLiteral("K"), QStringLiteral("T"),
                          tr("An expression: T (a heat physics' field), ht.T, or a value")),
                  QStringLiteral("thermal=true")),
          shownIf(spatial(QStringLiteral("Tref"), tr("Strain-free temperature Tref"), QStringLiteral("K"), QStringLiteral("293.15[K]")),
                  QStringLiteral("thermal=true"))},
         {}, {}, false, false, QStringLiteral("lemm"),
         tr("Hooke's law on its domains: E and ν (and α) from their material or as given - or an anisotropic crystal's stiffness "
            "Cij (silicon's cubic one, lithium niobate's trigonal one), turned by its orientation: plane strain takes its xx, yy, "
            "xy part, plane stress what is left with no stress out of the plane.")});
    {
        P es;
        es.key = QStringLiteral("es");
        es.label = tr("Electrostatics");
        es.kind = P::Physics;
        es.defaultValue = QJsonArray{QStringLiteral("es")};
        es.tooltip = tr("The electrostatics whose field it feels and whose charge its strain makes");
        add({QStringLiteral("solid/piezo"), tr("Piezoelectric Effect"), QStringLiteral("solid"), QStringLiteral("domainfeature"), Level::Domain,
             {selectionProp(none), es, source,
              shownIf(text(QStringLiteral("e"), tr("Piezoelectric stress constants eij"), QStringLiteral("e33=1.3[C/m^2]"),
                           tr("C/m², Voigt's, the model's axes: e15=3.7[C/m^2]; e22=2.4[C/m^2]; ... (the material's, turned, when it "
                              "is from the material)")),
                      QStringLiteral("source=user"))},
             {}, {}, false, false, QStringLiteral("pze"),
             tr("A piezoelectric solid, its stress and charge in one: σ = C S - eᵀ E, D = εS E + e S - the solid's stiffness the "
                "one at a constant field, the electrostatics' permittivity the clamped one (εS). The two physics solved in turn "
                "till they agree, at rest; in plane strain or about an axis. Its terminals' capacitance es.C11 the clamped one; "
                "es.Q_<name> over its voltage, the free one.")});
    }
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
          shownIf(choice(QStringLiteral("anisotropy"), tr("Anisotropy"), {QStringLiteral("isotropic"), QStringLiteral("diagonal"), QStringLiteral("symmetric")},
                         {tr("Isotropic"), tr("Diagonal (x, y)"), tr("Symmetric (xx, yy, xy)")}, QStringLiteral("isotropic"),
                         tr("µr in the plane, the model's axes: a laminated core's along its sheets and across them")),
                  QStringLiteral("source=user&constitutive=mur")),
          shownIf(spatial(QStringLiteral("mur"), tr("Relative permeability µr"), QString(), QStringLiteral("1")),
                  QStringLiteral("source=user&constitutive=mur&anisotropy=isotropic")),
          shownIf(spatial(QStringLiteral("murxx"), tr("µr, xx"), QString(), QStringLiteral("1")),
                  QStringLiteral("source=user&constitutive=mur&anisotropy!=isotropic")),
          shownIf(spatial(QStringLiteral("muryy"), tr("µr, yy"), QString(), QStringLiteral("1")),
                  QStringLiteral("source=user&constitutive=mur&anisotropy!=isotropic")),
          shownIf(spatial(QStringLiteral("murxy"), tr("µr, xy"), QString(), QStringLiteral("0")),
                  QStringLiteral("source=user&constitutive=mur&anisotropy=symmetric")),
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
        const QString tag = physics == QLatin1String("modes") ? QStringLiteral("emw") : physics == QLatin1String("resonator") ? QStringLiteral("emr") : QStringLiteral("ewfd");
        add({p + QStringLiteral("wave"), title, tag,
             QStringLiteral("domainfeature"), Level::Domain,
             {selectionProp(),
              choice(QStringLiteral("constitutive"), tr("Electric displacement"), {QStringLiteral("n"), QStringLiteral("epsilonr")},
                     {tr("Refractive index (n - jκ)"), tr("Relative permittivity (loss tangent, conductivity)")}, QStringLiteral("n")),
              source,
              shownIf(choice(QStringLiteral("anisotropy"), tr("Anisotropy"), {QStringLiteral("isotropic"), QStringLiteral("diagonal"), QStringLiteral("tensor")},
                             {tr("Isotropic"), tr("Diagonal (x, y, z)"), tr("Tensor (εr symmetric: xx, yy, zz, xy, xz, yz)")},
                             QStringLiteral("isotropic"),
                             tr("In the model's axes. A tensor's xz and yz couple the transverse field to Ez: a crystal turned out of "
                                "the cross-section's plane - its modes found by β, not β² (forward ones)")),
                      QStringLiteral("source=user")),
              shownIf(spatial(QStringLiteral("n"), tr("Refractive index n"), QString(), QStringLiteral("1")),
                      QStringLiteral("source=user&constitutive=n&anisotropy=isotropic")),
              shownIf(spatial(QStringLiteral("nx"), tr("nx"), QString(), QStringLiteral("1")), QStringLiteral("source=user&constitutive=n&anisotropy=diagonal")),
              shownIf(spatial(QStringLiteral("ny"), tr("ny"), QString(), QStringLiteral("1")), QStringLiteral("source=user&constitutive=n&anisotropy=diagonal")),
              shownIf(spatial(QStringLiteral("nz"), tr("nz"), QString(), QStringLiteral("1")), QStringLiteral("source=user&constitutive=n&anisotropy=diagonal")),
              shownIf(spatial(QStringLiteral("kappa"), tr("Extinction coefficient κ"), QString(), QStringLiteral("0"),
                              tr("One, or three along x, y and z split by ';' (a sheet's absorption in its plane alone: κ; 0; κ for "
                                 "one along x and z)")),
                      QStringLiteral("source=user&constitutive=n")),
              shownIf(spatial(QStringLiteral("epsilonr"), tr("Relative permittivity εr"), QString(), QStringLiteral("1")),
                      QStringLiteral("source=user&constitutive=epsilonr&anisotropy=isotropic")),
              shownIf(spatial(QStringLiteral("epsxx"), tr("εxx"), QString(), QStringLiteral("1")),
                      QStringLiteral("source=user&constitutive=epsilonr&anisotropy=diagonal|source=user&constitutive=epsilonr&anisotropy=tensor")),
              shownIf(spatial(QStringLiteral("epsyy"), tr("εyy"), QString(), QStringLiteral("1")),
                      QStringLiteral("source=user&constitutive=epsilonr&anisotropy=diagonal|source=user&constitutive=epsilonr&anisotropy=tensor")),
              shownIf(spatial(QStringLiteral("epszz"), tr("εzz"), QString(), QStringLiteral("1")),
                      QStringLiteral("source=user&constitutive=epsilonr&anisotropy=diagonal|source=user&constitutive=epsilonr&anisotropy=tensor")),
              shownIf(spatial(QStringLiteral("epsxy"), tr("εxy"), QString(), QStringLiteral("0")),
                      QStringLiteral("source=user&constitutive=epsilonr&anisotropy=tensor")),
              shownIf(spatial(QStringLiteral("epsxz"), tr("εxz"), QString(), QStringLiteral("0")),
                      QStringLiteral("source=user&constitutive=epsilonr&anisotropy=tensor")),
              shownIf(spatial(QStringLiteral("epsyz"), tr("εyz"), QString(), QStringLiteral("0")),
                      QStringLiteral("source=user&constitutive=epsilonr&anisotropy=tensor")),
              shownIf(spatial(QStringLiteral("tand"), tr("Loss tangent tan δ"), QString(), QStringLiteral("0"),
                              tr("One, or three along x, y and z split by ';'")),
                      QStringLiteral("source=user&constitutive=epsilonr")),
              shownIf(spatial(QStringLiteral("sigma"), tr("Electrical conductivity σ"), QStringLiteral("S/m"), QStringLiteral("0[S/m]"),
                              tr("One, or three along x, y and z split by ';': a conducting sheet's in its plane alone (graphene, ITO "
                                 "as a thin layer: σ; 0; σ for one along x and z), no current across it")),
                      QStringLiteral("source=user&constitutive=epsilonr")),
              shownIf(spatial(QStringLiteral("mur"), tr("Relative permeability µr"), QString(), QStringLiteral("1")), QStringLiteral("source=user")),
              flag(QStringLiteral("electrooptic"), tr("Electro-optic (Pockels)"), false,
                   tr("Δ(1/ε)_i = Σ r_ij E_j: its material's r (the crystal's axes, turned by its orientation) or as given, E a "
                      "static field - an electrostatics' (es.Ex, es.Ey) or as given: a lithium niobate modulator's index under its "
                      "electrodes, every r_ij - r33, r13, r22, r51 - and the field's every component")),
              shownIf(text(QStringLiteral("r"), tr("Electro-optic coefficients rij"), QStringLiteral("r33=30.9[pm/V]"),
                           tr("Voigt's, the model's axes: r13=9.6[pm/V]; r33=30.9[pm/V]; ... (the material's, turned, when it is "
                              "from the material)")),
                      QStringLiteral("electrooptic=true&source=user")),
              flag(QStringLiteral("kerr"), tr("Quadratic electro-optic (Kerr, QCSE)"), false,
                   tr("Δn = ½ n³ R (|E|² - E0²) along each axis, E the static field below: a quantum well stack's electrorefraction "
                      "below its edge (R about 1e-19 m²/V² TE in InGaAsP wells at 1550 nm), the simple alternative to its wells' "
                      "absorption computed (Quantum Wells' Absorption)")),
              shownIf(text(QStringLiteral("Rkerr"), tr("R along x, y, z"), QStringLiteral("1e-19[m^2/V^2]"),
                           tr("One value, or three \"a; b; c\" - a stack grown along y: TE's along x and z, TM's along y")),
                      QStringLiteral("kerr=true")),
              shownIf(expr(QStringLiteral("E0kerr"), tr("Field the index is given at E0"), QStringLiteral("V/m"), QStringLiteral("0[V/m]")),
                      QStringLiteral("kerr=true")),
              shownIf(spatial(QStringLiteral("Ex"), tr("Static field Ex"), QStringLiteral("V/m"), QStringLiteral("es.Ex")), QStringLiteral("electrooptic=true|kerr=true")),
              shownIf(spatial(QStringLiteral("Ey"), tr("Static field Ey"), QStringLiteral("V/m"), QStringLiteral("es.Ey")), QStringLiteral("electrooptic=true|kerr=true")),
              shownIf(spatial(QStringLiteral("Ez"), tr("Static field Ez"), QStringLiteral("V/m"), QStringLiteral("0[V/m]")), QStringLiteral("electrooptic=true|kerr=true")),
              flag(QStringLiteral("qwabsorption"), tr("Quantum wells' absorption (QCSE)"), false,
                   tr("A quantum wells physics' absorption and Kramers-Kronig index at the mode's photon energy, of the field at each "
                      "point: the wells solved again at fields from -Fmax to Fmax, their spectrum as the study's Spectrum step says (its "
                      "energies the Kramers-Kronig window, its broadening, excitons, absorption per), α in place of its material's κ (a "
                      "compound's bulk edge; a κ given here added to it), n(F) - n(F0) added to n - "
                      "TE along the wells' plane, TM along their axis. An MQW modulator's α(V) and Δn(V) from its semiconductor's field")),
              shownIf(text(QStringLiteral("qwtag"), tr("Quantum wells"), QStringLiteral("qw"), tr("The tag of the quantum wells physics")),
                      QStringLiteral("qwabsorption=true")),
              shownIf(text(QStringLiteral("spectrumstep"), tr("Spectrum step"), QString(),
                           tr("The tag of the Spectrum step whose options the wells' spectrum takes - one disabled too, its options "
                              "alone read (empty: the mode step's study's first)")),
                      QStringLiteral("qwabsorption=true")),
              shownIf(spatial(QStringLiteral("Fqw"), tr("Field along the wells' axis"), QStringLiteral("V/m"), QStringLiteral("semi.Ey"),
                              tr("At each point: a semiconductor's (semi.Ey for wells grown along y), as the wells' F")),
                      QStringLiteral("qwabsorption=true")),
              shownIf(expr(QStringLiteral("Fmax"), tr("Largest field"), QStringLiteral("V/m"), QStringLiteral("200[kV/cm]"),
                           tr("The table's: from -Fmax to Fmax, beyond it held")),
                      QStringLiteral("qwabsorption=true")),
              shownIf(integer(QStringLiteral("Fpoints"), tr("Fields solved"), 21, tr("Evenly from -Fmax to Fmax, interpolated between")),
                      QStringLiteral("qwabsorption=true")),
              shownIf(expr(QStringLiteral("F0qw"), tr("Field the index is given at F0"), QStringLiteral("V/m"), QStringLiteral("0[V/m]")),
                      QStringLiteral("qwabsorption=true")),
              flag(QStringLiteral("photoelastic"), tr("Photoelastic"), false,
                   tr("Δ(1/ε)_i = Σ p_ij S_j: its material's p (turned by its orientation) or as given, S a solid mechanics' strain "
                      "(solid.ex, solid.ey, solid.ez, solid.exy) - stress's birefringence, a heater's or a cladding's")),
              shownIf(text(QStringLiteral("p"), tr("Photoelastic coefficients pij"),
                           QStringLiteral("p11=-0.094; p22=-0.094; p33=-0.094; p12=0.017; p13=0.017; p23=0.017; p44=-0.051; p55=-0.051; p66=-0.051"),
                           tr("Voigt's, the model's axes (the material's, turned, when it is from the material)")),
                      QStringLiteral("photoelastic=true&source=user")),
              shownIf(text(QStringLiteral("strain"), tr("Solid mechanics"), QStringLiteral("solid"),
                           tr("The tag of the solid mechanics whose strain it is (its ex, ey, ez, exy)")),
                      QStringLiteral("photoelastic=true")),
              flag(QStringLiteral("magnetooptic"), tr("Magneto-optic"), false,
                   tr("A magnetized garnet's gyrotropy: ε_ij + j e_ijk g_k (e^{jωt}), g along its magnetization - its material's g or "
                      "as given. Along x (in the plane, across a waveguide), TM modes' β differs forward and back: an isolator's "
                      "non-reciprocal phase shift (the mode found is the forward one; g's sign turned, the backward)")),
              shownIf(choice(QStringLiteral("magnetization"), tr("Magnetization along"), {QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("z")},
                             {tr("x"), tr("y"), tr("z (along the waveguide: Faraday's)")}, QStringLiteral("x")),
                      QStringLiteral("magnetooptic=true")),
              shownIf(expr(QStringLiteral("g"), tr("Gyration g"), QString(), QStringLiteral("0.009"),
                           tr("Negative: magnetized the other way")),
                      QStringLiteral("magnetooptic=true&source=user")),
              flag(QStringLiteral("thermooptic"), tr("Thermo-optic"), false,
                   tr("n + dn/dT (T - Tref): T from a heat physics (as T) or as given")),
              shownIf(expr(QStringLiteral("dndT"), tr("Thermo-optic coefficient dn/dT"), QStringLiteral("1/K"), QStringLiteral("1.86e-4[1/K]")),
                      QStringLiteral("thermooptic=true&source=user")),
              shownIf(spatial(QStringLiteral("T"), tr("Temperature"), QStringLiteral("K"), QStringLiteral("T")), QStringLiteral("thermooptic=true")),
              shownIf(expr(QStringLiteral("Tref"), tr("Reference temperature"), QStringLiteral("K"), QStringLiteral("293.15[K]")),
                      QStringLiteral("thermooptic=true")),
              flag(QStringLiteral("plasma"), tr("Plasma dispersion (free carriers)"), false,
                   tr("The free carriers' index: silicon's fit (Soref and Bennett's), Drude's of the material's masses, or "
                      "coefficients given - N and P from a semiconductor physics, at the mode's wavelength")),
              shownIf(choice(QStringLiteral("plasmamodel"), tr("Model"), {QStringLiteral("soref"), QStringLiteral("drude"), QStringLiteral("user")},
                             {tr("Silicon's (Soref and Bennett)"), tr("Drude's (the material's masses)"), tr("Coefficients given")},
                             QStringLiteral("soref"),
                             tr("Silicon's: Δn = -(8.8e-22 N + 8.5e-18 P^0.8), Δα = 8.5e-18 N + 6.0e-18 P (cm⁻³, cm⁻¹; at 1550 nm, as λ²). "
                                "Drude's: Δn = -(q²λ²/8π²c²ε0 n)(N/me + P/mh) - a III-V's (InP's some five times silicon's per "
                                "electron); its absorption Free-carrier absorption's. Given: Δn = (∂n/∂N) N + (∂n/∂P) P")),
                      QStringLiteral("plasma=true")),
              shownIf(expr(QStringLiteral("mepl"), tr("Electron mass me (m0)"), QString(), QString(),
                           tr("Empty: its material's me (a III-V's, of its composition)")),
                      QStringLiteral("plasma=true&plasmamodel=drude")),
              shownIf(expr(QStringLiteral("mhpl"), tr("Hole mass mh (m0)"), QString(), QString(),
                           tr("Empty: its material's, of its Luttinger parameters - heavy and light holes' conductivity mass "
                              "(mhh^1.5 + mlh^1.5)/(mhh^0.5 + mlh^0.5), mhh = 1/(γ1 - 2γ2), mlh = 1/(γ1 + 2γ2)")),
                      QStringLiteral("plasma=true&plasmamodel=drude")),
              shownIf(expr(QStringLiteral("dndN"), tr("∂n/∂N"), QStringLiteral("m^3"), QStringLiteral("-4.3e-21[cm^3]")),
                      QStringLiteral("plasma=true&plasmamodel=user")),
              shownIf(expr(QStringLiteral("dndP"), tr("∂n/∂P"), QStringLiteral("m^3"), QStringLiteral("-1e-21[cm^3]")),
                      QStringLiteral("plasma=true&plasmamodel=user")),
              flag(QStringLiteral("fca"), tr("Free-carrier absorption"), false,
                   tr("Δα = σn N + σp P, each cross-section at 1550 nm and as λ² - the material's (sigmaFCn, sigmaFCp: InP's "
                      "family 1e-18 and 2e-17 cm²) or as given; of all the carriers there, a doped cladding's loss with them")),
              shownIf(choice(QStringLiteral("fcasource"), tr("Cross-sections"), {QStringLiteral("material"), QStringLiteral("user")},
                             {tr("From the material (sigmaFCn, sigmaFCp)"), tr("User defined")}, QStringLiteral("material")),
                      QStringLiteral("fca=true")),
              shownIf(expr(QStringLiteral("sigmaFCn"), tr("Electrons' σn (at 1550 nm)"), QStringLiteral("m^2"), QStringLiteral("1e-18[cm^2]")),
                      QStringLiteral("fca=true&fcasource=user")),
              shownIf(expr(QStringLiteral("sigmaFCp"), tr("Holes' σp (at 1550 nm)"), QStringLiteral("m^2"), QStringLiteral("2e-17[cm^2]")),
                      QStringLiteral("fca=true&fcasource=user")),
              flag(QStringLiteral("bandfilling"), tr("Band filling (Burstein-Moss)"), false,
                   tr("Free carriers filling the states a direct gap's absorption joins: that absorption bleached, the index "
                      "lowered below the gap (Kramers-Kronig's) - a depletion modulator's largest term. Computed of the "
                      "material's gap, masses and Ep (Bennett, Soref and del Alamo's), or Δn per carrier given")),
              shownIf(choice(QStringLiteral("bfmodel"), tr("Band filling"), {QStringLiteral("computed"), QStringLiteral("user")},
                             {tr("Computed (the material's Eg, me, γ1, γ2, Ep)"), tr("Δn per carrier given")}, QStringLiteral("computed")),
                      QStringLiteral("bandfilling=true")),
              shownIf(spatial(QStringLiteral("Pbfn"), tr("Δn per electron"), QStringLiteral("m^3"), QStringLiteral("-1e-21[cm^3]"),
                              tr("Of the wavelength too - an Interpolation of lambda0")),
                      QStringLiteral("bandfilling=true&bfmodel=user")),
              shownIf(spatial(QStringLiteral("Pbfp"), tr("Δn per hole"), QStringLiteral("m^3"), QStringLiteral("0[cm^3]")),
                      QStringLiteral("bandfilling=true&bfmodel=user")),
              shownIf(flag(QStringLiteral("bfholes"), tr("Holes' too"), true, tr("The holes' band filling besides the electrons'")),
                      QStringLiteral("bandfilling=true&bfmodel=computed")),
              shownIf(flag(QStringLiteral("bfexciton"), tr("Excitons' enhancement"), true,
                           tr("Of Kane's Ep: Sommerfeld's enhancement of the edge by the electron and hole's attraction (the "
                              "material's εr), as measured edges have - a material's Afk, a measured edge, is taken as it is")),
                      QStringLiteral("bandfilling=true&bfmodel=computed")),
              shownIf(expr(QStringLiteral("Tbf"), tr("Carriers' temperature"), QStringLiteral("K"), QStringLiteral("300[K]")),
                      QStringLiteral("bandfilling=true&bfmodel=computed")),
              shownIf(spatial(QStringLiteral("Ncarriers"), tr("Electron concentration"), QStringLiteral("1/m^3"), QStringLiteral("semi.n")),
                      QStringLiteral("plasma=true|fca=true|bandfilling=true")),
              shownIf(spatial(QStringLiteral("Pcarriers"), tr("Hole concentration"), QStringLiteral("1/m^3"), QStringLiteral("semi.p")),
                      QStringLiteral("plasma=true|fca=true|bandfilling=true")),
              flag(QStringLiteral("electroabsorption"), tr("Direct-gap absorption (Franz-Keldysh)"), false,
                   tr("αλ/4π in place of the material's κ (the same edge, measured - a κ given here is added), α a direct gap's: A "
                      "√((ħω - Eg)/1 eV) (1 eV/ħω) without a field, and under one the Franz-Keldysh "
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
                "y and z, or a tensor (a crystal's, turned by its material's orientation), its losses along each axis; warmed: n + "
                "dn/dT (T - Tref); free carriers' plasma dispersion (silicon's, Drude's), absorption and band filling; a direct "
                "gap's absorption and the Franz-Keldysh effect on it; "
                "Pockels' electro-optic effect of a static field, the photoelastic effect of a strain, a magnetized garnet's "
                "gyrotropy. From their material or as given.")});
        add({p + QStringLiteral("pml"), tr("Perfectly Matched Layer"), tag,
             QStringLiteral("pml"), Level::Domain,
             {selectionProp(none), expr(QStringLiteral("reflection"), tr("Reflection at its outside (its strength)"), QString(), QStringLiteral("1e-6"))},
             {}, {}, false, false, QStringLiteral("pml"),
             tr("Domains beside, above or round the others that take the waves in without reflecting them: their coordinates "
                "stretched into the complex plane, more the deeper - as strongly as a wave at normal incidence is reflected "
                "from its outside by \"reflection\" (1e-6: e^-13.8; less, stronger - and more of its own modes).")});
        add({p + QStringLiteral("pec"), tr("Perfect Electric Conductor"), tag,
             QStringLiteral("ground"), Level::Boundary, {selectionProp()}, {}, {}, false, false, QStringLiteral("pec"),
             tr("n × E = 0. On the outside by default.")});
        add({p + QStringLiteral("pmc"), tr("Perfect Magnetic Conductor"), tag,
             QStringLiteral("boundaryfeature"), Level::Boundary, {selectionProp(none)}, {}, {}, false, false, QStringLiteral("pmc"),
             tr("n × H = 0: a plane of symmetry of the fields.")});
    };
    // A sheet of surface conductivity on boundaries (a mode analysis's).
    auto sheet = [&add, &none]() {
        add({QStringLiteral("modes/sheet"), tr("Conductive Sheet"), QStringLiteral("emw"), QStringLiteral("boundaryfeature"), Level::Boundary,
             {selectionProp(none),
              choice(QStringLiteral("model"), tr("Its conductivity"), {QStringLiteral("graphene"), QStringLiteral("user")},
                     {tr("Graphene (Kubo's)"), tr("User defined σs")}, QStringLiteral("graphene"),
                     tr("Graphene's of its chemical potential, scattering time and temperature at the mode's frequency - the "
                        "Drude part and the interband one (Falkovsky's, of Kubo's formula) - or as given")),
              shownIf(spatial(QStringLiteral("muc"), tr("Chemical potential µc"), QStringLiteral("eV"), QStringLiteral("0.4[eV]"),
                              tr("Its Fermi level from the Dirac point (|µc| = ħ vF √(π n)); of a gate's voltage - a field - too")),
                      QStringLiteral("model=graphene")),
              shownIf(spatial(QStringLiteral("tau"), tr("Scattering time τ"), QStringLiteral("s"), QStringLiteral("0.1[ps]"),
                              tr("Its carriers' momentum relaxation time: 2Γ = 1/τ (0.1 ps a typical CVD sheet's)")),
                      QStringLiteral("model=graphene")),
              shownIf(spatial(QStringLiteral("T"), tr("Temperature"), QStringLiteral("K"), QStringLiteral("300[K]")), QStringLiteral("model=graphene")),
              shownIf(spatial(QStringLiteral("sigmas"), tr("Surface conductivity σs, real part"), QStringLiteral("S"), QStringLiteral("6.08e-5[S]"),
                              tr("σs = σs' - j σs'' (e^{jωt}: a Drude sheet's σs'' > 0); graphene's universal e²/4ħ is 6.08e-5 S")),
                      QStringLiteral("model=user")),
              shownIf(spatial(QStringLiteral("sigmas_im"), tr("Surface conductivity σs, -imaginary part σs''"), QStringLiteral("S"), QStringLiteral("0[S]")),
                      QStringLiteral("model=user"))},
             {}, {}, false, false, QStringLiteral("sht"),
             tr("A sheet too thin to mesh - graphene, a 2D material, a thin TCO - on interior boundaries (or on the outside): the "
                "tangential H jumps by the current it carries, n × (H1 - H2) = σs Et, along the boundary and along z. Graphene's "
                "σs of µc, τ and T at the mode's frequency; emw.sheet: the power it absorbs per metre, the mode carrying 1 W.")});
    };
    add({QStringLiteral("modes"), tr("Electromagnetic Waves, Mode Analysis"), QStringLiteral("physics"), QStringLiteral("emw"), Level::Domain,
         {selectionProp(),
          choice(QStringLiteral("order"), tr("Element order"), {QStringLiteral("1"), QStringLiteral("2")},
                 {tr("Linear (Whitney's edges)"), tr("Quadratic")}, QStringLiteral("2"))},
         {QStringLiteral("modes/wave"), QStringLiteral("modes/pml"), QStringLiteral("modes/pec"), QStringLiteral("modes/pmc"),
          QStringLiteral("modes/sheet")},
         {QStringLiteral("modes/wave"), QStringLiteral("modes/pec")}, false, false, QStringLiteral("emw"),
         tr("The modes of a waveguide's cross-section - the fields E(x, y) e^-jβz along it, full-vector (Et on Nédélec's edge "
            "elements, Ez on Lagrange's) - their effective index β/k0 and loss, at a frequency or a wavelength.")});
    waveEquation(QStringLiteral("modes"), tr("Wave Equation, Electric"));
    sheet();
    add({QStringLiteral("waves"), tr("Electromagnetic Waves, Frequency Domain"), QStringLiteral("physics"), QStringLiteral("ewfd"), Level::Domain,
         {selectionProp(),
          choice(QStringLiteral("polarization"), tr("Field out of the plane"), {QStringLiteral("ez"), QStringLiteral("hz")},
                 {tr("Electric (Ez: E across the plane - TE of a parallel-plate guide)"), tr("Magnetic (Hz: E in the plane)")},
                 QStringLiteral("ez")),
          choice(QStringLiteral("order"), tr("Element order"), {QStringLiteral("1"), QStringLiteral("2")}, {tr("Linear"), tr("Quadratic")},
                 QStringLiteral("2"))},
         {QStringLiteral("waves/wave"), QStringLiteral("waves/pml"), QStringLiteral("waves/pec"), QStringLiteral("waves/pmc"),
          QStringLiteral("waves/scattering"), QStringLiteral("waves/port"), QStringLiteral("waves/periodic"),
          QStringLiteral("waves/periodicport"), QStringLiteral("waves/dipole"), QStringLiteral("waves/linecurrent")},
         {QStringLiteral("waves/wave"), QStringLiteral("waves/pec")}, false, false, QStringLiteral("ewfd"),
         tr("Waves in the plane at each frequency, Ez or Hz: -∇·(P ∇u) - k0² q u = 0. Ports give the S-parameters "
            "(ewfd.S21, ewfd.S21dB) and the reflectance, transmittance and absorptance (ewfd.R, ewfd.T, ewfd.A); a unit "
            "cell's Periodic Condition and Periodic Ports a grating's or a film's, order by order (ewfd.R_0, ewfd.T_n1); "
            "scattering boundaries and PMLs let waves out. A Point Dipole or a Line Current radiates from inside: ewfd.Psource "
            "the power it gives, ewfd.Purcell that over the same source's in a homogeneous medium.")});
    waveEquation(QStringLiteral("waves"), tr("Wave Equation, Electric"));
    add({QStringLiteral("resonator"), tr("Electromagnetic Waves, Eigenfrequency"), QStringLiteral("physics"), QStringLiteral("emr"), Level::Domain,
         {selectionProp(),
          integer(QStringLiteral("m"), tr("Azimuthal order m"), 1,
                  tr("The fields E(r, z) e^-jmφ: 0 the TE0 and TM0 modes (E round the axis, or in its planes), 1 the HE11 and EH11 "
                     "(a VCSEL's fundamental), m the modes of m azimuthal periods - each order solved alone, both of a pair its "
                     "modes")),
          choice(QStringLiteral("order"), tr("Element order"), {QStringLiteral("1"), QStringLiteral("2")},
                 {tr("Linear (Whitney's edges)"), tr("Quadratic")}, QStringLiteral("2"))},
         {QStringLiteral("resonator/wave"), QStringLiteral("resonator/pml"), QStringLiteral("resonator/pec"), QStringLiteral("resonator/pmc")},
         {QStringLiteral("resonator/wave"), QStringLiteral("resonator/pec")}, false, false, QStringLiteral("emr"),
         tr("A body of revolution's resonant modes (its component axisymmetric: r along x from the axis, z along y), of one "
            "azimuthal order m - full-vector, E(r, z) e^-jmφ: (E_r, E_z) on Nédélec's edge elements, E_φ on Lagrange's (as r E_φ); "
            "their eigenfrequencies ω complex - perfectly matched layers (stretched as a cylinder's) or lossy media taking their "
            "energy: Q = Re ω / 2 Im ω. Solved by an Eigenfrequency step: emr.freq, emr.lambda0, emr.Q; emr.Er, emr.Ez, "
            "emr.Ephi, emr.normE, the H field and the power flow. A VCSEL's cavity, a microdisk's or a sphere's whispering "
            "gallery, a cylindrical cavity.")});
    waveEquation(QStringLiteral("resonator"), tr("Wave Equation, Electric"));
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
    add({QStringLiteral("waves/periodic"), tr("Periodic Condition"), QStringLiteral("ewfd"), QStringLiteral("boundaryfeature"), Level::Boundary,
         {selectionProp(none),
          choice(QStringLiteral("floquet"), tr("Periodicity"), {QStringLiteral("port"), QStringLiteral("continuity"), QStringLiteral("given")},
                 {tr("Floquet, the fed periodic port's"), tr("Continuity"), tr("Floquet, kF given")}, QStringLiteral("port"),
                 tr("u(r + d) = u(r) e^-jkF·d, d from one side to the other: kF the plane wave's along the cell, a fed Periodic "
                    "Port's (continuity without one), nought, or as given")),
          shownIf(expr(QStringLiteral("kx"), tr("kF, its x"), QStringLiteral("1/m"), QStringLiteral("0[1/m]")), QStringLiteral("floquet=given")),
          shownIf(expr(QStringLiteral("ky"), tr("kF, its y"), QStringLiteral("1/m"), QStringLiteral("0[1/m]")), QStringLiteral("floquet=given"))},
         {}, {}, false, false, QStringLiteral("pc"),
         tr("A unit cell's two opposite sides, one the other moved by d - a grating's, a film's, a photonic crystal's walls, "
            "both in one selection ({\"of\": [...], \"side\": [\"left\", \"right\"]}): the field on one the other's, "
            "shifted in phase by kF·d (Floquet's), the two meshed alike.")});
    add({QStringLiteral("waves/periodicport"), tr("Periodic Port"), QStringLiteral("ewfd"), QStringLiteral("terminal"), Level::Boundary,
         {selectionProp(none), text(QStringLiteral("name"), tr("Port name"), QStringLiteral("1")),
          flag(QStringLiteral("excite"), tr("Fed"), true,
               tr("The plane wave comes in by it: one periodic port fed, the others take what goes out")),
          shownIf(expr(QStringLiteral("angle"), tr("Angle of incidence"), QStringLiteral("angle"), QStringLiteral("0"),
                       tr("Degrees from the port's normal, into the cell, toward +x (toward +y for a port along y); in the "
                          "port's medium")),
                  QStringLiteral("excite=true")),
          shownIf(expr(QStringLiteral("E0"), tr("Its amplitude (Ez or Hz)"), QString(), QStringLiteral("1")), QStringLiteral("excite=true"))},
         {}, {}, false, false, QStringLiteral("pport"),
         tr("A unit cell's top or bottom, one period long in one material: a plane wave fed at an angle, and what leaves in "
            "each diffraction order (m: kt = kF + 2πm/L) taken out without reflection, the near field's orders too. "
            "ewfd.R, ewfd.T, ewfd.A; each order that propagates, ewfd.R_m and ewfd.T_m (m written n1 for -1); the zeroth's "
            "fields, ewfd.r_0 and ewfd.t_0 (real and imaginary parts). Ez: TE (s), Hz: TM (p).")});
    // Point sources (a line along z in the plane's model): no selection - a
    // place of their own.
    add({QStringLiteral("waves/dipole"), tr("Point Dipole"), QStringLiteral("ewfd"), QStringLiteral("terminal"), Level::None,
         {pair(QStringLiteral("position"), tr("Position"), QStringLiteral("length"), QStringLiteral("0"), QStringLiteral("0"),
               tr("Where it is, in the geometry's unit (or with one: 330[nm]) - inside the physics' domains")),
          expr(QStringLiteral("px"), tr("Dipole moment, x"), QStringLiteral("C"), QStringLiteral("1e-20[C]"),
               tr("Per metre along z (C m/m): in the plane, it radiates Hz waves (Field out of the plane: Magnetic)")),
          expr(QStringLiteral("py"), tr("Dipole moment, y"), QStringLiteral("C"), QStringLiteral("0[C]")),
          expr(QStringLiteral("pz"), tr("Dipole moment, z"), QStringLiteral("C"), QStringLiteral("0[C]"),
               tr("Out of the plane, it radiates Ez waves (Field out of the plane: Electric) - as a line current jω pz")),
          expr(QStringLiteral("phase"), tr("Phase"), QStringLiteral("angle"), QStringLiteral("0"), tr("Degrees"))},
         {}, {}, false, false, QStringLiteral("dip"),
         tr("An oscillating dipole at a point of the plane - a line of them along z -, its current jω p: in the plane (px, py) "
            "for Hz, out of it (pz) for Ez; the other polarization's part left out, said. ewfd.Psource the power it gives "
            "(W per metre of z, -½ Re E·J*), ewfd.Psource0 the same dipole's in any homogeneous medium (in 2D the same in "
            "every one: µ0 ω³ |p|²/16 in the plane, /8 out of it), ewfd.Purcell their ratio; ports not fed take what reaches "
            "them (ewfd.Pport_<name>). In a lossless medium: in an absorbing one its near field's absorption grows as the mesh "
            "is refined, said. The mesh about it four times finer than its domain's.")});
    add({QStringLiteral("waves/linecurrent"), tr("Line Current"), QStringLiteral("ewfd"), QStringLiteral("terminal"), Level::None,
         {pair(QStringLiteral("position"), tr("Position"), QStringLiteral("length"), QStringLiteral("0"), QStringLiteral("0"),
               tr("Where it is, in the geometry's unit (or with one: 330[nm]) - inside the physics' domains")),
          expr(QStringLiteral("I"), tr("Current"), QStringLiteral("A"), QStringLiteral("1[A]"), tr("Along z: it radiates Ez waves")),
          expr(QStringLiteral("phase"), tr("Phase"), QStringLiteral("angle"), QStringLiteral("0"), tr("Degrees"))},
         {}, {}, false, false, QStringLiteral("lc"),
         tr("A filament of current along z through a point of the plane: Ez waves (Field out of the plane: Electric). "
            "ewfd.Psource the power it gives (W per metre of z), ewfd.Psource0 the same current's in a homogeneous medium "
            "(ω µ0 |I|²/8, the same in every one in 2D), ewfd.Purcell their ratio; ports not fed take what reaches them "
            "(ewfd.Pport_<name>). The mesh about it four times finer than its domain's.")});
    add({QStringLiteral("waves/port"), tr("Port"), QStringLiteral("ewfd"), QStringLiteral("terminal"), Level::Boundary,
         {selectionProp(none), text(QStringLiteral("name"), tr("Port name"), QStringLiteral("1")),
          flag(QStringLiteral("excite"), tr("Fed"), true, tr("Swept: each port fed in turn, the others matched - the S-matrix"))},
         {}, {}, false, false, QStringLiteral("port"),
         tr("A port across a waveguide: its first mode (found on the port's line) fed at 1 W, and taken out matched; the "
            "S-parameters ewfd.S<i><j> (real and imaginary parts), ewfd.S<i><j>dB; the first port fed's reflectance "
            "|Sii|², transmittance and absorptance, ewfd.R, ewfd.T, ewfd.A.")});

    // Semiconductors.
    add({QStringLiteral("semiconductor"), tr("Semiconductor"), QStringLiteral("physics"), QStringLiteral("semi"), Level::Domain,
         {selectionProp(),
          choice(QStringLiteral("space"), tr("Solved"), {QStringLiteral("2d"), QStringLiteral("1d")},
                 {tr("2D: over the cross-section"), tr("1D: along a line across the layers, its solution of that coordinate alone")},
                 QStringLiteral("2d"),
                 tr("1D: a stack's vertically depleted junction under a mesa - solved along a line through \"at\" (a strip of its "
                    "own, its sides insulating: no lateral current), each node of its domains then taking the line's values at its "
                    "coordinate, as other physics read them; its contacts where the line ends (or meets an interface), their "
                    "currents the line's density through the narrowest of its domains across it (a mesa's). At rest alone "
                    "(stationary steps)")),
          shownIf(choice(QStringLiteral("axis"), tr("Line along"), {QStringLiteral("y"), QStringLiteral("x")}, {tr("y"), tr("x")}, QStringLiteral("y")),
                  QStringLiteral("space=1d")),
          shownIf(expr(QStringLiteral("at"), tr("Line through"), QStringLiteral("length"), QString(),
                       tr("x (y, the line along x) where it runs; empty: the middle of the physics' domains")),
                  QStringLiteral("space=1d")),
          shownIf(expr(QStringLiteral("h1d"), tr("Element size along the line"), QStringLiteral("m"), QStringLiteral("1[nm]"),
                       tr("A depletion region's edge wants a few Debye lengths' worth: 1 nm is 4 at 1e18 per cm³. A plain number is metres")),
                  QStringLiteral("space=1d")),
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
          QStringLiteral("semiconductor/polarization"), QStringLiteral("semiconductor/traps"), QStringLiteral("semiconductor/surfacerec"),
          QStringLiteral("semiconductor/fn"), QStringLiteral("semiconductor/stimulated"), QStringLiteral("semiconductor/laser")},
         {QStringLiteral("semiconductor/model"), QStringLiteral("semiconductor/insulation")}, false, false, QStringLiteral("semi"),
         tr("Drift and diffusion of electrons and holes: Poisson's equation for the potential V, continuity for the carriers n and "
            "p (Boltzmann's statistics, or Fermi-Dirac's; band gap narrowing, dopants ionized in part, if the material model asks), "
            "their currents Scharfetter and Gummel's along each edge of linear triangles - across heterojunctions too, the bands' "
            "offsets by Anderson's rule (χ, Eg), a node on one the narrower gap's. Mobilities by doping, at an insulator's surface (Lombardi) and saturated, warmed "
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
          choice(QStringLiteral("mobility"), tr("Mobility"), {QStringLiteral("constant"), QStringLiteral("doping"), QStringLiteral("masetti")},
                 {tr("Constant (the lattice's)"), tr("By doping (Caughey and Thomas)"), tr("By doping (Masetti)")}, QStringLiteral("constant"),
                 tr("By doping: µmin + (µ - µmin)/(1 + (N/Nref)^α), N the donors and acceptors together; Masetti's µmin1 e^(-Pc/N) + "
                    "(µ - µmin2)/(1 + (N/Cr)^α) - µ1/(1 + (Cs/N)^β) - silicon's arsenic and boron (Masetti, Severi and Solmi "
                    "1983) by default; Lumerical's Masetti is this with µmin1 = µmin2 and Pc 0")),
          shownIf(expr(QStringLiteral("munmin"), tr("Electrons' µmin"), QStringLiteral("m^2/(V*s)"), QStringLiteral("68.5[cm^2/(V*s)]")),
                  QStringLiteral("mobility=doping")),
          shownIf(expr(QStringLiteral("nrefn"), tr("Electrons' Nref"), QStringLiteral("1/m^3"), QStringLiteral("9.2e16[1/cm^3]")), QStringLiteral("mobility=doping")),
          shownIf(expr(QStringLiteral("alphan"), tr("Electrons' α"), QString(), QStringLiteral("0.711")), QStringLiteral("mobility=doping")),
          shownIf(expr(QStringLiteral("mupmin"), tr("Holes' µmin"), QStringLiteral("m^2/(V*s)"), QStringLiteral("44.9[cm^2/(V*s)]")),
                  QStringLiteral("mobility=doping")),
          shownIf(expr(QStringLiteral("nrefp"), tr("Holes' Nref"), QStringLiteral("1/m^3"), QStringLiteral("2.23e17[1/cm^3]")), QStringLiteral("mobility=doping")),
          shownIf(expr(QStringLiteral("alphap"), tr("Holes' α"), QString(), QStringLiteral("0.719")), QStringLiteral("mobility=doping")),
          shownIf(expr(QStringLiteral("m1n"), tr("Electrons' µmin1"), QStringLiteral("m^2/(V*s)"), QStringLiteral("52.2[cm^2/(V*s)]")),
                  QStringLiteral("mobility=masetti")),
          shownIf(expr(QStringLiteral("m2n"), tr("Electrons' µmin2"), QStringLiteral("m^2/(V*s)"), QStringLiteral("52.2[cm^2/(V*s)]")),
                  QStringLiteral("mobility=masetti")),
          shownIf(expr(QStringLiteral("mu1n"), tr("Electrons' µ1"), QStringLiteral("m^2/(V*s)"), QStringLiteral("43.4[cm^2/(V*s)]")),
                  QStringLiteral("mobility=masetti")),
          shownIf(expr(QStringLiteral("pcn"), tr("Electrons' Pc"), QStringLiteral("1/m^3"), QStringLiteral("0[1/cm^3]")), QStringLiteral("mobility=masetti")),
          shownIf(expr(QStringLiteral("crn"), tr("Electrons' Cr"), QStringLiteral("1/m^3"), QStringLiteral("9.68e16[1/cm^3]")), QStringLiteral("mobility=masetti")),
          shownIf(expr(QStringLiteral("csn"), tr("Electrons' Cs"), QStringLiteral("1/m^3"), QStringLiteral("3.43e20[1/cm^3]")), QStringLiteral("mobility=masetti")),
          shownIf(expr(QStringLiteral("man"), tr("Electrons' α"), QString(), QStringLiteral("0.68")), QStringLiteral("mobility=masetti")),
          shownIf(expr(QStringLiteral("mbn"), tr("Electrons' β"), QString(), QStringLiteral("2")), QStringLiteral("mobility=masetti")),
          shownIf(expr(QStringLiteral("m1p"), tr("Holes' µmin1"), QStringLiteral("m^2/(V*s)"), QStringLiteral("44.9[cm^2/(V*s)]")),
                  QStringLiteral("mobility=masetti")),
          shownIf(expr(QStringLiteral("m2p"), tr("Holes' µmin2"), QStringLiteral("m^2/(V*s)"), QStringLiteral("0[cm^2/(V*s)]")),
                  QStringLiteral("mobility=masetti")),
          shownIf(expr(QStringLiteral("mu1p"), tr("Holes' µ1"), QStringLiteral("m^2/(V*s)"), QStringLiteral("29.0[cm^2/(V*s)]")),
                  QStringLiteral("mobility=masetti")),
          shownIf(expr(QStringLiteral("pcp"), tr("Holes' Pc"), QStringLiteral("1/m^3"), QStringLiteral("9.23e16[1/cm^3]")), QStringLiteral("mobility=masetti")),
          shownIf(expr(QStringLiteral("crp"), tr("Holes' Cr"), QStringLiteral("1/m^3"), QStringLiteral("2.23e17[1/cm^3]")), QStringLiteral("mobility=masetti")),
          shownIf(expr(QStringLiteral("csp"), tr("Holes' Cs"), QStringLiteral("1/m^3"), QStringLiteral("6.1e20[1/cm^3]")), QStringLiteral("mobility=masetti")),
          shownIf(expr(QStringLiteral("map"), tr("Holes' α"), QString(), QStringLiteral("0.719")), QStringLiteral("mobility=masetti")),
          shownIf(expr(QStringLiteral("mbp"), tr("Holes' β"), QString(), QStringLiteral("2")), QStringLiteral("mobility=masetti")),
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
          shownIf(expr(QStringLiteral("betan"), tr("Electrons' saturation exponent β"), QString(), QStringLiteral("2"),
                       tr("µ0 / (1 + (µ0 F/vsat)^β)^(1/β): 2 for silicon's electrons, 1 for its holes (Caughey and Thomas)")),
                  QStringLiteral("saturation=true")),
          shownIf(expr(QStringLiteral("betap"), tr("Holes' saturation exponent β"), QString(), QStringLiteral("1")), QStringLiteral("saturation=true")),
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
            "given; ni = √(Nc Nv) exp(-Eg/2kT). Its models' parameters (Caughey and Thomas's, Masetti's, ζ, saturation, "
            "narrowing) each as set here, else its material's - the library's semiconductors each carry their own (GaAs's, InP's: "
            "Lumerical's database's) -, else the default shown, silicon's: said in the log where the material is no silicon. Its "
            "material's texts read T, the physics' temperature (the library's gap Varshni's, Nc and Nv (T/300 K)^1.5, mobilities "
            "(T/300 K)^-ζ).")});
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
          shownIf(spatial(QStringLiteral("taun"), tr("Electron lifetime τn"), QStringLiteral("s"), QStringLiteral("10[us]"),
                          tr("Of x and y too (a lifetime map); τ0 when it depends on the doping")),
                  QStringLiteral("source=user")),
          shownIf(spatial(QStringLiteral("taup"), tr("Hole lifetime τp"), QStringLiteral("s"), QStringLiteral("10[us]")), QStringLiteral("source=user")),
          expr(QStringLiteral("Et"), tr("Trap level above the intrinsic level"), QStringLiteral("eV"), QStringLiteral("0[eV]")),
          choice(QStringLiteral("dependence"), tr("Lifetimes' doping dependence"), {QStringLiteral("none"), QStringLiteral("doping")},
                 {tr("None"), tr("By doping (Fossum)")}, QStringLiteral("none"),
                 tr("τ0 / (A + B (N/N0) + C (N/N0)^E), N the donors and acceptors together - Fossum and Lee's silicon τ0/(1 + "
                    "N/7.1e15 cm⁻³) by default: A 1, B 0, C 1, E 1 (Lumerical's α, β, γ, σ)")),
          shownIf(expr(QStringLiteral("N0n"), tr("Electrons' N0"), QStringLiteral("1/m^3"), QStringLiteral("7.1e15[1/cm^3]")),
                  QStringLiteral("dependence=doping")),
          shownIf(expr(QStringLiteral("N0p"), tr("Holes' N0"), QStringLiteral("1/m^3"), QStringLiteral("7.1e15[1/cm^3]")),
                  QStringLiteral("dependence=doping")),
          shownIf(expr(QStringLiteral("fA"), tr("A"), QString(), QStringLiteral("1")), QStringLiteral("dependence=doping")),
          shownIf(expr(QStringLiteral("fB"), tr("B"), QString(), QStringLiteral("0")), QStringLiteral("dependence=doping")),
          shownIf(expr(QStringLiteral("fC"), tr("C"), QString(), QStringLiteral("1")), QStringLiteral("dependence=doping")),
          shownIf(expr(QStringLiteral("fE"), tr("E"), QString(), QStringLiteral("1")), QStringLiteral("dependence=doping")),
          flag(QStringLiteral("tat"), tr("Trap-assisted tunnelling (Hurkx)"), false,
               tr("A strong field lets the carriers tunnel to the trap: each lifetime τ/(1 + Γ), Γ = 2√(3π) (F/FΓ) e^((F/FΓ)²), "
                  "FΓ = √(24 m* (kT)³)/(q ħ), F the field at the node (Hurkx, Klaassen and Knuvers 1992) - a reverse-biased "
                  "junction's leakage, an APD's dark current")),
          shownIf(expr(QStringLiteral("mtn"), tr("Electrons' tunnelling mass (m0)"), QString(), QStringLiteral("0.25")), QStringLiteral("tat=true")),
          shownIf(expr(QStringLiteral("mtp"), tr("Holes' tunnelling mass (m0)"), QString(), QStringLiteral("0.25")), QStringLiteral("tat=true"))},
         {}, {}, false, false, QStringLiteral("srh"),
         tr("Shockley-Read-Hall: R = (np - ni²) / (τp (n + n1) + τn (p + p1)); the lifetimes of x and y, by doping (Fossum), and "
            "the field's trap-assisted tunnelling (Hurkx) if asked. Fossum's and the tunnelling masses as set here, else the "
            "material's, else silicon's defaults.")});
    add({QStringLiteral("semiconductor/auger"), tr("Auger Recombination"), QStringLiteral("semi"), QStringLiteral("domainfeature"), Level::Domain,
         {selectionProp(none), expr(QStringLiteral("Cn"), tr("Electrons' coefficient Cn"), QStringLiteral("m^6/s"), QStringLiteral("2.8e-31[cm^6/s]")),
          expr(QStringLiteral("Cp"), tr("Holes' coefficient Cp"), QStringLiteral("m^6/s"), QStringLiteral("9.9e-32[cm^6/s]"))},
         {}, {}, false, false, QStringLiteral("aug"),
         tr("R = (Cn n + Cp p)(np - ni²): heavy doping, high injection. Cn and Cp as set here, else the material's (caun, caup: "
            "GaAs 1e-31 and 2.28e-31 cm⁶/s, germanium 1e-30), else silicon's.")});
    add({QStringLiteral("semiconductor/radiative"), tr("Direct Recombination"), QStringLiteral("semi"), QStringLiteral("domainfeature"),
         Level::Domain,
         {selectionProp(none), expr(QStringLiteral("B"), tr("Coefficient B"), QStringLiteral("m^3/s"), QStringLiteral("1.1e-14[cm^3/s]"))},
         {}, {}, false, false, QStringLiteral("rad"),
         tr("Band to band: R = B (np - ni²) (GaAs about 7e-10 cm³/s). B as set here, else the material's (copt), else silicon's.")});
    {
        const QString user = QStringLiteral("model=user");
        const QString perCm = QStringLiteral("1/m"), fieldUnit = QStringLiteral("V/m");
        add({QStringLiteral("semiconductor/impact"), tr("Impact Ionization"), QStringLiteral("semi"), QStringLiteral("domainfeature"),
             Level::Domain,
             {selectionProp(none),
              choice(QStringLiteral("model"), tr("Model"), {QStringLiteral("material"), QStringLiteral("silicon"), QStringLiteral("user")},
                     {tr("The material's"), tr("Silicon (van Overstraeten and de Man)"), tr("User-defined (Chynoweth's)")},
                     QStringLiteral("material"),
                     tr("The material's: its coefficients (iian, iibn, iiap, iibp, iibetan, iibetap - the library's: Lumerical's "
                        "Selberherr ones; silicon's van Overstraeten and de Man's two ranges), else silicon's")),
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
            "point stands for the tunnelling path's. A, B, γ as set here, else the material's (bbtA, bbtB, bbtgamma: germanium "
            "9.1e16, 4.9e6 V/cm), else silicon's.")});
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
          expr(QStringLiteral("phiB"), tr("Barrier height"), QStringLiteral("eV"), QStringLiteral("0.8[eV]"),
               tr("The metal's work function less χ: unset, the metal domain's beside it (its material's φ: gold 5.1 eV, aluminium "
                  "4.28 eV) less the semiconductor's χ, else 0.8 eV")),
          expr(QStringLiteral("vn"), tr("Electrons' recombination velocity"), QStringLiteral("m/s"), QStringLiteral("1e7[cm/s]"),
               tr("A* T² / (q Nc): Richardson's")),
          expr(QStringLiteral("vp"), tr("Holes' recombination velocity"), QStringLiteral("m/s"), QStringLiteral("1e7[cm/s]"))},
         {}, {}, false, false, QStringLiteral("sc"), tr("A metal on the semiconductor with a barrier: thermionic emission over it.")});
    add({QStringLiteral("semiconductor/gate"), tr("Gate Contact"), QStringLiteral("semi"), QStringLiteral("terminal"), Level::Boundary,
         {selectionProp(none), text(QStringLiteral("name"), tr("Contact name"), QStringLiteral("g")),
          expr(QStringLiteral("V0"), tr("Voltage"), QStringLiteral("V"), QStringLiteral("0[V]")),
          expr(QStringLiteral("phim"), tr("Metal work function"), QStringLiteral("eV"), QStringLiteral("4.05[eV]"),
               tr("n⁺ polysilicon 4.05 eV, aluminium 4.28 eV, p⁺ polysilicon 5.17 eV. Unset: the metal domain's beside it (its "
                  "material's φ), else 4.05 eV"))},
         {}, {}, false, false, QStringLiteral("gc"),
         tr("A metal on an insulator (the gate oxide drawn as a domain): its potential V0 less the work function's difference. "
            "Its charge semi.Q_<name>; small-signal, its capacitance.")});
    add({QStringLiteral("semiconductor/charge"), tr("Surface Charge Density"), QStringLiteral("semi"), QStringLiteral("boundaryfeature"),
         Level::Boundary,
         {selectionProp(none), spatial(QStringLiteral("rhos"), tr("Surface charge density"), QStringLiteral("C/m^2"), QStringLiteral("0"),
                                       tr("Fixed charge at an interface: an oxide's, q times 1e10 to 1e11 per cm²"))},
         {}, {}, false, false, QStringLiteral("scd"), tr("Charge fixed on a boundary.")});
    add({QStringLiteral("semiconductor/polarization"), tr("Polarization Charge (wurtzite)"), QStringLiteral("semi"), QStringLiteral("boundaryfeature"),
         Level::Boundary,
         {selectionProp(none),
          choice(QStringLiteral("axis"), tr("c axis along"), {QStringLiteral("y"), QStringLiteral("x")}, {tr("y"), tr("x")}, QStringLiteral("y")),
          choice(QStringLiteral("polarity"), tr("Growth face"), {QStringLiteral("metal"), QStringLiteral("nitrogen")},
                 {tr("Metal (Ga) face: c along the axis"), tr("Nitrogen face: c against it")}, QStringLiteral("metal")),
          text(QStringLiteral("substrate"), tr("Substrate (strain)"), QString(),
               tr("The layers' lattice in the c-plane: a nitride material or compound (GaN, AlGaN...: x its material's) or a "
                  "lattice constant (0.3189[nm]); empty: relaxed"))},
         {}, {}, false, false, QStringLiteral("pol"),
         tr("Wurtzite's bound charge at the interfaces between nitrides (their materials' compounds): -ΔP·n, each side's P = Psp + "
            "e31 (εxx + εyy) + e33 εzz (Vurgaftman and Meyer's, Bernardini and Fiorentini's) - an AlGaN barrier on GaN's 2DEG, an "
            "LED's or laser's wells' field. The interfaces straight.")});
    add({QStringLiteral("semiconductor/surfacerec"), tr("Surface Recombination"), QStringLiteral("semi"), QStringLiteral("boundaryfeature"),
         Level::Boundary,
         {selectionProp(none),
          expr(QStringLiteral("Sn"), tr("Electrons' recombination velocity Sn"), QStringLiteral("m/s"), QStringLiteral("100[cm/s]")),
          expr(QStringLiteral("Sp"), tr("Holes' recombination velocity Sp"), QStringLiteral("m/s"), QStringLiteral("100[cm/s]")),
          expr(QStringLiteral("Et"), tr("Trap level above the intrinsic level"), QStringLiteral("eV"), QStringLiteral("0[eV]"))},
         {}, {}, false, false, QStringLiteral("srec"),
         tr("Pairs recombined at an interface - a semiconductor's with an insulator or another semiconductor: U = (n p - ni²) / ((n + "
            "n1)/Sp + (p + p1)/Sn) per area, its traps' charge none (Interface Traps for a charged one). Silicon's surface under "
            "oxide 1-1e4 cm/s; at an ohmic contact the carriers are at equilibrium already.")});
    add({QStringLiteral("semiconductor/stimulated"), tr("Stimulated Recombination"), QStringLiteral("semi"), QStringLiteral("domainfeature"),
         Level::Domain,
         {selectionProp(none),
          expr(QStringLiteral("c"), tr("Share above threshold c"), QString(), QStringLiteral("0.5"),
               tr("Of the recombination current above threshold, what is stimulated (0 to 1): an L-I curve's slope")),
          expr(QStringLiteral("Jth"), tr("Threshold current density"), QStringLiteral("A/m^2"), QStringLiteral("1e7[A/m^2]"),
               tr("The domains' recombination current per area at threshold - a laser model's (MQW gain, its cavity's losses)")),
          expr(QStringLiteral("t"), tr("Gain region's thickness"), QStringLiteral("m"), QStringLiteral("10[nm]"),
               tr("Its domains' (their volume over their area: per area the current is q t ⟨R⟩)"))},
         {}, {}, false, false, QStringLiteral("stim"),
         tr("A laser's light in a carrier model, Lumerical's: above threshold a share c of the recombination current is "
            "stimulated, R_stim = c (J_rec - J_th)/(q t) on its domains, J_rec = q t ⟨R⟩ the domains' recombination (SRH, Auger, "
            "radiative) per area - the carriers clamped, the rest light: semi.Istim, the stimulated current (A, by the norm "
            "length), its photons' rate semi.Istim/q. The domains' mean taken from the last iterate (Newton's otherwise). c, "
            "J_th, t as set here, else the material's (cstim, jthreshold, tgain).")});
    add({QStringLiteral("semiconductor/laser"), tr("Lasing (Cavity Mode)"), QStringLiteral("semi"), QStringLiteral("domainfeature"),
         Level::Domain,
         {selectionProp(none),
          text(QStringLiteral("mode"), tr("Resonator"), QStringLiteral("emr"),
               tr("The Electromagnetic Waves, Eigenfrequency whose mode lases - the one its step kept (solved before)")),
          choice(QStringLiteral("gain"), tr("Gain"), {QStringLiteral("logarithmic"), QStringLiteral("linear"), QStringLiteral("quantum")},
                 {tr("Logarithmic: g0 ln((N + Ns)/(Ntr + Ns)), held at -10 g0 far below transparency"), tr("Linear: a (N - Ntr)"),
                  tr("Its quantum wells' (a Laser L-I step's table; held at its ends beyond Nmin and Nmax, said)")},
                 QStringLiteral("logarithmic"),
                 tr("The material gain at the mode's wavelength of the carriers N there. The logarithm's held at -10 g0 where N + Ns is "
                    "below e^-10 (Ntr + Ns): an absorption without end otherwise")),
          shownIf(text(QStringLiteral("wells"), tr("Quantum wells"), QStringLiteral("qw"), tr("The Quantum Wells physics whose spectrum's gain it is")),
                  QStringLiteral("gain=quantum")),
          shownIf(text(QStringLiteral("spectrum"), tr("Spectrum step"), QString(),
                       tr("Whose options the wells' spectrum is solved with: its states, broadening (empty: its study's first)")),
                  QStringLiteral("gain=quantum")),
          shownIf(choice(QStringLiteral("polarization"), tr("Polarization"), {QStringLiteral("te"), QStringLiteral("tm")},
                         {tr("TE: the field in the wells' plane (a VCSEL's)"), tr("TM: along their axis")}, QStringLiteral("te")),
                  QStringLiteral("gain=quantum")),
          shownIf(expr(QStringLiteral("Nmin"), tr("Densities from"), QStringLiteral("1/m^3"), QStringLiteral("1e17[1/cm^3]")), QStringLiteral("gain=quantum")),
          shownIf(expr(QStringLiteral("Nmax"), tr("Densities to"), QStringLiteral("1/m^3"), QStringLiteral("2e19[1/cm^3]")), QStringLiteral("gain=quantum")),
          shownIf(integer(QStringLiteral("points"), tr("Densities"), 25, tr("Of the table, by their logarithm")), QStringLiteral("gain=quantum")),
          shownIf(expr(QStringLiteral("g0"), tr("Gain coefficient g0"), QStringLiteral("1/m"), QStringLiteral("2000[1/cm]")),
                  QStringLiteral("gain=logarithmic")),
          shownIf(expr(QStringLiteral("Ns"), tr("Shift Ns"), QStringLiteral("1/m^3"), QStringLiteral("0[1/cm^3]")), QStringLiteral("gain=logarithmic")),
          shownIf(expr(QStringLiteral("a"), tr("Differential gain a"), QStringLiteral("m^2"), QStringLiteral("5e-16[cm^2]")), QStringLiteral("gain=linear")),
          shownIf(expr(QStringLiteral("Ntr"), tr("Transparency density Ntr"), QStringLiteral("1/m^3"), QStringLiteral("2e18[1/cm^3]")),
                  QStringLiteral("gain!=quantum")),
          choice(QStringLiteral("carrier"), tr("Carriers N"), {QStringLiteral("mean"), QStringLiteral("electrons"), QStringLiteral("holes")},
                 {tr("√(n p)"), tr("Electrons n"), tr("Holes p")}, QStringLiteral("mean")),
          expr(QStringLiteral("beta"), tr("Spontaneous emission factor β"), QString(), QStringLiteral("1e-4"),
               tr("Of the radiative recombination on its domains, the share into the mode")),
          expr(QStringLiteral("ng"), tr("Group index"), QString(), QString(),
               tr("Of the gain region: the photons' velocity c/ng there (empty: the resonator's index there, √Re ε)")),
          expr(QStringLiteral("eta"), tr("Output share η"), QString(), QStringLiteral("1"),
               tr("Of the mode's loss rate ω/Q, the share that is light out (the rest absorbed, or out the other way)"))},
         {}, {}, false, false, QStringLiteral("las"),
         tr("A laser: the photons S of a resonator's mode (the one its Eigenfrequency step kept) solved with the carriers - "
            "their balance (G - 1/τp) S + β Rsp = 0, τp = Q/ω the cold cavity's photon lifetime, G = Σ v g(N) Π the modal "
            "gain rate (g the material gain of the carriers N at each node of its domains, v = c/ng, Π each node's share of the "
            "mode's energy), Rsp the radiative recombination on its domains (its material's B: a Radiative Recombination "
            "there) - and the stimulated recombination v g(N) S Π at each node taken from its carriers: below threshold S "
            "spontaneous, above it the carriers clamped and the current light. semi.S, semi.Pout (η ħω S/τp, W), semi.G and "
            "semi.Gnet = G - 1/τp (1/s), semi.Gamma (Σ Π), semi.gth (the material gain at threshold, 1/(τp v Γ)), semi.Istim "
            "(q, the stimulated recombination), semi.Isp (q, the spontaneous emission's on its domains), semi.Nact (the "
            "carriers' mean on its domains), semi.gmean (the gain's mean by the mode's energy), semi.tauph (τp, the photon "
            "lifetime; semi.taup is the holes' SRH lifetime); at each point "
            "semi.gain and semi.Rst. Stationary steps, and a Laser L-I step's bias points.")});
    add({QStringLiteral("semiconductor/fn"), tr("Fowler-Nordheim Tunnelling"), QStringLiteral("semi"), QStringLiteral("boundaryfeature"),
         Level::Boundary,
         {selectionProp(none),
          expr(QStringLiteral("phiB"), tr("Barrier φB"), QStringLiteral("eV"), QStringLiteral("3.1[eV]"), tr("Silicon's conduction band to silicon dioxide's")),
          expr(QStringLiteral("mox"), tr("Mass in the insulator (m0)"), QString(), QStringLiteral("0.42"))},
         {}, {}, false, false, QStringLiteral("fn"),
         tr("Electrons tunnelling through an insulator in a strong field - a gate oxide's leakage, a flash cell's writing: J = A "
            "E² e^(-B/E), A = q³ m0/(8π h φB m_ox), B = 8π √(2 m_ox) φB^(3/2)/(3 q h) (Lenzlinger and Snow; SiO₂ on silicon about "
            "1.2e-6 A/V² and 240 MV/cm), E the barrier's mean field - φB over the depth into the insulator at which the potential has "
            "moved by φB/q, along each boundary edge's normal (or its change over a thinner insulator: tunnelling direct): semi.Ifn, "
            "its current after the solve - not taken from the carriers (a leakage far below the device's own currents).")});
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
          choice(QStringLiteral("polarization"), tr("Polarization (wurtzite)"), {QStringLiteral("none"), QStringLiteral("periodic"), QStringLiteral("ends")},
                 {tr("None"), tr("Periodic (no drop across the stack)"), tr("No field at its ends")}, QStringLiteral("none"),
                 tr("A wurtzite stack's spontaneous and piezoelectric polarization (Psp + e31 (εxx + εyy) + e33 εzz, c along the axis): "
                    "its interfaces' bound charges -ΔP make each layer's field F = (D - P)/ε0εr, D such that the potential drops nothing "
                    "across the physics (an MQW's period: a well's Fw = (Pb - Pw) Lb/(εw Lb + εb Lw)) or that its first layer has no field "
                    "(thick barriers). A spectrum's carriers screen it: solved with the states in turn (1D)")),
          choice(QStringLiteral("polarity"), tr("Growth face"), {QStringLiteral("metal"), QStringLiteral("nitrogen")},
                 {tr("Metal (Ga) face: c along the axis"), tr("Nitrogen face: c against it")}, QStringLiteral("metal")),
          spatial(QStringLiteral("V"), tr("Electric potential"), QStringLiteral("V"), QStringLiteral("0"),
                  tr("Another physics' (semi.V, es.V): the bands move by -q V")),
          spatial(QStringLiteral("etan"), tr("Electrons' Fermi level above the band, Efn - Ec"), QStringLiteral("J"), QString(),
                  tr("Where the electrons' quasi-Fermi level is: a semiconductor's semi.Efn - semi.Ec. The states found filled to it give "
                     "qw.n - a Schrödinger-Poisson loop's. Empty: none counted")),
          spatial(QStringLiteral("etap"), tr("Holes' Fermi level below the band, Ev - Efp"), QStringLiteral("J"), QString(),
                  tr("A semiconductor's semi.Ev - semi.Efp: qw.p, the holes in the states found. Empty: none counted")),
          choice(QStringLiteral("valence"), tr("Valence bands"),
                 {QStringLiteral("parabolic"), QStringLiteral("kp4"), QStringLiteral("kp6"), QStringLiteral("kp8")},
                 {tr("Parabolic heavy and light holes"), tr("k·p 4x4 (Luttinger-Kohn)"), tr("k·p 6x6 (and the split-off band)"),
                  tr("k·p 8x8 (Kane's: the conduction band too)")},
                 QStringLiteral("parabolic"),
                 tr("A spectrum's holes (1D wells): each parabolic in the plane, or the k·p bands - heavy and light holes mixed as "
                    "the in-plane k grows (γ1, γ2, γ3; the 6x6 the split-off band too, Δ), solved at each k: the gain's width and "
                    "peak, the TE/TM ratio of strained wells. The 8x8 the electrons too, coupled to them by Kane's P (Ep): "
                    "their non-parabolic bands and masses - narrow gaps (InAs, InSb) -, the remote bands' γ and the conduction "
                    "band's term what is left (Foreman's), Ep lowered where that term would be below a free electron's (spurious states). A "
                    "Valence Bands step gives their dispersion (the 8x8's conduction states' too, qw.Ec1...)")),
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
          shownIf(choice(QStringLiteral("crystal"), tr("Crystal"), {QStringLiteral("zincblende"), QStringLiteral("wurtzite")},
                         {tr("Zinc blende"), tr("Wurtzite (c along the axis)")}, QStringLiteral("zincblende"),
                         tr("Wurtzite: Chuang and Chang's 6x6 (the physics' Valence bands k·p 6x6), its holes' masses those of A1-A6; Ev the "
                            "k·p's reference - the heavy holes' top Ev + Δcr + Δso/3 unstrained")),
                  QStringLiteral("source=user")),
          shownIf(text(QStringLiteral("A"), tr("A1-A6 (ħ²/2m0)"), QString(), tr("Six numbers: GaN's -7.21, -0.44, 6.68, -3.46, -3.40, -4.90")),
                  QStringLiteral("source=user&crystal=wurtzite")),
          shownIf(expr(QStringLiteral("Dcr"), tr("Crystal field Δcr"), QStringLiteral("eV"), QStringLiteral("0.010[eV]")),
                  QStringLiteral("source=user&crystal=wurtzite")),
          shownIf(expr(QStringLiteral("Dso"), tr("Spin-orbit Δso"), QStringLiteral("eV"), QStringLiteral("0.017[eV]")),
                  QStringLiteral("source=user&crystal=wurtzite")),
          shownIf(expr(QStringLiteral("mep"), tr("Electron mass across the axis (m0)"), QString(), QString(), tr("Empty: as along it")),
                  QStringLiteral("source=user&crystal=wurtzite")),
          shownIf(expr(QStringLiteral("Epz"), tr("Kane energy along the axis"), QStringLiteral("eV"), QString(),
                       tr("Ep∥: TM's (the Z orbital's); Ep TE's. Empty: Ep")),
                  QStringLiteral("source=user&crystal=wurtzite")),
          shownIf(expr(QStringLiteral("exx"), tr("Strain in the plane εxx"), QString(), QString(),
                       tr("Given (the layer's on its substrate's lattice, (a_s - a)/a). Empty: none")),
                  QStringLiteral("source=user")),
          shownIf(expr(QStringLiteral("ac"), tr("Conduction band's deformation potential ac"), QStringLiteral("eV"), QStringLiteral("-7.17[eV]"),
                       tr("With εxx: Bir and Pikus's, as a compound's on a substrate (GaAs's)")),
                  QStringLiteral("source=user&crystal=zincblende")),
          shownIf(expr(QStringLiteral("av"), tr("Valence band's av"), QStringLiteral("eV"), QStringLiteral("1.16[eV]")),
                  QStringLiteral("source=user&crystal=zincblende")),
          shownIf(expr(QStringLiteral("b"), tr("Shear deformation potential b"), QStringLiteral("eV"), QStringLiteral("-2.0[eV]")),
                  QStringLiteral("source=user&crystal=zincblende")),
          shownIf(expr(QStringLiteral("C11"), tr("Stiffness C11"), QStringLiteral("Pa"), QStringLiteral("122.1[GPa]")),
                  QStringLiteral("source=user&crystal=zincblende")),
          shownIf(expr(QStringLiteral("C12"), tr("Stiffness C12"), QStringLiteral("Pa"), QStringLiteral("56.6[GPa]")),
                  QStringLiteral("source=user&crystal=zincblende")),
          shownIf(expr(QStringLiteral("ezz"), tr("Strain along the axis εzz"), QString(), QString(), tr("Empty: -2 C13/C33 εxx")),
                  QStringLiteral("source=user&crystal=wurtzite")),
          shownIf(expr(QStringLiteral("C13"), tr("Stiffness C13"), QStringLiteral("Pa"), QStringLiteral("106[GPa]")),
                  QStringLiteral("source=user&crystal=wurtzite")),
          shownIf(expr(QStringLiteral("C33"), tr("Stiffness C33"), QStringLiteral("Pa"), QStringLiteral("398[GPa]")),
                  QStringLiteral("source=user&crystal=wurtzite")),
          shownIf(text(QStringLiteral("D"), tr("D1-D6 (eV)"), QString(),
                       tr("The valence bands' deformation potentials: six numbers (GaN's -3.7, 4.5, 8.2, -4.1, -4.0, -5.5). Empty: strain moves "
                          "them not")),
                  QStringLiteral("source=user&crystal=wurtzite")),
          shownIf(expr(QStringLiteral("a1"), tr("Conduction band's a1 (along c)"), QStringLiteral("eV"), QString(),
                       tr("Ec moves by a1 εzz + a2 (εxx + εyy). Empty: 0")),
                  QStringLiteral("source=user&crystal=wurtzite")),
          shownIf(expr(QStringLiteral("a2"), tr("Conduction band's a2 (across c)"), QStringLiteral("eV"), QString(), tr("Empty: 0")),
                  QStringLiteral("source=user&crystal=wurtzite")),
          shownIf(expr(QStringLiteral("Psp"), tr("Spontaneous polarization Psp"), QStringLiteral("C/m^2"), QString(),
                       tr("Along c (the metal face's: GaN -0.034). Empty: 0")),
                  QStringLiteral("source=user&crystal=wurtzite")),
          shownIf(expr(QStringLiteral("e31"), tr("Piezoelectric e31"), QStringLiteral("C/m^2"), QString(), tr("Empty: 0")),
                  QStringLiteral("source=user&crystal=wurtzite")),
          shownIf(expr(QStringLiteral("e33"), tr("Piezoelectric e33"), QStringLiteral("C/m^2"), QString(), tr("Empty: 0")),
                  QStringLiteral("source=user&crystal=wurtzite")),
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
                "method; linear in its unknowns (what multiplies them free of them: a wave's, diffusion's), solved at once, "
                "its matrix factored once while a time step stays (each step one solve; again each step where what "
                "multiplies them varies in time). On the mesh (2D, about an axis too) or along a line (1D: an ODE). Dirichlet, "
                "flux (Robin; g and q of the variables and their rates too - an absorbing boundary's g = -ut/c) and zero flux "
                "conditions; at rest, in time or its eigenvalues.")});
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
         {QStringLiteral("mesh/size"), QStringLiteral("mesh/distribution"), QStringLiteral("mesh/layered")}, {}, true, true, QString(),
         tr("Triangles, Delaunay, refined till their angles are good and their sizes as asked.")});
    add({QStringLiteral("mesh/size"), tr("Size"), QStringLiteral("mesh"), QStringLiteral("meshsize"), Level::Domain,
         {choice(QStringLiteral("level"), tr("Level"), {QStringLiteral("domain"), QStringLiteral("boundary"), QStringLiteral("point")},
                 {tr("Domains"), tr("Boundaries"), tr("Points")}, QStringLiteral("domain")),
          selectionProp(none), expr(QStringLiteral("hmax"), tr("Maximum element size"), QStringLiteral("length"), QStringLiteral("0.1"))},
         {}, {}, false, false, QStringLiteral("size"), tr("Smaller elements where it says.")});
    add({QStringLiteral("mesh/distribution"), tr("Distribution"), QStringLiteral("mesh"), QStringLiteral("meshdistribution"),
         Level::Boundary, {selectionProp(none), integer(QStringLiteral("count"), tr("Number of elements"), 10)},
         {}, {}, false, false, QStringLiteral("dis"), tr("A number of elements along boundaries.")});
    add({QStringLiteral("mesh/layered"), tr("Layered Region"), QStringLiteral("mesh"), QStringLiteral("meshlayered"), Level::Domain,
         {selectionProp(none),
          choice(QStringLiteral("axis"), tr("Layers stacked along"), {QStringLiteral("y"), QStringLiteral("x")}, {tr("y"), tr("x")}, QStringLiteral("y")),
          expr(QStringLiteral("hlateral"), tr("Lateral element size"), QStringLiteral("length"), QString(),
               tr("Along the layers (empty: the maximum element size)")),
          integer(QStringLiteral("count"), tr("Elements across each layer"), 2)},
         {}, {}, false, false, QStringLiteral("lay"),
         tr("A stack of thin layers - an MQW's wells and barriers, each a rectangle - as a grid of stretched triangles: so many "
            "across each layer, the lateral size along them; not refined, its sides never split. Forty 6 nm layers under a 2 µm "
            "mesa some 6400 triangles, not the 200 000 isotropic ones they force.")});

    // Studies.
    add({QStringLiteral("study"), tr("Study"), QStringLiteral("study"), QStringLiteral("study"), Level::None, {},
         {QStringLiteral("stationary"), QStringLiteral("transient"), QStringLiteral("frequency"), QStringLiteral("mode"), QStringLiteral("states"),
          QStringLiteral("spectrum"), QStringLiteral("bands"), QStringLiteral("eigenvalue"), QStringLiteral("eigenfrequency"), QStringLiteral("laser"),
          QStringLiteral("sweep")},
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
              choice(QStringLiteral("method"), tr("Method"), {QStringLiteral("bdf2"), QStringLiteral("euler"), QStringLiteral("midpoint")},
                     {tr("BDF of order 2"), tr("Backward Euler (order 1)"), tr("Implicit midpoint (order 2, no damping; fixed steps)")},
                     QStringLiteral("bdf2"),
                     tr("BDF damps what the steps resolve poorly - a wave loses some of its amplitude each period; the implicit "
                        "midpoint rule keeps a wave's energy (an undamped one exactly) but does not damp a stiff start either: for "
                        "waves, with fixed steps")),
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
                     tr("Those of one polarization alone - by emw.TEfraction, of the transverse |E|² the part along x: TE at the "
                        "fraction below or more, TM at 1 less it or less. A metal or magnetic wall's lateral modes (a 2 µm strip's "
                        "TE01 above a slab's TM0) left out")),
              shownIf(expr(QStringLiteral("tefraction"), tr("TE fraction at least"), QString(), QStringLiteral("0.5"),
                           tr("TE: emw.TEfraction this or more; TM: 1 less this or less (0.9: a hybrid mode near a slab's left out)")),
                      QStringLiteral("polarization=te|polarization=tm")),
              expr(QStringLiteral("minneff"), tr("Effective index above"), QString(), QString(),
                   tr("Guided: those above it alone - a substrate's or cladding's index, their modes left out (empty: any)")),
              expr(QStringLiteral("maxloss"), tr("Loss below (dB/cm)"), QString(), QString(),
                   tr("Guided: those losing less alone - an absorbing layer's or a leaky mode left out (empty: any)")),
              choice(QStringLiteral("choose"), tr("Mode chosen"), {QStringLiteral("first"), QStringLiteral("share"), QStringLiteral("te"), QStringLiteral("tm")},
                     {tr("The first (highest effective index)"), tr("The most in the domains tracked"), tr("The most TE"), tr("The most TM")},
                     QStringLiteral("first"),
                     tr("Of those kept, at each frequency and sweep point: the one a plot group shows (its mode 0), a derived value "
                        "evaluates (its solutions: chosen) and the steps after are lit by")),
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
        add({QStringLiteral("eigenfrequency"), tr("Eigenfrequency"), QStringLiteral("step"), QStringLiteral("eigenfrequency"), Level::None,
             {physics, integer(QStringLiteral("count"), tr("Eigenfrequencies"), 6, tr("Those nearest what it searches about")),
              expr(QStringLiteral("around"), tr("Search about"), QString(), QStringLiteral("1[um]"),
                   tr("A free-space wavelength (980[nm]) or a frequency (306[THz]); its media taken there")),
              integer(QStringLiteral("keep"), tr("Mode kept for the steps after"), 0,
                      tr("The mode a laser's step after it lases in: its number of those found (0: the one of highest Q)")),
              expr(QStringLiteral("tolerance"), tr("Relative tolerance"), QString(), QStringLiteral("1e-6"),
                   tr("Of the physics solved before")),
              before, nonlinear},
             {}, {}, false, false, QStringLiteral("eig"),
             tr("A resonator's (Electromagnetic Waves, Eigenfrequency) modes nearest a frequency or wavelength: each a solution, "
                "by frequency - its eigenfrequency, Q and fields. Physics at rest are solved first; the steps after see the mode "
                "kept.")});
        add({QStringLiteral("laser"), tr("Laser L-I"), QStringLiteral("step"), QStringLiteral("laser"), Level::None,
             {physics,
              text(QStringLiteral("contact"), tr("Contact swept"), QStringLiteral("anode"), tr("A semiconductor's metal contact, by name")),
              text(QStringLiteral("voltages"), tr("Its voltages"), QStringLiteral("range(0, 0.05, 2)"),
                   tr("In volts, or with a unit: range(start, step, stop), linspace(start, stop, count), or values split by commas")),
              expr(QStringLiteral("tolerance"), tr("Relative tolerance"), QString(), QStringLiteral("1e-6"), tr("Of coupled physics at each point")),
              nonlinear},
             {}, {}, false, false, QStringLiteral("lis"),
             tr("A laser's light against its current: the contact held at each voltage, the physics at rest solved there - a "
                "semiconductor's Lasing in the mode an Eigenfrequency step before it kept - each point a solution (by voltage, "
                "from the last): semi.Pout, semi.S against semi.I_<contact>, its threshold where semi.Gnet reaches nought.")});
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
              integer(QStringLiteral("cstates"), tr("Conduction subbands"), 0,
                      tr("How many electron states enter the spectrum, the lowest (0: States). Lumerical's max num cb subbands")),
              integer(QStringLiteral("vstates"), tr("Valence subbands"), 0,
                      tr("How many valence states enter, the highest by energy across heavy and light holes together - with k·p "
                         "bands, each a Kramers pair, after their mixing (0: States of each kind). Lumerical's max num vb subbands: "
                         "2 keeps hh1 and lh1 where hh2 lies below lh1")),
              expr(QStringLiteral("cutoff_c"), tr("Conduction states' wall cutoff"), QString(), QString(),
                   tr("An electron state whose wavefunction's slope at the line's ends (its hard walls) is more than this share of "
                      "its largest is left out - one held against a wall by a field, not a well's. Lumerical's cutoff cb with hard "
                      "walls. Empty: none")),
              expr(QStringLiteral("cutoff_v"), tr("Valence states' wall cutoff"), QString(), QString(), tr("The same of the valence states")),
              choice(QStringLiteral("select"), tr("States by"), {QStringLiteral("energy"), QStringLiteral("localized")},
                     {tr("Energy alone"), tr("Energy, of those in the wells")}, QStringLiteral("energy"),
                     tr("Localized: the subbands counted are the lowest (highest) of the states with at least a share of their |ψ|² in "
                        "the wells and the barriers between them - not a high field's triangle against the line's end")),
              shownIf(expr(QStringLiteral("pmin"), tr("Least share in the wells"), QString(), QStringLiteral("0.6")), QStringLiteral("select=localized")),
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
              choice(QStringLiteral("broaden"), tr("Transitions broadened"), {QStringLiteral("all"), QStringLiteral("window")},
                     {tr("All"), tr("Those within the photons' window")}, QStringLiteral("all"),
                     tr("All: every transition of the states kept, its line's tail reaching every photon. The window: only the "
                        "transitions whose energy lies within the photons' energies (to the extension above), as Lumerical's MQW - "
                        "below the edge a single line's tail, not the sum over the 2D continuum")),
              shownIf(expr(QStringLiteral("window_ext"), tr("Window's extension above"), QStringLiteral("eV"), QStringLiteral("0[eV]")),
                      QStringLiteral("broaden=window")),
              orValue(choice(QStringLiteral("normalize"), tr("Absorption per"), {QStringLiteral("wells"), QStringLiteral("stack"), QStringLiteral("length")},
                             {tr("The wells' thickness"), tr("The stack's (the line's)"), tr("A length")}, QStringLiteral("wells"),
                             tr("The wells': the material's absorption, to multiply by the wells' confinement factor. The stack's: the "
                                "whole line, its padding too - as Lumerical's MQW reports it (qw.Lstack)"))),
              shownIf(expr(QStringLiteral("normlength"), tr("Its length"), QStringLiteral("m"), QStringLiteral("100[nm]")),
                      QStringLiteral("normalize=length")),
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
                     {tr("All combinations"), tr("Specified combinations (the n-th values together)")}, QStringLiteral("all")),
              integer(QStringLiteral("parallel"), tr("Points at once"), 1,
                      tr("Points of the sweep solved at the same time, each with its own threads - for a sweep of points "
                         "independent of each other (wavelengths, angles, sources); each holds its own matrices, its memory "
                         "too. 1: one after another. A sweep whose points start from the one before (a semiconductor's, a "
                         "mode followed or tracked) is solved one after another, said."))},
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
              integer(QStringLiteral("mode"), tr("Mode"), 0,
                      tr("Of a mode analysis: which mode, from 1 (by effective index); 0 the one its step chose (the first unless it says)")),
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
        const P solutions = choice(QStringLiteral("solutions"), tr("Solutions"),
                                   {QStringLiteral("auto"), QStringLiteral("all"), QStringLiteral("selected"), QStringLiteral("chosen")},
                                   {tr("Automatic: all, but of a mode analysis whose step chose a mode that one alone"),
                                    tr("All: each output time, each parameter value, each mode"), tr("The plot group's time and parameter value"),
                                    tr("Each parameter value, the mode its step chose (a mode analysis)")},
                                   QStringLiteral("auto"));
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
                     {tr("Automatic"), tr("The sweep's parameters"), tr("The times, frequencies or modes")}, QStringLiteral("auto")),
              text(QStringLiteral("x"), tr("Against a global"), QString(),
                   tr("An expression of the globals along x, each solution's value of it, in place of its one axis (semi.I_anode: a "
                      "laser's light against its current); empty: the axis itself")),
              text(QStringLiteral("xunit"), tr("Its unit"), QString(),
                   tr("The unit its global is shown in along x (mA for semi.I_anode); empty: its own (A)")),
              choice(QStringLiteral("modes"), tr("Modes"), {QStringLiteral("auto"), QStringLiteral("all"), QStringLiteral("chosen")},
                     {tr("Automatic: the mode its step chose where it chose one, else each"), tr("Each (an axis of them)"),
                      tr("The one its step chose, at each frequency and parameter value")},
                     QStringLiteral("auto"),
                     tr("Of a mode analysis: each mode a curve, or the one its step chose (a guided TE0 against the bias, one curve)"))},
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
              QStringLiteral("minimum"), QStringLiteral("pointeval"), QStringLiteral("lineparams"), QStringLiteral("confinement"),
              QStringLiteral("perturbation")},
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
            add({QStringLiteral("perturbation"), tr("Perturbation"), QStringLiteral("derived"), QStringLiteral("perturbation"), Level::Domain,
                 {study, selectionProp(), modes,
                  spatial(QStringLiteral("dn"), tr("Δn"), QString(), QStringLiteral("emw.dncarriers"),
                          tr("The index's change: the free carriers' (emw.dncarriers), a field's, any expression - empty: none")),
                  spatial(QStringLiteral("dalpha"), tr("Δα (the power's)"), QStringLiteral("1/m"), QStringLiteral("emw.alphacarriers"),
                          tr("The absorption's change, 4πΔκ/λ: the free carriers' (emw.alphacarriers), any expression - empty: none")),
                  expr(QStringLiteral("length"), tr("Length"), QStringLiteral("m"), QString(), tr("A phase shifter's: Δφ = k0 Δneff L (empty: none)")),
                  text(QStringLiteral("bias"), tr("Bias"), QString(),
                       tr("The swept parameter (Va): each point's Δφ and VπL from the sweep's first, λ|V - V0|/2|Δneff - Δneff0|")),
                  solutions},
                 {}, {}, false, false, QStringLiteral("pert"),
                 tr("A mode's change to the first order, for each mode solved: Δneff = (c ε0/2) ∫ n Δn |E|² over ∫ Sz, and the "
                    "power's Δα = (c ε0/2) ∫ n Δα |E|² over ∫ Sz, in the domains chosen - without solving it again; along a bias "
                    "sweep its phase and VπL.")});
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
             {QStringLiteral("touchstone"), QStringLiteral("spice"), QStringLiteral("qucspart"), QStringLiteral("fieldexport"),
              QStringLiteral("lumericalmode"), QStringLiteral("lumericalcharge"), QStringLiteral("lumericalmqw")},
             {}, true, true, QString(),
             tr("Results as files and parts of circuits: a line's S-parameters (Touchstone), SPICE subcircuits, a Qucs "
                "subcircuit, fields on a grid (CSV, NumPy) - each written beside the model (model_tag.s2p, .cir, .sch, .csv); "
                "and the model as Lumerical scripts (MODE, CHARGE, MQW: model_tag.lsf) that build it there.")});
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
        {
            P which = solutions;
            which.defaultValue = QStringLiteral("selected");
            which.choiceLabels = {tr("Automatic: all, but of a mode analysis whose step chose a mode that one alone"),
                                  tr("All: each output time, each parameter value, each mode"), tr("One: its point, time or frequency, mode"),
                                  tr("Each parameter value, the mode its step chose (a mode analysis)")};
            const QString one = QStringLiteral("solutions=selected");
            add({QStringLiteral("fieldexport"), tr("Field on a Grid"), QStringLiteral("export"), QStringLiteral("fieldexport"), Level::None,
                 {study, which, shownIf(point, one), shownIf(time, one), shownIf(frequency, one),
                  shownIf(integer(QStringLiteral("mode"), tr("Mode"), 0, tr("Of a mode analysis: from 1; 0 the one its step chose")), one),
                  text(QStringLiteral("expressions"), tr("Expressions"), QStringLiteral("emw.normE"),
                       tr("Split by ';', each named if wanted: E = emw.normE; semi.n; emw.Sz - in SI")),
                  choice(QStringLiteral("where"), tr("At"), {QStringLiteral("grid"), QStringLiteral("nodes")},
                         {tr("A regular x-y grid"), tr("The mesh's nodes")}, QStringLiteral("grid")),
                  shownIf(pair(QStringLiteral("from"), tr("Lower left corner"), QStringLiteral("length"), QString(), QString(),
                               tr("In the geometry's unit; empty, both corners: the model's extent")),
                          QStringLiteral("where=grid")),
                  shownIf(pair(QStringLiteral("to"), tr("Upper right corner"), QStringLiteral("length"), QString(), QString()), QStringLiteral("where=grid")),
                  shownIf(integer(QStringLiteral("nx"), tr("Points along x"), 101), QStringLiteral("where=grid")),
                  shownIf(integer(QStringLiteral("ny"), tr("Points along y"), 101), QStringLiteral("where=grid")),
                  choice(QStringLiteral("format"), tr("Format"), {QStringLiteral("csv"), QStringLiteral("npy"), QStringLiteral("dat")},
                         {tr("CSV (a row per point: x, y, values)"), tr("NumPy .npy (a grid: [solution,] expression, y, x)"),
                          tr("Columns, # comments, a blank line after each grid row (gnuplot)")},
                         QStringLiteral("csv")),
                  file},
                 {}, {}, false, false, QStringLiteral("fx"),
                 tr("Expressions of the solution on a regular grid or at the mesh's nodes, for a contour outside Qucs-S: "
                    "CSV, NumPy or columns - x and y in the geometry's unit, the values in SI, NaN outside the domains; one "
                    "solution (a point, time, frequency or mode), each, or each point's chosen mode.")});
        }
        // The model as Lumerical scripts (no study need be computed).
        auto physicsOfType = [&](const QString& tag) {
            P p;
            p.key = QStringLiteral("physics");
            p.label = tr("Physics");
            p.kind = P::Physics;
            p.defaultValue = QJsonArray{tag};
            return p;
        };
        P lumStudy = study;
        lumStudy.tooltip = tr("Whose steps give the settings. Empty: the first with one; it need not be computed");
        const P run = flag(QStringLiteral("run"), tr("Run it there"), true, tr("The script runs the solver after it saves the project"));
        add({QStringLiteral("lumericalmode"), tr("Lumerical MODE (FDE)"), QStringLiteral("export"), QStringLiteral("lumericalmode"), Level::None,
             {physicsOfType(QStringLiteral("emw")), lumStudy,
              choice(QStringLiteral("materials"), tr("Materials"), {QStringLiteral("indices"), QStringLiteral("library")},
                     {tr("Their indices as the model has them, at the wavelength ((n,k) materials)"),
                      tr("Lumerical's own by name (Si (Silicon) - Palik...), else their indices")},
                     QStringLiteral("indices")),
              expr(QStringLiteral("step"), tr("FDE mesh step"), QStringLiteral("m"), QString(), tr("Empty: λ/(20 n) of the largest index")),
              integer(QStringLiteral("modes"), tr("Trial modes"), 0, tr("0: the mode step's")), run, file},
             {}, {}, false, false, QStringLiteral("lm"),
             tr("A Lumerical MODE script (.lsf): the cross-section a structure for each domain, each its index at the mode step's "
                "wavelength as the model has it (or Lumerical's material of that name), an FDE region over the physics' domains "
                "(2D Z normal; PML where the model has PML domains, Metal or PMC on the other sides), the wavelength, trial modes "
                "and the index searched about. Run in MODE it builds the project and saves it (model_tag.lms), then finds the "
                "modes.")});
        add({QStringLiteral("lumericalcharge"), tr("Lumerical CHARGE"), QStringLiteral("export"), QStringLiteral("lumericalcharge"), Level::None,
             {physicsOfType(QStringLiteral("semi")), lumStudy,
              expr(QStringLiteral("strip"), tr("Contacts' metal"), QStringLiteral("m"), QString(),
                   tr("The thickness of the metal strip each contact is put on, outside its boundary. Empty: 1 % of the device")),
              [&] {
                  P r = flag(QStringLiteral("run"), tr("Run it there"), false, tr("The script runs CHARGE after it saves the project"));
                  return r;
              }(),
              file},
             {}, {}, false, false, QStringLiteral("lc"),
             tr("A Lumerical CHARGE script (.lsf, the Finite Element IDE): Lumerical's electrical materials by name (alloys at "
                "their composition), the cross-section in its x-z plane (2D y-normal: the model's y its z), the doping - uniform "
                "and box profiles as constant and diffusion objects, an implant or an expression sampled into an import doping "
                "object -, each contact a thin metal strip outside its boundary at its voltage (a study's sweep a value table), "
                "the norm length the model's thickness. Run in the Finite Element IDE it builds and saves the project "
                "(model_tag.ldev).")});
        add({QStringLiteral("lumericalmqw"), tr("Lumerical MQW"), QStringLiteral("export"), QStringLiteral("lumericalmqw"), Level::None,
             {physicsOfType(QStringLiteral("qw")), lumStudy,
              choice(QStringLiteral("materials"), tr("Materials"), {QStringLiteral("model"), QStringLiteral("database")},
                     {tr("Each layer's parameters as the model has them"),
                      tr("Lumerical's database by compound and composition (its offsets), else the model's")},
                     QStringLiteral("model")),
              run, file},
             {}, {}, false, false, QStringLiteral("lq"),
             tr("A Lumerical MQW script (.lsf): the wells' stack for mqwindex (Lumerical's 4x4 k·p, zinc blende) - each layer's "
                "parameters as the model has them (its gap, Ep, me, γ1-γ3, deformation potentials, stiffness, valence band on one "
                "scale) and its strain -, the temperature and field, the spectrum step's photon frequencies, carrier density "
                "(over the stack) and broadening (a full width), the bands step's k. Run in any Lumerical product (an MQW license) it computes the stack's band structure, "
                "gain and index and saves them (model_tag_mqw.ldf; a study's fields swept, model_tag_mqw_1.ldf... each). A study's sweep of the "
                "carrier density is mqwindex's densities.")});
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
            QStringLiteral("magnetic"), QStringLiteral("modes"), QStringLiteral("waves"), QStringLiteral("resonator"), QStringLiteral("semiconductor"),
            QStringLiteral("rays"), QStringLiteral("quantum"), QStringLiteral("pde")};
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
    if (v.isArray()) {
        // A list given as JSON's - [1.2, 1.4, "1.6[V]"] -: its values, as a
        // list's text writes them. (It read as no values: a Laser L-I's
        // voltages - the update check of 110e8b9.)
        QStringList parts;
        for (const QJsonValue& e : v.toArray())
            if (e.isDouble()) parts << formatNumber(e.toDouble(), 15);
            else if (e.isString()) parts << e.toString();
        return parts.join(QLatin1String(", "));
    }
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
        } else if (f.type == QLatin1String("interpolation") && f.text(QStringLiteral("dimension")) == QLatin1String("2")) {
            // Its grid: the x's and y's of its rows, each pair's value.
            auto table = std::make_shared<Expression::Table>();
            const QStringList units = f.text(QStringLiteral("argunit")).split(QLatin1Char(','));
            const std::optional<Unit> xu = parseUnit(units.value(0).trimmed()), vu = parseUnit(units.value(1).trimmed());
            const std::optional<Unit> fu = parseUnit(f.text(QStringLiteral("unit")));
            auto number = [](const QJsonValue& v) { return v.isDouble() ? v.toDouble() : v.toString().toDouble(); };
            std::map<std::pair<double, double>, double> at;
            std::set<double> xs, ys;
            for (const QJsonValue& row : f.value(QStringLiteral("grid")).toArray()) {
                const QJsonArray r = row.toArray();
                if (r.size() < 3) continue;
                const double x = number(r.at(0)) * (xu ? xu->scale : 1) + (xu ? xu->offset : 0);
                const double y = number(r.at(1)) * (vu ? vu->scale : 1) + (vu ? vu->offset : 0);
                at[{x, y}] = number(r.at(2)) * (fu ? fu->scale : 1) + (fu ? fu->offset : 0);
                xs.insert(x);
                ys.insert(y);
            }
            table->x.assign(xs.begin(), xs.end());
            table->grid.assign(ys.begin(), ys.end());
            if (table->x.empty() || at.size() != xs.size() * ys.size()) {
                ps->errors.insert(name, tr("%1: its table is not a grid of every x with every y (%2 points for %3 x %4)")
                                            .arg(name)
                                            .arg(at.size())
                                            .arg(xs.size())
                                            .arg(ys.size()));
                continue;
            }
            for (double x : table->x)
                for (double y : table->grid) table->z.push_back(at.at({x, y}));
            Scope::Function fn;
            fn.arguments << QStringLiteral("x") << QStringLiteral("y");
            fn.argumentDims << (xu ? xu->dim : Dim()) << (vu ? vu->dim : Dim());
            fn.table = table;
            fn.dim = fu ? fu->dim : Dim();
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
