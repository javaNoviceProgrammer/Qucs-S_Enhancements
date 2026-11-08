/*
 * layoutrouting.cpp - the metal router against a layout, and the routing
 *                     sessions (layoutrouting.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "layoutrouting.h"

#include "markup.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QThread>
#include <QTimer>

#include <cmath>

using namespace qucs_s::router;

namespace qucs_s::routing {

namespace {

constexpr quint64 MostShapes = 20000000;

QString micro(double v)
{
    return QString::number(v, 'f', 3).remove(QRegularExpression(QStringLiteral("\\.?0+$")));
}

} // namespace

bool buildProblem(const layout::Layout& layout, int cell, const Job& job, const Technology& tech, Problem* out, QStringList* warnings, QString* error)
{
    Problem p;
    p.tech = tech;
    p.unit = layout.dbu > 0 && layout.dbu < 0.001 ? layout.dbu : 0.001;
    p.options = job.options;
    if (cell < 0 || cell >= layout.cells.size()) {
        *error = QStringLiteral("The layout has no cell to route in.");
        return false;
    }
    const auto toUnits = [&](QPointF q) { return Pt{p.units(q.x()), p.units(q.y())}; };

    // Which layout layer is which: a metal, a cut, an obstacle rule.
    struct Role {
        int kind = -1;   // 0 metal, 1 cut, 2 obstacle
        int index = -1;
    };
    QHash<int, QVector<Role>> roles;
    const auto role = [&](const LayerId& id, int kind, int index) {
        const int l = layout.layerIndex(layout::LayerKey{id.layer, id.datatype});
        if (l >= 0) roles[l].append({kind, index});
        return l >= 0;
    };
    for (int m = 0; m < tech.metals.size(); ++m)
        if (!role(tech.metals.at(m).layer, 0, m))
            warnings->append(QStringLiteral("The layout has nothing on %1 (%2)").arg(tech.metals.at(m).name, tech.metals.at(m).layer.text()));
    for (int v = 0; v < tech.vias.size(); ++v) role(tech.vias.at(v).layer, 1, v);
    for (int o = 0; o < tech.obstacles.size(); ++o)
        if (!role(tech.obstacles.at(o).layer, 2, o))
            warnings->append(QStringLiteral("The layout has nothing on the obstacle layer %1 (%2)").arg(tech.obstacles.at(o).name, tech.obstacles.at(o).layer.text()));
    const int routeCell = layout.cellIndex(job.routeCell);
    if (routeCell == cell) {
        *error = QStringLiteral("The cell routed is the route cell %1: route its parent.").arg(job.routeCell);
        return false;
    }
    layout::Query q;
    q.cell = cell;
    q.labels = false;
    q.layers.fill(false, layout.layers.size());
    for (auto it = roles.cbegin(); it != roles.cend(); ++it) q.layers[it.key()] = true;
    quint64 count = 0, skipped = 0;
    bool tooMany = false;
    layout::search(layout, q, [&](const layout::Found& f) {
        if (f.cell == routeCell || (routeCell >= 0 && f.via.contains(routeCell))) {
            ++skipped;
            return true;
        }
        if (++count > MostShapes) {
            tooMany = true;
            return false;
        }
        const QPolygonF outline = layout::outlineOf(layout, f);
        if (outline.size() < 3) return true;
        Poly poly;
        poly.reserve(std::size_t(outline.size()));
        for (const QPointF& pt : outline) poly.push_back(toUnits(pt));
        const int layer = int(layout.cells.at(f.cell).shapes.at(f.shape).layer);
        for (const Role& r : roles.value(layer)) {
            if (r.kind == 0) p.metal.append({poly, r.index});
            else if (r.kind == 1) p.cuts.append({poly, r.index});
            else p.obstacles.append({poly, r.index});
        }
        return true;
    });
    if (tooMany) {
        *error = QStringLiteral("The cell has more than %1 shapes on the routing layers: too many for the router.").arg(MostShapes);
        return false;
    }
    if (skipped > 0) warnings->append(QStringLiteral("%1 shape(s) of the route cell %2 left out: a commit replaces them").arg(skipped).arg(job.routeCell));

    // Labels, for terminals named by one.
    QHash<QString, QVector<QPointF>> labels;
    QHash<QString, int> labelMetal;   // the metal the first label of a text is on, or -1
    bool wantsLabels = false;
    for (const JobNet& n : job.nets)
        for (const JobTerminal& t : n.terminals) wantsLabels = wantsLabels || !t.text.isEmpty();
    if (wantsLabels) {
        layout::Query lq;
        lq.cell = cell;
        lq.labels = true;
        layout::search(layout, lq, [&](const layout::Found& f) {
            if (f.isText()) {
                const layout::Label& l = layout.cells.at(f.cell).labels.at(f.label);
                if (!labels.contains(l.text)) {
                    int metal = -1;
                    for (const Role& r : roles.value(int(l.layer)))
                        if (r.kind == 0) metal = r.index;
                    labelMetal.insert(l.text, metal);
                }
                labels[l.text].append(f.transform.map(l.origin));
            }
            return true;
        });
    }
    for (const JobNet& jn : job.nets) {
        Net n;
        n.name = jn.name;
        n.fixed = jn.fixed;
        n.netClass = jn.netClass.isEmpty() ? tech.defaultClass : tech.classIndex(jn.netClass);
        if (n.netClass < 0) {
            QStringList have;
            for (const NetClass& c : tech.classes) have << c.name;
            *error = QStringLiteral("Net %1's class %2 is not in the technology (it has %3).").arg(jn.name, jn.netClass, have.join(QStringLiteral(", ")));
            return false;
        }
        for (int i = 0; i < jn.terminals.size(); ++i) {
            const JobTerminal& jt = jn.terminals.at(i);
            Terminal t;
            t.mode = jt.mode;
            t.text = jt.text;
            QPointF at = jt.at;
            if (!jt.hasPoint) {
                const QVector<QPointF> found = labels.value(jt.text);
                if (found.isEmpty()) {
                    *error = QStringLiteral("Net %1: the cell has no label %2.").arg(jn.name, jt.text);
                    return false;
                }
                if (found.size() > 1)
                    warnings->append(QStringLiteral("Net %1: %2 labels read %3; the first, at (%4, %5), is taken")
                                         .arg(jn.name)
                                         .arg(found.size())
                                         .arg(jt.text, micro(found.first().x()), micro(found.first().y())));
                at = found.first();
                // On the label's metal: a label on M1 names the M1 contact, not
                // the M2 drawn over it.
                if (jt.metal.isEmpty()) t.metal = labelMetal.value(jt.text, -1);
            }
            t.at = toUnits(at);
            if (!jt.metal.isEmpty()) {
                t.metal = tech.metalIndex(jt.metal);
                if (t.metal < 0) {
                    *error = QStringLiteral("Net %1 names the metal %2, which the technology has not.").arg(jn.name, jt.metal);
                    return false;
                }
            }
            n.terminals.append(t);
        }
        for (const QPointF& g : jn.guide) n.guide.push_back(toUnits(g));
        n.band = p.units(jn.band);
        p.nets.append(n);
    }
    for (const JobKeepOut& jk : job.keepOuts) {
        KeepOut k;
        k.name = jk.name.isEmpty() ? QStringLiteral("keep-out %1").arg(p.keepOuts.size() + 1) : jk.name;
        for (const QPointF& pt : jk.outline) k.outline.push_back(toUnits(pt));
        for (const QString& m : jk.metals) {
            const int i = tech.metalIndex(m);
            if (i < 0) {
                *error = QStringLiteral("The keep-out %1 names the metal %2, which the technology has not.").arg(k.name, m);
                return false;
            }
            k.metals.append(i);
        }
        k.except = jk.except;
        k.letThrough = jk.letThrough;
        p.keepOuts.append(k);
    }
    *out = std::move(p);
    return true;
}

QVector<QPointF> strokeOutline(const markup::Stroke& stroke, bool box)
{
    if (box) {
        const QRectF r = stroke.points.boundingRect();
        return {r.bottomLeft(), r.bottomRight(), r.topRight(), r.topLeft()};
    }
    const QPolygonF s = markup::simplified(stroke.points, std::max(stroke.width, 1e-3));
    QVector<QPointF> out(s.begin(), s.end());
    if (out.size() > 1 && out.first() == out.last()) out.removeLast();
    return out;
}

QVector<QPointF> strokeGuide(const markup::Stroke& stroke)
{
    const QPolygonF s = markup::simplified(stroke.points, std::max(stroke.width, 1e-3));
    return QVector<QPointF>(s.begin(), s.end());
}

// ----------------------------------------------------------------------

namespace {
QHash<QString, Session*>& sessions()
{
    static QHash<QString, Session*> all;
    return all;
}
} // namespace

Session* Session::of(const QString& layoutFile, bool create)
{
    const QString key = QFileInfo(layoutFile).absoluteFilePath();
    if (Session* s = sessions().value(key)) return s;
    if (!create) return nullptr;
    static bool registered = false;
    if (!registered) {
        registered = true;
        qRegisterMetaType<qucs_s::router::PassRow>();
        qAddPostRoutine(&Session::stopAll);
    }
    auto* s = new Session(key);
    sessions().insert(key, s);
    if (QFileInfo::exists(s->jobFile())) {
        QString error;
        s->loadJob(&error);
    }
    return s;
}

Session::Session(const QString& file) : a_file(file)
{
    job.layout = QFileInfo(file).fileName();
}

Session::~Session()
{
    cancel();
    wait(60000);
}

void Session::stopAll()
{
    for (Session* s : sessions()) {
        s->cancel();
        if (s->a_thread) s->a_thread->wait();
    }
}

bool Session::technology(Technology* t, QString* error) const
{
    if (!job.technology.isEmpty()) {
        *t = readTechnology(job.technology, error);
        return error->isEmpty();
    }
    if (job.technologyFile.isEmpty()) {
        *error = QStringLiteral("No technology yet: route_setup's 'technology' (a JSON file, or the technology itself)");
        return false;
    }
    const QString path = QDir(QFileInfo(a_file).absolutePath()).absoluteFilePath(job.technologyFile);
    *t = readTechnologyFile(path, error);
    return error->isEmpty();
}

bool Session::saveJob(QString* error) const
{
    QSaveFile f(jobFile());
    if (!f.open(QIODevice::WriteOnly)) {
        *error = QStringLiteral("%1 cannot be written").arg(jobFile());
        return false;
    }
    Job j = job;
    j.layout = QFileInfo(a_file).fileName();
    f.write(QJsonDocument(jobJson(j)).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        *error = QStringLiteral("%1 cannot be written").arg(jobFile());
        return false;
    }
    return true;
}

bool Session::loadJob(QString* error)
{
    QFile f(jobFile());
    if (!f.open(QIODevice::ReadOnly) || f.size() > 64 * 1024 * 1024) {
        *error = QStringLiteral("%1 cannot be read").arg(jobFile());
        return false;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (!doc.isObject()) {
        *error = QStringLiteral("%1 is not JSON: %2").arg(QFileInfo(jobFile()).fileName(), pe.errorString());
        return false;
    }
    const Job j = readJob(doc.object(), error);
    if (!error->isEmpty()) {
        error->prepend(QFileInfo(jobFile()).fileName() + QStringLiteral(": "));
        return false;
    }
    job = j;
    jobChanged();
    return true;
}

void Session::jobChanged()
{
    ++a_jobRevision;
    a_prepared.reset();
    emit changedJob();
}

bool Session::prepare(std::shared_ptr<const layout::Layout> layout, QString* error)
{
    if (!layout) {
        *error = QStringLiteral("The layout is not read");
        return false;
    }
    Technology t;
    if (!technology(&t, error)) return false;
    const int cell = job.cell.isEmpty() ? layout->mainCell() : layout->cellIndex(job.cell);
    if (cell < 0) {
        *error = QStringLiteral("The layout has no cell %1").arg(job.cell);
        return false;
    }
    if (job.nets.isEmpty()) {
        *error = QStringLiteral("There are no nets to route yet");
        return false;
    }
    Problem p;
    QStringList warnings;
    if (!buildProblem(*layout, cell, job, t, &p, &warnings, error)) return false;
    auto r = std::make_shared<Router>(std::move(p));
    if (!r->prepare()) {
        *error = r->error();
        return false;
    }
    a_layout = layout;
    a_prepared = r;
    a_warnings = warnings;
    emit wasPrepared();
    return true;
}

bool Session::start(const QStringList& which, bool changedOnly, QString* error)
{
    if (a_thread) {
        *error = QStringLiteral("A run is going on already (route_stop stops it)");
        return false;
    }
    if (!a_prepared) {
        if (!a_layout || !prepare(a_layout, error)) {
            if (error->isEmpty()) *error = QStringLiteral("Nothing is set up to route (route_setup)");
            return false;
        }
    }
    std::shared_ptr<Router> router = a_prepared;
    QVector<Kept> kept;
    if (changedOnly || !which.isEmpty()) kept = keptRoutes();
    for (const QString& n : which)
        if (router->problem().nets.isEmpty() || std::none_of(router->problem().nets.cbegin(), router->problem().nets.cend(), [&](const Net& x) { return x.name == n; })) {
            *error = QStringLiteral("There is no net %1").arg(n);
            return false;
        }
    // A prepared router runs once: the next run prepares afresh (its maps
    // start clean), and the result keeps this one with its problem.
    a_prepared.reset();
    a_cancel = std::make_shared<std::atomic_bool>(false);
    {
        QMutexLocker lock(&a_mutex);
        a_live.clear();
    }
    a_started = QDateTime::currentDateTime();
    a_mode = !which.isEmpty() ? QStringLiteral("selected") : (changedOnly ? QStringLiteral("changed") : QStringLiteral("all"));
    const quint64 revision = a_jobRevision;
    auto cancel = a_cancel;
    auto done = std::make_shared<std::pair<Result, Verification>>();
    QPointer<Session> self(this);
    a_thread = QThread::create([this, router, which, kept, cancel, done, self] {
        Progress progress;
        progress.cancel = cancel.get();
        progress.pass = [this, self](const PassRow& row) {
            {
                QMutexLocker lock(&a_mutex);
                a_live.append(row);
            }
            QMetaObject::invokeMethod(
                this, [this, row] { emit passFinished(row); }, Qt::QueuedConnection);
        };
        done->first = router->run(which, kept, progress);
        done->second = verify(router->problem(), done->first);
        // The result is kept to be read and committed: its maps are not needed.
        router->releaseMaps();
    });
    a_thread->setStackSize(8u << 20);
    connect(a_thread, &QThread::finished, this, [this, router, done, revision] {
        a_thread->deleteLater();
        a_thread = nullptr;
        a_resultRouter = router;
        a_result = done->first;
        a_verification = done->second;
        a_finished = QDateTime::currentDateTime();
        a_resultJobRevision = revision;
        emit runFinished();
    });
    a_thread->start();
    return true;
}

void Session::cancel()
{
    if (a_cancel) a_cancel->store(true);
}

bool Session::wait(int ms)
{
    if (!a_thread) return true;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(this, &Session::runFinished, &loop, &QEventLoop::quit);
    timer.start(ms);
    if (a_thread) loop.exec();
    return a_thread == nullptr;
}

QVector<PassRow> Session::passesSoFar() const
{
    QMutexLocker lock(&a_mutex);
    return a_live;
}

const Problem* Session::resultProblem() const
{
    return a_resultRouter ? &a_resultRouter->problem() : nullptr;
}

QVector<Kept> Session::keptRoutes() const
{
    QVector<Kept> out;
    if (!a_resultRouter) return out;
    const Problem& p = a_resultRouter->problem();
    for (int n = 0; n < a_result.nets.size() && n < p.nets.size(); ++n) {
        const NetResult& r = a_result.nets.at(n);
        if (r.status == NetResult::Status::Routed || r.status == NetResult::Status::Kept) out.append({p.nets.at(n).name, r});
    }
    return out;
}

int Session::resultNet(const QString& name) const
{
    const Problem* p = resultProblem();
    if (!p) return -1;
    for (int i = 0; i < p->nets.size(); ++i)
        if (p->nets.at(i).name == name) return i;
    return -1;
}

} // namespace qucs_s::routing
