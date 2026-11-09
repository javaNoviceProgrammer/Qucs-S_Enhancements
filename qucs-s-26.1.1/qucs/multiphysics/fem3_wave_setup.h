/*
 * fem3_wave_setup.h - what a full-wave solve's frequency domain and time
 *                     domain share (fem3_wave.cpp, fem3_wavetd.cpp): the
 *                     model's materials, PMLs, ports and metal on the mesh,
 *                     the ports' modes and their columns
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM3_WAVE_SETUP_H
#define QUCS_FEM3_WAVE_SETUP_H

#include "fem3_wave.h"

#include <array>
#include <complex>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace qucs_s::fem::wave3 {

/// A port on the mesh: its plane, its faces' quadrilaterals and the prism on
/// the device's side of each, its cut.
struct Port {
    QPointF origin, across, s;   ///< a point of its plane (geometry unit), its across and into-the-device directions
    /// A PML behind it - its faces inside the geometry, between the device
    /// and a PML: a source and a monitor there, the PML taking what leaves
    /// (else on the outside, absorbing its cut's modes).
    bool backed = false;
    struct Quad {
        int a = -1, b = -1, layer = -1, prism = -1;
    };
    std::vector<Quad> quads;
    CrossSection cut;
    /// Its plane's degrees of freedom - those whose tangential E lies on
    /// its faces (not metal) -, and the prisms on the device's side that
    /// hold any of them: its monitor's.
    std::vector<int> wall, side;
};

/// A model on its mesh, as a full-wave solve sees it at a reference
/// wavelength (the materials' ε there; the PMLs' strength).
struct Setup {
    std::shared_ptr<const Mesh3> mesh;
    std::shared_ptr<EdgeSpace3> space;
    int N = 0;
    std::vector<const Node*> material;   ///< by domain
    std::vector<std::complex<double>> eps;   ///< by domain, at the reference wavelength
    std::set<int> pml;
    double inner[6] = {1e300, -1e300, 1e300, -1e300, 1e300, -1e300};   ///< the box of the rest (m)
    std::map<int, std::array<double, 6>> pmlBox;
    double logR = 13.8155;
    double pmlIndex = 1;   ///< the index σ is of: the lowest of the PMLs' materials'
    /// The stretching at point \a x (m) of PML domain \a domain, along x, y
    /// and z: s = 1 + σ/p, p = jk0 at a frequency (σ in 1/m; 0 outside a
    /// PML) - σ = 1.5 ln(1/R) ξ²/(n d), the frequency domain's s = 1 - jσ0ξ²,
    /// σ0 = 1.5 ln(1/R)/(k d) at every frequency, n pmlIndex.
    void sigma(int domain, const Vec3& x, double out[3]) const;
    std::vector<Port> ports;
    std::set<int> portFaces;
    std::vector<char> fixed;   ///< by degree of freedom: metal (tangential E nought)
    int layers = 0;
    std::unordered_map<long long, int> prismAt;
    int prism(int t, int j) const;
    QStringList log, warnings;
};

/// \a mesh's model as a full-wave solve of \a settings sees it at \a lambda
/// (m): false and why where it cannot be solved. Its ports' cuts made, not
/// their modes.
bool setUp(const Model& model, const ParameterScope& parameters, const Topology3& topology, std::shared_ptr<const Mesh3> mesh,
           const Wave3Settings& settings, double lambda, Setup* out, QString* error);

/// Port \a index's modes at \a frequency (Hz), \a count of them (its
/// absorbed by default), signed as one frame sees every port.
EmeModes portModes(const Model& model, std::shared_ptr<ParameterScope> parameters, const Topology3& topology, const Setup& setup,
                   const Wave3Settings& settings, int index, double frequency, int count, QString* why);

/// A sparse column: (degree of freedom, value).
using Column = std::vector<std::pair<int, std::complex<double>>>;

/// Element \a e's matrix at k0 (1/m): ∫ µ⁻¹ Λ⁻¹ ∇×W·∇×V - k0² ε Λ W·V, ε
/// its domain's of \a eps, Λ its PML's stretching (s = 1 + σ/(jk0)) - its
/// lower triangle (rows ≥ columns) filled.
void elementMatrix(const Setup& setup, int e, double k0, const std::vector<std::complex<double>>& eps, const PrismRule& rule,
                   Eigen::MatrixXcd& Ke);

/// Port \a port's modes on its faces, each a column: u_m(W) = ∮ (ŝ × h_m)·W
/// dS - so that of a field E there, ∮ (E × h_m)·ŝ = -u_m(E) -, and (\a v
/// given: a port with a PML behind it) v_m with ∮ (e_m × H)·ŝ =
/// -v_m(E)/(jk0 c µ0): the device's side's own equation, A(k0) of its
/// prisms at the port (ε of \a eps) against W_m, e_m on the port's faces
/// as the space has it (its projection there), nothing elsewhere - its
/// residual ∮ (n̂ × ∇×E)·W_m there, H's flux (more accurate than ∇×E at the
/// faces).
void portColumns(const Setup& setup, const Port& port, const EmeModes& modes, int order, double k0, const std::vector<std::complex<double>>& eps,
                 std::vector<Column>* u, std::vector<Column>* v);

/// What goes into the device (a) and out of it (b) in each mode of a port
/// whose columns are \a u and \a v, of field \a E at angular frequency \a
/// omega (rad/s): ½ (P ± Q), P = ∮ (E × h)·ŝ, Q = ∮ (e × H)·ŝ - the modes'
/// own normalization ∮ (e × h)·ŝ = 1, unconjugated.
void waveAmplitudes(const std::vector<Column>& u, const std::vector<Column>& v, const Eigen::VectorXcd& E, std::complex<double> jomega,
                    Eigen::VectorXcd* a, Eigen::VectorXcd* b);

/// The S-matrix's fed columns (\a r's S, by the port modes taken) and what
/// each leaves by (its balance) from what goes in (\a a) and out (\a b) in
/// every port mode (rows: every port's modes, in turn) in each fed one's
/// solve (columns): B A⁻¹, A the fed port modes' own incoming waves - each
/// column then of a unit wave in its own port mode, none in the other fed.
/// \a index: by port mode taken, its row; \a fed: by fed one, its index
/// among those taken; \a carries: by row, a mode that carries power. \a
/// perFed: A⁻¹ (each fed solve's share of a unit wave in each).
void scatteringOfAmplitudes(const Eigen::MatrixXcd& a, const Eigen::MatrixXcd& b, const std::vector<int>& index, const std::vector<int>& fed,
                            const std::vector<char>& carries, Wave3Result* r, Eigen::MatrixXcd* perFed);

} // namespace qucs_s::fem::wave3

#endif
