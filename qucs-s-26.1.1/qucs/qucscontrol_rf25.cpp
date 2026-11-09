/*
 * qucscontrol_rf25.cpp - Claude's tools for an RF 2.5D model: rf25_solve,
 *                        by the full-wave solver the ports' S, or by the
 *                        quasi-static one the capacitance of its conductors
 *                        and each net as a line with its RLGC and
 *                        S-parameters; and rf25_export, which writes a
 *                        Touchstone of them
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The tool the proposal's "Claude's tools" asks for over the solver of
 * phase B (docs/proposals/2026-10-23-rf-2p5d-moment-method.md). It drives
 * the tab's own solve rather than the library underneath, so that what a
 * tool answers and what the panel shows are the same run.
 */

#include "qucscontrol.h"
#include "qucscontrol_p.h"

#include "multiphysics/fem_expr.h"
#include "main.h"
#include "qucs.h"
#include "rf25/rf25_inductance.h"
#include "rf25/rf25_mesh.h"
#include "rf25/rf25_sparams.h"
#include "rf25/rf25_static.h"
#include "rf25doc.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QScopeGuard>

using namespace qucs_s::rf25;
using namespace qucs_s::control;
namespace fem = qucs_s::fem;

namespace {

/// \a text as a number, the model's own parameters in scope, so that
/// "800[um]" and a parameter's name both work. Nothing where it is no
/// constant above zero.
std::optional<double> positiveNumber(const QString& text, const RF25Doc& model)
{
    fem::Scope scope(&fem::Scope::builtins());
    for (const fem::Model::Parameter& param : model.model().parameters()) {
        const fem::Expression p = fem::Expression::compile(param.expression, scope);
        if (p.isValid() && p.isConstant()) scope.setConstant(param.name, p.constant(), p.dim());
    }
    const fem::Expression given = fem::Expression::compile(text, scope);
    if (!given.isValid() || !given.isConstant() || !(given.constant() > 0)) return std::nullopt;
    return given.constant();
}

/// \a m's real and imaginary parts, and its size in decibels and its angle
/// in degrees where \a polar: each a list of rows.
QJsonObject matrixJson(const QList<QList<std::complex<double>>>& m, bool polar)
{
    QJsonArray re, im, db, deg;
    for (const QList<std::complex<double>>& row : m) {
        QJsonArray r, i, d, a;
        for (const std::complex<double>& v : row) {
            r.append(v.real());
            i.append(v.imag());
            d.append(20 * std::log10(std::max(1e-300, std::abs(v))));
            a.append(std::arg(v) * 180 / M_PI);
        }
        re.append(r);
        im.append(i);
        db.append(d);
        deg.append(a);
    }
    QJsonObject out;
    out.insert(QStringLiteral("re"), re);
    out.insert(QStringLiteral("im"), im);
    if (polar) {
        out.insert(QStringLiteral("dB"), db);
        out.insert(QStringLiteral("degrees"), deg);
    }
    return out;
}

/*!
 * What a full-wave solve of \a model found, as rf25_solve answers it: its
 * ports, the ports' S and Z at the frequency nearest \a asked (the top
 * where none is), each S term's size over the sweep, and whether it is
 * reciprocal and passive. \a brief keeps to the ports, the top's S in
 * decibels and the checks.
 */
QJsonObject fullWaveAnswer(const RF25Doc& model, const FullWave& w, double asked, bool brief)
{
    QJsonObject out;
    out.insert(QStringLiteral("model"), QFileInfo(model.getDocName()).fileName());
    out.insert(QStringLiteral("solver"), QStringLiteral("full wave"));
    // (Kept beside the model, where it is saved: shown when it is opened again.)
    if (!model.keptFile().isEmpty()) out.insert(QStringLiteral("kept beside the model"), QFileInfo(model.keptFile()).fileName());
    const int np = int(w.portNames.size()), nf = int(w.frequencies.size());
    QJsonArray ports;
    for (int p = 0; p < np; ++p) {
        QJsonObject port;
        port.insert(QStringLiteral("name"), w.portNames.at(p));
        port.insert(QStringLiteral("reference impedance"), w.references.at(p));
        port.insert(QStringLiteral("drives"), w.portsSaid.value(p));
        // What it returns through, where that is more than the ground or the
        // metal under it - a coplanar port's conductors beside it, or the
        // terminals and ports it names -: each return's cells, and its share
        // of the port's current at the top of the sweep (an ampere in, the
        // other ports open), which a symmetric GSG shares half and half.
        if (!w.returnsOf.value(p).isEmpty()) {
            QJsonArray returns;
            for (const PortReturn& r : w.returnsOf.at(p)) {
                QJsonObject back;
                back.insert(QStringLiteral("name"), r.name);
                back.insert(QStringLiteral("cells"), r.cells);
                back.insert(QStringLiteral("share"), std::round(std::abs(r.share) * 1e6) / 1e6);
                back.insert(QStringLiteral("phase degrees"), std::round(std::arg(r.share) * 180 / M_PI * 1e3) / 1e3);
                returns.append(back);
            }
            port.insert(QStringLiteral("returns through"), w.namesReturns.value(p) ? QStringLiteral("the terminals and ports it names")
                                                                                  : QStringLiteral("the conductors beside it"));
            port.insert(QStringLiteral("returns"), returns);
            port.insert(QStringLiteral("shares at"), *std::max_element(w.frequencies.begin(), w.frequencies.end()));
        }
        // Its own discontinuity taken out, at the top of the sweep: where
        // its reference plane is, the line of its feed, and the
        // discontinuity as a shunt C at its face and a series L at the port.
        const PortDeembedding d = w.deembedding.value(p);
        if (d.done) {
            QJsonObject de;
            de.insert(QStringLiteral("reference plane"),
                      d.shift == 0 ? QStringLiteral("at its edge")
                                   : QStringLiteral("%1 m %2 from its edge").arg(std::abs(d.shift)).arg(d.shift > 0 ? "in" : "out"));
            de.insert(QStringLiteral("shift"), d.shift);
            de.insert(QStringLiteral("discontinuity taken out"), d.calibrated);
            de.insert(QStringLiteral("moved along"), d.givenLine ? QStringLiteral("a given line") : QStringLiteral("its feed"));
            de.insert(QStringLiteral("line impedance"), d.impedance.real());
            de.insert(QStringLiteral("line eps_eff"), d.epsEff);
            if (d.calibrated) {
                de.insert(QStringLiteral("line dB/m"), 20 / std::log(10.0) * d.alpha);
                // (Where its strips read it as gaining - their own ends
                //  coupling past it - and its plane was moved along it as
                //  losing nothing: the most they read.)
                if (d.gave < 0) de.insert(QStringLiteral("line dB/m as read"), 20 / std::log(10.0) * d.gave);
                de.insert(QStringLiteral("shunt C"), d.shuntC);
                de.insert(QStringLiteral("series L"), d.seriesL);
                // Where they were taken - the top of the sweep - and, where
                // its strips do not agree on one line there, that they are
                // not to be trusted (the check of 2 November's G8: 166 Ohm,
                // eps_eff 5.36 and 2374 dB/m at 100 GHz, the strips one line
                // only to 58 GHz, printed as if they were).
                const double top = *std::max_element(w.frequencies.begin(), w.frequencies.end());
                de.insert(QStringLiteral("taken at"), top);
                if (!d.topAgrees)
                    de.insert(QStringLiteral("not to be trusted"),
                              QCoreApplication::translate("QucsControl", "%1, taken at %2 GHz: its strips do not agree on one line from %3 GHz")
                                  .arg(d.givenLine ? QCoreApplication::translate("QucsControl", "the shunt C and series L (the line is the one given)")
                                                   : QCoreApplication::translate("QucsControl", "the line, the shunt C and the series L"))
                                  .arg(top / 1e9, 0, 'g', 4)
                                  .arg(d.offFrom / 1e9, 0, 'g', 4));
                de.insert(QStringLiteral("calibration strip"), d.length * 1e-6);
                de.insert(QStringLiteral("longer strip"), d.longer * 1e-6);
                // The third strip, which checks the other two: up to where
                // they agree on one line within a percent, the most they
                // differ and where, and the shorter strips solved beside
                // them where they did not.
                de.insert(QStringLiteral("third strip"), d.third * 1e-6);
                de.insert(QStringLiteral("strips agree to"), d.agreedTo);
                de.insert(QStringLiteral("strips differ by at most"), d.worst);
                de.insert(QStringLiteral("strips differ most at"), d.worstAt);
                if (d.strips.size() > 1) {
                    QJsonArray strips;
                    for (const double l : d.strips) strips.append(l * 1e-6);
                    de.insert(QStringLiteral("shorter strips solved"), strips);
                }
                // How far its feed runs straight before the structure begins,
                // beside how far its own field reaches along it.
                if (d.feedLength >= 0) de.insert(QStringLiteral("feed straight for"), d.feedLength * 1e-6);
                if (d.reach > 0) de.insert(QStringLiteral("its field reaches"), d.reach * 1e-6);
                // A coplanar feed's strips are its grounds and vias too; with
                // rows of vias, the line begins at the first.
                if (d.coplanar) {
                    de.insert(QStringLiteral("coplanar feed"), true);
                    if (d.firstVia > 0) de.insert(QStringLiteral("first via"), d.firstVia * 1e-6);
                }
            }
            port.insert(QStringLiteral("de-embedded"), de);
        }
        ports.append(port);
    }
    out.insert(QStringLiteral("ports"), ports);
    out.insert(QStringLiteral("frequencies"), nf);
    out.insert(QStringLiteral("from"), w.frequencies.first());
    out.insert(QStringLiteral("to"), w.frequencies.last());
    int k = nf - 1;
    if (asked > 0)
        for (int i = 0; i < nf; ++i)
            if (std::abs(w.frequencies.at(i) - asked) < std::abs(w.frequencies.at(k) - asked)) k = i;
    QJsonObject s = matrixJson(w.s.at(k), true);
    s.insert(QStringLiteral("frequency"), w.frequencies.at(k));
    QJsonObject checks;
    checks.insert(QStringLiteral("reciprocal"), w.asymmetry <= 1e-9);
    checks.insert(QStringLiteral("asymmetry"), w.asymmetry);
    checks.insert(QStringLiteral("passive"), w.gain <= 1e-6);
    checks.insert(QStringLiteral("most returned"), 1 + w.gain);
    // (A de-embedding can leave what its strips and the structure's own feed
    //  differ by: the solve's own checks beside it.)
    if (!w.zSolved.isEmpty()) {
        checks.insert(QStringLiteral("passive as solved"), w.solvedGain <= 1e-6);
        checks.insert(QStringLiteral("reciprocal as solved"), w.solvedAsymmetry <= 1e-9);
    }
    // What makes it, or part of it, not to be trusted - the brief answer
    // too; and where the stack carries a second surface wave, the top of a
    // microstrip's band on it.
    if (!w.warnings.isEmpty()) out.insert(QStringLiteral("warnings"), QJsonArray::fromStringList(w.warnings));
    if (w.secondWave > 0) out.insert(QStringLiteral("second surface wave from"), w.secondWave);
    if (w.gain > 1e-6) checks.insert(QStringLiteral("most returned at"), w.gainAt);
    // The differential pairs' modes (mixedModeOf()): each pair's
    // differential mode, then each common mode, then the ports of no pair,
    // into twice, half and their own reference.
    QJsonObject mixed;
    if (!w.pairs.isEmpty()) {
        QStringList modes, of;
        mixed = matrixJson(mixedModeOf(w, k, &modes, &of), true);
        mixed.insert(QStringLiteral("frequency"), w.frequencies.at(k));
        mixed.insert(QStringLiteral("modes"), QJsonArray::fromStringList(modes));
        mixed.insert(QStringLiteral("of"), QJsonArray::fromStringList(of));
        const int pairs = int(w.pairs.size());
        QJsonArray references;
        QList<bool> paired(np, false);
        for (const double factor : {2.0, 0.5})
            for (const QPair<int, int>& pair : w.pairs) {
                references.append(factor * w.references.at(pair.first));
                paired[pair.first] = paired[pair.second] = true;
            }
        for (int p = 0; p < np; ++p)
            if (!paired.at(p)) references.append(w.references.at(p));
        mixed.insert(QStringLiteral("reference impedances"), references);
        // Over the sweep, in decibels: each pair's terms, SDD21 the
        // differential mode out of the second pair for the differential mode
        // into the first; for more than two pairs, the first column and the
        // diagonal of the two modes' own blocks.
        QJsonArray sweep;
        for (int f = 0; f < nf && !brief; ++f) {
            const QList<QList<std::complex<double>>> m = mixedModeOf(w, f);
            QJsonObject point;
            point.insert(QStringLiteral("frequency"), w.frequencies.at(f));
            for (int j = 0; j < 2 * pairs; ++j)
                for (int i = 0; i < 2 * pairs; ++i) {
                    const bool rowD = i < pairs, columnD = j < pairs;
                    const int a = i % pairs, b = j % pairs;
                    if (pairs > 2 && (rowD != columnD || (b != 0 && a != b))) continue;
                    point.insert(QStringLiteral("S%1%2%3%4_dB")
                                     .arg(rowD ? QLatin1Char('D') : QLatin1Char('C'))
                                     .arg(columnD ? QLatin1Char('D') : QLatin1Char('C'))
                                     .arg(a + 1)
                                     .arg(b + 1),
                                 20 * std::log10(std::max(1e-300, std::abs(m.at(i).at(j)))));
                }
            sweep.append(point);
        }
        if (!brief) mixed.insert(QStringLiteral("sweep"), sweep);
    }
    // How the system was held: dense, or compressed and solved by iteration
    // - what that kept, and how far GMRES went.
    QJsonObject matrix;
    matrix.insert(QStringLiteral("held"), w.compressed ? QStringLiteral("compressed") : QStringLiteral("dense"));
    if (w.compressed) {
        matrix.insert(QStringLiteral("tolerance"), w.tolerance);
        matrix.insert(QStringLiteral("memory MB"), std::round(w.memory / 1e5) / 10);
        matrix.insert(QStringLiteral("dense MB"), std::round(w.denseMemory / 1e5) / 10);
        matrix.insert(QStringLiteral("largest rank"), w.largestRank);
        matrix.insert(QStringLiteral("iterations"), w.iterations);
        matrix.insert(QStringLiteral("residual"), w.residual);
    } else if (!w.frequencies.isEmpty()) {
        // Whose LU, and a frequency's fill and LU, each on its own thread.
        const double nf = double(w.frequencies.size());
        matrix.insert(QStringLiteral("LU by"), w.accelerated ? QStringLiteral("Accelerate") : QStringLiteral("Eigen"));
        matrix.insert(QStringLiteral("fill s"), std::round(w.fillSeconds / nf * 1e3) / 1e3);
        matrix.insert(QStringLiteral("LU s"), std::round(w.luSeconds / nf * 1e3) / 1e3);
    }
    if (brief) {
        out.insert(QStringLiteral("matrix"), matrix);
        QJsonObject few;
        few.insert(QStringLiteral("frequency"), w.frequencies.at(k));
        few.insert(QStringLiteral("dB"), s.value(QStringLiteral("dB")));
        out.insert(QStringLiteral("s matrix"), few);
        if (!mixed.isEmpty()) {
            QJsonObject modes;
            for (const char* key : {"modes", "of", "frequency", "dB"}) modes.insert(QLatin1String(key), mixed.value(QLatin1String(key)));
            out.insert(QStringLiteral("mixed-mode"), modes);
        }
        out.insert(QStringLiteral("checks"), checks);
        return out;
    }
    out.insert(QStringLiteral("s matrix"), s);
    if (!mixed.isEmpty()) out.insert(QStringLiteral("mixed-mode"), mixed);
    QJsonObject z = matrixJson(w.z.at(k), false);
    z.insert(QStringLiteral("frequency"), w.frequencies.at(k));
    out.insert(QStringLiteral("z matrix"), z);
    // Each term over the sweep, in decibels: every one for a few ports, and
    // for many the first port's column and each port's own reflection.
    QJsonArray sweep;
    for (int f = 0; f < nf; ++f) {
        QJsonObject point;
        point.insert(QStringLiteral("frequency"), w.frequencies.at(f));
        for (int j = 0; j < np; ++j)
            for (int i = 0; i < np; ++i) {
                if (np > 4 && i != j && j != 0) continue;
                point.insert(QStringLiteral("S%1%2_dB").arg(i + 1).arg(j + 1),
                             20 * std::log10(std::max(1e-300, std::abs(w.s.at(f).at(i).at(j)))));
            }
        sweep.append(point);
    }
    out.insert(QStringLiteral("sweep"), sweep);
    out.insert(QStringLiteral("checks"), checks);
    QJsonObject unknowns;
    unknowns.insert(QStringLiteral("runs of current"), w.runs);
    unknowns.insert(QStringLiteral("cells"), w.cells);
    out.insert(QStringLiteral("unknowns"), unknowns);
    out.insert(QStringLiteral("matrix"), matrix);
    out.insert(QStringLiteral("threads"), w.threads);
    QJsonArray said;
    for (const QString& line : model.preparedLines()) said.append(line);
    for (const QString& line : w.lines) said.append(line);
    for (const QString& line : model.fullWaveSummary()) said.append(line);
    out.insert(QStringLiteral("what it did"), said);
    out.insert(QStringLiteral("note"),
               QCoreApplication::translate(
                   "QucsControl",
                   "Full wave: the mixed-potential integral equation on the mesh - currents on the runs between cells "
                   "that share an edge, charges on the cells - with the stack's retarded Green's functions, solved at "
                   "each frequency. Each port drives the face of the conductor it is on (an edge port, its current "
                   "back through the ground - or, a coplanar one, through the nearest return conductors beside it, tied as a probe's "
                   "grounds are) or the cells under it (an internal port: its rectangle's, or the one under its point), its reference plane at the centres of the "
                   "cells it drives; or, one that names its returns, against the terminals and ports it names, tied, a source between "
                   "its cells and theirs with no path up to them (the probe's own loop not in it). Its S is into its own reference "
                   "impedance (Kurokawa's power waves). The "
                   "ground plane, where there is one, is taken as perfect - with none, every port returns through the "
                   "layout's conductors - and a metal as a sheet, its edges losing as its thickness lets the current "
                   "crowd there. A pair's mixed-mode S is its differential mode into twice the ports' reference and "
                   "its common mode into half. rf25_export writes these as a Touchstone."));
    return out;
}

/// A model closed while a call waited on it - its tab closed by the user, or
/// by another call -: said, and nothing of it touched after (RF25Doc::Held).
QJsonObject closedWhileWaited(const QString& name)
{
    return errorResult(QucsControl::tr("%1 was closed while this call waited on it: what it was doing was stopped, and "
                                       "there is nothing of it to answer.")
                           .arg(name));
}

} // namespace

QJsonObject QucsControl::rf25Solve(const QJsonObject& args)
{
    // The model: the one named, else the one in front.
    QucsDoc* doc = nullptr;
    const QString path = args.value(QLatin1String("path")).toString().trimmed();
    if (path.isEmpty()) {
        doc = a_app->getDoc();
    } else {
        const QJsonObject opened = openDocument(QJsonObject{{QStringLiteral("path"), path}});
        if (opened.value(QStringLiteral("isError")).toBool()) return opened;
        doc = a_app->getDoc();
    }
    auto* model = dynamic_cast<RF25Doc*>(doc);
    if (model == nullptr)
        return errorResult(tr("%1 is no RF 2.5D model (.qrf25): rf25_solve is of a planar structure's stack and layout.")
                               .arg(doc != nullptr ? QFileInfo(doc->getDocName()).fileName() : tr("nothing is open")));
    // (Held from here to its answer: its tab closed meanwhile - by the user, by
    //  another call -, it is not deleted under this call, which says so.)
    const RF25Doc::Held held(model);
    const QString heldName = QFileInfo(model->getDocName()).fileName();

    const int timeout = waitsFor(QStringLiteral("rf25_solve"), args);
    // Cancelled by its client while it waits (the user stopped Claude): its
    // solve stopped, or not begun - a solve it joined, someone else's, left
    // to go on.
    QPointer<RF25Doc> solving(model);
    auto cancelled = std::make_shared<bool>(false);
    auto startedHere = std::make_shared<bool>(false);
    const int stop = stoppable(running(), [solving, cancelled, startedHere] {
        *cancelled = true;
        if (solving && *startedHere) solving->stopSolving();
    });
    const auto over = qScopeGuard([this, stop] { stopped(stop); });
    // The shapes and the cells first: the tab makes them at every edit,
    // and a solve is of them as they stand.
    const bool laidOut = model->waitForLayout(timeout * 1000);
    if (model->closedWhenFree()) return closedWhileWaited(heldName);
    if (!laidOut)
        return errorResult(tr("Its layout is still being read after %1 s.").arg(timeout));
    const bool prepared = model->waitForPrepared(timeout * 1000);
    if (model->closedWhenFree()) return closedWhileWaited(heldName);
    if (!prepared)
        return errorResult(tr("Its shapes and its mesh are still being made after %1 s.").arg(timeout));

    // Which solver: the one named, else the study's own - for this solve
    // alone, the model left as it is.
    RF25Doc::SolveRequest request;
    if (args.contains(QLatin1String("solver"))) {
        request.solver = args.value(QLatin1String("solver")).toString().trimmed();
        if (request.solver != QLatin1String("quasi_static") && request.solver != QLatin1String("full_wave"))
            return errorResult(tr("'solver' is \"quasi_static\" or \"full_wave\"; %1 is neither.").arg(request.solver));
    }
    const bool fullWave = request.solver.isEmpty() ? model->studyIsFullWave() : request.solver == QLatin1String("full_wave");
    // How a full-wave solve holds its system: the one named, else the
    // study's Matrix.
    if (args.contains(QLatin1String("matrix"))) {
        request.matrix = args.value(QLatin1String("matrix")).toString().trimmed();
        if (request.matrix != QLatin1String("automatic") && request.matrix != QLatin1String("dense")
            && request.matrix != QLatin1String("compressed"))
            return errorResult(tr("'matrix' is \"automatic\", \"dense\" or \"compressed\"; %1 is none of them.")
                                   .arg(request.matrix));
        if (!fullWave) return errorResult(tr("'matrix' is the full-wave solve's: the quasi-static one is dense."));
    }
    // The frequency asked after: of the losses for the quasi-static solve,
    // and for the full-wave one the one frequency solved.
    double asked = 0;
    if (args.contains(QLatin1String("frequency"))) {
        const std::optional<double> given = positiveNumber(args.value(QLatin1String("frequency")).toString().trimmed(), *model);
        if (!given) return errorResult(tr("'frequency' is a frequency above zero (\"10[GHz]\")."));
        asked = *given;
    }
    if (fullWave && asked > 0) request.frequencies = {asked};

    if (*cancelled || !solving) return errorResult(tr("Cancelled by the client before it was solved."));
    // A solve going on already - the tab's, or an earlier call's that ran
    // past its timeout: one asked alike is joined, waited for rather than
    // started again; one asked otherwise is refused, not started beside
    // it. A second rf25_solve once let the first go and started another,
    // and the first, unstoppable, ran on beside it (the review of 3
    // November: two cores, twelve gigabytes and climbing).
    const bool joined = model->solvingAlike(request);
    if (model->isSolving() && !joined)
        return errorResult(tr("%1 is solving already, as it was asked otherwise (%2): rf25_solve asked as it was waits for "
                              "that solve, and rf25_stop stops it.")
                               .arg(QFileInfo(model->getDocName()).fileName(), model->solvingSaid()));
    if (!joined) {
        *startedHere = true;
        model->solveWith(request);
    }
    const bool solved = model->waitForSolved(timeout * 1000);
    if (model->closedWhenFree()) return closedWhileWaited(heldName);
    if (!solved) {
        // It goes on: stopped here, the next call started it from nothing
        // and its time went for nought. Where the time goes decides what
        // would make it shorter - the stack's Green's functions are no
        // quicker on a coarser mesh.
        const int step = model->quasiStaticStep();
        QString why;
        if (step == ExtractControl::Greens)
            why = tr(" The time is in the stack's Green's functions, which the mesh does not change: fewer layers, or "
                     "thin ones of permittivities nearer their neighbours', build sooner.");
        else if (step >= ExtractControl::Charge)
            why = tr(" A coarser mesh solves sooner: fewer cells across a conductor, or a larger largest cell (the Mesh "
                     "node).");
        else if (fullWave)
            why = tr(" A coarser mesh, or fewer frequencies, solve sooner.");
        return errorResult(tr("It is still solving after %1 s: %2 It goes on - rf25_solve asked again waits for it, and "
                              "rf25_stop stops it.%3")
                               .arg(timeout)
                               .arg(model->solvingSaid(), why));
    }
    if (fullWave) {
        const std::shared_ptr<const FullWave> w = model->fullWave();
        if (!w || !w->ok())
            return errorResult(!model->solveProblem().isEmpty() ? model->solveProblem()
                                                                : tr("It solved to nothing, and said no reason."));
        return jsonResult(fullWaveAnswer(*model, *w, asked, args.value(QLatin1String("brief")).toBool()));
    }
    const std::shared_ptr<const Extraction> e = model->extraction();
    if (!e || !e->ok())
        return errorResult(!model->solveProblem().isEmpty()
                               ? model->solveProblem()
                               : tr("It solved to nothing, and said no reason."));

    // The length a line is taken over: the one given, else the longer
    // side of what was solved.
    double length = model->solvedLength();
    if (args.contains(QLatin1String("length"))) {
        const std::optional<double> given = positiveNumber(args.value(QLatin1String("length")).toString().trimmed(), *model);
        if (!given) return errorResult(tr("'length' is a length above zero (\"800[um]\")."));
        length = *given;
    }

    // The frequency the metal's and the dielectrics' losses are taken at:
    // the one given, else the top of the model's own sweep.
    const double frequency = asked > 0 ? asked : topFrequency(model->model());
    // The ports the S-parameters are referred to.
    double zref = 50;
    if (args.contains(QLatin1String("impedance"))) {
        const std::optional<double> given = positiveNumber(args.value(QLatin1String("impedance")).toString().trimmed(), *model);
        if (!given) return errorResult(tr("'impedance' is a reference impedance above zero (\"50\")."));
        zref = *given;
    }

    QJsonObject out;
    out.insert(QStringLiteral("model"), QFileInfo(model->getDocName()).fileName());
    if (!model->keptFile().isEmpty()) out.insert(QStringLiteral("kept beside the model"), QFileInfo(model->keptFile()).fileName());
    QJsonArray nets;
    for (int i = 0; i < e->names.size(); ++i) {
        QJsonObject net;
        net.insert(QStringLiteral("name"), e->names.at(i));
        net.insert(QStringLiteral("capacitance"), e->selfCapacitance(i));
        QJsonArray row, bare;
        for (int j = 0; j < e->c.at(i).size(); ++j) {
            row.append(e->c.at(i).at(j));
            bare.append(e->c0.at(i).at(j));
        }
        net.insert(QStringLiteral("c"), row);
        net.insert(QStringLiteral("c0"), bare);
        // A return conductor is what the lines come back through; one of
        // several conductors - or signals, where there are returns - is no
        // line by itself: its own row is neither of a pair's modes, which
        // "coupled lines" gives.
        const bool back = e->returns.value(i, false);
        if (back) net.insert(QStringLiteral("return"), true);
        const int count = e->hasReturns() ? int(e->signalNets().size()) : int(e->names.size());
        if (back || count > 1) {
            net.insert(QStringLiteral("line not given"),
                       back ? tr("%1 is a return conductor: the lines are of the signals, against it.").arg(e->names.at(i))
                            : tr("%1 is one of %2 coupled %3, which have modes rather than a line each: see \"coupled "
                                 "lines\".")
                                  .arg(e->names.at(i))
                                  .arg(count)
                                  .arg(e->hasReturns() ? tr("signals") : tr("conductors")));
            nets.append(net);
            continue;
        }
        const LineParameters lp = lineParameters(*e, i, length, frequency);
        if (lp.ok()) {
            QJsonObject line;
            line.insert(QStringLiteral("C"), lp.c);
            line.insert(QStringLiteral("C0"), lp.c0);
            line.insert(QStringLiteral("epsilon_eff"), lp.epsilonEff);
            line.insert(QStringLiteral("Z0"), lp.z0);
            line.insert(QStringLiteral("L"), lp.l);
            line.insert(QStringLiteral("v"), lp.velocity);
            line.insert(QStringLiteral("n_eff"), lp.nEff);
            if (lp.frequency > 0) {
                line.insert(QStringLiteral("frequency"), lp.frequency);
                line.insert(QStringLiteral("R"), lp.r);
                if (lp.rDc > 0) line.insert(QStringLiteral("R_dc"), lp.rDc);
                if (!e->perMetre.wheeler.isEmpty()) line.insert(QStringLiteral("R_ground"), lp.rGround);
                line.insert(QStringLiteral("G"), lp.g);
                line.insert(QStringLiteral("skin_depth"), lp.skinDepth);
                line.insert(QStringLiteral("surface_resistance"), lp.surfaceResistance);
                if (lp.roughnessFactor > 1) line.insert(QStringLiteral("roughness_factor"), lp.roughnessFactor);
                line.insert(QStringLiteral("Zc"), lp.zc.real());
                line.insert(QStringLiteral("Zc_im"), lp.zc.imag());
                line.insert(QStringLiteral("alpha"), lp.alphaDb);
                line.insert(QStringLiteral("beta"), lp.gamma.imag());
                // And what that line is as a two-port of this length,
                // which is what a circuit wants of it.
                const fem::LineRLGC rlgc = asLine(*e, i, length, frequency);
                if (rlgc.error.isEmpty() && std::abs(rlgc.Zc) > 0) {
                    const std::array<std::complex<double>, 4> sp = fem::lineSParameters(rlgc, length, zref);
                    QJsonObject s;
                    s.insert(QStringLiteral("reference impedance"), zref);
                    static const char* const names[] = {"S11", "S21", "S12", "S22"};
                    for (int k = 0; k < 4; ++k) {
                        s.insert(QLatin1String(names[k]), sp[std::size_t(k)].real());
                        s.insert(QLatin1String(names[k]) + QStringLiteral("_im"), sp[std::size_t(k)].imag());
                        s.insert(QLatin1String(names[k]) + QStringLiteral("_dB"),
                                 20 * std::log10(std::max(1e-300, std::abs(sp[std::size_t(k)]))));
                    }
                    line.insert(QStringLiteral("s parameters"), s);
                }
            }
            net.insert(QStringLiteral("line"), line);
        } else if (!lp.problems.isEmpty()) {
            net.insert(QStringLiteral("line not given"), lp.problems.join(QStringLiteral("; ")));
        }
        nets.append(net);
    }
    out.insert(QStringLiteral("nets"), nets);
    // (What makes it doubtful, as the full wave's warnings: the mesh's.)
    if (const std::shared_ptr<const Mesh> mesh = model->mesh(); mesh && !mesh->warnings.isEmpty())
        out.insert(QStringLiteral("warnings"), QJsonArray::fromStringList(mesh->warnings));
    // Several conductors are coupled lines, whose modes are what a
    // designer of a pair actually asks after.
    double slowest = 0;   // m/s: the slowest mode's, for where the S parameters first turn over
    const int signalCount = e->hasReturns() ? int(e->signalNets().size()) : int(e->names.size());
    if (signalCount > 1 && length > 0) {
        const fem::CoupledLines cl = asCoupledLines(*e, length, frequency);
        if (cl.error.isEmpty() && !cl.modes.empty()) {
            QJsonObject coupled;
            // The signals, against the returns where there are any (a
            // coplanar line's grounds at nought: the GSSG check of 1
            // November, G1), and their matrices per metre (G3).
            QJsonArray conductors, returning;
            for (const QString& name : cl.names) conductors.append(name);
            for (int i = 0; i < e->names.size(); ++i)
                if (e->returns.value(i, false)) returning.append(e->names.at(i));
            coupled.insert(QStringLiteral("conductors"), conductors);
            if (!returning.isEmpty()) coupled.insert(QStringLiteral("returning through"), returning);
            coupled.insert(QStringLiteral("symmetric pair"), cl.symmetricPair);
            const auto matrix = [](const fem::CoupledLines::Matrix& m) {
                QJsonArray rows;
                for (const std::vector<double>& r : m) {
                    QJsonArray row;
                    for (const double v : r) row.append(v);
                    rows.append(row);
                }
                return rows;
            };
            QJsonObject per;
            per.insert(QStringLiteral("C"), matrix(cl.C));
            per.insert(QStringLiteral("C0"), matrix(cl.C0));
            per.insert(QStringLiteral("L"), matrix(cl.L));
            if (cl.rlgc) {
                per.insert(QStringLiteral("R"), matrix(cl.R));
                per.insert(QStringLiteral("G"), matrix(cl.G));
                per.insert(QStringLiteral("frequency"), cl.frequency);
            }
            per.insert(QStringLiteral("units"), QStringLiteral("F/m, H/m, Ohm/m, S/m; Maxwell form, of the conductors in order"));
            coupled.insert(QStringLiteral("per metre"), per);
            QJsonArray modes;
            for (const fem::LineMode& m : cl.modes) {
                QJsonObject one;
                one.insert(QStringLiteral("name"), m.name);
                one.insert(QStringLiteral("n_eff"), m.nEff);
                one.insert(QStringLiteral("Z"), m.Z.real());
                one.insert(QStringLiteral("Z_im"), m.Z.imag());
                one.insert(QStringLiteral("alpha"), m.alphaDb);
                one.insert(QStringLiteral("beta"), m.gamma.imag());
                one.insert(QStringLiteral("epsilon_eff"), m.nEff * m.nEff);
                modes.append(one);
                if (m.nEff > 0) slowest = slowest > 0 ? std::min(slowest, kLightSpeed / m.nEff) : kLightSpeed / m.nEff;
            }
            coupled.insert(QStringLiteral("modes"), modes);
            // The 2N-port, of a length asked for: the near ends first, then
            // the far ends. Of the solved length unasked, it came first in
            // the answer and was what max_chars cut around (G7).
            const std::vector<std::vector<std::complex<double>>> sm =
                args.contains(QLatin1String("length")) ? fem::coupledSParameters(cl, length, zref)
                                                       : std::vector<std::vector<std::complex<double>>>();
            QJsonArray rows;
            for (const std::vector<std::complex<double>>& row : sm) {
                QJsonArray re, im;
                for (const std::complex<double>& v : row) {
                    re.append(v.real());
                    im.append(v.imag());
                }
                QJsonObject pair;
                pair.insert(QStringLiteral("re"), re);
                pair.insert(QStringLiteral("im"), im);
                rows.append(pair);
            }
            if (!sm.empty()) {
                coupled.insert(QStringLiteral("s matrix"), rows);
                coupled.insert(QStringLiteral("ports"),
                               tr("1 to %1 are the near ends of %2, in that order; %3 to %4 their far ends")
                                   .arg(cl.names.size())
                                   .arg(cl.names.join(QStringLiteral(", ")))
                                   .arg(cl.names.size() + 1)
                                   .arg(2 * cl.names.size()));
            } else {
                coupled.insert(QStringLiteral("s matrix"),
                               tr("of a length asked for: give rf25_solve a 'length' (rf25_export writes it too)"));
            }
            out.insert(QStringLiteral("coupled lines"), coupled);
        } else if (!cl.error.isEmpty()) {
            out.insert(QStringLiteral("coupled lines not given"), cl.error);
        }
    }
    // Where a quasi-static answer stops being the whole story: the one
    // thing a caller must not be left to guess at.
    {
        if (e->names.size() == 1) slowest = lineParameters(*e, 0, length).velocity;
        const std::shared_ptr<const Mesh> mesh = model->mesh();
        const bool line = e->perMetre.fromMiddle;
        const QuasiStaticLimit limit = quasiStaticLimit(model->model().stack(), mesh ? *mesh : Mesh(), 1e-6, slowest, length,
                                                        int(e->names.size()), line);
        QJsonObject said;
        said.insert(QStringLiteral("frequency"), limit.frequency);
        said.insert(QStringLiteral("what sets it"), limit.what);
        said.insert(QStringLiteral("epsilon_max"), limit.epsilonMax);
        // (A line's own: what is no line has none, and it said 0 - the
        //  recheck of 1 November.)
        if (limit.resonance > 0) said.insert(QStringLiteral("first half-wave resonance"), limit.resonance);
        said.insert(QStringLiteral("why"),
                    line ? tr("A per-metre RLGC is a property of the cross-section and holds while the cross-section is "
                              "electrically small; this is a tenth of a wavelength of the dimension named, which is the "
                              "usual engineering rule. The resonance is not a limit of the model - a line's own S "
                              "parameters carry it - only where they first turn over.")
                         : tr("It is no line, so it has no per-metre values: its capacitance holds while all of it is "
                              "electrically small, and this is a tenth of a wavelength of its longest extent, the usual "
                              "engineering rule. Above it, and for its ports' S at all, solve it full-wave."));
        if (frequency > limit.frequency && limit.frequency > 0)
            said.insert(QStringLiteral("warning"),
                        tr("The frequency asked for (%1 GHz) is above it: these numbers are extrapolated, not solved.")
                            .arg(frequency / 1e9, 0, 'g', 3));
        out.insert(QStringLiteral("quasi-static limit"), said);
    }
    out.insert(QStringLiteral("unknowns"), e->unknowns);
    out.insert(QStringLiteral("length"), length);
    QJsonArray said;
    for (const QString& line : model->preparedLines()) said.append(line);
    for (const QString& line : e->lines) said.append(line);
    out.insert(QStringLiteral("what it did"), said);
    out.insert(QStringLiteral("note"),
               tr("Quasi-static throughout: C and, with every dielectric vacuum, C0, and a uniform line's eps_eff, Z0 "
                  "and L from the two; with a frequency, R from the metal's surface impedance and how the charge "
                  "crowds, G from the dielectrics' loss tangents, and the Zc, alpha, beta and S parameters those "
                  "make. The S parameters are of a uniform line of this length and are right wherever the "
                  "cross-section is electrically small - see the quasi-static limit. A loop's inductance is "
                  "rf25_inductance's, by another method; the full-wave solve is 'solver' full_wave, or the study's "
                  "Solver; rf25_export writes a Touchstone of these."));
    // Brief: the numbers a line is asked after and nothing else - an
    // answer of 2.5 kB a net had its line cut first by max_chars (the
    // phase B check of 24 October).
    if (args.value(QLatin1String("brief")).toBool()) {
        QJsonObject brief;
        brief.insert(QStringLiteral("model"), out.value(QStringLiteral("model")));
        brief.insert(QStringLiteral("length"), length);
        brief.insert(QStringLiteral("unknowns"), e->unknowns);
        if (out.contains(QStringLiteral("warnings"))) brief.insert(QStringLiteral("warnings"), out.value(QStringLiteral("warnings")));
        QJsonArray few;
        for (const QJsonValue& v : out.value(QStringLiteral("nets")).toArray()) {
            const QJsonObject net = v.toObject();
            QJsonObject one;
            one.insert(QStringLiteral("name"), net.value(QStringLiteral("name")));
            one.insert(QStringLiteral("capacitance"), net.value(QStringLiteral("capacitance")));
            const QJsonObject line = net.value(QStringLiteral("line")).toObject();
            if (!line.isEmpty()) {
                QJsonObject kept;
                for (const char* key : {"C", "C0", "epsilon_eff", "Z0", "L", "R", "G", "alpha"})
                    if (line.contains(QLatin1String(key))) kept.insert(QLatin1String(key), line.value(QLatin1String(key)));
                one.insert(QStringLiteral("line"), kept);
            } else if (net.contains(QStringLiteral("line not given"))) {
                one.insert(QStringLiteral("line not given"), net.value(QStringLiteral("line not given")));
            }
            few.append(one);
        }
        brief.insert(QStringLiteral("nets"), few);
        const QJsonObject coupled = out.value(QStringLiteral("coupled lines")).toObject();
        if (!coupled.isEmpty()) {
            // The signals, what they return through and their matrices per
            // metre, as well as the modes (G7).
            QJsonObject lines;
            for (const char* key : {"conductors", "returning through", "symmetric pair", "per metre"})
                if (coupled.contains(QLatin1String(key))) lines.insert(QLatin1String(key), coupled.value(QLatin1String(key)));
            brief.insert(QStringLiteral("coupled lines"), lines);
            QJsonArray modes;
            for (const QJsonValue& v : coupled.value(QStringLiteral("modes")).toArray()) {
                const QJsonObject mode = v.toObject();
                QJsonObject kept;
                for (const char* key : {"name", "n_eff", "Z", "epsilon_eff", "alpha"}) kept.insert(QLatin1String(key), mode.value(QLatin1String(key)));
                modes.append(kept);
            }
            brief.insert(QStringLiteral("modes"), modes);
        } else if (out.contains(QStringLiteral("coupled lines not given"))) {
            brief.insert(QStringLiteral("coupled lines not given"), out.value(QStringLiteral("coupled lines not given")));
        }
        brief.insert(QStringLiteral("quasi-static limit"),
                     out.value(QStringLiteral("quasi-static limit")).toObject().value(QStringLiteral("frequency")));
        brief.insert(QStringLiteral("per metre"), e->perMetre.said);
        return jsonResult(brief);
    }
    return jsonResult(out);
}


QJsonObject QucsControl::rf25Stop(const QJsonObject& args)
{
    // As the tab's Stop: the solve told to stop, and waited for - a
    // quasi-static one stops at its next row of a matrix or its next
    // Green's function's next product, a full-wave one at its next
    // frequency. The model as it is open: one not open is solving nothing.
    QString error;
    QucsDoc* doc = document(args, &error);
    if (doc == nullptr) return errorResult(error);
    auto* model = dynamic_cast<RF25Doc*>(doc);
    if (model == nullptr)
        return errorResult(tr("%1 is no RF 2.5D model (.qrf25): rf25_stop stops a planar structure's solve.").arg(titleOf(doc)));
    // (Held from here to its answer: its tab closed meanwhile - by the user, by
    //  another call -, it is not deleted under this call, which says so.)
    const RF25Doc::Held held(model);
    const QString heldName = QFileInfo(model->getDocName()).fileName();
    const QString name = QFileInfo(model->getDocName()).fileName();
    if (!model->isSolving() && model->solvesAtWork() == 0)
        return jsonResult(QJsonObject{{QStringLiteral("stopped"), false},
                                      {QStringLiteral("note"), tr("Nothing is solving in %1.").arg(name)}});
    const QString was = model->solvingSaid();
    model->stopSolving();
    const int timeout = waitsFor(QStringLiteral("rf25_stop"), args);
    const bool ended = model->waitForSolvesToEnd(timeout * 1000);
    if (model->closedWhenFree()) return closedWhileWaited(heldName);
    if (!ended)
        return errorResult(tr("Asked to stop, still at work after %1 s: it stops at its next check (rf25_stop again waits "
                              "more).").arg(timeout));
    QJsonObject out{{QStringLiteral("stopped"), true}};
    if (!was.isEmpty()) out.insert(QStringLiteral("note"), tr("Stopped %1, which was %2").arg(name, was));
    return jsonResult(out);
}

QJsonObject QucsControl::rf25Circuit(const QJsonObject& args)
{
    // The model, as rf25_export finds it: the one named, else in front.
    QucsDoc* doc = nullptr;
    const QString path = args.value(QLatin1String("path")).toString().trimmed();
    if (path.isEmpty()) {
        doc = a_app->getDoc();
    } else {
        const QJsonObject opened = openDocument(QJsonObject{{QStringLiteral("path"), path}});
        if (opened.value(QStringLiteral("isError")).toBool()) return opened;
        doc = a_app->getDoc();
    }
    auto* model = dynamic_cast<RF25Doc*>(doc);
    if (model == nullptr)
        return errorResult(tr("%1 is no RF 2.5D model (.qrf25): rf25_circuit fits a circuit to the full-wave solve of one.")
                               .arg(doc != nullptr ? QFileInfo(doc->getDocName()).fileName() : tr("nothing is open")));
    // (Held from here to its answer: its tab closed meanwhile - by the user, by
    //  another call -, it is not deleted under this call, which says so.)
    const RF25Doc::Held held(model);
    const QString heldName = QFileInfo(model->getDocName()).fileName();
    // Its shapes and its mesh first: a solution kept beside the model is
    // taken once they are made, a moment after the model is opened.
    const bool laidOut = model->waitForLayout(300000);
    if (model->closedWhenFree()) return closedWhileWaited(heldName);
    if (!laidOut) return errorResult(tr("Its layout is still being read after %1 s.").arg(300));
    const bool prepared = model->waitForPrepared(300000);
    if (model->closedWhenFree()) return closedWhileWaited(heldName);
    if (!prepared)
        return errorResult(tr("Its shapes and its mesh are still being made after %1 s.").arg(300));
    // Where it goes, if anywhere: the file given, else an Equivalent circuit
    // node's own; with neither, the fit alone.
    const QString node = args.value(QLatin1String("node")).toString().trimmed();
    QString name = args.value(QLatin1String("name")).toString().trimmed();
    QString where = args.value(QLatin1String("file")).toString().trimmed();
    if (!node.isEmpty()) {
        const Node* n = model->model().root.find(node);
        if (n == nullptr || n->type != QLatin1String("rf_circuit"))
            return errorResult(tr("%1 is no Equivalent circuit node of %2 (Exports > Equivalent circuit).")
                                   .arg(node, QFileInfo(model->getDocName()).fileName()));
        if (where.isEmpty()) where = model->exportPath(node);
        if (name.isEmpty()) name = n->props.value(QStringLiteral("name")).toString().trimmed();
    }
    if (!where.isEmpty() && QFileInfo(where).isRelative()) {
        const QString file = model->getDocName();
        const QDir dir = file.isEmpty() ? QDir(QucsSettings.QucsWorkDir) : QFileInfo(file).absoluteDir();
        where = QDir::cleanPath(dir.absoluteFilePath(where));
    }
    QString why;
    const std::optional<EquivalentCircuit> c = model->equivalentCircuit(&why);
    if (!c) return errorResult(model->fullWave() ? why : tr("Nothing is solved full-wave yet: rf25_solve it first (solver full_wave)."));

    QJsonObject out;
    out.insert(QStringLiteral("model"), QFileInfo(model->getDocName()).fileName());
    out.insert(QStringLiteral("ports"), QJsonArray::fromStringList(c->portNames));
    out.insert(QStringLiteral("circuit"), c->ports == 2 ? QStringLiteral("a pi: a branch between the ports, one from each to the ground")
                                                        : QStringLiteral("one branch to the ground"));
    // Each branch: between what, its form, its values (SI, and as a part
    // writes them) and how well it fits its own share.
    QJsonArray branches;
    const auto branchOf = [](const CircuitBranch& b, const QString& from, const QString& to) {
        QJsonObject o{{QStringLiteral("between"), QJsonArray{from, to}}, {QStringLiteral("form"), b.form}};
        QJsonObject values;
        QStringList said;
        for (const auto& [element, value] : b.values) {
            values.insert(element, value);
            said << elementSaid(element, value);
        }
        if (!b.values.isEmpty()) {
            o.insert(QStringLiteral("values"), values);
            o.insert(QStringLiteral("as written"), said.join(QStringLiteral(", ")));
            o.insert(QStringLiteral("its own misfit"), b.error);
        }
        return o;
    };
    if (c->ports == 2) branches.append(branchOf(c->between, c->portNames.at(0), c->portNames.at(1)));
    for (int i = 0; i < c->ports; ++i) branches.append(branchOf(c->toGround[i], c->portNames.at(i), QStringLiteral("ground")));
    out.insert(QStringLiteral("branches"), branches);
    out.insert(QStringLiteral("S differs by at most"), c->worst);
    out.insert(QStringLiteral("most at"), c->worstAt);
    out.insert(QStringLiteral("S differs by rms"), c->rms);
    out.insert(QStringLiteral("fitted from"), *std::min_element(c->frequencies.begin(), c->frequencies.end()));
    out.insert(QStringLiteral("fitted to"), *std::max_element(c->frequencies.begin(), c->frequencies.end()));
    out.insert(QStringLiteral("frequencies"), int(c->frequencies.size()));
    out.insert(QStringLiteral("what it did"), QJsonArray::fromStringList(c->lines));
    if (!where.isEmpty()) {
        const QString text = model->circuitText(*c, where, name);
        QFile file(where);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
            return errorResult(tr("%1 could not be written: %2.").arg(where, file.errorString()));
        file.write(text.toUtf8());
        file.close();
        out.insert(QStringLiteral("file"), where);
        out.insert(QStringLiteral("written as"), QFileInfo(where).suffix().compare(QLatin1String("sch"), Qt::CaseInsensitive) == 0
                                                     ? QStringLiteral("a Qucs-S part: a schematic's Subcircuit, its File this one, places it")
                                                     : QStringLiteral("a SPICE subcircuit: its ports, then the reference (ref)"));
    }
    out.insert(QStringLiteral("note"),
               QCoreApplication::translate("QucsControl", "A lumped circuit holds while what it stands for is short beside the "
                                                          "wavelength: where S differs by more than a few hundredths, solve the "
                                                          "model over a lower band or use its Touchstone (rf25_export)."));
    return jsonResult(out);
}

QJsonObject QucsControl::rf25Inductance(const QJsonObject& args)
{
    QucsDoc* doc = nullptr;
    const QString path = args.value(QLatin1String("path")).toString().trimmed();
    if (path.isEmpty()) {
        doc = a_app->getDoc();
    } else {
        const QJsonObject opened = openDocument(QJsonObject{{QStringLiteral("path"), path}});
        if (opened.value(QStringLiteral("isError")).toBool()) return opened;
        doc = a_app->getDoc();
    }
    auto* model = dynamic_cast<RF25Doc*>(doc);
    if (model == nullptr)
        return errorResult(tr("%1 is no RF 2.5D model (.qrf25): rf25_inductance is of the loop a planar structure's "
                              "ports make.")
                               .arg(doc != nullptr ? QFileInfo(doc->getDocName()).fileName() : tr("nothing is open")));
    // (Held from here to its answer: its tab closed meanwhile - by the user, by
    //  another call -, it is not deleted under this call, which says so.)
    const RF25Doc::Held held(model);
    const QString heldName = QFileInfo(model->getDocName()).fileName();

    const int timeout = waitsFor(QStringLiteral("rf25_inductance"), args);
    // A frequency of its own is only for Q, and L and R do not move with
    // it here, so it is taken afterwards rather than by editing the
    // model's own study - a tool that is asked a question should not
    // leave a step of Undo behind it.
    double asked = 0;
    if (args.contains(QLatin1String("frequency"))) {
        const std::optional<double> given = positiveNumber(args.value(QLatin1String("frequency")).toString().trimmed(), *model);
        if (!given) return errorResult(tr("'frequency' is a frequency above zero (\"10[GHz]\")."));
        asked = *given;
    }

    // The shapes, the cells, and then the solve - which finds the charge
    // and the loop together, on the same copies: the quasi-static one,
    // whatever the study's Solver, since that is where the loop is found.
    const bool laidOut = model->waitForLayout(timeout * 1000);
    if (model->closedWhenFree()) return closedWhileWaited(heldName);
    if (!laidOut)
        return errorResult(tr("Its layout is still being read after %1 s.").arg(timeout));
    const bool prepared = model->waitForPrepared(timeout * 1000);
    if (model->closedWhenFree()) return closedWhileWaited(heldName);
    if (!prepared)
        return errorResult(tr("Its shapes and its mesh are still being made after %1 s.").arg(timeout));
    RF25Doc::SolveRequest quasiStatic;
    quasiStatic.solver = QStringLiteral("quasi_static");
    // As rf25_solve: a quasi-static solve going on is joined, one of
    // another kind refused, and past the timeout the solve goes on.
    if (model->isSolving() && !model->solvingAlike(quasiStatic))
        return errorResult(tr("%1 is solving already, full-wave (%2): rf25_inductance's loop is of a quasi-static solve "
                              "- ask again once that one has ended, or after rf25_stop.")
                               .arg(QFileInfo(model->getDocName()).fileName(), model->solvingSaid()));
    if (!model->isSolving()) model->solveWith(quasiStatic);
    const bool solved = model->waitForSolved(timeout * 1000);
    if (model->closedWhenFree()) return closedWhileWaited(heldName);
    if (!solved) {
        const bool greens = model->quasiStaticStep() == ExtractControl::Greens;
        return errorResult(tr("It is still solving after %1 s: %2 It goes on - rf25_inductance asked again waits for it, "
                              "and rf25_stop stops it.%3")
                               .arg(timeout)
                               .arg(model->solvingSaid(),
                                    greens ? tr(" The time is in the stack's Green's functions, which the mesh does not "
                                                "change.")
                                           : tr(" A coarser mesh (the Mesh node's largest cell) solves sooner.")));
    }
    const std::shared_ptr<const Inductance> ind = model->inductance();
    if (!ind) {
        const QStringList said = model->inductanceSummary();
        return errorResult(said.isEmpty() ? tr("There is no loop to find an inductance of: the model wants two ports, "
                                               "one for the current to go in at and one for it to come out of.")
                                          : said.join(QStringLiteral(" ")));
    }
    if (!ind->ok())
        return errorResult(ind->problems.isEmpty() ? tr("The loop's inductance came to nothing, and it said no reason.")
                                                   : ind->problems.join(QStringLiteral(" ")));

    QJsonObject out;
    out.insert(QStringLiteral("model"), QFileInfo(model->getDocName()).fileName());
    out.insert(QStringLiteral("L"), ind->l);
    out.insert(QStringLiteral("L_dc"), ind->lDc);
    out.insert(QStringLiteral("R_dc"), ind->rDc);
    // Q at the frequency asked for, else at the study's own: wL/R, which
    // needs no second solve since neither L nor R moves with it.
    const double at = asked > 0 ? asked : ind->frequency;
    if (at > 0 && ind->rDc > 0) {
        out.insert(QStringLiteral("Q"), 2 * M_PI * at * ind->l / ind->rDc);
        out.insert(QStringLiteral("frequency"), at);
    }
    out.insert(QStringLiteral("runs of current"), ind->branches);
    out.insert(QStringLiteral("cells"), ind->nodes);
    QJsonArray said;
    for (const QString& line : model->preparedLines()) said.append(line);
    for (const QString& line : ind->lines) said.append(line);
    out.insert(QStringLiteral("what it did"), said);
    out.insert(QStringLiteral("note"),
               tr("Ruehli's partial inductances over the runs of current the mesh's shared edges give, with the "
                  "current solved for rather than assumed. L is of the current of least magnetic energy, which is "
                  "where a current goes once its inductance outweighs its resistance; L_dc and R_dc are of the current "
                  "at direct current, which the resistance divides instead - R_dc the loop's own resistance, not a "
                  "line's R per metre. Between the two regimes the current is neither, and is not solved. rf25_solve's "
                  "L is a different quantity: 1/(c0^2 C0), of a uniform cross-section."));
    return jsonResult(out);
}

QJsonObject QucsControl::rf25Export(const QJsonObject& args)
{
    QucsDoc* doc = nullptr;
    const QString path = args.value(QLatin1String("path")).toString().trimmed();
    if (path.isEmpty()) {
        doc = a_app->getDoc();
    } else {
        const QJsonObject opened = openDocument(QJsonObject{{QStringLiteral("path"), path}});
        if (opened.value(QStringLiteral("isError")).toBool()) return opened;
        doc = a_app->getDoc();
    }
    auto* model = dynamic_cast<RF25Doc*>(doc);
    if (model == nullptr)
        return errorResult(tr("%1 is no RF 2.5D model (.qrf25): rf25_export writes the S parameters of one.")
                               .arg(doc != nullptr ? QFileInfo(doc->getDocName()).fileName() : tr("nothing is open")));
    // (Held from here to its answer: its tab closed meanwhile - by the user, by
    //  another call -, it is not deleted under this call, which says so.)
    const RF25Doc::Held held(model);
    const QString heldName = QFileInfo(model->getDocName()).fileName();
    // Its shapes and its mesh first: a solution kept beside the model is
    // taken once they are made, a moment after the model is opened.
    const bool laidOut = model->waitForLayout(300000);
    if (model->closedWhenFree()) return closedWhileWaited(heldName);
    if (!laidOut) return errorResult(tr("Its layout is still being read after %1 s.").arg(300));
    const bool prepared = model->waitForPrepared(300000);
    if (model->closedWhenFree()) return closedWhileWaited(heldName);
    if (!prepared)
        return errorResult(tr("Its shapes and its mesh are still being made after %1 s.").arg(300));
    // (An equivalent circuit is fitted, not written as S: its own tool.)
    const QString node = args.value(QLatin1String("node")).toString().trimmed();
    if (const Node* n = model->model().root.find(node); !node.isEmpty() && n != nullptr && n->type == QLatin1String("rf_circuit"))
        return errorResult(tr("%1 is an Equivalent circuit export: rf25_circuit fits and writes it (its 'node').").arg(node));
    const std::shared_ptr<const FullWave> wave = model->fullWave();
    const bool full = wave && wave->ok();
    if (!full && (!model->extraction() || !model->extraction()->ok()))
        return errorResult(model->solveProblem().isEmpty()
                               ? tr("Nothing is solved yet: rf25_solve it first, or press Solve on its tab.")
                               : model->solveProblem());

    // What to write: the export node named, else what the arguments say.
    RF25Doc::ExportRequest what;
    if (!node.isEmpty()) {
        QString why;
        if (!model->exportRequest(node, &what, &why)) return errorResult(why);
    }
    if (args.contains(QLatin1String("of"))) {
        const QString of = args.value(QLatin1String("of")).toString().trimmed();
        if (of != QLatin1String("line") && of != QLatin1String("coupled"))
            return errorResult(tr("'of' is \"line\" (one net as a two-port) or \"coupled\" (every net as a 2N-port); "
                                  "%1 is neither.").arg(of));
        what.coupled = of == QLatin1String("coupled");
    }
    if (args.contains(QLatin1String("net"))) what.net = args.value(QLatin1String("net")).toString().trimmed();
    if (args.contains(QLatin1String("length"))) {
        const std::optional<double> given = positiveNumber(args.value(QLatin1String("length")).toString().trimmed(), *model);
        if (!given) return errorResult(tr("'length' is a length above zero (\"800[um]\")."));
        what.length = *given;
    }
    if (args.contains(QLatin1String("version"))) {
        const QString version = args.value(QLatin1String("version")).toString().trimmed();
        if (version != QLatin1String("1.1") && version != QLatin1String("2.0"))
            return errorResult(tr("'version' is \"1.1\" (every port into one reference, what a schematic's S-parameter "
                                  "file reads) or \"2.0\" (each full-wave port into its own), not \"%1\".").arg(version));
        what.version2 = version == QLatin1String("2.0");
    }
    if (args.contains(QLatin1String("impedance"))) {
        const std::optional<double> given = positiveNumber(args.value(QLatin1String("impedance")).toString().trimmed(), *model);
        if (!given) return errorResult(tr("'impedance' is a reference impedance above zero (\"50\")."));
        what.impedance = *given;
    }
    if (args.contains(QLatin1String("modes"))) {
        const QString modes = args.value(QLatin1String("modes")).toString().trimmed();
        if (modes != QLatin1String("single") && modes != QLatin1String("mixed"))
            return errorResult(tr("'modes' is \"single\" (each port as it is) or \"mixed\" (each differential pair's "
                                  "differential and common modes), not \"%1\".").arg(modes));
        what.mixed = modes == QLatin1String("mixed");
    }
    // A pair's modes are Touchstone 2.0's alone.
    if (what.mixed) {
        if (args.value(QLatin1String("version")).toString().trimmed() == QLatin1String("1.1"))
            return errorResult(tr("Mixed modes are written in Touchstone 2.0, whose [Mixed-Mode Order] says what each is; "
                                  "1.1 has no way to."));
        what.version2 = true;
    }

    QString why;
    const QString text = model->exportText(what, &why);
    if (!why.isEmpty()) return errorResult(why);

    // Where it goes: the file given, else the node's own, else beside the
    // model. The same path the tab's Write button uses.
    QString where = args.value(QLatin1String("file")).toString().trimmed();
    if (where.isEmpty() && !node.isEmpty()) where = model->exportPath(node);
    if (where.isEmpty()) {
        const QString name = model->getDocName();
        const QDir dir = name.isEmpty() ? QDir(QucsSettings.QucsWorkDir) : QFileInfo(name).absoluteDir();
        const int ports = full ? int(wave->portNames.size()) : what.coupled ? 2 * int(model->extraction()->signalNets().size()) : 2;
        const QString stem = name.isEmpty() ? QStringLiteral("model") : QFileInfo(name).completeBaseName();
        where = QDir::cleanPath(dir.absoluteFilePath(QStringLiteral("%1.s%2p").arg(stem).arg(ports)));
    } else if (QFileInfo(where).isRelative()) {
        const QString name = model->getDocName();
        const QDir dir = name.isEmpty() ? QDir(QucsSettings.QucsWorkDir) : QFileInfo(name).absoluteDir();
        where = QDir::cleanPath(dir.absoluteFilePath(where));
    }
    QFile file(where);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return errorResult(tr("%1 could not be written: %2.").arg(where, file.errorString()));
    file.write(text.toUtf8());
    file.close();

    QJsonObject out;
    out.insert(QStringLiteral("model"), QFileInfo(model->getDocName()).fileName());
    out.insert(QStringLiteral("file"), where);
    const int ports = full ? int(wave->portNames.size()) : what.coupled ? 2 * int(model->extraction()->signalNets().size()) : 2;
    out.insert(QStringLiteral("ports"), ports);
    out.insert(QStringLiteral("version"), what.version2 ? QStringLiteral("2.0") : QStringLiteral("1.1"));
    if (what.mixed) out.insert(QStringLiteral("modes"), QStringLiteral("mixed"));
    // A full-wave solve's 2.0 file puts each port into its own reference;
    // every other file every port into the one asked.
    const bool own = full && what.version2;
    QStringList owns;
    if (own) {
        QJsonArray references;
        for (const double r : wave->references) {
            references.append(r);
            owns << QString::number(r, 'g', 4);
        }
        out.insert(QStringLiteral("reference impedances"), references);
    } else {
        out.insert(QStringLiteral("reference impedance"), what.impedance);
    }
    if (!full) out.insert(QStringLiteral("length"), what.length > 0 ? what.length : model->solvedLength());
    out.insert(QStringLiteral("bytes"), text.toUtf8().size());
    QString sweepProblem;
    const QList<double> sweep = full ? wave->frequencies : studySweep(model->model(), &sweepProblem);
    out.insert(QStringLiteral("frequencies"), int(sweep.size()));
    if (!sweep.isEmpty()) {
        out.insert(QStringLiteral("from"), sweep.first());
        out.insert(QStringLiteral("to"), sweep.last());
    }
    QStringList modes, of;
    if (full && what.mixed) mixedModeOf(*wave, 0, &modes, &of);
    for (int m = 0; m < modes.size(); ++m) modes[m] = QStringLiteral("%1 (%2)").arg(modes.at(m), of.at(m));
    out.insert(QStringLiteral("what it wrote"),
               !modes.isEmpty()
                   ? tr("The full-wave solve's ports as their modes, in the order its [Mixed-Mode Order] says: %1 - each "
                        "differential mode into twice its ports' reference, each common mode into half, a port of no pair "
                        "into its own.")
                         .arg(modes.join(QStringLiteral(", ")))
               : full ? tr("The ports of the full-wave solve: a %1-port, %2 in that order, %3. ('of', 'net' and 'length' "
                         "are a quasi-static solve's.)")
                          .arg(ports)
                          .arg(wave->portNames.join(QStringLiteral(", ")),
                               own ? tr("each into its own reference (%1 Ohm)").arg(owns.join(QStringLiteral(", ")))
                                   : tr("each into %1 Ohm").arg(what.impedance, 0, 'g', 4))
               : what.coupled ? tr("Every net as coupled lines: a %1-port, the near ends first, then the far ends.").arg(ports)
                              : tr("Net %1 as a two-port, port 1 at its start.")
                                    .arg(what.net.isEmpty() ? model->extraction()->names.value(model->extraction()->signalNets().value(0, 0))
                                                            : what.net));
    // Which Touchstone it is, and whether a schematic's part reads it.
    const QString version =
        what.version2 ? tr("Touchstone 2.0, real and imaginary, its [Reference] giving %1. A schematic's S-parameter part "
                           "does not read it: it hands its file to s2spice, which reads Touchstone 1 only - 'version' 1.1 "
                           "writes one it does.")
                            .arg(own ? tr("each port its own") : tr("every port 'impedance'"))
                      : tr("Touchstone 1.1, real and imaginary, %1. A schematic's S-parameter part reads it at once.")
                            .arg(full         ? tr("the frequencies of the solve")
                                 : ports <= 2 ? tr("one line a frequency")
                                              : tr("each frequency's matrix by rows, four to a line"));
    out.insert(QStringLiteral("note"),
               version + QLatin1Char(' ')
                   + (full ? tr("The file's own comments carry what each port drives and where its reference plane is.")
                           : tr("The file's own comments carry what the solve was and where the quasi-static answer "
                                "stops being the whole story.")));
    return jsonResult(out);
}

// ------------------------------------------------------- HFSS 3D Layout

QJsonObject QucsControl::rf25HfssExport(const QJsonObject& args)
{
    QucsDoc* doc = nullptr;
    const QString path = args.value(QLatin1String("path")).toString().trimmed();
    if (path.isEmpty()) {
        doc = a_app->getDoc();
    } else {
        const QJsonObject opened = openDocument(QJsonObject{{QStringLiteral("path"), path}});
        if (opened.value(QStringLiteral("isError")).toBool()) return opened;
        doc = a_app->getDoc();
    }
    auto* model = dynamic_cast<RF25Doc*>(doc);
    if (model == nullptr)
        return errorResult(tr("%1 is no RF 2.5D model (.qrf25): rf25_hfss_export writes one for HFSS™ 3D Layout.")
                               .arg(doc != nullptr ? QFileInfo(doc->getDocName()).fileName() : tr("nothing is open")));
    // (Held from here to its answer: its tab closed meanwhile - by the user, by
    //  another call -, it is not deleted under this call, which says so.)
    const RF25Doc::Held held(model);
    const QString heldName = QFileInfo(model->getDocName()).fileName();
    const bool laidOut = model->waitForLayout(300000);
    if (model->closedWhenFree()) return closedWhileWaited(heldName);
    if (!laidOut) return errorResult(tr("Its layout is still being read after %1 s.").arg(300));
    const bool prepared = model->waitForPrepared(300000);
    if (model->closedWhenFree()) return closedWhileWaited(heldName);
    if (!prepared)
        return errorResult(tr("Its shapes and its mesh are still being made after %1 s.").arg(300));

    HfssOptions how;
    if (args.contains(QLatin1String("port_type"))) how.portType = args.value(QLatin1String("port_type")).toString().trimmed();
    // (Nought is a length here: no margin, no air.)
    const auto lengthIn = [model](const QJsonValue& value, double* into) {
        const QString text = value.toString().trimmed();
        if (text == QLatin1String("0")) {
            *into = 0;
            return true;
        }
        const std::optional<double> given = positiveNumber(text, *model);
        if (!given) return false;
        *into = *given;
        return true;
    };
    if (args.contains(QLatin1String("margin")) && !lengthIn(args.value(QLatin1String("margin")), &how.margin))
        return errorResult(tr("'margin' is a length of nought or more (\"2[mm]\")."));
    if (args.contains(QLatin1String("air")) && !lengthIn(args.value(QLatin1String("air")), &how.air))
        return errorResult(tr("'air' is a length of nought or more (\"5[mm]\")."));
    if (args.contains(QLatin1String("thin_metal")) && !lengthIn(args.value(QLatin1String("thin_metal")), &how.thinMetal))
        return errorResult(tr("'thin_metal' is a length of nought or more (\"2[um]\")."));
    if (args.contains(QLatin1String("adaptive_frequency"))) {
        const std::optional<double> given = positiveNumber(args.value(QLatin1String("adaptive_frequency")).toString().trimmed(), *model);
        if (!given) return errorResult(tr("'adaptive_frequency' is a frequency above nought (\"20[GHz]\")."));
        how.adaptive = *given;
    }
    if (args.contains(QLatin1String("max_delta"))) {
        how.maxDelta = args.value(QLatin1String("max_delta")).toDouble();
        if (!(how.maxDelta > 0)) return errorResult(tr("'max_delta' is a number above nought (0.02)."));
    }
    if (args.contains(QLatin1String("max_passes"))) {
        how.maxPasses = args.value(QLatin1String("max_passes")).toInt();
        if (how.maxPasses < 1 || how.maxPasses > 1000) return errorResult(tr("'max_passes' is a whole number from 1 to 1000."));
    }
    if (args.contains(QLatin1String("sweep"))) {
        const QString sweep = args.value(QLatin1String("sweep")).toString().trimmed();
        if (sweep != QLatin1String("interpolating") && sweep != QLatin1String("discrete"))
            return errorResult(tr("'sweep' is \"interpolating\" or \"discrete\", not \"%1\".").arg(sweep));
        how.discrete = sweep == QLatin1String("discrete");
    }
    const HfssExport written = model->hfssExport(how);
    if (!written.ok()) return errorResult(written.problems.join(QLatin1Char(' ')));

    // Where: the file given, relative to the model; else beside it.
    const QString name = model->getDocName();
    const QDir dir = name.isEmpty() ? QDir(QucsSettings.QucsWorkDir) : QFileInfo(name).absoluteDir();
    QString where = args.value(QLatin1String("file")).toString().trimmed();
    if (where.isEmpty()) where = (name.isEmpty() ? QStringLiteral("rf25") : QFileInfo(name).completeBaseName()) + QStringLiteral("_hfss.py");
    if (const QString bad = badFileName(QFileInfo(where).fileName()); !bad.isEmpty()) return errorResult(tr("'file': %1.").arg(bad));
    where = QDir::cleanPath(dir.absoluteFilePath(where));
    if (QFileInfo(where).suffix().compare(QLatin1String("py"), Qt::CaseInsensitive) != 0)
        return errorResult(tr("'file' is the script's, a .py: the .gds and the .xml go beside it by its name."));
    if (!QFileInfo(QFileInfo(where).absolutePath()).isDir()) return errorResult(tr("There is no folder %1.").arg(QFileInfo(where).absolutePath()));
    const QString stem = QFileInfo(where).absoluteDir().filePath(QFileInfo(where).completeBaseName());
    // The .gds and the .xml take the script's name, so never the model's own
    // layout; and with no script there, what has those names is no earlier export.
    const QString layoutName = qucs_s::rf25::textOf(model->model().layout(), QStringLiteral("file")).trimmed();
    if (!layoutName.isEmpty() && sameFile(dir.absoluteFilePath(layoutName), stem + QStringLiteral(".gds")))
        return errorResult(tr("%1 is this model's layout, and the GDSII file goes beside the script by its name: name the script otherwise.").arg(layoutName));
    if (!QFileInfo::exists(where))
        for (const QString& file : {stem + QStringLiteral(".xml"), stem + QStringLiteral(".gds")})
            if (QFileInfo::exists(file))
                return errorResult(tr("%1 is there already, and the export would write over it by the script's name: name the script otherwise.").arg(QFileInfo(file).fileName()));
    for (const QString& file : {where, stem + QStringLiteral(".xml"), stem + QStringLiteral(".gds")})
        if (const QString no = aboutToWrite(file); !no.isEmpty()) return errorResult(no);
    QString why;
    QStringList files;
    if (!writeHfssExport(written, where, &files, &why)) return errorResult(why);

    QJsonArray ports;
    for (const QString& p : written.portSaid) ports.append(p);
    QJsonArray notes;
    for (const QString& n : written.notes) notes.append(n);
    return jsonResult(QJsonObject{{QStringLiteral("files"), QJsonArray::fromStringList(files)},
                                  {QStringLiteral("design"), written.name},
                                  {QStringLiteral("layers"), written.layers},
                                  {QStringLiteral("materials"), written.materials},
                                  {QStringLiteral("polygons"), written.polygons},
                                  {QStringLiteral("voids"), written.voids},
                                  {QStringLiteral("vias"), written.vias},
                                  {QStringLiteral("planes"), written.planes},
                                  {QStringLiteral("ports"), ports},
                                  {QStringLiteral("setup"), written.setup},
                                  {QStringLiteral("sweep"), written.sweep},
                                  {QStringLiteral("sweep_data"), written.sweepData},
                                  {QStringLiteral("adaptive_frequency_hz"), written.adaptive},
                                  {QStringLiteral("margin_m"), written.marginUsed},
                                  {QStringLiteral("air_m"), written.airUsed},
                                  {QStringLiteral("notes"), notes},
                                  {QStringLiteral("run"), tr("Not run: no Electronics Desktop here. Tools > Run Script, or ansysedt -RunScriptAndExit %1 -ScriptArgs solve; it leaves a report beside it.")
                                                              .arg(QFileInfo(where).fileName())}});
}

namespace {

QString stackFormatName(StackFormat format)
{
    return format == StackFormat::Hfss ? QStringLiteral("hfss") : QStringLiteral("emx");
}

} // namespace

QJsonObject QucsControl::rf25StackExport(const QJsonObject& args)
{
    const QString path = args.value(QLatin1String("path")).toString().trimmed();
    if (!path.isEmpty()) {
        const QJsonObject opened = openDocument(QJsonObject{{QStringLiteral("path"), path}});
        if (opened.value(QStringLiteral("isError")).toBool()) return opened;
    }
    QucsDoc* doc = a_app->getDoc();
    auto* model = dynamic_cast<RF25Doc*>(doc);
    if (model == nullptr)
        return errorResult(tr("%1 is no RF 2.5D model (.qrf25): rf25_stack_export writes the stack of one.")
                               .arg(doc != nullptr ? QFileInfo(doc->getDocName()).fileName() : tr("nothing is open")));
    QString where = args.value(QLatin1String("file")).toString().trimmed();
    if (where.isEmpty()) return errorResult(tr("'file' says where: a .xml for an HFSS™ 3D Layout XML stackup, a .proc for an EMX process file."));
    if (const QString bad = badFileName(QFileInfo(where).fileName()); !bad.isEmpty()) return errorResult(tr("'file': %1.").arg(bad));
    const QString name = model->getDocName();
    const QDir dir = name.isEmpty() ? QDir(QucsSettings.QucsWorkDir) : QFileInfo(name).absoluteDir();
    where = QDir::cleanPath(dir.absoluteFilePath(where));
    const std::optional<StackFormat> format = stackFormatOf(where);
    if (!format)
        return errorResult(tr("'file' is a .xml (an HFSS™ 3D Layout XML stackup) or a .proc (an EMX process file), not %1.").arg(QFileInfo(where).fileName()));
    if (!QFileInfo(QFileInfo(where).absolutePath()).isDir()) return errorResult(tr("There is no folder %1.").arg(QFileInfo(where).absolutePath()));
    if (const QString no = aboutToWrite(where); !no.isEmpty()) return errorResult(no);
    StackWritten written;
    QString why;
    if (!model->exportStack(where, &written, &why)) return errorResult(why);
    return jsonResult(QJsonObject{{QStringLiteral("file"), where},
                                  {QStringLiteral("format"), stackFormatName(*format)},
                                  {QStringLiteral("layers"), written.layers},
                                  {QStringLiteral("vias"), written.vias},
                                  {QStringLiteral("gds_layers"), written.mapped},
                                  {QStringLiteral("notes"), QJsonArray::fromStringList(written.notes)}});
}

QJsonObject QucsControl::rf25StackImport(const QJsonObject& args)
{
    const QString path = args.value(QLatin1String("path")).toString().trimmed();
    if (!path.isEmpty()) {
        const QJsonObject opened = openDocument(QJsonObject{{QStringLiteral("path"), path}});
        if (opened.value(QStringLiteral("isError")).toBool()) return opened;
    }
    QucsDoc* doc = a_app->getDoc();
    auto* model = dynamic_cast<RF25Doc*>(doc);
    if (model == nullptr)
        return errorResult(tr("%1 is no RF 2.5D model (.qrf25): rf25_stack_import reads a stack into one.")
                               .arg(doc != nullptr ? QFileInfo(doc->getDocName()).fileName() : tr("nothing is open")));
    QString file = args.value(QLatin1String("file")).toString().trimmed();
    if (file.isEmpty()) return errorResult(tr("'file' names the stack file: an HFSS™ 3D Layout XML stackup (.xml) or an EMX process file (.proc)."));
    const QString name = model->getDocName();
    const QDir dir = name.isEmpty() ? QDir(QucsSettings.QucsWorkDir) : QFileInfo(name).absoluteDir();
    file = QDir::cleanPath(dir.absoluteFilePath(file));
    if (!QFileInfo(file).isFile()) return errorResult(tr("There is no file %1.").arg(QDir::toNativeSeparators(file)));
    StackRead read;
    QStringList notes;
    QString why;
    if (!model->importStack(file, &read, &notes, &why)) return errorResult(why);
    return jsonResult(QJsonObject{{QStringLiteral("file"), file},
                                  {QStringLiteral("format"), stackFormatName(read.format)},
                                  {QStringLiteral("stack"), QJsonArray::fromStringList(read.layers)},
                                  {QStringLiteral("vias"), read.vias},
                                  {QStringLiteral("layer_map_rows"), read.rows},
                                  {QStringLiteral("notes"), QJsonArray::fromStringList(notes)},
                                  {QStringLiteral("problems"), QJsonArray::fromStringList(model->problems())}});
}

QJsonObject QucsControl::rf25HfssImport(const QJsonObject& args)
{
    const QString given = args.value(QLatin1String("path")).toString().trimmed();
    if (given.isEmpty()) return errorResult(tr("'path' names what to read: an .aedt, an .aedb folder or its edb.def, a control file (.xml) or an exported script (.py)."));
    const QString path = absolute(given);
    QString layoutFile = args.value(QLatin1String("layout")).toString().trimmed();
    const QFileInfo info(path);
    if (!layoutFile.isEmpty()) {
        layoutFile = QFileInfo(layoutFile).isAbsolute() ? layoutFile : info.absoluteDir().absoluteFilePath(layoutFile);
        if (!QFileInfo(layoutFile).isFile()) return errorResult(tr("There is no file %1.").arg(QDir::toNativeSeparators(layoutFile)));
    }
    QString why;
    const HfssImport read = hfssImportOf(path, layoutFile, &why);
    if (!read.model) return errorResult(why);

    // Where the model goes: as asked, relative to what was read; else beside it.
    const QString edbFolder = info.fileName() == QLatin1String("edb.def") ? info.absolutePath() : QString();
    const QFileInfo named(edbFolder.isEmpty() ? path : edbFolder);
    QString where = args.value(QLatin1String("save_as")).toString().trimmed();
    if (where.isEmpty()) where = named.completeBaseName() + QStringLiteral(".qrf25");
    if (const QString bad = badFileName(QFileInfo(where).fileName()); !bad.isEmpty()) return errorResult(tr("'save_as': %1.").arg(bad));
    where = QFileInfo(where).isAbsolute() ? QDir::cleanPath(where) : QDir::cleanPath(named.absoluteDir().absoluteFilePath(where));
    if (QFileInfo(where).suffix().compare(QLatin1String("qrf25"), Qt::CaseInsensitive) != 0)
        return errorResult(tr("'save_as' is a model's, a .qrf25."));
    if (!QFileInfo(QFileInfo(where).absolutePath()).isDir()) return errorResult(tr("There is no folder %1.").arg(QFileInfo(where).absolutePath()));
    const bool replace = args.value(QLatin1String("replace")).toBool();
    // (Its shapes, where it keeps naming no layout, go in a .gds of its name.)
    const QString shapesFile = read.layoutFile.isEmpty() && !read.shapes.isEmpty() ? QFileInfo(where).absoluteDir().filePath(QFileInfo(where).completeBaseName() + QStringLiteral(".gds")) : QString();
    for (const QString& file : {where, shapesFile})
        if (!file.isEmpty() && QFileInfo::exists(file) && !replace)
            return errorResult(tr("%1 is there already: 'replace' writes over it.").arg(QDir::toNativeSeparators(file)));
    for (QucsDoc* d : a_app->allDocuments())
        for (const QString& file : {where, shapesFile})
            if (!file.isEmpty() && sameFile(d->getDocName(), file)) return errorResult(tr("%1 is open: close it first.").arg(QFileInfo(file).fileName()));
    if (const QString no = aboutToWrite(where); !no.isEmpty()) return errorResult(no);
    if (!shapesFile.isEmpty())
        if (const QString no = aboutToWrite(shapesFile); !no.isEmpty()) return errorResult(no);
    QStringList files;
    if (!writeHfssImport(read, where, &files, &why)) return errorResult(why);
    const QJsonObject opened = openDocument(QJsonObject{{QStringLiteral("path"), where}});
    if (opened.value(QStringLiteral("isError")).toBool()) return opened;

    QJsonObject sweep;
    if (read.points > 0) {
        sweep.insert(QStringLiteral("from_hz"), read.from);
        sweep.insert(QStringLiteral("to_hz"), read.to);
        sweep.insert(QStringLiteral("points"), read.points);
    }
    return jsonResult(QJsonObject{{QStringLiteral("model"), where},
                                  {QStringLiteral("read_as"), read.kind},
                                  {QStringLiteral("stack"), QJsonArray::fromStringList(read.layers)},
                                  {QStringLiteral("polygons"), read.polygons},
                                  {QStringLiteral("vias"), read.vias},
                                  {QStringLiteral("ports"), read.ports},
                                  {QStringLiteral("sweep"), sweep},
                                  {QStringLiteral("notes"), QJsonArray::fromStringList(read.notes)},
                                  {QStringLiteral("problems"), QJsonArray::fromStringList(read.problems)},
                                  {QStringLiteral("files"), QJsonArray::fromStringList(files)}});
}

QJsonObject QucsControl::rf25Compare(const QJsonObject& args)
{
    QucsDoc* doc = nullptr;
    const QString path = args.value(QLatin1String("path")).toString().trimmed();
    if (path.isEmpty()) {
        doc = a_app->getDoc();
    } else {
        const QJsonObject opened = openDocument(QJsonObject{{QStringLiteral("path"), path}});
        if (opened.value(QStringLiteral("isError")).toBool()) return opened;
        doc = a_app->getDoc();
    }
    auto* model = dynamic_cast<RF25Doc*>(doc);
    if (model == nullptr)
        return errorResult(tr("%1 is no RF 2.5D model (.qrf25): rf25_compare sets the full-wave solve of one beside a Touchstone file.")
                               .arg(doc != nullptr ? QFileInfo(doc->getDocName()).fileName() : tr("nothing is open")));
    // (Held from here to its answer: its tab closed meanwhile - by the user, by
    //  another call -, it is not deleted under this call, which says so.)
    const RF25Doc::Held held(model);
    const QString heldName = QFileInfo(model->getDocName()).fileName();
    const bool laidOut = model->waitForLayout(300000);
    if (model->closedWhenFree()) return closedWhileWaited(heldName);
    if (!laidOut) return errorResult(tr("Its layout is still being read after %1 s.").arg(300));
    const bool prepared = model->waitForPrepared(300000);
    if (model->closedWhenFree()) return closedWhileWaited(heldName);
    if (!prepared)
        return errorResult(tr("Its shapes and its mesh are still being made after %1 s.").arg(300));
    const std::shared_ptr<const FullWave> wave = model->fullWave();
    if (!wave || !wave->ok())
        return errorResult(model->solveProblem().isEmpty() ? tr("It has no full-wave solve to compare: rf25_solve it first, or press Solve on its tab.") : model->solveProblem());
    const QString name = model->getDocName();
    const QDir dir = name.isEmpty() ? QDir(QucsSettings.QucsWorkDir) : QFileInfo(name).absoluteDir();
    const QString given = args.value(QLatin1String("file")).toString().trimmed();
    if (given.isEmpty()) return errorResult(tr("'file' names the Touchstone file (.sNp) to compare it with."));
    const QString file = QDir::cleanPath(dir.absoluteFilePath(given));
    if (!QFileInfo(file).isFile()) return errorResult(tr("There is no file %1.").arg(QDir::toNativeSeparators(file)));
    QList<int> order;
    for (const QJsonValue& v : args.value(QLatin1String("order")).toArray()) order << v.toInt();
    const SCompared compared = compareWithTouchstone(*wave, file, order);
    if (!compared.ok()) return errorResult(compared.problem);

    const auto saidOf = [this](const SCompared::Term& t) {
        return tr("S%1,%2: at most %3 apart, at %4 GHz; rms %5; at most %6 dB and %7 degrees")
            .arg(t.i).arg(t.j).arg(t.worst, 0, 'g', 3).arg(t.worstAt / 1e9, 0, 'g', 5).arg(t.rms, 0, 'g', 3).arg(t.dbWorst, 0, 'f', 3).arg(t.degWorst, 0, 'f', 2);
    };
    QJsonArray terms;
    for (const SCompared::Term& t : compared.terms)
        terms.append(QJsonObject{{QStringLiteral("term"), QStringLiteral("S%1,%2").arg(t.i).arg(t.j)},
                                 {QStringLiteral("apart"), t.worst},
                                 {QStringLiteral("at_hz"), t.worstAt},
                                 {QStringLiteral("rms"), t.rms},
                                 {QStringLiteral("db"), t.dbWorst},
                                 {QStringLiteral("degrees"), t.degWorst},
                                 {QStringLiteral("said"), saidOf(t)}});
    QJsonObject result{{QStringLiteral("file"), file},
                       {QStringLiteral("ports"), compared.ports},
                       {QStringLiteral("frequencies"), int(compared.frequencies.size())},
                       {QStringLiteral("interpolated"), compared.interpolated},
                       {QStringLiteral("reference_ohm"), compared.reference > 0 ? QJsonValue(compared.reference) : QJsonValue(tr("each port's own, as the file says"))},
                       {QStringLiteral("worst"), QJsonObject{{QStringLiteral("term"), QStringLiteral("S%1,%2").arg(compared.worstI).arg(compared.worstJ)},
                                                             {QStringLiteral("apart"), compared.worst},
                                                             {QStringLiteral("at_hz"), compared.worstAt}}},
                       {QStringLiteral("said"), tr("At most %1 apart: S%2,%3 at %4 GHz, over %5 frequencies.")
                                                     .arg(compared.worst, 0, 'g', 3).arg(compared.worstI).arg(compared.worstJ).arg(compared.worstAt / 1e9, 0, 'g', 5).arg(compared.frequencies.size())},
                       {QStringLiteral("terms"), terms},
                       {QStringLiteral("notes"), QJsonArray::fromStringList(compared.notes)}};
    const QString table = args.value(QLatin1String("table")).toString().trimmed();
    if (!table.isEmpty()) {
        const QString where = QDir::cleanPath(dir.absoluteFilePath(table));
        if (const QString no = aboutToWrite(where); !no.isEmpty()) return errorResult(no);
        QFile out(where);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Text))
            return errorResult(tr("%1 could not be written: %2.").arg(QDir::toNativeSeparators(where), out.errorString()));
        out.write(comparisonTable(compared, QFileInfo(name).fileName(), QFileInfo(file).fileName()).toUtf8());
        out.close();
        result.insert(QStringLiteral("table"), where);
    }
    return jsonResult(result);
}
