/*
 * test_fem_lumerical.cpp - a multiphysics model as Lumerical scripts
 *                          (MODE, CHARGE, MQW) and Lumerical's projects
 *                          and scripts as models
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Lumerical does not run here: its scripts are read back by the importer's
 * reader of its language (what they would make), and the models made of
 * them solved against the models they were made of. Its projects are the
 * synthetic ones of data/lumerical_projects.py (laid out as its own are).
 */
#include "fem_geometry.h"
#include "fem_lumerical.h"
#include "fem_lumerical_scene.h"
#include "fem_materials.h"
#include "fem_model.h"
#include "fem_solver.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <complex>

using namespace qucs_s::fem;
namespace lum = qucs_s::fem::lumerical;

namespace {

constexpr double C0 = 299792458.0;

/// A sanitized build's: its solves many times slower.
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

QJsonObject exampleJson(const QString& name)
{
    QFile f(QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/") + name);
    if (!f.open(QIODevice::ReadOnly)) qFatal("no example %s", qPrintable(name));
    return QJsonDocument::fromJson(f.readAll()).object();
}

Model modelOf(const QJsonObject& o, const QString& name = {})
{
    QString why;
    std::optional<Model> m = Model::fromJson(QJsonDocument(o).toJson(), &why);
    if (!m) qFatal("model %s: %s", qPrintable(name), qPrintable(why));
    m->fileName = name;
    return *m;
}

Model example(const QString& name)
{
    return modelOf(exampleJson(name), QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/") + name);
}

/// The SOI wire, its silicon dispersive: 3.476 at 1550 nm, 0.08 less a µm on.
QJsonObject soi()
{
    QJsonObject o = exampleJson(QStringLiteral("soi_waveguide_modes.qfem"));
    QJsonArray comps = o.value(QStringLiteral("components")).toArray();
    QJsonObject c = comps.at(0).toObject();
    QJsonArray mats = c.value(QStringLiteral("materials")).toArray();
    mats[1] = QJsonObject{{QStringLiteral("label"), QStringLiteral("silicon")},
                          {QStringLiteral("n"), QStringLiteral("3.476 - 0.08*(lambda0/1[um] - 1.55)")},
                          {QStringLiteral("selection"), QJsonObject{{QStringLiteral("only"), QJsonArray{QStringLiteral("core")}}}}};
    c.insert(QStringLiteral("materials"), mats);
    comps[0] = c;
    o.insert(QStringLiteral("components"), comps);
    return o;
}

/// The first study's first step of \a type set as \a props say.
QJsonObject withStep(QJsonObject o, const QString& type, const QJsonObject& props)
{
    QJsonArray studies = o.value(QStringLiteral("studies")).toArray();
    QJsonObject st = studies.at(0).toObject();
    QJsonArray steps = st.value(QStringLiteral("steps")).toArray();
    for (int i = 0; i < steps.size(); ++i) {
        QJsonObject step = steps.at(i).toObject();
        if (step.value(QStringLiteral("type")).toString() != type) continue;
        for (auto it = props.begin(); it != props.end(); ++it) step.insert(it.key(), it.value());
        steps[i] = step;
        break;
    }
    st.insert(QStringLiteral("steps"), steps);
    studies[0] = st;
    o.insert(QStringLiteral("studies"), studies);
    return o;
}

struct Solved {
    StudyRun study;
    QString error;
};
Solved solve(const Model& model, const QString& studyTag = {})
{
    Solved r;
    const std::shared_ptr<ParameterScope> ps = evaluateParameters(model);
    const GeometryBuild g = buildGeometry(model, *ps);
    if (!g.ok()) {
        r.error = QStringLiteral("geometry: ") + QStringList(g.errors.values()).join(QLatin1String("; "));
        return r;
    }
    r.study = runStudy(model, ps, g.topology, nullptr, studyTag);
    if (!r.study.ok()) r.error = r.study.error;
    return r;
}
std::vector<double> globals(const Solved& r, const QString& name)
{
    std::vector<double> out;
    for (const auto& s : r.study.solutions) out.push_back(s->global(name).value_or(NAN));
    return out;
}

LumericalScript exported(const Model& m, const QString& type, const QJsonObject& props = {})
{
    Node n = m.make(type);
    for (auto it = props.begin(); it != props.end(); ++it) n.props.insert(it.key(), it.value());
    return lumericalScript(m, n, QStringLiteral("x"), QStringLiteral("27"));
}

/// A script written to \a dir and imported.
LumericalImport imported(const QTemporaryDir& dir, const QString& text, const QString& name = QStringLiteral("x.lsf"))
{
    QFile f(dir.filePath(name));
    if (!f.open(QIODevice::WriteOnly)) qFatal("cannot write %s", qPrintable(name));
    f.write(text.toUtf8());
    f.close();
    return importLumerical(dir.filePath(name));
}

Model modelOfImport(const LumericalImport& im)
{
    if (!im.error.isEmpty()) qFatal("import: %s", qPrintable(im.error));
    return modelOf(im.model);
}

const lum::Object* object(const lum::Scene& s, const QString& kind, const QString& name = {})
{
    for (const lum::Object& o : s.objects)
        if (o.kind == kind && (name.isEmpty() || o.name() == name)) return &o;
    return nullptr;
}

QJsonObject component(const QJsonObject& model)
{
    return model.value(QStringLiteral("components")).toArray().at(0).toObject();
}
QJsonObject named(const QJsonArray& list, const QString& key, const QString& value)
{
    for (const QJsonValue& v : list)
        if (v.toObject().value(key).toString() == value) return v.toObject();
    return {};
}
QJsonArray features(const QJsonObject& model)
{
    return component(model).value(QStringLiteral("physics")).toArray().at(0).toObject().value(QStringLiteral("features")).toArray();
}
double numberOf(const QJsonValue& v)
{
    QString t = v.toString();
    const int bracket = t.indexOf(QLatin1Char('['));
    if (bracket > 0) t = t.left(bracket);
    return v.isDouble() ? v.toDouble() : t.toDouble();
}
bool near(double a, double b, double rel)
{
    return std::abs(a - b) <= rel * std::max(std::abs(a), std::abs(b));
}

} // namespace

class TestFemLumerical : public QObject
{
    Q_OBJECT

private slots:
    // --- Exports --------------------------------------------------------

    // MODE: an SOI wire in oxide, its window ended by PML domains: a
    // structure for each of its domains (the core's mesh order the lower),
    // (n,k) materials of their indices, an FDE region over the window - PML
    // where the PML domains were -, the mode step's first wavelength, its
    // modes and the index about, a frequency sweep to its last.
    void aModeScript()
    {
        const Model m = modelOf(soi(), QStringLiteral("soi.qfem"));
        const LumericalScript s = exported(m, QStringLiteral("lumericalmode"));
        QVERIFY2(s.error.isEmpty(), qPrintable(s.error));
        QVERIFY(s.text.startsWith(QLatin1String("# x.lsf - a Lumerical MODE script")) && s.text.contains(QLatin1String("save(\"x.lms\");")));
        const lum::Scene sc = lum::readScript(s.text, QStringLiteral("x.lsf"));
        QVERIFY2(sc.error.isEmpty(), qPrintable(sc.error));
        const lum::Object* core = object(sc, QStringLiteral("rect"), QStringLiteral("core d3"));
        const lum::Object* oxide = object(sc, QStringLiteral("rect"), QStringLiteral("oxide d2"));
        QVERIFY(core && oxide);
        QVERIFY(!object(sc, QStringLiteral("rect"), QStringLiteral("pml d1")));
        QCOMPARE(core->number(QStringLiteral("x min")), -0.25e-6);
        QCOMPARE(core->number(QStringLiteral("y max")), 0.11e-6);
        QVERIFY(core->number(QStringLiteral("mesh order")) < oxide->number(QStringLiteral("mesh order")));
        QCOMPARE(core->number(QStringLiteral("override mesh order from material database")), 1.0);
        // (Their indices at 1500 nm: the silicon's 3.48, the oxide's.)
        QCOMPARE(int(sc.materials.size()), 2);
        for (const lum::Material& mat : sc.materials) {
            QCOMPARE(mat.type, QStringLiteral("(n,k) Material"));
            const double n = mat.n[0];
            QVERIFY2(mat.name.startsWith(QLatin1String("silicon n=")) ? std::abs(n - 3.48) < 1e-9 : std::abs(n - 1.444) < 1e-9, qPrintable(mat.name));
            QCOMPARE(mat.name, mat.name.section(QLatin1Char('='), 0, 0) + QLatin1Char('=') + QString::number(n, 'g', 6));
        }
        QCOMPARE(core->text(QStringLiteral("material")), sc.materials[std::size_t(sc.materials[0].n[0] > 2 ? 0 : 1)].name);
        const lum::Object* fde = object(sc, QStringLiteral("fde"));
        QVERIFY(fde);
        QCOMPARE(fde->text(QStringLiteral("solver type")), QStringLiteral("2D Z normal"));
        QVERIFY(near(fde->number(QStringLiteral("x span")), 3.4e-6, 1e-12) && near(fde->number(QStringLiteral("y span")), 3.2e-6, 1e-12));
        for (const char* bc : {"x min bc", "x max bc", "y min bc", "y max bc"}) QCOMPARE(fde->text(QString::fromLatin1(bc)), QStringLiteral("PML"));
        QCOMPARE(fde->number(QStringLiteral("analysis/wavelength")), 1.5e-6);
        QCOMPARE(fde->number(QStringLiteral("analysis/number of trial modes")), 2.0);
        QCOMPARE(fde->number(QStringLiteral("analysis/n")), 2.2);
        QCOMPARE(fde->text(QStringLiteral("analysis/search")), QStringLiteral("near n"));
        QCOMPARE(fde->number(QStringLiteral("analysis/stop wavelength")), 1.6e-6);
        QCOMPARE(fde->number(QStringLiteral("analysis/number of points")), 3.0);
        QVERIFY(s.text.contains(QLatin1String("frequencysweep;")) && s.text.contains(QLatin1String("try {")));
        // (Its mesh: λ/(20 n) of the largest index.)
        QVERIFY(near(fde->number(QStringLiteral("dx")), 1.5e-6 / (20 * sc.materials[sc.materials[0].n[0] > 2 ? 0 : 1].n[0]), 1e-9));
    }

    // The script made a model again: the window its region, its PML sides
    // layers inside it, the core's index as the model had it - the modes
    // at 1550 nm as the model's (solved by the Release jobs: two fine mode
    // solves past QtTest's 300 s under the sanitizers).
    void aModeRoundTrip()
    {
        const Model m = modelOf(withStep(soi(), QStringLiteral("mode"),
                                         QJsonObject{{QStringLiteral("frequency"), QStringLiteral("1550[nm]")}}),
                                QStringLiteral("soi.qfem"));
        QTemporaryDir dir;
        const LumericalImport im = imported(dir, exported(m, QStringLiteral("lumericalmode")).text);
        QVERIFY2(im.error.isEmpty(), qPrintable(im.error));
        const QJsonArray geometry = component(im.model).value(QStringLiteral("geometry")).toArray();
        QVERIFY(!named(geometry, QStringLiteral("label"), QStringLiteral("window")).isEmpty());
        QVERIFY(!named(geometry, QStringLiteral("label"), QStringLiteral("pml_left")).isEmpty() && !named(geometry, QStringLiteral("label"), QStringLiteral("pml_top")).isEmpty());
        QVERIFY(!named(features(im.model), QStringLiteral("type"), QStringLiteral("pml")).isEmpty());
        // (The core's index its material's name says.)
        bool core = false;
        for (const QJsonValue& v : component(im.model).value(QStringLiteral("materials")).toArray()) {
            const QString l = v.toObject().value(QStringLiteral("label")).toString();
            if (!l.startsWith(QLatin1String("silicon n="))) continue;
            core = true;
            QCOMPARE(numberOf(v.toObject().value(QStringLiteral("n"))), l.mid(10).toDouble());
        }
        QVERIFY(core);
        if (slowBuild()) QSKIP("its two fine mode solves past 300 s under the sanitizers: the Release jobs solve them");
        const Solved a = solve(m), b = solve(modelOfImport(im));
        QVERIFY2(a.error.isEmpty() && b.error.isEmpty(), qPrintable(a.error + b.error));
        const std::vector<double> x = globals(a, QStringLiteral("emw.neff")), y = globals(b, QStringLiteral("emw.neff"));
        QCOMPARE(x.size(), y.size());
        for (std::size_t i = 0; i < x.size(); ++i) {
            qInfo("mode %d: neff %.6f, its round trip's %.6f", int(i + 1), x[i], y[i]);
            QVERIFY2(std::abs(x[i] - y[i]) < 3e-4, qPrintable(QStringLiteral("%1 against %2").arg(x[i]).arg(y[i])));
        }
    }

    // CHARGE: a pn diode - its materials Lumerical's by name, its domains in
    // the x-z plane, each contact a metal strip outside its boundary (the
    // anode swept: a value table), the doping constant boxes, the norm
    // length the model's thickness, a 2D y-normal region over all.
    void aChargeScript()
    {
        const Model m = example(QStringLiteral("pn_diode.qfem"));
        const LumericalScript s = exported(m, QStringLiteral("lumericalcharge"));
        QVERIFY2(s.error.isEmpty(), qPrintable(s.error));
        QVERIFY(s.text.contains(QLatin1String("save(\"x.ldev\");")) && !s.text.contains(QLatin1String("run(\"CHARGE\")")));
        const lum::Scene sc = lum::readScript(s.text, QStringLiteral("x.lsf"));
        QVERIFY2(sc.error.isEmpty(), qPrintable(sc.error));
        const lum::Object* si = object(sc, QStringLiteral("modelmaterial"), QStringLiteral("Silicon"));
        const lum::Object* metal = object(sc, QStringLiteral("modelmaterial"), QStringLiteral("contact metal"));
        QVERIFY(si && metal);
        QCOMPARE(si->text(QStringLiteral("@CT")), QStringLiteral("Si (Silicon)"));
        QCOMPARE(metal->text(QStringLiteral("@CT")), QStringLiteral("Al (Aluminium) - CRC"));
        const lum::Object* p = object(sc, QStringLiteral("rect"), QStringLiteral("pside d1"));
        QVERIFY(p);
        QCOMPARE(p->number(QStringLiteral("x max")), 5e-6);
        QCOMPARE(p->number(QStringLiteral("z max")), 1e-6);
        QCOMPARE(p->text(QStringLiteral("material")), QStringLiteral("Silicon"));
        // (The anode's strip: left of x = 0, 1 % of the device thick.)
        const lum::Object* strip = object(sc, QStringLiteral("rect"), QStringLiteral("contact a"));
        QVERIFY(strip);
        QVERIFY(std::abs(strip->number(QStringLiteral("x max"))) < 1e-15 && near(strip->number(QStringLiteral("x min")), -1e-7, 1e-9));
        QCOMPARE(strip->number(QStringLiteral("z max")), 1e-6);
        QCOMPARE(strip->text(QStringLiteral("material")), QStringLiteral("contact metal"));
        QCOMPARE(strip->number(QStringLiteral("mesh order")), 1.0);
        const lum::Object* dope = object(sc, QStringLiteral("dope"), QStringLiteral("p side"));
        QVERIFY(dope);
        QCOMPARE(dope->text(QStringLiteral("dopant type")), QStringLiteral("p"));
        QCOMPARE(dope->number(QStringLiteral("concentration")), 1e22);
        QCOMPARE(dope->number(QStringLiteral("x max")), 5e-6);
        const lum::Object* a = object(sc, QStringLiteral("contact"), QStringLiteral("a"));
        const lum::Object* k = object(sc, QStringLiteral("contact"), QStringLiteral("k"));
        QVERIFY(a && k);
        QCOMPARE(a->text(QStringLiteral("sweep type")), QStringLiteral("value"));
        QCOMPARE(a->prop(QStringLiteral("value table"))->m, (std::vector<double>{-2, -1, -0.5, 0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7}));
        QCOMPARE(a->text(QStringLiteral("solid")), QStringLiteral("contact a"));
        QCOMPARE(k->text(QStringLiteral("sweep type")), QStringLiteral("single"));
        const lum::Object* solver = object(sc, QStringLiteral("charge"));
        const lum::Object* region = object(sc, QStringLiteral("region"));
        QVERIFY(solver && region);
        QCOMPARE(solver->number(QStringLiteral("norm length")), 1e-4);
        QCOMPARE(solver->text(QStringLiteral("simulation region")), region->name());
        QCOMPARE(region->number(QStringLiteral("dimension")), 2.0);
        QVERIFY(near(region->number(QStringLiteral("x span")), 10.2e-6, 1e-9));   // (the strips with it)
    }

    // The script made a model again (its strips left out, each contact on
    // the boundaries it covered, the gate's work function its own): its gate
    // charge over the sweep within 3 % of the model's (its mesh its own).
    void aChargeRoundTrip()
    {
        // (A slow build's two voltages: three's six solves near QtTest's 300 s on CI.)
        const bool slow = slowBuild();
        QJsonArray rows;
        rows.append(QJsonArray{QStringLiteral("Vg"), slow ? QStringLiteral("-2, 2") : QStringLiteral("-2, 0, 2"), QStringLiteral("V")});
        const QJsonObject o = withStep(exampleJson(QStringLiteral("mos_capacitor.qfem")), QStringLiteral("sweep"), QJsonObject{{QStringLiteral("parameters"), rows}});
        const Model m = modelOf(o, QStringLiteral("moscap.qfem"));
        QTemporaryDir dir;
        const LumericalScript s = exported(m, QStringLiteral("lumericalcharge"));
        QVERIFY(s.text.contains(QLatin1String("set(\"work function\", 4.05)")));
        const LumericalImport im = imported(dir, s.text);
        QVERIFY2(im.error.isEmpty(), qPrintable(im.error));
        const QJsonObject gate = named(features(im.model), QStringLiteral("type"), QStringLiteral("gate"));
        QCOMPARE(gate.value(QStringLiteral("phim")).toString(), QStringLiteral("4.05[eV]"));
        QVERIFY(gate.value(QStringLiteral("selection")).toObject().contains(QStringLiteral("box")));
        QCOMPARE(named(features(im.model), QStringLiteral("type"), QStringLiteral("ohmic")).value(QStringLiteral("name")).toString(), QStringLiteral("b"));
        const Solved a = solve(m), b = solve(modelOfImport(im));
        QVERIFY2(a.error.isEmpty() && b.error.isEmpty(), qPrintable(a.error + b.error));
        const std::vector<double> x = globals(a, QStringLiteral("semi.Q_g")), y = globals(b, QStringLiteral("semi.Q_g"));
        const std::size_t n = slow ? 2 : 3;
        QCOMPARE(x.size(), n);
        QCOMPARE(y.size(), n);
        for (std::size_t i = 0; i < n; ++i) {
            qInfo("Vg %d V: Q_g %.5g C, its round trip's %.5g", int(4 * i / (n - 1)) - 2, x[i], y[i]);
            // (The model's mesh is not carried: Lumerical's is its own, the round trip's "fine".)
            QVERIFY2(std::abs(x[i] - y[i]) <= 0.03 * std::abs(x[0]), qPrintable(QStringLiteral("%1 against %2").arg(x[i]).arg(y[i])));
        }
    }

    // A box's Gaussian tails as Lumerical's diffusion - its box the model's
    // grown by the junction depth, the junction width that, the reference
    // concentration the background - and back; an annealed implant sampled
    // into an import doping object as the semiconductor has it (left out on
    // the way back, said).
    void aChargeProfiles()
    {
        const QJsonObject o = QJsonDocument::fromJson(R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "um", "thickness": "10[um]",
            "geometry": [{"type": "rectangle", "label": "si", "size": ["4", "2"]}, {"type": "form union"}],
            "materials": [{"material": "Silicon", "selection": {"all": true}}],
            "physics": [{"type": "semiconductor", "tag": "semi", "T": "310[K]", "features": [
              {"type": "doping", "label": "well", "dopant": "acceptor", "N0": "1e16[1/cm^3]", "selection": {"all": true}},
              {"type": "doping", "label": "source", "dopant": "donor", "profile": "box", "position": ["0.5", "1.6"], "size": ["1", "0.4"],
               "N0": "1e19[1/cm^3]", "Nb": "1e15[1/cm^3]", "dj": "0.2[um]", "selection": {"all": true}},
              {"type": "doping", "label": "implant", "dopant": "acceptor", "profile": "implant", "dose": "1e13[1/cm^2]", "Rp": "0.2[um]",
               "dRp": "0.05[um]", "dRl": "0.08[um]", "surface": "2", "x1": "2.5", "x2": "3.5", "ta": "0[s]", "selection": {"all": true}},
              {"type": "ohmic", "label": "Top", "name": "t", "V0": "0.5[V]", "selection": {"of": ["si"], "side": "top"}},
              {"type": "ohmic", "label": "Bottom", "name": "b", "selection": {"of": ["si"], "side": "bottom"}}]}],
            "mesh": {"size": "normal"}}],
          "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json").object();
        const Model m = modelOf(o, QStringLiteral("profiles.qfem"));
        const LumericalScript s = exported(m, QStringLiteral("lumericalcharge"), QJsonObject{{QStringLiteral("run"), true}});
        QVERIFY2(s.error.isEmpty(), qPrintable(s.error));
        QVERIFY(s.text.contains(QLatin1String("run(\"CHARGE\");")) && s.text.contains(QLatin1String("set(\"simulation temperature\", 310)")));
        const lum::Scene sc = lum::readScript(s.text, QStringLiteral("x.lsf"));
        QVERIFY2(sc.error.isEmpty(), qPrintable(sc.error));
        const lum::Object* d = object(sc, QStringLiteral("diffusion"), QStringLiteral("source"));
        QVERIFY(d);
        QVERIFY(near(d->number(QStringLiteral("x min")), 0.3e-6, 1e-9) && near(d->number(QStringLiteral("x max")), 1.7e-6, 1e-9));
        QVERIFY(near(d->number(QStringLiteral("z min")), 1.4e-6, 1e-9) && near(d->number(QStringLiteral("z max")), 2.2e-6, 1e-9));
        QVERIFY(near(d->number(QStringLiteral("junction width")), 0.2e-6, 1e-12));
        QVERIFY(near(d->number(QStringLiteral("ref concentration")), 1e21, 1e-12) && near(d->number(QStringLiteral("concentration")), 1e25, 1e-12));
        QCOMPARE(d->number(QStringLiteral("source face")), 4.0);
        // The implant: dose/(√(2π) σ)(e^(-(d - Rp)²/2σ²) + e^(-(d + Rp)²/2σ²)) under the surface, its window's edges erf's.
        const lum::Object* imp = object(sc, QStringLiteral("importdope"), QStringLiteral("implant"));
        QVERIFY(imp);
        const lum::Value* ds = imp->prop(QStringLiteral("@dataset"));
        QVERIFY(ds && ds->kind == lum::Value::Dataset);
        const lum::Value* x = ds->field(QStringLiteral("x"));
        const lum::Value* z = ds->field(QStringLiteral("z"));
        const lum::Value* N = ds->field(QStringLiteral("N"));
        QVERIFY(x && z && N);
        const std::size_t nx = x->m.size(), nz = z->m.size();
        QCOMPARE(N->m.size(), nx * 2 * nz);
        QCOMPARE(N->dims, (std::vector<int>{int(nx), 2, int(nz)}));
        auto implant = [](double px, double pz) {
            const double dose = 1e17, Rp = 0.2e-6, sd = 0.05e-6, sl = 0.08e-6, d0 = 2e-6 - pz;
            if (d0 < 0) return 0.0;
            const double depth = dose / (std::sqrt(2 * M_PI) * sd) * (std::exp(-(d0 - Rp) * (d0 - Rp) / (2 * sd * sd)) + std::exp(-(d0 + Rp) * (d0 + Rp) / (2 * sd * sd)));
            return depth * 0.5 * (std::erf((px - 2.5e-6) / (std::sqrt(2.0) * sl)) - std::erf((px - 3.5e-6) / (std::sqrt(2.0) * sl)));
        };
        double peak = 0;
        for (std::size_t k = 0; k < nz; ++k)
            for (std::size_t i = 0; i < nx; ++i) {
                const double want = implant(x->m[i], z->m[k]);
                peak = std::max(peak, want);
                for (int j = 0; j < 2; ++j) {
                    const double got = N->m[k * 2 * nx + std::size_t(j) * nx + i];
                    QVERIFY2(std::abs(got - want) <= 1e-9 * std::max(1.0, want), qPrintable(QStringLiteral("at %1, %2: %3 against %4").arg(x->m[i]).arg(z->m[k]).arg(got).arg(want)));
                }
            }
        QVERIFY(peak > 1e23);
        // Back: the box as it was, the implant said.
        QTemporaryDir dir;
        const LumericalImport im = imported(dir, s.text);
        QVERIFY2(im.error.isEmpty(), qPrintable(im.error));
        const QJsonObject box = named(features(im.model), QStringLiteral("label"), QStringLiteral("source"));
        QCOMPARE(box.value(QStringLiteral("profile")).toString(), QStringLiteral("box"));
        QCOMPARE(box.value(QStringLiteral("position")).toArray(), (QJsonArray{QStringLiteral("0.5"), QStringLiteral("1.6")}));
        QCOMPARE(box.value(QStringLiteral("size")).toArray(), (QJsonArray{QStringLiteral("1"), QStringLiteral("0.4")}));
        QCOMPARE(numberOf(box.value(QStringLiteral("dj"))), 0.2);
        QCOMPARE(numberOf(box.value(QStringLiteral("Nb"))), 1e21);
        QCOMPARE(named(features(im.model), QStringLiteral("label"), QStringLiteral("well")).value(QStringLiteral("profile")).toString(), QStringLiteral("uniform"));
        QVERIFY(named(features(im.model), QStringLiteral("label"), QStringLiteral("implant")).isEmpty());
        QVERIFY2(im.notes.join(QLatin1Char('\n')).contains(QLatin1String("implant: an imported doping profile")), qPrintable(im.notes.join(QLatin1Char('\n'))));
        QCOMPARE(numberOf(named(features(im.model), QStringLiteral("name"), QStringLiteral("t")).value(QStringLiteral("V0"))), 0.5);
        QCOMPARE(component(im.model).value(QStringLiteral("thickness")).toString(), QStringLiteral("10[um]"));
    }

    // MQW: a strained In0.2Ga0.8As well on GaAs - each layer's own
    // parameters for mqwindex (its gap, Kane's energy, masses, Luttinger's
    // γ, deformation potentials, stiffness, lattice, valence band), its
    // strain, the spectrum's photon frequencies, broadening (a full width),
    // the study's densities (over the stack) - and back.
    void aMqwScript()
    {
        const Model m = example(QStringLiteral("strained_laser_gain.qfem"));
        const LumericalScript s = exported(m, QStringLiteral("lumericalmqw"));
        QVERIFY2(s.error.isEmpty(), qPrintable(s.error));
        const lum::Scene sc = lum::readScript(s.text, QStringLiteral("x.lsf"));
        QVERIFY2(sc.error.isEmpty(), qPrintable(sc.error));
        QVERIFY(sc.stack.kind == lum::Value::Struct);
        QCOMPARE(sc.stack.field(QStringLiteral("length"))->m, (std::vector<double>{20e-9, 8e-9, 20e-9}));
        QCOMPARE(sc.stack.field(QStringLiteral("gamma"))->toNumber(), 0.0132);
        const BandParameters g = *bandParameters(QStringLiteral("GaAs"), 0, 0, 300), w = *bandParameters(QStringLiteral("InGaAs"), 0.2, 0, 300);
        const std::vector<double> strain = sc.stack.field(QStringLiteral("strain"))->m;
        QCOMPARE(strain[0], 0.0);
        QVERIFY(near(strain[1], (g.a0 - w.a0) / w.a0, 1e-9));
        const lum::Value& well = sc.stack.field(QStringLiteral("material"))->cell[1];
        auto f = [&](const char* k) { return well.field(QString::fromLatin1(k))->toNumber(); };
        QVERIFY(near(f("eg"), w.Eg, 1e-9) && near(f("vb"), w.VBO, 1e-9) && near(f("ep"), w.Ep, 1e-9) && near(f("me"), w.me, 1e-9));
        QVERIFY(near(f("gamma1"), w.gamma1, 1e-9) && near(f("gamma2"), w.gamma2, 1e-9) && near(f("gamma3"), w.gamma3, 1e-9));
        QVERIFY(near(f("ac"), w.ac, 1e-9) && near(f("av"), w.av, 1e-9) && near(f("b"), w.b, 1e-9));
        QVERIFY(near(f("c11"), w.C11, 1e-9) && near(f("c12"), w.C12, 1e-9) && near(f("lc"), w.a0, 1e-9) && near(f("eps"), w.epsr, 1e-9));
        QVERIFY(f("c11") > 1e11 && f("c11") < 1.3e11);
        // (The densities per the stack: the well's 8 of its 48 nm.)
        QCOMPARE(sc.params.field(QStringLiteral("cden"))->m.size(), std::size_t(4));
        for (int i = 0; i < 4; ++i) QVERIFY(near(sc.params.field(QStringLiteral("cden"))->m[std::size_t(i)], (i + 1) * 1e24 * 8 / 48, 1e-9));
        // (Wavelengths 900 to 1060 nm: their frequencies, not evenly spaced.)
        const std::vector<double> ph = sc.params.field(QStringLiteral("phfreq"))->m;
        QCOMPARE(ph.size(), std::size_t(161));
        QVERIFY(near(ph.front(), C0 / 900e-9, 1e-9) && near(ph.back(), C0 / 1060e-9, 1e-9));
        QCOMPARE(sc.params.field(QStringLiteral("T"))->toNumber(), 300.0);
        QCOMPARE(sc.params.field(QStringLiteral("V"))->m, (std::vector<double>{0, 48e-9, 0, 0}));
        QCOMPARE(sc.config.field(QStringLiteral("dz"))->toNumber(), 1e-10);
        QCOMPARE(sc.config.field(QStringLiteral("numeigenvalues"))->toNumber(), 24.0);
        QVERIFY(s.notes.join(QLatin1Char('\n')).contains(QLatin1String("holes parabolic")));
        // Back: each layer a Quantum Well Material of its own, the densities a sweep.
        QTemporaryDir dir;
        const LumericalImport im = imported(dir, s.text);
        QVERIFY2(im.error.isEmpty(), qPrintable(im.error));
        const QJsonObject wm = named(features(im.model), QStringLiteral("label"), QStringLiteral("layer 2's"));
        QVERIFY(near(numberOf(wm.value(QStringLiteral("Ec"))), w.VBO + w.Eg, 1e-9) && near(numberOf(wm.value(QStringLiteral("Ev"))), w.VBO, 1e-9));
        QVERIFY(near(numberOf(wm.value(QStringLiteral("mhh"))), 1 / (w.gamma1 - 2 * w.gamma2), 1e-9));
        QVERIFY(near(numberOf(wm.value(QStringLiteral("mlh"))), 1 / (w.gamma1 + 2 * w.gamma2), 1e-9));
        QVERIFY(near(numberOf(wm.value(QStringLiteral("exx"))), strain[1], 1e-9) && near(numberOf(wm.value(QStringLiteral("C11"))), w.C11, 1e-9));
        const QJsonObject study = im.model.value(QStringLiteral("studies")).toArray().at(0).toObject();
        const QJsonArray steps = study.value(QStringLiteral("steps")).toArray();
        const QJsonArray row = steps.at(0).toObject().value(QStringLiteral("parameters")).toArray().at(0).toArray();
        QCOMPARE(row.at(0).toString(), QStringLiteral("N"));
        const QStringList values = row.at(1).toString().split(QStringLiteral(", "));
        QCOMPARE(values.size(), 4);
        for (int i = 0; i < 4; ++i) QVERIFY(near(values[i].toDouble(), (i + 1) * 1e24, 1e-9));
        const QJsonObject spectrum = steps.at(1).toObject();
        QCOMPARE(spectrum.value(QStringLiteral("density")).toString(), QStringLiteral("N"));
        QCOMPARE(numberOf(spectrum.value(QStringLiteral("linewidth"))), 0.0066);
        QCOMPARE(spectrum.value(QStringLiteral("states")).toInt(), 6);
        QCOMPARE(spectrum.value(QStringLiteral("energies")).toString().count(QLatin1String("[eV]")), 161);
    }

    // The fields of a study's sweep: a solve and a file for each in a loop
    // (mqwindex at each field's potential, its middle 0) - read back a
    // sweep of them; the excitons asked; the line a Gaussian, said.
    void aMqwFieldSweep()
    {
        const Model m = example(QStringLiteral("gaas_eam_excitons.qfem"));
        const LumericalScript s = exported(m, QStringLiteral("lumericalmqw"));
        QVERIFY2(s.error.isEmpty(), qPrintable(s.error));
        QVERIFY(s.text.contains(QLatin1String("for (i = 1:length(fields)) {")) && s.text.contains(QLatin1String("savedata(\"x_mqw_\" + num2str(i)")));
        QVERIFY(s.notes.join(QLatin1Char('\n')).contains(QLatin1String("line gaussian")));
        const lum::Scene sc = lum::readScript(s.text, QStringLiteral("x.lsf"));
        QVERIFY2(sc.error.isEmpty(), qPrintable(sc.error));
        QCOMPARE(sc.fields.size(), std::size_t(3));
        for (int i = 0; i < 3; ++i) QVERIFY(std::abs(sc.fields[std::size_t(i)] - i * 5e6) < 1e-3);
        QCOMPARE(sc.config.field(QStringLiteral("exciton"))->toNumber(), 1.0);
        QTemporaryDir dir;
        const LumericalImport im = imported(dir, s.text);
        QVERIFY2(im.error.isEmpty(), qPrintable(im.error));
        const QJsonArray steps = im.model.value(QStringLiteral("studies")).toArray().at(0).toObject().value(QStringLiteral("steps")).toArray();
        QCOMPARE(steps.at(0).toObject().value(QStringLiteral("parameters")).toArray().at(0).toArray(), (QJsonArray{QStringLiteral("F"), QStringLiteral("0, 5000000, 10000000"), QStringLiteral("V/m")}));
        QVERIFY(steps.at(1).toObject().value(QStringLiteral("excitons")).toBool());
        QCOMPARE(component(im.model).value(QStringLiteral("physics")).toArray().at(0).toObject().value(QStringLiteral("F")).toString(), QStringLiteral("F"));
    }

    // Lumerical's database by compound and composition: GaAs, InGaAs at x
    // = 0.2 - read back as the compounds.
    void aMqwDatabase()
    {
        const Model m = example(QStringLiteral("strained_laser_gain.qfem"));
        const LumericalScript s = exported(m, QStringLiteral("lumericalmqw"), QJsonObject{{QStringLiteral("materials"), QStringLiteral("database")}});
        QVERIFY2(s.error.isEmpty(), qPrintable(s.error));
        QVERIFY(s.text.contains(QLatin1String("stack.material{2}.database_material = \"InGaAs\";")) && s.text.contains(QLatin1String("stack.material{2}.x = 0.2;")));
        QVERIFY(s.text.contains(QLatin1String("stack.material{1}.database_material = \"GaAs\";")));
        QVERIFY(!s.text.contains(QLatin1String(".eg =")));
        QTemporaryDir dir;
        const LumericalImport im = imported(dir, s.text);
        QVERIFY2(im.error.isEmpty(), qPrintable(im.error));
        const QJsonArray materials = component(im.model).value(QStringLiteral("materials")).toArray();
        const QJsonObject ingaas = named(materials, QStringLiteral("compound"), QStringLiteral("InGaAs"));
        QCOMPARE(ingaas.value(QStringLiteral("x")).toString(), QStringLiteral("0.2"));
        QVERIFY(!named(materials, QStringLiteral("compound"), QStringLiteral("GaAs")).isEmpty());
    }

    // What cannot be: CHARGE of a model with no semiconductor or of an
    // axisymmetric one, MQW of a 2D quantum physics, MODE of one with no
    // mode analysis.
    void exportsRefused()
    {
        QVERIFY(exported(example(QStringLiteral("soi_waveguide_modes.qfem")), QStringLiteral("lumericalcharge")).error.contains(QLatin1String("no Semiconductor")));
        QVERIFY(exported(example(QStringLiteral("pn_diode.qfem")), QStringLiteral("lumericalmode")).error.contains(QLatin1String("Mode Analysis")));
        const LumericalScript wire = exported(example(QStringLiteral("quantum_wire.qfem")), QStringLiteral("lumericalmqw"));
        QVERIFY2(wire.error.contains(QLatin1String("2D")), qPrintable(wire.error));
        // (An axisymmetric diode: Lumerical's cross-sections are planar here.)
        QJsonObject o = exampleJson(QStringLiteral("pn_diode.qfem"));
        QJsonArray comps = o.value(QStringLiteral("components")).toArray();
        QJsonObject c = comps.at(0).toObject();
        c.insert(QStringLiteral("space"), QStringLiteral("axisymmetric"));
        comps[0] = c;
        o.insert(QStringLiteral("components"), comps);
        QVERIFY(exported(modelOf(o), QStringLiteral("lumericalcharge")).error.contains(QLatin1String("axisymmetric")));
    }

    // --- The script's language ---------------------------------------------

    void theScriptsLanguage()
    {
        const lum::Scene sc = lum::readScript(QStringLiteral(R"lsf(# a comment; "not a text"
a = [1, 2; 3, 4];          # 2 x 2, column-major
b = a(2, 1) * 10 + a(3);   # 30 + 2
for (i = 1:3) { b = b + i; }
for (k = 0; k < 2; k = k + 1) { b = b * 2; }
if (b > 1000) { c = 1; } else if (b > 100) { c = 2; } else { c = 3; }
t = struct; t.u = cell(2); t.u{2} = struct; t.u{2}.v = linspace(0, 1, 5);
s = "it's" + ' "quoted"';
try { nosuchcommand(1); set("never", 1); } catch(err);
addrect; set("name", "r"); set("x", 1e-6); set("x span", 2e-6); set("material", "Si (Silicon) - Palik");
setnamed("r", "b", b); setnamed("r", "c", c); setnamed("r", "v", t.u{2}.v(5)); setnamed("r", "s", s);
r = 1:0.5:2; setnamed("r", "n", length(r));
z = reshape([1, 2, 3, 4, 5, 6], [2, 3]); setnamed("r", "z", z(2, 3));
setnamed("r", "e", err);
q = getnamed("r", "x span"); setnamed("r", "q", q * 2);
setnamed("r", "neg", -2^2 + 10 / 4 * 2);
plot(r, r); somethingunknown;
)lsf"),
                                             QStringLiteral("s.lsf"));
        QVERIFY2(sc.error.isEmpty(), qPrintable(sc.error));
        const lum::Object* r = object(sc, QStringLiteral("rect"), QStringLiteral("r"));
        QVERIFY(r);
        QCOMPARE(r->number(QStringLiteral("b")), 152.0);
        QCOMPARE(r->number(QStringLiteral("c")), 2.0);
        QCOMPARE(r->number(QStringLiteral("v")), 1.0);
        QCOMPARE(r->text(QStringLiteral("s")), QStringLiteral("it's \"quoted\""));
        QCOMPARE(r->number(QStringLiteral("n")), 3.0);
        QCOMPARE(r->number(QStringLiteral("z")), 6.0);
        QCOMPARE(r->number(QStringLiteral("q")), 4e-6);
        QCOMPARE(r->number(QStringLiteral("neg")), 1.0);
        QVERIFY(r->text(QStringLiteral("e")).contains(QLatin1String("nosuchcommand is not known")));
        QVERIFY(!r->prop(QStringLiteral("never")));
        QCOMPARE(int(r->history.size()), 13);
        QVERIFY2(sc.notes.join(QLatin1Char('\n')).contains(QLatin1String("somethingunknown: not read")), qPrintable(sc.notes.join(QLatin1Char('\n'))));
        // Its errors at their lines.
        QVERIFY(lum::readScript(QStringLiteral("a = 1;\nb = (2;\n"), QStringLiteral("e.lsf")).error.startsWith(QLatin1String("line 2:")));
        QVERIFY(lum::readScript(QStringLiteral("a = \"open\n"), QStringLiteral("e.lsf")).error.contains(QLatin1String("not ended")));
        QVERIFY(lum::readScript(QStringLiteral("x = [1, 2] + [1, 2, 3];"), QStringLiteral("e.lsf")).error.contains(QLatin1String("other sizes")));
        QVERIFY(lum::readScript(QStringLiteral("set(\"a\", 1);"), QStringLiteral("e.lsf")).error.contains(QLatin1String("nothing is selected")));
    }

    // --- Imports -------------------------------------------------------------

    // A MODE project (data/lumerical_mode.lms): its FDE region's cross-
    // section (its own rectangle not one of the structures), each material
    // its index at the wavelength - sampled data between its samples, a
    // dielectric's, an object defined one's -, PML on the sides its flags
    // say (layers of 12 of its steps inside the region), metal on the
    // others; the wavelength, trial modes, the index about, the sweep's
    // wavelengths; what was not read said - and the model solves.
    void aModeProject()
    {
        const LumericalImport im = importLumerical(QStringLiteral(QUCS_TEST_DATA "/lumerical_mode.lms"));
        QVERIFY2(im.error.isEmpty(), qPrintable(im.error));
        const QString notes = im.notes.join(QLatin1Char('\n'));
        QVERIFY2(notes.contains(QStringLiteral("an FDE region 4 x 3 µm (2D Z normal), 4 structures, 6 modes at 1550 nm")), qPrintable(notes));
        QVERIFY2(notes.contains(QLatin1String("ring: a structure of a kind not read")) && notes.contains(QLatin1String("mesh override")), qPrintable(notes));
        const QJsonObject c = component(im.model);
        const QJsonArray geometry = c.value(QStringLiteral("geometry")).toArray();
        const QJsonObject window = named(geometry, QStringLiteral("label"), QStringLiteral("window"));
        QCOMPARE(window.value(QStringLiteral("position")).toArray(), (QJsonArray{QStringLiteral("-2"), QStringLiteral("-1.4")}));
        QCOMPARE(window.value(QStringLiteral("size")).toArray(), (QJsonArray{QStringLiteral("4"), QStringLiteral("3")}));
        // (The substrate kept to the region.)
        QCOMPARE(named(geometry, QStringLiteral("label"), QStringLiteral("substrate")).value(QStringLiteral("size")).toArray(), (QJsonArray{QStringLiteral("4"), QStringLiteral("1.4")}));
        QVERIFY(named(geometry, QStringLiteral("label"), QStringLiteral("rectangle")).isEmpty());
        const QJsonObject core = named(geometry, QStringLiteral("label"), QStringLiteral("core"));
        QCOMPARE(core.value(QStringLiteral("position")).toArray(), (QJsonArray{QStringLiteral("-0.6"), QStringLiteral("0")}));
        QCOMPARE(core.value(QStringLiteral("size")).toArray(), (QJsonArray{QStringLiteral("1.2"), QStringLiteral("0.4")}));
        QCOMPARE(named(geometry, QStringLiteral("label"), QStringLiteral("pml_left")).value(QStringLiteral("size")).toArray(), (QJsonArray{QStringLiteral("0.24"), QStringLiteral("3")}));
        QVERIFY(!named(geometry, QStringLiteral("label"), QStringLiteral("pml_right")).isEmpty() && named(geometry, QStringLiteral("label"), QStringLiteral("pml_top")).isEmpty());
        const QJsonArray materials = c.value(QStringLiteral("materials")).toArray();
        const double f = C0 / 1.55e-6, f0 = C0 / 1.6e-6, f1 = C0 / 1.5e-6, t = (f - f0) / (f1 - f0);
        QVERIFY(near(numberOf(named(materials, QStringLiteral("label"), QStringLiteral("SiO2 (Glass) - Palik")).value(QStringLiteral("n"))), std::sqrt(2.10 + t * (2.08 - 2.10)), 1e-9));
        const std::complex<double> si = std::sqrt(std::complex<double>(12.0 + t * 0.2, 0.01 + t * 0.01));
        const QJsonObject sim = named(materials, QStringLiteral("label"), QStringLiteral("Si (Silicon) - Palik"));
        QVERIFY(near(numberOf(sim.value(QStringLiteral("n"))), si.real(), 1e-9) && near(numberOf(sim.value(QStringLiteral("kappa"))), si.imag(), 1e-9));
        QCOMPARE(numberOf(named(materials, QStringLiteral("label"), QStringLiteral("Si3N4 (Silicon Nitride) - Phillip")).value(QStringLiteral("n"))), 2.0);
        QCOMPARE(numberOf(named(materials, QStringLiteral("label"), QStringLiteral("clad's index")).value(QStringLiteral("n"))), 1.45);
        // (The strip drawn after the core: its own where they meet.)
        QVERIFY(materials.size() == 5 && materials.at(4).toObject().value(QStringLiteral("selection")).toObject().value(QStringLiteral("objects")).toArray().at(0) == QLatin1String("strip"));
        const QJsonObject step = im.model.value(QStringLiteral("studies")).toArray().at(0).toObject().value(QStringLiteral("steps")).toArray().at(0).toObject();
        QCOMPARE(step.value(QStringLiteral("modes")).toInt(), 6);
        QCOMPARE(step.value(QStringLiteral("neff")).toString(), QStringLiteral("1.9"));
        QVERIFY2(step.value(QStringLiteral("frequency")).toString().startsWith(QLatin1String("linspace(1.93414489e+14[Hz], 1.873702863e+14[Hz], 5)")), qPrintable(step.value(QStringLiteral("frequency")).toString()));
        // It solves: a mode guided - its index above the cladding's, below the strip's.
        QJsonObject o = withStep(im.model, QStringLiteral("mode"), QJsonObject{{QStringLiteral("frequency"), QStringLiteral("1550[nm]")}, {QStringLiteral("modes"), 2}});
        if (slowBuild()) {
            // (Meshed normal: its fine mesh's solve over 130 s under the sanitizers.)
            QJsonArray comps = o.value(QStringLiteral("components")).toArray();
            QJsonObject c = comps.at(0).toObject();
            c.insert(QStringLiteral("mesh"), QJsonObject{{QStringLiteral("size"), QStringLiteral("normal")}});
            comps[0] = c;
            o.insert(QStringLiteral("components"), comps);
        }
        const Model m = modelOf(o);
        const Solved r = solve(m);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const double neff = globals(r, QStringLiteral("emw.neff")).front();
        qInfo("its first mode's index %.5f", neff);
        QVERIFY(neff > 1.46 && neff < si.real());
    }

    // A CHARGE project (data/lumerical_charge.ldev): its geometry in the
    // region (the silicon kept to it, the spacer a polygon turned into the
    // x-z plane), its materials the library's by name or of their own, its
    // doping (per cm³: Lumerical's) - constant, a diffusion's plateau short
    // of its faces by the junction width but at its source face -, its
    // contacts - ohmic between a metal and the silicon, a gate on the oxide
    // and the spacer (its work function; kept: the boundary in its box does
    // not cover it - the oxide's top runs on past it), a range a sweep -,
    // its mesh constraint, temperature and norm length.
    void aChargeProject()
    {
        const LumericalImport im = importLumerical(QStringLiteral(QUCS_TEST_DATA "/lumerical_charge.ldev"));
        QVERIFY2(im.error.isEmpty(), qPrintable(im.error));
        const QString notes = im.notes.join(QLatin1Char('\n'));
        QVERIFY2(notes.contains(QStringLiteral("a CHARGE cross-section (x-z), 6 structures, 2 doping objects, 3 contacts, 320 K, norm length 2 µm")), qPrintable(notes));
        QVERIFY2(notes.contains(QLatin1String("surface_recombination (Surface Recombination): not read")) && notes.contains(QStringLiteral("per cm³")), qPrintable(notes));
        QVERIFY2(notes.contains(QLatin1String("HfO2 (Hafnium oxide) - Robertson not in the library")) && !notes.contains(QLatin1String("Silicon nitride")), qPrintable(notes));
        const QJsonObject c = component(im.model);
        QCOMPARE(c.value(QStringLiteral("thickness")).toString(), QStringLiteral("2[um]"));
        const QJsonArray geometry = c.value(QStringLiteral("geometry")).toArray();
        QCOMPARE(named(geometry, QStringLiteral("label"), QStringLiteral("silicon")).value(QStringLiteral("size")).toArray(), (QJsonArray{QStringLiteral("4"), QStringLiteral("2")}));
        QVERIFY(!named(geometry, QStringLiteral("label"), QStringLiteral("gate")).isEmpty());
        QCOMPARE(named(geometry, QStringLiteral("label"), QStringLiteral("spacer")).value(QStringLiteral("points")).toArray(),
                 (QJsonArray{QJsonArray{QStringLiteral("0.5"), QStringLiteral("0.02")}, QJsonArray{QStringLiteral("0.8"), QStringLiteral("0.02")}, QJsonArray{QStringLiteral("0.5"), QStringLiteral("0.12")}}));
        // (The source drawn after the silicon - its mesh order the lower: its own where they meet.)
        const QJsonArray materials = c.value(QStringLiteral("materials")).toArray();
        int si = -1, al = -1;
        for (int i = 0; i < materials.size(); ++i) {
            const QJsonObject mo = materials.at(i).toObject();
            if (mo.value(QStringLiteral("material")) == QLatin1String("Silicon")) si = i;
            if (mo.value(QStringLiteral("material")) == QLatin1String("Aluminium") && al < 0) al = i;
        }
        QVERIFY(si >= 0 && al > si);
        QCOMPARE(named(materials, QStringLiteral("label"), QStringLiteral("HfO2 (Hafnium oxide) - Robertson")).value(QStringLiteral("epsilonr")).toString(), QStringLiteral("25"));
        const QJsonObject physics = c.value(QStringLiteral("physics")).toArray().at(0).toObject();
        QCOMPARE(physics.value(QStringLiteral("T")).toString(), QStringLiteral("320[K]"));
        QCOMPARE(physics.value(QStringLiteral("selection")).toObject().value(QStringLiteral("except")).toObject().value(QStringLiteral("objects")).toArray(), (QJsonArray{QStringLiteral("source"), QStringLiteral("gate"), QStringLiteral("body")}));
        const QJsonArray fs = features(im.model);
        const QJsonObject well = named(fs, QStringLiteral("label"), QStringLiteral("pwell"));
        QCOMPARE(well.value(QStringLiteral("N0")).toString(), QStringLiteral("1e+23[1/m^3]"));
        QCOMPARE(well.value(QStringLiteral("selection")).toObject().value(QStringLiteral("objects")).toArray(), (QJsonArray{QStringLiteral("silicon")}));
        const QJsonObject n = named(fs, QStringLiteral("label"), QStringLiteral("source n+"));
        QCOMPARE(n.value(QStringLiteral("profile")).toString(), QStringLiteral("box"));
        QCOMPARE(n.value(QStringLiteral("dopant")).toString(), QStringLiteral("donor"));
        QCOMPARE(n.value(QStringLiteral("position")).toArray(), (QJsonArray{QStringLiteral("-2.1"), QStringLiteral("-0.2")}));
        QCOMPARE(n.value(QStringLiteral("size")).toArray(), (QJsonArray{QStringLiteral("1.4"), QStringLiteral("0.2")}));
        QCOMPARE(n.value(QStringLiteral("Nb")).toString(), QStringLiteral("1e+21[1/m^3]"));
        const QJsonObject source = named(fs, QStringLiteral("name"), QStringLiteral("source"));
        QCOMPARE(source.value(QStringLiteral("type")).toString(), QStringLiteral("ohmic"));
        QCOMPARE(source.value(QStringLiteral("selection")).toObject().value(QStringLiteral("between")).toArray(), (QJsonArray{QJsonArray{QStringLiteral("source")}, QJsonArray{QStringLiteral("silicon")}}));
        const QJsonObject gate = named(fs, QStringLiteral("name"), QStringLiteral("gate"));
        QCOMPARE(gate.value(QStringLiteral("type")).toString(), QStringLiteral("gate"));
        QCOMPARE(gate.value(QStringLiteral("phim")).toString(), QStringLiteral("5.17[eV]"));
        QCOMPARE(gate.value(QStringLiteral("V0")).toString(), QStringLiteral("V_gate"));
        QCOMPARE(gate.value(QStringLiteral("selection")).toObject().value(QStringLiteral("between")).toArray(),
                 (QJsonArray{QJsonArray{QStringLiteral("gate")}, QJsonArray{QStringLiteral("oxide"), QStringLiteral("spacer")}}));
        QCOMPARE(im.model.value(QStringLiteral("studies")).toArray().at(0).toObject().value(QStringLiteral("steps")).toArray().at(0).toObject().value(QStringLiteral("parameters")).toArray().at(0).toArray(),
                 (QJsonArray{QStringLiteral("V_gate"), QStringLiteral("-1, -0.5, 0, 0.5, 1"), QStringLiteral("V")}));
        const QJsonObject mesh = c.value(QStringLiteral("mesh")).toObject().value(QStringLiteral("features")).toArray().at(0).toObject();
        QCOMPARE(mesh.value(QStringLiteral("hmax")).toString(), QStringLiteral("0.005"));
        QCOMPARE(mesh.value(QStringLiteral("level")).toString(), QStringLiteral("boundary"));
        // Its selections resolve: the gate on the oxide's top under it and the
        // spacer's side, the source on the silicon it is sunk in, the body
        // under the silicon.
        const Model m = modelOfImport(im);
        const std::shared_ptr<ParameterScope> ps = evaluateParameters(m);
        const GeometryBuild g = buildGeometry(m, *ps);
        QVERIFY(g.ok());
        for (const char* contact : {"gate", "source", "body"}) {
            const QJsonObject f = named(fs, QStringLiteral("name"), QString::fromLatin1(contact));
            QString why;
            const QVector<int> b = resolveSelection(*g.topology, m, Level::Boundary, f.value(QStringLiteral("selection")).toObject(), &why);
            QVERIFY2(!b.isEmpty() && why.isEmpty(), contact);
            for (int bd : b) {
                const Topology::Boundary& B = g.topology->boundaries[std::size_t(bd)];
                for (QPointF p : B.polyline(*g.topology))
                    QVERIFY2(qstrcmp(contact, "gate") != 0 || (std::abs(p.y() - 0.02) < 1e-9 && std::abs(p.x()) <= 0.5 + 1e-9)
                                 || (std::abs(p.x() - 0.5) < 1e-9 && p.y() >= 0.02 - 1e-9 && p.y() <= 0.12 + 1e-9),
                             contact);
            }
        }
    }

    // The kinds it reads by their file; what it does not, said.
    void importsRefused()
    {
        QVERIFY(importLumerical(QStringLiteral(QUCS_TEST_DATA "/lumerical_sample.ldf")).error.contains(QLatin1String("data file")));
        QVERIFY(importLumerical(QStringLiteral(QUCS_TEST_DATA "/klayout_sample.oas")).error.contains(QLatin1String("not a Lumerical project")));
        QTemporaryDir dir;
        QFile cut(dir.filePath(QStringLiteral("cut.lms")));
        QFile whole(QStringLiteral(QUCS_TEST_DATA "/lumerical_mode.lms"));
        QVERIFY(whole.open(QIODevice::ReadOnly) && cut.open(QIODevice::WriteOnly));
        cut.write(whole.readAll().left(3000));
        cut.close();
        QVERIFY(importLumerical(cut.fileName()).error.contains(QLatin1String("do not read")));
        QVERIFY(imported(dir, QStringLiteral("addrect; set(\"x span\", 1e-6);")).error.contains(QLatin1String("makes no FDE region, CHARGE solver or mqwindex stack")));
        QVERIFY(imported(dir, QStringLiteral("addfde;\nset(\"x\", (1;")).error.startsWith(QLatin1String("line 2:")));
        // (The MQW project still the MQW importer's.)
        QVERIFY(importLumerical(QStringLiteral(QUCS_TEST_DATA "/lumerical_sample.ldev")).notes.value(0).contains(QLatin1String("3 layers")));
    }
};

QTEST_GUILESS_MAIN(TestFemLumerical)
#include "test_fem_lumerical.moc"
