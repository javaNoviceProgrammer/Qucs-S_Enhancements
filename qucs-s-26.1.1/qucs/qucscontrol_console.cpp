/*
 * qucscontrol_console.cpp - Claude's console tool: a line typed into the
 *                           Octave, Python Shell or Terminal dock, as the
 *                           user types it there, and what it printed - the
 *                           user asked about each line (it runs, with the
 *                           user's rights)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "qucscontrol.h"
#include "qucscontrol_p.h"
#include "octave_window.h"
#include "processconsole.h"
#include "qucs.h"

#include <QDockWidget>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>
#include <memory>

using namespace qucs_s::control;

namespace {

// A terminal's output as text: no colour or cursor codes, no carriage
// returns.
QString plainOutput(QString text)
{
    static const QRegularExpression codes(QStringLiteral("\\x1B(\\[[0-?]*[ -/]*[@-~]|\\][^\\x07\\x1B]*(\\x07|\\x1B\\\\)|[@-Z\\\\-_])"));
    text.remove(codes);
    text.remove(QLatin1Char('\r'));
    return text;
}

// The last \a count lines of \a text.
QString lastLines(const QString& text, int count)
{
    QStringList lines = text.split(QLatin1Char('\n'));
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) lines.removeLast();
    if (lines.size() > count) lines = lines.mid(lines.size() - count);
    return lines.join(QLatin1Char('\n'));
}

} // namespace

namespace qucs_s::control {

QString consoleReply(const QString& shown, const QString& input, const QRegularExpression& prompt, bool* promptBack)
{
    QString said = shown;
    // What it printed is what came after the line typed, echoed back - by
    // the terminal, and again after the prompt by the program's own line
    // editor (Python's, on its first line) -: not the line, nor what came
    // before it (a program started for it says who it is first: Python's
    // banner).
    const QString typed = input.trimmed();
    const auto echo = [&typed, &prompt](const QString& line) {
        const QString l = line.trimmed();
        if (l == typed) return 1;   // the terminal's
        if (l.endsWith(typed) && prompt.match(l.left(l.size() - typed.size()).trimmed() + QLatin1Char(' ')).hasMatch()) return 2;   // after the prompt
        return 0;
    };
    if (!typed.isEmpty()) {
        const QStringList lines = said.split(QLatin1Char('\n'));
        qsizetype at = -1;
        for (qsizetype i = 0; i < lines.size() && at < 0; ++i)
            if (echo(lines.at(i)) != 0) at = i;
        if (at >= 0) {
            qsizetype from = at + 1;
            if (echo(lines.at(at)) == 1 && from < lines.size() && echo(lines.at(from)) == 2) ++from;   // (shown twice)
            said = lines.mid(from).join(QLatin1Char('\n'));
        }
    }
    // Its prompt back: not what it printed either.
    const QString tail = said.right(40);
    *promptBack = !tail.trimmed().isEmpty() && prompt.match(tail.trimmed() + QLatin1Char(' ')).hasMatch();
    if (*promptBack) {
        QStringList kept = said.split(QLatin1Char('\n'));
        // (">>> " ends in its space already: matched as written.)
        if (!kept.isEmpty() && prompt.match(kept.last().trimmed() + QLatin1Char(' ')).hasMatch()) kept.removeLast();
        said = kept.join(QLatin1Char('\n'));
    }
    return said;
}

} // namespace qucs_s::control

void QucsControl::console(const QJsonObject& args, const Done& done)
{
    const QString kind = args.value(QLatin1String("kind")).toString().trimmed().toLower();
    const QString input = args.value(QLatin1String("input")).toString();
    const bool interrupt = args.value(QLatin1String("interrupt")).toBool();
    const int wait = std::clamp(args.value(QLatin1String("wait")).toInt(10), 1, 600);
    const int lines = std::clamp(args.value(QLatin1String("lines")).toInt(40), 1, 2000);
    if (!input.isEmpty() && interrupt) {
        done(errorResult(tr("'input' or 'interrupt', not both.")));
        return;
    }
    if (input.contains(QLatin1Char('\n'))) {
        done(errorResult(tr("'input' is one line, as typed: call again for the next.")));
        return;
    }
    // Each console: what it shows, whether it runs, how a line is typed
    // into it and a run stopped, and what its prompt looks like.
    std::function<QString()> read;
    std::function<bool()> running;
    std::function<bool(const QString&)> type;
    std::function<void()> stop;
    QDockWidget* dock = nullptr;
    QRegularExpression prompt;
    if (kind == QLatin1String("python") || kind == QLatin1String("terminal")) {
        const bool python = kind == QLatin1String("python");
        QPointer<ProcessConsole> c = python ? a_app->pythonConsole() : a_app->terminalConsole();
        dock = python ? a_app->pythonDockWidget() : a_app->terminalDockWidget();
        if (c == nullptr) {
            done(errorResult(tr("There is no %1 console here.").arg(kind)));
            return;
        }
        read = [c] { return c ? plainOutput(c->outputText()) : QString(); };
        running = [c] { return c && c->isRunning(); };
        type = [c](const QString& line) {
            if (!c) return false;
            c->sendLine(line);
            return c->isRunning();
        };
        stop = [c] {
            if (c) c->interrupt();
        };
        prompt = python ? QRegularExpression(QStringLiteral("(>>>|\\.\\.\\.) ?$")) : QRegularExpression(QStringLiteral("[$#%>\\x{276F}] ?$"));
    } else if (kind == QLatin1String("octave")) {
        QPointer<OctaveWindow> o = a_app->octaveWindow();
        if (o == nullptr) {
            done(errorResult(tr("There is no Octave console here.")));
            return;
        }
        for (QDockWidget* d : a_app->findChildren<QDockWidget*>())
            if (d->isAncestorOf(o)) dock = d;
        read = [o] { return o ? o->outputText() : QString(); };
        running = [o] { return o && o->isRunning(); };
        type = [o](const QString& line) {
            if (!o || !o->startOctave()) return false;
            o->sendCommand(line);
            return true;
        };
        stop = nullptr;   // (no Ctrl-C to Octave here)
        prompt = QRegularExpression(QStringLiteral(">> ?$"));
    } else {
        done(errorResult(tr("'kind' is octave, python or terminal.")));
        return;
    }
    // Only read: its last lines.
    if (input.isEmpty() && !interrupt) {
        done(jsonResult(QJsonObject{{QStringLiteral("kind"), kind},
                                    {QStringLiteral("running"), running()},
                                    {QStringLiteral("output"), lastLines(read(), lines)}}));
        return;
    }
    if (interrupt && !stop) {
        done(errorResult(tr("The Octave console takes no interrupt here.")));
        return;
    }
    // In view, as the user types there.
    if (dock != nullptr) {
        dock->show();
        dock->raise();
    }
    const qsizetype before = read().size();
    if (interrupt) {
        stop();
    } else if (!type(input)) {
        done(errorResult(kind == QLatin1String("octave")
                             ? tr("Octave could not be started: Application Settings > Locations > Octave.")
                             : tr("The %1 console's program could not be started.").arg(kind)));
        return;
    }
    // What it prints, until its prompt is back and nothing more comes (or,
    // with no prompt to know, nothing more for a while), or 'wait' runs out.
    struct Watch {
        QElapsedTimer clock;
        QElapsedTimer quiet;
        qsizetype seen = 0;
    };
    auto w = std::make_shared<Watch>();
    w->clock.start();
    w->quiet.start();
    w->seen = before;
    auto* timer = new QTimer(this);
    timer->setInterval(100);
    connect(timer, &QTimer::timeout, this, [=] {
        const QString all = read();
        if (all.size() != w->seen) {
            w->seen = all.size();
            w->quiet.restart();
        }
        bool promptBack = false;
        const QString said = consoleReply(all.mid(std::min(before, all.size())), input, prompt, &promptBack);
        // Its prompt back (Octave's, which a pipe may not show: quiet for a
        // while after it printed) - a run that prints nothing for long is
        // not over.
        const bool settled = w->clock.elapsed() > 300
                             && ((promptBack && w->quiet.elapsed() > 300)
                                 || (kind == QLatin1String("octave") && !said.trimmed().isEmpty() && w->quiet.elapsed() > 1500));
        const bool timedOut = w->clock.elapsed() >= qint64(wait) * 1000;
        if (!settled && !timedOut) return;
        timer->stop();
        timer->deleteLater();
        QJsonObject result{{QStringLiteral("kind"), kind},
                           {QStringLiteral("output"), lastLines(said, lines)},
                           {QStringLiteral("finished"), settled}};
        if (!settled)
            result.insert(QStringLiteral("note"),
                          stop ? tr("Still running after %1 s: it goes on in the console. 'interrupt': true stops it (Ctrl-C); a "
                                    "call with no input reads what came since.").arg(wait)
                               : tr("Still running after %1 s: it goes on in the console; a call with no input reads what came "
                                    "since.").arg(wait));
        done(jsonResult(result));
    });
    timer->start();
}
