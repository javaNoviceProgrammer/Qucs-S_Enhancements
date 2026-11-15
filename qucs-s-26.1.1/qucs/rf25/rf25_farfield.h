/*
 * rf25_farfield.h - RF 2.5D: the far field of a full-wave solve's currents,
 * and what comes of it - patterns, radiated power, directivity, gain and
 * radiation efficiency.
 *
 * By reciprocity: the field a current puts at a far point (r, theta, phi),
 * polarized along p, is
 *
 *     E_p = -j w mu0 exp(-j k0 r)/(4 pi r) Int J . E_p^pw dV,
 *
 * E_p^pw the field a plane wave from that point puts in the stack - its
 * incident part p at the origin, unit -, the stack's own reflections and all.
 * That field is the stack's TE line's (p = phi: the tangential E alone) and
 * TM line's (p = theta: the tangential E cos theta of it, and E_z = k_r I /
 * (w eps_z) for a vertical current), at k_r = k0 sin theta, the ground a short
 * under it; each layer its own complex permittivity, permeability and, along
 * z, uniaxial permittivity. So each run of current, in the plane or up a via
 * or a port's path, adds its current times that field along it.
 *
 * The far field is of the upper half-space, over a ground; a stack with no
 * ground, or closed by a lid, has none of its own here.
 */
#ifndef QUCS_RF25_FARFIELD_H
#define QUCS_RF25_FARFIELD_H

#include "rf25_fullwave.h"
#include "rf25_green.h"

#include <QList>
#include <QPointF>
#include <QStringList>

#include <complex>

namespace qucs_s::rf25 {

/// A straight current: in the plane at z (from != to, z0 == z1) or up z at a
/// point (from == to, z0 < z1), m - the stack's own heights; its current,
/// A, from \a from to \a to, or up.
struct FarSource {
    QPointF from, to;
    double z0 = 0, z1 = 0;
    std::complex<double> current;
};

/// The far field's E_theta and E_phi at (\a theta, \a phi) (rad) of \a sources
/// over \a layers at \a frequency, V: r times the field, its exp(-j k0 r) out.
struct FarFieldAt {
    std::complex<double> theta, phi;
};
FarFieldAt farFieldOf(const Layers& layers, double frequency, const QList<FarSource>& sources, double theta, double phi);

/// The radiated power, W, over the upper half-space, of \a sources (Gauss's
/// rule in theta and phi, \a order points each way per its own quarter of a
/// turn); its intensity's largest, W/sr, and where.
struct Radiation {
    double power = 0;
    double most = 0;             ///< W/sr
    double thetaMost = 0, phiMost = 0;
    double directivity() const;  ///< 4 pi most / power
};
Radiation radiationOf(const Layers& layers, double frequency, const QList<FarSource>& sources, int order = 24);

/// The sources of \a wave's port \a port driven by an ampere at frequency
/// index \a k (FullWave::currents): its runs, in the plane and up its vias,
/// and the ports' own paths up from the ground. \a notes: what is not in
/// them (a coplanar port's legs across its gaps).
QList<FarSource> farSourcesOf(const FullWave& wave, int k, int port, QStringList* notes = nullptr);

/// A port's figures at frequency index \a k: the power it accepts driven by
/// an ampere (half Re Z_kk), the radiated power, directivity, gain and the
/// radiation efficiency, radiated over accepted.
struct AntennaFigures {
    double accepted = 0, radiated = 0;
    double directivity = 0, gain = 0, efficiency = 0;
    double thetaMost = 0, phiMost = 0;
    QStringList problems, notes;
    bool ok() const { return problems.isEmpty(); }
};
AntennaFigures antennaFiguresOf(const FullWave& wave, const Stack& stack, int k, int port);

} // namespace qucs_s::rf25

#endif
