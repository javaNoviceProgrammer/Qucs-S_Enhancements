/*
 * fem3_eme.h - light through a 3D model's waveguides by eigenmode expansion:
 *              the geometry cut across a line of its plan (every cut of a
 *              layered geometry is rectangles), each cut's modes solved by
 *              the 2D full-vector mode solver, the cuts joined by mode
 *              matching - their modes' overlaps on the two meshes' common
 *              refinement - and each section's modes carried along it as
 *              e^-jβs; the S-matrices cascaded (Redheffer's star product)
 *              into the S-parameters between the ports' modes
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM3_EME_H
#define QUCS_FEM3_EME_H

#include "fem3_geometry.h"
#include "fem_model.h"

#include <Eigen/Core>

#include <QPointF>
#include <QString>
#include <QStringList>

#include <complex>
#include <functional>
#include <memory>
#include <vector>

namespace qucs_s::fem {

class Solution;

/*!
 * A cut of a 3D geometry across a line of its plan: the rectangles of the
 * domains it crosses, in (u, z) - u along the line, z up, both in the
 * geometry's unit. The line runs along `across`, the propagation's direction
 * turned a quarter to the left, so that (u, z, s) is right-handed: along +x,
 * u is y; along +y, u is -x.
 */
struct CrossSection {
    struct Rect {
        double u0 = 0, u1 = 0, z0 = 0, z1 = 0;
        int domain = -1;   ///< the 3D domain
    };
    std::vector<Rect> rects;
    double u0 = 0, u1 = 0, z0 = 0, z1 = 0;   ///< its window
    /// A bend's cut (radial, across an arc of the path): its modes the bend's
    /// - e^-jβs along the arc of radius bendRadius (the geometry's unit),
    /// its axis at u = bendCenter. 0: a straight cut.
    double bendRadius = 0, bendCenter = 0;
    /// Whether \a o is the same cut: the same domains in rectangles whose
    /// edges are within \a tolerance (the geometry's unit) - or, given \a
    /// classes (by domain), domains of the same class: of one material.
    bool sameAs(const CrossSection& o, double tolerance, const std::vector<int>* classes = nullptr) const;
};

/// The cut of \a topology across the line through plan point \a origin along
/// \a across (a unit vector): from u0 to u1 along it and z0 to z1. Empty and
/// why in \a error where the window reaches outside every solid.
CrossSection cutAcross(const Topology3& topology, QPointF origin, QPointF across, double u0, double u1, double z0, double z1,
                       QString* error = nullptr);

/// How a cross-section's modes are solved.
struct EmeSettings {
    int modes = 40;              ///< of each cut
    int order = 2;               ///< the elements' (Nédélec's and Lagrange's)
    /// Elements per wavelength in each material (λ0/n), and at least this
    /// many across each layer and rectangle.
    double resolution = 15;
    int across = 4;
    /// The window's walls: "pec", "pmc" or "pml" - its sides (along u), and
    /// its bottom and top. A PML is a layer \a pmlThickness thick (the
    /// geometry's unit) outside the window, of the material at its edge,
    /// stretched as the mode solver's are (\a pmlReflection at normal
    /// incidence from its outside), the layers' own modes kept as the
    /// radiation's basis - its fast-dying ones (all in a layer) left out.
    /// NOT offered by the model: a single junction with it agreed with
    /// metal walls (0.894 against 0.892), but a 1x2 MMI's field carried
    /// more power out than in (0.54 W through its window of 0.5 W fed, with
    /// either projection alone): the layers' modes are not orthogonal in
    /// power, and truncated, mode matching no longer keeps it. Kept for
    /// whoever takes that up (docs/feature_gaps/2026-11-08-optical-eme-3d.md).
    QString sides = QStringLiteral("pec"), floors = QStringLiteral("pec");
    double pmlThickness = 1;
    double pmlReflection = 1e-6;
    /// "absorbing" walls: a layer \a absorberThickness thick (the geometry's
    /// unit) outside the window, metal behind it, of the material at its
    /// edge with an extinction growing in \a absorberLayers steps as the
    /// square of the depth to \a absorberKappa (one step: that κ throughout)
    /// - isotropic and lossy, its modes as the window's are, so what a
    /// junction radiates dies out there rather than coming back off a wall.
    double absorberThickness = 1, absorberKappa = 0.3;
    int absorberLayers = 4;
    /// The 3D domains that are PMLs (a full-wave solve's): their rectangles
    /// of a cut are its PMLs - stretched outward from the box of the rest, as
    /// the 3D solve stretches them, \a pmlReflection - so a port's modes are
    /// those of the cross-section the 3D field sees there.
    std::vector<int> pmlDomains;
    std::function<bool()> cancelled;
    /// How far a solve is, of 1, and in what (its cuts, then its junctions'
    /// overlaps) - called from its threads, one at a time.
    std::function<void(double, const QString&)> progress;
    /// The cuts, and then the junctions' overlaps, solved side by side (0: as
    /// many as the machine's cores).
    int threads = 0;
};

/*!
 * The modes of a cross-section, E(u, z) e^-jβs: the 2D mode solver's on
 * the cut's own mesh (its solution kept: its mesh, its spaces), each
 * normalized so that ∫ (e × h)·ŝ = 1 without a conjugate - the
 * bi-orthogonality mode matching wants - and turned forward: Im β ≤ 0, Re β
 * > 0 where it is 0. The phase: a propagating mode's fields real; its sign
 * such that its main transverse field's weighted integral over the window
 * is positive (the same mode at both ends of a guide alike).
 */
struct EmeModes {
    std::shared_ptr<const Solution> solution;
    CrossSection section;
    double frequency = 0, k0 = 0;
    double unitScale = 1e-6;                      ///< metres in a unit of the cut
    std::vector<std::complex<double>> beta;
    /// Each mode a column: its transverse E's edge coefficients (Et) and its
    /// longitudinal E_s's node coefficients (Es), normalized - V/m.
    Eigen::MatrixXcd Et, Es;
    std::vector<double> te;                       ///< of each mode's transverse |E|², the part along u
    /// Of each mode's transverse |E|², the part in the PMLs (0 without).
    std::vector<double> inLayers;
    std::vector<double> mur;                      ///< by triangle
    /// A PML's stretching, as the mode solver's (fem_waves.cpp's Media):
    /// beyond the window [inner] toward its own box's edge [outer], s = 1 -
    /// jσ0ξ², σ0 = 1.5 ln(1/R)/(k depth); µ along u, v, s times s_v/s_u,
    /// s_u/s_v, s_u s_v. By triangle the layer it is in (-1: none).
    struct Layer {
        double outer[4] = {0, 0, 0, 0};   ///< u0, u1, v0, v1 (m)
        double k = 0;                     ///< k0 √(n² µr), 1/m
    };
    std::vector<Layer> layers;
    std::vector<int> layerOf;
    /// By triangle: in an absorbing wall (counted with the layers' share).
    std::vector<char> inAbsorber;
    double inner[4] = {0, 0, 0, 0};       ///< the window (m)
    double pmlLog = 13.8155;
    /// A bend's cut: µ along u, v, s times f, f, 1/f, f = |u - centre|/R (m).
    double bendRadius = 0, bendCenter = 0;
    /// How many modes of its window propagate, by Weyl's law: k0² Σ A n²/2π
    /// over its rectangles (both polarizations).
    double propagating = 0;
    int count() const { return int(beta.size()); }
    std::complex<double> neff(int m) const { return beta[std::size_t(m)] / k0; }
    /// "TE0", "TM1": by polarization (TE where most of its transverse field
    /// lies along u), numbered in order of index within it - of the modes
    /// in the window (most of their field out of the PMLs, losing less
    /// than 5 % of their index a radian); a PML's own "PML3" by its number.
    QString label(int m) const;
    /// Whether mode \a m is one of the window's, not a PML's.
    bool inWindow(int m) const;
    /// The mode labelled \a label ("TE0"), or by number ("1", "2"): -1 none.
    int find(const QString& label) const;

    /// Every mode's fields at reference point (xi, eta) of triangle \a t, a
    /// row each: E and H along u, along v (the cut's z) and along s - E in
    /// V/m, H in A/m. \a fields 6 by count().
    void fieldsAt(int t, double xi, double eta, Eigen::MatrixXcd& fields) const;
    /// Mode \a m's at point (u, v) of the window (the geometry's unit): E
    /// and H along u, v, s. False outside its mesh.
    bool fieldAt(int m, double u, double v, std::complex<double> E[3], std::complex<double> H[3]) const;
};

/// \a m's modes' signs as the eigenmode expansion sets them, but seen in the
/// cut's frame \a mirrored - u and the field across it turned over: a port
/// at a device's far end, whose frame looks back, its modes signed as the
/// near end's are (a straight guide's S21 e^-jβL, a TE mode's as a TM's).
void orientModes(EmeModes& m, bool mirrored);

/// The modes of \a section of 3D \a model at \a frequency (Hz): its
/// rectangles a 2D model's, each of its 3D domain's materials, meshed to the
/// settings' resolution and solved by a Mode Analysis about the largest
/// index. Empty and why in \a error.
EmeModes solveSectionModes(const Model& model, std::shared_ptr<ParameterScope> parameters, const Topology3& topology,
                           const CrossSection& section, double frequency, const EmeSettings& settings, QString* error);
/// The cuts solved and kept let go (each kept by its 2D model and the
/// parameters that reads: a swept length's cuts solved once).
void clearEmeCache();

/// O(m, n) = ∫ (e_m of a × h_n of b)·ŝ over the window, integrated exactly
/// (to the elements' degree) on the two meshes' common refinement - each
/// triangle of a clipped by those of b it overlaps.
Eigen::MatrixXcd modeOverlap(const EmeModes& a, const EmeModes& b);
/// The same both ways at once: \a ab ⟨e of a, h of b⟩, \a ba (null: not
/// wanted) ⟨e of b, h of a⟩.
void modeOverlaps(const EmeModes& a, const EmeModes& b, Eigen::MatrixXcd* ab, Eigen::MatrixXcd* ba);
/*!
 * Many pairs' overlaps at once, side by side on \a threads (0: the machine's
 * cores) - each pair's a-triangles in fixed chunks, a job each, a pair's
 * chunks added in their order: the same to the last bit on any number of
 * threads, and as modeOverlaps(). The points' fields are gathered in batches
 * and their products are matrix products, e_u, e_v, h_u and h_v alone.
 * \a progress (jobs done, jobs) after each; false, and nothing to use, if \a
 * cancelled said so between jobs.
 */
bool modeOverlapsMany(const std::vector<std::pair<const EmeModes*, const EmeModes*>>& pairs, int threads, const std::function<bool()>& cancelled,
                      const std::function<void(int, int)>& progress, std::vector<Eigen::MatrixXcd>* ab, std::vector<Eigen::MatrixXcd>* ba);
/// modeOverlaps() point by point, through fieldsAt(): what the batched one
/// was checked against (test_fem3_eme) - slower, the same to rounding.
void modeOverlapsByPoint(const EmeModes& a, const EmeModes& b, Eigen::MatrixXcd* ab, Eigen::MatrixXcd* ba);

/// A scattering matrix between a basis on the left (n1 modes) and one on the
/// right (n2): s21 what goes right of what comes from the left.
struct SMatrix {
    Eigen::MatrixXcd s11, s12, s21, s22;
    int left() const { return int(s11.rows()); }
    int right() const { return int(s22.rows()); }
};
/// An interface's, of the overlap O(m, n) = ⟨e_m left, h_n right⟩ of two
/// bases normalized each to ⟨e_m, h_n⟩ = δ_mn: E continuous projected on the
/// right's h, H on the left's e - R = (I + OOᵀ)⁻¹(I - OOᵀ), T = 2Oᵀ(I + OOᵀ)⁻¹
/// from the left; symmetric, so reciprocal, whatever the modes kept.
SMatrix interfaceS(const Eigen::MatrixXcd& overlap);
/*!
 * An interface's as the expansion takes it: the mean of interfaceS() of
 * \a ab = ⟨e left, h right⟩ and of the other way - E on the left's h, H on the
 * right's e, interfaceS() of \a ba = ⟨e right, h left⟩ turned round. Each
 * converges to the interface's S as the modes kept do, but slowly where a
 * cut has corners (a wire's): of a 0.5 to 1 µm silicon wire's TE0 they
 * gave |T|² 0.912 and 0.892 with 40 modes, 0.906 and 0.895 with 100, 0.900
 * and 0.901 with 200 - their mean 0.902, 0.901, 0.901. Reciprocal, as each
 * is.
 */
SMatrix junctionS(const Eigen::MatrixXcd& ab, const Eigen::MatrixXcd& ba);
/// A section's: its modes carried a length \a length (m), e^-jβL.
SMatrix propagationS(const std::vector<std::complex<double>>& beta, double length);
/// The two cascaded, \a a then \a b (Redheffer's star product).
SMatrix cascade(const SMatrix& a, const SMatrix& b);
/// The identity of \a n modes (no interface).
SMatrix identityS(int n);
/// \a s cascaded with itself \a n times (n ≥ 1; its two sides the same
/// basis), by squaring: log2 n products for a grating's thousand periods.
SMatrix powerS(const SMatrix& s, long long n);

/// A port of an eigenmode expansion: at the start or the end of its axis,
/// over its whole window or a box of it, the modes of its cut it takes.
struct EmePort {
    QString name;
    bool atEnd = false;
    bool whole = true;
    double u0 = 0, u1 = 0, z0 = 0, z1 = 0;   ///< its box (the geometry's unit; u along the cut)
    QStringList modes{QStringLiteral("TE0")};
    bool excite = true;
};

/// A piece of a path through the plan: straight, from a point along a
/// direction, or an arc about a centre (its sweep signed: positive turning
/// left, counter-clockwise). Lengths in the geometry's unit; an arc's its
/// length at its radius.
struct EmePiece {
    bool arc = false;
    QPointF start, dir;            ///< a straight piece's
    QPointF center;                ///< an arc's
    double radius = 0, angle0 = 0, sweep = 0;   ///< its radius, the angle of its start, its sweep (radians)
    double length = 0;
};

/// A section of a device repeated: its cells from s0 to s1 along the axis -
/// one period, as drawn - taken \a periods times in a row in their place, the
/// period's S-matrix with the junction back to its start raised to that
/// power by squaring: a grating or a Bragg mirror of many periods drawn once.
struct EmePeriodic {
    double s0 = 0, s1 = 0;   ///< along the axis (the geometry's unit)
    long long periods = 1;
};

/// What an eigenmode expansion solves: an axis of the plan (from origin, s
/// along `along`) - or a path of straight pieces and arcs, s its length -,
/// its range, the window of its cuts (u across it, to the left), its ports.
struct EmeDevice {
    /// A full-wave solve's ports kept as a device's (Solution3::Optics): their
    /// names and feeds alone, no axis.
    bool fullWave = false;
    QPointF origin, along{1, 0}, across{0, 1};
    /// A path's pieces in order (empty: the straight axis); its s from 0 at
    /// its start to the sum of their lengths.
    std::vector<EmePiece> path;
    double s0 = 0, s1 = 0;                   ///< along the axis (the geometry's unit)
    double u0 = 0, u1 = 0, z0 = 0, z1 = 0;   ///< the window
    double maxCell = 0;                      ///< the longest cell where the cut changes (0: one per piece)
    double unitScale = 1e-6;
    std::vector<EmePort> ports;
    /// Sections repeated, in order along the axis and apart: no cell crosses
    /// their ends.
    std::vector<EmePeriodic> periodic;
    EmeSettings settings;
};

/// A cell of the expansion: from s0 to s1 along the axis, cut at its middle.
struct EmeCell {
    double s0 = 0, s1 = 0;
    int section = -1;   ///< its cut, among the device's distinct ones
};
/// The cells of \a device: a cell from each change of the cut to the next -
/// a vertex of the plan, or where it meets the axis -, a piece where the cut
/// changes all along it (a taper, a curve) divided into cells no longer than
/// maxCell; and its distinct cuts - given \a materialOf (each domain's
/// material's class), cuts alike in their rectangles and those rectangles'
/// materials one (a drawn grating's teeth, each a domain of its own: two).
std::vector<EmeCell> emeCells(const Topology3& topology, const EmeDevice& device, std::vector<CrossSection>* sections, QString* error,
                              const std::vector<int>* materialOf = nullptr);

/// One frequency's expansion: the S-matrix between the ports' modes, each
/// port mode a column of what it feeds.
struct EmeResult {
    double frequency = 0;
    struct PortMode {
        int port = -1;
        QString label;                 ///< "TE0"
        std::complex<double> neff;     ///< β/k0, Im ≤ 0
        double te = 0;
    };
    std::vector<PortMode> portModes;
    Eigen::MatrixXcd S;                ///< [out][in], by port mode
    std::vector<EmeCell> cells;
    int sections = 0;                  ///< distinct cuts solved
    /// Of each fed port mode: the power that leaves by the ports' modes, of 1
    /// fed (1 - it is lost: radiated, absorbed, into modes no port takes).
    std::vector<double> balance;
    QStringList log;
    /// What may make it less than it should be: a cut whose modes kept are
    /// all propagating (the window's radiation not all carried).
    QStringList warnings;
    QString error;
    bool ok() const { return error.isEmpty(); }
};
/// \a device of 3D \a model at \a frequency (Hz).
EmeResult solveEme(const Model& model, std::shared_ptr<ParameterScope> parameters, const Topology3& topology, const EmeDevice& device,
                   double frequency);

/// \a results (an expansion's at each wavelength solved, of \a device) as a
/// Touchstone 1.1 file (.sNp: # Hz S RI R 50): its port modes the file's
/// ports, in order, each normalized to its own mode at 1 W - the reference
/// impedance a placeholder, the same at every port -, the frequencies
/// rising, comments naming each port. Their count in \a ports; empty and
/// why in \a error.
QString emeTouchstone(const EmeDevice& device, const std::vector<EmeResult>& results, const QStringList& comments, int* ports, QString* error);

/// The device a physics of type "eme" of 3D \a model asks for, its
/// expressions read in \a parameters at \a frequency (the cells' longest
/// part may be of λ0). Empty and why in \a error.
bool readEmeDevice(const Model& model, const Node& physics, const Topology3& topology, const ParameterScope& parameters, double frequency,
                   EmeDevice* device, QString* error);

} // namespace qucs_s::fem

#endif // QUCS_FEM3_EME_H
