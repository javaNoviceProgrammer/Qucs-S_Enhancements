/*
 * fem_process_p.h - a process flow's insides: Clipper's integer polygons
 *                   (a process grid's 64th each) and what is done to them
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_PROCESS_P_H
#define QUCS_FEM_PROCESS_P_H

#include "fem_model.h"
#include "fem_process.h"

#include <clipper/clipper.hpp>

#include <QString>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace qucs_s::fem::process {

using cInt = ClipperLib::cInt;
using Path = ClipperLib::Path;
using Paths = ClipperLib::Paths;

/// Their union (nonzero), outlines counterclockwise, holes clockwise.
Paths unite(const Paths& a);
/// a op b: union, difference, intersection.
Paths clip(ClipperLib::ClipType type, const Paths& a, const Paths& b);
/// Grown by \a by (shrunk: negative), round, arcs within \a arcTolerance.
Paths offset(const Paths& a, double by, double arcTolerance);
Paths rectangle(cInt x0, cInt y0, cInt x1, cInt y1);
/// What \a a sweeps moving by (dx, dy): it, it moved and between - holes
/// kept as they are swept.
Paths sweep(const Paths& a, cInt dx, cInt dy);
double area(const Paths& a);
/// Its lowest and highest y.
std::pair<cInt, cInt> yRange(const Paths& a);
/// Each point on a multiple of \a step, made clean again.
void snap(Paths& a, cInt step);
/// Its connected pieces, each an outline and its holes.
std::vector<Paths> components(const Paths& a);

/// A region as the flow works on it: a step's material, integer polygons.
struct Region {
    QString name, material;
    bool crystal = false;
    Paths p;
};

// ------------------------------------------------------------------ Ions

/// An element: its atomic number, standard atomic weight (a target's), the
/// mass of its most abundant isotope (an ion's), u; its symbol.
struct Element {
    int Z = 0;
    double weight = 0, isotope = 0;
    QString symbol;
};
const Element* element(int Z);
const Element* element(const QString& symbol);

/// A target: its atoms' elements and shares (adding up to 1), its atoms per
/// m³.
struct Target {
    struct Part {
        int Z = 0;
        double M = 0, share = 0;
    };
    std::vector<Part> parts;
    double atoms = 0;
    QString formula;   ///< as said: "Si", "Si O2", "In0.53 Ga0.47 As"
};

/// An ion's range in an amorphous target, from transport theory: along the
/// beam its mean Rp, straggle ΔRp, skewness γ, kurtosis β (m); across it
/// the straggle, and how the lateral variance changes with depth (⟨y²|x⟩ ≈
/// σ⊥² + slope (x - Rp)); the energy that goes into nuclear collisions
/// (eV, an ion's) and where it goes (its mean depth and straggle); the
/// path's length.
struct RangeMoments {
    bool ok = false;
    double Rp = 0, dRp = 0, gamma = 0, beta = 3;
    double lateral = 0, lateralSlope = 0;
    double nuclear = 0, damageMean = 0, damageStraggle = 0;
    double pathLength = 0;
};
/// An ion of atomic number \a Z1, mass \a M1 (u), energy \a energy (eV), in
/// \a target; its electronic stopping Lindhard and Scharff's times \a
/// electronicScale.
RangeMoments rangeMoments(int Z1, double M1, const Target& target, double energy, QString* error = nullptr,
                          double electronicScale = 1);

/// A material as an implant's target: the model's material of that name
/// (or library entry), else the library's entry - a compound (GaAs, InGaAs
/// at x...) its elements by its sublattices, else a name known (Silicon,
/// Silicon dioxide, Silicon nitride, Photoresist, BCB, the metals...); its
/// atoms from its density (rho), else its lattice. None, and why.
std::optional<Target> targetOf(const QString& material, const Model& model, const ParameterScope& ps, QString* why = nullptr);
/// What a target is as a host for dopants (ProcessDopants::hosts).
int hostOf(const Target& target);

/// An implanted species: the atom it leaves (BF2's boron), its atomic
/// number and mass, the share of the beam's energy it has.
struct Ion {
    QString atom;
    int Z = 0;
    double M = 0, energyShare = 1;
    QString symbol() const { return atom; }
};
std::optional<Ion> ionOf(const QString& species);

/// An implant's moments in a target (m), for a Pearson profile along the
/// beam and the lateral straggle across it, the energy into nuclear
/// collisions (eV an ion) and its depth's mean and straggle; where they
/// came from, and a note when they are extrapolated.
struct ImplantMoments {
    double Rp = 0, dRp = 0, gamma = 0, beta = 3;
    double lateral = 0, lateralSlope = 0;
    double nuclear = 0, damageMean = 0, damageStraggle = 0;
    QString source, note;
    bool extrapolated = false;
};
/// Boron, phosphorus and arsenic in silicon from Plummer et al.'s tables
/// (their γ, β and lateral straggle the theory's, scaled); in another
/// target, and every other ion, by transport theory - the tabled ions'
/// electronic stopping fitted to their tables, scaled as in silicon.
std::optional<ImplantMoments> implantMoments(const Ion& ion, double energy, const Target& target, QString* why = nullptr);

/// The ZBL universal potential (Ziegler, Biersack and Littmark, 1985): its
/// screening function φ(x), x in screening lengths; a pair's universal
/// screening length (Å).
double zblScreening(double x);
double screeningLength(int Z1, int Z2);
/// A collision in the universal potential, in reduced units - ε the energy
/// in the centre of mass in Z1 Z2 e²/a, b the impact parameter in a -: the
/// scattering angle θ in the centre of mass, the time integral τ and the
/// closest approach r0 (both in a). The transport theory's θ and the
/// crystal's Monte Carlo's tables both.
struct Scattering {
    double theta = 0, tau = 0, r0 = 0;
};
Scattering universalScattering(double eps, double b);

// ------------------------------------------------ The crystal's Monte Carlo

/// An amorphous layer over the crystal as the beam meets it: its material,
/// its target, its thickness along the normal (m).
struct CrystalLayer {
    QString material;
    Target target;
    double thickness = 0;
};

/*!
 * A crystal implant for the binary-collision Monte Carlo (the "crystal"
 * profile): the ion (Z, mass u) and its energy (eV, the atom's); the beam's
 * tilt from the normal, its rotation (from the surface's ⟨110⟩ - the
 * wafer's flat - to the direction it tilts toward, the section's +x) and
 * its divergence (a uniform cone's half-angle), radians; the wafer's
 * surface (100, 110, 111) and its crystal; the amorphous layers over it
 * from the top; the wafer's temperature (K); how many ions, the random
 * numbers' seed; the deepest it follows them (m; 0: from its amorphous
 * range). Amorphous: the crystal taken as amorphous (a check of the model).
 */
struct CrystalRun {
    int Z1 = 0;
    double M1 = 0, energy = 0;
    double tilt = 0, rotation = 0, divergence = 0;
    QString orientation = QStringLiteral("100");
    Target crystal;
    std::vector<CrystalLayer> cover;
    double temperature = 300;
    long ions = 50000;
    unsigned long long seed = 1;
    double depth = 0;
    bool amorphous = false;
    int threads = 0;   ///< 0: as many as the machine has (the result is the same)
    /// What the result depends on (its cache's key).
    QString key() const;
};

/*!
 * Its result: where the ions stopped, by depth along the normal in bins
 * from the top of the cover (its first coverBins bins over the crystal's
 * surface, the rest below it), and across the section (Σx, Σx², m, about
 * where each came in); those backscattered, those past the depth followed.
 * Option B of the proposal: the histogram is the profile - remaining()
 * reads its cumulative off it, merged in the tail until each piece holds
 * 20 ions -, so a tail no Pearson fits is kept.
 */
struct CrystalProfile {
    double bin = 0;   ///< m
    int coverBins = 0;
    double coverThickness = 0;   ///< m
    std::vector<double> counts, sumX, sumX2;
    long ions = 0, back = 0, beyond = 0;
    double ms = 0;
    int threads = 0;
    /// In the crystal, from its surface (m): the moments of where the ions
    /// stopped, the depth where the profile falls to 1e-3 of its peak; the
    /// share of the ions that stopped in it.
    double Rp = 0, dRp = 0, gamma = 0, beta = 3, tail = 0, inCrystal = 0;
    /// The share of the ions that stopped deeper than \a depth (m, from the
    /// cover's top) - those past the depth followed among them.
    double remaining(double depth) const;
    /// The straggle across the section (m) of those that stopped at \a depth.
    double lateral(double depth) const;
    /// The merged pieces: their edges (m from the top), the ions before
    /// each edge, each piece's lateral straggle.
    std::vector<double> edges, cumulative, pieceLateral;
    void finish();
};

/// Whether the Monte Carlo holds for \a run: an error where it does not
/// (refused), warnings where it was not checked (item 9 of the proposal).
QString crystalTrust(const CrystalRun& run, QStringList* warnings);
/// The Monte Carlo of \a run - cached by its key, so a flow built again
/// does not run it again (\a cached says so). \a ticker told how far it is,
/// false from it stops it ("cancelled"). Null and why on an error.
std::shared_ptr<const CrystalProfile> crystalImplant(const CrystalRun& run, const std::function<bool(double)>& ticker, QString* error,
                                                     bool* cached = nullptr);
/// The cache emptied.
void clearCrystalCache();
/// The Monte Carlo's table of universalScattering at (ε, b), read as its
/// collisions read it (bilinear in log ε and b).
Scattering tabulatedScattering(double eps, double b);

/// A compound's elements by its sublattices (shares on each adding up to
/// 1): group III's (cations) and group V's (anions) - or a group IV
/// alloy's, one lattice, on the first.
struct Sublattices {
    std::vector<std::pair<int, double>> cation, anion;
    bool ok = false;
};
/// Of a compound as the materials name it (GaAs, InGaAs at x of In,
/// InGaAsP's In1-x Gax Asy P1-y, AlGaInAs's Alx Gay In1-x-y As, SiGe at x
/// of Ge...).
Sublattices sublatticesOf(const QString& compound, double x, double y);
/// The compound and its x, y for sublattices' shares (AlGaInAs, AlGaInP,
/// InGaAsP, GaAsSb, AlGaInN as their x and y take them); false when no
/// compound known holds those elements.
bool compoundOf(const Sublattices& s, QString* compound, double* x, double* y);

/*!
 * A Pearson distribution of its four moments - mean, standard deviation,
 * skewness γ, kurtosis β: type IV (from Pearson's equation f'/f = (s -
 * a)/(b0 + b1 s + b2 s²)), the Gaussian at γ = 0, β = 3 - tabulated
 * finely over its support, its cumulative too.
 */
class Pearson
{
public:
    static Pearson make(double mean, double sd, double gamma, double beta);
    static Pearson gaussian(double mean, double sd);
    /// Whether (γ, β) is in the type IV region (0 ≤ κ < 1).
    static bool typeIV(double gamma, double beta);
    /// The least kurtosis of type IV at skewness γ (its edge, κ = 1).
    static double leastBeta(double gamma);
    double pdf(double z) const;
    double cdf(double z) const;
    /// Where \a fraction of it lies beyond (1 - cdf = fraction).
    double depthPassed(double fraction) const;
    double mean() const { return a_mean; }
    double sd() const { return a_sd; }
    double gamma() const { return a_gamma; }
    double beta() const { return a_beta; }
    /// Its table's own moments: mean, σ, γ, β.
    std::array<double, 4> momentsOfTable() const;
    bool valid() const { return !a_z.empty(); }

private:
    double a_mean = 0, a_sd = 0, a_gamma = 0, a_beta = 3;
    std::vector<double> a_z, a_pdf, a_cdf;
};

// ------------------------------------------------------ Dopants on a grid

/// A thermal step's temperature against time: points (s, K), linear
/// between them, from t = 0.
struct Thermal {
    std::vector<std::pair<double, double>> points;
    double duration() const { return points.empty() ? 0 : points.back().first; }
    double at(double t) const;
    /// The highest and lowest temperatures (K).
    double highest() const;
    double lowest() const;
};

/// A parameter of a species in a material a step's table gives (SI): D0,
/// Ea, Dm0, Em, Dmm0, Emm, Dp0, Ep (the charge states' diffusivities);
/// Css0, Ecss (solid solubility), tau0, Etau (its clusters' time
/// constant); loss (dose loss at a surface, m/s); fI (the interstitials'
/// share); m:<material> and h:<material> (segregation into that one: its
/// ratio, its transport, m/s).
struct Override {
    QString species, material, key;
    double value = 0;
};

/// An implant as a step gives it.
struct ImplantSpec {
    QString species;
    double energy = 0;     ///< eV
    double dose = 0;       ///< m⁻²
    double tilt = 0;       ///< rad, the beam toward +x as it goes down
    bool gaussian = false; ///< γ 0, β 3 (else Pearson IV)
    struct Given {
        QString material;
        double Rp, dRp, gamma, beta, lateral;   ///< NaN: computed
    };
    std::vector<Given> given;
    struct Channel {
        QString material;
        double fraction = 0, Rp = 0, dRp = 0, gamma = 0, beta = 3;
    };
    std::vector<Channel> channel;   ///< a dual Pearson's channelled part
    double plus = 1;                ///< interstitials an ion leaves ("+1")
    double critical = 6e29;         ///< eV/m³: amorphous above
    /// The moments given (and the channelled part's) are along the normal
    /// (an MC's, a SIMS's taken at the tilt): not projected by the tilt again.
    bool normal = false;
    /// The crystal profile: the Monte Carlo through the crystal and its
    /// amorphous cover; its beam's rotation, divergence (rad), the wafer's
    /// temperature (K), its ions and seed; the wafer's surface.
    bool crystalProfile = false;
    double rotation = 0, divergence = 0, temperature = 300;
    long ions = 50000;
    unsigned long long seed = 1;
    QString orientation = QStringLiteral("100");
};

/// An anneal (a drive-in, a predeposition) as a step gives it.
struct AnnealSpec {
    Thermal thermal;
    QString model;                  ///< fermi, constant, defects
    QString source;                 ///< a predeposition's species (empty: inert)
    double surface = 0;             ///< its concentration at the surface, m⁻³
    std::vector<Override> overrides;
    double clusterLife = 600, clusterLifeT = 1073.15, clusterE = 3.8;   ///< the {311} clusters' lifetime at a temperature, eV
};

/// An oxidation as a step gives it.
struct OxideSpec {
    Thermal thermal;
    bool wet = false;
    QString model;                  ///< massoud, dealgrove
    QString orientation;            ///< 100, 110, 111
    double pressure = 1;            ///< atm
    double factor = 1;              ///< on both rates (HCl)
    std::vector<std::pair<cInt, cInt>> open;   ///< where it oxidizes (empty, unmasked: everywhere)
    bool masked = false;
    double beak = -1;               ///< the bird's beak's length (geometry's unit); < 0: the oxide's thickness
    QString material, name;         ///< the oxide's material and its region's name
    AnnealSpec dopants;             ///< the dopants' models meanwhile
    std::optional<double> givenB, givenBA;   ///< m²/s, m/s: Deal and Grove's rates given (one temperature)
};

/// A quantum-well intermixing anneal as a step gives it.
struct IntermixSpec {
    Thermal thermal;
    double D0III = 0, EIII = 0, D0V = 0, EV = 0;   ///< m²/s, eV
    std::vector<std::pair<cInt, cInt>> open;
    bool masked = false;
    double enhance = 1;             ///< the interdiffusion's factor where the mask is open
};

/*!
 * A flow's dopants on their grid: made at the first step that needs them,
 * kept up with the shapes (an etch takes a cell's dopants with it, a
 * deposit's cells start clean), and the dopant steps done on them.
 * Lengths in the geometry's unit, its integers a_k to one.
 */
class Dopants
{
public:
    Dopants(const Model& model, const ParameterScope& ps, double unitScale, double k);
    bool made() const { return a_d != nullptr; }
    /// The grid refused, past its budget of cells (a flow failed so): said.
    const QString& tooLarge() const { return a_tooLarge; }
    std::shared_ptr<const ProcessDopants> state() const;
    /// The grid over the window x0..x1 from the bottom up: dx across, dy
    /// down to depth below the lowest surface, coarser below.
    void make(cInt x0, cInt x1, cInt bottom, const std::vector<Region>& regions, double dx, double dy, double depth);
    /// After a shape step: the cells' materials again (rows added above a
    /// new top); a cell gone to air its dopants with it, said.
    void reshape(const std::vector<Region>& regions, QStringList* warnings);
    /// Region \a name's cells (new) doped: \a species at \a concentration.
    void dope(const QString& name, const std::vector<Region>& regions, const QString& species, double concentration);
    /// The fine rows reaching depth below the lowest surface (made finer
    /// where an etch went below them).
    void refine(const std::vector<Region>& regions);
    QString implant(const ImplantSpec& spec, const std::vector<Region>& regions, const Paths& resist, QStringList* warnings, QString* error);
    QString anneal(const AnnealSpec& spec, const std::vector<Region>& regions, QStringList* warnings, QString* error);
    QString oxidize(const OxideSpec& spec, std::vector<Region>& regions, cInt x0, cInt x1, cInt bottom, QStringList* warnings, QString* error);
    QString intermix(const IntermixSpec& spec, const std::vector<Region>& regions, QStringList* warnings, QString* error);
    /// The material index of a name (added).
    int materialIndex(const QString& name);
    /// Told how far a step is through its time (0 to 1) at each of its
    /// time steps - an anneal's, an oxidation's, an intermixing's -: false
    /// from it stops the step, its error "cancelled".
    void setTicker(std::function<bool(double)> ticker) { a_ticker = std::move(ticker); }
    struct Solver;

private:
    struct MaterialInfo;
    const MaterialInfo& info(int m);
    void assign(const std::vector<Region>& regions, std::vector<int>* material) const;
    void addRows(double top);
    bool fits(double nx, double ny);
    void remapRows(const std::vector<double>& ye);
    double cellWidth(int i) const;    // m
    double cellHeight(int j) const;   // m

    const Model& a_model;
    const ParameterScope& a_ps;
    double a_unitScale, a_k;
    double a_dy = 0, a_depth = 0;
    std::shared_ptr<ProcessDopants> a_d;
    std::vector<std::shared_ptr<MaterialInfo>> a_info;
    QString a_tooLarge;
    std::function<bool(double)> a_ticker;
};

} // namespace qucs_s::fem::process

#endif // QUCS_FEM_PROCESS_P_H
