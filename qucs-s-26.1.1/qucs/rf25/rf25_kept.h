/*
 * rf25_kept.h - an RF 2.5D model's solution kept beside it, so that a model
 *               solved once is not solved again each time it is opened
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_RF25_KEPT_H
#define QUCS_RF25_KEPT_H

#include "rf25_fullwave.h"
#include "rf25_inductance.h"
#include "rf25_model.h"
#include "rf25_static.h"

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>

namespace qucs_s::rf25 {

/*!
 * What a solve found, kept in a file of its own beside the model -
 * "<model>.solution", which sorts with it and goes where it is copied -
 * with what it was solved from: the model's own JSON and the layout file's
 * bytes, each by its SHA-1, and the cells of the mesh it was solved on. A
 * model opened again takes it only where all three are the same: an edit,
 * another layout, a mesh made otherwise - a newer mesher's - and it is
 * solved again as before.
 *
 * A full-wave solve's whole answer is kept - the ports' S and Z, what the
 * de-embedding did, the checks, the currents for the drawing -, or a
 * quasi-static one's extraction and the loop's inductance with it; and the
 * build that solved it and when.
 */
struct KeptSolution {
    QByteArray modelHash;         ///< SHA-1 of what the model gives the solver (solveStampOf())
    QByteArray layoutHash;        ///< SHA-1 of the layout file's bytes; empty where it names none
    int cells = 0;                ///< the mesh's cells it was solved on
    QString solvedBy;             ///< the build: "27.0.5 (934b0de)"
    QDateTime solvedAt;           ///< UTC
    std::shared_ptr<const FullWave> fullWave;     ///< a full-wave solve's answer; or
    QStringList waveNotes;                        ///< (what reading its ports said)
    std::shared_ptr<const Extraction> extraction; ///< a quasi-static one's,
    std::shared_ptr<const Inductance> inductance; ///< and its loop's, where it had one
    QString portsSaid;                            ///< what its ports were read as
    double solvedLength = 0;                      ///< m, the length its per-metre values are of
};

/// Where \a model's solution is kept: beside it, "<model>.solution".
QString solutionPathOf(const QString& model);
/// SHA-1 of \a bytes, as the solution's stamps are.
QByteArray stampOf(const QByteArray& bytes);
/// What \a model gives the solver, stamped: its JSON without its exports,
/// which change nothing that is solved - an export added or its file named
/// leaves a solution standing, and kept.
QByteArray solveStampOf(const Model& model);
/// \a kept written to \a path, whole or not at all; false and why if not.
bool writeSolution(const QString& path, const KeptSolution& kept, QString* error = nullptr);
/// The solution kept at \a path; none where there is none, or it is not
/// one this build reads (\a why says which).
std::optional<KeptSolution> readSolution(const QString& path, QString* why = nullptr);

} // namespace qucs_s::rf25

#endif
