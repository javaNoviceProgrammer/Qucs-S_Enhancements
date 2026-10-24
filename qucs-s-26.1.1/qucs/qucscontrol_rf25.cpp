/*
 * qucscontrol_rf25.cpp - Claude's tools for an RF 2.5D model: rf25_solve,
 *                        the capacitance of its conductors and each net
 *                        as a line
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
#include "qucs.h"
#include "rf25doc.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>

using namespace qucs_s::rf25;
using namespace qucs_s::control;

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
        const QString text = args.value(QLatin1String("length")).toString().trimmed();
        // Read as the model's own expressions are, so that "800[um]" and
        // a parameter's name both work.
        qucs_s::fem::Scope scope(&qucs_s::fem::Scope::builtins());
        for (const qucs_s::fem::Model::Parameter& param : model->model().parameters()) {
            const qucs_s::fem::Expression p = qucs_s::fem::Expression::compile(param.expression, scope);
            if (p.isValid() && p.isConstant()) scope.setConstant(param.name, p.constant(), p.dim());
        }
        const qucs_s::fem::Expression given = qucs_s::fem::Expression::compile(text, scope);
        if (!given.isValid() || !given.isConstant() || !(given.constant() > 0))
            return errorResult(given.isValid() ? tr("'length' is a length above zero (\"800[um]\"); %1 is %2.")
                                                     .arg(text, given.isConstant() ? tr("not above zero") : tr("not a constant"))
                                               : tr("'length' is a length above zero (\"800[um]\"): %1").arg(given.error()));
        length = given.constant();
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
        const LineParameters lp = lineParameters(*e, i, length);
        if (lp.ok()) {
            QJsonObject line;
            line.insert(QStringLiteral("C"), lp.c);
            line.insert(QStringLiteral("C0"), lp.c0);
            line.insert(QStringLiteral("epsilon_eff"), lp.epsilonEff);
            line.insert(QStringLiteral("Z0"), lp.z0);
            line.insert(QStringLiteral("L"), lp.l);
            line.insert(QStringLiteral("v"), lp.velocity);
            line.insert(QStringLiteral("n_eff"), lp.nEff);
            net.insert(QStringLiteral("line"), line);
        } else if (!lp.problems.isEmpty()) {
            net.insert(QStringLiteral("line not given"), lp.problems.join(QStringLiteral("; ")));
        }
        nets.append(net);
    }
    out.insert(QStringLiteral("nets"), nets);
    out.insert(QStringLiteral("unknowns"), e->unknowns);
    out.insert(QStringLiteral("length"), length);
    QJsonArray said;
    for (const QString& line : model->preparedLines()) said.append(line);
    for (const QString& line : e->lines) said.append(line);
    out.insert(QStringLiteral("what it did"), said);
    out.insert(QStringLiteral("note"),
               tr("The electrostatics alone: C and, with every dielectric vacuum, C0, and a uniform line's eps_eff, Z0 "
                  "and L from the two. No resistance, no inductance of a loop and no S-parameters in this build."));
    return jsonResult(out);
}
