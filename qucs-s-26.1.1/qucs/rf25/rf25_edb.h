/*
 * rf25_edb.h - an HFSS™ 3D Layout project read without any Ansys® software:
 *              its .aedb's edb.def, and the .aedt beside it
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Nothing documents edb.def. What follows was found in eleven public samples
 * of its versions 8.6 to 12.1 (github.com/ansys/example-data, .../pyaedt):
 *
 *   - a byte, a length and a text block "Hdr": Version, Encrypted;
 *   - a table of the file's record classes: -1, how many, and for each its id,
 *     how many fields, and each field's number and type;
 *   - text blocks of $begin and $end, each after its length: the project's
 *     (materials, padstacks, variables), and each cell's: its name, variables,
 *     setups and extents; its stack, one SLayer(...) a layer;
 *     each via's use of a padstack (def=, its layers fl= and tl=);
 *   - after a cell's text, lists of records, each written by the table:
 *
 *         0 a byte; 1 an int32; 2 a double; 3 a double and the expression it
 *         is the value of; 4 a string (int32 length, bytes); 5 a record (its
 *         class, -1 for none; a class's field of type 5 is what it derives
 *         from); 6 a list (int32 size, int32 type of its items, the items);
 *         7 an array (int32 size, each item its type and then its value).
 *
 * The reader goes by the file's own table, so a version that adds a field to a
 * class reads the same. It is strict: a class the table has not, a length past
 * the end or a list that does not end where the next begins stops it, and what
 * stopped it is said - a shape is never guessed at.
 */
#ifndef QUCS_RF25_EDB_H
#define QUCS_RF25_EDB_H

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QPointF>
#include <QPolygonF>
#include <QString>
#include <QStringList>

namespace qucs_s::rf25 {

struct EdbLayer {
    QString name;
    int id = -1;
    bool signal = false;
    double thickness = 0, elevation = 0;   ///< m
    QString material, fill;
};

struct EdbMaterial {
    double epsr = 1, tand = 0, sigma = 0;
};

/// A shape as a polygon in µm: a rectangle, a circle or a path made one, an
/// arc made chords. A void is a shape whose owner is another's id.
struct EdbShape {
    int id = -1;
    int layer = -1;        ///< a layer's id
    int net = -1;
    int owner = -1;        ///< the shape it is a void of; -1: none
    QString kind;          ///< "polygon", "rectangle", "circle", "path"
    QString name;
    QPolygonF outline;
};

/// A padstack on one layer: its pad, and its anti-pad - what a via clears in
/// metal of another net. A shape is "Cir", "Sq", "Rct", "No", ...
struct EdbPadLayer {
    QString layer;         ///< the layer's name, or one of the padstack's own ("s1"): then in the stack's order
    QString padShape, antiShape;
    QList<double> padSizes, antiSizes;   ///< m
};

struct EdbPadstack {
    QString name;
    QString hole;          ///< its shape: "Cir", "Sq", "Rct", ...
    QList<double> sizes;   ///< m
    QStringList layers;    ///< the layers it has pads on, upper first
    QList<EdbPadLayer> onLayers;   ///< what it has on each of them, upper first
    QString material;
    double plating = 100;  ///< percent of the hole's radius
};

struct EdbVia {
    int id = -1, net = -1;
    QString padstack, name;
    QPointF at;            ///< µm
    double hole = 0;       ///< m, a number it keeps beside its padstack's hole, whether or not it is the same: of use only where the padstack has none
    double rotation = 0;   ///< radians
    int usage = -1;        ///< which use of a definition it is
    int fromLayer = -1, toLayer = -1;   ///< the layers it spans, by their ids; -1: its definition's
};

/// A terminal: a port where it has an impedance of its own, else what another
/// returns through.
struct EdbPort {
    QString name;
    QString kind;          ///< "edge", "point", "via"
    int id = -1;
    int primitive = -1;    ///< an edge terminal's shape, a via terminal's via
    QPointF at;            ///< µm
    int layer = -1;        ///< a layer's id; -1 or -100: none said
    int reference = -1;    ///< the terminal it returns through; -1: none said
    QString referenceName; ///< "layer:net:shape", what the HFSS tab names
    QString hfssType;      ///< "Gap", "Wave", ...
    QString impedanceText;
    double impedance = 50; ///< Ohm, what it is renormalized to
    double deembed = 0;    ///< m
    bool portSolver = true;
    bool isPort = false;
};

struct EdbSweep {
    QString name, setup;
    QString data;                 ///< "LINC 1GHz 10GHz 101", as the setup says it
    QList<double> frequencies;    ///< Hz, where it lists them
    double adaptive = 0;          ///< Hz, the setup's adaptive frequency
};

/// What a cell of an edb.def holds, with what was found on the way.
struct EdbDesign {
    QString version;
    QString name;                 ///< the cell's
    QString mode;                 ///< its stack's: "Laminate", "Overlap", ...
    QList<EdbLayer> layers;
    QHash<QString, EdbMaterial> materials;   ///< by name, lower case
    QHash<int, QString> nets;
    QList<EdbShape> shapes;
    QHash<QString, EdbPadstack> padstacks;
    QList<EdbVia> vias;
    QList<EdbPort> ports;
    QList<EdbSweep> sweeps;
    QStringList notes;
    QHash<QString, double> variables;   ///< SI, by name
    int unread = 0;               ///< records of a kind passed over
};

/*!
 * The design in \a bytes, an edb.def: the cell named \a wanted, else the one
 * with the most shapes. False and why in \a error where the file is none, is
 * encrypted, or holds what the reader does not know: nothing half-read is
 * handed on as a design - \a out then still has the stack and materials the
 * text gave, and says what stopped it in its notes.
 */
bool readEdb(const QByteArray& bytes, const QString& wanted, EdbDesign* out, QString* error);

/// What a project's .aedt says of its 3D Layout design.
struct AedtProject {
    QString design;               ///< the first 3D Layout design's name
    QHash<QString, EdbMaterial> materials;
    QList<EdbSweep> sweeps;
    /// Each port's properties as it keeps them, by name.
    QHash<QString, QHash<QString, QString>> ports;
    QStringList portOrder;
    bool found = false;           ///< whether it has a 3D Layout design at all
};
AedtProject readAedt(const QString& text);

/// \a text a length ("0.4mm", "1.5e-6", "$w/2+1um") in metres, \a variables in
/// scope: a number with a unit, or an expression of them. False where it is
/// neither.
bool lengthOf(const QString& text, const QHash<QString, double>& variables, double* metres);

} // namespace qucs_s::rf25

#endif
