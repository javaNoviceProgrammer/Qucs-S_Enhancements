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
#include "fem_results.h"
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
        {
            QSet<int> orders;
            int structures = 0;
            for (const lum::Object& so : sc.objects)
                if (so.kind == QLatin1String("rect") || so.kind == QLatin1String("poly")) {
                    ++structures;
                    orders.insert(int(so.number(QStringLiteral("mesh order"), -1)));
                }
            QCOMPARE(orders.size(), structures);
        }
        QCOMPARE(core->number(QStringLiteral("override mesh order from material database")), 1.0);
        // (Their indices at 1500 nm: the silicon's 3.48, the oxide's - the
        // library's silicon dioxide's there, Malitson's.)
        Scope at1500(&Scope::builtins());
        at1500.setConstant(QStringLiteral("lambda0"), 1.5e-6, Dim::length());
        const double nOxide = evaluateConstant(findMaterial(QStringLiteral("Silicon dioxide"))->properties.value(QStringLiteral("n")).toString(), at1500, Dim()).value_or(NAN);
        QVERIFY2(std::abs(nOxide - 1.4446) < 1e-4, qPrintable(QString::number(nOxide, 'g', 10)));
        QCOMPARE(int(sc.materials.size()), 2);
        for (const lum::Material& mat : sc.materials) {
            QCOMPARE(mat.type, QStringLiteral("(n,k) Material"));
            const double n = mat.n[0];
            QVERIFY2(mat.name.startsWith(QLatin1String("silicon n=")) ? std::abs(n - 3.48) < 1e-9 : std::abs(n - nOxide) < 1e-8, qPrintable(mat.name));
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

    // A crystal both ways: the library's lithium niobate (an x-cut wafer's,
    // y along z) in the SOI wire's place, absorbing along two of its axes
    // (κ 1e-4 along its x, 2e-4 along its z). Exported: an (n,k) material of
    // Lumerical's diagonal anisotropy - ne along x (its optic axis), no
    // along y and z, κ 2e-4, 1e-4, 0 - not Lumerical's Z-cut material.
    // Imported again: n and κ along each axis. Turned off the model's axes
    // (30° about z): its ε's diagonal, and a note.
    void aCrystalBothWays()
    {
        auto lnWire = [](const QString& orientation, const QString& oxideIndex = QString()) {
            QJsonObject o = soi();
            QJsonArray comps = o.value(QStringLiteral("components")).toArray();
            QJsonObject c = comps.at(0).toObject();
            QJsonArray mats = c.value(QStringLiteral("materials")).toArray();
            if (!oxideIndex.isEmpty()) {
                QJsonObject oxide = mats[0].toObject();
                oxide.insert(QStringLiteral("n"), oxideIndex);
                mats[0] = oxide;
            }
            QJsonObject ln{{QStringLiteral("label"), QStringLiteral("LN")}, {QStringLiteral("library"), QStringLiteral("Lithium niobate")},
                           {QStringLiteral("kappa"), QStringLiteral("1e-4; 0; 2e-4")},
                           {QStringLiteral("selection"), QJsonObject{{QStringLiteral("only"), QJsonArray{QStringLiteral("core")}}}}};
            if (!orientation.isEmpty()) {
                ln.insert(QStringLiteral("orientation"), QStringLiteral("euler"));
                ln.insert(QStringLiteral("euler_phi"), orientation);
            }
            mats[1] = ln;
            c.insert(QStringLiteral("materials"), mats);
            comps[0] = c;
            o.insert(QStringLiteral("components"), comps);
            return modelOf(withStep(o, QStringLiteral("mode"), QJsonObject{{QStringLiteral("frequency"), QStringLiteral("1550[nm]")}}));
        };
        const double l2 = 1.55 * 1.55;
        const double no = std::sqrt(1 + 2.6734 * l2 / (l2 - 0.01764) + 1.2290 * l2 / (l2 - 0.05914) + 12.614 * l2 / (l2 - 474.60));
        const double ne = std::sqrt(1 + 2.9804 * l2 / (l2 - 0.02047) + 0.5981 * l2 / (l2 - 0.0666) + 8.9543 * l2 / (l2 - 416.08));
        const LumericalScript s = exported(lnWire(QString()), QStringLiteral("lumericalmode"));
        QVERIFY2(s.error.isEmpty(), qPrintable(s.error));
        QVERIFY(!s.text.contains(QLatin1String("Z cut")));
        const lum::Scene sc = lum::readScript(s.text, QStringLiteral("x.lsf"));
        QVERIFY2(sc.error.isEmpty(), qPrintable(sc.error));
        const lum::Material* crystal = nullptr;
        for (const lum::Material& mat : sc.materials)
            if (mat.name.startsWith(QLatin1String("LN"))) crystal = &mat;
        QVERIFY2(crystal, qPrintable(s.text));
        qInfo("exported: %s - n %.6f, %.6f, %.6f; κ %.3g, %.3g, %.3g", qPrintable(crystal->name), crystal->n[0], crystal->n[1], crystal->n[2], crystal->k[0],
              crystal->k[1], crystal->k[2]);
        QVERIFY(std::abs(crystal->n[0] - ne) < 1e-6 && std::abs(crystal->n[1] - no) < 1e-6 && std::abs(crystal->n[2] - no) < 1e-6);
        QVERIFY(std::abs(crystal->k[0] - 2e-4) < 1e-9 && std::abs(crystal->k[1] - 1e-4) < 1e-9 && std::abs(crystal->k[2]) < 1e-12);
        QVERIFY(!s.notes.join(QLatin1Char('\n')).contains(QLatin1String("turned off")));
        // Imported again.
        QTemporaryDir dir;
        const LumericalImport back = imported(dir, s.text);
        QVERIFY2(back.error.isEmpty(), qPrintable(back.error));
        const QJsonObject f = named(features(back.model), QStringLiteral("label"), crystal->name);
        QVERIFY2(!f.isEmpty(), qPrintable(QJsonDocument(back.model).toJson(QJsonDocument::Compact)));
        QCOMPARE(f.value(QStringLiteral("anisotropy")).toString(), QStringLiteral("diagonal"));
        QVERIFY(std::abs(numberOf(f.value(QStringLiteral("nx"))) - ne) < 1e-6 && std::abs(numberOf(f.value(QStringLiteral("ny"))) - no) < 1e-6);
        const QStringList kappa = f.value(QStringLiteral("kappa")).toString().split(QLatin1Char(';'));
        QCOMPARE(kappa.size(), 3);
        QVERIFY(std::abs(kappa[0].toDouble() - 2e-4) < 1e-9 && std::abs(kappa[1].toDouble() - 1e-4) < 1e-9 && std::abs(kappa[2].toDouble()) < 1e-12);
        // Asked for Lumerical's own materials: still its own (n,k) one; the
        // oxide Lumerical's - but given an index along its axes, its own.
        const QJsonObject library{{QStringLiteral("materials"), QStringLiteral("library")}};
        const LumericalScript own = exported(lnWire(QString()), QStringLiteral("lumericalmode"), library);
        QVERIFY2(own.error.isEmpty() && !own.text.contains(QLatin1String("LiNbO3")) && own.text.contains(QLatin1String("\"Anisotropy\", 1")),
                 qPrintable(own.error + own.text));
        QVERIFY(own.text.contains(QLatin1String("SiO2 (Glass) - Palik")));
        const LumericalScript strained = exported(lnWire(QString(), QStringLiteral("1.444; 1.444; 1.46")), QStringLiteral("lumericalmode"), library);
        QVERIFY2(strained.error.isEmpty() && !strained.text.contains(QLatin1String("SiO2 (Glass) - Palik")), qPrintable(strained.error + strained.text));
        // (Turned 30° about z: its ε off the model's axes.)
        const LumericalScript t = exported(lnWire(QStringLiteral("30")), QStringLiteral("lumericalmode"));
        QVERIFY2(t.error.isEmpty(), qPrintable(t.error));
        QVERIFY2(t.notes.join(QLatin1Char('\n')).contains(QLatin1String("turned off the model's axes")), qPrintable(t.notes.join(QLatin1Char('\n'))));
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
    // charge over the sweep within 3 % of the model's largest (its mesh its
    // own).
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
        // (The model's mesh is not carried: Lumerical's is its own, the round
        // trip's "fine" - 1.3 % apart at 2 V, twice the charge at -2 V.)
        double largest = 0;
        for (double q : x) largest = std::max(largest, std::abs(q));
        for (std::size_t i = 0; i < n; ++i) {
            qInfo("Vg %d V: Q_g %.5g C, its round trip's %.5g", int(4 * i / (n - 1)) - 2, x[i], y[i]);
            QVERIFY2(std::abs(x[i] - y[i]) <= 0.03 * largest, qPrintable(QStringLiteral("%1 against %2").arg(x[i]).arg(y[i])));
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

    // An expression's doping sampled where it changes: a Gaussian 10.6 nm
    // wide peaking on a germanium layer's interface with the silicon under
    // it (the review's photodiode's p- layer) - its peak at the interface
    // sampled from inside the germanium, three samples a σ and more near
    // it, the grid's straight lines within 1.5 % of the peak everywhere;
    // nought in the silicon. Each structure its own mesh order.
    void anInterfaceDopingSampled()
    {
        const QJsonObject o = QJsonDocument::fromJson(R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "um",
            "geometry": [{"type": "rectangle", "label": "si", "size": ["2", "0.22"]},
                         {"type": "rectangle", "label": "ge", "position": ["0", "0.22"], "size": ["2", "0.5"]}, {"type": "form union"}],
            "materials": [{"material": "Silicon", "selection": {"objects": ["si"]}}, {"material": "Germanium", "selection": {"objects": ["ge"]}}],
            "physics": [{"type": "semiconductor", "tag": "semi", "features": [
              {"type": "doping", "label": "well", "dopant": "acceptor", "N0": "1e15[1/cm^3]", "selection": {"objects": ["si"]}},
              {"type": "doping", "label": "p-", "dopant": "acceptor", "profile": "expression",
               "N": "4e15[1/cm^3]*exp(-((y - 0.22[um])/10.6[nm])^2/2)", "selection": {"objects": ["ge"]}},
              {"type": "ohmic", "label": "Bottom", "name": "b", "selection": {"of": ["si"], "side": "bottom"}},
              {"type": "ohmic", "label": "Top", "name": "t", "selection": {"of": ["ge"], "side": "top"}}]}],
            "mesh": {"size": "normal"}}],
          "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json").object();
        const Model m = modelOf(o, QStringLiteral("interface.qfem"));
        const LumericalScript s = exported(m, QStringLiteral("lumericalcharge"));
        QVERIFY2(s.error.isEmpty(), qPrintable(s.error));
        const lum::Scene sc = lum::readScript(s.text, QStringLiteral("x.lsf"));
        QVERIFY2(sc.error.isEmpty(), qPrintable(sc.error));
        const lum::Object* imp = object(sc, QStringLiteral("importdope"), QStringLiteral("p-"));
        QVERIFY(imp);
        const lum::Value* ds = imp->prop(QStringLiteral("@dataset"));
        QVERIFY(ds && ds->kind == lum::Value::Dataset);
        const lum::Value* x = ds->field(QStringLiteral("x"));
        const lum::Value* z = ds->field(QStringLiteral("z"));
        const lum::Value* N = ds->field(QStringLiteral("N"));
        QVERIFY(x && z && N);
        const std::size_t nx = x->m.size(), nz = z->m.size();
        QCOMPARE(N->m.size(), nx * 2 * nz);
        const double peak = 4e21, sigma = 10.6e-9, z0 = 0.22e-6;
        auto profile = [&](double zz) { return zz < z0 ? 0.0 : peak * std::exp(-(zz - z0) * (zz - z0) / (2 * sigma * sigma)); };
        // (Along z at the middle column; x's lines alike.)
        const std::size_t col = nx / 2;
        auto sample = [&](std::size_t k) { return N->m[k * 2 * nx + col]; };
        double most = 0;
        int near3 = 0;
        for (std::size_t k = 0; k < nz; ++k) {
            most = std::max(most, sample(k));
            if (z->m[k] > z0 + 1e-12 && z->m[k] <= z0 + 3 * sigma) ++near3;
        }
        qInfo("the interface's doping: %zu x %zu points, its largest %.4g /m³ (the peak %.4g), %d lines within 3σ of the interface", nx, nz, most, peak, near3);
        QVERIFY2(std::abs(z->m.front() - z0) < 1e-12 && std::abs(sample(0) / peak - 1) < 1e-6, qPrintable(QString::number(sample(0))));
        QVERIFY2(near3 >= 9, qPrintable(QString::number(near3)));
        for (int i = 0; i <= 400; ++i) {
            const double zz = z0 + (0.72e-6 - z0) * i / 400;
            std::size_t k = 1;
            while (k + 1 < nz && z->m[k] < zz) ++k;
            const double t = (zz - z->m[k - 1]) / (z->m[k] - z->m[k - 1]), lin = sample(k - 1) + t * (sample(k) - sample(k - 1));
            QVERIFY2(std::abs(lin - profile(zz)) <= 0.015 * peak, qPrintable(QStringLiteral("at %1 nm: %2 against %3").arg(zz * 1e9).arg(lin).arg(profile(zz))));
        }
        // Each structure its own mesh order.
        QSet<int> orders;
        int structures = 0;
        for (const lum::Object& so : sc.objects)
            if (so.kind == QLatin1String("rect") || so.kind == QLatin1String("poly")) {
                ++structures;
                orders.insert(int(so.number(QStringLiteral("mesh order"), -1)));
            }
        QVERIFY(structures >= 2);
        QCOMPARE(orders.size(), structures);
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
    // dielectric's, an object defined one's (one, or along x, y, z), a
    // Sellmeier one's along each axis, a temperature dependent one's its
    // material's with dn/dT at the FDE's temperature -, a pyramid's cut, a
    // structure group's polygon and rectangle where its script left them
    // (from the group's place), each structure's mesh order its own or its
    // material's; PML on the sides its flags say (layers of 12 of its steps
    // inside the region), metal on the others; the wavelength, trial modes,
    // the index about, the sweep's wavelengths; what was not read said - and
    // the model solves.
    void aModeProject()
    {
        const LumericalImport im = importLumerical(QStringLiteral(QUCS_TEST_DATA "/lumerical_mode.lms"));
        QVERIFY2(im.error.isEmpty(), qPrintable(im.error));
        const QString notes = im.notes.join(QLatin1Char('\n'));
        QVERIFY2(notes.contains(QStringLiteral("an FDE region 4 x 3 µm (2D Z normal), 8 structures, 6 modes at 1550 nm")), qPrintable(notes));
        QVERIFY2(notes.contains(QLatin1String("a structure of a kind not read (7)")) && notes.contains(QLatin1String("mesh override")), qPrintable(notes));
        for (const char* said : {"its model's setup script not run", "LNOI: a structure group - its structures as its script made them",
                                 "temperature: a temperature import", "at the FDE's 310 K", "silicon_thermal: its free carriers' terms (Soref's) not taken",
                                 "LN_Sellmeier_MgO_Zel: its index of the wavelength, Lumerical's Sellmeier coefficients"})
            QVERIFY2(notes.contains(QString::fromUtf8(said)), said);
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
        // (Sampled data: its index of the wavelength - each sample's n and κ,
        // straight between them - read at 1550 nm.)
        auto at1550 = [](const QJsonValue& v) {
            Scope s(&Scope::builtins());
            s.setConstant(QStringLiteral("lambda0"), 1.55e-6, Dim::length());
            return evaluateConstant(v.toString(), s, Dim()).value_or(NAN);
        };
        const QJsonObject oxide = named(materials, QStringLiteral("label"), QStringLiteral("SiO2 (Glass) - Palik"));
        QVERIFY2(oxide.value(QStringLiteral("n")).toString().startsWith(QLatin1String("interp(lambda0")) && oxide.value(QStringLiteral("nrange")).toString() == QLatin1String("1.5[um] 1.6[um]"),
                 qPrintable(QJsonDocument(oxide).toJson(QJsonDocument::Compact)));
        QVERIFY(near(at1550(oxide.value(QStringLiteral("n"))), (std::sqrt(2.10) + std::sqrt(2.08)) / 2, 1e-6));
        const std::complex<double> si = std::sqrt(std::complex<double>(12.0 + t * 0.2, 0.01 + t * 0.01));
        const std::complex<double> si0 = std::sqrt(std::complex<double>(12.0, 0.01)), si1 = std::sqrt(std::complex<double>(12.2, 0.02));
        const QJsonObject sim = named(materials, QStringLiteral("label"), QStringLiteral("Si (Silicon) - Palik"));
        QVERIFY(near(at1550(sim.value(QStringLiteral("n"))), (si0.real() + si1.real()) / 2, 1e-6) && near(at1550(sim.value(QStringLiteral("kappa"))), (si0.imag() + si1.imag()) / 2, 1e-6));
        QCOMPARE(numberOf(named(materials, QStringLiteral("label"), QStringLiteral("Si3N4 (Silicon Nitride) - Phillip")).value(QStringLiteral("n"))), 2.0);
        QCOMPARE(numberOf(named(materials, QStringLiteral("label"), QStringLiteral("clad's index")).value(QStringLiteral("n"))), 1.45);
        // (Drawn the highest mesh order first, the objects' order between
        // equals: the substrate's own 5 first, then the material's 2s, the
        // core's own 1 after them, the pad's and the warm one's last.)
        QStringList drawn;
        for (const QJsonValue& v : materials) drawn << v.toObject().value(QStringLiteral("selection")).toObject().value(QStringLiteral("objects")).toArray().at(0).toString();
        QCOMPARE(drawn, (QStringList{QStringLiteral("window"), QStringLiteral("substrate"), QStringLiteral("clad"), QStringLiteral("strip"), QStringLiteral("pedestal"),
                                     QStringLiteral("ridge"), QStringLiteral("core"), QStringLiteral("pad"), QStringLiteral("warm")}));
        // The pyramid cut halfway up: its spans halfway between its foot's and its top's.
        QCOMPARE(named(geometry, QStringLiteral("label"), QStringLiteral("pedestal")).value(QStringLiteral("position")).toArray(), (QJsonArray{QStringLiteral("1.05"), QStringLiteral("0.4")}));
        QCOMPARE(named(geometry, QStringLiteral("label"), QStringLiteral("pedestal")).value(QStringLiteral("size")).toArray(), (QJsonArray{QStringLiteral("0.3"), QStringLiteral("0.2")}));
        QCOMPARE(numberOf(named(materials, QStringLiteral("label"), QStringLiteral("pedestal's index")).value(QStringLiteral("n"))), 1.6);
        // The group's: from its place, (-1.4, 0.6) µm.
        QCOMPARE(named(geometry, QStringLiteral("label"), QStringLiteral("ridge")).value(QStringLiteral("points")).toArray(),
                 (QJsonArray{QJsonArray{QStringLiteral("-1.7"), QStringLiteral("0.5")}, QJsonArray{QStringLiteral("-1.1"), QStringLiteral("0.5")},
                             QJsonArray{QStringLiteral("-1.25"), QStringLiteral("0.7")}, QJsonArray{QStringLiteral("-1.55"), QStringLiteral("0.7")}}));
        QCOMPARE(named(geometry, QStringLiteral("label"), QStringLiteral("pad")).value(QStringLiteral("position")).toArray(), (QJsonArray{QStringLiteral("-1.6"), QStringLiteral("0.2")}));
        QCOMPARE(named(geometry, QStringLiteral("label"), QStringLiteral("pad")).value(QStringLiteral("size")).toArray(), (QJsonArray{QStringLiteral("0.4"), QStringLiteral("0.2")}));
        const QJsonArray fs = features(im.model);
        // The ridge's own index along x, y, z ("2.2;2.1;2.2").
        const QJsonObject ridge = named(fs, QStringLiteral("label"), QStringLiteral("ridge's index"));
        QCOMPARE(ridge.value(QStringLiteral("anisotropy")).toString(), QStringLiteral("diagonal"));
        QVERIFY(numberOf(ridge.value(QStringLiteral("nx"))) == 2.2 && numberOf(ridge.value(QStringLiteral("ny"))) == 2.1 && numberOf(ridge.value(QStringLiteral("nz"))) == 2.2);
        // The pad's LN: Zelmon's n² = 1 + Σ A λ²/(λ² - B) along each axis.
        auto zelmon = [](double a1, double b1, double a2, double b2, double a3, double b3) {
            const double l2 = 1.55 * 1.55;
            return std::sqrt(1 + a1 * l2 / (l2 - b1) + a2 * l2 / (l2 - b2) + a3 * l2 / (l2 - b3));
        };
        const double no = zelmon(2.2454, 0.01242, 1.3005, 0.05313, 6.8972, 331.33), ne = zelmon(2.4272, 0.01478, 1.4617, 0.05612, 9.6536, 371.216);
        const QJsonObject ln = named(fs, QStringLiteral("label"), QStringLiteral("LN_Sellmeier_MgO_Zel"));
        QVERIFY2(near(at1550(ln.value(QStringLiteral("nx"))), no, 1e-9) && near(at1550(ln.value(QStringLiteral("ny"))), no, 1e-9) && near(at1550(ln.value(QStringLiteral("nz"))), ne, 1e-9),
                 qPrintable(QJsonDocument(ln).toJson(QJsonDocument::Compact)));
        // The warm one: the silicon's index and dn/dT from 300 K, at the FDE's 310 K.
        const QJsonObject warm = named(fs, QStringLiteral("label"), QStringLiteral("silicon_thermal"));
        QVERIFY(near(numberOf(warm.value(QStringLiteral("n"))), si.real(), 1e-9) && near(numberOf(warm.value(QStringLiteral("kappa"))), si.imag(), 1e-9));
        QVERIFY(warm.value(QStringLiteral("thermooptic")).toBool());
        QCOMPARE(warm.value(QStringLiteral("dndT")).toString(), QStringLiteral("0.00018[1/K]"));
        QCOMPARE(warm.value(QStringLiteral("T")).toString(), QStringLiteral("310[K]"));
        QCOMPARE(warm.value(QStringLiteral("Tref")).toString(), QStringLiteral("300[K]"));
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
        QVERIFY2(notes.contains(QStringLiteral("per cm³")), qPrintable(notes));
        QVERIFY2(notes.contains(QLatin1String("surface_recombination: its two materials not given (surface type 6): left out")), qPrintable(notes));
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

    // A DEVICE 7.17 project (data/lumerical_device.ldev): its materials the
    // project's own material objects, each with its models - the silicon's
    // Varshni gap, its masses' Nc and Nv, χ from its work function (to the
    // intrinsic level), its lattice's mobilities of T, Masetti's by doping,
    // saturation (each carrier's β), band gap narrowing, SRH (Fossum's
    // doping dependence, Hurkx's tunnelling, its trap), Auger, impact
    // ionization, band-to-band tunnelling; one off where its group is -; a
    // pyramid of germanium a trapezoid, a diffusion box as wide as its
    // junction kept (no plateau), surface recombination between the silicon
    // and the oxide (not at the metal), the imported generation a table of x
    // and y times its scale factor, the gate's work function its own, the
    // current of the contact not swept plotted.
    void aDeviceProject()
    {
        const LumericalImport im = importLumerical(QStringLiteral(QUCS_TEST_DATA "/lumerical_device.ldev"));
        QVERIFY2(im.error.isEmpty(), qPrintable(im.error));
        const QString notes = im.notes.join(QLatin1Char('\n'));
        QVERIFY2(notes.contains(QStringLiteral("a CHARGE cross-section (x-z), 3 structures, 2 doping objects, 2 contacts, 300 K, norm length 1 µm")), qPrintable(notes));
        QVERIFY2(notes.contains(QStringLiteral("Eg 0.66 eV (constant)")) && notes.contains(QStringLiteral("1 monitors left out")) && !notes.contains(QLatin1String("edges 0")),
                 qPrintable(notes));
        // The silicon: Eg y0 - αT²/(T + β), Nc and Nv 2.509e19 m*^1.5 cm⁻³, χ W - Eg/2 - kT/2 ln(Nc/Nv).
        const double T = 300, kT = 8.617333262e-5 * T, Eg = 1.169 - 4.73e-4 * T * T / (T + 636);
        const double Nc = 2.509e19 * std::pow(1.09, 1.5), Nv = 2.509e19 * std::pow(1.15, 1.5), chi = 4.2 - Eg / 2 - kT / 2 * std::log(Nc / Nv);
        const QJsonArray materials = component(im.model).value(QStringLiteral("materials")).toArray();
        const QJsonObject si = named(materials, QStringLiteral("label"), QStringLiteral("Si (Silicon)"));
        QVERIFY2(near(numberOf(si.value(QStringLiteral("Eg"))), Eg, 1e-9) && near(numberOf(si.value(QStringLiteral("chi"))), chi, 1e-9), qPrintable(QJsonDocument(si).toJson()));
        QVERIFY(near(numberOf(si.value(QStringLiteral("Nc"))), Nc, 1e-9) && near(numberOf(si.value(QStringLiteral("Nv"))), Nv, 1e-9));
        QCOMPARE(si.value(QStringLiteral("mun")).toString(), QStringLiteral("1471[cm^2/(V*s)]"));
        QCOMPARE(si.value(QStringLiteral("mup")).toString(), QStringLiteral("470.5[cm^2/(V*s)]"));
        QCOMPARE(si.value(QStringLiteral("taun")).toString(), QStringLiteral("3e-06[s]"));
        QCOMPARE(si.value(QStringLiteral("taup")).toString(), QStringLiteral("1e-06[s]"));
        QCOMPARE(si.value(QStringLiteral("epsilonr")).toString(), QStringLiteral("11.7"));
        QCOMPARE(named(materials, QStringLiteral("label"), QStringLiteral("Ge (Germanium)")).value(QStringLiteral("Eg")).toString(), QStringLiteral("0.66[eV]"));
        const QJsonArray fs = features(im.model);
        const QJsonObject m = named(fs, QStringLiteral("label"), QStringLiteral("Si (Silicon) (Lumerical's models)"));
        const std::pair<const char*, const char*> model[] = {
            {"mobility", "masetti"}, {"m1n", "52.2[cm^2/(V*s)]"}, {"m2n", "52.2[cm^2/(V*s)]"}, {"mu1n", "43.4[cm^2/(V*s)]"}, {"pcn", "0[1/cm^3]"},
            {"crn", "9.68e+16[1/cm^3]"}, {"csn", "3.43e+20[1/cm^3]"}, {"man", "0.68"}, {"mbn", "2"}, {"m1p", "44.9[cm^2/(V*s)]"}, {"mu1p", "29[cm^2/(V*s)]"},
            {"crp", "2.23e+17[1/cm^3]"}, {"csp", "6.1e+20[1/cm^3]"}, {"map", "0.719"}, {"betan", "2"}, {"betap", "1"}, {"vsatn", "10700000[cm/s]"},
            {"vsatp", "8370000[cm/s]"}, {"driving", "current"}, {"Ebgn", "0.00692[eV]"}, {"Nbgn", "1.3e+17[1/cm^3]"}, {"zetan", "2.42"}, {"zetap", "2.2"}};
        for (const auto& [k, v] : model) QVERIFY2(m.value(QLatin1String(k)).toString() == QLatin1String(v), k);
        QVERIFY(m.value(QStringLiteral("saturation")).toBool() && m.value(QStringLiteral("bgn")).toBool() && !m.contains(QStringLiteral("fermi")));
        const QJsonObject srh = named(fs, QStringLiteral("label"), QStringLiteral("Si (Silicon): SRH"));
        const std::pair<const char*, const char*> srhs[] = {{"Et", "0.1[eV]"}, {"dependence", "doping"}, {"N0n", "7.1e+15[1/cm^3]"}, {"N0p", "5e+15[1/cm^3]"},
                                                            {"fA", "1"}, {"fB", "0"}, {"fC", "1"}, {"fE", "1"}, {"mtn", "0.25"}, {"mtp", "0.3"}};
        for (const auto& [k, v] : srhs) QVERIFY2(srh.value(QLatin1String(k)).toString() == QLatin1String(v), k);
        QVERIFY(srh.value(QStringLiteral("tat")).toBool());
        const QJsonObject auger = named(fs, QStringLiteral("label"), QStringLiteral("Si (Silicon): Auger"));
        QVERIFY(auger.value(QStringLiteral("Cn")).toString() == QLatin1String("2.8e-31[cm^6/s]") && auger.value(QStringLiteral("Cp")).toString() == QLatin1String("9.9e-32[cm^6/s]"));
        QVERIFY(named(fs, QStringLiteral("type"), QStringLiteral("radiative")).isEmpty());   // (its group off)
        const QJsonObject ii = named(fs, QStringLiteral("label"), QStringLiteral("Si (Silicon): impact ionization"));
        QVERIFY(ii.value(QStringLiteral("an")).toString() == QLatin1String("703000[1/cm]") && ii.value(QStringLiteral("bp")).toString() == QLatin1String("2036000[V/cm]"));
        QVERIFY(ii.value(QStringLiteral("ap")).toString() == QLatin1String("1582000[1/cm]") && ii.value(QStringLiteral("betan")).toString() == QLatin1String("1"));
        const QJsonObject bt = named(fs, QStringLiteral("type"), QStringLiteral("btbt"));
        QVERIFY(bt.value(QStringLiteral("A")).toString() == QLatin1String("4e+14") && bt.value(QStringLiteral("B")).toString() == QLatin1String("19000000[V/cm]"));
        // The pyramid: its trapezoid where y = 0 cuts it.
        const QJsonArray geometry = component(im.model).value(QStringLiteral("geometry")).toArray();
        QCOMPARE(named(geometry, QStringLiteral("label"), QStringLiteral("ge")).value(QStringLiteral("points")).toArray(),
                 (QJsonArray{QJsonArray{QStringLiteral("0.75"), QStringLiteral("0")}, QJsonArray{QStringLiteral("1.75"), QStringLiteral("0")},
                             QJsonArray{QStringLiteral("1.55"), QStringLiteral("0.3")}, QJsonArray{QStringLiteral("0.95"), QStringLiteral("0.3")}}));
        // The diffusion box as wide as its junction: kept, its plateau none
        // (its box short of its faces by the junction width, but at its source face).
        const QJsonObject pp = named(fs, QStringLiteral("label"), QStringLiteral("ge p++"));
        QCOMPARE(pp.value(QStringLiteral("profile")).toString(), QStringLiteral("box"));
        QCOMPARE(pp.value(QStringLiteral("position")).toArray(), (QJsonArray{QStringLiteral("0.97"), QStringLiteral("0.3")}));
        QCOMPARE(pp.value(QStringLiteral("size")).toArray(), (QJsonArray{QStringLiteral("0.56"), QStringLiteral("0")}));
        QCOMPARE(pp.value(QStringLiteral("dj")).toString(), QStringLiteral("0.02[um]"));
        // Surface recombination between the silicon and the oxide, by their
        // material objects' names; at the metal left out.
        const QJsonObject sr = named(fs, QStringLiteral("type"), QStringLiteral("surfacerec"));
        QVERIFY(sr.value(QStringLiteral("Sn")).toString() == QLatin1String("100[m/s]") && sr.value(QStringLiteral("Sp")).toString() == QLatin1String("200[m/s]"));
        QCOMPARE(sr.value(QStringLiteral("Et")).toString(), QStringLiteral("0.05[eV]"));
        QCOMPARE(sr.value(QStringLiteral("selection")).toObject().value(QStringLiteral("between")).toArray(),
                 (QJsonArray{QJsonArray{QStringLiteral("body")}, QJsonArray{QStringLiteral("oxide")}}));
        QVERIFY2(notes.contains(QLatin1String("surface recombination at the metal contacts left out (at metal)")), qPrintable(notes));
        // The generation: its slice a table of x and y (µm), times 2 mW; off, as saved.
        const QJsonObject g = im.model.value(QStringLiteral("functions")).toArray().at(0).toObject();
        QCOMPARE(g.value(QStringLiteral("name")).toString(), QStringLiteral("G_gen"));
        QCOMPARE(g.value(QStringLiteral("dimension")).toString(), QStringLiteral("2"));
        const QJsonArray grid = g.value(QStringLiteral("grid")).toArray();
        QCOMPARE(grid.size(), 6);
        bool found = false;
        for (const QJsonValue& r : grid) {
            const QJsonArray row = r.toArray();
            if (near(row.at(0).toDouble(), 1.25, 1e-9) && near(row.at(1).toDouble(), 0.3, 1e-9)) found = near(row.at(2).toDouble(), 4e27 * 2e-3, 1e-9);
        }
        QVERIFY(found);
        const QJsonObject gen = named(fs, QStringLiteral("type"), QStringLiteral("generation"));
        QVERIFY(!gen.value(QStringLiteral("enabled")).toBool(true) && gen.value(QStringLiteral("G")).toString().startsWith(QLatin1String("G_gen(x, y)")));
        // The gate its metal's work function; the substrate's current plotted.
        QCOMPARE(named(fs, QStringLiteral("type"), QStringLiteral("gate")).value(QStringLiteral("phim")).toString(), QStringLiteral("3.64[eV]"));
        QCOMPARE(named(fs, QStringLiteral("name"), QStringLiteral("substrate")).value(QStringLiteral("type")).toString(), QStringLiteral("ohmic"));
        const QJsonArray plots = im.model.value(QStringLiteral("results")).toObject().value(QStringLiteral("plots")).toArray();
        QCOMPARE(named(plots, QStringLiteral("type"), QStringLiteral("plotgroup1d")).value(QStringLiteral("plots")).toArray().at(0).toObject().value(QStringLiteral("expression")).toString(),
                 QStringLiteral("semi.I_substrate"));
    }

    // The work functions' convention: Lumerical's semiconductor's to its
    // intrinsic level, its metals' to their Fermi level - the imported
    // device's MOS stack flat at Lumerical's flat band voltage, W_gate -
    // (W_si + kT ln(NA/ni)): its gate's charge nought there, against what
    // half a volt either side puts on it. (Taken as Qucs-S's χ, 4.05 eV,
    // the flat band would be 0.41 V away; a Lumerical MOSFET's threshold so.)
    void aMosFlatBand()
    {
        const LumericalImport im = importLumerical(QStringLiteral(QUCS_TEST_DATA "/lumerical_device.ldev"));
        QVERIFY2(im.error.isEmpty(), qPrintable(im.error));
        const double T = 300, kT = 8.617333262e-5 * T, Eg = 1.169 - 4.73e-4 * T * T / (T + 636);
        const double Nc = 2.509e19 * std::pow(1.09, 1.5), Nv = 2.509e19 * std::pow(1.15, 1.5), ni = std::sqrt(Nc * Nv) * std::exp(-Eg / (2 * kT));
        const double vfb = 3.64 - (4.2 + kT * std::log(1e17 / ni));
        QJsonArray rows;
        rows.append(QJsonArray{QStringLiteral("V_gate"), QStringLiteral("%1, %2, %3").arg(vfb - 0.5, 0, 'g', 10).arg(vfb, 0, 'g', 10).arg(vfb + 0.5, 0, 'g', 10), QStringLiteral("V")});
        const Solved r = solve(modelOf(withStep(im.model, QStringLiteral("sweep"), QJsonObject{{QStringLiteral("parameters"), rows}})));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const std::vector<double> q = globals(r, QStringLiteral("semi.Q_gate"));
        QCOMPARE(q.size(), std::size_t(3));
        qInfo("flat band at %.4f V: the gate's charge %.4g C there, %.4g and %.4g half a volt either side", vfb, q[1], q[0], q[2]);
        QVERIFY(q[0] < 0 && q[2] > 0);
        QVERIFY2(std::abs(q[1]) < 0.03 * std::min(std::abs(q[0]), std::abs(q[2])), qPrintable(QString::number(q[1] / q[2])));
    }

    // A HEAT project (data/lumerical_heat.ldev): its materials' thermal
    // records (a conductivity of T), its fluid not solved, a temperature on
    // the region's bottom face, convection between the oxide and the air,
    // a heat source over the norm length; solved - a heater layer the
    // region's width, nearly all its heat down: its rise q (t_ox/k_ox +
    // t_si/k_si), q its power over the width and the norm length.
    void aHeatProject()
    {
        const LumericalImport im = importLumerical(QStringLiteral(QUCS_TEST_DATA "/lumerical_heat.ldev"));
        QVERIFY2(im.error.isEmpty(), qPrintable(im.error));
        const QString notes = im.notes.join(QLatin1Char('\n'));
        QVERIFY2(notes.contains(QStringLiteral("a HEAT cross-section (x-z), 4 structures, norm length 1 µm")) && notes.contains(QLatin1String("its fluids (air) not solved")),
                 qPrintable(notes));
        const QJsonArray materials = component(im.model).value(QStringLiteral("materials")).toArray();
        QCOMPARE(named(materials, QStringLiteral("label"), QStringLiteral("Si (Silicon)")).value(QStringLiteral("k")).toString(), QStringLiteral("148[W/(m*K)]"));
        QCOMPARE(named(materials, QStringLiteral("label"), QStringLiteral("SiO2 (Glass) - Sze")).value(QStringLiteral("k")).toString(), QStringLiteral("1.4[W/(m*K)]*(T/300[K])^(0.1)"));
        QCOMPARE(named(materials, QStringLiteral("label"), QStringLiteral("Si (Silicon)")).value(QStringLiteral("Cp")).toString(), QStringLiteral("711[J/(kg*K)]"));
        const QJsonObject physics = component(im.model).value(QStringLiteral("physics")).toArray().at(0).toObject();
        QCOMPARE(physics.value(QStringLiteral("selection")).toObject().value(QStringLiteral("except")).toObject().value(QStringLiteral("objects")).toArray(), (QJsonArray{QStringLiteral("air")}));
        const QJsonArray fs = physics.value(QStringLiteral("features")).toArray();
        QCOMPARE(named(fs, QStringLiteral("label"), QStringLiteral("sink")).value(QStringLiteral("T0")).toString(), QStringLiteral("300[K]"));
        const QJsonObject top = named(fs, QStringLiteral("label"), QStringLiteral("top"));
        QCOMPARE(top.value(QStringLiteral("h")).toString(), QStringLiteral("10[W/(m^2*K)]"));
        QCOMPARE(top.value(QStringLiteral("selection")).toObject().value(QStringLiteral("between")).toArray(), (QJsonArray{QJsonArray{QStringLiteral("box")}, QJsonArray{QStringLiteral("air")}}));
        QCOMPARE(named(fs, QStringLiteral("label"), QStringLiteral("heater")).value(QStringLiteral("P0")).toString(), QStringLiteral("1e-05[W]"));
        const Solved r = solve(modelOfImport(im));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const Solution& sol = *r.study.solutions.back();
        const Locator locator(sol.mesh());
        const std::optional<double> Tw = evaluateAt(sol, locator, QStringLiteral("T"), QPointF(0, 1.6));
        const double q = 1e-5 / (10e-6 * 1e-6), expected = 300 + q * (1.5e-6 / 1.4 + 5e-6 / 148);
        qInfo("HEAT: the heater at %.5f K (%.5f: its rise q (t_ox/k_ox + t_si/k_si))", Tw.value_or(NAN), expected);
        QVERIFY2(Tw && std::abs(*Tw - expected) < 3e-3 * (expected - 300), qPrintable(QString::number(Tw.value_or(NAN))));
    }

    // The kinds it reads by their file; what it does not, said.
    void importsRefused()
    {
        QVERIFY(importLumerical(QStringLiteral(QUCS_TEST_DATA "/lumerical_sample.ldf")).error.contains(QLatin1String("data file")));
        QVERIFY(importLumerical(QStringLiteral(QUCS_TEST_DATA "/klayout_sample.oas")).error.contains(QLatin1String("not a Lumerical project")));
        QTemporaryDir dir;
        // (Cut short in its materials, in its objects.)
        QFile whole(QStringLiteral(QUCS_TEST_DATA "/lumerical_mode.lms"));
        QVERIFY(whole.open(QIODevice::ReadOnly));
        const QByteArray bytes = whole.readAll();
        for (const auto& [at, said] : {std::pair{qsizetype(3000), "its material 5 does not read"}, std::pair{bytes.size() - 600, "its objects do not read"}}) {
            QFile cut(dir.filePath(QStringLiteral("cut.lms")));
            QVERIFY(cut.open(QIODevice::WriteOnly));
            cut.write(bytes.left(at));
            cut.close();
            const QString error = importLumerical(cut.fileName()).error;
            QVERIFY2(error.contains(QLatin1String(said)), qPrintable(error));
        }
        QVERIFY(imported(dir, QStringLiteral("addrect; set(\"x span\", 1e-6);")).error.contains(QLatin1String("makes no FDE region, CHARGE solver or mqwindex stack")));
        QVERIFY(imported(dir, QStringLiteral("addfde;\nset(\"x\", (1;")).error.startsWith(QLatin1String("line 2:")));
        // (The MQW project still the MQW importer's.)
        QVERIFY(importLumerical(QStringLiteral(QUCS_TEST_DATA "/lumerical_sample.ldev")).notes.value(0).contains(QLatin1String("3 layers")));
    }
};

QTEST_GUILESS_MAIN(TestFemLumerical)
#include "test_fem_lumerical.moc"
