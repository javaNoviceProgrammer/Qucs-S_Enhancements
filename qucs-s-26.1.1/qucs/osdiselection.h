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

#include <QList>
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

/// Whether \a file is a shared library the \a simulator program cannot
/// load: of another format (a Mach-O one on Linux, an ELF one on macOS),
/// or with code for none of its processors (an x86-64 one for an Arm
/// ngspice). Without a program to read - a script, not found - this
/// Qucs-S's platform stands for it (on macOS its format alone: Rosetta).
/// A file of no format it knows is not said to be.
bool builtForAnotherPlatform(const QString& file, const QString& simulator = QString());

/// Whether the library defines \a module (any case): its modules when it
/// can be loaded, else whether the name is a string in its bytes.
bool defines(const QString& osdiFile, const QString& module);

/// The libraries of \a osdiFiles a netlist that uses \a types needs, in
/// the order given: each type a library defines is taken from one library
/// - one already taken, else the one with the most of what is needed,
/// else the most recently built. What was left out for another library is
/// said in \a notes.
QStringList needed(const QStringList& osdiFiles, const QSet<QString>& types,
                   QStringList* notes = nullptr);

/// Whether the Verilog-A source defines \a module (any case).
bool sourceDefines(const QString& vaFile, const QString& module);

/// The files a Verilog-A source brings in with `include "FILE" - and
/// the files those bring in - that are there: taken against the folder of
/// the file that names them (where a link leads, for a link).
QStringList sourceIncludes(const QString& vaFile);

/// Where the library of the Verilog-A source goes.
enum class Into {
    Beside,              ///< NAME.osdi beside it, as OpenVAF writes it
    CacheReadOnly,       ///< the cache: its folder (or NAME.osdi) cannot be written
    CacheKeepsForeign,   ///< the cache: NAME.osdi beside it is another platform's, kept
};

/// Where the library of the Verilog-A source \a vaFile is compiled to:
/// NAME.osdi beside it - unless that folder cannot be written (a library
/// folder shared read-only) or NAME.osdi there was built for another
/// platform than \a simulator's (builtForAnotherPlatform()), kept for the
/// computers it was built for. Then a folder of its own in \a cacheDir,
/// osdi/NAME-<hash of the source's path>/NAME.osdi (a link's: of where the
/// link is, not the file it leads to). Without \a cacheDir,
/// beside it. \a into, when given, says which.
QString buildTarget(const QString& vaFile, const QString& cacheDir, const QString& simulator = QString(),
                    Into* into = nullptr);

/// What OpenVAF is given to compile \a vaFile into \a library (none:
/// NAME.osdi beside \a vaFile): the source - for a link (a library's source
/// linked into a project), the file it leads to, so that its `include
/// lines are found beside that file whatever an OpenVAF does with links -,
/// then "-o library" when that is not where OpenVAF writes by itself
/// (beside the file it compiles).
QStringList compileArguments(const QString& vaFile, const QString& library = QString());

/// The library of the Verilog-A source to load: NAME.osdi beside it or
/// the one compiled into \a cacheDir for it (buildTarget()) - the first
/// \a simulator loads that is newer than the source and what it includes,
/// else the first it loads, else NAME.osdi beside it (another platform's).
/// Empty when there is none.
QString modelOf(const QString& vaFile, const QString& cacheDir, const QString& simulator = QString());

/// A Verilog-A source of the project to compile before a simulation.
struct Build {
    QString source;        ///< the .va
    QString library;       ///< what OpenVAF writes: buildTarget()
    Into into = Into::Beside;
    QString built;         ///< the library it has (modelOf()); empty when missing
    QStringList modules;   ///< the modules the netlist uses that it defines
    bool missing = false;  ///< no library has them yet (else: older than its source,
    bool foreign = false;  ///< or built for another platform: builtForAnotherPlatform())
};

/// The sources of \a vaFiles to compile for a netlist that uses \a types:
/// one defining a module used whose library (modelOf()) is older than it
/// or than a file it includes, or was built for another platform (a
/// library brought from elsewhere); or, when none of \a osdiFiles defines
/// the module, whose library is not there yet. \a simulator is the program
/// that will load the libraries (builtForAnotherPlatform()); \a cacheDir
/// where a library goes that cannot go beside its source (buildTarget()).
QList<Build> builds(const QStringList& vaFiles, const QStringList& osdiFiles, const QSet<QString>& types,
                    const QString& simulator = QString(), const QString& cacheDir = QString());

} // namespace qucs_s::osdi

#endif
