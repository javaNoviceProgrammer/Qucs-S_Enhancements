/*
 * qucscontrol_design.cpp - QucsControl's design tools: Verilog-A compiled on
 *                          demand, a property tuned to a target, datasheets
 *                          read, the libraries searched by value, netlists
 *                          and symbols, projects, and the history of changes
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

#include "dataset.h"
#include "main.h"
#include "misc.h"
#include "projectView.h"
#include "qucs.h"
#include "schematic.h"
#include "simulationconsole.h"
#include "workspace.h"
#include "textdoc.h"
#include "vamodule.h"

#include "components/component.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#ifdef QUCS_HAVE_QTPDF
#include <QPdfDocument>
#include <QPdfSelection>
#endif

#include <cmath>
#include <functional>
#include <memory>

using namespace qucs_s::control;
using qucs_s::dataset::rounded;

namespace {

// OpenVAF's diagnostics: "error: message" (or warning), then
// "  --> file:line:column", then the source with marks.
QJsonArray diagnostics(const QString& output, const QString& severity)
{
    static const QRegularExpression head(QStringLiteral("^(error|warning)(?:\\[\\w+\\])?:\\s*(.*)$"));
    static const QRegularExpression where(QStringLiteral("^\\s*-->\\s*(.*):(\\d+):(\\d+)\\s*$"));
    QJsonArray list;
    const QStringList lines = output.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const QRegularExpressionMatch m = head.match(lines.at(i).trimmed());
        if (!m.hasMatch() || m.captured(1) != severity) continue;
        const QString message = m.captured(2).trimmed();
        if (message.startsWith(QLatin1String("could not compile"))) continue;   // the summary
        QJsonObject o{{QStringLiteral("message"), message}};
        if (i + 1 < lines.size()) {
            const QRegularExpressionMatch w = where.match(lines.at(i + 1));
            if (w.hasMatch()) {
                const QString file = w.captured(1);
                const int line = w.captured(2).toInt();
                o.insert(QStringLiteral("file"), QFileInfo(file).fileName());
                o.insert(QStringLiteral("line"), line);
                o.insert(QStringLiteral("column"), w.captured(3).toInt());
                // The source line it names, and what OpenVAF points at.
                QFile f(file);
                if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    const QStringList source = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
                    if (line >= 1 && line <= source.size()) o.insert(QStringLiteral("source"), source.at(line - 1).trimmed());
                }
                QStringList notes;
                for (int k = i + 2; k < lines.size() && !lines.at(k).trimmed().isEmpty(); ++k) {
                    // "  |     ^ unexpected token", "  |   - expected ';'"
                    static const QRegularExpression mark(QStringLiteral("^\\s*\\|\\s*[\\^\\-]+\\s*(.+)$"));
                    const QRegularExpressionMatch n = mark.match(lines.at(k));
                    if (n.hasMatch()) notes << n.captured(1).trimmed();
                }
                if (!notes.isEmpty()) o.insert(QStringLiteral("notes"), QJsonArray::fromStringList(notes));
            }
        }
        list.append(o);
    }
    return list;
}

} // namespace

// ----------------------------------------------------------------------
// Verilog-A compiled on demand

void QucsControl::buildVerilogA(const QJsonObject& args, const Done& done)
{
    QString file = args.value(QLatin1String("file")).toString().trimmed();
    if (file.isEmpty()) {
        // The .va document in front.
        if (a_app->DocumentTab->count() > 0) {
            QucsDoc* doc = a_app->getDoc();
            if (doc != nullptr && doc->getDocName().endsWith(QLatin1String(".va"), Qt::CaseInsensitive)) file = doc->getDocName();
        }
        if (file.isEmpty()) {
            done(errorResult(tr("Which file? 'file': a Verilog-A source (.va) - the document in front is none.")));
            return;
        }
    }
    file = absolute(file);
    if (!file.endsWith(QLatin1String(".va"), Qt::CaseInsensitive)) {
        done(errorResult(tr("%1 is not a Verilog-A source (.va).").arg(QFileInfo(file).fileName())));
        return;
    }
    if (!QFileInfo(file).isFile()) {
        done(errorResult(tr("There is no file %1.").arg(QDir::toNativeSeparators(file))));
        return;
    }
    // An open document with unsaved changes: saved first, or compiled as
    // it is on disk - as asked.
    const QString unsaved = args.value(QLatin1String("unsaved")).toString();
    for (QucsDoc* doc : a_app->allDocuments()) {
        if (!sameFile(doc->getDocName(), file) || !doc->getDocChanged()) continue;
        if (unsaved == QLatin1String("save")) {
            if (doc->save() < 0) {
                done(errorResult(tr("%1 could not be saved.").arg(QFileInfo(file).fileName())));
                return;
            }
        } else if (unsaved != QLatin1String("as_saved")) {
            done(errorResult(tr("%1 has unsaved changes: 'unsaved' says what to do - save (saved first, then compiled) or "
                                "as_saved (the file as it is on disk).").arg(QFileInfo(file).fileName())));
            return;
        }
    }
    const QString openVAF = QucsSettings.OpenVAFExecutable.trimmed();
    if (openVAF.isEmpty() || !QFileInfo(openVAF).isExecutable()) {
        done(errorResult(openVAF.isEmpty() ? tr("OpenVAF is not set: Application Settings > Locations > OpenVAF Path.")
                                           : tr("OpenVAF (%1) cannot be run: Application Settings > Locations > OpenVAF Path.")
                                                 .arg(QDir::toNativeSeparators(openVAF))));
        return;
    }
    const int timeout = std::clamp(args.value(QLatin1String("timeout")).toInt(120), 5, 3600) * 1000;
    auto* process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setWorkingDirectory(QFileInfo(file).absolutePath());
    const QString osdi = file.left(file.size() - 3) + QStringLiteral(".osdi");
    const QDateTime started = QDateTime::currentDateTime().addSecs(-1);
    auto answered = std::make_shared<bool>(false);
    QPointer<QProcess> guard(process);
    const auto report = [this, done, answered, guard, file, osdi, started](bool timedOut) {
        if (*answered) return;
        *answered = true;
        const QString output = guard ? QString::fromLocal8Bit(guard->readAll()) : QString();
        const int code = guard && guard->exitStatus() == QProcess::NormalExit ? guard->exitCode() : -1;
        const bool written = QFileInfo(osdi).isFile() && QFileInfo(osdi).lastModified() >= started;
        const bool compiled = !timedOut && code == 0 && written;
        QJsonObject result{{QStringLiteral("file"), QDir::toNativeSeparators(file)},
                           {QStringLiteral("compiled"), compiled},
                           {QStringLiteral("errors"), diagnostics(output, QStringLiteral("error"))},
                           {QStringLiteral("warnings"), diagnostics(output, QStringLiteral("warning"))}};
        QStringList lines = output.trimmed().split(QLatin1Char('\n'));
        if (lines.size() > 60) lines = lines.mid(lines.size() - 60);
        result.insert(QStringLiteral("output"), lines.join(QLatin1Char('\n')));
        if (timedOut) result.insert(QStringLiteral("note"), tr("OpenVAF did not finish in time; it was stopped."));
        else if (!compiled && code != 0 && result.value(QStringLiteral("errors")).toArray().isEmpty())
            result.insert(QStringLiteral("note"), code < 0 ? tr("OpenVAF crashed or did not start.") : tr("OpenVAF ended with exit code %1.").arg(code));
        if (compiled) {
            result.insert(QStringLiteral("osdi"), QDir::toNativeSeparators(osdi));
            // What it holds: its modules, each with its parameters counted.
            QStringList names;
            if (qucs_s::vamodule::osdiModules(osdi, &names)) {
                QJsonArray modules;
                for (const QString& name : std::as_const(names)) {
                    qucs_s::vamodule::VerilogModule module;
                    QJsonObject m{{QStringLiteral("module"), name}};
                    if (qucs_s::vamodule::readOsdi(osdi, name, &module)) m.insert(QStringLiteral("parameters"), int(module.parameters.size()));
                    modules.append(m);
                }
                result.insert(QStringLiteral("modules"), modules);
                result.insert(QStringLiteral("next"), tr("describe_component_type with the module's name lists its parameters and a .model card."));
            }
        }
        if (a_app->projectView() != nullptr) a_app->projectView()->refresh();   // the .osdi
        if (guard) guard->deleteLater();
        done(jsonResult(result));
    };
    connect(process, &QProcess::finished, this, [report] { report(false); });
    connect(process, &QProcess::errorOccurred, this, [report, guard](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) report(false);
    });
    QTimer::singleShot(timeout, this, [report, guard] {
        if (guard && guard->state() != QProcess::NotRunning) {
            guard->kill();
            guard->waitForFinished(2000);
            report(true);
        }
    });
    process->start(openVAF, {file});
}

// ----------------------------------------------------------------------
// A property tuned to a target: set, simulate, measure, again

namespace {

// A value given as a number or as text with a unit ("4.7k", "10 nF").
bool valueOf(const QJsonValue& v, double* out)
{
    if (v.isDouble()) {
        *out = v.toDouble();
        return std::isfinite(*out);
    }
    if (!v.isString() || v.toString().trimmed().isEmpty()) return false;
    double number = 0, factor = 1;
    QString unit;
    misc::str2num(v.toString(), number, unit, factor);
    *out = number * factor;
    return std::isfinite(*out) && !v.toString().trimmed().startsWith(QLatin1Char('{'));
}

// The unit of a property's value without its prefix: "Ohm" of "1 kOhm".
QString baseUnit(const QString& value)
{
    double number = 0, factor = 1;
    QString unit;
    misc::str2num(value, number, unit, factor);
    if (unit.isEmpty()) return {};
    if (factor != 1.0 || unit.startsWith(QLatin1Char('d'))) unit = unit.mid(1);
    return unit.trimmed();
}

QString valueText(double value, const QString& unit)
{
    QString text = misc::num2str(value, 6);
    if (!unit.isEmpty()) text += QLatin1Char(' ') + unit;
    return text;
}

// The number a measurement spec takes from get_dataset's answer (one
// variable), or from simulate's operating point; NaN and why when none.
double measured(const QJsonObject& spec, const QJsonObject& answer, QString* used, QString* why)
{
    const QString op = spec.value(QLatin1String("operating_point")).toString().trimmed();
    if (!op.isEmpty()) {
        const QJsonObject point = answer.value(QStringLiteral("operating point")).toObject();
        // A node ("e", "v(e)") or a device's quantity ("Q1.ic").
        const QJsonObject nodes = point.value(QStringLiteral("nodes")).toObject();
        const QString bare = op.startsWith(QLatin1String("v("), Qt::CaseInsensitive) && op.endsWith(QLatin1Char(')')) ? op.mid(2, op.size() - 3) : op;
        for (auto it = nodes.begin(); it != nodes.end(); ++it)
            if (it.key().compare(op, Qt::CaseInsensitive) == 0 || it.key().compare(bare, Qt::CaseInsensitive) == 0
                || it.key().compare(QStringLiteral("v(%1)").arg(bare), Qt::CaseInsensitive) == 0) {
                *used = it.key();
                return it.value().toDouble();
            }
        const int dot = int(op.lastIndexOf(QLatin1Char('.')));
        if (dot > 0) {
            const QString device = op.left(dot), quantity = op.mid(dot + 1);
            for (const QJsonValue& d : point.value(QStringLiteral("devices")).toArray()) {
                const QJsonObject o = d.toObject();
                if (o.value(QStringLiteral("component")).toString().compare(device, Qt::CaseInsensitive) != 0
                    && o.value(QStringLiteral("device")).toString().compare(device, Qt::CaseInsensitive) != 0)
                    continue;
                for (const char* group : {"values", "derived"}) {
                    const QJsonObject values = o.value(QLatin1String(group)).toObject();
                    for (auto it = values.begin(); it != values.end(); ++it)
                        if (it.key().compare(quantity, Qt::CaseInsensitive) == 0 || it.key().startsWith(quantity + QLatin1Char(' '))) {
                            *used = device + QLatin1Char('.') + it.key();
                            return it.value().toDouble();
                        }
                }
            }
        }
        QStringList have = nodes.keys();
        *why = tr("the operating point has no %1 (its nodes: %2)").arg(op, have.mid(0, 20).join(QStringLiteral(", ")));
        return NAN;
    }
    const QJsonArray variables = answer.value(QStringLiteral("variables")).toArray();
    if (variables.isEmpty()) {
        *why = tr("no variable was read");
        return NAN;
    }
    QJsonObject v = variables.first().toObject();
    *used = v.value(QStringLiteral("name")).toString();
    if (v.contains(QStringLiteral("curves"))) v = v.value(QStringLiteral("curves")).toArray().first().toObject();   // a family: the first curve
    const QString what = spec.value(QLatin1String("what")).toString(QStringLiteral("final")).trimmed().toLower().replace(QLatin1Char('_'), QLatin1Char(' '));
    if (spec.contains(QLatin1String("at"))) {
        const QJsonArray row = v.value(QStringLiteral("at")).toArray().first().toArray();
        if (row.size() < 2 || row.at(1).isNull()) {
            *why = tr("%1 has no value at %2").arg(*used).arg(spec.value(QLatin1String("at")).toDouble());
            return NAN;
        }
        return row.at(1).toDouble();
    }
    static const QStringList stats{QStringLiteral("min"), QStringLiteral("max"), QStringLiteral("mean"), QStringLiteral("rms"),
                                   QStringLiteral("initial"), QStringLiteral("final"), QStringLiteral("peak to peak")};
    if (stats.contains(what)) {
        if (!v.contains(what)) {
            *why = tr("%1 has no points in the range").arg(*used);
            return NAN;
        }
        return v.value(what).toDouble();
    }
    const QJsonObject m = v.value(QStringLiteral("measurements")).toObject().value(what.contains(QLatin1Char(' ')) ? QString(what).replace(QLatin1Char(' '), QLatin1Char('_')) : what).toObject();
    const QString field = spec.value(QLatin1String("field")).toString(QStringLiteral("value"));
    const QJsonValue value = m.value(field);
    if (!value.isDouble()) {
        *why = m.contains(QStringLiteral("note")) ? m.value(QStringLiteral("note")).toString()
                                                  : m.contains(QStringLiteral("error")) ? m.value(QStringLiteral("error")).toString()
                                                                                         : tr("%1 of %2 gave no %3").arg(what, *used, field);
        return NAN;
    }
    return value.toDouble();
}

} // namespace

void QucsControl::tune(const QJsonObject& args, const Done& done)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) {
        done(errorResult(error));
        return;
    }
    const QString name = args.value(QLatin1String("component")).toString().trimmed();
    Component* c = sch->getComponentByName(name);
    if (c == nullptr) {
        done(errorResult(tr("There is no component %1 in %2 ('component': the part whose property is tuned; an equation "
                            "block for a parameter it defines).").arg(name, titleOf(sch))));
        return;
    }
    QString property = args.value(QLatin1String("property")).toString().trimmed();
    if (property.isEmpty() && !c->Props.isEmpty()) property = c->Props.first()->Name;
    Property* p = c->getProperty(property);
    if (p == nullptr) {
        QStringList names;
        for (Property* q : c->Props) names << q->Name;
        done(errorResult(tr("%1 has no property %2; its properties are: %3.").arg(name, property, names.join(QStringLiteral(", ")))));
        return;
    }
    // What is measured after each run.
    const QJsonObject spec = args.value(QLatin1String("measure")).toObject();
    const bool atOperatingPoint = spec.contains(QLatin1String("operating_point"));
    if (!atOperatingPoint && spec.value(QLatin1String("variable")).toString().trimmed().isEmpty()) {
        done(errorResult(tr("'measure' says what is measured after each run: {\"variable\": \"tran.v(out)\", \"what\": \"final\"} - "
                            "what: min, max, mean, rms, initial, final, peak_to_peak, or a measurement of get_dataset's (bandwidth, "
                            "overshoot, rise_time, gain, thd, ...: its value, or 'field'); 'at' a value at an x; 'from', 'to' the "
                            "range - or {\"operating_point\": \"e\"} (a node's DC voltage, or Q1.ic).")));
        return;
    }
    // The values tried: a list, or a range searched for the target.
    QList<double> values;
    for (const QJsonValue& v : args.value(QLatin1String("values")).toArray()) {
        double x = 0;
        if (!valueOf(v, &x)) {
            done(errorResult(tr("%1 is not a value ('values': numbers, or text with units: 4.7k).").arg(QString::fromUtf8(QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact)))));
            return;
        }
        values << x;
    }
    const bool hasTarget = args.value(QLatin1String("target")).isDouble();
    const double target = args.value(QLatin1String("target")).toDouble();
    double lo = 0, hi = 0;
    const QJsonArray range = args.value(QLatin1String("range")).toArray();
    if (values.isEmpty()) {
        if (!hasTarget || range.size() != 2 || !valueOf(range.at(0), &lo) || !valueOf(range.at(1), &hi) || lo == hi) {
            done(errorResult(tr("Give 'target' (the number the measurement is to come to) and 'range' ([low, high] of the "
                                "property, numbers or 4.7k) - or 'values', a list of values each simulated and measured.")));
            return;
        }
        if (lo > hi) std::swap(lo, hi);
    } else if (values.size() > 40) {
        done(errorResult(tr("'values' is 40 values at most (each is a simulation).")));
        return;
    }
    const int most = std::clamp(args.value(QLatin1String("max_runs")).toInt(12), 2, 40);
    const double tolerance = args.value(QLatin1String("tolerance")).isDouble() ? std::abs(args.value(QLatin1String("tolerance")).toDouble())
                                                                               : std::max(1e-12, std::abs(target) * 0.005);
    const bool logScale = lo > 0 && hi / lo >= 10;
    const bool apply = args.value(QLatin1String("apply")).toBool(true);
    const QString was = p->Value;
    const QString unit = baseUnit(was);
    const int timeout = std::clamp(args.value(QLatin1String("timeout")).toInt(120), 5, 3600);

    struct State {
        QList<std::pair<double, double>> runs;   // value, measured
        QString used;                            // the variable measured
        QStringList notes;
        double a = 0, fa = NAN, b = 0, fb = NAN; // the bracket, as values measured less the target
        int side = 0;                            // (Illinois: the end kept twice)
        int next = 0;                            // the index of 'values' to try next
    };
    // Untitled: saved in the scratch folder first (each run simulates the
    // file's schematic), and said.
    QString savedNote;
    if (sch->getDocName().isEmpty() && !saveInScratch(sch, &savedNote, &error)) {
        done(errorResult(error));
        return;
    }
    auto state = std::make_shared<State>();
    QPointer<Schematic> doc(sch);
    const QString path = sch->getDocName();

    // Sets the property (no step to undo: only the value found is one).
    const auto setValue = [doc, name, property](const QString& text) {
        if (!doc) return;
        Component* comp = doc->getComponentByName(name);
        if (comp == nullptr || comp->getProperty(property) == nullptr) return;
        comp->getProperty(property)->Value = text;
        doc->recreateComponent(comp);
        doc->viewport()->update();
    };
    auto finishUp = std::make_shared<std::function<void(const QString&)>>();
    auto evaluate = std::make_shared<std::function<void(double, std::function<void(double)>)>>();
    *evaluate = [=, this](double x, std::function<void(double)> then) {
        if (!doc) {
            (*finishUp)(tr("The schematic was closed."));
            return;
        }
        setValue(valueText(x, unit));
        QJsonObject run{{QStringLiteral("path"), path}, {QStringLiteral("timeout"), timeout}};
        if (atOperatingPoint) run.insert(QStringLiteral("operating_point"), true);
        if (args.contains(QLatin1String("simulator"))) run.insert(QStringLiteral("simulator"), args.value(QLatin1String("simulator")));
        simulate(run, [=, this](const QJsonObject& r) {
            const QJsonObject result = QJsonDocument::fromJson(textOf(r).section(QLatin1Char('\n'), -1).toUtf8()).object();
            QJsonObject answer = result;
            if (r.value(QStringLiteral("isError")).toBool() || !result.value(QStringLiteral("succeeded")).toBool(true)
                || result.isEmpty()) {
                const QJsonArray errors = result.value(QStringLiteral("errors")).toArray();
                (*finishUp)(tr("The simulation with %1 = %2 failed: %3").arg(property, valueText(x, unit),
                                                                               errors.isEmpty() ? textOf(r).left(600)
                                                                                                : errors.first().toObject().value(QStringLiteral("message")).toString()));
                return;
            }
            if (!atOperatingPoint) {
                QJsonObject read{{QStringLiteral("path"), path},
                                 {QStringLiteral("variables"), QJsonArray{spec.value(QLatin1String("variable"))}},
                                 {QStringLiteral("points"), 0}};
                for (const char* key : {"from", "to", "level", "tolerance", "fundamental", "harmonics", "periods", "decibels", "form", "simulator"})
                    if (spec.contains(QLatin1String(key))) read.insert(QLatin1String(key), spec.value(QLatin1String(key)));
                if (args.contains(QLatin1String("simulator")) && !read.contains(QStringLiteral("simulator")))
                    read.insert(QStringLiteral("simulator"), args.value(QLatin1String("simulator")));
                if (spec.contains(QLatin1String("at"))) read.insert(QStringLiteral("at"), QJsonArray{spec.value(QLatin1String("at"))});
                const QString what = spec.value(QLatin1String("what")).toString(QStringLiteral("final")).trimmed().toLower();
                static const QStringList stats{QStringLiteral("min"), QStringLiteral("max"), QStringLiteral("mean"), QStringLiteral("rms"),
                                               QStringLiteral("initial"), QStringLiteral("final"), QStringLiteral("peak_to_peak"),
                                               QStringLiteral("peak to peak")};
                if (!spec.contains(QLatin1String("at")) && !stats.contains(what)) read.insert(QStringLiteral("measure"), QJsonArray{what});
                const QJsonObject got = getDataset(read);
                if (got.value(QStringLiteral("isError")).toBool()) {
                    (*finishUp)(tr("The result could not be read: %1").arg(textOf(got)));
                    return;
                }
                answer = QJsonDocument::fromJson(textOf(got).toUtf8()).object();
            }
            QString why, used;
            const double m = measured(spec, answer, &used, &why);
            if (std::isnan(m)) {
                (*finishUp)(tr("With %1 = %2 nothing could be measured: %3.").arg(property, valueText(x, unit), why));
                return;
            }
            if (state->used.isEmpty()) state->used = used;
            state->runs.append({x, m});
            then(m);
        });
    };

    // The search.
    auto step = std::make_shared<std::function<void()>>();
    *step = [=]() {
        if (!values.isEmpty()) {
            if (state->next >= values.size()) {
                (*finishUp)(QString());
                return;
            }
            const double x = values.at(state->next++);
            (*evaluate)(x, [step](double) { (*step)(); });
            return;
        }
        if (int(state->runs.size()) >= most) {
            (*finishUp)(tr("It did not come within the tolerance in %1 runs (max_runs).").arg(most));
            return;
        }
        // Within the tolerance: done.
        if (!state->runs.isEmpty() && std::abs(state->runs.last().second - target) <= tolerance) {
            (*finishUp)(QString());
            return;
        }
        if (std::isnan(state->fa)) {
            (*evaluate)(lo, [=](double m) {
                state->a = lo;
                state->fa = m - target;
                (*step)();
            });
            return;
        }
        if (std::isnan(state->fb)) {
            (*evaluate)(hi, [=](double m) {
                state->b = hi;
                state->fb = m - target;
                if ((state->fa > 0) == (state->fb > 0) && std::abs(state->fa) > tolerance && std::abs(state->fb) > tolerance) {
                    (*finishUp)(tr("The target is not between what the range's ends give (%1 at %2, %3 at %4): widen the range, or "
                                   "look at 'runs' for the way it goes.")
                                    .arg(state->fa + target).arg(valueText(lo, unit)).arg(state->fb + target).arg(valueText(hi, unit)));
                    return;
                }
                (*step)();
            });
            return;
        }
        // False position with the Illinois rule, on a log scale over decades.
        const auto u = [logScale](double x) { return logScale ? std::log(x) : x; };
        const auto back = [logScale](double t) { return logScale ? std::exp(t) : t; };
        const double fa = state->fa, fb = state->fb;
        double t = (u(state->a) * fb - u(state->b) * fa) / (fb - fa);
        if (!std::isfinite(t) || t <= std::min(u(state->a), u(state->b)) || t >= std::max(u(state->a), u(state->b)))
            t = (u(state->a) + u(state->b)) / 2;
        const double x = back(t);
        (*evaluate)(x, [=](double m) {
            const double fx = m - target;
            // The end on its side is replaced; the other, kept twice in a
            // row, counts half (so it does not hold the search back).
            if ((fx > 0) == (state->fb > 0)) {
                state->b = x;
                state->fb = fx;
                if (state->side == -1) state->fa /= 2;
                state->side = -1;
            } else {
                state->a = x;
                state->fa = fx;
                if (state->side == 1) state->fb /= 2;
                state->side = 1;
            }
            (*step)();
        });
    };

    *finishUp = [=, this](const QString& stopped) {
        if (!doc) {
            done(errorResult(tr("The schematic was closed while it was tuned.")));
            return;
        }
        // The best value: the one closest to the target (or, for a list
        // without one, none - the table is the answer).
        int best = -1;
        for (int i = 0; i < state->runs.size(); ++i)
            if (hasTarget && (best < 0 || std::abs(state->runs.at(i).second - target) < std::abs(state->runs.at(best).second - target))) best = i;
        QJsonArray runs;
        for (const auto& [x, m] : state->runs)
            runs.append(QJsonObject{{QStringLiteral("value"), valueText(x, unit)}, {QStringLiteral("measured"), rounded(m)}});
        QJsonObject result{{QStringLiteral("component"), name},
                           {QStringLiteral("property"), property},
                           {QStringLiteral("was"), was},
                           {QStringLiteral("measured"), state->used.isEmpty() ? spec.value(QLatin1String("variable")).toString() : state->used},
                           {QStringLiteral("runs"), runs}};
        if (hasTarget) result.insert(QStringLiteral("target"), target);
        if (!savedNote.isEmpty()) result.insert(QStringLiteral("saved"), savedNote);
        // Back to where it was; the value found is one step to undo.
        setValue(was);
        const bool applying = apply && best >= 0;
        if (applying) {
            const auto& [x, m] = state->runs.at(best);
            setValue(valueText(x, unit));
            doc->setChanged(true, true);
            result.insert(QStringLiteral("value"), valueText(x, unit));
            result.insert(QStringLiteral("gives"), rounded(m));
            result.insert(QStringLiteral("off by"), rounded(m - target));
            result.insert(QStringLiteral("within tolerance"), std::abs(m - target) <= tolerance);
            result.insert(QStringLiteral("set"), tr("%1 of %2 is %3 now: one step to undo (it was %4).").arg(property, name, valueText(x, unit), was));
        } else if (best >= 0) {
            const auto& [x, m] = state->runs.at(best);
            result.insert(QStringLiteral("value"), valueText(x, unit));
            result.insert(QStringLiteral("gives"), rounded(m));
            result.insert(QStringLiteral("set"), tr("not set ('apply' false): %1 is %2 as it was.").arg(property, was));
        } else {
            result.insert(QStringLiteral("set"), tr("%1 is %2 as it was.").arg(property, was));
        }
        if (!stopped.isEmpty()) result.insert(QStringLiteral("stopped"), stopped);
        // The datasets and diagrams: of the value set (a run again when the
        // last one was of another).
        const bool rerun = applying && !state->runs.isEmpty() && state->runs.last().first != state->runs.at(best).first;
        if (!rerun || !doc) {
            if (!state->runs.isEmpty() && !applying && !values.isEmpty())
                result.insert(QStringLiteral("note"), tr("The dataset is the last run's (%1 = %2).").arg(property, valueText(state->runs.last().first, unit)));
            done(jsonResult(result));
            return;
        }
        QJsonObject run{{QStringLiteral("path"), path}, {QStringLiteral("timeout"), timeout}};
        if (atOperatingPoint) run.insert(QStringLiteral("operating_point"), true);
        if (args.contains(QLatin1String("simulator"))) run.insert(QStringLiteral("simulator"), args.value(QLatin1String("simulator")));
        simulate(run, [done, result](const QJsonObject&) mutable {
            result.insert(QStringLiteral("dataset"), tr("of the value set (simulated again)"));
            done(jsonResult(result));
        });
    };
    (*step)();
}

// ----------------------------------------------------------------------
// A datasheet read

QJsonObject QucsControl::readPdf(const QJsonObject& args)
{
#ifdef QUCS_HAVE_QTPDF
    QString file = args.value(QLatin1String("path")).toString().trimmed();
    if (file.isEmpty()) {
        QucsDoc* doc = a_app->DocumentTab->count() > 0 ? a_app->getDoc() : nullptr;
        if (doc != nullptr && doc->getDocName().endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive)) file = doc->getDocName();
        if (file.isEmpty()) return errorResult(tr("Which PDF? 'path' (the document in front is none)."));
    }
    file = absolute(file);
    if (!QFileInfo(file).isFile()) return errorResult(tr("There is no file %1.").arg(QDir::toNativeSeparators(file)));
    QPdfDocument pdf;
    if (pdf.load(file) != QPdfDocument::Error::None || pdf.status() != QPdfDocument::Status::Ready)
        return errorResult(tr("%1 cannot be read as a PDF (locked, or not a PDF).").arg(QFileInfo(file).fileName()));
    const int count = pdf.pageCount();
    // The pages: [3, 4], "2-5", 7 - the first 3 unless given (or all, to search).
    QList<int> pages;
    const QString search = args.value(QLatin1String("search")).toString().trimmed();
    const QJsonValue p = args.value(QLatin1String("pages"));
    const auto add = [&pages, count](int page) {
        if (page >= 1 && page <= count && !pages.contains(page) && pages.size() < 200) pages << page;
    };
    if (p.isDouble()) add(p.toInt());
    else if (p.isArray())
        for (const QJsonValue& v : p.toArray()) add(v.toInt());
    else if (p.isString()) {
        for (const QString& part : p.toString().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QStringList ends = part.split(QLatin1Char('-'));
            const int a = ends.value(0).trimmed().toInt(), b = ends.size() > 1 ? ends.at(1).trimmed().toInt() : a;
            for (int k = a; k <= b && k - a < 200; ++k) add(k);
        }
    } else if (!search.isEmpty()) {
        for (int k = 1; k <= count; ++k) add(k);
    } else {
        for (int k = 1; k <= std::min(3, count); ++k) add(k);
    }
    if (pages.isEmpty()) return errorResult(tr("%1 has pages 1 to %2.").arg(QFileInfo(file).fileName()).arg(count));
    QJsonObject result{{QStringLiteral("file"), QDir::toNativeSeparators(file)}, {QStringLiteral("pages in all"), count}};
    const QString title = pdf.metaData(QPdfDocument::MetaDataField::Title).toString().trimmed();
    if (!title.isEmpty()) result.insert(QStringLiteral("title"), title);
    constexpr int kMost = 40000;
    int size = 0;
    QJsonArray read, found;
    QStringList skipped;
    for (int page : std::as_const(pages)) {
        const QString text = pdf.getAllText(page - 1).text();
        if (!search.isEmpty()) {
            // The lines that have it, with the one before and after.
            const QStringList lines = text.split(QLatin1Char('\n'));
            for (int i = 0; i < lines.size() && found.size() < 60; ++i)
                if (lines.at(i).contains(search, Qt::CaseInsensitive))
                    found.append(QJsonObject{{QStringLiteral("page"), page},
                                             {QStringLiteral("lines"), QStringList(lines.mid(std::max(0, i - 1), 3)).join(QLatin1Char('\n'))}});
            continue;
        }
        if (size + text.size() > kMost) {
            skipped << QString::number(page);
            continue;
        }
        size += int(text.size());
        read.append(QJsonObject{{QStringLiteral("page"), page}, {QStringLiteral("text"), text}});
    }
    if (!search.isEmpty()) {
        result.insert(QStringLiteral("search"), search);
        result.insert(QStringLiteral("found"), found);
        if (found.isEmpty()) result.insert(QStringLiteral("note"), tr("Not found on the pages looked at (a scanned page has no text)."));
    } else {
        result.insert(QStringLiteral("pages"), read);
        if (!skipped.isEmpty()) result.insert(QStringLiteral("left out"), tr("pages %1: too much text at once - ask for them").arg(skipped.join(QStringLiteral(", "))));
        bool empty = true;
        for (const QJsonValue& v : std::as_const(read)) empty = empty && v.toObject().value(QStringLiteral("text")).toString().trimmed().isEmpty();
        if (empty) result.insert(QStringLiteral("note"), tr("The pages have no text (scanned): screenshot of the PDF's tab shows them."));
    }
    return jsonResult(result);
#else
    Q_UNUSED(args)
    return errorResult(tr("This build of Qucs-S has no PDF reader (Qt PDF)."));
#endif
}

// ----------------------------------------------------------------------
// The libraries searched by value

namespace {

struct Found {
    QJsonObject json;
    double score = 0;
};

// How far \a value is from \a target: on a log scale when both are above 0.
double distance(double value, double target)
{
    if (value > 0 && target > 0) return std::abs(std::log(value / target));
    return std::abs(value - target) / std::max(1e-30, std::abs(target));
}

bool numberIn(const QString& text, double* x)
{
    double number = 0, factor = 1;
    QString unit;
    const QString t = text.trimmed();
    if (t.isEmpty() || !(t.at(0).isDigit() || t.at(0) == QLatin1Char('-') || t.at(0) == QLatin1Char('+') || t.at(0) == QLatin1Char('.'))) return false;
    misc::str2num(t, number, unit, factor);
    // SPICE: meg is mega, m milli.
    if (unit.startsWith(QLatin1String("meg"), Qt::CaseInsensitive)) factor = 1e6;
    *x = number * factor;
    return std::isfinite(*x);
}

} // namespace

QJsonObject QucsControl::findLibraryComponent(const QJsonObject& args)
{
    const QString search = args.value(QLatin1String("search")).toString().trimmed();
    const QStringList words = search.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    QString kind = args.value(QLatin1String("type")).toString().trimmed().toLower();
    const QJsonObject near = args.value(QLatin1String("near")).toObject();
    const QString onlyLibrary = args.value(QLatin1String("library")).toString().trimmed();
    const int limit = std::clamp(args.value(QLatin1String("limit")).toInt(15), 1, 100);
    if (words.isEmpty() && kind.isEmpty() && near.isEmpty())
        return errorResult(tr("Say what to look for: 'search' (words in the name or description: 2N3904, NPN 40V), 'type' "
                              "(npn, pnp, nmos, pmos, njf, pjf, diode, or a model: _BJT, _MOSFET, Diode, ...), 'near' values of "
                              "its parameters ({\"Bf\": 200}). A plain resistor, capacitor or inductor is add_component R, C or L "
                              "with its value."));
    QHash<QString, double> targets;
    for (auto it = near.begin(); it != near.end(); ++it) {
        double x = 0;
        if (it.value().isDouble()) x = it.value().toDouble();
        else if (!numberIn(it.value().toString(), &x))
            return errorResult(tr("'near': %1 takes a number (or text with units).").arg(it.key()));
        targets.insert(it.key(), x);
    }
    // A kind by its common name: the Qucs model and its Type, the SPICE type.
    static const QHash<QString, std::tuple<QString, QString, QString>> kinds{
        {QStringLiteral("npn"), {QStringLiteral("_BJT"), QStringLiteral("npn"), QStringLiteral("NPN")}},
        {QStringLiteral("pnp"), {QStringLiteral("_BJT"), QStringLiteral("pnp"), QStringLiteral("PNP")}},
        {QStringLiteral("nmos"), {QStringLiteral("_MOSFET"), QStringLiteral("nfet"), QStringLiteral("NMOS")}},
        {QStringLiteral("pmos"), {QStringLiteral("_MOSFET"), QStringLiteral("pfet"), QStringLiteral("PMOS")}},
        {QStringLiteral("njf"), {QStringLiteral("JFET"), QStringLiteral("nfet"), QStringLiteral("NJF")}},
        {QStringLiteral("pjf"), {QStringLiteral("JFET"), QStringLiteral("pfet"), QStringLiteral("PJF")}},
        {QStringLiteral("diode"), {QStringLiteral("Diode"), QString(), QStringLiteral("D")}}};
    QString model, modelType, spiceType;
    if (kinds.contains(kind)) std::tie(model, modelType, spiceType) = kinds.value(kind);
    else if (!kind.isEmpty()) model = args.value(QLatin1String("type")).toString().trimmed();

    QList<Found> found;
    int looked = 0;
    QHash<QString, QStringList> propertyNames;   // by model
    const auto namesOf = [&propertyNames](const QString& type) {
        if (!propertyNames.contains(type)) {
            QStringList names;
            if (std::unique_ptr<Component> c{newComponent(type)})
                for (Property* p : c->Props) names << p->Name;
            propertyNames.insert(type, names);
        }
        return propertyNames.value(type);
    };
    const auto wordsIn = [&words](const QString& text) {
        for (const QString& w : words)
            if (!text.contains(w, Qt::CaseInsensitive)) return false;
        return true;
    };

    // Qucs libraries: the installed ones and the user's (user_lib).
    QStringList libraryDirs{QucsSettings.LibDir, QucsSettings.qucsWorkspaceDir.filePath(QStringLiteral("user_lib"))};
    for (const QString& dirName : std::as_const(libraryDirs)) {
        const QDir dir(dirName);
        for (const QFileInfo& fi : dir.entryInfoList({QStringLiteral("*.lib")}, QDir::Files, QDir::Name)) {
            const QString library = fi.completeBaseName();
            if (!onlyLibrary.isEmpty() && library.compare(onlyLibrary, Qt::CaseInsensitive) != 0) continue;
            QFile f(fi.filePath());
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
            const QString text = QString::fromUtf8(f.readAll());
            static const QRegularExpression block(QStringLiteral("<Component\\s+([^>]+)>(.*?)</Component>"),
                                                  QRegularExpression::DotMatchesEverythingOption);
            for (auto it = block.globalMatch(text); it.hasNext();) {
                const QRegularExpressionMatch m = it.next();
                ++looked;
                const QString name = m.captured(1).trimmed(), body = m.captured(2);
                const QString description = body.section(QStringLiteral("<Description>"), 1).section(QStringLiteral("</Description>"), 0, 0).trimmed();
                const QString modelLine = body.section(QStringLiteral("<Model>"), 1).section(QStringLiteral("</Model>"), 0, 0).trimmed();
                const QString type = modelLine.mid(1).section(QLatin1Char(' '), 0, 0);
                if (!wordsIn(name + QLatin1Char(' ') + description + QLatin1Char(' ') + library)) continue;
                if (!model.isEmpty() && type.compare(model, Qt::CaseInsensitive) != 0) continue;
                // Its values, in the order of its type's properties.
                QStringList values;
                static const QRegularExpression quoted(QStringLiteral("\"([^\"]*)\""));
                for (auto q = quoted.globalMatch(modelLine); q.hasNext();) values << q.next().captured(1);
                const QStringList names = namesOf(type);
                QJsonObject shown;
                if (!modelType.isEmpty()) {
                    const int t = int(names.indexOf(QStringLiteral("Type")));
                    if (t < 0 || values.value(t) != modelType) continue;
                }
                double score = 0;
                bool all = true;
                for (auto t = targets.cbegin(); t != targets.cend() && all; ++t) {
                    int i = -1;
                    for (int k = 0; k < names.size(); ++k)
                        if (names.at(k).compare(t.key(), Qt::CaseInsensitive) == 0) i = k;
                    double x = 0;
                    if (i < 0 || !numberIn(values.value(i), &x)) all = false;
                    else {
                        score += distance(x, t.value());
                        shown.insert(names.at(i), values.value(i));
                    }
                }
                if (!all) continue;
                if (const int t = int(names.indexOf(QStringLiteral("Type"))); t >= 0) shown.insert(QStringLiteral("Type"), values.value(t));
                QJsonObject o{{QStringLiteral("library"), library},
                              {QStringLiteral("component"), name},
                              {QStringLiteral("model"), type},
                              {QStringLiteral("description"), description.section(QLatin1Char('\n'), 0, 1).simplified()},
                              {QStringLiteral("place"), QJsonObject{{QStringLiteral("type"), QStringLiteral("Lib")},
                                                                    {QStringLiteral("properties"), QJsonObject{{QStringLiteral("Lib"), library},
                                                                                                               {QStringLiteral("Comp"), name}}}}}};
                if (!shown.isEmpty()) o.insert(QStringLiteral("values"), shown);
                found.append(Found{o, score});
            }
        }
    }

    // SPICE models (.model cards) in the project's, the workspace's and
    // the installed SPICE libraries.
    QStringList spiceRoots{QucsSettings.QucsWorkDir.absolutePath()};
    if (!sameFile(spiceRoots.first(), QucsSettings.qucsWorkspaceDir.absolutePath())) spiceRoots << QucsSettings.qucsWorkspaceDir.absolutePath();
    if (!QucsSettings.SpiceLibDir.isEmpty()) spiceRoots << QucsSettings.SpiceLibDir;
    int files = 0;
    for (const QString& root : std::as_const(spiceRoots)) {
        if (!onlyLibrary.isEmpty()) break;
        for (const QString& rel : misc::projectFiles(QDir(root), {QStringLiteral("*.lib"), QStringLiteral("*.mod"), QStringLiteral("*.inc"),
                                                                  QStringLiteral("*.cir"), QStringLiteral("*.sp"), QStringLiteral("*.spi")})) {
            if (++files > 600) break;
            const QString path = QDir(root).filePath(rel);
            if (QFileInfo(path).size() > 4 * 1024 * 1024) continue;
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
            const QStringList lines = QString::fromLatin1(f.readAll()).split(QLatin1Char('\n'));
            for (int i = 0; i < lines.size(); ++i) {
                QString line = lines.at(i).trimmed();
                if (!line.startsWith(QLatin1String(".model"), Qt::CaseInsensitive)) continue;
                // With its continuation lines (+ ...).
                QString card = line;
                while (i + 1 < lines.size() && lines.at(i + 1).trimmed().startsWith(QLatin1Char('+'))) card += QLatin1Char(' ') + lines.at(++i).trimmed().mid(1);
                ++looked;
                static const QRegularExpression head(QStringLiteral("^\\.model\\s+(\\S+)\\s+([A-Za-z]+)\\s*\\(?(.*)$"), QRegularExpression::CaseInsensitiveOption);
                const QRegularExpressionMatch h = head.match(card);
                if (!h.hasMatch()) continue;
                const QString name = h.captured(1), type = h.captured(2).toUpper();
                if (!wordsIn(name + QLatin1Char(' ') + type + QLatin1Char(' ') + QFileInfo(path).fileName())) continue;
                if (!spiceType.isEmpty() && type != spiceType) continue;
                if (spiceType.isEmpty() && !model.isEmpty()) continue;   // a Qucs model asked for
                QHash<QString, QString> params;
                static const QRegularExpression pair(QStringLiteral("([A-Za-z_][A-Za-z0-9_]*)\\s*=\\s*([^\\s()=]+)"));
                for (auto p = pair.globalMatch(h.captured(3)); p.hasNext();) {
                    const QRegularExpressionMatch q = p.next();
                    params.insert(q.captured(1).toLower(), q.captured(2));
                }
                double score = 0;
                bool all = true;
                QJsonObject shown;
                for (auto t = targets.cbegin(); t != targets.cend() && all; ++t) {
                    double x = 0;
                    const QString v = params.value(t.key().toLower());
                    if (v.isEmpty() || !numberIn(v, &x)) all = false;
                    else {
                        score += distance(x, t.value());
                        shown.insert(t.key().toUpper(), v);
                    }
                }
                if (!all) continue;
                QString device = type == QLatin1String("NPN") ? QStringLiteral("NPN_SPICE")
                                 : type == QLatin1String("PNP") ? QStringLiteral("PNP_SPICE") : QString();
                QJsonObject o{{QStringLiteral("file"), QDir::toNativeSeparators(QDir(QucsSettings.QucsWorkDir).relativeFilePath(path).startsWith(QLatin1String(".."))
                                                                                    ? path : QDir(QucsSettings.QucsWorkDir).relativeFilePath(path))},
                              {QStringLiteral("model"), name},
                              {QStringLiteral("spice type"), type},
                              {QStringLiteral("model card"), card.size() > 1500 ? card.left(1500) + QStringLiteral(" ...") : card},
                              {QStringLiteral("place"), device.isEmpty()
                                   ? tr("a SPICE device of type %1 whose model is %2, and the file included (a SpiceInclude block)").arg(type, name)
                                   : tr("%1 whose model is %2, and the file included (a SpiceInclude block)").arg(device, name)}};
                if (!shown.isEmpty()) o.insert(QStringLiteral("values"), shown);
                found.append(Found{o, score});
            }
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.score < b.score; });
    QJsonArray list;
    for (const Found& f : std::as_const(found)) {
        if (list.size() >= limit) break;
        QJsonObject o = f.json;
        if (!targets.isEmpty()) o.insert(QStringLiteral("off by"), rounded(f.score));
        list.append(o);
    }
    QJsonObject result{{QStringLiteral("found"), list}, {QStringLiteral("looked at"), looked}, {QStringLiteral("matching"), int(found.size())}};
    if (!targets.isEmpty()) result.insert(QStringLiteral("order"), tr("the nearest first: the sum of each value's distance from its target, on a log scale"));
    if (found.size() > list.size()) result.insert(QStringLiteral("left out"), tr("%1 more (limit)").arg(found.size() - list.size()));
    if (list.isEmpty()) result.insert(QStringLiteral("note"), tr("Nothing matches: fewer words, or another type."));
    return jsonResult(result);
}

// ----------------------------------------------------------------------
// What changed between two states of a schematic, in words

namespace {

struct PartLine {
    QString model, name;
    int active = 1, cx = 0, cy = 0, tx = 0, ty = 0, mirror = 0, rotation = 0;
    QStringList values;
    QList<bool> shown;
    QString line;
};

bool parsePart(const QString& line, PartLine* p)
{
    const QString t = line.trimmed();
    if (!t.startsWith(QLatin1Char('<')) || !t.endsWith(QLatin1Char('>'))) return false;
    const QString head = t.mid(1).section(QLatin1Char('"'), 0, 0);
    const QStringList f = head.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (f.size() < 9) return false;
    p->model = f.at(0);
    p->name = f.at(1);
    p->active = f.at(2).toInt();
    p->cx = f.at(3).toInt();
    p->cy = f.at(4).toInt();
    p->tx = f.at(5).toInt();
    p->ty = f.at(6).toInt();
    p->mirror = f.at(7).toInt();
    p->rotation = f.at(8).toInt();
    // "value" shown, "value" shown, ...
    static const QRegularExpression value(QStringLiteral("\"([^\"]*)\"\\s*(\\d)?"));
    for (auto it = value.globalMatch(t); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        p->values << m.captured(1);
        p->shown << (m.captured(2) == QLatin1String("1"));
    }
    p->line = t;
    return true;
}

QStringList sectionsOf(const QString& state)
{
    QStringList sections = state.split(QStringLiteral("</>\n"));
    if (!sections.isEmpty()) sections[0] = sections.at(0).section(QLatin1Char('\n'), 1);   // (the first line: the step's kind)
    while (sections.size() < 4) sections << QString();
    return sections;
}

QStringList linesOf(const QString& section)
{
    QStringList lines;
    for (const QString& l : section.split(QLatin1Char('\n')))
        if (!l.trimmed().isEmpty()) lines << l.trimmed();
    return lines;
}

// A wire's line with its ends in one order; its label's name aside.
QString wireKey(const QString& line, QString* label)
{
    static const QRegularExpression w(QStringLiteral("^<(-?\\d+) (-?\\d+) (-?\\d+) (-?\\d+) \"([^\"]*)\"(.*)$"));
    const QRegularExpressionMatch m = w.match(line);
    if (!m.hasMatch()) return line;
    *label = m.captured(5);
    QPoint a(m.captured(1).toInt(), m.captured(2).toInt()), b(m.captured(3).toInt(), m.captured(4).toInt());
    if (std::pair(b.x(), b.y()) < std::pair(a.x(), a.y())) std::swap(a, b);
    return QStringLiteral("%1 %2 %3 %4").arg(a.x()).arg(a.y()).arg(b.x()).arg(b.y());
}

struct DiagramBlock {
    QString header, name;
    QStringList traces;
    int markers = 0;
};

QList<DiagramBlock> diagramsIn(const QString& section)
{
    QList<DiagramBlock> list;
    DiagramBlock* open = nullptr;
    for (const QString& l : linesOf(section)) {
        if (open == nullptr) {
            list.append(DiagramBlock{l, l.mid(1).section(QLatin1Char(' '), 0, 0), {}, 0});
            open = &list.last();
        } else if (l == QStringLiteral("</%1>").arg(open->name)) {
            open = nullptr;
        } else if (l.startsWith(QLatin1String("<\""))) {
            open->traces << l;
        } else if (l.startsWith(QLatin1String("<Mkr"))) {
            ++open->markers;
        }
    }
    return list;
}

QString where(int x, int y) { return QStringLiteral("(%1, %2)").arg(x).arg(y); }

} // namespace

namespace qucs_s::control {

QStringList describeChanges(const QString& before, const QString& after, int most)
{
    QStringList changes;
    if (before == after) return changes;
    const QStringList a = sectionsOf(before), b = sectionsOf(after);

    // Components, by name (an unnamed one - a ground - by its type and place).
    const auto parts = [](const QString& section) {
        QList<PartLine> list;
        for (const QString& l : linesOf(section)) {
            PartLine p;
            if (parsePart(l, &p)) list << p;
        }
        return list;
    };
    const QList<PartLine> was = parts(a.at(0)), now = parts(b.at(0));
    const auto key = [](const PartLine& p) {
        return p.name == QLatin1String("*") ? QStringLiteral("%1@%2,%3").arg(p.model).arg(p.cx).arg(p.cy) : p.name;
    };
    QHash<QString, const PartLine*> byKeyWas, byKeyNow;
    for (const PartLine& p : was) byKeyWas.insert(key(p), &p);
    for (const PartLine& p : now) byKeyNow.insert(key(p), &p);
    static QHash<QString, QStringList> names;   // a type's properties, in order
    const auto propertyName = [](const QString& model, int i, const QString& value) {
        if (value.contains(QLatin1Char('=')) && !value.startsWith(QLatin1Char('='))) return QString();   // an equation: name=value
        if (!names.contains(model)) {
            QStringList list;
            if (std::unique_ptr<Component> c{newComponent(model)})
                for (Property* p : c->Props) list << p->Name;
            names.insert(model, list);
        }
        return names.value(model).value(i, QStringLiteral("property %1").arg(i + 1));
    };
    QStringList added, removed;
    for (const PartLine& p : now)
        if (!byKeyWas.contains(key(p))) added << key(p);
    for (const PartLine& p : was)
        if (!byKeyNow.contains(key(p))) removed << key(p);
    // A rename: gone and come of one type at one place.
    for (int i = int(removed.size()) - 1; i >= 0; --i) {
        const PartLine* old = byKeyWas.value(removed.at(i));
        for (int j = 0; j < added.size(); ++j) {
            const PartLine* fresh = byKeyNow.value(added.at(j));
            if (old->model == fresh->model && old->cx == fresh->cx && old->cy == fresh->cy && old->name != QLatin1String("*")) {
                changes << tr("%1 renamed %2").arg(old->name, fresh->name);
                byKeyWas.insert(fresh->name, old);   // (compared below as one part)
                removed.removeAt(i);
                added.removeAt(j);
                break;
            }
        }
    }
    for (const QString& k : std::as_const(added)) {
        const PartLine* p = byKeyNow.value(k);
        QString first;
        if (!p->values.isEmpty()) {
            const QString name = propertyName(p->model, 0, p->values.first());
            first = name.isEmpty() ? p->values.first() : name + QLatin1Char('=') + p->values.first();
        }
        changes << tr("%1 added (%2%3) at %4").arg(p->name == QLatin1String("*") ? p->model : p->name, p->model,
                                                   first.isEmpty() ? QString() : QStringLiteral(", ") + first, where(p->cx, p->cy));
    }
    for (const QString& k : std::as_const(removed)) {
        const PartLine* p = byKeyWas.value(k);
        changes << tr("%1 deleted (it was at %2)").arg(p->name == QLatin1String("*") ? p->model : p->name, where(p->cx, p->cy));
    }
    for (const PartLine& p : now) {
        const PartLine* old = byKeyWas.value(key(p));
        if (old == nullptr || old->line == p.line) continue;
        const QString who = p.name == QLatin1String("*") ? p.model : p.name;
        QStringList what;
        if (old->cx != p.cx || old->cy != p.cy) what << tr("moved from %1 to %2").arg(where(old->cx, old->cy), where(p.cx, p.cy));
        if (old->rotation != p.rotation) what << tr("turned");
        if (old->mirror != p.mirror) what << tr("mirrored");
        if (old->active != p.active) what << ((p.active & 1) ? tr("made active") : tr("made inactive (left out of the simulation)"));
        // Of another type now (replace_component): its properties are
        // another list - compared by their names, not their places.
        if (old->model != p.model) {
            what << tr("now type %1 (was %2)").arg(p.model, old->model);
            QHash<QString, QString> before;
            for (int i = 0; i < old->values.size(); ++i)
                if (const QString name = propertyName(old->model, i, old->values.at(i)); !name.isEmpty()) before.insert(name, old->values.at(i));
            for (int i = 0; i < p.values.size(); ++i) {
                const QString name = propertyName(p.model, i, p.values.at(i));
                if (name.isEmpty() || name.startsWith(QLatin1String("property "))) continue;
                if (before.contains(name) && before.value(name) != p.values.at(i))
                    what << tr("%1 %2 → %3").arg(name, before.value(name), p.values.at(i));
                else if (!before.contains(name) && i == 0)
                    what << QStringLiteral("%1=%2").arg(name, p.values.at(i));   // (its value, as a new one's is told)
            }
            changes << QStringLiteral("%1: %2").arg(who, what.join(QStringLiteral(", ")));
            continue;
        }
        const int n = int(std::max(old->values.size(), p.values.size()));
        int shownChanges = 0;
        for (int i = 0; i < n; ++i) {
            const QString v0 = old->values.value(i), v1 = p.values.value(i);
            const QString name = propertyName(p.model, i, v1.isEmpty() ? v0 : v1);
            if (v0 != v1) what << (name.isEmpty() ? tr("%1 is %2 (was %3)").arg(v1.section(QLatin1Char('='), 0, 0), v1.section(QLatin1Char('='), 1), v0.section(QLatin1Char('='), 1))
                                                  : tr("%1 %2 → %3").arg(name, v0, v1));
            else if (old->shown.value(i) != p.shown.value(i)) ++shownChanges;
        }
        if (shownChanges > 0) what << tr("the properties shown changed");
        if (what.isEmpty() && (old->tx != p.tx || old->ty != p.ty)) what << tr("its text moved");
        if (what.isEmpty()) what << tr("changed");
        changes << QStringLiteral("%1: %2").arg(who, what.join(QStringLiteral(", ")));
    }

    // Wires and net labels.
    QHash<QString, int> wiresWas, wiresNow;
    QSet<QString> labelsWas, labelsNow;
    for (const QString& l : linesOf(a.at(1))) {
        QString label;
        wiresWas[wireKey(l, &label)]++;
        if (!label.isEmpty()) labelsWas.insert(label);
    }
    for (const QString& l : linesOf(b.at(1))) {
        QString label;
        wiresNow[wireKey(l, &label)]++;
        if (!label.isEmpty()) labelsNow.insert(label);
    }
    int wiresAdded = 0, wiresRemoved = 0;
    for (auto it = wiresNow.cbegin(); it != wiresNow.cend(); ++it) wiresAdded += std::max(0, it.value() - wiresWas.value(it.key()));
    for (auto it = wiresWas.cbegin(); it != wiresWas.cend(); ++it) wiresRemoved += std::max(0, it.value() - wiresNow.value(it.key()));
    if (wiresAdded > 0 || wiresRemoved > 0) {
        QStringList w;
        if (wiresAdded > 0) w << (wiresAdded == 1 ? tr("a wire drawn") : tr("%1 wires drawn").arg(wiresAdded));
        if (wiresRemoved > 0) w << (wiresRemoved == 1 ? tr("a wire taken away") : tr("%1 wires taken away").arg(wiresRemoved));
        changes << w.join(QStringLiteral(", "));
    }
    for (const QString& l : labelsNow - labelsWas) changes << tr("net label %1 set").arg(l);
    for (const QString& l : labelsWas - labelsNow) changes << tr("net label %1 taken away").arg(l);

    // Diagrams, by their numbers.
    const QList<DiagramBlock> dw = diagramsIn(a.at(2)), dn = diagramsIn(b.at(2));
    for (int i = 0; i < std::max(dw.size(), dn.size()); ++i) {
        if (i >= dw.size()) {
            changes << tr("diagram %1 added (%2)").arg(i + 1).arg(dn.at(i).name);
            continue;
        }
        if (i >= dn.size()) {
            changes << tr("diagram %1 deleted").arg(i + 1);
            continue;
        }
        const DiagramBlock &o = dw.at(i), &d = dn.at(i);
        QStringList what;
        const QStringList fo = o.header.split(QLatin1Char(' ')), fd = d.header.split(QLatin1Char(' '));
        if (fo.value(1) != fd.value(1) || fo.value(2) != fd.value(2)) what << tr("moved");
        if (fo.value(3) != fd.value(3) || fo.value(4) != fd.value(4)) what << tr("resized");
        const QString to = o.header.section(QLatin1Char('"'), 7, 7), td = d.header.section(QLatin1Char('"'), 7, 7);
        if (to != td) what << (td.isEmpty() ? tr("its title taken away") : tr("its title “%1”").arg(td));
        const auto rest = [](const QStringList& f) { return QStringList(f.mid(5)).join(QLatin1Char(' ')).section(QLatin1Char('"'), 0, 5); };
        if (rest(fo) != rest(fd)) what << tr("its axes, grid or legend changed");
        const auto vars = [](const QStringList& traces) {
            QStringList v;
            for (const QString& t : traces) v << t.section(QLatin1Char('"'), 1, 1);
            return v;
        };
        const QStringList vo = vars(o.traces), vd = vars(d.traces);
        for (const QString& v : vd)
            if (!vo.contains(v)) what << tr("trace %1 added").arg(v);
        for (const QString& v : vo)
            if (!vd.contains(v)) what << tr("trace %1 deleted").arg(v);
        for (int t = 0; t < std::min(o.traces.size(), d.traces.size()); ++t)
            if (o.traces.at(t) != d.traces.at(t) && vo.value(t) == vd.value(t)) what << tr("trace %1's look changed").arg(t + 1);
        if (o.markers != d.markers) what << tr("markers %1 → %2").arg(o.markers).arg(d.markers);
        if (!what.isEmpty()) changes << tr("diagram %1: %2").arg(i + 1).arg(what.join(QStringLiteral(", ")));
    }

    // Paintings, by their numbers.
    const QStringList pw = linesOf(a.at(3)), pn = linesOf(b.at(3));
    const auto typeOfLine = [](const QString& l) { return l.mid(1).section(QLatin1Char(' '), 0, 0).remove(QLatin1Char('<')); };
    for (int i = 0; i < std::max(pw.size(), pn.size()); ++i) {
        if (i >= pw.size()) changes << tr("painting %1 added (%2)").arg(i + 1).arg(typeOfLine(pn.at(i)));
        else if (i >= pn.size()) changes << tr("painting %1 deleted (%2)").arg(i + 1).arg(typeOfLine(pw.at(i)));
        else if (pw.at(i) != pn.at(i)) changes << tr("painting %1 (%2) changed").arg(i + 1).arg(typeOfLine(pn.at(i)));
    }
    if (changes.size() > most) {
        const int left = int(changes.size()) - most;
        changes = changes.mid(0, most);
        changes << tr("and %1 more").arg(left);
    }
    return changes;
}

} // namespace qucs_s::control

// ----------------------------------------------------------------------
// The steps to undo, in words

QJsonObject QucsControl::undoHistory(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, false);
    if (sch == nullptr) return errorResult(error);
    const QStringList states = sch->undoStates();
    const int at = sch->undoIndex();
    const int count = std::clamp(args.value(QLatin1String("steps")).toInt(10), 1, 200);
    QJsonArray steps;
    for (int k = std::max(1, at - count + 1); k < states.size() && k <= at + 5; ++k) {
        const QStringList what = describeChanges(states.at(k - 1), states.at(k), 8);
        steps.append(QJsonObject{{QStringLiteral("step"), k},
                                 {QStringLiteral("done"), k <= at},
                                 {QStringLiteral("change"), what.isEmpty() ? tr("nothing seen (a symbol's change, or what the schematic's text does not keep)")
                                                                           : what.join(QStringLiteral("; "))}});
    }
    QJsonObject result{{QStringLiteral("document"), titleOf(sch)},
                       {QStringLiteral("at"), at},
                       {QStringLiteral("steps"), steps},
                       {QStringLiteral("how"), tr("undo with 'to': n makes it as it was after step n (0: as loaded); a step not done yet is redone so")}};
    if (at > count) result.insert(QStringLiteral("earlier"), tr("%1 steps before these (steps gives more)").arg(at - count));
    if (states.size() - 1 >= QucsSettings.maxUndo)
        result.insert(QStringLiteral("note"), tr("Only the last %1 steps are kept (Application Settings > Maximum undo operations).").arg(QucsSettings.maxUndo));
    return jsonResult(result);
}

// ----------------------------------------------------------------------
// Projects: made, opened; a schematic copied with its results; a run's
// scratch files cleared

namespace {

bool anyUnsaved(QucsApp* app, QStringList* names)
{
    for (QucsDoc* doc : app->allDocuments())
        if (doc->getDocChanged()) *names << QFileInfo(doc->getDocName()).fileName();
    return !names->isEmpty();
}

// Its <Properties>' <DataSet=...> and <DataDisplay=...> set to \a dataset
// and \a display.
QString withResultsNamed(const QString& text, const QString& dataset, const QString& display)
{
    QString out = text;
    static const QRegularExpression ds(QStringLiteral("<DataSet=[^>]*>")), dd(QStringLiteral("<DataDisplay=[^>]*>"));
    out.replace(ds, QStringLiteral("<DataSet=%1>").arg(dataset));
    out.replace(dd, QStringLiteral("<DataDisplay=%1>").arg(display));
    return out;
}

bool copyText(const QString& from, const QString& to, const std::function<QString(const QString&)>& change, QString* error)
{
    QFile in(from);
    if (!in.open(QIODevice::ReadOnly)) {
        *error = QObject::tr("%1 cannot be read.").arg(QDir::toNativeSeparators(from));
        return false;
    }
    const QString text = change(QString::fromUtf8(in.readAll()));
    QFile out(to);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = QObject::tr("%1 cannot be written.").arg(QDir::toNativeSeparators(to));
        return false;
    }
    out.write(text.toUtf8());
    return true;
}

// To the system's trash; else away (scratch files are made again).
bool toTrash(const QString& path, bool* trashed)
{
    if (QFile::moveToTrash(path)) {
        *trashed = true;
        return true;
    }
    *trashed = false;
    const QFileInfo fi(path);
    return fi.isDir() ? QDir(path).removeRecursively() : QFile::remove(path);
}

} // namespace

QJsonObject QucsControl::newProject(const QJsonObject& args)
{
    const QString name = args.value(QLatin1String("name")).toString().trimmed();
    if (name.isEmpty() || name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')) || name.startsWith(QLatin1Char('.')))
        return errorResult(tr("'name' is the project's name (a folder name: no slashes)."));
    const QString folder = qucs_s::workspace::folderFor(name);
    const QDir workspace(QucsSettings.qucsWorkspaceDir.absolutePath());
    const QString path = workspace.filePath(folder);
    if (QFileInfo::exists(path)) return errorResult(tr("There is a project or folder %1 in the workspace already.").arg(folder));
    if (!workspace.mkdir(folder)) return errorResult(tr("%1 cannot be made.").arg(QDir::toNativeSeparators(path)));
    QDir(path).mkdir(QLatin1String(misc::ScratchFolder));
    a_app->readProjects();
    QJsonObject result{{QStringLiteral("project"), qucs_s::workspace::projectName(folder)}, {QStringLiteral("folder"), QDir::toNativeSeparators(path)}};
    if (args.value(QLatin1String("open")).toBool(true)) {
        QStringList unsaved;
        if (anyUnsaved(a_app, &unsaved))
            result.insert(QStringLiteral("opened"), tr("not opened: %1 have unsaved changes (opening a project closes the documents) - "
                                                       "save or close them, then open_project").arg(unsaved.join(QStringLiteral(", "))));
        else {
            a_app->openProject(path);
            result.insert(QStringLiteral("opened"), true);
            result.insert(QStringLiteral("note"), tr("Relative paths are taken from it now."));
        }
    }
    return jsonResult(result);
}

QJsonObject QucsControl::openProject(const QJsonObject& args)
{
    const QString name = args.value(QLatin1String("name")).toString().trimmed();
    if (name.isEmpty()) return errorResult(tr("'name': a project of the workspace (amp or amp_prj), or a project's folder."));
    const QDir workspace(QucsSettings.qucsWorkspaceDir.absolutePath());
    QString path;
    for (const QString& candidate : {workspace.filePath(name), workspace.filePath(qucs_s::workspace::folderFor(name)),
                                     workspace.filePath(name + QStringLiteral("_prj")), name})
        if (QFileInfo(candidate).isDir() && path.isEmpty()) path = QFileInfo(candidate).absoluteFilePath();
    if (path.isEmpty()) return errorResult(tr("There is no project %1 (list_documents lists the workspace's).").arg(name));
    // (What openProject() would refuse in a message box: told here.)
    const QString real = QFileInfo(path).canonicalFilePath();
    if (real == QDir(workspace).canonicalPath() || real == QDir::home().canonicalPath())
        return errorResult(tr("%1 is the %2 folder, not a project: name a folder in it.")
                               .arg(QDir::toNativeSeparators(path), real == QDir::home().canonicalPath() ? tr("home") : tr("workspace")));
    if (!qucs_s::workspace::isProjectName(QFileInfo(path).fileName()))
        return errorResult(tr("%1 is not a project: its name does not end in _prj (any folder is one when Settings > Locations "
                              "says so), or it is hidden or the folder of user libraries.")
                               .arg(QDir::toNativeSeparators(path)));
    QStringList unsaved;
    if (anyUnsaved(a_app, &unsaved))
        return errorResult(tr("Opening a project closes the documents, and %1 have unsaved changes: save or close them first.")
                               .arg(unsaved.join(QStringLiteral(", "))));
    a_app->openProject(path);
    if (QDir::cleanPath(QucsSettings.QucsWorkDir.absolutePath()) != QDir::cleanPath(path))
        return errorResult(tr("%1 could not be opened.").arg(QDir::toNativeSeparators(path)));
    return textResult(tr("The project %1 is open (%2): relative paths are taken from it now.")
                          .arg(QFileInfo(path).fileName(), QDir::toNativeSeparators(path)));
}

QJsonObject QucsControl::copyDocument(const QJsonObject& args)
{
    QString error;
    // The schematic: open (as it is in Qucs-S, unsaved changes too), or a file.
    QString from = args.value(QLatin1String("path")).toString().trimmed();
    Schematic* open = nullptr;
    if (QucsDoc* doc = document(args, &error)) {
        open = dynamic_cast<Schematic*>(doc);
        from = doc->getDocName();
    } else if (!from.isEmpty()) {
        from = absolute(from);
    }
    if (from.isEmpty() || !from.endsWith(QLatin1String(".sch"), Qt::CaseInsensitive))
        return errorResult(tr("Which schematic? 'path' (a .sch, open or not; the one in front unless given)."));
    if (open == nullptr && !QFileInfo(from).isFile()) return errorResult(tr("There is no file %1.").arg(QDir::toNativeSeparators(from)));
    QString to = args.value(QLatin1String("to")).toString().trimmed();
    if (to.isEmpty()) return errorResult(tr("'to': the copy's name (amp2: amp2.sch beside it), a path, or a folder or project to copy into."));
    // Into a folder or a project: of the same name.
    const QString asProject = QucsSettings.qucsWorkspaceDir.absoluteFilePath(qucs_s::workspace::folderFor(to));
    if (QFileInfo(absolute(to)).isDir()) to = QDir(absolute(to)).filePath(QFileInfo(from).fileName());
    else if (!to.contains(QLatin1Char('/')) && QFileInfo(asProject).isDir() && !to.endsWith(QLatin1String(".sch")))
        to = QDir(asProject).filePath(QFileInfo(from).fileName());
    else {
        if (!to.endsWith(QLatin1String(".sch"), Qt::CaseInsensitive)) to += QStringLiteral(".sch");
        to = to.contains(QLatin1Char('/')) || to.contains(QLatin1Char('\\')) ? absolute(to) : QFileInfo(from).absoluteDir().filePath(to);
    }
    if (sameFile(from, to)) return errorResult(tr("The copy would be the schematic itself."));
    // A file there already: written over when 'replace' says so, or when
    // the user says yes, asked.
    if (QFileInfo::exists(to) && !args.value(QLatin1String("replace")).toBool()
        && !confirmed(tr("%1 exists. Write over it?").arg(QDir::toNativeSeparators(to))))
        return errorResult(tr("%1 exists: 'replace' writes over it (the user was not asked, or said no).").arg(QDir::toNativeSeparators(to)));
    if (!QFileInfo(QFileInfo(to).absolutePath()).isDir()) return errorResult(tr("There is no folder %1.").arg(QDir::toNativeSeparators(QFileInfo(to).absolutePath())));
    for (QucsDoc* doc : a_app->allDocuments())
        if (sameFile(doc->getDocName(), to)) return errorResult(tr("%1 is open: close it first.").arg(QFileInfo(to).fileName()));

    const QFileInfo src(from), dst(to);
    const QString base = src.completeBaseName(), newBase = dst.completeBaseName();
    QStringList written;
    // The schematic, its results named after it.
    const auto rename = [&](const QString& text) {
        return withResultsNamed(text, newBase + QStringLiteral(".dat"), newBase + QStringLiteral(".dpl"));
    };
    if (open != nullptr) {
        const QString temp = to + QStringLiteral(".part");
        if (!open->writeTo(temp) || !copyText(temp, to, rename, &error)) {
            QFile::remove(temp);
            return errorResult(error.isEmpty() ? tr("%1 cannot be written.").arg(QDir::toNativeSeparators(to)) : error);
        }
        QFile::remove(temp);
    } else if (!copyText(from, to, rename, &error)) {
        return errorResult(error);
    }
    written << dst.fileName();
    if (args.value(QLatin1String("results")).toBool(true)) {
        // Its datasets (each simulator's) and its data display.
        for (const QString& suffix : {QStringLiteral(".dat"), QStringLiteral(".dat.ngspice"), QStringLiteral(".dat.xyce"), QStringLiteral(".dat.spopus")}) {
            const QString a = src.absoluteDir().filePath(base + suffix), b = dst.absoluteDir().filePath(newBase + suffix);
            if (!QFileInfo::exists(a)) continue;
            if (misc::copyFileOver(a, b)) written << QFileInfo(b).fileName();
        }
        const QString dpl = src.absoluteDir().filePath(base + QStringLiteral(".dpl"));
        if (QFileInfo::exists(dpl)) {
            const QString b = dst.absoluteDir().filePath(newBase + QStringLiteral(".dpl"));
            if (copyText(dpl, b, [&](const QString& text) { return withResultsNamed(text, newBase + QStringLiteral(".dat"), newBase + QStringLiteral(".sch")); }, &error))
                written << QFileInfo(b).fileName();
        }
    }
    if (a_app->projectView() != nullptr) a_app->projectView()->refresh();
    QJsonObject result{{QStringLiteral("copy"), QDir::toNativeSeparators(to)}, {QStringLiteral("written"), QJsonArray::fromStringList(written)}};
    if (open != nullptr && open->getDocChanged()) result.insert(QStringLiteral("note"), tr("Copied as it is in Qucs-S, its unsaved changes too."));
    return jsonResult(result);
}

QJsonObject QucsControl::cleanScratch(const QJsonObject& args)
{
    QString error;
    QString docName;
    if (QucsDoc* doc = document(args, &error)) docName = doc->getDocName();
    else if (!args.value(QLatin1String("path")).toString().trimmed().isEmpty()) docName = absolute(args.value(QLatin1String("path")).toString().trimmed());
    if (docName.isEmpty() || !docName.endsWith(QLatin1String(".sch"), Qt::CaseInsensitive))
        return errorResult(tr("Which schematic's? 'path' (a .sch; the one in front unless given)."));
    if (a_app->simulationConsole() != nullptr && a_app->simulationConsole()->isRunning())
        return errorResult(tr("A simulation is running: its files are cleared after it has ended."));
    QStringList gone;
    bool trashed = true;
    const QString scratch = misc::scratchDirFor(docName);
    // Its own subfolder of the Scratch folder only - never the folder shared.
    if (QFileInfo(scratch).isDir() && QFileInfo(scratch).fileName() != QLatin1String(misc::ScratchFolder)) {
        const int files = int(QDir(scratch).entryList(QDir::Files | QDir::NoDotAndDotDot).size());
        bool t = false;
        if (toTrash(scratch, &t)) gone << tr("%1 (%2 files: netlists, the simulator's output, logs)").arg(QDir::toNativeSeparators(scratch)).arg(files);
        trashed = trashed && t;
    }
    if (args.value(QLatin1String("datasets")).toBool()) {
        const QFileInfo info(docName);
        for (const QString& suffix : {QStringLiteral(".dat"), QStringLiteral(".dat.ngspice"), QStringLiteral(".dat.xyce"), QStringLiteral(".dat.spopus")}) {
            const QString f = info.absoluteDir().filePath(info.completeBaseName() + suffix);
            bool t = false;
            if (QFileInfo::exists(f) && toTrash(f, &t)) {
                gone << QFileInfo(f).fileName();
                trashed = trashed && t;
            }
        }
    }
    if (gone.isEmpty()) return textResult(tr("There was nothing to clear for %1.").arg(QFileInfo(docName).fileName()));
    return textResult(tr("Cleared: %1. %2").arg(gone.join(QStringLiteral("; ")),
                                                 trashed ? tr("(In the trash, to take back.)") : tr("(Deleted: this system has no trash.)")));
}

// ----------------------------------------------------------------------
// A subcircuit's symbol drawn: a box, each pin on its side

QJsonObject QucsControl::makeSymbol(const QJsonObject& args)
{
    QString error, note;
    std::list<Painting*>* list = nullptr;
    QJsonObject onSymbol = args;
    onSymbol.insert(QStringLiteral("symbol"), true);
    Schematic* sch = paintingsOf(onSymbol, &list, &note, &error);
    if (sch == nullptr) return errorResult(error);
    if (sch->getIsSymbolOnly()) return errorResult(tr("%1 is a symbol file: its ports are drawn by hand.").arg(titleOf(sch)));
    QHash<QString, QString> sides;
    const QJsonObject given = args.value(QLatin1String("sides")).toObject();
    for (auto it = given.begin(); it != given.end(); ++it) sides.insert(it.key().toLower(), it.value().toString().trimmed().toLower());
    const auto before = sch->snapshotAll();
    QStringList placed;
    if (!sch->buildSymbol(sides, &error, &placed)) {
        sch->restoreAll(before);
        return errorResult(error);
    }
    sch->updateAllBoundingRect();
    sch->setChanged(true, true);
    sch->viewport()->update();
    QJsonObject result{{QStringLiteral("document"), titleOf(sch)}, {QStringLiteral("pins"), QJsonArray::fromStringList(placed)},
                       {QStringLiteral("then"), tr("Save it for the subcircuit's instances to take it; edit_painting moves a port or the name "
                                                   "text, add_painting draws more.")}};
    if (!note.isEmpty()) result.insert(QStringLiteral("note"), note);
    return jsonResult(result);
}

// ----------------------------------------------------------------------
// A SPICE netlist as a schematic: each element a SPICE part of its kind,
// placed in rows, its nets joined by labels (0 by grounds)

namespace {

// The lines of a netlist: continuations joined, comments left out.
QStringList netlistLines(const QString& text)
{
    QStringList lines;
    for (QString line : text.split(QLatin1Char('\n'))) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('*'))) continue;
        // ; and " $ " start a comment on a line.
        const int semicolon = int(line.indexOf(QLatin1Char(';')));
        if (semicolon >= 0) line = line.left(semicolon).trimmed();
        const int dollar = int(line.indexOf(QStringLiteral(" $ ")));
        if (dollar >= 0) line = line.left(dollar).trimmed();
        if (line.isEmpty()) continue;
        if (line.startsWith(QLatin1Char('+')) && !lines.isEmpty()) lines.last() += QLatin1Char(' ') + line.mid(1).trimmed();
        else lines << line;
    }
    return lines;
}

// Numbers as SPICE writes them: 1k, 10u, 1meg, 2.5e-3.
double spiceNumber(const QString& text)
{
    static const QRegularExpression n(QStringLiteral("^([-+]?[0-9]*\\.?[0-9]+(?:[eE][-+]?[0-9]+)?)([a-zA-Z]*)"));
    const QRegularExpressionMatch m = n.match(text.trimmed());
    if (!m.hasMatch()) return NAN;
    double v = m.captured(1).toDouble();
    const QString s = m.captured(2).toLower();
    if (s.startsWith(QLatin1String("meg"))) v *= 1e6;
    else if (s.startsWith(QLatin1String("mil"))) v *= 25.4e-6;
    else if (!s.isEmpty()) {
        switch (s.at(0).toLatin1()) {
        case 't': v *= 1e12; break;
        case 'g': v *= 1e9; break;
        case 'k': v *= 1e3; break;
        case 'm': v *= 1e-3; break;
        case 'u': v *= 1e-6; break;
        case 'n': v *= 1e-9; break;
        case 'p': v *= 1e-12; break;
        case 'f': v *= 1e-15; break;
        default: break;
        }
    }
    return v;
}

struct NetlistElement {
    QChar letter;
    QString name, value;
    QStringList nodes;
};

} // namespace

QJsonObject QucsControl::importNetlist(const QJsonObject& args)
{
    QString text = args.value(QLatin1String("text")).toString();
    const QString fileArg = args.value(QLatin1String("file")).toString().trimmed();
    QString source = tr("the text given");
    if (text.trimmed().isEmpty() && !fileArg.isEmpty()) {
        const QString file = absolute(fileArg);
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return errorResult(tr("There is no file %1.").arg(QDir::toNativeSeparators(file)));
        text = QString::fromUtf8(f.readAll());
        source = QFileInfo(file).fileName();
    }
    if (text.trimmed().isEmpty()) return errorResult(tr("Give the netlist: 'text', or 'file' (.cir, .sp, .net)."));
    QStringList lines = netlistLines(text);
    // A SPICE netlist's first line is its title - unless it plainly is an
    // element of a kind placed here (a fragment of a netlist), with a
    // value where one belongs.
    QString title;
    const auto looksLikeElement = [](const QString& line) {
        const QStringList f = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (f.size() < 4 || QStringLiteral("RCLVIDQMJEGXKB").indexOf(f.first().at(0).toUpper()) < 0) return false;
        if (QStringLiteral("RCL").indexOf(f.first().at(0).toUpper()) >= 0) {
            const QChar v = f.at(3).at(0);
            return v.isDigit() || v == QLatin1Char('{') || v == QLatin1Char('.') || v == QLatin1Char('+') || v == QLatin1Char('-');
        }
        return true;
    };
    QString first;
    for (const QString& raw : text.split(QLatin1Char('\n')))
        if (!raw.trimmed().isEmpty()) {
            first = raw.trimmed();
            break;
        }
    const QJsonValue titleLine = args.value(QLatin1String("title_line"));
    const bool isTitle = titleLine.isBool() ? titleLine.toBool()
                                            : !first.startsWith(QLatin1Char('*')) && !first.startsWith(QLatin1Char('.')) && !looksLikeElement(first);
    if (isTitle && !lines.isEmpty() && !first.startsWith(QLatin1Char('*'))) title = lines.takeFirst();

    // What is in it.
    QList<NetlistElement> elements;
    QStringList models, subcircuitText, params, options, includes, skipped, analyses;
    QHash<QString, QString> modelType;      // name (lower) -> NPN, PMOS, D, ...
    QHash<QString, int> subcircuitPins;     // name (lower) -> its pins
    QString inSubcircuit;
    bool inControl = false;
    for (const QString& line : std::as_const(lines)) {
        const QString lower = line.toLower();
        if (inControl) {
            if (lower.startsWith(QLatin1String(".endc"))) inControl = false;
            continue;
        }
        if (!inSubcircuit.isEmpty()) {
            subcircuitText << line;
            if (lower.startsWith(QLatin1String(".ends"))) inSubcircuit.clear();
            continue;
        }
        if (lower.startsWith(QLatin1String(".subckt"))) {
            const QStringList f = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            inSubcircuit = f.value(1).toLower();
            int pins = 0;
            for (int i = 2; i < f.size() && !f.at(i).contains(QLatin1Char('=')) && f.at(i).compare(QLatin1String("params:"), Qt::CaseInsensitive) != 0; ++i) ++pins;
            subcircuitPins.insert(inSubcircuit, pins);
            subcircuitText << line;
            continue;
        }
        if (lower.startsWith(QLatin1String(".control"))) {
            inControl = true;
            skipped << tr(".control ... .endc (a NutmegEq or a .control script of the schematic takes it)");
            continue;
        }
        if (lower.startsWith(QLatin1String(".end"))) break;
        if (lower.startsWith(QLatin1String(".model"))) {
            models << line;
            const QStringList f = line.split(QRegularExpression(QStringLiteral("[\\s(]+")), Qt::SkipEmptyParts);
            modelType.insert(f.value(1).toLower(), f.value(2).toUpper());
            continue;
        }
        if (lower.startsWith(QLatin1String(".param"))) {
            static const QRegularExpression pair(QStringLiteral("([A-Za-z_][A-Za-z0-9_]*)\\s*=\\s*(\\{[^}]*\\}|'[^']*'|\\S+)"));
            for (auto it = pair.globalMatch(line.mid(6)); it.hasNext();) {
                const QRegularExpressionMatch m = it.next();
                params << m.captured(1) + QLatin1Char('=') + m.captured(2);
            }
            continue;
        }
        if (lower.startsWith(QLatin1String(".option"))) {
            static const QRegularExpression pair(QStringLiteral("([A-Za-z_][A-Za-z0-9_]*)\\s*=\\s*(\\S+)"));
            for (auto it = pair.globalMatch(line); it.hasNext();) {
                const QRegularExpressionMatch m = it.next();
                options << m.captured(1).toUpper() + QLatin1Char('=') + m.captured(2);
            }
            continue;
        }
        if (lower.startsWith(QLatin1String(".include")) || lower.startsWith(QLatin1String(".inc ")) || lower.startsWith(QLatin1String(".lib"))) {
            includes << line;
            continue;
        }
        if (lower.startsWith(QLatin1String(".tran")) || lower.startsWith(QLatin1String(".ac")) || lower.startsWith(QLatin1String(".op"))
            || lower.startsWith(QLatin1String(".dc"))) {
            analyses << line;
            continue;
        }
        if (line.startsWith(QLatin1Char('.'))) {
            skipped << tr("%1 (not taken)").arg(line.left(80));
            continue;
        }
        // An element: its letter, name, nodes and value.
        const QStringList f = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (f.size() < 2) {
            skipped << tr("%1 (not read)").arg(line.left(80));
            continue;
        }
        NetlistElement e;
        e.letter = f.first().at(0).toUpper();
        e.name = f.first();
        int nodes = 2;
        const char letter = e.letter.toLatin1();
        if (letter == 'Q') {
            // c b e [s] model: the model ends the nodes.
            nodes = 3;
            if (f.size() > 5 && !modelType.contains(f.value(4).toLower()) && modelType.contains(f.value(5).toLower())) nodes = 4;
        } else if (letter == 'M') {
            nodes = 4;
        } else if (letter == 'J' || letter == 'Z') {
            nodes = 3;
        } else if (letter == 'E' || letter == 'G') {
            // Linear: n+ n- nc+ nc- gain; else n+ n- value=... (nonlinear)
            nodes = f.size() >= 6 && !f.value(3).contains(QLatin1Char('=')) && !f.value(3).startsWith(QLatin1String("value"), Qt::CaseInsensitive)
                            && !f.value(3).startsWith(QLatin1String("poly"), Qt::CaseInsensitive) ? 4 : 2;
        } else if (letter == 'X') {
            // n1 n2 ... subcircuit [params]: its name the last word without '='.
            int last = int(f.size()) - 1;
            while (last > 1 && (f.at(last).contains(QLatin1Char('=')) || f.at(last).compare(QLatin1String("params:"), Qt::CaseInsensitive) == 0)) --last;
            nodes = last - 1;
        } else if (letter == 'K') {
            nodes = 0;
        } else if (letter == 'F' || letter == 'H' || letter == 'T' || letter == 'O' || letter == 'U' || letter == 'A' || letter == 'W' || letter == 'S') {
            skipped << tr("%1 (%2: no SPICE part of this kind is placed by an import)").arg(line.left(80)).arg(e.letter);
            continue;
        } else if (QStringLiteral("RCLVIDB").indexOf(e.letter) < 0) {
            skipped << tr("%1 (an element of kind %2 is not known)").arg(line.left(80)).arg(e.letter);
            continue;
        }
        if (f.size() < 1 + nodes + (letter == 'K' ? 3 : 1)) {
            skipped << tr("%1 (too few fields)").arg(line.left(80));
            continue;
        }
        e.nodes = f.mid(1, nodes);
        // The value: the rest of the line, as written.
        QString rest = line;
        for (int i = 0; i <= nodes; ++i) rest = rest.section(QRegularExpression(QStringLiteral("\\s+")), 1).trimmed();
        e.value = rest;
        elements << e;
    }
    if (elements.isEmpty()) return errorResult(tr("No elements were found in %1.").arg(source));

    // The parts, as .sch lines, in rows.
    QStringList componentLines, wireLines, placed;
    QSet<QString> nets;
    int grounds = 0;
    const int spacing = std::clamp(args.value(QLatin1String("spacing")).toInt(200), 120, 600);
    const int perRow = std::clamp(int(std::ceil(std::sqrt(double(elements.size())))), 2, 8);
    int index = 0;
    const auto netName = [](const QString& node) {
        QString n = node;
        n.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_]")), QStringLiteral("_"));
        return n;
    };
    for (const NetlistElement& e : std::as_const(elements)) {
        const char letter = e.letter.toLatin1();
        QString model;
        QList<int> portOfNode;   // node i goes to this port
        switch (letter) {
        case 'R': model = QStringLiteral("R_SPICE"); break;
        case 'C': model = QStringLiteral("C_SPICE"); break;
        case 'L': model = QStringLiteral("L_SPICE"); break;
        case 'V': model = QStringLiteral("S4Q_V"); break;
        case 'I': model = QStringLiteral("S4Q_I"); break;
        case 'D': model = QStringLiteral("DIODE_SPICE"); break;
        case 'Q': model = modelType.value(e.value.section(QLatin1Char(' '), 0, 0).toLower()) == QLatin1String("PNP") ? QStringLiteral("PNP_SPICE") : QStringLiteral("NPN_SPICE"); break;
        case 'M': model = modelType.value(e.value.section(QLatin1Char(' '), 0, 0).toLower()) == QLatin1String("PMOS") ? QStringLiteral("PMOS_SPICE") : QStringLiteral("NMOS_SPICE"); break;
        case 'J': model = modelType.value(e.value.section(QLatin1Char(' '), 0, 0).toLower()) == QLatin1String("PJF") ? QStringLiteral("PJF_SPICE") : QStringLiteral("NJF_SPICE"); break;
        case 'Z': model = QStringLiteral("MESFET_SPICE"); break;
        case 'E': model = e.nodes.size() == 4 ? QStringLiteral("VCVS") : QStringLiteral("eNL"); break;
        case 'G': model = e.nodes.size() == 4 ? QStringLiteral("VCCS") : QStringLiteral("gNL"); break;
        case 'B': model = e.value.trimmed().startsWith(QLatin1Char('I'), Qt::CaseInsensitive) ? QStringLiteral("S4Q_Ieqndef") : QStringLiteral("src_eqndef"); break;
        case 'X': model = QStringLiteral("SPICE_dev"); break;
        case 'K': model = QStringLiteral("K_SPICE"); break;
        default: break;
        }
        std::unique_ptr<Component> c{newComponent(model)};
        if (!c) {
            skipped << tr("%1 (no %2 part in this build)").arg(e.name, model);
            continue;
        }
        // Its value: the SPICE text after the nodes, as written.
        const auto set = [&c](const QString& name, const QString& value) {
            if (Property* p = c->getProperty(name)) p->Value = value;
        };
        if (letter == 'X') {
            const QStringList f = e.value.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            set(QStringLiteral("NPins"), QString::number(e.nodes.size()));
            // (It writes its letter before its name: X1 is X1 with none.)
            set(QStringLiteral("Letter"), QString());
            set(QStringLiteral("Model"), f.value(0));
            set(QStringLiteral("Params"), QStringList(f.mid(1)).join(QLatin1Char(' ')));
            if (subcircuitPins.contains(f.value(0).toLower()) && subcircuitPins.value(f.value(0).toLower()) != e.nodes.size())
                skipped << tr("%1: %2 nodes, but %3 has %4 pins").arg(e.name).arg(e.nodes.size()).arg(f.value(0)).arg(subcircuitPins.value(f.value(0).toLower()));
        } else if (letter == 'K') {
            const QStringList f = e.value.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            // K name L1 L2 k: nodes 0, value "L1 L2 k"
            set(QStringLiteral("Ind1"), f.value(0));
            set(QStringLiteral("Ind2"), f.value(1));
            set(QStringLiteral("K"), f.value(2));
        } else if (model == QLatin1String("VCVS") || model == QLatin1String("VCCS")) {
            set(QStringLiteral("G"), e.value.section(QLatin1Char(' '), 0, 0));
            // SPICE's n+ n- nc+ nc- are its ports 2, 3, 1, 4.
            portOfNode = {1, 2, 0, 3};
        } else if (!c->Props.isEmpty()) {
            c->Props.first()->Value = e.value;
        }
        c->Name = e.name;
        c->recreate();
        if (portOfNode.isEmpty())
            for (int i = 0; i < e.nodes.size(); ++i) portOfNode << i;
        if (c->Ports.size() < e.nodes.size()) {
            skipped << tr("%1 (%2 nodes; %3 has %4 pins)").arg(e.name).arg(e.nodes.size()).arg(model).arg(c->Ports.size());
            continue;
        }
        const int x = 200 + (index % perRow) * spacing, y = 200 + (index / perRow) * spacing;
        ++index;
        c->moveCenter(x - c->cx, y - c->cy);
        componentLines << QStringLiteral("  ") + c->save();
        placed << e.name;
        // Each pin: its net's label on it, or a ground under a stub.
        for (int i = 0; i < e.nodes.size(); ++i) {
            const Port* port = c->Ports.at(portOfNode.at(i));
            const QPoint pin(c->cx + port->x, c->cy + port->y);
            const QString node = e.nodes.at(i);
            if (node == QLatin1String("0") || node.compare(QLatin1String("gnd"), Qt::CaseInsensitive) == 0) {
                // A stub away from the part, the ground at its end.
                QPoint dir(port->x > 0 ? 1 : port->x < 0 ? -1 : 0, port->y > 0 ? 1 : port->y < 0 ? -1 : 0);
                if (dir == QPoint(0, 0) || (dir.x() != 0 && dir.y() != 0)) dir = QPoint(0, 1);
                const QPoint end = pin + dir * 30;
                wireLines << QStringLiteral("  <%1 %2 %3 %4 \"\" 0 0 0 \"\">").arg(pin.x()).arg(pin.y()).arg(end.x()).arg(end.y());
                componentLines << QStringLiteral("  <GND * 1 %1 %2 0 0 0 0>").arg(end.x()).arg(end.y());
                ++grounds;
            } else {
                const QString net = netName(node);
                nets.insert(net);
                wireLines << QStringLiteral("  <%1 %2 %1 %2 \"%3\" %4 %5 0 \"\">").arg(pin.x()).arg(pin.y()).arg(net).arg(pin.x() + 10).arg(pin.y() - 20);
            }
        }
    }
    // The rest below: models, parameters, options, includes, analyses.
    const int rows = (index + perRow - 1) / perRow;
    int bx = 200;
    const int by = 200 + rows * spacing + 60;
    const auto block = [&](const QString& model, const std::function<void(Component*)>& fill) {
        std::unique_ptr<Component> c{newComponent(model)};
        if (!c) return;
        fill(c.get());
        c->recreate();
        c->moveCenter(bx - c->cx, by - c->cy);
        bx += 220;
        componentLines << QStringLiteral("  ") + c->save();
    };
    for (int i = 0; i < models.size(); i += 5) {
        block(QStringLiteral("SpiceModel"), [&](Component* c) {
            c->Name = QStringLiteral("SpiceModel%1").arg(i / 5 + 1);
            for (int k = 0; k < 5; ++k)
                if (Property* p = c->getProperty(QStringLiteral("Line_%1").arg(k + 1))) p->Value = models.value(i + k);
        });
    }
    if (!params.isEmpty())
        block(QStringLiteral("SpicePar"), [&](Component* c) {
            c->Name = QStringLiteral("SpicePar1");
            qDeleteAll(c->Props);
            c->Props.clear();
            for (const QString& p : std::as_const(params)) c->Props.append(new Property(p.section(QLatin1Char('='), 0, 0), p.section(QLatin1Char('='), 1), true));
        });
    if (!options.isEmpty())
        block(QStringLiteral("SpiceOptions"), [&](Component* c) {
            c->Name = QStringLiteral("SpiceOptions1");
            while (c->Props.size() > 1) delete c->Props.takeLast();
            for (const QString& o : std::as_const(options)) c->Props.append(new Property(o.section(QLatin1Char('='), 0, 0), o.section(QLatin1Char('='), 1), true));
        });
    // The subcircuits defined in it: into a library file beside the schematic.
    QString subcircuitFile;
    const QString saveAs = args.value(QLatin1String("save_as")).toString().trimmed();
    if (!subcircuitText.isEmpty()) {
        const QString base = saveAs.isEmpty() ? (title.isEmpty() ? QStringLiteral("imported") : title.left(40)) : QFileInfo(saveAs).completeBaseName();
        QString stem = base;
        stem.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")), QStringLiteral("_"));
        const QString dir = saveAs.isEmpty() ? QucsSettings.QucsWorkDir.absolutePath() : QFileInfo(absolute(saveAs.endsWith(QLatin1String(".sch")) ? saveAs : saveAs + QStringLiteral(".sch"))).absolutePath();
        subcircuitFile = QDir(dir).filePath(stem + QStringLiteral("_subcircuits.lib"));
        QFile f(subcircuitFile);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            f.write((QStringLiteral("* The subcircuits of %1, taken out by import_netlist\n").arg(source) + subcircuitText.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8());
            includes << QStringLiteral(".include \"%1\"").arg(subcircuitFile);
        } else {
            skipped << tr("the subcircuits: %1 could not be written").arg(QDir::toNativeSeparators(subcircuitFile));
            subcircuitFile.clear();
        }
    }
    for (const QString& inc : std::as_const(includes)) {
        const QString file = inc.section(QRegularExpression(QStringLiteral("\\s+")), 1, 1).remove(QLatin1Char('"')).remove(QLatin1Char('\''));
        const bool isLib = inc.toLower().startsWith(QLatin1String(".lib"));
        block(isLib ? QStringLiteral("SpiceLib") : QStringLiteral("SpiceInclude"), [&](Component* c) {
            if (!c->Props.isEmpty()) c->Props.first()->Value = file;
            if (isLib && c->Props.size() > 1) c->Props.at(1)->Value = inc.section(QRegularExpression(QStringLiteral("\\s+")), 2, 2);
        });
    }
    for (const QString& a : std::as_const(analyses)) {
        const QStringList f = a.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        const QString kind = f.first().toLower();
        if (kind == QLatin1String(".tran") && f.size() >= 3) {
            const double step = spiceNumber(f.at(1)), stop = spiceNumber(f.at(2)), start = f.size() > 3 ? spiceNumber(f.at(3)) : 0;
            block(QStringLiteral(".TR"), [&](Component* c) {
                c->Name = QStringLiteral("TR1");
                // Plain numbers: Qucs reads 1meg as milli.
                if (Property* p = c->getProperty(QStringLiteral("Start"))) p->Value = f.size() > 3 ? QString::number(start) : QStringLiteral("0");
                if (Property* p = c->getProperty(QStringLiteral("Stop"))) p->Value = QString::number(stop);
                if (Property* p = c->getProperty(QStringLiteral("Points")))
                    p->Value = QString::number(std::clamp(int(std::round((stop - (std::isnan(start) ? 0 : start)) / step)) + 1, 2, 1000001));
            });
        } else if (kind == QLatin1String(".ac") && f.size() >= 5) {
            const QString type = f.at(1).toLower();
            const double n = spiceNumber(f.at(2)), from = spiceNumber(f.at(3)), to = spiceNumber(f.at(4));
            int points = int(n);
            if (type == QLatin1String("dec") && from > 0) points = int(std::round(n * std::log10(to / from))) + 1;
            else if (type == QLatin1String("oct") && from > 0) points = int(std::round(n * std::log2(to / from))) + 1;
            block(QStringLiteral(".AC"), [&](Component* c) {
                c->Name = QStringLiteral("AC1");
                if (Property* p = c->getProperty(QStringLiteral("Type"))) p->Value = type == QLatin1String("lin") ? QStringLiteral("lin") : QStringLiteral("log");
                if (Property* p = c->getProperty(QStringLiteral("Start"))) p->Value = QString::number(from);
                if (Property* p = c->getProperty(QStringLiteral("Stop"))) p->Value = QString::number(to);
                if (Property* p = c->getProperty(QStringLiteral("Points"))) p->Value = QString::number(std::max(2, points));
            });
        } else if (kind == QLatin1String(".op")) {
            block(QStringLiteral(".DC"), [&](Component* c) { c->Name = QStringLiteral("DC1"); });
        } else {
            skipped << tr("%1 (a DC sweep: add_analysis with kind sweep makes one)").arg(a.left(80));
        }
    }

    // Into a new schematic, as one step to undo.
    a_app->slotFileNew();
    QString error;
    QJsonObject newDoc;
    Schematic* sch = schematic(newDoc, &error, true);
    if (sch == nullptr) return errorResult(error);
    const QString content = QStringLiteral("<Components>\n%1\n</Components>\n<Wires>\n%2\n</Wires>\n")
                                .arg(componentLines.join(QLatin1Char('\n')), wireLines.join(QLatin1Char('\n')));
    QStringList notes;
    if (!sch->replaceContent(content, &error, &notes)) return errorResult(tr("The schematic made of it does not read: %1").arg(error));
    closeUntouched(sch);
    sch->showAll();
    QJsonObject result{{QStringLiteral("document"), titleOf(sch)},
                       {QStringLiteral("parts"), QJsonArray::fromStringList(placed)},
                       {QStringLiteral("nets"), int(nets.size())},
                       {QStringLiteral("grounds"), grounds},
                       {QStringLiteral("placed as"), tr("each element a SPICE part of its kind with its netlist text (R_SPICE, S4Q_V, NPN_SPICE, "
                                                        "SPICE_dev for a subcircuit, ...), in rows; the nets joined by net labels on the pins, "
                                                        "0 by grounds - rough: move and connect tidy it; check_schematic checks it")}};
    if (!title.isEmpty()) result.insert(QStringLiteral("title"), title);
    if (!models.isEmpty()) result.insert(QStringLiteral("models"), int(models.size()));
    if (!subcircuitFile.isEmpty()) result.insert(QStringLiteral("subcircuits"), QDir::toNativeSeparators(subcircuitFile));
    if (!skipped.isEmpty()) result.insert(QStringLiteral("not taken"), QJsonArray::fromStringList(skipped));
    if (!notes.isEmpty()) result.insert(QStringLiteral("note"), notes.join(QLatin1Char(' ')));
    if (!saveAs.isEmpty()) {
        const QJsonObject saved = callNow(QStringLiteral("save_document"), {{QStringLiteral("as"), saveAs}});
        result.insert(QStringLiteral("saved"), saved.value(QStringLiteral("isError")).toBool() ? textOf(saved) : QDir::toNativeSeparators(sch->getDocName()));
    }
    return jsonResult(result);
}
