/*
 * rf25_farfield.cpp - RF 2.5D: the far field of a full-wave solve's currents
 * by reciprocity (rf25_farfield.h).
 */
#include "rf25_farfield.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <vector>

namespace qucs_s::rf25 {

namespace {

using Complex = std::complex<double>;
const Complex kJ(0, 1);
constexpr double kMu0 = 4e-7 * M_PI;

QString tr(const char* text)
{
    return QCoreApplication::translate("RF25FarField", text);
}

/// sin(x)/x, at nought one.
Complex sinc(Complex x)
{
    return std::abs(x) < 1e-6 ? Complex(1, 0) - x * x / 6.0 : std::sin(x) / x;
}

/*!
 * One of the stack's two lines for a plane wave from (theta, .) above: its
 * voltage and current at any height, the incident wave's tangential E at the
 * top \a incident (1 for TE, cos theta for TM), the ground a short.
 */
struct PlaneLine {
    struct Slab {
        double z0 = 0, z1 = 0;
        Complex kz, a, b;       ///< a = Z kz, b = kz / Z: finite at kz nought
        Complex epsZ;           ///< F/m, along z (E_z = k_r I / (w eps_z))
        Complex v, i;           ///< at its bottom, scaled
    };
    QList<Slab> slabs;
    bool ok = false;

    PlaneLine(const Layers& layers, double frequency, double theta, bool tm, Complex incident)
    {
        const double w = 2 * M_PI * frequency, k0 = w * std::sqrt(kMu0 * kEps0);
        const double kr = k0 * std::sin(theta);
        // (Im kz <= 0: the wave that goes, or dies, away from its source.)
        const auto kzOf = [](Complex k2) {
            Complex v = std::sqrt(k2);
            return v.imag() > 0 ? -v : v;
        };
        Complex v(0, 0), i(1, 0);
        for (const Layers::Slab& s : layers.slabs) {
            Slab out;
            out.z0 = s.z0;
            out.z1 = s.z1;
            const Complex eps = kEps0 * s.permittivityAt(frequency) - Complex(0, s.sigma / w);
            const Complex epsZ = s.uniaxial() ? kEps0 * s.permittivityZAt(frequency) - Complex(0, s.sigma / w) : eps;
            const Complex mu = kMu0 * s.mur * Complex(1, -s.mtand);
            if (tm) {
                out.kz = std::sqrt(eps / epsZ) * kzOf(w * w * mu * epsZ - kr * kr);
                out.a = out.kz * out.kz / (w * eps);
                out.b = w * eps;
            } else {
                out.kz = kzOf(w * w * mu * eps - kr * kr);
                out.a = w * mu;
                out.b = out.kz * out.kz / (w * mu);
            }
            out.epsZ = epsZ;
            out.v = v;
            out.i = i;
            const double h = s.z1 - s.z0;
            const Complex c = std::cos(out.kz * h), si = h * sinc(out.kz * h);
            const Complex nv = v * c - kJ * out.a * si * i, ni = i * c - kJ * out.b * si * v;
            v = nv;
            i = ni;
            slabs << out;
        }
        // The air over it: the incident wave and what the stack sends back.
        const double kz0 = k0 * std::cos(theta);
        const Complex z0 = tm ? Complex(kz0 / (w * kEps0), 0) : Complex(w * kMu0 / std::max(kz0, 1e-300), 0);
        if (std::abs(i) == 0) return;
        const Complex zin = -v / i;
        const Complex gamma = (zin - z0) / (zin + z0);
        const Complex top = incident * (1.0 + gamma);
        if (std::abs(v) == 0) return;
        const Complex scale = top / v;
        for (Slab& s : slabs) {
            s.v *= scale;
            s.i *= scale;
        }
        ok = true;
    }

    /// The slab \a z (m) is in, or -1.
    int slabAt(double z) const
    {
        for (int n = 0; n < slabs.size(); ++n)
            if (z >= slabs.at(n).z0 - 1e-15 && z <= slabs.at(n).z1 + 1e-15) return n;
        return -1;
    }
    /// V and I at \a z.
    void at(double z, Complex* v, Complex* i) const
    {
        const int n = slabAt(z);
        if (n < 0) {
            *v = *i = Complex(0, 0);
            return;
        }
        const Slab& s = slabs.at(n);
        const double d = z - s.z0;
        const Complex c = std::cos(s.kz * d), si = d * sinc(s.kz * d);
        *v = s.v * c - kJ * s.a * si * s.i;
        *i = s.i * c - kJ * s.b * si * s.v;
    }
    /// E_z at \a z: k_r I / (w eps_z).
    Complex ez(double z, double kr, double w) const
    {
        Complex v, i;
        at(z, &v, &i);
        const int n = slabAt(z);
        return n < 0 ? Complex(0, 0) : kr * i / (w * slabs.at(n).epsZ);
    }
};

/// Gauss-Legendre's \a n nodes and weights on [-1, 1].
void gaussLegendre(int n, std::vector<double>* x, std::vector<double>* wt)
{
    x->assign(std::size_t(n), 0);
    wt->assign(std::size_t(n), 0);
    for (int i = 0; i < n; ++i) {
        double z = std::cos(M_PI * (i + 0.75) / (n + 0.5)), dp = 0;
        for (int it = 0; it < 100; ++it) {
            double p0 = 1, p1 = z;
            for (int k = 2; k <= n; ++k) {
                const double p2 = ((2 * k - 1) * z * p1 - (k - 1) * p0) / k;
                p0 = p1;
                p1 = p2;
            }
            dp = n * (z * p1 - p0) / (z * z - 1);
            const double dz = p1 / dp;
            z -= dz;
            if (std::abs(dz) < 1e-16) break;
        }
        (*x)[std::size_t(i)] = z;
        (*wt)[std::size_t(i)] = 2 / ((1 - z * z) * dp * dp);
    }
}

} // namespace

double Radiation::directivity() const
{
    return power > 0 ? 4 * M_PI * most / power : 0;
}

FarFieldAt farFieldOf(const Layers& layers, double frequency, const QList<FarSource>& sources, double theta, double phi)
{
    FarFieldAt out;
    const double w = 2 * M_PI * frequency, k0 = w * std::sqrt(kMu0 * kEps0), kr = k0 * std::sin(theta);
    const PlaneLine te(layers, frequency, theta, false, 1.0), tm(layers, frequency, theta, true, std::cos(theta));
    if (!te.ok || !tm.ok) return out;
    const double cp = std::cos(phi), sp = std::sin(phi);
    std::vector<double> gx, gw;
    gaussLegendre(8, &gx, &gw);
    Complex eTheta(0, 0), ePhi(0, 0);
    for (const FarSource& s : sources) {
        const QPointF d = s.to - s.from;
        const double length = std::hypot(d.x(), d.y());
        if (length > 0) {
            // In the plane: the tangential field along it, its phase across it.
            const double tx = d.x() / length, ty = d.y() / length;
            const QPointF mid = 0.5 * (s.from + s.to);
            const double u = kr * (tx * cp + ty * sp);
            const Complex along = s.current * length * sinc(0.5 * u * length) * std::exp(kJ * kr * (mid.x() * cp + mid.y() * sp));
            Complex v, i;
            tm.at(s.z0, &v, &i);
            eTheta += along * v * (tx * cp + ty * sp);
            te.at(s.z0, &v, &i);
            ePhi += along * v * (-tx * sp + ty * cp);
        } else if (s.z1 > s.z0) {
            // Up z: E_z along it, the TM line's alone, slab by slab.
            Complex sum(0, 0);
            for (const PlaneLine::Slab& slab : tm.slabs) {
                const double lo = std::max(s.z0, slab.z0), hi = std::min(s.z1, slab.z1);
                if (!(hi > lo)) continue;
                for (std::size_t g = 0; g < gx.size(); ++g) {
                    const double z = 0.5 * (lo + hi) + 0.5 * (hi - lo) * gx[g];
                    sum += 0.5 * (hi - lo) * gw[g] * tm.ez(z, kr, w);
                }
            }
            eTheta += s.current * sum * std::exp(kJ * kr * (s.from.x() * cp + s.from.y() * sp));
        }
    }
    const Complex factor = -kJ * w * kMu0 / (4 * M_PI);
    out.theta = factor * eTheta;
    out.phi = factor * ePhi;
    return out;
}

Radiation radiationOf(const Layers& layers, double frequency, const QList<FarSource>& sources, int order)
{
    Radiation out;
    const double eta0 = std::sqrt(kMu0 / kEps0);
    std::vector<double> x, wt;
    gaussLegendre(order, &x, &wt);
    for (std::size_t a = 0; a < x.size(); ++a) {
        const double theta = M_PI / 4 * (1 + x[a]), wTheta = M_PI / 4 * wt[a];
        for (int quarter = 0; quarter < 4; ++quarter)
            for (std::size_t b = 0; b < x.size(); ++b) {
                const double phi = quarter * M_PI / 2 + M_PI / 4 * (1 + x[b]), wPhi = M_PI / 4 * wt[b];
                const FarFieldAt e = farFieldOf(layers, frequency, sources, theta, phi);
                const double u = (std::norm(e.theta) + std::norm(e.phi)) / (2 * eta0);
                out.power += u * std::sin(theta) * wTheta * wPhi;
                if (u > out.most) {
                    out.most = u;
                    out.thetaMost = theta;
                    out.phiMost = phi;
                }
            }
    }
    return out;
}

QList<FarSource> farSourcesOf(const FullWave& wave, int k, int port, QStringList* notes)
{
    QList<FarSource> out;
    if (k < 0 || k >= wave.currents.size() || port < 0 || port >= wave.currents.at(k).size()) return out;
    const QList<Complex>& currents = wave.currents.at(k).at(port);
    for (int a = 0; a < wave.geometry.size() && a < currents.size(); ++a) {
        const Filament& f = wave.geometry.at(a);
        FarSource s;
        s.from = f.from;
        s.to = f.to;
        s.z0 = std::min(f.z0, f.z1);
        s.z1 = std::max(f.z0, f.z1);
        s.current = f.z1 < f.z0 ? -currents.at(a) : currents.at(a);
        out << s;
    }
    for (const FullWave::PortPathLeg& leg : wave.portPaths) {
        if (leg.port != port) continue;
        FarSource s;
        s.from = s.to = leg.at;
        s.z0 = leg.z0;
        s.z1 = leg.z1;
        s.current = leg.current;
        out << s;
    }
    if (notes != nullptr && port < wave.namesReturns.size() && wave.namesReturns.at(port))
        *notes << tr("%1 returns through what it names: its own leg across to them is not in the far field.").arg(wave.portNames.value(port));
    return out;
}

AntennaFigures antennaFiguresOf(const FullWave& wave, const Stack& stack, int k, int port)
{
    AntennaFigures out;
    const Layers layers = layersOf(stack);
    if (!layers.ok()) {
        out.problems += layers.problems;
        return out;
    }
    if (!layers.groundBelow || layers.groundAbove) {
        out.problems << (layers.groundAbove ? tr("A stack under a lid radiates nothing out of it: it has no far field.")
                                            : tr("The far field is of the upper half-space over a ground plane, and the stack has "
                                                 "none under it."));
        return out;
    }
    if (k < 0 || k >= wave.frequencies.size() || port < 0 || port >= wave.portNames.size() || k >= wave.currents.size()) {
        out.problems << tr("No such port or frequency of the solve.");
        return out;
    }
    const QList<FarSource> sources = farSourcesOf(wave, k, port, &out.notes);
    const double f = wave.frequencies.at(k);
    out.accepted = 0.5 * wave.z.at(k).at(port).at(port).real();
    const Radiation r = radiationOf(layers, f, sources);
    out.radiated = r.power;
    out.directivity = r.directivity();
    out.gain = out.accepted > 0 ? 4 * M_PI * r.most / out.accepted : 0;
    out.efficiency = out.accepted > 0 ? r.power / out.accepted : 0;
    out.thetaMost = r.thetaMost;
    out.phiMost = r.phiMost;
    return out;
}

} // namespace qucs_s::rf25
