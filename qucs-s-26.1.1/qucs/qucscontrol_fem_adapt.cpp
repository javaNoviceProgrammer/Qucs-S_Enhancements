/*
 * qucscontrol_fem_adapt.cpp - Claude's adaptive mesh convergence check of a 2D
 *                             multiphysics model (fem_converge with "method":
 *                             "adaptive"): the check's node set from the call,
 *                             run as the window runs it, its result read, its
 *                             refined mesh kept
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "qucscontrol.h"
#include "qucscontrol_p.h"

#include "fem_adapt.h"
#include "multiphysicsdoc.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>

#include <cmath>

using namespace qucs_s::control;
using namespace qucs_s::fem;
using FNode = qucs_s::fem::Node;

namespace {

QJsonValue numberOrWord(double v)
{
    return std::isfinite(v) ? QJsonValue(v) : QJsonValue(std::isnan(v) ? QStringLiteral("unknown") : QStringLiteral("infinite"));
}

} // namespace

QJsonObject QucsControl::femConverged(MultiphysicsDoc* doc, const QString& tag)
{
    const FNode* node = convergenceNode(doc->model(), tag);
    if (!node) return errorResult(tr("There is no mesh convergence check %1.").arg(tag));
    const std::optional<ConvergenceResult> kept = doc->convergenceResult(node->tag);
    if (!kept) return errorResult(tr("%1 has not been run: fem_converge with \"method\": \"adaptive\" runs it.").arg(node->tag));
    const ConvergenceResult& r = *kept;
    if (!r.error.isEmpty()) return errorResult(tr("%1 could not be run: %2").arg(node->tag, r.error));
    QJsonObject o{{QStringLiteral("convergence"), node->tag},
                  {QStringLiteral("status"), r.status},
                  {QStringLiteral("converged"), r.converged},
                  {QStringLiteral("summary"), r.summary},
                  {QStringLiteral("refined by"), r.method},
                  {QStringLiteral("evidence"), QJsonArray::fromStringList(r.evidence)}};
    if (r.falseConvergence) o.insert(QStringLiteral("false agreement caught"), true);
    if (!r.points.isEmpty()) o.insert(QStringLiteral("sweep points checked"), QJsonArray::fromStringList(r.points));
    QJsonArray values;
    for (const ConvergenceResult::Final& f : r.values) {
        const QString unit = r.units.value(f.column);
        QJsonObject v{{QStringLiteral("value"), r.columns.value(f.column)},
                      {QStringLiteral("on the finest mesh"), numberOrWord(f.value)},
                      {QStringLiteral("estimated limit"), numberOrWord(f.limit)},
                      {QStringLiteral("estimated error"), numberOrWord(f.error)},
                      {QStringLiteral("estimated error %"), std::isfinite(f.relative) ? QJsonValue(std::round(f.relative * 1e8) / 1e6) : QJsonValue(QStringLiteral("unknown"))},
                      {QStringLiteral("tolerance %"), f.tolerance * 100},
                      {QStringLiteral("settled"), f.settled},
                      {QStringLiteral("trust"), f.trust},
                      {QStringLiteral("why"), f.why}};
        if (!unit.isEmpty()) v.insert(QStringLiteral("unit"), unit);
        if (std::isfinite(f.rate)) v.insert(QStringLiteral("rate"), QStringLiteral("error ~ unknowns^-%1").arg(formatNumber(f.rate, 3)));
        if (!r.points.isEmpty()) v.insert(QStringLiteral("point"), r.points.value(f.point));
        values.append(v);
    }
    o.insert(QStringLiteral("values"), values);
    QJsonArray meshes;
    for (const ConvergenceRow& row : r.rows) {
        QJsonObject m{{QStringLiteral("mesh"), row.index},
                      {QStringLiteral("kind"), row.kind},
                      {QStringLiteral("triangles"), row.triangles},
                      {QStringLiteral("unknowns"), row.unknowns},
                      {QStringLiteral("smallest element"), formatNumber(row.smallest, 4)},
                      {QStringLiteral("seconds"), std::round(row.seconds * 1000) / 1000}};
        if (!r.points.isEmpty()) m.insert(QStringLiteral("point"), r.points.value(row.point));
        QJsonObject at;
        for (int c = 0; c < r.columns.size() && c < int(row.values.size()); ++c) at.insert(r.columns.at(c), numberOrWord(row.values[std::size_t(c)]));
        m.insert(QStringLiteral("values"), at);
        meshes.append(m);
    }
    o.insert(QStringLiteral("meshes"), meshes);
    o.insert(QStringLiteral("solves"), r.solves);
    if (r.adjoints) o.insert(QStringLiteral("adjoint solves"), r.adjoints);
    o.insert(QStringLiteral("unknowns solved in all"), r.unknownsSolved);
    o.insert(QStringLiteral("largest mesh's unknowns"), r.largest);
    o.insert(QStringLiteral("seconds"), std::round(r.seconds * 100) / 100);
    if (!r.notes.isEmpty()) o.insert(QStringLiteral("notes"), QJsonArray::fromStringList(r.notes));
    if (doc->convergenceStale(node->tag)) o.insert(QStringLiteral("changed since"), tr("The model changed since this was found: run it again."));
    o.insert(QStringLiteral("mesh"), r.sizes.isEmpty() ? tr("The model's mesh is as it was; nothing was refined to keep.")
                                                       : tr("The model's mesh is as it was. fem_converge with \"action\": \"keep\" (or \"keep_mesh\": true on a run) writes the "
                                                            "refined mesh's sizes into the mesh node, an Adapted Sizes feature: built, it gives about that mesh."));
    return jsonResult(o);
}

void QucsControl::femConvergeAdaptive(MultiphysicsDoc* doc, const QJsonObject& args, const Done& given)
{
    const QString action = args.value(QLatin1String("action")).toString(QStringLiteral("run")).trimmed();
    if (action != QLatin1String("run") && action != QLatin1String("result") && action != QLatin1String("keep"))
        return given(errorResult(tr("'action' is run, result or keep, not \"%1\".").arg(action)));
    if (args.contains(QLatin1String("timeout")) && args.value(QLatin1String("timeout")).toDouble(-1) < 1)
        return given(errorResult(tr("'timeout' is how many seconds the whole check is waited for, 1 or more (600 when not given).")));
    const QString tag = args.value(QLatin1String("convergence")).toString().trimmed();
    const FNode* node = convergenceNode(doc->model(), tag);
    if (!tag.isEmpty() && !node) {
        QStringList tags;
        for (const FNode* n : convergenceNodes(doc->model())) tags << n->tag;
        return given(errorResult(tags.isEmpty() ? tr("The model has no mesh convergence check %1: leave 'convergence' out and one is made.").arg(tag)
                                                : tr("There is no mesh convergence check %1: the model has %2.").arg(tag, tags.join(QStringLiteral(", ")))));
    }
    if (action == QLatin1String("result")) {
        if (!node) return given(errorResult(tr("The model has no mesh convergence check: fem_converge with \"method\": \"adaptive\" and 'values' makes and runs one.")));
        return given(femConverged(doc, node->tag));
    }
    if (action == QLatin1String("keep")) {
        if (!node) return given(errorResult(tr("The model has no mesh convergence check.")));
        if (doc->isBusy()) return given(errorResult(tr("It is busy building or solving; fem_converge action=keep in a while.")));
        QString why;
        if (!doc->keepRefinedMesh(node->tag, &why)) return given(errorResult(why));
        return given(jsonResult(QJsonObject{{QStringLiteral("kept"), tr("The refined mesh's sizes are in the mesh node now, an Adapted Sizes feature (one step of Undo): fem_build or fem_solve builds about that mesh.")}}));
    }
    // The node's settings from the call: what is given, the rest as the node has them.
    QJsonObject props;
    if (args.contains(QLatin1String("values"))) {
        QJsonArray rows;
        for (const QJsonValue& v : args.value(QLatin1String("values")).toArray())
            if (const QString e = v.toString().trimmed(); !e.isEmpty()) rows.append(QJsonArray{e, QString(), QString()});
        if (rows.isEmpty()) return given(errorResult(tr("'values' are what is watched, as fem_evaluate takes them (es.C11, emw.neff, ec.R, a Derived Values node's lp1.Z0): give at least one.")));
        props.insert(QStringLiteral("values"), rows);
    } else if (!node) {
        return given(errorResult(tr("'values' are what is watched (es.C11, emw.neff, ec.R, lp1.Z0): the model has no check yet to take them from.")));
    }
    if (args.contains(QLatin1String("tolerance"))) {
        const double t = args.value(QLatin1String("tolerance")).toDouble(-1);
        if (!(t > 0 && t < 1)) return given(errorResult(tr("'tolerance' is the relative error asked of each value, above 0 and below 1 (0.01 is 1 %).")));
        props.insert(QStringLiteral("tolerance"), formatNumber(t, 12));
    }
    if (args.contains(QLatin1String("by"))) {
        const QString by = args.value(QLatin1String("by")).toString().trimmed();
        if (by != QLatin1String("auto") && by != QLatin1String("goal") && by != QLatin1String("uniform"))
            return given(errorResult(tr("'by' is auto, goal or uniform, not \"%1\".").arg(by)));
        props.insert(QStringLiteral("refine"), by);
    }
    if (args.contains(QLatin1String("max_unknowns"))) {
        const double n = args.value(QLatin1String("max_unknowns")).toDouble(-1);
        if (!(n >= 100 && n <= 1e9)) return given(errorResult(tr("'max_unknowns' is from 100 to 1e9.")));
        props.insert(QStringLiteral("maxunknowns"), formatNumber(n, 12));
    }
    if (args.contains(QLatin1String("max_meshes"))) {
        const int n = args.value(QLatin1String("max_meshes")).toInt(-1);
        if (n < 2 || n > 40) return given(errorResult(tr("'max_meshes' is a whole number from 2 to 40.")));
        props.insert(QStringLiteral("maxmeshes"), n);
    }
    if (args.contains(QLatin1String("check"))) props.insert(QStringLiteral("check"), args.value(QLatin1String("check")).toBool(true));
    if (args.contains(QLatin1String("sweep"))) {
        const QString sweep = args.value(QLatin1String("sweep")).toString().trimmed();
        if (!QStringList{QStringLiteral("ends"), QStringLiteral("first"), QStringLiteral("last"), QStringLiteral("all"), QStringLiteral("own")}.contains(sweep))
            return given(errorResult(tr("'sweep' is ends, first, last, all or own, not \"%1\".").arg(sweep)));
        props.insert(QStringLiteral("sweep"), sweep);
    }
    if (args.contains(QLatin1String("study"))) props.insert(QStringLiteral("study"), args.value(QLatin1String("study")).toString().trimmed());
    if (args.contains(QLatin1String("physics"))) {
        QJsonArray tags;
        for (const QJsonValue& v : args.value(QLatin1String("physics")).toArray())
            if (const QString t = v.toString().trimmed(); !t.isEmpty()) tags.append(t);
        props.insert(QStringLiteral("physics"), tags);
    }
    const bool keepAfter = args.value(QLatin1String("keep_mesh")).toBool();
    const int timeout = waitsFor(QStringLiteral("fem_optimize"), args);
    QPointer<MultiphysicsDoc> d(doc);
    auto begun = std::make_shared<int>(0);   // 1: its run begun, -1: cancelled first
    const int stop = stoppable(running(), [d, begun] {
        if (*begun == 1 && d) d->cancelJob();
        else *begun = -1;
    });
    const Done done = [this, stop, given](const QJsonObject& r) {
        stopped(stop);
        given(r);
    };
    const quint64 caller = a_callers.isEmpty() ? 0 : a_callers.last();
    const auto run = [this, d, tag, props, timeout, done, begun, keepAfter, caller] {
        if (!d) return done(errorResult(tr("The model was closed.")));
        if (*begun == -1) return done(errorResult(tr("Cancelled by the client before it began.")));
        // The node, made or set, as one step of Undo (of the caller's).
        Model m = d->model();
        QString check = tag;
        FNode* n = check.isEmpty() ? nullptr : m.find(check);
        if (!n && check.isEmpty())
            if (const FNode* first = convergenceNode(m, QString())) n = m.find(first->tag);
        bool changed = false;
        if (!n) {
            FNode made = m.make(QStringLiteral("convergence"));
            for (auto it = props.begin(); it != props.end(); ++it) made.props.insert(it.key(), it.value());
            QString why;
            n = m.add(m.root.tag, made, -1, &why);
            if (!n) return done(errorResult(why));
            changed = true;
        } else {
            for (auto it = props.begin(); it != props.end(); ++it)
                if (n->props.value(it.key()) != it.value()) {
                    n->props.insert(it.key(), it.value());
                    changed = true;
                }
        }
        check = n->tag;
        if (changed) {
            const quint64 editor = QucsDoc::editor();
            QucsDoc::setEditor(caller);
            d->setModel(m, tr("Mesh Convergence Settings"));
            QucsDoc::setEditor(editor);
        }
        *begun = 1;
        d->converge(check);
        if (!d->isBusy()) {
            // (Not begun: what cannot be run, said at once.)
            const std::optional<ConvergenceResult> r = d->convergenceResult(check);
            return done(errorResult(r && !r->error.isEmpty() ? r->error : d->lastError()));
        }
        d->whenJobEnds(timeout * 1000, [this, d, check, timeout, done, keepAfter, caller](bool ended) {
            if (!d) return done(errorResult(tr("The model was closed.")));
            if (!ended)
                return done(errorResult(tr("Still checking after %1 s, %2: it goes on (fem_describe says how far; fem_stop stops it, keeping the meshes solved; fem_converge "
                                           "with \"action\": \"result\" reads it once it has ended).")
                                            .arg(timeout)
                                            .arg(d->jobProgress())));
            const std::optional<ConvergenceResult> r = d->convergenceResult(check);
            if (r && !r->error.isEmpty()) return done(errorResult(r->error));
            if (!r) return done(errorResult(d->lastError().isEmpty() ? tr("It did not end with a result.") : d->lastError()));
            QJsonObject answer = femConverged(d, check);
            if (keepAfter) {
                QString why;
                const quint64 editor = QucsDoc::editor();
                QucsDoc::setEditor(caller);
                const bool ok = d->keepRefinedMesh(check, &why);
                QucsDoc::setEditor(editor);
                QJsonObject o = QJsonDocument::fromJson(answer.value(QStringLiteral("content")).toArray().first().toObject().value(QStringLiteral("text")).toString().toUtf8()).object();
                o.insert(QStringLiteral("mesh"), ok ? tr("The refined mesh's sizes were written into the mesh node (Adapted Sizes, one step of Undo).") : tr("Not kept: %1").arg(why));
                answer = jsonResult(o);
            }
            done(answer);
        });
    };
    if (!doc->isBusy()) return run();
    const quint64 stops = doc->stops();
    doc->whenJobEnds(1000, [this, d, done, run, stops](bool ended) {
        if (!d) return done(errorResult(tr("The model was closed.")));
        if (!ended) return done(errorResult(tr("It is busy building or solving; fem_converge again in a while.")));
        if (d->stops() != stops) return done(errorResult(tr("Not run: the work it waited behind was stopped (fem_stop, Cancel), and a stop stops what waits for it too; fem_converge again to run it.")));
        run();
    });
}
