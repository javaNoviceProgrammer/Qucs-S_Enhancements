/*
 * shellenvironment.h - the environment of the account's login shell,
 *                      for an application started from the desktop
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef SHELLENVIRONMENT_H
#define SHELLENVIRONMENT_H

#include <QString>
#include <QStringList>

/*!
 * An application started from the Finder, the Dock or a desktop menu
 * does not get the environment a terminal gives: not the PATH the shell
 * profile builds (Homebrew, ~/bin, ...), none of the variables it
 * exports (SPICE_LIB_DIR, say). This asks the account's login shell for
 * its environment and brings into the process what the process does not
 * have - PATH is merged, the shell's order kept - so that the simulators,
 * OpenVAF, Python and the shell in the Terminal dock are found and set up
 * as in a terminal.
 */
namespace qucs_s::shellenv {

/// The environment the login shell provides, as "NAME=value" entries;
/// empty when the shell cannot be run or says nothing usable. \a shell is
/// $SHELL (or /bin/sh) when empty. The shell is run as a login shell and
/// interactively (the profile files of both), then, if that gave nothing,
/// as a plain login shell; each try is given \a timeoutMs.
QStringList loginShellEnvironment(const QString& shell = QString(), int timeoutMs = 3000);

/// The "NAME=value" entries of a shell's output: what follows the marker
/// the shell was told to print, NUL-separated - a profile's greeting or
/// warning before it does no harm.
QStringList parseEnvironmentDump(const QByteArray& output);

/// What importing \a entries would change: the variables the process
/// lacks, and PATH, when the shell's has directories the process's has
/// not - as "NAME=value" pairs to set. Variables of no use outside a
/// shell (PWD, SHLVL, TERM, ...) are left out. \a current is the
/// process's environment, \a currentPath its PATH.
QStringList changesFor(const QStringList& entries, const QStringList& current, const QString& currentPath);

/// The PATH the process should have: its own directories the shell's
/// PATH lacks (given on purpose, they stay in front), then the shell's
/// in the shell's order.
QString mergedPath(const QString& processPath, const QString& shellPath);

/// Brings the login shell's environment into the process (see above).
/// Nothing is done when QUCS_NO_SHELL_ENV is set, or on Windows, where a
/// desktop start has the user's environment already. Returns the names
/// of the variables set or changed.
QStringList importLoginShellEnvironment(const QString& shell = QString(), int timeoutMs = 3000);

} // namespace qucs_s::shellenv

#endif // SHELLENVIRONMENT_H
