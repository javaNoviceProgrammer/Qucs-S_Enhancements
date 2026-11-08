/*
 * rf25_circuit.cpp - a lumped circuit fitted to a one- or two-port's S:
 *                    each branch of a pi fitted on its own by each form of
 *                    a few R, L and C (Levenberg and Marquardt's, the values
 *                    kept positive by fitting their logarithms), the circuit
 *                    held to the solve's S, and written as a SPICE
 *                    subcircuit or a Qucs-S schematic
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "rf25_circuit.h"

#include "config.h"

#include <QCoreApplication>

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <limits>

namespace qucs_s::rf25 {

namespace {

using Complex = std::complex<double>;
const Complex J(0, 1);

QString tr(const char* text) { return QCoreApplication::translate("RF25Circuit", text); }

// The forms a branch is fitted by, the fewest values first: what each is
// made of, in the order its values are fitted, and the unit of each.
struct Form {
    QString name;
    QStringList elements;
    QStringList units;
};

const QList<Form>& betweenForms()
{
    static const QList<Form> forms = {
        {QStringLiteral("R"), {QStringLiteral("R")}, {QStringLiteral("Ohm")}},
        {QStringLiteral("C"), {QStringLiteral("C")}, {QStringLiteral("F")}},
        {QStringLiteral("R+L"), {QStringLiteral("R"), QStringLiteral("L")}, {QStringLiteral("Ohm"), QStringLiteral("H")}},
        {QStringLiteral("R+C"), {QStringLiteral("R"), QStringLiteral("C")}, {QStringLiteral("Ohm"), QStringLiteral("F")}},
        {QStringLiteral("R||C"), {QStringLiteral("R"), QStringLiteral("C")}, {QStringLiteral("Ohm"), QStringLiteral("F")}},
        {QStringLiteral("R+L+C"),
         {QStringLiteral("R"), QStringLiteral("L"), QStringLiteral("C")},
         {QStringLiteral("Ohm"), QStringLiteral("H"), QStringLiteral("F")}},
        {QStringLiteral("(R+L)||C"),
         {QStringLiteral("R"), QStringLiteral("L"), QStringLiteral("C")},
         {QStringLiteral("Ohm"), QStringLiteral("H"), QStringLiteral("F")}},
    };
    return forms;
}

const QList<Form>& groundForms()
{
    static const QList<Form> forms = {
        {QStringLiteral("C"), {QStringLiteral("C")}, {QStringLiteral("F")}},
        {QStringLiteral("R"), {QStringLiteral("R")}, {QStringLiteral("Ohm")}},
        {QStringLiteral("R||C"), {QStringLiteral("R"), QStringLiteral("C")}, {QStringLiteral("Ohm"), QStringLiteral("F")}},
        {QStringLiteral("R+C"), {QStringLiteral("R"), QStringLiteral("C")}, {QStringLiteral("Ohm"), QStringLiteral("F")}},
        {QStringLiteral("C+(R||C)"),
         {QStringLiteral("Cox"), QStringLiteral("Rsub"), QStringLiteral("Csub")},
         {QStringLiteral("F"), QStringLiteral("Ohm"), QStringLiteral("F")}},
    };
    return forms;
}

// The admittance of form \a name of values \a p at \a w.
Complex admittanceOf(const QString& name, double w, const double* p)
{
    const Complex s = J * w;
    if (name == QLatin1String("R")) return 1.0 / p[0];
    if (name == QLatin1String("C")) return s * p[0];
    if (name == QLatin1String("R+L")) return 1.0 / (p[0] + s * p[1]);
    if (name == QLatin1String("R+C")) return 1.0 / (p[0] + 1.0 / (s * p[1]));
    if (name == QLatin1String("R||C")) return 1.0 / p[0] + s * p[1];
    if (name == QLatin1String("R+L+C")) return 1.0 / (p[0] + s * p[1] + 1.0 / (s * p[2]));
    if (name == QLatin1String("(R+L)||C")) return 1.0 / (p[0] + s * p[1]) + s * p[2];
    if (name == QLatin1String("C+(R||C)")) return 1.0 / (1.0 / (s * p[0]) + 1.0 / (1.0 / p[1] + s * p[2]));
    return 0.0;
}

// The relative misfit at each frequency, Re then Im: Y fitted / Y - 1.
Eigen::VectorXd misfit(const QString& form, const QList<double>& w, const QList<Complex>& y, const Eigen::VectorXd& logs)
{
    Eigen::VectorXd values = logs.array().exp();
    Eigen::VectorXd r(2 * w.size());
    for (int k = 0; k < w.size(); ++k) {
        const Complex e = admittanceOf(form, w.at(k), values.data()) / y.at(k) - 1.0;
        r(2 * k) = std::isfinite(e.real()) ? e.real() : 1e6;
        r(2 * k + 1) = std::isfinite(e.imag()) ? e.imag() : 1e6;
    }
    return r;
}

// Levenberg and Marquardt's on the values' logarithms from \a start: the
// least squares misfit's values, and its rms in \a error.
Eigen::VectorXd fitted(const QString& form, const QList<double>& w, const QList<Complex>& y, Eigen::VectorXd logs, double* error)
{
    const int n = int(logs.size());
    const double low = std::log(1e-30), high = std::log(1e30);
    Eigen::VectorXd r = misfit(form, w, y, logs);
    double cost = r.squaredNorm();
    double lambda = 1e-3;
    for (int it = 0; it < 400 && cost > 0; ++it) {
        Eigen::MatrixXd jac(r.size(), n);
        for (int j = 0; j < n; ++j) {
            Eigen::VectorXd moved = logs;
            moved(j) += 1e-6;
            jac.col(j) = (misfit(form, w, y, moved) - r) / 1e-6;
        }
        const Eigen::MatrixXd a = jac.transpose() * jac;
        const Eigen::VectorXd g = jac.transpose() * r;
        bool better = false;
        for (int tries = 0; tries < 20 && !better; ++tries) {
            Eigen::MatrixXd damped = a;
            for (int j = 0; j < n; ++j) damped(j, j) += lambda * std::max(a(j, j), 1e-12);
            const Eigen::VectorXd step = damped.ldlt().solve(-g);
            Eigen::VectorXd next = (logs + step).cwiseMax(low).cwiseMin(high);
            const Eigen::VectorXd rn = misfit(form, w, y, next);
            const double cn = rn.squaredNorm();
            if (std::isfinite(cn) && cn < cost) {
                const bool still = cost - cn < 1e-14 * cost || step.norm() < 1e-10;
                logs = next;
                r = rn;
                cost = cn;
                lambda = std::max(lambda / 3, 1e-12);
                better = true;
                if (still) it = 400;
            } else {
                lambda *= 4;
            }
        }
        if (!better) break;
    }
    *error = std::sqrt(cost / std::max(1, int(w.size())));
    return logs;
}

double median(QList<double> v)
{
    if (v.isEmpty()) return 0;
    std::sort(v.begin(), v.end());
    return v.at(v.size() / 2);
}

// A first guess of each value of \a form, from how the branch's admittance
// behaves at the lowest and highest frequencies.
QList<double> guessOf(const Form& form, const QList<double>& w, const QList<Complex>& y)
{
    int lo = 0, hi = 0;
    for (int k = 0; k < w.size(); ++k) {
        if (w.at(k) < w.at(lo)) lo = k;
        if (w.at(k) > w.at(hi)) hi = k;
    }
    QList<double> re, g;
    for (const Complex& v : y) {
        re << (1.0 / v).real();
        g << v.real();
    }
    const Complex zlo = 1.0 / y.at(lo), zhi = 1.0 / y.at(hi);
    const double r = median(re) > 1e-6 ? median(re) : 1.0;
    const double l = zhi.imag() > 0 ? zhi.imag() / w.at(hi) : 1e-9;
    const double cSeries = zlo.imag() < 0 ? -1.0 / (w.at(lo) * zlo.imag()) : 1e-12;
    const double cShunt = y.at(lo).imag() > 0 ? y.at(lo).imag() / w.at(lo) : 1e-13;
    const double rParallel = median(g) > 1e-12 ? 1.0 / median(g) : 1e6;
    QList<double> out;
    for (const QString& e : form.elements) {
        if (form.name == QLatin1String("R")) out << std::max(std::abs(1.0 / y.at(lo)), 1e-6);
        else if (form.name == QLatin1String("R||C")) out << (e == QLatin1String("R") ? rParallel : cShunt);
        else if (form.name == QLatin1String("C+(R||C)")) out << (e == QLatin1String("Rsub") ? 1e3 : cShunt);
        else if (e == QLatin1String("R")) out << r;
        // (An inductor with its own capacitance looks like itself low down.)
        else if (e == QLatin1String("L") && form.name == QLatin1String("(R+L)||C"))
            out << (zlo.imag() > 0 ? zlo.imag() / w.at(lo) : l);
        else if (e == QLatin1String("L")) out << l;
        else if (form.name == QLatin1String("(R+L)||C")) out << 1e-15;
        else out << (form.name == QLatin1String("C") ? cShunt : cSeries);
    }
    return out;
}

// \a form fitted to \a y from its guess and from tenfold either side of it
// in each value: the best of them.
CircuitBranch fitForm(const Form& form, const QList<double>& w, const QList<Complex>& y)
{
    const QList<double> guess = guessOf(form, w, y);
    const int n = int(guess.size());
    double best = std::numeric_limits<double>::infinity();
    Eigen::VectorXd bestLogs(n);
    int starts = 1;
    for (int j = 0; j < n; ++j) starts *= 3;
    for (int start = 0; start < starts; ++start) {
        Eigen::VectorXd logs(n);
        int code = start;
        for (int j = 0; j < n; ++j) {
            logs(j) = std::log(guess.at(j)) + std::log(10.0) * (code % 3 - 1);
            code /= 3;
        }
        double error = 0;
        const Eigen::VectorXd found = fitted(form.name, w, y, logs, &error);
        if (error < best) {
            best = error;
            bestLogs = found;
        }
    }
    CircuitBranch b;
    b.form = form.name;
    b.error = best;
    for (int j = 0; j < n; ++j) b.values.append({form.elements.at(j), std::exp(bestLogs(j))});
    return b;
}

// The branch of the forms \a forms that fits \a y best - a simpler one
// where it fits nearly as well: within a quarter of the best's misfit,
// or a fifth of a percent more.
CircuitBranch fitBranch(const QList<Form>& forms, const QList<double>& w, const QList<Complex>& y, double reference, bool between)
{
    // None to speak of, or (between two ports) a wire.
    double most = 0, least = std::numeric_limits<double>::infinity();
    for (const Complex& v : y) {
        most = std::max(most, std::abs(v) * reference);
        least = std::min(least, std::abs(v) * reference);
    }
    CircuitBranch none;
    if (most < 1e-6) {
        none.form = QStringLiteral("open");
        return none;
    }
    if (between && least > 1e6) {
        none.form = QStringLiteral("short");
        return none;
    }
    QList<CircuitBranch> tried;
    for (const Form& f : forms) tried << fitForm(f, w, y);
    // (The fewest values first, whatever order the forms came in.)
    std::stable_sort(tried.begin(), tried.end(),
                     [](const CircuitBranch& a, const CircuitBranch& b) { return a.values.size() < b.values.size(); });
    double best = std::numeric_limits<double>::infinity();
    for (const CircuitBranch& b : std::as_const(tried)) best = std::min(best, b.error);
    for (const CircuitBranch& b : std::as_const(tried))
        if (b.error <= std::max(1.25 * best, best + 2e-3)) return b;
    return tried.first();
}

QString prefixed(double value)
{
    static const struct {
        double scale;
        const char* prefix;
    } prefixes[] = {{1e12, "T"}, {1e9, "G"}, {1e6, "M"}, {1e3, "k"}, {1, ""}, {1e-3, "m"}, {1e-6, "u"},
                    {1e-9, "n"}, {1e-12, "p"}, {1e-15, "f"}, {1e-18, "a"}};
    const double a = std::abs(value);
    // (Past the prefixes, a number alone: 8.5e24 Ohm was "8.518e+12 TOhm",
    // and nought "0 a" - the check of 3 November, 5.)
    if (!std::isfinite(a) || a >= 999.95e12 || a < 0.99995e-18) return QString::number(value, 'g', 4) + QStringLiteral(" ");
    for (const auto& p : prefixes)
        if (a >= p.scale * 0.99995 || p.scale == 1e-18)
            return QString::number(value / p.scale, 'g', 4) + QStringLiteral(" ") + QLatin1String(p.prefix);
    return QString::number(value, 'g', 4) + QStringLiteral(" ");
}

QString sayBranch(const CircuitBranch& b)
{
    if (b.form == QLatin1String("open")) return tr("nothing");
    if (b.form == QLatin1String("short")) return tr("a wire");
    QStringList values;
    for (const auto& [name, value] : b.values) values << elementSaid(name, value);
    // (100 % was "1e+02 %".)
    const double percent = 100 * b.error;
    return tr("%1: %2 (fitting its own share within %3 %)")
        .arg(b.form, values.join(QStringLiteral(", ")), percent >= 10 ? QString::number(std::round(percent), 'f', 0) : QString::number(percent, 'g', 2));
}

Eigen::MatrixXcd yOf(const EquivalentCircuit& c, double w)
{
    Eigen::MatrixXcd y = Eigen::MatrixXcd::Zero(c.ports, c.ports);
    const Complex g0 = c.toGround[0].admittance(w);
    if (c.ports == 1) {
        y(0, 0) = g0;
        return y;
    }
    const Complex b = c.between.admittance(w), g1 = c.toGround[1].admittance(w);
    y(0, 0) = g0 + b;
    y(1, 1) = g1 + b;
    y(0, 1) = y(1, 0) = -b;
    return y;
}

} // namespace

Complex CircuitBranch::admittance(double w) const
{
    if (form == QLatin1String("open") || values.isEmpty()) return 0.0;
    QList<double> p;
    for (const auto& v : values) p << v.second;
    return admittanceOf(form, w, p.constData());
}

double CircuitBranch::value(const QString& element) const
{
    for (const auto& v : values)
        if (v.first == element) return v.second;
    return 0;
}

QList<QList<Complex>> EquivalentCircuit::sAt(double frequency) const
{
    const int n = ports;
    const Eigen::MatrixXcd y = yOf(*this, 2 * M_PI * frequency);
    Eigen::VectorXd root(n);
    for (int i = 0; i < n; ++i) root(i) = std::sqrt(references.value(i, 50));
    const Eigen::MatrixXcd yn = root.cast<Complex>().asDiagonal() * y * root.cast<Complex>().asDiagonal();
    const Eigen::MatrixXcd one = Eigen::MatrixXcd::Identity(n, n);
    const Eigen::MatrixXcd s = (one - yn) * (one + yn).inverse();
    QList<QList<Complex>> out(n, QList<Complex>(n));
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) out[i][j] = s(i, j);
    return out;
}

EquivalentCircuit fitCircuit(const QStringList& ports, const QList<double>& frequencies, const QList<QList<QList<Complex>>>& s,
                             const QList<double>& references)
{
    EquivalentCircuit c;
    c.portNames = ports;
    c.frequencies = frequencies;
    c.references = references;
    const int n = int(ports.size());
    if (n < 1 || n > 2) {
        c.problems << tr("An equivalent circuit is fitted to one or two ports; this has %1.").arg(n);
        return c;
    }
    if (frequencies.isEmpty() || s.size() != frequencies.size() || references.size() != n) {
        c.problems << tr("There is nothing solved to fit a circuit to.");
        return c;
    }
    for (const double f : frequencies)
        if (!(f > 0)) {
            c.problems << tr("A circuit is fitted at frequencies above nought; %1 Hz is not.").arg(f);
            return c;
        }
    c.ports = n;
    // Y of each frequency, each port into its own reference: Y = F (1 - S)
    // (1 + S)^-1 F, F the references' roots inverted.
    // (Where there is none - a port shorted to the ground, the two joined by
    // a wire, a line a whole number of half waves long - that frequency is
    // left out of the fit, and said; the circuit is held to its S all the
    // same.)
    QList<double> w, without;
    QList<Complex> y11, y22, y12;
    for (int k = 0; k < frequencies.size(); ++k) {
        Eigen::MatrixXcd m(n, n);
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) m(i, j) = s.at(k).value(i).value(j);
        const Eigen::MatrixXcd one = Eigen::MatrixXcd::Identity(n, n);
        Eigen::FullPivLU<Eigen::MatrixXcd> lu(one + m);
        lu.setThreshold(1e-9);
        if (!lu.isInvertible()) {
            without << frequencies.at(k);
            continue;
        }
        Eigen::VectorXd inverseRoot(n);
        for (int i = 0; i < n; ++i) inverseRoot(i) = 1 / std::sqrt(references.at(i));
        const Eigen::MatrixXcd y = inverseRoot.cast<Complex>().asDiagonal() * ((one - m) * lu.inverse())
                                   * inverseRoot.cast<Complex>().asDiagonal();
        w << 2 * M_PI * frequencies.at(k);
        y11 << y(0, 0);
        if (n == 2) {
            y22 << y(1, 1);
            y12 << 0.5 * (y(0, 1) + y(1, 0));
        }
    }
    if (w.isEmpty()) {
        c.problems << tr("Its Y-parameters are none at every frequency solved - a port shorted to the ground, or the two joined "
                         "by no more than a wire: there is no circuit's Y to fit.");
        c.ports = 0;
        return c;
    }
    // Each branch on its own: to the reference Y11 + Y12 and Y22 + Y12, and
    // between the two -Y12.
    if (n == 1) {
        QList<Form> all = groundForms();
        for (const Form& f : betweenForms())
            if (!std::any_of(all.begin(), all.end(), [&f](const Form& g) { return g.name == f.name; })) all << f;
        c.toGround[0] = fitBranch(all, w, y11, references.at(0), false);
    } else {
        QList<Complex> between, first, second;
        for (int k = 0; k < w.size(); ++k) {
            between << -y12.at(k);
            first << y11.at(k) + y12.at(k);
            second << y22.at(k) + y12.at(k);
        }
        c.between = fitBranch(betweenForms(), w, between, std::sqrt(references.at(0) * references.at(1)), true);
        if (c.between.form == QLatin1String("short")) {
            c.problems << tr("The two ports are joined by no more than a wire: there is no circuit between them to fit.");
            c.ports = 0;
            return c;
        }
        c.toGround[0] = fitBranch(groundForms(), w, first, references.at(0), false);
        c.toGround[1] = fitBranch(groundForms(), w, second, references.at(1), false);
    }
    // The circuit held to the solve: its S beside the solve's.
    double sum = 0;
    int terms = 0;
    for (int k = 0; k < frequencies.size(); ++k) {
        const QList<QList<Complex>> fit = c.sAt(frequencies.at(k));
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) {
                const double d = std::abs(fit.at(i).at(j) - s.at(k).value(i).value(j));
                sum += d * d;
                ++terms;
                if (d > c.worst) {
                    c.worst = d;
                    c.worstAt = frequencies.at(k);
                }
            }
    }
    c.rms = std::sqrt(sum / std::max(1, terms));
    const double lowest = *std::min_element(frequencies.begin(), frequencies.end()),
                 highest = *std::max_element(frequencies.begin(), frequencies.end());
    if (n == 1)
        c.lines << tr("One branch from %1 to the reference, fitted to its S from %2 to %3 GHz (%4 frequencies): %5.")
                       .arg(ports.at(0))
                       .arg(lowest / 1e9, 0, 'g', 4)
                       .arg(highest / 1e9, 0, 'g', 4)
                       .arg(frequencies.size())
                       .arg(sayBranch(c.toGround[0]));
    else
        c.lines << tr("A pi fitted to the two-port's S from %1 to %2 GHz (%3 frequencies): between %4 and %5 %6; from %4 to the "
                      "reference %7; from %5 to the reference %8.")
                       .arg(lowest / 1e9, 0, 'g', 4)
                       .arg(highest / 1e9, 0, 'g', 4)
                       .arg(frequencies.size())
                       .arg(ports.at(0), ports.at(1), sayBranch(c.between), sayBranch(c.toGround[0]), sayBranch(c.toGround[1]));
    if (!without.isEmpty()) {
        QStringList at;
        for (const double f : std::as_const(without)) at << QString::number(f / 1e9, 'g', 4);
        c.lines << tr("At %1 GHz its Y-parameters are none - a port shorted through, or a line a whole number of half waves "
                      "long - and those were left out of the fit.")
                       .arg(at.join(QStringLiteral(", ")));
    }
    c.lines << tr("Its S differs from the solve's by %1 at most (at %2 GHz), %3 rms.")
                   .arg(c.worst, 0, 'g', 2)
                   .arg(c.worstAt / 1e9, 0, 'g', 4)
                   .arg(c.rms, 0, 'g', 2);
    return c;
}

QString valueSaid(double value, const QString& unit)
{
    return prefixed(value) + unit;
}

bool isOpenInEffect(const QString& element, double value)
{
    // (Under a femtosiemens: a femtofarad's at 1 GHz is six million times it.)
    return element.startsWith(QLatin1Char('R')) && std::abs(value) >= 1e15;
}

QString elementSaid(const QString& element, double value)
{
    const QString unit = element.startsWith(QLatin1Char('R')) ? QStringLiteral("Ohm")
                         : element.startsWith(QLatin1Char('L')) ? QStringLiteral("H")
                                                                : QStringLiteral("F");
    return QStringLiteral("%1 %2").arg(element, valueSaid(value, unit)) + (isOpenInEffect(element, value) ? tr(" (an open)") : QString());
}

namespace {

// Each element of branch \a b between nodes \a a and \a z, as SPICE lines:
// a name of its kind and a number each, new nodes as it needs them.
void spiceLines(const CircuitBranch& b, const QString& a, const QString& z, int* count, int* nodes, QStringList* out)
{
    const auto next = [nodes] { return QStringLiteral("n%1").arg(++*nodes); };
    const auto element = [count, out](const QString& name, double value, const QString& from, const QString& to) {
        out->append(QStringLiteral("%1%2 %3 %4 %5").arg(name.left(1)).arg(++*count).arg(from, to).arg(value, 0, 'g', 6));
    };
    const QString f = b.form;
    if (f == QLatin1String("R") || f == QLatin1String("C")) {
        element(b.values.at(0).first, b.values.at(0).second, a, z);
    } else if (f == QLatin1String("R+L") || f == QLatin1String("R+C")) {
        const QString m = next();
        element(b.values.at(0).first, b.values.at(0).second, a, m);
        element(b.values.at(1).first, b.values.at(1).second, m, z);
    } else if (f == QLatin1String("R||C")) {
        element(b.values.at(0).first, b.values.at(0).second, a, z);
        element(b.values.at(1).first, b.values.at(1).second, a, z);
    } else if (f == QLatin1String("R+L+C")) {
        const QString m1 = next(), m2 = next();
        element(QStringLiteral("R"), b.value(QStringLiteral("R")), a, m1);
        element(QStringLiteral("L"), b.value(QStringLiteral("L")), m1, m2);
        element(QStringLiteral("C"), b.value(QStringLiteral("C")), m2, z);
    } else if (f == QLatin1String("(R+L)||C")) {
        const QString m = next();
        element(QStringLiteral("R"), b.value(QStringLiteral("R")), a, m);
        element(QStringLiteral("L"), b.value(QStringLiteral("L")), m, z);
        element(QStringLiteral("C"), b.value(QStringLiteral("C")), a, z);
    } else if (f == QLatin1String("C+(R||C)")) {
        const QString m = next();
        element(QStringLiteral("C"), b.value(QStringLiteral("Cox")), a, m);
        element(QStringLiteral("R"), b.value(QStringLiteral("Rsub")), m, z);
        element(QStringLiteral("C"), b.value(QStringLiteral("Csub")), m, z);
    }
}

} // namespace

QString spiceOf(const EquivalentCircuit& circuit, const QString& name, const QStringList& comment)
{
    QStringList out;
    for (const QString& line : comment) out << QStringLiteral("* ") + line;
    QStringList pins;
    for (int i = 0; i < circuit.ports; ++i) pins << QStringLiteral("p%1").arg(i + 1);
    for (int i = 0; i < circuit.ports; ++i) out << QStringLiteral("* p%1: %2").arg(i + 1).arg(circuit.portNames.value(i));
    out << QStringLiteral(".subckt %1 %2 ref").arg(name, pins.join(QLatin1Char(' ')));
    int count = 0, nodes = 0;
    if (circuit.ports == 2) {
        out << QStringLiteral("* between p1 and p2: %1").arg(circuit.between.form);
        spiceLines(circuit.between, QStringLiteral("p1"), QStringLiteral("p2"), &count, &nodes, &out);
    }
    for (int i = 0; i < circuit.ports; ++i) {
        out << QStringLiteral("* from p%1 to ref: %2").arg(i + 1).arg(circuit.toGround[i].form);
        spiceLines(circuit.toGround[i], pins.at(i), QStringLiteral("ref"), &count, &nodes, &out);
    }
    out << QStringLiteral(".ends %1").arg(name);
    return out.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

namespace {

// A Qucs-S schematic of two-terminal parts placed on a grid: each part's
// pins 30 either side of its middle, across or (turned) down; wires from
// pin to pin, every connection at a wire's end.
struct Sheet {
    QStringList components, wires;
    int r = 0, l = 0, c = 0;

    void part(const QString& kind, double value, int x, int y, bool down)
    {
        const QString unit = kind == QLatin1String("R") ? QStringLiteral("Ohm") : kind == QLatin1String("L") ? QStringLiteral("H")
                                                                                                           : QStringLiteral("F");
        const int n = kind == QLatin1String("R") ? ++r : kind == QLatin1String("L") ? ++l : ++c;
        const QString said = valueSaid(value, unit);
        const QString at = down ? QStringLiteral("%1 %2 15 -26 0 1").arg(x).arg(y) : QStringLiteral("%1 %2 -26 15 0 0").arg(x).arg(y);
        if (kind == QLatin1String("R"))
            components << QStringLiteral("  <R R%1 1 %2 \"%3\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>")
                              .arg(n).arg(at, said);
        else if (kind == QLatin1String("L"))
            components << QStringLiteral("  <L L%1 1 %2 \"%3\" 1 \"\" 0>").arg(n).arg(at, said);
        else
            components << QStringLiteral("  <C C%1 1 %2 \"%3\" 1 \"\" 0 \"neutral\" 0>").arg(n).arg(at, said);
    }
    void wire(int x1, int y1, int x2, int y2, const QString& label = QString())
    {
        if (x1 == x2 && y1 == y2) return;
        // (A label names the net in a netlist: the ports' own, p1 and p2.)
        if (label.isEmpty())
            wires << QStringLiteral("  <%1 %2 %3 %4 \"\" 0 0 0 \"\">").arg(x1).arg(y1).arg(x2).arg(y2);
        else
            wires << QStringLiteral("  <%1 %2 %3 %4 \"%5\" %6 %7 20 \"\">").arg(x1).arg(y1).arg(x2).arg(y2).arg(label).arg(x1 + 20).arg(y1 - 30);
    }
    void ground(int x, int y) { components << QStringLiteral("  <GND * 1 %1 %2 0 0 0 0>").arg(x).arg(y); }
};

// The kind of each element of a branch, by its name: "Cox" is a C.
QString kindOf(const QString& element)
{
    return element.left(1);
}

// Branch \a b across, from (\a x, \a y) rightwards: where its right end is.
// A part beside the line (R||C's C, (R+L)||C's C) goes 60 above it.
int across(Sheet& sheet, const CircuitBranch& b, int x, int y)
{
    const QString f = b.form;
    if (f == QLatin1String("R||C")) {
        sheet.part(QStringLiteral("R"), b.value(QStringLiteral("R")), x + 30, y, false);
        sheet.part(QStringLiteral("C"), b.value(QStringLiteral("C")), x + 30, y - 60, false);
        sheet.wire(x, y, x, y - 60);
        sheet.wire(x + 60, y - 60, x + 60, y);
        return x + 60;
    }
    if (f == QLatin1String("(R+L)||C")) {
        sheet.part(QStringLiteral("R"), b.value(QStringLiteral("R")), x + 30, y, false);
        sheet.part(QStringLiteral("L"), b.value(QStringLiteral("L")), x + 90, y, false);
        sheet.part(QStringLiteral("C"), b.value(QStringLiteral("C")), x + 60, y - 60, false);
        sheet.wire(x, y, x, y - 60);
        sheet.wire(x, y - 60, x + 30, y - 60);
        sheet.wire(x + 90, y - 60, x + 120, y - 60);
        sheet.wire(x + 120, y - 60, x + 120, y);
        return x + 120;
    }
    int at = x;
    for (const auto& [name, value] : b.values) {
        sheet.part(kindOf(name), value, at + 30, y, false);
        at += 60;
    }
    return at;
}

// Branch \a b down from (\a x, \a y) to a ground, a part beside it going
// \a side (-60: left, 60: right) of the line.
void down(Sheet& sheet, const CircuitBranch& b, int x, int y, int side)
{
    const QString f = b.form;
    if (f == QLatin1String("open") || b.values.isEmpty()) return;
    // (A short wire down first: the part's top pin off the line it hangs from.)
    const int top = y + 30;
    sheet.wire(x, y, x, top);
    if (f == QLatin1String("R||C")) {
        sheet.part(QStringLiteral("R"), b.value(QStringLiteral("R")), x, top + 30, true);
        sheet.part(QStringLiteral("C"), b.value(QStringLiteral("C")), x + side, top + 30, true);
        sheet.wire(x, top, x + side, top);
        sheet.wire(x, top + 60, x + side, top + 60);
        sheet.ground(x, top + 60);
        return;
    }
    if (f == QLatin1String("C+(R||C)")) {
        sheet.part(QStringLiteral("C"), b.value(QStringLiteral("Cox")), x, top + 30, true);
        sheet.part(QStringLiteral("R"), b.value(QStringLiteral("Rsub")), x, top + 90, true);
        sheet.part(QStringLiteral("C"), b.value(QStringLiteral("Csub")), x + side, top + 90, true);
        sheet.wire(x, top + 60, x + side, top + 60);
        sheet.wire(x, top + 120, x + side, top + 120);
        sheet.ground(x, top + 120);
        return;
    }
    int at = top;
    for (const auto& [name, value] : b.values) {
        sheet.part(kindOf(name), value, x, at + 30, true);
        at += 60;
    }
    sheet.ground(x, at);
}

} // namespace

QString schematicOf(const EquivalentCircuit& circuit, const QStringList& comment, const QString& name)
{
    Sheet sheet;
    // Its first port at the left of the line at y 100; the branch between
    // the ports along it; each port's branch to the ground down from where
    // it meets the line, what goes beside it outward.
    const int y = 100, left = 120;
    QStringList ports;
    ports << QStringLiteral("  <Port P1 1 40 %1 -23 12 0 0 \"1\" 1 \"analog\" 0>").arg(y);
    sheet.wire(40, y, left, y, QStringLiteral("p1"));
    int right = left;
    if (circuit.ports == 2) {
        right = across(sheet, circuit.between, left, y);
        // (A wire's length on, where the ground branches hang: never a
        // part's pin under them.)
        sheet.wire(right, y, right + 60, y);
        right += 60;
        ports << QStringLiteral("  <Port P2 1 %1 %2 4 12 1 2 \"2\" 1 \"analog\" 0>").arg(right + 80).arg(y);
        sheet.wire(right, y, right + 80, y, QStringLiteral("p2"));
        down(sheet, circuit.toGround[1], right, y, 60);
    }
    down(sheet, circuit.toGround[0], left, y, -60);
    QString text = comment.join(QStringLiteral("\\n"));
    text.replace(QLatin1Char('"'), QLatin1Char('\''));
    const QString base = name.isEmpty() ? QStringLiteral("equivalent") : name;
    QString out = QStringLiteral("<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n  <View=0,-60,800,600,1,0,0>\n  <Grid=10,10,1>\n"
                                 "  <DataSet=%1.dat>\n  <DataDisplay=%1.dpl>\n  <OpenDisplay=0>\n</Properties>\n")
                      .arg(base);
    // Its symbol as a part: a box, a pin each side (one, for a one-port).
    out += QStringLiteral("<Symbol>\n  <.ID -20 24 SUB>\n  <Line -20 -10 40 0 #000080 2 1>\n  <Line 20 -10 0 20 #000080 2 1>\n"
                          "  <Line -20 10 40 0 #000080 2 1>\n  <Line -20 -10 0 20 #000080 2 1>\n  <Line -30 0 10 0 #000080 2 1>\n"
                          "  <.PortSym -30 0 1 0>\n");
    if (circuit.ports == 2) out += QStringLiteral("  <Line 20 0 10 0 #000080 2 1>\n  <.PortSym 30 0 2 0>\n");
    out += QStringLiteral("</Symbol>\n<Components>\n");
    out += ports.join(QLatin1Char('\n')) + QLatin1Char('\n');
    out += sheet.components.join(QLatin1Char('\n')) + QLatin1Char('\n');
    out += QStringLiteral("</Components>\n<Wires>\n");
    out += sheet.wires.join(QLatin1Char('\n')) + QLatin1Char('\n');
    out += QStringLiteral("</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n");
    out += QStringLiteral("  <Text 40 400 10 #000000 0 \"%1\">\n").arg(text);
    out += QStringLiteral("</Paintings>\n");
    return out;
}

} // namespace qucs_s::rf25
