/*
 * fem_process_results.cpp - a process flow's step as results: its state as
 *                           a solution, the values derived down a column
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_process_results.h"

#include "fem_geometry.h"
#include "fem_mesh.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <tuple>

namespace qucs_s::fem {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::fem::Process", text);
}

/// Masetti, Severi and Solmi's mobility in silicon (IEEE Trans. Electron
/// Devices 30, 764, 1983), cm²/Vs, N cm⁻³: electrons arsenic's, holes
/// boron's - the semiconductor physics' defaults.
double masetti(double N, bool electrons)
{
    N = std::max(N, 1.0);
    if (electrons)
        return 52.2 + (1417 - 52.2) / (1 + std::pow(N / 9.68e16, 0.68)) - 43.4 / (1 + std::pow(3.43e20 / N, 2.0));
    return 44.9 * std::exp(-9.23e16 / N) + 470.5 / (1 + std::pow(N / 2.23e17, 0.719)) - 29.0 / (1 + std::pow(6.1e20 / N, 2.0));
}

bool oxideName(const QString& material)
{
    const QString m = material.toLower();
    return m.contains(QLatin1String("dioxide")) || m.contains(QLatin1String("sio2")) || m == QLatin1String("oxide")
           || m.contains(QLatin1String("silica"));
}

} // namespace

const Node* processFlowOf(const Model& model, const QString& tag)
{
    for (const Node& f : model.geometry().children) {
        if (f.type != QLatin1String("process")) continue;
        if (f.tag == tag || f.find(tag)) return &f;
    }
    return nullptr;
}

ProcessColumn processColumn(const ProcessState& state, double x)
{
    ProcessColumn out;
    out.x = x;
    const ProcessDopants* d = state.dopants.get();
    if (!d || d->nx() < 1) return out;
    const double width = d->xe.back() - d->xe.front();
    // Each species' dose over the window (the geometry's unit to metres
    // cancels in the ratio but for the cells' areas: in the unit's, over
    // the width's, a dose per the unit's square).
    for (const ProcessDopants::Field* f : d->species()) {
        double sum = 0;
        for (int j = 0; j < d->ny(); ++j)
            for (int i = 0; i < d->nx(); ++i) {
                const std::size_t k = d->index(i, j);
                if (d->material[k] < 0) continue;
                sum += f->total[k] * (d->xe[std::size_t(i + 1)] - d->xe[std::size_t(i)]) * (d->ye[std::size_t(j + 1)] - d->ye[std::size_t(j)]);
            }
        out.doses.emplace_back(f->name, sum / width);   // m⁻³ × unit: the caller scales by the unit
    }
    int i = 0, j = 0;
    if (!d->cellAt(std::clamp(x, d->xe.front(), d->xe.back()), d->ye.front(), &i, &j)) return out;
    // Down the column: the oxide over the first semiconductor, then its
    // net doping to where it changes sign.
    int top = -1;
    for (int k = d->ny() - 1; k >= 0; --k) {
        const int m = d->material[d->index(i, k)];
        if (m < 0) continue;
        if (d->hosts[std::size_t(m)] != 0) {
            top = k;
            break;
        }
        if (oxideName(d->materials.at(m))) out.oxide += d->ye[std::size_t(k + 1)] - d->ye[std::size_t(k)];
    }
    if (top < 0) return out;
    const int host = d->material[d->index(i, top)];
    out.surface = d->materials.at(host);
    const double xc = 0.5 * (d->xe[std::size_t(i)] + d->xe[std::size_t(i + 1)]);
    auto net = [&](int k) {
        double nd = 0, na = 0;
        const int m = d->material[d->index(i, k)];
        d->carriers(xc, 0.5 * (d->ye[std::size_t(k)] + d->ye[std::size_t(k + 1)]), m, &nd, &na);
        return std::make_tuple(nd - na, nd + na);
    };
    const double surface = d->ye[std::size_t(top + 1)];
    double conductance = 0;   // S (per square), the unit's lengths scaled by the caller
    auto [n0, t0] = net(top);
    const double sign = n0 > 0 ? 1 : n0 < 0 ? -1 : 0;
    for (int k = top; k >= 0; --k) {
        const int m = d->material[d->index(i, k)];
        if (m < 0 || d->hosts[std::size_t(m)] == 0) break;
        const auto [n, tot] = net(k);
        if (sign != 0 && n * sign <= 0) {
            // The junction: between this cell's middle and the last's.
            const auto [np, tp] = net(k + 1);
            const double yp = 0.5 * (d->ye[std::size_t(k + 1)] + d->ye[std::size_t(k + 2)]), y = 0.5 * (d->ye[std::size_t(k)] + d->ye[std::size_t(k + 1)]);
            const double cross = np != n ? yp + (y - yp) * np / (np - n) : y;
            out.junction = surface - cross;
            Q_UNUSED(tp);
            break;
        }
        const double h = d->ye[std::size_t(k + 1)] - d->ye[std::size_t(k)];
        // q µ |N|: cm²/Vs times m⁻³ (the unit's height, scaled after).
        conductance += 1.602176634e-19 * masetti(tot * 1e-6, n > 0) * 1e-4 * std::abs(n) * h;
    }
    if (sign != 0 && conductance > 0) out.sheet = 1 / conductance;
    Q_UNUSED(t0);
    return out;
}

std::shared_ptr<Solution> processSolution(const Model& model, std::shared_ptr<ParameterScope> parameters, const QString& step,
                                          std::optional<double> x, QString* error)
{
    auto fail = [&](const QString& why) -> std::shared_ptr<Solution> {
        if (error) *error = why;
        return nullptr;
    };
    const Node* flow = processFlowOf(model, step);
    if (!flow) return fail(tr("%1 is no process flow, nor a step of one").arg(step));
    const GeometryBuild g = buildGeometry(model, *parameters, step);
    if (!g.ok()) {
        const Node* n = model.find(g.failedAt);
        return fail(tr("the geometry up to it: %1: %2").arg(n ? n->name() : g.failedAt, g.errors.value(g.failedAt)));
    }
    if (!g.process) return fail(tr("%1: the flow made nothing").arg(step));
    const MeshBuild m = buildMesh(model, *parameters, g.topology);
    if (!m.mesh) return fail(tr("its mesh: %1").arg(m.error));
    // (A model of no physics: the flow's variables alone.)
    Model bare = model;
    std::vector<Node>& parts = bare.component().children;
    parts.erase(std::remove_if(parts.begin(), parts.end(), [](const Node& c) { return physicsTypes().contains(c.type); }), parts.end());
    auto s = std::make_shared<Solution>(bare, parameters, m.mesh);
    s->addProcessVariables(g.process, flow->tag);
    s->setProcess(g.process, flow->tag);
    setProcessColumn(*s, x);
    s->log << tr("%1: its state as a solution").arg(model.find(step) ? model.find(step)->name() : step);
    return s;
}

void setProcessColumn(Solution& s, std::optional<double> x)
{
    const std::shared_ptr<const ProcessState> state = s.process();
    if (!state) return;
    const ProcessState& st = *state;
    const double at = x ? *x : 0.5 * (st.x0 + st.x1);
    const ProcessColumn c = processColumn(st, at);
    const double unit = s.mesh().unitScale;
    const QString pre = s.processTag() + QLatin1Char('.');
    // (Each found again: a junction none at this x is said as none.)
    s.setGlobal(pre + QStringLiteral("x"), at * unit, Dim::length(), tr("where the column's values are found"));
    s.setGlobal(pre + QStringLiteral("xj"), std::isfinite(c.junction) ? c.junction * unit : std::numeric_limits<double>::quiet_NaN(), Dim::length(),
                tr("junction depth (net doping's sign change below its surface; none: NaN)"));
    s.setGlobal(pre + QStringLiteral("Rs"), std::isfinite(c.sheet) ? c.sheet / unit : std::numeric_limits<double>::quiet_NaN(), Dim::of(2, 1, -3, -2),
                tr("sheet resistance of the layer above the junction (Masetti's mobilities)"), QStringLiteral("Ω/sq"));
    s.setGlobal(pre + QStringLiteral("tox"), c.oxide * unit, Dim::length(), tr("oxide over the semiconductor"));
    for (const auto& [name, dose] : c.doses) s.setGlobal(pre + QStringLiteral("dose_") + name, dose * unit, Dim::of(-2), tr("%1's dose kept, over the window").arg(name));
}

} // namespace qucs_s::fem
