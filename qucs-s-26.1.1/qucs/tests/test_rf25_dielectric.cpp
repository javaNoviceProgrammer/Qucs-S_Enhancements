/*
 * test_rf25_dielectric.cpp - a dielectric of an RF 2.5D stack that ends
 * (rf25/rf25_dielectric.h, F2 of the open items of 8 November): the panel's
 * closed form against quadrature, a wall's potential against the Green's
 * function summed over it, and a plate's capacitance over a substrate that
 * ends - against the stack's own Green's function where it ends nowhere
 * near, and between what bounds it where it ends close.
 */
#include <QtTest>

#include "rf25/rf25_dielectric.h"

#include <Eigen/Dense>

#include <cmath>

namespace rf = qucs_s::rf25;
using Point3 = std::array<double, 3>;

namespace {

/// The integral of 1/R over a parallelogram (\a c its corners, the fourth
/// opposite the second) by the midpoint rule on \a n by \a n.
double quadrature(const QList<Point3>& c, const Point3& at, int n = 400)
{
    const Point3 a = c[0], u{c[1][0] - a[0], c[1][1] - a[1], c[1][2] - a[2]}, v{c[3][0] - a[0], c[3][1] - a[1], c[3][2] - a[2]};
    const Point3 cr{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    const double area = std::sqrt(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]);
    double sum = 0;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            const double x = (i + 0.5) / n, y = (j + 0.5) / n;
            const double dx = a[0] + x * u[0] + y * v[0] - at[0], dy = a[1] + x * u[1] + y * v[1] - at[1],
                         dz = a[2] + x * u[2] + y * v[2] - at[2];
            sum += 1 / std::sqrt(dx * dx + dy * dy + dz * dz);
        }
    return sum * area / (double(n) * n);
}

/// A substrate of \a epsr, 100 µm on a ground plane; \a oxide (µm), an
/// oxide of 4 over it, the metal on that.
rf::Layers substrate(double epsr, double oxide = 0)
{
    rf::Layers l;
    l.groundBelow = true;
    rf::Layers::Slab s;
    s.z0 = 0;
    s.z1 = 100e-6;
    s.epsr = epsr;
    s.name = QStringLiteral("SUB");
    l.slabs << s;
    if (oxide > 0) {
        rf::Layers::Slab o;
        o.z0 = 100e-6;
        o.z1 = 100e-6 + oxide * 1e-6;
        o.epsr = 4;
        o.name = QStringLiteral("OX");
        l.slabs << o;
    }
    return l;
}

/// A square plate of \a w µm at \a z, in cells of 12.5 µm.
QList<rf::ChargedCell> plate(double w, double z)
{
    QList<rf::ChargedCell> cells;
    const int n = int(std::lround(w / 12.5));
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            rf::ChargedCell c;
            c.rect = QRectF(i * 12.5e-6, j * 12.5e-6, 12.5e-6, 12.5e-6);
            c.z = z;
            cells << c;
        }
    return cells;
}

/// The potential coefficients of \a cells on \a layers, as the
/// quasi-static solve has them: the strongest images in closed form, the
/// rest at the cell.
Eigen::MatrixXd coefficients(const rf::Layers& layers, const QList<rf::ChargedCell>& cells)
{
    const int n = int(cells.size());
    Eigen::MatrixXd p(n, n);
    const rf::Green g(layers, cells.first().z);
    const QList<rf::Green::Image> terms = g.strongestTerms(12);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            const QRectF r = cells.at(j).rect;
            const QPointF at = cells.at(i).rect.center();
            const QList<Point3> c{{r.left(), r.top(), 0}, {r.right(), r.top(), 0}, {r.right(), r.bottom(), 0}, {r.left(), r.bottom(), 0}};
            double sum = 0;
            for (const rf::Green::Image& t : terms) sum += t.a * rf::polygonIntegral(c, {at.x(), at.y(), t.z});
            const double far = std::hypot(at.x() - r.center().x(), at.y() - r.center().y());
            p(i, j) = sum / (4 * M_PI * g.epsilon() * r.width() * r.height()) + g.beyondStrongest(std::max(far, 1e-12), int(terms.size()));
        }
    return p;
}

double capacitance(const Eigen::MatrixXd& p)
{
    const Eigen::VectorXd one = Eigen::VectorXd::Ones(p.rows());
    return one.dot(p.partialPivLu().solve(one));
}

/// The plate's capacitance with the substrate there only inside \a shapes
/// (µm), \a beyond beyond them; and what the correction said.
double ended(const rf::Layers& layers, const QList<rf::ChargedCell>& cells, const QList<QPolygonF>& shapes, double beyond,
             QStringList* said = nullptr)
{
    rf::Extent e;
    e.layer = QStringLiteral("SUB");
    e.shapes = shapes;
    e.beyond = beyond;
    QRectF around;
    for (const rf::ChargedCell& c : cells) around = around.united(c.rect);
    const rf::DielectricPlan plan = rf::dielectricPlan(layers, {e}, around);
    const rf::DielectricCorrection d = rf::dielectricCorrection(plan, cells);
    if (said != nullptr) *said = plan.lines + d.lines + d.problems;
    if (!plan.ok() || !d.ok()) return 0;
    Eigen::MatrixXd p = coefficients(plan.background, cells);
    if (d.delta.size() > 0) p += d.delta;
    return capacitance(p);
}

QPolygonF square(double from, double to)
{
    return QPolygonF(QRectF(QPointF(from, from), QPointF(to, to)));
}

} // namespace

class TestRf25Dielectric : public QObject
{
    Q_OBJECT

private slots:
    // A plane polygon's integral of 1/R, Wilton's closed form, against the
    // midpoint rule: off it, on it, beside it, and a wall standing up.
    void aPolygonsIntegralIsItsClosedForm()
    {
        const QList<Point3> square{{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
        for (const Point3& at : QList<Point3>{{0.3, 0.4, 0.2}, {1.5, 0.2, 0.1}, {2, 3, 0.5}, {0.3, 0.4, 0.05}}) {
            const double closed = rf::polygonIntegral(square, at), sum = quadrature(square, at);
            QVERIFY2(std::abs(closed / sum - 1) < 2e-5, qPrintable(QStringLiteral("%1 against %2").arg(closed).arg(sum)));
        }
        // (On the polygon itself the midpoint rule converges slowly; its
        // own square's at its middle is 4 asinh(1) a.)
        QVERIFY(std::abs(rf::polygonIntegral(square, {0.5, 0.5, 0}) - 4 * std::asinh(1.0)) < 1e-12);
        const QList<Point3> wall{{0, 0, 0}, {1, 0.5, 0}, {1, 0.5, 0.7}, {0, 0, 0.7}};
        for (const Point3& at : QList<Point3>{{0.3, 0.6, 0.2}, {1.5, 0.2, 0.1}}) {
            const double closed = rf::polygonIntegral(wall, at), sum = quadrature(wall, at);
            QVERIFY2(std::abs(closed / sum - 1) < 2e-5, qPrintable(QStringLiteral("%1 against %2").arg(closed).arg(sum)));
        }
    }

    // A wall's potential near it - its direct path and its strong images
    // each whole, as vertical rectangles - against the Green's function
    // summed over it at sixty heights and sixty places along it: inside the
    // slab, at its interface (where the direct path and a reflection are one
    // term), beyond its height, and straight below it; in a slab of 4 under
    // air on a ground.
    void aWallsPotentialIsItsGreensFunctionSummedOverIt()
    {
        const rf::Layers layers = substrate(4);
        const QPointF a(0, 0), b(12.5e-6, 0);
        const double z0 = 85e-6, z1 = 100e-6;
        // (And straight below its middle, far: a Green's function across two
        // heights at no distance - Green::potential() is nought there.)
        for (const Point3& at : QList<Point3>{{6e-6, -3e-6, 92e-6}, {6e-6, -1e-6, 99e-6}, {6e-6, -8e-6, 100e-6}, {30e-6, -5e-6, 60e-6},
                                              {6e-6, -0.5e-6, 100e-6}, {6.25e-6, 0, 15e-6}}) {
            const double mine = rf::wallPotentialForTests(layers, 0, a, b, z0, z1, at);
            double sum = 0;
            for (int i = 0; i < 60; ++i) {
                const double zq = z0 + (i + 0.5) * (z1 - z0) / 60;
                const rf::Green g(layers, std::min(at[2], zq), std::max(at[2], zq));
                for (int j = 0; j < 60; ++j)
                    sum += g.potential(std::max(std::hypot(at[0] - (j + 0.5) * 12.5e-6 / 60, at[1]), 1e-12));
            }
            sum /= 3600;
            // (The last far: its charge at its middle, to some tenths of a per
            // cent - unguarded, nought.)
            const double within = at[2] < 50e-6 ? 1e-2 : 2e-4;
            QVERIFY2(std::abs(mine / sum - 1) < within,
                     qPrintable(QStringLiteral("at %1, %2, %3 µm: %4 against %5").arg(at[0] * 1e6).arg(at[1] * 1e6).arg(at[2] * 1e6).arg(mine).arg(sum)));
        }
    }

    // The normal field's Green's functions - each image signed by how its
    // path moves with the point's height - against the slope of the spectral
    // integral, which rests on no expansion, across a step of nanometres -
    // to 2e-4, and its table to 1e-5: a thin film of 28 between an oxide and air (a charge
    // on its faces, on either side), and a point under an oxide's interface
    // of a charge above it.
    void aNormalFieldIsItsPotentialsSlope()
    {
        rf::Layers t;
        const double zs[] = {0, 500e-6, 504.7e-6, 505.3e-6};
        const double er[] = {11.7, 3.9, 28};
        for (int i = 0; i < 3; ++i) {
            rf::Layers::Slab s;
            s.z0 = zs[i];
            s.z1 = zs[i + 1];
            s.epsr = er[i];
            s.name = QString::number(i);
            t.slabs << s;
        }
        const rf::Layers ox = substrate(9.8, 10);
        struct Case {
            const rf::Layers* layers;
            double zf, zs;
        };
        for (const Case& c : {Case{&t, 505.3e-6, 504.7e-6}, Case{&t, 504.7e-6, 505.3e-6}, Case{&t, 502e-6, 505.3e-6},
                              Case{&ox, 100e-6, 110e-6}, Case{&ox, 60e-6, 110e-6}, Case{&ox, 100e-6, 50e-6}}) {
            for (const double rho : {2e-6, 20e-6, 300e-6}) {
                const double d = std::min(1e-3 * std::hypot(rho, c.zf - c.zs), 2e-9);
                const auto potential = [&](double z) {
                    const rf::Green g(*c.layers, std::min(z, c.zs), std::max(z, c.zs));
                    return g.freeSpace(rho) + g.reflectedByIntegral(rho);
                };
                // (Both 2d below the face, where the slope is taken: under a
                // film's face that reflects 93 %, the field a few nanometres
                // down is some per cent from its value at the face.)
                const rf::Green f(*c.layers, c.zf - 2 * d, c.zs, -1);
                const double slope = -(potential(c.zf - d) - potential(c.zf - 3 * d)) / (2 * d);
                const double mine = f.normalFieldExact(rho);
                QVERIFY2(std::abs(mine / slope - 1) < 2e-4 && std::abs(f.normalField(rho) / mine - 1) < 1e-5,
                         qPrintable(QStringLiteral("%1 µm of a charge at %2, %3 µm off: %4 against %5, its table %6")
                                        .arg(c.zf * 1e6).arg(c.zs * 1e6).arg(rho * 1e6).arg(mine).arg(slope).arg(f.normalField(rho))));
            }
        }
        // Its table, against its images summed, over seven decades of
        // distance from a nanometre: within 1e-6 of the largest it is near
        // there, under the oxide's interface of a charge above it.
        {
            const rf::Green f(ox, 100e-6, 110e-6, -1);
            for (int k = 0; k <= 400; ++k) {
                const auto at = [](int j) { return 1e-9 * std::pow(1e7, j / 400.0); };
                double near = 0;
                for (int j = std::max(0, k - 3); j <= std::min(400, k + 3); ++j) near = std::max(near, std::abs(f.normalFieldExact(at(j))));
                QVERIFY2(std::abs(f.normalField(at(k)) - f.normalFieldExact(at(k))) < 1e-6 * near,
                         qPrintable(QStringLiteral("%1 µm: %2 against %3").arg(at(k) * 1e6).arg(f.normalField(at(k))).arg(f.normalFieldExact(at(k)))));
            }
        }
        // On the charge's own plane: the charge itself on the side asked for,
        // half the sheet's jump either way.
        const rf::Green below(ox, 100e-6, 100e-6, -1), above(ox, 100e-6, 100e-6, 1);
        QVERIFY(std::abs(below.strongestTerms(1).first().a) > 0 && std::abs(above.strongestTerms(1).first().a) > 0);
    }

    // The mean normal field between two parallel rectangles, in closed form,
    // against the midpoint rule over both: apart, overlapping, one inside the
    // other, side by side; in one plane, 2 pi their overlap. And a polygon's
    // solid angle against its own sum.
    void aRectanglePairsFieldIsItsClosedForm()
    {
        const auto quadrature4 = [](const QRectF& a, const QRectF& b, double w) {
            const int n = 40;
            double sum = 0;
            for (int i = 0; i < n; ++i)
                for (int j = 0; j < n; ++j)
                    for (int k = 0; k < n; ++k)
                        for (int l = 0; l < n; ++l) {
                            const double dx = b.left() + (k + 0.5) * b.width() / n - a.left() - (i + 0.5) * a.width() / n;
                            const double dy = b.top() + (l + 0.5) * b.height() / n - a.top() - (j + 0.5) * a.height() / n;
                            const double r2 = dx * dx + dy * dy + w * w;
                            sum += w / (r2 * std::sqrt(r2));
                        }
            return sum * a.width() * a.height() * b.width() * b.height() / (double(n) * n * n * n);
        };
        struct Case {
            QRectF a, b;
            double w;
        };
        for (const Case& c : {Case{QRectF(0, 0, 1, 1), QRectF(0, 0, 1, 1), 0.5}, Case{QRectF(0, 0, 1, 2), QRectF(1.5, -1, 1, 1.3), 0.3},
                              Case{QRectF(0, 0, 1, 1), QRectF(0.2, 0.1, 0.5, 0.3), 1.0}, Case{QRectF(0, 0, 2, 1), QRectF(3, 0, 1, 1), 0.2}}) {
            const double closed = rf::rectanglePairField(c.a, c.b, c.w), sum = quadrature4(c.a, c.b, c.w);
            QVERIFY2(std::abs(closed / sum - 1) < 2e-3, qPrintable(QStringLiteral("%1 against %2").arg(closed).arg(sum)));
        }
        QVERIFY(std::abs(rf::rectanglePairField(QRectF(0, 0, 1, 1), QRectF(0.5, 0, 1, 1), 0) - 2 * M_PI * 0.5) < 1e-12);
        QVERIFY(std::abs(rf::rectanglePairField(QRectF(0, 0, 1, 1), QRectF(2, 0, 1, 1), 0)) < 1e-12);
        const QList<Point3> triangle{{0, 0, 0}, {1, 0, 0}, {0.3, 0.8, 0}};
        for (const Point3& at : QList<Point3>{{0.3, 0.3, 0.2}, {1.5, 0.2, 0.4}, {0.3, 0.3, 0}, {2, 2, 0}}) {
            double sum = 0;
            const int n = 600;
            for (int i = 0; i < n; ++i)
                for (int j = 0; j < n; ++j) {
                    const double u = (i + 0.5) / n, v = (j + 0.5) / n;
                    if (u + v > 1) continue;
                    const double x = u * 1 + v * 0.3, y = v * 0.8;
                    const double r2 = (x - at[0]) * (x - at[0]) + (y - at[1]) * (y - at[1]) + at[2] * at[2];
                    sum += at[2] / (r2 * std::sqrt(r2));
                }
            sum *= 0.8 / (double(n) * n);
            const double closed = rf::solidAngle(triangle, at);
            const double want = at[2] > 0 ? sum : (at[0] < 1 ? 2 * M_PI : 0.0);
            QVERIFY2(std::abs(closed - want) < 2e-3 * std::max(want, 1.0), qPrintable(QStringLiteral("%1 against %2").arg(closed).arg(want)));
        }
    }

    // A plate of 20 µm on a film of 0.6 µm of 28 - over 4.7 µm of oxide and
    // 500 µm of silicon, and alone in air: the film there only inside a
    // square far wider than the box is its region whole, the stack's own
    // answer within 3 %. Its bound charge under the plate is all but 1/28 of
    // the plate's; what is left is what the fields between its two faces'
    // panels make of it. (Read as a difference of potentials at three
    // heights, the film under a coplanar line read more capacitance than the
    // film everywhere; averaged over four points rather than exactly, 8 %
    // high.)
    void aThinFilmIsItsStacksOwn()
    {
        for (const bool onOxide : {true, false}) {
            rf::Layers t;
            const QList<double> zs = onOxide ? QList<double>{0, 500e-6, 504.7e-6, 505.3e-6} : QList<double>{0, 10e-6, 10.6e-6};
            const QList<double> er = onOxide ? QList<double>{11.7, 3.9, 28} : QList<double>{1, 28};
            for (int i = 0; i + 1 < zs.size(); ++i) {
                rf::Layers::Slab s;
                s.z0 = zs.at(i);
                s.z1 = zs.at(i + 1);
                s.epsr = er.at(i);
                s.name = i + 2 == zs.size() ? QStringLiteral("SUB") : QString::number(i);
                t.slabs << s;
            }
            QList<rf::ChargedCell> cells;
            for (int i = 0; i < 8; ++i)
                for (int j = 0; j < 8; ++j) {
                    rf::ChargedCell c;
                    c.rect = QRectF(i * 2.5e-6, j * 2.5e-6, 2.5e-6, 2.5e-6);
                    c.z = zs.last();
                    cells << c;
                }
            const double whole = capacitance(coefficients(t, cells));
            rf::Layers air = t;
            air.slabs.last().epsr = 1;
            const double bare = capacitance(coefficients(air, cells));
            QStringList said;
            const double everywhere = ended(t, cells, {square(-1e6, 1e6)}, 1, &said);
            QVERIFY2(std::abs(everywhere / whole - 1) < 0.03 && everywhere > bare,
                     qPrintable(QStringLiteral("%1: %2 against %3 fF, air %4 | %5").arg(onOxide ? "on oxide" : "in air").arg(everywhere * 1e15)
                                    .arg(whole * 1e15).arg(bare * 1e15).arg(said.join(" | "))));
        }
    }

    // A plate on a mesa flush with it on one side and 60 µm wider on the
    // others, its panels too many for the most allowed: how fast they grow
    // away from the plate coarsened, not those next to it - the flush wall's
    // strips - within 1 % of the answer with room for all, and said; with
    // room for fewer than the plate's own cells, refused; flush all round,
    // with nothing it may coarsen, refused.
    void panelsNextToTheConductorsAreNotCoarsened()
    {
        const rf::Layers layers = substrate(9.8);
        const QList<rf::ChargedCell> cells = plate(100, 100e-6);
        QRectF around;
        for (const rf::ChargedCell& c : cells) around = around.united(c.rect);
        rf::Extent e;
        e.layer = QStringLiteral("SUB");
        e.shapes = {QPolygonF(QRectF(QPointF(-60, -60), QPointF(100, 160)))};
        e.beyond = 1;
        const rf::DielectricPlan plan = rf::dielectricPlan(layers, {e}, around);
        const auto solved = [&](int most, rf::DielectricCorrection* d) {
            *d = rf::dielectricCorrection(plan, cells, false, nullptr, most);
            if (!d->ok()) return 0.0;
            return capacitance(coefficients(plan.background, cells) + d->delta);
        };
        rf::DielectricCorrection all, fewer, none;
        const double full = solved(0, &all);
        QVERIFY2(full > 0 && !all.lines.join(' ').contains(QStringLiteral("grow by")), qPrintable(all.lines.join(" | ") + all.problems.join(" | ")));
        const double coarse = solved(int(0.8 * all.panels), &fewer);
        QVERIFY2(coarse > 0 && std::abs(coarse / full - 1) < 0.01 && fewer.panels < all.panels && fewer.lines.join(' ').contains(QStringLiteral("grow by")),
                 qPrintable(QStringLiteral("%1 against %2 fF in %3 and %4 panels | %5").arg(coarse * 1e15).arg(full * 1e15).arg(fewer.panels).arg(all.panels)
                                .arg(fewer.lines.join(" | ") + fewer.problems.join(" | "))));
        solved(int(cells.size()) / 2, &none);
        QVERIFY2(!none.ok() && none.problems.join(' ').contains(QStringLiteral("panels under a metal on the layer's face are its cells")),
                 qPrintable(none.problems.join(" | ")));
        // Flush all round, every panel next to the plate: with room for four
        // fifths of them, refused - coarsened, the strips along its edges grew
        // from 20 µm to 45 and its capacitance 5 %.
        e.shapes = {square(0, 100)};
        const rf::DielectricPlan flush = rf::dielectricPlan(layers, {e}, around);
        const rf::DielectricCorrection whole = rf::dielectricCorrection(flush, cells);
        QVERIFY2(whole.ok(), qPrintable(whole.problems.join(" | ")));
        const rf::DielectricCorrection squeezed = rf::dielectricCorrection(flush, cells, false, nullptr, int(0.8 * whole.panels));
        QVERIFY2(!squeezed.ok() && squeezed.problems.join(' ').contains(QStringLiteral("coarsened as far as it goes")),
                 qPrintable(squeezed.lines.join(" | ") + squeezed.problems.join(" | ")));
    }

    // A plate of 100 µm on 100 µm of 9.8 over a ground: the substrate there
    // only inside a square far wider than the box round the plate is its
    // region whole - the stack's Green's function's answer to 0.5 %. A
    // mesa round it, ever narrower: its capacitance ever less, between the
    // air's and the whole substrate's. Beyond it 4 rather than air: more,
    // still within them. An edge on one side: between.
    void aSubstrateThatEndsIsSolvedByItsRegion()
    {
        const rf::Layers layers = substrate(9.8);
        const QList<rf::ChargedCell> cells = plate(100, 100e-6);
        const double whole = capacitance(coefficients(layers, cells));
        rf::Layers air = layers;
        air.slabs[0].epsr = 1;
        const double bare = capacitance(coefficients(air, cells));
        QStringList said;
        const double everywhere = ended(layers, cells, {square(-1e6, 1e6)}, 1, &said);
        QVERIFY2(std::abs(everywhere / whole - 1) < 5e-3, qPrintable(QStringLiteral("%1 against %2 fF | %3").arg(everywhere * 1e15).arg(whole * 1e15).arg(said.join(" | "))));
        QVERIFY2(said.join(' ').contains(QStringLiteral("SUB ends: eps_r 9.8 inside its shapes, 1 beyond them")), qPrintable(said.join(" | ")));
        double last = everywhere * 1.01;
        for (const double margin : {200.0, 60.0, 20.0, 5.0, 0.0}) {
            const double c = ended(layers, cells, {square(-margin, 100 + margin)}, 1, &said);
            QVERIFY2(c < last && c > bare && c < whole * 1.005,
                     qPrintable(QStringLiteral("a mesa %1 µm round it: %2 fF (the last %3, air %4, whole %5) | %6")
                                    .arg(margin).arg(c * 1e15).arg(last * 1e15).arg(bare * 1e15).arg(whole * 1e15).arg(said.join(" | "))));
            last = c;
        }
        const double flushAir = last;
        const double flushFour = ended(layers, cells, {square(0, 100)}, 4, &said);
        QVERIFY2(flushFour > flushAir && flushFour < whole, qPrintable(QStringLiteral("%1 fF beyond 4, %2 beyond air").arg(flushFour * 1e15).arg(flushAir * 1e15)));
        const double edge = ended(layers, cells, {QPolygonF(QRectF(-1e6, -1e6, 1e6 + 130, 2e6))}, 1, &said);
        QVERIFY2(edge < everywhere && edge > flushAir, qPrintable(QStringLiteral("an edge 30 µm off: %1 fF").arg(edge * 1e15)));
    }

    // A plate of 300 µm on a mesa cut flush with it: at least the parallel
    // plate's eps A/h (Thomson's principle: the field straight down under
    // it alone), and its fringing no less than the plate in air has beyond
    // its own parallel plate's, a few per cent either way - the bound the
    // other way round to the region, tried and dropped, missed by half.
    void aPlateOnAMesaFlushWithItFringesAsInAir()
    {
        const rf::Layers layers = substrate(9.8);
        const QList<rf::ChargedCell> cells = plate(300, 100e-6);
        rf::Layers air = layers;
        air.slabs[0].epsr = 1;
        const double bare = capacitance(coefficients(air, cells));
        const double plateOnly = rf::kEps0 * 300e-6 * 300e-6 / 100e-6;
        QStringList said;
        const double flush = ended(layers, cells, {square(0, 300)}, 1, &said);
        const double parallel = 9.8 * plateOnly, fringe = bare - plateOnly;
        QVERIFY2(flush > parallel + 0.9 * fringe && flush < parallel + 1.5 * fringe,
                 qPrintable(QStringLiteral("%1 fF: the parallel plate %2, its fringe in air %3 | %4").arg(flush * 1e15).arg(parallel * 1e15).arg(fringe * 1e15).arg(said.join(" | "))));
    }

    // An oxide between the plate and the substrate's face: its region's face
    // an interface of two dielectrics and the metal not on it. Ending
    // nowhere near, within 2 % of the stack's own; a mesa ever narrower,
    // ever less.
    void anOxideOverTheSubstratesFace()
    {
        for (const double oxide : {1.0, 10.0}) {
            const rf::Layers layers = substrate(9.8, oxide);
            const QList<rf::ChargedCell> cells = plate(100, 100e-6 + oxide * 1e-6);
            const double whole = capacitance(coefficients(layers, cells));
            QStringList said;
            const double everywhere = ended(layers, cells, {square(-1e6, 1e6)}, 1, &said);
            // (Within 2 %: the stack's own answer moves 2 % from these cells
            // of 12.5 µm to 6.25, and the two meet as they are refined - 1.8,
            // 1.1 and 0.7 % apart at 12.5, 6.25 and 3.125 under 1 µm.)
            QVERIFY2(std::abs(everywhere / whole - 1) < 0.02,
                     qPrintable(QStringLiteral("under %1 µm of oxide: %2 against %3 fF | %4").arg(oxide).arg(everywhere * 1e15).arg(whole * 1e15).arg(said.join(" | "))));
            double last = whole * 1.02;
            for (const double margin : {60.0, 20.0, 0.0}) {
                const double c = ended(layers, cells, {square(-margin, 100 + margin)}, 1, &said);
                QVERIFY2(c < last, qPrintable(QStringLiteral("under %1 µm of oxide, a mesa %2 µm round it: %3 fF, the last %4").arg(oxide).arg(margin).arg(c * 1e15).arg(last * 1e15)));
                last = c;
            }
        }
    }

    // What the plan says where there is nothing to do: no shape within the
    // box (as beyond its shapes there), the same permittivity beyond, a layer
    // that is no dielectric of the stack.
    void whatNeedsNoRegionIsSaid()
    {
        const rf::Layers layers = substrate(9.8);
        const QRectF around(0, 0, 100e-6, 100e-6);
        rf::Extent far;
        far.layer = QStringLiteral("SUB");
        far.shapes = {square(5000, 6000)};
        rf::DielectricPlan plan = rf::dielectricPlan(layers, {far}, around);
        QVERIFY2(plan.ok() && plan.isEmpty() && plan.lines.join(' ').contains(QStringLiteral("has no shape within")), qPrintable(plan.lines.join(" | ")));
        QCOMPARE(plan.background.slabs.first().epsr, 1.0);
        rf::Extent same = far;
        same.shapes = {square(0, 50)};
        same.beyond = 9.8;
        plan = rf::dielectricPlan(layers, {same}, around);
        QVERIFY(plan.ok() && plan.isEmpty() && plan.lines.join(' ').contains(QStringLiteral("nothing changes")));
        rf::Extent none = far;
        none.layer = QStringLiteral("M1");
        plan = rf::dielectricPlan(layers, {none}, around);
        QVERIFY(!plan.ok() && plan.problems.join(' ').contains(QStringLiteral("no dielectric of the stack")));
    }
};

QTEST_MAIN(TestRf25Dielectric)
#include "test_rf25_dielectric.moc"
