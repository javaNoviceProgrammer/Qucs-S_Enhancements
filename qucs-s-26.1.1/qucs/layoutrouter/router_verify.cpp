/*
 * router_verify.cpp - the routes checked on the shapes drawn
 *                     (router_verify.h); booleans by Clipper, which
 *                     gdstk carries
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "router_verify.h"

#include <clipper/clipper.hpp>

#include <QElapsedTimer>
#include <QHash>
#include <QRegularExpression>

#include <map>
#include <numeric>

namespace qucs_s::router {

namespace {

using ClipperLib::Path;
using ClipperLib::Paths;

Path pathOf(const Poly& p)
{
    Path out;
    out.reserve(p.size());
    for (const Pt& q : p) out.push_back(ClipperLib::IntPoint(q.x, q.y));
    return out;
}

Poly polyOfPath(const Path& p)
{
    Poly out;
    out.reserve(p.size());
    for (const ClipperLib::IntPoint& q : p) out.push_back({Coord(q.X), Coord(q.Y)});
    return out;
}

Paths unionOf(const std::vector<Box>& boxes)
{
    ClipperLib::Clipper c;
    for (const Box& b : boxes) c.AddPath(pathOf(polyOf(b)), ClipperLib::ptSubject, true);
    Paths out;
    c.Execute(ClipperLib::ctUnion, out, ClipperLib::pftNonZero, ClipperLib::pftNonZero);
    return out;
}

Paths difference(const Paths& a, const std::vector<const Poly*>& b)
{
    if (b.empty()) return a;
    ClipperLib::Clipper c;
    c.AddPaths(a, ClipperLib::ptSubject, true);
    for (const Poly* q : b) c.AddPath(pathOf(*q), ClipperLib::ptClip, true);
    Paths out;
    c.Execute(ClipperLib::ctDifference, out, ClipperLib::pftNonZero, ClipperLib::pftNonZero);
    return out;
}

double overlapArea(const Poly& a, const Poly& b)
{
    ClipperLib::Clipper c;
    c.AddPath(pathOf(a), ClipperLib::ptSubject, true);
    c.AddPath(pathOf(b), ClipperLib::ptClip, true);
    Paths out;
    c.Execute(ClipperLib::ctIntersection, out, ClipperLib::pftNonZero, ClipperLib::pftNonZero);
    double area = 0;
    for (const Path& q : out) area += ClipperLib::Area(q);
    return std::abs(area);
}

Paths shrunkPaths(const std::vector<Poly>& polys, Coord by)
{
    ClipperLib::Clipper c;
    for (const Poly& q : polys) c.AddPath(pathOf(q), ClipperLib::ptSubject, true);
    Paths joined;
    c.Execute(ClipperLib::ctUnion, joined, ClipperLib::pftNonZero, ClipperLib::pftNonZero);
    if (by <= 0 || joined.empty()) return joined;
    ClipperLib::ClipperOffset o;
    o.AddPaths(joined, ClipperLib::jtMiter, ClipperLib::etClosedPolygon);
    Paths out;
    o.Execute(out, -double(by));
    return out;
}

std::vector<Poly> polysOf(const Paths& paths)
{
    std::vector<Poly> out;
    out.reserve(paths.size());
    for (const Path& q : paths) out.push_back(polyOfPath(q));
    return out;
}

QString micro(double um)
{
    return QString::number(um, 'f', 3).remove(QRegularExpression(QStringLiteral("\\.?0+$")));
}

struct MetalMap {
    std::vector<std::vector<int>> onPlane;     // shape indices
    std::vector<BoxIndex> index;               // their bounds, by plane
    std::vector<Box> bounds;
    std::vector<int> root;                     // piece
    std::vector<int> owner;                    // by piece: the net, or -1
    std::vector<int> padOf;                    // by shape: the net whose terminal is on it, or -1
};

MetalMap readMetal(const Problem& p)
{
    MetalMap m;
    const int planes = int(p.tech.metals.size());
    const int ns = int(p.metal.size());
    m.onPlane.assign(std::size_t(planes), {});
    m.index.assign(std::size_t(planes), BoxIndex());
    m.bounds.resize(std::size_t(ns));
    m.root.resize(std::size_t(ns));
    std::iota(m.root.begin(), m.root.end(), 0);
    const std::function<int(int)> find = [&](int a) {
        while (m.root[std::size_t(a)] != a) a = m.root[std::size_t(a)] = m.root[std::size_t(m.root[std::size_t(a)])];
        return a;
    };
    const auto unite = [&](int a, int b) {
        a = find(a);
        b = find(b);
        if (a != b) m.root[std::size_t(std::max(a, b))] = std::min(a, b);
    };
    for (int s = 0; s < ns; ++s) {
        m.bounds[std::size_t(s)] = boundsOf(p.metal.at(s).outline);
        if (p.metal.at(s).layer >= 0 && p.metal.at(s).layer < planes) m.onPlane[std::size_t(p.metal.at(s).layer)].push_back(s);
    }
    for (int l = 0; l < planes; ++l) {
        std::vector<Box> boxes;
        for (int s : m.onPlane[std::size_t(l)]) boxes.push_back(m.bounds[std::size_t(s)]);
        m.index[std::size_t(l)].build(boxes);
        for (std::size_t i = 0; i < boxes.size(); ++i)
            m.index[std::size_t(l)].query(boxes[i], [&](int j) {
                if (std::size_t(j) <= i) return true;
                const int a = m.onPlane[std::size_t(l)][i], b = m.onPlane[std::size_t(l)][std::size_t(j)];
                if (find(a) != find(b) && polyDistance(p.metal.at(a).outline, p.metal.at(b).outline) == 0) unite(a, b);
                return true;
            });
    }
    for (const Shape& cut : p.cuts) {
        if (cut.layer < 0 || cut.layer >= p.tech.vias.size()) continue;
        const int lo = p.tech.vias.at(cut.layer).lower;
        const Box cb = boundsOf(cut.outline);
        int below = -1, above = -1;
        for (int side = 0; side < 2; ++side) {
            const int l = lo + side;
            m.index[std::size_t(l)].query(cb, [&](int j) {
                const int s = m.onPlane[std::size_t(l)][std::size_t(j)];
                if (polyDistance(cut.outline, p.metal.at(s).outline) == 0) (side ? above : below) = s;
                return (side ? above : below) < 0;
            });
        }
        if (below >= 0 && above >= 0) unite(below, above);
    }
    for (int s = 0; s < ns; ++s) m.root[std::size_t(s)] = find(s);
    m.owner.assign(std::size_t(ns), -1);
    m.padOf.assign(std::size_t(ns), -1);
    // As the router has it: every piece under a terminal's point is its
    // net's; the shapes of the piece the terminal is (its metal's, else the
    // lowest metal's) are its pad.
    for (int n = 0; n < p.nets.size(); ++n)
        for (const Terminal& t : p.nets.at(n).terminals) {
            std::vector<int> under;
            for (int l = 0; l < planes; ++l) {
                if (t.metal >= 0 && t.metal != l) continue;
                m.index[std::size_t(l)].query(Box{t.at.x, t.at.y, t.at.x, t.at.y}, [&](int j) {
                    const int s = m.onPlane[std::size_t(l)][std::size_t(j)];
                    if (inside(p.metal.at(s).outline, t.at)) under.push_back(s);
                    return true;
                });
            }
            for (int s : under) {
                if (m.owner[std::size_t(m.root[std::size_t(s)])] < 0) m.owner[std::size_t(m.root[std::size_t(s)])] = n;
                if (m.root[std::size_t(s)] == m.root[std::size_t(under.front())]) m.padOf[std::size_t(s)] = n;
            }
        }
    return m;
}

int ownerOf(const MetalMap& m, int shape)
{
    return m.owner[std::size_t(m.root[std::size_t(shape)])];
}

// The design rule of \a type a value comes from, by name: the one on \a layer
// (a metal's or a via's name, or a via's layer) whose minimum it is; empty
// when none is (the value is the technology's own).
QString ruleOf(const Technology& t, const QString& type, const QStringList& layers, double value)
{
    for (const DesignRule& r : t.rules)
        if (r.type == type && r.other.isEmpty() == (type != QLatin1String("enclosure") && type != QLatin1String("inclusion")) && std::abs(r.min - value) <= 1e-9
            && layers.contains(r.layer, Qt::CaseInsensitive))
            return r.name;
    return {};
}

// What a rule may call a via: its cut layer, as "3/0" or by the name of any
// via on it.
QStringList namesOf(const Technology& t, const Via& v)
{
    QStringList out{v.layer.text()};
    for (const Via& o : t.vias)
        if (o.layer == v.layer) out << o.name;
    return out;
}

// A metal's spacing to other metal there: the rule's, else the technology's \a base.
Coord spacingOn(const Problem& p, int l, Coord base)
{
    return l < p.tech.metalSpacing.size() ? std::max(base, p.units(p.tech.metalSpacing.at(l))) : base;
}

// Each inclusion rule's region, shrunk by its minimum, as Clipper paths.
std::vector<Paths> regionsOf(const Problem& p)
{
    std::vector<Paths> out;
    for (int r = 0; r < p.tech.inclusions.size(); ++r) {
        std::vector<Poly> in;
        for (const Shape& sh : p.regions)
            if (sh.layer == r) in.push_back(sh.outline);
        out.push_back(shrunkPaths(in, p.units(p.tech.inclusions.at(r).min)));
    }
    return out;
}

// What of \a q lies outside \a region (each piece with some area).
Paths outsidePart(const Poly& q, const Paths& region)
{
    ClipperLib::Clipper c;
    c.AddPath(pathOf(q), ClipperLib::ptSubject, true);
    c.AddPaths(region, ClipperLib::ptClip, true);
    Paths out;
    c.Execute(ClipperLib::ctDifference, out, ClipperLib::pftNonZero, ClipperLib::pftNonZero);
    out.erase(std::remove_if(out.begin(), out.end(), [](const Path& x) { return std::abs(ClipperLib::Area(x)) <= 1; }), out.end());
    return out;
}

// A net's drawn metal: its runs, and each via's landings, by plane; its cuts.
void drawn(const Problem& p, const NetResult& r, int net, std::vector<std::vector<Box>>* byPlane, std::vector<std::pair<int, Box>>* cuts,
           std::vector<std::pair<int, Box>>* landings)
{
    const int planes = int(p.tech.metals.size());
    byPlane->assign(std::size_t(planes), {});
    const double w = p.tech.classes.at(p.nets.at(net).netClass).width;
    for (const Run& run : r.runs)
        if (run.metal >= 0 && run.metal < planes) (*byPlane)[std::size_t(run.metal)].push_back(run.box());
    for (const ViaPlaced& v : r.vias) {
        if (v.via < 0 || v.via >= p.tech.vias.size()) continue;
        const Via& via = p.tech.vias.at(v.via);
        const Coord lo = p.units(p.tech.landing(v.via, via.lower, w)), up = p.units(p.tech.landing(v.via, via.lower + 1, w));
        // The cut, or an array's outline: what vias keep 'spacing' between.
        const Coord cw = p.units(via.outlineWidth()), ch = p.units(via.outlineHeight());
        const Box bl{v.at.x - lo / 2, v.at.y - lo / 2, v.at.x - lo / 2 + lo, v.at.y - lo / 2 + lo};
        const Box bu{v.at.x - up / 2, v.at.y - up / 2, v.at.x - up / 2 + up, v.at.y - up / 2 + up};
        (*byPlane)[std::size_t(via.lower)].push_back(bl);
        (*byPlane)[std::size_t(via.lower + 1)].push_back(bu);
        if (landings) {
            landings->push_back({via.lower, bl});
            landings->push_back({via.lower + 1, bu});
        }
        if (cuts) cuts->push_back({v.via, Box{v.at.x - cw / 2, v.at.y - ch / 2, v.at.x - cw / 2 + cw, v.at.y - ch / 2 + ch}});
    }
}

QVector<QVector<Poly>> newMetalWith(const Problem& p, const MetalMap& m, const NetResult& r, int net, const std::vector<std::vector<Box>>& byPlane)
{
    const int planes = int(p.tech.metals.size());
    QVector<QVector<Poly>> out(planes);
    for (int l = 0; l < planes; ++l) {
        const auto& boxes = byPlane[std::size_t(l)];
        if (boxes.empty()) continue;
        Box all;
        for (const Box& b : boxes) all = all.united(b);
        Paths drawnHere = unionOf(boxes);
        bool filled = false;
        for (const auto& [metal, poly] : r.fills)
            if (metal == l) {
                drawnHere.push_back(pathOf(poly));
                all = all.united(boundsOf(poly));
                filled = true;
            }
        if (filled) {
            ClipperLib::Clipper c;
            c.AddPaths(drawnHere, ClipperLib::ptSubject, true);
            drawnHere.clear();
            c.Execute(ClipperLib::ctUnion, drawnHere, ClipperLib::pftNonZero, ClipperLib::pftNonZero);
        }
        std::vector<const Poly*> own;
        m.index[std::size_t(l)].query(all, [&](int j) {
            const int s = m.onPlane[std::size_t(l)][std::size_t(j)];
            if (ownerOf(m, s) == net) own.push_back(&p.metal.at(s).outline);
            return true;
        });
        for (const Path& q : difference(drawnHere, own)) {
            if (std::abs(ClipperLib::Area(q)) <= 0) continue;
            out[l].append(polyOfPath(q));
        }
    }
    return out;
}

// The slots narrower than \a notch between a net's lines and its own metal
// (and its other lines) on a plane: what closing their union by half the
// rule fills, the pieces that touch its new lines. A Manhattan closing (miter
// joins): an inner corner is not a slot.
std::vector<Poly> slotsOf(const Problem& p, const MetalMap& m, int net, int l, const std::vector<Box>& boxes, const QVector<QPair<int, Poly>>& fills,
                          Coord notch)
{
    std::vector<Poly> out;
    if (boxes.empty() || notch <= 0) return out;
    Box all;
    for (const Box& b : boxes) all = all.united(b);
    ClipperLib::Clipper u;
    for (const Box& b : boxes) u.AddPath(pathOf(polyOf(b)), ClipperLib::ptSubject, true);
    for (const auto& [metal, poly] : fills)
        if (metal == l) u.AddPath(pathOf(poly), ClipperLib::ptSubject, true);
    m.index[std::size_t(l)].query(all.grown(notch), [&](int j) {
        const int s = m.onPlane[std::size_t(l)][std::size_t(j)];
        if (ownerOf(m, s) == net) u.AddPath(pathOf(p.metal.at(s).outline), ClipperLib::ptSubject, true);
        return true;
    });
    Paths a;
    u.Execute(ClipperLib::ctUnion, a, ClipperLib::pftNonZero, ClipperLib::pftNonZero);
    const double h = double(notch) / 2;
    ClipperLib::ClipperOffset grow(2.0), shrink(2.0);
    grow.AddPaths(a, ClipperLib::jtMiter, ClipperLib::etClosedPolygon);
    Paths grown, closed;
    grow.Execute(grown, h);
    shrink.AddPaths(grown, ClipperLib::jtMiter, ClipperLib::etClosedPolygon);
    shrink.Execute(closed, -h);
    ClipperLib::Clipper d;
    d.AddPaths(closed, ClipperLib::ptSubject, true);
    d.AddPaths(a, ClipperLib::ptClip, true);
    Paths gaps;
    d.Execute(ClipperLib::ctDifference, gaps, ClipperLib::pftNonZero, ClipperLib::pftNonZero);
    for (const Path& q : gaps) {
        if (ClipperLib::Area(q) <= 0) continue;   // (a hole, or nothing)
        const Poly poly = polyOfPath(q);
        const Box qb = boundsOf(poly);
        bool touches = false;
        for (const Box& b : boxes) touches = touches || (b.meets(qb) && polyDistance(polyOf(b), poly) == 0);
        if (touches) out.push_back(poly);
    }
    return out;
}

// Whether metal \a q of \a net on plane \a l keeps clear of everything not
// its own: other metal, other nets' \a lines, obstacles and keep-outs.
bool clearOfOthers(const Problem& p, const MetalMap& m, int net, int l, const Poly& q,
                   const std::vector<std::vector<std::pair<int, Poly>>>& lines, const std::vector<Paths>& regions)
{
    const Box qb = boundsOf(q);
    for (int r = 0; r < p.tech.inclusions.size(); ++r)
        if (p.tech.inclusions.at(r).metal == l && !outsidePart(q, regions[std::size_t(r)]).empty()) return false;
    const Coord toMetal = spacingOn(p, l, p.units(p.tech.toMetal)), toTerminal = spacingOn(p, l, p.units(p.tech.toTerminal));
    bool ok = true;
    m.index[std::size_t(l)].query(qb.grown(std::max(toMetal, toTerminal) + 1), [&](int j) {
        const int s = m.onPlane[std::size_t(l)][std::size_t(j)];
        if (ownerOf(m, s) == net) return true;
        const Coord need = m.padOf[std::size_t(s)] >= 0 ? toTerminal : toMetal;
        if (polyDistance(q, p.metal.at(s).outline) < double(std::max<Coord>(need, 1))) ok = false;
        return ok;
    });
    if (!ok) return false;
    const NetClass& mine = p.tech.classes.at(p.nets.at(net).netClass);
    for (const auto& [other, poly] : lines[std::size_t(l)]) {
        if (other == net) continue;
        const NetClass& theirs = p.tech.classes.at(p.nets.at(other).netClass);
        const Coord need = spacingOn(p, l, p.units(std::max({mine.spacing, theirs.spacing, p.tech.sameLayer})));
        if (boxDistance2(qb, boundsOf(poly)) >= double(need) * double(need)) continue;
        if (polyDistance(q, poly) < double(need)) return false;
    }
    for (const Shape& o : p.obstacles) {
        if (o.layer < 0 || o.layer >= p.tech.obstacles.size() || !p.tech.obstacles.at(o.layer).metals.contains(l)) continue;
        const Coord c = p.units(p.tech.obstacles.at(o.layer).clearance);
        if (c > 0 ? polyDistance(q, o.outline) < double(c) : overlapArea(q, o.outline) > 0) return false;
    }
    for (const KeepOut& k : p.keepOuts) {
        if (!k.metals.isEmpty() && !k.metals.contains(l)) continue;
        if (overlapArea(q, k.outline) > 0) return false;   // a fill never goes into one
    }
    return true;
}

} // namespace

void fillNotches(const Problem& p, Result* result)
{
    for (NetResult& r : result->nets) r.fills.clear();
    const int planes = int(p.tech.metals.size());
    bool any = false;
    for (int l = 0; l < planes; ++l) any = any || p.tech.notchOn(l) > 0;
    if (!any) return;
    const MetalMap m = readMetal(p);
    const std::vector<Paths> regions = regionsOf(p);
    // Every net's lines, by plane, as the fills are added.
    std::vector<std::vector<std::pair<int, Poly>>> lines(static_cast<std::size_t>(planes));
    std::vector<std::vector<std::vector<Box>>> drawnOf(static_cast<std::size_t>(result->nets.size()));
    for (int n = 0; n < result->nets.size() && n < p.nets.size(); ++n) {
        const NetResult& r = result->nets.at(n);
        if (r.status != NetResult::Status::Routed && r.status != NetResult::Status::Kept) continue;
        drawn(p, r, n, &drawnOf[std::size_t(n)], nullptr, nullptr);
        for (int l = 0; l < planes; ++l)
            for (const Box& b : drawnOf[std::size_t(n)][std::size_t(l)]) lines[std::size_t(l)].push_back({n, polyOf(b)});
    }
    for (int n = 0; n < result->nets.size() && n < p.nets.size(); ++n) {
        if (drawnOf[std::size_t(n)].empty()) continue;
        NetResult& r = result->nets[n];
        for (int l = 0; l < planes; ++l)
            for (const Poly& q : slotsOf(p, m, n, l, drawnOf[std::size_t(n)][std::size_t(l)], r.fills, p.units(p.tech.notchOn(l)))) {
                if (!clearOfOthers(p, m, n, l, q, lines, regions)) continue;
                r.fills.append({l, q});
                lines[std::size_t(l)].push_back({n, q});
            }
    }
}

std::vector<Poly> shrunkUnion(const std::vector<Poly>& polys, Coord by)
{
    return polysOf(shrunkPaths(polys, by));
}

std::vector<Poly> outsideOf(const Box& area, const std::vector<Poly>& in, Coord by)
{
    ClipperLib::Clipper c;
    c.AddPath(pathOf(polyOf(area)), ClipperLib::ptSubject, true);
    c.AddPaths(shrunkPaths(in, by), ClipperLib::ptClip, true);
    Paths out;
    c.Execute(ClipperLib::ctDifference, out, ClipperLib::pftNonZero, ClipperLib::pftNonZero);
    return polysOf(out);
}

QVector<QVector<Poly>> newMetal(const Problem& problem, const NetResult& route, int net)
{
    if (!problem.oriented()) {
        Problem turned = problem;
        turned.orient();
        return newMetal(turned, route, net);
    }
    const MetalMap m = readMetal(problem);
    std::vector<std::vector<Box>> byPlane;
    drawn(problem, route, net, &byPlane, nullptr, nullptr);
    return newMetalWith(problem, m, route, net, byPlane);
}

Verification verify(const Problem& problem, const Result& result)
{
    QElapsedTimer timer;
    timer.start();
    // (shapes clockwise turned: Clipper's nonzero unions would cancel them
    // against the counterclockwise boxes drawn over them)
    Problem turned;
    const bool asIs = problem.oriented();
    if (!asIs) {
        turned = problem;
        turned.orient();
    }
    const Problem& p = asIs ? problem : turned;
    Verification v;
    const MetalMap m = readMetal(p);
    const int planes = int(p.tech.metals.size());
    const double tol = 0.5;   // units: a distance this short of the rule is the rule
    const auto um = [&](double u) { return u * p.unit; };
    const auto add = [&](Finding f) {
        if (v.findings.size() < 5000) v.findings.append(f);
    };
    const auto metalName = [&](int l) { return l >= 0 && l < planes ? p.tech.metals.at(l).name : QString(); };
    const auto at = [&](Pt q) { return QStringLiteral("(%1, %2)").arg(micro(um(double(q.x))), micro(um(double(q.y)))); };

    struct Piece {
        int net;
        int plane;
        Poly poly;
        Box bounds;
    };
    const std::vector<Paths> regions = regionsOf(p);
    std::vector<Piece> pieces;                  // every net's new metal
    std::vector<std::pair<int, std::pair<int, Box>>> cuts;   // net, via, cut
    for (int n = 0; n < result.nets.size() && n < p.nets.size(); ++n) {
        const NetResult& r = result.nets.at(n);
        const Net& net = p.nets.at(n);
        if (r.status == NetResult::Status::Failed) {
            Finding f;
            f.kind = QStringLiteral("unroutable");
            f.net = net.name;
            f.at = r.hasWhere ? r.where : net.terminals.first().at;
            f.box = Box{f.at.x, f.at.y, f.at.x, f.at.y}.grown(p.units(p.tech.grid) * 10);
            f.text = QStringLiteral("%1 is not routed: %2").arg(net.name, r.reason);
            add(f);
            continue;
        }
        if (r.status != NetResult::Status::Routed && r.status != NetResult::Status::Kept) continue;
        ++v.netsChecked;
        std::vector<std::vector<Box>> byPlane;
        std::vector<std::pair<int, Box>> netCuts, landings;
        drawn(p, r, n, &byPlane, &netCuts, &landings);
        for (const auto& c : netCuts) cuts.push_back({n, c});

        // Opens: the net's drawn boxes, its own pieces of metal and its
        // terminals, joined where they touch.
        std::vector<std::pair<int, Box>> boxes;
        for (int l = 0; l < planes; ++l)
            for (const Box& b : byPlane[std::size_t(l)]) boxes.push_back({l, b});
        std::map<int, int> pieceNode;           // piece root -> node
        const int nb = int(boxes.size());
        std::vector<int> parent(static_cast<std::size_t>(nb));
        std::iota(parent.begin(), parent.end(), 0);
        const auto node = [&](int root) {
            const auto it = pieceNode.find(root);
            if (it != pieceNode.end()) return it->second;
            const int id = int(parent.size());
            parent.push_back(id);
            pieceNode[root] = id;
            return id;
        };
        const std::function<int(int)> find = [&](int a) {
            while (parent[std::size_t(a)] != a) a = parent[std::size_t(a)] = parent[std::size_t(parent[std::size_t(a)])];
            return a;
        };
        const auto unite = [&](int a, int b) {
            a = find(a);
            b = find(b);
            if (a != b) parent[std::size_t(std::max(a, b))] = std::min(a, b);
        };
        for (int i = 0; i < nb; ++i)
            for (int j = i + 1; j < nb; ++j)
                if (boxes[std::size_t(i)].first == boxes[std::size_t(j)].first && boxes[std::size_t(i)].second.joins(boxes[std::size_t(j)].second))
                    unite(i, j);
        // A via's two landings are one.
        for (const ViaPlaced& vp : r.vias) {
            int lo = -1, up = -1;
            if (vp.via < 0 || vp.via >= p.tech.vias.size()) continue;
            for (int i = 0; i < nb; ++i) {
                if (!boxes[std::size_t(i)].second.contains(vp.at)) continue;
                if (boxes[std::size_t(i)].first == p.tech.vias.at(vp.via).lower) lo = i;
                if (boxes[std::size_t(i)].first == p.tech.vias.at(vp.via).lower + 1) up = i;
            }
            if (lo >= 0 && up >= 0) unite(lo, up);
        }
        for (int i = 0; i < nb; ++i) {
            const int l = boxes[std::size_t(i)].first;
            const Poly bp = polyOf(boxes[std::size_t(i)].second);
            m.index[std::size_t(l)].query(boxes[std::size_t(i)].second, [&](int j) {
                const int s = m.onPlane[std::size_t(l)][std::size_t(j)];
                if (ownerOf(m, s) == n && polyDistance(bp, p.metal.at(s).outline) == 0) unite(i, node(m.root[std::size_t(s)]));
                return true;
            });
        }
        std::vector<int> terminalNode;
        for (const Terminal& t : net.terminals) {
            // The piece of metal under its point on its metal, else on the
            // lowest metal there - as the router reads it. Metal over it with
            // no cut is not joined to it, so a line ending there is an open.
            int found = -1;
            for (int l = 0; l < planes && found < 0; ++l) {
                if (t.metal >= 0 && t.metal != l) continue;
                m.index[std::size_t(l)].query(Box{t.at.x, t.at.y, t.at.x, t.at.y}, [&](int j) {
                    const int s = m.onPlane[std::size_t(l)][std::size_t(j)];
                    if (inside(p.metal.at(s).outline, t.at) && ownerOf(m, s) == n) found = node(m.root[std::size_t(s)]);
                    return found < 0;
                });
            }
            if (found < 0)
                for (int i = 0; i < nb && found < 0; ++i)
                    if ((t.metal < 0 || t.metal == boxes[std::size_t(i)].first) && boxes[std::size_t(i)].second.contains(t.at)) found = i;
            terminalNode.push_back(found);
        }
        for (int t = 1; t < int(terminalNode.size()); ++t) {
            if (terminalNode[std::size_t(t)] >= 0 && terminalNode[0] >= 0 && find(terminalNode[std::size_t(t)]) == find(terminalNode[0])) continue;
            Finding f;
            f.kind = QStringLiteral("open");
            f.net = net.name;
            const bool startLost = terminalNode[0] < 0;
            f.at = startLost ? net.terminals.first().at : net.terminals.at(t).at;
            f.box = Box{f.at.x, f.at.y, f.at.x, f.at.y}.grown(p.units(p.tech.grid) * 10);
            f.text = startLost ? QStringLiteral("%1: its lines do not reach its start at %2").arg(net.name, at(f.at))
                               : QStringLiteral("%1: pad %2 at %3 is not joined to its start").arg(net.name).arg(t).arg(at(f.at));
            add(f);
            if (startLost) break;
        }

        // Its new metal, for the spacing checks.
        const QVector<QVector<Poly>> fresh = newMetalWith(p, m, r, n, byPlane);
        for (int l = 0; l < planes; ++l)
            for (const Poly& q : fresh.at(l)) pieces.push_back({n, l, q, boundsOf(q)});

        // Obstacles (a landing on its own contact excused where the rule allows).
        for (int o = 0; o < p.obstacles.size(); ++o) {
            const Shape& sh = p.obstacles.at(o);
            if (sh.layer < 0 || sh.layer >= p.tech.obstacles.size()) continue;
            const ObstacleRule& rule = p.tech.obstacles.at(sh.layer);
            const Coord clearance = p.units(rule.clearance);
            const Box ob = boundsOf(sh.outline);
            for (int l : rule.metals) {
                for (const Poly& q : fresh.at(l)) {
                    if (boxDistance2(boundsOf(q), ob) >= double(clearance) * double(clearance) && clearance > 0) continue;
                    if (clearance <= 0 && !boundsOf(q).overlaps(ob)) continue;
                    Poly piece = q;
                    if (rule.landings) {
                        // the part of it in a landing on its own contact is excused
                        std::vector<Box> mine;
                        for (const auto& [ll, b] : landings) {
                            if (ll != l) continue;
                            bool onOwn = false;
                            m.index[std::size_t(l)].query(Box{b.centre().x, b.centre().y, b.centre().x, b.centre().y}, [&](int j) {
                                const int s = m.onPlane[std::size_t(l)][std::size_t(j)];
                                onOwn = onOwn || (ownerOf(m, s) == n && inside(p.metal.at(s).outline, b.centre()));
                                return !onOwn;
                            });
                            if (onOwn) mine.push_back(b);
                        }
                        if (!mine.empty()) {
                            std::vector<Poly> polys;
                            for (const Box& b : mine) polys.push_back(polyOf(b));
                            std::vector<const Poly*> refs;
                            for (const Poly& pp : polys) refs.push_back(&pp);
                            const Paths left = difference(Paths{pathOf(q)}, refs);
                            if (left.empty()) continue;
                            piece = polyOfPath(left.front());
                        }
                    }
                    Pt where;
                    const double d = polyDistance(piece, sh.outline, &where);
                    const bool bad = clearance > 0 ? d < double(clearance) - tol : overlapArea(piece, sh.outline) > 0;
                    if (!bad) continue;
                    Finding f;
                    f.kind = QStringLiteral("obstacle");
                    f.net = net.name;
                    f.other = rule.name;
                    for (const DesignRule& d : p.tech.rules)
                        if (d.name == rule.name && (d.type == QLatin1String("exclusion") || d.type == QLatin1String("spacing"))) f.rule = d.name;
                    f.metal = l;
                    f.at = where;
                    f.box = boundsOf(piece).united(ob).intersected(Box{where.x, where.y, where.x, where.y}.grown(std::max<Coord>(clearance * 4, p.units(20))));
                    f.measured = um(d);
                    f.required = rule.clearance;
                    f.text = clearance > 0 ? QStringLiteral("%1 on %2 is %3 µm from the obstacle %4 at %5 (%6 µm needed)")
                                                 .arg(net.name, metalName(l), micro(f.measured), rule.name, at(where), micro(f.required))
                                           : QStringLiteral("%1 on %2 runs over the obstacle %3 at %4").arg(net.name, metalName(l), rule.name, at(where));
                    add(f);
                }
            }
        }

        // Notches: slots under the rule between its lines and its own metal
        // that are still open (no fill fitted). One with metal of another in
        // it is no notch: the spacing checks have it.
        for (int l = 0; l < planes; ++l)
            for (const Poly& q : slotsOf(p, m, n, l, byPlane[std::size_t(l)], r.fills, p.units(p.tech.notchOn(l)))) {
                bool foreign = false;
                m.index[std::size_t(l)].query(boundsOf(q), [&](int j) {
                    const int s = m.onPlane[std::size_t(l)][std::size_t(j)];
                    foreign = foreign || (ownerOf(m, s) != n && overlapArea(q, p.metal.at(s).outline) > 0);
                    return !foreign;
                });
                if (foreign) continue;
                const Box b = boundsOf(q);
                Finding f;
                f.kind = QStringLiteral("notch");
                f.net = net.name;
                f.metal = l;
                f.box = b;
                f.at = b.centre();
                f.measured = um(double(std::min(b.width(), b.height())));
                f.required = p.tech.notchOn(l);
                f.rule = ruleOf(p.tech, QStringLiteral("notch"), {metalName(l)}, f.required);
                if (f.rule.isEmpty()) f.rule = ruleOf(p.tech, QStringLiteral("spacing"), {metalName(l)}, f.required);
                f.text = QStringLiteral("%1 on %2 leaves a slot %3 µm wide against its own metal at %4 (the notch rule%5 %6 µm), and no fill fits there")
                             .arg(net.name, metalName(l), micro(f.measured), at(f.at), f.rule.isEmpty() ? QString() : QLatin1Char(' ') + f.rule,
                                  micro(f.required));
                add(f);
            }

        // The rest of the design rules, on the net's metal as drawn: its
        // lines and landings with its own metal they touch, by metal.
        for (int l = 0; l < planes; ++l) {
            if (byPlane[std::size_t(l)].empty() && fresh.at(l).isEmpty()) continue;
            const double minWidth = l < p.tech.minWidth.size() ? p.tech.minWidth.at(l) : 0;
            const double minArea = l < p.tech.minArea.size() ? p.tech.minArea.at(l) : 0;
            Paths merged;
            if (minWidth > 0 || minArea > 0) {
                Box all;
                ClipperLib::Clipper u;
                for (const Box& b : byPlane[std::size_t(l)]) {
                    u.AddPath(pathOf(polyOf(b)), ClipperLib::ptSubject, true);
                    all = all.united(b);
                }
                for (const auto& [metal, poly] : r.fills)
                    if (metal == l) u.AddPath(pathOf(poly), ClipperLib::ptSubject, true);
                m.index[std::size_t(l)].query(all, [&](int j) {
                    const int s = m.onPlane[std::size_t(l)][std::size_t(j)];
                    if (ownerOf(m, s) == n) u.AddPath(pathOf(p.metal.at(s).outline), ClipperLib::ptSubject, true);
                    return true;
                });
                u.Execute(ClipperLib::ctUnion, merged, ClipperLib::pftNonZero, ClipperLib::pftNonZero);
            }
            // Width: what an opening by the rule (a square of its side rolled
            // inside) leaves out, where it is new metal.
            if (minWidth > 0) {
                // (in quarter units, eroded by a quarter unit less than half the
                // rule: a width of the rule, or a quarter unit short of it,
                // passes; half a unit short is found)
                Paths fine = merged;
                for (Path& x : fine)
                    for (ClipperLib::IntPoint& q : x) q = ClipperLib::IntPoint(q.X * 4, q.Y * 4);
                const double h = double(2 * p.units(minWidth) - 1);
                ClipperLib::ClipperOffset in(2.0), out(2.0);
                in.AddPaths(fine, ClipperLib::jtMiter, ClipperLib::etClosedPolygon);
                Paths eroded, opened;
                in.Execute(eroded, -h);
                out.AddPaths(eroded, ClipperLib::jtMiter, ClipperLib::etClosedPolygon);
                out.Execute(opened, h);
                // New metal no square of the rule's side inside the net's
                // metal covers (its own metal narrow already is not its doing).
                ClipperLib::Clipper d;
                for (const Poly& q : fresh.at(l)) {
                    Path x = pathOf(q);
                    for (ClipperLib::IntPoint& pt : x) pt = ClipperLib::IntPoint(pt.X * 4, pt.Y * 4);
                    d.AddPath(x, ClipperLib::ptSubject, true);
                }
                d.AddPaths(opened, ClipperLib::ptClip, true);
                Paths narrow;
                d.Execute(ClipperLib::ctDifference, narrow, ClipperLib::pftNonZero, ClipperLib::pftNonZero);
                for (Path& x : narrow)
                    for (ClipperLib::IntPoint& q : x) q = ClipperLib::IntPoint(q.X / 4, q.Y / 4);
                bool told = false;
                for (const Path& x : narrow) {
                    if (told || ClipperLib::Area(x) <= 0) continue;
                    const Poly xp = polyOfPath(x);
                    const Box xb = boundsOf(xp);
                    if (std::min(xb.width(), xb.height()) <= 2) continue;   // (rounding)
                    told = true;
                    Finding f;
                    f.kind = QStringLiteral("width");
                    f.net = net.name;
                    f.metal = l;
                    f.box = xb.grown(p.units(minWidth));
                    f.at = xb.centre();
                    f.measured = um(double(std::min(xb.width(), xb.height())));
                    f.required = minWidth;
                    f.rule = ruleOf(p.tech, QStringLiteral("width"), {metalName(l)}, minWidth);
                    f.text = QStringLiteral("%1 on %2 is narrower than %3 µm at %4 (the width rule %5)")
                                 .arg(net.name, metalName(l), micro(minWidth), at(f.at), f.rule);
                    add(f);
                }
            }
            // Area: each piece of the net's metal there that has new metal in it.
            if (minArea > 0) {
                const double need = minArea / (p.unit * p.unit);
                double area = 0;
                Box pieceBox;
                std::vector<std::pair<double, Box>> outers;
                for (const Path& x : merged) {
                    const double a = ClipperLib::Area(x);
                    if (a > 0) outers.push_back({a, boundsOf(polyOfPath(x))});
                }
                for (const Path& x : merged) {
                    const double a = ClipperLib::Area(x);
                    if (a >= 0) continue;   // a hole: its piece's area less it
                    const Box hb = boundsOf(polyOfPath(x));
                    for (auto& [oa, ob] : outers)
                        if (ob.contains(hb)) {
                            oa += a;
                            break;
                        }
                }
                for (const auto& [oa, ob] : outers) {
                    if (oa >= need * (1 - 1e-6)) continue;
                    bool isNew = false;
                    for (const Poly& q : fresh.at(l)) isNew = isNew || ob.contains(boundsOf(q));
                    if (!isNew) continue;
                    area = oa;
                    pieceBox = ob;
                    Finding f;
                    f.kind = QStringLiteral("area");
                    f.net = net.name;
                    f.metal = l;
                    f.box = pieceBox.grown(p.units(std::sqrt(minArea)));
                    f.at = pieceBox.centre();
                    f.measured = area * p.unit * p.unit;
                    f.required = minArea;
                    f.rule = ruleOf(p.tech, QStringLiteral("area"), {metalName(l)}, minArea);
                    f.text = QStringLiteral("%1's metal on %2 at %3 is %4 µm², less than the area rule %5's %6 µm²")
                                 .arg(net.name, metalName(l), at(f.at), micro(f.measured), f.rule, micro(minArea));
                    add(f);
                }
            }
            // Inclusion: its new metal inside each region the rules keep it in.
            for (int ri = 0; ri < p.tech.inclusions.size(); ++ri) {
                const InclusionRule& rule = p.tech.inclusions.at(ri);
                if (rule.metal != l) continue;
                for (const Poly& q : fresh.at(l)) {
                    const Paths outside = outsidePart(q, regions[std::size_t(ri)]);
                    if (outside.empty()) continue;
                    Box ob;
                    double area = 0;
                    for (const Path& x : outside) {
                        ob = ob.united(boundsOf(polyOfPath(x)));
                        area += std::abs(ClipperLib::Area(x));
                    }
                    Finding f;
                    f.kind = QStringLiteral("inclusion");
                    f.rule = rule.name;
                    f.net = net.name;
                    f.other = rule.of.text();
                    f.metal = l;
                    f.box = ob.grown(p.units(std::max(rule.min, p.tech.grid)) * 2);
                    f.at = ob.centre();
                    f.measured = area * p.unit * p.unit;
                    f.required = rule.min;
                    f.text = QStringLiteral("%1 on %2 leaves the region the rule %3 keeps it in (inside %4 by %5 µm) near %6, %7 µm² of it")
                                 .arg(net.name, metalName(l), rule.name, f.other, micro(rule.min), at(f.at), micro(f.measured));
                    add(f);
                    break;
                }
            }
        }
        // Enclosure: each cut the rules name inside the net's metal on that
        // side (its landing, line and own metal) by the minimum.
        for (const DesignRule& rule : p.tech.rules) {
            if (rule.type != QLatin1String("enclosure")) continue;
            const int l = p.tech.metalIndex(rule.other);
            if (l < 0) continue;
            for (int ci = 0; ci < int(netCuts.size()); ++ci) {
                const Via& via = p.tech.vias.at(netCuts[std::size_t(ci)].first);
                if (!namesOf(p.tech, via).contains(rule.layer, Qt::CaseInsensitive)) continue;
                const Box need = netCuts[std::size_t(ci)].second.grown(p.units(rule.min));
                std::vector<Box> around;
                for (const Box& b : byPlane[std::size_t(l)])
                    if (b.overlaps(need)) around.push_back(b);
                std::vector<const Poly*> covers;
                std::vector<Poly> boxes;
                boxes.reserve(around.size());
                for (const Box& b : around) boxes.push_back(polyOf(b));
                for (const Poly& b : boxes) covers.push_back(&b);
                m.index[std::size_t(l)].query(need, [&](int j) {
                    const int s = m.onPlane[std::size_t(l)][std::size_t(j)];
                    if (ownerOf(m, s) == n) covers.push_back(&p.metal.at(s).outline);
                    return true;
                });
                const Paths left = difference(Paths{pathOf(polyOf(need))}, covers);
                double area = 0;
                for (const Path& x : left) area += std::abs(ClipperLib::Area(x));
                if (area <= double(p.units(0.001)) * double(need.width())) continue;
                Finding f;
                f.kind = QStringLiteral("enclosure");
                f.rule = rule.name;
                f.net = net.name;
                f.other = via.name;
                f.metal = l;
                f.box = need.grown(p.units(rule.min) * 2);
                f.at = need.centre();
                f.required = rule.min;
                f.text = QStringLiteral("%1's %2 at %3 is not enclosed by %4 µm of %5 (the enclosure rule %6)")
                             .arg(net.name, via.name, at(f.at), micro(rule.min), rule.other, rule.name);
                add(f);
            }
        }

        // Keep-outs.
        for (const KeepOut& k : p.keepOuts) {
            bool letIn = k.letThrough != KeepOut::LetThrough::None && k.except.contains(net.name);
            if (k.letThrough != KeepOut::LetThrough::None)
                for (const Terminal& t : net.terminals) letIn = letIn || inside(k.outline, t.at);
            const Box kb = boundsOf(k.outline);
            for (int l = 0; l < planes; ++l) {
                if (!k.metals.isEmpty() && !k.metals.contains(l)) continue;
                for (const Poly& q : fresh.at(l)) {
                    if (!boundsOf(q).overlaps(kb)) continue;
                    const double area = overlapArea(q, k.outline);
                    if (area <= 0) continue;
                    if (letIn) {
                        v.notes << QStringLiteral("%1 goes into the keep-out %2 on %3 (%4 µm²), as it may")
                                       .arg(net.name, k.name, metalName(l), micro(area * p.unit * p.unit));
                        continue;
                    }
                    Finding f;
                    f.kind = QStringLiteral("keep_out");
                    f.net = net.name;
                    f.other = k.name;
                    f.metal = l;
                    f.box = boundsOf(q).intersected(kb);
                    f.at = f.box.centre();
                    f.measured = area * p.unit * p.unit;
                    f.text = QStringLiteral("%1 on %2 goes into the keep-out %3 near %4 (%5 µm²)").arg(net.name, metalName(l), k.name, at(f.at), micro(f.measured));
                    add(f);
                }
            }
        }
    }

    // New metal against other nets' new metal, and against metal not its own.
    for (int l = 0; l < planes; ++l) {
        std::vector<int> ids;
        std::vector<Box> bounds;
        for (int i = 0; i < int(pieces.size()); ++i)
            if (pieces[std::size_t(i)].plane == l) {
                ids.push_back(i);
                bounds.push_back(pieces[std::size_t(i)].bounds);
            }
        BoxIndex index;
        index.build(bounds);
        Coord most = 0;
        for (const NetClass& c : p.tech.classes) most = std::max(most, spacingOn(p, l, p.units(std::max(c.spacing, p.tech.sameLayer))));
        std::map<std::pair<int, int>, Finding> worst;   // a pair of nets: the nearest place
        for (std::size_t a = 0; a < ids.size(); ++a) {
            const Piece& pa = pieces[std::size_t(ids[a])];
            index.query(pa.bounds.grown(most), [&](int bi) {
                const Piece& pb = pieces[std::size_t(ids[std::size_t(bi)])];
                if (std::size_t(bi) <= a || pb.net == pa.net) return true;
                const NetClass& ca = p.tech.classes.at(p.nets.at(pa.net).netClass);
                const NetClass& cb = p.tech.classes.at(p.nets.at(pb.net).netClass);
                const Coord classes = p.units(std::max({ca.spacing, cb.spacing, p.tech.sameLayer}));
                const Coord need = spacingOn(p, l, classes);
                if (boxDistance2(pa.bounds, pb.bounds) >= double(need) * double(need)) return true;
                Pt where;
                const double d = polyDistance(pa.poly, pb.poly, &where);
                if (d >= double(need) - tol) return true;
                const std::pair<int, int> key{std::min(pa.net, pb.net), std::max(pa.net, pb.net)};
                auto it = worst.find(key);
                if (it != worst.end() && it->second.measured <= um(d)) return true;
                Finding f;
                f.kind = d <= 0 ? QStringLiteral("short") : QStringLiteral("spacing");
                f.net = p.nets.at(key.first).name;
                f.other = p.nets.at(key.second).name;
                f.metal = l;
                f.at = where;
                f.box = Box{where.x, where.y, where.x, where.y}.grown(std::max<Coord>(need * 3, p.units(10)));
                f.measured = um(d);
                f.required = um(double(need));
                if (need > classes) f.rule = ruleOf(p.tech, QStringLiteral("spacing"), {metalName(l)}, p.tech.metalSpacing.at(l));
                f.text = d <= 0 ? QStringLiteral("%1 and %2 touch on %3 at %4").arg(f.net, f.other, metalName(l), at(where))
                                : QStringLiteral("%1 and %2 are %3 µm apart on %4 at %5 (%6 µm needed%7)")
                                      .arg(f.net, f.other, micro(f.measured), metalName(l), at(where), micro(f.required),
                                           f.rule.isEmpty() ? QString() : QStringLiteral(", the rule ") + f.rule);
                worst[key] = f;
                return true;
            });
        }
        for (const auto& [key, f] : worst) add(f);

        // Metal already there that is not the net's own.
        const Coord toMetal = spacingOn(p, l, p.units(p.tech.toMetal)), toTerminal = spacingOn(p, l, p.units(p.tech.toTerminal));
        const Coord reach = std::max({toMetal, toTerminal, Coord(1)});
        const QString spacingRule = l < p.tech.metalSpacing.size() && p.tech.metalSpacing.at(l) > 0
                                        ? ruleOf(p.tech, QStringLiteral("spacing"), {metalName(l)}, p.tech.metalSpacing.at(l))
                                        : QString();
        std::map<std::pair<int, int>, Finding> near;    // net, shape
        for (int id : ids) {
            const Piece& pc = pieces[std::size_t(id)];
            m.index[std::size_t(l)].query(pc.bounds.grown(reach), [&](int j) {
                const int s = m.onPlane[std::size_t(l)][std::size_t(j)];
                const int own = ownerOf(m, s);
                if (own == pc.net) return true;
                const bool pad = m.padOf[std::size_t(s)] >= 0;
                const Coord need = pad ? toTerminal : toMetal;
                Pt where;
                const double d = polyDistance(pc.poly, p.metal.at(s).outline, &where, 0);
                if (d > 0 && d >= double(need) - tol) return true;
                const std::pair<int, int> key{pc.net, s};
                auto it = near.find(key);
                if (it != near.end() && it->second.measured <= um(d)) return true;
                Finding f;
                f.net = p.nets.at(pc.net).name;
                f.metal = l;
                f.at = where;
                f.box = Box{where.x, where.y, where.x, where.y}.grown(std::max<Coord>(need * 3, p.units(10)));
                f.measured = um(d);
                f.required = um(double(need));
                if (need == p.units(p.tech.metalSpacing.value(l)) && need > p.units(pad ? p.tech.toTerminal : p.tech.toMetal)) f.rule = spacingRule;
                if (own >= 0) f.other = p.nets.at(own).name;
                if (d <= 0) {
                    f.kind = own >= 0 ? QStringLiteral("short") : QStringLiteral("metal");
                    f.text = own >= 0 ? QStringLiteral("%1 touches %2 of net %3 on %4 at %5")
                                            .arg(f.net, pad ? QStringLiteral("the pad") : QStringLiteral("metal"), f.other, metalName(l), at(where))
                                      : QStringLiteral("%1 touches metal of no net on %2 at %3").arg(f.net, metalName(l), at(where));
                } else {
                    f.kind = pad ? QStringLiteral("pad") : QStringLiteral("metal");
                    f.text = QStringLiteral("%1 is %2 µm from %3 on %4 at %5 (%6 µm needed%7)")
                                 .arg(f.net, micro(f.measured),
                                      pad ? QStringLiteral("net %1's pad").arg(f.other)
                                          : (own >= 0 ? QStringLiteral("metal of net %1").arg(f.other) : QStringLiteral("metal of no net")),
                                      metalName(l), at(where), micro(f.required), f.rule.isEmpty() ? QString() : QStringLiteral(", the rule ") + f.rule);
                }
                near[key] = f;
                return true;
            });
        }
        for (const auto& [key, f] : near) add(f);
    }

    // Via cuts apart, the routes' against all, by cut layer (two vias of
    // one layer keep the larger of their spacings).
    std::vector<LayerId> cutLayers;
    for (const Via& via : p.tech.vias)
        if (std::find(cutLayers.begin(), cutLayers.end(), via.layer) == cutLayers.end()) cutLayers.push_back(via.layer);
    for (const LayerId& layer : cutLayers) {
        Coord most = 0;
        for (const Via& via : p.tech.vias)
            if (via.layer == layer) most = std::max(most, p.units(via.spacing));
        if (most <= 0) continue;
        std::vector<Box> all;
        std::vector<int> netOf, defOf;
        for (const auto& [n, c] : cuts)
            if (p.tech.vias.at(c.first).layer == layer) {
                all.push_back(c.second);
                netOf.push_back(n);
                defOf.push_back(c.first);
            }
        const std::size_t ours = all.size();
        for (const Shape& c : p.cuts)
            if (c.layer >= 0 && c.layer < p.tech.vias.size() && p.tech.vias.at(c.layer).layer == layer) {
                all.push_back(boundsOf(c.outline));
                netOf.push_back(-1);
                defOf.push_back(c.layer);
            }
        BoxIndex index;
        index.build(all);
        for (std::size_t a = 0; a < ours; ++a)
            index.query(all[a].grown(most), [&](int b) {
                if (std::size_t(b) == a || (std::size_t(b) < ours && std::size_t(b) < a)) return true;
                if (all[std::size_t(b)] == all[a]) return true;
                const Via& via = p.tech.vias.at(defOf[a]);
                const Via& theirs = p.tech.vias.at(defOf[std::size_t(b)]);
                const Coord need = std::max(p.units(via.spacing), p.units(theirs.spacing));
                if (need <= 0) return true;
                const double d = std::sqrt(boxDistance2(all[a], all[std::size_t(b)]));
                if (d >= double(need) - tol) return true;
                Finding f;
                f.kind = QStringLiteral("via_spacing");
                f.net = p.nets.at(netOf[a]).name;
                f.other = netOf[std::size_t(b)] >= 0 ? p.nets.at(netOf[std::size_t(b)]).name : QStringLiteral("a via already there");
                f.at = all[a].centre();
                f.box = all[a].united(all[std::size_t(b)]).grown(need);
                f.measured = um(d);
                f.required = std::max(via.spacing, theirs.spacing);
                f.rule = ruleOf(p.tech, QStringLiteral("spacing"), namesOf(p.tech, via.spacing >= theirs.spacing ? via : theirs), f.required);
                f.text = QStringLiteral("%1's %2 at %3 is %4 µm from %5's (%6 µm needed%7)")
                             .arg(f.net, via.name, at(f.at), micro(f.measured), f.other, micro(f.required),
                                  f.rule.isEmpty() ? QString() : QStringLiteral(", the rule ") + f.rule);
                add(f);
                return true;
            });
    }
    v.notes.removeDuplicates();
    v.seconds = timer.elapsed() / 1000.0;
    return v;
}

} // namespace qucs_s::router
