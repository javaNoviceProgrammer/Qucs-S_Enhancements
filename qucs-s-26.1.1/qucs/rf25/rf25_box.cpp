/*
 * rf25_box.cpp - RF 2.5D: a box's kernels from its guide's modes and the
 * rectangle's Green's function (rf25_box.h).
 */
#include "rf25_box.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <vector>

namespace qucs_s::rf25 {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("RF25Box", text);
}

constexpr double kMu0 = 4e-7 * M_PI;
const Complex kJ(0, 1);

/// \a layers with every loss times \a t: its loss tangents, conductivities
/// and magnetic loss tangents - nought, the lossless guide.
Layers withLoss(const Layers& layers, double t)
{
    Layers out = layers;
    for (Layers::Slab& s : out.slabs) {
        s.tand *= t;
        s.sigma *= t;
        s.mtand *= t;
    }
    return out;
}

/// What a pole is looked for in: the kernel itself, or one of the two
/// lines whose poles it has - the TE line's voltage (the vector kernel's, j w
/// times it) or the TM line's (k_r^2 K_phi/(j w) + j w G_A). The scalar
/// kernel has both lines' poles, and where the fill is nearly even a TE and a
/// TM pole fall so close that their residues nearly cancel and f has a zero
/// between them: 1/f crosses nought and jumps through infinity in no more than
/// a sample, and neither is seen (the review of 3c834794). Each line alone has
/// no such pair.
enum class Line { Kernel, TE, TM };

/// The function \a line of \a layers' kernel at \a kr.
struct Spectral {
    WaveGreen green;
    BoxKernel kernel;
    Line line = Line::Kernel;
    Complex operator()(Complex kr) const
    {
        const Complex jw(0, 2 * M_PI * green.frequency());
        switch (line) {
        case Line::TE: return green.vectorSpectral(kr);
        // (V_e over j: real where the lossless kernels are, as G_A is.)
        case Line::TM: return (kr * kr * green.scalarSpectral(kr) / jw + jw * green.vectorSpectral(kr)) / kJ;
        case Line::Kernel: break;
        }
        return kernel == BoxKernel::Scalar ? green.scalarSpectral(kr) : green.vectorSpectral(kr);
    }
};

/// A root of 1/f near \a start by the secant method, or NaN.
Complex rootNear(const Spectral& f, Complex start)
{
    const auto h = [&f](Complex z) { return 1.0 / f(z); };
    Complex z0 = start, z1 = start * (1.0 + 1e-6) + Complex(0, -1e-9 * std::abs(start));
    Complex h0 = h(z0), h1 = h(z1);
    for (int i = 0; i < 80; ++i) {
        if (h1 == h0) break;
        const Complex z2 = z1 - h1 * (z1 - z0) / (h1 - h0);
        if (!std::isfinite(z2.real()) || !std::isfinite(z2.imag())) break;
        z0 = z1;
        h0 = h1;
        z1 = z2;
        h1 = h(z1);
        if (std::abs(z1 - z0) <= 1e-12 * std::abs(z1)) return z1;
    }
    return Complex(std::nan(""), 0);
}

/// The lossless guide's poles of \a f along kr = s t, t from nought to
/// \a span (s 1 the real axis, -j the imaginary one), where f is real:
/// where 1/f passes through nought between samples - and, where it dips
/// toward nought between three with no sign change, two poles closer than a
/// sample (a TE and a TM mode of a nearly even fill), that stretch sampled
/// finely and searched again (the review of 3c834794).
QList<Complex> polesAlong(const Spectral& f, Complex s, double span)
{
    QList<Complex> out;
    const int samples = 4000;
    const auto h = [&f, s](double t) { return (1.0 / f(s * t)).real(); };
    // (Each sign change between t0 and t1 bisected; a pole, where 1/f passes
    //  through nought - not a zero of f, where it jumps through infinity -
    //  told by f's size there.)
    const auto between = [&](double t0, double h0, double t1, double h1) {
        if (!std::isfinite(h0) || !std::isfinite(h1) || (h0 > 0) == (h1 > 0)) return;
        double lo = t0, hi = t1, hlo = h0;
        for (int k = 0; k < 80; ++k) {
            const double mid = 0.5 * (lo + hi), hm = h(mid);
            if ((hm > 0) == (hlo > 0)) {
                lo = mid;
                hlo = hm;
            } else {
                hi = mid;
            }
        }
        const double mid = 0.5 * (lo + hi);
        const double there = std::abs(f(s * mid)), beside = std::max(std::abs(f(s * t0)), std::abs(f(s * t1)));
        if (there > 1e3 * beside) out << s * mid;
    };
    std::vector<double> t(samples + 1), v(samples + 1);
    for (int i = 0; i <= samples; ++i) {
        t[i] = span * (i + 0.5) / (samples + 1);
        v[i] = h(t[i]);
    }
    for (int i = 0; i < samples; ++i) between(t[i], v[i], t[i + 1], v[i + 1]);
    for (int i = 1; i < samples; ++i) {
        const bool dips = std::abs(v[i]) < std::abs(v[i - 1]) && std::abs(v[i]) < std::abs(v[i + 1]);
        const bool crossed = (v[i - 1] > 0) != (v[i] > 0) || (v[i] > 0) != (v[i + 1] > 0);
        if (!dips || crossed) continue;
        const int fine = 400;
        double t0 = t[i - 1], h0 = v[i - 1];
        for (int k = 1; k <= fine; ++k) {
            const double t1 = t[i - 1] + (t[i + 1] - t[i - 1]) * k / fine, h1 = h(t1);
            between(t0, h0, t1, h1);
            t0 = t1;
            h0 = h1;
        }
    }
    std::sort(out.begin(), out.end(), [](Complex p, Complex q) { return std::abs(p) < std::abs(q); });
    out.erase(std::unique(out.begin(), out.end(), [](Complex p, Complex q) { return std::abs(p - q) <= 1e-9 * std::abs(p); }), out.end());
    return out;
}

} // namespace

QList<GuideMode> guideModes(const Layers& layers, double zField, double zSource, double frequency, BoxKernel kernel,
                            double slowerThan, QStringList* problems)
{
    QList<GuideMode> out;
    if (!layers.groundBelow || !layers.groundAbove || !(frequency > 0)) return out;
    const double w = 2 * M_PI * frequency;
    double kMax = 0;
    for (const Layers::Slab& s : layers.slabs)
        kMax = std::max(kMax, w * std::sqrt(kMu0 * kEps0 * std::max(s.epsr, s.epsz) * std::max(1.0, s.mur)));
    bool lossy = false;
    for (const Layers::Slab& s : layers.slabs) lossy = lossy || s.tand > 0 || s.sigma > 0 || s.mtand > 0;
    const Spectral kernelOf{WaveGreen(layers, zField, zSource, frequency, 1e-3), kernel, Line::Kernel};
    const QList<Line> lines = kernel == BoxKernel::Scalar ? QList<Line>{Line::TM, Line::TE} : QList<Line>{Line::TE};
    for (const Line line : lines) {
        const Spectral lossless{WaveGreen(withLoss(layers, 0), zField, zSource, frequency, 1e-3), kernel, line};
        QList<Complex> starts = polesAlong(lossless, Complex(1, 0), 1.2 * kMax);
        if (slowerThan > 0) starts += polesAlong(lossless, Complex(0, -1), slowerThan);
        for (Complex z : std::as_const(starts)) {
            // Followed as the losses are turned up, a tenth at a time, in its
            // own line's function.
            bool lost = false;
            if (lossy)
                for (int step = 1; step <= 10 && !lost; ++step) {
                    const Spectral f{WaveGreen(withLoss(layers, step / 10.0), zField, zSource, frequency, 1e-3), kernel, line};
                    const Complex next = rootNear(f, z);
                    if (!std::isfinite(next.real())) lost = true;
                    else z = next;
                }
            if (lost) {
                if (problems != nullptr)
                    *problems << tr("A mode of the guide between the ground and the lid near kr %1 /m was lost as its losses were "
                                    "turned up.").arg(std::abs(z), 0, 'g', 4);
                continue;
            }
            if (!lossy) {
                const Spectral f{WaveGreen(layers, zField, zSource, frequency, 1e-3), kernel, line};
                const Complex polished = rootNear(f, z);
                if (std::isfinite(polished.real())) z = Complex(polished.real(), std::min(0.0, polished.imag()));
            }
            // In the lower half-plane, as the Sommerfeld path passes above it.
            if (z.imag() > 0) z = -z;
            if (std::abs(z.imag()) >= slowerThan && std::abs(z.real()) < 1e-9 * std::abs(z)) continue;   // (fast enough: the images')
            // The kernel's residue there, f's two sides about the pole: its
            // own, whatever its neighbour's (a zero of f beside it leaves 1/f's
            // slope no measure of it).
            // (A TE and a TM pole of an even fill are one pole of the scalar
            //  kernel, its residue both lines': once.)
            bool known = false;
            for (const GuideMode& m : std::as_const(out)) known = known || std::abs(m.kr - z) <= 1e-9 * std::abs(z);
            if (known) continue;
            const double delta = 1e-7 * std::abs(z);
            const Complex residue = 0.5 * delta * (kernelOf(z + delta) - kernelOf(z - delta));
            out << GuideMode{z, residue};
        }
    }
    return out;
}

Complex modeKernel(const GuideMode& mode, double rho)
{
    return -0.5 * kJ * mode.residue * mode.kr * hankelH0(mode.kr * rho, 2);
}

Complex rectangleGreen(Complex k, const QRectF& box, const QPointF& field, const QPointF& source, bool neumannX, bool neumannY)
{
    // The series over the modes across the way the two are apart least:
    // each term then dies as exp(-pi m |apart|/side), where across the way
    // they are apart most it fell as 1/m^3 alone - two cells of a strip along
    // x, apart in x alone, were 2000 terms each (the review of a02647f6). The
    // same function, the box turned over its diagonal.
    if (std::abs(field.x() - source.x()) > std::abs(field.y() - source.y()))
        return rectangleGreen(k, QRectF(box.top(), box.left(), box.height(), box.width()), QPointF(field.y(), field.x()),
                              QPointF(source.y(), source.x()), neumannY, neumannX);
    const double a = box.width(), b = box.height();
    const double x = field.x() - box.left(), xs = source.x() - box.left();
    const double y = field.y() - box.top(), ys = source.y() - box.top();
    const double lo = std::min(y, ys), hi = std::max(y, ys), dy = hi - lo, sy = hi + lo;
    const double ySign = neumannY ? 1.0 : -1.0;
    const double rho = std::hypot(x - xs, y - ys);
    // The strip's own (walls at x alone) of a source t away in y, x's modes
    // m >= 1 summed: the log sums, each factor (1 - q)^2 + 4 q sin^2(theta/2)
    // so that nothing cancels near the source.
    const auto factor = [a](double t, double theta) {
        const double q = std::exp(-M_PI * t / a), s = std::sin(0.5 * theta);
        return (1 - q) * (1 - q) + 4 * q * s * s;
    };
    const double thetaMinus = M_PI * (x - xs) / a, thetaPlus = M_PI * (x + xs) / a;
    const auto strip = [&](double t, bool direct) -> Complex {
        const double minus = factor(t, thetaMinus), plus = factor(t, thetaPlus);
        // (At the source itself, the direct term's minus factor is nought:
        //  left out here, and its limit with the free function's, below.)
        const double lnMinus = direct ? 0.0 : std::log(minus);
        return neumannX ? -(lnMinus + std::log(plus)) / (4 * M_PI) : (std::log(plus) - lnMinus) / (4 * M_PI);
    };
    Complex sum = strip(dy, true) + strip(2 * b - dy, false) + ySign * (strip(2 * b - sy, false) + strip(sy, false));
    // The direct term's singular log with the free function: its limit at
    // the source, or the two as they are.
    const double regularBelow = 1e-7 * std::min(a, b);
    const Complex freeG = rho > regularBelow ? -0.25 * kJ * hankelH0(k * rho, 2) : Complex(0, 0);
    if (rho > regularBelow) {
        sum += -std::log(factor(dy, thetaMinus)) / (4 * M_PI) - freeG;
    } else {
        constexpr double gamma = 0.57721566490153286;
        sum += -std::log(M_PI / a) / (2 * M_PI) + 0.25 * kJ + (std::log(k / 2.0) + gamma) / (2 * M_PI);
    }
    // y's 1D Green's function, each x mode's, less the strip's terms.
    const auto oneD = [&](Complex g) {
        const Complex e = std::exp(-2.0 * g * b);
        return (std::exp(-g * dy) + std::exp(-g * (2 * b - dy)) + ySign * (std::exp(-g * (2 * b - sy)) + std::exp(-g * sy))) / (2.0 * g * (1.0 - e));
    };
    const auto gammaOf = [&k](double alpha) {
        Complex g = std::sqrt(Complex(alpha * alpha, 0) - k * k);
        return g.real() < 0 ? -g : g;
    };
    if (neumannX) sum += oneD(gammaOf(0)) / a;
    const int most = int(std::clamp(100.0 * std::abs(k) * a / M_PI, 2000.0, 20000.0));
    int small = 0;
    for (int m = 1; m <= most; ++m) {
        const double alpha = m * M_PI / a;
        const double modes = neumannX ? std::cos(alpha * x) * std::cos(alpha * xs) : std::sin(alpha * x) * std::sin(alpha * xs);
        const double staticPart = (std::exp(-alpha * dy) + std::exp(-alpha * (2 * b - dy))
                                   + ySign * (std::exp(-alpha * (2 * b - sy)) + std::exp(-alpha * sy))) / (2 * alpha);
        const Complex term = 2.0 / a * modes * (oneD(gammaOf(alpha)) - staticPart);
        sum += term;
        small = std::abs(term) < 1e-15 * std::max(1.0, std::abs(sum)) ? small + 1 : 0;
        if (small > 30 && alpha > 4 * std::abs(k)) break;
    }
    return sum;
}

QList<BoxResonance> boxResonances(const Layers& layers, double top)
{
    QList<BoxResonance> out;
    if (!layers.walls || !layers.groundBelow || !layers.groundAbove || !(top > 0)) return out;
    const double a = layers.box.width(), b = layers.box.height();
    if (!(a > 0) || !(b > 0)) return out;
    const double c0 = 1 / std::sqrt(kMu0 * kEps0);
    double most = 1, rate = 0;
    for (const Layers::Slab& s : layers.slabs) {
        const double n = std::sqrt(std::max(s.epsr, s.epsz) * std::max(1.0, s.mur));
        most = std::max(most, n);
        rate += 2 * M_PI / c0 * n * (s.z1 - s.z0);
    }
    // The chain's top right entry, lossless (j times a real), at f and kr:
    // nought where the guide resonates across, a short at either end.
    const auto across = [&layers](double f, double kr, bool tm) {
        const double w = 2 * M_PI * f;
        Complex A(1, 0), B(0, 0), C(0, 0), D(1, 0);
        for (const Layers::Slab& s : layers.slabs) {
            const double et = s.epsr, ez = s.epsz > 0 ? s.epsz : s.epsr, mu = kMu0 * s.mur;
            const Complex kz = tm ? std::sqrt(et / ez) * std::sqrt(Complex(w * w * mu * kEps0 * ez - kr * kr, 0))
                                  : std::sqrt(Complex(w * w * mu * kEps0 * et - kr * kr, 0));
            const Complex z = tm ? kz / (w * kEps0 * et) : w * mu / kz;
            const double h = s.z1 - s.z0;
            const Complex co = std::cos(kz * h), si = std::abs(kz) > 0 ? std::sin(kz * h) : Complex(0, 0);
            const Complex m12 = std::abs(kz) > 0 ? kJ * z * si : kJ * (tm ? h / (w * kEps0 * et) : w * mu * h);
            const Complex m21 = std::abs(kz) > 0 ? kJ * si / z : Complex(0, 0);
            const Complex nA = co * A + m12 * C, nB = co * B + m12 * D, nC = m21 * A + co * C, nD = m21 * B + co * D;
            A = nA;
            B = nB;
            C = nC;
            D = nD;
        }
        return B.imag();
    };
    const double step = 0.05 / rate;
    const double kTop = 2 * M_PI * top * most / c0;
    for (int m = 0; m * M_PI / a <= kTop; ++m)
        for (int n = 0; std::hypot(m * M_PI / a, n * M_PI / b) <= kTop; ++n)
            for (const bool tm : {true, false}) {
                if (tm && (m == 0 || n == 0)) continue;   // (E_z sin-sin)
                if (!tm && m == 0 && n == 0) continue;    // (H_z cos-cos, not both nought)
                const double kr = std::hypot(m * M_PI / a, n * M_PI / b);
                const double from = std::max(step, kr * c0 / (2 * M_PI * most) * 0.999);
                if (from >= top) continue;
                // (At least a few steps between its cutoff and the top, the
                //  last at the top itself: a mode whose cutoff is just under
                //  it was stepped over.)
                const double each = std::min(step, (top - from) / 8);
                double f0 = from, v0 = across(f0, kr, tm);
                for (double f1 = std::min(top, from + each);; f1 = std::min(top, f1 + each)) {
                    const double v1 = across(f1, kr, tm);
                    if (std::isfinite(v0) && std::isfinite(v1) && (v0 > 0) != (v1 > 0)) {
                        double lo = f0, hi = f1, vlo = v0;
                        for (int i = 0; i < 80; ++i) {
                            const double mid = 0.5 * (lo + hi), vm = across(mid, kr, tm);
                            if ((vm > 0) == (vlo > 0)) {
                                lo = mid;
                                vlo = vm;
                            } else {
                                hi = mid;
                            }
                        }
                        out << BoxResonance{0.5 * (lo + hi), m, n, tm};
                    }
                    f0 = f1;
                    v0 = v1;
                    if (f1 >= top) break;
                }
            }
    std::sort(out.begin(), out.end(), [](const BoxResonance& p, const BoxResonance& q) { return p.frequency < q.frequency; });
    return out;
}

} // namespace qucs_s::rf25
