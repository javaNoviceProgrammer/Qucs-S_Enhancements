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

/// The source of a trace's bits: a V(PRBS) of the schematic, its Tbit and
/// its coding - as the run the data is of gave them, when its netlist is
/// kept (misc::keepRunNetlist), else as they are now.
struct Source {
    QString name;   ///< "V1" (several, as near: "V1 and V3")
    double ui = std::numeric_limits<double>::quiet_NaN();   ///< its Tbit, in seconds; NaN: none
    int levels = 2;   ///< 4 when coded PAM4 (two bits a symbol on four levels), else 2
    QString why;    ///< when there is none though there are PRBS sources: why
    QString note;   ///< what changed since the run: "V1's Tbit is 200 ps now, 100 ps in the run ..."
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
/// With \a dataset (the file the variable is read from), the Tbit and
/// coding are those its run's netlist gave the source - the data is folded
/// at the bits it was made of - and the note says when the source has others
/// now; a source not in that run is none (why says so).
Source sourceOf(const Schematic* sch, const QString& variable, const QString& dataset = QString());

/// The dataset file \a variable of a diagram of \a sch is read from:
/// beside it, its Data Set with the simulator's ending ("ngspice/tran.v(rx)":
/// amp.dat.ngspice).
QString datasetOf(const Schematic* sch, const QString& variable);

/// Lets the eye diagrams of the application's documents find their PRBS
/// sources: on a schematic in it, a diagram's own; on a data display, the
/// open schematic of its dataset's.
void installForEyeDiagrams();

} // namespace qucs_s::prbs

#endif // QUCS_PRBSSOURCE_H
