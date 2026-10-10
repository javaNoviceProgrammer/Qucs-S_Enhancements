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
#include <map>
#include <memory>
#include <vector>

namespace qucs_s::fem {

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
        double potential = 0;     ///< V
        double charge = 0;        ///< C (es) or A (ec): over the out-of-plane thickness (all round the axis)
    };
    std::vector<Terminal> terminals;
    /// The terminals' matrix: each at 1 V in turn, the others at 0 V - a
    /// capacitance (es, F) or conductance (ec, S) matrix. Empty: no sweep.
    std::vector<std::vector<double>> matrix;
    double initial = 0;           ///< its value before it is solved
    bool solved = false;

    int count() const { return int(components.size()); }
    double value(int component, int dof) const { return values[std::size_t(component) * std::size_t(space->size()) + std::size_t(dof)]; }
};

/// A variable an expression may use where a solution is evaluated.
struct Variable {
    QString name;
    Dim dim;
    QString description;
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

    /// The scope of point expressions (x, y, t, the fields' variables).
    const Scope& pointScope() const { return *a_pointScope; }
    /// That and the globals: what a plot's or a derived value's
    /// expression is compiled in.
    const Scope& scope() const { return *a_globalScope; }
    /// The variables and the globals, for a menu and for Claude.
    QList<Variable> variables() const;
    QList<Variable> globals() const;
    void setGlobal(const QString& name, double value, const Dim& dim, const QString& description = {});
    std::optional<double> global(const QString& name) const;

    /// Each physics' coefficient expressions, by domain (compiled in the
    /// point scope): what es.epsr, ec.sigma, ht.k, solid.E read.
    struct Coefficients {
        std::vector<Expression> c;        // by domain (invalid: not solved there)
        std::vector<std::vector<Expression>> sources;   // by domain
        std::vector<double> sourceScale;  // by domain: a total power spread
        std::vector<Expression> capacity; // by domain: what multiplies du/dt (heat: rho Cp)
        std::map<QString, std::vector<Expression>> named;   // others, by name (solid: E, nu, alpha, dT)
    };
    std::vector<Coefficients>& coefficients() { return a_coefficients; }
    const std::vector<Coefficients>& coefficients() const { return a_coefficients; }

    // Variables by slot: how each is found at a point (PointEvaluator).
    struct VariableDef {
        enum Kind { X, Y, Time, DomainNumber, Size, Value, GradX, GradY, GradNorm, Coefficient, Derived };
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

    // Snapshots: the fields and globals at the output times.
    int snapshotCount() const { return int(a_snapshots.size()); }
    double snapshotTime(int i) const { return a_snapshots[std::size_t(i)].time; }
    int selected() const { return a_selected; }
    /// The fields and globals of snapshot \a i (out of range: the last).
    void select(int i);
    /// The fields and globals as they are now, kept as a snapshot at time().
    void addSnapshot();
    /// The snapshot nearest time \a t (s), or -1 when there is none.
    int snapshotAt(double t) const;

    QStringList log;
    QString studyTag;
    double seconds = 0;
    /// Of a sweep: the parameters' values this solution is of ("w = 2 mm").
    QString label;
    QHash<QString, double> sweptValues;   ///< SI, by parameter
    bool timeDependent = false;

private:
    void defineVariables();
    int define(const QString& name, const Dim& dim, const QString& description, VariableDef::Kind kind, int field = -1,
               std::function<double(PointEvaluator&)> derived = {});

    Model a_model;
    std::shared_ptr<ParameterScope> a_params;
    std::shared_ptr<const Mesh> a_mesh;
    std::vector<Field> a_fields;
    std::vector<Coefficients> a_coefficients;
    double a_thickness = 1;
    bool a_axisymmetric = false;
    double a_time = 0;
    std::unique_ptr<Scope> a_pointScope;
    std::unique_ptr<Scope> a_globalScope;
    std::vector<VariableDef> a_defs;
    QList<Variable> a_globals;
    struct Snapshot {
        double time = 0;
        std::vector<std::vector<double>> values;   // by field
        std::vector<std::vector<Field::Terminal>> terminals;
        std::vector<double> globals;               // by a_globals' order
    };
    std::vector<Snapshot> a_snapshots;
    int a_selected = -1;
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

private:
    const Solution& a_s;
    int a_t = -1;
    double a_xi = 0, a_eta = 0;
    std::vector<double> a_slots;
    std::vector<char> a_done;
    struct FieldCache {
        bool done = false;
        double u[2] = {0, 0}, gx[2] = {0, 0}, gy[2] = {0, 0};
    };
    std::vector<FieldCache> a_fields;
    std::vector<double> a_offset;
    bool a_mapped = false;
    QPointF a_x;
    int a_depth = 0;
};

/// The variables a solution of \a model would have (x, y, V, es.normE,
/// ht.qx, solid.mises...): to check an expression before it is solved.
QList<Variable> modelVariables(const Model& model);

/// A study solved on one mesh with one set of parameters, or why not.
struct StudyResult {
    std::shared_ptr<Solution> solution;
    QString error;
    QStringList warnings;
};

/// Solves the study \a studyTag of \a model on \a mesh (built from its
/// geometry) - its steps, not its sweep. \a progress is told how far;
/// false from it cancels.
StudyResult solveStudy(const Model& model, std::shared_ptr<ParameterScope> parameters, std::shared_ptr<const Mesh> mesh,
                       const QString& studyTag, const Progress& progress = {});

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
    bool ok() const { return error.isEmpty() && !solutions.empty(); }
};
/// Runs study \a studyTag: \a topology and \a mesh are of the model's own
/// parameters (a null mesh is made).
StudyRun runStudy(const Model& model, std::shared_ptr<ParameterScope> parameters, std::shared_ptr<const Topology> topology,
                  std::shared_ptr<const Mesh> mesh, const QString& studyTag, const Progress& progress = {});

/// A list of values as a sweep or a time step gives them: "1, 2, 5",
/// "range(0, 0.1, 1)" (start, step, stop), "linspace(0, 1, 11)", with units
/// (2[mm], range(0[s], 10[ms], 1[s])). SI values; their unit in \a dim; an
/// error in \a error.
std::optional<std::vector<double>> valueList(const QString& text, const Scope& scope, Dim* dim = nullptr, QString* error = nullptr);

/// The capacitance of terminal \a terminal of the electrostatics \a tag
/// in \a solution (F, over the thickness) with every relative permittivity
/// 1: the C0 of a line's quasi-TEM parameters. NaN and \a error on failure.
double vacuumCapacitance(const Solution& solution, const QString& tag, const QString& terminal, QString* error = nullptr);

} // namespace qucs_s::fem

#endif // QUCS_FEM_SOLVER_H
