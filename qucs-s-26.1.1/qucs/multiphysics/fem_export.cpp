/*
 * fem_export.cpp - a multiphysics model's results as parts of circuits:
 *                  Touchstone files, SPICE and Qucs subcircuits
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

#include <cmath>

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
        const LineRLGC r = lineRLGC(*s, *l);
        if (!r.error.isEmpty()) {
            out.error = r.error;
            return out;
        }
        const bool ltra = node.text(QStringLiteral("model")) == QLatin1String("ltra");
        const int sections = std::clamp(node.integer(QStringLiteral("sections")), 1, 10000);
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
        const LineRLGC r = lineRLGC(*s, *l);
        if (!r.error.isEmpty()) {
            out.error = r.error;
            return out;
        }
        const QString name = node.text(QStringLiteral("name")).trimmed().isEmpty() ? node.tag : node.text(QStringLiteral("name")).trimmed();
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
