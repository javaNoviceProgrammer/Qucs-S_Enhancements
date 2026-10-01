/*
 * prbssource.h - the PRBS source a trace's signal comes from, for the bit
 * period an eye is folded at
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_PRBSSOURCE_H
#define QUCS_PRBSSOURCE_H

#include <QString>

#include <limits>

class Schematic;

namespace qucs_s::prbs {

/// The source of a trace's bits: a V(PRBS) of the schematic and its Tbit.
struct Source {
    QString name;   ///< "V1" (several, as near: "V1, V3")
    double ui = std::numeric_limits<double>::quiet_NaN();   ///< its Tbit, in seconds; NaN: none
    QString why;    ///< when there is none though there are PRBS sources: why
    bool found() const { return ui > 0.0; }
};

/// The node whose voltage \a variable is: "ngspice/tran.v(rx)", "v(rx)",
/// "rx.Vt" (Qucsator's) are "rx"; empty for anything else (a current, a
/// difference, an expression) and for another dataset's variable (a run
/// kept, "run1:v(rx)", or an import: not of the circuit as it is).
QString nodeOf(const QString& variable);

/// The PRBS source \a variable's signal comes from in \a sch: the V(PRBS)
/// nearest its node's net - fewest parts crossed from it, never through
/// ground (nor a part left out of the simulation). Several as near with
/// one Tbit are one source; with different ones, none (why says so), as
/// for a Tbit that is no number (a parameter). None when the variable is
/// no node's voltage or the schematic has no such net.
Source sourceOf(const Schematic* sch, const QString& variable);

/// Lets the eye diagrams of the application's documents find their PRBS
/// sources: on a schematic in it, a diagram's own; on a data display, the
/// open schematic of its dataset's.
void installForEyeDiagrams();

} // namespace qucs_s::prbs

#endif // QUCS_PRBSSOURCE_H
