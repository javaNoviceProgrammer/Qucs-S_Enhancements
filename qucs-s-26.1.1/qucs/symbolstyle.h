/*
 * symbolstyle.h - US or European schematic symbols: the ones a new part is
 *                 drawn with (Application Settings > Settings > Component
 *                 symbols), and the ones the synthesis programs give the
 *                 parts of their designs. Header only: those programs
 *                 (qucs-sactivefilter, qucs-sattenuator,
 *                 qucs-spowercombining) are built on their own.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_SYMBOLSTYLE_H
#define QUCS_SYMBOLSTYLE_H

#include <QByteArray>
#include <QRegularExpression>
#include <QSettings>
#include <QString>

namespace qucs_s::symbols {

/*!
 * The symbols of a part that has both: US (ANSI/IEEE 315) - a resistor's
 * zigzag, a diode's bar and triangle on a broken line, logic gates'
 * distinctive shapes - or European (IEC 60617, DIN 40900) - a resistor's
 * box, a diode on an unbroken line, logic gates as rectangles. Each part
 * keeps its own in its Symbol property; this is what a new one gets.
 */
enum Style { US = 0, European = 1 };

/// The environment variable Qucs-S sets for the programs it starts (the
/// synthesis tools): the style of its settings, "US" or "European".
inline const char* environmentVariable() { return "QUCS_S_COMPONENT_SYMBOLS"; }

/// The style \a name says (US, European; 0, 1), else \a fallback.
inline Style styleNamed(const QString& name, Style fallback)
{
    const QString n = name.trimmed().toLower();
    if (n == QLatin1String("us") || n == QLatin1String("0")) return US;
    if (n == QLatin1String("european") || n == QLatin1String("1")) return European;
    return fallback;
}

inline QString nameOf(Style style) { return style == European ? QStringLiteral("European") : QStringLiteral("US"); }

/// A resistor's Symbol property in \a style ("[european, US]").
inline QString resistorSymbol(Style style) { return style == European ? QStringLiteral("european") : QStringLiteral("US"); }

/// Tells the programs this process starts from now on \a style (in
/// environmentVariable()).
inline void tellTools(Style style) { qputenv(environmentVariable(), nameOf(style).toLatin1()); }

/// The style a synthesis program gives its parts: as the Qucs-S that
/// started it says, else as the settings say (ComponentSymbols), else US.
inline Style forTools()
{
    const QByteArray set = qgetenv(environmentVariable());
    if (!set.isEmpty()) return styleNamed(QString::fromLatin1(set), US);
    const QSettings settings(QStringLiteral("qucs"), QStringLiteral("qucs_s"));
    return styleNamed(settings.value(QStringLiteral("ComponentSymbols")).toString(), US);
}

/// \a schematic - a design's components, as a schematic file or the
/// clipboard holds them - with each resistor drawn in \a style: the Symbol
/// value (european or US) of each <R ...> line, its last property.
inline QString styled(QString schematic, Style style)
{
    static const QRegularExpression resistor(QStringLiteral(R"((^\s*<R [^\n]*")(?:european|US)("\s+\d+>[ \t\r]*)$)"),
                                             QRegularExpression::MultilineOption);
    return schematic.replace(resistor, QStringLiteral("\\1") + resistorSymbol(style) + QStringLiteral("\\2"));
}

} // namespace qucs_s::symbols

#endif // QUCS_SYMBOLSTYLE_H
