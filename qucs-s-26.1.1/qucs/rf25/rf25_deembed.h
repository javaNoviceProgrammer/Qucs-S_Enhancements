/*
 * rf25_deembed.h - an edge port's own discontinuity taken out of a full-wave
 *                  solve, and its reference plane moved along its line
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Phase D of docs/proposals/2026-10-23-rf-2p5d-moment-method.md: "the line's
 * own propagation is removed by solving a short and a long calibration line
 * (the classic two-line de-embedding)".
 *
 * An edge port drives the face of its conductor as a gap: what a circuit
 * connects there is the line the face ends, and the port adds a
 * discontinuity of its own - the face's fringing charge, the current's turn
 * into the face, a reference half a cell in - which is the port's, not the
 * structure's. It is found by solving two straight strips of the port's
 * feed, the face's width on the port's metal, l and 2l long, each with a
 * port of the same kind at both ends:
 *
 *     T_l = E L(l) E',   T_2l = E L(2l) E',   so   T_l T_2l^-1 T_l = E E'
 *
 * T a strip's chain matrix as solved, E the port's discontinuity from the
 * port to its face, E' the same the other way round, L the line. E E' - a
 * reciprocal, symmetric two-port - gives E up to an ideal transformer at the
 * face, which nothing measured on lines can fix: taking the discontinuity as
 * a series element at the port and a shunt one at the face (E's D one) fixes
 * it, and keeps the port's own voltage and current at the face. The line is
 * then L(l) = E^-1 T_l E'^-1: its propagation constant and its impedance in
 * that voltage and current, along which a reference plane is moved.
 *
 * A structure's ports are each taken back through their E and the line of
 * the shift asked, [V; I] = F [V'; I']: Z' = (A - Z C)^-1 (Z D - B), F's
 * terms on the diagonals.
 *
 * A coplanar port's strips are its feed whole: the signal, the conductors
 * beside it as wide and as far from it as at the port, and the vias down
 * them as the feed has them - a grounded coplanar line without its vias is
 * another line. A row of vias is a line only a whole number of periods at
 * a time, so the strips begin as the feed does, their first vias as far
 * in from each end, and the longer has twice the shorter's periods
 * between - not twice its length:
 *
 *     T_1 = E P U^n P',   T_2 = E P U^2n P',   so   T_1 T_2^-1 T_1 = (E P)(E P)'
 *
 * P the feed from the face to its first via's centre, U a period. E P is
 * the port's own discontinuity to there, and the line beyond it is the
 * rows' own; the line is then taken back to the face, (E P) L(-o), o the
 * first via in from it.
 *
 * Two strips fit what they are taken for whatever they are: T_2l = E L(l)^2 E'
 * holds by how E and L(l) are found. A third strip, its line phi = 1.618
 * times the shorter's (a whole number of the rows' periods with vias),
 * checks them: E^-1 T_3 E'^-1 should be the line over its own length, and
 * how far it is from it is the calibration's residual at each frequency. On
 * a substrate thick beside the wavelength the ports at a strip's two ends
 * reach one another past the line, by its surface wave, and the strips are
 * no longer one line between two ends alike: a 10 µm feed over 305 µm of
 * silicon was within 1e-4 to 13 GHz and wrong by tens of percent from 45
 * (the phase E check of 2 November). Where they differ by more than a
 * percent, shorter strips - half, a quarter and an eighth as long - are
 * solved too, their ports the nearer but the surface wave's way between
 * them the shorter, and each frequency is taken from the strips that agree
 * best there. The third also stands in where the shorter strip is a whole
 * number of half waves long and its L(l), all but the identity, gives the
 * line's impedance poorly: the impedance is taken from whichever of the
 * two is the further from it.
 */
#ifndef QUCS_RF25_DEEMBED_H
#define QUCS_RF25_DEEMBED_H

#include "rf25_fullwave.h"
#include "rf25_mesh.h"

#include <array>

namespace qucs_s::rf25 {

/// What two lengths of a port's feed say of the port, at each frequency.
struct Calibration {
    QString metal;                 ///< the feed's metal
    double width = 0;              ///< its width, the layout's units
    Feed feed;                     ///< the whole of it: a coplanar one's grounds and vias
    /// The line the shorter strip has between its ends' first vias - its
    /// whole length where it has none - the layout's units; the longer has
    /// twice it.
    double length = 0;
    double strip = 0;              ///< how long the shorter strip is, the layout's units
    QList<double> frequencies;     ///< Hz, as asked
    /// The port's own discontinuity, from the port to its face: a chain
    /// matrix [A, B, C, D] at each frequency, D one - B the series
    /// impedance at the port, C the shunt admittance at the face.
    QList<std::array<Complex, 4>> discontinuity;
    QList<Complex> gamma;          ///< the line's propagation constant, 1/m
    QList<Complex> impedance;      ///< its impedance in the port's voltage and current, Ohm
    double third = 0;              ///< the third strip's line, the layout's units, which checks the other two
    /// At each frequency asked, how far the strips taken there are from one
    /// line between two ends alike (the largest term of the difference over
    /// the largest of the line's chain matrix), and the shorter strip's
    /// length they were (the layout's units).
    QList<double> residual, stripAt;
    QList<double> tried;           ///< the shorter strips' lengths solved, longest first
    /// Hz: up to it the strips agree within a percent at every frequency
    /// asked (nought where they do not at the lowest); and the most they
    /// differ, and where.
    double agreedTo = 0, worst = 0, worstAt = 0;
    /// How far, the layout's units, a port's own field reaches along its
    /// feed - four heights over the ground and its width - past which a
    /// structure's feed must run straight for what its strips find of the
    /// port to be what the port has there; nought for a coplanar feed.
    double reach = 0;
    QStringList problems;
    QStringList lines;             ///< what it did, for the panel
    bool ok() const { return problems.isEmpty() && !discontinuity.isEmpty(); }
    /// The index of \a frequency among those asked (to a part in 1e9), or -1.
    int indexOf(double frequency) const;
};

/// The strips calibrate() solves at each length it tries, all at the same
/// frequencies: the shorter, the longer of twice its line, and the third
/// that checks the two. (The RF 2.5D tab counts a calibration by them.)
constexpr int kStripsPerLength = 3;

/*!
 * The feed of a port on \a metal of \a stack, \a width wide (the layout's
 * units, \a micrometre metres each), solved full-wave at \a frequencies as
 * strips - kStripsPerLength of one length, and as many of each shorter one
 * where those do not agree - meshed as \a how and \a top say - as the
 * structure was - and what they say of the port. \a control, given, can
 * stop it; \a length, given, is the shorter strip's (the layout's units),
 * else it is chosen.
 */
Calibration calibrate(const Stack& stack, const MeshSettings& how, double top, const QString& metal, double width,
                      const QList<double>& frequencies, double micrometre = 1e-6, const FullWaveControl* control = nullptr,
                      double length = 0);

/// The same of \a feed as the full-wave solve found it: a coplanar port's
/// strips with its grounds and their vias, its ports coplanar too.
Calibration calibrate(const Stack& stack, const MeshSettings& how, double top, const Feed& feed,
                      const QList<double>& frequencies, double micrometre = 1e-6, const FullWaveControl* control = nullptr,
                      double length = 0);

/// A port's de-embedding: its own discontinuity taken out by
/// \a calibration (none: left in), and its reference plane moved \a shift
/// (m) in along its line - the calibration's, or a line of \a impedance
/// (Ohm) and effective permittivity \a eps where those are given.
struct Deembedding {
    const Calibration* calibration = nullptr;
    double shift = 0;
    double impedance = 0, eps = 0;
    bool any() const { return (calibration != nullptr && calibration->ok()) || (shift != 0 && impedance > 0 && eps > 0); }
};

/// The most a third strip may differ from the line the other two read for
/// the strips to be one line between two ends alike: a percent.
constexpr double kStripsAgree = 1e-2;

/// \a solved with each of its ports de-embedded as \a ports says, in their
/// order: Z and S at the new reference planes, each port into its own
/// reference, Z as solved kept beside them. What makes the de-embedding not
/// to be trusted - strips that do not agree, a feed shorter than its port's
/// own field reaches, a de-embedding that gives back where the structure as
/// solved does not - goes into its warnings.
FullWave deembedded(const FullWave& solved, const QList<Deembedding>& ports);

} // namespace qucs_s::rf25

#endif
