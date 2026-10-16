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
#include "fem_tensor.h"

#include <Eigen/IterativeLinearSolvers>
#include <Eigen/SparseCholesky>
#include <Eigen/SparseCore>
#include <Eigen/SparseLU>

#include <QSet>

#include <algorithm>
#include <complex>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

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
    /// A mode analysis's: 1 TE modes alone (their transverse E mostly along
    /// x - in the plane of layers stacked along y), 2 TM (along y); 0 any.
    /// TE: a TE fraction of teFraction or more; TM: 1 - teFraction or less.
    int polarization = 0;
    double teFraction = 0.5;
    /// A mode analysis's: those kept - an effective index above minNeff,
    /// a loss (of the power, 1/m) below maxAlpha.
    double minNeff = 0, maxAlpha = std::numeric_limits<double>::infinity();
    /// A mode analysis's mode chosen of those kept (the one shown, evaluated
    /// and lit by default): 0 the first (the highest effective index), 1
    /// the most in the domains tracked, 2 the most TE, 3 the most TM.
    int choose = 0;
};

/// An optical spectrum of quantum states: at photon energies (J), from so
/// many states of each carrier, broadened (a Lorentzian's half width, J),
/// with carriers in the wells (1/m³; 0: none - absorption) and a refractive
/// index (0: the wells').
struct SpectrumOptions {
    std::vector<double> energies;
    int states = 6;
    double linewidth = 0;   ///< J: the line's half width at half maximum
    int lineshape = 0;      ///< 0 Lorentzian, 1 Gaussian, 2 hyperbolic secant
    bool excitons = false;  ///< each transition's 1s exciton and its continuum's Sommerfeld factor
    int excitonModel = 0;   ///< 0 two parameters (λ, γ), 1 Bastard's one (λ)
    double epsr = 0;        ///< the excitons' static permittivity (0: the wells' material's)
    double dlinewidth = 0;  ///< J per V/m: the broadening grown by the field across the wells
    double fade = 0;        ///< continuum bound: the bound steps fade into the bulk over this above the barriers' gap (J)
    double kmax = 0;        ///< a Valence Bands step's: its largest in-plane k (1/m)
    int kpoints = 0;        ///< and how many values of k from 0 (a spectrum's: its k·p bands')
    int continuum = 0;      ///< 0 the states found; 1 bound states' transitions and the barriers' bulk absorption
    double density = 0;
    double index = 0;
    /// The subbands that enter: so many conduction ones (0: states), so
    /// many valence ones counted by energy across heavy and light holes (0:
    /// states of each); a state whose wavefunction's slope at the line's
    /// ends (its hard walls) is more than cutoffC (cutoffV) of its largest
    /// left out (NaN: none) - Lumerical's "cutoff cb/vb" -, and with \a select
    /// 1 only those with at least \a localized of |ψ|² in the wells and the
    /// barriers between them.
    int cstates = 0, vstates = 0;
    double cutoffC = std::numeric_limits<double>::quiet_NaN(), cutoffV = std::numeric_limits<double>::quiet_NaN();
    int select = 0;
    double localized = 0.6;
    /// 0 every transition broadened; 1 only those within the photons'
    /// window (to windowExt (J) above its top), as Lumerical's MQW.
    int broaden = 0;
    double windowExt = 0;
    /// What the absorption is per: 0 the wells' thickness, 1 the line's (the
    /// stack's), 2 normLength (m).
    int normalize = 0;
    double normLength = 0;
    SolveOptions solve;
};

/// A quantum wells' absorption (the power's, 1/m) and Kramers-Kronig index
/// at one photon energy (J), TE and TM, at fields along their axis F (V/m,
/// rising): what a mode analysis's Quantum Wells' Absorption reads.
struct WellTable {
    double photon = 0;
    std::vector<double> F, alphaTE, alphaTM, nTE, nTM;
    /// At field \a f - linear between, held beyond: α TE, α TM, n TE, n TM.
    void at(double f, double out[4]) const
    {
        const std::size_t n = F.size();
        const std::vector<double>* cols[4] = {&alphaTE, &alphaTM, &nTE, &nTM};
        if (n == 0) {
            for (int c = 0; c < 4; ++c) out[c] = 0;
            return;
        }
        std::size_t k = 0;
        double s = 0;
        if (n > 1 && f > F.front()) {
            k = std::min<std::size_t>(std::size_t(std::upper_bound(F.begin(), F.end(), f) - F.begin()) - 1, n - 2);
            s = std::clamp((f - F[k]) / (F[k + 1] - F[k]), 0.0, 1.0);
        }
        for (int c = 0; c < 4; ++c) out[c] = n > 1 ? (1 - s) * (*cols[c])[k] + s * (*cols[c])[k + 1] : (*cols[c])[0];
    }
};
/// A table a mode analysis's feature asks for: of quantum wells \a wells
/// (a tag), at Fpoints fields from -Fmax to Fmax.
struct WellRequest {
    QString feature, wells, spectrum;   ///< (spectrum: a Spectrum step's tag; empty: the study's first)
    double Fmax = 0;
    int Fpoints = 0;
};

/// A waves physics' media's: compiling, its materials' texts as they are
/// (tensors too), their orientations, its problems.
struct MediaHooks {
    std::function<Expression(const QString&, const Node&, const QString&)> compile;
    std::function<QString(const Node&, const QString&)> property;
    std::function<Rotation(const Node&)> rotation;
    std::function<void(const QString&)> problem;
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
    /// The mode chosen of those found (from 0; SolveOptions::choose).
    virtual int chosenMode() const { return 0; }
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
    /// Quantum wells: the field along their axis (V/m) - its value, and
    /// set (solve() again after); false: none to set.
    virtual double wellField() const { return 0; }
    virtual bool setWellField(double F)
    {
        (void)F;
        return false;
    }
    /// Quantum wells: the spectrum's absorption and index at photon energy
    /// \a E (one of its energies): α TE, α TM, n TE, n TM. False: none.
    virtual bool spectrumAt(double E, double out[4]) const
    {
        (void)E;
        (void)out;
        return false;
    }
    /// A mode analysis: the wells' tables its features ask for, and each
    /// as found (at the frequency solved).
    virtual std::vector<WellRequest> wellRequests() const { return {}; }
    virtual void setWellTable(const QString& feature, std::shared_ptr<const WellTable> table)
    {
        (void)feature;
        (void)table;
    }
    /// Quantum states: the k·p valence bands at \a o's in-plane k (kmax,
    /// kpoints). How many points; -1 and why on failure.
    virtual int solveBands(const SpectrumOptions& o, QString* error)
    {
        (void)o;
        *error = tr("%1 has no valence bands (Quantum Wells has)").arg(field().tag);
        return -1;
    }
    /// The globals (set after) of the bands' point \a k.
    virtual void selectBands(int k) { (void)k; }
    /// A Schrödinger-Poisson loop's turn: the carriers found by the states
    /// (another physics') taken in where asked; the largest change made (in
    /// Vt; 0 none, NaN an error in \a error).
    virtual double quantumCorrection(QString* error)
    {
        (void)error;
        return 0;
    }
    /// Whether what it reads (its coefficients, sources, conditions) reads
    /// field \a f.
    bool reads(int f) const { return a_reads.contains(f); }
    int index() const { return a_fi; }
    Field& field() { return a_s.fields()[std::size_t(a_fi)]; }
    const Node* node() const { return a_node; }
    /// Asked whether to stop (its step's: the model tab's Cancel, Claude's
    /// fem_stop) - a long setup or solve, a semiconductor's way to its
    /// contacts' voltages, looks between its steps.
    void setCancelled(std::function<bool()> asked) { a_cancelled = std::move(asked); }

protected:
    void problem(const QString& what) { a_problems << what; }
    bool cancelled() const { return a_cancelled && a_cancelled(); }
    /// \a text of property \a key of \a feature compiled in the point
    /// scope, what it reads noted; a field read where \a spatial is not, an
    /// error.
    /// (\a unitChecked: its unit against the property's - not where it is
    /// the equation's own, a PDE's flux.) In \a scope where given (one over
    /// the point scope: a material's T bound).
    Expression compile(const QString& text, const Node& feature, const QString& key, bool spatial = true, bool unitChecked = true,
                       const Scope* scope = nullptr);
    /// The material of each domain: the last that has it (null: none). A
    /// physics that takes a material's property as one value (not a
    /// tensor's: a semiconductor, rays, quantum wells) says in the log where
    /// one of its materials gives a tensor, whose mean it takes.
    std::vector<const Node*> materials();
    /// Property \a key of material \a m as a tensor in the model's axes:
    /// the crystal's texts turned by the material's orientation (one item:
    /// isotropic, as it is). \a allowed: the counts of items it may have.
    /// None when it is not given, or wrong (said to the problems). Its
    /// orientation's turn in \a rotation.
    std::optional<TensorText> materialTensor(const Node& m, const QString& key, const QList<int>& allowed = {1, 3, 6, 9},
                                             Rotation* rotation = nullptr);
    /// Material \a m's orientation (said to the problems, once, when wrong).
    Rotation rotationOf(const Node& m);
    /// A waves physics' media's hooks.
    MediaHooks mediaHooks()
    {
        // (Pockels' r, m/V, a text property's: its unit checked by the media.)
        return {[this](const QString& t, const Node& n, const QString& k) { return compile(t, n, k, true, k != QLatin1String("r")); },
                [this](const Node& m, const QString& k) { return materialText(m, k, &a_s.parameters().scope); },
                [this](const Node& m) { return rotationOf(m); }, [this](const QString& w) { problem(w); }};
    }
    /// A Piezoelectric Effect feature's e (3x6, Voigt's, C/m²) in the
    /// model's axes: as given on it, or its domain's material's (\a
    /// material) crystal's turned by its orientation. None, and why.
    std::optional<VoigtText> piezoMatrix(const Node& feature, const Node* material, QString* why);

public:
    /// Property \a key of material \a m: its own, else its library entry's,
    /// else (a III-V alloy's) its compound's at its composition and
    /// temperature \a T (K) - evaluated in \a scope (the parameters'; none: a
    /// plain number). (A text may read T, the temperature it is at: read it
    /// in a MaterialScope.)
    /// A tensor's (a property of tensorKeys() given as "a; b; c") is its
    /// mean: materialText() has it as it is.
    static QString materialProperty(const Node& m, const QString& key, const Scope* scope = nullptr, double T = 300);
    static QString materialText(const Node& m, const QString& key, const Scope* scope = nullptr, double T = 300);
    /// Material \a m's composition solved for its gap ("composition":
    /// "gap" - hc/λg at 300 K, a quaternary on its "lattice"'s): x and y.
    /// None when it is not asked, or (why in \a error) not found.
    static std::optional<std::pair<double, double>> solvedComposition(const Node& m, const Scope* scope = nullptr, QString* error = nullptr);
    /// The material properties that may be tensors: εr, σ, k, tan δ, µr,
    /// α, n, κ, dn/dT.
    static const QStringList& tensorKeys();
    /// Property \a key as given: its own, else its library entry's - and
    /// for Eg, without either, hc/λg of a gap wavelength λg given. Empty: not
    /// given (a compound's, if any, is what is left).
    static QString givenProperty(const Node& m, const QString& key, const Scope* scope = nullptr);

protected:
    Solution& a_s;
    int a_fi;
    const Node* a_node = nullptr;
    QSet<int> a_reads;
    QStringList a_problems;
    std::function<bool()> a_cancelled;
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
    /// SRH's lifetimes here: by doping, of x and y, over 1 + Γ with
    /// trap-assisted tunnelling (infinite: no SRH).
    double taun = std::numeric_limits<double>::infinity(), taup = std::numeric_limits<double>::infinity();
    double Jn[2] = {0, 0}, Jp[2] = {0, 0};
    /// The carriers' Boltzmann densities: n/γ, p/γ with Fermi-Dirac's
    /// statistics (γ = ℱ1/2(η)/e^η), n and p with Boltzmann's.
    double nB = NaN, pB = NaN;
    /// The carriers' Joule heat as solved, the triangle's (W/m³).
    double joule = 0;
    /// Incomplete ionization's (0: the dopants all ionized): Nd⁺ = Nd/(1 +
    /// gd nB/n1d), Na⁻ = Na/(1 + ga pB/p1a) - filled by the Fermi level.
    double n1d = 0, gd = 0, p1a = 0, ga = 0;
    double donors(double Nd) const { return n1d > 0 ? Nd / (1 + gd * nB / n1d) : Nd; }
    double acceptors(double Na) const { return p1a > 0 ? Na / (1 + ga * pB / p1a) : Na; }
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

/// A quantum wells physics' layers as it reads them, for an export
/// (Lumerical's MQW): by domain, its band edges unstrained (eV; a Quantum
/// Well Material's when constants), electron mass, Luttinger's γ1-γ3 (a
/// Quantum Well Material's of its masses), split-off Δ and Kane's Ep (eV),
/// refractive index and static permittivity; its strain in the plane and
/// what strains it (a0 m; ac, av, b eV, Chuang's signs; C11, C12 Pa). The
/// physics' temperature (K), field (V/m), element size along its line (m),
/// k·p bands' size (0 parabolic, 4, 6, 8), axis (0 x, 1 y), whether it is a
/// line's and whether it has a potential of another physics'.
struct QuantumLayer {
    bool on = false, user = false, wurtzite = false, valleys = false;
    double Ec = NaN, Ev = NaN, me = NaN, g1 = NaN, g2 = NaN, g3 = NaN, delta = NaN, Ep = NaN, nr = NaN, epsr = NaN;
    double exx = 0, a0 = NaN, ac = NaN, av = NaN, b = NaN, C11 = NaN, C12 = NaN;
};
struct QuantumLayers {
    std::vector<QuantumLayer> layers;
    double T = 300, F = 0, h1d = 1e-10;
    int valence = 0, axis = 1;
    bool line = true, potential = false;
    QString error;
};
QuantumLayers quantumLayers(Solution& solution, int field);

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

/// A body of revolution's resonant mode at a point (V/m, A/m): along r, z
/// and φ (the model's x, y and the third axis), its fields E(r, z) e^{-jmφ}
/// at φ = 0, the largest |E| 1 V/m.
struct ResonatorFields {
    std::complex<double> E[3], H[3];
    bool defined = false;
};
ResonatorFields resonatorFieldsAt(PointEvaluator& e, int field);

/// A mode analysis's medium where the evaluator is (constant over a
/// triangle, at its middle - kept with the mode selected): n, κ absorbed
/// across a gap (the medium's own and a direct gap's, not the free
/// carriers'), the direct gap's alone and its value without a field, ε's
/// loss ε'' = (εᴴ - ε)/2j (Hermitian: -Im ε along x, y, z where it is
/// diagonal); the angular frequency; the free carriers' Δn (their plasma
/// dispersion and band filling) and absorption (1/m: theirs, less what band
/// filling bleaches); quantum wells' Δn (from F0) and absorption, TE and TM.
struct MediumAt {
    static constexpr int PerTriangle = 19;
    bool defined = false;
    double omega = NaN, n = NaN, kappaGap = 0, kappaEA = 0, kappaEA0 = 0, dnCarriers = 0, alphaCarriers = 0;
    double dnWells[2] = {0, 0}, alphaWells[2] = {0, 0};
    std::complex<double> lossEps[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
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
