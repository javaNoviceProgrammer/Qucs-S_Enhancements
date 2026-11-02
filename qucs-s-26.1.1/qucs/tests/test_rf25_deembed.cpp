/*
 * test_rf25_deembed.cpp - an edge port's own discontinuity taken out of a
 * full-wave solve by two calibration strips of its feed, and its reference
 * plane moved along its line (rf25/rf25_deembed.h); and the Touchstone 2.0
 * that carries each port's own reference.
 *
 * Held to what does not rest on the calibration: a line de-embedded half
 * way from each end is a thru; the feed's line is phase B's quasi-static
 * line; an open line de-embedded is its open end, whose extension
 * Kirschning, Jansen and Koster give in closed form.
 *
 * Phase D of docs/proposals/2026-10-23-rf-2p5d-moment-method.md.
 */
#include <QtTest>

#include "rf25/rf25_deembed.h"
#include "rf25/rf25_static.h"

#include <cmath>
#include <complex>

namespace rf = qucs_s::rf25;
using Complex = std::complex<double>;

namespace {

constexpr double kMu0 = 4e-7 * M_PI;
const double kC0 = 1.0 / std::sqrt(kMu0 * rf::kEps0);

/// A ground plane, a substrate of \a epsr and \a height (m), M1 on it.
rf::Stack grounded(double epsr, double height, double tand = 0)
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
    sub.thickness = height;
    sub.epsr = epsr;
    sub.tand = tand;
    rf::Layer metal;
    metal.kind = rf::Layer::Metal;
    metal.name = QStringLiteral("M1");
    metal.tag = QStringLiteral("m1");
    s.layers << ground << sub << metal;
    return s;
}

rf::Conductor strip(double x, double y, double w, double h)
{
    rf::Conductor c;
    c.layer = QStringLiteral("M1");
    c.outline = QPolygonF(QRectF(x, y, w, h));
    return c;
}

rf::FullWavePort edgePort(QPointF at, QPointF facing, double impedance = 50)
{
    rf::FullWavePort p;
    p.at = at;
    p.facing = facing;
    p.impedance = impedance;
    return p;
}

/// \a stack with V1, vias from its ground to M1.
rf::Stack withVias(rf::Stack stack)
{
    rf::Layer via;
    via.kind = rf::Layer::Via;
    via.name = QStringLiteral("V1");
    via.tag = QStringLiteral("v1");
    via.from = QStringLiteral("GND");
    via.to = QStringLiteral("M1");
    stack.layers << via;
    return stack;
}

/// A via \a side µm square from the ground at \a x, \a y (its lower corner).
rf::Conductor via(double x, double y, double side = 50)
{
    rf::Conductor c = strip(x, y, side, side);
    c.layer = QStringLiteral("V1");
    return c;
}

/// A grounded coplanar line \a length µm long: a 100 µm signal on y = 0,
/// 50 µm gaps, 150 µm grounds, and a 50 µm via down the middle of each
/// ground at each of \a rows (µm in along it) - the recheck's line, on 100
/// µm of substrate rather than 200.
QList<rf::Conductor> groundedCpw(double length, const QList<double>& rows)
{
    QList<rf::Conductor> out = {strip(0, -50, length, 100), strip(0, 100, length, 150), strip(0, -250, length, 150)};
    for (const double x : rows) out << via(x, 150) << via(x, -200);
    return out;
}

/// Cells \a across a conductor, none longer than \a largest µm, twice as
/// long as wide along a strip: the structure's and its strips' alike.
rf::MeshSettings meshing(int across, double largest)
{
    rf::MeshSettings how;
    how.maximum = largest * 1e-6;
    how.cellsAcross = across;
    how.stretch = 2;
    return how;
}

QString why(const rf::FullWave& fw)
{
    return fw.problems.join(QStringLiteral(" | "));
}

/// Whether this build is one whose dense solves are many times slower - a
/// sanitized one, as CI's Linux job is (QUCS_TEST_SLOW: its path in any).
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

/// The S matrix [S11, S21, S12, S22] of a line \a length (m) of propagation
/// constant \a gamma and impedance \a z between two ports of \a reference.
std::array<Complex, 4> lineS(Complex gamma, Complex z, double length, double reference)
{
    const Complex c = std::cosh(gamma * length), s = std::sinh(gamma * length);
    const Complex den = 2.0 * c + z * s / reference + s * reference / z;
    const Complex s11 = (z * s / reference - s * reference / z) / den;
    return {s11, 2.0 / den, 2.0 / den, s11};
}

/// Kirschning, Jansen and Koster's open end of a microstrip (Electron. Lett.
/// 17 (1981) 123): its extension over its height, for a strip \a u = w/h
/// wide on a substrate of \a epsr whose quasi-static effective permittivity
/// is \a eps. Good to 2.5 % for 0.01 <= u <= 100, epsr to 128.
double openEnd(double u, double epsr, double eps)
{
    const double e = std::pow(eps, 0.81), p = std::pow(u, 0.8544);
    const double x1 = 0.434907 * (e + 0.26) / (e - 0.189) * (p + 0.236) / (p + 0.87);
    const double x2 = 1 + std::pow(u, 0.371) / (2.358 * epsr + 1);
    const double x3 = 1 + 0.5274 * std::atan(0.084 * std::pow(u, 1.9413 / x2)) / std::pow(eps, 0.9236);
    const double x4 = 1 + 0.0377 * std::atan(0.067 * std::pow(u, 1.456)) * (6 - 5 * std::exp(0.036 * (1 - epsr)));
    const double x5 = 1 - 0.218 * std::exp(-7.5 * u);
    return x1 * x3 * x5 / x4;
}

} // namespace

class TestRf25Deembed : public QObject
{
    Q_OBJECT

private slots:
    // The proposal's: "a line of a known length de-embedded to within a
    // degree of phase". A line of 3 mm, 100 µm wide on 100 µm of 9.8, its
    // ports' own discontinuity taken out by two strips of its feed (1 and
    // 2 mm, as the calibration chooses), its reference planes moved 1.5 mm
    // in from each end: a thru, S21 one and S11 nothing. Moved 0.5 mm in:
    // the 2 mm of line between, as the calibration's own line says it.
    void aLineDeembeddedHalfWayIsAThru()
    {
        const double h = 100e-6, w = 100, l = 3000;
        const rf::Stack stack = grounded(9.8, h);
        const rf::MeshSettings how = meshing(4, 50);
        const QList<double> fs = {1e9, 5e9, 10e9, 20e9};
        const rf::Calibration cal = rf::calibrate(stack, how, 0, QStringLiteral("M1"), w, fs);
        QVERIFY2(cal.ok(), qPrintable(cal.problems.join(" | ")));
        QCOMPARE(cal.length, 1000.0);   // (8 heights and 2 widths)
        QCOMPARE(cal.discontinuity.size(), fs.size());
        const QList<rf::Conductor> line = {strip(0, 0, l, w)};
        const rf::FullWave raw = rf::fullWaveOf(rf::meshOf(line, how, stack, 0), line, stack,
                                                {edgePort(QPointF(0, w / 2), QPointF(-1, 0)), edgePort(QPointF(l, w / 2), QPointF(1, 0))}, fs);
        QVERIFY2(raw.ok(), qPrintable(why(raw)));
        QCOMPARE(raw.faceWidths, (QList<double>{w, w}));
        QCOMPARE(raw.faceMetals, (QStringList{QStringLiteral("M1"), QStringLiteral("M1")}));

        const rf::FullWave thru = rf::deembedded(raw, {{&cal, 1500e-6}, {&cal, 1500e-6}});
        QVERIFY2(thru.ok(), qPrintable(why(thru)));
        QCOMPARE(thru.zSolved.size(), fs.size());
        for (int k = 0; k < fs.size(); ++k) {
            const Complex s21 = thru.s.at(k).at(1).at(0), s11 = thru.s.at(k).at(0).at(0);
            qWarning().noquote() << QStringLiteral("a thru at %1 GHz: S21 %2 at %3 deg, |S11| %4")
                                        .arg(fs.at(k) / 1e9).arg(std::abs(s21), 0, 'f', 6).arg(std::arg(s21) * 180 / M_PI, 0, 'f', 4)
                                        .arg(std::abs(s11), 0, 'e', 2);
            QVERIFY2(std::abs(s21 - 1.0) < 2e-3 && std::abs(s11) < 2e-3,
                     qPrintable(QStringLiteral("%1 GHz: S21 %2 %3, |S11| %4").arg(fs.at(k) / 1e9).arg(s21.real()).arg(s21.imag()).arg(std::abs(s11))));
            // (Within a tenth of a degree to 20 GHz, where the surface wave
            //  couples the two ports a little over the strips' lengths and
            //  the line's, which differ: measured 0.002 degrees at 1 GHz,
            //  0.05 at 20.)
            QVERIFY2(std::abs(std::arg(s21)) * 180 / M_PI < 0.1, "within a tenth of a degree");
        }
        const rf::FullWave part = rf::deembedded(raw, {{&cal, 500e-6}, {&cal, 500e-6}});
        QVERIFY2(part.ok(), qPrintable(why(part)));
        for (int k = 0; k < fs.size(); ++k) {
            const std::array<Complex, 4> want = lineS(cal.gamma.at(k), cal.impedance.at(k), 2e-3, 50);
            const Complex s21 = part.s.at(k).at(1).at(0), s11 = part.s.at(k).at(0).at(0);
            const double degrees = std::abs(std::arg(s21 / want[1])) * 180 / M_PI;
            QVERIFY2(degrees < 0.1 && std::abs(s21 - want[1]) < 2e-3 && std::abs(s11 - want[0]) < 3e-3,
                     qPrintable(QStringLiteral("%1 GHz: %2 deg from the line's own phase, S11 %3 off")
                                    .arg(fs.at(k) / 1e9).arg(degrees).arg(std::abs(s11 - want[0]))));
        }
        // The currents are the solve's, driven at the ports as solved,
        // whatever the reference planes were moved to.
        for (int k = 0; k < fs.size(); ++k) QCOMPARE(rf::currentDensity(thru, k, 0, true), rf::currentDensity(raw, k, 0, true));
        // What each port had, at the top frequency, and said.
        QVERIFY(thru.deembedding.value(0).done && thru.deembedding.value(1).done);
        QCOMPARE(thru.deembedding.value(0).shift, 1500e-6);
        QCOMPARE(thru.deembedding.value(0).length, 1000.0);
        QVERIFY2(thru.lines.join(' ').contains(QStringLiteral("P1 de-embedded")), qPrintable(thru.lines.join(" | ")));
        QVERIFY2(cal.lines.join(' ').contains(QStringLiteral("Three strips of the feed")), qPrintable(cal.lines.join(" | ")));
    }

    // The feed's line is the line: its impedance and effective permittivity
    // at 1 GHz against phase B's quasi-static extraction of the same cross-
    // section; and an open line, its port's own discontinuity taken out, is
    // the line and its open end - whose extension Kirschning, Jansen and
    // Koster give in closed form, 31.7 µm here. Six cells across, as the
    // mesh converges there (four: 29.6 µm, six 31.0, eight 31.5).
    void theFeedIsALineAndAnOpenEndIsItsOwn()
    {
        const double h = 100e-6, w = 100, l = 1500, f = 1e9;
        const rf::Stack stack = grounded(9.8, h);
        const rf::MeshSettings how = meshing(6, 33);
        const rf::Calibration cal = rf::calibrate(stack, how, 0, QStringLiteral("M1"), w, {f});
        QVERIFY2(cal.ok(), qPrintable(cal.problems.join(" | ")));
        const QList<rf::Conductor> piece = {strip(0, 0, 1000, w)};
        const rf::Extraction ex = rf::extract(rf::meshOf(piece, how, stack, 0), piece, stack);
        QVERIFY2(ex.ok(), qPrintable(ex.problems.join(" | ")));
        const rf::LineParameters quasi = rf::lineParameters(ex, 0, 1000e-6, f);
        const Complex z = cal.impedance.at(0), gamma = cal.gamma.at(0);
        const double eps = std::pow(gamma.imag() * kC0 / (2 * M_PI * f), 2);
        qWarning().noquote() << QStringLiteral("the feed: %1 Ohm, eps_eff %2; phase B %3 Ohm, %4")
                                    .arg(z.real(), 0, 'f', 3).arg(eps, 0, 'f', 4).arg(quasi.z0, 0, 'f', 3).arg(quasi.epsilonEff, 0, 'f', 4);
        QVERIFY2(std::abs(z.real() / quasi.z0 - 1) < 1e-2, "the impedance, phase B's");
        QVERIFY2(std::abs(eps / quasi.epsilonEff - 1) < 1.5e-2, "the effective permittivity, phase B's");
        QVERIFY2(std::abs(z.imag()) < 1e-3 * z.real() && gamma.real() < 1e-3 * gamma.imag(), "a line with no loss");

        const QList<rf::Conductor> open = {strip(0, 0, l, w)};
        const rf::FullWave raw = rf::fullWaveOf(rf::meshOf(open, how, stack, 0), open, stack, {edgePort(QPointF(0, w / 2), QPointF(-1, 0))}, {f});
        QVERIFY2(raw.ok(), qPrintable(why(raw)));
        const rf::FullWave d = rf::deembedded(raw, {{&cal, 0}});
        QVERIFY2(d.ok(), qPrintable(why(d)));
        // Z_in = Z0 coth(gamma (l + dl)).
        const Complex length = std::atanh(z / d.z.at(0).at(0).at(0)) / gamma;
        const double extension = length.real() - l * 1e-6;
        const double want = openEnd(w * 1e-6 / h, 9.8, quasi.epsilonEff) * h;
        qWarning().noquote() << QStringLiteral("the open end: %1 um; Kirschning, Jansen and Koster %2 um (%3)")
                                    .arg(extension * 1e6, 0, 'f', 2).arg(want * 1e6, 0, 'f', 2).arg(extension / want - 1, 0, 'e', 2);
        QVERIFY2(std::abs(extension / want - 1) < 4e-2, "the open end's own extension");
        // Solved as it is, the port's own discontinuity is in it: the same
        // line read as an open end comes out otherwise.
        const Complex rawLength = std::atanh(z / raw.z.at(0).at(0).at(0)) / gamma;
        QVERIFY2(std::abs(rawLength.real() - length.real()) > 5e-6,
                 qPrintable(QStringLiteral("as solved %1 um").arg((rawLength.real() - l * 1e-6) * 1e6)));
    }

    // De-embedding keeps what physics allows: the de-embedded two-port is
    // reciprocal, and passive where the solve was, but for what the strips
    // and the structure's own feed differ by - the solve's own checks kept
    // beside it. A step between two widths takes a calibration of each feed.
    void deembeddingKeepsWhatPhysicsAllows()
    {
        const double h = 100e-6;
        const rf::Stack stack = grounded(9.8, h, 0.002);
        const rf::MeshSettings how = meshing(4, 50);
        const QList<double> fs = {2e9, 12e9};
        QPolygonF outline;   // (100 µm wide for 1.5 mm, then 200 µm for 1.5 mm)
        outline << QPointF(0, 0) << QPointF(1500, 0) << QPointF(1500, -50) << QPointF(3000, -50) << QPointF(3000, 150)
                << QPointF(1500, 150) << QPointF(1500, 100) << QPointF(0, 100);
        rf::Conductor step;
        step.layer = QStringLiteral("M1");
        step.outline = outline;
        const rf::FullWave raw = rf::fullWaveOf(rf::meshOf({step}, how, stack, 0), {step}, stack,
                                                {edgePort(QPointF(0, 50), QPointF(-1, 0)), edgePort(QPointF(3000, 50), QPointF(1, 0), 25)}, fs);
        QVERIFY2(raw.ok(), qPrintable(why(raw)));
        QCOMPARE(raw.faceWidths, (QList<double>{100, 200}));
        const rf::Calibration narrow = rf::calibrate(stack, how, 0, QStringLiteral("M1"), 100, fs);
        const rf::Calibration wide = rf::calibrate(stack, how, 0, QStringLiteral("M1"), 200, fs);
        QVERIFY2(narrow.ok() && wide.ok(), qPrintable(narrow.problems.join(" | ") + wide.problems.join(" | ")));
        QVERIFY2(wide.impedance.at(0).real() < 0.8 * narrow.impedance.at(0).real(), "the wider feed the lower line");
        const rf::FullWave d = rf::deembedded(raw, {{&narrow, 200e-6}, {&wide, 200e-6}});
        QVERIFY2(d.ok(), qPrintable(why(d)));
        qWarning().noquote() << QStringLiteral("a step: asymmetry %1, gain %2 (as solved %3)").arg(d.asymmetry).arg(d.gain).arg(d.solvedGain);
        QVERIFY2(d.asymmetry < 1e-12, "reciprocal");
        QVERIFY2(d.solvedGain <= 1e-12 && d.gain < 1e-4, "passive as solved, and nearly so de-embedded");
        QCOMPARE(d.solvedAsymmetry, raw.asymmetry);
        // Each port into its own reference, the de-embedded Z's own S.
        QCOMPARE(d.references, (QList<double>{50, 25}));
        for (int k = 0; k < fs.size(); ++k) {
            const QList<QList<Complex>> own = rf::scatteringOf(d.z.at(k), QList<double>{50, 25});
            for (int i = 0; i < 2; ++i)
                for (int j = 0; j < 2; ++j) QVERIFY(std::abs(d.s.at(k).at(i).at(j) - own.at(i).at(j)) < 1e-14);
        }
        // A port not de-embedded is as solved: only the other's reference
        // plane moves. Nothing to de-embed: the solve as it was.
        const rf::FullWave one = rf::deembedded(raw, {{&narrow, 0}, {}});
        QVERIFY(one.deembedding.value(0).done && !one.deembedding.value(1).done);
        const rf::FullWave none = rf::deembedded(raw, {{}, {}});
        QVERIFY(none.zSolved.isEmpty() && none.z == raw.z && none.deembedding.isEmpty());
    }

    // The strips' phase followed up from a frequency below the sweep: 4 mm
    // strips on 100 µm of 9.8 at 18 and 20 GHz are more than half a wave
    // long, their phase past pi before the lowest frequency asked, and only
    // the anchor below says how many turns. The feed's effective
    // permittivity is then the line's - near its quasi-static 6.5, rising,
    // and all but lossless - and a line de-embedded half way is a thru. (On
    // 635 µm of alumina at 30 GHz the strips' ports reach one another past
    // the line through the substrate's surface wave, the shorter strips' too:
    // said. On 254 µm they did from 20 GHz, and the line came out with gain,
    // while the ports' paths up from the ground had no field - G9 of the
    // phase E check, addPortPaths(); with it they agree there.)
    void aLongStripsPhaseIsFollowedFromBelow()
    {
        const double h = 100e-6, w = 100, l = 6000;
        const rf::Stack stack = grounded(9.8, h);
        const rf::MeshSettings how = meshing(2, 100);
        const QList<double> fs = {18e9, 20e9};
        const rf::Calibration cal = rf::calibrate(stack, how, 0, QStringLiteral("M1"), w, fs, 1e-6, nullptr, 4000);
        QVERIFY2(cal.ok(), qPrintable(cal.problems.join(" | ")));
        QCOMPARE(cal.length, 4000.0);
        QVERIFY2(cal.gamma.at(0).imag() * cal.length * 1e-6 > M_PI, "past half a wave");
        double eps[2];
        for (int k = 0; k < 2; ++k) eps[k] = std::pow(cal.gamma.at(k).imag() * kC0 / (2 * M_PI * fs.at(k)), 2);
        qWarning().noquote() << QStringLiteral("eps_eff %1 at 18 GHz, %2 at 20; %3 and %4 dB/m")
                                    .arg(eps[0], 0, 'f', 4).arg(eps[1], 0, 'f', 4)
                                    .arg(20 / std::log(10.0) * cal.gamma.at(0).real(), 0, 'f', 1)
                                    .arg(20 / std::log(10.0) * cal.gamma.at(1).real(), 0, 'f', 1);
        QVERIFY2(eps[0] > 6.2 && eps[1] < 7.2 && eps[1] > eps[0], "the line's, rising");
        // (A lossless bound mode: no loss but the little of what the ports
        //  radiate to each other that two strips read as the line's.)
        for (int k = 0; k < 2; ++k)
            QVERIFY2(std::abs(cal.gamma.at(k).real()) * cal.length * 1e-6 < 5e-3, "all but lossless");
        QVERIFY2(cal.lines.join(' ').contains(QStringLiteral("to follow the phase up from")), qPrintable(cal.lines.join(" | ")));
        const QList<rf::Conductor> line = {strip(0, 0, l, w)};
        const rf::FullWave raw = rf::fullWaveOf(rf::meshOf(line, how, stack, 0), line, stack,
                                                {edgePort(QPointF(0, w / 2), QPointF(-1, 0)), edgePort(QPointF(l, w / 2), QPointF(1, 0))}, fs);
        QVERIFY2(raw.ok(), qPrintable(why(raw)));
        const rf::FullWave thru = rf::deembedded(raw, {{&cal, l * 0.5e-6}, {&cal, l * 0.5e-6}});
        QVERIFY2(thru.ok(), qPrintable(why(thru)));
        for (int k = 0; k < 2; ++k)
            QVERIFY2(std::abs(thru.s.at(k).at(1).at(0) - 1.0) < 1e-2 && std::abs(thru.s.at(k).at(0).at(0)) < 1e-2,
                     qPrintable(QStringLiteral("%1 GHz: S21 %2 %3").arg(fs.at(k) / 1e9).arg(thru.s.at(k).at(1).at(0).real())
                                    .arg(thru.s.at(k).at(1).at(0).imag())));
        // The thick alumina's: the strips not one line, and said.
        const rf::Calibration thick = rf::calibrate(grounded(9.8, 635e-6), meshing(3, 200), 0, QStringLiteral("M1"), 600, {30e9});
        QVERIFY2(thick.ok(), qPrintable(thick.problems.join(" | ")));
        QVERIFY2(thick.lines.join(' ').contains(QStringLiteral("surface wave")), qPrintable(thick.lines.join(" | ")));
        QVERIFY2(!cal.lines.join(' ').contains(QStringLiteral("surface wave")), qPrintable(cal.lines.join(" | ")));
    }

    // A reference plane moved along a line given rather than the feed's:
    // the calibration's own line given, at its own frequency, is the feed's
    // to rounding; and with no strips at all - the port's own discontinuity
    // left in - a line of the ports' own 50 Ohm moves the planes by phase
    // alone, every term of S turned by exp(j beta (d_i + d_j)).
    void aReferencePlaneMovesAlongAGivenLine()
    {
        const double h = 100e-6, w = 100, l = 2000, f = 6e9;
        const rf::Stack stack = grounded(9.8, h);
        const rf::MeshSettings how = meshing(4, 50);
        const rf::Calibration cal = rf::calibrate(stack, how, 0, QStringLiteral("M1"), w, {f});
        QVERIFY2(cal.ok(), qPrintable(cal.problems.join(" | ")));
        const QList<rf::Conductor> line = {strip(0, 0, l, w)};
        const rf::FullWave raw = rf::fullWaveOf(rf::meshOf(line, how, stack, 0), line, stack,
                                                {edgePort(QPointF(0, w / 2), QPointF(-1, 0)), edgePort(QPointF(l, w / 2), QPointF(1, 0))}, {f});
        QVERIFY2(raw.ok(), qPrintable(why(raw)));
        const double eps = std::pow(cal.gamma.at(0).imag() * kC0 / (2 * M_PI * f), 2);
        const rf::FullWave feed = rf::deembedded(raw, {{&cal, 300e-6}, {&cal, 300e-6}});
        const rf::FullWave given = rf::deembedded(raw, {{&cal, 300e-6, cal.impedance.at(0).real(), eps}, {&cal, 300e-6, cal.impedance.at(0).real(), eps}});
        QVERIFY2(feed.ok() && given.ok(), qPrintable(why(feed) + why(given)));
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 2; ++j)
                QVERIFY2(std::abs(feed.s.at(0).at(i).at(j) - given.s.at(0).at(i).at(j)) < 1e-4,
                         qPrintable(QStringLiteral("S%1%2 %3 off").arg(i + 1).arg(j + 1).arg(std::abs(feed.s.at(0).at(i).at(j) - given.s.at(0).at(i).at(j)))));
        QVERIFY(given.deembedding.value(0).calibrated && given.deembedding.value(0).givenLine);

        // No strips: the planes moved 100 and 250 µm along a given 50 Ohm
        // line of eps_eff 6.5, by phase alone.
        const double beta = 2 * M_PI * f * std::sqrt(6.5) / kC0, d[2] = {100e-6, 250e-6};
        const rf::FullWave moved = rf::deembedded(raw, {{nullptr, d[0], 50, 6.5}, {nullptr, d[1], 50, 6.5}});
        QVERIFY2(moved.ok(), qPrintable(why(moved)));
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 2; ++j) {
                const Complex want = raw.s.at(0).at(i).at(j) * std::exp(Complex(0, beta * (d[i] + d[j])));
                QVERIFY2(std::abs(moved.s.at(0).at(i).at(j) - want) < 1e-12,
                         qPrintable(QStringLiteral("S%1%2 %3 off").arg(i + 1).arg(j + 1).arg(std::abs(moved.s.at(0).at(i).at(j) - want))));
            }
        QVERIFY(moved.deembedding.value(0).done && !moved.deembedding.value(0).calibrated && moved.deembedding.value(0).givenLine);
        QVERIFY2(moved.lines.join(' ').contains(QStringLiteral("discontinuity left in")), qPrintable(moved.lines.join(" | ")));
        // A given line with no shift and no strips is nothing to do.
        QVERIFY(rf::deembedded(raw, {{nullptr, 0, 50, 6.5}, {}}).zSolved.isEmpty());
    }

    // Touchstone 2.0 carries each port's own reference: [Reference], the
    // two-port's order said, the S as solved (no renormalising), [End].
    void aTouchstone2CarriesEachPortsReference()
    {
        const double h = 100e-6, w = 100;
        const rf::Stack stack = grounded(9.8, h);
        const QList<rf::Conductor> line = {strip(0, 0, 1000, w)};
        const rf::FullWave fw = rf::fullWaveOf(rf::meshOf(line, meshing(4, 50), stack, 0), line, stack,
                                               {edgePort(QPointF(0, w / 2), QPointF(-1, 0), 50), edgePort(QPointF(1000, w / 2), QPointF(1, 0), 25)},
                                               {1e9, 3e9});
        QVERIFY2(fw.ok(), qPrintable(why(fw)));
        const QString text = rf::touchstone2Of(fw, {QStringLiteral("a line")});
        const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const char* key : {"[Version] 2.0", "[Number of Ports] 2", "[Two-Port Data Order] 21_12", "[Number of Frequencies] 2",
                                "[Reference] 50 25", "[Network Data]"})
            QVERIFY2(lines.contains(QLatin1String(key)), key);
        QCOMPARE(lines.last(), QStringLiteral("[End]"));
        QVERIFY(lines.first().startsWith(QStringLiteral("! a line")));
        // The keywords in the order 2.0 asks: [Version] first of them,
        // [Reference] before [Network Data].
        QVERIFY(lines.indexOf(QStringLiteral("[Version] 2.0")) < lines.indexOf(QStringLiteral("[Number of Ports] 2")));
        QVERIFY(lines.indexOf(QStringLiteral("[Reference] 50 25")) < lines.indexOf(QStringLiteral("[Network Data]")));
        const int at = int(lines.indexOf(QStringLiteral("[Network Data]")));
        const QStringList first = lines.at(at + 1).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        QCOMPARE(first.size(), 9);
        QCOMPARE(first.at(0).toDouble(), 1e9);
        // S11 S21 S12 S22, each port into its own reference: the solve's S.
        const QList<QList<Complex>>& s = fw.s.at(0);
        const Complex values[4] = {s[0][0], s[1][0], s[0][1], s[1][1]};
        for (int i = 0; i < 4; ++i) {
            QVERIFY(std::abs(first.at(1 + 2 * i).toDouble() - values[i].real()) < 1e-11);
            QVERIFY(std::abs(first.at(2 + 2 * i).toDouble() - values[i].imag()) < 1e-11);
        }
        // And 1.1 renormalises every port into one, as before.
        const QString old = rf::touchstoneOf(fw, 50);
        QVERIFY(!old.contains(QStringLiteral("[Version]")) && old.contains(QStringLiteral("# Hz S RI R 50")));
        QVERIFY(rf::touchstone2Of(rf::FullWave()).isEmpty());
    }

    // The recheck of 1 November (afternoon): its thru - a line de-embedded
    // half way in from each end, the two reference planes meeting - came out
    // passive: false. Most of that is the strips' own accuracy, a reflection
    // of -56 dB beside a whole transmission; but the de-embedding went
    // through Z, and a thru has none: a line meshed as the longer strip is,
    // de-embedded to its middle, came out 59 % active. In waves it is the
    // thru it is.
    void aThruWhosePlanesMeetIsAThru()
    {
        const double h = 100e-6, w = 100;
        const rf::Stack stack = grounded(9.8, h);
        const rf::MeshSettings how = meshing(4, 50);
        const QList<double> fs = {1e9, 10e9, 20e9};
        const rf::Calibration cal = rf::calibrate(stack, how, 0, QStringLiteral("M1"), w, fs);
        QVERIFY2(cal.ok(), qPrintable(cal.problems.join(" | ")));
        const double l = cal.strip + cal.length;   // the longer strip's own, 2000 µm
        QCOMPARE(l, 2000.0);
        const QList<rf::Conductor> line = {strip(0, 0, l, w)};
        const rf::FullWave raw = rf::fullWaveOf(rf::meshOf(line, how, stack, 0), line, stack,
                                                {edgePort(QPointF(0, w / 2), QPointF(-1, 0)), edgePort(QPointF(l, w / 2), QPointF(1, 0))}, fs);
        QVERIFY2(raw.ok(), qPrintable(why(raw)));
        const rf::FullWave thru = rf::deembedded(raw, {{&cal, l / 2 * 1e-6}, {&cal, l / 2 * 1e-6}});
        QVERIFY2(thru.ok(), qPrintable(why(thru)));
        for (int k = 0; k < fs.size(); ++k) {
            const Complex s21 = thru.s.at(k).at(1).at(0), s11 = thru.s.at(k).at(0).at(0);
            QVERIFY2(std::abs(s11) < 1e-6 && std::abs(s21 - 1.0) < 1e-6,
                     qPrintable(QStringLiteral("%1 GHz: |S11| %2, S21 %3 %4").arg(fs.at(k) / 1e9).arg(std::abs(s11))
                                    .arg(s21.real()).arg(s21.imag())));
        }
        QVERIFY2(thru.gain < 1e-9 && thru.asymmetry < 1e-9, qPrintable(QStringLiteral("%1, %2").arg(thru.gain).arg(thru.asymmetry)));
    }

    // The recheck of 1 November (afternoon): a perfect metal's coplanar line
    // read -2 to -6 dB/m at 50-60 GHz from two lines 0.9 mm apart - their
    // ends coupling past the line - and strips 1.8 mm apart still read -1.6.
    // A line gives nothing back: a plane is moved along one read as gaining
    // as along one losing nothing, and what was read is kept and said.
    void aLineReadAsGainingIsMovedAlongAsLosingNothing()
    {
        const rf::Stack stack = grounded(9.8, 100e-6);
        const rf::MeshSettings how = meshing(4, 50);
        const QList<rf::Conductor> line = {strip(0, 0, 1500, 100)};
        const rf::FullWave raw = rf::fullWaveOf(rf::meshOf(line, how, stack, 0), line, stack,
                                                {edgePort(QPointF(0, 50), QPointF(-1, 0)), edgePort(QPointF(1500, 50), QPointF(1, 0))},
                                                {10e9});
        QVERIFY2(raw.ok(), qPrintable(why(raw)));
        // Two calibrations alike but for the line's loss: one read as gaining
        // 1 Np/m, one as losing nothing; no discontinuity of their own.
        rf::Calibration gaining;
        gaining.metal = QStringLiteral("M1");
        gaining.width = 100;
        gaining.length = gaining.strip = 1000;
        gaining.frequencies = {10e9};
        gaining.discontinuity = {{Complex(1, 0), Complex(0, 0), Complex(0, 0), Complex(1, 0)}};
        gaining.gamma = {Complex(-1.0, 520.0)};
        gaining.impedance = {Complex(48, 0)};
        QVERIFY(gaining.ok());
        rf::Calibration lossless = gaining;
        lossless.gamma = {Complex(0.0, 520.0)};
        const rf::FullWave moved = rf::deembedded(raw, {{&gaining, 500e-6}, {&gaining, 500e-6}});
        const rf::FullWave plain = rf::deembedded(raw, {{&lossless, 500e-6}, {&lossless, 500e-6}});
        QVERIFY2(moved.ok() && plain.ok(), qPrintable(why(moved) + why(plain)));
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 2; ++j)
                QVERIFY2(std::abs(moved.s.at(0).at(i).at(j) - plain.s.at(0).at(i).at(j)) < 1e-12,
                         qPrintable(QStringLiteral("S%1%2").arg(i + 1).arg(j + 1)));
        QCOMPARE(moved.deembedding.value(0).gave, -1.0);
        QCOMPARE(moved.deembedding.value(0).alpha, 0.0);
        QVERIFY(plain.deembedding.value(0).gave == 0);
        QVERIFY2(moved.lines.join(' ').contains(QStringLiteral("as losing nothing")), qPrintable(moved.lines.join(" | ")));
        QVERIFY2(!plain.lines.join(' ').contains(QStringLiteral("as losing nothing")), qPrintable(plain.lines.join(" | ")));
    }

    // The recheck of 1 November (afternoon): a coplanar port kept its own
    // discontinuity - |S11| -15 dB at 60 GHz on a 1 mm grounded coplanar
    // line whose line alone reflects -37 - since no strips of a coplanar
    // feed were solved. Its strips are its feed whole: the signal, the
    // grounds beside it as wide and as far from it as at the port, and the
    // vias down them, the first as far in from each end as the feed's, the
    // longer strip twice the shorter's periods between. The line here, 1.3
    // mm, is neither strip, and has a row of vias down each ground every
    // 150 µm from 25 µm in.
    void aCoplanarPortsFeedIsItsGroundsAndItsVias()
    {
        const rf::Stack stack = withVias(grounded(9.8, 100e-6));
        QList<double> rows;
        for (double x = 25; x < 1300; x += 150) rows << x;
        const QList<rf::Conductor> line = groundedCpw(1300, rows);
        const rf::MeshSettings how = meshing(2, 100);
        rf::FullWavePort near = edgePort(QPointF(0, 0), QPointF(-1, 0)), far = edgePort(QPointF(1300, 0), QPointF(1, 0));
        near.coplanar = far.coplanar = true;
        const QList<double> fs = {10e9, 40e9};
        const rf::FullWave raw = rf::fullWaveOf(rf::meshOf(line, how, stack, 0), line, stack, {near, far}, fs);
        QVERIFY2(raw.ok(), qPrintable(why(raw)));

        // The feed as the solve found it at the port: the grounds beside the
        // signal on the edge's line, and the rows of vias down them.
        QCOMPARE(raw.feeds.size(), 2);
        const rf::Feed& feed = raw.feeds.at(0);
        QVERIFY(feed.coplanar);
        QVERIFY2(feed.unlike.isEmpty(), qPrintable(feed.unlike));
        QCOMPARE(feed.width, 100.0);
        for (int k = 0; k < 2; ++k) {
            QVERIFY2(std::abs(feed.gap[k] - 50) < 1e-6 && std::abs(feed.ground[k] - 150) < 1e-6,
                     qPrintable(QStringLiteral("%1: gap %2, ground %3").arg(k).arg(feed.gap[k]).arg(feed.ground[k])));
        }
        QCOMPARE(feed.vias.size(), 2);
        QVERIFY2(std::abs(feed.pitch - 150) < 1e-6, qPrintable(QString::number(feed.pitch)));
        // Its grounds beside it are no widening, nor their vias over them:
        // the feed runs straight the line's whole length.
        QVERIFY2(std::abs(feed.length - 1300) <= 25, qPrintable(QString::number(feed.length)));
        QVERIFY2(std::abs(feed.offset() - 50) < 1e-6, qPrintable(QString::number(feed.offset())));
        // The far port's feed is the near one's mirror image: one calibration.
        QCOMPARE(raw.feeds.at(1).key(), feed.key());
        // So too where the grounds differ, and the two ends see them - and
        // the rows of vias down them - the other way round: 150 µm one side,
        // 250 the other.
        {
            QList<rf::Conductor> uneven = {strip(0, -50, 1300, 100), strip(0, 100, 1300, 150), strip(0, -350, 1300, 250)};
            for (const double x : rows) uneven << via(x, 150) << via(x, -250);
            const rf::FullWave twoSided = rf::fullWaveOf(rf::meshOf(uneven, how, stack, 0), uneven, stack, {near, far}, {1e9});
            QVERIFY2(twoSided.ok(), qPrintable(why(twoSided)));
            const rf::Feed& a = twoSided.feeds.at(0);
            const rf::Feed& b = twoSided.feeds.at(1);
            QVERIFY2(a.ground[0] == b.ground[1] && a.ground[1] == b.ground[0] && a.ground[0] != a.ground[1],
                     qPrintable(QStringLiteral("%1/%2 and %3/%4").arg(a.ground[0]).arg(a.ground[1]).arg(b.ground[0]).arg(b.ground[1])));
            QCOMPARE(a.vias.size(), 2);
            QCOMPARE(a.key(), b.key());
        }

        const rf::Calibration cal = rf::calibrate(stack, how, 0, feed, fs);
        QVERIFY2(cal.ok(), qPrintable(cal.problems.join(" | ")));
        // Eight heights and twice the width, 1000 µm: 900 µm between the
        // ends' first vias, six periods; the longer strip twelve.
        QVERIFY2(std::abs(cal.length - 900) < 1e-6 && std::abs(cal.strip - 1000) < 1e-6,
                 qPrintable(QStringLiteral("%1 between, %2 long").arg(cal.length).arg(cal.strip)));
        const QString said = cal.lines.join(QStringLiteral(" | "));
        QVERIFY2(said.contains(QStringLiteral("coplanar feed")) && said.contains(QStringLiteral("row(s) of vias every 150 µm"))
                     && said.contains(QStringLiteral("1000, 1900 and 1600 µm long")) && said.contains(QStringLiteral("first via, 50 µm")),
                 qPrintable(said));

        // De-embedded at both ends, the 1.3 mm is a line of the calibration's
        // own gamma and impedance; as solved, it was its ports' own
        // discontinuity that set it apart from that line.
        const rf::FullWave d = rf::deembedded(raw, {{&cal, 0}, {&cal, 0}});
        QVERIFY2(d.ok(), qPrintable(why(d)));
        QVERIFY(d.deembedding.value(0).calibrated && d.deembedding.value(0).coplanar);
        for (int k = 0; k < fs.size(); ++k) {
            const auto ideal = lineS(cal.gamma.at(k), cal.impedance.at(k), 1.3e-3, 50);
            const Complex s11 = d.s.at(k).at(0).at(0), s21 = d.s.at(k).at(1).at(0);
            const double phase = std::abs(std::arg(s21 / ideal[1])) * 180 / M_PI;
            const double off = std::abs(s11 - ideal[0]), rawOff = std::abs(raw.s.at(k).at(0).at(0) - ideal[0]);
            QVERIFY2(phase < 0.1 && off < 2e-3 && rawOff > 20 * off,
                     qPrintable(QStringLiteral("%1 GHz: S21 %2 degrees from the line's; S11 %3 from the line's, as solved %4")
                                    .arg(fs.at(k) / 1e9).arg(phase).arg(off).arg(rawOff)));
        }
        // And half way in from each end - at a via, four periods from the
        // feed's first - it is a thru.
        const rf::FullWave thru = rf::deembedded(raw, {{&cal, 0.65e-3}, {&cal, 0.65e-3}});
        QVERIFY2(thru.ok(), qPrintable(why(thru)));
        for (int k = 0; k < fs.size(); ++k) {
            const Complex s11 = thru.s.at(k).at(0).at(0), s21 = thru.s.at(k).at(1).at(0);
            QVERIFY2(std::abs(s11) < 2e-3 && std::abs(std::abs(s21) - 1) < 1e-3 && std::abs(std::arg(s21)) * 180 / M_PI < 0.1,
                     qPrintable(QStringLiteral("%1 GHz: |S11| %2, |S21| %3, %4 degrees").arg(fs.at(k) / 1e9).arg(std::abs(s11))
                                    .arg(std::abs(s21)).arg(std::arg(s21) * 180 / M_PI)));
        }
    }

    // Strips too large for a dense solve on the structure's mesh - the
    // recheck's coplanar line at eight cells across, three conductors 3.7
    // mm long, past the build's limit, and not calibrated at all - are cut
    // to a half, then a quarter, and it is said, a dense solve asked; past
    // that, refused with why. The automatic choice holds them compressed
    // past the limit, and solves them whole - as a dense solve of them
    // does. (The limit lowered here, so that strips of a few hundred
    // unknowns meet it.)
    void stripsTooLargeAreCutAndSaid()
    {
        const rf::Stack stack = grounded(9.8, 100e-6);
        const rf::MeshSettings how = meshing(2, 100);
        const auto unknowns = [&](double l) {
            const QList<rf::Conductor> one = {strip(0, 0, l, 100)};
            const rf::FullWave fw = rf::fullWaveOf(rf::meshOf(one, how, stack, 0), one, stack,
                                                   {edgePort(QPointF(0, 50), QPointF(-1, 0)), edgePort(QPointF(l, 50), QPointF(1, 0))},
                                                   {1e9});
            return fw.runs + fw.cells;
        };
        const int whole = unknowns(2000), half = unknowns(1000), quarter = unknowns(500);
        QVERIFY2(quarter < half && half < whole, qPrintable(QStringLiteral("%1 %2 %3").arg(quarter).arg(half).arg(whole)));
        rf::FullWaveControl limited;
        limited.mostUnknowns = (half + whole) / 2;
        limited.matrix = rf::FullWaveControl::Matrix::Dense;
        const rf::Calibration cut = rf::calibrate(stack, how, 0, QStringLiteral("M1"), 100, {1e9}, 1e-6, &limited);
        QVERIFY2(cut.ok(), qPrintable(cut.problems.join(" | ")));
        QCOMPARE(cut.strip, 500.0);
        QVERIFY2(cut.lines.join(' ').contains(QStringLiteral("Strips of 1000 µm")) && cut.lines.join(' ').contains(QStringLiteral("cut to 500 µm")),
                 qPrintable(cut.lines.join(" | ")));
        limited.mostUnknowns = quarter / 2;
        const rf::Calibration refused = rf::calibrate(stack, how, 0, QStringLiteral("M1"), 100, {1e9}, 1e-6, &limited);
        QVERIFY(!refused.ok());
        QVERIFY2(refused.problems.join(' ').contains(QStringLiteral("dense complex matrix")), qPrintable(refused.problems.join(" | ")));
        // The structure's own length asked for is not cut: its strips are
        // what it asked, or nothing.
        limited.mostUnknowns = (half + whole) / 2;
        QVERIFY(!rf::calibrate(stack, how, 0, QStringLiteral("M1"), 100, {1e9}, 1e-6, &limited, 1000).ok());
        rf::FullWaveControl automatic;
        automatic.mostUnknowns = (half + whole) / 2;
        const rf::Calibration held = rf::calibrate(stack, how, 0, QStringLiteral("M1"), 100, {1e9}, 1e-6, &automatic);
        rf::FullWaveControl dense;
        dense.matrix = rf::FullWaveControl::Matrix::Dense;
        const rf::Calibration reference = rf::calibrate(stack, how, 0, QStringLiteral("M1"), 100, {1e9}, 1e-6, &dense);
        QVERIFY2(held.ok() && reference.ok(), qPrintable(held.problems.join(" | ") + reference.problems.join(" | ")));
        QCOMPARE(held.strip, 1000.0);
        QCOMPARE(reference.strip, 1000.0);
        QVERIFY2(std::abs(held.impedance.first() / reference.impedance.first() - 1.0) < 1e-6
                     && std::abs(held.gamma.first() / reference.gamma.first() - 1.0) < 1e-6,
                 qPrintable(QStringLiteral("Z %1 %2j / %3 %4j, gamma %5 %6j / %7 %8j")
                                .arg(held.impedance.first().real()).arg(held.impedance.first().imag())
                                .arg(reference.impedance.first().real()).arg(reference.impedance.first().imag())
                                .arg(held.gamma.first().real()).arg(held.gamma.first().imag())
                                .arg(reference.gamma.first().real()).arg(reference.gamma.first().imag())));
    }

    // What strips cannot copy of a coplanar feed is said and not calibrated:
    // one via of a row near the port, whose period nothing tells; rows
    // staggered by other than half a period, which no strip has alike at
    // both ends; a via on the signal itself, which is no line.
    void whatACoplanarFeedsStripsCannotCopyIsSaid()
    {
        const rf::Stack stack = withVias(grounded(9.8, 100e-6));
        const rf::MeshSettings how = meshing(2, 100);
        rf::FullWavePort near = edgePort(QPointF(0, 0), QPointF(-1, 0)), far = edgePort(QPointF(1000, 0), QPointF(1, 0));
        near.coplanar = far.coplanar = true;
        const auto feedOf = [&](const QList<rf::Conductor>& line) {
            const rf::FullWave fw = rf::fullWaveOf(rf::meshOf(line, how, stack, 0), line, stack, {near, far}, {1e9});
            return fw.ok() ? fw.feeds.value(0) : rf::Feed();
        };
        const rf::Feed lone = feedOf(groundedCpw(1000, {500}));
        QVERIFY2(lone.unlike.contains(QStringLiteral("only one via of a row")), qPrintable(lone.unlike));
        QList<rf::Conductor> staggered = groundedCpw(1000, {});
        for (double x = 25; x < 1000; x += 150) staggered << via(x, 150);
        for (double x = 75; x < 1000; x += 150) staggered << via(x, -200);
        const rf::Feed third = feedOf(staggered);
        QVERIFY2(third.unlike.contains(QStringLiteral("staggered")), qPrintable(third.unlike));
        QList<rf::Conductor> shorted = groundedCpw(1000, {25, 175, 325});
        shorted << via(400, -25);
        const rf::Feed onSignal = feedOf(shorted);
        QVERIFY2(onSignal.unlike.contains(QStringLiteral("own conductor")), qPrintable(onSignal.unlike));
        // Calibrated, each is refused with its reason.
        const rf::Calibration refused = rf::calibrate(stack, how, 0, third, {1e9});
        QVERIFY(!refused.ok());
        QVERIFY2(refused.problems.join(' ').contains(QStringLiteral("staggered")), qPrintable(refused.problems.join(' ')));
        // Rows staggered by half a period are a feed strips can have.
        QList<rf::Conductor> half = groundedCpw(1000, {});
        for (double x = 25; x < 1000; x += 150) half << via(x, 150);
        for (double x = 100; x < 1000; x += 150) half << via(x, -200);
        const rf::Feed halfway = feedOf(half);
        QVERIFY2(halfway.unlike.isEmpty() && halfway.vias.size() == 2, qPrintable(halfway.unlike));
    }

    // What cannot be calibrated is said, not answered: a feed of no width, a
    // metal that is not over the ground, no frequency; a de-embedding asked
    // at a frequency its calibration was not solved at.
    // A third strip checks the other two (the phase E check of 2 November):
    // on 100 µm of alumina they are one line between two ends alike over
    // the sweep, and only the strips asked are solved. On 254 µm the strips
    // of eight heights stop agreeing at 26 GHz - their ends reach one another
    // past the line through the surface wave - and shorter strips, solved
    // beside them, agree further; each frequency is taken from the longest
    // that agree, said, and a length given is solved as given. (They stopped
    // at 12 GHz while the ports' paths up from the ground had no field - G9
    // of the phase E check, addPortPaths().)
    void aThirdStripSaysWhereTheStripsAreOneLine()
    {
        const bool slow = slowBuild();
        const rf::Calibration thin = rf::calibrate(grounded(9.8, 100e-6), meshing(slow ? 3 : 4, slow ? 100 : 50), 0,
                                                   QStringLiteral("M1"), 100, {2e9, 10e9, 20e9});
        QVERIFY2(thin.ok(), qPrintable(thin.problems.join(" | ")));
        qWarning() << "thin: residuals" << thin.residual << "agreed to" << thin.agreedTo;
        QCOMPARE(thin.third, 1000 * 1.6180339887498949);
        QCOMPARE(thin.tried.size(), 1);
        QCOMPARE(thin.tried.first(), 1000.0);
        QCOMPARE(thin.residual.size(), 3);
        // (Never nought: the third is a strip of its own, which the line the
        //  other two read fits only as far as they are one line.)
        for (const double r : thin.residual) QVERIFY2(r > 1e-7 && r < 3e-3, qPrintable(QString::number(r)));
        QCOMPARE(thin.agreedTo, 20e9);
        QVERIFY2(thin.lines.join(' ').contains(QStringLiteral("The three agree on one line")), qPrintable(thin.lines.join(" | ")));

        const rf::Stack thick = grounded(9.8, 254e-6);
        const rf::MeshSettings how = meshing(3, slow ? 200 : 100);
        const QList<double> fs = slow ? QList<double>{6e9, 26e9} : QList<double>{6e9, 12e9, 20e9, 26e9};
        const rf::Calibration chosen = rf::calibrate(thick, how, 0, QStringLiteral("M1"), 250, fs);
        const rf::Calibration fixed = rf::calibrate(thick, how, 0, QStringLiteral("M1"), 250, fs, 1e-6, nullptr, 8 * 254 + 2 * 250);
        QVERIFY2(chosen.ok() && fixed.ok(), qPrintable(chosen.problems.join(" | ") + fixed.problems.join(" | ")));
        qWarning() << "thick: chosen" << chosen.residual << chosen.tried << chosen.stripAt << "agreed to" << chosen.agreedTo;
        qWarning() << "thick: fixed " << fixed.residual << fixed.tried << "agreed to" << fixed.agreedTo;
        QCOMPARE(fixed.tried.size(), 1);
        QCOMPARE(fixed.tried.first(), 2532.0);
        QVERIFY2(fixed.residual.last() > rf::kStripsAgree, "eight heights' strips do not agree at the top");
        QVERIFY2(chosen.tried.size() > 1 && chosen.tried.at(1) < chosen.tried.at(0), "shorter strips solved beside them");
        QVERIFY2(chosen.residual.last() < 0.5 * fixed.residual.last(), "and they agree the better there");
        QVERIFY2(chosen.agreedTo >= fixed.agreedTo, "as far up at least");
        QVERIFY2(fixed.residual.first() > 1e-6, qPrintable(QString::number(fixed.residual.first())));
        // Where the longest agree they are taken, whatever shorter ones say.
        for (int k = 0; k < fs.size(); ++k)
            if (fixed.residual.at(k) <= rf::kStripsAgree) {
                QCOMPARE(chosen.residual.at(k), fixed.residual.at(k));
                QCOMPARE(chosen.stripAt.at(k), 2532.0);
            }
        QVERIFY2(chosen.lines.join(' ').contains(QStringLiteral("shorter ones")), qPrintable(chosen.lines.join(" | ")));
        // A length given is solved as given, whatever its strips say.
        const rf::Calibration given = rf::calibrate(thick, how, 0, QStringLiteral("M1"), 250, {20e9}, 1e-6, nullptr, 800);
        QVERIFY2(given.ok(), qPrintable(given.problems.join(" | ")));
        QCOMPARE(given.tried.size(), 1);
        QCOMPARE(given.tried.first(), 800.0);
        QCOMPARE(given.strip, 800.0);
    }

    // Where the shorter strip is a whole number of half waves long its line
    // is all but the identity, and gave the line's impedance poorly - 124
    // Ohm among 145s on the phase E check's feed at 26 GHz. The third strip
    // is not, and its impedance is taken there: strips of 3 mm on 100 µm of
    // alumina, a half wave at 19.5 GHz, read the line's impedance there as
    // either side of it.
    void aHalfWaveTakesItsImpedanceFromTheThird()
    {
        const bool slow = slowBuild();
        const QList<double> fs = {18.5e9, 19.47e9, 20.5e9};
        const rf::Calibration cal = rf::calibrate(grounded(9.8, 100e-6), meshing(slow ? 2 : 3, slow ? 100 : 50), 0,
                                                  QStringLiteral("M1"), 100, fs, 1e-6, nullptr, 3000);
        QVERIFY2(cal.ok(), qPrintable(cal.problems.join(" | ")));
        for (int k = 0; k < fs.size(); ++k)
            qWarning().noquote() << QStringLiteral("%1 GHz: %2 Ohm, %3 half waves, residual %4")
                                        .arg(fs.at(k) / 1e9).arg(std::abs(cal.impedance.at(k)), 0, 'f', 3)
                                        .arg(cal.gamma.at(k).imag() * cal.length * 1e-6 / M_PI, 0, 'f', 3).arg(cal.residual.at(k), 0, 'e', 2);
        const double turns = cal.gamma.at(1).imag() * cal.length * 1e-6 / M_PI;
        QVERIFY2(std::abs(turns - 1) < 0.05, qPrintable(QString::number(turns)));
        const double either = 0.5 * (std::abs(cal.impedance.at(0)) + std::abs(cal.impedance.at(2)));
        QVERIFY2(std::abs(std::abs(cal.impedance.at(1)) / either - 1) < 1e-2,
                 qPrintable(QStringLiteral("%1 Ohm at the half wave, %2 either side").arg(std::abs(cal.impedance.at(1))).arg(either)));
        QVERIFY2(cal.lines.join(' ').contains(QStringLiteral("the third strip's is taken there")), qPrintable(cal.lines.join(" | ")));
    }

    // A structure's own feed must run straight as far as its port's own field
    // reaches - four heights over the ground and its width - for the port
    // there to be what its strips find: the phase E check's MOM capacitor,
    // its feeds 50 µm long over a ground 305 µm down, came out de-embedded
    // giving back 16 times what it was given. A feed of 50 µm into a pad on
    // 100 µm of alumina is said not to be trusted; one of 600 µm is not.
    void aFeedShorterThanItsPortsFieldIsSaid()
    {
        const bool slow = slowBuild();
        const rf::Stack stack = grounded(9.8, 100e-6);
        const rf::MeshSettings how = meshing(slow ? 2 : 3, slow ? 100 : 50);
        const QList<double> fs = {5e9};
        const rf::Calibration cal = rf::calibrate(stack, how, 0, QStringLiteral("M1"), 100, fs);
        QVERIFY2(cal.ok(), qPrintable(cal.problems.join(" | ")));
        QCOMPARE(cal.reach, 500.0);
        for (const double feed : {50.0, 600.0}) {
            QPolygonF outline;   // (the feed 100 µm wide, then a pad 500 µm square)
            outline << QPointF(0, 0) << QPointF(feed, 0) << QPointF(feed, -200) << QPointF(feed + 500, -200)
                    << QPointF(feed + 500, 300) << QPointF(feed, 300) << QPointF(feed, 100) << QPointF(0, 100);
            rf::Conductor pad;
            pad.layer = QStringLiteral("M1");
            pad.outline = outline;
            const rf::FullWave raw = rf::fullWaveOf(rf::meshOf({pad}, how, stack, 0), {pad}, stack,
                                                    {edgePort(QPointF(0, 50), QPointF(-1, 0))}, fs);
            QVERIFY2(raw.ok(), qPrintable(why(raw)));
            qWarning() << "a feed of" << feed << "um: measured" << raw.feeds.value(0).length;
            QVERIFY2(std::abs(raw.feeds.value(0).length - feed) <= 25, qPrintable(QString::number(raw.feeds.value(0).length)));
            const rf::FullWave d = rf::deembedded(raw, {{&cal, 0}});
            QVERIFY2(d.ok(), qPrintable(why(d)));
            QCOMPARE(d.deembedding.value(0).reach, 500.0);
            const bool said = d.warnings.join(' ').contains(QStringLiteral("feed is as wide as its face and alone for"));
            QVERIFY2(said == (feed < 500), qPrintable(d.warnings.join(" | ")));
        }
        // A straight line's feed is as long as was looked: no warning.
        const QList<rf::Conductor> line = {strip(0, 0, 3000, 100)};
        const rf::FullWave raw = rf::fullWaveOf(rf::meshOf(line, how, stack, 0), line, stack,
                                                {edgePort(QPointF(0, 50), QPointF(-1, 0)), edgePort(QPointF(3000, 50), QPointF(1, 0))}, fs);
        QVERIFY2(raw.ok(), qPrintable(why(raw)));
        QVERIFY2(raw.feeds.value(0).length >= 500 && raw.feeds.value(1).length >= 500,
                 qPrintable(QStringLiteral("%1, %2").arg(raw.feeds.value(0).length).arg(raw.feeds.value(1).length)));
    }

    // A de-embedding that gives back where the structure as solved does not
    // is said to be the de-embedding's: a line taken out through a
    // discontinuity that loses, which it does not.
    void aDeembeddingThatGivesBackIsSaid()
    {
        const rf::Stack stack = grounded(9.8, 100e-6);
        const rf::MeshSettings how = meshing(2, 100);
        const QList<double> fs = {5e9};
        const QList<rf::Conductor> line = {strip(0, 0, 2000, 100)};
        const rf::FullWave raw = rf::fullWaveOf(rf::meshOf(line, how, stack, 0), line, stack,
                                                {edgePort(QPointF(0, 50), QPointF(-1, 0)), edgePort(QPointF(2000, 50), QPointF(1, 0))}, fs);
        QVERIFY2(raw.ok() && raw.gain <= 1e-9, qPrintable(why(raw)));
        rf::Calibration lossy;
        lossy.metal = QStringLiteral("M1");
        lossy.width = 100;
        lossy.frequencies = fs;
        lossy.discontinuity = {{Complex(1, 0), Complex(20, 0), Complex(0, 0), Complex(1, 0)}};   // (20 Ohm in series)
        lossy.gamma = {Complex(0, 200)};
        lossy.impedance = {Complex(50, 0)};
        lossy.residual = {0};
        lossy.agreedTo = 5e9;
        const rf::FullWave d = rf::deembedded(raw, {{&lossy, 0}, {&lossy, 0}});
        QVERIFY2(d.ok(), qPrintable(why(d)));
        QVERIFY2(d.gain > 0.1 && d.gainAt == 5e9, qPrintable(QString::number(d.gain)));
        QVERIFY2(d.warnings.join(' ').contains(QStringLiteral("that is the de-embedding's")), qPrintable(d.warnings.join(" | ")));
        QVERIFY2(d.lines.join(' ').contains(QStringLiteral("that is the de-embedding's")), "said among the lines too");
        // The structure's own loss taken out is no gain: nothing said.
        lossy.discontinuity = {{Complex(1, 0), Complex(0, 0), Complex(0, 0), Complex(1, 0)}};
        const rf::FullWave plain = rf::deembedded(raw, {{&lossy, 0}, {&lossy, 0}});
        QVERIFY2(!plain.warnings.join(' ').contains(QStringLiteral("de-embedding's")), qPrintable(plain.warnings.join(" | ")));
    }

    void whatCannotBeCalibratedIsSaid()
    {
        const rf::Stack stack = grounded(9.8, 100e-6);
        const rf::MeshSettings how = meshing(4, 50);
        const rf::Calibration narrow = rf::calibrate(stack, how, 0, QStringLiteral("M1"), 0, {1e9});
        QVERIFY(!narrow.ok());
        QVERIFY2(narrow.problems.join(' ').contains(QStringLiteral("no width")), qPrintable(narrow.problems.join(' ')));
        const rf::Calibration nowhere = rf::calibrate(stack, how, 0, QStringLiteral("M9"), 100, {1e9});
        QVERIFY(!nowhere.ok());
        QVERIFY2(nowhere.problems.join(' ').contains(QStringLiteral("no metal over the ground")), qPrintable(nowhere.problems.join(' ')));
        QVERIFY(!rf::calibrate(stack, how, 0, QStringLiteral("M1"), 100, {}).ok());
        const rf::Calibration cal = rf::calibrate(stack, how, 0, QStringLiteral("M1"), 100, {1e9});
        QVERIFY2(cal.ok(), qPrintable(cal.problems.join(" | ")));
        QCOMPARE(cal.indexOf(1e9), 0);
        QCOMPARE(cal.indexOf(2e9), -1);
        const QList<rf::Conductor> line = {strip(0, 0, 1500, 100)};
        const rf::FullWave raw = rf::fullWaveOf(rf::meshOf(line, how, stack, 0), line, stack,
                                                {edgePort(QPointF(0, 50), QPointF(-1, 0)), edgePort(QPointF(1500, 50), QPointF(1, 0))},
                                                {1e9, 2e9});
        QVERIFY2(raw.ok(), qPrintable(why(raw)));
        const rf::FullWave d = rf::deembedded(raw, {{&cal, 0}, {}});
        QVERIFY(!d.ok());
        QVERIFY2(d.problems.join(' ').contains(QStringLiteral("was not solved at 2 GHz")), qPrintable(d.problems.join(' ')));
    }
};

QTEST_MAIN(TestRf25Deembed)
#include "test_rf25_deembed.moc"
