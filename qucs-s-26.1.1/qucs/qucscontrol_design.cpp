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
#include "erc.h"

#include "dataset.h"
#include "main.h"
#include "misc.h"
#include "dataimport.h"
#include "filebrowser.h"
#include "projectView.h"
#include "qucs.h"
#include "schematic.h"
#include "simulationconsole.h"
#include "workspace.h"
#include "textdoc.h"
#include "vamodule.h"
#include "valuereading.h"

#include "components/component.h"
#include "paintings/id_text.h"

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

#include <climits>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>

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
        // Marked in the open tab, as the user sees them: a wavy line, a dot
        // in the margin, the message on the line (none: those of the last
        // build go).
        for (QucsDoc* doc : a_app->allDocuments()) {
            auto* text = dynamic_cast<TextDoc*>(doc);
            if (text == nullptr || !sameFile(doc->getDocName(), file)) continue;
            QList<TextDoc::Diagnostic> marks;
            for (const char* severity : {"errors", "warnings"})
                for (const QJsonValue& v : result.value(QLatin1String(severity)).toArray()) {
                    const QJsonObject o = v.toObject();
                    if (o.value(QLatin1String("file")).toString() != QFileInfo(file).fileName() || o.value(QLatin1String("line")).toInt() < 1) continue;
                    marks.append({o.value(QLatin1String("line")).toInt(), o.value(QLatin1String("column")).toInt(),
                                  o.value(QLatin1String("message")).toString(), qstrcmp(severity, "errors") == 0});
                }
            text->setDiagnostics(marks);
            if (!marks.isEmpty())
                result.insert(QStringLiteral("marked"), tr("in the open tab: %n place(s), each with its message", "", int(marks.size())));
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
    // Six figures at most: 1k, not 999.999719. (Not of a number so small
    // its scale is past a double's: 1e-310 became nan.)
    if (std::isfinite(value) && value != 0) {
        const double scale = std::pow(10.0, 5 - int(std::floor(std::log10(std::abs(value)))));
        if (std::isfinite(scale) && std::isfinite(value * scale)) value = std::round(value * scale) / scale;
    }
    QString text = misc::num2str(value, 6);
    // Without the zeros after the point: 1k, not 1.000000k.
    static const QRegularExpression zeros(QStringLiteral("^([-+]?\\d+)(?:\\.(\\d*?))?0*(\\D*)$"));
    if (const QRegularExpressionMatch m = zeros.match(text); m.hasMatch() && text.contains(QLatin1Char('.')))
        text = m.captured(1) + (m.captured(2).isEmpty() ? QString() : QLatin1Char('.') + m.captured(2)) + m.captured(3);
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
        // (Its nodes and its branch currents, each called so: i(v1) is no node.)
        QStringList nodeNames, currents;
        for (const QString& name : nodes.keys()) (name.contains(QLatin1Char('(')) ? currents : nodeNames) << name;
        *why = tr("the operating point has no %1 (its nodes: %2%3)")
                   .arg(op, nodeNames.mid(0, 20).join(QStringLiteral(", ")),
                        currents.isEmpty() ? QString() : tr("; its currents: %1").arg(currents.mid(0, 20).join(QStringLiteral(", "))));
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

// A measurement 'hold' keeps within bounds while tune moves the values:
// {"measure": {...}, "min": 50e3, "max": ...}.
struct Hold {
    QJsonObject spec;
    double min = -INFINITY, max = INFINITY;
    bool keeps(double v) const { return v >= min && v <= max; }
    // "ac.v(out) bandwidth 42.1k, below 50k"
    QString broken(const QString& name, double v) const
    {
        return v < min ? tr("%1 is %2, below %3").arg(name).arg(v, 0, 'g', 4).arg(min, 0, 'g', 4)
                       : tr("%1 is %2, above %3").arg(name).arg(v, 0, 'g', 4).arg(max, 0, 'g', 4);
    }
};

// tune's 'hold', read: all of the operating point, or all of the analyses,
// as the targets are (one run gives them all).
bool readHolds(const QJsonObject& args, bool atOperatingPoint, QList<Hold>* holds, QString* error)
{
    for (const QJsonValue& v : args.value(QLatin1String("hold")).toArray()) {
        const QJsonObject item = v.toObject();
        Hold h;
        h.spec = item.value(QLatin1String("measure")).toObject();
        const bool op = h.spec.contains(QLatin1String("operating_point"));
        if ((!op && h.spec.value(QLatin1String("variable")).toString().trimmed().isEmpty())
            || (!item.value(QLatin1String("min")).isDouble() && !item.value(QLatin1String("max")).isDouble())) {
            *error = tr("Each of 'hold' is {\"measure\": {...as 'measure'}, \"min\": 50e3} - a 'min', a 'max' or both.");
            return false;
        }
        if (op != atOperatingPoint) {
            *error = tr("'hold' measures what the target measures: all of the operating point, or all of the analyses' "
                        "dataset (one run gives them all).");
            return false;
        }
        if (item.value(QLatin1String("min")).isDouble()) h.min = item.value(QLatin1String("min")).toDouble();
        if (item.value(QLatin1String("max")).isDouble()) h.max = item.value(QLatin1String("max")).toDouble();
        if (h.min > h.max) {
            *error = tr("A hold's 'min' is above its 'max'.");
            return false;
        }
        *holds << h;
    }
    return true;
}

} // namespace

double QucsControl::measureRun(const QJsonObject& spec, const QJsonObject& simulated, const QString& path, const QJsonObject& args,
                               QString* used, QString* why)
{
    QJsonObject answer = simulated;
    if (!spec.contains(QLatin1String("operating_point"))) {
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
            *why = tr("the result could not be read: %1").arg(textOf(got));
            return NAN;
        }
        answer = QJsonDocument::fromJson(textOf(got).toUtf8()).object();
    }
    return measured(spec, answer, used, why);
}

// Several knobs at once, for as many targets (Rf and Rg for a gain and an
// input resistance): Broyden's method on each value's place in its range -
// on its logarithm across decades - from the middle; the first Jacobian by
// a run with each knob moved, then each run's change folded in, a step
// that makes it worse halved. The values found are set as one step to undo.
void QucsControl::tuneKnobs(const QJsonObject& args, const Done& done)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) {
        done(errorResult(error));
        return;
    }
    // (Run after run: its commands not unasked, as simulate's.)
    if (const QString refused = commandsRefused(sch, args); !refused.isEmpty()) {
        done(errorResult(refused));
        return;
    }
    struct Knob {
        QString name, property, was, unit;
        double lo = 0, hi = 0;
        bool log = false;
    };
    struct Target {
        QJsonObject spec;
        double value = 0, tolerance = 0;
    };
    QList<Knob> knobs;
    QList<Target> targets;
    const QJsonArray knobArgs = args.value(QLatin1String("knobs")).toArray(), targetArgs = args.value(QLatin1String("targets")).toArray();
    if (knobArgs.size() < 2 || knobArgs.size() > 4 || targetArgs.size() != knobArgs.size()) {
        done(errorResult(tr("'knobs' are 2 to 4 parts, [{\"component\": \"RF\", \"range\": [\"1k\", \"100k\"]}, ...], and 'targets' as "
                            "many, [{\"measure\": {...}, \"target\": 20}, ...]: one target for each knob.")));
        return;
    }
    for (const QJsonValue& v : knobArgs) {
        const QJsonObject knob = v.toObject();
        Knob k;
        k.name = knob.value(QLatin1String("component")).toString().trimmed();
        Component* c = sch->getComponentByName(k.name);
        if (c == nullptr) {
            done(errorResult(tr("There is no component %1 in %2 (knobs: 'component').").arg(k.name, titleOf(sch))));
            return;
        }
        k.property = knob.value(QLatin1String("property")).toString().trimmed();
        if (k.property.isEmpty() && !c->Props.isEmpty()) k.property = c->Props.first()->Name;
        if (c->getProperty(k.property) == nullptr) {
            done(errorResult(tr("%1 has no property %2.").arg(k.name, k.property)));
            return;
        }
        for (const Knob& other : std::as_const(knobs))
            if (other.name == k.name && other.property == k.property) {
                done(errorResult(tr("%1's %2 is a knob twice.").arg(k.name, k.property)));
                return;
            }
        const QJsonArray range = knob.value(QLatin1String("range")).toArray();
        if (range.size() != 2 || !valueOf(range.at(0), &k.lo) || !valueOf(range.at(1), &k.hi) || k.lo == k.hi) {
            done(errorResult(tr("Knob %1: 'range' is [low, high], numbers or 4.7k.").arg(k.name)));
            return;
        }
        if (k.lo > k.hi) std::swap(k.lo, k.hi);
        k.was = c->getProperty(k.property)->Value;
        k.unit = baseUnit(k.was);
        k.log = k.lo > 0 && k.hi / k.lo >= 100;
        knobs << k;
    }
    bool atOperatingPoint = false, anyDataset = false;
    for (const QJsonValue& v : targetArgs) {
        const QJsonObject item = v.toObject();
        Target t;
        t.spec = item.value(QLatin1String("measure")).toObject();
        if (!item.value(QLatin1String("target")).isDouble()
            || (!t.spec.contains(QLatin1String("operating_point")) && t.spec.value(QLatin1String("variable")).toString().trimmed().isEmpty())) {
            done(errorResult(tr("Each of 'targets' is {\"measure\": {...as tune's 'measure'}, \"target\": 20, \"tolerance\": 0.1}.")));
            return;
        }
        t.value = item.value(QLatin1String("target")).toDouble();
        t.tolerance = item.value(QLatin1String("tolerance")).isDouble() ? std::abs(item.value(QLatin1String("tolerance")).toDouble())
                                                                      : std::max(1e-12, std::abs(t.value) * 0.005);
        (t.spec.contains(QLatin1String("operating_point")) ? atOperatingPoint : anyDataset) = true;
        targets << t;
    }
    if (atOperatingPoint && anyDataset) {
        done(errorResult(tr("The targets are all of the operating point, or all of the analyses' dataset: one run gives them.")));
        return;
    }
    const int most = std::clamp(args.value(QLatin1String("max_runs")).toInt(24), 4, 60);
    const bool apply = args.value(QLatin1String("apply")).toBool(true);
    const int timeout = std::clamp(args.value(QLatin1String("timeout")).toInt(120), 5, 3600);
    QList<Hold> holds;
    if (!readHolds(args, atOperatingPoint, &holds, &error)) {
        done(errorResult(error));
        return;
    }
    const bool compare = args.value(QLatin1String("compare")).toBool() || !holds.isEmpty();
    QString savedNote;
    if (sch->getDocName().isEmpty() && !saveInScratch(sch, &savedNote, &error)) {
        done(errorResult(error));
        return;
    }
    QPointer<Schematic> doc(sch);
    const QString path = sch->getDocName();
    const int n = int(knobs.size());

    // A place in the ranges (0 to 1 each) as values, and back.
    const auto valueAt = [knobs](int i, double u) {
        const Knob& k = knobs.at(i);
        return k.log ? std::exp(std::log(k.lo) + u * (std::log(k.hi) - std::log(k.lo))) : k.lo + u * (k.hi - k.lo);
    };
    const auto setValues = [doc, knobs](const QStringList& texts) {
        if (!doc) return;
        for (int i = 0; i < knobs.size(); ++i)
            if (Component* c = doc->getComponentByName(knobs.at(i).name); c != nullptr && c->getProperty(knobs.at(i).property) != nullptr) {
                c->getProperty(knobs.at(i).property)->Value = texts.at(i);
                doc->recreateComponent(c);
            }
        doc->viewport()->update();
    };
    struct Run {
        QStringList texts;
        QList<double> measured;
        double worst = INFINITY;   // (the largest miss, in tolerances)
        QList<double> held;        // the hold measurements
    };
    struct State {
        QList<Run> runs;
        QStringList targetNames, holdNames;     // what each target and hold measured
        int before = -1;                        // the run of the values as they were ('compare')
        bool beforeTried = false;
        std::vector<double> u, f;               // where it is, its misses (scaled)
        std::vector<std::vector<double>> J;     // d miss / d place
        int column = 0;                         // the first Jacobian: the knob moved next
        std::vector<double> tried;              // a step being halved
        int halvings = 0;
    };
    auto state = std::make_shared<State>();
    state->u.assign(n, 0.5);
    const auto missOf = [targets](const QList<double>& m) {
        std::vector<double> f;
        for (int j = 0; j < targets.size(); ++j)
            f.push_back((m.at(j) - targets.at(j).value) / std::max(std::abs(targets.at(j).value), targets.at(j).tolerance));
        return f;
    };
    auto finishUp = std::make_shared<std::function<void(const QString&)>>();
    // A run of the values as written, measured.
    auto evaluateTexts = std::make_shared<std::function<void(const QStringList&, std::function<void(const QList<double>&)>)>>();
    auto evaluate = std::make_shared<std::function<void(const std::vector<double>&, std::function<void(const QList<double>&)>)>>();
    *evaluate = [=](const std::vector<double>& u, std::function<void(const QList<double>&)> then) {
        QStringList texts;
        for (int i = 0; i < n; ++i) texts << valueText(valueAt(i, std::clamp(u[i], 0.0, 1.0)), knobs.at(i).unit);
        (*evaluateTexts)(texts, then);
    };
    *evaluateTexts = [=, this](const QStringList& texts, std::function<void(const QList<double>&)> then) {
        if (!doc) {
            (*finishUp)(tr("The schematic was closed."));
            return;
        }
        setValues(texts);
        QJsonObject run{{QStringLiteral("path"), path}, {QStringLiteral("timeout"), timeout}};
        if (atOperatingPoint) run.insert(QStringLiteral("operating_point"), true);
        if (args.contains(QLatin1String("simulator"))) run.insert(QStringLiteral("simulator"), args.value(QLatin1String("simulator")));
        if (args.value(QLatin1String("allow_commands")).toBool()) run.insert(QStringLiteral("allow_commands"), true);
        simulate(run, [=, this](const QJsonObject& r) {
            const QJsonObject result = QJsonDocument::fromJson(textOf(r).section(QLatin1Char('\n'), -1).toUtf8()).object();
            QStringList said;
            for (int i = 0; i < n; ++i) said << QStringLiteral("%1 = %2").arg(knobs.at(i).name, texts.at(i));
            if (r.value(QStringLiteral("isError")).toBool() || !result.value(QStringLiteral("succeeded")).toBool(true) || result.isEmpty()) {
                const QJsonArray errors = result.value(QStringLiteral("errors")).toArray();
                (*finishUp)(tr("The simulation with %1 failed: %2").arg(said.join(QStringLiteral(", ")),
                                                                     errors.isEmpty() ? textOf(r).left(600)
                                                                                      : errors.first().toObject().value(QStringLiteral("message")).toString()));
                return;
            }
            QList<double> m;
            for (int j = 0; j < targets.size(); ++j) {
                QString why, used;
                const double value = measureRun(targets.at(j).spec, result, path, args, &used, &why);
                if (std::isnan(value)) {
                    (*finishUp)(tr("With %1 nothing could be measured: %2.").arg(said.join(QStringLiteral(", ")), why));
                    return;
                }
                m << value;
                if (state->targetNames.size() <= j) state->targetNames << used;
            }
            QList<double> held;
            for (int j = 0; j < holds.size(); ++j) {
                QString why, used;
                const double value = measureRun(holds.at(j).spec, result, path, args, &used, &why);
                if (std::isnan(value)) {
                    (*finishUp)(tr("With %1 hold %2 could not be measured: %3.").arg(said.join(QStringLiteral(", "))).arg(j + 1).arg(why));
                    return;
                }
                held << value;
                if (state->holdNames.size() <= j) state->holdNames << used;
            }
            Run run{texts, m};
            run.held = held;
            run.worst = 0;
            for (int j = 0; j < targets.size(); ++j)
                run.worst = std::max(run.worst, std::abs(m.at(j) - targets.at(j).value) / targets.at(j).tolerance);
            state->runs << run;
            then(m);
        });
    };

    auto step = std::make_shared<std::function<void()>>();
    *step = [=, this]() {
        // First the values as they are, for 'compare' (and 'hold').
        if (compare && !state->beforeTried) {
            state->beforeTried = true;
            QStringList was;
            for (const Knob& k : std::as_const(knobs)) was << k.was;
            (*evaluateTexts)(was, [state, step](const QList<double>&) {
                state->before = int(state->runs.size()) - 1;
                (*step)();
            });
            return;
        }
        if (!state->runs.isEmpty() && state->runs.last().worst <= 1) {
            (*finishUp)(QString());
            return;
        }
        if (state->runs.size() >= most) {
            (*finishUp)(tr("%1 runs, the most asked for ('max_runs'), without every target within its tolerance.").arg(most));
            return;
        }
        // Where it starts: the middle of every range.
        if (state->f.empty()) {
            (*evaluate)(state->u, [=](const QList<double>& m) {
                state->f = missOf(m);
                state->J.assign(n, std::vector<double>(n, 0.0));
                (*step)();
            });
            return;
        }
        // The first Jacobian: each knob moved a tenth of its range.
        if (state->column < n) {
            const int i = state->column;
            std::vector<double> moved = state->u;
            const double h = moved[i] + 0.1 <= 1 ? 0.1 : -0.1;
            moved[i] += h;
            (*evaluate)(moved, [=](const QList<double>& m) {
                const std::vector<double> f = missOf(m);
                for (int j = 0; j < n; ++j) state->J[j][i] = (f[j] - state->f[j]) / h;
                ++state->column;
                (*step)();
            });
            return;
        }
        // Newton's step on the Jacobian as it is (Gaussian elimination), at
        // most half the ranges, kept in them.
        std::vector<double> du = state->tried;
        if (du.empty()) {
            std::vector<std::vector<double>> a = state->J;
            std::vector<double> b(n);
            for (int j = 0; j < n; ++j) b[j] = -state->f[j];
            for (int c = 0; c < n; ++c) {
                int pivot = c;
                for (int r = c + 1; r < n; ++r)
                    if (std::abs(a[r][c]) > std::abs(a[pivot][c])) pivot = r;
                if (std::abs(a[pivot][c]) < 1e-12) {
                    (*finishUp)(tr("The measurements do not change independently with the knobs here (the Jacobian is singular): "
                                   "other knobs, or ranges, or targets."));
                    return;
                }
                std::swap(a[c], a[pivot]);
                std::swap(b[c], b[pivot]);
                for (int r = c + 1; r < n; ++r) {
                    const double k = a[r][c] / a[c][c];
                    for (int cc = c; cc < n; ++cc) a[r][cc] -= k * a[c][cc];
                    b[r] -= k * b[c];
                }
            }
            du.assign(n, 0.0);
            for (int c = n - 1; c >= 0; --c) {
                double s = b[c];
                for (int cc = c + 1; cc < n; ++cc) s -= a[c][cc] * du[cc];
                du[c] = s / a[c][c];
            }
            double largest = 0;
            for (double d : du) largest = std::max(largest, std::abs(d));
            if (largest > 0.5)
                for (double& d : du) d *= 0.5 / largest;
        }
        std::vector<double> next(n);
        for (int i = 0; i < n; ++i) next[i] = std::clamp(state->u[i] + du[i], 0.0, 1.0);
        for (int i = 0; i < n; ++i) du[i] = next[i] - state->u[i];
        double moved = 0;
        for (double d : du) moved += d * d;
        if (moved < 1e-14) {
            (*finishUp)(tr("It went to the end of a range and could go no further: widen the ranges ('runs' shows the way it went)."));
            return;
        }
        (*evaluate)(next, [=](const QList<double>& m) {
            const std::vector<double> f = missOf(m);
            double before = 0, after = 0;
            for (int j = 0; j < n; ++j) {
                before += state->f[j] * state->f[j];
                after += f[j] * f[j];
            }
            // Broyden: the Jacobian corrected by what this step did.
            std::vector<double> jdu(n, 0.0);
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i) jdu[j] += state->J[j][i] * du[i];
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i) state->J[j][i] += ((f[j] - state->f[j]) - jdu[j]) * du[i] / moved;
            // Worse: the step again, half as long (twice at most); better, or
            // halved enough: taken.
            if (after > before && state->halvings < 2) {
                ++state->halvings;
                state->tried = du;
                for (double& d : state->tried) d /= 2;
            } else {
                state->halvings = 0;
                state->tried.clear();
                state->u = next;
                state->f = f;
            }
            (*step)();
        });
    };

    *finishUp = [=, this](const QString& stopped) {
        if (!doc) {
            done(errorResult(tr("The schematic was closed while it was tuned.")));
            return;
        }
        // The best run: the nearest the targets of those that keep every hold.
        const auto keeps = [&](const Run& r) {
            for (int j = 0; j < holds.size(); ++j)
                if (j >= r.held.size() || !holds.at(j).keeps(r.held.at(j))) return false;
            return true;
        };
        int best = -1, closest = -1;
        for (int r = 0; r < state->runs.size(); ++r) {
            if (closest < 0 || state->runs.at(r).worst < state->runs.at(closest).worst) closest = r;
            if (keeps(state->runs.at(r)) && (best < 0 || state->runs.at(r).worst < state->runs.at(best).worst)) best = r;
        }
        QStringList was;
        for (const Knob& k : std::as_const(knobs)) was << k.was;
        setValues(was);
        QJsonArray runs;
        for (int r = 0; r < state->runs.size(); ++r) {
            const Run& run = state->runs.at(r);
            QJsonObject values, got;
            for (int i = 0; i < n; ++i) values.insert(knobs.at(i).name, run.texts.at(i));
            QJsonArray measures;
            for (double m : run.measured) measures.append(rounded(m));
            QJsonObject row{{QStringLiteral("values"), values}, {QStringLiteral("measured"), measures}};
            if (!holds.isEmpty()) {
                QJsonArray hv;
                for (double v : run.held) hv.append(rounded(v));
                row.insert(QStringLiteral("hold"), hv);
                row.insert(QStringLiteral("keeps"), keeps(run));
            }
            if (r == state->before) row.insert(QStringLiteral("as it was"), true);
            runs.append(row);
        }
        QJsonObject result{{QStringLiteral("runs"), runs}};
        if (!savedNote.isEmpty()) result.insert(QStringLiteral("saved"), savedNote);
        if (!stopped.isEmpty()) result.insert(QStringLiteral("stopped"), stopped);
        if (!holds.isEmpty() && closest >= 0 && closest != best && state->runs.at(closest).worst <= 1) {
            QStringList broken, values;
            for (int j = 0; j < holds.size(); ++j)
                if (!holds.at(j).keeps(state->runs.at(closest).held.at(j)))
                    broken << holds.at(j).broken(state->holdNames.value(j), state->runs.at(closest).held.at(j));
            for (int i = 0; i < n; ++i) values << QStringLiteral("%1 = %2").arg(knobs.at(i).name, state->runs.at(closest).texts.at(i));
            result.insert(QStringLiteral("held back"), tr("%1 gives every target, but %2 - not taken").arg(values.join(QStringLiteral(", ")),
                                                                                                           broken.join(QStringLiteral("; "))));
        }
        if (compare && state->before >= 0) {
            const int after = best >= 0 ? best : closest;
            QJsonArray table;
            const auto row = [&](const QString& what, double beforeValue, double afterValue) {
                QJsonObject r{{QStringLiteral("measured"), what}, {QStringLiteral("before"), rounded(beforeValue)}};
                if (after >= 0) {
                    r.insert(QStringLiteral("after"), rounded(afterValue));
                    r.insert(QStringLiteral("change"), rounded(afterValue - beforeValue));
                }
                table.append(r);
            };
            for (int j = 0; j < targets.size(); ++j)
                row(state->targetNames.value(j), state->runs.at(state->before).measured.at(j),
                    after >= 0 ? state->runs.at(after).measured.at(j) : NAN);
            for (int j = 0; j < holds.size(); ++j)
                row(state->holdNames.value(j), state->runs.at(state->before).held.at(j), after >= 0 ? state->runs.at(after).held.at(j) : NAN);
            result.insert(QStringLiteral("before and after"), table);
        }
        if (best < 0) {
            result.insert(QStringLiteral("set"), holds.isEmpty() || closest < 0 ? tr("Nothing changed.")
                                                                                : tr("Nothing changed: no run kept every hold."));
            done(jsonResult(result));
            return;
        }
        const Run& b = state->runs.at(best);
        const bool reached = b.worst <= 1;
        QJsonArray knobsOut, targetsOut;
        for (int i = 0; i < n; ++i)
            knobsOut.append(QJsonObject{{QStringLiteral("component"), knobs.at(i).name}, {QStringLiteral("property"), knobs.at(i).property},
                                        {QStringLiteral("was"), knobs.at(i).was}, {QStringLiteral("value"), b.texts.at(i)}});
        for (int j = 0; j < targets.size(); ++j) {
            const Target& t = targets.at(j);
            targetsOut.append(QJsonObject{{QStringLiteral("measure"), t.spec}, {QStringLiteral("target"), t.value},
                                          {QStringLiteral("gives"), rounded(b.measured.at(j))},
                                          {QStringLiteral("off by"), rounded(b.measured.at(j) - t.value)},
                                          {QStringLiteral("within tolerance"), std::abs(b.measured.at(j) - t.value) <= t.tolerance}});
        }
        result.insert(QStringLiteral("knobs"), knobsOut);
        result.insert(QStringLiteral("targets"), targetsOut);
        if (apply && reached) {
            setValues(b.texts);
            doc->setChanged(true, true);
            result.insert(QStringLiteral("set"), tr("The values found are set: one step to undo."));
        } else {
            result.insert(QStringLiteral("set"), !apply ? tr("not set ('apply' false): the knobs are as they were.")
                                                        : holds.isEmpty()
                                                              ? tr("not set: no run gave every target within its tolerance; the knobs are as "
                                                                   "they were. The closest run is 'knobs' (edit_component sets them).")
                                                              : tr("not set: no run gave every target within its tolerance and kept every "
                                                                   "hold; the knobs are as they were. The closest that keeps them is 'knobs'."));
        }
        // The dataset of the values set (a run again, when the last was of others).
        if (!(apply && reached) || best == state->runs.size() - 1) {
            done(jsonResult(result));
            return;
        }
        QJsonObject run{{QStringLiteral("path"), path}, {QStringLiteral("timeout"), timeout}};
        if (atOperatingPoint) run.insert(QStringLiteral("operating_point"), true);
        if (args.contains(QLatin1String("simulator"))) run.insert(QStringLiteral("simulator"), args.value(QLatin1String("simulator")));
        if (args.value(QLatin1String("allow_commands")).toBool()) run.insert(QStringLiteral("allow_commands"), true);
        simulate(run, [done, result](const QJsonObject&) mutable {
            result.insert(QStringLiteral("dataset"), tr("of the values set (simulated again)"));
            done(jsonResult(result));
        });
    };
    (*step)();
}

void QucsControl::tune(const QJsonObject& args, const Done& done)
{
    if (args.contains(QLatin1String("knobs")) || args.contains(QLatin1String("targets"))) {
        tuneKnobs(args, done);
        return;
    }
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) {
        done(errorResult(error));
        return;
    }
    // (Run after run: its commands not unasked, as simulate's.)
    if (const QString refused = commandsRefused(sch, args); !refused.isEmpty()) {
        done(errorResult(refused));
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
    // The values tried: a list, or a range searched for the target. (A
    // value of the list is set as it is written there - 10 Ohm, .5k - not
    // as a number found is: 10, 500.)
    QList<double> values;
    QStringList spelt;
    for (const QJsonValue& v : args.value(QLatin1String("values")).toArray()) {
        double x = 0;
        if (!valueOf(v, &x)) {
            done(errorResult(tr("%1 is not a value ('values': numbers, or text with units: 4.7k).").arg(QString::fromUtf8(QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact)))));
            return;
        }
        values << x;
        spelt << (v.isString() ? v.toString().trimmed() : QString());
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
    const bool apply = args.value(QLatin1String("apply")).toBool(true);
    const QString was = p->Value;
    const QString unit = baseUnit(was);
    const int timeout = std::clamp(args.value(QLatin1String("timeout")).toInt(120), 5, 3600);
    // What must stay within bounds; the measurements before and after (a
    // run of the value as it is first).
    QList<Hold> holds;
    if (!readHolds(args, atOperatingPoint, &holds, &error)) {
        done(errorResult(error));
        return;
    }
    const bool compare = args.value(QLatin1String("compare")).toBool() || !holds.isEmpty();

    struct State {
        QList<std::pair<double, double>> runs;   // value, measured
        QStringList texts;                       // each run's value as it was set
        QString used;                            // the variable measured
        QStringList notes;
        double a = 0, ma = NAN, b = 0, mb = NAN; // the bracket: its values and what they measured
        double wa = 1, wb = 1;                   // (Illinois: the weight of an end kept twice, halved)
        int side = 0;
        // The scales the secant is drawn on: the value's or its logarithm's
        // (logX), the measurement's or its logarithm's (logM) - a gain over
        // decades of a resistance goes as the logarithm of the value, a
        // bandwidth as a power of it. At first, the logarithms when the
        // range is two decades or more (and, for the measurement, when both
        // ends measured more than 0 and a decade apart); then whichever of
        // the four foretold the last run best.
        bool logX = false, logM = false;
        int next = 0;                            // the index of 'values' to try next
        QString lastSet;                         // the value it set last (a run that failed too)
        QList<QList<double>> held;               // each run's hold measurements
        QStringList holdNames;                   // what each hold measured
        int before = -1;                         // the run of the value as it was ('compare')
        bool beforeTried = false;
    };
    // Untitled: saved in the scratch folder first (each run simulates the
    // file's schematic), and said.
    QString savedNote;
    if (sch->getDocName().isEmpty() && !saveInScratch(sch, &savedNote, &error)) {
        done(errorResult(error));
        return;
    }
    auto state = std::make_shared<State>();
    state->logX = lo > 0 && hi / lo >= 100;
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
    auto evaluate = std::make_shared<std::function<void(double, std::function<void(double)>, QString)>>();
    *evaluate = [=, this](double x, std::function<void(double)> then, QString text) {
        if (!doc) {
            (*finishUp)(tr("The schematic was closed."));
            return;
        }
        if (text.isEmpty()) text = valueText(x, unit);
        setValue(text);
        state->lastSet = text;
        QJsonObject run{{QStringLiteral("path"), path}, {QStringLiteral("timeout"), timeout}};
        if (atOperatingPoint) run.insert(QStringLiteral("operating_point"), true);
        if (args.contains(QLatin1String("simulator"))) run.insert(QStringLiteral("simulator"), args.value(QLatin1String("simulator")));
        if (args.value(QLatin1String("allow_commands")).toBool()) run.insert(QStringLiteral("allow_commands"), true);
        simulate(run, [=, this](const QJsonObject& r) {
            const QJsonObject result = QJsonDocument::fromJson(textOf(r).section(QLatin1Char('\n'), -1).toUtf8()).object();
            QJsonObject answer = result;
            if (r.value(QStringLiteral("isError")).toBool() || !result.value(QStringLiteral("succeeded")).toBool(true)
                || result.isEmpty()) {
                const QJsonArray errors = result.value(QStringLiteral("errors")).toArray();
                (*finishUp)(tr("The simulation with %1 = %2 failed: %3").arg(property, text,
                                                                               errors.isEmpty() ? textOf(r).left(600)
                                                                                                : errors.first().toObject().value(QStringLiteral("message")).toString()));
                return;
            }
            QString why, used;
            const double m = measureRun(spec, answer, path, args, &used, &why);
            if (std::isnan(m)) {
                (*finishUp)(tr("With %1 = %2 nothing could be measured: %3.").arg(property, text, why));
                return;
            }
            QList<double> held;
            for (int j = 0; j < holds.size(); ++j) {
                QString holdUsed, holdWhy;
                const double v = measureRun(holds.at(j).spec, answer, path, args, &holdUsed, &holdWhy);
                if (std::isnan(v)) {
                    (*finishUp)(tr("With %1 = %2 hold %3 could not be measured: %4.").arg(property, text).arg(j + 1).arg(holdWhy));
                    return;
                }
                held << v;
                if (state->holdNames.size() <= j) state->holdNames << holdUsed;
            }
            if (state->used.isEmpty()) state->used = used;
            state->runs.append({x, m});
            state->texts.append(text);
            state->held.append(held);
            then(m);
        });
    };

    // The search.
    auto step = std::make_shared<std::function<void()>>();
    *step = [=]() {
        // First the value as it is, for 'compare' (and 'hold').
        if (compare && !state->beforeTried) {
            state->beforeTried = true;
            double x = 0;
            if (valueOf(QJsonValue(was), &x)) {
                (*evaluate)(x, [state, step](double) {
                    state->before = int(state->runs.size()) - 1;
                    (*step)();
                }, was);
                return;
            }
            state->notes << tr("no run of the value as it was: %1 is not a number").arg(was);
        }
        if (!values.isEmpty()) {
            if (state->next >= values.size()) {
                (*finishUp)(QString());
                return;
            }
            const QString text = spelt.at(state->next);
            const double x = values.at(state->next++);
            (*evaluate)(x, [step](double) { (*step)(); }, text);
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
        if (std::isnan(state->ma)) {
            (*evaluate)(lo, [=](double m) {
                state->a = lo;
                state->ma = m;
                (*step)();
            }, QString());
            return;
        }
        if (std::isnan(state->mb)) {
            (*evaluate)(hi, [=](double m) {
                state->b = hi;
                state->mb = m;
                const double fa = state->ma - target, fb = m - target;
                if ((fa > 0) == (fb > 0) && std::abs(fa) > tolerance && std::abs(fb) > tolerance) {
                    (*finishUp)(tr("The target is not between what the range's ends give (%1 at %2, %3 at %4): widen the range, or "
                                   "look at 'runs' for the way it goes.")
                                    .arg(state->ma).arg(valueText(lo, unit)).arg(m).arg(valueText(hi, unit)));
                    return;
                }
                state->logM = state->logX && state->ma > 0 && m > 0 && target > 0
                              && std::max(state->ma, m) / std::min(state->ma, m) >= 10;
                (*step)();
            }, QString());
            return;
        }
        // False position with the Illinois rule, on the scales that foretell
        // the measurements best. (On the value's logarithm alone, a divider's
        // voltage over its source took six runs, not three; a bandwidth over
        // a capacitance nine, not three.)
        const bool logX = state->logX && state->a > 0 && state->b > 0;
        const bool logM = state->logM && state->ma > 0 && state->mb > 0 && target > 0;
        const auto u = [logX](double v) { return logX ? std::log(v) : v; };
        const auto g = [logM, target](double m) { return logM ? std::log(m) - std::log(target) : m - target; };
        const double ga = g(state->ma) * state->wa, gb = g(state->mb) * state->wb;
        double t = (u(state->a) * gb - u(state->b) * ga) / (gb - ga);
        if (!std::isfinite(t) || t <= std::min(u(state->a), u(state->b)) || t >= std::max(u(state->a), u(state->b)))
            t = (u(state->a) + u(state->b)) / 2;
        const double x = logX ? std::exp(t) : t;
        const double a0 = state->a, b0 = state->b, ma0 = state->ma, mb0 = state->mb;
        (*evaluate)(x, [=](double m) {
            // What each pair of scales foretold here, between the two ends as
            // they measured: the nearest is taken from now on.
            double nearest = INFINITY;
            for (const bool lx : {false, true})
                for (const bool lm : {false, true}) {
                    if ((lx && (a0 <= 0 || b0 <= 0 || x <= 0)) || (lm && (ma0 <= 0 || mb0 <= 0 || m <= 0)) || a0 == b0) continue;
                    const auto sx = [lx](double v) { return lx ? std::log(v) : v; };
                    const auto sm = [lm](double v) { return lm ? std::log(v) : v; };
                    const double on = sm(ma0) + (sm(mb0) - sm(ma0)) * (sx(x) - sx(a0)) / (sx(b0) - sx(a0));
                    const double foretold = lm ? std::exp(on) : on;
                    if (std::isfinite(foretold) && std::abs(foretold - m) < nearest) {
                        nearest = std::abs(foretold - m);
                        state->logX = lx;
                        state->logM = lm;
                    }
                }
            // The end on its side is replaced; the other, kept twice in a
            // row, counts half (so it does not hold the search back).
            if ((m > target) == (state->mb > target)) {
                state->b = x;
                state->mb = m;
                state->wb = 1;
                if (state->side == -1) state->wa /= 2;
                state->side = -1;
            } else {
                state->a = x;
                state->ma = m;
                state->wa = 1;
                if (state->side == 1) state->wb /= 2;
                state->side = 1;
            }
            (*step)();
        }, QString());
    };

    *finishUp = [=, this](const QString& stopped) {
        if (!doc) {
            done(errorResult(tr("The schematic was closed while it was tuned.")));
            return;
        }
        // The best value: the one closest to the target that keeps every
        // hold (or, for a list without a target, none - the table is the
        // answer).
        const auto keeps = [&](int i) {
            for (int j = 0; j < holds.size(); ++j)
                if (i >= state->held.size() || j >= state->held.at(i).size() || !holds.at(j).keeps(state->held.at(i).at(j))) return false;
            return true;
        };
        int best = -1, closest = -1;
        for (int i = 0; i < state->runs.size(); ++i) {
            if (!hasTarget) continue;
            const double off = std::abs(state->runs.at(i).second - target);
            if (closest < 0 || off < std::abs(state->runs.at(closest).second - target)) closest = i;
            if (keeps(i) && (best < 0 || off < std::abs(state->runs.at(best).second - target))) best = i;
        }
        QJsonArray runs;
        for (int i = 0; i < state->runs.size(); ++i) {
            QJsonObject row{{QStringLiteral("value"), state->texts.at(i)}, {QStringLiteral("measured"), rounded(state->runs.at(i).second)}};
            if (!holds.isEmpty()) {
                QJsonArray hv;
                for (double v : state->held.at(i)) hv.append(rounded(v));
                row.insert(QStringLiteral("hold"), hv);
                row.insert(QStringLiteral("keeps"), keeps(i));
            }
            if (i == state->before) row.insert(QStringLiteral("as it was"), true);
            runs.append(row);
        }
        // (The best value as it was set: a value of 'values' as written.)
        const QString bestText = best >= 0 ? state->texts.at(best) : QString();
        QJsonObject result{{QStringLiteral("component"), name},
                           {QStringLiteral("property"), property},
                           {QStringLiteral("was"), was},
                           {QStringLiteral("measured"), state->used.isEmpty() ? spec.value(QLatin1String("variable")).toString() : state->used},
                           {QStringLiteral("runs"), runs}};
        if (hasTarget) result.insert(QStringLiteral("target"), target);
        if (!savedNote.isEmpty()) result.insert(QStringLiteral("saved"), savedNote);
        if (!holds.isEmpty()) {
            QJsonArray kept;
            for (int j = 0; j < holds.size(); ++j) {
                QJsonObject h{{QStringLiteral("measured"), state->holdNames.value(j, holds.at(j).spec.value(QLatin1String("variable")).toString())}};
                if (std::isfinite(holds.at(j).min)) h.insert(QStringLiteral("min"), holds.at(j).min);
                if (std::isfinite(holds.at(j).max)) h.insert(QStringLiteral("max"), holds.at(j).max);
                kept.append(h);
            }
            result.insert(QStringLiteral("hold"), kept);
            // The target reached, but not keeping what it was to keep: said.
            if (closest >= 0 && closest != best && std::abs(state->runs.at(closest).second - target) <= tolerance) {
                QStringList broken;
                for (int j = 0; j < holds.size(); ++j)
                    if (!holds.at(j).keeps(state->held.at(closest).at(j)))
                        broken << holds.at(j).broken(state->holdNames.value(j), state->held.at(closest).at(j));
                // (The part by its name; the property too when it is not its first.)
                const Component* part = doc->getComponentByName(name);
                const QString what = part != nullptr && !part->Props.isEmpty() && part->Props.first()->Name == property
                                         ? name : QStringLiteral("%1.%2").arg(name, property);
                result.insert(QStringLiteral("held back"), tr("%1 = %2 gives the target, but %3 - not taken").arg(what, state->texts.at(closest),
                                                                                                                  broken.join(QStringLiteral("; "))));
            }
        }
        // Every measurement, as it was and with the value found.
        if (compare && state->before >= 0) {
            const int after = best >= 0 ? best : closest;
            QJsonArray table;
            const auto row = [&](const QString& what, double beforeValue, double afterValue) {
                QJsonObject r{{QStringLiteral("measured"), what}, {QStringLiteral("before"), rounded(beforeValue)}};
                if (after >= 0) {
                    r.insert(QStringLiteral("after"), rounded(afterValue));
                    r.insert(QStringLiteral("change"), rounded(afterValue - beforeValue));
                }
                table.append(r);
            };
            row(state->used, state->runs.at(state->before).second, after >= 0 ? state->runs.at(after).second : NAN);
            for (int j = 0; j < holds.size(); ++j)
                row(state->holdNames.value(j), state->held.at(state->before).at(j), after >= 0 ? state->held.at(after).at(j) : NAN);
            result.insert(QStringLiteral("before and after"), table);
        }
        if (!state->notes.isEmpty()) result.insert(QStringLiteral("notes"), QJsonArray::fromStringList(state->notes));
        // Changed while it was tuned (the user, in the window): left as they
        // made it - not put back to what it was, and nothing applied over it.
        const QString lastSet = state->lastSet.isEmpty() ? was : state->lastSet;
        QString now = lastSet;
        if (Component* comp = doc->getComponentByName(name); comp != nullptr && comp->getProperty(property) != nullptr)
            now = comp->getProperty(property)->Value;
        const bool changedMeanwhile = now != lastSet;
        // Back to where it was; the value found is one step to undo - when
        // it gives the target (the closest of a search that missed it is
        // told, not set).
        if (!changedMeanwhile) setValue(was);
        const bool reached = best >= 0 && std::abs(state->runs.at(best).second - target) <= tolerance;
        const bool applying = apply && reached && !changedMeanwhile;
        if (changedMeanwhile) {
            if (best >= 0) {
                const double m = state->runs.at(best).second;
                result.insert(QStringLiteral("value"), bestText);
                result.insert(QStringLiteral("gives"), rounded(m));
            }
            result.insert(QStringLiteral("set"), tr("%1 of %2 was changed to %3 while it was tuned: left so, not put back to %4, and "
                                                    "nothing applied over it.").arg(property, name, now, was));
        } else if (applying) {
            const double m = state->runs.at(best).second;
            setValue(bestText);
            doc->setChanged(true, true);
            result.insert(QStringLiteral("value"), bestText);
            result.insert(QStringLiteral("gives"), rounded(m));
            result.insert(QStringLiteral("off by"), rounded(m - target));
            result.insert(QStringLiteral("within tolerance"), std::abs(m - target) <= tolerance);
            result.insert(QStringLiteral("set"), tr("%1 of %2 is %3 now: one step to undo (it was %4).").arg(property, name, bestText, was));
        } else if (best >= 0 && apply) {
            const double m = state->runs.at(best).second;
            result.insert(QStringLiteral("value"), bestText);
            result.insert(QStringLiteral("gives"), rounded(m));
            result.insert(QStringLiteral("off by"), rounded(m - target));
            result.insert(QStringLiteral("within tolerance"), false);
            result.insert(QStringLiteral("set"), tr("not set: no value tried gave the target, within %1%6; %2 is %3 as it was. The closest%7, "
                                                    "%4, gives %5 (edit_component sets it).")
                                                     .arg(rounded(tolerance)).arg(property, was, bestText).arg(rounded(m))
                                                     .arg(holds.isEmpty() ? QString() : tr(", keeping every hold"),
                                                          holds.isEmpty() ? QString() : tr(" that keeps them")));
        } else if (best >= 0) {
            const double m = state->runs.at(best).second;
            result.insert(QStringLiteral("value"), bestText);
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
                result.insert(QStringLiteral("note"), tr("The dataset is the last run's (%1 = %2).").arg(property, state->texts.last()));
            done(jsonResult(result));
            return;
        }
        QJsonObject run{{QStringLiteral("path"), path}, {QStringLiteral("timeout"), timeout}};
        if (atOperatingPoint) run.insert(QStringLiteral("operating_point"), true);
        if (args.contains(QLatin1String("simulator"))) run.insert(QStringLiteral("simulator"), args.value(QLatin1String("simulator")));
        if (args.value(QLatin1String("allow_commands")).toBool()) run.insert(QStringLiteral("allow_commands"), true);
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

namespace {

// What the run of every library part under ngspice found
// (scripts/ci/test-library-parts.py writes it beside the libraries): by
// "library/part", its outcome - read again when the file changes.
struct Tested {
    QString date, ngspice;
    QHash<QString, QJsonObject> parts;
};
const Tested& testedParts()
{
    static Tested tested;
    static QDateTime read;
    static QString from;
    const QString file = QDir(QucsSettings.LibDir).filePath(QStringLiteral("ngspice-tested.json"));
    const QDateTime changed = QFileInfo(file).lastModified();
    if (file == from && changed == read) return tested;
    from = file;
    read = changed;
    tested = Tested{};
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return tested;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    tested.date = o.value(QLatin1String("tested")).toString();
    tested.ngspice = o.value(QLatin1String("ngspice")).toString();
    const QJsonObject parts = o.value(QLatin1String("parts")).toObject();
    for (auto it = parts.begin(); it != parts.end(); ++it) tested.parts.insert(it.key(), it.value().toObject());
    return tested;
}

// A bench's numbers as a reader wants them: each with its unit and what
// it is to be ("follower of 1 V: 0.999 V (expected 1 V)"; "at 9 uA: Vbe
// 0.609 V (0.1 to 1.6 V), beta 20.4 (3 to 5000), Vce 9.808 V"). The ranges
// are those of scripts/ci/test-library-parts.py. (A transistor's are one
// object per bias point: they read as 0.)
QString benchNumbers(const QString& kind, const QJsonObject& measured)
{
    static const QRegularExpression follower(QStringLiteral("^follower of ([0-9.]+) V$"));
    static const QRegularExpression gain(QStringLiteral("^gain of ([0-9.]+) of ([0-9.]+) V$"));
    const auto n = [](double v) { return QString::number(v, 'g', 4); };
    const bool fet = kind == QLatin1String("nfet") || kind == QLatin1String("pfet");
    const auto one = [&](const QString& key, const QJsonValue& value) -> QString {
        if (value.isBool()) return key;
        const double v = value.toDouble();
        if (const QRegularExpressionMatch m = follower.match(key); m.hasMatch())
            return tr("%1: %2 V (expected %3 V)").arg(key, QString::number(v, 'f', 3), n(m.captured(1).toDouble()));
        if (const QRegularExpressionMatch m = gain.match(key); m.hasMatch())
            return tr("%1: %2 V (expected %3 V)").arg(key, QString::number(v, 'f', 3), n(m.captured(1).toDouble() * m.captured(2).toDouble()));
        if (key == QLatin1String("Vbe")) return tr("Vbe %1 V (0.1 to 1.6 V)").arg(n(v));
        if (key == QLatin1String("beta")) return tr("beta %1 (3 to 5000)").arg(n(v));
        if (key == QLatin1String("Vce")) return tr("Vce %1 V").arg(n(v));
        if (key == QLatin1String("Id mA")) return tr("Id %1 mA (%2 to 10 mA)").arg(n(v), fet ? QStringLiteral("1") : QStringLiteral("0.001"));
        if (key == QLatin1String("Vf")) return tr("Vf %1 V (0.1 to 4.5 V)").arg(n(v));
        return QStringLiteral("%1 %2").arg(key, n(v));
    };
    // A transistor's bias points in the order the bench tried them, 9 uA
    // then 0.9 mA (a JSON object's keys come sorted: "at 0.9 mA" first).
    QStringList keys = measured.keys();
    const auto current = [](const QString& key) {
        const qucs_s::units::Reading r = qucs_s::units::read(key.mid(3));
        return key.startsWith(QLatin1String("at ")) && r.kind == qucs_s::units::Reading::Number ? r.value : -1.0;
    };
    std::stable_sort(keys.begin(), keys.end(), [&](const QString& a, const QString& b) { return current(a) < current(b); });
    QStringList said;
    for (const QString& key : std::as_const(keys)) {
        const QJsonValue value = measured.value(key);
        if (!value.isObject()) {
            said << one(key, value);
            continue;
        }
        // A bias point's: Vbe and beta first (the numbers judged), Vce after.
        const QJsonObject point = value.toObject();
        QStringList here;
        for (const char* k : {"Vbe", "beta", "saturated", "Vce"})
            if (point.contains(QLatin1String(k))) here << one(QLatin1String(k), point.value(QLatin1String(k)));
        for (auto p = point.begin(); p != point.end(); ++p)
            if (!QStringList{QStringLiteral("Vbe"), QStringLiteral("beta"), QStringLiteral("saturated"), QStringLiteral("Vce")}.contains(p.key()))
                here << one(p.key(), p.value());
        // Out of its ranges, as the bench judges them: said - a power part
        // that passes at 0.9 mA has failed at 9 uA.
        const double vbe = point.value(QLatin1String("Vbe")).toDouble(), vce = point.value(QLatin1String("Vce")).toDouble();
        const double beta = point.value(QLatin1String("beta")).toDouble();
        const bool inRange = vbe >= 0.1 && vbe <= 1.6 && vce >= -0.05 && vce <= 10.05
                             && (point.value(QLatin1String("saturated")).toBool() || (beta >= 3 && beta <= 5000));
        said << (inRange ? QStringLiteral("%1: %2") : tr("%1, out of range: %2")).arg(key, here.join(QStringLiteral(", ")));
    }
    return said.join(QStringLiteral("; "));
}

// How the run of every library part under ngspice found \a library's
// \a part, and whether it passed.
QString testedText(const QString& library, const QString& part, bool* passes)
{
    const Tested& tested = testedParts();
    const QJsonObject outcome = tested.parts.value(library + QLatin1Char('/') + part);
    *passes = outcome.value(QLatin1String("passes")).toBool();
    if (outcome.isEmpty()) return tested.parts.isEmpty() ? tr("not tested (no test of the libraries here)") : tr("not tested");
    if (outcome.value(QLatin1String("untested")).toBool()) return tr("not tested - %1").arg(outcome.value(QLatin1String("why")).toString());
    if (!*passes)
        return tr("tested: fails - %1 (%2, ngspice %3)").arg(outcome.value(QLatin1String("why")).toString(), tested.date, tested.ngspice);
    const QString smoke = tr("tested: it netlists and its operating point converges, each pin to ground through 1 MOhm (%1, ngspice %2)")
                              .arg(tested.date, tested.ngspice);
    // The bench of its kind: what it does, its numbers in a range. A part
    // whose bench fails is not taken as working ('tested').
    const QJsonObject bench = outcome.value(QLatin1String("bench")).toObject();
    if (bench.isEmpty() || bench.value(QLatin1String("untested")).toBool()) return smoke + tr(" - not a test of what it does");
    const QString what = bench.value(QLatin1String("bench")).toString();
    const QString numbers = benchNumbers(what.section(QLatin1Char(':'), 0, 0), bench.value(QLatin1String("measured")).toObject());
    if (bench.value(QLatin1String("passes")).toBool()) return smoke + tr("; its bench passes (%1 - %2)").arg(what, numbers);
    *passes = false;
    return smoke + tr("; but its bench FAILS (%1): %2 - the model runs and does the wrong thing")
                       .arg(bench.value(QLatin1String("bench")).toString(), bench.value(QLatin1String("why")).toString());
}

} // namespace

// A library part in one call: its pins (order, names, sides, roles), what
// its model is, its supplies, how the test of the libraries found it.
QJsonObject QucsControl::describePart(const QJsonObject& args)
{
    const QString wantedLibrary = args.value(QLatin1String("library")).toString().trimmed();
    const QString part = args.value(QLatin1String("part")).toString().trimmed();
    if (wantedLibrary.isEmpty() || part.isEmpty())
        return errorResult(tr("Say which part: 'library' and 'part', as find_library_component gives them (OpAmps, uA741)."));
    // Its block in its library: the installed ones, then the user's.
    QString library, body;
    for (const QString& dirName : {QucsSettings.LibDir, QucsSettings.qucsWorkspaceDir.filePath(QStringLiteral("user_lib"))}) {
        for (const QFileInfo& fi : QDir(dirName).entryInfoList({QStringLiteral("*.lib")}, QDir::Files, QDir::Name)) {
            if (fi.completeBaseName().compare(wantedLibrary, Qt::CaseInsensitive) != 0) continue;
            QFile f(fi.filePath());
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
            const QRegularExpression block(QStringLiteral("<Component\\s+%1>(.*?)</Component>").arg(QRegularExpression::escape(part)),
                                           QRegularExpression::DotMatchesEverythingOption);
            if (const QRegularExpressionMatch m = block.match(QString::fromUtf8(f.readAll())); m.hasMatch()) {
                library = fi.completeBaseName();
                body = m.captured(1);
                break;
            }
        }
        if (!body.isEmpty()) break;
    }
    if (body.isEmpty())
        return errorResult(tr("There is no part %1 in a library %2 here: find_library_component finds one by its name or values.")
                               .arg(part, wantedLibrary));
    const auto section = [&body](const QString& name) {
        return body.section(QStringLiteral("<%1>").arg(name), 1).section(QStringLiteral("</%1>").arg(name), 0, 0).trimmed();
    };
    const QString description = section(QStringLiteral("Description"));
    const QString modelLine = section(QStringLiteral("Model"));
    const QString spice = section(QStringLiteral("Spice"));
    const bool oneLine = modelLine.startsWith(QLatin1Char('<')) && modelLine.endsWith(QLatin1Char('>')) && !modelLine.contains(QLatin1Char('\n'));
    const QString placedAs = oneLine ? modelLine.mid(1).section(QLatin1Char(' '), 0, 0) : QString();

    // The part as add_component makes it: its pins.
    std::unique_ptr<Component> c(newComponent(oneLine ? placedAs : QStringLiteral("Lib")));
    if (c == nullptr) return errorResult(tr("%1's model is a %2, which this Qucs-S does not have.").arg(part, placedAs));
    if (!oneLine && c->Props.size() >= 2) {
        c->Props.at(0)->Value = library;
        c->Props.at(1)->Value = part;
    }
    c->recreate();
    QJsonArray pins;
    QStringList supplies;
    for (int i = 0; i < c->Ports.size(); ++i) {
        const QString name = c->Ports.at(i)->Name;
        QJsonObject pin{{QStringLiteral("pin"), i + 1}, {QStringLiteral("side"), pinSide(c.get(), i)}};
        if (!name.isEmpty()) pin.insert(QStringLiteral("name"), name);
        if (const QString role = qucs_s::erc::pinRole(name); !role.isEmpty()) pin.insert(QStringLiteral("role"), role);
        if (qucs_s::erc::pinRole(name) == QLatin1String("supply")) supplies << name;
        pins.append(pin);
    }

    // What its model is, by the elements the simulator gets: the SPICE
    // model when it has one, else the Qucs one (Type:Name lines).
    QString kind;
    if (oneLine) {
        kind = tr("one component: placed as a %1 with the library's values").arg(placedAs);
    } else {
        int transistors = 0, controlled = 0, diodes = 0, passives = 0, calls = 0, elements = 0;
        static const QStringList transistorTypes{QStringLiteral("BJT"), QStringLiteral("_BJT"), QStringLiteral("MOSFET"),
                                                 QStringLiteral("_MOSFET"), QStringLiteral("JFET"), QStringLiteral("MESFET")};
        static const QStringList controlledTypes{QStringLiteral("VCVS"), QStringLiteral("VCCS"), QStringLiteral("CCCS"),
                                                 QStringLiteral("CCVS"), QStringLiteral("EDD"), QStringLiteral("RFEDD")};
        for (const QString& raw : (spice.isEmpty() ? modelLine : spice).split(QLatin1Char('\n'))) {
            const QString line = raw.trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('*')) || line.startsWith(QLatin1Char('+')) || line.startsWith(QLatin1Char('.')))
                continue;
            ++elements;
            const QString type = spice.isEmpty() ? line.section(QLatin1Char(':'), 0, 0) : QString(line.at(0).toUpper());
            if (transistorTypes.contains(type) || (type.size() == 1 && QStringLiteral("QMJZ").contains(type))) ++transistors;
            else if (controlledTypes.contains(type) || (type.size() == 1 && QStringLiteral("EFGHB").contains(type))) ++controlled;
            else if (type == QLatin1String("Diode") || type == QLatin1String("D")) ++diodes;
            else if (type == QLatin1String("R") || type == QLatin1String("C") || type == QLatin1String("L")) ++passives;
            else if (type == QLatin1String("Sub") || type == QLatin1String("X")) ++calls;
        }
        const QString counts = tr("%1 transistors, %2 controlled sources, %3 diodes, %4 R/C/L, %5 subcircuit calls")
                                   .arg(transistors).arg(controlled).arg(diodes).arg(passives).arg(calls);
        kind = transistors >= 6 && controlled == 0 ? tr("transistor level (%1): slow to simulate, true to the silicon's limits").arg(counts)
             : controlled > 0 ? tr("macromodel (%1): fast, as good as its maker's fit").arg(counts)
             : tr("subcircuit of %1 elements (%2)").arg(elements).arg(counts);
    }
    bool passes = false;
    QJsonObject result{{QStringLiteral("library"), library},
                       {QStringLiteral("part"), part},
                       {QStringLiteral("description"), description.simplified()},
                       {QStringLiteral("pins"), pins},
                       {QStringLiteral("model"), kind},
                       {QStringLiteral("ngspice"), testedText(library, part, &passes)},
                       {QStringLiteral("place"), QJsonObject{{QStringLiteral("type"), QStringLiteral("Lib")},
                                                             {QStringLiteral("properties"), QJsonObject{{QStringLiteral("Lib"), library},
                                                                                                        {QStringLiteral("Comp"), part}}}}}};
    if (!description.isEmpty()) result.insert(QStringLiteral("note"), description.section(QLatin1Char('\n'), 0, 0).simplified());
    if (!supplies.isEmpty()) result.insert(QStringLiteral("supply pins"), QJsonArray::fromStringList(supplies));
    if (oneLine) result.insert(QStringLiteral("placed as"), placedAs);
    if (!c->Ports.isEmpty() && std::all_of(c->Ports.cbegin(), c->Ports.cend(), [](const Port* p) { return p->Name.isEmpty(); })
        && c->Ports.size() > 2)
        result.insert(QStringLiteral("pins without names"),
                      tr("wired by number: its model's pin order is the only guide (the sides above say where each is)"));
    return jsonResult(result);
}

QJsonObject QucsControl::findLibraryComponent(const QJsonObject& args)
{
    const QString search = args.value(QLatin1String("search")).toString().trimmed();
    const QStringList words = search.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    QString kind = args.value(QLatin1String("type")).toString().trimmed().toLower();
    const QJsonObject near = args.value(QLatin1String("near")).toObject();
    const QString onlyLibrary = args.value(QLatin1String("library")).toString().trimmed();
    const int limit = std::clamp(args.value(QLatin1String("limit")).toInt(15), 1, 100);
    // Only the parts the run of the libraries under ngspice found working.
    const bool onlyTested = args.value(QLatin1String("tested")).toBool();
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
                // As the run of every part under ngspice found it.
                bool passes = false;
                const QString outcome = testedText(library, name, &passes);
                if (onlyTested && !passes) continue;
                if (const int t = int(names.indexOf(QStringLiteral("Type"))); t >= 0) shown.insert(QStringLiteral("Type"), values.value(t));
                QJsonObject o{{QStringLiteral("library"), library},
                              {QStringLiteral("component"), name},
                              {QStringLiteral("model"), type},
                              {QStringLiteral("description"), description.section(QLatin1Char('\n'), 0, 1).simplified()},
                              {QStringLiteral("place"), QJsonObject{{QStringLiteral("type"), QStringLiteral("Lib")},
                                                                    {QStringLiteral("properties"), QJsonObject{{QStringLiteral("Lib"), library},
                                                                                                               {QStringLiteral("Comp"), name}}}}}};
                if (!shown.isEmpty()) o.insert(QStringLiteral("values"), shown);
                // A part whose model is one component line: 'place' makes that
                // component with the library's values, not a Lib (said here, as
                // add_component says it).
                if (modelLine.startsWith(QLatin1Char('<')) && modelLine.endsWith(QLatin1Char('>')) && !modelLine.contains(QLatin1Char('\n')))
                    o.insert(QStringLiteral("placed as"), type);
                o.insert(QStringLiteral("ngspice"), outcome);
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
    // Only the lines that differ: a part whose line is the same says nothing,
    // and parsing every line of both, at 37,500 parts, took a second.
    const QStringList linesWas = linesOf(a.at(0)), linesNow = linesOf(b.at(0));
    QHash<QString, int> more;   // how many more times a line was there than is
    for (const QString& l : linesWas) ++more[l];
    for (const QString& l : linesNow) --more[l];
    const auto parts = [&more](const QStringList& lines, int side) {
        QHash<QString, int> left = more;
        QList<PartLine> list;
        for (const QString& l : lines) {
            int& k = left[l];
            if (k * side <= 0) continue;   // (as many there as here)
            k -= side;
            PartLine p;
            if (parsePart(l, &p)) list << p;
        }
        return list;
    };
    const QList<PartLine> was = parts(linesWas, 1), now = parts(linesNow, -1);
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
    // An unnamed part (a ground) moved: gone from one place and come to
    // another, of one type - paired in their order (a selection moved with
    // its grounds read as each deleted and added).
    {
        QHash<QString, QList<qsizetype>> come;   // the unnamed added, by type
        for (qsizetype j = 0; j < added.size(); ++j)
            if (const PartLine* p = byKeyNow.value(added.at(j)); p->name == QLatin1String("*")) come[p->model] << j;
        QSet<qsizetype> paired;
        QStringList gone;
        for (const QString& k : std::as_const(removed)) {
            const PartLine* old = byKeyWas.value(k);
            const auto it = old->name == QLatin1String("*") ? come.find(old->model) : come.end();
            if (it == come.end() || it->isEmpty()) {
                gone << k;
                continue;
            }
            const qsizetype j = it->takeFirst();
            paired.insert(j);
            const PartLine* fresh = byKeyNow.value(added.at(j));
            QStringList what{tr("moved from %1 to %2").arg(where(old->cx, old->cy), where(fresh->cx, fresh->cy))};
            if (old->rotation != fresh->rotation) what << tr("turned");
            if (old->mirror != fresh->mirror) what << tr("mirrored");
            changes << QStringLiteral("%1: %2").arg(old->model, what.join(QStringLiteral(", ")));
        }
        QStringList come2;
        for (qsizetype j = 0; j < added.size(); ++j)
            if (!paired.contains(j)) come2 << added.at(j);
        removed = gone;
        added = come2;
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
        if (what.isEmpty() && (old->tx != p.tx || old->ty != p.ty)) {
            // (A simulation block's text is placed as it is drawn - Simulation-
            // Component::drawSymbol(): every load of a file "moved" it.)
            if (p.model.startsWith(QLatin1Char('.'))) continue;
            what << tr("its text moved");
        }
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
    QStringList drawn, takenAway;   // (each by its ends)
    const auto ends = [](const QString& key) {
        const QStringList n = key.split(QLatin1Char(' '));
        return n.size() == 4 ? QStringLiteral("%1,%2-%3,%4").arg(n.at(0), n.at(1), n.at(2), n.at(3)) : key;
    };
    for (auto it = wiresNow.cbegin(); it != wiresNow.cend(); ++it)
        for (int k = wiresWas.value(it.key()); k < it.value(); ++k, ++wiresAdded) drawn << ends(it.key());
    for (auto it = wiresWas.cbegin(); it != wiresWas.cend(); ++it)
        for (int k = wiresNow.value(it.key()); k < it.value(); ++k, ++wiresRemoved) takenAway << ends(it.key());
    if (wiresAdded > 0 || wiresRemoved > 0) {
        // Where, when they are few: a wire drawn again round a turned part
        // is seen before it is made.
        const auto where = [](QStringList list) {
            list.sort();
            return list.size() <= 6 ? QStringLiteral(" (%1)").arg(list.join(QStringLiteral("; "))) : QString();
        };
        QStringList w;
        if (wiresAdded > 0) w << (wiresAdded == 1 ? tr("a wire drawn") : tr("%1 wires drawn").arg(wiresAdded)) + where(drawn);
        if (wiresRemoved > 0) w << (wiresRemoved == 1 ? tr("a wire taken away") : tr("%1 wires taken away").arg(wiresRemoved)) + where(takenAway);
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
        if (o.header.section(QLatin1Char('"'), 9, 9) != d.header.section(QLatin1Char('"'), 9, 9)) what << tr("its colours changed");
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
    // The files the tools wrote, the last first: undo with 'files' puts
    // them back.
    QJsonArray files;
    for (qsizetype k = a_fileSteps.size() - 1; k >= 0 && files.size() < 10; --k) {
        QJsonArray names;
        for (const auto& kept : a_fileSteps.at(k).before)
            names.append(QStringLiteral("%1%2").arg(QDir::toNativeSeparators(kept.first), kept.second ? QString() : tr(" (made by it)")));
        for (const auto& [from, to] : a_fileSteps.at(k).moved)
            names.append(tr("%1 (moved to %2)").arg(QDir::toNativeSeparators(from), QDir::toNativeSeparators(to)));
        files.append(QJsonObject{{QStringLiteral("tool"), a_fileSteps.at(k).tool},
                                 {QStringLiteral("at"), a_fileSteps.at(k).when.toString(Qt::ISODate)}, {QStringLiteral("files"), names}});
    }
    if (!files.isEmpty()) result.insert(QStringLiteral("files written"), files);
    return jsonResult(result);
}

// ----------------------------------------------------------------------
// Projects: made, opened; a schematic copied with its results; a run's
// scratch files cleared

namespace {

bool anyUnsaved(QucsApp* app, QStringList* names)
{
    for (QucsDoc* doc : app->allDocuments())
        if (doc->getDocChanged())   // (an untitled one by its title: it was "and  have ...")
            *names << (doc->getDocName().isEmpty() ? tr("an untitled document") : QFileInfo(doc->getDocName()).fileName());
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

// A schematic's Data Set and Data Display, as Qucs-S has them (open) or its
// file says (<DataSet=run.dat>); its own name's by default. Its datasets
// are named after the Data Set, not after the file: amp.sch of Data Set
// run.dat writes run.dat.ngspice, and amp.dat beside it may be another's.
QString resultName(QucsDoc* open, const QString& file, bool display)
{
    const QString base = QFileInfo(file).completeBaseName();
    QString name = base + (display ? QStringLiteral(".dpl") : QStringLiteral(".dat"));
    if (open != nullptr) {
        const QString own = display ? open->getDataDisplay() : open->getDataSet();
        return own.isEmpty() ? name : own;
    }
    QFile f(file);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QByteArray head = f.read(4096);
        static const QRegularExpression set(QStringLiteral("<DataSet=([^>\\n]*)>")), shown(QStringLiteral("<DataDisplay=([^>\\n]*)>"));
        const QRegularExpressionMatch m = (display ? shown : set).match(QString::fromUtf8(head));
        if (m.hasMatch()) name = display ? QucsDoc::fileBeside(m.captured(1), name) : QucsDoc::dataSetBeside(m.captured(1), name);
    }
    return name;
}

// To the system's trash; else away (scratch files are made again).
bool toTrash(const QString& path, bool* trashed)
{
    if (misc::moveToTrash(path)) {
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
    // (Each fault said as it is: "." was told "no slashes".)
    if (name.isEmpty()) return errorResult(tr("'name' is the project's name, a folder's name: amp (or amp_prj)."));
    if (!name.contains(QRegularExpression(QStringLiteral("[\\p{L}\\p{N}]"))))
        return errorResult(tr("'name': %1 is no project's name - it has no letter or digit.").arg(name));
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')))
        return errorResult(tr("'name': %1 has a slash - a project's name is one folder's name, made in the workspace.").arg(name));
    if (const QString bad = badFileName(name); !bad.isEmpty()) return errorResult(tr("'name': %1.").arg(bad));
    if (name.startsWith(QLatin1Char('.')))
        return errorResult(tr("'name': %1 begins with a dot - a folder so named is hidden. Begin it with a letter or digit.").arg(name));
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
            result.insert(QStringLiteral("opened"), (unsaved.size() == 1 ? tr("not opened: %1 has unsaved changes (opening a project closes the documents) - "
                                                                              "save or close it, then open_project")
                                                                           : tr("not opened: %1 have unsaved changes (opening a project closes the documents) - "
                                                                              "save or close them, then open_project"))
                                                        .arg(unsaved.join(QStringLiteral(", "))));
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
        return errorResult((unsaved.size() == 1 ? tr("Opening a project closes the documents, and %1 has unsaved changes: save or close it first.")
                                                : tr("Opening a project closes the documents, and %1 have unsaved changes: save or close them first."))
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
    if (const QString bad = badFileName(QFileInfo(to).fileName()); !bad.isEmpty()) return errorResult(tr("'to': %1.").arg(bad));
    if (sameFile(from, to)) return errorResult(tr("The copy would be the schematic itself."));
    // Not the name of a dataset imported there (its Data Set would be it).
    if (qucs_s::dataimport::Origin origin;
        qucs_s::dataimport::originOf(QFileInfo(to).absoluteDir().filePath(QFileInfo(to).completeBaseName() + QStringLiteral(".dat")), &origin))
        return errorResult(tr("%1.dat there is a dataset imported from %2, and a schematic %1 simulates into %1.dat: choose another name.")
                               .arg(QFileInfo(to).completeBaseName(), QDir::toNativeSeparators(origin.source)));
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
    aboutToWrite(to);
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
        // Its datasets (each simulator's) and its data display: those its
        // Data Set and Data Display name (the copy's are named after it).
        QString dataSet = resultName(open, from, false);
        if (dataSet.endsWith(QLatin1String(".dat"), Qt::CaseInsensitive)) dataSet.chop(4);
        for (const QString& suffix : {QStringLiteral(".dat"), QStringLiteral(".dat.ngspice"), QStringLiteral(".dat.xyce"), QStringLiteral(".dat.spopus")}) {
            const QString a = src.absoluteDir().filePath(dataSet + suffix), b = dst.absoluteDir().filePath(newBase + suffix);
            qucs_s::dataimport::Origin origin;
            if (suffix == QLatin1String(".dat") && qucs_s::dataimport::originOf(a, &origin)) continue;   // (an import: no run's)
            if (!QFileInfo::exists(a)) continue;
            aboutToWrite(b);
            if (!misc::copyFileOver(a, b)) continue;
            written << QFileInfo(b).fileName();
            // The netlist its run was given, the copy's now: get_dataset
            // then tells by it whether the copy is still of that circuit.
            // (Kept as of that run: the copy, written after, is compared
            // with it once - it may have unsaved changes of the original.)
            QDateTime at;
            if (const QString netlist = misc::runNetlistOf(a, &at); !netlist.isEmpty()) misc::keepRunNetlist(b, netlist, to, at);
        }
        const QString dpl = src.absoluteDir().filePath(resultName(open, from, true));
        if (QFileInfo::exists(dpl)) {
            const QString b = dst.absoluteDir().filePath(newBase + QStringLiteral(".dpl"));
            aboutToWrite(b);
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
    QString note;
    // Its own subfolder of the Scratch folder only - never the folder shared:
    // the project's Scratch, or with no project open the one every
    // schematic of none runs in (trashed whole, it was).
    const bool shared = QFileInfo(scratch).fileName() == QLatin1String(misc::ScratchFolder) || sameFile(scratch, misc::scratchDir())
                        || sameFile(scratch, QucsSettings.S4Qworkdir);
    if (QFileInfo(scratch).isDir() && !shared) {
        const int files = int(QDir(scratch).entryList(QDir::Files | QDir::NoDotAndDotDot).size());
        bool t = false;
        if (toTrash(scratch, &t)) gone << tr("%1 (%2 files: netlists, the simulator's output, logs)").arg(QDir::toNativeSeparators(scratch)).arg(files);
        trashed = trashed && t;
    } else if (QFileInfo(scratch).isDir()) {
        // The shared one: the files of its last run there, when that was
        // this schematic's (its netlist's first line names it) - not the
        // folder, nor what else is in it.
        QString head;
        if (QFile f(QDir(scratch).filePath(QStringLiteral("spice4qucs.cir"))); f.open(QIODevice::ReadOnly | QIODevice::Text))
            head = QString::fromUtf8(f.readLine()).trimmed();
        static const QRegularExpression named(QStringLiteral("^\\*\\s*Qucs\\S*\\s+\\S+\\s+(.+)$"));
        const QRegularExpressionMatch m = named.match(head);
        if (m.hasMatch() && sameFile(m.captured(1).trimmed(), docName)) {
            int files = 0;
            // (The run's own: its netlist and what the simulator wrote for
            // it, spice4qucs.*, and the log - not what else is there.)
            for (const QString& file : QDir(scratch).entryList({QStringLiteral("spice4qucs*"), QStringLiteral("log.txt")},
                                                               QDir::Files | QDir::NoDotAndDotDot)) {
                bool t = false;
                if (toTrash(QDir(scratch).filePath(file), &t)) ++files;
                trashed = trashed && t;
            }
            if (files > 0)
                gone << (files == 1 ? tr("the file of its last run in %1, the scratch folder schematics of no project share (the folder "
                                         "stays)").arg(QDir::toNativeSeparators(scratch))
                                    : tr("the %1 files of its last run in %2, the scratch folder schematics of no project share (the "
                                         "folder stays)").arg(files).arg(QDir::toNativeSeparators(scratch)));
        } else {
            note = tr("Its runs go to %1, the scratch folder schematics of no project share, and the last run there was another "
                      "schematic's: nothing of its own is there.").arg(QDir::toNativeSeparators(scratch));
        }
    }
    if (args.value(QLatin1String("datasets")).toBool()) {
        // Those of its Data Set (each simulator's), not of its file's name;
        // never one imported (a run of the schematic does not make it).
        const QFileInfo info(docName);
        QString dataSet = resultName(document(args, &error), docName, false);
        if (dataSet.endsWith(QLatin1String(".dat"), Qt::CaseInsensitive)) dataSet.chop(4);
        for (const QString& suffix : {QStringLiteral(".dat"), QStringLiteral(".dat.ngspice"), QStringLiteral(".dat.xyce"), QStringLiteral(".dat.spopus")}) {
            const QString f = info.absoluteDir().filePath(dataSet + suffix);
            qucs_s::dataimport::Origin origin;
            if (!QFileInfo::exists(f) || qucs_s::dataimport::originOf(f, &origin)) continue;
            bool t = false;
            const QString kept = misc::runNetlistFile(f);   // (named by where it is: while it is there)
            if (toTrash(f, &t)) {
                gone << QFileInfo(f).fileName();
                trashed = trashed && t;
                QFile::remove(kept);   // (the netlist kept for it: of nothing now)
            }
        }
    }
    if (gone.isEmpty())
        return textResult(tr("There was nothing to clear for %1.").arg(QFileInfo(docName).fileName()) + (note.isEmpty() ? QString() : QLatin1Char(' ') + note));
    return textResult(tr("Cleared: %1. %2").arg(gone.join(QStringLiteral("; ")),
                                                 trashed ? tr("(In the trash, to take back.)") : tr("(Deleted: this system has no trash.)")));
}

// ----------------------------------------------------------------------
// Files renamed, moved and trashed, as the File Browser does them

namespace {

// Why \a path may not be renamed or trashed - the workspace, the home
// folder, a folder holding either, or the project open now (or a folder
// holding it), which Qucs-S works in; empty when it may.
QString keptInPlace(const QString& path)
{
    const auto holds = [](const QString& folder, const QString& inside) {
        const QString f = QFileInfo(folder).canonicalFilePath(), i = QFileInfo(inside).canonicalFilePath();
        return !f.isEmpty() && !i.isEmpty() && (i == f || i.startsWith(f + QLatin1Char('/')));
    };
    if (holds(path, QucsSettings.qucsWorkspaceDir.absolutePath()))
        return QObject::tr("%1 is the workspace, or holds it").arg(QDir::toNativeSeparators(path));
    if (holds(path, QDir::homePath())) return QObject::tr("%1 is the home folder, or holds it").arg(QDir::toNativeSeparators(path));
    if (QucsMain != nullptr && !QucsMain->ProjName.isEmpty() && holds(path, QucsSettings.QucsWorkDir.absolutePath()))
        return QObject::tr("%1 is the project open now, or holds it: Project > Close Project first").arg(QDir::toNativeSeparators(path));
    return {};
}

} // namespace

QJsonObject QucsControl::renameFile(const QJsonObject& args)
{
    const QString given = args.value(QLatin1String("path")).toString().trimmed();
    const QString to = args.value(QLatin1String("to")).toString().trimmed();
    if (given.isEmpty()) return errorResult(tr("'path' is the file or folder renamed or moved."));
    if (to.isEmpty()) return errorResult(tr("'to' is its new name (amp2.sch), or a path to move it to."));
    const QString path = absolute(given);
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink()) return errorResult(tr("There is no %1.").arg(QDir::toNativeSeparators(path)));
    if (const QString why = keptInPlace(path); !why.isEmpty()) return errorResult(tr("%1: it is not renamed or moved.").arg(why));
    const bool isFile = info.isFile();   // (asked before: afterwards nothing is there)
    // Its documents open now, to say which followed.
    QStringList open;
    for (QucsDoc* doc : a_app->allDocuments())
        if (const QString name = doc->getDocName(); !name.isEmpty() && (name == QDir::cleanPath(path) || name.startsWith(QDir::cleanPath(path) + QLatin1Char('/'))))
            open << name;
    // A name stays in its folder (the File Browser's rename, which takes
    // another case of the same name too); a path goes there - into a
    // folder that is there, or as the name it ends in.
    const bool asPath = to.contains(QLatin1Char('/')) || to.contains(QLatin1Char('\\'));
    QString target;
    if (!asPath) {
        if (const QString bad = badFileName(to); !bad.isEmpty()) return errorResult(tr("'to': %1.").arg(bad));
        target = QDir::cleanPath(info.dir().filePath(to));
        FileBrowser* browser = a_app->fileBrowserPanel();
        if (browser == nullptr) return errorResult(tr("Files cannot be renamed here."));
        if (const QString why = browser->renameEntry(path, to); !why.isEmpty()) return errorResult(why);
        movedFile(path, target);
    } else {
        target = absolute(to);
        if (QFileInfo(target).isDir() && !sameFile(target, path)) target = QDir::cleanPath(QDir(target).filePath(info.fileName()));
        if (QFileInfo::exists(target) || QFileInfo(target).isSymLink())
            return errorResult(tr("There is one there already: %1. Nothing was moved.").arg(QDir::toNativeSeparators(target)));
        if (!QFileInfo(QFileInfo(target).absolutePath()).isDir())
            return errorResult(tr("There is no folder %1 to move it into.").arg(QDir::toNativeSeparators(QFileInfo(target).absolutePath())));
        if (info.isDir() && QFileInfo(target).absoluteFilePath().startsWith(QDir::cleanPath(path) + QLatin1Char('/')))
            return errorResult(tr("%1 cannot go into itself.").arg(QDir::toNativeSeparators(path)));
        if (!QDir().rename(path, target))
            return errorResult(tr("%1 could not be moved to %2 (another disk? then copy it, and trash_file the first).")
                                   .arg(QDir::toNativeSeparators(path), QDir::toNativeSeparators(target)));
        a_app->documentsMoved({QDir::cleanPath(path)}, {target});
        movedFile(path, target);
    }
    if (a_app->projectView() != nullptr) a_app->projectView()->refresh();
    QJsonObject result{{QStringLiteral("renamed"), QDir::toNativeSeparators(path)}, {QStringLiteral("to"), QDir::toNativeSeparators(target)},
                       {QStringLiteral("undo"), tr("undo with 'files' renames it back")}};
    if (!open.isEmpty()) {
        QJsonArray followed;
        for (const QString& was : open) {
            const QString now = target + was.mid(QDir::cleanPath(path).size());
            followed.append(QStringLiteral("%1 -> %2").arg(QFileInfo(was).fileName(), QDir::toNativeSeparators(now)));
        }
        result.insert(QStringLiteral("documents"), followed);
    }
    QStringList notes;
    if (isFile && info.suffix().compare(QFileInfo(target).suffix(), Qt::CaseInsensitive) != 0)
        notes << tr("Its suffix changed (.%1 to %2): Qucs-S opens it as its new suffix says.")
                     .arg(info.suffix(), QFileInfo(target).suffix().isEmpty() ? tr("none") : QStringLiteral(".") + QFileInfo(target).suffix());
    if (isFile && info.suffix().compare(QLatin1String("sch"), Qt::CaseInsensitive) == 0)
        notes << tr("Its datasets and data display keep their names (its Data Set still names them): copy_document copies a "
                    "schematic with them under a new name.");
    if (!notes.isEmpty()) result.insert(QStringLiteral("notes"), QJsonArray::fromStringList(notes));
    return jsonResult(result);
}

QJsonObject QucsControl::trashFile(const QJsonObject& args)
{
    const QString given = args.value(QLatin1String("path")).toString().trimmed();
    if (given.isEmpty()) return errorResult(tr("'path' is the file or folder moved to the trash."));
    const QString path = QDir::cleanPath(absolute(given));
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink()) return errorResult(tr("There is no %1.").arg(QDir::toNativeSeparators(path)));
    if (const QString why = keptInPlace(path); !why.isEmpty()) return errorResult(tr("%1: it is not moved to the trash.").arg(why));
    // The documents open from it close with it - unless one has unsaved
    // changes, which would be lost.
    QStringList open, unsaved;
    for (QucsDoc* doc : a_app->allDocuments()) {
        const QString name = doc->getDocName();
        if (name.isEmpty() || !(name == path || name.startsWith(path + QLatin1Char('/')))) continue;
        (doc->getDocChanged() ? unsaved : open) << name;
    }
    const auto names = [](const QStringList& paths) {
        QStringList list;
        for (const QString& p : paths) list << QFileInfo(p).fileName();
        return list.join(QStringLiteral(", "));
    };
    if (!unsaved.isEmpty())
        return errorResult(tr("%1 is not moved to the trash: %2 open here with unsaved changes. Save or close it first.")
                               .arg(info.fileName(), names(unsaved)));
    if (a_app->simulationConsole() != nullptr && a_app->simulationConsole()->isRunning())
        return errorResult(tr("A simulation is running: files are moved to the trash after it has ended."));
    QString where;
    if (!misc::moveToTrash(path, &where))
        return errorResult(tr("%1 could not be moved to the trash. Nothing was deleted.").arg(QDir::toNativeSeparators(path)));
    a_app->documentsTrashed(open);
    if (a_app->projectView() != nullptr) a_app->projectView()->refresh();
    // Where it went, for undo's 'files' to take it back from (the system
    // may not say: then the user takes it back).
    if (!where.isEmpty()) movedFile(path, where);
    QJsonObject result{{QStringLiteral("trashed"), QDir::toNativeSeparators(path)},
                       {QStringLiteral("note"), where.isEmpty() ? tr("In the trash, to take back from there (the system did not say where it went: "
                                                                     "undo cannot).")
                                                                : tr("In the trash: undo with 'files' takes it back from there (or Finder's Put "
                                                                     "Back).")}};
    if (!where.isEmpty()) result.insert(QStringLiteral("in trash"), QDir::toNativeSeparators(where));
    if (!open.isEmpty()) result.insert(QStringLiteral("closed"), QJsonArray::fromStringList(open));
    return jsonResult(result);
}

// ----------------------------------------------------------------------
// A subcircuit's symbol drawn: a box, each pin on its side

namespace {

// A subcircuit's symbol's name text, which holds its parameters.
ID_Text* idOf(const std::list<Painting*>& paintings)
{
    for (Painting* p : paintings)
        if (p->Name == QLatin1String(".ID ")) return static_cast<ID_Text*>(p);
    return nullptr;
}

QJsonObject parameterJson(const SubParameter& p)
{
    return {{QStringLiteral("name"), p.name.section(QLatin1Char('='), 0, 0)},
            {QStringLiteral("default"), p.name.section(QLatin1Char('='), 1)},
            {QStringLiteral("description"), p.description},
            {QStringLiteral("type"), p.type},
            {QStringLiteral("shown"), p.display}};
}

// How they go on the .SUBCKT line: Rs=1k k=2.
QString subcktParameters(const ID_Text* id)
{
    QStringList pairs;
    for (const auto& p : id->subParameters) pairs << p->name;
    return pairs.join(QLatin1Char(' '));
}

// Changes \a id's parameters as 'parameters' (each set or added by name;
// with \a replace, the list is those alone), 'remove' and 'prefix' ask.
// What changed in words in \a done; nothing is changed when one is wrong.
bool changeParameters(ID_Text* id, const QJsonObject& args, bool replace, QStringList* done, bool* reordered, QString* error)
{
    const auto quoteOrEquals = [](const QString& s) { return s.contains(QLatin1Char('"')) || s.contains(QLatin1Char('=')); };
    struct Wanted {
        QString name;
        std::optional<QString> value, description, type;
        std::optional<bool> shown;
    };
    QList<Wanted> wanted;
    for (const QJsonValue& v : args.value(QLatin1String("parameters")).toArray()) {
        Wanted w;
        if (v.isString()) {
            // "Rs=1k"
            const QString text = v.toString().trimmed();
            const int eq = int(text.indexOf(QLatin1Char('=')));
            w.name = (eq < 0 ? text : text.left(eq)).trimmed();
            if (eq >= 0) w.value = text.mid(eq + 1).trimmed();
        } else {
            const QJsonObject o = v.toObject();
            w.name = o.value(QLatin1String("name")).toString().trimmed();
            const QJsonValue d = o.value(QLatin1String("default"));
            if (d.isString()) w.value = d.toString().trimmed();
            else if (d.isDouble()) w.value = QString::number(d.toDouble(), 'g', 12);
            if (o.contains(QLatin1String("description"))) w.description = o.value(QLatin1String("description")).toString();
            if (o.contains(QLatin1String("type"))) w.type = o.value(QLatin1String("type")).toString();
            if (o.contains(QLatin1String("shown"))) w.shown = o.value(QLatin1String("shown")).toBool();
        }
        static const QRegularExpression word(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
        if (!word.match(w.name).hasMatch()) {
            *error = tr("A parameter's name is a word (letters, digits and _, not starting with a digit), as SPICE takes it: %1 is not.")
                         .arg(w.name.isEmpty() ? tr("\"\"") : w.name);
            return false;
        }
        if (w.name.compare(QLatin1String("File"), Qt::CaseInsensitive) == 0) {
            *error = tr("A parameter cannot be called File: an instance's first property is its file.");
            return false;
        }
        for (const Wanted& other : std::as_const(wanted))
            if (other.name.compare(w.name, Qt::CaseInsensitive) == 0) {
                *error = tr("%1 is given twice (SPICE reads names without case).").arg(w.name);
                return false;
            }
        static const QRegularExpression space(QStringLiteral("\\s"));
        if ((w.value && (w.value->isEmpty() || quoteOrEquals(*w.value) || w.value->contains(space)))
            || (w.description && quoteOrEquals(*w.description)) || (w.type && quoteOrEquals(*w.type))) {
            *error = tr("%1: a default is one value with no space in it, as the .SUBCKT line takes it (1k, 2.5, {2*Rs}), and none of "
                        "default, description and type has = or \" in it.").arg(w.name);
            return false;
        }
        // One SPICE reads as a value: a number (a scale and unit letters
        // after it), an expression in braces or quotes, or a name - not
        // "-", "1k;" (a comment from the ;) or "1,5".
        if (w.value) {
            static const QRegularExpression number(QStringLiteral("^[+-]?(\\d+\\.?\\d*|\\.\\d+)([eE][+-]?\\d+)?[A-Za-z]*$"));
            const QString& v = *w.value;
            const bool braced = v.size() > 2 && ((v.startsWith(QLatin1Char('{')) && v.endsWith(QLatin1Char('}')) && v.count(QLatin1Char('{')) == v.count(QLatin1Char('}')))
                                                 || (v.startsWith(QLatin1Char('\'')) && v.endsWith(QLatin1Char('\'')) && v.count(QLatin1Char('\'')) == 2));
            if (!braced && !number.match(v).hasMatch() && !word.match(v).hasMatch()) {
                *error = tr("%1's default %2 is no value SPICE reads: a number (1k, 2.5, 4.7n), an expression in braces ({2*Rs}) or "
                            "a parameter's name.").arg(w.name, v);
                return false;
            }
        }
        wanted << w;
    }
    QStringList remove;
    for (const QJsonValue& v : args.value(QLatin1String("remove")).toArray()) remove << v.toString().trimmed();

    // The list as it is, then as it will be.
    std::vector<std::unique_ptr<SubParameter>> now;
    for (const auto& p : id->subParameters) now.push_back(std::make_unique<SubParameter>(*p));
    const auto nameOf = [](const SubParameter& p) { return p.name.section(QLatin1Char('='), 0, 0); };
    const auto find = [&](std::vector<std::unique_ptr<SubParameter>>& in, const QString& name) {
        return std::find_if(in.begin(), in.end(), [&](const auto& p) { return nameOf(*p).compare(name, Qt::CaseInsensitive) == 0; });
    };
    QStringList names;
    for (const auto& p : now) names << nameOf(*p);
    for (const QString& r : std::as_const(remove)) {
        const auto it = find(now, r);
        if (it == now.end()) {
            *error = tr("There is no parameter %1 to remove: %2.").arg(r, names.isEmpty() ? tr("it has none") : names.join(QStringLiteral(", ")));
            return false;
        }
        done->append(tr("%1 removed").arg(nameOf(**it)));
        now.erase(it);
    }
    std::vector<std::unique_ptr<SubParameter>> result;
    if (!replace) result = std::move(now);
    for (const Wanted& w : std::as_const(wanted)) {
        auto it = find(result, w.name);
        if (it == result.end() && replace) {
            // (Kept from the list as it was: its fields not given stay.)
            const auto was = find(now, w.name);
            if (was != now.end()) {
                result.push_back(std::move(*was));
                now.erase(was);
                it = result.end() - 1;
            }
        }
        if (it == result.end()) {
            if (!w.value) {
                *error = tr("%1 is new: it needs a 'default' (ngspice refuses a .SUBCKT parameter with none).").arg(w.name);
                return false;
            }
            result.push_back(std::make_unique<SubParameter>(w.shown.value_or(true), w.name + QLatin1Char('=') + *w.value,
                                                            w.description.value_or(QString()), w.type.value_or(QString())));
            done->append(tr("%1 added, %2 by default").arg(w.name, *w.value));
            continue;
        }
        SubParameter& p = **it;
        const QString before = p.name;
        if (nameOf(p) != w.name) done->append(tr("%1 now written %2").arg(nameOf(p), w.name));
        p.name = w.name + QLatin1Char('=') + w.value.value_or(p.name.section(QLatin1Char('='), 1));
        if (w.value && before != p.name) done->append(tr("%1's default is %2").arg(w.name, *w.value));
        if (w.description) p.description = *w.description;
        if (w.type) p.type = *w.type;
        if (w.shown) p.display = *w.shown;
    }
    if (replace)
        for (const auto& gone : now) done->append(tr("%1 removed").arg(nameOf(*gone)));
    // Did any keep its place? An instance in a schematic not open takes
    // its values by place when that opens.
    QStringList after;
    for (const auto& p : result) after << nameOf(*p);
    *reordered = false;
    for (int i = 0; i < names.size() && i < after.size(); ++i)
        if (names.at(i).compare(after.at(i), Qt::CaseInsensitive) != 0) *reordered = true;
    if (args.contains(QLatin1String("prefix"))) {
        const QString prefix = args.value(QLatin1String("prefix")).toString().trimmed();
        // (A SPICE word: X;Y1 was a comment to ngspice from the ;.)
        static const QRegularExpression spiceWord(QStringLiteral("^[A-Za-z][A-Za-z0-9_]*$"));
        if (!spiceWord.match(prefix).hasMatch()) {
            *error = tr("'prefix' is a word SPICE reads as a name - a letter, then letters, digits and _ -: what the instances' names "
                        "begin with (SUB gives SUB1, SUB2, ...). %1 is not.").arg(prefix.isEmpty() ? tr("\"\"") : prefix);
            return false;
        }
        if (prefix != id->prefix) done->append(tr("the instances' names begin with %1").arg(prefix));
        id->prefix = prefix;
    }
    id->subParameters = std::move(result);
    return true;
}

} // namespace

QJsonArray qucs_s::control::subcircuitParametersJson(const Schematic* sch)
{
    QJsonArray list;
    if (const ID_Text* id = idOf(sch->a_SymbolPaints))
        for (const auto& p : id->subParameters) list.append(parameterJson(*p));
    return list;
}

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
    QStringList placed, changed;
    bool reordered = false;
    if (!sch->buildSymbol(sides, &error, &placed)
        || ((args.contains(QLatin1String("parameters")) || args.contains(QLatin1String("prefix")))
            && !changeParameters(idOf(sch->a_SymbolPaints), args, args.contains(QLatin1String("parameters")), &changed, &reordered,
                                 &error))) {
        sch->restoreAll(before);
        return errorResult(error);
    }
    sch->updateAllBoundingRect();
    sch->setChanged(true, true);
    sch->viewport()->update();
    QJsonObject result{{QStringLiteral("document"), titleOf(sch)}, {QStringLiteral("pins"), QJsonArray::fromStringList(placed)},
                       {QStringLiteral("then"), tr("Save it for the subcircuit's instances to take it; edit_painting moves a port or the name "
                                                   "text, add_painting draws more.")}};
    const QJsonArray parameters = subcircuitParametersJson(sch);
    if (!parameters.isEmpty()) result.insert(QStringLiteral("parameters"), parameters);
    if (!note.isEmpty()) result.insert(QStringLiteral("note"), note);
    return jsonResult(result);
}

QJsonObject QucsControl::setSubcircuitParameters(const QJsonObject& args)
{
    if (!args.contains(QLatin1String("parameters")) && !args.contains(QLatin1String("remove")) && !args.contains(QLatin1String("prefix")))
        return errorResult(tr("Give 'parameters' ([{\"name\": \"Rs\", \"default\": \"1k\"}] or [\"Rs=1k\"]), 'remove' or 'prefix'."));
    QString error, note;
    std::list<Painting*>* list = nullptr;
    QJsonObject onSymbol = args;
    onSymbol.insert(QStringLiteral("symbol"), true);
    Schematic* sch = paintingsOf(onSymbol, &list, &note, &error);
    if (sch == nullptr) return errorResult(error);
    if (sch->getIsSymbolOnly())
        return errorResult(tr("%1 is a symbol file: a subcircuit's parameters are on the symbol of its schematic (.sch).").arg(titleOf(sch)));
    const auto before = sch->snapshotAll();
    QStringList drawn;
    ID_Text* id = idOf(sch->a_SymbolPaints);
    if (id == nullptr) {
        if (sch->a_SymbolPaints.empty() || std::none_of(sch->a_SymbolPaints.begin(), sch->a_SymbolPaints.end(),
                                                        [](const Painting* p) { return p->Name != QLatin1String(".PortSym "); })) {
            // No symbol yet: one as make_symbol draws it.
            if (!sch->buildSymbol({}, &error, &drawn)) {
                sch->restoreAll(before);
                return errorResult(tr("A subcircuit's parameters are on its symbol, and %1").arg(error.left(1).toLower() + error.mid(1)));
            }
        } else {
            // A symbol drawn by hand with no name text: one below it.
            int left = INT_MAX, bottom = INT_MIN;
            for (const Painting* p : sch->a_SymbolPaints) {
                const QRect box = p->boundingRect();
                left = std::min(left, box.left());
                bottom = std::max(bottom, box.bottom());
            }
            sch->a_SymbolPaints.push_front(new ID_Text(left, bottom + 4));
        }
        id = idOf(sch->a_SymbolPaints);
    }
    QStringList changed;
    bool reordered = false;
    if (!changeParameters(id, args, args.value(QLatin1String("replace")).toBool(), &changed, &reordered, &error)) {
        sch->restoreAll(before);
        return errorResult(error);
    }
    sch->updateAllBoundingRect();
    sch->setChanged(true, true);
    sch->viewport()->update();
    QJsonObject result{{QStringLiteral("document"), titleOf(sch)},
                       {QStringLiteral("parameters"), subcircuitParametersJson(sch)},
                       {QStringLiteral("prefix"), id->prefix},
                       {QStringLiteral("on the .SUBCKT line"), subcktParameters(id)},
                       {QStringLiteral("changed"), changed.isEmpty() ? QJsonValue(tr("nothing: they were so already"))
                                                                     : QJsonValue(changed.join(QStringLiteral("; ")))},
                       {QStringLiteral("one step to undo"), true},
                       {QStringLiteral("then"), tr("Save it: its instances in open schematics take the parameters by name, each keeping "
                                                   "the value set on it, a new one at its default. Inside the subcircuit a value uses one "
                                                   "as {Rs}.")}};
    if (!drawn.isEmpty())
        result.insert(QStringLiteral("symbol drawn"), tr("It had no symbol: one was drawn as make_symbol draws it - %1.").arg(drawn.join(QStringLiteral("; "))));
    if (reordered)
        result.insert(QStringLiteral("instances not open"),
                      tr("A schematic saved with an instance of it keeps the instance's values in order, not by name: one not open now "
                         "reads them by place when it opens, and a parameter taken away or moved shifts the values after it. Open those "
                         "schematics before saving this one - their instances then follow by name - and save them after."));
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
    // Where it is to be saved, checked first: a file there already kept the
    // document untitled, said only in its 'saved' field, and the next call
    // by that name found nothing open.
    const bool replace = args.value(QLatin1String("replace")).toBool();
    if (const QString saveAs = args.value(QLatin1String("save_as")).toString().trimmed(); !saveAs.isEmpty()) {
        if (const QString bad = badFileName(QFileInfo(saveAs).fileName()); !bad.isEmpty()) return errorResult(tr("'save_as': %1.").arg(bad));
        QString target = absolute(saveAs);
        if (QFileInfo(target).suffix().isEmpty()) target += QStringLiteral(".sch");
        if (QFileInfo(target).suffix().compare(QLatin1String("sch"), Qt::CaseInsensitive) != 0)
            return errorResult(tr("'save_as': %1 - a schematic's file ends in .sch.").arg(QFileInfo(target).fileName()));
        for (QucsDoc* doc : a_app->allDocuments())
            if (!doc->getDocName().isEmpty() && sameFile(doc->getDocName(), target))
                return errorResult(tr("%1 is open: close it, or choose another name. Nothing was imported.").arg(QDir::toNativeSeparators(target)));
        if (QFileInfo::exists(target) && !replace)
            return errorResult(tr("%1 exists: 'replace': true writes over it. Nothing was imported.").arg(QDir::toNativeSeparators(target)));
        if (!QFileInfo(QFileInfo(target).absolutePath()).isDir())
            return errorResult(tr("There is no folder %1. Nothing was imported.").arg(QDir::toNativeSeparators(QFileInfo(target).absolutePath())));
    }
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
    // (The file's first line itself - "; a comment" too: taken from the lines
    // read, with the comment gone, it was the first element that was lost.)
    QStringList raw = text.split(QLatin1Char('\n'));
    int firstAt = -1;
    for (int i = 0; i < raw.size() && firstAt < 0; ++i)
        if (!raw.at(i).trimmed().isEmpty()) firstAt = i;
    const QString first = firstAt >= 0 ? raw.at(firstAt).trimmed() : QString();
    const QJsonValue titleLine = args.value(QLatin1String("title_line"));
    const bool isTitle = titleLine.isBool() ? titleLine.toBool()
                                            : !first.startsWith(QLatin1Char('*')) && !first.startsWith(QLatin1Char('.')) && !looksLikeElement(first);
    if (isTitle && firstAt >= 0 && !first.startsWith(QLatin1Char('*'))) {
        title = first;
        if (title.startsWith(QLatin1Char(';'))) title = title.mid(1).trimmed();
        raw.removeAt(firstAt);
    }
    // A title of the caller's, in place of the netlist's.
    if (const QString given = args.value(QLatin1String("title")).toString().simplified(); !given.isEmpty()) title = given;
    QStringList lines = netlistLines(raw.join(QLatin1Char('\n')));

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
    // Two elements of one name (SPICE's names know no case): the second
    // renamed - ngspice refuses a netlist with both - and said.
    {
        QSet<QString> seen;
        for (NetlistElement& e : elements) {
            QString name = e.name;
            for (int k = 2; seen.contains(name.toLower()); ++k) name = QStringLiteral("%1_%2").arg(e.name).arg(k);
            if (name != e.name) {
                skipped << tr("%1: a second element of that name, placed as %2").arg(e.name, name);
                e.name = name;
            }
            seen.insert(name.toLower());
        }
    }
    // Each node's net: SPICE's names know no case (In and in are one node),
    // and a name made safe for a label (n+1 as n_1) is not another node's
    // name - two nodes merged so, and V1 and V2 were in parallel.
    QHash<QString, QString> netOf;   // a node (lower case) -> its net's name
    {
        QSet<QString> used;   // (lower case)
        QStringList order;
        for (const NetlistElement& e : std::as_const(elements))
            for (const QString& node : e.nodes)
                if (!order.contains(node.toLower())) order << node.toLower();
        QHash<QString, QString> spelled;   // lower -> as first written
        for (const NetlistElement& e : std::as_const(elements))
            for (const QString& node : e.nodes)
                if (!spelled.contains(node.toLower())) spelled.insert(node.toLower(), node);
        static const QRegularExpression unsafe(QStringLiteral("[^A-Za-z0-9_]"));
        // Safe names first, as they are; then the others, made safe.
        for (const QString& key : std::as_const(order))
            if (!spelled.value(key).contains(unsafe)) {
                netOf.insert(key, spelled.value(key));
                used.insert(key);
            }
        for (const QString& key : std::as_const(order)) {
            if (netOf.contains(key)) continue;
            QString base = spelled.value(key);
            base.replace(unsafe, QStringLiteral("_"));
            QString name = base;
            for (int k = 2; used.contains(name.toLower()); ++k) name = QStringLiteral("%1_%2").arg(base).arg(k);
            netOf.insert(key, name);
            used.insert(name.toLower());
        }
    }

    // The parts, as .sch lines, in rows.
    QStringList componentLines, wireLines, placed;
    QSet<QString> nets;
    int grounds = 0;
    const int spacing = std::clamp(args.value(QLatin1String("spacing")).toInt(200), 120, 600);
    const int perRow = std::clamp(int(std::ceil(std::sqrt(double(elements.size())))), 2, 8);
    int index = 0;
    const auto netName = [&netOf](const QString& node) { return netOf.value(node.toLower(), node); };
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
        // A name of its own: another import of the same title wrote over
        // the file the first one's schematic includes, and changed its parts.
        const QByteArray content = (QStringLiteral("* The subcircuits of %1, taken out by import_netlist\n").arg(source)
                                    + subcircuitText.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8();
        for (int k = 1;; ++k) {
            subcircuitFile = QDir(dir).filePath(stem + (k == 1 ? QStringLiteral("_subcircuits.lib") : QStringLiteral("_subcircuits-%1.lib").arg(k)));
            QFile there(subcircuitFile);
            if (!there.exists() || (there.open(QIODevice::ReadOnly) && there.readAll() == content)) break;
        }
        aboutToWrite(subcircuitFile);
        QFile f(subcircuitFile);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            f.write(content);
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
    QString content = QStringLiteral("<Components>\n%1\n</Components>\n<Wires>\n%2\n</Wires>\n")
                          .arg(componentLines.join(QLatin1Char('\n')), wireLines.join(QLatin1Char('\n')));
    // The title, a text above the parts (which start at 200, 200).
    if (!title.isEmpty()) {
        QString text = title.left(200);
        text.replace(QLatin1Char('"'), QLatin1Char('\'')).replace(QLatin1Char('\\'), QLatin1Char('/'));
        content += QStringLiteral("<Paintings>\n  <Text 100 60 14 #000000 0 \"%1\">\n</Paintings>\n").arg(text);
    }
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
        const QJsonObject saved = callNow(QStringLiteral("save_document"), {{QStringLiteral("as"), saveAs}, {QStringLiteral("replace"), replace}});
        result.insert(QStringLiteral("saved"), saved.value(QStringLiteral("isError")).toBool() ? textOf(saved) : QDir::toNativeSeparators(sch->getDocName()));
    }
    return jsonResult(result);
}
