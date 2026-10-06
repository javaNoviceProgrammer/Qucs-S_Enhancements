/*
 * eyeanalysis.cpp - the eye of a serial data signal (see eyeanalysis.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "eyeanalysis.h"

#include <QCoreApplication>
#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <set>

namespace qucs_s::eye {

namespace {

using dataset::Curve;

constexpr double kPi = 3.14159265358979323846;
// A UI less this many rms jitters is the eye's width at a bit error rate
// of 1e-12, the jitter Gaussian (2 x 7.0345, its Q at that rate).
constexpr double kBer12 = 14.069;

QString tr(const char* text)
{
    return QCoreApplication::translate("EyeAnalysis", text);
}

// y at x, straight between the samples (x rising); NaN outside.
double interpolated(const Curve& c, double x)
{
    const auto first = c.x.cbegin(), last = c.x.cend();
    const auto it = std::lower_bound(first, last, x);
    if (it == last) return NaN;
    const int i = int(it - first);
    if (*it == x) return c.y.at(i);
    if (i == 0) return NaN;
    const double x0 = c.x.at(i - 1), x1 = c.x.at(i);
    return c.y.at(i - 1) + (x - x0) / (x1 - x0) * (c.y.at(i) - c.y.at(i - 1));
}

// The circular mean of the phases (in UI, 0 to 1) of \a times counted
// from \a start, and the spread of each about it.
struct Phases {
    double mean = NaN;      // 0 to 1
    double earliest = 0.0, latest = 0.0, rms = 0.0;   // in UI about the mean
    int count = 0;
};

Phases phasesOf(const QVector<double>& times, double start, double ui)
{
    Phases p;
    double sx = 0.0, sy = 0.0;
    QVector<double> ph;
    ph.reserve(times.size());
    for (double t : times) {
        double f = (t - start) / ui;
        f -= std::floor(f);
        ph << f;
        sx += std::cos(2 * kPi * f);
        sy += std::sin(2 * kPi * f);
    }
    if (ph.isEmpty()) return p;
    p.count = int(ph.size());
    p.mean = std::atan2(sy, sx) / (2 * kPi);
    if (p.mean < 0) p.mean += 1.0;
    double squares = 0.0;
    for (double f : ph) {
        double d = f - p.mean;
        d -= std::round(d);
        p.earliest = std::min(p.earliest, d);
        p.latest = std::max(p.latest, d);
        squares += d * d;
    }
    p.rms = std::sqrt(squares / ph.size());
    return p;
}

// The values of \a c at the sampling instants: from \a first, a UI apart,
// to the curve's end.
QVector<double> centres(const Curve& c, double first, double ui, double end)
{
    QVector<double> out;
    for (qint64 k = 0;; ++k) {
        const double t = first + double(k) * ui;
        if (t > end) break;
        const double y = interpolated(c, t);
        if (std::isfinite(y)) out << y;
    }
    return out;
}

// Four levels among \a values: k-means from their quantiles. False when a
// level has no value.
bool fourLevels(const QVector<double>& values, QVector<double>& levels)
{
    if (values.size() < 4) return false;
    QVector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    if (levels.size() != 4) {
        levels.clear();
        for (int j = 0; j < 4; ++j) levels << sorted.at(int((2 * j + 1) * (sorted.size() - 1) / 8));
    }
    for (int round = 0; round < 100; ++round) {
        double sum[4] = {0, 0, 0, 0};
        int count[4] = {0, 0, 0, 0};
        for (double v : values) {
            int best = 0;
            for (int j = 1; j < 4; ++j)
                if (std::abs(v - levels.at(j)) < std::abs(v - levels.at(best))) best = j;
            sum[best] += v;
            ++count[best];
        }
        QVector<double> next;
        for (int j = 0; j < 4; ++j) {
            if (count[j] == 0) return false;
            next << sum[j] / count[j];
        }
        std::sort(next.begin(), next.end());
        if (next == levels) break;
        levels = next;
    }
    return levels.at(0) < levels.at(1) && levels.at(1) < levels.at(2) && levels.at(2) < levels.at(3);
}

// The level of \a levels nearest to \a v.
int nearest(const QVector<double>& levels, double v)
{
    int best = 0;
    for (int j = 1; j < levels.size(); ++j)
        if (std::abs(v - levels.at(j)) < std::abs(v - levels.at(best))) best = j;
    return best;
}

double cross(QPointF o, QPointF a, QPointF b)
{
    return (a.x() - o.x()) * (b.y() - o.y()) - (a.y() - o.y()) * (b.x() - o.x());
}

bool onSegment(QPointF a, QPointF b, QPointF p)
{
    return std::min(a.x(), b.x()) <= p.x() && p.x() <= std::max(a.x(), b.x()) && std::min(a.y(), b.y()) <= p.y()
           && p.y() <= std::max(a.y(), b.y());
}

bool segmentsMeet(QPointF a, QPointF b, QPointF c, QPointF d)
{
    const double d1 = cross(c, d, a), d2 = cross(c, d, b), d3 = cross(a, b, c), d4 = cross(a, b, d);
    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0))) return true;
    return (d1 == 0 && onSegment(c, d, a)) || (d2 == 0 && onSegment(c, d, b)) || (d3 == 0 && onSegment(a, b, c))
           || (d4 == 0 && onSegment(a, b, d));
}

// Whether the segment a-b enters the convex polygon \a p.
bool entersPolygon(const QPolygonF& p, QPointF a, QPointF b)
{
    if (p.containsPoint(a, Qt::OddEvenFill) || p.containsPoint(b, Qt::OddEvenFill)) return true;
    for (int i = 0; i < p.size(); ++i)
        if (segmentsMeet(a, b, p.at(i), p.at((i + 1) % p.size()))) return true;
    return false;
}

// How closely \a count crossings gather at one phase of the interval
// \a ui: 1 when all at one, near 0 when spread over it.
double gathering(const QVector<double>& times, int count, double ui)
{
    double sx = 0.0, sy = 0.0;
    const double t0 = times.first();
    for (int i = 0; i < count; ++i) {
        const double a = 2 * kPi * (times.at(i) - t0) / ui;
        sx += std::cos(a);
        sy += std::sin(a);
    }
    return std::hypot(sx, sy) / count;
}

// The interval crossings at bit boundaries gather at, looked for from
// \a rough (the shortest gaps between them) up: crossings a UI apart also
// gather at a half and a third of it, never at twice it, so the longest
// interval they gather at as closely as at any is the UI. First over a few
// crossings, then finer over more and more.
double scanUi(const QVector<double>& times, double rough)
{
    const int n = int(times.size());
    if (n < 4 || !(rough > 0.0)) return NaN;
    int m = std::min(n, 64);
    // (A step below a double's resolution would not step.)
    auto stepFor = [&](int count, double ui) { return std::max(1e-12, 1.0 / (10.0 * std::max(1.0, (times.at(count - 1) - times.first()) / ui))); };
    double step = stepFor(m, rough);
    QVector<double> at, score;
    for (double t = 0.75 * rough; t <= 2.5 * rough && at.size() < 200000; t *= 1.0 + step) {
        at << t;
        score << gathering(times, m, t);
    }
    if (at.size() < 3) return NaN;
    const double best = *std::max_element(score.cbegin(), score.cend());
    double ui = NaN;
    for (int i = 1; i + 1 < at.size(); ++i)
        if (score.at(i) >= 0.7 * best && score.at(i) >= score.at(i - 1) && score.at(i) >= score.at(i + 1)) ui = at.at(i);
    if (std::isnan(ui)) return NaN;
    while (m < n) {
        const int more = std::min(n, 4 * m);
        const double window = 3.0 * step;
        step = stepFor(more, ui);
        double bestUi = ui, bestScore = -1.0;
        int tried = 0;
        for (double t = ui * (1.0 - window); t <= ui * (1.0 + window) && tried < 100000; t *= 1.0 + step, ++tried) {
            const double g = gathering(times, more, t);
            if (g > bestScore) {
                bestScore = g;
                bestUi = t;
            }
        }
        ui = bestUi;
        m = more;
    }
    return gathering(times, n, ui) >= 0.3 ? ui : NaN;
}

QString number(double v)
{
    return QString::number(v, 'g', 4);
}

} // namespace

QVector<double> crossingTimes(const Curve& c, double level, double hysteresis)
{
    QVector<double> out;
    const int n = int(std::min(c.x.size(), c.y.size()));
    const double h = std::max(0.0, hysteresis);
    int side = 0;       // the side of the band the last sample outside it was on
    int lastOut = -1;   // ... and its index
    for (int i = 0; i < n; ++i) {
        const double y = c.y.at(i);
        if (!std::isfinite(y)) continue;
        const int s = y > level + h ? 1 : (y < level - h ? -1 : 0);
        if (s == 0) continue;
        if (side != 0 && s != side) {
            // The level was crossed between lastOut and i: where it was
            // crossed last.
            for (int j = i; j > lastOut; --j) {
                const double y0 = c.y.at(j - 1), y1 = c.y.at(j);
                if (!std::isfinite(y0) || !std::isfinite(y1) || y0 == y1) continue;
                if ((y0 - level) * (y1 - level) <= 0) {
                    const double x0 = c.x.at(j - 1), x1 = c.x.at(j);
                    out << x0 + (level - y0) / (y1 - y0) * (x1 - x0);
                    break;
                }
            }
        }
        side = s;
        lastOut = i;
    }
    return out;
}

double estimateUi(const QVector<double>& crossings, QString* why, QStringList* doubts)
{
    auto fail = [why](const QString& reason) {
        if (why) *why = reason;
        return NaN;
    };
    QVector<double> gaps;
    for (int i = 1; i < crossings.size(); ++i)
        if (crossings.at(i) > crossings.at(i - 1)) gaps << crossings.at(i) - crossings.at(i - 1);
    if (gaps.size() < 3) return fail(tr("%1 crossings, too few").arg(crossings.size()));
    QVector<double> sorted = gaps;
    std::sort(sorted.begin(), sorted.end());
    // The UI from a first guess: the mean of the gaps that are whole
    // multiples of it, each over how many it is, and again.
    auto refined = [&gaps](double ui, int* kept) {
        for (int round = 0; round < 5; ++round) {
            double gapsKept = 0.0, uis = 0.0;
            *kept = 0;
            for (double g : gaps) {
                const double r = g / ui;
                const double whole = std::round(r);
                if (whole >= 1.0 && whole <= 1e6 && std::abs(r - whole) <= 0.25) {
                    gapsKept += g;
                    uis += whole;
                    ++*kept;
                }
            }
            if (uis <= 0.0) break;
            ui = gapsKept / uis;
        }
        return ui;
    };
    // The shortest gaps are a single UI (a PRBS's runs are half of them one
    // bit long); a tenth of the way up leaves a stray short one out. But
    // when few of them are - a PRBS31's start, its long runs of ones - that
    // is two or three UIs, and the shortest gap is the UI: the guess of the
    // two that more gaps are whole multiples of (of as many, the longer -
    // half a UI divides them all too).
    const double rough = sorted.at(int((sorted.size() - 1) / 10));
    int kept = 0, keptShortest = 0;
    double ui = refined(rough, &kept);
    if (const double shortest = refined(sorted.first(), &keptShortest);
        keptShortest > kept && keptShortest >= 0.8 * gaps.size() && shortest < ui) {
        ui = shortest;
        kept = keptShortest;
    }
    const bool scanned = kept < 0.8 * gaps.size();
    if (scanned) {
        // Crossings spread over much of a UI - PAM4's, of edges of different
        // sizes through a slow channel - are no whole multiples of one: the
        // interval they gather at is looked for instead.
        ui = scanUi(crossings, rough);
        if (std::isnan(ui))
            return fail(tr("only %1 of the %2 intervals between crossings are whole multiples of one, and they gather at "
                           "no interval").arg(kept).arg(gaps.size()));
    }
    ui = fitted(crossings, ui);
    // What makes it doubtful: few crossings, gaps that are no whole number
    // of it, a gap shorter than it.
    if (doubts != nullptr) {
        if (crossings.size() < 50)
            *doubts << tr("the unit interval is told from only %1 crossings: give it if that is not a bit's length")
                           .arg(crossings.size());
        int whole = 0;
        for (double g : gaps) {
            const double r = g / ui;
            if (std::round(r) >= 1.0 && std::abs(r - std::round(r)) <= 0.25) ++whole;
        }
        if (!scanned && whole < 0.95 * gaps.size())
            *doubts << tr("only %1 of the %2 intervals between crossings are a whole number of unit intervals")
                           .arg(whole).arg(gaps.size());
        if (!scanned && sorted.first() < 0.75 * ui)
            *doubts << tr("two crossings are %1 apart, less than the unit interval").arg(number(sorted.first()));
    }
    return ui;
}

double fitted(const QVector<double>& crossings, double ui)
{
    // The mean gap errs by the first and the last crossings' jitter over
    // their distance, and folded that error adds up, a UI at a time, into
    // jitter that is not there: the crossings' times fitted against their
    // UI's number instead, all of them weighing in.
    if (crossings.size() < 3 || !(ui > 0.0)) return ui;
    for (int round = 0; round < 3; ++round) {
        const double t0 = crossings.first();
        double sx = 0.0, sy = 0.0;
        for (double t : crossings) {
            const double a = 2 * kPi * (t - t0) / ui;
            sx += std::cos(a);
            sy += std::sin(a);
        }
        const double phase = std::atan2(sy, sx) / (2 * kPi);   // where they gather, in UI from the first
        double sn = 0.0, st = 0.0, snn = 0.0, snt = 0.0;
        for (double t : crossings) {
            const double k = std::round((t - t0) / ui - phase);
            sn += k;
            st += t - t0;
            snn += k * k;
            snt += k * (t - t0);
        }
        const double count = double(crossings.size());
        const double denominator = count * snn - sn * sn;
        if (!(denominator > 0.0)) break;
        const double slope = (count * snt - sn * st) / denominator;
        if (!(slope > 0.0) || std::abs(slope - ui) > 0.25 * ui) break;
        ui = slope;
    }
    return ui;
}

void insertQ(QJsonObject& o, const Eye& e)
{
    if (std::isfinite(e.q)) {
        o.insert(QStringLiteral("Q"), dataset::rounded(e.q));
    } else if (std::isinf(e.q)) {
        o.insert(QStringLiteral("Q"), QJsonValue(QJsonValue::Null));
        o.insert(QStringLiteral("Q note"), tr("infinite: no noise - each level is the same at every bit's centre"));
    }
}

QPolygonF mask(double width, double height)
{
    const double w = width / 2.0, h = height / 2.0;
    return QPolygonF({QPointF(-w, 0.0), QPointF(-w / 2.0, h), QPointF(w / 2.0, h), QPointF(w, 0.0),
                      QPointF(w / 2.0, -h), QPointF(-w / 2.0, -h)});
}

QJsonObject toJson(const Result& r)
{
    using dataset::rounded;
    if (!r.ok()) return {{QStringLiteral("error"), r.error}};
    QJsonObject o{{QStringLiteral("unit interval"), rounded(r.ui)},
                  {QStringLiteral("unit interval from"), r.uiEstimated           ? tr("the crossings")
                                                         : !r.uiSource.isEmpty() ? tr("%1's Tbit").arg(r.uiSource)
                                                                                 : tr("given")},
                  {QStringLiteral("symbols"), r.symbols}};
    QJsonArray levels, eyes;
    for (double l : r.levels) levels << rounded(l);
    for (const Eye& e : r.eyes) {
        QJsonObject j{{QStringLiteral("height"), rounded(e.height)},
                      {QStringLiteral("width"), rounded(e.width)},
                      {QStringLiteral("width, UI"), rounded(e.width / r.ui)},
                      {QStringLiteral("width at BER 1e-12"), rounded(e.widthBer12)},
                      {QStringLiteral("jitter, peak to peak"), rounded(e.jitterPp)},
                      {QStringLiteral("jitter, rms"), rounded(e.jitterRms)},
                      {QStringLiteral("threshold"), rounded(e.threshold)},
                      {QStringLiteral("crossings"), e.crossings}};
        insertQ(j, e);
        eyes << j;
    }
    o.insert(QStringLiteral("levels"), levels);
    o.insert(QStringLiteral("eyes"), eyes);
    if (r.maskHits >= 0) o.insert(QStringLiteral("mask hits"), r.maskHits);
    if (!r.notes.isEmpty()) o.insert(QStringLiteral("note"), r.notes.join(QStringLiteral("; ")));
    return o;
}

double originFor(const Result& r, int span)
{
    return r.centre - span * r.ui / 2.0;
}

int fold(const Curve& c, double start, double ui, double origin, int span,
         const std::function<void(const QVector<QPointF>&)>& each)
{
    const int n = int(std::min(c.x.size(), c.y.size()));
    if (n < 2 || !(ui > 0.0) || span < 1 || !std::isfinite(origin)) return 0;
    const double end = c.x.at(n - 1);
    start = std::isfinite(start) ? std::max(start, c.x.at(0)) : c.x.at(0);
    if (!(end > start)) return 0;
    const double width = span * ui;
    // The windows that reach into [start, end].
    const double first = std::floor((start - origin - width) / ui) + 1.0;
    const double last = std::ceil((end - origin) / ui) - 1.0;
    // (Counted whole: past 2^53 a double's m + 1 is m.)
    if (!(last >= first) || last - first > 1e8 || std::abs(first) > 1e15 || std::abs(last) > 1e15) return 0;
    int windows = 0;
    int hint = 0;
    QVector<QPointF> line;
    for (qint64 m = qint64(first); m <= qint64(last); ++m) {
        const double from = origin + double(m) * ui, to = from + width;
        const double a = std::max(from, start), b = std::min(to, end);
        if (!(b > a)) continue;
        int i = int(std::lower_bound(c.x.cbegin() + hint, c.x.cbegin() + n, a) - c.x.cbegin());
        hint = i;
        line.clear();
        const double ya = interpolated(c, a);
        if (std::isfinite(ya)) line << QPointF(a - from, ya);
        for (; i < n && c.x.at(i) < b; ++i)
            if (c.x.at(i) > a && std::isfinite(c.y.at(i))) line << QPointF(c.x.at(i) - from, c.y.at(i));
        const double yb = interpolated(c, b);
        if (std::isfinite(yb)) line << QPointF(b - from, yb);
        if (line.size() >= 2) {
            each(line);
            ++windows;
        }
    }
    return windows;
}

Result analyse(const Curve& c, const Options& o)
{
    Result r;
    const int n = int(std::min(c.x.size(), c.y.size()));
    if (n < 2) {
        r.error = tr("too few points");
        return r;
    }
    for (int i = 1; i < n; ++i)
        if (!(c.x.at(i) >= c.x.at(i - 1))) {
            r.error = tr("its x does not rise: an eye is of a transient");
            return r;
        }
    if (o.levels != 2 && o.levels != 4) {
        r.error = tr("an eye has 2 levels (NRZ) or 4 (PAM4), not %1").arg(o.levels);
        return r;
    }
    if (o.ui > 0.0) r.ui = o.ui;   // (said, also when there is no eye)
    r.start = std::isfinite(o.start) ? std::max(o.start, c.x.at(0)) : c.x.at(0);
    r.end = c.x.at(n - 1);
    if (!(r.end > r.start)) {
        r.error = tr("nothing after its start (%1)").arg(number(r.start));
        return r;
    }
    // The curve from the start, the first point where it starts.
    Curve part;
    {
        const int i0 = int(std::lower_bound(c.x.cbegin(), c.x.cbegin() + n, r.start) - c.x.cbegin());
        if (i0 < n && c.x.at(i0) > r.start) {
            const double y = interpolated(c, r.start);
            if (std::isfinite(y)) {
                part.x << r.start;
                part.y << y;
            }
        }
        part.x.reserve(n - i0 + 1);
        part.y.reserve(n - i0 + 1);
        for (int i = i0; i < n; ++i) {
            part.x << c.x.at(i);
            part.y << c.y.at(i);
        }
    }
    double lo = NaN, hi = NaN;
    for (double y : part.y)
        if (std::isfinite(y)) {
            lo = std::isnan(lo) ? y : std::min(lo, y);
            hi = std::isnan(hi) ? y : std::max(hi, y);
        }
    if (!(hi > lo)) {
        r.error = tr("the signal is flat: no bits");
        return r;
    }
    const double swing = hi - lo;
    const bool pam4 = o.levels == 4;
    double threshold = !pam4 && std::isfinite(o.threshold) ? o.threshold : (lo + hi) / 2.0;
    if (pam4 && std::isfinite(o.threshold)) r.notes << tr("a PAM4 eye's thresholds are between its levels: the threshold given is not used");
    QVector<double> crossed = crossingTimes(part, threshold, 0.05 * swing);
    if (crossed.size() < 2) {
        r.error = tr("it does not cross %1 twice: no bits").arg(number(threshold));
        return r;
    }

    if (std::isfinite(o.ui)) {
        if (!(o.ui > 0.0)) {
            r.error = tr("the unit interval must be above 0, not %1").arg(number(o.ui));
            return r;
        }
        r.ui = o.ui;
    } else {
        QString why;
        QStringList doubts;
        r.ui = estimateUi(crossed, &why, &doubts);
        if (std::isnan(r.ui)) {
            r.error = tr("the unit interval cannot be told from the crossings (%1): give it").arg(why);
            return r;
        }
        r.uiEstimated = true;
        r.notes << doubts;
    }
    // A bit shorter than a sample is no eye - and 1e-300 s bits never
    // ended (t + T == t) while the bits filled memory.
    const double bits = (r.end - r.start) / r.ui;
    if (!(bits <= double(part.x.size()))) {
        r.error = tr("%1 bits in the range, more than its %2 samples: the bit period is too short")
                      .arg(bits, 0, 'g', 3).arg(part.x.size());
        return r;
    }
    if (bits < 3.0) {
        r.error = tr("fewer than 3 bits in the range");
        return r;
    }

    // The sampling instant: half a UI from the crossings' mean.
    auto centreOf = [&](const QVector<double>& times) {
        const Phases p = phasesOf(times, r.start, r.ui);
        double c0 = p.mean + 0.5;
        c0 -= std::floor(c0);
        return r.start + c0 * r.ui;
    };
    r.centre = centreOf(crossed);
    QVector<double> sampled = centres(part, r.centre, r.ui, r.end);

    if (!pam4) {
        auto split = [](const QVector<double>& values, double at, QVector<double>& lower, QVector<double>& upper) {
            lower.clear();
            upper.clear();
            for (double v : values) (v > at ? upper : lower) << v;
        };
        QVector<double> lower, upper;
        split(sampled, threshold, lower, upper);
        if (lower.isEmpty() || upper.isEmpty()) {
            r.error = tr("the bits are all high or all low at their centres");
            return r;
        }
        auto mean = [](const QVector<double>& v) {
            double s = 0.0;
            for (double x : v) s += x;
            return s / v.size();
        };
        if (!std::isfinite(o.threshold)) {
            // Halfway between the levels, which a ringing or an overshoot
            // does not move as it moves the extremes.
            const double between = (mean(lower) + mean(upper)) / 2.0;
            if (std::abs(between - threshold) > 0.01 * swing) {
                const QVector<double> again = crossingTimes(part, between, 0.05 * (mean(upper) - mean(lower)));
                if (again.size() >= 2) {
                    threshold = between;
                    crossed = again;
                    r.centre = centreOf(crossed);
                    sampled = centres(part, r.centre, r.ui, r.end);
                    split(sampled, threshold, lower, upper);
                    if (lower.isEmpty() || upper.isEmpty()) {
                        r.error = tr("the bits are all high or all low at their centres");
                        return r;
                    }
                }
            }
        }
        r.levels = {mean(lower), mean(upper)};
    } else {
        QVector<double> levels;
        if (!fourLevels(sampled, levels)) {
            r.error = tr("not four levels at the centres of the symbols: is it PAM4?");
            return r;
        }
        // The crossings of the middle threshold, between the middle levels,
        // place the sampling instant.
        const double middle = (levels.at(1) + levels.at(2)) / 2.0;
        if (std::abs(middle - threshold) > 0.01 * swing) {
            const QVector<double> again = crossingTimes(part, middle, 0.1 * (levels.at(2) - levels.at(1)));
            if (again.size() >= 2) {
                r.centre = centreOf(again);
                sampled = centres(part, r.centre, r.ui, r.end);
                if (!fourLevels(sampled, levels)) {
                    r.error = tr("not four levels at the centres of the symbols: is it PAM4?");
                    return r;
                }
            }
        }
        // Crossings of the middle threshold by edges of different sizes fall
        // at different points of a UI, and the UI fitted to them is off by
        // that pattern; edges symmetric about it (0 to 3, 1 to 2, and back)
        // cross it at one point, in a linear channel as in a slow one.
        if (r.uiEstimated) {
            const double middle = (levels.at(1) + levels.at(2)) / 2.0;
            QVector<double> symmetric;
            for (double t : crossingTimes(part, middle, 0.1 * (levels.at(2) - levels.at(1)))) {
                const int a = nearest(levels, interpolated(part, t - r.ui / 2.0));
                const int b = nearest(levels, interpolated(part, t + r.ui / 2.0));
                if (a + b == 3) symmetric << t;
            }
            if (symmetric.size() >= 8) {
                r.ui = fitted(symmetric, r.ui);
                r.centre = centreOf(symmetric);
                sampled = centres(part, r.centre, r.ui, r.end);
                if (!fourLevels(sampled, levels)) {
                    r.error = tr("not four levels at the centres of the symbols: is it PAM4?");
                    return r;
                }
            }
        }
        r.levels = levels;
        const double gap1 = levels.at(1) - levels.at(0), gap2 = levels.at(2) - levels.at(1), gap3 = levels.at(3) - levels.at(2);
        if (std::min({gap1, gap2, gap3}) < 0.25 * std::max({gap1, gap2, gap3}))
            r.notes << tr("the four levels are far from evenly spaced: is it PAM4?");
    }
    r.symbols = int(sampled.size());

    // Each eye: its crossings, and its levels' symbols at the centre.
    const double centrePhase = [&] {
        double f = (r.centre - r.start) / r.ui;
        return f - std::floor(f);
    }();
    for (int i = 0; i + 1 < r.levels.size(); ++i) {
        Eye e;
        e.low = r.levels.at(i);
        e.high = r.levels.at(i + 1);
        e.threshold = pam4 ? (e.low + e.high) / 2.0 : threshold;
        const QVector<double> times = pam4 ? crossingTimes(part, e.threshold, 0.1 * (e.high - e.low)) : crossed;
        const Phases p = phasesOf(times, r.start, r.ui);
        e.crossings = p.count;
        if (p.count >= 2) {
            double d = p.mean - centrePhase;
            d -= std::floor(d);
            e.phase = d - 1.0;
            e.earliest = p.earliest;
            e.latest = p.latest;
            const double spread = p.latest - p.earliest;
            e.jitterPp = spread * r.ui;
            e.jitterRms = p.rms * r.ui;
            e.width = std::max(0.0, 1.0 - spread) * r.ui;
            e.widthBer12 = std::max(0.0, r.ui - kBer12 * e.jitterRms);
        }
        QVector<double> lower, upper;
        for (double v : sampled) {
            const int level = pam4 ? nearest(r.levels, v) : (v > threshold ? 1 : 0);
            if (level == i) lower << v;
            else if (level == i + 1) upper << v;
        }
        e.lower = int(lower.size());
        e.upper = int(upper.size());
        if (!lower.isEmpty() && !upper.isEmpty()) {
            e.inner = *std::max_element(lower.cbegin(), lower.cend());
            e.outer = *std::min_element(upper.cbegin(), upper.cend());
            e.height = e.outer - e.inner;
            auto sigma = [](const QVector<double>& v, double m) {
                double s = 0.0;
                for (double x : v) s += (x - m) * (x - m);
                return std::sqrt(s / v.size());
            };
            // A spread below a billionth of the levels' spacing is the
            // numbers' rounding (an ideal source's 3.3 V gave Q = 4e14): no
            // noise, and Q infinite.
            const double noise = sigma(lower, e.low) + sigma(upper, e.high);
            if (e.high > e.low) e.q = noise > 1e-9 * (e.high - e.low) ? (e.high - e.low) / noise : std::numeric_limits<double>::infinity();
            if (e.height <= 0.0)
                r.notes << (pam4 ? tr("eye %1 is closed at its centre").arg(i + 1) : tr("the eye is closed at its centre"));
        }
        r.eyes << e;
    }

    // The mask, at each eye's centre: the UIs whose trace goes through it.
    if (std::isfinite(o.maskWidth) && std::isfinite(o.maskHeight) && o.maskWidth > 0.0 && o.maskHeight > 0.0) {
        if (o.maskWidth > 1.0) {
            r.notes << tr("a mask wider than a UI (%1) is not tested").arg(number(o.maskWidth));
        } else {
            const QPolygonF shape = mask(o.maskWidth, o.maskHeight);
            const double half = o.maskWidth / 2.0;
            std::set<qint64> hits;
            for (const Eye& e : r.eyes) {
                for (int i = 1; i < part.x.size(); ++i) {
                    const double y0 = part.y.at(i - 1), y1 = part.y.at(i);
                    if (!std::isfinite(y0) || !std::isfinite(y1)) continue;
                    const double s0 = (part.x.at(i - 1) - r.centre) / r.ui, s1 = (part.x.at(i) - r.centre) / r.ui;
                    const double from = std::ceil(std::min(s0, s1) - half), to = std::floor(std::max(s0, s1) + half);
                    for (double k = from; k <= to; k += 1.0) {
                        // (The UIs of the sampling instants only.)
                        if (k < 0.0 || r.centre + k * r.ui > r.end || hits.count(qint64(k))) continue;
                        if (entersPolygon(shape, QPointF(s0 - k, y0 - e.threshold), QPointF(s1 - k, y1 - e.threshold)))
                            hits.insert(qint64(k));
                    }
                }
            }
            r.maskHits = int(hits.size());
        }
    }
    return r;
}

} // namespace qucs_s::eye
