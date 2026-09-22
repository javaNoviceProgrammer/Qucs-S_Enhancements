/*
 * valuereading.cpp - what a property value says: a number with its prefix
 * and unit, an expression or a name - and what the SPICE netlist gets
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "valuereading.h"

#include "misc.h"
#include "extsimkernels/spicecompat.h"

#include <QCoreApplication>
#include <QRegularExpression>

#include <cmath>

namespace qucs_s::units {

namespace {

QString tr(const char* s) { return QCoreApplication::translate("ValueReading", s); }

// A number as it starts: sign, digits with a point, exponent.
const QRegularExpression& leadingNumber()
{
    static const QRegularExpression re("^\\s*([+-]?(?:[0-9]+\\.?[0-9]*|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?)");
    return re;
}

bool same(double a, double b)
{
    return a == b || std::abs(a - b) <= 1e-9 * std::max(std::abs(a), std::abs(b));
}

} // namespace

double spiceNumber(const QString& text, bool* ok)
{
    const QRegularExpressionMatch m = leadingNumber().match(text);
    if (ok) *ok = m.hasMatch();
    if (!m.hasMatch()) return 0;
    double number = m.captured(1).toDouble();
    const QString rest = text.mid(m.capturedEnd()).trimmed().toUpper();
    if (rest.startsWith(QLatin1String("MEG"))) return number * 1e6;
    if (rest.startsWith(QLatin1String("MIL"))) return number * 25.4e-6;
    if (rest.isEmpty()) return number;
    switch (rest.at(0).toLatin1()) {
    case 'T': return number * 1e12;
    case 'G': return number * 1e9;
    case 'K': return number * 1e3;
    case 'M': return number * 1e-3;
    case 'U': return number * 1e-6;
    case 'N': return number * 1e-9;
    case 'P': return number * 1e-12;
    case 'F': return number * 1e-15;
    default: return number;
    }
}

QString engineering(double value, const QString& unit)
{
    // misc::num2str writes the prefix after the number ("10k"); a space
    // between it and the unit reads better.
    const QString n = misc::num2str(value, -1, QString());
    const bool prefixed = !n.isEmpty() && n.back().isLetter();
    if (unit.isEmpty()) return prefixed ? n.left(n.size() - 1) + QLatin1Char(' ') + n.back() : n;
    return prefixed ? n.left(n.size() - 1) + QLatin1Char(' ') + n.back() + unit : n + QLatin1Char(' ') + unit;
}

Reading read(const QString& input)
{
    Reading r;
    const QString value = input.trimmed();
    if (value.isEmpty()) return r;
    r.spice = spicecompat::normalize_value(value);

    static const QRegularExpression name("^[A-Za-z_][A-Za-z0-9_.]*$");
    static const QRegularExpression operators("[*/^()+\\-,<>=!?:{}']");
    static const QRegularExpression list("\\s[+-]?[0-9.]");
    if (name.match(value).hasMatch()) {
        r.kind = Reading::Name;
        return r;
    }
    const QRegularExpressionMatch number = leadingNumber().match(value);
    if (!number.hasMatch() || operators.match(value.mid(number.capturedEnd())).hasMatch()) {
        r.kind = Reading::Expression;   // "2*R1", "1e3/f0", "{...}", "'...'"
        return r;
    }
    const QString rest = value.mid(number.capturedEnd()).trimmed();
    if (list.match(value.mid(number.capturedEnd())).hasMatch()) {
        r.kind = Reading::List;
        return r;
    }

    // After the number: a scale prefix and a unit, as Qucs writes them;
    // the same in another case is read (and compared with what SPICE
    // makes of it); a digit after the prefix is the European "4k7".
    static const QString units = QStringLiteral("Ohm|F|H|V|A|Hz|S|s|m|dBm|dB|W|K|deg|%");
    static const QRegularExpression exact("^(?:Meg|[TGMkcmunpf])?(?:" + units + ")?$");
    static const QRegularExpression loose("^(?:meg|[tgmkcunpf])?(?:" + units + ")?$",
                                          QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression european("^[TGMkmunpfR][0-9]+$");
    const bool isEuropean = european.match(rest).hasMatch();
    if (!exact.match(rest).hasMatch() && !loose.match(rest).hasMatch() && !isEuropean) {
        r.kind = Reading::Text;
        return r;
    }

    r.kind = Reading::Number;
    double n = 0, factor = 1;
    QString unused;   // str2num gives the whole text as the unit when there is none
    misc::str2num(value, n, unused, factor);
    r.value = n * factor;
    // The unit: what follows the prefix Qucs took (none when it took none).
    r.unit = isEuropean ? QString() : factor != 1.0 && !rest.isEmpty() ? rest.mid(1) : rest;
    if (factor != 1.0 && r.unit.startsWith(QLatin1String("eg"))) r.unit = r.unit.mid(2);   // "Meg"
    bool ok = false;
    r.spiceValue = spiceNumber(r.spice, &ok);

    if (isEuropean) {
        r.warning = tr("the digits after the prefix are not read: \"%1\" is %2; write it with a point (4.7k)")
                        .arg(value, engineering(r.value));
    } else if (ok && !same(r.value, r.spiceValue)) {
        r.warning = tr("SPICE reads the netlist's %1 as %2, Qucs reads %3 - check the prefix and the unit")
                        .arg(r.spice, engineering(r.spiceValue), engineering(r.value));
    }
    return r;
}

QString Reading::describe(bool spiceSimulator) const
{
    QString s;
    switch (kind) {
    case Empty:
        return QString();
    case Number:
        s = engineering(value, unit);
        if (!unit.isEmpty() || std::abs(value) >= 1e3 || (value != 0 && std::abs(value) < 1))
            s += QStringLiteral(" = %1").arg(value, 0, 'g', 12);
        break;
    case Expression:
        s = tr("an expression the simulator works out");
        break;
    case Name:
        s = tr("a name: a parameter or a model defined elsewhere");
        break;
    case List:
        return tr("a list of values");
    case Text:
        return tr("text");
    }
    if (spiceSimulator && !spice.isEmpty()) s += QStringLiteral("  →  ") + tr("netlist: %1").arg(spice);
    if (!warning.isEmpty()) s += QStringLiteral("\n⚠ ") + warning;
    return s;
}

} // namespace qucs_s::units
