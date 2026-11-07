/*
 * router_assign.cpp - the least-distance pairing of starts with pads
 *                     (router.h's assignPads): the Hungarian algorithm on
 *                     Manhattan distances, pairs given by hand kept
 *
 * Least-distance pairings cross little, which is why they route easily:
 * the chip measured on 5 November routed in 3.6 s paired so, 66 s paired
 * at random (2026-11-06-metal-router-improvements.md).
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "router.h"

#include <limits>

namespace qucs_s::router {

QVector<int> assignPads(const QVector<Pt>& starts, const QVector<Pt>& pads, const QVector<QPair<int, int>>& fixed)
{
    QVector<int> out(starts.size(), -1);
    QVector<bool> startTaken(starts.size(), false), padTaken(pads.size(), false);
    for (const auto& [s, p] : fixed) {
        if (s < 0 || s >= starts.size() || p < 0 || p >= pads.size() || startTaken[s] || padTaken[p]) continue;
        out[s] = p;
        startTaken[s] = padTaken[p] = true;
    }
    QVector<int> rows, cols;
    for (int i = 0; i < starts.size(); ++i)
        if (!startTaken[i]) rows.append(i);
    for (int j = 0; j < pads.size(); ++j)
        if (!padTaken[j]) cols.append(j);
    if (rows.isEmpty() || cols.isEmpty()) return out;
    // The rectangular problem made square with dummies of no cost: n rows
    // of starts (and dummies), m >= n columns.
    const bool transposed = rows.size() > cols.size();
    const QVector<int>& R = transposed ? cols : rows;
    const QVector<int>& C = transposed ? rows : cols;
    const int n = int(R.size()), m = int(C.size());
    const auto cost = [&](int i, int j) {
        const Pt a = transposed ? starts[C[j]] : starts[R[i]];
        const Pt b = transposed ? pads[R[i]] : pads[C[j]];
        return double(std::abs(a.x - b.x) + std::abs(a.y - b.y));
    };
    // Jonker-Volgenant style potentials (e-maxx's O(n^2 m)).
    const double inf = std::numeric_limits<double>::infinity();
    std::vector<double> u(std::size_t(n) + 1, 0), v(std::size_t(m) + 1, 0);
    std::vector<int> p(std::size_t(m) + 1, 0), way(std::size_t(m) + 1, 0);
    for (int i = 1; i <= n; ++i) {
        p[0] = i;
        int j0 = 0;
        std::vector<double> minv(std::size_t(m) + 1, inf);
        std::vector<char> used(std::size_t(m) + 1, 0);
        do {
            used[std::size_t(j0)] = 1;
            const int i0 = p[std::size_t(j0)];
            double delta = inf;
            int j1 = 0;
            for (int j = 1; j <= m; ++j) {
                if (used[std::size_t(j)]) continue;
                const double cur = cost(i0 - 1, j - 1) - u[std::size_t(i0)] - v[std::size_t(j)];
                if (cur < minv[std::size_t(j)]) {
                    minv[std::size_t(j)] = cur;
                    way[std::size_t(j)] = j0;
                }
                if (minv[std::size_t(j)] < delta) {
                    delta = minv[std::size_t(j)];
                    j1 = j;
                }
            }
            for (int j = 0; j <= m; ++j) {
                if (used[std::size_t(j)]) {
                    u[std::size_t(p[std::size_t(j)])] += delta;
                    v[std::size_t(j)] -= delta;
                } else {
                    minv[std::size_t(j)] -= delta;
                }
            }
            j0 = j1;
        } while (p[std::size_t(j0)] != 0);
        do {
            const int j1 = way[std::size_t(j0)];
            p[std::size_t(j0)] = p[std::size_t(j1)];
            j0 = j1;
        } while (j0);
    }
    for (int j = 1; j <= m; ++j) {
        if (p[std::size_t(j)] == 0) continue;
        const int i = p[std::size_t(j)] - 1;
        if (transposed) out[C[j - 1]] = R[i];
        else out[R[i]] = C[j - 1];
    }
    return out;
}

} // namespace qucs_s::router
