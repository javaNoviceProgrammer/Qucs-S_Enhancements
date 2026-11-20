/*
 * rf25_tdr.h - RF 2.5D: time-domain reflectometry and transmission from a
 * sweep's S - the low-pass step and impulse responses, and the impedance
 * profile Z(t) a TDR shows.
 *
 * The sweep is taken as a low-pass one: its frequencies k df from nought,
 * resampled onto that grid where they are not on it (magnitude and phase
 * between the sweep's points), S at nought extrapolated from its
 * lowest three (its real part, as at direct current it is real; where the
 * phase turns past 20 degrees below the first point, the real part of the
 * lowest point carried down to nought along its phase), windowed
 * (Kaiser's, its beta given) so that the end of the band rings little, and
 * transformed with its negative frequencies the conjugates of its positive:
 *
 *     h(t) = df [S_0 W_0 + 2 Re sum_k S_k W_k exp(j 2 pi k df t)],
 *
 * the impulse response at t = n dt, dt = 1/(2 N df), over a period 1/df from
 * -1/(2 df); the step response its running sum from there - the reflection a
 * unit step meets, rho(t), the pulse at nought whole in it -, kept from
 * nought to half the period; and Z(t) = Z0 (1 + rho)/(1 - rho) of S11's.
 */
#ifndef QUCS_RF25_TDR_H
#define QUCS_RF25_TDR_H

#include <QList>
#include <QStringList>

#include <complex>

namespace qucs_s::rf25 {

struct TimeDomain {
    QList<double> t;            ///< s, from nought
    QList<double> impulse;      ///< 1/s
    QList<double> step;         ///< the step response: rho(t) of a reflection, the transmitted step of a transmission
    QList<double> impedance;    ///< Ohm, Z(t) - of a reflection alone, Z0 its reference
    double df = 0;              ///< Hz, the low-pass grid's
    QStringList problems, lines;
    bool ok() const { return problems.isEmpty(); }
};

/// The largest Kaiser's beta a response takes.
constexpr double kMostBeta = 100;

/// The time-domain responses of \a s at \a frequencies (Hz, ascending, above
/// nought), \a reflection: S11's, with Z(t) against \a reference (Ohm); else a
/// transmission's. Kaiser's window of \a beta (0, none; at most kMostBeta); \a points on the
/// low-pass grid (0: the sweep's own count; at most 2^20), transformed by an
/// FFT.
TimeDomain timeDomainOf(const QList<double>& frequencies, const QList<std::complex<double>>& s, bool reflection, double reference = 50,
                        double beta = 6, int points = 0);

} // namespace qucs_s::rf25

#endif
