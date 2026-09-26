/*
 * simulatorlog.cpp - a simulator's errors and warnings, read out of what
 *                    it printed: the message, the netlist line, the part
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "simulatorlog.h"

#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

namespace qucs_s::simlog {

namespace {

// What begins a problem, and how bad it is: 1 an error, 0 a warning, -1
// not one.
int severityOf(const QString& line)
{
    static const QRegularExpression error(
        QStringLiteral("^(error\\b|syntax error|expression err|netlist error|msg_error|msg_fatal|user fatal)"
                       "|simulation\\(s\\) aborted|simulation aborted|analysis aborted|timestep too small"
                       "|cannot proceed|^no ground found|^no simulation found|^only dc simulation found"
                       "|singular matrix|gmin stepping failed|source stepping failed|transient op failed"
                       "|failed to start simulator|simulator crashed|^netlist line no\\.\\s*\\d+"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression warning(QStringLiteral("^(warning\\b|msg_warning)"),
                                            QRegularExpression::CaseInsensitiveOption);
    if (warning.match(line).hasMatch()) return 0;
    if (error.match(line).hasMatch()) return 1;
    return -1;
}

// What ngspice says as it goes, which is no part of a problem.
bool isProgress(const QString& line)
{
    static const QRegularExpression progress(
        QStringLiteral("^(circuit:|doing analysis|using sparse|using klu|note:|no\\. of data rows|binary raw file|ascii raw file"
                       "|reset re-loads|initial transient solution|ngspice-|ngspice started|xyce started|simulation finished"
                       "|reference value|total analysis time|total elapsed time|node\\s+voltage|----)"),
        QRegularExpression::CaseInsensitiveOption);
    return progress.match(line).hasMatch();
}

QString prefixless(QString message)
{
    static const QRegularExpression prefix(QStringLiteral("^(error|warning)(\\s+from\\s+\\w+)?\\s*:\\s*"),
                                           QRegularExpression::CaseInsensitiveOption);
    // "Error on line 3 or its substitute:": the line is told on its own.
    static const QRegularExpression onLine(QStringLiteral("^((error|warning)\\s+on\\s+line\\s+\\d+(\\s+or its substitute)?|netlist line no\\.\\s*\\d+)\\s*:\\s*"),
                                           QRegularExpression::CaseInsensitiveOption);
    message.remove(prefix);
    message.remove(onLine);
    return message.trimmed();
}

} // namespace

QList<Problem> problems(const QString& output, const QStringList& netlist, const QHash<QString, QString>& parts)
{
    static const QRegularExpression lineNumber(
        QStringLiteral("(?:on line|line no\\.|at or near line|in line|line)\\s*(\\d+)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression nodeName(QStringLiteral("node\\s+['\"]?([^'\"\\s;,]+)['\"]?"),
                                             QRegularExpression::CaseInsensitiveOption);
    // A name, or a part of one: a subcircuit's device is "n.xpd1.npd"
    // (its type, the instance, itself), a model of it "xpd1:pdmod".
    static const QRegularExpression words(QStringLiteral("[A-Za-z_][\\w#]*"));

    QList<Problem> list;
    QSet<QString> seen;
    const QStringList lines = output.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const QString head = lines.at(i).trimmed();
        const int severity = severityOf(head);
        if (severity < 0) continue;
        Problem p;
        p.severity = severity > 0 ? Problem::Error : Problem::Warning;
        QStringList message{prefixless(head)};
        // What follows it: indented lines (the netlist line, where it is),
        // and after a head that ends in ':' - "on line 6 :" - the line
        // that says what is wrong.
        const bool namesALine = head.endsWith(QLatin1Char(':')) || head.contains(QLatin1String("from line"), Qt::CaseInsensitive)
                                || head.contains(QLatin1String("or its substitute"), Qt::CaseInsensitive);
        bool reasonTaken = false;
        int k = i + 1;
        for (; k < lines.size(); ++k) {
            const QString raw = lines.at(k);
            const QString text = raw.trimmed();
            if (text.isEmpty()) break;
            if (severityOf(text) >= 0 || isProgress(text)) break;
            const bool indented = raw.startsWith(QLatin1Char(' ')) || raw.startsWith(QLatin1Char('\t'));
            if (!indented) {
                // "on line 6 :" and its line, then why; or "Netlist line
                // no. 4:" and why at once (older ngspice).
                if (!namesALine || reasonTaken) break;
                message << text;
                reasonTaken = true;
                continue;
            }
            if (text.startsWith(QLatin1String("Simulation interrupted"), Qt::CaseInsensitive)) continue;
            const QRegularExpressionMatch at = lineNumber.match(text);
            if (at.hasMatch() && text.startsWith(QLatin1String("in line"), Qt::CaseInsensitive)) {
                p.line = at.captured(1).toInt();
                continue;
            }
            if (namesALine && p.netlistLine.isEmpty()) {
                QString netlistText = text;
                if (netlistText.endsWith(QLatin1String("..."))) netlistText.chop(3);
                p.netlistLine = netlistText.trimmed();
                continue;
            }
            message << text;
        }
        i = k - 1;

        p.message = message.join(QLatin1Char(' ')).simplified();
        if (p.line == 0) {
            const QRegularExpressionMatch at = lineNumber.match(head);
            if (at.hasMatch()) p.line = at.captured(1).toInt();
        }
        if (p.netlistLine.isEmpty() && p.line > 0 && p.line <= netlist.size()) p.netlistLine = netlist.at(p.line - 1).trimmed();
        const QRegularExpressionMatch node = nodeName.match(p.message);
        if (node.hasMatch()) p.node = node.captured(1);
        // The part: the netlist line's (a subcircuit's, for a device in
        // it), else the first part the message names.
        const QString first = p.netlistLine.section(QLatin1Char(' '), 0, 0).toLower();
        if (!first.startsWith(QLatin1Char('.'))) {
            if (parts.contains(first)) p.component = parts.value(first);
            for (auto it = words.globalMatch(first); p.component.isEmpty() && it.hasNext();) {
                const QString word = it.next().captured(0);
                if (parts.contains(word)) p.component = parts.value(word);
            }
        }
        for (auto it = words.globalMatch(p.message); p.component.isEmpty() && it.hasNext();) {
            const QString word = it.next().captured(0).toLower();
            if (parts.contains(word)) p.component = parts.value(word);
        }

        const QString key = QString::number(p.severity) + p.message + p.netlistLine;
        if (seen.contains(key)) continue;
        seen.insert(key);
        list << p;
    }
    return list;
}

QJsonArray toJson(const QList<Problem>& problems)
{
    QJsonArray array;
    for (const Problem& p : problems) {
        QJsonObject o{{QStringLiteral("severity"), p.severity == Problem::Error ? QStringLiteral("error") : QStringLiteral("warning")},
                      {QStringLiteral("message"), p.message}};
        if (p.line > 0) o.insert(QStringLiteral("netlist line number"), p.line);
        if (!p.netlistLine.isEmpty()) o.insert(QStringLiteral("netlist line"), p.netlistLine);
        if (!p.component.isEmpty()) o.insert(QStringLiteral("component"), p.component);
        if (!p.node.isEmpty()) o.insert(QStringLiteral("node"), p.node);
        array.append(o);
    }
    return array;
}

} // namespace qucs_s::simlog
