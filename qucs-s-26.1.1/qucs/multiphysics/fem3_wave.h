/*
 * fem3_wave.h - light through a 3D model's devices by the finite element
 *               method: E on edge elements of its prism mesh, the curl-curl
 *               equation at a frequency, PMLs, and ports that absorb every
 *               mode of their cross-section the 2D mode solver finds - the
 *               S-parameters between the ports' modes
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM3_WAVE_H
#define QUCS_FEM3_WAVE_H

#include "fem3_eme.h"
#include "fem3_estimate.h"
#include "fem3_fe.h"
#include "fem3_mesh.h"
#include "fem_edge.h"

#include <Eigen/Core>

#include <array>

#include <complex>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace qucs_s::fem {

/*!
 * Edge elements on a swept mesh of prisms (Mesh3) - Nédélec's of the first
 * kind of order 1 or 2, as a prism is its plan triangle times a layer: E
 * across z the plan's triangle edge functions (EdgeSpace) times Lagrange's
 * polynomials in z of the order (shared by the layers at their heights, and
 * at a layer's middle for order 2), E_z the plan's Lagrange nodal functions
 * (Space) times polynomials in z one order lower (a layer's own: E_z is
 * normal to the heights' faces, free to jump there). Tangential E is
 * continuous across every face: across a height through the shared edge
 * functions there, across a wall through the triangle edges' and the nodes'
 * functions. 9 functions a prism for order 1, 36 for order 2. The plan's
 * triangles taken straight (an arc's chords).
 */
class EdgeSpace3
{
public:
    EdgeSpace3(std::shared_ptr<const Mesh3> mesh, int order);

    const Mesh3& mesh() const { return *a_mesh; }
    std::shared_ptr<const Mesh3> meshPtr() const { return a_mesh; }
    int order() const { return a_order; }
    int size() const { return a_size; }
    int perElement() const { return a_order == 1 ? 9 : 36; }
    int elements() const { return int(a_mesh->prisms.size()); }
    const int* dofs(int e) const { return &a_dofs[std::size_t(e) * std::size_t(perElement())]; }
    const EdgeSpace& planEdges() const { return *a_edges; }
    const Space& planNodes() const { return *a_nodes; }

    /// The functions of prism \a e at its reference point (ξ, η, ζ): their
    /// values and curls (m⁻¹, m⁻²), x y z, and the volume element there
    /// (m³ per unit of the reference prism: the rule's weights times it).
    struct Basis {
        int count = 9;
        double v[36][3], c[36][3];
        double det = 0;
        Vec3 x;   ///< the point (m)
    };
    void basis(int e, double xi, double eta, double zeta, Basis& b) const;

    /// The degrees of freedom whose tangential E lies on a floor - plan
    /// triangle \a t at height \a height - and on a wall - the plan's edge
    /// from node \a a to \a b in layer \a layer. -1s (none) left out.
    std::vector<int> floorDofs(int t, int height) const;
    std::vector<int> wallDofs(int a, int b, int layer) const;
    /// The height's z (m).
    double z(int height) const { return a_mesh->heights[std::size_t(height)] * a_mesh->unitScale; }

private:
    int levelOf(int height, int k) const { return a_order == 1 ? height + k : 2 * height + k; }
    std::shared_ptr<const Mesh3> a_mesh;
    std::shared_ptr<const Space> a_nodes;
    std::shared_ptr<const EdgeSpace> a_edges;
    int a_order = 1;
    int a_size = 0;
    int a_levels = 0, a_layers = 0;
    std::vector<int> a_dofs;
    std::unordered_map<long long, int> a_across;   // plan edge dof * levels + level
    std::unordered_map<long long, int> a_along;    // (plan node dof * layers + layer) * 2 + m
    std::unordered_map<unsigned long long, int> a_middle;   // order 2: a plan edge's middle node dof, by its nodes
};

/// A port of a full-wave solve: a set of the geometry's faces in one vertical
/// plane - the end of a waveguide -, the modes of its cross-section it takes
/// (each an S-parameter port), and how many modes of that cross-section it
/// absorbs (what reaches it leaves in them: a waveguide going on).
struct Wave3Port {
    QString name;
    std::vector<int> faces;
    QStringList modes{QStringLiteral("TE0")};
    int absorbed = 20;
    bool excite = true;
};

/// What a full-wave solve solves: the elements' order, the frequency, the
/// ports, the domains that are PMLs (stretched outward from the box of the
/// rest: s = 1 - jσ0ξ², σ0 = 1.5 ln(1/R)/(k d), the mode solver's own), the
/// ports' cuts' resolution (elements per wavelength of their 2D meshes).
struct Wave3Settings {
    int order = 2;
    std::vector<Wave3Port> ports;
    std::vector<int> pmlDomains;
    double pmlReflection = 1e-6;
    /// Ports through the PMLs: their faces on PML domains kept, their cuts'
    /// modes with the PMLs stretched as the 3D field's are - the layers' own
    /// modes then crowd the cut's (80 asked for 10 kept). Off: those faces
    /// left out, metal behind the PML there (a wire's S the same).
    bool portsThroughPmls = false;
    double portResolution = 30;
    /// The wavelength (m) the ports' cuts are meshed for (0: each solve's
    /// own): a sweep's or a time domain's every wavelength of one mesh.
    double portMeshWavelength = 0;
    /// The most its factorization may take (bytes; 0: no limit): past it,
    /// as its analysis says, not solved - and what it would take said.
    double memoryLimit = 0;
    /// The fed port modes whose field is kept: the first so many (-1: every
    /// one) - a solve each.
    int fieldsKept = -1;
    /// Ports with a PML behind them: their sources' profiles the modes' at
    /// this frequency (Hz; 0: at the solve's own) - as the time domain's
    /// are, its band's middle.
    double sourceFrequency = 0;
    /// The time domain's: its steps a period at its band's middle; when it
    /// stops - the ports' fields fallen to this of their peak -, or at most
    /// so many steps.
    double stepsPerPeriod = 6;
    double decay = 1e-5;
    int maxSteps = 200000;
    /// The time domain's: the wavelengths whose field it keeps - 0 none, 1 the
    /// one nearest the band's middle, n so many evenly over the band, -1
    /// every one (as far as 2 GB hold them); the S-parameters at every one.
    int fieldWavelengths = 5;
    std::function<bool()> cancelled;
    std::function<void(double, const QString&)> progress;
};

/// One frequency's full-wave solve: the S-matrix between the ports' modes
/// (each normalized to 1 W, as the eigenmode expansion's: comparable), each
/// fed port mode's E (its degrees of freedom on the space), what leaves by
/// the ports of each fed one, the log.
struct Wave3Result {
    double frequency = 0;
    struct PortMode {
        int port = -1;
        QString label;
        std::complex<double> neff;
        double te = 0;
    };
    std::vector<PortMode> portModes;
    Eigen::MatrixXcd S;   ///< [out][in]
    std::vector<double> balance;   ///< by port mode fed: the power leaving by every port in every propagating mode, of 1 W
    std::vector<double> back;      ///< by port mode fed: of it, the power back by its own port, in every propagating mode
    std::shared_ptr<const EdgeSpace3> space;
    std::vector<Eigen::VectorXcd> fields;   ///< by fed port mode, in portModes' order of the fed
    std::vector<int> fed;                   ///< the port modes fed (their index in portModes)
    int unknowns = 0;
    QStringList log, warnings;
    QString error;
    bool ok() const { return error.isEmpty(); }
};

/// The settings an Electromagnetic Waves, Frequency Domain physics of 3D \a
/// model asks for: its order, its ports (Optical Port features: their faces,
/// modes and how many they absorb), its PMLs (Perfectly Matched Layer
/// features' domains, their reflection). False and why where it cannot be
/// solved.
bool readWave3Settings(const Model& model, const Node& physics, const Topology3& topology, const ParameterScope& parameters, Wave3Settings* out,
                       QString* error);

/// E of a solve's \a field at each degree of freedom of \a nodes - an
/// order-1 Lagrange space on the same mesh: the mean over the prisms that
/// share it (its normal component jumps across a material's face).
std::vector<std::array<std::complex<double>, 3>> nodalField(const EdgeSpace3& space, const Eigen::VectorXcd& field, const Space3& nodes);

/// What a full-wave solve of \a order on \a mesh will hold: its unknowns,
/// and its peak memory - some 20 kB an unknown, the most measured (a box of
/// 973,000 unknowns 19.8 GB, 1.92 million 43 GB; a long guide's 11 kB) - and
/// seconds (a box's: 68 s and 249 s there; a long guide's a fifth).
Estimate3 estimateWave3(std::shared_ptr<const Mesh3> mesh, int order);

/// What a time-domain solve of \a order on \a mesh will hold and take: a
/// real Cholesky's factors (half a frequency domain's), its PMLs' states;
/// its steps some 700, each a solve with the factors.
Estimate3 estimateWave3Time(std::shared_ptr<const Mesh3> mesh, int order);

/// \a mesh's device of 3D \a model at \a frequency (Hz).
Wave3Result solveWave3(const Model& model, std::shared_ptr<ParameterScope> parameters, const Topology3& topology, std::shared_ptr<const Mesh3> mesh,
                       const Wave3Settings& settings, double frequency);

/// A time-domain full-wave solve: each frequency asked as a frequency-domain
/// solve's result (its S-matrix's fed columns, what each leaves by, the
/// first fed port mode's field), and how it ran - its step, its steps, the
/// run's frequencies whose transforms are those asked (Hz: (2/Δt)
/// atan(πfΔt)/(2π) of each f asked), whether its fields settled.
struct Wave3TimeResult {
    std::vector<Wave3Result> at;
    double dt = 0;
    int steps = 0;
    bool settled = false;
    std::vector<double> sampled;
    QStringList log, warnings;
    QString error;
    bool ok() const { return error.isEmpty(); }
};

/// \a mesh's device of 3D \a model in the time domain: one pulse from each
/// fed port mode (all together), the S-parameters at each of \a frequencies
/// (Hz) - the frequency domain's at each, to its Fourier transforms'
/// truncation (fem3_wavetd.cpp). Every port must have a PML behind it.
Wave3TimeResult solveWave3Time(const Model& model, std::shared_ptr<ParameterScope> parameters, const Topology3& topology,
                               std::shared_ptr<const Mesh3> mesh, const Wave3Settings& settings, const std::vector<double>& frequencies);

/// A material's relative permittivity at \a lambda (m): (n - jκ)², or εr.
std::complex<double> relativePermittivity(const Node& material, double lambda, const Scope& scope);

} // namespace qucs_s::fem

#endif
