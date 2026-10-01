/*
 * qucscontrol_results.cpp - the tools for Claude that read what a
 *                           simulation made: the netlist, the dataset as
 *                           numbers, the diagrams and their traces, and
 *                           the parts' types described
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

#include "components/component.h"
#include "components/libcomp.h"
#include "dataexport.h"
#include "dataimport.h"
#include "dataset.h"
#include "ngstatistics.h"
#include "vamodule.h"
#include "textdoc.h"
#include "diagrams/diagrams.h"
#include "diagrams/marker.h"
#include "extsimkernels/CdlNetlistWriter.h"
#include "extsimkernels/simulationrun.h"
#include "extsimkernels/spicecompat.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "node.h"
#include "oppoint.h"
#include "projectView.h"
#include "qucs.h"
#include "schematic.h"
#include "settings.h"

#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <tuple>

using namespace qucs_s::control;
namespace ds = qucs_s::dataset;
namespace di = qucs_s::dataimport;

namespace {

constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
constexpr double Degrees = 180.0 / 3.14159265358979323846;

// ----------------------------------------------------------------------
// Diagrams

struct DiagramKind {
    const char* name;   // as the tools say it
    const char* file;   // as the .sch file says it
    const char* what;
};
const DiagramKind kDiagramKinds[] = {
    {"rect", "Rect", "x-y (cartesian)"},
    {"polar", "Polar", "polar"},
    {"smith", "Smith", "Smith chart (impedance)"},
    {"admittance_smith", "ySmith", "Smith chart (admittance)"},
    {"polar_smith", "PS", "polar and Smith chart in one"},
    {"smith_polar", "SP", "Smith chart and polar in one"},
    {"tab", "Tab", "a table of the values"},
    {"timing", "Time", "timing diagram (digital)"},
    {"truth", "Truth", "truth table (digital)"},
    {"3d", "Rect3D", "3D cartesian"},
    {"locus", "Curve", "locus curve"},
    {"histogram", "Histogram", "histogram"},
};

QString kindName(const Diagram* d)
{
    for (const DiagramKind& k : kDiagramKinds)
        if (d->Name == QLatin1String(k.file)) return QString::fromLatin1(k.name);
    return d->Name;
}

Diagram* newDiagram(const QString& wanted)
{
    QString file;
    for (const DiagramKind& k : kDiagramKinds)
        if (wanted.compare(QLatin1String(k.name), Qt::CaseInsensitive) == 0 || wanted == QLatin1String(k.file)) file = QString::fromLatin1(k.file);
    if (file == QLatin1String("Rect")) return new RectDiagram();
    if (file == QLatin1String("Polar")) return new PolarDiagram();
    if (file == QLatin1String("Tab")) return new TabDiagram();
    if (file == QLatin1String("Smith")) return new SmithDiagram();
    if (file == QLatin1String("ySmith")) return new SmithDiagram(0, 0, false);
    if (file == QLatin1String("PS")) return new PSDiagram();
    if (file == QLatin1String("SP")) return new PSDiagram(0, 0, false);
    if (file == QLatin1String("Rect3D")) return new Rect3DDiagram();
    if (file == QLatin1String("Curve")) return new CurveDiagram();
    if (file == QLatin1String("Time")) return new TimingDiagram();
    if (file == QLatin1String("Truth")) return new TruthDiagram();
    if (file == QLatin1String("Histogram")) return new HistogramDiagram();
    return nullptr;
}

QString kindNames()
{
    QStringList names;
    for (const DiagramKind& k : kDiagramKinds) names << QString::fromLatin1(k.name);
    return names.join(QStringLiteral(", "));
}

// Whether a diagram of this kind has a right y axis for its traces.
bool hasRightAxis(const Diagram* d)
{
    return d->Name == QLatin1String("Rect") || d->Name == QLatin1String("PS") || d->Name == QLatin1String("SP")
           || d->Name == QLatin1String("Curve");
}

// Whether a diagram of this kind draws curves (colors, styles, markers).
bool drawsCurves(const Diagram* d)
{
    return d->Name != QLatin1String("Tab") && d->Name != QLatin1String("Truth");
}

const char* const kStyles[] = {"solid", "dash", "dot", "long_dash", "stars", "circles", "arrows"};
const char* const kMarkers[] = {"none", "auto", "circle", "square", "triangle", "diamond", "triangle_down", "cross", "plus"};
const char* const kLegends[] = {"off", "top_left", "top_right", "bottom_left", "bottom_right"};
const char* const kUnits[] = {"none", "dB", "dBuV", "dBm"};
// A trace's part of each value, by Graph::ValuePart's value.
const char* const kParts[] = {"auto", "magnitude", "db", "phase", "real", "imaginary"};

// What is easily misread in \a d: an axis's units on a linear axis (they
// label a logarithmic one's ticks in dB of the values, and do nothing on
// a linear one), and, when \a legendNote, several traces with no legend.
QStringList diagramNotes(const Diagram* d, bool legendNote)
{
    QStringList notes;
    for (const auto& [axis, name, number] : {std::tuple<const Axis*, const char*, int>{&d->yAxis, "y_axis", 0}, {&d->zAxis, "y2_axis", 1}}) {
        if (axis->Units <= 0 || axis->Units >= int(std::size(kUnits)) || axis->log) continue;
        const bool inDb = std::any_of(d->Graphs.cbegin(), d->Graphs.cend(), [n = number](const Graph* g) { return g->yAxisNo == n; })
                          && std::all_of(d->Graphs.cbegin(), d->Graphs.cend(), [n = number](const Graph* g) {
                                 return g->yAxisNo != n || g->valuePart == Graph::ValuePart::Db;
                             });
        notes << (inDb ? tr("%1's units %2 do nothing on a linear axis, and its traces are in dB already ('part': 'db'): the "
                            "units can go.")
                       : tr("%1's units %2 label a logarithmic axis's numbers (20 log10 of the values); this axis is linear, so "
                            "the values show as they are - 'log': true reads them so, or a trace's 'part': 'db' plots them in dB."))
                     .arg(QLatin1String(name), QLatin1String(kUnits[axis->Units]));
    }
    if (legendNote && drawsCurves(d) && d->Graphs.size() > 1 && d->legendPos == Diagram::LegendOff)
        notes << tr("No legend (a new diagram's is off, in the window too): 'legend': \"top_right\" says which curve is which.");
    return notes;
}
const char* const kNumbers[] = {"real_imaginary", "magnitude_degrees", "magnitude_radians"};
const char* const kIndicators[] = {"off", "square", "triangle"};
// The notations by numberformat::Notation's value (what a file keeps).
const char* const kNotations[] = {"automatic", "engineering", "scientific", "engineering_exponent", "decimal", "power_of_ten"};
// The ready-made themes, in diagramtheme::Preset's order, then the user's
// default for new diagrams.
const char* const kThemePresets[] = {"automatic", "light", "dark", "no_background", "default"};

template <size_t N>
int indexIn(const char* const (&names)[N], const QString& wanted)
{
    QString w = wanted.trimmed();
    w.replace(QLatin1Char(' '), QLatin1Char('_'));
    for (size_t i = 0; i < N; ++i)
        if (w.compare(QLatin1String(names[i]), Qt::CaseInsensitive) == 0) return int(i);
    return -1;
}

template <size_t N>
QString namesOf(const char* const (&names)[N])
{
    QStringList list;
    for (size_t i = 0; i < N; ++i) list << QString::fromLatin1(names[i]);
    return list.join(QStringLiteral(", "));
}

// A step for an axis from \a from to \a to: 1, 2 or 5 times a power of
// ten, some five of them across.
double niceStep(double from, double to)
{
    const double span = std::abs(to - from);
    if (!(span > 0) || !std::isfinite(span)) return 1;
    const double raw = span / 5;
    const double power = std::pow(10.0, std::floor(std::log10(raw)));
    const double f = raw / power;
    return (f < 1.5 ? 1 : f < 3.5 ? 2 : f < 7.5 ? 5 : 10) * power;
}

QJsonObject axisJson(const Axis& a, bool units)
{
    QJsonObject o{{QStringLiteral("label"), a.Label}, {QStringLiteral("log"), a.log}, {QStringLiteral("auto"), a.autoScale}};
    if (!a.autoScale) {
        o.insert(QStringLiteral("from"), a.limit_min);
        o.insert(QStringLiteral("to"), a.limit_max);
        o.insert(QStringLiteral("step"), a.step);
    }
    if (units && a.Units > 0 && a.Units < int(std::size(kUnits))) o.insert(QStringLiteral("units"), QString::fromLatin1(kUnits[a.Units]));
    return o;
}

// Sets an axis from {"label", "log", "auto", "from", "to", "step",
// "units"}; what is not given stays.
bool applyAxis(Axis* a, const QJsonObject& o, const QString& which, QString* error)
{
    if (o.contains(QLatin1String("label"))) {
        const QString label = o.value(QLatin1String("label")).toString();
        // (The file keeps a label between double quotes, on one line.)
        if (label.contains(QLatin1Char('"')) || label.contains(QLatin1Char('\n'))) {
            *error = tr("%1: a label has no double quotes or line breaks.").arg(which);
            return false;
        }
        a->Label = label;
    }
    if (o.contains(QLatin1String("log"))) a->log = o.value(QLatin1String("log")).toBool();
    if (o.contains(QLatin1String("units"))) {
        const int u = indexIn(kUnits, o.value(QLatin1String("units")).toString());
        if (u < 0) {
            *error = tr("%1: units are one of %2.").arg(which, namesOf(kUnits));
            return false;
        }
        a->Units = u;
    }
    const bool limits = o.contains(QLatin1String("from")) || o.contains(QLatin1String("to")) || o.contains(QLatin1String("step"));
    if (o.value(QLatin1String("auto")).toBool() && !limits) a->autoScale = true;
    if (limits) {
        double from = o.contains(QLatin1String("from")) ? o.value(QLatin1String("from")).toDouble(NaN) : a->limit_min;
        double to = o.contains(QLatin1String("to")) ? o.value(QLatin1String("to")).toDouble(NaN) : a->limit_max;
        if (a->autoScale && !(o.contains(QLatin1String("from")) && o.contains(QLatin1String("to")))) {
            // From automatic limits, those of the data now.
            if (!o.contains(QLatin1String("from"))) from = a->low;
            if (!o.contains(QLatin1String("to"))) to = a->up;
        }
        if (!std::isfinite(from) || !std::isfinite(to) || !(from < to)) {
            *error = tr("%1: 'from' must be less than 'to'.").arg(which);
            return false;
        }
        if (a->log && from <= 0) {
            *error = tr("%1 is logarithmic: 'from' must be above 0.").arg(which);
            return false;
        }
        double step = o.contains(QLatin1String("step")) ? o.value(QLatin1String("step")).toDouble(NaN) : niceStep(from, to);
        if (!std::isfinite(step) || step <= 0) {
            *error = tr("%1: 'step' must be above 0.").arg(which);
            return false;
        }
        if ((to - from) / step > Diagram::MaxGridLines) step = niceStep(from, to);
        a->limit_min = from;
        a->limit_max = to;
        a->step = step;
        a->autoScale = false;
    }
    return true;
}

// Sets the colours of a diagram's parts from {"preset", "background",
// "plot_area", "frame", ...}: the preset first, then each part given
// (#rrggbb, #aarrggbb, a name, or auto). Parts it does not have are left
// out; what is not given stays.
bool applyTheme(Diagram* d, const QJsonValue& v, QString* error)
{
    namespace theme = qucs_s::diagramtheme;
    QStringList partNames;
    for (theme::Part p : theme::allParts()) partNames << theme::keyOf(p);
    if (!v.isObject()) {
        *error = tr("'theme' is an object: {\"preset\": \"dark\"} or the colours of parts - %1.").arg(partNames.join(QStringLiteral(", ")));
        return false;
    }
    const QJsonObject o = v.toObject();
    theme::Theme t = d->theme();
    if (o.contains(QLatin1String("preset"))) {
        const int preset = indexIn(kThemePresets, o.value(QLatin1String("preset")).toString());
        if (preset < 0) {
            *error = tr("The theme's preset is one of %1.").arg(namesOf(kThemePresets));
            return false;
        }
        t = preset == int(std::size(kThemePresets)) - 1 ? theme::defaultForNewDiagrams() : theme::preset(theme::Preset(preset));
    }
    for (auto it = o.begin(); it != o.end(); ++it) {
        if (it.key() == QLatin1String("preset")) continue;
        const std::optional<theme::Part> part = theme::partNamed(it.key());
        if (!part) {
            *error = tr("A theme has no part %1: its parts are %2, and preset.").arg(it.key(), partNames.join(QStringLiteral(", ")));
            return false;
        }
        const QString text = it.value().toString().trimmed();
        if (text.compare(QLatin1String("auto"), Qt::CaseInsensitive) == 0 || text.compare(QLatin1String("automatic"), Qt::CaseInsensitive) == 0) {
            t.choose(*part, QColor());
            continue;
        }
        const QColor c = QColor::fromString(text);
        if (!c.isValid()) {
            *error = tr("theme.%1: %2 is no color - give #rrggbb, #aarrggbb (see-through), a name (white, darkgray, ...) or auto.")
                         .arg(it.key(), text.isEmpty() ? it.value().toVariant().toString() : text);
            return false;
        }
        t.choose(*part, c);
    }
    d->setTheme(t);
    return true;
}

// The colours chosen for a diagram's parts, by name; the others are
// automatic.
QJsonObject themeJson(const Diagram* d)
{
    namespace theme = qucs_s::diagramtheme;
    QJsonObject o;
    const theme::Theme t = d->theme();
    for (theme::Part p : theme::allParts())
        if (t.chosen(p).isValid()) o.insert(theme::keyOf(p), theme::colorText(t.chosen(p)));
    return o;
}

// Sets what a diagram's arguments give: place and size, grid, legend,
// theme, axes. What is not given stays.
bool applyDiagram(Diagram* d, const QJsonObject& args, QString* error)
{
    if (args.contains(QLatin1String("x"))) d->cx = misc::clampCoordinate(args.value(QLatin1String("x")).toInt());
    if (args.contains(QLatin1String("y"))) d->cy = misc::clampCoordinate(args.value(QLatin1String("y")).toInt());
    if (args.contains(QLatin1String("width"))) d->x2 = std::clamp(args.value(QLatin1String("width")).toInt(), 10, 100000);
    if (args.contains(QLatin1String("height"))) d->y2 = std::clamp(args.value(QLatin1String("height")).toInt(), 10, 100000);
    if (args.contains(QLatin1String("grid"))) d->xAxis.GridOn = d->yAxis.GridOn = args.value(QLatin1String("grid")).toBool();
    if (args.contains(QLatin1String("title"))) {
        const QString title = args.value(QLatin1String("title")).toString().trimmed();
        if (title.contains(QLatin1Char('"')) || title.contains(QLatin1Char('\n'))) {
            *error = tr("A title has no double quotes or line breaks (the file keeps it between quotes).");
            return false;
        }
        d->title = title;
    }
    if (args.contains(QLatin1String("legend"))) {
        const QJsonValue v = args.value(QLatin1String("legend"));
        const int pos = v.isBool() ? (v.toBool() ? int(Diagram::LegendTopRight) : int(Diagram::LegendOff)) : indexIn(kLegends, v.toString());
        if (pos < 0) {
            *error = tr("The legend is one of %1.").arg(namesOf(kLegends));
            return false;
        }
        d->legendPos = pos;
    }
    // A table and a truth table have no axes: each trace writes its numbers
    // with its own 'numbers' and 'precision'. (Taken, nothing used them.)
    if ((args.contains(QLatin1String("notation")) || args.contains(QLatin1String("decimals")))
        && (d->Name == QLatin1String("Tab") || d->Name == QLatin1String("Truth"))) {
        *error = tr("A %1 has no axes: 'notation' and 'decimals' are for diagrams with axes - a table writes each trace's numbers "
                    "with the trace's 'numbers' and 'precision' (edit_trace).")
                     .arg(d->Name == QLatin1String("Tab") ? tr("table") : tr("truth table"));
        return false;
    }
    if (args.contains(QLatin1String("notation"))) {
        const int n = indexIn(kNotations, args.value(QLatin1String("notation")).toString());
        if (n < 0) {
            *error = tr("'notation' is %1.").arg(namesOf(kNotations));
            return false;
        }
        d->notation = qucs_s::numberformat::fromInt(n);
    }
    if (args.contains(QLatin1String("decimals"))) {
        const QJsonValue v = args.value(QLatin1String("decimals"));
        if (!v.isDouble() || v.toDouble() != std::floor(v.toDouble()) || v.toDouble() < -1 || v.toDouble() > 15) {
            *error = tr("'decimals' is a whole number from 0 to 15, or -1: as many as each number needs.");
            return false;
        }
        d->notationDecimals = v.toInt();
    }
    if (args.contains(QLatin1String("theme")) && !applyTheme(d, args.value(QLatin1String("theme")), error)) return false;
    const struct {
        const char* key;
        Axis* axis;
    } axes[] = {{"x_axis", &d->xAxis}, {"y_axis", &d->yAxis}, {"y2_axis", &d->zAxis}};
    for (const auto& a : axes) {
        const QJsonValue v = args.value(QLatin1String(a.key));
        if (v.isUndefined() || v.isNull()) continue;
        if (!v.isObject()) {
            *error = tr("%1 is an object: {\"label\", \"log\", \"auto\", \"from\", \"to\", \"step\", \"units\"}.").arg(QLatin1String(a.key));
            return false;
        }
        if (!applyAxis(a.axis, v.toObject(), QLatin1String(a.key), error)) return false;
    }
    return true;
}

// Sets a trace's look from {"color", "thickness", "style", "axis",
// "marker", "auto_color", "precision", "numbers"}; what is not given stays.
bool applyTrace(Graph* g, const Diagram* d, const QJsonObject& o, QString* error)
{
    const QString color = o.value(QLatin1String("color")).toString().trimmed();
    if (color.compare(QLatin1String("auto"), Qt::CaseInsensitive) == 0) {
        g->autoColor = true;   // each curve its own
    } else if (o.contains(QLatin1String("color"))) {
        const QColor c = QColor::fromString(color);
        if (!c.isValid()) {
            *error = color.isEmpty() ? tr("'color' is #rrggbb, a name (red, darkgreen, ...) or auto.")
                                     : tr("%1 is no color: give #rrggbb, a name (red, darkgreen, ...) or auto.").arg(color);
            return false;
        }
        g->Color = c;
        // A color asked for is to be seen: not the automatic ones.
        if (!o.contains(QLatin1String("auto_color"))) g->autoColor = false;
    }
    if (o.contains(QLatin1String("thickness"))) g->Thick = std::clamp(o.value(QLatin1String("thickness")).toInt(), 0, 99);
    if (o.contains(QLatin1String("style"))) {
        const int style = indexIn(kStyles, o.value(QLatin1String("style")).toString());
        const int most = d->Name == QLatin1String("Time") ? GRAPHSTYLE_DOT : GRAPHSTYLE_ARROW;
        if (style < 0 || style > most) {
            QStringList allowed;
            for (int i = 0; i <= most; ++i) allowed << QString::fromLatin1(kStyles[i]);
            *error = tr("The style is one of %1.").arg(allowed.join(QStringLiteral(", ")));
            return false;
        }
        g->Style = toGraphStyle(style);
    }
    if (o.contains(QLatin1String("axis"))) {
        const QString axis = o.value(QLatin1String("axis")).toString();
        if (!hasRightAxis(d) && axis == QLatin1String("right")) {
            *error = tr("A %1 diagram has no right axis.").arg(kindName(d));
            return false;
        }
        if (axis != QLatin1String("left") && axis != QLatin1String("right")) {
            *error = tr("The axis is left or right.");
            return false;
        }
        g->yAxisNo = axis == QLatin1String("right") ? 1 : 0;
    }
    if (o.contains(QLatin1String("marker"))) {
        const int m = indexIn(kMarkers, o.value(QLatin1String("marker")).toString());
        if (m < 0) {
            *error = tr("The marker is one of %1.").arg(namesOf(kMarkers));
            return false;
        }
        g->pointMarker = Graph::PointMarker(m);
    }
    if (o.contains(QLatin1String("auto_color"))) g->autoColor = o.value(QLatin1String("auto_color")).toBool();
    if (o.contains(QLatin1String("part"))) {
        const int part = indexIn(kParts, o.value(QLatin1String("part")).toString().trimmed().toLower());
        if (part < 0) {
            *error = tr("A trace's 'part' is one of %1.").arg(namesOf(kParts));
            return false;
        }
        if (part != 0 && !Graph::valuePartApplies(d->Name)) {
            *error = tr("A %1 diagram plots the complex value itself: 'part' is for a Cartesian diagram or a table.").arg(kindName(d));
            return false;
        }
        g->valuePart = Graph::ValuePart(part);
    }
    if (o.contains(QLatin1String("precision"))) {
        const QJsonValue p = o.value(QLatin1String("precision"));
        if (!p.isDouble() || p.toDouble() != std::floor(p.toDouble()) || p.toDouble() < 0 || p.toDouble() > 16) {
            *error = tr("A trace's 'precision' is a whole number from 0 to 16: a table's digits.");
            return false;
        }
        g->Precision = p.toInt();
    }
    if (o.contains(QLatin1String("numbers"))) {
        const int n = indexIn(kNumbers, o.value(QLatin1String("numbers")).toString());
        if (n < 0) {
            *error = tr("The numbers are one of %1.").arg(namesOf(kNumbers));
            return false;
        }
        g->numMode = n;
    }
    return true;
}

// The dataset file and the variable in it that a trace shows - as
// Graph::loadDatFile() finds them: "ngspice/tran.v(out)" is tran.v(out)
// in name.dat.ngspice, "other:v(out)" v(out) in other.dat.
QString traceFile(Schematic* sch, const QString& var, QString* variable)
{
    const QFileInfo doc(sch->getDocName());
    const QString dir = doc.absolutePath();
    QString tail;
    QString svar = var;
    const qsizetype slash = var.indexOf(QLatin1Char('/'));
    if (slash > 0) {
        tail = QLatin1Char('.') + var.left(slash);
        svar = var.mid(slash + 1);
    }
    const qsizetype colon = svar.indexOf(QLatin1Char(':'));
    QString file;
    if (colon <= 0) {
        file = dir + QLatin1Char('/') + sch->getDataSet() + tail;
    } else {
        file = dir + QLatin1Char('/') + svar.left(colon) + QStringLiteral(".dat") + tail;
        svar = svar.mid(colon + 1);
    }
    if (svar.contains(QLatin1Char('@'))) svar = svar.section(QLatin1Char('@'), 0, 0);
    *variable = svar;
    return file;
}

// The file of \a sch's own dataset for the simulator in the settings.
QString defaultDataFile(Schematic* sch)
{
    const QString prefix = simulatorPrefix();
    return QFileInfo(sch->getDocName()).absolutePath() + QLatin1Char('/') + sch->getDataSet()
           + (prefix.isEmpty() ? QString() : QLatin1Char('.') + prefix);
}

// The analyses of \a sch's active simulations that write curves.
QStringList analysesOf(Schematic* sch)
{
    QStringList list;
    for (Component* c : sch->a_DocComps) {
        if (!c->isSimulation || c->isActive != COMP_IS_ACTIVE) continue;
        if (c->Model == QLatin1String(".TR") && !list.contains(QLatin1String("tran"))) list << QStringLiteral("tran");
        if (c->Model == QLatin1String(".AC") && !list.contains(QLatin1String("ac"))) list << QStringLiteral("ac");
    }
    return list;
}

// What a trace of \a d in \a sch shows for what was asked: the name its
// dataset has for it, with the simulator's prefix. \a note: what to know
// (it has no data yet); empty with \a error when it cannot be told.
QString traceVariable(Schematic* sch, const Diagram* d, const QString& wanted, QString* note, QString* error)
{
    const QString w = wanted.trimmed();
    if (w.isEmpty()) {
        *error = tr("Which variable? ('variable': as get_dataset lists them)");
        return QString();
    }
    QString sim;
    const QString bare = ds::withoutSimulator(w, &sim);
    // Qucsator's datasets have no suffix (name.dat): qucsator/ asks for
    // one of no simulator, which a diagram reads without a prefix (with
    // it, from a name.dat.qucsator that is never there).
    if (sim == QLatin1String("qucsator")) return bare;
    // As the diagram's dialog writes it (with its simulator): as it is.
    if (!sim.isEmpty()) return w;
    const QString prefix = simulatorPrefix();
    const auto named = [&prefix](const QString& name) { return prefix.isEmpty() ? name : prefix + QLatin1Char('/') + name; };
    // Of another dataset ("run1:tran.v(out)", one simulate kept): the
    // simulator's, as the current run's - unless that dataset is no
    // simulator's (imported, as import_data and the Import tab make them:
    // name.dat, read by name:variable; ngspice/name:... looked for a
    // name.dat.ngspice that is not there).
    if (bare.contains(QLatin1Char(':'))) {
        const QString folder = sch->getDocName().isEmpty() ? QString() : QFileInfo(sch->getDocName()).absolutePath();
        if (!plainDataset(folder, bare.section(QLatin1Char(':'), 0, 0)).isEmpty()) return bare;
        return named(bare);
    }
    const QString file = defaultDataFile(sch);
    ds::Dataset data;
    if (!sch->getDocName().isEmpty() && QFileInfo::exists(file) && data.read(file)) {
        QStringList names = data.resolve(bare);
        names.erase(std::remove_if(names.begin(), names.end(), [&data](const QString& n) {
                        const ds::Variable* v = data.find(n);
                        return v != nullptr && v->independent;
                    }),
                    names.end());
        if (names.size() > 1) {
            // A Smith or polar chart shows an AC analysis's.
            const bool ac = d->Name.contains(QLatin1String("Smith")) || d->Name == QLatin1String("Polar")
                            || d->Name == QLatin1String("PS") || d->Name == QLatin1String("SP");
            if (ac) {
                QStringList acNames;
                for (const QString& n : names)
                    if (ds::analysisOf(n) == QLatin1String("ac") || ds::analysisOf(n) == QLatin1String("sp")) acNames << n;
                if (acNames.size() == 1) names = acNames;
            }
        }
        if (names.size() == 1) return named(names.first());
        if (names.size() > 1) {
            *error = tr("%1 may be any of %2: say which.").arg(w, names.join(QStringLiteral(", ")));
            return QString();
        }
        *note = tr("%1 has no variable %2: the trace shows nothing until a simulation writes it.").arg(QFileInfo(file).fileName(), bare);
    } else {
        *note = tr("There is no dataset yet (%1): the trace shows data after a simulation.").arg(QFileInfo(file).fileName());
    }
    // A dataset imported beside it that has it: named so, it shows now.
    if (!sch->getDocName().isEmpty()) {
        const QString folder = QFileInfo(sch->getDocName()).absolutePath();
        QString source;
        if (const QString name = importedWith(folder, bare, &source); !name.isEmpty())
            *note += QLatin1Char(' ') + tr("%1 (imported from %2) has %3: the trace %1:%3 shows it.").arg(name, shownFrom(folder, source), bare);
    }
    // A SPICE simulator's variable is named after its analysis: when the
    // schematic has one analysis, that one.
    QString name = bare;
    if (!prefix.isEmpty()) {
        // A node's voltage (out: v(out), ac.out: ac.v(out)) - not a name a
        // NutmegEq computes (ac.gain_db, gain_db), which is the dataset's
        // as it is.
        QSet<QString> computed;
        for (const Component* c : sch->a_DocComps)
            if (c->Model == QLatin1String("NutmegEq"))
                for (const Property* p : c->Props)
                    if (p->Name != QLatin1String("Simulation")) computed << p->Name.toLower();
        const QString analysis = ds::analysisOf(name);
        const QString rest = analysis.isEmpty() ? name : name.mid(analysis.size() + 1);
        if (!rest.contains(QLatin1Char('(')) && !computed.contains(rest.toLower()))
            name = (analysis.isEmpty() ? QString() : analysis + QLatin1Char('.')) + QStringLiteral("v(%1)").arg(rest);
        if (prefix != QLatin1String("xyce")) name = name.toLower();   // ngspice writes its names so
        if (ds::analysisOf(name).isEmpty()) {
            const QStringList analyses = analysesOf(sch);
            if (analyses.size() == 1) name = analyses.first() + QLatin1Char('.') + name;
            else if (analyses.size() > 1) {
                *error = tr("%1 has several analyses (%2): say which, e.g. %3.%4.")
                             .arg(QFileInfo(sch->getDocName()).fileName(), analyses.join(QStringLiteral(", ")), analyses.first(), name);
                return QString();
            }
        }
    }
    return named(name);
}

// Loads the traces of \a d afresh from their datasets.
void reloadDiagram(Schematic* sch, Diagram* d)
{
    for (Graph* g : d->Graphs) g->lastLoaded = QDateTime();
    const QFileInfo info(sch->getDocName());
    d->loadGraphData(info.absolutePath() + QDir::separator() + sch->getDataSet());
}

QJsonObject traceJson(Schematic* sch, const Diagram* d, Graph* g, int number)
{
    QJsonObject t{{QStringLiteral("trace"), number}, {QStringLiteral("variable"), g->Var}};
    if (g->valuePart != Graph::ValuePart::Auto) t.insert(QStringLiteral("part"), QString::fromLatin1(kParts[int(g->valuePart)]));
    if (drawsCurves(d)) {
        t.insert(QStringLiteral("color"), g->Color.name());
        if (g->autoColor) t.insert(QStringLiteral("auto_color"), true);
        t.insert(QStringLiteral("thickness"), g->Thick);
        if (g->Style >= 0 && g->Style < GRAPHSTYLE_COUNT) t.insert(QStringLiteral("style"), QString::fromLatin1(kStyles[g->Style]));
        if (g->pointMarker != Graph::PointMarker::None) t.insert(QStringLiteral("marker"), QString::fromLatin1(kMarkers[int(g->pointMarker)]));
        if (hasRightAxis(d)) t.insert(QStringLiteral("axis"), g->yAxisNo == 1 ? QStringLiteral("right") : QStringLiteral("left"));
    } else if (d->Name == QLatin1String("Tab")) {
        t.insert(QStringLiteral("precision"), g->Precision);
        if (g->numMode >= 0 && g->numMode < int(std::size(kNumbers))) t.insert(QStringLiteral("numbers"), QString::fromLatin1(kNumbers[g->numMode]));
    }
    if (!g->isEmpty()) {
        t.insert(QStringLiteral("points"), int(g->count(0)));
        if (g->countY > 1) t.insert(QStringLiteral("curves"), g->countY);
    } else {
        t.insert(QStringLiteral("no data"), whyNoData(sch, g));
    }
    return t;
}

// ----------------------------------------------------------------------
// Datasets

int simulatorNamed(const QString& name)
{
    if (name == QLatin1String("ngspice")) return spicecompat::simNgspice;
    if (name == QLatin1String("xyce")) return spicecompat::simXyce;
    if (name == QLatin1String("spiceopus") || name == QLatin1String("spopus")) return spicecompat::simSpiceOpus;
    if (name == QLatin1String("qucsator")) return spicecompat::simQucsator;
    return -1;
}

// What a document's file says its dataset is (<DataSet=...>).
QString dataSetInFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    for (int i = 0; i < 40 && !f.atEnd(); ++i) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.startsWith(QLatin1String("<DataSet=")) && line.endsWith(QLatin1Char('>'))) return line.mid(9).chopped(1);
    }
    return QString();
}

bool isDatasetFile(const QString& path)
{
    const QString name = QFileInfo(path).fileName();
    return name.endsWith(QLatin1String(".dat")) || name.contains(QLatin1String(".dat.")) || ds::Dataset::isTable(path);
}

// Samples of \a count, spread evenly over \a indices.
QList<int> spread(const QList<int>& indices, int count)
{
    QList<int> picked;
    const int n = int(indices.size());
    if (count <= 0 || n == 0) return picked;
    if (count >= n) return indices;
    if (count == 1) return {indices.first()};
    for (int j = 0; j < count; ++j) picked << indices.at(int(std::llround(double(j) * (n - 1) / (count - 1))));
    return picked;
}

struct ReadOptions {
    double from = NaN, to = NaN;
    int points = 0;
    QList<double> at;
    QStringList measure;
    ds::MeasureOptions measureOptions;
    std::optional<bool> decibels;   // as said; else told from the unit
    ds::Form form = ds::Form::MagnitudePhase;
    QString prefix;   // the simulator's, for the name of a trace
    QString dataset;  // a dataset not the schematic's own (imported, a run kept): its name, for a trace (name:variable)
    QHash<QString, QString> definitions;   // the equations' variables (lower case): what they are defined as
};

// The trace that shows \a variable of the dataset read: ngspice/v(out),
// ngspice/run1:v(out) of a run kept, m:gain of an imported one; none of a
// Qucsator run's own (its variable as it is).
QString traceOf(const ReadOptions& o, const QString& variable)
{
    const QString named = o.dataset.isEmpty() ? variable : o.dataset + QLatin1Char(':') + variable;
    if (!o.prefix.isEmpty()) return o.prefix + QLatin1Char('/') + named;
    return o.dataset.isEmpty() ? QString() : named;
}

// What an equation of the schematic defines a variable of the dataset as
// ("y" of ac.y: db(norm(v(out)))), or empty.
QString definitionOf(const ReadOptions& o, const QString& name)
{
    return o.definitions.value(ds::bareName(ds::withoutSimulator(name)).toLower());
}

// The unit, the definition and how a variable was written: what tells a
// reader what its numbers are before measuring them.
void describe(QJsonObject& out, const ds::Variable& v, const ReadOptions& o)
{
    const QString definition = definitionOf(o, v.name);
    const QString unit = ds::unitOf(v.name, definition);
    if (!unit.isEmpty()) out.insert(QStringLiteral("units"), unit);
    if (!definition.isEmpty()) out.insert(QStringLiteral("defined as"), definition);
    if (v.writtenComplex) {
        out.insert(QStringLiteral("real"), true);
        out.insert(QStringLiteral("written"), QStringLiteral("complex, with no imaginary part: read as real, sign and all"));
    }
}

// The variables a schematic's equations define - Qucsator's Eqn, ngspice's
// NutmegEq - and what each is defined as, by name in lower case (ngspice
// writes its vectors so).
QHash<QString, QString> definitionsIn(Schematic* sch)
{
    QHash<QString, QString> definitions;
    for (Component* c : sch->a_DocComps) {
        if (c->Model != QLatin1String("Eqn") && c->Model != QLatin1String("NutmegEq")) continue;
        for (const Property* p : c->Props) {
            if (p->Name == QLatin1String("Export") || p->Name == QLatin1String("Simulation")) continue;
            definitions.insert(p->Name.trimmed().toLower(), p->Value.trimmed());
        }
    }
    return definitions;
}

// A complex value as the form gives it.
QJsonArray complexParts(double re, double im, ds::Form form)
{
    switch (form) {
    case ds::Form::RealImaginary: return {ds::rounded(re), ds::rounded(im)};
    case ds::Form::DbPhase: {
        const double mag = std::hypot(re, im);
        return {mag > 0 ? ds::rounded(20 * std::log10(mag)) : QJsonValue(QJsonValue::Null), ds::rounded(std::atan2(im, re) * Degrees)};
    }
    default: return {ds::rounded(std::hypot(re, im)), ds::rounded(std::atan2(im, re) * Degrees)};
    }
}

QJsonValue number(double v)
{
    return std::isfinite(v) ? QJsonValue(ds::rounded(v)) : QJsonValue(QJsonValue::Null);
}

// A device's quantity as the dataset names it (@jt1[id]): the device and
// the quantity.
bool deviceQuantity(const QString& name, QString* device, QString* quantity)
{
    static const QRegularExpression re(QStringLiteral("^@([^\\[\\]]+)\\[([^\\[\\]]+)\\]$"));
    const QRegularExpressionMatch m = re.match(name);
    if (!m.hasMatch()) return false;
    *device = m.captured(1);
    *quantity = m.captured(2);
    return true;
}

QString operatingUnit(const QString& name)
{
    QString device, quantity;
    if (deviceQuantity(name, &device, &quantity)) return qucs_s::oppoint::unitOf(QString(), quantity);
    return ds::unitOf(name);
}

// What a device's quantities make plain, each named with its formula: a
// transistor's small-signal resistances and gains (re = 1/gm of a BJT,
// rpi, beta, ro; a FET's intrinsic gain gm/gds).
QJsonObject derivedQuantities(const QList<qucs_s::oppoint::Parameter>& parameters)
{
    QHash<QString, double> v;
    for (const auto& p : parameters) v.insert(p.name.toLower(), p.value);
    QJsonObject d;
    const auto has = [&v](const char* k) { return v.contains(QLatin1String(k)) && v.value(QLatin1String(k)) > 0; };
    if (has("gm")) d.insert(QStringLiteral("1/gm (re of a BJT)"), number(1 / v.value(QStringLiteral("gm"))));
    if (has("gpi")) {
        d.insert(QStringLiteral("rpi = 1/gpi"), number(1 / v.value(QStringLiteral("gpi"))));
        if (has("gm")) d.insert(QStringLiteral("beta = gm/gpi"), number(v.value(QStringLiteral("gm")) / v.value(QStringLiteral("gpi"))));
    }
    if (has("go")) d.insert(QStringLiteral("ro = 1/go"), number(1 / v.value(QStringLiteral("go"))));
    if (has("gds")) {
        d.insert(QStringLiteral("ro = 1/gds"), number(1 / v.value(QStringLiteral("gds"))));
        if (has("gm")) d.insert(QStringLiteral("gm/gds (intrinsic gain)"), number(v.value(QStringLiteral("gm")) / v.value(QStringLiteral("gds"))));
    }
    return d;
}

// The operating point in a dataset - an op analysis's node values and,
// with ngspice, every device's quantities - each device under the
// component of the schematic it is (T1 for ngspice's jt1). With
// \a devicesInFull false, only which devices there are when there are many.
QJsonObject operatingPointJson(const ds::Dataset& data, Schematic* sch, bool devicesInFull)
{
    QJsonObject nodes;
    QList<qucs_s::oppoint::Device> devices;
    QJsonObject units;
    for (const ds::Variable& v : data.variables()) {
        if (!ds::isOperatingPointValue(data, v)) continue;
        QString device, quantity;
        if (deviceQuantity(v.name, &device, &quantity)) {
            auto it = std::find_if(devices.begin(), devices.end(), [&device](const auto& d) { return d.name == device; });
            if (it == devices.end()) {
                devices.append(qucs_s::oppoint::Device{});
                devices.last().name = device;
                it = devices.end() - 1;
            }
            it->parameters.append({quantity, v.re.first()});
            if (const QString u = operatingUnit(v.name); !u.isEmpty()) units.insert(quantity, u);
        } else {
            nodes.insert(v.name, number(v.re.first()));
            if (const QString u = operatingUnit(v.name); !u.isEmpty()) units.insert(v.name, u);
        }
    }
    if (nodes.isEmpty() && devices.isEmpty()) return {};
    if (sch != nullptr) {
        QStringList names;
        for (Component* c : sch->a_DocComps) names << c->Name;
        qucs_s::oppoint::attribute(devices, names);
    }
    QJsonObject op;
    if (!nodes.isEmpty()) op.insert(QStringLiteral("nodes"), nodes);
    if (!devices.isEmpty()) {
        QJsonArray list;
        const bool full = devicesInFull || devices.size() <= 12;
        for (const auto& d : devices) {
            QJsonObject e{{QStringLiteral("device"), d.name}};
            if (!d.component.isEmpty()) e.insert(QStringLiteral("component"), d.component);
            if (!d.inside.isEmpty()) e.insert(QStringLiteral("inside"), d.inside);
            if (full) {
                QJsonObject values;
                for (const auto& p : d.parameters) values.insert(p.name, number(p.value));
                e.insert(QStringLiteral("values"), values);
                if (const QJsonObject derived = derivedQuantities(d.parameters); !derived.isEmpty())
                    e.insert(QStringLiteral("derived"), derived);
            }
            list.append(e);
        }
        op.insert(QStringLiteral("devices"), list);
        if (!full) op.insert(QStringLiteral("note"), tr("operating_point: true gives the devices' values."));
    }
    op.insert(QStringLiteral("units"), units);
    return op;
}

QJsonObject variableJson(const ds::Dataset& data, const ds::Variable& v, const ReadOptions& o)
{
    QJsonObject out{{QStringLiteral("name"), v.name}};
    if (ds::isOperatingPointValue(data, v)) {
        out.insert(QStringLiteral("operating point"), true);
        out.insert(QStringLiteral("value"), number(v.re.first()));
        if (const QString u = operatingUnit(v.name); !u.isEmpty()) out.insert(QStringLiteral("units"), u);
        return out;
    }
    if (v.independent) {
        double lo = std::numeric_limits<double>::infinity(), hi = -lo;
        for (double x : v.re)
            if (std::isfinite(x)) {
                lo = std::min(lo, x);
                hi = std::max(hi, x);
            }
        out.insert(QStringLiteral("independent"), true);
        out.insert(QStringLiteral("points"), v.size());
        out.insert(QStringLiteral("from"), number(v.re.value(0, NaN)));
        out.insert(QStringLiteral("to"), number(v.re.value(v.size() - 1, NaN)));
        out.insert(QStringLiteral("min"), number(lo));
        out.insert(QStringLiteral("max"), number(hi));
        return out;
    }
    if (const QString trace = traceOf(o, v.name); !trace.isEmpty()) out.insert(QStringLiteral("trace"), trace);
    describe(out, v, o);
    ds::MeasureOptions measureOptions = o.measureOptions;
    measureOptions.decibels = o.decibels.value_or(ds::isDecibels(ds::unitOf(v.name, definitionOf(o, v.name))));
    const QString xName = v.dependencies.value(0, QStringLiteral("index"));
    out.insert(QStringLiteral("x"), xName);
    if (v.dependencies.size() > 1) out.insert(QStringLiteral("swept"), QJsonArray::fromStringList(v.dependencies.mid(1)));
    QJsonArray columns{xName};
    if (v.isComplex()) {
        out.insert(QStringLiteral("complex"), true);
        switch (o.form) {
        case ds::Form::RealImaginary: columns << QStringLiteral("real") << QStringLiteral("imaginary"); break;
        case ds::Form::DbPhase: columns << QStringLiteral("dB") << QStringLiteral("phase (degrees)"); break;
        default: columns << QStringLiteral("magnitude") << QStringLiteral("phase (degrees)"); break;
        }
        out.insert(QStringLiteral("statistics of"), QStringLiteral("the magnitude"));
    } else {
        columns << v.name;
    }

    QList<QList<QPair<QString, double>>> outer;
    const QList<ds::Curve> curves = ds::curvesOf(data, v, &outer);
    // (The loop gain's margins need its phase too.)
    const bool wantsPhase = v.isComplex() && (o.measure.contains(QStringLiteral("phase_margin")) || o.measure.contains(QStringLiteral("gain_margin")));
    const QList<QVector<double>> phases = wantsPhase ? ds::phasesOf(data, v) : QList<QVector<double>>();
    QJsonArray curveList;
    int offset = 0;
    const int most = 50;
    for (int k = 0; k < curves.size(); ++k) {
        const ds::Curve& full = curves.at(k);
        const int length = int(full.x.size());
        if (k >= most) {
            offset += length;
            continue;
        }
        QList<int> inRange;
        for (int i = 0; i < length; ++i) {
            const double x = full.x.at(i);
            if (!std::isnan(o.from) && x < o.from) continue;
            if (!std::isnan(o.to) && x > o.to) continue;
            inRange << i;
        }
        const ds::Curve part = ds::within(full, o.from, o.to);
        QJsonObject c;
        if (!outer.value(k).isEmpty()) {
            QJsonObject parameters;
            for (const auto& p : outer.at(k)) parameters.insert(p.first, number(p.second));
            c.insert(QStringLiteral("parameters"), parameters);
        }
        c.insert(QStringLiteral("points in range"), int(inRange.size()));
        if (part.x.isEmpty()) {
            curveList.append(c);
            offset += length;
            continue;
        }
        const ds::Stats s = ds::statsOf(part);
        c.insert(QStringLiteral("from"), number(part.x.first()));
        c.insert(QStringLiteral("to"), number(part.x.last()));
        c.insert(QStringLiteral("min"), number(s.min));
        c.insert(QStringLiteral("at min"), number(s.xMin));
        c.insert(QStringLiteral("max"), number(s.max));
        c.insert(QStringLiteral("at max"), number(s.xMax));
        c.insert(QStringLiteral("peak to peak"), number(s.max - s.min));
        c.insert(QStringLiteral("mean"), number(s.mean));
        c.insert(QStringLiteral("rms"), number(s.rms));
        c.insert(QStringLiteral("initial"), number(s.first));
        c.insert(QStringLiteral("final"), number(s.last));

        // The values at the x asked for, straight between the samples.
        if (!o.at.isEmpty()) {
            ds::Curve re{full.x, {}}, im{full.x, {}};
            for (int i = 0; i < length; ++i) {
                re.y << v.re.value(offset + i, NaN);
                if (v.isComplex()) im.y << v.im.value(offset + i, NaN);
            }
            QJsonArray values;
            for (double x : o.at) {
                const double r = ds::valueAt(re, x);
                QJsonArray row{number(x)};
                if (std::isnan(r)) row << QJsonValue(QJsonValue::Null);
                else if (v.isComplex()) {
                    for (const QJsonValue& part2 : complexParts(r, ds::valueAt(im, x), o.form)) row << part2;
                } else row << number(r);
                values.append(row);
            }
            c.insert(QStringLiteral("at"), values);
        }
        if (!o.measure.isEmpty()) {
            QJsonObject m;
            ds::MeasureOptions options = measureOptions;
            if (k < phases.size() && phases.at(k).size() == full.x.size())
                options.phase = ds::within(ds::Curve{full.x, phases.at(k)}, o.from, o.to).y;
            for (const QString& what : o.measure) m.insert(what, ds::measure(part, what, options));
            c.insert(QStringLiteral("measurements"), m);
        }
        if (o.points > 0) {
            QJsonArray samples;
            for (int i : spread(inRange, o.points)) {
                QJsonArray row{number(full.x.at(i))};
                if (v.isComplex()) {
                    for (const QJsonValue& p : complexParts(v.re.value(offset + i), v.im.value(offset + i), o.form)) row << p;
                } else row << number(v.re.value(offset + i, NaN));
                samples.append(row);
            }
            c.insert(QStringLiteral("samples"), samples);
        }
        curveList.append(c);
        offset += length;
    }
    // A family measured: a row for each curve - bandwidth against R, the
    // overshoot of each sample - its parameters, then each value asked for.
    if (curves.size() > 1 && (!o.measure.isEmpty() || !o.at.isEmpty())) {
        QStringList head;
        for (const auto& p : outer.value(0)) head << p.first;
        if (head.isEmpty()) head << QStringLiteral("curve");
        for (double x : o.at) head << QStringLiteral("at %1").arg(x);
        for (const QString& m : o.measure) head << m;
        QJsonArray rows;
        const int most = 1000;
        for (int k = 0; k < curves.size() && k < most; ++k) {
            QJsonArray row;
            for (const auto& p : outer.value(k)) row << number(p.second);
            if (outer.value(k).isEmpty()) row << k + 1;
            for (double x : o.at) {
                const double y = ds::valueAt(curves.at(k), x);
                row << (std::isnan(y) ? QJsonValue(QJsonValue::Null) : QJsonValue(number(y)));
            }
            const ds::Curve part = ds::within(curves.at(k), o.from, o.to);
            ds::MeasureOptions options = measureOptions;
            if (k < phases.size() && phases.at(k).size() == curves.at(k).x.size())
                options.phase = ds::within(ds::Curve{curves.at(k).x, phases.at(k)}, o.from, o.to).y;
            for (const QString& what : o.measure) {
                const QJsonValue value = part.x.size() < 2 ? QJsonValue() : ds::measure(part, what, options).value(QStringLiteral("value"));
                row << (value.isDouble() ? value : QJsonValue(QJsonValue::Null));
            }
            rows.append(row);
        }
        QJsonObject table{{QStringLiteral("columns"), QJsonArray::fromStringList(head)}, {QStringLiteral("rows"), rows}};
        if (curves.size() > most) table.insert(QStringLiteral("note"), tr("the first %1 curves of %2").arg(most).arg(curves.size()));
        if (!o.measure.isEmpty()) table.insert(QStringLiteral("values"), tr("each measurement's value (null: none on that curve; 'curves' tells why)"));
        out.insert(QStringLiteral("table"), table);
    }
    if (o.points > 0 || !o.at.isEmpty()) out.insert(QStringLiteral("columns"), columns);
    if (curveList.size() == 1 && outer.value(0).isEmpty()) {
        const QJsonObject c = curveList.first().toObject();
        for (auto it = c.begin(); it != c.end(); ++it) out.insert(it.key(), it.value());
    } else {
        out.insert(QStringLiteral("curves"), curveList);
        if (curves.size() > most)
            out.insert(QStringLiteral("note"), tr("The first %1 curves of %2 only.").arg(most).arg(curves.size()));
    }
    return out;
}

// ----------------------------------------------------------------------
// Component types

// The unit of a property from its default ("1 kHz": Hz) or what it says
// of itself ("in Ohms").
QString unitOf(const Property* p)
{
    static const QStringList units = {QStringLiteral("V"),  QStringLiteral("A"),   QStringLiteral("Ohm"), QStringLiteral("F"),
                                      QStringLiteral("H"),  QStringLiteral("Hz"),  QStringLiteral("s"),   QStringLiteral("W"),
                                      QStringLiteral("dB"), QStringLiteral("dBm"), QStringLiteral("K"),   QStringLiteral("S"),
                                      QStringLiteral("m"),  QStringLiteral("deg"), QStringLiteral("J"),   QStringLiteral("C")};
    static const QRegularExpression valued(QStringLiteral("^\\s*[-+]?[0-9.]+(?:[eE][-+]?[0-9]+)?\\s+([A-Za-z]+)\\s*$"));
    const QRegularExpressionMatch m = valued.match(p->Value);
    if (m.hasMatch()) {
        const QString u = m.captured(1);
        if (units.contains(u)) return u;
        static const QString prefixes = QStringLiteral("afpnumkMGT");
        if (u.size() > 1 && prefixes.contains(u.at(0)) && units.contains(u.mid(1))) return u.mid(1);
    }
    static const QRegularExpression said(QStringLiteral("\\bin (Volts?|Ohms?|Hertz|Farads?|Henry|Amperes?|seconds?|Watts?|dB|Kelvin|degrees?(?: Celsius)?)\\b"),
                                         QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch s = said.match(p->Description);
    if (s.hasMatch()) {
        const QString w = s.captured(1).toLower();
        if (w.startsWith(QLatin1String("volt"))) return QStringLiteral("V");
        if (w.startsWith(QLatin1String("ohm"))) return QStringLiteral("Ohm");
        if (w == QLatin1String("hertz")) return QStringLiteral("Hz");
        if (w.startsWith(QLatin1String("farad"))) return QStringLiteral("F");
        if (w == QLatin1String("henry")) return QStringLiteral("H");
        if (w.startsWith(QLatin1String("ampere"))) return QStringLiteral("A");
        if (w.startsWith(QLatin1String("second"))) return QStringLiteral("s");
        if (w.startsWith(QLatin1String("watt"))) return QStringLiteral("W");
        if (w == QLatin1String("db")) return QStringLiteral("dB");
        if (w == QLatin1String("kelvin")) return QStringLiteral("K");
        if (w.startsWith(QLatin1String("degree"))) return w.contains(QLatin1String("celsius")) ? QStringLiteral("°C") : QStringLiteral("deg");
    }
    return QString();
}

QJsonArray simulatorsOf(int mask)
{
    QJsonArray list;
    if (mask & spicecompat::simNgspice) list << QStringLiteral("ngspice");
    if (mask & spicecompat::simXyce) list << QStringLiteral("Xyce");
    if (mask & spicecompat::simSpiceOpus) list << QStringLiteral("SPICE OPUS");
    if (mask & spicecompat::simQucsator) list << QStringLiteral("Qucsator");
    return list;
}

// What a type does that is easy to get wrong: known traps.
QStringList notesOn(const QString& type)
{
    QStringList notes;
    if (type == QLatin1String("Vpulse") || type == QLatin1String("Ipulse")) {
        const QString other = type.at(0) == QLatin1Char('V') ? QStringLiteral("Vrect") : QStringLiteral("Irect");
        notes << tr("One pulse, not a train: it rises at T1, falls so that it ends at T2 and stays at the first value "
                    "after. Under the SPICE simulators it is netlisted as PULSE with a period of 1e9 s, beyond any run (a "
                    "PULSE with no period is not one pulse under ngspice: it repeats, high most of the time). For a "
                    "repeating pulse train use %1.").arg(other);
    }
    if (type == QLatin1String("Vrect") || type == QLatin1String("Irect"))
        notes << tr("A repeating rectangular wave: high for TH, low for TL, with edges Tr and Tf between them (period "
                    "TH + TL + Tr + Tf), from Td on. Under the SPICE simulators it is netlisted as PULSE with that "
                    "period; the value outside the pulses is U0 (I0).");
    if (type == QLatin1String("Vdc") || type == QLatin1String("Idc"))
        notes << tr("A DC value only: nothing in an AC analysis. To drive an AC analysis use Vac (Iac), whose "
                    "amplitude is also the AC magnitude unless its ACmag is set.");
    if (type == QLatin1String("Vac") || type == QLatin1String("Iac"))
        notes << tr("One source for a transient and an AC analysis: its %1 is the sine's peak and, unless ACmag is set, "
                    "the AC magnitude as well - so %1 = 0 to silence the transient silences the AC analysis too (every "
                    "voltage of it 0, and check_schematic says so). ACmag (SPICE simulators) sets the AC magnitude "
                    "apart: %1 = 0.1 for a small-signal transient with ACmag = 1 for a transfer function.")
                     .arg(type == QLatin1String("Vac") ? QStringLiteral("U") : QStringLiteral("I"));
    if (type == QLatin1String(".TR"))
        notes << tr("Under ngspice the transient's print step is (Stop - Start) / (Points - 1): the dataset has at least "
                    "Points samples; raise Points for finer results. MaxStep bounds the simulator's own step.");
    if (type == QLatin1String("GND"))
        notes << tr("Every circuit needs a ground (the SPICE node 0); its pin is GND.1 to connect.");
    if (type == QLatin1String("Eqn") || type == QLatin1String("NutmegEq"))
        notes << tr("Under ngspice an equation that uses no simulated voltage or current becomes a .PARAM line; one "
                    "that does is computed after each analysis from its results, and is in the dataset (NutmegEq "
                    "does that for one analysis you name).");
    if (type == QLatin1String("NutmegEq") || type == QLatin1String("Eqn"))
        notes << tr("Name its variables unlike the circuit's nodes and net labels: under ngspice an equation's variable "
                    "and a node of one name clash (a variable out beside the node out) - the node's voltage or the "
                    "equation's result comes out wrong, without an error. Names such as gain_db or vout_pp are safe.");
    if (type == QLatin1String("NutmegEq"))
        notes << tr("Simulation: the analysis its equations are computed after - its name (TR1, AC1), its kind by "
                    "ngspice's name (tran, ac, dc or op, noise, sp, ...: every analysis of that kind), or ALL.");
    static const QSet<QString> equationBlocks{QStringLiteral("Eqn"), QStringLiteral("NutmegEq"), QStringLiteral("SpiceIC"),
                                              QStringLiteral("SpicePar"), QStringLiteral("SpiceOptions"), QStringLiteral("SpiceFunc"),
                                              QStringLiteral("SpiceCSPar"), QStringLiteral("SpGlobPar"), QStringLiteral("SpiceNodeset")};
    if (equationBlocks.contains(type))
        notes << tr("Its properties are its equations - name = value, as many as wanted: add_component and "
                    "edit_component set them with 'equations' ({\"gain_db\": \"db(v(out))\"}, or a list of "
                    "\"name=value\" in their order).");
    if (type == QLatin1String("SpiceOptions"))
        notes << tr("An option with no value is a flag, written alone (.OPTION noinit): 'flags' ([\"noinit\", "
                    "\"keepopinfo\"]) or {\"noinit\": true} in 'equations' on add_component and edit_component; "
                    "{\"noinit\": \"\"} takes it away. get_schematic marks it \"flag\": true.");
    if (type == QLatin1String(qucs_s::ngstats::kMonteCarloModel) || type == QLatin1String(qucs_s::ngstats::kCornersModel))
        notes << tr("What it records for each sample and the limits a sample passes within: 'records' ([{\"name\": "
                    "\"gain\", \"expression\": \"db(v(out))\"}]) and 'specs' ([{\"expression\": \"gain\", \"min\": "
                    "\"19\"}]) on add_component and edit_component. get_dataset reads the results "
                    "(ngmontecarlo1.gain: a value per sample) and measures their distribution.");
    return notes;
}

} // namespace

namespace qucs_s::control {

Component* newComponent(const QString& type, Module** module)
{
    // A library part (its library and component its properties Lib and
    // Comp): made by hand as the file loader makes it, in no module - "no
    // component type Lib", though find_library_component said to use it.
    if (type == QLatin1String("Lib")) return new LibComp();
    if (Module::Modules.contains(type)) {
        if (module != nullptr) *module = Module::Modules.value(type);
        return Module::getComponent(type);
    }
    for (Category* category : Category::Categories)
        for (Module* m : category->Content)
            if (typeOf(m) == type && m->info != nullptr) {
                QString name;
                char* file = nullptr;
                if (module != nullptr) *module = m;
                return dynamic_cast<Component*>(m->info(name, file, true));
            }
    return nullptr;
}

QString typeOf(Module* module)
{
    // Not kept: the library is made again when the simulator changes.
    for (auto it = Module::Modules.cbegin(); it != Module::Modules.cend(); ++it)
        if (it.value() == module) return it.key();
    if (module->info == nullptr) return QString();
    QString name;
    char* file = nullptr;
    std::unique_ptr<Element> e(module->info(name, file, true));
    const auto* c = dynamic_cast<Component*>(e.get());
    return c != nullptr ? c->Model : QString();
}

QString simulatorPrefix()
{
    switch (QucsSettings.DefaultSimulator) {
    case spicecompat::simNgspice: return QStringLiteral("ngspice");
    case spicecompat::simXyce: return QStringLiteral("xyce");
    case spicecompat::simSpiceOpus: return QStringLiteral("spopus");
    default: return QString();
    }
}

QString noSimulatorText()
{
    return tr("No simulator is chosen: Qucs-S found none installed. Install ngspice (or say where it is: Simulation > "
              "Simulators Settings...), then set_simulator chooses it.");
}

QString datasetFile(const QString& schematic, const QString& dataSet, int simulator)
{
    const QFileInfo info(schematic);
    const QString dir = info.absolutePath() + QLatin1Char('/');
    const QString base = dataSet.isEmpty() ? info.completeBaseName() + QStringLiteral(".dat") : dataSet;
    switch (simulator) {
    case spicecompat::simNgspice: return dir + base + QStringLiteral(".ngspice");
    case spicecompat::simXyce: return dir + base + QStringLiteral(".xyce");
    case spicecompat::simSpiceOpus: return dir + base + QStringLiteral(".spopus");
    default: return dir + base;
    }
}

QList<Marker*> markersOf(const Diagram* d)
{
    QList<Marker*> list;
    for (Graph* g : d->Graphs) list << g->Markers;
    return list;
}

QJsonObject markerJson(const Diagram* d, Marker* m, int index)
{
    const Graph* g = m->graph();
    QJsonObject o{{QStringLiteral("marker"), index},
                  {QStringLiteral("trace"), int(d->Graphs.indexOf(const_cast<Graph*>(g))) + 1},
                  {QStringLiteral("variable"), g->Var}};
    // The sample it shows: its independent variables' values, and the value.
    QJsonObject at;
    const std::vector<double>& pos = m->varPos();
    for (unsigned i = 0; i < g->numAxes() && i < pos.size(); ++i) at.insert(g->axis(i)->Var, number(pos[i]));
    if (!at.isEmpty()) o.insert(QStringLiteral("at"), at);
    if (!g->isEmpty())
        o.insert(QStringLiteral("value"), m->powImag() == 0 ? number(m->powReal()) : QJsonValue(QJsonArray{number(m->powReal()), number(m->powImag())}));
    else
        o.insert(QStringLiteral("no data"), tr("its trace shows no data"));
    o.insert(QStringLiteral("text"), m->Text.trimmed());
    o.insert(QStringLiteral("label"), QJsonArray{d->cx + m->x1, d->cy + m->y1});
    o.insert(QStringLiteral("precision"), m->Precision);
    if (m->numMode >= 0 && m->numMode < int(std::size(kNumbers))) o.insert(QStringLiteral("format"), QString::fromLatin1(kNumbers[m->numMode]));
    o.insert(QStringLiteral("notation"), m->notation >= 0 && m->notation < int(std::size(kNotations)) ? QString::fromLatin1(kNotations[m->notation])
                                                                                                        : QStringLiteral("diagram"));
    o.insert(QStringLiteral("transparent"), m->transparent);
    if (int(m->indicatorMode) >= 0 && int(m->indicatorMode) < int(std::size(kIndicators)))
        o.insert(QStringLiteral("indicator"), QString::fromLatin1(kIndicators[int(m->indicatorMode)]));
    const auto colour = [](const QColor& c) {
        return !c.isValid() ? QStringLiteral("auto") : c.name(c.alpha() < 255 ? QColor::HexArgb : QColor::HexRgb);
    };
    o.insert(QStringLiteral("text_color"), colour(m->textColor));
    o.insert(QStringLiteral("fill_color"), colour(m->fillColor));
    return o;
}

Marker* markerOf(const Diagram* d, const QJsonValue& which, QString* error)
{
    const QList<Marker*> all = markersOf(d);
    if (all.isEmpty()) {
        *error = tr("The diagram has no marker (add_marker places one).");
        return nullptr;
    }
    if (which.isUndefined() || which.isNull()) {
        if (all.size() == 1) return all.first();
        *error = tr("The diagram has %1 markers: say which ('marker', its number as get_schematic gives it).").arg(all.size());
        return nullptr;
    }
    const int n = which.toInt(which.toString().toInt());
    if (n < 1 || n > all.size()) {
        *error = tr("There is no marker %1: they are numbered 1 to %2.").arg(n).arg(all.size());
        return nullptr;
    }
    return all.at(n - 1);
}

QJsonArray diagramsJson(Schematic* sch)
{
    QJsonArray list;
    int n = 0;
    for (Diagram* d : sch->a_DocDiags) {
        ++n;
        QJsonObject o{{QStringLiteral("diagram"), n},
                      {QStringLiteral("type"), kindName(d)},
                      {QStringLiteral("x"), d->cx},
                      {QStringLiteral("y"), d->cy},
                      {QStringLiteral("width"), d->x2},
                      {QStringLiteral("height"), d->y2}};
        if (!d->title.isEmpty()) o.insert(QStringLiteral("title"), d->title);
        if (const QJsonObject colors = themeJson(d); !colors.isEmpty()) o.insert(QStringLiteral("theme"), colors);
        if (d->Name != QLatin1String("Tab") && d->Name != QLatin1String("Truth")) {
            o.insert(QStringLiteral("x_axis"), axisJson(d->xAxis, false));
            o.insert(QStringLiteral("y_axis"), axisJson(d->yAxis, true));
            if (hasRightAxis(d)) o.insert(QStringLiteral("y2_axis"), axisJson(d->zAxis, true));
            o.insert(QStringLiteral("grid"), d->xAxis.GridOn);
            if (d->legendPos >= 0 && d->legendPos < int(std::size(kLegends)))
                o.insert(QStringLiteral("legend"), QString::fromLatin1(kLegends[d->legendPos]));
            o.insert(QStringLiteral("notation"), QString::fromLatin1(kNotations[int(d->notation)]));
            if (d->notationDecimals >= 0) o.insert(QStringLiteral("decimals"), d->notationDecimals);
        }
        QJsonArray traces;
        int t = 0;
        for (Graph* g : d->Graphs) traces.append(traceJson(sch, d, g, ++t));
        o.insert(QStringLiteral("traces"), traces);
        QJsonArray markers;
        int m = 0;
        for (Marker* mk : markersOf(d)) markers.append(markerJson(d, mk, ++m));
        if (!markers.isEmpty()) o.insert(QStringLiteral("markers"), markers);
        list.append(o);
    }
    return list;
}

Diagram* diagramOf(Schematic* sch, const QJsonValue& which, QString* error)
{
    const int count = int(sch->a_DocDiags.size());
    if (count == 0) {
        *error = tr("%1 has no diagram (add_diagram places one).")
                     .arg(sch->getDocName().isEmpty() ? tr("The untitled schematic") : QFileInfo(sch->getDocName()).fileName());
        return nullptr;
    }
    if (which.isUndefined() || which.isNull()) {
        if (count == 1) return sch->a_DocDiags.front();
        *error = tr("There are %1 diagrams: say which ('diagram', its number as get_schematic gives it).").arg(count);
        return nullptr;
    }
    const int n = which.toInt(which.toString().toInt());
    if (n < 1 || n > count) {
        *error = tr("There is no diagram %1: they are numbered 1 to %2.").arg(which.isString() ? which.toString() : QString::number(which.toInt())).arg(count);
        return nullptr;
    }
    auto it = sch->a_DocDiags.begin();
    std::advance(it, n - 1);
    return *it;
}

Graph* traceOf(Diagram* d, const QJsonValue& which, QString* error)
{
    if (d->Graphs.isEmpty()) {
        *error = tr("The diagram has no trace.");
        return nullptr;
    }
    if (which.isUndefined() || which.isNull()) {
        if (d->Graphs.size() == 1) return d->Graphs.first();
        *error = tr("The diagram has %1 traces: say which ('trace': its number or its variable).").arg(d->Graphs.size());
        return nullptr;
    }
    if (which.isDouble()) {
        const int n = which.toInt();
        if (n >= 1 && n <= d->Graphs.size()) return d->Graphs.at(n - 1);
        *error = tr("There is no trace %1: they are numbered 1 to %2.").arg(n).arg(d->Graphs.size());
        return nullptr;
    }
    const QString w = which.toString().trimmed();
    for (Graph* g : d->Graphs)
        if (g->Var == w) return g;
    // Without the simulator, or in another case.
    QList<Graph*> like;
    for (Graph* g : d->Graphs) {
        const QString bare = ds::withoutSimulator(g->Var);
        if (bare.compare(w, Qt::CaseInsensitive) == 0 || ds::bareName(bare).compare(ds::withoutSimulator(w), Qt::CaseInsensitive) == 0)
            like << g;
    }
    if (like.size() == 1) return like.first();
    QStringList vars;
    for (Graph* g : d->Graphs) vars << g->Var;
    *error = like.isEmpty() ? tr("The diagram has no trace %1 (it has %2).").arg(w, vars.join(QStringLiteral(", ")))
                            : tr("%1 may be any of several traces: give its number.").arg(w);
    return nullptr;
}

QString datasetOfTrace(Schematic* sch, const QString& var, QString* variable)
{
    return traceFile(sch, var, variable);
}

QString plainDataset(const QString& folder, const QString& name, QString* source)
{
    if (folder.isEmpty() || name.isEmpty()) return QString();
    const QDir dir(folder);
    const QString plain = dir.filePath(name + QStringLiteral(".dat"));
    if (!QFileInfo(plain).isFile()) return QString();
    di::Origin origin;
    if (di::originOf(plain, &origin)) {
        if (source != nullptr) *source = origin.source;
        return plain;
    }
    // (A simulation's copy beside it, name.dat.ngspice: that is the one.)
    const QString prefix = simulatorPrefix();
    if (!prefix.isEmpty() && QFileInfo::exists(dir.filePath(name + QStringLiteral(".dat.") + prefix))) return QString();
    return plain;
}

namespace {
// The names of a dataset's dependent variables, from its headers alone
// (<dep name ...>), kept while its file is as it was: a trace of no data
// read every imported dataset beside it in full for a hint - 47 MB each,
// for every trace of every listing.
QSet<QString> dependentNames(const QString& path)
{
    struct Known {
        QDateTime at;
        qint64 size = -1;
        QSet<QString> names;
    };
    static QHash<QString, Known> known;
    const QFileInfo info(path);
    if (const auto it = known.constFind(info.absoluteFilePath());
        it != known.constEnd() && it->size == info.size() && it->at == info.lastModified())
        return it->names;
    Known k{info.lastModified(), info.size(), {}};
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QByteArray read;
    const uchar* mapped = f.size() > 0 ? f.map(0, f.size()) : nullptr;
    if (mapped == nullptr) read = f.readAll();
    const QByteArrayView all = mapped != nullptr ? QByteArrayView(mapped, f.size()) : QByteArrayView(read);
    for (qsizetype at = all.indexOf("<dep "); at >= 0; at = all.indexOf("<dep ", at + 5)) {
        qsizetype end = at + 5;
        while (end < all.size() && all.at(end) != ' ' && all.at(end) != '>' && all.at(end) != '\n') ++end;
        if (end > at + 5) k.names.insert(QString::fromUtf8(all.sliced(at + 5, end - at - 5)));
    }
    if (known.size() > 500) known.clear();
    known.insert(info.absoluteFilePath(), k);
    return k.names;
}
} // namespace

QString importedWith(const QString& folder, const QString& variable, QString* source)
{
    if (folder.isEmpty() || variable.isEmpty()) return QString();
    int looked = 0;
    for (const di::Imported& i : di::importedIn(folder)) {
        if (++looked > 20) break;
        if (!dependentNames(i.path).contains(variable)) continue;
        if (source != nullptr) *source = i.origin.source;
        return i.name;
    }
    return QString();
}

QString shownFrom(const QString& folder, const QString& file)
{
    const QString relative = QDir(folder).relativeFilePath(file);
    return QDir::toNativeSeparators(relative.startsWith(QLatin1String("..")) ? QFileInfo(file).absoluteFilePath() : relative);
}

QString whyNoData(Schematic* sch, Graph* g)
{
    if (!g->isEmpty()) return QString();
    if (sch->getDocName().isEmpty()) return tr("the document has no file yet, so no dataset");
    QString variable;
    const QString file = traceFile(sch, g->Var, &variable);
    if (!QFileInfo::exists(file)) {
        const QString folder = QFileInfo(sch->getDocName()).absolutePath();
        const QString missing = QFileInfo(file).fileName();
        const QString bare = ds::withoutSimulator(g->Var);
        // name:variable with a simulator's prefix, its name.dat no simulator's.
        if (bare != g->Var && bare.contains(QLatin1Char(':'))) {
            const QString name = bare.section(QLatin1Char(':'), 0, 0);
            const QString plain = QDir(folder).filePath(name + QStringLiteral(".dat"));
            di::Origin origin;
            if (di::originOf(plain, &origin))
                return tr("there is no dataset %1: %2.dat is imported (from %3), and its traces are %4, without a simulator's "
                          "prefix (edit_trace's 'variable' makes it so)")
                    .arg(missing, name, shownFrom(folder, origin.source), bare);
            if (QFileInfo(plain).isFile())
                return tr("there is no dataset %1; %2.dat is there, a dataset of no simulator (a Qucsator run's): its traces "
                          "are %3, without a prefix")
                    .arg(missing, name, bare);
        }
        QString why = tr("there is no dataset %1 (simulate to make it)").arg(missing);
        if (!bare.contains(QLatin1Char(':'))) {
            QString source;
            if (const QString name = importedWith(folder, variable, &source); !name.isEmpty())
                why += tr("; %1 (imported from %2) has %3: the trace %1:%3 shows it").arg(name, shownFrom(folder, source), variable);
        }
        return why;
    }
    ds::Dataset data;
    QString error;
    if (!data.read(file, &error)) return error;
    if (data.find(variable) == nullptr) {
        QStringList like = data.resolve(ds::bareName(variable));
        like.removeAll(variable);
        QString why = tr("%1 has no variable %2").arg(QFileInfo(file).fileName(), variable);
        if (!like.isEmpty()) return why + tr("; it has %1").arg(like.join(QStringLiteral(", ")));
        QStringList names;
        for (const ds::Variable& v : data.variables())
            if (!v.independent && names.size() < 12) names << v.name;
        return why + tr("; its variables: %1").arg(names.join(QStringLiteral(", ")));
    }
    return tr("%1 has %2, but the diagram cannot draw it").arg(QFileInfo(file).fileName(), variable);
}

QString renameNetIn(const QString& text, const QString& from, const QString& to)
{
    if (from.isEmpty() || from == to || text.isEmpty()) return text;
    const QString f = QRegularExpression::escape(from);
    // The case the simulator gave it: ngspice writes names in lower case.
    const auto cased = [&to, &from](const QString& found) {
        if (found == from && found != found.toLower()) return to;   // as written (V1): as given
        if (found == found.toLower() && found != found.toUpper()) return to.toLower();
        if (found == found.toUpper() && found != found.toLower()) return to.toUpper();
        return to;
    };
    const auto replaced = [&](const QString& in, const QRegularExpression& re) {
        QString out;
        qsizetype last = 0;
        for (auto it = re.globalMatch(in); it.hasNext();) {
            const QRegularExpressionMatch m = it.next();
            out += in.mid(last, m.capturedStart(2) - last);
            out += cased(m.captured(2));
            last = m.capturedEnd(2);
        }
        return out + in.mid(last);
    };
    // SPICE: v(out), vdb(out), v(out,in), v(in,out).
    const QRegularExpression spice(QStringLiteral("(\\b[vV][a-zA-Z]{0,3}\\(\\s*(?:[^(),]*,\\s*)?)(%1)(?=\\s*[,)])").arg(f),
                                   QRegularExpression::CaseInsensitiveOption);
    // Qucsator: out.v, out.Vt, out.vn.
    const QRegularExpression qucsator(QStringLiteral("((?<![\\w.])|^)(%1)(?=\\.(?i:v|vt|vn|vb)\\b)").arg(f));
    return replaced(replaced(text, spice), qucsator);
}

QString renameComponentIn(const QString& text, const QString& from, const QString& to)
{
    if (from.isEmpty() || from == to || text.isEmpty()) return text;
    const QString f = QRegularExpression::escape(from);
    const auto cased = [&to, &from](const QString& found) {
        if (found == from && found != found.toLower()) return to;   // as written (V1): as given
        if (found == found.toLower() && found != found.toUpper()) return to.toLower();
        if (found == found.toUpper() && found != found.toLower()) return to.toUpper();
        return to;
    };
    const auto replaced = [&](const QString& in, const QRegularExpression& re) {
        QString out;
        qsizetype last = 0;
        for (auto it = re.globalMatch(in); it.hasNext();) {
            const QRegularExpressionMatch m = it.next();
            out += in.mid(last, m.capturedStart(2) - last);
            out += cased(m.captured(2));
            last = m.capturedEnd(2);
        }
        return out + in.mid(last);
    };
    const auto ci = QRegularExpression::CaseInsensitiveOption;
    QString out = text;
    // SPICE: a source's or an inductor's current i(v1), a device's
    // quantity @q1[ic] (@m.x1.m1[gm] too), a branch v1#branch.
    out = replaced(out, QRegularExpression(QStringLiteral("(\\b[iI]\\(\\s*)(%1)(?=\\s*\\))").arg(f), ci));
    out = replaced(out, QRegularExpression(QStringLiteral("(@(?:[a-zA-Z]\\.)?)(%1)(?=\\[)").arg(f), ci));
    out = replaced(out, QRegularExpression(QStringLiteral("((?<!\\w)|^)(%1)(?=#branch\\b)").arg(f), ci));
    // Qucsator: V1.It, R1.I, the operating point D1.Id.
    out = replaced(out, QRegularExpression(QStringLiteral("((?<![\\w.@(])|^)(%1)(?=\\.[A-Za-z]\\w*)").arg(f)));
    return out;
}


QJsonObject operatingPointOfRun(Schematic* sch, const QString& scratch)
{
    // The node values: ngspice's "print all" ("out = 1.2"), or Xyce's
    // .PRINT (the names on one line, the values on the next).
    QJsonObject nodes, units;
    const QString ngspice = QDir(scratch).filePath(QStringLiteral("spice4qucs.cir.dc_op"));
    const QString xyce = QDir(scratch).filePath(QStringLiteral("spice4qucs.cir.dc_op_xyce"));
    const auto name = [](QString n) {
        n = n.trimmed();
        if (n.endsWith(QLatin1String("#branch"))) return QStringLiteral("i(%1)").arg(n.chopped(7));
        return n;
    };
    if (QFile f(ngspice); f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        for (const QString& line : QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'))) {
            if (!line.contains(QLatin1Char('='))) continue;
            bool ok = false;
            const double v = line.section(QLatin1Char('='), 1, 1).trimmed().toDouble(&ok);
            if (!ok) continue;
            const QString n = name(line.section(QLatin1Char('='), 0, 0));
            nodes.insert(n, number(v));
            if (const QString u = ds::unitOf(n); !u.isEmpty()) units.insert(n, u);
        }
    } else if (QFile x(xyce); x.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QStringList lines = QString::fromUtf8(x.readAll()).split(QLatin1Char('\n'));
        if (lines.size() >= 2) {
            const QStringList names = lines.at(0).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            const QStringList values = lines.at(1).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            for (int i = 0; i < names.size() && i < values.size(); ++i) nodes.insert(names.at(i).toLower(), number(values.at(i).toDouble()));
        }
    }
    QJsonArray devices;
    if (sch != nullptr)
        for (const qucs_s::oppoint::Device& d : sch->operatingPoint()) {
            QJsonObject e{{QStringLiteral("device"), d.name}, {QStringLiteral("type"), d.type}};
            if (!d.component.isEmpty()) e.insert(QStringLiteral("component"), d.component);
            if (!d.inside.isEmpty()) e.insert(QStringLiteral("inside"), d.inside);
            if (!d.model.isEmpty()) e.insert(QStringLiteral("model"), d.model);
            QJsonObject values;
            QList<qucs_s::oppoint::Parameter> operating;
            for (const auto& p : d.parameters) {
                if (!qucs_s::oppoint::isOperatingQuantity(d.type, p.name)) continue;
                values.insert(p.name, number(p.value));
                operating << p;
                if (const QString u = qucs_s::oppoint::unitOf(d.type, p.name); !u.isEmpty()) units.insert(p.name, u);
            }
            e.insert(QStringLiteral("values"), values);
            if (const QJsonObject derived = derivedQuantities(operating); !derived.isEmpty()) e.insert(QStringLiteral("derived"), derived);
            devices.append(e);
        }
    QJsonObject op;
    if (!nodes.isEmpty()) op.insert(QStringLiteral("nodes"), nodes);
    if (!devices.isEmpty()) op.insert(QStringLiteral("devices"), devices);
    if (!op.isEmpty()) op.insert(QStringLiteral("units"), units);
    return op;
}

} // namespace qucs_s::control

// ----------------------------------------------------------------------
// The tools

QString QucsControl::datasetPath(const QJsonObject& args, QString* error) const
{
    const QString path = args.value(QLatin1String("path")).toString().trimmed();
    const QString simArg = args.value(QLatin1String("simulator")).toString().trimmed().toLower();
    int simulator = QucsSettings.DefaultSimulator;
    if (!simArg.isEmpty()) {
        simulator = simulatorNamed(simArg);
        if (simulator < 0) {
            *error = tr("The simulator is ngspice, xyce, spiceopus or qucsator.");
            return QString();
        }
    }
    if (!path.isEmpty() && isDatasetFile(path)) {
        const QString file = absolute(path);
        if (!QFileInfo(file).isFile()) {
            *error = tr("There is no dataset %1.").arg(QDir::toNativeSeparators(file));
            return QString();
        }
        return file;
    }
    // A document: open, or a file.
    QString docName, dataSet;
    QString ignored;
    if (QucsDoc* doc = document(args, &ignored)) {
        docName = doc->getDocName();
        dataSet = doc->getDataSet();
        if (docName.isEmpty()) {
            *error = tr("%1 has no file yet, so no dataset.").arg(titleOf(doc));
            return QString();
        }
    } else if (!path.isEmpty() && QFileInfo(absolute(path)).isFile()) {
        docName = absolute(path);
        dataSet = dataSetInFile(docName);
    } else {
        *error = path.isEmpty() ? tr("No document is open: give 'path'.") : tr("There is no file %1.").arg(QDir::toNativeSeparators(absolute(path)));
        return QString();
    }
    const QString file = datasetFile(docName, dataSet, simulator);
    if (QFileInfo(file).isFile()) return file;
    if (!simArg.isEmpty()) {
        *error = tr("There is no dataset %1 (simulate with %2 first).").arg(QDir::toNativeSeparators(file), simArg);
        return QString();
    }
    // Another simulator's: the newest there is.
    QString newest;
    for (int sim : {int(spicecompat::simNgspice), int(spicecompat::simXyce), int(spicecompat::simSpiceOpus), int(spicecompat::simQucsator)}) {
        const QString other = datasetFile(docName, dataSet, sim);
        if (QFileInfo(other).isFile() && (newest.isEmpty() || QFileInfo(other).lastModified() > QFileInfo(newest).lastModified())) newest = other;
    }
    if (!newest.isEmpty()) return newest;
    *error = tr("There is no dataset of %1 yet (simulate first; it would be %2).")
                 .arg(QFileInfo(docName).fileName(), QDir::toNativeSeparators(file));
    return QString();
}

Schematic* QucsControl::schematicOfDataset(const QString& file, const QJsonObject& args) const
{
    // The document asked for, when it is the one whose dataset this is;
    // else an open schematic that writes this dataset.
    QString ignored;
    const auto writes = [&file](Schematic* sch) {
        if (sch == nullptr || sch->getDocName().isEmpty()) return false;
        for (int sim : {int(spicecompat::simNgspice), int(spicecompat::simXyce), int(spicecompat::simSpiceOpus), int(spicecompat::simQucsator)})
            if (sameFile(datasetFile(sch->getDocName(), sch->getDataSet(), sim), file)) return true;
        return false;
    };
    if (auto* sch = dynamic_cast<Schematic*>(document(args, &ignored)); writes(sch)) return sch;
    for (QucsDoc* doc : a_app->allDocuments())
        if (auto* sch = dynamic_cast<Schematic*>(doc); writes(sch)) return sch;
    return nullptr;
}

QList<Schematic*> QucsControl::showingDataOf(Schematic* sch) const
{
    QList<Schematic*> list{sch};
    const QFileInfo info(sch->getDocName());
    for (QucsDoc* doc : a_app->allDocuments()) {
        auto* other = dynamic_cast<Schematic*>(doc);
        if (other == nullptr || other == sch || other->getDocName().isEmpty()) continue;
        const QFileInfo o(other->getDocName());
        if (o.absolutePath() == info.absolutePath() && other->getDataSet() == sch->getDataSet()) list << other;
    }
    return list;
}

namespace {

// \a va of \a a beside \b vb of another run \a b: the other's statistics
// over the same range, its measurements, and the difference on this run's
// samples (the other's value straight between its samples): the largest
// and where, its mean and RMS. The first curve of a swept one.
QJsonObject comparedJson(const ds::Dataset& a, const ds::Variable& va, const ds::Dataset& b, const ds::Variable& vb, const ReadOptions& o)
{
    QJsonObject out;
    const QList<ds::Curve> mine = ds::curvesOf(a, va), theirs = ds::curvesOf(b, vb);
    if (mine.isEmpty() || theirs.isEmpty()) return {{QStringLiteral("note"), tr("nothing to compare")}};
    const ds::Curve here = ds::within(mine.first(), o.from, o.to), there = ds::within(theirs.first(), o.from, o.to);
    if (here.x.isEmpty() || there.x.isEmpty()) return {{QStringLiteral("note"), tr("no samples in the range to compare")}};
    const ds::Stats s = ds::statsOf(there);
    out.insert(QStringLiteral("min"), number(s.min));
    out.insert(QStringLiteral("max"), number(s.max));
    out.insert(QStringLiteral("peak to peak"), number(s.max - s.min));
    out.insert(QStringLiteral("mean"), number(s.mean));
    out.insert(QStringLiteral("rms"), number(s.rms));
    out.insert(QStringLiteral("final"), number(s.last));
    if (!o.measure.isEmpty()) {
        QJsonObject m;
        for (const QString& what : o.measure) m.insert(what, ds::measure(there, what, o.measureOptions));
        out.insert(QStringLiteral("measurements"), m);
    }
    // The difference, this run's less the other's, where both have data.
    const bool rising = there.x.size() < 2 || there.x.last() >= there.x.first();
    double worst = 0, worstAt = NaN, sum = 0, sumSq = 0;
    int n = 0;
    int j = 0;
    for (int i = 0; i < here.x.size(); ++i) {
        const double x = here.x.at(i);
        double y = NaN;
        if (rising) {
            while (j + 1 < there.x.size() && there.x.at(j + 1) < x) ++j;
            if (j + 1 < there.x.size() && there.x.at(j) <= x && x <= there.x.at(j + 1)) {
                const double x0 = there.x.at(j), x1 = there.x.at(j + 1);
                y = x1 > x0 ? there.y.at(j) + (x - x0) / (x1 - x0) * (there.y.at(j + 1) - there.y.at(j)) : there.y.at(j);
            } else if (there.x.size() == 1 && there.x.first() == x) y = there.y.first();
        } else {
            y = ds::valueAt(there, x);
        }
        if (!std::isfinite(y) || !std::isfinite(here.y.at(i))) continue;
        const double d = here.y.at(i) - y;
        if (std::abs(d) > std::abs(worst) || std::isnan(worstAt)) {
            worst = d;
            worstAt = x;
        }
        sum += d;
        sumSq += d * d;
        ++n;
    }
    if (n > 0)
        out.insert(QStringLiteral("difference (this run less that)"),
                   QJsonObject{{QStringLiteral("largest"), number(worst)}, {QStringLiteral("at"), number(worstAt)},
                               {QStringLiteral("mean"), number(sum / n)}, {QStringLiteral("rms"), number(std::sqrt(sumSq / n))},
                               {QStringLiteral("samples compared"), n}});
    else out.insert(QStringLiteral("note"), tr("the two runs' x ranges do not meet"));
    if (mine.size() > 1 || theirs.size() > 1) out.insert(QStringLiteral("of"), tr("the first curve of the sweep"));
    return out;
}

} // namespace

namespace {

// A netlist's lines as a simulator reads them: no comments (its first line
// names the file and the version), no blank lines.
QStringList netlistLines(const QString& file)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    QStringList lines;
    for (const QString& l : QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'))) {
        const QString t = l.trimmed();
        if (!t.isEmpty() && !t.startsWith(QLatin1Char('*'))) lines << t;
    }
    return lines;
}

} // namespace

QString QucsControl::staleness(Schematic* sch, const QString& file, bool* certain)
{
    bool sure = false;
    if (certain == nullptr) certain = &sure;
    *certain = false;
    const QDateTime written = QFileInfo(file).lastModified();
    const QString when = written.toString(QStringLiteral("HH:mm:ss"));
    // A run after it that failed: this is the run's before.
    const QucsDoc::Run run = sch->lastRun();
    if (run.at.isValid() && run.failed && run.at > written) {
        *certain = true;
        return tr("The last simulation of %1, at %2, failed: this dataset is of a run before it (written at %3), not of the "
                  "circuit as it is.").arg(titleOf(sch), run.at.toString(QStringLiteral("HH:mm:ss")), when);
    }
    // The netlist the run that wrote it was given, against the one a run
    // would be given now: kept for the dataset when the run wrote it (and
    // by copy_document for a copy's), else the last run's in the scratch
    // folder, when that wrote this dataset (the netlist is written as a
    // run begins, so the run's is the one no newer than its dataset).
    int simulator = spicecompat::simQucsator;
    if (file.endsWith(QLatin1String(".ngspice"))) simulator = spicecompat::simNgspice;
    else if (file.endsWith(QLatin1String(".spopus"))) simulator = spicecompat::simSpiceOpus;
    else if (file.endsWith(QLatin1String(".xyce"))) simulator = spicecompat::simXyce;
    const auto& edits = sch->recentEdits();
    const QDateTime edited = edits.isEmpty() ? QDateTime() : edits.last().at;
    if (simulator == QucsSettings.DefaultSimulator && (simulator == spicecompat::simNgspice || simulator == spicecompat::simSpiceOpus)) {
        QDateTime at;
        QString last = misc::runNetlistOf(file, &at);
        if (last.isEmpty()) {
            const QString scratch = QDir(misc::scratchDirFor(sch->getDocName())).filePath(QStringLiteral("spice4qucs.cir"));
            const QFileInfo info(scratch);
            if (QFile f(scratch); info.isFile() && info.lastModified() <= written.addSecs(2) && f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                last = QString::fromUtf8(f.readAll());
                at = info.lastModified();
            }
        }
        // It is this schematic's: "* Qucs 26.1.4  /path/amp.sch" first.
        static const QRegularExpression named(QStringLiteral("^\\*\\s*Qucs\\S*\\s+\\S+\\s+(.+)$"));
        const QRegularExpressionMatch m = named.match(last.section(QLatin1Char('\n'), 0, 0).trimmed());
        if (m.hasMatch() && sameFile(m.captured(1).trimmed(), sch->getDocName())) {
            // Nothing done to it since that run began - no edit, no part
            // made again (a value set for a while and put back), its file
            // not written since (by another program, before it was opened,
            // or a copy's): as it was, and not netlisted again (tune and
            // scripts read a dataset after every run).
            const QDateTime saved = QFileInfo(sch->getDocName()).lastModified();
            const QDateTime touched = sch->touched();
            if (!(edited.isValid() && edited > at) && !(saved.isValid() && saved > at) && !(touched.isValid() && touched > at)) return {};
            QTemporaryDir temporary;
            const QString now = temporary.filePath(QStringLiteral("now.cir"));
            misc::ErrorCapture capture;
            SimulationRun netlister(sch, false);
            if (netlister.writeNetlist(now)) {
                QStringList lines;
                for (const QString& l : last.split(QLatin1Char('\n')))
                    if (const QString t = l.trimmed(); !t.isEmpty() && !t.startsWith(QLatin1Char('*'))) lines << t;
                if (netlistLines(now) != lines) {
                    *certain = true;
                    return tr("%1 changed since the run that wrote this dataset (at %2): the netlist a simulation would be given now "
                              "is not the one that run was given - simulate again for results of the circuit as it is.")
                        .arg(titleOf(sch), when);
                }
                return {};
            }
        }
    }
    // Else what can be told: an edit after it (which may not matter - a
    // diagram, a move), and that no more can.
    if (edited.isValid() && edited > written)
        return tr("%1 was edited at %2, after this dataset was written (%3): if a value or a connection changed, the dataset is not "
                  "of the circuit as it is (simulate again). The netlist of the run that wrote it is not at hand to compare with "
                  "- it was written by another simulator than the one set now, or by another way than a simulation here - so "
                  "only the time tells.")
            .arg(titleOf(sch), edited.toString(QStringLiteral("HH:mm:ss")), when);
    return {};
}

namespace {
// The dataset \a stem in \a folder: the simulator's (stem.dat.ngspice when
// \a simulator says), an imported or Qucsator's (stem.dat), the one of the
// simulator in the settings - the first there; empty when none is, the
// files looked for in \a tried.
QString datasetCalled(const QString& folder, const QString& stem, const QString& simulator, QStringList* tried)
{
    for (const QString& suffix : {simulator.isEmpty() ? QString() : QStringLiteral(".dat.") + simulator, QStringLiteral(".dat"),
                                  QStringLiteral(".dat.") + simulatorPrefix()}) {
        if (suffix.isEmpty() || suffix == QLatin1String(".dat.")) continue;
        const QString candidate = QDir(folder).filePath(stem + suffix);
        if (tried != nullptr && !tried->contains(QFileInfo(candidate).fileName())) *tried << QFileInfo(candidate).fileName();
        if (QFileInfo(candidate).isFile()) return candidate;
    }
    return QString();
}
} // namespace

QString QucsControl::datasetNamedBy(const QJsonObject& args, const QJsonArray& wanted, const QString& tool, QString* error) const
{
    error->clear();
    const QString path = args.value(QLatin1String("path")).toString().trimmed();
    if (wanted.isEmpty() || (!path.isEmpty() && isDatasetFile(path))) return QString();
    QSet<QString> names;
    QString simulator;
    for (const QJsonValue& w : wanted) {
        QString sim;
        const QString bare = ds::withoutSimulator(w.toString().trimmed(), &sim);
        const qsizetype colon = bare.indexOf(QLatin1Char(':'));
        if (colon <= 0 || bare.left(colon).contains(QLatin1Char('('))) return QString();
        names.insert(bare.left(colon).toLower());
        if (!sim.isEmpty()) simulator = sim;
    }
    QString folder, ignored;
    if (QucsDoc* doc = document(args, &ignored); doc != nullptr && !doc->getDocName().isEmpty()) folder = QFileInfo(doc->getDocName()).absolutePath();
    else if (!path.isEmpty() && QFileInfo(absolute(path)).isFile()) folder = QFileInfo(absolute(path)).absolutePath();
    if (names.size() > 1) {
        *error = tr("The variables are of %1 datasets: %2 reads one at a time.").arg(names.size()).arg(tool);
        return QString();
    }
    if (folder.isEmpty()) return QString();
    const QString stem = ds::withoutSimulator(wanted.first().toString().trimmed()).section(QLatin1Char(':'), 0, 0);
    QStringList tried;
    const QString file = datasetCalled(folder, stem, simulator, &tried);
    if (file.isEmpty()) *error = tr("There is no dataset %1 beside it (%2).").arg(stem, tried.join(QStringLiteral(", ")));
    return file;
}

QJsonObject QucsControl::getDataset(const QJsonObject& args)
{
    QString error;
    // Its variables named name:variable, as the traces of a dataset beside
    // the schematic's own are (m:gain imported, ngspice/run1:v(out) kept):
    // that dataset's.
    QString file = datasetNamedBy(args, args.value(QLatin1String("variables")).toArray(), QStringLiteral("get_dataset"), &error);
    if (!error.isEmpty()) return errorResult(error);
    if (file.isEmpty()) file = datasetPath(args, &error);
    if (file.isEmpty()) return errorResult(error);
    ds::Dataset data;
    if (!data.read(file, &error)) return errorResult(error);
    const QFileInfo info(file);
    QJsonObject result{{QStringLiteral("dataset"), QDir::toNativeSeparators(file)},
                       {QStringLiteral("written"), info.lastModified().toString(Qt::ISODate)}};
    ReadOptions o;
    const qsizetype dot = info.fileName().indexOf(QLatin1String(".dat."));
    if (dot >= 0) o.prefix = info.fileName().mid(dot + 5);
    // Imported (import_data, the Import tab): where from, and its traces
    // are name:variable, with no simulator's prefix.
    QString importedName;
    if (di::Origin origin; di::originOf(file, &origin)) {
        importedName = info.completeBaseName();
        QJsonObject from{{QStringLiteral("file"), QDir::toNativeSeparators(origin.source)},
                         {QStringLiteral("format"), origin.format},
                         {QStringLiteral("when"), origin.imported.toString(Qt::ISODate)}};
        if (!origin.options.x.isEmpty())
            from.insert(QStringLiteral("x"), origin.options.x == di::rowX() ? QStringLiteral("the row") : origin.options.x);
        if (!origin.options.sheet.isEmpty()) from.insert(QStringLiteral("sheet"), origin.options.sheet);
        result.insert(QStringLiteral("imported from"), from);
    }
    if (Schematic* definer = schematicOfDataset(file, args)) o.definitions = definitionsIn(definer);
    // A dataset not of a schematic's Data Set - imported, a run keep_as
    // kept - is named in a trace: ngspice/v(out) is the schematic's own.
    if (!importedName.isEmpty()) {
        o.dataset = importedName;
    } else if (const qsizetype d = info.fileName().indexOf(QLatin1String(".dat")); d > 0 && schematicOfDataset(file, args) == nullptr) {
        const QString name = info.fileName().left(d);
        if (!di::dataSetsOfSchematics(info.absolutePath()).contains(name, Qt::CaseInsensitive)) o.dataset = name;
    }
    if (args.value(QLatin1String("decibels")).isBool()) o.decibels = args.value(QLatin1String("decibels")).toBool();

    Schematic* sch = schematicOfDataset(file, args);
    if (sch != nullptr) {
        bool certain = false;
        if (const QString stale = staleness(sch, file, &certain); !stale.isEmpty()) {
            result.insert(QStringLiteral("stale"), stale);
            result.insert(QStringLiteral("stale certain"), certain);
        }
    }
    if (args.value(QLatin1String("operating_point")).toBool()) {
        const QJsonObject op = operatingPointJson(data, sch, true);
        if (op.isEmpty())
            return errorResult(tr("%1 holds no operating point: the schematic needs a DC simulation (op) - its node "
                                  "values, and with ngspice every device's, are written with the others.")
                                   .arg(info.fileName()));
        result.insert(QStringLiteral("operating point"), op);
        return jsonResult(result);
    }
    const QJsonArray wanted = args.value(QLatin1String("variables")).toArray();
    if (wanted.isEmpty()) {
        QJsonArray independent, variables;
        for (const ds::Variable& v : data.variables()) {
            if (ds::isOperatingPointValue(data, v)) continue;
            if (v.independent) {
                independent.append(variableJson(data, v, o));
                continue;
            }
            QJsonObject e{{QStringLiteral("name"), v.name}, {QStringLiteral("depends on"), QJsonArray::fromStringList(v.dependencies)},
                          {QStringLiteral("points"), v.size()}};
            if (const QString trace = traceOf(o, v.name); !trace.isEmpty()) e.insert(QStringLiteral("trace"), trace);
            if (v.isComplex()) e.insert(QStringLiteral("complex"), true);
            describe(e, v, o);
            const ds::Curve all{QVector<double>(v.size(), 0), [&v] {
                                    QVector<double> y(v.size());
                                    for (int i = 0; i < v.size(); ++i) y[i] = v.isComplex() ? std::hypot(v.re.at(i), v.im.at(i)) : v.re.at(i);
                                    return y;
                                }()};
            const ds::Stats s = ds::statsOf(all);
            e.insert(v.isComplex() ? QStringLiteral("magnitude min") : QStringLiteral("min"), number(s.min));
            e.insert(v.isComplex() ? QStringLiteral("magnitude max") : QStringLiteral("max"), number(s.max));
            variables.append(e);
            if (variables.size() >= 400) break;
        }
        result.insert(QStringLiteral("independent variables"), independent);
        result.insert(QStringLiteral("variables"), variables);
        if (const QJsonObject op = operatingPointJson(data, sch, false); !op.isEmpty())
            result.insert(QStringLiteral("operating point"), op);
        return jsonResult(result);
    }

    o.from = args.value(QLatin1String("from")).toDouble(NaN);
    o.to = args.value(QLatin1String("to")).toDouble(NaN);
    for (const QJsonValue& x : args.value(QLatin1String("at")).toArray())
        if (x.isDouble()) o.at << x.toDouble();
    for (const QJsonValue& m : args.value(QLatin1String("measure")).toArray()) o.measure << m.toString();
    const QStringList known = ds::measurements();
    for (const QString& m : std::as_const(o.measure))
        if (!known.contains(m)) return errorResult(tr("There is no measurement %1: %2.").arg(m, known.join(QStringLiteral(", "))));
    if (args.contains(QLatin1String("level"))) o.measureOptions.level = args.value(QLatin1String("level")).toDouble(NaN);
    if (args.contains(QLatin1String("tolerance")))
        o.measureOptions.tolerance = std::clamp(args.value(QLatin1String("tolerance")).toDouble(0.02), 1e-6, 0.5);
    // thd: the fundamental, the harmonics counted, the periods it is measured on.
    if (args.contains(QLatin1String("fundamental"))) {
        const double f = args.value(QLatin1String("fundamental")).toDouble(NaN);
        if (!(f > 0) || !std::isfinite(f)) return errorResult(tr("'fundamental' is a frequency in Hz, above 0."));
        o.measureOptions.fundamental = f;
    }
    if (args.contains(QLatin1String("harmonics"))) {
        const double h = args.value(QLatin1String("harmonics")).toDouble(NaN);
        if (!(h >= 2 && h <= 100) || h != std::floor(h)) return errorResult(tr("'harmonics' is the highest harmonic counted, 2 to 100."));
        o.measureOptions.harmonics = int(h);
    }
    if (args.contains(QLatin1String("periods"))) {
        const double n = args.value(QLatin1String("periods")).toDouble(NaN);
        if (!(n >= 1 && n <= 10000) || n != std::floor(n)) return errorResult(tr("'periods' is how many whole periods, 1 to 10000."));
        o.measureOptions.periods = int(n);
    }
    // eye: the bit period, and where the first bit begins.
    if (args.contains(QLatin1String("bit_period"))) {
        const double t = args.value(QLatin1String("bit_period")).toDouble(NaN);
        if (!(t > 0) || !std::isfinite(t)) return errorResult(tr("'bit_period' is a bit's length in the unit of x (seconds), above 0."));
        o.measureOptions.period = t;
    }
    if (args.contains(QLatin1String("offset"))) o.measureOptions.offset = args.value(QLatin1String("offset")).toDouble(0);
    const QString form = args.value(QLatin1String("form")).toString();
    if (form == QLatin1String("db_phase")) o.form = ds::Form::DbPhase;
    else if (form == QLatin1String("real_imaginary")) o.form = ds::Form::RealImaginary;
    o.points = args.contains(QLatin1String("points")) ? std::clamp(args.value(QLatin1String("points")).toInt(), 0, 5000)
                                                      : (o.at.isEmpty() && o.measure.isEmpty() ? 100 : 0);

    // Another run to compare with: a dataset file, or a name simulate's
    // keep_as gave (run1: run1.dat.ngspice beside this one).
    ds::Dataset other;
    QString otherFile;
    if (const QString compare = args.value(QLatin1String("compare")).toString().trimmed(); !compare.isEmpty()) {
        const qsizetype d = info.fileName().indexOf(QLatin1String(".dat"));
        const QString kept = info.absoluteDir().filePath(compare + (d >= 0 ? info.fileName().mid(d) : QStringLiteral(".dat")));
        otherFile = QFileInfo(absolute(compare)).isFile() ? absolute(compare) : QFileInfo(kept).isFile() ? kept : QString();
        if (otherFile.isEmpty()) {
            QStringList there;
            const QString suffix = d >= 0 ? info.fileName().mid(d) : QStringLiteral(".dat");
            for (const QFileInfo& fi : info.absoluteDir().entryInfoList({QStringLiteral("*") + suffix}, QDir::Files, QDir::Time))
                if (fi.absoluteFilePath() != info.absoluteFilePath() && there.size() < 12) there << fi.fileName().left(fi.fileName().size() - suffix.size());
            return errorResult(tr("There is no dataset %1 to compare with: 'compare' is a dataset file, or a name simulate's keep_as "
                                  "gave (%2).")
                                   .arg(compare, there.isEmpty() ? tr("none is kept beside this one") : tr("kept: %1").arg(there.join(QStringLiteral(", ")))));
        }
        if (!other.read(otherFile, &error)) return errorResult(error);
        result.insert(QStringLiteral("compared with"), QDir::toNativeSeparators(otherFile));
    }

    QJsonArray out;
    QStringList missing;
    QStringList done;
    const auto withComparison = [&](QJsonObject json, const ds::Variable& v, const QString& wantedName) {
        if (otherFile.isEmpty()) return json;
        ds::Variable theirs;
        const QStringList names = other.resolve(v.name).isEmpty() ? other.resolve(wantedName) : other.resolve(v.name);
        QString why;
        if (!names.isEmpty()) theirs = *other.find(names.first());
        else if (!ds::isExpression(wantedName) || !ds::evaluate(other, wantedName, &theirs, &why)) {
            json.insert(QStringLiteral("compared"), tr("the other run has no %1").arg(wantedName));
            return json;
        }
        json.insert(QStringLiteral("other run"), comparedJson(data, v, other, theirs, o));
        return json;
    };
    for (const QJsonValue& w : wanted) {
        const QString asked = w.toString();
        const QStringList names = data.resolve(asked);
        if (names.isEmpty()) {
            // An expression of variables: v(out)/v(in), db(ac.v(out)/ac.v(in)).
            ds::Variable made;
            QString why;
            if (ds::isExpression(asked) && done.size() < 20) {
                // (A long one shown by its ends: 20,000 signs came back whole.)
                if (!ds::evaluate(data, asked, &made, &why))
                    return errorResult(tr("%1 does not evaluate: %2.")
                                           .arg(asked.size() > 120 ? asked.left(60) + QStringLiteral(" ... ") + asked.right(40) : asked, why));
                done << asked;
                QJsonObject json = variableJson(data, made, o);
                json.insert(QStringLiteral("expression"), true);
                out.append(withComparison(json, made, asked));
                continue;
            }
            missing << asked;
        }
        for (const QString& name : names) {
            if (done.contains(name) || done.size() >= 20) continue;
            done << name;
            const ds::Variable& v = *data.find(name);
            out.append(withComparison(variableJson(data, v, o), v, asked));
        }
    }
    if (!missing.isEmpty()) {
        QStringList names;
        for (const ds::Variable& v : data.variables())
            if (names.size() < 60) names << v.name;
        if (out.isEmpty())
            return errorResult(tr("%1 has no variable %2. It has: %3.")
                                   .arg(info.fileName(), missing.join(QStringLiteral(", ")), names.join(QStringLiteral(", "))));
        result.insert(QStringLiteral("not found"), QJsonArray::fromStringList(missing));
    }
    result.insert(QStringLiteral("variables"), out);
    return jsonResult(result);
}

QJsonObject QucsControl::reloadData(const QJsonObject& args)
{
    QList<Schematic*> docs;
    if (args.value(QLatin1String("path")).toString().trimmed().isEmpty()) {
        for (QucsDoc* doc : a_app->allDocuments())
            if (auto* sch = dynamic_cast<Schematic*>(doc)) docs << sch;
    } else {
        QString error;
        Schematic* sch = schematic(args, &error, false);
        if (sch == nullptr) return errorResult(error);
        docs << sch;
    }
    QJsonArray list;
    for (Schematic* sch : std::as_const(docs)) {
        if (sch->a_DocDiags.empty()) continue;
        int shown = 0;
        QJsonArray blank;
        int n = 0;
        for (Diagram* d : sch->a_DocDiags) {
            ++n;
            reloadDiagram(sch, d);
            int t = 0;
            for (Graph* g : d->Graphs) {
                ++t;
                if (!g->isEmpty()) ++shown;
                else blank.append(QJsonObject{{QStringLiteral("diagram"), n}, {QStringLiteral("trace"), t},
                                              {QStringLiteral("variable"), g->Var}, {QStringLiteral("why"), whyNoData(sch, g)}});
            }
        }
        sch->viewport()->update();
        QJsonObject o{{QStringLiteral("document"), titleOf(sch)}, {QStringLiteral("traces with data"), shown}};
        if (!blank.isEmpty()) o.insert(QStringLiteral("traces without data"), blank);
        list.append(o);
    }
    if (list.isEmpty()) return textResult(tr("No open document has a diagram."));
    return jsonResult(QJsonObject{{QStringLiteral("reloaded"), list}});
}

QJsonObject QucsControl::importData(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, false);
    if (sch == nullptr) return errorResult(error);
    if (sch->getDocName().isEmpty())
        return errorResult(tr("%1 has no file yet: save it first (save_document with 'as') - the dataset goes beside it. Nothing "
                              "was imported.")
                               .arg(titleOf(sch)));
    const QString folder = QFileInfo(sch->getDocName()).absolutePath();
    const bool reload = args.value(QLatin1String("reload")).toBool();
    const bool remove = args.value(QLatin1String("remove")).toBool();
    if (reload && remove) return errorResult(tr("'reload' or 'remove', not both. Nothing was done."));
    QString name = args.value(QLatin1String("name")).toString().trimmed();
    const QString fileArg = args.value(QLatin1String("file")).toString().trimmed();
    // The file: beside the schematic, else in the project or the workspace.
    QString source;
    if (!fileArg.isEmpty()) {
        const QString beside = QDir::cleanPath(QDir(folder).absoluteFilePath(fileArg));
        source = QFileInfo(beside).isFile() ? beside : absolute(fileArg);
    }
    const QList<di::Imported> there = di::importedIn(folder);
    QStringList thereNames;
    for (const di::Imported& i : there) thereNames << i.name;
    const auto importedNamed = [&there](const QString& n) -> std::optional<di::Imported> {
        for (const di::Imported& i : there)
            if (i.name.compare(n, Qt::CaseInsensitive) == 0) return i;
        return std::nullopt;
    };
    // The diagrams of that folder read again; the traces of the dataset in
    // each, how many show data, and those with a simulator's prefix (added
    // before it was there), which read a file that is not.
    const auto reread = [this, &folder](const QString& dataset) {
        QJsonArray shown;
        for (QucsDoc* doc : a_app->allDocuments()) {
            auto* s = dynamic_cast<Schematic*>(doc);
            if (s == nullptr || s->getDocName().isEmpty() || !sameFile(QFileInfo(s->getDocName()).absolutePath(), folder)) continue;
            int traces = 0, withData = 0, n = 0;
            QJsonArray prefixed;
            for (Diagram* d : s->a_DocDiags) {
                ++n;
                reloadDiagram(s, d);
                int t = 0;
                for (Graph* g : d->Graphs) {
                    ++t;
                    const QString bare = ds::withoutSimulator(g->Var);
                    if (!bare.contains(QLatin1Char(':')) || bare.section(QLatin1Char(':'), 0, 0).compare(dataset, Qt::CaseInsensitive) != 0)
                        continue;
                    ++traces;
                    if (!g->isEmpty()) ++withData;
                    else if (bare != g->Var)
                        prefixed.append(QJsonObject{{QStringLiteral("diagram"), n}, {QStringLiteral("trace"), t},
                                                    {QStringLiteral("variable"), g->Var}, {QStringLiteral("to read it"), bare}});
                }
            }
            s->viewport()->update();
            if (traces == 0) continue;
            QJsonObject o{{QStringLiteral("document"), titleOf(s)}, {QStringLiteral("traces of it"), traces},
                          {QStringLiteral("with data"), withData}};
            if (!prefixed.isEmpty()) {
                o.insert(QStringLiteral("with a simulator's prefix"), prefixed);
                o.insert(QStringLiteral("note"), tr("A trace of it with a prefix reads a simulation's dataset that is not there: "
                                                    "edit_trace with 'variable' as 'to read it' shows it."));
            }
            shown.append(o);
        }
        return shown;
    };

    // Read again or removed: an imported one, by its name or its file.
    std::optional<di::Imported> chosen;
    if (reload || remove) {
        if (!name.isEmpty()) chosen = importedNamed(name);
        else if (!source.isEmpty())
            for (const di::Imported& i : there)
                if (sameFile(i.origin.source, source)) chosen = i;
        if (!chosen) {
            const QString imported = thereNames.isEmpty() ? tr("none") : thereNames.join(QStringLiteral(", "));
            if (name.isEmpty() && fileArg.isEmpty())
                return errorResult(tr("Which dataset? 'name' (or 'file', the one imported from it); imported in %1: %2.")
                                       .arg(QDir::toNativeSeparators(folder), imported));
            return errorResult(tr("%1 is no dataset imported in %2 (imported there: %3). Nothing was done.")
                                   .arg(name.isEmpty() ? fileArg : name, QDir::toNativeSeparators(folder), imported));
        }
    }
    if (remove) {
        aboutToWrite(chosen->path);
        const bool trashed = misc::moveToTrash(chosen->path);
        if (!trashed && !QFile::remove(chosen->path))
            return errorResult(tr("%1 could not be removed.").arg(QDir::toNativeSeparators(chosen->path)));
        QJsonObject result{{QStringLiteral("removed"), chosen->name},
                           {QStringLiteral("file"), QDir::toNativeSeparators(chosen->path)},
                           {QStringLiteral("note"), (trashed ? tr("In the trash, to take back; %1, which it came from, stays.")
                                                             : tr("Deleted; %1, which it came from, stays."))
                                                        .arg(shownFrom(folder, chosen->origin.source))}};
        if (const QJsonArray shown = reread(chosen->name); !shown.isEmpty()) result.insert(QStringLiteral("diagrams"), shown);
        return jsonResult(result);
    }

    di::Options options;
    if (chosen) {
        options = chosen->origin.options;
        source = chosen->origin.source;
        name = chosen->name;
        if (!QFileInfo(source).isFile())
            return errorResult(tr("%1: its file %2 is not there any more. Nothing was done.").arg(name, QDir::toNativeSeparators(source)));
    } else {
        if (fileArg.isEmpty())
            return errorResult(tr("Which file? 'file': a CSV or TSV, an Excel workbook (.xlsx), text of numbers in columns, NumPy "
                                  ".npy or .npz, Touchstone (.s2p ...) or a Qucs-S dataset."));
        if (!QFileInfo(source).isFile())
            return errorResult(tr("There is no file %1 (beside the schematic, in the project or in the workspace). Nothing was "
                                  "imported.")
                                   .arg(QDir::toNativeSeparators(fileArg)));
    }
    const bool xGiven = args.contains(QLatin1String("x")), sheetGiven = args.contains(QLatin1String("sheet"));
    if (xGiven) options.x = args.value(QLatin1String("x")).toString().trimmed();
    if (sheetGiven) options.sheet = args.value(QLatin1String("sheet")).toString().trimmed();

    // Read first: a sheet or an x that is not there is said before
    // anything is written (the Import tab reads on with the first sheet, x
    // chosen by itself, and says so).
    const QString shownSource = shownFrom(folder, source);
    di::Data data;
    if (!di::read(source, options, &data, &error)) return errorResult(tr("%1: %2 Nothing was imported.").arg(shownSource, error));
    bool again = false;
    // (Sheets are a workbook's: a table is read as one of no name.)
    const auto sheetsOf = [](const di::Data& d) { return d.format == di::Format::Workbook ? d.sheets : QStringList(); };
    if (sheetGiven && !options.sheet.isEmpty() && !sheetsOf(data).contains(options.sheet)) {
        QString like;
        for (const QString& s : sheetsOf(data))
            if (s.compare(options.sheet, Qt::CaseInsensitive) == 0) like = s;
        if (sheetsOf(data).isEmpty())
            return errorResult(tr("'sheet': %1 is a %2, which has no sheets. Nothing was imported.").arg(shownSource, di::formatName(data.format, source)));
        if (like.isEmpty())
            return errorResult(tr("'sheet': %1 has no sheet %2 (it has %3). Nothing was imported.")
                                   .arg(shownSource, options.sheet, data.sheets.join(QStringLiteral(", "))));
        options.sheet = like;
        again = true;
    }
    if (xGiven && !options.x.isEmpty() && options.x != di::rowX() && !data.columns.contains(options.x)) {
        // As the columns are named (made safe), or in another case; "row" the row.
        QString like;
        const QString safe = di::safeName(options.x);
        for (const QString& c : std::as_const(data.columns))
            if (c == safe || c.compare(options.x, Qt::CaseInsensitive) == 0 || c.compare(safe, Qt::CaseInsensitive) == 0) like = c;
        if (like.isEmpty() && options.x.compare(QLatin1String("row"), Qt::CaseInsensitive) == 0) like = di::rowX();
        if (like.isEmpty())
            return errorResult(data.columns.isEmpty()
                                   ? tr("'x': %1 has no columns to choose x from (%2). Nothing was imported.").arg(shownSource, di::formatName(data.format, source))
                                   : tr("'x': %1 has no column %2 (it has %3; \"#row\" is the row). Nothing was imported.")
                                         .arg(shownSource, options.x, data.columns.join(QStringLiteral(", "))));
        options.x = like;
        again = true;
    }
    if (again && !di::read(source, options, &data, &error)) return errorResult(tr("%1: %2 Nothing was imported.").arg(shownSource, error));

    // Its name: the file's (a number after it when taken), or the one given
    // - not one a simulation's dataset or a schematic there has (a
    // simulation of x.sch writes x.dat).
    std::optional<di::Imported> replaced;
    if (!chosen && !name.isEmpty()) {
        static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9_]+$"));
        if (!safe.match(name).hasMatch())
            return errorResult(tr("'name': %1 - letters, digits and _ (a trace names it name:variable). Nothing was imported.").arg(name));
        replaced = importedNamed(name);
        if (replaced) name = replaced->name;
        const QString stem = name.toLower();
        for (const QString& entry : QDir(folder).entryList(QDir::Files)) {
            const QString lower = entry.toLower();
            if (lower == stem + QStringLiteral(".sch"))
                return errorResult(tr("'name': %1 is there, and a simulation of it writes %2.dat over the import. Choose another "
                                      "name. Nothing was imported.")
                                       .arg(entry, name));
            if (lower.startsWith(stem + QStringLiteral(".dat.")))
                return errorResult(tr("'name': %1 is a simulation's dataset of that name. Choose another name. Nothing was imported.").arg(entry));
            if (lower == stem + QStringLiteral(".dat") && !replaced)
                return errorResult(tr("'name': %1 is a dataset that was not imported (a simulation's): it would be written over. "
                                      "Choose another name. Nothing was imported.")
                                       .arg(entry));
        }
        // A schematic's Data Set of that name: its runs write there.
        for (const QString& set : di::dataSetsOfSchematics(folder))
            if (set.compare(name, Qt::CaseInsensitive) == 0)
                return errorResult(tr("'name': %1.dat is the Data Set of a schematic here, which its simulations write. Choose "
                                      "another name. Nothing was imported.")
                                       .arg(set));
    }
    QString renamed;   // (why the file's name was not the dataset's)
    if (name.isEmpty()) {
        name = di::datasetNameFor(folder, source);
        replaced = importedNamed(name);   // (this file's, read again)
        if (const QString base = di::datasetBaseFor(source); name != base) {
            QString by;
            if (const auto other = importedNamed(base)) by = tr("the dataset imported from %1").arg(shownFrom(folder, other->origin.source));
            else if (!QDir(folder).entryList({base + QStringLiteral(".dat.*")}, QDir::Files).isEmpty()) by = tr("a simulation's dataset");
            else if (QFileInfo::exists(QDir(folder).filePath(base + QStringLiteral(".sch")))) by = tr("the schematic %1.sch (its runs write %1.dat)").arg(base);
            else if (di::dataSetsOfSchematics(folder).contains(base, Qt::CaseInsensitive)) by = tr("a schematic's Data Set");
            else by = tr("a dataset there");
            renamed = importedNamed(base) ? tr("%1 is %2: this one is %3 ('name': \"%1\" puts it in that one's place).").arg(base, by, name)
                                          : tr("%1 is %2: this one is %3.").arg(base, by, name);
        }
    }
    aboutToWrite(QDir(folder).filePath(name + QStringLiteral(".dat")));
    di::Imported imported;
    if (!di::importRead(folder, source, data, options, &imported, &error, name))
        return errorResult(tr("%1 Nothing was imported.").arg(error));

    // What it holds, as a diagram reads it.
    ds::Dataset written;
    if (!written.read(imported.path, &error)) return errorResult(error);
    QJsonArray variables;
    QStringList traces;
    int listed = 0, left = 0;
    for (const ds::Variable& v : written.variables()) {
        if (!v.independent) traces << name + QLatin1Char(':') + v.name;
        if (++listed > 100) {
            ++left;
            continue;
        }
        QJsonObject e{{QStringLiteral("name"), v.name}, {QStringLiteral("points"), v.size()}};
        if (v.independent) e.insert(QStringLiteral("independent"), true);
        if (v.isComplex()) e.insert(QStringLiteral("complex"), true);
        double lo = std::numeric_limits<double>::infinity(), hi = -lo;
        for (int i = 0; i < v.size(); ++i) {
            const double y = v.isComplex() ? std::hypot(v.re.at(i), v.im.at(i)) : v.re.at(i);
            if (!std::isfinite(y)) continue;
            lo = std::min(lo, y);
            hi = std::max(hi, y);
        }
        if (lo <= hi) {
            const QString what = v.isComplex() ? QStringLiteral("magnitude ") : QString();
            e.insert(what + (v.independent ? QStringLiteral("from") : QStringLiteral("min")), number(v.independent && !v.isComplex() ? v.re.first() : lo));
            e.insert(what + (v.independent ? QStringLiteral("to") : QStringLiteral("max")), number(v.independent && !v.isComplex() ? v.re.last() : hi));
        }
        variables.append(e);
    }
    QJsonObject result{{QStringLiteral("dataset"), name},
                       {QStringLiteral("file"), QDir::toNativeSeparators(imported.path)},
                       {QStringLiteral("from"), QDir::toNativeSeparators(source)},
                       {QStringLiteral("format"), imported.origin.format},
                       {QStringLiteral("x"), data.x == QLatin1String("row") && !data.columns.contains(QStringLiteral("row")) ? tr("the row") : data.x},
                       {QStringLiteral("variables"), variables},
                       {QStringLiteral("traces"), QJsonArray::fromStringList(traces.mid(0, 100))}};
    if (left > 0) result.insert(QStringLiteral("left out"), tr("%1 more variables (get_dataset lists them all)").arg(left));
    // The columns x may be chosen from, cut as the variables are: 5,000 of
    // them made an answer of 112 KB.
    if (!data.columns.isEmpty()) result.insert(QStringLiteral("columns"), QJsonArray::fromStringList(data.columns.mid(0, 100)));
    if (data.columns.size() > 100)
        result.insert(QStringLiteral("columns left out"),
                      tr("%1 more columns (%2 in all): 'x' takes any of them by its name").arg(data.columns.size() - 100).arg(data.columns.size()));
    if (const QStringList sheets = sheetsOf(data); !sheets.isEmpty()) {
        result.insert(QStringLiteral("sheets"), QJsonArray::fromStringList(sheets));
        result.insert(QStringLiteral("sheet"), options.sheet.isEmpty() ? sheets.first() : options.sheet);
    }
    if (!data.notes.isEmpty()) result.insert(QStringLiteral("notes"), QJsonArray::fromStringList(data.notes));
    if (!renamed.isEmpty()) result.insert(QStringLiteral("named"), renamed);
    if (reload) result.insert(QStringLiteral("read again"), true);
    else if (replaced && !sameFile(replaced->origin.source, source))
        result.insert(QStringLiteral("replaced"), tr("%1, imported before from %2").arg(replaced->name, shownFrom(folder, replaced->origin.source)));
    else if (replaced)
        result.insert(QStringLiteral("read again"), true);
    if (const QJsonArray shown = reread(name); !shown.isEmpty()) result.insert(QStringLiteral("diagrams"), shown);
    result.insert(QStringLiteral("note"),
                  tr("Its traces are %1:variable, with no simulator's prefix - add_diagram with traces [\"%2\"] plots it. After %3 "
                     "changes, import_data with 'reload' reads it again.")
                      .arg(name, traces.value(0, name + QStringLiteral(":variable")), shownSource));
    return jsonResult(result);
}

// Curves for another program: a diagram's traces, or a dataset's
// variables, written as the Export tab writes them (dataexport.h).
QJsonObject QucsControl::exportData(const QJsonObject& args)
{
    namespace de = qucs_s::dataexport;
    QString error;
    Schematic* sch = schematic(args, &error, false);
    if (sch == nullptr) return errorResult(error);
    if (sch->getDocName().isEmpty()) return errorResult(tr("%1 has no file yet, so no dataset: save_document with 'as' first.").arg(titleOf(sch)));
    const bool fromDiagram = args.contains(QLatin1String("diagram"));
    if (fromDiagram && (args.contains(QLatin1String("variables")) || args.contains(QLatin1String("dataset"))))
        return errorResult(tr("Either a 'diagram' (its traces) or 'variables' (of 'dataset'): not both."));
    if (!fromDiagram && args.contains(QLatin1String("traces"))) return errorResult(tr("'traces' are a 'diagram''s: give its number too."));
    if (!fromDiagram && args.value(QLatin1String("variables")).toArray().isEmpty())
        return errorResult(tr("What to write: a 'diagram' (its traces, or 'traces' of it), or 'variables' of a dataset."));
    const QString folder = QFileInfo(sch->getDocName()).absolutePath();

    // The dataset and the variables of it asked for.
    QString file;
    QStringList asked;   // as asked: a trace's variable, a name, an expression
    if (fromDiagram) {
        Diagram* d = diagramOf(sch, args.value(QLatin1String("diagram")), &error);
        if (d == nullptr) return errorResult(error);
        if (d->Graphs.isEmpty()) return errorResult(tr("Diagram %1 has no trace.").arg(args.value(QLatin1String("diagram")).toInt()));
        QList<Graph*> graphs;
        if (args.contains(QLatin1String("traces"))) {
            const QJsonArray which = args.value(QLatin1String("traces")).toArray();
            if (which.isEmpty()) return errorResult(tr("'traces': the diagram's traces to write, by their numbers or variables (all by default)."));
            for (const QJsonValue& w : which) {
                Graph* g = traceOf(d, w, &error);
                if (g == nullptr) return errorResult(error);
                if (!graphs.contains(g)) graphs << g;
            }
        } else {
            graphs = QList<Graph*>(d->Graphs.cbegin(), d->Graphs.cend());
        }
        // One dataset: the traces of two (a run and a measurement) are
        // written one dataset at a time.
        QMap<QString, QStringList> byFile;   // dataset file: its traces
        for (Graph* g : std::as_const(graphs)) {
            QString variable;
            const QString f = traceFile(sch, g->Var, &variable);
            const int n = int(d->Graphs.indexOf(g)) + 1;
            if (!QFileInfo(f).isFile())
                return errorResult(tr("Trace %1 (%2) shows no data: %3.").arg(n).arg(g->Var, whyNoData(sch, g)));
            if (byFile.isEmpty() || byFile.contains(f)) file = f;
            byFile[f] << tr("%1 (%2)").arg(n).arg(g->Var);
            if (!asked.contains(variable)) asked << variable;
        }
        if (byFile.size() > 1) {
            QStringList parts;
            for (auto it = byFile.cbegin(); it != byFile.cend(); ++it)
                parts << tr("%1: traces %2").arg(QFileInfo(it.key()).fileName(), it.value().join(QStringLiteral(", ")));
            return errorResult(tr("The traces are of %1 datasets - %2. A file holds one dataset's: give 'traces' of one (and "
                                  "another call, another file, for the others).")
                                   .arg(byFile.size())
                                   .arg(parts.join(QStringLiteral("; "))));
        }
    } else {
        const QJsonArray wanted = args.value(QLatin1String("variables")).toArray();
        for (const QJsonValue& w : wanted) asked << w.toString().trimmed();
        if (const QString named = args.value(QLatin1String("dataset")).toString().trimmed(); !named.isEmpty()) {
            // A file (beside the schematic, or a path), or a name keep_as
            // or import_data gave.
            QStringList tried;
            if (isDatasetFile(named)) {
                const QString beside = QDir(folder).filePath(named);
                file = QFileInfo(beside).isFile() ? beside : QFileInfo(absolute(named)).isFile() ? absolute(named) : QString();
                tried << QDir::toNativeSeparators(named);
            } else {
                file = datasetCalled(folder, named, QString(), &tried);
            }
            if (file.isEmpty()) return errorResult(tr("There is no dataset %1 (%2).").arg(named, tried.join(QStringLiteral(", "))));
        } else {
            file = datasetNamedBy(args, wanted, QStringLiteral("export_data"), &error);
            if (!error.isEmpty()) return errorResult(error);
            if (file.isEmpty()) file = datasetPath(args, &error);
            if (file.isEmpty()) return errorResult(error);
        }
    }
    ds::Dataset data;
    if (!data.read(file, &error)) return errorResult(error);
    // Each as the dataset names it: itself, a trace's name of it (resolve()
    // leaves its simulator and name: out), one it may mean, or an
    // expression, evaluated and written as a variable of its own.
    QStringList chosen, notes;
    for (const QString& w : std::as_const(asked)) {
        const QString bare = ds::withoutSimulator(w);
        if (data.find(bare) != nullptr) {
            if (!chosen.contains(bare)) chosen << bare;
            continue;
        }
        const QStringList names = data.resolve(bare);
        if (names.size() == 1) {
            if (!chosen.contains(names.first())) chosen << names.first();
            continue;
        }
        if (names.size() > 1) return errorResult(tr("%1 may be any of %2: say which.").arg(w, names.join(QStringLiteral(", "))));
        if (ds::isExpression(bare)) {
            ds::Variable made;
            if (!ds::evaluate(data, bare, &made, &error)) return errorResult(tr("%1 does not evaluate: %2.").arg(w, error));
            data.add(made);
            if (!chosen.contains(made.name)) chosen << made.name;
            continue;
        }
        QStringList some;
        for (const ds::Variable& v : data.variables())
            if (!v.independent && some.size() < 12) some << v.name;
        return errorResult(tr("%1 has no variable %2; its variables: %3.").arg(QFileInfo(file).fileName(), w, some.join(QStringLiteral(", "))));
    }

    // The file: its suffix gives the format when it is one's (out.xlsx),
    // else 'format''s is added (csv by default).
    QString target = args.value(QLatin1String("save_as")).toString().trimmed();
    if (target.isEmpty()) return errorResult(tr("'save_as' names the file to write."));
    if (const QString bad = badFileName(QFileInfo(target).fileName()); !bad.isEmpty()) return errorResult(tr("'save_as': %1.").arg(bad));
    target = absolute(target);
    static const QHash<QString, de::Format> formats{{QStringLiteral("csv"), de::Format::Csv},     {QStringLiteral("tsv"), de::Format::Tsv},
                                                    {QStringLiteral("xlsx"), de::Format::Xlsx},   {QStringLiteral("text"), de::Format::Text},
                                                    {QStringLiteral("npz"), de::Format::Npz},     {QStringLiteral("dataset"), de::Format::Dataset}};
    const QString formatName = args.value(QLatin1String("format")).toString().trimmed().toLower();
    if (!formatName.isEmpty() && !formats.contains(formatName)) return errorResult(tr("'format' is csv, tsv, xlsx, text, npz or dataset."));
    bool known = false;
    const de::Format bySuffix = de::formatOfSuffix(QFileInfo(target).suffix(), &known);
    de::Options options;
    options.format = !formatName.isEmpty() ? formats.value(formatName) : known ? bySuffix : de::Format::Csv;
    if (known && !formatName.isEmpty() && bySuffix != options.format)
        return errorResult(tr("'save_as' %1 is a file of %2, and 'format' is %3: the one or the other.")
                               .arg(QFileInfo(target).fileName(), de::formatName(bySuffix), formatName));
    if (!known) target += QLatin1Char('.') + de::suffixOf(options.format);
    if (const QString bad = badFileName(QFileInfo(target).fileName()); !bad.isEmpty()) return errorResult(tr("'save_as': %1.").arg(bad));
    if (!QFileInfo(QFileInfo(target).absolutePath()).isDir())
        return errorResult(tr("There is no folder %1.").arg(QDir::toNativeSeparators(QFileInfo(target).absolutePath())));
    // Never the dataset read, nor a file a dataset beside it was imported
    // from (the measurement itself).
    if (sameFile(target, file)) return errorResult(tr("%1 is the dataset itself: write another file.").arg(QFileInfo(target).fileName()));
    for (const QString& where : {QFileInfo(file).absolutePath(), folder})
        for (const di::Imported& i : di::importedIn(where))
            if (QFileInfo::exists(i.origin.source) && sameFile(target, i.origin.source))
                return errorResult(tr("%1 is the file the dataset %2 was imported from: write another file.").arg(QFileInfo(target).fileName(), i.name));
    const QString complex = args.value(QLatin1String("complex")).toString(QStringLiteral("real_imaginary")).trimmed().toLower();
    if (complex == QLatin1String("real_imaginary")) options.complex = ds::Form::RealImaginary;
    else if (complex == QLatin1String("magnitude_phase")) options.complex = ds::Form::MagnitudePhase;
    else if (complex == QLatin1String("db_phase")) options.complex = ds::Form::DbPhase;
    else return errorResult(tr("'complex' is real_imaginary (the default), magnitude_phase or db_phase."));

    const QList<de::Table> tables = de::tablesOf(data, chosen);
    const bool replacing = QFileInfo::exists(target);
    aboutToWrite(target);
    de::Written w;
    if (!de::write(target, data, chosen, options, &w, &error)) return errorResult(error);

    QJsonObject result{{QStringLiteral("file"), QDir::toNativeSeparators(target)},
                       {QStringLiteral("format"), de::formatName(options.format)},
                       {QStringLiteral("dataset"), QDir::toNativeSeparators(file)}};
    if (replacing) result.insert(QStringLiteral("written over"), tr("the file that was there (undo with 'files' puts it back)"));
    if (de::isTable(options.format)) {
        QJsonArray list;
        for (const de::Table& t : tables) {
            QStringList columns;
            for (const QString& name : t.independents + t.dependents) columns << de::columnsOf(*data.find(name), options.complex);
            QJsonObject o{{QStringLiteral("over"), t.name()}, {QStringLiteral("rows"), t.rows},
                          {QStringLiteral("columns"), QJsonArray::fromStringList(columns.mid(0, 100))}};
            if (columns.size() > 100) o.insert(QStringLiteral("columns left out"), int(columns.size() - 100));
            list.append(o);
        }
        result.insert(options.format == de::Format::Xlsx ? QStringLiteral("sheets") : QStringLiteral("table"),
                      options.format == de::Format::Xlsx ? QJsonValue(list) : list.first());
    } else {
        QStringList written;
        for (const de::Table& t : tables)
            for (const QString& name : t.independents + t.dependents)
                if (!written.contains(name)) written << name;
        result.insert(options.format == de::Format::Npz ? QStringLiteral("arrays") : QStringLiteral("variables"),
                      QJsonArray::fromStringList(written.mid(0, 100)));
    }
    notes << w.notes;
    if (!notes.isEmpty()) result.insert(QStringLiteral("notes"), QJsonArray::fromStringList(notes));
    // A dataset written beside the schematic is one to plot.
    if (options.format == de::Format::Dataset && sameFile(QFileInfo(target).absolutePath(), folder))
        result.insert(QStringLiteral("then"), tr("Its traces are %1:variable (no simulator's prefix): add_diagram plots it.")
                                                  .arg(QFileInfo(target).completeBaseName()));
    return jsonResult(result);
}

QJsonObject QucsControl::getNetlist(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, false);
    if (sch == nullptr) return errorResult(error);
    const QString format = args.value(QLatin1String("format")).toString(QStringLiteral("spice"));
    if (format != QLatin1String("spice") && format != QLatin1String("cdl"))
        return errorResult(tr("'format' is spice or cdl."));
    // Saved to a file, as Simulation > Save netlist and Save CDL netlist
    // do - without their file dialogs.
    const QString saveAs = args.value(QLatin1String("save_as")).toString().trimmed();
    const auto write = [&](const QString& text) -> QJsonObject {
        const QString target = absolute(saveAs);
        if (const QString bad = badFileName(QFileInfo(saveAs).fileName()); !bad.isEmpty()) return errorResult(tr("'save_as': %1.").arg(bad));
        if (const QString bad = badFileName(QFileInfo(target).fileName()); !bad.isEmpty()) return errorResult(tr("'save_as': %1.").arg(bad));
        if (!QFileInfo(target).absoluteDir().exists())
            return errorResult(tr("There is no folder %1.").arg(QDir::toNativeSeparators(QFileInfo(target).absolutePath())));
        // Not over a document: an open one's file would be loaded again as
        // the netlist, and a schematic, symbol or data display lost.
        for (QucsDoc* doc : a_app->allDocuments())
            if (!doc->getDocName().isEmpty() && sameFile(doc->getDocName(), target))
                return errorResult(tr("%1 is open in Qucs-S (%2): the netlist would be written over it. Name another file.")
                                       .arg(QDir::toNativeSeparators(target), titleOf(doc)));
        if (QFileInfo(target).isFile() && !args.value(QLatin1String("replace")).toBool() && isQucsDocument(target))
            return errorResult(tr("%1 is a document of Qucs-S, not a netlist: 'replace': true writes the netlist over it.")
                                   .arg(QDir::toNativeSeparators(target)));
        aboutToWrite(target);
        QFile out(target);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Text))
            return errorResult(tr("%1 cannot be written: %2").arg(QDir::toNativeSeparators(target), out.errorString()));
        out.write(text.toUtf8());
        out.close();
        a_app->projectView()->refresh();
        return jsonResult(QJsonObject{{QStringLiteral("written"), QDir::toNativeSeparators(target)},
                                      {QStringLiteral("format"), format},
                                      {QStringLiteral("lines"), int(text.count(QLatin1Char('\n')))}});
    };
    if (format == QLatin1String("cdl")) {
        QString text;
        {
            QTextStream stream(&text);
            misc::ErrorCapture capture;
            CdlNetlistWriter writer(stream, sch, QucsSettings.ResolveSpicePrefix);
            if (!writer.write())
                return errorResult(tr("The CDL netlist could not be written. %1").arg(capture.errors().join(QLatin1Char('\n'))));
        }
        if (!saveAs.isEmpty()) return write(text);
        return textResult(tr("The CDL netlist of %1:").arg(titleOf(sch)) + QStringLiteral("\n\n") + text);
    }
    const bool last = args.value(QLatin1String("last")).toBool();
    const int simulator = QucsSettings.DefaultSimulator;
    if (simulator == spicecompat::simNotSpecified) return errorResult(noSimulatorText());
    const QString simName = spicecompat::getDefaultSimulatorName(simulator);
    const bool map = args.value(QLatin1String("map")).toBool();
    if (map && (last || simulator == spicecompat::simQucsator || !saveAs.isEmpty()))
        return errorResult(tr("'map' is of the netlist a SPICE simulator would be given now: not with 'last', 'save_as' or Qucsator."));
    QStringList files;
    std::unique_ptr<QTemporaryDir> temporary;
    if (last || simulator == spicecompat::simQucsator) {
        const QDir scratch(misc::scratchDirFor(sch->getDocName()));
        if (simulator == spicecompat::simQucsator) files << QucsSettings.tempFilesDir.filePath(QStringLiteral("netlist.txt"));
        else if (simulator == spicecompat::simXyce)
            for (const QString& f : scratch.entryList({QStringLiteral("spice4qucs.*.cir")}, QDir::Files, QDir::Name)) files << scratch.filePath(f);
        else files << scratch.filePath(QStringLiteral("spice4qucs.cir"));
        files.erase(std::remove_if(files.begin(), files.end(), [](const QString& f) { return !QFileInfo(f).isFile(); }), files.end());
        // Documents outside a project share a Scratch folder: the netlist
        // there may be another's (its first line names the schematic).
        if (!files.isEmpty() && simulator != spicecompat::simQucsator) {
            QFile first(files.first());
            QString head;
            if (first.open(QIODevice::ReadOnly | QIODevice::Text)) head = QString::fromUtf8(first.readLine()).trimmed();
            static const QRegularExpression named(QStringLiteral("^\\*\\s*Qucs\\S*\\s+\\S+\\s+(.+)$"));
            const QRegularExpressionMatch m = named.match(head);
            if (m.hasMatch() && !sameFile(m.captured(1).trimmed(), sch->getDocName()))
                return errorResult(tr("The last simulation here was of %1, not of %2: simulate it first, or leave out 'last' "
                                      "for its netlist as a simulation would write it now.")
                                       .arg(QDir::toNativeSeparators(m.captured(1).trimmed()), titleOf(sch)));
        }
        if (files.isEmpty())
            return errorResult(simulator == spicecompat::simQucsator
                                   ? tr("There is no netlist of a Qucsator simulation yet: simulate first.")
                                   : tr("%1 has not been simulated yet: without 'last', get_netlist writes its netlist now.").arg(titleOf(sch)));
    } else {
        temporary = std::make_unique<QTemporaryDir>();
        const QString file = temporary->filePath(QStringLiteral("netlist.cir"));
        misc::ErrorCapture capture;
        SimulationRun run(sch, false);
        if (!run.writeNetlist(file))
            return errorResult(tr("The netlist could not be written. %1").arg(capture.errors().join(QLatin1Char('\n'))));
        // Written only in part: the netlister gave up (a part with no model,
        // a subcircuit it could not read) - what it said, not a netlist of
        // its title line.
        if (const QString said = run.netlistOutput(); !said.isEmpty() && said.contains(QLatin1String("ERROR")))
            return errorResult(tr("The netlist could not be written: %1").arg(said));
        files << file;
        // Which part of the schematic wrote each line, and which pins each
        // node of the netlist joins: an error or a warning that names a
        // line, a device or a node is found on the schematic by it.
        if (map) {
            QFile f(file);
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return errorResult(tr("The netlist could not be read back."));
            QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
            if (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
            QHash<QString, QString> parts;   // a device's name in the netlist (lower case): its part
            QJsonObject nodes;
            for (Component* c : sch->a_DocComps) {
                // (A ground by its ref, as the other tools take it: GND#1 of
                // several, not GND.)
                const QString ref = refOf(sch, c);
                if (!c->Name.isEmpty()) {
                    parts.insert(c->Name.toLower(), c->Name);
                    if (!c->SpiceModel.isEmpty() && !c->SpiceModel.startsWith(QLatin1Char('.')))
                        parts.insert(spicecompat::check_refdes(c->Name, c->SpiceModel).toLower(), c->Name);
                }
                for (int i = 0; i < c->Ports.size(); ++i) {
                    const Node* n = c->Ports.at(i)->Connection;
                    if (n == nullptr || n->Name.isEmpty()) continue;
                    // (Ground as the netlist writes it: 0, not Qucs-S's gnd.)
                    const QString node = n->Name == QLatin1String("gnd") ? QStringLiteral("0") : n->Name;
                    // (With its name: after its ports were numbered anew,
                    // "SUB1.1" on in looked right while the pin called out
                    // sat there.)
                    QJsonArray on = nodes.value(node).toArray();
                    const QString pinName = c->Ports.at(i)->Name;
                    on.append(pinName.isEmpty() ? QStringLiteral("%1.%2").arg(ref).arg(i + 1)
                                                : QStringLiteral("%1.%2 (%3)").arg(ref).arg(i + 1).arg(pinName));
                    nodes.insert(node, on);
                }
            }
            // Parts of more than two pins none of which has a name: by number
            // only, each with the side it is on (the pin order is the model's).
            QJsonArray unnamed;
            for (Component* c : sch->a_DocComps)
                if (c->isActive == COMP_IS_ACTIVE && c->Ports.size() > 2
                    && std::all_of(c->Ports.cbegin(), c->Ports.cend(), [](const Port* p) { return p->Name.isEmpty(); }))
                    unnamed.append(QStringLiteral("%1: %2").arg(refOf(sch, c), pinSides(c)));
            QJsonArray owned;
            QString owner;
            for (int i = 0; i < lines.size(); ++i) {
                const QString line = lines.at(i).trimmed();
                if (line.startsWith(QLatin1Char('+'))) {   // (the line before, continued)
                    if (!owner.isEmpty()) owned.append(QJsonObject{{QStringLiteral("line"), i + 1}, {QStringLiteral("part"), owner}});
                    continue;
                }
                owner.clear();
                if (line.isEmpty() || line.startsWith(QLatin1Char('*')) || line.startsWith(QLatin1Char('.'))) continue;
                owner = parts.value(line.section(QLatin1Char(' '), 0, 0).toLower());
                if (!owner.isEmpty())
                    owned.append(QJsonObject{{QStringLiteral("line"), i + 1}, {QStringLiteral("part"), owner}, {QStringLiteral("text"), line}});
            }
            QJsonObject result{{QStringLiteral("document"), titleOf(sch)},
                               {QStringLiteral("simulator"), simName},
                               {QStringLiteral("netlist"), QJsonArray::fromStringList(lines)},
                               {QStringLiteral("lines"), owned},
                               {QStringLiteral("nodes"), nodes},
                               {QStringLiteral("how"), tr("'lines': each line a part wrote (its number in 'netlist', from 1); "
                                                          "'nodes': each node of the netlist with the pins it joins, "
                                                          "by number and, when the pin has one, name (0 is ground)")}};
            if (!unnamed.isEmpty()) result.insert(QStringLiteral("pins without names"), unnamed);
            return jsonResult(result);
        }
    }
    if (!saveAs.isEmpty()) {
        QString whole;
        for (const QString& f : std::as_const(files)) {
            QFile file(f);
            if (file.open(QIODevice::ReadOnly | QIODevice::Text)) whole += QString::fromUtf8(file.readAll());
        }
        return write(whole);
    }
    QString text;
    const bool numbered = args.value(QLatin1String("numbered")).toBool();
    for (const QString& f : std::as_const(files)) {
        QFile file(f);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        QString body = QString::fromUtf8(file.readAll());
        if (numbered) {
            QStringList lines = body.split(QLatin1Char('\n'));
            if (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
            for (int i = 0; i < lines.size(); ++i) lines[i] = QStringLiteral("%1  %2").arg(i + 1, 4).arg(lines.at(i));
            body = lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
        }
        if (files.size() > 1 || last) text += tr("--- %1 ---\n").arg(QDir::toNativeSeparators(f));
        text += body;
    }
    const QString head = last ? tr("The netlist of the last %1 simulation of %2:").arg(simName, titleOf(sch))
                              : tr("The netlist of %1 for %2, as a simulation would write it now:").arg(titleOf(sch), simName);
    return textResult(head + QStringLiteral("\n\n") + text);
}

namespace {

// What diagram \a d lies over: other diagrams (by their numbers) and parts
// of the circuit - said, as placing one by its lower left corner makes
// it easy to put it on something.
QString overlapOf(Schematic* sch, const Diagram* d)
{
    const QRect mine(d->cx, d->cy - d->y2, d->x2, d->y2);
    QStringList diagrams, parts;
    int n = 0;
    for (const Diagram* other : sch->a_DocDiags) {
        ++n;
        if (other != d && mine.intersects(QRect(other->cx, other->cy - other->y2, other->x2, other->y2)))
            diagrams << QString::number(n);
    }
    for (Component* c : sch->a_DocComps)
        if (mine.intersects(c->boundingRect()) && parts.size() < 8) parts << (c->Name.isEmpty() ? c->Model : c->Name);
    QStringList what;
    if (!diagrams.isEmpty()) what << tr("diagram %1").arg(diagrams.join(QStringLiteral(", ")));
    if (!parts.isEmpty()) what << parts.join(QStringLiteral(", "));
    if (what.isEmpty()) return {};
    const QRect used = sch->allBoundingRect();
    return tr("It lies over %1: x, y is its lower left corner - below everything is y %2 and more (add_diagram without x, y "
              "puts it there).")
        .arg(what.join(QStringLiteral(" and ")))
        .arg(used.bottom() + 80 + d->y2);
}

} // namespace

QJsonObject QucsControl::addDiagram(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) return errorResult(error);
    const QString type = args.value(QLatin1String("type")).toString(QStringLiteral("rect")).trimmed();
    std::unique_ptr<Diagram> d(newDiagram(type.isEmpty() ? QStringLiteral("rect") : type));
    if (!d) return errorResult(tr("There is no diagram type %1: %2.").arg(type, kindNames()));
    // In the colours new diagrams start with (the user's default).
    d->setTheme(qucs_s::diagramtheme::defaultForNewDiagrams());
    if (!applyDiagram(d.get(), args, &error)) return errorResult(error);
    QStringList notes;
    // Not told where: below everything there is, room left for its axes'
    // numbers and labels.
    if (!args.contains(QLatin1String("x")) || !args.contains(QLatin1String("y"))) {
        const QRect used = sch->allBoundingRect();
        const bool empty = sch->a_DocComps.empty() && sch->a_DocWires.empty() && sch->a_DocDiags.empty() && sch->a_DocPaints.empty();
        if (!args.contains(QLatin1String("x"))) d->cx = empty ? 60 : used.left() + 60;
        if (!args.contains(QLatin1String("y"))) d->cy = (empty ? 0 : used.bottom()) + 80 + d->y2;
        notes << tr("Placed below the circuit, its lower left corner at %1, %2.").arg(d->cx).arg(d->cy);
    }
    int x = d->cx, y = d->cy;
    sch->setOnGrid(x, y);
    d->cx = x;
    d->cy = y;
    // Expressions (ac.db(v(out))): a NutmegEq's variables, made once every
    // trace is known to do - each trace's graph named after it then.
    QList<QPair<Graph*, QString>> computed;
    for (const QJsonValue& v : args.value(QLatin1String("traces")).toArray()) {
        const QJsonObject t = v.isString() ? QJsonObject{{QStringLiteral("variable"), v.toString()}} : v.toObject();
        QString note;
        const QString wanted = t.value(QLatin1String("variable")).toString();
        QString var = expressionTrace(sch, wanted, args.value(QLatin1String("path")), true, &note, &error);
        const bool expression = !var.isEmpty();
        if (!expression && !error.isEmpty()) return errorResult(error);
        if (!expression) var = traceVariable(sch, d.get(), wanted, &note, &error);
        if (var.isEmpty()) return errorResult(error);
        auto* g = new Graph(d.get(), var);
        if (expression) computed.append({g, wanted});
        g->Color = QColor(QRgb(0x0000ff));
        static const QRgb palette[] = {0x0000ff, 0xff0000, 0xff00ff, 0x00ff00, 0x00ffff, 0xffff00, 0x777777, 0x000000};
        g->Color = QColor(palette[d->Graphs.size() % 8]);
        g->Thick = _settings::Get().item<QString>("DefaultGraphLineWidth").toInt();
        if (d->Name == QLatin1String("Rect3D")) g->yAxisNo = 1;
        if (!applyTrace(g, d.get(), t, &error)) {
            delete g;
            return errorResult(error);
        }
        d->Graphs.append(g);
        if (!note.isEmpty() && !notes.contains(note)) notes << note;   // (no dataset: said once)
    }
    for (const auto& [g, wanted] : std::as_const(computed)) {
        QString note;
        const QString var = expressionTrace(sch, wanted, args.value(QLatin1String("path")), false, &note, &error);
        if (var.isEmpty()) {
            notes << tr("%1: not computed - %2").arg(wanted, error);
            continue;
        }
        g->Var = var;
        if (!note.isEmpty()) notes << note;
    }
    prepare(sch);
    Diagram* placed = d.release();
    sch->a_DocDiags.push_back(placed);
    reloadDiagram(sch, placed);
    sch->enlargeView(placed);
    finish(sch, {QPoint(placed->cx, placed->cy)});
    QJsonObject result = diagramsJson(sch).last().toObject();
    if (const QString over = overlapOf(sch, placed); !over.isEmpty()) notes << over;
    notes << diagramNotes(placed, !args.contains(QLatin1String("legend")));
    if (!notes.isEmpty()) result.insert(QStringLiteral("note"), notes.join(QLatin1Char(' ')));
    return jsonResult(result);
}

QJsonObject QucsControl::editDiagram(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) return errorResult(error);
    Diagram* d = diagramOf(sch, args.value(QLatin1String("diagram")), &error);
    if (d == nullptr) return errorResult(error);
    // Tried on a copy: a change that does not read leaves it as it was.
    std::unique_ptr<Diagram> trial(newDiagram(d->Name));
    QString text = d->save();
    QTextStream stream(&text);
    const QString head = stream.readLine().trimmed();
    if (!trial || !trial->load(head, &stream) || !applyDiagram(trial.get(), args, &error))
        return errorResult(error.isEmpty() ? tr("The diagram could not be changed.") : error);
    prepare(sch);
    applyDiagram(d, args, &error);
    if (args.contains(QLatin1String("x")) || args.contains(QLatin1String("y"))) {
        int x = d->cx, y = d->cy;
        sch->setOnGrid(x, y);
        d->cx = x;
        d->cy = y;
    }
    reloadDiagram(sch, d);
    sch->enlargeView(d);
    finish(sch, {QPoint(d->cx, d->cy)});
    int n = 0;
    for (Diagram* each : sch->a_DocDiags) {
        if (each == d) break;
        ++n;
    }
    QJsonObject result = diagramsJson(sch).at(n).toObject();
    QStringList notes;
    if (const QString over = overlapOf(sch, d); !over.isEmpty()) notes << over;
    if (args.contains(QLatin1String("y_axis")) || args.contains(QLatin1String("y2_axis"))) notes << diagramNotes(d, false);
    if (!notes.isEmpty()) result.insert(QStringLiteral("note"), notes.join(QLatin1Char(' ')));
    return jsonResult(result);
}

QJsonObject QucsControl::addTrace(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) return errorResult(error);
    Diagram* d = diagramOf(sch, args.value(QLatin1String("diagram")), &error);
    if (d == nullptr) return errorResult(error);
    QString note;
    const QString wanted = args.value(QLatin1String("variable")).toString();
    // An expression: a NutmegEq's variable (checked first, made last).
    const bool expression = !expressionTrace(sch, wanted, args.value(QLatin1String("path")), true, &note, &error).isEmpty();
    if (!expression && !error.isEmpty()) return errorResult(error);
    QString var = expression ? QString() : traceVariable(sch, d, wanted, &note, &error);
    if (!expression && var.isEmpty()) return errorResult(error);
    if (expression) {
        var = expressionTrace(sch, wanted, args.value(QLatin1String("path")), false, &note, &error);
        if (var.isEmpty()) return errorResult(error);
    }
    for (Graph* g : d->Graphs)
        if (g->Var == var) return errorResult(tr("The diagram shows %1 already.").arg(var));
    auto g = std::make_unique<Graph>(d, var);
    static const QRgb palette[] = {0x0000ff, 0xff0000, 0xff00ff, 0x00ff00, 0x00ffff, 0xffff00, 0x777777, 0x000000};
    g->Color = QColor(palette[d->Graphs.size() % 8]);
    g->Thick = _settings::Get().item<QString>("DefaultGraphLineWidth").toInt();
    if (d->Name == QLatin1String("Rect3D")) g->yAxisNo = 1;
    if (!applyTrace(g.get(), d, args, &error)) return errorResult(error);
    prepare(sch);
    d->Graphs.append(g.release());
    reloadDiagram(sch, d);
    finish(sch, {QPoint(d->cx, d->cy)});
    QJsonObject result = traceJson(sch, d, d->Graphs.last(), int(d->Graphs.size()));
    if (!note.isEmpty()) result.insert(QStringLiteral("note"), note);
    return jsonResult(result);
}

QJsonObject QucsControl::editTrace(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) return errorResult(error);
    Diagram* d = diagramOf(sch, args.value(QLatin1String("diagram")), &error);
    if (d == nullptr) return errorResult(error);
    Graph* g = traceOf(d, args.value(QLatin1String("trace")), &error);
    if (g == nullptr) return errorResult(error);
    QString note;
    QString var = g->Var;
    bool expression = false;
    const QString wanted = args.value(QLatin1String("variable")).toString();
    if (args.contains(QLatin1String("variable"))) {
        // (An expression's NutmegEq made once the rest is known to do.)
        var = expressionTrace(sch, wanted, args.value(QLatin1String("path")), true, &note, &error);
        expression = !var.isEmpty();
        if (!expression && !error.isEmpty()) return errorResult(error);
        if (!expression) var = traceVariable(sch, d, wanted, &note, &error);
        if (var.isEmpty()) return errorResult(error);
    }
    // Tried on a copy first.
    Graph trial(d, var);
    trial.Color = g->Color;
    if (!applyTrace(&trial, d, args, &error)) return errorResult(error);
    if (expression) {
        var = expressionTrace(sch, wanted, args.value(QLatin1String("path")), false, &note, &error);
        if (var.isEmpty()) return errorResult(error);
    }
    // Nothing to change: said, and why when the variable asked for is
    // written as the trace has it already (the simulator's prefix added).
    const std::unique_ptr<Graph> after(g->sameNewOne());
    after->Var = var;
    applyTrace(after.get(), d, args, &error);
    const int number = int(d->Graphs.indexOf(g)) + 1;
    if (after->save() == g->save()) {
        QJsonObject result = traceJson(sch, d, g, number);
        result.insert(QStringLiteral("changed"), false);
        QString why = tr("Nothing changed: trace %1 is so already.").arg(number);
        if (args.contains(QLatin1String("variable")) && wanted.trimmed() != var)
            why += QLatin1Char(' ') + tr("'variable' %1 is written %2 here, which it is.").arg(wanted.trimmed(), var);
        result.insert(QStringLiteral("note"), note.isEmpty() ? why : why + QLatin1Char(' ') + note);
        return jsonResult(result);
    }
    prepare(sch);
    applyTrace(g, d, args, &error);
    if (var != g->Var) {
        g->Var = var;
        // The markers were of the curve before.
        qDeleteAll(g->Markers);
        g->Markers.clear();
    }
    // (Another part of the value: read again for it.)
    g->lastLoaded = QDateTime();
    reloadDiagram(sch, d);
    finish(sch, {QPoint(d->cx, d->cy)});
    QJsonObject result = traceJson(sch, d, g, number);
    if (!note.isEmpty()) result.insert(QStringLiteral("note"), note);
    return jsonResult(result);
}

namespace {

// The first curve of a trace as it shows it: x, and y real (a complex value
// with no imaginary part keeps its sign) or the magnitude.
ds::Curve shownCurve(const Graph* g)
{
    ds::Curve c;
    const DataX* x = g->axis(0);
    if (x == nullptr || g->cPointsY == nullptr) return c;
    const int n = int(x->count);
    bool real = true;
    for (int i = 0; i < n; ++i)
        if (g->cPointsY[2 * i + 1] != 0) real = false;
    for (int i = 0; i < n; ++i) {
        c.x << x->Points[i];
        const double re = g->cPointsY[2 * i], im = g->cPointsY[2 * i + 1];
        c.y << (real ? re : std::hypot(re, im));
    }
    return c;
}

// Where a marker goes on \a g: at x, or where 'at' says - "peak" (or
// "max"), "min", "-3dB" (3 dB below the peak on a curve in dB, 1/sqrt(2)
// of it on a magnitude; on the far side of the peak first), "crossing:<y>".
// -3dB is below \a reference: "peak" (the default), "dc" (the value at the
// curve's first point, the lowest frequency) or a level (0: a filter's
// spec in dB). What it found, in \a found.
bool markerPlace(Schematic* sch, const Graph* g, const QJsonValue& at, double* x, QJsonObject* found, QString* error,
                 const QJsonValue& reference = QJsonValue())
{
    if (at.isDouble()) {
        // On the curve: past its ends, the marker sat on its last sample and
        // nothing was said.
        const ds::Curve c = shownCurve(g);
        if (!c.x.isEmpty()) {
            const auto [lo, hi] = std::minmax_element(c.x.cbegin(), c.x.cend());
            const double span = *hi - *lo, tolerance = span > 0 ? span * 1e-9 : 0;
            if (!std::isfinite(at.toDouble()) || at.toDouble() < *lo - tolerance || at.toDouble() > *hi + tolerance) {
                *error = tr("'at' %1 is off the trace: its x goes from %2 to %3.").arg(at.toDouble()).arg(*lo).arg(*hi);
                return false;
            }
        }
        *x = at.toDouble();
        return true;
    }
    const QString w = at.toString().trimmed().toLower().remove(QLatin1Char(' '));
    if (w.isEmpty()) {
        *error = tr("Say where: 'at', an x value or peak, min, -3dB, crossing:<y>.");
        return false;
    }
    const ds::Curve c = shownCurve(g);
    if (c.x.size() < 2) {
        *error = tr("The trace has too few points to find %1 on.").arg(at.toString());
        return false;
    }
    const ds::Stats s = ds::statsOf(c);
    if (w == QLatin1String("peak") || w == QLatin1String("max")) {
        *x = s.xMax;
        found->insert(QStringLiteral("peak"), number(s.max));
        return true;
    }
    if (w == QLatin1String("min")) {
        *x = s.xMin;
        found->insert(QStringLiteral("min"), number(s.min));
        return true;
    }
    double level = NaN;
    if (w == QLatin1String("-3db") || w == QLatin1String("3db")) {
        const QString unit = ds::unitOf(g->Var, definitionsIn(sch).value(ds::bareName(ds::withoutSimulator(g->Var)).toLower()));
        const bool decibels = ds::isDecibels(unit);
        if (!decibels && s.min < 0) {
            *error = tr("%1 goes below 0 and is not known to be in dB: where 3 dB below its peak is cannot be told.").arg(g->Var);
            return false;
        }
        // Below what: the peak, the value at the start, or a level given.
        double ref = s.max;
        QString refName = QStringLiteral("the peak");
        const QString r = reference.toString().trimmed().toLower();
        if (reference.isDouble()) {
            ref = reference.toDouble();
            if (!decibels && ref <= 0) {
                *error = tr("%1 is a magnitude: its reference is above 0 (1 for a gain of one), or peak or dc.").arg(g->Var);
                return false;
            }
            refName = decibels ? tr("%1 dB").arg(ref) : QString::number(ref);
        } else if (r == QLatin1String("dc") || r == QLatin1String("start") || r == QLatin1String("first")) {
            ref = c.y.first();
            refName = tr("the value at the start (%1 at %2)").arg(ref).arg(c.x.first());
        } else if (!r.isEmpty() && r != QLatin1String("peak") && r != QLatin1String("max")) {
            *error = tr("'reference' is peak (the default), dc (the value at the start) or a level (0 for 0 dB).");
            return false;
        }
        level = decibels ? ref - 3 : ref / std::sqrt(2.0);
        found->insert(QStringLiteral("peak"), number(s.max));
        found->insert(QStringLiteral("at peak"), number(s.xMax));
        found->insert(QStringLiteral("reference"), refName);
        found->insert(QStringLiteral("level"), number(level));
        found->insert(QStringLiteral("measured on"), decibels ? tr("dB: 3 below %1").arg(refName)
                                                              : tr("a magnitude: %1 over sqrt(2)").arg(refName));
    } else if (w.startsWith(QLatin1String("crossing:"))) {
        bool ok = false;
        level = w.mid(9).toDouble(&ok);
        if (!ok) {
            *error = tr("crossing:<y> takes a number: crossing:-3.");
            return false;
        }
        found->insert(QStringLiteral("level"), number(level));
    } else {
        *error = tr("'at' is an x value or peak, min, -3dB, crossing:<y> - not %1.").arg(at.toString());
        return false;
    }
    const QList<ds::Crossing> all = ds::crossings(c, level);
    if (all.isEmpty()) {
        *error = tr("%1 does not reach %2 (it runs from %3 to %4).").arg(g->Var).arg(level).arg(s.min).arg(s.max);
        return false;
    }
    // For -3 dB: past the peak first (a low pass), else before it.
    const ds::Crossing* chosen = &all.first();
    if (w != QLatin1String("-3db") && w != QLatin1String("3db")) {
    } else if (auto after = std::find_if(all.cbegin(), all.cend(), [&s](const ds::Crossing& k) { return k.x > s.xMax; }); after != all.cend()) {
        chosen = &*after;
    } else {
        chosen = &all.last();
    }
    *x = chosen->x;
    found->insert(QStringLiteral("crossing"), number(chosen->x));
    return true;
}

bool colourOf(const QJsonValue& v, QColor* colour, QString* error)
{
    const QString name = v.toString().trimmed();
    if (name.isEmpty() || name == QLatin1String("auto")) {
        *colour = QColor();
        return true;
    }
    const QColor c(name);
    if (!c.isValid()) {
        *error = tr("%1 is not a colour (#rrggbb, #aarrggbb, a name, or auto).").arg(name);
        return false;
    }
    *colour = c;
    return true;
}

// The settings of a marker 'args' give: its label (where its box's top
// left corner is, or how far from the point it marks), precision, format,
// transparency, indicator and colours.
bool applyMarker(Marker* m, const Diagram* d, const QJsonObject& args, QString* error)
{
    if (args.contains(QLatin1String("label"))) {
        const QJsonArray p = args.value(QLatin1String("label")).toArray();
        if (p.size() != 2) {
            *error = tr("'label' is [x, y]: where its box's top left corner goes.");
            return false;
        }
        // (In 64 bits, then kept on the canvas: 2^31 - 1 overflowed.)
        const auto within = [](double v) { return qint64(std::clamp(std::isfinite(v) ? v : 0.0, -1e9, 1e9)); };
        m->x1 = misc::clampCoordinate(int(std::clamp<qint64>(within(p.at(0).toDouble()) - d->cx, -misc::MaxCoordinate, misc::MaxCoordinate)));
        m->y1 = misc::clampCoordinate(int(std::clamp<qint64>(within(p.at(1).toDouble()) - d->cy, -misc::MaxCoordinate, misc::MaxCoordinate)));
    } else if (args.contains(QLatin1String("label_offset"))) {
        const QJsonArray p = args.value(QLatin1String("label_offset")).toArray();
        if (p.size() != 2 || !p.at(0).isDouble() || !p.at(1).isDouble() || std::abs(p.at(0).toDouble()) > 10000
            || std::abs(p.at(1).toDouble()) > 10000) {
            *error = tr("'label_offset' is [dx, dy]: from the point it marks to its box's top left corner (y down), each within "
                        "10000.");
            return false;
        }
        m->x1 = misc::clampCoordinate(m->cx + p.at(0).toInt());
        m->y1 = misc::clampCoordinate(-m->cy + p.at(1).toInt());
    }
    // (As the marker dialog takes it: 0 to 12. Clamped, 99 was 12 and 2.7
    // was 1, without a word.)
    if (args.contains(QLatin1String("precision"))) {
        const QJsonValue p = args.value(QLatin1String("precision"));
        if (!p.isDouble() || p.toDouble() != std::floor(p.toDouble()) || p.toDouble() < 0 || p.toDouble() > 12) {
            *error = tr("'precision' is a whole number from 0 to 12: significant digits (automatic notation) or places after the "
                        "point (the others).");
            return false;
        }
        m->Precision = p.toInt();
    }
    if (args.contains(QLatin1String("format"))) {
        const int n = indexIn(kNumbers, args.value(QLatin1String("format")).toString());
        if (n < 0) {
            *error = tr("'format' is %1.").arg(namesOf(kNumbers));
            return false;
        }
        m->numMode = n;
    }
    if (args.contains(QLatin1String("notation"))) {
        const QString wanted = args.value(QLatin1String("notation")).toString();
        const int n = wanted == QLatin1String("diagram") ? -1 : indexIn(kNotations, wanted);
        if (n < 0 && wanted != QLatin1String("diagram")) {
            *error = tr("'notation' is diagram (as the diagram's axes), %1.").arg(namesOf(kNotations));
            return false;
        }
        m->notation = n;
    }
    if (args.contains(QLatin1String("transparent"))) m->transparent = args.value(QLatin1String("transparent")).toBool();
    if (args.contains(QLatin1String("indicator"))) {
        const int n = indexIn(kIndicators, args.value(QLatin1String("indicator")).toString());
        if (n < 0) {
            *error = tr("'indicator' is %1.").arg(namesOf(kIndicators));
            return false;
        }
        m->indicatorMode = indicatorMode_t(n);
    }
    if (args.contains(QLatin1String("text_color")) && !colourOf(args.value(QLatin1String("text_color")), &m->textColor, error)) return false;
    if (args.contains(QLatin1String("fill_color")) && !colourOf(args.value(QLatin1String("fill_color")), &m->fillColor, error)) return false;
    return true;
}

bool drawsMarkers(const Diagram* d)
{
    return d->Name != QLatin1String("Tab") && d->Name != QLatin1String("Truth");
}

} // namespace

QJsonObject QucsControl::addMarker(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) return errorResult(error);
    Diagram* d = diagramOf(sch, args.value(QLatin1String("diagram")), &error);
    if (d == nullptr) return errorResult(error);
    if (!drawsMarkers(d)) return errorResult(tr("A %1 diagram has no markers.").arg(kindName(d)));
    Graph* g = traceOf(d, args.value(QLatin1String("trace")), &error);
    if (g == nullptr) return errorResult(error);
    if (g->isEmpty()) return errorResult(tr("%1 shows no data (%2): a marker needs it.").arg(g->Var, whyNoData(sch, g)));
    double x = NaN;
    QJsonObject found;
    if (!markerPlace(sch, g, args.value(QLatin1String("at")), &x, &found, &error, args.value(QLatin1String("reference"))))
        return errorResult(error);

    auto m = std::make_unique<Marker>(g);
    m->setPos(x);
    m->createText();   // (on the sample nearest x)
    m->x1 = m->cx + 20;
    m->y1 = -m->cy - 40;
    if (!applyMarker(m.get(), d, args, &error)) return errorResult(error);
    prepare(sch);
    Marker* placed = m.release();
    g->Markers.append(placed);
    placed->createText();
    finish(sch, {QPoint(d->cx + placed->x1, d->cy + placed->y1)});
    QJsonObject result = markerJson(d, placed, int(markersOf(d).indexOf(placed)) + 1);
    if (!found.isEmpty()) {
        found.insert(QStringLiteral("marker on the nearest sample"), number(placed->varPos().at(0)));
        result.insert(QStringLiteral("found"), found);
    }
    return jsonResult(result);
}

QJsonObject QucsControl::editMarker(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) return errorResult(error);
    Diagram* d = diagramOf(sch, args.value(QLatin1String("diagram")), &error);
    if (d == nullptr) return errorResult(error);
    Marker* m = markerOf(d, args.value(QLatin1String("marker")), &error);
    if (m == nullptr) return errorResult(error);
    const Graph* g = m->graph();
    double x = NaN;
    QJsonObject found;
    if (args.contains(QLatin1String("at"))
        && !markerPlace(sch, g, args.value(QLatin1String("at")), &x, &found, &error, args.value(QLatin1String("reference"))))
        return errorResult(error);
    // Tried on a copy first.
    Marker trial(const_cast<Graph*>(g));
    trial.x1 = m->x1;
    trial.y1 = m->y1;
    if (!applyMarker(&trial, d, args, &error)) return errorResult(error);
    prepare(sch);
    if (!std::isnan(x)) {
        // The label keeps its place beside the point, moved with it.
        const int dx = m->x1 - m->cx, dy = m->y1 + m->cy;
        m->setPos(x);
        m->createText();
        m->x1 = m->cx + dx;
        m->y1 = -m->cy + dy;
    }
    applyMarker(m, d, args, &error);
    m->createText();
    finish(sch, {QPoint(d->cx + m->x1, d->cy + m->y1)});
    QJsonObject result = markerJson(d, m, int(markersOf(d).indexOf(m)) + 1);
    if (!found.isEmpty()) {
        found.insert(QStringLiteral("marker on the nearest sample"), number(m->varPos().at(0)));
        result.insert(QStringLiteral("found"), found);
    }
    return jsonResult(result);
}

QJsonObject QucsControl::deleteMarker(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) return errorResult(error);
    Diagram* d = diagramOf(sch, args.value(QLatin1String("diagram")), &error);
    if (d == nullptr) return errorResult(error);
    Marker* m = markerOf(d, args.value(QLatin1String("marker")), &error);
    if (m == nullptr) return errorResult(error);
    prepare(sch);
    const QString text = m->Text.trimmed().replace(QLatin1Char('\n'), QStringLiteral("; "));
    for (Graph* g : d->Graphs) g->Markers.removeOne(m);
    delete m;
    finish(sch, {QPoint(d->cx, d->cy)});
    return textResult(tr("The marker (%1) is deleted (one step to undo).").arg(text));
}

// A Verilog-A module described - by a .va or .osdi file, or by its name,
// looked for in the open documents, the project and the workspace: its
// parameters (default, units, description, instance or model), the
// files, and a .model card with the defaults. Empty when there is none.
static QJsonObject describeVerilogAModule(const QString& type, const QList<QucsDoc*>& open, QString* error)
{
    namespace va = qucs_s::vamodule;
    QString source, osdi;   // the files found
    QString wanted = type;
    const auto moduleIn = [](const QStringList& names, const QString& name) {
        for (const QString& n : names)
            if (n.compare(name, Qt::CaseInsensitive) == 0) return n;
        return QString();
    };
    const QString lower = type.toLower();
    if (lower.endsWith(QLatin1String(".va")) || lower.endsWith(QLatin1String(".osdi"))) {
        QString file = type;
        if (QDir::isRelativePath(file)) {
            for (const QString& root : {QucsSettings.QucsWorkDir.absolutePath(), QucsSettings.qucsWorkspaceDir.absolutePath()})
                if (QFileInfo::exists(QDir(root).filePath(type))) {
                    file = QDir(root).filePath(type);
                    break;
                }
        }
        if (!QFileInfo::exists(file)) {
            *error = tr("There is no file %1.").arg(type);
            return {};
        }
        (lower.endsWith(QLatin1String(".va")) ? source : osdi) = QFileInfo(file).absoluteFilePath();
        wanted.clear();   // (its first module)
    } else {
        // By its name: an open .va document, then the project's and the
        // workspace's .va and .osdi files.
        QStringList sources, libraries;
        for (QucsDoc* doc : open)
            if (doc->getDocName().endsWith(QLatin1String(".va"), Qt::CaseInsensitive)) sources << doc->getDocName();
        QStringList roots{QucsSettings.QucsWorkDir.absolutePath()};
        if (!sameFile(roots.first(), QucsSettings.qucsWorkspaceDir.absolutePath())) roots << QucsSettings.qucsWorkspaceDir.absolutePath();
        for (const QString& root : std::as_const(roots))
            for (const QString& f : misc::projectFiles(QDir(root), {QStringLiteral("*.va"), QStringLiteral("*.osdi")})) {
                const QString path = QDir(root).filePath(f);
                (f.endsWith(QLatin1String(".osdi"), Qt::CaseInsensitive) ? libraries : sources) << path;
                if (sources.size() + libraries.size() > 400) break;
            }
        for (const QString& f : std::as_const(libraries)) {
            QStringList names;
            if (osdi.isEmpty() && va::osdiModules(f, &names) && !moduleIn(names, type).isEmpty()) osdi = f;
        }
        for (const QString& f : std::as_const(sources)) {
            QFile file(f);
            if (source.isEmpty() && file.open(QIODevice::ReadOnly)
                && !moduleIn(va::sourceModules(QString::fromUtf8(file.readAll())), type).isEmpty())
                source = f;
        }
        if (source.isEmpty() && osdi.isEmpty()) return {};
    }
    // The source beside a library, and the library beside a source.
    if (source.isEmpty() && !osdi.isEmpty()) {
        const QString beside = osdi.left(osdi.size() - 5) + QStringLiteral(".va");
        if (QFileInfo::exists(beside)) source = beside;
    }
    if (osdi.isEmpty() && !source.isEmpty()) {
        const QString beside = source.left(source.size() - 3) + QStringLiteral(".osdi");
        if (QFileInfo::exists(beside)) osdi = beside;
    }
    va::VerilogModule fromSource, fromLibrary;
    bool haveSource = false, haveLibrary = false;
    if (!source.isEmpty()) {
        QString text;
        // An open document's text, unsaved changes too.
        for (QucsDoc* doc : open)
            if (sameFile(doc->getDocName(), source))
                if (auto* t = dynamic_cast<TextDoc*>(doc)) text = t->toPlainText();
        if (text.isEmpty()) {
            QFile file(source);
            if (file.open(QIODevice::ReadOnly)) text = QString::fromUtf8(file.readAll());
        }
        fromSource = va::readSource(text, wanted);
        haveSource = !fromSource.name.isEmpty();
    }
    QString libraryError;
    if (!osdi.isEmpty()) haveLibrary = va::readOsdi(osdi, wanted.isEmpty() ? fromSource.name : wanted, &fromLibrary, &libraryError);
    if (!haveSource && !haveLibrary) {
        *error = libraryError.isEmpty() ? tr("No Verilog-A module %1 could be read.").arg(type) : libraryError;
        return {};
    }
    // The library's parameters (what the simulator takes), the defaults
    // and descriptions from the source where the library has none.
    const va::VerilogModule& module = haveLibrary ? fromLibrary : fromSource;
    QJsonArray parameters;
    QStringList card;
    for (const va::Parameter& p : module.parameters) {
        va::Parameter q = p;
        for (const va::Parameter& s : fromSource.parameters)
            if (s.name.compare(p.name, Qt::CaseInsensitive) == 0) {
                if (q.value.isEmpty()) q.value = s.value;
                if (q.description.isEmpty()) q.description = s.description;
                if (q.units.isEmpty()) q.units = s.units;
            }
        QJsonObject o{{QStringLiteral("name"), q.name}, {QStringLiteral("default"), q.value},
                      {QStringLiteral("kind"), q.instance ? QStringLiteral("instance") : QStringLiteral("model")}};
        if (!q.units.isEmpty()) o.insert(QStringLiteral("units"), q.units);
        if (!q.description.isEmpty()) o.insert(QStringLiteral("description"), q.description);
        parameters.append(o);
        if (!q.instance && !q.value.isEmpty()) card << QStringLiteral("%1=%2").arg(q.name, q.value);
    }
    const QString root = QucsSettings.QucsWorkDir.absolutePath();
    const auto shownPath = [&root](const QString& f) {
        const QString rel = QDir(root).relativeFilePath(f);
        return QDir::toNativeSeparators(rel.startsWith(QLatin1String("..")) ? f : rel);
    };
    QJsonObject result{{QStringLiteral("module"), module.name},
                       {QStringLiteral("kind"), tr("Verilog-A module")},
                       {QStringLiteral("parameters"), parameters},
                       {QStringLiteral("model card"), QStringLiteral(".model %1_model %1 (%2)").arg(module.name, card.join(QLatin1Char(' ')))}};
    if (!source.isEmpty()) result.insert(QStringLiteral("source"), shownPath(source));
    if (!osdi.isEmpty() && haveLibrary) result.insert(QStringLiteral("compiled"), shownPath(osdi));
    else if (!source.isEmpty())
        result.insert(QStringLiteral("compiled"), osdi.isEmpty() ? tr("not yet: build_verilog_a compiles it (its parameters here are the source's)")
                                                                 : tr("%1 could not be read: %2").arg(shownPath(osdi), libraryError));
    result.insert(QStringLiteral("use"), tr("ngspice loads the compiled library (.osdi) and takes a .model line of the module with "
                                            "the model parameters (the card above has their defaults: change those that matter); an "
                                            "instance line names the model and sets the instance parameters. In a schematic, the "
                                            "project's Verilog-A components carry both."));
    return result;
}

namespace {

// Another value for a property, to see whether a netlist line uses it: a
// number (with its unit, if any) for a number, else the text altered.
QString probeValue(const QString& value)
{
    static const QRegularExpression number(QStringLiteral("^\\s*[-+]?(\\d+\\.?\\d*|\\.\\d+)([eE][-+]?\\d+)?\\s*([A-Za-z]*)\\s*(.*)$"));
    const QRegularExpressionMatch m = number.match(value);
    if (value.trimmed().isEmpty()) return QStringLiteral("1.234567");
    if (m.hasMatch()) {
        QString probe = QStringLiteral("1.234567");
        // (A scale letter, k or m, kept - "15 V" gives "1.234567 V".)
        if (!m.captured(3).isEmpty()) probe += (value.contains(QLatin1Char(' ')) ? QStringLiteral(" ") : QString()) + m.captured(3);
        if (!m.captured(4).isEmpty()) probe += QLatin1Char(' ') + m.captured(4);
        return probe;
    }
    return value + QStringLiteral("_q7");
}

// Where two netlist lines differ, as the first has it: the words around
// the change (up to some 90 characters).
QString differingPart(const QString& a, const QString& b)
{
    qsizetype pre = 0;
    while (pre < a.size() && pre < b.size() && a.at(pre) == b.at(pre)) ++pre;
    qsizetype suf = 0;
    while (suf < a.size() - pre && suf < b.size() - pre && a.at(a.size() - 1 - suf) == b.at(b.size() - 1 - suf)) ++suf;
    qsizetype from = pre, to = a.size() - suf;
    const auto boundary = [](QChar ch) { return ch.isSpace() || ch == QLatin1Char('\n'); };
    while (from > 0 && !boundary(a.at(from - 1))) --from;
    while (to < a.size() && !boundary(a.at(to))) ++to;
    QString part = a.mid(from, to - from).simplified();
    if (part.size() > 90) part = part.left(87) + QStringLiteral("...");
    return part;
}

// A new Verilog-A module that OpenVAF compiles as it is, and that runs in
// ngspice with no warning: the parameters' attributes before them, an
// instance parameter, every pin a DC path through the module.
const char* const kVerilogATemplate = R"VA(// A Verilog-A module for Qucs-S: OpenVAF compiles it (build_verilog_a),
// ngspice simulates it through OSDI. The module's name is the component's,
// its ports in order its pins.
`include "disciplines.vams"
`include "constants.vams"

module amp(inp, inn, out);
    inout inp, inn, out;
    electrical inp, inn, out;

    // Attributes come BEFORE the declaration they describe.
    (* desc = "open-loop voltage gain", units = "V/V" *)
    parameter real gain = 1e5 from (0:inf);
    (* desc = "input resistance", units = "Ohm" *)
    parameter real rin = 1e12 from (0:inf);
    (* desc = "output resistance", units = "Ohm" *)
    parameter real rout = 1 from [0:inf);
    // An instance parameter: set on each part, not in the model card.
    (* desc = "input offset voltage", units = "V", type = "instance" *)
    parameter real vos = 0;

    analog begin
        // Every node needs a DC path: a resistance between the inputs...
        I(inp, inn) <+ V(inp, inn) / rin;
        // ...and the output a potential contribution - a voltage source,
        // with rout in series (the branch's own current, I(out)).
        V(out) <+ gain * (V(inp, inn) + vos) + rout * I(out);
    end
endmodule
)VA";

bool asksForVerilogATemplate(const QString& type)
{
    QString t = type.toLower();
    t.remove(QRegularExpression(QStringLiteral("[\\s_\\-]")));
    static const QStringList asked{QStringLiteral("veriloga"), QStringLiteral("va"), QStringLiteral("newveriloga"),
                                   QStringLiteral("verilogamodule"), QStringLiteral("newverilogamodule"),
                                   QStringLiteral("verilogatemplate")};
    return asked.contains(t);
}

QJsonObject verilogATemplate()
{
    QJsonArray rules;
    rules << QObject::tr("Attributes come before the declaration they describe: (* desc = \"...\", units = \"V\" *) "
                         "parameter real gain = 1e5; - after it (parameter real gain = 1e5 (* desc = \"...\" *);) OpenVAF "
                         "stops: \"unexpected token '(*'; expected ',' or ';'\".")
          << QObject::tr("desc and units are what describe_component_type and the part's dialog show. type = \"instance\" makes "
                         "a parameter the instance's, set on each part; the others are the model's, in its .model card.")
          << QObject::tr("from (0:inf) leaves 0 out, from [0:inf) takes it; a value outside the range is refused when the model "
                         "loads.")
          << QObject::tr("V(a, b) <+ is a potential contribution - a voltage source from a to b; I(a, b) <+ a flow contribution - "
                         "a current. Contribute to a branch one way only. A series resistance goes on a potential "
                         "contribution with the branch's own current: V(out) <+ ... + rout * I(out).")
          << QObject::tr("Every node needs a DC path to ground. An input the module draws no current from, reached only through "
                         "a capacitor, is a node ngspice finds none for: it warns (\"no DC path from node ... to ground; gmin "
                         "installed\") or the matrix is singular. A large resistance across the inputs, I(inp, inn) <+ "
                         "V(inp, inn) / rin, gives one; a potential contribution gives its output one. (check_schematic takes a "
                         "Verilog-A part as conducting between its pins: it does not see this.)")
          << QObject::tr("`include \"disciplines.vams\" and \"constants.vams\" (OpenVAF has them): electrical, V(), I(), "
                         "`M_PI, ... A module's ports are declared twice: inout (their direction), electrical (their "
                         "discipline).");
    QJsonArray steps;
    steps << QObject::tr("Write it to a .va file in the project - the module's name is the part's, its ports in order the pins "
                         "(the template: amp, pins inp, inn, out).")
          << QObject::tr("build_verilog_a compiles it: each error with its line and column.")
          << QObject::tr("describe_component_type with the module's name then lists its parameters and gives the .model card "
                         "ngspice takes.");
    return {{QStringLiteral("type"), QObject::tr("a new Verilog-A module")},
            {QStringLiteral("kind"), QObject::tr("template")},
            {QStringLiteral("template"), QString::fromUtf8(kVerilogATemplate)},
            {QStringLiteral("rules"), rules},
            {QStringLiteral("steps"), steps},
            {QStringLiteral("checked"), QObject::tr("The template compiles with OpenVAF as it is and, in ngspice, amplifies "
                                                     "without a warning: an inverter of gain -10 made with it gives -1.0 V "
                                                     "for 0.1 V in.")}};
}

} // namespace

QJsonObject QucsControl::describeComponentType(const QJsonObject& args)
{
    const QString type = args.value(QLatin1String("type")).toString().trimmed();
    if (type.isEmpty()) return errorResult(tr("Which type? ('type', as list_component_types gives it)"));
    // "Verilog-A": a module to start from, and how OpenVAF wants it written.
    if (asksForVerilogATemplate(type)) return jsonResult(verilogATemplate());
    Module* m = nullptr;
    std::unique_ptr<Component> c(newComponent(type, &m));
    if (!c) {
        // A Verilog-A module: a .va or .osdi file, or its name.
        QString error;
        const QJsonObject module = describeVerilogAModule(type, a_app->allDocuments(), &error);
        if (!module.isEmpty()) return jsonResult(module);
        if (!error.isEmpty()) return errorResult(error);
        return errorResult(tr("There is no component type %1 (list_component_types lists them), nor a Verilog-A module of that "
                              "name in the project or the workspace.").arg(type));
    }
    QString name;
    if (m != nullptr && m->info != nullptr) {
        char* file = nullptr;
        m->info(name, file, false);
    }
    QString category;
    for (Category* cat : Category::Categories)
        if (cat->Content.contains(m)) category = cat->Name;

    // Turned and mirrored as a placed part is (mirrored first, as a file
    // has it): its pins where they then are.
    const QJsonValue rotationArg = args.value(QLatin1String("rotation"));
    if (!rotationArg.isUndefined() && (!rotationArg.isDouble() || rotationArg.toDouble() != std::floor(rotationArg.toDouble())
                                       || rotationArg.toInt() < 0 || rotationArg.toInt() > 3))
        return errorResult(tr("'rotation' is 0 to 3, a quarter turn each, as add_component takes it."));
    const int rotation = rotationArg.toInt(0);
    const bool mirror = args.value(QLatin1String("mirror")).toBool();
    if (mirror) c->mirrorX();
    for (int r = 0; r < rotation; ++r) c->rotate();

    QJsonArray pins;
    for (int i = 0; i < c->Ports.size(); ++i) {
        QJsonObject pin{{QStringLiteral("pin"), i + 1}, {QStringLiteral("x"), c->Ports.at(i)->x}, {QStringLiteral("y"), c->Ports.at(i)->y}};
        if (!c->Ports.at(i)->Name.isEmpty()) pin.insert(QStringLiteral("name"), c->Ports.at(i)->Name);
        pins.append(pin);
    }
    QJsonArray props;
    int order = 0;
    for (Property* p : c->Props) {
        QJsonObject o{{QStringLiteral("order"), ++order},
                      {QStringLiteral("name"), p->Name},
                      {QStringLiteral("default"), p->Value},
                      {QStringLiteral("shown"), p->display}};
        if (!p->Description.isEmpty()) o.insert(QStringLiteral("description"), p->Description);
        const QString unit = unitOf(p);
        if (!unit.isEmpty()) o.insert(QStringLiteral("unit"), unit);
        if (p->type == Property::Type::File) o.insert(QStringLiteral("kind"), QStringLiteral("file"));
        else if (p->type == Property::Type::Equation) o.insert(QStringLiteral("kind"), QStringLiteral("equation"));
        if ((p->simulators & spicecompat::simAll) != spicecompat::simAll) o.insert(QStringLiteral("simulators"), simulatorsOf(p->simulators));
        props.append(o);
    }
    QJsonObject result{{QStringLiteral("type"), type},
                       {QStringLiteral("name"), name},
                       {QStringLiteral("description"), c->Description},
                       {QStringLiteral("category"), category},
                       {QStringLiteral("pins"), pins},
                       {QStringLiteral("properties"), props},
                       {QStringLiteral("simulators"), simulatorsOf(c->Simulator)},
                       {QStringLiteral("property order"), tr("as listed: the order of the values in a .sch line")}};
    if (!c->Ports.isEmpty()) {
        if (rotation != 0 || mirror) {
            result.insert(QStringLiteral("rotation"), rotation);
            result.insert(QStringLiteral("mirror"), mirror);
        } else {
            result.insert(QStringLiteral("turned"),
                          tr("the pins above are at rotation 0; each quarter turn moves a pin at (x, y) to (y, -x): rotation 1 "
                             "puts it at (y, -x), 2 at (-x, -y), 3 at (-y, x); mirror puts it at (x, -y) first. 'rotation' and "
                             "'mirror' here give them turned so."));
        }
    }
    if (const QJsonArray repeated = repeatedProperties(c.get()); !repeated.isEmpty())
        result.insert(QStringLiteral("repeated properties"), repeated);
    const QString prefix = c->Name;
    if (!prefix.isEmpty() && prefix != QLatin1String("*"))
        result.insert(QStringLiteral("names"), tr("%1 and a number: %2, %3, ... (the next free one when add_component is given none)")
                                                   .arg(prefix, prefix + QLatin1Char('1'), prefix + QLatin1Char('2')));
    if (c->isSimulation) result.insert(QStringLiteral("kind"), QStringLiteral("simulation"));
    else if (c->isEquation) result.insert(QStringLiteral("kind"), QStringLiteral("equation"));
    else if (c->isProbe) result.insert(QStringLiteral("kind"), QStringLiteral("probe"));

    // What it netlists as under the SPICE simulator in the settings, with
    // its defaults: those of a file or a subcircuit need the schematic.
    static const QStringList needsSchematic = {QStringLiteral("Lib"), QStringLiteral(".SW"), QStringLiteral(".SP"),
                                               QStringLiteral("SPfile"), QStringLiteral("Sub"), QStringLiteral("SPICE"),
                                               QStringLiteral("SpiceLib"), QStringLiteral("SpiceInclude"),
                                               QStringLiteral("SpLib"), QStringLiteral("XSP_CMlib")};
    const int sim = QucsSettings.DefaultSimulator;
    if (sim != spicecompat::simQucsator && (c->Simulator & sim) && !needsSchematic.contains(type)) {
        std::vector<std::unique_ptr<Node>> nodes;
        c->Name += QLatin1Char('1');
        for (int i = 0; i < c->Ports.size(); ++i) {
            nodes.push_back(std::make_unique<Node>(0, 0));
            nodes.back()->Name = QStringLiteral("n%1").arg(i + 1);
            c->Ports.at(i)->Connection = nodes.back().get();
        }
        const spicecompat::SpiceDialect dialect = sim == spicecompat::simXyce ? spicecompat::SPICEXyce : spicecompat::SPICEDefault;
        const auto lineOf = [&] { return (c->isEquation ? c->getExpression(dialect) : c->getSpiceNetlist(dialect)).trimmed(); };
        const QString line = lineOf();
        // The properties not shown on the schematic that the line uses all
        // the same (an OpAmp's Umax clips its output at 15 V): each given
        // another value in turn, the line compared.
        QJsonArray hidden;
        if (!line.isEmpty())
            for (Property* p : c->Props) {
                if (p->display) continue;
                const QString was = p->Value;
                p->Value = probeValue(was);
                const QString probed = lineOf();
                p->Value = was;
                if (probed == line) continue;
                QJsonObject o{{QStringLiteral("name"), p->Name}, {QStringLiteral("default"), was},
                              {QStringLiteral("in the line"), differingPart(line, probed)}};
                if (!p->Description.isEmpty()) o.insert(QStringLiteral("description"), p->Description);
                hidden.append(o);
            }
        for (Port* p : c->Ports) p->Connection = nullptr;
        if (!line.isEmpty()) {
            QJsonObject netlist{{QStringLiteral("simulator"), spicecompat::getDefaultSimulatorName(sim)},
                                {QStringLiteral("pins as"), QStringLiteral("n1, n2, ...")},
                                {QStringLiteral("with the defaults"), line}};
            if (!hidden.isEmpty()) {
                netlist.insert(QStringLiteral("hidden but in it"), hidden);
                QStringList names;
                for (const QJsonValue& h : std::as_const(hidden)) names << h.toObject().value(QStringLiteral("name")).toString();
                result.insert(QStringLiteral("hidden properties"),
                              tr("%1 not shown on the schematic, yet in the netlist: their values count (netlist, "
                                 "'hidden but in it', says where). edit_component 'shown' shows one.")
                                  .arg(names.join(QStringLiteral(", "))));
            }
            result.insert(QStringLiteral("netlist"), netlist);
        }
    }
    const QStringList notes = notesOn(type);
    if (!notes.isEmpty()) result.insert(QStringLiteral("notes"), QJsonArray::fromStringList(notes));
    return jsonResult(result);
}
