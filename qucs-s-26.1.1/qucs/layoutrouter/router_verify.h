/*
 * router_verify.h - the routes checked on the shapes drawn, not on the
 *                   lattice: each net one piece touching its terminals,
 *                   its new metal (what it draws outside its own) kept
 *                   from other nets, other metal and pads, obstacles and
 *                   keep-outs; via cuts apart; every net not routed too.
 *                   It complements a foundry's DRC, and does not replace
 *                   it (no widths, notches, densities, antennas)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_ROUTER_VERIFY_H
#define QUCS_ROUTER_VERIFY_H

#include "router.h"

namespace qucs_s::router {

struct Finding {
    /// unroutable, open, short, spacing, pad, metal, obstacle, keep_out, via_spacing
    QString kind;
    QString net;
    QString other;              ///< the other net, or what is in the way
    int metal = -1;             ///< Technology::metals' index (-1: none)
    Pt at;                      ///< where to look
    Box box;                    ///< what to show round it
    double measured = 0;        ///< µm
    double required = 0;        ///< µm
    QString text;               ///< the whole finding in words
};

struct Verification {
    QVector<Finding> findings;
    /// Things worth knowing that are not wrong: a net let into a keep-out.
    QStringList notes;
    int netsChecked = 0;
    double seconds = 0;
    bool clean() const { return findings.isEmpty(); }
};

/// Checks \a result's routes against \a problem's shapes. A net Failed is
/// a finding of its own (kind unroutable), so clean() means every net
/// asked for is routed and correct.
Verification verify(const Problem& problem, const Result& result);

/// The new metal of each net, on each metal: its runs and landings less
/// its own metal (exposed for the tests).
QVector<QVector<Poly>> newMetal(const Problem& problem, const NetResult& route, int net);

} // namespace qucs_s::router

#endif // QUCS_ROUTER_VERIFY_H
