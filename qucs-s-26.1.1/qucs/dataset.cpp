/*
 * dataset.cpp - a simulation's dataset read as numbers, and what is
 *               measured on its curves
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "dataset.h"

#include <QByteArrayView>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>

namespace qucs_s::dataset {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("Dataset", text);
}

constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
constexpr double Degrees = 180.0 / 3.14159265358979323846;

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// "+1.5e-3", "+1.5e-3+j2e-4", "-j2e-4": the real and imaginary parts.
bool readValue(QByteArrayView text, double* re, double* im, bool* complex)
{
    *im = 0;
    bool ok = false;
    const qsizetype j = text.indexOf('j');
    *complex = j >= 0;
    if (j < 0) {
        *re = text.toDouble(&ok);
        return ok;
    }
    if (j == 0) return false;
    const char sign = text.at(j - 1);
    if (sign != '+' && sign != '-') return false;
    const QByteArrayView rePart = text.first(j - 1);
    *re = rePart.isEmpty() ? 0 : rePart.toDouble(&ok);
    if (!rePart.isEmpty() && !ok) return false;
    const double i = text.sliced(j + 1).toDouble(&ok);
    if (!ok) return false;
    *im = sign == '-' ? -i : i;
    return true;
}

} // namespace

bool Dataset::read(const QString& path, QString* error)
{
    a_path = path;
    a_variables.clear();
    a_index.clear();
    const auto fail = [error](const QString& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return fail(tr("%1 cannot be read.").arg(path));
    const QByteArray all = file.readAll();
    const char* p = all.constData();
    const char* const end = p + all.size();
    if (!all.trimmed().startsWith("<Qucs Dataset")) return fail(tr("%1 is not a Qucs dataset.").arg(path));

    int current = -1;   // the variable whose values come
    while (p < end) {
        const char* eol = static_cast<const char*>(std::memchr(p, '\n', size_t(end - p)));
        if (eol == nullptr) eol = end;
        const char* a = p;
        const char* b = eol;
        p = eol + 1;
        while (a < b && isSpace(*a)) ++a;
        while (b > a && isSpace(b[-1])) --b;
        if (a == b) continue;
        if (*a == '<') {
            current = -1;
            if (b - a < 2 || a[1] == '/' || b[-1] != '>') continue;
            const QStringList words = QString::fromUtf8(a + 1, int(b - a - 2)).split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (words.size() < 2) continue;
            Variable v;
            if (words.first() == QLatin1String("indep")) {
                v.independent = true;
                v.re.reserve(std::clamp(words.value(2).toInt(), 0, 10000000));
            } else if (words.first() == QLatin1String("dep")) {
                v.dependencies = words.mid(2);
            } else {
                continue;
            }
            v.name = words.at(1);
            // A name twice: the last one read is the one found.
            a_index.insert(v.name, int(a_variables.size()));
            a_variables.append(v);
            current = int(a_variables.size()) - 1;
            continue;
        }
        if (current < 0) continue;
        Variable& v = a_variables[current];
        double re = 0, im = 0;
        bool complex = false;
        if (!readValue(QByteArrayView(a, b - a), &re, &im, &complex)) {
            re = NaN;   // a digital value (0, 1, X, Z) or one that is not a number
            im = 0;
        }
        if (complex && v.im.isEmpty()) {
            v.im.fill(0, v.re.size());
        }
        v.re.append(re);
        if (!v.im.isEmpty()) v.im.append(im);
    }
    if (a_variables.isEmpty()) return fail(tr("%1 holds no variables.").arg(path));
    // Complex in the file, real in fact: read as real, with its sign.
    for (Variable& v : a_variables) {
        if (!v.isComplex()) continue;
        if (std::all_of(v.im.cbegin(), v.im.cend(), [](double i) { return i == 0; })) {
            v.im.clear();
            v.writtenComplex = true;
        }
    }
    return true;
}

const Variable* Dataset::find(const QString& name) const
{
    const auto it = a_index.constFind(name);
    return it == a_index.constEnd() ? nullptr : &a_variables.at(*it);
}

QString withoutSimulator(const QString& name, QString* simulator)
{
    const qsizetype slash = name.indexOf(QLatin1Char('/'));
    const qsizetype paren = name.indexOf(QLatin1Char('('));
    if (slash > 0 && (paren < 0 || slash < paren)) {
        const QString sim = name.left(slash);
        if (sim == QLatin1String("ngspice") || sim == QLatin1String("xyce") || sim == QLatin1String("spopus")
            || sim == QLatin1String("qucsator")) {
            if (simulator != nullptr) *simulator = sim;
            return name.mid(slash + 1);
        }
    }
    if (simulator != nullptr) simulator->clear();
    return name;
}

QString analysisOf(const QString& name)
{
    const qsizetype dot = name.indexOf(QLatin1Char('.'));
    const qsizetype paren = name.indexOf(QLatin1Char('('));
    if (dot <= 0 || (paren >= 0 && paren < dot)) return QString();
    return name.left(dot);
}

bool isOperatingPointValue(const Dataset& data, const Variable& v)
{
    if (!v.independent || v.size() != 1) return false;
    for (const Variable& other : data.variables())
        if (other.dependencies.contains(v.name)) return false;
    return true;
}

QString unitOf(const QString& name, const QString& definition)
{
    // The equation says what it is; else the name (without the analysis,
    // and as it is: Qucsator's out.Vt has none).
    for (const QString& text : {definition, bareName(withoutSimulator(name)), withoutSimulator(name)}) {
        const QString t = text.trimmed().toLower().remove(QLatin1Char(' '));
        if (t.isEmpty()) continue;
        static const QRegularExpression db(QStringLiteral("^(db|vdb|idb|dbv|dbm)\\(|^20\\*log10\\(|^10\\*log10\\(|^db\\[|^vdb\\["));
        if (db.match(t).hasMatch()) return QStringLiteral("dB");
        static const QRegularExpression phase(QStringLiteral("^(phase|cph|vp|ip|arg|angle|ph|unwrap)\\("));
        if (phase.match(t).hasMatch()) return QString(QChar(0x00B0));
        if (text == definition) continue;   // (a definition that is not one of these says nothing of the name)
        if (t == QLatin1String("time")) return QStringLiteral("s");
        if (t.contains(QLatin1String("freq"))) return QStringLiteral("Hz");
        static const QRegularExpression voltage(QStringLiteral("^(v|vm|vr|vi)\\(|\\.vt?$"));
        if (voltage.match(t).hasMatch()) return QStringLiteral("V");
        static const QRegularExpression current(QStringLiteral("^(i|im|ir|ii)\\(|\\.it?$|#branch$"));
        if (current.match(t).hasMatch()) return QStringLiteral("A");
    }
    return QString();
}

QString bareName(const QString& name)
{
    const QString analysis = analysisOf(name);
    return analysis.isEmpty() ? name : name.mid(analysis.size() + 1);
}

QStringList Dataset::resolve(const QString& wanted) const
{
    const QString w = withoutSimulator(wanted.trimmed());
    if (w.isEmpty()) return {};
    // A SPICE simulator's dataset names its variables after the analysis
    // (tran.v(out)); Qucsator's after the node or the part (out.Vt).
    const bool spice = !a_path.endsWith(QLatin1String(".dat"));
    if (const Variable* exact = find(w)) {
        QStringList names{w};
        // The op analysis prints v(out) as it is: the others' come too.
        if (spice && isOperatingPointValue(*this, *exact))
            for (const Variable& v : a_variables)
                if (!v.independent && bareName(v.name).compare(w, Qt::CaseInsensitive) == 0 && !names.contains(v.name)) names << v.name;
        return names;
    }
    const auto all = [this](const std::function<bool(const QString&)>& keep) {
        QStringList names;
        for (const Variable& v : a_variables)
            if (keep(v.name) && !names.contains(v.name)) names << v.name;
        return names;
    };
    QStringList names = all([&w](const QString& n) { return n.compare(w, Qt::CaseInsensitive) == 0; });
    if (!names.isEmpty()) return names;
    if (spice && analysisOf(w).isEmpty()) {
        names = all([&w](const QString& n) { return bareName(n).compare(w, Qt::CaseInsensitive) == 0; });
        if (!names.isEmpty()) return names;
    }
    // A node: its voltage.
    if (!w.contains(QLatin1Char('(')) && !w.contains(QLatin1Char('.'))) {
        const QString voltage = QStringLiteral("v(%1)").arg(w);
        names = all([&](const QString& n) {
            if (spice) return bareName(n).compare(voltage, Qt::CaseInsensitive) == 0 || n.compare(voltage, Qt::CaseInsensitive) == 0;
            return n.compare(w + QLatin1String(".v"), Qt::CaseInsensitive) == 0
                   || n.compare(w + QLatin1String(".Vt"), Qt::CaseInsensitive) == 0;
        });
    }
    return names;
}

// ----------------------------------------------------------------------
// Curves

QList<Curve> curvesOf(const Dataset& data, const Variable& v, QList<QList<QPair<QString, double>>>* outer)
{
    QList<Curve> curves;
    if (outer != nullptr) outer->clear();
    const int n = v.size();
    QVector<double> y(n);
    for (int i = 0; i < n; ++i) y[i] = v.isComplex() ? std::hypot(v.re.at(i), v.im.at(i)) : v.re.at(i);
    if (v.independent || v.dependencies.isEmpty()) {
        Curve c;
        c.y = y;
        c.x.resize(n);
        for (int i = 0; i < n; ++i) c.x[i] = i;
        curves << c;
        if (outer != nullptr) outer->append(QList<QPair<QString, double>>());
        return curves;
    }
    const Variable* first = data.find(v.dependencies.first());
    const int length = first != nullptr && first->size() > 0 ? first->size() : n;
    const int count = std::max(1, n / std::max(1, length));
    for (int k = 0; k < count; ++k) {
        Curve c;
        const int from = k * length;
        const int to = std::min(n, from + length);
        for (int i = from; i < to; ++i) {
            c.x << (first != nullptr ? first->re.value(i - from, NaN) : double(i - from));
            c.y << y.at(i);
        }
        curves << c;
        if (outer != nullptr) {
            // The values of the other independent variables at this curve:
            // the second varies fastest of them.
            QList<QPair<QString, double>> values;
            int rest = k;
            for (int d = 1; d < v.dependencies.size(); ++d) {
                const Variable* dep = data.find(v.dependencies.at(d));
                const int size = dep != nullptr && dep->size() > 0 ? dep->size() : 1;
                const int index = rest % size;
                rest /= size;
                values << qMakePair(v.dependencies.at(d), dep != nullptr ? dep->re.value(index, NaN) : double(index));
            }
            outer->append(values);
        }
    }
    return curves;
}

QList<QVector<double>> phasesOf(const Dataset& data, const Variable& v)
{
    QList<QVector<double>> phases;
    if (!v.isComplex()) return phases;
    const QList<Curve> curves = curvesOf(data, v);
    int i = 0;
    for (const Curve& c : curves) {
        QVector<double> p;
        for (int k = 0; k < c.x.size(); ++k, ++i) p << std::atan2(v.im.value(i), v.re.value(i)) * Degrees;
        phases << p;
    }
    return phases;
}

Curve within(const Curve& c, double from, double to)
{
    if (std::isnan(from) && std::isnan(to)) return c;
    Curve part;
    for (int i = 0; i < c.x.size(); ++i) {
        const double x = c.x.at(i);
        if (!std::isnan(from) && x < from) continue;
        if (!std::isnan(to) && x > to) continue;
        part.x << x;
        part.y << c.y.at(i);
    }
    return part;
}

double valueAt(const Curve& c, double x)
{
    const int n = int(c.x.size());
    for (int i = 0; i < n; ++i) {
        if (c.x.at(i) == x) return c.y.at(i);
        if (i + 1 < n) {
            const double x0 = c.x.at(i), x1 = c.x.at(i + 1);
            if ((x0 < x && x < x1) || (x1 < x && x < x0)) {
                const double t = (x - x0) / (x1 - x0);
                return c.y.at(i) + t * (c.y.at(i + 1) - c.y.at(i));
            }
        }
    }
    return NaN;
}

QList<Crossing> crossings(const Curve& c, double level)
{
    QList<Crossing> list;
    const int n = int(c.x.size());
    // A sample on the level counts once: with the side it came from and
    // the side it goes to.
    int side = 0;          // the side of the last sample off the level
    int sideIndex = -1;
    for (int i = 0; i < n; ++i) {
        const double y = c.y.at(i);
        if (std::isnan(y)) continue;
        const int s = y > level ? 1 : (y < level ? -1 : 0);
        if (s == 0) continue;
        if (side != 0 && s != side) {
            // Between sample sideIndex (on side) and i (on s).
            const double y0 = c.y.at(sideIndex), x0 = c.x.at(sideIndex);
            const double y1 = y, x1 = c.x.at(i);
            // A level reached by a sample between them: its x.
            double x = x0 + (level - y0) / (y1 - y0) * (x1 - x0);
            for (int k = sideIndex + 1; k < i; ++k)
                if (c.y.at(k) == level) {
                    x = c.x.at(k);
                    break;
                }
            list.append({x, s > 0 ? 1 : -1});
        }
        side = s;
        sideIndex = i;
    }
    return list;
}

Stats statsOf(const Curve& c)
{
    Stats s;
    const int n = int(c.x.size());
    bool rising = n > 1;
    for (int i = 1; i < n && rising; ++i)
        if (!(c.x.at(i) >= c.x.at(i - 1))) rising = false;
    double sum = 0, sumSq = 0, weight = 0;
    bool any = false;
    for (int i = 0; i < n; ++i) {
        const double y = c.y.at(i);
        if (!std::isfinite(y)) continue;
        if (!any) {
            s.min = s.max = s.first = y;
            s.xMin = s.xMax = c.x.at(i);
            any = true;
        }
        if (y < s.min) {
            s.min = y;
            s.xMin = c.x.at(i);
        }
        if (y > s.max) {
            s.max = y;
            s.xMax = c.x.at(i);
        }
        s.last = y;
        ++s.count;
        if (!rising) {
            sum += y;
            sumSq += y * y;
            weight += 1;
        }
    }
    if (rising) {
        // An average over x (time): each step weighs its width.
        for (int i = 1; i < n; ++i) {
            const double y0 = c.y.at(i - 1), y1 = c.y.at(i);
            if (!std::isfinite(y0) || !std::isfinite(y1)) continue;
            const double dx = c.x.at(i) - c.x.at(i - 1);
            sum += 0.5 * (y0 + y1) * dx;
            sumSq += (y0 * y0 + y0 * y1 + y1 * y1) / 3.0 * dx;
            weight += dx;
        }
        if (weight <= 0) {
            // All at one x: the plain average.
            sum = sumSq = weight = 0;
            for (int i = 0; i < n; ++i)
                if (std::isfinite(c.y.at(i))) {
                    sum += c.y.at(i);
                    sumSq += c.y.at(i) * c.y.at(i);
                    weight += 1;
                }
        }
    }
    if (weight > 0) {
        s.mean = sum / weight;
        s.rms = std::sqrt(std::max(0.0, sumSq / weight));
    }
    return s;
}

double rounded(double v)
{
    if (!std::isfinite(v) || v == 0) return v;
    return QString::number(v, 'g', 7).toDouble();
}

// ----------------------------------------------------------------------
// Measurements

QStringList measurements()
{
    return {QStringLiteral("rise_time"), QStringLiteral("fall_time"), QStringLiteral("overshoot"),
            QStringLiteral("settling_time"), QStringLiteral("period"), QStringLiteral("frequency"),
            QStringLiteral("duty_cycle"), QStringLiteral("crossings"), QStringLiteral("bandwidth")};
}

namespace {

QJsonObject cannot(const QString& why)
{
    return {{QStringLiteral("error"), why}};
}

// The first edge of the curve from \a lowLevel to \a highLevel (rising) or
// back: the last crossing of the level it leaves before the first
// crossing of the level it reaches. False when there is none.
bool edge(const Curve& c, double lowLevel, double highLevel, bool rising, double* from, double* to)
{
    const QList<Crossing> leaves = crossings(c, rising ? lowLevel : highLevel);
    const QList<Crossing> reaches = crossings(c, rising ? highLevel : lowLevel);
    const int dir = rising ? 1 : -1;
    for (const Crossing& r : reaches) {
        if (r.direction != dir) continue;
        double start = NaN;
        for (const Crossing& l : leaves)
            if (l.direction == dir && l.x <= r.x) start = l.x;
        if (std::isnan(start)) continue;
        *from = start;
        *to = r.x;
        return true;
    }
    return false;
}

} // namespace

QJsonObject measure(const Curve& c, const QString& what, const MeasureOptions& o)
{
    const Stats s = statsOf(c);
    if (s.count < 2) return cannot(tr("too few points"));
    const double swing = s.max - s.min;
    const double mid = std::isnan(o.level) ? (s.max + s.min) / 2 : o.level;
    const QString w = what.trimmed().toLower().replace(QLatin1Char(' '), QLatin1Char('_'));

    if (w == QLatin1String("rise_time") || w == QLatin1String("fall_time")) {
        const bool rising = w == QLatin1String("rise_time");
        if (swing <= 0) return cannot(tr("the curve is flat"));
        const double lo = s.min + o.low * swing, hi = s.min + o.high * swing;
        double from = NaN, to = NaN;
        if (!edge(c, lo, hi, rising, &from, &to))
            return cannot(rising ? tr("no rising edge in the range") : tr("no falling edge in the range"));
        return {{QStringLiteral("value"), rounded(to - from)},
                {QStringLiteral("from"), rounded(from)},
                {QStringLiteral("to"), rounded(to)},
                {QStringLiteral("levels"), QJsonArray{rounded(lo), rounded(hi)}}};
    }
    if (w == QLatin1String("overshoot")) {
        const double step = s.last - s.first;
        if (std::abs(step) <= 1e-12 * std::max(std::abs(s.first), std::abs(s.last)) || step == 0)
            return cannot(tr("the curve ends where it starts: no step to overshoot"));
        const double peak = step > 0 ? s.max : s.min;
        const double over = (peak - s.last) / step * 100;
        return {{QStringLiteral("value"), rounded(std::max(0.0, over))},
                {QStringLiteral("unit"), QStringLiteral("%")},
                {QStringLiteral("peak"), rounded(peak)},
                {QStringLiteral("at"), rounded(step > 0 ? s.xMax : s.xMin)},
                {QStringLiteral("initial"), rounded(s.first)},
                {QStringLiteral("final"), rounded(s.last)}};
    }
    if (w == QLatin1String("settling_time")) {
        const double step = s.last - s.first;
        const double band = std::abs(o.tolerance * (step != 0 ? step : s.last));
        if (band <= 0) return cannot(tr("no step to settle"));
        int last = -1;   // the last sample outside the band
        for (int i = 0; i < c.x.size(); ++i)
            if (std::isfinite(c.y.at(i)) && std::abs(c.y.at(i) - s.last) > band) last = i;
        if (last < 0)
            return {{QStringLiteral("value"), 0}, {QStringLiteral("settled at"), rounded(c.x.first())}};
        if (last + 1 >= c.x.size()) return cannot(tr("it does not settle within %1% before the end of the range").arg(o.tolerance * 100));
        // Where it enters the band, between the two samples.
        const double y0 = c.y.at(last), y1 = c.y.at(last + 1);
        const double edgeLevel = y0 > s.last ? s.last + band : s.last - band;
        const double x = y1 != y0 ? c.x.at(last) + (edgeLevel - y0) / (y1 - y0) * (c.x.at(last + 1) - c.x.at(last)) : c.x.at(last + 1);
        return {{QStringLiteral("value"), rounded(x - c.x.first())},
                {QStringLiteral("settled at"), rounded(x)},
                {QStringLiteral("final"), rounded(s.last)},
                {QStringLiteral("band"), rounded(band)}};
    }
    if (w == QLatin1String("period") || w == QLatin1String("frequency") || w == QLatin1String("duty_cycle")) {
        QList<double> rises;
        for (const Crossing& x : crossings(c, mid))
            if (x.direction > 0) rises << x.x;
        if (rises.size() < 2) return cannot(tr("fewer than two rising crossings of %1").arg(rounded(mid)));
        const double period = (rises.last() - rises.first()) / (rises.size() - 1);
        if (w == QLatin1String("duty_cycle")) {
            // The time above the level over the whole periods.
            const Curve cycles = within(c, rises.first(), rises.last());
            double above = 0;
            QList<Crossing> all = crossings(cycles, mid);
            double start = rises.first();
            bool high = true;
            for (const Crossing& x : all) {
                if (x.x <= rises.first()) continue;
                if (high && x.direction < 0) above += x.x - start;
                if (x.direction > 0) start = x.x;
                high = x.direction > 0;
            }
            if (high) above += rises.last() - start;
            return {{QStringLiteral("value"), rounded(above / (rises.last() - rises.first()) * 100)},
                    {QStringLiteral("unit"), QStringLiteral("%")},
                    {QStringLiteral("level"), rounded(mid)},
                    {QStringLiteral("periods"), int(rises.size() - 1)}};
        }
        QJsonObject r{{QStringLiteral("value"), rounded(w == QLatin1String("period") ? period : 1.0 / period)},
                      {QStringLiteral("level"), rounded(mid)},
                      {QStringLiteral("periods"), int(rises.size() - 1)}};
        return r;
    }
    if (w == QLatin1String("crossings")) {
        QJsonArray list;
        const QList<Crossing> all = crossings(c, mid);
        for (const Crossing& x : all) {
            if (list.size() >= 200) break;
            list.append(QJsonObject{{QStringLiteral("x"), rounded(x.x)},
                                    {QStringLiteral("direction"), x.direction > 0 ? QStringLiteral("rising") : QStringLiteral("falling")}});
        }
        QJsonObject r{{QStringLiteral("level"), rounded(mid)}, {QStringLiteral("count"), int(all.size())}, {QStringLiteral("at"), list}};
        if (all.size() > list.size()) r.insert(QStringLiteral("note"), tr("the first %1 only").arg(list.size()));
        return r;
    }
    if (w == QLatin1String("bandwidth")) {
        // 3 dB below the peak: in dB, the peak less 3; of a magnitude, the
        // peak over sqrt(2). A curve that goes below 0 is neither a
        // magnitude nor known to be in dB: the one or the other would be
        // a guess, and a wrong guess a wrong number.
        if (!o.decibels && s.min < 0)
            return cannot(tr("the curve goes below 0: it is not a magnitude, and not known to be in dB (say decibels: true if it is)"));
        if (!o.decibels && s.max <= 0) return cannot(tr("the magnitude is never above 0"));
        const double level = o.decibels ? s.max - 3.0 : s.max / std::sqrt(2.0);
        QJsonArray points;
        for (const Crossing& x : crossings(c, level)) points.append(rounded(x.x));
        QJsonObject r{{QStringLiteral("peak"), rounded(s.max)},
                      {QStringLiteral("at"), rounded(s.xMax)},
                      {QStringLiteral("level"), rounded(level)},
                      {QStringLiteral("measured on"), o.decibels ? QStringLiteral("dB: 3 below the peak") : QStringLiteral("a magnitude: the peak over sqrt(2)")},
                      {QStringLiteral("-3 dB points"), points}};
        double below = NaN, above = NaN;
        for (const QJsonValue& p : points) {
            if (p.toDouble() < s.xMax) below = p.toDouble();
            else if (std::isnan(above)) above = p.toDouble();
        }
        if (!std::isnan(below) && !std::isnan(above)) r.insert(QStringLiteral("value"), rounded(above - below));
        else if (!std::isnan(above)) r.insert(QStringLiteral("value"), rounded(above));
        else if (!std::isnan(below)) r.insert(QStringLiteral("value"), rounded(below));
        else r.insert(QStringLiteral("note"), tr("it does not fall 3 dB below its peak in the range"));
        return r;
    }
    return cannot(tr("there is no measurement %1 (%2)").arg(what, measurements().join(QStringLiteral(", "))));
}

} // namespace qucs_s::dataset
