/*
 * fem_export.cpp - a multiphysics model's results as parts of circuits:
 *                  Touchstone files, SPICE and Qucs subcircuits; its
 *                  fields on a grid (CSV, NumPy, columns)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_export.h"

#include <QCoreApplication>

#include <Eigen/Dense>

#include <cmath>
#include <cstring>

namespace qucs_s::fem {

namespace {

using cd = std::complex<double>;

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::fem::Export", text);
}

/// A number as SPICE and Touchstone take it: plain, 12 digits.
QString num(double v)
{
    return QString::number(v, 'g', 12);
}

/// A name SPICE takes: letters, digits and _.
QString spiceName(QString name)
{
    for (QChar& c : name)
        if (!c.isLetterOrNumber() && c != QLatin1Char('_')) c = QLatin1Char('_');
    if (name.isEmpty()) name = QStringLiteral("fem");
    return name;
}

QString commentLines(const QStringList& comments, const QString& mark)
{
    QString out;
    for (const QString& c : comments)
        for (const QString& line : c.split(QLatin1Char('\n'))) out += mark + QLatin1Char(' ') + line + QLatin1Char('\n');
    return out;
}

} // namespace

LineSweep lineSweep(const SolutionSet& set, const Node& lineParameters, int point)
{
    LineSweep out;
    if (set.solutions.empty()) {
        out.error = tr("nothing is solved");
        return out;
    }
    const int n = int(set.solutions.size());
    const int p = point <= 0 || point > n ? n - 1 : point - 1;
    Solution& s = *set.solutions[std::size_t(p)];
    const int snaps = s.snapshotCount();
    const int was = s.selected();
    for (int k = 0; k < std::max(1, snaps); ++k) {
        if (snaps > 0) s.select(k);
        LineRLGC l = lineRLGC(s, lineParameters);
        if (!l.error.isEmpty()) {
            out.error = l.error;
            break;
        }
        out.points.push_back(l);
    }
    if (snaps > 0) s.select(was);
    std::sort(out.points.begin(), out.points.end(), [](const LineRLGC& a, const LineRLGC& b) { return a.frequency < b.frequency; });
    return out;
}

std::array<cd, 4> lineSParameters(const LineRLGC& line, double length, double zref)
{
    // ABCD of the line: cosh γℓ, Zc sinh γℓ; sinh γℓ / Zc, cosh γℓ.
    const cd gl = line.gamma * length;
    const cd A = std::cosh(gl), B = line.Zc * std::sinh(gl), C = std::sinh(gl) / line.Zc, D = A;
    const cd den = A + B / zref + C * zref + D;
    const cd s11 = (A + B / zref - C * zref - D) / den;
    const cd s12 = 2.0 * (A * D - B * C) / den;
    const cd s21 = 2.0 / den;
    const cd s22 = (-A + B / zref - C * zref + D) / den;
    return {s11, s21, s12, s22};
}

QString touchstone(const LineSweep& sweep, double length, double zref, const QStringList& comments)
{
    QString out = commentLines(comments, QStringLiteral("!"));
    out += QStringLiteral("! A transmission line %1 long, as a two-port (port 1 at its start)\n")
               .arg(formatQuantity(length, Dim::length()));
    out += QStringLiteral("! f  S11 (re, im)  S21  S12  S22\n");
    out += QStringLiteral("# Hz S RI R %1\n").arg(num(zref));
    for (const LineRLGC& l : sweep.points) {
        const std::array<cd, 4> s = lineSParameters(l, length, zref);
        out += num(l.frequency);
        for (const cd& v : s) out += QLatin1Char(' ') + num(v.real()) + QLatin1Char(' ') + num(v.imag());
        out += QLatin1Char('\n');
    }
    return out;
}

QString touchstonePorts(const SolutionSet& set, const QString& physics, int point, const QStringList& comments, int* ports, QString* error)
{
    if (set.solutions.empty()) {
        if (error) *error = tr("nothing is solved");
        return {};
    }
    const int n = int(set.solutions.size());
    Solution& s = *set.solutions[std::size_t(point <= 0 || point > n ? n - 1 : point - 1)];
    const int fi = s.fieldIndex(physics);
    if (fi < 0 || s.fields()[std::size_t(fi)].type != QLatin1String("waves")) {
        if (error) *error = tr("there is no electromagnetic waves (frequency domain) %1 solved").arg(physics);
        return {};
    }
    QStringList names;
    for (const Field::Terminal& t : s.fields()[std::size_t(fi)].terminals) names << t.name;
    const int np = int(names.size());
    if (np == 0) {
        if (error) *error = tr("%1 has no ports").arg(physics);
        return {};
    }
    if (ports) *ports = np;
    const QString p = physics + QLatin1Char('.');
    auto entry = [&](const QString& a, const QString& b) -> std::optional<cd> {
        const bool brief = a.size() == 1 && b.size() == 1;
        const QString base = brief ? p + QStringLiteral("S") + a + b : p + QStringLiteral("S_") + a + QLatin1Char('_') + b;
        const std::optional<double> re = s.global(base), im = s.global(base + QStringLiteral("_im"));
        if (!re || !im || !std::isfinite(*re)) return std::nullopt;
        return cd(*re, *im);
    };
    QString out = commentLines(comments, QStringLiteral("!"));
    out += QStringLiteral("! The S-matrix of %1's ports %2, each normalized to its own mode's wave impedance\n").arg(physics, names.join(QStringLiteral(", ")));
    out += QStringLiteral("# Hz S RI R 50\n");
    const int was = s.selected();
    const int snaps = std::max(1, s.snapshotCount());
    for (int k = 0; k < snaps; ++k) {
        if (s.snapshotCount() > 0) s.select(k);
        QStringList cells;
        // Two ports: S11 S21 S12 S22; else by rows.
        std::vector<std::pair<int, int>> order;
        if (np == 2) order = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
        else
            for (int i = 0; i < np; ++i)
                for (int j = 0; j < np; ++j) order.push_back({i, j});
        QString line = num(s.frequency());
        int onLine = 0;
        for (const auto& [i, j] : order) {
            const std::optional<cd> v = entry(names[i], names[j]);
            if (!v) {
                if (error) *error = tr("%1: the S-parameter %2 %3 is not there - feed each port (its Fed on)").arg(physics, names[i], names[j]);
                if (s.snapshotCount() > 0) s.select(was);
                return {};
            }
            if (np > 2 && onLine == 4) {
                out += line + QLatin1Char('\n');
                line = QString();
                onLine = 0;
            }
            line += QLatin1Char(' ') + num(v->real()) + QLatin1Char(' ') + num(v->imag());
            ++onLine;
            if (np > 2 && j == np - 1) {
                out += line + QLatin1Char('\n');
                line = QString();
                onLine = 0;
            }
        }
        if (!line.isEmpty()) out += line + QLatin1Char('\n');
    }
    if (s.snapshotCount() > 0) s.select(was);
    return out;
}

QString spiceLine(const LineRLGC& line, double length, const QString& name, int sections, bool ltra, const QStringList& comments)
{
    QString out = commentLines(comments, QStringLiteral("*"));
    out += QStringLiteral("* A transmission line %1 long: R %2, L %3, G %4, C %5 per metre (at %6)\n")
               .arg(formatQuantity(length, Dim::length()), formatQuantity(line.R, Dim::resistance() * Dim::of(-1)),
                    formatQuantity(line.L, Dim::of(2, 1, -2, -2) * Dim::of(-1)), formatQuantity(line.G, Dim::conductance() * Dim::of(-1)),
                    formatQuantity(line.C, Dim::capacitance() * Dim::of(-1)), formatQuantity(line.frequency, Dim::of(0, 0, -1)));
    const QString sub = spiceName(name);
    out += QStringLiteral(".subckt %1 in out ref\n").arg(sub);
    if (ltra) {
        out += QStringLiteral("O1 in ref out ref %1_ltra\n").arg(sub);
        out += QStringLiteral(".model %1_ltra LTRA R=%2 L=%3 G=%4 C=%5 LEN=%6\n")
                   .arg(sub, num(line.R), num(line.L), num(line.G), num(line.C), num(length));
    } else {
        const int n = std::max(1, sections);
        const double dl = length / n;
        QString prev = QStringLiteral("in");
        for (int i = 1; i <= n; ++i) {
            const QString mid = QStringLiteral("a%1").arg(i);
            const QString next = i == n ? QStringLiteral("out") : QStringLiteral("n%1").arg(i);
            if (line.R > 0) {
                out += QStringLiteral("R%1 %2 %3 %4\n").arg(i).arg(prev, mid, num(line.R * dl));
                out += QStringLiteral("L%1 %2 %3 %4\n").arg(i).arg(mid, next, num(line.L * dl));
            } else {
                out += QStringLiteral("L%1 %2 %3 %4\n").arg(i).arg(prev, next, num(line.L * dl));
            }
            out += QStringLiteral("C%1 %2 ref %3\n").arg(i).arg(next, num(line.C * dl));
            if (line.G > 0) out += QStringLiteral("RG%1 %2 ref %3\n").arg(i).arg(next, num(1 / (line.G * dl)));
            prev = next;
        }
    }
    out += QStringLiteral(".ends %1\n").arg(sub);
    return out;
}

CoupledSweep coupledSweep(const SolutionSet& set, const Node& lineParameters, int point)
{
    CoupledSweep out;
    if (set.solutions.empty()) {
        out.error = tr("nothing is solved");
        return out;
    }
    const int n = int(set.solutions.size());
    Solution& s = *set.solutions[std::size_t(point <= 0 || point > n ? n - 1 : point - 1)];
    const int snaps = s.snapshotCount();
    const int was = s.selected();
    for (int k = 0; k < std::max(1, snaps); ++k) {
        if (snaps > 0) s.select(k);
        CoupledLines l = coupledLines(s, lineParameters);
        if (!l.error.isEmpty()) {
            out.error = l.error;
            break;
        }
        out.points.push_back(l);
    }
    if (snaps > 0) s.select(was);
    std::sort(out.points.begin(), out.points.end(), [](const CoupledLines& a, const CoupledLines& b) { return a.frequency < b.frequency; });
    return out;
}

std::vector<std::vector<cd>> coupledSParameters(const CoupledLines& lines, double length, double zref)
{
    // By the modes: V = Tv (e^-γz a + e^γz b), I = Ti (e^-γz a - e^γz b),
    // Ti's columns each mode's currents (Y v / γ). The near ends' and far
    // ends' currents into the line: Y11 = Ti coth(γℓ) Tv⁻¹, Y12 = -Ti
    // csch(γℓ) Tv⁻¹, Y21 = Y12, Y22 = Y11; S = (1 + zref Y)⁻¹ (1 - zref Y).
    const int n = int(lines.modes.size());
    Eigen::MatrixXcd Tv(n, n), Ti(n, n);
    Eigen::VectorXcd coth(n), csch(n);
    for (int k = 0; k < n; ++k) {
        const LineMode& m = lines.modes[std::size_t(k)];
        for (int j = 0; j < n; ++j) {
            Tv(j, k) = m.v[std::size_t(j)];
            Ti(j, k) = m.i[std::size_t(j)];
        }
        // (e^-2γℓ: no overflow of a long lossy line's.)
        const cd e1 = std::exp(-m.gamma * length), e2 = e1 * e1;
        coth[k] = (1.0 + e2) / (1.0 - e2);
        csch[k] = 2.0 * e1 / (1.0 - e2);
    }
    const Eigen::MatrixXcd inv = Tv.inverse();
    const Eigen::MatrixXcd y11 = Ti * coth.asDiagonal() * inv, y12 = -(Ti * csch.asDiagonal() * inv);
    Eigen::MatrixXcd Y(2 * n, 2 * n);
    Y << y11, y12, y12, y11;
    const Eigen::MatrixXcd one = Eigen::MatrixXcd::Identity(2 * n, 2 * n);
    const Eigen::MatrixXcd S = (one + zref * Y).partialPivLu().solve(one - zref * Y);
    std::vector<std::vector<cd>> out(std::size_t(2 * n), std::vector<cd>(std::size_t(2 * n)));
    for (int i = 0; i < 2 * n; ++i)
        for (int j = 0; j < 2 * n; ++j) out[std::size_t(i)][std::size_t(j)] = S(i, j);
    return out;
}

std::vector<std::vector<cd>> mixedModeSParameters(const std::vector<std::vector<cd>>& s)
{
    // d = (a1 - a2)/√2 at an end, c = (a1 + a2)/√2: M S Mᵀ, M orthogonal.
    const double h = 1 / std::sqrt(2.0);
    Eigen::Matrix4d M;
    M << h, -h, 0, 0, 0, 0, h, -h, h, h, 0, 0, 0, 0, h, h;
    Eigen::Matrix4cd S;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) S(i, j) = s[std::size_t(i)][std::size_t(j)];
    const Eigen::Matrix4cd mm = M.cast<cd>() * S * M.transpose().cast<cd>();
    std::vector<std::vector<cd>> out(4, std::vector<cd>(4));
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[std::size_t(i)][std::size_t(j)] = mm(i, j);
    return out;
}

QString touchstoneCoupled(const CoupledSweep& sweep, double length, double zref, bool mixed, const QStringList& comments)
{
    QString out = commentLines(comments, QStringLiteral("!"));
    const int n = sweep.points.empty() ? 0 : int(sweep.points.front().names.size());
    out += QStringLiteral("! %1 coupled transmission lines %2 long (%3), as a %4-port\n")
               .arg(n)
               .arg(formatQuantity(length, Dim::length()), sweep.points.empty() ? QString() : sweep.points.front().names.join(QStringLiteral(", ")))
               .arg(2 * n);
    if (mixed) {
        out += QStringLiteral("! Mixed-mode: ports d1 d2 c1 c2 - the differential mode at the near and far ends (its reference %1), "
                              "the common mode (%2); [Sdd Sdc; Scd Scc]\n")
                   .arg(formatQuantity(2 * zref, Dim::resistance()), formatQuantity(zref / 2, Dim::resistance()));
    } else {
        QStringList ports;
        const QStringList names = sweep.points.empty() ? QStringList() : sweep.points.front().names;
        for (int k = 0; k < n; ++k) ports << QStringLiteral("%1 line %2's near end").arg(k + 1).arg(names[k]);
        for (int k = 0; k < n; ++k) ports << QStringLiteral("%1 line %2's far end").arg(n + k + 1).arg(names[k]);
        out += QStringLiteral("! Ports: %1\n").arg(ports.join(QStringLiteral(", ")));
    }
    out += QStringLiteral("! f, then the S-matrix by rows (re, im), four to a line\n");
    out += QStringLiteral("# Hz S RI R %1\n").arg(num(zref));
    for (const CoupledLines& l : sweep.points) {
        std::vector<std::vector<cd>> s = coupledSParameters(l, length, zref);
        if (mixed) s = mixedModeSParameters(s);
        const int np = int(s.size());
        for (int i = 0; i < np; ++i) {
            QString line = i == 0 ? num(l.frequency) : QString();
            for (int j = 0; j < np; ++j) {
                if (j > 0 && j % 4 == 0) {
                    out += line + QLatin1Char('\n');
                    line.clear();
                }
                line += QLatin1Char(' ') + num(s[std::size_t(i)][std::size_t(j)].real()) + QLatin1Char(' ') + num(s[std::size_t(i)][std::size_t(j)].imag());
            }
            out += line + QLatin1Char('\n');
        }
    }
    return out;
}

QString spiceCoupled(const CoupledLines& lines, double length, const QString& name, int sections, const QStringList& comments)
{
    const int n = int(lines.names.size());
    const std::size_t N = std::size_t(n);
    QString out = commentLines(comments, QStringLiteral("*"));
    out += QStringLiteral("* %1 coupled transmission lines (%2) %3 long, per metre at %4 - the matrices by rows:\n")
               .arg(n)
               .arg(lines.names.join(QStringLiteral(", ")), formatQuantity(length, Dim::length()), formatQuantity(lines.frequency, Dim::of(0, 0, -1)));
    auto rows = [&](const QString& letter, const CoupledLines::Matrix& m, const Dim& dim) {
        QStringList r;
        for (std::size_t i = 0; i < N; ++i) {
            QStringList row;
            for (std::size_t j = 0; j < N; ++j) row << formatQuantity(m[i][j], dim);
            r << row.join(QStringLiteral(", "));
        }
        out += QStringLiteral("* %1: %2\n").arg(letter, r.join(QStringLiteral("; ")));
    };
    const Dim perMetre = Dim::of(-1);
    rows(QStringLiteral("R"), lines.R, Dim::resistance() * perMetre);
    rows(QStringLiteral("L"), lines.L, Dim::of(2, 1, -2, -2) * perMetre);
    rows(QStringLiteral("G"), lines.G, Dim::conductance() * perMetre);
    rows(QStringLiteral("C (Maxwell's: off the diagonal, minus the capacitance between)"), lines.C, Dim::capacitance() * perMetre);
    const QString sub = spiceName(name);
    QStringList ins, outs;
    for (const QString& c : lines.names) {
        ins << QStringLiteral("in_") + spiceName(c);
        outs << QStringLiteral("out_") + spiceName(c);
    }
    out += QStringLiteral(".subckt %1 %2 %3 ref\n").arg(sub, ins.join(QLatin1Char(' ')), outs.join(QLatin1Char(' ')));
    const int ns = std::max(1, sections);
    const double dl = length / ns;
    double largestC = 0, largestG = 0;
    for (std::size_t i = 0; i < N; ++i)
        for (std::size_t j = 0; j < N; ++j) {
            largestC = std::max(largestC, std::abs(lines.C[i][j]));
            largestG = std::max(largestG, std::abs(lines.G[i][j]));
        }
    std::vector<QString> prev(N);
    for (std::size_t i = 0; i < N; ++i) prev[i] = ins[int(i)];
    for (int k = 1; k <= ns; ++k) {
        std::vector<QString> next(N);
        for (std::size_t i = 0; i < N; ++i) next[i] = k == ns ? outs[int(i)] : QStringLiteral("n%1_%2").arg(k).arg(spiceName(lines.names[int(i)]));
        // Each line: a source of none (its current), R, the others'
        // currents' mutual R (H), L.
        for (std::size_t i = 0; i < N; ++i) {
            const QString c = spiceName(lines.names[int(i)]);
            const QString a = QStringLiteral("a%1_%2").arg(k).arg(c);
            out += QStringLiteral("V%1_%2 %3 %4 0\n").arg(k).arg(c, prev[i], a);
            QString at = a;
            if (lines.R[i][i] != 0) {
                const QString b = QStringLiteral("b%1_%2").arg(k).arg(c);
                out += QStringLiteral("R%1_%2 %3 %4 %5\n").arg(k).arg(c, at, b, num(lines.R[i][i] * dl));
                at = b;
            }
            for (std::size_t j = 0; j < N; ++j) {
                if (j == i || lines.R[i][j] == 0) continue;
                const QString o = spiceName(lines.names[int(j)]);
                const QString b = QStringLiteral("h%1_%2_%3").arg(k).arg(c, o);
                out += QStringLiteral("H%1_%2_%3 %4 %5 V%1_%3 %6\n").arg(k).arg(c, o, at, b, num(lines.R[i][j] * dl));
                at = b;
            }
            out += QStringLiteral("L%1_%2 %3 %4 %5\n").arg(k).arg(c, at, next[i], num(lines.L[i][i] * dl));
        }
        for (std::size_t i = 0; i < N; ++i)
            for (std::size_t j = i + 1; j < N; ++j) {
                const double kk = lines.L[i][j] / std::sqrt(lines.L[i][i] * lines.L[j][j]);
                if (!std::isfinite(kk) || std::abs(kk) < 1e-9) continue;
                out += QStringLiteral("K%1_%2_%3 L%1_%2 L%1_%3 %4\n")
                           .arg(k)
                           .arg(spiceName(lines.names[int(i)]), spiceName(lines.names[int(j)]), num(std::clamp(kk, -1.0, 1.0)));
            }
        // Across: each line to ref (its row's sum), between two (minus their entry).
        for (std::size_t i = 0; i < N; ++i) {
            const QString c = spiceName(lines.names[int(i)]);
            double cref = 0, gref = 0;
            for (std::size_t j = 0; j < N; ++j) {
                cref += lines.C[i][j];
                gref += lines.G[i][j];
            }
            if (cref > 1e-9 * largestC) out += QStringLiteral("C%1_%2 %3 ref %4\n").arg(k).arg(c, next[i], num(cref * dl));
            if (gref > 1e-9 * largestG) out += QStringLiteral("RG%1_%2 %3 ref %4\n").arg(k).arg(c, next[i], num(1 / (gref * dl)));
            for (std::size_t j = i + 1; j < N; ++j) {
                const QString o = spiceName(lines.names[int(j)]);
                if (-lines.C[i][j] > 1e-9 * largestC) out += QStringLiteral("C%1_%2_%3 %4 %5 %6\n").arg(k).arg(c, o, next[i], next[j], num(-lines.C[i][j] * dl));
                if (-lines.G[i][j] > 1e-9 * largestG)
                    out += QStringLiteral("RG%1_%2_%3 %4 %5 %6\n").arg(k).arg(c, o, next[i], next[j], num(1 / (-lines.G[i][j] * dl)));
            }
        }
        prev = next;
    }
    out += QStringLiteral(".ends %1\n").arg(sub);
    return out;
}

QString qucsCoupledSchematic(const CoupledLines& lines, double length, const QString& version, const QString& name, QString* error, bool clip)
{
    if (!lines.symmetricPair || lines.modes.size() != 2) {
        if (error)
            *error = tr("a Qucs subcircuit of coupled lines is a symmetric pair's (a coupled line part of its even and odd modes): "
                        "%1 lines not so - export them as Touchstone or SPICE")
                         .arg(lines.names.size());
        return {};
    }
    const LineMode& e = lines.modes[0];
    const LineMode& o = lines.modes[1];
    auto ohm = [](double v) { return num(v) + QStringLiteral(" Ohm"); };
    // (Its attenuation in dB per metre: the part's "dB" a power ratio per
    // metre.)
    const QString part = QStringLiteral("<CTLIN Line1 1 200 100 -26 34 0 0 \"%1\" 1 \"%2\" 1 \"%3\" 1 \"%4\" 1 \"%5\" 1 \"%6 dB\" 1 \"%7 dB\" 1 \"26.85\" 0>\n")
                             .arg(ohm(e.Z.real()), ohm(o.Z.real()), num(length), num(e.nEff * e.nEff), num(o.nEff * o.nEff), num(e.alphaDb),
                                  num(o.alphaDb));
    if (clip)
        return QStringLiteral("<Qucs Schematic %1>\n<Components>\n  %2</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n")
            .arg(version, part);
    QString out = QStringLiteral("<Qucs Schematic %1>\n").arg(version);
    out += QStringLiteral("<Properties>\n  <View=0,0,400,240,1,0,0>\n  <Grid=10,10,1>\n  <DataSet=%1.dat>\n  <DataDisplay=%1.dpl>\n"
                          "  <OpenDisplay=0>\n  <Script=%1.m>\n  <RunScript=0>\n  <showFrame=0>\n  <FrameText0=%2>\n"
                          "  <FrameText1=Drawn By:>\n  <FrameText2=Date:>\n  <FrameText3=Revision:>\n</Properties>\n")
               .arg(name, tr("Coupled transmission lines from a multiphysics model"));
    out += QStringLiteral("<Symbol>\n  <.ID -20 34 SUB>\n  <Line -20 -20 40 0 #000080 2 1>\n  <Line 20 -20 0 40 #000080 2 1>\n"
                          "  <Line -20 20 40 0 #000080 2 1>\n  <Line -20 -20 0 40 #000080 2 1>\n  <Line -30 -10 10 0 #000080 2 1>\n"
                          "  <Line -30 10 10 0 #000080 2 1>\n  <Line 20 -10 10 0 #000080 2 1>\n  <Line 20 10 10 0 #000080 2 1>\n"
                          "  <Line -16 -10 32 0 #000080 1 1>\n  <Line -16 10 32 0 #000080 1 1>\n"
                          "  <.PortSym -30 -10 1 0>\n  <.PortSym -30 10 2 0>\n  <.PortSym 30 -10 3 0>\n  <.PortSym 30 10 4 0>\n</Symbol>\n");
    // Its ports: 1, 2 the lines' near ends, 3, 4 their far ends (the part's
    // pins: line 1 left and right at the top, line 2 right and left below).
    out += QStringLiteral("<Components>\n  <Port P1 1 100 40 -23 12 0 0 \"1\" 1 \"analog\" 0>\n"
                          "  <Port P2 1 100 160 -23 12 0 0 \"2\" 1 \"analog\" 0>\n"
                          "  <Port P3 1 300 40 4 12 1 2 \"3\" 1 \"analog\" 0>\n"
                          "  <Port P4 1 300 160 4 12 1 2 \"4\" 1 \"analog\" 0>\n  %1</Components>\n")
               .arg(part);
    out += QStringLiteral("<Wires>\n  <100 40 170 40 \"\" 0 0 0 \"\">\n  <170 40 170 90 \"\" 0 0 0 \"\">\n"
                          "  <100 160 170 160 \"\" 0 0 0 \"\">\n  <170 160 170 110 \"\" 0 0 0 \"\">\n"
                          "  <230 40 300 40 \"\" 0 0 0 \"\">\n  <230 40 230 90 \"\" 0 0 0 \"\">\n"
                          "  <230 160 300 160 \"\" 0 0 0 \"\">\n  <230 160 230 110 \"\" 0 0 0 \"\">\n</Wires>\n");
    out += QStringLiteral("<Diagrams>\n</Diagrams>\n<Paintings>\n  <Text 100 190 10 #000000 0 \"%1\">\n</Paintings>\n")
               .arg(tr("The even and odd modes at %1 (Z0 their real parts: %2, %3 Ohm)")
                        .arg(formatQuantity(lines.frequency, Dim::of(0, 0, -1)), num(e.Z.real()), num(o.Z.real())));
    return out;
}

QString spiceMatrix(const Solution& solution, const QString& physics, const QString& name, const QStringList& comments, QString* error)
{
    const int fi = solution.fieldIndex(physics);
    if (fi < 0) {
        if (error) *error = tr("there is no physics %1 solved").arg(physics);
        return {};
    }
    const Field& f = solution.fields()[std::size_t(fi)];
    const QString sub = spiceName(name);
    QString out = commentLines(comments, QStringLiteral("*"));
    if (f.type == QLatin1String("electrostatics") || f.type == QLatin1String("currents")) {
        const int n = int(f.matrix.size());
        if (n == 0) {
            if (error) *error = tr("%1 has no matrix: give it terminals and keep the study's terminal sweep on").arg(physics);
            return {};
        }
        const bool caps = f.type == QLatin1String("electrostatics");
        QStringList pins;
        for (int i = 0; i < n; ++i) pins << QStringLiteral("t") + spiceName(f.terminals[std::size_t(i)].name);
        out += caps ? QStringLiteral("* The capacitance matrix of %1's terminals: each to the others and to ref (the grounds)\n").arg(physics)
                    : QStringLiteral("* The conductance matrix of %1's terminals: each to the others and to ref (the grounds)\n").arg(physics);
        out += QStringLiteral(".subckt %1 %2 ref\n").arg(sub, pins.join(QLatin1Char(' ')));
        double largest = 0;
        for (const auto& row : f.matrix)
            for (double v : row) largest = std::max(largest, std::abs(v));
        const QString letter = caps ? QStringLiteral("C") : QStringLiteral("R");
        int count = 0;
        for (int i = 0; i < n; ++i) {
            // To ref: the row's sum; to another: minus the entry.
            double toRef = 0;
            for (int j = 0; j < n; ++j) toRef += f.matrix[std::size_t(i)][std::size_t(j)];
            auto element = [&](const QString& a, const QString& b, double v) {
                if (!(v > 1e-9 * largest)) return;
                out += QStringLiteral("%1%2 %3 %4 %5\n").arg(letter).arg(++count).arg(a, b, num(caps ? v : 1 / v));
            };
            element(pins[i], QStringLiteral("ref"), toRef);
            for (int j = i + 1; j < n; ++j) element(pins[i], pins[j], -f.matrix[std::size_t(i)][std::size_t(j)]);
        }
        out += QStringLiteral(".ends %1\n").arg(sub);
        return out;
    }
    if (f.type == QLatin1String("magnetic")) {
        // L_i_j from the globals (the coils swept), each coil's R.
        const QString p = physics + QLatin1Char('.');
        QStringList coils;
        for (const Field::Terminal& t : f.terminals) coils << t.name;
        const int n = int(coils.size());
        if (n == 0) {
            if (error) *error = tr("%1 has no coils").arg(physics);
            return {};
        }
        std::vector<std::vector<double>> L(std::size_t(n), std::vector<double>(std::size_t(n), NAN));
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j)
                if (const std::optional<double> v = solution.global(p + QStringLiteral("L_") + coils[i] + QLatin1Char('_') + coils[j]))
                    L[std::size_t(i)][std::size_t(j)] = *v;
        for (int i = 0; i < n; ++i)
            if (!std::isfinite(L[std::size_t(i)][std::size_t(i)])) {
                if (error) *error = tr("%1 has no inductance matrix: keep the study's coil sweep on").arg(physics);
                return {};
            }
        QStringList pins;
        for (const QString& c : coils) pins << spiceName(c) + QStringLiteral("_p") << spiceName(c) + QStringLiteral("_n");
        out += QStringLiteral("* The inductance matrix of %1's coils%2: each coil between its pins _p and _n, coupled by K\n")
                   .arg(physics, solution.frequency() > 0 ? tr(" at %1").arg(formatQuantity(solution.frequency(), Dim::of(0, 0, -1))) : QString());
        out += QStringLiteral(".subckt %1 %2\n").arg(sub, pins.join(QLatin1Char(' ')));
        for (int i = 0; i < n; ++i) {
            const QString c = spiceName(coils[i]);
            double r = NAN;
            if (const std::optional<double> v = solution.global(p + QStringLiteral("R_") + coils[i] + QLatin1Char('_') + coils[i])) r = *v;
            if (std::isfinite(r) && r > 0) {
                out += QStringLiteral("R%1 %1_p %1_m %2\n").arg(c, num(r));
                out += QStringLiteral("L%1 %1_m %1_n %2\n").arg(c, num(L[std::size_t(i)][std::size_t(i)]));
            } else {
                out += QStringLiteral("L%1 %1_p %1_n %2\n").arg(c, num(L[std::size_t(i)][std::size_t(i)]));
            }
        }
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j) {
                const double m = 0.5 * (L[std::size_t(i)][std::size_t(j)] + L[std::size_t(j)][std::size_t(i)]);
                const double k = m / std::sqrt(L[std::size_t(i)][std::size_t(i)] * L[std::size_t(j)][std::size_t(j)]);
                if (!std::isfinite(k) || std::abs(k) < 1e-9) continue;
                out += QStringLiteral("K%1_%2 L%1 L%2 %3\n").arg(spiceName(coils[i]), spiceName(coils[j]), num(std::clamp(k, -1.0, 1.0)));
            }
        out += QStringLiteral(".ends %1\n").arg(sub);
        return out;
    }
    if (error) *error = tr("%1 has no terminal matrix (electrostatics, electric currents and magnetic fields have)").arg(physics);
    return {};
}

QString qucsLineSchematic(const LineRLGC& line, double length, const QString& version, const QString& name, bool clip)
{
    const QString rlcg = QStringLiteral("<RLCG Line1 1 %1 100 -26 20 0 0 \"%2\" 1 \"%3\" 1 \"%4\" 1 \"%5\" 1 \"%6\" 1 \"26.85\" 0>\n")
                             .arg(clip ? 0 : 200)
                             .arg(num(line.R), num(line.L), num(line.C), num(line.G), num(length));
    if (clip) return QStringLiteral("<Qucs Schematic %1>\n<Components>\n  %2</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n")
            .arg(version, rlcg);
    QString out = QStringLiteral("<Qucs Schematic %1>\n").arg(version);
    out += QStringLiteral("<Properties>\n  <View=0,0,400,240,1,0,0>\n  <Grid=10,10,1>\n  <DataSet=%1.dat>\n  <DataDisplay=%1.dpl>\n"
                          "  <OpenDisplay=0>\n  <Script=%1.m>\n  <RunScript=0>\n  <showFrame=0>\n  <FrameText0=%2>\n"
                          "  <FrameText1=Drawn By:>\n  <FrameText2=Date:>\n  <FrameText3=Revision:>\n</Properties>\n")
               .arg(name, tr("A transmission line from a multiphysics model"));
    out += QStringLiteral("<Symbol>\n  <.ID -20 24 SUB>\n  <Line -20 -10 40 0 #000080 2 1>\n  <Line 20 -10 0 20 #000080 2 1>\n"
                          "  <Line -20 10 40 0 #000080 2 1>\n  <Line -20 -10 0 20 #000080 2 1>\n  <Line -30 0 10 0 #000080 2 1>\n"
                          "  <Line 20 0 10 0 #000080 2 1>\n  <Line -16 4 32 0 #000080 1 1>\n  <Line -16 -4 32 0 #000080 1 1>\n"
                          "  <.PortSym -30 0 1 0>\n  <.PortSym 30 0 2 0>\n</Symbol>\n");
    out += QStringLiteral("<Components>\n  <Port P1 1 100 100 -23 12 0 0 \"1\" 1 \"analog\" 0>\n  %1"
                          "  <Port P2 1 300 100 4 12 1 2 \"2\" 1 \"analog\" 0>\n</Components>\n")
               .arg(rlcg);
    out += QStringLiteral("<Wires>\n  <100 100 170 100 \"\" 0 0 0 \"\">\n  <230 100 300 100 \"\" 0 0 0 \"\">\n</Wires>\n");
    out += QStringLiteral("<Diagrams>\n</Diagrams>\n<Paintings>\n  <Text 100 150 10 #000000 0 \"%1\">\n</Paintings>\n")
               .arg(tr("R, L, G, C per metre at %1").arg(formatQuantity(line.frequency, Dim::of(0, 0, -1))));
    return out;
}

namespace {

/// NumPy's .npy (format 1.0) of \a values, doubles little-endian in C's
/// order, of \a shape.
QByteArray npy(const std::vector<double>& values, const std::vector<std::size_t>& shape)
{
    QByteArray dims;
    for (std::size_t i = 0; i < shape.size(); ++i) dims += (i ? ", " : "") + QByteArray::number(qulonglong(shape[i]));
    if (shape.size() == 1) dims += ',';
    QByteArray header = "{'descr': '<f8', 'fortran_order': False, 'shape': (" + dims + "), }";
    // (Magic, version, its length, the header and a newline: a multiple of 64.)
    const int pad = (64 - (10 + header.size() + 1) % 64) % 64;
    header += QByteArray(pad, ' ') + '\n';
    QByteArray out("\x93NUMPY\x01\x00", 8);
    out += char(header.size() & 0xff);
    out += char((header.size() >> 8) & 0xff);
    out += header;
    out.reserve(out.size() + qsizetype(8 * values.size()));
    for (double v : values) {
        quint64 bits;
        std::memcpy(&bits, &v, 8);
        for (int b = 0; b < 8; ++b) out += char((bits >> (8 * b)) & 0xff);
    }
    return out;
}

/// A Field on a Grid export: its expressions at each of its instances, on
/// a regular grid or at the mesh's nodes.
ExportText fieldExport(const SolutionSet& set, const Node& node)
{
    ExportText out;
    // The expressions, each named if wanted ("E = emw.normE").
    std::vector<NamedExpression> items;
    for (const QString& raw : node.text(QStringLiteral("expressions")).split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        NamedExpression item = namedExpression(raw);
        if (item.expression.isEmpty()) continue;
        if (item.name.isEmpty()) item.name = item.expression;
        items.push_back(item);
    }
    if (items.empty()) {
        out.error = tr("give it an expression (emw.normE; semi.n...)");
        return out;
    }
    // Which solutions: the one asked (its point, time or frequency, mode),
    // each, or at each point the mode its step chose.
    const QString which = node.text(QStringLiteral("solutions"));
    const bool one = which == QLatin1String("selected") || which.isEmpty();
    const std::vector<Instance> at = one ? instances(set, false, node.integer(QStringLiteral("point")), node.text(QStringLiteral("time")),
                                                     node.text(QStringLiteral("frequency")), node.integer(QStringLiteral("mode")))
                                         : instances(set, true, 0, {}, {}, modesAsked(set, which));
    if (at.empty()) {
        out.error = tr("no solution to export");
        return out;
    }
    const Solution& first = *set.solutions[std::size_t(at.front().point)];
    const Topology& topo = first.topology();
    const double scale = topo.unitScale;
    const QString unit = topo.unitName.isEmpty() ? QStringLiteral("m") : topo.unitName;
    // Where: a grid's corners (the geometry's unit; empty: the model's
    // extent) and counts, or the mesh's nodes.
    const bool grid = node.text(QStringLiteral("where")) != QLatin1String("nodes");
    std::vector<QPointF> points;
    int nx = 0, ny = 0;
    if (grid) {
        nx = std::clamp(node.integer(QStringLiteral("nx")), 2, 10000);
        ny = std::clamp(node.integer(QStringLiteral("ny")), 2, 10000);
        if (qint64(nx) * ny > 4000000) {
            out.error = tr("%1 by %2 points: four million at most").arg(nx).arg(ny);
            return out;
        }
        // (The mesh's extent: its nodes' least and most x and y.)
        double corner[2][2] = {{INFINITY, INFINITY}, {-INFINITY, -INFINITY}};
        for (const QPointF& p : first.mesh().nodes) {
            corner[0][0] = std::min(corner[0][0], p.x());
            corner[0][1] = std::min(corner[0][1], p.y());
            corner[1][0] = std::max(corner[1][0], p.x());
            corner[1][1] = std::max(corner[1][1], p.y());
        }
        const QStringList from = node.pair(QStringLiteral("from")), to = node.pair(QStringLiteral("to"));
        bool given = false;
        for (const QString& t : from + to) given = given || !t.trimmed().isEmpty();
        if (given) {
            Expression::Options options;
            options.lengthUnit = scale;
            options.numbersAreLengths = true;
            const QStringList texts[2] = {from, to};
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < 2; ++k) {
                    const Expression e = Expression::compile(texts[c].value(k), first.parameters().scope, options);
                    if (!e.isValid() || !e.isConstant()) {
                        out.error = tr("the corner %1: %2").arg(texts[c].value(k), e.isValid() ? tr("not a constant") : e.error());
                        return out;
                    }
                    corner[c][k] = e.constant();
                }
        }
        if (!(corner[1][0] > corner[0][0]) || !(corner[1][1] > corner[0][1])) {
            out.error = tr("its corners: the second above and right of the first");
            return out;
        }
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
                points.emplace_back(corner[0][0] + (corner[1][0] - corner[0][0]) * i / (nx - 1),
                                    corner[0][1] + (corner[1][1] - corner[0][1]) * j / (ny - 1));
    } else {
        points = first.mesh().nodes;
        for (const Instance& in : at)
            if (set.solutions[std::size_t(in.point)]->mesh().nodes != points) {
                out.error = tr("the mesh is not the same at each solution (a swept geometry): export them on a grid, or one alone");
                return out;
            }
    }
    // values[(i K + k) N + n]: instance i, expression k, point n.
    const std::size_t N = points.size(), K = items.size(), I = at.size();
    std::vector<double> values(I * K * N, NAN);
    QStringList names;
    for (std::size_t i = 0; i < I; ++i) {
        Solution* s = select(set, at[i]);
        const Locator locator(s->mesh());
        for (std::size_t k = 0; k < K; ++k) {
            const Expression e = Expression::compile(items[k].expression, s->scope());
            if (!e.isValid()) {
                out.error = tr("%1: %2").arg(items[k].expression, e.error());
                return out;
            }
            const QString u = dimName(e.dim());
            names << (I > 1 && !at[i].label.isEmpty() ? at[i].label + QStringLiteral(": ") : QString()) + items[k].name +
                         (e.dim().isNone() ? QString() : QStringLiteral(" (%1)").arg(u));
            for (std::size_t n = 0; n < N; ++n) {
                const std::optional<double> v = evaluateAt(*s, locator, e, points[n]);
                if (v) values[(i * K + k) * N + n] = *v;
            }
        }
    }
    const QString format = node.text(QStringLiteral("format"));
    const QString where = grid ? tr("a %1 by %2 grid").arg(nx).arg(ny) : tr("the mesh's %1 nodes").arg(N);
    if (format == QLatin1String("npy")) {
        // A grid: (instances,) expressions, y, x; nodes: rows of x, y, then
        // each instance's each expression.
        out.suffix = QStringLiteral("npy");
        if (grid) {
            std::vector<std::size_t> shape;
            if (I > 1) shape.push_back(I);
            shape.insert(shape.end(), {K, std::size_t(ny), std::size_t(nx)});
            out.data = npy(values, shape);
            // (Its points' x and y beside it: shape 2, ny, nx.)
            std::vector<double> xy(2 * N);
            for (std::size_t n = 0; n < N; ++n) {
                xy[n] = points[n].x();
                xy[N + n] = points[n].y();
            }
            out.coordinates = npy(xy, {2, std::size_t(ny), std::size_t(nx)});
            out.summary = tr("%1 at %2 (shape %3; x, y in %4 in its _xy.npy beside it, shape (2, %5, %6))")
                              .arg(names.join(QLatin1String(", ")), where,
                                   QStringLiteral("(%1%2, %3, %4)").arg(I > 1 ? QString::number(I) + QStringLiteral(", ") : QString()).arg(K).arg(ny).arg(nx),
                                   unit)
                              .arg(ny)
                              .arg(nx);
        } else {
            std::vector<double> rows(N * (2 + I * K));
            for (std::size_t n = 0; n < N; ++n) {
                double* r = &rows[n * (2 + I * K)];
                r[0] = points[n].x();
                r[1] = points[n].y();
                for (std::size_t c = 0; c < I * K; ++c) r[2 + c] = values[c * N + n];
            }
            out.data = npy(rows, {N, 2 + I * K});
            out.summary = tr("x, y (%1), %2 at %3").arg(unit, names.join(QLatin1String(", ")), where);
        }
        return out;
    }
    // Text: a row per point - x, y, each value; .dat a blank line after each
    // grid row (gnuplot's splot), # before its header.
    const bool dat = format == QLatin1String("dat");
    const QString sep = dat ? QStringLiteral(" ") : QStringLiteral(",");
    QString text;
    QStringList header = {QStringLiteral("x (%1)").arg(unit), QStringLiteral("y (%1)").arg(unit)};
    header += names;
    if (dat) {
        text += QStringLiteral("# %1\n").arg(where);
        text += QStringLiteral("# ") + header.join(QLatin1String("\t")) + QLatin1Char('\n');
    } else {
        QStringList quoted;
        for (const QString& h : header) quoted << (h.contains(QLatin1Char(',')) || h.contains(QLatin1Char('"')) ? QLatin1Char('"') + QString(h).replace(QLatin1Char('"'), QStringLiteral("\"\"")) + QLatin1Char('"') : h);
        text += quoted.join(sep) + QLatin1Char('\n');
    }
    auto number = [](double v) { return std::isfinite(v) ? QString::number(v, 'g', 12) : QStringLiteral("nan"); };
    for (std::size_t n = 0; n < N; ++n) {
        QStringList row = {number(points[n].x()), number(points[n].y())};
        for (std::size_t c = 0; c < I * K; ++c) row << number(values[c * N + n]);
        text += row.join(sep) + QLatin1Char('\n');
        if (dat && grid && (n + 1) % std::size_t(nx) == 0 && n + 1 < N) text += QLatin1Char('\n');
    }
    out.text = text;
    out.suffix = dat ? QStringLiteral("dat") : QStringLiteral("csv");
    out.summary = tr("%1 at %2 (x, y in %3)").arg(names.join(QLatin1String(", ")), where, unit);
    return out;
}

} // namespace

ExportText exportText(const Model& model, const SolutionSet& set, const Node& node, const QString& version)
{
    ExportText out;
    if (set.solutions.empty()) {
        out.error = tr("its study is not solved: Compute it first");
        return out;
    }
    QString why;
    const int point = node.integer(QStringLiteral("point"));
    const QStringList comments = {tr("Qucs-S multiphysics: %1, %2").arg(model.fileName.isEmpty() ? tr("an unsaved model") : model.fileName, node.name())};
    auto lineNode = [&]() -> const Node* {
        const QString tag = node.text(QStringLiteral("line")).trimmed();
        const Node* l = model.find(tag);
        if (!l || l->type != QLatin1String("lineparams")) {
            out.error = tr("there is no Line Parameters node %1").arg(tag);
            return nullptr;
        }
        if (l->text(QStringLiteral("method")) != QLatin1String("rlgc")) {
            out.error = tr("%1 is quasi-TEM: a line's R, L, G and C want its method \"R, L, G, C over frequency\"").arg(l->name());
            return nullptr;
        }
        return l;
    };
    auto length = [&](double* v) {
        const std::optional<double> l = evaluateConstant(node.text(QStringLiteral("length")), set.solutions.front()->parameters().scope,
                                                         Dim::length(), &why);
        if (!l || !(*l > 0)) {
            out.error = tr("its length: %1").arg(l ? tr("it must be more than 0") : why);
            return false;
        }
        *v = *l;
        return true;
    };
    // One solution: its sweep point and the frequency asked (the last).
    auto one = [&]() -> Solution* {
        const std::vector<Instance> inst = instances(set, false, point, QString(), node.text(QStringLiteral("frequency")));
        if (inst.empty()) return nullptr;
        return select(set, inst.front());
    };
    const QString type = node.type;
    if (type == QLatin1String("fieldexport")) return fieldExport(set, node);
    if (type == QLatin1String("touchstone") && node.text(QStringLiteral("of")) == QLatin1String("ports")) {
        const QStringList tags = node.list(QStringLiteral("physics"));
        const QString tag = tags.isEmpty() ? QStringLiteral("ewfd") : tags.first();
        int np = 0;
        out.text = touchstonePorts(set, tag, point, comments, &np, &out.error);
        if (!out.error.isEmpty()) return out;
        out.suffix = QStringLiteral("s%1p").arg(np);
        out.summary = tr("the S-matrix of %1's %2 ports").arg(tag).arg(np);
        return out;
    }
    if (type == QLatin1String("touchstone")) {
        const Node* l = lineNode();
        double len = 0;
        if (!l || !length(&len)) return out;
        const std::optional<double> z = evaluateConstant(node.text(QStringLiteral("zref")), set.solutions.front()->parameters().scope,
                                                         Dim::resistance(), &why);
        if (!z || !(*z > 0)) {
            out.error = tr("its reference impedance: %1").arg(z ? tr("it must be more than 0") : why);
            return out;
        }
        if (lineTerminals(*l).size() > 1) {
            // Coupled lines: a 2N-port (a pair's mixed-mode 4-port).
            const CoupledSweep sweep = coupledSweep(set, *l, point);
            if (!sweep.error.isEmpty()) {
                out.error = sweep.error;
                return out;
            }
            const int n = int(lineTerminals(*l).size());
            const bool mixed = node.flag(QStringLiteral("mixed"));
            if (mixed && n != 2) {
                out.error = tr("mixed-mode S-parameters are a pair's: %1 has %2 lines").arg(l->name()).arg(n);
                return out;
            }
            out.text = touchstoneCoupled(sweep, len, *z, mixed, comments);
            out.suffix = QStringLiteral("s%1p").arg(2 * n);
            out.summary = (mixed ? tr("mixed-mode S-parameters (Sdd, Sdc, Scd, Scc) of %1 of the pair %2 at %3 frequencies, for ports of %4")
                                 : tr("S-parameters of %1 of the %5 coupled lines %2 at %3 frequencies, for ports of %4"))
                              .arg(formatQuantity(len, Dim::length()), l->name())
                              .arg(sweep.points.size())
                              .arg(formatQuantity(*z, Dim::resistance()))
                              .arg(n);
            return out;
        }
        const LineSweep sweep = lineSweep(set, *l, point);
        if (!sweep.error.isEmpty()) {
            out.error = sweep.error;
            return out;
        }
        out.text = touchstone(sweep, len, *z, comments);
        out.suffix = QStringLiteral("s2p");
        out.summary = tr("S-parameters of %1 of line %2 at %3 frequencies, for %4 ports")
                          .arg(formatQuantity(len, Dim::length()), l->name())
                          .arg(sweep.points.size())
                          .arg(formatQuantity(*z, Dim::resistance()));
        return out;
    }
    if (type == QLatin1String("spice")) {
        const QString name = node.text(QStringLiteral("name")).trimmed().isEmpty() ? node.tag : node.text(QStringLiteral("name")).trimmed();
        Solution* s = one();
        if (!s) {
            out.error = tr("nothing is solved");
            return out;
        }
        if (node.text(QStringLiteral("what")) == QLatin1String("matrix")) {
            const QStringList tags = node.list(QStringLiteral("physics"));
            const QString tag = tags.isEmpty() ? QStringLiteral("es") : tags.first();
            out.text = spiceMatrix(*s, tag, name, comments, &out.error);
            out.suffix = QStringLiteral("cir");
            out.summary = tr("subcircuit %1: %2's terminal matrix").arg(spiceName(name), tag);
            return out;
        }
        const Node* l = lineNode();
        double len = 0;
        if (!l || !length(&len)) return out;
        const QString model = node.text(QStringLiteral("model"));
        const int sections = std::clamp(node.integer(QStringLiteral("sections")), 1, 10000);
        if (lineTerminals(*l).size() > 1) {
            // Coupled lines: sections of their matrices.
            if (model == QLatin1String("ltra")) {
                out.error = tr("an LTRA line is one line: coupled lines as sections of R, L, G, C (Line model)");
                return out;
            }
            const CoupledLines c = coupledLines(*s, *l);
            if (!c.error.isEmpty()) {
                out.error = c.error;
                return out;
            }
            out.text = spiceCoupled(c, len, name, sections, comments);
            out.suffix = QStringLiteral("cir");
            out.summary = tr("subcircuit %1: %2 sections of %3 coupled lines, %4 at %5")
                              .arg(spiceName(name))
                              .arg(sections)
                              .arg(c.names.size())
                              .arg(formatQuantity(len, Dim::length()), formatQuantity(c.frequency, Dim::of(0, 0, -1)));
            return out;
        }
        const LineRLGC r = lineRLGC(*s, *l);
        if (!r.error.isEmpty()) {
            out.error = r.error;
            return out;
        }
        const bool ltra = model == QLatin1String("ltra");
        out.text = spiceLine(r, len, name, sections, ltra, comments);
        out.suffix = QStringLiteral("cir");
        out.summary = ltra ? tr("subcircuit %1: an LTRA line of %2 at %3").arg(spiceName(name), formatQuantity(len, Dim::length()),
                                                                                 formatQuantity(r.frequency, Dim::of(0, 0, -1)))
                           : tr("subcircuit %1: %2 sections of %3 at %4")
                                 .arg(spiceName(name))
                                 .arg(sections)
                                 .arg(formatQuantity(len, Dim::length()), formatQuantity(r.frequency, Dim::of(0, 0, -1)));
        return out;
    }
    if (type == QLatin1String("qucspart")) {
        Solution* s = one();
        const Node* l = lineNode();
        double len = 0;
        if (!s || !l || !length(&len)) {
            if (out.error.isEmpty()) out.error = tr("nothing is solved");
            return out;
        }
        const QString name = node.text(QStringLiteral("name")).trimmed().isEmpty() ? node.tag : node.text(QStringLiteral("name")).trimmed();
        if (lineTerminals(*l).size() > 1) {
            // A symmetric pair: a coupled line part of its modes.
            const CoupledLines c = coupledLines(*s, *l);
            if (!c.error.isEmpty()) {
                out.error = c.error;
                return out;
            }
            out.text = qucsCoupledSchematic(c, len, version, name, &out.error);
            if (!out.error.isEmpty()) return out;
            out.suffix = QStringLiteral("sch");
            out.summary = tr("a Qucs subcircuit: a coupled line part (CTLIN) of %1 of the pair's even and odd modes at %2")
                              .arg(formatQuantity(len, Dim::length()), formatQuantity(c.frequency, Dim::of(0, 0, -1)));
            return out;
        }
        const LineRLGC r = lineRLGC(*s, *l);
        if (!r.error.isEmpty()) {
            out.error = r.error;
            return out;
        }
        out.text = qucsLineSchematic(r, len, version, name);
        out.suffix = QStringLiteral("sch");
        out.summary = tr("a Qucs subcircuit: an RLCG part of %1 (its values at %2)").arg(formatQuantity(len, Dim::length()),
                                                                                         formatQuantity(r.frequency, Dim::of(0, 0, -1)));
        return out;
    }
    out.error = tr("%1 is not an export").arg(node.name());
    return out;
}

} // namespace qucs_s::fem
