/*
 * test_rf25_green.cpp - the static Green's function of a layered stack
 * (rf25/rf25_green.h), held to the closed forms of the cases that have
 * one, which is what the proposal asks of it: free space; a charge over a
 * ground plane; a charge at a dielectric interface; a slab over a ground
 * plane, against its image series summed to machine precision.
 *
 * Phase B of docs/proposals/2026-10-23-rf-2p5d-moment-method.md.
 */
#include <QtTest>

#include "rf25/rf25_green.h"

#include <cmath>

namespace rf = qucs_s::rf25;

namespace {

constexpr double kEps0 = rf::kEps0;

double coulomb(double epsr, double rho)
{
    return 1.0 / (4 * M_PI * kEps0 * epsr * rho);
}

/// A stack of one slab, built by hand so that a test can have exactly the
/// interfaces it wants: \a epsr from 0 to \a height, with \a below and
/// \a above outside it.
rf::Layers oneSlab(double epsr, double height, double below, double above, bool ground = false)
{
    rf::Layers l;
    rf::Layers::Slab s;
    s.z0 = 0;
    s.z1 = height;
    s.epsr = epsr;
    s.name = QStringLiteral("D");
    l.slabs << s;
    l.epsrBelow = below;
    l.epsrAbove = above;
    l.groundBelow = ground;
    return l;
}

/// A node of a stack, as the model holds one.
rf::Layer layer(rf::Layer::Kind kind, const QString& name, double thickness, double epsr = 1)
{
    rf::Layer l;
    l.kind = kind;
    l.name = name;
    l.tag = name.toLower();
    l.thickness = thickness;
    l.epsr = epsr;
    return l;
}

} // namespace

class TestRf25Green : public QObject
{
    Q_OBJECT

private slots:
    // Bessel's J0, which libc++ does not have and POSIX does not promise:
    // its own, against values of record.
    void besselJ0IsItsOwn()
    {
        // Abramowitz and Stegun, table 9.1, and the first zero.
        QVERIFY(std::abs(rf::besselJ0(0.0) - 1.0) < 1e-8);
        QVERIFY(std::abs(rf::besselJ0(1.0) - 0.7651976866) < 1e-8);
        QVERIFY(std::abs(rf::besselJ0(2.0) - 0.2238907791) < 1e-8);
        QVERIFY(std::abs(rf::besselJ0(5.0) + 0.1775967713) < 1e-8);
        QVERIFY(std::abs(rf::besselJ0(2.404825557695773)) < 1e-8);   // its first zero
        QVERIFY(std::abs(rf::besselJ0(10.0) - (-0.2459357645)) < 1e-8);
        QVERIFY(std::abs(rf::besselJ0(50.0) - 0.0558123277) < 1e-8);
        // Even, and decaying like 1/sqrt(x).
        QCOMPARE(rf::besselJ0(-3.5), rf::besselJ0(3.5));
        QVERIFY(std::abs(rf::besselJ0(400.0)) < 0.05);
    }

    // A stack read as dielectric slabs: the metals take no room, each
    // dielectric keeps its own thickness, and the ground is noted.
    void theStackIsReadAsSlabs()
    {
        rf::Stack stack;
        stack.layers << layer(rf::Layer::Ground, QStringLiteral("GND"), 0)
                     << layer(rf::Layer::Substrate, QStringLiteral("Si"), 300e-6, 11.9)
                     << layer(rf::Layer::Dielectric, QStringLiteral("ILD1"), 1e-6, 4.1)
                     << layer(rf::Layer::Metal, QStringLiteral("M1"), 0.6e-6)
                     << layer(rf::Layer::Dielectric, QStringLiteral("ILD2"), 2e-6, 4.2);
        // (stackOf() places them in z; this reads the kinds and thicknesses.)
        const rf::Layers l = rf::layersOf(stack);
        QVERIFY2(l.ok(), qPrintable(l.problems.join(" | ")));
        QCOMPARE(l.slabs.size(), 3);
        QVERIFY(l.groundBelow);
        QCOMPARE(l.slabs.at(0).epsr, 11.9);
        QCOMPARE(l.slabs.at(0).z1, 300e-6);
        QCOMPARE(l.slabs.at(1).z0, 300e-6);
        QCOMPARE(l.slabs.at(1).z1, 301e-6);
        // The metal between ILD1 and ILD2 took no room: ILD2 begins where
        // ILD1 ended, and the stack is as high as its dielectrics.
        QCOMPARE(l.slabs.at(2).z0, 301e-6);
        QCOMPARE(l.height(), 303e-6);
        QCOMPARE(l.slabAt(300.5e-6), 1);
        QCOMPARE(l.slabAt(302e-6), 2);
        QCOMPARE(l.slabAt(400e-6), -1);
        // A z on a boundary is in the slab below it: a metal rests on the
        // dielectric under it.
        QCOMPARE(l.slabAt(301e-6), 1);

        // A stack with no dielectric has nothing to solve in.
        rf::Stack bare;
        bare.layers << layer(rf::Layer::Metal, QStringLiteral("M1"), 1e-6);
        QVERIFY(!rf::layersOf(bare).ok());
        QVERIFY2(rf::layersOf(bare).problems.first().contains(QStringLiteral("no dielectric")),
                 qPrintable(rf::layersOf(bare).problems.join(" | ")));
    }

    // With nothing to reflect from, it is Coulomb's law.
    void freeSpaceIsCoulombsLaw()
    {
        const rf::Layers l = oneSlab(1.0, 100e-6, 1.0, 1.0);
        const rf::Green g(l, 50e-6);
        QVERIFY2(g.ok(), qPrintable(g.problems().join(" | ")));
        QCOMPARE(g.epsilon(), kEps0);
        for (double rho : {1e-9, 1e-7, 1e-6, 1e-5, 1e-4, 1e-2}) {
            QVERIFY2(std::abs(g.potential(rho) - coulomb(1.0, rho)) <= 1e-9 * coulomb(1.0, rho),
                     qPrintable(QStringLiteral("rho %1: %2 vs %3").arg(rho).arg(g.potential(rho)).arg(coulomb(1.0, rho))));
            QCOMPARE(g.reflected(rho), 0.0);
        }
        // In a dielectric, the same divided by its permittivity.
        const rf::Green inGlass(oneSlab(4.0, 100e-6, 4.0, 4.0), 50e-6);
        QVERIFY(inGlass.ok());
        QCOMPARE(inGlass.epsilon(), 4 * kEps0);
        QVERIFY(std::abs(inGlass.potential(1e-6) - coulomb(4.0, 1e-6)) <= 1e-9 * coulomb(4.0, 1e-6));

        // A charge outside the stack is refused, not answered.
        const rf::Green outside(l, 200e-6);
        QVERIFY(!outside.ok());
        QVERIFY2(outside.problems().first().contains(QStringLiteral("outside the stack")),
                 qPrintable(outside.problems().join(" | ")));
    }

    // A charge over a ground plane: itself and its image, the closed form.
    void aGroundPlaneIsTheImageCharge()
    {
        const double d = 5e-6;
        const rf::Layers l = oneSlab(1.0, 100e-6, 1.0, 1.0, true);
        const rf::Green g(l, d);
        QVERIFY2(g.ok(), qPrintable(g.problems().join(" | ")));
        for (double rho : {1e-8, 1e-7, 1e-6, 5e-6, 2e-5, 1e-4, 1e-3}) {
            const double want = (1.0 / (4 * M_PI * kEps0)) * (1.0 / rho - 1.0 / std::sqrt(rho * rho + 4 * d * d));
            const double got = g.potential(rho);
            QVERIFY2(std::abs(got - want) <= 1e-8 * std::abs(want) + 1e-12 * std::abs(g.freeSpace(rho)),
                     qPrintable(QStringLiteral("rho %1 µm: %2 vs %3 (%4 relative)")
                                    .arg(rho * 1e6).arg(got).arg(want).arg(std::abs(got - want) / std::abs(want))));
        }
        // And the integral, which rests on no expansion, is the same
        // function to its own accuracy: it is the slower and rougher of
        // the two, and this is what "rougher" means.
        for (double rho : {1e-7, 1e-6, 5e-6, 1e-4, 1e-3}) {
            const double want = -1.0 / (4 * M_PI * kEps0 * std::sqrt(rho * rho + 4 * d * d));
            // (Tight enough to see the accelerated sum being used where the
            // intervals have already covered the whole range, which is the
            // plain sum's business and not the accelerator's.)
            QVERIFY2(std::abs(g.reflectedByIntegral(rho) - want) <= 2e-6 * std::abs(want),
                     qPrintable(QStringLiteral("rho %1 µm: integral %2 vs %3 (%4 relative)")
                                    .arg(rho * 1e6).arg(g.reflectedByIntegral(rho)).arg(want)
                                    .arg(std::abs(g.reflectedByIntegral(rho) - want) / std::abs(want))));
        }

        // On the plane itself there is no potential at all.
        const rf::Green on(l, 0.0);
        QVERIFY(on.ok());
        QVERIFY2(std::abs(on.potential(1e-6)) < 1e-6 * coulomb(1.0, 1e-6), qPrintable(QString::number(on.potential(1e-6))));
    }

    // A charge at a distance from one dielectric interface: itself and the
    // image of strength (e1 - e2)/(e1 + e2).
    void oneInterfaceIsItsImage()
    {
        const double epsr = 11.9, h = 100e-6, d = 3e-6;
        // The slab's own permittivity below it, so only its top reflects.
        const rf::Layers l = oneSlab(epsr, h, epsr, 1.0);
        const rf::Green g(l, h - d);
        QVERIFY2(g.ok(), qPrintable(g.problems().join(" | ")));
        const double k = (epsr - 1.0) / (epsr + 1.0);
        for (double rho : {1e-8, 1e-7, 1e-6, 3e-6, 1e-5, 1e-4}) {
            const double want = (1.0 / (4 * M_PI * kEps0 * epsr)) * (1.0 / rho + k / std::sqrt(rho * rho + 4 * d * d));
            const double got = g.potential(rho);
            QVERIFY2(std::abs(got - want) <= 1e-8 * std::abs(want),
                     qPrintable(QStringLiteral("rho %1 µm: %2 vs %3 (%4 relative)")
                                    .arg(rho * 1e6).arg(got).arg(want).arg(std::abs(got - want) / std::abs(want))));
        }
        // Its spectral part is that image's at every k: K e^(-2kd).
        for (double kk : {1e3, 1e4, 1e5, 1e6}) {
            const double want = k * std::exp(-2 * kk * d);
            QVERIFY2(std::abs(g.spectralReflection(kk) - want) <= 1e-12 + 1e-9 * std::abs(want),
                     qPrintable(QStringLiteral("k %1: %2 vs %3").arg(kk).arg(g.spectralReflection(kk)).arg(want)));
        }
    }

    // The microstrip's own case: a charge on top of a dielectric slab over
    // a ground plane, against the image series that case has in closed
    // form, summed until it stops moving.
    void aSlabOverGroundIsItsImageSeries()
    {
        const double epsr = 9.8, h = 10e-6;
        rf::Layers l = oneSlab(epsr, h, 1.0, 1.0, true);
        const rf::Green g(l, h);   // on the slab's top face, under the air
        QVERIFY2(g.ok(), qPrintable(g.problems().join(" | ")));
        const double k = (epsr - 1.0) / (epsr + 1.0);
        // S(k) = [K - (1 + 2K) e^-2kh] / (1 + K e^-2kh), whose transform
        // term by term is a sum of images at 2mh.
        const auto series = [&](double rho) {
            double sum = 0;
            for (int m = 0; m < 4000; ++m) {
                const double sign = std::pow(-k, m);
                const double a = k * sign / std::sqrt(rho * rho + std::pow(2.0 * m * h, 2));
                const double b = (1 + 2 * k) * sign / std::sqrt(rho * rho + std::pow(2.0 * (m + 1) * h, 2));
                sum += a - b;
                if (std::abs(k) < 1e-12) break;
                if (std::abs(sign) < 1e-18) break;
            }
            return sum / (4 * M_PI * kEps0 * epsr);
        };
        for (double rho : {1e-8, 1e-7, 1e-6, 1e-5, 5e-5, 1e-4, 1e-3}) {
            const double want = coulomb(epsr, rho) + series(rho);
            const double got = g.potential(rho);
            QVERIFY2(std::abs(got - want) <= 2e-7 * std::abs(want),
                     qPrintable(QStringLiteral("rho %1 µm: %2 vs %3 (%4 relative)")
                                    .arg(rho * 1e6).arg(got).arg(want).arg(std::abs(got - want) / std::abs(want))));
        }
        // Far from the charge the stack behaves as one medium of the
        // average permittivity: the potential falls off as 1/rho with
        // 2/(1 + epsr) of the slab's own strength... and nearer, more.
        QVERIFY(g.potential(1e-6) > 0);
        QVERIFY(g.potential(1e-6) > g.potential(1e-5));
    }

    // The two ways of computing it - the images, which the solver uses,
    // and the spectral integral, which rests on no expansion - are the
    // same function. This is the check the proposal asks for, of the one
    // method against the other.
    void theImagesAndTheIntegralAgree()
    {
        rf::Layers many;   // four slabs: the recursion has something to walk
        double z = 0;
        for (double epsr : {11.9, 4.1, 3.0, 7.0}) {
            rf::Layers::Slab slab;
            slab.z0 = z;
            slab.z1 = z + 2e-6;
            slab.epsr = epsr;
            many.slabs << slab;
            z = slab.z1;
        }
        many.groundBelow = true;
        const QList<rf::Layers> stacks{
            oneSlab(4.2, 2e-6, 11.9, 1.0, true),     // an oxide on silicon, grounded
            oneSlab(11.9, 50e-6, 1.0, 1.0, true),    // a grounded substrate
            oneSlab(3.9, 5e-6, 3.9, 1.0, false),     // one interface, nothing below
            oneSlab(9.8, 10e-6, 1.0, 1.0, true),     // a charge on the top face: the microstrip's own
            many,                                    // and a charge inside a stack of four
        };
        const QList<double> heights{1e-6, 25e-6, 2e-6, 10e-6, 5e-6};
        for (int i = 0; i < stacks.size(); ++i) {
            const rf::Green g(stacks.at(i), heights.at(i));
            QVERIFY2(g.ok(), qPrintable(g.problems().join(" | ")));
            QVERIFY2(!g.images().isEmpty(), "a stack with interfaces has images");
            QVERIFY2(g.truncated() < 1e-7, qPrintable(QString::number(g.truncated())));
            // A stack of several slabs drops a few terms below the floor,
            // and what it drops is counted rather than passed over: for a
            // single slab there is nothing to drop at all.
            if (g.images().size() > 40) QVERIFY2(g.truncated() > 0, "it dropped terms and reported none");
            for (double rho : {3e-9, 1.3e-7, 7e-7, 4e-6, 2.2e-5, 9e-5}) {
                const double images = g.reflected(rho);
                const double integral = g.reflectedByIntegral(rho);
                QVERIFY2(std::abs(images - integral) <= 1e-6 * std::abs(integral) + 1e-9 * std::abs(g.freeSpace(rho)),
                         qPrintable(QStringLiteral("stack %1, rho %2 µm: images %3 vs integral %4")
                                        .arg(i).arg(rho * 1e6).arg(images).arg(integral)));
            }
        }
    }

    // The two recursions themselves, before either is transformed: the
    // images are an expansion of the spectral reflection, so summing them
    // back up as a function of k must give that reflection again. This
    // compares the one recursion against the other with no integral in
    // between, so a disagreement here is the formulation's and not the
    // quadrature's.
    void theImagesSumBackToTheSpectralReflection()
    {
        rf::Layers many;
        double z = 0;
        for (double epsr : {11.9, 4.1, 3.0, 7.0}) {
            rf::Layers::Slab slab;
            slab.z0 = z;
            slab.z1 = z + 2e-6;
            slab.epsr = epsr;
            many.slabs << slab;
            z = slab.z1;
        }
        many.groundBelow = true;
        const QList<QPair<rf::Layers, double>> cases{
            {oneSlab(9.8, 10e-6, 1.0, 1.0, true), 10e-6},   // on the top face
            {oneSlab(4.2, 2e-6, 11.9, 1.0, true), 1e-6},    // inside one slab
            {many, 5e-6},                                   // inside a stack of four
            {many, 4e-6},                                   // on a boundary inside it
        };
        for (int i = 0; i < cases.size(); ++i) {
            const rf::Green g(cases.at(i).first, cases.at(i).second);
            QVERIFY2(g.ok(), qPrintable(g.problems().join(" | ")));
            for (double k : {1e3, 1e4, 3e4, 1e5, 3e5, 1e6, 1e7}) {
                double sum = 0;
                for (const rf::Green::Image& im : g.images()) sum += im.a * std::exp(-k * im.z);
                const double want = g.spectralReflection(k);
                QVERIFY2(std::abs(sum - want) <= 1e-9 + 1e-7 * std::abs(want),
                         qPrintable(QStringLiteral("case %1, k %2: images sum to %3, the recursion gives %4")
                                        .arg(i).arg(k).arg(sum).arg(want)));
            }
        }
    }

    // The images are truncated where a stack reflects too strongly for
    // them ever to die away: what was dropped is counted, not forgotten,
    // and what is left is still the function to within it.
    void whatTheTruncationDroppedIsCounted()
    {
        // Two faces that each reflect almost wholly: every bounce keeps
        // nearly all of its strength, so the series is long.
        rf::Layers hard = oneSlab(1.0, 4e-6, 1000.0, 1000.0, false);
        const rf::Green g(hard, 2e-6);
        QVERIFY2(g.ok(), qPrintable(g.problems().join(" | ")));
        // Two thousand bounces of a cavity that keeps 99.8 % of each is
        // still not the whole of it, and the counter says how much is
        // missing rather than letting it pass as nothing.
        QVERIFY2(g.truncated() > 1e-6, "a stack that reflects nearly everything lost nothing at all");
        QVERIFY2(g.truncated() < 1e-2, qPrintable(QStringLiteral("it lost %1 of the reflection").arg(g.truncated())));
        // And it is still the same function as the integral, to about what
        // the truncation threw away.
        for (double rho : {1e-7, 1e-6, 1e-5}) {
            const double images = g.reflected(rho), integral = g.reflectedByIntegral(rho);
            QVERIFY2(std::abs(images - integral) <= 1e-3 * std::abs(integral) + 1e-9 * std::abs(g.freeSpace(rho)),
                     qPrintable(QStringLiteral("rho %1 µm: images %2 vs integral %3").arg(rho * 1e6).arg(images).arg(integral)));
        }
        // A stack that barely reflects loses nothing worth counting.
        const rf::Green easy(oneSlab(1.01, 4e-6, 1.0, 1.0, false), 2e-6);
        QVERIFY(easy.ok());
        QVERIFY2(easy.truncated() < 1e-11, qPrintable(QString::number(easy.truncated())));
    }

    // A stack of several slabs: its reflections are bounded, its potential
    // falls off, and the permittivity it tends to lies between the ones it
    // is made of.
    void aStackOfSlabsIsBoundedAndFallsOff()
    {
        rf::Layers l;
        const double eps[4] = {11.9, 4.1, 3.0, 7.0};
        double z = 0;
        for (int i = 0; i < 4; ++i) {
            rf::Layers::Slab s;
            s.z0 = z;
            s.z1 = z + 2e-6;
            s.epsr = eps[i];
            l.slabs << s;
            z = s.z1;
        }
        l.groundBelow = true;
        const rf::Green g(l, 5e-6);   // inside the third slab
        QVERIFY2(g.ok(), qPrintable(g.problems().join(" | ")));
        QCOMPARE(g.epsilon(), 3.0 * kEps0);
        // A reflection coefficient is a reflection coefficient: never
        // above one, whatever the stack.
        for (double k : {1e2, 1e3, 1e4, 1e5, 1e6, 1e7}) QVERIFY(std::abs(g.spectralReflection(k)) <= 1.0);
        // The ground is below: at a distance the potential is pulled down
        // towards zero, so it falls off faster than Coulomb's law.
        QVERIFY(g.potential(1e-6) > 0);
        QVERIFY(g.potential(1e-4) < coulomb(3.0, 1e-4));
        QVERIFY(g.potential(1e-8) > 0.9 * coulomb(3.0, 1e-8));   // near the charge, its own field
    }
};

QTEST_MAIN(TestRf25Green)
#include "test_rf25_green.moc"
