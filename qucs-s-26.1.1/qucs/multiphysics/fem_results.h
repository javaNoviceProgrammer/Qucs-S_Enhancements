/*
 * fem_results.h - what a multiphysics solution shows: an expression's
 *                 values over the domains (a color map), its contours, a
 *                 vector field's arrows, a value at a point, integrals,
 *                 averages, maxima over domains or boundaries, global
 *                 values, a transmission line's quasi-TEM parameters
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_RESULTS_H
#define QUCS_FEM_RESULTS_H

#include "fem_solver.h"

#include <array>

#include <QLineF>

#include <complex>

#include <optional>
#include <vector>

namespace qucs_s::fem {

/// The triangle a point is in and where in it, on a grid of the mesh's
/// triangles. Points in the geometry's unit.
class Locator
{
public:
    explicit Locator(const Mesh& mesh);
    /// False when \a p is in no triangle.
    bool locate(QPointF p, int* t, double* xi, double* eta) const;

private:
    const Mesh& a_mesh;
    QRectF a_box;
    int a_nx = 1, a_ny = 1;
    double a_cw = 1, a_ch = 1;
    std::vector<std::vector<int>> a_cells;
};

/// A ray's path as traced (geometrical optics): its points (x, y in m, its
/// power in W, its optical path in m there), its wavelength, its release's
/// number (from 1), how it ended (0 the most interactions or path, 1 a wall
/// or absorber, 2 a detector that stops it, 3 out of the physics, 4 its power
/// below the threshold).
struct RayPath {
    std::vector<std::array<double, 4>> points;
    double lambda = 0;
    int index = 0;
    int status = 0;
};
/// A geometrical optics field's rays (its snapshot's, as selected).
std::vector<RayPath> rayPaths(const Field& field);

/// A unit a value is shown in: shown = (SI - offset) / scale.
struct DisplayUnit {
    double scale = 1, offset = 0;
    QString name;   ///< as shown ("mV", "degC"); empty: SI's
};
/// \a text for values of \a dim: empty, SI's own; one not of that unit,
/// SI and why in \a warning.
DisplayUnit displayUnit(const QString& text, const Dim& dim, QString* warning = nullptr);
/// The unit a value of \a expression is shown in, \a text asked: as
/// displayUnit, and for a global of a unit its dimension cannot say
/// (emw.loss, dB/m) its own when none is asked, or that unit over another
/// length (dB/cm).
DisplayUnit displayUnitOf(const Solution& solution, const QString& expression, const QString& text, const Dim& dim, QString* warning = nullptr);

/// Where a plot is drawn: each point moved by a displacement (x, y: its
/// expressions, in metres) times a scale.
struct Deformation {
    QString x, y;
    double scale = 1;        ///< 0: automatic - the largest a tenth of the model's size
    double used = 0;         ///< the scale it was drawn with
    double largest = 0;      ///< the largest displacement (m)
};

/// An expression over the domains: each element in triangles (in four for
/// a quadratic field), its value at their corners. Points in the
/// geometry's unit (moved, deformed), values as shown.
struct SurfaceData {
    std::vector<QPointF> points;   ///< three a triangle
    std::vector<double> values;    ///< one a point (NaN: not defined there)
    double min = 0, max = 0;       ///< of those defined
    Dim dim;
    DisplayUnit unit;
    QString error, warning;
};
SurfaceData surfaceData(const Solution& solution, const QString& expression, const QString& unit = {}, Deformation* deformation = nullptr);

/// An expression along a line from \a a to \a b (the geometry's unit), at
/// \a count points: where (and how far along, in metres), and its value
/// (SI; NaN outside the domains).
struct LineData {
    std::vector<QPointF> points;
    std::vector<double> along;
    std::vector<double> values;
    Dim dim;
    QString error;
};
LineData lineData(const Solution& solution, const Locator& locator, const QString& expression, QPointF a, QPointF b, int count);

/// The lines where a surface's values are each of \a levels values,
/// evenly between its least and greatest.
struct ContourData {
    std::vector<double> levels;
    std::vector<std::vector<QLineF>> lines;   ///< by level
};
ContourData contourData(const SurfaceData& surface, int levels);

/// Arrows of a vector field on a grid over \a region (the geometry's
/// unit): where, and the vector (SI).
struct ArrowData {
    std::vector<QPointF> at;
    std::vector<QPointF> vector;
    double longest = 0;
    QString error;
};
ArrowData arrowData(const Solution& solution, const QString& x, const QString& y, const QRectF& region, int nx, int ny);

/// \a expression at \a p (the geometry's unit), SI; its unit in \a dim.
std::optional<double> evaluateAt(const Solution& solution, const Locator& locator, const QString& expression, QPointF p,
                                 QString* error = nullptr, Dim* dim = nullptr);
/// A compiled expression at \a p (none: in no domain, or not finite there).
std::optional<double> evaluateAt(const Solution& solution, const Locator& locator, const Expression& expression, QPointF p);

/// A derived value found: its name, value (SI) and how it is shown.
struct DerivedValue {
    QString name;
    double value = 0;
    Dim dim;
    QString text;   ///< as shown, with its unit
};
/// One item of a list of globals: "name = expression" names it (a
/// dataset's variable, a derived value - emw.loss/100 is otherwise
/// emw.loss_100 in a dataset), else the expression is its own name.
struct NamedExpression {
    QString name, expression;
};
NamedExpression namedExpression(const QString& item);
/// A family of globals in a list of items: "name*" (a global's name and a
/// star, alone) each of \a solution's globals name1, name2... in their
/// order - qw.Ev* a Valence Bands step's every state; any other item as it
/// is, as is one of no such globals (or no solution).
QStringList expandFamilies(const QStringList& items, const Solution* solution);

struct DerivedResult {
    QList<DerivedValue> values;
    QString error;
    QString note;   ///< what it is worth knowing (a number integrated: its unit the area's)
};
/// Evaluates a Derived Values node (global, integral, average, maximum,
/// minimum, pointeval, lineparams, confinement, perturbation) on \a
/// solution (its snapshot selected).
DerivedResult evaluateDerived(const Solution& solution, const Node& node);

/// A transmission line's parameters per metre at a solution's frequency:
/// C from electrostatics, G from its dielectrics' losses, R and L from the
/// magnetic fields' coils (a Line Parameters node of method "rlgc"); its
/// characteristic impedance and propagation constant.
struct LineRLGC {
    double frequency = 0;            ///< Hz
    double R = 0, L = 0, G = 0, C = 0;
    std::complex<double> Zc, gamma;  ///< Ω; 1/m (α + jβ)
    double alphaDb = 0;              ///< dB/m
    /// (β/k0)², the slow wave of the conductors' loss in it; c/v.
    double epsEff = 0, velocity = 0;
    QString error;
};
LineRLGC lineRLGC(const Solution& solution, const Node& node);

/// The signal terminals (conductors) of a Line Parameters node: its
/// "terminal" split by commas - two or more, coupled lines.
QStringList lineTerminals(const Node& node);

/// A mode of coupled lines at a frequency: its propagation constant, its
/// voltages on the conductors (the largest 1) and currents (Y v / γ), its
/// impedance (vᵀv / vᵀi: a pair's even or odd mode's Z of one line).
struct LineMode {
    QString name;                    ///< "even", "odd" (a symmetric pair); "1", "2"... (by n_eff, the highest first)
    std::complex<double> gamma, Z;   ///< 1/m (α + jβ); Ω
    std::vector<std::complex<double>> v, i;
    double alphaDb = 0;              ///< dB/m
    double nEff = 0;                 ///< β/k0
    double epsEffQs = 0;             ///< quasi-static: its mode of C v = ε C0 v
};
/// Coupled lines per metre at a solution's frequency (a Line Parameters
/// node with two or more terminals): N signal conductors over a reference
/// (the grounds, the return coil or the boundaries), their matrices -
/// C (Maxwell's: C_ij the charge on i of j's volt), C0 (every εr 1), G (the
/// dielectrics' losses), and of the magnetic fields' coils, reduced against
/// the return, R and L (quasi-TEM: L = C0⁻¹/c², R and G none) - and their
/// modes: of (R + jωL)(G + jωC); a symmetric pair's even and odd.
struct CoupledLines {
    using Matrix = std::vector<std::vector<double>>;
    double frequency = 0;            ///< Hz (quasi-TEM: none)
    bool rlgc = false;
    QStringList names;               ///< the conductors: their terminals' names
    Matrix R, L, G, C, C0;
    std::vector<LineMode> modes;
    /// The characteristic impedance matrix: V⁺ = Zc I⁺.
    std::vector<std::vector<std::complex<double>>> Zc;
    bool symmetricPair = false;
    QString error;
};
CoupledLines coupledLines(const Solution& solution, const Node& node);

/*!
 * The modes, their impedances and the characteristic impedance matrix of
 * \a lines, whose names and R, L, G, C and C0 matrices (per metre) are
 * already filled, at \a w (rad/s).
 *
 * False and why in its error() where they cannot be found. It is public
 * because a quasi-static extraction of a layout - the RF 2.5D solver's -
 * fills the same matrices by another route and must get the same modes
 * from them: there is one mode solver, not two that can drift apart.
 */
bool coupledModes(CoupledLines& lines, double w);

/// A study's solutions as results take them: each of its sweep's (one
/// without a sweep), and of each, each output time (none at rest).
struct SolutionSet {
    std::vector<std::shared_ptr<Solution>> solutions;
    QStringList swept;   ///< the parameters swept
    /// A sweep stopped before its last point (cancelled, a point without a
    /// solution): where and why, its points before kept; empty: whole.
    QString stopped;
};
/// One solution of a set: its sweep point and its snapshot (-1: as it is).
struct Instance {
    int point = 0;
    int snapshot = -1;
    double time = 0;     ///< its time (s), or its frequency (Hz) in the frequency domain
    int mode = 0;        ///< a mode analysis's mode (from 1), else 0
    QString label;       ///< "t = 0.5 s", "f = 1 GHz", "mode 2", "w = 2 mm, t = 0.5 s"; empty: the only one
};
/// Every instance of \a set (each point, each time or frequency), or those
/// picked: the point \a point (from 1; 0 the last), the time \a time or (in
/// the frequency domain) the frequency \a frequency (empty: the last), of a
/// mode analysis the mode \a mode (from 1; 0: the one its step chose, the
/// first unless it says; -1, all: the one chosen at each frequency). With
/// \a all, a time or frequency given picks it at each point. A time or
/// frequency that is not read, of another unit, or past the solution's: none,
/// and why in \a error.
std::vector<Instance> instances(const SolutionSet& set, bool all, int point = 0, const QString& time = {}, const QString& frequency = {},
                                int mode = 0, QString* error = nullptr);
/// The solution of \a instance, its snapshot selected.
Solution* select(const SolutionSet& set, const Instance& instance);
/// The mode a "solutions" (or a 1D plot group's "modes") choice \a which
/// asks of \a set's mode analyses, as instances() takes it: -1 the one its
/// step chose at each ("chosen"; "auto" where a step chose one), 0 each.
int modesAsked(const SolutionSet& set, const QString& which);
/// The mode analysis \a node (a derived value's, an export's) is of: the
/// physics it names - not named, where the solution has no mode analysis
/// of its default tag (emw), the solution's first.
QString modeAnalysisOf(const Solution& solution, const Node& node);
/// The domains of physics \a physics's perfectly matched layers (its
/// features of a type ending "/pml"), from 0.
std::vector<int> pmlDomainsOf(const Solution& solution, const QString& physics);
/// A Perturbation along a sweep (\a values: evaluateDerived's at each of
/// \a at, instances of \a set): to each but the first point's of its mode,
/// that point taken as the reference, its phase from it dphi_sweep = k0 L
/// (Δneff - Δneff0) (a length given) and VpiL = λ|V - V0|/2|Δneff - Δneff0|
/// (a bias given: the swept parameter).
void perturbationAlongSweep(const SolutionSet& set, const std::vector<Instance>& at, const Node& node, std::vector<DerivedResult>& values);

} // namespace qucs_s::fem

#endif // QUCS_FEM_RESULTS_H
