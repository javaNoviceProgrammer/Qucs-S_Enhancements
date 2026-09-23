/*
 * osdiselection.h - which OSDI libraries (compiled Verilog-A) a netlist
 * needs: the modules its .model cards name - in the netlist and in the
 * files it includes - against the modules each library defines, so that
 * ngspice loads (pre_osdi) those and no others
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_OSDISELECTION_H
#define QUCS_OSDISELECTION_H

#include <QSet>
#include <QString>
#include <QStringList>

namespace qucs_s::osdi {

/// The device types the .model cards of \a spice name, in lower case:
/// "bsimcmg" of ".model n1 BSIMCMG (l=1u ...)" and of ".MODEL n1
/// bsimcmg(l=1u)". Continuation lines ("+") are joined, comments left out.
QSet<QString> modelTypes(const QString& spice);

/// The files \a spice brings in - .include, .inc and .lib FILE [SECTION],
/// quoted or not - absolute, a relative one taken against \a baseDir.
QStringList includedFiles(const QString& spice, const QString& baseDir);

/// modelTypes() of \a spice and of the files it brings in, and of the
/// files those bring in (each read once, a relative path against the
/// folder of the file that names it).
QSet<QString> usedModelTypes(const QString& spice, const QString& baseDir);

/// The modules the OSDI library defines, in lower case, read from the
/// library as vamodule does (and remembered while the file is unchanged).
/// Empty, and \a readable false, when it cannot be loaded here - built for
/// another architecture than Qucs-S, for one.
QStringList modulesOf(const QString& osdiFile, bool* readable = nullptr);

/// Whether the library defines \a module (any case): its modules when it
/// can be loaded, else whether the name is a string in its bytes.
bool defines(const QString& osdiFile, const QString& module);

/// The libraries of \a osdiFiles a netlist that uses \a types needs, in
/// the order given: each type a library defines is taken from one library
/// - one already taken, else the most recently built. What was left out
/// for another library is said in \a notes.
QStringList needed(const QStringList& osdiFiles, const QSet<QString>& types,
                   QStringList* notes = nullptr);

} // namespace qucs_s::osdi

#endif
