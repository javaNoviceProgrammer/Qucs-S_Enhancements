/*
 * shellenvironment.cpp - the environment of the account's login shell,
 *                        for an application started from the desktop
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "shellenvironment.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <cctype>

namespace qucs_s::shellenv {

namespace {

// Printed by the shell before the environment: what comes before it
// (a greeting, a profile's chatter) is not part of the dump.
const char* const Marker = "__QUCS_ENVIRONMENT__";

// Set by and for a shell session; nothing an application wants.
const QSet<QString>& skippedNames()
{
    static const QSet<QString> names = {
        QStringLiteral("PWD"), QStringLiteral("OLDPWD"), QStringLiteral("SHLVL"), QStringLiteral("_"),
        QStringLiteral("TERM"), QStringLiteral("TTY"), QStringLiteral("COLUMNS"), QStringLiteral("LINES"),
        QStringLiteral("PS1"), QStringLiteral("PS2"), QStringLiteral("PROMPT"), QStringLiteral("RPROMPT"),
        QStringLiteral("HISTFILE"), QStringLiteral("HISTSIZE"), QStringLiteral("SAVEHIST"),
        QStringLiteral("TERM_PROGRAM"), QStringLiteral("TERM_PROGRAM_VERSION"), QStringLiteral("TERM_SESSION_ID"),
        QStringLiteral("SHELL_SESSION_ID"), QStringLiteral("ZDOTDIR"), QStringLiteral("STY"), QStringLiteral("TMUX"),
    };
    return names;
}

QByteArray runShell(const QString& shell, const QStringList& args, int timeoutMs)
{
    QProcess p;
    p.setProgram(shell);
    p.setArguments(args);
    p.setStandardInputFile(QProcess::nullDevice());   // nothing to read: a profile that asks gets EOF
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.setStandardErrorFile(QProcess::nullDevice());
    p.start();
    if (!p.waitForStarted(timeoutMs)) return {};
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(1000);
        return {};
    }
    return p.readAllStandardOutput();
}

QStringList splitPath(const QString& path)
{
    return path.split(QDir::listSeparator(), Qt::SkipEmptyParts);
}

} // namespace

QStringList parseEnvironmentDump(const QByteArray& output)
{
    const QByteArray marker = QByteArray(Marker) + QByteArray(1, '\0');
    const int at = output.indexOf(marker);
    if (at < 0) return {};
    const QByteArray dump = output.mid(at + marker.size());
    QStringList entries;
    for (const QByteArray& entry : dump.split('\0')) {
        if (entry.isEmpty()) continue;
        const int eq = entry.indexOf('=');
        if (eq <= 0) continue;
        const QByteArray name = entry.left(eq);
        bool validName = (name[0] == '_' || std::isalpha(static_cast<unsigned char>(name[0])));
        for (int i = 1; i < name.size() && validName; ++i)
            validName = name[i] == '_' || std::isalnum(static_cast<unsigned char>(name[i]));
        if (!validName) continue;
        entries << QString::fromLocal8Bit(entry);
    }
    return entries;
}

QStringList loginShellEnvironment(const QString& shellIn, int timeoutMs)
{
    QString shell = shellIn.isEmpty() ? qEnvironmentVariable("SHELL") : shellIn;
    if (shell.isEmpty() || !QFileInfo(shell).isExecutable()) shell = QStringLiteral("/bin/sh");
    // The marker, then the environment NUL-separated (env -0 on the
    // systems this runs on; a shell without it prints nothing usable and
    // the fallback below is tried).
    const QString command = QStringLiteral("printf '%s\\0' %1; exec /usr/bin/env -0").arg(QLatin1String(Marker));
    // A login shell, interactive: the profile files of both kinds, which
    // is where the PATH additions and the exports live. A profile that
    // starts another shell (zsh -> fish) or a terminal multiplexer gets
    // EOF on its input and ends; one that hangs is killed at the timeout.
    QStringList entries = parseEnvironmentDump(runShell(shell, {QStringLiteral("-l"), QStringLiteral("-i"), QStringLiteral("-c"), command}, timeoutMs));
    if (entries.isEmpty())   // then a plain login shell
        entries = parseEnvironmentDump(runShell(shell, {QStringLiteral("-l"), QStringLiteral("-c"), command}, timeoutMs));
    return entries;
}

QString mergedPath(const QString& processPath, const QString& shellPath)
{
    const QStringList shellDirs = splitPath(shellPath);
    QStringList merged;
    for (const QString& dir : splitPath(processPath))
        if (!shellDirs.contains(dir) && !merged.contains(dir)) merged << dir;
    for (const QString& dir : shellDirs)
        if (!merged.contains(dir)) merged << dir;
    return merged.join(QDir::listSeparator());
}

QStringList changesFor(const QStringList& entries, const QStringList& current, const QString& currentPath)
{
    QSet<QString> have;
    for (const QString& entry : current) have.insert(entry.section(QLatin1Char('='), 0, 0));
    QStringList changes;
    for (const QString& entry : entries) {
        const QString name = entry.section(QLatin1Char('='), 0, 0);
        const QString value = entry.section(QLatin1Char('='), 1);
        if (skippedNames().contains(name)) continue;
        if (name == QLatin1String("PATH")) {
            const QString merged = mergedPath(currentPath, value);
            if (merged != currentPath) changes << QStringLiteral("PATH=") + merged;
            continue;
        }
        if (have.contains(name)) continue;   // what the process was given stays
        changes << entry;
    }
    return changes;
}

QStringList importLoginShellEnvironment(const QString& shell, int timeoutMs)
{
#ifdef Q_OS_WIN
    Q_UNUSED(shell); Q_UNUSED(timeoutMs);
    return {};
#else
    if (qEnvironmentVariableIsSet("QUCS_NO_SHELL_ENV")) return {};
    const QStringList entries = loginShellEnvironment(shell, timeoutMs);
    if (entries.isEmpty()) return {};
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QStringList current;
    const QStringList keys = env.keys();
    for (const QString& key : keys) current << key + QLatin1Char('=') + env.value(key);
    QStringList names;
    for (const QString& change : changesFor(entries, current, qEnvironmentVariable("PATH"))) {
        const QString name = change.section(QLatin1Char('='), 0, 0);
        qputenv(name.toLocal8Bit().constData(), change.section(QLatin1Char('='), 1).toLocal8Bit());
        names << name;
    }
    return names;
#endif
}

} // namespace qucs_s::shellenv
