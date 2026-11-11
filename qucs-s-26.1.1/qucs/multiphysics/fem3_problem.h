/*
 * fem3_problem.h - what the 3D solver's physics have in common (not
 *                  installed: the solver's own): a physics as a problem on a
 *                  3D solution, and a symmetric system solved by multigrid or
 *                  a Cholesky
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM3_PROBLEM_H
#define QUCS_FEM3_PROBLEM_H

#include "fem3_solver.h"
#include "fem_amg.h"
#include "fem_sparse.h"
#include "fem_tensor.h"

#include <QSet>

#include <complex>
#include <functional>
#include <memory>

namespace qucs_s::fem::detail3 {

QString tr(const char* text);

/// How a 3D physics is solved, once.
struct Options3 {
    QString solver = QStringLiteral("auto");   ///< auto, direct or iterative
    double tolerance = 1e-6;                   ///< its nonlinear iterations', and (to 1e-9 at most) the iterative solve's
    int maxIterations = 50;                    ///< its nonlinear iterations
    bool sweep = true;                         ///< the terminals' matrix
    /// A step in time's: what multiplies the capacity's ∂u/∂t of u (1/h,
    /// BDF2's (1 + 2ω)/((1 + ω)h)), and the history it is taken from by dof
    /// - C (mass u - history) + K u = F. 0: at rest.
    double mass = 0;
    const std::vector<double>* history = nullptr;
    /// A physics that reads its own field: by Newton's method (auto, newton)
    /// or Picard's iterations (picard).
    QString nonlinear = QStringLiteral("auto");
    std::function<bool()> cancelled;
};

/*!
 * A symmetric positive definite system solved for as many right-hand sides
 * as wanted: by conjugate gradients preconditioned by algebraic multigrid
 * (AmgSolver) - or, asked for, or (auto) on macOS where more right-hand sides
 * than about 6 + n/20,000 share a matrix of no more than 300,000 unknowns, by
 * Accelerate's Cholesky in METIS's order (an iterative solve is quicker for
 * one right-hand side at every size measured). Off macOS, the direct solve
 * asked for is Eigen's. What it did is said in the log.
 */
class System3
{
public:
    /// \a a factorized, or set up for its iterations; \a symmetric false (a
    /// Newton's matrix): by an LU (direct) or BiCGStab with multigrid.
    bool factorize(const Eigen::SparseMatrix<double>& a, const QString& solver, int rhs, double tolerance, const QString& name,
                   QStringList* log, QString* error, bool symmetric = true);
    /// Solves for \a b, from \a x as it is (an iterative solve's guess).
    bool solve(const Eigen::VectorXd& b, Eigen::VectorXd& x, const QString& name, QStringList* log, QString* error);
    bool direct() const { return a_chol != nullptr || a_lu != nullptr; }
    /// Multigrid's near null space (AmgSolver::setNearNullspace), before
    /// factorize().
    void setNearNullspace(int cols, std::vector<double> b, int blockSize = 1)
    {
        a_nullCols = cols;
        a_nullB = std::move(b);
        a_blockSize = blockSize;
    }
    int size() const { return a_n; }

private:
    std::unique_ptr<AmgSolver> a_amg;
    std::unique_ptr<SparseCholesky> a_chol;
    std::unique_ptr<SparseLu<double>> a_lu;
    bool a_symmetric = true;
    int a_n = 0;
    int a_nullCols = 0;
    std::vector<double> a_nullB;
    int a_blockSize = 1;
    double a_tolerance = 1e-9;
};

class Problem3
{
public:
    Problem3(Solution3& solution, int field);
    virtual ~Problem3() = default;
    /// Reads the model; false and why (every problem found) in \a error.
    virtual bool setup(QString* error) = 0;
    /// Its field's initial values.
    virtual void initialize() {}
    /// Assembles and solves (one that reads its own field: Picard's
    /// iterations); false and why.
    virtual bool solve(const Options3& options, QString* error) = 0;
    /// Its globals from the field as solved.
    virtual void setGlobals() = 0;
    /// Whether its field changes in time (heat: ρ Cp ∂T/∂t), or is at rest
    /// at each step (electrostatics, currents).
    virtual bool hasMass() const { return false; }
    /// Whether a step in time starts from its solution at rest at its first
    /// time (a semiconductor's), not from its field as it is.
    virtual bool solvedAtRestFirst() const { return false; }
    /// Its \a count eigenfrequencies nearest \a shift (Hz) and their modes
    /// (its field's values, each scaled to a largest value of 1), lowest
    /// first - a solid's, K x = ω² M x; false and why (one that has none).
    virtual bool eigenmodes(const Options3& options, int count, double shift, std::vector<double>* frequencies,
                            std::vector<std::vector<double>>* modes, QString* error)
    {
        (void)options;
        (void)count;
        (void)shift;
        (void)frequencies;
        (void)modes;
        *error = QStringLiteral("no eigenfrequencies");
        return false;
    }
    /// Whether what it reads reads field \a f.
    bool reads(int f) const { return a_reads.contains(f); }
    int index() const { return a_fi; }
    Field3& field() { return a_s.fields()[std::size_t(a_fi)]; }
    const Node* node() const { return a_node; }

protected:
    void problem(const QString& what)
    {
        if (!a_problems.contains(what)) a_problems << what;
    }
    /// \a text of property \a key of \a feature compiled in the point
    /// scope, the fields it reads noted; one read where \a spatial is not,
    /// an error; its unit against the property's (a warning, once).
    Expression compile(const QString& text, const Node& feature, const QString& key, bool spatial = true);
    /// The material of each domain: the last that has it (null: none).
    std::vector<const Node*> materials();

    Solution3& a_s;
    int a_fi;
    const Node* a_node = nullptr;
    QSet<int> a_reads;
    QStringList a_problems;
};

/// The problem of field \a field of \a solution (heat, electrostatics,
/// currents).
std::unique_ptr<Problem3> makeScalarProblem3(Solution3& solution, int field);
/// A solid mechanics' (fem3_solid.cpp).
std::unique_ptr<Problem3> makeSolidProblem3(Solution3& solution, int field);

/// A semiconductor's (fem3_semiconductor.cpp).
std::unique_ptr<Problem3> makeSemiconductorProblem3(Solution3& solution, int field);

/// A semiconductor's small-signal admittance at \a frequency (Hz) about its
/// solution: Y[k][l] its contact k's current per volt at contact l.
bool semiconductorSmallSignal3(Problem3& problem, double frequency, std::vector<std::vector<std::complex<double>>>* Y, QStringList* names,
                               QString* error);

/// A magnetic field's (fem3_magnetic.cpp).
std::unique_ptr<Problem3> makeMagneticProblem3(Solution3& solution, int field);

/// A magnetic field at a point: A (Wb/m), B = ∇ × A (T), H (A/m), its
/// energy density (J/m³: ½ H·(B - Br), or ∫ H dB on a B-H curve) and its
/// relative permeability (a tensor's diagonal's mean; a curve's B/(µ0 H)).
struct MagneticState {
    double A[3] = {0, 0, 0}, B[3] = {0, 0, 0}, H[3] = {0, 0, 0};
    double energy = 0;
    double mur = 1;
};
/// Field \a f's (a magnetic field's) at \a ev's point; false where it is
/// not solved.
bool magneticStateAt(PointEvaluator3& ev, int f, MagneticState& out);

/// The outward normal of mesh face \a face at its point \a fp, from the
/// prism beside it: a floor's ±z, a wall's from its triangle's edge (exact
/// on an order 2 arc).
Vec3 outwardNormal3(const Space3& space, const Mesh3::Face& face, const Space3::FacePoint& fp);

/// A piezoelectric solid's stress constants e (3 x 6, Voigt's) in a 3D
/// model's axes: its feature's own, or its material's - e, or d C - turned
/// by its orientation. None, and why.
std::optional<VoigtText> piezoMatrix3(const Scope& params, const Node& feature, const Node* material, QString* why);

/// A solid's state at a point: its strain (Voigt's order xx, yy, zz, yz, xz,
/// xy, engineering shears), its stress σ = C (ε - ε0) - eᵀE (Pa) and its
/// strain energy density ½ (ε - ε0)·C (ε - ε0).
struct SolidState {
    double strain[6] = {0, 0, 0, 0, 0, 0};
    double stress[6] = {0, 0, 0, 0, 0, 0};
    double energy = 0;
};
/// Field \a f's (a solid's) at \a ev's point; false where it is not solved.
bool solidStateAt(PointEvaluator3& ev, int f, SolidState& out);

} // namespace qucs_s::fem::detail3

#endif // QUCS_FEM3_PROBLEM_H
