/*
 * rf25_fullwave.cpp - the full-wave solve of an RF 2.5D model: the current
 *                     on its metals at each frequency, and what its ports see
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "rf25_fullwave.h"

#include <QCoreApplication>
#include <QHash>
#include <QRectF>

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>

namespace qucs_s::rf25 {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::rf25", text);
}

constexpr double kMu0 = 4e-7 * M_PI;
const Complex kJ(0, 1);
/// Beyond this many runs and cells together the dense complex solve is not
/// set up: the inductance's bound - some 200 MB (two static inductance
/// matrices, the system and its LU) and, measured at 2858, 0.8 s a
/// frequency after 0.4 s once, on a desktop of 2026.
constexpr int kMostUnknowns = 3000;
/// How small a charged cell must be beside the distance the stack's
/// remainder changes over - the distance itself, or the remainder's reach,
/// whichever is the longer - for its charge to be taken at its centre: its
/// longest side squared over that distance squared, which is twelve times
/// the most the centre is out by, here a part in 1e4. Larger, the remainder
/// is integrated over the cell. (Judged by the square root of its area, as
/// phase B judged its own near pairs until this round, a cell four times as
/// long as it is wide was taken at its centre where the centre was a part
/// in a hundred out.)
constexpr double kCentreBound = 1.2e-3;
/// The most Gauss points along a side of a cell, for the remainder.
constexpr int kMostGauss = 8;

/// Gauss-Legendre's nodes and weights on [-1/2, 1/2], the weights summing
/// to one: \a n points, 1 to kMostGauss, by Newton's method on Legendre's
/// polynomial once and kept.
struct Rule {
    QList<double> at, weight;
};

const Rule& gaussRule(int n)
{
    static const QList<Rule> rules = [] {
        QList<Rule> all;
        all << Rule{};   // (none of order nought)
        for (int order = 1; order <= kMostGauss; ++order) {
            Rule r;
            for (int i = 0; i < order; ++i) {
                double x = std::cos(M_PI * (i + 0.75) / (order + 0.5));
                double p = 0, dp = 1;
                for (int pass = 0; pass < 100; ++pass) {
                    double p0 = 1, p1 = x;
                    for (int k = 2; k <= order; ++k) {
                        const double p2 = ((2 * k - 1) * x * p1 - (k - 1) * p0) / k;
                        p0 = p1;
                        p1 = p2;
                    }
                    p = p1;
                    dp = order * (x * p1 - p0) / (x * x - 1);
                    const double step = p / dp;
                    x -= step;
                    if (std::abs(step) < 1e-16) break;
                }
                r.at << x / 2;
                r.weight << 1.0 / ((1 - x * x) * dp * dp);   // (2/((1 - x^2) P'^2), halved)
            }
            all << r;
        }
        return all;
    }();
    return rules.at(std::clamp(n, 1, kMostGauss));
}

/// How many Gauss points along a side \a length long, for a remainder that
/// changes over \a scale: two while the side is short beside it, more as
/// it is not.
int pointsAlong(double length, double scale)
{
    if (!(scale > 0)) return 2;
    return std::clamp(1 + int(std::ceil(4 * length / scale)), 2, kMostGauss);
}

/// (exp(-j k r) - 1)/r: what the retardation adds to a static 1/r, finite
/// at nought (-j k there) and smooth, so it is taken at the centres.
Complex retardation(Complex k, double r)
{
    const Complex x = -kJ * k * r;
    if (std::abs(x) < 1e-4) return -kJ * k * (1.0 + x / 2.0 + x * x / 6.0);
    return (std::exp(x) - 1.0) / r;
}

/// The stack's scalar remainder averaged over \a rect (m) at \a at, by
/// Gauss's rule with as many points as a remainder changing over \a scale
/// wants.
Complex remainderOver(const WaveGreen& g, const QRectF& rect, const QPointF& at, double scale)
{
    const Rule& rx = gaussRule(pointsAlong(rect.width(), scale));
    const Rule& ry = gaussRule(pointsAlong(rect.height(), scale));
    const QPointF centre = rect.center();
    Complex sum(0, 0);
    for (int i = 0; i < rx.at.size(); ++i)
        for (int j = 0; j < ry.at.size(); ++j) {
            const double x = centre.x() + rx.at.at(i) * rect.width() - at.x();
            const double y = centre.y() + ry.at.at(j) * rect.height() - at.y();
            sum += rx.weight.at(i) * ry.weight.at(j) * g.scalarRemainder(std::hypot(x, y));
        }
    return sum;
}

/// Each of a port's cells' share of the face it drives: the length of its
/// side on the face's line over all of theirs - or, a port on no face, the
/// one cell's whole.
QList<double> sharesOf(const Mesh& mesh, const QList<int>& cells, QPointF on, QPointF along)
{
    QList<double> lengths(cells.size(), 0.0);
    const double n = std::hypot(along.x(), along.y());
    double sum = 0;
    if (n > 0) {
        const QPointF unit = along / n, normal(-unit.y(), unit.x());
        for (int k = 0; k < cells.size(); ++k) {
            const QPolygonF& outline = mesh.cells.at(cells.at(k)).outline;
            const double tol = 1e-6 * std::max(1.0, std::max(outline.boundingRect().width(), outline.boundingRect().height()));
            for (int i = 0, m = int(outline.size()); i < m; ++i) {
                const QPointF a = outline.at(i), b = outline.at((i + 1) % m);
                const double offA = (a.x() - on.x()) * normal.x() + (a.y() - on.y()) * normal.y();
                const double offB = (b.x() - on.x()) * normal.x() + (b.y() - on.y()) * normal.y();
                if (std::abs(offA) > tol || std::abs(offB) > tol) continue;
                lengths[k] = std::max(lengths.at(k), std::hypot(b.x() - a.x(), b.y() - a.y()));
            }
            sum += lengths.at(k);
        }
    }
    if (!(sum > 0)) return QList<double>(cells.size(), 1.0 / std::max<qsizetype>(1, cells.size()));
    for (double& l : lengths) l /= sum;
    return lengths;
}

QRectF scaledBy(const QRectF& box, double unit)
{
    return QRectF(box.left() * unit, box.top() * unit, box.width() * unit, box.height() * unit);
}

using MatrixC = Eigen::Matrix<Complex, Eigen::Dynamic, Eigen::Dynamic>;

} // namespace

Complex sheetImpedance(double sigma, double thickness, double frequency)
{
    // Half the slab of half the thickness: the current divides between the
    // two faces, each carrying it in from its own side.
    if (!(sigma > 0) || !(thickness > 0)) return Complex(0, 0);
    return 0.5 * surfaceImpedance(sigma, thickness / 2, frequency);
}

QList<QList<Complex>> scatteringOf(const QList<QList<Complex>>& z, double reference)
{
    const int n = int(z.size());
    MatrixC a(n, n), b(n, n);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            a(i, j) = z.at(i).at(j) - (i == j ? reference : 0.0);
            b(i, j) = z.at(i).at(j) + (i == j ? reference : 0.0);
        }
    // (Z - Z0) and (Z + Z0) are both functions of Z, so they commute and
    // the inverse may be taken on either side.
    const MatrixC s = Eigen::PartialPivLU<MatrixC>(b).solve(a);
    QList<QList<Complex>> out;
    for (int i = 0; i < n; ++i) {
        QList<Complex> row;
        for (int j = 0; j < n; ++j) row << s(i, j);
        out << row;
    }
    return out;
}

FullWave fullWaveOf(const Mesh& mesh, const QList<Conductor>& conductors, const Stack& stack, const QList<QPointF>& ports,
                    const QList<double>& frequencies, double reference, double micrometre)
{
    FullWave out;
    out.reference = reference;
    if (mesh.cells.isEmpty()) {
        out.problems << tr("There are no cells to solve on: mesh the layout first.");
        return out;
    }
    if (ports.isEmpty()) {
        out.problems << tr("A full-wave solve needs a port to drive it: put one on the layout.");
        return out;
    }
    if (frequencies.isEmpty()) {
        out.problems << tr("No frequency to solve at.");
        return out;
    }
    for (const double f : frequencies)
        if (!(f > 0) || !std::isfinite(f)) {
            out.problems << tr("A frequency of %1 Hz cannot be solved at: the full-wave solve needs one above nought "
                               "(at nought the quasi-static extraction answers).")
                                .arg(f);
            return out;
        }
    if (!(reference > 0) || !std::isfinite(reference)) {
        out.problems << tr("A reference impedance of %1 Ohm is none to give S parameters in.").arg(reference);
        return out;
    }
    const Layers layers = layersOf(stack);
    if (!layers.ok()) {
        out.problems << layers.problems;
        return out;
    }
    if (!layers.groundBelow) {
        out.problems << tr("The full-wave solve refers its ports to a ground plane, and the stack has none: put a ground "
                           "under it.");
        return out;
    }

    QList<QPair<int, int>> ends;
    const QList<Filament> runs = runsOf(mesh, conductors, stack, micrometre, &ends);
    for (const Filament& f : runs)
        if (f.z0 != f.z1) {
            out.problems << tr("A via joins two metals, and its current runs up and down, which the full-wave solve "
                               "does not do yet: solve one metal at a time, or use the quasi-static extraction.");
            return out;
        }
    if (runs.isEmpty()) {
        out.problems << tr("No two cells of the mesh share an edge, so there is nowhere for a current to flow. Mesh the "
                           "layout more finely.");
        return out;
    }

    // The cells the runs join - the charges - in the mesh's order.
    QList<int> cellOf;
    for (const QPair<int, int>& e : std::as_const(ends)) cellOf << e.first << e.second;
    std::sort(cellOf.begin(), cellOf.end());
    cellOf.erase(std::unique(cellOf.begin(), cellOf.end()), cellOf.end());
    QHash<int, int> nodeOf;
    for (int i = 0; i < cellOf.size(); ++i) nodeOf.insert(cellOf.at(i), i);
    const int nr = int(runs.size()), nc = int(cellOf.size());
    out.runs = nr;
    out.cells = nc;
    if (nr + nc > kMostUnknowns) {
        out.problems << tr("%1 runs of current over %2 cells is more than this build sets up a dense complex matrix for "
                           "(%3 in all). Mesh the layout more coarsely: fewer cells across a conductor, or a larger "
                           "largest cell.")
                            .arg(nr).arg(nc).arg(kMostUnknowns);
        return out;
    }
    QList<int> tail(nr), head(nr);
    for (int a = 0; a < nr; ++a) {
        tail[a] = nodeOf.value(ends.at(a).first);
        head[a] = nodeOf.value(ends.at(a).second);
    }

    // The ports: each drives the face of the conductor it is on, its
    // current shared out by the length of face each cell has.
    const int np = int(ports.size());
    QList<QList<int>> portNodes;
    QList<QList<double>> portShares;
    for (int p = 0; p < np; ++p) {
        QPointF on, along;
        const QList<int> face = faceCells(mesh, conductors, cellOf, ports.at(p), &on, &along);
        if (face.isEmpty()) {
            out.problems << tr("Port %1, at %2, %3 µm, is on no cell a current reaches.")
                                .arg(p + 1).arg(ports.at(p).x(), 0, 'g', 4).arg(ports.at(p).y(), 0, 'g', 4);
            return out;
        }
        QList<int> nodes;
        for (const int c : face) nodes << nodeOf.value(c);
        QList<int> sorted = nodes;
        std::sort(sorted.begin(), sorted.end());
        for (int q = 0; q < p; ++q) {
            QList<int> other = portNodes.at(q);
            std::sort(other.begin(), other.end());
            if (other == sorted) {
                out.problems << tr("Ports %1 and %2 drive the same face of the layout: put them on different ones.")
                                    .arg(q + 1).arg(p + 1);
                return out;
            }
        }
        portNodes << nodes;
        portShares << sharesOf(mesh, face, on, along);
        if (along != QPointF(0, 0))
            out.portsSaid << tr("Port %1 drives the %2 cell(s) along the face at %3, %4 µm.")
                                 .arg(p + 1).arg(face.size()).arg(ports.at(p).x(), 0, 'g', 4).arg(ports.at(p).y(), 0, 'g', 4);
        else
            out.portsSaid << tr("Port %1 is on no face, and drives the one cell nearest %2, %3 µm.")
                                 .arg(p + 1).arg(ports.at(p).x(), 0, 'g', 4).arg(ports.at(p).y(), 0, 'g', 4);
    }

    // The cells in metres, and the heights they are at.
    QList<QRectF> rect(nc);
    QList<QPointF> centre(nc);
    QList<double> zOf(nc, -1);
    QRectF extent;   // (of every cell, for how far the Green's tables go)
    for (int i = 0; i < nc; ++i) {
        const MeshCell& cell = mesh.cells.at(cellOf.at(i));
        rect[i] = scaledBy(cell.outline.boundingRect(), micrometre);
        centre[i] = cell.centre * micrometre;
        extent = i == 0 ? rect.at(i) : extent.united(rect.at(i));
    }
    for (int a = 0; a < nr; ++a) zOf[tail.at(a)] = zOf[head.at(a)] = runs.at(a).z0;
    const double tol = 1e-9 * std::max(1e-9, layers.height());
    QList<double> heights;
    for (const double z : std::as_const(zOf)) {
        bool known = false;
        for (const double h : std::as_const(heights)) known = known || std::abs(h - z) <= tol;
        if (!known) heights << z;
    }
    std::sort(heights.begin(), heights.end());
    const auto heightIndex = [&heights, tol](double z) {
        for (int i = 0; i < heights.size(); ++i)
            if (std::abs(heights.at(i) - z) <= tol) return i;
        return 0;
    };
    const int nh = int(heights.size());
    QList<int> hNode(nc), hRun(nr);
    for (int i = 0; i < nc; ++i) hNode[i] = heightIndex(zOf.at(i));
    for (int a = 0; a < nr; ++a) hRun[a] = heightIndex(runs.at(a).z0);
    // As far as any two points of the layout are apart, and a little more.
    const double rhoMax = 1.05 * std::hypot(extent.width(), extent.height()) + 1e-9;

    // What does not change with the frequency: the runs' static partial
    // inductances, direct and of their images in the ground, and the
    // static potential of each cell's charge spread over it - the singular
    // parts of the two kernels, in closed form.
    Eigen::MatrixXd lDirect = Eigen::MatrixXd::Zero(nr, nr), lImage = Eigen::MatrixXd::Zero(nr, nr);
    QList<double> dirX(nr), dirY(nr), midX(nr), midY(nr);
    QList<Filament> mirrored = runs;
    for (int a = 0; a < nr; ++a) {
        runs.at(a).direction(&dirX[a], &dirY[a], nullptr);
        runs.at(a).middle(&midX[a], &midY[a], nullptr);
        mirrored[a].z0 = mirrored[a].z1 = -runs.at(a).z0;
    }
    for (int a = 0; a < nr; ++a)
        for (int b = a; b < nr; ++b) {
            if (std::abs(dirX.at(a) * dirX.at(b) + dirY.at(a) * dirY.at(b)) < 1e-9) continue;
            lDirect(a, b) = lDirect(b, a) = partialInductance(runs.at(a), runs.at(b));
            lImage(a, b) = lImage(b, a) = partialInductance(runs.at(a), mirrored.at(b));
        }
    Eigen::MatrixXd pStatic(nc, nc);
    for (int i = 0; i < nc; ++i)
        for (int j = 0; j < nc; ++j) pStatic(i, j) = rectanglePotential(rect.at(j), centre.at(i), std::abs(zOf.at(i) - zOf.at(j)));

    out.lines << tr("%1 run(s) of current over %2 cell(s), at %3 metal height(s); %4 port(s), %5 frequency(ies) from "
                    "%6 to %7 GHz.")
                     .arg(nr).arg(nc).arg(nh).arg(np).arg(frequencies.size())
                     .arg(*std::min_element(frequencies.begin(), frequencies.end()) / 1e9, 0, 'g', 4)
                     .arg(*std::max_element(frequencies.begin(), frequencies.end()) / 1e9, 0, 'g', 4);
    out.lines << out.portsSaid;
    out.lines << tr("The ground plane is taken as perfect, a metal as a sheet with the surface impedance of its "
                    "thickness, and each port's current as coming back through the ground.");

    for (int fi = 0; fi < frequencies.size(); ++fi) {
        const double f = frequencies.at(fi), w = 2 * M_PI * f;
        const Complex jw(0, w);

        // The retarded Green's functions of each pair of heights.
        QList<WaveGreen> greens;
        QList<int> pairOf(nh * nh, -1);
        for (int i = 0; i < nh; ++i)
            for (int j = i; j < nh; ++j) {
                WaveGreen g(layers, heights.at(i), heights.at(j), f, rhoMax);
                if (!g.ok()) {
                    out.problems << g.problems();
                    out.frequencies.clear();
                    out.z.clear();
                    out.s.clear();
                    return out;
                }
                g.prepare();
                pairOf[i * nh + j] = pairOf[j * nh + i] = int(greens.size());
                greens << g;
            }

        // jw L: the static partial inductances scaled as the vector
        // potential's singular terms are, and the rest at the centres -
        // the retardation of the source and of its image, and the stack's
        // own remainder.
        MatrixC m(nr, nr);
        for (int a = 0; a < nr; ++a)
            for (int b = a; b < nr; ++b) {
                const double dot = dirX.at(a) * dirX.at(b) + dirY.at(a) * dirY.at(b);
                Complex l(0, 0);
                if (std::abs(dot) >= 1e-9) {
                    const WaveGreen& g = greens.at(pairOf.at(hRun.at(a) * nh + hRun.at(b)));
                    const WaveGreen::Term direct = g.vectorTerm(), image = g.vectorImage();
                    const double rho = std::hypot(midX.at(a) - midX.at(b), midY.at(a) - midY.at(b));
                    l = 2.0 * direct.coefficient / kMu0 * lDirect(a, b) + 2.0 * image.coefficient / kMu0 * lImage(a, b);
                    const Complex smooth = direct.coefficient / (2 * M_PI) * retardation(direct.k, std::hypot(rho, direct.dz))
                                           + image.coefficient / (2 * M_PI) * retardation(image.k, std::hypot(rho, image.dz))
                                           + g.vectorRemainder(rho);
                    l += dot * runs.at(a).length() * runs.at(b).length() * smooth;
                }
                m(a, b) = m(b, a) = jw * l;
            }

        // P: each cell's potential of a unit charge spread over another -
        // the static singular part over the cell in closed form, its
        // retardation at the centres, and the remainder integrated over the
        // charged cell, or at its centre where the cell is small beside the
        // distance the remainder changes over.
        // Made symmetric: a potential at a centre of a charge over a cell is
        // not quite the other way round where the cells differ in size,
        // and the port matrix is reciprocal only if P is.
        MatrixC p(nc, nc);
        const auto coefficient = [&](int i, int j, const WaveGreen& g) {
            const WaveGreen::Term t = g.scalarTerm();
            const double rho = std::hypot(centre.at(i).x() - centre.at(j).x(), centre.at(i).y() - centre.at(j).y());
            const Complex v = t.coefficient / (2 * M_PI) * (pStatic(i, j) + retardation(t.k, std::hypot(rho, t.dz)));
            const double longest = std::max(rect.at(j).width(), rect.at(j).height());
            const double scale = std::hypot(rho, g.reach());
            const bool centred = i != j && longest * longest < kCentreBound * scale * scale;
            return v + (centred ? g.scalarRemainder(rho) : remainderOver(g, rect.at(j), centre.at(i), scale));
        };
        for (int i = 0; i < nc; ++i)
            for (int j = i; j < nc; ++j) {
                const WaveGreen& g = greens.at(pairOf.at(hNode.at(i) * nh + hNode.at(j)));
                const Complex v = i == j ? coefficient(i, i, g) : 0.5 * (coefficient(i, j, g) + coefficient(j, i, g));
                p(i, j) = p(j, i) = v;
            }

        // The charge eliminated: M = Zs l/w + jw L + A^T P A / jw.
        for (int a = 0; a < nr; ++a)
            for (int b = 0; b < nr; ++b)
                m(a, b) += (p(tail.at(a), tail.at(b)) - p(tail.at(a), head.at(b)) - p(head.at(a), tail.at(b))
                            + p(head.at(a), head.at(b)))
                           / jw;
        for (int a = 0; a < nr; ++a) {
            const Filament& run = runs.at(a);
            if (run.width > 0) m(a, a) += sheetImpedance(run.sigma, run.thickness, f) * run.length() / run.width;
        }

        // Each port driven by an ampere in turn: its current into its
        // face's cells, A^T P J / jw the drive.
        MatrixC drive(nc, np), rhs(nr, np);
        drive.setZero();
        for (int q = 0; q < np; ++q)
            for (int k = 0; k < portNodes.at(q).size(); ++k) drive(portNodes.at(q).at(k), q) += portShares.at(q).at(k);
        const MatrixC pDrive = p * drive;
        for (int a = 0; a < nr; ++a)
            for (int q = 0; q < np; ++q) rhs(a, q) = (pDrive(tail.at(a), q) - pDrive(head.at(a), q)) / jw;
        const Eigen::PartialPivLU<MatrixC> lu(m);
        const MatrixC current = lu.solve(rhs);
        if (!current.allFinite()) {
            out.problems << tr("The solve at %1 GHz came to nothing finite.").arg(f / 1e9, 0, 'g', 4);
            out.frequencies.clear();
            out.z.clear();
            out.s.clear();
            return out;
        }
        if (lu.rcond() < 1e-14)
            out.lines << tr("At %1 GHz the system is nearly singular (a reciprocal condition of %2): the answer there may "
                            "be rounding.")
                             .arg(f / 1e9, 0, 'g', 4).arg(lu.rcond(), 0, 'g', 2);

        // The charges it leaves, (J - A I)/jw; their potentials; and each
        // port's voltage, its cells' potentials by the same shares.
        MatrixC charge = drive;
        for (int a = 0; a < nr; ++a)
            for (int q = 0; q < np; ++q) {
                charge(tail.at(a), q) -= current(a, q);
                charge(head.at(a), q) += current(a, q);
            }
        charge /= jw;
        const MatrixC potential = p * charge;
        const MatrixC z = drive.transpose() * potential;
        QList<QList<Complex>> zRows;
        for (int i = 0; i < np; ++i) {
            QList<Complex> row;
            for (int j = 0; j < np; ++j) row << z(i, j);
            zRows << row;
        }
        out.frequencies << f;
        out.z << zRows;
        out.s << scatteringOf(zRows, reference);
        if (fi + 1 == frequencies.size()) {
            out.current.clear();
            for (int a = 0; a < nr; ++a) out.current << current(a, 0);
        }
    }
    out.geometry = runs;
    out.lines << tr("The Green's functions of %1 pair(s) of heights, tabulated to %2 mm at each frequency.")
                     .arg(nh * (nh + 1) / 2).arg(rhoMax * 1e3, 0, 'g', 3);
    return out;
}

} // namespace qucs_s::rf25
