/*
 * test_fem_modeoverlap.cpp - a mode analysis's modes overlapped (with each
 *                            other, a Gaussian beam, another model's mode
 *                            from a file) and exported on a grid (MATLAB,
 *                            NumPy, CSV)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_export.h"
#include "fem_modeprofile.h"
#include "fem_problem.h"
#include "fem_results.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QRandomGenerator>
#include <QElapsedTimer>
#include <QtTest>

#include <cmath>
#include <complex>
#include <cstring>
#include <map>
#include <memory>

using namespace qucs_s::fem;

namespace {

using cd = std::complex<double>;

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
    StudyRun study;
    QString error;
    Solution& solution() const { return *study.solutions.back(); }
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
    r.study = runStudy(model, r.parameters, g.topology, nullptr, QString(), {});
    if (!r.study.ok()) r.error = r.study.error;
    return r;
}

/// A derived node's value \a name at snapshot \a k.
double derived(Solution& s, const Model& m, const QString& tag, const QString& name, int k = 0, QString* error = nullptr)
{
    s.select(k);
    const DerivedResult d = evaluateDerived(s, *m.find(tag));
    if (error) *error = d.error;
    for (const DerivedValue& v : d.values)
        if (v.name == name) return v.value;
    return NAN;
}

/// A real matrix of a .mat (column-major).
struct MatVar {
    QByteArray name;
    std::vector<double> values;
    quint32 rows, cols;
};

/// A .mat as MATLAB's save -v6 writes real ones: each a miMATRIX of
/// doubles, a name of 4 characters or fewer in the small format.
QByteArray matOf(const std::vector<MatVar>& vars)
{
    QByteArray b = QByteArray("MATLAB 5.0 MAT-file, written by hand").leftJustified(116, ' ');
    b += QByteArray(8, '\0');
    b += QByteArray("\x00\x01IM", 4);
    auto u32 = [](QByteArray& o, quint32 v) {
        for (int k = 0; k < 4; ++k) o += char((v >> (8 * k)) & 0xff);
    };
    for (const MatVar& v : vars) {
        QByteArray m;
        u32(m, 6), u32(m, 8), u32(m, 6), u32(m, 0);   // (mxDOUBLE_CLASS, real)
        u32(m, 5), u32(m, 8), u32(m, v.rows), u32(m, v.cols);
        if (v.name.size() <= 4) {
            u32(m, quint32(v.name.size()) << 16 | 1);
            m += v.name.leftJustified(4, '\0');
        } else {
            u32(m, 1), u32(m, quint32(v.name.size()));
            m += v.name;
            while (m.size() % 8) m += '\0';
        }
        u32(m, 9), u32(m, quint32(v.values.size() * 8));
        for (double d : v.values) {
            quint64 bits = 0;
            std::memcpy(&bits, &d, 8);
            for (int k = 0; k < 8; ++k) m += char((bits >> (8 * k)) & 0xff);
        }
        u32(b, 14), u32(b, quint32(m.size()));
        b += m;
    }
    return b;
}

/// The mode physics' order: 1 on the sanitizers' builds (their sparse LU
/// some hundred times slower).
QString order()
{
    return slowBuild() ? QStringLiteral("1") : QStringLiteral("2");
}

/// A silicon wire 500 by 220 nm in oxide, a window 3 µm square (its sides
/// conductors), its three highest modes at 1550 nm; \a derived its Derived
/// Values, \a exports its Export nodes.
QByteArray wire(const QString& derived, const QString& exports = QString())
{
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "components": [{"unit": "um",
        "geometry": [{"type": "rectangle", "name": "oxide", "position": ["-1.5", "-1.5"], "size": ["3", "3"]},
                     {"type": "rectangle", "name": "core", "position": ["-0.25", "-0.11"], "size": ["0.5", "0.22"]}],
        "materials": [{"label": "oxide", "n": "1.444", "selection": {"only": ["oxide"]}},
                      {"label": "silicon", "n": "3.476", "selection": {"only": ["core"]}}],
        "physics": [{"type": "modes", "tag": "emw", "order": "%4"}],
        "mesh": {"size": "fine", "features": [{"type": "size", "level": "domain", "selection": {"only": ["core"]}, "hmax": "%2"},
                                              {"type": "size", "level": "domain", "selection": {"only": ["oxide"]}, "hmax": "%3"}]}}],
      "studies": [{"type": "study", "steps": [{"type": "mode", "frequency": "1550[nm]", "modes": 3, "neff": "2.8"}]}],
      "results": {"derived": [%1], "exports": [%5]}})json")
        .arg(derived, slowBuild() ? QStringLiteral("0.05") : QStringLiteral("0.025"), slowBuild() ? QStringLiteral("0.3") : QStringLiteral("0.12"),
             order(), exports)
        .toUtf8();
}

/// A step-index fibre, its core 4.1 µm in radius (1.4504) in a cladding
/// (1.4447) 25 µm in radius: V 2.14 at 1550 nm, its LP01 (HE11, both
/// polarizations) found about 1.448.
QByteArray fibre(const QString& derived)
{
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "components": [{"unit": "um",
        "geometry": [{"type": "circle", "name": "cladding", "radius": "25"}, {"type": "circle", "name": "core", "radius": "4.1"}],
        "materials": [{"label": "cladding", "n": "1.4447", "selection": {"only": ["cladding"]}},
                      {"label": "core", "n": "1.4504", "selection": {"only": ["core"]}}],
        "physics": [{"type": "modes", "tag": "emw", "order": "%4"}],
        "mesh": {"size": "normal", "features": [{"type": "size", "level": "domain", "selection": {"only": ["core"]}, "hmax": "%2"},
                                                {"type": "size", "level": "domain", "selection": {"only": ["cladding"]}, "hmax": "%3"}]}}],
      "studies": [{"type": "study", "steps": [{"type": "mode", "frequency": "1550[nm]", "modes": 2, "neff": "1.449"}]}],
      "results": {"derived": [%1]}})json")
        .arg(derived, slowBuild() ? QStringLiteral("0.6") : QStringLiteral("0.3"), slowBuild() ? QStringLiteral("3") : QStringLiteral("1.5"), order())
        .toUtf8();
}

/// A temporary file's path.
QString tempFile(const char* name)
{
    return QDir(QDir::tempPath()).filePath(QString::fromLatin1(name));
}

/// The fibre's V at 1550 nm, and Marcuse's Gaussian width for it (m): w/a
/// = 0.65 + 1.619 V^-1.5 + 2.879 V^-6.
double fibreV()
{
    return 2 * M_PI / 1.55e-6 * 4.1e-6 * std::sqrt(1.4504 * 1.4504 - 1.4447 * 1.4447);
}
double marcuseWidth()
{
    const double V = fibreV();
    return 4.1e-6 * (0.65 + 1.619 / std::pow(V, 1.5) + 2.879 / std::pow(V, 6));
}

/// A model and its solution, solved once for the functions that evaluate
/// it (each its own Derived Values, on the same solution).
struct Solved {
    Model model;
    Run run;
};

Solved& solvedOnce(const QString& name, const QByteArray& json)
{
    static std::map<QString, std::unique_ptr<Solved>> solved;
    std::unique_ptr<Solved>& s = solved[name];
    if (!s) {
        s = std::make_unique<Solved>(Solved{modelOf(json), Run{}});
        s->run = run(s->model);
    }
    return *s;
}

/// The wire: its modes with each other, moved; with its own profile from a
/// file; its Mode Profile export.
Solved& theWire()
{
    return solvedOnce(QStringLiteral("wire"),
                      wire(QStringLiteral(R"({"type": "overlap", "tag": "o11", "with": "mode", "other": 1},
                                             {"type": "overlap", "tag": "o12", "with": "mode", "other": 2},
                                             {"type": "overlap", "tag": "o13", "with": "mode", "other": 3},
                                             {"type": "overlap", "tag": "o11m", "with": "mode", "other": 1, "offset": ["0.1", "0"]},
                                             {"type": "overlap", "tag": "o11n", "with": "mode", "other": 1, "offset": ["-0.1", "0"]},
                                             {"type": "overlap", "tag": "of", "with": "file", "file": "%1"})")
                               .arg(tempFile("qucs_test_wire_mode.mat")),
                           QStringLiteral(R"({"type": "modeprofile", "tag": "mp", "solutions": "all", "nx": 121, "ny": 101})")));
}

/// The fibre: with Marcuse's Gaussian along x and y, centred away and moved
/// back; with itself moved; with Gaussians from files.
Solved& theFibre()
{
    const QString waist = QString::number(marcuseWidth() * 1e6, 'g', 10) + QStringLiteral("[um]");
    return solvedOnce(QStringLiteral("fibre"),
                      fibre(QStringLiteral(R"({"type": "overlap", "tag": "gx", "with": "gaussian", "waist": "%1", "polarization": "x"},
                                              {"type": "overlap", "tag": "gy", "with": "gaussian", "waist": "%1", "polarization": "y"},
                                              {"type": "overlap", "tag": "gc", "with": "gaussian", "waist": "%1", "centre": ["2", "0"], "offset": ["-2", "0"]},
                                              {"type": "overlap", "tag": "s1", "with": "mode", "other": 1, "offset": ["2", "0"]},
                                              {"type": "overlap", "tag": "s2", "with": "mode", "other": 1, "offset": ["0", "3"]},
                                              {"type": "overlap", "tag": "gf", "with": "file", "file": "%2"},
                                              {"type": "overlap", "tag": "ge", "with": "file", "file": "%3"},
                                              {"type": "overlap", "tag": "gb", "with": "gaussian", "waist": "4.9[um]", "index": "1.4447"})")
                                .arg(waist, tempFile("qucs_test_gaussian.mat"), tempFile("qucs_test_gaussian_e.mat"))));
}

} // namespace

class TestFemModeOverlap : public QObject
{
    Q_OBJECT

private slots:
    // The wire's modes overlapped with each other: each with itself 1, with
    // another nought (lossless modes, power orthogonal); moved, less.
    void modesWithEachOther()
    {
        Solved& w = theWire();
        QVERIFY2(w.run.error.isEmpty(), qPrintable(w.run.error));
        const Model& m = w.model;
        Solution& s = w.run.solution();
        QString why;
        const double self = derived(s, m, QStringLiteral("o11"), QStringLiteral("overlap"), 0, &why);
        QVERIFY2(why.isEmpty(), qPrintable(why));
        const double selfE = derived(s, m, QStringLiteral("o11"), QStringLiteral("overlapE"));
        const double cross2 = derived(s, m, QStringLiteral("o12"), QStringLiteral("overlap")), cross3 = derived(s, m, QStringLiteral("o13"), QStringLiteral("overlap"));
        const double right = derived(s, m, QStringLiteral("o11m"), QStringLiteral("overlap")), left = derived(s, m, QStringLiteral("o11n"), QStringLiteral("overlap"));
        // Mode 2's with mode 1: the same as mode 1's with mode 2.
        s.select(1);
        Node back = *m.find(QStringLiteral("o12"));
        back.props.insert(QStringLiteral("other"), 1);
        const DerivedResult rb = evaluateDerived(s, back);
        const double cross21 = rb.values.isEmpty() ? NAN : rb.values.front().value;
        qInfo("mode 1 with itself %.12f (|E| alone %.12f), with mode 2 %.3g (and 2 with 1 %.3g), 3 %.3g; moved 100 nm right %.6f, left %.6f", self,
              selfE, cross2, cross21, cross3, right, left);
        QVERIFY2(std::abs(self - 1) < 1e-9 && std::abs(selfE - 1) < 1e-9, qPrintable(QString::number(self - 1)));
        QVERIFY2(std::abs(cross2) < 1e-6 && std::abs(cross3) < 1e-6 && std::abs(cross21) < 1e-6, qPrintable(QString::number(cross2)));
        QVERIFY(right < 0.99 && right > 0.5);
        // (The guide symmetric, its mesh not quite: 4e-4 apart here, 1.5e-3
        // on the sanitizers' coarser one.)
        QVERIFY2(std::abs(right / left - 1) < 5e-3, qPrintable(QString::number(right / left)));
        // (Its selection kept: the mode it was at.)
        QCOMPARE(s.selected(), 1);
    }

    // A lossy wire's mode (its core's κ 0.1): its fields complex, not one
    // phase - with itself still 1, each H conjugated as it must be.
    void aLossyModeWithItself()
    {
        QByteArray json = wire(QStringLiteral(R"({"type": "overlap", "tag": "o11", "with": "mode", "other": 1})"));
        json.replace(R"("n": "3.476")", R"("n": "3.476", "kappa": "0.1")");
        const Model m = modelOf(json);
        const Run r = run(m);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = r.solution();
        QString why;
        const double self = derived(s, m, QStringLiteral("o11"), QStringLiteral("overlap"), 0, &why);
        QVERIFY2(why.isEmpty(), qPrintable(why));
        s.select(0);
        const double loss = s.global(QStringLiteral("emw.loss")).value_or(NAN);
        qInfo("a lossy wire's mode 1 (%.4g dB/cm) with itself %.12f", loss / 100, self);
        QVERIFY2(loss > 100, qPrintable(QString::number(loss)));
        QVERIFY2(std::abs(self - 1) < 1e-9, qPrintable(QString::number(self - 1)));
    }

    // A step-index fibre's LP01 against Marcuse's Gaussian ("Gaussian
    // approximation of the fundamental modes of graded-index fibers", JOSA
    // 68, 103 (1978); "Loss analysis of single-mode fiber splices", BSTJ 56,
    // 703 (1977)): its width w/a = 0.65 + 1.619 V^-1.5 + 2.879 V^-6, the
    // power it couples into the pair of HE11 (each polarization one, mixed
    // as the solve finds them: the two modes' summed) near 1, and a splice
    // offset by d losing as two Gaussians do, exp(-d²/w²).
    void aFibreAgainstMarcuse()
    {
        const double V = fibreV(), w = marcuseWidth();
        Solved& f = theFibre();
        QVERIFY2(f.run.error.isEmpty(), qPrintable(f.run.error));
        const Model& m = f.model;
        Solution& s = f.run.solution();
        QString why;
        const double g1 = derived(s, m, QStringLiteral("gx"), QStringLiteral("overlap"), 0, &why), g2 = derived(s, m, QStringLiteral("gx"), QStringLiteral("overlap"), 1);
        QVERIFY2(why.isEmpty(), qPrintable(why));
        const double d2 = derived(s, m, QStringLiteral("s1"), QStringLiteral("overlap"), 0), d3 = derived(s, m, QStringLiteral("s2"), QStringLiteral("overlap"), 0);
        s.select(0);
        const double n1 = s.global(QStringLiteral("emw.neff")).value_or(NAN);
        s.select(1);
        const double n2 = s.global(QStringLiteral("emw.neff")).value_or(NAN);
        const double e2 = std::exp(-std::pow(2e-6 / w, 2)), e3 = std::exp(-std::pow(3e-6 / w, 2));
        qInfo("V %.4f, w %.4f um; HE11 %.7f, %.7f; an x Gaussian into them %.5f + %.5f = %.5f; moved 2 um %.5f (Marcuse %.5f), 3 um %.5f (%.5f)", V,
              w * 1e6, n1, n2, g1, g2, g1 + g2, d2, e2, d3, e3);
        QVERIFY2(g1 + g2 > 0.99 && g1 + g2 < 1, qPrintable(QString::number(g1 + g2)));
        // (Weakly guiding: H nearly (neff/η0) z × E, the power's overlap
        // nearly E's.)
        const double ge = derived(s, m, QStringLiteral("gx"), QStringLiteral("overlapE"), 0) + derived(s, m, QStringLiteral("gx"), QStringLiteral("overlapE"), 1);
        qInfo("E alone: %.6f", ge);
        QVERIFY2(std::abs(ge / (g1 + g2) - 1) < 1e-3, qPrintable(QString::number(ge / (g1 + g2))));
        // Along y the same into the pair; its centre moved back by the
        // offset: as it was.
        const double y1 = derived(s, m, QStringLiteral("gy"), QStringLiteral("overlap"), 0), y2 = derived(s, m, QStringLiteral("gy"), QStringLiteral("overlap"), 1);
        const double c1 = derived(s, m, QStringLiteral("gc"), QStringLiteral("overlap"), 0);
        qInfo("a y Gaussian %.5f + %.5f = %.5f; centred 2 um right, moved 2 um left %.8f (unmoved %.8f)", y1, y2, y1 + y2, c1, g1);
        QVERIFY2(std::abs((y1 + y2) / (g1 + g2) - 1) < 1e-3, qPrintable(QString::number(y1 + y2)));
        QVERIFY2(std::abs(c1 / g1 - 1) < 1e-12, qPrintable(QString::number(c1 / g1 - 1)));
        // (First-order elements on the sanitizers' builds: 1 % from it at 2 µm.)
        const double near = slowBuild() ? 0.02 : 0.01;
        QVERIFY2(std::abs(d2 / e2 - 1) < near && std::abs(d3 / e3 - 1) < 0.02, qPrintable(QString("%1 %2").arg(d2 / e2).arg(d3 / e3)));
    }

    // The wire's modes exported (Mode Profile): the .mat read back holds the
    // fields as they are at its grid's points, neff, λ0, its number, about
    // 1 W within the grid; a Mode Overlap with the file, the mode with its
    // own profile, 1 but for the grid's interpolation. NumPy's .npz a zip of
    // its arrays, CSV a row per point.
    void aProfileExportedAndReadBack()
    {
        Solved& w = theWire();
        QVERIFY2(w.run.error.isEmpty(), qPrintable(w.run.error));
        const Model& m = w.model;
        const SolutionSet set{w.run.study.solutions, {}, {}};
        const ExportText e = exportText(m, set, *m.find(QStringLiteral("mp")), QStringLiteral("27.0.2"));
        QVERIFY2(e.error.isEmpty(), qPrintable(e.error));
        QCOMPARE(e.suffix, QStringLiteral("mat"));
        qInfo("%s", qPrintable(e.summary));
        QVERIFY(e.summary.contains(QStringLiteral("[121 101 3]")));
        Solution& s = w.run.solution();
        double worst = 0;
        for (int k = 1; k <= 3; ++k) {
            QString why;
            const std::optional<ModeProfile> p = readProfileMat(e.data, k, &why);
            QVERIFY2(p, qPrintable(why));
            s.select(k - 1);
            const ModeProfile direct = sampleMode(s, s.fieldIndex(QStringLiteral("emw")), {}, p->x, p->y);
            for (int c = 0; c < 6; ++c)
                for (std::size_t i = 0; i < direct.F[std::size_t(c)].size(); ++i)
                    worst = std::max(worst, std::abs(p->F[std::size_t(c)][i] - direct.F[std::size_t(c)][i]));
            const double n = s.global(QStringLiteral("emw.neff")).value_or(NAN);
            qInfo("mode %d: neff %.8f (read %.8f), lambda0 %.4g, %.5f W within its grid", k, n, p->neff.real(), p->lambda0, 0.5 * p->flux().real());
            QCOMPARE(p->mode, k);
            QVERIFY(std::abs(p->neff.real() - n) < 1e-12 && std::abs(p->lambda0 - 1.55e-6) < 1e-18);
            // (The trapezoid rule across E's jumps: 1.017 W for the TM.)
            QVERIFY2(std::abs(0.5 * p->flux().real() - 1) < 0.05, qPrintable(QString::number(0.5 * p->flux().real())));
            QCOMPARE(p->x.size(), std::size_t(121));
            QCOMPARE(p->y.size(), std::size_t(101));
            QVERIFY(std::abs(p->x.front() + 1.5e-6) < 1e-15 && std::abs(p->y.back() - 1.5e-6) < 1e-15);
        }
        QVERIFY2(worst == 0, qPrintable(QString::number(worst)));
        // The mode with its own profile, read from the file: 1 but for the
        // grid's interpolation of E's jump at the core's sides (0.994 at 25
        // by 30 nm here, 0.986 with the sanitizers' first-order elements;
        // meshes differ between platforms).
        QFile out(tempFile("qucs_test_wire_mode.mat"));
        QVERIFY(out.open(QIODevice::WriteOnly) && out.write(e.data) == e.data.size());
        out.close();
        QString why;
        const double own = derived(s, m, QStringLiteral("of"), QStringLiteral("overlap"), 0, &why);
        QVERIFY2(why.isEmpty(), qPrintable(why));
        qInfo("the mode with its own profile (121 by 101): %.6f", own);
        QVERIFY2(own > (slowBuild() ? 0.97 : 0.99) && own < 1.000001, qPrintable(QString::number(own)));
        out.remove();
        // NumPy's: a zip, its arrays named; CSV: a row per point and mode.
        Node npz = *m.find(QStringLiteral("mp"));
        npz.props.insert(QStringLiteral("format"), QStringLiteral("npz"));
        const ExportText z = exportText(m, set, npz, QStringLiteral("27.0.2"));
        QVERIFY2(z.error.isEmpty() && z.suffix == QStringLiteral("npz") && z.data.startsWith("PK\x03\x04"), qPrintable(z.error));
        for (const char* name : {"x.npy", "y.npy", "Ex.npy", "Hz.npy", "neff.npy", "lambda0.npy", "mode.npy", "power.npy", "about.npy"})
            QVERIFY2(z.data.contains(name), name);
        QVERIFY(z.data.contains("'descr': '<c16', 'fortran_order': False, 'shape': (3, 101, 121), }"));
        Node csv = npz;
        csv.props.insert(QStringLiteral("format"), QStringLiteral("csv"));
        const ExportText c = exportText(m, set, csv, QStringLiteral("27.0.2"));
        QVERIFY2(c.error.isEmpty() && c.suffix == QStringLiteral("csv"), qPrintable(c.error));
        const QStringList lines = c.text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        int rows = 0;
        for (const QString& l : lines)
            if (!l.startsWith(QLatin1Char('#')) && !l.startsWith(QLatin1String("mode,"))) ++rows;
        QCOMPARE(rows, 3 * 121 * 101);
        QVERIFY(c.text.contains(QStringLiteral("mode,x (m),y (m),Re Ex (V/m),Im Ex (V/m)")));
        // One column: refused, said - a cross-section, which its overlap reads
        // two by two at the least (it was made 2 without a word, while a Field
        // on a Grid takes one column: the hunt of 22 October, N13).
        Node column = *m.find(QStringLiteral("mp"));
        column.props.insert(QStringLiteral("nx"), 1);
        const ExportText one = exportText(m, set, column, QStringLiteral("27.0.2"));
        QVERIFY2(one.error.startsWith(QLatin1String("1 by 101 points: a mode's profile is a cross-section - from 2 to 4000 along x and y")) && one.data.isEmpty(),
                 qPrintable(one.error));
    }

    // A profile whose "about" is UTF-32 (MATLAB's miUTF32): a character
    // past 16 bits read as UTF-16's two, one past Unicode's as the
    // replacement. (Cast to char16_t, undefined - the fuzz of 21 October,
    // P1.)
    void aUtf32AboutIsRead()
    {
        ModeProfile g;
        g.x = {0, 1e-6, 2e-6};
        g.y = {0, 1e-6};
        for (auto& f : g.F) f.assign(6, 0.0);
        g.F[0].assign(6, 1.0);
        g.neff = 1.5;
        g.mode = 1;
        QByteArray mat = profilesMat({g}, QStringLiteral("first"));
        // "about" again, after the first: a char array 1 by 3 in miUTF32.
        auto u32 = [](QByteArray& b, quint32 v) {
            for (int k = 0; k < 4; ++k) b.append(char((v >> (8 * k)) & 0xff));
        };
        QByteArray sub;
        for (quint32 v : {6u, 8u, 4u, 0u}) u32(sub, v);   // its flags: mxCHAR
        for (quint32 v : {5u, 8u, 1u, 3u}) u32(sub, v);   // 1 by 3
        for (quint32 v : {1u, 5u}) u32(sub, v);           // its name
        sub += QByteArray("about") + QByteArray(3, '\0');
        for (quint32 v : {18u, 12u, quint32('A'), 0x1f600u, 0x110000u, 0u}) u32(sub, v);   // miUTF32
        u32(mat, 14);
        u32(mat, quint32(sub.size()));
        mat += sub;
        QString why;
        const std::optional<ModeProfile> p = readProfileMat(mat, 1, &why);
        QVERIFY2(p, qPrintable(why));
        QCOMPARE(p->label, QString(u"A\U0001F600\uFFFD"));
    }

    // A Gaussian written as a file (its E and H a plane wave's in the
    // cladding) and read back: the fibre's mode with it as with the Mode
    // Overlap's own Gaussian - and a file of E alone, its H made from neff.
    void aGaussianFromAFile()
    {
        const double w = 4.9e-6, n = 1.4447;
        ModeProfile g;
        for (int i = 0; i <= 160; ++i) g.x.push_back(-20e-6 + 40e-6 * i / 160);
        for (int j = 0; j <= 140; ++j) g.y.push_back(-20e-6 + 40e-6 * j / 140);
        for (auto& f : g.F) f.assign(g.x.size() * g.y.size(), 0.0);
        for (std::size_t j = 0; j < g.y.size(); ++j)
            for (std::size_t i = 0; i < g.x.size(); ++i) {
                const double e = std::exp(-(g.x[i] * g.x[i] + g.y[j] * g.y[j]) / (w * w));
                g.F[0][j * g.x.size() + i] = e;
                g.F[4][j * g.x.size() + i] = n / 376.730313412 * e;
            }
        g.neff = n;
        g.mode = 1;
        const QString path = tempFile("qucs_test_gaussian.mat");
        QFile out(path);
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(profilesMat({g}, QStringLiteral("a Gaussian")));
        out.close();
        // E alone, real (as MATLAB writes it) and y the first index (as
        // SciPy saves a NumPy (ny, nx) array): turned round, its H a plane
        // wave's of its neff - the Gaussian's own -, its number its place.
        const quint32 nx = quint32(g.nx()), ny = quint32(g.ny());
        std::vector<double> ex(g.F[0].size());
        for (std::size_t j = 0; j < ny; ++j)
            for (std::size_t i = 0; i < nx; ++i) ex[i * ny + j] = g.F[0][j * nx + i].real();
        const QByteArray eOnly =
            matOf({{"x", g.x, nx, 1}, {"y", g.y, ny, 1}, {"Ex", ex, ny, nx}, {"Ey", std::vector<double>(ex.size()), ny, nx}, {"neff", {n}, 1, 1}});
        QString why;
        const std::optional<ModeProfile> e = readProfileMat(eOnly, 1, &why);
        QVERIFY2(e.has_value(), qPrintable(why));
        QCOMPARE(e->mode, 1);
        QCOMPARE(e->neff, cd(n));
        double most = 0;
        for (std::size_t k = 0; k < ex.size(); ++k)
            most = std::max({most, std::abs(e->F[0][k] - g.F[0][k]), std::abs(e->F[4][k] - g.F[4][k]), std::abs(e->F[3][k]), std::abs(e->F[5][k])});
        QVERIFY2(most < 1e-15, qPrintable(QString::number(most)));
        const QString ePath = tempFile("qucs_test_gaussian_e.mat");
        QFile eOut(ePath);
        QVERIFY(eOut.open(QIODevice::WriteOnly));
        eOut.write(eOnly);
        eOut.close();
        Solved& f = theFibre();
        QVERIFY2(f.run.error.isEmpty(), qPrintable(f.run.error));
        const Model& m = f.model;
        Solution& s = f.run.solution();
        double file = 0, fileE = 0, built = 0;
        for (int k = 0; k < 2; ++k) {
            file += derived(s, m, QStringLiteral("gf"), QStringLiteral("overlap"), k, &why);
            QVERIFY2(why.isEmpty(), qPrintable(why));
            fileE += derived(s, m, QStringLiteral("ge"), QStringLiteral("overlap"), k, &why);
            QVERIFY2(why.isEmpty(), qPrintable(why));
            built += derived(s, m, QStringLiteral("gb"), QStringLiteral("overlap"), k);
        }
        qInfo("the HE11 pair with a Gaussian of 4.9 um: from the file %.6f, of E alone %.6f, its own %.6f", file, fileE, built);
        QVERIFY2(std::abs(file / built - 1) < 2e-3, qPrintable(QString::number(file / built)));
        QVERIFY2(std::abs(fileE / file - 1) < 1e-12, qPrintable(QString::number(fileE / file - 1)));
        QFile::remove(path);
        QFile::remove(ePath);
    }

    // The .mat reader's fuzzer (QUCS_PROFILE_FUZZ seconds,
    // QUCS_PROFILE_FUZZ_SEED): profiles of any grid (one point, one row,
    // fields not numbers) written by profilesMat, profilesNpz and
    // profilesCsv, then damaged - bytes flipped, cut, repeated, sizes and
    // types written over with hostile numbers - and read back as any of its
    // modes, each read one asked its fields at hostile points and its flux.
    // The current file in QUCS_FUZZ_OUT/profile-current.mat.
    void fuzz()
    {
        const int seconds = qEnvironmentVariableIntValue("QUCS_PROFILE_FUZZ");
        if (seconds <= 0) QSKIP("set QUCS_PROFILE_FUZZ to a number of seconds");
        QRandomGenerator rng(quint32(qEnvironmentVariableIntValue("QUCS_PROFILE_FUZZ_SEED") + 1));
        const QString out = qEnvironmentVariable("QUCS_FUZZ_OUT");
        auto hostile = [&rng]() {
            static const double v[] = {0, -0.0, 1, -1, 1e308, -1e308, 1e-320, std::numeric_limits<double>::infinity(), std::nan("")};
            return rng.bounded(3u) == 0 ? v[rng.bounded(quint32(std::size(v)))] : rng.generateDouble() * 2 - 1;
        };
        QElapsedTimer timer;
        timer.start();
        long files = 0, read = 0;
        while (timer.elapsed() < seconds * 1000LL) {
            std::vector<ModeProfile> modes(1 + rng.bounded(3u));
            const int nx = rng.bounded(6u) == 0 ? 1 : int(1 + rng.bounded(rng.bounded(10u) == 0 ? 300u : 12u));
            const int ny = rng.bounded(6u) == 0 ? 1 : int(1 + rng.bounded(rng.bounded(10u) == 0 ? 300u : 12u));
            for (ModeProfile& m : modes) {
                for (int i = 0; i < nx; ++i) m.x.push_back(i * 1e-7 * (rng.bounded(10u) == 0 ? -1 : 1));
                for (int j = 0; j < ny; ++j) m.y.push_back(j * 1e-7);
                for (auto& f : m.F) {
                    f.resize(std::size_t(nx) * std::size_t(ny));
                    for (auto& c : f) c = {hostile(), hostile()};
                }
                m.neff = {hostile(), hostile()};
                m.lambda0 = hostile();
                m.mode = int(rng.bounded(5u));
                m.label = QStringLiteral("fuzz");
            }
            QByteArray bytes = profilesMat(modes, QStringLiteral("about"));
            if (rng.bounded(4u) == 0) (void)profilesNpz(modes, QStringLiteral("about"));
            if (rng.bounded(4u) == 0) (void)profilesCsv(modes, QStringLiteral("about"));
            // Damaged.
            const int damages = int(rng.bounded(6u));
            for (int k = 0; k < damages && !bytes.isEmpty(); ++k) {
                const qsizetype at = qsizetype(rng.bounded(quint32(bytes.size())));
                switch (rng.bounded(6u)) {
                case 0: bytes[at] = char(rng.bounded(256u)); break;
                case 1: bytes.truncate(at); break;
                case 2: bytes.insert(at, bytes.mid(at, qsizetype(rng.bounded(200u)))); break;
                case 3: bytes.remove(at, qsizetype(rng.bounded(64u))); break;
                default: {
                    // A 32-bit field written over: a size, a type, a count.
                    static const quint32 values[] = {0, 1, 2, 3, 4, 5, 6, 7, 9, 14, 15, 16, 0x7fffffffu, 0x80000000u, 0xffffffffu, 0xfffffff8u, 65536, 8, 24};
                    const quint32 v = values[rng.bounded(quint32(std::size(values)))];
                    const qsizetype aligned = at & ~qsizetype(3);
                    if (aligned + 4 <= bytes.size()) std::memcpy(bytes.data() + aligned, &v, 4);
                    break;
                }
                }
            }
            if (!out.isEmpty()) {
                QFile f(QDir(out).filePath(QStringLiteral("profile-current.mat")));
                if (f.open(QIODevice::WriteOnly)) f.write(bytes);
            }
            ++files;
            QString error;
            const int index = int(rng.bounded(7u)) - 1;
            const std::optional<ModeProfile> p = readProfileMat(bytes, index, &error);
            if (!p) continue;
            ++read;
            for (const auto& f : p->F) QVERIFY2(f.size() == std::size_t(p->nx()) * std::size_t(p->ny()), "a field's size not its grid's");
            for (int k = 0; k < 20; ++k) {
                std::complex<double> E[3], H[3];
                (void)p->at(QPointF(hostile() * 1e-6, hostile() * 1e-6), E, H);
            }
            (void)p->flux();
        }
        qInfo("profiles: %ld files, %ld read back, in %d s", files, read, seconds);
    }
};

QTEST_GUILESS_MAIN(TestFemModeOverlap)
#include "test_fem_modeoverlap.moc"
