/*
 * fem_solver.h - the multiphysics solver: each physics of a model as the
 *                equation it is - -∇·(c ∇u) = f with its conditions on the
 *                boundaries, or linear elasticity - assembled in parallel,
 *                solved by a sparse Cholesky (or LU, or conjugate
 *                gradients), its terminals swept for a capacitance or
 *                conductance matrix; physics that read each other's fields
 *                solved in turn until they agree, one that reads its own by
 *                Newton's method; heat in time by BDF; a study's sweep of
 *                parameters, its geometry and mesh made again where they
 *                change; in the plane or about an axis (2D axisymmetric)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_SOLVER_H
#define QUCS_FEM_SOLVER_H

#include "fem_fe.h"

#include <QHash>
#include <QMap>

#include <atomic>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <vector>

namespace qucs_s::fem {

class EdgeSpace;
class Solution;

/// Electrostatics' terminals at 1 V in turn (terminalMatrices): the charges
/// C, the dielectrics' loss integrals Gd (per ω) and Gs; by terminal.
struct TerminalMatrices {
    std::vector<std::vector<double>> C, Gd, Gs;
};

/// One physics solved: its field on its space, and what was found.
struct Field {
    QString tag;           ///< es, ec, ht, solid
    QString type;          ///< electrostatics, currents, heat, solid
    QString variable;      ///< its dependent variable's name: V, T (solid: u)
    QStringList components;   ///< each component's name: V; u, v (a displacement)
    Dim dim;
    std::shared_ptr<const Space> space;
    /// By degree of freedom, SI; a vector field's components one after the
    /// other: component c of dof i at c * space->size() + i.
    std::vector<double> values;
    std::vector<char> domains;    ///< by domain: solved there
    struct Terminal {
        QString name;
        bool floating = false;    ///< of a charge (current) given, its potential found
        double potential = 0;     ///< V (a magnetic field's conductor: its voltage, V/m in the plane; about an axis V)
        double charge = 0;        ///< C (es) or A (ec, mf): over the out-of-plane thickness (all round the axis)
        /// In the frequency domain, their imaginary parts.
        double potentialIm = 0, chargeIm = 0;
    };
    std::vector<Terminal> terminals;
    /// The terminals' matrix: each at 1 V in turn, the others at 0 V - a
    /// capacitance (es, F) or conductance (ec, S) matrix. Empty: no sweep.
    std::vector<std::vector<double>> matrix;
    double initial = 0;           ///< its value before it is solved
    bool solved = false;
    /// Solved elsewhere - a 1D semiconductor on its line's strip -: the
    /// unknowns it took there (-1: its space's), and the strip as solved (a
    /// sweep's next point starts from it).
    int unknowns = -1;
    std::shared_ptr<const Solution> line;
    /// A mode's tangential field on edge elements (its values first in
    /// values, Ez's after them); null for a field of nodes alone.
    std::shared_ptr<const EdgeSpace> edges;
    /// Numbers that go with the field and its snapshots (a mode's β).
    std::vector<double> extra;
    /// Numbers that go with the field, the same in all its snapshots (a
    /// quantum wells physics' bands).
    std::shared_ptr<const std::vector<double>> fixed;
    /// A semiconductor's way to its contacts' voltages, its last step: its
    /// unknowns a step before the solution and its contacts' voltages there
    /// - a sweep's next point starts on their line (empty: no step).
    std::shared_ptr<const std::vector<double>> walkedFrom;
    std::vector<double> walkedFromVolts;
    double walkedStep = 0;   ///< that step's length (V, the contact moved most)

    int count() const { return int(components.size()); }
    double value(int component, int dof) const { return values[std::size_t(component) * std::size_t(space->size()) + std::size_t(dof)]; }
};

/*!
 * A magnetic material's B-H curve: H of B through points (B, H), monotone
 * between them (Fritsch and Carlson's cubic, as PCHIP), beyond the last as
 * in vacuum (dH/dB = 1/µ0); its energy density ∫ H dB.
 */
class BHCurve
{
public:
    /// From points (T, A/m), B rising; (0, 0) added when not given. Null,
    /// and why in \a error, when they do not rise or do not start at 0.
    static std::shared_ptr<const BHCurve> make(std::vector<std::pair<double, double>> points, QString* error = nullptr);
    double H(double b) const;
    double slope(double b) const;    ///< dH/dB
    double nu(double b) const;       ///< H/B (its first slope at 0): the reluctivity
    double energy(double b) const;   ///< ∫ H dB from 0 to b (J/m³)
    const std::vector<double>& B() const { return a_b; }
    const std::vector<double>& Hs() const { return a_h; }

private:
    int segment(double b) const;
    std::vector<double> a_b, a_h, a_m, a_w;   // the points, H's slopes there, the energy to them
};

/// A variable an expression may use where a solution is evaluated.
struct Variable {
    QString name;
    Dim dim;
    QString description;
    QString unit;   ///< as shown, where its dimension cannot say it (dB/m); empty: its dimension's
    QString display;   ///< shown in this unit of its dimension, converted (eV, 1/nm); empty: SI's
};

class PointEvaluator;

/*!
 * A study's solution: the model as it was solved, its mesh, each physics'
 * field, the global values found (es.C11, ec.R, ht.Tmax...) and the scope
 * an expression of it is compiled in: x, y (r, z about an axis), t, the
 * fields and what follows from them (es.normE, ec.Jx, ht.qy, solid.mises...),
 * the parameters, the globals. A time-dependent study keeps its fields and
 * globals at each output time - snapshots, one of which is selected.
 */
class Solution
{
public:
    Solution(const Model& model, std::shared_ptr<ParameterScope> parameters, std::shared_ptr<const Mesh> mesh);

    const Model& model() const { return a_model; }
    const Mesh& mesh() const { return *a_mesh; }
    std::shared_ptr<const Mesh> meshPtr() const { return a_mesh; }
    const Topology& topology() const { return *a_mesh->topology; }
    const ParameterScope& parameters() const { return *a_params; }
    std::shared_ptr<ParameterScope> parametersPtr() const { return a_params; }
    std::vector<Field>& fields() { return a_fields; }
    const std::vector<Field>& fields() const { return a_fields; }
    int fieldIndex(const QString& tag) const;
    double thickness() const { return a_thickness; }
    /// About the y axis (2D axisymmetric): x is r, y is z, and what is
    /// integrated is integrated round the axis.
    bool axisymmetric() const { return a_axisymmetric; }
    /// What an area (a length) of the plane is worth in volume (area) at
    /// \a x (m): the thickness, or 2 pi r about the axis.
    double weight(QPointF x) const { return a_axisymmetric ? 2 * M_PI * x.x() : a_thickness; }
    /// The time the fields are at (s); 0 for a stationary study.
    double time() const { return a_time; }
    void setTime(double t) { a_time = t; }
    /// The frequency they are at (Hz) in the frequency domain; 0 at rest.
    double frequency() const { return a_frequency; }
    void setFrequency(double f) { a_frequency = f; }

    /// The scope of point expressions (x, y, t, the fields' variables).
    const Scope& pointScope() const { return *a_pointScope; }
    /// That and the globals: what a plot's or a derived value's
    /// expression is compiled in.
    const Scope& scope() const { return *a_globalScope; }
    /// The variables and the globals, for a menu and for Claude.
    QList<Variable> variables() const;
    QList<Variable> globals() const;
    void setGlobal(const QString& name, double value, const Dim& dim, const QString& description = {}, const QString& unit = {});
    /// A process flow's state as variables, each read where it is (the
    /// grid's cells of the domain's material there), \a tag their prefix:
    /// each species all of it (pf1.B) and active (pf1.B_a), the active
    /// donors and acceptors as each host has them (pf1.Nd, pf1.Na) and
    /// their difference (pf1.net), the interstitials (pf1.I, pf1.Icl), a
    /// III-V's sublattices' shares (pf1.x_In...).
    void addProcessVariables(std::shared_ptr<const ProcessState> state, const QString& tag);
    /// A process step's solution: its state and its flow's tag (null else).
    void setProcess(std::shared_ptr<const ProcessState> state, const QString& tag)
    {
        a_process = std::move(state);
        a_processTag = tag;
    }
    std::shared_ptr<const ProcessState> process() const { return a_process; }
    QString processTag() const { return a_processTag; }
    /// A global's value as it is shown: in its unit (dB/m) where it has one.
    QString globalText(const QString& name, int digits = 4) const;
    std::optional<double> global(const QString& name) const;

    /// Each physics' coefficient expressions, by domain (compiled in the
    /// point scope): what es.epsr, ec.sigma, ht.k, solid.E read.
    struct Coefficients {
        std::vector<Expression> c;        // by domain (invalid: not solved there)
        std::vector<std::vector<Expression>> sources;   // by domain
        std::vector<double> sourceScale;  // by domain: a total power spread
        std::vector<Expression> capacity; // by domain: what multiplies du/dt (heat: rho Cp)
        std::map<QString, std::vector<Expression>> named;   // others, by name (solid: E, nu, alpha, dT; mf: sigma, Je, Brx...)
        std::vector<std::shared_ptr<const BHCurve>> curves;  // by domain: a magnetic field's B-H curve (none: linear)
    };
    std::vector<Coefficients>& coefficients() { return a_coefficients; }
    const std::vector<Coefficients>& coefficients() const { return a_coefficients; }

    // Variables by slot: how each is found at a point (PointEvaluator).
    struct VariableDef {
        enum Kind { X, Y, Time, Frequency, DomainNumber, Size, Value, GradX, GradY, GradNorm, Coefficient, Derived };
        Variable info;
        Kind kind = X;
        int field = -1;
        int component = 0;
        QString coefficient;   ///< a Coefficient's: empty for c, else one of named
        std::function<double(PointEvaluator&)> derived;
    };
    const std::vector<VariableDef>& definitions() const { return a_defs; }
    /// Whether expression \a e may change as the fields or time do (it reads
    /// a field, or t): a matrix of it is made again at each step.
    bool varies(const Expression& e) const;
    /// Whether \a e reads field \a f.
    bool reads(const Expression& e, int f) const;

    // Snapshots: the fields and globals at the output times, or at the
    // frequencies of a study in the frequency domain.
    int snapshotCount() const { return int(a_snapshots.size()); }
    double snapshotTime(int i) const { return a_snapshots[std::size_t(i)].time; }
    double snapshotFrequency(int i) const { return a_snapshots[std::size_t(i)].frequency; }
    /// A snapshot's time (s) or, in the frequency domain, frequency (Hz).
    double snapshotValue(int i) const { return frequencyDomain ? snapshotFrequency(i) : snapshotTime(i); }
    /// A mode analysis's snapshot: its mode's number (from 1), else 0.
    int snapshotMode(int i) const { return a_snapshots[std::size_t(i)].mode; }
    /// Whether it is the mode its step chose at its frequency (a mode
    /// analysis's: the one shown and evaluated by default).
    bool snapshotChosen(int i) const { return a_snapshots[std::size_t(i)].chosen; }
    /// The last snapshot marked as its step's chosen mode.
    void chooseLastSnapshot()
    {
        if (!a_snapshots.empty()) a_snapshots.back().chosen = true;
    }
    /// Snapshot \a i marked as its step's chosen mode (an Eigenfrequency
    /// step's kept one).
    void chooseSnapshot(int i)
    {
        if (i >= 0 && i < int(a_snapshots.size())) a_snapshots[std::size_t(i)].chosen = true;
    }
    /// The mode the fields are of (from 1; 0: none).
    int mode() const { return a_mode; }
    void setMode(int m) { a_mode = m; }
    /// Its unit (s or Hz), its name in a dataset ("time", "frequency") and
    /// in a label ("t", "f").
    Dim snapshotDim() const { return frequencyDomain ? Dim::of(0, 0, -1) : biasAxis ? Dim::voltage() : Dim::of(0, 0, 1); }
    QString snapshotName() const
    {
        return frequencyDomain ? QStringLiteral("frequency") : biasAxis ? QStringLiteral("voltage") : QStringLiteral("time");
    }
    QString snapshotSymbol() const { return frequencyDomain ? QStringLiteral("f") : biasAxis ? QStringLiteral("V") : QStringLiteral("t"); }
    int selected() const { return a_selected; }
    /// Its fields and globals left those of snapshot \a i, not the last's
    /// (the mode a step keeps for the steps after): the next select()
    /// restores the one asked, a step at rest after it keeps its solution
    /// in \a i (replaceLastSnapshot()).
    void leaveAt(int i)
    {
        a_selected = -1;
        a_live = i;
    }
    /// The fields and globals of snapshot \a i (out of range: the last).
    void select(int i);
    /// \a text, an expression of the globals (and parameters), at each
    /// snapshot - its globals' (none: empty, and why); its dimension in \a
    /// dim. The selected snapshot's as they were after.
    std::vector<double> globalSeries(const QString& text, Dim* dim, QString* why) const;
    /// The fields and globals as they are now, kept as a snapshot at time()
    /// (frequency()).
    void addSnapshot();
    /// The snapshot the fields are of - the last, or the one a step left them
    /// at (leaveAt()) - made again of the fields as they are now (a step at
    /// rest after one in time, frequency or modes). (The last, always, it
    /// was: a mode kept for the steps after was written over the last mode -
    /// the update check of 110e8b9, 1.)
    void replaceLastSnapshot();
    /// No snapshots (a step's own series to come; the fields as they are).
    void clearSnapshots()
    {
        a_snapshots.clear();
        a_selected = -1;
        a_live = -1;
    }
    /// The snapshot nearest time \a t (s), or -1 when there is none.
    int snapshotAt(double t) const;
    /// The snapshot nearest frequency \a f (Hz), or -1.
    int snapshotAtFrequency(double f) const;

    QStringList log;
    /// What terminalMatrices found, by its physics, terminals, vacuum or not
    /// and (not in vacuum) the snapshot and frequency: a line's matrices at
    /// each frequency of a sweep without solving its vacuum again.
    mutable std::map<QString, TerminalMatrices> terminalCache;
    QString studyTag;
    double seconds = 0;
    /// Of a sweep: the parameters' values this solution is of ("w = 2 mm").
    QString label;
    QHash<QString, double> sweptValues;   ///< SI, by parameter
    /// A sweep's points solved at once (its "parallel"; 1: one after
    /// another), and then the whole sweep's time (s) - its points' own
    /// seconds overlapping.
    int atOnce = 1;
    double sweepSeconds = 0;
    /// By mode analysis: the mean effective index of the modes its last
    /// frequency found - where the next frequency, or a sweep's next point,
    /// searches when its step asks for one mode (Follow the mode).
    QHash<QString, double> modeCentres;
    bool timeDependent = false;
    bool frequencyDomain = false;         ///< its snapshots are of frequencies
    bool modeAnalysis = false;            ///< and of modes, at each frequency
    bool modeChosen = false;              ///< its mode step chose a mode (not the first) at each
    bool quantumStates = false;           ///< its modes are quantum states
    bool eigenfrequencies = false;        ///< its modes each at its own frequency (a resonator's)
    bool biasAxis = false;                ///< its snapshots are of a contact's voltage (a Laser L-I step's), kept as their time
    /// A spectrum's snapshots, shown by: 0 their frequency, 1 the photon's
    /// energy, 2 its wavelength in vacuum; 3 the valence bands' in-plane k
    /// (1/m, kept as their frequency).
    int photonAxis = 0;
    /// Snapshot \a i as a label shows it: "t = 1 s", "f = 1 GHz", "E = 1.42 eV",
    /// "λ = 870 nm".
    QString snapshotText(int i) const;

private:
    void defineVariables();
    int define(const QString& name, const Dim& dim, const QString& description, VariableDef::Kind kind, int field = -1,
               std::function<double(PointEvaluator&)> derived = {});

    Model a_model;
    std::shared_ptr<const ProcessState> a_process;
    QString a_processTag;
    std::shared_ptr<ParameterScope> a_params;
    std::shared_ptr<const Mesh> a_mesh;
    std::vector<Field> a_fields;
    std::vector<Coefficients> a_coefficients;
    double a_thickness = 1;
    bool a_axisymmetric = false;
    double a_time = 0;
    double a_frequency = 0;
    int a_mode = 0;
    std::unique_ptr<Scope> a_pointScope;
    std::unique_ptr<Scope> a_globalScope;
    std::vector<VariableDef> a_defs;
    QList<Variable> a_globals;
    struct Snapshot {
        double time = 0;
        double frequency = 0;
        int mode = 0;
        bool chosen = false;
        std::vector<std::vector<double>> extra;    // by field
        std::vector<std::vector<double>> values;   // by field
        std::vector<std::vector<Field::Terminal>> terminals;
        std::vector<double> globals;               // by a_globals' order
    };
    std::vector<Snapshot> a_snapshots;
    int a_selected = -1;
    int a_live = -1;   // the snapshot the fields are of when not the last (leaveAt())
};

/*!
 * Values of a solution at a point of an element: each variable found as
 * an expression asks for it, once a point. One for each thread.
 */
class PointEvaluator
{
public:
    explicit PointEvaluator(const Solution& solution);
    /// To reference point (xi, eta) of triangle \a t.
    void moveTo(int t, double xi, double eta);
    int triangle() const { return a_t; }
    double xi() const { return a_xi; }
    double eta() const { return a_eta; }
    /// The domain of the triangle it is in (-1: none).
    int domain() const;
    const Solution& solution() const { return a_s; }
    /// The value of the variable in slot \a slot here.
    double value(int slot);
    /// \a e here (NaN where a variable it reads is not defined).
    double eval(const Expression& e);
    /// A field's value and gradient here (its first component).
    void field(int f, double& u, double& gx, double& gy) { component(f, 0, u, gx, gy); }
    /// Component \a c of field \a f, and its gradient.
    void component(int f, int c, double& u, double& gx, double& gy);
    QPointF point();   ///< (m)
    /// What a coefficient of the field \a f in the solution is here (empty
    /// name: its c), NaN where it has none.
    double coefficient(int f, const QString& name);
    /// Field \a f's first component taken as \a du more than it is (for a
    /// derivative of what reads it); 0 again: as it is.
    void perturb(int f, double du);
    /// Component \a c of field \a f taken as \a delta more than it is - its
    /// value (\a which 0), its x or y derivative (1, 2; a line's: 1 along it)
    /// - for what reads it to be differentiated; delta 0: as it is.
    void shift(int f, int c, int which, double delta);
    /// The shift asked of that component and derivative (0: none).
    double shiftOf(int f, int c, int which) const
    {
        return f == a_shift.f && c == a_shift.c && which == a_shift.which ? a_shift.delta : 0.0;
    }
    /// The frequency (Hz) expressions here are at - freq, lambda0 - in place
    /// of the solution's (a ray's own); NaN: the solution's.
    void setFrequency(double f);
    double frequency() const;

private:
    const Solution& a_s;
    int a_t = -1;
    double a_xi = 0, a_eta = 0;
    std::vector<double> a_slots;
    std::vector<char> a_done;
    struct FieldCache {
        bool done = false;
        double u[12] = {}, gx[12] = {}, gy[12] = {};   // (up to twelve components: a PDE's variables and their rates)
    };
    std::vector<FieldCache> a_fields;
    std::vector<double> a_offset;
    bool a_mapped = false;
    QPointF a_x;
    int a_depth = 0;
    double a_frequency = std::numeric_limits<double>::quiet_NaN();
    struct Shift {
        int f = -1, c = 0, which = 0;
        double delta = 0;
    } a_shift;
};

/// The variables a solution of \a model would have (x, y, V, es.normE,
/// ht.qx, solid.mises...): to check an expression before it is solved.
QList<Variable> modelVariables(const Model& model);

/// Each triangle's error indicator of field \a field of \a solution
/// (Zienkiewicz and Zhu's: its gradient against one recovered from its
/// neighbours' in its domain, its L2 norm on the triangle); empty for a
/// field of edge elements.
std::vector<double> errorIndicators(const Solution& solution, int field);

/// A study solved on one mesh with one set of parameters, or why not.
struct StudyResult {
    std::shared_ptr<Solution> solution;
    QString error;
    QStringList warnings;
};

/// Solves the study \a studyTag of \a model on \a mesh (built from its
/// geometry) - its steps, not its sweep. \a progress is told how far;
/// false from it cancels. A semiconductor starts where it is in \a start
/// (a sweep's point before, on the same mesh): its contacts' voltages
/// raised from there.
StudyResult solveStudy(const Model& model, std::shared_ptr<ParameterScope> parameters, std::shared_ptr<const Mesh> mesh,
                       const QString& studyTag, const Progress& progress = {}, const Solution* start = nullptr);

/*!
 * A study run whole: once, or once for each point of its Parametric Sweep
 * - each with its own parameters, and its own geometry and mesh where a
 * parameter swept changes them. The solutions in the sweep's order.
 */
struct StudyRun {
    std::vector<std::shared_ptr<Solution>> solutions;
    QStringList swept;                       ///< the parameters swept, in order
    std::shared_ptr<const Mesh> mesh;        ///< the mesh of the model's own parameters, when made or given
    QString error;
    QStringList warnings;
    QStringList log;
    /// A sweep stopped before its last point - cancelled, or a point
    /// without a solution - its points before kept: where and why (empty:
    /// every point solved, or none kept and why in error).
    QString stopped;
    bool ok() const { return error.isEmpty() && !solutions.empty(); }
};
/// Runs study \a studyTag: \a topology and \a mesh are of the model's own
/// parameters (a null mesh is made). \a resume: a sweep's points kept when
/// it stopped, the model as it was since - on from the next point (where
/// they are its first points).
StudyRun runStudy(const Model& model, std::shared_ptr<ParameterScope> parameters, std::shared_ptr<const Topology> topology,
                  std::shared_ptr<const Mesh> mesh, const QString& studyTag, const Progress& progress = {},
                  const std::vector<std::shared_ptr<Solution>>& resume = {});

/// A list of values as a sweep or a time step gives them: "1, 2, 5",
/// "range(0, 0.1, 1)" (start, step, stop), "linspace(0, 1, 11)", with units
/// (2[mm], range(0[s], 10[ms], 1[s])). SI values; their unit in \a dim; an
/// error in \a error.
std::optional<std::vector<double>> valueList(const QString& text, const Scope& scope, Dim* dim = nullptr, QString* error = nullptr);

/// Electrostatics' terminals as a coupled line's: each of \a terminals at
/// 1 V in turn, the others and every fixed value at 0, no sources (one
/// factorization) - the charges, C (F, over the thickness: C_ij the charge
/// on i of j's volt); its dielectrics' losses, ∫ ∇φ_i·ε0 εr tan δ ∇φ_j (Gd,
/// G's per ω) and ∫ ∇φ_i·σ ∇φ_j (Gs, S); with every εr 1 (\a vacuum) C
/// alone, the line's C0 (TerminalMatrices). Kept in the solution's
/// terminalCache: C0 once, the others at each snapshot. False and why in
/// \a error when it cannot.
bool terminalMatrices(const Solution& solution, const QString& tag, const QStringList& terminals, bool vacuum, TerminalMatrices* out,
                      QString* error = nullptr);

/// For the tests alone: the semiconductor's Jacobian made inexact on purpose
/// - band-to-band tunnelling's derivative by the field dropped -, its
/// equations as they are, so that what Newton's method accepts is checked
/// against a Jacobian not their own. Off unless a test sets it.
void setSemiconductorJacobianFault(bool dropTunnellingField);

} // namespace qucs_s::fem

#endif // QUCS_FEM_SOLVER_H
