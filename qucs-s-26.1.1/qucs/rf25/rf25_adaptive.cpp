/*
 * rf25_adaptive.cpp - RF 2.5D's adaptive frequency sweep and its mesh
 *                     convergence check
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "rf25_adaptive.h"

#include <QCoreApplication>
#include <QMap>
#include <QSet>

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <limits>

namespace qucs_s::rf25 {

namespace {

using C = std::complex<double>;

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::rf25", text);
}

QString tr(const char* text, const char* disambiguation, int n)
{
    return QCoreApplication::translate("qucs_s::rf25", text, disambiguation, n);
}

/// The index of \a f in \a list, to a part in 1e9; -1 where it is not.
int indexIn(const QList<double>& list, double f)
{
    for (int i = 0; i < list.size(); ++i)
        if (std::abs(list.at(i) - f) <= 1e-9 * std::max(std::abs(f), 1.0)) return i;
    return -1;
}

/// A port matrix's terms, row by row.
QList<C> flat(const QList<QList<C>>& m)
{
    QList<C> out;
    for (const QList<C>& row : m) out << row;
    return out;
}

QList<QList<C>> square(const QList<C>& terms, int n)
{
    QList<QList<C>> out;
    for (int r = 0; r < n; ++r) out << terms.mid(r * n, n);
    return out;
}

double largestDifference(const QList<QList<C>>& a, const QList<QList<C>>& b)
{
    double d = 0;
    for (int r = 0; r < std::min(a.size(), b.size()); ++r)
        for (int c = 0; c < std::min(a.at(r).size(), b.at(r).size()); ++c) d = std::max(d, std::abs(a.at(r).at(c) - b.at(r).at(c)));
    return d;
}

QString ghz(double f)
{
    return QString::number(f / 1e9, 'g', 5);
}

} // namespace

// ------------------------------------------------------------- the fit

QList<C> Rational::coefficients(double frequency) const
{
    QList<C> c(support.size(), C(0, 0));
    for (int j = 0; j < at.size(); ++j)
        if (std::abs(frequency - at.at(j)) <= 1e-12 * std::max(std::abs(frequency), 1.0)) {
            c[j] = 1;
            return c;
        }
    C sum(0, 0);
    for (int j = 0; j < at.size(); ++j) {
        c[j] = weights.at(j) / (frequency - at.at(j));
        sum += c.at(j);
    }
    if (std::abs(sum) == 0) return QList<C>(support.size(), C(0, 0));
    for (C& v : c) v /= sum;
    return c;
}

Rational fitRational(const QList<double>& frequencies, const QList<QList<C>>& values, double tolerance, int most)
{
    Rational out;
    const int n = int(std::min(frequencies.size(), values.size()));
    if (n == 0) return out;
    const int k = int(values.first().size());
    // Each quantity over its own largest magnitude, so that a small term
    // counts as much as a large one; the frequencies over the largest, for
    // the Loewner matrix's conditioning (the weights do not change with it).
    QList<double> scale(k, 1.0);
    for (int q = 0; q < k; ++q) {
        double m = 0;
        for (int i = 0; i < n; ++i) m = std::max(m, std::abs(values.at(i).value(q)));
        scale[q] = m > 0 ? m : 1.0;
    }
    double fmax = 0;
    for (int i = 0; i < n; ++i) fmax = std::max(fmax, std::abs(frequencies.at(i)));
    if (!(fmax > 0)) fmax = 1;
    Eigen::MatrixXcd g(n, std::max(k, 1));
    g.setZero();
    for (int i = 0; i < n; ++i)
        for (int q = 0; q < k; ++q) g(i, q) = values.at(i).value(q) / scale.at(q);
    Eigen::VectorXd x(n);
    for (int i = 0; i < n; ++i) x(i) = frequencies.at(i) / fmax;
    const int kk = std::max(k, 1);
    // As many support points as the rows left can determine: (n - m) k
    // equations for m weights.
    int top = std::max(1, int(std::floor(double(n) * kk / (kk + 1))));
    if (n == 1) top = 1;
    if (most > 0) top = std::min(top, most);
    top = std::min(top, n);

    QList<int> chosen;
    QList<bool> isSupport(n, false);
    Eigen::VectorXcd w;
    Eigen::MatrixXcd r(n, kk);
    for (int q = 0; q < kk; ++q) r.col(q).setConstant(g.col(q).mean());
    double residual = std::numeric_limits<double>::infinity();
    while (true) {
        // The worst sample not yet a support point.
        int worst = -1;
        double err = -1;
        for (int i = 0; i < n; ++i) {
            if (isSupport.at(i)) continue;
            const double e = (g.row(i) - r.row(i)).cwiseAbs().maxCoeff();
            if (e > err) {
                err = e;
                worst = i;
            }
        }
        if (!chosen.isEmpty()) residual = std::max(err, 0.0);
        if (worst < 0 || (!chosen.isEmpty() && err <= tolerance) || int(chosen.size()) >= top) break;
        chosen << worst;
        isSupport[worst] = true;
        const int m = int(chosen.size());
        QList<int> rest;
        for (int i = 0; i < n; ++i)
            if (!isSupport.at(i)) rest << i;
        if (rest.isEmpty()) {
            w = Eigen::VectorXcd::Ones(m);
            r = g;
            residual = 0;
            break;
        }
        Eigen::MatrixXcd a(int(rest.size()) * kk, m);
        for (int ri = 0; ri < rest.size(); ++ri)
            for (int q = 0; q < kk; ++q)
                for (int l = 0; l < m; ++l) {
                    const int i = rest.at(ri), s = chosen.at(l);
                    a(ri * kk + q, l) = (g(i, q) - g(s, q)) / (x(i) - x(s));
                }
        const Eigen::JacobiSVD<Eigen::MatrixXcd> svd(a, Eigen::ComputeFullV);
        w = svd.matrixV().col(m - 1);
        for (const int i : std::as_const(rest)) {
            C d(0, 0);
            Eigen::RowVectorXcd num = Eigen::RowVectorXcd::Zero(kk);
            for (int l = 0; l < m; ++l) {
                const C c = w(l) / (x(i) - x(chosen.at(l)));
                d += c;
                num += c * g.row(chosen.at(l));
            }
            r.row(i) = std::abs(d) > 0 ? Eigen::RowVectorXcd(num / d) : Eigen::RowVectorXcd(g.row(i));
        }
        for (const int s : std::as_const(chosen)) r.row(s) = g.row(s);
    }
    if (chosen.isEmpty()) {
        chosen << 0;
        w = Eigen::VectorXcd::Ones(1);
    }
    for (int l = 0; l < chosen.size(); ++l) {
        out.support << chosen.at(l);
        out.at << frequencies.at(chosen.at(l));
        out.weights << w(l);
    }
    out.residual = std::isfinite(residual) ? residual : 0;
    return out;
}

QList<C> rationalAt(const Rational& fit, const QList<QList<C>>& values, double frequency)
{
    const QList<C> c = fit.coefficients(frequency);
    const int k = values.isEmpty() ? 0 : int(values.first().size());
    QList<C> out(k, C(0, 0));
    for (int j = 0; j < fit.support.size(); ++j) {
        const QList<C>& v = values.at(fit.support.at(j));
        for (int q = 0; q < k; ++q) out[q] += c.at(j) * v.value(q);
    }
    return out;
}

// ------------------------------------------------------------- the sweep

void checkReciprocityAndPassivity(FullWave& wave)
{
    wave.asymmetry = wave.gain = wave.gainAt = 0;
    for (int k = 0; k < wave.z.size(); ++k) {
        const QList<QList<C>>& z = wave.z.at(k);
        const int n = int(z.size());
        Eigen::MatrixXcd zm(n, n), sm(n, n);
        for (int r = 0; r < n; ++r)
            for (int c = 0; c < n; ++c) {
                zm(r, c) = z.at(r).value(c);
                sm(r, c) = k < wave.s.size() ? wave.s.at(k).value(r).value(c) : C(0, 0);
            }
        const double largest = n > 0 ? zm.cwiseAbs().maxCoeff() : 0;
        if (largest > 0) wave.asymmetry = std::max(wave.asymmetry, (zm - zm.transpose()).cwiseAbs().maxCoeff() / largest);
        const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXcd> eig(sm.adjoint() * sm);
        if (n > 0 && eig.info() == Eigen::Success && eig.eigenvalues().maxCoeff() - 1 > wave.gain) {
            wave.gain = eig.eigenvalues().maxCoeff() - 1;
            wave.gainAt = wave.frequencies.value(k);
        }
    }
    if (wave.zSolved.isEmpty()) {
        wave.solvedAsymmetry = wave.asymmetry;
        wave.solvedGain = wave.gain;
        wave.solvedGainAt = wave.gainAt;
    }
}

Adapted adaptiveSweep(const QList<double>& sweep, const std::function<FullWave(const QList<double>&)>& solve,
                      const AdaptiveSettings& how, const std::function<bool()>& stopped)
{
    Adapted out;
    const int n = int(sweep.size());
    if (n == 0) return out;
    if (n <= std::max(how.fewest, how.first)) {
        out.wave = solve(sweep);
        out.solved = sweep;
        out.rounds = 1;
        out.whole = true;
        out.converged = true;
        out.lines << tr("The study's %1 frequencies are too few for adding points to save anything: every one is solved.")
                         .arg(n);
        if (out.wave.ok()) out.wave.lines = out.lines + out.wave.lines;
        return out;
    }

    // What each frequency solved gave, by its index in the sweep.
    QMap<int, QList<QList<C>>> zAt, sAt;
    QMap<int, QList<QList<C>>> currentsAt;
    FullWave base;
    bool haveBase = false;
    QStringList warnings;
    const auto take = [&](const QList<int>& indices) -> bool {
        QList<double> asked;
        for (const int i : indices) asked << sweep.at(i);
        FullWave w = solve(asked);
        ++out.rounds;
        if (!w.ok()) {
            out.wave = w;
            return false;
        }
        for (int k = 0; k < w.frequencies.size(); ++k) {
            const int i = indexIn(sweep, w.frequencies.at(k));
            if (i < 0) continue;
            zAt.insert(i, w.z.at(k));
            sAt.insert(i, w.s.at(k));
            currentsAt.insert(i, w.currents.value(k));
        }
        if (!haveBase) {
            base = w;
            haveBase = true;
        } else {
            // What the solves had beside their answers: the most any took,
            // and every doubt any had.
            base.compressed = base.compressed || w.compressed;
            base.memory = std::max(base.memory, w.memory);
            base.denseMemory = std::max(base.denseMemory, w.denseMemory);
            base.largestRank = std::max(base.largestRank, w.largestRank);
            base.iterations = std::max(base.iterations, w.iterations);
            base.residual = std::max(base.residual, w.residual);
            base.fillSeconds += w.fillSeconds;
            base.luSeconds += w.luSeconds;
            base.threads = std::max(base.threads, w.threads);
            for (const QString& s : std::as_const(w.warnings))
                if (!base.warnings.contains(s)) base.warnings << s;
        }
        return true;
    };
    const auto fitOf = [&](const QList<int>& indices) {
        QList<double> f;
        QList<QList<C>> v;
        for (const int i : indices) {
            f << sweep.at(i);
            v << flat(zAt.value(i));
        }
        return std::make_pair(fitRational(f, v), v);
    };
    const auto sAtFit = [&](const std::pair<Rational, QList<QList<C>>>& fit, int i) {
        const int np = int(base.portNames.size());
        return scatteringOf(square(rationalAt(fit.first, fit.second, sweep.at(i)), np), base.references);
    };
    const auto stop = [&]() {
        if (stopped && stopped()) {
            out.wave = FullWave();
            out.wave.problems << tr("Stopped, %1 frequencies solved.").arg(zAt.size());
            return true;
        }
        return false;
    };

    // The ends, and as many between, evenly.
    QList<int> solved;
    {
        const int first = std::max(2, std::min(how.first, n));
        QSet<int> at;
        for (int j = 0; j < first; ++j) at.insert(int(std::lround(double(j) * (n - 1) / (first - 1))));
        solved = QList<int>(at.begin(), at.end());
        std::sort(solved.begin(), solved.end());
    }
    if (!take(solved)) return out;
    int inARow = 0;
    double lastError = 0;
    bool midpoints = true;
    QList<int> before;   // what was solved before the last round
    while (true) {
        if (stop()) return out;
        // The frequencies to solve next: first the middle of every gap; then
        // where the fit of everything solved differs most from the fit
        // before the last round, the largest first, one a gap.
        QList<int> next;
        if (midpoints) {
            for (int j = 0; j + 1 < solved.size(); ++j)
                if (solved.at(j + 1) - solved.at(j) > 1) next << (solved.at(j) + solved.at(j + 1)) / 2;
            midpoints = false;
        } else {
            const auto now = fitOf(solved);
            const auto was = fitOf(before);
            QList<QPair<double, int>> doubt;
            for (int i = 0; i < n; ++i)
                if (!zAt.contains(i)) doubt << qMakePair(largestDifference(sAtFit(now, i), sAtFit(was, i)), i);
            std::sort(doubt.begin(), doubt.end(), [](const auto& a, const auto& b) { return a.first > b.first || (a.first == b.first && a.second < b.second); });
            QSet<int> gaps;
            for (const auto& d : std::as_const(doubt)) {
                if (int(next.size()) >= std::max(1, how.batch)) break;
                const int gap = int(std::lower_bound(solved.begin(), solved.end(), d.second) - solved.begin());
                if (gaps.contains(gap)) continue;
                gaps.insert(gap);
                next << d.second;
            }
            std::sort(next.begin(), next.end());
        }
        if (next.isEmpty()) break;
        // What the fit says of them before they are solved, to hold it to.
        const auto said = fitOf(solved);
        QList<QList<QList<C>>> foretold;
        for (const int i : std::as_const(next)) foretold << sAtFit(said, i);
        before = solved;
        if (!take(next)) return out;
        solved << next;
        std::sort(solved.begin(), solved.end());
        lastError = 0;
        bool all = true;
        for (int j = 0; j < next.size(); ++j) {
            const double e = largestDifference(foretold.at(j), sAt.value(next.at(j)));
            lastError = std::max(lastError, e);
            all = all && e <= how.tolerance;
        }
        inARow = all ? inARow + int(next.size()) : 0;
        if (inARow >= 2) break;
        if (int(solved.size()) >= n) break;
    }

    // Every frequency of the sweep: those solved as they solved, the rest
    // from the fit of them all, their currents carried by its coefficients.
    const auto fit = fitOf(solved);
    FullWave wave = base;
    wave.frequencies = sweep;
    wave.z.clear();
    wave.s.clear();
    wave.currents.clear();
    const int np = int(base.portNames.size());
    for (int i = 0; i < n; ++i) {
        if (zAt.contains(i)) {
            wave.z << zAt.value(i);
            wave.s << sAt.value(i);
            wave.currents << currentsAt.value(i);
            continue;
        }
        const QList<C> c = fit.first.coefficients(sweep.at(i));
        const QList<QList<C>> z = square(rationalAt(fit.first, fit.second, sweep.at(i)), np);
        wave.z << z;
        wave.s << scatteringOf(z, base.references);
        QList<QList<C>> perPort;
        const QList<QList<C>> first = currentsAt.value(solved.first());
        for (int p = 0; p < first.size(); ++p) {
            QList<C> column(first.at(p).size(), C(0, 0));
            for (int j = 0; j < fit.first.support.size(); ++j) {
                const QList<QList<C>>& at = currentsAt.value(solved.at(fit.first.support.at(j)));
                if (p >= at.size()) continue;
                const QList<C>& from = at.at(p);
                for (int r = 0; r < std::min(column.size(), from.size()); ++r) column[r] += c.at(j) * from.at(r);
            }
            perPort << column;
        }
        wave.currents << perPort;
    }
    for (const int i : std::as_const(solved)) out.solved << sweep.at(i);
    wave.solvedFrequencies = out.solved;
    checkReciprocityAndPassivity(wave);
    out.error = lastError;
    out.converged = inARow >= 2;
    out.degree = int(fit.first.support.size());
    QStringList at;
    for (const double f : std::as_const(out.solved)) at << ghz(f);
    if (int(solved.size()) >= n && !out.converged)
        out.lines << tr("Adding points where the answer changes took every one of the study's %1 frequencies: at those "
                        "solved last a rational fit of the rest had said S to within %2 at best, past the tolerance of "
                        "%3, so the sweep is solved whole.")
                         .arg(n).arg(lastError, 0, 'g', 2).arg(how.tolerance, 0, 'g', 2);
    else
        out.lines << tr("Adding points where the answer changes: %1 of the study's %2 frequencies solved, in %3 "
                        "rounds; the other %4 read from a rational fit of the ports' Z through them (%5 support "
                        "points), which had said S at the %6 solved last to within %7 of what they solved to, the "
                        "tolerance %8.")
                         .arg(out.solved.size()).arg(n).arg(out.rounds).arg(n - out.solved.size()).arg(out.degree)
                         .arg(inARow).arg(lastError, 0, 'g', 2).arg(how.tolerance, 0, 'g', 2);
    out.lines << tr("Solved at %1 GHz.").arg(at.join(QStringLiteral(", ")));
    wave.lines = out.lines + wave.lines;
    out.wave = wave;
    return out;
}

Calibration calibrationAt(const Calibration& calibration, const QList<double>& sweep)
{
    if (!calibration.ok() || calibration.frequencies.size() >= sweep.size()) return calibration;
    Calibration out = calibration;
    QList<QList<C>> values;
    for (int k = 0; k < calibration.frequencies.size(); ++k) {
        const std::array<C, 4>& e = calibration.discontinuity.at(k);
        values << QList<C>{e[0], e[1], e[2], e[3], calibration.gamma.value(k), calibration.impedance.value(k)};
    }
    const Rational fit = fitRational(calibration.frequencies, values, 1e-9);
    out.frequencies = sweep;
    out.discontinuity.clear();
    out.gamma.clear();
    out.impedance.clear();
    out.residual.clear();
    out.stripAt.clear();
    for (const double f : sweep) {
        const int k = calibration.indexOf(f);
        if (k >= 0) {
            out.discontinuity << calibration.discontinuity.at(k);
            out.gamma << calibration.gamma.value(k);
            out.impedance << calibration.impedance.value(k);
            out.residual << calibration.residual.value(k);
            out.stripAt << calibration.stripAt.value(k);
            continue;
        }
        const QList<C> v = rationalAt(fit, values, f);
        out.discontinuity << std::array<C, 4>{v.at(0), v.at(1), v.at(2), v.at(3)};
        out.gamma << v.at(4);
        out.impedance << v.at(5);
        int nearest = 0;
        for (int j = 1; j < calibration.frequencies.size(); ++j)
            if (std::abs(calibration.frequencies.at(j) - f) < std::abs(calibration.frequencies.at(nearest) - f)) nearest = j;
        out.residual << calibration.residual.value(nearest);
        out.stripAt << calibration.stripAt.value(nearest);
    }
    return out;
}

// ------------------------------------------------------------- convergence

Convergence convergenceOf(const QList<double>& sizes, const QList<C>& values)
{
    Convergence out;
    const int n = int(std::min(sizes.size(), values.size()));
    if (n == 0) return out;
    out.limit = values.at(n - 1);
    if (n < 3) {
        out.kind = Convergence::Kind::TwoMeshes;
        out.error = n == 2 ? std::abs(values.at(1) - values.at(0)) : 0;
        return out;
    }
    const double h1 = sizes.at(n - 3), h2 = sizes.at(n - 2), h3 = sizes.at(n - 1);
    const C v1 = values.at(n - 3), v2 = values.at(n - 2), v3 = values.at(n - 1);
    const double d1 = std::abs(v2 - v1), d2 = std::abs(v3 - v2);
    const double scale = std::max({std::abs(v3), std::abs(v2), 1e-300});
    if (d2 <= 1e-10 * scale && d1 <= 1e-10 * scale) {
        out.kind = Convergence::Kind::Settled;
        out.error = d2;
        return out;
    }
    out.error = d2;
    if (d1 <= 1e-10 * scale || !(h1 > h2 && h2 > h3 && h3 > 0)) {
        out.kind = Convergence::Kind::NotYet;
        out.ratio = d1 > 0 ? d2 / d1 : std::numeric_limits<double>::infinity();
        return out;
    }
    out.ratio = d2 / d1;
    // The order the changes shrink at, of the sizes as they are: the ratio
    // of the last change to the one before is (h2^p - h3^p) / (h1^p - h2^p),
    // which falls as p rises.
    const auto ratioAt = [h1, h2, h3](double p) {
        return (std::pow(h2, p) - std::pow(h3, p)) / (std::pow(h1, p) - std::pow(h2, p));
    };
    out.slowest = ratioAt(0.5);
    double order = 0;
    if (out.ratio < ratioAt(1e-3)) {
        double lo = 1e-3, hi = 20;
        if (out.ratio <= ratioAt(hi)) order = hi;
        else {
            for (int it = 0; it < 200; ++it) {
                const double mid = 0.5 * (lo + hi);
                if (ratioAt(mid) > out.ratio) lo = mid;
                else hi = mid;
            }
            order = 0.5 * (lo + hi);
        }
    }
    const bool turned = std::real(std::conj(v2 - v1) * (v3 - v2)) < 0;
    if (out.ratio < 1 && turned) {
        out.kind = Convergence::Kind::Oscillating;
        return out;
    }
    if (order >= 0.5) {
        out.kind = Convergence::Kind::Converging;
        out.order = order;
        out.limit = v3 + (v3 - v2) * std::pow(h3, order) / (std::pow(h2, order) - std::pow(h3, order));
        out.error = std::abs(v3 - out.limit);
        return out;
    }
    out.kind = Convergence::Kind::NotYet;
    return out;
}

// ------------------------------------------------------------- the check

namespace {

QString portLabel(const FullWave& wave, int p)
{
    return wave.portNames.value(p, QStringLiteral("P%1").arg(p + 1));
}

QString termName(int r, int c, int ports)
{
    return ports > 9 ? QStringLiteral("S(%1,%2)").arg(r + 1).arg(c + 1) : QStringLiteral("S%1%2").arg(r + 1).arg(c + 1);
}

QString valueSaid(const Watched& w, const C& v)
{
    if (w.complex)
        return QStringLiteral("%1∠%2°").arg(std::abs(v), 0, 'f', 4).arg(std::arg(v) * 180 / M_PI, 0, 'f', 2);
    const QString n = QString::number(v.real(), 'g', 5);
    return w.unit.isEmpty() ? n : n + QLatin1Char(' ') + w.unit;
}

QString errorSaid(const Watched& w, double error, const C& limit)
{
    if (w.complex || !(std::abs(limit) > 0)) return QString::number(error, 'g', 2) + (w.unit.isEmpty() ? QString() : QLatin1Char(' ') + w.unit);
    return QStringLiteral("%1 %").arg(100 * error / std::abs(limit), 0, 'g', 2);
}

QString listSaid(const QStringList& items)
{
    if (items.size() <= 1) return items.join(QString());
    return tr("%1 and %2").arg(items.mid(0, items.size() - 1).join(QStringLiteral(", ")), items.last());
}

} // namespace

QList<Watched> watchedOf(const FullWave& wave, const QList<double>& frequencies)
{
    QList<Watched> out;
    if (!wave.ok()) return out;
    const int np = int(wave.portNames.size());
    for (const double f : frequencies) {
        int k = 0;
        for (int j = 1; j < wave.frequencies.size(); ++j)
            if (std::abs(wave.frequencies.at(j) - f) < std::abs(wave.frequencies.at(k) - f)) k = j;
        for (int r = 0; r < np; ++r)
            for (int c = 0; c <= r; ++c) {
                Watched w;
                w.name = tr("%1 at %2 GHz").arg(termName(r, c, np), ghz(wave.frequencies.at(k)));
                w.complex = true;
                w.values << wave.s.at(k).value(r).value(c);
                out << w;
            }
    }
    for (int p = 0; p < std::min(np, int(wave.deembedding.size())); ++p) {
        const PortDeembedding& d = wave.deembedding.at(p);
        if (!d.calibrated) continue;
        const auto add = [&out](const QString& name, const QString& unit, double v) {
            Watched w;
            w.name = name;
            w.unit = unit;
            w.values << C(v, 0);
            out << w;
        };
        add(tr("%1's feed: eps_eff").arg(portLabel(wave, p)), QString(), d.epsEff);
        add(tr("%1's feed: Z0").arg(portLabel(wave, p)), QStringLiteral("Ohm"), d.impedance.real());
        add(tr("%1's own discontinuity: C").arg(portLabel(wave, p)), QStringLiteral("fF"), d.shuntC * 1e15);
        add(tr("%1's own discontinuity: L").arg(portLabel(wave, p)), QStringLiteral("pH"), d.seriesL * 1e12);
    }
    return out;
}

QList<Watched> watchedOf(const Extraction& extraction)
{
    QList<Watched> out;
    if (!extraction.ok()) return out;
    const int n = int(std::min(extraction.names.size(), extraction.c.size()));
    const auto add = [&out](const QString& name, const QString& unit, double v) {
        Watched w;
        w.name = name;
        w.unit = unit;
        w.values << C(v, 0);
        out << w;
    };
    for (int i = 0; i < n; ++i) add(tr("C of %1").arg(extraction.names.at(i)), QStringLiteral("fF"), extraction.c.at(i).value(i) * 1e15);
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            add(tr("C between %1 and %2").arg(extraction.names.at(i), extraction.names.at(j)), QStringLiteral("fF"),
                extraction.c.at(i).value(j) * 1e15);
    const Extraction::PerMetre& m = extraction.perMetre;
    if (m.fromMiddle && m.maxwell.size() == 1 && m.maxwell0.size() == 1 && m.maxwell.at(0).value(0) > 0
        && m.maxwell0.at(0).value(0) > 0) {
        const double cl = m.maxwell.at(0).at(0), c0l = m.maxwell0.at(0).at(0);
        add(tr("The line: eps_eff"), QString(), cl / c0l);
        add(tr("The line: Z0"), QStringLiteral("Ohm"), 1.0 / (299792458.0 * std::sqrt(cl * c0l)));
    }
    return out;
}

void addWatched(MeshConvergence& check, const QList<Watched>& watched)
{
    if (check.values.isEmpty() && check.levels.size() <= 1) {
        check.values = watched;
        return;
    }
    QList<Watched> kept;
    for (Watched w : std::as_const(check.values)) {
        bool found = false;
        for (const Watched& now : watched)
            if (now.name == w.name && !now.values.isEmpty()) {
                w.values << now.values.first();
                found = true;
                break;
            }
        if (found) kept << w;
    }
    check.values = kept;
}

void judge(MeshConvergence& check)
{
    check.lines.clear();
    QList<double> sizes;
    QStringList across, factors;
    for (const MeshLevel& l : std::as_const(check.levels)) {
        sizes << (l.factor > 0 ? 1.0 / l.factor : 1.0 / std::max(1, l.across));
        across << QString::number(l.across);
        factors << QString::number(l.factor, 'g', 3);
    }
    int converging = 0, settled = 0, oscillating = 0, notYet = 0;
    double worstS = 0, worstRelative = 0;
    for (Watched& w : check.values) {
        w.verdict = convergenceOf(sizes, w.values);
        switch (w.verdict.kind) {
        case Convergence::Kind::Converging: ++converging; break;
        case Convergence::Kind::Settled: ++settled; break;
        case Convergence::Kind::Oscillating: ++oscillating; break;
        case Convergence::Kind::NotYet: ++notYet; break;
        case Convergence::Kind::TwoMeshes: break;
        }
        // (How far the finest is from the limits: of those that have one.)
        if (w.verdict.kind == Convergence::Kind::NotYet || w.verdict.kind == Convergence::Kind::TwoMeshes) continue;
        if (w.complex) worstS = std::max(worstS, w.verdict.error);
        else if (std::abs(w.verdict.limit) > 0) worstRelative = std::max(worstRelative, w.verdict.error / std::abs(w.verdict.limit));
    }
    QStringList at;
    for (const double f : std::as_const(check.frequencies)) at << ghz(f);
    QString head = tr("Mesh convergence over %n mesh(es) of %1 cells across the narrowest conductor, each the model's own "
                      "refined as a whole - %2 times as fine, its largest cells, cells per wavelength and cells at the gaps "
                      "with it", nullptr, int(check.levels.size()))
                       .arg(listSaid(across), listSaid(factors));
    if (check.fullWave && !at.isEmpty()) head += tr(", at %1 GHz").arg(listSaid(at));
    if (check.levels.size() < 3) {
        head += tr(": two meshes say how far a value moved, not whether it is converging - a third would.");
    } else {
        QStringList counts;
        if (converging > 0) counts << tr("%n converging", nullptr, converging);
        if (settled > 0) counts << tr("%n settled", nullptr, settled);
        if (oscillating > 0) counts << tr("%n converging but turning back", nullptr, oscillating);
        if (notYet > 0) counts << tr("%n not converging yet", nullptr, notYet);
        head += tr(": of %n value(s), %1.", nullptr, int(check.values.size())).arg(listSaid(counts));
        QStringList within;
        if (worstS > 0) within << tr("S within %1").arg(worstS, 0, 'g', 2);
        if (worstRelative > 0) within << tr("the rest within %1 %").arg(100 * worstRelative, 0, 'g', 2);
        if (!within.isEmpty())
            head += tr(" The finest mesh (%1 across) has %2 of the limits, at most%3.")
                        .arg(check.levels.last().across)
                        .arg(listSaid(within))
                        .arg(notYet > 0 ? tr(" - of those converging; what is not converging yet wants a finer mesh") : QString());
    }
    check.lines << head;
    for (int i = 0; i < check.levels.size(); ++i) {
        const MeshLevel& l = check.levels.at(i);
        QString line = tr("Mesh %1: %2 across").arg(i + 1).arg(l.across);
        if (l.across != l.asked) line += tr(" (%1 asked: the solver's cells ran out)").arg(l.asked);
        line += tr(", %1 cells from %2 to %3 µm, %4 unknowns, %5 s.")
                    .arg(l.cells).arg(l.smallest, 0, 'g', 3).arg(l.largest, 0, 'g', 3).arg(l.unknowns).arg(l.seconds, 0, 'f', 1);
        check.lines << line;
    }
    for (const Watched& w : std::as_const(check.values)) {
        QStringList read;
        for (const C& v : w.values) read << valueSaid(w, v);
        const Convergence& v = w.verdict;
        QString said;
        switch (v.kind) {
        case Convergence::Kind::Converging:
            said = tr("converging as h^%1, toward %2: the finest within %3 of it")
                       .arg(v.order, 0, 'f', 1).arg(valueSaid(w, v.limit), errorSaid(w, v.error, v.limit));
            break;
        case Convergence::Kind::Settled: said = tr("settled"); break;
        case Convergence::Kind::Oscillating:
            said = tr("converging but turning back: within %1 of the finest, its last change").arg(errorSaid(w, v.error, v.limit));
            break;
        case Convergence::Kind::NotYet:
            said = std::isfinite(v.ratio) && v.ratio < 1
                       ? tr("not converging yet: its last change, %1, is %2 times the one before, where a value converging "
                            "would have it %3 at most - a finer mesh is wanted")
                             .arg(errorSaid(w, v.error, v.limit)).arg(v.ratio, 0, 'g', 2).arg(v.slowest, 0, 'g', 2)
                   : std::isfinite(v.ratio) ? tr("not converging yet: its last change, %1, is %2 times the one before")
                                                .arg(errorSaid(w, v.error, v.limit)).arg(v.ratio, 0, 'g', 2)
                                          : tr("not converging yet: it stood still, then moved by %1").arg(errorSaid(w, v.error, v.limit));
            break;
        case Convergence::Kind::TwoMeshes: said = tr("moved by %1").arg(errorSaid(w, v.error, v.limit)); break;
        }
        check.lines << tr("%1: %2 - %3.").arg(w.name, read.join(QStringLiteral(", ")), said);
    }
}

QList<int> defaultLevels(int cellsAcross)
{
    if (cellsAcross < 3) return {std::max(1, cellsAcross), std::max(1, cellsAcross) * 2, std::max(1, cellsAcross) * 4};
    QList<int> out;
    for (const int n : {std::max(1, int(std::lround(cellsAcross / 4.0))), std::max(2, int(std::lround(cellsAcross / 2.0))), cellsAcross})
        if (!out.contains(n)) out << n;
    std::sort(out.begin(), out.end());
    if (out.size() < 3) return {cellsAcross, cellsAcross * 2, cellsAcross * 4};
    return out;
}

} // namespace qucs_s::rf25
