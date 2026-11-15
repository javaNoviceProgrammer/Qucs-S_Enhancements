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
    // The low-pass grid: k df up to the sweep's top.
    const int m = points > 0 ? points : n;
    const double top = frequencies.last(), df = top / m;
    out.df = df;
    // S on it: between the sweep's points, real and imaginary parts each;
    // below its first point, from S at nought to it.
    // (S at nought: the real part of the lowest three's quadratic through
    //  them, at nought - real at direct current.)
    const double f0 = frequencies.at(0), f1 = frequencies.at(1), f2 = frequencies.at(2);
    const double r0 = s.at(0).real(), r1 = s.at(1).real(), r2 = s.at(2).real();
    const double atZero = r0 * (f1 * f2) / ((f0 - f1) * (f0 - f2)) + r1 * (f0 * f2) / ((f1 - f0) * (f1 - f2)) + r2 * (f0 * f1) / ((f2 - f0) * (f2 - f1));
    const auto sAt = [&](double f) -> Complex {
        if (f <= 0) return atZero;
        if (f <= f0) return Complex(atZero, 0) + (s.at(0) - Complex(atZero, 0)) * (f / f0);
        const int hi = int(std::lower_bound(frequencies.begin(), frequencies.end(), f) - frequencies.begin());
        if (hi >= n) return s.last();
        const double u = (f - frequencies.at(hi - 1)) / (frequencies.at(hi) - frequencies.at(hi - 1));
        return s.at(hi - 1) + (s.at(hi) - s.at(hi - 1)) * u;
    };
    // Kaiser's window over the band, one at nought and its least at the top.
    const auto window = [&](int k) {
        if (!(beta > 0)) return 1.0;
        const double x = double(k) / m;
        return besselI0(beta * std::sqrt(std::max(0.0, 1 - x * x))) / besselI0(beta);
    };
    std::vector<Complex> grid(std::size_t(m + 1));
    for (int k = 0; k <= m; ++k) grid[std::size_t(k)] = sAt(k * df) * window(k);
    // The impulse response at t = i dt, dt = 1/(2 m df), over a period of
    // 1/df from -1/(2 df): the step its running sum from there, so that the
    // pulse at t nought - the port's own reflection, half of it before nought
    // - is whole in it. Kept from nought, to half the period.
    const double dt = 1 / (2.0 * m * df);
    double running = 0, before = 0;
    for (int i = -m; i < m; ++i) {
        const double t = i * dt;
        Complex sum = grid[0];
        for (int k = 1; k <= m; ++k) sum += 2.0 * grid[std::size_t(k)] * std::exp(Complex(0, 2 * M_PI * k * df * t));
        const double h = df * sum.real();
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
    out.lines << tr("A low-pass response: %1 points to %2 GHz, every %3 MHz; S at nought %4, the real part of the lowest "
                    "three's quadratic; Kaiser's window of beta %5; the time every %6 ps to %7 ns.")
                     .arg(m).arg(top / 1e9, 0, 'g', 4).arg(df / 1e6, 0, 'g', 4).arg(atZero, 0, 'g', 4).arg(beta, 0, 'g', 3)
                     .arg(dt * 1e12, 0, 'g', 4).arg(m * dt * 1e9, 0, 'g', 4);
    return out;
}

} // namespace qucs_s::rf25
