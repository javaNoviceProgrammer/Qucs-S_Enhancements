/*
 * oppoint.cpp - the operating point of every device after a DC bias run
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "oppoint.h"
#include "valuereading.h"

#include <QHash>
#include <QObject>
#include <QRegularExpression>

#include <cmath>

namespace qucs_s::oppoint {

QList<Device> parseShow(const QString& text)
{
    static const QRegularExpression space(QStringLiteral("\\s+"));
    QList<Device> devices;
    QList<int> block;   // the devices of the rows being read
    QString type, description;
    for (const QString& raw : text.split('\n')) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) {
            block.clear();
            continue;
        }
        const QStringList tokens = line.split(space, Qt::SkipEmptyParts);
        const QString& first = tokens.first();
        if (first.size() > 1 && first.endsWith(':')) {   // " BJT: Bipolar Junction Transistor"
            type = first.chopped(1);
            description = line.mid(line.indexOf(':') + 1).trimmed();
            block.clear();
            continue;
        }
        if (first == QLatin1String("device")) {
            block.clear();
            for (int i = 1; i < tokens.size(); ++i) {
                Device d;
                d.name = tokens.at(i).toLower();
                d.type = type;
                d.description = description;
                devices << d;
                block << devices.size() - 1;
            }
            continue;
        }
        // A row belongs to the block when it has a value for each device.
        if (block.isEmpty() || tokens.size() != block.size() + 1) continue;
        if (first == QLatin1String("model")) {
            for (int i = 0; i < block.size(); ++i) devices[block.at(i)].model = tokens.at(i + 1);
            continue;
        }
        for (int i = 0; i < block.size(); ++i) {
            bool ok = false;
            const double value = tokens.at(i + 1).toDouble(&ok);
            if (ok && std::isfinite(value)) devices[block.at(i)].parameters << Parameter{first, value};
        }
    }
    return devices;
}

void attribute(QList<Device>& devices, const QStringList& components)
{
    QHash<QString, QString> byName;   // lower case -> as written
    for (const QString& c : components) byName.insert(c.toLower(), c);
    const auto componentOf = [&byName](const QString& instance) -> QString {
        if (const auto it = byName.constFind(instance); it != byName.cend()) return *it;
        if (instance.size() > 1)
            if (const auto it = byName.constFind(instance.mid(1)); it != byName.cend()) return *it;
        return QString();
    };
    for (Device& d : devices) {
        const QStringList parts = d.name.split('.');
        if (parts.size() >= 3 && parts.first().size() == 1) {   // "m.xsub1.m9"
            d.component = componentOf(parts.at(1));
            d.inside = parts.mid(2).join('.');
        } else {
            d.component = componentOf(d.name);
            d.inside.clear();
        }
    }
}

QString unitOf(const QString& type, const QString& parameter)
{
    const QString t = type.toLower();
    const QString& p = parameter;
    if (p == QLatin1String("dc") || p == QLatin1String("acmag"))
        return t == QLatin1String("vsource") ? QStringLiteral("V")
             : t == QLatin1String("isource") ? QStringLiteral("A") : QString();
    if (p == QLatin1String("ac"))
        return t == QLatin1String("resistor") ? QStringLiteral("Ohm") : QString();
    static const QHash<QString, QString> named = {
        {QStringLiteral("resistance"), QStringLiteral("Ohm")},
        {QStringLiteral("conductance"), QStringLiteral("S")},
        {QStringLiteral("capacitance"), QStringLiteral("F")},
        {QStringLiteral("inductance"), QStringLiteral("H")},
        {QStringLiteral("flux"), QStringLiteral("Wb")},
        {QStringLiteral("p"), QStringLiteral("W")},
        {QStringLiteral("pwr"), QStringLiteral("W")},
        {QStringLiteral("power"), QStringLiteral("W")},
        {QStringLiteral("freq"), QStringLiteral("Hz")},
        {QStringLiteral("z0"), QStringLiteral("Ohm")},
        {QStringLiteral("temp"), QStringLiteral("°C")},
        {QStringLiteral("dtemp"), QStringLiteral("K")},
        {QStringLiteral("dt"), QStringLiteral("K")},
        {QStringLiteral("l"), QStringLiteral("m")},
        {QStringLiteral("w"), QStringLiteral("m")},
        {QStringLiteral("leff"), QStringLiteral("m")},
        {QStringLiteral("weff"), QStringLiteral("m")},
    };
    if (const auto it = named.constFind(p); it != named.cend()) return *it;
    // Flags and multipliers: "rgatemod", "mult_i".
    if (p.endsWith(QLatin1String("mod")) || p.startsWith(QLatin1String("mult"))) return QString();
    if (p.startsWith(QLatin1String("icv"))) return QStringLiteral("V");   // initial conditions
    // Short names by their first letter: "id", "ic", "i_p"; "vgs", "vdsat",
    // "vth"; "gm", "gds", "gpi"; "cgs", "cd", "capbd"; "qg", "qinv"; the
    // series resistances "rs", "rd", "rbdb".
    static const QRegularExpression electrical(QStringLiteral("^([ivgcqr])[a-z0-9_]{0,7}$"));
    const QRegularExpressionMatch m = electrical.match(p);
    if (!m.hasMatch()) return QString();
    switch (m.capturedView(1).at(0).unicode()) {
    case 'i': return QStringLiteral("A");
    case 'v': return QStringLiteral("V");
    case 'g': return QStringLiteral("S");
    case 'c': return QStringLiteral("F");
    case 'q': return QStringLiteral("C");
    case 'r': return p.size() <= 5 ? QStringLiteral("Ohm") : QString();
    }
    return QString();
}

bool isOperatingQuantity(const QString& type, const QString& parameter)
{
    if (parameter.startsWith(QLatin1String("icv"))) return false;   // set, not found
    static const QStringList units = {QStringLiteral("A"), QStringLiteral("V"), QStringLiteral("S"),
                                      QStringLiteral("F"), QStringLiteral("C"), QStringLiteral("W")};
    return units.contains(unitOf(type, parameter));
}

QString valueText(const Device& device, const Parameter& parameter)
{
    const QString unit = unitOf(device.type, parameter.name);
    // "Not set" is a huge number to ngspice (bv_max 1e99); integers stay so.
    if (std::fabs(parameter.value) >= 1e30 || (unit.isEmpty() && parameter.value == std::round(parameter.value)))
        return QString::number(parameter.value, 'g', 6);
    return units::engineering(parameter.value, unit);
}

QString tooltip(const QList<const Device*>& devices, int maxRows)
{
    if (devices.isEmpty()) return QString();
    QString html = QStringLiteral("<table cellspacing=\"0\" cellpadding=\"1\">");
    int rows = 0, left = 0;
    for (const Device* d : devices) {
        QList<const Parameter*> shown;
        // Not the zeros - nor what is zero but for rounding (an "off"
        // transistor's 1e-134 A) - nor ngspice's 1e99 for "not set".
        for (const Parameter& p : d->parameters)
            if (std::fabs(p.value) > 1e-30 && std::fabs(p.value) < 1e30 && isOperatingQuantity(d->type, p.name))
                shown << &p;
        if (rows >= maxRows) {
            left += shown.size();
            continue;
        }
        const QString who = d->inside.isEmpty() ? d->component : d->inside;
        QString head = QStringLiteral("<b>%1</b> &nbsp;%2").arg(who.toHtmlEscaped(), d->type.toHtmlEscaped());
        if (!d->model.isEmpty()) head += QStringLiteral(", ") + d->model.toHtmlEscaped();
        html += QStringLiteral("<tr><td colspan=\"2\">%1</td></tr>").arg(head);
        ++rows;
        for (const Parameter* p : shown) {
            if (rows >= maxRows) {
                ++left;
                continue;
            }
            html += QStringLiteral("<tr><td>%1&nbsp;&nbsp;</td><td align=\"right\">%2</td></tr>")
                        .arg(p->name.toHtmlEscaped(), valueText(*d, *p).toHtmlEscaped());
            ++rows;
        }
    }
    html += QStringLiteral("</table>");
    if (left > 0)
        html += QStringLiteral("<p>%1</p>").arg(
            QObject::tr("... and %n more on the Operating Point tab of the message dock", nullptr, left));
    return html;
}

} // namespace qucs_s::oppoint
