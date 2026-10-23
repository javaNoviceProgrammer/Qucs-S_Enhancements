/*
 * qucscontrol_script.cpp - run_script: a short JavaScript program that
 *                          calls Claude's Qucs-S tools, with loops and
 *                          conditions between the calls, in one turn
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

#include "mcpserver.h"
#include "qucs.h"
#include "schematic.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>

#ifdef QUCS_HAVE_QML
#include <QElapsedTimer>
#include <QJSEngine>
#include <QJSValue>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

using namespace qucs_s::control;

bool QucsControl::scriptingBuilt()
{
#ifdef QUCS_HAVE_QML
    return true;
#else
    return false;
#endif
}

#ifdef QUCS_HAVE_QML

namespace {

// What a script sees: qucs.call(tool, args), qucs.log(text).
class ScriptApi : public QObject
{
    Q_OBJECT

public:
    ScriptApi(QucsControl* control, QJSEngine* engine) : a_control(control), a_engine(engine) {}

    Q_INVOKABLE QJSValue call(const QString& tool, const QJSValue& args = QJSValue())
    {
        if (tool == QLatin1String("run_script")) return fail(QStringLiteral("run_script cannot run from a script"));
        // (A script runs alone: every other call would wait with it.)
        if (tool == QLatin1String("wait_for")) return fail(QStringLiteral("wait_for cannot run from a script, which every other call waits for"));
        if (++a_calls > kMostCalls) return fail(QStringLiteral("more than %1 calls: the script is stopped").arg(kMostCalls));
        QJsonObject arguments;
        if (!args.isUndefined() && !args.isNull()) {
            if (!args.isObject() || args.isArray()) return fail(QStringLiteral("%1's arguments are an object: qucs.call(\"%1\", {...})").arg(tool));
            arguments = QJsonObject::fromVariantMap(a_engine->fromScriptValue<QVariantMap>(args));
        }
        // Its own max_chars, as every tool takes it (and a batch's calls):
        // its answer cut.
        int most = 0;
        if (const QJsonValue given = arguments.take(QStringLiteral("max_chars"));
            !given.isUndefined() && !qucs_s::mcp::readMaxChars(given, &most))
            return fail(QStringLiteral("%1: %2").arg(tool, qucs_s::mcp::maxCharsRefusal(given)));
        // (A use the guards name: not unasked from a script.)
        if (const QString no = a_control->unaskedWithin(tool, arguments); !no.isEmpty()) return fail(no);
        a_running = a_last = tool;
        QJsonObject result = a_control->callNow(tool, arguments, 10 * 60 * 1000);
        a_running.clear();
        if (most > 0) result = qucs_s::mcp::trimmedTo(result, most);
        QStringList texts;
        for (const QJsonValue& v : result.value(QLatin1String("content")).toArray())
            if (v.toObject().value(QLatin1String("type")).toString() == QLatin1String("text"))
                texts << v.toObject().value(QLatin1String("text")).toString();
        if (result.value(QLatin1String("isError")).toBool()) {
            // (Its own name once: a tool's refusal names it already, and
            // "get_dataset: get_dataset: ..." came of saying it again -
            // the re-check of 23 October, 3.)
            const QString why = texts.isEmpty() ? QStringLiteral("failed") : texts.first();
            return fail(why.startsWith(tool + QLatin1Char(':')) ? why : QStringLiteral("%1: %2").arg(tool, why));
        }
        // Its answer as an object: its JSON, or {text} - with notes told beside it.
        const QJsonDocument doc = QJsonDocument::fromJson(texts.value(0).toUtf8());
        QJsonObject answer = doc.isObject() ? doc.object() : QJsonObject{{QStringLiteral("text"), texts.value(0)}};
        if (doc.isArray()) answer = QJsonObject{{QStringLiteral("items"), doc.array()}};
        if (texts.size() > 1) answer.insert(QStringLiteral("notes"), QJsonArray::fromStringList(texts.mid(1)));
        return a_engine->toScriptValue(answer.toVariantMap());
    }

    Q_INVOKABLE void log(const QJSValue& value)
    {
        if (a_log.size() >= 500) return;
        a_log << (value.isString() ? value.toString()
                                   : QString::fromUtf8(QJsonDocument::fromVariant(value.toVariant()).toJson(QJsonDocument::Compact)).left(4000));
        if (a_log.last().isEmpty()) a_log.last() = value.toString();
    }

    int calls() const { return a_calls; }
    QStringList logged() const { return a_log; }
    /// The tool whose call was under way when the script was stopped
    /// (empty: none - its own code was running between calls).
    QString running() const { return a_running; }
    /// The last tool it called (empty: it called none).
    QString last() const { return a_last; }

private:
    static constexpr int kMostCalls = 5000;
    QJSValue fail(const QString& message)
    {
        a_engine->throwError(QJSValue::GenericError, message);
        return QJSValue();
    }
    QucsControl* a_control;
    QJSEngine* a_engine;
    int a_calls = 0;
    QString a_running, a_last;   // the call under way, and the last made (MCP wishlist N10)
    QStringList a_log;
};

} // namespace

QJsonObject QucsControl::runScript(const QJsonObject& args)
{
    const QString script = args.value(QLatin1String("script")).toString();
    if (script.trimmed().isEmpty())
        return errorResult(tr("'script' is a JavaScript program: qucs.call(\"add_component\", {type: \"R\", x: 100, y: 100}) calls a tool."));
    const int timeout = std::clamp(args.value(QLatin1String("timeout")).toInt(60), 1, 600);
    // Put back when an atomic script fails: every open schematic.
    struct Kept {
        QPointer<Schematic> sch;
        QPair<QString, QString> state;
        Schematic::UndoStacks marks;
        bool changed;
    };
    QList<Kept> kept;
    const bool atomic = args.value(QLatin1String("atomic")).toBool();
    if (atomic)
        for (QucsDoc* doc : a_app->allDocuments())
            if (auto* sch = dynamic_cast<Schematic*>(doc)) kept.append(Kept{sch, sch->snapshotAll(), sch->undoStacks(), sch->getDocChanged()});

    QJSEngine engine;
    engine.installExtensions(QJSEngine::ConsoleExtension);   // console.log too (to stderr)
    ScriptApi api(this, &engine);
    QJSValue qucs = engine.newQObject(&api);
    engine.globalObject().setProperty(QStringLiteral("qucs"), qucs);
    // A script that runs too long is stopped (the engine is told from a
    // thread of its own: it runs on this one).
    std::mutex mutex;
    std::condition_variable over;
    bool finished = false, stopped = false;
    QElapsedTimer clock;
    clock.start();
    // The user's time is not the script's: while a call of it waits for an
    // answer to a card (askUser()), its clock stands still - the script was
    // "stopped after 60 s" where the user had taken that long to answer
    // (MCP wishlist N10). Watched in slices for that reason.
    const qint64 askedBefore = askedUserMs();
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> lock(mutex);
        while (!finished) {
            if (over.wait_for(lock, std::chrono::milliseconds(200), [&] { return finished; })) return;
            if (clock.elapsed() - (askedUserMs() - askedBefore) >= qint64(timeout) * 1000) {
                stopped = true;
                engine.setInterrupted(true);
                return;
            }
        }
    });
    QJSValue value = engine.evaluate(script, QStringLiteral("script"));
    // A 'return' outside a function - the result, as a function's body
    // gives it: the script run as one (a syntax error ran nothing yet).
    if (value.isError() && value.property(QStringLiteral("name")).toString() == QLatin1String("SyntaxError")
        && value.property(QStringLiteral("message")).toString().contains(QLatin1String("Return statement")))
        value = engine.evaluate(QStringLiteral("(function () {\n%1\n})()").arg(script), QStringLiteral("script"), 0);
    {
        std::lock_guard<std::mutex> lock(mutex);
        finished = true;
    }
    over.notify_all();
    watchdog.join();

    const qint64 userMs = askedUserMs() - askedBefore;
    QJsonObject result{{QStringLiteral("calls"), api.calls()}, {QStringLiteral("seconds"), (clock.elapsed() - userMs) / 1000.0}};
    // The user's own time beside the script's, where a card was answered.
    if (userMs > 0) {
        result.insert(QStringLiteral("waited for the user"), userMs / 1000.0);
        result.insert(QStringLiteral("note"), tr("'seconds' is the script's own time: the %1 s it waited for the user to answer a "
                                                 "card are not counted against 'timeout'.").arg(userMs / 1000.0, 0, 'f', 1));
    }
    if (!api.logged().isEmpty()) result.insert(QStringLiteral("log"), QJsonArray::fromStringList(api.logged()));
    if (value.isError() || stopped) {
        // What it was doing when it was stopped: the call under way (its
        // own code, when there is none), and the user's time told apart.
        QString message;
        if (stopped) {
            if (!api.running().isEmpty())
                message = tr("stopped after %1 s of its own, while its call %2 of %3 was running (a longer 'timeout' lets it run "
                             "longer; that call's own answer was not waited for)").arg(timeout).arg(api.calls()).arg(api.running());
            else if (api.calls() > 0)
                message = tr("stopped after %1 s of its own, between calls - after its call %2, of %3; its own code was running "
                             "then (a longer 'timeout' lets it run longer)").arg(timeout).arg(api.calls()).arg(api.last());
            else
                message = tr("stopped after %1 s of its own: its own code was running, and it had called no tool (a longer "
                             "'timeout' lets it run longer)").arg(timeout);
            if (userMs > 0)
                message += QLatin1Char(' ') + tr("The %1 s it waited for the user to answer a card are not counted in that.")
                                                  .arg(userMs / 1000.0, 0, 'f', 1);
        } else {
            message = value.property(QStringLiteral("message")).toString();
        }
        result.insert(QStringLiteral("error"), message);
        if (!stopped && value.hasProperty(QStringLiteral("lineNumber"))) result.insert(QStringLiteral("line"), value.property(QStringLiteral("lineNumber")).toInt());
        if (atomic) {
            for (const Kept& k : std::as_const(kept)) {
                if (!k.sch) continue;
                k.sch->restoreAll(k.state, false);
                k.sch->setUndoStacks(k.marks);
                k.sch->setChanged(k.changed, false);
            }
            result.insert(QStringLiteral("put back"), tr("every schematic is as it was before the script ('atomic')"));
        }
        return QJsonObject{{QStringLiteral("content"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                                                              {QStringLiteral("text"), QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact))}}}},
                           {QStringLiteral("isError"), true}};
    }
    if (!value.isUndefined()) result.insert(QStringLiteral("result"), QJsonValue::fromVariant(value.toVariant()));
    return jsonResult(result);
}

#include "qucscontrol_script.moc"

#else

QJsonObject QucsControl::runScript(const QJsonObject&)
{
    return errorResult(tr("run_script is not in this build of Qucs-S (it needs Qt's Qml module)."));
}

#endif
