/*
 * test_rf25_returns.cpp - a port that returns through the terminals and the
 * ports it names (rf25/rf25_fullwave.h, FullWavePort::terminals): the
 * coplanar port it is where the terminals are the faces beside it, a
 * probe's tips sharing its current, a port in series with another, a
 * terminal on another metal, and what it will not solve.
 *
 * The feature request of 4 November (~/Desktop/qucs-reviews/RF2p5D/
 * 2026-11-04-claude-rf25-feature-port-return-list.md): "a port that returns
 * through other ports or terminals (GSG probe landing, ground-signal, ground
 * rings)". Each return is a column of its own, the port's cells less the
 * return's, and a port's columns are tied as a coplanar port's two grounds
 * are; with no path up to them, the port's Z is the columns' own bilinear
 * form, which several tests here hold to the last digits.
 */
#include <QtTest>

#include "rf25/rf25_fullwave.h"

#include <cmath>
#include <complex>

namespace rf = qucs_s::rf25;
using Complex = std::complex<double>;

namespace {

/// A substrate of \a epsr and \a height (m) on a ground plane - or, not
/// \a plane, on air - with one metal on top of it.
rf::Stack stackOf(double epsr, double height, bool plane = true)
{
    rf::Stack s;
    rf::Layer ground;
    ground.kind = rf::Layer::Ground;
    ground.name = QStringLiteral("GND");
    ground.tag = QStringLiteral("gnd");
    if (!plane) ground.variant = QStringLiteral("open");
    rf::Layer sub;
    sub.kind = rf::Layer::Substrate;
    sub.name = QStringLiteral("SUB");
    sub.tag = QStringLiteral("sub");
    sub.thickness = height;
    sub.epsr = epsr;
    rf::Layer metal;
    metal.kind = rf::Layer::Metal;
    metal.name = QStringLiteral("M1");
    metal.tag = QStringLiteral("m1");
    s.layers << ground << sub << metal;
    return s;
}

/// \a stack as the request's structure has it: its metal 2 µm of gold, its
/// substrate's loss tangent 1e-4.
rf::Stack lossy(rf::Stack stack)
{
    for (rf::Layer& l : stack.layers) {
        if (l.kind == rf::Layer::Metal) {
            l.thickness = 2e-6;
            l.sigma = 4.1e7;
        }
        if (l.kind == rf::Layer::Substrate) l.tand = 1e-4;
    }
    return stack;
}

/// A rectangle in micrometres on \a layer.
rf::Conductor on(const QString& layer, double x, double y, double w, double h)
{
    rf::Conductor c;
    c.layer = layer;
    c.outline = QPolygonF(QRectF(x, y, w, h));
    return c;
}

/// A coplanar line \a l µm long along x on M1: a signal 50 µm wide about
/// the axis, gaps of 25 and grounds of 150 either side.
QList<rf::Conductor> cpw(double l)
{
    const QString m1 = QStringLiteral("M1");
    return {on(m1, 0, -25, l, 50), on(m1, 0, 50, l, 150), on(m1, 0, -200, l, 150)};
}

/// Those conductors meshed \a across cells across, none longer than
/// \a largest (m).
rf::Mesh meshed(const QList<rf::Conductor>& conductors, int across, double largest)
{
    rf::MeshSettings how;
    how.maximum = largest;
    how.cellsAcross = across;
    return rf::meshOf(conductors, how, rf::Stack(), 0);
}

rf::FullWavePort edgePort(const QString& name, QPointF at, QPointF facing)
{
    rf::FullWavePort p;
    p.name = name;
    p.at = at;
    p.metal = QStringLiteral("M1");
    p.facing = facing;
    return p;
}

/// An internal port over a \a side µm square at \a at on \a metal.
rf::FullWavePort patchPort(const QString& name, QPointF at, double side, const QString& metal = QStringLiteral("M1"))
{
    rf::FullWavePort p;
    p.name = name;
    p.internal = true;
    p.at = at;
    p.metal = metal;
    p.size = QSizeF(side, side);
    return p;
}

/// A terminal over a \a side µm square at \a at (the one cell under it
/// where \a side is nought), or the face facing \a facing at it.
rf::PortTerminal patch(const QString& name, QPointF at, double side, const QString& metal = QStringLiteral("M1"))
{
    rf::PortTerminal t;
    t.name = name;
    t.at = at;
    t.metal = metal;
    t.size = QSizeF(side, side);
    return t;
}

rf::PortTerminal face(const QString& name, QPointF at, QPointF facing)
{
    rf::PortTerminal t;
    t.name = name;
    t.at = at;
    t.metal = QStringLiteral("M1");
    t.face = true;
    t.facing = facing;
    return t;
}

rf::PortTerminal portNamed(const QString& name)
{
    rf::PortTerminal t;
    t.name = name;
    t.port = true;
    return t;
}

/// \a port returning through \a returns.
rf::FullWavePort returning(rf::FullWavePort port, const QList<rf::PortTerminal>& returns)
{
    port.terminals = true;
    port.returns = returns;
    return port;
}

QString why(const rf::FullWave& fw)
{
    return fw.problems.join(QStringLiteral(" | "));
}

bool says(const QStringList& lines, const QString& words)
{
    for (const QString& l : lines)
        if (l.contains(words)) return true;
    return false;
}

/// The largest difference between two solves' S and between their Z over
/// the larger of either's terms, at every frequency.
double sApart(const rf::FullWave& a, const rf::FullWave& b)
{
    double worst = 0;
    for (int k = 0; k < a.s.size(); ++k)
        for (int i = 0; i < a.s.at(k).size(); ++i)
            for (int j = 0; j < a.s.at(k).size(); ++j) worst = std::max(worst, std::abs(a.s[k][i][j] - b.s[k][i][j]));
    return worst;
}

double zApart(const rf::FullWave& a, const rf::FullWave& b)
{
    double worst = 0, largest = 0;
    for (int k = 0; k < a.z.size(); ++k)
        for (int i = 0; i < a.z.at(k).size(); ++i)
            for (int j = 0; j < a.z.at(k).size(); ++j) {
                worst = std::max(worst, std::abs(a.z[k][i][j] - b.z[k][i][j]));
                largest = std::max({largest, std::abs(a.z[k][i][j]), std::abs(b.z[k][i][j])});
            }
    return largest > 0 ? worst / largest : worst;
}

} // namespace

class TestRf25Returns : public QObject
{
    Q_OBJECT

private slots:

    // The request's first test: a port that names the faces beside it on
    // its edge's line as its returns is the coplanar port - the same
    // columns, so the same S to the rounding of their order - on a sheet,
    // and on a metal solved as its two faces, where each column's cells are
    // both faces' half each (put()). Its two returns share its current alike.
    void aPortReturningThroughTheFacesBesideItIsTheCoplanarOne()
    {
        const double l = 600;
        const QList<rf::Conductor> line = cpw(l);
        const rf::Mesh mesh = meshed(line, 3, 100e-6);
        QList<rf::FullWavePort> beside = {edgePort(QStringLiteral("P1"), QPointF(0, 0), QPointF(-1, 0)),
                                          edgePort(QStringLiteral("P2"), QPointF(l, 0), QPointF(1, 0))};
        for (rf::FullWavePort& p : beside) p.coplanar = true;
        const QList<rf::FullWavePort> named = {
            returning(beside.at(0), {face(QStringLiteral("T1"), QPointF(0, 125), QPointF(-1, 0)),
                                     face(QStringLiteral("T2"), QPointF(0, -125), QPointF(-1, 0))}),
            returning(beside.at(1), {face(QStringLiteral("T3"), QPointF(l, 125), QPointF(1, 0)),
                                     face(QStringLiteral("T4"), QPointF(l, -125), QPointF(1, 0))})};
        for (const bool faces : {false, true}) {
            rf::Stack stack = stackOf(9.8, 200e-6);
            if (faces) {
                stack.layers[2].thickness = 3e-6;
                stack.layers[2].sigma = 4.1e7;
                stack.layers[2].twoSheets = true;
            }
            const rf::FullWave a = rf::fullWaveOf(mesh, line, stack, beside, {20e9});
            const rf::FullWave b = rf::fullWaveOf(mesh, line, stack, named, {20e9});
            QVERIFY2(a.ok() && b.ok(), qPrintable(why(a) + why(b)));
            qWarning("%s: S apart by %.3g, Z by %.3g of its largest", faces ? "two faces" : "a sheet", sApart(a, b), zApart(a, b));
            QVERIFY2(sApart(a, b) < 1e-11 && zApart(a, b) < 1e-11,
                     qPrintable(QStringLiteral("%1 %2").arg(sApart(a, b)).arg(zApart(a, b))));
            // What each drives and returns through, said; the coplanar one's
            // grounds and the named returns' cells alike.
            QVERIFY2(b.portsSaid.at(0).contains(QStringLiteral("against T1 (")) && b.portsSaid.at(0).contains(QStringLiteral("and T2 ("))
                         && b.portsSaid.at(0).contains(QStringLiteral("tied: a ground-signal-ground probe's")),
                     qPrintable(b.portsSaid.join(" | ")));
            if (faces)
                QVERIFY2(b.portsSaid.at(0).contains(QStringLiteral("half of it on M1's top face")), qPrintable(b.portsSaid.join(" | ")));
            QVERIFY(b.namesReturns.at(0) && b.namesReturns.at(1) && !a.namesReturns.at(0));
            QCOMPARE(b.returnsOf.at(0).size(), 2);
            QCOMPARE(a.returnsOf.at(0).size(), 2);
            QCOMPARE(b.returnsOf.at(0).at(0).name, QStringLiteral("T1"));
            int counted = 0, coplanar = 0;
            for (int k = 0; k < 2; ++k) {
                counted += b.returnsOf.at(0).at(k).cells;
                coplanar += a.returnsOf.at(0).at(k).cells;
                // Half each, by the line's symmetry, as the coplanar port's.
                QVERIFY2(std::abs(b.returnsOf.at(0).at(k).share - 0.5) < 1e-9, qPrintable(QString::number(std::abs(b.returnsOf.at(0).at(k).share))));
                QVERIFY2(std::abs(a.returnsOf.at(0).at(k).share - 0.5) < 1e-9, qPrintable(QString::number(std::abs(a.returnsOf.at(0).at(k).share))));
            }
            QCOMPARE(counted, coplanar);
            QVERIFY2(says(b.lines, QStringLiteral("current comes back 50.0 % through T1, 50.0 % through T2")), qPrintable(b.lines.join(" | ")));
            QVERIFY2(says(b.lines, QStringLiteral("2 port(s) return through the terminals and ports they name")), qPrintable(b.lines.join(" | ")));
            QVERIFY2(b.asymmetry < 1e-9 && b.gain <= 1e-6, qPrintable(QStringLiteral("%1 %2").arg(b.asymmetry).arg(b.gain)));
            // (Over the ground plane, and needing none.)
            QVERIFY2(!says(b.lines, QStringLiteral("There is no ground plane")), qPrintable(b.lines.join(" | ")));
        }
    }

    // A GSG probe landing mid-pad: an internal port on a patch of the signal
    // returning through patches of the two grounds, with no ground plane
    // under the line - the request's second and fourth tests in small. The
    // two tips share its current half and half; one moved out to its
    // ground's far edge takes less of it, and S says so; one taken away
    // leaves a ground-signal probe, and S says that too.
    void aProbesTipsShareItsCurrent()
    {
        const double l = 600;
        const QList<rf::Conductor> line = cpw(l);
        const rf::Mesh mesh = meshed(line, 3, 50e-6);
        const rf::Stack open = stackOf(3.8, 500e-6, false);
        const auto gsg = [l](double lowerAt) {
            return QList<rf::FullWavePort>{
                returning(patchPort(QStringLiteral("P1"), QPointF(40, 0), 20),
                          {patch(QStringLiteral("T1"), QPointF(40, 125), 20), patch(QStringLiteral("T2"), QPointF(40, lowerAt), 20)}),
                returning(patchPort(QStringLiteral("P2"), QPointF(l - 40, 0), 20),
                          {patch(QStringLiteral("T3"), QPointF(l - 40, 125), 20), patch(QStringLiteral("T4"), QPointF(l - 40, -125), 20)})};
        };
        const rf::FullWave even = rf::fullWaveOf(mesh, line, open, gsg(-125), {20e9});
        QVERIFY2(even.ok(), qPrintable(why(even)));
        QVERIFY2(even.asymmetry < 1e-9 && even.gain <= 1e-6, qPrintable(QStringLiteral("%1 %2").arg(even.asymmetry).arg(even.gain)));
        QVERIFY2(even.portsSaid.at(0).contains(QStringLiteral("P1 drives the")) && even.portsSaid.at(0).contains(QStringLiteral("under its 20 by 20 µm rectangle at 40, 0 µm against T1 ("))
                     && even.portsSaid.at(0).contains(QStringLiteral("tied: a ground-signal-ground probe's")),
                 qPrintable(even.portsSaid.join(" | ")));
        const QList<rf::PortReturn>& tips = even.returnsOf.at(0);
        QCOMPARE(tips.size(), 2);
        // (The shares add up to the whole, whatever they are.)
        QVERIFY2(std::abs(tips.at(0).share + tips.at(1).share - 1.0) < 1e-9,
                 qPrintable(QStringLiteral("%1").arg(std::abs(tips.at(0).share + tips.at(1).share - 1.0))));
        QVERIFY2(std::abs(tips.at(0).share - 0.5) < 1e-6 && std::abs(tips.at(1).share - 0.5) < 1e-6,
                 qPrintable(QStringLiteral("%1 %2").arg(std::abs(tips.at(0).share)).arg(std::abs(tips.at(1).share))));
        QVERIFY2(says(even.lines, QStringLiteral("P1's current comes back 50.0 % through T1, 50.0 % through T2")), qPrintable(even.lines.join(" | ")));
        QVERIFY2(says(even.lines, QStringLiteral("There is no ground plane")), qPrintable(even.lines.join(" | ")));
        // One tip out at its ground's far edge, the other where it was.
        const rf::FullWave skewed = rf::fullWaveOf(mesh, line, open, gsg(-185), {20e9});
        QVERIFY2(skewed.ok(), qPrintable(why(skewed)));
        const double near = std::abs(skewed.returnsOf.at(0).at(0).share), far = std::abs(skewed.returnsOf.at(0).at(1).share);
        qWarning("a tip moved out: %.2f %% and %.2f %%; S11 %.3f to %.3f dB", 100 * near, 100 * far,
                 20 * std::log10(std::abs(even.s[0][0][0])), 20 * std::log10(std::abs(skewed.s[0][0][0])));
        QVERIFY2(near > far + 0.01 && std::abs(skewed.returnsOf.at(0).at(0).share + skewed.returnsOf.at(0).at(1).share - 1.0) < 1e-9,
                 qPrintable(QStringLiteral("%1 %2").arg(near).arg(far)));
        QVERIFY2(std::abs(skewed.s[0][0][0] - even.s[0][0][0]) > 1e-4, qPrintable(QString::number(std::abs(skewed.s[0][0][0] - even.s[0][0][0]))));
        QVERIFY2(says(skewed.lines, QStringLiteral("P1's current comes back %1 % through T1").arg(100 * near, 0, 'f', 1)),
                 qPrintable(skewed.lines.join(" | ")));
        QVERIFY2(skewed.asymmetry < 1e-9 && skewed.gain <= 1e-6, qPrintable(QStringLiteral("%1 %2").arg(skewed.asymmetry).arg(skewed.gain)));
        // Of a sweep, the shares at its top - whatever order its frequencies
        // are in -, which differ from a lower one's.
        const rf::FullWave low = rf::fullWaveOf(mesh, line, open, gsg(-185), {2e9});
        QVERIFY2(low.ok(), qPrintable(why(low)));
        QVERIFY2(std::abs(low.returnsOf.at(0).at(0).share - skewed.returnsOf.at(0).at(0).share) > 1e-6,
                 qPrintable(QStringLiteral("%1 %2").arg(std::abs(skewed.returnsOf.at(0).at(0).share)).arg(std::abs(low.returnsOf.at(0).at(0).share))));
        for (const QList<double>& sweep : {QList<double>{2e9, 20e9}, QList<double>{20e9, 2e9}}) {
            const rf::FullWave swept = rf::fullWaveOf(mesh, line, open, gsg(-185), sweep);
            QVERIFY2(swept.ok(), qPrintable(why(swept)));
            QVERIFY2(std::abs(swept.returnsOf.at(0).at(0).share - skewed.returnsOf.at(0).at(0).share) < 1e-12,
                     qPrintable(QStringLiteral("%1 %2").arg(std::abs(swept.returnsOf.at(0).at(0).share)).arg(std::abs(skewed.returnsOf.at(0).at(0).share))));
        }
        // A ground-signal probe: the lower tip taken away.
        QList<rf::FullWavePort> gs = gsg(-125);
        gs[0].returns.removeLast();
        const rf::FullWave single = rf::fullWaveOf(mesh, line, open, gs, {20e9});
        QVERIFY2(single.ok(), qPrintable(why(single)));
        QVERIFY2(single.portsSaid.at(0).contains(QStringLiteral("against T1 (")) && single.portsSaid.at(0).contains(QStringLiteral("a ground-signal probe's")),
                 qPrintable(single.portsSaid.join(" | ")));
        QCOMPARE(single.returnsOf.at(0).size(), 1);
        QVERIFY2(std::abs(single.returnsOf.at(0).at(0).share - 1.0) < 1e-12, qPrintable(QString::number(std::abs(single.returnsOf.at(0).at(0).share))));
        QVERIFY2(!says(single.lines, QStringLiteral("P1's current comes back")), qPrintable(single.lines.join(" | ")));
        QVERIFY2(std::abs(single.s[0][0][0] - even.s[0][0][0]) > 1e-3, qPrintable(QString::number(std::abs(single.s[0][0][0] - even.s[0][0][0]))));
        QVERIFY2(single.asymmetry < 1e-9 && single.gain <= 1e-6, qPrintable(QStringLiteral("%1 %2").arg(single.asymmetry).arg(single.gain)));
        // The probe's own current, from its tip to its signal, has the
        // radiating part of its field (SourceRun): ground-signal probes at
        // both ends, their dipoles alike, give back nothing with no plane - on
        // this lossless line 9e-4 at 40 GHz without it -, nor over one, nor on
        // gold; and nothing is warned of.
        QList<rf::FullWavePort> both = gs;
        both[1].returns.removeLast();
        for (const rf::Stack& under : {open, stackOf(3.8, 500e-6), lossy(open)}) {
            const rf::FullWave dipoles = rf::fullWaveOf(mesh, line, under, both, {40e9, 60e9});
            QVERIFY2(dipoles.ok() && dipoles.gain <= 1e-6 && dipoles.warnings.isEmpty(),
                     qPrintable(why(dipoles) + QString::number(dipoles.gain) + dipoles.warnings.join(" | ")));
        }
    }

    // A port named among another's returns: its cells that one's return,
    // and it a port of its own - the two in series. With no path up to the
    // cells, Z is the columns' bilinear form, so the series pair is the pair
    // of ports of the same cells against a common terminal, turned:
    // P1 = A - B and P2 = B - C are R1 - R2 and R2, R1 = A - C and R2 =
    // B - C, and Z_P = T' Z_R T with T = [1 0; -1 1], to the rounding. A
    // terminal on P2's cells, not P2 itself, is refused; so are two ports
    // that return through each other. Still to the rounding with the ports'
    // own currents' field (SourceRun): the two pairs are one probe of the
    // same three sides, and what flows along its legs is the cells'
    // injections alone (one straight run a column, P1's from B to A, R1's
    // and R2's from C, left them 3e-6 apart).
    void aPortNamedAmongTheReturnsIsInSeries()
    {
        const double l = 600;
        const QList<rf::Conductor> line = cpw(l);
        const rf::Mesh mesh = meshed(line, 3, 100e-6);
        const rf::Stack open = stackOf(3.8, 500e-6);
        const rf::PortTerminal c = patch(QStringLiteral("TC"), QPointF(40, -125), 20);
        const QList<rf::FullWavePort> reference = {returning(patchPort(QStringLiteral("R1"), QPointF(40, 0), 20), {c}),
                                                   returning(patchPort(QStringLiteral("R2"), QPointF(40, 125), 20), {c})};
        const QList<rf::FullWavePort> series = {returning(patchPort(QStringLiteral("P1"), QPointF(40, 0), 20), {portNamed(QStringLiteral("P2"))}),
                                                returning(patchPort(QStringLiteral("P2"), QPointF(40, 125), 20), {c})};
        const rf::FullWave r = rf::fullWaveOf(mesh, line, open, reference, {5e9, 20e9});
        const rf::FullWave p = rf::fullWaveOf(mesh, line, open, series, {5e9, 20e9});
        QVERIFY2(r.ok() && p.ok(), qPrintable(why(r) + why(p)));
        QVERIFY2(p.portsSaid.at(0).contains(QStringLiteral("against P2 (")), qPrintable(p.portsSaid.join(" | ")));
        double worst = 0, largest = 0;
        for (int k = 0; k < 2; ++k) {
            const auto& z = r.z.at(k);
            const Complex expected[2][2] = {{z[0][0] - z[0][1] - z[1][0] + z[1][1], z[0][1] - z[1][1]}, {z[1][0] - z[1][1], z[1][1]}};
            for (int i = 0; i < 2; ++i)
                for (int j = 0; j < 2; ++j) {
                    worst = std::max(worst, std::abs(p.z[k][i][j] - expected[i][j]));
                    largest = std::max(largest, std::abs(expected[i][j]));
                }
        }
        qWarning("the series pair's Z from the pair against one terminal: %.3g of its largest term", worst / largest);
        QVERIFY2(worst < 1e-9 * largest, qPrintable(QStringLiteral("%1 of %2").arg(worst).arg(largest)));
        QVERIFY2(p.asymmetry < 1e-9 && p.gain <= 1e-6, qPrintable(QStringLiteral("%1 %2").arg(p.asymmetry).arg(p.gain)));
        // A terminal on what P2 drives is not P2: refused, with the remedy.
        QList<rf::FullWavePort> overlapping = series;
        overlapping[0].returns = {patch(QStringLiteral("TB"), QPointF(40, 125), 20)};
        const rf::FullWave o = rf::fullWaveOf(mesh, line, open, overlapping, {20e9});
        QVERIFY(!o.ok());
        QVERIFY2(why(o).contains(QStringLiteral("P1's return TB is on the cells P2 drives: name P2 itself among P1's returns")), qPrintable(why(o)));
        // Two ports that return through each other drive nothing between them.
        QList<rf::FullWavePort> loop = series;
        loop[1].returns = {portNamed(QStringLiteral("P1"))};
        const rf::FullWave round = rf::fullWaveOf(mesh, line, open, loop, {20e9});
        QVERIFY(!round.ok());
        QVERIFY2(why(round).contains(QStringLiteral("P1, P2 drive no cell between them")) || why(round).contains(QStringLiteral("P2, P1 drive no cell between them")),
                 qPrintable(why(round)));
        // Three round a ring alike.
        QList<rf::FullWavePort> ring = {returning(patchPort(QStringLiteral("P1"), QPointF(40, 0), 20), {portNamed(QStringLiteral("P2"))}),
                                        returning(patchPort(QStringLiteral("P2"), QPointF(40, 125), 20), {portNamed(QStringLiteral("P3"))}),
                                        returning(patchPort(QStringLiteral("P3"), QPointF(40, -125), 20), {portNamed(QStringLiteral("P1"))})};
        const rf::FullWave three = rf::fullWaveOf(mesh, line, open, ring, {20e9});
        QVERIFY(!three.ok());
        QVERIFY2(why(three).contains(QStringLiteral("drive no cell between them")) && why(three).contains(QStringLiteral("P3")), qPrintable(why(three)));
    }

    // Beyond a GSG (the request's own list): a GSSG's two signals each
    // against the same two tips, one on each ground, at either end, the two
    // at an end a differential pair. Their four columns at an end come round
    // to a loop through the two ports' ties that no port's current is, which
    // the loop check lets through as it lets a coplanar GSSG's; and the pair's
    // modes are a symmetric pair's, neither turned into the other.
    void aGssgsSignalsReturnThroughTheSameTwoTips()
    {
        const rf::Stack open = stackOf(12.4, 150e-6, false);
        const double l = 600;
        const QString m1 = QStringLiteral("M1");
        const QList<rf::Conductor> line = {on(m1, 0, 25, l, 50), on(m1, 0, -75, l, 50), on(m1, 0, 110, l, 300), on(m1, 0, -410, l, 300)};
        const rf::Mesh mesh = meshed(line, 3, 100e-6);
        const auto end = [](double x, const QString& plus, const QString& minus, const QString& upper, const QString& lower) {
            const QList<rf::PortTerminal> tips = {patch(upper, QPointF(x, 260), 20), patch(lower, QPointF(x, -260), 20)};
            QList<rf::FullWavePort> two = {returning(patchPort(plus, QPointF(x, 50), 20), tips), returning(patchPort(minus, QPointF(x, -50), 20), tips)};
            two[0].pair = minus;
            return two;
        };
        const QList<rf::FullWavePort> ports = end(30, QStringLiteral("P1"), QStringLiteral("P2"), QStringLiteral("T1"), QStringLiteral("T2"))
                                              + end(l - 30, QStringLiteral("P3"), QStringLiteral("P4"), QStringLiteral("T3"), QStringLiteral("T4"));
        const rf::FullWave w = rf::fullWaveOf(mesh, line, open, ports, {2e9});
        QVERIFY2(w.ok(), qPrintable(why(w)));
        QCOMPARE(w.pairs, (QList<QPair<int, int>>{qMakePair(0, 1), qMakePair(2, 3)}));
        QVERIFY2(w.asymmetry < 1e-9 && w.gain <= 1e-6, qPrintable(QStringLiteral("%1 %2").arg(w.asymmetry).arg(w.gain)));
        for (int p = 0; p < 4; ++p) {
            double total = 0;
            for (const rf::PortReturn& r : w.returnsOf.at(p)) total += r.share.real();
            QVERIFY2(w.returnsOf.at(p).size() == 2 && std::abs(total - 1) < 1e-9, qPrintable(QString::number(total)));
        }
        const QList<QList<Complex>> modes = rf::mixedModeOf(w, 0);
        double converted = 0;
        for (int i = 0; i < 2; ++i)
            for (int j = 2; j < 4; ++j) converted = std::max({converted, std::abs(modes[i][j]), std::abs(modes[j][i])});
        QVERIFY2(converted < 1e-6, qPrintable(QString::number(converted)));
    }

    // The port's own current, where it comes up by no path, radiates: a
    // port between two small pads a millimetre apart in vacuum, nothing else
    // there - its current from one pad to the other, its charges at the
    // ends, is a Hertzian dipole's, and its resistance is the dipole's
    // radiation, 80 pi^2 (d/lambda)^2. With the scalar potential alone - the
    // source's current with no field of its own, as it was - it came out
    // at minus half of that (G9's -R/2, horizontal), and a lossless line
    // with a ground-signal probe at each end gave back 9e-4 at 40 GHz.
    void aPortsOwnCurrentRadiatesAsAHertzianDipole()
    {
        const rf::Stack vacuum = stackOf(1.0, 1e-3, false);
        const QString m1 = QStringLiteral("M1");
        const double d = 1000, side = 20;
        const QList<rf::Conductor> pads = {on(m1, -side / 2, -side / 2, side, side), on(m1, d - side / 2, -side / 2, side, side)};
        const rf::Mesh mesh = meshed(pads, 2, 100e-6);
        const QList<rf::FullWavePort> port = {returning(patchPort(QStringLiteral("P1"), QPointF(d, 0), side), {patch(QStringLiteral("T1"), QPointF(0, 0), side)})};
        // Dense, and held compressed: each solve gives it its field.
        rf::FullWaveControl compressed;
        compressed.matrix = rf::FullWaveControl::Matrix::Compressed;
        for (const rf::FullWaveControl* held : {static_cast<const rf::FullWaveControl*>(nullptr), static_cast<const rf::FullWaveControl*>(&compressed)})
            for (const double f : {5e9, 10e9}) {
                const rf::FullWave w = rf::fullWaveOf(mesh, pads, vacuum, port, {f}, 1e-6, held);
                QVERIFY2(w.ok() && w.compressed == (held != nullptr), qPrintable(why(w)));
                const double lambda = 299792458.0 / f, hertz = 80 * M_PI * M_PI * std::pow(d * 1e-6 / lambda, 2);
                const double r = w.z.at(0).at(0).at(0).real();
                qWarning("a dipole of 1 mm in vacuum at %g GHz, %s: %.5g Ohm, Hertz's %.5g", f / 1e9, held ? "compressed" : "dense", r, hertz);
                QVERIFY2(std::abs(r / hertz - 1) < 0.02, qPrintable(QStringLiteral("%1 Ohm, Hertz's %2").arg(r).arg(hertz)));
            }
    }

    // The request's fifth test: a signal's patch on M2 against a terminal on
    // M1 under it, no via and no ground plane, is today's internal port from
    // M1 without its path up: the same columns, the same S to the rounding
    // once that port's path is left out (FullWaveControl::portPaths). With
    // it, the two differ by the path's own field - a few tenths of an ohm,
    // its inductance up the 5 µm between the metals.
    void aTerminalOnAnotherMetalIsItsFromMetalWithoutAPath()
    {
        rf::Stack two = stackOf(3.8, 200e-6, false);
        rf::Layer ild;
        ild.kind = rf::Layer::Dielectric;
        ild.name = QStringLiteral("ILD");
        ild.tag = QStringLiteral("ild");
        ild.thickness = 5e-6;
        ild.epsr = 4;
        rf::Layer m2;
        m2.kind = rf::Layer::Metal;
        m2.name = QStringLiteral("M2");
        m2.tag = QStringLiteral("m2");
        two.layers << ild << m2;
        const double l = 600;
        const QList<rf::Conductor> line = {on(QStringLiteral("M2"), 0, -25, l, 50), on(QStringLiteral("M1"), 0, -100, l, 200)};
        const rf::Mesh mesh = meshed(line, 3, 100e-6);
        QList<rf::FullWavePort> from = {patchPort(QStringLiteral("P1"), QPointF(20, 0), 20, QStringLiteral("M2")),
                                        patchPort(QStringLiteral("P2"), QPointF(l - 20, 0), 20, QStringLiteral("M2"))};
        for (rf::FullWavePort& p : from) p.from = QStringLiteral("M1");
        const QList<rf::FullWavePort> named = {
            returning(patchPort(QStringLiteral("P1"), QPointF(20, 0), 20, QStringLiteral("M2")), {patch(QStringLiteral("T1"), QPointF(20, 0), 20)}),
            returning(patchPort(QStringLiteral("P2"), QPointF(l - 20, 0), 20, QStringLiteral("M2")), {patch(QStringLiteral("T2"), QPointF(l - 20, 0), 20)})};
        rf::FullWaveControl pathless;
        pathless.portPaths = false;
        const rf::FullWave a = rf::fullWaveOf(mesh, line, two, from, {10e9, 30e9}, 1e-6, &pathless);
        const rf::FullWave b = rf::fullWaveOf(mesh, line, two, named, {10e9, 30e9});
        const rf::FullWave pathed = rf::fullWaveOf(mesh, line, two, from, {10e9, 30e9});
        QVERIFY2(a.ok() && b.ok() && pathed.ok(), qPrintable(why(a) + why(b) + why(pathed)));
        qWarning("from M1 with no path, and against a terminal on M1: S apart by %.3g", sApart(a, b));
        QVERIFY2(sApart(a, b) < 1e-11 && zApart(a, b) < 1e-11, qPrintable(QStringLiteral("%1 %2").arg(sApart(a, b)).arg(zApart(a, b))));
        QVERIFY2(!says(a.lines, QStringLiteral("path(s) of their own")) && says(pathed.lines, QStringLiteral("path(s) of their own")),
                 qPrintable(a.lines.join(" | ")));
        QVERIFY2(b.portsSaid.at(0).contains(QStringLiteral("against T1 (")), qPrintable(b.portsSaid.join(" | ")));
        // Its path's own: a reactance, small beside the port's, growing with
        // the frequency.
        const Complex low = pathed.z[0][0][0] - b.z[0][0][0], high = pathed.z[1][0][0] - b.z[1][0][0];
        qWarning("the path up 5 µm adds %.4g%+.4gj Ohm at 10 GHz, %.4g%+.4gj at 30", low.real(), low.imag(), high.real(), high.imag());
        QVERIFY2(std::abs(low) > 1e-6 && std::abs(low) < 0.05 * std::abs(b.z[0][0][0]) && std::abs(high.imag()) > std::abs(low.imag()),
                 qPrintable(QStringLiteral("%1 %2").arg(std::abs(low)).arg(std::abs(high))));
        QVERIFY2(b.asymmetry < 1e-9 && b.gain <= 1e-6, qPrintable(QStringLiteral("%1 %2").arg(b.asymmetry).arg(b.gain)));
    }

    // The request's sixth: what such a port cannot be is refused, by name -
    // a return on its own conductor, none named, a terminal over no cell or
    // on no face, a port that is not there or itself, a name twice, and a
    // calibration by strips of a feed it has not.
    void whatAPortThatNamesItsReturnsCannotBeIsSaid()
    {
        const double l = 600;
        const QList<rf::Conductor> line = cpw(l);
        const rf::Mesh mesh = meshed(line, 3, 100e-6);
        const rf::Stack open = stackOf(3.8, 500e-6, false);
        const rf::FullWavePort p1 = patchPort(QStringLiteral("P1"), QPointF(40, 0), 20);
        const auto refused = [&](const QList<rf::PortTerminal>& returns, const QString& said, bool calibrate = false) {
            rf::FullWavePort port = returning(p1, returns);
            port.calibrate = calibrate;
            const rf::FullWave w = rf::fullWaveOf(mesh, line, open, {port}, {20e9});
            if (w.ok()) return QStringLiteral("solved");
            return why(w).contains(said) ? QString() : why(w);
        };
        const rf::PortTerminal ground = patch(QStringLiteral("T1"), QPointF(40, 125), 20);
        QCOMPARE(refused({ground}, QString()), QStringLiteral("solved"));
        QCOMPARE(refused({patch(QStringLiteral("T2"), QPointF(300, 0), 20)}, QStringLiteral("P1's return T2 is on the same conductor as P1")), QString());
        QCOMPARE(refused({}, QStringLiteral("P1 returns through the terminals and ports it names, and names none")), QString());
        QCOMPARE(refused({patch(QStringLiteral("T3"), QPointF(40, 900), 20)},
                         QStringLiteral("P1's return T3, a 20 by 20 µm rectangle at 40, 900 µm, is over no cell of M1")),
                 QString());
        QCOMPARE(refused({patch(QStringLiteral("T4"), QPointF(40, 900), 0)}, QStringLiteral("P1's return T4, at 40, 900 µm, is on no shape of M1")), QString());
        rf::PortTerminal nowhere = face(QStringLiteral("T5"), QPointF(0, 125), QPointF(-1, 0));
        nowhere.metal = QStringLiteral("M9");
        QCOMPARE(refused({nowhere}, QStringLiteral("P1's return T5, at 0, 125 µm, is on no face of M9")), QString());
        QCOMPARE(refused({portNamed(QStringLiteral("P7"))}, QStringLiteral("P1 returns through P7, and there is no port called that")), QString());
        QCOMPARE(refused({portNamed(QStringLiteral("P1"))}, QStringLiteral("P1 returns through itself")), QString());
        QCOMPARE(refused({ground, ground}, QStringLiteral("P1 names T1 twice among its returns")), QString());
        QCOMPARE(refused({ground}, QStringLiteral("P1 returns through the terminals it names (T1), and its own discontinuity cannot be taken out"), true),
                 QString());
        // A plane is wanted by none of them, and one under the stack is no
        // return of theirs: over a plane it solves as it is.
        const rf::FullWave over = rf::fullWaveOf(mesh, line, stackOf(3.8, 500e-6), {returning(p1, {ground})}, {20e9});
        QVERIFY2(over.ok(), qPrintable(why(over)));
    }

    // The request's seventh, in small: a GSG landing held compressed is the
    // dense solve - GMRES converging, S within a part in 1e5 - and both are
    // reciprocal and passive, on the request's own quartz and gold with
    // nothing below.
    void aCompressedSolveOfTheTipsIsTheDenseOne()
    {
        const double l = 600;
        const QList<rf::Conductor> line = cpw(l);
        const rf::Mesh mesh = meshed(line, 3, 50e-6);
        const rf::Stack open = lossy(stackOf(3.8, 500e-6, false));
        const QList<rf::FullWavePort> gsg = {
            returning(patchPort(QStringLiteral("P1"), QPointF(40, 0), 20),
                      {patch(QStringLiteral("T1"), QPointF(40, 125), 20), patch(QStringLiteral("T2"), QPointF(40, -125), 20)}),
            returning(patchPort(QStringLiteral("P2"), QPointF(l - 40, 0), 20),
                      {patch(QStringLiteral("T3"), QPointF(l - 40, 125), 20), patch(QStringLiteral("T4"), QPointF(l - 40, -125), 20)})};
        rf::FullWaveControl dense, compressed;
        dense.matrix = rf::FullWaveControl::Matrix::Dense;
        compressed.matrix = rf::FullWaveControl::Matrix::Compressed;
        const rf::FullWave d = rf::fullWaveOf(mesh, line, open, gsg, {2e9, 60e9}, 1e-6, &dense);
        const rf::FullWave c = rf::fullWaveOf(mesh, line, open, gsg, {2e9, 60e9}, 1e-6, &compressed);
        QVERIFY2(d.ok() && c.ok(), qPrintable(why(d) + why(c)));
        QVERIFY(c.compressed && !d.compressed);
        qWarning("compressed: %d iteration(s) at most, residual %.2g; S apart from the dense by %.3g", c.iterations, c.residual, sApart(c, d));
        QVERIFY2(c.iterations < 3000 && c.residual < 1e-9, qPrintable(QStringLiteral("%1 %2").arg(c.iterations).arg(c.residual)));
        QVERIFY2(sApart(c, d) < 1e-5, qPrintable(QString::number(sApart(c, d))));
        for (const rf::FullWave* w : {&d, &c}) {
            QVERIFY2(w->asymmetry < 1e-6 && w->gain <= 1e-6, qPrintable(QStringLiteral("%1 %2").arg(w->asymmetry).arg(w->gain)));
            QVERIFY2(std::abs(std::abs(w->returnsOf.at(0).at(0).share) - 0.5) < 1e-5, qPrintable(QString::number(std::abs(w->returnsOf.at(0).at(0).share))));
        }
    }
};

QTEST_GUILESS_MAIN(TestRf25Returns)
#include "test_rf25_returns.moc"
