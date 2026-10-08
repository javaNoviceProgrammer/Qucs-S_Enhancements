/*
 * cursorvalues.cpp - a schematic's nets labelled with their values where a
 * diagram's marker is (see cursorvalues.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "cursorvalues.h"

#include "dataset.h"
#include "misc.h"
#include "node.h"
#include "schematic.h"
#include "wire.h"
#include "wirelabel.h"
#include "diagrams/diagram.h"
#include "diagrams/graph.h"
#include "diagrams/marker.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>

#include <cmath>
#include <functional>
#include <memory>

namespace qucs_s::cursor {

namespace {

namespace ds = qucs_s::dataset;

QString tr(const char* text)
{
    return QCoreApplication::translate("CursorValues", text);
}

// The dataset last read, read again only when its file changed: the labels
// are laid out at every paint as the marker moves.
const ds::Dataset* cached(const QString& file, QString* error)
{
    static QString lastFile;
    static QDateTime lastModified;
    static qint64 lastSize = -1;
    static std::unique_ptr<ds::Dataset> data;
    const QFileInfo info(file);
    if (!info.exists()) {
        *error = tr("%1 is not there: simulate").arg(info.fileName());
        return nullptr;
    }
    if (data && file == lastFile && info.lastModified() == lastModified && info.size() == lastSize) return data.get();
    auto fresh = std::make_unique<ds::Dataset>();
    if (!fresh->read(file, error)) return nullptr;
    data = std::move(fresh);
    lastFile = file;
    lastModified = info.lastModified();
    lastSize = info.size();
    return data.get();
}

// \a v with four significant digits, an SI prefix and the unit: "424.7 mV".
QString volts(double v)
{
    static const char* const prefixes[] = {"f", "p", "n", "\u00B5", "m", "", "k", "M", "G", "T"};
    if (!std::isfinite(v)) return QString::number(v);
    if (std::abs(v) < 1e-18) return QStringLiteral("0 V");
    int e3 = std::clamp(int(std::floor(std::log10(std::abs(v)) / 3.0)), -5, 4);
    QString mantissa = QString::number(v / std::pow(10.0, 3 * e3), 'g', 4);
    if (std::abs(mantissa.toDouble()) >= 1000.0 && e3 < 4) mantissa = QString::number(v / std::pow(10.0, 3 * ++e3), 'g', 4);
    return mantissa + QLatin1Char(' ') + QString::fromUtf8(prefixes[e3 + 5]) + QLatin1Char('V');
}

// The index of the sample of \a v nearest \a x.
int nearest(const QVector<double>& v, double x)
{
    int best = 0;
    for (int i = 1; i < v.size(); ++i)
        if (std::abs(v.at(i) - x) < std::abs(v.at(best) - x)) best = i;
    return best;
}

} // namespace

const Marker* source(const Schematic* sch)
{
    if (sch == nullptr) return nullptr;
    const Marker* chosen = sch->cursorMarker();
    const Marker* selected = nullptr;
    const Marker* first = nullptr;
    for (const Diagram* d : sch->a_DocDiags)
        for (const Graph* g : d->Graphs)
            for (const Marker* m : g->Markers) {
                if (m == chosen) return m;
                if (selected == nullptr && m->isSelected) selected = m;
                if (first == nullptr) first = m;
            }
    return selected ? selected : first;
}

Reading at(const Schematic* sch, const Marker* marker)
{
    Reading r;
    if (marker == nullptr || marker->graph() == nullptr || marker->varPos().empty()) {
        r.error = tr("no marker: place one on a trace of a transient or a sweep");
        return r;
    }
    const Graph* g = marker->graph();
    // The run the trace is of: its dataset, and how it names a voltage.
    QString var = g->Var, tail;
    const int slash = int(var.indexOf(QLatin1Char('/')));
    if (slash > 0) {
        tail = QLatin1Char('.') + var.left(slash);
        var = var.mid(slash + 1);
    }
    const QDir folder = QFileInfo(sch->getDocName()).absoluteDir();
    QString file = folder.filePath(sch->getDataSet()) + tail;
    const int colon = int(var.indexOf(QLatin1Char(':')));
    if (colon > 0) {
        file = folder.filePath(var.left(colon) + QStringLiteral(".dat") + tail);
        var = var.mid(colon + 1);
    }
    static const QRegularExpression spice(QStringLiteral("^(\\w+)\\.v\\((.+)\\)$"));
    static const QRegularExpression qucsator(QStringLiteral("^([^.]+)\\.(Vt|v)$"));
    std::function<QString(const QString&)> voltageOf;
    if (const QRegularExpressionMatch m = spice.match(var); m.hasMatch()) {
        const QString analysis = m.captured(1);
        voltageOf = [analysis](const QString& net) { return analysis + QStringLiteral(".v(") + net + QLatin1Char(')'); };
    } else if (const QRegularExpressionMatch m = qucsator.match(var); m.hasMatch()) {
        const QString suffix = m.captured(2);
        voltageOf = [suffix](const QString& net) { return net + QLatin1Char('.') + suffix; };
    } else if (var.contains(QLatin1Char('.'))) {
        const QString analysis = var.section(QLatin1Char('.'), 0, 0);
        voltageOf = [analysis](const QString& net) { return analysis + QStringLiteral(".v(") + net + QLatin1Char(')'); };
    } else {
        voltageOf = [](const QString& net) { return QStringLiteral("v(") + net + QLatin1Char(')'); };
    }
    const ds::Dataset* data = cached(file, &r.error);
    if (data == nullptr) return r;
    // The marker's place: its x, and its values of any further sweep.
    const std::vector<double>& pos = marker->varPos();
    const DataX* xs = g->axis(0);
    r.x = xs != nullptr ? xs->Var : QString();
    r.at = pos.front();
    // The named nets, each once, where its label is.
    QStringList order;
    QHash<QString, QPoint> anchors;
    const auto take = [&](const QString& name, QPoint where) {
        if (name.isEmpty() || anchors.contains(name)) return;
        anchors.insert(name, where);
        order << name;
    };
    for (const Wire* w : *sch->a_Wires)
        if (w->hasLabel()) take(w->label()->Name, w->label()->root());
    for (const Node* n : *sch->a_Nodes)
        if (n->hasLabel()) take(n->label()->Name, n->label()->root());
    for (const QString& net : std::as_const(order)) {
        const ds::Variable* v = data->find(voltageOf(net));
        if (v == nullptr || v->dependencies.isEmpty() || v->re.isEmpty()) continue;
        // The slice of the further sweeps the marker is on, then along x.
        QList<const ds::Variable*> deps;
        for (const QString& name : v->dependencies) deps << data->find(name);
        if (deps.contains(nullptr)) continue;
        qint64 offset = 0, stride = deps.first()->size();
        for (int k = 1; k < deps.size(); ++k) {
            const int i = k < int(pos.size()) ? nearest(deps.at(k)->re, pos.at(k)) : 0;
            offset += i * stride;
            stride *= deps.at(k)->size();
        }
        const QVector<double>& x = deps.first()->re;
        const int n = int(x.size());
        if (n < 1 || offset + n > v->size()) continue;
        int i = 0;
        double f = 0;
        if (n > 1) {
            const bool rising = x.last() >= x.first();
            i = 0;
            while (i + 2 < n && (rising ? x.at(i + 1) < r.at : x.at(i + 1) > r.at)) ++i;
            const double span = x.at(i + 1) - x.at(i);
            f = span != 0 ? std::clamp((r.at - x.at(i)) / span, 0.0, 1.0) : 0.0;
        }
        const auto sample = [&](const QVector<double>& values) {
            const double a = values.at(offset + i);
            const double b = n > 1 ? values.at(offset + i + 1) : a;
            return a + f * (b - a);
        };
        Value value;
        value.net = net;
        value.anchor = anchors.value(net);
        const double re = sample(v->re);
        if (v->isComplex()) {
            const double im = sample(v->im);
            value.complex = true;
            value.value = std::hypot(re, im);
            value.phase = std::atan2(im, re) * 180.0 / M_PI;
            value.text = volts(value.value) + QStringLiteral(" ∠")
                         + QString::number(value.phase, 'f', 1) + QStringLiteral("°");
        } else {
            value.value = re;
            value.text = volts(re);
        }
        r.values << value;
    }
    if (r.values.isEmpty() && !order.isEmpty())
        r.error = tr("%1 has no voltage of the named nets (%2)").arg(QFileInfo(file).fileName(), voltageOf(order.first()));
    return r;
}

} // namespace qucs_s::cursor
