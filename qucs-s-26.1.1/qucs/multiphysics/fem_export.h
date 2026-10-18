/*
 * fem_export.h - a multiphysics model's results as parts of circuits: a
 *                transmission line's S-parameters as a Touchstone file,
 *                as a SPICE subcircuit (sections of R, L, G, C, or a lossy
 *                line) or a Qucs subcircuit of its RLCG part; terminals'
 *                capacitance, conductance or inductance matrices as a SPICE
 *                subcircuit of capacitors, resistors, coupled inductors
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_EXPORT_H
#define QUCS_FEM_EXPORT_H

#include "fem_results.h"

namespace qucs_s::fem {

/// A line's parameters at each frequency of a solution set's point of a
/// sweep (\a point from 1; 0: the last), as a Line Parameters node of
/// method "rlgc" finds them.
struct LineSweep {
    std::vector<LineRLGC> points;
    QString error;
};
LineSweep lineSweep(const SolutionSet& set, const Node& lineParameters, int point = 0);

/// A line of \a length (m) as a two-port: its S-parameters for ports of
/// \a zref (Ω) at each frequency, a Touchstone 1.1 file (.s2p: # Hz S RI R
/// zref), \a comments first ("!" lines).
QString touchstone(const LineSweep& sweep, double length, double zref, const QStringList& comments = {});
/// The S-matrix of physics \a physics's ports (waves in the plane) at each
/// frequency of a set's point, a Touchstone 1.1 file (.sNp: # Hz S RI R 50,
/// each port normalized to its own mode); their count in \a ports.
QString touchstonePorts(const SolutionSet& set, const QString& physics, int point, const QStringList& comments, int* ports, QString* error);
/// S11, S21, S12, S22 of that line at one point.
std::array<std::complex<double>, 4> lineSParameters(const LineRLGC& line, double length, double zref);

/// A SPICE subcircuit of a line of \a length (m), its pins in, out and ref:
/// \a sections of R, L in series and C, G across (a ladder), or (\a ltra)
/// one LTRA lossy line (ngspice's, Xyce's).
QString spiceLine(const LineRLGC& line, double length, const QString& name, int sections, bool ltra, const QStringList& comments = {});

/// A SPICE subcircuit of a physics' terminal matrix in \a solution: an
/// electrostatics' capacitance matrix as capacitors (each terminal's to the
/// others and to ref), currents' conductance matrix as resistors, magnetic
/// fields' inductance matrix as inductors (each coil's two pins) coupled by
/// K, with their resistances. Empty and why in \a error when it has none.
QString spiceMatrix(const Solution& solution, const QString& physics, const QString& name, const QStringList& comments, QString* error);

/// A Qucs schematic (\a version: Qucs-S's, as its files say) of a
/// subcircuit: an RLCG part of \a line's values and \a length between ports
/// 1 and 2, its symbol a box. As a clip (\a clip): the part alone, for a
/// paste.
QString qucsLineSchematic(const LineRLGC& line, double length, const QString& version, const QString& name, bool clip = false);

/// Coupled lines at each frequency of a set's point (a Line Parameters node
/// with two or more terminals, of method "rlgc"), by frequency.
struct CoupledSweep {
    std::vector<CoupledLines> points;
    QString error;
};
CoupledSweep coupledSweep(const SolutionSet& set, const Node& lineParameters, int point = 0);

/// N coupled lines of \a length (m) as a 2N-port, by their modes: ports 1
/// to N their near ends (in the conductors' order), N+1 to 2N their far
/// ends; its S-matrix for ports of \a zref (Ω).
std::vector<std::vector<std::complex<double>>> coupledSParameters(const CoupledLines& lines, double length, double zref);
/// A pair's mixed-mode S-matrix of its single-ended 4-port's (ports 1, 2
/// the near ends, 3, 4 the far ends): ports d1, d2, c1, c2 - the
/// differential and common modes at each end ([Sdd Sdc; Scd Scc]).
std::vector<std::vector<std::complex<double>>> mixedModeSParameters(const std::vector<std::vector<std::complex<double>>>& s);
/// Coupled lines of \a length at each frequency, a Touchstone 1.1 file
/// (.s4p, .s6p...: # Hz S RI R zref, rows of the matrix); a pair's
/// mixed-mode matrix (\a mixed: its ports d1, d2, c1, c2, said).
QString touchstoneCoupled(const CoupledSweep& sweep, double length, double zref, bool mixed, const QStringList& comments = {});
/// A SPICE subcircuit of coupled lines of \a length, its pins in_<line>,
/// out_<line> and ref: \a sections of each line's R, L in series (mutual
/// L by K, mutual R by current-controlled voltage sources), C and G across
/// (to ref and between the lines).
QString spiceCoupled(const CoupledLines& lines, double length, const QString& name, int sections, const QStringList& comments = {});
/// A Qucs schematic of a subcircuit of a symmetric pair: a coupled line part
/// (CTLIN) of its even and odd modes' Z0 (their real parts), εeff and
/// attenuation at its frequency, its ports 1, 2 the near ends, 3, 4 the far
/// ends. As a clip (\a clip): the part alone, for a paste. Empty and why in
/// \a error for lines that are not a symmetric pair.
QString qucsCoupledSchematic(const CoupledLines& lines, double length, const QString& version, const QString& name, QString* error,
                             bool clip = false);

/// What a Results > Export node makes: a file's text and its suffix.
struct ExportText {
    QString text;
    QByteArray data;  ///< a binary file's (NumPy's .npy) instead of text
    /// A grid's .npy: its points' x and y (shape 2, ny, nx), written beside
    /// it as <file>_xy.npy - the values' own file has no coordinates.
    QByteArray coordinates;
    QString suffix;   ///< s2p, cir, sch, csv, npy, dat
    QString summary;  ///< a line saying what is in it
    QString error;
};
/// Runs export node \a node of \a model on \a set (the solutions of its
/// study): Touchstone, SPICE or Qucs subcircuit, fields on a grid or at the
/// mesh's nodes. \a version: Qucs-S's.
ExportText exportText(const Model& model, const SolutionSet& set, const Node& node, const QString& version);

} // namespace qucs_s::fem

#endif // QUCS_FEM_EXPORT_H
