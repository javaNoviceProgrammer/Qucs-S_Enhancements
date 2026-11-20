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
#include "rf25_bondwire.h"
#include "rf25_layout.h"
#include "rf25_sparams.h"

#include "multiphysics/fem_expr.h"
#include "multiphysics/optimizer_core.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
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

/// A list of names - of terminals and ports - written as a list of texts
/// (namesOf() reads a text of them too).
P names(const QString& key, const QString& label, const QString& tip = {})
{
    P p;
    p.key = key;
    p.label = label;
    p.kind = P::Objects;
    p.defaultValue = QJsonArray();
    p.tooltip = tip;
    return p;
}

/// What a port's Returns are, as both kinds of port say it.
QString returnsTip()
{
    return QCoreApplication::translate("qucs_s::rf25",
                                       "The terminals (a Terminal under Ports) and the ports it returns through, by "
                                       "their names, all tied: one voltage, the current shared out between them by the "
                                       "solve, as a probe's ground tips are - one for a ground-signal probe, two for a "
                                       "GSG, four for the ground ring round a pad. A port named is in series with this "
                                       "one: its cells are this one's return, and it a port of its own. One on this port's own conductor, apart from "
                                       "its cells, is a source across that conductor: a shorted line, a stub, a loop "
                                       "(full wave)");
}

QList<NodeKind> makeKinds()
{
    QList<NodeKind> k;
    const auto add = [&k](NodeKind kind) { k.append(std::move(kind)); };
    constexpr fem::Level none = fem::Level::None;

    add({QStringLiteral("rf_model"), tr("RF 2.5D Model"), QString(), QStringLiteral("model"), none,
         {text(QStringLiteral("description"), tr("Description"), QString())},
         {QStringLiteral("rf_parameters"), QStringLiteral("rf_stack"), QStringLiteral("rf_layout"),
          QStringLiteral("rf_ports"), QStringLiteral("rf_mesh"), QStringLiteral("rf_study"), QStringLiteral("rf_analyses"),
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
               tr("A lossy silicon substrate: 10[S/m] for 10 Ohm cm. Zero for an insulating one.")),
          expr(QStringLiteral("mur"), tr("Relative permeability"), QString(), QStringLiteral("1"),
               tr("A ferrite's or a magnetic film's: the full wave takes it in its fields, the quasi-static solve in its L "
                  "(a second charge solve with 1/mur for each layer's permittivity, by duality). 1 for anything not "
                  "magnetic.")),
          expr(QStringLiteral("mtand"), tr("Magnetic loss tangent"), QString(), QStringLiteral("0"),
               tr("Its permeability's loss, mur (1 - j mtand), in the full wave.")),
          expr(QStringLiteral("epsz"), tr("Relative permittivity along z"), QString(), QStringLiteral("0"),
               tr("Uniaxial, its axis vertical - sapphire, LCP, a z-cut crystal -: its relative permittivity along z, the one "
                  "above then that in the plane. 0: isotropic.")),
          expr(QStringLiteral("epsy"), tr("Relative permittivity along y"), QString(), QStringLiteral("0"),
               tr("Where the plane's permittivity differs along x and y - an x-cut crystal -: that along y, the one above "
                  "that along x. A layered solve has none in the plane: it takes their geometric mean, and says so. 0: the "
                  "same as along x.")),
          choice(QStringLiteral("dispersion"), tr("Dispersion"), {QStringLiteral("none"), QStringLiteral("djordjevic_sarkar")},
                 {tr("None (the same at every frequency)"), tr("Djordjevic-Sarkar (wideband Debye)")}, QStringLiteral("none"),
                 tr("Djordjevic-Sarkar: its permittivity falls slowly with frequency and its loss tangent stays nearly flat, "
                    "as a real board's do and as a constant pair cannot - causal. Its relative permittivity and loss tangent "
                    "are then those at the frequency below.")),
          expr(QStringLiteral("dispersion_at"), tr("Given at"), QStringLiteral("Hz"), QStringLiteral("1[GHz]"),
               tr("The frequency its relative permittivity and loss tangent are measured at, where its dispersion is "
                  "Djordjevic-Sarkar.")),
          expr(QStringLiteral("beyond"), tr("Beyond its shapes"), QString(), QStringLiteral("1"),
               tr("Where the layer map sends shapes to this layer, it is there only inside them - a chip's or a board's "
                  "edge, a trench -, and this relative permittivity beyond them: 1 for air."))},
         {}, {}, false, true, QStringLiteral("sub"),
         tr("The bottom layer, below every metal. One at most, and first in the stack.")});
    add({QStringLiteral("rf_dielectric"), tr("Dielectric"), QStringLiteral("stack"), QStringLiteral("layer"), none,
         {expr(QStringLiteral("thickness"), tr("Thickness"), QStringLiteral("m"), QStringLiteral("1[um]")),
          expr(QStringLiteral("epsr"), tr("Relative permittivity"), QString(), QStringLiteral("4.1")),
          expr(QStringLiteral("tand"), tr("Loss tangent"), QString(), QStringLiteral("0.002")),
          // (Any layer's, not the substrate's alone: epi on bulk, an SOI
          //  device layer, a trap-rich layer - F7 of the open items.)
          expr(QStringLiteral("sigma"), tr("Conductivity"), QStringLiteral("S/m"), QStringLiteral("0"),
               tr("A conducting layer over the substrate - an epitaxial layer, an SOI device layer, a trap-rich layer, a "
                  "well: 5[S/m] for 20 Ohm cm. The full wave takes it in its fields, as it takes the substrate's; the "
                  "quasi-static solve takes the layer as a dielectric of its permittivity. Zero for an insulator.")),
          expr(QStringLiteral("mur"), tr("Relative permeability"), QString(), QStringLiteral("1"),
               tr("A ferrite's or a magnetic film's: the full wave takes it in its fields, the quasi-static solve in its L "
                  "(a second charge solve with 1/mur for each layer's permittivity, by duality). 1 for anything not "
                  "magnetic.")),
          expr(QStringLiteral("mtand"), tr("Magnetic loss tangent"), QString(), QStringLiteral("0"),
               tr("Its permeability's loss, mur (1 - j mtand), in the full wave.")),
          expr(QStringLiteral("epsz"), tr("Relative permittivity along z"), QString(), QStringLiteral("0"),
               tr("Uniaxial, its axis vertical - sapphire, LCP, a z-cut crystal -: its relative permittivity along z, the one "
                  "above then that in the plane. 0: isotropic.")),
          expr(QStringLiteral("epsy"), tr("Relative permittivity along y"), QString(), QStringLiteral("0"),
               tr("Where the plane's permittivity differs along x and y - an x-cut crystal -: that along y, the one above "
                  "that along x. A layered solve has none in the plane: it takes their geometric mean, and says so. 0: the "
                  "same as along x.")),
          choice(QStringLiteral("dispersion"), tr("Dispersion"), {QStringLiteral("none"), QStringLiteral("djordjevic_sarkar")},
                 {tr("None (the same at every frequency)"), tr("Djordjevic-Sarkar (wideband Debye)")}, QStringLiteral("none"),
                 tr("Djordjevic-Sarkar: its permittivity falls slowly with frequency and its loss tangent stays nearly flat, "
                    "as a real board's do and as a constant pair cannot - causal. Its relative permittivity and loss tangent "
                    "are then those at the frequency below.")),
          expr(QStringLiteral("dispersion_at"), tr("Given at"), QStringLiteral("Hz"), QStringLiteral("1[GHz]"),
               tr("The frequency its relative permittivity and loss tangent are measured at, where its dispersion is "
                  "Djordjevic-Sarkar.")),
          expr(QStringLiteral("beyond"), tr("Beyond its shapes"), QString(), QStringLiteral("1"),
               tr("Where the layer map sends shapes to this layer, it is there only inside them - an oxide under one part "
                  "alone, a dielectric cut back -, and this relative permittivity beyond them: 1 for air."))},
         {}, {}, false, false, QStringLiteral("d"),
         tr("An insulating layer: an oxide between two metals, a passivation over the top metal.")});
    add({QStringLiteral("rf_metal"), tr("Metal"), QStringLiteral("stack"), QStringLiteral("conductor"), none,
         {expr(QStringLiteral("thickness"), tr("Thickness"), QStringLiteral("m"), QStringLiteral("0.6[um]")),
          expr(QStringLiteral("sigma"), tr("Conductivity"), QStringLiteral("S/m"), QStringLiteral("3.3e7")),
          expr(QStringLiteral("roughness"), tr("Roughness"), QStringLiteral("m"), QStringLiteral("0"),
               tr("The rms roughness of its surface; it raises the loss at high frequency (Hammerstad's model).")),
          choice(QStringLiteral("roughness_model"), tr("Roughness model"), {QStringLiteral("hammerstad"), QStringLiteral("huray")},
                 {tr("Hammerstad (its rms roughness)"), tr("Huray (snowballs: nodules on a flat)")}, QStringLiteral("hammerstad"),
                 tr("Hammerstad's adds at most the loss again, which a rough foil passes at tens of GHz; Huray's, from "
                    "copper nodules on the foil's surface, does not saturate there - what a board's copper wants above "
                    "some 10 GHz.")),
          expr(QStringLiteral("nodule_radius"), tr("Nodule radius"), QStringLiteral("m"), QStringLiteral("0.5[um]"),
               tr("Huray's: the radius of the copper nodules on the surface.")),
          expr(QStringLiteral("surface_ratio"), tr("Surface ratio"), QString(), QStringLiteral("2"),
               tr("Huray's: the nodules' surface over the flat surface's they stand on, N 4 pi a^2 / A.")),
          choice(QStringLiteral("model"), tr("Solved as"), {QStringLiteral("automatic"), QStringLiteral("sheet"), QStringLiteral("faces")},
                 {tr("Automatic (its two faces where it is thick beside its gaps)"), tr("A sheet (its thickness in its loss)"),
                  tr("Its two faces, joined at its walls")},
                 QStringLiteral("sheet"),
                 tr("A sheet is quickest, and right where the metal is thin beside its gaps. Its two faces put its "
                    "thickness in the field too - its top face higher, its walls carrying current and, where they face "
                    "another conductor's across a narrow gap, charge - at twice its cells. A sheet has no walls: on thick "
                    "metal in narrow gaps its inductance reads high and its capacitance low (some 20 % where they are "
                    "equal), and the solve says so. Automatic solves it as its two faces where it is at least a fortieth "
                    "as thick as the narrowest gap beside it, and as a sheet elsewhere; a new metal is Automatic, and a "
                    "file that says nothing a sheet."))},
         {}, {}, false, false, QStringLiteral("m"),
         tr("A metal layer. The layout's polygons on the layers mapped to it are its conductors; its name (the label) "
            "is what the layer map and the ports call it.")});
    add({QStringLiteral("rf_via"), tr("Via"), QStringLiteral("stack"), QStringLiteral("via"), none,
         {text(QStringLiteral("from"), tr("From metal"), QString(),
               tr("The metal below, by its name, or \"ground\" for a via down to the ground plane")),
          text(QStringLiteral("to"), tr("To metal"), QString(), tr("The metal above, by its name")),
          expr(QStringLiteral("sigma"), tr("Conductivity"), QStringLiteral("S/m"), QStringLiteral("2.1e7")),
          expr(QStringLiteral("fill"), tr("Filled fraction"), QString(), QStringLiteral("1"),
               tr("How much of the drawn shape is metal (a tungsten plug in a wider opening: below 1)."))},
         {}, {}, false, false, QStringLiteral("v"),
         tr("The metal that joins two layers through the dielectric between them, or a metal to the ground plane "
            "(\"ground\", or the ground's name) through the substrate. Its two metals must be neighbours in the stack, "
            "and a via to the ground goes up to the lowest metal. The full-wave solve carries its current up and down; "
            "the quasi-static extraction joins what a via joins, and does not solve a via to the ground.")});
    add({QStringLiteral("rf_ground"), tr("Ground"), QStringLiteral("stack"), QStringLiteral("conductor"), none,
         {choice(QStringLiteral("kind"), tr("Below the stack"),
                 {QStringLiteral("perfect"), QStringLiteral("metal"), QStringLiteral("impedance"), QStringLiteral("open")},
                 {tr("A perfect plane"), tr("A metal plane"), tr("A surface impedance"), tr("Open (no plane)")},
                 QStringLiteral("perfect")),
          shownIf(expr(QStringLiteral("sigma"), tr("Conductivity"), QStringLiteral("S/m"), QStringLiteral("5.8e7"),
                       tr("Its loss, a skin depth deep: its share of a line's R")),
                  "kind=metal"),
          shownIf(expr(QStringLiteral("impedance"), tr("Surface impedance"), QStringLiteral("Ohm"), QStringLiteral("0"),
                       tr("Of the plane, per square")),
                  "kind=impedance"),
          shownIf(boolean(QStringLiteral("fullwave_loss"), tr("Its loss in the full-wave solve"), false,
                          tr("Count the plane's loss in the full-wave solve too, on the current its mirror image of each "
                             "run carries - a microstrip's ground is some 15 to 30 % of its metal's loss. The fields stay a "
                             "perfect plane's; the quasi-static extraction counts the loss always")),
                  "kind=metal|kind=impedance")},
         {}, {}, false, true, QStringLiteral("gnd"),
         tr("What closes the stack at the bottom: a plane under the substrate, wherever it is in the tree. One at "
            "most. A perfect plane loses nothing; a metal plane loses as its conductivity says, its share of a "
            "line's R - in the full-wave solve only where its loss there is asked for. Open is no plane at all - the "
            "substrate on air.")});
    add({QStringLiteral("rf_cover"), tr("Cover"), QStringLiteral("stack"), QStringLiteral("conductor"), none,
         {expr(QStringLiteral("height"), tr("Height above the top"), QStringLiteral("m"), QStringLiteral("1[mm]")),
          choice(QStringLiteral("kind"), tr("Above the stack"),
                 {QStringLiteral("open"), QStringLiteral("perfect")},
                 {tr("Open (air above)"), tr("A metal lid")}, QStringLiteral("open")),
          shownIf(boolean(QStringLiteral("walls"), tr("Side walls"), false,
                          tr("Four perfect walls from the ground up to the lid round a rectangle: a box, the layout inside "
                             "it")),
                  "kind=perfect"),
          shownIf(expr(QStringLiteral("box_x0"), tr("Box from x"), QStringLiteral("m"), QStringLiteral("0")), "walls=true"),
          shownIf(expr(QStringLiteral("box_y0"), tr("Box from y"), QStringLiteral("m"), QStringLiteral("0")), "walls=true"),
          shownIf(expr(QStringLiteral("box_x1"), tr("Box to x"), QStringLiteral("m"), QStringLiteral("1[mm]")), "walls=true"),
          shownIf(expr(QStringLiteral("box_y1"), tr("Box to y"), QStringLiteral("m"), QStringLiteral("1[mm]")), "walls=true")},
         {}, {}, false, true, QStringLiteral("cov"),
         tr("What closes the stack at the top: air without end, or a lid at a height - and, with side walls, a box round "
            "the layout.")});

    // ---- the layout: x and y ----
    add({QStringLiteral("rf_layout"), tr("Layout"), QString(), QStringLiteral("layout"), none,
         {file(QStringLiteral("file"), tr("GDSII or OASIS file"), tr("Relative to the model's folder")),
          text(QStringLiteral("cell"), tr("Cell"), QString(), tr("The cell to solve; the top cell when empty")),
          expr(QStringLiteral("unit"), tr("Unit of its coordinates"), QStringLiteral("m"), QStringLiteral("1[um]"),
               tr("What one unit of the file means, where the file itself does not say"))},
         {QStringLiteral("rf_layer_map"), QStringLiteral("rf_processing"), QStringLiteral("rf_cutout"), QStringLiteral("rf_shapes"), QStringLiteral("rf_dimension")},
         {QStringLiteral("rf_layer_map"), QStringLiteral("rf_processing"), QStringLiteral("rf_cutout")}, true, true, QString(),
         tr("The shapes in x and y, read with gdstk as get_layout and find_shapes read them - and the model's own "
            "shapes, whose sizes are expressions of its parameters (Shapes).")});
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
          shownIf(boolean(QStringLiteral("keep_area"), tr("Keep the vias' metal"), true,
                          tr("The union conducts as the vias in it do, their area over its own, its current spread "
                             "over the whole of it as theirs is; off, it conducts as solid metal")),
                  "merge_vias=true"),
          boolean(QStringLiteral("drop_slivers"), tr("Drop slivers"), true),
          shownIf(expr(QStringLiteral("sliver_width"), tr("Thinner than"), QStringLiteral("m"), QStringLiteral("0.1[um]")),
                  "drop_slivers=true"),
          boolean(QStringLiteral("snap"), tr("Snap to a grid"), false),
          shownIf(expr(QStringLiteral("grid"), tr("Grid"), QStringLiteral("m"), QStringLiteral("5[nm]")), "snap=true"),
          boolean(QStringLiteral("heal"), tr("Heal polygons"), true,
                  tr("Self-intersections, repeated points and zero-width necks")),
          boolean(QStringLiteral("simplify"), tr("Simplify curves"), true,
                  tr("Corners nearer the line between their neighbours than the tolerance dropped: a circle drawn of "
                     "many segments meshed as one of fewer")),
          shownIf(expr(QStringLiteral("curve_tolerance"), tr("Tolerance"), QStringLiteral("m"), QStringLiteral("0"),
                       tr("How far the outline may move; 0: a hundredth of each shape's width, at most 0.1 µm")),
                  "simplify=true"),
          boolean(QStringLiteral("remove_fill"), tr("Remove dummy fill"), false),
          choice(QStringLiteral("symmetry"), tr("Symmetry"), {QStringLiteral("none"), QStringLiteral("x"), QStringLiteral("y")},
                 {tr("None"), tr("A plane x = constant"), tr("A plane y = constant")}, QStringLiteral("none"),
                 tr("A plane the layout is the mirror of itself across: the half beyond it solved, with its mirror, in a "
                    "quarter of the dense matrix - the full-wave solve's")),
          shownIf(expr(QStringLiteral("symmetry_at"), tr("The plane at"), QStringLiteral("m"), QStringLiteral("0"),
                       tr("Its x, or its y")),
                  "symmetry!=none"),
          shownIf(choice(QStringLiteral("symmetry_wall"), tr("Its wall"), {QStringLiteral("magnetic"), QStringLiteral("electric")},
                         {tr("Magnetic: the even mode, ports on it"), tr("Electric: the odd mode, ports off it")},
                         QStringLiteral("magnetic"),
                         tr("A magnetic wall: the fields' mirror even - a line along the plane, its edge ports on it, the "
                            "whole's ports; or every port off it, each with its mirror in phase. An electric wall: odd, "
                            "every port off it, each against its mirror, nothing touching it")),
                  "symmetry!=none")},
         {}, {}, true, true, QString(),
         tr("What is done to the shapes before they are meshed. Each step says what it changed, and the processed "
            "layout is drawn over the original.")});
    add({QStringLiteral("rf_cutout"), tr("Cutout"), QString(), QStringLiteral("cutout"), none,
         {boolean(QStringLiteral("apply"), tr("Apply its regions"), true,
                  tr("Off: the whole layout is solved, the regions kept for later"))},
         {QStringLiteral("rf_region")}, {}, true, true, QString(),
         tr("The part of the layout to solve. Each region under it keeps what is inside it, the rest of the layout "
            "masked, or takes out what is inside it; with none, the whole layout is solved. Its shapes are cut along "
            "the regions' edges before the processing's steps, and the solve, the mesh and an export see only what is "
            "left.")});
    add({QStringLiteral("rf_region"), tr("Region"), QStringLiteral("cutout"), QStringLiteral("region"), none,
         {choice(QStringLiteral("shape"), tr("Shape"),
                 {QStringLiteral("rectangle"), QStringLiteral("polygon"), QStringLiteral("circle")},
                 {tr("A rectangle"), tr("A polygon"), tr("A circle")}, QStringLiteral("rectangle")),
          shownIf(expr(QStringLiteral("x0"), tr("From x"), QStringLiteral("m"), QStringLiteral("0"),
                       tr("One corner of the rectangle, in the layout's coordinates")),
                  "shape=rectangle"),
          shownIf(expr(QStringLiteral("y0"), tr("From y"), QStringLiteral("m"), QStringLiteral("0")), "shape=rectangle"),
          shownIf(expr(QStringLiteral("x1"), tr("To x"), QStringLiteral("m"), QStringLiteral("100[um]"),
                       tr("The corner across from it")),
                  "shape=rectangle"),
          shownIf(expr(QStringLiteral("y1"), tr("To y"), QStringLiteral("m"), QStringLiteral("100[um]")), "shape=rectangle"),
          shownIf(rows(QStringLiteral("points"), tr("Corners"), {tr("x"), tr("y")},
                       tr("The polygon's corners in order, each an x and a y in the layout's coordinates: 30[um], "
                          "-5[um]. Three at least.")),
                  "shape=polygon"),
          shownIf(expr(QStringLiteral("x"), tr("Centre x"), QStringLiteral("m"), QStringLiteral("0")), "shape=circle"),
          shownIf(expr(QStringLiteral("y"), tr("Centre y"), QStringLiteral("m"), QStringLiteral("0")), "shape=circle"),
          shownIf(expr(QStringLiteral("radius"), tr("Radius"), QStringLiteral("m"), QStringLiteral("50[um]")),
                  "shape=circle"),
          choice(QStringLiteral("action"), tr("What is inside"), {QStringLiteral("keep"), QStringLiteral("remove")},
                 {tr("Kept, the rest masked"), tr("Taken out")}, QStringLiteral("keep"),
                 tr("Kept: of the shapes, only what is inside this region or another region that keeps is solved. "
                    "Taken out: what is inside is cut away, from what the regions that keep left")),
          text(QStringLiteral("layers"), tr("On layers"), QString(),
               tr("The stack's layers it cuts, by name, apart by commas: M1, M2, V1. Empty: every layer"))},
         {}, {}, false, false, QStringLiteral("cut"),
         tr("A part of the layout, in its coordinates: a rectangle by two corners, a polygon by its corners or a "
            "circle. What is inside it is kept, the rest of the layout masked - or taken out. A shape across its edge "
            "is cut along it.")});

    // The model's own shapes: each number an expression of the parameters,
    // added to the file's shapes on their layer before the processing.
    add({QStringLiteral("rf_shapes"), tr("Shapes"), QString(), QStringLiteral("layout"), none, {},
         {QStringLiteral("rf_shape"), QStringLiteral("rf_bondwire")}, {}, false, true, QStringLiteral("shapes"),
         tr("Shapes of the model's own, beside the layout file's: rectangles, polygons, paths and vias whose corners, "
            "widths and lengths are expressions of the parameters (w, L/2 + 5[um]) - what a Parametric Sweep or an "
            "Optimization varies. Each is added to its layer of the stack with the file's shapes, before the "
            "processing's steps; a model of these alone needs no layout file.")});
    add({QStringLiteral("rf_bondwire"), tr("Bond Wire"), QStringLiteral("shapes"), QStringLiteral("region"), none,
         {text(QStringLiteral("from_metal"), tr("From metal"), QString(), tr("The metal of its first pad")),
          expr(QStringLiteral("x0"), tr("From x"), QStringLiteral("m"), QStringLiteral("0")),
          expr(QStringLiteral("y0"), tr("From y"), QStringLiteral("m"), QStringLiteral("0")),
          text(QStringLiteral("to_metal"), tr("To metal"), QString(), tr("The metal of its second pad")),
          expr(QStringLiteral("x1"), tr("To x"), QStringLiteral("m"), QStringLiteral("1[mm]")),
          expr(QStringLiteral("y1"), tr("To y"), QStringLiteral("m"), QStringLiteral("0")),
          choice(QStringLiteral("profile"), tr("Profile"), {QStringLiteral("arc"), QStringLiteral("ball")},
                 {tr("An arc (a parabola)"), tr("Up from a ball bond (JEDEC-like)")}, QStringLiteral("arc"),
                 tr("A parabola from pad to pad, or straight up from the first pad, flat over it, and straight down to the "
                    "second, as a ball-bonded wire's loop is")),
          expr(QStringLiteral("height"), tr("Loop height"), QStringLiteral("m"), QStringLiteral("150[um]"),
               tr("An arc's peak over its pads' chord; a ball bond's loop over its first pad")),
          shownIf(expr(QStringLiteral("knee"), tr("Flat over the ball"), QString(), QStringLiteral("0.15"),
                       tr("How much of the span the loop runs flat at its height before it falls to the second pad")),
                  "profile=ball"),
          expr(QStringLiteral("diameter"), tr("Diameter"), QStringLiteral("m"), QStringLiteral("25[um]")),
          expr(QStringLiteral("sigma"), tr("Conductivity"), QStringLiteral("S/m"), QStringLiteral("4.1e7"), tr("Gold's; nought, perfect")),
          expr(QStringLiteral("pieces"), tr("Pieces"), QString(), QStringLiteral("3"),
               tr("The runs along its span, each at the height that keeps the profile's mean ln h over it: one to four, its "
                  "inductance its profile's within 3 % (more is not solved yet)")),
          boolean(QStringLiteral("fit"), tr("Fitted to its profile"), true,
                  tr("Its runs' heights over the chord scaled so that the staircase's inductance over the ground is the "
                     "profile's own"))},
         {}, {}, false, true, QStringLiteral("wire"),
         tr("A bond wire looped in the air between two pads, made of runs on metal layers of its own in air slabs over the "
            "stack and vias between them and up from its pads: a staircase of its profile, its heights fitted so that its "
            "inductance over the ground is the profile's. Solved full wave and quasi-static alike; the solve's alone, not "
            "the board's layers.")});
    add({QStringLiteral("rf_shape"), tr("Shape"), QStringLiteral("shapes"), QStringLiteral("region"), none,
         {text(QStringLiteral("layer"), tr("On layer"), QString(),
               tr("A metal or a via of the stack, by name - or a dielectric or the substrate, where it is (a layer that "
                  "ends)")),
          choice(QStringLiteral("shape"), tr("Shape"),
                 {QStringLiteral("rectangle"), QStringLiteral("polygon"), QStringLiteral("path"), QStringLiteral("via"),
                  QStringLiteral("circle"), QStringLiteral("ring")},
                 {tr("A rectangle"), tr("A polygon"), tr("A path"), tr("A via"), tr("A circle"), tr("A ring")},
                 QStringLiteral("rectangle")),
          shownIf(choice(QStringLiteral("anchor"), tr("Placed by"), {QStringLiteral("corner"), QStringLiteral("centre")},
                         {tr("Its lower left corner"), tr("Its centre")}, QStringLiteral("corner")),
                  "shape=rectangle"),
          shownIf(expr(QStringLiteral("x"), tr("x"), QStringLiteral("m"), QStringLiteral("0"),
                       tr("Of the rectangle's lower left corner or centre, or the via's, circle's or ring's centre, in the "
                          "layout's coordinates")),
                  "shape=rectangle|shape=via|shape=circle|shape=ring"),
          shownIf(expr(QStringLiteral("y"), tr("y"), QStringLiteral("m"), QStringLiteral("0")),
                  "shape=rectangle|shape=via|shape=circle|shape=ring"),
          shownIf(expr(QStringLiteral("width"), tr("Width"), QStringLiteral("m"), QStringLiteral("100[um]"), tr("Along x")),
                  "shape=rectangle"),
          shownIf(expr(QStringLiteral("height"), tr("Height"), QStringLiteral("m"), QStringLiteral("10[um]"), tr("Along y")),
                  "shape=rectangle"),
          shownIf(rows(QStringLiteral("points"), tr("Points"), {tr("x"), tr("y")},
                       tr("A polygon's corners in order, three at least, or a path's points along its spine, two at least: "
                          "each an x and a y in the layout's coordinates (0, L/2 + 5[um])")),
                  "shape=polygon|shape=path"),
          shownIf(expr(QStringLiteral("path_width"), tr("Width"), QStringLiteral("m"), QStringLiteral("10[um]"),
                       tr("The path's width, across its spine")),
                  "shape=path"),
          shownIf(choice(QStringLiteral("ends"), tr("Ends"),
                         {QStringLiteral("flush"), QStringLiteral("round"), QStringLiteral("extended")},
                         {tr("Flush with its first and last points"), tr("Round"), tr("Extended by half its width")},
                         QStringLiteral("flush")),
                  "shape=path"),
          shownIf(expr(QStringLiteral("size"), tr("Side"), QStringLiteral("m"), QStringLiteral("1[um]"), tr("Of a square via")),
                  "shape=via"),
          shownIf(expr(QStringLiteral("radius"), tr("Radius"), QStringLiteral("m"), QStringLiteral("50[um]"),
                       tr("The circle's, or the ring's outer edge's")),
                  "shape=circle|shape=ring"),
          shownIf(expr(QStringLiteral("inner_radius"), tr("Inner radius"), QStringLiteral("m"), QStringLiteral("25[um]"),
                       tr("The ring's inner edge's, under its radius")),
                  "shape=ring")},
         {}, {}, false, false, QStringLiteral("shape"),
         tr("A shape of the model's own on a layer of the stack, its numbers expressions of the parameters: a "
            "rectangle by a corner or its centre and its width and height, a polygon by its corners, a path by its "
            "points and width, a square via by its centre and side, a circle by its centre and radius, a ring by its "
            "centre and its two radii (a circle of up to 1024 sides, within a nanometre of it to a radius of some 200 µm).")});

    // A parameter tied to the file's own shapes: the corners of a layer's
    // shapes inside a box moved by an expression - a line widened, a stub
    // made longer - without drawing them again.
    add({QStringLiteral("rf_dimension"), tr("Dimension"), QStringLiteral("dimension"), QStringLiteral("region"), none,
         {text(QStringLiteral("layer"), tr("On layer"), QString(), tr("The layer of the stack whose shapes of the layout file it moves")),
          expr(QStringLiteral("x0"), tr("Box from x"), QStringLiteral("m"), QStringLiteral("0"),
               tr("One corner of the box round the corners to move, in the layout's coordinates")),
          expr(QStringLiteral("y0"), tr("Box from y"), QStringLiteral("m"), QStringLiteral("0")),
          expr(QStringLiteral("x1"), tr("Box to x"), QStringLiteral("m"), QStringLiteral("10[um]"), tr("The corner across from it")),
          expr(QStringLiteral("y1"), tr("Box to y"), QStringLiteral("m"), QStringLiteral("10[um]")),
          choice(QStringLiteral("direction"), tr("Moved toward"),
                 {QStringLiteral("+x"), QStringLiteral("-x"), QStringLiteral("+y"), QStringLiteral("-y"), QStringLiteral("angle")},
                 {tr("+x"), tr("-x"), tr("+y"), tr("-y"), tr("An angle")}, QStringLiteral("+x")),
          shownIf(expr(QStringLiteral("angle"), tr("Angle"), QString(), QStringLiteral("0"),
                       tr("Degrees from +x, counterclockwise")),
                  "direction=angle"),
          expr(QStringLiteral("offset"), tr("By"), QStringLiteral("m"), QStringLiteral("0"),
               tr("How far, an expression of the parameters: w - 50[um] widens a 50 µm line to w where the box holds "
                  "its one edge"))},
         {}, {}, false, false, QStringLiteral("dim"),
         tr("Moves the corners of the layout file's shapes on a layer that are inside a box - its edges too - by an "
            "expression of the parameters, before the processing: a box round a line's one edge widens it, one round "
            "a stub's end lengthens it, one round the whole of a shape moves it. Several apply in order. A shape that "
            "would cross itself is left as it was, and said.")});

    // ---- the ports ----
    add({QStringLiteral("rf_ports"), tr("Ports"), QString(), QStringLiteral("ports"), none, {},
         {QStringLiteral("rf_edge_port"), QStringLiteral("rf_internal_port"), QStringLiteral("rf_terminal"),
          QStringLiteral("rf_return"), QStringLiteral("rf_trl")},
         {}, true, true, QString(),
         tr("Where the structure is driven, and what its lines return through. The ports' order is the order of the "
            "S matrix.")});
    add({QStringLiteral("rf_edge_port"), tr("Edge Port"), QStringLiteral("port"), QStringLiteral("port"), none,
         {text(QStringLiteral("conductor"), tr("On metal"), QString(), tr("A metal of the stack, by its name")),
          expr(QStringLiteral("x"), tr("At x"), QStringLiteral("m"), QStringLiteral("0")),
          expr(QStringLiteral("y"), tr("At y"), QStringLiteral("m"), QStringLiteral("0")),
          choice(QStringLiteral("facing"), tr("Facing"),
                 {QStringLiteral("auto"), QStringLiteral("+x"), QStringLiteral("-x"), QStringLiteral("+y"), QStringLiteral("-y")},
                 {tr("Outwards"), QStringLiteral("+x"), QStringLiteral("-x"), QStringLiteral("+y"), QStringLiteral("-y")},
                 QStringLiteral("auto")),
          choice(QStringLiteral("return"), tr("Return"),
                 {QStringLiteral("ground"), QStringLiteral("beside"), QStringLiteral("terminals")},
                 {tr("Through the ground plane"), tr("Through the conductors beside it (ground-signal-ground)"),
                  tr("Through the terminals and ports it names (a probe's tips)")},
                 QStringLiteral("ground"),
                 tr("Where its current comes back: up from the ground plane under its face, or - for a coplanar "
                    "line, as a probe's grounds touch it - through the conductors either side of it on the line of "
                    "its edge, which must reach that edge too, tied together: the nearest return conductors, past "
                    "any other; or through the terminals and ports it names, wherever they are (full wave)")),
          shownIf(names(QStringLiteral("returns"), tr("Returns"), returnsTip()), "return=terminals"),
          expr(QStringLiteral("impedance"), tr("Reference impedance"), QStringLiteral("Ohm"), QStringLiteral("50")),
          text(QStringLiteral("pair"), tr("Pair with"), QString(),
               tr("Another port, by its name: this one the pair's plus, that one its minus - a differential pair, as "
                  "a GSSG's two signals are driven. The solve then gives its mixed-mode S too: the differential and "
                  "common modes (full wave)")),
          boolean(QStringLiteral("calibrate"), tr("Take out its own discontinuity"), false,
                  tr("Solve strips of its feed - a coplanar port's with its grounds and their vias - and take the "
                     "port's own discontinuity out of the answer: its reference plane is then at its edge (full "
                     "wave)")),
          shownIf(expr(QStringLiteral("strips"), tr("Calibration strips"), QStringLiteral("m"), QStringLiteral("0"),
                       tr("How long the shortest strip of its feed is: nought chooses - eight heights over the ground and "
                          "twice its width (a coplanar feed with no ground plane: eight of its signal and gaps), and "
                          "shorter strips beside them where a third says those do not agree on one line. On a substrate "
                          "thick beside the wavelength, shorter strips agree to a higher frequency")),
                  "calibrate=true"),
          expr(QStringLiteral("deembed"), tr("De-embed"), QStringLiteral("m"), QStringLiteral("0"),
               tr("How far the reference plane is then moved in along the line (out, below nought)")),
          choice(QStringLiteral("along"), tr("Moved along"), {QStringLiteral("feed"), QStringLiteral("line")},
                 {tr("Its feed, as two strips of it say (and its own discontinuity taken out)"),
                  tr("A line of given impedance and permittivity (nothing else taken out)")},
                 QStringLiteral("feed")),
          shownIf(expr(QStringLiteral("line_z0"), tr("Line impedance"), QStringLiteral("Ohm"), QStringLiteral("50")),
                  "along=line"),
          shownIf(expr(QStringLiteral("line_eps"), tr("Line effective permittivity"), QString(), QStringLiteral("1")),
                  "along=line")},
         {}, {}, false, false, QStringLiteral("p"),
         tr("A port on the edge of a polygon, driven against the ground below it - or, for a coplanar line, "
            "against the conductors beside it on the line of its edge, as a ground-signal-ground probe drives it. "
            "Solved as it is, its reference plane is at the centres of the cells along its face, and its own "
            "discontinuity - the face's fringing charge, the current's turn into it - is in the answer. Taken out, by "
            "two strips of its feed solved the same way, the reference plane is at its edge, and can be moved along "
            "the line from there. The Edge Port tool puts one on an edge clicked on the layout, facing out of it.")});
    add({QStringLiteral("rf_internal_port"), tr("Internal Port"), QStringLiteral("port"), QStringLiteral("port"), none,
         {expr(QStringLiteral("x"), tr("At x"), QStringLiteral("m"), QStringLiteral("0")),
          expr(QStringLiteral("y"), tr("At y"), QStringLiteral("m"), QStringLiteral("0")),
          choice(QStringLiteral("return"), tr("Return"), {QStringLiteral("from"), QStringLiteral("terminals")},
                 {tr("Its From metal (or the ground) under it"), tr("Through the terminals and ports it names (a probe's tips)")},
                 QStringLiteral("from"),
                 tr("Where its current comes back: from the cells of its From metal under it - or the ground - up a "
                    "path of its own, or through the terminals and ports it names, wherever they are, with no path "
                    "up to them: a probe's tips on its pads (full wave)")),
          shownIf(text(QStringLiteral("from"), tr("From metal"), QString(), tr("A metal of the stack, or \"ground\"")),
                  "return!=terminals"),
          shownIf(names(QStringLiteral("returns"), tr("Returns"), returnsTip()), "return=terminals"),
          text(QStringLiteral("to"), tr("To metal"), QString()),
          expr(QStringLiteral("width"), tr("Width"), QStringLiteral("m"), QStringLiteral("0"),
               tr("Across x, the rectangle about its point whose cells it drives together; nought, with the height, "
                  "the one cell under the point (full wave)")),
          expr(QStringLiteral("height"), tr("Height"), QStringLiteral("m"), QStringLiteral("0"),
               tr("Across y, the same rectangle's")),
          expr(QStringLiteral("impedance"), tr("Reference impedance"), QStringLiteral("Ohm"), QStringLiteral("50")),
          text(QStringLiteral("pair"), tr("Pair with"), QString(),
               tr("Another port, by its name: this one the pair's plus, that one its minus - a differential pair, as "
                  "a GSSG's two signals are driven. The solve then gives its mixed-mode S too: the differential and "
                  "common modes (full wave)")),
          choice(QStringLiteral("element"), tr("What it is"), {QStringLiteral("port"), QStringLiteral("lumped")},
                 {tr("A port to measure at"), tr("A lumped element: R, L and C in its place")}, QStringLiteral("port"),
                 tr("A lumped element - a chip resistor, an inductor, a capacitor - stands where the port would: across a "
                    "gap, returning through a terminal over its other pad, or up from the ground. It is solved as a port "
                    "and folded in at each frequency by its own impedance, so that S is of the measuring ports alone "
                    "(full wave)")),
          shownIf(expr(QStringLiteral("r"), tr("Resistance"), QStringLiteral("Ohm"), QStringLiteral("0"), tr("Nought: none")), "element=lumped"),
          shownIf(expr(QStringLiteral("l"), tr("Inductance"), QStringLiteral("H"), QStringLiteral("0"), tr("Nought: none")), "element=lumped"),
          shownIf(expr(QStringLiteral("c"), tr("Capacitance"), QStringLiteral("F"), QStringLiteral("0"),
                       tr("Nought: none - in series its two ends joined")),
                  "element=lumped"),
          shownIf(choice(QStringLiteral("topology"), tr("R, L and C"), {QStringLiteral("series"), QStringLiteral("parallel")},
                         {tr("In series"), tr("In parallel")}, QStringLiteral("series")),
                  "element=lumped")},
         {}, {}, false, false, QStringLiteral("p"),
         tr("A port between two metals, or between a metal and the ground: at a point, the one cell under it, or over "
            "a rectangle about the point, every cell of the metal in it driven together - as a rectangle drawn on "
            "the layout with the Internal Port tool makes it.")});

    add({QStringLiteral("rf_terminal"), tr("Terminal"), QStringLiteral("port"), QStringLiteral("port"), none,
         {text(QStringLiteral("metal"), tr("On metal"), QString(),
               tr("A metal of the stack, by its name; empty: whichever is there")),
          expr(QStringLiteral("x"), tr("At x"), QStringLiteral("m"), QStringLiteral("0")),
          expr(QStringLiteral("y"), tr("At y"), QStringLiteral("m"), QStringLiteral("0")),
          choice(QStringLiteral("face"), tr("Of"),
                 {QStringLiteral("none"), QStringLiteral("auto"), QStringLiteral("+x"), QStringLiteral("-x"),
                  QStringLiteral("+y"), QStringLiteral("-y")},
                 {tr("The cells under its rectangle"), tr("The face nearest its point"), tr("The face facing +x"),
                  tr("The face facing -x"), tr("The face facing +y"), tr("The face facing -y")},
                 QStringLiteral("none"),
                 tr("The cells under a rectangle about its point, each by its share of the area - or the one cell under "
                    "the point -, or the face of the conductor at its point, each of its cells by its share of the "
                    "face's length, as an edge port's")),
          shownIf(expr(QStringLiteral("width"), tr("Width"), QStringLiteral("m"), QStringLiteral("0"),
                       tr("Across x, the rectangle about its point whose cells it is; nought, with the height, the one "
                          "cell under the point")),
                  "face=none"),
          shownIf(expr(QStringLiteral("height"), tr("Height"), QStringLiteral("m"), QStringLiteral("0"),
                       tr("Across y, the same rectangle's")),
                  "face=none")},
         {}, {}, false, false, QStringLiteral("t"),
         tr("A place a port returns through: the cells of a metal under a rectangle about its point - or the one cell "
            "under the point -, or the face of the conductor at its point, as an edge port's. It drives nothing by "
            "itself and is no port of the S matrix: a port whose Return is the terminals it names lists it among its "
            "Returns, as a probe's ground tips land on their pads - one for a ground-signal probe, two for a GSG, four "
            "for the ground ring round a pad. With the Internal Port tool, a Shift-drag puts one among the returns of "
            "the port drawn last (full wave).")});

    add({QStringLiteral("rf_return"), tr("Return Conductor"), QStringLiteral("port"), QStringLiteral("port"), none,
         {text(QStringLiteral("conductor"), tr("On metal"), QString(),
               tr("A metal of the stack, by its name; empty: whichever is there")),
          expr(QStringLiteral("x"), tr("At x"), QStringLiteral("m"), QStringLiteral("0")),
          expr(QStringLiteral("y"), tr("At y"), QStringLiteral("m"), QStringLiteral("0"))},
         {}, {}, false, false, QStringLiteral("g"),
         tr("A conductor the lines return through, as a coplanar line's grounds: the one under its point. The "
            "quasi-static solve holds it at nought with the ground and gives the lines of the rest, the signals, "
            "against it - a symmetric pair's even and odd modes by name - and a coplanar port returns through the "
            "nearest of them either side. With none, what a via joins to the ground is one, and with no ground plane "
            "the two outermost conductors of a coplanar line are.")});
    // F3 of the open items of 8 November: a fixture - a GSG pad and its feed
    // at each port - taken out by standards made with it (rf25_trl.h).
    add({QStringLiteral("rf_trl"), tr("TRL Calibration"), QStringLiteral("port"), QStringLiteral("port"), none,
         {file(QStringLiteral("thru"), tr("Thru"),
               tr("A model (.qrf25) of the two fixtures joined: its line between them as long as the fixture's feed on "
                  "each side of the device. Relative to this model's folder")),
          rows(QStringLiteral("lines"), tr("Lines"), {tr("Model (.qrf25)"), tr("Longer than the thru by")},
               tr("One or more models of the fixtures joined by a longer line, each with how much longer: \"500[um]\". "
                  "Lengths whose phases differ by a quarter wave or so tell best; each frequency takes the best of them")),
          file(QStringLiteral("reflect"), tr("Reflect"), tr("A model of the two fixtures each ended alike, open or short")),
          choice(QStringLiteral("reflect_kind"), tr("The reflect is"), {QStringLiteral("open"), QStringLiteral("short")},
                 {tr("Near an open"), tr("Near a short")}, QStringLiteral("open")),
          expr(QStringLiteral("impedance"), tr("The lines' impedance"), QStringLiteral("Ohm"), QString(),
               tr("What the calibrated S is stated in by the lines; given it, S is in the ports' references. Empty: "
                  "the lines' own, which a calibration of lines alone cannot know"))},
         {}, {}, false, true, QStringLiteral("trl"),
         tr("A full-wave solve's two ports calibrated by standards solved as the structure is - a thru, lines and a "
            "reflect, each its own model with the same fixtures -: each fixture's error box found by multiline TRL and "
            "taken out, the S at the thru's middle. Each standard is solved at this model's frequencies with it.")});

    // ---- the mesh, the study, the results ----
    add({QStringLiteral("rf_mesh"), tr("Mesh"), QString(), QStringLiteral("mesh"), none,
         {integer(QStringLiteral("per_wavelength"), tr("Cells per wavelength"), 20),
          expr(QStringLiteral("maximum"), tr("Largest cell"), QStringLiteral("m"), QStringLiteral("0")),
          boolean(QStringLiteral("edges"), tr("Refine at the edges"), true,
                  tr("The current crowds at a conductor's edge; cells there are smaller")),
          integer(QStringLiteral("cells_across"), tr("Cells across a conductor"), 10,
                  tr("At least this many across each conductor's narrowest part (0: none): its charge crowds at "
                     "the edges, so the accuracy is in its width")),
          expr(QStringLiteral("stretch"), tr("Stretch along a line"), QString(), QStringLiteral("4"),
               tr("How many times longer than wide a straight strip's cells may be along it: its charge hardly "
                  "changes along it")),
          integer(QStringLiteral("skin"), tr("Layers through the skin depth"), 0,
                  tr("0: the surface impedance alone, which is right above a few skin depths")),
          expr(QStringLiteral("gap_share"), tr("Gap share"), QString(), QStringLiteral("0.25"),
               tr("Where a conductor faces another across a gap, its outermost cells are halved until they are no wider "
                  "than this share of the gap - where the charge and the current crowd. A convergence check refines it "
                  "with the rest, and says each of its meshes' value")),
          integer(QStringLiteral("most_cells"), tr("Most cells"), 0,
                  tr("How many cells the mesh may have before fewer are put across a conductor (0: the solver's own - "
                     "8000 for the quasi-static, 40 000 for the full wave held compressed)"))},
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
          shownIf(boolean(QStringLiteral("adaptive"), tr("Add points where it changes"), false,
                          tr("Solve a few of the sweep's frequencies and read the rest from a rational fit of the "
                             "ports' Z through them, adding frequencies where the fit is least sure until what it "
                             "says of them is what solving them gives: a sweep of many points in a fraction of the "
                             "time. A sweep of 12 points or fewer is solved whole. On for a new study")),
                  "kind=full_wave"),
          shownIf(expr(QStringLiteral("adaptive_tolerance"), tr("Tolerance"), QString(), QStringLiteral("0.002"),
                       tr("How far any term of S may be from what the fit said of a frequency before it was solved, "
                          "at the frequencies it was least sure of, for the fit to be taken")),
                  "kind=full_wave&adaptive=true"),
          shownIf(choice(QStringLiteral("matrix"), tr("Matrix"),
                         {QStringLiteral("automatic"), QStringLiteral("dense"), QStringLiteral("compressed")},
                         {tr("Automatic"), tr("Dense"), tr("Compressed")}, QStringLiteral("automatic"),
                         tr("How the full-wave solve holds its system: dense up to the limit set in RF 2.5D > "
                            "Settings… and compressed past it (Automatic), always dense - to that limit at most - or "
                            "always compressed, its far blocks of low rank and solved by iteration, to some hundreds of "
                            "thousands")),
                  "kind=full_wave")},
         {}, {}, true, true, QStringLiteral("st"),
         tr("The frequencies to solve at, and by which solver. The quasi-static one solves the charge once and reads "
            "the frequencies for a line's R and G and its S-parameters; the full-wave one solves the currents at each "
            "frequency, driven at the ports, for their S.")});
    // What varies the parameters: a sweep over their values, an optimization.
    add({QStringLiteral("rf_analyses"), tr("Analyses"), QString(), QStringLiteral("study"), none, {},
         {QStringLiteral("rf_sweep"), QStringLiteral("rf_optimization")}, {}, false, true, QStringLiteral("analyses"),
         tr("What varies the model's parameters, each point a whole solve by the study: a Parametric Sweep over their "
            "values, an Optimization that searches them for what is asked of the results.")});
    add({QStringLiteral("rf_sweep"), tr("Parametric Sweep"), QStringLiteral("analysis"), QStringLiteral("study"), none,
         {rows(QStringLiteral("parameters"), tr("Parameters"), {tr("Parameter"), tr("From"), tr("To"), tr("Points"), tr("Spacing"), tr("List")},
               tr("One or two of the model's parameters, each over From to To in Points (linear, or log for logarithmic), or over "
                  "its List of values apart by commas (10[um], 20[um], 40[um])")),
          choice(QStringLiteral("combine"), tr("Two parameters"), {QStringLiteral("crossed"), QStringLiteral("paired")},
                 {tr("Crossed: each value of the first with each of the second"), tr("Paired: the first of each, then the second...")},
                 QStringLiteral("crossed")),
          rows(QStringLiteral("quantities"), tr("Quantities"), {tr("Expression"), tr("Over"), tr("From"), tr("To"), tr("At")},
               tr("What is read at each point: an expression of the solution's values and the parameters - S21_dB, Z11_re, GD21, "
                  "VSWR1, Z0, eps_eff, C11, Lloop - over the frequencies From to To (all of them when empty), Over its last "
                  "(the default), min, max, at the frequency At, or min_at and max_at - where its min or max falls, in Hz: a "
                  "notch's frequency. Two of one expression are named by how each is read")),
          integer(QStringLiteral("parallel"), tr("Solves at once"), 1,
                  tr("How many points are solved at a time: each takes the memory of a solve, and the machine's threads are "
                     "shared between them")),
          // What the last run found: written by it, not a setting - the
          // Optimization's own as the multiphysics kind has it. (Not one of
          // the kind's, a saved or exported sweep's was said to be "no
          // property" when read again: NF3 of the open items.)
          [] {
              P kept;
              kept.key = QStringLiteral("result");
              kept.label = tr("The last run's result, written by it");
              kept.kind = P::Text;
              kept.defaultValue = QJsonValue(QJsonValue::Undefined);
              kept.showIf = QStringLiteral("result=shown");
              return kept;
          }()},
         {}, {}, false, false, QStringLiteral("sweep"),
         tr("The model solved at each value of one or two of its parameters - a width, a length, a permittivity -, each "
            "point a whole solve by the study, the quantities asked read at each: a table and a plot of them, and each "
            "point's S for a Touchstone file. What it found is kept with the model, marked old once the model changes.")});
    // The Optimization: the multiphysics one's settings, as that kind has
    // them (the same defaults, labels and help, so the two cannot drift), but
    // for its study - an RF 2.5D model has one - and its objectives' own
    // columns: a window of frequencies in place of a study and a mode.
    {
        QList<PropertyDef> settings;
        if (const fem::NodeKind* own = fem::nodeKind(QStringLiteral("optimization")))
            for (PropertyDef p : own->properties) {
                if (p.key == QLatin1String("study")) continue;
                if (p.key == QLatin1String("objectives")) {
                    p.choiceLabels = {tr("Expression"), tr("Kind"), tr("Value"), tr("Weight"), tr("Over"), tr("From"), tr("To"), tr("At")};
                    p.columnChoices = {{}, QStringList{QStringLiteral("MIN"), QStringLiteral("MAX"), QStringLiteral("TARGET"), QStringLiteral("LE"),
                                                       QStringLiteral("GE"), QStringLiteral("EQ"), QStringLiteral("MON")},
                                       {}, {}, QStringList{QStringLiteral("worst"), QStringLiteral("last"), QStringLiteral("min"), QStringLiteral("max"), QStringLiteral("at"),
                                                   QStringLiteral("min_at"), QStringLiteral("max_at")},
                                       {}, {}, {}};
                    p.tooltip = tr("Each an expression of the solution's values - S21_dB, Z11_re, GD21, VSWR1, Z0, eps_eff, C11, Lloop - and the "
                                   "parameters, and what is asked of it: MIN as small and MAX as large as it can be; TARGET at Value (least "
                                   "squares); LE, GE and EQ limits at Value; MON only shown. Weight: how much it counts among the others (1). "
                                   "Over the frequencies From to To (all of them when empty): its worst (the default, as its kind judges it), its "
                                   "last, min or max, at the frequency At, or min_at and max_at - where its min or max falls, in Hz.");
                }
                settings << p;
            }
        add({QStringLiteral("rf_optimization"), tr("Optimization"), QStringLiteral("analysis"), QStringLiteral("study"), none, settings, {}, {},
             false, false, QStringLiteral("opt"),
             tr("Varies parameters of the model within their ranges - widths, lengths, a permittivity, a Dimension's By - and solves the model "
                "by its study for each candidate, to reach its objectives, targets and limits: any method of the multiphysics optimizer "
                "(differential evolution and its variants, particle swarm, annealing, CMA-ES, Bayesian, Nelder-Mead, trust region, "
                "Levenberg-Marquardt, NSGA-II's Pareto front, tune), design centering too. The best values and the history in the tab's page, "
                "kept with the model; Apply Optimum writes them into the parameters.")});
    }
    add({QStringLiteral("rf_results"), tr("Results"), QString(), QStringLiteral("results"), none, {}, {}, {}, true, true,
         QString(), tr("What the solve found. Quasi-static: each net's capacitance, each as a line with its RLGC and S "
                       "parameters, and the frequency above which that is no longer the whole story. Full wave: the "
                       "ports' S at each frequency, whether it is reciprocal and passive, and the currents drawn on "
                       "the layout.")});
    add({QStringLiteral("rf_exports"), tr("Exports"), QString(), QStringLiteral("exports"), none, {},
         {QStringLiteral("rf_touchstone"), QStringLiteral("rf_circuit"), QStringLiteral("rf_mp3d"), QStringLiteral("rf_palace"),
          QStringLiteral("rf_gds"), QStringLiteral("rf_dxf")},
         {}, true, true, QString(), tr("What is written out of the model: of a solution, or of its shapes and its stack.")});
    add({QStringLiteral("rf_touchstone"), tr("Touchstone"), QStringLiteral("export"), QStringLiteral("export"), none,
         {file(QStringLiteral("file"), tr("File")),
          choice(QStringLiteral("of"), tr("Of"), {QStringLiteral("line"), QStringLiteral("coupled")},
                 {tr("One net, as a two-port"), tr("Every net, as coupled lines")}, QStringLiteral("line"),
                 tr("Of a quasi-static solve; a full-wave solve writes its ports")),
          text(QStringLiteral("net"), tr("Net"), QString(),
               tr("Which conductor, by the name the solve gave it (empty: the first)")),
          expr(QStringLiteral("length"), tr("Length"), QStringLiteral("m"), QString(),
               tr("How long a piece of line to write (empty: the length that was solved)")),
          expr(QStringLiteral("impedance"), tr("Reference impedance"), QStringLiteral("Ohm"), QStringLiteral("50")),
          choice(QStringLiteral("version"), tr("Version"), {QStringLiteral("1.1"), QStringLiteral("2.0")},
                 {tr("1.1, every port into one reference"), tr("2.0, each port into its own")}, QStringLiteral("1.1"),
                 tr("1.1 is what a schematic's S-parameter file reads; 2.0 carries each full-wave port's own "
                    "reference impedance")),
          choice(QStringLiteral("modes"), tr("Modes"), {QStringLiteral("single"), QStringLiteral("mixed")},
                 {tr("Each port as it is"), tr("Mixed: each pair's differential and common modes")},
                 QStringLiteral("single"),
                 tr("Mixed writes a full-wave solve's differential pairs as their modes, in Touchstone 2.0 with its "
                    "[Mixed-Mode Order]: each pair's differential mode, then each common mode, then the ports of no "
                    "pair"))},
         {}, {}, false, false, QStringLiteral("ts"),
         tr("An .sNp file of the S parameters, at the study's own frequencies: a full-wave solve's ports, in their "
            "order under Ports; or of a quasi-static solve, one net as a line or every net as coupled lines - a "
            "2N-port, the near ends first, then the far ends.")});
    add({QStringLiteral("rf_circuit"), tr("Equivalent circuit"), QStringLiteral("export"), QStringLiteral("export"), none,
         {file(QStringLiteral("file"), tr("File")),
          text(QStringLiteral("name"), tr("Name"), QString(), tr("The SPICE subcircuit's name (empty: the model's)")),
          // (NF7 of the open items: a wide solve fitted below its first
          //  resonance without solving it again.)
          expr(QStringLiteral("from"), tr("From"), QStringLiteral("Hz"), QString(),
               tr("The solve's frequencies it is fitted to, from this (empty: its first) - below a resonance, where a lumped "
                  "circuit holds")),
          expr(QStringLiteral("to"), tr("To"), QStringLiteral("Hz"), QString(), tr("... to this (empty: its last)"))},
         {}, {}, false, false, QStringLiteral("ec"),
         tr("A lumped circuit fitted to a full-wave solve of one or two ports: a pi of R, L and C - between the ports, "
            "and from each to the ground - each branch the form that fits it best, written as a Qucs-S schematic to "
            "place as a part (.sch) or as a SPICE subcircuit (.cir, .sp, .lib), with how far its S is from the "
            "solve's: it holds only while what it stands for is short beside the wavelength.")});
    // The model's shapes and stack written out, to be looked at or worked on
    // elsewhere: no solve needed.
    add({QStringLiteral("rf_mp3d"), tr("Multiphysics 3D"), QStringLiteral("export"), QStringLiteral("export"), none,
         {file(QStringLiteral("file"), tr("File")),
          expr(QStringLiteral("margin"), tr("Margin"), QStringLiteral("m"), QString(),
               tr("How far the stack's slabs reach beyond the shapes on every side (empty: a tenth of the shapes' larger "
                  "side; a lid's side walls, where it has them, give the box instead)"))},
         {}, {}, false, false, QStringLiteral("mp3"),
         tr("The model as a 3D model of Multiphysics 3D (.qfem3d), to open and look at in its panel: a Layer Stack of the "
            "stack's slabs across a window round the shapes, each metal and via drawn up from its shapes as processed - "
            "read from a GDSII file of the same name written beside it -, each with its material (permittivity, loss "
            "tangent, conductivity). The air above is drawn as its frame alone; what is hidden in this tab is hidden there.")});
    // The model as a project of Palace, the open-source 3D solver, to run as
    // it is written.
    add({QStringLiteral("rf_palace"), tr("Palace EM"), QStringLiteral("export"), QStringLiteral("export"), none,
         {file(QStringLiteral("file"), tr("File")),
          choice(QStringLiteral("sides"), tr("Box sides"), {QStringLiteral("perfect"), QStringLiteral("absorbing")},
                 {tr("Perfect conductors"), tr("Absorbing")}, QStringLiteral("perfect"),
                 tr("Where the stack ends across the plane. An absorbing side takes a line's field along it as a resistive wall "
                    "would: a microstrip's loss came out four times its copper's with absorbing sides five heights out, half again "
                    "with them a quarter wave out; with perfect sides, its copper's. Perfect sides near a line take a little of its "
                    "field the other way: its eps_eff some 1 % low five heights out, 0.3 % at twenty - a larger Margin. A lid's side "
                    "walls, where it has them, are the box instead")),
          expr(QStringLiteral("margin"), tr("Margin"), QStringLiteral("m"), QString(),
               tr("How far the stack reaches beyond the shapes (empty: five heights of the stack to perfect sides, a quarter "
                  "wave at the top of the sweep to absorbing ones)")),
          expr(QStringLiteral("air"), tr("Air above"), QStringLiteral("m"), QString(),
               tr("How far the air reaches over the stack, to an absorbing boundary (empty: a quarter wave at the top of the "
                  "sweep); a metal lid closes it at its own height")),
          expr(QStringLiteral("largest"), tr("Largest element"), QStringLiteral("m"), QString(),
               tr("Empty: a tenth of the wavelength in the slowest dielectric at the top of the sweep")),
          expr(QStringLiteral("edges"), tr("Elements at the edges"), QStringLiteral("m"), QString(),
               tr("Along the metals' edges and the ports (empty: a quarter of the narrowest conductor)")),
          choice(QStringLiteral("metals"), tr("Metals"), {QStringLiteral("thick"), QStringLiteral("sheets")},
                 {tr("As thick as they are, cut out of the mesh"), tr("Sheets at their bottoms, as RF 2.5D solves a sheet")},
                 QStringLiteral("thick"),
                 tr("A sheet's thickness is in its loss alone; it meshes with far fewer elements than a thin metal cut out")),
          integer(QStringLiteral("order"), tr("Order"), 2, tr("The finite elements' order, 1 to 4: Palace's Solver Order")),
          choice(QStringLiteral("solver"), tr("Linear solver"), {QStringLiteral("default"), QStringLiteral("ams")},
                 {tr("Palace's default: a sparse direct solver where it has one"), tr("Auxiliary-space multigrid (AMS) alone")},
                 QStringLiteral("default")),
          expr(QStringLiteral("adaptive_tolerance"), tr("Adaptive tolerance"), QString(), QStringLiteral("1e-3"),
               tr("Palace's adaptive sweep: a few frequencies solved and the rest from a reduced model through them, to this "
                  "tolerance (0: every frequency solved)"))},
         {}, {}, false, false, QStringLiteral("pal"),
         tr("The model as a project of Palace, the open-source 3D finite-element solver, ready to run: \"palace -np 4 "
            "<name>.json\" in the folder it is written to. The configuration (.json) holds a driven solve over the study's "
            "sweep, every port driven in turn, and names the mesh beside it (.msh, Gmsh's format, in µm): tetrahedra through "
            "the stack's slabs across a box round the shapes, each metal and via cut out of it - its surface perfect or of its "
            "conductivity -, or each metal a sheet at its bottom, the ground the box's floor, perfect or absorbing sides, an "
            "absorbing boundary over the air. Each port is a lumped port: an edge port's under its face down to the "
            "ground, or across its gaps to the conductors beside it; an internal port's up from its From metal to its To "
            "metal. No solve is needed for it.")});
    add({QStringLiteral("rf_gds"), tr("GDSII"), QStringLiteral("export"), QStringLiteral("export"), none,
         {file(QStringLiteral("file"), tr("File"))},
         {}, {}, false, false, QStringLiteral("gds"),
         tr("The shapes as processed - the layout's and the model's own, cut by the Cutout - as a GDSII file of one cell, "
            "each on the layer and datatype the layer map gives its layer of the stack (a layer it gives none, the next "
            "free number).")});
    add({QStringLiteral("rf_dxf"), tr("DXF"), QStringLiteral("export"), QStringLiteral("export"), none,
         {file(QStringLiteral("file"), tr("File"))},
         {}, {}, false, false, QStringLiteral("dxf"),
         tr("The shapes as processed as a DXF drawing (AutoCAD R12, in µm): each a closed polyline on a DXF layer named "
            "after its layer of the stack, coloured as this tab draws it.")});
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
    // (A finite one: "1/0" read as infinity, a substrate of it no problem
    //  said and the charge solve at work for ever, "0/0" as NaN, a
    //  permittivity of it solved to answers said to be right.)
    if (!std::isfinite(e.constant())) {
        if (why != nullptr) *why = tr("it comes to %1, no finite number").arg(e.constant());
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
    // (Only where it is not shown: a model with nothing hidden is written as
    //  it was before there was any.)
    if (!node.visibility.isEmpty()) o.insert(QStringLiteral("visibility"), node.visibility);
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
 * missing. They are still not read -
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
    {
        const QString v = o.value(QLatin1String("visibility")).toString();
        if (v == QLatin1String("hidden") || v == QLatin1String("frame")) n.visibility = v;
    }
    const NodeKind* kind = nodeKind(n.type);
    const QString who = !n.label.isEmpty() ? n.label : !n.tag.isEmpty() ? n.tag : n.type;
    if (notes != nullptr) {
        static const QStringList keys{QStringLiteral("type"), QStringLiteral("tag"), QStringLiteral("label"),
                                      QStringLiteral("enabled"), QStringLiteral("properties"), QStringLiteral("children"),
                                      QStringLiteral("visibility")};
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

QStringList namesOf(const Node& node, const QString& key)
{
    const QJsonValue v = valueOf(node, key);
    QStringList out;
    if (v.isArray()) {
        for (const QJsonValue& each : v.toArray())
            if (const QString name = each.toString().trimmed(); !name.isEmpty()) out << name;
        return out;
    }
    // (A text of them, as a file written by hand may give: "T1, T2".)
    for (const QString& name : v.toString().split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts))
        out << name;
    return out;
}

QString portName(const Node& node)
{
    return node.label.isEmpty() ? node.tag : node.label;
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

std::complex<double> djordjevicSarkar(double epsr, double tand, double at, double frequency)
{
    using C = std::complex<double>;
    if (!(tand > 0) || !(at > 0)) return C(epsr, -epsr * std::max(0.0, tand));
    constexpr double f1 = 1e3, f2 = 1e12;
    const double span = std::log(f2 / f1);
    const auto shape = [&](double f) { return std::log(C(f2, f) / C(f1, f)) / span; };
    const C given = shape(at);
    // eps(at) = eps_inf + d_eps shape(at) = epsr (1 - j tand).
    const double delta = -epsr * tand / given.imag();
    const double infinite = epsr - delta * given.real();
    return infinite + delta * shape(std::max(0.0, frequency));
}

bool Stack::magnetic() const
{
    for (const Layer& l : layers)
        if ((l.kind == Layer::Dielectric || l.kind == Layer::Substrate) && (l.mur != 1 || l.mtand > 0)) return true;
    return false;
}

bool Stack::dispersive() const
{
    for (const Layer& l : layers)
        if (!l.dispersion.isEmpty()) return true;
    return false;
}

Stack Stack::atFrequency(double frequency) const
{
    Stack out = *this;
    for (Layer& l : out.layers) {
        if (l.dispersion.isEmpty()) continue;
        const std::complex<double> e = djordjevicSarkar(l.epsr, l.tand, l.dispersionAt, frequency);
        // (A uniaxial layer's along z: its susceptibility, eps_z - 1, the same
        //  share of the plane's - not eps_z itself, which took one given at
        //  1.01 to 0.96 at 40 GHz, under 1, and the solve refused a value never
        //  given (the review of 3f31f0c4). One the same as the plane's stays
        //  the plane's.)
        if (l.epsz > 0 && l.epsr > 1) l.epsz = 1 + (l.epsz - 1) * (e.real() - 1) / (l.epsr - 1);
        l.epsr = e.real();
        l.tand = e.real() > 0 ? -e.imag() / e.real() : 0.0;
        l.dispersion.clear();
    }
    return out;
}

const Layer* Stack::named(const QString& name) const
{
    for (const Layer& l : layers)
        if (!name.isEmpty() && (l.name == name || l.tag == name)) return &l;
    return nullptr;
}

bool Stack::isGround(const QString& name) const
{
    for (const Layer& l : layers)
        if (l.kind == Layer::Ground
            && (l.name == name || l.tag == name || name.trimmed().compare(QLatin1String("ground"), Qt::CaseInsensitive) == 0))
            return !name.trimmed().isEmpty();
    return false;
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
    // A new study's full-wave sweep adaptive: a few frequencies solved and
    // the rest read from a rational fit through them, which on a sweep of
    // 100 points or more saves more than any speed of the solve itself (a
    // sweep of 12 or fewer is solved whole). A file that says nothing of it
    // is not adaptive, as it was solved before.
    child(m.root, QStringLiteral("rf_study")).props.insert(QStringLiteral("adaptive"), true);
    // The layout's own fixed children.
    child(m.layout(), QStringLiteral("rf_layer_map"));
    child(m.layout(), QStringLiteral("rf_processing"));
    child(m.layout(), QStringLiteral("rf_cutout"));
    // A stack one can see at once and solve as a line: a ground plane, a
    // substrate, an oxide and one metal. (Without the ground, its first
    // solve was of a strip with nothing to return through - a capacitance
    // to infinity, and no line at all.)
    Node& stack = m.stackNode();
    Node ground;
    ground.type = QStringLiteral("rf_ground");
    ground.tag = QStringLiteral("gnd1");
    ground.label = tr("Ground");
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
    metal.props.insert(QStringLiteral("model"), QStringLiteral("automatic"));   // (a new metal's; a file that says nothing, a sheet)
    stack.children.push_back(ground);
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
        // (Valid JSON that is no object - an array - was said as "does not
        //  read as JSON: no error occurred"
        //  second hour.)
        if (error != nullptr)
            *error = document.isNull() ? tr("It does not read as JSON: %1").arg(parsed.errorString())
                                       : tr("It is JSON, but not an object: an RF 2.5D model is one, beginning \"format\": \"%1\".")
                                             .arg(formatName());
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
    child(m.layout(), QStringLiteral("rf_cutout"));
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
        p.name = tableCell(cells.at(0)).trimmed();
        p.expression = cells.size() > 1 ? tableCell(cells.at(1)).trimmed() : QString();
        p.description = cells.size() > 2 ? cells.at(2).toString() : QString();
        if (!p.name.isEmpty()) list << p;
    }
    return list;
}

Stack Model::stack() const
{
    return stackOf(stackNode(), parameters());
}

Stack Model::solveStack() const
{
    // (Its bond wires' air slabs, metals and legs with it: rf25_bondwire.h.)
    Stack s = stack();
    QStringList unread;
    const QList<BondWire> wires = bondWiresOf(*this, &unread);
    s.problems += unread;
    if (!wires.isEmpty() && s.ok()) {
        const BondWired bw = withBondWires(s, {}, wires);
        if (bw.ok()) s.layers = bw.stack.layers;
        else s.problems += bw.problems;
    }
    return s;
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
    // two things missing).
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
        // (Dispersion said, its frequency read: a file that says none of it
        //  is a dielectric the same at every frequency, as it was.)
        const auto readDispersion = [&] {
            if (textOf(c, QStringLiteral("dispersion")) != QLatin1String("djordjevic_sarkar")) return;
            layer.dispersion = QStringLiteral("djordjevic_sarkar");
            if (c.props.contains(QLatin1String("dispersion_at"))) read(QStringLiteral("dispersion_at"), &layer.dispersionAt);
        };
        // (A file that says nothing of them is not magnetic, as it was.)
        const auto readMagnetic = [&] {
            if (c.props.contains(QLatin1String("mur"))) read(QStringLiteral("mur"), &layer.mur);
            if (c.props.contains(QLatin1String("mtand"))) read(QStringLiteral("mtand"), &layer.mtand);
            // (And anisotropic: none said, isotropic.)
            if (c.props.contains(QLatin1String("epsz"))) read(QStringLiteral("epsz"), &layer.epsz);
            if (c.props.contains(QLatin1String("epsy"))) read(QStringLiteral("epsy"), &layer.epsy);
            // (Averaged where it can be; one below 1 kept as given, for the
            //  stack's check to say - reset to nought, it never could.)
            if (layer.epsy >= 1 && layer.epsy != layer.epsr && layer.epsr > 0) {
                layer.epsx = layer.epsr;
                layer.epsr = std::sqrt(layer.epsx * layer.epsy);
            } else if (layer.epsy >= 1 || layer.epsy == 0) {
                layer.epsy = 0;
            }
        };
        switch (layer.kind) {
        case Layer::Substrate:
            read(QStringLiteral("thickness"), &layer.thickness);
            read(QStringLiteral("epsr"), &layer.epsr);
            read(QStringLiteral("tand"), &layer.tand);
            read(QStringLiteral("sigma"), &layer.sigma);
            if (c.props.contains(QLatin1String("beyond"))) read(QStringLiteral("beyond"), &layer.beyond);
            readDispersion();
            readMagnetic();
            break;
        case Layer::Dielectric:
            read(QStringLiteral("thickness"), &layer.thickness);
            read(QStringLiteral("epsr"), &layer.epsr);
            read(QStringLiteral("tand"), &layer.tand);
            if (c.props.contains(QLatin1String("sigma"))) read(QStringLiteral("sigma"), &layer.sigma);
            if (c.props.contains(QLatin1String("beyond"))) read(QStringLiteral("beyond"), &layer.beyond);
            readDispersion();
            readMagnetic();
            break;
        case Layer::Metal:
            read(QStringLiteral("thickness"), &layer.thickness);
            read(QStringLiteral("sigma"), &layer.sigma);
            read(QStringLiteral("roughness"), &layer.roughness);
            if (textOf(c, QStringLiteral("roughness_model")) == QLatin1String("huray")) {
                layer.huray = true;
                layer.nodule = 0.5e-6;
                layer.surfaceRatio = 2;
                if (c.props.contains(QLatin1String("nodule_radius"))) read(QStringLiteral("nodule_radius"), &layer.nodule);
                if (c.props.contains(QLatin1String("surface_ratio"))) read(QStringLiteral("surface_ratio"), &layer.surfaceRatio);
            }
            layer.twoSheets = textOf(c, QStringLiteral("model")) == QLatin1String("faces");
            layer.automatic = textOf(c, QStringLiteral("model")) == QLatin1String("automatic");
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
            else if (layer.variant == QLatin1String("metal"))
                read(QStringLiteral("sigma"), &layer.sigma);
            layer.lossInFullWave = flagOf(c, QStringLiteral("fullwave_loss"))
                                   && (layer.variant == QLatin1String("metal") || layer.variant == QLatin1String("impedance"));
            break;
        case Layer::Cover:
            layer.variant = textOf(c, QStringLiteral("kind"));
            read(QStringLiteral("height"), &layer.thickness);
            layer.walls = flagOf(c, QStringLiteral("walls"));
            if (layer.walls) {
                double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                read(QStringLiteral("box_x0"), &x0);
                read(QStringLiteral("box_y0"), &y0);
                read(QStringLiteral("box_x1"), &x1);
                read(QStringLiteral("box_y1"), &y1);
                layer.box = QRectF(QPointF(x0, y0), QPointF(x1, y1));
            }
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
    // the passivation sat on top of it.)
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

    // What is wrong with it.
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
        if (l.kind == Layer::Cover && l.thickness < 0 && read("height"))
            out.problems << tr("The cover's height above the top is below nought.");
        if (l.kind == Layer::Cover && l.walls && l.variant != QLatin1String("perfect"))
            out.problems << tr("Side walls want a metal lid: a box is closed, its walls from the ground up to the lid - make the "
                               "cover a metal lid, or take the walls away.");
        if (l.kind == Layer::Cover && l.walls && !(l.box.width() > 0 && l.box.height() > 0) && read("box_x0") && read("box_y0")
            && read("box_x1") && read("box_y1"))
            out.problems << tr("The box's side walls stand round no rectangle: its \"to\" corner must be beyond its \"from\" in x and y.");
        if ((l.kind == Layer::Substrate || l.kind == Layer::Dielectric || l.kind == Layer::Metal) && !(l.thickness > 0)
            && read("thickness"))
            out.problems << tr("%1 has no thickness.").arg(name(l));
        if ((l.kind == Layer::Dielectric || l.kind == Layer::Substrate) && l.epsr < 1 && read("epsr"))
            out.problems << tr("%1 has a relative permittivity below 1 (%2).").arg(name(l)).arg(l.epsr);
        if ((l.kind == Layer::Dielectric || l.kind == Layer::Substrate) && l.beyond < 1 && read("beyond"))
            out.problems << tr("%1 has a relative permittivity beyond its shapes below 1 (%2).").arg(name(l)).arg(l.beyond);
        if ((l.kind == Layer::Dielectric || l.kind == Layer::Substrate) && l.tand < 0 && read("tand"))
            out.problems << tr("%1 has a loss tangent below zero.").arg(name(l));
        if ((l.kind == Layer::Dielectric || l.kind == Layer::Substrate) && !(l.mur > 0) && read("mur"))
            out.problems << tr("%1 has a relative permeability of no more than nought (%2).").arg(name(l)).arg(l.mur);
        if ((l.kind == Layer::Dielectric || l.kind == Layer::Substrate) && l.mtand < 0 && read("mtand"))
            out.problems << tr("%1 has a magnetic loss tangent below zero.").arg(name(l));
        if ((l.kind == Layer::Dielectric || l.kind == Layer::Substrate) && ((l.epsz != 0 && l.epsz < 1) || (l.epsy != 0 && l.epsy < 1))
            && read("epsz") && read("epsy"))
            out.problems << tr("%1 has a relative permittivity along z or y below 1.").arg(name(l));
        if (!l.dispersion.isEmpty() && !(l.dispersionAt > 0) && read("dispersion_at"))
            out.problems << tr("%1's dispersion is given at no frequency above nought.").arg(name(l));
        // Djordjevic-Sarkar's poles lie from 1 kHz to 1 THz: given outside
        // them, or with a loss so high that what is left at an infinite
        // frequency is under 1, it fits a permittivity of no material - 11.9
        // and 0.02 given at 10 Hz made eps_inf -481, and eps' at 40 GHz -405
        // (the review of 3789cfcb).
        else if (!l.dispersion.isEmpty() && (l.dispersionAt < 1e3 || l.dispersionAt > 1e12) && read("dispersion_at"))
            out.problems << tr("%1's dispersion is given at %2 Hz, outside Djordjevic-Sarkar's poles from 1 kHz to 1 THz: give "
                               "its permittivity and loss tangent at a frequency between them.")
                                .arg(name(l))
                                .arg(l.dispersionAt, 0, 'g', 4);
        else if (!l.dispersion.isEmpty() && read("epsr") && read("tand")
                 && djordjevicSarkar(l.epsr, l.tand, l.dispersionAt, 1e30).real() < 1)
            out.problems << tr("%1's loss tangent of %2 is too high for Djordjevic-Sarkar's model at %3 GHz: its permittivity "
                               "at a high frequency would come out at %4, under 1. Give a lower loss tangent, or no "
                               "dispersion.")
                                .arg(name(l))
                                .arg(l.tand, 0, 'g', 3)
                                .arg(l.dispersionAt / 1e9, 0, 'g', 4)
                                .arg(djordjevicSarkar(l.epsr, l.tand, l.dispersionAt, 1e30).real(), 0, 'g', 3);
        if (l.kind == Layer::Metal && !(l.sigma > 0) && read("sigma"))
            out.problems << tr("%1 has no conductivity.").arg(name(l));
        if (l.kind == Layer::Via && !(l.sigma > 0) && read("sigma"))
            out.problems << tr("The via %1 has no conductivity.").arg(name(l));
        if (l.kind == Layer::Metal && l.roughness < 0 && read("roughness"))
            out.problems << tr("%1 has a roughness below zero.").arg(name(l));
        if (l.kind == Layer::Metal && l.huray && !(l.nodule > 0) && read("nodule_radius"))
            out.problems << tr("%1's nodules have no radius.").arg(name(l));
        if (l.kind == Layer::Metal && l.huray && l.surfaceRatio < 0 && read("surface_ratio"))
            out.problems << tr("%1's surface ratio is below nought.").arg(name(l));
        // A via joins the metals it names, or a metal and the ground, and
        // they must be neighbours: nothing but dielectric between them - for
        // the ground, the metal the lowest. (The ground by its name, or by
        // the word "ground" as a port names it.)
        if (l.kind == Layer::Via) {
            const auto conductorAt = [&out, &name](const QString& named) {
                for (int j = 0; j < out.layers.size(); ++j) {
                    const Layer& o = out.layers.at(j);
                    if (o.kind == Layer::Metal && name(o) == named) return j;
                    if (o.kind == Layer::Ground
                        && (name(o) == named || named.compare(QLatin1String("ground"), Qt::CaseInsensitive) == 0))
                        return j;
                }
                return -1;
            };
            const int a = conductorAt(l.from), b = conductorAt(l.to);
            if (l.from.isEmpty() || l.to.isEmpty())
                out.problems << tr("The via %1 does not say which metals it joins.").arg(name(l));
            else if (a < 0 || b < 0)
                out.problems << tr("The via %1 names %2, which is no metal of the stack.")
                                    .arg(name(l), a < 0 ? l.from : l.to);
            else if (a == b)
                out.problems << tr("The via %1 joins %2 to itself.").arg(name(l), l.from);
            else {
                int between = 0;
                const bool a0 = out.layers.at(a).kind == Layer::Ground, b0 = out.layers.at(b).kind == Layer::Ground;
                if (a0 || b0) {
                    const Layer& metal = out.layers.at(a0 ? b : a);
                    for (const Layer& o : std::as_const(out.layers))
                        if (o.kind == Layer::Metal && o.z0 < metal.z0) ++between;
                } else {
                    for (int j = std::min(a, b) + 1; j < std::max(a, b); ++j)
                        if (out.layers.at(j).kind == Layer::Metal) ++between;
                }
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
    // (The board's own layers are what a map or a port may name; the solve
    //  stack's problems, its bond wires' among them, are the model's.)
    const Stack s = stack();
    QStringList said = solveStack().problems;
    // The layout: a file, and a map that names layers of the stack.
    // (A model of its own shapes alone needs no file.)
    const Node* own = layout().child(QStringLiteral("rf_shapes"));
    const bool shapes = own != nullptr && std::any_of(own->children.cbegin(), own->children.cend(), [](const Node& n) {
        return n.enabled && n.type == QLatin1String("rf_shape");
    });
    if (textOf(layout(), QStringLiteral("file")).trimmed().isEmpty() && !shapes)
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
    // The ports: each names conductors that are there - and, one that
    // names its returns, terminals and ports that are (a terminal's name
    // and a port's not the same, or a return would be the one or the other).
    QStringList terminals, driven;
    for (const Node& p : ports().children) {
        if (!p.enabled) continue;
        if (p.type == QLatin1String("rf_terminal")) terminals << portName(p);
        else if (p.type == QLatin1String("rf_edge_port") || p.type == QLatin1String("rf_internal_port")) driven << portName(p);
    }
    for (const Node& p : ports().children) {
        if (!p.enabled) continue;
        const QString who = portName(p);
        const bool named = textOf(p, QStringLiteral("return")) == QLatin1String("terminals")
                           && p.type != QLatin1String("rf_terminal") && p.type != QLatin1String("rf_return");
        for (const QString& key : {QStringLiteral("conductor"), QStringLiteral("from"), QStringLiteral("to"), QStringLiteral("metal")}) {
            // (An internal port that names its returns is from no metal.)
            if (named && key == QLatin1String("from")) continue;
            const QString layer = textOf(p, key).trimmed();
            if (layer.isEmpty() || layer.compare(QLatin1String("ground"), Qt::CaseInsensitive) == 0) continue;
            if (s.named(layer) == nullptr)
                said << (p.type == QLatin1String("rf_terminal")
                             ? tr("The terminal %1 names %2, which is no layer of the stack.").arg(who, layer)
                             : tr("The port %1 names %2, which is no layer of the stack.").arg(who, layer));
        }
        if (p.type == QLatin1String("rf_terminal") && driven.contains(who))
            said << tr("%1 is the name of a terminal and of a port: a port's returns name one or the other, so each needs "
                       "a name of its own.")
                        .arg(who);
        if (!named) continue;
        const QStringList returns = namesOf(p, QStringLiteral("returns"));
        if (returns.isEmpty())
            said << tr("The port %1 returns through the terminals and ports it names, and names none.").arg(who);
        for (const QString& r : returns)
            if (r == who)
                said << tr("The port %1 returns through itself.").arg(who);
            else if (!terminals.contains(r) && !driven.contains(r))
                said << tr("The port %1 returns through %2, which is no terminal or port of the model.").arg(who, r);
    }
    // A TRL calibration (rf25_trl.h): its standards named, and a two-port.
    int driven2 = 0, calibrations = 0;
    for (const Node& p : ports().children) {
        if (!p.enabled) continue;
        if (p.type == QLatin1String("rf_edge_port") || p.type == QLatin1String("rf_internal_port")) ++driven2;
        if (p.type != QLatin1String("rf_trl")) continue;
        ++calibrations;
        if (textOf(p, QStringLiteral("thru")).trimmed().isEmpty()) said << tr("The TRL calibration names no thru.");
        if (textOf(p, QStringLiteral("reflect")).trimmed().isEmpty()) said << tr("The TRL calibration names no reflect.");
        bool line = false;
        for (const QJsonValue& row : p.props.value(QLatin1String("lines")).toArray())
            line = line || !row.toArray().at(0).toString().trimmed().isEmpty();
        if (!line) said << tr("The TRL calibration names no line: one at least, longer than the thru.");
    }
    if (calibrations > 0 && driven2 != 2)
        said << tr("A TRL calibration is of a two-port, and the model has %1 port(s).").arg(driven2);
    // An Optimization's variables: each Type one the optimizer knows (NF4 of
    // the open items - said only by its run, so that rf25_edit took any).
    if (const Node* analyses = root.child(QStringLiteral("rf_analyses")))
        for (const Node& a : analyses->children) {
            if (!a.enabled || a.type != QLatin1String("rf_optimization")) continue;
            for (const QJsonValue& row : a.props.value(QLatin1String("variables")).toArray()) {
                const QJsonArray cells = row.toArray();
                const QString type = cells.size() > 4 ? cells.at(4).toString().trimmed() : QString();
                if (!type.isEmpty() && !qucs_s::optimization::knownVariableType(type))
                    said << tr("%1: %2's Type \"%3\" is not one of LIN_DOUBLE, LOG_DOUBLE, LIN_INT, LOG_INT, E3, E6, E12, E24, E48, E96, E192.")
                                .arg(a.label.isEmpty() ? a.tag : a.label, cells.at(0).toString().trimmed(), type);
            }
        }
    if (ports().children.empty())
        said << tr("The model has no port: there is nothing to drive it with.");
    // What the solve refuses of the shapes and the study, said at the edit
    // too: a strip "1/0" long, or a sweep of 200,001 points, went in with no
    // problem said, where a substrate's eps_r of 1/0 was said here.
    ownShapesOf(*this, &said);
    // (The study's refusal whole, in the solve's own words: a from of
    //  -1 GHz, a to of 1/0 or no points were the solve's alone - the review
    //  of 1213eeef.)
    if (root.child(QStringLiteral("rf_study")) != nullptr) {
        QString why;
        studySweep(*this, &why);
        if (!why.isEmpty()) said << why;
    }
    return said;
}

// ------------------------------------------------------------------ The tree

QString tableCell(const QJsonValue& cell)
{
    if (cell.isDouble()) {
        const double v = cell.toDouble();
        return v == std::floor(v) && std::abs(v) < 1e15 ? QString::number(qint64(v)) : QString::number(v, 'g', 15);
    }
    return cell.toString();
}

void Model::normalise()
{
    // (A table's cells as text: a layer map written with numbers - [[1, 0,
    //  "M1"]] - read as no row at all, its shapes nowhere, and a shape drawn
    //  into it went to layer "/".)
    std::function<void(Node&)> cells = [&](Node& n) {
        for (auto it = n.props.begin(); it != n.props.end(); ++it) {
            if (!it.value().isArray() || it.key() != QLatin1String("rows")) continue;
            QJsonArray rows = it.value().toArray();
            bool changed = false;
            for (qsizetype r = 0; r < rows.size(); ++r) {
                if (!rows.at(r).isArray()) continue;
                QJsonArray row = rows.at(r).toArray();
                for (qsizetype c = 0; c < row.size(); ++c)
                    if (row.at(c).isDouble()) {
                        row.replace(c, tableCell(row.at(c)));
                        changed = true;
                    }
                rows.replace(r, row);
            }
            if (changed) it.value() = rows;
        }
        for (Node& child : n.children) cells(child);
    };
    cells(root);
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
    // metals are M1, M2..., the dielectrics ILD1..., the ports P1.... One
    // past the highest of its stem in use, by any node: an internal port
    // counted among internal ports alone was P1 beside an edge port P1.
    const auto numbered = [this](const QString& stem) {
        int highest = 0;
        std::function<void(const Node&)> walk = [&](const Node& node) {
            if (node.label.startsWith(stem)) {
                bool number = false;
                const int n = node.label.mid(stem.size()).toInt(&number);
                if (number && n > highest) highest = n;
            }
            for (const Node& c : node.children) walk(c);
        };
        walk(root);
        return stem + QString::number(highest + 1);
    };
    if (type == QLatin1String("rf_metal")) {
        n.label = numbered(QStringLiteral("M"));
        n.props.insert(QStringLiteral("model"), QStringLiteral("automatic"));   // (a new metal's; a file that says nothing, a sheet)
    }
    else if (type == QLatin1String("rf_dielectric")) n.label = numbered(QStringLiteral("ILD"));
    else if (type == QLatin1String("rf_via")) n.label = numbered(QStringLiteral("V"));
    else if (type == QLatin1String("rf_edge_port") || type == QLatin1String("rf_internal_port"))
        n.label = numbered(QStringLiteral("P"));
    else if (type == QLatin1String("rf_return")) n.label = numbered(QStringLiteral("G"));
    else if (type == QLatin1String("rf_terminal")) n.label = numbered(QStringLiteral("T"));
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
        // it.)
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
        rename(p, QStringLiteral("metal"));
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
    // The Cutout's regions that cut it alone.
    if (Node* cutout = layout().child(QStringLiteral("rf_cutout")))
        for (Node& r : cutout->children) {
            QStringList layers = namesOf(r, QStringLiteral("layers"));
            if (!layers.contains(from)) continue;
            for (QString& l : layers)
                if (l == from) l = to;
            r.props.insert(QStringLiteral("layers"), layers.join(QStringLiteral(", ")));
        }
}

void Model::renamePort(const QString& from, const QString& to)
{
    if (from.isEmpty() || to.isEmpty() || from == to) return;
    for (Node& p : ports().children) {
        if (textOf(p, QStringLiteral("pair")).trimmed() == from) p.props.insert(QStringLiteral("pair"), to);
        if (!p.props.contains(QLatin1String("returns"))) continue;
        QStringList returns = namesOf(p, QStringLiteral("returns"));
        if (!returns.contains(from)) continue;
        for (QString& r : returns)
            if (r == from) r = to;
        p.props.insert(QStringLiteral("returns"), QJsonArray::fromStringList(returns));
    }
}

bool canHide(const Node& node)
{
    const NodeKind* k = kindOf(node);
    return k != nullptr && k->group == QLatin1String("stack");
}

namespace {

void clearVisibility(Node& node)
{
    node.visibility.clear();
    for (Node& c : node.children) clearVisibility(c);
}

} // namespace

Model withoutVisibility(const Model& model)
{
    Model m = model;
    clearVisibility(m.root);
    return m;
}

QHash<QString, QString> layerVisibility(const Model& model)
{
    QHash<QString, QString> out;
    for (const Node& n : model.stackNode().children)
        if (!n.visibility.isEmpty() && canHide(n)) out.insert(nameOf(n), n.visibility);
    return out;
}

} // namespace qucs_s::rf25
