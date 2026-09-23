/*
 * ngstatistics.h - the NgMonteCarlo and NgCorners components' ngspice
 * commands: `montecarlo` and `corners`, the statistical loops built into
 * ngspice builds that have them (the Ngspice_OpenVAF_Enhancements fork):
 * what the components store, the command lines and the .control lines
 * they become, the results read back into the dataset, and what the
 * status log says of them
 *
 *   montecarlo <N> [-lhs] [-seed <s>] -analysis "<command>"
 *              (-expr <name>=<expression>)... (-spec <metric> [-min <lo>] [-max <hi>])...
 *   corners [-list <c1>,<c2>...] [-nonominal] -analysis "<command>"
 *           ( -output <name>=<expression> ...
 *             | -mc <N> [-seed <s>] (-spec <metric> [-min <lo>] [-max <hi>])... )
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_NGSTATISTICS_H
#define QUCS_NGSTATISTICS_H

#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

class Component;
class Schematic;

namespace qucs_s::ngstats {

/// "Record=gain|db(v(out))": a value recorded for every sample or corner
/// under a name of its own.
struct Record {
    QString name;
    QString expression;

    static bool parse(const QString& value, Record* record);
    QString toString() const;
};

/// "Spec=v(out)|0.49|0.51": a metric and its limits (one of them may be
/// empty); a sample passes when every spec is within its limits.
struct Spec {
    QString expression;
    QString min;
    QString max;

    static bool parse(const QString& value, Spec* spec);
    QString toString() const;
};

/// What the NgMonteCarlo component stores.
struct MonteCarlo {
    QString samples = QStringLiteral("100");
    QString seed;                 ///< empty: ngspice's default (1)
    bool lhs = false;             ///< Latin hypercube sampling
    bool modelStatistics = false; ///< .option osdimc: the Verilog-A models' declared statistics
    QString analysis;             ///< a simulation component's name, or an ngspice analysis command
    QList<Record> records;
    QList<Spec> specs;

    static MonteCarlo read(const Component* component);
    void write(Component* component) const;
};

/// What the NgCorners component stores.
struct Corners {
    QString analysis;
    QString corners;              ///< "ss, ff": empty for every corner the models declare
    bool nominal = true;          ///< the nominal, tt, first
    bool waveforms = true;        ///< the analysis' voltages and currents at every corner
    QList<Record> records;
    QString samples;              ///< a Monte Carlo of this many samples at every corner; empty: none
    QString seed;
    bool modelStatistics = false;
    QList<Spec> specs;

    bool monteCarlo() const;
    static Corners read(const Component* component);
    void write(Component* component) const;
};

/// The component models.
inline constexpr const char* kMonteCarloModel = ".NGMONTECARLO";
inline constexpr const char* kCornersModel = ".NGCORNERS";
bool isStatistics(const Component* component);

/// The montecarlo line; false and why in \a error when it cannot be
/// written: no samples, a value that is not a number, nothing to record
/// or judge, a name ngspice cannot take.
bool commandLine(const MonteCarlo& command, const Schematic* schematic, QString* line, QString* error = nullptr);
/// The corners line, likewise.
bool commandLine(const Corners& command, const Schematic* schematic, QString* line, QString* error = nullptr);
/// Either, for the component.
bool commandLine(const Component* component, const Schematic* schematic, QString* line, QString* error = nullptr);

/// The files a component's results are written to, in the Scratch folder.
QString monteCarloFile(const QString& component);
QString cornersFile(const QString& component);
QString waveformsFile(const QString& component);

/// The .control lines of \a component: the command between two markers
/// in the output, and the writes of its results. \a nodes are the
/// voltages and currents the simulations save ("v(out) i(v1)"); the
/// files written are appended to \a outputs. Empty, and why in \a error,
/// when the command cannot be written.
QString controlBlock(const Component* component, const Schematic* schematic, const QString& nodes,
                     QStringList* outputs, QString* error = nullptr);

/// One plot of an ngspice raw file (ASCII): a vector per variable, with
/// its dimensions (a Monte Carlo family is N x L).
struct RawVector {
    QString name;
    QString type;
    QList<int> dims;
    QVector<double> re;
    QVector<double> im;   ///< empty unless the plot is complex

    int length() const;
};
struct RawPlot {
    QString title;
    QString name;         ///< "Monte Carlo", "Corners", "AC Analysis", ...
    bool complex = false;
    QList<RawVector> vectors;

    const RawVector* vector(const QString& name) const;
};
/// Every plot of \a file, in order; empty if it cannot be read.
QList<RawPlot> readRaw(const QString& file);

/// Whether \a file (a name of the outputs list) is one of these results.
bool isResultFile(const QString& file);
/// The Qucs dataset blocks of the result \a file in \a workdir: every
/// variable under the component's name ("ngmontecarlo1.gain"). Empty for
/// a file that holds none (the waveforms of corners come with
/// the corners' own file).
QString datasetBlocks(const QString& workdir, const QString& file);

/// What the status log says of a component's run: its yield or its
/// corners, and ngspice's notes.
struct Summary {
    QString text;
    bool warning = false;
};
Summary summarize(const Component* component, const QString& output, const QString& workdir);
/// This ngspice has no montecarlo or corners command.
bool unsupported(const QString& output, bool corners);

/// The analysis command a component or a stage names (qucs_s::ngopt).
QString analysisCommand(const Schematic* schematic, const QString& analysis);

} // namespace qucs_s::ngstats

#endif
