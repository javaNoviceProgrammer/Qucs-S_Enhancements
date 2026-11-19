/*
 * fem3_magnetic.cpp - magnetic fields in 3D: magnetostatics, the vector
 *                     potential A on a swept mesh's edge elements (Nédélec's
 *                     of order 1 or 2), ∇ × (H(B)) = J with B = ∇ × A, its
 *                     gauge ∇·A = 0 held weakly by a nodal Lagrange
 *                     multiplier of the same order; µr materials (a tensor)
 *                     and B-H curves by Newton's method, currents given or
 *                     read from a currents physics (coils), permanent
 *                     magnets, magnetic insulation, a potential held, a
 *                     surface current; the energy, the coils' flux linkage
 *                     and inductance matrix, the Lorentz force on each coil;
 *                     in the frequency domain eddy currents jωσA with an
 *                     electric potential on the conductors' nodes, single
 *                     conductors fed at a currents physics' terminal (a
 *                     current or a voltage held), the coils' impedance
 *                     matrix, the losses (resistive, µr'') as time averages
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem3_problem.h"

#include "fem_edge.h"
#include "fem_problem.h"
#include "fem_tensor.h"

#include <Eigen/Dense>

#include <QElapsedTimer>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace qucs_s::fem::detail3 {

namespace {

constexpr double Mu0 = 1.25663706127e-6;
const double NaN = std::numeric_limits<double>::quiet_NaN();

/// A face's own condition: the last feature's.
struct FaceCondition {
    enum Kind { Free, Insulation, Potential };
    Kind kind = Free;
    int order = -1;
    Expression a0[3];   // a potential's
};

/// A coil: its current density the currents physics' J between its
/// terminal and the rest, scaled to N I0 through it. In the frequency
/// domain a single conductor's is not given but found: its eddy currents
/// in it, its terminal's potential U (a current held, or no net current) or
/// its current (a voltage held, or none) - turns of wire still carry the
/// currents physics' J, evenly, no eddy currents in them.
struct Coil {
    enum Role { Current, Voltage, Floating, Grounded };
    QString name;
    std::vector<char> on;        // by domain
    Expression current, turns;   // I0, N
    bool single = true;          // a single conductor (not turns)
    Role role = Current;         // a single conductor's in the frequency domain
    Expression voltage;          // V0 (role Voltage)
    QString ec;                  // the currents physics' tag
    QString terminal;            // its terminal whose current J carries (empty: its first)
    double perAmp = 0;           // N / I_ec, as last solved: its J at 1 A
    double scale = 0;            // N I0 / I_ec
    double flux = 0;             // its flux linkage as solved (Wb)
    double force[3] = {0, 0, 0}; // ∫ J × B over it (N)
};

/// The reference prism's nodal shape functions' gradients by a straight
/// triangle's map (as the edge elements take theirs: the gauge's gradients
/// then the edge space's own).
void nodalGradients(const PrismShape& s, const AffineMap& am, double h, double g[18][3])
{
    for (int k = 0; k < s.count; ++k) {
        g[k][0] = s.dxi[k] * am.grad[1][0] + s.deta[k] * am.grad[2][0];
        g[k][1] = s.dxi[k] * am.grad[1][1] + s.deta[k] * am.grad[2][1];
        g[k][2] = s.dzeta[k] / h;
    }
}

/// ν (A m / (V s)): the inverse of µ0 µr, a 3 x 3 tensor (row by row).
bool reluctivity(const double mur[9], double nu[9])
{
    Eigen::Matrix3d m;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m(i, j) = Mu0 * mur[3 * i + j];
    Eigen::FullPivLU<Eigen::Matrix3d> lu(m);
    if (!lu.isInvertible()) return false;
    const Eigen::Matrix3d inv = lu.inverse();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) nu[3 * i + j] = inv(i, j);
    return true;
}

class MagneticProblem3 : public Problem3
{
public:
    using Problem3::Problem3;
    bool setup(QString* error) override;
    bool solve(const Options3& o, QString* error) override;
    void setGlobals() override;
    void releaseFactors() override
    {
        a_lu.reset();
        a_factored = Eigen::SparseMatrix<double>();
    }

private:
    using cd = std::complex<double>;
    struct Equations {
        std::vector<int> eqA, eqP;     // by edge dof / node dof: its equation, -1 none, -3 fixed
        std::vector<double> fixedA;    // by edge dof: its held value (a potential's)
        int nA = 0, nP = 0;
    };
    bool buildEquations(Equations& q, QString* error);
    /// The frequency domain's conductors: the electric potential V on the
    /// nodes of the elements that conduct, its equations after A's and the
    /// gauge's - each single conductor's terminal's nodes one equation, its
    /// potential U (the coil's voltage), or held at its voltage; a ground's
    /// (any other face the currents physics holds) nought; one node of a
    /// conductor that touches neither held, its constant -, scaled by
    /// 1/(jω) as ψ = V/(jω) (the system complex symmetric).
    struct Conductors {
        std::vector<char> conducting;   // by domain
        std::vector<int> eqV;           // by node dof: its equation (offset by nA + nP), -1 none, -3 held
        std::vector<cd> heldPsi;        // by node dof: its held ψ
        std::vector<char> electrode;    // by node dof: a terminal's or a ground's
        std::vector<char> electrodeFaces;   // by mesh face: a terminal's or a ground's
        std::vector<int> terminalOf;    // by node dof: the coil whose terminal it is on, -1
        std::vector<int> eqU;           // by coil: its potential's equation (a current held), -1 none (turns, or its voltage held)
        int nV = 0;
    };
    bool buildConductors(Conductors& h, double omega, QString* error);
    /// The faces currents physics \a tag holds, by topology face - its
    /// terminal's index in \a names (a Terminal's name, a Floating's own), -2
    /// a ground's (Ground, Electric Potential), -1 none: the last condition
    /// on a face its own, as that physics takes them.
    std::vector<int> heldFacesOf(const QString& tag, QStringList* names) const;
    /// What coil \a c's currents physics' J carries in at its terminal in
    /// this field's own elements, -∫ J·∇h over its domains (h one on the
    /// terminal's nodes): their triangles straight, an arc's chords, where
    /// that physics' are curved - its section smaller. NaN where the
    /// terminal is on none of its domains.
    double carriedCurrent(const Coil& c) const;
    void publishConductivity();
    bool solveHarmonic(const Options3& o, QString* error);
    void setHarmonicGlobals();
    /// The harmonic system at ω (\a omega): (K + jωσM) A + jω C ψ + Bᵀ λ =
    /// J, jω (Cᵀ A + L ψ) = the currents held (each terminal's equation's),
    /// B A = 0; \a r its right-hand side less what the held values give
    /// (\a held: by A's edge dof, its held value; ψ's by Conductors), \a M
    /// its matrix where \a matrix.
    void assembleHarmonic(const Equations& q, const Conductors& h, double omega, const std::vector<double>& held, const std::vector<cd>& heldPsi,
                          const std::vector<std::array<Expression, 3>>& J, bool matrix, bool sheets, Eigen::SparseMatrix<cd>& M,
                          Eigen::VectorXcd& r, double gaugeScale) const;
    double gaugeScaleOf() const;
    /// A warning in the log where a conductor's surface's mesh is coarser
    /// than its skin depth at ω.
    void skinDepthCheck(const Conductors& h, double omega);
    bool conducts(int d) const
    {
        if (d < 0 || d >= int(a_sigma.size())) return false;
        for (const Expression& e : a_sigma[std::size_t(d)])
            if (e.isValid()) return true;
        return false;
    }
    /// The source J by domain: the External Current Densities', and each
    /// coil's N I0 / I_ec times the currents physics' J (\a only: that coil
    /// alone at 1 A, nothing else).
    bool buildSources(QString* error);
    std::vector<std::array<Expression, 3>> sourcesOf(int only) const;
    /// Newton's system at A (\a a, by edge dof): its matrix (the tangent,
    /// and the gauge's rows and columns scaled), and the residual.
    void assemble(const Equations& q, const std::vector<double>& a, const std::vector<std::array<Expression, 3>>& J, bool magnets,
                  Eigen::SparseMatrix<double>& M, Eigen::VectorXd& r, double gaugeScale) const;
    double evalAt(const Expression& e, const Vec3& x) const;

    std::vector<FaceCondition> a_faces;                                // by face
    std::vector<std::vector<std::array<Expression, 3>>> a_sheets;      // by face
    std::vector<std::vector<std::array<Expression, 3>>> a_external;    // by domain
    std::vector<Coil> a_coils;
    bool a_nonlinear = false;
    bool a_magnets = false;
    double a_energy = 0;
    std::vector<std::vector<double>> a_inductance;   // coils swept at 1 A each: [i][j]
    std::vector<std::array<Expression, 9>> a_sigma;   // by domain: σ, the frequency domain's (invalid: none)
    // The frequency domain's, as last solved: ω (0 at rest), by coil its
    // potential U, its current and its flux linkage (turns'), the coils fed
    // a current (its ports) and their impedance matrix, the losses (resistive
    // and magnetic) and the energy (time averages).
    double a_omega = 0;
    std::vector<cd> a_U, a_I, a_flux;
    std::vector<int> a_ports;
    std::vector<std::vector<cd>> a_Z;
    double a_losses = 0, a_magneticLosses = 0;
    // The last factorization, and the matrix it is of: a matrix the same at
    // the next solve - a cycle's next pass, a step in time's next step at
    // rest, of a linear model - solved with it, not factorized again (a coil's 1.7 M unknowns factorized twice, 2739
    // s and 717 s, for one solve).
    std::unique_ptr<SparseRealLdlt> a_lu;
    Eigen::SparseMatrix<double> a_factored;
};

/// The same matrix: its pattern and its values, bit for bit (both
/// compressed, as setFromTriplets leaves them).
bool sameMatrix(const Eigen::SparseMatrix<double>& a, const Eigen::SparseMatrix<double>& b)
{
    if (a.rows() != b.rows() || a.cols() != b.cols() || a.nonZeros() != b.nonZeros() || !a.isCompressed() || !b.isCompressed()) return false;
    const auto nz = std::size_t(a.nonZeros());
    return std::equal(a.outerIndexPtr(), a.outerIndexPtr() + a.cols() + 1, b.outerIndexPtr())
           && std::equal(a.innerIndexPtr(), a.innerIndexPtr() + nz, b.innerIndexPtr()) && std::equal(a.valuePtr(), a.valuePtr() + nz, b.valuePtr());
}

// ------------------------------------------------------------------ Setup

bool MagneticProblem3::setup(QString* error)
{
    if (!a_node) {
        *error = tr("there is no physics %1").arg(field().tag);
        return false;
    }
    Field3& f = field();
    const Model& model = a_s.model();
    const Topology3& topo = a_s.topology();
    const std::size_t D = topo.domains.size(), F = topo.faces.size();
    QString why;
    const QVector<int> domains = resolveSelection3(topo, model, Level::Domain, a_node->selection(), &why);
    if (!why.isEmpty()) problem(tr("%1: %2").arg(a_node->name(), why));
    f.domains.assign(D, 0);
    for (int d : domains)
        if (d >= 0 && d < int(D)) f.domains[std::size_t(d)] = 1;
    if (domains.isEmpty()) problem(tr("%1 is on no domain").arg(a_node->name()));
    const std::vector<const Node*> material = materials();
    Solution3::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    coef.c.assign(D, {});
    coef.isotropic.assign(D, 1);
    coef.sources.assign(D, {});
    coef.sourceScale.assign(D, 1.0);
    coef.capacity.assign(D, Expression());
    coef.named.clear();
    for (const char* k : {"Brx", "Bry", "Brz", "Jx", "Jy", "Jz", "murIm"}) coef.named[QString::fromLatin1(k)].assign(D, Expression());
    for (int k = 0; k < 9; ++k) coef.named[QStringLiteral("sigma%1").arg(k)].assign(D, Expression());
    coef.curves.assign(D, nullptr);
    a_faces.assign(F, FaceCondition());
    a_sheets.assign(F, {});
    a_external.assign(D, {});
    a_coils.clear();
    a_nonlinear = false;
    a_magnets = false;
    a_sigma.assign(D, {});
    const Scope& params = a_s.parameters().scope;
    std::vector<const Node*> ampereOf(D, nullptr);
    std::vector<const Node*> magnetOf(D, nullptr);
    int order = 0;
    const auto vector3 = [this](const Node& feature, const char* const keys[3], const QString& zero, std::array<Expression, 3>* out) {
        bool ok = true;
        for (int c = 0; c < 3; ++c) {
            const QString key = QString::fromLatin1(keys[c]);
            const QString text = feature.text(key);
            (*out)[std::size_t(c)] = compile(text.trimmed().isEmpty() ? zero : text, feature, key);
            ok = ok && (*out)[std::size_t(c)].isValid();
        }
        return ok;
    };
    for (const Node& feature : a_node->children) {
        ++order;
        if (!feature.enabled) continue;
        const NodeKind* k = feature.kind();
        if (!k) continue;
        const QVector<int> picked = resolveSelection3(topo, model, k->selection, feature.selection(), &why);
        if (!why.isEmpty()) problem(tr("%1: %2").arg(feature.name(), why));
        const QString t = feature.type;
        if (t == QLatin1String("magnetic/ampere")) {
            for (int d : picked)
                if (d >= 0 && d < int(D)) ampereOf[std::size_t(d)] = &feature;
        } else if (t == QLatin1String("magnetic/magnet")) {
            for (int d : picked)
                if (d >= 0 && d < int(D)) magnetOf[std::size_t(d)] = &feature;
        } else if (t == QLatin1String("magnetic/current")) {
            static const char* const keys[3] = {"Jex", "Jey", "Jez"};
            std::array<Expression, 3> j;
            if (vector3(feature, keys, QStringLiteral("0[A/m^2]"), &j))
                for (int d : picked)
                    if (d >= 0 && d < int(D)) a_external[std::size_t(d)].push_back(j);
        } else if (t == QLatin1String("magnetic/coil")) {
            Coil c;
            c.name = feature.text(QStringLiteral("name"));
            if (c.name.trimmed().isEmpty()) c.name = feature.name();
            c.on.assign(D, 0);
            for (int d : picked)
                if (d >= 0 && d < int(D)) c.on[std::size_t(d)] = 1;
            c.current = compile(feature.text(QStringLiteral("I0")), feature, QStringLiteral("I0"), false);
            c.single = feature.text(QStringLiteral("conductor")) != QLatin1String("multi");
            const QString role = feature.text(QStringLiteral("role"));
            if (c.single && role == QLatin1String("voltage")) c.role = Coil::Voltage;
            else if (c.single && role == QLatin1String("floating")) c.role = Coil::Floating;
            else if (c.single && role == QLatin1String("grounded")) c.role = Coil::Grounded;
            if (c.role == Coil::Voltage) c.voltage = compile(feature.text(QStringLiteral("V0")), feature, QStringLiteral("V0"), false);
            for (const Coil& other : a_coils)
                if (other.name == c.name) problem(tr("%1: another coil is named %2").arg(feature.name(), c.name));
            const QString n = !c.single ? feature.text(QStringLiteral("N")) : QStringLiteral("1");
            c.turns = compile(n.trimmed().isEmpty() ? QStringLiteral("1") : n, feature, QStringLiteral("N"), false);
            const QStringList tags = feature.list(QStringLiteral("ec"));
            c.ec = tags.isEmpty() ? QStringLiteral("ec") : tags.first();
            c.terminal = feature.text(QStringLiteral("terminal")).trimmed();
            const int fi = a_s.fieldIndex(c.ec);
            if (fi < 0 || a_s.fields()[std::size_t(fi)].type != QLatin1String("currents")) {
                problem(tr("%1: its current density is a currents physics' (%2), which the model does not solve").arg(feature.name(), c.ec));
                continue;
            }
            a_reads.insert(fi);
            a_coils.push_back(c);
        } else if (t == QLatin1String("magnetic/insulation") || t == QLatin1String("magnetic/potential") || t == QLatin1String("magnetic/pmc")) {
            FaceCondition c;
            c.order = order;
            c.kind = t == QLatin1String("magnetic/insulation") ? FaceCondition::Insulation
                     : t == QLatin1String("magnetic/potential") ? FaceCondition::Potential
                                                                : FaceCondition::Free;
            if (c.kind == FaceCondition::Potential) {
                static const char* const keys[3] = {"A0x", "A0y", "A0z"};
                std::array<Expression, 3> a0;
                if (!vector3(feature, keys, QStringLiteral("0[Wb/m]"), &a0)) continue;
                for (int i = 0; i < 3; ++i) c.a0[i] = a0[std::size_t(i)];
            }
            for (int b : picked)
                if (b >= 0 && b < int(F)) a_faces[std::size_t(b)] = c;
        } else if (t == QLatin1String("magnetic/surfacecurrent")) {
            static const char* const keys[3] = {"Jsx", "Jsy", "Jsz"};
            std::array<Expression, 3> js;
            if (vector3(feature, keys, QStringLiteral("0[A/m]"), &js))
                for (int b : picked)
                    if (b >= 0 && b < int(F)) a_sheets[std::size_t(b)].push_back(js);
        }
    }
    // Each domain's permeability: a value, a tensor in the model's axes (a
    // material's turned into them), or a B-H curve.
    for (int d = 0; d < int(D); ++d) {
        if (!f.domains[std::size_t(d)]) continue;
        const Node* feat = ampereOf[std::size_t(d)];
        const QString object = topo.objectNames.value(topo.domains[std::size_t(d)].owner);
        if (!feat) {
            problem(tr("domain %1 (%2) has no Ampère's Law feature").arg(d + 1).arg(object));
            continue;
        }
        const bool user = feat->text(QStringLiteral("source")) == QLatin1String("user");
        const Node* m = material[std::size_t(d)];
        if (!user && !m) {
            problem(tr("domain %1 (%2) has no material: its relative permeability is needed").arg(d + 1).arg(object));
            continue;
        }
        std::array<Expression, 9>& c = coef.c[std::size_t(d)];
        const Expression zero = Expression::compile(QStringLiteral("0"), a_s.pointScope());
        if (feat->text(QStringLiteral("constitutive")) == QLatin1String("bh")) {
            QString cwhy;
            const QJsonValue table = user ? feat->value(QStringLiteral("BH")) : detail::materialValue(*m, QStringLiteral("BH"));
            if (table.isUndefined() || table.isNull() || (table.isArray() && table.toArray().isEmpty())
                || (table.isString() && table.toString().trimmed().isEmpty())) {
                problem(user ? tr("%1: its B-H curve is empty").arg(feat->name())
                             : tr("domain %1 (%2): its material, %3, has no B-H curve").arg(d + 1).arg(object, m->name()));
                continue;
            }
            coef.curves[std::size_t(d)] = bhCurveOf(table, params, &cwhy);
            if (!coef.curves[std::size_t(d)]) {
                problem(tr("domain %1 (%2): %3").arg(d + 1).arg(object, cwhy));
                continue;
            }
            // (µr, as a variable reads it: the curve's B/(µ0 H) at its first
            // slope.)
            const double mu = 1 / (Mu0 * coef.curves[std::size_t(d)]->nu(0));
            for (int i = 0; i < 9; ++i) c[std::size_t(i)] = i % 4 == 0 ? Expression::compile(QString::number(mu, 'g', 17), a_s.pointScope()) : zero;
            a_nonlinear = true;
        } else if (user) {
            const QString aniso = feat->text(QStringLiteral("anisotropy"));
            static const char axes[] = "xyz";
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) {
                    QString key, text;
                    if (aniso == QLatin1String("diagonal") || aniso == QLatin1String("symmetric")) {
                        if (i != j && aniso != QLatin1String("symmetric")) {
                            c[std::size_t(3 * i + j)] = zero;
                            continue;
                        }
                        key = QStringLiteral("mur") + QLatin1Char(axes[std::min(i, j)]) + QLatin1Char(axes[std::max(i, j)]);
                        text = feat->text(key);
                        if (text.trimmed().isEmpty()) text = i == j ? QStringLiteral("1") : QStringLiteral("0");
                    } else {
                        key = QStringLiteral("mur");
                        text = i == j ? feat->text(key) : QStringLiteral("0");
                    }
                    c[std::size_t(3 * i + j)] = text == QLatin1String("0") ? zero : compile(text, *feat, key);
                    if (i != j && text != QLatin1String("0")) coef.isotropic[std::size_t(d)] = 0;
                }
            if (aniso == QLatin1String("diagonal") || aniso == QLatin1String("symmetric")) coef.isotropic[std::size_t(d)] = 0;
        } else {
            const QString text = detail::Problem::materialText(*m, QStringLiteral("mur"), &params);
            if (text.trimmed().isEmpty()) {
                problem(tr("domain %1 (%2): its material, %3, has no relative permeability").arg(d + 1).arg(object, m->name()));
                continue;
            }
            std::optional<TensorText> t = tensorText(text, &why);
            if (!t) {
                problem(tr("%1's relative permeability: %2").arg(m->name(), why));
                continue;
            }
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    c[std::size_t(3 * i + j)] = t->c[i][j] == QLatin1String("0") ? zero : compile(t->c[i][j], *m, QStringLiteral("mur"));
            coef.isotropic[std::size_t(d)] = t->anisotropic() ? 0 : 1;
        }
        // A permanent magnet: Br along its direction (its recoil
        // permeability Ampère's Law's µr).
        if (const Node* mg = magnetOf[std::size_t(d)]) {
            if (coef.curves[std::size_t(d)]) {
                problem(tr("%1: a magnet of a B-H curve (its µr is the recoil one, a value)").arg(mg->name()));
                continue;
            }
            const bool fromUser = mg->text(QStringLiteral("source")) == QLatin1String("user");
            const QString br = fromUser ? mg->text(QStringLiteral("Br")) : m ? detail::Problem::materialText(*m, QStringLiteral("Br"), &params) : QString();
            if (br.trimmed().isEmpty()) {
                problem(tr("%1: its remanent flux density Br is not given").arg(mg->name()));
                continue;
            }
            double dir[3] = {0, 0, 0};
            static const char* const keys[3] = {"dx", "dy", "dz"};
            for (int i = 0; i < 3; ++i) {
                const std::optional<double> v = evaluateConstant(mg->text(QString::fromLatin1(keys[i])).trimmed().isEmpty() ? QStringLiteral("0") : mg->text(QString::fromLatin1(keys[i])),
                                                                 params, Dim(), &why);
                if (!v) {
                    problem(tr("%1: its direction: %2").arg(mg->name(), why));
                    continue;
                }
                dir[i] = *v;
            }
            const double len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            if (!(len > 0)) {
                problem(tr("%1: its direction (dx, dy, dz) is nought").arg(mg->name()));
                continue;
            }
            static const char* const named[3] = {"Brx", "Bry", "Brz"};
            for (int i = 0; i < 3; ++i)
                coef.named[QString::fromLatin1(named[i])][std::size_t(d)] =
                    Expression::compile(QStringLiteral("(%1)*%2").arg(br).arg(dir[i] / len, 0, 'g', 17), a_s.pointScope());
            a_magnets = true;
        }
    }
    // Each domain's conductivity σ (a value, or a tensor in the model's axes)
    // and µr'' (µr = µr' - j µr'', a core's losses), the frequency domain's:
    // Ampère's Law's, or its material's. None in a coil of turns (no eddy
    // currents in them).
    std::vector<char> turns(D, 0);
    for (const Coil& c : a_coils)
        if (!c.single)
            for (std::size_t d = 0; d < D; ++d) turns[d] = turns[d] || c.on[d];
    for (int d = 0; d < int(D); ++d) {
        const Node* feat = ampereOf[std::size_t(d)];
        if (!f.domains[std::size_t(d)] || !feat) continue;
        const bool user = feat->text(QStringLiteral("source")) == QLatin1String("user");
        const Node* m = material[std::size_t(d)];
        if (!turns[std::size_t(d)]) {
            const QString text = user ? feat->text(QStringLiteral("sigma")) : m ? detail::Problem::materialText(*m, QStringLiteral("sigma"), &params) : QString();
            if (!text.trimmed().isEmpty()) {
                std::optional<TensorText> t = tensorText(text, &why);
                if (!t) {
                    problem(tr("%1's electrical conductivity: %2").arg(user ? feat->name() : m->name(), why));
                    continue;
                }
                bool any = false;
                std::array<Expression, 9> sg;
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) {
                        const QString& c = t->c[i][j];
                        if (c == QLatin1String("0")) continue;
                        sg[std::size_t(3 * i + j)] = compile(c, user ? *feat : *m, QStringLiteral("sigma"));
                        const Expression& e = sg[std::size_t(3 * i + j)];
                        any = any || !e.isValid() || !e.isConstant() || e.constant() != 0;
                    }
                if (any) a_sigma[std::size_t(d)] = sg;
            }
        }
        if (coef.curves[std::size_t(d)]) continue;
        const QString im = user ? feat->text(QStringLiteral("murIm")) : m ? detail::Problem::materialText(*m, QStringLiteral("murIm"), &params) : QString();
        if (!im.trimmed().isEmpty() && im.trimmed() != QLatin1String("0"))
            coef.named[QStringLiteral("murIm")][std::size_t(d)] = compile(im, user ? *feat : *m, QStringLiteral("murIm"));
    }
    publishConductivity();
    if (!a_problems.isEmpty()) {
        *error = a_problems.join(QLatin1Char('\n'));
        return false;
    }
    return true;
}

void MagneticProblem3::publishConductivity()
{
    Solution3::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    for (int k = 0; k < 9; ++k) {
        std::vector<Expression>& v = coef.named[QStringLiteral("sigma%1").arg(k)];
        v.assign(a_sigma.size(), Expression());
        for (std::size_t d = 0; d < a_sigma.size(); ++d) v[d] = a_sigma[d][std::size_t(k)];
    }
}

double MagneticProblem3::evalAt(const Expression& e, const Vec3& x) const
{
    if (!e.isValid()) return NaN;
    if (e.isConstant()) return e.constant();
    std::vector<double> values(std::size_t(a_s.pointScope().slotCount()), NaN);
    for (int slot : e.variables()) {
        const Solution3::VariableDef& d = a_s.definitions()[std::size_t(slot)];
        using K = Solution3::VariableDef::Kind;
        values[std::size_t(slot)] = d.kind == K::X ? x.x : d.kind == K::Y ? x.y : d.kind == K::Z ? x.z : d.kind == K::Time ? a_s.time() : NaN;
    }
    return e.eval(values.data());
}

// --------------------------------------------------------------- Sources

std::vector<std::array<Expression, 3>> MagneticProblem3::sourcesOf(int only) const
{
    const std::size_t D = a_s.topology().domains.size();
    std::vector<std::array<Expression, 3>> out(D);
    const Field3& f = a_s.fields()[std::size_t(a_fi)];
    static const char* const axes[3] = {"x", "y", "z"};
    for (std::size_t d = 0; d < D; ++d) {
        if (!f.domains[d]) continue;
        for (int c = 0; c < 3; ++c) {
            QStringList terms;
            if (only < 0)
                for (const auto& j : a_external[d]) terms << QStringLiteral("(") + j[std::size_t(c)].text() + QLatin1Char(')');
            for (int k = 0; k < int(a_coils.size()); ++k) {
                const Coil& coil = a_coils[std::size_t(k)];
                if (!coil.on[d] || (only >= 0 && only != k)) continue;
                // (In the frequency domain a single conductor's current is
                // found, not given.)
                if (a_omega > 0 && coil.single) continue;
                // (A coil alone at 1 A: N / I_ec times its J.)
                const double scale = only >= 0 ? coil.perAmp : coil.scale;
                terms << QStringLiteral("%1*%2.J%3").arg(scale, 0, 'g', 17).arg(coil.ec, QString::fromLatin1(axes[c]));
            }
            if (!terms.isEmpty()) out[d][std::size_t(c)] = Expression::compile(terms.join(QLatin1String(" + ")), a_s.pointScope());
        }
    }
    return out;
}

bool MagneticProblem3::buildSources(QString* error)
{
    // Each coil's scale: N I0 over the current its terminal carries in the
    // currents physics as solved.
    for (Coil& c : a_coils) {
        const int fi = a_s.fieldIndex(c.ec);
        const Field3& ec = a_s.fields()[std::size_t(fi)];
        const double i0 = evalAt(c.current, Vec3{}), n = evalAt(c.turns, Vec3{});
        double through = NaN;
        for (const Field3::Terminal& t : ec.terminals)
            if (c.terminal.isEmpty() || t.name == c.terminal) {
                through = t.charge;
                break;
            }
        if (!ec.solved && std::isnan(through)) {
            c.scale = c.perAmp = 0;   // (not yet solved: a coupled round's first)
            continue;
        }
        if (!std::isfinite(through) || through == 0) {
            *error = tr("coil %1: its currents physics %2 has no terminal %3 carrying a current").arg(c.name, c.ec, c.terminal.isEmpty() ? tr("(any)") : c.terminal);
            return false;
        }
        // (Its current as this field's elements carry it: their straight
        // section's, where the currents physics' own is curved - a round
        // wire's 0.7 % less at 0.1 mm, its L 0.9 % low with the physics'
        // own.)
        const double carried = carriedCurrent(c);
        if (std::isfinite(carried) && carried * through > 0) {
            if (std::abs(carried / through - 1) > 1e-6)
                a_s.log << tr("%1: coil %2's current, %3 A in %4, is %5 A through this field's elements (their triangles straight): N I0 "
                              "over the latter")
                               .arg(a_node->name(), c.name)
                               .arg(through, 0, 'g', 6)
                               .arg(c.ec)
                               .arg(carried, 0, 'g', 6);
            through = carried;
        }
        c.perAmp = n / through;
        c.scale = c.perAmp * i0;
    }
    Solution3::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    const std::vector<std::array<Expression, 3>> J = sourcesOf(-1);
    static const char* const named[3] = {"Jx", "Jy", "Jz"};
    for (std::size_t d = 0; d < J.size(); ++d)
        for (int c = 0; c < 3; ++c) coef.named[QString::fromLatin1(named[c])][d] = J[d][std::size_t(c)];
    return true;
}

// ------------------------------------------------------------- Equations

bool MagneticProblem3::buildEquations(Equations& q, QString* error)
{
    const Field3& f = a_s.fields()[std::size_t(a_fi)];
    const EdgeSpace3& es = *f.edges;
    const Space3& ns = *f.space;
    const Mesh3& mesh = a_s.mesh();
    q = Equations();
    q.eqA.assign(std::size_t(es.size()), -1);
    q.eqP.assign(std::size_t(ns.size()), -1);
    q.fixedA.assign(std::size_t(es.size()), 0.0);
    for (int e = 0; e < es.elements(); ++e) {
        const int d = mesh.domain[std::size_t(e)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        const int* ed = es.dofs(e);
        for (int i = 0; i < es.perElement(); ++i)
            if (ed[i] >= 0) q.eqA[std::size_t(ed[i])] = -2;
        const int* nd = ns.dofs(e);
        for (int i = 0; i < ns.perElement(); ++i) q.eqP[std::size_t(nd[i])] = -2;
    }
    // A face's tangential A held: its edge dofs (nought, or a potential's
    // projection), and the gauge's multiplier nought on it.
    const auto edgeDofs = [&mesh, &es](int fi) {
        const Mesh3::Face& mf = mesh.faces[std::size_t(fi)];
        if (!mf.quad) return es.floorDofs(mf.plan, mf.height);
        const Mesh::Edge& pe = mesh.plan->edges[std::size_t(mf.plan)];
        return es.wallDofs(pe.a, pe.b, mf.height);
    };
    std::vector<std::pair<int, int>> held;   // (order, mesh face)
    for (int fi = 0; fi < int(mesh.faces.size()); ++fi) {
        const int face = mesh.faces[std::size_t(fi)].face;
        const FaceCondition& c = a_faces[std::size_t(face)];
        if (c.kind == FaceCondition::Free) continue;
        // (Insulation on the physics' outer faces alone: "all" boundaries,
        // its default, is not the faces between its domains.)
        const Topology3::Face& tf = a_s.topology().faces[std::size_t(face)];
        const auto in = [&f](int d) { return d >= 0 && d < int(f.domains.size()) && f.domains[std::size_t(d)]; };
        if (c.kind == FaceCondition::Insulation && in(tf.back) && in(tf.front)) continue;
        held.emplace_back(c.order, fi);
    }
    std::stable_sort(held.begin(), held.end());
    std::set<int> potential;   // edge dofs a potential holds
    std::vector<int> potentialFaces;
    for (const auto& [ord, fi] : held) {
        (void)ord;
        const bool held0 = a_faces[std::size_t(mesh.faces[std::size_t(fi)].face)].kind != FaceCondition::Potential;
        bool on = true;
        for (int dof : ns.faceDofs(fi)) on = on && dof >= 0 && q.eqP[std::size_t(dof)] != -1;
        if (!on) continue;
        for (int dof : edgeDofs(fi)) {
            if (dof < 0 || q.eqA[std::size_t(dof)] == -1) continue;
            q.eqA[std::size_t(dof)] = -3;
            if (!held0) potential.insert(dof);
            else potential.erase(dof);
        }
        for (int dof : ns.faceDofs(fi))
            if (dof >= 0) q.eqP[std::size_t(dof)] = -3;
        if (!held0) potentialFaces.push_back(fi);
    }
    // A potential's edge dofs: the projection of n × A0 on the faces' edge
    // functions' tangential traces, ∫ (n × Wi)·(n × Wj) x = ∫ (n × Wi)·(n ×
    // A0).
    if (!potential.empty()) {
        std::vector<int> local(std::size_t(es.size()), -1);
        int np = 0;
        for (int dof : potential) local[std::size_t(dof)] = np++;
        using T = Eigen::Triplet<double>;
        std::vector<T> tp;
        Eigen::VectorXd rhs = Eigen::VectorXd::Zero(np);
        const int degree = es.order() == 1 ? 2 : 4;
        PointEvaluator3 ev(a_s);
        for (int fi : potentialFaces) {
            const Mesh3::Face& mf = mesh.faces[std::size_t(fi)];
            const FaceCondition& c = a_faces[std::size_t(mf.face)];
            const Topology3::Face& tf = a_s.topology().faces[std::size_t(mf.face)];
            const int side = tf.back >= 0 && f.domains[std::size_t(tf.back)] ? tf.back : tf.front;
            for (const Space3::FacePoint& fp : ns.facePoints(fi, degree, side)) {
                if (fp.prism < 0) continue;
                const Vec3 n = outwardNormal3(ns, mf, fp);
                EdgeSpace3::Basis b;
                es.basis(fp.prism, fp.xi, fp.eta, fp.zeta, b);
                double a0[3];
                for (int k = 0; k < 3; ++k) {
                    a0[k] = evalAt(c.a0[k], fp.x);
                    if (!std::isfinite(a0[k])) a0[k] = 0;
                }
                const auto cross = [&n](const double v[3], double out[3]) {
                    out[0] = n.y * v[2] - n.z * v[1];
                    out[1] = n.z * v[0] - n.x * v[2];
                    out[2] = n.x * v[1] - n.y * v[0];
                };
                double na0[3];
                cross(a0, na0);
                const int* ed = es.dofs(fp.prism);
                for (int i = 0; i < b.count; ++i) {
                    const int li = ed[i] >= 0 ? local[std::size_t(ed[i])] : -1;
                    if (li < 0) continue;
                    double ni[3];
                    cross(b.v[i], ni);
                    rhs(li) += fp.weight * (ni[0] * na0[0] + ni[1] * na0[1] + ni[2] * na0[2]);
                    for (int j = 0; j < b.count; ++j) {
                        const int lj = ed[j] >= 0 ? local[std::size_t(ed[j])] : -1;
                        if (lj < 0) continue;
                        double nj[3];
                        cross(b.v[j], nj);
                        tp.emplace_back(li, lj, fp.weight * (ni[0] * nj[0] + ni[1] * nj[1] + ni[2] * nj[2]));
                    }
                }
            }
        }
        Eigen::SparseMatrix<double> P(np, np);
        P.setFromTriplets(tp.begin(), tp.end());
        SparseLu<double> lu;
        lu.compute(P);
        if (lu.info() != Eigen::Success) {
            *error = tr("%1: the potential held on its faces could not be projected").arg(a_node->name());
            return false;
        }
        const Eigen::VectorXd x = lu.solve(rhs);
        for (int dof : potential) q.fixedA[std::size_t(dof)] = x(local[std::size_t(dof)]);
    }
    for (std::size_t k = 0; k < q.eqA.size(); ++k)
        if (q.eqA[k] == -2) q.eqA[k] = q.nA++;
    // (No face holds the multiplier: one node's held, its constant.)
    bool anyHeld = false;
    for (int v : q.eqP) anyHeld = anyHeld || v == -3;
    for (std::size_t k = 0; k < q.eqP.size(); ++k)
        if (q.eqP[k] == -2) {
            if (!anyHeld) {
                q.eqP[k] = -3;
                anyHeld = true;
                continue;
            }
            q.eqP[k] = q.nP++;
        }
    if (q.nA == 0) {
        *error = tr("%1 has nothing to solve").arg(a_node->name());
        return false;
    }
    return true;
}

// --------------------------------------------------------------- Assembly

void MagneticProblem3::assemble(const Equations& q, const std::vector<double>& a, const std::vector<std::array<Expression, 3>>& J, bool magnets,
                                Eigen::SparseMatrix<double>& M, Eigen::VectorXd& r, double gaugeScale) const
{
    const Field3& f = a_s.fields()[std::size_t(a_fi)];
    const EdgeSpace3& es = *f.edges;
    const Space3& ns = *f.space;
    const Mesh3& mesh = a_s.mesh();
    const Solution3::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    const int nA = q.nA, n = q.nA + q.nP;
    using T = Eigen::Triplet<double>;
    std::vector<T> tm;
    r = Eigen::VectorXd::Zero(n);
    const PrismRule& rule = es.order() == 1 ? prismRule(2, 2) : prismRule(4, 3);
    const int nb = es.perElement(), npn = ns.perElement();
    tm.reserve(std::size_t(es.elements()) * std::size_t(nb * nb + 2 * nb * npn));
    PointEvaluator3 ev(a_s);
    std::vector<double> Ke(static_cast<std::size_t>(nb * nb)), Be(static_cast<std::size_t>(npn * nb)), Fe(static_cast<std::size_t>(nb));
    static const char* const brKeys[3] = {"Brx", "Bry", "Brz"};
    const std::vector<Expression>* br[3];
    for (int c = 0; c < 3; ++c) br[c] = &coef.named.at(QString::fromLatin1(brKeys[c]));
    for (int e = 0; e < es.elements(); ++e) {
        const int d = mesh.domain[std::size_t(e)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        const std::array<Expression, 9>& mur = coef.c[std::size_t(d)];
        const std::shared_ptr<const BHCurve>& curve = coef.curves[std::size_t(d)];
        bool constant = !curve;
        for (const Expression& x : mur) constant = constant && (!x.isValid() || x.isConstant());
        double nu[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
        if (constant) {
            double m9[9];
            for (int k = 0; k < 9; ++k) m9[k] = mur[std::size_t(k)].isValid() ? mur[std::size_t(k)].constant() : 0;
            reluctivity(m9, nu);
        }
        const bool sourced = J[std::size_t(d)][0].isValid() || J[std::size_t(d)][1].isValid() || J[std::size_t(d)][2].isValid();
        const bool magnet = magnets && (*br[0])[std::size_t(d)].isValid();
        std::fill(Ke.begin(), Ke.end(), 0.0);
        std::fill(Be.begin(), Be.end(), 0.0);
        std::fill(Fe.begin(), Fe.end(), 0.0);
        const int* ed = es.dofs(e);
        const int* nd = ns.dofs(e);
        const int t = mesh.prismTriangle[std::size_t(e)], layer = mesh.prismLayer[std::size_t(e)];
        const AffineMap am = affineMap(es.planNodes(), t);
        const double h = es.z(layer + 1) - es.z(layer);
        double ae[36];
        for (int i = 0; i < nb; ++i) ae[i] = ed[i] >= 0 ? a[std::size_t(ed[i])] : 0.0;
        for (const auto& qp : rule.points) {
            EdgeSpace3::Basis b;
            es.basis(e, qp[0], qp[1], qp[2], b);
            const double w = qp[3] * b.det;
            if (!constant || sourced || magnet) ev.at(e, qp[0], qp[1], qp[2], b.x);
            if (!constant && !curve) {
                double m9[9];
                for (int k = 0; k < 9; ++k) m9[k] = mur[std::size_t(k)].isValid() ? ev.eval(mur[std::size_t(k)]) : 0;
                reluctivity(m9, nu);
            }
            // B here, H(B) and its tangent dH/dB.
            double B[3] = {0, 0, 0};
            for (int i = 0; i < nb; ++i)
                for (int k = 0; k < 3; ++k) B[k] += b.c[i][k] * ae[i];
            double H[3], Tg[9];
            if (curve) {
                const double bn = std::sqrt(B[0] * B[0] + B[1] * B[1] + B[2] * B[2]);
                const double nub = curve->nu(bn), slope = curve->slope(bn);
                for (int k = 0; k < 3; ++k) H[k] = nub * B[k];
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        Tg[3 * i + j] = (i == j ? nub : 0.0) + (bn > 0 ? (slope - nub) * B[i] * B[j] / (bn * bn) : 0.0);
            } else {
                double Br[3] = {0, 0, 0};
                if (magnet)
                    for (int k = 0; k < 3; ++k) {
                        Br[k] = ev.eval((*br[k])[std::size_t(d)]);
                        if (!std::isfinite(Br[k])) Br[k] = 0;
                    }
                for (int i = 0; i < 3; ++i) {
                    H[i] = 0;
                    for (int j = 0; j < 3; ++j) H[i] += nu[3 * i + j] * (B[j] - Br[j]);
                }
                std::copy(nu, nu + 9, Tg);
            }
            double Jv[3] = {0, 0, 0};
            if (sourced)
                for (int k = 0; k < 3; ++k)
                    if (J[std::size_t(d)][std::size_t(k)].isValid()) {
                        Jv[k] = ev.eval(J[std::size_t(d)][std::size_t(k)]);
                        if (!std::isfinite(Jv[k])) Jv[k] = 0;
                    }
            for (int i = 0; i < nb; ++i) {
                // The residual: ∫ J·Wi - ∫ H·curl Wi.
                Fe[std::size_t(i)] += w * (Jv[0] * b.v[i][0] + Jv[1] * b.v[i][1] + Jv[2] * b.v[i][2] - (H[0] * b.c[i][0] + H[1] * b.c[i][1] + H[2] * b.c[i][2]));
                double tc[3];
                for (int k = 0; k < 3; ++k) tc[k] = Tg[3 * k] * b.c[i][0] + Tg[3 * k + 1] * b.c[i][1] + Tg[3 * k + 2] * b.c[i][2];
                for (int j = 0; j < nb; ++j) Ke[std::size_t(i * nb + j)] += w * (b.c[j][0] * tc[0] + b.c[j][1] * tc[1] + b.c[j][2] * tc[2]);
            }
            // The gauge: ∫ ∇qk·Wj, scaled.
            PrismShape s;
            prismShapeAt(ns.order(), qp[0], qp[1], qp[2], s);
            double g[18][3];
            nodalGradients(s, am, h, g);
            for (int k = 0; k < npn; ++k)
                for (int j = 0; j < nb; ++j) Be[std::size_t(k * nb + j)] += w * gaugeScale * (g[k][0] * b.v[j][0] + g[k][1] * b.v[j][1] + g[k][2] * b.v[j][2]);
        }
        for (int i = 0; i < nb; ++i) {
            const int ei = ed[i] >= 0 ? q.eqA[std::size_t(ed[i])] : -1;
            if (ei < 0) continue;
            r(ei) += Fe[std::size_t(i)];
            for (int j = 0; j < nb; ++j) {
                const int ej = ed[j] >= 0 ? q.eqA[std::size_t(ed[j])] : -1;
                if (ej >= 0) tm.emplace_back(ei, ej, Ke[std::size_t(i * nb + j)]);
            }
        }
        for (int k = 0; k < npn; ++k) {
            const int pk = q.eqP[std::size_t(nd[k])];
            for (int j = 0; j < nb; ++j) {
                const int ej = ed[j] >= 0 ? q.eqA[std::size_t(ed[j])] : -1;
                if (ej < 0) continue;
                const double v = Be[std::size_t(k * nb + j)];
                // (Its constraint's residual -B a, and Bᵀψ's: ψ is solved
                // for whole, from nought, each step.)
                if (pk >= 0) {
                    tm.emplace_back(nA + pk, ej, v);
                    tm.emplace_back(ej, nA + pk, v);
                    r(nA + pk) -= v * ae[j];
                }
            }
            // (Held dofs' share of the constraint: B times their values.)
            if (pk >= 0)
                for (int j = 0; j < nb; ++j)
                    if (ed[j] >= 0 && q.eqA[std::size_t(ed[j])] == -3) r(nA + pk) -= Be[std::size_t(k * nb + j)] * ae[j];
        }
    }
    // The surface currents: ∫ Js·Wi on their faces.
    const int degree = es.order() == 1 ? 2 : 4;
    for (int fi = 0; fi < int(mesh.faces.size()); ++fi) {
        const Mesh3::Face& mf = mesh.faces[std::size_t(fi)];
        const auto& sheets = a_sheets[std::size_t(mf.face)];
        if (sheets.empty()) continue;
        const Topology3::Face& tf = a_s.topology().faces[std::size_t(mf.face)];
        const int side = tf.back >= 0 && f.domains[std::size_t(tf.back)] ? tf.back : tf.front;
        for (const Space3::FacePoint& fp : ns.facePoints(fi, degree, side)) {
            if (fp.prism < 0) continue;
            EdgeSpace3::Basis b;
            es.basis(fp.prism, fp.xi, fp.eta, fp.zeta, b);
            ev.at(fp.prism, fp.xi, fp.eta, fp.zeta, fp.x);
            double js[3] = {0, 0, 0};
            for (const auto& s : sheets)
                for (int k = 0; k < 3; ++k) {
                    const double v = ev.eval(s[std::size_t(k)]);
                    if (std::isfinite(v)) js[k] += v;
                }
            const int* ed = es.dofs(fp.prism);
            for (int i = 0; i < b.count; ++i) {
                const int ei = ed[i] >= 0 ? q.eqA[std::size_t(ed[i])] : -1;
                if (ei >= 0) r(ei) += fp.weight * (js[0] * b.v[i][0] + js[1] * b.v[i][1] + js[2] * b.v[i][2]);
            }
        }
    }
    M.resize(n, n);
    M.setFromTriplets(tm.begin(), tm.end());
}

// ------------------------------------------------------------------ Solve

bool MagneticProblem3::solve(const Options3& o, QString* error)
{
    Field3& f = field();
    const EdgeSpace3& es = *f.edges;
    const Space3& ns = *f.space;
    const QString name = a_node->name();
    // (A Frequency Domain step's: harmonic, eddy currents in its
    // conductors.)
    if (o.frequency > 0) return solveHarmonic(o, error);
    a_omega = 0;
    f.terminals.clear();
    a_U.clear();
    a_I.clear();
    a_flux.clear();
    a_Z.clear();
    a_ports.clear();
    if (!buildSources(error)) return false;
    Equations q;
    if (!buildEquations(q, error)) return false;
    const double gaugeScale = gaugeScaleOf();
    // A: its held values, else as it was (Newton's start).
    std::vector<double> a(std::size_t(es.size()), 0.0);
    const std::size_t nE = std::size_t(es.size());
    if (f.values.size() >= nE)
        for (std::size_t k = 0; k < nE; ++k) a[k] = q.eqA[k] >= 0 ? f.values[k] : 0.0;
    for (std::size_t k = 0; k < nE; ++k)
        if (q.eqA[k] == -3) a[k] = q.fixedA[k];
    const std::vector<std::array<Expression, 3>> J = sourcesOf(-1);
    std::vector<double> psi(std::size_t(ns.size()), 0.0);
    const int rounds = a_nonlinear ? std::max(2, o.maxIterations) : 1;
    bool converged = !a_nonlinear;
    // The saddle point is real, symmetric and indefinite: a real LDLᵀ with
    // pivoting (an LU of a solenoid's 53,705 unknowns took 18.6 s; the
    // complex symmetric LDLᵀ it was factorized by before, its imaginary
    // parts nought, some four times the work of the real one).
    // (One step of iterative refinement against the matrix: the pivots of a
    // saddle point scaled as it is lose a digit or two.)
    double residual = 0;
    const auto solveWith = [this, &residual](const Eigen::VectorXd& b) -> Eigen::VectorXd {
        Eigen::VectorXd x = a_lu->solve(b);
        const Eigen::VectorXd r0 = b - a_factored * x;
        x += a_lu->solve(r0);
        const double bn = b.norm();
        residual = bn > 0 ? (b - a_factored * x).norm() / bn : 0;
        return x;
    };
    for (int it = 0; it < rounds; ++it) {
        if (o.cancelled && o.cancelled()) {
            *error = tr("cancelled");
            return false;
        }
        Eigen::SparseMatrix<double> M;
        Eigen::VectorXd r;
        QElapsedTimer clock;
        clock.start();
        // (A Newton's step's said, past the first.)
        const QString step = a_nonlinear && it > 0 ? tr(", Newton's step %1").arg(it + 1) : QString();
        o.tell(tr("%1: assembling%2").arg(name, step));
        assemble(q, a, J, a_magnets, M, r, gaugeScale);
        const qint64 assembled = clock.elapsed();
        if (a_lu && a_lu->info() == Eigen::Success && sameMatrix(M, a_factored)) {
            a_s.log << tr("%1: assembled in %2 s; its matrix unchanged, its factors kept").arg(name).arg(assembled / 1000.0, 0, 'f', 2);
        } else {
            a_lu.reset();
            a_factored = Eigen::SparseMatrix<double>();
            auto lu = std::make_unique<SparseRealLdlt>();
            o.tell(tr("%1: factorizing, %2 unknowns%3").arg(name).arg(M.rows()).arg(step));
            lu->compute(M);
            // (Its analysis - the ordering, kept from a pattern factorized
            // before or made - and its numeric factorization each said, and
            // its factors' size, where the backend says them: Accelerate's
            // and MUMPS's, not an LU's.)
            const QString factorized = tr("%1: assembled in %2 s, %3 nonzeros; factorized in %4 s")
                                           .arg(name)
                                           .arg(assembled / 1000.0, 0, 'f', 2)
                                           .arg(M.nonZeros())
                                           .arg((clock.elapsed() - assembled) / 1000.0, 0, 'f', 2);
            if (lu->factorBytes() > 0)
                a_s.log << factorized
                               + tr(" (its analysis %1 s%2, its numeric factorization %3 s; its factors %4 MB)")
                                     .arg(lu->analysisSeconds(), 0, 'f', 2)
                                     .arg(lu->reusedAnalysis() ? tr(", kept from before") : QString())
                                     .arg(lu->factorSeconds(), 0, 'f', 2)
                                     .arg(lu->factorBytes() / 1048576.0, 0, 'f', 1);
            else
                a_s.log << factorized + tr(" (%1)").arg(lu->name());
            if (lu->info() != Eigen::Success) {
                *error = tr("%1: its system could not be factorized (%2) - is A held somewhere (Magnetic Insulation, a Magnetic Potential)?")
                             .arg(name, QString::fromStdString(lu->lastErrorMessage()));
                return false;
            }
            a_lu = std::move(lu);
            a_factored = std::move(M);
        }
        if (it == 0)
            a_s.log << tr("%1: %2 unknowns (%3 of A on edge elements of order %4, %5 of the gauge's multiplier), %6")
                           .arg(name)
                           .arg(q.nA + q.nP)
                           .arg(q.nA)
                           .arg(es.order())
                           .arg(q.nP)
                           .arg(a_lu->name());
        o.tell(tr("%1: solving with its factors%2").arg(name, step));
        const Eigen::VectorXd x = solveWith(r);
        if (it == 0) a_s.log << tr("%1: relative residual %2, refined once").arg(name).arg(residual, 0, 'g', 2);
        if (!x.allFinite()) {
            *error = tr("%1: its solve is not finite").arg(name);
            return false;
        }
        double num = 0, den = 0;
        for (std::size_t k = 0; k < nE; ++k)
            if (q.eqA[k] >= 0) {
                const double dv = x(q.eqA[k]);
                a[k] += dv;
                num += dv * dv;
                den += a[k] * a[k];
            }
        for (std::size_t k = 0; k < psi.size(); ++k) psi[k] = q.eqP[k] >= 0 ? x(q.nA + q.eqP[k]) / gaugeScale : 0.0;
        if (!a_nonlinear) break;
        const double change = den > 0 ? std::sqrt(num / den) : std::sqrt(num);
        a_s.log << tr("%1, Newton's step %2: relative change %3").arg(name).arg(it + 1).arg(change, 0, 'g', 3);
        if (change < o.tolerance && it > 0) {
            converged = true;
            break;
        }
    }
    if (!converged) {
        *error = tr("%1: Newton's method did not converge in %2 steps").arg(name).arg(rounds);
        return false;
    }
    f.values.assign(nE + std::size_t(ns.size()), 0.0);
    std::copy(a.begin(), a.end(), f.values.begin());
    std::copy(psi.begin(), psi.end(), f.values.begin() + std::ptrdiff_t(nE));
    f.solved = true;
    // The coils swept at 1 A each, the others off and no magnets (a linear
    // model's: its matrix's factors kept): each one's flux through every
    // other, the inductance matrix L_ij = ∫ A_j·J_i / I_i.
    a_inductance.clear();
    if (!a_coils.empty() && !a_nonlinear && o.sweep) {
        const int nc = int(a_coils.size());
        std::vector<std::vector<std::array<Expression, 3>>> unit(static_cast<std::size_t>(nc));
        for (int c = 0; c < nc; ++c) unit[std::size_t(c)] = sourcesOf(c);
        std::vector<std::vector<double>> fields(static_cast<std::size_t>(nc));
        for (int c = 0; c < nc; ++c) {
            o.tell(tr("%1: the inductance matrix, coil %2 of %3").arg(name).arg(c + 1).arg(nc));
            std::vector<double> a0(nE, 0.0);   // (held values nought)
            Eigen::SparseMatrix<double> M;
            Eigen::VectorXd r;
            assemble(q, a0, unit[std::size_t(c)], false, M, r, gaugeScale);
            const Eigen::VectorXd x = solveWith(r);
            for (std::size_t k = 0; k < nE; ++k)
                if (q.eqA[k] >= 0) a0[k] = x(q.eqA[k]);
            fields[std::size_t(c)] = std::move(a0);
        }
        a_inductance.assign(std::size_t(nc), std::vector<double>(std::size_t(nc), 0.0));
        const Mesh3& mesh = a_s.mesh();
        const PrismRule& rule = es.order() == 1 ? prismRule(2, 2) : prismRule(4, 3);
        PointEvaluator3 ev(a_s);
        for (int e = 0; e < es.elements(); ++e) {
            const int d = mesh.domain[std::size_t(e)];
            if (d < 0 || !f.domains[std::size_t(d)]) continue;
            const int* ed = es.dofs(e);
            for (const auto& qp : rule.points) {
                EdgeSpace3::Basis b;
                es.basis(e, qp[0], qp[1], qp[2], b);
                ev.at(e, qp[0], qp[1], qp[2], b.x);
                for (int i = 0; i < nc; ++i) {
                    const auto& ji = unit[std::size_t(i)][std::size_t(d)];
                    if (!ji[0].isValid() && !ji[1].isValid() && !ji[2].isValid()) continue;
                    double jv[3];
                    for (int k = 0; k < 3; ++k) jv[k] = ji[std::size_t(k)].isValid() ? ev.eval(ji[std::size_t(k)]) : 0.0;
                    for (int j = 0; j < nc; ++j) {
                        double A[3] = {0, 0, 0};
                        for (int m = 0; m < b.count; ++m)
                            if (ed[m] >= 0)
                                for (int k = 0; k < 3; ++k) A[k] += b.v[m][k] * fields[std::size_t(j)][std::size_t(ed[m])];
                        a_inductance[std::size_t(i)][std::size_t(j)] += qp[3] * b.det * (A[0] * jv[0] + A[1] * jv[1] + A[2] * jv[2]);
                    }
                }
            }
        }
    }
    return true;
}

// --------------------------------------------------- The frequency domain

/// ν = (µ0 (µr' - j µr''))⁻¹, a 3 x 3 complex tensor (row by row): µr'' on
/// the diagonal (a core's losses, isotropic).
bool complexReluctivity(const double mur[9], double murIm, std::complex<double> nu[9])
{
    Eigen::Matrix3cd m;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m(i, j) = Mu0 * std::complex<double>(mur[3 * i + j], i == j ? -murIm : 0.0);
    Eigen::FullPivLU<Eigen::Matrix3cd> lu(m);
    if (!lu.isInvertible()) return false;
    const Eigen::Matrix3cd inv = lu.inverse();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) nu[3 * i + j] = inv(i, j);
    return true;
}

std::vector<int> MagneticProblem3::heldFacesOf(const QString& tag, QStringList* names) const
{
    const Topology3& topo = a_s.topology();
    const Model& model = a_s.model();
    const std::size_t F = topo.faces.size();
    std::vector<int> cond(F, -1);
    names->clear();
    const Node* en = model.find(tag);
    if (!en) return cond;
    for (const Node& feat : en->children) {
        if (!feat.enabled) continue;
        const NodeKind* k = feat.kind();
        if (!k || k->selection != Level::Boundary) continue;
        const QString t = feat.type;
        const bool terminal = t.endsWith(QLatin1String("/terminal")) || t.endsWith(QLatin1String("/floating"));
        const bool ground = t.endsWith(QLatin1String("/ground")) || t.endsWith(QLatin1String("/potential"));
        const bool other = t.endsWith(QLatin1String("/insulation")) || t.endsWith(QLatin1String("/zerocharge"));
        if (!terminal && !ground && !other) continue;
        QString why;
        const QVector<int> picked = resolveSelection3(topo, model, k->selection, feat.selection(), &why);
        int v = -1;
        if (ground) v = -2;
        else if (terminal) {
            *names << (t.endsWith(QLatin1String("/terminal")) ? feat.text(QStringLiteral("name")) : feat.name());
            v = int(names->size()) - 1;
        }
        for (int b : picked)
            if (b >= 0 && b < int(F)) cond[std::size_t(b)] = v;
    }
    return cond;
}

double MagneticProblem3::carriedCurrent(const Coil& c) const
{
    const Field3& f = a_s.fields()[std::size_t(a_fi)];
    const EdgeSpace3& es = *f.edges;
    const Space3& ns = *f.space;
    const Mesh3& mesh = a_s.mesh();
    QStringList names;
    const std::vector<int> cond = heldFacesOf(c.ec, &names);
    if (names.isEmpty()) return NaN;
    const int mine = c.terminal.isEmpty() ? 0 : int(names.indexOf(c.terminal));
    if (mine < 0) return NaN;
    std::vector<char> terminal(std::size_t(ns.size()), 0);
    for (int fi = 0; fi < int(mesh.faces.size()); ++fi)
        if (cond[std::size_t(mesh.faces[std::size_t(fi)].face)] == mine)
            for (int dof : ns.faceDofs(fi))
                if (dof >= 0) terminal[std::size_t(dof)] = 1;
    static const char* const axes[3] = {"x", "y", "z"};
    Expression J[3];
    for (int k = 0; k < 3; ++k) J[k] = Expression::compile(QStringLiteral("%1.J%2").arg(c.ec, QString::fromLatin1(axes[k])), a_s.pointScope());
    if (!J[0].isValid() || !J[1].isValid() || !J[2].isValid()) return NaN;
    const PrismRule& rule = es.order() == 1 ? prismRule(2, 2) : prismRule(4, 3);
    PointEvaluator3 ev(a_s);
    double sum = 0;
    bool any = false;
    for (int e = 0; e < es.elements(); ++e) {
        const int d = mesh.domain[std::size_t(e)];
        if (d < 0 || !f.domains[std::size_t(d)] || !c.on[std::size_t(d)]) continue;
        const int* nd = ns.dofs(e);
        bool touches = false;
        for (int k = 0; k < ns.perElement() && !touches; ++k) touches = terminal[std::size_t(nd[k])];
        if (!touches) continue;
        any = true;
        const int t = mesh.prismTriangle[std::size_t(e)], layer = mesh.prismLayer[std::size_t(e)];
        const AffineMap am = affineMap(es.planNodes(), t);
        const double hz = es.z(layer + 1) - es.z(layer);
        for (const auto& qp : rule.points) {
            EdgeSpace3::Basis b;
            es.basis(e, qp[0], qp[1], qp[2], b);
            ev.at(e, qp[0], qp[1], qp[2], b.x);
            PrismShape sh;
            prismShapeAt(ns.order(), qp[0], qp[1], qp[2], sh);
            double g[18][3];
            nodalGradients(sh, am, hz, g);
            double gh[3] = {0, 0, 0};
            for (int k = 0; k < sh.count; ++k)
                if (terminal[std::size_t(nd[k])])
                    for (int m = 0; m < 3; ++m) gh[m] += g[k][m];
            double jv[3];
            for (int k = 0; k < 3; ++k) {
                jv[k] = ev.eval(J[k]);
                if (!std::isfinite(jv[k])) jv[k] = 0;
            }
            sum -= qp[3] * b.det * (jv[0] * gh[0] + jv[1] * gh[1] + jv[2] * gh[2]);
        }
    }
    return any ? sum : NaN;
}

bool MagneticProblem3::buildConductors(Conductors& h, double omega, QString* error)
{
    const Field3& f = a_s.fields()[std::size_t(a_fi)];
    const Space3& ns = *f.space;
    const Mesh3& mesh = a_s.mesh();
    const Topology3& topo = a_s.topology();
    const std::size_t D = topo.domains.size(), F = topo.faces.size(), N = std::size_t(ns.size());
    // A single conductor's domains that have no conductivity of their own:
    // its currents physics' (the same metal).
    for (const Coil& c : a_coils) {
        if (!c.single) continue;
        const int fi = a_s.fieldIndex(c.ec);
        if (fi < 0) continue;
        const Solution3::Coefficients& ec = a_s.coefficients()[std::size_t(fi)];
        for (std::size_t d = 0; d < D && d < ec.c.size(); ++d) {
            if (!c.on[d] || !f.domains[d] || conducts(int(d))) continue;
            bool any = false;
            for (const Expression& e : ec.c[d]) any = any || (e.isValid() && !(e.isConstant() && e.constant() == 0));
            if (any) a_sigma[d] = ec.c[d];
        }
    }
    publishConductivity();
    h = Conductors();
    h.conducting.assign(D, 0);
    for (std::size_t d = 0; d < D; ++d) h.conducting[d] = f.domains[d] && conducts(int(d)) ? 1 : 0;
    // The faces each single conductor's currents physics holds - the last
    // condition on a face its own, as that physics takes them: its coil's
    // terminal (the coil), or a ground (any other it holds: -2).
    std::vector<int> faceRole(F, -1);
    QStringList tags;
    for (const Coil& c : a_coils)
        if (c.single && !tags.contains(c.ec)) tags << c.ec;
    for (const QString& tag : tags) {
        QStringList names;
        const std::vector<int> cond = heldFacesOf(tag, &names);
        for (std::size_t b = 0; b < F; ++b) {
            if (cond[b] == -1) continue;
            int role = -2;
            if (cond[b] >= 0) {
                const QString& name = names[cond[b]];
                for (int k = 0; k < int(a_coils.size()); ++k) {
                    const Coil& c = a_coils[std::size_t(k)];
                    if (c.single && c.ec == tag && (c.terminal.isEmpty() ? name == names.first() : name == c.terminal)) role = k;
                }
            }
            // (A ground's face wins over a terminal another physics holds.)
            if (faceRole[b] == -1 || role == -2) faceRole[b] = role;
        }
    }
    h.eqV.assign(N, -1);
    h.heldPsi.assign(N, cd(0));
    h.electrode.assign(N, 0);
    h.terminalOf.assign(N, -1);
    for (int e = 0; e < ns.elements(); ++e) {
        const int d = mesh.domain[std::size_t(e)];
        if (d < 0 || !h.conducting[std::size_t(d)]) continue;
        const int* nd = ns.dofs(e);
        for (int k = 0; k < ns.perElement(); ++k) h.eqV[std::size_t(nd[k])] = -2;
    }
    std::vector<char> ground(N, 0);
    h.electrodeFaces.assign(mesh.faces.size(), 0);
    for (int fi = 0; fi < int(mesh.faces.size()); ++fi) {
        const int role = faceRole[std::size_t(mesh.faces[std::size_t(fi)].face)];
        if (role == -1) continue;
        h.electrodeFaces[std::size_t(fi)] = 1;
        for (int dof : ns.faceDofs(fi)) {
            if (dof < 0 || h.eqV[std::size_t(dof)] == -1) continue;
            h.electrode[std::size_t(dof)] = 1;
            if (role >= 0) h.terminalOf[std::size_t(dof)] = role;
            else ground[std::size_t(dof)] = 1;
        }
    }
    for (std::size_t k = 0; k < N; ++k)
        if (ground[k]) h.terminalOf[k] = -1;
    // Each conductor (the nodes its elements join): one that touches no
    // terminal or ground has its potential's constant held at one node;
    // a coil's must touch a ground (its current's way back).
    std::vector<int> parent(N);
    for (std::size_t k = 0; k < N; ++k) parent[k] = int(k);
    const auto root = [&parent](int k) {
        while (parent[std::size_t(k)] != k) {
            parent[std::size_t(k)] = parent[std::size_t(parent[std::size_t(k)])];
            k = parent[std::size_t(k)];
        }
        return k;
    };
    for (int e = 0; e < ns.elements(); ++e) {
        const int d = mesh.domain[std::size_t(e)];
        if (d < 0 || !h.conducting[std::size_t(d)]) continue;
        const int* nd = ns.dofs(e);
        const int r0 = root(nd[0]);
        for (int k = 1; k < ns.perElement(); ++k) {
            const int rk = root(nd[k]);
            if (rk != r0) parent[std::size_t(rk)] = r0;
        }
    }
    std::vector<char> grounded(N, 0), touched(N, 0);
    for (std::size_t k = 0; k < N; ++k) {
        if (h.eqV[k] == -1) continue;
        const int r = root(int(k));
        if (ground[k]) grounded[std::size_t(r)] = 1;
        if (h.electrode[k]) touched[std::size_t(r)] = 1;
    }
    for (int c = 0; c < int(a_coils.size()); ++c) {
        const Coil& coil = a_coils[std::size_t(c)];
        if (!coil.single) continue;
        int at = -1;
        for (std::size_t k = 0; k < N && at < 0; ++k)
            if (h.terminalOf[k] == c) at = int(k);
        if (at < 0) {
            *error = tr("coil %1: in the frequency domain a single conductor is fed at its currents physics' terminal (%2 of %3) - none is on a "
                        "domain of %4 that conducts (its material's sigma, Ampère's Law's or %3's)")
                         .arg(coil.name, coil.terminal.isEmpty() ? tr("its first") : coil.terminal, coil.ec, a_node->name());
            return false;
        }
        if (!grounded[std::size_t(root(at))]) {
            *error = tr("coil %1: its conductor touches no ground of its currents physics %2 (a Ground on it): in the frequency domain its current "
                        "would have no way back")
                         .arg(coil.name, coil.ec);
            return false;
        }
    }
    for (std::size_t k = 0; k < N; ++k) {
        if (h.eqV[k] == -1) continue;
        const int r = root(int(k));
        if (!touched[std::size_t(r)]) {
            touched[std::size_t(r)] = 1;   // (its first node held, the rest free)
            h.eqV[k] = -3;
        }
    }
    // The equations: each coil's terminal one (its current held), or held
    // at its voltage; a ground's nought; the rest one each.
    const cd jw(0, omega);
    h.eqU.assign(a_coils.size(), -1);
    int next = 0;
    for (int c = 0; c < int(a_coils.size()); ++c) {
        const Coil& coil = a_coils[std::size_t(c)];
        if (coil.single && (coil.role == Coil::Current || coil.role == Coil::Floating)) h.eqU[std::size_t(c)] = next++;
    }
    for (std::size_t k = 0; k < N; ++k) {
        if (h.eqV[k] != -2) continue;
        if (!h.electrode[k]) {
            h.eqV[k] = next++;
            continue;
        }
        const int c = h.terminalOf[k];
        h.eqV[k] = c >= 0 && h.eqU[std::size_t(c)] >= 0 ? h.eqU[std::size_t(c)] : -3;
        if (c >= 0 && a_coils[std::size_t(c)].role == Coil::Voltage) h.heldPsi[k] = evalAt(a_coils[std::size_t(c)].voltage, Vec3{}) / jw;
    }
    h.nV = next;
    return true;
}

void MagneticProblem3::assembleHarmonic(const Equations& q, const Conductors& h, double omega, const std::vector<double>& held,
                                        const std::vector<cd>& heldPsi, const std::vector<std::array<Expression, 3>>& J, bool matrix,
                                        bool sheets, Eigen::SparseMatrix<cd>& M, Eigen::VectorXcd& r, double gaugeScale) const
{
    const Field3& f = a_s.fields()[std::size_t(a_fi)];
    const EdgeSpace3& es = *f.edges;
    const Space3& ns = *f.space;
    const Mesh3& mesh = a_s.mesh();
    const Solution3::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    const int oP = q.nA, oV = q.nA + q.nP, n = oV + h.nV;
    const cd jw(0, omega);
    using T = Eigen::Triplet<cd>;
    std::vector<T> tm;
    r = Eigen::VectorXcd::Zero(n);
    const PrismRule& rule = es.order() == 1 ? prismRule(2, 2) : prismRule(4, 3);
    const int nb = es.perElement(), npn = ns.perElement();
    if (matrix) tm.reserve(std::size_t(es.elements()) * std::size_t(nb * nb + 2 * nb * npn));
    PointEvaluator3 ev(a_s);
    std::vector<cd> Ke(static_cast<std::size_t>(nb * nb)), Cav(static_cast<std::size_t>(nb * npn)), Lvv(static_cast<std::size_t>(npn * npn)), Fe(static_cast<std::size_t>(nb));
    std::vector<double> Be(static_cast<std::size_t>(npn * nb));
    const std::vector<Expression>& murIm = coef.named.at(QStringLiteral("murIm"));
    for (int e = 0; e < es.elements(); ++e) {
        const int d = mesh.domain[std::size_t(e)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        const std::array<Expression, 9>& mur = coef.c[std::size_t(d)];
        const Expression& im = murIm[std::size_t(d)];
        bool constNu = !im.isValid() || im.isConstant();
        for (const Expression& x : mur) constNu = constNu && (!x.isValid() || x.isConstant());
        const bool conducting = h.conducting[std::size_t(d)];
        const std::array<Expression, 9>& sg = a_sigma[std::size_t(d)];
        bool constSigma = true;
        for (const Expression& x : sg) constSigma = constSigma && (!x.isValid() || x.isConstant());
        cd nu[9];
        double s9[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
        const auto readNu = [&](bool at) {
            double m9[9];
            for (int k = 0; k < 9; ++k) m9[k] = mur[std::size_t(k)].isValid() ? (at ? ev.eval(mur[std::size_t(k)]) : mur[std::size_t(k)].constant()) : 0;
            const double mi = im.isValid() ? (at ? ev.eval(im) : im.constant()) : 0.0;
            complexReluctivity(m9, std::isfinite(mi) ? mi : 0.0, nu);
        };
        const auto readSigma = [&](bool at) {
            for (int k = 0; k < 9; ++k) {
                s9[k] = sg[std::size_t(k)].isValid() ? (at ? ev.eval(sg[std::size_t(k)]) : sg[std::size_t(k)].constant()) : 0;
                if (!std::isfinite(s9[k])) s9[k] = 0;
            }
        };
        if (constNu) readNu(false);
        if (conducting && constSigma) readSigma(false);
        const bool sourced = J[std::size_t(d)][0].isValid() || J[std::size_t(d)][1].isValid() || J[std::size_t(d)][2].isValid();
        std::fill(Ke.begin(), Ke.end(), cd(0));
        std::fill(Cav.begin(), Cav.end(), cd(0));
        std::fill(Lvv.begin(), Lvv.end(), cd(0));
        std::fill(Fe.begin(), Fe.end(), cd(0));
        std::fill(Be.begin(), Be.end(), 0.0);
        const int* ed = es.dofs(e);
        const int* nd = ns.dofs(e);
        const int t = mesh.prismTriangle[std::size_t(e)], layer = mesh.prismLayer[std::size_t(e)];
        const AffineMap am = affineMap(es.planNodes(), t);
        const double hz = es.z(layer + 1) - es.z(layer);
        for (const auto& qp : rule.points) {
            EdgeSpace3::Basis b;
            es.basis(e, qp[0], qp[1], qp[2], b);
            const double w = qp[3] * b.det;
            if (!constNu || (conducting && !constSigma) || sourced) ev.at(e, qp[0], qp[1], qp[2], b.x);
            if (!constNu) readNu(true);
            if (conducting && !constSigma) readSigma(true);
            PrismShape s;
            prismShapeAt(ns.order(), qp[0], qp[1], qp[2], s);
            double g[18][3];
            nodalGradients(s, am, hz, g);
            double Jv[3] = {0, 0, 0};
            if (sourced)
                for (int k = 0; k < 3; ++k)
                    if (J[std::size_t(d)][std::size_t(k)].isValid()) {
                        Jv[k] = ev.eval(J[std::size_t(d)][std::size_t(k)]);
                        if (!std::isfinite(Jv[k])) Jv[k] = 0;
                    }
            if (matrix)
                for (int j = 0; j < nb; ++j) {
                    // ν curl Wj, σ Wj
                    cd nc[3];
                    for (int k = 0; k < 3; ++k) nc[k] = nu[3 * k] * b.c[j][0] + nu[3 * k + 1] * b.c[j][1] + nu[3 * k + 2] * b.c[j][2];
                    double sw[3] = {0, 0, 0};
                    if (conducting)
                        for (int k = 0; k < 3; ++k) sw[k] = s9[3 * k] * b.v[j][0] + s9[3 * k + 1] * b.v[j][1] + s9[3 * k + 2] * b.v[j][2];
                    for (int i = 0; i < nb; ++i) {
                        cd v = b.c[i][0] * nc[0] + b.c[i][1] * nc[1] + b.c[i][2] * nc[2];
                        if (conducting) v += jw * (b.v[i][0] * sw[0] + b.v[i][1] * sw[1] + b.v[i][2] * sw[2]);
                        Ke[std::size_t(i * nb + j)] += w * v;
                    }
                }
            if (conducting)
                for (int k = 0; k < npn; ++k) {
                    double sgk[3];
                    for (int c = 0; c < 3; ++c) sgk[c] = s9[3 * c] * g[k][0] + s9[3 * c + 1] * g[k][1] + s9[3 * c + 2] * g[k][2];
                    for (int i = 0; i < nb; ++i) Cav[std::size_t(i * npn + k)] += w * jw * (b.v[i][0] * sgk[0] + b.v[i][1] * sgk[1] + b.v[i][2] * sgk[2]);
                    for (int l = 0; l < npn; ++l) Lvv[std::size_t(l * npn + k)] += w * jw * (g[l][0] * sgk[0] + g[l][1] * sgk[1] + g[l][2] * sgk[2]);
                }
            for (int k = 0; k < npn; ++k)
                for (int j = 0; j < nb; ++j) Be[std::size_t(k * nb + j)] += w * gaugeScale * (g[k][0] * b.v[j][0] + g[k][1] * b.v[j][1] + g[k][2] * b.v[j][2]);
            if (sourced)
                for (int i = 0; i < nb; ++i) Fe[std::size_t(i)] += w * (Jv[0] * b.v[i][0] + Jv[1] * b.v[i][1] + Jv[2] * b.v[i][2]);
        }
        const auto vEq = [&](int k) { return h.eqV[std::size_t(nd[k])]; };
        for (int i = 0; i < nb; ++i) {
            const int ei = ed[i] >= 0 ? q.eqA[std::size_t(ed[i])] : -1;
            if (ei < 0) continue;
            r(ei) += Fe[std::size_t(i)];
            for (int j = 0; j < nb; ++j) {
                const int ej = ed[j] >= 0 ? q.eqA[std::size_t(ed[j])] : -1;
                const cd v = Ke[std::size_t(i * nb + j)];
                if (ej >= 0) {
                    if (matrix) tm.emplace_back(ei, ej, v);
                } else if (ed[j] >= 0 && ej == -3 && held[std::size_t(ed[j])] != 0) {
                    r(ei) -= v * held[std::size_t(ed[j])];
                }
            }
            if (conducting)
                for (int k = 0; k < npn; ++k) {
                    const int vk = vEq(k);
                    const cd v = Cav[std::size_t(i * npn + k)];
                    if (vk >= 0) {
                        if (matrix) tm.emplace_back(ei, oV + vk, v);
                    } else if (vk == -3 && heldPsi[std::size_t(nd[k])] != cd(0)) {
                        r(ei) -= v * heldPsi[std::size_t(nd[k])];
                    }
                }
        }
        for (int k = 0; k < npn; ++k) {
            const int pk = q.eqP[std::size_t(nd[k])];
            if (pk < 0) continue;
            for (int j = 0; j < nb; ++j) {
                const int ej = ed[j] >= 0 ? q.eqA[std::size_t(ed[j])] : -1;
                const double v = Be[std::size_t(k * nb + j)];
                if (ej >= 0) {
                    if (matrix) {
                        tm.emplace_back(oP + pk, ej, v);
                        tm.emplace_back(ej, oP + pk, v);
                    }
                } else if (ed[j] >= 0 && ej == -3 && held[std::size_t(ed[j])] != 0) {
                    r(oP + pk) -= v * held[std::size_t(ed[j])];
                }
            }
        }
        if (!conducting) continue;
        for (int k = 0; k < npn; ++k) {
            const int vk = vEq(k);
            if (vk < 0) continue;
            for (int j = 0; j < nb; ++j) {
                const int ej = ed[j] >= 0 ? q.eqA[std::size_t(ed[j])] : -1;
                const cd v = Cav[std::size_t(j * npn + k)];
                if (ej >= 0) {
                    if (matrix) tm.emplace_back(oV + vk, ej, v);
                } else if (ed[j] >= 0 && ej == -3 && held[std::size_t(ed[j])] != 0) {
                    r(oV + vk) -= v * held[std::size_t(ed[j])];
                }
            }
            for (int l = 0; l < npn; ++l) {
                const int vl = vEq(l);
                const cd v = Lvv[std::size_t(l * npn + k)];
                if (vl >= 0) {
                    if (matrix) tm.emplace_back(oV + vk, oV + vl, v);
                } else if (vl == -3 && heldPsi[std::size_t(nd[l])] != cd(0)) {
                    r(oV + vk) -= v * heldPsi[std::size_t(nd[l])];
                }
            }
        }
    }
    // The surface currents: ∫ Js·Wi on their faces (the sweep's: none).
    if (sheets) {
        const int degree = es.order() == 1 ? 2 : 4;
        for (int fi = 0; fi < int(mesh.faces.size()); ++fi) {
            const Mesh3::Face& mf = mesh.faces[std::size_t(fi)];
            const auto& sheets = a_sheets[std::size_t(mf.face)];
            if (sheets.empty()) continue;
            const Topology3::Face& tf = a_s.topology().faces[std::size_t(mf.face)];
            const int side = tf.back >= 0 && f.domains[std::size_t(tf.back)] ? tf.back : tf.front;
            for (const Space3::FacePoint& fp : ns.facePoints(fi, degree, side)) {
                if (fp.prism < 0) continue;
                EdgeSpace3::Basis b;
                es.basis(fp.prism, fp.xi, fp.eta, fp.zeta, b);
                ev.at(fp.prism, fp.xi, fp.eta, fp.zeta, fp.x);
                double js[3] = {0, 0, 0};
                for (const auto& sh : sheets)
                    for (int k = 0; k < 3; ++k) {
                        const double v = ev.eval(sh[std::size_t(k)]);
                        if (std::isfinite(v)) js[k] += v;
                    }
                const int* ed = es.dofs(fp.prism);
                for (int i = 0; i < b.count; ++i) {
                    const int ei = ed[i] >= 0 ? q.eqA[std::size_t(ed[i])] : -1;
                    if (ei >= 0) r(ei) += fp.weight * (js[0] * b.v[i][0] + js[1] * b.v[i][1] + js[2] * b.v[i][2]);
                }
            }
        }
    }
    if (matrix) {
        M.resize(n, n);
        M.setFromTriplets(tm.begin(), tm.end());
    }
}

void MagneticProblem3::skinDepthCheck(const Conductors& h, double omega)
{
    // The skin depth δ = √(2/(ωµσ)) against the mesh at each conductor's
    // surface - a face between it and what does not conduct -, its prism's
    // size across the face (a wall's: its triangle's height over that edge;
    // a floor's: its layer's): past δ (order 2; δ/2 at order 1) the eddy
    // currents are not resolved there, and the swept mesh does not make
    // itself finer as a 2D mesh adapting would.
    const Field3& f = a_s.fields()[std::size_t(a_fi)];
    const EdgeSpace3& es = *f.edges;
    const Space3& ns = *f.space;
    const Mesh3& mesh = a_s.mesh();
    const Topology3& topo = a_s.topology();
    const Solution3::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    const auto conducting = [&h](int d) { return d >= 0 && d < int(h.conducting.size()) && h.conducting[std::size_t(d)]; };
    PointEvaluator3 ev(a_s);
    double worst = 0, sizeAt = 0, deltaAt = 0;
    int domainAt = -1;
    for (int fi = 0; fi < int(mesh.faces.size()); ++fi) {
        const Mesh3::Face& mf = mesh.faces[std::size_t(fi)];
        const Topology3::Face& tf = topo.faces[std::size_t(mf.face)];
        if (conducting(tf.back) == conducting(tf.front)) continue;
        const int side = conducting(tf.back) ? tf.back : tf.front;
        const std::vector<Space3::FacePoint> points = ns.facePoints(fi, 1, side);
        if (points.empty() || points.front().prism < 0) continue;
        const int e = points.front().prism;
        if (mesh.domain[std::size_t(e)] != side) continue;
        const int t = mesh.prismTriangle[std::size_t(e)], layer = mesh.prismLayer[std::size_t(e)];
        double size = es.z(layer + 1) - es.z(layer);
        if (mf.quad) {
            const Mesh::Edge& pe = mesh.plan->edges[std::size_t(mf.plan)];
            const QPointF d = (mesh.plan->nodes[std::size_t(pe.a)] - mesh.plan->nodes[std::size_t(pe.b)]) * mesh.unitScale;
            const double len = std::hypot(d.x(), d.y());
            if (!(len > 0)) continue;
            size = std::abs(affineMap(es.planNodes(), t).det) / len;
        }
        EdgeSpace3::Basis b;
        es.basis(e, 1.0 / 3, 1.0 / 3, 0.5, b);
        ev.at(e, 1.0 / 3, 1.0 / 3, 0.5, b.x);
        double sigma = 0, mur = 0;
        for (int k : {0, 4, 8}) {
            const Expression& sg = a_sigma[std::size_t(side)][std::size_t(k)];
            const Expression& mu = coef.c[std::size_t(side)][std::size_t(k)];
            if (sg.isValid()) sigma += ev.eval(sg) / 3;
            mur += (mu.isValid() ? ev.eval(mu) : 1.0) / 3;
        }
        if (!(sigma > 0) || !(mur > 0)) continue;
        const double delta = std::sqrt(2 / (omega * Mu0 * mur * sigma));
        if (size / delta > worst) {
            worst = size / delta;
            sizeAt = size;
            deltaAt = delta;
            domainAt = side;
        }
    }
    if (worst > (es.order() == 2 ? 1.0 : 0.5))
        a_s.log << tr("Warning: %1 at %2: the mesh at the surface of domain %3 (%4) is %5 across where its skin depth is %6 - its eddy currents "
                      "are not resolved: make the mesh finer there (a Size on it, or the layers' divisions)")
                       .arg(a_node->name(), formatQuantity(omega / (2 * M_PI), Dim::of(0, 0, -1)))
                       .arg(domainAt + 1)
                       .arg(topo.objectNames.value(topo.domains[std::size_t(domainAt)].owner), formatQuantity(sizeAt, Dim::length()),
                            formatQuantity(deltaAt, Dim::length()));
}

double MagneticProblem3::gaugeScaleOf() const
{
    // The gauge's rows scaled to the curl-curl's: ν0 / h², h the mesh's
    // typical prism (the multiplier is nought where J is free of divergence:
    // only the pivots mind).
    const Field3& f = a_s.fields()[std::size_t(a_fi)];
    const EdgeSpace3& es = *f.edges;
    const Mesh3& mesh = a_s.mesh();
    double volume = 0;
    for (int e = 0; e < es.elements(); ++e) {
        const int d = mesh.domain[std::size_t(e)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        EdgeSpace3::Basis b;
        es.basis(e, 1.0 / 3, 1.0 / 3, 0.5, b);
        volume += 0.5 * b.det;
    }
    const double hTypical = std::cbrt(std::max(volume, 1e-300) / std::max(1, es.elements()));
    return 1 / (Mu0 * hTypical * hTypical);
}

bool MagneticProblem3::solveHarmonic(const Options3& o, QString* error)
{
    Field3& f = field();
    const EdgeSpace3& es = *f.edges;
    const Space3& ns = *f.space;
    const Mesh3& mesh = a_s.mesh();
    const QString name = a_node->name();
    if (a_nonlinear) {
        *error = tr("%1: a B-H curve in the frequency domain - give its domains a relative permeability (Ampère's Law, Relative permeability, "
                    "and µr'' for a core's losses) for a study in frequency")
                     .arg(name);
        return false;
    }
    const double omega = 2 * M_PI * o.frequency;
    const cd jw(0, omega);
    a_omega = omega;
    a_inductance.clear();
    if (!buildSources(error)) return false;
    Conductors h;
    if (!buildConductors(h, omega, error)) return false;
    Equations q;
    if (!buildEquations(q, error)) return false;
    // A coil's terminal and ground on faces whose tangential A is held
    // (Magnetic Insulation, a Magnetic Potential): n × E = 0 on them, the
    // voltage between them the field's integral through that perfect
    // conductor. Elsewhere it is not one value - A + ∇h, U - 1 the same
    // field: the system singular.
    for (int fi = 0; fi < int(mesh.faces.size()); ++fi) {
        if (!h.electrodeFaces[std::size_t(fi)]) continue;
        bool held = true;
        for (int dof : ns.faceDofs(fi)) held = held && (dof < 0 || q.eqP[std::size_t(dof)] == -3);
        if (held) continue;
        const int face = mesh.faces[std::size_t(fi)].face;
        *error = tr("%1: face %2 is a coil's terminal or ground (its currents physics holds it) where A is free - in the frequency domain a "
                    "conductor is fed on faces Magnetic Insulation holds, the model's outer faces (a perfect conductor closing its circuit)")
                     .arg(name)
                     .arg(face + 1);
        return false;
    }
    const double gaugeScale = gaugeScaleOf();
    skinDepthCheck(h, omega);
    const std::size_t nE = std::size_t(es.size()), nN = std::size_t(ns.size());
    std::vector<double> held(nE, 0.0);
    for (std::size_t k = 0; k < nE; ++k)
        if (q.eqA[k] == -3) held[k] = q.fixedA[k];
    const int oV = q.nA + q.nP;
    const std::vector<std::array<Expression, 3>> J = sourcesOf(-1);
    const QString at = tr(" at %1").arg(formatQuantity(o.frequency, Dim::of(0, 0, -1)));
    o.tell(tr("%1: assembling").arg(name));
    QElapsedTimer clock;
    clock.start();
    Eigen::SparseMatrix<cd> M;
    Eigen::VectorXcd r;
    assembleHarmonic(q, h, omega, held, h.heldPsi, J, true, true, M, r, gaugeScale);
    for (int c = 0; c < int(a_coils.size()); ++c)
        if (h.eqU[std::size_t(c)] >= 0 && a_coils[std::size_t(c)].role == Coil::Current) r(oV + h.eqU[std::size_t(c)]) += evalAt(a_coils[std::size_t(c)].current, Vec3{});
    const qint64 assembled = clock.elapsed();
    o.tell(tr("%1: factorizing, %2 unknowns").arg(name).arg(M.rows()));
    SparseComplexLdlt lu;
    lu.compute(M);
    a_s.log << tr("%1%2: assembled in %3 s, %4 nonzeros; factorized in %5 s (%6%7)")
                   .arg(name, at)
                   .arg(assembled / 1000.0, 0, 'f', 2)
                   .arg(M.nonZeros())
                   .arg((clock.elapsed() - assembled) / 1000.0, 0, 'f', 2)
                   .arg(lu.name(), lu.factorBytes() > 0 ? tr("; its factors %1 MB%2")
                                                              .arg(lu.factorBytes() / 1048576.0, 0, 'f', 1)
                                                              .arg(lu.reusedAnalysis() ? tr(", its analysis kept from before") : QString())
                                                        : QString());
    if (lu.info() != Eigen::Success) {
        *error = tr("%1: its system could not be factorized (%2) - is A held somewhere (Magnetic Insulation, a Magnetic Potential)?")
                     .arg(name, QString::fromStdString(lu.lastErrorMessage()));
        return false;
    }
    double residual = 0;
    const auto solveWith = [&lu, &M, &residual](const Eigen::VectorXcd& b) -> Eigen::VectorXcd {
        Eigen::VectorXcd x = lu.solve(b);
        x += lu.solve(b - M * x);
        const double bn = b.norm();
        residual = bn > 0 ? (b - M * x).norm() / bn : 0;
        return x;
    };
    o.tell(tr("%1: solving with its factors").arg(name));
    const Eigen::VectorXcd x = solveWith(r);
    if (!x.allFinite()) {
        *error = tr("%1: its solve is not finite").arg(name);
        return false;
    }
    a_s.log << tr("%1%2: %3 unknowns (%4 of A on edge elements of order %5, %6 of the gauge's multiplier, %7 of the conductors' potential), "
                  "relative residual %8, refined once")
                   .arg(name, at)
                   .arg(M.rows())
                   .arg(q.nA)
                   .arg(es.order())
                   .arg(q.nP)
                   .arg(h.nV)
                   .arg(residual, 0, 'g', 2);
    // A (by edge dof) and V = jω ψ (by node) of a solution x, the held
    // values \a heldA and \a heldPsi.
    const auto unpack = [&](const Eigen::VectorXcd& sol, const std::vector<double>& heldA, const std::vector<cd>& heldPsi, std::vector<cd>& A,
                            std::vector<cd>& V) {
        A.assign(nE, cd(0));
        for (std::size_t k = 0; k < nE; ++k) A[k] = q.eqA[k] >= 0 ? sol(q.eqA[k]) : q.eqA[k] == -3 ? cd(heldA[k]) : cd(0);
        V.assign(nN, cd(0));
        for (std::size_t k = 0; k < nN; ++k) V[k] = jw * (h.eqV[k] >= 0 ? sol(oV + h.eqV[k]) : h.eqV[k] == -3 ? heldPsi[k] : cd(0));
    };
    std::vector<cd> A, V;
    unpack(x, held, h.heldPsi, A, V);
    f.values.assign(2 * nE + 3 * nN, 0.0);
    for (std::size_t k = 0; k < nE; ++k) {
        f.values[k] = A[k].real();
        f.values[nE + nN + k] = A[k].imag();
    }
    for (std::size_t k = 0; k < nN; ++k) {
        f.values[nE + k] = q.eqP[k] >= 0 ? x(q.nA + q.eqP[k]).real() / gaugeScale : 0.0;
        f.values[2 * nE + nN + k] = V[k].real();
        f.values[2 * nE + 2 * nN + k] = V[k].imag();
    }
    f.solved = true;
    // Each single conductor's current: what flows in at its terminal,
    // ∫ σ (jωA + ∇V)·∇h, h one on its terminal's nodes; turns' flux linkage
    // ∫ A·j, j their current density at 1 A.
    const PrismRule& rule = es.order() == 1 ? prismRule(2, 2) : prismRule(4, 3);
    const int nc = int(a_coils.size());
    std::vector<std::vector<std::array<Expression, 3>>> unit(static_cast<std::size_t>(nc));
    for (int c = 0; c < nc; ++c)
        if (!a_coils[std::size_t(c)].single) unit[std::size_t(c)] = sourcesOf(c);
    const auto currents = [&](const std::vector<cd>& Av, const std::vector<cd>& Vv) {
        std::vector<cd> out(static_cast<std::size_t>(nc), cd(0));
        PointEvaluator3 ev(a_s);
        for (int e = 0; e < es.elements(); ++e) {
            const int d = mesh.domain[std::size_t(e)];
            if (d < 0 || !h.conducting[std::size_t(d)]) continue;
            const int* nd = ns.dofs(e);
            int which = -1;
            for (int k = 0; k < ns.perElement() && which < 0; ++k) which = h.terminalOf[std::size_t(nd[k])];
            if (which < 0) continue;
            const int* ed = es.dofs(e);
            const int t = mesh.prismTriangle[std::size_t(e)], layer = mesh.prismLayer[std::size_t(e)];
            const AffineMap am = affineMap(es.planNodes(), t);
            const double hz = es.z(layer + 1) - es.z(layer);
            const std::array<Expression, 9>& sg = a_sigma[std::size_t(d)];
            for (const auto& qp : rule.points) {
                EdgeSpace3::Basis b;
                es.basis(e, qp[0], qp[1], qp[2], b);
                ev.at(e, qp[0], qp[1], qp[2], b.x);
                PrismShape s;
                prismShapeAt(ns.order(), qp[0], qp[1], qp[2], s);
                double g[18][3];
                nodalGradients(s, am, hz, g);
                cd E[3] = {0, 0, 0};   // jωA + ∇V: -E
                for (int i = 0; i < b.count; ++i)
                    if (ed[i] >= 0)
                        for (int k = 0; k < 3; ++k) E[k] += jw * b.v[i][k] * Av[std::size_t(ed[i])];
                for (int k = 0; k < s.count; ++k)
                    for (int c = 0; c < 3; ++c) E[c] += g[k][c] * Vv[std::size_t(nd[k])];
                double s9[9];
                for (int k = 0; k < 9; ++k) {
                    s9[k] = sg[std::size_t(k)].isValid() ? ev.eval(sg[std::size_t(k)]) : 0.0;
                    if (!std::isfinite(s9[k])) s9[k] = 0;
                }
                for (int c = 0; c < nc; ++c) {
                    double gh[3] = {0, 0, 0};
                    bool any = false;
                    for (int k = 0; k < s.count; ++k)
                        if (h.terminalOf[std::size_t(nd[k])] == c) {
                            any = true;
                            for (int m = 0; m < 3; ++m) gh[m] += g[k][m];
                        }
                    if (!any) continue;
                    cd v = 0;
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j) v += gh[i] * s9[3 * i + j] * E[j];
                    out[std::size_t(c)] += qp[3] * b.det * v;
                }
            }
        }
        return out;
    };
    const auto fluxOf = [&](int c, const std::vector<cd>& Av) {
        cd total = 0;
        PointEvaluator3 ev(a_s);
        for (int e = 0; e < es.elements(); ++e) {
            const int d = mesh.domain[std::size_t(e)];
            if (d < 0 || !f.domains[std::size_t(d)]) continue;
            const auto& ju = unit[std::size_t(c)][std::size_t(d)];
            if (!ju[0].isValid() && !ju[1].isValid() && !ju[2].isValid()) continue;
            const int* ed = es.dofs(e);
            for (const auto& qp : rule.points) {
                EdgeSpace3::Basis b;
                es.basis(e, qp[0], qp[1], qp[2], b);
                ev.at(e, qp[0], qp[1], qp[2], b.x);
                cd a[3] = {0, 0, 0};
                for (int i = 0; i < b.count; ++i)
                    if (ed[i] >= 0)
                        for (int k = 0; k < 3; ++k) a[k] += b.v[i][k] * Av[std::size_t(ed[i])];
                for (int k = 0; k < 3; ++k)
                    if (ju[std::size_t(k)].isValid()) total += qp[3] * b.det * a[k] * ev.eval(ju[std::size_t(k)]);
            }
        }
        return total;
    };
    const auto voltageOf = [&](int c, const Eigen::VectorXcd& sol, const std::vector<cd>& Av, const std::vector<cd>& heldPsi) -> cd {
        const Coil& coil = a_coils[std::size_t(c)];
        if (!coil.single) return jw * fluxOf(c, Av);
        if (h.eqU[std::size_t(c)] >= 0) return jw * sol(oV + h.eqU[std::size_t(c)]);
        for (std::size_t k = 0; k < nN; ++k)
            if (h.terminalOf[k] == c) return jw * heldPsi[k];
        return 0;
    };
    a_U.assign(std::size_t(nc), cd(0));
    a_I.assign(std::size_t(nc), cd(0));
    a_flux.assign(std::size_t(nc), cd(0));
    const std::vector<cd> found = currents(A, V);
    f.terminals.clear();
    a_ports.clear();
    for (int c = 0; c < nc; ++c) {
        const Coil& coil = a_coils[std::size_t(c)];
        a_U[std::size_t(c)] = voltageOf(c, x, A, h.heldPsi);
        if (coil.single) a_I[std::size_t(c)] = coil.role == Coil::Current ? cd(evalAt(coil.current, Vec3{})) : coil.role == Coil::Floating ? cd(0) : found[std::size_t(c)];
        else {
            a_I[std::size_t(c)] = evalAt(coil.current, Vec3{});
            a_flux[std::size_t(c)] = fluxOf(c, A);
        }
        const bool port = !coil.single || coil.role == Coil::Current;
        if (port) a_ports.push_back(c);
        Field3::Terminal t;
        t.name = coil.name;
        t.floating = port;
        t.potential = a_U[std::size_t(c)].real();
        t.charge = a_I[std::size_t(c)].real();
        f.terminals.push_back(t);
    }
    // A current held as given: what flows in as solved, said where it is
    // not (to a part in 10⁴: a coil fed where A is not held, its current
    // half lost through the gauge).
    for (int c = 0; c < nc; ++c) {
        const Coil& coil = a_coils[std::size_t(c)];
        if (!coil.single || coil.role != Coil::Current) continue;
        const cd want = a_I[std::size_t(c)], got = found[std::size_t(c)];
        if (std::abs(got - want) > 1e-4 * std::max(std::abs(want), 1e-30))
            a_s.log << tr("%1: coil %2's current as solved, %3 A, is not its %4 A").arg(name, coil.name).arg(std::abs(got), 0, 'g', 6).arg(std::abs(want), 0, 'g', 6);
    }
    // Swept: each port fed 1 A in turn, the others none (a conductor's
    // voltage held at nought), no other source - their impedance matrix.
    a_Z.clear();
    if (o.sweep && !a_ports.empty()) {
        const int np = int(a_ports.size());
        a_Z.assign(std::size_t(np), std::vector<cd>(std::size_t(np), cd(0)));
        const std::vector<double> none(nE, 0.0);
        const std::vector<cd> nonePsi(nN, cd(0));
        for (int jp = 0; jp < np; ++jp) {
            const int c = a_ports[std::size_t(jp)];
            o.tell(tr("%1: the impedance matrix, coil %2 of %3").arg(name).arg(jp + 1).arg(np));
            Eigen::VectorXcd rj;
            if (a_coils[std::size_t(c)].single) {
                rj = Eigen::VectorXcd::Zero(M.rows());
                rj(oV + h.eqU[std::size_t(c)]) = 1.0;
            } else {
                Eigen::SparseMatrix<cd> unused;
                assembleHarmonic(q, h, omega, none, nonePsi, unit[std::size_t(c)], false, false, unused, rj, gaugeScale);
            }
            const Eigen::VectorXcd xj = solveWith(rj);
            std::vector<cd> Aj, Vj;
            unpack(xj, none, nonePsi, Aj, Vj);
            for (int kp = 0; kp < np; ++kp) a_Z[std::size_t(kp)][std::size_t(jp)] = voltageOf(a_ports[std::size_t(kp)], xj, Aj, nonePsi);
        }
    }
    return true;
}

// ---------------------------------------------------------------- Globals

void MagneticProblem3::setHarmonicGlobals()
{
    Solution3& s = a_s;
    Field3& f = field();
    const EdgeSpace3& es = *f.edges;
    const Mesh3& mesh = s.mesh();
    const QString p = f.tag + QLatin1Char('.');
    const Dim henry = Dim::of(2, 1, -2, -2), weber = Dim::of(2, 1, -2, -1);
    a_energy = a_losses = a_magneticLosses = 0;
    const PrismRule& rule = es.order() == 1 ? prismRule(2, 2) : prismRule(4, 3);
    PointEvaluator3 ev(s);
    for (int e = 0; e < es.elements(); ++e) {
        const int d = mesh.domain[std::size_t(e)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        for (const auto& qp : rule.points) {
            EdgeSpace3::Basis b;
            es.basis(e, qp[0], qp[1], qp[2], b);
            ev.at(e, qp[0], qp[1], qp[2], b.x);
            MagneticState st;
            if (!magneticStateAt(ev, a_fi, st, true)) continue;
            const double w = qp[3] * b.det;
            if (std::isfinite(st.energy)) a_energy += w * st.energy;
            if (std::isfinite(st.q)) a_losses += w * st.q;
            if (std::isfinite(st.qm)) a_magneticLosses += w * st.qm;
        }
    }
    s.setGlobal(p + QStringLiteral("W"), a_energy, Dim::of(2, 1, -2), tr("magnetic energy, its time average"));
    s.setGlobal(p + QStringLiteral("P"), a_losses, Dim::power(), tr("resistive losses, their time average"));
    s.setGlobal(p + QStringLiteral("Pm"), a_magneticLosses, Dim::power(), tr("magnetic losses (a core's, of its µr''), their time average"));
    for (int c = 0; c < int(a_coils.size()) && c < int(a_U.size()); ++c) {
        const Coil& coil = a_coils[std::size_t(c)];
        const QString n = coil.name;
        const cd I = a_I[std::size_t(c)], U = a_U[std::size_t(c)];
        const bool found = coil.single && coil.role != Coil::Current;
        s.setGlobal(p + QStringLiteral("I_") + n, I.real(), Dim::current(),
                    found ? tr("coil %1's current, found (its role holds its voltage), its real part").arg(n) : tr("coil %1's current").arg(n));
        if (found) s.setGlobal(p + QStringLiteral("I_") + n + QStringLiteral("_im"), I.imag(), Dim::current(), tr("coil %1's current, its imaginary part").arg(n));
        s.setGlobal(p + QStringLiteral("V_") + n, U.real(), Dim::voltage(), tr("coil %1's voltage, its real part").arg(n));
        s.setGlobal(p + QStringLiteral("V_") + n + QStringLiteral("_im"), U.imag(), Dim::voltage(), tr("coil %1's voltage, its imaginary part").arg(n));
        if (!coil.single) {
            s.setGlobal(p + QStringLiteral("Phi_") + n, a_flux[std::size_t(c)].real(), weber, tr("coil %1's flux linkage, its real part").arg(n));
            s.setGlobal(p + QStringLiteral("Phi_") + n + QStringLiteral("_im"), a_flux[std::size_t(c)].imag(), weber,
                        tr("coil %1's flux linkage, its imaginary part").arg(n));
        }
        if (std::abs(I) > 0) {
            const cd z = U / I;
            s.setGlobal(p + QStringLiteral("R_") + n, z.real(), Dim::resistance(), tr("coil %1's resistance, V/I's real part").arg(n));
            s.setGlobal(p + QStringLiteral("L_") + n, z.imag() / a_omega, henry, tr("coil %1's inductance, V/I's imaginary part over ω").arg(n));
            s.setGlobal(p + QStringLiteral("Z_") + n, std::abs(z), Dim::resistance(), tr("coil %1's impedance, |V/I|").arg(n));
            if (z.real() != 0) s.setGlobal(p + QStringLiteral("Q_") + n, z.imag() / z.real(), Dim(), tr("coil %1's quality factor, ωL/R").arg(n));
        }
    }
    const int np = int(a_Z.size());
    for (int i = 0; i < np; ++i)
        for (int j = 0; j < np; ++j) {
            const QString a = a_coils[std::size_t(a_ports[std::size_t(i)])].name, b = a_coils[std::size_t(a_ports[std::size_t(j)])].name;
            const cd z = a_Z[std::size_t(i)][std::size_t(j)];
            s.setGlobal(p + QStringLiteral("L_") + a + QLatin1Char('_') + b, z.imag() / a_omega, henry, tr("inductance matrix, %1 %2").arg(a, b));
            s.setGlobal(p + QStringLiteral("R_") + a + QLatin1Char('_') + b, z.real(), Dim::resistance(), tr("resistance matrix, %1 %2").arg(a, b));
            if (a.size() == 1 && b.size() == 1) {
                s.setGlobal(p + QStringLiteral("L") + a + b, z.imag() / a_omega, henry, tr("inductance matrix, %1 %2").arg(a, b));
                s.setGlobal(p + QStringLiteral("R") + a + b, z.real(), Dim::resistance(), tr("resistance matrix, %1 %2").arg(a, b));
            }
        }
}

void MagneticProblem3::setGlobals()
{
    if (a_omega > 0) {
        setHarmonicGlobals();
        return;
    }
    Solution3& s = a_s;
    Field3& f = field();
    const EdgeSpace3& es = *f.edges;
    const Mesh3& mesh = s.mesh();
    const QString p = f.tag + QLatin1Char('.');
    const Solution3::Coefficients& coef = s.coefficients()[std::size_t(a_fi)];
    const Dim henry = Dim::of(2, 1, -2, -2), weber = Dim::of(2, 1, -2, -1), force = Dim::of(1, 1, -2);
    // The energy, ∫ Wm; each coil's flux linkage ∫ A·J / I0 and Lorentz force
    // ∫ J × B.
    a_energy = 0;
    for (Coil& c : a_coils) {
        c.flux = 0;
        c.force[0] = c.force[1] = c.force[2] = 0;
    }
    const PrismRule& rule = es.order() == 1 ? prismRule(2, 2) : prismRule(4, 3);
    PointEvaluator3 ev(s);
    const std::vector<std::array<Expression, 3>> J = sourcesOf(-1);
    std::vector<std::vector<std::array<Expression, 3>>> unit;
    for (int c = 0; c < int(a_coils.size()); ++c) unit.push_back(sourcesOf(c));
    for (int e = 0; e < es.elements(); ++e) {
        const int d = mesh.domain[std::size_t(e)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        for (const auto& qp : rule.points) {
            EdgeSpace3::Basis b;
            es.basis(e, qp[0], qp[1], qp[2], b);
            ev.at(e, qp[0], qp[1], qp[2], b.x);
            const double w = qp[3] * b.det;
            MagneticState st;
            if (!magneticStateAt(ev, a_fi, st)) continue;
            if (std::isfinite(st.energy)) a_energy += w * st.energy;
            for (int c = 0; c < int(a_coils.size()); ++c) {
                const auto& ju = unit[std::size_t(c)][std::size_t(d)];
                if (!ju[0].isValid() && !ju[1].isValid() && !ju[2].isValid()) continue;
                double jv[3];
                for (int k = 0; k < 3; ++k) jv[k] = ju[std::size_t(k)].isValid() ? ev.eval(ju[std::size_t(k)]) : 0.0;
                Coil& coil = a_coils[std::size_t(c)];
                coil.flux += w * (st.A[0] * jv[0] + st.A[1] * jv[1] + st.A[2] * jv[2]);
                const double i0 = evalAt(coil.current, Vec3{});
                const double j[3] = {jv[0] * i0, jv[1] * i0, jv[2] * i0};
                coil.force[0] += w * (j[1] * st.B[2] - j[2] * st.B[1]);
                coil.force[1] += w * (j[2] * st.B[0] - j[0] * st.B[2]);
                coil.force[2] += w * (j[0] * st.B[1] - j[1] * st.B[0]);
            }
        }
    }
    (void)coef;
    (void)J;
    s.setGlobal(p + QStringLiteral("W"), a_energy, Dim::of(2, 1, -2), tr("magnetic energy"));
    for (const Coil& c : a_coils) {
        const double i0 = evalAt(c.current, Vec3{});
        s.setGlobal(p + QStringLiteral("I_") + c.name, i0, Dim::current(), tr("coil %1's current").arg(c.name));
        s.setGlobal(p + QStringLiteral("Phi_") + c.name, c.flux, weber, tr("coil %1's flux linkage").arg(c.name));
        if (i0 != 0) s.setGlobal(p + QStringLiteral("L_") + c.name, c.flux / i0, henry, tr("coil %1's inductance: its flux linkage over its current").arg(c.name));
        static const char* const axes[3] = {"x", "y", "z"};
        for (int k = 0; k < 3; ++k)
            s.setGlobal(p + QStringLiteral("F") + QString::fromLatin1(axes[k]) + QLatin1Char('_') + c.name, c.force[k], force,
                        tr("the Lorentz force on coil %1, %2 component").arg(c.name, QString::fromLatin1(axes[k])));
    }
    const int nc = int(a_inductance.size());
    for (int i = 0; i < nc; ++i)
        for (int j = 0; j < nc; ++j) {
            const QString a = a_coils[std::size_t(i)].name, b = a_coils[std::size_t(j)].name;
            const double l = a_inductance[std::size_t(i)][std::size_t(j)];
            s.setGlobal(p + QStringLiteral("L_") + a + QLatin1Char('_') + b, l, henry, tr("inductance matrix, %1 %2").arg(a, b));
            if (a.size() == 1 && b.size() == 1) s.setGlobal(p + QStringLiteral("L") + a + b, l, henry, tr("inductance matrix, %1 %2").arg(a, b));
        }
}

} // namespace

std::unique_ptr<Problem3> makeMagneticProblem3(Solution3& solution, int field)
{
    return std::make_unique<MagneticProblem3>(solution, field);
}

bool magneticStateAt(PointEvaluator3& ev, int f, MagneticState& out, bool currents)
{
    const Solution3& s = ev.solution();
    if (f < 0 || f >= int(s.fields().size()) || ev.prism() < 0) return false;
    const Field3& field = s.fields()[std::size_t(f)];
    const int d = ev.domain();
    if (!field.edges || d < 0 || d >= int(field.domains.size()) || !field.domains[std::size_t(d)]) return false;
    const EdgeSpace3& es = *field.edges;
    if (field.values.size() < std::size_t(es.size())) return false;
    EdgeSpace3::Basis b;
    es.basis(ev.prism(), ev.xi(), ev.eta(), ev.zeta(), b);
    const int* ed = es.dofs(ev.prism());
    for (int k = 0; k < 3; ++k) out.A[k] = out.B[k] = 0;
    for (int i = 0; i < b.count; ++i) {
        if (ed[i] < 0) continue;
        const double a = field.values[std::size_t(ed[i])];
        for (int k = 0; k < 3; ++k) {
            out.A[k] += b.v[i][k] * a;
            out.B[k] += b.c[i][k] * a;
        }
    }
    const Solution3::Coefficients& coef = s.coefficients()[std::size_t(f)];
    // The sources' current density (a coil's of turns, an External Current
    // Density's).
    for (int k = 0; k < 3 && currents; ++k) {
        static const char* const keys[3] = {"Jx", "Jy", "Jz"};
        const auto it = coef.named.find(QString::fromLatin1(keys[k]));
        out.J[k] = out.Ji[k] = 0;
        if (it != coef.named.end() && d < int(it->second.size()) && it->second[std::size_t(d)].isValid()) {
            out.J[k] = ev.eval(it->second[std::size_t(d)]);
            if (!std::isfinite(out.J[k])) out.J[k] = 0;
        }
    }
    // Solved in the frequency domain: A's imaginary part after the
    // multiplier, then V's real and imaginary parts by node (MagneticProblem3
    // ::solveHarmonic()).
    const std::size_t nE = std::size_t(es.size()), nN = field.space ? std::size_t(field.space->size()) : 0;
    const double omega = 2 * M_PI * s.frequency();
    out.harmonic = omega > 0 && nN > 0 && field.values.size() == 2 * nE + 3 * nN;
    if (out.harmonic) {
        using cd = std::complex<double>;
        for (int k = 0; k < 3; ++k) out.Ai[k] = out.Bi[k] = 0;
        for (int i = 0; i < b.count; ++i) {
            if (ed[i] < 0) continue;
            const double a = field.values[nE + nN + std::size_t(ed[i])];
            for (int k = 0; k < 3; ++k) {
                out.Ai[k] += b.v[i][k] * a;
                out.Bi[k] += b.c[i][k] * a;
            }
        }
        double mur[9];
        const std::array<Expression, 9>& c = coef.c[std::size_t(d)];
        for (int k = 0; k < 9; ++k) mur[k] = c[std::size_t(k)].isValid() ? ev.eval(c[std::size_t(k)]) : 0;
        double murIm = 0;
        if (const auto it = coef.named.find(QStringLiteral("murIm")); it != coef.named.end() && d < int(it->second.size()) && it->second[std::size_t(d)].isValid()) {
            murIm = ev.eval(it->second[std::size_t(d)]);
            if (!std::isfinite(murIm)) murIm = 0;
        }
        cd nu[9];
        if (!complexReluctivity(mur, murIm, nu)) return false;
        out.energy = out.qm = 0;
        for (int i = 0; i < 3; ++i) {
            cd H = 0;
            for (int j = 0; j < 3; ++j) H += nu[3 * i + j] * cd(out.B[j], out.Bi[j]);
            out.H[i] = H.real();
            out.Hi[i] = H.imag();
            // (¼ Re(B·H*), ½ Re(jω B·H*).)
            out.energy += 0.25 * (out.B[i] * out.H[i] + out.Bi[i] * out.Hi[i]);
            out.qm += 0.5 * omega * (out.B[i] * out.Hi[i] - out.Bi[i] * out.H[i]);
        }
        out.mur = (mur[0] + mur[4] + mur[8]) / 3;
        // In a conductor: E = -jωA - ∇V, J = σE.
        double s9[9];
        bool conducting = false;
        for (int k = 0; k < 9; ++k) {
            s9[k] = 0;
            const auto it = coef.named.find(QStringLiteral("sigma%1").arg(k));
            if (it != coef.named.end() && d < int(it->second.size()) && it->second[std::size_t(d)].isValid()) {
                s9[k] = ev.eval(it->second[std::size_t(d)]);
                if (!std::isfinite(s9[k])) s9[k] = 0;
                conducting = true;
            }
        }
        out.sigma = (s9[0] + s9[4] + s9[8]) / 3;
        for (int k = 0; k < 3; ++k) out.E[k] = out.Ei[k] = 0;
        out.q = 0;
        if (conducting && currents) {
            const Mesh3& mesh = s.mesh();
            const int e = ev.prism();
            const Space3& ns = *field.space;
            const int t = mesh.prismTriangle[std::size_t(e)], layer = mesh.prismLayer[std::size_t(e)];
            const AffineMap am = affineMap(es.planNodes(), t);
            PrismShape sh;
            prismShapeAt(ns.order(), ev.xi(), ev.eta(), ev.zeta(), sh);
            double g[18][3];
            nodalGradients(sh, am, es.z(layer + 1) - es.z(layer), g);
            const int* nd = ns.dofs(e);
            double gr[3] = {0, 0, 0}, gi[3] = {0, 0, 0};
            for (int k = 0; k < sh.count; ++k) {
                const double vr = field.values[2 * nE + nN + std::size_t(nd[k])], vi = field.values[2 * nE + 2 * nN + std::size_t(nd[k])];
                for (int m = 0; m < 3; ++m) {
                    gr[m] += g[k][m] * vr;
                    gi[m] += g[k][m] * vi;
                }
            }
            for (int k = 0; k < sh.count; ++k) {
                out.V += sh.n[k] * field.values[2 * nE + nN + std::size_t(nd[k])];
                out.Vi += sh.n[k] * field.values[2 * nE + 2 * nN + std::size_t(nd[k])];
            }
            for (int k = 0; k < 3; ++k) {
                out.E[k] = omega * out.Ai[k] - gr[k];
                out.Ei[k] = -omega * out.A[k] - gi[k];
            }
            for (int i = 0; i < 3; ++i) {
                double jr = 0, ji = 0;
                for (int j = 0; j < 3; ++j) {
                    jr += s9[3 * i + j] * out.E[j];
                    ji += s9[3 * i + j] * out.Ei[j];
                }
                out.J[i] += jr;
                out.Ji[i] += ji;
                out.q += 0.5 * (out.E[i] * jr + out.Ei[i] * ji);
            }
        }
        return true;
    }
    const std::shared_ptr<const BHCurve>& curve = d < int(coef.curves.size()) ? coef.curves[std::size_t(d)] : nullptr;
    const double bn = std::sqrt(out.B[0] * out.B[0] + out.B[1] * out.B[1] + out.B[2] * out.B[2]);
    if (curve) {
        const double nu = curve->nu(bn);
        for (int k = 0; k < 3; ++k) out.H[k] = nu * out.B[k];
        out.energy = curve->energy(bn);
        out.mur = bn > 0 ? 1 / (Mu0 * nu) : 1 / (Mu0 * curve->nu(0));
        return true;
    }
    double mur[9], nu[9];
    const std::array<Expression, 9>& c = coef.c[std::size_t(d)];
    for (int k = 0; k < 9; ++k) mur[k] = c[std::size_t(k)].isValid() ? ev.eval(c[std::size_t(k)]) : 0;
    if (!reluctivity(mur, nu)) return false;
    double Br[3] = {0, 0, 0};
    static const char* const keys[3] = {"Brx", "Bry", "Brz"};
    for (int k = 0; k < 3; ++k) {
        const auto it = coef.named.find(QString::fromLatin1(keys[k]));
        if (it != coef.named.end() && d < int(it->second.size()) && it->second[std::size_t(d)].isValid()) {
            Br[k] = ev.eval(it->second[std::size_t(d)]);
            if (!std::isfinite(Br[k])) Br[k] = 0;
        }
    }
    out.energy = 0;
    for (int i = 0; i < 3; ++i) {
        out.H[i] = 0;
        for (int j = 0; j < 3; ++j) out.H[i] += nu[3 * i + j] * (out.B[j] - Br[j]);
        out.energy += 0.5 * out.H[i] * (out.B[i] - Br[i]);
    }
    out.mur = (mur[0] + mur[4] + mur[8]) / 3;
    double diagonal = 0;
    for (int k : {0, 4, 8}) {
        const auto it = coef.named.find(QStringLiteral("sigma%1").arg(k));
        if (it != coef.named.end() && d < int(it->second.size()) && it->second[std::size_t(d)].isValid()) {
            const double v = ev.eval(it->second[std::size_t(d)]);
            if (std::isfinite(v)) diagonal += v;
        }
    }
    out.sigma = diagonal / 3;
    return true;
}

} // namespace qucs_s::fem::detail3
