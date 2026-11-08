/*
 * test_layout_router.cpp - the layout viewer's metal router alone
 *                          (layoutrouter/): synthetic layouts with known
 *                          answers, the checks on the shapes drawn, the
 *                          cell written to a GDS and read back, the
 *                          technology, job and CSV readers fuzzed.
 *                          No time is asserted (CI's runners vary).
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "router.h"
#include "router_gds.h"
#include "router_job.h"
#include "router_verify.h"

#include <gdstk/gdstk.hpp>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QtTest>

#include <numbers>
#include <numeric>
#include <set>

#ifndef _WIN32
#include <sys/resource.h>   // (the process's peak memory, for the benchmark's line)
#endif

using namespace qucs_s::router;

namespace {

// Two metals, M1 lines along x and M2 along y, a via of 2 µm in 4 µm
// landings; lines 2 µm wide, 2 µm apart; a grid of 1 µm.
QJsonObject techJson(double width = 2, double spacing = 2, double grid = 1)
{
    return QJsonObject{
        {"name", "test"},
        {"grid", grid},
        {"metals", QJsonArray{QJsonObject{{"name", "M1"}, {"layer", "1/0"}, {"direction", "h"}},
                              QJsonObject{{"name", "M2"}, {"layer", "2/0"}, {"direction", "v"}}}},
        {"vias", QJsonArray{QJsonObject{{"name", "V1"}, {"layer", "3/0"}, {"between", QJsonArray{"M1", "M2"}}, {"size", 2}, {"enclosure", 1}, {"spacing", 2}}}},
        {"classes", QJsonArray{QJsonObject{{"name", "signal"}, {"width", width}}, QJsonObject{{"name", "wide"}, {"width", 6}}}},
        {"spacing", QJsonObject{{"same_layer", spacing}, {"to_metal", spacing}, {"to_terminal", spacing}}},
        {"costs", QJsonObject{{"bend", 2}, {"via", 10}, {"wrong_way", 3}}},
        {"area", QJsonObject{{"margin", 40}}}};
}

Technology tech(const QJsonObject& o = techJson())
{
    QString error;
    Technology t = readTechnology(o, &error);
    if (!error.isEmpty()) qWarning() << error;
    return t;
}

Problem problemOf(const Technology& t)
{
    Problem p;
    p.tech = t;
    p.unit = 0.001;
    p.options.threads = 2;
    return p;
}

Poly rect(const Problem& p, double x0, double y0, double x1, double y1)
{
    return polyOf(Box{p.units(x0), p.units(y0), p.units(x1), p.units(y1)});
}

// A square pad of side \a side at (x, y) on \a metal.
void pad(Problem& p, int metal, double x, double y, double side = 6)
{
    p.metal.append({rect(p, x - side / 2, y - side / 2, x + side / 2, y + side / 2), metal});
}

Terminal at(const Problem& p, double x, double y, int metal = -1, Terminal::Mode mode = Terminal::Mode::Centre)
{
    Terminal t;
    t.at = {p.units(x), p.units(y)};
    t.metal = metal;
    t.mode = mode;
    return t;
}

Net net(const QString& name, const Terminal& a, const Terminal& b, int cls = 0)
{
    Net n;
    n.name = name;
    n.netClass = cls;
    n.terminals = {a, b};
    return n;
}

int viaCount(const Result& r)
{
    int n = 0;
    for (const NetResult& x : r.nets) n += x.vias.size();
    return n;
}

bool usesMetal(const NetResult& r, int metal)
{
    for (const Run& run : r.runs)
        if (run.metal == metal && run.a != run.b) return true;
    return false;
}

QString describe(const Verification& v)
{
    QStringList s;
    for (const Finding& f : v.findings) s << f.kind + QStringLiteral(": ") + f.text;
    return s.join(QStringLiteral("\n"));
}

Result routeAndCheck(Problem p, Verification* v = nullptr, const QStringList& which = {})
{
    Router router(std::move(p));
    if (!router.prepare()) {
        qWarning() << router.error();
        return {};
    }
    Result r = router.run(which);
    if (v) *v = verify(router.problem(), r);
    return r;
}

// The rectangles of \a area not covered by any of \a free.
QVector<Box> wallsAround(const Box& area, const QVector<Box>& free)
{
    std::set<Coord> xs{area.x0, area.x1}, ys{area.y0, area.y1};
    for (const Box& f : free) {
        xs.insert(f.x0);
        xs.insert(f.x1);
        ys.insert(f.y0);
        ys.insert(f.y1);
    }
    const std::vector<Coord> X(xs.begin(), xs.end()), Y(ys.begin(), ys.end());
    QVector<Box> walls;
    for (std::size_t i = 0; i + 1 < X.size(); ++i)
        for (std::size_t j = 0; j + 1 < Y.size(); ++j) {
            const Box c{X[i], Y[j], X[i + 1], Y[j + 1]};
            if (c.x0 < area.x0 || c.x1 > area.x1 || c.y0 < area.y0 || c.y1 > area.y1) continue;
            bool covered = false;
            for (const Box& f : free) covered = covered || f.contains(c);
            if (!covered) walls.append(c);
        }
    return walls;
}

} // namespace

class TestLayoutRouter : public QObject
{
    Q_OBJECT

private slots:
    void aStraightNetEndsInTheMiddleOfItsPads()
    {
        Problem p = problemOf(tech());
        pad(p, 0, 0, 0);
        pad(p, 0, 100, 0.5);   // its middle off the grid: the tail takes the line there
        p.nets = {net("a", at(p, 1, 1), at(p, 99, 0))};
        Verification v;
        const Result r = routeAndCheck(p, &v);
        QCOMPARE(r.nets.size(), 1);
        QCOMPARE(r.nets[0].status, NetResult::Status::Routed);
        QCOMPARE(viaCount(r), 0);
        QVERIFY(usesMetal(r.nets[0], 0));
        QVERIFY2(v.clean(), qPrintable(describe(v)));
        // Its ends: the pads' middles, (0, 0) and (100, 0.5), each a run's end.
        bool start = false, end = false;
        for (const Run& run : r.nets[0].runs) {
            for (const Pt& q : {run.a, run.b}) {
                start = start || q == Pt{0, 0};
                end = end || q == Pt{100000, 500};
            }
        }
        QVERIFY(start);
        QVERIFY(end);
        QVERIFY(std::abs(double(r.nets[0].length) / 1000 - 100.5) < 2.5);
    }

    void aCrossingTakesTheSecondMetal()
    {
        Problem p = problemOf(tech());
        // Pads on M1 only: one net along x, one along y, through each other.
        pad(p, 0, 0, 50);
        pad(p, 0, 100, 50);
        pad(p, 0, 50, 0);
        pad(p, 0, 50, 100);
        p.nets = {net("h", at(p, 0, 50), at(p, 100, 50)), net("v", at(p, 50, 0), at(p, 50, 100))};
        Verification v;
        const Result r = routeAndCheck(p, &v);
        QCOMPARE(r.routed(), 2);
        QVERIFY2(v.clean(), qPrintable(describe(v)));
        QVERIFY(viaCount(r) >= 2);
        QVERIFY(usesMetal(r.nets[0], 1) || usesMetal(r.nets[1], 1));
    }

    void aKeepOutIsGoneRound()
    {
        Problem p = problemOf(tech());
        pad(p, 0, 0, 0);
        pad(p, 0, 100, 0);
        KeepOut k;
        k.name = "box";
        k.outline = rect(p, 40, -20, 60, 20);
        p.keepOuts = {k};
        p.nets = {net("a", at(p, 0, 0), at(p, 100, 0))};
        Verification v;
        const Result r = routeAndCheck(p, &v);
        QCOMPARE(r.routed(), 1);
        QVERIFY2(v.clean(), qPrintable(describe(v)));
        // Round it: at least 2 x 21 µm more than straight.
        QVERIFY(double(r.nets[0].length) / 1000 > 140);
    }

    void aKeepOutKeepsOutANetNotNamedInIt()
    {
        // A wall 4 µm thick and 400 µm long across the way: through it is
        // far shorter than round it, but the net is neither named in it nor
        // has a terminal inside, so it goes round.
        Problem p = problemOf(tech());
        pad(p, 0, 0, 0);
        pad(p, 0, 100, 0);
        KeepOut k;
        k.name = "wall";
        k.outline = rect(p, 48, -120, 52, 120);
        p.keepOuts = {k};
        p.tech.margin = 160;   // room to go round
        p.nets = {net("a", at(p, 0, 0), at(p, 100, 0))};
        Verification v;
        const Result r = routeAndCheck(p, &v);
        QCOMPARE(r.routed(), 1);
        QVERIFY2(v.clean(), qPrintable(describe(v)));
        QVERIFY(v.notes.isEmpty());
        QVERIFY(double(r.nets[0].length) / 1000 > 300);
    }

    void aKeepOutOnOneMetalIsCrossedOnTheOther()
    {
        Problem p = problemOf(tech());
        pad(p, 0, 0, 0);
        pad(p, 0, 100, 0);
        KeepOut k;
        k.name = "m1 only";
        k.outline = rect(p, 40, -60, 60, 60);
        k.metals = {0};
        p.keepOuts = {k};
        p.nets = {net("a", at(p, 0, 0), at(p, 100, 0))};
        Verification v;
        const Result r = routeAndCheck(p, &v);
        QCOMPARE(r.routed(), 1);
        QVERIFY2(v.clean(), qPrintable(describe(v)));
        QCOMPARE(viaCount(r), 2);
        QVERIFY(usesMetal(r.nets[0], 1));
    }

    void aContactUnderALowerMetalKeepOutLeavesThroughAVia()
    {
        // The contact sits on M1 inside an area where M1 may not run (an
        // obstacle on 30/0 for M1): the line must land a via on it and go up.
        QJsonObject o = techJson();
        o.insert("obstacles", QJsonArray{QJsonObject{{"name", "optical"}, {"layer", "30/0"}, {"clearance", 0}, {"metals", QJsonArray{"M1"}}, {"landings", true}}});
        Problem p = problemOf(tech(o));
        pad(p, 0, 0, 0, 8);         // the contact, 8 µm, on M1
        pad(p, 0, 120, 0);          // the pad, on M1
        p.obstacles.append({rect(p, -30, -30, 30, 30), 0});
        p.nets = {net("c", at(p, 0, 0), at(p, 120, 0))};
        Verification v;
        const Result r = routeAndCheck(p, &v);
        QCOMPARE(r.routed(), 1);
        QVERIFY2(v.clean(), qPrintable(describe(v)));
        QVERIFY(viaCount(r) >= 2);
        // The first via is on the contact.
        bool onContact = false;
        for (const ViaPlaced& via : r.nets[0].vias) onContact = onContact || (std::abs(via.at.x) <= 4000 && std::abs(via.at.y) <= 4000);
        QVERIFY(onContact);
        // No M1 of the route inside the obstacle beyond the contact.
        for (const Run& run : r.nets[0].runs)
            if (run.metal == 0) {
                const Box b = run.box();
                QVERIFY(!(b.overlaps(Box{-30000, -30000, 30000, 30000}) && !Box{-4000, -4000, 4000, 4000}.contains(b)));
            }
    }

    void aLandingLargerThanItsContactNeedsTheRuleToAllowIt()
    {
        // A 3 µm contact under the 4 µm landing: with landings allowed it
        // routes; without, the M1 the landing adds is in the obstacle.
        for (const bool allowed : {true, false}) {
            QJsonObject o = techJson();
            o.insert("obstacles",
                     QJsonArray{QJsonObject{{"name", "optical"}, {"layer", "30/0"}, {"clearance", 0}, {"metals", QJsonArray{"M1"}}, {"landings", allowed}}});
            Problem p = problemOf(tech(o));
            pad(p, 0, 0, 0, 3);
            pad(p, 0, 120, 0);
            p.obstacles.append({rect(p, -30, -30, 30, 30), 0});
            p.nets = {net("c", at(p, 0, 0), at(p, 120, 0))};
            Verification v;
            const Result r = routeAndCheck(p, &v);
            if (allowed) {
                QCOMPARE(r.routed(), 1);
                QVERIFY2(v.clean(), qPrintable(describe(v)));
            } else {
                QCOMPARE(r.routed(), 0);
                QVERIFY2(r.nets[0].reason.contains("optical"), qPrintable(r.nets[0].reason));
            }
        }
    }

    void anImpossibleNetSaysWhyAndWhere()
    {
        Problem p = problemOf(tech());
        pad(p, 0, 0, 0);
        pad(p, 0, 100, 0);
        // A ring round the pad on both metals, nothing let through.
        KeepOut ring;
        ring.name = "fence";
        ring.letThrough = KeepOut::LetThrough::None;
        ring.outline = {{p.units(80), p.units(-20)}, {p.units(120), p.units(-20)}, {p.units(120), p.units(20)}, {p.units(80), p.units(20)},
                        {p.units(80), p.units(-16)}, {p.units(84), p.units(-16)}, {p.units(84), p.units(16)}, {p.units(116), p.units(16)},
                        {p.units(116), p.units(-16)}, {p.units(80), p.units(-16)}};
        p.keepOuts = {ring};
        p.nets = {net("a", at(p, 0, 0), at(p, 100, 0))};
        Verification v;
        const Result r = routeAndCheck(p, &v);
        QCOMPARE(r.nets[0].status, NetResult::Status::Failed);
        QVERIFY2(r.nets[0].reason.contains("no way"), qPrintable(r.nets[0].reason));
        QVERIFY2(r.nets[0].reason.contains("fence"), qPrintable(r.nets[0].reason));
        QVERIFY(r.nets[0].hasWhere);
        // The place named is where it was stopped: by the fence's west side.
        QVERIFY(std::abs(double(r.nets[0].where.x) / 1000 - 76) < 6);
        // And the check lists it, so a clean run means every net is routed.
        QVERIFY(!v.clean());
        QCOMPARE(v.findings[0].kind, QStringLiteral("unroutable"));
    }

    void aTerminalThatCannotBeLeftIsSaidAtOnce()
    {
        // A 10 µm line from a 4 µm wide stub boxed in by foreign metal on all
        // metals: prepare() says so before any run.
        Problem p = problemOf(tech(techJson(10, 4)));
        p.metal.append({rect(p, -2, -20, 2, 20), 0});     // the stub
        for (int m = 0; m < 2; ++m) {
            p.metal.append({rect(p, -10, -40, -6, 40), m});
            p.metal.append({rect(p, 6, -40, 10, 40), m});
        }
        pad(p, 0, 200, 0, 20);
        p.nets = {net("s", at(p, 0, 0), at(p, 200, 0))};
        Router router(p);
        QVERIFY(router.prepare());
        QVERIFY(!router.terminals().isEmpty());
        QVERIFY2(router.terminals()[0].note.contains("can leave"), qPrintable(router.terminals()[0].note));
        const Result r = router.run();
        QCOMPARE(r.nets[0].status, NetResult::Status::Failed);
        QVERIFY2(r.nets[0].reason.contains("cannot be left"), qPrintable(r.nets[0].reason));
    }

    void stackedMetalWithNoCutIsNotTheTerminal()
    {
        // The black-box test's finding 1: a contact on M1 with M2 drawn over
        // it and no cut between them. The terminal is the M1 square; the M2
        // over it is said, and a line that ends only on the M2 is an open.
        Problem p = problemOf(tech());
        p.metal.append({rect(p, -4, -4, 4, 4), 0});   // the contact, M1
        p.metal.append({rect(p, -4, -4, 4, 4), 1});   // M2 over it, no cut
        pad(p, 1, 100, 0);                            // the pad, on M2
        p.nets = {net("n", at(p, 0, 0), at(p, 100, 0))};
        Router router(p);
        QVERIFY(router.prepare());
        QVERIFY2(router.terminals()[0].note.contains("not joined"), qPrintable(router.terminals()[0].note));
        QCOMPARE(router.terminals()[0].metals, (QVector<int>{0}));
        const Result r = router.run();
        QCOMPARE(r.routed(), 1);
        const Verification v = verify(router.problem(), r);
        QVERIFY2(v.clean(), qPrintable(describe(v)));
        QVERIFY(viaCount(r) >= 1);   // down to the contact
        // Ending on the M2 square alone: the check says open.
        Result bad = r;
        bad.nets[0].runs = {Run{1, {0, 0}, {p.units(100), 0}, p.units(2)}};
        bad.nets[0].vias.clear();
        bool open = false;
        for (const Finding& f : verify(router.problem(), bad).findings) open = open || f.kind == QLatin1String("open");
        QVERIFY(open);
    }

    void aNotchIsFilledWhereItFits()
    {
        // A line back along its own bar 4 µm above it: a slot under the 5 µm
        // notch rule. Found by the check; filled with the net's metal; then
        // clean. A fill never takes metal of another net's into the slot.
        QJsonObject o = techJson();
        o.insert("spacing", QJsonObject{{"same_layer", 2}, {"to_metal", 2}, {"to_terminal", 2}, {"notch", 5}});
        Problem p = problemOf(tech(o));
        QCOMPARE(p.tech.notch, 5.0);
        p.metal.append({rect(p, 0, 0, 100, 4), 0});   // its own bar
        p.nets = {net("n", at(p, 95, 2, -1, Terminal::Mode::Exact), at(p, 10, 9, 0, Terminal::Mode::Exact))};
        Router router(p);
        QVERIFY(router.prepare());
        Result r;
        NetResult x;
        x.status = NetResult::Status::Routed;
        x.runs = {Run{0, {p.units(95), p.units(2)}, {p.units(95), p.units(9)}, p.units(2)}, Run{0, {p.units(95), p.units(9)}, {p.units(10), p.units(9)}, p.units(2)}};
        r.nets = {x};
        const auto kinds = [&](const Result& res) {
            QStringList k;
            for (const Finding& f : verify(router.problem(), res).findings) k << f.kind;
            return k;
        };
        QVERIFY2(kinds(r).contains("notch"), qPrintable(kinds(r).join(',')));
        fillNotches(router.problem(), &r);
        QCOMPARE(r.nets[0].fills.size(), 1);
        QVERIFY2(kinds(r).isEmpty(), qPrintable(kinds(r).join(',')));
        // The fill is written with the route.
        bool written = false;
        for (const DrawnShape& d : drawnShapes(router.problem(), r.nets[0], 0)) written = written || d.points.size() > 4 || d.points.size() == 4;
        QVERIFY(written);
        QCOMPARE(int(drawnShapes(router.problem(), r.nets[0], 0).size()), 3);
        // Metal of no net in the slot: no fill, and no notch either (the
        // spacing check has it).
        Problem q = p;
        q.metal.append({rect(q, 40, 5.5, 50, 6.5), 0});
        Router second(q);
        QVERIFY(second.prepare());
        Result r2;
        r2.nets = {x};
        fillNotches(second.problem(), &r2);
        QVERIFY(r2.nets[0].fills.isEmpty());
        QStringList k2;
        for (const Finding& f : verify(second.problem(), r2).findings) k2 << f.kind;
        QVERIFY2(!k2.contains("notch") && k2.contains("metal"), qPrintable(k2.join(',')));
    }

    void viasAreArraysOfCuts()
    {
        // The black-box test's finding 7: a 2 x 2 array of 1 µm cuts 1 µm
        // apart, 0.5 µm enclosed: a 3 µm outline in a 4 µm landing.
        QJsonObject o = techJson(4, 2);
        o.insert("vias", QJsonArray{QJsonObject{{"name", "V1"}, {"layer", "3/0"}, {"between", QJsonArray{"M1", "M2"}}, {"size", 1}, {"cuts", QJsonArray{2, 2}},
                                                {"cut_spacing", 1}, {"enclosure", 0.5}, {"spacing", 3}}});
        Problem p = problemOf(tech(o));
        QCOMPARE(p.tech.vias[0].outline(), 3.0);
        QCOMPARE(p.tech.vias[0].landingLower, 4.0);
        pad(p, 0, 0, 50, 8);
        pad(p, 0, 100, 50, 8);
        pad(p, 0, 50, 0, 8);
        pad(p, 0, 50, 100, 8);
        p.nets = {net("h", at(p, 0, 50), at(p, 100, 50)), net("v", at(p, 50, 0), at(p, 50, 100))};
        Verification v;
        Router router(p);
        QVERIFY(router.prepare());
        const Result r = router.run();
        v = verify(router.problem(), r);
        QCOMPARE(r.routed(), 2);
        QVERIFY2(v.clean(), qPrintable(describe(v)));
        int vias = 0, cuts = 0;
        for (int n = 0; n < 2; ++n) {
            vias += r.nets[n].vias.size();
            for (const DrawnShape& d : drawnShapes(router.problem(), r.nets[n], n))
                if (d.layer.layer == 3) {
                    ++cuts;
                    QCOMPARE(d.points[1].x() - d.points[0].x(), 1.0);
                }
        }
        QVERIFY(vias >= 2);
        QCOMPARE(cuts, 4 * vias);
        // Two arrays 2 µm apart (3 needed): found, on their outlines.
        Result bad = r;
        bad.nets[0].vias = {ViaPlaced{0, {p.units(30), p.units(50)}}, ViaPlaced{0, {p.units(35), p.units(50)}}};
        bool near = false;
        for (const Finding& f : verify(router.problem(), bad).findings) near = near || f.kind == QLatin1String("via_spacing");
        QVERIFY(near);
        // An array with no spacing inside it is refused.
        QJsonObject noGap = o;
        QJsonArray vs = noGap.value("vias").toArray();
        QJsonObject v0 = vs[0].toObject();
        v0.remove("cut_spacing");
        vs[0] = v0;
        noGap.insert("vias", vs);
        QString e;
        readTechnology(noGap, &e);
        QVERIFY2(e.contains("cut_spacing"), qPrintable(e));
    }

    void aRunNearItsMemoryCapGoesOnOneSearchAtATime()
    {
        // The black-box test's finding 2: a cap a run nears does not stop it.
        Problem p = problemOf(tech());
        for (int i = 0; i < 6; ++i) {
            pad(p, 0, 0, i * 20);
            pad(p, 0, 200, (5 - i) * 20);
            p.nets.append(net(QStringLiteral("n%1").arg(i), at(p, 0, i * 20), at(p, 200, (5 - i) * 20)));
        }
        p.options.maxMemory = 16;
        p.options.threads = 4;
        Router router(p);
        QVERIFY2(router.prepare(), qPrintable(router.error()));
        const Result r = router.run();
        QCOMPARE(r.routed(), 6);
        QVERIFY2(r.stopped == "converged", qPrintable(r.stopped));
        // Released afterwards, and prepared again for the next run.
        router.releaseMaps();
        QCOMPARE(router.run().routed(), 6);
    }

    void aLineLeavesItsOwnBusPastForeignFingers()
    {
        // The first-use note's case: a 10 µm line from the middle of a 12 µm
        // bus, foreign finger tips 2 µm east of it, 6 µm to metal. The new
        // metal starts west of the bus, 14 µm from the tips: it routes.
        QJsonObject o = techJson(10, 4);
        o.insert("spacing", QJsonObject{{"same_layer", 4}, {"to_metal", 6}, {"to_terminal", 6}});
        o.insert("metals", QJsonArray{QJsonObject{{"name", "M1"}, {"layer", "1/0"}}});
        o.remove("vias");
        Problem p = problemOf(tech(o));
        QVERIFY(p.tech.metals.size() == 1);
        p.metal.append({rect(p, 388, 0, 400, 400), 0});         // the bus
        p.metal.append({rect(p, 402, 180, 450, 190), 0});       // foreign fingers
        p.metal.append({rect(p, 402, 210, 450, 220), 0});
        pad(p, 0, 200, 200, 20);
        p.nets = {net("sig", at(p, 394, 200), at(p, 200, 200))};
        Verification v;
        const Result r = routeAndCheck(p, &v);
        QCOMPARE(r.routed(), 1);
        QVERIFY2(v.clean(), qPrintable(describe(v)));
    }

    void aWideLineDoesNotLeaveANarrowBusOverForeignFingers()
    {
        // One capacitor of the chip: a 20 µm line, 10 µm to metal, from the
        // middle of its plate's 12 µm bus, the other plate's finger tips 2 µm
        // from the bus. Either it cannot be left, or it leaves clean.
        QJsonObject o = techJson(20, 10, 4);
        o.insert("vias", QJsonArray{QJsonObject{{"name", "V1"}, {"layer", "3/0"}, {"between", QJsonArray{"M1", "M2"}}, {"size", 4}, {"enclosure", 2}, {"spacing", 4}}});
        o.insert("classes", QJsonArray{QJsonObject{{"name", "signal"}, {"width", 20}}});
        o.insert("spacing", QJsonObject{{"same_layer", 10}, {"to_metal", 10}, {"to_terminal", 10}});
        Problem p = problemOf(tech(o));
        const double x0 = 3500, y0 = 800;
        for (int m = 0; m < 2; ++m) {
            p.metal.append({rect(p, x0, y0, x0 + 12, y0 + 150), m});
            p.metal.append({rect(p, x0 + 188, y0, x0 + 200, y0 + 150), m});
            for (int f = 0; f < 9; ++f) {
                const double fy = y0 + 4 + f * 16;
                p.metal.append({rect(p, x0 + 12, fy, x0 + 186, fy + 4), m});
                p.metal.append({rect(p, x0 + 14, fy + 8, x0 + 188, fy + 12), m});
            }
        }
        pad(p, 1, 3900, 600, 80);
        p.nets = {net("b", at(p, x0 + 194, y0 + 75), at(p, 3900, 600))};
        Verification v;
        const Result r = routeAndCheck(p, &v);
        QVERIFY2(v.clean() || (r.nets[0].status == NetResult::Status::Failed && r.nets[0].reason.contains("cannot be left")),
                 qPrintable(describe(v)));
    }

    void anOrderedRunFailsWhereNegotiationSucceeds()
    {
        // Two rooms joined by a short corridor wide enough for one line of
        // either class, and by a long narrow one only the thin class fits.
        // The thin net is shorter and goes first: in order it takes the short
        // corridor and the wide net has no way; negotiated, it gives way.
        Problem p = problemOf(tech());
        p.options.threads = 1;
        const auto u = [&](double v) { return p.units(v); };
        const Box area{u(-70), u(-100), u(170), u(30)};
        const QVector<Box> free = {
            {u(-60), u(-20), u(0), u(20)},     // the left room
            {u(100), u(-20), u(160), u(20)},   // the right room
            {u(0), u(-4), u(100), u(4)},       // the short corridor, 8 µm
            {u(-52), u(-90), u(-48), u(-20)},  // the long one, 4 µm
            {u(-52), u(-90), u(154), u(-86)},
            {u(150), u(-90), u(154), u(-20)},
        };
        int w = 0;
        for (const Box& b : wallsAround(area, free)) {
            KeepOut k;
            k.name = QStringLiteral("wall %1").arg(++w);
            k.outline = polyOf(b);
            k.letThrough = KeepOut::LetThrough::None;
            p.keepOuts.append(k);
        }
        p.tech.box = {-70, -100, 170, 30};
        p.tech.metals.resize(1);   // one metal
        p.tech.vias.clear();
        p.nets = {net("thin", at(p, -10, 0), at(p, 110, 0), 0), net("wide", at(p, -55, 10), at(p, 155, 10), 1)};

        Problem ordered = p;
        ordered.options.negotiate = false;
        Verification vo;
        const Result ro = routeAndCheck(ordered, &vo);
        QCOMPARE(ro.nets[0].status, NetResult::Status::Routed);
        QCOMPARE(ro.nets[1].status, NetResult::Status::Failed);
        QCOMPARE(ro.stopped, QStringLiteral("ordered"));

        Verification v;
        const Result r = routeAndCheck(p, &v);
        QCOMPARE(r.routed(), 2);
        QVERIFY2(v.clean(), qPrintable(describe(v)));
        QVERIFY(r.passes.size() >= 2);
        QCOMPARE(r.stopped, QStringLiteral("converged"));
        // The thin net took the long way, below the rooms.
        bool low = false;
        for (const Run& run : r.nets[0].runs) low = low || run.a.y < u(-80) || run.b.y < u(-80);
        QVERIFY(low);
    }

    void aCrowdedRunSaysSoAndStops()
    {
        // Six nets through a corridor with room for two: crowded before the
        // run, stalled during it, and the four that do not fit reported.
        Problem p = problemOf(tech());
        p.tech.metals.resize(1);
        p.tech.vias.clear();
        const auto u = [&](double v) { return p.units(v); };
        const Box area{u(-60), u(-40), u(160), u(40)};
        const QVector<Box> free = {{u(-60), u(-40), u(0), u(40)}, {u(100), u(-40), u(160), u(40)}, {u(0), u(-5), u(100), u(5)}};
        for (const Box& b : wallsAround(area, free)) {
            KeepOut k;
            k.name = "wall";
            k.outline = polyOf(b);
            k.letThrough = KeepOut::LetThrough::None;
            p.keepOuts.append(k);
        }
        p.tech.box = {-60, -40, 160, 40};
        for (int i = 0; i < 6; ++i) p.nets.append(net(QStringLiteral("n%1").arg(i), at(p, -50, -30 + 12 * i), at(p, 150, -30 + 12 * i)));
        p.options.stallPasses = 3;
        p.options.passes = 30;
        Verification v;
        const Result r = routeAndCheck(p, &v);
        QCOMPARE(r.routed(), 2);
        QCOMPARE(r.failed(), 4);
        QVERIFY2(r.stopped == "stalled", qPrintable(r.stopped));
        QVERIFY(r.passes.size() < 30);
        QVERIFY(r.passes.last().cleanup);
        bool crowded = false;
        for (const NetResult& n : r.nets) crowded = crowded || n.reason.contains("too crowded");
        QVERIFY(crowded);
        // What did route is correct; the rest are its findings.
        int unroutable = 0;
        for (const Finding& f : v.findings) {
            QCOMPARE(f.kind, QStringLiteral("unroutable"));
            ++unroutable;
        }
        QCOMPARE(unroutable, 4);
        QVERIFY(!r.crowding.isEmpty());
    }

    void aStoppedRunIsCleanedUp()
    {
        // The black-box test's finding 4: stopped after its first pass, a
        // crowded run still routes its contested nets again around the
        // others, or reports them - nothing it calls routed is too near.
        Problem p = problemOf(tech());
        p.tech.metals.resize(1);
        p.tech.vias.clear();
        const auto u = [&](double v) { return p.units(v); };
        const Box area{u(-60), u(-40), u(160), u(40)};
        const QVector<Box> free = {{u(-60), u(-40), u(0), u(40)}, {u(100), u(-40), u(160), u(40)}, {u(0), u(-5), u(100), u(5)}};
        for (const Box& b : wallsAround(area, free)) {
            KeepOut k;
            k.name = "wall";
            k.outline = polyOf(b);
            k.letThrough = KeepOut::LetThrough::None;
            p.keepOuts.append(k);
        }
        p.tech.box = {-60, -40, 160, 40};
        for (int i = 0; i < 6; ++i) p.nets.append(net(QStringLiteral("n%1").arg(i), at(p, -50, -30 + 12 * i), at(p, 150, -30 + 12 * i)));
        Router router(p);
        QVERIFY(router.prepare());
        std::atomic_bool cancel{false};
        Progress progress;
        progress.cancel = &cancel;
        progress.pass = [&](const PassRow& row) {
            if (row.pass == 1 && !row.cleanup) cancel = true;
        };
        const Result r = router.run({}, {}, progress);
        QCOMPARE(r.stopped, QStringLiteral("cancelled"));
        QVERIFY(r.passes.last().cleanup);
        const Verification v = verify(router.problem(), r);
        for (const Finding& f : v.findings) QVERIFY2(f.kind == QLatin1String("unroutable"), qPrintable(f.text));
        QCOMPARE(r.routed() + r.failed(), 6);
    }

    void aGuideIsFollowed()
    {
        Problem p = problemOf(tech());
        pad(p, 0, 0, 0);
        pad(p, 0, 100, 0);
        Net n = net("g", at(p, 0, 0), at(p, 100, 0));
        n.guide = {{p.units(0), p.units(0)}, {p.units(0), p.units(40)}, {p.units(60), p.units(40)}};
        n.band = p.units(6);
        p.nets = {n};
        Verification v;
        const Result r = routeAndCheck(p, &v);
        QCOMPARE(r.routed(), 1);
        QVERIFY2(v.clean(), qPrintable(describe(v)));
        // Up the guide to y = 40 and along it, not straight across.
        bool high = false;
        for (const Run& run : r.nets[0].runs) high = high || (run.a.y >= p.units(34) && run.b.y >= p.units(34) && run.a.x != run.b.x);
        QVERIFY(high);
    }

    void aNetMayGoIntoAKeepOutItsTerminalIsIn()
    {
        Problem p = problemOf(tech());
        pad(p, 0, 0, 0);
        pad(p, 0, 100, 0);
        KeepOut k;
        k.name = "device";
        k.outline = rect(p, -20, -20, 20, 20);
        p.keepOuts = {k};
        p.nets = {net("lead", at(p, 0, 0), at(p, 100, 0)), net("other", at(p, -60, 30), at(p, -60, -30))};
        pad(p, 0, -60, 30);
        pad(p, 0, -60, -30);
        Verification v;
        const Result r = routeAndCheck(p, &v);
        QCOMPARE(r.routed(), 2);
        QVERIFY2(v.clean(), qPrintable(describe(v)));
        QVERIFY(!v.notes.isEmpty());   // "lead goes into the keep-out device ... as it may"
        // Only its way in: the lead leaves the box eastwards, straight.
        QVERIFY(double(r.nets[0].length) / 1000 < 110);
    }

    void threeMetalsStackTheirVias()
    {
        QJsonObject o = techJson();
        o.insert("metals", QJsonArray{QJsonObject{{"name", "M1"}, {"layer", "1/0"}}, QJsonObject{{"name", "M2"}, {"layer", "2/0"}},
                                      QJsonObject{{"name", "M3"}, {"layer", "4/0"}}});
        o.insert("vias", QJsonArray{QJsonObject{{"name", "V1"}, {"layer", "3/0"}, {"between", QJsonArray{"M1", "M2"}}, {"size", 2}, {"enclosure", 1}},
                                    QJsonObject{{"name", "V2"}, {"layer", "5/0"}, {"between", QJsonArray{"M2", "M3"}}, {"size", 2}, {"enclosure", 1}}});
        Problem p = problemOf(tech(o));
        QCOMPARE(p.tech.metals.size(), 3);
        pad(p, 0, 0, 0);
        pad(p, 0, 100, 0);
        KeepOut k;
        k.name = "low";
        k.outline = rect(p, 40, -80, 60, 80);
        k.metals = {0, 1};
        p.keepOuts = {k};
        p.nets = {net("a", at(p, 0, 0), at(p, 100, 0))};
        Verification v;
        const Result r = routeAndCheck(p, &v);
        QCOMPARE(r.routed(), 1);
        QVERIFY2(v.clean(), qPrintable(describe(v)));
        QVERIFY(usesMetal(r.nets[0], 2));
        QCOMPARE(viaCount(r), 4);
    }

    void manyPadsAreJoinedAsOneTree()
    {
        Problem p = problemOf(tech());
        pad(p, 0, 0, 0);
        pad(p, 0, 80, 0);
        pad(p, 0, 80, 60);
        pad(p, 0, 0, 60);
        Net n = net("t", at(p, 0, 0), at(p, 80, 0));
        n.terminals.append(at(p, 80, 60));
        n.terminals.append(at(p, 0, 60));
        p.nets = {n};
        Verification v;
        const Result r = routeAndCheck(p, &v);
        QCOMPARE(r.routed(), 1);
        QVERIFY2(v.clean(), qPrintable(describe(v)));
        // Three legs of a tree: some 80 + 60 + 80 µm, no more than a loop.
        QVERIFY(double(r.nets[0].length) / 1000 < 240);
    }

    void threadsDoNotChangeTheRoutes()
    {
        Problem p = problemOf(tech());
        for (int i = 0; i < 12; ++i) {
            pad(p, 0, 0, i * 20);
            pad(p, 0, 300, (11 - i) * 20);   // each crossing every other
            p.nets.append(net(QStringLiteral("n%1").arg(i), at(p, 0, i * 20), at(p, 300, (11 - i) * 20)));
        }
        QList<QByteArray> answers;
        for (const int threads : {1, 4}) {
            Problem q = p;
            q.options.threads = threads;
            Verification v;
            const Result r = routeAndCheck(q, &v);
            QCOMPARE(r.routed(), 12);
            QVERIFY2(v.clean(), qPrintable(describe(v)));
            QByteArray a;
            for (const NetResult& n : r.nets)
                for (const Run& run : n.runs) a += QByteArray::number(run.metal) + ':' + QByteArray::number(run.a.x) + ',' + QByteArray::number(run.a.y) + '-'
                                                 + QByteArray::number(run.b.x) + ',' + QByteArray::number(run.b.y) + ';';
            answers << a;
        }
        QCOMPARE(answers[0], answers[1]);
    }

    void onlyWhatChangedIsRoutedAgain()
    {
        Problem p = problemOf(tech());
        for (int i = 0; i < 3; ++i) {
            pad(p, 0, 0, i * 100);
            pad(p, 0, 200, i * 100);
            p.nets.append(net(QStringLiteral("n%1").arg(i), at(p, 0, i * 100), at(p, 200, i * 100)));
        }
        Router first(p);
        QVERIFY(first.prepare());
        const Result r1 = first.run();
        QCOMPARE(r1.routed(), 3);
        QVector<Kept> kept;
        for (int i = 0; i < 3; ++i) kept.append({p.nets[i].name, r1.nets[i]});

        // Net n1's pad moves; the others keep their routes.
        Problem q = p;
        q.metal[3] = {rect(q, 197, 107, 203, 113), 0};
        q.nets[1].terminals[1] = at(q, 200, 110);
        Router second(q);
        QVERIFY(second.prepare());
        QCOMPARE(second.keyOf(0), first.keyOf(0));
        QVERIFY(second.keyOf(1) != first.keyOf(1));
        {   // A terminal's mode alone, no shape moved: another key.
            Problem m = p;
            m.nets[2].terminals[1].mode = Terminal::Mode::Exact;
            Router third(m);
            QVERIFY(third.prepare());
            QCOMPARE(third.keyOf(0), first.keyOf(0));
            QVERIFY(third.keyOf(2) != first.keyOf(2));
        }
        const Result r2 = second.run({}, kept);
        QCOMPARE(r2.nets[0].status, NetResult::Status::Kept);
        QCOMPARE(r2.nets[2].status, NetResult::Status::Kept);
        QCOMPARE(r2.nets[1].status, NetResult::Status::Routed);
        QCOMPARE(r2.nets[0].runs.size(), r1.nets[0].runs.size());
        const Verification v = verify(second.problem(), r2);
        QVERIFY2(v.clean(), qPrintable(describe(v)));
        // Asked for by name, a kept net is routed again.
        const Result r3 = second.run({"n0"}, kept);
        QCOMPARE(r3.nets[0].status, NetResult::Status::Routed);
    }

    void verificationCatchesBrokenRoutes()
    {
        Problem p = problemOf(tech());
        pad(p, 0, 0, 0);
        pad(p, 0, 100, 0);
        pad(p, 0, 0, 30);
        pad(p, 0, 100, 30);
        pad(p, 0, 50, 60);   // a pad of no net
        KeepOut k;
        k.name = "hole";
        k.outline = rect(p, 40, -40, 60, -30);
        p.keepOuts = {k};
        p.nets = {net("a", at(p, 0, 0), at(p, 100, 0)), net("b", at(p, 0, 30), at(p, 100, 30))};
        Router router(p);
        QVERIFY(router.prepare());
        const Result good = router.run();
        QCOMPARE(good.routed(), 2);
        QVERIFY(verify(router.problem(), good).clean());
        const auto kinds = [&](const Result& r) {
            QStringList k;
            for (const Finding& f : verify(router.problem(), r).findings) k << f.kind;
            return k;
        };
        const Coord w = p.units(2);
        {   // An open: a's runs cut in the middle.
            Result r = good;
            r.nets[0].runs = {Run{0, {0, 0}, {p.units(40), 0}, w}, Run{0, {p.units(60), 0}, {p.units(100), 0}, w}};
            QVERIFY2(kinds(r).contains("open"), qPrintable(kinds(r).join(',')));
        }
        {   // Too near: a piece of b 1.5 µm from a's line (2 needed), away from every pad.
            Result r = good;
            r.nets[1].runs.append(Run{0, {p.units(40), p.units(3.5)}, {p.units(60), p.units(3.5)}, w});
            const QStringList k = kinds(r);
            QVERIFY2(k.contains("spacing") && !k.contains("short"), qPrintable(k.join(',')));
        }
        {   // Touching it: a short between the two nets.
            Result r = good;
            r.nets[1].runs.append(Run{0, {p.units(40), p.units(1.5)}, {p.units(60), p.units(1.5)}, w});
            QVERIFY2(kinds(r).contains("short"), qPrintable(kinds(r).join(',')));
        }
        {   // Over another net's pad: a short.
            Result r = good;
            r.nets[0].runs.append(Run{0, {p.units(100), 0}, {p.units(100), p.units(30)}, w});
            QVERIFY2(kinds(r).contains("short"), qPrintable(kinds(r).join(',')));
        }
        {   // Near a pad of no net: metal.
            Result r = good;
            r.nets[1].runs.append(Run{0, {p.units(50), p.units(30)}, {p.units(50), p.units(55)}, w});
            QVERIFY2(kinds(r).contains("metal"), qPrintable(kinds(r).join(',')));
        }
        {   // Into the keep-out.
            Result r = good;
            r.nets[0].runs.append(Run{0, {p.units(50), 0}, {p.units(50), p.units(-35)}, w});
            QVERIFY2(kinds(r).contains("keep_out"), qPrintable(kinds(r).join(',')));
        }
        {   // Two vias 1 µm apart (2 needed).
            Result r = good;
            r.nets[0].vias = {ViaPlaced{0, {p.units(20), 0}}, ViaPlaced{0, {p.units(23), 0}}};
            QVERIFY2(kinds(r).contains("via_spacing"), qPrintable(kinds(r).join(',')));
        }
        {   // A net marked failed is a finding.
            Result r = good;
            r.nets[1].status = NetResult::Status::Failed;
            r.nets[1].reason = "test";
            QVERIFY2(kinds(r).contains("unroutable"), qPrintable(kinds(r).join(',')));
        }
    }

    void anObstacleIsCheckedOnTheShapesDrawn()
    {
        QJsonObject o = techJson();
        o.insert("obstacles", QJsonArray{QJsonObject{{"name", "heater"}, {"layer", "40/0"}, {"clearance", 5}}});
        Problem p = problemOf(tech(o));
        pad(p, 0, 0, 0);
        pad(p, 0, 100, 0);
        p.obstacles.append({rect(p, 45, 3, 55, 20), 0});
        p.nets = {net("a", at(p, 0, 0), at(p, 100, 0))};
        Router router(p);
        QVERIFY(router.prepare());
        const Result r = router.run();
        QCOMPARE(r.routed(), 1);
        QVERIFY(verify(router.problem(), r).clean());
        // Straight through, 2 µm from it: a finding with its distance.
        Result bad = r;
        bad.nets[0].runs = {Run{0, {0, 0}, {p.units(100), 0}, p.units(2)}};
        const Verification v = verify(router.problem(), bad);
        QCOMPARE(v.findings.size(), 1);
        QCOMPARE(v.findings[0].kind, QStringLiteral("obstacle"));
        QCOMPARE(v.findings[0].measured, 2.0);
        QCOMPARE(v.findings[0].required, 5.0);
    }

    void theCellIsWrittenAndReadBack()
    {
        QTemporaryDir dir;
        const QString source = dir.filePath("chip.gds"), routed = dir.filePath("chip_routed.gds");
        // A source with pads on 1/0 in TOP.
        {
            gdstk::Library lib = {};
            lib.init("LIB", 1e-6, 1e-9);
            auto* top = static_cast<gdstk::Cell*>(gdstk::allocate_clear(sizeof(gdstk::Cell)));
            top->init("TOP");
            for (const auto& c : {std::pair{0.0, 0.0}, {100.0, 0.0}, {50.0, -50.0}, {50.0, 50.0}}) {
                auto* poly = static_cast<gdstk::Polygon*>(gdstk::allocate_clear(sizeof(gdstk::Polygon)));
                *poly = gdstk::rectangle(gdstk::Vec2{c.first - 3, c.second - 3}, gdstk::Vec2{c.first + 3, c.second + 3}, gdstk::make_tag(1, 0));
                top->polygon_array.append(poly);
            }
            lib.cell_array.append(top);
            QCOMPARE(int(lib.write_gds(QFile::encodeName(source).constData(), 0, nullptr)), 0);
            lib.free_all();
        }
        Problem p = problemOf(tech());
        pad(p, 0, 0, 0);
        pad(p, 0, 100, 0);
        pad(p, 0, 50, -50);
        pad(p, 0, 50, 50);
        p.nets = {net("h", at(p, 0, 0), at(p, 100, 0)), net("v", at(p, 50, -50), at(p, 50, 50))};
        Router router(p);
        QVERIFY(router.prepare());
        const Result r = router.run();
        QCOMPARE(r.routed(), 2);
        int expected = 0;
        QMap<quint32, int> perLayer;
        for (int n = 0; n < 2; ++n)
            for (const DrawnShape& s : drawnShapes(router.problem(), r.nets[n], n)) {
                ++expected;
                ++perLayer[s.layer.layer];
            }
        QVERIFY(perLayer.value(3) >= 2);   // the crossing's cuts

        const auto readBack = [&](const QString& file, int* polygons, int* placements, QMap<quint32, int>* layers) {
            gdstk::ErrorCode e = gdstk::ErrorCode::NoError;
            gdstk::Library lib = gdstk::read_gds(QFile::encodeName(file).constData(), 0, 0, nullptr, &e);
            *polygons = *placements = -1;
            gdstk::Cell* cell = lib.get_cell("ROUTES");
            gdstk::Cell* top = lib.get_cell("TOP");
            if (cell && top) {
                *polygons = int(cell->polygon_array.count);
                *placements = 0;
                for (uint64_t i = 0; i < top->reference_array.count; ++i)
                    *placements += top->reference_array[i]->type == gdstk::ReferenceType::Cell && top->reference_array[i]->cell == cell;
                if (layers)
                    for (uint64_t i = 0; i < cell->polygon_array.count; ++i) ++(*layers)[gdstk::get_layer(cell->polygon_array[i]->tag)];
                QCOMPARE(int(top->polygon_array.count), 4);   // the source's own, untouched
            }
            lib.free_all();
        };
        Commit c;
        c.source = source;
        c.target = routed;
        CommitReport report = commitRoutes(c, router.problem(), r);
        QVERIFY2(report.ok, qPrintable(report.error));
        QVERIFY(!report.replaced);
        QVERIFY(report.placed);
        int polygons, placements;
        QMap<quint32, int> layers;
        readBack(routed, &polygons, &placements, &layers);
        QCOMPARE(polygons, expected);
        QCOMPARE(placements, 1);
        QCOMPARE(layers, perLayer);
        QVERIFY(QFileInfo::exists(source));

        // Again, over the routed file, one net only: replaced, placed once.
        c.source = c.target = routed;
        c.nets = {"h"};
        report = commitRoutes(c, router.problem(), r);
        QVERIFY2(report.ok, qPrintable(report.error));
        QVERIFY(report.replaced);
        QVERIFY(!report.placed);
        readBack(routed, &polygons, &placements, nullptr);
        QCOMPARE(polygons, int(drawnShapes(router.problem(), r.nets[0], 0).size()));
        QCOMPARE(placements, 1);

        // Taken away.
        c.remove = true;
        report = commitRoutes(c, router.problem(), r);
        QVERIFY(report.ok);
        readBack(routed, &polygons, &placements, nullptr);
        QCOMPARE(polygons, -1);

        // OASIS, by the suffix.
        Commit oas;
        oas.source = source;
        oas.target = dir.filePath("chip_routed.oas");
        QVERIFY(commitRoutes(oas, router.problem(), r).ok);
        gdstk::ErrorCode e = gdstk::ErrorCode::NoError;
        gdstk::Library lib = gdstk::read_oas(QFile::encodeName(oas.target).constData(), 0, 0, &e);
        QVERIFY(lib.get_cell("ROUTES") != nullptr);
        QCOMPARE(int(lib.get_cell("ROUTES")->polygon_array.count), expected);
        lib.free_all();
        // Refused: a .gds.gz, a cell routed into itself.
        Commit gz = c;
        gz.remove = false;
        gz.source = dir.filePath("x.gds.gz");
        QVERIFY(!commitRoutes(gz, router.problem(), r).ok);
        Commit self = c;
        self.remove = false;
        self.topCell = "ROUTES";
        QVERIFY(!commitRoutes(self, router.problem(), r).ok);
    }

    void theTechnologyIsReadKeyByKey()
    {
        QString e;
        Technology t = readTechnology(techJson(), &e);
        QVERIFY2(e.isEmpty(), qPrintable(e));
        QCOMPARE(t.metals.size(), 2);
        QCOMPARE(t.vias[0].landingLower, 4.0);
        QCOMPARE(t.classes[1].width, 6.0);
        QCOMPARE(t.toTerminal, 2.0);
        // Round trip.
        const Technology again = readTechnology(technologyJson(t), &e);
        QVERIFY2(e.isEmpty(), qPrintable(e));
        QCOMPARE(QJsonDocument(technologyJson(again)).toJson(), QJsonDocument(technologyJson(t)).toJson());
        const auto refused = [&](QJsonObject o, const char* word) {
            QString why;
            readTechnology(o, &why);
            return why.contains(QLatin1String(word)) ? QString() : why + QStringLiteral(" [wanted ") + QLatin1String(word) + QLatin1Char(']');
        };
        QJsonObject o = techJson();
        o.insert("grdi", 1);
        QCOMPARE(refused(o, "'grdi'"), QString());
        o = techJson();
        o.remove("spacing");
        QCOMPARE(refused(o, "spacing"), QString());
        o = techJson();
        o.insert("vias", QJsonArray{});
        QCOMPARE(refused(o, "No via joins M1 and M2"), QString());
        o = techJson();
        o.insert("grid", -1);
        QCOMPARE(refused(o, "grid"), QString());
        o = techJson();
        QJsonArray metals = o.value("metals").toArray();
        QJsonObject m2 = metals[1].toObject();
        m2.insert("layer", "1/0");
        metals[1] = m2;
        o.insert("metals", metals);
        QCOMPARE(refused(o, "both on 1/0"), QString());
        o = techJson();
        o.insert("default_class", "nope");
        QCOMPARE(refused(o, "default_class"), QString());
    }

    void netsAreReadFromCsvAndJson()
    {
        QString e;
        QVector<JobNet> nets = readNetsCsv("a, 0, 0, 100, 0\nb, 0, 10, 100, 10, wide\n", &e);
        QVERIFY2(e.isEmpty(), qPrintable(e));
        QCOMPARE(nets.size(), 2);
        QCOMPARE(nets[1].netClass, QStringLiteral("wide"));
        QCOMPARE(nets[0].terminals[1].at, QPointF(100, 0));
        nets = readNetsCsv("net,start_text,pad_text,pad_x,pad_y\nG1,C1,,50,50\nG1,,P2,,\n", &e);
        QVERIFY2(e.isEmpty(), qPrintable(e));
        QCOMPARE(nets.size(), 1);
        QCOMPARE(nets[0].terminals.size(), 3);
        QCOMPARE(nets[0].terminals[0].text, QStringLiteral("C1"));
        QCOMPARE(nets[0].terminals[1].at, QPointF(50, 50));
        QCOMPARE(nets[0].terminals[2].text, QStringLiteral("P2"));
        nets = readNetsCsv("x;S1;P1\n", &e);
        QVERIFY2(e.isEmpty(), qPrintable(e));
        QCOMPARE(nets[0].terminals[0].text, QStringLiteral("S1"));
        readNetsCsv("a\n", &e);
        QVERIFY(!e.isEmpty());
        readNetsCsv("a,0,0,1,1\na,0,0,1,1\n", &e);
        QVERIFY(e.contains("twice"));

        const QJsonObject jobJsonIn{
            {"layout", "chip.gds"},
            {"technology", "tech.json"},
            {"nets", QJsonArray{QJsonObject{{"name", "a"}, {"start", QJsonArray{0, 0}}, {"pads", QJsonArray{QJsonObject{{"text", "P1"}, {"mode", "anywhere"}}}},
                                            {"guide", QJsonArray{QJsonArray{0, 0}, QJsonArray{10, 0}}}}}},
            {"keep_outs", QJsonArray{QJsonObject{{"name", "k"}, {"box", QJsonArray{0, 0, 10, 10}}, {"except_nets", QJsonArray{"a"}}}}},
            {"options", QJsonObject{{"passes", 5}}}};
        const Job job = readJob(jobJsonIn, &e);
        QVERIFY2(e.isEmpty(), qPrintable(e));
        QCOMPARE(job.nets[0].terminals[1].mode, Terminal::Mode::Anywhere);
        QCOMPARE(job.keepOuts[0].outline.size(), 4);
        QCOMPARE(job.options.passes, 5);
        const Job again = readJob(jobJson(job), &e);
        QVERIFY2(e.isEmpty(), qPrintable(e));
        QCOMPARE(QJsonDocument(jobJson(again)).toJson(), QJsonDocument(jobJson(job)).toJson());
        QJsonObject badJob = jobJsonIn;
        badJob.insert("netz", QJsonArray{});
        readJob(badJob, &e);
        QVERIFY(e.contains("'netz'"));
    }

    // Damaged technologies, jobs and CSVs, the same each run: refused or
    // read, never a crash (under the sanitizers on CI). QUCS_ROUTER_FUZZ
    // seconds of it at random.
    void damagedInputIsRefusedNotACrash()
    {
        const QByteArray tech = QJsonDocument(techJson()).toJson();
        QJsonArray onePad;
        onePad.append(QJsonArray{1, 1});   // (built by append: [[1, 1]] on every compiler)
        const QByteArray job = QJsonDocument(QJsonObject{
                                                 {"nets", QJsonArray{QJsonObject{{"name", "a"}, {"start", QJsonArray{0, 0}}, {"pads", onePad}}}},
                                                 {"keep_outs", QJsonArray{QJsonObject{{"outline", QJsonArray{QJsonArray{0, 0}, QJsonArray{1, 0}, QJsonArray{1, 1}}}}}},
                                                 {"options", QJsonObject{{"passes", 3}}}})
                                   .toJson();
        const QByteArray csv = "net,start_x,start_y,pad_x,pad_y,class\na,0,0,10,0,signal\nb,\"1\",2,3,4,\n";
        const qint64 seconds = qEnvironmentVariableIntValue("QUCS_ROUTER_FUZZ");
        QRandomGenerator random(seconds > 0 ? QRandomGenerator::global()->generate() : 20261107u);
        QElapsedTimer timer;
        timer.start();
        const QByteArray tokens[] = {"-1", "1e400", "null", "[]", "{}", "\"\"", "true", "0", "1e9", "\"1/0/0\"", "\"M1\"", "[1,2,3]"};
        int read = 0;
        for (int round = 0; round < 3000 || timer.elapsed() < seconds * 1000; ++round) {
            for (const QByteArray* source : {&tech, &job, &csv}) {
                QByteArray d = *source;
                const int changes = 1 + int(random.bounded(4));
                for (int c = 0; c < changes && !d.isEmpty(); ++c) {
                    const int at = int(random.bounded(d.size()));
                    switch (random.bounded(4)) {
                    case 0: d[at] = char(random.bounded(256)); break;
                    case 1: d.remove(at, int(random.bounded(8))); break;
                    case 2: d.insert(at, tokens[random.bounded(int(std::size(tokens)))]); break;
                    default: d.truncate(at); break;
                    }
                }
                QString e;
                if (source == &csv) {
                    readNetsCsv(QString::fromUtf8(d), &e);
                } else {
                    const QJsonDocument doc = QJsonDocument::fromJson(d);
                    if (!doc.isObject()) continue;
                    if (source == &tech) {
                        const Technology t = readTechnology(doc.object(), &e);
                        if (e.isEmpty()) {
                            ++read;
                            // What reads routes, or is refused by prepare(): never a crash.
                            Problem p;
                            p.tech = t;
                            p.options.threads = 1;
                            p.options.maxMemory = 64;
                            p.options.passes = 2;
                            pad(p, 0, 0, 0);
                            pad(p, 0, 20, 0);
                            p.nets = {net("a", at(p, 0, 0), at(p, 20, 0), std::min<int>(t.defaultClass, int(t.classes.size()) - 1))};
                            Router router(p);
                            if (router.prepare()) router.run();
                        }
                    } else {
                        readJob(doc.object(), &e);
                    }
                }
            }
        }
        QVERIFY(read > 0);
    }

    void padsArePairedByLeastDistance()
    {
        const QVector<Pt> starts = {{0, 0}, {0, 10}, {0, 20}};
        const QVector<Pt> pads = {{100, 21}, {100, 1}, {100, 11}, {100, 500}};
        QCOMPARE(assignPads(starts, pads), (QVector<int>{1, 2, 0}));
        // A pair kept by hand, the rest paired round it.
        QCOMPARE(assignPads(starts, pads, {{0, 0}}), (QVector<int>{0, 1, 2}));
        // More starts than pads: one left without.
        QCOMPARE(assignPads(starts, {{100, 9}}).count(-1), 2);
    }

    void tooMuchMemoryIsRefusedBeforeARun()
    {
        Problem p = problemOf(tech(techJson(2, 2, 0.1)));   // a 0.1 µm grid over 4 mm
        p.options.maxMemory = 16;
        pad(p, 0, 0, 0);
        pad(p, 0, 4000, 0);
        p.nets = {net("a", at(p, 0, 0), at(p, 4000, 0))};
        Router router(p);
        QVERIFY(!router.prepare());
        QVERIFY2(router.error().contains("max_memory_mb"), qPrintable(router.error()));
    }

    // The chip of the improvements note (2026-11-06), rebuilt from its
    // description: 8 x 5 mm, 100 two-metal MoM capacitors in a 10 x 10
    // array, 200 pads round the edge, a net from each plate to its own pad.
    // Line/space and the pairing as QUCS_ROUTER_BENCH says: "10/4",
    // "10/4 random", "20/10 random"... Prints what it took; asserts only that
    // the routes it reports are correct.
    void aLargeChip()
    {
        const QString spec = qEnvironmentVariable("QUCS_ROUTER_BENCH");
        if (spec.isEmpty()) QSKIP("QUCS_ROUTER_BENCH=\"10/4\" (or \"20/10 random\", ...) to run it");
        const QStringList parts = spec.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        const double width = parts.value(0).section('/', 0, 0).toDouble(), space = parts.value(0).section('/', 1, 1).toDouble();
        const bool random = parts.contains("random");
        const double grid = qEnvironmentVariableIsSet("QUCS_ROUTER_GRID") ? qEnvironmentVariable("QUCS_ROUTER_GRID").toDouble() : 2.0;
        QJsonObject o = techJson(width, space, grid);
        o.insert("vias", QJsonArray{QJsonObject{{"name", "V1"}, {"layer", "3/0"}, {"between", QJsonArray{"M1", "M2"}}, {"size", 4}, {"enclosure", 2}, {"spacing", 4}}});
        o.insert("classes", QJsonArray{QJsonObject{{"name", "signal"}, {"width", width}}});
        o.insert("area", QJsonObject{{"box", QJsonArray{0, 0, 8000, 5000}}});
        Problem p = problemOf(tech(o));
        p.options.threads = qEnvironmentVariableIntValue("QUCS_ROUTER_THREADS");
        p.options.maxMemory = 32768;
        if (qEnvironmentVariableIsSet("QUCS_ROUTER_WEIGHT")) p.options.searchWeight = qEnvironmentVariable("QUCS_ROUTER_WEIGHT").toDouble();
        if (qEnvironmentVariableIsSet("QUCS_ROUTER_GROWTH")) p.options.presentGrowth = qEnvironmentVariable("QUCS_ROUTER_GROWTH").toDouble();
        if (qEnvironmentVariableIsSet("QUCS_ROUTER_HISTORY")) p.options.historyGain = qEnvironmentVariable("QUCS_ROUTER_HISTORY").toDouble();
        if (qEnvironmentVariableIsSet("QUCS_ROUTER_PASSES")) p.options.passes = qEnvironmentVariableIntValue("QUCS_ROUTER_PASSES");
        if (qEnvironmentVariableIsSet("QUCS_ROUTER_NOTCH")) p.tech.notch = qEnvironmentVariable("QUCS_ROUTER_NOTCH").toDouble();
        if (qEnvironmentVariable("QUCS_ROUTER_ARRAY") == QLatin1String("2x2")) {
            // The black-box test's array: four 3 µm cuts 2 µm apart, 1 µm enclosed.
            Via& v = p.tech.vias[0];
            v.size = 3;
            v.cols = v.rows = 2;
            v.cutSpacing = 2;
            v.landingLower = v.landingUpper = v.outline() + 2;
        }
        // Capacitors: 200 x 150 µm, buses 12 µm at either side on both
        // metals with a row of cuts, fingers 4 µm wide 4 µm apart, their tips
        // 2 µm from the other bus.
        QVector<Pt> plates;
        for (int cy = 0; cy < 10; ++cy)
            for (int cx = 0; cx < 10; ++cx) {
                const double x0 = 1100 + cx * 600, y0 = 800 + cy * 340;
                for (int m = 0; m < 2; ++m) {
                    p.metal.append({rect(p, x0, y0, x0 + 12, y0 + 150), m});
                    p.metal.append({rect(p, x0 + 188, y0, x0 + 200, y0 + 150), m});
                    for (int f = 0; f < 9; ++f) {
                        const double fy = y0 + 4 + f * 16;
                        p.metal.append({rect(p, x0 + 12, fy, x0 + 186, fy + 4), m});
                        p.metal.append({rect(p, x0 + 14, fy + 8, x0 + 188, fy + 12), m});
                    }
                }
                for (int k = 0; k < 14; ++k) {
                    p.cuts.append({rect(p, x0 + 4, y0 + 5 + k * 10, x0 + 8, y0 + 9 + k * 10), 0});
                    p.cuts.append({rect(p, x0 + 192, y0 + 5 + k * 10, x0 + 196, y0 + 9 + k * 10), 0});
                }
                plates.append({p.units(x0 + 6), p.units(y0 + 75)});
                plates.append({p.units(x0 + 194), p.units(y0 + 75)});
            }
        // Pads: 80 µm on M2, 66 along each long side, 34 along each short one.
        QVector<Pt> pads;
        for (int i = 0; i < 66; ++i) {
            pads.append({p.units(250 + i * 115), p.units(150)});
            pads.append({p.units(250 + i * 115), p.units(4850)});
        }
        for (int i = 0; i < 34; ++i) {
            pads.append({p.units(150), p.units(560 + i * 115)});
            pads.append({p.units(7850), p.units(560 + i * 115)});
        }
        for (const Pt& q : pads) p.metal.append({rect(p, p.um(q.x) - 40, p.um(q.y) - 40, p.um(q.x) + 40, p.um(q.y) + 40), 1});
        QVector<int> pairing;
        if (random) {
            QRandomGenerator g(5112026);
            pairing.resize(200);
            std::iota(pairing.begin(), pairing.end(), 0);
            std::shuffle(pairing.begin(), pairing.end(), g);
        } else {
            pairing = assignPads(plates, pads);
        }
        for (int i = 0; i < 200; ++i) {
            // "anywhere": the line ends where it meets its plate (a 20 µm line
            // cannot reach a 12 µm bus's middle past the other plate's fingers).
            const Terminal::Mode mode = parts.contains("anywhere") ? Terminal::Mode::Anywhere : Terminal::Mode::Centre;
            Net n = net(QStringLiteral("c%1%2").arg(i / 2).arg(i % 2 ? 'b' : 'a'), at(p, p.um(plates[i].x), p.um(plates[i].y), -1, mode),
                        at(p, p.um(pads[pairing[i]].x), p.um(pads[pairing[i]].y)));
            p.nets.append(n);
        }
        double lower = 0;
        for (int i = 0; i < 200; ++i) lower += p.um(std::abs(plates[i].x - pads[pairing[i]].x) + std::abs(plates[i].y - pads[pairing[i]].y));
        QElapsedTimer timer;
        timer.start();
        Router router(p);
        QVERIFY2(router.prepare(), qPrintable(router.error()));
        const double prepared = timer.elapsed() / 1000.0;
        Progress progress;
        progress.pass = [](const PassRow& row) {
            printf("  pass %d%s: routed %d, failed %d, contested %d, %.1f mm, %.2f s\n", row.pass, row.cleanup ? " (clean-up)" : "", row.routed,
                   row.failed, row.contested, row.length / 1000, row.seconds);
            fflush(stdout);
        };
        const Result r = router.run({}, {}, progress);
        const double routed = timer.elapsed() / 1000.0;
        const Verification v = verify(router.problem(), r);
        const double checked = timer.elapsed() / 1000.0;
        // The process's peak: none on Windows, which has no getrusage (0
        // there); ru_maxrss is bytes on macOS and kilobytes on Linux.
        double processPeakMb = 0;
#ifndef _WIN32
        struct rusage use {};
        getrusage(RUSAGE_SELF, &use);
#ifdef __APPLE__
        processPeakMb = double(use.ru_maxrss) / 1048576.0;
#else
        processPeakMb = double(use.ru_maxrss) / 1024.0;
#endif
#endif
        int vias = 0, fills = 0;
        for (const NetResult& n : r.nets) {
            vias += n.vias.size();
            fills += n.fills.size();
        }
        double length = 0;
        for (const NetResult& n : r.nets) length += p.um(n.length);
        QMap<QString, int> kinds;
        for (const Finding& f : v.findings) ++kinds[f.kind];
        printf("%s, grid %g µm, %d threads: prepare %.2f s, route %.2f s (%s, %d passes), check %.2f s; routed %d of 200, %d vias, %.1f mm (%.1f mm "
               "lower bound); %lld states searched; router peak %.0f MB, process peak %.0f MB; findings %d",
               qPrintable(spec), grid, router.problem().options.threads, prepared, routed - prepared, qPrintable(r.stopped), int(r.passes.size()),
               checked - routed, r.routed(), vias, length / 1000, lower / 1000, (long long)r.statesSearched, r.peakBytes / 1048576.0,
               processPeakMb, int(v.findings.size()));
        for (auto it = kinds.begin(); it != kinds.end(); ++it) printf(" %s %d", qPrintable(it.key()), it.value());
        printf("; notch fills %d\n", fills);
        for (const QString& n : r.notes) printf("  note: %s\n", qPrintable(n));
        for (const Crowding& c : r.crowding) printf("  crowding %.0f %%\n", c.use * 100);
        for (int i = 0, shown = 0; i < v.findings.size() && shown < 6; ++i)
            if (v.findings[i].kind != QLatin1String("unroutable") || i < 2) {
                printf("  finding: %s\n", qPrintable(v.findings[i].text));
                ++shown;
            }
        if (qEnvironmentVariableIsSet("QUCS_ROUTER_DUMP")) {
            const NetResult& d = r.nets[qEnvironmentVariableIntValue("QUCS_ROUTER_DUMP")];
            for (const Run& run : d.runs) printf("  run M%d (%g,%g)-(%g,%g)\n", run.metal + 1, p.um(run.a.x), p.um(run.a.y), p.um(run.b.x), p.um(run.b.y));
            for (const ViaPlaced& vp : d.vias) printf("  via (%g,%g)\n", p.um(vp.at.x), p.um(vp.at.y));
        }
        int wrong = 0;
        for (const Finding& f : v.findings) wrong += f.kind != QLatin1String("unroutable");
        QCOMPARE(wrong, 0);
    }

    void cancelledRunKeepsWhatItHas()
    {
        Problem p = problemOf(tech());
        for (int i = 0; i < 6; ++i) {
            pad(p, 0, 0, i * 20);
            pad(p, 0, 200, (5 - i) * 20);
            p.nets.append(net(QStringLiteral("n%1").arg(i), at(p, 0, i * 20), at(p, 200, (5 - i) * 20)));
        }
        Router router(p);
        QVERIFY(router.prepare());
        std::atomic_bool cancel{true};
        Progress progress;
        progress.cancel = &cancel;
        const Result r = router.run({}, {}, progress);
        QCOMPARE(r.stopped, QStringLiteral("cancelled"));
        QCOMPARE(r.nets.size(), 6);
    }
};

QTEST_GUILESS_MAIN(TestLayoutRouter)
#include "test_layout_router.moc"
