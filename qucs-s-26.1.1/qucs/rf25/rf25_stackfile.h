/*
 * rf25_stackfile.h - an RF 2.5D model's stack written to and read from the
 *                    stack files of other solvers: HFSS™ 3D Layout's XML
 *                    stackup and EMX's process file - and drawn, as EMX
 *                    draws a process, as a PDF of its cross-section
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * docs/feature_gaps/2026-11-09-rf25-stack-files.md says why it is what it
 * is. The stackup is the control file's <Stackup> alone, what Layout >
 * Layers > Stackup > Import XML and Export XML read and write. The
 * process file is EMX's: layers from the bottom up,
 * conductors in them, vias between conductors, and the GDSII layers each
 * conductor and via is drawn on.
 *
 * None of it knows of a tab: a test hands it a model.
 */
#ifndef QUCS_RF25_STACKFILE_H
#define QUCS_RF25_STACKFILE_H

#include "rf25_edb.h"
#include "rf25_model.h"

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QLineF>
#include <QList>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QStringList>

#include <optional>

class QPainter;

namespace qucs_s::rf25 {

/// A stack file's format, by what it is called here. A PDF is a drawing of
/// the stack, written and never read.
enum class StackFormat { Hfss, Emx, Pdf };

/// The format of \a path by its suffix (.xml, .proc, .pdf); none for another.
std::optional<StackFormat> stackFormatOf(const QString& path);

/// A stack file written: its text (a drawing's bytes in \a data), and where
/// it is not what RF 2.5D solves.
struct StackWritten {
    QString text;
    QByteArray data;          ///< a PDF's bytes; empty for a file of text
    QStringList notes;
    QStringList problems;     ///< why nothing can be written
    int layers = 0, vias = 0, mapped = 0;   ///< what it holds; mapped: GDSII layers named
    int pages = 0;            ///< a drawing's pages
    bool ok() const { return problems.isEmpty(); }
    /// What goes in the file.
    QByteArray bytes() const { return data.isEmpty() ? text.toUtf8() : data; }
};

/// \a model's stack as an HFSS 3D Layout XML stackup (a control file with
/// its <Stackup> alone). Each metal and via the layer map names is written
/// by its GDSII layer as well, so the same file serves a GDSII import.
/// \a name is the model's, \a madeBy what its header says wrote it.
StackWritten hfssStackupOf(const Model& model, const QString& name = {}, const QString& madeBy = {});

/// \a model's stack as an EMX process file: a define for each metal and via
/// from the layer map, the layers from the bottom up with each metal in a
/// layer of its own room, the vias, and the air without end on top.
StackWritten emxProcessOf(const Model& model, const QString& name = {}, const QString& madeBy = {});

/// A stack file read: the Stack's children from the bottom up, and the rows
/// of a layer map it gives ([layer, datatype, stack layer]).
struct StackRead {
    StackFormat format = StackFormat::Hfss;
    QList<Node> nodes;
    QJsonArray rows;
    QStringList layers;       ///< the stack as said back, from the bottom up
    QStringList notes;        ///< what is not as it was, what was left out
    int vias = 0;
};

/// \a path read as a stack (its format by its suffix, else by what it
/// begins with). None, and why in \a error, where it cannot be read.
std::optional<StackRead> stackFileRead(const QString& path, QString* error = nullptr);
/// The same of \a text, in \a format; \a name says what it is called.
std::optional<StackRead> stackTextRead(const QByteArray& text, StackFormat format, const QString& name,
                                       QString* error = nullptr);

/// \a model with \a read as its stack: the Stack's children in its place,
/// and the layer map's rows for the layers \a read maps in place of those it
/// had for them. What it leaves named that is no longer there is said in
/// \a notes.
Model withStackOf(const Model& model, const StackRead& read, QStringList* notes = nullptr);

// ---- The cross-section drawn (rf25_stackdrawing.cpp) ----

/*!
 * The stack's cross-section as EMX's --print-process draws a process: the
 * levels where one layer gives way to the next, each layer's thickness and
 * permittivity beside it, the metals and vias as boxes in columns - a via
 * between the metals it joins - and a key of the metals and vias below.
 * Lengths in metres, z from the ground plane up.
 */
struct StackDrawing {
    /// A layer between two levels, from the bottom up, each on the last.
    struct Band {
        enum Kind { Substrate, Dielectric, Room, Gap };   ///< Room: a metal's, of what fills it; Gap: under a lid
        Kind kind = Dielectric;
        QString name;
        double z0 = 0, z1 = 0;
        QStringList words;    ///< what is said of it beside it: "h=2 µm", "ε=3.9"
    };
    /// A metal or a via, in its column.
    struct Box {
        QString name;
        bool via = false;
        double z0 = 0, z1 = 0;
        int column = 0;
    };
    QString title;
    QList<Band> bands;
    QList<Box> boxes;
    int columns = 0;
    QString ground;           ///< what the ground is, said; "" with no plane
    QString top;              ///< what closes it at the top: the air without end, or the lid
    bool lid = false;
    QStringList metals, vias; ///< the key, top first
    QStringList problems;     ///< why it cannot be drawn
    bool ok() const { return problems.isEmpty(); }
};

/// \a model's stack drawn. \a name is the model's.
StackDrawing stackDrawingOf(const Model& model, const QString& name = {});

/*!
 * A drawing laid out on pages of a size in points (y down, as a painter
 * has it): every text, box and line where it goes. The key goes on a page
 * of its own where the layers fill the first.
 */
struct StackSheet {
    struct Text {
        QRectF rect;
        QString text;
        double points = 8;
        int align = 0;        ///< Qt::Alignment
        bool bold = false, grey = false;
    };
    struct Shape {
        enum Kind { Metal, Via, Ground, Lid };
        Kind kind = Metal;
        QRectF rect;
        QString name;
    };
    struct Line {
        QLineF line;
        bool dashed = false;
    };
    struct Page {
        QList<Text> texts;
        QList<Shape> shapes;
        QList<Line> lines;
    };
    QSizeF size;
    QList<Page> pages;
    QList<double> levels, levelY;   ///< each level's z (m) and where it is drawn (pt), from the bottom up
    QList<QRectF> bandRects;        ///< each band's rows, as StackDrawing::bands
    /// Where z (m) is drawn, between the levels.
    double yOf(double z) const;
};

/// \a drawing laid out on pages of \a size points. \a footer goes at the
/// foot of each page.
StackSheet stackSheetOf(const StackDrawing& drawing, const QSizeF& size, const QString& footer = {});

/// One page of a sheet painted, in points.
void paintStackPage(QPainter& painter, const StackSheet::Page& page);

/// \a model's stack drawn as a PDF, on A4, or US Letter where the locale
/// measures in inches. \a name is the model's, \a madeBy what drew it.
StackWritten stackPdfOf(const Model& model, const QString& name = {}, const QString& madeBy = {});

// ---- Shared with the HFSS 3D Layout import (rf25_hfss_import.cpp) ----

/// The first dielectric above layer \a i short of another metal or the
/// cover: what fills a metal's room, as the export to HFSS 3D Layout has it.
/// Null: the air.
const Layer* fillAbove(const Stack& stack, int i);

/// The stack a model has of layers placed in z: dielectrics, cut where the
/// metals take room, and the metals (signal layers) between them, from the
/// bottom up. \a plane names the signal layer that becomes the ground (none:
/// an open ground), \a lid the one that becomes a metal cover (none: no
/// cover). \a viaRuns: signal layers each on the last take turns as metal and
/// via, as a GDSII-derived stack has them. What is read is said in
/// \a notes. The metals' names, the project's to the model's, in \a nameOf.
QList<Node> stackOfLayers(const QList<EdbLayer>& layers, const QHash<QString, EdbMaterial>& materials,
                          const QString& plane, const QString& lid, bool viaRuns, QStringList* said,
                          QStringList* notes, QHash<QString, QString>* nameOf = nullptr);

/// \a text, an XML stackup or control file called \a name, read as a stack
/// (its <Stackup> alone: what else it holds is said and left).
std::optional<StackRead> hfssStackupRead(const QByteArray& text, const QString& name, QString* error = nullptr);

} // namespace qucs_s::rf25

#endif
