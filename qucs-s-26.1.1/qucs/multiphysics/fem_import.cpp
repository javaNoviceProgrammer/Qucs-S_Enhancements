/*
 * fem_import.cpp - DXF and SVG drawings read into a geometry's curves
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_import.h"

#include <QCoreApplication>
#include <QHash>
#include <QList>
#include <QMap>
#include <QRegularExpression>
#include <QXmlStreamReader>

#include <algorithm>
#include <cmath>
#include <map>

namespace qucs_s::fem {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::fem::Import", text);
}

/// x' = a x + c y + e, y' = b x + d y + f (an SVG matrix's order).
struct Map2 {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    QPointF map(QPointF p) const { return {a * p.x() + c * p.y() + e, b * p.x() + d * p.y() + f}; }
    QPointF vec(QPointF v) const { return {a * v.x() + c * v.y(), b * v.x() + d * v.y()}; }
    /// This after \a o: (this ∘ o)(p) = this(o(p)).
    Map2 operator*(const Map2& o) const
    {
        Map2 r;
        r.a = a * o.a + c * o.b;
        r.b = b * o.a + d * o.b;
        r.c = a * o.c + c * o.d;
        r.d = b * o.c + d * o.d;
        r.e = a * o.e + c * o.f + e;
        r.f = b * o.e + d * o.f + f;
        return r;
    }
    static Map2 translate(double x, double y)
    {
        Map2 m;
        m.e = x;
        m.f = y;
        return m;
    }
    static Map2 scale(double x, double y)
    {
        Map2 m;
        m.a = x;
        m.d = y;
        return m;
    }
    static Map2 rotate(double radians)
    {
        Map2 m;
        m.a = std::cos(radians);
        m.b = std::sin(radians);
        m.c = -std::sin(radians);
        m.d = std::cos(radians);
        return m;
    }
};

Curve mapped(Curve c, const Map2& m)
{
    c.c = m.map(c.c);
    c.a = m.vec(c.a);
    c.b = m.vec(c.b);
    for (QPointF& p : c.pts) p = m.map(p);
    return c;
}

Curve line(QPointF from, QPointF to)
{
    Curve c;
    c.kind = Curve::Line;
    c.c = from;
    c.a = to - from;
    c.t0 = 0;
    c.t1 = 1;
    return c;
}

Curve arc(QPointF center, QPointF a, QPointF b, double t0, double t1)
{
    Curve c;
    c.kind = Curve::Arc;
    c.c = center;
    c.a = a;
    c.b = b;
    c.t0 = t0;
    c.t1 = t1;
    return c;
}

Curve polyline(const std::vector<QPointF>& pts)
{
    Curve c;
    c.kind = Curve::Polyline;
    c.pts = pts;
    c.t0 = 0;
    c.t1 = double(pts.size()) - 1;
    return c;
}

/// A whole ellipse (circle) as four quarters, as the geometry's own are.
ImportedPath fullEllipse(QPointF center, QPointF a, QPointF b)
{
    ImportedPath p;
    p.closed = true;
    for (int q = 0; q < 4; ++q) p.curves.push_back(arc(center, a, b, q * M_PI / 2, (q + 1) * M_PI / 2));
    return p;
}

QPointF startOf(const Curve& c)
{
    return c.at(c.t0);
}

QPointF endOf(const Curve& c)
{
    return c.at(c.t1);
}

Curve reversed(Curve c)
{
    std::swap(c.t0, c.t1);
    return c;
}

/// A closed path of one curve in four (the topology walks no loop of one).
void splitSingle(ImportedPath& p)
{
    if (!p.closed || p.curves.size() != 1) return;
    const Curve c = p.curves.front();
    p.curves.clear();
    if (c.kind == Curve::Polyline) {
        const int n = int(c.pts.size()) - 1;
        if (n < 4) {
            for (int i = 0; i < n; ++i) p.curves.push_back(line(c.pts[std::size_t(i)], c.pts[std::size_t(i) + 1]));
            return;
        }
        for (int q = 0; q < 4; ++q) {
            Curve piece = c;
            piece.t0 = double(q * n / 4);
            piece.t1 = double((q + 1) * n / 4);
            p.curves.push_back(piece);
        }
    } else if (c.kind == Curve::Arc) {
        for (int q = 0; q < 4; ++q) {
            Curve piece = c;
            piece.t0 = c.t0 + (c.t1 - c.t0) * q / 4;
            piece.t1 = c.t0 + (c.t1 - c.t0) * (q + 1) / 4;
            p.curves.push_back(piece);
        }
    } else {
        p.curves.push_back(c);
        p.closed = false;
    }
}

/// Open pieces joined end to end into paths where they meet (within a
/// millionth of the drawing's size), closed where a path comes back.
std::vector<ImportedPath> joined(std::vector<ImportedPath> paths)
{
    std::vector<ImportedPath> out, open;
    QRectF box;
    for (ImportedPath& p : paths) {
        if (p.curves.empty()) continue;
        for (const Curve& c : p.curves) box |= QRectF(startOf(c), endOf(c)).normalized().adjusted(-1e-30, -1e-30, 1e-30, 1e-30);
        (p.closed ? out : open).push_back(std::move(p));
    }
    const double tol = 1e-6 * std::max({box.width(), box.height(), 1e-30});
    auto near = [tol](QPointF a, QPointF b) { return std::hypot(a.x() - b.x(), a.y() - b.y()) <= tol; };
    // A grid of the open paths' ends.
    auto key = [tol](QPointF p) { return std::pair<long long, long long>(std::llround(p.x() / tol / 4), std::llround(p.y() / tol / 4)); };
    std::map<std::pair<long long, long long>, std::vector<int>> ends;
    for (int i = 0; i < int(open.size()); ++i)
        for (QPointF p : {startOf(open[std::size_t(i)].curves.front()), endOf(open[std::size_t(i)].curves.back())}) ends[key(p)].push_back(i);
    std::vector<char> used(open.size(), 0);
    auto partner = [&](QPointF p, int self) {
        const auto k = key(p);
        for (long long dx = -1; dx <= 1; ++dx)
            for (long long dy = -1; dy <= 1; ++dy) {
                auto it = ends.find({k.first + dx, k.second + dy});
                if (it == ends.end()) continue;
                for (int j : it->second) {
                    if (j == self || used[std::size_t(j)]) continue;
                    const ImportedPath& q = open[std::size_t(j)];
                    if (near(startOf(q.curves.front()), p) || near(endOf(q.curves.back()), p)) return j;
                }
            }
        return -1;
    };
    auto reverse = [](ImportedPath& p) {
        std::reverse(p.curves.begin(), p.curves.end());
        for (Curve& c : p.curves) c = reversed(c);
    };
    auto degree = [&](QPointF p, int self) {
        int count = 0;
        const auto k = key(p);
        for (long long dx = -1; dx <= 1; ++dx)
            for (long long dy = -1; dy <= 1; ++dy) {
                auto it = ends.find({k.first + dx, k.second + dy});
                if (it == ends.end()) continue;
                for (int j : it->second)
                    if (j != self && (near(startOf(open[std::size_t(j)].curves.front()), p) || near(endOf(open[std::size_t(j)].curves.back()), p)))
                        ++count;
            }
        return count;
    };
    // Chains from loose ends first, then what is left (rings).
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < int(open.size()); ++i) {
            if (used[std::size_t(i)]) continue;
            if (pass == 0 && degree(startOf(open[std::size_t(i)].curves.front()), i) > 0
                && degree(endOf(open[std::size_t(i)].curves.back()), i) > 0)
                continue;
            ImportedPath chain = open[std::size_t(i)];
            used[std::size_t(i)] = 1;
            if (pass == 0 && degree(startOf(chain.curves.front()), i) > 0) reverse(chain);
            for (;;) {
                const QPointF end = endOf(chain.curves.back());
                if (near(end, startOf(chain.curves.front())) && chain.curves.size() > 1) {
                    chain.closed = true;
                    break;
                }
                const int j = partner(end, -1);
                if (j < 0) break;
                ImportedPath next = open[std::size_t(j)];
                used[std::size_t(j)] = 1;
                if (!near(startOf(next.curves.front()), end)) reverse(next);
                for (Curve& c : next.curves) chain.curves.push_back(c);
            }
            out.push_back(std::move(chain));
        }
    for (ImportedPath& p : out) splitSingle(p);
    return out;
}

// ------------------------------------------------------------------ DXF

struct Group {
    int code;
    QString value;
};

struct Entity {
    QString type;
    QList<Group> groups;
    QList<Entity> vertices;   // a POLYLINE's
    double num(int code, double fallback = 0) const
    {
        for (const Group& g : groups)
            if (g.code == code) return g.value.trimmed().toDouble();
        return fallback;
    }
    QString str(int code) const
    {
        for (const Group& g : groups)
            if (g.code == code) return g.value.trimmed();
        return {};
    }
    bool has(int code) const
    {
        for (const Group& g : groups)
            if (g.code == code) return true;
        return false;
    }
};

struct Block {
    QPointF base;
    QList<Entity> entities;
};

/// A B-spline's point at u (de Boor), its control points weighted.
QPointF deBoor(int p, const std::vector<double>& knots, const std::vector<QPointF>& ctrl, const std::vector<double>& w, double u)
{
    const int n = int(ctrl.size()) - 1;
    int k = p;
    while (k < n && u >= knots[std::size_t(k) + 1]) ++k;
    std::vector<double> hx(std::size_t(p) + 1), hy(std::size_t(p) + 1), hw(std::size_t(p) + 1);
    for (int j = 0; j <= p; ++j) {
        const int i = std::clamp(j + k - p, 0, n);
        const double wi = w.empty() ? 1 : w[std::size_t(i)];
        hx[std::size_t(j)] = ctrl[std::size_t(i)].x() * wi;
        hy[std::size_t(j)] = ctrl[std::size_t(i)].y() * wi;
        hw[std::size_t(j)] = wi;
    }
    for (int r = 1; r <= p; ++r)
        for (int j = p; j >= r; --j) {
            const int i = j + k - p;
            const double lo = knots[std::size_t(std::max(0, i))], hi = knots[std::size_t(std::min(int(knots.size()) - 1, i + p - r + 1))];
            const double alpha = hi > lo ? (u - lo) / (hi - lo) : 0;
            hx[std::size_t(j)] = (1 - alpha) * hx[std::size_t(j) - 1] + alpha * hx[std::size_t(j)];
            hy[std::size_t(j)] = (1 - alpha) * hy[std::size_t(j) - 1] + alpha * hy[std::size_t(j)];
            hw[std::size_t(j)] = (1 - alpha) * hw[std::size_t(j) - 1] + alpha * hw[std::size_t(j)];
        }
    const double ww = hw[std::size_t(p)] != 0 ? hw[std::size_t(p)] : 1;
    return {hx[std::size_t(p)] / ww, hy[std::size_t(p)] / ww};
}

class DxfReader
{
public:
    ImportedDrawing read(const QByteArray& data, const QStringList& layers)
    {
        ImportedDrawing out;
        if (data.startsWith("AutoCAD Binary DXF")) {
            out.error = tr("a binary DXF: save it as an ASCII DXF");
            return out;
        }
        for (const QString& l : layers) a_layers << l.trimmed().toLower();
        const QStringList lines = QString::fromUtf8(data).split(QRegularExpression(QStringLiteral("\r\n|\n|\r")));
        QList<Group> groups;
        for (int i = 0; i + 1 < lines.size(); i += 2) {
            bool ok = false;
            const int code = lines[i].trimmed().toInt(&ok);
            if (!ok) {
                out.error = tr("not a DXF: line %1 is no group code").arg(i + 1);
                return out;
            }
            groups.append({code, lines[i + 1]});
        }
        // The sections.
        QString section;
        for (int i = 0; i < groups.size(); ++i) {
            const Group& g = groups[i];
            if (g.code == 0 && g.value.trimmed() == QLatin1String("SECTION") && i + 1 < groups.size() && groups[i + 1].code == 2) {
                section = groups[i + 1].value.trimmed();
                ++i;
                continue;
            }
            if (g.code == 0 && g.value.trimmed() == QLatin1String("ENDSEC")) {
                section.clear();
                continue;
            }
            if (section == QLatin1String("HEADER") && g.code == 9 && g.value.trimmed() == QLatin1String("$INSUNITS") && i + 1 < groups.size()) {
                static const QMap<int, double> units = {{1, 0.0254}, {2, 0.3048}, {4, 1e-3}, {5, 1e-2}, {6, 1},
                                                        {8, 2.54e-8}, {9, 2.54e-5}, {11, 1e-10}, {12, 1e-9}, {13, 1e-6}, {14, 0.1}};
                out.unit = units.value(groups[i + 1].value.trimmed().toInt(), 0);
            }
            if (section == QLatin1String("BLOCKS") || section == QLatin1String("ENTITIES")) {
                if (g.code != 0) continue;
                // An entity: its groups up to the next 0.
                Entity e;
                e.type = g.value.trimmed();
                int j = i + 1;
                while (j < groups.size() && groups[j].code != 0) e.groups.append(groups[j++]);
                if (e.type == QLatin1String("POLYLINE")) {
                    while (j < groups.size() && groups[j].code == 0 && groups[j].value.trimmed() == QLatin1String("VERTEX")) {
                        Entity v;
                        v.type = QStringLiteral("VERTEX");
                        ++j;
                        while (j < groups.size() && groups[j].code != 0) v.groups.append(groups[j++]);
                        e.vertices.append(v);
                    }
                    if (j < groups.size() && groups[j].value.trimmed() == QLatin1String("SEQEND")) {
                        ++j;
                        while (j < groups.size() && groups[j].code != 0) ++j;
                    }
                }
                i = j - 1;
                if (section == QLatin1String("BLOCKS")) {
                    if (e.type == QLatin1String("BLOCK")) {
                        a_block = e.str(2);
                        a_blocks[a_block].base = QPointF(e.num(10), e.num(20));
                    } else if (e.type == QLatin1String("ENDBLK")) {
                        a_block.clear();
                    } else if (!a_block.isEmpty()) {
                        a_blocks[a_block].entities.append(e);
                    }
                } else {
                    a_entities.append(e);
                }
            }
        }
        std::vector<ImportedPath> pieces;
        for (const Entity& e : a_entities) entity(e, Map2(), pieces, 0);
        if (!a_ignored.isEmpty()) {
            QStringList list;
            for (auto it = a_ignored.cbegin(); it != a_ignored.cend(); ++it) list << QStringLiteral("%1 %2").arg(it.value()).arg(it.key());
            out.warnings << tr("left out (not curves): %1").arg(list.join(QLatin1String(", ")));
        }
        if (a_skippedLayers > 0) out.warnings << tr("%1 entities on other layers left out").arg(a_skippedLayers);
        out.paths = joined(pieces);
        if (out.paths.empty()) out.error = a_layers.isEmpty() ? tr("it has no curves") : tr("it has no curves on the layers %1").arg(layers.join(QLatin1String(", ")));
        return out;
    }

private:
    void entity(const Entity& e, const Map2& m, std::vector<ImportedPath>& out, int depth)
    {
        const QString layer = e.str(8).toLower();
        if (!a_layers.isEmpty() && depth == 0 && !a_layers.contains(layer)) {
            ++a_skippedLayers;
            return;
        }
        // An entity in its own coordinates (OCS) turned over: x mirrored.
        Map2 mm = m;
        if (e.num(230, 1) < 0) mm = m * Map2::scale(-1, 1);
        auto add = [&](ImportedPath p) {
            for (Curve& c : p.curves) c = mapped(c, mm);
            out.push_back(std::move(p));
        };
        const QString& t = e.type;
        if (t == QLatin1String("LINE")) {
            ImportedPath p;
            const QPointF a(e.num(10), e.num(20)), b(e.num(11), e.num(21));
            if (a == b) return;
            p.curves.push_back(line(a, b));
            add(p);
        } else if (t == QLatin1String("CIRCLE")) {
            const double r = e.num(40);
            if (r <= 0) return;
            add(fullEllipse(QPointF(e.num(10), e.num(20)), QPointF(r, 0), QPointF(0, r)));
        } else if (t == QLatin1String("ARC")) {
            const double r = e.num(40);
            if (r <= 0) return;
            double a1 = e.num(50) * M_PI / 180, a2 = e.num(51) * M_PI / 180;
            while (a2 <= a1) a2 += 2 * M_PI;
            ImportedPath p;
            p.curves.push_back(arc(QPointF(e.num(10), e.num(20)), QPointF(r, 0), QPointF(0, r), a1, a2));
            add(p);
        } else if (t == QLatin1String("ELLIPSE")) {
            const QPointF center(e.num(10), e.num(20)), major(e.num(11), e.num(21));
            const double ratio = e.num(40, 1);
            const QPointF minor(-major.y() * ratio, major.x() * ratio);
            double s = e.num(41, 0), f = e.num(42, 2 * M_PI);
            while (f <= s) f += 2 * M_PI;
            if (std::abs(f - s - 2 * M_PI) < 1e-9) add(fullEllipse(center, major, minor));
            else {
                ImportedPath p;
                p.curves.push_back(arc(center, major, minor, s, f));
                add(p);
            }
        } else if (t == QLatin1String("LWPOLYLINE") || t == QLatin1String("POLYLINE")) {
            std::vector<QPointF> pts;
            std::vector<double> bulges;
            if (t == QLatin1String("LWPOLYLINE")) {
                for (const Group& g : e.groups) {
                    if (g.code == 10) {
                        pts.push_back(QPointF(g.value.trimmed().toDouble(), 0));
                        bulges.push_back(0);
                    } else if (g.code == 20 && !pts.empty()) {
                        pts.back().setY(g.value.trimmed().toDouble());
                    } else if (g.code == 42 && !bulges.empty()) {
                        bulges.back() = g.value.trimmed().toDouble();
                    }
                }
            } else {
                for (const Entity& v : e.vertices) {
                    if (int(v.num(70)) & 16) continue;   // (a spline's frame)
                    pts.push_back(QPointF(v.num(10), v.num(20)));
                    bulges.push_back(v.num(42));
                }
            }
            const bool closed = int(e.num(70)) & 1;
            ImportedPath p;
            const int n = int(pts.size());
            for (int i = 0; i < (closed ? n : n - 1); ++i) {
                const QPointF a = pts[std::size_t(i)], b = pts[std::size_t((i + 1) % n)];
                if (a == b) continue;
                const double bulge = bulges[std::size_t(i)];
                if (std::abs(bulge) < 1e-12) {
                    p.curves.push_back(line(a, b));
                    continue;
                }
                // A bulge: an arc of 4 atan(bulge) from a to b, its centre to the
                // left of a -> b when it turns left.
                const double theta = 4 * std::atan(bulge);
                const QPointF d = b - a;
                const double chord = std::hypot(d.x(), d.y());
                const double h = chord / (2 * std::tan(theta / 2));
                const QPointF left(-d.y() / chord, d.x() / chord);
                const QPointF center = (a + b) / 2 + left * h;
                const double r = std::hypot(a.x() - center.x(), a.y() - center.y());
                const double start = std::atan2(a.y() - center.y(), a.x() - center.x());
                p.curves.push_back(arc(center, QPointF(r, 0), QPointF(0, r), start, start + theta));
            }
            if (p.curves.empty()) return;
            p.closed = closed;
            add(p);
        } else if (t == QLatin1String("SPLINE")) {
            const int degree = int(e.num(71, 3));
            const bool closed = int(e.num(70)) & 1;
            std::vector<double> knots, weights;
            std::vector<QPointF> ctrl, fit;
            for (const Group& g : e.groups) {
                const double v = g.value.trimmed().toDouble();
                if (g.code == 40) knots.push_back(v);
                else if (g.code == 41) weights.push_back(v);
                else if (g.code == 10) ctrl.push_back(QPointF(v, 0));
                else if (g.code == 20 && !ctrl.empty()) ctrl.back().setY(v);
                else if (g.code == 11) fit.push_back(QPointF(v, 0));
                else if (g.code == 21 && !fit.empty()) fit.back().setY(v);
            }
            std::vector<QPointF> pts;
            if (int(ctrl.size()) > degree && int(knots.size()) == int(ctrl.size()) + degree + 1 && degree >= 1) {
                if (weights.size() != ctrl.size()) weights.clear();
                const double u0 = knots[std::size_t(degree)], u1 = knots[ctrl.size()];
                const int samples = std::clamp(int(ctrl.size()) * 8, 32, 400);
                for (int i = 0; i <= samples; ++i) {
                    const double u = u0 + (u1 - u0) * i / samples;
                    pts.push_back(deBoor(degree, knots, ctrl, weights, std::min(u, u1 - 1e-12 * (u1 - u0))));
                }
                pts.back() = deBoor(degree, knots, ctrl, weights, u1 - 1e-12 * std::max(1.0, u1 - u0));
            } else if (fit.size() >= 2) {
                pts = fit;
            } else {
                ++a_ignored[QStringLiteral("SPLINE")];
                return;
            }
            if (closed && pts.front() != pts.back()) pts.push_back(pts.front());
            ImportedPath p;
            p.curves.push_back(polyline(pts));
            p.closed = closed || pts.front() == pts.back();
            add(p);
        } else if (t == QLatin1String("INSERT")) {
            if (depth > 8) return;
            auto it = a_blocks.find(e.str(2));
            if (it == a_blocks.end()) return;
            const double sx = e.num(41, 1), sy = e.num(42, 1), rot = e.num(50) * M_PI / 180;
            const int cols = std::max(1, int(e.num(70, 1))), rows = std::max(1, int(e.num(71, 1)));
            const double dc = e.num(44), dr = e.num(45);
            for (int r = 0; r < rows && r < 100; ++r)
                for (int c = 0; c < cols && c < 100; ++c) {
                    const QPointF at = QPointF(e.num(10), e.num(20)) + Map2::rotate(rot).vec(QPointF(c * dc, r * dr));
                    const Map2 place = mm * Map2::translate(at.x(), at.y()) * Map2::rotate(rot) * Map2::scale(sx, sy)
                                       * Map2::translate(-it->base.x(), -it->base.y());
                    for (const Entity& be : it->entities) entity(be, place, out, depth + 1);
                }
        } else if (t != QLatin1String("POINT") && t != QLatin1String("SEQEND") && t != QLatin1String("VERTEX")) {
            ++a_ignored[t];
        }
    }

    QStringList a_layers;
    QList<Entity> a_entities;
    QMap<QString, Block> a_blocks;
    QString a_block;
    QMap<QString, int> a_ignored;
    int a_skippedLayers = 0;
};

// ------------------------------------------------------------------ SVG

/// The numbers of an attribute (a list, a path's arguments).
class Numbers
{
public:
    explicit Numbers(const QString& text) : a_s(text) {}
    bool atEnd()
    {
        skip();
        return a_i >= a_s.size();
    }
    bool atNumber()
    {
        skip();
        if (a_i >= a_s.size()) return false;
        const QChar c = a_s[a_i];
        return c.isDigit() || c == QLatin1Char('-') || c == QLatin1Char('+') || c == QLatin1Char('.');
    }
    QChar peek()
    {
        skip();
        return a_i < a_s.size() ? a_s[a_i] : QChar();
    }
    QChar take()
    {
        skip();
        return a_i < a_s.size() ? a_s[a_i++] : QChar();
    }
    bool number(double* v)
    {
        skip();
        static const QRegularExpression re(QStringLiteral("[+-]?(\\d+\\.?\\d*|\\.\\d+)([eE][+-]?\\d+)?"));
        const QRegularExpressionMatch m = re.match(a_s, a_i, QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
        if (!m.hasMatch()) return false;
        *v = m.captured(0).toDouble();
        a_i += m.capturedLength(0);
        return true;
    }
    /// An arc's flag: one digit, 0 or 1, separators or none after it.
    bool flag(bool* f)
    {
        skip();
        if (a_i >= a_s.size() || (a_s[a_i] != QLatin1Char('0') && a_s[a_i] != QLatin1Char('1'))) return false;
        *f = a_s[a_i++] == QLatin1Char('1');
        return true;
    }

private:
    void skip()
    {
        while (a_i < a_s.size() && (a_s[a_i].isSpace() || a_s[a_i] == QLatin1Char(','))) ++a_i;
    }
    QString a_s;
    int a_i = 0;
};

double length(const QString& text, double fallback = 0)
{
    static const QRegularExpression re(QStringLiteral("^\\s*([+-]?(\\d+\\.?\\d*|\\.\\d+)([eE][+-]?\\d+)?)\\s*([a-z%]*)\\s*$"));
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch()) return fallback;
    return m.captured(1).toDouble();
}

/// An attribute's length in metres, when it has an absolute unit (else 0).
double metres(const QString& text)
{
    static const QRegularExpression re(QStringLiteral("^\\s*([+-]?(\\d+\\.?\\d*|\\.\\d+)([eE][+-]?\\d+)?)\\s*([a-z]*)\\s*$"));
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch()) return 0;
    static const QHash<QString, double> units = {{QStringLiteral("mm"), 1e-3}, {QStringLiteral("cm"), 1e-2}, {QStringLiteral("in"), 0.0254},
                                                 {QStringLiteral("pt"), 0.0254 / 72}, {QStringLiteral("pc"), 0.0254 / 6},
                                                 {QStringLiteral("px"), 0.0254 / 96}, {QString(), 0.0254 / 96}, {QStringLiteral("m"), 1}};
    if (!units.contains(m.captured(4))) return 0;
    return m.captured(1).toDouble() * units.value(m.captured(4));
}

Map2 transformOf(const QString& text)
{
    Map2 m;
    static const QRegularExpression item(QStringLiteral("(matrix|translate|scale|rotate|skewX|skewY)\\s*\\(([^)]*)\\)"));
    auto it = item.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch x = it.next();
        Numbers nums(x.captured(2));
        std::vector<double> v;
        double d = 0;
        while (nums.number(&d)) v.push_back(d);
        const QString kind = x.captured(1);
        Map2 t;
        if (kind == QLatin1String("matrix") && v.size() == 6) {
            t.a = v[0];
            t.b = v[1];
            t.c = v[2];
            t.d = v[3];
            t.e = v[4];
            t.f = v[5];
        } else if (kind == QLatin1String("translate") && !v.empty()) {
            t = Map2::translate(v[0], v.size() > 1 ? v[1] : 0);
        } else if (kind == QLatin1String("scale") && !v.empty()) {
            t = Map2::scale(v[0], v.size() > 1 ? v[1] : v[0]);
        } else if (kind == QLatin1String("rotate") && !v.empty()) {
            const double a = v[0] * M_PI / 180;
            if (v.size() >= 3) t = Map2::translate(v[1], v[2]) * Map2::rotate(a) * Map2::translate(-v[1], -v[2]);
            else t = Map2::rotate(a);
        } else if (kind == QLatin1String("skewX") && !v.empty()) {
            t.c = std::tan(v[0] * M_PI / 180);
        } else if (kind == QLatin1String("skewY") && !v.empty()) {
            t.b = std::tan(v[0] * M_PI / 180);
        }
        m = m * t;
    }
    return m;
}

/// A Bézier of three or four points, as a polyline.
Curve bezier(const std::vector<QPointF>& p)
{
    std::vector<QPointF> pts;
    const int n = 24;
    for (int i = 0; i <= n; ++i) {
        const double t = double(i) / n, s = 1 - t;
        if (p.size() == 4) pts.push_back(p[0] * (s * s * s) + p[1] * (3 * s * s * t) + p[2] * (3 * s * t * t) + p[3] * (t * t * t));
        else pts.push_back(p[0] * (s * s) + p[1] * (2 * s * t) + p[2] * (t * t));
    }
    return polyline(pts);
}

/// An SVG arc from \a from to \a to (its endpoints' form) as an arc of the
/// geometry's (its centre's form): the SVG's implementation notes, F.6.5.
Curve svgArc(QPointF from, QPointF to, double rx, double ry, double phiDeg, bool large, bool sweep)
{
    rx = std::abs(rx);
    ry = std::abs(ry);
    if (rx == 0 || ry == 0) return line(from, to);
    const double phi = phiDeg * M_PI / 180, cp = std::cos(phi), sp = std::sin(phi);
    const double dx = (from.x() - to.x()) / 2, dy = (from.y() - to.y()) / 2;
    const double x1 = cp * dx + sp * dy, y1 = -sp * dx + cp * dy;
    const double lambda = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);
    if (lambda > 1) {
        rx *= std::sqrt(lambda);
        ry *= std::sqrt(lambda);
    }
    const double num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    const double den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    double coef = den > 0 ? std::sqrt(std::max(0.0, num / den)) : 0;
    if (large == sweep) coef = -coef;
    const double cxp = coef * rx * y1 / ry, cyp = -coef * ry * x1 / rx;
    const QPointF center(cp * cxp - sp * cyp + (from.x() + to.x()) / 2, sp * cxp + cp * cyp + (from.y() + to.y()) / 2);
    auto angle = [](double ux, double uy, double vx, double vy) { return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy); };
    const double ux = (x1 - cxp) / rx, uy = (y1 - cyp) / ry, vx = (-x1 - cxp) / rx, vy = (-y1 - cyp) / ry;
    const double t1 = angle(1, 0, ux, uy);
    double dt = angle(ux, uy, vx, vy);
    if (!sweep && dt > 0) dt -= 2 * M_PI;
    else if (sweep && dt < 0) dt += 2 * M_PI;
    return arc(center, QPointF(rx * cp, rx * sp), QPointF(-ry * sp, ry * cp), t1, t1 + dt);
}

std::vector<ImportedPath> pathOf(const QString& d, QStringList& warnings)
{
    std::vector<ImportedPath> out;
    Numbers in(d);
    ImportedPath current;
    QPointF at, start, control;
    QChar command, last;
    auto flush = [&] {
        if (!current.curves.empty()) out.push_back(current);
        current = ImportedPath();
    };
    auto add = [&](const Curve& c) {
        if (startOf(c) != endOf(c) || c.kind != Curve::Line) current.curves.push_back(c);
    };
    while (!in.atEnd()) {
        if (!in.atNumber()) command = in.take();
        else if (command == QLatin1Char('M')) command = QLatin1Char('L');   // (pairs after a move are lines)
        else if (command == QLatin1Char('m')) command = QLatin1Char('l');
        const bool rel = command.isLower();
        const QPointF base = rel ? at : QPointF();
        double v[7];
        auto read = [&](int n) {
            for (int i = 0; i < n; ++i)
                if (!in.number(&v[i])) return false;
            return true;
        };
        const QChar up = command.toUpper();
        bool ok = true;
        if (up == QLatin1Char('M')) {
            if ((ok = read(2))) {
                flush();
                at = base + QPointF(v[0], v[1]);
                start = at;
            }
        } else if (up == QLatin1Char('L')) {
            if ((ok = read(2))) {
                const QPointF to = base + QPointF(v[0], v[1]);
                add(line(at, to));
                at = to;
            }
        } else if (up == QLatin1Char('H')) {
            if ((ok = read(1))) {
                const QPointF to(rel ? at.x() + v[0] : v[0], at.y());
                add(line(at, to));
                at = to;
            }
        } else if (up == QLatin1Char('V')) {
            if ((ok = read(1))) {
                const QPointF to(at.x(), rel ? at.y() + v[0] : v[0]);
                add(line(at, to));
                at = to;
            }
        } else if (up == QLatin1Char('C') || up == QLatin1Char('S')) {
            QPointF c1, c2, to;
            if (up == QLatin1Char('C') && (ok = read(6))) {
                c1 = base + QPointF(v[0], v[1]);
                c2 = base + QPointF(v[2], v[3]);
                to = base + QPointF(v[4], v[5]);
            } else if (up == QLatin1Char('S') && (ok = read(4))) {
                const bool after = last.toUpper() == QLatin1Char('C') || last.toUpper() == QLatin1Char('S');
                c1 = after ? at * 2 - control : at;
                c2 = base + QPointF(v[0], v[1]);
                to = base + QPointF(v[2], v[3]);
            }
            if (ok) {
                add(bezier({at, c1, c2, to}));
                control = c2;
                at = to;
            }
        } else if (up == QLatin1Char('Q') || up == QLatin1Char('T')) {
            QPointF c1, to;
            if (up == QLatin1Char('Q') && (ok = read(4))) {
                c1 = base + QPointF(v[0], v[1]);
                to = base + QPointF(v[2], v[3]);
            } else if (up == QLatin1Char('T') && (ok = read(2))) {
                const bool after = last.toUpper() == QLatin1Char('Q') || last.toUpper() == QLatin1Char('T');
                c1 = after ? at * 2 - control : at;
                to = base + QPointF(v[0], v[1]);
            }
            if (ok) {
                add(bezier({at, c1, to}));
                control = c1;
                at = to;
            }
        } else if (up == QLatin1Char('A')) {
            bool large = false, sweep = false;
            ok = in.number(&v[0]) && in.number(&v[1]) && in.number(&v[2]) && in.flag(&large) && in.flag(&sweep) && in.number(&v[3])
                 && in.number(&v[4]);
            if (ok) {
                const QPointF to = base + QPointF(v[3], v[4]);
                if (to != at) add(svgArc(at, to, v[0], v[1], v[2], large, sweep));
                at = to;
            }
        } else if (up == QLatin1Char('Z')) {
            if (at != start) add(line(at, start));
            at = start;
            if (!current.curves.empty()) {
                current.closed = true;
                flush();
            }
        } else {
            warnings << tr("a path's command %1 is not known: the rest of it left out").arg(command);
            break;
        }
        if (!ok) {
            warnings << tr("a path's %1 lacks its numbers: the rest of it left out").arg(command);
            break;
        }
        last = command;
    }
    flush();
    return out;
}

ImportedDrawing svg(const QByteArray& data)
{
    ImportedDrawing out;
    QXmlStreamReader xml(data);
    std::vector<Map2> stack{Map2()};
    std::vector<ImportedPath> paths;
    int hidden = 0;   // depth inside what is not drawn (defs, a hidden group)
    QMap<QString, int> ignored;
    bool root = true;
    while (!xml.atEnd()) {
        const QXmlStreamReader::TokenType token = xml.readNext();
        if (token == QXmlStreamReader::EndElement) {
            if (hidden > 0) --hidden;
            if (stack.size() > 1) stack.pop_back();
            continue;
        }
        if (token != QXmlStreamReader::StartElement) continue;
        const QXmlStreamAttributes at = xml.attributes();
        const QString name = xml.name().toString();
        const QString style = at.value(QLatin1String("style")).toString();
        const bool none = at.value(QLatin1String("display")) == QLatin1String("none") || style.contains(QLatin1String("display:none"))
                          || style.contains(QLatin1String("display: none"));
        Map2 m = stack.back() * transformOf(at.value(QLatin1String("transform")).toString());
        if (root && name == QLatin1String("svg")) {
            // Its user unit: the width's over the viewBox's, else a pixel.
            root = false;
            out.unit = 0.0254 / 96;
            Numbers box(at.value(QLatin1String("viewBox")).toString());
            double vb[4];
            bool hasBox = true;
            for (double& x : vb)
                if (!box.number(&x)) hasBox = false;
            const double w = metres(at.value(QLatin1String("width")).toString());
            const double h = metres(at.value(QLatin1String("height")).toString());
            if (hasBox && vb[2] > 0 && w > 0) out.unit = w / vb[2];
            else if (hasBox && vb[3] > 0 && h > 0) out.unit = h / vb[3];
            if (hasBox) m = m * Map2::translate(0, 0);
        }
        stack.push_back(m);
        if (hidden > 0 || none || name == QLatin1String("defs") || name == QLatin1String("clipPath") || name == QLatin1String("mask")
            || name == QLatin1String("symbol") || name == QLatin1String("marker") || name == QLatin1String("pattern")) {
            ++hidden;
            continue;
        }
        auto num = [&](const char* key, double fallback = 0) { return length(at.value(QLatin1String(key)).toString(), fallback); };
        std::vector<ImportedPath> made;
        if (name == QLatin1String("path")) {
            made = pathOf(at.value(QLatin1String("d")).toString(), out.warnings);
        } else if (name == QLatin1String("rect")) {
            const double x = num("x"), y = num("y"), w = num("width"), h = num("height");
            double rx = num("rx", -1), ry = num("ry", -1);
            if (rx < 0) rx = ry;
            if (ry < 0) ry = rx;
            rx = std::clamp(rx, 0.0, w / 2);
            ry = std::clamp(ry, 0.0, h / 2);
            if (w > 0 && h > 0) {
                ImportedPath p;
                p.closed = true;
                if (rx <= 0 || ry <= 0) {
                    p.curves = {line({x, y}, {x + w, y}), line({x + w, y}, {x + w, y + h}), line({x + w, y + h}, {x, y + h}), line({x, y + h}, {x, y})};
                } else {
                    const QPointF A(rx, 0), B(0, ry);
                    p.curves = {line({x + rx, y}, {x + w - rx, y}), arc({x + w - rx, y + ry}, A, B, -M_PI / 2, 0),
                                line({x + w, y + ry}, {x + w, y + h - ry}), arc({x + w - rx, y + h - ry}, A, B, 0, M_PI / 2),
                                line({x + w - rx, y + h}, {x + rx, y + h}), arc({x + rx, y + h - ry}, A, B, M_PI / 2, M_PI),
                                line({x, y + h - ry}, {x, y + ry}), arc({x + rx, y + ry}, A, B, M_PI, 3 * M_PI / 2)};
                    p.curves.erase(std::remove_if(p.curves.begin(), p.curves.end(),
                                                  [](const Curve& c) { return c.kind == Curve::Line && startOf(c) == endOf(c); }),
                                   p.curves.end());
                }
                made.push_back(p);
            }
        } else if (name == QLatin1String("circle")) {
            const double r = num("r");
            if (r > 0) made.push_back(fullEllipse({num("cx"), num("cy")}, {r, 0}, {0, r}));
        } else if (name == QLatin1String("ellipse")) {
            const double rx = num("rx"), ry = num("ry");
            if (rx > 0 && ry > 0) made.push_back(fullEllipse({num("cx"), num("cy")}, {rx, 0}, {0, ry}));
        } else if (name == QLatin1String("line")) {
            const QPointF a(num("x1"), num("y1")), b(num("x2"), num("y2"));
            if (a != b) {
                ImportedPath p;
                p.curves.push_back(line(a, b));
                made.push_back(p);
            }
        } else if (name == QLatin1String("polyline") || name == QLatin1String("polygon")) {
            Numbers nums(at.value(QLatin1String("points")).toString());
            std::vector<QPointF> pts;
            double x = 0, y = 0;
            while (nums.number(&x) && nums.number(&y)) pts.push_back({x, y});
            const bool closed = name == QLatin1String("polygon");
            ImportedPath p;
            for (std::size_t i = 0; i + 1 < pts.size(); ++i)
                if (pts[i] != pts[i + 1]) p.curves.push_back(line(pts[i], pts[i + 1]));
            if (closed && pts.size() > 2 && pts.back() != pts.front()) p.curves.push_back(line(pts.back(), pts.front()));
            p.closed = closed && p.curves.size() >= 3;
            if (!p.curves.empty()) made.push_back(p);
        } else if (name == QLatin1String("text") || name == QLatin1String("image") || name == QLatin1String("use")) {
            ++ignored[name];
            ++hidden;   // (its children too)
            continue;
        }
        for (ImportedPath& p : made) {
            for (Curve& c : p.curves) c = mapped(c, m);
            paths.push_back(std::move(p));
        }
    }
    if (xml.hasError() && paths.empty()) {
        out.error = tr("not an SVG: %1 (line %2)").arg(xml.errorString()).arg(xml.lineNumber());
        return out;
    }
    if (!ignored.isEmpty()) {
        QStringList list;
        for (auto it = ignored.cbegin(); it != ignored.cend(); ++it) list << QStringLiteral("%1 <%2>").arg(it.value()).arg(it.key());
        out.warnings << tr("left out (not shapes): %1").arg(list.join(QLatin1String(", ")));
    }
    // y up.
    for (ImportedPath& p : paths)
        for (Curve& c : p.curves) c = mapped(c, Map2::scale(1, -1));
    out.paths = joined(paths);
    if (out.paths.empty()) out.error = tr("it has no shapes");
    return out;
}

} // namespace

ImportedDrawing readDxf(const QByteArray& data, const QStringList& layers)
{
    DxfReader reader;
    return reader.read(data, layers);
}

ImportedDrawing readSvg(const QByteArray& data)
{
    return svg(data);
}

} // namespace qucs_s::fem
