/*
 * ngstatistics.cpp - the NgMonteCarlo and NgCorners components' ngspice
 * commands (see ngstatistics.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "ngstatistics.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>

#include <algorithm>
#include <cmath>

#include "components/component.h"
#include "ngoptimize.h"
#include "schematic.h"
#include "valuereading.h"

namespace qucs_s::ngstats {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("NgStatistics", text);
}

const char* const kMonteCarloFixed[] = {"Samples", "Seed", "LHS", "ModelStats", "Analysis"};
const char* const kCornersFixed[] = {"Analysis", "Corners", "Nominal", "Waveforms", "Samples", "Seed", "ModelStats"};

bool yes(const QString& value)
{
    return value.trimmed() == QLatin1String("yes");
}

QString yesNo(bool value)
{
    return value ? QStringLiteral("yes") : QStringLiteral("no");
}

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

// An expression as one word: ngspice takes -expr, -output and -spec
// arguments as single words.
QString oneToken(const QString& expression)
{
    QString s = expression;
    s.remove(QRegularExpression(QStringLiteral("\\s+")));
    return s;
}

QString quoted(const QString& command)
{
    QString s = command.simplified();
    s.remove(QLatin1Char('"'));
    return QLatin1Char('"') + s + QLatin1Char('"');
}

bool validName(const QString& name)
{
    static const QRegularExpression rx(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
    return rx.match(name).hasMatch();
}

QString firstWord(const QString& command)
{
    return command.trimmed().section(QLatin1Char(' '), 0, 0).toLower();
}

// The analyses whose output is a waveform the corners can keep.
bool waveformAnalysis(const QString& command)
{
    const QString w = firstWord(command);
    return w == QLatin1String("ac") || w == QLatin1String("dc") || w == QLatin1String("tran");
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

void writeProperties(Component* component, const QStringList& names, const QStringList& values,
                     const QStringList& shownByDefault, const QList<Record>& records, const QList<Spec>& specs)
{
    Shown shown(component);
    QList<Property*> props;
    for (int i = 0; i < names.size(); ++i)
        props << new Property(names.at(i), values.at(i), shown(names.at(i), shownByDefault.contains(names.at(i))),
                              QString());
    for (const Record& r : records)
        props << new Property(QStringLiteral("Record"), r.toString(), shown(QStringLiteral("Record"), true), QString());
    for (const Spec& s : specs)
        props << new Property(QStringLiteral("Spec"), s.toString(), shown(QStringLiteral("Spec"), true), QString());
    qDeleteAll(component->Props);
    component->Props = props;
}

// The -spec arguments of \a specs; false and why when one cannot be
// written.
bool specArguments(const QList<Spec>& specs, QStringList* parts, QString* why)
{
    for (const Spec& s : specs) {
        if (s.expression.trimmed().isEmpty()) {
            *why = tr("a spec has no expression");
            return false;
        }
        QString lo, hi;
        if (!s.min.trimmed().isEmpty() && !spiceNumber(s.min, &lo)) {
            *why = tr("%1: the minimum \"%2\" is not a number").arg(s.expression, s.min);
            return false;
        }
        if (!s.max.trimmed().isEmpty() && !spiceNumber(s.max, &hi)) {
            *why = tr("%1: the maximum \"%2\" is not a number").arg(s.expression, s.max);
            return false;
        }
        if (lo.isEmpty() && hi.isEmpty()) {
            *why = tr("%1: a spec needs a minimum, a maximum or both").arg(s.expression);
            return false;
        }
        if (!lo.isEmpty() && !hi.isEmpty() && !(hi.toDouble() > lo.toDouble())) {
            *why = tr("%1: the maximum must be above the minimum").arg(s.expression);
            return false;
        }
        *parts << QStringLiteral("-spec") << oneToken(s.expression);
        if (!lo.isEmpty()) *parts << QStringLiteral("-min") << lo;
        if (!hi.isEmpty()) *parts << QStringLiteral("-max") << hi;
    }
    return true;
}

// The name a spec's metric is recorded under, for its distribution.
QString specName(int index)
{
    return QStringLiteral("spec%1").arg(index + 1);
}

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

// Whether a vector of length L is the analysis scale a family is drawn
// against: time, frequency, or a dc sweep's (v-sweep, temp-sweep, ...).
bool scaleName(const QString& name)
{
    return name == QLatin1String("time") || name == QLatin1String("frequency") || name.endsWith(QLatin1String("sweep"));
}

QString monteCarloBlocks(const QString& path, const QString& prefix)
{
    const QList<RawPlot> plots = readRaw(path);
    const RawPlot* plot = nullptr;
    for (const RawPlot& p : plots)
        if (p.name == QLatin1String("Monte Carlo")) {
            plot = &p;
            break;
        }
    if (plot == nullptr) return QString();
    const RawVector* sample = plot->vector(QStringLiteral("sample"));
    if (sample == nullptr || sample->length() == 0) return QString();
    const int n = sample->length();

    QString out;
    QTextStream s(&out);
    const QString sampleName = datasetName(prefix, QStringLiteral("sample"));
    indep(s, sampleName, sample->re.mid(0, n));

    // The scale of the families: a vector of the family's inner length.
    QSet<QString> written;
    for (const RawVector& v : plot->vectors) {
        if (v.dims.size() != 2 || v.dims.at(0) != n || v.length() > v.re.size()) continue;
        const int l = v.dims.at(1);
        const RawVector* scale = nullptr;
        for (const RawVector& c : plot->vectors)
            if (c.dims.size() == 1 && c.length() == l && scaleName(c.name)) scale = &c;
        if (scale == nullptr)
            for (const RawVector& c : plot->vectors)
                if (c.dims.size() == 1 && c.length() == l && l != n && c.name != QLatin1String("sample")) scale = &c;
        QString scaleVar;
        if (scale != nullptr) {
            scaleVar = datasetName(prefix, scale->name);
            if (!written.contains(scaleVar)) indep(s, scaleVar, scale->re.mid(0, l));
        } else {
            scaleVar = datasetName(prefix, v.name + QStringLiteral("_index"));
            QVector<double> index(l);
            for (int i = 0; i < l; ++i) index[i] = i;
            indep(s, scaleVar, index);
        }
        written << scaleVar;
        // ngspice keeps sample k's L points together: the scale varies
        // fastest, as the dataset wants its first dependency to.
        s << "<dep " << datasetName(prefix, v.name) << ' ' << scaleVar << ' ' << sampleName << ">\n";
        for (int i = 0; i < n * l; ++i) s << value(v, i) << '\n';
        s << "</dep>\n";
    }

    for (const RawVector& v : plot->vectors) {
        if (v.dims.size() != 1 || v.name == QLatin1String("sample") || v.length() > v.re.size()) continue;
        if (v.name.startsWith(QLatin1String("montecarlo_"))) {
            static const QStringList kept = {"yield", "npass", "nvalid", "nfailed"};
            const QString what = v.name.mid(int(qstrlen("montecarlo_")));
            if (!kept.contains(what) || v.length() != 1) continue;
            indep(s, datasetName(prefix, what), v.re.mid(0, 1));
            continue;
        }
        if (v.length() != n || written.contains(datasetName(prefix, v.name))) continue;
        s << "<dep " << datasetName(prefix, v.name) << ' ' << sampleName << ">\n";
        for (int i = 0; i < n; ++i) s << value(v, i) << '\n';
        s << "</dep>\n";
    }
    return out;
}

QString cornersBlocks(const QString& path, const QString& wavesPath, const QString& prefix)
{
    const QList<RawPlot> plots = readRaw(path);
    const RawPlot* plot = nullptr;
    for (const RawPlot& p : plots)
        if (p.name == QLatin1String("Corners")) {
            plot = &p;
            break;
        }
    if (plot == nullptr) return QString();
    const RawVector* corner = plot->vector(QStringLiteral("corner"));
    if (corner == nullptr || corner->length() == 0 || corner->length() > corner->re.size()) return QString();
    const int k = corner->length();

    QString out;
    QTextStream s(&out);
    const QString cornerName = datasetName(prefix, QStringLiteral("corner"));
    indep(s, cornerName, corner->re.mid(0, k));
    for (const RawVector& v : plot->vectors) {
        if (v.name == QLatin1String("corner") || v.name == QLatin1String("corners_n")) continue;
        if (v.dims.size() != 1 || v.length() != k || v.length() > v.re.size()) continue;
        s << "<dep " << datasetName(prefix, v.name) << ' ' << cornerName << ">\n";
        for (int i = 0; i < k; ++i) s << value(v, i) << '\n';
        s << "</dep>\n";
    }

    // The waveforms: a plot per corner, in the corners' order, all on one
    // scale (a transient's is interpolated onto its step).
    const QList<RawPlot> waves = readRaw(wavesPath);
    if (waves.size() != k || waves.first().vectors.isEmpty()) return out;
    const RawVector& scale = waves.first().vectors.first();
    const int l = scale.length();
    for (const RawPlot& w : waves)
        if (w.vectors.isEmpty() || w.vectors.first().name != scale.name || w.vectors.first().length() != l
            || w.vectors.first().re.size() < l)
            return out;
    const QString scaleVar = datasetName(prefix, scale.name);
    indep(s, scaleVar, scale.re.mid(0, l));
    for (int i = 1; i < waves.first().vectors.size(); ++i) {
        const QString name = waves.first().vectors.at(i).name;
        QList<const RawVector*> each;
        for (const RawPlot& w : waves) {
            const RawVector* v = w.vector(name);
            if (v == nullptr || v->length() != l || v->re.size() < l) break;
            each << v;
        }
        if (each.size() != k) continue;
        s << "<dep " << datasetName(prefix, name) << ' ' << scaleVar << ' ' << cornerName << ">\n";
        for (const RawVector* v : each)
            for (int j = 0; j < l; ++j) s << value(*v, j) << '\n';
        s << "</dep>\n";
    }
    return out;
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

bool errorLine(const QString& line)
{
    const QString t = line.trimmed();
    return t.startsWith(QLatin1String("Error")) || t.contains(QLatin1String("error:"), Qt::CaseInsensitive)
           || t.contains(QLatin1String("refused"));
}

} // namespace

// ---------------------------------------------------------------------------

bool Record::parse(const QString& value, Record* record)
{
    const int bar = int(value.indexOf(QLatin1Char('|')));
    if (bar < 0) return false;
    record->name = value.left(bar).trimmed();
    record->expression = value.mid(bar + 1).trimmed();
    return true;
}

QString Record::toString() const
{
    return name + QLatin1Char('|') + expression;
}

bool Spec::parse(const QString& value, Spec* spec)
{
    const QStringList f = value.split(QLatin1Char('|'));
    if (f.size() < 3) return false;
    spec->expression = f.at(0).trimmed();
    spec->min = f.at(1).trimmed();
    spec->max = f.at(2).trimmed();
    return true;
}

QString Spec::toString() const
{
    return QStringList({expression, min, max}).join(QLatin1Char('|'));
}

MonteCarlo MonteCarlo::read(const Component* component)
{
    MonteCarlo c;
    if (component == nullptr) return c;
    for (const Property* p : component->Props) {
        const QString v = p->Value.trimmed();
        if (p->Name == QLatin1String("Samples")) c.samples = v;
        else if (p->Name == QLatin1String("Seed")) c.seed = v;
        else if (p->Name == QLatin1String("LHS")) c.lhs = yes(v);
        else if (p->Name == QLatin1String("ModelStats")) c.modelStatistics = yes(v);
        else if (p->Name == QLatin1String("Analysis")) c.analysis = v;
        else if (p->Name == QLatin1String("Record")) {
            Record r;
            if (Record::parse(v, &r)) c.records << r;
        } else if (p->Name == QLatin1String("Spec")) {
            Spec s;
            if (Spec::parse(v, &s)) c.specs << s;
        }
    }
    return c;
}

void MonteCarlo::write(Component* component) const
{
    QStringList names;
    for (const char* n : kMonteCarloFixed) names << QString::fromLatin1(n);
    writeProperties(component, names, {samples, seed, yesNo(lhs), yesNo(modelStatistics), analysis},
                    {QStringLiteral("Samples"), QStringLiteral("Analysis")}, records, specs);
}

bool Corners::monteCarlo() const
{
    qint64 n = 0;
    return wholeNumber(samples, 1, &n);
}

Corners Corners::read(const Component* component)
{
    Corners c;
    if (component == nullptr) return c;
    for (const Property* p : component->Props) {
        const QString v = p->Value.trimmed();
        if (p->Name == QLatin1String("Analysis")) c.analysis = v;
        else if (p->Name == QLatin1String("Corners")) c.corners = v;
        else if (p->Name == QLatin1String("Nominal")) c.nominal = yes(v);
        else if (p->Name == QLatin1String("Waveforms")) c.waveforms = yes(v);
        else if (p->Name == QLatin1String("Samples")) c.samples = v;
        else if (p->Name == QLatin1String("Seed")) c.seed = v;
        else if (p->Name == QLatin1String("ModelStats")) c.modelStatistics = yes(v);
        else if (p->Name == QLatin1String("Record")) {
            Record r;
            if (Record::parse(v, &r)) c.records << r;
        } else if (p->Name == QLatin1String("Spec")) {
            Spec s;
            if (Spec::parse(v, &s)) c.specs << s;
        }
    }
    return c;
}

void Corners::write(Component* component) const
{
    QStringList names;
    for (const char* n : kCornersFixed) names << QString::fromLatin1(n);
    writeProperties(component, names,
                    {analysis, corners, yesNo(nominal), yesNo(waveforms), samples, seed, yesNo(modelStatistics)},
                    {QStringLiteral("Analysis"), QStringLiteral("Corners")}, records, specs);
}

bool isStatistics(const Component* component)
{
    return component != nullptr
           && (component->Model == QLatin1String(kMonteCarloModel) || component->Model == QLatin1String(kCornersModel));
}

QString analysisCommand(const Schematic* schematic, const QString& analysis)
{
    return ngopt::analysisCommand(schematic, analysis);
}

bool commandLine(const MonteCarlo& command, const Schematic* schematic, QString* line, QString* error)
{
    auto fail = [&](const QString& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    qint64 samples = 0;
    if (!wholeNumber(command.samples, 1, &samples))
        return fail(tr("the samples \"%1\" are not a whole number above 0").arg(command.samples));
    const QString analysis = analysisCommand(schematic, command.analysis);
    if (analysis.isEmpty()) return fail(tr("no analysis to run"));
    if (command.records.isEmpty() && command.specs.isEmpty()) return fail(tr("nothing to record and no spec to judge"));

    QStringList parts{QStringLiteral("montecarlo"), QString::number(samples)};
    if (command.lhs) parts << QStringLiteral("-lhs");
    if (!command.seed.trimmed().isEmpty()) {
        qint64 seed = 0;
        if (!wholeNumber(command.seed, 0, &seed)) return fail(tr("the seed \"%1\" is not a whole number").arg(command.seed));
        parts << QStringLiteral("-seed") << QString::number(seed);
    }
    parts << QStringLiteral("-analysis") << quoted(analysis);
    QString why;
    if (!specArguments(command.specs, &parts, &why)) return fail(why);

    QSet<QString> names;
    for (const Record& r : command.records) {
        if (!validName(r.name)) return fail(tr("the name \"%1\" is not a name ngspice takes (letters, digits, _)").arg(r.name));
        const QString lower = r.name.toLower();
        if (lower == QLatin1String("sample") || lower.startsWith(QLatin1String("montecarlo_"))
            || QRegularExpression(QStringLiteral("^spec[0-9]+$")).match(lower).hasMatch())
            return fail(tr("the name %1 is taken by the results").arg(r.name));
        if (names.contains(lower)) return fail(tr("the name %1 is given twice").arg(r.name));
        if (r.expression.trimmed().isEmpty()) return fail(tr("%1 has no expression").arg(r.name));
        names << lower;
        parts << QStringLiteral("-expr") << r.name + QLatin1Char('=') + oneToken(r.expression);
    }
    // Every spec's metric is recorded too, for its distribution.
    for (int i = 0; i < command.specs.size(); ++i)
        parts << QStringLiteral("-expr") << specName(i) + QLatin1Char('=') + oneToken(command.specs.at(i).expression);
    *line = parts.join(QLatin1Char(' '));
    return true;
}

bool commandLine(const Corners& command, const Schematic* schematic, QString* line, QString* error)
{
    auto fail = [&](const QString& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    const QString analysis = analysisCommand(schematic, command.analysis);
    if (analysis.isEmpty()) return fail(tr("no analysis to run"));

    QStringList parts{QStringLiteral("corners")};
    const QStringList list = command.corners.split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts);
    for (const QString& c : list)
        if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9_]+$")).match(c).hasMatch())
            return fail(tr("\"%1\" is not a corner's name").arg(c));
    if (!list.isEmpty()) parts << QStringLiteral("-list") << list.join(QLatin1Char(','));
    if (!command.nominal) parts << QStringLiteral("-nonominal");

    if (command.monteCarlo()) {
        qint64 samples = 0;
        wholeNumber(command.samples, 1, &samples);
        if (command.specs.isEmpty()) return fail(tr("a Monte Carlo at every corner needs a spec to judge"));
        parts << QStringLiteral("-mc") << QString::number(samples);
        if (!command.seed.trimmed().isEmpty()) {
            qint64 seed = 0;
            if (!wholeNumber(command.seed, 0, &seed))
                return fail(tr("the seed \"%1\" is not a whole number").arg(command.seed));
            parts << QStringLiteral("-seed") << QString::number(seed);
        }
        parts << QStringLiteral("-analysis") << quoted(analysis);
        QString why;
        if (!specArguments(command.specs, &parts, &why)) return fail(why);
    } else {
        if (!command.samples.trimmed().isEmpty() && command.samples.trimmed() != QLatin1String("0"))
            return fail(tr("the samples \"%1\" are not a whole number").arg(command.samples));
        parts << QStringLiteral("-analysis") << quoted(analysis);
        const bool waves = command.waveforms && waveformAnalysis(analysis);
        if (command.records.isEmpty() && !waves)
            return fail(tr("nothing to record: add a value, or keep the waveforms of an ac, dc or tran analysis"));
        QSet<QString> names;
        QStringList outputs;
        for (const Record& r : command.records) {
            if (!validName(r.name))
                return fail(tr("the name \"%1\" is not a name ngspice takes (letters, digits, _)").arg(r.name));
            const QString lower = r.name.toLower();
            if (lower == QLatin1String("corner") || lower == QLatin1String("corners_n"))
                return fail(tr("the name %1 is taken by the results").arg(r.name));
            if (names.contains(lower)) return fail(tr("the name %1 is given twice").arg(r.name));
            if (r.expression.trimmed().isEmpty()) return fail(tr("%1 has no expression").arg(r.name));
            names << lower;
            outputs << r.name + QLatin1Char('=') + oneToken(r.expression);
        }
        if (!outputs.isEmpty()) parts << QStringLiteral("-output") << outputs;
    }
    *line = parts.join(QLatin1Char(' '));
    return true;
}

bool commandLine(const Component* component, const Schematic* schematic, QString* line, QString* error)
{
    if (component != nullptr && component->Model == QLatin1String(kMonteCarloModel))
        return commandLine(MonteCarlo::read(component), schematic, line, error);
    if (component != nullptr && component->Model == QLatin1String(kCornersModel))
        return commandLine(Corners::read(component), schematic, line, error);
    if (error != nullptr) *error = tr("not a Monte Carlo or corners component");
    return false;
}

QString monteCarloFile(const QString& component)
{
    return QStringLiteral("spice4qucs.%1.ngmc").arg(component.toLower());
}

QString cornersFile(const QString& component)
{
    return QStringLiteral("spice4qucs.%1.ngcorners").arg(component.toLower());
}

QString waveformsFile(const QString& component)
{
    return QStringLiteral("spice4qucs.%1.ngcwaves").arg(component.toLower());
}

QString controlBlock(const Component* component, const Schematic* schematic, const QString& nodes,
                     QStringList* outputs, QString* error)
{
    QString line;
    if (!commandLine(component, schematic, &line, error)) return QString();
    const bool corners = component->Model == QLatin1String(kCornersModel);
    const MonteCarlo mc = corners ? MonteCarlo() : MonteCarlo::read(component);
    const Corners cr = corners ? Corners::read(component) : Corners();
    const QString analysis = analysisCommand(schematic, corners ? cr.analysis : mc.analysis);
    const bool statistics = corners ? cr.monteCarlo() && cr.modelStatistics : mc.modelStatistics;

    QString s;
    s += QStringLiteral("echo \"qucs-s: begin %1\"\n").arg(component->Name);
    s += QStringLiteral("set filetype=ascii\n");
    // A transient on its step, so every sample or corner has the same
    // points and they make one family.
    if (firstWord(analysis) == QLatin1String("tran")) s += QStringLiteral("option interp\n");
    if (statistics) s += QStringLiteral("option osdimc\n");
    s += line + QLatin1Char('\n');
    if (!corners) {
        const QString file = monteCarloFile(component->Name);
        s += QStringLiteral("setplot $montecarlo_plot\nwrite %1\n").arg(file);
        *outputs << file;
    } else {
        const QString file = cornersFile(component->Name);
        s += QStringLiteral("setplot $corners_plot\nwrite %1\n").arg(file);
        *outputs << file;
        // Each corner's own analysis plot comes before the corners plot,
        // in the corners' order.
        const QString vectors = nodes.simplified();
        if (cr.waveforms && !cr.monteCarlo() && waveformAnalysis(analysis) && !vectors.isEmpty()) {
            const QString waves = waveformsFile(component->Name);
            s += QStringLiteral("repeat $corners_n\nsetplot previous\nend\n");
            s += QStringLiteral("write %1 %2\n").arg(waves, vectors);
            // At the top level: ngspice reads the count of a repeat inside
            // an if before the if runs. With one corner it repeats 0 times.
            s += QStringLiteral("let qucs_corners_more = $corners_n - 1\n"
                                "set appendwrite\n"
                                "repeat $&qucs_corners_more\n"
                                "setplot next\n"
                                "write %1 %2\n"
                                "end\n"
                                "unset appendwrite\n")
                     .arg(waves, vectors);
            *outputs << waves;
        }
    }
    if (statistics) s += QStringLiteral("option noosdimc\n");
    s += QStringLiteral("echo \"qucs-s: end %1\"\n").arg(component->Name);
    return s;
}

// ---- the raw file ---------------------------------------------------------

int RawVector::length() const
{
    int n = 1;
    for (int d : dims) n *= d;
    return dims.isEmpty() ? 0 : n;
}

const RawVector* RawPlot::vector(const QString& name) const
{
    for (const RawVector& v : vectors)
        if (v.name == name) return &v;
    return nullptr;
}

QList<RawPlot> readRaw(const QString& file)
{
    QList<RawPlot> plots;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return plots;
    QTextStream in(&f);

    enum { Header, Variables, Values } state = Header;
    int declared = 0;     // No. Variables
    int points = 0;       // No. Points
    QList<int> plotDims;  // Dimensions:
    bool padded = true;
    // The values: point by point, the vectors still within their length.
    int point = 0;
    int next = 0;         // the vector whose value comes next; -1: the point's index
    auto finishHeader = [&]() {
        RawPlot& p = plots.last();
        for (RawVector& v : p.vectors)
            if (v.dims.isEmpty()) v.dims = plotDims.isEmpty() ? QList<int>{points} : plotDims;
    };
    // The next vector that has a value at this point, from \a from on.
    auto nextVector = [&](int from) {
        const RawPlot& p = plots.last();
        for (int i = from; i < p.vectors.size(); ++i)
            if (padded || point < p.vectors.at(i).length()) return i;
        return int(p.vectors.size());
    };
    auto store = [&](const QString& token) {
        RawPlot& p = plots.last();
        if (next >= p.vectors.size()) return;
        RawVector& v = p.vectors[next];
        double re = 0, im = 0;
        bool ok = false;
        if (p.complex) {
            const int comma = int(token.indexOf(QLatin1Char(',')));
            re = token.left(comma).toDouble(&ok);
            if (ok && comma >= 0) im = token.mid(comma + 1).toDouble(&ok);
        } else {
            re = token.toDouble(&ok);
        }
        if (!ok) re = im = std::nan("");
        if (point < v.length()) {
            v.re << re;
            if (p.complex) v.im << im;
        }
        next = nextVector(next + 1);
    };

    while (!in.atEnd()) {
        const QString line = in.readLine();
        if (line.startsWith(QLatin1String("Title:"))) {
            plots << RawPlot();
            plots.last().title = line.mid(6).trimmed();
            state = Header;
            declared = points = 0;
            plotDims.clear();
            padded = true;
            continue;
        }
        if (plots.isEmpty()) continue;
        RawPlot& plot = plots.last();
        if (state == Header) {
            if (line.startsWith(QLatin1String("Plotname:"))) plot.name = line.mid(9).trimmed();
            else if (line.startsWith(QLatin1String("Flags:"))) {
                plot.complex = line.contains(QLatin1String("complex"));
                padded = !line.contains(QLatin1String("unpadded"));
            } else if (line.startsWith(QLatin1String("No. Variables:"))) declared = line.mid(14).trimmed().toInt();
            else if (line.startsWith(QLatin1String("No. Points:"))) points = line.mid(11).trimmed().toInt();
            else if (line.startsWith(QLatin1String("Dimensions:"))) {
                for (const QString& d : line.mid(11).trimmed().split(QLatin1Char(','))) plotDims << d.toInt();
            } else if (line.startsWith(QLatin1String("Variables:"))) state = Variables;
            else if (line.startsWith(QLatin1String("Binary:"))) {
                plots.removeLast();   // only the ASCII form is written for these results
                break;
            }
            continue;
        }
        if (state == Variables) {
            if (line.startsWith(QLatin1String("Values:"))) {
                finishHeader();
                state = Values;
                point = 0;
                next = -1;
                continue;
            }
            const QStringList f = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            if (f.size() < 3) continue;
            RawVector v;
            v.name = f.at(1);
            v.type = f.at(2);
            for (int i = 3; i < f.size(); ++i)
                if (f.at(i).startsWith(QLatin1String("dims=")))
                    for (const QString& d : f.at(i).mid(5).split(QLatin1Char(','))) v.dims << d.toInt();
            plot.vectors << v;
            continue;
        }
        // Values: " <point>\t<value>" opens a point, "\t<value>" follows.
        for (const QString& token : line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts)) {
            if (next == -1) {
                point = token.toInt();
                next = nextVector(0);
                continue;
            }
            store(token);
            if (next >= plot.vectors.size()) next = -1;
        }
    }
    Q_UNUSED(declared);
    return plots;
}

bool isResultFile(const QString& file)
{
    return file.endsWith(QLatin1String(".ngmc")) || file.endsWith(QLatin1String(".ngcorners"))
           || file.endsWith(QLatin1String(".ngcwaves"));
}

QString datasetBlocks(const QString& workdir, const QString& file)
{
    const QString prefix = file.section(QLatin1Char('.'), 1, 1);
    const QString path = QDir(workdir).filePath(file);
    if (file.endsWith(QLatin1String(".ngmc"))) return monteCarloBlocks(path, prefix);
    if (file.endsWith(QLatin1String(".ngcorners")))
        return cornersBlocks(path, QDir(workdir).filePath(waveformsFile(prefix)), prefix);
    return QString();
}

Summary summarize(const Component* component, const QString& output, const QString& workdir)
{
    Summary summary;
    const bool corners = component->Model == QLatin1String(kCornersModel);
    const QStringList lines = block(output, component->Name);
    const QString command = corners ? QStringLiteral("corners: ") : QStringLiteral("montecarlo: ");
    if (lines.isEmpty()) {
        summary.text = tr("%1: ngspice reported nothing. Please check log.").arg(component->Name);
        summary.warning = true;
        return summary;
    }
    QStringList text;
    for (const QString& l : lines) {
        if (errorLine(l)) {
            text << l.trimmed();
            summary.warning = true;
        }
    }
    if (!corners) {
        // "montecarlo: 50 Latin-Hypercube samples, analysis 'op', 1 spec, seed 4"
        // and the indented report: yield, CI, violations, notes.
        bool report = false;
        for (const QString& l : lines) {
            if (l.startsWith(command) && l.contains(QLatin1String(" samples, analysis "))) {
                text << l.mid(command.size()).trimmed();
                report = false;
                continue;
            }
            if (l.startsWith(command) && l.contains(QLatin1String("recorded into plot"))) {
                report = true;
                continue;
            }
            // The seed the dialog explains; ngspice notes it on every run
            // without one.
            if (l.contains(QLatin1String("no -seed given"))) continue;
            if (l.startsWith(QLatin1String("  ")) && !l.trimmed().isEmpty()
                && (report || l.trimmed().startsWith(QLatin1String("yield")) || l.trimmed().startsWith(QLatin1String("NOTE")))) {
                text << l.simplified();
                report = true;
            }
        }
        const QList<RawPlot> plots = readRaw(QDir(workdir).filePath(monteCarloFile(component->Name)));
        for (const RawPlot& p : plots) {
            const RawVector* failed = p.vector(QStringLiteral("montecarlo_nfailed"));
            if (p.name == QLatin1String("Monte Carlo") && failed != nullptr && !failed->re.isEmpty()
                && failed->re.first() > 0) {
                text << tr("%1 samples did not solve").arg(failed->re.first());
                summary.warning = true;
            }
        }
    } else {
        // "corners: 4 corners (tt ss ff sf), analysis 'ac ...'" and the table.
        QStringList header;
        for (const QString& l : lines) {
            const QString t = l.simplified();
            if (l.startsWith(command) && l.contains(QLatin1String(" corners ("))) {
                text << l.mid(command.size()).trimmed();
                continue;
            }
            if (t.startsWith(QLatin1String("idx corner"))) {
                header = t.split(QLatin1Char(' ')).mid(2);
                continue;
            }
            if (header.isEmpty() || !l.startsWith(QLatin1String("  "))) {
                if (!l.startsWith(QLatin1String("  "))) header.clear();
                continue;
            }
            const QStringList f = t.split(QLatin1Char(' '));
            if (f.size() < 2) continue;
            QStringList row{f.at(1) + QLatin1Char(':')};
            for (int i = 0; i < header.size() && i + 2 < f.size(); ++i) row << header.at(i) + QLatin1Char('=') + f.at(i + 2);
            text << row.join(QLatin1Char(' '));
        }
    }
    if (text.isEmpty()) {
        text << tr("ngspice ran no %1. Please check log.").arg(corners ? QStringLiteral("corners") : QStringLiteral("montecarlo"));
        summary.warning = true;
    }
    summary.text = component->Name + QStringLiteral(": ") + text.join(QLatin1Char('\n'));
    return summary;
}

bool unsupported(const QString& output, bool corners)
{
    return output.contains(corners ? QLatin1String("corners: no such command")
                                   : QLatin1String("montecarlo: no such command"));
}

} // namespace qucs_s::ngstats
