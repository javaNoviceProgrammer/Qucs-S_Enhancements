/*
 * rf25_trl.cpp - a multiline TRL calibration of an RF 2.5D two-port
 *                (rf25_trl.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "rf25_trl.h"

#include <QCoreApplication>

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>

namespace qucs_s::rf25 {

namespace {

QString tr(const char* text, const char* disambiguation = nullptr, int n = -1)
{
    return QCoreApplication::translate("RF25Trl", text, disambiguation, n);
}

constexpr double kLight = 299792458.0;
/// Below this |sin(beta l)| of the line taken, e^-gl and e^gl are too
/// alike to be told apart well: said.
constexpr double kPoorPhase = 0.2;

using M2 = Eigen::Matrix2cd;

M2 matrixOf(const std::array<Complex, 4>& t)
{
    M2 m;
    m << t[0], t[1], t[2], t[3];
    return m;
}

std::array<Complex, 4> arrayOf(const M2& m)
{
    return {m(0, 0), m(0, 1), m(1, 0), m(1, 1)};
}

} // namespace

std::array<Complex, 4> cascadeOf(const S2& s)
{
    // [b1; a1] = T [a2; b2]: T = (1/S21) [[-det S, S11], [-S22, 1]].
    const Complex det = s[0] * s[3] - s[1] * s[2];
    return {-det / s[2], s[0] / s[2], -s[3] / s[2], 1.0 / s[2]};
}

S2 scatteringOf(const std::array<Complex, 4>& t)
{
    const Complex s21 = 1.0 / t[3], s11 = t[1] / t[3], s22 = -t[2] / t[3];
    const Complex s12 = (t[0] * t[3] - t[1] * t[2]) / t[3];
    return {s11, s12, s21, s22};
}

S2 renormalized(const S2& s, double from, double to)
{
    M2 S;
    S << s[0], s[1], s[2], s[3];
    const M2 one = M2::Identity();
    const M2 z = from * (one + S) * (one - S).inverse();
    const M2 out = (z - to * one) * (z + to * one).inverse();
    return {out(0, 0), out(0, 1), out(1, 0), out(1, 1)};
}

TrlCalibration calibrateTrl(const TrlStandards& st, double epsGuess)
{
    TrlCalibration out;
    out.frequencies = st.frequencies;
    const int nf = int(st.frequencies.size()), nl = int(st.lines.size());
    if (nf == 0) {
        out.problems << tr("A TRL calibration wants frequencies to calibrate at.");
        return out;
    }
    if (nl == 0 || st.longer.size() != nl) {
        out.problems << tr("A TRL calibration wants a line at least beside its thru, each with how much longer it is.");
        return out;
    }
    if (st.thru.size() != nf || st.reflect.size() != nf) {
        out.problems << tr("Each standard is wanted at every frequency.");
        return out;
    }
    for (int i = 0; i < nl; ++i) {
        if (st.lines.at(i).size() != nf) {
            out.problems << tr("Each standard is wanted at every frequency.");
            return out;
        }
        if (!(st.longer.at(i) > 0)) {
            out.problems << tr("Line %1 is no longer than the thru: a line of a TRL calibration is longer than it.").arg(i + 1);
            return out;
        }
    }
    double beta = 0;   // the last frequency's, to unwrap the next's phase by
    double lastF = 0;
    int poor = 0;
    double poorFrom = 0, poorTo = 0;
    for (int k = 0; k < nf; ++k) {
        const double f = st.frequencies.at(k);
        // What beta should be here: the last one's scaled, or the guess.
        const double expected = k == 0 ? 2 * M_PI * f * std::sqrt(std::max(1.0, epsGuess)) / kLight : beta * f / lastF;
        const M2 thru = matrixOf(cascadeOf(st.thru.at(k)));
        const M2 thruInverse = thru.inverse();
        // Each line: its two eigenvalues, e^-gl and e^gl, and so its gamma.
        QList<Complex> gammas;
        QList<double> weights;
        QList<M2> forA, forB;
        for (int i = 0; i < nl; ++i) {
            const M2 line = matrixOf(cascadeOf(st.lines.at(i).at(k)));
            const M2 a = line * thruInverse, b = thruInverse * line;
            forA << a;
            forB << b;
            const Eigen::ComplexEigenSolver<M2> es(a);
            const double l = st.longer.at(i);
            // Of the two, the one that is e^-gl: its -ln / l, unwrapped to
            // the beta expected, with a loss not below nought.
            Complex best;
            double off = 1e300;
            for (int e = 0; e < 2; ++e) {
                const Complex g0 = -std::log(es.eigenvalues()(e)) / l;
                const double turns = std::round((expected - g0.imag()) * l / (2 * M_PI));
                const Complex g(g0.real(), g0.imag() + 2 * M_PI * turns / l);
                const double o = std::abs(g.imag() - expected) + (g.real() < 0 ? std::abs(g.real()) * 10 : 0.0);
                if (o < off) {
                    off = o;
                    best = g;
                }
            }
            gammas << best;
            weights << std::abs(std::sin(best.imag() * l));
        }
        // gamma from every line, each weighted by how well its phase tells
        // its two eigenvalues apart and by its length; the eigenvectors from
        // the best told.
        Complex sum = 0;
        double total = 0;
        int taken = 0;
        for (int i = 0; i < nl; ++i) {
            const double w = std::pow(weights.at(i) * st.longer.at(i), 2);
            sum += w * gammas.at(i);
            total += w;
            if (weights.at(i) > weights.at(taken) + 1e-12) taken = i;
        }
        const Complex gamma = total > 0 ? sum / total : gammas.at(taken);
        beta = gamma.imag();
        lastF = f;
        const double l = st.longer.at(taken);
        const Complex forward = std::exp(-gamma * l);
        // A's columns: the eigenvector of e^-gl, then of e^gl.
        const Eigen::ComplexEigenSolver<M2> ea(forA.at(taken));
        const int first = std::abs(ea.eigenvalues()(0) - forward) <= std::abs(ea.eigenvalues()(1) - forward) ? 0 : 1;
        const Eigen::Vector2cd v1 = ea.eigenvectors().col(first), v2 = ea.eigenvectors().col(1 - first);
        // B's rows: the left eigenvectors, of the transpose.
        const Eigen::ComplexEigenSolver<M2> eb(forB.at(taken).transpose());
        const int firstB = std::abs(eb.eigenvalues()(0) - forward) <= std::abs(eb.eigenvalues()(1) - forward) ? 0 : 1;
        const Eigen::Vector2cd w1 = eb.eigenvectors().col(firstB), w2 = eb.eigenvectors().col(1 - firstB);
        const Complex r1 = v1(0) / v1(1), r2 = v2(0) / v2(1), q1 = w1(0) / w1(1), q2 = w2(0) / w2(1);
        const Complex e00 = r2, a = e00 - r1;          // a = e01 e10 / e11
        const Complex e33 = -q2, b = q1 - q2;          // b = e23 e32 / e22
        // The thru: e11 e22, from each end, averaged.
        const S2& t = st.thru.at(k);
        const Complex x1 = (t[0] - e00) / (a + t[0] - e00), x2 = (t[3] - e33) / (b + t[3] - e33);
        const Complex x = 0.5 * (x1 + x2);
        // The reflect: e11 G and e22 G, and so e11 and the sign of it.
        const S2& r = st.reflect.at(k);
        const Complex u = (r[0] - e00) / (a + r[0] - e00), v = (r[3] - e33) / (b + r[3] - e33);
        Complex e11 = std::sqrt(x * u / v);
        Complex reflection = u / e11;
        const double expect = st.shortReflect ? -1.0 : 1.0;
        if ((reflection * expect).real() < 0) {
            e11 = -e11;
            reflection = -reflection;
        }
        const Complex e22 = x / e11;
        const Complex e10e32 = t[2] * (1.0 - x);
        out.gamma << gamma;
        out.line << taken;
        out.phase << weights.at(taken);
        out.reflect << reflection;
        out.terms << std::array<Complex, 7>{e00, e11, a * e11, e22, e33, b * e22, e10e32};
        if (weights.at(taken) < kPoorPhase) {
            if (poor++ == 0) poorFrom = f;
            poorTo = f;
        }
    }
    // The lines not taken, corrected: how near a matched line each is.
    for (int k = 0; k < nf; ++k) {
        double worst = 0;
        for (int i = 0; i < nl; ++i) {
            if (i == out.line.at(k)) continue;
            TrlCalibration one;
            one.terms = {out.terms.at(k)};
            const S2 c = corrected(one, {st.lines.at(i).at(k)}).value(0);
            worst = std::max({worst, std::abs(c[0]), std::abs(c[3])});
        }
        out.residual << worst;
    }
    const auto ghz = [](double f) { return QString::number(f / 1e9, 'g', 4); };
    const double eps = std::pow(out.gamma.last().imag() * kLight / (2 * M_PI * st.frequencies.last()), 2);
    out.lines << tr("A TRL calibration of a thru, %n line(s) and a reflect (%1) from %2 to %3 GHz: the lines' effective "
                    "permittivity %4 at the top, their loss %5 dB/mm there.", nullptr, nl)
                     .arg(st.shortReflect ? tr("a short") : tr("an open"), ghz(st.frequencies.first()), ghz(st.frequencies.last()))
                     .arg(eps, 0, 'g', 4)
                     .arg(8.685889638 * out.gamma.last().real() / 1000, 0, 'g', 3);
    // (What the reflect was found to be, there: an open's angle near nought,
    //  a short's near 180 degrees - the reflect taken for the other is
    //  invisible in a symmetric device's S, both boxes' signs turned.)
    out.lines << tr("The reflect found to reflect %1 at %2 degrees at the top.")
                     .arg(std::abs(out.reflect.last()), 0, 'g', 3)
                     .arg(std::arg(out.reflect.last()) * 180 / M_PI, 0, 'f', 1);
    if (nl > 1) {
        double most = 0;
        for (const double r : std::as_const(out.residual)) most = std::max(most, r);
        out.lines << tr("Each frequency's error boxes from the line whose phase beside the thru's is furthest from 0 and 180 "
                        "degrees; the others corrected come within %1 of a matched line.")
                         .arg(most, 0, 'g', 2);
    }
    if (poor > 0)
        out.warnings << tr("From %1 to %2 GHz (%3 frequencies) no line's phase beside the thru's is further than %4 from 0 or "
                           "180 degrees: e^-gl and e^gl hardly differ there and the error boxes are poorly told - add a line "
                           "of another length.")
                            .arg(ghz(poorFrom), ghz(poorTo))
                            .arg(poor)
                            .arg(std::asin(kPoorPhase) * 180 / M_PI, 0, 'f', 0);
    for (int k = 0; k < nf; ++k)
        if (std::abs(std::abs(out.reflect.at(k)) - 1) > 0.5) {
            out.warnings << tr("At %1 GHz the reflect reflects %2 of what comes in, far from all: is it an open or a short?")
                                .arg(ghz(st.frequencies.at(k)))
                                .arg(std::abs(out.reflect.at(k)), 0, 'g', 2);
            break;
        }
    return out;
}

QList<S2> corrected(const TrlCalibration& c, const QList<S2>& measured)
{
    QList<S2> out;
    for (int k = 0; k < measured.size() && k < c.terms.size(); ++k) {
        const std::array<Complex, 4> tm = cascadeOf(measured.at(k));
        const auto& e = c.terms.at(k);
        const Complex e00 = e[0], e11 = e[1], e01e10 = e[2], e22 = e[3], e33 = e[4], e23e32 = e[5], e10e32 = e[6];
        // T_A e10 and T_B e32: what of the two boxes is known; the scale
        // they share is e10 e32.
        M2 ka, kb;
        ka << -(e00 * e11 - e01e10), e00, -e11, 1.0;
        kb << -(e22 * e33 - e23e32), e22, -e33, 1.0;
        const M2 td = e10e32 * (ka.inverse() * matrixOf(tm) * kb.inverse());
        out << scatteringOf(arrayOf(td));
    }
    return out;
}

} // namespace qucs_s::rf25
