/*
 * plotdigitizer.cpp - calibration, the page's words, a plot's frame and axes,
 *                     how far to trust it, its results, and keeping it with
 *                     the PDF (plotdigitizer.h). The curves themselves are in
 *                     plotdigitizer_trace.cpp.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "plotdigitizer.h"
#include "plotdigitizer_p.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QPdfDocument>
#include <QPdfDocumentRenderOptions>
#include <QPdfSelection>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>

namespace qucs_s::digitizer {

namespace {

QString tr(const char* text) { return QCoreApplication::translate("PlotDigitizer", text); }

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

void multiply3(const double a[9], const double b[9], double out[9])
{
    double r[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    std::copy(r, r + 9, out);
}

bool invert3(const double m[9], double out[9])
{
    const double c00 = m[4] * m[8] - m[5] * m[7], c01 = m[5] * m[6] - m[3] * m[8], c02 = m[3] * m[7] - m[4] * m[6];
    const double det = m[0] * c00 + m[1] * c01 + m[2] * c02;
    if (!std::isfinite(det) || det == 0) return false;
    const double k = 1 / det;
    out[0] = c00 * k;
    out[1] = (m[2] * m[7] - m[1] * m[8]) * k;
    out[2] = (m[1] * m[5] - m[2] * m[4]) * k;
    out[3] = c01 * k;
    out[4] = (m[0] * m[8] - m[2] * m[6]) * k;
    out[5] = (m[2] * m[3] - m[0] * m[5]) * k;
    out[6] = c02 * k;
    out[7] = (m[1] * m[6] - m[0] * m[7]) * k;
    out[8] = (m[0] * m[4] - m[1] * m[3]) * k;
    return true;
}

// a x = b, n unknowns, the matrix by rows; a and b are spoiled.
bool solve(QList<double>& a, QList<double>& b, int n)
{
    for (int c = 0; c < n; ++c) {
        int pivot = c;
        for (int r = c + 1; r < n; ++r)
            if (std::abs(a[r * n + c]) > std::abs(a[pivot * n + c])) pivot = r;
        if (std::abs(a[pivot * n + c]) < 1e-11) return false;
        if (pivot != c) {
            for (int k = 0; k < n; ++k) std::swap(a[pivot * n + k], a[c * n + k]);
            std::swap(b[pivot], b[c]);
        }
        for (int r = c + 1; r < n; ++r) {
            const double f = a[r * n + c] / a[c * n + c];
            if (f == 0) continue;
            for (int k = c; k < n; ++k) a[r * n + k] -= f * a[c * n + k];
            b[r] -= f * b[c];
        }
    }
    for (int r = n - 1; r >= 0; --r) {
        double v = b[r];
        for (int k = r + 1; k < n; ++k) v -= a[r * n + k] * b[k];
        b[r] = v / a[r * n + r];
        if (!std::isfinite(b[r])) return false;
    }
    return true;
}

struct Plane {
    double a = 0, b = 0, c = 0;
};

// w = a x + b y + c through the points: over the plane where they span one,
// along their line where they lie on one (an axis's ticks: the value changes
// along the axis only). False when they are all at one place or one value.
bool fitPlane(const QList<QPointF>& at, const QList<double>& w, bool lineAllowed, Plane* out)
{
    const int n = int(at.size());
    if (n < 2) return false;
    double mx = 0, my = 0, mw = 0;
    for (int i = 0; i < n; ++i) {
        mx += at[i].x();
        my += at[i].y();
        mw += w[i];
    }
    mx /= n;
    my /= n;
    mw /= n;
    double xx = 0, xy = 0, yy = 0;
    for (int i = 0; i < n; ++i) {
        const double dx = at[i].x() - mx, dy = at[i].y() - my;
        xx += dx * dx;
        xy += dx * dy;
        yy += dy * dy;
    }
    // The spreads along and across the line of best fit.
    const double trace = xx + yy, gap = std::sqrt(std::max(0.0, (xx - yy) * (xx - yy) + 4 * xy * xy));
    const double along = std::sqrt(std::max(0.0, (trace + gap) / 2 / n)), across = std::sqrt(std::max(0.0, (trace - gap) / 2 / n));
    if (along < 0.25) return false;
    const bool spans = n >= 3 && across > std::max(0.35, 0.015 * along);
    if (spans) {
        QList<double> m(9, 0.0), r(3, 0.0);
        for (int i = 0; i < n; ++i) {
            const double dx = at[i].x() - mx, dy = at[i].y() - my, dw = w[i] - mw;
            m[0] += dx * dx;
            m[1] += dx * dy;
            m[3] += dx * dy;
            m[4] += dy * dy;
            r[0] += dx * dw;
            r[1] += dy * dw;
        }
        m[8] = 1;
        if (!solve(m, r, 3)) return false;
        out->a = r[0];
        out->b = r[1];
        out->c = mw - r[0] * mx - r[1] * my;
    } else {
        if (!lineAllowed) return false;
        const double ux0 = xy != 0 ? trace / 2 + gap / 2 - yy : (xx >= yy ? 1.0 : 0.0);
        const double uy0 = xy != 0 ? xy : (xx >= yy ? 0.0 : 1.0);
        const double norm = std::hypot(ux0, uy0);
        if (norm <= 0) return false;
        const double ux = ux0 / norm, uy = uy0 / norm;
        double st = 0, tt = 0;
        for (int i = 0; i < n; ++i) {
            const double t = (at[i].x() - mx) * ux + (at[i].y() - my) * uy;
            st += t * (w[i] - mw);
            tt += t * t;
        }
        if (tt <= 0) return false;
        const double slope = st / tt;
        out->a = slope * ux;
        out->b = slope * uy;
        out->c = mw - out->a * mx - out->b * my;
    }
    return std::isfinite(out->a) && std::isfinite(out->b) && std::isfinite(out->c);
}

} // namespace

// --- Calibration -----------------------------------------------------------

void Calibration::reset()
{
    a_valid = false;
    a_residuals.clear();
    a_rms = a_worst = 0;
}

QPointF Calibration::forward(double x, double y) const
{
    if (kind == Kind::Polar) {
        double rho = x - rAtCentre;
        if (xScale == Scale::Log) rho = x > 0 && rAtCentre > 0 ? std::log10(x / rAtCentre) : kNaN;
        const double theta = y * M_PI / 180;
        return {rho * std::cos(theta), rho * std::sin(theta)};
    }
    return {xScale == Scale::Log ? (x > 0 ? std::log10(x) : kNaN) : x, yScale == Scale::Log ? (y > 0 ? std::log10(y) : kNaN) : y};
}

QPointF Calibration::backward(double u, double v) const
{
    if (kind == Kind::Polar) {
        const double rho = std::hypot(u, v);
        const double r = xScale == Scale::Log ? rAtCentre * std::pow(10.0, rho) : rho + rAtCentre;
        return {r, std::atan2(v, u) * 180 / M_PI};
    }
    return {xScale == Scale::Log ? std::pow(10.0, u) : u, yScale == Scale::Log ? std::pow(10.0, v) : v};
}

QPointF Calibration::toValue(QPointF p) const
{
    if (!a_valid) return {kNaN, kNaN};
    const double w = a_m[6] * p.x() + a_m[7] * p.y() + a_m[8];
    if (std::abs(w) < 1e-14) return {kNaN, kNaN};
    return backward((a_m[0] * p.x() + a_m[1] * p.y() + a_m[2]) / w, (a_m[3] * p.x() + a_m[4] * p.y() + a_m[5]) / w);
}

QPointF Calibration::toPage(QPointF value) const
{
    if (!a_valid) return {kNaN, kNaN};
    const QPointF f = forward(value.x(), value.y());
    const double w = a_inverse[6] * f.x() + a_inverse[7] * f.y() + a_inverse[8];
    if (std::abs(w) < 1e-14 || !std::isfinite(w)) return {kNaN, kNaN};
    return {(a_inverse[0] * f.x() + a_inverse[1] * f.y() + a_inverse[2]) / w, (a_inverse[3] * f.x() + a_inverse[4] * f.y() + a_inverse[5]) / w};
}

QPointF Calibration::unitsPerPoint(QPointF p) const
{
    if (!a_valid) return {kNaN, kNaN};
    if (kind == Kind::Polar) {
        const QPointF v = toValue(p);
        double dr = 0, dt = 0;
        for (const QPointF step : {QPointF(0.5, 0), QPointF(-0.5, 0), QPointF(0, 0.5), QPointF(0, -0.5)}) {
            const QPointF o = toValue(p + step);
            dr = std::max(dr, std::abs(o.x() - v.x()) * 2);
            double a = std::abs(o.y() - v.y());
            if (a > 180) a = 360 - a;
            dt = std::max(dt, a * 2);
        }
        return {dr, dt};
    }
    const auto uv = [this](QPointF q) {
        const double w = a_m[6] * q.x() + a_m[7] * q.y() + a_m[8];
        return QPointF((a_m[0] * q.x() + a_m[1] * q.y() + a_m[2]) / w, (a_m[3] * q.x() + a_m[4] * q.y() + a_m[5]) / w);
    };
    const QPointF o = uv(p), dx = uv(p + QPointF(1, 0)) - o, dy = uv(p + QPointF(0, 1)) - o;
    return {std::hypot(dx.x(), dy.x()), std::hypot(dx.y(), dy.y())};
}

bool Calibration::fitAffine(QString* why)
{
    QList<QPointF> atU, atV;
    QList<double> us, vs;
    for (const Reference& r : std::as_const(references)) {
        if (!std::isfinite(r.at.x()) || !std::isfinite(r.at.y())) {
            if (why != nullptr) *why = tr("A reference's place is not a number.");
            return false;
        }
        if (kind == Kind::Polar) {
            if (!r.hasX || !r.hasY) continue;
            const QPointF f = forward(r.x, r.y);
            if (!std::isfinite(f.x()) || !std::isfinite(f.y())) {
                if (why != nullptr) *why = tr("A polar reference's r is off the scale (a log r needs r above 0, and r_at_centre above 0).");
                return false;
            }
            atU << r.at;
            atV << r.at;
            us << f.x();
            vs << f.y();
            continue;
        }
        const QPointF f = forward(r.hasX ? r.x : (xScale == Scale::Log ? 1 : 0), r.hasY ? r.y : (yScale == Scale::Log ? 1 : 0));
        if (r.hasX) {
            if (!std::isfinite(f.x())) {
                if (why != nullptr) *why = tr("x = %1 cannot be on a log axis.").arg(r.x);
                return false;
            }
            atU << r.at;
            us << f.x();
        }
        if (r.hasY) {
            if (!std::isfinite(f.y())) {
                if (why != nullptr) *why = tr("y = %1 cannot be on a log axis.").arg(r.y);
                return false;
            }
            atV << r.at;
            vs << f.y();
        }
    }
    const bool polar = kind == Kind::Polar;
    if (polar && atU.size() < 3) {
        if (why != nullptr) *why = tr("A polar plot takes three references or more with both r and angle, not on one line.");
        return false;
    }
    if (!polar && (atU.size() < 2 || atV.size() < 2)) {
        if (why != nullptr) *why = tr("It takes two references or more on each axis: x has %1, y has %2.").arg(atU.size()).arg(atV.size());
        return false;
    }
    Plane pu, pv;
    if (!fitPlane(atU, us, !polar, &pu) || !fitPlane(atV, vs, !polar, &pv)) {
        if (why != nullptr) *why = polar ? tr("The references lie on one line, or at one place: they do not fix the plot.")
                                         : tr("The references of an axis are all at one place: they do not fix it.");
        return false;
    }
    const double gu = std::hypot(pu.a, pu.b), gv = std::hypot(pv.a, pv.b);
    if (gu <= 0 || gv <= 0 || !std::isfinite(gu) || !std::isfinite(gv)) {
        if (why != nullptr) *why = tr("The references of an axis all have the same value.");
        return false;
    }
    if (std::abs(pu.a * pv.b - pu.b * pv.a) / (gu * gv) < 0.03) {
        // Two opposite corners and nothing else: they say nothing of how the
        // axes lie, so they lie as the page does, x across it and y up it.
        QList<double> xs, ys;
        for (const QPointF& at : std::as_const(atU)) xs << at.x();
        for (const QPointF& at : std::as_const(atV)) ys << at.y();
        const auto line = [](const QList<double>& p, const QList<double>& w, double* slope, double* offset) {
            double mp = 0, mw = 0, pp = 0, pw = 0;
            for (int i = 0; i < p.size(); ++i) {
                mp += p[i];
                mw += w[i];
            }
            mp /= p.size();
            mw /= p.size();
            for (int i = 0; i < p.size(); ++i) {
                pp += (p[i] - mp) * (p[i] - mp);
                pw += (p[i] - mp) * (w[i] - mw);
            }
            if (pp < 0.25 || pw == 0) return false;
            *slope = pw / pp;
            *offset = mw - *slope * mp;
            return true;
        };
        double sx = 0, ox = 0, sy = 0, oy = 0;
        if (polar || !line(xs, us, &sx, &ox) || !line(ys, vs, &sy, &oy)) {
            if (why != nullptr) *why = tr("The two axes come out parallel: the references do not tell them apart.");
            return false;
        }
        pu = Plane{sx, 0, ox};
        pv = Plane{0, sy, oy};
    }
    const double m[9] = {pu.a, pu.b, pu.c, pv.a, pv.b, pv.c, 0, 0, 1};
    std::copy(m, m + 9, a_m);
    if (!invert3(a_m, a_inverse)) {
        if (why != nullptr) *why = tr("The references give no way back from the values to the page.");
        return false;
    }
    return true;
}

bool Calibration::fitProjective(QString* why)
{
    QList<QPointF> at, uv;
    for (const Reference& r : std::as_const(references)) {
        if (!r.hasX || !r.hasY) continue;
        const QPointF f = forward(r.x, r.y);
        if (!std::isfinite(f.x()) || !std::isfinite(f.y()) || !std::isfinite(r.at.x()) || !std::isfinite(r.at.y())) {
            if (why != nullptr) *why = tr("A reference's value is off the scale.");
            return false;
        }
        at << r.at;
        uv << f;
    }
    if (at.size() < 4) {
        if (why != nullptr) *why = tr("A projective fit takes four references or more, each with both values; there are %1.").arg(at.size());
        return false;
    }
    // Each set moved to about the origin and scaled, for the arithmetic's sake.
    const auto normaliser = [](const QList<QPointF>& p, double t[9]) {
        double mx = 0, my = 0, spread = 0;
        for (const QPointF& q : p) {
            mx += q.x();
            my += q.y();
        }
        mx /= p.size();
        my /= p.size();
        for (const QPointF& q : p) spread += std::hypot(q.x() - mx, q.y() - my);
        spread /= p.size();
        const double k = spread > 0 ? 1 / spread : 1;
        const double m[9] = {k, 0, -k * mx, 0, k, -k * my, 0, 0, 1};
        std::copy(m, m + 9, t);
    };
    double tp[9], tu[9], tuInverse[9];
    normaliser(at, tp);
    normaliser(uv, tu);
    if (!invert3(tu, tuInverse)) return false;
    QList<double> m(64, 0.0), r(8, 0.0);
    for (int i = 0; i < at.size(); ++i) {
        const double x = tp[0] * at[i].x() + tp[2], y = tp[4] * at[i].y() + tp[5];
        const double u = tu[0] * uv[i].x() + tu[2], v = tu[4] * uv[i].y() + tu[5];
        const double row1[8] = {x, y, 1, 0, 0, 0, -u * x, -u * y};
        const double row2[8] = {0, 0, 0, x, y, 1, -v * x, -v * y};
        for (int a = 0; a < 8; ++a) {
            r[a] += row1[a] * u + row2[a] * v;
            for (int b = 0; b < 8; ++b) m[a * 8 + b] += row1[a] * row1[b] + row2[a] * row2[b];
        }
    }
    if (!solve(m, r, 8)) {
        if (why != nullptr) *why = tr("The references do not fix a projective fit (three of them on one line, or the same place twice).");
        return false;
    }
    const double h[9] = {r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], 1};
    double tmp[9];
    multiply3(h, tp, tmp);
    multiply3(tuInverse, tmp, a_m);
    if (!invert3(a_m, a_inverse)) {
        if (why != nullptr) *why = tr("The references give no way back from the values to the page.");
        return false;
    }
    return true;
}

void Calibration::measure()
{
    a_residuals.clear();
    double sum = 0;
    a_worst = 0;
    const auto uv = [this](QPointF q) {
        const double w = a_m[6] * q.x() + a_m[7] * q.y() + a_m[8];
        return QPointF((a_m[0] * q.x() + a_m[1] * q.y() + a_m[2]) / w, (a_m[3] * q.x() + a_m[4] * q.y() + a_m[5]) / w);
    };
    for (const Reference& r : std::as_const(references)) {
        if (!r.hasX && !r.hasY) {
            a_residuals << 0;
            continue;
        }
        double d = 0;
        const QPointF here = uv(r.at);
        if (kind == Kind::Polar || (r.hasX && r.hasY)) {
            if (r.hasX && r.hasY) {
                const QPointF page = toPage(QPointF(r.x, r.y));
                d = QLineF(page, r.at).length();
            }
        } else {
            const QPointF f = forward(r.hasX ? r.x : (xScale == Scale::Log ? 1 : 0), r.hasY ? r.y : (yScale == Scale::Log ? 1 : 0));
            const QPointF dx = uv(r.at + QPointF(1, 0)) - here, dy = uv(r.at + QPointF(0, 1)) - here;
            const double g = r.hasX ? std::hypot(dx.x(), dy.x()) : std::hypot(dx.y(), dy.y());
            const double dw = r.hasX ? f.x() - here.x() : f.y() - here.y();
            d = g > 0 ? std::abs(dw) / g : 0;
        }
        if (!std::isfinite(d)) d = 0;
        a_residuals << d;
        sum += d * d;
        a_worst = std::max(a_worst, d);
    }
    a_rms = a_residuals.isEmpty() ? 0 : std::sqrt(sum / a_residuals.size());
}

bool Calibration::fit(QString* why)
{
    reset();
    QString reason;
    bool fine = false;
    if (model == Model::Projective && kind == Kind::Cartesian) fine = fitProjective(&reason);
    else if (model == Model::Projective) reason = tr("A projective fit is for rectangular axes.");
    else fine = fitAffine(&reason);
    if (!fine) {
        if (why != nullptr) *why = reason;
        return false;
    }
    a_valid = true;
    for (const double v : a_m)
        a_valid = a_valid && std::isfinite(v);
    for (const double v : a_inverse)
        a_valid = a_valid && std::isfinite(v);
    if (!a_valid) {
        if (why != nullptr) *why = tr("The references give nothing finite.");
        return false;
    }
    measure();
    return true;
}

// --- Tick labels -----------------------------------------------------------

bool parseTickLabel(const QString& input, double* value)
{
    QString s = input.trimmed();
    if (s.isEmpty() || s.size() > 32) return false;
    // Signs, multiplication signs, spaces and superscripts in one writing.
    QString t;
    t.reserve(s.size());
    bool inSuperscript = false;
    for (const QChar c : std::as_const(s)) {
        const ushort u = c.unicode();
        int sup = -1;
        switch (u) {
        case 0x2070: sup = 0; break;
        case 0x00B9: sup = 1; break;
        case 0x00B2: sup = 2; break;
        case 0x00B3: sup = 3; break;
        case 0x2074: sup = 4; break;
        case 0x2075: sup = 5; break;
        case 0x2076: sup = 6; break;
        case 0x2077: sup = 7; break;
        case 0x2078: sup = 8; break;
        case 0x2079: sup = 9; break;
        default: break;
        }
        const bool supSign = u == 0x207B || u == 0x207A;
        if (sup >= 0 || supSign) {
            if (!inSuperscript) t += QLatin1Char('^');
            inSuperscript = true;
            t += supSign ? (u == 0x207B ? QLatin1Char('-') : QLatin1Char('+')) : QChar(u'0' + sup);
            continue;
        }
        inSuperscript = false;
        if (u == 0x2212 || u == 0x2013 || u == 0x2014 || u == 0x2012 || u == 0x2010 || u == 0x2011) t += QLatin1Char('-');
        else if (u == 0x00D7 || u == 0x00B7 || u == 0x22C5 || u == 0x2217 || u == '*') t += QLatin1Char('x');
        else if (u == 0x00B5 || u == 0x03BC) t += QLatin1Char('u');
        else if (c.isSpace() || u == 0x2009 || u == 0x202F || u == 0x00A0) continue;
        else if (u == 0xFFFE || u == 0xFFFF) continue;
        else t += c;
    }
    if (t.isEmpty()) return false;
    // (A comma only between digits: "8," is a word of a sentence, not a number.)
    static const QRegularExpression plain(QStringLiteral("^([+-]?(?:\\d+(?:,\\d+|\\.\\d*)?|\\.\\d+))(?:[eE]([+-]?\\d+))?$"));
    static const QRegularExpression power(QStringLiteral("^([+-]?)10(?:\\^|\\*\\*)?\\{?([+-]?\\d+)\\}?$"));
    static const QRegularExpression timesPower(QStringLiteral("^([+-]?(?:\\d+(?:\\.\\d*)?|\\.\\d+))x10(?:\\^|\\*\\*)?\\{?([+-]?\\d+)\\}?$"));
    static const QRegularExpression withUnit(
        QStringLiteral("^([+-]?(?:\\d+(?:,\\d+|\\.\\d*)?|\\.\\d+))([fpnumkKMGT]?)(Hz|V|A|W|s|\\x{03A9}|F|H|dBm|dB|%|\\x{00B0}C|\\x{00B0})?$"));
    const auto number = [](QString text, double* out) {
        // A comma: thousands where three digits follow and something other
        // than a lone zero leads; otherwise a decimal point.
        if (text.contains(QLatin1Char(','))) {
            static const QRegularExpression thousands(QStringLiteral("^[+-]?[1-9]\\d{0,2}(,\\d{3})+$"));
            if (thousands.match(text).hasMatch()) text.remove(QLatin1Char(','));
            else text.replace(QLatin1Char(','), QLatin1Char('.'));
        }
        bool ok = false;
        *out = text.toDouble(&ok);
        return ok && std::isfinite(*out);
    };
    double v = 0;
    QRegularExpressionMatch m = plain.match(t);
    if (m.hasMatch()) {
        if (!number(m.captured(1), &v)) return false;
        if (!m.captured(2).isEmpty()) v *= std::pow(10.0, m.captured(2).toDouble());
    } else if (m = timesPower.match(t); m.hasMatch()) {
        if (!number(m.captured(1), &v)) return false;
        v *= std::pow(10.0, m.captured(2).toDouble());
    } else if (m = power.match(t); m.hasMatch()) {
        v = std::pow(10.0, m.captured(2).toDouble());
        if (m.captured(1) == QLatin1String("-")) v = -v;
    } else if (m = withUnit.match(t); m.hasMatch()) {
        if (!number(m.captured(1), &v)) return false;
        const QString prefix = m.captured(2);
        const QString unit = m.captured(3);
        // A lone letter and no unit could be a panel's label; these are not.
        static const QHash<QString, double> scales{{QStringLiteral("f"), 1e-15}, {QStringLiteral("p"), 1e-12}, {QStringLiteral("n"), 1e-9},
                                                   {QStringLiteral("u"), 1e-6},  {QStringLiteral("m"), 1e-3},  {QStringLiteral("k"), 1e3},
                                                   {QStringLiteral("K"), 1e3},   {QStringLiteral("M"), 1e6},   {QStringLiteral("G"), 1e9},
                                                   {QStringLiteral("T"), 1e12}};
        if (!prefix.isEmpty()) {
            // (dB and % take no prefix; "5 m" alone is milli.)
            if (unit == QLatin1String("%") || unit.startsWith(QLatin1String("dB"))) return false;
            v *= scales.value(prefix, 1);
        }
    } else return false;
    if (!std::isfinite(v)) return false;
    *value = v;
    return true;
}

// --- The source ------------------------------------------------------------

struct Source::Text {
    QString all;
    QList<QRectF> runs;
    int first = -1;
    QPointF firstAt;
    int last = -1;
    QPointF lastAt;
    QHash<int, QRectF> boxes;
    QHash<qint64, QRectF> words;   // by start << 20 | length
    int calls = 0;
};

Source::Source(QPdfDocument* pdf, const QString& file) : a_pdf(pdf)
{
    if (file.isEmpty()) return;
    QFile in(file);
    if (!in.open(QIODevice::ReadOnly)) {
        a_problem = tr("The file could not be read for its paths: %1").arg(in.errorString());
        return;
    }
    a_reader = pdf::vec::Reader::open(in.readAll(), &a_problem);
}

Source::~Source() = default;

bool Source::hasVectors() const { return a_reader != nullptr; }

const pdf::vec::Graphics& Source::graphics(int page)
{
    auto at = a_graphics.find(page);
    if (at != a_graphics.end()) return *at;
    pdf::vec::Graphics g;
    if (a_reader != nullptr) {
        QString why;
        a_reader->page(page, &g, &why);
        // The reader's own page against pdfium's: their sizes differ when
        // UserUnit or a box was read otherwise, and the page's points are
        // pdfium's everywhere else here.
        const QSizeF truth = a_pdf != nullptr ? a_pdf->pagePointSize(page) : QSizeF();
        if (!truth.isEmpty() && !g.size.isEmpty() && (std::abs(truth.width() - g.size.width()) > 0.5 || std::abs(truth.height() - g.size.height()) > 0.5)) {
            const double kx = truth.width() / g.size.width(), ky = truth.height() / g.size.height();
            for (pdf::vec::Path& p : g.paths) {
                for (pdf::vec::Part& part : p.parts)
                    for (QPointF& q : part.points) q = QPointF(q.x() * kx, q.y() * ky);
                p.origin = QPointF(p.origin.x() * kx, p.origin.y() * ky);
                if (!p.clip.isNull()) p.clip = QRectF(p.clip.x() * kx, p.clip.y() * ky, p.clip.width() * kx, p.clip.height() * ky);
                p.width *= std::sqrt(kx * ky);
            }
            for (pdf::vec::Picture& pic : g.pictures)
                for (QPointF& q : pic.corners) q = QPointF(q.x() * kx, q.y() * ky);
            g.size = truth;
        }
    }
    if (a_graphics.size() > 16) a_graphics.clear();
    at = a_graphics.insert(page, g);
    return *at;
}

QImage Source::render(int page, const QRectF& region, double scale) const
{
    if (a_pdf == nullptr || page < 0 || page >= a_pdf->pageCount() || !(scale > 0)) return {};
    const QSizeF paper = a_pdf->pagePointSize(page);
    const QSize full = (paper * scale).toSize();
    if (full.isEmpty()) return {};
    QRect rect = region.isNull() ? QRect(QPoint(0, 0), full)
                                 : QRectF(region.topLeft() * scale, region.size() * scale).toAlignedRect().intersected(QRect(QPoint(0, 0), full));
    if (rect.isEmpty() || qint64(rect.width()) * rect.height() > kMostPixels * 2) return {};
    QPdfDocumentRenderOptions options;
    options.setRenderFlags(QPdfDocumentRenderOptions::RenderFlag::Annotations);
    options.setScaledSize(full);
    options.setScaledClipRect(rect);
    const QImage image = a_pdf->render(page, rect.size(), options);
    if (image.isNull()) return image;
    QImage out(image.size(), QImage::Format_RGB32);
    out.fill(Qt::white);
    QPainter painter(&out);
    painter.drawImage(0, 0, image);
    painter.end();
    return out;
}

QList<Token> Source::tokens(int page, const QRectF& within)
{
    QList<Token> out;
    if (a_pdf == nullptr || page < 0 || page >= a_pdf->pageCount()) return out;
    auto have = a_text.find(page);
    if (have == a_text.end()) {
        auto t = std::make_shared<Text>();
        const QPdfSelection all = a_pdf->getAllText(page);
        t->all = all.text();
        for (const QPolygonF& run : all.bounds()) t->runs << run.boundingRect();
        const int n = int(t->all.size());
        for (int i = 0; i < std::min(n, 64) && t->first < 0; ++i)
            if (const QRectF b = a_pdf->getSelectionAtIndex(page, i, 1).boundingRectangle(); !b.isEmpty()) {
                t->first = i;
                t->firstAt = b.center();
                t->boxes.insert(i, b);
            }
        for (int i = n - 1; i >= std::max(t->first + 1, n - 64) && t->last < 0; --i)
            if (const QRectF b = a_pdf->getSelectionAtIndex(page, i, 1).boundingRectangle(); !b.isEmpty()) {
                t->last = i;
                t->lastAt = b.center();
                t->boxes.insert(i, b);
            }
        if (a_text.size() > 16) a_text.clear();
        have = a_text.insert(page, t);
    }
    Text& t = **have;
    if (t.all.isEmpty()) return out;
    // Which characters to ask about: a box a character costs a call, and the
    // runs meeting the place are few and together in the text.
    int firstRun = -1, lastRun = -1;
    for (int i = 0; i < t.runs.size(); ++i)
        if (t.runs.at(i).intersects(within)) {
            if (firstRun < 0) firstRun = i;
            lastRun = i;
        }
    if (firstRun < 0) return out;
    const auto hit = [&](QPointF point) -> int {
        if (t.first < 0) return -1;
        QPdfSelection s = a_pdf->getSelection(page, t.firstAt, point);
        if (s.isValid() && s.endIndex() > t.first) return s.endIndex();
        if (t.last > t.first) {
            s = a_pdf->getSelection(page, point, t.lastAt);
            if (s.isValid() && s.startIndex() >= t.first && s.startIndex() < t.last) return s.startIndex();
        }
        return -1;
    };
    int from = hit(t.runs.at(firstRun).center());
    int to = hit(t.runs.at(lastRun).center());
    if (from > to && to >= 0) std::swap(from, to);
    constexpr int kMostCharacters = 1500;
    if (from < 0 || to < 0) {
        // No hit test here (no widths for its fonts): the text from its start,
        // as far as is sensible.
        from = 0;
        to = std::min<int>(int(t.all.size()) - 1, kMostCharacters);
    } else {
        from = std::max(0, from - 24);
        to = std::min<int>(int(t.all.size()) - 1, to + 24);
        if (to - from > kMostCharacters) to = from + kMostCharacters;
    }
    const auto boxOf = [&](int i) {
        if (const auto b = t.boxes.constFind(i); b != t.boxes.cend()) return *b;
        const QRectF b = a_pdf->getSelectionAtIndex(page, i, 1).boundingRectangle();
        t.boxes.insert(i, b);
        return b;
    };
    // Characters into words, by place: tick labels are often one line of the
    // text with nothing between them but distance.
    Token cur;
    double curHeight = 0;
    bool raised = false;
    QRectF base;
    const auto flush = [&] {
        if (!cur.text.isEmpty()) out.append(cur);
        cur = Token{};
        curHeight = 0;
        raised = false;
    };
    const auto isGap = [](QChar c) { return c.isSpace() || c == QChar(0xFFFE) || c == QChar(0xFFFF) || c.isNull(); };
    // A word's box takes one call to pdfium, a character's one each: words are
    // read whole, but where a power of ten may be raised in one ("10-3",
    // "104" set as 10 and a raised 4), character by character.
    static const QRegularExpression maybeRaised(QStringLiteral("10[-+\\x{2212}]?\\d"));
    int pendingChars = -1;   // read character by character up to here
    for (int i = from; i <= to; ++i) {
        const QChar c = t.all.at(i);
        if (i > pendingChars && !isGap(c) && cur.text.isEmpty() && (i == from || isGap(t.all.at(i - 1)))) {
            int e = i;
            while (e + 1 <= to && !isGap(t.all.at(e + 1))) ++e;
            const QString word = t.all.mid(i, e - i + 1);
            // (Short ones - most tick labels - are as cheap by their characters;
            // and the text may run two lines together with nothing between,
            // which a word's box taller than its first letter shows.)
            if (word.size() > 3 && (!word.contains(maybeRaised) || word.size() > 12)) {
                const qint64 key = (qint64(i) << 20) | qint64(e - i + 1);
                QRectF b;
                if (const auto have = t.words.constFind(key); have != t.words.cend()) b = *have;
                else {
                    b = a_pdf->getSelectionAtIndex(page, i, e - i + 1).boundingRectangle();
                    t.words.insert(key, b);
                }
                const QRectF first = boxOf(i);
                const bool oneLine = !first.isNull() && b.height() <= 1.6 * std::max(first.height(), 3.0);
                if (!oneLine) {
                    // Character by character, as below.
                    --i;
                    cur.text = QString();
                    pendingChars = e;
                    continue;
                }
                if (!b.isNull() && (b.adjusted(-0.5, -0.5, 0.5, 0.5).intersects(within) || within.contains(b.center()))) {
                    Token word_;
                    word_.text = word;
                    word_.box = b;
                    out.append(word_);
                }
                i = e;
                continue;
            }
        }
        if (isGap(c)) {
            flush();
            continue;
        }
        const QRectF b = boxOf(i);
        // (pdfium's boxes come in whole points: an "i" can be as wide as nothing.
        // It is still a letter of the word it is in.)
        if (b.isNull() || (b.width() <= 0 && b.height() <= 0)) {
            flush();
            continue;
        }
        if (!b.adjusted(-0.5, -0.5, 0.5, 0.5).intersects(within) && !within.contains(b.center())) {
            flush();
            continue;
        }
        const double height = std::max({b.height(), curHeight, 3.0});
        if (!cur.text.isEmpty()) {
            const double dx = std::max({0.0, b.left() - cur.box.right(), cur.box.left() - b.right()});
            const double dy = std::max({0.0, b.top() - cur.box.bottom(), cur.box.top() - b.bottom()});
            if (std::max(dx, dy) > 0.55 * height) flush();
        }
        // A smaller digit raised after a "10": its power.
        if (!cur.text.isEmpty() && !raised && cur.text.endsWith(QLatin1String("10")) && curHeight > 0 && b.height() < 0.85 * curHeight &&
            b.center().y() < base.center().y() - 0.1 * curHeight && (c.isDigit() || c == QLatin1Char('-') || c == QChar(0x2212))) {
            cur.text += QLatin1Char('^');
            raised = true;
        }
        if (cur.text.isEmpty()) {
            cur.box = b;
            base = b;
        } else cur.box = cur.box.united(b);
        cur.text += c;
        if (!raised && c.isDigit()) {
            curHeight = std::max(curHeight, b.height());
            base = b;
        } else if (!raised) curHeight = std::max(curHeight, b.height() > 2 ? b.height() : 0.0);
    }
    flush();
    // "10" then, apart in the text, a small number raised beside it.
    QList<Token> joined;
    for (int i = 0; i < out.size(); ++i) {
        Token tk = out.at(i);
        double v = 0;
        if (i + 1 < out.size() && (tk.text.endsWith(QLatin1String("10"))) && parseTickLabel(out.at(i + 1).text, &v)) {
            const Token& next = out.at(i + 1);
            const double h = tk.box.height();
            if (std::abs(v) < 400 && v == std::floor(v) && next.box.left() - tk.box.right() < 0.8 * h && next.box.left() >= tk.box.left() + 0.5 * tk.box.width() &&
                next.box.center().y() < tk.box.center().y() - 0.1 * h && next.box.height() <= h) {
                tk.text += QLatin1Char('^') + next.text;
                tk.box = tk.box.united(next.box);
                ++i;
            }
        }
        joined.append(tk);
    }
    for (Token& tk : joined) tk.numeric = parseTickLabel(tk.text, &tk.value);
    return joined;
}

// --- Frames and axes -------------------------------------------------------

namespace {

constexpr double kLabelReachX = 42;    // points below the frame for x's labels
constexpr double kLabelReachY = 95;    // points beside it for y's
constexpr double kEndReach = 3;       // a label this near an end of the frame is centred on it

struct Lines {
    QList<HLine> horizontal, vertical;
    QList<HLine> shortH, shortV;
};

} // namespace

void collectVectorLines(const pdf::vec::Graphics& g, const QTransform& work, QList<HLine>* longH, QList<HLine>* longV, QList<HLine>* shortH, QList<HLine>* shortV)
{
    Q_UNUSED(work)
    const double pageArea = g.size.width() * g.size.height();
    const auto add = [&](QPointF a, QPointF b, double width, const QColor& colour) {
        const double dx = std::abs(a.x() - b.x()), dy = std::abs(a.y() - b.y());
        if (dy <= 0.25 && dx >= 0.8) {
            HLine l{(a.y() + b.y()) / 2, std::min(a.x(), b.x()), std::max(a.x(), b.x()), width, colour};
            (dx >= kMostTickLength ? longH : shortH)->append(l);
        } else if (dx <= 0.25 && dy >= 0.8) {
            HLine l{(a.x() + b.x()) / 2, std::min(a.y(), b.y()), std::max(a.y(), b.y()), width, colour};
            (dy >= kMostTickLength ? longV : shortV)->append(l);
        }
    };
    for (const pdf::vec::Path& p : g.paths) {
        for (const pdf::vec::Part& part : p.parts) {
            const QPolygonF& q = part.points;
            if (q.size() < 2 || q.size() > 6 || part.curved) continue;
            const QRectF box = q.boundingRect();
            // A filled box thin enough is a line drawn so.
            if (p.filled && !p.stroked && box.width() <= 2.5 && box.height() >= 0.8 && q.size() >= 4) {
                add(QPointF(box.center().x(), box.top()), QPointF(box.center().x(), box.bottom()), box.width(), p.fill);
                continue;
            }
            if (p.filled && !p.stroked && box.height() <= 2.5 && box.width() >= 0.8 && q.size() >= 4) {
                add(QPointF(box.left(), box.center().y()), QPointF(box.right(), box.center().y()), box.height(), p.fill);
                continue;
            }
            // The page's own background is no frame.
            if (box.width() * box.height() > 0.97 * pageArea) continue;
            const bool invisible = !p.stroked && !p.filled;
            if (invisible) continue;
            const QColor colour = p.stroked ? p.stroke : p.fill;
            if (p.stroked && colour.lightness() > 250) continue;
            for (int i = 0; i + 1 < q.size(); ++i) add(q.at(i), q.at(i + 1), p.width, colour);
            if (part.closed && q.size() > 2) add(q.last(), q.first(), p.width, colour);
        }
    }
}

namespace {

} // namespace

// Lines at one place and overlapping or touching, as one.
QList<HLine> merged(QList<HLine> lines, double tolerance, double gap)
{
    std::sort(lines.begin(), lines.end(), [](const HLine& a, const HLine& b) { return a.at != b.at ? a.at < b.at : a.from < b.from; });
    QList<HLine> out;
    int start = 0;
    while (start < lines.size()) {
        int end = start + 1;
        while (end < lines.size() && lines.at(end).at - lines.at(end - 1).at <= tolerance) ++end;
        QList<HLine> group = lines.mid(start, end - start);
        std::sort(group.begin(), group.end(), [](const HLine& a, const HLine& b) { return a.from < b.from; });
        HLine cur = group.first();
        double weight = cur.to - cur.from, sum = cur.at * weight;
        const auto push = [&] {
            cur.at = weight > 0 ? sum / weight : cur.at;
            out.append(cur);
        };
        for (int i = 1; i < group.size(); ++i) {
            const HLine& g = group.at(i);
            if (g.from <= cur.to + gap) {
                cur.to = std::max(cur.to, g.to);
                cur.width = std::max(cur.width, g.width);
                weight += g.to - g.from;
                sum += g.at * (g.to - g.from);
            } else {
                push();
                cur = g;
                weight = cur.to - cur.from;
                sum = cur.at * weight;
            }
        }
        push();
        start = end;
    }
    return out;
}

struct Candidate {
    QRectF box;
    double width = 0;
};

// The rectangles the long lines make, by the corners where they meet.
QList<QRectF> rectanglesOf(const QList<HLine>& h, const QList<HLine>& v, double minWidth, double minHeight)
{
    QList<QRectF> out;
    constexpr double t = 3;
    for (const HLine& a : h) {
        if (a.to - a.from < minWidth) continue;
        for (const HLine& b : v) {
            if (b.to - b.from < minHeight) continue;
            const bool atStart = std::abs(b.at - a.from) <= t, atEnd = std::abs(b.at - a.to) <= t;
            if (!atStart && !atEnd) continue;
            const bool atTop = std::abs(a.at - b.from) <= t, atBottom = std::abs(a.at - b.to) <= t;
            if (!atTop && !atBottom) continue;
            // The box by where the lines are, not how far they run: a tick at
            // a corner carries a line on past it. Its far sides by the lines
            // there, if any.
            double left = atStart ? b.at : a.from, right = atEnd ? b.at : a.to;
            double top = atTop ? a.at : b.from, bottom = atBottom ? a.at : b.to;
            // (The nearest that runs along most of that side: not a line of
            // another plot beside it at about the same height.)
            const auto farSide = [](const QList<HLine>& lines, double estimate, double from, double to, double least) {
                double best = estimate, nearest = 6.0001;
                for (const HLine& other : lines) {
                    if (other.to - other.from < least) continue;
                    if (std::min(to, other.to) - std::max(from, other.from) < 0.5 * (to - from)) continue;
                    if (std::abs(other.at - estimate) < nearest) {
                        nearest = std::abs(other.at - estimate);
                        best = other.at;
                    }
                }
                return best;
            };
            if (atStart) right = farSide(v, right, std::min(top, bottom), std::max(top, bottom), minHeight);
            if (atEnd) left = farSide(v, left, std::min(top, bottom), std::max(top, bottom), minHeight);
            if (atTop) bottom = farSide(h, bottom, left, right, minWidth);
            if (atBottom) top = farSide(h, top, left, right, minWidth);
            const QRectF box(QPointF(left, top), QPointF(right, bottom));
            if (box.width() < minWidth || box.height() < minHeight) continue;
            bool seen = false;
            for (QRectF& o : out)
                if (std::abs(o.left() - box.left()) <= t && std::abs(o.right() - box.right()) <= t && std::abs(o.top() - box.top()) <= t &&
                    std::abs(o.bottom() - box.bottom()) <= t) {
                    seen = true;
                    break;
                }
            if (!seen) out << box;
            if (out.size() > 400) return out;
        }
    }
    return out;
}

namespace {

// The marks along an edge: short strokes meeting it, and grid lines crossing
// the frame. A side's marks are its own plus the grid's. \a vertical: an x
// axis's marks, which are vertical strokes.
QList<double> marksOn(const QRectF& frame, bool xAxis, int side, const QList<HLine>& shorts, const QList<HLine>& longs, double gridSpan = 0.5)
{
    QList<double> out;
    constexpr double t = 2.2;
    const double edge = xAxis ? (side == 0 ? frame.bottom() : frame.top()) : (side == 0 ? frame.left() : frame.right());
    const double low = xAxis ? frame.left() : frame.top(), high = xAxis ? frame.right() : frame.bottom();
    for (const HLine& s : shorts) {
        if (s.at < low - 1 || s.at > high + 1) continue;
        if (s.from - t > edge || s.to + t < edge) continue;
        out << s.at;
    }
    const double span = xAxis ? frame.height() : frame.width();
    const double crossLow = xAxis ? frame.top() : frame.left();
    for (const HLine& g : longs) {
        if (g.at < low - 1 || g.at > high + 1) continue;
        // (A grid line, not the frame's own edge - which is no tick unless
        // it is one, as an end of the axis often is.)
        if (g.to - g.from < gridSpan * span) continue;
        if (std::abs(g.at - low) < 1.5 || std::abs(g.at - high) < 1.5) continue;
        if (g.from > crossLow + 0.6 * span) continue;
        out << g.at;
    }
    std::sort(out.begin(), out.end());
    QList<double> unique;
    for (const double v : std::as_const(out))
        if (unique.isEmpty() || v - unique.last() > 0.4) unique << v;
    return unique;
}

// A straight line through position and scale(value), robustly: the pair that
// most others agree with, then least squares on those.
struct AxisLine {
    double slope = 0, intercept = 0, rms = 0;
    int kept = 0;
    QList<bool> used;
};

AxisLine fitLine(const QList<double>& p, const QList<double>& u, double tolerance)
{
    AxisLine best;
    const int n = int(p.size());
    best.used = QList<bool>(n, false);
    if (n < 2) return best;
    const auto refine = [&](double slope, double intercept) {
        AxisLine l;
        l.used = QList<bool>(n, false);
        for (int round = 0; round < 3; ++round) {
            double sp = 0, su = 0, spp = 0, spu = 0;
            int kept = 0;
            for (int i = 0; i < n; ++i) {
                const bool in = slope != 0 && std::abs((u.at(i) - intercept) / slope - p.at(i)) <= tolerance;
                l.used[i] = in;
                if (!in) continue;
                sp += p.at(i);
                su += u.at(i);
                spp += p.at(i) * p.at(i);
                spu += p.at(i) * u.at(i);
                ++kept;
            }
            l.kept = kept;
            if (kept < 2) return l;
            const double d = kept * spp - sp * sp;
            if (d <= 0) {
                l.kept = 0;
                return l;
            }
            slope = (kept * spu - sp * su) / d;
            intercept = (su - slope * sp) / kept;
        }
        l.slope = slope;
        l.intercept = intercept;
        double sum = 0;
        for (int i = 0; i < n; ++i)
            if (l.used.at(i) && slope != 0) {
                const double e = (u.at(i) - intercept) / slope - p.at(i);
                sum += e * e;
            }
        l.rms = l.kept > 0 ? std::sqrt(sum / l.kept) : 0;
        return l;
    };
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j) {
            if (std::abs(p.at(j) - p.at(i)) < 1 || u.at(i) == u.at(j)) continue;
            const double s = (u.at(j) - u.at(i)) / (p.at(j) - p.at(i));
            const AxisLine l = refine(s, u.at(i) - s * p.at(i));
            if (l.kept > best.kept || (l.kept == best.kept && l.kept >= 2 && l.rms < best.rms)) best = l;
        }
    return best;
}

// The numbers of one side's labels, as ticks; the marks they sit on found.
// (\a ends: the frame's ends along the axis, where a label is often written
// with no tick mark of its own - its first and last.)
bool fitAxisFrom(const QList<Token>& labels, const QList<double>& marks, bool xAxis, Axis* axis, const QList<double>& ends = {})
{
    axis->marks = marks;
    axis->ticks.clear();
    axis->found = false;
    if (labels.size() < 2) return false;
    QList<Token> sorted = labels;
    std::sort(sorted.begin(), sorted.end(), [xAxis](const Token& a, const Token& b) {
        return xAxis ? a.box.center().x() < b.box.center().x() : a.box.center().y() < b.box.center().y();
    });
    QList<double> gaps;
    for (int i = 0; i + 1 < sorted.size(); ++i)
        gaps << (xAxis ? sorted.at(i + 1).box.center().x() - sorted.at(i).box.center().x() : sorted.at(i + 1).box.center().y() - sorted.at(i).box.center().y());
    std::sort(gaps.begin(), gaps.end());
    const double typical = gaps.isEmpty() ? 20 : gaps.at(gaps.size() / 2);
    const double snap = std::clamp(typical * 0.4, 3.0, 12.0);
    QList<double> positions, valuesOf;
    int onMarks = 0;
    for (const Token& t : std::as_const(sorted)) {
        const double centre = xAxis ? t.box.center().x() : t.box.center().y();
        double best = centre, distance = snap;
        bool on = false;
        for (const double m : marks)
            if (std::abs(m - centre) < distance) {
                distance = std::abs(m - centre);
                best = m;
                on = true;
            }
        for (const double e : ends)
            if (std::abs(e - centre) <= std::min(distance, kEndReach)) {
                distance = std::abs(e - centre);
                best = e;
                on = true;
            }
        Tick tick;
        tick.position = best;
        tick.hasValue = true;
        tick.value = t.value;
        tick.label = t.text;
        tick.onMark = on;
        axis->ticks << tick;
        positions << best;
        valuesOf << t.value;
        onMarks += on;
    }
    // Linear, and log where every label is above 0.
    const double tolerance = onMarks * 2 >= int(sorted.size()) ? 1.2 : 2.5;
    const AxisLine lin = fitLine(positions, valuesOf, tolerance);
    QList<double> logs;
    bool allPositive = true;
    for (const double v : std::as_const(valuesOf)) {
        allPositive = allPositive && v > 0;
        logs << (v > 0 ? std::log10(v) : 0.0);
    }
    AxisLine lg;
    if (allPositive) lg = fitLine(positions, logs, tolerance);
    bool useLog = false;
    if (lg.kept > lin.kept) useLog = true;
    else if (lg.kept == lin.kept && lg.kept >= 2 && lg.rms < lin.rms * 0.7 - 1e-9) useLog = true;
    axis->scaleGuessed = false;
    const AxisLine& chosen = useLog ? lg : lin;
    if (chosen.kept < 2) return false;
    if (chosen.kept == 2) {
        // Two labels fit anything. Marks between them tell log from linear:
        // uneven marks crowding one way are a log axis's. Powers of ten far
        // apart say so as well.
        bool looksLog = false;
        QList<double> between;
        double low = 1e300, high = -1e300;
        int a = -1, b = -1;
        for (int i = 0; i < chosen.used.size(); ++i)
            if (chosen.used.at(i)) (a < 0 ? a : b) = i;
        if (a >= 0 && b >= 0) {
            low = std::min(positions.at(a), positions.at(b));
            high = std::max(positions.at(a), positions.at(b));
            for (const double m : marks)
                if (m > low + 0.5 && m < high - 0.5) between << m;
            if (between.size() >= 3) {
                // A decade's marks crowd one way, each gap smaller than the
                // last; a linear axis's are even, but for a mark not found
                // (a gap twice the others).
                QList<double> gaps;
                double previous = low;
                between << high;
                for (const double m : std::as_const(between)) {
                    gaps << m - previous;
                    previous = m;
                }
                int falling = 0, rising = 0;
                for (int i = 0; i + 1 < gaps.size(); ++i) {
                    falling += gaps.at(i + 1) < 0.97 * gaps.at(i);
                    rising += gaps.at(i + 1) > gaps.at(i) / 0.97;
                }
                const int steps = int(gaps.size()) - 1;
                const auto [least, most] = std::minmax_element(gaps.begin(), gaps.end());
                looksLog = *most > 1.8 * *least && std::max(falling, rising) >= std::max(2, int(std::ceil(0.8 * steps)));
            }
            const double va = valuesOf.at(a), vb = valuesOf.at(b);
            if (va > 0 && vb > 0) {
                const double ratio = std::log10(vb / va);
                if (std::abs(ratio) >= 1 && std::abs(ratio - std::round(ratio)) < 1e-6 && between.size() < 3 && std::abs(std::log10(va) - std::round(std::log10(va))) < 1e-6) looksLog = true;
            }
        }
        useLog = allPositive && looksLog;
        axis->scaleGuessed = true;
        // Two labels and no marks to place them by: nothing to go on.
        if (marks.isEmpty()) return false;
    }
    axis->scale = useLog ? Scale::Log : Scale::Linear;
    const AxisLine& line = useLog ? lg : lin;
    int kept = 0;
    for (int i = 0; i < axis->ticks.size(); ++i) {
        axis->ticks[i].used = line.used.value(i);
        kept += axis->ticks.at(i).used;
    }
    axis->fitError = line.rms;
    axis->found = kept >= 2;
    return axis->found;
}

// The labels of a side: the nearest row (or column) of numbers to the frame.
QList<Token> labelsOn(const QList<Token>& tokens, const QRectF& frame, bool xAxis, int side)
{
    QList<Token> near;
    const double margin = xAxis ? frame.width() * 0.08 : frame.height() * 0.08;
    for (const Token& t : tokens) {
        if (!t.numeric) continue;
        const QPointF c = t.box.center();
        if (xAxis) {
            if (c.x() < frame.left() - margin || c.x() > frame.right() + margin) continue;
            const double d = side == 0 ? c.y() - frame.bottom() : frame.top() - c.y();
            if (d < -0.5 || d > kLabelReachX) continue;
        } else {
            if (c.y() < frame.top() - margin || c.y() > frame.bottom() + margin) continue;
            const double d = side == 0 ? frame.left() - c.x() : c.x() - frame.right();
            if (d < -0.5 || d > kLabelReachY) continue;
        }
        near << t;
    }
    if (near.isEmpty()) return near;
    // Sorted by distance: the rows from the frame out, the nearest row that
    // has two numbers or more.
    const auto distance = [&](const Token& t) {
        if (xAxis) return side == 0 ? t.box.top() - frame.bottom() : frame.top() - t.box.bottom();
        return side == 0 ? frame.left() - t.box.right() : t.box.left() - frame.right();
    };
    std::sort(near.begin(), near.end(), [&](const Token& a, const Token& b) { return distance(a) < distance(b); });
    constexpr double rowTolerance = 7;
    int i = 0;
    while (i < near.size()) {
        const double base = xAxis ? near.at(i).box.center().y() : near.at(i).box.right();
        QList<Token> row;
        int j = i;
        for (; j < near.size(); ++j) {
            const Token& t = near.at(j);
            if (std::abs((xAxis ? t.box.center().y() : (side == 0 ? t.box.right() : t.box.left())) - (xAxis ? base : (side == 0 ? near.at(i).box.right() : near.at(i).box.left()))) <= rowTolerance) row << t;
        }
        if (row.size() >= 2) return row;
        ++i;
    }
    return {};
}

// A power of ten written apart at the end of an axis: "×10⁻³", "1e6".
double multiplierFor(const QList<Token>& tokens, const QRectF& frame, bool xAxis, const QList<Token>& labels, QString* text)
{
    static const QRegularExpression pattern(QStringLiteral("^(?:[x\\x{00D7}]\\s*10\\^?[-+\\x{2212}]?\\d+|[+\\x{00D7}x]?\\s*1[eE][-+\\x{2212}]?\\d+)$"));
    for (const Token& t : tokens) {
        if (labels.contains(t)) continue;
        if (!pattern.match(t.text.trimmed()).hasMatch()) continue;
        const QPointF c = t.box.center();
        const bool place = xAxis ? (c.x() > frame.right() - 0.25 * frame.width() && c.x() < frame.right() + 40 && c.y() > frame.bottom() - 2 && c.y() < frame.bottom() + 45)
                                 : (c.y() < frame.top() + 0.1 * frame.height() && c.y() > frame.top() - 30 && c.x() > frame.left() - 50 && c.x() < frame.left() + 0.3 * frame.width());
        if (!place) continue;
        double v = 0;
        QString clean = t.text;
        clean.remove(QLatin1Char('x')).remove(QChar(0x00D7)).remove(QLatin1Char('+'));
        if (parseTickLabel(clean, &v) && v > 0) {
            *text = t.text;
            return v;
        }
    }
    return 1;
}

} // namespace

bool operator==(const Token& a, const Token& b) { return a.text == b.text && a.box == b.box; }

// The labels that stand by a tick mark - within half the marks' spacing of
// one - or, where no marks were found, evenly spaced in one row of one size.
// The rest is text that happens to be near: a page number, a caption.
// The frame's two ends along an axis.
QList<double> endsOf(const QRectF& frame, bool xAxis) { return xAxis ? QList<double>{frame.left(), frame.right()} : QList<double>{frame.top(), frame.bottom()}; }

// A label centred on an end of the frame, outside it on the axis' side: the
// axis' first or last, written at the corner with no tick of its own ("0.0"
// under the corner; not the other axis's label beside it).
bool atAnEnd(const Token& t, const QRectF& frame, bool xAxis, int side)
{
    const QPointF c = t.box.center();
    if (xAxis) {
        const bool outside = side == 0 ? t.box.top() >= frame.bottom() - 0.5 : t.box.bottom() <= frame.top() + 0.5;
        return outside && (std::abs(c.x() - frame.left()) <= kEndReach || std::abs(c.x() - frame.right()) <= kEndReach);
    }
    const bool outside = side == 0 ? t.box.right() <= frame.left() + 0.5 : t.box.left() >= frame.right() - 0.5;
    return outside && (std::abs(c.y() - frame.top()) <= kEndReach || std::abs(c.y() - frame.bottom()) <= kEndReach);
}

static QList<Token> alignedLabels(const QList<Token>& labels, const QList<double>& marks, bool xAxis, const QRectF& frame, int side)
{
    if (labels.isEmpty()) return labels;
    const auto along = [xAxis](const Token& t) { return xAxis ? t.box.center().x() : t.box.center().y(); };
    if (marks.size() >= 2) {
        QList<double> gaps;
        for (int i = 0; i + 1 < marks.size(); ++i) gaps << marks.at(i + 1) - marks.at(i);
        std::sort(gaps.begin(), gaps.end());
        const double reach = std::clamp(gaps.at(gaps.size() / 2) / 2, 2.0, 12.0);
        QList<Token> kept;
        for (const Token& t : labels) {
            double nearest = 1e9;
            for (const double m : marks) nearest = std::min(nearest, std::abs(m - along(t)));
            if (nearest <= reach || atAnEnd(t, frame, xAxis, side)) kept << t;
        }
        return kept;
    }
    if (labels.size() < 3) return {};
    QList<Token> sorted = labels;
    std::sort(sorted.begin(), sorted.end(), [&](const Token& a, const Token& b) { return along(a) < along(b); });
    QList<double> gaps;
    for (int i = 0; i + 1 < sorted.size(); ++i) gaps << along(sorted.at(i + 1)) - along(sorted.at(i));
    const auto [low, high] = std::minmax_element(gaps.begin(), gaps.end());
    if (*low <= 0 || *high > 1.25 * *low) return {};
    double hLow = 1e9, hHigh = 0;
    for (const Token& t : std::as_const(sorted)) {
        hLow = std::min(hLow, t.box.height());
        hHigh = std::max(hHigh, t.box.height());
    }
    if (hHigh > 1.3 * hLow + 1) return {};
    return sorted;
}

void readAxes(Plot& plot, const QList<Token>& tokens, const QList<HLine>& shortV, const QList<HLine>& shortH, const QList<HLine>& longV, const QList<HLine>& longH)
{
    const QRectF& frame = plot.frame;
    // Each axis from the side its labels are on.
    for (const bool xAxis : {true, false}) {
        Axis best;
        int bestCount = -1;
        // (In a picture a steep curve is a long run too: a grid line there runs
        // the whole frame.)
        const double gridSpan = plot.route == QLatin1String("raster") ? 0.92 : 0.5;
        const QList<double> marksAt[2] = {marksOn(frame, xAxis, 0, xAxis ? shortV : shortH, xAxis ? longV : longH, gridSpan),
                                          marksOn(frame, xAxis, 1, xAxis ? shortV : shortH, xAxis ? longV : longH, gridSpan)};
        for (const int side : {0, 1}) {
            const QList<double> marks = marksAt[side];
            QList<Token> labels = alignedLabels(labelsOn(tokens, frame, xAxis, side), marks, xAxis, frame, side);
            // Labels at the top (or the right) with no marks there, and marks
            // at the bottom (or the left): a running header, a caption.
            if (side == 1 && marks.size() < 2 && marksAt[0].size() >= 2) labels.clear();
            Axis axis;
            axis.side = side;
            fitAxisFrom(labels, marks, xAxis, &axis, endsOf(frame, xAxis));
            int count = 0;
            for (const Tick& t : std::as_const(axis.ticks)) count += t.used;
            // (The side with the most marks when no labels decide: a scan's.)
            const bool better = count > bestCount || (count == bestCount && axis.marks.size() > best.marks.size());
            if (better) {
                if (count >= 2) {
                    QString said;
                    const double m = multiplierFor(tokens, frame, xAxis, labels, &said);
                    if (m != 1) {
                        axis.multiplier = m;
                        plot.notes << tr("%1 at the end of the %2 axis is taken as a multiplier of its labels.").arg(said, xAxis ? QStringLiteral("x") : QStringLiteral("y"));
                    }
                }
                best = axis;
                bestCount = count;
            }
        }
        // Labels on the other side too, saying something else: a second axis.
        {
            const QList<Token> other = labelsOn(tokens, frame, xAxis, best.side == 0 ? 1 : 0);
            if (best.found && other.size() >= 2) {
                Axis second;
                second.side = best.side == 0 ? 1 : 0;
                const QList<double> marks = marksOn(frame, xAxis, second.side, xAxis ? shortV : shortH, xAxis ? longV : longH, gridSpan);
                bool differs = fitAxisFrom(other, marks, xAxis, &second, endsOf(frame, xAxis)) && second.ticks.size() != best.ticks.size();
                for (int i = 0; second.found && !differs && i < second.ticks.size(); ++i)
                    differs = std::abs(second.ticks.at(i).value - best.ticks.at(i).value) > 1e-12 * std::abs(best.ticks.at(i).value);
                if (differs)
                    plot.notes << tr("A second %1 axis runs along the other side; the numbers here use the one at the %2. A curve read against the other takes 'references' from that one.")
                                      .arg(xAxis ? QStringLiteral("x") : QStringLiteral("y"),
                                           xAxis ? (best.side == 0 ? QStringLiteral("bottom") : QStringLiteral("top")) : (best.side == 0 ? QStringLiteral("left") : QStringLiteral("right")));
            }
        }
        // A title: the first line of words beyond the labels, on their side -
        // not the caption under it, nor a page's running header.
        {
            double far = 0;   // how far out the labels reach
            for (const Token& t : labelsOn(tokens, frame, xAxis, best.side)) {
                const double d = xAxis ? (best.side == 0 ? t.box.bottom() - frame.bottom() : frame.top() - t.box.top())
                                       : (best.side == 0 ? frame.left() - t.box.left() : t.box.right() - frame.right());
                far = std::max(far, d);
            }
            QList<Token> words;
            for (const Token& t : tokens) {
                if (t.numeric) continue;
                const QPointF c = t.box.center();
                const double d = xAxis ? (best.side == 0 ? c.y() - frame.bottom() : frame.top() - c.y()) : (best.side == 0 ? frame.left() - c.x() : c.x() - frame.right());
                const bool along = xAxis ? (c.x() > frame.left() && c.x() < frame.right()) : (c.y() > frame.top() && c.y() < frame.bottom());
                if (along && d > std::max(2.0, far - 2) && d < far + (xAxis ? 22 : 30)) words << t;
            }
            // The nearest line of them.
            std::sort(words.begin(), words.end(), [&](const Token& a, const Token& b) {
                return xAxis ? std::abs(a.box.center().y() - frame.center().y()) < std::abs(b.box.center().y() - frame.center().y())
                             : std::abs(a.box.center().x() - frame.center().x()) < std::abs(b.box.center().x() - frame.center().x());
            });
            QList<Token> line;
            for (const Token& t : std::as_const(words)) {
                if (line.isEmpty()) {
                    line << t;
                    continue;
                }
                const double off = xAxis ? std::abs(t.box.center().y() - line.first().box.center().y()) : std::abs(t.box.center().x() - line.first().box.center().x());
                if (off <= 0.6 * std::max(4.0, xAxis ? line.first().box.height() : line.first().box.width())) line << t;
            }
            std::sort(line.begin(), line.end(), [&](const Token& a, const Token& b) { return xAxis ? a.box.left() < b.box.left() : a.box.bottom() > b.box.bottom(); });
            QStringList title;
            for (const Token& t : std::as_const(line)) title << t.text;
            static const QRegularExpression caption(QStringLiteral("^(Fig\\.?|Figure|FIG\\.?|\\([a-z]\\))"));
            if (!title.isEmpty() && caption.match(title.first()).hasMatch()) title.clear();
            best.title = title.join(QLatin1Char(' ')).left(120);
        }
        (xAxis ? plot.xAxis : plot.yAxis) = best;
    }
}

bool calibrateFromAxes(Plot& plot, QString* why)
{
    Calibration& c = plot.calibration;
    c.references.clear();
    c.kind = Kind::Cartesian;
    c.model = Model::Affine;
    c.reset();
    if (plot.frame.isNull()) {
        if (why != nullptr) *why = tr("No frame was found to put the axes on.");
        return false;
    }
    const QTransform back = plot.work.inverted();
    const auto add = [&](const Axis& axis, bool xAxis) {
        const double edge = xAxis ? (axis.side == 0 ? plot.frame.bottom() : plot.frame.top()) : (axis.side == 0 ? plot.frame.left() : plot.frame.right());
        for (const Tick& t : axis.ticks) {
            if (!t.used || !t.hasValue) continue;
            Reference r;
            r.at = back.map(xAxis ? QPointF(t.position, edge) : QPointF(edge, t.position));
            (xAxis ? r.hasX : r.hasY) = true;
            (xAxis ? r.x : r.y) = t.value * axis.multiplier;
            c.references << r;
        }
    };
    add(plot.xAxis, true);
    add(plot.yAxis, false);
    c.xScale = plot.xAxis.scale;
    c.yScale = plot.yAxis.scale;
    return c.fit(why);
}

bool assignTickValues(Plot& plot, bool xAxis, const QList<double>& given, QString* why)
{
    Axis& axis = xAxis ? plot.xAxis : plot.yAxis;
    const QList<double> marks = axis.marks;
    if (marks.size() < 2 || given.size() < 2) {
        if (why != nullptr) *why = tr("The %1 axis has %2 tick marks found and %3 values given: it takes two of each.").arg(xAxis ? QStringLiteral("x") : QStringLiteral("y")).arg(marks.size()).arg(given.size());
        return false;
    }
    if (marks.size() != given.size()) {
        if (why != nullptr)
            *why = tr("The %1 axis has %2 tick marks found, at %3 (points); %4 values were given. Give one for each, or give 'references' instead.")
                       .arg(xAxis ? QStringLiteral("x") : QStringLiteral("y"))
                       .arg(marks.size())
                       .arg([&] {
                           QStringList at;
                           for (const double m : marks) at << QString::number(m, 'f', 1);
                           return at.join(QStringLiteral(", "));
                       }())
                       .arg(given.size());
        return false;
    }
    axis.ticks.clear();
    for (int i = 0; i < marks.size(); ++i) {
        Tick t;
        t.position = marks.at(i);
        t.hasValue = true;
        t.value = given.at(i);
        t.label = QString::number(given.at(i), 'g', 12);
        t.onMark = true;
        t.used = true;
        axis.ticks << t;
    }
    QList<double> logs;
    bool positive = true;
    for (const double v : given) {
        positive = positive && v > 0;
        logs << (v > 0 ? std::log10(v) : 0);
    }
    const AxisLine lin = fitLine(marks, given, 1e9);
    const AxisLine lg = positive ? fitLine(marks, logs, 1e9) : AxisLine{};
    axis.scale = positive && lg.rms < lin.rms * 0.7 ? Scale::Log : Scale::Linear;
    axis.fitError = axis.scale == Scale::Log ? lg.rms : lin.rms;
    axis.found = true;
    axis.scaleGuessed = given.size() == 2;
    return true;
}

// --- Finding the plots -----------------------------------------------------

QList<Plot> findPlots(Source& source, int page, const QRectF& within)
{
    QList<Plot> out;
    QPdfDocument* pdf = source.pdf();
    if (pdf == nullptr || page < 0 || page >= pdf->pageCount()) return out;
    const QSizeF paper = pdf->pagePointSize(page);
    const QRectF whole(QPointF(0, 0), paper);
    const pdf::vec::Graphics& g = source.graphics(page);

    // The picture's share of the page: a scanned page, or a figure pasted in.
    double pictureShare = 0;
    for (const pdf::vec::Picture& p : g.pictures) {
        const QRectF box = p.corners.boundingRect().intersected(whole);
        pictureShare = std::max(pictureShare, box.width() * box.height() / std::max(1.0, paper.width() * paper.height()));
    }

    struct Way {
        QTransform work;
        double angle = 0;
        QString route;
        QList<HLine> longH, longV, shortH, shortV;
        double renderScale = 0;
        const RasterLines* picture = nullptr;
    };
    QList<Way> ways;

    Way vector;
    vector.route = QStringLiteral("vector");
    collectVectorLines(g, QTransform(), &vector.longH, &vector.longV, &vector.shortH, &vector.shortV);
    if (!vector.longH.isEmpty() && !vector.longV.isEmpty()) ways << vector;
    bool pictureMatters = pictureShare > 0.02 || g.paths.isEmpty() || !source.hasVectors();

    const double minW = std::max(48.0, paper.width() * 0.09), minH = std::max(36.0, paper.height() * 0.05);
    const auto tryWay = [&](const Way& way) {
        const QList<HLine> h = merged(way.longH, 1.0, 3.0), v = merged(way.longV, 1.0, 3.0);
        const QList<QRectF> rects = rectanglesOf(h, v, minW, minH);
        struct Found {
            Plot plot;
            int score;
        };
        QList<Found> founds;
        for (const QRectF& box : rects) {
            if (!within.isNull() && !within.intersects(box)) continue;
            // (The page's border, not a plot.)
            if (way.work.isIdentity() && box.width() > 0.97 * paper.width() && box.height() > 0.97 * paper.height()) continue;
            if (box.width() / box.height() > 10 || box.height() / box.width() > 10) continue;
            Plot plot;
            plot.page = page;
            plot.work = way.work;
            plot.angle = way.angle;
            plot.frame = box;
            plot.route = way.route;
            plot.renderScale = way.renderScale;
            const QRectF reach = box.adjusted(-120, -40, 120, 70);
            const QRectF pageReach = way.work.isIdentity() ? reach : way.work.inverted().mapRect(reach);
            QList<Token> tokens = source.tokens(page, pageReach);
            if (!way.work.isIdentity())
                for (Token& t : tokens) t.box = way.work.mapRect(t.box);
            QList<HLine> shortV = way.shortV, shortH = way.shortH;
            // (In a picture the short strokes are anything: a curve's steep part
            // meeting the frame reads as one. Only what stands outside the frame.)
            if (way.picture != nullptr) {
                shortV.clear();
                shortH.clear();
                marksInPicture(*way.picture, box, &shortV, &shortH);
            }
            readAxes(plot, tokens, shortV, shortH, v, h);
            plot.region = (way.work.isIdentity() ? box : way.work.inverted().mapRect(box)).adjusted(-60, -20, 15, 50).intersected(whole);
            int labelled = 0;
            for (const Tick& t : std::as_const(plot.xAxis.ticks)) labelled += t.used;
            for (const Tick& t : std::as_const(plot.yAxis.ticks)) labelled += t.used;
            const bool bothAxes = plot.xAxis.found && plot.yAxis.found;
            // Marks enough to be an axis, its labels unread (a scan's).
            const bool marked = plot.xAxis.marks.size() >= 3 && plot.yAxis.marks.size() >= 3;
            // Or, in a picture, a box drawn whole with a few marks on it: a
            // plot whose ticks are small and mostly under its markers.
            const auto drawn = [](const QList<HLine>& lines, double at, double from, double to) {
                double sum = 0;
                for (const HLine& l : lines)
                    if (std::abs(l.at - at) <= 1.5) sum += std::max(0.0, std::min(to, l.to) - std::max(from, l.from));
                return sum >= 0.8 * (to - from);
            };
            const bool boxed = way.picture != nullptr && plot.xAxis.marks.size() + plot.yAxis.marks.size() >= 2 && drawn(h, box.top(), box.left(), box.right()) &&
                               drawn(h, box.bottom(), box.left(), box.right()) && drawn(v, box.left(), box.top(), box.bottom()) && drawn(v, box.right(), box.top(), box.bottom());
            if (!bothAxes && !marked && !boxed && within.isNull()) continue;
            if (bothAxes) {
                QString why;
                if (!calibrateFromAxes(plot, &why)) plot.notes << why;
            } else if (plot.xAxis.found != plot.yAxis.found) {
                plot.notes << tr("Only the %1 axis could be read from its labels.").arg(plot.xAxis.found ? QStringLiteral("x") : QStringLiteral("y"));
            } else {
                plot.notes << tr("No axis could be read from labels: its tick marks are found; 'x_values' and 'y_values' give them values.");
            }
            founds << Found{plot, labelled + (bothAxes ? 100 : 0)};
        }
        // The outermost of what contains what, unless the outer holds several
        // plots of its own (a figure's border round its panels) or is itself
        // no plot; a plot's legend box or inset does not count.
        QList<Found> kept;
        for (int i = 0; i < founds.size(); ++i) {
            bool drop = false;
            int inside = 0;
            for (int j = 0; j < founds.size(); ++j) {
                if (i == j) continue;
                const QRectF a = founds.at(i).plot.frame, b = founds.at(j).plot.frame;
                const bool iInJ = b.adjusted(-2, -2, 2, 2).contains(a) && a.width() * a.height() < b.width() * b.height();
                const bool jInI = a.adjusted(-2, -2, 2, 2).contains(b) && b.width() * b.height() < a.width() * a.height();
                if (iInJ && founds.at(i).score <= founds.at(j).score + 2) {
                    // Keep the one with labels of its own.
                    drop = true;
                }
                if (jInI && founds.at(j).plot.calibration.valid()) ++inside;
                // The same box twice over.
                if (a == b && j < i) drop = true;
            }
            if (inside >= 2) drop = true;
            if (!drop) kept << founds.at(i);
        }
        std::sort(kept.begin(), kept.end(), [](const Found& a, const Found& b) {
            const QRectF x = a.plot.frame, y = b.plot.frame;
            if (std::abs(x.top() - y.top()) > 10) return x.top() < y.top();
            return x.left() < y.left();
        });
        for (const Found& f : std::as_const(kept)) out << f.plot;
    };

    for (const Way& way : std::as_const(ways)) tryWay(way);

    // The picture's way, where the paths gave nothing or the page is a picture.
    if (out.isEmpty() && (pictureMatters || !within.isNull())) {
        const RasterLines raster = rasterLines(source, page, within);
        if (raster.valid) {
            Way way;
            way.route = QStringLiteral("raster");
            way.work = raster.work;
            way.angle = raster.angle;
            way.longH = raster.longH;
            way.longV = raster.longV;
            way.shortH = raster.shortH;
            way.shortV = raster.shortV;
            way.renderScale = raster.scale;
            way.picture = &raster;
            tryWay(way);
        }
    }
    for (int i = 0; i < out.size(); ++i) {
        const QString title = out.at(i).yAxis.title.isEmpty() ? QString() : out.at(i).yAxis.title;
        Q_UNUSED(title)
        out[i].name = tr("Plot %1 of page %2").arg(i + 1).arg(page + 1);
    }
    return out;
}

Plot plotInRegion(Source& source, int page, const QRectF& region)
{
    QList<Plot> found = findPlots(source, page, region);
    // The one the region holds the most of.
    int best = -1;
    double most = 0;
    for (int i = 0; i < found.size(); ++i) {
        const QRectF both = found.at(i).pageBox().intersected(region);
        const double area = both.width() * both.height();
        if (area > most) {
            most = area;
            best = i;
        }
    }
    if (best >= 0) {
        Plot p = found.at(best);
        p.region = region;
        return p;
    }
    Plot p;
    p.page = page;
    p.region = region;
    p.route = source.graphics(page).paths.isEmpty() ? QStringLiteral("raster") : QStringLiteral("vector");
    p.name = tr("A region of page %1").arg(page + 1);
    return p;
}

QRectF Plot::pageBox() const
{
    if (frame.isNull()) return region;
    return work.isIdentity() ? frame : work.inverted().mapRect(frame);
}

// --- Trust -----------------------------------------------------------------

Trust assess(const Plot& plot)
{
    Trust t;
    const Calibration& c = plot.calibration;
    if (!c.valid()) {
        t.level = QStringLiteral("none");
        t.reasons << tr("It is not calibrated: the points are in page points only.");
        return t;
    }
    t.calibrationPoints = c.rms();
    // What the route is good to, from the tests' measurements (see the
    // write-up): the file's own vertices, or a trace at renderScale.
    bool anyRaster = false, anyHand = false;
    for (const Series& s : plot.series) {
        anyRaster = anyRaster || s.origin == Series::Origin::Raster;
        anyHand = anyHand || s.origin == Series::Origin::Hand;
    }
    if (anyRaster) t.tracePoints = plot.renderScale > 0 ? 1.5 / plot.renderScale : 0.25;
    else if (anyHand) t.tracePoints = 0.5;
    else t.tracePoints = 0.02;
    t.totalPoints = std::hypot(t.calibrationPoints, t.tracePoints);

    int level = 2;   // 2 high, 1 medium, 0 low
    const auto lower = [&](int to, const QString& why) {
        level = std::min(level, to);
        t.reasons << why;
    };
    if (c.rms() > 1.0) lower(0, tr("The references miss by %1 points on average.").arg(c.rms(), 0, 'f', 2));
    else if (c.rms() > 0.35) lower(1, tr("The references miss by %1 points on average.").arg(c.rms(), 0, 'f', 2));
    const auto axisCheck = [&](const Axis& axis, const QString& name) {
        if (c.kind == Kind::Polar) return;
        if (!axis.found) return;   // (calibrated by references given: not this axis's)
        int used = 0, placed = 0;
        for (const Tick& tick : axis.ticks) {
            used += tick.used;
            placed += tick.used && tick.onMark;
        }
        if (used < 3) lower(1, tr("The %1 axis rests on %2 labels alone.").arg(name).arg(used));
        if (axis.scaleGuessed) lower(0, tr("Whether the %1 axis is linear or log is a guess (%2): check it, or set it.").arg(name, axis.scale == Scale::Log ? QStringLiteral("log") : QStringLiteral("linear")));
        if (placed * 2 < used) lower(1, tr("The %1 axis's ticks are placed by their labels, no marks being found under them (labels are good to about a point).").arg(name));
        if (axis.ticks.size() > used && used > 0) t.reasons << tr("%1 of the %2 axis's labels did not fit the others and were left out.").arg(axis.ticks.size() - used).arg(name);
    };
    axisCheck(plot.xAxis, QStringLiteral("x"));
    axisCheck(plot.yAxis, QStringLiteral("y"));
    if (plot.angle != 0) t.reasons << tr("The scan is turned by %1°, set straight first.").arg(plot.angle, 0, 'f', 2);
    if (plot.series.isEmpty()) lower(0, tr("Nothing was extracted."));
    if (plot.seriesLeftOut > 0)
        lower(1, tr("%1 more series were found and left out: only the %2 with the most points are here. 'colours', 'exclude' or a smaller region narrows it.").arg(plot.seriesLeftOut).arg(plot.series.size()));
    // Precision is not all: many series, most of them scraps, are mostly
    // junk - an inset's outlines, a legend, labels drawn as pictures.
    {
        QList<double> lengths;
        for (const Series& s : plot.series) {
            if (s.shape != Series::Shape::Line) continue;
            double length = 0;
            for (int k = 0; k + 1 < s.points.size(); ++k)
                if (!s.breaks.contains(k + 1)) length += QLineF(s.points.at(k), s.points.at(k + 1)).length();
            lengths << length;
        }
        const double longest = lengths.isEmpty() ? 0 : *std::max_element(lengths.begin(), lengths.end());
        int scraps = 0;
        for (const double l : std::as_const(lengths)) scraps += l < 0.1 * longest;
        if (lengths.size() >= 4 && scraps * 2 >= lengths.size())
            lower(0, tr("%1 of the %2 curves are scraps beside the longest: much of this may not be data. Look at the picture; 'exclude', 'colours' or 'seeds' narrow it.").arg(scraps).arg(lengths.size()));
        else {
            // (Curves the plot's own arrows name are known for what they are.)
            int unknown = 0;
            for (const Series& s : plot.series) {
                bool named = false;
                for (const Annotation& n : plot.annotations) named = named || n.names.contains(s.name);
                unknown += !named;
            }
            if (unknown > 10) lower(1, tr("%1 series were found: more than most plots have. Look at the picture.").arg(plot.series.size()));
        }
    }
    // A line of the colour of a set of markers that runs along them may be
    // what is left of the markers, not a curve of its own (a picture's rows of
    // rings touching): said, and counted.
    {
        int lines = 0, along = 0;
        for (const Series& s : plot.series) {
            if (s.shape != Series::Shape::Line || s.origin != Series::Origin::Raster) continue;
            ++lines;
            for (const Series& m : plot.series) {
                if (m.shape != Series::Shape::Markers || m.points.isEmpty() || colourDistance(m.colour, s.colour) >= 60) continue;
                int near = 0;
                for (const QPointF& q : s.points)
                    for (const QPointF& c : m.points)
                        if (std::abs(c.x() - q.x()) <= 5 && QLineF(c, q).length() <= 5) {
                            ++near;
                            break;
                        }
                if (near >= 0.5 * s.points.size()) {
                    ++along;
                    break;
                }
            }
        }
        if (along > 0 && along * 2 >= lines)
            lower(0, tr("%1 of the %2 lines run along markers of their own colour: they may be what is left of the markers, not curves. Look at the picture; 'colours' without the markers' colour leaves them out.").arg(along).arg(lines));
        else if (along > 0)
            lower(1, tr("%1 of the %2 lines run along markers of their own colour: they may be what is left of the markers, not curves. Look at the picture.").arg(along).arg(lines));
    }
    int bridged = 0;
    double worstAgreement = 1;
    for (const Series& s : plot.series) {
        bridged += s.bridged;
        if (s.agreement >= 0) worstAgreement = std::min(worstAgreement, s.agreement);
    }
    if (worstAgreement < 0.85) lower(0, tr("Only %1% of a series' points have ink under them in the picture of the page: look at the overlay.").arg(int(worstAgreement * 100)));
    else if (worstAgreement < 0.97) lower(1, tr("%1% of a series' points have ink under them in the picture of the page.").arg(int(worstAgreement * 100)));
    if (anyRaster) {
        lower(1, tr("Traced from a picture (%1 pixels a point), not read from the file's vertices.").arg(plot.renderScale, 0, 'f', 1));
        if (bridged > 0) t.reasons << tr("%1 gaps (dashes, crossings, what lies over the line) were crossed by following the line: no points are made up inside them.").arg(bridged);
    }
    t.level = level == 2 ? QStringLiteral("high") : (level == 1 ? QStringLiteral("medium") : QStringLiteral("low"));
    const QPointF middle = plot.pageBox().center();
    t.unitsPerPoint = c.unitsPerPoint(middle);
    t.uncertainty = QPointF(t.unitsPerPoint.x() * t.totalPoints, t.unitsPerPoint.y() * t.totalPoints);
    return t;
}

// --- Results ---------------------------------------------------------------

QList<QPointF> values(const Plot& plot, const Series& series)
{
    QList<QPointF> out;
    out.reserve(series.points.size());
    for (const QPointF& p : series.points) out << plot.calibration.toValue(p);
    return out;
}

QList<int> thinned(const QList<QPointF>& points, int most)
{
    QList<int> out;
    const int n = int(points.size());
    if (most <= 0 || n <= most) {
        out.reserve(n);
        for (int i = 0; i < n; ++i) out << i;
        return out;
    }
    // Douglas & Peucker's, by splitting the segment that strays the most
    // until there are \a most: the shape first, whatever the count asked.
    struct Piece {
        int from, to, worst;
        double distance;
    };
    const auto measure = [&](int from, int to) {
        Piece p{from, to, -1, -1};
        const QPointF a = points.at(from), b = points.at(to);
        const QPointF d = b - a;
        const double length = std::hypot(d.x(), d.y());
        for (int i = from + 1; i < to; ++i) {
            const QPointF e = points.at(i) - a;
            const double dist = length > 1e-9 ? std::abs(e.x() * d.y() - e.y() * d.x()) / length : std::hypot(e.x(), e.y());
            if (dist > p.distance) {
                p.distance = dist;
                p.worst = i;
            }
        }
        return p;
    };
    QList<Piece> heap{measure(0, n - 1)};
    const auto order = [](const Piece& a, const Piece& b) { return a.distance < b.distance; };
    QList<int> kept{0, n - 1};
    while (kept.size() < most && !heap.isEmpty()) {
        std::pop_heap(heap.begin(), heap.end(), order);
        const Piece p = heap.takeLast();
        if (p.worst < 0) continue;
        kept << p.worst;
        heap << measure(p.from, p.worst);
        std::push_heap(heap.begin(), heap.end(), order);
        heap << measure(p.worst, p.to);
        std::push_heap(heap.begin(), heap.end(), order);
    }
    std::sort(kept.begin(), kept.end());
    return kept;
}

QString variableName(const QString& name)
{
    QString out;
    for (const QChar c : name) out += (c.isLetterOrNumber() && c.unicode() < 128) ? c : QLatin1Char('_');
    while (out.contains(QLatin1String("__"))) out.replace(QLatin1String("__"), QLatin1String("_"));
    while (out.startsWith(QLatin1Char('_'))) out.remove(0, 1);
    while (out.endsWith(QLatin1Char('_'))) out.chop(1);
    if (out.isEmpty() || out.at(0).isDigit()) out.prepend(QStringLiteral("s_"));
    return out.left(48);
}

namespace {
QString number(double v) { return std::isfinite(v) ? QString::number(v, 'g', 12) : QString(); }
}

namespace {
// A series' points to write, finite ones only, each with the piece it is in
// (1 on): a series with gaps is in pieces, and what plots it ends a stroke
// at each.
struct Written {
    QList<QPointF> points;
    QList<int> piece;
};
Written writtenOf(const Plot& plot, const Series& s)
{
    const QList<QPointF> v = plot.calibration.valid() ? values(plot, s) : s.points;
    Written out;
    int piece = 1;
    bool started = false;
    for (int k = 0; k < v.size(); ++k) {
        if (k > 0 && s.breaks.contains(k) && started) {
            ++piece;
            started = false;
        }
        const QPointF p = v.at(k);
        if (!std::isfinite(p.x()) || !std::isfinite(p.y())) continue;
        out.points << p;
        out.piece << piece;
        started = true;
    }
    return out;
}
} // namespace

QString csv(const Plot& plot)
{
    const auto quoted = [](QString s) {
        if (s.contains(QLatin1Char(',')) || s.contains(QLatin1Char('"')) || s.contains(QLatin1Char('\n'))) {
            s.replace(QLatin1Char('"'), QLatin1String("\"\""));
            s = QLatin1Char('"') + s + QLatin1Char('"');
        }
        // (A spreadsheet takes a leading =, +, - or @ as a formula.)
        if (!s.isEmpty() && QStringLiteral("=+-@").contains(s.at(0))) s.prepend(QLatin1Char('\''));
        return s;
    };
    // (The piece of the series each point is in: a gap in it, where nothing
    // of the curve could be seen, is between two pieces.)
    QString out = plot.calibration.valid() ? QStringLiteral("series,x,y,piece\n") : QStringLiteral("series,page_x_points,page_y_points,piece\n");
    for (const Series& s : plot.series) {
        const Written w = writtenOf(plot, s);
        for (int k = 0; k < w.points.size(); ++k)
            out += quoted(s.name) + QLatin1Char(',') + number(w.points.at(k).x()) + QLatin1Char(',') + number(w.points.at(k).y()) + QLatin1Char(',') + QString::number(w.piece.at(k)) + QLatin1Char('\n');
    }
    return out;
}

QByteArray dataset(const Plot& plot, QStringList* variables)
{
    QByteArray out = "<Qucs Dataset 26.1.1>\n";
    QSet<QString> seen;
    const auto unique = [&](const QString& wanted) {
        QString name = wanted;
        for (int n = 2; seen.contains(name) || seen.contains(name + QLatin1String("_x")); ++n) name = wanted + QLatin1Char('_') + QString::number(n);
        seen.insert(name);
        seen.insert(name + QLatin1String("_x"));
        return name;
    };
    const auto write = [&](const QString& name, const QList<QPointF>& points) {
        // (A gap's NaN as "nan": what the dataset's reader, strtod, takes.)
        const auto value = [](double v) { return std::isfinite(v) ? number(v).toUtf8() : QByteArray("nan"); };
        const QByteArray y = name.toUtf8(), x = y + "_x";
        out += "<indep " + x + ' ' + QByteArray::number(points.size()) + ">\n";
        for (const QPointF& p : points) out += "  " + value(p.x()) + '\n';
        out += "</indep>\n<dep " + y + ' ' + x + ">\n";
        for (const QPointF& p : points) out += "  " + value(p.y()) + '\n';
        out += "</dep>\n";
        if (variables != nullptr) *variables << name;
    };
    // Each series' own name first, all of them, its pieces' after: a
    // series' name does not hang on another having gaps.
    QList<Written> written;
    QStringList names;
    for (const Series& s : plot.series) {
        written << writtenOf(plot, s);
        names << (written.last().points.isEmpty() ? QString() : unique(variableName(s.name)));
    }
    for (int si = 0; si < plot.series.size(); ++si) {
        const Written& w = written.at(si);
        if (w.points.isEmpty()) continue;
        const QString name = names.at(si);
        // Every series as NAME over NAME_x, all of it, whether it has gaps
        // or not - a diagram tracing it keeps working when the plot is done
        // again (the PDF open items' A1). A gap is a NaN between its pieces,
        // halfway across it, where a diagram ends the line.
        QList<QPointF> all;
        for (int k = 0; k < w.points.size(); ++k) {
            if (k > 0 && w.piece.at(k) != w.piece.at(k - 1))
                all << QPointF((w.points.at(k - 1).x() + w.points.at(k).x()) / 2, std::numeric_limits<double>::quiet_NaN());
            all << w.points.at(k);
        }
        write(name, all);
        // With gaps, each piece too, NAME_1, NAME_2... (as before).
        const int pieces = w.piece.last();
        if (pieces == 1) continue;
        for (int piece = 1, k = 0; piece <= pieces; ++piece) {
            QList<QPointF> part;
            for (; k < w.points.size() && w.piece.at(k) == piece; ++k) part << w.points.at(k);
            if (!part.isEmpty()) write(unique(name + QLatin1Char('_') + QString::number(piece)), part);
        }
    }
    return out;
}

// --- The overlay -----------------------------------------------------------

void paintMarks(QPainter& painter, const Plot& plot, double ppp, int selectedSeries, int selectedPoint)
{
    painter.save();
    const double px = 1.0 / std::max(0.05, ppp);   // a device pixel, in points
    // The frame.
    QPolygonF frame;
    if (!plot.frame.isNull()) {
        const QTransform back = plot.work.inverted();
        frame = back.map(QPolygonF(plot.frame));
    } else {
        frame = QPolygonF(plot.region);
    }
    frame << frame.first();
    QPen framePen(QColor(0, 150, 255, 160), 1.5 * px, Qt::DashLine);
    painter.setPen(framePen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPolyline(frame);
    // Regions left out: hatched grey, so that what was not looked at shows.
    {
        QList<QRectF> regions = plot.exclude;
        regions << plot.leftOut;
        painter.setPen(QPen(QColor(120, 120, 120, 200), 1.0 * px, Qt::DotLine));
        QBrush hatch(QColor(120, 120, 120, 110), Qt::BDiagPattern);
        hatch.setTransform(QTransform::fromScale(px, px));
        painter.setBrush(hatch);
        for (const QRectF& r : std::as_const(regions)) painter.drawRect(r);
        painter.setBrush(Qt::NoBrush);
    }
    // The references: a cross each.
    const Calibration& c = plot.calibration;
    QPen refPen(QColor(230, 0, 180), 1.5 * px);
    painter.setPen(refPen);
    for (const Reference& r : c.references) {
        const double s = 5 * px;
        painter.drawLine(r.at + QPointF(-s, -s), r.at + QPointF(s, s));
        painter.drawLine(r.at + QPointF(-s, s), r.at + QPointF(s, -s));
    }
    // Each series: its line, thin, and its points, so that one off its curve shows.
    for (int i = 0; i < plot.series.size(); ++i) {
        const Series& s = plot.series.at(i);
        QColor dark = s.colour;
        if (dark.lightness() > 170) dark = dark.darker(160);
        const QColor mark = QColor::fromHsv((dark.hsvHue() + 180) % 360 < 0 ? 300 : (dark.hsvHue() + 180) % 360, 255, 220);
        const bool selected = i == selectedSeries;
        if (s.shape == Series::Shape::Line || s.shape == Series::Shape::ByHand || s.shape == Series::Shape::Area) {
            QPen pen(mark, (selected ? 1.6 : 0.9) * px);
            painter.setPen(pen);
            QPolygonF line;
            for (int k = 0; k < s.points.size(); ++k) {
                if (s.breaks.contains(k) && !line.isEmpty()) {
                    painter.drawPolyline(line);
                    line.clear();
                }
                line << s.points.at(k);
            }
            if (s.shape == Series::Shape::Area && !line.isEmpty()) line << line.first();
            if (!line.isEmpty()) painter.drawPolyline(line);
        }
        const bool sparse = s.points.size() <= 400 || selected;
        if (sparse) {
            const double radius = (s.shape == Series::Shape::Markers || s.shape == Series::Shape::ByHand || s.shape == Series::Shape::Bars ? 3.0 : 1.8) * px;
            painter.setPen(QPen(Qt::white, 0.6 * px));
            painter.setBrush(mark);
            for (const QPointF& p : s.points) painter.drawEllipse(p, radius, radius);
        }
        if (selected && selectedPoint >= 0 && selectedPoint < s.points.size()) {
            painter.setPen(QPen(Qt::black, 1.5 * px));
            painter.setBrush(Qt::yellow);
            painter.drawEllipse(s.points.at(selectedPoint), 5 * px, 5 * px);
        }
    }
    painter.restore();
}

QImage overlay(Source& source, const Plot& plot, double scale)
{
    QRectF region = plot.region.isNull() ? plot.pageBox() : plot.region;
    if (region.isEmpty()) return {};
    QImage image = source.render(plot.page, region, scale);
    if (image.isNull()) return image;
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(scale, scale);
    painter.translate(-std::floor(region.left() * scale) / scale, -std::floor(region.top() * scale) / scale);
    paintMarks(painter, plot, scale);
    // Each series' name beside its first point.
    QFont font = painter.font();
    font.setPixelSize(std::max(1, int(std::round(11 / scale))));
    font.setPointSizeF(10 / scale);
    painter.setFont(font);
    for (const Series& s : plot.series) {
        if (s.points.isEmpty()) continue;
        QColor dark = s.colour;
        if (dark.lightness() > 170) dark = dark.darker(160);
        const QPointF at = s.points.at(s.points.size() / 2);
        painter.setPen(Qt::white);
        painter.drawText(at + QPointF(4.5 / scale + 0.5 / scale, -4 / scale + 0.5 / scale), s.name);
        painter.setPen(dark);
        painter.drawText(at + QPointF(4.5 / scale, -4 / scale), s.name);
    }
    painter.end();
    return image;
}

// --- Kept with the PDF -----------------------------------------------------

namespace {

constexpr int kMostSeriesKept = 256;
constexpr int kMostPointsKept = 2000000;
constexpr int kMostReferencesKept = 4096;
constexpr int kMostPlotsKept = 4096;
constexpr double kMostCoordinate = 1e7;

QJsonArray pointArray(QPointF p) { return QJsonArray{p.x(), p.y()}; }

bool sensibleCoordinate(double v) { return std::isfinite(v) && std::abs(v) <= kMostCoordinate; }

bool readPoint(const QJsonValue& v, QPointF* out)
{
    const QJsonArray a = v.toArray();
    if (a.size() < 2 || !a.at(0).isDouble() || !a.at(1).isDouble()) return false;
    const double x = a.at(0).toDouble(), y = a.at(1).toDouble();
    if (!sensibleCoordinate(x) || !sensibleCoordinate(y)) return false;
    *out = QPointF(x, y);
    return true;
}

QString shapeName(Series::Shape s)
{
    switch (s) {
    case Series::Shape::Markers: return QStringLiteral("markers");
    case Series::Shape::Area: return QStringLiteral("area");
    case Series::Shape::Bars: return QStringLiteral("bars");
    case Series::Shape::ByHand: return QStringLiteral("by hand");
    default: return QStringLiteral("line");
    }
}

Series::Shape shapeOf(const QString& s)
{
    if (s == QLatin1String("markers")) return Series::Shape::Markers;
    if (s == QLatin1String("area")) return Series::Shape::Area;
    if (s == QLatin1String("bars")) return Series::Shape::Bars;
    if (s == QLatin1String("by hand")) return Series::Shape::ByHand;
    return Series::Shape::Line;
}

QString originName(Series::Origin o)
{
    return o == Series::Origin::Vector ? QStringLiteral("vector") : (o == Series::Origin::Raster ? QStringLiteral("raster") : QStringLiteral("hand"));
}

Series::Origin originOf(const QString& s)
{
    return s == QLatin1String("vector") ? Series::Origin::Vector : (s == QLatin1String("raster") ? Series::Origin::Raster : Series::Origin::Hand);
}

} // namespace

QString shapeText(Series::Shape s) { return shapeName(s); }
QString originText(Series::Origin o) { return originName(o); }

bool inkSaysSomething(const Series& s) { return s.agreement >= 0 && (s.origin != Series::Origin::Vector || s.agreement < 0.98); }

QJsonObject toJson(const Plot& plot)
{
    QJsonObject o{{QStringLiteral("name"), plot.name},
                  {QStringLiteral("page"), plot.page + 1},
                  {QStringLiteral("route"), plot.route},
                  {QStringLiteral("region"), QJsonArray{plot.region.left(), plot.region.top(), plot.region.right(), plot.region.bottom()}}};
    if (!plot.frame.isNull()) o.insert(QStringLiteral("frame"), QJsonArray{plot.frame.left(), plot.frame.top(), plot.frame.right(), plot.frame.bottom()});
    if (!plot.work.isIdentity())
        o.insert(QStringLiteral("work"), QJsonArray{plot.work.m11(), plot.work.m12(), plot.work.m21(), plot.work.m22(), plot.work.dx(), plot.work.dy()});
    if (plot.angle != 0) o.insert(QStringLiteral("angle"), plot.angle);
    const auto rects = [](const QList<QRectF>& list) {
        QJsonArray a;
        for (const QRectF& r : list) a.append(QJsonArray{r.left(), r.top(), r.right(), r.bottom()});
        return a;
    };
    if (!plot.exclude.isEmpty()) o.insert(QStringLiteral("exclude"), rects(plot.exclude));
    if (!plot.leftOut.isEmpty()) o.insert(QStringLiteral("left_out"), rects(plot.leftOut));
    if (plot.renderScale > 0) o.insert(QStringLiteral("render_scale"), plot.renderScale);
    const Calibration& c = plot.calibration;
    QJsonArray refs;
    for (const Reference& r : c.references) {
        QJsonObject item{{QStringLiteral("at"), pointArray(r.at)}};
        if (r.hasX) item.insert(QStringLiteral("x"), r.x);
        if (r.hasY) item.insert(QStringLiteral("y"), r.y);
        refs.append(item);
    }
    QJsonObject cal{{QStringLiteral("kind"), c.kind == Kind::Polar ? QStringLiteral("polar") : QStringLiteral("cartesian")},
                    {QStringLiteral("model"), c.model == Model::Projective ? QStringLiteral("projective") : QStringLiteral("affine")},
                    {QStringLiteral("x_scale"), c.xScale == Scale::Log ? QStringLiteral("log") : QStringLiteral("linear")},
                    {QStringLiteral("y_scale"), c.yScale == Scale::Log ? QStringLiteral("log") : QStringLiteral("linear")},
                    {QStringLiteral("references"), refs}};
    if (c.rAtCentre != 0) cal.insert(QStringLiteral("r_at_centre"), c.rAtCentre);
    o.insert(QStringLiteral("calibration"), cal);
    QJsonArray series;
    for (const Series& s : plot.series) {
        QJsonArray pts;
        for (const QPointF& p : s.points) {
            pts.append(p.x());
            pts.append(p.y());
        }
        QJsonObject item{{QStringLiteral("name"), s.name},
                         {QStringLiteral("colour"), s.colour.name()},
                         {QStringLiteral("shape"), shapeName(s.shape)},
                         {QStringLiteral("origin"), originName(s.origin)},
                         {QStringLiteral("points"), pts}};
        if (!s.breaks.isEmpty()) {
            QJsonArray b;
            for (const int k : s.breaks) b.append(k);
            item.insert(QStringLiteral("breaks"), b);
        }
        if (s.dashed) item.insert(QStringLiteral("dashed"), true);
        if (s.width > 0) item.insert(QStringLiteral("width"), s.width);
        if (s.bridged > 0) item.insert(QStringLiteral("bridged"), s.bridged);
        if (inkSaysSomething(s)) item.insert(QStringLiteral("agreement"), s.agreement);
        series.append(item);
    }
    o.insert(QStringLiteral("series"), series);
    if (!plot.annotations.isEmpty()) {
        QJsonArray notes;
        for (const Annotation& a : plot.annotations) {
            QJsonArray pts;
            for (const QPointF& p : a.points) {
                pts.append(p.x());
                pts.append(p.y());
            }
            QJsonObject item{{QStringLiteral("kind"), a.kind == Annotation::Kind::Ellipse ? QStringLiteral("ellipse") : QStringLiteral("arrow")},
                             {QStringLiteral("points"), pts}};
            if (!a.text.isEmpty()) item.insert(QStringLiteral("text"), a.text);
            if (!a.names.isEmpty()) item.insert(QStringLiteral("names"), QJsonArray::fromStringList(a.names));
            notes.append(item);
        }
        o.insert(QStringLiteral("annotations"), notes);
    }
    if (!plot.asked.isEmpty()) o.insert(QStringLiteral("asked"), plot.asked);
    return o;
}

bool fromJson(const QJsonObject& o, Plot* out, QString* why)
{
    const auto fail = [&](const QString& reason) {
        if (why != nullptr) *why = reason;
        return false;
    };
    Plot plot;
    plot.name = o.value(QLatin1String("name")).toString().left(200);
    const double page = o.value(QLatin1String("page")).toDouble(-1);
    if (!(page >= 1 && page <= 1e7)) return fail(tr("A plot's page is not a page."));
    plot.page = int(page) - 1;
    plot.route = o.value(QLatin1String("route")).toString().left(16);
    const auto rectOf = [](const QJsonValue& v, QRectF* r) {
        const QJsonArray a = v.toArray();
        if (a.size() != 4) return false;
        double n[4];
        for (int i = 0; i < 4; ++i) {
            n[i] = a.at(i).toDouble(kNaN);
            if (!sensibleCoordinate(n[i])) return false;
        }
        *r = QRectF(QPointF(n[0], n[1]), QPointF(n[2], n[3])).normalized();
        return true;
    };
    if (!rectOf(o.value(QLatin1String("region")), &plot.region)) return fail(tr("A plot's region is not a rectangle."));
    if (o.contains(QLatin1String("frame")) && !rectOf(o.value(QLatin1String("frame")), &plot.frame)) return fail(tr("A plot's frame is not a rectangle."));
    if (o.contains(QLatin1String("work"))) {
        const QJsonArray w = o.value(QLatin1String("work")).toArray();
        if (w.size() != 6) return fail(tr("A plot's turn is not a transform."));
        double n[6];
        for (int i = 0; i < 6; ++i) {
            n[i] = w.at(i).toDouble(kNaN);
            if (!sensibleCoordinate(n[i])) return fail(tr("A plot's turn is not a transform."));
        }
        plot.work = QTransform(n[0], n[1], n[2], n[3], n[4], n[5]);
        if (!plot.work.isInvertible()) return fail(tr("A plot's turn is not a transform."));
    }
    for (const char* key : {"exclude", "left_out"}) {
        const QJsonArray list = o.value(QLatin1String(key)).toArray();
        if (list.size() > 1000) return fail(tr("A plot has too many regions left out."));
        for (const QJsonValue& v : list) {
            QRectF r;
            if (!rectOf(v, &r)) return fail(tr("A region left out is not a rectangle."));
            (key == std::string_view("exclude") ? plot.exclude : plot.leftOut) << r;
        }
    }
    const double angle = o.value(QLatin1String("angle")).toDouble(0);
    if (std::isfinite(angle) && std::abs(angle) <= 360) plot.angle = angle;
    const double renderScale = o.value(QLatin1String("render_scale")).toDouble(0);
    if (std::isfinite(renderScale) && renderScale > 0 && renderScale <= 1000) plot.renderScale = renderScale;
    const QJsonObject cal = o.value(QLatin1String("calibration")).toObject();
    Calibration& c = plot.calibration;
    c.kind = cal.value(QLatin1String("kind")).toString() == QLatin1String("polar") ? Kind::Polar : Kind::Cartesian;
    c.model = cal.value(QLatin1String("model")).toString() == QLatin1String("projective") ? Model::Projective : Model::Affine;
    c.xScale = cal.value(QLatin1String("x_scale")).toString() == QLatin1String("log") ? Scale::Log : Scale::Linear;
    c.yScale = cal.value(QLatin1String("y_scale")).toString() == QLatin1String("log") ? Scale::Log : Scale::Linear;
    const double r0 = cal.value(QLatin1String("r_at_centre")).toDouble(0);
    if (std::isfinite(r0)) c.rAtCentre = r0;
    const QJsonArray refs = cal.value(QLatin1String("references")).toArray();
    if (refs.size() > kMostReferencesKept) return fail(tr("A plot has too many references."));
    for (const QJsonValue& v : refs) {
        const QJsonObject item = v.toObject();
        Reference r;
        if (!readPoint(item.value(QLatin1String("at")), &r.at)) return fail(tr("A reference's place is not a point."));
        const QJsonValue x = item.value(QLatin1String("x")), y = item.value(QLatin1String("y"));
        if (x.isDouble() && std::isfinite(x.toDouble())) {
            r.hasX = true;
            r.x = x.toDouble();
        }
        if (y.isDouble() && std::isfinite(y.toDouble())) {
            r.hasY = true;
            r.y = y.toDouble();
        }
        c.references << r;
    }
    if (!c.references.isEmpty()) {
        QString fitWhy;
        if (!c.fit(&fitWhy)) plot.notes << tr("Its calibration no longer fits: %1").arg(fitWhy);
    }
    const QJsonArray series = o.value(QLatin1String("series")).toArray();
    if (series.size() > kMostSeriesKept) return fail(tr("A plot has too many series."));
    qint64 total = 0;
    for (const QJsonValue& v : series) {
        const QJsonObject item = v.toObject();
        Series s;
        s.name = item.value(QLatin1String("name")).toString().left(200);
        const QColor colour(item.value(QLatin1String("colour")).toString());
        s.colour = colour.isValid() ? colour : QColor(Qt::black);
        s.shape = shapeOf(item.value(QLatin1String("shape")).toString());
        s.origin = originOf(item.value(QLatin1String("origin")).toString());
        const QJsonArray pts = item.value(QLatin1String("points")).toArray();
        total += pts.size() / 2;
        if (total > kMostPointsKept) return fail(tr("A plot has too many points."));
        if (pts.size() % 2 != 0) return fail(tr("A series' points do not come in pairs."));
        s.points.reserve(pts.size() / 2);
        for (qsizetype i = 0; i + 1 < pts.size(); i += 2) {
            const double x = pts.at(i).toDouble(kNaN), y = pts.at(i + 1).toDouble(kNaN);
            if (!sensibleCoordinate(x) || !sensibleCoordinate(y)) return fail(tr("A series' point is not a number."));
            s.points << QPointF(x, y);
        }
        for (const QJsonValue& b : item.value(QLatin1String("breaks")).toArray()) {
            const int k = b.toInt(-1);
            if (k > 0 && k < s.points.size()) s.breaks << k;
        }
        s.dashed = item.value(QLatin1String("dashed")).toBool();
        const double width = item.value(QLatin1String("width")).toDouble(0);
        if (std::isfinite(width) && width >= 0 && width < 1000) s.width = width;
        const int bridged = item.value(QLatin1String("bridged")).toInt(0);
        if (bridged >= 0) s.bridged = bridged;
        const double agreement = item.value(QLatin1String("agreement")).toDouble(-1);
        if (std::isfinite(agreement) && agreement >= 0 && agreement <= 1) s.agreement = agreement;
        plot.series << s;
    }
    const QJsonArray annotations = o.value(QLatin1String("annotations")).toArray();
    if (annotations.size() > kMostSeriesKept) return fail(tr("A plot has too many annotations."));
    for (const QJsonValue& v : annotations) {
        const QJsonObject item = v.toObject();
        Annotation a;
        a.kind = item.value(QLatin1String("kind")).toString() == QLatin1String("ellipse") ? Annotation::Kind::Ellipse : Annotation::Kind::Arrow;
        const QJsonArray pts = item.value(QLatin1String("points")).toArray();
        if (pts.size() % 2 != 0 || pts.size() > 2 * 4096) return fail(tr("An annotation's points do not come in pairs."));
        for (qsizetype i = 0; i + 1 < pts.size(); i += 2) {
            const double x = pts.at(i).toDouble(kNaN), y = pts.at(i + 1).toDouble(kNaN);
            if (!sensibleCoordinate(x) || !sensibleCoordinate(y)) return fail(tr("An annotation's point is not a number."));
            a.points << QPointF(x, y);
        }
        a.text = item.value(QLatin1String("text")).toString().left(400);
        for (const QJsonValue& n : item.value(QLatin1String("names")).toArray())
            if (a.names.size() < 64) a.names << n.toString().left(200);
        plot.annotations << a;
    }
    // (What the call asked for: kept as it came, for reading - small.)
    const QJsonObject asked = o.value(QLatin1String("asked")).toObject();
    if (QJsonDocument(asked).toJson(QJsonDocument::Compact).size() <= 64 * 1024) plot.asked = asked;
    *out = std::move(plot);
    return true;
}

QString sidecarPath(const QString& pdfFile, bool forWriting)
{
    const QFileInfo info(pdfFile);
    const QString beside = info.absoluteFilePath() + QStringLiteral(".digitized.json");
    const QString key = QString::fromLatin1(QCryptographicHash::hash(info.absoluteFilePath().toUtf8(), QCryptographicHash::Sha1).toHex().left(24));
    const QString elsewhere = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/digitized/") + key + QStringLiteral(".json");
    if (!forWriting) return QFileInfo::exists(beside) || !QFileInfo::exists(elsewhere) ? beside : elsewhere;
    // A folder of papers may be read-only, or a mounted volume.
    const QFileInfo besideInfo(beside);
    if (besideInfo.exists() ? besideInfo.isWritable() : QFileInfo(info.absolutePath()).isWritable()) return beside;
    return elsewhere;
}

bool saveSidecar(const QString& pdfFile, const QList<Plot>& plots, QString* why)
{
    const QFileInfo info(pdfFile);
    const QString path = sidecarPath(pdfFile, true);
    bool anyKept = false;
    for (const Plot& p : plots) anyKept = anyKept || !p.transient;
    if (!anyKept) {
        if (QFileInfo::exists(path) && !QFile::remove(path)) {
            if (why != nullptr) *why = tr("%1 could not be removed.").arg(QDir::toNativeSeparators(path));
            return false;
        }
        return true;
    }
    QJsonArray all;
    for (const Plot& p : plots)
        if (!p.transient) all.append(toJson(p));
    const QJsonObject root{{QStringLiteral("format"), QStringLiteral("qucs-s plot digitizer")},
                           {QStringLiteral("version"), 1},
                           {QStringLiteral("pdf"), info.fileName()},
                           {QStringLiteral("pdf_size"), double(info.size())},
                           {QStringLiteral("pdf_modified"), info.lastModified().toSecsSinceEpoch() * 1.0},
                           {QStringLiteral("plots"), all}};
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        if (why != nullptr) *why = tr("%1 could not be written: %2").arg(QDir::toNativeSeparators(path), out.errorString());
        return false;
    }
    out.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    if (!out.commit()) {
        if (why != nullptr) *why = tr("%1 could not be written: %2").arg(QDir::toNativeSeparators(path), out.errorString());
        return false;
    }
    return true;
}

bool parseSidecar(const QByteArray& bytes, QList<Plot>* plots, QString* why)
{
    plots->clear();
    constexpr qsizetype kMostBytes = qsizetype(256) << 20;
    if (bytes.size() > kMostBytes) {
        if (why != nullptr) *why = tr("It is too large to be a digitizer's file.");
        return false;
    }
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &error);
    if (!doc.isObject()) {
        if (why != nullptr) *why = tr("It is not JSON: %1").arg(error.errorString());
        return false;
    }
    const QJsonObject root = doc.object();
    if (root.value(QLatin1String("format")).toString() != QLatin1String("qucs-s plot digitizer")) {
        if (why != nullptr) *why = tr("It is not a digitizer's file.");
        return false;
    }
    if (root.value(QLatin1String("version")).toInt() > 1) {
        if (why != nullptr) *why = tr("It was written by a newer version.");
        return false;
    }
    const QJsonArray all = root.value(QLatin1String("plots")).toArray();
    if (all.size() > kMostPlotsKept) {
        if (why != nullptr) *why = tr("It holds too many plots.");
        return false;
    }
    for (const QJsonValue& v : all) {
        Plot p;
        QString reason;
        if (!fromJson(v.toObject(), &p, &reason)) {
            plots->clear();
            if (why != nullptr) *why = reason;
            return false;
        }
        plots->append(std::move(p));
    }
    return true;
}

bool loadSidecar(const QString& pdfFile, QList<Plot>* plots, QString* why, bool* stale)
{
    plots->clear();
    if (stale != nullptr) *stale = false;
    const QString path = sidecarPath(pdfFile);
    QFile in(path);
    if (!in.exists()) return true;
    if (!in.open(QIODevice::ReadOnly)) {
        if (why != nullptr) *why = tr("%1 could not be read: %2").arg(QDir::toNativeSeparators(path), in.errorString());
        return false;
    }
    const QByteArray bytes = in.read((qint64(256) << 20) + 1);
    if (!parseSidecar(bytes, plots, why)) return false;
    const QJsonObject root = QJsonDocument::fromJson(bytes).object();
    const QFileInfo info(pdfFile);
    if (stale != nullptr)
        *stale = qint64(root.value(QLatin1String("pdf_size")).toDouble(-1)) != info.size() ||
                 qint64(root.value(QLatin1String("pdf_modified")).toDouble(-1)) != info.lastModified().toSecsSinceEpoch();
    return true;
}

} // namespace qucs_s::digitizer
