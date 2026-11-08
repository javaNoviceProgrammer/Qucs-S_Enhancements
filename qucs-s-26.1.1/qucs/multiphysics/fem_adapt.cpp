/*
 * fem_adapt.cpp - a 2D multiphysics model's mesh convergence check (see
 *                 fem_adapt.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_adapt.h"

#include "fem_edge.h"
#include "fem_optimize.h"
#include "fem_problem.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <unordered_map>

namespace qucs_s::fem {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInfinity = std::numeric_limits<double>::infinity();
constexpr int kMostValues = 20;
constexpr int kMostMeshes = 40;
// What a run keeps of its mesh's sizes: more than this and they are gathered.
constexpr int kMostSizes = 4000;

QString tr(const char* text)
{
    return QCoreApplication::translate("MultiphysicsConvergence", text);
}

unsigned long long edgeKeyOf(int a, int b)
{
    if (a > b) std::swap(a, b);
    return (static_cast<unsigned long long>(static_cast<unsigned int>(a)) << 32) | static_cast<unsigned int>(b);
}

double areaOf(QPointF a, QPointF b, QPointF c)
{
    return ((b.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (b.y() - a.y())) / 2;
}

double sizeOf(const Mesh& m, int t)
{
    const auto& v = m.triangles[std::size_t(t)];
    const double area = std::abs(areaOf(m.nodes[std::size_t(v[0])], m.nodes[std::size_t(v[1])], m.nodes[std::size_t(v[2])]));
    return std::sqrt(4 * area / std::sqrt(3.0));
}

QString cellText(const QJsonValue& v)
{
    if (v.isString()) return v.toString().trimmed();
    if (v.isDouble()) return formatNumber(v.toDouble(), 15);
    if (v.isBool()) return v.toBool() ? QStringLiteral("yes") : QStringLiteral("no");
    return QString();
}

} // namespace

// ----------------------------------------------------------------- The mesh

std::shared_ptr<Mesh> bisectMesh(const Mesh& m, const std::vector<char>& marked, const std::vector<MeshSizes::Periodic>& periodic)
{
    const int nt = int(m.triangles.size());
    if (int(marked.size()) != nt) return nullptr;
    // The edges, each with its triangles (at most two) and whether it is cut.
    struct EdgeInfo {
        int a, b;
        int t0 = -1, t1 = -1;
        double length2 = 0;
        bool cut = false;
        int mid = -1;
    };
    std::vector<EdgeInfo> edges;
    edges.reserve(std::size_t(nt) * 2);
    std::unordered_map<unsigned long long, int> byNodes;
    byNodes.reserve(std::size_t(nt) * 2);
    std::vector<std::array<int, 3>> ofTriangle(static_cast<std::size_t>(nt));   // its edges: (0,1), (1,2), (2,0)
    std::vector<int> longest(static_cast<std::size_t>(nt), 0);                  // which of them
    for (int t = 0; t < nt; ++t) {
        const auto& v = m.triangles[std::size_t(t)];
        for (int k = 0; k < 3; ++k) {
            const int a = v[std::size_t(k)], b = v[std::size_t((k + 1) % 3)];
            const auto [it, added] = byNodes.try_emplace(edgeKeyOf(a, b), int(edges.size()));
            if (added) {
                EdgeInfo e{a, b};
                const QPointF d = m.nodes[std::size_t(a)] - m.nodes[std::size_t(b)];
                e.length2 = d.x() * d.x() + d.y() * d.y();
                edges.push_back(e);
            }
            EdgeInfo& e = edges[std::size_t(it->second)];
            (e.t0 < 0 ? e.t0 : e.t1) = t;
            ofTriangle[std::size_t(t)][std::size_t(k)] = it->second;
        }
        // Its longest edge, ties the same way from either triangle: a mesh of
        // squares cut in two would otherwise cut a shared edge from one side.
        int best = 0;
        for (int k = 1; k < 3; ++k) {
            const EdgeInfo& e = edges[std::size_t(ofTriangle[std::size_t(t)][std::size_t(k)])];
            const EdgeInfo& b = edges[std::size_t(ofTriangle[std::size_t(t)][std::size_t(best)])];
            if (e.length2 > b.length2 * (1 + 1e-12)
                || (e.length2 >= b.length2 * (1 - 1e-12) && edgeKeyOf(e.a, e.b) > edgeKeyOf(b.a, b.b)))
                best = k;
        }
        longest[std::size_t(t)] = best;
    }
    // What is cut: each marked triangle's longest edge, and a triangle with any
    // of its edges cut has its longest cut too (the angles stay bounded: Rivara's).
    std::vector<int> stack;
    const auto cut = [&](int e) {
        if (!edges[std::size_t(e)].cut) {
            edges[std::size_t(e)].cut = true;
            stack.push_back(e);
        }
    };
    const auto closure = [&] {
        while (!stack.empty()) {
            const EdgeInfo e = edges[std::size_t(stack.back())];
            stack.pop_back();
            for (int t : {e.t0, e.t1})
                if (t >= 0) cut(ofTriangle[std::size_t(t)][std::size_t(longest[std::size_t(t)])]);
        }
    };
    for (int t = 0; t < nt; ++t)
        if (marked[std::size_t(t)]) cut(ofTriangle[std::size_t(t)][std::size_t(longest[std::size_t(t)])]);
    closure();
    // The new nodes, and which were put on an arc.
    auto out = std::make_shared<Mesh>();
    out->nodes = m.nodes;
    out->pointNode = m.pointNode;
    out->topology = m.topology;
    out->unitScale = m.unitScale;
    out->sharpCorners = m.sharpCorners;
    std::unordered_map<unsigned long long, int> boundaryEdge;
    boundaryEdge.reserve(m.edges.size());
    for (int e = 0; e < int(m.edges.size()); ++e) boundaryEdge.emplace(edgeKeyOf(m.edges[std::size_t(e)].a, m.edges[std::size_t(e)].b), e);
    std::vector<std::pair<int, QPointF>> moved;   // a node on an arc, and where the chord has it
    for (EdgeInfo& e : edges) {
        if (!e.cut) continue;
        e.mid = int(out->nodes.size());
        QPointF p = (m.nodes[std::size_t(e.a)] + m.nodes[std::size_t(e.b)]) / 2;
        const auto b = boundaryEdge.find(edgeKeyOf(e.a, e.b));
        if (b != boundaryEdge.end() && m.topology) {
            const Mesh::Edge& me = m.edges[std::size_t(b->second)];
            if (me.curve >= 0 && me.curve < int(m.topology->curves.size())) {
                const Curve& c = m.topology->curves[std::size_t(me.curve)];
                if (c.kind == Curve::Arc) {
                    moved.emplace_back(e.mid, p);
                    p = c.at((me.ta + me.tb) / 2);
                }
            }
        }
        out->nodes.push_back(p);
    }
    // Each triangle: by its longest edge if any is cut, then each half by the
    // edge of its own still to be cut.
    const std::function<void(int, int, int, int, int, int, int)> split = [&](int a, int b, int c, int ab, int bc, int ca, int domain) {
        // (a, b, c) counter-clockwise; ab, bc, ca: the edge to cut's middle, or -1.
        if (ab >= 0) {
            split(a, ab, c, -1, -1, ca, domain);
            split(ab, b, c, -1, bc, -1, domain);
        } else if (bc >= 0) {
            split(b, bc, a, -1, -1, -1, domain);
            split(bc, c, a, -1, ca, -1, domain);
        } else if (ca >= 0) {
            split(c, ca, b, -1, -1, -1, domain);
            split(ca, a, b, -1, -1, -1, domain);
        } else {
            out->triangles.push_back({a, b, c});
            out->domain.push_back(domain);
        }
    };
    out->triangles.reserve(std::size_t(nt) + std::size_t(nt) / 2);
    out->domain.reserve(std::size_t(nt) + std::size_t(nt) / 2);
    for (int t = 0; t < nt; ++t) {
        const auto& v = m.triangles[std::size_t(t)];
        const int domain = m.domain[std::size_t(t)];
        const auto& es = ofTriangle[std::size_t(t)];
        const int mids[3] = {edges[std::size_t(es[0])].mid, edges[std::size_t(es[1])].mid, edges[std::size_t(es[2])].mid};
        if (mids[0] < 0 && mids[1] < 0 && mids[2] < 0) {
            out->triangles.push_back(v);
            out->domain.push_back(domain);
            continue;
        }
        const int l = longest[std::size_t(t)];
        // Turned so that its longest edge is first.
        const int a = v[std::size_t(l)], b = v[std::size_t((l + 1) % 3)], c = v[std::size_t((l + 2) % 3)];
        split(a, b, c, mids[l], mids[(l + 1) % 3], mids[(l + 2) % 3], domain);
    }
    // A node on an arc back on its chord where a triangle would turn over.
    if (!moved.empty()) {
        std::unordered_map<int, QPointF> chord;
        for (const auto& [node, p] : moved) chord.emplace(node, p);
        for (int pass = 0; pass < 4; ++pass) {
            bool changed = false;
            for (const auto& t : out->triangles) {
                if (areaOf(out->nodes[std::size_t(t[0])], out->nodes[std::size_t(t[1])], out->nodes[std::size_t(t[2])]) > 0) continue;
                for (int v : t) {
                    const auto it = chord.find(v);
                    if (it != chord.end() && out->nodes[std::size_t(v)] != it->second) {
                        out->nodes[std::size_t(v)] = it->second;
                        changed = true;
                    }
                }
            }
            if (!changed) break;
        }
    }
    // The boundary edges, each cut one in two; the triangles on their sides.
    out->edges.reserve(m.edges.size() + 16);
    for (const Mesh::Edge& me : m.edges) {
        const auto it = byNodes.find(edgeKeyOf(me.a, me.b));
        const int mid = it != byNodes.end() ? edges[std::size_t(it->second)].mid : -1;
        if (mid < 0) {
            out->edges.push_back(me);
            continue;
        }
        Mesh::Edge first = me, second = me;
        const double tm = (me.ta + me.tb) / 2;
        first.b = mid;
        first.tb = tm;
        second.a = mid;
        second.ta = tm;
        out->edges.push_back(first);
        out->edges.push_back(second);
    }
    std::unordered_map<unsigned long long, int> directed;
    directed.reserve(out->triangles.size() * 3);
    const auto directedKey = [](int a, int b) {
        return (static_cast<unsigned long long>(static_cast<unsigned int>(a)) << 32) | static_cast<unsigned int>(b);
    };
    for (int t = 0; t < int(out->triangles.size()); ++t) {
        const auto& v = out->triangles[std::size_t(t)];
        for (int k = 0; k < 3; ++k) directed[directedKey(v[std::size_t(k)], v[std::size_t((k + 1) % 3)])] = t;
    }
    for (Mesh::Edge& e : out->edges) {
        const auto l = directed.find(directedKey(e.a, e.b));
        const auto r = directed.find(directedKey(e.b, e.a));
        e.left = l != directed.end() ? l->second : -1;
        e.right = r != directed.end() ? r->second : -1;
    }
    if (!periodic.empty()) {
        MeshSizes sizes;
        sizes.periodic = periodic;
        matchPeriodicSides(*out, sizes);
    }
    out->computeStatistics();
    return out;
}

// --------------------------------------------------------------- The indicator

namespace {

/// Each triangle's squared error of a function it gives at any point (\a comps
/// numbers) against the one recovered from its neighbours in its domain: its
/// values at each corner, weighed by the triangles' areas, then linear across each
/// triangle. Added to \a out with the group's total brought to \a weight.
class Recovery
{
public:
    Recovery(const Solution& s, const std::vector<char>& solved, int comps) : a_s(s), a_solved(solved), a_comps(comps)
    {
        const Mesh& m = s.mesh();
        a_slots.resize(m.triangles.size());
        a_head.assign(m.nodes.size(), -1);
        for (int t = 0; t < int(m.triangles.size()); ++t) {
            const int d = m.domain[std::size_t(t)];
            if (d < 0 || d >= int(solved.size()) || !solved[std::size_t(d)]) {
                a_slots[std::size_t(t)] = {-1, -1, -1};
                continue;
            }
            for (int k = 0; k < 3; ++k) {
                const int n = m.triangles[std::size_t(t)][std::size_t(k)];
                int slot = a_head[std::size_t(n)];
                while (slot >= 0 && a_domain[std::size_t(slot)] != d) slot = a_next[std::size_t(slot)];
                if (slot < 0) {
                    slot = int(a_domain.size());
                    a_domain.push_back(d);
                    a_next.push_back(a_head[std::size_t(n)]);
                    a_head[std::size_t(n)] = slot;
                }
                a_slots[std::size_t(t)][std::size_t(k)] = slot;
            }
        }
        a_sum.assign(a_domain.size() * std::size_t(comps), 0.0);
        a_area.assign(a_domain.size(), 0.0);
    }

    template <class F>
    double add(const std::vector<std::array<double, 3>>& rule, std::vector<double>& out, double weight, F at)
    {
        static const double corner[3][2] = {{0, 0}, {1, 0}, {0, 1}};
        const Mesh& m = a_s.mesh();
        const int nt = int(m.triangles.size());
        std::vector<double> value(std::size_t(a_comps), 0.0);
        for (int t = 0; t < nt; ++t) {
            const auto& held = a_slots[std::size_t(t)];
            if (held[0] < 0) continue;
            const auto& v = m.triangles[std::size_t(t)];
            const double area = std::abs(areaOf(m.nodes[std::size_t(v[0])], m.nodes[std::size_t(v[1])], m.nodes[std::size_t(v[2])]));
            for (int k = 0; k < 3; ++k) {
                at(t, corner[k][0], corner[k][1], value.data());
                const std::size_t slot = std::size_t(held[std::size_t(k)]);
                for (int c = 0; c < a_comps; ++c) a_sum[slot * std::size_t(a_comps) + std::size_t(c)] += area * value[std::size_t(c)];
                a_area[slot] += area;
            }
        }
        std::vector<double> errors(std::size_t(nt), 0.0);
        double total = 0;
        for (int t = 0; t < nt; ++t) {
            const auto& held = a_slots[std::size_t(t)];
            if (held[0] < 0) continue;
            const auto& v = m.triangles[std::size_t(t)];
            const double area = std::abs(areaOf(m.nodes[std::size_t(v[0])], m.nodes[std::size_t(v[1])], m.nodes[std::size_t(v[2])]));
            double e2 = 0;
            for (const auto& q : rule) {
                const double lambda[3] = {1 - q[0] - q[1], q[0], q[1]};
                at(t, q[0], q[1], value.data());
                for (int c = 0; c < a_comps; ++c) {
                    double recovered = 0;
                    for (int k = 0; k < 3; ++k) {
                        const std::size_t slot = std::size_t(held[std::size_t(k)]);
                        recovered += lambda[k] * a_sum[slot * std::size_t(a_comps) + std::size_t(c)] / a_area[slot];
                    }
                    const double d = value[std::size_t(c)] - recovered;
                    if (std::isfinite(d)) e2 += q[2] * 2 * area * d * d;
                }
            }
            errors[std::size_t(t)] = e2;
            total += e2;
        }
        if (total > 0)
            for (int t = 0; t < nt; ++t) out[std::size_t(t)] += weight * errors[std::size_t(t)] / total;
        std::fill(a_sum.begin(), a_sum.end(), 0.0);
        std::fill(a_area.begin(), a_area.end(), 0.0);
        return total;
    }

private:
    const Solution& a_s;
    const std::vector<char>& a_solved;
    int a_comps;
    std::vector<int> a_head, a_next, a_domain;
    std::vector<std::array<int, 3>> a_slots;
    std::vector<double> a_sum, a_area;
};

/// Field \a fi's share of the indicator, its total brought to 1, added to \a out;
/// what it was found from, or why none, said.
void fieldIndicator(const Solution& s, int fi, std::vector<double>& out, RefinementIndicator* said)
{
    const Field& f = s.fields()[std::size_t(fi)];
    if (f.type == QLatin1String("rays")) {
        said->without << tr("%1: rays have no error to estimate on a mesh").arg(f.tag);
        return;
    }
    if (!f.space) {
        said->without << tr("%1: no field on the mesh").arg(f.tag);
        return;
    }
    if (f.line || f.along || f.unknowns >= 0) {
        said->without << tr("%1: solved along a line, not on the triangles").arg(f.tag);
        return;
    }
    // Not in an absorbing layer: what is refined there does not reach the answer.
    std::vector<char> solved = f.domains;
    for (int d : pmlDomainsOf(s, f.tag))
        if (d >= 0 && d < int(solved.size())) solved[std::size_t(d)] = 0;
    const Space& sp = *f.space;
    const std::vector<std::array<double, 3>>& rule = triangleRule(sp.order() == 1 ? 2 : 4).points;
    if (f.edges) {
        // Edge elements: their field's tangential part is continuous and its
        // normal part jumps, within a domain they should be one smooth field -
        // the recovery of E itself.
        Recovery recovery(s, solved, 6);
        PointEvaluator ev(s);
        const bool resonator = f.type == QLatin1String("resonator");
        const double total = recovery.add(rule, out, 1.0, [&](int t, double xi, double eta, double* v) {
            ev.moveTo(t, xi, eta);
            std::complex<double> E[3];
            if (resonator) {
                const detail::ResonatorFields r = detail::resonatorFieldsAt(ev, fi);
                for (int c = 0; c < 3; ++c) E[c] = r.defined ? r.E[c] : std::complex<double>();
            } else {
                const detail::ModeFields m = detail::modeFieldsAt(ev, fi);
                for (int c = 0; c < 3; ++c) E[c] = m.defined ? m.E[c] : std::complex<double>();
            }
            for (int c = 0; c < 3; ++c) {
                v[2 * c] = E[c].real();
                v[2 * c + 1] = E[c].imag();
            }
        });
        if (total > 0) said->fields << tr("the electric field recovered, of %1").arg(f.tag);
        else said->without << tr("%1: nothing in its field to estimate").arg(f.tag);
        return;
    }
    // Each group of components: one number's parts, a recovery of its gradient.
    struct Group {
        std::vector<int> components;
        bool logarithm = false;
    };
    std::vector<Group> groups;
    if (f.type == QLatin1String("semiconductor")) {
        groups = {{{0}, false}, {{1}, true}, {{2}, true}};
    } else if (f.type == QLatin1String("quantum")) {
        for (int c = 0; c < std::min(3, f.count()); ++c) groups.push_back({{c}, false});
    } else if (f.type == QLatin1String("pde")) {
        for (int c = 0; c < f.count(); ++c) {
            const QString name = f.components.at(c);
            const bool rate = (name.endsWith(QLatin1String("_t")) && f.components.contains(name.chopped(2)))
                              || (name.endsWith(QLatin1Char('t')) && f.components.contains(name.chopped(1)));
            if (!rate) groups.push_back({{c}, false});
        }
    } else {
        Group all;
        for (int c = 0; c < f.count(); ++c) all.components.push_back(c);
        groups.push_back(all);
    }
    const std::size_t n = std::size_t(sp.size());
    if (f.values.size() < n * std::size_t(f.count())) {
        said->without << tr("%1: not solved on its space").arg(f.tag);
        return;
    }
    double reached = 0;
    for (const Group& g : groups) {
        Recovery recovery(s, solved, 2 * int(g.components.size()));
        const double total = recovery.add(rule, out, 1.0 / double(groups.size()), [&](int t, double xi, double eta, double* v) {
            Shape sh;
            shapeAt(sp.order(), xi, eta, sh);
            const Space::Map map = sp.map(t, xi, eta);
            for (std::size_t c = 0; c < g.components.size(); ++c) {
                double rx = 0, ry = 0;
                for (int i = 0; i < sh.count; ++i) {
                    double value = f.values[std::size_t(g.components[c]) * n + std::size_t(sp.dofs(t)[i])];
                    if (g.logarithm) value = std::log(std::max(std::abs(value), 1.0));
                    rx += sh.dxi[i] * value;
                    ry += sh.deta[i] * value;
                }
                double gx = 0, gy = 0;
                Space::gradient(map, rx, ry, gx, gy);
                v[2 * c] = gx;
                v[2 * c + 1] = gy;
            }
        });
        reached += total;
    }
    if (reached > 0) said->fields << tr("Zienkiewicz-Zhu, of %1").arg(f.tag);
    else said->without << tr("%1: its gradient has no error to estimate").arg(f.tag);
}

} // namespace

RefinementIndicator refinementIndicator(const Solution& s, const QStringList& physics)
{
    RefinementIndicator out;
    const int nt = int(s.mesh().triangles.size());
    std::vector<double> squared(std::size_t(nt), 0.0);
    for (int i = 0; i < int(s.fields().size()); ++i) {
        const Field& f = s.fields()[std::size_t(i)];
        if (!f.solved || (!physics.isEmpty() && !physics.contains(f.tag))) continue;
        fieldIndicator(s, i, squared, &out);
    }
    if (out.fields.isEmpty()) return out;
    out.eta.resize(squared.size());
    for (std::size_t t = 0; t < squared.size(); ++t) out.eta[t] = std::sqrt(squared[t]);
    return out;
}

std::vector<char> markBulk(const std::vector<double>& eta, double share, double least)
{
    std::vector<char> marked(eta.size(), 0);
    std::vector<int> order;
    order.reserve(eta.size());
    double total = 0;
    for (int t = 0; t < int(eta.size()); ++t)
        if (std::isfinite(eta[std::size_t(t)]) && eta[std::size_t(t)] > 0) {
            order.push_back(t);
            total += eta[std::size_t(t)] * eta[std::size_t(t)];
        }
    std::sort(order.begin(), order.end(), [&](int a, int b) { return eta[std::size_t(a)] != eta[std::size_t(b)] ? eta[std::size_t(a)] > eta[std::size_t(b)] : a < b; });
    const std::size_t atLeast = std::size_t(std::ceil(least * double(eta.size())));
    double held = 0;
    std::size_t count = 0;
    for (int t : order) {
        if (held >= share * total && count >= atLeast) break;
        marked[std::size_t(t)] = 1;
        held += eta[std::size_t(t)] * eta[std::size_t(t)];
        ++count;
    }
    return marked;
}

// ---------------------------------------------------------------- The estimate

QString ValueEstimate::trustWord(Trust trust)
{
    switch (trust) {
    case Few: return QStringLiteral("few");
    case Unmoved: return QStringLiteral("unmoved");
    case Extrapolated: return QStringLiteral("extrapolated");
    case Bounded: return QStringLiteral("bounded");
    case Erratic: return QStringLiteral("erratic");
    }
    return QString();
}

std::optional<ValueEstimate::Trust> ValueEstimate::trustOf(const QString& word)
{
    for (Trust t : {Few, Unmoved, Extrapolated, Bounded, Erratic})
        if (trustWord(t) == word) return t;
    return std::nullopt;
}

namespace {

/// q of (1 - b^-q) / (a^q - 1) = rho, which falls from ln b / ln a at q = 0.
double rateOf(double a, double b, double rho)
{
    const auto f = [&](double q) { return (1 - std::pow(b, -q)) / (std::pow(a, q) - 1); };
    double lo = 1e-3, hi = 30;
    if (rho >= f(lo)) return 0;
    if (rho <= f(hi)) return hi;
    for (int i = 0; i < 80; ++i) {
        const double mid = std::sqrt(lo * hi);
        if (f(mid) > rho) lo = mid;
        else hi = mid;
    }
    return std::sqrt(lo * hi);
}

} // namespace

ValueEstimate estimateValue(const std::vector<double>& unknowns, const std::vector<double>& values, int order)
{
    ValueEstimate e;
    // The meshes it is read from, from the finest back, each at least 1.25 times
    // as large as the next: on meshes hardly apart a rate is mostly noise. (1.4
    // at first; with a tenth of the triangles marked a round grows by 1.1 to 1.3,
    // and 1.4 kept the TSV's first three meshes to compare until its seventh.)
    std::vector<double> ns, qs;
    for (std::size_t i = std::min(unknowns.size(), values.size()); i-- > 0;) {
        if (!std::isfinite(values[i]) || !(unknowns[i] > 0)) continue;
        if (!ns.empty() && unknowns[i] * 1.25 > ns.back()) continue;
        ns.push_back(unknowns[i]);
        qs.push_back(values[i]);
    }
    std::reverse(ns.begin(), ns.end());
    std::reverse(qs.begin(), qs.end());
    if (qs.empty()) {
        e.why = tr("no mesh gave it a value");
        return e;
    }
    e.value = qs.back();
    e.limit = e.value;
    if (qs.size() == 1) {
        e.why = tr("one mesh: nothing to compare it with");
        return e;
    }
    double scale = 0;
    for (double q : qs) scale = std::max(scale, std::abs(q));
    const std::size_t n = qs.size();
    const double d2 = qs[n - 1] - qs[n - 2];
    // What a solve's own rounding moves a value by.
    const double noise = 1e-11 * scale;
    bool still = true;
    for (std::size_t i = n >= 3 ? n - 3 : 0; i + 1 < n; ++i) still = still && std::abs(qs[i + 1] - qs[i]) <= noise;
    if (still) {
        e.trust = ValueEstimate::Unmoved;
        e.error = std::abs(d2);
        e.why = tr("it did not move at all between the meshes: it is exact on any of them, or does not depend on the mesh");
        return e;
    }
    const double b = ns[n - 1] / ns[n - 2];
    // With no rate to believe: as if it fell as slowly as a singularity would let it.
    const double slow = 3 * std::abs(d2) / (std::pow(b, 0.5) - 1);
    if (n == 2) {
        e.trust = ValueEstimate::Bounded;
        e.error = slow;
        e.why = tr("two meshes: a bound from their difference, with room");
        return e;
    }
    const double d1 = qs[n - 2] - qs[n - 3];
    const double a = ns[n - 2] / ns[n - 3];
    if (d1 * d2 <= 0) {
        e.error = 3 * std::max(std::abs(d1), std::abs(d2));
        if (std::abs(d2) > 0.8 * std::abs(d1)) {
            e.trust = ValueEstimate::Erratic;
            e.why = tr("it moves one way and then the other and no less: no rate, the limit not known");
        } else {
            e.trust = ValueEstimate::Bounded;
            e.why = tr("it moves one way and then the other, by less each time: bounded by its moves, not extrapolated");
        }
        return e;
    }
    const double rho = std::abs(d2) / std::abs(d1);
    const double q = rateOf(a, b, rho);
    const double highest = order + 1;
    if (q <= 0) {
        e.trust = ValueEstimate::Erratic;
        e.error = std::max(slow, 3 * std::abs(d2));
        e.why = tr("its changes are not slowing as the mesh grows: no rate, the limit not known");
        return e;
    }
    e.rate = q;
    if (q < 0.1) {
        e.trust = ValueEstimate::Bounded;
        e.error = slow;
        e.why = tr("a rate of %1 in the unknowns is too slow to extrapolate").arg(formatNumber(q, 3));
        return e;
    }
    if (q > 1.05 * order) {
        // Faster than the order, short of the asymptote: the changes fall faster
        // than the error does (a corner's read 2.2 where its error fell as 1.2),
        // so no rate is claimed - bounded as if the error fell at the order, with
        // room. This decides when the check runs; the check's own change, read at
        // a crack's rate, is what is said at the end. (A slower bound here refined
        // the review's corner to 1.5e-7 and the goal case to 8e-8 where 1e-5 and
        // 1e-6 were asked.)
        // (Above the order plus one too: there the meshes may have agreed by
        // chance, which is what the check is for. The crack's slowest bound
        // there held the TSV to its tenth mesh, its error 3e-4 by its seventh.)
        e.trust = ValueEstimate::Bounded;
        e.error = 2 * std::abs(d2) / (std::pow(b, double(order)) - 1);
        e.rate = kNaN;
        e.why = q > highest ? tr("its changes fell as unknowns^-%1, faster than elements of order %2 can converge (at most about %3): two meshes may have agreed by "
                                 "chance - bounded as if its error fell as unknowns^-%2, not extrapolated")
                                  .arg(formatNumber(q, 3))
                                  .arg(order)
                                  .arg(highest)
                            : tr("its changes fell as unknowns^-%1, faster than the error of elements of order %2 falls (unknowns^-%2): not taken as its rate, the "
                                 "error bounded from the last change")
                                  .arg(formatNumber(q, 3))
                                  .arg(order);
        return e;
    }
    // (A rate above the order is not extrapolated, above: seen short of the
    // asymptote, it made the corner's error said 1.6 times too small.)
    const double beyond = d2 / (std::pow(b, q) - 1);
    e.limit = qs[n - 1] + beyond;
    double room = 1.25;
    e.trust = ValueEstimate::Extrapolated;
    e.why = tr("a rate of %1 in the unknowns").arg(formatNumber(q, 3));
    if (n >= 4) {
        const double d0 = qs[n - 3] - qs[n - 4];
        const double earlier = d0 * d1 > 0 ? rateOf(ns[n - 3] / ns[n - 4], a, std::abs(d1) / std::abs(d0)) : 0.0;
        if (earlier > 0 && std::abs(earlier - q) <= 0.5 * std::max(earlier, q)) {
            e.steady = true;
            e.why = tr("a steady rate of %1 in the unknowns (%2 on the meshes before)").arg(formatNumber(q, 3), formatNumber(earlier, 3));
        } else {
            room = 3;
            e.why = tr("a rate of %1 in the unknowns, not yet steady (%2 on the meshes before): with more room").arg(formatNumber(q, 3), formatNumber(earlier, 3));
        }
    } else {
        room = 2;
        e.why = tr("a rate of %1 in the unknowns, from three meshes").arg(formatNumber(q, 3));
    }
    e.error = room * std::abs(beyond);
    return e;
}

// -------------------------------------------------------------------- The node

std::vector<const Node*> convergenceNodes(const Model& model)
{
    std::vector<const Node*> out;
    for (const Node& c : model.root.children) {
        if (c.type == QLatin1String("convergence")) out.push_back(&c);
        if (c.type == QLatin1String("convergences"))
            for (const Node& o : c.children) out.push_back(&o);
    }
    return out;
}

const Node* convergenceNode(const Model& model, const QString& tag)
{
    for (const Node* n : convergenceNodes(model))
        if (tag.isEmpty() || n->tag == tag) return n;
    return nullptr;
}

std::optional<ConvergencePlan> convergencePlan(const Model& model, const Node& node, const ParameterScope& ps, QString* error)
{
    const auto fail = [&](const QString& why) {
        if (error) *error = why;
        return std::optional<ConvergencePlan>();
    };
    if (model.is3D()) return fail(tr("%1: a 3D model's mesh is checked by its own estimate, not by this.").arg(node.name()));
    ConvergencePlan plan;
    plan.tag = node.tag;
    plan.name = node.name();
    plan.study = node.text(QStringLiteral("study")).trimmed();
    const std::vector<const Node*> studies = model.studies();
    if (studies.empty()) return fail(tr("%1: the model has no study.").arg(node.name()));
    if (plan.study.isEmpty()) plan.study = studies.front()->tag;
    bool found = false;
    QStringList tags;
    for (const Node* s : studies) {
        found = found || s->tag == plan.study;
        tags << s->tag;
    }
    if (!found) return fail(tr("%1: there is no study %2: the model has %3.").arg(node.name(), plan.study, tags.join(QStringLiteral(", "))));
    const auto number = [&](const QString& key, double* out, const QString& what) {
        const QString text = cellText(node.value(key));
        if (text.isEmpty()) return true;
        QString why;
        const std::optional<double> v = evaluateConstant(text, ps.scope, Dim(), &why);
        if (!v || !std::isfinite(*v)) {
            if (error) *error = tr("%1: its %2 \"%3\" is not a number%4").arg(node.name(), what, text, why.isEmpty() ? QString() : QStringLiteral(": ") + why);
            return false;
        }
        *out = *v;
        return true;
    };
    if (!number(QStringLiteral("tolerance"), &plan.tolerance, tr("tolerance"))) return std::nullopt;
    if (!(plan.tolerance > 0 && plan.tolerance < 1))
        return fail(tr("%1: its tolerance is the relative error asked of each value, above 0 and below 1 (0.01 is 1 %): %2.").arg(node.name(), formatNumber(plan.tolerance, 6)));
    double meshes = plan.maxMeshes;
    if (!number(QStringLiteral("maxmeshes"), &meshes, tr("meshes at most"))) return std::nullopt;
    if (!(meshes >= 2 && meshes <= kMostMeshes) || meshes != std::floor(meshes))
        return fail(tr("%1: its meshes at most is a whole number from 2 to %2: %3.").arg(node.name()).arg(kMostMeshes).arg(formatNumber(meshes, 6)));
    plan.maxMeshes = int(meshes);
    if (!number(QStringLiteral("maxunknowns"), &plan.maxUnknowns, tr("unknowns at most"))) return std::nullopt;
    if (!(plan.maxUnknowns >= 100 && plan.maxUnknowns <= 1e9))
        return fail(tr("%1: its unknowns at most is from 100 to 1e9: %2.").arg(node.name(), formatNumber(plan.maxUnknowns, 6)));
    plan.refine = node.text(QStringLiteral("refine")).trimmed();
    if (plan.refine.isEmpty()) plan.refine = QStringLiteral("auto");
    if (plan.refine != QLatin1String("auto") && plan.refine != QLatin1String("goal") && plan.refine != QLatin1String("uniform"))
        return fail(tr("%1: it refines by auto, goal or uniform, not \"%2\".").arg(node.name(), plan.refine));
    plan.sweep = node.text(QStringLiteral("sweep")).trimmed();
    if (plan.sweep.isEmpty()) plan.sweep = QStringLiteral("ends");
    if (!QStringList{QStringLiteral("own"), QStringLiteral("first"), QStringLiteral("last"), QStringLiteral("ends"), QStringLiteral("all")}.contains(plan.sweep))
        return fail(tr("%1: of a sweep it checks own, first, last, ends or all, not \"%2\".").arg(node.name(), plan.sweep));
    plan.check = !node.props.contains(QStringLiteral("check")) || node.flag(QStringLiteral("check"));
    for (const QString& t : node.list(QStringLiteral("physics"))) {
        const QString tag = t.trimmed();
        if (tag.isEmpty()) continue;
        bool has = false;
        QStringList all;
        for (const Node* p : model.physics()) {
            has = has || p->tag == tag;
            all << p->tag;
        }
        if (!has) return fail(tr("%1: there is no physics %2: the model has %3.").arg(node.name(), tag, all.join(QStringLiteral(", "))));
        plan.physics << tag;
    }
    const QJsonArray rows = node.value(QStringLiteral("values")).toArray();
    int row = 0;
    for (const QJsonValue& r : rows) {
        ++row;
        const QJsonArray cells = r.toArray();
        ConvergenceWatch w;
        w.expression = cellText(cells.at(0));
        if (w.expression.isEmpty()) continue;
        const QString tol = cellText(cells.at(1));
        if (!tol.isEmpty()) {
            bool ok = false;
            w.tolerance = tol.toDouble(&ok);
            if (!ok || !(w.tolerance > 0 && w.tolerance < 1))
                return fail(tr("%1: row %2 (%3): its tolerance is above 0 and below 1: \"%4\".").arg(node.name()).arg(row).arg(w.expression, tol));
        }
        w.absoluteText = cellText(cells.at(2));
        if (!w.absoluteText.isEmpty()) {
            // (Its unit, the value's, known once it is read: an expression
            // of the parameters read here in what it says.)
            Expression::Options o;
            const Expression e = Expression::compile(w.absoluteText, ps.scope, o);
            if (!e.isValid() || !e.isConstant() || !std::isfinite(e.constant()) || e.constant() < 0)
                return fail(tr("%1: row %2 (%3): its \"settled below\" is a value of nought or more: \"%4\"%5")
                                .arg(node.name())
                                .arg(row)
                                .arg(w.expression, w.absoluteText, e.isValid() ? QString() : QStringLiteral(": ") + e.error()));
            w.absolute = e.constant();
        }
        plan.values.push_back(w);
    }
    if (plan.values.empty())
        return fail(tr("%1: give at least one value to watch, as an optimization's objectives: es.C11, ec.R, emw.neff, a Derived Values node's lp1.Z0.").arg(node.name()));
    if (int(plan.values.size()) > kMostValues)
        return fail(tr("%1: %2 values at most (%3 given).").arg(node.name()).arg(kMostValues).arg(plan.values.size()));
    return plan;
}

// ------------------------------------------------------------------ The result

namespace {

QString rowText(const ConvergenceRow& r)
{
    QStringList cells;
    cells << QString::number(r.index) << QString::number(r.point) << r.kind << QString::number(r.triangles) << QString::number(r.unknowns, 'g', 17)
          << QString::number(r.smallest, 'g', 8) << QString::number(r.seconds, 'f', 3) << QString::number(r.indicator, 'g', 8);
    for (double v : r.values) cells << QString::number(v, 'g', 17);
    return cells.join(QLatin1Char(' '));
}

bool rowFrom(const QString& text, int columns, ConvergenceRow* row)
{
    const QStringList cells = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (cells.size() != 8 + columns) return false;
    bool ok = true;
    const auto integer = [&](int i) {
        bool good = false;
        const int v = cells.at(i).toInt(&good);
        ok = ok && good;
        return v;
    };
    const auto real = [&](int i) {
        bool good = false;
        const double v = cells.at(i).toDouble(&good);
        ok = ok && good;
        return v;
    };
    row->index = integer(0);
    row->point = integer(1);
    row->kind = cells.at(2);
    row->triangles = integer(3);
    row->unknowns = real(4);
    row->smallest = real(5);
    row->seconds = real(6);
    row->indicator = real(7);
    for (int c = 0; c < columns; ++c) row->values.push_back(real(8 + c));
    return ok && row->point >= 0;
}

QJsonArray textsOf(const QStringList& list)
{
    return QJsonArray::fromStringList(list);
}

QStringList textsIn(const QJsonValue& value)
{
    QStringList out;
    for (const QJsonValue& v : value.toArray())
        if (v.isString()) out << v.toString();
    return out;
}

double numberIn(const QJsonObject& o, const QString& key, double fallback)
{
    const QJsonValue v = o.value(key);
    if (v.isDouble()) return v.toDouble();
    if (v.isString()) {
        // (inf and nan, which JSON has no numbers for.)
        bool ok = false;
        const double d = v.toString().toDouble(&ok);
        if (ok) return d;
    }
    return fallback;
}

QJsonValue numberOf(double v)
{
    return std::isfinite(v) ? QJsonValue(v) : QJsonValue(QString::number(v));
}

} // namespace

QJsonObject ConvergenceResult::toJson() const
{
    QJsonObject o;
    if (!error.isEmpty()) {
        o.insert(QStringLiteral("error"), error);
        if (!setup.isEmpty()) o.insert(QStringLiteral("setup"), setup);
        return o;
    }
    o.insert(QStringLiteral("status"), status);
    o.insert(QStringLiteral("converged"), converged);
    if (falseConvergence) o.insert(QStringLiteral("falseconvergence"), true);
    o.insert(QStringLiteral("summary"), summary);
    o.insert(QStringLiteral("setup"), setup);
    o.insert(QStringLiteral("method"), method);
    o.insert(QStringLiteral("evidence"), textsOf(evidence));
    if (!notes.isEmpty()) o.insert(QStringLiteral("notes"), textsOf(notes));
    o.insert(QStringLiteral("columns"), textsOf(columns));
    o.insert(QStringLiteral("units"), textsOf(units));
    if (!points.isEmpty()) o.insert(QStringLiteral("points"), textsOf(points));
    QJsonArray lines;
    for (const ConvergenceRow& r : rows) lines.append(rowText(r));
    o.insert(QStringLiteral("rows"), lines);
    QJsonArray finals;
    for (const Final& f : values) {
        QJsonObject one{{QStringLiteral("column"), f.column},
                        {QStringLiteral("value"), numberOf(f.value)},
                        {QStringLiteral("limit"), numberOf(f.limit)},
                        {QStringLiteral("error"), numberOf(f.error)},
                        {QStringLiteral("relative"), numberOf(f.relative)},
                        {QStringLiteral("rate"), numberOf(f.rate)},
                        {QStringLiteral("tolerance"), f.tolerance},
                        {QStringLiteral("trust"), f.trust},
                        {QStringLiteral("why"), f.why},
                        {QStringLiteral("settled"), f.settled}};
        if (f.point) one.insert(QStringLiteral("point"), f.point);
        finals.append(one);
    }
    o.insert(QStringLiteral("values"), finals);
    o.insert(QStringLiteral("solves"), solves);
    if (adjoints) o.insert(QStringLiteral("adjoints"), adjoints);
    o.insert(QStringLiteral("unknownssolved"), unknownsSolved);
    o.insert(QStringLiteral("largest"), largest);
    o.insert(QStringLiteral("seconds"), seconds);
    if (!sizes.isEmpty()) {
        o.insert(QStringLiteral("sizes"), textsOf(sizes));
        o.insert(QStringLiteral("smallestsize"), smallestSize);
    }
    if (keptRow > 0) o.insert(QStringLiteral("keptrow"), keptRow);
    return o;
}

ConvergenceResult ConvergenceResult::fromJson(const QJsonObject& o)
{
    ConvergenceResult r;
    r.error = o.value(QStringLiteral("error")).toString();
    r.setup = o.value(QStringLiteral("setup")).toString();
    r.status = o.value(QStringLiteral("status")).toString();
    r.converged = o.value(QStringLiteral("converged")).toBool();
    r.falseConvergence = o.value(QStringLiteral("falseconvergence")).toBool();
    r.summary = o.value(QStringLiteral("summary")).toString();
    r.method = o.value(QStringLiteral("method")).toString();
    r.evidence = textsIn(o.value(QStringLiteral("evidence")));
    r.notes = textsIn(o.value(QStringLiteral("notes")));
    r.columns = textsIn(o.value(QStringLiteral("columns")));
    r.units = textsIn(o.value(QStringLiteral("units")));
    while (r.units.size() < r.columns.size()) r.units << QString();
    r.points = textsIn(o.value(QStringLiteral("points")));
    for (const QJsonValue& line : o.value(QStringLiteral("rows")).toArray()) {
        ConvergenceRow row;
        if (line.isString() && rowFrom(line.toString(), int(r.columns.size()), &row)) r.rows.push_back(row);
    }
    for (const QJsonValue& v : o.value(QStringLiteral("values")).toArray()) {
        const QJsonObject one = v.toObject();
        const int column = one.value(QStringLiteral("column")).toInt(-1);
        const int point = one.value(QStringLiteral("point")).toInt(0);
        if (column < 0 || column >= r.columns.size() || point < 0) continue;
        Final f;
        f.column = column;
        f.point = point;
        f.value = numberIn(one, QStringLiteral("value"), kNaN);
        f.limit = numberIn(one, QStringLiteral("limit"), kNaN);
        f.error = numberIn(one, QStringLiteral("error"), kInfinity);
        f.relative = numberIn(one, QStringLiteral("relative"), kInfinity);
        f.rate = numberIn(one, QStringLiteral("rate"), kNaN);
        f.tolerance = numberIn(one, QStringLiteral("tolerance"), 0);
        f.trust = one.value(QStringLiteral("trust")).toString();
        f.why = one.value(QStringLiteral("why")).toString();
        f.settled = one.value(QStringLiteral("settled")).toBool();
        r.values.push_back(f);
    }
    r.solves = o.value(QStringLiteral("solves")).toInt();
    r.adjoints = o.value(QStringLiteral("adjoints")).toInt();
    r.unknownsSolved = numberIn(o, QStringLiteral("unknownssolved"), 0);
    r.largest = numberIn(o, QStringLiteral("largest"), 0);
    r.seconds = numberIn(o, QStringLiteral("seconds"), 0);
    r.sizes = textsIn(o.value(QStringLiteral("sizes")));
    r.smallestSize = numberIn(o, QStringLiteral("smallestsize"), 0);
    r.keptRow = std::max(0, o.value(QStringLiteral("keptrow")).toInt());
    return r;
}

QString convergenceSetup(const Model& model, const Node& node)
{
    const QJsonObject whole = model.toObject();
    QJsonObject setup;
    setup.insert(QStringLiteral("parameters"), whole.value(QStringLiteral("parameters")));
    setup.insert(QStringLiteral("functions"), whole.value(QStringLiteral("functions")));
    setup.insert(QStringLiteral("components"), whole.value(QStringLiteral("components")));
    setup.insert(QStringLiteral("studies"), whole.value(QStringLiteral("studies")));
    // (Its own sizes kept do not make it old: the result read "changed since" the
    // moment Keep Refined Mesh had written them - the review of 063142f.)
    QJsonArray components = whole.value(QStringLiteral("components")).toArray();
    for (int i = 0; i < components.size(); ++i) {
        QJsonObject c = components.at(i).toObject();
        QJsonObject mesh = c.value(QStringLiteral("mesh")).toObject();
        QJsonArray features, kept = mesh.value(QStringLiteral("features")).toArray();
        for (const QJsonValue& f : kept)
            if (f.toObject().value(QStringLiteral("type")).toString() != QLatin1String("adapted")) features.append(f);
        if (features.isEmpty()) mesh.remove(QStringLiteral("features"));
        else mesh.insert(QStringLiteral("features"), features);
        c.insert(QStringLiteral("mesh"), mesh);
        components.replace(i, c);
    }
    setup.insert(QStringLiteral("components"), components);
    QJsonObject mine = node.props;
    mine.remove(QStringLiteral("result"));
    setup.insert(QStringLiteral("convergence"), mine);
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(setup).toJson(QJsonDocument::Compact), QCryptographicHash::Md5).toHex());
}

QJsonArray convergenceValueRow(const Solution* solved, const QStringList& used)
{
    QString first;
    if (solved)
        for (const Variable& g : solved->globals())
            if (!used.contains(g.name)) {
                first = g.name;
                break;
            }
    return QJsonArray{first, QString(), QString()};
}

// -------------------------------------------------------------- The adjoint

namespace {

/// What one value watched is of, for goal-oriented refinement: the energy of a
/// field (its own error is the value's), an integral or average of a field over
/// domains or boundaries, or its highest or lowest point (a heat's Tmax); and the
/// model whose solution is its adjoint, its sources left out and its fixed values
/// nought, a source of the value's added on each mesh.
struct Goal {
    enum Kind { None, Energy, Domains, Boundaries, Peak };
    Kind kind = None;
    QString why;           ///< of None: why there is no adjoint
    QString physics;       ///< the field's tag
    QString type;          ///< its physics' type: heat, electrostatics, currents
    QJsonValue selection;  ///< of Domains and Boundaries
    bool highest = true;   ///< of Peak
    Model dual;
    QString words;         ///< what it refines by, in words
};

bool linearIn(const Solution& s, int fi)
{
    if (fi < 0 || fi >= int(s.coefficients().size())) return false;
    const Solution::Coefficients& c = s.coefficients()[std::size_t(fi)];
    for (const Expression& e : c.c)
        if (e.isValid() && s.varies(e)) return false;
    for (const auto& [name, list] : c.named)
        for (const Expression& e : list)
            if (e.isValid() && s.varies(e)) return false;
    return true;
}

/// The adjoint's model of \a physics in \a model, or why none.
std::optional<Model> dualModel(const Model& model, const QString& physics, const QString& study, QString* why)
{
    Model m = model;
    Node* st = nullptr;
    for (Node& c : m.root.children)
        if (c.type == QLatin1String("study") && c.tag == study) st = &c;
    if (!st) {
        *why = tr("no study");
        return std::nullopt;
    }
    int steps = 0;
    for (Node& step : st->children) {
        if (!step.enabled) continue;
        if (step.type != QLatin1String("stationary")) {
            *why = tr("its study has a %1 step: an adjoint is set up for a stationary study alone").arg(step.name());
            return std::nullopt;
        }
        step.props.remove(QStringLiteral("physics"));
        ++steps;
    }
    if (steps != 1) {
        *why = tr("its study is not one stationary step");
        return std::nullopt;
    }
    Node* ph = nullptr;
    for (const Node* p : m.physics()) {
        Node* mine = m.find(p->tag);
        if (!mine) continue;
        if (p->tag == physics) ph = mine;
        else mine->enabled = false;
    }
    if (!ph) {
        *why = tr("no physics %1").arg(physics);
        return std::nullopt;
    }
    // Each feature: a fixed value made nought, a source taken out, a flux's
    // outside made nought; one not known, no adjoint.
    std::vector<Node> kept;
    for (Node f : ph->children) {
        const QString t = f.type;
        if (!f.enabled) continue;
        if (t == QLatin1String("heat/source") || t == QLatin1String("heat/linesource") || t == QLatin1String("electrostatics/spacecharge")
            || t == QLatin1String("electrostatics/surfacecharge") || t == QLatin1String("currents/source") || t == QLatin1String("currents/normalcurrent"))
            continue;
        if (t == QLatin1String("heat/temperature")) f.props.insert(QStringLiteral("T0"), QStringLiteral("0[K]"));
        else if (t == QLatin1String("heat/flux")) {
            f.props.insert(QStringLiteral("q0"), QStringLiteral("0[W/m^2]"));
            f.props.insert(QStringLiteral("Text"), QStringLiteral("0[K]"));
        } else if (t == QLatin1String("electrostatics/potential") || t == QLatin1String("electrostatics/terminal") || t == QLatin1String("currents/potential")) {
            f.props.insert(QStringLiteral("V0"), QStringLiteral("0[V]"));
        } else if (t == QLatin1String("currents/terminal")) {
            f.props.insert(QStringLiteral("V0"), QStringLiteral("0[V]"));
            f.props.insert(QStringLiteral("I0"), QStringLiteral("0[A]"));
        } else if (t == QLatin1String("electrostatics/floating")) {
            f.props.insert(QStringLiteral("Q0"), QStringLiteral("0[C]"));
        } else if (t == QLatin1String("currents/floating")) {
            f.props.insert(QStringLiteral("I0"), QStringLiteral("0[A]"));
        } else if (t == QLatin1String("heat/init") || t == QLatin1String("heat/solid") || t == QLatin1String("heat/insulation")
                   || t == QLatin1String("electrostatics/charge") || t == QLatin1String("electrostatics/zerocharge") || t == QLatin1String("electrostatics/ground")
                   || t == QLatin1String("currents/conservation") || t == QLatin1String("currents/insulation") || t == QLatin1String("currents/ground")) {
        } else {
            *why = tr("its %1 is not a feature an adjoint is set up for").arg(f.name());
            return std::nullopt;
        }
        kept.push_back(f);
    }
    ph->children = kept;
    return m;
}

Goal goalOf(const Model& model, const Solution& s, const QString& study, const QString& expression)
{
    Goal g;
    static const QRegularExpression named(QStringLiteral("^\\s*([A-Za-z_][A-Za-z0-9_]*)(?:\\.([A-Za-z_][A-Za-z0-9_]*))?\\s*$"));
    const QRegularExpressionMatch m = named.match(expression);
    if (!m.hasMatch()) {
        g.why = tr("it is not one global value or Derived Values node");
        return g;
    }
    const QString head = m.captured(1), name = m.captured(2);
    const auto physicsOf = [&](const QString& tag) -> const Node* {
        for (const Node* p : model.physics())
            if (p->enabled && p->tag == tag) return p;
        return nullptr;
    };
    const auto scalar = [](const QString& type) {
        return type == QLatin1String("heat") || type == QLatin1String("electrostatics") || type == QLatin1String("currents");
    };
    const Node* derived = nullptr;
    if (const Node* d = model.results().child(QStringLiteral("derived")))
        for (const Node& c : d->children)
            if (c.enabled && c.tag == head) derived = &c;
    if (const Node* p = physicsOf(head); p && !derived) {
        if (!scalar(p->type)) {
            g.why = tr("an adjoint is set up for heat, electrostatics and electric currents alone");
            return g;
        }
        g.physics = p->tag;
        g.type = p->type;
        static const QRegularExpression energy(QStringLiteral("^(W|P|C\\w*|R\\w*|G\\w*)$"));
        if (energy.match(name).hasMatch()) {
            g.kind = Goal::Energy;
            g.words = tr("%1: an energy (or of one), whose error is its field's: the field's error").arg(expression);
            return g;
        }
        if (p->type == QLatin1String("heat") && (name == QLatin1String("Tmax") || name == QLatin1String("Tmin"))) {
            g.kind = Goal::Peak;
            g.highest = name == QLatin1String("Tmax");
            g.words = tr("%1: the field's error weighed by its adjoint's, of a source at the %2 point").arg(expression, g.highest ? tr("hottest") : tr("coldest"));
        } else {
            g.why = tr("%1 is not an energy, an integral or a peak of its field").arg(expression);
            return g;
        }
    } else if (derived) {
        const QString type = derived->type;
        const QString text = derived->text(QStringLiteral("expression")).trimmed();
        if (type != QLatin1String("integral") && type != QLatin1String("average")) {
            g.why = tr("%1 is a %2 node: an adjoint is set up for an integral or an average").arg(head, type);
            return g;
        }
        for (const Field& f : s.fields()) {
            if (!f.solved || !scalar(f.type)) continue;
            if (text == f.variable || text == f.tag + QLatin1Char('.') + f.variable) {
                g.physics = f.tag;
                g.type = f.type;
            }
        }
        if (g.physics.isEmpty()) {
            g.why = tr("%1 integrates %2: an adjoint is set up for a field itself (T, V)").arg(head, text);
            return g;
        }
        g.kind = levelOf(derived->text(QStringLiteral("level"))) == Level::Boundary ? Goal::Boundaries : Goal::Domains;
        g.selection = derived->props.value(QStringLiteral("selection"));
        g.words = tr("%1: the field's error weighed by its adjoint's, of a source where it is integrated").arg(expression);
    } else {
        g.why = tr("%1 is not a physics' value or a Derived Values node").arg(head);
        return g;
    }
    const int fi = s.fieldIndex(g.physics);
    if (fi < 0 || !linearIn(s, fi)) {
        g.kind = Goal::None;
        g.why = tr("%1's coefficients depend on the fields: an adjoint is set up for a linear physics").arg(g.physics);
        return g;
    }
    QString why;
    std::optional<Model> dual = dualModel(model, g.physics, study, &why);
    if (!dual) {
        g.kind = Goal::None;
        g.why = why;
        return g;
    }
    g.dual = *dual;
    return g;
}

/// The adjoint's model on \a s's mesh: its source added (a peak's where the field
/// is highest or lowest now).
std::optional<Model> dualOn(const Goal& g, const Solution& s, QString* why)
{
    Model m = g.dual;
    const bool heat = g.type == QLatin1String("heat"), es = g.type == QLatin1String("electrostatics");
    Node source;
    if (g.kind == Goal::Domains || g.kind == Goal::Peak) {
        source = m.make(heat ? QStringLiteral("heat/source") : es ? QStringLiteral("electrostatics/spacecharge") : QStringLiteral("currents/source"));
        const QString unit = heat ? QStringLiteral("W/m^3") : es ? QStringLiteral("C/m^3") : QStringLiteral("A/m^3");
        const QString key = heat ? QStringLiteral("Q0") : es ? QStringLiteral("rho") : QStringLiteral("Qj");
        if (g.kind == Goal::Domains) {
            source.props.insert(QStringLiteral("selection"), g.selection);
            source.props.insert(key, QStringLiteral("1[%1]").arg(unit));
        } else {
            // A narrow bump at the peak, of the size of its elements there.
            const int fi = s.fieldIndex(g.physics);
            const Field& f = s.fields()[std::size_t(fi)];
            const Mesh& mesh = s.mesh();
            int best = -1;
            double bestValue = 0;
            std::vector<char> on(mesh.nodes.size(), 0);
            for (int t = 0; t < int(mesh.triangles.size()); ++t) {
                const int d = mesh.domain[std::size_t(t)];
                if (d >= 0 && d < int(f.domains.size()) && f.domains[std::size_t(d)])
                    for (int v : mesh.triangles[std::size_t(t)]) on[std::size_t(v)] = 1;
            }
            for (int n = 0; n < int(mesh.nodes.size()) && n < f.space->size(); ++n) {
                if (!on[std::size_t(n)]) continue;
                const double v = f.value(0, n);
                if (best < 0 || (g.highest ? v > bestValue : v < bestValue)) {
                    best = n;
                    bestValue = v;
                }
            }
            if (best < 0) {
                *why = tr("the field has no nodes");
                return std::nullopt;
            }
            double h = kInfinity;
            for (int t = 0; t < int(mesh.triangles.size()); ++t)
                for (int v : mesh.triangles[std::size_t(t)])
                    if (v == best) h = std::min(h, sizeOf(mesh, t));
            const QPointF at = mesh.nodes[std::size_t(best)] * mesh.unitScale;
            const double width = 2 * h * mesh.unitScale;
            source.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("all"), true}});
            source.props.insert(key, QStringLiteral("exp(-((x - (%1)[m])^2 + (y - (%2)[m])^2)/(%3[m])^2)*1[%4]")
                                         .arg(at.x(), 0, 'g', 15)
                                         .arg(at.y(), 0, 'g', 15)
                                         .arg(width, 0, 'g', 15)
                                         .arg(unit));
        }
    } else if (g.kind == Goal::Boundaries) {
        source = m.make(heat ? QStringLiteral("heat/linesource") : es ? QStringLiteral("electrostatics/surfacecharge") : QStringLiteral("currents/normalcurrent"));
        source.props.insert(QStringLiteral("selection"), g.selection);
        source.props.insert(heat ? QStringLiteral("Qb") : es ? QStringLiteral("rhos") : QStringLiteral("Jn"),
                            heat ? QStringLiteral("1[W/m^2]") : es ? QStringLiteral("1[C/m^2]") : QStringLiteral("1[A/m^2]"));
    } else {
        return m;
    }
    if (!m.add(g.physics, source, -1, why)) return std::nullopt;
    return m;
}

/// Field \a fi of \a s's indicator, its total 1 (empty: none).
std::vector<double> fieldShare(const Solution& s, int fi)
{
    RefinementIndicator said;
    std::vector<double> squared(s.mesh().triangles.size(), 0.0);
    fieldIndicator(s, fi, squared, &said);
    if (said.fields.isEmpty()) return {};
    return squared;
}

} // namespace

// --------------------------------------------------------------------- The run

namespace {

/// The sizes that \a mesh asks for where it is finer than \a coarse: at most
/// kMostSizes of them, what falls in a cell of about its size gathered into the
/// least there.
QStringList sizesOf(const Mesh& mesh, const Mesh& coarse, double* smallest)
{
    *smallest = 0;
    // The coarse mesh's size near a point: a grid of what lies in each cell.
    const QRectF box = coarse.bounds();
    const double extent = std::max({box.width(), box.height(), 1e-300});
    const int cells = std::clamp(int(std::sqrt(double(coarse.triangles.size()))), 1, 512);
    const double cw = extent / cells;
    std::vector<double> least(std::size_t(cells) * std::size_t(cells), kInfinity);
    const auto cellAt = [&](QPointF p) {
        const int i = std::clamp(int((p.x() - box.left()) / cw), 0, cells - 1), j = std::clamp(int((p.y() - box.top()) / cw), 0, cells - 1);
        return std::size_t(j) * std::size_t(cells) + std::size_t(i);
    };
    for (int t = 0; t < int(coarse.triangles.size()); ++t) {
        const auto& v = coarse.triangles[std::size_t(t)];
        const QPointF c = (coarse.nodes[std::size_t(v[0])] + coarse.nodes[std::size_t(v[1])] + coarse.nodes[std::size_t(v[2])]) / 3.0;
        double& l = least[cellAt(c)];
        l = std::min(l, sizeOf(coarse, t));
    }
    struct Size {
        QPointF at;
        double h;
    };
    std::vector<Size> finer;
    for (int t = 0; t < int(mesh.triangles.size()); ++t) {
        const auto& v = mesh.triangles[std::size_t(t)];
        const QPointF c = (mesh.nodes[std::size_t(v[0])] + mesh.nodes[std::size_t(v[1])] + mesh.nodes[std::size_t(v[2])]) / 3.0;
        const double h = sizeOf(mesh, t);
        const double near = least[cellAt(c)];
        if (std::isfinite(near) && h >= 0.75 * near) continue;
        finer.push_back({c, h});
    }
    if (finer.empty()) return {};
    std::map<std::array<long long, 3>, Size> gathered;
    for (double reach = 1;; reach *= 2) {
        gathered.clear();
        for (const Size& s : finer) {
            const long long level = (long long)std::floor(std::log2(s.h));
            const double cell = reach * std::ldexp(2.0, int(level));
            const std::array<long long, 3> key{(long long)std::floor(s.at.x() / cell), (long long)std::floor(s.at.y() / cell), level};
            auto it = gathered.find(key);
            if (it == gathered.end() || s.h < it->second.h) gathered[key] = s;
        }
        if (int(gathered.size()) <= kMostSizes || reach > 1e6) break;
    }
    QStringList out;
    double low = kInfinity;
    for (const auto& [key, s] : gathered) {
        out << QStringLiteral("%1 %2 %3").arg(s.at.x(), 0, 'g', 10).arg(s.at.y(), 0, 'g', 10).arg(s.h, 0, 'g', 6);
        low = std::min(low, s.h);
    }
    *smallest = low;
    return out;
}

/// Each triangle's centre with the size wanted, for a mesh made anew from sizes:
/// \a factor of its size.
void addSizes(const Mesh& m, const std::vector<char>& marked, double factor, MeshSizes& sizes)
{
    for (int t = 0; t < int(m.triangles.size()); ++t) {
        if (!marked[std::size_t(t)]) continue;
        const auto& v = m.triangles[std::size_t(t)];
        const QPointF c = (m.nodes[std::size_t(v[0])] + m.nodes[std::size_t(v[1])] + m.nodes[std::size_t(v[2])]) / 3.0;
        const double h = factor * sizeOf(m, t);
        sizes.points.push_back({c.x(), c.y(), h});
        // (A floor in the sizes held every refinement back there: the microstrip
        // example's triangles stopped growing at its third mesh.)
        sizes.hmin = std::min(sizes.hmin, h);
    }
}

/// The model as the check solves it: its study's own adaptive rounds off (the
/// check does that), its sweep's left out (the check picks the points).
Model checkedModel(const Model& model, const QString& study)
{
    Model m = model;
    for (Node& s : m.root.children) {
        if (s.type != QLatin1String("study") || s.tag != study) continue;
        s.children.erase(std::remove_if(s.children.begin(), s.children.end(), [](const Node& c) { return c.type == QLatin1String("sweep"); }), s.children.end());
        for (Node& step : s.children) {
            step.props.remove(QStringLiteral("adaptive"));
            step.props.remove(QStringLiteral("adaptsteps"));
            step.props.remove(QStringLiteral("adaptphysics"));
        }
    }
    return m;
}

double unknownsOf(const Solution& s)
{
    double d = 0;
    for (const Field& f : s.fields())
        if (f.solved) {
            // (As fem_solve counts them, so the two routes' unknowns compare: an edge
            // element's field by its nodal space and components - the review of
            // 063142f found 6,793 here against fem_solve's 3,950 on one mesh.)
            d += f.unknowns >= 0 ? f.unknowns : (f.space ? f.space->size() * f.count() : 0);
        }
    return d;
}

int orderOf(const Solution& s)
{
    int order = 1;
    for (const Field& f : s.fields())
        if (f.solved && f.space) order = std::max(order, f.space->order());
    return order;
}

class Runner
{
public:
    Runner(const Model& model, const ConvergencePlan& plan, std::shared_ptr<ParameterScope> parameters, std::shared_ptr<const Topology> topology,
           std::shared_ptr<const Mesh> mesh, const Progress& progress, const ConvergenceListener& listener)
        : a_model(checkedModel(model, plan.study)), a_plan(plan), a_params(std::move(parameters)), a_topology(std::move(topology)),
          a_mesh(std::move(mesh)), a_progress(progress), a_listener(listener)
    {
        for (const ConvergenceWatch& w : plan.values) a_result.columns << w.expression;
        a_result.units = QStringList(a_result.columns);
        for (QString& u : a_result.units) u.clear();
        a_dims.assign(plan.values.size(), Dim());
    }

    ConvergenceResult run(const Model& original);

private:
    struct Series {
        std::vector<double> unknowns;
        std::vector<std::vector<double>> values;   // by column, by mesh
    };
    bool told(double fraction, const QString& what);
    /// The check of one point of a sweep (or the model's own).
    void runPoint(int point, const QHash<QString, QString>& overrides);
    bool readValues(Solution& s, std::vector<double>* out, QString* fatal);
    RefinementIndicator goalIndicator(const Solution& s, const std::vector<Goal>& goals, std::shared_ptr<ParameterScope> ps, std::shared_ptr<const Mesh> mesh);
    void finishPoint(int point, const Series& series, const std::vector<ValueEstimate>& estimates);
    std::vector<ValueEstimate> estimates(const Series& series, int from, int order) const;
    std::vector<ValueEstimate> afterCheck(const Series& series, const std::vector<ValueEstimate>& before, int from, int order) const;
    bool settled(const std::vector<ValueEstimate>& es, QString* why = nullptr) const;
    double toleranceOf(std::size_t column) const
    {
        const double t = a_plan.values[column].tolerance;
        return t > 0 ? t : a_plan.tolerance;
    }
    double relativeOf(const ValueEstimate& e) const
    {
        const double reference = std::isfinite(e.limit) ? std::abs(e.limit) : std::abs(e.value);
        if (e.error == 0) return 0;
        return reference > 0 ? e.error / reference : kInfinity;
    }
    bool valueSettled(std::size_t column, const ValueEstimate& e) const
    {
        if (e.trust == ValueEstimate::Few || !std::isfinite(e.error)) return false;
        // One that jumps about settles only where its jumps, with their room, are
        // well inside the tolerance: a first-order mesh of arcs moves its domain
        // as nodes go onto them, by a few parts in 1e5 for the coax.
        const double share = e.trust == ValueEstimate::Erratic ? 1.0 / 3 : 1.0;
        const double absolute = a_plan.values[column].absolute;
        return relativeOf(e) <= share * toleranceOf(column) || (absolute > 0 && e.error <= share * absolute);
    }

    const Model a_model;
    const ConvergencePlan& a_plan;
    std::shared_ptr<ParameterScope> a_params;
    std::shared_ptr<const Topology> a_topology;
    std::shared_ptr<const Mesh> a_mesh;
    const Progress& a_progress;
    const ConvergenceListener& a_listener;
    ConvergenceResult a_result;
    std::vector<Dim> a_dims;
    QStringList a_methods;
    QSet<QString> a_warned;
    bool a_stopped = false;
    double a_fractionFrom = 0, a_fractionSpan = 1;
    double a_lastFraction = 0;
    int a_pointIndex = 0;
    int a_rowIndex = 0;
    QStringList a_pointStatus;
};

bool Runner::told(double fraction, const QString& what)
{
    if (a_stopped) return false;
    if (!a_progress) return true;
    if (fraction < 0) fraction = a_lastFraction;
    a_lastFraction = fraction;
    if (!a_progress(std::min(0.995, a_fractionFrom + a_fractionSpan * std::clamp(fraction, 0.0, 1.0)), what)) {
        a_stopped = true;
        return false;
    }
    return true;
}

bool Runner::readValues(Solution& s, std::vector<double>* out, QString* fatal)
{
    SolutionSet set;
    // (A set of the one solution: its last time or frequency, the mode its step
    // chose.)
    set.solutions.push_back(std::shared_ptr<Solution>(&s, [](Solution*) {}));
    QString asked;
    const std::vector<Instance> at = instances(set, false, 0, {}, {}, modesAsked(set, QStringLiteral("auto")), &asked);
    if (at.empty()) {
        *fatal = asked.isEmpty() ? tr("no solution to read the values on") : asked;
        return false;
    }
    Solution* selected = select(set, at.back());
    if (!selected) {
        *fatal = tr("no solution to read the values on");
        return false;
    }
    out->assign(a_plan.values.size(), kNaN);
    for (std::size_t i = 0; i < a_plan.values.size(); ++i) {
        double v = kNaN;
        Dim d;
        QString why;
        if (!solutionValue(*selected, a_plan.values[i].expression, &v, &d, &why)) {
            QStringList names;
            for (const Variable& g : selected->globals()) names << g.name;
            names.sort(Qt::CaseInsensitive);
            *fatal = tr("%1: %2 - the results' global values are: %3").arg(a_plan.values[i].expression, why, names.isEmpty() ? tr("none") : names.mid(0, 60).join(QStringLiteral(", ")));
            return false;
        }
        (*out)[i] = v;
        a_dims[i] = d;
        a_result.units[int(i)] = d.isNone() ? QString() : dimName(d);
    }
    return true;
}

RefinementIndicator Runner::goalIndicator(const Solution& s, const std::vector<Goal>& goals, std::shared_ptr<ParameterScope> ps, std::shared_ptr<const Mesh> mesh)
{
    // Each value's share, its total 1: an energy's its field's error; an
    // integral's or a peak's the field's error times its adjoint's, element by
    // element (the dual-weighted residual's estimate, each factor recovered as
    // Zienkiewicz and Zhu's); one without an adjoint, every field's error.
    RefinementIndicator out;
    const std::size_t nt = s.mesh().triangles.size();
    std::vector<double> total(nt, 0.0);
    for (std::size_t c = 0; c < goals.size(); ++c) {
        const Goal& g = goals[c];
        std::vector<double> share;
        if (g.kind == Goal::Energy) {
            share = fieldShare(s, s.fieldIndex(g.physics));
        } else if (g.kind != Goal::None) {
            QString why;
            const std::optional<Model> dual = dualOn(g, s, &why);
            StudyResult z;
            if (dual) {
                z = solveStudy(*dual, ps, mesh, a_plan.study, [this](double, const QString&) { return !a_stopped && (!a_progress || told(-1, QString())); });
                if (a_stopped) return out;
                if (z.solution) {
                    a_result.adjoints += 1;
                    a_result.unknownsSolved += unknownsOf(*z.solution);
                }
            }
            const int zi = z.solution ? z.solution->fieldIndex(g.physics) : -1;
            const std::vector<double> u = fieldShare(s, s.fieldIndex(g.physics));
            const std::vector<double> w = zi >= 0 ? fieldShare(*z.solution, zi) : std::vector<double>();
            if (!u.empty() && w.size() == u.size()) {
                share.assign(nt, 0.0);
                double sum = 0;
                for (std::size_t t = 0; t < nt; ++t) sum += share[t] = std::sqrt(u[t] * w[t]);
                if (sum > 0)
                    for (double& v : share) v /= sum;
            } else {
                const QString said = tr("%1: its adjoint did not solve (%2): the fields' error stood in").arg(a_plan.values[c].expression, z.solution ? tr("nothing to estimate") : (dual ? z.error : why));
                if (!a_result.notes.contains(said)) a_result.notes << said;
            }
        }
        if (share.empty()) {
            const RefinementIndicator all = refinementIndicator(s, a_plan.physics);
            double sum = 0;
            for (double e : all.eta) sum += e * e;
            if (sum > 0) {
                share.assign(nt, 0.0);
                for (std::size_t t = 0; t < nt; ++t) share[t] = all.eta[t] * all.eta[t] / sum;
                for (const QString& f : all.fields)
                    if (!out.fields.contains(f)) out.fields << f;
            }
            out.without << all.without;
        } else {
            out.fields << tr("goal-oriented, of %1").arg(a_plan.values[c].expression);
        }
        for (std::size_t t = 0; t < share.size() && t < nt; ++t) total[t] += share[t];
    }
    if (out.fields.isEmpty()) return out;
    out.eta.resize(nt);
    for (std::size_t t = 0; t < nt; ++t) out.eta[t] = std::sqrt(total[t]);
    return out;
}

std::vector<ValueEstimate> Runner::estimates(const Series& series, int from, int order) const
{
    std::vector<ValueEstimate> out;
    const std::vector<double> ns(series.unknowns.begin() + from, series.unknowns.end());
    for (std::size_t c = 0; c < a_plan.values.size(); ++c) {
        std::vector<double> qs;
        for (std::size_t k = std::size_t(from); k < series.values.size(); ++k) qs.push_back(series.values[k][c]);
        out.push_back(estimateValue(ns, qs, order));
    }
    return out;
}

std::vector<ValueEstimate> Runner::afterCheck(const Series& series, const std::vector<ValueEstimate>& before, int from, int order) const
{
    std::vector<ValueEstimate> out = estimates(series, from, order);
    const std::size_t last = series.unknowns.size() - 1;
    const double grown = series.unknowns[last] / series.unknowns[last - 1];
    for (std::size_t c = 0; c < out.size() && c < before.size(); ++c) {
        ValueEstimate& e = out[c];
        if (e.trust == ValueEstimate::Unmoved) continue;
        const double q = series.values[last][c], d = q - series.values[last - 1][c];
        if (!std::isfinite(q) || !std::isfinite(d)) continue;
        // The check cut every element, so what it moved is read at the rate of
        // cutting every element - for a field with a corner, slower than the
        // rounds' - and at least as fast as a crack's (unknowns^-1/2, the slowest a
        // field of finite energy converges at): read at the rounds' rate alone, the
        // error said was up to four times the error made below it (the TSV's,
        // the resistor's, the microstrip's).
        // (With Roache's room of 1.25: the heater's transient settled slower still,
        // its error said 10 % below the error made without it.)
        const double slowest = grown > 1 ? 1.25 * std::abs(d) / (std::pow(grown, 0.5) - 1) : kInfinity;
        if (before[c].trust == ValueEstimate::Extrapolated && grown > 1) {
            const double beyond = d / (std::pow(grown, std::min(before[c].rate, double(order))) - 1);
            e.trust = ValueEstimate::Extrapolated;
            e.value = q;
            e.limit = q + beyond;
            e.error = std::max({2 * std::abs(beyond), std::abs(q - before[c].limit), slowest});
            e.rate = before[c].rate;
            e.why = tr("the check's change read at no faster than unknowns^-1/2 (a crack's), and against the limit the meshes before it gave at their rate of %1")
                        .arg(formatNumber(before[c].rate, 3));
        } else {
            e.trust = ValueEstimate::Bounded;
            e.value = q;
            e.limit = q;
            e.rate = kNaN;
            // (The crack's rate is the slowest there is, and 1.25 its room: more
            // room on it made the goal case check twice, 186,000 unknowns for a
            // value already within its tolerance at 20,000.)
            // (Room on the crack's rate too: without it the resistor's error was said
            // below the error made.)
            e.error = 2.4 * slowest;
            e.why = tr("the check's change read at no faster than unknowns^-1/2 (a crack's), with room: the meshes before it showed no rate to go by");
        }
    }
    return out;
}

bool Runner::settled(const std::vector<ValueEstimate>& es, QString* why) const
{
    for (std::size_t c = 0; c < es.size(); ++c)
        if (!valueSettled(c, es[c])) {
            if (why) *why = a_plan.values[c].expression;
            return false;
        }
    return true;
}

void Runner::finishPoint(int point, const Series&, const std::vector<ValueEstimate>& es)
{
    for (std::size_t c = 0; c < es.size(); ++c) {
        ConvergenceResult::Final f;
        f.point = point;
        f.column = int(c);
        f.value = es[c].value;
        f.limit = es[c].limit;
        f.error = es[c].error;
        f.relative = relativeOf(es[c]);
        f.rate = es[c].rate;
        f.tolerance = toleranceOf(c);
        f.trust = ValueEstimate::trustWord(es[c].trust);
        f.why = es[c].why;
        f.settled = valueSettled(c, es[c]);
        a_result.values.push_back(f);
    }
}

void Runner::runPoint(int point, const QHash<QString, QString>& overrides)
{
    const QString prefix = a_result.points.isEmpty() ? QString() : a_result.points.at(point) + QStringLiteral(": ");
    const auto stop = [&](const QString& status, const QString& why) {
        a_pointStatus << status;
        a_result.notes << prefix + why;
    };
    // Its parameters, geometry and first mesh: those it was given where it is the
    // model's own.
    std::shared_ptr<ParameterScope> ps = a_params;
    std::shared_ptr<const Topology> topology = a_topology;
    std::shared_ptr<const Mesh> mesh = a_mesh;
    if (!overrides.isEmpty()) {
        ps = evaluateParameters(a_model, overrides);
        topology = nullptr;
        mesh = nullptr;
    }
    if (!ps->errors.isEmpty()) {
        QStringList list;
        for (auto it = ps->errors.cbegin(); it != ps->errors.cend(); ++it) list << tr("%1: %2").arg(it.key(), it.value());
        return stop(QStringLiteral("failed"), tr("the parameters: %1").arg(list.join(QLatin1String("; "))));
    }
    if (!told(0, tr("Building the geometry"))) return;
    if (!topology) {
        const GeometryBuild g = buildGeometry(a_model, *ps);
        if (!g.ok()) {
            const Node* n = a_model.find(g.failedAt);
            return stop(QStringLiteral("failed"), tr("the geometry: %1: %2").arg(n ? n->name() : g.failedAt, g.errors.value(g.failedAt)));
        }
        topology = g.topology;
    }
    QString why;
    MeshSizes sizes = meshSizes(a_model, *ps, *topology, &why);
    if (!mesh) {
        const MeshBuild built = buildMesh(topology, sizes, [this](double f, const QString& w) { return told(0.01 * f, w); });
        if (a_stopped) return;
        if (!built.mesh) return stop(QStringLiteral("failed"), tr("the mesh: %1").arg(built.error));
        for (const QString& w : built.warnings)
            if (!a_warned.contains(w)) {
                a_warned.insert(w);
                a_result.notes << w;
            }
        mesh = built.mesh;
    }
    // A domain that must stay Delaunay (a semiconductor's box method) is meshed
    // anew each time; every other mesh is cut, its old nodes kept.
    bool remeshes = false;
    for (char d : sizes.delaunay) remeshes = remeshes || d;
    const std::shared_ptr<const Mesh> first = mesh;
    std::vector<std::shared_ptr<const Mesh>> meshes;   // by row of this point

    Series series;
    int seriesFrom = 0;
    enum Mode { Adaptive, Uniform } mode = a_plan.refine == QLatin1String("uniform") ? Uniform : Adaptive;
    QString kind = QStringLiteral("start");
    bool checking = false;
    bool caught = false;
    double checkMoved = 0;
    QStringList said;
    std::shared_ptr<Solution> previous;
    double smallestAllowed = 1e-6 * std::max(topology->bounds.width(), topology->bounds.height());
    int solvedHere = 0;
    QElapsedTimer timer;
    timer.start();
    double meshSeconds = 0;
    std::vector<ValueEstimate> es;
    int order = 1;
    const bool goalOriented = a_plan.refine == QLatin1String("goal");
    std::vector<Goal> goals;
    if (mode == Uniform && !a_methods.contains(tr("every element cut, as asked"))) a_methods << tr("every element cut, as asked");

    for (;;) {
        if (solvedHere >= a_plan.maxMeshes) {
            stop(QStringLiteral("meshes"), tr("it stopped at its limit of %1 meshes, the values not yet settled").arg(a_plan.maxMeshes));
            break;
        }
        // The solve.
        const double N = double(mesh->triangles.size());
        const auto part = [this, solvedHere, N](double f, const QString& w) {
            return told((solvedHere + f) / double(a_plan.maxMeshes),
                        tr("Mesh %1 (%2 triangles): %3").arg(solvedHere + 1).arg(qRound64(N)).arg(w));
        };
        timer.restart();
        StudyResult solved;
        if (solvedHere == 0 || hasExpressionFlags(a_model.root, a_plan.study, false)) {
            // (Through the study's runner: what it checks and reads first.)
            StudyRun run = runStudy(a_model, ps, topology, mesh, a_plan.study, part);
            for (const QString& w : std::as_const(run.warnings))
                if (!a_warned.contains(w)) {
                    a_warned.insert(w);
                    a_result.notes << w;
                }
            if (run.ok()) solved.solution = run.solutions.front();
            else solved.error = run.error;
        } else {
            solved = solveStudy(a_model, ps, mesh, a_plan.study, part, previous.get());
            for (const QString& w : std::as_const(solved.warnings))
                if (!a_warned.contains(w)) {
                    a_warned.insert(w);
                    a_result.notes << w;
                }
        }
        const double seconds = timer.elapsed() / 1000.0 + meshSeconds;
        if (a_stopped) break;
        if (!solved.solution) {
            if (solvedHere == 0) {
                stop(QStringLiteral("failed"), tr("the mesh as it is did not solve: %1").arg(solved.error));
            } else {
                stop(QStringLiteral("failed"), tr("the mesh of %1 triangles did not solve: %2. What the coarser meshes gave stands, unconfirmed.")
                                                   .arg(mesh->triangles.size())
                                                   .arg(solved.error.section(QLatin1Char('\n'), 0, 0).left(300)));
            }
            break;
        }
        ++solvedHere;
        order = orderOf(*solved.solution);
        // The values.
        std::vector<double> values;
        QString fatal;
        if (!readValues(*solved.solution, &values, &fatal)) {
            if (solvedHere == 1) {
                a_result.error = fatal;
                a_pointStatus << QStringLiteral("failed");
                return;
            }
            values.assign(a_plan.values.size(), kNaN);
            a_result.notes << prefix + tr("mesh %1: %2").arg(solvedHere).arg(fatal.left(200));
        }
        ConvergenceRow row;
        row.index = ++a_rowIndex;
        row.point = point;
        row.kind = kind;
        row.triangles = int(mesh->triangles.size());
        row.unknowns = unknownsOf(*solved.solution);
        row.smallest = kInfinity;
        for (int t = 0; t < row.triangles; ++t) row.smallest = std::min(row.smallest, sizeOf(*mesh, t));
        row.seconds = seconds;
        row.values = values;
        a_result.solves += 1;
        a_result.unknownsSolved += row.unknowns;
        a_result.largest = std::max(a_result.largest, row.unknowns);
        series.unknowns.push_back(row.unknowns);
        meshes.push_back(mesh);
        series.values.push_back(values);
        // The indicator where it will be asked for.
        RefinementIndicator indicator;
        if (mode == Adaptive) {
            timer.restart();
            if (goalOriented && goals.empty()) {
                for (const ConvergenceWatch& w : a_plan.values) goals.push_back(goalOf(a_model, *solved.solution, a_plan.study, w.expression));
                for (std::size_t c = 0; c < goals.size(); ++c) {
                    if (goals[c].kind == Goal::None) a_result.notes << prefix + tr("Goal-oriented: %1 has no adjoint here (%2): the fields' error stands in for it.").arg(a_plan.values[c].expression, goals[c].why);
                    else if (!a_methods.contains(goals[c].words)) a_methods << goals[c].words;
                }
            }
            if (goalOriented) {
                indicator = goalIndicator(*solved.solution, goals, ps, mesh);
                if (a_stopped) break;
            } else {
                indicator = refinementIndicator(*solved.solution, a_plan.physics);
            }
            if (!indicator.eta.empty()) {
                double total = 0;
                for (double e : indicator.eta) total += e * e;
                row.indicator = std::sqrt(total);
            }
            meshSeconds = timer.elapsed() / 1000.0;
        } else {
            meshSeconds = 0;
        }
        a_result.rows.push_back(row);
        if (a_listener.row) a_listener.row(row, a_result.columns, a_result.points);
        // The solution kept for the next one's start, the one before freed.
        previous = solved.solution;
        // How the values stand. (The check's mesh is of another kind than the
        // rounds': a rate through it says little. It is read by theirs.)
        const std::vector<ValueEstimate> before = es;
        es = checking ? afterCheck(series, before, seriesFrom, order) : estimates(series, seriesFrom, order);
        QString pending;
        const bool within = settled(es, &pending);
        if (checking) {
            // The check: nothing chose where it was refined, so a value that
            // moves here was moved by what the rounds did not look at.
            checking = false;
            QStringList movedBy;
            double worstOver = 0;
            const std::vector<double>& was = series.values[series.values.size() - 2];
            for (std::size_t c = 0; c < values.size(); ++c) {
                const double moved = std::abs(values[c] - was[c]);
                const double reference = std::abs(values[c]);
                const double rel = reference > 0 ? moved / reference : (moved == 0 ? 0 : kInfinity);
                const double allowed = toleranceOf(c);
                const bool ok = std::isfinite(rel) && (rel <= allowed || (a_plan.values[c].absolute > 0 && moved <= a_plan.values[c].absolute));
                if (!ok) {
                    movedBy << tr("%1 by %2 %").arg(a_plan.values[c].expression, formatNumber(rel * 100, 3));
                    worstOver = std::max(worstOver, std::isfinite(rel) ? rel / allowed : 1e300);
                }
                if (c < before.size() && std::isfinite(before[c].error) && before[c].error > 0) checkMoved = std::max(checkMoved, moved / before[c].error);
            }
            if (!movedBy.isEmpty()) {
                caught = true;
                a_result.falseConvergence = true;
                said << tr("the check cut every element and moved %1: the values had agreed on meshes refined elsewhere. Nothing before it is counted now; every element is cut from here.")
                            .arg(movedBy.join(QStringLiteral(", ")));
                if (a_listener.line) a_listener.line(said.last());
                mode = Uniform;
                seriesFrom = int(series.unknowns.size()) - 1;
                es = estimates(series, seriesFrom, order);
                a_methods << tr("every element cut, after the check caught the rounds' false agreement");
            } else if (within) {
                said << tr("the check, every element cut, moved each value by less than its tolerance");
                if (checkMoved > 10) said << tr("(it moved a value %1 times what the estimate before it said: that estimate was too hopeful, and only the one after it stands)").arg(formatNumber(checkMoved, 3));
                a_pointStatus << QStringLiteral("converged");
                break;
            } else {
                // One more cut of every element: the mesh before the check, the check's
                // and this one are nested meshes nothing chose, whose rate can be read -
                // cheaper than more rounds and a check of their own (the goal case
                // checked twice, 186,000 unknowns, for a value within its tolerance
                // at 20,000).
                said << tr("the check moved no value past its tolerance, but %1's error after it is still above it: every element cut once more, for a rate of "
                           "meshes nothing chose")
                            .arg(pending);
                mode = Uniform;
                seriesFrom = int(series.unknowns.size()) - 2;
                es = estimates(series, seriesFrom, order);
            }
        } else if (within && solvedHere >= 2) {
            if (mode == Uniform && int(series.unknowns.size()) - seriesFrom >= 3) {
                said << tr("every element was cut each time, so no indicator chose where");
                a_pointStatus << QStringLiteral("converged");
                break;
            }
            if (mode == Adaptive && !a_plan.check) {
                said << tr("no independent check was asked for: the agreement of meshes refined by one indicator is all there is");
                a_pointStatus << QStringLiteral("converged");
                break;
            }
            if (mode == Adaptive) checking = true;
        }
        // Past the cap on what the rate says it needs: no use going on.
        if (!checking && !within) {
            bool projected = false;
            double needs = 0;
            QString which;
            for (std::size_t c = 0; c < es.size(); ++c) {
                if (es[c].trust != ValueEstimate::Extrapolated || !es[c].steady || !std::isfinite(es[c].rate) || valueSettled(c, es[c])) continue;
                const double ratio = relativeOf(es[c]) / toleranceOf(c);
                const double wanted = row.unknowns * std::pow(ratio, 1 / es[c].rate);
                if (wanted > 2 * a_plan.maxUnknowns && wanted > needs) {
                    projected = true;
                    needs = wanted;
                    which = a_plan.values[c].expression;
                }
            }
            if (projected && solvedHere >= 4) {
                stop(QStringLiteral("projected"), tr("at the rate it has shown, %1 would need about %2 unknowns to settle, past the limit of %3: it stopped here")
                                                      .arg(which, formatNumber(needs, 3), formatNumber(a_plan.maxUnknowns, 6)));
                break;
            }
        }
        // The next mesh.
        const bool all = checking || mode == Uniform;
        std::vector<char> marked;
        QString nextKind;
        if (all) {
            marked.assign(mesh->triangles.size(), 1);
            nextKind = checking ? QStringLiteral("check") : QStringLiteral("uniform");
        } else {
            if (indicator.eta.empty()) {
                // No field gives one: every element cut, and said.
                for (const QString& w : std::as_const(indicator.without))
                    if (!a_methods.contains(w)) a_methods << w;
                a_methods << tr("every element cut: no solved field gives an error indicator");
                mode = Uniform;
                marked.assign(mesh->triangles.size(), 1);
                nextKind = QStringLiteral("uniform");
            } else {
                for (const QString& w : std::as_const(indicator.fields))
                    if (!a_methods.contains(w)) a_methods << w;
                for (const QString& w : std::as_const(indicator.without))
                    if (!a_result.notes.contains(w)) a_result.notes << w;
                marked = markBulk(indicator.eta);
                nextKind = goalOriented ? QStringLiteral("goal") : QStringLiteral("adaptive");
            }
        }
        // None past the smallest size a mesh's rounding allows.
        int count = 0;
        for (int t = 0; t < int(marked.size()); ++t) {
            if (marked[std::size_t(t)] && sizeOf(*mesh, t) < smallestAllowed) marked[std::size_t(t)] = 0;
            count += marked[std::size_t(t)] ? 1 : 0;
        }
        if (count == 0) {
            stop(QStringLiteral("stalled"), tr("every element marked is at the smallest a mesh of this size allows: it cannot be refined further"));
            break;
        }
        // Past the cap: not solved. (A mesh grows at most four times over; cut
        // by its marks, about 1 + 3 of each marked's share, closure included.)
        const double grown = row.unknowns * (remeshes ? (all ? 2.0 : 1.0 + double(count) / marked.size()) : 1.0 + 1.5 * double(count) / marked.size());
        if (grown > a_plan.maxUnknowns) {
            stop(QStringLiteral("unknowns"), tr("the next mesh would have about %1 unknowns, past the limit of %2").arg(formatNumber(grown, 3), formatNumber(a_plan.maxUnknowns, 6)));
            break;
        }
        if (!told(double(solvedHere) / a_plan.maxMeshes, tr("Refining the mesh"))) break;
        timer.restart();
        std::shared_ptr<const Mesh> next;
        if (remeshes) {
            addSizes(*mesh, marked, all ? M_SQRT1_2 : 0.5, sizes);
            if (all) {
                for (double& l : sizes.layerLateral) l *= M_SQRT1_2;
                for (double& l : sizes.layerWall) l *= M_SQRT1_2;
                for (int& c : sizes.layerCount)
                    if (c > 0) c = std::max(c + 1, int(std::ceil(c * M_SQRT2)));
                for (int& c : sizes.boundaryCount)
                    if (c > 0) c = std::max(c + 1, int(std::ceil(c * M_SQRT2)));
            }
            const MeshBuild built = buildMesh(topology, sizes, [this, solvedHere](double f, const QString& w) {
                return told((solvedHere + 0.1 * f) / double(a_plan.maxMeshes), w);
            });
            if (a_stopped) break;
            if (!built.mesh) {
                stop(QStringLiteral("failed"), tr("the finer mesh: %1").arg(built.error));
                break;
            }
            next = built.mesh;
            if (double(next->triangles.size()) < 1.05 * double(mesh->triangles.size())) {
                stop(QStringLiteral("stalled"), tr("meshed anew, it hardly grew (%1 triangles to %2): what holds it is outside the sizes it can ask for, a layered region's grid or the like")
                                                    .arg(mesh->triangles.size())
                                                    .arg(next->triangles.size()));
                break;
            }
        } else {
            next = bisectMesh(*mesh, marked, sizes.periodic);
            if (!next) {
                stop(QStringLiteral("failed"), tr("the mesh could not be cut"));
                break;
            }
        }
        meshSeconds += timer.elapsed() / 1000.0;
        mesh = next;
        kind = nextKind;
    }
    // (Ended without a status of its own: stopped.)
    if (a_pointStatus.size() <= point) a_pointStatus << QStringLiteral("cancelled");
    Q_UNUSED(caught);
    if (!series.unknowns.empty()) finishPoint(point, series, es);
    // What stands behind the verdict.
    const QString method2 = (remeshes ? tr("meshed anew at each step, a semiconductor's domains kept Delaunay") : tr("elements cut by their longest edges, each mesh inside the one before"));
    if (!a_methods.contains(method2)) a_methods.prepend(method2);
    QStringList evidence;
    if (series.unknowns.size() >= 2)
        evidence << prefix + tr("%1 meshes, from %2 to %3 unknowns").arg(series.unknowns.size()).arg(formatNumber(series.unknowns.front(), 6), formatNumber(series.unknowns.back(), 6));
    else if (series.unknowns.size() == 1)
        evidence << prefix + tr("one mesh of %1 unknowns: nothing to compare it with").arg(formatNumber(series.unknowns.front(), 6));
    for (const QString& s : std::as_const(said)) evidence << prefix + s;
    a_result.evidence << evidence;
    // The refined mesh: the check's largest is not asked of the model, the last the
    // rounds refined is. (Sizes for Keep Refined Mesh.)
    // The mesh kept: the coarsest whose values are each within their tolerance of
    // the finest's limit, the finest's error counted - not the check's, which was
    // five to ten times what was enough (the review of 063142f, its SOI strip:
    // 4,140 triangles enough, 37,190 kept). The finest where none is.
    if (point == int(a_result.points.size()) - 1 || a_result.points.isEmpty()) {
        a_result.mesh = mesh;
        std::size_t chosen = meshes.empty() ? 0 : meshes.size() - 1;
        if (a_pointStatus.value(point) == QLatin1String("converged") && es.size() == a_plan.values.size())
            for (std::size_t k = 0; k < meshes.size(); ++k) {
                bool enough = true;
                for (std::size_t c = 0; c < es.size() && enough; ++c) {
                    const double v = series.values[k][c];
                    const double reference = std::isfinite(es[c].limit) ? es[c].limit : es[c].value;
                    const double off = std::abs(v - reference) + (std::isfinite(es[c].error) ? es[c].error : kInfinity);
                    const double absolute = a_plan.values[c].absolute;
                    enough = std::isfinite(v) && (off <= toleranceOf(c) * std::abs(reference) || (absolute > 0 && off <= absolute));
                }
                if (enough && series.unknowns[k] < series.unknowns[chosen]) chosen = k;
            }
        if (!meshes.empty()) {
            const std::shared_ptr<const Mesh>& kept = meshes[chosen];
            a_result.sizes = kept != first ? sizesOf(*kept, *first, &a_result.smallestSize) : QStringList();
            a_result.keptRow = a_result.rows.empty() ? 0 : a_result.rows[a_result.rows.size() - meshes.size() + chosen].index;
            if (!a_result.sizes.isEmpty())
                a_result.notes << prefix + tr("Keep Refined Mesh keeps mesh %1's sizes (%2 triangles), the coarsest whose values are within their tolerances of the limit.")
                                               .arg(a_result.keptRow)
                                               .arg(kept->triangles.size());
        }
    }
}

ConvergenceResult Runner::run(const Model& original)
{
    QElapsedTimer wall;
    wall.start();
    const Node* node = convergenceNode(original, a_plan.tag);
    if (node) a_result.setup = convergenceSetup(original, *node);
    // The points of a sweep.
    std::vector<QHash<QString, QString>> overrides;
    const Node* study = nullptr;
    for (const Node* s : original.studies())
        if (s->tag == a_plan.study) study = s;
    const Node* sweep = nullptr;
    if (study)
        for (const Node& c : study->children)
            if (c.type == QLatin1String("sweep") && c.enabled) sweep = &c;
    if (sweep && a_plan.sweep != QLatin1String("own")) {
        QString why;
        const std::optional<SweepPlan> plan = sweepPlan(original, *sweep, *a_params, &why);
        if (!plan) {
            a_result.error = tr("its study's sweep: %1").arg(why);
            return a_result;
        }
        const int count = int(plan->overrides.size());
        std::vector<int> picked;
        if (a_plan.sweep == QLatin1String("first")) picked = {0};
        else if (a_plan.sweep == QLatin1String("last")) picked = {count - 1};
        else if (a_plan.sweep == QLatin1String("ends")) picked = count > 1 ? std::vector<int>{0, count - 1} : std::vector<int>{0};
        else
            for (int i = 0; i < count; ++i) picked.push_back(i);
        for (int i : picked) {
            if (i < 0 || i >= count) continue;
            overrides.push_back(plan->overrides[std::size_t(i)]);
            a_result.points << plan->labels.value(i);
        }
    } else if (sweep) {
        a_result.notes << tr("Its study's sweep is not run: the model's own values of its parameters are checked (\"own\").");
    }
    if (overrides.empty()) overrides.push_back({});
    const int count = int(overrides.size());
    for (int p = 0; p < count; ++p) {
        a_fractionFrom = double(p) / count;
        a_fractionSpan = 1.0 / count;
        runPoint(p, overrides[std::size_t(p)]);
        if (!a_result.error.isEmpty() || a_stopped) break;
    }
    a_result.seconds = wall.elapsed() / 1000.0;
    if (!a_result.error.isEmpty()) return a_result;
    // The verdict: the worst of the points'.
    static const QStringList order{QStringLiteral("cancelled"), QStringLiteral("failed"), QStringLiteral("stalled"), QStringLiteral("projected"),
                                   QStringLiteral("unknowns"), QStringLiteral("meshes"), QStringLiteral("converged")};
    a_result.status = QStringLiteral("converged");
    for (const QString& s : order)
        if (a_pointStatus.contains(s)) {
            a_result.status = s;
            break;
        }
    if (a_pointStatus.isEmpty()) a_result.status = QStringLiteral("cancelled");
    a_result.converged = a_result.status == QLatin1String("converged");
    QStringList methods;
    for (const QString& m : std::as_const(a_methods))
        if (!methods.contains(m)) methods << m;
    a_result.method = methods.join(QStringLiteral("; "));
    // In a line.
    QStringList parts;
    QStringList worst;
    for (const ConvergenceResult::Final& f : a_result.values)
        if (!f.settled) worst << a_result.columns.value(f.column);
    worst.removeDuplicates();
    if (a_result.converged) {
        double largest = 0;
        for (const ConvergenceResult::Final& f : a_result.values)
            if (std::isfinite(f.relative)) largest = std::max(largest, f.relative);
        parts << tr("Converged: each value within its tolerance, the largest estimated error %1 %").arg(formatNumber(largest * 100, 3));
    } else if (a_result.status == QLatin1String("cancelled")) {
        parts << tr("Cancelled, not converged");
    } else if (a_result.status == QLatin1String("failed")) {
        parts << tr("Not converged: a solve failed");
    } else if (a_result.status == QLatin1String("stalled")) {
        parts << tr("Not converged: the mesh could not be refined further");
    } else if (a_result.status == QLatin1String("projected")) {
        parts << tr("Not converged: settling would take far more unknowns than allowed");
    } else if (a_result.status == QLatin1String("unknowns")) {
        parts << tr("Not converged: stopped at the limit on unknowns");
    } else {
        parts << tr("Not converged: stopped at the limit on meshes");
    }
    if (!a_result.converged && !worst.isEmpty()) parts << tr("not settled: %1").arg(worst.join(QStringLiteral(", ")));
    parts << tr("%1 solves, %2 unknowns solved in all").arg(a_result.solves).arg(formatNumber(a_result.unknownsSolved, 6));
    if (a_result.falseConvergence) parts << tr("a false agreement was caught");
    a_result.summary = parts.join(QStringLiteral("; "));
    return a_result;
}

} // namespace

ConvergenceResult runConvergence(const Model& model, const QString& tag, std::shared_ptr<ParameterScope> parameters,
                                 std::shared_ptr<const Topology> topology, std::shared_ptr<const Mesh> mesh, const Progress& progress,
                                 const ConvergenceListener& listener)
{
    ConvergenceResult failed;
    const Node* node = convergenceNode(model, tag);
    if (!node) {
        failed.error = tag.isEmpty() ? tr("The model has no mesh convergence check.") : tr("There is no mesh convergence check %1.").arg(tag);
        return failed;
    }
    if (!parameters) parameters = evaluateParameters(model);
    failed.setup = convergenceSetup(model, *node);
    QString why;
    const std::optional<ConvergencePlan> plan = convergencePlan(model, *node, *parameters, &why);
    if (!plan) {
        failed.error = why;
        return failed;
    }
    Runner runner(model, *plan, parameters, std::move(topology), std::move(mesh), progress, listener);
    return runner.run(model);
}

bool keepRefinedMesh(Model& model, const Node& node, QString* error)
{
    const ConvergenceResult r = ConvergenceResult::fromJson(node.value(QStringLiteral("result")).toObject());
    if (r.sizes.isEmpty()) {
        if (error)
            *error = r.converged && r.keptRow == 1 ? tr("%1: the model's own mesh is within the tolerance already: nothing to keep.").arg(node.name())
                                                   : tr("%1: no refined mesh to keep: it has not been run, or refined nothing.").arg(node.name());
        return false;
    }
    Node* mesh = model.find(model.mesh().tag);
    if (!mesh) {
        if (error) *error = tr("The model has no mesh node.");
        return false;
    }
    mesh->children.erase(std::remove_if(mesh->children.begin(), mesh->children.end(), [](const Node& c) { return c.type == QLatin1String("mesh/adapted"); }),
                         mesh->children.end());
    Node kept = model.make(QStringLiteral("mesh/adapted"));
    kept.props.insert(QStringLiteral("points"), QJsonArray::fromStringList(r.sizes));
    kept.props.insert(QStringLiteral("hmin"), r.smallestSize);
    QString why;
    if (!model.add(mesh->tag, kept, -1, &why)) {
        if (error) *error = why;
        return false;
    }
    return true;
}

} // namespace qucs_s::fem
