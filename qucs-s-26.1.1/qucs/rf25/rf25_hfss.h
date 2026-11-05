/*
 * rf25_hfss.h - an RF 2.5D model written for HFSS™ 3D Layout and read back
 *               from it, and its S compared with a Touchstone file's: what
 *               a benchmark of the solver against Ansys® HFSS wants
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * docs/feature_gaps/2026-11-04-rf25-hfss-3d-layout.md says why it is what it
 * is. A 3D Layout project is an .aedt, which is text, and an .aedb, whose
 * edb.def nothing documents: it cannot be written here, so what is written
 * is what Electronics Desktop itself makes a project of - a script, and a
 * GDSII file with its XML control file. What is read is the script back, a
 * control file with its GDSII, or a project: its .aedt and the .aedb beside
 * it, without any Ansys software.
 *
 * None of it knows of a tab: a test hands it a model and its shapes.
 */
#ifndef QUCS_RF25_HFSS_H
#define QUCS_RF25_HFSS_H

#include "rf25_edb.h"
#include "rf25_fullwave.h"
#include "rf25_layout.h"
#include "rf25_model.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace qucs_s::rf25 {

/// How a model is written for HFSS 3D Layout.
struct HfssOptions {
    /// An edge port's HFSS Type: "wave", "gap", or "auto" - a wave port where
    /// the model takes its own discontinuity out (its reference plane is at
    /// its edge, as a wave port's is), else a gap.
    QString portType = QStringLiteral("auto");
    double margin = -1;       ///< m, the dielectrics and a plane past the shapes; below nought: five heights of the stack
    double air = -1;          ///< m, the air box past the stack; below nought: a quarter wave at the adaptive frequency
    double adaptive = 0;      ///< Hz, the adaptive frequency; nought: the top of the sweep
    double maxDelta = 0.02;   ///< the adaptive solve's Delta S
    int maxPasses = 20;
    bool discrete = false;    ///< a discrete sweep; else interpolating
    /// m: "signal layers thinner than this are modeled as zero thickness",
    /// the setup's own option - RF 2.5D's sheets, for a like with like.
    /// Nought: every metal as thick as it is.
    double thinMetal = 0;
    /// The folder of the model, kept in the script: its layout file is found
    /// there when the script has moved. Empty: none said.
    QString modelFolder;
};

/// A polygon of the export, its keyholes opened: an outline counter-clockwise
/// and its holes, in µm. The numbers CreateEdgePort takes are of its outline.
struct HfssPolygon {
    QString name;
    QString layer;
    QString net;
    QPolygonF outline;
    QList<QPolygonF> holes;
};

/// What is written: the three files' contents, and what a reader must know.
struct HfssExport {
    QString name;             ///< the design's
    QString script;           ///< the .py
    QString control;          ///< the .xml
    /// What the .gds holds: its shapes by layer number (datatype 0), a plane
    /// a rectangle.
    QList<QPair<int, QPolygonF>> gdsShapes;
    QStringList notes;        ///< where it is not the same as what RF 2.5D solves
    QStringList problems;     ///< why nothing can be written
    int layers = 0, materials = 0, polygons = 0, voids = 0, vias = 0, planes = 0, ports = 0;
    QStringList portSaid;     ///< each port as it came out: "P1: wave port on M1, referenced to ..."
    QString setup, sweep, sweepData;
    double adaptive = 0, marginUsed = 0, airUsed = 0;
    bool ok() const { return problems.isEmpty(); }
};

/*!
 * \a model and its shapes, \a conductors as the processing leaves them (µm),
 * written for HFSS 3D Layout. \a name is the design's (empty: "RF25"),
 * \a madeBy what the files' headers say wrote them. \a returns, one for each
 * of \a conductors, says which a coplanar port may return through, as the
 * full-wave solve finds them (empty: any beside it).
 */
HfssExport hfssExportOf(const Model& model, const QList<Conductor>& conductors, const HfssOptions& options = {},
                        const QString& name = {}, const QString& madeBy = {}, const QList<bool>& returns = {});

/// Writes \a written beside each other: \a scriptPath, and the .gds and the
/// .xml of its name. False and why in \a error; those it wrote in \a files.
bool writeHfssExport(const HfssExport& written, const QString& scriptPath, QStringList* files, QString* error);

/// Writes \a shapes (µm) as a GDSII file of one cell. False and why.
bool writeGdsOf(const QList<QPair<int, QPolygonF>>& shapes, const QString& path, const QString& cell, QString* error);

/// What a file of a 3D Layout project read as an RF 2.5D model gives.
struct HfssImport {
    QString kind;              ///< "exported script", "control file" or "project"
    std::optional<Model> model;
    /// An existing layout file its Layout may keep naming (absolute); empty:
    /// \a shapes are written, as a GDSII file beside the model.
    QString layoutFile;
    QList<QPair<int, QPolygonF>> shapes;
    QStringList notes;         ///< what was read, what is not as it was, what is missing
    QStringList problems;      ///< why it is not ready to solve as it stands (it is still a model)
    QStringList layers;        ///< its stack from the bottom up, for what is said of it
    int polygons = 0, vias = 0, ports = 0;
    double from = 0, to = 0;   ///< Hz, its sweep; nought where it has none
    int points = 0;
};

/*!
 * \a path read as a model: an exported script (.py, by its "#qucs-s" line), an
 * XML control file (.xml, with the GDSII of its name beside it, else
 * \a layout), or a project: its .aedt, its .aedb folder or that folder's
 * edb.def. Nothing is written. No model, and why in \a error, where it cannot
 * be read as any of them.
 */
HfssImport hfssImportOf(const QString& path, const QString& layout = {}, QString* error = nullptr);

/// \a read as a model at \a modelPath: the .qrf25, and the .gds of its name
/// where its shapes want one. False and why in \a error; those it wrote in
/// \a files.
bool writeHfssImport(const HfssImport& read, const QString& modelPath, QStringList* files, QString* error);

/// How far two sets of S are apart, term by term, over the frequencies both
/// have.
struct SCompared {
    struct Term {
        int i = 0, j = 0;          ///< from 1
        double worst = 0;          ///< at most |S - S'|
        double worstAt = 0;        ///< Hz, where
        double rms = 0;            ///< over the frequencies compared
        double dbWorst = 0;        ///< at most | |S| dB - |S'| dB |, where |S| is above -60 dB
        double degWorst = 0;       ///< at most the difference of their angles, where |S| is above -40 dB
    };
    QList<Term> terms;
    QList<double> frequencies;     ///< Hz, those compared
    QList<QList<QList<Complex>>> ours, theirs;   ///< [frequency][i][j], each into the reference compared in
    double reference = 0;          ///< Ohm, of \a theirs as the file gave it; nought: its own ports' (unchanged)
    double worst = 0, worstAt = 0;
    int worstI = 0, worstJ = 0;
    int interpolated = 0;          ///< frequencies read between a file's points
    int ports = 0;
    QStringList notes;
    QString problem;
    bool ok() const { return problem.isEmpty(); }
};

/*!
 * \a solved beside the Touchstone file \a path, each port's S into the
 * reference the file says - HFSS writes every port into one - and read at
 * \a solved's frequencies, interpolated between the file's in magnitude and
 * angle. \a order, where it is given, says which port of the file each of
 * \a solved's is (from 1). The file's own format and units are its; what it
 * does not hold is said.
 */
SCompared compareWithTouchstone(const FullWave& solved, const QString& path, const QList<int>& order = {});

/// That comparison as a table of text: frequency, then each term's |S| in dB
/// and angle in degrees, solved and from the file.
QString comparisonTable(const SCompared& compared, const QString& solvedBy, const QString& theirs);

} // namespace qucs_s::rf25

#endif
