/*
 * qucscontrol_router.cpp - Claude's tools for the layout viewer's metal
 *                          router (layoutrouting.h): route_setup sets a
 *                          layout's job (technology, nets, keep-outs,
 *                          guides) and reads its terminals, route_run
 *                          routes in the background, route_report tells
 *                          of it, route_commit writes the routes as a cell
 *                          of their own, route_stop stops a run and
 *                          route_assign_pads pairs starts with pads. With
 *                          or without a window (--mcp-server).
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

#include "layoutdoc.h"
#include "layoutrouter/router_gds.h"
#include "layoutrouting.h"
#include "markup.h"
#include "qucs.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QPointer>
#include <QTimer>

#include <cmath>
#include <memory>

using namespace qucs_s::control;
using namespace qucs_s::router;
using qucs_s::routing::Session;

namespace {

double um3(double v)
{
    return std::round(v * 1000) / 1000;
}

QJsonArray pointJson(const Problem& p, Pt q)
{
    return {um3(p.um(q.x)), um3(p.um(q.y))};
}

QString statusName(NetResult::Status s)
{
    switch (s) {
    case NetResult::Status::Routed: return QStringLiteral("routed");
    case NetResult::Status::Failed: return QStringLiteral("failed");
    case NetResult::Status::Fixed: return QStringLiteral("fixed");
    case NetResult::Status::Kept: return QStringLiteral("kept");
    default: return QStringLiteral("not run");
    }
}

QJsonObject passJson(const PassRow& r)
{
    QJsonObject o{{"pass", r.pass}, {"routed", r.routed}, {"failed", r.failed}, {"contested", r.contested},
                  {"length µm", um3(r.length)}, {"seconds", um3(r.seconds)}};
    if (r.cleanup) o.insert(QStringLiteral("clean-up"), true);
    if (r.partial) o.insert(QStringLiteral("partial"), true);   // stopped part way: not every net routed again
    return o;
}

QJsonObject findingJson(const Problem& p, const Finding& f)
{
    QJsonObject o{{"kind", f.kind}, {"net", f.net}, {"at", pointJson(p, f.at)}, {"text", f.text}};
    if (!f.other.isEmpty()) o.insert(QStringLiteral("other"), f.other);
    if (f.metal >= 0 && f.metal < p.tech.metals.size()) o.insert(QStringLiteral("metal"), p.tech.metals.at(f.metal).name);
    if (f.required > 0) {
        o.insert(QStringLiteral("measured µm"), um3(f.measured));
        o.insert(QStringLiteral("required µm"), um3(f.required));
    }
    if (!f.box.empty()) o.insert(QStringLiteral("box"), QJsonArray{um3(p.um(f.box.x0)), um3(p.um(f.box.y0)), um3(p.um(f.box.x1)), um3(p.um(f.box.y1))});
    return o;
}

QJsonArray terminalsJson(const Problem& p, const QVector<TerminalReading>& readings, int most)
{
    QJsonArray a;
    for (const TerminalReading& t : readings) {
        if (a.size() >= most) break;
        QJsonArray metals;
        for (int m : t.metals) metals.append(p.tech.metals.at(m).name);
        QJsonObject o{{"net", t.net}, {"terminal", t.index == 0 ? QStringLiteral("start") : QStringLiteral("pad %1").arg(t.index)},
                      {"ends at", pointJson(p, t.centre)}, {"metals", metals}};
        if (!t.shape.empty()) {
            o.insert(QStringLiteral("shape"), QJsonArray{um3(p.um(t.shape.x0)), um3(p.um(t.shape.y0)), um3(p.um(t.shape.x1)), um3(p.um(t.shape.y1))});
        } else {
            o.insert(QStringLiteral("shape"), QStringLiteral("none: the line ends at the point"));
        }
        if (t.joined > 0) o.insert(QStringLiteral("joined shapes"), t.joined);
        if (!t.note.isEmpty()) o.insert(QStringLiteral("warning"), t.note);
        a.append(o);
    }
    return a;
}

QJsonArray crowdingJson(const Problem& p, const QVector<Crowding>& crowding)
{
    QJsonArray a;
    for (const Crowding& c : crowding)
        a.append(QJsonObject{{"box", QJsonArray{um3(p.um(c.where.x0)), um3(p.um(c.where.y0)), um3(p.um(c.where.x1)), um3(p.um(c.where.y1))}},
                             {"tracks wanted %", qRound(c.use * 100)}});
    return a;
}

// How the session stands: its job, its run, its result and check.
QJsonObject summaryOf(const Session* s, int mostFindings)
{
    QJsonObject o{{"layout", QDir::toNativeSeparators(s->file())}, {"job file", QDir::toNativeSeparators(s->jobFile())},
                  {"nets", s->job.nets.size()}, {"keep-outs", s->job.keepOuts.size()}, {"route cell", s->job.routeCell}};
    if (s->isRunning()) {
        const QVector<PassRow> passes = s->passesSoFar();
        QJsonArray rows;
        for (int i = std::max(0, int(passes.size()) - 5); i < passes.size(); ++i) rows.append(passJson(passes.at(i)));
        o.insert(QStringLiteral("state"), QStringLiteral("running"));
        o.insert(QStringLiteral("mode"), s->runMode());
        o.insert(QStringLiteral("seconds so far"), um3(s->startedAt().msecsTo(QDateTime::currentDateTime()) / 1000.0));
        o.insert(QStringLiteral("passes so far"), rows);
        o.insert(QStringLiteral("note"), QObject::tr("route_report says how it goes on; route_stop stops it with the best pass so far"));
        return o;
    }
    if (!s->hasResult()) {
        o.insert(QStringLiteral("state"), QStringLiteral("not run"));
        return o;
    }
    const Result& r = s->result();
    const Verification& v = s->verification();
    const Problem& p = *s->resultProblem();
    int vias = 0, kept = 0, notRun = 0;
    double length = 0;
    for (const NetResult& n : r.nets) {
        vias += n.vias.size();
        length += p.um(n.length);
        kept += n.status == NetResult::Status::Kept;
        notRun += n.status == NetResult::Status::NotRun;
    }
    o.insert(QStringLiteral("state"), s->resultIsOld() ? QStringLiteral("done, but the job has changed since") : QStringLiteral("done"));
    o.insert(QStringLiteral("mode"), s->runMode());
    o.insert(QStringLiteral("stopped"), r.stopped);
    o.insert(QStringLiteral("routed"), r.routed());
    o.insert(QStringLiteral("failed"), r.failed());
    if (kept) o.insert(QStringLiteral("kept from the last run"), kept);
    if (notRun) o.insert(QStringLiteral("not run"), notRun);
    o.insert(QStringLiteral("vias"), vias);
    o.insert(QStringLiteral("length µm"), um3(length));
    // clean: every net routed and no finding - the field a script tests.
    o.insert(QStringLiteral("clean"), v.clean());
    o.insert(QStringLiteral("findings count"), v.findings.size());
    QJsonArray findings;
    for (const Finding& f : v.findings) {
        if (findings.size() >= mostFindings) break;
        findings.append(findingJson(p, f));
    }
    o.insert(QStringLiteral("findings"), findings);
    if (findings.size() < v.findings.size())
        o.insert(QStringLiteral("findings shown"), QObject::tr("the first %1 of %2: route_report what=findings lists them all").arg(findings.size()).arg(v.findings.size()));
    QJsonArray passes;
    for (int i = std::max(0, int(r.passes.size()) - 3); i < r.passes.size(); ++i) passes.append(passJson(r.passes.at(i)));
    o.insert(QStringLiteral("last passes"), passes);
    o.insert(QStringLiteral("passes"), r.passes.size());
    o.insert(QStringLiteral("seconds"), um3(r.seconds));
    o.insert(QStringLiteral("check seconds"), um3(v.seconds));
    o.insert(QStringLiteral("router memory MB"), qRound64(double(r.peakBytes) / 1048576.0));
    o.insert(QStringLiteral("cells searched"), double(r.statesSearched));
    QStringList notes = r.notes + v.notes;
    if (!notes.isEmpty()) o.insert(QStringLiteral("notes"), QJsonArray::fromStringList(notes.mid(0, 20)));
    return o;
}

// The layout file a call names: 'path', or the layout in front.
QString layoutFileOf(QucsApp* app, const QJsonObject& args, QString* error)
{
    QString file = args.value(QLatin1String("path")).toString().trimmed();
    if (file.isEmpty()) {
        if (auto* doc = qobject_cast<LayoutDoc*>(app && app->DocumentTab->count() > 0 ? app->DocumentTab->currentWidget() : nullptr))
            file = doc->getDocName();
        if (file.isEmpty()) {
            *error = QObject::tr("Which layout? 'path' (the document in front is none).");
            return {};
        }
    }
    file = absolute(file);
    // A tab's title names its file too.
    if (!QFileInfo(file).isFile() && app)
        for (QucsDoc* d : app->allDocuments())
            if (auto* l = dynamic_cast<LayoutDoc*>(d); l != nullptr && QFileInfo(l->getDocName()).fileName() == args.value(QLatin1String("path")).toString().trimmed())
                file = l->getDocName();
    return QFileInfo(file).absoluteFilePath();
}

LayoutDoc* tabOf(QucsApp* app, const QString& file)
{
    if (!app) return nullptr;
    for (QucsDoc* d : app->allDocuments())
        if (auto* l = dynamic_cast<LayoutDoc*>(d); l != nullptr && sameFile(l->getDocName(), file)) return l;
    return nullptr;
}

// A pen stroke of the cell a tab shows, by its number there (from 1, as
// read_drawings counts them).
bool strokeOf(LayoutDoc* doc, int number, qucs_s::markup::Stroke* out, QString* error)
{
    if (!doc) {
        *error = QObject::tr("'from_stroke' reads the pen's strokes of the layout's open tab, and it has none");
        return false;
    }
    const QList<qucs_s::markup::Stroke> strokes = doc->view()->drawing().on(doc->view()->drawingPart());
    if (number < 1 || number > strokes.size()) {
        *error = QObject::tr("The cell shown has %1 stroke(s): 'from_stroke' %2 is none of them (read_drawings numbers them)").arg(strokes.size()).arg(number);
        return false;
    }
    *out = strokes.at(number - 1);
    return true;
}

} // namespace

// ----------------------------------------------------------------------
// route_setup

void QucsControl::routeSetup(const QJsonObject& args, const Done& done)
{
    // (Read through a copy in the lambda: what it reads is route_setup's, not withLayout's.)
    const QJsonObject call = args;
    withLayout(args, false, [this, call, done](std::shared_ptr<const qucs_s::layout::Layout> layout, LayoutDoc* doc) {
        Session* s = Session::of(layout->file);
        if (s->isRunning()) {
            done(errorResult(tr("A run of this layout is going on: route_stop stops it, or route_report waits for it.")));
            return;
        }
        Job job = call.value(QLatin1String("clear")).toBool() ? Job() : s->job;
        const QDir folder = QFileInfo(layout->file).absoluteDir();
        QString error;
        const QJsonValue technology = call.value(QLatin1String("technology"));
        if (technology.isString()) {
            job.technologyFile = technology.toString();
            job.technology = {};
        } else if (technology.isObject()) {
            job.technology = technology.toObject();
            job.technologyFile.clear();
        }
        if (call.contains(QLatin1String("cell"))) job.cell = call.value(QLatin1String("cell")).toString();
        if (call.contains(QLatin1String("route_cell"))) {
            const QString c = call.value(QLatin1String("route_cell")).toString().trimmed();
            if (c.isEmpty() || c.size() > 120 || c.contains(QRegularExpression(QStringLiteral("[^A-Za-z0-9_$?.-]")))) {
                done(errorResult(tr("'route_cell' is a cell name of letters, digits and _$?.-")));
                return;
            }
            job.routeCell = c;
        }
        const auto putNet = [&](const JobNet& n) {
            const int i = job.netIndex(n.name);
            if (i >= 0) job.nets[i] = n;
            else job.nets.append(n);
        };
        if (call.value(QLatin1String("replace_nets")).toBool()) job.nets.clear();
        for (const QJsonValue& v : call.value(QLatin1String("nets")).toArray()) {
            JobNet n;
            if (!readNet(v, &n, &error)) {
                done(errorResult(error));
                return;
            }
            putNet(n);
        }
        if (call.contains(QLatin1String("nets_csv"))) {
            QFile f(folder.absoluteFilePath(call.value(QLatin1String("nets_csv")).toString()));
            if (!f.open(QIODevice::ReadOnly) || f.size() > 64 * 1024 * 1024) {
                done(errorResult(tr("The CSV %1 cannot be read.").arg(QDir::toNativeSeparators(f.fileName()))));
                return;
            }
            const QVector<JobNet> nets = readNetsCsv(QString::fromUtf8(f.readAll()), &error);
            if (!error.isEmpty()) {
                done(errorResult(QFileInfo(f.fileName()).fileName() + QStringLiteral(": ") + error));
                return;
            }
            for (const JobNet& n : nets) putNet(n);
        }
        for (const QJsonValue& v : call.value(QLatin1String("remove_nets")).toArray()) {
            const int i = job.netIndex(v.toString());
            if (i < 0) {
                done(errorResult(tr("There is no net %1 to remove.").arg(v.toString())));
                return;
            }
            job.nets.removeAt(i);
        }
        for (const QJsonValue& v : call.value(QLatin1String("remove_keep_outs")).toArray()) {
            const qsizetype before = job.keepOuts.size();
            job.keepOuts.removeIf([&](const JobKeepOut& k) { return k.name == v.toString() || v.toString() == QLatin1String("*"); });
            if (job.keepOuts.size() == before) {
                done(errorResult(tr("There is no keep-out %1 to remove.").arg(v.toString())));
                return;
            }
        }
        for (const QJsonValue& v : call.value(QLatin1String("keep_outs")).toArray()) {
            QJsonObject o = v.toObject();
            if (o.contains(QLatin1String("from_stroke"))) {
                qucs_s::markup::Stroke stroke;
                if (!strokeOf(doc, o.value(QLatin1String("from_stroke")).toInt(), &stroke, &error)) {
                    done(errorResult(error));
                    return;
                }
                QJsonArray outline;
                for (const QPointF& q : qucs_s::routing::strokeOutline(stroke, o.value(QLatin1String("box")).toBool()))
                    outline.append(QJsonArray{um3(q.x()), um3(q.y())});
                o.remove(QStringLiteral("from_stroke"));
                o.remove(QStringLiteral("box"));
                o.insert(QStringLiteral("outline"), outline);
                if (!o.contains(QLatin1String("name"))) o.insert(QStringLiteral("name"), QStringLiteral("stroke %1").arg(v.toObject().value(QLatin1String("from_stroke")).toInt()));
            }
            JobKeepOut k;
            if (!readKeepOut(o, &k, &error)) {
                done(errorResult(error));
                return;
            }
            if (k.name.isEmpty()) k.name = QStringLiteral("keep-out %1").arg(job.keepOuts.size() + 1);
            job.keepOuts.removeIf([&](const JobKeepOut& x) { return x.name == k.name; });   // the same name: replaced
            job.keepOuts.append(k);
        }
        for (const QJsonValue& v : call.value(QLatin1String("guides")).toArray()) {
            const QJsonObject o = v.toObject();
            for (auto it = o.begin(); it != o.end(); ++it)
                if (it.key() != QLatin1String("net") && it.key() != QLatin1String("from_stroke") && it.key() != QLatin1String("points") && it.key() != QLatin1String("band")) {
                    done(errorResult(tr("A guide has no '%1' (it takes net, from_stroke or points, band).").arg(it.key())));
                    return;
                }
            const int i = job.netIndex(o.value(QLatin1String("net")).toString());
            if (i < 0) {
                done(errorResult(tr("A guide names the net %1, which the job has not.").arg(o.value(QLatin1String("net")).toString())));
                return;
            }
            QVector<QPointF> guide;
            if (o.contains(QLatin1String("from_stroke"))) {
                qucs_s::markup::Stroke stroke;
                if (!strokeOf(doc, o.value(QLatin1String("from_stroke")).toInt(), &stroke, &error)) {
                    done(errorResult(error));
                    return;
                }
                guide = qucs_s::routing::strokeGuide(stroke);
            } else {
                for (const QJsonValue& q : o.value(QLatin1String("points")).toArray()) {
                    const QJsonArray xy = q.toArray();
                    if (xy.size() != 2 || !xy.at(0).isDouble() || !xy.at(1).isDouble()) {
                        done(errorResult(tr("A guide's 'points' are [x, y] in µm.")));
                        return;
                    }
                    guide.append({xy.at(0).toDouble(), xy.at(1).toDouble()});
                }
            }
            if (guide.size() == 1) {
                done(errorResult(tr("A guide needs two points at least (none: no guide).")));
                return;
            }
            job.nets[i].guide = guide;
            if (o.contains(QLatin1String("band"))) job.nets[i].band = std::max(0.0, o.value(QLatin1String("band")).toDouble());
        }
        if (call.contains(QLatin1String("options")) && !readOptions(call.value(QLatin1String("options")), &job.options, &error)) {
            done(errorResult(error));
            return;
        }
        // The technology must read before anything is kept.
        const Job before = s->job;
        s->job = job;
        Technology t;
        if (!s->technology(&t, &error)) {
            s->job = before;
            done(errorResult(error));
            return;
        }
        s->jobChanged();
        QString saveError;
        const bool saved = s->saveJob(&saveError);
        QJsonObject o{{"layout", QDir::toNativeSeparators(layout->file)}, {"technology", t.name}, {"grid µm", t.grid},
                      {"nets", job.nets.size()}, {"keep-outs", job.keepOuts.size()}, {"route cell", job.routeCell}};
        QJsonArray metals, classes;
        for (const Metal& m : t.metals)
            metals.append(QStringLiteral("%1 %2%3").arg(m.name, m.layer.text(), m.direction ? QStringLiteral(" (%1)").arg(QLatin1Char(m.direction)) : QString()));
        for (const NetClass& c : t.classes) classes.append(QStringLiteral("%1 %2 µm, %3 µm apart").arg(c.name).arg(c.width).arg(std::max(c.spacing, t.sameLayer)));
        o.insert(QStringLiteral("metals"), metals);
        o.insert(QStringLiteral("classes"), classes);
        o.insert(QStringLiteral("spacing"), QStringLiteral("line to line %1 µm; to metal of no net, or of a net that is not the line's own, %2 µm; to the shape "
                                                          "another net's terminal is on (its pad or contact) %3 µm")
                                                .arg(t.sameLayer)
                                                .arg(t.toMetal)
                                                .arg(t.toTerminal));
        o.insert(QStringLiteral("job file"), saved ? QDir::toNativeSeparators(s->jobFile()) : tr("not written: %1").arg(saveError));
        if (job.nets.isEmpty()) {
            o.insert(QStringLiteral("note"), tr("No nets yet: 'nets' (or 'nets_csv') adds them."));
            done(jsonResult(o));
            return;
        }
        if (!s->prepare(layout, &error)) {
            o.insert(QStringLiteral("problem"), error);
            done(jsonResult(o));
            return;
        }
        const Router& router = *s->prepared();
        const Problem& p = router.problem();
        o.insert(QStringLiteral("cell"), job.cell.isEmpty() ? layout->cells.at(layout->mainCell()).name : job.cell);
        o.insert(QStringLiteral("shapes read"), QJsonObject{{"metal", p.metal.size()}, {"cuts", p.cuts.size()}, {"obstacles", p.obstacles.size()}});
        o.insert(QStringLiteral("terminals"), terminalsJson(p, router.terminals(), call.value(QLatin1String("max")).toInt(60)));
        int warned = 0;
        for (const TerminalReading& r : router.terminals()) warned += !r.note.isEmpty();
        if (warned) o.insert(QStringLiteral("terminals with a warning"), warned);
        if (!router.crowding().isEmpty()) o.insert(QStringLiteral("crowded"), crowdingJson(p, router.crowding()));
        o.insert(QStringLiteral("memory a run needs MB"), qRound64(double(router.estimatedBytes()) / 1048576.0));
        o.insert(QStringLiteral("threads"), p.options.threads);
        if (!s->warnings().isEmpty()) o.insert(QStringLiteral("warnings"), QJsonArray::fromStringList(s->warnings()));
        if (doc) doc->showRouting();
        done(jsonResult(o));
    }, done);
}

// ----------------------------------------------------------------------
// route_run

void QucsControl::routeRun(const QJsonObject& args, const Done& done)
{
    QString error;
    const QString file = layoutFileOf(a_app, args, &error);
    if (file.isEmpty()) {
        done(errorResult(error));
        return;
    }
    Session* s = Session::of(file, false);
    if (!s || s->job.nets.isEmpty()) {
        done(errorResult(tr("Nothing is set up to route in %1: route_setup first.").arg(QFileInfo(file).fileName())));
        return;
    }
    const QString mode = args.value(QLatin1String("mode")).toString(s->hasResult() ? QStringLiteral("changed") : QStringLiteral("all"));
    if (mode != QLatin1String("all") && mode != QLatin1String("changed") && mode != QLatin1String("selected")) {
        done(errorResult(tr("'mode' is all, changed or selected.")));
        return;
    }
    QStringList which;
    for (const QJsonValue& v : args.value(QLatin1String("nets")).toArray()) which << v.toString();
    if (mode == QLatin1String("selected") && which.isEmpty()) {
        done(errorResult(tr("'mode' selected routes the nets 'nets' names: none named.")));
        return;
    }
    const int timeout = waitsFor(QStringLiteral("route_run"), args);
    const auto go = [this, s, which, mode, timeout, done, file](std::shared_ptr<const qucs_s::layout::Layout> layout) {
        QString why;
        if ((!s->prepared() || s->layout() != layout) && !s->prepare(layout, &why)) {
            done(errorResult(why));
            return;
        }
        if (!s->start(mode == QLatin1String("selected") ? which : QStringList(), mode == QLatin1String("changed"), &why)) {
            done(errorResult(why));
            return;
        }
        if (LayoutDoc* doc = tabOf(a_app, file)) doc->showRouting();
        auto answered = std::make_shared<bool>(false);
        auto connection = std::make_shared<QMetaObject::Connection>();
        QPointer<Session> session = s;
        *connection = connect(s, &Session::runFinished, this, [session, answered, connection, done] {
            QObject::disconnect(*connection);
            if (*answered || !session) return;
            *answered = true;
            done(jsonResult(summaryOf(session, 25)));
        });
        QTimer::singleShot(timeout * 1000, this, [session, answered, connection, done] {
            if (*answered || !session) return;
            *answered = true;
            QObject::disconnect(*connection);
            done(jsonResult(summaryOf(session, 25)));
        });
    };
    withLayout(args, false, [go](std::shared_ptr<const qucs_s::layout::Layout> layout, LayoutDoc*) { go(layout); }, done);
}

// ----------------------------------------------------------------------
// route_report

QJsonObject QucsControl::routeReport(const QJsonObject& args)
{
    QString error;
    const QString file = layoutFileOf(a_app, args, &error);
    if (file.isEmpty()) return errorResult(error);
    Session* s = Session::of(file, false);
    if (!s) return errorResult(tr("Nothing is set up to route in %1: route_setup first.").arg(QFileInfo(file).fileName()));
    const QString what = args.value(QLatin1String("what")).toString(QStringLiteral("summary"));
    const int most = std::clamp(args.value(QLatin1String("max")).toInt(50), 1, 5000);
    if (what == QLatin1String("summary")) return jsonResult(summaryOf(s, std::min(most, 25)));
    if (what == QLatin1String("passes")) {
        const QVector<PassRow> rows = s->isRunning() ? s->passesSoFar() : (s->hasResult() ? s->result().passes : QVector<PassRow>());
        QJsonArray a;
        for (int i = std::max(0, int(rows.size()) - most); i < rows.size(); ++i) a.append(passJson(rows.at(i)));
        return jsonResult(QJsonObject{{"state", s->isRunning() ? QStringLiteral("running") : (s->hasResult() ? QStringLiteral("done") : QStringLiteral("not run"))},
                                      {"passes", a}});
    }
    if (what == QLatin1String("terminals") || what == QLatin1String("crowding")) {
        if (!s->prepared()) {
            if (!s->layout()) return errorResult(tr("route_setup reads the terminals; it has not been called since Qucs-S started."));
            if (!s->prepare(s->layout(), &error)) return errorResult(error);
        }
        const Router& r = *s->prepared();
        if (what == QLatin1String("crowding")) return jsonResult(QJsonObject{{"crowded", crowdingJson(r.problem(), r.crowding())}});
        return jsonResult(QJsonObject{{"terminals", terminalsJson(r.problem(), r.terminals(), most)}});
    }
    if (s->isRunning()) return errorResult(tr("A run is going on: what 'summary' or 'passes' say how far it is."));
    if (!s->hasResult()) return errorResult(tr("Nothing has been routed yet (route_run)."));
    const Problem& p = *s->resultProblem();
    const Result& r = s->result();
    if (what == QLatin1String("findings")) {
        QJsonArray a;
        for (const Finding& f : s->verification().findings) {
            if (a.size() >= most) break;
            a.append(findingJson(p, f));
        }
        return jsonResult(QJsonObject{{"clean", s->verification().clean()}, {"findings count", s->verification().findings.size()}, {"findings", a}});
    }
    if (what == QLatin1String("nets")) {
        QJsonArray a;
        for (int n = 0; n < r.nets.size() && a.size() < most; ++n) {
            const NetResult& x = r.nets.at(n);
            QJsonObject o{{"net", p.nets.at(n).name}, {"status", statusName(x.status)}, {"class", p.tech.classes.at(p.nets.at(n).netClass).name}};
            if (x.status == NetResult::Status::Routed || x.status == NetResult::Status::Kept) {
                o.insert(QStringLiteral("length µm"), um3(p.um(x.length)));
                o.insert(QStringLiteral("vias"), x.vias.size());
            }
            if (!x.reason.isEmpty()) o.insert(QStringLiteral("reason"), x.reason);
            a.append(o);
        }
        return jsonResult(QJsonObject{{"nets", a}});
    }
    if (what == QLatin1String("net")) {
        const QString name = args.value(QLatin1String("net")).toString();
        const int n = s->resultNet(name);
        if (n < 0) return errorResult(tr("The last run has no net %1.").arg(name));
        const NetResult& x = r.nets.at(n);
        QJsonArray runs, vias;
        for (const Run& run : x.runs) {
            if (runs.size() >= most) break;
            runs.append(QJsonArray{p.tech.metals.at(run.metal).name, um3(p.um(run.a.x)), um3(p.um(run.a.y)), um3(p.um(run.b.x)), um3(p.um(run.b.y))});
        }
        for (const ViaPlaced& v : x.vias) vias.append(QJsonArray{p.tech.vias.at(v.via).name, um3(p.um(v.at.x)), um3(p.um(v.at.y))});
        QJsonObject o{{"net", name}, {"status", statusName(x.status)}, {"width µm", p.tech.classes.at(p.nets.at(n).netClass).width},
                      {"runs (metal, x0, y0, x1, y1)", runs}, {"vias (via, x, y)", vias}, {"length µm", um3(p.um(x.length))}};
        if (!x.reason.isEmpty()) o.insert(QStringLiteral("reason"), x.reason);
        if (x.hasWhere) o.insert(QStringLiteral("where"), pointJson(p, x.where));
        QJsonArray findings;
        for (const Finding& f : s->verification().findings)
            if (f.net == name || f.other == name) findings.append(findingJson(p, f));
        o.insert(QStringLiteral("findings"), findings);
        return jsonResult(o);
    }
    return errorResult(tr("'what' is summary, passes, nets, net, findings, terminals or crowding."));
}

// ----------------------------------------------------------------------
// route_stop

QJsonObject QucsControl::routeStop(const QJsonObject& args)
{
    QString error;
    const QString file = layoutFileOf(a_app, args, &error);
    if (file.isEmpty()) return errorResult(error);
    Session* s = Session::of(file, false);
    if (!s || !s->isRunning()) return jsonResult(QJsonObject{{"ended", false}, {"note", tr("Nothing is being routed in %1.").arg(QFileInfo(file).fileName())}});
    s->cancel();
    const bool ended = s->wait(std::clamp(args.value(QLatin1String("timeout")).toInt(30), 1, 600) * 1000);
    QJsonObject o = summaryOf(s, 10);
    o.insert(QStringLiteral("ended"), ended);   // ('stopped' stays why the run ended)
    return jsonResult(o);
}

// ----------------------------------------------------------------------
// route_commit

QJsonObject QucsControl::routeCommit(const QJsonObject& args)
{
    QString error;
    const QString file = layoutFileOf(a_app, args, &error);
    if (file.isEmpty()) return errorResult(error);
    Session* s = Session::of(file, false);
    if (!s || !s->hasResult()) return errorResult(tr("Nothing has been routed in %1 yet (route_run).").arg(QFileInfo(file).fileName()));
    if (s->isRunning()) return errorResult(tr("A run is going on: route_report waits for it, route_stop stops it."));
    const bool remove = args.value(QLatin1String("remove")).toBool();
    const Verification& v = s->verification();
    if (!remove && !v.clean() && !args.value(QLatin1String("allow_findings")).toBool()) {
        QMap<QString, int> kinds;
        for (const Finding& f : v.findings) ++kinds[f.kind];
        QStringList said;
        for (auto it = kinds.cbegin(); it != kinds.cend(); ++it) said << QStringLiteral("%1 %2").arg(it.value()).arg(it.key());
        return errorResult(tr("The routes have %1 finding(s) (%2): route_report what=findings lists them. 'allow_findings' commits the nets routed anyway.")
                               .arg(v.findings.size())
                               .arg(said.join(QStringLiteral(", "))));
    }
    if (s->resultIsOld() && !args.value(QLatin1String("allow_findings")).toBool() && !remove)
        return errorResult(tr("The job has changed since the routes were made: route_run again, or 'allow_findings' to commit them as they are."));
    Commit c;
    c.source = file;
    const QString saveAs = args.value(QLatin1String("save_as")).toString().trimmed();
    if (!saveAs.isEmpty()) c.target = QFileInfo(file).absoluteDir().absoluteFilePath(saveAs);
    else if (args.value(QLatin1String("in_place")).toBool()) c.target = file;
    else return errorResult(tr("Where to? 'save_as' (a .gds or .oas beside the layout or anywhere) or 'in_place' true (the layout itself is written)."));
    c.topCell = s->job.cell;
    c.cell = args.value(QLatin1String("cell")).toString(s->job.routeCell);
    for (const QJsonValue& n : args.value(QLatin1String("nets")).toArray()) c.nets << n.toString();
    c.remove = remove;
    const CommitReport report = commitRoutes(c, *s->resultProblem(), s->result());
    if (!report.ok) return errorResult(report.error);
    // Read back: the route cell, as a viewer reads it.
    QJsonObject o{{"file", QDir::toNativeSeparators(c.target)}, {"cell", c.cell}, {"removed", remove}, {"nets", report.nets},
                  {"polygons", report.polygons}, {"replaced its earlier contents", report.replaced}, {"placed in the top cell", report.placed}};
    QString readError;
    const std::shared_ptr<qucs_s::layout::Layout> back = qucs_s::layout::read(c.target, &readError);
    if (!back) {
        o.insert(QStringLiteral("read back"), tr("could not be read: %1").arg(readError));
    } else {
        const int cell = back->cellIndex(c.cell);
        o.insert(QStringLiteral("read back"), remove ? (cell < 0 ? tr("the cell is gone") : tr("the cell is still there"))
                                                     : (cell < 0 ? tr("the cell is missing") : tr("%1 shapes in the cell").arg(back->cells.at(cell).shapes.size())));
    }
    if (!saveAs.isEmpty() && saveAs != QFileInfo(file).fileName()) {
        // The job goes with the copy, so a session opened on it has the nets.
        Session* copy = Session::of(c.target);
        copy->job = s->job;
        copy->jobChanged();
        QString e;
        copy->saveJob(&e);
    }
    if (LayoutDoc* doc = tabOf(a_app, c.target)) doc->reload();
    return jsonResult(o);
}

// ----------------------------------------------------------------------
// route_assign_pads

QJsonObject QucsControl::routeAssignPads(const QJsonObject& args)
{
    QString error;
    const QString file = layoutFileOf(a_app, args, &error);
    if (file.isEmpty()) return errorResult(error);
    Session* s = Session::of(file, false);
    if (!s || s->job.nets.isEmpty()) return errorResult(tr("Nothing is set up to route in %1: route_setup first.").arg(QFileInfo(file).fileName()));
    if (!s->prepared()) {
        if (!s->layout()) return errorResult(tr("route_setup reads the layout first."));
        if (!s->prepare(s->layout(), &error)) return errorResult(error);
    }
    const Problem& p = s->prepared()->problem();
    QStringList names;
    for (const QJsonValue& v : args.value(QLatin1String("nets")).toArray()) names << v.toString();
    if (names.isEmpty())
        for (const Net& n : p.nets)
            if (n.terminals.size() == 2 && !n.fixed) names << n.name;
    QVector<int> nets;
    for (const QString& n : names) {
        int i = -1;
        for (int k = 0; k < p.nets.size(); ++k)
            if (p.nets.at(k).name == n) i = k;
        if (i < 0) return errorResult(tr("There is no net %1.").arg(n));
        if (p.nets.at(i).terminals.size() != 2) return errorResult(tr("Net %1 has more than one pad: pairing is for nets of one pad.").arg(n));
        nets << i;
    }
    QVector<Pt> starts, pads;
    for (int i : nets) starts << p.nets.at(i).terminals.at(0).at;
    if (args.contains(QLatin1String("pads"))) {
        for (const QJsonValue& v : args.value(QLatin1String("pads")).toArray()) {
            const QJsonArray xy = v.toArray();
            if (xy.size() != 2 || !xy.at(0).isDouble() || !xy.at(1).isDouble()) return errorResult(tr("'pads' are [x, y] in µm."));
            pads << Pt{p.units(xy.at(0).toDouble()), p.units(xy.at(1).toDouble())};
        }
    } else {
        for (int i : nets) pads << p.nets.at(i).terminals.at(1).at;
    }
    QVector<QPair<int, int>> fixed;
    const QJsonObject keep = args.value(QLatin1String("fixed")).toObject();
    for (auto it = keep.begin(); it != keep.end(); ++it) {
        const int at = int(names.indexOf(it.key()));
        const QJsonArray xy = it.value().toArray();
        if (at < 0 || xy.size() != 2) return errorResult(tr("'fixed' maps a net it pairs to its pad [x, y]: %1 is not one.").arg(it.key()));
        const Pt q{p.units(xy.at(0).toDouble()), p.units(xy.at(1).toDouble())};
        int pad = -1;
        for (int k = 0; k < pads.size(); ++k)
            if (std::abs(pads[k].x - q.x) + std::abs(pads[k].y - q.y) <= p.units(1)) pad = k;
        if (pad < 0) return errorResult(tr("'fixed': %1's pad (%2, %3) is not among the pads.").arg(it.key()).arg(xy.at(0).toDouble()).arg(xy.at(1).toDouble()));
        fixed.append({at, pad});
    }
    const QVector<int> pairing = assignPads(starts, pads, fixed);
    QJsonArray rows;
    double before = 0, after = 0;
    for (int k = 0; k < nets.size(); ++k) {
        const Net& n = p.nets.at(nets[k]);
        const Pt was = n.terminals.at(1).at;
        before += p.um(std::abs(starts[k].x - was.x) + std::abs(starts[k].y - was.y));
        QJsonObject row{{"net", n.name}, {"was", pointJson(p, was)}};
        if (pairing[k] >= 0) {
            const Pt to = pads[pairing[k]];
            after += p.um(std::abs(starts[k].x - to.x) + std::abs(starts[k].y - to.y));
            row.insert(QStringLiteral("pad"), pointJson(p, to));
        } else {
            row.insert(QStringLiteral("pad"), QStringLiteral("none left"));
        }
        rows.append(row);
    }
    const bool apply = args.value(QLatin1String("apply")).toBool();
    if (apply) {
        for (int k = 0; k < nets.size(); ++k) {
            if (pairing[k] < 0) continue;
            const int j = s->job.netIndex(p.nets.at(nets[k]).name);
            if (j < 0 || s->job.nets[j].terminals.size() != 2) continue;
            JobTerminal& t = s->job.nets[j].terminals[1];
            // (µm to the millionth: 100, not 99.99999999999999)
            t.at = {std::round(p.um(pads[pairing[k]].x) * 1e6) / 1e6, std::round(p.um(pads[pairing[k]].y) * 1e6) / 1e6};
            t.hasPoint = true;
            t.text.clear();
        }
        s->jobChanged();
        QString e;
        s->saveJob(&e);
    }
    return jsonResult(QJsonObject{{"pairs", rows},
                                  {"distance before µm", um3(before)},
                                  {"distance after µm", um3(after)},
                                  {"applied", apply},
                                  {"note", apply ? tr("The nets' pads are set as paired: route_run routes them.")
                                                 : tr("A proposal: pad order is often a system decision. 'apply' true sets it.")}});
}
