/*
 * fem_solver.h - the multiphysics solver: each physics of a model as the
 *                equation it is, -∇·(c ∇u) = f with its conditions on the
 *                boundaries, assembled in parallel, solved by a sparse
 *                Cholesky (or conjugate gradients), its terminals swept
 *                for a capacitance or conductance matrix; physics that
 *                read each other's fields solved in turn until they agree
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
#include <memory>
#include <vector>

namespace qucs_s::fem {

/// One physics solved: its field on its space, and what was found.
struct Field {
    QString tag;           ///< es, ec, ht
    QString type;          ///< electrostatics, currents, heat
    QString variable;      ///< its dependent variable's name: V, T
    Dim dim;
    std::shared_ptr<const Space> space;
    std::vector<double> values;   ///< by degree of freedom, SI (NaN where it is not solved)
    std::vector<char> domains;    ///< by domain: solved there
    struct Terminal {
        QString name;
        bool floating = false;    ///< of a charge (current) given, its potential found
        double potential = 0;     ///< V
        double charge = 0;        ///< C (es) or A (ec): over the out-of-plane thickness
    };
    std::vector<Terminal> terminals;
    /// The terminals' matrix: each at 1 V in turn, the others at 0 V - a
    /// capacitance (es, F) or conductance (ec, S) matrix. Empty: no sweep.
    std::vector<std::vector<double>> matrix;
    double initial = 0;           ///< its value before it is solved
    bool solved = false;
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
 * an expression of it is compiled in: x, y, the fields and what follows
 * from them (es.normE, ec.Jx, ht.qy...), the parameters, the globals.
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
    std::vector<Field>& fields() { return a_fields; }
    const std::vector<Field>& fields() const { return a_fields; }
    int fieldIndex(const QString& tag) const;
    double thickness() const { return a_thickness; }

    /// The scope of point expressions (x, y, the fields' variables).
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
    /// point scope): what es.epsr, ec.sigma, ht.k read.
    struct Coefficients {
        std::vector<Expression> c;        // by domain (invalid: not solved there)
        std::vector<std::vector<Expression>> sources;   // by domain
        std::vector<double> sourceScale;  // by domain: a total power spread
    };
    std::vector<Coefficients>& coefficients() { return a_coefficients; }
    const std::vector<Coefficients>& coefficients() const { return a_coefficients; }

    // Variables by slot: how each is found at a point (PointEvaluator).
    struct VariableDef {
        enum Kind { X, Y, DomainNumber, Size, Value, GradX, GradY, GradNorm, Coefficient, Derived };
        Variable info;
        Kind kind = X;
        int field = -1;
        std::function<double(PointEvaluator&)> derived;
    };
    const std::vector<VariableDef>& definitions() const { return a_defs; }

    QStringList log;
    QString studyTag;
    double seconds = 0;

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
    std::unique_ptr<Scope> a_pointScope;
    std::unique_ptr<Scope> a_globalScope;
    std::vector<VariableDef> a_defs;
    QList<Variable> a_globals;
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
    /// A field's value and gradient here.
    void field(int f, double& u, double& gx, double& gy);
    QPointF point();   ///< (m)

private:
    const Solution& a_s;
    int a_t = -1;
    double a_xi = 0, a_eta = 0;
    std::vector<double> a_slots;
    std::vector<char> a_done;
    struct FieldCache {
        bool done = false;
        double u = 0, gx = 0, gy = 0;
    };
    std::vector<FieldCache> a_fields;
    bool a_mapped = false;
    QPointF a_x;
    int a_depth = 0;
};

/// The variables a solution of \a model would have (x, y, V, es.normE,
/// ht.qx...): to check an expression before it is solved.
QList<Variable> modelVariables(const Model& model);

/// A study solved, or why not.
struct StudyResult {
    std::shared_ptr<Solution> solution;
    QString error;
    QStringList warnings;
};

/// Solves the study \a studyTag of \a model on \a mesh (built from its
/// geometry). \a progress is told how far; false from it cancels.
StudyResult solveStudy(const Model& model, std::shared_ptr<ParameterScope> parameters, std::shared_ptr<const Mesh> mesh,
                       const QString& studyTag, const Progress& progress = {});

/// The capacitance of terminal \a terminal of the electrostatics \a tag
/// in \a solution (F, over the thickness) with every relative permittivity
/// 1: the C0 of a line's quasi-TEM parameters. NaN and \a error on failure.
double vacuumCapacitance(const Solution& solution, const QString& tag, const QString& terminal, QString* error = nullptr);

} // namespace qucs_s::fem

#endif // QUCS_FEM_SOLVER_H
