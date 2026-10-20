/*
 * fem_process.cpp - a process flow: its shape steps on Clipper's integer
 *                   polygons (a process grid's 64th), its masks from a
 *                   layout's layers along a cut; its dopants on a grid
 *                   (fem_process_dopants.cpp)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_process.h"
#include "fem_process_p.h"

#include "fem_expr.h"
#include "fem_import.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <map>

namespace qucs_s::fem {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::fem::Process", text);
}

double loopArea(const std::vector<QPointF>& loop)
{
    double s = 0;
    for (std::size_t i = 0; i < loop.size(); ++i) {
        const QPointF a = loop[i], b = loop[(i + 1) % loop.size()];
        s += a.x() * b.y() - b.x() * a.y();
    }
    return s / 2;
}

} // namespace

double ProcessRegion::area() const
{
    double s = 0;
    for (const auto& loop : loops) s += loopArea(loop);
    return s;
}

double ProcessState::top() const
{
    double t = -std::numeric_limits<double>::infinity();
    for (const ProcessRegion& r : regions)
        for (const auto& loop : r.loops)
            for (const QPointF& p : loop) t = std::max(t, p.y());
    return std::isfinite(t) ? t : bottom;
}

namespace process {

// ------------------------------------------------------------- Polygons

Paths unite(const Paths& a)
{
    if (a.empty()) return {};
    ClipperLib::Clipper c;
    c.AddPaths(a, ClipperLib::ptSubject, true);
    Paths r;
    c.Execute(ClipperLib::ctUnion, r, ClipperLib::pftNonZero, ClipperLib::pftNonZero);
    return r;
}

Paths clip(ClipperLib::ClipType type, const Paths& a, const Paths& b)
{
    if (a.empty()) return type == ClipperLib::ctUnion || type == ClipperLib::ctXor ? unite(b) : Paths();
    if (b.empty()) return type == ClipperLib::ctIntersection ? Paths() : unite(a);
    ClipperLib::Clipper c;
    c.AddPaths(a, ClipperLib::ptSubject, true);
    c.AddPaths(b, ClipperLib::ptClip, true);
    Paths r;
    c.Execute(type, r, ClipperLib::pftNonZero, ClipperLib::pftNonZero);
    return r;
}

Paths offset(const Paths& a, double by, double arcTolerance)
{
    if (a.empty() || by == 0) return a;
    ClipperLib::ClipperOffset o(2.0, arcTolerance);
    o.AddPaths(a, ClipperLib::jtRound, ClipperLib::etClosedPolygon);
    Paths r;
    o.Execute(r, by);
    return r;
}

Paths rectangle(cInt x0, cInt y0, cInt x1, cInt y1)
{
    if (x1 <= x0 || y1 <= y0) return {};
    return {Path{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}};
}

Paths sweep(const Paths& a, cInt dx, cInt dy)
{
    // Every place a takes moving by (dx, dy): it, it moved, and the
    // parallelogram each edge sweeps - its holes too, which Clipper's
    // Minkowski sum fills.
    if (a.empty() || (dx == 0 && dy == 0)) return a;
    Paths all = a;
    for (const Path& p : a) {
        Path moved;
        for (const ClipperLib::IntPoint& q : p) moved.push_back({q.X + dx, q.Y + dy});
        all.push_back(moved);
        for (std::size_t i = 0; i < p.size(); ++i) {
            const ClipperLib::IntPoint s = p[i], e = p[(i + 1) % p.size()];
            Path quad{s, e, {e.X + dx, e.Y + dy}, {s.X + dx, s.Y + dy}};
            if (!ClipperLib::Orientation(quad)) std::reverse(quad.begin(), quad.end());
            if (ClipperLib::Area(quad) != 0) all.push_back(quad);
        }
    }
    return unite(all);
}

double area(const Paths& a)
{
    double s = 0;
    for (const Path& p : a) s += ClipperLib::Area(p);
    return s;
}

std::pair<cInt, cInt> yRange(const Paths& a)
{
    cInt lo = std::numeric_limits<cInt>::max(), hi = std::numeric_limits<cInt>::min();
    for (const Path& p : a)
        for (const ClipperLib::IntPoint& q : p) {
            lo = std::min(lo, q.Y);
            hi = std::max(hi, q.Y);
        }
    return {lo, hi};
}

void snap(Paths& a, cInt step)
{
    for (Path& p : a)
        for (ClipperLib::IntPoint& q : p) {
            q.X = cInt(std::llround(double(q.X) / double(step))) * step;
            q.Y = cInt(std::llround(double(q.Y) / double(step))) * step;
        }
    ClipperLib::CleanPolygons(a, 1.415);
    a = unite(a);
    a.erase(std::remove_if(a.begin(), a.end(), [](const Path& p) { return p.size() < 3 || ClipperLib::Area(p) == 0; }), a.end());
}

std::vector<Paths> components(const Paths& a)
{
    std::vector<Paths> out;
    if (a.empty()) return out;
    ClipperLib::Clipper c;
    c.AddPaths(a, ClipperLib::ptSubject, true);
    ClipperLib::PolyTree tree;
    c.Execute(ClipperLib::ctUnion, tree, ClipperLib::pftNonZero, ClipperLib::pftNonZero);
    std::vector<const ClipperLib::PolyNode*> outers;
    for (const ClipperLib::PolyNode* n : tree.Childs) outers.push_back(n);
    while (!outers.empty()) {
        const ClipperLib::PolyNode* n = outers.back();
        outers.pop_back();
        Paths one{n->Contour};
        for (const ClipperLib::PolyNode* hole : n->Childs) {
            one.push_back(hole->Contour);
            for (const ClipperLib::PolyNode* island : hole->Childs) outers.push_back(island);
        }
        out.push_back(one);
    }
    return out;
}

} // namespace process

namespace {

using namespace process;

bool makesRegionImpl(const QString& type)
{
    static const QStringList made = {QStringLiteral("process/substrate"), QStringLiteral("process/deposit"), QStringLiteral("process/epitaxy"),
                                     QStringLiteral("process/fill"), QStringLiteral("process/oxidize")};
    return made.contains(type);
}

/// The name a step's region takes: its label, else its tag.
QString regionName(const Node& step)
{
    return step.label.isEmpty() ? step.tag : step.label;
}

/*!
 * A flow run: its regions on Clipper's integers - a 64th of the process
 * grid each -, its window x0 to x1 above the substrate's bottom, the
 * surface at y = 0 to start; each step done to them in order, then the
 * regions snapped to the grid, cleaned and rid of slivers.
 */
class Runner
{
public:
    Runner(const Node& flow, const Model& model, const ParameterScope& ps, double unitScale)
        : a_flow(flow), a_model(model), a_ps(ps), a_unitScale(unitScale)
    {
    }

    ProcessRun run(const QString& until)
    {
        ProcessRun out;
        a_out = &out;
        setUp();
        if (!a_error.isEmpty()) {
            out.error = a_error;
            out.failedAt = a_flow.tag;
            return out;
        }
        bool started = false;
        for (const Node& step : a_flow.children) {
            if (!step.enabled) continue;
            a_error.clear();
            a_warnings.clear();
            a_said.clear();
            QElapsedTimer clock;
            clock.start();
            const bool start = step.type == QLatin1String("process/substrate") || step.type == QLatin1String("process/stack");
            if (start && started) fail(tr("a flow starts once: a substrate or an epitaxial stack, first"));
            else if (!start && !started) fail(tr("a flow starts with a substrate or an epitaxial stack"));
            else doStep(step);
            started = started || start;
            if (a_error.isEmpty()) finish(step);
            ProcessStepReport report{step.tag, step.name(), a_said, double(clock.nsecsElapsed()) / 1e6, a_warnings};
            out.steps.push_back(report);
            for (const QString& w : a_warnings) out.warnings << tr("%1: %2").arg(step.name(), w);
            if (!a_error.isEmpty()) {
                out.error = tr("%1: %2").arg(step.name(), a_error);
                out.failedAt = step.tag;
                break;
            }
            if (!until.isEmpty() && step.tag == until) break;
        }
        if (!started && out.error.isEmpty()) {
            out.error = tr("%1: no steps - a flow starts with a substrate or an epitaxial stack").arg(a_flow.name());
            out.failedAt = a_flow.tag;
        }
        out.state = state();
        out.files = a_files;
        return out;
    }

private:
    // --- Values ---------------------------------------------------------
    double evaluate(const QString& text, const Dim& expected, double lengthUnit, bool numbersAreLengths)
    {
        Expression::Options options;
        options.lengthUnit = lengthUnit;
        options.numbersAreLengths = numbersAreLengths;
        const Expression e = Expression::compile(text, a_ps.scope, options);
        if (!e.isValid()) {
            fail(tr("%1: %2").arg(text, e.error()));
            return 0;
        }
        if (!e.isConstant()) {
            fail(tr("%1 must be a constant").arg(text));
            return 0;
        }
        if (!e.dim().isNone() && !(e.dim() == expected)) warn(tr("%1 is in %2, where %3 is expected").arg(text, dimName(e.dim()), dimName(expected)));
        if (!std::isfinite(e.constant())) fail(tr("%1 is not a finite number").arg(text));
        return e.constant();
    }
    /// A length in the geometry's unit: a plain number is in it.
    double length(const QString& text) { return evaluate(text, Dim::length(), a_unitScale, true); }
    double number(const QString& text, double fallback = 0)
    {
        if (text.trimmed().isEmpty()) return fallback;
        return evaluate(text, Dim(), a_unitScale, false);
    }
    /// An angle, in radians: a plain number in degrees.
    double angle(const QString& text)
    {
        const double v = evaluate(text, Dim(), a_unitScale, false);
        if (text.contains(QLatin1String("[rad]")) || text.contains(QLatin1String("[deg]"))) return v;
        return v * M_PI / 180;
    }
    /// A length in µm (a plain number), in the geometry's unit.
    double micrometres(const QString& text) { return evaluate(text, Dim::length(), 1e-6, true) * 1e-6 / a_unitScale; }
    void fail(const QString& why)
    {
        if (a_error.isEmpty()) a_error = why;
    }
    void warn(const QString& what)
    {
        if (!a_warnings.contains(what)) a_warnings << what;
    }
    void say(const QString& what) { a_said = what; }
    cInt toInt(double v) const { return cInt(std::llround(v * a_k)); }
    double toGeom(cInt v) const { return double(v) / a_k; }
    QString lengthText(double geometryLength) const
    {
        return formatQuantity(geometryLength * a_unitScale, Dim::length());
    }

    // --- The flow's set-up: its grid, window, the layout's cut ----------
    void setUp()
    {
        a_grid = length(a_flow.text(QStringLiteral("grid")));
        if (!a_error.isEmpty()) return;
        if (!(a_grid > 0)) return fail(tr("its process grid must be more than 0"));
        a_k = 64 / a_grid;
        a_step = 64;
        a_minimum = length(a_flow.text(QStringLiteral("minimum")));
        if (!(a_minimum >= 0)) a_minimum = 0;
        a_arc = std::max(1.0, 0.05 * 64);
        const QString file = a_flow.text(QStringLiteral("file")).trimmed();
        if (file.isEmpty()) {
            const double x0 = length(a_flow.text(QStringLiteral("x0")));
            const double width = length(a_flow.text(QStringLiteral("width")));
            if (!a_error.isEmpty()) return;
            if (!(width > 0)) return fail(tr("its width must be more than 0"));
            a_x0 = toInt(x0);
            a_x1 = toInt(x0 + width);
            return;
        }
        // Along a layout's cut: x from its start (or its middle), in the
        // geometry's unit; the layout's µm.
        a_layout = file;
        if (QFileInfo(file).isRelative()) {
            if (a_model.fileName.isEmpty())
                return fail(tr("%1 is relative to the model's folder: save the model first, or give its whole path").arg(file));
            a_layout = QFileInfo(a_model.fileName).absoluteDir().absoluteFilePath(file);
        }
        a_files << QFileInfo(a_layout).absoluteFilePath();
        const QStringList s0 = a_flow.pair(QStringLiteral("start")), s1 = a_flow.pair(QStringLiteral("end"));
        a_cutA = QPointF(evaluate(s0.value(0), Dim::length(), 1e-6, true), evaluate(s0.value(1), Dim::length(), 1e-6, true));
        a_cutB = QPointF(evaluate(s1.value(0), Dim::length(), 1e-6, true), evaluate(s1.value(1), Dim::length(), 1e-6, true));
        if (!a_error.isEmpty()) return;
        const double cut = std::hypot(a_cutB.x() - a_cutA.x(), a_cutB.y() - a_cutA.y());
        if (!(cut > 0)) return fail(tr("the cut's start and end are the same point"));
        a_um = 1e-6 / a_unitScale;
        a_shift = a_flow.text(QStringLiteral("origin")) == QLatin1String("center") ? cut / 2 : 0;
        a_x0 = toInt(-a_shift * a_um);
        a_x1 = toInt((cut - a_shift) * a_um);
        // Every layer a mask names, read once.
        QStringList layers;
        for (const Node& step : a_flow.children)
            if (step.enabled && step.text(QStringLiteral("mask")) == QLatin1String("layer")) {
                LayoutLayer key;
                if (layoutLayerNamed(step.text(QStringLiteral("layer")), a_layout, &key) && !layers.contains(key.text())) layers << key.text();
            }
        if (layers.isEmpty()) return;
        const QRectF window = QRectF(a_cutA, a_cutB).normalized().adjusted(-1e-3, -1e-3, 1e-3, 1e-3);
        a_shapes = readLayoutShapes(a_layout, a_flow.text(QStringLiteral("cell")), layers.join(QLatin1String(", ")), window);
        if (!a_shapes.error.isEmpty()) return fail(a_shapes.error);
    }

    // --- The state ---------------------------------------------------------
    Paths solids() const
    {
        Paths all;
        for (const Region& r : a_regions) all.insert(all.end(), r.p.begin(), r.p.end());
        return unite(all);
    }
    cInt topOf(const Paths& p) const
    {
        const auto [lo, hi] = yRange(p);
        return p.empty() ? a_bottom : hi;
    }
    /// The window, from below the bottom to \a ceiling.
    Paths window(cInt ceiling) const { return rectangle(a_x0, a_bottom, a_x1, ceiling); }
    /// Vertical strips over \a open (x intervals), from \a lo to \a hi.
    Paths columns(const std::vector<std::pair<cInt, cInt>>& open, cInt lo, cInt hi) const
    {
        Paths out;
        for (const auto& [a, b] : open)
            if (b > a) out.push_back(rectangle(a, lo, b, hi).front());
        return out;
    }
    ProcessState state() const
    {
        ProcessState s;
        s.x0 = toGeom(a_x0);
        s.x1 = toGeom(a_x1);
        s.bottom = toGeom(a_bottom);
        for (const Region& r : a_regions) {
            ProcessRegion out;
            out.name = r.name;
            out.material = r.material;
            out.crystal = r.crystal;
            for (const Path& p : r.p) {
                std::vector<QPointF> loop;
                for (const ClipperLib::IntPoint& q : p) loop.push_back(QPointF(toGeom(q.X), toGeom(q.Y)));
                out.loops.push_back(loop);
            }
            s.regions.push_back(out);
        }
        return s;
    }
    void addRegion(const QString& name, const QString& material, bool crystal, const Paths& p)
    {
        if (p.empty() || area(p) <= 0) return;
        a_regions.push_back(Region{name, material, crystal, p});
    }

    // --- Masks --------------------------------------------------------------
    /*!
     * A step's mask: where it is open, as x intervals - a layout's layer
     * along the cut (open where drawn, or where not), or typed in -, each
     * opening grown by its bias. None: open everywhere (an empty list and
     * \a any false).
     */
    std::vector<std::pair<cInt, cInt>> maskOpenings(const Node& step, bool* any)
    {
        *any = false;
        const QString kind = step.text(QStringLiteral("mask"));
        if (kind.isEmpty() || kind == QLatin1String("none")) return {};
        *any = true;
        std::vector<std::pair<double, double>> drawn;   // geometry units
        if (kind == QLatin1String("layer")) {
            if (a_layout.isEmpty()) {
                fail(tr("its mask is a layout's layer: give the flow its layout and cut (or type the openings)"));
                return {};
            }
            LayoutLayer key;
            if (!layoutLayerNamed(step.text(QStringLiteral("layer")), a_layout, &key)) {
                const QString lyp = layoutLayerPropertiesFile(a_layout);
                fail(lyp.isEmpty() ? tr("%1 is not a layer (as 1/0; a name needs the layout's .lyp beside it)").arg(step.text(QStringLiteral("layer")))
                                   : tr("%1 is not a layer: as 1/0, or a name %2 gives").arg(step.text(QStringLiteral("layer")), QFileInfo(lyp).fileName()));
                return {};
            }
            if (const LayoutShapes::Layer* l = a_shapes.layer(key))
                for (const auto& [t0, t1] : cutIntervals(l->loops, a_cutA, a_cutB)) drawn.emplace_back((t0 - a_shift) * a_um, (t1 - a_shift) * a_um);
            if (drawn.empty()) warn(tr("the cut crosses nothing of layer %1").arg(key.text()));
            if (step.text(QStringLiteral("polarity")) == QLatin1String("undrawn")) {
                // Open where it is not drawn: the window less what is.
                std::vector<std::pair<double, double>> open;
                double from = toGeom(a_x0);
                for (const auto& [a, b] : drawn) {
                    if (a > from) open.emplace_back(from, a);
                    from = std::max(from, b);
                }
                if (from < toGeom(a_x1)) open.emplace_back(from, toGeom(a_x1));
                drawn = open;
            }
        } else {
            // "x0, x1; x2, x3": the openings, in the geometry's unit.
            for (const QString& pairText : step.text(QStringLiteral("openings")).split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
                const QStringList ends = pairText.split(QLatin1Char(','));
                if (ends.size() != 2) {
                    fail(tr("its openings are pairs - from, to - split by ';': \"%1\" is not one").arg(pairText.trimmed()));
                    return {};
                }
                const double a = length(ends.at(0)), b = length(ends.at(1));
                if (!a_error.isEmpty()) return {};
                drawn.emplace_back(std::min(a, b), std::max(a, b));
            }
            if (drawn.empty()) warn(tr("its mask has no openings: closed everywhere"));
        }
        const double bias = step.text(QStringLiteral("bias")).trimmed().isEmpty() ? 0 : length(step.text(QStringLiteral("bias")));
        std::vector<std::pair<cInt, cInt>> open;
        for (const auto& [a, b] : drawn) {
            cInt lo = std::max(toInt(a - bias), a_x0), hi = std::min(toInt(b + bias), a_x1);
            if (hi > lo) open.emplace_back(lo, hi);
        }
        // (Overlapping once biased: one.)
        std::sort(open.begin(), open.end());
        std::vector<std::pair<cInt, cInt>> merged;
        for (const auto& o : open) {
            if (!merged.empty() && o.first <= merged.back().second) merged.back().second = std::max(merged.back().second, o.second);
            else merged.push_back(o);
        }
        return merged;
    }
    /// The resist a mask leaves: its thickness over the surface where it is
    /// closed (spun on - level - then opened with vertical walls).
    Paths resist(const std::vector<std::pair<cInt, cInt>>& open, double thickness, const Paths& all)
    {
        const cInt level = topOf(all) + toInt(thickness);
        Paths coat = clip(ClipperLib::ctDifference, rectangle(a_x0, a_bottom, a_x1, level), all);
        return clip(ClipperLib::ctDifference, coat, columns(open, a_bottom - toInt(1), level + toInt(1)));
    }

    // --- Steps -----------------------------------------------------------
    void doStep(const Node& step)
    {
        const QString& t = step.type;
        if (t == QLatin1String("process/substrate")) substrate(step);
        else if (t == QLatin1String("process/stack")) stack(step);
        else if (t == QLatin1String("process/deposit")) deposit(step);
        else if (t == QLatin1String("process/etch")) etch(step);
        else if (t == QLatin1String("process/planarize")) planarize(step);
        else if (t == QLatin1String("process/strip")) strip(step);
        else if (t == QLatin1String("process/epitaxy")) epitaxy(step);
        else if (t == QLatin1String("process/fill")) fill(step);
        else fail(tr("%1 is not a process step").arg(t));
    }
    QString material(const Node& step)
    {
        const QString m = step.text(QStringLiteral("material")).trimmed();
        if (m.isEmpty()) fail(tr("no material: name it (a library entry's name, as the materials take it)"));
        return m;
    }
    double positiveLength(const Node& step, const QString& key, const QString& what)
    {
        const double v = length(step.text(key));
        if (a_error.isEmpty() && !(v > 0)) fail(tr("its %1 must be more than 0").arg(what));
        return v;
    }
    void substrate(const Node& step)
    {
        const QString m = material(step);
        const double thickness = positiveLength(step, QStringLiteral("thickness"), tr("thickness"));
        if (!a_error.isEmpty()) return;
        a_bottom = -toInt(thickness);
        a_orientation = step.text(QStringLiteral("orientation"));
        addRegion(regionName(step), m, true, rectangle(a_x0, a_bottom, a_x1, 0));
        say(tr("%1 of %2, (%3), %4 wide").arg(lengthText(thickness), m, a_orientation, lengthText(toGeom(a_x1 - a_x0))));
    }
    void stack(const Node& step)
    {
        // Its rows from the bottom: name, material, thickness - the last's
        // top at y = 0.
        struct Row {
            QString name, material;
            double thickness;
        };
        std::vector<Row> rows;
        double total = 0;
        for (const QJsonValue& v : step.value(QStringLiteral("layers")).toArray()) {
            const QJsonArray r = v.toArray();
            auto cell = [&](int i) { return r.at(i).isDouble() ? formatNumber(r.at(i).toDouble(), 15) : r.at(i).toString().trimmed(); };
            Row row{cell(0), cell(1), 0};
            if (row.name.isEmpty()) return fail(tr("a layer of the stack has no name"));
            if (row.material.isEmpty()) return fail(tr("%1: no material").arg(row.name));
            row.thickness = length(cell(2));
            if (!a_error.isEmpty()) return;
            if (!(row.thickness > 0)) return fail(tr("%1: its thickness must be more than 0").arg(row.name));
            total += row.thickness;
            rows.push_back(row);
        }
        if (rows.empty()) return fail(tr("the stack has no layers: give its rows (name, material, thickness), the bottom first"));
        a_orientation = step.text(QStringLiteral("orientation"));
        cInt y = -toInt(total);
        a_bottom = y;
        for (const Row& row : rows) {
            const cInt top = y + toInt(row.thickness);
            addRegion(row.name, row.material, true, rectangle(a_x0, y, a_x1, top));
            y = top;
        }
        say(tr("%1 layers, %2 in all").arg(rows.size()).arg(lengthText(total)));
    }
    void deposit(const Node& step)
    {
        const QString m = material(step);
        const double t = positiveLength(step, QStringLiteral("thickness"), tr("thickness"));
        bool masked = false;
        const auto open = maskOpenings(step, &masked);
        if (!a_error.isEmpty()) return;
        const Paths all = solids();
        const cInt top = topOf(all), ti = toInt(t);
        const QString mode = step.text(QStringLiteral("mode"));
        Paths made;
        if (mode == QLatin1String("planar")) {
            made = clip(ClipperLib::ctDifference, rectangle(a_x0, a_bottom, a_x1, top + ti), all);
        } else if (mode == QLatin1String("directional")) {
            // Down from above only: on what the sky sees.
            const Paths sky = clip(ClipperLib::ctDifference, window(top + 2 * ti + toInt(a_grid)), sweep(all, 0, a_bottom - top - toInt(a_grid)));
            made = clip(ClipperLib::ctIntersection, clip(ClipperLib::ctDifference, sweep(all, 0, ti), all), sky);
        } else {
            // Conformal: within its thickness of every surface.
            made = clip(ClipperLib::ctDifference, offset(all, double(ti), a_arc), all);
        }
        made = clip(ClipperLib::ctIntersection, made, window(top + 2 * ti + toInt(a_grid)));
        // A lift-off: where the mask is open alone.
        if (masked) made = clip(ClipperLib::ctIntersection, made, columns(open, a_bottom - toInt(1), top + 3 * ti + toInt(a_grid)));
        addRegion(regionName(step), m, false, made);
        say(tr("%1 of %2, %3%4").arg(lengthText(t), m, mode.isEmpty() ? QStringLiteral("conformal") : mode,
                                    masked ? tr(", lifted off but in %1 openings").arg(open.size()) : QString()));
    }
    void fill(const Node& step)
    {
        const QString m = material(step);
        const Paths all = solids();
        const cInt top = topOf(all);
        cInt level = top;
        if (!step.text(QStringLiteral("height")).trimmed().isEmpty()) level = toInt(length(step.text(QStringLiteral("height"))));
        else level = top + toInt(positiveLength(step, QStringLiteral("thickness"), tr("thickness")));
        if (!a_error.isEmpty()) return;
        if (level <= a_bottom) return fail(tr("its height is below the substrate's bottom"));
        const Paths made = clip(ClipperLib::ctDifference, rectangle(a_x0, a_bottom, a_x1, level), all);
        addRegion(regionName(step), m, false, made);
        say(tr("%1 to y = %2").arg(m, lengthText(toGeom(level))));
    }
    void epitaxy(const Node& step)
    {
        const QString m = material(step);
        const double t = positiveLength(step, QStringLiteral("thickness"), tr("thickness"));
        if (!a_error.isEmpty()) return;
        const bool selective = step.flag(QStringLiteral("selective"));
        const Paths all = solids();
        Paths seed;
        for (const Region& r : a_regions)
            if (r.crystal || !selective) seed.insert(seed.end(), r.p.begin(), r.p.end());
        seed = unite(seed);
        if (seed.empty()) return fail(tr("nothing crystalline to grow on"));
        const cInt ti = toInt(t);
        Paths made = clip(ClipperLib::ctDifference, sweep(seed, 0, ti), all);
        made = clip(ClipperLib::ctIntersection, made, window(topOf(all) + 2 * ti + toInt(a_grid)));
        if (made.empty()) warn(tr("no crystal is open to grow on"));
        addRegion(regionName(step), m, true, made);
        say(tr("%1 of %2%3").arg(lengthText(t), m, selective ? tr(", on crystal alone") : QString()));
    }
    void planarize(const Node& step)
    {
        cInt level = 0;
        const QString to = step.text(QStringLiteral("to")).trimmed();
        if (!step.text(QStringLiteral("height")).trimmed().isEmpty()) {
            level = toInt(length(step.text(QStringLiteral("height"))));
        } else if (!to.isEmpty()) {
            Paths stop;
            for (const Region& r : a_regions)
                if (r.material.compare(to, Qt::CaseInsensitive) == 0 || r.name == to) stop.insert(stop.end(), r.p.begin(), r.p.end());
            if (stop.empty()) return fail(tr("there is no %1 to stop on").arg(to));
            level = topOf(stop);
        } else {
            return fail(tr("give it a height, or a material it stops on"));
        }
        if (!a_error.isEmpty()) return;
        const Paths keep = rectangle(a_x0 - toInt(1), a_bottom - toInt(1), a_x1 + toInt(1), level);
        double removed = 0;
        for (Region& r : a_regions) {
            const double before = area(r.p);
            r.p = clip(ClipperLib::ctIntersection, r.p, keep);
            removed += before - area(r.p);
        }
        say(tr("to y = %1, %2 removed").arg(lengthText(toGeom(level)), areaText(removed)));
    }
    QString areaText(double intArea) const
    {
        return formatQuantity(intArea / (a_k * a_k) * a_unitScale * a_unitScale, Dim::of(2));
    }
    void strip(const Node& step)
    {
        const QString m = step.text(QStringLiteral("material")).trimmed();
        if (m.isEmpty()) return fail(tr("no material: name what it strips"));
        const Paths all = solids();
        const Paths air = clip(ClipperLib::ctDifference, window(topOf(all) + toInt(a_grid)), all);
        int stripped = 0, kept = 0;
        for (Region& r : a_regions) {
            if (r.material.compare(m, Qt::CaseInsensitive) != 0 && r.name != m) continue;
            Paths left;
            for (const Paths& piece : components(r.p)) {
                // Exposed: it touches the air (a piece buried stays).
                const bool exposed = area(clip(ClipperLib::ctIntersection, offset(piece, 2.0 * double(a_step), a_arc), air)) > 0;
                if (exposed) ++stripped;
                else {
                    ++kept;
                    left.insert(left.end(), piece.begin(), piece.end());
                }
            }
            r.p = left;
        }
        if (stripped == 0) warn(tr("there is no %1 exposed to strip").arg(m));
        say(kept > 0 ? tr("%1: %2 piece(s) stripped, %3 buried kept").arg(m).arg(stripped).arg(kept)
                     : tr("%1: %2 piece(s) stripped").arg(m).arg(stripped));
    }

    /*!
     * An etch: a front - the air, the mask's openings - moving into each
     * material at its rate (1 for those it etches, 1/S for a selectivity
     * S, none for the stop and the mask unless given), in steps: down
     * (anisotropic), every way (isotropic), or down within its sidewalls'
     * slopes from the mask's edges (tapered, a crystal plane's). For a
     * depth (of a material etched at 1), or until the stop is reached in
     * every opening, then its over-etch.
     */
    void etch(const Node& step)
    {
        const QString mode = step.text(QStringLiteral("mode"));
        const QString stopMaterial = step.text(QStringLiteral("stop")).trimmed();
        const QString depthText = step.text(QStringLiteral("depth")).trimmed();
        double depth = depthText.isEmpty() ? 0 : length(depthText);
        if (!a_error.isEmpty()) return;
        if (depthText.isEmpty() && stopMaterial.isEmpty()) return fail(tr("give it a depth, or a material it stops on"));
        if (!depthText.isEmpty() && !(depth > 0)) return fail(tr("its depth must be more than 0"));
        const double over = number(step.text(QStringLiteral("overetch")), 0);
        if (!(over >= 0)) return fail(tr("its over-etch is a fraction: 0, or more"));
        bool masked = false;
        const auto open = maskOpenings(step, &masked);
        if (!a_error.isEmpty()) return;
        // The rates, by material.
        QStringList targets;
        for (const QString& m : step.text(QStringLiteral("materials")).split(QLatin1Char(','), Qt::SkipEmptyParts)) targets << m.trimmed();
        std::map<QString, double> slower;
        for (const QJsonValue& v : step.value(QStringLiteral("selectivity")).toArray()) {
            const QJsonArray r = v.toArray();
            const QString m = r.at(0).toString().trimmed();
            const double S = r.at(1).isDouble() ? r.at(1).toDouble() : number(r.at(1).toString());
            if (m.isEmpty()) continue;
            if (!(S > 0)) return fail(tr("%1: its selectivity must be more than 0").arg(m));
            slower[m.toLower()] = 1 / S;
        }
        const double stopRate = number(step.text(QStringLiteral("stopratio")), 0);
        const double maskRate = number(step.text(QStringLiteral("maskratio")), 0);
        if (!a_error.isEmpty()) return;
        auto rateOf = [&targets, &slower, &stopMaterial, stopRate](const Region& r) {
            const QString m = r.material.toLower();
            if (!stopMaterial.isEmpty() && (r.material.compare(stopMaterial, Qt::CaseInsensitive) == 0 || r.name == stopMaterial)) return stopRate;
            if (auto it = slower.find(m); it != slower.end()) return it->second;
            if (targets.isEmpty()) return 1.0;
            for (const QString& t : targets)
                if (t.compare(r.material, Qt::CaseInsensitive) == 0 || t == r.name) return 1.0;
            return 0.0;
        };
        Paths all = solids();
        const cInt top = topOf(all);
        // The mask: its resist, where it is closed, for the etch alone.
        const double resistThickness = step.text(QStringLiteral("resist")).trimmed().isEmpty() ? 1e-6 / a_unitScale : length(step.text(QStringLiteral("resist")));
        if (!a_error.isEmpty()) return;
        Paths mask = masked ? resist(open, resistThickness, all) : Paths();
        const cInt ceiling = std::max(top, topOf(mask)) + toInt(a_grid) * 4;
        Paths front = clip(ClipperLib::ctDifference, window(ceiling), unite([&] {
                                Paths both = all;
                                both.insert(both.end(), mask.begin(), mask.end());
                                return both;
                            }()));
        // Tapered and a crystal plane's: within the slopes from the mask's
        // edges (none: the window's).
        double slope = 0;   // the sidewall's angle from the horizontal
        if (mode == QLatin1String("tapered")) slope = angle(step.text(QStringLiteral("angle")));
        else if (mode == QLatin1String("crystal")) {
            // {111} planes: 54.74° from a (100) surface, upright on (110).
            const QString o = a_orientation.isEmpty() ? QStringLiteral("100") : a_orientation;
            if (o == QLatin1String("100")) slope = std::atan(std::sqrt(2.0));
            else if (o == QLatin1String("110")) slope = M_PI / 2;
            else return fail(tr("a (111) surface is a {111} plane itself: a crystal-plane etch stops on it"));
            // (Another plane's angle, given - a III-V mesa's: not tapered's default.)
            if (step.props.contains(QStringLiteral("angle")) && !step.text(QStringLiteral("angle")).trimmed().isEmpty())
                slope = angle(step.text(QStringLiteral("angle")));
        }
        if (!a_error.isEmpty()) return;
        Paths within;
        const bool sloped = (mode == QLatin1String("tapered") || mode == QLatin1String("crystal")) && std::abs(slope - M_PI / 2) > 1e-9;
        const cInt fullDepth = top - a_bottom + toInt(a_grid);
        if (sloped) {
            if (!(slope > 0 && slope < M_PI)) return fail(tr("its sidewalls' angle is between 0° and 180° from the surface"));
            const double run = 1 / std::tan(slope);   // inward per unit down (negative: outward, a dovetail)
            std::vector<std::pair<cInt, cInt>> openings = masked ? open : std::vector<std::pair<cInt, cInt>>{{a_x0, a_x1}};
            for (const auto& [a, b] : openings) {
                // A trapezoid from the opening's surface down - a V where its
                // walls meet; above it, the opening upright.
                const cInt lift = toInt(a_grid);
                const Paths under = clip(ClipperLib::ctIntersection, all, rectangle(a, a_bottom - lift, b, top + lift));
                const cInt surface = (under.empty() ? a_bottom : topOf(under)) + lift;
                within.push_back(rectangle(a, surface, b, ceiling + lift).front());
                const double d = double(surface - a_bottom + lift);
                double ina = double(a) + run * d, inb = double(b) - run * d;
                Path p;
                if (run > 0 && ina >= inb) {
                    const double meet = (double(b) - double(a)) / (2 * run);
                    p = {{a, surface}, {b, surface}, {cInt((double(a) + double(b)) / 2), cInt(double(surface) - meet)}};
                } else {
                    p = {{a, surface}, {b, surface}, {cInt(inb), cInt(double(surface) - d)}, {cInt(ina), cInt(double(surface) - d)}};
                }
                if (!ClipperLib::Orientation(p)) std::reverse(p.begin(), p.end());
                within.push_back(p);
            }
            within = unite(within);
        }
        // The front's steps: no more than a fifth of the depth, nor finer
        // than four grid steps.
        std::map<double, std::vector<Region*>> byRate;
        for (Region& r : a_regions)
            if (const double rate = rateOf(r); rate > 0) byRate[rate].push_back(&r);
        if (byRate.empty() && maskRate <= 0) return fail(tr("it etches nothing here: none of its materials is in the structure"));
        const double total = depthText.isEmpty() ? double(fullDepth) / a_k : depth;   // geometry units
        const int n = std::clamp(int(std::ceil(total / (4 * a_grid))), 5, 400);
        const double dt = total / n;   // a rate-1 material's depth per step
        const bool toStop = depthText.isEmpty();
        Paths stops;
        for (const Region& r : a_regions)
            if (!stopMaterial.isEmpty() && (r.material.compare(stopMaterial, Qt::CaseInsensitive) == 0 || r.name == stopMaterial))
                stops.insert(stops.end(), r.p.begin(), r.p.end());
        stops = unite(stops);
        if (toStop && stops.empty()) return fail(tr("there is no %1 to stop on").arg(stopMaterial));
        // (Where the stop shows at last: each opening's.)
        std::vector<std::pair<cInt, cInt>> lanes = masked ? open : std::vector<std::pair<cInt, cInt>>{{a_x0, a_x1}};
        auto reached = [this, &front, &stops, &lanes, ceiling]() {
            const Paths touch = clip(ClipperLib::ctIntersection, offset(front, 2.0 * double(a_step), a_arc), stops);
            for (const auto& [a, b] : lanes) {
                const Paths lane = rectangle(a, a_bottom - toInt(1), b, ceiling + toInt(1));
                // (A lane with no stop below it ends nothing.)
                if (area(clip(ClipperLib::ctIntersection, stops, lane)) <= 0) continue;
                if (area(clip(ClipperLib::ctIntersection, touch, lane)) <= 0) return false;
            }
            return true;
        };
        const bool isotropic = mode == QLatin1String("isotropic");
        auto advance = [this, &byRate, &front, &mask, &within, sloped, isotropic, maskRate](double amount) {
            Paths gained;
            for (auto& [rate, regions] : byRate) {
                const double by = rate * amount * a_k;
                if (by <= 0) continue;
                Paths r = isotropic ? offset(front, by, a_arc) : sweep(front, 0, -cInt(std::llround(by)));
                if (sloped) r = clip(ClipperLib::ctIntersection, r, within);
                for (Region* region : regions) {
                    const Paths taken = clip(ClipperLib::ctIntersection, region->p, r);
                    if (taken.empty()) continue;
                    // (What the front touches alone: a step longer than a
                    // stop layer or a mask is thick went through it.)
                    Paths touching;
                    for (const Paths& piece : components(taken))
                        if (area(clip(ClipperLib::ctIntersection, offset(piece, 2.0 * double(a_step), a_arc), front)) > 0)
                            touching.insert(touching.end(), piece.begin(), piece.end());
                    if (touching.empty()) continue;
                    region->p = clip(ClipperLib::ctDifference, region->p, touching);
                    gained.insert(gained.end(), touching.begin(), touching.end());
                }
            }
            if (maskRate > 0 && !mask.empty()) {
                const Paths r = offset(front, maskRate * amount * a_k, a_arc);
                const Paths taken = clip(ClipperLib::ctIntersection, mask, r);
                mask = clip(ClipperLib::ctDifference, mask, taken);
                gained.insert(gained.end(), taken.begin(), taken.end());
            }
            if (!gained.empty()) {
                gained.insert(gained.end(), front.begin(), front.end());
                front = unite(gained);
            }
        };
        double spent = 0;
        if (toStop) {
            const int most = std::max(n * 4, 20);
            int k = 0;
            for (; k < most && !reached(); ++k) {
                advance(dt);
                spent += dt;
            }
            if (k == most && !reached()) warn(tr("the stop, %1, was not reached in every opening").arg(stopMaterial));
        } else {
            for (int k = 0; k < n; ++k) advance(dt);
            spent = total;
        }
        if (over > 0) {
            const int m = std::max(1, int(std::ceil(n * over)));
            for (int k = 0; k < m; ++k) advance(spent * over / m);
        }
        say(toStop ? tr("to %1 (%2 of a material it etches at its full rate)%3").arg(stopMaterial, lengthText(spent),
                                                                                     over > 0 ? tr(", over-etched %1 %").arg(over * 100) : QString())
                   : tr("%1 deep, %2%3").arg(lengthText(depth), mode.isEmpty() ? QStringLiteral("anisotropic") : mode,
                                             masked ? tr(", through %1 openings").arg(open.size()) : QString()));
    }

    /// After a step: each region on the grid, cleaned, rid of slivers - a
    /// piece thinner than the minimum everywhere -, regions of one name and
    /// material one, the empty gone.
    void finish(const Node& step)
    {
        Q_UNUSED(step);
        int slivers = 0;
        double sliverArea = 0;
        const double half = a_minimum / 2 * a_k;
        for (Region& r : a_regions) {
            const double before = area(r.p);
            snap(r.p, a_step);
            // (Gone on the grid: thinner than a step of it.)
            if (r.p.empty() && before > 0) {
                ++slivers;
                sliverArea += before;
                continue;
            }
            if (half <= 0) continue;
            Paths kept;
            for (const Paths& piece : components(r.p)) {
                if (offset(piece, -half, a_arc).empty()) {
                    ++slivers;
                    sliverArea += area(piece);
                    continue;
                }
                kept.insert(kept.end(), piece.begin(), piece.end());
            }
            r.p = kept;
        }
        if (slivers > 0) warn(tr("%1 sliver(s) thinner than %2 dropped (%3)").arg(slivers).arg(lengthText(a_minimum), areaText(sliverArea)));
        std::vector<Region> merged;
        for (Region& r : a_regions) {
            if (r.p.empty()) continue;
            auto same = std::find_if(merged.begin(), merged.end(), [&r](const Region& m) { return m.name == r.name && m.material == r.material; });
            if (same == merged.end()) merged.push_back(r);
            else {
                same->p.insert(same->p.end(), r.p.begin(), r.p.end());
                same->p = unite(same->p);
            }
        }
        a_regions = merged;
    }

    const Node& a_flow;
    const Model& a_model;
    const ParameterScope& a_ps;
    double a_unitScale;
    ProcessRun* a_out = nullptr;
    double a_grid = 1e-3, a_k = 64e3, a_minimum = 0, a_arc = 1;
    cInt a_step = 64;
    cInt a_x0 = 0, a_x1 = 0, a_bottom = 0;
    QString a_orientation;
    std::vector<Region> a_regions;
    QString a_error, a_said;
    QStringList a_warnings, a_files;
    // The layout's cut, for masks.
    QString a_layout;
    QPointF a_cutA, a_cutB;
    double a_um = 1, a_shift = 0;
    LayoutShapes a_shapes;
};

} // namespace

ProcessRun runProcess(const Node& flow, const Model& model, const ParameterScope& parameters, double unitScale, const QString& until)
{
    return Runner(flow, model, parameters, unitScale).run(until);
}

bool processStepMakesRegion(const QString& type)
{
    return makesRegionImpl(type);
}

QStringList processObjectNames(const Node& flow)
{
    QStringList out;
    for (const Node& step : flow.children) {
        if (!step.enabled) continue;
        if (step.type == QLatin1String("process/stack")) {
            for (const QJsonValue& v : step.value(QStringLiteral("layers")).toArray()) {
                const QString name = v.toArray().at(0).toString().trimmed();
                if (!name.isEmpty() && !out.contains(name)) out << name;
            }
        } else if (processStepMakesRegion(step.type) && !out.contains(regionName(step))) {
            out << regionName(step);
        }
    }
    return out;
}

} // namespace qucs_s::fem
