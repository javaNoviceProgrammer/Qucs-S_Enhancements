/*
 * test_rf25_edge.cpp - the current across a metal's edge, solved in its
 *                      cross-section (rf25_edge.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <QtTest>

#include "rf25/rf25_edge.h"

#include <cmath>

namespace rf = qucs_s::rf25;

class TestRf25Edge : public QObject
{
    Q_OBJECT

private slots:
    // At a frequency whose skin is many times the metal, the current is
    // even across it: a band loses its own resistance at direct current,
    // |I|^2 / (sigma w t/2), and carries its share of the current.
    void atALowFrequencyTheCurrentIsEven()
    {
        const double sigma = 5.8e7, t = 3e-6;
        const rf::EdgeSolution e = rf::solveEdge(sigma, t, 1e3);
        QVERIFY(e.ok());
        double total = 0;
        for (const auto& i : e.current) total += i.real();
        QVERIFY2(std::abs(total - 0.25) < 1e-9, qPrintable(QString::number(total)));
        for (const double w : {1e-6, 5e-6, 20e-6}) {
            const rf::EdgeBand b = rf::bandOf(e, 0, w);
            const double even = 0.25 * w / e.reach;
            QVERIFY2(std::abs(b.current.real() / even - 1) < 1e-3, qPrintable(QStringLiteral("w %1: %2 of %3").arg(w).arg(b.current.real()).arg(even)));
            const double own = std::norm(b.current) / (sigma * w * t / 2);
            QVERIFY2(std::abs(b.loss / own - 1) < 1e-3, qPrintable(QStringLiteral("w %1: loss %2 of %3").arg(w).arg(b.loss).arg(own)));
        }
    }

    // Many skin depths thick, the current crowds to the edge: a band at the
    // edge loses more than its current spread evenly over its face's strip
    // and its half of the wall at the surface resistance would, and a band
    // as wide far in from the edge loses as an even sheet of its face does.
    void athighFrequencyTheEdgeCrowds()
    {
        const double sigma = 4.1e7, t = 3e-6, f = 30e9;
        const double rs = std::sqrt(M_PI * f * 4e-7 * M_PI / sigma);
        const rf::EdgeSolution e = rf::solveEdge(sigma, t, f);
        QVERIFY(e.ok());
        const double w = 2e-6;
        const rf::EdgeBand atEdge = rf::bandOf(e, 0, w);
        const double evenEdge = rs * std::norm(atEdge.current) / (w + t / 2);
        QVERIFY2(atEdge.loss > 1.05 * evenEdge, qPrintable(QStringLiteral("%1 W/m against an even %2").arg(atEdge.loss).arg(evenEdge)));
        const rf::EdgeBand inside = rf::bandOf(e, 15 * t, 15 * t + w);
        const double evenInside = rs * std::norm(inside.current) / w;
        QVERIFY2(std::abs(inside.loss / evenInside - 1) < 0.05, qPrintable(QStringLiteral("%1 W/m against an even %2").arg(inside.loss).arg(evenInside)));
        // The edge carries more than its share of the current, too.
        QVERIFY(std::abs(atEdge.current) / w > 1.5 * std::abs(inside.current) / w);
    }

    // Made once for a metal and a frequency, and the same when asked again.
    void anEdgeIsSolvedOnceAndKept()
    {
        const auto a = rf::edgeSolution(3.6e7, 6.5e-6, 10e9);
        const auto b = rf::edgeSolution(3.6e7, 6.5e-6, 10e9);
        QVERIFY(a && a->ok());
        QCOMPARE(a.get(), b.get());
        QVERIFY(!rf::solveEdge(0, 1e-6, 1e9).ok());
        QVERIFY(!rf::solveEdge(1e7, 0, 1e9).ok());
        QVERIFY(!rf::solveEdge(1e7, 1e-6, 0).ok());
    }
};

QTEST_GUILESS_MAIN(TestRf25Edge)
#include "test_rf25_edge.moc"
