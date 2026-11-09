/*
 * fem3_solver.h - the multiphysics solver in 3D: a 3D model's heat,
 *                 electrostatics and electric currents on a swept mesh's
 *                 prisms, its study at rest and in time, and the fields and
 *                 globals as solved (at each output time)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM3_SOLVER_H
#define QUCS_FEM3_SOLVER_H

#include "fem3_eme.h"
#include "fem3_wave.h"
#include "fem3_estimate.h"
#include "fem3_fe.h"
#include "fem_expr.h"
#include "fem_model.h"
#include "fem_solver.h"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace qucs_s::fem {

/// One physics solved in 3D: its field on its space, and what was found.
struct Field3 {
    QString tag;           ///< es, ec, ht
    QString type;          ///< electrostatics, currents, heat
    QString variable;      ///< its dependent variable's name: V, T
    Dim dim;
    std::shared_ptr<const Space3> space;
    std::vector<double> values;   ///< by degree of freedom, SI
    std::vector<char> domains;    ///< by domain: solved there
    struct Terminal {
        QString name;
        bool floating = false;    ///< of a charge (current) given, its potential found
        double potential = 0;     ///< V
        double charge = 0;        ///< C (es) or A (ec): into the domain from it
    };
    std::vector<Terminal> terminals;
    /// Each terminal at 1 V in turn, the others at 0: the capacitance (es, F)
    /// or conductance (ec, S) matrix - [i][j] terminal i's charge of j's
    /// volt. Empty: no sweep.
    std::vector<std::vector<double>> matrix;
    double initial = 0;           ///< its value before it is solved
    bool solved = false;
};

class PointEvaluator3;
class Locator;

/*!
 * A 3D model's fields as solved - each physics' on its space - and its
 * globals (ht.Tmax, es.C11, ec.R...). Expressions read x, y, z (m), t, dom
 * and the fields' variables (es.V, es.Ex, es.normE, ec.Jz, ec.Qrh, ht.T,
 * ht.qz...) in its point scope; its scope has the globals too.
 */
class Solution3
{
public:
    Solution3(const Model& model, std::shared_ptr<ParameterScope> parameters, std::shared_ptr<const Mesh3> mesh);

    const Model& model() const { return a_model; }
    const Mesh3& mesh() const { return *a_mesh; }
    std::shared_ptr<const Mesh3> meshPtr() const { return a_mesh; }
    const Topology3& topology() const { return *a_mesh->topology; }
    const ParameterScope& parameters() const { return *a_params; }
    std::shared_ptr<ParameterScope> parametersPtr() const { return a_params; }
    std::vector<Field3>& fields() { return a_fields; }
    const std::vector<Field3>& fields() const { return a_fields; }
    int fieldIndex(const QString& tag) const;
    /// The time the fields are at (s): 0 at rest.
    double time() const { return a_time; }
    void setTime(double t) { a_time = t; }

    // Snapshots: a study in time's fields and globals at its output times
    // (none at rest), one of which is selected.
    int snapshotCount() const { return int(a_snapshots.size()); }
    double snapshotTime(int i) const { return a_snapshots[std::size_t(i)].time; }
    /// The snapshot the fields and globals are of (-1: none kept).
    int selected() const { return a_selected; }
    /// The fields and globals of snapshot \a i (out of range: the last).
    void select(int i);
    /// The fields and globals as they are now, kept as a snapshot at time().
    void addSnapshot();
    /// The last snapshot made again of the fields as they are now (a step at
    /// rest after one in time).
    void replaceLastSnapshot();
    /// The snapshot nearest time \a t (s), or -1 when there is none.
    int snapshotAt(double t) const;
    /// \a text, an expression of the globals and parameters, at each
    /// snapshot (none: empty, and why); its dimension in \a dim. The
    /// selected snapshot's as they were after.
    std::vector<double> globalSeries(const QString& text, Dim* dim, QString* why) const;
    /// Its snapshots are of output times (a Time Dependent step's).
    bool timeDependent = false;
    /// Its snapshots are of wavelengths (a Wavelength Domain step's), each of
    /// its frequency (Hz).
    bool spectral = false;
    double snapshotFrequency(int i) const { return a_snapshots[std::size_t(i)].frequency; }
    /// The frequency the globals are of (Hz; 0: none).
    double frequency() const { return a_frequency; }
    void setFrequency(double f) { a_frequency = f; }
    /// An eigenmode expansion's results, by physics: its device as read, and
    /// its S-parameters and ports' modes at each wavelength solved (the
    /// snapshots' order). A full-wave solve's (Electromagnetic Waves,
    /// Frequency Domain) the same: its ports' names and whether each is fed
    /// in the device, its S-parameters and ports' modes in the results.
    struct Optics {
        QString physics;
        EmeDevice device;
        std::vector<EmeResult> results;
        bool fullWave = false;
    };
    std::vector<Optics> optics;

    /// The scope of point expressions: x, y, z, t, dom, the fields'
    /// variables.
    const Scope& pointScope() const { return *a_pointScope; }
    /// That and the globals: a plot's or a derived value's.
    const Scope& scope() const { return *a_globalScope; }
    QList<Variable> variables() const;
    QList<Variable> globals() const;
    void setGlobal(const QString& name, double value, const Dim& dim, const QString& description = {});
    std::optional<double> global(const QString& name) const;
    /// A global as it is shown, in its unit.
    QString globalText(const QString& name, int digits = 4) const;

    /// Each physics' coefficient - by domain its tensor (row by row, xx, xy,
    /// xz, yx...; invalid: not solved there) and whether it is isotropic -,
    /// and its sources by domain, each scaled (a total power spread).
    struct Coefficients {
        std::vector<std::array<Expression, 9>> c;
        std::vector<char> isotropic;
        std::vector<std::vector<Expression>> sources;
        std::vector<double> sourceScale;
        /// By domain: what multiplies ∂u/∂t (heat: ρ Cp; invalid: not given).
        std::vector<Expression> capacity;
    };
    std::vector<Coefficients>& coefficients() { return a_coefficients; }
    const std::vector<Coefficients>& coefficients() const { return a_coefficients; }

    /// How each variable is found at a point (PointEvaluator3), by slot.
    struct VariableDef {
        enum Kind { X, Y, Z, Time, DomainNumber, Value, Gradient, GradNorm, Coefficient, Derived };
        Variable info;
        Kind kind = X;
        int field = -1;
        int component = 0;     ///< a gradient's axis; a coefficient's tensor entry (-1: its diagonal's mean)
        double sign = 1;       ///< a gradient's (E is -∇V)
        std::function<double(PointEvaluator3&)> derived;
    };
    const std::vector<VariableDef>& definitions() const { return a_defs; }
    /// The variables of \a model's fields as solved here (and x, y, z, t,
    /// dom): made once its fields are, before its problems compile what they
    /// read.
    void defineVariables();
    /// Whether \a e may change as the fields do (it reads one, or t).
    bool varies(const Expression& e) const;
    /// Whether \a e reads field \a f.
    bool reads(const Expression& e, int f) const;

    QStringList log;
    QString studyTag;
    double seconds = 0;
    /// Of a sweep: the parameters' values this solution is of ("w = 2 mm"),
    /// and each in SI by name.
    QString label;
    QHash<QString, double> sweptValues;

private:
    int define(const QString& name, const Dim& dim, const QString& description, VariableDef::Kind kind, int field, int component = 0,
               double sign = 1, std::function<double(PointEvaluator3&)> derived = {});

    Model a_model;   // (a copy: the window's changes after)
    std::shared_ptr<ParameterScope> a_params;
    std::shared_ptr<const Mesh3> a_mesh;
    std::vector<Field3> a_fields;
    std::vector<Coefficients> a_coefficients;
    double a_time = 0;
    double a_frequency = 0;
    std::unique_ptr<Scope> a_pointScope, a_globalScope;
    std::vector<VariableDef> a_defs;
    struct Global {
        double value = 0;
        Dim dim;
        QString description;
    };
    std::map<QString, Global> a_globals;
    QStringList a_globalOrder;
    struct Snapshot {
        double time = 0;
        double frequency = 0;
        std::vector<std::vector<double>> values;                  // by field
        std::vector<std::vector<Field3::Terminal>> terminals;     // by field
        std::vector<std::vector<std::vector<double>>> matrices;   // by field
        std::map<QString, Global> globals;
    };
    Snapshot snapshotNow() const;
    std::vector<Snapshot> a_snapshots;
    int a_selected = -1;
};

/*!
 * A solution read at one point: its variables by slot, as Expression::eval
 * wants them, each found when first asked - a field's value or gradient by
 * its prism's shape functions there, a coefficient by its domain's
 * expression. NaN where it is not there (a field not solved in that
 * domain).
 */
class PointEvaluator3
{
public:
    explicit PointEvaluator3(const Solution3& solution);
    ~PointEvaluator3();

    /// At reference point (ξ, η, ζ) of prism \a e.
    void at(int e, double xi, double eta, double zeta);
    /// The same, its point \a x (m) as the caller's space maps it.
    void at(int e, double xi, double eta, double zeta, const Vec3& x);
    /// At point \a x of the model (m); false where no prism holds it.
    bool at(const Vec3& x);
    const Solution3& solution() const { return a_s; }
    int prism() const { return a_prism; }
    int domain() const { return a_domain; }
    Vec3 point() const { return a_x; }
    double value(int slot);
    /// \a e's value here (NaN where it reads what is not here).
    double eval(const Expression& e);
    /// Field \a f's value here and its gradient (m^-1 times its unit); false
    /// where it is not solved here.
    bool field(int f, double& u, double g[3]);
    /// Field \a f's value here taken as \a du more (0: as it is), every value
    /// read of it found again: a coefficient's derivative in its field, as
    /// Newton's method takes it.
    void perturb(int f, double du);

private:
    const Solution3& a_s;
    int a_perturbed = -1;
    double a_perturbation = 0;
    int a_prism = -1, a_domain = -1;
    double a_xi = 0, a_eta = 0, a_zeta = 0;
    Vec3 a_x;
    std::vector<double> a_values;
    std::vector<char> a_known;   // 0 not yet, 1 known, 2 being found (a loop: NaN)
    std::vector<double> a_scratch;
    std::unique_ptr<Locator> a_locator;
};

/// What a 3D solve is asked beyond its model: its linear solver for this
/// solve alone, and the memory its estimate is held to.
struct SolveAsked3 {
    QString solver;       ///< auto, direct or iterative (empty: each step's own)
    double limit = 0;     ///< bytes: a study estimated past it is not solved (0: no limit)
    bool force = false;   ///< solved past the limit all the same, said
};

/// A 3D study solved: its solution, or why not; what it was estimated to
/// hold and take, each physics', before it began.
struct StudyResult3 {
    std::shared_ptr<Solution3> solution;
    QString error;
    QStringList warnings;
    QStringList log;
    std::vector<Estimate3> estimates;
};

/// Solves study \a studyTag (the first when empty) of 3D \a model on \a mesh:
/// its Stationary steps, each its physics (all when none chosen) in turn till
/// they agree, and its Time Dependent ones - heat by BDF (its ρ Cp ∂T/∂t),
/// the other physics at rest at each step, kept at the output times; a step
/// of another kind is refused, said. Each physics' estimate is said in its
/// log first; past \a asked's limit it is not solved unless forced, and what
/// would bring it under is said.
StudyResult3 solveStudy3(const Model& model, std::shared_ptr<ParameterScope> parameters, std::shared_ptr<const Mesh3> mesh,
                         const QString& studyTag = {}, const Progress& progress = {}, const SolveAsked3& asked = {});

/*!
 * A 3D study run whole: once, or once for each point of its Parametric
 * Sweep - each with its own parameters, and its own geometry and mesh where
 * a parameter swept shapes them -, one point after another. The solutions in
 * the sweep's order; a point without one after the first stops the sweep
 * there, its points before kept, where and why said.
 */
struct StudyRun3 {
    std::vector<std::shared_ptr<Solution3>> solutions;
    QStringList swept;                     ///< the parameters swept, in order
    std::shared_ptr<const Mesh3> mesh;     ///< the mesh of the model's own parameters, when made or given
    QString error;
    QStringList warnings;
    QStringList log;
    QString stopped;
    std::vector<Estimate3> estimates;      ///< its first point's (or its own)
    bool ok() const { return error.isEmpty() && !solutions.empty(); }
};
/// Runs study \a studyTag (the first when empty): \a topology and \a mesh
/// are of the model's own parameters (null: made); each point solved as \a
/// asked.
StudyRun3 runStudy3(const Model& model, std::shared_ptr<ParameterScope> parameters, std::shared_ptr<const Topology3> topology,
                    std::shared_ptr<const Mesh3> mesh, const QString& studyTag = {}, const Progress& progress = {},
                    const SolveAsked3& asked = {});

/// The variables a 3D model's physics give its expressions (x, y, z, t, dom,
/// es.V, ht.T...): for a menu, a completer and Claude.
QList<Variable> modelVariables3(const Model& model);

/// The parts a 3D assembly's elements are made in, each on a thread of its
/// own and joined in the elements' order (0, as it is unless set: one for
/// each 2,000 elements, 64 at most; 1: one, the calling thread's) - for a
/// test of the matrix's being the same, bit for bit, whatever the parts.
void setAssemblyParts(int parts);
int assemblyParts();

/// The physics a 3D model solves (heat, electrostatics, currents): those of
/// its enabled physics that are not, by type.
QStringList unsolvedPhysics3(const Model& model);

} // namespace qucs_s::fem

#endif // QUCS_FEM3_SOLVER_H
