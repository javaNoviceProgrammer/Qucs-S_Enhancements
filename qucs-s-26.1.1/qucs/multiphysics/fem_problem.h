/*
 * fem_problem.h - what the solver's physics have in common (not installed:
 *                 the solver's own): a sparse system of equations, and a
 *                 physics as a problem - set up from the model, solved at
 *                 the solution's time, its globals set
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_PROBLEM_H
#define QUCS_FEM_PROBLEM_H

#include "fem_solver.h"

#include <Eigen/IterativeLinearSolvers>
#include <Eigen/SparseCholesky>
#include <Eigen/SparseCore>
#include <Eigen/SparseLU>

#include <QSet>

#include <functional>
#include <limits>
#include <memory>

namespace qucs_s::fem::detail {

QString tr(const char* text);
constexpr double Eps0 = 8.8541878188e-12;
inline const double NaN = std::numeric_limits<double>::quiet_NaN();

/// Runs \a work(begin, end) on parts of [0, n) in threads.
void parallelFor(int n, const std::function<void(int, int)>& work);

/*!
 * A sparse system A x = b: its pattern from the elements (the equations
 * each couples), its values added element by element, factorized - by a
 * Cholesky when it is symmetric and positive definite, else by LU, or by
 * conjugate gradients when large - and solved for as many right-hand sides
 * as wanted. The pattern's analysis is kept while the pattern is.
 */
class LinearSystem
{
public:
    /// The equations: \a neq of them; \a elements called with a function
    /// each element gives its equations to (negative ones left out).
    void setPattern(int neq, const std::function<void(const std::function<void(const int*, int)>&)>& elements);
    int size() const { return a_n; }
    /// Every value 0 (the pattern kept).
    void clear();
    void add(int i, int j, double v);
    /// \a symmetric: the matrix is (a Cholesky is tried). \a solver auto,
    /// direct or iterative. Its name for messages.
    bool factorize(bool symmetric, const QString& solver, double tolerance, const QString& name, QStringList* log, QString* error);
    bool solve(const Eigen::VectorXd& rhs, Eigen::VectorXd& x, const QString& name, QString* error) const;
    const Eigen::SparseMatrix<double>& matrix() const { return a_A; }

private:
    int a_n = 0;
    Eigen::SparseMatrix<double> a_A;
    std::unique_ptr<Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>>> a_ldlt;
    std::unique_ptr<Eigen::SparseLU<Eigen::SparseMatrix<double>>> a_lu;
    std::unique_ptr<Eigen::ConjugateGradient<Eigen::SparseMatrix<double>, Eigen::Lower | Eigen::Upper,
                                             Eigen::IncompleteCholesky<double>>> a_cg;
    bool a_ldltAnalyzed = false, a_luAnalyzed = false;
};

/// How a physics is solved, once.
struct SolveOptions {
    QString solver = QStringLiteral("auto");      ///< auto, direct, iterative
    double tolerance = 1e-6;                      ///< of Newton's steps (and of an iterative solver)
    QString nonlinear = QStringLiteral("auto");   ///< auto (Newton for what reads its own field), newton, picard
    int maxIterations = 50;
    bool sweep = false;                           ///< the terminals swept (a matrix)
    /// A step in time: mass * M u - M history is added to the equations
    /// (history by degree of freedom; mass 0: at rest).
    double mass = 0;
    const std::vector<double>* history = nullptr;
};

/*!
 * One physics of a solution as the problem it is: set up from the model
 * (its domains, materials, conditions - what is wrong said), solved at the
 * solution's time into its field, its globals set.
 */
class Problem
{
public:
    Problem(Solution& solution, int field);
    virtual ~Problem() = default;

    /// Reads the model; false and why (every problem found) in \a error.
    virtual bool setup(QString* error) = 0;
    /// Its field's initial values (before a step in time, or as Newton's
    /// first guess).
    virtual void initialize() {}
    /// Assembles and solves; the field's values set.
    virtual bool solve(const SolveOptions& options, QString* error) = 0;
    /// Its globals (es.C11, ht.Tmax, solid.W...) from the field as solved.
    virtual void setGlobals() = 0;
    /// Whether it has a time derivative (heat), for a step in time.
    virtual bool hasMass() const { return false; }
    /// Whether what it reads (its coefficients, sources, conditions) reads
    /// field \a f.
    bool reads(int f) const { return a_reads.contains(f); }
    int index() const { return a_fi; }
    Field& field() { return a_s.fields()[std::size_t(a_fi)]; }
    const Node* node() const { return a_node; }

protected:
    void problem(const QString& what) { a_problems << what; }
    /// \a text of property \a key of \a feature compiled in the point
    /// scope, what it reads noted; a field read where \a spatial is not, an
    /// error.
    Expression compile(const QString& text, const Node& feature, const QString& key, bool spatial = true);
    /// The material of each domain: the last that has it (null: none).
    std::vector<const Node*> materials();
    /// Property \a key of material \a m: its own, else its library entry's.
    static QString materialProperty(const Node& m, const QString& key);

    Solution& a_s;
    int a_fi;
    const Node* a_node = nullptr;
    QSet<int> a_reads;
    QStringList a_problems;
};

/// The problem of field \a field of \a solution (by its type).
std::unique_ptr<Problem> makeProblem(Solution& solution, int field);

/// Electrostatics' C0 (every εr 1) for a line's parameters.
double scalarVacuumCapacitance(Solution& solution, int field, const QString& terminal, QString* error);

} // namespace qucs_s::fem::detail

#endif // QUCS_FEM_PROBLEM_H
