/*
 * rf25_stackfile.h - an RF 2.5D model's stack written to and read from the
 *                    stack files of other solvers: HFSS™ 3D Layout's XML
 *                    stackup and EMX's process file
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

#include <QHash>
#include <QJsonArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace qucs_s::rf25 {

/// A stack file's format, by what it is called here.
enum class StackFormat { Hfss, Emx };

/// The format of \a path by its suffix (.xml, .proc); none for another.
std::optional<StackFormat> stackFormatOf(const QString& path);

/// A stack file written: its text, and where it is not what RF 2.5D solves.
struct StackWritten {
    QString text;
    QStringList notes;
    QStringList problems;     ///< why nothing can be written
    int layers = 0, vias = 0, mapped = 0;   ///< what it holds; mapped: GDSII layers named
    bool ok() const { return problems.isEmpty(); }
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

// ---- Shared with the HFSS 3D Layout import (rf25_hfss_import.cpp) ----

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
