/*
 * ngsweep.h - the NgSweep component's ngspice command: `sweep`, the
 * parametric sweep built into ngspice builds that have it (the
 * Ngspice_OpenVAF_Enhancements fork): what the component stores, the
 * command line and the .control lines it becomes, the results read back
 * into the dataset, and what the status log says of them
 *
 *   sweep <knob> (lin <N> <start> <stop> | list <v1> <v2> ...)
 *         [-vs <knob> <spec>]... -analysis <command> [-output <expr> ...]
 *
 * The knob is anything ngspice can change - a device (R1, V1), an
 * instance parameter (@m1[w]), a model parameter (@dmod[is]), a .param,
 * temp - and ngspice tells which it is. Every value of it (and of the
 * outer -vs knobs) runs the analysis once: the last value of each output
 * is recorded against the knob, and each run's own plot - its voltages
 * and currents over frequency, time or a dc sweep - is kept, and becomes
 * a family of curves, one per value.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_NGSWEEP_H
#define QUCS_NGSWEEP_H

#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

#include "ngstatistics.h"

class Component;
class Schematic;

namespace qucs_s::ngsweep {

/// A swept knob and its values: "lin" from Start to Stop in Points
/// evenly spaced values, "log" the same spaced evenly on a log scale,
/// "list" the values listed ("1k; 2.2k; 4.7k": the List property). As an
/// outer knob:
/// "Vs=C1|list|||| 1n; 2n".
struct Knob {
    QString name;                           ///< as ngspice names it: R1, @r1[resistance], @dmod[is], a .param, temp
    QString type = QStringLiteral("lin");   ///< lin, log, list
    QString start;
    QString stop;
    QString points;
    QString values;

    static bool parse(const QString& value, Knob* knob);
    QString toString() const;
};

/// The values a knob takes, and the spec ngspice's sweep takes for them
/// ("lin 5 1000 5000", "list 1000 2154.43469 4641.588834 10000"); false
/// and why in \a error when they cannot be written. The values are the
/// ones ngspice computes from the spec.
bool knobValues(const Knob& knob, QVector<double>* values, QString* spec, QString* error = nullptr);

/// What the NgSweep component stores.
struct Sweep {
    QString analysis;         ///< a simulation component's name, or an ngspice analysis command
    Knob knob;                ///< the inner knob: the x-axis of the recorded values
    QList<Knob> outer;        ///< -vs knobs (up to 3): a curve for every combination
    bool waveforms = true;    ///< keep every run's voltages and currents (ac, dc, tran)
    QList<ngstats::Record> records;

    static Sweep read(const Component* component);
    void write(Component* component) const;
};

/// The component model.
inline constexpr const char* kModel = ".NGSWEEP";
bool isSweep(const Component* component);

/// At most this many knobs (the inner one and the -vs ones) and runs.
inline constexpr int kMaxKnobs = 4;
inline constexpr int kMaxRuns = 100000;

/// Whether \a analysis (an ngspice command) makes waveforms a sweep can
/// keep as families: ac, dc and tran.
bool waveformAnalysis(const QString& analysis);

/// The sweep line of \a command; false and why in \a error when it cannot
/// be written. \a nodes are the voltages and currents the simulations
/// save ("v(out) i(vpr1)"): recorded after the named values. Null when
/// not known (the dialog, the ERC): then they are left out, and nothing
/// is refused for their lack.
bool commandLine(const Sweep& command, const Schematic* schematic, const QString& nodes, QString* line,
                 QString* error = nullptr);
bool commandLine(const Component* component, const Schematic* schematic, const QString& nodes, QString* line,
                 QString* error = nullptr);

/// The files a component's results are written to, in the Scratch folder:
/// the recorded values, and every run's plot.
QString valuesFile(const QString& component);
QString waveformsFile(const QString& component);

/// The .control lines of \a component: the command between two markers
/// in the output, and the writes of its results; the files written are
/// appended to \a outputs. Empty, and why in \a error, when the command
/// cannot be written.
QString controlBlock(const Component* component, const Schematic* schematic, const QString& nodes,
                     QStringList* outputs, QString* error = nullptr);

/// Whether \a file (a name of the outputs list) is one of these results.
bool isResultFile(const QString& file);
/// The Qucs dataset blocks of the result \a file in \a workdir, for the
/// NgSweep component of \a schematic it names: under the component's
/// name, each knob's values ("ngsweep1.r1"), every recorded value against
/// them, and each waveform ("ngsweep1.v(out)") against its scale and the
/// knobs. Empty for a file that holds none (the waveforms come with the
/// values' own file).
QString datasetBlocks(const QString& workdir, const QString& file, const Schematic* schematic);

/// The dataset name of a knob's values, after the component's name:
/// "R1" is r1, "@r1[resistance]" r1_resistance.
QString knobVariable(const QString& knob);

/// What the status log says of a component's run: the knobs and the
/// analysis, and ngspice's warnings.
ngstats::Summary summarize(const Component* component, const Schematic* schematic, const QString& output,
                           const QString& workdir);
/// This ngspice has no sweep command.
bool unsupported(const QString& output);

/// \a output without the warnings ngspice prints of the knobs of the
/// sweeps named in \a sweeps when it reads them to tell what they are
/// ("Warning from checkvalid: vector r1 is not available or has zero
/// length.") - nothing wrong with the circuit.
QString withoutKnobProbes(const QString& output, const Schematic* schematic, const QStringList& sweeps);

} // namespace qucs_s::ngsweep

#endif
