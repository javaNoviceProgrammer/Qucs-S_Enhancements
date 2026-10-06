/*
 * tooljson.h - the --json mode of the synthesis and calculation programs
 *              (qucs-sfilter, qucs-sactivefilter, qucs-sattenuator,
 *              qucs-spowercombining, qucs-strans, rxcalc): a spec as one
 *              JSON object on standard input, the result as one on
 *              standard output - what Qucs-S's tools for Claude run them
 *              with. Header only: each program is built on its own.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_TOOLJSON_H
#define QUCS_TOOLJSON_H

#include <QByteArray>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QString>
#include <QStringList>

#include <cmath>
#include <cstdio>

namespace qucs_s::tooljson {

/// Whether the program was started with --json.
inline bool wanted(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i)
        if (QByteArray(argv[i]) == "--json") return true;
    return false;
}

/// The spec on standard input; an empty object and \a error when it is not
/// one.
inline QJsonObject spec(QString* error)
{
    QFile in;
    if (!in.open(stdin, QIODevice::ReadOnly)) {
        *error = QStringLiteral("standard input cannot be read");
        return {};
    }
    QJsonParseError parse;
    const QJsonDocument d = QJsonDocument::fromJson(in.readAll(), &parse);
    if (!d.isObject()) {
        *error = QStringLiteral("the spec is one JSON object (%1)").arg(parse.errorString());
        return {};
    }
    return d.object();
}

/// \a result on standard output, a line; 0 to return from main.
inline int answer(const QJsonObject& result)
{
    QFile out;
    if (out.open(stdout, QIODevice::WriteOnly)) {
        out.write(QJsonDocument(result).toJson(QJsonDocument::Compact));
        out.write("\n");
    }
    return 0;
}

inline int fail(const QString& error)
{
    return answer(QJsonObject{{QStringLiteral("error"), error}});
}

/// The simulator a schematic is made for, as spicecompat numbers them:
/// ngspice (the default), xyce, spiceopus, qucsator.
inline int simulator(const QJsonObject& spec)
{
    const QString s = spec.value(QStringLiteral("simulator")).toString(QStringLiteral("ngspice")).toLower();
    if (s == QLatin1String("qucsator")) return 0b00001000;
    if (s == QLatin1String("xyce")) return 0b00000010;
    if (s == QLatin1String("spiceopus")) return 0b00000100;
    return 0b00000001;
}

/// A number of the spec: a JSON number, or text with a scale (1 GHz, 2.2k,
/// 100n, 3.3e9) - NaN when it is neither (or not there).
inline double number(const QJsonValue& v)
{
    if (v.isDouble()) return v.toDouble();
    if (!v.isString()) return std::nan("");
    QString t = v.toString().trimmed();
    // The unit after it (Hz, Ohm, m, dB) is left out.
    int end = 0;
    while (end < t.size() && (t.at(end).isDigit() || QStringLiteral(".+-eE").contains(t.at(end)))) {
        // ("e" of a unit after a number: 1 meg is not 1e.)
        if ((t.at(end) == QLatin1Char('e') || t.at(end) == QLatin1Char('E'))
            && !(end + 1 < t.size() && (t.at(end + 1).isDigit() || t.at(end + 1) == QLatin1Char('-') || t.at(end + 1) == QLatin1Char('+'))))
            break;
        ++end;
    }
    bool ok = false;
    double x = t.left(end).toDouble(&ok);
    if (!ok) return std::nan("");
    const QString rest = t.mid(end).trimmed();
    static const QStringList scales{QStringLiteral("T"), QStringLiteral("G"), QStringLiteral("M"), QStringLiteral("k"), QStringLiteral("m"),
                                    QStringLiteral("u"), QStringLiteral("n"), QStringLiteral("p"), QStringLiteral("f")};
    static const double factors[] = {1e12, 1e9, 1e6, 1e3, 1e-3, 1e-6, 1e-9, 1e-12, 1e-15};
    if (rest.startsWith(QLatin1String("meg"), Qt::CaseInsensitive)) return x * 1e6;
    if (rest.startsWith(QLatin1String("mil"))) return x * 25.4e-6;   // (a thousandth of an inch, in metres)
    if (!rest.isEmpty()) {
        const int k = int(scales.indexOf(rest.left(1)));
        // (A unit of one letter alone is no scale: 10 m is metres, 10 mm is milli-metres.)
        const bool unitOnly = rest.size() == 1 && (rest == QLatin1String("m") || rest == QLatin1String("F") || rest == QLatin1String("H"));
        if (k >= 0 && !unitOnly) x *= factors[k];
        else if (rest.startsWith(QStringLiteral("µ"))) x *= 1e-6;
    }
    return x;
}

} // namespace qucs_s::tooljson

#endif // QUCS_TOOLJSON_H
