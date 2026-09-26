/*
 * dataset.h - a simulation's dataset read as numbers, and what is
 *             measured on its curves
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_DATASET_H
#define QUCS_DATASET_H

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

namespace qucs_s::dataset {

/// A variable of a dataset: an independent one (time, frequency, a swept
/// parameter) or one that depends on some, the one varying fastest first.
struct Variable {
    QString name;
    bool independent = false;
    QStringList dependencies;
    QVector<double> re;
    QVector<double> im;   // empty unless complex
    /// Written as complex numbers whose imaginary parts are all zero (a
    /// Nutmeg equation's db(...) is): read as real, sign and all - its
    /// magnitude would turn -52 dB into +52.
    bool writtenComplex = false;
    bool isComplex() const { return !im.isEmpty(); }
    int size() const { return int(re.size()); }
};

/// A dataset file of Qucs-S (name.dat, name.dat.ngspice, .dat.xyce,
/// .dat.spopus): its variables, values and all.
class Dataset
{
public:
    /// Reads \a path; false (and why in \a error) when it cannot.
    bool read(const QString& path, QString* error = nullptr);
    QString path() const { return a_path; }
    const QList<Variable>& variables() const { return a_variables; }
    const Variable* find(const QString& name) const;
    /// The variables \a wanted may mean: itself; the same name in another
    /// case; the name without the analysis the simulator put before it
    /// ("v(out)" for tran.v(out) and ac.v(out)); a node's voltage ("out"
    /// for v(out), out.v, out.Vt). A simulator's prefix ("ngspice/") is
    /// left out. The first of these that finds any gives them all - but
    /// the operating point's v(out) comes with the analyses' v(out).
    QStringList resolve(const QString& wanted) const;

private:
    QString a_path;
    QList<Variable> a_variables;
    QHash<QString, int> a_index;
};

/// A value of an operating point: an independent variable of one value
/// that nothing depends on - a node voltage or branch current an op
/// analysis printed (v(out), i(v1)), a device's quantity (@jt1[id]).
bool isOperatingPointValue(const Dataset& data, const Variable& v);

/// "ngspice/tran.v(out)" is tran.v(out) in the dataset of ngspice; the
/// simulator ("ngspice", "xyce", "spopus") in \a simulator.
QString withoutSimulator(const QString& name, QString* simulator = nullptr);
/// The analysis a name begins with ("tran" of tran.v(out)), or empty.
QString analysisOf(const QString& name);
/// The unit of a variable, from its name or \a definition (the equation
/// that makes it, if any): "dB" for db(...), vdb(...), 20*log10(...);
/// degrees for a phase; V, A, s, Hz; empty when it cannot be told.
QString unitOf(const QString& name, const QString& definition = QString());
/// Whether a unit is a logarithmic one (dB).
inline bool isDecibels(const QString& unit) { return unit == QLatin1String("dB"); }
/// The name without its analysis ("v(out)" of tran.v(out)).
QString bareName(const QString& name);

/// A curve: y over x, one sweep of the fastest independent variable -
/// real values, or what \a form made of complex ones.
struct Curve {
    QVector<double> x;
    QVector<double> y;
};

/// How complex values are given: magnitude and phase (degrees), dB and
/// phase, or real and imaginary parts.
enum class Form { MagnitudePhase, DbPhase, RealImaginary };

/// The curves of \a v: one for each value (or combination of values) of
/// the independent variables after the first, whose values are in
/// \a outer; y is the real value, the magnitude of a complex one. A
/// variable that depends on nothing is one curve over its index.
QList<Curve> curvesOf(const Dataset& data, const Variable& v, QList<QList<QPair<QString, double>>>* outer = nullptr);
/// The phase (degrees) of \a v on the same curves (empty for a real one).
QList<QVector<double>> phasesOf(const Dataset& data, const Variable& v);
/// The part of \a c with x from \a from to \a to (either may be NaN: no
/// limit).
Curve within(const Curve& c, double from, double to);

/// y at \a x, straight between the samples; NaN outside the curve.
double valueAt(const Curve& c, double x);

struct Crossing {
    double x;
    int direction;   // +1 rising, -1 falling
};
/// Where the curve crosses \a level.
QList<Crossing> crossings(const Curve& c, double level);

struct Stats {
    double min = 0, xMin = 0, max = 0, xMax = 0;
    double mean = 0, rms = 0;   // weighted by x when it rises, as a time average
    double first = 0, last = 0;
    int count = 0;
};
Stats statsOf(const Curve& c);

/// The measurements measure() knows.
QStringList measurements();

struct MeasureOptions {
    double level = qQNaN();       // crossings, period: the level (else the middle of min and max)
    double tolerance = 0.02;      // settling: the band, a fraction of the step
    double low = 0.1, high = 0.9; // rise and fall: the fractions of the swing
    bool decibels = false;        // bandwidth: the curve is in dB (3 dB below its peak, not 1/sqrt(2) of it)
};
/// \a what measured on \a c - "rise_time", "fall_time", "overshoot",
/// "settling_time", "period", "frequency", "duty_cycle", "crossings",
/// "bandwidth" - as an object of numbers and what they mean; an object
/// with "error" when it cannot be measured on this curve. The bandwidth
/// of a magnitude is where it falls to 1/sqrt(2) of its peak; of a curve
/// in dB (options.decibels), 3 dB below it; of a real curve that goes
/// below 0 and is not in dB, it cannot be told.
QJsonObject measure(const Curve& c, const QString& what, const MeasureOptions& options);

/// \a v written with 7 significant digits (as the dataset has more than
/// anyone reads): for JSON, where a double takes up to 17.
double rounded(double v);

} // namespace qucs_s::dataset

#endif // QUCS_DATASET_H
