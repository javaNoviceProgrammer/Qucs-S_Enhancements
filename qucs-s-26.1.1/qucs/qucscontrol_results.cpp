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
#include "dataset.h"
#include "diagrams/diagrams.h"
#include "extsimkernels/simulationrun.h"
#include "extsimkernels/spicecompat.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "node.h"
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

using namespace qucs_s::control;
namespace ds = qucs_s::dataset;

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
const char* const kNumbers[] = {"real_imaginary", "magnitude_degrees", "magnitude_radians"};

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
    if (o.contains(QLatin1String("label"))) a->Label = o.value(QLatin1String("label")).toString();
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

// Sets what a diagram's arguments give: place and size, grid, legend,
// axes. What is not given stays.
bool applyDiagram(Diagram* d, const QJsonObject& args, QString* error)
{
    if (args.contains(QLatin1String("x"))) d->cx = misc::clampCoordinate(args.value(QLatin1String("x")).toInt());
    if (args.contains(QLatin1String("y"))) d->cy = misc::clampCoordinate(args.value(QLatin1String("y")).toInt());
    if (args.contains(QLatin1String("width"))) d->x2 = std::clamp(args.value(QLatin1String("width")).toInt(), 10, 100000);
    if (args.contains(QLatin1String("height"))) d->y2 = std::clamp(args.value(QLatin1String("height")).toInt(), 10, 100000);
    if (args.contains(QLatin1String("grid"))) d->xAxis.GridOn = d->yAxis.GridOn = args.value(QLatin1String("grid")).toBool();
    if (args.contains(QLatin1String("legend"))) {
        const QJsonValue v = args.value(QLatin1String("legend"));
        const int pos = v.isBool() ? (v.toBool() ? int(Diagram::LegendTopRight) : int(Diagram::LegendOff)) : indexIn(kLegends, v.toString());
        if (pos < 0) {
            *error = tr("The legend is one of %1.").arg(namesOf(kLegends));
            return false;
        }
        d->legendPos = pos;
    }
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
            *error = tr("%1 is no color: give #rrggbb, a name (red, darkgreen, ...) or auto.").arg(color);
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
    if (o.contains(QLatin1String("precision"))) g->Precision = std::clamp(o.value(QLatin1String("precision")).toInt(), 0, 16);
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
    // As the diagram's dialog writes it (with its simulator): as it is.
    if (!sim.isEmpty()) return w;
    const QString prefix = simulatorPrefix();
    const auto named = [&prefix](const QString& name) { return prefix.isEmpty() ? name : prefix + QLatin1Char('/') + name; };
    // Of another dataset ("run1:tran.v(out)", one simulate kept): the
    // simulator's, as the current run's.
    if (bare.contains(QLatin1Char(':'))) return named(bare);
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
    // A SPICE simulator's variable is named after its analysis: when the
    // schematic has one analysis, that one.
    QString name = bare;
    if (!prefix.isEmpty()) {
        if (!name.contains(QLatin1Char('('))) name = QStringLiteral("v(%1)").arg(name);
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
    return name.endsWith(QLatin1String(".dat")) || name.contains(QLatin1String(".dat."));
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
    ds::Form form = ds::Form::MagnitudePhase;
    QString prefix;   // the simulator's, for the name of a trace
};

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

QJsonObject variableJson(const ds::Dataset& data, const ds::Variable& v, const ReadOptions& o)
{
    QJsonObject out{{QStringLiteral("name"), v.name}};
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
    if (!o.prefix.isEmpty()) out.insert(QStringLiteral("trace"), o.prefix + QLatin1Char('/') + v.name);
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
            for (const QString& what : o.measure) m.insert(what, ds::measure(part, what, o.measureOptions));
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
        notes << tr("One pulse, not a train: under the SPICE simulators it is netlisted as PULSE with no period, so it "
                    "rises at T1, falls so that it ends at T2 and stays at the first value after. For a repeating pulse "
                    "train use %1.").arg(other);
    }
    if (type == QLatin1String("Vrect") || type == QLatin1String("Irect"))
        notes << tr("A repeating rectangular wave: high for TH, low for TL, with edges Tr and Tf between them (period "
                    "TH + TL + Tr + Tf), from Td on. Under the SPICE simulators it is netlisted as PULSE with that "
                    "period; the value outside the pulses is U0 (I0).");
    if (type == QLatin1String("Vdc") || type == QLatin1String("Idc"))
        notes << tr("A DC value only: nothing in an AC analysis. To drive an AC analysis use Vac (Iac), whose "
                    "amplitude is also the AC magnitude.");
    if (type == QLatin1String(".TR"))
        notes << tr("Under ngspice the transient's print step is (Stop - Start) / (Points - 1): the dataset has at least "
                    "Points samples; raise Points for finer results. MaxStep bounds the simulator's own step.");
    if (type == QLatin1String("GND"))
        notes << tr("Every circuit needs a ground (the SPICE node 0); its pin is GND.1 to connect.");
    if (type == QLatin1String("Eqn"))
        notes << tr("Under ngspice an equation that uses no simulated voltage or current becomes a .PARAM line; one "
                    "that does is computed after each analysis from its results, and is in the dataset (NutmegEq "
                    "does that for one analysis you name).");
    return notes;
}

} // namespace

namespace qucs_s::control {

Component* newComponent(const QString& type, Module** module)
{
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
        if (d->Name != QLatin1String("Tab") && d->Name != QLatin1String("Truth")) {
            o.insert(QStringLiteral("x_axis"), axisJson(d->xAxis, false));
            o.insert(QStringLiteral("y_axis"), axisJson(d->yAxis, true));
            if (hasRightAxis(d)) o.insert(QStringLiteral("y2_axis"), axisJson(d->zAxis, true));
            o.insert(QStringLiteral("grid"), d->xAxis.GridOn);
            if (d->legendPos >= 0 && d->legendPos < int(std::size(kLegends)))
                o.insert(QStringLiteral("legend"), QString::fromLatin1(kLegends[d->legendPos]));
        }
        QJsonArray traces;
        int t = 0;
        for (Graph* g : d->Graphs) traces.append(traceJson(sch, d, g, ++t));
        o.insert(QStringLiteral("traces"), traces);
        list.append(o);
    }
    return list;
}

Diagram* diagramOf(Schematic* sch, const QJsonValue& which, QString* error)
{
    const int count = int(sch->a_DocDiags.size());
    if (count == 0) {
        *error = tr("%1 has no diagram (add_diagram places one).").arg(QFileInfo(sch->getDocName()).fileName());
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

QString whyNoData(Schematic* sch, Graph* g)
{
    if (!g->isEmpty()) return QString();
    if (sch->getDocName().isEmpty()) return tr("the document has no file yet, so no dataset");
    QString variable;
    const QString file = traceFile(sch, g->Var, &variable);
    if (!QFileInfo::exists(file)) return tr("there is no dataset %1 (simulate to make it)").arg(QFileInfo(file).fileName());
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
    const auto cased = [&to](const QString& found) {
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

QJsonObject QucsControl::getDataset(const QJsonObject& args)
{
    QString error;
    const QString file = datasetPath(args, &error);
    if (file.isEmpty()) return errorResult(error);
    ds::Dataset data;
    if (!data.read(file, &error)) return errorResult(error);
    const QFileInfo info(file);
    QJsonObject result{{QStringLiteral("dataset"), QDir::toNativeSeparators(file)},
                       {QStringLiteral("written"), info.lastModified().toString(Qt::ISODate)}};
    ReadOptions o;
    const qsizetype dot = info.fileName().indexOf(QLatin1String(".dat."));
    if (dot >= 0) o.prefix = info.fileName().mid(dot + 5);

    const QJsonArray wanted = args.value(QLatin1String("variables")).toArray();
    if (wanted.isEmpty()) {
        QJsonArray independent, variables;
        for (const ds::Variable& v : data.variables()) {
            if (v.independent) {
                independent.append(variableJson(data, v, o));
                continue;
            }
            QJsonObject e{{QStringLiteral("name"), v.name}, {QStringLiteral("depends on"), QJsonArray::fromStringList(v.dependencies)},
                          {QStringLiteral("points"), v.size()}};
            if (!o.prefix.isEmpty()) e.insert(QStringLiteral("trace"), o.prefix + QLatin1Char('/') + v.name);
            if (v.isComplex()) e.insert(QStringLiteral("complex"), true);
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
    const QString form = args.value(QLatin1String("form")).toString();
    if (form == QLatin1String("db_phase")) o.form = ds::Form::DbPhase;
    else if (form == QLatin1String("real_imaginary")) o.form = ds::Form::RealImaginary;
    o.points = args.contains(QLatin1String("points")) ? std::clamp(args.value(QLatin1String("points")).toInt(), 0, 5000)
                                                      : (o.at.isEmpty() && o.measure.isEmpty() ? 100 : 0);

    QJsonArray out;
    QStringList missing;
    QStringList done;
    for (const QJsonValue& w : wanted) {
        const QStringList names = data.resolve(w.toString());
        if (names.isEmpty()) missing << w.toString();
        for (const QString& name : names) {
            if (done.contains(name) || done.size() >= 20) continue;
            done << name;
            out.append(variableJson(data, *data.find(name), o));
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
    return jsonResult(result, true);
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

QJsonObject QucsControl::getNetlist(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, false);
    if (sch == nullptr) return errorResult(error);
    const bool last = args.value(QLatin1String("last")).toBool();
    const int simulator = QucsSettings.DefaultSimulator;
    const QString simName = spicecompat::getDefaultSimulatorName(simulator);
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
        files << file;
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

QJsonObject QucsControl::addDiagram(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) return errorResult(error);
    const QString type = args.value(QLatin1String("type")).toString(QStringLiteral("rect")).trimmed();
    std::unique_ptr<Diagram> d(newDiagram(type.isEmpty() ? QStringLiteral("rect") : type));
    if (!d) return errorResult(tr("There is no diagram type %1: %2.").arg(type, kindNames()));
    if (!args.contains(QLatin1String("x")) || !args.contains(QLatin1String("y")))
        return errorResult(tr("Where? ('x', 'y': its lower left corner)"));
    if (!applyDiagram(d.get(), args, &error)) return errorResult(error);
    int x = d->cx, y = d->cy;
    sch->setOnGrid(x, y);
    d->cx = x;
    d->cy = y;
    QStringList notes;
    for (const QJsonValue& v : args.value(QLatin1String("traces")).toArray()) {
        const QJsonObject t = v.isString() ? QJsonObject{{QStringLiteral("variable"), v.toString()}} : v.toObject();
        QString note;
        const QString var = traceVariable(sch, d.get(), t.value(QLatin1String("variable")).toString(), &note, &error);
        if (var.isEmpty()) return errorResult(error);
        auto* g = new Graph(d.get(), var);
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
        if (!note.isEmpty()) notes << note;
    }
    prepare(sch);
    Diagram* placed = d.release();
    sch->a_DocDiags.push_back(placed);
    reloadDiagram(sch, placed);
    sch->enlargeView(placed);
    finish(sch, {QPoint(placed->cx, placed->cy)});
    QJsonObject result = diagramsJson(sch).last().toObject();
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
    return jsonResult(diagramsJson(sch).at(n).toObject());
}

QJsonObject QucsControl::addTrace(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, true);
    if (sch == nullptr) return errorResult(error);
    Diagram* d = diagramOf(sch, args.value(QLatin1String("diagram")), &error);
    if (d == nullptr) return errorResult(error);
    QString note;
    const QString var = traceVariable(sch, d, args.value(QLatin1String("variable")).toString(), &note, &error);
    if (var.isEmpty()) return errorResult(error);
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
    if (args.contains(QLatin1String("variable"))) {
        var = traceVariable(sch, d, args.value(QLatin1String("variable")).toString(), &note, &error);
        if (var.isEmpty()) return errorResult(error);
    }
    // Tried on a copy first.
    Graph trial(d, var);
    trial.Color = g->Color;
    if (!applyTrace(&trial, d, args, &error)) return errorResult(error);
    prepare(sch);
    applyTrace(g, d, args, &error);
    if (var != g->Var) {
        g->Var = var;
        // The markers were of the curve before.
        qDeleteAll(g->Markers);
        g->Markers.clear();
    }
    reloadDiagram(sch, d);
    finish(sch, {QPoint(d->cx, d->cy)});
    QJsonObject result = traceJson(sch, d, g, int(d->Graphs.indexOf(g)) + 1);
    if (!note.isEmpty()) result.insert(QStringLiteral("note"), note);
    return jsonResult(result);
}

QJsonObject QucsControl::describeComponentType(const QJsonObject& args)
{
    const QString type = args.value(QLatin1String("type")).toString().trimmed();
    if (type.isEmpty()) return errorResult(tr("Which type? ('type', as list_component_types gives it)"));
    Module* m = nullptr;
    std::unique_ptr<Component> c(newComponent(type, &m));
    if (!c) return errorResult(tr("There is no component type %1 (list_component_types lists them).").arg(type));
    QString name;
    if (m != nullptr && m->info != nullptr) {
        char* file = nullptr;
        m->info(name, file, false);
    }
    QString category;
    for (Category* cat : Category::Categories)
        if (cat->Content.contains(m)) category = cat->Name;

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
        QString line = c->isEquation ? c->getExpression(dialect) : c->getSpiceNetlist(dialect);
        for (Port* p : c->Ports) p->Connection = nullptr;
        line = line.trimmed();
        if (!line.isEmpty())
            result.insert(QStringLiteral("netlist"), QJsonObject{{QStringLiteral("simulator"), spicecompat::getDefaultSimulatorName(sim)},
                                                                 {QStringLiteral("pins as"), QStringLiteral("n1, n2, ...")},
                                                                 {QStringLiteral("with the defaults"), line}});
    }
    const QStringList notes = notesOn(type);
    if (!notes.isEmpty()) result.insert(QStringLiteral("notes"), QJsonArray::fromStringList(notes));
    return jsonResult(result);
}
