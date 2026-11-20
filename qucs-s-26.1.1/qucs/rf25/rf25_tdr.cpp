/*
 * rf25_tdr.cpp - RF 2.5D: time-domain responses of a sweep's S
 * (rf25_tdr.h).
 */
#include "rf25_tdr.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>

namespace qucs_s::rf25 {

namespace {

using Complex = std::complex<double>;

QString tr(const char* text)
{
    return QCoreApplication::translate("RF25Tdr", text);
}

/// Bessel's I0, by its series.
double besselI0(double x)
{
    double sum = 1, term = 1;
    for (int k = 1; k < 200; ++k) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
        if (term < 1e-17 * sum) break;
    }
    return sum;
}

/// The discrete Fourier transform of \a x in place, its exponent's sign
/// \a sign: X_k = sum_n x_n exp(sign j 2 pi k n / N). Radix two where N is a
/// power of two, else Bluestein's chirp through one - N log N either way, where
/// the plain sum took 3.5 s for 16,000 points and would have taken hours for a
/// million (a bug hunt's second hour).
void transform(std::vector<Complex>& x, int sign)
{
    const std::size_t n = x.size();
    if (n <= 1) return;
    if ((n & (n - 1)) == 0) {
        for (std::size_t i = 1, j = 0; i < n; ++i) {
            std::size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(x[i], x[j]);
        }
        for (std::size_t len = 2; len <= n; len <<= 1) {
            const double angle = sign * 2 * M_PI / double(len);
            for (std::size_t i = 0; i < n; i += len)
                for (std::size_t k = 0; k < len / 2; ++k) {
                    // (Each twiddle of its own, not a running product, whose
                    //  rounding grows with the length.)
                    const Complex w = std::polar(1.0, angle * double(k));
                    const Complex u = x[i + k], v = x[i + k + len / 2] * w;
                    x[i + k] = u + v;
                    x[i + k + len / 2] = u - v;
                }
        }
        return;
    }
    std::size_t big = 1;
    while (big < 2 * n - 1) big <<= 1;
    std::vector<Complex> chirp(n), a(big), b(big);
    for (std::size_t k = 0; k < n; ++k) {
        // (k^2 mod 2N, so that the angle stays small for large k.)
        const std::size_t kk = (k * k) % (2 * n);
        chirp[k] = std::polar(1.0, sign * M_PI * double(kk) / double(n));
    }
    for (std::size_t k = 0; k < n; ++k) a[k] = x[k] * chirp[k];
    b[0] = std::conj(chirp[0]);
    for (std::size_t k = 1; k < n; ++k) b[k] = b[big - k] = std::conj(chirp[k]);
    transform(a, -1);
    transform(b, -1);
    for (std::size_t k = 0; k < big; ++k) a[k] *= b[k];
    transform(a, 1);
    for (std::size_t k = 0; k < n; ++k) x[k] = a[k] * chirp[k] / double(big);
}

} // namespace

TimeDomain timeDomainOf(const QList<double>& frequencies, const QList<Complex>& s, bool reflection, double reference, double beta, int points)
{
    TimeDomain out;
    const int n = int(frequencies.size());
    if (n < 4 || s.size() != n) {
        out.problems << tr("A time-domain response wants four frequencies at least, each with its S.");
        return out;
    }
    for (int k = 1; k < n; ++k)
        if (!(frequencies.at(k) > frequencies.at(k - 1))) {
            out.problems << tr("The sweep's frequencies are not ascending.");
            return out;
        }
    if (!(frequencies.first() > 0)) {
        out.problems << tr("The sweep starts at nought: a low-pass response takes S at nought from the sweep's lowest points.");
        return out;
    }
    for (const Complex& v : s)
        if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) {
            out.problems << tr("The sweep's S is not finite at every frequency.");
            return out;
        }
    // The low-pass grid: k df up to the sweep's top. (Its points capped: a
    // transform of more would be gigabytes, and its time nanoseconds past
    // anything a layout's lengths give.)
    constexpr int kMostPoints = 1 << 20;
    if (points > kMostPoints) {
        out.problems << tr("A low-pass response of %1 points is past the %2 it takes.").arg(points).arg(kMostPoints);
        return out;
    }
    // (Kaiser's beta bounded: past some 710 Bessel's I0 overflows, and beta
    //  1000 made every point NaN, said as fine - the review of 5d395cad.)
    if (!(beta <= kMostBeta)) {
        out.problems << tr("Kaiser's window's beta of %1 is past the %2 it takes (some 6 to 12 is usual; nought, none).")
                            .arg(beta, 0, 'g', 4).arg(kMostBeta, 0, 'g', 3);
        return out;
    }
    const int m = points > 0 ? points : n;
    const double top = frequencies.last(), df = top / m;
    out.df = df;
    // S on it: between the sweep's points, along its magnitude and phase;
    // below its first point, from S at nought to it.
    // S at nought, real at direct current: where the phase turns little below
    // the sweep's first point, the real part of the lowest three's quadratic
    // through them, at nought. Where it turns more, that quadratic is wild - a
    // phase turning between the three sends it far off: S11 = 0.2 exp(-j w 1
    // ns) swept every 50 MHz from 1 GHz came to -3.51, from 5 GHz to -92.9,
    // and Z(t) to nought throughout, said as fine (a bug hunt's second hour) -, so the lowest point carried down to nought along the lowest
    // three's slope of phase, and its real part taken: |S(f0)| cos(phase at
    // nought). (Its magnitude with the sign of that cosine, as it first was,
    // took a pad's or a bond wire's S, rising as j w from nought, at +-|S(f0)|
    // with the sign a third-order term's: a 0.2 pF pad on a matched line read
    // as a step down to 37 Ohm from 5 GHz, a 1 nH wire as one up to 93 - the
    // review of 5d395cad. The real part is continuous through 90 degrees.)
    const double f0 = frequencies.at(0), f1 = frequencies.at(1), f2 = frequencies.at(2);
    const double r0 = s.at(0).real(), r1 = s.at(1).real(), r2 = s.at(2).real();
    const double quadratic = r0 * (f1 * f2) / ((f0 - f1) * (f0 - f2)) + r1 * (f0 * f2) / ((f1 - f0) * (f1 - f2)) + r2 * (f0 * f1) / ((f2 - f0) * (f2 - f1));
    // (The phase's slope from the lowest three, unwrapped between them; where
    //  S is nothing there, no slope.)
    double slope = 0;
    if (std::abs(s.at(0)) > 0 && std::abs(s.at(1)) > 0 && std::abs(s.at(2)) > 0)
        slope = (std::arg(s.at(1) / s.at(0)) + std::arg(s.at(2) / s.at(1))) / (f2 - f0);
    const double turnBelow = std::abs(slope) * f0;
    double atZero = quadratic, offReal = 0, turnUp = 0;
    const bool carried = turnBelow > 20 * M_PI / 180;
    if (carried) {
        const double phase = std::arg(s.at(0)) - slope * f0;
        const double c = std::cos(phase);
        atZero = std::abs(s.at(0)) * c;
        // (How far the carried phase lands from nought or 180 degrees - the
        //  level a guess past 45.)
        offReal = std::acos(std::min(1.0, std::abs(c)));
        // (And the phase's whole turn from nought up to the first point, the
        //  whole turns nearest the slope's: below it S is taken along that
        //  turn, as a delay's is, not a chord through the circle.)
        const double from = c < 0 ? M_PI : 0;
        turnUp = std::arg(s.at(0)) - from;
        turnUp += 2 * M_PI * std::round((slope * f0 - turnUp) / (2 * M_PI));
    }
    if (std::abs(atZero) > 1.05) {
        out.problems << tr("S at nought comes to %1 from the sweep's lowest points (%2 GHz on): past what a passive port "
                           "gives, so the step's level would rest on nothing - start the sweep lower, nearer its top over its "
                           "points' count (%3 MHz here).")
                            .arg(atZero, 0, 'g', 4).arg(f0 / 1e9, 0, 'g', 4).arg(top / n / 1e6, 0, 'g', 4);
        return out;
    }
    const auto sAt = [&](double f) -> Complex {
        if (f <= 0) return atZero;
        if (f <= f0) {
            if (!carried) return Complex(atZero, 0) + (s.at(0) - Complex(atZero, 0)) * (f / f0);
            const double u = f / f0;
            return std::polar(std::abs(atZero) * (1 - u) + std::abs(s.at(0)) * u, (atZero < 0 ? M_PI : 0.0) + turnUp * u);
        }
        const int hi = int(std::lower_bound(frequencies.begin(), frequencies.end(), f) - frequencies.begin());
        if (hi >= n) return s.last();
        const double u = (f - frequencies.at(hi - 1)) / (frequencies.at(hi) - frequencies.at(hi - 1));
        // (Its magnitude and its phase each along the step, not its real and
        //  imaginary parts: a straight chord between two points a phase
        //  apart dips to cos(dphi/2) of them - 3.4 % at 30 degrees -, a
        //  ripple at the sweep's own step that the transform shows as echoes
        //  at multiples of its inverse - the review of 8b91a9ab.)
        const Complex lo = s.at(hi - 1), up = s.at(hi);
        if (std::abs(lo) == 0 || std::abs(up) == 0) return lo + (up - lo) * u;
        const double turn = std::arg(up / lo);
        return std::polar(std::abs(lo) * (1 - u) + std::abs(up) * u, std::arg(lo) + turn * u);
    };
    // How far the phase turns between the sweep's points, where S is not
    // nothing: past some 20 degrees a step, the resampling between them is
    // the phase's guess. Not through a null - a point under half the largest
    // within three either side, S dipping there, which a coarse sweep of an
    // even |S| does not -, where the phase flips however finely S is swept:
    // a line's S11 passing through its nulls said its 178 degrees there
    // (P37 of the open items).
    double largest = 0, turned = 0;
    for (const Complex& v : s) largest = std::max(largest, std::abs(v));
    const auto nearNull = [&s, n](int k) {
        double around = 0;
        for (int j = std::max(0, k - 3); j <= std::min(n - 1, k + 3); ++j) around = std::max(around, std::abs(s.at(j)));
        return std::abs(s.at(k)) < 0.5 * around;
    };
    for (int k = 1; k < n; ++k)
        if (std::abs(s.at(k)) > 1e-3 * largest && std::abs(s.at(k - 1)) > 1e-3 * largest && !nearNull(k) && !nearNull(k - 1))
            turned = std::max(turned, std::abs(std::arg(s.at(k) / s.at(k - 1))));
    // Kaiser's window over the band, one at nought and its least at the top.
    const auto window = [&](int k) {
        if (!(beta > 0)) return 1.0;
        const double x = double(k) / m;
        return besselI0(beta * std::sqrt(std::max(0.0, 1 - x * x))) / besselI0(beta);
    };
    // The impulse response at t = i dt, dt = 1/(2 m df), over a period of
    // 1/df from -1/(2 df): the step its running sum from there, so that the
    // pulse at t nought - the port's own reflection, half of it before nought
    // - is whole in it. Kept from nought, to half the period.
    // (h_i = df Re[S_0 W_0 + 2 sum_k S_k W_k exp(j 2 pi k i / 2m)]: a
    //  transform of 2m, S_k W_k doubled at k from 1 to m and nothing above;
    //  i from -m, at 2m - m.)
    std::vector<Complex> grid(std::size_t(2 * m));
    for (int k = 0; k <= m; ++k) grid[std::size_t(k)] = sAt(k * df) * window(k) * (k == 0 ? 1.0 : 2.0);
    transform(grid, 1);
    const double dt = 1 / (2.0 * m * df);
    double running = 0, before = 0;
    for (int i = -m; i < m; ++i) {
        const double t = i * dt;
        const double h = df * grid[std::size_t((i + 2 * m) % (2 * m))].real();
        // (The step: the impulse's running sum, trapezoids.)
        running += i > -m ? 0.5 * (h + before) * dt : 0.0;
        before = h;
        if (i < 0) continue;
        out.t << t;
        out.impulse << h;
        out.step << running;
        if (reflection) {
            const double rho = std::clamp(running, -0.999999, 0.999999);
            out.impedance << reference * (1 + rho) / (1 - rho);
        }
    }
    if (turned > 20 * M_PI / 180)
        out.lines << tr("S's phase turns by up to %1 degrees between the sweep's points: between them it is taken along its "
                        "magnitude and phase, which is a guess past some 20 degrees - a finer sweep is surer.")
                         .arg(turned * 180 / M_PI, 0, 'f', 0);
    if (f0 > 3 * df)
        out.lines << tr("The sweep starts at %1 GHz, %2 of the grid's steps above nought: below it S is drawn from S at "
                        "nought to it, a guess on which the step's late level rests. A low-pass response wants the sweep to start at "
                        "its top over its points' count (%3 MHz here).")
                         .arg(f0 / 1e9, 0, 'g', 4).arg(f0 / df, 0, 'f', 0).arg(top / n / 1e6, 0, 'g', 4);
    {
        // (A reflection whose round trip is past the window's half folds back
        //  into it: the impulse still carrying something at its end.)
        double peak = 0, late = 0;
        for (int i = 0; i < out.impulse.size(); ++i) {
            peak = std::max(peak, std::abs(out.impulse.at(i)));
            if (i >= int(0.9 * out.impulse.size())) late = std::max(late, std::abs(out.impulse.at(i)));
        }
        if (peak > 0 && late > 0.01 * peak)
            out.lines << tr("The impulse still carries %1 % of its peak at the end of the window (%2 ns): a response from "
                            "further away folds back into it - a finer grid (more points) sees further.")
                             .arg(100 * late / peak, 0, 'g', 2).arg(m * dt * 1e9, 0, 'g', 4);
    }
    if (carried && offReal > M_PI / 4)
        out.lines << tr("S's phase, carried from the sweep's first point down to nought, lands %1 degrees from the real "
                        "axis, where S at nought is real: S at nought, and the step's late level with it, is a guess - a "
                        "reactance's S rising from nought does so - start the sweep lower.")
                         .arg(offReal * 180 / M_PI, 0, 'f', 0);
    out.lines << tr("A low-pass response: %1 points to %2 GHz, every %3 MHz; S at nought %4, %5; Kaiser's window of beta "
                    "%6; the time every %7 ps to %8 ns.")
                     .arg(m).arg(top / 1e9, 0, 'g', 4).arg(df / 1e6, 0, 'g', 4).arg(atZero, 0, 'g', 4)
                     .arg(carried ? tr("the real part of the lowest point carried down to nought along its phase (it turns "
                                       "%1 degrees below the first point)")
                                        .arg(turnBelow * 180 / M_PI, 0, 'f', 0)
                                  : tr("the real part of the lowest three's quadratic"))
                     .arg(beta, 0, 'g', 3).arg(dt * 1e12, 0, 'g', 4).arg(m * dt * 1e9, 0, 'g', 4);
    return out;
}

} // namespace qucs_s::rf25
