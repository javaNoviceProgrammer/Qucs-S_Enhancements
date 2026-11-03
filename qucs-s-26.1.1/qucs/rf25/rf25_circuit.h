/*
 * rf25_circuit.h - a lumped circuit fitted to a one- or two-port's S: an
 *                  RF 2.5D solve's ports as R, L and C for a schematic
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_RF25_CIRCUIT_H
#define QUCS_RF25_CIRCUIT_H

#include <QList>
#include <QString>
#include <QStringList>

#include <complex>
#include <utility>

namespace qucs_s::rf25 {

/*!
 * One branch of an equivalent circuit: a few R, L and C in a form, as an
 * impedance between its two nodes, with how well it fits its share of the
 * solve.
 *
 * The forms: "R", "C", "R+L", "R+C", "R+L+C" in series; "R||C" side by
 * side; "(R+L)||C", an inductor with its own capacitance across it;
 * "C+(R||C)", a capacitor over a lossy substrate (Cox, then Rsub beside
 * Csub); "open", none; "short", a wire.
 */
struct CircuitBranch {
    QString form;
    QList<std::pair<QString, double>> values;   ///< each element by name ("R", "L", "C", "Cox", "Rsub", "Csub"), SI
    double error = 0;                           ///< rms of |Y fitted / Y - 1| over the frequencies
    /// Its admittance at \a w, rad/s.
    std::complex<double> admittance(double w) const;
    /// A value by its element's name; nought where it has none.
    double value(const QString& element) const;
};

/*!
 * A lumped circuit fitted to a one- or two-port's S: a two-port as a pi - a
 * branch between its two ports, and one from each port to the reference
 * (the ground) -, a one-port as one branch to the reference.
 *
 * Each branch is fitted on its own to what the solve's Y-parameters give it
 * - Y11 + Y12 and Y22 + Y12 to the reference, -Y12 between - by each form,
 * the values kept positive; the form chosen is the one that fits best, a
 * simpler one where it fits nearly as well. How far the circuit's S then
 * is from the solve's, at most and on the whole, is said: a lumped circuit
 * holds only while what it stands for is short beside the wavelength.
 */
struct EquivalentCircuit {
    int ports = 0;
    QStringList portNames;
    QList<double> frequencies;   ///< Hz, those fitted to
    QList<double> references;    ///< Ohm, each port's
    CircuitBranch between;       ///< a two-port's, from its first port to its second
    CircuitBranch toGround[2];   ///< from each port to the reference
    double worst = 0;            ///< the most |S - S of the solve| over every term and frequency
    double worstAt = 0;          ///< Hz, where
    double rms = 0;              ///< its rms over them all
    QStringList problems;
    QStringList lines;           ///< what was fitted, and how well, in words
    bool ok() const { return problems.isEmpty() && ports > 0; }
    /// Its S at \a frequency, each port into its own reference.
    QList<QList<std::complex<double>>> sAt(double frequency) const;
};

/// \a s - a matrix at each of \a frequencies, each port into its own
/// reference, \a references - fitted by a lumped circuit; a one- or
/// two-port's, else said that it is not.
EquivalentCircuit fitCircuit(const QStringList& ports, const QList<double>& frequencies,
                             const QList<QList<QList<std::complex<double>>>>& s, const QList<double>& references);

/// \a value as Qucs-S writes one: four figures and a prefix, "117.3 pH".
QString valueSaid(double value, const QString& unit);
/// It as a SPICE subcircuit named \a name, its ports then the reference
/// ("ref") as its pins; \a comment, lines put before it.
QString spiceOf(const EquivalentCircuit& circuit, const QString& name, const QStringList& comment);
/// It as a Qucs-S schematic made to be a part of another: a subcircuit
/// port for each of its ports, the reference its ground; \a comment, a
/// text on it.
QString schematicOf(const EquivalentCircuit& circuit, const QStringList& comment);

} // namespace qucs_s::rf25

#endif
