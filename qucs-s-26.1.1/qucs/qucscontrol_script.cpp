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

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>

#ifdef QUCS_HAVE_QML
#include <QElapsedTimer>
#include <QJSEngine>
#include <QJSValue>
#include <QScopeGuard>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

#if defined(Q_OS_MACOS)
#include <mach/mach.h>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif
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

// The program's memory now, in bytes (0: not known here): what the system
// counts against it - on macOS its footprint, as Activity Monitor shows it.
qint64 footprint()
{
#if defined(Q_OS_MACOS)
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, task_info_t(&info), &count) == KERN_SUCCESS) return qint64(info.phys_footprint);
    return 0;
#elif defined(Q_OS_WIN)
    PROCESS_MEMORY_COUNTERS_EX counters;
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof counters))
        return qint64(counters.PrivateUsage);
    return 0;
#else
    long pages = 0, resident = 0;
    FILE* statm = std::fopen("/proc/self/statm", "r");
    if (statm == nullptr) return 0;
    const bool read = std::fscanf(statm, "%ld %ld", &pages, &resident) == 2;
    std::fclose(statm);
    return read ? qint64(resident) * sysconf(_SC_PAGESIZE) : 0;
#endif
}

// The most a script's own data may grow the program by: 4 GB, or a quarter
// of the machine's memory where that is less (QUCS_SCRIPT_MEMORY_MB sets it).
qint64 mostScriptMemory()
{
    bool given = false;
    const qint64 mb = qEnvironmentVariableIntValue("QUCS_SCRIPT_MEMORY_MB", &given);
    if (given && mb > 0) return mb * 1024 * 1024;
    qint64 most = qint64(4) * 1024 * 1024 * 1024;
#if defined(Q_OS_WIN)
    MEMORYSTATUSEX status;
    status.dwLength = sizeof status;
    if (GlobalMemoryStatusEx(&status)) most = std::min(most, qint64(status.ullTotalPhys / 4));
#else
    if (const long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGESIZE); pages > 0 && size > 0)
        most = std::min(most, qint64(pages) * size / 4);
#endif
    return std::max(most, qint64(256) * 1024 * 1024);
}

// Run before each script: where V4's own code recurses without its stack
// check, the same done in JavaScript, which has it - or bounded. A value
// nested 100,000 deep ended the program through Array.prototype.
// toLocaleString, a chain of 100,000 Proxies through a property read, and
// console.log of a deep array (the re-check of round four's fixes, 14
// November, J4); String(), join and sort stop at V4's limit already.
constexpr const char* kPrelude = R"js((function (global) {
  const join = Array.prototype.join, Too = RangeError, Real = Proxy, revocable = Proxy.revocable, depth = new WeakMap();
  Object.defineProperty(Array.prototype, 'toLocaleString', {writable: true, configurable: true, value: function toLocaleString() {
    const o = Object(this), n = o.length >>> 0, out = [];
    for (let i = 0; i < n; i++) { const v = o[i]; out.push(v === undefined || v === null ? '' : String(v.toLocaleString())); }
    return join.call(out, ',');
  }});
  const nested = (target, handler) => {
    const d = Math.max(depth.get(target) || 0, depth.get(handler) || 0) + 1;
    if (d > 1000) throw new Too('proxies nested more than 1,000 deep');
    return d;
  };
  const made = class Proxy {
    constructor(target, handler) { const d = nested(target, handler), p = new Real(target, handler); depth.set(p, d); return p; }
  };
  Object.defineProperty(made, 'revocable', {writable: true, configurable: true, value: function (target, handler) {
    const d = nested(target, handler), r = revocable(target, handler); depth.set(r.proxy, d); return r;
  }});
  Object.defineProperty(global, 'Proxy', {writable: true, configurable: true, value: made});
  const said = f => function () { const parts = []; for (let i = 0; i < arguments.length; i++) parts.push(String(arguments[i])); return f(join.call(parts, ' ')); };
  if (typeof console === 'object' && console)
    for (const k of ['log', 'debug', 'info', 'warn', 'error'])
      if (typeof console[k] === 'function') { const f = console[k].bind(console); console[k] = said(f); }
  if (typeof print === 'function') { const f = print; global.print = said(f); }
})(this))js";

// Scripts running now: one, at most - run_script is refused within one,
// called through batch too.
int scriptsRunning = 0;

// What a script sees: qucs.call(tool, args), qucs.log(text).
class ScriptApi : public QObject
{
    Q_OBJECT

public:
    ScriptApi(QucsControl* control, QJSEngine* engine)
        : a_control(control), a_engine(engine), a_stringify(engine->evaluate(QLatin1String(kStringify))), a_since(footprint())
    {
    }

    Q_INVOKABLE QJSValue call(const QString& tool, const QJSValue& args = QJSValue())
    {
        if (tool == QLatin1String("run_script")) return fail(QStringLiteral("run_script cannot run from a script"));
        // (A script runs alone: every other call would wait with it.)
        if (tool == QLatin1String("wait_for")) return fail(QStringLiteral("wait_for cannot run from a script, which every other call waits for"));
        if (++a_calls > kMostCalls) return fail(QStringLiteral("more than %1 calls: the script is stopped").arg(kMostCalls));
        QJsonObject arguments;
        if (!args.isUndefined() && !args.isNull()) {
            if (!args.isObject() || args.isArray()) return fail(QStringLiteral("%1's arguments are an object: qucs.call(\"%1\", {...})").arg(tool));
            QJsonValue made;
            QString why;
            if (!jsonOf(args, &made, &why)) return fail(QStringLiteral("%1's arguments are not JSON: %2").arg(tool, why));
            arguments = made.toObject();
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
        // (What the tool takes is not the script's own: its memory is
        // counted between its calls.)
        callStarts();
        QJsonObject result = a_control->callNow(tool, arguments, 10 * 60 * 1000);
        callEnds();
        a_running.clear();
        if (most > 0) result = qucs_s::mcp::trimmedTo(result, most);
        QStringList texts;
        for (const QJsonValue& v : result.value(QLatin1String("content")).toArray())
            if (v.toObject().value(QLatin1String("type")).toString() == QLatin1String("text"))
                texts << v.toObject().value(QLatin1String("text")).toString();
        if (result.value(QLatin1String("isError")).toBool()) {
            // (Its own name once: a tool's refusal names it already, and
            // "get_dataset: get_dataset: ..." came of saying it again.)
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
        QString line = value.toString();
        if (!value.isString() && value.isObject()) {
            QJsonValue made;
            QString why;
            if (!jsonOf(value, &made, &why)) line = QStringLiteral("(not JSON: %1)").arg(why);
            else if (made.isObject()) line = QString::fromUtf8(QJsonDocument(made.toObject()).toJson(QJsonDocument::Compact));
            else if (made.isArray()) line = QString::fromUtf8(QJsonDocument(made.toArray()).toJson(QJsonDocument::Compact));
        }
        a_log << (value.isString() ? line : line.left(4000));
    }

    /// \a value made JSON by the script's own engine (kStringify), read back
    /// by fromJson, which stops at 1,024 levels. Qt's QJSValue::toVariant
    /// recursed a level at a time without a limit: a value nested 100,000
    /// deep overflowed the stack and ended the program. False, with why, where it is not JSON.
    bool jsonOf(const QJSValue& value, QJsonValue* made, QString* why)
    {
        const QJSValue text = a_stringify.call({value});
        if (text.isError()) {
            *why = text.property(QStringLiteral("message")).toString();
            if (why->contains(QLatin1String("stack"), Qt::CaseInsensitive) || why->contains(QLatin1String("recursion"), Qt::CaseInsensitive))
                *why = QStringLiteral("it is nested too deep (%1)").arg(*why);
            return false;
        }
        if (!text.isString()) {   // undefined, a function
            *made = QJsonValue(QJsonValue::Undefined);
            return true;
        }
        QJsonParseError error;
        const QJsonDocument doc = QJsonDocument::fromJson("[" + text.toString().toUtf8() + "]", &error);
        if (error.error != QJsonParseError::NoError) {
            *why = error.error == QJsonParseError::DeepNesting ? QStringLiteral("it is nested deeper than 1,000 levels") : error.errorString();
            return false;
        }
        *made = doc.array().at(0);
        return true;
    }

    /// The script's own memory: how much it has grown the program by, its
    /// calls' tools left out - read by the watchdog's thread.
    qint64 ownMemory() const
    {
        std::lock_guard<std::mutex> lock(a_memory);
        return a_inCall ? a_grown : a_grown + footprint() - a_since;
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
    // JSON.stringify as it was before the script ran: it stops at V4's own
    // stack limit and on a cycle. Its replacer counts what it writes, and
    // stops it past a million values or 100 million characters - a value of
    // shared parts, {a: d, b: d} doubled 40 times, was written out for ever,
    // V4's own code not stopped by the watchdog's interrupt.
    static constexpr const char* kStringify =
        "(function () {\n"
        "  const stringify = JSON.stringify, Too = RangeError;\n"
        "  return function (value) {\n"
        "    let values = 0, chars = 0;\n"
        "    return stringify(value, function (key, v) {\n"
        "      chars += key.length + (typeof v === 'string' ? v.length : 0);\n"
        "      if (++values > 1000000) throw new Too('it has more than 1,000,000 values');\n"
        "      if (chars > 100000000) throw new Too('it is longer than 100,000,000 characters');\n"
        "      return v;\n"
        "    });\n"
        "  };\n"
        "})()";
    void callStarts()
    {
        const qint64 now = footprint();
        std::lock_guard<std::mutex> lock(a_memory);
        a_grown += now - a_since;
        a_inCall = true;
    }
    void callEnds()
    {
        const qint64 now = footprint();
        std::lock_guard<std::mutex> lock(a_memory);
        a_since = now;
        a_inCall = false;
    }
    QJSValue fail(const QString& message)
    {
        a_engine->throwError(QJSValue::GenericError, message);
        return QJSValue();
    }
    QucsControl* a_control;
    QJSEngine* a_engine;
    QJSValue a_stringify;   // (the engine's own, taken before the script could change it)
    mutable std::mutex a_memory;
    qint64 a_grown = 0, a_since = 0;   // grown by before the last call; the footprint after it
    bool a_inCall = false;
    int a_calls = 0;
    QString a_running, a_last;   // the call under way, and the last made
    QStringList a_log;
};

} // namespace

QJsonObject QucsControl::runScript(const QJsonObject& args)
{
    const QString script = args.value(QLatin1String("script")).toString();
    if (script.trimmed().isEmpty())
        return errorResult(tr("'script' is a JavaScript program: qucs.call(\"add_component\", {type: \"R\", x: 100, y: 100}) calls a tool."));
    if (scriptsRunning > 0)
        return errorResult(tr("run_script cannot run from a script, through batch neither: the script that called it is running "
                              "(its calls are made one by one with qucs.call)."));
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

    ++scriptsRunning;
    const auto ended = qScopeGuard([] { --scriptsRunning; });
    QJSEngine engine;
    engine.installExtensions(QJSEngine::ConsoleExtension);   // console.log too (to stderr)
    ScriptApi api(this, &engine);
    QJSValue qucs = engine.newQObject(&api);
    engine.globalObject().setProperty(QStringLiteral("qucs"), qucs);
    if (const QJSValue set = engine.evaluate(QLatin1String(kPrelude)); set.isError())
        qWarning("run_script's prelude: %s", qPrintable(set.toString()));
    // A script that runs too long is stopped (the engine is told from a
    // thread of its own: it runs on this one).
    std::mutex mutex;
    std::condition_variable over;
    bool finished = false, stopped = false;
    // A script that takes too much memory is stopped too, at the same
    // watch: V4's heap grew until the system refused it, 6.3 GB in 12 s
    const qint64 mostMemory = mostScriptMemory();
    qint64 tookMemory = 0;
    QElapsedTimer clock;
    clock.start();
    // Cancelled by its client (the user stopped Claude): stopped as at its
    // timeout, and the call of it under way with it (a solve, a run).
    const int stop = stoppable(running(), [&engine] { engine.setInterrupted(true); });
    const auto unstop = qScopeGuard([this, stop] { this->stopped(stop); });
    // The user's time is not the script's: while a call of it waits for an
    // answer to a card (askUser()), its clock stands still - the script was
    // "stopped after 60 s" where the user had taken that long to answer
    // Watched in slices for that reason.
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
            if (const qint64 took = api.ownMemory(); took > mostMemory) {
                stopped = true;
                tookMemory = took;
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
    // Its result made JSON while the watch is kept: a value of shared parts
    // can take long, or much, to write out.
    QJsonValue made(QJsonValue::Undefined);
    QString notJson;
    if (!value.isError() && !value.isUndefined() && !api.jsonOf(value, &made, &notJson))
        made = QJsonValue(QJsonValue::Undefined);
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
        if (stopped && tookMemory > 0) {
            message = tr("stopped after %1 s: its own data had grown the program by %2 MB, past the %3 MB a script may take (what "
                         "the tools it calls take is not counted)")
                          .arg((clock.elapsed() - userMs) / 1000.0, 0, 'f', 1).arg(tookMemory / (1024 * 1024)).arg(mostMemory / (1024 * 1024));
        } else if (stopped) {
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
        // (Its line, where it is one: V4's RangeError of a stack overflow
        // came with 1825783312.)
        if (const int line = value.property(QStringLiteral("lineNumber")).toInt();
            !stopped && value.hasProperty(QStringLiteral("lineNumber")) && line >= 1 && line <= script.count(QLatin1Char('\n')) + 1)
            result.insert(QStringLiteral("line"), line);
        if (atomic && QApplication::activeModalWidget() != nullptr) {
            // (Not under a dialog: one open over a schematic holds its parts,
            // which putting it back freed.)
            result.insert(QStringLiteral("put back"), tr("nothing: a dialog is open (%1), and putting a schematic back under it is not "
                                                         "safe - undo takes the script's changes back once it is closed")
                                                          .arg(QApplication::activeModalWidget()->windowTitle()));
        } else if (atomic) {
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
    if (!made.isUndefined()) result.insert(QStringLiteral("result"), made);
    // (It ran to its end, and what it changed stays: only its result is
    // not given.)
    if (!notJson.isEmpty())
        result.insert(QStringLiteral("result not given"), tr("the script ran to its end, but its result is not JSON: %1").arg(notJson));
    return jsonResult(result);
}

#include "qucscontrol_script.moc"

#else

QJsonObject QucsControl::runScript(const QJsonObject&)
{
    return errorResult(tr("run_script is not in this build of Qucs-S (it needs Qt's Qml module)."));
}

#endif
