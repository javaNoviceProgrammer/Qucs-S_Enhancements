/*
 * test_fem_anisotropy.cpp - anisotropic materials against what is known:
 *                           tensors as texts, a crystal's orientation and
 *                           Bond's rotation; electrostatics, currents and
 *                           heat with a tensor (a parallel-plate capacitor
 *                           along each crystal axis, a turned crystal
 *                           against the PDE of its components), a
 *                           dielectric's losses along its axes; solids,
 *                           magnetic cores, piezoelectric plates; modes
 *                           of turned, electro-optic, strained and
 *                           magnetized crystals against their layers'
 *                           transfer matrices; conducting sheets and
 *                           graphene; the library's crystals; a z-cut
 *                           modulator against its x-cut turned
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_materials.h"
#include "fem_optics.h"
#include "fem_problem.h"
#include "fem_results.h"
#include "fem_tensor.h"

#include <QtTest>

#include <Eigen/Dense>

#include <cmath>
#include <complex>
#include <random>

using namespace qucs_s::fem;

namespace {

constexpr double Eps0 = 8.8541878188e-12;
constexpr double C0 = 299792458.0;
using cd = std::complex<double>;
const cd J(0, 1);

bool slowBuild()
{
    if (qEnvironmentVariableIsSet("QUCS_TEST_SLOW")) return true;
#if defined(__SANITIZE_ADDRESS__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
    return true;
#endif
#endif
    return false;
}

Model modelOf(const QByteArray& json)
{
    QString why;
    std::optional<Model> m = Model::fromJson(json, &why);
    if (!m) qFatal("not a model: %s", qPrintable(why));
    if (!why.isEmpty()) qWarning("%s", qPrintable(why));
    return *m;
}

struct Run {
    std::shared_ptr<ParameterScope> parameters;
    std::shared_ptr<Solution> solution;
    StudyRun study;
    QString error;
};

Run run(const Model& model)
{
    Run r;
    r.parameters = evaluateParameters(model);
    const GeometryBuild g = buildGeometry(model, *r.parameters);
    if (!g.ok()) {
        r.error = QStringLiteral("geometry: ") + QStringList(g.errors.values()).join(QLatin1String("; "));
        return r;
    }
    r.study = runStudy(model, r.parameters, g.topology, nullptr, QString());
    if (!r.study.ok()) {
        r.error = r.study.error;
        return r;
    }
    r.solution = r.study.solutions.back();
    return r;
}

double at(Solution& s, const QString& expression, QPointF p)
{
    const Locator locator(s.mesh());
    QString why;
    const std::optional<double> v = evaluateAt(s, locator, expression, p, &why);
    if (!v) qWarning("%s at (%g, %g): %s", qPrintable(expression), p.x(), p.y(), qPrintable(why));
    return v ? *v : NAN;
}

double global(Solution& s, const QString& name)
{
    const std::optional<double> v = s.global(name);
    return v ? *v : NAN;
}

/// A text's value, a constant of its unit.
double value(const QString& text)
{
    const std::optional<double> v = evaluateConstant(text, Scope::builtins(), Dim());
    return v ? *v : NAN;
}

/// A text's value at the wavelength \a lambda (m), whatever its unit.
double valueAt(const QString& text, double lambda = 1.55e-6)
{
    MaterialScope scope(Scope::builtins(), 300);
    scope.setConstant(QStringLiteral("lambda0"), lambda, Dim::length());
    const Expression x = Expression::compile(text, scope);
    return x.isValid() && x.isConstant() ? x.constant() : NAN;
}

/// A library entry's material node, as the panel makes it.
Node libraryNode(const QString& name)
{
    const std::optional<MaterialEntry> e = findMaterial(name);
    if (!e) qFatal("no %s in the library", qPrintable(name));
    return materialNode(Model::blank(), *e);
}

/// The numbers R T Rᵀ.
void rotate(const Rotation& r, const double in[3][3], double out[3][3])
{
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            out[i][j] = 0;
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b) out[i][j] += r.r[i][a] * r.r[j][b] * in[a][b];
        }
}

/// A plate capacitor 10 mm wide and 1 mm across (y), its plates the top
/// (1 V) and the bottom (0 V), of material \a material.
QByteArray plates(const QString& material)
{
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "components": [{"unit": "mm",
        "geometry": [{"type": "rectangle", "name": "gap", "size": ["10", "1"]}],
        "materials": [{"label": "crystal", %1, "selection": {"all": true}}],
        "physics": [{"type": "electrostatics", "tag": "es", "features": [
           {"type": "terminal", "name": "1", "V0": "1[V]", "selection": {"of": ["gap"], "side": "top"}},
           {"type": "ground", "selection": {"of": ["gap"], "side": "bottom"}}]}],
        "mesh": {"size": "coarse"}}],
      "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json")
        .arg(material)
        .toUtf8();
}

/// A square of 1 mm with an electrode off its middle (1 V) and its left and
/// bottom sides at 0: \a physics's model of \a material - or a PDE of the
/// coefficients \a pde (cxx, cxy, cyx, cyy), the same mesh.
QByteArray square(const QString& physics, const QString& material, const QString& pde = {})
{
    const bool isPde = !pde.isEmpty();
    const QString fixed = isPde ? QStringLiteral(R"x({"type": "dirichlet", "r": "1", "selection": {"of": ["pin"]}},
                                                     {"type": "dirichlet", "r": "0", "selection": {"of": ["box"], "side": "left"}},
                                                     {"type": "dirichlet", "r": "0", "selection": {"of": ["box"], "side": "bottom"}})x")
                                : physics == QLatin1String("heat")
                                    ? QStringLiteral(R"x({"type": "temperature", "T0": "1[K]", "selection": {"of": ["pin"]}},
                                                         {"type": "temperature", "T0": "0[K]", "selection": {"of": ["box"], "side": "left"}},
                                                         {"type": "temperature", "T0": "0[K]", "selection": {"of": ["box"], "side": "bottom"}})x")
                                    : QStringLiteral(R"x({"type": "potential", "V0": "1[V]", "selection": {"of": ["pin"]}},
                                                         {"type": "ground", "selection": {"of": ["box"], "side": "left"}},
                                                         {"type": "ground", "selection": {"of": ["box"], "side": "bottom"}})x");
    const QString physicsJson = isPde ? QStringLiteral(R"x({"type": "pde", "tag": "c", "selection": {"only": ["rest"]}, "features": [
                                                            {"type": "coefficient", "diffusion": "anisotropic", %1, "f": "0", "selection": {"all": true}}, %2]})x")
                                            .arg(pde, fixed)
                                      : QStringLiteral(R"x({"type": "%1", "tag": "p", "selection": {"only": ["rest"]}, "features": [%2]})x").arg(physics, fixed);
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "components": [{"unit": "mm",
        "geometry": [{"type": "rectangle", "name": "box", "size": ["1", "1"]},
                     {"type": "circle", "name": "pin", "center": ["0.62", "0.55"], "radius": "0.1"},
                     {"type": "difference", "name": "rest", "input": ["box"], "tools": ["pin"], "keep": true}],
        "materials": [{"label": "crystal", %1, "selection": {"all": true}}],
        "physics": [%2],
        "mesh": {"size": "normal"}}],
      "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json")
        .arg(material, physicsJson)
        .toUtf8();
}

/// A TM slab's guided mode, its layers along y - a bottom cladding, a core
/// \a d thick, a top cladding - each of η = ε⁻¹'s yz block (yy, yz, zy, zz;
/// Hx alone, its equation ηzz H'' + jβ(ηzy + ηyz) H' + (k0² - β² ηyy) H = 0,
/// H and ηzz H' + jβ ηzy H continuous): the β nearest \a guess (a secant).
cd slabTM(const std::array<cd, 4> eta[3], double d, double k0, cd guess)
{
    auto roots = [&](const std::array<cd, 4>& e, cd beta, cd* s1, cd* s2) {
        const cd a = e[3], b = J * beta * (e[1] + e[2]), c = k0 * k0 - beta * beta * e[0];
        const cd q = std::sqrt(b * b - 4.0 * a * c);
        *s1 = (-b + q) / (2.0 * a);
        *s2 = (-b - q) / (2.0 * a);
    };
    auto f = [&roots, eta, d](cd beta) {
        cd s1, s2;
        // Below: decaying downward (Re s > 0).
        roots(eta[0], beta, &s1, &s2);
        const cd sb = s1.real() > 0 ? s1 : s2;
        const cd H0 = 1.0, P0 = eta[0][3] * sb + J * beta * eta[0][2];
        // The core: H = A e^{s1 y} + B e^{s2 y}.
        roots(eta[1], beta, &s1, &s2);
        const cd p1 = eta[1][3] * s1 + J * beta * eta[1][2], p2 = eta[1][3] * s2 + J * beta * eta[1][2];
        const cd A = (P0 - p2 * H0) / (p1 - p2), B = (p1 * H0 - P0) / (p1 - p2);
        const cd Hd = A * std::exp(s1 * d) + B * std::exp(s2 * d), Pd = A * p1 * std::exp(s1 * d) + B * p2 * std::exp(s2 * d);
        // Above: decaying upward (Re s < 0).
        roots(eta[2], beta, &s1, &s2);
        const cd st = s1.real() < 0 ? s1 : s2;
        return Pd - (eta[2][3] * st + J * beta * eta[2][2]) * Hd;
    };
    cd b0 = guess, b1 = guess * (1.0 + 1e-6);
    cd f0 = f(b0), f1 = f(b1);
    for (int i = 0; i < 60 && std::abs(b1 - b0) > 1e-15 * std::abs(b1); ++i) {
        const cd b2 = b1 - f1 * (b1 - b0) / (f1 - f0);
        b0 = b1;
        f0 = f1;
        b1 = b2;
        f1 = f(b1);
    }
    return b1;
}

/// η of an isotropic index n.
std::array<cd, 4> isotropicEta(double n)
{
    return {1 / (n * n), 0, 0, 1 / (n * n)};
}

/// η of a permittivity's yz block (yy, yz, zy, zz).
std::array<cd, 4> etaOf(cd yy, cd yz, cd zy, cd zz)
{
    const cd det = yy * zz - yz * zy;
    return {zz / det, -yz / det, -zy / det, yy / det};
}

/// A slab \a d µm thick of a core's material (its JSON) between an oxide
/// below and \a above (its index) above, 3 µm each (a weakly guided mode's
/// field gone at the metal walls there), across a window of magnetic walls:
/// its TM modes at 1550 nm about \a neff - the core's wave feature's more
/// \a wave.
QByteArray tmSlab(const QString& core, double d, const QString& above, const QString& wave, const QString& neff)
{
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "components": [{"unit": "um",
        "geometry": [{"type": "rectangle", "name": "below", "position": ["0", "-3"], "size": ["0.1", "3"]},
                     {"type": "rectangle", "name": "core", "size": ["0.1", "%2"]},
                     {"type": "rectangle", "name": "above", "position": ["0", "%2"], "size": ["0.1", "3"]}],
        "materials": [{"label": "oxide", "n": "1.444", "selection": {"only": ["below"]}},
                      {"label": "cover", "n": "%3", "selection": {"only": ["above"]}},
                      {"label": "core", %1, "selection": {"only": ["core"]}}],
        "physics": [{"type": "modes", "tag": "emw", "features": [
            {"type": "wave", "selection": {"only": ["core"]} %4},
            {"type": "pmc", "selection": {"of": ["below", "core", "above"], "side": "left"}},
            {"type": "pmc", "selection": {"of": ["below", "core", "above"], "side": "right"}}]}],
        "mesh": {"size": "fine", "features": [{"type": "size", "level": "domain", "selection": {"only": ["core"]}, "hmax": "%6"},
                                              {"type": "size", "level": "domain", "selection": {"only": ["below", "above"]}, "hmax": "%7"}]}}],
      "studies": [{"type": "study", "steps": [{"type": "mode", "frequency": "1550[nm]", "modes": 1, "neff": "%5", "polarization": "tm"}]}]})json")
        .arg(core, QString::number(d), above, wave, neff,
             slowBuild() ? QStringLiteral("0.04") : QStringLiteral("0.02"),
             slowBuild() ? QStringLiteral("0.2") : QStringLiteral("0.06"))
        .toUtf8();
}

} // namespace

class TestFemAnisotropy : public QObject
{
    Q_OBJECT

private slots:
    void tensorTexts()
    {
        // One, three, six, nine items; anything else said.
        const std::optional<TensorText> one = tensorText(QStringLiteral("28"));
        QVERIFY(one && one->items == 1 && one->c[2][2] == QLatin1String("28") && one->c[0][1] == QLatin1String("0"));
        const std::optional<TensorText> three = tensorText(QStringLiteral("44; 44; 28"));
        QVERIFY(three && three->items == 3 && three->c[0][0] == QLatin1String("44") && three->c[2][2] == QLatin1String("28"));
        const std::optional<TensorText> six = tensorText(QStringLiteral("1; 2; 3; 4; 5; 6"));
        QVERIFY(six && six->c[1][2] == QLatin1String("4") && six->c[2][1] == QLatin1String("4") && six->c[0][2] == QLatin1String("5")
                && six->c[0][1] == QLatin1String("6") && six->c[1][0] == QLatin1String("6"));
        const std::optional<TensorText> nine = tensorText(QStringLiteral("1;2;3;4;5;6;7;8;9"));
        QVERIFY(nine && nine->c[0][1] == QLatin1String("2") && nine->c[1][0] == QLatin1String("4") && nine->c[2][1] == QLatin1String("8"));
        QString why;
        QVERIFY(!tensorText(QStringLiteral("1; 2"), &why));
        QVERIFY2(why.contains(QLatin1String("2 values")), qPrintable(why));
        QVERIFY(!tensorText(QStringLiteral("1; 2; 3; 4; 5; 6"), &why, {1, 3}));
        // ';' inside brackets or parentheses is no split.
        QCOMPARE(tensorItems(QStringLiteral("max(1, 2); 3[m]")).size(), 2);
        QCOMPARE(isotropicText(QStringLiteral("44; 44; 28")), QStringLiteral("((44) + (44) + (28))/3"));
        QCOMPARE(isotropicText(QStringLiteral("2.2")), QStringLiteral("2.2"));
        QCOMPARE(isotropicText(QStringLiteral("5; 5; 5")), QStringLiteral("5"));

        // Voigt's entries: named, the rest nought, a symmetric one mirrored.
        const std::optional<VoigtText> c = voigtText(QStringLiteral("C11=198[GPa]; C12=55[GPa]; C44=60[GPa]"), QLatin1Char('C'), 6, 6, true);
        QVERIFY(c && c->any);
        QCOMPARE(c->c[1][0], QStringLiteral("55[GPa]"));
        QCOMPARE(c->c[3][3], QStringLiteral("60[GPa]"));
        QCOMPARE(c->c[2][2], QStringLiteral("0"));
        const std::optional<VoigtText> r = voigtText(QStringLiteral("r13=9.6[pm/V]; r22=6.8[pm/V]"), QLatin1Char('r'), 6, 3, false);
        QVERIFY(r && r->c[0][2] == QLatin1String("9.6[pm/V]") && r->c[2][0] == QLatin1String("0"));
        QVERIFY(!voigtText(QStringLiteral("r71=1"), QLatin1Char('r'), 6, 3, false, &why));
        QVERIFY(!voigtText(QStringLiteral("e15"), QLatin1Char('e'), 3, 6, false, &why));
        QVERIFY(!voigtText(QStringLiteral("C11=1; d12=2"), QLatin1Char('C'), 6, 6, true, &why));
    }

    void orientations()
    {
        // A cut: the crystal's axis up along y, the other along z, the
        // third along ±x - always right-handed.
        for (int cut = 0; cut < 3; ++cut)
            for (int along = 0; along < 3; ++along) {
                const std::optional<Rotation> r = cutRotation(cut, along);
                if (cut == along) {
                    QVERIFY(!r);
                    continue;
                }
                QVERIFY(r);
                QCOMPARE(r->r[1][cut], 1.0);
                QCOMPARE(r->r[2][along], 1.0);
                const auto& m = r->r;
                const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
                                   + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
                QCOMPARE(det, 1.0);
            }
        // X-cut, y-propagating lithium niobate: its optic axis z across, +x.
        const Rotation xcut = *cutRotation(0, 1);
        const double z[3] = {0, 0, 1};
        double model[3];
        xcut.toModel(z, model);
        QCOMPARE(model[0], 1.0);
        double back[3];
        xcut.toCrystal(model, back);
        QCOMPARE(back[2], 1.0);
        // Euler's z-x-z: φ alone turns x toward y.
        const Rotation e = eulerRotation(M_PI / 2, 0, 0);
        const double x[3] = {1, 0, 0};
        e.toModel(x, model);
        QVERIFY(std::abs(model[1] - 1) < 1e-15 && std::abs(model[0]) < 1e-15);
        // θ about the new x: z toward -y.
        eulerRotation(0, M_PI / 2, 0).toModel(z, model);
        QVERIFY(std::abs(model[1] + 1) < 1e-15);

        // A material node's.
        Node m;
        m.type = QStringLiteral("material");
        m.props.insert(QStringLiteral("orientation"), QStringLiteral("cut"));
        m.props.insert(QStringLiteral("cut"), QStringLiteral("z"));
        m.props.insert(QStringLiteral("along"), QStringLiteral("x"));
        const std::optional<Rotation> zcut = materialRotation(m, Scope::builtins());
        QVERIFY(zcut);
        QCOMPARE(zcut->r[1][2], 1.0);
        m.props.insert(QStringLiteral("orientation"), QStringLiteral("euler"));
        m.props.insert(QStringLiteral("euler_phi"), QStringLiteral("30"));
        m.props.insert(QStringLiteral("euler_theta"), QStringLiteral("0.5[rad]"));
        const std::optional<Rotation> euler = materialRotation(m, Scope::builtins());
        QVERIFY(euler);
        const Rotation expected = eulerRotation(M_PI / 6, 0.5, 0);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) QVERIFY(std::abs(euler->r[i][j] - expected.r[i][j]) < 1e-15);
        m.props.insert(QStringLiteral("orientation"), QStringLiteral("sideways"));
        QString why;
        QVERIFY(!materialRotation(m, Scope::builtins(), &why));
        QVERIFY(why.contains(QLatin1String("sideways")));
    }

    void rotations()
    {
        // A second-rank tensor's texts turned: what they evaluate to, R T Rᵀ.
        const Rotation r = eulerRotation(0.3, 1.1, -0.7);
        const TensorText t = *tensorText(QStringLiteral("2; 3; 5; 0.25; -0.5; 0.75"));
        const TensorText turned = rotated(t, r);
        const double in[3][3] = {{2, 0.75, -0.5}, {0.75, 3, 0.25}, {-0.5, 0.25, 5}};
        double out[3][3];
        rotate(r, in, out);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                QVERIFY2(std::abs(value(turned.c[i][j]) - out[i][j]) < 1e-12, qPrintable(turned.c[i][j]));
        QCOMPARE(turned.items, 6);
        // A diagonal one about z by 90°: still diagonal, x and y swapped.
        const TensorText swapped = rotated(*tensorText(QStringLiteral("44; 44; 28")), *cutRotation(0, 1));
        QCOMPARE(swapped.items, 3);
        QVERIFY(std::abs(value(swapped.c[0][0]) - 28) < 1e-12 && std::abs(value(swapped.c[1][1]) - 44) < 1e-12);

        // Bond's matrix against the rotation of the fourth-rank stiffness
        // itself, C'_ijkl = r_ia r_jb r_kc r_ld C_abcd, for a triclinic C.
        std::mt19937 rng(7);
        std::uniform_real_distribution<double> u(-1, 1);
        double C[6][6];
        for (int I = 0; I < 6; ++I)
            for (int J = I; J < 6; ++J) C[I][J] = C[J][I] = u(rng) + (I == J ? 4 : 0);
        const int vi[3][3] = {{0, 5, 4}, {5, 1, 3}, {4, 3, 2}};
        auto full = [&](int i, int j, int k, int l) { return C[vi[i][j]][vi[k][l]]; };
        double M[6][6];
        bondMatrix(r, M);
        const int pi[6] = {0, 1, 2, 1, 0, 0}, pj[6] = {0, 1, 2, 2, 2, 1};
        VoigtText v;
        v.any = true;
        for (int I = 0; I < 6; ++I)
            for (int J = 0; J < 6; ++J) v.c[I][J] = QString::number(C[I][J], 'g', 17);
        const VoigtText vr = rotated(v, r, QLatin1Char('C'));
        for (int I = 0; I < 6; ++I)
            for (int J = 0; J < 6; ++J) {
                double direct = 0;
                for (int a = 0; a < 3; ++a)
                    for (int b = 0; b < 3; ++b)
                        for (int c = 0; c < 3; ++c)
                            for (int d = 0; d < 3; ++d)
                                direct += r.r[pi[I]][a] * r.r[pj[I]][b] * r.r[pi[J]][c] * r.r[pj[J]][d] * full(a, b, c, d);
                double bond = 0;
                for (int K = 0; K < 6; ++K)
                    for (int L = 0; L < 6; ++L) bond += M[I][K] * C[K][L] * M[J][L];
                QVERIFY2(std::abs(bond - direct) < 1e-12, qPrintable(QStringLiteral("C'%1%2: %3 against %4").arg(I + 1).arg(J + 1).arg(bond).arg(direct)));
                QVERIFY(std::abs(value(vr.c[I][J]) - direct) < 1e-11);
            }
        // e (3x6: D_i = e_iJ S_J) turned as a third-rank tensor is:
        // e'_ijk = r_ia r_jb r_kc e_abc.
        double e[3][6];
        for (auto& row : e)
            for (double& x : row) x = u(rng);
        VoigtText ev;
        ev.rows = 3;
        ev.any = true;
        for (int i = 0; i < 3; ++i)
            for (int J = 0; J < 6; ++J) ev.c[i][J] = QString::number(e[i][J], 'g', 17);
        const VoigtText er = rotated(ev, r, QLatin1Char('e'));
        for (int i = 0; i < 3; ++i)
            for (int J = 0; J < 6; ++J) {
                double direct = 0;
                for (int a = 0; a < 3; ++a)
                    for (int b = 0; b < 3; ++b)
                        for (int c = 0; c < 3; ++c) direct += r.r[i][a] * r.r[pi[J]][b] * r.r[pj[J]][c] * e[a][vi[b][c]];
                QVERIFY2(std::abs(value(er.c[i][J]) - direct) < 1e-12, qPrintable(QStringLiteral("e'%1%2").arg(i + 1).arg(J + 1)));
            }
        // r (6x3: Δη_I = r_Ik E_k): r'_ijk = r_ia r_jb r_kc r_abc.
        VoigtText rv;
        rv.cols = 3;
        rv.any = true;
        for (int I = 0; I < 6; ++I)
            for (int k = 0; k < 3; ++k) rv.c[I][k] = QString::number(e[k][I], 'g', 17);
        const VoigtText rr = rotated(rv, r, QLatin1Char('r'));
        for (int I = 0; I < 6; ++I)
            for (int k = 0; k < 3; ++k) {
                double direct = 0;
                for (int a = 0; a < 3; ++a)
                    for (int b = 0; b < 3; ++b)
                        for (int c = 0; c < 3; ++c) direct += r.r[pi[I]][a] * r.r[pj[I]][b] * r.r[k][c] * e[c][vi[a][b]];
                QVERIFY2(std::abs(value(rr.c[I][k]) - direct) < 1e-12, qPrintable(QStringLiteral("r'%1%2").arg(I + 1).arg(k + 1)));
            }
    }

    void capacitorAlongEachAxis()
    {
        // Its field straight across (y): C = ε0 εyy w/d, εyy the crystal's
        // axis that is up - y as it is, x when x-cut, z when z-cut; and
        // its permittivity in x and z not felt.
        const double w = 10e-3, d = 1e-3;
        const struct {
            QString orientation;
            double epsyy;
        } cases[] = {{QString(), 44},
                     {QStringLiteral(R"("orientation": "cut", "cut": "x", "along": "y")"), 28},
                     {QStringLiteral(R"("orientation": "cut", "cut": "z", "along": "x")"), 99},
                     {QStringLiteral(R"("orientation": "euler", "euler_phi": "90")"), 28}};
        for (const auto& c : cases) {
            const QString material = QStringLiteral(R"("epsilonr": "28; 44; 99")") + (c.orientation.isEmpty() ? QString() : QStringLiteral(", ") + c.orientation);
            const Run r = run(modelOf(plates(material)));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const double C = global(*r.solution, QStringLiteral("es.C11"));
            QVERIFY2(std::abs(C / (Eps0 * c.epsyy * w / d) - 1) < 1e-9,
                     qPrintable(QStringLiteral("%1: C %2 against %3").arg(c.orientation).arg(C).arg(Eps0 * c.epsyy * w / d)));
            // D along y, ε0 εyy E.
            QVERIFY(std::abs(at(*r.solution, QStringLiteral("es.Dy/(eps0*es.Ey)"), {5, 0.5}) - c.epsyy) < 1e-9);
            QVERIFY(std::abs(at(*r.solution, QStringLiteral("es.epsryy"), {5, 0.5}) - c.epsyy) < 1e-12);
        }
        // A tensor of two values: said.
        const Run bad = run(modelOf(plates(QStringLiteral(R"("epsilonr": "28; 44")"))));
        QVERIFY(bad.error.contains(QLatin1String("2 values")));
        QVERIFY(bad.error.contains(QLatin1String("Relative permittivity")));
    }

    void turnedCrystalAgainstItsPde()
    {
        // A crystal turned out of its axes in the plane - its permittivity's
        // xy part not nought - against the PDE of its components: the same
        // potential everywhere. The same of currents and of heat.
        const Rotation r = eulerRotation(35 * M_PI / 180, 20 * M_PI / 180, 10 * M_PI / 180);
        const double crystal[3][3] = {{44, 0, 0}, {0, 30, 0}, {0, 0, 28}};
        double m[3][3];
        rotate(r, crystal, m);
        const QString pde = QStringLiteral(R"("cxx": "%1", "cxy": "%2", "cyx": "%3", "cyy": "%4")")
                                .arg(m[0][0], 0, 'g', 17)
                                .arg(m[0][1], 0, 'g', 17)
                                .arg(m[1][0], 0, 'g', 17)
                                .arg(m[1][1], 0, 'g', 17);
        QVERIFY(std::abs(m[0][1]) > 1);
        const Run reference = run(modelOf(square(QString(), QStringLiteral(R"("epsilonr": "1")"), pde)));
        QVERIFY2(reference.error.isEmpty(), qPrintable(reference.error));
        const struct {
            QString physics, key, field;
        } cases[] = {{QStringLiteral("electrostatics"), QStringLiteral("epsilonr"), QStringLiteral("p.V")},
                     {QStringLiteral("currents"), QStringLiteral("sigma"), QStringLiteral("p.V")},
                     {QStringLiteral("heat"), QStringLiteral("k"), QStringLiteral("p.T")}};
        for (const auto& c : cases) {
            const QString unit = c.key == QLatin1String("sigma") ? QStringLiteral("[S/m]") : c.key == QLatin1String("k") ? QStringLiteral("[W/(m*K)]") : QString();
            const QString material = QStringLiteral(R"("%1": "44%2; 30%2; 28%2", "orientation": "euler", "euler_phi": "35", "euler_theta": "20", "euler_psi": "10")").arg(c.key, unit);
            const Run got = run(modelOf(square(c.physics, material)));
            QVERIFY2(got.error.isEmpty(), qPrintable(c.physics + QStringLiteral(": ") + got.error));
            double worst = 0;
            for (const QPointF p : {QPointF(0.2, 0.3), QPointF(0.8, 0.2), QPointF(0.5, 0.9), QPointF(0.9, 0.9), QPointF(0.4, 0.6)})
                worst = std::max(worst, std::abs(at(*got.solution, c.field, p) - at(*reference.solution, QStringLiteral("c.u"), p)));
            QVERIFY2(worst < 1e-9, qPrintable(QStringLiteral("%1: %2 off the PDE's").arg(c.physics).arg(worst)));
        }
        // The flux: D = ε0 ε E with its xy part.
        const Run es = run(modelOf(square(QStringLiteral("electrostatics"),
                                          QStringLiteral(R"("epsilonr": "44; 30; 28", "orientation": "euler", "euler_phi": "35", "euler_theta": "20", "euler_psi": "10")"))));
        const QPointF p(0.3, 0.7);
        const double ex = at(*es.solution, QStringLiteral("p.Ex"), p), ey = at(*es.solution, QStringLiteral("p.Ey"), p);
        QVERIFY(std::abs(at(*es.solution, QStringLiteral("p.Dx"), p) - Eps0 * (m[0][0] * ex + m[0][1] * ey)) < 1e-9 * Eps0 * std::hypot(ex, ey) * 44);
        QVERIFY(std::abs(at(*es.solution, QStringLiteral("p.epsrxy"), p) - m[0][1]) < 1e-12);
        // As given on the feature (the model's axes): the same.
        const Run user = run(modelOf(square(QStringLiteral("electrostatics"), QStringLiteral(R"("epsilonr": "1")"))
                                         .replace(R"("features": [)",
                                                  QStringLiteral(R"("features": [{"type": "charge", "source": "user", "anisotropy": "symmetric",
                                                                    "epsxx": "%1", "epsyy": "%2", "epsxy": "%3", "selection": {"all": true}},)")
                                                      .arg(m[0][0], 0, 'g', 17)
                                                      .arg(m[1][1], 0, 'g', 17)
                                                      .arg(m[0][1], 0, 'g', 17)
                                                      .toUtf8())));
        QVERIFY2(user.error.isEmpty(), qPrintable(user.error));
        QVERIFY(std::abs(at(*user.solution, QStringLiteral("p.V"), p) - at(*reference.solution, QStringLiteral("c.u"), p)) < 1e-9);
    }

    void lossesAlongTheAxes()
    {
        // A coax filled with a crystal isotropic in the plane, its z axis
        // out of it: C and G the closed forms of its in-plane εr and tan δ,
        // its z values (5 and 0.5) not felt.
        const QByteArray json = R"json({"format": "qucs-s multiphysics 1",
          "parameters": [{"name": "a", "expression": "0.5[mm]"}, {"name": "b", "expression": "1.75[mm]"}],
          "components": [{"unit": "mm",
            "geometry": [{"type": "circle", "name": "dielectric", "radius": "b"}, {"type": "circle", "name": "inner", "radius": "a"}],
            "materials": [{"label": "crystal", "epsilonr": "2.1; 2.1; 5", "tand": "0.002; 0.002; 0.5", "mur": "1", "selection": {"only": ["dielectric"]}},
                          {"material": "Copper", "selection": {"only": ["inner"]}}],
            "physics": [{"type": "electrostatics", "tag": "es", "selection": {"only": ["dielectric"]}, "features": [
                           {"type": "terminal", "name": "1", "V0": "1[V]", "selection": {"of": ["inner"]}},
                           {"type": "ground", "selection": {"of": ["dielectric"]}}]},
                        {"type": "magnetic", "tag": "mf", "features": [
                           {"type": "coil", "name": "1", "I0": "1[A]", "selection": {"only": ["inner"]}}]}],
            "mesh": {"size": "fine", "features": [
              {"type": "size", "level": "domain", "selection": {"only": ["inner"]}, "hmax": "0.05"},
              {"type": "size", "level": "boundary", "selection": {"of": ["inner"]}, "hmax": "0.02"}]}}],
          "studies": [{"type": "study", "steps": [{"type": "stationary", "physics": ["es"]},
                                                  {"type": "frequency", "physics": ["mf"], "frequencies": "1[MHz]"}]}],
          "results": {"derived": [{"type": "lineparams", "tag": "lp1", "method": "rlgc", "physics": ["es"], "terminal": "1",
                                   "magnetic": ["mf"], "conductor": "1"}]}})json";
        const Model model = modelOf(json);
        const Run r = run(model);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const LineRLGC l = lineRLGC(*r.solution, *model.find(QStringLiteral("lp1")));
        QVERIFY2(l.error.isEmpty(), qPrintable(l.error));
        const double C = 2 * M_PI * Eps0 * 2.1 / std::log(1.75 / 0.5), omega = 2 * M_PI * 1e6;
        QVERIFY2(std::abs(l.C / C - 1) < 1e-3, qPrintable(QStringLiteral("C %1 against %2").arg(l.C).arg(C)));
        QVERIFY2(std::abs(l.G / (omega * C * 0.002) - 1) < 2e-3, qPrintable(QStringLiteral("G %1 against %2").arg(l.G).arg(omega * C * 0.002)));
        // Turned, the crystal's z up: εr tan δ along y its z's.
        Model turned = model;
        for (Node& m : turned.materials().children)
            if (m.label == QLatin1String("crystal")) {
                m.props.insert(QStringLiteral("orientation"), QStringLiteral("cut"));
                m.props.insert(QStringLiteral("cut"), QStringLiteral("z"));
                m.props.insert(QStringLiteral("along"), QStringLiteral("x"));
            }
        const Run t = run(turned);
        QVERIFY2(t.error.isEmpty(), qPrintable(t.error));
        QVERIFY(std::abs(at(*t.solution, QStringLiteral("es.epsimyy"), {1, 0}) - 5 * 0.5) < 1e-12);
        QVERIFY(std::abs(at(*t.solution, QStringLiteral("es.epsimxx"), {1, 0}) - 2.1 * 0.002) < 1e-12);
        QVERIFY(std::abs(at(*t.solution, QStringLiteral("es.epsimxy"), {1, 0})) < 1e-15);
        const LineRLGC lt = lineRLGC(*t.solution, *turned.find(QStringLiteral("lp1")));
        QVERIFY(lt.C > 1.3 * l.C && lt.G > 100 * l.G);
    }
    void laminatedCore()
    {
        // A core of µr 2000 along its sheets and 50 across them, turned 30°
        // in the plane, round a wire: its A as the PDE of ν's components has
        // it (β = (∂yA, -∂xA): -∇·(c ∇A) = J, c = [νyy, -νyx; -νxy, νxx]); and
        // as given on Ampère's Law, the same.
        const double mu0 = 1.25663706127e-6, th = 30 * M_PI / 180;
        const double mxx = 2000 * std::cos(th) * std::cos(th) + 50 * std::sin(th) * std::sin(th);
        const double myy = 2000 * std::sin(th) * std::sin(th) + 50 * std::cos(th) * std::cos(th);
        const double mxy = (2000 - 50) * std::cos(th) * std::sin(th);
        const double det = mxx * myy - mxy * mxy;
        const double nxx = myy / det / mu0, nyy = mxx / det / mu0, nxy = -mxy / det / mu0;
        auto json = [](const QString& core, const QString& physics) {
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "mm",
                "geometry": [{"type": "rectangle", "name": "box", "position": ["-1", "-1"], "size": ["2", "2"]},
                             {"type": "circle", "name": "wire", "center": ["0.2", "-0.1"], "radius": "0.15"}],
                "materials": [{"label": "core", %1, "selection": {"all": true}},
                              {"label": "copper", "mur": "1", "selection": {"only": ["wire"]}}],
                "physics": [%2],
                "mesh": {"size": "normal"}}],
              "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json")
                .arg(core, physics)
                .toUtf8();
        };
        const QString mf = QStringLiteral(R"x({"type": "magnetic", "tag": "mf", "features": [
            {"type": "current", "Je": "1e6[A/m^2]", "selection": {"only": ["wire"]}}]})x");
        const QString pde = QStringLiteral(R"x({"type": "pde", "tag": "c", "features": [
            {"type": "coefficient", "diffusion": "anisotropic", "cxx": "%1", "cxy": "%2", "cyx": "%2", "cyy": "%3", "f": "0", "selection": {"all": true}},
            {"type": "coefficient", "c": "%4", "f": "1e6", "selection": {"only": ["wire"]}},
            {"type": "dirichlet", "r": "0", "selection": {"exterior": true}}]})x")
                                .arg(nyy, 0, 'g', 17)
                                .arg(-nxy, 0, 'g', 17)
                                .arg(nxx, 0, 'g', 17)
                                .arg(1 / mu0, 0, 'g', 17);
        const Run reference = run(modelOf(json(QStringLiteral(R"("mur": "1")"), pde)));
        QVERIFY2(reference.error.isEmpty(), qPrintable(reference.error));
        const Run core = run(modelOf(json(QStringLiteral(R"("mur": "2000; 50; 1", "orientation": "euler", "euler_phi": "30")"), mf)));
        QVERIFY2(core.error.isEmpty(), qPrintable(core.error));
        QVERIFY(std::abs(at(*core.solution, QStringLiteral("mf.murxy"), {-0.7, 0.7}) - mxy) < 1e-9);
        double worst = 0, largest = 0;
        for (const QPointF p : {QPointF(-0.5, 0.4), QPointF(0.6, 0.5), QPointF(0.2, -0.6), QPointF(-0.8, -0.8), QPointF(0.25, -0.1)}) {
            const double a = at(*core.solution, QStringLiteral("mf.Az"), p);
            worst = std::max(worst, std::abs(a - at(*reference.solution, QStringLiteral("c.u"), p)));
            largest = std::max(largest, std::abs(a));
        }
        QVERIFY2(worst < 1e-9 * largest, qPrintable(QStringLiteral("%1 off the PDE's (of %2)").arg(worst).arg(largest)));
        // H = ν B: its x component from both B's.
        const QPointF p(-0.4, 0.3);
        const double bx = at(*core.solution, QStringLiteral("mf.Bx"), p), by = at(*core.solution, QStringLiteral("mf.By"), p);
        QVERIFY(std::abs(at(*core.solution, QStringLiteral("mf.Hx"), p) - (nxx * bx + nxy * by)) < 1e-9 * std::abs(nxx * bx));
        // The energy ½ ∫ B·H, the PDE's ½ ∫ f u over the wire.
        Node w;
        w.type = QStringLiteral("integral");
        w.props.insert(QStringLiteral("expression"), QStringLiteral("0.5*1e6*c.u"));
        w.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("only"), QJsonArray{QStringLiteral("wire")}}});
        const DerivedResult d = evaluateDerived(*reference.solution, w);
        QVERIFY2(d.error.isEmpty(), qPrintable(d.error));
        const double W = global(*core.solution, QStringLiteral("mf.W"));
        QVERIFY2(std::abs(W / d.values.at(0).value - 1) < 1e-6, qPrintable(QStringLiteral("W %1 against %2").arg(W).arg(d.values.at(0).value)));
        // As given in the model's axes.
        const Run user = run(modelOf(json(QStringLiteral(R"("mur": "1")"), mf).replace(
            R"("features": [)", QStringLiteral(R"("features": [{"type": "ampere", "source": "user", "anisotropy": "symmetric", "murxx": "%1",
                                  "muryy": "%2", "murxy": "%3", "sigma": "0", "selection": {"all": true}},
                                  {"type": "ampere", "source": "user", "mur": "1", "sigma": "0", "selection": {"only": ["wire"]}},)")
                                    .arg(mxx, 0, 'g', 17)
                                    .arg(myy, 0, 'g', 17)
                                    .arg(mxy, 0, 'g', 17)
                                    .toUtf8())));
        QVERIFY2(user.error.isEmpty(), qPrintable(user.error));
        QVERIFY(std::abs(at(*user.solution, QStringLiteral("mf.Az"), p) - at(*core.solution, QStringLiteral("mf.Az"), p)) < 1e-9 * largest);
    }
    void cubicSilicon()
    {
        // A silicon bar pulled along x, thin (plane stress), its cube's
        // [001] out of the plane: along [100] E = 1/S11 = 130.1 GPa, turned
        // 45° about z (x along [110]) 169.1 GPa; its lateral strain -S12/S11
        // (0.278) along [100], -S'12/S'11 along [110] (0.062).
        const double C11 = 165.7e9, C12 = 63.9e9, C44 = 79.6e9;
        const double S11 = (C11 + C12) / ((C11 - C12) * (C11 + 2 * C12)), S12 = -C12 / ((C11 - C12) * (C11 + 2 * C12)), S44 = 1 / C44;
        const double S11r = S11 - (2 * (S11 - S12) - S44) * 0.25, S12r = S12 + (2 * (S11 - S12) - S44) * 0.25;
        auto bar = [](const QString& orientation, const QString& model) {
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm", "thickness": "1[mm]",
              "geometry": [{"type": "rectangle", "name": "bar", "size": ["10", "1"]}],
              "materials": [{"label": "silicon", "C": "C11=165.7[GPa]; C12=63.9[GPa]; C13=63.9[GPa]; C22=165.7[GPa]; C23=63.9[GPa]; C33=165.7[GPa]; C44=79.6[GPa]; C55=79.6[GPa]; C66=79.6[GPa]",
                             "E": "1[GPa]", "nu": "0.1" %1, "selection": {"all": true}}],
              "physics": [{"type": "solid", "tag": "solid", "model": "%2", "features": [
                {"type": "roller", "selection": {"of": ["bar"], "side": "left"}},
                {"type": "roller", "selection": {"of": ["bar"], "side": "bottom"}},
                {"type": "load", "Fx": "100[MPa]", "selection": {"of": ["bar"], "side": "right"}}]}],
              "mesh": {"size": "coarse"}}],
              "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json")
                .arg(orientation, model)
                .toUtf8();
        };
        const struct {
            QString orientation;
            double S11, S12;
        } cases[] = {{QString(), S11, S12}, {QStringLiteral(R"(, "orientation": "euler", "euler_phi": "45")"), S11r, S12r}};
        for (const auto& c : cases) {
            const Run r = run(modelOf(bar(c.orientation, QStringLiteral("planestress"))));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const double exx = at(*r.solution, QStringLiteral("solid.ex"), {5, 0.5}), eyy = at(*r.solution, QStringLiteral("solid.ey"), {5, 0.5});
            qInfo("silicon%s: E %.2f GPa (1/S11 %.2f), lateral %.4f", qPrintable(c.orientation), 100e6 / exx / 1e9, 1 / c.S11 / 1e9, -eyy / exx);
            QVERIFY2(std::abs(exx / (c.S11 * 100e6) - 1) < 1e-9, qPrintable(QStringLiteral("εxx %1 against %2").arg(exx).arg(c.S11 * 100e6)));
            QVERIFY(std::abs(eyy / (c.S12 * 100e6) - 1) < 1e-9);
            QVERIFY(std::abs(at(*r.solution, QStringLiteral("solid.sx"), {5, 0.5}) - 100e6) < 1e-3);
            QVERIFY(std::abs(at(*r.solution, QStringLiteral("solid.sy"), {5, 0.5})) < 1e-3);
            QVERIFY(std::abs(at(*r.solution, QStringLiteral("solid.sz"), {5, 0.5})) < 1e-3);
            // Its strain energy ½ σ ε over the bar.
            QVERIFY(std::abs(at(*r.solution, QStringLiteral("solid.Ws"), {5, 0.5}) - 0.5 * 100e6 * exx) < 1e-6 * 100e6 * exx);
        }
        QVERIFY(std::abs(1 / S11r / 1e9 - 169.1) < 0.05 && std::abs(1 / S11 / 1e9 - 130.1) < 0.1);
        // Plane strain along [100]: εzz none, σzz = C12 (εxx + εyy).
        const Run ps = run(modelOf(bar(QString(), QStringLiteral("planestrain"))));
        QVERIFY2(ps.error.isEmpty(), qPrintable(ps.error));
        const double exx = at(*ps.solution, QStringLiteral("solid.ex"), {5, 0.5}), eyy = at(*ps.solution, QStringLiteral("solid.ey"), {5, 0.5});
        QVERIFY(std::abs(exx * C11 + eyy * C12 - 100e6) < 1e-3 && std::abs(exx * C12 + eyy * C11) < 1e-3);
        QVERIFY(std::abs(at(*ps.solution, QStringLiteral("solid.sz"), {5, 0.5}) - C12 * (exx + eyy)) < 1e-3);
    }

    void thermalExpansionAlongAxes()
    {
        // A free crystal warmed 100 K, α along its axes 1, 2 and 3 ppm/K,
        // turned 90° about z: εxx its y's 2e-4, εyy its x's 1e-4, no stress
        // (plane stress); held at its ends along x, σxx = -E'... C (S - S0).
        const QByteArray json = R"json({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm",
          "geometry": [{"type": "rectangle", "name": "block", "size": ["4", "2"]}],
          "materials": [{"label": "crystal", "C": "C11=200[GPa]; C12=60[GPa]; C13=50[GPa]; C22=180[GPa]; C23=55[GPa]; C33=220[GPa]; C44=70[GPa]; C55=65[GPa]; C66=60[GPa]",
                         "alpha": "1e-6[1/K]; 2e-6[1/K]; 3e-6[1/K]", "orientation": "euler", "euler_phi": "90", "selection": {"all": true}}],
          "physics": [{"type": "solid", "tag": "solid", "model": "planestress", "features": [
            {"type": "elastic", "thermal": true, "T": "393.15[K]", "Tref": "293.15[K]", "selection": {"all": true}},
            {"type": "roller", "selection": {"of": ["block"], "side": "left"}},
            {"type": "roller", "selection": {"of": ["block"], "side": "bottom"}}]}],
          "mesh": {"size": "coarse"}}],
          "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json";
        const Run r = run(modelOf(json));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QVERIFY(std::abs(at(*r.solution, QStringLiteral("solid.ex"), {2, 1}) - 2e-4) < 1e-12);
        QVERIFY(std::abs(at(*r.solution, QStringLiteral("solid.ey"), {2, 1}) - 1e-4) < 1e-12);
        QVERIFY(std::abs(at(*r.solution, QStringLiteral("solid.sx"), {2, 1})) < 1e-2);
        QVERIFY(std::abs(at(*r.solution, QStringLiteral("solid.u"), {4, 1}) - 2e-4 * 4e-3) < 1e-14);
        // Along axes on an isotropic solid: said.
        QByteArray iso = json;
        iso.replace(R"("C": "C11=200[GPa]; C12=60[GPa]; C13=50[GPa]; C22=180[GPa]; C23=55[GPa]; C33=220[GPa]; C44=70[GPa]; C55=65[GPa]; C66=60[GPa]",)",
                    R"("E": "200[GPa]", "nu": "0.3",)");
        const Run bad = run(modelOf(iso));
        QVERIFY(bad.error.contains(QLatin1String("anisotropic solid")));
    }

    void piezoelectricPlate()
    {
        // An aluminium nitride plate, its c axis up (z-cut), between
        // electrodes at 0 and 10 V, held across (rollers at its sides and
        // bottom), free on top: σyy = C33 Syy - e33 Ey = 0, so Syy = e33
        // Ey/C33 and Dy = (ε0 ε33 + e33²/C33) Ey - its top moved -e33 V/C33,
        // its charge that D's. The two physics solved in turn; the
        // capacitance its terminals give, the clamped one (ε33 alone).
        const double C33 = 395e9, e33 = 1.55, eps33 = 10.7, V = 10, t = 1e-6, w = 20e-6;
        auto plate = [](const QString& top) {
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1", "components": [{"unit": "um",
              "geometry": [{"type": "rectangle", "name": "film", "size": ["20", "1"]}],
              "materials": [{"label": "AlN", "epsilonr": "9.0; 9.0; 10.7",
                             "C": "C11=345[GPa]; C12=125[GPa]; C13=120[GPa]; C22=345[GPa]; C23=120[GPa]; C33=395[GPa]; C44=118[GPa]; C55=118[GPa]; C66=110[GPa]",
                             "e": "e15=-0.48[C/m^2]; e24=-0.48[C/m^2]; e31=-0.58[C/m^2]; e32=-0.58[C/m^2]; e33=1.55[C/m^2]",
                             "orientation": "cut", "cut": "z", "along": "x", "selection": {"all": true}}],
              "physics": [{"type": "electrostatics", "tag": "es", "features": [
                             {"type": "terminal", "name": "1", "V0": "10[V]", "selection": {"of": ["film"], "side": "top"}},
                             {"type": "ground", "selection": {"of": ["film"], "side": "bottom"}}]},
                          {"type": "solid", "tag": "solid", "features": [
                             {"type": "piezo", "selection": {"all": true}},
                             {"type": "roller", "selection": {"of": ["film"], "side": "left"}},
                             {"type": "roller", "selection": {"of": ["film"], "side": "right"}},
                             {"type": "roller", "selection": {"of": ["film"], "side": "bottom"}} %1]}],
              "mesh": {"size": "coarse"}}],
              "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json")
                .arg(top)
                .toUtf8();
        };
        const Run r = run(modelOf(plate(QString())));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.solution;
        const double Ey = -V / t;
        const double v = at(s, QStringLiteral("solid.v"), {10, 1});
        qInfo("AlN plate at 10 V: its top %.6e m (-e33 V/C33 %.6e)", v, -e33 * V / C33);
        QVERIFY2(std::abs(v / (-e33 * V / C33) - 1) < 1e-6, qPrintable(QString::number(v)));
        const double free = (Eps0 * eps33 + e33 * e33 / C33) * w / t;
        const double Q = global(s, QStringLiteral("es.Q_1"));
        QVERIFY2(std::abs(Q / V / free - 1) < 1e-6, qPrintable(QStringLiteral("Q/V %1 against %2").arg(Q / V).arg(free)));
        QVERIFY(std::abs(at(s, QStringLiteral("es.Dy"), {10, 0.5}) - (Eps0 * eps33 + e33 * e33 / C33) * Ey) < 1e-6 * std::abs(Eps0 * eps33 * Ey));
        QVERIFY(std::abs(at(s, QStringLiteral("es.Py"), {10, 0.5}) - e33 * e33 / C33 * Ey) < 1e-6 * std::abs(e33 * e33 / C33 * Ey));
        QVERIFY(std::abs(at(s, QStringLiteral("solid.sy"), {10, 0.5})) < 1e-6 * e33 * std::abs(Ey));
        QVERIFY(std::abs(global(s, QStringLiteral("es.C11")) / (Eps0 * eps33 * w / t) - 1) < 1e-6);
        // Held on top too: no strain, the clamped capacitance; σyy -e33 Ey.
        const Run held = run(modelOf(plate(QStringLiteral(R"(, {"type": "roller", "selection": {"of": ["film"], "side": "top"}})"))));
        QVERIFY2(held.error.isEmpty(), qPrintable(held.error));
        QVERIFY(std::abs(global(*held.solution, QStringLiteral("es.Q_1")) / V / (Eps0 * eps33 * w / t) - 1) < 1e-6);
        QVERIFY(std::abs(at(*held.solution, QStringLiteral("solid.sy"), {10, 0.5}) + e33 * Ey) < 1e-6 * e33 * std::abs(Ey));
        // Its strain constants given instead, d = e C⁻¹ (a datasheet's form):
        // e = d C again, the same plate.
        {
            Eigen::Matrix<double, 6, 6> C = Eigen::Matrix<double, 6, 6>::Zero();
            const double c[6][6] = {{345, 125, 120, 0, 0, 0}, {125, 345, 120, 0, 0, 0}, {120, 120, 395, 0, 0, 0},
                                    {0, 0, 0, 118, 0, 0},     {0, 0, 0, 0, 118, 0},     {0, 0, 0, 0, 0, 110}};
            for (int I = 0; I < 6; ++I)
                for (int K = 0; K < 6; ++K) C(I, K) = c[I][K] * 1e9;
            Eigen::Matrix<double, 3, 6> e = Eigen::Matrix<double, 3, 6>::Zero();
            e(0, 4) = e(1, 3) = -0.48;
            e(2, 0) = e(2, 1) = -0.58;
            e(2, 2) = 1.55;
            const Eigen::Matrix<double, 3, 6> d = e * C.inverse();
            QStringList entries;
            for (int i = 0; i < 3; ++i)
                for (int J = 0; J < 6; ++J)
                    if (std::abs(d(i, J)) > 1e-20) entries << QStringLiteral("d%1%2=%3[C/N]").arg(i + 1).arg(J + 1).arg(d(i, J), 0, 'g', 17);
            QByteArray strain = plate(QString());
            strain.replace(R"("e": "e15=-0.48[C/m^2]; e24=-0.48[C/m^2]; e31=-0.58[C/m^2]; e32=-0.58[C/m^2]; e33=1.55[C/m^2]")",
                           QStringLiteral(R"("d": "%1")").arg(entries.join(QLatin1String("; "))).toUtf8());
            QVERIFY(!strain.contains(R"("e": )"));
            const Run byD = run(modelOf(strain));
            QVERIFY2(byD.error.isEmpty(), qPrintable(byD.error));
            qInfo("by d33 %.4g pC/N (and d31, d15): its top %.6e m", d(2, 2) * 1e12, at(*byD.solution, QStringLiteral("solid.v"), {10, 1}));
            QVERIFY(std::abs(at(*byD.solution, QStringLiteral("solid.v"), {10, 1}) / v - 1) < 1e-9);
            QVERIFY(std::abs(global(*byD.solution, QStringLiteral("es.Q_1")) / Q - 1) < 1e-9);
        }
        // In plane stress: said.
        QByteArray stress = plate(QString());
        stress.replace(R"("tag": "solid", )", R"("tag": "solid", "model": "planestress", )");
        QVERIFY(run(modelOf(stress)).error.contains(QLatin1String("plane strain")));
    }
    void turnedCrystalModes()
    {
        // X-cut, y-propagating lithium niobate: its optic axis along x, so
        // n along x its ne - the same modes as the feature's own nx, ny, nz.
        auto ridge = [](const QString& core, const QString& wave) {
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "name": "clad", "position": ["-2", "-1.5"], "size": ["4", "3.5"]},
                             {"type": "rectangle", "name": "core", "position": ["-0.6", "0"], "size": ["1.2", "0.6"]}],
                "materials": [{"label": "oxide", "n": "1.444", "selection": {"all": true}},
                              {"label": "LN", %1, "selection": {"only": ["core"]}}],
                "physics": [{"type": "modes", "tag": "emw", "features": [{"type": "wave", "selection": {"only": ["core"]} %2}]}],
                "mesh": {"size": "%3", "features": [{"type": "size", "level": "domain", "selection": {"only": ["core"]}, "hmax": "%4"}]}}],
              "studies": [{"type": "study", "steps": [{"type": "mode", "frequency": "1550[nm]", "modes": 2, "neff": "2.2"}]}]})json")
                .arg(core, wave)
                // (The two the same on any mesh: a coarse one on a sanitized build.)
                .arg(slowBuild() ? QStringLiteral("normal") : QStringLiteral("fine"), slowBuild() ? QStringLiteral("0.15") : QStringLiteral("0.06"))
                .toUtf8();
        };
        const Run crystal = run(modelOf(ridge(QStringLiteral(R"("n": "2.211; 2.211; 2.138", "orientation": "cut", "cut": "x", "along": "y")"), QString())));
        const Run given = run(modelOf(
            ridge(QStringLiteral(R"("n": "1")"), QStringLiteral(R"(, "source": "user", "anisotropy": "diagonal", "nx": "2.138", "ny": "2.211", "nz": "2.211")"))));
        QVERIFY2(crystal.error.isEmpty() && given.error.isEmpty(), qPrintable(crystal.error + given.error));
        for (int k = 0; k < 2; ++k) {
            crystal.solution->select(k);
            given.solution->select(k);
            const double a = global(*crystal.solution, QStringLiteral("emw.neff")), b = global(*given.solution, QStringLiteral("emw.neff"));
            QVERIFY2(std::abs(a - b) < 1e-10, qPrintable(QStringLiteral("mode %1: %2 against %3").arg(k + 1).arg(a, 0, 'g', 12).arg(b, 0, 'g', 12)));
        }
        // Its TE mode (E along x, the optic axis) under ne, below the TM's.
        crystal.solution->select(0);
        QVERIFY(global(*crystal.solution, QStringLiteral("emw.TEfraction")) < 0.5);
    }

    void tiltedCrystalSlab()
    {
        // A slab of a strongly birefringent crystal (no 2.2, ne 2.6) whose
        // optic axis is tilted 45° out of the cross-section (about x): its
        // ε's yz part couples Ey to Ez - the modes found by β. Its TM0 against
        // the transfer of Hx across the layers (an independent solution); the
        // coupling worth well more than the mesh's error.
        const double no = 2.2, ne = 2.6, th = 45 * M_PI / 180, k0 = 2 * M_PI / 1.55e-6, d = 0.3e-6;
        const cd yy = no * no * std::cos(th) * std::cos(th) + ne * ne * std::sin(th) * std::sin(th);
        const cd zz = no * no * std::sin(th) * std::sin(th) + ne * ne * std::cos(th) * std::cos(th);
        const cd yz = (no * no - ne * ne) * std::sin(th) * std::cos(th);
        // (The test's own solver: an isotropic slab's TM0 as its equation has it.)
        {
            const std::array<cd, 4> iso[3] = {isotropicEta(1.444), isotropicEta(2.2), isotropicEta(1.444)};
            const double n1 = 2.2, n2 = 1.444, di = 0.4e-6;
            double lo = n2 + 1e-9, hi = n1 - 1e-9;
            for (int i = 0; i < 200; ++i) {
                const double mid = (lo + hi) / 2, kappa = k0 * std::sqrt(n1 * n1 - mid * mid), gamma = k0 * std::sqrt(mid * mid - n2 * n2);
                if (kappa * di / 2 >= M_PI / 2 || std::tan(kappa * di / 2) - (n1 * n1) / (n2 * n2) * gamma / kappa > 0) lo = mid;
                else hi = mid;
            }
            QVERIFY(std::abs(slabTM(iso, di, k0, k0 * 1.9).real() / k0 - (lo + hi) / 2) < 1e-10);
        }
        const Run r = run(modelOf(tmSlab(QStringLiteral(R"("n": "2.2; 2.2; 2.6", "orientation": "euler", "euler_theta": "45")"), 0.3,
                                         QStringLiteral("1.444"), QString(), QStringLiteral("2.2"))));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        r.solution->select(0);
        const double got = global(*r.solution, QStringLiteral("emw.neff"));
        // (Rx(θ) turns the crystal's z to (0, -sin θ, cos θ): its yz -(no² - ne²) sin θ cos θ.)
        const std::array<cd, 4> layers[3] = {isotropicEta(1.444), etaOf(yy, -yz, -yz, zz), isotropicEta(1.444)};
        const double expected = slabTM(layers, d, k0, k0 * got).real() / k0;
        const std::array<cd, 4> uncoupled[3] = {isotropicEta(1.444), etaOf(yy, 0, 0, zz), isotropicEta(1.444)};
        const double without = slabTM(uncoupled, d, k0, k0 * got).real() / k0;
        qInfo("tilted slab TM0: %.7f, its layers' transfer %.7f (without the coupling %.7f)", got, expected, without);
        QVERIFY2(std::abs(got - expected) < 2e-5, qPrintable(QStringLiteral("%1 against %2").arg(got, 0, 'g', 10).arg(expected, 0, 'g', 10)));
        QVERIFY(std::abs(without - expected) > 10 * std::abs(got - expected));
        qInfo("its imaginary part %.3g", global(*r.solution, QStringLiteral("emw.neff_im")));
        QVERIFY(std::abs(global(*r.solution, QStringLiteral("emw.neff_im"))) < 1e-7);
    }

    void nonReciprocalPhaseShift()
    {
        // A cerium-substituted garnet slab on oxide under air, magnetized
        // across the waveguide (x): its TM0's β forward differs from its
        // backward one (g's sign turned) - the isolator's non-reciprocal
        // phase shift - as the transfer of Hx across the layers has it.
        const double n = 2.22, g = 0.009, k0 = 2 * M_PI / 1.55e-6, d = 0.25e-6;
        auto fem = [&](double sign) {
            const Run r = run(modelOf(tmSlab(QStringLiteral(R"("n": "2.22", "g": "%1")").arg(sign * g), 0.25, QStringLiteral("1"),
                                             QStringLiteral(R"(, "magnetooptic": true, "magnetization": "x")"), QStringLiteral("2"))));
            if (!r.error.isEmpty()) qWarning("%s", qPrintable(r.error));
            r.solution->select(0);
            return global(*r.solution, QStringLiteral("emw.neff"));
        };
        auto exact = [&](double sign, double near) {
            // ε_yz = j g, ε_zy = -j g (e^{jωt}).
            const std::array<cd, 4> layers[3] = {isotropicEta(1.444), etaOf(n * n, J * sign * g, -J * sign * g, n * n), isotropicEta(1)};
            return slabTM(layers, d, k0, k0 * near).real() / k0;
        };
        // (Its index without g a reference: the oxide's 3 µm truncate this
        // weakly guided mode by some 6e-5 alike - what g moves, compared.)
        const double none = fem(0), forward = fem(1), backward = fem(-1);
        const double e0 = exact(0, none), ef = exact(1, forward), eb = exact(-1, backward);
        qInfo("garnet TM0: forward %.7f, backward %.7f (NRPS %.3e), without g %.7f; the layers' %.7f, %.7f (%.3e), %.7f", forward, backward,
              forward - backward, none, ef, eb, ef - eb, e0);
        QVERIFY(std::abs(ef - eb) > 1e-4);
        QVERIFY2(std::abs((forward - backward) / (ef - eb) - 1) < 1e-2, qPrintable(QStringLiteral("%1 against %2").arg(forward - backward).arg(ef - eb)));
        QVERIFY(std::abs((forward - none) / (ef - e0) - 1) < 2e-2);
        QVERIFY(std::abs(none - e0) < 1e-4);
    }

    void pockelsAndPhotoelasticShifts()
    {
        // A metal guide 1 µm by 0.5 µm filled with a crystal, its TE10 (E
        // along y): β² = k0² ny² - (π/a)². Its field across, E y-wards from
        // an electrostatics between the walls, under Pockels: 1/ny² = 1/ne²
        // + r33 Ey (z-cut: its optic axis up). Strained along x (a solid
        // mechanics' displacement), photoelastic: 1/ny² = 1/n² + p12 Sxx +
        // p11 Syy + p12 Szz.
        const double a = 1e-6, k0 = 2 * M_PI / 1.55e-6;
        auto neff = [&](double ny) { return std::sqrt(ny * ny - std::pow(M_PI / (a * k0), 2)); };
        auto box = [](const QString& material, const QString& wave, const QString& more, const QString& steps) {
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "name": "guide", "size": ["1", "0.5"]}],
                "materials": [{"label": "crystal", %1, "selection": {"all": true}}],
                "physics": [{"type": "modes", "tag": "emw", "features": [{"type": "wave", "selection": {"all": true} %2}]} %3],
                "mesh": {"size": "fine", "features": [{"type": "size", "level": "domain", "selection": {"all": true}, "hmax": "0.05"}]}}],
              "studies": [{"type": "study", "steps": [%4{"type": "mode", "frequency": "1550[nm]", "modes": 1, "neff": "2.1"}]}]})json")
                .arg(material, wave, more, steps)
                .toUtf8();
        };
        const QString es = QStringLiteral(R"x(, {"type": "electrostatics", "tag": "es", "features": [
            {"type": "terminal", "name": "1", "V0": "5[V]", "selection": {"of": ["guide"], "side": "top"}},
            {"type": "ground", "selection": {"of": ["guide"], "side": "bottom"}}]})x");
        const QString ln = QStringLiteral(R"("n": "2.211; 2.211; 2.138", "epsilonr": "44; 44; 28",
                                              "r": "r13=9.6[pm/V]; r23=9.6[pm/V]; r33=30.9[pm/V]; r22=6.8[pm/V]; r12=-6.8[pm/V]; r61=-6.8[pm/V]; r42=32.6[pm/V]; r51=32.6[pm/V]",
                                              "orientation": "cut", "cut": "z", "along": "x")");
        const Run eo = run(modelOf(box(ln, QStringLiteral(R"(, "electrooptic": true)"), es, QString())));
        QVERIFY2(eo.error.isEmpty(), qPrintable(eo.error));
        eo.solution->select(0);
        const double Ey = -5 / 0.5e-6;
        const double ny = 1 / std::sqrt(1 / (2.138 * 2.138) + 30.9e-12 * Ey);
        const double got = global(*eo.solution, QStringLiteral("emw.neff"));
        const Run plain = run(modelOf(box(ln, QString(), es, QString())));
        plain.solution->select(0);
        const double still = global(*plain.solution, QStringLiteral("emw.neff"));
        qInfo("Pockels: TE10 %.7f at 10 V/um (%.7f), unbiased %.7f (%.7f)", got, neff(ny), still, neff(2.138));
        QVERIFY2(std::abs(got - neff(ny)) < 2e-6 && std::abs(still - neff(2.138)) < 2e-6,
                 qPrintable(QStringLiteral("%1 against %2").arg(got, 0, 'g', 10).arg(neff(ny), 0, 'g', 10)));
        QVERIFY(std::abs((got - still) - (neff(ny) - neff(2.138))) < 1e-7);

        // Strained 1e-3 along x (plane strain), an isotropic glass's p.
        const QString glass = QStringLiteral(R"("n": "2.1", "E": "70[GPa]", "nu": "0.17",
                                                 "p": "p11=0.121; p22=0.121; p33=0.121; p12=0.270; p13=0.270; p23=0.270; p44=-0.0745; p55=-0.0745; p66=-0.0745")");
        const QString solid = QStringLiteral(R"x(, {"type": "solid", "tag": "solid", "features": [
            {"type": "roller", "selection": {"of": ["guide"], "side": "left"}},
            {"type": "roller", "selection": {"of": ["guide"], "side": "bottom"}},
            {"type": "displacement", "x": true, "u0": "1e-3*1[um]", "y": false, "selection": {"of": ["guide"], "side": "right"}}]})x");
        const Run pe = run(modelOf(box(glass, QStringLiteral(R"(, "photoelastic": true)"), solid, QString())));
        QVERIFY2(pe.error.isEmpty(), qPrintable(pe.error));
        pe.solution->select(0);
        const double sxx = at(*pe.solution, QStringLiteral("solid.ex"), {0.5, 0.25}), syy = at(*pe.solution, QStringLiteral("solid.ey"), {0.5, 0.25});
        QVERIFY(std::abs(sxx - 1e-3) < 1e-12);
        const double nys = 1 / std::sqrt(1 / (2.1 * 2.1) + 0.270 * sxx + 0.121 * syy);
        const double strained = global(*pe.solution, QStringLiteral("emw.neff"));
        qInfo("photoelastic: TE10 %.7f strained (%.7f), Syy %.3e", strained, neff(nys), syy);
        QVERIFY2(std::abs(strained - neff(nys)) < 2e-6, qPrintable(QStringLiteral("%1 against %2").arg(strained, 0, 'g', 10).arg(neff(nys), 0, 'g', 10)));

        // Sheared (u = γ y on every edge: a uniform simple shear, exy γ/2), a
        // cubic crystal of strong anisotropy (p11 - p12 - 2 p44 = -0.6) turned
        // 22.5° about z: its p in the model's axes (Bond's) has p26, so the
        // shear moves ηyy at once - 1/ny² = 1/n² + p'26 γ.
        const double gamma = 1e-3;
        const QString cubic = QStringLiteral("p11=0.1; p22=0.1; p33=0.1; p12=0.3; p13=0.3; p23=0.3; p44=0.2; p55=0.2; p66=0.2");
        const QString turned = QStringLiteral(R"("n": "2.1", "E": "70[GPa]", "nu": "0.17", "p": "%1", "orientation": "euler", "euler_phi": "22.5")").arg(cubic);
        const QString shear = QStringLiteral(R"x(, {"type": "solid", "tag": "solid", "features": [
            {"type": "displacement", "x": true, "u0": "1e-3*y", "y": true, "v0": "0[m]", "selection": {"of": ["guide"]}}]})x");
        const Run sh = run(modelOf(box(turned, QStringLiteral(R"(, "photoelastic": true)"), shear, QString())));
        QVERIFY2(sh.error.isEmpty(), qPrintable(sh.error));
        sh.solution->select(0);
        QVERIFY2(std::abs(at(*sh.solution, QStringLiteral("solid.exy"), {0.5, 0.25}) - gamma / 2) < 1e-12,
                 qPrintable(QString::number(at(*sh.solution, QStringLiteral("solid.exy"), {0.5, 0.25}))));
        const std::optional<VoigtText> pc = voigtText(cubic, QLatin1Char('p'), 6, 6, true);
        QVERIFY(pc);
        const VoigtText pm = rotated(*pc, eulerRotation(M_PI / 8, 0, 0), QLatin1Char('p'));
        const double p26 = value(pm.c[1][5]);
        QVERIFY(std::abs(p26) > 0.1);
        const double nyShear = 1 / std::sqrt(1 / (2.1 * 2.1) + p26 * gamma);
        const double gotShear = global(*sh.solution, QStringLiteral("emw.neff"));
        qInfo("sheared: TE10 %.7f (%.7f, p'26 %.4f)", gotShear, neff(nyShear), p26);
        QVERIFY2(std::abs(gotShear - neff(nyShear)) < 2e-6, qPrintable(QStringLiteral("%1 against %2").arg(gotShear, 0, 'g', 10).arg(neff(nyShear), 0, 'g', 10)));
    }

    void conductiveSheet()
    {
        // WR-90 at 10 GHz, a sheet of σs down its middle (x = a/2, in the
        // plane of E): TE10's tan(kx a/2) = 2j kx/(ω µ0 σs), β² = k0² - kx².
        const double a = 22.86e-3, f = 10e9, omega = 2 * M_PI * f, k0 = omega / C0, mu0 = 1.25663706127e-6;
        const cd sigma(1e-3, -2e-4);
        // (Its pole away: 2j kx cos(kx a/2) - ω µ0 σs sin(kx a/2) = 0, from the
        // empty guide's π/a.)
        cd kx = M_PI / a;
        for (int i = 0; i < 50; ++i) {
            auto g = [&](cd k) { return 2.0 * J * k * std::cos(k * a / 2.0) - omega * mu0 * sigma * std::sin(k * a / 2.0); };
            const cd dk = 1e-7 * kx;
            kx -= g(kx) * dk / (g(kx + dk) - g(kx));
        }
        const cd beta = std::sqrt(k0 * k0 - kx * kx);
        const QByteArray json = R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "mm",
            "geometry": [{"type": "rectangle", "name": "left", "size": ["11.43", "10.16"]},
                         {"type": "rectangle", "name": "right", "position": ["11.43", "0"], "size": ["11.43", "10.16"]}],
            "materials": [{"label": "air", "n": "1", "selection": {"all": true}}],
            "physics": [{"type": "modes", "tag": "emw", "features": [
              {"type": "sheet", "model": "user", "sigmas": "1e-3[S]", "sigmas_im": "2e-4[S]", "selection": {"between": [["left"], ["right"]]}}]}],
            "mesh": {"size": "fine", "custom": true, "hmax": "0.6"}}],
          "studies": [{"type": "study", "steps": [{"type": "mode", "frequency": "10[GHz]", "modes": 1, "neff": "0.75"}]}]})json";
        const Run r = run(modelOf(json));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        r.solution->select(0);
        const double neff = global(*r.solution, QStringLiteral("emw.neff")), loss = global(*r.solution, QStringLiteral("emw.alpha"));
        qInfo("sheet in WR-90: neff %.6f, α %.4f Np/m; its transverse resonance %.6f, %.4f", neff, loss, beta.real() / k0, -beta.imag());
        QVERIFY2(std::abs(neff - beta.real() / k0) < 2e-4, qPrintable(QStringLiteral("%1 against %2").arg(neff).arg(beta.real() / k0)));
        QVERIFY2(std::abs(loss / -beta.imag() - 1) < 5e-3, qPrintable(QStringLiteral("α %1 against %2").arg(loss).arg(-beta.imag())));
        // The power it absorbs: the mode's loss, 2α, all in the sheet.
        QVERIFY2(std::abs(global(*r.solution, QStringLiteral("emw.Psheet")) / (2 * loss) - 1) < 5e-3,
                 qPrintable(QString::number(global(*r.solution, QStringLiteral("emw.Psheet")))));
    }

    void crystalLibrary()
    {
        // The crystals' indices at 1550 nm, their sources' formulas'.
        const double l = 1.55, l2 = l * l;
        auto zelmon = [l2](double b1, double c1, double b2, double c2, double b3, double c3) {
            return std::sqrt(1 + b1 * l2 / (l2 - c1) + b2 * l2 / (l2 - c2) + b3 * l2 / (l2 - c3));
        };
        auto kato = [l2](double a, double b, double c, double d, double e) { return std::sqrt(a + b / (l2 - c) + d / (l2 - e)); };
        auto sell = [l2](double a, std::initializer_list<std::pair<double, double>> t) {
            double n2 = a;
            for (const auto& [b, c] : t) n2 += b * l2 / (l2 - c * c);
            return std::sqrt(n2);
        };
        struct {
            const char* name;
            double n[3];
        } indices[] = {
            {"Lithium niobate",
             {zelmon(2.6734, 0.01764, 1.2290, 0.05914, 12.614, 474.60), zelmon(2.6734, 0.01764, 1.2290, 0.05914, 12.614, 474.60),
              zelmon(2.9804, 0.02047, 0.5981, 0.0666, 8.9543, 416.08)}},
            // (Bond's, straight between 1.4 and 1.6 µm.)
            {"Lithium tantalate", {2.1236 - 0.75 * 0.0062, 2.1236 - 0.75 * 0.0062, 2.1273 - 0.75 * 0.0060}},
            {"KTP",
             {kato(3.29100, 0.04140, 0.03978, 9.35522, 31.45571), kato(3.45018, 0.04341, 0.04597, 16.98825, 39.43799),
              kato(4.59423, 0.06206, 0.04763, 110.80672, 86.12171)}},
            {"Quartz",
             {sell(1.28604141, {{1.07044083, std::sqrt(1.00585997e-2)}, {1.10202242, 10}}),
              sell(1.28604141, {{1.07044083, std::sqrt(1.00585997e-2)}, {1.10202242, 10}}),
              sell(1.28851804, {{1.09509924, std::sqrt(1.02101864e-2)}, {1.15662475, 10}})}},
            {"Sapphire",
             {sell(1, {{1.4313493, 0.0726631}, {0.65054713, 0.1193242}, {5.3414021, 18.028251}}),
              sell(1, {{1.4313493, 0.0726631}, {0.65054713, 0.1193242}, {5.3414021, 18.028251}}),
              sell(1, {{1.5039759, 0.0740288}, {0.55069141, 0.1216529}, {6.5927379, 20.072248}})}}};
        for (const auto& c : indices) {
            const Node m = libraryNode(QString::fromLatin1(c.name));
            const QStringList items = tensorItems(detail::Problem::materialText(m, QStringLiteral("n")));
            QCOMPARE(items.size(), 3);
            for (int k = 0; k < 3; ++k) {
                const double got = valueAt(items[k]);
                QVERIFY2(std::abs(got - c.n[k]) < 1e-9, qPrintable(QStringLiteral("%1's n%2: %3 against %4").arg(QLatin1String(c.name)).arg(k + 1).arg(got).arg(c.n[k])));
            }
            qInfo("%s at 1550 nm: n %.5f, %.5f, %.5f", c.name, c.n[0], c.n[1], c.n[2]);
        }
        QVERIFY(std::abs(indices[0].n[0] - 2.2111) < 1e-4 && std::abs(indices[0].n[2] - 2.1376) < 1e-4);
        // Each tensor text read whole.
        for (const char* name : {"Lithium niobate", "Lithium tantalate", "KTP", "Quartz", "Sapphire", "YIG", "Ce:YIG", "Silicon", "Gallium arsenide",
                                 "Indium phosphide", "Silicon dioxide", "Fused silica"}) {
            const Node m = libraryNode(QString::fromLatin1(name));
            struct {
                const char* key;
                char letter;
                int rows, cols;
                bool symmetric;
            } voigt[] = {{"C", 'C', 6, 6, true}, {"d", 'd', 3, 6, false}, {"r", 'r', 6, 3, false}, {"p", 'p', 6, 6, true}};
            for (const auto& v : voigt) {
                const QString t = detail::Problem::materialText(m, QString::fromLatin1(v.key));
                if (t.isEmpty()) continue;
                QString why;
                const std::optional<VoigtText> parsed = voigtText(t, QLatin1Char(v.letter), v.rows, v.cols, v.symmetric, &why);
                QVERIFY2(parsed && parsed->any, qPrintable(QStringLiteral("%1's %2: %3").arg(QLatin1String(name), QLatin1String(v.key), why)));
                for (int I = 0; I < v.rows; ++I)
                    for (int K = 0; K < v.cols; ++K)
                        QVERIFY2(std::isfinite(valueAt(parsed->c[I][K])), qPrintable(QStringLiteral("%1's %2: %3").arg(QLatin1String(name), QLatin1String(v.key), parsed->c[I][K])));
            }
            for (const char* key : {"epsilonr", "k", "alpha", "n"}) {
                const QString t = detail::Problem::materialText(m, QString::fromLatin1(key));
                if (t.isEmpty()) continue;
                QString why;
                QVERIFY2(tensorText(t, &why), qPrintable(QStringLiteral("%1's %2: %3").arg(QLatin1String(name), QLatin1String(key), why)));
            }
        }

        // A physics of isotropic media (a semiconductor's, rays') takes a
        // tensor's mean: lithium niobate's εr (44 + 44 + 28)/3.
        QVERIFY(std::abs(valueAt(detail::Problem::materialProperty(libraryNode(QStringLiteral("Lithium niobate")), QStringLiteral("epsilonr"))) - 116.0 / 3) < 1e-12);
        // e = d C, against the stress constants measured (lithium niobate's
        // Weis and Gaylord's 3.7, 2.5, 0.2, 1.3 C/m²; quartz's Bechmann's
        // 0.171, -0.0406).
        auto stressConstants = [](const QString& name) {
            const Node m = libraryNode(name);
            const std::optional<VoigtText> d = voigtText(detail::Problem::materialText(m, QStringLiteral("d")), QLatin1Char('d'), 3, 6, false);
            const std::optional<VoigtText> c = voigtText(detail::Problem::materialText(m, QStringLiteral("C")), QLatin1Char('C'), 6, 6, true);
            std::array<std::array<double, 6>, 3> e {};
            if (!d || !c) return e;
            const VoigtText p = multiplied(*d, *c);
            for (int i = 0; i < 3; ++i)
                for (int J = 0; J < 6; ++J) e[std::size_t(i)][std::size_t(J)] = valueAt(p.c[i][J]);
            return e;
        };
        const auto ln = stressConstants(QStringLiteral("Lithium niobate"));
        qInfo("lithium niobate's e = d C: e15 %.3f, e22 %.3f, e31 %.3f, e33 %.3f C/m^2", ln[0][4], ln[1][1], ln[2][0], ln[2][2]);
        QVERIFY(std::abs(ln[0][4] / 3.7 - 1) < 0.03 && std::abs(ln[1][1] / 2.5 - 1) < 0.03 && std::abs(ln[2][2] / 1.3 - 1) < 0.04);
        QVERIFY(std::abs(ln[2][0] - 0.2) < 0.05);
        // 3m's: e16 = -e22, e24 = e15, e32 = e31.
        QVERIFY(std::abs(ln[0][5] + ln[1][1]) < 1e-12 && std::abs(ln[1][3] - ln[0][4]) < 1e-12 && std::abs(ln[2][1] - ln[2][0]) < 1e-12);
        const auto quartz = stressConstants(QStringLiteral("Quartz"));
        qInfo("quartz's e = d C: e11 %.4f, e14 %.4f C/m^2", quartz[0][0], quartz[0][3]);
        QVERIFY(std::abs(quartz[0][0] / 0.171 - 1) < 0.01 && std::abs(quartz[0][3] / -0.0406 - 1) < 0.01);

        // Silicon's a (100) wafer's axes, from its panel's node and from a
        // file's (its library named alone): E 169 GPa along x ([1-10]), 130
        // along y ([001]).
        const Node panel = libraryNode(QStringLiteral("Silicon"));
        Node file;
        file.type = QStringLiteral("material");
        file.props.insert(QStringLiteral("library"), QStringLiteral("Silicon"));
        for (const Node* m : std::initializer_list<const Node*>{&panel, &file}) {
            const std::optional<Rotation> r = materialRotation(*m, Scope::builtins());
            QVERIFY(r);
            QVERIFY(std::abs(r->r[1][2] - 1) < 1e-15);
            QVERIFY(std::abs(r->r[0][0] - M_SQRT1_2) < 1e-15 && std::abs(r->r[0][1] + M_SQRT1_2) < 1e-15);
            const std::optional<VoigtText> c = voigtText(detail::Problem::materialText(*m, QStringLiteral("C")), QLatin1Char('C'), 6, 6, true);
            QVERIFY(c);
            const VoigtText turned = rotated(*c, *r, QLatin1Char('C'));
            Eigen::Matrix<double, 6, 6> C;
            for (int I = 0; I < 6; ++I)
                for (int K = 0; K < 6; ++K) C(I, K) = valueAt(turned.c[I][K]);
            const Eigen::Matrix<double, 6, 6> S = C.inverse();
            qInfo("silicon on its wafer: E %.2f GPa along x, %.2f along y", 1e-9 / S(0, 0), 1e-9 / S(1, 1));
            QVERIFY(std::abs(1e-9 / S(0, 0) - 169.1) < 0.1 && std::abs(1e-9 / S(1, 1) - 130.1) < 0.1);
        }
        // Lithium niobate's an x-cut wafer's, y along z: its optic axis along x.
        Node lnFile;
        lnFile.type = QStringLiteral("material");
        lnFile.props.insert(QStringLiteral("library"), QStringLiteral("Lithium niobate"));
        const std::optional<Rotation> xcut = materialRotation(lnFile, Scope::builtins());
        QVERIFY(xcut && xcut->r[1][0] == 1 && xcut->r[2][1] == 1 && xcut->r[0][2] == 1);
        // Its own orientation over its entry's.
        lnFile.props.insert(QStringLiteral("orientation"), QStringLiteral("none"));
        QVERIFY(materialRotation(lnFile, Scope::builtins())->isIdentity());
    }

    void turnedCrystalInWaves()
    {
        // The waves physics, Hz, in a parallel-plate guide 10 mm high at
        // 6 GHz (its TEM mode alone). A crystal (n 1.5, 2, 1.5): a slab of it
        // 10 mm long between air, S21 the slab's of εyy (E across, along y);
        // the guide filled with it, ports in it: e^{-jβL}, β = k0 √εyy, its
        // ports' mode the medium's along and across them.
        auto guide = [](const QString& before, const QString& crystal, const QString& after, const QString& extra) {
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "mm",
                "geometry": [{"type": "rectangle", "name": "before", "size": ["10", "10"]},
                             {"type": "rectangle", "name": "crystal", "position": ["10", "0"], "size": ["%4", "10"]},
                             {"type": "rectangle", "name": "after", "position": ["10+%4", "0"], "size": ["%5", "10"]}],
                "materials": [{"label": "before", %1, "selection": {"only": ["before"]}},
                              {"label": "crystal", %2, "selection": {"only": ["crystal"]}},
                              {"label": "after", %3, "selection": {"only": ["after"]}}],
                "physics": [{"type": "waves", "tag": "ewfd", "polarization": "hz", "features": [
                  {"type": "port", "name": "1", "selection": {"of": ["before"], "side": "left"}} %6]}],
                "mesh": {"size": "fine", "custom": true, "hmax": "0.5"}}],
              "studies": [{"type": "study", "steps": [{"type": "frequency", "frequencies": "6[GHz]"}]}]})json")
                .arg(before, crystal, after)
                .arg(extra.contains(QLatin1String("pml")) ? QStringLiteral("50") : QStringLiteral("10"),
                     extra.contains(QLatin1String("pml")) ? QStringLiteral("20") : QStringLiteral("10"), extra)
                .toUtf8();
        };
        const QString air = QStringLiteral(R"("n": "1")"), crystal = QStringLiteral(R"("n": "1.5; 2; 1.5")");
        const QString port2 = QStringLiteral(R"(, {"type": "port", "name": "2", "selection": {"of": ["after"], "side": "right"}})");
        const double k0 = 2 * M_PI * 6e9 / C0, L = 10e-3;
        auto slab = [k0, L](double eps) {
            const double n = std::sqrt(eps), r = (1 - n) / (1 + n);
            const cd p = std::exp(cd(0, -k0 * n * L));
            return (1 - r * r) * p / (1.0 - r * r * p * p) * std::exp(cd(0, -k0 * 2 * L));
        };
        auto s21 = [](const Run& r) { return cd(global(*r.solution, QStringLiteral("ewfd.S21")), global(*r.solution, QStringLiteral("ewfd.S21_im"))); };
        const Run between = run(modelOf(guide(air, crystal, air, port2)));
        QVERIFY2(between.error.isEmpty(), qPrintable(between.error));
        qInfo("crystal slab, Hz: S21 %.5f%+.5fj, εyy's slab %.5f%+.5fj", s21(between).real(), s21(between).imag(), slab(4).real(), slab(4).imag());
        QVERIFY(std::abs(s21(between) - slab(4)) < 2e-3);
        const Run filled = run(modelOf(guide(crystal, crystal, crystal, port2)));
        QVERIFY2(filled.error.isEmpty(), qPrintable(filled.error));
        const cd through = std::exp(cd(0, -2 * k0 * 30e-3));
        qInfo("filled, its ports in it: S21 %.5f%+.5fj (e^-jβL %.5f%+.5fj), |S11| %.1e, β %.4f /m (k0 √εyy %.4f)", s21(filled).real(), s21(filled).imag(),
              through.real(), through.imag(), global(*filled.solution, QStringLiteral("ewfd.S11_abs")), global(*filled.solution, QStringLiteral("ewfd.beta_1")),
              2 * k0);
        QVERIFY(std::abs(global(*filled.solution, QStringLiteral("ewfd.beta_1")) / (2 * k0) - 1) < 1e-6);
        QVERIFY(std::abs(s21(filled) - through) < 2e-3 && global(*filled.solution, QStringLiteral("ewfd.S11_abs")) < 2e-3);

        // Turned 30° about z: εxx 2.6875, εyy 3.5625, εxy -0.758. Between the
        // plates (Ex = 0 on them) its wave is e^{-jβx - jκy}, β = k0 √εyy and
        // κ = (ηxy/ηxx) β (E = η ∇×H/jωε0: Ex ∝ -jκ ηxx + jβ ηxy, nought) -
        // a front tilted, its fundamental mode alone at 6 GHz. Fed from air at
        // its face, 50 mm of it, then a PML: its phase 20 to 30 mm in.
        const QString turned = QStringLiteral(R"("n": "1.5; 2; 1.5", "orientation": "euler", "euler_phi": "30")");
        const Run tilted = run(modelOf(guide(air, turned, turned, QStringLiteral(R"(, {"type": "pml", "selection": {"only": ["after"]}})"))));
        QVERIFY2(tilted.error.isEmpty(), qPrintable(tilted.error));
        Solution& t = *tilted.solution;
        auto hz = [&t](double x, double y) {
            return cd(at(t, QStringLiteral("ewfd.Hz"), {x, y}), at(t, QStringLiteral("ewfd.Hz_im"), {x, y}));
        };
        const double c = std::cos(M_PI / 6), sn = std::sin(M_PI / 6);
        const double exx = 2.25 * c * c + 4 * sn * sn, eyy = 2.25 * sn * sn + 4 * c * c, exy = (2.25 - 4) * c * sn, det = exx * eyy - exy * exy;
        const double beta = k0 * std::sqrt(eyy), kappa = (-exy / det) / (eyy / det) * beta;
        double gotBeta = 0, gotKappa = 0;
        for (const double y : {2.0, 5.0, 8.0}) gotBeta += -std::arg(hz(40, y) / hz(30, y)) / 10e-3 / 3;
        for (const double x : {30.0, 35.0, 40.0}) gotKappa += -std::arg(hz(x, 8) / hz(x, 2)) / 6e-3 / 3;
        qInfo("turned crystal between plates: β %.3f /m (k0 √εyy %.3f), κ %.3f /m ((ηxy/ηxx) β %.3f)", gotBeta, beta, gotKappa, kappa);
        QVERIFY2(std::abs(gotBeta / beta - 1) < 5e-3, qPrintable(QString::number(gotBeta)));
        QVERIFY2(std::abs(gotKappa / kappa - 1) < 0.03, qPrintable(QString::number(gotKappa)));
        QVERIFY(std::abs(kappa) > 0.2 * beta);
        // Ez in a guide filled with µr 2 along x, 1 across (y) and along z,
        // its ports in it: the TE10 (sin(πy/w), along the port) β² = µyy (k0²
        // εzz - (π/w)²/µxx) - its port's line problem µxx along it, µyy across.
        QByteArray ez = guide(QStringLiteral(R"("n": "1", "mur": "2; 1; 1")"), QStringLiteral(R"("n": "1", "mur": "2; 1; 1")"),
                              QStringLiteral(R"("n": "1", "mur": "2; 1; 1")"), port2);
        ez.replace(R"("polarization": "hz")", R"("polarization": "ez")");
        ez.replace(R"("frequencies": "6[GHz]")", R"("frequencies": "20[GHz]")");
        const Run ferrite = run(modelOf(ez));
        QVERIFY2(ferrite.error.isEmpty(), qPrintable(ferrite.error));
        const double k20 = 2 * M_PI * 20e9 / C0, betaTE = std::sqrt(k20 * k20 - std::pow(M_PI / 10e-3, 2) / 2);
        qInfo("Ez, µr 2, 1, 1: β %.4f /m (%.4f), |S11| %.1e", global(*ferrite.solution, QStringLiteral("ewfd.beta_1")), betaTE,
              global(*ferrite.solution, QStringLiteral("ewfd.S11_abs")));
        QVERIFY(std::abs(global(*ferrite.solution, QStringLiteral("ewfd.beta_1")) / betaTE - 1) < 1e-4);
        QVERIFY(global(*ferrite.solution, QStringLiteral("ewfd.S11_abs")) < 1e-2);
        // A port in it: its mode a tilted one, refused.
        const Run inside = run(modelOf(guide(turned, turned, turned, port2)));
        QVERIFY(inside.error.contains(QLatin1String("turned in the plane")));
    }

    void grapheneLayerAgainstSheet()
    {
        // Graphene on a silicon slab (220 nm, in oxide), undoped at 1550 nm
        // (60.8 µS): as a sheet (in the middle of a layer 1 nm thick of
        // oxide), as the layer conducting in its plane alone (σs/t along x
        // and z), and conducting every way. The layer in its plane is the
        // sheet - TE and TM alike; conducting every way, TM loses more: its
        // normal field in the layer, εyy 2.085 - 5.65j there, a third of the
        // oxide's (D across it the same), and a TM mode's large at the
        // silicon's face.
        auto model = [](bool tm, const QString& layer) {
            const bool sheet = layer.isEmpty();
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "name": "below", "position": ["0", "-%8"], "size": ["0.02", "%8"]},
                             {"type": "rectangle", "name": "core", "size": ["0.02", "0.22"]},
                             {"type": "rectangle", "name": "layer", "position": ["0", "0.22"], "size": ["0.02", "0.0005"]},
                             {"type": "rectangle", "name": "layer2", "position": ["0", "0.2205"], "size": ["0.02", "0.0005"]},
                             {"type": "rectangle", "name": "above", "position": ["0", "0.221"], "size": ["0.02", "%8"]}],
                "materials": [{"label": "oxide", "n": "1.444", "selection": {"only": ["below", "above"]}},
                              {"label": "silicon", "n": "3.476", "selection": {"only": ["core"]}},
                              {"label": "graphene", "epsilonr": "2.085136", %1 "selection": {"only": ["layer", "layer2"]}}],
                "physics": [{"type": "modes", "tag": "emw", "features": [
                    {"type": "wave", "constitutive": "epsilonr", "selection": {"only": ["layer", "layer2"]}}
                    %2 %3]}],
                "mesh": {"size": "%9", "features": [{"type": "size", "level": "domain", "selection": {"only": ["layer", "layer2"]}, "hmax": "0.002"},
                                                      {"type": "size", "level": "domain", "selection": {"only": ["core"]}, "hmax": "%6"},
                                                      {"type": "size", "level": "domain", "selection": {"only": ["below", "above"]}, "hmax": "%7"}]}}],
              "studies": [{"type": "study", "steps": [{"type": "mode", "frequency": "1550[nm]", "modes": 1, "neff": "%4", "polarization": "%5"}]}]})json")
                .arg(sheet ? QString() : layer + QStringLiteral(","),
                     sheet ? QStringLiteral(R"(, {"type": "sheet", "model": "user", "sigmas": "60.8e-6[S]", "sigmas_im": "0[S]",
                                                  "selection": {"between": [["layer"], ["layer2"]]}})")
                           : QString(),
                     tm ? QStringLiteral(R"(, {"type": "pmc", "selection": {"of": ["below", "core", "layer", "layer2", "above"], "side": "left"}},
                                            {"type": "pmc", "selection": {"of": ["below", "core", "layer", "layer2", "above"], "side": "right"}})")
                        : QString(),
                     tm ? QStringLiteral("1.8") : QStringLiteral("2.8"), tm ? QStringLiteral("tm") : QStringLiteral("te"),
                     // (Coarser on a sanitized build: its six solves.)
                     QStringLiteral("0.02"), QStringLiteral("0.08"))
                // (Its global size coarse on a sanitized build: its layers' grading, the
                // unknowns a third - the losses the same to 1e-6.)
                .arg(QStringLiteral("2"), slowBuild() ? QStringLiteral("coarse") : QStringLiteral("normal"))
                .toUtf8();
        };
        const QString inPlane = QStringLiteral(R"("sigma": "6.08e4[S/m]; 0[S/m]; 6.08e4[S/m]")");
        const QString everyWay = QStringLiteral(R"("sigma": "6.08e4[S/m]")");
        for (const bool tm : {false, true}) {
            double alpha[3];
            const QString layers[3] = {QString(), inPlane, everyWay};
            for (int k = 0; k < 3; ++k) {
                const Run r = run(modelOf(model(tm, layers[k])));
                QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
                r.solution->select(0);
                alpha[k] = global(*r.solution, QStringLiteral("emw.alpha"));
            }
            qInfo("graphene on silicon, %s: α %.3f /m as a sheet, %.3f a layer conducting in its plane (%+.3f %%), %.3f every way (%+.3f %%)",
                  tm ? "TM" : "TE", alpha[0], alpha[1], 100 * (alpha[1] / alpha[0] - 1), alpha[2], 100 * (alpha[2] / alpha[0] - 1));
            QVERIFY(alpha[0] > 10);
            QVERIFY2(std::abs(alpha[1] / alpha[0] - 1) < 2e-4, qPrintable(QStringLiteral("%1 against %2").arg(alpha[1]).arg(alpha[0])));
            if (tm) QVERIFY2(alpha[2] / alpha[1] - 1 > 0.1, qPrintable(QStringLiteral("%1 against %2").arg(alpha[2]).arg(alpha[1])));
            else QVERIFY(std::abs(alpha[2] / alpha[1] - 1) < 1e-3);
        }
    }

    void zCutAgainstXCut()
    {
        // The review's: a lithium niobate core (the library's) in oxide,
        // between plates 4 µm apart - z-cut, the plates above and below, its
        // TM mode; and the same turned 90° in the plane, x-cut, the plates
        // left and right, its TE mode. Turned, it is the same device: the
        // same Δneff per volt, the same VπL.
        auto device = [](bool zcut, double volts) {
            const QString w = zcut ? QStringLiteral("6") : QStringLiteral("4"), h = zcut ? QStringLiteral("4") : QStringLiteral("6");
            const QString cw = zcut ? QStringLiteral("1.2") : QStringLiteral("0.6"), ch = zcut ? QStringLiteral("0.6") : QStringLiteral("1.2");
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "parameters": [{"name": "V", "expression": "%9[V]"}],
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "name": "box", "position": ["-%1/2", "-%2/2"], "size": ["%1", "%2"]},
                             {"type": "rectangle", "name": "core", "position": ["-%3/2", "-%4/2"], "size": ["%3", "%4"]},
                             {"type": "difference", "name": "oxide", "input": ["box"], "tools": ["core"], "keep": true}],
                "materials": [{"material": "Silicon dioxide", "selection": {"only": ["oxide"]}},
                              {"material": "Lithium niobate", "orientation": "cut", "cut": "%5", "along": "y", "selection": {"only": ["core"]}}],
                "physics": [{"type": "electrostatics", "tag": "es", "features": [
                    {"type": "potential", "V0": "V", "selection": {"box": [%6]}},
                    {"type": "ground", "selection": {"box": [%7]}}]},
                  {"type": "modes", "tag": "emw", "features": [
                    {"type": "wave", "electrooptic": true, "selection": {"only": ["core"]}}]}],
                "mesh": {"size": "%12", "features": [{"type": "size", "level": "domain", "selection": {"only": ["core"]}, "hmax": "%10"},
                                                      {"type": "size", "level": "domain", "selection": {"only": ["oxide"]}, "hmax": "%11"}]}}],
              "studies": [{"type": "study", "steps": [{"type": "mode", "frequency": "1550[nm]", "modes": 1, "neff": "2.1", "polarization": "%8"}]}]})json")
                // (The plates: the box's bottom and top, its left and right.)
                .arg(w, h, cw, ch, zcut ? QStringLiteral("z") : QStringLiteral("x"),
                     zcut ? QStringLiteral("-3.01, -2.01, 3.01, -1.99") : QStringLiteral("-2.01, -3.01, -1.99, 3.01"),
                     zcut ? QStringLiteral("-3.01, 1.99, 3.01, 2.01") : QStringLiteral("1.99, -3.01, 2.01, 3.01"), zcut ? QStringLiteral("tm") : QStringLiteral("te"))
                .arg(volts)
                .arg(slowBuild() ? QStringLiteral("0.1") : QStringLiteral("0.05"))
                .arg(slowBuild() ? QStringLiteral("0.6") : QStringLiteral("0.4"), slowBuild() ? QStringLiteral("coarse") : QStringLiteral("fine"))
                .toUtf8();
        };
        // (On a sanitized build the biased devices alone: the same device
        // turned, the same index - to 5e-6, a tenth of what 10 V moves it.)
        if (slowBuild()) {
            double biased[2];
            for (const bool zcut : {true, false}) {
                const Run r = run(modelOf(device(zcut, 10)));
                QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
                r.solution->select(0);
                biased[zcut ? 0 : 1] = global(*r.solution, QStringLiteral("emw.neff"));
            }
            qInfo("at 10 V: z-cut TM %.7f, x-cut turned TE %.7f", biased[0], biased[1]);
            QVERIFY2(std::abs(biased[0] - biased[1]) < 5e-6, qPrintable(QStringLiteral("%1 against %2").arg(biased[0]).arg(biased[1])));
            return;
        }
        double perVolt[2], neff0[2];
        for (const bool zcut : {true, false}) {
            double n[2];
            for (int k = 0; k < 2; ++k) {
                const Run r = run(modelOf(device(zcut, k == 0 ? 0 : 10)));
                QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
                r.solution->select(0);
                n[k] = global(*r.solution, QStringLiteral("emw.neff"));
                // (Pockels' r in pm/V, as m/V expected: no warning.)
                for (const QString& l : r.solution->log) QVERIFY2(!l.contains(QLatin1String("Warning")), qPrintable(l));
            }
            perVolt[zcut ? 0 : 1] = (n[1] - n[0]) / 10;
            neff0[zcut ? 0 : 1] = n[0];
        }
        const double vpil[2] = {1.55e-6 / (2 * std::abs(perVolt[0])) * 100, 1.55e-6 / (2 * std::abs(perVolt[1])) * 100};
        qInfo("z-cut, TM: neff %.6f, %.4e per volt (VπL %.3f V·cm); x-cut turned, TE: neff %.6f, %.4e per volt (VπL %.3f V·cm)", neff0[0], perVolt[0],
              vpil[0], neff0[1], perVolt[1], vpil[1]);
        QVERIFY2(std::abs(neff0[0] - neff0[1]) < 2e-4, qPrintable(QStringLiteral("%1 against %2").arg(neff0[0]).arg(neff0[1])));
        QVERIFY2(std::abs(perVolt[0] / perVolt[1] - 1) < 1e-3, qPrintable(QStringLiteral("%1 against %2").arg(perVolt[0]).arg(perVolt[1])));
        // r33 the one that acts: -n³ r33 E Γ/2 with E the core's (a layer's
        // ε33 28 in oxide's 3.9) - within a fifth.
        const double eCore = 10 / (4e-6 - 0.6e-6 + 0.6e-6 * 3.9 / 28) * 3.9 / 28;
        const double estimate = -0.5 * std::pow(2.1376, 3) * 30.8e-12 * eCore / 10;
        QVERIFY2(std::abs(perVolt[0] / estimate - 1) < 0.5, qPrintable(QStringLiteral("%1 against %2").arg(perVolt[0]).arg(estimate)));
    }

    void grapheneConductivity()
    {
        // Kubo's: at 1550 nm (ħω 0.80 eV), undoped graphene absorbs e²/4ħ
        // (60.8 µS); with µc 0.5 eV, Pauli-blocked, a few µS (the Drude
        // tail) and its interband part capacitive (Im σ > 0 for e^{jωt});
        // at 1 THz, µc 0.2 eV, it is Drude's D/(1/τ + jω).
        const double e = 1.602176634e-19, hbar = 1.054571817e-34, omega = 2 * M_PI * C0 / 1.55e-6;
        const cd undoped = qucs_s::fem::grapheneConductivity(omega, 0, 1e-13, 300);
        QVERIFY2(std::abs(undoped.real() / (e * e / (4 * hbar)) - 1) < 0.01, qPrintable(QString::number(undoped.real())));
        const cd doped = qucs_s::fem::grapheneConductivity(omega, 0.5 * e, 1e-13, 300);
        QVERIFY(doped.real() < 0.1 * e * e / (4 * hbar));
        // (T → 0: (e²/4πħ) ln((2µ + ħω)/(2µ - ħω)) for the interband's imaginary part, less the Drude part's.)
        const cd cold = qucs_s::fem::grapheneConductivity(omega, 0.5 * e, 1e-13, 1);
        const double D = e * e / (M_PI * hbar * hbar) * 0.5 * e;
        const cd drude = D / cd(1e13, omega);
        const double inter = e * e / (4 * M_PI * hbar) * std::log((2 * 0.5 * e + hbar * omega) / (2 * 0.5 * e - hbar * omega));
        QVERIFY2(std::abs((cold - drude).imag() / inter - 1) < 1e-3, qPrintable(QStringLiteral("%1 against %2").arg((cold - drude).imag()).arg(inter)));
        const double w = 2 * M_PI * 1e12;
        const cd thz = qucs_s::fem::grapheneConductivity(w, 0.2 * e, 1e-13, 300);
        const double Dt = e * e / (M_PI * hbar * hbar) * (0.2 * e + 2 * 1.380649e-23 * 300 * std::log1p(std::exp(-0.2 * e / (1.380649e-23 * 300))));
        QVERIFY(std::abs(thz / (Dt / cd(1e13, w)) - 1.0) < 1e-2);
    }
};

QTEST_GUILESS_MAIN(TestFemAnisotropy)
#include "test_fem_anisotropy.moc"
