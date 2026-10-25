/*
 * rf25_sparams.cpp - the quasi-static extraction carried across into the
 *                    multiphysics engine's own line, so that its writers
 *                    and its mode solver apply to an RF 2.5D answer
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "rf25_sparams.h"

#include <QCoreApplication>

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>

namespace qucs_s::rf25 {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::rf25", text);
}

fem::Scope scopeOf(const QList<fem::Model::Parameter>& parameters)
{
    fem::Scope scope(&fem::Scope::builtins());
    for (const fem::Model::Parameter& p : parameters) {
        const fem::Expression e = fem::Expression::compile(p.expression, scope);
        if (e.isValid() && e.isConstant()) scope.setConstant(p.name, e.constant(), e.dim());
    }
    return scope;
}

/// A node's property as a number, the model's parameters in scope.
/// \a problem is set where it is set to something that is none.
double numberIn(const Node& node, const QString& key, const fem::Scope& scope, double fallback, QString* problem)
{
    const QString text = textOf(node, key);
    if (text.trimmed().isEmpty()) return fallback;
    const fem::Expression e = fem::Expression::compile(text, scope);
    if (!e.isValid() || !e.isConstant()) {
        if (problem != nullptr && problem->isEmpty())
            *problem = tr("Study: %1 is not a number (%2).").arg(key, e.isValid() ? tr("it is not a constant") : e.error());
        return fallback;
    }
    return e.constant();
}

} // namespace

fem::LineRLGC asLine(const Extraction& extraction, int index, double length, double frequency)
{
    fem::LineRLGC out;
    const LineParameters lp = lineParameters(extraction, index, length, frequency);
    if (!lp.ok()) {
        out.error = lp.problems.join(QStringLiteral(" "));
        return out;
    }
    out.frequency = lp.frequency;
    out.R = lp.r;
    out.L = lp.l;
    out.G = lp.g;
    out.C = lp.c;
    out.Zc = lp.zc;
    out.gamma = lp.gamma;
    out.alphaDb = lp.alphaDb;
    // The engine's epsEff is (beta/k0)^2 - what the wave actually does,
    // the conductors' loss slowing it included - where there is a
    // frequency to take beta at; the quasi-static C/C0 where there is not.
    const double k0 = 2 * M_PI * lp.frequency / kLightSpeed;
    if (k0 > 0 && lp.gamma.imag() > 0) {
        const double n = lp.gamma.imag() / k0;
        out.epsEff = n * n;
        out.velocity = 2 * M_PI * lp.frequency / lp.gamma.imag();
    } else {
        out.epsEff = lp.epsilonEff;
        out.velocity = lp.velocity;
    }
    return out;
}

fem::CoupledLines asCoupledLines(const Extraction& extraction, double length, double frequency)
{
    fem::CoupledLines out;
    if (!extraction.ok()) {
        out.error = extraction.problems.isEmpty() ? tr("nothing was extracted") : extraction.problems.join(QStringLiteral(" "));
        return out;
    }
    if (!(length > 0)) {
        out.error = tr("coupled lines' parameters are per metre: say how long the piece that was solved is");
        return out;
    }
    const int n = int(extraction.names.size());
    if (n < 1) {
        out.error = tr("there are no nets to make lines of");
        return out;
    }
    out.names = extraction.names;
    out.rlgc = frequency > 0;
    out.frequency = out.rlgc ? frequency : 0.0;
    const double omega = 2 * M_PI * frequency;
    const std::size_t N = std::size_t(n);
    // Per metre, and symmetric: the solve's own asymmetry is numerical,
    // and a capacitance matrix that is not symmetric has no modes.
    auto perMetre = [n, N, length](const QList<QList<double>>& m, double scale) {
        fem::CoupledLines::Matrix r(N, std::vector<double>(N, 0));
        if (int(m.size()) != n) return r;
        for (int i = 0; i < n; ++i) {
            if (int(m.at(i).size()) != n) return fem::CoupledLines::Matrix(N, std::vector<double>(N, 0));
            for (int j = 0; j < n; ++j)
                r[std::size_t(i)][std::size_t(j)] = scale * 0.5 * (m.at(i).at(j) + m.at(j).at(i)) / length;
        }
        return r;
    };
    // From the line's middle where that was taken - the ends' fringing
    // left out, and per metre already - else the whole over the length.
    const Extraction::PerMetre& pm = extraction.perMetre;
    const bool middle = pm.fromMiddle && int(pm.maxwell.size()) == n && int(pm.maxwell0.size()) == n;
    const auto symmetric = [n, N](const QList<QList<double>>& m, double scale) {
        fem::CoupledLines::Matrix r(N, std::vector<double>(N, 0));
        if (int(m.size()) != n) return r;
        for (int i = 0; i < n; ++i) {
            if (int(m.at(i).size()) != n) return fem::CoupledLines::Matrix(N, std::vector<double>(N, 0));
            for (int j = 0; j < n; ++j) r[std::size_t(i)][std::size_t(j)] = scale * 0.5 * (m.at(i).at(j) + m.at(j).at(i));
        }
        return r;
    };
    out.C = middle ? symmetric(pm.maxwell, 1) : perMetre(extraction.maxwell, 1);
    out.C0 = middle ? symmetric(pm.maxwell0, 1) : perMetre(extraction.maxwell0, 1);
    out.G = !out.rlgc ? fem::CoupledLines::Matrix(N, std::vector<double>(N, 0))
            : middle && int(pm.dielectricLoss.size()) == n ? symmetric(pm.dielectricLoss, omega)
                                                          : perMetre(extraction.dielectricLoss, omega);

    // L = C0^-1/c^2, the quasi-TEM relation, as the engine's own coupled
    // lines take it for a lossless line of any number of conductors.
    Eigen::MatrixXd C0m(n, n);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) C0m(i, j) = out.C0[std::size_t(i)][std::size_t(j)];
    const Eigen::FullPivLU<Eigen::MatrixXd> lu(C0m);
    if (!lu.isInvertible()) {
        out.error = tr("the capacitance matrix in vacuum cannot be inverted for L: is there a ground beside the conductors?");
        return out;
    }
    const Eigen::MatrixXd l = lu.inverse() / (kLightSpeed * kLightSpeed);
    out.L.assign(N, std::vector<double>(N, 0));
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) out.L[std::size_t(i)][std::size_t(j)] = 0.5 * (l(i, j) + l(j, i));

    // R is diagonal: each net's surface resistance times how its own
    // charge crowds. One conductor pushing another's current about - the
    // proximity effect - is not in an electrostatic solve.
    out.R.assign(N, std::vector<double>(N, 0));
    if (out.rlgc)
        for (int i = 0; i < n; ++i) {
            const std::complex<double> zs =
                surfaceImpedance(extraction.sigma.value(i), extraction.thickness.value(i), frequency);
            out.R[std::size_t(i)][std::size_t(i)] = middle && i < pm.crowding.size()
                                                        ? zs.real() * pm.crowding.at(i)
                                                        : zs.real() * length * extraction.crowding.value(i);
        }
    // The modes, by the engine's own solver: a lossless line's at 1 GHz,
    // as its quasi-TEM lines are taken.
    fem::coupledModes(out, out.rlgc ? omega : 2 * M_PI * 1e9);
    return out;
}

QList<double> sweepFrequencies(double from, double to, int points, bool logarithmic, QString* problem)
{
    const auto refuse = [problem](const QString& why) {
        if (problem != nullptr) *problem = why;
        return QList<double>();
    };
    if (problem != nullptr) problem->clear();
    if (points < 1) return refuse(tr("A sweep wants at least one point."));
    if (!(from >= 0) || !(to >= 0)) return refuse(tr("A sweep's frequencies cannot be less than nothing."));
    if (to < from) return refuse(tr("A sweep ends above where it starts: %1 Hz to %2 Hz.")
                                     .arg(from, 0, 'g', 4)
                                     .arg(to, 0, 'g', 4));
    if (logarithmic && !(from > 0))
        return refuse(tr("A logarithmic sweep cannot start at nothing: give it a first frequency above zero."));
    QList<double> out;
    if (points == 1 || to == from) {
        out << to;
        return out;
    }
    for (int i = 0; i < points; ++i) {
        const double t = double(i) / double(points - 1);
        out << (logarithmic ? from * std::pow(to / from, t) : from + t * (to - from));
    }
    return out;
}

double numberOf(const Node& node, const QString& key, const Model& model, double fallback, bool* ok)
{
    if (ok != nullptr) *ok = true;
    const QString text = textOf(node, key);
    if (text.trimmed().isEmpty()) return fallback;
    const fem::Expression e = fem::Expression::compile(text, scopeOf(model.parameters()));
    if (!e.isValid() || !e.isConstant()) {
        if (ok != nullptr) *ok = false;
        return fallback;
    }
    return e.constant();
}

QList<double> studySweep(const Model& model, QString* problem)
{
    if (problem != nullptr) problem->clear();
    const Node* study = model.root.child(QStringLiteral("rf_study"));
    if (study == nullptr) {
        if (problem != nullptr) *problem = tr("The model has no study to take its frequencies from.");
        return {};
    }
    const fem::Scope scope = scopeOf(model.parameters());
    QString why;
    const double from = numberIn(*study, QStringLiteral("from"), scope, 0, &why);
    const double to = numberIn(*study, QStringLiteral("to"), scope, 0, &why);
    const double points = numberIn(*study, QStringLiteral("points"), scope, 1, &why);
    if (!why.isEmpty()) {
        if (problem != nullptr) *problem = why;
        return {};
    }
    const bool logarithmic = textOf(*study, QStringLiteral("scale")) == QLatin1String("log");
    // (A study written from low to high or the other way about: a sweep
    // runs upwards either way.)
    return sweepFrequencies(std::min(from, to), std::max(from, to), int(std::lround(points)), logarithmic, problem);
}

fem::LineSweep lineSweep(const Extraction& extraction, int index, double length, const QList<double>& frequencies)
{
    fem::LineSweep out;
    if (frequencies.isEmpty()) {
        out.error = tr("there are no frequencies to sweep");
        return out;
    }
    for (const double f : frequencies) {
        if (!(f > 0)) {
            out.error = tr("a line's S-parameters want a frequency above nothing: at none it has no impedance and no "
                           "propagation, only the capacitance it was solved for");
            return out;
        }
        const fem::LineRLGC l = asLine(extraction, index, length, f);
        if (!l.error.isEmpty()) {
            out.error = l.error;
            return out;
        }
        out.points.push_back(l);
    }
    return out;
}

fem::CoupledSweep coupledSweep(const Extraction& extraction, double length, const QList<double>& frequencies)
{
    fem::CoupledSweep out;
    if (frequencies.isEmpty()) {
        out.error = tr("there are no frequencies to sweep");
        return out;
    }
    for (const double f : frequencies) {
        if (!(f > 0)) {
            out.error = tr("coupled lines' S-parameters want a frequency above nothing: at none they have no impedance "
                           "and no propagation, only the capacitance they were solved for");
            return out;
        }
        fem::CoupledLines l = asCoupledLines(extraction, length, f);
        if (!l.error.isEmpty()) {
            out.error = l.error;
            return out;
        }
        out.points.push_back(l);
    }
    return out;
}

QuasiStaticLimit quasiStaticLimit(const Stack& stack, const Mesh& mesh, double micrometre, double velocity, double length,
                                  int conductors)
{
    QuasiStaticLimit out;
    // The slowest dielectric the field crosses.
    for (const Layer& l : stack.layers)
        if ((l.kind == Layer::Dielectric || l.kind == Layer::Substrate) && l.epsr > out.epsilonMax) out.epsilonMax = l.epsr;
    // How deep the dielectrics go, and how wide the widest conductor is:
    // the cross-section's two transverse dimensions.
    double deep = 0;
    for (const Layer& l : stack.layers)
        if (l.kind == Layer::Dielectric || l.kind == Layer::Substrate) deep += l.thickness;
    double wide = 0;
    if (!mesh.cells.isEmpty()) {
        QRectF box;
        for (const MeshCell& c : mesh.cells) box = box.isNull() ? c.outline.boundingRect() : box.united(c.outline.boundingRect());
        // A line runs along its longer side, so the shorter one is its
        // width - the dimension across which the quasi-TEM field stands.
        wide = std::min(box.width(), box.height()) * micrometre;
    }
    if (wide > deep) {
        out.transverse = wide;
        // (Of several, what they span together: two 100 µm strips 50 µm
        // apart are 250 µm, which is not the width of either.)
        out.what = conductors > 1 ? tr("the conductors span %1 µm across").arg(wide * 1e6, 0, 'g', 4)
                                  : tr("the conductor is %1 µm across").arg(wide * 1e6, 0, 'g', 4);
    } else {
        out.transverse = deep;
        out.what = tr("the dielectrics are %1 µm deep").arg(deep * 1e6, 0, 'g', 4);
    }
    // A tenth of a wavelength in that dielectric: the engineering rule,
    // said as a rule and not as a theorem.
    if (out.transverse > 0) out.frequency = kLightSpeed / (10.0 * out.transverse * std::sqrt(out.epsilonMax));
    if (velocity > 0 && length > 0) out.resonance = velocity / (2.0 * length);
    return out;
}

} // namespace qucs_s::rf25
