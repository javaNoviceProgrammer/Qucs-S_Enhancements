/***************************************************************************
                              crashhandler.cpp
                             ------------------
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "crashhandler.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QSysInfo>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>

#if defined(_WIN32)
#  include <io.h>
#  include <fcntl.h>
#  define QUCS_OPEN(path)  _open(path, _O_WRONLY | _O_CREAT | _O_TRUNC, 0644)
#  define QUCS_WRITE       _write
#  define QUCS_CLOSE       _close
#else
#  include <fcntl.h>
#  include <unistd.h>
#  define QUCS_OPEN(path)  ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644)
#  define QUCS_WRITE       ::write
#  define QUCS_CLOSE       ::close
#endif

#if defined(__APPLE__) || defined(__linux__)
#  include <execinfo.h>
#  define QUCS_HAVE_BACKTRACE 1
#endif

namespace qucs_s::crash {

namespace {

// Everything the handler needs is prepared at install time as plain C
// buffers: no allocation, no Qt, inside the handler.
constexpr int kPathMax = 1024;
char g_reportDir[kPathMax] = {0};
char g_reportDirOverride[kPathMax] = {0};
char g_header[512] = {0};
std::function<void()> g_emergency;
std::atomic<bool> g_handling{false};

// Ring buffer of recent log lines.
constexpr int kRingLines = 64;
constexpr int kRingWidth = 400;
char g_ring[kRingLines][kRingWidth];
std::atomic<int> g_ringNext{0};

void put(int fd, const char* s)
{
    if (fd < 0 || s == nullptr) return;
    const auto n = QUCS_WRITE(fd, s, static_cast<unsigned>(strlen(s)));
    (void)n;
}

const char* signalName(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV (invalid memory access)";
    case SIGFPE:  return "SIGFPE (arithmetic error)";
    case SIGILL:  return "SIGILL (illegal instruction)";
    case SIGABRT: return "SIGABRT (abort)";
#ifdef SIGBUS
    case SIGBUS:  return "SIGBUS (bus error)";
#endif
    default:      return "unknown signal";
    }
}

void resetHandlers()
{
    signal(SIGSEGV, SIG_DFL);
    signal(SIGFPE, SIG_DFL);
    signal(SIGILL, SIG_DFL);
    signal(SIGABRT, SIG_DFL);
#ifdef SIGBUS
    signal(SIGBUS, SIG_DFL);
#endif
}

// Writes <reportDir>/crash-<epoch>.txt. Returns the descriptor (or -1).
int openReport(char* pathOut, size_t pathOutSize)
{
    const long long now = static_cast<long long>(time(nullptr));
    snprintf(pathOut, pathOutSize, "%s/crash-%lld.txt", g_reportDir, now);
    return QUCS_OPEN(pathOut);
}

void writeReport(int fd, const char* what)
{
    put(fd, g_header);
    put(fd, "What: ");
    put(fd, what);
    put(fd, "\n\nBacktrace (innermost first):\n");
#ifdef QUCS_HAVE_BACKTRACE
    void* frames[96];
    const int n = backtrace(frames, 96);
    backtrace_symbols_fd(frames, n, fd);
#else
    put(fd, "  (not available on this platform)\n");
#endif
    put(fd, "\nRecent log messages (oldest first):\n");
    const int next = g_ringNext.load();
    for (int i = 0; i < kRingLines; ++i) {
        const char* line = g_ring[(next + i) % kRingLines];
        if (line[0] == '\0') continue;
        put(fd, "  ");
        put(fd, line);
        put(fd, "\n");
    }
}

void die(const char* what, int sig)
{
    resetHandlers();
    if (g_handling.exchange(true)) {   // crashed again while handling: give up
        if (sig) raise(sig);
        abort();
    }
    char path[kPathMax];
    const int fd = openReport(path, sizeof path);
    if (fd >= 0) {
        writeReport(fd, what);
        QUCS_CLOSE(fd);
        put(2, "\n*** qucs-s crashed: ");
        put(2, what);
        put(2, "\n*** report written to ");
        put(2, path);
        put(2, "\n");
    }
    if (g_emergency) {
        put(2, "*** trying to autosave modified documents...\n");
        g_emergency();
    }
    if (sig) raise(sig);
    abort();
}

void onSignal(int sig)
{
    die(signalName(sig), sig);
}

void onTerminate()
{
    const char* what = "std::terminate (uncaught exception)";
    // Try to name the exception without allocating in the common case.
    if (const std::exception_ptr ep = std::current_exception()) {
        try { std::rethrow_exception(ep); }
        catch (const std::exception& e) { what = e.what(); }
        catch (...) {}
    }
    die(what, 0);
}

QString defaultReportDirectory()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (dir.isEmpty())
        dir = QDir::homePath() + QStringLiteral("/.qucs-s");
    return dir + QStringLiteral("/crash-reports");
}

QString sessionMarker()
{
    return reportDirectory() + QStringLiteral("/session.running");
}

} // namespace

QString reportDirectory()
{
    QString dir = g_reportDirOverride[0] ? QString::fromUtf8(g_reportDirOverride) : defaultReportDirectory();
    QDir().mkpath(dir);
    return dir;
}

void setReportDirectory(const QString& dir)
{
    snprintf(g_reportDirOverride, sizeof g_reportDirOverride, "%s", dir.toUtf8().constData());
    snprintf(g_reportDir, sizeof g_reportDir, "%s", reportDirectory().toUtf8().constData());
}

void noteMessage(QtMsgType type, const QString& message)
{
    const char* prefix = "";
    switch (type) {
    case QtDebugMsg:    prefix = "debug: "; break;
    case QtInfoMsg:     prefix = "info: "; break;
    case QtWarningMsg:  prefix = "warning: "; break;
    case QtCriticalMsg: prefix = "critical: "; break;
    case QtFatalMsg:    prefix = "fatal: "; break;
    }
    const int slot = g_ringNext.fetch_add(1) % kRingLines;
    snprintf(g_ring[slot], kRingWidth, "%s%s", prefix, message.toUtf8().constData());
}

void install(std::function<void()> emergency)
{
    g_emergency = std::move(emergency);
    snprintf(g_reportDir, sizeof g_reportDir, "%s", reportDirectory().toUtf8().constData());
    snprintf(g_header, sizeof g_header,
             "qucs-s crash report\nVersion: %s (%s)\nQt: %s\nOS: %s\n",
#ifdef PACKAGE_VERSION
             PACKAGE_VERSION,
#else
             "?",
#endif
#ifdef GIT
             GIT,
#else
             "",
#endif
             qVersion(), QSysInfo::prettyProductName().toUtf8().constData());
#ifdef QUCS_HAVE_BACKTRACE
    // backtrace() may allocate the first time it runs; do that now, not in
    // the handler.
    void* warm[4];
    backtrace(warm, 4);
#endif
    signal(SIGSEGV, onSignal);
    signal(SIGFPE, onSignal);
    signal(SIGILL, onSignal);
    signal(SIGABRT, onSignal);
#ifdef SIGBUS
    signal(SIGBUS, onSignal);
#endif
    std::set_terminate(onTerminate);
}

bool markSessionStart()
{
    const QString marker = sessionMarker();
    const bool crashedLastTime = QFileInfo::exists(marker);
    QFile f(marker);
    if (f.open(QIODevice::WriteOnly))
        f.write(QByteArray::number(static_cast<qlonglong>(QDateTime::currentSecsSinceEpoch())));
    return crashedLastTime;
}

void markSessionEnd()
{
    QFile::remove(sessionMarker());
}

QString latestReport()
{
    const QFileInfoList reports = QDir(reportDirectory())
        .entryInfoList({QStringLiteral("crash-*.txt")}, QDir::Files, QDir::Time);
    return reports.isEmpty() ? QString() : reports.first().absoluteFilePath();
}

} // namespace qucs_s::crash
