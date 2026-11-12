/*
 * rf25_solve.cpp - an RF 2.5D model solved without its tab (rf25_solve.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "rf25_solve.h"

#include "layout.h"
#include "rf25_deembed.h"
#include "rf25_sparams.h"
#include "rf25_trl.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>

namespace qucs_s::rf25 {

namespace {

/// (What the tab said, said in its words: these were the tab's own.)
QString tr(const char* text, const char* disambiguation = nullptr, int n = -1)
{
    return QCoreApplication::translate("RF25Doc", text, disambiguation, n);
}

} // namespace

/// How the study asks a full-wave solve to hold its system (its Matrix).
FullWaveControl::Matrix studyMatrix(const Model& model)
{
    const Node* study = model.root.child(QStringLiteral("rf_study"));
    const QString matrix = study != nullptr ? textOf(*study, QStringLiteral("matrix")) : QString();
    if (matrix == QLatin1String("dense")) return FullWaveControl::Matrix::Dense;
    if (matrix == QLatin1String("compressed")) return FullWaveControl::Matrix::Compressed;
    return FullWaveControl::Matrix::Automatic;
}

/// Whether the model's mesh is for a full-wave solve held compressed - the
/// study's Solver full wave, and its Matrix not dense: then the mesh may
/// have as many cells as that holds (MeshSettings::compressed).
bool meshedCompressed(const Model& model)
{
    const Node* study = model.root.child(QStringLiteral("rf_study"));
    return study != nullptr && textOf(*study, QStringLiteral("kind")) == QLatin1String("full_wave")
           && studyMatrix(model) != FullWaveControl::Matrix::Dense;
}

/// How the tab meshes \a model: its Mesh node's settings, held compressed as
/// its study is, and narrow gaps graded - the structure and, for a full-wave
/// study, the strips of its ports' feeds alike (calibrate()). For both
/// studies: a quasi-static one's capacitance is in the charge crowding on
/// both edges of a gap (a coplanar line's 6 µm gaps on four cells across
/// left it 10 % short), and a full-wave one's loss and impedance too in the
/// current crowding there (a measured coplanar line on alumina lost 19 %
/// under its 2D eddy-current solve on the edges' one halving, its impedance
/// 17 % over; 1 % under to 10 % over on the graded edges, its impedance 6 %
/// over).
MeshSettings tabMeshSettings(const Model& model, QStringList* said)
{
    MeshSettings how = meshSettingsOf(model.mesh(), model.parameters(), said);
    how.compressed = meshedCompressed(model);
    how.gaps = true;
    return how;
}

/// The model's ports as the full-wave solve takes them: an edge port on its
/// metal, facing the way it says; an internal port from the metal it names
/// (or the ground) to the other; each with its own reference and its
/// de-embedding. One that cannot be read is \a problem, and nothing is
/// returned.
/// The model's Return Conductors as the solvers take them: each one's point,
/// in the layout's units, on its metal. One whose point is no number is
/// \a problem, and nothing is returned.
QList<ReturnMark> returnMarksOf(const Model& model, QString* problem)
{
    QList<ReturnMark> out;
    for (const Node& node : model.ports().children) {
        if (!node.enabled || node.type != QLatin1String("rf_return")) continue;
        bool okX = true, okY = true;
        const double x = numberOf(node, QStringLiteral("x"), model, 0, &okX);
        const double y = numberOf(node, QStringLiteral("y"), model, 0, &okY);
        if (!okX || !okY) {
            *problem = tr("%1's %2 is not a number.")
                           .arg(node.label.isEmpty() ? node.tag : node.label, !okX ? QStringLiteral("x") : QStringLiteral("y"));
            return {};
        }
        out << ReturnMark{QPointF(x, y) / 1e-6, textOf(node, QStringLiteral("conductor")).trimmed()};
    }
    return out;
}

QList<FullWavePort> portsOf(const Model& model, QString* problem);

/// A standard of a TRL calibration - the model in \a path, read, its layout
/// read and processed and meshed as its own tab would - solved full-wave as
/// it is at \a frequencies, its ports as the model has them, under
/// \a control's stop. Empty and why in \a why where it cannot be.
std::optional<FullWave> solvedStandard(const QString& path, const QList<double>& frequencies, const FullWaveControl& control,
                                       QString* why)
{
    const auto fail = [why](const QString& text) {
        *why = text;
        return std::optional<FullWave>();
    };
    const QString name = QFileInfo(path).fileName();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return fail(tr("%1 could not be read: %2.").arg(name, f.errorString()));
    QString notes;
    const std::optional<Model> model = Model::fromJson(f.readAll(), &notes);
    if (!model) return fail(tr("%1 is no RF 2.5D model: %2").arg(name, notes));
    const QString named = textOf(model->layout(), QStringLiteral("file")).trimmed();
    if (named.isEmpty()) return fail(tr("%1 names no layout.").arg(name));
    const QString layoutPath = QFileInfo(named).isAbsolute() ? named : QFileInfo(path).absoluteDir().absoluteFilePath(named);
    QString error;
    std::atomic_bool cancelled{false};
    qucs_s::layout::Progress progress{0};
    const std::shared_ptr<const qucs_s::layout::Layout> read = qucs_s::layout::read(layoutPath, &error, &cancelled, &progress);
    if (!read) return fail(tr("%1's layout could not be read: %2").arg(name, error));
    const Processed processed = processRead(*model, *read, QFileInfo(layoutPath).fileName());
    if (!processed.ok()) return fail(tr("%1: %2").arg(name, processed.problems.join(QLatin1Char(' '))));
    QStringList said;
    const Mesh mesh = meshOf(processed.conductors, tabMeshSettings(*model, &said), model->stack(), topFrequency(*model));
    QString problem;
    const QList<FullWavePort> ports = portsOf(*model, &problem);
    if (!problem.isEmpty()) return fail(tr("%1: %2").arg(name, problem));
    if (ports.size() != 2) return fail(tr("%1 has %2 port(s): a standard has two.").arg(name).arg(ports.size()));
    const QList<ReturnMark> returns = returnMarksOf(*model, &problem);
    FullWaveControl c = control;
    c.extents.clear();
    const Stack stack = model->stack();
    for (auto it = processed.extents.cbegin(); it != processed.extents.cend(); ++it)
        if (const Layer* l = stack.named(it.key())) c.extents << Extent{l->name, it.value(), l->beyond};
    // (Raw - fullWaveOf() de-embeds nothing: a standard's ports as solved,
    //  the error boxes taking their faces and their fixtures alike.)
    FullWave w = fullWaveOf(mesh, processed.conductors, stack, ports, frequencies, 1e-6, &c, returns);
    if (!w.ok()) return fail(tr("%1 did not solve: %2").arg(name, w.problems.join(QLatin1Char(' '))));
    return w;
}

/// \a wave's S as solved - before any de-embedding - at each frequency, in
/// 50 Ohm at both ports.
QList<S2> rawS(const FullWave& wave)
{
    const QList<QList<QList<Complex>>>& z = wave.zSolved.isEmpty() ? wave.z : wave.zSolved;
    QList<S2> out;
    for (const QList<QList<Complex>>& m : z) {
        Eigen::Matrix2cd Z;
        Z << m.at(0).at(0), m.at(0).at(1), m.at(1).at(0), m.at(1).at(1);
        const Eigen::Matrix2cd one = Eigen::Matrix2cd::Identity();
        const Eigen::Matrix2cd S = (Z - 50.0 * one) * (Z + 50.0 * one).inverse();
        out << S2{S(0, 0), S(0, 1), S(1, 0), S(1, 1)};
    }
    return out;
}

/// A way a face is said ("+x"), as a direction; nought for "auto" or none.
QPointF facingOf(const QString& facing)
{
    if (facing == QLatin1String("+x")) return QPointF(1, 0);
    if (facing == QLatin1String("-x")) return QPointF(-1, 0);
    if (facing == QLatin1String("+y")) return QPointF(0, 1);
    if (facing == QLatin1String("-y")) return QPointF(0, -1);
    return QPointF();
}

/// The returns a port names (its "returns"), as the full-wave solve takes
/// them: each a terminal of the model - its point, metal and rectangle, or
/// its face - or another port, by its name. One that is neither, an empty
/// list, or a terminal whose numbers are none is \a problem.
QList<PortTerminal> returnsOf(const Node& port, const Model& model, QString* problem)
{
    QList<PortTerminal> out;
    const QString name = portName(port);
    const QStringList named = namesOf(port, QStringLiteral("returns"));
    if (named.isEmpty()) {
        *problem = tr("%1 returns through the terminals and ports it names, and names "
                                                          "none: name them (its Returns - a Terminal under Ports, or "
                                                          "another port), or return it another way.")
                       .arg(name);
        return {};
    }
    for (const QString& r : named) {
        // (A terminal by that name before a port: the model says where the
        //  two share one.)
        const Node* terminal = nullptr;
        const Node* other = nullptr;
        for (const Node& n : model.ports().children) {
            if (!n.enabled || portName(n) != r) continue;
            if (n.type == QLatin1String("rf_terminal") && terminal == nullptr) terminal = &n;
            else if ((n.type == QLatin1String("rf_edge_port") || n.type == QLatin1String("rf_internal_port")) && other == nullptr)
                other = &n;
        }
        const Node* found = terminal != nullptr ? terminal : other;
        const bool isPort = terminal == nullptr && other != nullptr;
        if (found == nullptr) {
            *problem = tr("%1 returns through %2, which is no terminal or port of the "
                                                              "model (or it is disabled).")
                           .arg(name, r);
            return {};
        }
        PortTerminal t;
        t.name = r;
        t.port = isPort;
        if (!isPort) {
            bool okX = true, okY = true, okW = true, okH = true;
            const double x = numberOf(*found, QStringLiteral("x"), model, 0, &okX);
            const double y = numberOf(*found, QStringLiteral("y"), model, 0, &okY);
            const double w = numberOf(*found, QStringLiteral("width"), model, 0, &okW);
            const double h = numberOf(*found, QStringLiteral("height"), model, 0, &okH);
            if (!okX || !okY) {
                *problem = tr("%1's %2 is not a number.").arg(r, !okX ? QStringLiteral("x") : QStringLiteral("y"));
                return {};
            }
            if (!okW || !okH || w < 0 || h < 0) {
                *problem = tr("%1's rectangle is none: its width and height are lengths "
                                                                  "of nought or more.")
                               .arg(r);
                return {};
            }
            // (The terminals are in metres; the cells are in the layout's own.)
            t.at = QPointF(x, y) / 1e-6;
            t.metal = textOf(*found, QStringLiteral("metal")).trimmed();
            const QString face = textOf(*found, QStringLiteral("face"));
            t.face = !face.isEmpty() && face != QLatin1String("none");
            t.facing = facingOf(face);
            if (!t.face) t.size = QSizeF(w, h) / 1e-6;
        }
        out << t;
    }
    return out;
}

QList<FullWavePort> portsOf(const Model& model, QString* problem)
{
    QList<FullWavePort> out;
    for (const Node& port : model.ports().children) {
        if (!port.enabled) continue;
        const bool edge = port.type == QLatin1String("rf_edge_port");
        if (!edge && port.type != QLatin1String("rf_internal_port")) continue;
        FullWavePort p;
        p.name = port.label.isEmpty() ? port.tag : port.label;
        bool okX = true, okY = true, okZ = true;
        const double x = numberOf(port, QStringLiteral("x"), model, 0, &okX);
        const double y = numberOf(port, QStringLiteral("y"), model, 0, &okY);
        p.impedance = numberOf(port, QStringLiteral("impedance"), model, 50, &okZ);
        if (!okX || !okY || !okZ) {
            *problem = tr("%1's %2 is not a number.")
                           .arg(p.name, !okX ? QStringLiteral("x") : !okY ? QStringLiteral("y")
                                                                           : tr("reference impedance"));
            return {};
        }
        // (The ports are in metres; the cells are in the layout's own.)
        p.at = QPointF(x, y) / 1e-6;
        p.pair = textOf(port, QStringLiteral("pair")).trimmed();
        // Through the terminals and ports it names, wherever they are.
        if (textOf(port, QStringLiteral("return")) == QLatin1String("terminals")) {
            p.terminals = true;
            p.returns = returnsOf(port, model, problem);
            if (p.returns.isEmpty()) return {};
        }
        if (edge) {
            p.metal = textOf(port, QStringLiteral("conductor")).trimmed();
            p.facing = facingOf(textOf(port, QStringLiteral("facing")));
            // Its own discontinuity taken out after the solve, by strips of
            // its feed, and its reference plane moved along the line - asked
            // for, or implied by a move.
            bool ok = true;
            p.shift = numberOf(port, QStringLiteral("deembed"), model, 0, &ok);
            if (!ok) {
                *problem = tr("%1's de-embedding is not a number.").arg(p.name);
                return {};
            }
            p.calibrate = flagOf(port, QStringLiteral("calibrate"));
            bool okStrips = true;
            p.strips = numberOf(port, QStringLiteral("strips"), model, 0, &okStrips);
            if (!okStrips || !(p.strips >= 0)) {
                *problem = tr("%1's calibration strips are no length: nought, to have "
                                                                  "them chosen, or above.")
                               .arg(p.name);
                return {};
            }
            p.coplanar = textOf(port, QStringLiteral("return")) == QLatin1String("beside");
            if (textOf(port, QStringLiteral("along")) == QLatin1String("line")) {
                bool okZ = true, okE = true;
                p.lineImpedance = numberOf(port, QStringLiteral("line_z0"), model, 50, &okZ);
                p.lineEps = numberOf(port, QStringLiteral("line_eps"), model, 1, &okE);
                if (!okZ || !okE || !(p.lineImpedance > 0) || !(p.lineEps >= 1)) {
                    *problem = tr("The line %1's reference plane is to move along is "
                                                                      "none: its impedance above nought and its effective "
                                                                      "permittivity at least one.")
                                   .arg(p.name);
                    return {};
                }
            }
        } else {
            p.internal = true;
            p.metal = textOf(port, QStringLiteral("to")).trimmed();
            // Over a rectangle about its point, where it has one.
            bool okW = true, okH = true;
            const double w = numberOf(port, QStringLiteral("width"), model, 0, &okW);
            const double h = numberOf(port, QStringLiteral("height"), model, 0, &okH);
            if (!okW || !okH || w < 0 || h < 0) {
                *problem = tr("%1's rectangle is none: its width and height are "
                                                                  "lengths of nought or more.")
                               .arg(p.name);
                return {};
            }
            p.size = QSizeF(w, h) / 1e-6;
            const QString from = textOf(port, QStringLiteral("from")).trimmed();
            p.from = p.terminals || from.compare(QLatin1String("ground"), Qt::CaseInsensitive) == 0 ? QString() : from;
        }
        out << p;
    }
    return out;
}

bool studyIsFullWave(const Model& model)
{
    const Node* study = model.root.child(QStringLiteral("rf_study"));
    return study != nullptr && textOf(*study, QStringLiteral("kind")) == QLatin1String("full_wave");
}

/// What a solve's mesh is against what it cuts - its smallest and largest
/// cells, the cells across the narrowest conductor and the narrowest gap -,
/// the first of the solve's own lines: how fine it is, said where the
/// answer is (F1 of the open items of 8 November).
QString meshNoteOf(const Mesh& mesh)
{
    if (mesh.cells.isEmpty()) return QString();
    QString note = tr("Solved on %1 cell(s) from %2 to %3 µm")
                       .arg(mesh.cells.size()).arg(mesh.smallest, 0, 'g', 3).arg(mesh.largest, 0, 'g', 3);
    if (mesh.narrowest > 0 && mesh.across > 0)
        note += tr("; %1 across the narrowest conductor, %2 µm, each %3 µm there")
                    .arg(mesh.across).arg(mesh.narrowest, 0, 'g', 4).arg(mesh.acrossCell, 0, 'g', 3);
    if (mesh.narrowestGap > 0 && mesh.smallest > 0)
        note += tr("; the narrowest gap %1 µm, %2 times the smallest cell")
                    .arg(mesh.narrowestGap, 0, 'g', 4).arg(mesh.narrowestGap / mesh.smallest, 0, 'g', 2);
    return note + tr(". How far that is from the limit: RF 2.5D > Check Mesh Convergence.");
}


// ------------------------------------------------------------- The solve

QString setUpSolve(SolveJob& job, const Model& model, const Processed& processed, const Mesh& mesh, const SolveRequest& request,
                   const QString& folder)
{
    const auto said = [](const QString& why) { return why; };
    job.mesh = mesh;
    job.conductors = processed.conductors;
    job.stack = model.stack();
    // The layers that end: their shapes, and what is beyond them.
    for (auto it = processed.extents.cbegin(); it != processed.extents.cend(); ++it) {
        const Layer* layer = job.stack.named(it.key());
        if (layer == nullptr) continue;
        Extent e;
        e.layer = layer->name;
        e.shapes = it.value();
        e.beyond = layer->beyond;
        job.extents << e;
    }
    // How long the piece that was solved is: the longer side of what it
    // covers, which is a straight line's own length and what makes a
    // capacitance into one per metre.
    QRectF bounds;
    for (const Conductor& c : std::as_const(job.conductors)) bounds = bounds.united(c.outline.boundingRect());
    job.length = std::max(bounds.width(), bounds.height()) * 1e-6;
    {
        QString problem;
        job.returns = returnMarksOf(model, &problem);
        if (!problem.isEmpty()) return said(problem);
    }
    job.fullWave = request.solver.isEmpty() ? studyIsFullWave(model) : request.solver == QLatin1String("full_wave");
    job.levels = request.levels;
    {
        QStringList said;
        job.how = tabMeshSettings(model, &said);
        job.top = topFrequency(model);
    }
    if (job.fullWave) {
        // The full-wave solve is driven at the ports, at each of the
        // study's frequencies (or of those asked for).
        QString problem;
        job.ports = portsOf(model, &problem);
        if (!problem.isEmpty()) return said(problem);
        if (job.ports.isEmpty())
            return said(tr("A full-wave solve is driven at its ports, and the model has none: put an edge port or an "
                              "internal port under Ports."));
        // A terminal no port names among its returns does nothing: said (it
        // solved without a word - the returns check of 4 November).
        {
            QSet<QString> named;
            for (const FullWavePort& p : std::as_const(job.ports))
                for (const PortTerminal& t : p.returns) named.insert(t.name);
            QStringList idle;
            for (const Node& p : model.ports().children)
                if (p.enabled && p.type == QLatin1String("rf_terminal") && !named.contains(portName(p))) idle << portName(p);
            if (!idle.isEmpty())
                job.notes << tr("%1 %2 named by no port's returns, so %3 nothing in this solve.")
                                  .arg(idle.join(QStringLiteral(", ")), idle.size() == 1 ? tr("is") : tr("are"),
                                       idle.size() == 1 ? tr("it does") : tr("they do"));
        }
        QString why;
        // (A convergence check at the sweep's top, where none are asked: a
        //  mesh is worst at its highest frequency.)
        job.frequencies = !request.frequencies.isEmpty() ? request.frequencies
                           : job.levels.isEmpty()        ? studySweep(model, &why)
                                                          : QList<double>{topFrequency(model)};
        if (job.frequencies.isEmpty() || !(job.frequencies.first() > 0))
            return said(why.isEmpty() ? tr("The study asks for no frequencies to solve at.") : why);
        job.matrix = request.matrix.isEmpty() ? studyMatrix(model)
                      : request.matrix == QLatin1String("dense")    ? FullWaveControl::Matrix::Dense
                      : request.matrix == QLatin1String("compressed") ? FullWaveControl::Matrix::Compressed
                                                                      : FullWaveControl::Matrix::Automatic;
        const Node* study = model.root.child(QStringLiteral("rf_study"));
        const bool studyAdaptive = study != nullptr && study->props.value(QStringLiteral("adaptive")).toBool();
        job.adaptive = request.frequencies.isEmpty() && job.levels.isEmpty()
                        && (request.adaptive < 0 ? studyAdaptive : request.adaptive > 0);
        if (job.adaptive && study != nullptr) {
            bool ok = true;
            const double tolerance = numberOf(*study, QStringLiteral("adaptive_tolerance"), model, 2e-3, &ok);
            if (!ok || !(tolerance > 0) || tolerance >= 1)
                return said(tr("The study's Tolerance is to be a number between nought and one: the most a term of "
                                  "S may differ from what the fit said of it."));
            job.tolerance = tolerance;
        }
        // A TRL calibration (rf25_trl.h): its standards' files, read here; each
        // solved with the structure, on its thread.
        for (const Node& n : model.ports().children) {
            if (!n.enabled || n.type != QLatin1String("rf_trl") || !job.levels.isEmpty()) continue;
            if (job.ports.size() != 2)
                return said(tr("A TRL calibration is of a two-port, and the model has %n port(s).", nullptr, int(job.ports.size())));
            if (job.adaptive)
                return said(tr("A TRL calibration wants every frequency solved, its standards at each: untick the study's "
                                  "Add points where it changes."));
            SolveJob::Trl t;
            const QDir dir(folder);
            const auto pathOf = [&dir](const QString& v) {
                const QString given = v.trimmed();
                return given.isEmpty() ? QString() : QFileInfo(given).isAbsolute() ? given : QDir::cleanPath(dir.absoluteFilePath(given));
            };
            const auto there = [](const QString& path, const QString& what) {
                if (path.isEmpty()) return QString(tr("The TRL calibration names no %1.").arg(what));
                if (!QFileInfo::exists(path)) return QString(tr("The TRL calibration's %1, %2, is not there.").arg(what, QDir::toNativeSeparators(path)));
                return QString();
            };
            t.thru = pathOf(textOf(n, QStringLiteral("thru")));
            t.reflect = pathOf(textOf(n, QStringLiteral("reflect")));
            if (const QString no = there(t.thru, tr("thru")); !no.isEmpty()) return said(no);
            if (const QString no = there(t.reflect, tr("reflect")); !no.isEmpty()) return said(no);
            for (const QJsonValue& row : n.props.value(QLatin1String("lines")).toArray()) {
                const QJsonArray cells = row.toArray();
                const QString file = pathOf(cells.at(0).toString());
                if (file.isEmpty()) continue;
                if (const QString no = there(file, tr("line")); !no.isEmpty()) return said(no);
                Node value;
                value.props.insert(QStringLiteral("v"), cells.size() > 1 ? cells.at(1).toString() : QString());
                bool ok = true;
                const double longer = numberOf(value, QStringLiteral("v"), model, 0, &ok);
                if (!ok || !(longer > 0))
                    return said(tr("The TRL calibration's line %1 is longer than the thru by no length: give it as \"500[um]\".")
                                       .arg(QFileInfo(file).fileName()));
                t.lines << qMakePair(file, longer);
            }
            if (t.lines.isEmpty()) return said(tr("The TRL calibration names no line: one at least, longer than the thru."));
            t.shortReflect = textOf(n, QStringLiteral("reflect_kind")) == QLatin1String("short");
            if (!textOf(n, QStringLiteral("impedance")).trimmed().isEmpty()) {
                bool ok = true;
                t.impedance = numberOf(n, QStringLiteral("impedance"), model, 0, &ok);
                if (!ok || !(t.impedance > 0)) return said(tr("The TRL calibration's lines' impedance is no impedance."));
            }
            for (const Layer& l : job.stack.layers)
                if (l.kind == Layer::Dielectric || l.kind == Layer::Substrate) t.epsGuess = std::max(t.epsGuess, 0.5 * (l.epsr + 1));
            job.trl = t;
            break;
        }
    } else if (!job.levels.isEmpty()) {
        // A quasi-static convergence check: the charge on each mesh, no loop.
    } else {
        // An inductor's L is of a loop, and what the loop is comes from the
        // ports: the current goes in at one and out at another. Two is what
        // it takes; with one, or none, there is no loop and the quasi-static
        // L of a uniform line is all that can be said.
        QList<QPointF> at;
        for (const Node& port : model.ports().children) {
            if (!port.enabled) continue;
            if (port.type != QLatin1String("rf_edge_port") && port.type != QLatin1String("rf_internal_port")) continue;
            bool ok = true;
            const double x = numberOf(port, QStringLiteral("x"), model, 0, &ok);
            if (!ok) continue;
            const double y = numberOf(port, QStringLiteral("y"), model, 0, &ok);
            if (!ok) continue;
            // (The ports are in metres; the cells are in the layout's own.)
            at << QPointF(x, y) / 1e-6;
        }
        if (at.size() >= 2) {
            job.loop = true;
            job.in = at.at(0);
            job.out = at.at(1);
            job.frequency = topFrequency(model);
            if (at.size() > 2)
                job.portsSaid = tr("%1 ports: the loop is taken from the first two.").arg(at.size());
        } else {
            job.portsSaid = at.size() == 1 ? tr("One port: a loop's inductance wants two, so that a current has "
                                                 "somewhere to go in and somewhere to come out.")
                                            : tr("No ports: a loop's inductance wants two of them.");
        }
    }
    return QString();
}

void runSolve(SolveJob& job)
{
    if (job.fullWave) {
        job.wave = solveWaveOn(job, job.mesh, job.how, job.frequencies, job.adaptive, job.tolerance, &job.notes);
        if (job.wave->ok()) job.wave->lines.prepend(meshNoteOf(job.mesh));
        if (job.trl && job.wave->ok() && !job.stop.load()) job.wave = trlOn(job, *job.wave);
        return;
    }
    // Told to stop, it stops: let go and left to run, an extraction whose
    // stack's Green's function would not end held a core and gigabytes
    // until Qucs-S was killed, and the next solve ran beside it.
    ExtractControl control;
    control.extents = job.extents;
    control.stop = &job.stop;
    control.step = &job.step;
    control.done = &job.stepDone;
    control.all = &job.stepAll;
    control.images = &job.images;
    job.result =
        std::make_shared<Extraction>(extract(job.mesh, job.conductors, job.stack, 1e-6, job.returns, &control));
    if (job.stop.load()) return;
    if (job.result->ok()) job.result->lines.prepend(meshNoteOf(job.mesh));
    if (job.loop)
        job.inductance = std::make_shared<Inductance>(
            inductanceOf(job.mesh, job.conductors, job.stack, job.in, job.out, 1e-6, job.frequency, &job.stop));
}

std::shared_ptr<FullWave> trlOn(SolveJob& job, const FullWave& wave)
{
    const SolveJob::Trl& t = *job.trl;
    auto out = std::make_shared<FullWave>(wave);
    const auto failed = [&out](const QString& why) {
        out->problems << why;
        return out;
    };
    FullWaveControl control;
    control.stop = &job.stop;
    control.matrix = job.matrix;
    const QList<double>& f = wave.frequencies;
    // Each standard as it is, at the structure's frequencies.
    TrlStandards st;
    st.frequencies = f;
    st.shortReflect = t.shortReflect;
    QString why;
    std::optional<FullWave> thru = solvedStandard(t.thru, f, control, &why);
    if (job.stop.load()) return out;
    if (!thru) return failed(tr("The TRL calibration's thru: %1").arg(why));
    st.thru = rawS(*thru);
    for (const auto& [file, longer] : t.lines) {
        std::optional<FullWave> line = solvedStandard(file, f, control, &why);
        if (job.stop.load()) return out;
        if (!line) return failed(tr("The TRL calibration's line: %1").arg(why));
        st.lines << rawS(*line);
        st.longer << longer;
    }
    std::optional<FullWave> reflect = solvedStandard(t.reflect, f, control, &why);
    if (job.stop.load()) return out;
    if (!reflect) return failed(tr("The TRL calibration's reflect: %1").arg(why));
    st.reflect = rawS(*reflect);
    const TrlCalibration c = calibrateTrl(st, t.epsGuess);
    if (!c.ok()) return failed(c.problems.join(QLatin1Char(' ')));
    // The structure as solved, its fixtures taken out: at the thru's middle,
    // in the lines' impedance - stated in the ports' references where it is
    // given.
    QList<S2> s = corrected(c, rawS(wave));
    const double r0 = wave.references.value(0, 50), r1 = wave.references.value(1, 50);
    QStringList said = c.lines;
    if (t.impedance > 0) {
        for (S2& m : s) m = renormalized(m, t.impedance, r0);
        said << tr("Its S stated in the ports' references from the lines' %1 Ohm.").arg(t.impedance, 0, 'g', 4);
        if (std::abs(r0 - r1) > 1e-9 * r0)
            out->warnings << tr("The two ports' references differ (%1 and %2 Ohm): the calibrated S is in the first's.").arg(r0).arg(r1);
    } else {
        out->warnings << tr("The calibrated S is in the lines' own impedance, which a calibration of lines alone cannot know: "
                            "give it (the TRL calibration's lines' impedance) to have S in the ports' references.");
    }
    if (!wave.zSolved.isEmpty())
        said << tr("The ports' own de-embedding is not taken: the error boxes hold their faces with the fixtures.");
    for (int k = 0; k < f.size() && k < s.size(); ++k) {
        const S2& m = s.at(k);
        out->s[k] = {{m[0], m[1]}, {m[2], m[3]}};
        Eigen::Matrix2cd S;
        S << m[0], m[1], m[2], m[3];
        const Eigen::Matrix2cd one = Eigen::Matrix2cd::Identity();
        const Eigen::Matrix2cd root = Eigen::Vector2cd(std::sqrt(r0), std::sqrt(r1)).asDiagonal();
        const Eigen::Matrix2cd z = root * (one + S) * (one - S).inverse() * root;
        out->z[k] = {{z(0, 0), z(0, 1)}, {z(1, 0), z(1, 1)}};
    }
    // (As solved, beside the calibrated: what the currents are of.)
    if (out->zSolved.isEmpty()) out->zSolved = wave.z;
    checkReciprocityAndPassivity(*out);
    out->lines << said;
    out->warnings << c.warnings;
    return out;
}

std::shared_ptr<FullWave> solveWaveOn(SolveJob& job, const Mesh& mesh, const MeshSettings& how,
                                               const QList<double>& frequencies, bool adaptive, double tolerance,
                                               QStringList* notes)
{
    FullWaveControl control;
    control.extents = job.extents;
    control.stop = &job.stop;
    control.done = &job.done;
    control.gmresIterations = &job.gmresIterations;
    control.gmresResidual = &job.gmresResidual;
    control.atOnce = &job.atOnce;
    control.allowed = job.allowed.get();
    control.matrix = job.matrix;
    const auto solveAt = [&job, &mesh, &control](const QList<double>& f) {
        job.asked += int(f.size());
        return fullWaveOf(mesh, job.conductors, job.stack, job.ports, f, 1e-6, &control, job.returns);
    };
    std::shared_ptr<FullWave> wave;
    // The frequencies solved, which the ports' feeds are calibrated at: an
    // adaptive sweep's few, its calibrations read at the rest as it reads
    // the structure (calibrationAt()).
    QList<double> solvedAt = frequencies;
    if (adaptive) {
        AdaptiveSettings settings;
        settings.tolerance = tolerance;
        const Adapted a = adaptiveSweep(frequencies, solveAt, settings, [&job] { return job.stop.load(); });
        wave = std::make_shared<FullWave>(a.wave);
        if (!a.whole) solvedAt = a.solved;
    } else {
        wave = std::make_shared<FullWave>(solveAt(frequencies));
    }
    // The ports that asked: their own discontinuity taken out by strips of
    // their feeds - one calibration for each feed, its metal and its width,
    // and a coplanar one's grounds and vias - and their reference planes
    // moved.
    if (!wave->ok()) return wave;
    FullWaveControl quiet;
    quiet.stop = &job.stop;
    quiet.matrix = job.matrix;
    // (Counted for the bar: the strips as they begin, with their
    //  frequencies, and those frequencies as they are solved.)
    quiet.done = &job.calibrated;
    quiet.allowed = job.allowed.get();
    quiet.begins = [counted = &job](int count) {
        counted->stripFrequencies += count;
        ++counted->stripsBegun;
    };
    // (One calibration for feeds alike and strips asked alike.)
    const auto keyOf = [](const Feed& feed, const FullWavePort& port) {
        return feed.key() + QLatin1Char('|') + QString::number(port.strips, 'g', 9);
    };
    {
        // The calibrations to make, counted before the first is: the bar
        // says which of them is going on.
        QSet<QString> keys;
        for (int p = 0; p < job.ports.size(); ++p) {
            const FullWavePort& port = job.ports.at(p);
            const Feed feed = wave->feeds.value(p);
            if (port.wantsStrips() && !port.internal && feed.width > 0) keys.insert(keyOf(feed, port));
        }
        job.calibrations = int(keys.size());
        job.calibrationsBegun = 0;
    }
    QList<Deembedding> each(job.ports.size());
    QMap<QString, std::shared_ptr<Calibration>> feeds;
    bool asked = false;
    for (int p = 0; p < job.ports.size(); ++p) {
        const FullWavePort& port = job.ports.at(p);
        if (!port.deembeds()) continue;
        asked = true;
        each[p] = Deembedding{nullptr, port.shift, port.lineImpedance, port.lineEps};
        if (!port.wantsStrips()) continue;
        const QString name = wave->portNames.at(p);
        const Feed feed = wave->feeds.value(p);
        if (port.internal || !(feed.width > 0)) {
            *notes << tr("%1 is not on the face of a conductor, so it has no feed to calibrate: it is solved as it is.").arg(name);
            continue;
        }
        std::shared_ptr<Calibration>& calibration = feeds[keyOf(feed, port)];
        if (!calibration) {
            job.calibrating = true;
            // (The bar counts each calibration from nought: its strips
            //  begun first, so that it is busy meanwhile.)
            job.stripsBegun = 0;
            job.stripFrequencies = 0;
            job.calibrated = 0;
            ++job.calibrationsBegun;
            calibration = std::make_shared<Calibration>(
                calibrate(job.stack, how, job.top, feed, solvedAt, 1e-6, &quiet, port.strips / 1e-6));
            if (calibration->ok() && solvedAt.size() < frequencies.size()) {
                *calibration = calibrationAt(*calibration, frequencies);
                calibration->lines << tr("Its strips solved at the %1 frequencies the structure was, and read at the "
                                         "sweep's other %2 from a rational fit through them, as the structure is.")
                                          .arg(solvedAt.size()).arg(frequencies.size() - solvedAt.size());
            }
        }
        if (job.stop.load()) {
            wave->problems << tr("Stopped while taking out the ports' own discontinuities.");
            return wave;
        }
        if (!calibration->ok()) {
            *notes << tr("%1's feed could not be calibrated: %2 It is solved as it is.")
                          .arg(name, calibration->problems.join(QLatin1Char(' ')));
            continue;
        }
        each[p].calibration = calibration.get();
    }
    if (!asked) return wave;
    *wave = deembedded(*wave, each);
    for (const std::shared_ptr<Calibration>& c : std::as_const(feeds))
        if (c->ok()) {
            wave->lines << c->lines;
            // (Its strips' solves short of their residual: doubtful.)
            wave->warnings << c->warnings;
            wave->lines << c->warnings;
        }
    return wave;
}

void convergeOn(SolveJob& job)
{
    auto check = std::make_shared<MeshConvergence>();
    check->fullWave = job.fullWave;
    if (job.fullWave) check->frequencies = job.frequencies;
    for (int i = 0; i < job.levels.size() && !job.stop.load(); ++i) {
        job.levelAt = i;
        job.done = 0;
        job.asked = 0;
        job.calibrating = false;
        job.gmresIterations = 0;
        job.step = ExtractControl::Starting;
        job.stepDone = 0;
        job.stepAll = 0;
        QElapsedTimer t;
        t.start();
        // The model's mesh refined as a whole - the cells across, per
        // wavelength and at the gaps, and the largest - so many times as the
        // cells across asked are of the model's own: a mesh whose largest
        // cell, or the wavelength, sets its size was the same mesh at every
        // level when the cells across alone were changed.
        const double factor = double(job.levels.at(i)) / std::max(1, job.how.cellsAcross);
        MeshSettings how = refinedBy(job.how, factor);
        how.cellsAcross = job.levels.at(i);
        const Mesh mesh = meshOf(job.conductors, how, job.stack, job.top);
        if (!mesh.ok() || mesh.cells.isEmpty()) {
            check->problems << tr("The mesh of %1 cells across could not be made: %2")
                                   .arg(job.levels.at(i)).arg(mesh.problems.join(QLatin1Char(' ')));
            break;
        }
        MeshLevel level;
        level.asked = job.levels.at(i);
        level.factor = factor;
        level.across = mesh.across;
        level.cell = mesh.acrossCell;
        level.smallest = mesh.smallest;
        level.largest = mesh.largest;
        level.cells = int(mesh.cells.size());
        level.unknowns = mesh.cost.unknowns;
        QList<Watched> watched;
        if (job.fullWave) {
            QStringList notes;
            const std::shared_ptr<FullWave> wave = solveWaveOn(job, mesh, how, job.frequencies, false, 0, &notes);
            if (job.stop.load()) break;
            if (!wave->ok()) {
                check->problems << tr("At %1 cells across: %2").arg(level.asked).arg(wave->problems.join(QLatin1Char(' ')));
                break;
            }
            watched = watchedOf(*wave, job.frequencies);
        } else {
            ExtractControl control;
            control.extents = job.extents;
            control.stop = &job.stop;
            control.step = &job.step;
            control.done = &job.stepDone;
            control.all = &job.stepAll;
            control.images = &job.images;
            const Extraction e = extract(mesh, job.conductors, job.stack, 1e-6, job.returns, &control);
            if (job.stop.load()) break;
            if (!e.ok()) {
                check->problems << tr("At %1 cells across: %2").arg(level.asked).arg(e.problems.join(QLatin1Char(' ')));
                break;
            }
            watched = watchedOf(e);
        }
        level.seconds = double(t.elapsed()) / 1000;
        check->levels << level;
        addWatched(*check, watched);
    }
    if (job.stop.load())
        check->problems << tr("Stopped, %1 of %2 meshes solved.").arg(check->levels.size()).arg(job.levels.size());
    judge(*check);
    job.check = check;
}

ModelSolution solveModel(SolveJob& job, const Model& model, const qucs_s::layout::Layout& read, const QString& called, const QString& folder,
                         const SolveRequest& request)
{
    ModelSolution out;
    if (!request.levels.isEmpty()) {
        out.problem = tr("A mesh convergence check is the tab's: solveModel() solves the model once.");
        return out;
    }
    // Processed and meshed as the tab's preparation makes them.
    auto processed = std::make_shared<Processed>(processRead(model, read, called));
    out.processed = processed;
    QStringList said;
    const MeshSettings how = tabMeshSettings(model, &said);
    auto mesh = std::make_shared<Mesh>(meshOf(processed->conductors, how, model.stack(), topFrequency(model)));
    mesh->problems = said + mesh->problems;
    out.mesh = mesh;
    if (mesh->cells.isEmpty()) {
        QStringList why{tr("There is no mesh to solve on: give the model a layout and a layer map first.")};
        why += processed->problems;
        why += mesh->problems;
        out.problem = why.join(QLatin1Char(' '));
        return out;
    }
    out.problem = setUpSolve(job, model, *processed, *mesh, request, folder);
    if (!out.problem.isEmpty()) return out;
    if (job.stop.load()) {
        out.stopped = true;
        return out;
    }
    runSolve(job);
    out.stopped = job.stop.load();
    out.notes = job.notes;
    out.portsSaid = job.portsSaid;
    if (job.fullWave) {
        out.wave = job.wave;
        if (job.wave && !job.wave->ok()) out.problem = job.wave->problems.join(QStringLiteral("; "));
    } else {
        out.extraction = job.result;
        out.inductance = job.inductance;
        // (A uniform line's own length along it, as the tab says it.)
        out.length = job.result && job.result->perMetre.fromMiddle && job.result->perMetre.length > 0 ? job.result->perMetre.length : job.length;
        if (job.result && !job.result->ok()) out.problem = job.result->problems.join(QStringLiteral("; "));
    }
    if (out.problem.isEmpty() && !out.wave && !out.extraction && !out.stopped) out.problem = tr("The solve gave no answer.");
    return out;
}

} // namespace qucs_s::rf25
