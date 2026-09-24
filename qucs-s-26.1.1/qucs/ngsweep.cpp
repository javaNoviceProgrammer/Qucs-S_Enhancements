/*
 * ngsweep.cpp - the NgSweep component's ngspice command (see ngsweep.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "ngsweep.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>

#include <cmath>

#include "components/component.h"
#include "ngoptimize.h"
#include "schematic.h"
#include "valuereading.h"

namespace qucs_s::ngsweep {

using ngstats::RawPlot;
using ngstats::RawVector;
using ngstats::Record;

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("NgSweep", text);
}

// "List", not "Values": the component dialog keeps a property of that
// name for the parameter sweep's own use.
const char* const kFixed[] = {"Analysis", "Param", "Type", "Start", "Stop", "Points", "List", "Waveforms"};

// A value as ngspice reads it ("4.7k", "1e-9", "0.5 V"), else false.
bool spiceNumber(const QString& text, QString* out)
{
    const units::Reading r = units::read(text);
    if (r.kind != units::Reading::Number || !std::isfinite(r.value)) return false;
    *out = QString::number(r.value, 'g', 10);
    return true;
}

// A whole number of at least \a least, else false.
bool wholeNumber(const QString& text, qint64 least, qint64* out)
{
    bool ok = false;
    const qint64 v = text.trimmed().toLongLong(&ok);
    if (!ok || v < least) return false;
    *out = v;
    return true;
}

// An expression as one word: ngspice takes an -output as a single word.
QString oneToken(const QString& expression)
{
    QString s = expression;
    s.remove(QRegularExpression(QStringLiteral("\\s+")));
    return s;
}

bool validName(const QString& name)
{
    static const QRegularExpression rx(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
    return rx.match(name).hasMatch();
}

// A knob as one word ngspice can read: no spaces, no quotes, not a flag.
bool validKnob(const QString& name)
{
    static const QRegularExpression rx(QStringLiteral("^[^\\s\"'-][^\\s\"']*$"));
    return rx.match(name).hasMatch();
}

QString firstWord(const QString& command)
{
    return command.trimmed().section(QLatin1Char(' '), 0, 0).toLower();
}

// The names the results of a run take for themselves in the dataset.
bool reservedName(const QString& lower)
{
    return lower == QLatin1String("frequency") || lower == QLatin1String("time");
}

// The display flags of the properties as they are, by name and occurrence,
// so rewriting the list keeps what the schematic shows.
class Shown
{
public:
    explicit Shown(const Component* component)
    {
        for (const Property* p : component->Props) m_shown[p->Name] << p->display;
    }
    bool operator()(const QString& name, bool fallback)
    {
        const int i = m_seen[name]++;
        const QList<bool>& d = m_shown.value(name);
        return i < d.size() ? d.at(i) : fallback;
    }

private:
    QHash<QString, QList<bool>> m_shown;
    QHash<QString, int> m_seen;
};

// ---- the dataset ----------------------------------------------------------

QString number(double v)
{
    return QString::number(v, 'e', 12);
}

QString value(const RawVector& v, int i)
{
    if (v.im.isEmpty()) return number(v.re.at(i));
    const double im = v.im.at(i);
    return number(v.re.at(i)) + (im < 0 ? QStringLiteral("-j") : QStringLiteral("+j")) + number(std::fabs(im));
}

// A variable's name in the dataset: under the component's, and without
// the colon Qucs reserves for the dataset.
QString datasetName(const QString& prefix, const QString& name)
{
    QString n = name;
    n.replace(QLatin1Char(':'), QLatin1Char('_'));
    return prefix + QLatin1Char('.') + n;
}

void indep(QTextStream& s, const QString& name, const QVector<double>& values)
{
    s << "<indep " << name << ' ' << values.size() << ">\n";
    for (double v : values) s << number(v) << '\n';
    s << "</indep>\n";
}

// \a y (on \a x) at \a at, straight between the points, the end values
// beyond them. \a x ascending.
double interpolate(const QVector<double>& x, const QVector<double>& y, int n, double at)
{
    if (n <= 0) return std::nan("");
    if (at <= x.at(0)) return y.at(0);
    if (at >= x.at(n - 1)) return y.at(n - 1);
    int lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        const int mid = (lo + hi) / 2;
        (x.at(mid) <= at ? lo : hi) = mid;
    }
    const double dx = x.at(hi) - x.at(lo);
    return dx == 0 ? y.at(lo) : y.at(lo) + (y.at(hi) - y.at(lo)) * (at - x.at(lo)) / dx;
}

// \a v, which a run gave on \a scale, on \a onto: as it is when the run's
// points are those, else interpolated (a transient ngspice did not keep
// on its step).
RawVector onScale(const RawVector& v, const RawVector& scale, const QVector<double>& onto)
{
    const int n = scale.length();
    bool same = n == onto.size() && scale.re.size() >= n && v.re.size() >= n;
    for (int i = 0; same && i < n; ++i)
        same = std::fabs(scale.re.at(i) - onto.at(i)) <= 1e-12 * std::max(1.0, std::fabs(onto.at(i)));
    if (same) return v;
    RawVector out;
    out.name = v.name;
    out.type = v.type;
    out.dims = {int(onto.size())};
    const int m = std::min({n, int(scale.re.size()), int(v.re.size())});
    for (double at : onto) {
        out.re << interpolate(scale.re, v.re, m, at);
        if (!v.im.isEmpty()) out.im << interpolate(scale.re, v.im, std::min(m, int(v.im.size())), at);
    }
    return out;
}

// The waveforms of every run: a family per vector, over the first run's
// scale and the knobs. False when the file does not hold one plot for
// every run, all of one kind.
bool waveformBlocks(QTextStream& s, const QString& path, const QString& prefix, const QStringList& knobVars, int runs)
{
    const QList<RawPlot> waves = ngstats::readRaw(path);
    if (runs < 1 || waves.size() != runs || waves.first().vectors.isEmpty()) return false;
    const RawVector& scale = waves.first().vectors.first();
    const int l = scale.length();
    if (l < 1 || scale.re.size() < l) return false;
    for (const RawPlot& w : waves)
        if (w.name != waves.first().name || w.vectors.isEmpty() || w.vectors.first().name != scale.name
            || w.vectors.first().length() < 1 || w.vectors.first().re.size() < w.vectors.first().length())
            return false;
    const QVector<double> grid = scale.re.mid(0, l);
    const QString scaleVar = datasetName(prefix, scale.name);
    indep(s, scaleVar, grid);
    for (int i = 1; i < waves.first().vectors.size(); ++i) {
        const QString name = waves.first().vectors.at(i).name;
        QList<RawVector> each;
        for (const RawPlot& w : waves) {
            const RawVector* v = w.vector(name);
            if (v == nullptr || v->re.size() < v->length()) break;
            each << onScale(*v, w.vectors.first(), grid);
        }
        if (each.size() != runs) continue;
        s << "<dep " << datasetName(prefix, name) << ' ' << scaleVar << ' ' << knobVars.join(QLatin1Char(' '))
          << ">\n";
        for (const RawVector& v : each)
            for (int j = 0; j < l; ++j) s << value(v, j) << '\n';
        s << "</dep>\n";
    }
    return true;
}

// The name ngspice gives a knob's axis: the knob as the line was read
// (lower case), everything but letters, digits and _ an _.
QString ngspiceScaleName(const QString& knob)
{
    QString s = knob.toLower();
    for (QChar& c : s)
        if (!c.isLetterOrNumber() && c != QLatin1Char('_')) c = QLatin1Char('_');
    return s;
}

// What ngspice appends to an output's name for a curve of the outer
// knobs: "_r2_1000_c1_1e_09".
QString familySuffix(const QList<Knob>& knobs, const QList<QVector<double>>& values, int combination)
{
    QString suffix;
    int c = combination;
    for (int j = 1; j < knobs.size(); ++j) {
        const int n = int(values.at(j).size());
        const int index = c % n;
        c /= n;
        suffix += QLatin1Char('_') + ngspiceScaleName(knobs.at(j).name) + QLatin1Char('_')
                  + QString::asprintf("%g", values.at(j).at(index));
    }
    for (QChar& ch : suffix)
        if (!ch.isLetterOrNumber() && ch != QLatin1Char('_')) ch = QLatin1Char('_');
    return suffix;
}

// The lines ngspice printed between the markers of \a name.
QStringList block(const QString& output, const QString& name)
{
    const QString begin = QStringLiteral("qucs-s: begin ") + name;
    const QString end = QStringLiteral("qucs-s: end ") + name;
    QStringList lines;
    bool inside = false;
    for (QString line : output.split(QLatin1Char('\n'))) {
        line.remove(QLatin1Char('\r'));
        if (line.trimmed() == begin) {
            inside = true;
            lines.clear();
            continue;
        }
        if (line.trimmed() == end) return lines;
        if (inside) lines << line;
    }
    return inside ? lines : QStringList();
}

QList<Knob> allKnobs(const Sweep& sweep)
{
    return QList<Knob>{sweep.knob} + sweep.outer;
}

// Every knob's values; false when one cannot be worked out.
bool allValues(const QList<Knob>& knobs, QList<QVector<double>>* values, QString* error = nullptr)
{
    for (const Knob& k : knobs) {
        QVector<double> v;
        QString spec;
        if (!knobValues(k, &v, &spec, error)) return false;
        *values << v;
    }
    return true;
}

int runCount(const QList<QVector<double>>& values)
{
    qint64 n = 1;
    for (const QVector<double>& v : values) n *= v.size();
    return n > kMaxRuns ? kMaxRuns + 1 : int(n);
}

const Component* sweepNamed(const Schematic* schematic, const QString& lowerName)
{
    if (schematic == nullptr) return nullptr;
    for (const Component* c : schematic->a_DocComps)
        if (isSweep(c) && c->Name.toLower() == lowerName) return c;
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------

bool Knob::parse(const QString& value, Knob* knob)
{
    const QStringList f = value.split(QLatin1Char('|'));
    if (f.size() < 2) return false;
    knob->name = f.at(0).trimmed();
    knob->type = f.at(1).trimmed().toLower();
    knob->start = f.value(2).trimmed();
    knob->stop = f.value(3).trimmed();
    knob->points = f.value(4).trimmed();
    knob->values = f.value(5).trimmed();
    return true;
}

QString Knob::toString() const
{
    return QStringList({name, type, start, stop, points, values}).join(QLatin1Char('|'));
}

bool knobValues(const Knob& knob, QVector<double>* values, QString* spec, QString* error)
{
    auto fail = [&](const QString& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    const QString type = knob.type.trimmed().toLower();
    QVector<double> v;
    QString s;
    if (type == QLatin1String("list")) {
        QString text = knob.values;
        text.remove(QLatin1Char('[')).remove(QLatin1Char(']'));
        const QStringList items = text.split(QRegularExpression(QStringLiteral("[;,\\s]+")), Qt::SkipEmptyParts);
        if (items.isEmpty()) return fail(tr("%1: no values to sweep").arg(knob.name));
        if (items.size() > kMaxRuns) return fail(tr("%1: more than %2 values").arg(knob.name).arg(kMaxRuns));
        QStringList words;
        for (const QString& item : items) {
            QString n;
            if (!spiceNumber(item, &n)) return fail(tr("%1: \"%2\" is not a number").arg(knob.name, item));
            words << n;
            v << n.toDouble();
        }
        s = QStringLiteral("list ") + words.join(QLatin1Char(' '));
    } else if (type == QLatin1String("lin") || type == QLatin1String("log")) {
        QString a, b;
        qint64 n = 0;
        if (!spiceNumber(knob.start, &a))
            return fail(tr("%1: the start \"%2\" is not a number").arg(knob.name, knob.start));
        if (!spiceNumber(knob.stop, &b))
            return fail(tr("%1: the stop \"%2\" is not a number").arg(knob.name, knob.stop));
        if (!wholeNumber(knob.points, 1, &n))
            return fail(tr("%1: the points \"%2\" are not a whole number above 0").arg(knob.name, knob.points));
        if (n > kMaxRuns) return fail(tr("%1: more than %2 points").arg(knob.name).arg(kMaxRuns));
        const double f0 = a.toDouble(), f1 = b.toDouble();
        if (type == QLatin1String("lin")) {
            // As ngspice spaces them.
            s = QStringLiteral("lin %1 %2 %3").arg(n).arg(a, b);
            for (qint64 i = 0; i < n; ++i) v << (n == 1 ? f0 : f0 + (f1 - f0) * double(i) / double(n - 1));
        } else {
            if (!(f0 > 0 && f1 > 0))
                return fail(tr("%1: a logarithmic sweep needs a start and a stop above 0").arg(knob.name));
            // ngspice's dec counts points in a decade; Points is all of
            // them, so they go as a list.
            QStringList words;
            for (qint64 i = 0; i < n; ++i) {
                const double x = n == 1 ? f0 : f0 * std::pow(f1 / f0, double(i) / double(n - 1));
                const QString w = QString::number(x, 'g', 10);
                words << w;
                v << w.toDouble();
            }
            s = QStringLiteral("list ") + words.join(QLatin1Char(' '));
        }
    } else {
        return fail(tr("%1: the sweep type \"%2\" is not lin, log or list").arg(knob.name, knob.type));
    }
    if (values != nullptr) *values = v;
    if (spec != nullptr) *spec = s;
    return true;
}

Sweep Sweep::read(const Component* component)
{
    Sweep c;
    if (component == nullptr) return c;
    for (const Property* p : component->Props) {
        const QString v = p->Value.trimmed();
        if (p->Name == QLatin1String("Analysis")) c.analysis = v;
        else if (p->Name == QLatin1String("Param")) c.knob.name = v;
        else if (p->Name == QLatin1String("Type")) c.knob.type = v.toLower();
        else if (p->Name == QLatin1String("Start")) c.knob.start = v;
        else if (p->Name == QLatin1String("Stop")) c.knob.stop = v;
        else if (p->Name == QLatin1String("Points")) c.knob.points = v;
        else if (p->Name == QLatin1String("List")) c.knob.values = v;
        else if (p->Name == QLatin1String("Waveforms")) c.waveforms = v == QLatin1String("yes");
        else if (p->Name == QLatin1String("Vs")) {
            Knob k;
            if (Knob::parse(v, &k)) c.outer << k;
        } else if (p->Name == QLatin1String("Record")) {
            Record r;
            if (Record::parse(v, &r)) c.records << r;
        }
    }
    return c;
}

void Sweep::write(Component* component) const
{
    Shown shown(component);
    const bool list = knob.type == QLatin1String("list");
    const QStringList values = {analysis,    knob.name,   knob.type,   knob.start,
                                knob.stop,   knob.points, knob.values, waveforms ? QStringLiteral("yes") : QStringLiteral("no")};
    QList<Property*> props;
    for (int i = 0; i < int(std::size(kFixed)); ++i) {
        const QString name = QString::fromLatin1(kFixed[i]);
        bool display;
        // The values a list sweeps, or the range: whichever there is.
        if (name == QLatin1String("List")) display = list;
        else if (name == QLatin1String("Start") || name == QLatin1String("Stop") || name == QLatin1String("Points"))
            display = !list;
        else display = shown(name, name != QLatin1String("Waveforms"));
        props << new Property(name, values.at(i), display, QString());
    }
    for (const Knob& k : outer) props << new Property(QStringLiteral("Vs"), k.toString(), shown(QStringLiteral("Vs"), true), QString());
    for (const Record& r : records)
        props << new Property(QStringLiteral("Record"), r.toString(), shown(QStringLiteral("Record"), true), QString());
    qDeleteAll(component->Props);
    component->Props = props;
}

bool isSweep(const Component* component)
{
    return component != nullptr && component->Model == QLatin1String(kModel);
}

bool waveformAnalysis(const QString& analysis)
{
    const QString w = firstWord(analysis);
    return w == QLatin1String("ac") || w == QLatin1String("dc") || w == QLatin1String("tran");
}

QString knobVariable(const QString& knob)
{
    QString s = knob.toLower();
    s.remove(QLatin1Char('@')).remove(QLatin1Char(']'));
    for (QChar& c : s)
        if (!c.isLetterOrNumber() && c != QLatin1Char('_')) c = QLatin1Char('_');
    s.replace(QRegularExpression(QStringLiteral("_+")), QStringLiteral("_"));
    while (s.startsWith(QLatin1Char('_'))) s.remove(0, 1);
    while (s.endsWith(QLatin1Char('_'))) s.chop(1);
    return s.isEmpty() ? QStringLiteral("knob") : s;
}

bool commandLine(const Sweep& command, const Schematic* schematic, const QString& nodes, QString* line, QString* error)
{
    auto fail = [&](const QString& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    const QString analysis = ngopt::analysisCommand(schematic, command.analysis);
    if (analysis.isEmpty()) return fail(tr("no analysis to run"));
    const QList<Knob> knobs = allKnobs(command);
    if (knobs.size() > kMaxKnobs)
        return fail(tr("more than %1 parameters (ngspice sweeps up to %1 at once)").arg(kMaxKnobs));

    QStringList parts{QStringLiteral("sweep")};
    QSet<QString> names;
    qint64 runs = 1;
    for (int j = 0; j < knobs.size(); ++j) {
        const Knob& k = knobs.at(j);
        if (k.name.isEmpty()) return fail(j == 0 ? tr("no parameter to sweep") : tr("an outer sweep has no parameter"));
        if (!validKnob(k.name)) return fail(tr("\"%1\" is not a parameter ngspice can read (no spaces or quotes)").arg(k.name));
        const QString variable = knobVariable(k.name);
        if (names.contains(variable)) return fail(tr("%1 is swept twice").arg(k.name));
        if (reservedName(variable)) return fail(tr("%1 is the name of the analysis' own scale").arg(k.name));
        names << variable;
        QVector<double> values;
        QString spec, why;
        if (!knobValues(k, &values, &spec, &why)) return fail(why);
        runs *= values.size();
        if (runs > kMaxRuns) return fail(tr("more than %1 runs").arg(kMaxRuns));
        if (j > 0) parts << QStringLiteral("-vs");
        parts << k.name << spec;
    }
    parts << QStringLiteral("-analysis") << analysis.simplified();

    QStringList outputs;
    QSet<QString> recorded;
    for (const Record& r : command.records) {
        if (!validName(r.name)) return fail(tr("the name \"%1\" is not a name ngspice takes (letters, digits, _)").arg(r.name));
        const QString lower = r.name.toLower();
        if (names.contains(lower) || reservedName(lower)) return fail(tr("the name %1 is taken by the results").arg(r.name));
        if (recorded.contains(lower)) return fail(tr("the name %1 is given twice").arg(r.name));
        if (r.expression.trimmed().isEmpty()) return fail(tr("%1 has no expression").arg(r.name));
        recorded << lower;
        outputs << r.name + QLatin1Char('=') + oneToken(r.expression);
    }
    const bool op = firstWord(analysis) == QLatin1String("op");
    if (!nodes.isNull()) {
        // The voltages and currents too: after an op they are the result;
        // after an ac, dc or tran their last value, and every analysis but
        // op needs something to record. ngspice records 256 at most.
        // Other analyses (noise, sp, pz, ...) have no voltages to read.
        if (op || waveformAnalysis(analysis))
            for (const QString& n : nodes.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts))
                if (outputs.size() < 256 && !outputs.contains(n)) outputs << n;
        if (outputs.isEmpty() && !op)
            return fail(op || waveformAnalysis(analysis)
                            ? tr("nothing to record: label a wire (its voltage is recorded) or add a value")
                            : tr("nothing to record: add a value (after this analysis there are no voltages "
                                 "to keep)"));
    }
    if (!outputs.isEmpty()) parts << QStringLiteral("-output") << outputs;
    *line = parts.join(QLatin1Char(' '));
    return true;
}

bool commandLine(const Component* component, const Schematic* schematic, const QString& nodes, QString* line,
                 QString* error)
{
    if (!isSweep(component)) {
        if (error != nullptr) *error = tr("not a sweep component");
        return false;
    }
    return commandLine(Sweep::read(component), schematic, nodes, line, error);
}

QString valuesFile(const QString& component)
{
    return QStringLiteral("spice4qucs.%1.ngsweep").arg(component.toLower());
}

QString waveformsFile(const QString& component)
{
    return QStringLiteral("spice4qucs.%1.ngswaves").arg(component.toLower());
}

QString controlBlock(const Component* component, const Schematic* schematic, const QString& nodes,
                     QStringList* outputs, QString* error)
{
    QString line;
    if (!commandLine(component, schematic, nodes, &line, error)) return QString();
    const Sweep sweep = Sweep::read(component);
    const QString analysis = ngopt::analysisCommand(schematic, sweep.analysis);
    QList<QVector<double>> values;
    allValues(allKnobs(sweep), &values);
    const int runs = runCount(values);

    QString s;
    s += QStringLiteral("echo \"qucs-s: begin %1\"\n").arg(component->Name);
    // Only the runs' plots before the sweep's own: walking back from it
    // reaches the first run.
    s += QStringLiteral("destroy all\n");
    s += QStringLiteral("set filetype=ascii\n");
    // A transient on its step, so every run has the same points and they
    // make one family.
    if (firstWord(analysis) == QLatin1String("tran")) s += QStringLiteral("option interp\n");
    s += line + QLatin1Char('\n');
    // The sweep's plot is the current one: the values against the knobs.
    const QString file = valuesFile(component->Name);
    s += QStringLiteral("write %1\n").arg(file);
    *outputs << file;
    // Each run's own plot comes before it, in the runs' order.
    const QString vectors = nodes.simplified();
    if (sweep.waveforms && waveformAnalysis(analysis) && !vectors.isEmpty()) {
        const QString waves = waveformsFile(component->Name);
        s += QStringLiteral("repeat %1\nsetplot previous\nend\n").arg(runs);
        s += QStringLiteral("write %1 %2\n").arg(waves, vectors);
        if (runs > 1)
            s += QStringLiteral("set appendwrite\n"
                                "repeat %1\n"
                                "setplot next\n"
                                "write %2 %3\n"
                                "end\n"
                                "unset appendwrite\n")
                     .arg(runs - 1)
                     .arg(waves, vectors);
        *outputs << waves;
    }
    s += QStringLiteral("echo \"qucs-s: end %1\"\n").arg(component->Name);
    return s;
}

bool isResultFile(const QString& file)
{
    return file.endsWith(QLatin1String(".ngsweep")) || file.endsWith(QLatin1String(".ngswaves"));
}

QString datasetBlocks(const QString& workdir, const QString& file, const Schematic* schematic)
{
    if (!file.endsWith(QLatin1String(".ngsweep"))) return QString();
    const QString prefix = file.section(QLatin1Char('.'), 1, 1);
    const Component* component = sweepNamed(schematic, prefix);
    if (component == nullptr) return QString();
    const Sweep sweep = Sweep::read(component);
    const QList<Knob> knobs = allKnobs(sweep);
    QList<QVector<double>> values;
    if (!allValues(knobs, &values)) return QString();
    const int runs = runCount(values);

    const QList<RawPlot> plots = ngstats::readRaw(QDir(workdir).filePath(file));
    const RawPlot* plot = nullptr;
    for (const RawPlot& p : plots)
        if (p.name == QLatin1String("Sweep")) plot = &p;
    if (plot == nullptr || plot->vectors.isEmpty()) return QString();   // the sweep did not run: the log says why

    QString out;
    QTextStream s(&out);
    QStringList knobVars;
    for (int j = 0; j < knobs.size(); ++j) {
        knobVars << datasetName(prefix, knobVariable(knobs.at(j).name));
        // The inner knob's values as ngspice swept them.
        const RawVector& scale = plot->vectors.first();
        const bool fromPlot = j == 0 && scale.length() == values.first().size() && scale.re.size() >= scale.length();
        indep(s, knobVars.last(), fromPlot ? scale.re.mid(0, scale.length()) : values.at(j));
    }

    // Every run's voltages and currents, as families.
    const QString analysis = ngopt::analysisCommand(schematic, sweep.analysis);
    const bool waves = sweep.waveforms && waveformAnalysis(analysis)
                       && waveformBlocks(s, QDir(workdir).filePath(waveformsFile(prefix)), prefix, knobVars, runs);

    // The recorded values: one vector per output, or with outer knobs one
    // per output and combination of theirs, named after it.
    const int inner = int(values.first().size());
    const int combinations = runs / std::max(1, inner);
    QStringList suffixes;
    for (int c = 0; c < combinations; ++c) suffixes << familySuffix(knobs, values, c);
    QStringList bases;
    QHash<QString, QVector<const RawVector*>> families;
    for (int i = 1; i < plot->vectors.size(); ++i) {
        const RawVector& v = plot->vectors.at(i);
        if (v.length() != inner || v.re.size() < inner) continue;
        int best = -1;
        for (int c = 0; c < combinations; ++c)
            if (v.name.endsWith(suffixes.at(c), Qt::CaseInsensitive)
                && (best < 0 || suffixes.at(c).size() > suffixes.at(best).size()))
                best = c;
        if (best < 0 || v.name.size() == suffixes.at(best).size()) continue;
        const QString base = v.name.left(v.name.size() - suffixes.at(best).size());
        if (!families.contains(base)) {
            families.insert(base, QVector<const RawVector*>(combinations, nullptr));
            bases << base;
        }
        families[base][best] = &v;
    }
    QSet<QString> recorded;
    for (const Record& r : sweep.records) recorded << r.name.toLower();
    for (const QString& base : bases) {
        // With the waveforms there, the voltages' last values would only
        // take their names.
        if (waves && !recorded.contains(base.toLower())) continue;
        const QVector<const RawVector*>& each = families.value(base);
        if (each.contains(nullptr)) continue;
        s << "<dep " << datasetName(prefix, base) << ' ' << knobVars.join(QLatin1Char(' ')) << ">\n";
        for (const RawVector* v : each)
            for (int i = 0; i < inner; ++i) s << value(*v, i) << '\n';
        s << "</dep>\n";
    }
    return out;
}

ngstats::Summary summarize(const Component* component, const Schematic* schematic, const QString& output,
                           const QString& workdir)
{
    ngstats::Summary summary;
    const QStringList lines = block(output, component->Name);
    if (lines.isEmpty()) {
        summary.text = tr("%1: ngspice reported nothing. Please check log.").arg(component->Name);
        summary.warning = true;
        return summary;
    }
    QStringList text;
    bool ran = false;
    for (const QString& l : lines) {
        const QString t = l.simplified();
        if (t.isEmpty() || t.startsWith(QLatin1String("Warning from checkvalid"))) continue;
        if (t.startsWith(QLatin1String("sweep: "))) {
            const QString what = t.mid(7);
            // "r1 (instance/device) over 5 points, analysis 'ac ...'", or
            // with outer knobs "... = 6 runs -> 2 curves per output, ...".
            if (what.contains(QLatin1String(" point")) && what.contains(QLatin1String(", analysis '"))
                && !what.contains(QLatin1String("into plot"))) {
                text << what;
                ran = true;
                continue;
            }
            if (what.contains(QLatin1String("into plot")) || what.startsWith(QLatin1String("fast "))
                || what.startsWith(QLatin1String("setup reused")))
                continue;
            text << what;
            summary.warning = true;
            continue;
        }
        if (t.startsWith(QLatin1String("Error")) || t.startsWith(QLatin1String("Warning: sweep"))
            || t.contains(QLatin1String("error:"), Qt::CaseInsensitive)) {
            text << t;
            summary.warning = true;
        }
    }
    if (!ran) {
        text << tr("ngspice ran no sweep. Please check log.");
        summary.warning = true;
    } else {
        // The waveforms, when they were asked for and there are none.
        const Sweep sweep = Sweep::read(component);
        const QString analysis = ngopt::analysisCommand(schematic, sweep.analysis);
        const QString waves = QDir(workdir).filePath(waveformsFile(component->Name));
        QList<QVector<double>> values;
        if (sweep.waveforms && waveformAnalysis(analysis) && QFileInfo::exists(waves) && allValues(allKnobs(sweep), &values)) {
            const int runs = runCount(values);
            const int kept = int(ngstats::readRaw(waves).size());
            if (kept != runs) {
                text << tr("the voltages and currents of %1 of %2 runs could not be read (a run that did not "
                           "solve?): no families of curves, their last values instead")
                            .arg(runs - kept)
                            .arg(runs);
                summary.warning = true;
            }
        }
    }
    summary.text = component->Name + QStringLiteral(": ") + text.join(QLatin1Char('\n'));
    return summary;
}

bool unsupported(const QString& output)
{
    return output.contains(QLatin1String("sweep: no such command"));
}

QString withoutKnobProbes(const QString& output, const Schematic* schematic, const QStringList& sweeps)
{
    QSet<QString> probes;
    for (const QString& name : sweeps) {
        const Component* c = sweepNamed(schematic, name.toLower());
        if (c == nullptr) continue;
        for (const Knob& k : allKnobs(Sweep::read(c)))
            probes << QStringLiteral("Warning from checkvalid: vector %1 is not available or has zero length.")
                          .arg(k.name.toLower());
    }
    if (probes.isEmpty()) return output;
    QStringList kept;
    for (const QString& line : output.split(QLatin1Char('\n')))
        if (!probes.contains(QString(line).remove(QLatin1Char('\r')).trimmed())) kept << line;
    return kept.join(QLatin1Char('\n'));
}

} // namespace qucs_s::ngsweep
