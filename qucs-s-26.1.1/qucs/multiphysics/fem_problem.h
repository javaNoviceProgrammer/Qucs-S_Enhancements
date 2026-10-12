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

#include <complex>
#include <functional>
#include <limits>
#include <memory>

namespace qucs_s::fem::detail {

QString tr(const char* text);
constexpr double Eps0 = 8.8541878188e-12;
inline const double NaN = std::numeric_limits<double>::quiet_NaN();

/// Runs \a work(begin, end) on parts of [0, n) in threads.
void parallelFor(int n, const std::function<void(int, int)>& work);

/// The elements of a system: called with a function each element gives
/// its equations to (negative ones left out).
using ElementPattern = std::function<void(const std::function<void(const int*, int)>&)>;
/// A sparse matrix's compressed columns of \a neq equations, every pair
/// of an element's (and the diagonal): \a outer (neq + 1) and \a inner.
void buildPattern(int neq, const ElementPattern& elements, std::vector<int>& outer, std::vector<int>& inner);

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
    void setPattern(int neq, const ElementPattern& elements);
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

/*!
 * A sparse complex system A x = b (the frequency domain: K + jωM):
 * factorized, solved as often as wanted. One complex symmetric whose real
 * part is positive definite (\a quasiDefinite) is factorized as its real
 * equivalent [K, -B; -B, -K] (for K + jB) - symmetric and quasi-definite, so
 * an LDLT of it exists in any order - by a sparse Cholesky's LDLT, faster
 * than a complex LU; else, or should that fail, by LU.
 */
class ComplexSystem
{
public:
    using Matrix = Eigen::SparseMatrix<std::complex<double>>;
    void setPattern(int neq, const ElementPattern& elements);
    int size() const { return a_n; }
    void clear();
    void add(int i, int j, std::complex<double> v);
    bool factorize(const QString& name, QString* error, bool quasiDefinite = false);
    bool solve(const Eigen::VectorXcd& rhs, Eigen::VectorXcd& x, const QString& name, QString* error) const;

private:
    int a_n = 0;
    Matrix a_A;
    std::unique_ptr<Eigen::SparseLU<Matrix>> a_lu;
    bool a_analyzed = false;
    Eigen::SparseMatrix<double> a_real;
    std::unique_ptr<Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>>> a_ldlt;
    bool a_ldltAnalyzed = false;
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
    /// In the frequency domain, at this frequency (Hz): a physics that has
    /// one (harmonic()) solved there, others at rest.
    bool harmonic = false;
    double frequency = 0;
    /// A mode analysis's: by domain, those whose modes are kept - the most
    /// of |E|² there (its share emw.Gamma); empty: the nearest the index.
    std::vector<char> track;
};

/// An optical spectrum of quantum states: at photon energies (J), from so
/// many states of each carrier, broadened (a Lorentzian's half width, J),
/// with carriers in the wells (1/m³; 0: none - absorption) and a refractive
/// index (0: the wells').
struct SpectrumOptions {
    std::vector<double> energies;
    int states = 6;
    double linewidth = 0;
    double density = 0;
    double index = 0;
    SolveOptions solve;
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
    /// Whether its last solve's Newton's method converged (one that did not
    /// leaves its field as the last step had it, and says so in the log).
    virtual bool converged() const { return true; }
    /// Whether it has a frequency domain (magnetic fields' eddy currents).
    virtual bool harmonic() const { return false; }
    /// Whether it is a mode analysis (its field a mode of the cross-section).
    virtual bool modal() const { return false; }
    /// A mode analysis at the solution's frequency: \a count modes nearest
    /// the effective index \a guess (0: the largest index there is). How
    /// many were found; -1 and why on failure.
    virtual int solveModes(const SolveOptions& options, int count, double guess, QString* error)
    {
        (void)options;
        (void)count;
        (void)guess;
        *error = tr("%1 has no modes").arg(field().tag);
        return -1;
    }
    /// The field (and its globals, set after) of mode \a k found (from 0).
    virtual void selectMode(int k) { (void)k; }
    /// Quantum states: their optical spectrum at \a o's photon energies.
    /// How many points; -1 and why on failure.
    virtual int solveSpectrum(const SpectrumOptions& o, QString* error)
    {
        (void)o;
        *error = tr("%1 has no optical spectrum (Quantum Wells has)").arg(field().tag);
        return -1;
    }
    /// The globals (set after) of the spectrum's point \a k.
    virtual void selectSpectrum(int k) { (void)k; }
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
    /// Property \a key of material \a m: its own, else its library entry's,
    /// else (a III-V alloy's) its compound's at its composition - evaluated in
    /// \a scope (the parameters'; none: a plain number).
    static QString materialProperty(const Node& m, const QString& key, const Scope* scope = nullptr);

    Solution& a_s;
    int a_fi;
    const Node* a_node = nullptr;
    QSet<int> a_reads;
    QStringList a_problems;
};

/// The problem of field \a field of \a solution (by its type).
std::unique_ptr<Problem> makeProblem(Solution& solution, int field);

/// A semiconductor's state at a point: the potential, the carriers (their
/// logarithms interpolated), the element's mobilities and band energies,
/// the net recombination, the currents (Scharfetter and Gummel's along the
/// element's edges, the vector agreeing best with the three).
struct SemiconductorState {
    bool defined = false;         ///< in the physics
    bool semiconductor = false;   ///< not an insulator
    double V = NaN, n = NaN, p = NaN, ni = NaN, Vt = NaN;
    double mun = NaN, mup = NaN, dEc = NaN, Eg = NaN;   // (Ec - Ei and Eg in J)
    double delta = 0;                 ///< the intrinsic level's offset here (V): Ei = -q (V + δ)
    double phin = NaN, phip = NaN;    ///< the quasi-Fermi potentials (V), smooth across a heterojunction
    double R = 0;
    double Jn[2] = {0, 0}, Jp[2] = {0, 0};
};
SemiconductorState semiconductorAt(PointEvaluator& e, int field);

/// A quantum wells physics at a point: the state selected - each carrier's
/// wavefunction (1/√m along a 1D line, 1/m in the plane, 1/m^(3/2) about an
/// axis: ∫|ψ|² = 1), its energy and its largest |ψ|² - and the bands there
/// (J), the masses along the growth axis (m0), the strain in the plane, the
/// height a wave is drawn (J).
struct QuantumState {
    bool defined = false;
    double psi[3] = {NaN, NaN, NaN};
    double E[3] = {NaN, NaN, NaN};
    double peak[3] = {NaN, NaN, NaN};
    double Ec = NaN, Ehh = NaN, Elh = NaN;
    double me = NaN, mhh = NaN, mlh = NaN;
    double strain = NaN, drawn = NaN;
};
QuantumState quantumAt(PointEvaluator& e, int field);

/// A PDE's dependent variables (by name), and whether it is of the second
/// order in time (their rates its components too, named <u>_t).
QStringList pdeVariables(const Node& physics);
bool pdeSecondOrder(const Node& physics);
/// A PDE solved along a line: component \a c's value and its derivative
/// along the line at the evaluator's point (NaN off it).
void pdeLineAt(PointEvaluator& e, int field, int c, double* u, double* us);

/// A mode's fields at a point (V/m, A/m), as found (a phase of 0 at z = 0).
struct ModeFields {
    std::complex<double> E[3], H[3];
    bool defined = false;
};
ModeFields modeFieldsAt(PointEvaluator& e, int field);

/// A mode analysis's medium where the evaluator is (constant over a
/// triangle, at its middle - kept with the mode selected): n, κ absorbed
/// across a gap (the medium's own and a direct gap's, not the free
/// carriers'), the direct gap's alone and its value without a field, -Im ε
/// along x, y and z; the angular frequency.
struct MediumAt {
    static constexpr int PerTriangle = 7;
    bool defined = false;
    double omega = NaN, n = NaN, kappaGap = 0, kappaEA = 0, kappaEA0 = 0;
    double lossEps[3] = {0, 0, 0};
};
MediumAt modeMediumAt(PointEvaluator& e, int field);
/// The in-plane waves' field at a point: u (Ez or Hz), its gradient, the
/// other field's in-plane components (Hx, Hy or Ex, Ey).
struct WaveFields {
    std::complex<double> u, ux, uy;
    std::complex<double> E[3], H[3];
    bool defined = false;
};
WaveFields waveFieldsAt(PointEvaluator& e, int field);

/// A material property's value as the node has it, else as its library
/// entry has it (a table: an array).
QJsonValue materialValue(const Node& m, const QString& key);

/// Electrostatics' C0 (every εr 1) for a line's parameters.
double scalarVacuumCapacitance(Solution& solution, int field, const QString& terminal, QString* error);

} // namespace qucs_s::fem::detail

#endif // QUCS_FEM_PROBLEM_H
