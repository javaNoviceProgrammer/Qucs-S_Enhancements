/*
 * fem_rays.cpp - geometrical optics: rays released from points, beams and
 *                boundaries, traced through the domains - straight where
 *                the refractive index is uniform, met by the geometry's own
 *                curves (an arc a true circle, not its mesh), bent by the
 *                ray equation where it varies (RK4) - refracted by Snell's
 *                law and split by Fresnel's coefficients (s and p), wholly
 *                reflected beyond the critical angle, reflected by mirrors,
 *                stopped by absorbers and walls, counted by detectors;
 *                absorbed as they go (n - jκ), what is absorbed a power
 *                density on the mesh (gop.Q) that heat may take
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_problem.h"
#include "fem_geometry.h"
#include "fem_mesh.h"
#include "fem_results.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <thread>

namespace qucs_s::fem::detail {

namespace {

constexpr double C0 = 299792458.0;

double dot(QPointF a, QPointF b) { return a.x() * b.x() + a.y() * b.y(); }
double cross(QPointF a, QPointF b) { return a.x() * b.y() - a.y() * b.x(); }
double norm(QPointF a) { return std::hypot(a.x(), a.y()); }

/// A straight piece or an arc of a boundary (metres).
struct Piece {
    bool arc = false;
    QPointF a, b;              // a line from a to b
    QPointF c, u, v;           // an arc: c + u cos t + v sin t
    double tlo = 0, thi = 0;   // its parameters (thi > tlo)
    int segment = -1;
};

struct Hit {
    double s = std::numeric_limits<double>::infinity();
    QPointF normal;            // unit
    int segment = -1;
};

/// Where the ray p + s d (s > eps) first meets \a piece.
void intersect(const Piece& piece, QPointF p, QPointF d, double eps, Hit& best)
{
    if (!piece.arc) {
        const QPointF e = piece.b - piece.a;
        const double den = cross(d, e);
        if (std::abs(den) < 1e-300) return;
        const QPointF w = piece.a - p;
        const double s = cross(w, e) / den, t = cross(w, d) / den;
        if (s > eps && s < best.s && t >= -1e-12 && t <= 1 + 1e-12) {
            best.s = s;
            best.normal = QPointF(-e.y(), e.x()) / norm(e);
            best.segment = piece.segment;
        }
        return;
    }
    // In the arc's own frame: |q + s e|² = 1, (q, e) = M⁻¹ (p - c, d).
    const double det = piece.u.x() * piece.v.y() - piece.v.x() * piece.u.y();
    if (std::abs(det) < 1e-300) return;
    auto inv = [&](QPointF w) {
        return QPointF((piece.v.y() * w.x() - piece.v.x() * w.y()) / det, (-piece.u.y() * w.x() + piece.u.x() * w.y()) / det);
    };
    const QPointF q = inv(p - piece.c), e = inv(d);
    const double A = dot(e, e), B = 2 * dot(q, e), Cc = dot(q, q) - 1;
    const double disc = B * B - 4 * A * Cc;
    if (disc < 0 || A <= 0) return;
    const double root = std::sqrt(disc);
    for (double s : {(-B - root) / (2 * A), (-B + root) / (2 * A)}) {
        if (!(s > eps) || s >= best.s) continue;
        const QPointF w = q + e * s;
        double t = std::atan2(w.y(), w.x());
        while (t < piece.tlo - 1e-12) t += 2 * M_PI;
        while (t > piece.tlo + 2 * M_PI) t -= 2 * M_PI;
        if (t > piece.thi + 1e-12) continue;
        // The normal: the gradient of the implicit form, M⁻ᵀ w.
        const QPointF n((piece.v.y() * w.x() - piece.u.y() * w.y()) / det, (-piece.v.x() * w.x() + piece.u.x() * w.y()) / det);
        best.s = s;
        best.normal = n / norm(n);
        best.segment = piece.segment;
    }
}

struct Release {
    QPointF p, d;          // m; unit
    double lambda = 0;     // m
    double power = 0;      // W
    double ps = 0.5;       // its share polarized s (E out of the plane)
    int index = 0;         // from 1
};

struct Medium {
    Expression n, kappa;
    bool graded = false;   // n or κ varies in space (or with another field)
    bool defined = false;
    bool absorbing = false;   // κ given, not 0
};

struct BoundaryRule {
    enum Kind { Interface, Mirror, Absorber };
    Kind kind = Interface;
    double reflectance = 1;
    bool coating = false;
    int detector = -1;
    bool detectorStops = false;
};

struct Detector {
    QString name;
    int count = 0;
    double power = 0, px = 0, py = 0, pxx = 0, pyy = 0, opl = 0;
};

/// A ray's path, as traced.
struct Path {
    std::vector<std::array<double, 4>> points;   // x, y (m), power (W), optical path (m)
    double lambda = 0;
    int index = 0;
    int status = 0;
};

struct DetectorHit {
    int detector = -1;
    double x = 0, y = 0, power = 0, opl = 0;
};

class RayProblem : public Problem
{
public:
    RayProblem(Solution& solution, int field) : Problem(solution, field) {}

    bool setup(QString* error) override;
    bool solve(const SolveOptions& options, QString* error) override;
    void setGlobals() override;

private:
    struct Traced {
        std::vector<Path> paths;
        std::vector<DetectorHit> hits;
        double absorbedWalls = 0;
        int rays = 0;
    };
    /// Release \a r traced: its paths and hits in \a out, what the media
    /// absorb added to \a deposit (by triangle, W).
    void trace(const Release& r, Traced& out, std::vector<double>& deposit, PointEvaluator& ev, const Locator& locator) const;
    /// n and κ of domain \a d at \a p (m) at the ray's wavelength (set on \a ev).
    bool indexAt(int d, QPointF p, PointEvaluator& ev, const Locator& locator, double* n, double* kappa) const;
    void depositAlong(QPointF a, QPointF b, double p0, double alpha, std::vector<double>& deposit, const Locator& locator) const;

    std::vector<Medium> a_media;               // by domain
    std::vector<Piece> a_pieces;
    std::vector<std::vector<int>> a_domainPieces;
    std::vector<BoundaryRule> a_rules;         // by boundary
    std::vector<Detector> a_detectors;
    std::vector<Release> a_releases;
    int a_maxInteractions = 100;
    double a_threshold = 1e-4;
    bool a_reflections = true;
    int a_maxRays = 20000;
    double a_size = 1;                         // the model's size (m)
    double a_maxLength = 1;                    // a ray's longest path (m)
    // Results.
    double a_released = 0, a_absorbedMedia = 0, a_absorbedWalls = 0;
    int a_primaries = 0, a_traced = 0;
};

bool RayProblem::setup(QString* error)
{
    if (!a_node) {
        *error = tr("there is no physics %1").arg(field().tag);
        return false;
    }
    Field& f = field();
    const Model& model = a_s.model();
    const Topology& topo = a_s.topology();
    const Scope& constants = a_s.parameters().scope;
    const double unit = topo.unitScale;
    QString why;
    const QVector<int> domains = resolveSelection(topo, model, Level::Domain, a_node->selection(), &why);
    if (!why.isEmpty()) problem(tr("%1: %2").arg(a_node->name(), why));
    f.domains.assign(topo.domains.size(), 0);
    for (int d : domains) f.domains[std::size_t(d)] = 1;
    if (domains.isEmpty()) problem(tr("%1 is on no domain").arg(a_node->name()));
    a_maxInteractions = std::clamp(a_node->integer(QStringLiteral("maxinteractions")), 1, 100000);
    a_maxRays = std::clamp(a_node->integer(QStringLiteral("maxrays")), 1, 1000000);
    a_reflections = a_node->flag(QStringLiteral("reflections"));
    if (const std::optional<double> t = evaluateConstant(a_node->text(QStringLiteral("threshold")), constants, Dim()); t && *t >= 0) a_threshold = *t;
    a_size = std::max(topo.bounds.width(), topo.bounds.height()) * unit;
    a_maxLength = 1000 * a_size;
    // Media.
    const std::vector<const Node*> material = materials();
    a_media.assign(topo.domains.size(), Medium());
    std::vector<const Node*> mediumOf(topo.domains.size(), nullptr);
    std::vector<const Node*> releases, boundaryFeatures;
    for (const Node& feature : a_node->children) {
        if (!feature.enabled) continue;
        const NodeKind* k = feature.kind();
        if (!k) continue;
        if (feature.type == QLatin1String("rays/medium")) {
            const QVector<int> picked = resolveSelection(topo, model, Level::Domain, feature.selection(), &why);
            for (int d : picked) mediumOf[std::size_t(d)] = &feature;
        } else if (feature.type == QLatin1String("rays/point") || feature.type == QLatin1String("rays/beam")
                   || feature.type == QLatin1String("rays/boundary")) {
            releases.push_back(&feature);
        } else if (k->selection == Level::Boundary) {
            boundaryFeatures.push_back(&feature);
        }
    }
    auto spatialSlots = [&](const Expression& e) {
        for (int slot : e.variables()) {
            const Solution::VariableDef& d = a_s.definitions()[std::size_t(slot)];
            if (d.kind != Solution::VariableDef::Frequency && d.kind != Solution::VariableDef::Time
                && !(d.kind == Solution::VariableDef::Derived && d.info.name == QLatin1String("lambda0")))
                return true;
        }
        return false;
    };
    Solution::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    coef.c.assign(topo.domains.size(), Expression());
    for (int d : domains) {
        Medium& m = a_media[std::size_t(d)];
        const Node* mf = mediumOf[std::size_t(d)];
        const Node* mat = material[std::size_t(d)];
        const bool user = mf && mf->text(QStringLiteral("source")) == QLatin1String("user");
        const QString object = topo.objectNames.value(topo.domains[std::size_t(d)].owner);
        const QString where = tr("domain %1 (%2)").arg(d + 1).arg(object);
        const Node& from = user ? *mf : mat ? *mat : *a_node;
        const QString nText = user ? mf->text(QStringLiteral("n")) : mat ? materialProperty(*mat, QStringLiteral("n"), &a_s.parameters().scope) : QString();
        const QString kText = user ? mf->text(QStringLiteral("kappa")) : mat ? materialProperty(*mat, QStringLiteral("kappa"), &a_s.parameters().scope) : QString();
        if (nText.trimmed().isEmpty()) {
            problem(tr("%1: its material has no refractive index n").arg(where));
            continue;
        }
        m.n = compile(nText, from, QStringLiteral("n"));
        if (!kText.trimmed().isEmpty()) m.kappa = compile(kText, from, QStringLiteral("kappa"));
        m.absorbing = m.kappa.isValid() && !(m.kappa.isConstant() && m.kappa.constant() == 0);
        m.graded = (m.n.isValid() && spatialSlots(m.n)) || (m.kappa.isValid() && spatialSlots(m.kappa));
        m.defined = m.n.isValid();
        coef.c[std::size_t(d)] = m.n;
    }
    // The boundaries: each segment's pieces, by domain.
    a_pieces.clear();
    a_domainPieces.assign(topo.domains.size(), {});
    for (int sIdx = 0; sIdx < int(topo.segments.size()); ++sIdx) {
        const Topology::Segment& seg = topo.segments[std::size_t(sIdx)];
        const QPointF va = topo.vertices[std::size_t(seg.v0)] * unit, vb = topo.vertices[std::size_t(seg.v1)] * unit;
        std::vector<Piece> made;
        const Curve* curve = seg.curve >= 0 && seg.curve < int(topo.curves.size()) ? &topo.curves[std::size_t(seg.curve)] : nullptr;
        if (curve && curve->kind == Curve::Arc) {
            Piece p;
            p.arc = true;
            p.c = curve->c * unit;
            p.u = curve->a * unit;
            p.v = curve->b * unit;
            p.tlo = std::min(seg.t0, seg.t1);
            p.thi = std::max(seg.t0, seg.t1);
            made.push_back(p);
        } else if (curve && curve->kind == Curve::Polyline) {
            // Its straight pieces between t0 and t1.
            const double lo = std::min(seg.t0, seg.t1), hi = std::max(seg.t0, seg.t1);
            std::vector<QPointF> pts{curve->at(lo) * unit};
            for (int k = int(std::floor(lo)) + 1; k < hi; ++k) pts.push_back(curve->at(k) * unit);
            pts.push_back(curve->at(hi) * unit);
            for (std::size_t k = 0; k + 1 < pts.size(); ++k) {
                Piece p;
                p.a = pts[k];
                p.b = pts[k + 1];
                made.push_back(p);
            }
        } else {
            Piece p;
            p.a = va;
            p.b = vb;
            made.push_back(p);
        }
        for (Piece& p : made) {
            p.segment = sIdx;
            const int index = int(a_pieces.size());
            a_pieces.push_back(p);
            if (seg.left >= 0) a_domainPieces[std::size_t(seg.left)].push_back(index);
            if (seg.right >= 0 && seg.right != seg.left) a_domainPieces[std::size_t(seg.right)].push_back(index);
        }
    }
    // Boundary conditions (the last feature on a boundary wins; a detector is
    // kept with what else is there).
    a_rules.assign(topo.boundaries.size(), BoundaryRule());
    a_detectors.clear();
    for (const Node* bf : boundaryFeatures) {
        const QVector<int> picked = resolveSelection(topo, model, Level::Boundary, bf->selection(), &why);
        if (!why.isEmpty()) problem(tr("%1: %2").arg(bf->name(), why));
        const QString t = bf->type;
        if (t == QLatin1String("rays/detector")) {
            Detector det;
            det.name = bf->text(QStringLiteral("name"));
            for (const Detector& other : a_detectors)
                if (other.name == det.name) problem(tr("two detectors are named %1").arg(det.name));
            const int index = int(a_detectors.size());
            a_detectors.push_back(det);
            for (int b : picked) {
                a_rules[std::size_t(b)].detector = index;
                a_rules[std::size_t(b)].detectorStops = bf->flag(QStringLiteral("stop"));
            }
            continue;
        }
        for (int b : picked) {
            BoundaryRule& r = a_rules[std::size_t(b)];
            if (t == QLatin1String("rays/mirror")) {
                r.kind = BoundaryRule::Mirror;
                const std::optional<double> R = evaluateConstant(bf->text(QStringLiteral("R")), constants, Dim(), &why);
                if (!R || *R < 0 || *R > 1) problem(tr("%1: its reflectance: %2").arg(bf->name(), R ? tr("from 0 to 1") : why));
                else r.reflectance = *R;
            } else if (t == QLatin1String("rays/absorber")) {
                r.kind = BoundaryRule::Absorber;
            } else if (t == QLatin1String("rays/coating")) {
                r.kind = BoundaryRule::Interface;
                r.coating = true;
            }
        }
    }
    // Releases.
    a_releases.clear();
    Expression::Options inUnit;
    inUnit.lengthUnit = unit;
    inUnit.numbersAreLengths = true;
    auto length = [&](const QJsonValue& v, const QString& where) {
        const Expression e = Expression::compile(v.toVariant().toString(), constants, inUnit);
        if (!e.isValid() || !e.isConstant()) {
            problem(tr("%1: %2").arg(where, e.isValid() ? tr("its coordinates must be constants") : e.error()));
            return 0.0;
        }
        return e.constant() * unit;
    };
    auto angle = [&](const QString& text, const QString& where) {
        const std::optional<double> a = evaluateConstant(text, constants, Dim(), &why);
        if (!a) {
            problem(tr("%1: its angle: %2").arg(where, why));
            return 0.0;
        }
        // Degrees, or radians given as such ([rad]: already in radians).
        return text.contains(QLatin1String("[rad]")) ? *a : *a * M_PI / 180;
    };
    int index = 0;
    for (const Node* rf : releases) {
        const QString where = rf->name();
        Dim dim;
        const std::optional<std::vector<double>> lambdas = valueList(rf->text(QStringLiteral("lambda0")), constants, &dim, &why);
        if (!lambdas || lambdas->empty() || !(dim == Dim::length())) {
            problem(tr("%1: its wavelengths: %2").arg(where, lambdas ? tr("give lengths: 550[nm], 450[nm]") : why));
            continue;
        }
        const int count = std::clamp(rf->integer(QStringLiteral("count")), 1, 100000);
        const std::optional<double> P0 = evaluateConstant(rf->text(QStringLiteral("P0")), constants, Dim::power(), &why);
        if (!P0 || *P0 < 0) {
            problem(tr("%1: its power: %2").arg(where, P0 ? tr("it must be 0 or more") : why));
            continue;
        }
        const QString pol = rf->text(QStringLiteral("polarization"));
        const double ps = pol == QLatin1String("s") ? 1.0 : pol == QLatin1String("p") ? 0.0 : 0.5;
        std::vector<std::pair<QPointF, QPointF>> rays;   // point, direction
        std::vector<double> weights;
        if (rf->type == QLatin1String("rays/point")) {
            const QJsonArray at = rf->value(QStringLiteral("position")).toArray();
            const QPointF p(length(at.at(0), where), length(at.at(1), where));
            const double centre = angle(rf->text(QStringLiteral("angle")), where);
            const double half = std::clamp(angle(rf->text(QStringLiteral("spread")), where), 0.0, M_PI);
            for (int k = 0; k < count; ++k) {
                double th = centre;
                if (half >= M_PI - 1e-12) th = centre + 2 * M_PI * k / count;
                else if (count > 1) th = centre - half + 2 * half * k / (count - 1);
                rays.push_back({p, QPointF(std::cos(th), std::sin(th))});
                weights.push_back(1);
            }
        } else if (rf->type == QLatin1String("rays/beam")) {
            const QJsonArray a = rf->value(QStringLiteral("start")).toArray(), b = rf->value(QStringLiteral("end")).toArray();
            const QPointF pa(length(a.at(0), where), length(a.at(1), where)), pb(length(b.at(0), where), length(b.at(1), where));
            const double th = angle(rf->text(QStringLiteral("angle")), where);
            const bool gaussian = rf->text(QStringLiteral("profile")) == QLatin1String("gaussian");
            double waist = 0;
            if (gaussian) {
                const std::optional<double> w = evaluateConstant(rf->text(QStringLiteral("waist")), constants, Dim::length(), &why);
                if (!w || !(*w > 0)) problem(tr("%1: its waist: %2").arg(where, w ? tr("it must be more than 0") : why));
                else waist = *w;
            }
            const QPointF mid = (pa + pb) / 2;
            for (int k = 0; k < count; ++k) {
                const double u = (k + 0.5) / count;
                const QPointF p = pa + (pb - pa) * u;
                rays.push_back({p, QPointF(std::cos(th), std::sin(th))});
                const double r = norm(p - mid);
                weights.push_back(gaussian && waist > 0 ? std::exp(-2 * r * r / (waist * waist)) : 1.0);
            }
        } else {
            // From boundaries, evenly along them, into the physics' domains.
            const QVector<int> picked = resolveSelection(topo, model, Level::Boundary, rf->selection(), &why);
            std::vector<std::pair<QPointF, QPointF>> pieces;   // straight pieces of them (m)
            std::vector<QPointF> inward;
            double total = 0;
            for (int b : picked) {
                const Topology::Boundary& bd = topo.boundaries[std::size_t(b)];
                const std::vector<QPointF> pts = bd.polyline(topo);
                const bool upIn = bd.up >= 0 && f.domains[std::size_t(bd.up)];
                for (std::size_t k = 0; k + 1 < pts.size(); ++k) {
                    const QPointF a = pts[k] * unit, c = pts[k + 1] * unit;
                    const QPointF t = (c - a) / std::max(norm(c - a), 1e-300);
                    // (Up: on the left of the boundary as walked.)
                    inward.push_back(upIn ? QPointF(-t.y(), t.x()) : QPointF(t.y(), -t.x()));
                    pieces.push_back({a, c});
                    total += norm(c - a);
                }
            }
            if (!(total > 0)) {
                problem(tr("%1 is on no boundary").arg(where));
                continue;
            }
            for (int k = 0; k < count; ++k) {
                double s = (k + 0.5) / count * total;
                for (std::size_t j = 0; j < pieces.size(); ++j) {
                    const double len = norm(pieces[j].second - pieces[j].first);
                    if (s <= len || j + 1 == pieces.size()) {
                        const QPointF p = pieces[j].first + (pieces[j].second - pieces[j].first) * (len > 0 ? s / len : 0);
                        rays.push_back({p + inward[j] * (1e-9 * a_size), inward[j]});
                        weights.push_back(1);
                        break;
                    }
                    s -= len;
                }
            }
        }
        double sum = 0;
        for (double w : weights) sum += w;
        for (double lam : *lambdas) {
            if (!(lam > 0)) {
                problem(tr("%1: a wavelength must be more than 0").arg(where));
                continue;
            }
            for (std::size_t k = 0; k < rays.size(); ++k) {
                Release r;
                r.p = rays[k].first;
                r.d = rays[k].second;
                r.lambda = lam;
                r.power = sum > 0 ? *P0 * weights[k] / sum : 0;
                r.ps = ps;
                r.index = ++index;
                a_releases.push_back(r);
            }
        }
    }
    if (a_releases.empty() && !domains.isEmpty()) problem(tr("%1 releases no rays: add a release (from a point, a beam or boundaries)").arg(a_node->name()));
    if (!a_problems.isEmpty()) {
        *error = a_problems.join(QLatin1Char('\n'));
        return false;
    }
    return true;
}

bool RayProblem::indexAt(int d, QPointF p, PointEvaluator& ev, const Locator& locator, double* n, double* kappa) const
{
    const Medium& m = a_media[std::size_t(d)];
    if (!m.defined) return false;
    if (m.n.isConstant() && (!m.kappa.isValid() || m.kappa.isConstant())) {
        *n = m.n.constant();
        *kappa = m.kappa.isValid() ? m.kappa.constant() : 0.0;
        return true;
    }
    // At the point, in a triangle of that domain (a point on its boundary:
    // nudged into it).
    const Mesh& mesh = a_s.mesh();
    const double unit = mesh.unitScale;
    int t = -1;
    double xi = 0, eta = 0;
    if (!locator.locate(p / unit, &t, &xi, &eta) || mesh.domain[std::size_t(t)] != d) {
        const QPointF inside = a_s.topology().domains[std::size_t(d)].inside;
        const QPointF toward = inside * unit - p;
        bool found = false;
        for (double frac : {1e-6, 1e-4, 1e-2}) {
            const QPointF q = p + toward * frac;
            if (locator.locate(q / unit, &t, &xi, &eta) && mesh.domain[std::size_t(t)] == d) {
                found = true;
                break;
            }
        }
        if (!found && !(locator.locate(inside, &t, &xi, &eta) && mesh.domain[std::size_t(t)] == d)) return false;
    }
    ev.moveTo(t, xi, eta);
    *n = ev.eval(m.n);
    *kappa = m.kappa.isValid() ? ev.eval(m.kappa) : 0.0;
    return std::isfinite(*n) && *n > 0;
}

void RayProblem::depositAlong(QPointF a, QPointF b, double p0, double alpha, std::vector<double>& deposit, const Locator& locator) const
{
    if (!(alpha > 0) || !(p0 > 0)) return;
    const Mesh& mesh = a_s.mesh();
    const double unit = mesh.unitScale;
    const double L = norm(b - a);
    if (!(L > 0)) return;
    // Pieces about a tenth of the mesh's mean size each.
    const double h = std::max(1e-12, std::sqrt(std::max(mesh.bounds().width() * mesh.bounds().height(), 1e-300) /
                                               std::max<std::size_t>(1, mesh.triangles.size())) * unit / 4);
    const int pieces = std::clamp(int(std::ceil(L / h)), 1, 10000);
    for (int k = 0; k < pieces; ++k) {
        const double s0 = L * k / pieces, s1 = L * (k + 1) / pieces;
        const double lost = p0 * (std::exp(-alpha * s0) - std::exp(-alpha * s1));
        const QPointF mid = a + (b - a) * ((s0 + s1) / 2 / L);
        int t = -1;
        double xi = 0, eta = 0;
        if (locator.locate(mid / unit, &t, &xi, &eta)) deposit[std::size_t(t)] += lost;
    }
}

void RayProblem::trace(const Release& release, Traced& out, std::vector<double>& deposit, PointEvaluator& ev, const Locator& locator) const
{
    const Topology& topo = a_s.topology();
    const Field& f = a_s.fields()[std::size_t(a_fi)];
    const double unit = topo.unitScale;
    const double eps = 1e-9 * a_size;
    ev.setFrequency(C0 / release.lambda);
    struct Ray {
        QPointF p, d;
        double ps, pp;     // its powers polarized s and p (W)
        double opl = 0, length = 0;
        int domain = -1, interactions = 0;
    };
    std::vector<Ray> stack;
    Ray first;
    first.p = release.p;
    first.d = release.d / norm(release.d);
    first.ps = release.power * release.ps;
    first.pp = release.power * (1 - release.ps);
    first.domain = topo.domainAt(release.p / unit);
    stack.push_back(first);
    const double floor = a_threshold * release.power;
    while (!stack.empty()) {
        if (out.rays >= a_maxRays) break;
        Ray ray = stack.back();
        stack.pop_back();
        ++out.rays;
        Path path;
        path.lambda = release.lambda;
        path.index = release.index;
        path.points.push_back({ray.p.x(), ray.p.y(), ray.ps + ray.pp, ray.opl});
        int status = 0;
        while (true) {
            const double P = ray.ps + ray.pp;
            if (ray.domain < 0 || !f.domains[std::size_t(ray.domain)]) {
                status = 3;   // outside the physics
                break;
            }
            if (P <= floor || P <= 0) {
                status = 4;
                break;
            }
            if (ray.interactions >= a_maxInteractions || ray.length >= a_maxLength) {
                status = 0;
                break;
            }
            const Medium& m = a_media[std::size_t(ray.domain)];
            double n = 1, kappa = 0;
            if (!indexAt(ray.domain, ray.p, ev, locator, &n, &kappa)) {
                status = 3;
                break;
            }
            Hit hit;
            QPointF dEnd = ray.d;
            double travelled = 0;
            if (!m.graded) {
                for (int pi : a_domainPieces[std::size_t(ray.domain)]) intersect(a_pieces[std::size_t(pi)], ray.p, ray.d, eps, hit);
                if (hit.segment < 0) {
                    status = 3;   // (a hole in the boundary: lost)
                    break;
                }
                travelled = hit.s;
                const double alpha = 4 * M_PI * kappa / release.lambda;
                const QPointF end = ray.p + ray.d * hit.s;
                if (alpha > 0) {
                    depositAlong(ray.p, end, P, alpha, deposit, locator);
                    const double keep = std::exp(-alpha * hit.s);
                    ray.ps *= keep;
                    ray.pp *= keep;
                }
                ray.opl += n * hit.s;
                ray.p = end;
            } else {
                // The ray equation, d(n dr/ds)/ds = ∇n, by RK4 in steps of a
                // fraction of the mesh's size, until a boundary is crossed.
                const Mesh& mesh = a_s.mesh();
                const double h = std::max(1e-12, std::sqrt(std::max(mesh.bounds().width() * mesh.bounds().height(), 1e-300) /
                                                           std::max<std::size_t>(1, mesh.triangles.size())) * unit / 3);
                auto field = [&](QPointF p, double* nn, QPointF* grad) {
                    double kk = 0;
                    if (!indexAt(ray.domain, p, ev, locator, nn, &kk)) return false;
                    const double dl = 1e-3 * h;
                    double nx1 = 0, nx0 = 0, ny1 = 0, ny0 = 0;
                    if (!indexAt(ray.domain, p + QPointF(dl, 0), ev, locator, &nx1, &kk) || !indexAt(ray.domain, p - QPointF(dl, 0), ev, locator, &nx0, &kk)
                        || !indexAt(ray.domain, p + QPointF(0, dl), ev, locator, &ny1, &kk) || !indexAt(ray.domain, p - QPointF(0, dl), ev, locator, &ny0, &kk))
                        return false;
                    *grad = QPointF((nx1 - nx0) / (2 * dl), (ny1 - ny0) / (2 * dl));
                    return true;
                };
                QPointF r = ray.p, T = ray.d * n;
                bool crossed = false, lost = false;
                for (int step = 0; step < 1000000; ++step) {
                    double n1 = 0;
                    QPointF g1, g2, g3, g4;
                    double n2 = 0, n3 = 0, n4 = 0;
                    if (!field(r, &n1, &g1)) {
                        lost = true;
                        break;
                    }
                    const QPointF k1r = T / n1, k1t = g1;
                    if (!field(r + k1r * (h / 2), &n2, &g2)) n2 = n1, g2 = g1;
                    const QPointF k2r = (T + k1t * (h / 2)) / n2, k2t = g2;
                    if (!field(r + k2r * (h / 2), &n3, &g3)) n3 = n1, g3 = g1;
                    const QPointF k3r = (T + k2t * (h / 2)) / n3, k3t = g3;
                    if (!field(r + k3r * h, &n4, &g4)) n4 = n1, g4 = g1;
                    const QPointF k4r = (T + k3t * h) / n4, k4t = g4;
                    const QPointF rNext = r + (k1r + k2r * 2 + k3r * 2 + k4r) * (h / 6);
                    const QPointF tNext = T + (k1t + k2t * 2 + k3t * 2 + k4t) * (h / 6);
                    // A boundary on the chord?
                    const QPointF chord = rNext - r;
                    const double clen = norm(chord);
                    Hit h2;
                    if (clen > 0)
                        for (int pi : a_domainPieces[std::size_t(ray.domain)]) intersect(a_pieces[std::size_t(pi)], r, chord / clen, eps, h2);
                    const double kappaHere = m.kappa.isValid() ? [&] {
                        double nn = 0, kk = 0;
                        indexAt(ray.domain, r, ev, locator, &nn, &kk);
                        return kk;
                    }() : 0.0;
                    const double alpha = 4 * M_PI * kappaHere / release.lambda;
                    if (h2.segment >= 0 && h2.s <= clen) {
                        const QPointF end = r + chord / clen * h2.s;
                        if (alpha > 0) {
                            depositAlong(r, end, ray.ps + ray.pp, alpha, deposit, locator);
                            const double keep = std::exp(-alpha * h2.s);
                            ray.ps *= keep;
                            ray.pp *= keep;
                        }
                        ray.opl += n1 * h2.s;
                        travelled += h2.s;
                        r = end;
                        dEnd = chord / clen;
                        hit = h2;
                        crossed = true;
                        break;
                    }
                    if (alpha > 0) {
                        depositAlong(r, rNext, ray.ps + ray.pp, alpha, deposit, locator);
                        const double keep = std::exp(-alpha * clen);
                        ray.ps *= keep;
                        ray.pp *= keep;
                    }
                    ray.opl += (n1 + n4) / 2 * clen;
                    travelled += clen;
                    r = rNext;
                    T = tNext;
                    if (step % 16 == 0) path.points.push_back({r.x(), r.y(), ray.ps + ray.pp, ray.opl});
                    if (ray.length + travelled >= a_maxLength || ray.ps + ray.pp <= floor) break;
                }
                ray.p = r;
                ray.d = crossed ? dEnd : T / norm(T);
                if (lost) {
                    status = 3;
                    break;
                }
                if (!crossed) continue;
            }
            ray.length += travelled;
            path.points.push_back({ray.p.x(), ray.p.y(), ray.ps + ray.pp, ray.opl});
            ++ray.interactions;
            // At the boundary.
            const Topology::Segment& seg = topo.segments[std::size_t(hit.segment)];
            const int other = seg.left == ray.domain ? seg.right : seg.left;
            const BoundaryRule& rule = seg.boundary >= 0 ? a_rules[std::size_t(seg.boundary)] : BoundaryRule();
            QPointF N = hit.normal;
            if (dot(N, ray.d) > 0) N = -N;   // against the ray
            if (rule.detector >= 0) {
                DetectorHit dh;
                dh.detector = rule.detector;
                dh.x = ray.p.x();
                dh.y = ray.p.y();
                dh.power = ray.ps + ray.pp;
                dh.opl = ray.opl;
                out.hits.push_back(dh);
                if (rule.detectorStops) {
                    status = 2;
                    break;
                }
            }
            const QPointF reflected = ray.d + N * (2 * -dot(ray.d, N));
            if (rule.kind == BoundaryRule::Absorber) {
                out.absorbedWalls += ray.ps + ray.pp;
                status = 1;
                break;
            }
            if (rule.kind == BoundaryRule::Mirror) {
                out.absorbedWalls += (ray.ps + ray.pp) * (1 - rule.reflectance);
                ray.ps *= rule.reflectance;
                ray.pp *= rule.reflectance;
                ray.d = reflected / norm(reflected);
                continue;
            }
            if (other < 0 || !f.domains[std::size_t(other)]) {
                // The outside: a wall that stops it.
                out.absorbedWalls += ray.ps + ray.pp;
                status = 1;
                break;
            }
            double n2 = 1, k2 = 0;
            if (!indexAt(other, ray.p, ev, locator, &n2, &k2)) {
                status = 3;
                break;
            }
            double n1 = n;
            if (m.graded) indexAt(ray.domain, ray.p, ev, locator, &n1, &k2);
            if (std::abs(n2 - n1) < 1e-14 * n1) {
                ray.domain = other;
                continue;
            }
            const double cosi = -dot(ray.d, N);
            const double eta = n1 / n2;
            const double sin2t = eta * eta * (1 - cosi * cosi);
            if (sin2t >= 1) {
                // Totally reflected.
                ray.d = reflected / norm(reflected);
                continue;
            }
            const double cost = std::sqrt(1 - sin2t);
            const double rs = (n1 * cosi - n2 * cost) / (n1 * cosi + n2 * cost);
            const double rp = (n2 * cosi - n1 * cost) / (n2 * cosi + n1 * cost);
            const double Rs = rule.coating ? 0 : rs * rs, Rp = rule.coating ? 0 : rp * rp;
            const QPointF refracted = ray.d * eta + N * (eta * cosi - cost);
            if (a_reflections && !rule.coating) {
                Ray back = ray;
                back.ps = ray.ps * Rs;
                back.pp = ray.pp * Rp;
                back.d = reflected / norm(reflected);
                if (back.ps + back.pp > floor) stack.push_back(back);
            }
            ray.ps *= 1 - Rs;
            ray.pp *= 1 - Rp;
            ray.d = refracted / norm(refracted);
            ray.domain = other;
        }
        path.status = status;
        if (path.points.size() < 2 || norm(QPointF(path.points.back()[0], path.points.back()[1]) - ray.p) > 0)
            path.points.push_back({ray.p.x(), ray.p.y(), ray.ps + ray.pp, ray.opl});
        out.paths.push_back(std::move(path));
    }
}

bool RayProblem::solve(const SolveOptions& o, QString* error)
{
    (void)o;
    (void)error;
    Field& f = field();
    const Mesh& mesh = a_s.mesh();
    const Locator locator(mesh);
    const int nt = int(mesh.triangles.size());
    // Each release traced on its own (in threads): its paths, its hits.
    const int count = int(a_releases.size());
    std::vector<Traced> traced(static_cast<std::size_t>(count));
    const int threads = count < 8 ? 1 : std::clamp(int(std::thread::hardware_concurrency()), 1, 16);
    std::vector<std::vector<double>> deposits(std::size_t(threads), std::vector<double>(std::size_t(nt), 0.0));
    auto work = [&](int thread, int begin, int end) {
        PointEvaluator ev(a_s);
        for (int k = begin; k < end; ++k) trace(a_releases[std::size_t(k)], traced[std::size_t(k)], deposits[std::size_t(thread)], ev, locator);
    };
    if (threads == 1) work(0, 0, count);
    else {
        Threads pool;
        const int chunk = (count + threads - 1) / threads;
        for (int k = 0; k < threads; ++k) {
            const int b = k * chunk, e = std::min(count, b + chunk);
            if (b >= e) break;
            pool.start([&work, k, b, e] { work(k, b, e); });
        }
        pool.join();
    }
    // The power absorbed by triangle, as a density at the nodes: each node
    // the mean of its absorbing triangles', by area - its integral over the
    // absorbing media kept (what heat or a semiconductor there takes).
    std::vector<double> deposit(std::size_t(nt), 0.0);
    for (const std::vector<double>& d : deposits)
        for (int i = 0; i < nt; ++i) deposit[std::size_t(i)] += d[std::size_t(i)];
    a_released = a_absorbedMedia = a_absorbedWalls = 0;
    a_traced = 0;
    for (Detector& d : a_detectors) d = Detector{d.name};
    for (int k = 0; k < count; ++k) {
        const Traced& t = traced[std::size_t(k)];
        a_released += a_releases[std::size_t(k)].power;
        a_absorbedWalls += t.absorbedWalls;
        a_traced += t.rays;
        for (const DetectorHit& h : t.hits) {
            Detector& d = a_detectors[std::size_t(h.detector)];
            ++d.count;
            d.power += h.power;
            d.px += h.power * h.x;
            d.py += h.power * h.y;
            d.pxx += h.power * h.x * h.x;
            d.pyy += h.power * h.y * h.y;
            d.opl += h.power * h.opl;
        }
    }
    a_primaries = count;
    const Space& sp = *f.space;
    std::vector<double> mass(std::size_t(sp.size()), 0.0), q(std::size_t(sp.size()), 0.0);
    for (int t = 0; t < nt; ++t) {
        const auto& tri = mesh.triangles[std::size_t(t)];
        const QPointF a = mesh.nodes[std::size_t(tri[0])] * mesh.unitScale, b = mesh.nodes[std::size_t(tri[1])] * mesh.unitScale,
                      c = mesh.nodes[std::size_t(tri[2])] * mesh.unitScale;
        const double area = std::abs(cross(b - a, c - a)) / 2;
        const double volume = area * a_s.weight((a + b + c) / 3.0);
        a_absorbedMedia += deposit[std::size_t(t)];
        const int d = mesh.domain[std::size_t(t)];
        if (!(volume > 0) || d < 0 || !a_media[std::size_t(d)].absorbing) continue;
        const double density = deposit[std::size_t(t)] / volume;
        const int* dof = sp.dofs(t);
        for (int k = 0; k < 3; ++k) {
            mass[std::size_t(dof[k])] += volume / 3;
            q[std::size_t(dof[k])] += density * volume / 3;
        }
    }
    f.values.assign(std::size_t(sp.size()), 0.0);
    for (int i = 0; i < sp.size(); ++i)
        if (mass[std::size_t(i)] > 0) f.values[std::size_t(i)] = q[std::size_t(i)] / mass[std::size_t(i)];
    // The paths: [rays, then each: points, λ, index, status, then x, y, P, opl
    // a point] (as the plots read them).
    std::vector<double>& ex = f.extra;
    ex.clear();
    std::size_t total = 0;
    for (const Traced& t : traced) total += t.paths.size();
    ex.push_back(double(total));
    for (const Traced& t : traced)
        for (const Path& p : t.paths) {
            ex.push_back(double(p.points.size()));
            ex.push_back(p.lambda);
            ex.push_back(double(p.index));
            ex.push_back(double(p.status));
            for (const auto& pt : p.points) ex.insert(ex.end(), pt.begin(), pt.end());
        }
    f.solved = true;
    a_s.log << tr("%1: %2 rays released, %3 traced (reflections too)").arg(a_node->name()).arg(a_primaries).arg(a_traced);
    return true;
}

void RayProblem::setGlobals()
{
    const QString p = field().tag + QLatin1Char('.');
    a_s.setGlobal(p + QStringLiteral("N"), a_primaries, Dim(), tr("rays released"));
    a_s.setGlobal(p + QStringLiteral("Ntraced"), a_traced, Dim(), tr("rays traced, the reflected ones too"));
    a_s.setGlobal(p + QStringLiteral("Pin"), a_released, Dim::power(), tr("power released"));
    a_s.setGlobal(p + QStringLiteral("Pabs"), a_absorbedMedia, Dim::power(), tr("power absorbed in the media"));
    a_s.setGlobal(p + QStringLiteral("Pwall"), a_absorbedWalls, Dim::power(), tr("power stopped by walls, absorbers and mirrors"));
    for (const Detector& d : a_detectors) {
        const QString s = QLatin1Char('_') + d.name;
        const double P = d.power;
        const double x = P > 0 ? d.px / P : NaN, y = P > 0 ? d.py / P : NaN;
        const double rms = P > 0 ? std::sqrt(std::max(0.0, d.pxx / P - x * x + d.pyy / P - y * y)) : NaN;
        a_s.setGlobal(p + QStringLiteral("N") + s, d.count, Dim(), tr("rays at detector %1").arg(d.name));
        a_s.setGlobal(p + QStringLiteral("P") + s, P, Dim::power(), tr("power at detector %1").arg(d.name));
        a_s.setGlobal(p + QStringLiteral("x") + s, x, Dim::length(), tr("centroid of the rays at %1, x").arg(d.name));
        a_s.setGlobal(p + QStringLiteral("y") + s, y, Dim::length(), tr("centroid of the rays at %1, y").arg(d.name));
        a_s.setGlobal(p + QStringLiteral("rms") + s, rms, Dim::length(), tr("rms spot size at %1 (about the centroid, by power)").arg(d.name));
        a_s.setGlobal(p + QStringLiteral("opl") + s, P > 0 ? d.opl / P : NaN, Dim::length(), tr("mean optical path to %1").arg(d.name));
    }
}

} // namespace

std::unique_ptr<Problem> makeRayProblem(Solution& solution, int field)
{
    return std::make_unique<RayProblem>(solution, field);
}

} // namespace qucs_s::fem::detail

namespace qucs_s::fem {

std::vector<RayPath> rayPaths(const Field& field)
{
    std::vector<RayPath> out;
    const std::vector<double>& ex = field.extra;
    if (field.type != QLatin1String("rays") || ex.empty()) return out;
    const int rays = int(ex[0]);
    std::size_t at = 1;
    for (int r = 0; r < rays && at + 4 <= ex.size(); ++r) {
        RayPath p;
        const int points = int(ex[at]);
        p.lambda = ex[at + 1];
        p.index = int(ex[at + 2]);
        p.status = int(ex[at + 3]);
        at += 4;
        for (int k = 0; k < points && at + 4 <= ex.size(); ++k, at += 4) p.points.push_back({ex[at], ex[at + 1], ex[at + 2], ex[at + 3]});
        out.push_back(std::move(p));
    }
    return out;
}

} // namespace qucs_s::fem

