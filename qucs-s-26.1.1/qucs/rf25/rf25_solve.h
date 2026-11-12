/*
 * rf25_solve.h - an RF 2.5D model solved without its tab: what the tab's
 *                Solve reads out of the model (the job), the solve itself on
 *                the caller's thread, and the whole of it from a model and its
 *                layout as read - for a sweep's points and an optimization's
 *                candidates, solved as the tab would solve them
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_RF25_SOLVE_H
#define QUCS_RF25_SOLVE_H

#include "rf25_adaptive.h"
#include "rf25_dielectric.h"
#include "rf25_fullwave.h"
#include "rf25_inductance.h"
#include "rf25_layout.h"
#include "rf25_mesh.h"
#include "rf25_model.h"
#include "rf25_static.h"

#include <QList>
#include <QPointF>
#include <QString>
#include <QStringList>

#include <atomic>
#include <memory>
#include <optional>

namespace qucs_s::layout {
struct Layout;
}

namespace qucs_s::rf25 {

/// What a solve may be asked to be in place of what the study says - a
/// tool's question, which must leave the model as it was.
struct SolveRequest {
    QString solver;              ///< "quasi_static" or "full_wave"; empty: the study's
    QList<double> frequencies;   ///< a full-wave solve's, Hz; empty: the study's sweep
    QString matrix;              ///< "automatic", "dense" or "compressed"; empty: the study's Matrix
    /// A full-wave sweep's points added where its answer changes
    /// (rf25_adaptive.h): 1 yes, 0 no, -1 as the study says. Never of
    /// frequencies asked for, nor of a convergence check.
    int adaptive = -1;
    /// A mesh convergence check rather than a solve: the cells across
    /// the narrowest conductor of each mesh, the coarsest first (a full
    /// wave at \a frequencies, the top of the study's sweep where none
    /// are given). Empty: a solve.
    QList<int> levels;
};

/// A solve on a thread of its own: the shapes and the cells as they
/// stand, and the stack read out of the model, all copied so that an edit
/// while it runs changes nothing under it.
struct SolveJob {
    // What it solves, stamped as it starts - the model's JSON and the
    // layout file's bytes - for the solution kept beside the model.
    QByteArray modelStamp, layoutStamp;
    Mesh mesh;
    QList<Conductor> conductors;
    Stack stack;
    QList<Extent> extents;       ///< the layers that end (rf25_dielectric.h)
    /// A TRL calibration's standards (rf25_trl.h), where the model has one:
    /// each a model's file, a line's how much longer than the thru.
    struct Trl {
        QString thru, reflect;
        QList<QPair<QString, double>> lines;
        bool shortReflect = false;
        double impedance = 0;    ///< the lines', Ohm; nought: not given
        double epsGuess = 1;
    };
    std::optional<Trl> trl;
    QList<ReturnMark> returns;   ///< the Return Conductors under Ports
    double length = 0;
    std::shared_ptr<Extraction> result;
    // The loop's inductance, where two ports say what the loop is: the
    // magnetic half of the extraction, solved on the same thread and the
    // same copies as the charge.
    bool loop = false;
    QPointF in, out;
    double frequency = 0;
    QString portsSaid;
    std::shared_ptr<Inductance> inductance;
    // A full-wave solve's: the ports as the model has them, the sweep, and
    // what reading the ports said; and the flag that stops it between
    // frequencies, with the count it keeps of them.
    bool fullWave = false;
    QList<FullWavePort> ports;
    QList<double> frequencies;
    QStringList notes;
    std::shared_ptr<FullWave> wave;
    std::atomic_bool stop{false};
    std::atomic_int done{0};
    // A compressed solve's GMRES under way, for the bar (FullWaveControl).
    std::atomic_int gmresIterations{0};
    std::atomic<double> gmresResidual{0};
    std::atomic_int atOnce{0};   // (frequencies at a time, once the solve knows)
    // The ports' de-embedding after it: the mesh's settings, so that the
    // strips of their feeds are meshed as the structure was, and whether
    // those strips are being solved; and how far they have got, for the
    // bar - the calibrations to make and begun, and of the one going on the
    // strips begun, the frequencies they have and how many are solved.
    MeshSettings how;
    double top = 0;
    std::atomic_bool calibrating{false};
    std::atomic_int calibrations{0}, calibrationsBegun{0}, stripsBegun{0}, stripFrequencies{0}, calibrated{0};
    FullWaveControl::Matrix matrix = FullWaveControl::Matrix::Automatic;   ///< the study's
    // A quasi-static extraction's: its step, how far into it, and the images
    // of the Green's function it built last (ExtractControl).
    std::atomic_int step{ExtractControl::Starting}, stepDone{0}, stepAll{0}, images{0};

    // An adaptive sweep's (rf25_adaptive.h): whether it is one, its
    // tolerance on S, and the frequencies it has asked for so far, which
    // the bar counts against.
    bool adaptive = false;
    double tolerance = 2e-3;
    std::atomic_int asked{0};
    // A mesh convergence check's: the cells across of each mesh, the one it
    // is at, and what it found.
    QList<int> levels;
    std::atomic_int levelAt{0};
    std::shared_ptr<MeshConvergence> check;

    // The tab's holdSolves() and allowFrequencies(), which a test holds it
    // back by.
    std::shared_ptr<std::atomic_bool> held;
    std::shared_ptr<std::atomic_int> allowed;
};


/// How the study asks a full-wave solve to hold its system (its Matrix).
FullWaveControl::Matrix studyMatrix(const Model& model);
/// Whether the model's mesh is for a full-wave solve held compressed.
bool meshedCompressed(const Model& model);
/// How the tab meshes \a model: its Mesh node's settings, held compressed as
/// its study is, and narrow gaps graded.
MeshSettings tabMeshSettings(const Model& model, QStringList* said);
/// The model's Return Conductors as the solvers take them; \a problem where
/// one cannot be read, and nothing returned.
QList<ReturnMark> returnMarksOf(const Model& model, QString* problem);
/// The model's ports as the full-wave solve takes them; \a problem where one
/// cannot be read, and nothing returned.
QList<FullWavePort> portsOf(const Model& model, QString* problem);
/// A way a face is said ("+x"), as a direction; nought for "auto" or none.
QPointF facingOf(const QString& facing);
/// Whether the model's study is a full-wave one.
bool studyIsFullWave(const Model& model);
/// What a solve's mesh is against what it cuts: the first of its lines.
QString meshNoteOf(const Mesh& mesh);

/// \a job made ready to solve \a model on \a mesh of \a processed (the
/// model's, as the tab makes them), as \a request asks: the conductors, the
/// stack, the layers that end, the ports or the loop, the frequencies, a TRL
/// calibration's standards (their files found from \a folder, the model's
/// own). Empty, or what keeps it from being solved. Its stamps and its holds
/// are the caller's.
QString setUpSolve(SolveJob& job, const Model& model, const Processed& processed, const Mesh& mesh, const SolveRequest& request,
                   const QString& folder);
/// \a job solved on this thread, as it is set up: full-wave, de-embedded,
/// adaptive, calibrated as it asks; else the quasi-static extraction and the
/// loop's inductance. (Not a convergence check: convergeOn().)
void runSolve(SolveJob& job);
/// A full-wave solve of \a mesh at \a frequencies, its ports de-embedded
/// where they ask - as \a job has them -, the strips of its ports' feeds
/// meshed as \a how says; \a adaptive, its points added where its answer
/// changes (\a tolerance on S). What reading the ports said goes to \a notes.
std::shared_ptr<FullWave> solveWaveOn(SolveJob& job, const Mesh& mesh, const MeshSettings& how, const QList<double>& frequencies,
                                      bool adaptive, double tolerance, QStringList* notes);
/// \a wave calibrated by the job's TRL standards (rf25_trl.h), each solved
/// here at its frequencies.
std::shared_ptr<FullWave> trlOn(SolveJob& job, const FullWave& wave);
/// A convergence check's meshes, each solved.
void convergeOn(SolveJob& job);

/// A model solved as its tab would solve it, on this thread.
struct ModelSolution {
    std::shared_ptr<const Processed> processed;
    std::shared_ptr<const Mesh> mesh;
    std::shared_ptr<const Extraction> extraction;   ///< a quasi-static solve's
    std::shared_ptr<const Inductance> inductance;   ///< its loop's, where two ports make one
    std::shared_ptr<const FullWave> wave;           ///< a full-wave solve's
    QStringList notes;     ///< what reading the ports said
    QString portsSaid;     ///< why there is no loop, or which ports make it
    double length = 0;     ///< m: a uniform line's own length, else the longer side solved
    QString problem;       ///< why there is no answer
    bool stopped = false;  ///< \a job.stop was set
    bool ok() const { return problem.isEmpty() && !stopped && (extraction || wave); }
};
/*!
 * \a model solved from \a read, its layout already in memory (\a called its
 * file's name, \a folder the model's own): processed, meshed, set up and
 * solved on this thread, as the tab's Solve does - the same answer to the
 * last digit. \a job holds the solve's controls: another thread may set its
 * \a stop. Not a convergence check.
 */
ModelSolution solveModel(SolveJob& job, const Model& model, const qucs_s::layout::Layout& read, const QString& called, const QString& folder,
                         const SolveRequest& request = {});

} // namespace qucs_s::rf25

#endif
