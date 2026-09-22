/*
 * valuereading.h - what a property value says: a number with its prefix
 * and unit, an expression or a name - and what the SPICE netlist gets
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_VALUEREADING_H
#define QUCS_VALUEREADING_H

#include <QString>

namespace qucs_s::units {

/// A property value as Qucs and a SPICE simulator read it.
struct Reading {
    enum Kind {
        Empty,        ///< nothing written
        Number,       ///< a number, maybe with a scale prefix and a unit: "10 kOhm", "4.7n", "1e-3"
        Expression,   ///< worked out by the simulator: "2*R1", "{Rload/2}", "'x+1'"
        Name,         ///< a parameter or a model defined elsewhere: "Rload"
        List,         ///< several values: "0 0 1u 5"
        Text          ///< anything else, a model name like "2N2222" among it
    };
    Kind kind = Empty;
    double value = 0;       ///< for a number: what Qucs reads, the prefix applied
    QString unit;           ///< for a number: what follows the prefix ("Ohm", "F", "")
    QString spice;          ///< what the SPICE netlist carries (spicecompat::normalize_value)
    double spiceValue = 0;  ///< for a number: what SPICE reads in spice
    /// Something the writer will not have meant: SPICE reads another
    /// number than Qucs, or a digit follows the prefix ("4k7").
    QString warning;

    /// One line for the component dialog: the value, what the netlist
    /// gets when \a spiceSimulator, and the warning if there is one.
    QString describe(bool spiceSimulator) const;
};

/// Reads a property value.
Reading read(const QString& value);

/// How SPICE reads a number: the number, then a scale factor (T, G, MEG,
/// K, MIL, M = milli, U, N, P, F, any case), anything after it ignored.
/// \a ok is false when it does not start with a number.
double spiceNumber(const QString& text, bool* ok = nullptr);

/// A number written with an engineering prefix and \a unit: "10 k", "4.7 nF".
QString engineering(double value, const QString& unit = QString());

} // namespace qucs_s::units

#endif
