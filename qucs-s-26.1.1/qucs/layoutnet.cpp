/*
 * layoutnet.cpp - a net of a layout traced from one of its shapes
 *                 (layoutnet.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "layoutnet.h"

#include "layoutrouter/router.h"
#include "layoutrouter/router_geometry.h"
#include "layoutrouter/router_job.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QSet>

#include <cmath>
#include <deque>
#include <functional>

namespace qucs_s::layout {

namespace {

using router::Coord;
using router::Poly;

Poly polyOf(const QPolygonF& outline, double dbu)
{
    Poly out;
    out.reserve(std::size_t(outline.size()));
    for (const QPointF& p : outline) out.push_back({Coord(std::llround(p.x() / dbu)), Coord(std::llround(p.y() / dbu))});
    if (out.size() > 1 && out.front() == out.back()) out.pop_back();
    return out;
}

// Which shape a Found is, in the traced cell: its definition, its copy,
// and where its placements put it (two placements of one cell are two
// shapes).
QString keyOf(const Found& f, double dbu)
{
    const QTransform& t = f.transform;
    return QStringLiteral("%1:%2:%3:%4:%5:%6:%7")
        .arg(f.cell)
        .arg(f.shape)
        .arg(f.copy)
        .arg(std::llround(t.dx() / dbu))
        .arg(std::llround(t.dy() / dbu))
        .arg(std::llround(t.m11() * 1e6))
        .arg(std::llround(t.m12() * 1e6));
}

// A run's centre line: a path's spine, an axis-aligned rectangle's long
// side less its short.
double runLength(const Layout& layout, const Found& f, const QPolygonF& outline)
{
    const QPolygonF spine = spineOf(layout, f);
    if (spine.size() >= 2) {
        double s = 0;
        for (int i = 1; i < spine.size(); ++i) s += std::hypot(spine[i].x() - spine[i - 1].x(), spine[i].y() - spine[i - 1].y());
        return s;
    }
    QPolygonF o = outline;
    if (o.size() > 1 && o.first() == o.last()) o.removeLast();
    if (o.size() != 4) return 0;
    const double eps = layout.dbu / 2;
    for (int i = 0; i < 4; ++i) {
        const QPointF a = o[i], b = o[(i + 1) % 4];
        if (std::abs(a.x() - b.x()) > eps && std::abs(a.y() - b.y()) > eps) return 0;   // (not a rectangle along x and y)
    }
    const QRectF r = o.boundingRect();
    return std::abs(r.width() - r.height());
}

} // namespace

QVector<int> NetConnectivity::layers() const
{
    QVector<int> out;
    for (const NetStep& s : steps)
        for (int l : {s.lower, s.cut, s.upper})
            if (l >= 0 && !out.contains(l)) out << l;
    return out;
}

QVector<int> TracedNet::cells() const
{
    QVector<int> out;
    for (const NetShape& s : shapes)
        if (!out.contains(s.found.cell)) out << s.found.cell;
    return out;
}

bool connectivityBeside(const Layout& layout, NetConnectivity* out, QString* why)
{
    const QString jobFile = layout.file + QStringLiteral(".route.json");
    QFile f(jobFile);
    if (!f.exists()) {
        *why = QStringLiteral("no router job beside the layout (%1)").arg(QFileInfo(jobFile).fileName());
        return false;
    }
    if (!f.open(QIODevice::ReadOnly) || f.size() > 64 * 1024 * 1024) {
        *why = QStringLiteral("%1 cannot be read").arg(QFileInfo(jobFile).fileName());
        return false;
    }
    QJsonParseError pe;
    const QJsonDocument d = QJsonDocument::fromJson(f.readAll(), &pe);
    QString error;
    const router::Job job = d.isObject() ? router::readJob(d.object(), &error) : router::Job();
    if (!d.isObject() || !error.isEmpty()) {
        *why = QStringLiteral("%1: %2").arg(QFileInfo(jobFile).fileName(), d.isObject() ? error : pe.errorString());
        return false;
    }
    router::Technology t;
    QString from;
    if (!job.technology.isEmpty()) {
        t = router::readTechnology(job.technology, &error);
        from = QFileInfo(jobFile).fileName();
    } else if (!job.technologyFile.isEmpty()) {
        const QString path = QDir(QFileInfo(layout.file).absolutePath()).absoluteFilePath(job.technologyFile);
        t = router::readTechnologyFile(path, &error);
        from = QFileInfo(path).fileName();
    } else {
        *why = QStringLiteral("%1 names no technology").arg(QFileInfo(jobFile).fileName());
        return false;
    }
    if (!error.isEmpty()) {
        *why = error;
        return false;
    }
    const auto index = [&](const router::LayerId& id) { return layout.layerIndex(LayerKey{id.layer, id.datatype}); };
    out->steps.clear();
    for (int pair = 0; pair + 1 < t.metals.size(); ++pair) {
        QVector<int> cuts;   // (each cut layer between the pair, once)
        for (const router::Via& v : std::as_const(t.vias))
            if (v.lower == pair && !cuts.contains(index(v.layer))) cuts << index(v.layer);
        for (int c : std::as_const(cuts)) out->steps.append({index(t.metals.at(pair).layer), c, index(t.metals.at(pair + 1).layer)});
    }
    if (out->steps.isEmpty() && !t.metals.isEmpty()) out->steps.append({index(t.metals.first().layer), -1, -1});
    out->source = QStringLiteral("the router's technology (%1)").arg(from);
    return true;
}

NetTracing traceNetLater(std::shared_ptr<const Layout> layout, NetTrace trace, QObject* context, std::function<void(const TracedNet&)> then)
{
    NetTracing going;
    going.cancel = std::make_shared<std::atomic_bool>(false);
    auto result = std::make_shared<TracedNet>();
    trace.cancel = going.cancel.get();   // (held by the thread's function below)
    const std::shared_ptr<std::atomic_bool> cancel = going.cancel;
    QThread* thread = QThread::create([layout, trace, result, cancel] { *result = traceNet(*layout, trace); });
    thread->setStackSize(8u << 20);   // (a main thread's: a thread's is 512 kB on macOS)
    QObject::connect(thread, &QThread::finished, context, [result, then] { then(*result); });
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    going.thread = thread;
    thread->start();
    return going;
}

QVector<Found> seedsAt(const Layout& layout, int cell, QPointF point, int layer, const NetConnectivity& connectivity)
{
    QVector<int> order;   // the metals, bottom first
    if (layer >= 0) order << layer;
    else
        for (const NetStep& s : connectivity.steps)
            for (int l : {s.lower, s.upper})
                if (l >= 0 && !order.contains(l)) order << l;
    const double tol = layout.dbu;
    for (int l : std::as_const(order)) {
        Query q;
        q.cell = cell;
        q.labels = false;
        q.layers.fill(false, layout.layers.size());
        if (l >= q.layers.size()) continue;
        q.layers[l] = true;
        q.region = QRectF(point - QPointF(tol, tol), QSizeF(2 * tol, 2 * tol));
        QVector<Found> found;
        search(layout, q, [&](const Found& f) {
            if (hits(layout, f, point, tol)) found << f;
            return found.size() < 100;
        });
        if (!found.isEmpty()) return found;
    }
    return {};
}

TracedNet traceNet(const Layout& layout, const NetTrace& trace)
{
    TracedNet net;
    const int nl = int(layout.layers.size());
    const double dbu = layout.dbu > 0 ? layout.dbu : 0.001;
    const auto stopped = [&](int l) { return l >= 0 && l < trace.stopAt.size() && trace.stopAt.at(l); };
    // What each layer joins: itself, and through the steps the cut layers
    // its metal meets, or the metals its cut joins.
    QVector<QVector<int>> joins(nl);
    QVector<bool> isCut(nl, false);
    for (int l = 0; l < nl; ++l) joins[l] << l;
    for (const NetStep& s : trace.connectivity.steps) {
        if (s.cut < 0) continue;
        isCut[s.cut] = true;
        for (int m : {s.lower, s.upper}) {
            if (m < 0) continue;
            if (!joins[m].contains(s.cut)) joins[m] << s.cut;
            if (!joins[s.cut].contains(m)) joins[s.cut] << m;
        }
    }
    QSet<QString> seen;
    std::vector<Poly> polys;
    std::deque<int> queue;
    const auto add = [&](const Found& f, Poly&& poly, const QPolygonF& outline) {
        const int layer = int(layout.cells.at(f.cell).shapes.at(f.shape).layer);
        net.shapes.append({f, outline, layer, layer < nl && isCut[layer]});
        polys.push_back(std::move(poly));
        queue.push_back(int(net.shapes.size()) - 1);
        net.bounds = net.bounds.isNull() ? outline.boundingRect() : net.bounds.united(outline.boundingRect());
    };
    for (const Found& f : trace.seeds) {
        if (f.isText() || f.shape < 0) continue;
        const QString key = keyOf(f, dbu);
        if (seen.contains(key)) continue;
        seen.insert(key);
        const QPolygonF outline = outlineOf(layout, f);
        add(f, polyOf(outline, dbu), outline);
    }
    while (!queue.empty()) {
        if (trace.cancel && trace.cancel->load()) {
            net.cancelled = true;
            break;
        }
        if (int(net.shapes.size()) >= trace.maxShapes) {
            net.stopped = true;
            break;
        }
        const int at = queue.front();
        queue.pop_front();
        const int layer = net.shapes.at(at).layer;
        Query q;
        q.cell = trace.cell;
        q.labels = false;
        q.layers.fill(false, nl);
        bool any = false;
        for (int l : joins.value(layer))
            if (!stopped(l)) {
                q.layers[l] = true;
                any = true;
            }
        if (!any) continue;
        q.region = net.shapes.at(at).outline.boundingRect().adjusted(-dbu, -dbu, dbu, dbu);
        const Poly here = polys[std::size_t(at)];
        search(layout, q, [&](const Found& f) {
            if (f.isText() || f.shape < 0) return true;
            const QString key = keyOf(f, dbu);
            if (seen.contains(key)) return true;
            const QPolygonF outline = outlineOf(layout, f);
            Poly poly = polyOf(outline, dbu);
            if (router::polyDistance(here, poly) != 0) return true;
            seen.insert(key);
            add(f, std::move(poly), outline);
            return int(net.shapes.size()) < trace.maxShapes;
        });
        if (int(net.shapes.size()) >= trace.maxShapes && !queue.empty()) net.stopped = true;
    }
    // Its length, its vias (a cut's two metals: the cuts that join the same
    // two shapes are one via) and the texts on it.
    for (int i = 0; i < net.shapes.size(); ++i)
        if (!net.shapes.at(i).cut) net.length += runLength(layout, net.shapes.at(i).found, net.shapes.at(i).outline);
    std::vector<router::BoxIndex> index(static_cast<std::size_t>(nl));
    std::vector<std::vector<int>> onLayer(static_cast<std::size_t>(nl));
    {
        std::vector<std::vector<router::Box>> boxes(static_cast<std::size_t>(nl));
        for (int i = 0; i < net.shapes.size(); ++i) {
            const int l = net.shapes.at(i).layer;
            if (l < 0 || l >= nl) continue;
            boxes[std::size_t(l)].push_back(router::boundsOf(polys[std::size_t(i)]));
            onLayer[std::size_t(l)].push_back(i);
        }
        for (int l = 0; l < nl; ++l) index[std::size_t(l)].build(boxes[std::size_t(l)]);
    }
    // Its pieces on each layer: its shapes there joined where they touch.
    for (int l = 0; l < nl; ++l) {
        const std::vector<int>& ids = onLayer[std::size_t(l)];
        if (ids.empty()) continue;
        std::vector<int> root(ids.size());
        for (std::size_t i = 0; i < ids.size(); ++i) root[i] = int(i);
        const std::function<int(int)> find = [&](int a) {
            while (root[std::size_t(a)] != a) a = root[std::size_t(a)] = root[std::size_t(root[std::size_t(a)])];
            return a;
        };
        int count = int(ids.size());
        for (std::size_t i = 0; i < ids.size(); ++i)
            index[std::size_t(l)].query(router::boundsOf(polys[std::size_t(ids[i])]), [&](int j) {
                if (std::size_t(j) <= i) return true;
                const int a = find(int(i)), b = find(j);
                if (a != b && router::polyDistance(polys[std::size_t(ids[i])], polys[std::size_t(ids[std::size_t(j)])]) == 0) {
                    root[std::size_t(std::max(a, b))] = std::min(a, b);
                    --count;
                }
                return true;
            });
        net.pieces.insert(l, count);
    }
    QMap<QString, NetVia> vias;
    QMap<QString, QPointF> viaSum;
    for (int i = 0; i < net.shapes.size(); ++i) {
        if (!net.shapes.at(i).cut) continue;
        const int c = net.shapes.at(i).layer;
        QStringList touched;
        for (int m : joins.value(c)) {
            if (m == c || m < 0 || m >= nl) continue;
            int first = -1;
            index[std::size_t(m)].query(router::boundsOf(polys[std::size_t(i)]), [&](int j) {
                const int s = onLayer[std::size_t(m)][std::size_t(j)];
                if (router::polyDistance(polys[std::size_t(i)], polys[std::size_t(s)]) == 0 && (first < 0 || s < first)) first = s;
                return true;
            });
            touched << QString::number(first);
        }
        const QString key = QString::number(c) + QLatin1Char(':') + touched.join(QLatin1Char(':'));
        NetVia& v = vias[key];
        v.layer = c;
        ++v.cuts;
        viaSum[key] += net.shapes.at(i).outline.boundingRect().center();
    }
    for (auto it = vias.begin(); it != vias.end(); ++it) {
        it->at = viaSum.value(it.key()) / it->cuts;
        net.vias.append(*it);
    }
    if (!net.shapes.isEmpty()) {
        Query lq;
        lq.cell = trace.cell;
        lq.labels = true;
        lq.region = net.bounds.adjusted(-dbu, -dbu, dbu, dbu);
        search(layout, lq, [&](const Found& f) {
            if (!f.isText()) return true;
            const Label& label = layout.cells.at(f.cell).labels.at(f.label);
            const int l = int(label.layer);
            if (l < 0 || l >= nl || onLayer[std::size_t(l)].empty()) return true;
            const QPointF p = f.transform.map(label.origin);
            const router::Pt pt{Coord(std::llround(p.x() / dbu)), Coord(std::llround(p.y() / dbu))};
            bool on = false;
            index[std::size_t(l)].query(router::Box{pt.x, pt.y, pt.x, pt.y}, [&](int j) {
                on = router::inside(polys[std::size_t(onLayer[std::size_t(l)][std::size_t(j)])], pt);
                return !on;
            });
            if (on) net.labels.append({label.text, l, p, f.cell});
            return net.labels.size() < 1000;
        });
    }
    return net;
}

} // namespace qucs_s::layout
