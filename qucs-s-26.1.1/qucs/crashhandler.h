/***************************************************************************
                               crashhandler.h
                              ----------------
    Last-resort diagnostics and document rescue when the process dies.
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef QUCS_CRASHHANDLER_H
#define QUCS_CRASHHANDLER_H

#include <QString>
#include <QtGlobal>
#include <functional>

/*!
 * When a fatal signal (SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT) or an
 * uncaught exception ends the process, the handler installed here
 *
 *  1. writes a report to reportDirectory(): version, commit, what happened,
 *     a backtrace where the platform provides one, and the most recent log
 *     messages (see noteMessage);
 *  2. runs the emergency callback given to install(), which the application
 *     uses to autosave modified documents;
 *  3. lets the default action happen, so the OS still produces its own
 *     crash report.
 *
 * Everything in 2. runs in a signal handler and is best effort by nature:
 * the handlers are reset first so a second fault simply terminates.
 *
 * A session marker (markSessionStart/markSessionEnd) records whether the
 * previous run exited cleanly; on start the application can then tell the
 * user that it crashed and where the report is.
 */
namespace qucs_s::crash {

void install(std::function<void()> emergency);

QString reportDirectory();
/// Override the report directory (tests). Empty restores the default.
void setReportDirectory(const QString& dir);

/// Remember a log message for the report (called by the Qt message handler).
void noteMessage(QtMsgType type, const QString& message);

/// Session marker: true from markSessionStart() if the previous session
/// did not call markSessionEnd(), i.e. it crashed or was killed.
bool markSessionStart();
void markSessionEnd();

/// The newest report in reportDirectory(), or an empty string.
QString latestReport();

} // namespace qucs_s::crash

#endif // QUCS_CRASHHANDLER_H
