/*
 * test_rf25_box.cpp - a box's kernels (rf25/rf25_box.h): the guide's modes
 * between a ground and a lid, against the parallel plates' own in closed
 * form; the layered kernel far out, the sum of its slow modes; the
 * rectangle's 2D Green's function, against the other single series - over
 * y's modes, x's 1D function in closed form -, written here independently;
 * and a box's resonances, against a homogeneous box's f_mnp.
 *
 * The full wave of a box.
 */
#include <QtTest>

#include "rf25/rf25_box.h"
#include "rf25/rf25_green.h"

#include <cmath>
#include <complex>

namespace rf = qucs_s::rf25;
using Complex = std::complex<double>;

namespace {

constexpr double kMu0 = 4e-7 * M_PI;
const Complex kJ(0, 1);

/// Two perfect plates \a h apart (m), filled with \a epsr, split into two
/// slabs at \a split.
rf::Layers plates(double epsr, double h, double split, double tand = 0)
{
    rf::Layers l;
    l.groundBelow = l.groundAbove = true;
    rf::Layers::Slab a, b;
    a.z1 = b.z0 = split;
    b.z1 = h;
    a.epsr = b.epsr = epsr;
    a.tand = b.tand = tand;
    l.slabs << a << b;
    return l;
}

/// The rectangle's 2D Helmholtz Green's function by the other series: over
/// y's modes, each x's 1D function in closed form - converging fast where
/// the field and the source are apart in x.
Complex otherSeries(Complex k, double a, double b, double x, double y, double xs, double ys, bool neumannX, bool neumannY)
{
    Complex sum(0, 0);
    const double lo = std::min(x, xs), hi = std::max(x, xs);
    for (int n = neumannY ? 0 : 1; n < 20000; ++n) {
        const double beta = n * M_PI / b, norm = neumannY ? (n == 0 ? 1 / b : 2 / b) : 2 / b;
        const double modes = neumannY ? std::cos(beta * y) * std::cos(beta * ys) : std::sin(beta * y) * std::sin(beta * ys);
        Complex g = std::sqrt(Complex(beta * beta, 0) - k * k);
        if (g.real() < 0) g = -g;
        const Complex oneD = neumannX ? std::cosh(g * lo) * std::cosh(g * (a - hi)) / (g * std::sinh(g * a))
                                      : std::sinh(g * lo) * std::sinh(g * (a - hi)) / (g * std::sinh(g * a));
        const Complex term = norm * modes * oneD;
        sum += term;
        if (n > 50 && std::abs(term) < 1e-16 * std::abs(sum)) break;
    }
    return sum;
}

} // namespace

class TestRf25Box : public QObject
{
    Q_OBJECT

private slots:
    // Two perfect plates 2 mm apart in eps_r 4, at 60 GHz: the vector
    // kernel's modes are the plates' TE ones, kz_n = n pi/h, k_p^2 = k^2 -
    // kz_n^2 - TE1 propagating (1962 /m), TE2 below its cutoff (-1886j),
    // TE3 dying faster than asked; their residues mu (2/h) sin sin / (2 k_p),
    // written here in closed form, to 1e-6. And 10 mm away, where every
    // other mode has died as exp(-3986 rho), the layered kernel is theirs.
    void theGuidesModesAreThePlatesOwn()
    {
        const double h = 2e-3, zf = 0.6e-3, zs = 1.3e-3, f = 60e9, er = 4;
        const rf::Layers l = plates(er, h, 1e-3);
        const double k = 2 * M_PI * f * std::sqrt(kMu0 * rf::kEps0 * er);
        QStringList problems;
        const QList<rf::GuideMode> modes = rf::guideModes(l, zf, zs, f, rf::BoxKernel::Vector, 3000, &problems);
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join(" | ")));
        QCOMPARE(modes.size(), 2);
        for (const rf::GuideMode& m : modes) {
            const int n = m.kr.real() > 0 ? 1 : 2;
            const double kz = n * M_PI / h;
            const Complex kp = std::sqrt(Complex(k * k - kz * kz, 0));
            const Complex want = n == 1 ? kp : Complex(0, -std::abs(kp));
            QVERIFY2(std::abs(m.kr / want - 1.0) < 1e-6, qPrintable(QStringLiteral("TE%1: %2%3j against %4%5j").arg(n).arg(m.kr.real()).arg(m.kr.imag())
                                                                    .arg(want.real()).arg(want.imag())));
            const Complex residue = kMu0 * (2 / h) * std::sin(kz * zf) * std::sin(kz * zs) / (2.0 * want);
            QVERIFY2(std::abs(m.residue / residue - 1.0) < 1e-6,
                     qPrintable(QStringLiteral("TE%1's residue %2 against %3").arg(n).arg(std::abs(m.residue)).arg(std::abs(residue))));
        }
        const rf::WaveGreen g(l, zf, zs, f, 12e-3);
        for (const double rho : {6e-3, 10e-3}) {
            Complex sum(0, 0);
            for (const rf::GuideMode& m : modes) sum += rf::modeKernel(m, rho);
            QVERIFY2(std::abs(g.vector(rho) / sum - 1.0) < 1e-5,
                     qPrintable(QStringLiteral("%1 mm: the kernel %2, its modes %3").arg(rho * 1e3).arg(std::abs(g.vector(rho))).arg(std::abs(sum))));
        }
        // With loss, followed off the axis: tan d 0.01 moves TE1 below it.
        const QList<rf::GuideMode> lossy = rf::guideModes(plates(er, h, 1e-3, 0.01), zf, zs, f, rf::BoxKernel::Vector, 3000, &problems);
        QVERIFY2(problems.isEmpty() && lossy.size() == 2, qPrintable(problems.join(" | ")));
        const Complex kLossy = 2 * M_PI * f * std::sqrt(kMu0 * rf::kEps0 * er * Complex(1, -0.01));
        const Complex te1 = std::sqrt(kLossy * kLossy - std::pow(M_PI / h, 2));
        bool found = false;
        for (const rf::GuideMode& m : lossy) found = found || std::abs(m.kr / te1 - 1.0) < 1e-6;
        QVERIFY2(found, qPrintable(QStringLiteral("TE1 with loss: %1%2j").arg(te1.real()).arg(te1.imag())));
    }

    // The rectangle's Green's function less the free one, plus the free one,
    // is the other series' - each wall's condition either way, at a real k,
    // a complex one and one below the lowest mode - to 1e-8 of it; and less
    // the free one it is smooth at the source, its value there the limit.
    void theRectanglesGreenIsTheOtherSeries()
    {
        const QRectF box(QPointF(-1e-3, 2e-3), QPointF(4e-3, 5e-3));
        const double a = box.width(), b = box.height();
        for (const Complex k : {Complex(1500, 0), Complex(2500, -40), Complex(300, 0)})
            for (const bool nx : {false, true})
                for (const bool ny : {false, true})
                    for (const auto& [field, source] : {std::pair{QPointF(0.5e-3, 3e-3), QPointF(2.2e-3, 4.1e-3)},
                                                        std::pair{QPointF(-0.9e-3, 2.05e-3), QPointF(3.5e-3, 4.95e-3)},
                                                        std::pair{QPointF(1e-3, 3.5e-3), QPointF(1.4e-3, 3.5e-3)}}) {
                        const double rho = std::hypot(field.x() - source.x(), field.y() - source.y());
                        const Complex got = rf::rectangleGreen(k, box, field, source, nx, ny) - 0.25 * kJ * rf::hankelH0(k * rho, 2);
                        const Complex want = otherSeries(k, a, b, field.x() - box.left(), field.y() - box.top(), source.x() - box.left(),
                                                         source.y() - box.top(), nx, ny);
                        QVERIFY2(std::abs(got - want) < 1e-8 * std::abs(want) + 1e-12,
                                 qPrintable(QStringLiteral("k %1%2j, %3%4: %5 against %6").arg(k.real()).arg(k.imag()).arg(nx ? "N" : "D")
                                                .arg(ny ? "N" : "D").arg(std::abs(got)).arg(std::abs(want))));
                    }
        // Smooth at the source - not flat: the walls' images give it a slope
        // there - so its limit at the source is what it tends to a hair away,
        // two of them extrapolated.
        const QPointF at(1e-3, 3.5e-3);
        for (const QPointF way : {QPointF(1, 0), QPointF(0, 1), QPointF(0.6, -0.8)}) {
            const Complex there = rf::rectangleGreen(Complex(1500, 0), box, at, at, false, false);
            const Complex one = rf::rectangleGreen(Complex(1500, 0), box, at + 1e-8 * way, at, false, false);
            const Complex two = rf::rectangleGreen(Complex(1500, 0), box, at + 2e-8 * way, at, false, false);
            QVERIFY2(std::abs(2.0 * one - two - there) < 1e-8 * std::abs(there),
                     qPrintable(QStringLiteral("%1 against %2").arg(std::abs(there)).arg(std::abs(2.0 * one - two))));
        }
    }

    // A box 5 by 3 mm, 1 mm high, filled with eps_r 4: its resonances are
    // f_mnp = c/(2 sqrt eps) sqrt((m/a)^2 + (n/b)^2 + (p/h)^2), E_z's with m,
    // n from 1, H_z's with p from 1 and m, n not both nought, to 1e-7.
    void aBoxsResonancesAreItsFmnp()
    {
        rf::Layers l = plates(4, 1e-3, 0.4e-3);
        l.walls = true;
        l.box = QRectF(0, 0, 5e-3, 3e-3);
        const double top = 80e9, c = 1 / std::sqrt(kMu0 * rf::kEps0);
        QList<double> want;
        for (int m = 0; m < 20; ++m)
            for (int n = 0; n < 20; ++n)
                for (int p = 0; p < 20; ++p) {
                    const double fmnp = c / (2 * 2) * std::sqrt(std::pow(m / 5e-3, 2) + std::pow(n / 3e-3, 2) + std::pow(p / 1e-3, 2));
                    if (fmnp > top) continue;
                    if (m >= 1 && n >= 1) want << fmnp;                // TM_z (E_z)
                    if (p >= 1 && (m > 0 || n > 0)) want << fmnp;       // TE_z (H_z)
                }
        std::sort(want.begin(), want.end());
        const QList<rf::BoxResonance> got = rf::boxResonances(l, top);
        QCOMPARE(got.size(), want.size());
        for (int i = 0; i < got.size(); ++i)
            QVERIFY2(std::abs(got.at(i).frequency / want.at(i) - 1) < 1e-7,
                     qPrintable(QStringLiteral("%1: %2 GHz against %3").arg(i).arg(got.at(i).frequency / 1e9).arg(want.at(i) / 1e9)));
    }
};

QTEST_GUILESS_MAIN(TestRf25Box)
#include "test_rf25_box.moc"
