/*
 * qucscontrol_fem.cpp - Claude's multiphysics tools: a model (.qfem) read,
 *                       made, changed node by node, its geometry and mesh
 *                       built, its studies solved, its results evaluated
 *                       and shown - as the Multiphysics panel does
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "qucscontrol.h"
#include "qucscontrol_p.h"

#include "fem_lumerical.h"
#include "fem_material_plot.h"
#include "fem_materials.h"
#include "main.h"
#include "misc.h"
#include "multiphysicsdoc.h"
#include "multiphysicspanel.h"
#include "qucs.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUndoStack>
#include <cmath>
#include <map>

using namespace qucs_s::control;
using namespace qucs_s::fem;
using FNode = qucs_s::fem::Node;

namespace {

QJsonObject nodeJson(const FNode& n, int depth)
{
    QJsonObject o{{QStringLiteral("tag"), n.tag}, {QStringLiteral("type"), n.type}, {QStringLiteral("name"), n.name()}};
    if (!n.enabled) o.insert(QStringLiteral("enabled"), false);
    if (n.isDefault) o.insert(QStringLiteral("default"), true);
    if (!n.props.isEmpty()) o.insert(QStringLiteral("props"), n.props);
    if (const NodeKind* k = n.kind(); k && k->property(QStringLiteral("selection"))) {
        const Level level = k->property(QStringLiteral("level")) ? levelOf(n.text(QStringLiteral("level"))) : k->selection;
        o.insert(QStringLiteral("selection is"), describeSelection(n.selection(), level));
    }
    if (!n.children.empty()) {
        if (depth <= 0) {
            o.insert(QStringLiteral("children"), int(n.children.size()));
        } else {
            QJsonArray c;
            for (const FNode& child : n.children) c.append(nodeJson(child, depth - 1));
            o.insert(QStringLiteral("children"), c);
        }
    }
    return o;
}

QJsonArray numbers(const QVector<int>& v)
{
    QJsonArray a;
    for (int i : v) a.append(i + 1);
    return a;
}

QString lengthText(double v, const Topology& t)
{
    return formatNumber(v, 6) + QLatin1Char(' ') + (t.unitName == QLatin1String("um") ? QStringLiteral("µm") : t.unitName);
}

QJsonObject topologyJson(const Topology& t, int most)
{
    QJsonObject o{{QStringLiteral("domains"), int(t.domains.size())},
                  {QStringLiteral("boundaries"), int(t.boundaries.size())},
                  {QStringLiteral("points"), int(t.points.size())},
                  {QStringLiteral("unit"), t.unitName},
                  {QStringLiteral("objects"), QJsonArray::fromStringList(t.objectNames)}};
    QJsonArray domains;
    for (int d = 0; d < int(t.domains.size()) && d < most; ++d) {
        const Topology::Domain& dm = t.domains[std::size_t(d)];
        QStringList objs;
        for (int oi : dm.objects) objs << t.objectNames.value(oi);
        objs.sort();
        domains.append(QJsonObject{{QStringLiteral("number"), d + 1},
                                   {QStringLiteral("object"), t.objectNames.value(dm.owner)},
                                   {QStringLiteral("in objects"), QJsonArray::fromStringList(objs)},
                                   {QStringLiteral("area"), formatNumber(dm.area, 6)},
                                   {QStringLiteral("a point in it"), QJsonArray{formatNumber(dm.inside.x(), 6), formatNumber(dm.inside.y(), 6)}}});
    }
    o.insert(QStringLiteral("domain list"), domains);
    QJsonArray boundaries;
    for (int b = 0; b < int(t.boundaries.size()) && b < most; ++b) {
        const Topology::Boundary& bd = t.boundaries[std::size_t(b)];
        QStringList objs;
        for (int oi : bd.objects) objs << t.objectNames.value(oi);
        objs.sort();
        const QPointF a = t.vertices[std::size_t(bd.start)], e = t.vertices[std::size_t(bd.end)];
        QJsonObject bo{{QStringLiteral("number"), b + 1},
                       {QStringLiteral("from"), QJsonArray{formatNumber(a.x(), 6), formatNumber(a.y(), 6)}},
                       {QStringLiteral("to"), QJsonArray{formatNumber(e.x(), 6), formatNumber(e.y(), 6)}},
                       {QStringLiteral("length"), lengthText(bd.length, t)},
                       {QStringLiteral("of objects"), QJsonArray::fromStringList(objs)}};
        QJsonArray sides;
        if (bd.up >= 0) sides.append(bd.up + 1);
        if (bd.down >= 0) sides.append(bd.down + 1);
        bo.insert(QStringLiteral("between domains"), sides);
        if (bd.curve >= 0 && t.curves[std::size_t(bd.curve)].kind == Curve::Arc) bo.insert(QStringLiteral("arc"), true);
        boundaries.append(bo);
    }
    o.insert(QStringLiteral("boundary list"), boundaries);
    if (int(std::max(t.domains.size(), t.boundaries.size())) > most)
        o.insert(QStringLiteral("more"), QStringLiteral("only the first %1 of each are listed ('max' lists more)").arg(most));
    return o;
}

/// A node's parent named as Claude names it: its tag, or a part of the
/// tree by its kind ("geometry", "materials", "mesh", "results",
/// "derived", "definitions", "selections", "component", "model").
FNode* parentNamed(Model& m, const QString& name)
{
    if (FNode* n = m.find(name)) return n;
    if (name == QLatin1String("model") || name.isEmpty()) return &m.root;
    if (name == QLatin1String("component")) return &m.component();
    if (name == QLatin1String("geometry")) return &m.geometry();
    if (name == QLatin1String("materials")) return &m.materials();
    if (name == QLatin1String("mesh")) return &m.mesh();
    if (name == QLatin1String("results")) return &m.results();
    if (name == QLatin1String("definitions")) return &m.definitions();
    if (name == QLatin1String("derived")) return m.results().child(QStringLiteral("derived"));
    if (name == QLatin1String("exports")) return m.results().child(QStringLiteral("exports"));
    if (name == QLatin1String("selections")) return m.component().child(QStringLiteral("selections"));
    return nullptr;
}

QString formatText()
{
    return QStringLiteral(
        "A model (.qfem) is JSON: {\"format\": \"qucs-s multiphysics 1\", \"title\", \"parameters\": [{\"name\", \"expression\", "
        "\"description\"}], \"functions\": [analytic or interpolation nodes], \"components\": [{\"unit\": \"mm\", \"thickness\": "
        "\"1[m]\", \"space\": \"2D\" or \"axisymmetric\" (x is r, y is z), \"selections\": [...], \"geometry\": [features in order "
        "(an \"import\" reads a DXF or SVG \"file\"), \"form union\" last], \"materials\": [...], "
        "\"physics\": [{\"type\": \"electrostatics\", \"tag\": \"es\", \"features\": [...]}], \"mesh\": {\"size\": \"normal\", "
        "\"features\": [...]}}], \"studies\": [{\"type\": \"study\", \"steps\": [{\"type\": \"stationary\"}]}], \"results\": "
        "{\"plots\": [plot groups, each with \"plots\"; cut lines, cut points; 1D plot groups - \"x\" a global along x, "
        "semi.I_anode], \"derived\": [...]}}. A step in "
        "time: {\"type\": \"transient\", \"times\": \"range(0, 0.1, 1)\"} (heat changes in time: its materials need rho, Cp; "
        "\"method\": bdf2, euler or midpoint - the implicit midpoint rule, no damping for waves, with \"stepping\": \"fixed\" and "
        "\"dt\"); in "
        "frequency: {\"type\": \"frequency\", \"frequencies\": \"logspace(1[kHz], 1[GHz], 31)\"} (magnetic fields' eddy currents; the "
        "other physics at rest at each); a sweep, first among the steps: {\"type\": \"sweep\", \"parameters\": [[\"w\", \"1, 2, 3\", \"mm\"]]}. "
        "Each solve's last log line names the sparse solver that ran (\"Solved N degrees of freedom in 1.2 s with Accelerate's LU\"): "
        "Accelerate's (macOS - its LU on macOS 26 and later) from 20,000 unknowns, Eigen's below; the user's Multiphysics > "
        "Settings… chooses, QUCS_FEM_SOLVER=eigen, accelerate or auto in the environment over it. A matrix singular to "
        "rounding - a part of the model whose value nothing sets - is refused, not solved. "
        "A node: {\"type\", \"tag\", \"label\", "
        "\"enabled\", properties...}; a geometry object's name is its label. Under a physics a feature's type is short "
        "(\"terminal\"). A material {\"material\": \"FR-4\"} takes the library's properties (a soft magnetic one its \"BH\" "
        "curve, [[B, H], ...] in T and A/m; a magnet its \"Br\"; a dielectric its loss tangent \"tand\").\n"
        "Anisotropic materials: epsilonr, sigma, k, tand, mur, alpha, n, kappa and dndT may be tensors - \"a; b; c\" along x, y, "
        "z, six items xx; yy; zz; yz; xz; xy, nine row by row - a crystal's in its own axes, its \"orientation\" turning them "
        "into the model's (x across, y up - a wafer's normal -, z out of the plane, along a waveguide): \"cut\" (\"cut\": its "
        "axis up, x, y or z; \"along\": its axis along z) or \"euler\" (euler_phi, euler_theta, euler_psi in degrees, z-x-z). "
        "Its higher tensors by named Voigt entries, \"C11=203[GPa]; C12=53[GPa]; ...\" (each named; Cji alike): stiffness \"C\", "
        "piezoelectric \"e\" (C/m²) or \"d\" (C/N: e = d C), electro-optic \"r\" (m/V), photoelastic \"p\"; a garnet's "
        "gyration \"g\". The library's crystals: Lithium niobate and Lithium tantalate (x-cut, y along z), KTP, Quartz, "
        "Sapphire (c-plane), YIG, Ce:YIG; Silicon (stiffness, photoelastic), Gallium arsenide, Indium phosphide (r41) on a "
        "(100) wafer. Electrostatics, currents and heat take the in-plane 2x2 part (a feature's own: \"anisotropy\" diagonal "
        "or symmetric, epsxx, epsyy, epsxy - sigmaxx..., kxx...; es.Dx, ec.Jx, ht.qx along it; a line's G its loss tangent's "
        "tensor); magnetics µr's (murxx, muryy, murxy); solid mechanics a material's C (\"anisotropy\": \"anisotropic\" with "
        "C on the feature; plane stress condensed) and its expansion along its axes; a \"piezo\" feature couples it to an "
        "electrostatics (\"es\" its tag) - σ = C S - eᵀE, D = εE + e S, solved in turn.\n"
        "Magnetic fields (\"magnetic\", tag mf): A out of the plane (round the axis); features ampere (\"constitutive\": \"mur\" or "
        "\"bh\"), current (Je), coil (\"name\", \"conductor\": \"single\" - its current's distribution found, the skin effect - or "
        "\"multi\" with \"N\" turns; \"I0\"; a single one's \"role\": current - the default, a port of the coils' matrix -, voltage "
        "with \"V0\", floating - no net current - or grounded - no voltage between its ends, as a conductor with no coil, its "
        "current mf.I_<name> found: an odd mode's half-model's ground, not one fed -1 A), magnet (\"direction\", Br), insulation "
        "(A = 0, on the outside), potential, pmc, surfacecurrent; the physics' \"reference\" (a coil's name) gives the matrix "
        "reduced against it, mf.Lr_i_j and mf.Rr_i_j (Z_ij - Z_ir - Z_rj + Z_rr). A layout: an \"import\" of a .gds/.oas \"file\" (\"cell\", \"layers\": \"1/0, 2/0\") makes an object per "
        "layer, name_layer_datatype; a \"crosssection\" cuts one from \"start\" to \"end\" (µm) into its \"stack\" rows [name, layer "
        "(empty: all along the cut), z, thickness] - an object per row. A line's R, L, G, C: a \"lineparams\" of \"method\": \"rlgc\" "
        "(es's \"terminal\", mf's \"conductor\" coil and \"return\"), its study a stationary es step then a frequency mf step - "
        "n_eff = β/k0 (epsilon_eff its square) and epsilon_eff_qs = C/C0; coupled lines: \"terminal\": \"1, 2\" and as many "
        "\"conductor\" coils, their C, C0, L, R, G matrices (C_1_2...) and modes - a symmetric pair's even and odd (Z0_odd, "
        "n_eff_even, alpha_odd, epsilon_eff_qs_even, Zdiff, Zcm), others' 1, 2... by n_eff with their V_, I_ and Zc_; "
        "\"exports\": touchstone (\"line\", \"length\", \"zref\"; coupled lines a 2N-port, a pair's \"mixed\": true mixed-mode), "
        "spice (\"what\": line or matrix; coupled lines as sections), qucspart (an RLCG subcircuit; a pair's a CTLIN part), "
        "lumericalmode (\"materials\": indices or library, \"step\", \"modes\"), lumericalcharge (\"strip\"), lumericalmqw (\"materials\": model "
        "or database) - their \"physics\", \"run\" and \"file\" too, a Lumerical script of the model - "
        "fem_export writes them.\n"
        "Waves: \"modes\" (tag emw) - a waveguide cross-section's modes, a \"mode\" step {\"frequency\": \"1550[nm]\" (Hz or a "
        "wavelength), \"modes\": 4, \"neff\": the index to search about, \"track\": a selection (those most in it, emw.Gamma), "
        "\"polarization\": \"te\" or \"tm\" (one alone: emw.TEfraction, of the transverse |E|² the part along x, at 1/2 or "
        "more is TE - a strip's lateral TE modes kept from outranking a slab's TM0)}; features wave (\"constitutive\": \"n\" - n, kappa - or "
        "\"epsilonr\"; \"anisotropy\": \"diagonal\" with nx, ny, nz or \"tensor\" with epsxy, epsxz, epsyz too - xz and yz "
        "couple Ez, the modes then found by β, a magnetized garnet's forward ones; kappa, tand, sigma one or three along x, y, z "
        "(a sheet as a thin layer: σ; 0; σ); a material's crystal turned; \"thermooptic\" with T, Tref and the material's dndT; "
        "\"electrooptic\" - Pockels' r, the material's or \"r\", the field Ex, Ey, Ez (es.Ex, es.Ey); \"photoelastic\" - p, "
        "\"strain\" a solid's tag; \"magnetooptic\" - \"magnetization\" x, y or z, g), sheet (on an interior boundary: "
        "\"model\" graphene - muc, tau, T, Kubo's - or user - sigmas, sigmas_im (σs - j sigmas_im) -; emw.Psheet, emw.loss_sheet), pml, "
        "pec (the outside by default), pmc; each mode a solution (\"mode\" picks one) with emw.neff, emw.loss (dB/m), emw.alpha, "
        "emw.loss_gap (dB/m absorbed across a gap: what a photodiode turns into pairs), emw.TEfraction, and fields emw.Ex... "
        "emw.Sz, emw.n, emw.Qh and "
        "emw.Qgap (W/m³ absorbed, the mode at 1 W), emw.alphagap. A wave feature's \"electroabsorption\": true adds a direct "
        "gap's absorption and the Franz-Keldysh effect - its material's Egd, mr, Afk (Germanium, Germanium on silicon: 0.80 and "
        "0.773 eV; override Egd for GeSi) or \"easource\": \"user\" with them; \"Fea\" the field (semi.normE), \"Ebroad\" "
        "a Gaussian broadening (8[meV]) - and emw.dloss_EA, emw.dalphaEA what the field adds: an electroabsorption modulator's "
        "loss against its bias (stationary semi then a mode step, swept). A derived value \"confinement\" (\"selection\" the "
        "core's domains, \"kind\": e2 - |E|²'s share, the usual Γ -, power - Sz's -, or index - (c ε0/2) ∫ n |E|² there over "
        "∫ Sz, ∂neff/∂n: a modal gain's Γ, a Δn there moving neff by Γ Δn) gives each mode's Γ without cut lines (a TM mode's "
        "normal E jumps at an index step). \"waves\" (tag ewfd) - waves in the plane (\"polarization\": \"ez\" or \"hz\") in a "
        "frequency step; features wave, pml, pec, pmc, scattering (\"incident\": \"plane\" with E0, angle), port (\"name\", "
        "\"excite\") - S-parameters ewfd.S21, S21_im, S21dB, S21_abs, S21_arg, and the first port fed's reflectance, "
        "transmittance, absorptance ewfd.R, ewfd.T, ewfd.A; a touchstone export \"of\": \"ports\" writes them; Hz "
        "takes ε's in-plane tensor (a port's medium's axes along and across it). A unit cell (a grating, a film, a "
        "metasurface): \"periodic\" on its two opposite walls in one selection ({\"of\": [all its layers], \"side\": "
        "[\"left\", \"right\"]}; \"floquet\": \"port\" by default - the fed periodic port's kF -, \"continuity\", or "
        "\"given\" with kx, ky), meshed alike; \"periodicport\" on its top and bottom, one period long in one material "
        "(\"name\", \"excite\" - one fed, at \"angle\" degrees from its normal toward +x, amplitude E0): a plane wave in and "
        "each diffraction order out without reflection - ewfd.R, ewfd.T, ewfd.A, each propagating order's ewfd.R_0, R_1, R_n1 "
        "(n1: -1) and ewfd.T_0... (T_<port>_<m> with more ports), the zeroth's fields ewfd.r_0, r_0_im, t_0, t_0_im, ewfd.kF; "
        "the log lists the orders and their angles. Ez is TE (s), Hz TM (p). Point sources: \"dipole\" (\"position\" [x, y] in "
        "the geometry's unit, inside the physics' domains; px, py - in the plane, Hz - and pz - out of it, Ez - in C, per "
        "metre of z; \"phase\" degrees) and \"linecurrent\" (position, I, phase; Ez): ewfd.Psource the power they give (W/m), "
        "ewfd.Psource0 the same sources' in a homogeneous medium (in 2D the same in every one), ewfd.Purcell their ratio; "
        "ports not fed (\"excite\": false) take what reaches them, ewfd.Pport_<name>; a fed port or a plane wave coming in "
        "with them refused; the mesh four times finer about each; in a lossless medium, not a PML (said). An array scan - "
        "one source in a periodic structure from its unit cell: \"periodic\" with \"floquet\": \"scan\", \"points\" (32) and "
        "\"sampling\" (\"anomalies\": clustered about Rayleigh's anomalies, where an order grazes a lossless outer medium; "
        "\"even\": the trapezoid rule, the same as a supercell of N cells), a dipole or line current in the cell, PMLs or "
        "periodic ports not fed above and below: ewfd.Psource, ewfd.Purcell and the field one source's, ewfd.Pport_<name> "
        "what it sends out by each periodic port; the log says what half the points give (a guided mode that loses little "
        "wants many). "
        "\"resonator\" (tag emr) - an axisymmetric component's body of revolution (x = r, y = z, the axis x = 0, nothing at "
        "x < 0), its cavity modes of one azimuthal order (E ∝ e^(-jmφ), \"m\" 0, 1, 2...; m and -m alike), full-vector - E_r, "
        "E_z on edge elements, E_φ on nodes, \"order\" 1 or 2 - by an \"eigenfrequency\" step {\"around\": a free-space "
        "wavelength (980[nm]) or a frequency, \"count\": 6 - the modes nearest it}: each mode a solution (\"mode\" picks "
        "one), its complex eigenfrequency emr.freq, emr.freq_im, emr.lambda0 and emr.Q = Re ω / 2 Im ω (infinite with nothing "
        "lossy or open), emr.m; a pml (cylindrical about the axis: r stretched where it is along r) for an open cavity's Q "
        "(the modes mostly in it left out, said); pec (the outside by default), pmc; materials by n, kappa (a metal's too) "
        "or epsilonr, one or three along r, z, φ. Fields at φ = 0 emr.Er, Ez, Ephi (each _im too), Hr, Hz, Hphi, normE, "
        "normH, Sr, Sz - the largest |E| outside the pml 1 V/m. m 0 splits into TE0n (E_φ alone) and TM0n, m 1 holds "
        "HE1n and EH1n, m 2 HE2n... (an LP mode's vector parts); fem_solve lists each mode's lambda0 and Q to seven figures - "
        "the solve's on its mesh, not its accuracy (a VCSEL's LP11 splitting, some pm, wants a finer mesh than its modes: solve "
        "again finer before reading it). A pml's \"reflection\" its strength (1e-6). Modes of Q near 1, mostly in the layers, "
        "are left out with the layers' own. "
        "Its stored energy emr.U (½ ε0 ∫ Re ε |E|², the layers left out) and loss emr.Ploss = ω U/Q; emr.we its density, "
        "emr.epsr Re ε. The step's \"keep\": the mode the steps after see (its number, 0 the highest Q) - a laser's.\n"
        "Lasers: a semiconductor's \"laser\" feature on its gain region (its wells) lases in a resonator's kept mode "
        "(\"mode\": emr) - the mode's photons S solved with the carriers: (G - 1/τp) S + β Rsp = 0, τp = Q/ω, G = Σ v g(N) Π "
        "(Π each node's share of the mode's energy, v = c/\"ng\"), Rsp its radiative recombination (a radiative feature there), "
        "the stimulated recombination v g S Π taken from the carriers; \"gain\": logarithmic (g0, Ntr, Ns), linear (a, Ntr) "
        "or quantum - a quantum wells physics' (\"wells\") TE (\"polarization\") gain at the mode's photon energy "
        "tabulated over \"points\" densities from \"Nmin\" to \"Nmax\", a Spectrum step's options (\"spectrum\", else its "
        "study's first; disabled, it is only read; its line \"sech\"); \"carrier\": mean (√np), electrons or holes; "
        "\"beta\", \"eta\" (the loss's share out). Give the gain region's material \"kappa\": \"0\" - its absorption is the "
        "gain's, not the cold cavity's. semi.S, semi.Pout, semi.G, semi.Gnet, semi.Gamma, semi.gth, semi.gmean, "
        "semi.Istim, semi.Isp, semi.Nact, semi.tauph (the photon lifetime; semi.taup is the holes' SRH lifetime); semi.gain, "
        "semi.Rst. Refused: a second laser feature; ng below 1; beta or eta outside 0 to 1; a, g0 not above 0; Ntr, Ns below 0; "
        "Nmin not below Nmax; no spontaneous emission (beta 0, or no Radiative on the gain region); a mode that loses "
        "nothing (Q infinite); quantum gain in a step other than a Laser L-I step; a negative keep. A \"laser\" step {\"contact\": \"anode\", "
        "\"voltages\": \"range(1.3, 0.02, 1.9)\", \"physics\": [\"semi\", \"qw\"]} after an eigenfrequency step: each "
        "voltage a solution (from the last), the log its threshold and slope; a 1D plot group's \"x\": \"semi.I_anode\" "
        "for the light against the current. "
        "A stationary or frequency step's \"adaptive\": true refines the mesh where the error is largest (\"adaptsteps\" times; in frequency, the last one's).\n"
        "Semiconductors: \"semiconductor\" (tag semi, \"T\"; \"space\": \"1d\" solves it along a line across its layers - "
        "\"axis\" y or x, \"at\", \"h1d\" - each node then of its coordinate: a stack under a mesa, its contacts where the line "
        "ends, its currents through the narrowest of its domains, at rest alone) - drift and diffusion of electrons and holes on linear elements; a "
        "domain whose material has a band gap Eg is a semiconductor (Silicon, Germanium, Gallium arsenide, Indium phosphide, "
        "Gallium nitride, Germanium on silicon, the III-V and II-VI compounds, Silicon germanium), else an insulator (the "
        "potential alone; Hafnium, Zirconium, Titanium and Aluminium oxide among them); its material's texts read T, the "
        "physics' temperature (the library's gap Varshni's, Nc, Nv, mobilities of T); heterojunctions between them by "
        "Anderson's rule (their chi, Eg, Nc, Nv: Ge on Si's bands step 0.05 and 0.52 eV; a node on one the narrower gap's); its \"TL\" a lattice temperature for "
        "the mobilities (a heat physics' ht.T, (TL/300 K)^-zeta; semi.H the carriers' heat, Joule and recombination's, for "
        "its source: solved in turn). Each feature's parameter as set on it, else its material's (the library's semiconductors "
        "carry their own: Caughey-Thomas, saturation, Fossum, Auger caun/caup, radiative copt, impact iian..., band-to-band "
        "bbtA...; fem_describe what=material shows them), else silicon's (said in the log). Features: model (\"source\": "
        "material or user - epsilonr, Eg, chi, Nc, Nv, mun, mup; \"mobility\": \"doping\" for Caughey-Thomas; \"saturation\" with vsatn, vsatp, \"driving\": "
        "current (each carrier's quasi-Fermi gradient) or field; \"surface\": true for Lombardi's scattering at the nearest "
        "insulator's interface (Bn, Cn, lambdan, deltan, Bp..., lcrit); zetan, zetap; \"fermi\": true for Fermi-Dirac "
        "statistics; \"bgn\": true for Slotboom's narrowing (Ebgn, Nbgn, Cbgn); \"incomplete\": true for dopants ionized "
        "in part (Ed, gd, Ea, ga): semi.Ndi, semi.Nai), doping (\"dopant\": donor or "
        "acceptor; \"profile\": uniform with N0, box with N0, position, size, dj, Nb, implant with dose, Rp, dRp, dRl, "
        "surface (y), x1, x2 (a mask's window) and an anneal's D0, Ea, Ta, ta, or expression with N - another physics' u, "
        "a diffusion solved in time), srh (taun, taup, Et), auger, radiative, stimulated (c, Jth, t: a laser's, semi.Istim), "
        "impact (\"model\": material, silicon or user an, bn, "
        "ap, bp...; \"currents\": false for the ionization integrals alone - semi.In, semi.Ip, semi.Mn, semi.Mp, semi.Emax: "
        "breakdown where they reach 1), btbt (\"kind\": hurkx or generation; A, B, gamma), quantum (Schrödinger-Poisson: "
        "\"electrons\", \"holes\", nq/pq from a quantum wells physics' qw.n, qw.p - with a states step's \"consistent\": "
        "true, \"iterations\", \"sptolerance\"), generation (G), optical (a waveguide photodiode's light: \"mode\" the modes tag, \"P0\", \"eta\", "
        "\"RL\" - each photon its mode loses across the gap a pair, averaged over the thickness as the device's length; its "
        "study a stationary semi, a mode emw, a stationary semi: semi.Gop pairs/s, semi.Iop = q Gop, semi.Pin - a responsivity "
        "semi.I_k/semi.Pin -; then a frequency step: semi.Hop_<name>, semi.HopRL_<name>, the photocurrent modulated per pair, "
        "its bandwidth), ohmic (\"name\", \"V0\"), schottky (name, V0, phiB), gate (name, V0, phim: on an "
        "oxide's boundary), charge (rhos), polarization (wurtzite's bound charge -ΔP·n at interfaces between nitrides - their "
        "materials' compounds GaN, AlN, InN, AlGaN, InGaN, InAlN -, Psp and the piezoelectric part on \"substrate\"'s lattice; \"axis\" "
        "y or x the c axis, \"polarity\" metal or nitrogen: an AlGaN barrier on GaN's 2DEG), traps (an interface's: \"kind\" acceptor or donor, \"distribution\" level with "
        "Nit, Et or uniform with Dit, Elow, Ehigh, levels; sigman, sigmap, vth - their charge and recombination, small-signal "
        "their time), fn (Fowler-Nordheim through an insulator: phiB, mox - semi.Ifn after the solve). Globals semi.I_<name> (A into the device), semi.V_<name>, semi.Q_<name>; a frequency "
        "step after a stationary one gives the small-signal semi.G<a><b>, semi.C<a><b> (semi.C_a_b for longer names). Variables "
        "semi.V, semi.n, semi.p, semi.Ndop, semi.Ec, semi.Ev, semi.Efn, semi.Efp (J: unit eV), semi.Jx, semi.normJ, semi.R. Sweep "
        "a contact's voltage as a parameter (each point starts from the last). A modes wave feature's free carriers, N and P "
        "(\"Ncarriers\", \"Pcarriers\": semi.n, semi.p by default), at the mode's wavelength: \"plasma\": true - "
        "\"plasmamodel\" soref (silicon's, its absorption too), drude (-(q²λ²/8π²c²ε0 n)(N/me + P/mh), the material's me and "
        "its γ1, γ2's conductivity mass, or \"mepl\", \"mhpl\"; a III-V's) or user (dndN, dndP); \"fca\": true - free-carrier "
        "absorption σn N + σp P as λ² (the material's sigmaFCn, sigmaFCp, InP's family 1e-18 and 2e-17 cm², or \"fcasource\": "
        "\"user\"; with soref refused: counted twice); \"bandfilling\": true - Burstein-Moss, of the material's Eg (Egd), me, γ1, "
        "γ2 and Afk (a measured edge: the library's InP 1.16e5 per cm, Aspnes and Studna's) or Ep with Sommerfeld's excitonic "
        "enhancement of its epsilonr (\"bfexciton\"), Fermi-Dirac at \"Tbf\", Kramers-Kronig's Δn, the edge bleached no more "
        "than the medium absorbs (\"bfholes\"; or \"bfmodel\": \"user\" with Pbfn, Pbfp per carrier); emw.dncarriers, "
        "emw.alphacarriers. "
        "\"kerr\": true - Δn = ½ n³ R (|E|² - E0²) along x, y, z (\"Rkerr\" one or three in m²/V², \"E0kerr\"; the field Ex, "
        "Ey, Ez); \"qwabsorption\": true - a quantum wells physics' (\"qwtag\") absorption and Kramers-Kronig index at the "
        "mode's photon energy of the field at each point (\"Fqw\": semi.Ey), the wells solved at \"Fpoints\" fields to ±\"Fmax\" "
        "with a Spectrum step's options (\"spectrumstep\", one disabled too; its energies the window round the photon's), Δn "
        "from \"F0qw\" - emw.alphaqwTE, alphaqwTM, dnqwTE, dnqwTM. A derived value \"perturbation\" (\"dn\": "
        "emw.dncarriers, \"dalpha\": emw.alphacarriers, any expressions; \"selection\"; \"length\"; \"bias\": the swept "
        "parameter) gives each mode's dneff = (c ε0/2) ∫ n Δn |E|² / ∫ Sz, dalpha, dloss (dB/cm), dphi; along a sweep from its "
        "first point dphi_sweep and VpiL (V·cm; with a bias of no unit, cm times its unit). A mode step keeps \"minneff\" (an "
        "index above), \"maxloss\" (dB/cm below), \"tefraction\" (with a polarization: TE at it or more, TM at 1 less it or "
        "less) and \"choose\"s the mode shown and evaluated by default: first, share (most in \"track\"), te, tm (the "
        "TE-like, TM-like of the highest index: TE fraction over, under a half) - a plot group's \"mode\" 0, a 1D plot "
        "group's \"modes\" and a derived value's \"solutions\" \"auto\" (the chosen one where the step chose one; \"all\", "
        "\"chosen\" too; \"selected\" the plot group's or the export's own point, time, mode). \"track\" chooses by the "
        "share in its domains about the step's own \"neff\" (its \"follow\" then not followed). A \"mode\" asked that a "
        "solution has not is refused. An export \"fieldexport\" writes \"expressions\" on a grid (\"from\", "
        "\"to\", \"nx\", \"ny\") or at the mesh's nodes (\"where\": nodes) as csv, npy or dat. A mesh feature \"layered\" "
        "(\"selection\" rectangles of a stack, \"axis\" y, \"hlateral\", \"count\" across each layer) meshes thin layers as "
        "a grid of stretched triangles, not refined.\n"
        "Rays: \"rays\" (tag gop, Geometrical Optics; a stationary step traces them) - features medium (n, kappa from the "
        "material or \"source\": \"user\"; n may vary with x, y, lambda0, a field: graded, dispersive, thermal), point "
        "(\"position\", \"angle\", \"spread\" in degrees), beam (\"start\", \"end\", \"angle\", \"profile\" uniform or "
        "gaussian with \"waist\"), boundary (released normal to boundaries) - each with \"count\", \"P0\", \"lambda0\" (\"450[nm], "
        "650[nm]\") - mirror (R), absorber, coating (no reflection), detector (\"name\", \"stop\"). Globals gop.N, gop.Pin, "
        "gop.Pabs (absorbed in the media; gop.Q per volume, a heat source), gop.P_<det>, gop.x_<det>, gop.y_<det>, gop.rms_<det>, "
        "gop.opl_<det>. A plot group's \"rayplot\" draws them (\"by\": wavelength, power, release, single).\n"
        "Quantum wells: \"quantum\" (tag qw, Schrödinger's effective-mass equation of electrons, heavy and light holes) - "
        "\"space\": \"1d\" (a line across the layers through \"at\", elements of \"h1d\" - 0.1[nm] by default, a plain number "
        "metres; the error as h⁴, 0.5[nm] about 0.2 meV, fine for thick stacks; free in the layers' plane; about an axis "
        "along it, \"axis\": \"y\" - a VCSEL's wells) or "
        "\"2d\" (the cross-section: wires; dots about an axis); \"axis\": the growth axis y or x; \"substrate\" (a material, "
        "a compound or a lattice constant: biaxial strain); \"F\" (V/m along the axis: the Stark effect); \"V\" (another "
        "physics' potential, semi.V); \"T\"; \"electrons\", \"heavy\", \"light\". Bands from each material's \"compound\" "
        "(GaAs, AlAs, InAs, InP, GaP, GaSb, AlSb, InSb; alloys AlGaAs, InGaAs, InAlAs, GaInP, GaAsP, InAsP, GaAsSb with \"x\", "
        "InGaAsP In1-xGaxAsyP1-y and AlGaInAs AlxGayIn1-x-yAs with \"x\", \"y\" - library entries such as Indium gallium "
        "arsenide) or its Eg, VBO (or chi), me, gamma1, gamma2. A property given on the material or its library entry is used "
        "over the compound's in every physics (the library's Indium phosphide 1.344 eV in qw and semi alike): Eg given (or "
        "\"lambdag\", a gap wavelength: Eg = hc/λg, \"1.3Q\" 1.3[um]) is the gap at 300 K, moved with T as the compound's, the "
        "conduction band moving (VBO the compound's unless given); \"composition\": \"gap\" solves the alloy for hc/λg at 300 "
        "K instead - a ternary's x, a quaternary's x and y on \"lattice\" (InP) - every property of that composition. "
        "\"bands\": \"measured\" takes gaps fitted to 300 K "
        "measurements (InP 1.344, In0.53Ga0.47As 0.750, In0.52Al0.48As 1.46, InGaAsP within 7 meV of Nahory's, the 1.3 µm "
        "AlGaInAs 0.954) instead of Vurgaftman's, 9-16 meV off in the InP family. A feature material with \"source\": "
        "\"user\" gives Ec, Ev (of x, y), me, mhh, mlh, mhhp, mlhp, Ep, nr, epsilonr, gamma3 and Delta (the k·p bands'); with "
        "\"crystal\": \"wurtzite\" (c along the axis) A (A1-A6, six numbers), Dcr, Dso, mep (the electron across c), Epz (TM's Ep), "
        "exx (given strain) with ezz or C13, C33, D (D1-D6), a1, a2, Psp, e31, e33 - Ev Chuang and Chang's reference (the heavy holes' "
        "top Ev + Dcr + Dso/3 unstrained). The nitride compounds (GaN, AlN, InN, AlGaN, InGaN, InAlN: Vurgaftman and Meyer's) are "
        "wurtzite too, on the physics' \"substrate\"; one crystal a stack. Its "
        "\"valleys\" (either source; 1D wells): Si(100), Si(110), Si(111), Ge(100), Ge(110), Ge(111) (each with \"ml mt\" if not "
        "0.916 0.190 or 1.59 0.0815) or \"g mz md [dE]; ...\" - each set a ladder of electron states of its own (mz along the axis, "
        "md in the plane, g valleys), merged by energy (qw.Ve1... each state's set), carriers summed in qw.n; no optical "
        "spectrum with them. With etan or etap a well's carriers per area: qw.Ne1, qw.Nhh1... each state's, qw.Ns, qw.Ps, "
        "qw.Nv1... each valley set's (1/m²: shown in 1/cm²). "
        "\"polarization\" (wurtzite, 1D): periodic (no drop across the physics: an MQW's period) or ends (its first layer no field) - "
        "the layers' fields from their Psp and piezoelectric parts, screened by a spectrum's carriers (a loop, said in the log); "
        "\"polarity\" metal or nitrogen. "
        "\"valence\": \"kp4\" or \"kp6\" (1D wells) makes the holes Luttinger and Kohn's 4x4 or the 6x6 with the split-off "
        "band - mixed as the in-plane k grows, Burt and Foreman's operator order across interfaces -, \"kp8\" Kane's 8x8 (the "
        "electrons too, non-parabolic: their energies, masses and densities from it; qw.Ec1... in a bands step; Ep lowered where "
        "the conduction band's remote term would be below a free electron's, said in the log; no excitons), \"inplane\" axial (the "
        "default: Chuang's, the Lumerical one), 100, 110 or average (four directions, four times the work); a k·p spectrum's cost "
        "grows with the line's equations and the step's \"kpoints\" (121): \"h1d\" 0.25[nm] is near 0.1's. A \"bands\" step {\"kmax\": \"1.5[1/nm]\", "
        "\"points\": 41, \"states\": 4} makes each k a solution: qw.k (shown in 1/nm), qw.Ev1... (each state, Kramers' pairs both). Features zero (ψ = 0 where it ends: a "
        "hard wall - a state a few meV below the barriers' top feels it; thick barriers) and flux (∂ψ/∂n = 0, 2D). A \"states\" step "
        "{\"states\": 4} makes each state a solution (\"mode\" picks one): qw.Ee1, qw.Ehh1, qw.Elh1... (J, shown in eV), "
        "qw.E_e of the one selected, qw.Eg and qw.lambda (the effective gap), qw.Ee1hh1 and the overlap qw.Oe1hh1, qw.pos_e, "
        "qw.bound_e; variables qw.P_e (|ψ|²), qw.psi_e, qw.wave_e (a level with its |ψ|²: a band diagram), qw.Ec, qw.Ehh, "
        "qw.Elh, qw.Ev, qw.me, qw.strain. A \"spectrum\" step {\"energies\": \"linspace(0.75[eV], 0.95[eV], 201)\" (or "
        "wavelengths), \"states\", \"linewidth\" (half width at half maximum, eV), \"lineshape\" (lorentzian, gaussian, "
        "sech; for gain gaussian or sech - a Lorentzian's tails move the gain's zero some 10 meV below qw.dF, and the higher states' tails reach under the gain's peak: with a Lorentzian the peak depends on \"states\", 394 /cm with 2 against 304 with 4 in a 10 nm GaAs well - the Lumerical MQW solver keeps 2 conduction and 4 valence subbands, \"states\": 2 -, and the log warns past 10 %; qw.alphaTEfar, qw.alphaTMfar what the transitions more than 5 half widths above add by their tails), \"dlinewidth\" (its growth with the field across the wells, eV per V/m: qw.Fw, qw.linewidth), "
        "\"excitons\": true (each transition's 1s exciton - \"excitonmodel\" two, the default: e^(-√(ρ² + γ²(ze - zh)²)/λ), "
        "Khramtsov et al.'s exact GaAs/AlGaAs binding to 1.2 %, their measured radiative decay to 4 %; or bastard, γ 0, its "
        "line weaker in wide wells - a field weakens it -, and the continuum's 2D Sommerfeld factor; \"epsr\" its "
        "permittivity, else the wells' material's; none with carriers), \"continuum\": \"bound\" (the transitions of states "
        "confined by their own well's barriers - in a field too - and the barriers' bulk absorption over their thickness, not "
        "the box's states above them; over \"fade\" (0.1[eV]) above the barriers' gap the wells' steps fade into their own "
        "bulk absorption: past it the bulk's alone, the same in any field, and Δn between fields the same whatever the "
        "window's top past it), \"density\" (carriers in the wells: gain), \"nr\"} makes each photon energy a "
        "solution: qw.alphaTE, qw.alphaTM (1/m: unit 1/cm), qw.gTE, qw.gTM, qw.Fc, qw.Fv, qw.dF, qw.Lw; qw.dnTE, qw.dnTM (the "
        "index the window's absorption makes, Kramers-Kronig: between two fields Δn; qw.dnTEtail, qw.dnTMtail what the "
        "absorption above the window would add were it its top's; warned when the top absorbs short of the fade's end); "
        "with carriers qw.rsp (spontaneous emission per volume, time and energy: 1/(s cm³ eV)), qw.Rsp, qw.gspTE, qw.gspTM "
        "(the Lumerical g_sp); with excitons qw.Eb, qw.Ex, qw.aX, qw.gX (the e1-hh1 exciton's binding, line, radius and γ). Each "
        "transition's reduced mass is of its states' own in-plane masses (the inverse mass averaged over each |ψ|²); the "
        "physics' \"T\" moves the compounds' gaps (Varshni) and the carriers' kT.\n"
        "Your own PDE: \"pde\" (tag c by default) - ea ∂²u/∂t² + da ∂u/∂t + ∇·Γ = F; \"variables\": \"u\" or \"u, v\" (each "
        "with ux, uy, and c.u, c.ux; along a line us; of the second order in time their rates ut), \"unit\"; \"space\": \"2d\" "
        "(on the mesh, about an axis too) or \"1d\" (an ODE along a line: \"axis\" x or y, \"at\", \"elements\"). Features: "
        "coefficient (the coefficient form: c - or \"diffusion\": \"anisotropic\" with cxx, cxy, cyx, cyy -, a, f, ea, da, "
        "\"alpha\", \"beta\", \"gamma\" as [x, y]: Γ = -c∇u - αu + γ, F = f - β·∇u - au) or general (Gx, Gy, F, ea, da) - "
        "any expressions of x, y, t, the variables, their gradients, other fields (nonlinear: Newton's method); several "
        "variables, one item each split by ';' (\"f\": \"1; -v\"); init (u0, ut0); dirichlet (\"r\": u = r; along a line where "
        "it meets the boundaries chosen), flux (g, q: -n·Γ = g - q u), zeroflux (the default). Steps stationary, transient, "
        "and \"eigenvalue\" {\"count\": 6, \"shift\": 0}: -λ da u + ∇·Γ = F, each eigenvalue a solution with c.lambda; "
        "c.iterations Newton's.\n"
        "Expressions: numbers (1.5e-3, Qucs's 10u 2.2k, 3mm, 1GHz), units in brackets (3[mm], 20[degC], W/(m*K)), + - * / ^, "
        "functions (sin, exp, sqrt, min, max, if(c,a,b), interp(x, x0, y0, x1, y1, ...) through points, neps(e1, e2) and "
        "keps(e1, e2) the index and extinction of a permittivity...), constants with their units (%CONSTANTS% - q0*semi.R "
        "is a current's density, 1.602e-19*semi.R a plain number's: give the constant, not its value), the parameters. In the "
        "geometry and the mesh a plain number is in the component's unit and parameters are converted to it (L = 2[mm] "
        "is 2 in mm). A property's plain number is in its SI unit. Materials and sources may use x, y and fields (T, V, "
        "es.normE, ec.Qrh...); a material's n and kappa lambda0 - the library's of the wavelength, each checked against the "
        "measured spectra of the refractiveindex.info database: silicon (Green's, Li's) and germanium (Nunley's, Burnett's) n and "
        "κ; every zinc blende III-V and alloy Afromowitz's oscillator of its binaries at its gap and x, y (an alloy's bowed as "
        "half its gap; its ends its binaries' own; its gap broadened 20 meV: finite at it and on above it - InGaAsP Adachi's, "
        "its edge broadened 50 meV), its lattice's term in the infrared, κ its direct gap's edge above it (nought below; an "
        "alloy's of its binaries over µ^1.5 Ep); of T a compound's (\"compound\", at its material's T) its oscillator with "
        "its gap and E0 moved by the gap's shift - its dndT that derivative (the Thermo-optic option takes it); the library's "
        "entries' n at 300 K, their T the Thermo-optic option's measured dn/dT. SiGe silicon's and germanium's mixed (from 1.9 "
        "µm). The II-VIs, GaN, the oxides, nitrides and their films (ALD alumina, sputtered hafnia, PECVD nitride: n and κ), "
        "the crystals, water, the metals; nrange where each holds (a III-V's from 0.2 eV above its gap, mostly-indirect ones' "
        "from 0.15 eV below their direct gap) - a mode or wave solve warns outside it, fem_describe what=material notes it; a "
        "formula that fails past its range (a Sellmeier's pole) held at its end there. A "
        "wave feature's Direct-gap absorption and quantum wells' absorption take the place of a material's κ (the same edge), "
        "not added to it. A gate's or Schottky contact's metal beside it its work function phi.\n"
        "Selections (\"selection\"): {\"all\": true}, {\"numbers\": [1, 3]}, {\"objects\": [\"air\"]} (domains in them), "
        "{\"only\": [\"substrate\"]} (domains whose smallest object it is), {\"of\": [\"trace\"], \"side\": \"bottom\"} "
        "(boundaries on an object's edges, those facing down), {\"between\": [[\"a\"], [\"b\"]]}, {\"exterior\": true}, "
        "{\"interior\": true}, {\"adjacent\": {domain rule}}, {\"box\": [x0, y0, x1, y1]}, {\"named\": \"sel\"}; any with "
        "\"except\": {rule}. An array's name (no object of its own) names all its copies, row_1, row_2... (its input object's "
        "used up by it: named, the array said). Numbers change "
        "when the geometry does: rules do not.\n"
        "Results: after solving, variables at a point (x, y, t, V, T, es.Ex, es.normE, es.Dx, es.We, ec.Jx, ec.normJ, ec.Qrh, "
        "ht.qx, ht.normq, solid.u, solid.v, solid.disp, solid.sx, solid.mises, mf.Az, mf.Bx, mf.normB, mf.Hx, mf.Jz, mf.normJ, "
        "mf.Qrh, mf.Wm, mf.mur...; about an axis r, z, solid.sr, solid.sphi, mf.Aphi, mf.Br, mf.Bz, mf.psi; in the frequency "
        "domain the real part, and _im the imaginary: mf.Az_im, mf.Bx_im; freq) and globals (es.C11, es.C_1_2, es.Q_1, es.V_1, "
        "es.W, ec.G11, ec.R, ec.I_1, ec.P, ht.Tmax, ht.Tmin, ht.P, ht.Rth, solid.dmax, solid.Rx, solid.Ry, solid.W, mf.L_1, "
        "mf.R_1, mf.V_1, mf.Phi_1, mf.L11, mf.R_1_2, mf.W, mf.P). A terminal named 1 at 1 V in turn gives the capacitance (es) "
        "or conductance (ec) matrix; coils at 1 A in turn the inductance (impedance) matrix (mf). A parameter of a plain name "
        "(t, T, h) keeps it: time is then 'time'.")
        .replace(QLatin1String("%CONSTANTS%"), constantsText());
}

} // namespace

MultiphysicsDoc* QucsControl::femDocument(const QJsonObject& args, QString* error)
{
    const QString given = args.value(QLatin1String("path")).toString().trimmed();
    if (!given.isEmpty()) {
        QucsDoc* doc = document(QJsonObject{{QStringLiteral("path"), given}}, error);
        if (doc == nullptr) {
            const QString file = absolute(given);
            if (!QFileInfo(file).isFile()) {
                *error = tr("There is no model %1.").arg(QDir::toNativeSeparators(file));
                return nullptr;
            }
            if (!QucsApp::isMultiphysicsFile(file)) {
                *error = tr("%1 is not a multiphysics model (.qfem).").arg(QFileInfo(file).fileName());
                return nullptr;
            }
            misc::ErrorCapture said;
            if (!a_app->gotoPage(file)) {
                *error = tr("%1 could not be opened: %2").arg(QDir::toNativeSeparators(file), said.errors().join(QLatin1Char(' ')));
                return nullptr;
            }
            doc = document(QJsonObject{{QStringLiteral("path"), file}}, error);
        }
        auto* model = dynamic_cast<MultiphysicsDoc*>(doc);
        if (model == nullptr && doc != nullptr) *error = tr("%1 is not a multiphysics model.").arg(titleOf(doc));
        if (model != nullptr) a_lastModel = model;
        return model;
    }
    if (a_app->DocumentTab->count() > 0)
        if (auto* model = qobject_cast<MultiphysicsDoc*>(a_app->DocumentTab->currentWidget())) {
            a_lastModel = model;
            return model;
        }
    QList<MultiphysicsDoc*> open;
    for (QucsDoc* doc : a_app->allDocuments())
        if (auto* m = dynamic_cast<MultiphysicsDoc*>(doc)) open << m;
    if (open.size() == 1) return a_lastModel = open.first();
    // A plot group's display in front (<model>_<tag>.dpl beside it): its model.
    if (a_app->DocumentTab->count() > 0)
        if (auto* front = dynamic_cast<QucsDoc*>(a_app->DocumentTab->currentWidget()); front && front->getDocName().endsWith(QLatin1String(".dpl")))
            for (MultiphysicsDoc* m : open) {
                if (m->getDocName().isEmpty()) continue;
                const QFileInfo fi(m->getDocName());
                for (const FNode& g : m->model().results().children)
                    if (QFileInfo(front->getDocName()) == QFileInfo(fi.absoluteDir().filePath(fi.completeBaseName() + QLatin1Char('_') + g.tag + QStringLiteral(".dpl"))))
                        return a_lastModel = m;
            }
    // Else the one a tool last acted on (solved, plotted...), still open.
    if (a_lastModel && open.contains(a_lastModel.data())) return a_lastModel.data();
    QStringList names;
    for (MultiphysicsDoc* m : open) names << (m->getDocName().isEmpty() ? tr("(not saved)") : QFileInfo(m->getDocName()).fileName());
    *error = open.isEmpty() ? tr("No multiphysics model is open: fem_model opens or makes one.")
                            : tr("%1 models are open: which? ('path': %2)").arg(open.size()).arg(names.join(QLatin1String(", ")));
    return nullptr;
}

QJsonObject QucsControl::femDescribe(const QJsonObject& args)
{
    const QString what = args.value(QLatin1String("what")).toString(QStringLiteral("model"));
    if (what == QLatin1String("material")) {
        // A material's properties along one variable: a library entry (the
        // user's too), else a material of the model open (its tag or name).
        const QString name = args.value(QLatin1String("material")).toString().trimmed();
        if (name.isEmpty()) return errorResult(tr("'material': a library entry (fem_describe what=format lists them) or a material of the model"));
        std::optional<FNode> node;
        std::shared_ptr<ParameterScope> parameters;
        if (const std::optional<MaterialEntry> e = findMaterial(name, MultiphysicsPanel::userMaterials())) {
            node = materialNode(Model::blank(), *e);
            parameters = evaluateParameters(Model::blank());
        } else {
            QString error;
            MultiphysicsDoc* doc = femDocument(args, &error);
            if (doc == nullptr) return errorResult(tr("The library has no material %1, and %2").arg(name, error));
            for (const FNode& m : doc->model().materials().children)
                if (m.tag == name || m.name().compare(name, Qt::CaseInsensitive) == 0) node = m;
            if (!node) return errorResult(tr("Neither the library nor the model has a material %1.").arg(name));
            parameters = evaluateParameters(doc->model());
        }
        MaterialSweep sweep;
        sweep.variable = args.value(QLatin1String("vs")).toString(QStringLiteral("T"));
        // (Each variable's range when not given, and its unit.)
        struct Default {
            const char* name;
            double from, to;
            bool log;
            Dim dim;
        };
        const Default defaults[] = {{"T", 200, 500, false, Dim::temperature()}, {"N", 1e20, 1e26, true, Dim::of(-3)},
                                    {"x", 0, 1, false, Dim()},                    {"lambda", 0.4e-6, 2e-6, false, Dim::length()},
                                    {"F", 1e6, 1e8, true, Dim::of(1, 1, -3, -1)}};
        const Default* d = nullptr;
        for (const Default& c : defaults)
            if (sweep.variable == QLatin1String(c.name)) d = &c;
        if (!d) return errorResult(tr("'vs' is T, N, x, lambda or F"));
        auto quantity = [&](const char* key, double otherwise, const Dim& dim, QString* why) {
            const QJsonValue v = args.value(QLatin1String(key));
            if (v.isUndefined() || v.isNull()) return otherwise;
            const QString t = v.isDouble() ? formatNumber(v.toDouble(), 15) : v.toString();
            const std::optional<double> x = evaluateConstant(t, parameters->scope, dim, why);
            return x ? *x : std::numeric_limits<double>::quiet_NaN();
        };
        QString why;
        sweep.from = quantity("from", d->from, d->dim, &why);
        sweep.to = quantity("to", d->to, d->dim, &why);
        sweep.log = args.value(QLatin1String("log")).toBool(d->log);
        sweep.points = args.value(QLatin1String("points")).toInt(51);
        sweep.T = quantity("T", 300, Dim::temperature(), &why);
        sweep.N = quantity("N", 0, Dim::of(-3), &why);
        sweep.lambda = quantity("lambda", 1.55e-6, Dim::length(), &why);
        sweep.F = quantity("F", 0, Dim::of(1, 1, -3, -1), &why);
        sweep.x = quantity("x", std::numeric_limits<double>::quiet_NaN(), Dim(), &why);
        sweep.y = quantity("y", std::numeric_limits<double>::quiet_NaN(), Dim(), &why);
        if (!why.isEmpty()) return errorResult(why);
        QStringList keys;
        for (const QJsonValue& k : args.value(QLatin1String("properties")).toArray()) keys << k.toString();
        const MaterialCurves curves = materialCurves(*node, keys, sweep, parameters->scope);
        if (!curves.error.isEmpty()) return errorResult(curves.error);
        auto list = [](const std::vector<double>& v) {
            QJsonArray a;
            for (double x : v) a.append(std::isfinite(x) ? QJsonValue(formatNumber(x, 6).toDouble()) : QJsonValue());
            return a;
        };
        QJsonArray properties;
        for (const MaterialCurve& c : curves.curves) {
            QJsonObject p{{QStringLiteral("key"), c.key}, {QStringLiteral("label"), c.label}, {QStringLiteral("unit"), c.unit},
                          {QStringLiteral("values"), list(c.values)}};
            // (A crystal's along one of its axes.)
            if (c.axis >= 0) p.insert(QStringLiteral("axis"), QStringLiteral("x%1").arg(c.axis + 1));
            properties.append(p);
        }
        QJsonObject out{{QStringLiteral("material"), node->name()},
                        {QStringLiteral("variable"), curves.variable},
                        {QStringLiteral("unit"), curves.unit},
                        {QStringLiteral("at"), list(curves.at)},
                        {QStringLiteral("properties"), properties}};
        if (!curves.notes.isEmpty()) out.insert(QStringLiteral("notes"), QJsonArray::fromStringList(curves.notes));
        return jsonResult(out);
    }
    if (what == QLatin1String("format")) {
        // The kinds of node, each with its properties.
        QJsonArray kinds;
        const QString group = args.value(QLatin1String("group")).toString();
        for (const NodeKind* k : nodeKinds()) {
            if (k->fixed && k->properties.isEmpty()) continue;
            if (!group.isEmpty() && k->group != group && k->type != group && !k->type.startsWith(group + QLatin1Char('/'))) continue;
            QJsonArray props;
            for (const PropertyDef& p : k->properties) {
                // (As PropertyDef::Kind has them, in its order.)
                static const char* kindNames[] = {"expression", "pair", "text", "choice", "bool", "integer", "selection", "objects",
                                                  "points", "physics", "study", "rows", "terminal", "file", "time or frequency",
                                                  "sweep point", "dataset", "node tag"};
                const int ki = int(p.kind);
                QJsonObject po{{QStringLiteral("key"), p.key}, {QStringLiteral("label"), p.label},
                               {QStringLiteral("kind"), ki >= 0 && ki < int(std::size(kindNames)) ? QString::fromLatin1(kindNames[ki]) : QStringLiteral("value")}};
                if (p.kind == PropertyDef::Rows && !p.choiceLabels.isEmpty()) po.insert(QStringLiteral("columns"), QJsonArray::fromStringList(p.choiceLabels));
                if (!p.unit.isEmpty()) po.insert(QStringLiteral("unit"), p.unit);
                if (!p.defaultValue.isUndefined() && !(p.defaultValue.isString() && p.defaultValue.toString().isEmpty()))
                    po.insert(QStringLiteral("default"), p.defaultValue);
                if (!p.choices.isEmpty()) po.insert(QStringLiteral("choices"), QJsonArray::fromStringList(p.choices));
                if (!p.showIf.isEmpty()) po.insert(QStringLiteral("when"), p.showIf);
                if (p.spatial) po.insert(QStringLiteral("may use x, y, fields"), true);
                props.append(po);
            }
            QJsonObject ko{{QStringLiteral("type"), k->type}, {QStringLiteral("title"), k->title}};
            if (!k->group.isEmpty()) ko.insert(QStringLiteral("group"), k->group);
            if (k->selection != Level::None) ko.insert(QStringLiteral("selects"), levelName(k->selection));
            if (!props.isEmpty()) ko.insert(QStringLiteral("properties"), props);
            if (!k->children.isEmpty()) ko.insert(QStringLiteral("children"), QJsonArray::fromStringList(k->children));
            if (!k->help.isEmpty()) ko.insert(QStringLiteral("help"), k->help);
            kinds.append(ko);
        }
        // (A long text - an index of the wavelength, a metal's permittivity -
        // said by its length: fem_describe what=material gives its values.)
        QJsonArray materials;
        for (const MaterialEntry& e : builtinMaterials()) {
            QJsonObject properties = e.properties;
            for (auto it = properties.begin(); it != properties.end(); ++it)
                if (it.value().isString() && it.value().toString().size() > 160)
                    it.value() = tr("an expression of %1 characters (fem_describe what=material plots it)").arg(it.value().toString().size());
            materials.append(QJsonObject{{QStringLiteral("name"), e.name}, {QStringLiteral("category"), e.category}, {QStringLiteral("properties"), properties}});
        }
        // The text: all of it; of a group, its paragraphs that name the group,
        // its kinds or their tags ("text": "all" all the same, "none" none).
        const QString textWanted = args.value(QLatin1String("text")).toString();
        QString text = formatText();
        QString textNote;
        if (textWanted == QLatin1String("none")) {
            textNote = tr("the format's text left out ('text' none)");
            text.clear();
        } else if (!group.isEmpty() && textWanted != QLatin1String("all")) {
            QStringList words = {group};
            for (const NodeKind* k : nodeKinds()) {
                if (k->fixed && k->properties.isEmpty()) continue;
                if (k->group != group && k->type != group && !k->type.startsWith(group + QLatin1Char('/'))) continue;
                words << k->type << k->type.section(QLatin1Char('/'), -1);
                if (!k->prefix.isEmpty() && k->group == QLatin1String("physics")) words << k->prefix;
            }
            words.removeDuplicates();
            QStringList escaped;
            for (const QString& w : words) escaped << QRegularExpression::escape(w);
            // (Quoted, as a file writes it - "lineparams" -, or a tag.)
            const QRegularExpression named(QStringLiteral("\"(%1)\"|\\btag (%1)\\b|\\b(%1)\\.").arg(escaped.join(QLatin1Char('|'))));
            const QStringList paragraphs = text.split(QLatin1Char('\n'));
            QStringList kept;
            for (const QString& p : paragraphs)
                if (named.match(p).hasMatch()) kept << p;
            textNote = tr("of the format's text the %1 of %2 paragraphs that name %3 or its kinds ('text' all: the whole, %4 characters)")
                           .arg(kept.size())
                           .arg(paragraphs.size())
                           .arg(group)
                           .arg(text.size());
            text = kept.join(QLatin1Char('\n'));
        }
        QJsonObject out{{QStringLiteral("kinds"), kinds}};
        if (!text.isEmpty()) out.insert(QStringLiteral("format"), text);
        // (The library's materials: with a group not of materials, left out.)
        QStringList notes;
        if (!textNote.isEmpty()) notes << textNote;
        if (group.isEmpty() || group.startsWith(QLatin1String("material"))) out.insert(QStringLiteral("materials"), materials);
        else notes << tr("the library's materials left out (group material lists them)");
        if (!notes.isEmpty()) out.insert(QStringLiteral("note"), notes.join(QStringLiteral("; ")));
        return jsonResult(out);
    }
    QString error;
    MultiphysicsDoc* doc = femDocument(args, &error);
    if (doc == nullptr) return errorResult(error);
    const Model& m = doc->model();
    if (what == QLatin1String("geometry")) {
        const GeometryBuild& g = doc->geometry();
        if (!g.topology) return errorResult(tr("The geometry has not been built."));
        QJsonObject o = topologyJson(*g.topology, std::clamp(args.value(QLatin1String("max")).toInt(100), 1, 5000));
        if (!g.ok()) o.insert(QStringLiteral("error"), g.errors.value(g.failedAt));
        if (args.contains(QLatin1String("selection"))) {
            const Level level = levelOf(args.value(QLatin1String("level")).toString(QStringLiteral("domain")));
            QString why;
            const QVector<int> picked = resolveSelection(*g.topology, m, level, args.value(QLatin1String("selection")).toObject(), &why);
            o.insert(QStringLiteral("selection picks"), numbers(picked));
            if (!why.isEmpty()) o.insert(QStringLiteral("selection problem"), why);
        }
        return jsonResult(o);
    }
    if (what == QLatin1String("variables")) {
        QJsonArray vars, globals;
        const std::shared_ptr<Solution> sol = doc->solution(args.value(QLatin1String("study")).toString());
        const QList<Variable> list = sol ? sol->variables() : modelVariables(m);
        for (const Variable& v : list)
            vars.append(QJsonObject{{QStringLiteral("name"), v.name}, {QStringLiteral("unit"), dimName(v.dim)}, {QStringLiteral("is"), v.description}});
        if (sol)
            for (const Variable& v : sol->globals())
                globals.append(QJsonObject{{QStringLiteral("name"), v.name},
                                           {QStringLiteral("value"), sol->globalText(v.name)},
                                           {QStringLiteral("is"), v.description}});
        QJsonObject o{{QStringLiteral("at a point"), vars}};
        if (sol) o.insert(QStringLiteral("globals"), globals);
        else o.insert(QStringLiteral("globals"), tr("none until the study is solved (fem_solve)"));
        return jsonResult(o);
    }
    // The model: its tree, its parameters' values, what is built.
    const int depth = std::clamp(args.value(QLatin1String("depth")).toInt(8), 1, 16);
    QJsonObject o{{QStringLiteral("file"), doc->getDocName().isEmpty() ? tr("(not saved yet)") : QDir::toNativeSeparators(doc->getDocName())},
                  {QStringLiteral("tree"), nodeJson(m.root, depth)}};
    if (doc->getDocChanged()) o.insert(QStringLiteral("unsaved"), true);
    if (!doc->readNotes().isEmpty()) o.insert(QStringLiteral("read with notes"), doc->readNotes());
    const std::shared_ptr<ParameterScope> ps = doc->parameters();
    QJsonArray params;
    for (const Model::Parameter& p : m.parameters()) {
        QJsonObject po{{QStringLiteral("name"), p.name}, {QStringLiteral("expression"), p.expression}};
        if (ps->errors.contains(p.name)) po.insert(QStringLiteral("error"), ps->errors.value(p.name));
        else if (ps->values.contains(p.name)) po.insert(QStringLiteral("value"), formatQuantity(ps->values.value(p.name), ps->dims.value(p.name)));
        params.append(po);
    }
    o.insert(QStringLiteral("parameters"), params);
    const GeometryBuild& g = doc->geometry();
    if (g.topology)
        o.insert(QStringLiteral("geometry"), QJsonObject{{QStringLiteral("domains"), int(g.topology->domains.size())},
                                                         {QStringLiteral("boundaries"), int(g.topology->boundaries.size())},
                                                         {QStringLiteral("points"), int(g.topology->points.size())},
                                                         {QStringLiteral("objects"), QJsonArray::fromStringList(g.topology->objectNames)}});
    if (!g.ok()) o.insert(QStringLiteral("geometry error"), tr("%1: %2").arg(g.failedAt, g.errors.value(g.failedAt)));
    if (doc->mesh() && !doc->meshStale())
        o.insert(QStringLiteral("mesh"), tr("%1 triangles, smallest angle %2°").arg(doc->mesh()->triangles.size()).arg(doc->mesh()->minAngle, 0, 'f', 1));
    QJsonArray studies;
    for (const FNode* s : m.studies())
        studies.append(QJsonObject{{QStringLiteral("tag"), s->tag},
                                   {QStringLiteral("name"), s->name()},
                                   {QStringLiteral("solved"), doc->solution(s->tag) != nullptr},
                                   {QStringLiteral("changed since"), doc->solutionStale(s->tag)}});
    o.insert(QStringLiteral("studies"), studies);
    if (!doc->lastError().isEmpty()) o.insert(QStringLiteral("last error"), doc->lastError());
    return jsonResult(o);
}

QJsonObject QucsControl::femModel(const QJsonObject& args)
{
    const QString action = args.value(QLatin1String("action")).toString();
    const QString given = args.value(QLatin1String("path")).toString().trimmed();
    // A file open in a tab that 'new' or 'import' writes over ('replace'):
    // that tab takes the new model - one tab of the file, the tools on it
    // (a second tab of it, the tools found the first: its old model solved,
    // nothing said); its changes not saved are not thrown away.
    auto tabOf = [this](const QString& file, MultiphysicsDoc** open) -> QString {
        *open = nullptr;
        for (QucsDoc* other : a_app->allDocuments())
            if (!other->getDocName().isEmpty() && sameFile(other->getDocName(), file)) {
                *open = dynamic_cast<MultiphysicsDoc*>(other);
                if (*open == nullptr) return tr("%1 is open: another kind of document's file.").arg(QDir::toNativeSeparators(file));
                if ((*open)->getDocChanged())
                    return tr("%1 is open, its changes not saved: save or close it first ('replace' does not throw them away).")
                        .arg(QDir::toNativeSeparators(file));
            }
        return QString();
    };
    if (action == QLatin1String("new")) {
        std::optional<Model> m;
        if (args.contains(QLatin1String("model"))) {
            QString why;
            m = Model::fromJson(QJsonDocument(args.value(QLatin1String("model")).toObject()).toJson(), &why);
            if (!m) return errorResult(tr("'model': %1").arg(why));
        }
        QString file;
        MultiphysicsDoc* open = nullptr;
        QStringList notes;
        if (!given.isEmpty()) {
            file = absolute(given);
            if (!QucsApp::isMultiphysicsFile(file)) file += QStringLiteral(".qfem");
            if (QFileInfo::exists(file) && !args.value(QLatin1String("replace")).toBool())
                return errorResult(tr("%1 is there already ('replace' writes over it).").arg(QDir::toNativeSeparators(file)));
            const QString busy = tabOf(file, &open);
            if (!busy.isEmpty()) return errorResult(busy);
            // A relative path whose first folder is one of the workspace's,
            // not the open project's: it went into the project all the same
            // (the project first) - said.
            const QString project = QDir::cleanPath(QucsSettings.QucsWorkDir.absolutePath());
            const QString workspace = QDir::cleanPath(QucsSettings.qucsWorkspaceDir.absolutePath());
            const QString first = QDir::fromNativeSeparators(given).section(QLatin1Char('/'), 0, 0);
            if (!QFileInfo(given).isAbsolute() && given.contains(QRegularExpression(QStringLiteral("[/\\\\]"))) && project != workspace
                && file.startsWith(project + QLatin1Char('/')) && QFileInfo(QDir(workspace).filePath(first)).isDir()
                && !QFileInfo(QDir(project).filePath(first)).isDir())
                notes << tr("%1 is a folder of the workspace, but a relative path is taken in the open project %2 first: written to %3 "
                            "(the workspace's: %4)")
                             .arg(first, QFileInfo(project).fileName(), QDir::toNativeSeparators(file),
                                  QDir::toNativeSeparators(QDir(workspace).filePath(QDir::fromNativeSeparators(given))));
            // (Its folder made if it is not there.)
            const QString folder = QFileInfo(file).absolutePath();
            if (!QFileInfo(folder).isDir()) {
                if (!QDir().mkpath(folder))
                    return errorResult(tr("The folder %1 cannot be made: the model is not written.").arg(QDir::toNativeSeparators(folder)));
                notes << tr("the folder %1 made").arg(QDir::toNativeSeparators(folder));
            }
        }
        MultiphysicsDoc* doc = open ? open : a_app->newMultiphysicsModel();
        if (open) a_app->DocumentTab->setCurrentWidget(open);
        if (m || open) {
            doc->setModel(m ? *m : Model::blank(), tr("Model"));
            doc->undoStack()->clear();
        }
        if (!file.isEmpty()) {
            doc->setName(file);
            if (doc->save() != 0) {
                // (Not left open, unsaved, in the file's name: a tab of its
                // own made for it closed.)
                if (!open) {
                    doc->setDocChanged(false);
                    a_app->slotFileClose(a_app->DocumentTab->indexOf(QucsApp::documentWidget(doc)));
                }
                return errorResult(tr("%1 cannot be written.").arg(QDir::toNativeSeparators(file)));
            }
            a_app->titleDocumentTab(doc);
        }
        QString why;
        doc->buildGeometry(&why);
        // (Its title: what names it to the other tools - 'path' - while it has
        // no file.)
        QJsonObject out{{QStringLiteral("file"), doc->getDocName().isEmpty() ? tr("(not saved yet)") : QDir::toNativeSeparators(doc->getDocName())},
                        {QStringLiteral("title"), titleOf(doc)},
                        {QStringLiteral("geometry"), why.isEmpty() ? tr("built") : why}};
        if (!notes.isEmpty()) out.insert(QStringLiteral("note"), notes.join(QStringLiteral("; ")));
        return jsonResult(out);
    }
    if (action == QLatin1String("materials")) {
        // Into My Materials (the user's library): a library file's, or a
        // Lumerical project's materials - an .ldev's database, an .lms's or
        // .fsp's optical ones.
        if (given.isEmpty()) return errorResult(tr("'path': a library (.json) or a Lumerical project (.ldev, .lms, .fsp)."));
        const QString source = absolute(given);
        QString why;
        QStringList notes;
        const QList<MaterialEntry> taken = source.endsWith(QLatin1String(".json"), Qt::CaseInsensitive) ? readMaterials(source, &why)
                                                                                                        : qucs_s::fem::lumericalMaterials(source, &why, &notes);
        if (taken.isEmpty()) return errorResult(why.isEmpty() ? tr("%1 has no materials").arg(given) : why);
        QList<MaterialEntry> mine = MultiphysicsPanel::userMaterials();
        QJsonArray names;
        for (const MaterialEntry& e : taken) {
            mine.erase(std::remove_if(mine.begin(), mine.end(), [&](const MaterialEntry& x) { return x.name == e.name; }), mine.end());
            mine << e;
            names.append(e.name);
        }
        QDir().mkpath(QFileInfo(MultiphysicsPanel::userMaterialsFile()).absolutePath());
        if (!writeMaterials(MultiphysicsPanel::userMaterialsFile(), mine, &why)) return errorResult(why);
        QJsonObject out{{QStringLiteral("materials"), names}, {QStringLiteral("library"), MultiphysicsPanel::userMaterialsFile()}};
        if (!notes.isEmpty()) out.insert(QStringLiteral("read"), QJsonArray::fromStringList(notes));
        return jsonResult(out);
    }
    if (action == QLatin1String("import")) {
        // A Lumerical project (.ldev, .lms) or script (.lsf) made a model:
        // written beside it (or to 'to'), opened - what was read and how it
        // was taken said.
        if (given.isEmpty()) return errorResult(tr("'path': the Lumerical project (.ldev, .lms) or script (.lsf)."));
        const QString source = absolute(given);
        const qucs_s::fem::LumericalImport imported = qucs_s::fem::importLumerical(source);
        if (!imported.error.isEmpty()) return errorResult(imported.error);
        QString why;
        std::optional<Model> m = Model::fromJson(QJsonDocument(imported.model).toJson(), &why);
        if (!m) return errorResult(tr("The model made of it does not read: %1").arg(why));
        const QString to = args.value(QLatin1String("to")).toString().trimmed();
        QString file = to.isEmpty() ? QFileInfo(source).dir().filePath(QFileInfo(source).completeBaseName() + QStringLiteral(".qfem")) : absolute(to);
        if (!QucsApp::isMultiphysicsFile(file)) file += QStringLiteral(".qfem");
        if (QFileInfo::exists(file) && !args.value(QLatin1String("replace")).toBool())
            return errorResult(tr("%1 is there already ('replace' writes over it).").arg(QDir::toNativeSeparators(file)));
        MultiphysicsDoc* open = nullptr;
        if (const QString busy = tabOf(file, &open); !busy.isEmpty()) return errorResult(busy);
        // (Its folder made if it is not there.)
        if (!QDir().mkpath(QFileInfo(file).absolutePath()))
            return errorResult(tr("%1 cannot be made: the model is not written.").arg(QDir::toNativeSeparators(QFileInfo(file).absolutePath())));
        MultiphysicsDoc* doc = open ? open : a_app->newMultiphysicsModel();
        if (open) a_app->DocumentTab->setCurrentWidget(open);
        doc->setModel(*m, tr("Model"));
        doc->undoStack()->clear();
        doc->setName(file);
        if (doc->save() != 0) {
            // (Not left open, unsaved, in the file's name: a later import or
            // write of it would meet this tab - one of its own made for it.)
            if (!open) {
                doc->setDocChanged(false);
                a_app->slotFileClose(a_app->DocumentTab->indexOf(QucsApp::documentWidget(doc)));
            }
            return errorResult(tr("%1 cannot be written.").arg(QDir::toNativeSeparators(file)));
        }
        a_app->titleDocumentTab(doc);
        QString built;
        doc->buildGeometry(&built);
        return jsonResult(QJsonObject{{QStringLiteral("file"), QDir::toNativeSeparators(file)},
                                      {QStringLiteral("read"), QJsonArray::fromStringList(imported.notes)},
                                      {QStringLiteral("geometry"), built.isEmpty() ? tr("built") : built}});
    }
    if (action == QLatin1String("open")) {
        QString error;
        MultiphysicsDoc* doc = femDocument(QJsonObject{{QStringLiteral("path"), given}}, &error);
        if (doc == nullptr) return errorResult(error);
        a_app->DocumentTab->setCurrentWidget(doc);
        return femDescribe(QJsonObject{{QStringLiteral("path"), doc->getDocName()}, {QStringLiteral("depth"), 3}});
    }
    QString error;
    if (action == QLatin1String("save")) {
        // Which model: the one 'path' names - an open model's file or title
        // (whichever is in front: another conversation's may be), a model's
        // file opened -, else the one in front; 'as' a file to save it as.
        // A 'path' that is no model's file and no file at all is taken as
        // 'as' was before it: the model in front saved as it - an untitled
        // one only, never one with a file of its own written elsewhere.
        QString as = args.value(QLatin1String("as")).toString().trimmed();
        MultiphysicsDoc* doc = nullptr;
        if (!given.isEmpty()) {
            QString none;
            if (!dynamic_cast<MultiphysicsDoc*>(document(QJsonObject{{QStringLiteral("path"), given}}, &none)) && as.isEmpty()) {
                QString file = absolute(given);
                if (!QucsApp::isMultiphysicsFile(file)) file += QStringLiteral(".qfem");
                if (!QFileInfo::exists(file)) {
                    doc = femDocument(QJsonObject(), &error);
                    if (doc == nullptr) return errorResult(error);
                    if (!doc->getDocName().isEmpty())
                        return errorResult(tr("No model is %1. The one in front is %2 - 'as' saves it under another file, 'path' names the "
                                              "model saved").arg(QDir::toNativeSeparators(file), titleOf(doc)));
                    as = file;
                }
            }
        }
        if (doc == nullptr) doc = femDocument(given.isEmpty() ? QJsonObject() : QJsonObject{{QStringLiteral("path"), given}}, &error);
        if (doc == nullptr) return errorResult(error);
        const QString was = titleOf(doc);
        QString file = doc->getDocName();
        if (!as.isEmpty()) {
            file = absolute(as);
            if (!QucsApp::isMultiphysicsFile(file)) file += QStringLiteral(".qfem");
            const bool own = !doc->getDocName().isEmpty() && sameFile(doc->getDocName(), file);
            if (!own && QFileInfo::exists(file) && !args.value(QLatin1String("replace")).toBool())
                return errorResult(tr("%1 is there already ('replace' writes over it).").arg(QDir::toNativeSeparators(file)));
            for (QucsDoc* other : a_app->allDocuments())
                if (other != doc && !other->getDocName().isEmpty() && sameFile(other->getDocName(), file))
                    return errorResult(tr("%1 is open: another document's file.").arg(QDir::toNativeSeparators(file)));
        }
        if (file.isEmpty()) return errorResult(tr("%1 has no file yet: give 'as'.").arg(was));
        doc->setName(file);
        if (doc->save() != 0) return errorResult(tr("%1 cannot be written.").arg(QDir::toNativeSeparators(file)));
        a_app->titleDocumentTab(doc);
        return jsonResult(QJsonObject{{QStringLiteral("saved"), QDir::toNativeSeparators(file)}, {QStringLiteral("model"), was}});
    }
    MultiphysicsDoc* doc = femDocument(args, &error);
    if (action == QLatin1String("set")) {
        if (doc == nullptr) return errorResult(error);
        QString why;
        std::optional<Model> m = Model::fromJson(QJsonDocument(args.value(QLatin1String("model")).toObject()).toJson(), &why);
        if (!m) return errorResult(tr("'model': %1").arg(why));
        doc->setModel(*m, tr("Model"));
        QString built;
        doc->buildGeometry(&built);
        QJsonObject o{{QStringLiteral("set"), tr("the whole model, one step of Undo")},
                      {QStringLiteral("geometry"), built.isEmpty() ? tr("built") : built}};
        if (!why.isEmpty()) o.insert(QStringLiteral("notes"), why);
        return jsonResult(o);
    }
    return errorResult(tr("'action' is new, open, save, set or import."));
}

QJsonObject QucsControl::femEdit(const QJsonObject& args)
{
    QString error;
    MultiphysicsDoc* doc = femDocument(args, &error);
    if (doc == nullptr) return errorResult(error);
    const QString action = args.value(QLatin1String("action")).toString();
    Model m = doc->model();
    const QJsonObject props = args.value(QLatin1String("props")).toObject();
    // Properties set: only those its kind has (a material: and what a
    // library entry gives one - its semiconductor models' parameters too).
    auto apply = [&](FNode& n) -> QString {
        const NodeKind* k = n.kind();
        for (auto it = props.begin(); it != props.end(); ++it) {
            if (k && !k->property(it.key()) && !(n.type == QLatin1String("material") && isMaterialKey(it.key()))) {
                QStringList keys;
                for (const PropertyDef& p : k->properties) keys << p.key;
                if (n.type == QLatin1String("material"))
                    for (const QString& key : materialKeys())
                        if (!keys.contains(key)) keys << key;
                return tr("%1 has no property %2 (it has %3)").arg(k->title, it.key(), keys.join(QStringLiteral(", ")));
            }
            if (it.value().isNull()) n.props.remove(it.key());
            else {
                QJsonValue v = it.value();
                if (it.key() == QLatin1String("selection") && v.isArray()) v = QJsonObject{{QStringLiteral("numbers"), v}};
                n.props.insert(it.key(), v);
            }
        }
        if (args.contains(QLatin1String("label"))) {
            const QString label = args.value(QLatin1String("label")).toString().trimmed();
            if (k && k->group == QLatin1String("geometry") && n.type != QLatin1String("form union")) {
                if (label.isEmpty()) return tr("a geometry object's name may not be empty");
                if (label != n.label && m.objectNames().contains(label)) return tr("there is an object %1 already").arg(label);
                const QString old = n.label;
                n.label = label;
                m.renameObject(old, label);
            } else {
                n.label = label;
            }
        }
        if (args.contains(QLatin1String("enabled"))) n.enabled = args.value(QLatin1String("enabled")).toBool();
        return {};
    };
    QString what;
    QJsonObject answer;
    if (action == QLatin1String("add")) {
        const QString parentName = args.value(QLatin1String("parent")).toString();
        FNode* parent = parentNamed(m, parentName);
        if (!parent) return errorResult(tr("There is no node %1 to add to.").arg(parentName));
        QString type = args.value(QLatin1String("type")).toString().trimmed();
        FNode node;
        if (type.startsWith(QLatin1String("material:")) || (type == QLatin1String("material") && args.contains(QLatin1String("library")))) {
            const QString name = type.startsWith(QLatin1String("material:")) ? type.mid(9) : args.value(QLatin1String("library")).toString();
            const std::optional<MaterialEntry> e = findMaterial(name, MultiphysicsPanel::userMaterials());
            if (!e) return errorResult(tr("The library has no material %1 (fem_describe what=format lists them).").arg(name));
            node = materialNode(m, *e);
            parent = &m.materials();
        } else {
            // A feature's short type under its physics: "terminal".
            if (!nodeKind(type) && nodeKind(parent->type + QLatin1Char('/') + type)) type = parent->type + QLatin1Char('/') + type;
            if (!nodeKind(type)) return errorResult(tr("%1 is not a kind of node (fem_describe what=format lists them).").arg(type));
            node = m.make(type);
            if (type == QLatin1String("plotgroup") && !m.studies().empty()) node.props.insert(QStringLiteral("study"), m.studies().front()->tag);
        }
        const QString why = apply(node);
        if (!why.isEmpty()) return errorResult(why);
        const int at = args.contains(QLatin1String("index")) ? args.value(QLatin1String("index")).toInt() : -1;
        QString refused;
        FNode* added = m.add(parent->tag, node, at, &refused);
        if (!added) return errorResult(refused);
        answer.insert(QStringLiteral("added"), nodeJson(*added, 2));
        what = tr("Add %1").arg(added->name());
    } else if (action == QLatin1String("set")) {
        FNode* n = m.find(args.value(QLatin1String("tag")).toString());
        if (!n) return errorResult(tr("There is no node %1.").arg(args.value(QLatin1String("tag")).toString()));
        const QString why = apply(*n);
        if (!why.isEmpty()) return errorResult(why);
        answer.insert(QStringLiteral("set"), nodeJson(*n, 1));
        what = tr("Change %1").arg(n->name());
    } else if (action == QLatin1String("delete")) {
        const QString tag = args.value(QLatin1String("tag")).toString();
        const FNode* n = m.find(tag);
        if (!n) return errorResult(tr("There is no node %1.").arg(tag));
        what = tr("Delete %1").arg(n->name());
        QString why;
        if (!m.remove(tag, &why)) return errorResult(why);
        answer.insert(QStringLiteral("deleted"), tag);
    } else if (action == QLatin1String("move")) {
        const QString tag = args.value(QLatin1String("tag")).toString();
        int index = -1;
        FNode* parent = m.parentOf(tag, &index);
        if (!parent) return errorResult(tr("There is no node %1.").arg(tag));
        const int to = std::clamp(index + args.value(QLatin1String("by")).toInt(), 0, int(parent->children.size()) - 1);
        FNode moving = parent->children[std::size_t(index)];
        parent->children.erase(parent->children.begin() + index);
        parent->children.insert(parent->children.begin() + to, moving);
        if (parent->type == QLatin1String("geometry") && parent->children.back().type != QLatin1String("form union"))
            return errorResult(tr("Form Union stays the geometry's last feature."));
        answer.insert(QStringLiteral("moved"), QJsonObject{{QStringLiteral("tag"), tag}, {QStringLiteral("index"), to}});
        what = tr("Move %1").arg(moving.name());
    } else if (action == QLatin1String("parameters")) {
        QList<Model::Parameter> list;
        for (const QJsonValue& v : args.value(QLatin1String("parameters")).toArray()) {
            const QJsonObject o = v.toObject();
            list.append({o.value(QStringLiteral("name")).toString(), o.value(QStringLiteral("expression")).toString(),
                         o.value(QStringLiteral("description")).toString()});
        }
        m.setParameters(list);
        what = tr("Change Parameters");
        const std::shared_ptr<ParameterScope> ps = evaluateParameters(m);
        QJsonArray values;
        for (const Model::Parameter& p : list) {
            QJsonObject po{{QStringLiteral("name"), p.name}};
            if (ps->errors.contains(p.name)) po.insert(QStringLiteral("error"), ps->errors.value(p.name));
            else po.insert(QStringLiteral("value"), formatQuantity(ps->values.value(p.name), ps->dims.value(p.name)));
            values.append(po);
        }
        answer.insert(QStringLiteral("parameters"), values);
    } else {
        return errorResult(tr("'action' is add, set, delete, move or parameters."));
    }
    doc->setModel(m, what);
    // The geometry built at once: its errors said with the change.
    QString built;
    if (doc->geometryStale()) doc->buildGeometry(&built);
    const GeometryBuild& g = doc->geometry();
    if (g.topology)
        answer.insert(QStringLiteral("geometry"), g.ok() ? tr("%1 domains, %2 boundaries, %3 points").arg(g.topology->domains.size()).arg(g.topology->boundaries.size()).arg(g.topology->points.size())
                                                         : tr("stopped at %1: %2").arg(g.failedAt, g.errors.value(g.failedAt)));
    answer.insert(QStringLiteral("undo"), tr("one step of Edit > Undo; not saved (fem_model save)"));
    return jsonResult(answer);
}

QJsonObject QucsControl::femBuild(const QJsonObject& args)
{
    QString error;
    MultiphysicsDoc* doc = femDocument(args, &error);
    if (doc == nullptr) return errorResult(error);
    const QString what = args.value(QLatin1String("what")).toString(QStringLiteral("geometry"));
    QString why;
    if (!doc->buildGeometry(&why)) return errorResult(tr("The geometry stopped at %1").arg(why));
    if (what == QLatin1String("geometry")) {
        doc->showGeometry();
        return femDescribe(QJsonObject{{QStringLiteral("path"), doc->getDocName().isEmpty() ? QString() : doc->getDocName()},
                                       {QStringLiteral("what"), QStringLiteral("geometry")},
                                       {QStringLiteral("max"), args.value(QLatin1String("max")).toInt(100)}});
    }
    if (what != QLatin1String("mesh")) return errorResult(tr("'what' is geometry or mesh."));
    doc->buildMesh();
    const int timeout = std::clamp(args.value(QLatin1String("timeout")).toInt(300), 1, 3600);
    if (!doc->waitForJob(timeout * 1000)) return errorResult(tr("The mesh is still being built after %1 s (fem_build again waits more).").arg(timeout));
    if (!doc->lastError().isEmpty()) return errorResult(doc->lastError());
    const std::shared_ptr<const Mesh> mesh = doc->mesh();
    if (!mesh) return errorResult(tr("The mesh failed."));
    QJsonArray histogram;
    for (int c : mesh->histogram) histogram.append(c);
    return jsonResult(QJsonObject{{QStringLiteral("triangles"), int(mesh->triangles.size())},
                                  {QStringLiteral("nodes"), int(mesh->nodes.size())},
                                  {QStringLiteral("smallest angle"), QStringLiteral("%1°").arg(mesh->minAngle, 0, 'f', 1)},
                                  {QStringLiteral("quality"), QJsonObject{{QStringLiteral("worst"), mesh->minQuality},
                                                                          {QStringLiteral("mean"), mesh->meanQuality},
                                                                          {QStringLiteral("tenths"), histogram}}}});
}

QJsonObject QucsControl::femStop(const QJsonObject& args)
{
    // As the model tab's Cancel: the job asked to stop, waited for - it
    // stops at its next check (a time step, a frequency, a sweep's point, a
    // coupled loop's iteration, a semiconductor's Newton step).
    QString error;
    MultiphysicsDoc* doc = femDocument(args, &error);
    if (doc == nullptr) return errorResult(error);
    if (!doc->isBusy())
        return jsonResult(QJsonObject{{QStringLiteral("stopped"), false},
                                      {QStringLiteral("note"), tr("Nothing is being built or solved in %1.").arg(titleOf(doc))}});
    const qsizetype before = doc->messages().size();
    doc->cancelJob();
    const int timeout = std::clamp(args.value(QLatin1String("timeout")).toInt(30), 1, 600);
    if (!doc->waitForJob(timeout * 1000))
        return errorResult(tr("Asked to stop, still at work after %1 s: it stops at its next check (fem_stop again waits more).").arg(timeout));
    QJsonObject o{{QStringLiteral("stopped"), true}};
    // What it said as it stopped (the solutions it had kept stay).
    QJsonArray said;
    const QStringList messages = doc->messages();
    for (qsizetype i = std::max<qsizetype>(0, std::min(before, messages.size()) - 1); i < messages.size(); ++i) said.append(messages.at(i));
    if (!said.isEmpty()) o.insert(QStringLiteral("log"), said);
    return jsonResult(o);
}

/// A mode (state) asked of \a set, of modes: from 1 to the most it has -
/// else why not (it was clamped: mode 5 of 4 gave the 4th, 0 the 1st, and a
/// plot group kept "mode": 9).
static QString modeAsked(const SolutionSet& set, int mode)
{
    int most = 0;
    bool modal = false;
    for (const auto& s : set.solutions) {
        modal = modal || s->modeAnalysis;
        for (int k = 0; k < s->snapshotCount(); ++k) most = std::max(most, s->snapshotMode(k));
    }
    const QString what = !set.solutions.empty() && set.solutions.front()->quantumStates ? QObject::tr("state") : QObject::tr("mode");
    // (No modes here: not this study's - fem_evaluate looks in the others.)
    if (!modal) return QString();
    if (mode < 1 || mode > most) return QObject::tr("%1 %2: it has %3 (from 1 to %3)").arg(what).arg(mode).arg(most);
    return QString();
}

QJsonObject QucsControl::femSolve(const QJsonObject& args)
{
    QString error;
    MultiphysicsDoc* doc = femDocument(args, &error);
    if (doc == nullptr) return errorResult(error);
    const QString study = args.value(QLatin1String("study")).toString();
    if (doc->isBusy() && !doc->waitForJob(1000)) return errorResult(tr("It is busy building or solving; fem_solve again in a while."));
    doc->compute(study);
    const int timeout = std::clamp(args.value(QLatin1String("timeout")).toInt(600), 1, 7200);
    if (!doc->waitForJob(timeout * 1000))
        return errorResult(tr("Still solving after %1 s: it goes on (fem_describe says when it is solved; fem_stop stops it).").arg(timeout));
    if (!doc->lastError().isEmpty()) return errorResult(doc->lastError());
    const std::shared_ptr<SolutionSet> set = doc->solutions(study);
    if (!set || set->solutions.empty()) return errorResult(tr("It was not solved."));
    // A solution's globals as said: at its last output time - a mode
    // analysis's the mode chosen there, as fem_evaluate's (its last mode
    // was said at a sweep's last point, the chosen one at the others).
    auto atItsLast = [](Solution& s) {
        int k = s.snapshotCount() - 1;
        // (A resonator's: the mode kept for the steps after.)
        if (s.eigenfrequencies)
            for (int j = 0; j < s.snapshotCount(); ++j)
                if (s.snapshotChosen(j)) k = j;
        if (s.modeAnalysis && !s.eigenfrequencies && k >= 0) {
            int first = k;
            for (int j = k; j >= 0 && s.snapshotFrequency(j) == s.snapshotFrequency(k); --j) first = j;
            k = first;
            for (int j = first; j < s.snapshotCount(); ++j)
                if (s.snapshotChosen(j)) {
                    k = j;
                    break;
                }
        }
        s.select(k);
    };
    const std::shared_ptr<Solution> sol = set->solutions.back();
    atItsLast(*sol);
    QJsonObject globals;
    for (const Variable& v : sol->globals()) globals.insert(v.name, sol->globalText(v.name));
    // A sweep's solutions, each with its globals (at its last time).
    QJsonArray swept;
    if (set->solutions.size() > 1)
        for (std::size_t i = 0; i < set->solutions.size() && i < 50; ++i) {
            Solution& s = *set->solutions[i];
            atItsLast(s);
            QJsonObject g;
            for (const Variable& v : s.globals()) g.insert(v.name, s.globalText(v.name));
            swept.append(QJsonObject{{QStringLiteral("point"), int(i) + 1}, {QStringLiteral("parameters"), s.label}, {QStringLiteral("globals"), g}});
        }
    QJsonArray derived;
    for (const FNode& d : doc->model().results().child(QStringLiteral("derived"))->children) {
        if (!d.enabled) continue;
        const DerivedResult r = doc->evaluate(d.tag);
        QJsonObject values;
        // (Of many times or parameter values: the last 40.)
        const int from = std::max(0, int(r.values.size()) - 40);
        for (int i = from; i < int(r.values.size()); ++i) values.insert(r.values[i].name, r.values[i].text);
        QJsonObject o{{QStringLiteral("tag"), d.tag}, {QStringLiteral("name"), d.name()}, {QStringLiteral("values"), values}};
        if (from > 0) o.insert(QStringLiteral("note"), tr("%1 values: the last 40 here; fem_evaluate with 'all' gives each").arg(r.values.size()));
        else if (!r.note.isEmpty()) o.insert(QStringLiteral("note"), r.note);
        if (!r.error.isEmpty()) o.insert(QStringLiteral("error"), r.error);
        derived.append(o);
    }
    int dofs = 0;
    for (const Field& f : sol->fields())
        if (f.solved) dofs += f.unknowns >= 0 ? f.unknowns : f.space->size() * f.count();
    QJsonObject extra;
    if (!swept.isEmpty()) {
        extra.insert(QStringLiteral("sweep"), QJsonObject{{QStringLiteral("parameters"), QJsonArray::fromStringList(set->swept)},
                                                          {QStringLiteral("solutions"), int(set->solutions.size())},
                                                          {QStringLiteral("each"), swept}});
    }
    if (sol->modeAnalysis) {
        // Each mode's effective index and loss; each quantum state's energies.
        QJsonArray modes;
        const bool states = sol->quantumStates;
        const int was = sol->selected();
        // (A resonator's modes to seven figures: a VCSEL's split by 0.01 nm.)
        bool resonator = false;
        for (const Variable& v : sol->globals()) resonator = resonator || v.name.endsWith(QLatin1String(".Q"));
        const int digits = resonator ? 7 : 4;
        for (int k = 0; k < sol->snapshotCount() && k < 60; ++k) {
            sol->select(k);
            QJsonObject m{{states ? QStringLiteral("state") : QStringLiteral("mode"), sol->snapshotMode(k)}};
            if (!states) m.insert(QStringLiteral("frequency"), formatQuantity(sol->snapshotFrequency(k), Dim::of(0, 0, -1), digits));
            for (const Variable& v : sol->globals()) {
                const bool wanted = states ? v.name.endsWith(QLatin1String(".E_e")) || v.name.endsWith(QLatin1String(".E_hh"))
                                                 || v.name.endsWith(QLatin1String(".E_lh"))
                                           : v.name.endsWith(QLatin1String(".neff")) || v.name.endsWith(QLatin1String(".loss"))
                                                 || v.name.endsWith(QLatin1String(".lambda")) || v.name.endsWith(QLatin1String(".Q"))
                                                 || (v.name.endsWith(QLatin1String(".lambda0")) && sol->global(v.name.chopped(7) + QStringLiteral("Q")));
                const std::optional<double> g = sol->global(v.name);
                if (wanted && g && std::isfinite(*g))
                    m.insert(v.name, states ? formatQuantity(*g / 1.602176634e-19, Dim()) + QStringLiteral(" eV") : sol->globalText(v.name, digits));
            }
            modes.append(m);
        }
        sol->select(was);
        extra.insert(states ? QStringLiteral("states") : QStringLiteral("modes"), modes);
        // (Seven figures are the solve's, not the mesh's: a VCSEL's modes
        // moved 0.06 nm on finer meshes, its LP11 group's 7 pm splitting
        // 27-33 pm, reordered - the oxide's corner.)
        if (resonator)
            extra.insert(QStringLiteral("figures"), tr("seven: the solve's on this mesh - its error is the mesh's, larger: before reading "
                                                       "the splittings of close modes, solve again with a finer mesh"));
    }
    if (sol->snapshotCount() > 0)
        extra.insert(sol->photonAxis == 3 ? QStringLiteral("bands") : sol->photonAxis > 0 ? QStringLiteral("spectrum") : sol->frequencyDomain ? QStringLiteral("frequencies") : QStringLiteral("times"),
                     QJsonObject{{QStringLiteral("outputs"), sol->snapshotCount()},
                                 {QStringLiteral("from"), sol->photonAxis > 0 ? sol->snapshotText(0) : formatQuantity(sol->snapshotValue(0), sol->snapshotDim())},
                                 {QStringLiteral("to"), sol->photonAxis > 0 ? sol->snapshotText(sol->snapshotCount() - 1)
                                                                            : formatQuantity(sol->snapshotValue(sol->snapshotCount() - 1), sol->snapshotDim())},
                                 {QStringLiteral("globals at"), sol->eigenfrequencies && sol->selected() >= 0
                                                                    ? tr("mode %1, the one kept for the steps after").arg(sol->snapshotMode(sol->selected()))
                                                                    : tr("the last")}});
    // (A sweep's seconds: all its points', not its last one's - to the
    // millisecond: 0.4460000000000003 was a sum's noise, the update check of
    // 8bd5709; its log the last point's, the sweep's said after it.)
    // (Points solved at once: the sweep's own time, theirs overlapping.)
    double seconds = 0;
    for (const auto& one : set->solutions) seconds += one->seconds;
    if (sol->atOnce > 1) seconds = sol->sweepSeconds;
    seconds = std::round(seconds * 1000) / 1000;
    QStringList log = sol->log;
    if (set->solutions.size() > 1 && sol->atOnce > 1)
        log << tr("The sweep: %1 points, %2 at once, in %3 s (the lines above the last point's)").arg(set->solutions.size()).arg(sol->atOnce).arg(seconds);
    else if (set->solutions.size() > 1)
        log << tr("The sweep: %1 points in %2 s (the lines above the last point's)").arg(set->solutions.size()).arg(seconds);
    QJsonObject result{{QStringLiteral("study"), sol->studyTag},
                                  {QStringLiteral("seconds"), seconds},
                                  {QStringLiteral("unknowns"), dofs},
                                  {QStringLiteral("triangles"), int(sol->mesh().triangles.size())},
                                  {QStringLiteral("log"), QJsonArray::fromStringList(log)},
                                  {QStringLiteral("globals"), globals},
                                  {QStringLiteral("derived values"), derived},
                                  {QStringLiteral("shown"), doc->shownPlotGroup().isEmpty() ? QString() : doc->model().find(doc->shownPlotGroup())->name()}};
    for (auto it = extra.begin(); it != extra.end(); ++it) result.insert(it.key(), it.value());
    // (A Newton's method that did not converge, a coupling that did not
    // agree: said here, not only in the log.)
    if (!doc->lastWarnings().isEmpty()) result.insert(QStringLiteral("warnings"), QJsonArray::fromStringList(doc->lastWarnings()));
    return jsonResult(result);
}

QJsonObject QucsControl::femEvaluate(const QJsonObject& args)
{
    QString error;
    MultiphysicsDoc* doc = femDocument(args, &error);
    if (doc == nullptr) return errorResult(error);
    const QString study = args.value(QLatin1String("study")).toString();
    std::shared_ptr<SolutionSet> set = doc->solutions(study);
    QString usedStudy;
    if ((!set || set->solutions.empty()) && study.trimmed().isEmpty())
        // (No study given and the first not solved: the first that is.)
        for (const Node* st : doc->model().studies())
            if (const std::shared_ptr<SolutionSet> other = doc->solutions(st->tag); other && !other->solutions.empty()) {
                set = other;
                usedStudy = st->tag;
                break;
            }
    if (!set || set->solutions.empty()) return errorResult(tr("The study is not solved yet: fem_solve solves it."));
    const QString expression = args.value(QLatin1String("expression")).toString();
    if (expression.trimmed().isEmpty()) return errorResult(tr("What? ('expression': V, es.normE, T, es.C11...)"));
    // Several, split by ';': each at each point, or each a global value.
    QStringList items;
    for (const QString& part : expression.split(QLatin1Char(';')))
        if (!part.trimmed().isEmpty()) items << part.trimmed();
    const QString unit = args.value(QLatin1String("unit")).toString();
    const QString over = args.value(QLatin1String("over")).toString();
    if (!over.isEmpty()
        && !QStringList{QStringLiteral("integral"), QStringLiteral("average"), QStringLiteral("maximum"), QStringLiteral("minimum")}.contains(
            args.value(QLatin1String("kind")).toString(QStringLiteral("integral"))))
        return errorResult(tr("'kind' is integral, average, maximum or minimum."));
    // Which solutions: a time, a sweep's point, or all of them; a mode (a
    // state), all of them taking it alone.
    const bool all = args.value(QLatin1String("all")).toBool();
    const int mode = args.contains(QLatin1String("state")) ? args.value(QLatin1String("state")).toInt(0) : args.value(QLatin1String("mode")).toInt(0);
    QString time = args.value(QLatin1String("time")).toVariant().toString();
    const QString frequency = args.value(QLatin1String("frequency")).toVariant().toString();
    struct Found {
        QJsonArray values;
        QStringList errors;
        QString note, solutions;
        bool any = false;
    };
    struct Global {
        int point;
        QString name;
        int index;   // in the values
    };
    auto evaluateIn = [&](const SolutionSet& in) {
        Found out;
        std::vector<Global> global;
        const std::vector<Instance> which = instances(in, all, args.value(QLatin1String("point")).toInt(0), time, frequency, mode);
        if (which.empty()) {
            out.errors << tr("No such solution.");
            return out;
        }
        out.any = true;
        if (which.size() > 1 || !which.front().label.isEmpty())
            out.solutions = which.size() > 1 ? tr("%1: each value's name says which").arg(which.size()) : which.front().label;
        for (const Instance& at : which) {
            Solution* sol = qucs_s::fem::select(in, at);
            if (!sol) continue;
            const QString prefix = which.size() > 1 && !at.label.isEmpty() ? at.label + QStringLiteral(": ") : QString();
            if (args.contains(QLatin1String("at"))) {
                const Locator locator(sol->mesh());
                for (const QJsonValue& v : args.value(QLatin1String("at")).toArray()) {
                    const QJsonArray p = v.toArray();
                    const QPointF point(p.at(0).toDouble(), p.at(1).toDouble());
                    for (const QString& text : items) {
                        // (name = expression: named so.)
                        const NamedExpression item = namedExpression(text);
                        QString why;
                        Dim dim;
                        const std::optional<double> value = evaluateAt(*sol, locator, item.expression, point, &why, &dim);
                        QJsonObject vo{{QStringLiteral("at"), p}};
                        if (items.size() > 1 || item.name != item.expression) vo.insert(QStringLiteral("name"), item.name);
                        if (!prefix.isEmpty()) vo.insert(QStringLiteral("solution"), at.label);
                        if (value) {
                            const DisplayUnit du = displayUnitOf(*sol, item.expression, unit, dim);
                            vo.insert(QStringLiteral("value"), du.name.isEmpty() ? formatQuantity(*value, dim)
                                                                                : formatNumber((*value - du.offset) / du.scale, 6) + QLatin1Char(' ') + du.name);
                            vo.insert(QStringLiteral("SI"), *value);
                        } else {
                            vo.insert(QStringLiteral("error"), why);
                            out.errors << why;
                        }
                        out.values.append(vo);
                    }
                }
                continue;
            }
            // Over domains or boundaries, or a global value.
            FNode node;
            if (over.isEmpty()) {
                node.type = QStringLiteral("global");
            } else {
                node.type = args.value(QLatin1String("kind")).toString(QStringLiteral("integral"));
                node.props.insert(QStringLiteral("level"), over);
                node.props.insert(QStringLiteral("selection"), args.value(QLatin1String("selection")).toObject(QJsonObject{{QStringLiteral("all"), true}}));
            }
            node.props.insert(QStringLiteral("expression"), expression);
            node.props.insert(QStringLiteral("unit"), unit);
            const DerivedResult r = evaluateDerived(*sol, node);
            for (const DerivedValue& v : r.values) {
                if (over.isEmpty() && at.snapshot >= 0) global.push_back({at.point, v.name, int(out.values.size())});
                out.values.append(QJsonObject{{QStringLiteral("name"), prefix + v.name}, {QStringLiteral("value"), v.text}, {QStringLiteral("SI"), v.value}});
            }
            if (!r.error.isEmpty()) out.errors << prefix + r.error;
            if (out.note.isEmpty()) out.note = r.note;
        }
        // A global the same at each mode (state) of a point - a states
        // study's qw.Ee1, a mode step's wavelength - once per point.
        if (all && which.size() > 1 && mode <= 0) {
            std::map<std::pair<int, QString>, std::vector<int>> byPoint;
            for (const Global& g : global) byPoint[{g.point, g.name}].push_back(g.index);
            std::vector<char> dropped(std::size_t(out.values.size()), 0);
            bool any = false;
            for (const auto& [key, indices] : byPoint) {
                if (indices.size() < 2) continue;
                const double first = out.values[indices.front()].toObject().value(QStringLiteral("SI")).toDouble();
                bool same = true;
                for (int k : indices) same = same && out.values[k].toObject().value(QStringLiteral("SI")).toDouble() == first;
                if (!same) continue;
                QJsonObject one = out.values[indices.front()].toObject();
                const QString label = in.solutions[std::size_t(key.first)]->label;
                one.insert(QStringLiteral("name"), label.isEmpty() ? key.second : label + QStringLiteral(": ") + key.second);
                out.values[indices.front()] = one;
                for (std::size_t k = 1; k < indices.size(); ++k) dropped[std::size_t(indices[k])] = 1;
                any = true;
            }
            if (any) {
                QJsonArray kept;
                for (int k = 0; k < out.values.size(); ++k)
                    if (!dropped[std::size_t(k)]) kept.append(out.values[k]);
                out.values = kept;
                out.note = tr("a value the same at each %1 of a point is given once for the point; 'mode' (or 'state') takes one alone")
                               .arg(in.solutions.front()->quantumStates ? tr("state") : tr("mode"));
            }
        }
        return out;
    };
    if (args.contains(QLatin1String("mode")) || args.contains(QLatin1String("state")))
        if (const QString bad = modeAsked(*set, mode); !bad.isEmpty()) return errorResult(bad);
    Found found = evaluateIn(*set);
    // No study given and the first has not what it reads: the first
    // computed study that has.
    auto unknown = [](const Found& f) {
        if (f.errors.isEmpty()) return false;
        for (const QString& e : f.errors)
            if (!e.contains(QLatin1String("is not a parameter, a variable or a function"))) return false;
        for (const QJsonValue& v : f.values)
            if (v.toObject().contains(QLatin1String("SI"))) return false;
        return true;
    };
    if (study.trimmed().isEmpty() && unknown(found)) {
        const QString first = usedStudy.isEmpty() ? doc->studyTag(QString()) : usedStudy;
        bool replaced = false;
        for (const Node* st : doc->model().studies()) {
            if (st->tag == first) continue;
            const std::shared_ptr<SolutionSet> other = doc->solutions(st->tag);
            if (!other || other->solutions.empty()) continue;
            Found f = evaluateIn(*other);
            if (f.any && !unknown(f) && f.errors.isEmpty()) {
                found = f;
                usedStudy = st->tag;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            const Node* st = doc->model().find(first);
            for (QString& e : found.errors) e += tr(" (in %1, the first study: give 'study')").arg(st ? st->name() : first);
        }
    }
    QJsonObject o{{QStringLiteral("expression"), expression}};
    if (doc->solutionStale(usedStudy.isEmpty() ? study : usedStudy)) o.insert(QStringLiteral("note"), tr("the model changed since it was solved"));
    if (!usedStudy.isEmpty()) o.insert(QStringLiteral("study"), usedStudy);
    if (!found.note.isEmpty()) o.insert(QStringLiteral("note"), found.note);
    // (A constant's value as a bare number: its result's unit wrong -
    // 1.602e-19*semi.R integrated in µHz, not µA: the update check of 48d5fa8.)
    if (const QStringList bare = bareConstants(expression); !bare.isEmpty())
        o.insert(QStringLiteral("hint"), bare.join(QLatin1String("; ")));
    if (!found.solutions.isEmpty()) o.insert(QStringLiteral("solutions"), found.solutions);
    o.insert(QStringLiteral("values"), found.values);
    found.errors.removeDuplicates();
    if (!found.errors.isEmpty()) o.insert(QStringLiteral("error"), found.errors.join(QLatin1String("; ")));
    if (found.values.isEmpty()) return errorResult(found.errors.join(QLatin1String("; ")));
    return jsonResult(o);
}

QJsonObject QucsControl::femPlot(const QJsonObject& args)
{
    QString error;
    MultiphysicsDoc* doc = femDocument(args, &error);
    if (doc == nullptr) return errorResult(error);
    a_app->DocumentTab->setCurrentWidget(doc);
    const QString show = args.value(QLatin1String("show")).toString();
    if (show == QLatin1String("geometry")) {
        doc->showGeometry();
        return jsonResult(QJsonObject{{QStringLiteral("shown"), tr("the geometry (screenshot takes a picture)")}});
    }
    if (show == QLatin1String("mesh")) {
        doc->showMesh();
        return jsonResult(QJsonObject{{QStringLiteral("shown"), tr("the mesh (screenshot takes a picture)")}});
    }
    QString group = args.value(QLatin1String("plot")).toString();
    if (group.isEmpty()) {
        // A plot group of the expression: added (one step of Undo).
        const QString expression = args.value(QLatin1String("expression")).toString();
        if (expression.isEmpty()) {
            QStringList groups;
            for (const FNode& g : doc->model().results().children)
                if (g.type == QLatin1String("plotgroup") || g.type == QLatin1String("plotgroup1d")) groups << QStringLiteral("%1 (%2)").arg(g.name(), g.tag);
            return errorResult(tr("Which plot? 'plot' (a plot group: %1), 'expression' (one made), or 'show' geometry or mesh.")
                                   .arg(groups.isEmpty() ? tr("none yet") : groups.join(QStringLiteral(", "))));
        }
        Model m = doc->model();
        FNode g = m.make(QStringLiteral("plotgroup"));
        g.label = args.value(QLatin1String("title")).toString(expression);
        if (args.contains(QLatin1String("study"))) g.props.insert(QStringLiteral("study"), args.value(QLatin1String("study")).toString());
        FNode s = m.make(QStringLiteral("surface"));
        s.tag += QStringLiteral("_") + g.tag;
        s.props.insert(QStringLiteral("expression"), expression);
        if (args.contains(QLatin1String("unit"))) s.props.insert(QStringLiteral("unit"), args.value(QLatin1String("unit")).toString());
        if (args.contains(QLatin1String("colors"))) s.props.insert(QStringLiteral("colors"), args.value(QLatin1String("colors")).toString());
        g.children.push_back(s);
        if (args.value(QLatin1String("contours")).toBool()) {
            FNode c = m.make(QStringLiteral("contour"));
            c.tag += QStringLiteral("_") + g.tag;
            c.props.insert(QStringLiteral("expression"), expression);
            if (args.contains(QLatin1String("unit"))) c.props.insert(QStringLiteral("unit"), args.value(QLatin1String("unit")).toString());
            g.children.push_back(c);
        }
        FNode* added = m.add(m.results().tag, g);
        if (!added) return errorResult(tr("The plot group could not be added."));
        group = added->tag;
        doc->setModel(m, tr("Add %1").arg(g.label));
    }
    // At a time, a sweep's point: the plot group's settings (one step of Undo).
    if (args.contains(QLatin1String("time")) || args.contains(QLatin1String("point")) || args.contains(QLatin1String("frequency"))
        || args.contains(QLatin1String("mode"))) {
        const QString time = args.value(QLatin1String("time")).toVariant().toString();
        const QString frequency = args.value(QLatin1String("frequency")).toVariant().toString();
        const int point = args.value(QLatin1String("point")).toInt(0);
        const bool hasTime = args.contains(QLatin1String("time")), hasPoint = args.contains(QLatin1String("point"));
        const bool hasFrequency = args.contains(QLatin1String("frequency")), hasMode = args.contains(QLatin1String("mode"));
        const int mode = args.value(QLatin1String("mode")).toInt(1);
        if (hasMode) {
            const FNode* g = doc->model().find(group);
            const QString study = g ? g->text(QStringLiteral("study")) : QString();
            if (const std::shared_ptr<SolutionSet> set = doc->solutions(study))
                if (const QString bad = modeAsked(*set, mode); !bad.isEmpty()) return errorResult(bad);
        }
        QString why;
        if (!doc->editNode(group, [&](FNode& n) {
                if (hasTime) n.props.insert(QStringLiteral("time"), time);
                if (hasFrequency) n.props.insert(QStringLiteral("frequency"), frequency);
                if (hasMode) n.props.insert(QStringLiteral("mode"), mode);
                if (hasPoint) n.props.insert(QStringLiteral("point"), point);
            }, tr("Show a Solution"), &why))
            return errorResult(why);
    }
    QString why;
    if (const FNode* g = doc->model().find(group); g && g->type == QLatin1String("plotgroup1d")) {
        // Graphs: a dataset and its data display.
        const QString display = doc->showGraphs(group, &why);
        if (display.isEmpty()) return errorResult(why);
        return jsonResult(QJsonObject{{QStringLiteral("plot group"), group},
                                      {QStringLiteral("display"), QDir::toNativeSeparators(display)},
                                      {QStringLiteral("dataset"), QDir::toNativeSeparators(display.left(display.size() - 4) + QStringLiteral(".dat"))},
                                      {QStringLiteral("see it"), tr("the data display is in front: screenshot takes a picture; get_dataset reads it")}});
    }
    if (!doc->showPlotGroup(group, &why)) return errorResult(why);
    const std::shared_ptr<const PlotScene> scene = doc->plotScene();
    QJsonObject o{{QStringLiteral("shown"), scene->title}, {QStringLiteral("plot group"), group}};
    if (!doc->shownInstance().isEmpty()) o.insert(QStringLiteral("solution"), doc->shownInstance());
    if (scene->surface) {
        const QString unit = scene->values.unit.name.isEmpty() ? dimName(scene->values.dim) : scene->values.unit.name;
        o.insert(QStringLiteral("range"), QStringLiteral("%1 to %2 %3").arg(formatNumber(scene->values.min, 5), formatNumber(scene->values.max, 5), unit));
    }
    if (!scene->problems.isEmpty()) o.insert(QStringLiteral("problems"), QJsonArray::fromStringList(scene->problems));
    o.insert(QStringLiteral("see it"), tr("screenshot takes a picture of the window"));
    return jsonResult(o);
}

QJsonObject QucsControl::femExport(const QJsonObject& args)
{
    QString error;
    MultiphysicsDoc* doc = femDocument(args, &error);
    if (doc == nullptr) return errorResult(error);
    const QString tag = args.value(QLatin1String("export")).toString().trimmed();
    const FNode* exports = doc->model().results().child(QStringLiteral("exports"));
    if (tag.isEmpty()) {
        QStringList list;
        if (exports)
            for (const FNode& e : exports->children) list << QStringLiteral("%1 (%2)").arg(e.name(), e.tag);
        return errorResult(tr("Which export? 'export': %1 - fem_edit adds one under \"exports\" (touchstone, spice, qucspart, fieldexport, lumericalmode, lumericalcharge, lumericalmqw).")
                               .arg(list.isEmpty() ? tr("none yet") : list.join(QStringLiteral(", "))));
    }
    if (args.value(QLatin1String("copy")).toBool()) {
        QString why;
        if (!doc->copyPart(tag, &why)) return errorResult(why);
        return jsonResult(QJsonObject{{QStringLiteral("copied"), tr("the RLCG part is on the clipboard: paste it into a schematic")}});
    }
    QString why;
    const QString path = doc->runExport(tag, &why);
    if (path.isEmpty()) return errorResult(why);
    QFile f(path);
    QString head;
    if (f.open(QIODevice::ReadOnly)) {
        const QByteArray bytes = f.read(1500);
        // (NumPy's: its header, the array's shape - not its numbers' bytes.)
        if (bytes.startsWith("\x93NUMPY") && bytes.size() > 10) {
            const int length = quint8(bytes[8]) | (quint8(bytes[9]) << 8);
            head = QString::fromLatin1(bytes.mid(10, length)).trimmed();
        } else {
            head = QString::fromUtf8(bytes);
        }
    }
    return jsonResult(QJsonObject{{QStringLiteral("written"), QDir::toNativeSeparators(path)}, {QStringLiteral("begins"), head}});
}
