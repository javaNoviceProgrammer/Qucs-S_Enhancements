/*
 * test_rf25_farfield.cpp - the far field of a full-wave solve's currents
 * (rf25/rf25_farfield.h): a short horizontal dipole and a short monopole
 * over a ground in vacuum, against their closed forms - the image theory's
 * array factors - and their radiated power, integrated here; a dipole on a
 * grounded slab against the slab's own line written here; and, in a full
 * solve of a patch over vacuum, the power it accepts radiated, nothing else
 * carrying any away.
 */
#include <QtTest>

#include "rf25/rf25_farfield.h"
#include "rf25/rf25_fullwave.h"
#include "rf25/rf25_kept.h"
#include "rf25/rf25_mesh.h"

#include <cmath>
#include <complex>

namespace rf = qucs_s::rf25;
using Complex = std::complex<double>;

namespace {

constexpr double kMu0 = 4e-7 * M_PI;
const Complex kJ(0, 1);
const double kEta0 = std::sqrt(kMu0 / rf::kEps0);

/// A ground under \a h (m) of \a epsr, air above.
rf::Layers grounded(double epsr, double h)
{
    rf::Layers l;
    l.groundBelow = true;
    rf::Layers::Slab s;
    s.z1 = h;
    s.epsr = epsr;
    l.slabs << s;
    return l;
}

rf::Stack stackOf(double epsr, double h)
{
    rf::Stack s;
    rf::Layer ground;
    ground.kind = rf::Layer::Ground;
    ground.name = QStringLiteral("GND");
    ground.tag = QStringLiteral("gnd");
    rf::Layer sub;
    sub.kind = rf::Layer::Substrate;
    sub.name = QStringLiteral("SUB");
    sub.tag = QStringLiteral("sub");
    sub.thickness = h;
    sub.epsr = epsr;
    rf::Layer metal;
    metal.kind = rf::Layer::Metal;
    metal.name = QStringLiteral("M1");
    metal.tag = QStringLiteral("m1");
    s.layers << ground << sub << metal;
    return s;
}

} // namespace

class TestRf25FarField : public QObject
{
    Q_OBJECT

private slots:
    // A short horizontal current along x, l = 0.01 µm at 2 mm over a ground, in
    // vacuum, at 10 GHz: with its image, |E_theta| = w mu0 I l/(4 pi) cos
    // theta |cos phi| 2|sin(k0 h cos theta)| and |E_phi| the same with
    // |sin phi| for cos theta |cos phi| - each to 1e-9; and its power,
    // integrated here from those, to 1e-6.
    void aDipoleOverAGroundIsItsImagesPair()
    {
        const double f = 10e9, w = 2 * M_PI * f, k0 = w / 299792458.0, h = 2e-3, l = 1e-8;
        const rf::Layers layers = grounded(1, 5e-3);
        const QList<rf::FarSource> dipole{{QPointF(-l / 2, 0), QPointF(l / 2, 0), h, h, Complex(1, 0)}};
        const double scale = w * kMu0 * l / (4 * M_PI);
        for (const double theta : {0.1, 0.5, 1.0, 1.4})
            for (const double phi : {0.0, 0.7, 1.9, 4.0}) {
                const rf::FarFieldAt e = rf::farFieldOf(layers, f, dipole, theta, phi);
                const double array = 2 * std::abs(std::sin(k0 * h * std::cos(theta)));
                const double wantTheta = scale * std::cos(theta) * std::abs(std::cos(phi)) * array, wantPhi = scale * std::abs(std::sin(phi)) * array;
                QVERIFY2(std::abs(std::abs(e.theta) - wantTheta) <= 1e-9 * scale && std::abs(std::abs(e.phi) - wantPhi) <= 1e-9 * scale,
                         qPrintable(QStringLiteral("%1, %2: %3 %4 against %5 %6").arg(theta).arg(phi).arg(std::abs(e.theta)).arg(std::abs(e.phi))
                                        .arg(wantTheta).arg(wantPhi)));
            }
        // Its power: the closed forms' intensity over the half-space, by the
        // midpoint rule on a fine grid.
        double power = 0;
        const int n = 2000;
        for (int a = 0; a < n; ++a) {
            const double theta = (a + 0.5) * (M_PI / 2) / n;
            const double array = 2 * std::sin(k0 * h * std::cos(theta));
            // (Over phi: cos^2 and sin^2 each average a half.)
            const double u = scale * scale * array * array * (std::cos(theta) * std::cos(theta) + 1) * M_PI / (2 * kEta0);
            power += u * std::sin(theta) * (M_PI / 2) / n;
        }
        const rf::Radiation r = rf::radiationOf(layers, f, dipole);
        QVERIFY2(std::abs(r.power / power - 1) < 1e-6, qPrintable(QStringLiteral("%1 W against %2").arg(r.power).arg(power)));
    }

    // A short monopole on the ground, 1 mm up, in vacuum at 10 GHz: with its
    // image, |E_theta| = w mu0 I/(4 pi) sin theta 2|sin(k0 l cos theta)/(k0
    // cos theta)|, to 1e-9.
    void aMonopoleOnTheGroundIsItsImagesPair()
    {
        const double f = 10e9, w = 2 * M_PI * f, k0 = w / 299792458.0, l = 1e-3;
        const rf::Layers layers = grounded(1, 5e-3);
        const QList<rf::FarSource> monopole{{QPointF(0, 0), QPointF(0, 0), 0, l, Complex(1, 0)}};
        const double scale = w * kMu0 / (4 * M_PI);
        for (const double theta : {0.2, 0.8, 1.3})
            for (const double phi : {0.0, 2.0}) {
                const rf::FarFieldAt e = rf::farFieldOf(layers, f, monopole, theta, phi);
                const double want = scale * std::sin(theta) * 2 * std::abs(std::sin(k0 * l * std::cos(theta)) / (k0 * std::cos(theta)));
                QVERIFY2(std::abs(std::abs(e.theta) - want) <= 1e-9 * scale * l && std::abs(e.phi) <= 1e-12 * scale * l,
                         qPrintable(QStringLiteral("%1: %2 against %3").arg(theta).arg(std::abs(e.theta)).arg(want)));
            }
    }

    // A short dipole on a grounded slab, 0.5 mm of eps_r 4 (tan d 0.01) at
    // 20 GHz, on its top: each line's tangential field there by the slab's
    // own line written here - Z_in = j Z1 tan(kz1 h), V = V+ 2 Z_in/(Z_in +
    // Z0), V+ 1 for TE and cos theta for TM -, to 1e-9.
    void aDipoleOnASlabIsItsLines()
    {
        const double f = 20e9, w = 2 * M_PI * f, k0 = w / 299792458.0, h = 0.5e-3, l = 1e-8;
        rf::Layers layers = grounded(4, h);
        layers.slabs[0].tand = 0.01;
        const QList<rf::FarSource> dipole{{QPointF(-l / 2, 0), QPointF(l / 2, 0), h, h, Complex(1, 0)}};
        const Complex eps = rf::kEps0 * Complex(4, -0.04);
        const double scale = w * kMu0 * l / (4 * M_PI);
        for (const double theta : {0.15, 0.6, 1.2}) {
            const double phi = 0.6, kr = k0 * std::sin(theta), kz0 = k0 * std::cos(theta);
            Complex kz1 = std::sqrt(w * w * kMu0 * eps - kr * kr);
            if (kz1.imag() > 0) kz1 = -kz1;
            const Complex zinE = kJ * kz1 / (w * eps) * std::tan(kz1 * h), zinH = kJ * w * kMu0 / kz1 * std::tan(kz1 * h);
            const double z0E = kz0 / (w * rf::kEps0), z0H = w * kMu0 / kz0;
            const Complex vE = std::cos(theta) * 2.0 * zinE / (zinE + z0E), vH = 2.0 * zinH / (zinH + z0H);
            const rf::FarFieldAt e = rf::farFieldOf(layers, f, dipole, theta, phi);
            const double wantTheta = scale * std::abs(vE * std::cos(phi)), wantPhi = scale * std::abs(vH * std::sin(phi));
            QVERIFY2(std::abs(std::abs(e.theta) - wantTheta) <= 1e-9 * scale && std::abs(std::abs(e.phi) - wantPhi) <= 1e-9 * scale,
                     qPrintable(QStringLiteral("%1: %2 %3 against %4 %5").arg(theta).arg(std::abs(e.theta)).arg(std::abs(e.phi)).arg(wantTheta).arg(wantPhi)));
        }
    }

    // A patch 10 by 6 mm over 1 mm of vacuum, fed at an edge, at 12 and 14
    // GHz: the power its port accepts is radiated - vacuum over a perfect
    // ground carries no surface wave, nor loses any - within 3 % (1.0020 and
    // 1.0044), the far field of its runs and its port's path up from the
    // ground; broadside its strongest, its directivity a patch's few (7.5
    // and 8.7).
    void aPatchRadiatesWhatItAccepts()
    {
        rf::Conductor patch;
        patch.layer = QStringLiteral("M1");
        patch.outline = QPolygonF(QRectF(0, 0, 10000, 6000));
        rf::MeshSettings how;
        how.maximum = 0.5e-3;
        how.cellsAcross = 8;
        const rf::Mesh mesh = rf::meshOf({patch}, how, rf::Stack(), 0);
        rf::FullWavePort port;
        port.name = QStringLiteral("P1");
        port.at = QPointF(0, 3000);
        port.impedance = 50;
        port.metal = QStringLiteral("M1");
        port.facing = QPointF(-1, 0);
        const rf::Stack stack = stackOf(1, 1e-3);
        const rf::FullWave wave = rf::fullWaveOf(mesh, {patch}, stack, {port}, {12e9, 14e9});
        QVERIFY2(wave.ok(), qPrintable(wave.problems.join(" | ")));
        for (int k = 0; k < 2; ++k) {
            const rf::AntennaFigures a = rf::antennaFiguresOf(wave, stack, k, 0);
            QVERIFY2(a.ok(), qPrintable(a.problems.join(" | ")));
            qWarning("a patch over vacuum at %g GHz: accepted %.5g W, radiated %.5g W (efficiency %.4f), directivity %.3g, gain %.3g, at theta %.3g",
                     wave.frequencies.at(k) / 1e9, a.accepted, a.radiated, a.efficiency, a.directivity, a.gain, a.thetaMost);
            QVERIFY2(std::abs(a.efficiency - 1) < 0.03, qPrintable(QString::number(a.efficiency)));
            QVERIFY(a.directivity > 2 && a.directivity < 12);
            QVERIFY(std::abs(a.gain / a.directivity - a.efficiency) < 1e-9);
        }
        // Realized gain: over what the source offers, the port's mismatch in
        // it - the gain times 1 - |S11|^2, to 1e-9.
        const rf::AntennaFigures one = rf::antennaFiguresOf(wave, stack, 0, 0);
        const double mismatch = 1 - std::norm(wave.s.at(0).at(0).at(0));
        QVERIFY2(std::abs(one.realizedGain / (one.gain * mismatch) - 1) < 1e-9,
                 qPrintable(QStringLiteral("%1 against %2").arg(one.realizedGain).arg(one.gain * mismatch)));
        // Its ports' paths kept beside the model, with the rest (format 5), so
        // that a solution read back has the same far field.
        QVERIFY(!wave.portPaths.isEmpty());
        QTemporaryDir dir;
        rf::KeptSolution kept;
        kept.fullWave = std::make_shared<rf::FullWave>(wave);
        kept.solvedAt = QDateTime::currentDateTimeUtc();
        QString why;
        QVERIFY2(rf::writeSolution(dir.filePath(QStringLiteral("p.solution")), kept, &why), qPrintable(why));
        const std::optional<rf::KeptSolution> back = rf::readSolution(dir.filePath(QStringLiteral("p.solution")), &why);
        QVERIFY2(back && back->fullWave, qPrintable(why));
        QCOMPARE(back->fullWave->portPaths.size(), wave.portPaths.size());
        const rf::AntennaFigures again = rf::antennaFiguresOf(*back->fullWave, stack, 0, 0);
        QVERIFY(again.ok() && again.radiated == rf::antennaFiguresOf(wave, stack, 0, 0).radiated);
        // Under a lid, or with no ground: no far field of its own, said.
        rf::Stack open = stack;
        open.layers.removeFirst();
        QVERIFY(!rf::antennaFiguresOf(wave, open, 0, 0).ok());
    }

    // A two-port (the review of eac3641e: the solve's currents are each
    // port's with the others open): a line 12 mm over 1 mm of vacuum at 10
    // GHz, driven at one end. With the other port ended by its reference -
    // as a measurement has it, and the default - it radiates what it takes,
    // accepted less what that load takes, within 3 %; with it open (asked
    // for), what it accepts, as a standing wave; and the two differ.
    void aTwoPortRadiatesWhatItTakesItsOtherEnded()
    {
        rf::Conductor strip;
        strip.layer = QStringLiteral("M1");
        strip.outline = QPolygonF(QRectF(0, 0, 12000, 3000));
        rf::MeshSettings how;
        how.maximum = 0.5e-3;
        how.cellsAcross = 6;
        const rf::Mesh mesh = rf::meshOf({strip}, how, rf::Stack(), 0);
        rf::FullWavePort a, b;
        a.name = QStringLiteral("P1");
        a.at = QPointF(0, 1500);
        a.metal = QStringLiteral("M1");
        a.facing = QPointF(-1, 0);
        b = a;
        b.name = QStringLiteral("P2");
        b.at = QPointF(12000, 1500);
        b.facing = QPointF(1, 0);
        const rf::Stack stack = stackOf(1, 1e-3);
        const rf::FullWave wave = rf::fullWaveOf(mesh, {strip}, stack, {a, b}, {10e9});
        QVERIFY2(wave.ok(), qPrintable(wave.problems.join(" | ")));
        const rf::AntennaFigures ended = rf::antennaFiguresOf(wave, stack, 0, 0), open = rf::antennaFiguresOf(wave, stack, 0, 0, true);
        QVERIFY(ended.ok() && open.ok() && !ended.othersOpen && open.othersOpen);
        qWarning("a line over vacuum: ended, accepted %.4g W, its load %.4g, radiated %.4g (efficiency %.4f); open, accepted %.4g, radiated %.4g (%.4f)",
                 ended.accepted, ended.loads, ended.radiated, ended.efficiency, open.accepted, open.radiated, open.efficiency);
        QVERIFY2(std::abs(ended.efficiency - 1) < 0.03 && ended.loads > 0, qPrintable(QString::number(ended.efficiency)));
        QVERIFY2(std::abs(open.efficiency - 1) < 0.03, qPrintable(QString::number(open.efficiency)));
        QVERIFY(std::abs(ended.directivity / open.directivity - 1) > 0.01);
        QVERIFY(ended.notes.join(' ').contains(QStringLiteral("ended by their references")));
    }

    // A narrow beam: sixteen short dipoles along y half a wavelength apart,
    // 3 mm over a ground in vacuum at 10 GHz. Its peak falls between the
    // quadrature's nodes; refined, the directivity is that of a fine search
    // (the intensity along phi 90 degrees every hundredth of a degree, the
    // power by four times the order) within 0.2 %.
    void aNarrowBeamsPeakIsFoundBetweenTheNodes()
    {
        const double f = 10e9, lambda = 299792458.0 / f;
        const rf::Layers layers = grounded(1, 5e-3);
        QList<rf::FarSource> array;
        for (int i = 0; i < 16; ++i) {
            const double y = (i - 7.5) * lambda / 2;
            array << rf::FarSource{QPointF(-1e-6, y), QPointF(1e-6, y), 3e-3, 3e-3, std::complex<double>(1, 0)};
        }
        const rf::Radiation r = rf::radiationOf(layers, f, array);
        const rf::Radiation fine = rf::radiationOf(layers, f, array, 96);
        double most = 0;
        for (int i = 0; i <= 9000; ++i) {
            const double theta = i * 0.01 * M_PI / 180;
            const rf::FarFieldAt e = rf::farFieldOf(layers, f, array, theta, M_PI / 2);
            most = std::max(most, (std::norm(e.theta) + std::norm(e.phi)) / (2 * std::sqrt(kMu0 / rf::kEps0)));
        }
        const double want = 4 * M_PI * most / fine.power;
        qWarning("sixteen dipoles: directivity %.5g, the fine search's %.5g", r.directivity(), want);
        QVERIFY2(std::abs(r.directivity() / want - 1) < 0.002, qPrintable(QStringLiteral("%1 against %2").arg(r.directivity()).arg(want)));
    }
};

QTEST_GUILESS_MAIN(TestRf25FarField)
#include "test_rf25_farfield.moc"
