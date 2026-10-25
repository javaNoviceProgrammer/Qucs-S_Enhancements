/*
 * qucscontrol_rf25.cpp - Claude's tools for an RF 2.5D model: rf25_solve,
 *                        the capacitance of its conductors, each net as a
 *                        line with its RLGC and S-parameters, and
 *                        rf25_export, which writes a Touchstone of them
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
#include "rf25doc.h"

#include <QFileInfo>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>

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

    const int timeout = args.contains(QLatin1String("timeout"))
                            ? qBound(1, args.value(QLatin1String("timeout")).toInt(300), 3600)
                            : 300;
    // The shapes and the cells first: the tab makes them at every edit,
    // and a solve is of them as they stand.
    if (!model->waitForLayout(timeout * 1000))
        return errorResult(tr("Its layout is still being read after %1 s.").arg(timeout));
    if (!model->waitForPrepared(timeout * 1000))
        return errorResult(tr("Its shapes and its mesh are still being made after %1 s.").arg(timeout));

    model->solve();
    if (!model->waitForSolved(timeout * 1000)) {
        model->stopSolving();
        return errorResult(tr("It is still solving after %1 s: mesh it more coarsely (the Mesh node's largest cell), or "
                              "give rf25_solve a longer 'timeout'.").arg(timeout));
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
    double frequency = topFrequency(model->model());
    if (args.contains(QLatin1String("frequency"))) {
        const std::optional<double> given = positiveNumber(args.value(QLatin1String("frequency")).toString().trimmed(), *model);
        if (!given) return errorResult(tr("'frequency' is a frequency above zero (\"10[GHz]\")."));
        frequency = *given;
    }
    // The ports the S-parameters are referred to.
    double zref = 50;
    if (args.contains(QLatin1String("impedance"))) {
        const std::optional<double> given = positiveNumber(args.value(QLatin1String("impedance")).toString().trimmed(), *model);
        if (!given) return errorResult(tr("'impedance' is a reference impedance above zero (\"50\")."));
        zref = *given;
    }

    QJsonObject out;
    out.insert(QStringLiteral("model"), QFileInfo(model->getDocName()).fileName());
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
        // One of several conductors is no line by itself: its own row - its
        // charge with every conductor at a volt - is neither of a pair's
        // modes, which "coupled lines" gives.
        if (e->names.size() > 1) {
            net.insert(QStringLiteral("line not given"),
                       tr("%1 is one of %2 coupled conductors, which have modes rather than a line each: see \"coupled "
                          "lines\".")
                           .arg(e->names.at(i))
                           .arg(e->names.size()));
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
                line.insert(QStringLiteral("G"), lp.g);
                line.insert(QStringLiteral("skin_depth"), lp.skinDepth);
                line.insert(QStringLiteral("surface_resistance"), lp.surfaceResistance);
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
    // Several conductors are coupled lines, whose modes are what a
    // designer of a pair actually asks after.
    double slowest = 0;   // m/s: the slowest mode's, for where the S parameters first turn over
    if (e->names.size() > 1 && length > 0) {
        const fem::CoupledLines cl = asCoupledLines(*e, length, frequency);
        if (cl.error.isEmpty() && !cl.modes.empty()) {
            QJsonObject coupled;
            coupled.insert(QStringLiteral("symmetric pair"), cl.symmetricPair);
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
            // The 2N-port: the near ends first, then the far ends.
            const std::vector<std::vector<std::complex<double>>> sm = fem::coupledSParameters(cl, length, zref);
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
            coupled.insert(QStringLiteral("s matrix"), rows);
            coupled.insert(QStringLiteral("ports"),
                           tr("1 to %1 are the near ends of %2, in that order; %3 to %4 their far ends")
                               .arg(e->names.size())
                               .arg(e->names.join(QStringLiteral(", ")))
                               .arg(e->names.size() + 1)
                               .arg(2 * e->names.size()));
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
        const QuasiStaticLimit limit = quasiStaticLimit(model->model().stack(), mesh ? *mesh : Mesh(), 1e-6, slowest, length,
                                                        int(e->names.size()));
        QJsonObject said;
        said.insert(QStringLiteral("frequency"), limit.frequency);
        said.insert(QStringLiteral("what sets it"), limit.what);
        said.insert(QStringLiteral("epsilon_max"), limit.epsilonMax);
        said.insert(QStringLiteral("first half-wave resonance"), limit.resonance);
        said.insert(QStringLiteral("why"),
                    tr("A per-metre RLGC is a property of the cross-section and holds while the cross-section is "
                       "electrically small; this is a tenth of a wavelength of the dimension named, which is the usual "
                       "engineering rule. The resonance is not a limit of the model - a line's own S parameters carry "
                       "it - only where they first turn over."));
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
                  "rf25_inductance's, by another method; no full-wave solve in this build; rf25_export writes a "
                  "Touchstone of these."));
    // Brief: the numbers a line is asked after and nothing else - an
    // answer of 2.5 kB a net had its line cut first by max_chars (the
    // phase B check of 24 October).
    if (args.value(QLatin1String("brief")).toBool()) {
        QJsonObject brief;
        brief.insert(QStringLiteral("model"), out.value(QStringLiteral("model")));
        brief.insert(QStringLiteral("length"), length);
        brief.insert(QStringLiteral("unknowns"), e->unknowns);
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
            QJsonArray modes;
            for (const QJsonValue& v : coupled.value(QStringLiteral("modes")).toArray()) {
                const QJsonObject mode = v.toObject();
                QJsonObject kept;
                for (const char* key : {"name", "Z", "epsilon_eff", "alpha"}) kept.insert(QLatin1String(key), mode.value(QLatin1String(key)));
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

    const int timeout = args.contains(QLatin1String("timeout"))
                            ? qBound(1, args.value(QLatin1String("timeout")).toInt(300), 3600)
                            : 300;
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
    // and the loop together, on the same copies.
    if (!model->waitForLayout(timeout * 1000))
        return errorResult(tr("Its layout is still being read after %1 s.").arg(timeout));
    if (!model->waitForPrepared(timeout * 1000))
        return errorResult(tr("Its shapes and its mesh are still being made after %1 s.").arg(timeout));
    model->solve();
    if (!model->waitForSolved(timeout * 1000)) {
        model->stopSolving();
        return errorResult(tr("It is still solving after %1 s: mesh it more coarsely (the Mesh node's largest cell), or "
                              "give rf25_inductance a longer 'timeout'.").arg(timeout));
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
    if (!model->extraction() || !model->extraction()->ok())
        return errorResult(model->solveProblem().isEmpty()
                               ? tr("Nothing is solved yet: rf25_solve it first, or press Solve on its tab.")
                               : model->solveProblem());

    // What to write: the export node named, else what the arguments say.
    RF25Doc::ExportRequest what;
    const QString node = args.value(QLatin1String("node")).toString().trimmed();
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
    if (args.contains(QLatin1String("impedance"))) {
        const std::optional<double> given = positiveNumber(args.value(QLatin1String("impedance")).toString().trimmed(), *model);
        if (!given) return errorResult(tr("'impedance' is a reference impedance above zero (\"50\")."));
        what.impedance = *given;
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
        const int ports = what.coupled ? 2 * int(model->extraction()->names.size()) : 2;
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
    const int ports = what.coupled ? 2 * int(model->extraction()->names.size()) : 2;
    out.insert(QStringLiteral("ports"), ports);
    out.insert(QStringLiteral("reference impedance"), what.impedance);
    out.insert(QStringLiteral("length"), what.length > 0 ? what.length : model->solvedLength());
    out.insert(QStringLiteral("bytes"), text.toUtf8().size());
    QString sweepProblem;
    const QList<double> sweep = studySweep(model->model(), &sweepProblem);
    out.insert(QStringLiteral("frequencies"), int(sweep.size()));
    if (!sweep.isEmpty()) {
        out.insert(QStringLiteral("from"), sweep.first());
        out.insert(QStringLiteral("to"), sweep.last());
    }
    out.insert(QStringLiteral("what it wrote"),
               what.coupled ? tr("Every net as coupled lines: a %1-port, the near ends first, then the far ends.").arg(ports)
                            : tr("Net %1 as a two-port, port 1 at its start.")
                                  .arg(what.net.isEmpty() ? model->extraction()->names.value(0) : what.net));
    out.insert(QStringLiteral("note"),
               tr("Touchstone 1.1, real and imaginary, one line a frequency. A schematic's S-parameter part reads it "
                  "at once. The file's own comments carry what the solve was and where the quasi-static answer stops "
                  "being the whole story."));
    return jsonResult(out);
}
