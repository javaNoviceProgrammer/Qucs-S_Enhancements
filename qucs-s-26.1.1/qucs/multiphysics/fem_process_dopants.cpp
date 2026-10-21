/*
 * fem_process_dopants.cpp - a process flow's dopants on a grid: implants
 *                           that follow the topography (Pearson profiles
 *                           along the beam through each layer, dose-
 *                           matched; masks' resist stopping them; the
 *                           damage's amorphous layer and its interstitials),
 *                           anneals (Fermi-level diffusion of several
 *                           dopants with their field, segregation, dose
 *                           loss, activation, a source at the surface,
 *                           transient enhanced diffusion), oxidation (Deal
 *                           and Grove's, Massoud's) with its moving
 *                           interface, quantum wells' intermixing
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_process_p.h"

#include "fem_expr.h"
#include "fem_materials.h"
#include "fem_problem.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace qucs_s::fem {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::fem::Process", text);
}

constexpr double KB = 8.617333262e-5;   // eV/K
constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

bool isSpecies(const QString& name)
{
    return name != QLatin1String("I") && name != QLatin1String("Icl") && !name.startsWith(QLatin1String("x:"));
}

} // namespace

// ------------------------------------------------------------ The grid

bool ProcessDopants::cellAt(double x, double y, int* i, int* j) const
{
    if (xe.size() < 2 || ye.size() < 2) return false;
    if (!(x >= xe.front() && x <= xe.back() && y >= ye.front() && y <= ye.back())) return false;
    *i = std::clamp(int(std::upper_bound(xe.begin(), xe.end(), x) - xe.begin()) - 1, 0, nx() - 1);
    *j = std::clamp(int(std::upper_bound(ye.begin(), ye.end(), y) - ye.begin()) - 1, 0, ny() - 1);
    return true;
}

const ProcessDopants::Field* ProcessDopants::field(const QString& name) const
{
    for (const Field& f : fields)
        if (f.name == name) return &f;
    return nullptr;
}

ProcessDopants::Field* ProcessDopants::field(const QString& name)
{
    for (Field& f : fields)
        if (f.name == name) return &f;
    return nullptr;
}

std::vector<const ProcessDopants::Field*> ProcessDopants::species() const
{
    std::vector<const Field*> out;
    for (const Field& f : fields)
        if (isSpecies(f.name)) out.push_back(&f);
    return out;
}

int ProcessDopants::materialAt(double x, double y) const
{
    int i = 0, j = 0;
    return cellAt(x, y, &i, &j) ? material[index(i, j)] : -1;
}

double ProcessDopants::at(const Field& f, double x, double y, int m, bool active) const
{
    int i = 0, j = 0;
    if (!cellAt(x, y, &i, &j)) return NaN;
    const std::vector<double>& v = active && !f.active.empty() ? f.active : f.total;
    auto cx = [this](int k) { return 0.5 * (xe[std::size_t(k)] + xe[std::size_t(k + 1)]); };
    auto cy = [this](int k) { return 0.5 * (ye[std::size_t(k)] + ye[std::size_t(k + 1)]); };
    // Linear between the cells of material m about a point, or - at its
    // edge (a surface, an interface) - out from the nearest two to it: half
    // a cell at most. (The nearest cell's value alone had an etched slab's
    // surface nodes half a cell deep: a steep profile's 5 % there.)
    auto line = [](double p, double c1, double v1, double c2, double v2, bool two) {
        if (!two || c1 == c2) return v1;
        return std::max(0.0, v1 + (v2 - v1) * (p - c1) / (c2 - c1));
    };
    // Down column \a col at y.
    auto along = [&j, this, &m, &cy, &y, &line, &v](int col, double* out) {
        int below = -1, above = -1;
        for (int k = j; k >= std::max(0, j - 3) && below < 0; --k)
            if (material[index(col, k)] == m && cy(k) <= y) below = k;
        for (int k = j; k <= std::min(ny() - 1, j + 3) && above < 0; ++k)
            if (material[index(col, k)] == m && cy(k) >= y) above = k;
        if (below >= 0 && above >= 0) {
            *out = line(y, cy(below), v[index(col, below)], cy(above), v[index(col, above)], true);
            return true;
        }
        const int a = below >= 0 ? below : above;
        if (a < 0) return false;
        const int b = below >= 0 ? a - 1 : a + 1;   // (the next, further from y)
        const bool two = b >= 0 && b < ny() && material[index(col, b)] == m;
        *out = line(y, cy(a), v[index(col, a)], two ? cy(b) : cy(a), two ? v[index(col, b)] : 0.0, two);
        return true;
    };
    // Then across, the same.
    double left = 0, right = 0, far = 0;
    int l = -1, r = -1;
    for (int k = i; k >= std::max(0, i - 3) && l < 0; --k)
        if (cx(k) <= x && along(k, &left)) l = k;
    for (int k = i; k <= std::min(nx() - 1, i + 3) && r < 0; ++k)
        if (cx(k) >= x && along(k, &right)) r = k;
    if (l >= 0 && r >= 0) return line(x, cx(l), left, cx(r), right, true);
    const int a = l >= 0 ? l : r;
    if (a < 0) return NaN;
    const double va = l >= 0 ? left : right;
    const int b = l >= 0 ? a - 1 : a + 1;
    const bool two = b >= 0 && b < nx() && along(b, &far);
    return line(x, cx(a), va, two ? cx(b) : cx(a), far, two);
}

void ProcessDopants::carriers(double x, double y, int m, double* donors, double* acceptors) const
{
    *donors = *acceptors = 0;
    if (m < 0 || m >= int(hosts.size())) return;
    for (const Field* f : species()) {
        const int q = dopantCharge(f->name, hosts[std::size_t(m)]);
        if (q == 0) continue;
        const double v = at(*f, x, y, m, true);
        if (!(v > 0)) continue;
        (q > 0 ? *donors : *acceptors) += v;
    }
}

int dopantCharge(const QString& species, int host)
{
    static const QStringList groupIVAcceptors = {QStringLiteral("B"), QStringLiteral("Al"), QStringLiteral("Ga"), QStringLiteral("In")};
    static const QStringList groupIVDonors = {QStringLiteral("P"), QStringLiteral("As"), QStringLiteral("Sb"), QStringLiteral("Li")};
    static const QStringList IIIVAcceptors = {QStringLiteral("Zn"), QStringLiteral("Be"), QStringLiteral("Mg"), QStringLiteral("C"),
                                              QStringLiteral("Cd"), QStringLiteral("Mn")};
    static const QStringList IIIVDonors = {QStringLiteral("Si"), QStringLiteral("Ge"), QStringLiteral("Sn"), QStringLiteral("S"),
                                           QStringLiteral("Se"), QStringLiteral("Te")};
    static const QStringList IIVIDonors = {QStringLiteral("Al"), QStringLiteral("Ga"), QStringLiteral("In"), QStringLiteral("Cl"),
                                           QStringLiteral("Br"), QStringLiteral("I")};
    static const QStringList IIVIAcceptors = {QStringLiteral("N"), QStringLiteral("P"), QStringLiteral("As"), QStringLiteral("Li"),
                                              QStringLiteral("Cu")};
    const QString s = species.compare(QLatin1String("BF2"), Qt::CaseInsensitive) == 0 ? QStringLiteral("B") : species;
    switch (host) {
    case 4: return groupIVDonors.contains(s) ? 1 : groupIVAcceptors.contains(s) ? -1 : 0;
    case 35: return IIIVDonors.contains(s) ? 1 : IIIVAcceptors.contains(s) ? -1 : 0;
    case 26: return IIVIDonors.contains(s) ? 1 : IIVIAcceptors.contains(s) ? -1 : 0;
    default: return 0;
    }
}

namespace process {

double Thermal::at(double t) const
{
    if (points.empty()) return 0;
    if (t <= points.front().first) return points.front().second;
    for (std::size_t k = 1; k < points.size(); ++k)
        if (t <= points[k].first) {
            const double a = points[k - 1].first, b = points[k].first;
            return b > a ? points[k - 1].second + (points[k].second - points[k - 1].second) * (t - a) / (b - a) : points[k].second;
        }
    return points.back().second;
}

double Thermal::highest() const
{
    double h = 0;
    for (const auto& p : points) h = std::max(h, p.second);
    return h;
}

double Thermal::lowest() const
{
    double l = std::numeric_limits<double>::max();
    for (const auto& p : points) l = std::min(l, p.second);
    return points.empty() ? 0 : l;
}

namespace {

/// Scharfetter and Gummel's Bernoulli function x/(e^x - 1).
double bernoulli(double x)
{
    if (std::abs(x) < 1e-6) return 1 - x / 2;
    if (x > 700) return x * std::exp(-x);
    return x / std::expm1(x);
}

/// Where a vertical line at X (integers) crosses region r's outline, its
/// inside intervals in y (even-odd), integers.
std::vector<std::pair<double, double>> verticalIntervals(const Region& r, double X)
{
    std::vector<double> ys;
    for (const Path& p : r.p)
        for (std::size_t k = 0; k < p.size(); ++k) {
            const ClipperLib::IntPoint a = p[k], b = p[(k + 1) % p.size()];
            if ((double(a.X) <= X) != (double(b.X) <= X)) ys.push_back(double(a.Y) + (X - double(a.X)) * double(b.Y - a.Y) / double(b.X - a.X));
        }
    std::sort(ys.begin(), ys.end());
    std::vector<std::pair<double, double>> out;
    for (std::size_t k = 0; k + 1 < ys.size(); k += 2) out.emplace_back(ys[k], ys[k + 1]);
    return out;
}

/// The same along a horizontal line at Y: x intervals.
std::vector<std::pair<double, double>> horizontalIntervals(const Paths& paths, double Y)
{
    std::vector<double> xs;
    for (const Path& p : paths)
        for (std::size_t k = 0; k < p.size(); ++k) {
            const ClipperLib::IntPoint a = p[k], b = p[(k + 1) % p.size()];
            if ((double(a.Y) <= Y) != (double(b.Y) <= Y)) xs.push_back(double(a.X) + (Y - double(a.Y)) * double(b.X - a.X) / double(b.Y - a.Y));
        }
    std::sort(xs.begin(), xs.end());
    std::vector<std::pair<double, double>> out;
    for (std::size_t k = 0; k + 1 < xs.size(); k += 2) out.emplace_back(xs[k], xs[k + 1]);
    return out;
}

/// Where a line p + s d crosses \a paths: the s intervals inside (even-odd).
std::vector<std::pair<double, double>> lineIntervals(const Paths& paths, double px, double py, double dx, double dy)
{
    std::vector<double> ss;
    for (const Path& p : paths)
        for (std::size_t k = 0; k < p.size(); ++k) {
            const double ax = double(p[k].X), ay = double(p[k].Y);
            const double bx = double(p[(k + 1) % p.size()].X), by = double(p[(k + 1) % p.size()].Y);
            const double ex = bx - ax, ey = by - ay;
            const double den = dx * ey - dy * ex;
            if (std::abs(den) < 1e-12) continue;
            const double s = ((ax - px) * ey - (ay - py) * ex) / den;
            const double u = ((ax - px) * dy - (ay - py) * dx) / den;
            if (u >= 0 && u < 1) ss.push_back(s);
        }
    std::sort(ss.begin(), ss.end());
    std::vector<std::pair<double, double>> out;
    for (std::size_t k = 0; k + 1 < ss.size(); k += 2) out.emplace_back(ss[k], ss[k + 1]);
    return out;
}

/// The highest point of the solids over a vertical line (integers), or -inf.
double columnTop(const std::vector<Region>& regions, double X)
{
    double top = -std::numeric_limits<double>::infinity();
    for (const Region& r : regions)
        for (const auto& [a, b] : verticalIntervals(r, X)) top = std::max(top, b);
    return top;
}

double regionsTop(const std::vector<Region>& regions)
{
    double top = -std::numeric_limits<double>::infinity();
    for (const Region& r : regions)
        for (const Path& p : r.p)
            for (const ClipperLib::IntPoint& q : p) top = std::max(top, double(q.Y));
    return top;
}

} // namespace

// ---------------------------------------------------------- Its materials

struct Dopants::MaterialInfo {
    QString name;
    std::optional<Target> target;
    QString why;
    int host = 0;
    bool silicon = false, oxide = false, nitride = false;
    bool crystal = false;
    Sublattices lattice;
};

Dopants::Dopants(const Model& model, const ParameterScope& ps, double unitScale, double k)
    : a_model(model), a_ps(ps), a_unitScale(unitScale), a_k(k)
{
}

std::shared_ptr<const ProcessDopants> Dopants::state() const
{
    return a_d ? std::make_shared<ProcessDopants>(*a_d) : nullptr;
}

int Dopants::materialIndex(const QString& name)
{
    const int at = int(a_d->materials.indexOf(name));
    if (at >= 0) return at;
    a_d->materials << name;
    auto inf = std::make_shared<MaterialInfo>();
    inf->name = name;
    inf->target = targetOf(name, a_model, a_ps, &inf->why);
    if (inf->target) {
        inf->host = hostOf(*inf->target);
        const auto& p = inf->target->parts;
        inf->silicon = p.size() == 1 && p[0].Z == 14;
        inf->oxide = p.size() == 2 && p[0].Z == 14 && p[1].Z == 8;
        inf->nitride = p.size() == 2 && p[0].Z == 14 && p[1].Z == 7;
    }
    // A III-V's composition, for its intermixing.
    for (const Node& m : a_model.materials().children)
        if (m.name().compare(name, Qt::CaseInsensitive) == 0 || m.text(QStringLiteral("library")).compare(name, Qt::CaseInsensitive) == 0) {
            const QString compound = detail::Problem::materialProperty(m, QStringLiteral("compound"), &a_ps.scope).trimmed();
            const double x = evaluateConstant(detail::Problem::materialProperty(m, QStringLiteral("x"), &a_ps.scope), a_ps.scope, Dim()).value_or(0);
            const double y = evaluateConstant(detail::Problem::materialProperty(m, QStringLiteral("y"), &a_ps.scope), a_ps.scope, Dim()).value_or(0);
            if (!compound.isEmpty()) inf->lattice = sublatticesOf(compound, x, y);
            break;
        }
    if (!inf->lattice.ok)
        if (const std::optional<MaterialEntry> e = findMaterial(name); e && !e->properties.value(QStringLiteral("compound")).toString().isEmpty())
            inf->lattice = sublatticesOf(e->properties.value(QStringLiteral("compound")).toString(),
                                         evaluateConstant(e->properties.value(QStringLiteral("x")).toString(), a_ps.scope, Dim()).value_or(0),
                                         evaluateConstant(e->properties.value(QStringLiteral("y")).toString(), a_ps.scope, Dim()).value_or(0));
    a_info.push_back(inf);
    a_d->hosts.push_back(inf->host);
    return int(a_d->materials.size()) - 1;
}

const Dopants::MaterialInfo& Dopants::info(int m)
{
    return *a_info[std::size_t(m)];
}

double Dopants::cellWidth(int i) const
{
    return (a_d->xe[std::size_t(i + 1)] - a_d->xe[std::size_t(i)]) * a_unitScale;
}

double Dopants::cellHeight(int j) const
{
    return (a_d->ye[std::size_t(j + 1)] - a_d->ye[std::size_t(j)]) * a_unitScale;
}

void Dopants::assign(const std::vector<Region>& regions, std::vector<int>* material) const
{
    const ProcessDopants& d = *a_d;
    const int nx = d.nx(), ny = d.ny();
    material->assign(std::size_t(nx) * std::size_t(ny), -1);
    std::vector<int> index(regions.size(), -1);
    for (std::size_t r = 0; r < regions.size(); ++r) index[r] = int(d.materials.indexOf(regions[r].material));
    for (int j = 0; j < ny; ++j) {
        const double Y = 0.5 * (d.ye[std::size_t(j)] + d.ye[std::size_t(j + 1)]) * a_k;
        for (std::size_t r = 0; r < regions.size(); ++r) {
            if (index[r] < 0) continue;
            for (const auto& [a, b] : horizontalIntervals(regions[r].p, Y)) {
                // Cells whose centres are in [a, b).
                int i = int(std::lower_bound(d.xe.begin(), d.xe.end(), a / a_k) - d.xe.begin()) - 1;
                for (i = std::max(i, 0); i < nx; ++i) {
                    const double X = 0.5 * (d.xe[std::size_t(i)] + d.xe[std::size_t(i + 1)]) * a_k;
                    if (X >= b) break;
                    if (X >= a) (*material)[std::size_t(j) * std::size_t(nx) + std::size_t(i)] = index[r];
                }
            }
        }
    }
}

void Dopants::make(cInt x0, cInt x1, cInt bottom, const std::vector<Region>& regions, double dx, double dy, double depth)
{
    if (a_d || !a_tooLarge.isEmpty()) return;
    a_dy = dy;
    a_depth = depth;
    const double X0 = double(x0) / a_k, X1 = double(x1) / a_k;
    const int nx = std::clamp(int(std::ceil((X1 - X0) / dx - 1e-9)), 1, 4000);
    std::vector<double> xe;
    for (int i = 0; i <= nx; ++i) xe.push_back(X0 + (X1 - X0) * i / nx);
    // Rows: dy from the top down to depth below the lowest surface, then
    // each a quarter taller to the bottom.
    double low = std::numeric_limits<double>::max();
    for (int i = 0; i < nx; ++i) {
        const double t = columnTop(regions, 0.5 * (xe[std::size_t(i)] + xe[std::size_t(i + 1)]) * a_k) / a_k;
        if (std::isfinite(t)) low = std::min(low, t);
    }
    const double top = regionsTop(regions) / a_k, B = double(bottom) / a_k;
    if (!std::isfinite(low) || low > top) low = top;
    const double fineFrom = std::max(B, std::floor((low - depth) / dy) * dy);
    if (!fits(nx, (std::ceil(top / dy) * dy + dy - fineFrom) / dy)) return;
    a_d = std::make_shared<ProcessDopants>();
    a_d->xe = std::move(xe);
    for (const Region& r : regions) materialIndex(r.material);
    std::vector<double> ye;
    for (double y = std::ceil(top / dy) * dy + dy; y > fineFrom + 1e-9 * dy; y -= dy) ye.push_back(y);
    double y = fineFrom, h = dy;
    ye.push_back(y);
    while (y > B + 1e-9 * dy) {
        h *= 1.25;
        y = std::max(B, y - h);
        if (y - B < 0.5 * h) y = B;
        ye.push_back(y);
    }
    std::reverse(ye.begin(), ye.end());
    a_d->ye = ye;
    assign(regions, &a_d->material);
}

void Dopants::addRows(double top)
{
    ProcessDopants& d = *a_d;
    if (top + 1e-9 * a_dy <= d.ye.back()) return;
    if (!fits(d.nx(), d.ny() + (top - d.ye.back()) / a_dy + 1)) return;
    const std::size_t nx = std::size_t(d.nx());
    while (d.ye.back() < top + 1e-9 * a_dy) {
        d.ye.push_back(d.ye.back() + a_dy);
        d.material.insert(d.material.end(), nx, -1);
        for (ProcessDopants::Field& f : d.fields) {
            f.total.insert(f.total.end(), nx, 0.0);
            if (!f.active.empty()) f.active.insert(f.active.end(), nx, 0.0);
        }
    }
}

/// The grid's budget: a flow whose cells would pass it refused (a dy of
/// 0.1 nm under 300 µm filled: 10⁹ cells, minutes to make and memory past
/// any machine's - the fuzz of 21 October, PF2).
bool Dopants::fits(double nx, double ny)
{
    constexpr double MaxCells = 5e6;
    if (nx * ny <= MaxCells) return true;
    if (a_tooLarge.isEmpty())
        a_tooLarge = tr("its dopants' grid would have %1 cells (%2 across, %3 down), more than %4: a coarser dx or dy, or less depth")
                         .arg(formatNumber(nx * ny, 3), QString::number(qint64(nx)), QString::number(qint64(std::ceil(ny))), formatNumber(MaxCells, 3));
    return false;
}

void Dopants::remapRows(const std::vector<double>& ye)
{
    ProcessDopants& d = *a_d;
    const int nx = d.nx(), ny0 = d.ny(), ny1 = int(ye.size()) - 1;
    auto remap = [&nx, &ny1, &ye, &ny0, &d](std::vector<double>& v) {
        if (v.empty()) return;
        std::vector<double> out(std::size_t(nx) * std::size_t(ny1), 0.0);
        for (int i = 0; i < nx; ++i) {
            int a = 0;
            for (int k = 0; k < ny1; ++k) {
                const double lo = ye[std::size_t(k)], hi = ye[std::size_t(k + 1)];
                while (a < ny0 && d.ye[std::size_t(a + 1)] <= lo) ++a;
                double amount = 0;
                for (int b = a; b < ny0 && d.ye[std::size_t(b)] < hi; ++b) {
                    const double o = std::min(hi, d.ye[std::size_t(b + 1)]) - std::max(lo, d.ye[std::size_t(b)]);
                    if (o > 0) amount += v[d.index(i, b)] * o;
                }
                out[std::size_t(k) * std::size_t(nx) + std::size_t(i)] = amount / (hi - lo);
            }
        }
        v = out;
    };
    for (ProcessDopants::Field& f : d.fields) {
        remap(f.total);
        remap(f.active);
    }
    d.ye = ye;
}

void Dopants::refine(const std::vector<Region>& regions)
{
    ProcessDopants& d = *a_d;
    // (No grid - a flow narrower than a column, no rows -: nothing to
    // refine. Its row -1 was read: the fuzz of 21 October, PF1.)
    if (d.nx() <= 0 || d.ny() <= 0) return;
    double low = std::numeric_limits<double>::max();
    bool any = false;
    for (int i = 0; i < d.nx(); ++i) {
        const double t = columnTop(regions, 0.5 * (d.xe[std::size_t(i)] + d.xe[std::size_t(i + 1)]) * a_k) / a_k;
        if (std::isfinite(t)) {
            low = std::min(low, t);
            any = true;
        }
    }
    if (!any) return;
    const double want = std::max(d.ye.front(), low - a_depth);
    // The fine rows' bottom: the lowest of the rows a dy tall up to the top.
    int j = d.ny() - 1;
    while (j > 0 && d.ye[std::size_t(j)] - d.ye[std::size_t(j - 1)] <= a_dy * 1.01) --j;
    const double fineFrom = d.ye[std::size_t(j)];
    if (fineFrom <= want + a_dy) return;
    const double from = std::max(d.ye.front(), std::floor(want / a_dy) * a_dy);
    if (!fits(d.nx(), d.ny() + (fineFrom - from) / a_dy)) return;
    std::vector<double> ye;
    for (int k = int(d.ye.size()) - 1; k >= 0 && d.ye[std::size_t(k)] > fineFrom - 1e-9 * a_dy; --k) ye.push_back(d.ye[std::size_t(k)]);
    for (double y = fineFrom - a_dy; y > from + 1e-9 * a_dy; y -= a_dy) ye.push_back(y);
    double y = from, h = a_dy;
    ye.push_back(y);
    while (y > d.ye.front() + 1e-9 * a_dy) {
        h *= 1.25;
        y = std::max(d.ye.front(), y - h);
        if (y - d.ye.front() < 0.5 * h) y = d.ye.front();
        ye.push_back(y);
    }
    std::reverse(ye.begin(), ye.end());
    remapRows(ye);
    assign(regions, &d.material);
}

void Dopants::reshape(const std::vector<Region>& regions, QStringList* warnings)
{
    if (!a_d) return;
    ProcessDopants& d = *a_d;
    for (const Region& r : regions) materialIndex(r.material);
    addRows(regionsTop(regions) / a_k);
    std::vector<int> now;
    assign(regions, &now);
    std::map<QString, double> removed;   // m⁻¹ (per unit length out of the plane)
    for (int j = 0; j < d.ny(); ++j)
        for (int i = 0; i < d.nx(); ++i) {
            const std::size_t k = d.index(i, j);
            // (One solid become another - an interface snapped - keeps its
            // dopants; gone to the air or come from it, none.)
            if (now[k] == d.material[k] || (now[k] >= 0 && d.material[k] >= 0)) continue;
            for (ProcessDopants::Field& f : d.fields) {
                if (d.material[k] >= 0 && now[k] < 0 && isSpecies(f.name) && f.total[k] > 0)
                    removed[f.name] += f.total[k] * cellWidth(i) * cellHeight(j);
                f.total[k] = 0;
                if (!f.active.empty()) f.active[k] = 0;
            }
        }
    d.material = now;
    const double width = (d.xe.back() - d.xe.front()) * a_unitScale;
    for (const auto& [name, amount] : removed)
        if (amount / width > 1e8)   // (1e4 cm⁻²: what is worth saying)
            warnings->append(tr("%1 taken away with what was removed: %2 cm⁻² over the window").arg(name, formatNumber(amount / width * 1e-4, 3)));
}

void Dopants::dope(const QString& name, const std::vector<Region>& regions, const QString& species, double concentration)
{
    if (!a_d || species.trimmed().isEmpty() || !(concentration > 0)) return;
    ProcessDopants& d = *a_d;
    std::vector<Region> one;
    for (const Region& r : regions)
        if (r.name == name) one.push_back(r);
    if (one.empty()) return;
    std::vector<int> mask;
    assign(one, &mask);
    const QString s = species.compare(QLatin1String("BF2"), Qt::CaseInsensitive) == 0 ? QStringLiteral("B") : species.trimmed();
    ProcessDopants::Field* f = d.field(s);
    if (!f) {
        d.fields.push_back({s, std::vector<double>(d.material.size(), 0.0), std::vector<double>(d.material.size(), 0.0)});
        f = &d.fields.back();
    }
    for (std::size_t k = 0; k < mask.size(); ++k)
        if (mask[k] >= 0) {
            f->total[k] = concentration;
            f->active[k] = concentration;
        }
}

// --------------------------------------------------------------- Implants

QString Dopants::implant(const ImplantSpec& spec, const std::vector<Region>& regions, const Paths& resist, QStringList* warnings, QString* error)
{
    ProcessDopants& d = *a_d;
    const std::optional<Ion> ion = ionOf(spec.species);
    if (!ion) {
        *error = tr("%1 is not an ion known: an element's symbol (B, P, As, Sb, In, Zn, Be, Si, H, He...) or BF2").arg(spec.species);
        return {};
    }
    if (!(spec.energy > 0) || !(spec.dose > 0)) {
        *error = tr("its energy and dose must be more than 0");
        return {};
    }
    const double E = spec.energy * ion->energyShare;
    const int nx = d.nx(), ny = d.ny();
    for (const Region& r : regions) materialIndex(r.material);
    const int resistIndex = resist.empty() ? -1 : materialIndex(QStringLiteral("Photoresist"));
    std::set<int> crystal;
    for (const Region& r : regions)
        if (r.crystal) crystal.insert(materialIndex(r.material));
    // Each material's profiles, made as the beam meets it.
    struct Profile {
        bool ok = false;
        Pearson main, channel;
        double r = 0, lateral = 0, slope = 0, Rp = 0;
        double nuclear = 0, dMean = 0, dSd = 1;
        ImplantMoments m;
    };
    std::map<int, Profile> profiles;
    auto profileOf = [&profiles, this, &error, &ion, &E, &spec, &warnings, &resistIndex](int m) -> const Profile* {
        if (auto it = profiles.find(m); it != profiles.end()) return it->second.ok ? &it->second : nullptr;
        Profile& p = profiles[m];
        const MaterialInfo& mi = info(m);
        if (!mi.target) {
            if (error->isEmpty()) *error = mi.why;
            return nullptr;
        }
        QString why;
        const std::optional<ImplantMoments> im = implantMoments(*ion, E, *mi.target, &why);
        if (!im) {
            if (error->isEmpty()) *error = tr("%1 in %2: %3").arg(ion->atom, mi.name, why);
            return nullptr;
        }
        p.m = *im;
        // Moments given for this material: in place of those found.
        for (const ImplantSpec::Given& g : spec.given)
            if (g.material.compare(mi.name, Qt::CaseInsensitive) == 0) {
                if (std::isfinite(g.Rp)) p.m.Rp = g.Rp;
                if (std::isfinite(g.dRp)) p.m.dRp = g.dRp;
                if (std::isfinite(g.gamma)) p.m.gamma = g.gamma;
                if (std::isfinite(g.beta)) p.m.beta = g.beta;
                if (std::isfinite(g.lateral)) {
                    // (Given: the same at every depth.)
                    p.m.lateral = g.lateral;
                    p.m.lateralSlope = 0;
                }
                p.m.source = tr("given");
                p.m.extrapolated = false;
            }
        if (p.m.extrapolated && !p.m.note.isEmpty()) warnings->append(p.m.note);
        double gamma = spec.gaussian ? 0 : p.m.gamma, beta = spec.gaussian ? 3 : p.m.beta;
        if (!spec.gaussian && !(std::abs(gamma) < 1e-9 && beta > 3) && !Pearson::typeIV(gamma, beta)) {
            const double least = Pearson::leastBeta(gamma);
            // (Said where the dopant stays: not the mask's resist.)
            if (beta < least * 1.001 && m != resistIndex) {
                warnings->append(tr("%1 in %2: its kurtosis %3 is below Pearson IV's least at skewness %4 - %5 taken")
                                     .arg(ion->atom, mi.name, formatNumber(beta, 3), formatNumber(gamma, 3), formatNumber(least * 1.01, 3)));
            }
            if (beta < least * 1.001) beta = least * 1.01;
        }
        if (std::abs(gamma) < 1e-9 && beta <= 3 && !spec.gaussian) beta = 3;
        const double unit = 1 / a_unitScale;   // m to the geometry's unit
        p.main = Pearson::make(p.m.Rp * unit, p.m.dRp * unit, gamma, beta);
        p.Rp = p.m.Rp * unit;
        p.lateral = p.m.lateral * unit;
        p.slope = p.m.lateralSlope * unit;
        p.nuclear = p.m.nuclear;
        p.dMean = p.m.damageMean * unit;
        p.dSd = std::max(p.m.damageStraggle * unit, 1e-6 * p.Rp);
        for (const ImplantSpec::Channel& c : spec.channel)
            if (c.material.compare(mi.name, Qt::CaseInsensitive) == 0 && c.fraction > 0) {
                p.r = std::clamp(c.fraction, 0.0, 1.0);
                p.channel = Pearson::make(c.Rp * unit, c.dRp * unit, c.gamma, c.beta);
            }
        p.ok = true;
        return &p;
    };
    // The beam: down, tilted toward +x.
    const double sx = std::sin(spec.tilt), sy = -std::cos(spec.tilt);
    if (!(-sy > 0.05)) {
        *error = tr("its tilt must be less than 87° from the normal");
        return {};
    }
    double ceiling = d.ye.back();
    for (const Path& p : resist)
        for (const ClipperLib::IntPoint& q : p) ceiling = std::max(ceiling, double(q.Y) / a_k);
    ceiling += a_dy;
    const QString s = ion->atom;
    ProcessDopants::Field* f = d.field(s);
    if (!f) {
        d.fields.push_back({s, std::vector<double>(d.material.size(), 0.0), std::vector<double>(d.material.size(), 0.0)});
        f = &d.fields.back();
    }
    std::vector<double> added(d.material.size(), 0.0), plus(d.material.size(), 0.0);
    std::map<int, double> stoppedIn;   // ions per unit length, by material (-1 out through a sidewall, -2 passed through)
    double amorphousDeepest = 0;       // the geometry's unit, from the crystal's surface
    int amorphousSlices = 0;
    const double X0 = d.xe.front(), X1 = d.xe.back();
    const double minDy = [&] {
        double m = std::numeric_limits<double>::max();
        for (int j = 0; j < ny; ++j) m = std::min(m, d.ye[std::size_t(j + 1)] - d.ye[std::size_t(j)]);
        return m;
    }();
    for (int col = 0; col < nx; ++col) {
        const double xc = 0.5 * (d.xe[std::size_t(col)] + d.xe[std::size_t(col + 1)]);
        const double ions = spec.dose * cellWidth(col);   // per metre out of the plane
        // Where the path is in the resist (s in the geometry's unit).
        std::vector<std::pair<double, double>> inResist;
        if (!resist.empty())
            for (const auto& [a, b] : lineIntervals(resist, xc * a_k, ceiling * a_k, sx, sy)) inResist.emplace_back(a / a_k, b / a_k);
        // The samples along the path: each its material, the cell it is in.
        struct Sample {
            double s, ds, x, y;
            int material, i, j;
        };
        std::vector<Sample> samples;
        double sPos = 0;
        while (true) {
            const double x = xc + sPos * sx, y = ceiling + sPos * sy;
            if (y < d.ye.front() || x < X0 || x > X1) break;
            // Its length a third of the row it starts in; what it is, its
            // middle's (its start may be a rounding into the cell above a
            // boundary: the surface's first sample dropped, the cell a third
            // short).
            int i = col, j = ny - 1;
            double h = a_dy;
            if (y <= d.ye.back() && d.cellAt(x, y, &i, &j)) h = d.ye[std::size_t(j + 1)] - d.ye[std::size_t(j)];
            const double ds = std::max(std::min(h, minDy * 8) / 3, 1e-6 * a_dy);
            const double mid = sPos + ds / 2;
            const double xm = xc + mid * sx, ym = ceiling + mid * sy;
            int m = -1;
            i = col;
            j = ny - 1;
            if (ym >= d.ye.front() && ym <= d.ye.back() && d.cellAt(std::clamp(xm, X0, X1), ym, &i, &j)) m = d.material[d.index(i, j)];
            if (m < 0)
                for (const auto& [a, b] : inResist)
                    if (mid >= a && mid < b) m = resistIndex;
            samples.push_back({sPos, ds, xm, ym, m, i, j});
            sPos += ds;
        }
        // The fraction still moving after each sample: dose-matched from
        // layer to layer - entering a material with a fraction G, at the
        // depth of its own profile that leaves G beyond (its profile in a
        // target of it alone, what falls above its surface folded back).
        auto march = [&samples, &profileOf, &crystal](bool channelled, auto&& visit) {
            double G = 1, z = 0;
            int current = -3;
            const Profile* prof = nullptr;
            bool crystalStart = true;
            for (const Sample& sm : samples) {
                if (G < 1e-10) break;
                if (sm.material < 0) {
                    current = -1;
                    continue;
                }
                const Profile* p = profileOf(sm.material);
                if (!p) return false;
                const double r = channelled && crystal.count(sm.material) ? p->r : 0;
                // (What a profile puts above its surface folded back below
                // it - the Implant profile's image -: 1 - P(z) + P(-z).)
                auto remaining = [p, r](double zz) {
                    const double main = 1 - p->main.cdf(zz) + p->main.cdf(-zz);
                    if (r <= 0) return main;
                    return (1 - r) * main + r * (1 - p->channel.cdf(zz) + p->channel.cdf(-zz));
                };
                if (sm.material != current) {
                    current = sm.material;
                    prof = p;
                    // z with remaining(z) = G, by bisection.
                    double lo = 0, hi = std::max(p->Rp, 1e-12) * 2 + 20 * p->main.sd() + (r > 0 ? p->channel.mean() + 20 * p->channel.sd() : 0);
                    if (G >= 1 - 1e-15) hi = lo;
                    else
                        for (int k = 0; k < 80; ++k) {
                            const double m = 0.5 * (lo + hi);
                            (remaining(m) > G ? lo : hi) = m;
                        }
                    z = 0.5 * (lo + hi);
                    crystalStart = true;
                }
                const double zNext = z + sm.ds;
                const double Gn = std::min(G, std::max(0.0, remaining(zNext)));
                visit(sm, G - Gn, 0.5 * (z + zNext), *prof, crystalStart);
                crystalStart = false;
                G = Gn;
                z = zNext;
            }
            return true;
        };
        // The damage first: the energy into nuclear collisions, deposited as
        // its own Gaussian about its mean depth; amorphous where it passes
        // the critical density (in a crystal).
        std::vector<char> amorphous(samples.size(), 0);
        bool anyAmorphous = false;
        double crystalFrom = NaN;
        {
            if (!march(false, [&](const Sample& sm, double, double zz, const Profile& p, bool) {
                    const std::size_t at = std::size_t(&sm - samples.data());
                    if (!crystal.count(sm.material)) return;
                    if (!std::isfinite(crystalFrom)) crystalFrom = sm.s;
                    const double perLength = p.nuclear / (std::sqrt(2 * M_PI) * p.dSd * a_unitScale)
                                             * std::exp(-0.5 * std::pow((zz - p.dMean) / p.dSd, 2));   // eV/m per ion
                    const double density = spec.dose * perLength;   // eV/m³
                    if (density >= spec.critical) {
                        amorphous[at] = 1;
                        anyAmorphous = true;
                        amorphousDeepest = std::max(amorphousDeepest, (sm.s + sm.ds - crystalFrom) * -sy);
                    }
                }))
                return {};
        }
        if (anyAmorphous) ++amorphousSlices;
        // Then the ions, spread across the beam with the lateral straggle
        // their depth has (σ⊥² + slope (z - Rp)), horizontally (σ/cos θ:
        // the tilt small); a channelled part where the crystal is not
        // made amorphous.
        double G = 1;
        if (!march(!anyAmorphous, [&ions, &G, &resistIndex, &stoppedIn, &sy, &crystal, &samples, &d, this, &added, &amorphous, &plus, &spec, &nx, &X0, &X1](const Sample& sm, double fraction, double zz, const Profile& p, bool) {
                const double dN = ions * fraction;
                G -= fraction;
                if (sm.material == resistIndex && resistIndex >= 0) {
                    stoppedIn[resistIndex] += dN;
                    return;
                }
                const double var = std::max(p.lateral * p.lateral + p.slope * (zz - p.Rp), std::pow(0.25 * p.lateral, 2));
                const double sigma = std::sqrt(var) / std::max(-sy, 0.05);
                const int j = sm.j;
                const bool crystalHere = crystal.count(sm.material) > 0;
                const std::size_t at = std::size_t(&sm - samples.data());
                auto put = [&](int i, double share) {
                    if (share <= 0) return;
                    const std::size_t k = d.index(i, j);
                    if (d.material[k] < 0) {
                        stoppedIn[-1] += dN * share;
                        return;
                    }
                    stoppedIn[d.material[k]] += dN * share;
                    const double c = dN * share / (cellWidth(i) * cellHeight(j));
                    added[k] += c;
                    if (crystalHere && crystal.count(d.material[k]) && !amorphous[at]) plus[k] += spec.plus * c;
                };
                if (!(sigma > 0)) {
                    put(sm.i, 1);
                    return;
                }
                // Over the columns within 5σ; what falls beyond the window's
                // edges mirrored back in (its images at 2 X0 - x, 2 X1 - x).
                const double reach = 5 * sigma;
                const int i0 = std::max(0, int(std::upper_bound(d.xe.begin(), d.xe.end(), sm.x - reach) - d.xe.begin()) - 1);
                const int i1 = std::min(nx - 1, int(std::upper_bound(d.xe.begin(), d.xe.end(), sm.x + reach) - d.xe.begin()) - 1);
                auto phi = [sigma](double u) { return 0.5 * std::erfc(-u / (std::sqrt(2.0) * sigma)); };
                const double centres[3] = {sm.x, 2 * X0 - sm.x, 2 * X1 - sm.x};
                for (int i = i0; i <= i1; ++i) {
                    const double a = d.xe[std::size_t(i)], b = d.xe[std::size_t(i + 1)];
                    double share = 0;
                    for (double c : centres) share += phi(b - c) - phi(a - c);
                    put(i, share);
                }
            }))
            return {};
        stoppedIn[-2] += ions * std::max(G, 0.0);
    }
    for (std::size_t k = 0; k < added.size(); ++k) {
        f->total[k] += added[k];
        f->active[k] += added[k];
    }
    double plusTotal = 0;
    for (double v : plus) plusTotal += v;
    if (plusTotal > 0) {
        ProcessDopants::Field* cl = d.field(QStringLiteral("Icl"));
        if (!cl) {
            d.fields.push_back({QStringLiteral("Icl"), std::vector<double>(d.material.size(), 0.0), {}});
            cl = &d.fields.back();
        }
        for (std::size_t k = 0; k < plus.size(); ++k) cl->total[k] += plus[k];
    }
    // What it says: the first crystal's moments (else the first solid's),
    // where the ions stopped.
    double all = 0;
    for (const auto& [m, n] : stoppedIn) all += n;
    QStringList where;
    for (const auto& [m, n] : stoppedIn) {
        if (n <= 1e-4 * all) continue;
        const QString what = m == -1 ? tr("out through sidewalls") : m == -2 ? tr("through the bottom") : m == resistIndex ? tr("the mask") : d.materials.at(m);
        where << tr("%1 % in %2").arg(formatNumber(100 * n / all, 3), what);
    }
    const Profile* shown = nullptr;
    QString shownIn;
    for (int pass = 0; pass < 2 && !shown; ++pass)
        for (const auto& [m, p] : profiles)
            if (p.ok && m != resistIndex && (pass == 1 || crystal.count(m))) {
                shown = &p;
                shownIn = d.materials.at(m);
                break;
            }
    const double unitToUm = a_unitScale * 1e6;
    QString said = tr("%1 %2 cm⁻² at %3 keV%4").arg(spec.species, formatNumber(spec.dose * 1e-4, 3), formatNumber(spec.energy / 1e3, 4),
                                                    spec.tilt != 0 ? tr(", tilted %1°").arg(formatNumber(spec.tilt * 180 / M_PI, 3)) : QString());
    if (shown)
        said += tr(": in %1 Rp %2 µm, ΔRp %3 µm, γ %4, β %5, lateral %6 µm (%7)")
                    .arg(shownIn, formatNumber(shown->Rp * unitToUm, 3), formatNumber(shown->main.sd() * unitToUm, 3), formatNumber(shown->main.gamma(), 3),
                         formatNumber(shown->main.beta(), 3), formatNumber(shown->lateral * unitToUm, 3), shown->m.source);
    if (!where.isEmpty()) said += tr("; %1").arg(where.join(QStringLiteral(", ")));
    if (amorphousSlices > 0) said += tr("; amorphous to %1 µm below the crystal's surface").arg(formatNumber(amorphousDeepest * unitToUm, 3));
    if (stoppedIn[-1] > 1e-3 * all) warnings->append(tr("%1 % scattered out through sidewalls into the air").arg(formatNumber(100 * stoppedIn[-1] / all, 3)));
    if (stoppedIn[-2] > 1e-3 * all) warnings->append(tr("%1 % went through the grid's bottom").arg(formatNumber(100 * stoppedIn[-2] / all, 3)));
    return said;
}

// -------------------------------------------------------------- Anneals

namespace {

/// An Arrhenius term: D0 e^(-E/kT).
struct Arr {
    double D0 = 0, E = 0;
    double at(double T) const { return D0 > 0 && T > 0 ? D0 * std::exp(-E / (KB * T)) : 0; }
};

/// A species in a material: its diffusivities by charge state (Fair's
/// D = D0 + D-(n/ni) + D=(n/ni)² for a donor, D0 + D+(p/ni) for an
/// acceptor; elsewhere their sum, intrinsic), a square law's (D_sq (C/Cref)²:
/// zinc's kick-out in a III-V), its solid solubility and its clusters' time
/// constant, a surface's dose loss, its share of diffusing with
/// interstitials.
struct InMaterial {
    Arr d0, dm, dmm, dp, sq;
    double Cref = 0;
    Arr css, tau;
    bool hasCss = false, hasTau = false;
    double loss = 0, fI = 1;
    bool moves() const { return d0.D0 > 0 || dm.D0 > 0 || dmm.D0 > 0 || dp.D0 > 0 || sq.D0 > 0; }
};

/// In silicon: Fair's diffusivities (R. B. Fair, in Impurity Doping
/// Processes in Silicon, North-Holland 1981; as Campbell tabulates them),
/// cm²/s and eV; the interstitials' shares Plummer, Deal and Griffin give
/// (Silicon VLSI Technology, 2000, ch. 7).
InMaterial siliconDefaults(const QString& species)
{
    InMaterial p;
    const double cm2 = 1e-4;
    if (species == QLatin1String("B")) {
        p.d0 = {0.037 * cm2, 3.46};
        p.dp = {0.72 * cm2, 3.46};
        p.fI = 1.0;
    } else if (species == QLatin1String("P")) {
        p.d0 = {3.85 * cm2, 3.66};
        p.dm = {4.44 * cm2, 4.00};
        p.dmm = {44.2 * cm2, 4.37};
        p.fI = 1.0;
    } else if (species == QLatin1String("As")) {
        p.d0 = {0.066 * cm2, 3.44};
        p.dm = {22.9 * cm2, 4.1};
        p.fI = 0.4;
    } else if (species == QLatin1String("Sb")) {
        p.d0 = {0.214 * cm2, 3.65};
        p.dm = {15.0 * cm2, 4.08};
        p.fI = 0.02;
    }
    return p;
}

/// Silicon's intrinsic carriers: 3.87e16 T^1.5 e^(-0.605 eV/kT) cm⁻³ (Morin
/// and Maita, Phys. Rev. 96, 28, 1954), m⁻³.
double siliconNi(double T)
{
    return 3.87e16 * std::pow(T, 1.5) * std::exp(-0.605 / (KB * T)) * 1e6;
}

/// Silicon's point defects (TSUPREM-4's set, as Heitzinger lists it - TU
/// Wien thesis, 2002, Table 14.1): the interstitials' diffusivity 51
/// e^(-1.77 eV/kT) cm²/s and equilibrium 2.9e24 e^(-3.18 eV/kT) cm⁻³.
double interstitialD(double T)
{
    return 51e-4 * std::exp(-1.77 / (KB * T));
}

double interstitialEquilibrium(double T)
{
    return 2.9e24 * std::exp(-3.18 / (KB * T)) * 1e6;
}

/// The ratio a species takes at an interface between silicon and its oxide
/// (C_Si/C_SiO2 at equilibrium): boron 0.3, phosphorus 10 (Grove, Leistiko
/// and Sah, J. Appl. Phys. 35, 2695, 1964), arsenic and antimony as
/// phosphorus, 10 (Plummer et al., ch. 6); 1 for others.
double siliconOxideRatio(const QString& species)
{
    if (species == QLatin1String("B")) return 0.3;
    if (species == QLatin1String("P") || species == QLatin1String("As") || species == QLatin1String("Sb")) return 10;
    return 1;
}

/// A tridiagonal system's solution (Thomas): a x(i-1) + b x(i) + c x(i+1) = r.
void tridiagonal(std::vector<double>& a, std::vector<double>& b, std::vector<double>& c, std::vector<double>& r, int n)
{
    for (int i = 1; i < n; ++i) {
        const double m = a[std::size_t(i)] / b[std::size_t(i - 1)];
        b[std::size_t(i)] -= m * c[std::size_t(i - 1)];
        r[std::size_t(i)] -= m * r[std::size_t(i - 1)];
    }
    r[std::size_t(n - 1)] /= b[std::size_t(n - 1)];
    for (int i = n - 2; i >= 0; --i) r[std::size_t(i)] = (r[std::size_t(i)] - c[std::size_t(i)] * r[std::size_t(i + 1)]) / b[std::size_t(i)];
}

} // namespace

/*!
 * The fluxes of one field over the grid and its steps in time: each face
 * between two cells carries J = α C_left - β C_right (from left to right,
 * or below to above); a face to the air takes g (C - C_ambient) out of its
 * cell. Peaceman and Rachford's alternating directions (second order;
 * backward Euler's in each direction for a stiff field), each a
 * tridiagonal solve a row or a column.
 */
struct Dopants::Solver {
    const ProcessDopants& d;
    int nx, ny;
    std::vector<double> hx, hy;                 // m
    std::vector<double> ax, bx, ay, by;         // faces: x (j (nx+1) + i, left of i), y (j nx + i, below j)
    std::vector<double> diagX, srcX, diagY, srcY;   // the air faces', by cell

    explicit Solver(const Dopants& o)
        : d(*o.a_d), nx(o.a_d->nx()), ny(o.a_d->ny())
    {
        for (int i = 0; i < nx; ++i) hx.push_back(o.cellWidth(i));
        for (int j = 0; j < ny; ++j) hy.push_back(o.cellHeight(j));
    }
    void clear()
    {
        ax.assign(std::size_t(nx + 1) * std::size_t(ny), 0.0);
        bx = ax;
        ay.assign(std::size_t(nx) * std::size_t(ny + 1), 0.0);
        by = ay;
        diagX.assign(std::size_t(nx) * std::size_t(ny), 0.0);
        srcX = diagX;
        diagY = diagX;
        srcY = diagX;
    }
    std::size_t fx(int i, int j) const { return std::size_t(j) * std::size_t(nx + 1) + std::size_t(i); }
    std::size_t fy(int i, int j) const { return std::size_t(j) * std::size_t(nx) + std::size_t(i); }

    /// The faces from each cell's D and potential ψ (nought: no field),
    /// an interface's ratio and transport (m = C_left/C_right at
    /// equilibrium, h; h <= 0 infinite), the air faces' (g's transport, C at
    /// the surface, the face's D).
    template <typename Interface, typename Air>
    void faces(const std::vector<double>& D, const std::vector<double>* psi, int charge, Interface&& interface, Air&& air)
    {
        clear();
        auto between = [this, &D, &psi, &charge, &interface](std::size_t kl, std::size_t kr, double hl, double hr, double* alpha, double* beta) {
            const int ml = d.material[kl], mr = d.material[kr];
            *alpha = *beta = 0;
            if (ml < 0 || mr < 0) return;
            const double Dl = D[kl], Dr = D[kr];
            if (ml == mr) {
                // (Within a material its mean: a diffusivity that grows with
                // the concentration - zinc's square law, arsenic's Fermi
                // level's - not choked at a front by a harmonic mean.)
                const double Df = 0.5 * (Dl + Dr), dist = 0.5 * (hl + hr);
                if (!(Df > 0)) return;
                const double delta = psi ? charge * ((*psi)[kr] - (*psi)[kl]) : 0.0;
                *alpha = Df / dist * bernoulli(delta);
                *beta = Df / dist * bernoulli(-delta);
                return;
            }
            // An interface: C_left = m C_right at equilibrium, J = (C_l - m
            // C_r)/(h_l/2D_l + 1/h + m h_r/2D_r) through its half-cells and
            // its transport h.
            double m = 1, h = 0;
            interface(ml, mr, &m, &h);
            if (!(Dl > 0) || !(Dr > 0) || !(m > 0)) return;
            const double R = hl / (2 * Dl) + (h > 0 ? 1 / h : 0) + m * hr / (2 * Dr);
            *alpha = 1 / R;
            *beta = m / R;
        };
        auto toAir = [&](std::size_t k, double h, double* diag, double* src) {
            double g = 0, c = 0, Dface = D[k];
            air(k, &g, &c, &Dface);
            if (!(g > 0) || !(Dface > 0)) return;
            const double G = 1 / (h / (2 * Dface) + (std::isfinite(g) ? 1 / g : 0));
            *diag += G;
            *src += G * c;
        };
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                const std::size_t k = d.index(i, j);
                if (i + 1 < nx) {
                    const std::size_t r = d.index(i + 1, j);
                    between(k, r, hx[std::size_t(i)], hx[std::size_t(i + 1)], &ax[fx(i + 1, j)], &bx[fx(i + 1, j)]);
                    // (A side face to the air.)
                    if (d.material[k] >= 0 && d.material[r] < 0) toAir(k, hx[std::size_t(i)], &diagX[k], &srcX[k]);
                    if (d.material[k] < 0 && d.material[r] >= 0) toAir(r, hx[std::size_t(i + 1)], &diagX[r], &srcX[r]);
                }
                if (d.material[k] < 0) continue;
                const bool up = j + 1 < ny;
                if (up) {
                    const std::size_t u = d.index(i, j + 1);
                    between(k, u, hy[std::size_t(j)], hy[std::size_t(j + 1)], &ay[fy(i, j + 1)], &by[fy(i, j + 1)]);
                    if (d.material[u] < 0) toAir(k, hy[std::size_t(j)], &diagY[k], &srcY[k]);
                } else {
                    toAir(k, hy[std::size_t(j)], &diagY[k], &srcY[k]);
                }
                if (j > 0 && d.material[d.index(i, j - 1)] < 0) toAir(k, hy[std::size_t(j)], &diagY[k], &srcY[k]);
            }
    }
    /// L_x C at cell (i, j), the air faces' sources with it.
    double lx(const std::vector<double>& C, int i, int j) const
    {
        const std::size_t k = d.index(i, j);
        double v = -diagX[k] * C[k] + srcX[k];
        if (i > 0) v += ax[fx(i, j)] * C[d.index(i - 1, j)] - bx[fx(i, j)] * C[k];
        if (i + 1 < nx) v -= ax[fx(i + 1, j)] * C[k] - bx[fx(i + 1, j)] * C[d.index(i + 1, j)];
        return v / hx[std::size_t(i)];
    }
    double ly(const std::vector<double>& C, int i, int j) const
    {
        const std::size_t k = d.index(i, j);
        double v = -diagY[k] * C[k] + srcY[k];
        if (j > 0) v += ay[fy(i, j)] * C[d.index(i, j - 1)] - by[fy(i, j)] * C[k];
        if (j + 1 < ny) v -= ay[fy(i, j + 1)] * C[k] - by[fy(i, j + 1)] * C[d.index(i, j + 1)];
        return v / hy[std::size_t(j)];
    }
    /// (I - τ L_x) out = rhs + τ srcX, row by row.
    void solveX(std::vector<double>& out, const std::vector<double>& rhs, double tau) const
    {
        std::vector<double> a(static_cast<std::size_t>(nx)), b(static_cast<std::size_t>(nx)), c(static_cast<std::size_t>(nx)), r(static_cast<std::size_t>(nx));
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                const std::size_t k = d.index(i, j);
                const double h = hx[std::size_t(i)];
                if (d.material[k] < 0) {
                    a[std::size_t(i)] = c[std::size_t(i)] = 0;
                    b[std::size_t(i)] = 1;
                    r[std::size_t(i)] = 0;
                    continue;
                }
                const double left = i > 0 ? ax[fx(i, j)] : 0, leftB = i > 0 ? bx[fx(i, j)] : 0;
                const double right = i + 1 < nx ? ax[fx(i + 1, j)] : 0, rightB = i + 1 < nx ? bx[fx(i + 1, j)] : 0;
                a[std::size_t(i)] = -tau * left / h;
                c[std::size_t(i)] = -tau * rightB / h;
                b[std::size_t(i)] = 1 + tau * (leftB + right + diagX[k]) / h;
                r[std::size_t(i)] = rhs[k] + tau * srcX[k] / h;
            }
            tridiagonal(a, b, c, r, nx);
            for (int i = 0; i < nx; ++i) out[d.index(i, j)] = r[std::size_t(i)];
        }
    }
    void solveY(std::vector<double>& out, const std::vector<double>& rhs, double tau) const
    {
        std::vector<double> a(static_cast<std::size_t>(ny)), b(static_cast<std::size_t>(ny)), c(static_cast<std::size_t>(ny)), r(static_cast<std::size_t>(ny));
        for (int i = 0; i < nx; ++i) {
            for (int j = 0; j < ny; ++j) {
                const std::size_t k = d.index(i, j);
                const double h = hy[std::size_t(j)];
                if (d.material[k] < 0) {
                    a[std::size_t(j)] = c[std::size_t(j)] = 0;
                    b[std::size_t(j)] = 1;
                    r[std::size_t(j)] = 0;
                    continue;
                }
                const double below = j > 0 ? ay[fy(i, j)] : 0, belowB = j > 0 ? by[fy(i, j)] : 0;
                const double above = j + 1 < ny ? ay[fy(i, j + 1)] : 0, aboveB = j + 1 < ny ? by[fy(i, j + 1)] : 0;
                a[std::size_t(j)] = -tau * below / h;
                c[std::size_t(j)] = -tau * aboveB / h;
                b[std::size_t(j)] = 1 + tau * (belowB + above + diagY[k]) / h;
                r[std::size_t(j)] = rhs[k] + tau * srcY[k] / h;
            }
            tridiagonal(a, b, c, r, ny);
            for (int j = 0; j < ny; ++j) out[d.index(i, j)] = r[std::size_t(j)];
        }
    }
    /// A step dt: Peaceman and Rachford's, or backward Euler's in each
    /// direction (\a stiff: damping what changes fast).
    void step(std::vector<double>& C, double dt, bool stiff) const
    {
        std::vector<double> rhs(C.size()), half(C.size());
        if (stiff) {
            solveX(half, C, dt);
            solveY(C, half, dt);
            return;
        }
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                const std::size_t k = d.index(i, j);
                rhs[k] = d.material[k] < 0 ? 0 : C[k] + 0.5 * dt * ly(C, i, j);
            }
        solveX(half, rhs, 0.5 * dt);
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                const std::size_t k = d.index(i, j);
                rhs[k] = d.material[k] < 0 ? 0 : half[k] + 0.5 * dt * lx(half, i, j);
            }
        solveY(C, rhs, 0.5 * dt);
    }
};

namespace {

/*!
 * An anneal's dopants in time: the species' diffusivities in each cell
 * from their models (Fermi-level dependent in silicon, the field of the
 * carriers on them; intrinsic elsewhere; enhanced by interstitials), each
 * a step of the solver, then each's activation (its solubility, its
 * clusters' time constant) and the interstitials' (their clusters'
 * emission, their diffusion to the surfaces). A hook between steps moves
 * an oxidation's interface.
 */
class Annealer
{
public:
    struct Sp {
        int field;
        QString name;
        std::vector<InMaterial> in;   // by material
        bool source = false;
    };

    Annealer(ProcessDopants& d, const AnnealSpec& spec, const std::vector<bool>& silicon, const std::vector<bool>& oxide,
             const std::vector<QString>& names, QStringList* warnings)
        : a_d(d), a_spec(spec), a_silicon(silicon), a_oxide(oxide), a_names(names), a_warnings(warnings)
    {
    }

    QString setUp()
    {
        // The source's species: a field of it.
        if (!a_spec.source.trimmed().isEmpty()) {
            const QString s = a_spec.source.compare(QLatin1String("BF2"), Qt::CaseInsensitive) == 0 ? QStringLiteral("B") : a_spec.source.trimmed();
            if (!a_d.field(s)) a_d.fields.push_back({s, std::vector<double>(a_d.material.size(), 0.0), std::vector<double>(a_d.material.size(), 0.0)});
        }
        for (int f = 0; f < int(a_d.fields.size()); ++f) {
            ProcessDopants::Field& fd = a_d.fields[std::size_t(f)];
            if (!isSpecies(fd.name)) continue;
            if (fd.active.empty()) fd.active = fd.total;
            Sp sp;
            sp.field = f;
            sp.name = fd.name;
            sp.source = !a_spec.source.trimmed().isEmpty()
                        && (fd.name == a_spec.source.trimmed() || (fd.name == QLatin1String("B") && a_spec.source.compare(QLatin1String("BF2"), Qt::CaseInsensitive) == 0));
            for (std::size_t m = 0; m < a_names.size(); ++m) {
                InMaterial p = a_silicon[m] ? siliconDefaults(fd.name) : InMaterial();
                for (const Override& o : a_spec.overrides) {
                    if (o.species.compare(fd.name, Qt::CaseInsensitive) != 0 || o.material.compare(a_names[m], Qt::CaseInsensitive) != 0) continue;
                    const QString& key = o.key;
                    if (key == QLatin1String("D0")) p.d0.D0 = o.value;
                    else if (key == QLatin1String("Ea")) p.d0.E = o.value;
                    else if (key == QLatin1String("Dm0")) p.dm.D0 = o.value;
                    else if (key == QLatin1String("Em")) p.dm.E = o.value;
                    else if (key == QLatin1String("Dmm0")) p.dmm.D0 = o.value;
                    else if (key == QLatin1String("Emm")) p.dmm.E = o.value;
                    else if (key == QLatin1String("Dp0")) p.dp.D0 = o.value;
                    else if (key == QLatin1String("Ep")) p.dp.E = o.value;
                    else if (key == QLatin1String("Dsq0")) p.sq.D0 = o.value;
                    else if (key == QLatin1String("Esq")) p.sq.E = o.value;
                    else if (key == QLatin1String("Cref")) p.Cref = o.value;
                    else if (key == QLatin1String("Css0")) {
                        p.css.D0 = o.value;
                        p.hasCss = true;
                    } else if (key == QLatin1String("Ecss")) p.css.E = o.value;
                    else if (key == QLatin1String("tau0")) {
                        p.tau.D0 = o.value;
                        p.hasTau = true;
                    } else if (key == QLatin1String("Etau")) p.tau.E = -o.value;   // (τ = τ0 e^(+E/kT))
                    else if (key == QLatin1String("loss")) p.loss = o.value;
                    else if (key == QLatin1String("fI")) p.fI = o.value;
                }
                if (p.sq.D0 > 0 && !(p.Cref > 0)) return tr("%1 in %2: its square law wants its reference concentration (Cref)").arg(fd.name, a_names[m]);
                sp.in.push_back(p);
            }
            a_species.push_back(sp);
        }
        // What does not move, said: a species present in a material with no
        // diffusivity given, none defaulted.
        for (const Sp& sp : a_species)
            for (std::size_t m = 0; m < a_names.size(); ++m) {
                if (sp.in[m].moves()) continue;
                bool present = false;
                const std::vector<double>& v = a_d.fields[std::size_t(sp.field)].total;
                for (std::size_t k = 0; k < v.size() && !present; ++k) present = a_d.material[k] == int(m) && v[k] > 0;
                if (present)
                    a_warnings->append(tr("%1 does not move in %2: no diffusivity given (none defaulted there)").arg(sp.name, a_names[m]));
            }
        return {};
    }

    /// Each cell's diffusivity of species s at T; the potential ln(n/ni)
    /// in silicon (nought elsewhere, or with the constant model).
    void coefficients(double T, std::vector<std::vector<double>>* D, std::vector<double>* psi) const
    {
        const std::size_t n = a_d.material.size();
        psi->assign(n, 0.0);
        const bool fermi = a_spec.model != QLatin1String("constant");
        const double ni = siliconNi(T);
        std::vector<double> ratio(n, 1.0);   // n/ni
        if (fermi)
            for (std::size_t k = 0; k < n; ++k) {
                const int m = a_d.material[k];
                if (m < 0 || !a_silicon[std::size_t(m)]) continue;
                double net = 0;
                for (const Sp& sp : a_species) {
                    const int q = dopantCharge(sp.name, 4);
                    if (q != 0) net += q * a_d.fields[std::size_t(sp.field)].active[k];
                }
                const double nn = 0.5 * net + std::sqrt(0.25 * net * net + ni * ni);
                ratio[k] = nn / ni;
                (*psi)[k] = std::log(ratio[k]);
            }
        // Interstitials' supersaturation (the defects model): s = CI/CI*,
        // vacancies in equilibrium with them (CV/CV* = 1/s).
        std::vector<double> ss(n, 1.0);
        if (a_spec.model == QLatin1String("defects"))
            if (const ProcessDopants::Field* I = a_d.field(QStringLiteral("I"))) {
                const double eq = interstitialEquilibrium(T);
                for (std::size_t k = 0; k < n; ++k) ss[k] = 1 + std::max(I->total[k], 0.0) / eq;
            }
        a_ratio = ratio;
        a_ss = ss;
        D->assign(a_species.size(), std::vector<double>(n, 0.0));
        for (std::size_t s = 0; s < a_species.size(); ++s) {
            const std::vector<double>& C = a_d.fields[std::size_t(a_species[s].field)].active;
            for (std::size_t k = 0; k < n; ++k)
                if (a_d.material[k] >= 0) (*D)[s][k] = diffusivity(s, k, C[k], T);
        }
    }
    /// Species s's diffusivity in cell k at concentration C (the cell's
    /// n/ni and interstitials as coefficients() last found them).
    double diffusivity(std::size_t s, std::size_t k, double C, double T) const
    {
        const Sp& sp = a_species[s];
        const int m = a_d.material[k];
        if (m < 0) return 0;
        const InMaterial& p = sp.in[std::size_t(m)];
        const int q = dopantCharge(sp.name, 4);
        const bool fermi = a_spec.model != QLatin1String("constant");
        const double r = a_silicon[std::size_t(m)] && fermi && k < a_ratio.size() ? a_ratio[k] : 1.0;
        double v = p.d0.at(T);
        if (q > 0) v += p.dm.at(T) * r + p.dmm.at(T) * r * r;
        else if (q < 0) v += p.dp.at(T) / r;
        else v += p.dm.at(T) + p.dmm.at(T) + p.dp.at(T);
        if (p.sq.D0 > 0) v += p.sq.at(T) * std::pow(std::max(C, 0.0) / p.Cref, 2);
        const double ss = k < a_ss.size() ? a_ss[k] : 1.0;
        if (a_silicon[std::size_t(m)] && ss != 1) v *= p.fI * ss + (1 - p.fI) / ss;
        return v;
    }

    /// Its time steps over the recipe (\a hook after each: the oxidation's
    /// interface); false, and why, when it could not.
    bool run(Dopants::Solver& solver, const std::function<void(double t, double dt)>& hook, int* steps, QString* error)
    {
        const double duration = a_spec.thermal.duration();
        if (!(duration > 0)) {
            *error = tr("its time must be more than 0");
            return false;
        }
        const std::size_t n = a_d.material.size();
        // The first step: a fifth of the finest cell's diffusion time at the
        // highest temperature, at most a thousandth of the whole.
        double Dmax = 0;
        {
            std::vector<std::vector<double>> D;
            std::vector<double> psi;
            coefficients(a_spec.thermal.highest(), &D, &psi);
            for (const auto& v : D)
                for (double x : v) Dmax = std::max(Dmax, x);
        }
        double hmin = std::numeric_limits<double>::max();
        for (double h : solver.hy) hmin = std::min(hmin, h);
        for (double h : solver.hx) hmin = std::min(hmin, h);
        double dt = Dmax > 0 ? std::min(0.2 * hmin * hmin / Dmax, duration * 1e-3) : duration * 1e-3;
        dt = std::max(dt, duration * 1e-9);
        const bool defects = a_spec.model == QLatin1String("defects");
        ProcessDopants::Field* I = nullptr;
        ProcessDopants::Field* Icl = a_d.field(QStringLiteral("Icl"));
        if (defects) {
            if (!a_d.field(QStringLiteral("I"))) a_d.fields.push_back({QStringLiteral("I"), std::vector<double>(n, 0.0), {}});
            I = a_d.field(QStringLiteral("I"));
            Icl = a_d.field(QStringLiteral("Icl"));
        }
        double t = 0;
        int k = 0;
        std::vector<double> breaks;
        for (const auto& p : a_spec.thermal.points) breaks.push_back(p.first);
        while (t < duration * (1 - 1e-12)) {
            // Not past a break of the recipe; within a ramp, a tenth of it.
            double next = duration;
            double segment = duration;
            for (std::size_t b = 0; b + 1 < breaks.size(); ++b)
                if (breaks[b + 1] > t * (1 + 1e-12) + 1e-15) {
                    next = breaks[b + 1];
                    if (std::abs(a_spec.thermal.points[b + 1].second - a_spec.thermal.points[b].second) > 1e-9) segment = breaks[b + 1] - breaks[b];
                    break;
                }
            double h = std::min({dt, next - t, segment / 10});
            if (next - t - h < 1e-6 * h) h = next - t;
            const double T = a_spec.thermal.at(t + h / 2);
            // (Peaceman and Rachford's rings at a front a diffusivity of the
            // concentration makes steep - zinc's, arsenic's -, and at what an
            // interface moving or the interstitials change: backward Euler's.)
            const bool nonlinearNow = a_spec.model != QLatin1String("constant") && nonlinear(T);
            const bool stiff = k < 2 || defects || hook || nonlinearNow;
            // Predictor, then (nonlinear) a corrector from the mean of the two.
            std::vector<std::vector<double>> before;
            for (const Sp& sp : a_species) before.push_back(a_d.fields[std::size_t(sp.field)].active);
            std::vector<std::vector<double>> D;
            std::vector<double> psi;
            coefficients(T, &D, &psi);
            auto advance = [this, &before, &solver, &T, &h, &stiff](const std::vector<std::vector<double>>& Ds, const std::vector<double>& ps) {
                for (std::size_t s = 0; s < a_species.size(); ++s) {
                    const Sp& sp = a_species[s];
                    std::vector<double>& C = a_d.fields[std::size_t(sp.field)].active;
                    C = before[s];
                    const int q = dopantCharge(sp.name, 4);
                    solver.faces(
                        Ds[s], &ps, q,
                        [this, &sp](int ml, int mr, double* m, double* hh) { interfaceOf(sp, ml, mr, m, hh); },
                        [this, &sp, &before, s, T](std::size_t cell, double* g, double* c, double* Dface) {
                            const int m = a_d.material[cell];
                            if (sp.source) {
                                *g = std::numeric_limits<double>::infinity();
                                *c = a_spec.surface;
                                // (Its face's diffusivity at the mean of the
                                // cell's and the source's: a square law's
                                // nought in an empty cell.)
                                *Dface = diffusivity(s, cell, 0.5 * (before[s][cell] + a_spec.surface), T);
                            } else if (m >= 0 && sp.in[std::size_t(m)].loss > 0) {
                                *g = sp.in[std::size_t(m)].loss;
                                *c = 0;
                            }
                        });
                    solver.step(C, h, stiff);
                    for (double& v : C) v = std::max(v, 0.0);
                }
            };
            advance(D, psi);
            if (nonlinearNow) {
                // Backward Euler's coefficients those of the step's end
                // (Picard's: each pass from the last, until the dopants move
                // less than 1e-3 of their largest between two).
                for (int pass = 0; pass < 6; ++pass) {
                    std::vector<std::vector<double>> last;
                    for (const Sp& sp : a_species) last.push_back(a_d.fields[std::size_t(sp.field)].active);
                    std::vector<std::vector<double>> D2;
                    std::vector<double> psi2;
                    coefficients(T, &D2, &psi2);
                    for (std::size_t s = 0; s < D.size(); ++s)
                        for (std::size_t c = 0; c < n; ++c) D[s][c] = 0.5 * (D[s][c] + D2[s][c]);
                    psi = psi2;
                    advance(D, psi);
                    double moved = 0, largest = 0;
                    for (std::size_t s = 0; s < a_species.size(); ++s) {
                        const std::vector<double>& C = a_d.fields[std::size_t(a_species[s].field)].active;
                        for (std::size_t c = 0; c < n; ++c) {
                            moved = std::max(moved, std::abs(C[c] - last[s][c]));
                            largest = std::max(largest, C[c]);
                        }
                    }
                    if (moved <= 1e-3 * largest) break;
                }
            }
            // The totals: what moved (the active part) and what stayed.
            for (std::size_t s = 0; s < a_species.size(); ++s) {
                ProcessDopants::Field& f = a_d.fields[std::size_t(a_species[s].field)];
                for (std::size_t c = 0; c < n; ++c) f.total[c] = std::max(0.0, f.total[c] - before[s][c] + f.active[c]);
            }
            activate(T, h);
            if (defects) interstitials(solver, T, h, I, Icl);
            if (hook) hook(t, h);
            t += h;
            ++k;
            // (Backward Euler's first order: no more than a hundredth of the
            // anneal a step - a square law's front 13 % off its √t at a
            // twentieth.)
            dt = std::min(h * 1.05, duration / (stiff ? 100 : 20));
            if (k > 200000) {
                *error = tr("it took too many time steps");
                return false;
            }
        }
        *steps = k;
        return true;
    }

    const std::vector<Sp>& species() const { return a_species; }

private:
    bool nonlinear(double T) const
    {
        // Fermi-level dependence matters where a dopant passes a tenth of
        // ni; a square law always.
        const double ni = siliconNi(T);
        for (const Sp& sp : a_species) {
            for (const InMaterial& p : sp.in)
                if (p.sq.D0 > 0) return true;
            const std::vector<double>& C = a_d.fields[std::size_t(sp.field)].active;
            for (std::size_t k = 0; k < C.size(); ++k)
                if (C[k] > 0.1 * ni && a_d.material[k] >= 0 && a_silicon[std::size_t(a_d.material[k])]) return true;
        }
        return false;
    }
    void interfaceOf(const Sp& sp, int ml, int mr, double* m, double* h) const
    {
        *m = 1;
        *h = 0;
        if (a_silicon[std::size_t(ml)] && a_oxide[std::size_t(mr)]) *m = siliconOxideRatio(sp.name);
        else if (a_oxide[std::size_t(ml)] && a_silicon[std::size_t(mr)]) *m = 1 / siliconOxideRatio(sp.name);
        for (const Override& o : a_spec.overrides) {
            if (o.species.compare(sp.name, Qt::CaseInsensitive) != 0) continue;
            const bool lr = o.material.compare(a_names[std::size_t(ml)], Qt::CaseInsensitive) == 0;
            const bool rl = o.material.compare(a_names[std::size_t(mr)], Qt::CaseInsensitive) == 0;
            const QString other = o.key.section(QLatin1Char(':'), 1).trimmed();
            if (o.key.startsWith(QLatin1String("m:"))) {
                if (lr && other.compare(a_names[std::size_t(mr)], Qt::CaseInsensitive) == 0) *m = o.value;
                if (rl && other.compare(a_names[std::size_t(ml)], Qt::CaseInsensitive) == 0) *m = 1 / o.value;
            } else if (o.key.startsWith(QLatin1String("h:"))) {
                if ((lr && other.compare(a_names[std::size_t(mr)], Qt::CaseInsensitive) == 0)
                    || (rl && other.compare(a_names[std::size_t(ml)], Qt::CaseInsensitive) == 0))
                    *h = o.value;
            }
        }
    }
    /// Each species' active part: its solubility's (where given), the rest
    /// clustered - at once, or with its time constant.
    void activate(double T, double dt)
    {
        for (const Sp& sp : a_species) {
            ProcessDopants::Field& f = a_d.fields[std::size_t(sp.field)];
            for (std::size_t k = 0; k < f.total.size(); ++k) {
                const int m = a_d.material[k];
                if (m < 0) continue;
                const InMaterial& p = sp.in[std::size_t(m)];
                if (!p.hasCss) {
                    f.active[k] = f.total[k];
                    continue;
                }
                const double css = p.css.at(T);
                const double want = std::max(0.0, f.total[k] - css);   // inactive
                double inactive = std::max(0.0, f.total[k] - f.active[k]);
                if (p.hasTau && p.tau.D0 > 0) inactive += (want - inactive) * (1 - std::exp(-dt / p.tau.at(T)));
                else inactive = want;
                f.active[k] = std::max(0.0, f.total[k] - inactive);
            }
        }
    }
    /// The interstitials: their clusters emit them (a lifetime from its
    /// value at a temperature, its activation energy); they diffuse, held at
    /// equilibrium at silicon's surfaces and interfaces (as the excess,
    /// nought there).
    void interstitials(Dopants::Solver& solver, double T, double dt, ProcessDopants::Field* I, ProcessDopants::Field* Icl)
    {
        const std::size_t n = a_d.material.size();
        if (Icl) {
            const double life = a_spec.clusterLife * std::exp(a_spec.clusterE / KB * (1 / T - 1 / a_spec.clusterLifeT));
            const double gone = 1 - std::exp(-dt / life);
            for (std::size_t k = 0; k < n; ++k) {
                const double e = Icl->total[k] * gone;
                Icl->total[k] -= e;
                I->total[k] += e;
            }
        }
        std::vector<double> D(n, 0.0);
        const double DI = interstitialD(T);
        for (std::size_t k = 0; k < n; ++k)
            if (a_d.material[k] >= 0 && a_silicon[std::size_t(a_d.material[k])]) D[k] = DI;
        // (At silicon's faces to anything else: the excess held at nought.)
        auto held = [](int, int, double* m, double* h) {
            *m = 1;
            *h = 0;
        };
        solver.faces(D, nullptr, 0, held, [](std::size_t, double* g, double* c, double*) {
            *g = std::numeric_limits<double>::infinity();
            *c = 0;
        });
        // Interfaces silicon | other: a sink as the air's.
        for (int j = 0; j < solver.ny; ++j)
            for (int i = 0; i < solver.nx; ++i) {
                const std::size_t k = a_d.index(i, j);
                if (!(D[k] > 0)) continue;
                auto sink = [&](std::size_t other, double h, double* diag) {
                    if (a_d.material[other] >= 0 && !(D[other] > 0)) *diag += 2 * D[k] / h;
                };
                if (i > 0) sink(a_d.index(i - 1, j), solver.hx[std::size_t(i)], &solver.diagX[k]);
                if (i + 1 < solver.nx) sink(a_d.index(i + 1, j), solver.hx[std::size_t(i)], &solver.diagX[k]);
                if (j > 0) sink(a_d.index(i, j - 1), solver.hy[std::size_t(j)], &solver.diagY[k]);
                if (j + 1 < solver.ny) sink(a_d.index(i, j + 1), solver.hy[std::size_t(j)], &solver.diagY[k]);
            }
        solver.step(I->total, dt, true);
        for (std::size_t k = 0; k < n; ++k)
            if (!(D[k] > 0)) I->total[k] = 0;
    }

    ProcessDopants& a_d;
    const AnnealSpec& a_spec;
    std::vector<bool> a_silicon, a_oxide;
    std::vector<QString> a_names;
    QStringList* a_warnings;
    std::vector<Sp> a_species;
    mutable std::vector<double> a_ratio, a_ss;   // by cell: n/ni, CI/CI*
};

QString durationText(double s)
{
    if (s >= 3600) return QStringLiteral("%1 h").arg(formatNumber(s / 3600, 3));
    if (s >= 60) return QStringLiteral("%1 min").arg(formatNumber(s / 60, 3));
    return QStringLiteral("%1 s").arg(formatNumber(s, 3));
}

} // namespace

QString Dopants::anneal(const AnnealSpec& spec, const std::vector<Region>& regions, QStringList* warnings, QString* error)
{
    for (const Region& r : regions) materialIndex(r.material);
    std::vector<bool> silicon, oxide;
    std::vector<QString> names;
    for (int m = 0; m < int(a_d->materials.size()); ++m) {
        silicon.push_back(info(m).silicon);
        oxide.push_back(info(m).oxide);
        names.push_back(a_d->materials.at(m));
    }
    Annealer a(*a_d, spec, silicon, oxide, names, warnings);
    if (const QString why = a.setUp(); !why.isEmpty()) {
        *error = why;
        return {};
    }
    if (a.species().empty() && spec.model != QLatin1String("defects")) warnings->append(tr("no dopants to move"));
    // Each species' dose before and after (over the window, per unit
    // length out of the plane), what of it is inactive after.
    auto amounts = [this](bool activeOnly) {
        std::map<QString, double> out;
        for (const ProcessDopants::Field& f : a_d->fields) {
            if (!isSpecies(f.name)) continue;
            double sum = 0;
            for (int j = 0; j < a_d->ny(); ++j)
                for (int i = 0; i < a_d->nx(); ++i) {
                    const std::size_t k = a_d->index(i, j);
                    if (a_d->material[k] >= 0) sum += (activeOnly && !f.active.empty() ? f.active : f.total)[k] * cellWidth(i) * cellHeight(j);
                }
            out[f.name] = sum;
        }
        return out;
    };
    const std::map<QString, double> before = amounts(false);
    Solver solver(*this);
    int steps = 0;
    if (!a.run(solver, {}, &steps, error)) return {};
    const std::map<QString, double> after = amounts(false), active = amounts(true);
    QStringList changed;
    for (const auto& [name, was] : before) {
        const double now = after.count(name) ? after.at(name) : 0;
        if (was > 0 && std::abs(now / was - 1) > 1e-3)
            changed << (now < was ? tr("%1 % of the %2 lost at the surfaces").arg(formatNumber(100 * (1 - now / was), 3), name)
                                  : tr("the %1 taken in %2-fold").arg(name, formatNumber(now / was, 3)));
    }
    if (!spec.source.isEmpty())
        for (const auto& [name, now] : after)
            if (!before.count(name) || !(before.at(name) > 0)) {
                const double width = (a_d->xe.back() - a_d->xe.front()) * a_unitScale;
                changed << tr("%1 taken in: %2 cm⁻²").arg(name, formatNumber(now / width * 1e-4, 3));
            }
    for (const auto& [name, all] : after)
        if (all > 0 && active.count(name) && active.at(name) < (1 - 1e-3) * all)
            changed << tr("%1 % of the %2 inactive (above its solubility)").arg(formatNumber(100 * (1 - active.at(name) / all), 3), name);
    QString what = tr("%1 at %2 °C").arg(durationText(spec.thermal.duration()), formatNumber(spec.thermal.highest() - 273.15, 4));
    if (spec.thermal.points.size() > 2) what = tr("%1, up to %2 °C").arg(durationText(spec.thermal.duration()), formatNumber(spec.thermal.highest() - 273.15, 4));
    QStringList models;
    models << (spec.model == QLatin1String("constant") ? tr("intrinsic diffusivities") : spec.model == QLatin1String("defects") ? tr("Fermi level and interstitials (TED)") : tr("Fermi level"));
    if (!spec.source.isEmpty()) models << tr("%1 at %2 cm⁻³ at the surface").arg(spec.source, formatNumber(spec.surface * 1e-6, 3));
    return tr("%1 (%2), %3 steps%4").arg(what, models.join(QStringLiteral(", "))).arg(steps).arg(changed.isEmpty() ? QString() : QStringLiteral("; ") + changed.join(QStringLiteral("; ")));
}

// ------------------------------------------------------------- Oxidation

namespace {

/// An oxidation's rates at T (SI): Deal and Grove's B and B/A, Massoud's
/// thin-regime terms K1 e^(-t/τ1) + K2 e^(-t/τ2) added to B (dry, 800 to
/// 1000 °C).
struct Law {
    double B = 0, BA = 0, K1 = 0, K2 = 0, tau1 = 1, tau2 = 1;
};

Law lawOf(const OxideSpec& s, double T, QStringList* notes)
{
    const double kT = KB * T;
    const QString o = s.orientation.isEmpty() ? QStringLiteral("100") : s.orientation;
    Law l;
    auto note = [notes](const QString& n) {
        if (notes && !notes->contains(n)) notes->append(n);
    };
    if (s.givenB && s.givenBA) {
        l.B = *s.givenB;
        l.BA = *s.givenBA;
    } else if (s.wet || s.model == QLatin1String("dealgrove")) {
        // Deal and Grove (J. Appl. Phys. 36, 3770, 1965) for (111), B/A over
        // 1.68 on (100): dry 7.72e2 µm²/h, 1.23 eV and 6.23e6 µm/h, 2.0 eV;
        // wet (640 Torr of water) 3.86e2, 0.78 and 1.63e8, 2.05.
        const double um2h = 1e-12 / 3600, umh = 1e-6 / 3600;
        if (s.wet) {
            l.B = 3.86e2 * um2h * std::exp(-0.78 / kT);
            l.BA = 1.63e8 * umh * std::exp(-2.05 / kT);
        } else {
            l.B = 7.72e2 * um2h * std::exp(-1.23 / kT);
            l.BA = 6.23e6 * umh * std::exp(-2.0 / kT);
        }
        if (o == QLatin1String("100")) l.BA /= 1.68;
        else if (o == QLatin1String("110")) note(tr("(110): Deal and Grove gave (111) and (100) alone - (111)'s taken"));
    } else {
        // Massoud, Plummer and Irene (J. Electrochem. Soc. 132, 2685, 1985)
        // and Massoud and Plummer's thin-regime terms (J. Appl. Phys. 62,
        // 3416, 1987), as Hollauer's thesis tabulates them (TU Wien 2007,
        // Tables 2.3, 2.4): nm²/min, nm/min, min; τ = τ0 e^(+E/kT).
        const double nm2min = 1e-18 / 60, nmmin = 1e-9 / 60;
        const bool hot = T >= 1273.15;
        struct Row {
            double CB, EB, CBA, EBA;
        };
        Row r{};
        if (!hot) {
            if (o == QLatin1String("111")) r = {1.34e9, 1.71, 1.32e7, 1.74};
            else if (o == QLatin1String("110")) r = {3.73e8, 1.63, 4.73e8, 2.10};
            else r = {1.70e11, 2.22, 7.35e6, 1.76};
        } else {
            if (o == QLatin1String("100")) r = {1.31e5, 0.68, 3.53e12, 3.20};
            else {
                r = {2.56e5, 0.76, 6.50e11, 2.95};
                if (o == QLatin1String("110")) note(tr("(110) above 1000 °C: Massoud gave (100) and (111) alone - (111)'s taken"));
            }
        }
        l.B = r.CB * nm2min * std::exp(-r.EB / kT);
        l.BA = r.CBA * nmmin * std::exp(-r.EBA / kT);
        if (T >= 1073.15 - 1 && !hot) {
            struct K {
                double K1, E1, K2, E2, t1, Et1, t2, Et2;
            };
            K k{};
            if (o == QLatin1String("111")) k = {2.70e9, 1.74, 1.33e9, 1.76, 1.72e-6, 1.45, 1.56e-7, 1.90};
            else if (o == QLatin1String("110")) k = {4.07e8, 1.54, 1.20e8, 1.56, 5.38e-9, 2.02, 1.63e-8, 2.12};
            else k = {2.49e11, 2.18, 3.72e11, 2.28, 4.14e-6, 1.38, 2.71e-7, 1.88};
            l.K1 = k.K1 * nm2min * std::exp(-k.E1 / kT);
            l.K2 = k.K2 * nm2min * std::exp(-k.E2 / kT);
            l.tau1 = k.t1 * 60 * std::exp(k.Et1 / kT);
            l.tau2 = k.t2 * 60 * std::exp(k.Et2 / kT);
        } else {
            note(tr("Massoud's thin-regime terms are fitted from 800 to 1000 °C: none outside"));
        }
    }
    // The oxidant's pressure (Henry's law: both rates go as it), a factor
    // (HCl's).
    l.B *= s.pressure * s.factor;
    l.BA *= s.pressure * s.factor;
    l.K1 *= s.pressure * s.factor;
    l.K2 *= s.pressure * s.factor;
    return l;
}

/// The oxide's thickness through the recipe from x0 (m): on a time grid,
/// RK4 on dx/dt = (B + K1 e^(-t/τ1) + K2 e^(-t/τ2))/(2x + A).
std::vector<double> growth(const OxideSpec& s, double x0, const std::vector<double>& times, QStringList* notes)
{
    std::vector<double> x(times.size(), x0);
    auto rate = [&](double t, double xx) {
        const Law l = lawOf(s, s.thermal.at(t), notes);
        if (!(l.BA > 0)) return 0.0;
        const double A = l.B / l.BA;
        return (l.B + l.K1 * std::exp(-t / l.tau1) + l.K2 * std::exp(-t / l.tau2)) / (2 * xx + A);
    };
    for (std::size_t k = 1; k < times.size(); ++k) {
        const double t = times[k - 1], h = times[k] - times[k - 1];
        double xx = x[k - 1];
        const int sub = 4;
        for (int q = 0; q < sub; ++q) {
            const double tt = t + q * h / sub, hh = h / sub;
            const double k1 = rate(tt, xx), k2 = rate(tt + hh / 2, xx + hh / 2 * k1), k3 = rate(tt + hh / 2, xx + hh / 2 * k2),
                         k4 = rate(tt + hh, xx + hh * k3);
            xx += hh / 6 * (k1 + 2 * k2 + 2 * k3 + k4);
        }
        x[k] = xx;
    }
    return x;
}

} // namespace

QString Dopants::oxidize(const OxideSpec& spec, std::vector<Region>& regions, cInt x0, cInt x1, cInt bottom, QStringList* warnings, QString* error)
{
    Q_UNUSED(x0);
    Q_UNUSED(x1);
    Q_UNUSED(bottom);
    ProcessDopants& d = *a_d;
    for (const Region& r : regions) materialIndex(r.material);
    const int oxideIndex = materialIndex(spec.material);
    const int nx = d.nx();
    QStringList notes;
    // Each column: its silicon's top (the oxidizable: silicon, polysilicon),
    // the oxide over it, whether covered (by a mask closed there, or by
    // anything but oxide over it).
    struct Column {
        double s = NaN, xi = 0;   // the geometry's unit
        bool silicon = false, covered = false;
        double delta = 0;         // the oxide grown (geometry's unit)
    };
    std::vector<Column> cols(static_cast<std::size_t>(nx));
    std::vector<double> X(static_cast<std::size_t>(nx));
    for (int i = 0; i < nx; ++i) {
        X[std::size_t(i)] = 0.5 * (d.xe[std::size_t(i)] + d.xe[std::size_t(i + 1)]);
        struct Piece {
            double lo, hi;
            int region;
        };
        std::vector<Piece> pieces;
        for (std::size_t r = 0; r < regions.size(); ++r)
            for (const auto& [a, b] : verticalIntervals(regions[r], X[std::size_t(i)] * a_k)) pieces.push_back({a / a_k, b / a_k, int(r)});
        std::sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) { return a.hi > b.hi; });
        Column& c = cols[std::size_t(i)];
        double oxide = 0, at = std::numeric_limits<double>::infinity();
        for (const Piece& p : pieces) {
            const MaterialInfo& mi = info(materialIndex(regions[std::size_t(p.region)].material));
            const bool touching = !std::isfinite(at) || std::abs(at - p.hi) < 2.0 / a_k;
            if (mi.silicon) {
                c.silicon = true;
                c.s = p.hi;
                c.xi = touching ? oxide : 0;
                if (!touching) c.covered = true;
                break;
            }
            if (mi.oxide && touching) oxide += p.hi - p.lo;
            else c.covered = true;
            at = p.lo;
        }
        if (spec.masked) {
            bool open = false;
            for (const auto& [a, b] : spec.open) open = open || (X[std::size_t(i)] * a_k >= double(a) && X[std::size_t(i)] * a_k < double(b));
            if (!open) c.covered = true;
        }
    }
    // The thickness grown on each open column, on the recipe's time grid.
    const double duration = spec.thermal.duration();
    if (!(duration > 0)) {
        *error = tr("its time must be more than 0");
        return {};
    }
    std::vector<double> times;
    const int steps = 2000;
    for (int k = 0; k <= steps; ++k) times.push_back(duration * k / steps);
    std::map<long long, std::vector<double>> byStart;   // x0 in 1e-4 of the geometry's unit
    QString start;
    auto trajectory = [this, &spec, &start, &byStart, &times, &notes](double xi) -> const std::vector<double>& {
        double x0m = xi * a_unitScale;
        // Deal and Grove's dry oxide starts at 25 nm (their τ).
        if (!spec.wet && spec.model == QLatin1String("dealgrove") && !(spec.givenB && spec.givenBA) && x0m < 25e-9) {
            x0m = 25e-9;
            start = tr("Deal and Grove's initial 25 nm (their τ) taken");
        }
        const long long key = std::llround(x0m * 1e13);
        auto it = byStart.find(key);
        if (it == byStart.end()) {
            std::vector<double> x = growth(spec, x0m, times, &notes);
            for (double& v : x) v /= a_unitScale;
            it = byStart.emplace(key, x).first;
        }
        return it->second;
    };
    std::vector<const std::vector<double>*> path(std::size_t(nx), nullptr);
    double grownOpen = 0, xiOpen = 0;
    for (int i = 0; i < nx; ++i) {
        Column& c = cols[std::size_t(i)];
        if (!c.silicon || c.covered) continue;
        path[std::size_t(i)] = &trajectory(c.xi);
        c.delta = path[std::size_t(i)]->back() - c.xi;
        if (c.delta > grownOpen) {
            grownOpen = c.delta;
            xiOpen = c.xi;
        }
    }
    // A bird's beak: under a covered edge, the open neighbour's thickness
    // falling as erfc(d/L) with its distance in (an error function's taper,
    // as Guillemot et al.'s LOCOS model has it) - approximate: Stage 3's
    // viscous flow would shape it.
    std::vector<double> share(static_cast<std::size_t>(nx), 1.0);
    std::vector<int> from(static_cast<std::size_t>(nx), -1);
    for (int i = 0; i < nx; ++i) {
        if (path[std::size_t(i)]) {
            from[std::size_t(i)] = i;
            continue;
        }
        if (!cols[std::size_t(i)].silicon) continue;
        int best = -1;
        double dist = std::numeric_limits<double>::max();
        for (int k = 0; k < nx; ++k)
            if (path[std::size_t(k)] && std::abs(X[std::size_t(k)] - X[std::size_t(i)]) < dist) {
                dist = std::abs(X[std::size_t(k)] - X[std::size_t(i)]);
                best = k;
            }
        if (best < 0) continue;
        const double edge = dist - 0.5 * (d.xe[std::size_t(1)] - d.xe[std::size_t(0)]);
        const double L = spec.beak >= 0 ? spec.beak : cols[std::size_t(best)].delta;
        if (!(L > 0)) continue;
        const double f = std::erfc(std::max(edge, 0.0) / L);
        if (f < 1e-3) continue;
        from[std::size_t(i)] = best;
        share[std::size_t(i)] = f;
        cols[std::size_t(i)].delta = cols[std::size_t(best)].delta * f;
    }
    double grownMax = 0;
    for (const Column& c : cols) grownMax = std::max(grownMax, c.delta);
    if (!(grownMax > 0)) {
        warnings->append(tr("no silicon open to the oxidant: nothing grown"));
        return tr("nothing grown");
    }
    // The rows to hold the grown top.
    addRows(regionsTop(regions) / a_k + 0.56 * grownMax + a_dy);
    if (!a_tooLarge.isEmpty()) {
        *error = a_tooLarge;
        return {};
    }
    // The dopants meanwhile: diffused, the interface swept through each
    // column's silicon - a cell made oxide as the interface passes its
    // middle, its dopant shared as the interface's ratio m says (the
    // oxide takes C/m over its 1/0.44 of the silicon's thickness, the rest
    // left in the silicon below, or taken from it).
    std::vector<double> delta(static_cast<std::size_t>(nx), 0.0);   // grown so far
    std::vector<int> converted(static_cast<std::size_t>(nx), 0);      // cells made oxide so far, from each column's top silicon
    std::vector<int> firstSilicon(static_cast<std::size_t>(nx), -1);
    for (int i = 0; i < nx; ++i) {
        if (!(cols[std::size_t(i)].delta > 0)) continue;
        // (The cell under the surface: the first silicon cell at s.)
        int i0 = 0, j0 = 0;
        if (d.cellAt(X[std::size_t(i)], cols[std::size_t(i)].s - 1e-9, &i0, &j0)) firstSilicon[std::size_t(i)] = j0;
    }
    std::vector<bool> silicon, oxide;
    std::vector<QString> names;
    for (int m = 0; m < int(d.materials.size()); ++m) {
        silicon.push_back(info(m).silicon);
        oxide.push_back(info(m).oxide);
        names.push_back(d.materials.at(m));
    }
    AnnealSpec dopantSpec = spec.dopants;
    dopantSpec.thermal = spec.thermal;
    Annealer a(d, dopantSpec, silicon, oxide, names, warnings);
    if (const QString why = a.setUp(); !why.isEmpty()) {
        *error = why;
        return {};
    }
    auto at = [&](const std::vector<double>& x, double t) {
        const double u = std::clamp(t / duration * steps, 0.0, double(steps));
        const std::size_t k = std::min(std::size_t(u), std::size_t(steps - 1));
        return x[k] + (x[k + 1] - x[k]) * (u - double(k));
    };
    auto hook = [&nx, &from, &firstSilicon, &cols, &path, &at, &share, &delta, &converted, &d, &silicon, &a, &spec, &names, &oxideIndex](double t, double h) {
        for (int i = 0; i < nx; ++i) {
            if (from[std::size_t(i)] < 0 || firstSilicon[std::size_t(i)] < 0) continue;
            const Column& c = cols[std::size_t(i)];
            // (From the oxide it had: Deal and Grove's dry 25 nm grown at once.)
            const int src = from[std::size_t(i)];
            const std::vector<double>& x = *path[std::size_t(src)];
            const double grown = (at(x, t + h) - cols[std::size_t(src)].xi) * share[std::size_t(i)];
            delta[std::size_t(i)] = grown;
            const double interface = c.s - 0.44 * grown;
            // Each cell whose middle the interface passed: oxide now.
            while (true) {
                const int j = firstSilicon[std::size_t(i)] - converted[std::size_t(i)];
                if (j < 0) break;
                const std::size_t k = d.index(i, j);
                const double mid = 0.5 * (d.ye[std::size_t(j)] + d.ye[std::size_t(j + 1)]);
                if (interface > mid || d.material[k] < 0 || !silicon[std::size_t(d.material[k])]) break;
                const std::size_t below = j > 0 ? d.index(i, j - 1) : k;
                const bool hasBelow = j > 0 && d.material[below] >= 0 && silicon[std::size_t(d.material[below])];
                for (const Annealer::Sp& sp : a.species()) {
                    ProcessDopants::Field& f = d.fields[std::size_t(sp.field)];
                    double m = siliconOxideRatio(sp.name);
                    for (const Override& o : spec.dopants.overrides)
                        if (o.species.compare(sp.name, Qt::CaseInsensitive) == 0 && o.key.startsWith(QLatin1String("m:"))
                            && o.material.compare(names[std::size_t(d.material[k])], Qt::CaseInsensitive) == 0
                            && o.key.section(QLatin1Char(':'), 1).trimmed().compare(spec.material, Qt::CaseInsensitive) == 0)
                            m = o.value;
                    const double Cs = f.total[k];
                    // The oxide's share of this cell's silicon: C/(0.44 m) in
                    // place (C/m once grown to its 1/0.44).
                    double want = Cs / (0.44 * m);
                    double rest = Cs - want;   // to the silicon below (< 0: from it)
                    if (hasBelow) {
                        const double hk = d.ye[std::size_t(j + 1)] - d.ye[std::size_t(j)], hb = d.ye[std::size_t(j)] - d.ye[std::size_t(j - 1)];
                        double add = rest * hk / hb;
                        if (f.total[below] + add < 0) {
                            add = -f.total[below];
                            want = Cs + f.total[below] * hb / hk;
                        }
                        f.total[below] += add;
                        f.active[below] = std::max(0.0, f.active[below] + add);
                    } else {
                        want = Cs;
                    }
                    f.total[k] = want;
                    f.active[k] = want;
                }
                d.material[k] = oxideIndex;
                ++converted[std::size_t(i)];
            }
        }
    };
    Solver solver(*this);
    int steps2 = 0;
    if (!a.run(solver, hook, &steps2, error)) return {};
    // Then the oxide grown to its thickness: per column, what was silicon
    // above the interface stretched by 1/0.44, everything above the old
    // surface lifted by 0.56 of what grew.
    for (ProcessDopants::Field& f : d.fields) {
        for (int i = 0; i < nx; ++i) {
            const Column& c = cols[std::size_t(i)];
            if (!(c.delta > 0)) continue;
            const double grown = delta[std::size_t(i)];
            const double yi = c.s - 0.44 * grown;
            auto image = [&](double y) {
                if (y <= yi) return y;
                if (y <= c.s) return yi + (y - yi) / 0.44;
                return y + 0.56 * grown;
            };
            const int ny = d.ny();
            std::vector<double> amount(static_cast<std::size_t>(ny), 0.0), activeAmount(std::size_t(ny), 0.0);
            for (int j = 0; j < ny; ++j) {
                const std::size_t k = d.index(i, j);
                const double lo = d.ye[std::size_t(j)], hi = d.ye[std::size_t(j + 1)];
                const double a0 = image(lo), a1 = image(hi);
                const double tot = f.total[k] * (hi - lo), act = f.active.empty() ? 0 : f.active[k] * (hi - lo);
                if (!(tot > 0) && !(act > 0)) continue;
                // Spread over [a0, a1] by overlap with the rows.
                for (int q = std::max(0, j - 1); q < ny; ++q) {
                    const double o = std::min(a1, d.ye[std::size_t(q + 1)]) - std::max(a0, d.ye[std::size_t(q)]);
                    if (d.ye[std::size_t(q)] > a1) break;
                    if (o <= 0) continue;
                    amount[std::size_t(q)] += tot * o / (a1 - a0);
                    activeAmount[std::size_t(q)] += act * o / (a1 - a0);
                }
            }
            for (int j = 0; j < ny; ++j) {
                const std::size_t k = d.index(i, j);
                const double hgt = d.ye[std::size_t(j + 1)] - d.ye[std::size_t(j)];
                f.total[k] = amount[std::size_t(j)] / hgt;
                if (!f.active.empty()) f.active[k] = activeAmount[std::size_t(j)] / hgt;
            }
        }
    }
    // The shapes: silicon cut down to the interface, the new oxide between
    // it and the lifted surface, everything above the surface lifted.
    auto columnValue = [&](const std::vector<double>& v, double x) {
        if (x <= X.front()) return v.front();
        if (x >= X.back()) return v.back();
        const std::size_t k = std::min(std::size_t(std::upper_bound(X.begin(), X.end(), x) - X.begin()), X.size() - 1);
        const double t = (x - X[k - 1]) / (X[k] - X[k - 1]);
        return v[k - 1] + t * (v[k] - v[k - 1]);
    };
    std::vector<double> grownAt(static_cast<std::size_t>(nx)), surface(static_cast<std::size_t>(nx));
    for (int i = 0; i < nx; ++i) {
        grownAt[std::size_t(i)] = cols[std::size_t(i)].delta > 0 ? delta[std::size_t(i)] : 0;
        surface[std::size_t(i)] = std::isfinite(cols[std::size_t(i)].s) ? cols[std::size_t(i)].s : regionsTop(regions) / a_k;
    }
    const double ceiling = regionsTop(regions) / a_k + grownMax + 10 * a_dy;
    Paths band, grownOxide;
    // Each interval from a column's middle to the next's (the ends to the
    // window's edges): the band cut from the silicon, the oxide grown.
    for (int i = 0; i <= nx; ++i) {
        const double xa = i == 0 ? d.xe.front() : X[std::size_t(i - 1)];
        const double xb = i == nx ? d.xe.back() : X[std::size_t(i)];
        const int ia = std::max(0, i - 1), ib = std::min(nx - 1, i);
        const double ga = grownAt[std::size_t(ia)], gb = grownAt[std::size_t(ib)];
        if (!(ga > 0) && !(gb > 0)) continue;
        const double sa = surface[std::size_t(ia)], sb = surface[std::size_t(ib)];
        const double la = ga > 0 ? sa - 0.44 * ga : sa, lb = gb > 0 ? sb - 0.44 * gb : sb;
        const double ua = ga > 0 ? sa + 0.56 * ga : sa, ub = gb > 0 ? sb + 0.56 * gb : sb;
        auto P = [this](double x, double y) { return ClipperLib::IntPoint(cInt(std::llround(x * a_k)), cInt(std::llround(y * a_k))); };
        Path q1{P(xa, la), P(xb, lb), P(xb, ceiling), P(xa, ceiling)};
        Path q2{P(xa, la), P(xb, lb), P(xb, ub), P(xa, ua)};
        if (!ClipperLib::Orientation(q1)) std::reverse(q1.begin(), q1.end());
        if (!ClipperLib::Orientation(q2)) std::reverse(q2.begin(), q2.end());
        band.push_back(q1);
        if (ClipperLib::Area(q2) != 0) grownOxide.push_back(q2);
    }
    band = unite(band);
    grownOxide = unite(grownOxide);
    for (Region& r : regions) {
        const MaterialInfo& mi = info(materialIndex(r.material));
        if (mi.silicon) {
            // (Only the silicon whose surface oxidized: its band cut.)
            r.p = clip(ClipperLib::ctDifference, r.p, band);
            continue;
        }
        // Lifted above the old surface: each edge cut at the columns'
        // middles, each point above the surface there moved up.
        Paths moved;
        for (const Path& p : r.p) {
            Path out;
            for (std::size_t k = 0; k < p.size(); ++k) {
                const ClipperLib::IntPoint a = p[k], b = p[(k + 1) % p.size()];
                auto lift = [&](double x, double y) {
                    const double g = columnValue(grownAt, x), sf = columnValue(surface, x);
                    return y >= sf - 1.5 / a_k ? y + 0.56 * g : y;
                };
                const double ax = double(a.X) / a_k, ay = double(a.Y) / a_k, bx = double(b.X) / a_k, by = double(b.Y) / a_k;
                out.push_back({a.X, cInt(std::llround(lift(ax, ay) * a_k))});
                // The columns' middles between a and b.
                if (std::abs(bx - ax) > 1e-12) {
                    const double lo = std::min(ax, bx), hi = std::max(ax, bx);
                    std::vector<double> cuts;
                    for (double xc : X)
                        if (xc > lo && xc < hi) cuts.push_back(xc);
                    if (bx < ax) std::reverse(cuts.begin(), cuts.end());
                    for (double xc : cuts) {
                        const double yc = ay + (by - ay) * (xc - ax) / (bx - ax);
                        out.push_back({cInt(std::llround(xc * a_k)), cInt(std::llround(lift(xc, yc) * a_k))});
                    }
                }
            }
            moved.push_back(out);
        }
        r.p = unite(moved);
    }
    regions.push_back(Region{spec.name, spec.material, false, grownOxide});
    // The cells' materials as the shapes now say - their dopants kept where
    // one solid became another (the interface's cells, the lifted oxide's).
    std::vector<int> now;
    assign(regions, &now);
    for (std::size_t k = 0; k < now.size(); ++k)
        if (now[k] < 0 && d.material[k] >= 0)
            for (ProcessDopants::Field& f : d.fields) {
                f.total[k] = 0;
                if (!f.active.empty()) f.active[k] = 0;
            }
    d.material = now;
    for (const QString& n : notes) warnings->append(n);
    if (!start.isEmpty()) warnings->append(start);
    const double um = a_unitScale * 1e6;
    QString said = tr("%1, %2 at %3 °C (%4): %5 µm grown%6")
                       .arg(spec.wet ? tr("wet") : tr("dry"), durationText(duration), formatNumber(spec.thermal.highest() - 273.15, 4),
                            spec.givenB && spec.givenBA ? tr("its rates given")
                            : spec.wet || spec.model == QLatin1String("dealgrove")
                                ? tr("Deal and Grove's, (%1)").arg(spec.orientation.isEmpty() ? QStringLiteral("100") : spec.orientation)
                                : tr("Massoud's, (%1)").arg(spec.orientation.isEmpty() ? QStringLiteral("100") : spec.orientation),
                            formatNumber(grownOpen * um, 4),
                            xiOpen > 0 ? tr(" under %1 µm of oxide").arg(formatNumber(xiOpen * um, 3)) : tr(" on bare silicon"));
    said += tr(", %1 µm of silicon consumed").arg(formatNumber(0.44 * grownOpen * um, 4));
    bool beak = false;
    for (int i = 0; i < nx; ++i) beak = beak || (from[std::size_t(i)] >= 0 && from[std::size_t(i)] != i);
    if (beak) said += tr("; a bird's beak under the mask's edges (an analytic taper, approximate)");
    return said;
}

// ----------------------------------------------------- Quantum-well intermixing

QString Dopants::intermix(const IntermixSpec& spec, const std::vector<Region>& regions, QStringList* warnings, QString* error)
{
    ProcessDopants& d = *a_d;
    for (const Region& r : regions) materialIndex(r.material);
    const std::size_t n = d.material.size();
    // The sublattices' shares, made from each III-V's composition.
    std::map<int, bool> cationOf;   // element: on the cations' sublattice
    for (int m = 0; m < int(d.materials.size()); ++m) {
        const Sublattices& l = info(m).lattice;
        if (!l.ok || l.anion.empty()) continue;
        for (const auto& [Z, f] : l.cation) cationOf[Z] = true;
        for (const auto& [Z, f] : l.anion) cationOf[Z] = false;
    }
    if (cationOf.empty()) {
        *error = tr("no III-V here: its materials need compounds (InGaAs at x, InGaAsP...)");
        return {};
    }
    for (const auto& [Z, cation] : cationOf) {
        const QString name = QStringLiteral("x:") + element(Z)->symbol;
        if (d.field(name)) continue;
        std::vector<double> v(n, 0.0);
        for (std::size_t k = 0; k < n; ++k) {
            const int m = d.material[k];
            if (m < 0) continue;
            const Sublattices& l = info(m).lattice;
            if (!l.ok || l.anion.empty()) continue;
            for (const auto& [z, f] : cation ? l.cation : l.anion)
                if (z == Z) v[k] = f;
        }
        d.fields.push_back({name, v, {}});
    }
    const double duration = spec.thermal.duration();
    if (!(duration > 0)) {
        *error = tr("its time must be more than 0");
        return {};
    }
    Solver solver(*this);
    std::vector<char> open(std::size_t(d.nx()), spec.masked ? 0 : 1);
    if (spec.masked)
        for (int i = 0; i < d.nx(); ++i) {
            const double X = 0.5 * (d.xe[std::size_t(i)] + d.xe[std::size_t(i + 1)]) * a_k;
            for (const auto& [a, b] : spec.open) open[std::size_t(i)] = open[std::size_t(i)] || (X >= double(a) && X < double(b));
        }
    // Its steps: a fifth of the finest cell's time to start, growing.
    double hmin = std::numeric_limits<double>::max();
    for (double h : solver.hy) hmin = std::min(hmin, h);
    const double Tmax = spec.thermal.highest();
    const double Dmax = std::max(spec.D0III * std::exp(-spec.EIII / (KB * Tmax)), spec.D0V * std::exp(-spec.EV / (KB * Tmax))) * std::max(spec.enhance, 1.0);
    double dt = Dmax > 0 ? std::min(0.2 * hmin * hmin / Dmax, duration / 200) : duration / 200;
    double t = 0, LIII = 0, LV = 0;
    int steps = 0;
    while (t < duration * (1 - 1e-12)) {
        const double h = std::min(dt, duration - t);
        const double T = spec.thermal.at(t + h / 2);
        const double DIII = spec.D0III * std::exp(-spec.EIII / (KB * T)), DV = spec.D0V * std::exp(-spec.EV / (KB * T));
        LIII += DIII * h;
        LV += DV * h;
        for (const auto& [Z, cation] : cationOf) {
            ProcessDopants::Field* f = d.field(QStringLiteral("x:") + element(Z)->symbol);
            std::vector<double> D(n, 0.0);
            for (int j = 0; j < d.ny(); ++j)
                for (int i = 0; i < d.nx(); ++i) {
                    const std::size_t k = d.index(i, j);
                    const int m = d.material[k];
                    if (m < 0 || !info(m).lattice.ok || info(m).lattice.anion.empty()) continue;
                    D[k] = (cation ? DIII : DV) * (open[std::size_t(i)] ? spec.enhance : 1.0);
                }
            solver.faces(D, nullptr, 0, [](int, int, double* m, double* hh) {
                *m = 1;
                *hh = 0;
            }, [](std::size_t, double* g, double*, double*) { *g = 0; });
            solver.step(f->total, h, steps < 2);
        }
        t += h;
        ++steps;
        dt = std::min(h * 1.1, duration / 20);
    }
    Q_UNUSED(warnings);
    const double nm = 1e9;
    return tr("%1 at %2 °C: the group III atoms' diffusion length √(Dt) %3 nm, the group V's %4 nm%5, %6 steps")
        .arg(durationText(duration), formatNumber(Tmax - 273.15, 4), formatNumber(std::sqrt(LIII) * nm, 3), formatNumber(std::sqrt(LV) * nm, 3),
             spec.masked ? tr(" (where the mask is closed; ×%1 in its openings)").arg(formatNumber(spec.enhance, 3)) : QString())
        .arg(steps);
}

} // namespace process

} // namespace qucs_s::fem
