/*
 * numberformat.cpp - numbers on a diagram's axes, in its markers and in
 * the cursor readout
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "numberformat.h"
#include "misc.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>

namespace qucs_s::numberformat {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("NumberFormat", text);
}

// A number the axis code added up (0.1 + 0.2) as it was meant: 12
// significant digits.
double tidy(double v)
{
    return v == 0.0 ? v : QString::number(v, 'g', 12).toDouble();
}

QString withoutTrailingZeros(QString s)
{
    if (s.contains(QLatin1Char('.'))) {
        while (s.endsWith(QLatin1Char('0')))
            s.chop(1);
        if (s.endsWith(QLatin1Char('.')))
            s.chop(1);
    }
    return s;
}

// Without an exponent: \a decimals places, or the fewest that show it to
// 12 significant digits.
QString fixed(double v, int decimals)
{
    if (decimals >= 0)
        return QString::number(v, 'f', decimals);
    const int magnitude = int(std::floor(std::log10(std::fabs(v))));
    return withoutTrailingZeros(QString::number(v, 'f', std::clamp(11 - magnitude, 0, 40)));
}

// The places a step between labels needs: 0.25 two, 0.1 one, 5 none;
// -1 without a step.
int placesOf(double step)
{
    step = std::fabs(step);
    if (!(step > 0.0) || !std::isfinite(step))
        return -1;
    for (int d = 0; d <= 15; ++d) {
        const double scaled = step * std::pow(10.0, d);
        if (std::fabs(scaled - std::round(scaled)) < 1e-6 * std::max(1.0, scaled))
            return d;
    }
    return 15;
}

// value = mantissa x 10^exponent, the exponent a multiple of \a every (1
// or 3), 1 <= |mantissa| < 10^every as written with \a decimals places.
void split(double value, int every, int decimals, QString* mantissa, int* exponent)
{
    const double v = tidy(value);
    int e = int(std::floor(std::log10(std::fabs(v)) + 1e-12));
    e = every * int(std::floor(double(e) / every));
    for (int pass = 0; pass < 2; ++pass) {
        const double m = v / std::pow(10.0, e);
        const QString text = decimals >= 0 ? QString::number(m, 'f', decimals) : QString::number(m, 'g', 10);
        // 9.9999 written as 10.00: one step up.
        if (pass == 0 && std::fabs(text.toDouble()) >= std::pow(10.0, every)) {
            e += every;
            continue;
        }
        *mantissa = text;
        *exponent = e;
        return;
    }
}

QString superscript(int n)
{
    static const char16_t digits[] = {0x2070, 0x00B9, 0x00B2, 0x00B3, 0x2074,
                                      0x2075, 0x2076, 0x2077, 0x2078, 0x2079};
    QString out;
    for (const QChar c : QString::number(n))
        out += c == QLatin1Char('-') ? QChar(0x207B) : QChar(digits[c.digitValue()]);
    return out;
}

// SI prefixes as Qucs always wrote them (misc::num2str): a number from
// 0.25 up to 1000 keeps no prefix, femto to tera.
QString si(double value, int decimals)
{
    char prefix = 0;
    double num = value;
    double cal = std::fabs(value);
    if (cal > 1e-20) {
        cal = std::log10(cal) / 3.0;
        if (cal < -0.2)
            cal -= 0.98;
        const int expo = int(cal);
        switch (expo) {
        case -5: prefix = 'f'; break;
        case -4: prefix = 'p'; break;
        case -3: prefix = 'n'; break;
        case -2: prefix = 'u'; break;
        case -1: prefix = 'm'; break;
        case 1:  prefix = 'k'; break;
        case 2:  prefix = 'M'; break;
        case 3:  prefix = 'G'; break;
        case 4:  prefix = 'T'; break;
        default: break;
        }
        if (prefix)
            num /= std::pow(10.0, double(3 * expo));
    }
    QString s = decimals < 0 ? QString::number(num) : QString::number(num, 'f', decimals);
    if (prefix)
        s += QLatin1Char(prefix);
    return s;
}

} // namespace

Notation fromInt(int value)
{
    return value >= int(Notation::Automatic) && value <= int(Notation::Power) ? Notation(value)
                                                                               : Notation::Automatic;
}

QString format(double value, Notation notation, int decimals, double step)
{
    if (!std::isfinite(value))
        return QString::number(value);
    if (std::fabs(value) < 1e-300) {   // 0, and no "-0"
        if (notation == Notation::Decimal && decimals < 0)
            decimals = placesOf(step);
        return decimals > 0 ? QString::number(0.0, 'f', decimals) : QStringLiteral("0");
    }
    QString mantissa;
    int exponent = 0;
    switch (notation) {
    case Notation::Automatic:
        if (decimals < 0)
            return misc::StringNiceNum(value);   // as it always was
        if (std::fabs(std::log10(std::fabs(value))) < 3.0)
            return QString::number(value, 'f', decimals);
        split(value, 1, decimals, &mantissa, &exponent);
        return mantissa + QLatin1Char('e') + QString::number(exponent);
    case Notation::Engineering:
        return si(value, decimals);
    case Notation::Scientific:
        split(value, 1, decimals, &mantissa, &exponent);
        return mantissa + QLatin1Char('e') + QString::number(exponent);
    case Notation::EngineeringExponent:
        split(value, 3, decimals, &mantissa, &exponent);
        return exponent == 0 ? mantissa : mantissa + QLatin1Char('e') + QString::number(exponent);
    case Notation::Decimal:
        return fixed(value, decimals >= 0 ? decimals : placesOf(step));
    case Notation::Power:
        split(value, 1, decimals, &mantissa, &exponent);
        if (exponent == 0)
            return mantissa;
        if (mantissa == QLatin1String("1"))
            return QStringLiteral("10") + superscript(exponent);
        if (mantissa == QLatin1String("-1"))
            return QStringLiteral("-10") + superscript(exponent);
        return mantissa + QChar(0x00D7) + QStringLiteral("10") + superscript(exponent);
    }
    return misc::StringNiceNum(value);
}

QList<QPair<Notation, QString>> choices()
{
    const QList<QPair<Notation, QString>> named{
        {Notation::Automatic, tr("automatic")},
        {Notation::Decimal, tr("decimal")},
        {Notation::Scientific, tr("scientific")},
        {Notation::Power, tr("scientific, power of ten")},
        {Notation::Engineering, tr("engineering, SI prefixes")},
        {Notation::EngineeringExponent, tr("engineering, exponent")},
    };
    QList<QPair<Notation, QString>> out;
    for (const auto& [notation, name] : named)
        out.append({notation, QStringLiteral("%1 (%2, %3, %4)")
                                  .arg(name, format(0.5, notation), format(1500, notation),
                                       format(2.5e-5, notation))});
    return out;
}

} // namespace qucs_s::numberformat
