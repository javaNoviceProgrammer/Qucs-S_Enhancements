/*
 * fem_lumerical_export.cpp - a multiphysics model as Lumerical scripts
 *                            (.lsf): MODE's FDE, CHARGE, an MQW stack
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The scripts use the commands and properties of Lumerical's scripting
 * reference (addrect, addpoly, addfde, setanalysis, addmaterial and
 * setmaterial; addmodelmaterial, addmaterialproperties,
 * addsemiconductorfromalloy, addsimulationregion, addchargesolver,
 * adddope, adddiffusion, addimportdope, addelectricalcontact; mqwindex)
 * and the database names its own projects carry. What a version may not
 * have is tried (try { } catch) and said.
 */
#include "fem_lumerical.h"
#include "fem_lumerical_scene.h"

#include "fem_geometry.h"
#include "fem_materials.h"
#include "fem_mesh.h"
#include "fem_model.h"
#include "fem_problem.h"
#include "fem_results.h"
#include "fem_solver.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <map>
#include <set>

namespace qucs_s::fem {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::fem::Lumerical", text);
}

constexpr double C0 = 299792458.0, Q = 1.602176634e-19, H = 6.62607015e-34;

/// A number as a script's literal (enough digits, no locale).
QString num(double v)
{
    if (!std::isfinite(v)) return QStringLiteral("0");
    if (v == 0) return QStringLiteral("0");
    return QString::number(v, 'g', 12);
}

/// A string as a script's literal.
QString str(QString s)
{
    s.replace(QLatin1Char('"'), QLatin1Char('\''));
    return QLatin1Char('"') + s + QLatin1Char('"');
}

/// A name an object or a material may have: none of a scope's separator
/// (::), a quote, a backslash or a control character.
QString objectName(const QString& s)
{
    QString out;
    for (const QChar c : s) out += c.isPrint() && !QStringLiteral(":\"'\\").contains(c) ? c : QLatin1Char('_');
    out = out.trimmed();
    return out.isEmpty() ? QStringLiteral("object") : out;
}

/// The box about points.
QRectF boundsOf(const std::vector<QPointF>& pts)
{
    double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    for (const QPointF& p : pts) {
        x0 = std::min(x0, p.x());
        y0 = std::min(y0, p.y());
        x1 = std::max(x1, p.x());
        y1 = std::max(y1, p.y());
    }
    return pts.empty() ? QRectF() : QRectF(QPointF(x0, y0), QPointF(x1, y1));
}

/// A matrix's literal: [a,b;c,d].
QString matrix(const std::vector<std::vector<double>>& rows)
{
    QStringList r;
    for (const auto& row : rows) {
        QStringList c;
        for (double v : row) c << num(v);
        r << c.join(QLatin1Char(','));
    }
    return QLatin1Char('[') + r.join(QLatin1Char(';')) + QLatin1Char(']');
}

QString column(const std::vector<double>& values)
{
    std::vector<std::vector<double>> rows;
    for (double v : values) rows.push_back({v});
    return matrix(rows);
}

// --- Lumerical's databases --------------------------------------------------

/// A library material's names in Lumerical's electrical (CT) and optical
/// (EM) databases (as its projects carry them; empty: none).
struct Names {
    const char* ours;
    const char* ct;
    const char* em;
};
const Names Library[] = {
    {"Silicon", "Si (Silicon)", "Si (Silicon) - Palik"},
    {"Germanium", "Ge (Germanium)", "Ge (Germanium) - Palik"},
    {"Germanium on silicon", "Ge (Germanium)", "Ge (Germanium) - Palik"},
    {"Gallium arsenide", "GaAs (Gallium Arsenide)", "GaAs - Palik"},
    {"Indium phosphide", "InP (Indium Phosphide)", "InP - Palik"},
    {"Gallium nitride", "GaN (Gallium Nitride)", ""},
    {"Silicon dioxide", "SiO2 (Glass) - Sze", "SiO2 (Glass) - Palik"},
    {"Fused silica", "SiO2 (Glass) - Sze", "SiO2 (Glass) - Palik"},
    {"Silicon nitride", "Si3N4 (Silicon nitride) - Sze", "Si3N4 (Silicon Nitride) - Phillip"},
    {"Lithium niobate", "LiNbO3 insulator - Z cut (Lithium Niobate)", ""},
    {"Alumina (96%)", "Al2O3 (Aluminium oxide) - Robertson", "Al2O3 - Palik"},
    {"Aluminium", "Al (Aluminium) - CRC", "Al (Aluminium) - CRC"},
    {"Copper", "Cu (Copper) - CRC", "Cu (Copper) - CRC"},
    {"Gold", "Au (Gold) - CRC", "Au (Gold) - CRC"},
    {"Silver", "Ag (Silver) - CRC", "Ag (Silver) - CRC"},
    {"Air", "Air", "etch"},
    {"Water", "", "H2O (Water) - Palik"},
};
const Names* libraryNames(const QString& name)
{
    for (const Names& n : Library)
        if (name.compare(QLatin1String(n.ours), Qt::CaseInsensitive) == 0) return &n;
    return nullptr;
}

/// A compound's (fem_materials') name in Lumerical's electrical database
/// and the composition there: a binary, or an alloy at x (and y).
struct Alloy {
    QString name;
    double x = 0, y = 0;
    int fractions = 0;   ///< 0 a binary, 1 a ternary, 2 a quaternary
    QString note;
};
std::optional<Alloy> lumericalCompound(const QString& compound, double x, double y)
{
    static const std::map<QString, const char*> binaries = {
        {QStringLiteral("GaAs"), "GaAs (Gallium Arsenide)"}, {QStringLiteral("AlAs"), "AlAs (Aluminium Arsenide)"},
        {QStringLiteral("InAs"), "InAs (Indium Arsenide)"},  {QStringLiteral("InP"), "InP (Indium Phosphide)"},
        {QStringLiteral("GaP"), "GaP (Gallium Phosphide)"},  {QStringLiteral("GaSb"), "GaSb (Gallium Antimonide)"},
        {QStringLiteral("AlSb"), "AlSb (Aluminium Antimonide)"}, {QStringLiteral("InSb"), "InSb (Indium Antimonide)"},
        {QStringLiteral("GaN"), "GaN (Gallium Nitride)"},    {QStringLiteral("AlN"), "AlN (Aluminium Nitride)"},
        {QStringLiteral("InN"), "InN (Indium Nitride)"}};
    if (const auto it = binaries.find(compound); it != binaries.end()) return Alloy{QString::fromLatin1(it->second), 0, 0, 0, {}};
    // (Ternaries in Lumerical's naming: its first element's fraction x.)
    static const std::map<QString, std::pair<const char*, bool>> ternaries = {
        {QStringLiteral("AlGaAs"), {"AlGaAs (Aluminium Gallium Arsenide)", false}},
        {QStringLiteral("InGaAs"), {"InGaAs (Indium Gallium Arsenide)", false}},
        {QStringLiteral("InAlAs"), {"InAlAs (Indium Aluminium Arsenide)", false}},
        {QStringLiteral("GaInP"), {"GaInP (Gallium Indium Phosphide)", false}},
        {QStringLiteral("GaAsP"), {"GaAsP (Gallium Arsenide Phosphide)", false}},
        {QStringLiteral("InAsP"), {"InAsP (Indium Arsenide Phosphide)", false}},
        {QStringLiteral("GaAsSb"), {"GaAsSb (Gallium Arsenide Antimonide)", false}},
        {QStringLiteral("AlGaN"), {"AlGaN (Aluminium Gallium Nitride)", false}},
        {QStringLiteral("InGaN"), {"InGaN (Indium Gallium Nitride)", false}},
        // (Ours In x Al 1-x N; Lumerical's Al x In 1-x N.)
        {QStringLiteral("InAlN"), {"AlInN (Aluminium Indium Nitride)", true}}};
    if (const auto it = ternaries.find(compound); it != ternaries.end())
        return Alloy{QString::fromLatin1(it->second.first), it->second.second ? 1 - x : x, 0, 1, {}};
    if (compound == QLatin1String("AlGaInAs")) return Alloy{QStringLiteral("AlGaInAs"), x, y, 2, {}};
    if (compound == QLatin1String("InGaAsP"))
        return Alloy{QStringLiteral("InGaAsP"), x, y, 2,
                     tr("InGaAsP: ours In(1-x) Ga(x) As(y) P(1-y) - x and y passed as they are; check Lumerical's convention for it")};
    return std::nullopt;
}

// --- The cross-section ------------------------------------------------------

/// A domain's outline (the geometry's unit, counter-clockwise), a polygon
/// for each of its outer loops; its holes left to the domains in them.
struct Outline {
    int domain = -1;
    std::vector<std::vector<QPointF>> loops;
    int depth = 0;           ///< how many domains' outlines hold it
    bool rectangle = false;  ///< one loop, an axis-aligned rectangle
    QRectF bounds;
};

double signedArea(const std::vector<QPointF>& p)
{
    double a = 0;
    for (std::size_t i = 0; i < p.size(); ++i) {
        const QPointF& u = p[i];
        const QPointF& v = p[(i + 1) % p.size()];
        a += u.x() * v.y() - v.x() * u.y();
    }
    return a / 2;
}

bool contains(const std::vector<QPointF>& poly, QPointF p)
{
    bool in = false;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const QPointF& a = poly[i];
        const QPointF& b = poly[j];
        if ((a.y() > p.y()) != (b.y() > p.y()) && p.x() < (b.x() - a.x()) * (p.y() - a.y()) / (b.y() - a.y()) + a.x()) in = !in;
    }
    return in;
}

/// A segment's points from v0 to v1 (an arc's at 2.5° steps, a polyline's
/// own), its ends the vertices.
std::vector<QPointF> segmentPoints(const Topology& t, const Topology::Segment& s)
{
    const QPointF a = t.vertices[std::size_t(s.v0)], b = t.vertices[std::size_t(s.v1)];
    if (s.curve < 0 || t.curves[std::size_t(s.curve)].kind == Curve::Line) return {a, b};
    const Curve& c = t.curves[std::size_t(s.curve)];
    std::vector<QPointF> out;
    if (c.kind == Curve::Arc) {
        const int n = std::max(2, int(std::ceil(std::abs(s.t1 - s.t0) / (M_PI / 72))));
        for (int i = 0; i <= n; ++i) out.push_back(c.at(s.t0 + (s.t1 - s.t0) * i / n));
    } else {
        out.push_back(c.at(s.t0));
        const double dir = s.t1 >= s.t0 ? 1 : -1;
        for (double tt = dir > 0 ? std::floor(s.t0) + 1 : std::ceil(s.t0) - 1; dir > 0 ? tt < s.t1 - 1e-9 : tt > s.t1 + 1e-9; tt += dir) out.push_back(c.at(tt));
        out.push_back(c.at(s.t1));
    }
    out.front() = a;
    out.back() = b;
    return out;
}

/// Each domain's outline, by domain; their depths (a domain in another's
/// hole one more than it).
std::vector<Outline> outlines(const Topology& t)
{
    const int nd = int(t.domains.size());
    std::vector<Outline> out(static_cast<std::size_t>(nd));
    for (int d = 0; d < nd; ++d) {
        // Its segments, each walked with d on its left.
        struct Piece {
            int from, to;
            std::vector<QPointF> pts;
            bool used = false;
        };
        std::vector<Piece> pieces;
        for (const Topology::Segment& s : t.segments) {
            if (s.left != d && s.right != d) continue;
            if (s.left == d && s.right == d) continue;   // (an edge inside it: none of its outline)
            std::vector<QPointF> pts = segmentPoints(t, s);
            if (s.left == d) pieces.push_back({s.v0, s.v1, pts});
            else {
                std::reverse(pts.begin(), pts.end());
                pieces.push_back({s.v1, s.v0, pts});
            }
        }
        std::multimap<int, std::size_t> from;
        for (std::size_t k = 0; k < pieces.size(); ++k) from.insert({pieces[k].from, k});
        std::vector<std::vector<QPointF>> loops;
        for (std::size_t k = 0; k < pieces.size(); ++k) {
            if (pieces[k].used) continue;
            std::vector<QPointF> loop;
            std::size_t at = k;
            const int start = pieces[k].from;
            for (std::size_t guard = 0; guard <= pieces.size(); ++guard) {
                Piece& p = pieces[at];
                p.used = true;
                loop.insert(loop.end(), p.pts.begin(), p.pts.end() - 1);
                if (p.to == start) break;
                std::size_t next = pieces.size();
                for (auto [it, end] = from.equal_range(p.to); it != end; ++it)
                    if (!pieces[it->second].used) {
                        next = it->second;
                        break;
                    }
                if (next == pieces.size()) break;
                at = next;
            }
            if (loop.size() >= 3) loops.push_back(loop);
        }
        Outline& o = out[std::size_t(d)];
        o.domain = d;
        for (auto& loop : loops)
            if (signedArea(loop) > 0) o.loops.push_back(loop);
        o.bounds = t.domains[std::size_t(d)].bounds;
        if (o.loops.size() == 1) {
            // A rectangle: four corners left once points on its sides go.
            std::vector<QPointF> corners;
            const auto& l = o.loops.front();
            for (std::size_t i = 0; i < l.size(); ++i) {
                const QPointF& a = l[(i + l.size() - 1) % l.size()];
                const QPointF& b = l[i];
                const QPointF& c = l[(i + 1) % l.size()];
                const double cr = (b.x() - a.x()) * (c.y() - b.y()) - (b.y() - a.y()) * (c.x() - b.x());
                const double scale = std::max(1e-300, std::hypot(b.x() - a.x(), b.y() - a.y()) * std::hypot(c.x() - b.x(), c.y() - b.y()));
                if (std::abs(cr) > 1e-9 * scale) corners.push_back(b);
            }
            const double tol = 1e-9 * std::max(o.bounds.width(), o.bounds.height());
            o.rectangle = corners.size() == 4 && std::all_of(corners.begin(), corners.end(), [&](QPointF p) {
                              return (std::abs(p.x() - o.bounds.left()) < tol || std::abs(p.x() - o.bounds.right()) < tol)
                                     && (std::abs(p.y() - o.bounds.top()) < tol || std::abs(p.y() - o.bounds.bottom()) < tol);
                          });
        }
    }
    // Depths: the outlines that hold each domain's point.
    for (Outline& o : out) {
        const QPointF p = t.domains[std::size_t(o.domain)].inside;
        for (const Outline& other : out) {
            if (other.domain == o.domain) continue;
            for (const auto& loop : other.loops)
                if (contains(loop, p)) {
                    ++o.depth;
                    break;
                }
        }
    }
    return out;
}

/// The order the structures are written: the outer first (Lumerical: a
/// later structure of the same mesh order wins where they overlap).
std::vector<int> drawingOrder(const std::vector<Outline>& o, const std::vector<int>& domains)
{
    std::vector<int> order = domains;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return o[std::size_t(a)].depth < o[std::size_t(b)].depth; });
    return order;
}

/// A domain's structure's name: its object's and its number.
QString structureName(const Topology& t, int d)
{
    const QString owner = t.objectNames.value(t.domains[std::size_t(d)].owner);
    return objectName(QStringLiteral("%1 d%2").arg(owner.isEmpty() ? QStringLiteral("domain") : owner).arg(d + 1));
}

/// The script's lines, its notes.
struct Writer {
    QStringList lines;
    QStringList notes;
    void line(const QString& s = {}) { lines << s; }
    void comment(const QString& s) { lines << QStringLiteral("# ") + s; }
    void set(const QString& key, const QString& value) { lines << QStringLiteral("set(%1, %2);").arg(str(key), value); }
    void set(const QString& key, double value) { set(key, num(value)); }
    void setText(const QString& key, const QString& value) { set(key, str(value)); }
    void note(const QString& s)
    {
        if (!notes.contains(s)) notes << s;
    }
};

/// Domain \a d's structure: a rectangle or a polygon (or several), in the
/// x-y plane extruded along z (\a plane 'z'), or in the x-z plane extruded
/// along y (\a plane 'y': the polygon's y as z, turned about x).
/// Its mesh order (\a order, Lumerical's: the lower wins where they
/// overlap): none, MODE's (over the material's), the Finite Element IDE's.
enum class Order { None, Mode, Device };
void writeStructure(Writer& w, const Topology& t, const Outline& o, const QString& name, const QString& material, char plane, double extent,
                    Order meshOrder, int order, const QStringList& extra = {})
{
    const double s = t.unitScale;
    const int n = int(o.loops.size());
    for (int k = 0; k < n; ++k) {
        const QString nm = n > 1 ? QStringLiteral("%1 part%2").arg(name).arg(k + 1) : name;
        if (o.rectangle) {
            const QRectF b = o.bounds;
            w.line(QStringLiteral("addrect;"));
            w.setText(QStringLiteral("name"), nm);
            w.set(QStringLiteral("x min"), b.left() * s);
            w.set(QStringLiteral("x max"), b.right() * s);
            if (plane == 'z') {
                w.set(QStringLiteral("y min"), b.top() * s);
                w.set(QStringLiteral("y max"), b.bottom() * s);
                w.set(QStringLiteral("z"), 0.0);
                w.set(QStringLiteral("z span"), extent);
            } else {
                w.set(QStringLiteral("z min"), b.top() * s);
                w.set(QStringLiteral("z max"), b.bottom() * s);
                w.set(QStringLiteral("y"), 0.0);
                w.set(QStringLiteral("y span"), extent);
            }
        } else {
            std::vector<std::vector<double>> v;
            for (const QPointF& p : o.loops[std::size_t(k)]) v.push_back({p.x() * s, p.y() * s});
            w.line(QStringLiteral("addpoly;"));
            w.setText(QStringLiteral("name"), nm);
            w.set(QStringLiteral("x"), 0.0);
            w.set(QStringLiteral("y"), 0.0);
            w.set(QStringLiteral("z"), 0.0);
            if (plane == 'y') {
                // (Turned about x by 90°: its y the global z, its z span along y.)
                w.setText(QStringLiteral("first axis"), QStringLiteral("x"));
                w.set(QStringLiteral("rotation 1"), 90.0);
            }
            w.set(QStringLiteral("vertices"), matrix(v));
            w.set(QStringLiteral("z span"), extent);
        }
        w.setText(QStringLiteral("material"), material);
        for (const QString& l : extra) w.line(l);
        if (meshOrder == Order::Mode) w.set(QStringLiteral("override mesh order from material database"), 1.0);
        if (meshOrder != Order::None) w.set(QStringLiteral("mesh order"), double(order));
    }
}

/// The value of a material's property \a key at a point of domain \a d:
/// its text evaluated as the solution has it (lengths, the wavelength).
std::optional<double> valueIn(const Solution& s, const Locator& locator, const QString& text, QPointF p)
{
    if (text.trimmed().isEmpty()) return std::nullopt;
    const std::optional<double> v = evaluateAt(s, locator, text, p);
    if (!v || !std::isfinite(*v)) return std::nullopt;
    return v;
}

/// A node's physics tag: its "physics" property (a list's first), else
/// the first of \a type.
const Node* physicsOf(const Model& m, const Node& node, const QString& type)
{
    const QJsonValue v = node.value(QStringLiteral("physics"));
    QString tag = v.isArray() ? v.toArray().at(0).toString() : v.toString();
    for (const Node* p : m.physics())
        if ((tag.isEmpty() || p->tag == tag) && p->type == type && p->enabled) return p;
    for (const Node* p : m.physics())
        if (p->type == type && p->enabled) return p;
    return nullptr;
}

/// The export's study (its "study", else the first that has a step of
/// \a stepType), and that step.
std::pair<const Node*, const Node*> studyStep(const Model& m, const Node& node, const QStringList& stepTypes)
{
    const QString tag = node.text(QStringLiteral("study"));
    for (const Node* st : m.studies()) {
        if (!tag.isEmpty() && st->tag != tag) continue;
        for (const Node& step : st->children)
            if (step.enabled && stepTypes.contains(step.type)) return {st, &step};
        if (!tag.isEmpty()) return {st, nullptr};
    }
    return {nullptr, nullptr};
}

QString header(const QString& product, const Model& model, const QString& base, const QString& version, const QString& saved,
               const QString& does, const QString& where)
{
    const QString from = model.fileName.isEmpty() ? tr("an unsaved model") : QFileInfo(model.fileName).fileName();
    return tr("# %1 - a Lumerical %2 script, written by Qucs-S %3 from %4 on %5.\n"
              "# Run it in %6 (Script File Editor > Run, or the script prompt: %7): it %8%9.\n"
              "# Lengths in metres, every value SI.\n")
        .arg(base + QStringLiteral(".lsf"), product, version, from, QDateTime::currentDateTime().toString(Qt::ISODate), where,
             QStringLiteral("feval(\"%1.lsf\");").arg(base), does, saved.isEmpty() ? QString() : tr(" and saves it as %1").arg(saved));
}

/// Lumerical's cross-sections are planar: an axisymmetric model's is not one.
QString planarOnly(const Model& model)
{
    return model.axisymmetric() ? tr("the model is axisymmetric: Lumerical's 2D regions here are planar cross-sections") : QString();
}

/// The script: its header, its notes (what was taken, and how), its body.
QString compose(const QString& head, const Writer& w)
{
    QString text = head;
    if (!w.notes.isEmpty()) {
        text += tr("#\n# Taken from the model:\n");
        for (const QString& n : w.notes) text += QStringLiteral("#  - ") + n + QLatin1Char('\n');
    }
    return text + QLatin1Char('\n') + w.lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

/// Each domain's material: the last whose selection has it (null: none).
std::vector<const Node*> domainMaterials(const Model& m, const Topology& t)
{
    std::vector<const Node*> out(t.domains.size(), nullptr);
    for (const Node& mat : m.materials().children) {
        if (!mat.enabled) continue;
        QString why;
        for (int d : resolveSelection(t, m, Level::Domain, mat.selection(), &why))
            if (d >= 0 && d < int(out.size())) out[std::size_t(d)] = &mat;
    }
    return out;
}

QString materialName(const Node* mat)
{
    if (!mat) return QString();
    const QString lib = mat->text(QStringLiteral("library"));
    return mat->label.isEmpty() && !lib.isEmpty() ? lib : mat->name();
}

/// The model and what it needs built: its parameters, geometry, mesh.
struct Built {
    std::shared_ptr<ParameterScope> parameters;
    std::shared_ptr<const Topology> topology;
    std::shared_ptr<const Mesh> mesh;
    QString error;
};
Built build(const Model& model, bool meshed)
{
    Built b;
    b.parameters = evaluateParameters(model);
    if (!b.parameters->errors.isEmpty()) {
        QStringList list;
        for (auto it = b.parameters->errors.cbegin(); it != b.parameters->errors.cend(); ++it) list << tr("%1: %2").arg(it.key(), it.value());
        b.error = tr("its parameters: %1").arg(list.join(QStringLiteral("; ")));
        return b;
    }
    const GeometryBuild g = buildGeometry(model, *b.parameters);
    if (!g.ok()) {
        b.error = tr("its geometry: %1").arg(QStringList(g.errors.values()).join(QStringLiteral("; ")));
        return b;
    }
    b.topology = g.topology;
    if (meshed) {
        const MeshBuild m = buildMesh(model, *b.parameters, g.topology);
        if (!m.mesh) {
            b.error = tr("its mesh: %1").arg(m.error);
            return b;
        }
        b.mesh = m.mesh;
    }
    return b;
}

// --- MODE -------------------------------------------------------------------

LumericalScript exportMode(const Model& model, const Node& node, const QString& base, const QString& version)
{
    LumericalScript out;
    Writer w;
    const Node* phys = physicsOf(model, node, QStringLiteral("modes"));
    if (!phys) {
        out.error = tr("the model has no Electromagnetic Waves, Mode Analysis physics");
        return out;
    }
    if (const QString why = planarOnly(model); !why.isEmpty()) {
        out.error = why;
        return out;
    }
    const Built b = build(model, true);
    if (!b.error.isEmpty()) {
        out.error = b.error;
        return out;
    }
    const Topology& t = *b.topology;
    const Scope& scope = b.parameters->scope;
    QString why;
    // The mode step: its (first) wavelength, its modes, the index about.
    double lambda = 1.55e-6;
    std::vector<double> sweepLambda;
    int trial = 4;
    QString neffText;
    const std::pair<const Node*, const Node*> studyAndStep = studyStep(model, node, {QStringLiteral("mode")});
    const Node* const step = studyAndStep.second;
    if (step) {
        Dim dim;
        const std::optional<std::vector<double>> v = valueList(step->text(QStringLiteral("frequency")), scope, &dim, &why);
        if (!v || v->empty() || !((*v)[0] > 0)) {
            out.error = tr("%1: its frequency: %2").arg(step->name(), why);
            return out;
        }
        lambda = dim == Dim::length() ? (*v)[0] : C0 / (*v)[0];
        for (double x : *v) sweepLambda.push_back(dim == Dim::length() ? x : C0 / x);
        if (v->size() > 1) w.note(tr("%1 sweeps %2 wavelengths: its first, %3 nm, taken (Lumerical's frequency sweep goes on from it)")
                                      .arg(step->name()).arg(v->size()).arg(num(lambda * 1e9)));
        trial = std::max(1, step->integer(QStringLiteral("modes")));
        neffText = step->text(QStringLiteral("neff")).trimmed();
    } else {
        w.note(tr("no study with a mode analysis: 1550 nm, 4 modes"));
    }
    if (node.integer(QStringLiteral("modes")) > 0) trial = node.integer(QStringLiteral("modes"));
    double neff = NAN;
    if (!neffText.isEmpty()) neff = evaluateConstant(neffText, scope, Dim()).value_or(NAN);
    // Its domains, its PMLs, each domain's wave feature, its PMC boundaries.
    const QVector<int> on = resolveSelection(t, model, Level::Domain, phys->selection(), &why);
    const std::size_t nd = t.domains.size();
    std::vector<char> pml(nd, 0);
    std::vector<const Node*> waveOf(nd, nullptr);
    std::set<int> pmc;
    for (const Node& f : phys->children) {
        if (!f.enabled) continue;
        if (f.type == QLatin1String("modes/wave") || f.type == QLatin1String("modes/pml")) {
            for (int d : resolveSelection(t, model, Level::Domain, f.selection(), &why)) {
                if (d < 0 || d >= int(nd)) continue;
                if (f.type == QLatin1String("modes/pml")) pml[std::size_t(d)] = 1;
                else waveOf[std::size_t(d)] = &f;
            }
        } else if (f.type == QLatin1String("modes/pmc")) {
            for (int bd : resolveSelection(t, model, Level::Boundary, f.selection(), &why)) pmc.insert(bd);
        }
    }
    std::vector<int> inner;
    QRectF region, all;
    for (int d : on) {
        all = all.isNull() ? t.domains[std::size_t(d)].bounds : all.united(t.domains[std::size_t(d)].bounds);
        if (pml[std::size_t(d)]) continue;
        inner.push_back(d);
        region = region.isNull() ? t.domains[std::size_t(d)].bounds : region.united(t.domains[std::size_t(d)].bounds);
    }
    if (inner.empty()) {
        out.error = tr("%1 is on no domain (but PMLs)").arg(phys->name());
        return out;
    }
    // Each domain's index at the wavelength, as the model has it there.
    Solution s(model, b.parameters, b.mesh);
    s.setFrequency(C0 / lambda);
    const Locator locator(*b.mesh);
    const std::vector<const Node*> mats = domainMaterials(model, t);
    struct Index {
        double n[3] = {NAN, NAN, NAN}, k[3] = {0, 0, 0};
        bool anisotropic = false;
        QString name, library;
    };
    std::vector<Index> index(nd);
    const bool library = node.text(QStringLiteral("materials")) == QLatin1String("library");
    double nMax = 1;
    for (int d : inner) {
        Index& ix = index[std::size_t(d)];
        const Node* feat = waveOf[std::size_t(d)];
        const Node* mat = mats[std::size_t(d)];
        const bool user = feat && feat->text(QStringLiteral("source")) == QLatin1String("user");
        const QString object = t.objectNames.value(t.domains[std::size_t(d)].owner);
        if (!user && !mat) {
            out.error = tr("domain %1 (%2) has no material: its refractive index is needed").arg(d + 1).arg(object);
            return out;
        }
        auto text = [&](const QString& key) { return user ? feat->text(key) : detail::Problem::materialProperty(*mat, key, &scope); };
        const QPointF p = t.domains[std::size_t(d)].inside;
        const bool byIndex = !feat || feat->text(QStringLiteral("constitutive")) != QLatin1String("epsilonr");
        ix.anisotropic = user && feat->text(QStringLiteral("anisotropy")) == QLatin1String("diagonal");
        ix.name = user ? feat->name() : materialName(mat);
        if (!user && mat) {
            const Names* names = libraryNames(mat->text(QStringLiteral("library")).isEmpty() ? mat->name() : mat->text(QStringLiteral("library")));
            if (names && *names->em) ix.library = QString::fromLatin1(names->em);
        }
        if (byIndex) {
            if (ix.anisotropic) {
                const char* keys[3] = {"nx", "ny", "nz"};
                for (int c = 0; c < 3; ++c) ix.n[c] = valueIn(s, locator, text(QString::fromLatin1(keys[c])), p).value_or(NAN);
            } else {
                ix.n[0] = ix.n[1] = ix.n[2] = valueIn(s, locator, text(QStringLiteral("n")), p).value_or(NAN);
            }
            const double kappa = valueIn(s, locator, text(QStringLiteral("kappa")), p).value_or(0);
            for (double& k : ix.k) k = kappa;
        } else {
            // (By εr, tan δ and σ: n + jκ = √(εr (1 - j tan δ) - j σ/(ω ε0)).)
            const char* keys[3] = {"epsxx", "epsyy", "epszz"};
            const double omega = 2 * M_PI * C0 / lambda, eps0 = 8.8541878188e-12;
            const double tand = valueIn(s, locator, text(QStringLiteral("tand")), p).value_or(0);
            const double sigma = valueIn(s, locator, text(QStringLiteral("sigma")), p).value_or(0);
            for (int c = 0; c < 3; ++c) {
                const double er = valueIn(s, locator, ix.anisotropic ? text(QString::fromLatin1(keys[c])) : text(QStringLiteral("epsilonr")), p).value_or(NAN);
                const std::complex<double> e = std::sqrt(std::complex<double>(er, -er * tand - sigma / (omega * eps0)));
                ix.n[c] = e.real();
                ix.k[c] = -e.imag();
            }
        }
        if (!std::isfinite(ix.n[0]) || !std::isfinite(ix.n[1]) || !std::isfinite(ix.n[2])) {
            out.error = tr("domain %1 (%2): its refractive index at %3 nm could not be had (%4)").arg(d + 1).arg(object, num(lambda * 1e9), ix.name);
            return out;
        }
        nMax = std::max({nMax, ix.n[0], ix.n[1], ix.n[2]});
        if (feat && (feat->flag(QStringLiteral("plasma")) || feat->flag(QStringLiteral("electroabsorption")) || feat->flag(QStringLiteral("thermooptic"))))
            w.note(tr("domain %1 (%2): its index the material's alone - the free carriers', a direct gap's absorption, the heat's not in it")
                       .arg(d + 1).arg(object));
    }
    // The materials: one (n,k) material for each index (or Lumerical's own).
    std::vector<QString> materialOf(nd);
    std::map<QString, const Index*> made;
    for (int d : inner) {
        const Index& ix = index[std::size_t(d)];
        if (library && !ix.library.isEmpty()) {
            materialOf[std::size_t(d)] = ix.library;
            continue;
        }
        if (library) w.note(tr("%1: no material of Lumerical's for it - its index as an (n,k) material").arg(ix.name));
        QString nm = objectName(QStringLiteral("%1 n=%2").arg(ix.name, QString::number(ix.n[0], 'g', 6)));
        if (ix.anisotropic) nm = objectName(QStringLiteral("%1 n=%2,%3,%4").arg(ix.name, QString::number(ix.n[0], 'g', 5), QString::number(ix.n[1], 'g', 5), QString::number(ix.n[2], 'g', 5)));
        if (ix.k[0] != 0) nm += QStringLiteral(" k=") + QString::number(ix.k[0], 'g', 4);
        made.insert({nm, &ix});
        materialOf[std::size_t(d)] = nm;
    }
    const QString saved = base + QStringLiteral(".lms");
    w.line(QStringLiteral("newproject;"));
    w.line();
    if (!made.empty()) {
        w.comment(tr("The materials: each domain's index at %1 nm as the model has it.").arg(num(lambda * 1e9)));
        for (const auto& [nm, ix] : made) {
            w.line(QStringLiteral("temp = addmaterial(\"(n,k) Material\");"));
            w.line(QStringLiteral("setmaterial(temp, \"name\", %1);").arg(str(nm)));
            if (ix->anisotropic) {
                w.line(QStringLiteral("setmaterial(%1, \"Anisotropy\", 1);").arg(str(nm)));
                w.line(QStringLiteral("setmaterial(%1, \"Refractive Index\", [%2, %3, %4]);").arg(str(nm), num(ix->n[0]), num(ix->n[1]), num(ix->n[2])));
                w.line(QStringLiteral("setmaterial(%1, \"Imaginary Refractive Index\", [%2, %3, %4]);").arg(str(nm), num(ix->k[0]), num(ix->k[1]), num(ix->k[2])));
            } else {
                w.line(QStringLiteral("setmaterial(%1, \"Refractive Index\", %2);").arg(str(nm), num(ix->n[0])));
                w.line(QStringLiteral("setmaterial(%1, \"Imaginary Refractive Index\", %2);").arg(str(nm), num(ix->k[0])));
            }
        }
        w.line();
    }
    // The cross-section.
    const std::vector<Outline> o = outlines(t);
    const double size = std::max(region.width(), region.height()) * t.unitScale;
    w.comment(tr("The cross-section in the x-y plane, each domain a structure (the outer first: where they overlap, the inner's mesh order wins; "
                 "each its own, none left to the objects' order)."));
    const std::vector<int> modeOrder = drawingOrder(o, inner);
    for (std::size_t i = 0; i < modeOrder.size(); ++i) {
        const int d = modeOrder[i];
        w.comment(tr("domain %1: %2").arg(d + 1).arg(index[std::size_t(d)].name));
        writeStructure(w, t, o[std::size_t(d)], structureName(t, d), materialOf[std::size_t(d)], 'z', 10 * size, Order::Mode, int(modeOrder.size() - i) + 1);
    }
    w.line();
    // The FDE region, its boundaries, mesh and analysis.
    const double sc = t.unitScale;
    double meshStep = NAN;
    if (!node.text(QStringLiteral("step")).trimmed().isEmpty()) meshStep = evaluateConstant(node.text(QStringLiteral("step")), scope, Dim::length()).value_or(NAN);
    if (!(meshStep > 0)) meshStep = std::min(lambda / (20 * nMax), std::min(region.width(), region.height()) * sc / 40);
    const double tol = 1e-6 * std::max(region.width(), region.height());
    auto side = [&on, &pml, &t, &region, &tol, &pmc](int which) {
        // PML: a PML domain beyond that side; PMC: a PMC boundary on it.
        for (int d : on) {
            if (!pml[std::size_t(d)]) continue;
            const QRectF bd = t.domains[std::size_t(d)].bounds;
            if ((which == 0 && bd.left() < region.left() - tol) || (which == 1 && bd.right() > region.right() + tol)
                || (which == 2 && bd.top() < region.top() - tol) || (which == 3 && bd.bottom() > region.bottom() + tol))
                return QStringLiteral("PML");
        }
        for (int bd : pmc) {
            const std::vector<QPointF> pts = t.boundaries[std::size_t(bd)].polyline(t);
            const bool there = !pts.empty() && std::all_of(pts.begin(), pts.end(), [&](QPointF q) {
                return which == 0 ? std::abs(q.x() - region.left()) < tol : which == 1 ? std::abs(q.x() - region.right()) < tol
                     : which == 2 ? std::abs(q.y() - region.top()) < tol : std::abs(q.y() - region.bottom()) < tol;
            });
            if (there) return QStringLiteral("PMC");
        }
        return QStringLiteral("Metal");
    };
    const QStringList bcs = {side(0), side(1), side(2), side(3)};
    w.comment(tr("The FDE region over %1's domains%2.").arg(phys->name(), all != region ? tr(" (its PML domains Lumerical's PML on those sides)") : QString()));
    w.line(QStringLiteral("addfde;"));
    w.setText(QStringLiteral("solver type"), QStringLiteral("2D Z normal"));
    w.set(QStringLiteral("x"), region.center().x() * sc);
    w.set(QStringLiteral("x span"), region.width() * sc);
    w.set(QStringLiteral("y"), region.center().y() * sc);
    w.set(QStringLiteral("y span"), region.height() * sc);
    w.set(QStringLiteral("z"), 0.0);
    w.setText(QStringLiteral("x min bc"), bcs[0]);
    w.setText(QStringLiteral("x max bc"), bcs[1]);
    w.setText(QStringLiteral("y min bc"), bcs[2]);
    w.setText(QStringLiteral("y max bc"), bcs[3]);
    w.setText(QStringLiteral("define x mesh by"), QStringLiteral("maximum mesh step"));
    w.set(QStringLiteral("dx"), meshStep);
    w.setText(QStringLiteral("define y mesh by"), QStringLiteral("maximum mesh step"));
    w.set(QStringLiteral("dy"), meshStep);
    w.line();
    w.comment(tr("The modes: %1 at %2 nm%3.").arg(trial).arg(num(lambda * 1e9), std::isfinite(neff) ? tr(", about n = %1").arg(num(neff)) : tr(", about the largest index")));
    w.line(QStringLiteral("setanalysis(\"wavelength\", %1);").arg(num(lambda)));
    w.line(QStringLiteral("setanalysis(\"number of trial modes\", %1);").arg(trial));
    if (std::isfinite(neff)) {
        w.line(QStringLiteral("setanalysis(\"search\", \"near n\");"));
        w.line(QStringLiteral("setanalysis(\"use max index\", 0);"));
        w.line(QStringLiteral("setanalysis(\"n\", %1);").arg(num(neff)));
    }
    w.line(QStringLiteral("save(%1);").arg(str(saved)));
    if (node.flag(QStringLiteral("run"))) {
        w.line(QStringLiteral("n = findmodes;"));
        if (sweepLambda.size() > 1) {
            // (Lumerical's sweep: from the analysis' wavelength to its stop,
            // evenly; its first mode followed.)
            w.comment(tr("The mode step's wavelengths: the first mode followed to %1 nm, %2 points (Lumerical's spacing).").arg(num(sweepLambda.back() * 1e9)).arg(sweepLambda.size()));
            w.line(QStringLiteral("try {"));
            w.line(QStringLiteral("    selectmode(1);"));
            w.line(QStringLiteral("    setanalysis(\"track selected mode\", 1);"));
            w.line(QStringLiteral("    setanalysis(\"stop wavelength\", %1);").arg(num(sweepLambda.back())));
            w.line(QStringLiteral("    setanalysis(\"number of points\", %1);").arg(sweepLambda.size()));
            w.line(QStringLiteral("    frequencysweep;"));
            w.line(QStringLiteral("} catch(err);"));
        }
        w.line(QStringLiteral("for (i = 1:n) { ?\"mode\" + num2str(i) + \": neff \" + num2str(real(getdata(\"FDE::data::mode\" + num2str(i), \"neff\"))) + \", loss \" + num2str(getdata(\"FDE::data::mode\" + num2str(i), \"loss\")) + \" dB/m\"; }"));
    }
    if (bcs[0] == QLatin1String("PML") || bcs[1] == QLatin1String("PML") || bcs[2] == QLatin1String("PML") || bcs[3] == QLatin1String("PML"))
        w.note(tr("its PML domains left out: Lumerical's PML at the FDE region's sides beyond which they were"));
    out.text = compose(header(QStringLiteral("MODE"), model, base, version, saved, tr("builds the model in a new project"), tr("Lumerical MODE")), w);
    out.notes = w.notes;
    out.summary = tr("a MODE script: %1 structures, an FDE region %2 x %3 µm, %4 modes at %5 nm").arg(inner.size()).arg(num(region.width() * sc * 1e6), num(region.height() * sc * 1e6)).arg(trial).arg(num(lambda * 1e9));
    return out;
}

// --- CHARGE -----------------------------------------------------------------

/// A material's model material in the Finite Element IDE: Lumerical's
/// electrical material of its name (a binary, an alloy at its
/// composition), or one guessed by its kind (said).
struct ChargeMaterial {
    QString name;          ///< the model material's (ours)
    QString database;      ///< Lumerical's electrical database's
    int fractions = 0;     ///< an alloy's: addsemiconductorfromalloy
    double x = 0, y = 0;
};

ChargeMaterial chargeMaterial(const Node& mat, const Scope& scope, Writer& w)
{
    ChargeMaterial cm;
    cm.name = objectName(materialName(&mat));
    const QString compound = detail::Problem::materialProperty(mat, QStringLiteral("compound"));
    if (!compound.trimmed().isEmpty()) {
        const double x = evaluateConstant(detail::Problem::materialProperty(mat, QStringLiteral("x")), scope, Dim()).value_or(0);
        const double y = evaluateConstant(detail::Problem::materialProperty(mat, QStringLiteral("y")), scope, Dim()).value_or(0);
        if (const std::optional<Alloy> a = lumericalCompound(compound.trimmed(), x, y)) {
            cm.database = a->name;
            cm.fractions = a->fractions;
            cm.x = a->x;
            cm.y = a->y;
            if (!a->note.isEmpty()) w.note(a->note);
            return cm;
        }
    }
    const QString lib = mat.text(QStringLiteral("library")).isEmpty() ? mat.name() : mat.text(QStringLiteral("library"));
    if (const Names* n = libraryNames(lib); n && *n->ct) {
        cm.database = QString::fromLatin1(n->ct);
        if (lib.compare(QLatin1String("Germanium on silicon"), Qt::CaseInsensitive) == 0)
            w.note(tr("%1: Lumerical's Ge (Germanium), unstrained - the tensile strain's narrowed gaps are not in it").arg(cm.name));
        return cm;
    }
    // A material of its own: by what it has.
    const bool semiconductor = !detail::Problem::materialProperty(mat, QStringLiteral("Eg"), &scope).trimmed().isEmpty();
    const QString sigma = detail::Problem::materialProperty(mat, QStringLiteral("sigma"), &scope);
    const double s = evaluateConstant(sigma, scope, Dim::of(-3, -1, 3, 2)).value_or(0);
    if (semiconductor) cm.database = QStringLiteral("Si (Silicon)");
    else if (s > 1e5) cm.database = QStringLiteral("Al (Aluminium) - CRC");
    else cm.database = QStringLiteral("SiO2 (Glass) - Sze");
    w.note(tr("%1: no material of Lumerical's of its name - %2 taken: set its properties there (Materials) to the model's").arg(cm.name, cm.database));
    return cm;
}

/// A band energy (eV, V as eV per electron, a plain number in eV; J) in
/// eV, as the semiconductor reads it; NaN when not a constant.
double energyIn(const QString& text, const Scope& scope)
{
    const Expression e = Expression::compile(text, scope);
    if (!e.isValid() || !e.isConstant()) return NAN;
    return e.dim() == Dim::of(2, 1, -2) ? e.constant() / Q : e.constant();
}

/// The values a property (\a text, of unit \a dim) takes: at the
/// parameters as they are, or at each value of the study's sweep (its first
/// parameter that changes it), as the sweep sets them.
std::vector<double> sweptValues(const Model& model, const QString& text, const Dim& unit, const Node* study, Writer& w, const QString& what)
{
    auto at = [&](const QHash<QString, QString>& overrides) {
        const std::shared_ptr<ParameterScope> ps = evaluateParameters(model, overrides);
        return evaluateConstant(text, ps->scope, unit).value_or(NAN);
    };
    const double here = at({});
    if (study)
        for (const Node& step : study->children) {
            if (!step.enabled || step.type != QLatin1String("sweep")) continue;
            const QJsonArray rows = step.value(QStringLiteral("parameters")).toArray();
            const std::shared_ptr<ParameterScope> ps = evaluateParameters(model);
            for (const QJsonValue& row : rows) {
                const QJsonArray r = row.toArray();
                const QString name = r.at(0).toString().trimmed(), unitText = r.at(2).toString().trimmed();
                if (name.isEmpty()) continue;
                Dim dim;
                QString why;
                std::optional<std::vector<double>> listed = valueList(r.at(1).toString(), ps->scope, &dim, &why);
                if (!listed || listed->empty()) continue;
                // (Plain numbers in the row's unit, as the sweep takes them.)
                if (dim.isNone() && !unitText.isEmpty())
                    if (const std::optional<Unit> u = parseUnit(unitText)) {
                        for (double& v : *listed) v = v * u->scale + u->offset;
                        dim = u->dim;
                    }
                std::vector<double> out;
                bool changes = false;
                for (double v : *listed) {
                    const QString value = dim.isNone() ? QString::number(v, 'g', 17) : QString::number(v, 'g', 17) + QLatin1Char('[') + dimName(dim) + QLatin1Char(']');
                    out.push_back(at({{name, value}}));
                    if (std::abs(out.back() - here) > 1e-12 * std::max(1.0, std::abs(here))) changes = true;
                }
                if (!changes) continue;
                if (rows.size() > 1) w.note(tr("%1 sweeps more than one parameter: %2's values taken for %3, the others as they are").arg(step.name(), name, what));
                return out;
            }
        }
    return {here};
}

/// An implant's doping at a point (m), as the semiconductor has it.
struct Implant {
    double dose = 0, Rp = 0, sd = 0, sl = 0, ys = 0, w1 = -INFINITY, w2 = INFINITY;
    double at(double x, double y) const
    {
        const double d = ys - y;
        if (d < 0 || !(sd > 0)) return 0;
        const double depth = dose / (std::sqrt(2 * M_PI) * sd) * (std::exp(-(d - Rp) * (d - Rp) / (2 * sd * sd)) + std::exp(-(d + Rp) * (d + Rp) / (2 * sd * sd)));
        const double across = 0.5 * ((std::isfinite(w1) ? std::erf((x - w1) / (std::sqrt(2.0) * sl)) : 1.0) - (std::isfinite(w2) ? std::erf((x - w2) / (std::sqrt(2.0) * sl)) : -1.0));
        return depth * across;
    }
};

LumericalScript exportCharge(const Model& model, const Node& node, const QString& base, const QString& version)
{
    LumericalScript out;
    Writer w;
    const Node* phys = physicsOf(model, node, QStringLiteral("semiconductor"));
    if (!phys) {
        out.error = tr("the model has no Semiconductor physics");
        return out;
    }
    if (const QString why = planarOnly(model); !why.isEmpty()) {
        out.error = why;
        return out;
    }
    const Built b = build(model, true);
    if (!b.error.isEmpty()) {
        out.error = b.error;
        return out;
    }
    const Topology& t = *b.topology;
    const Scope& scope = b.parameters->scope;
    const double sc = t.unitScale;
    QString why;
    const QVector<int> semiList = resolveSelection(t, model, Level::Domain, phys->selection(), &why);
    const std::set<int> semi(semiList.begin(), semiList.end());
    const std::vector<const Node*> mats = domainMaterials(model, t);
    // The domains drawn: every one with a material.
    std::vector<int> drawn;
    QRectF device;
    for (int d = 0; d < int(t.domains.size()); ++d) {
        if (!mats[std::size_t(d)]) {
            if (semi.count(d)) {
                out.error = tr("domain %1 is in %2 but has no material").arg(d + 1).arg(phys->name());
                return out;
            }
            continue;
        }
        drawn.push_back(d);
        device = device.isNull() ? t.domains[std::size_t(d)].bounds : device.united(t.domains[std::size_t(d)].bounds);
    }
    if (semi.empty()) {
        out.error = tr("%1 is on no domain").arg(phys->name());
        return out;
    }
    const double size = std::max(device.width(), device.height());
    // The materials.
    std::map<const Node*, ChargeMaterial> material;
    for (int d : drawn) {
        const Node* m = mats[std::size_t(d)];
        if (!material.count(m)) material.insert({m, chargeMaterial(*m, scope, w)});
    }
    // (Model materials' names their own; two alike told apart.)
    {
        std::map<QString, int> seen;
        for (auto& [m, cm] : material)
            if (int& k = seen[cm.name]; ++k > 1) cm.name += QStringLiteral(" %1").arg(k);
    }
    const QString saved = base + QStringLiteral(".ldev");
    w.line(QStringLiteral("newproject;"));
    w.line();
    w.comment(tr("The materials: Lumerical's electrical ones of the model's (CT)."));
    bool alloys = false;
    auto modelMaterial = [&w, &alloys](const QString& name, const QString& database, int fractions, double x, double y) {
        w.line(QStringLiteral("addmodelmaterial;"));
        w.setText(QStringLiteral("name"), name);
        if (fractions == 0) {
            w.line(QStringLiteral("addmaterialproperties(\"CT\", %1);").arg(str(database)));
            return;
        }
        // (An alloy at its composition; else the alloy, its structures' mole
        // fraction set.)
        alloys = true;
        w.line(QStringLiteral("err = \"\";"));
        w.line(QStringLiteral("try { addsemiconductorfromalloy(%1, %2); } catch(err);")
                   .arg(str(database), fractions == 1 ? num(x) : num(x) + QStringLiteral(", ") + num(y)));
        w.line(QStringLiteral("if (length(err) > 0) { addmaterialproperties(\"CT\", %1); }").arg(str(database)));
    };
    for (const auto& [m, cm] : material) modelMaterial(cm.name, cm.database, cm.fractions, cm.x, cm.y);
    const std::vector<Outline> o = outlines(t);
    // The contacts: each on a solid - a metal domain at its boundary, else a
    // thin strip of metal outside it.
    struct ContactSolid {
        const Node* feature;
        QString name;                 // (a part's: its number after it)
        QString solid;
        std::vector<QPointF> strip;   // (empty: a domain's)
        QString metal;
    };
    std::vector<ContactSolid> contacts;
    double strip = NAN;
    if (!node.text(QStringLiteral("strip")).trimmed().isEmpty()) strip = evaluateConstant(node.text(QStringLiteral("strip")), scope, Dim::length()).value_or(NAN) / sc;
    if (!(strip > 0)) strip = 0.01 * size;
    std::map<QString, double> gateMetals;   // its name: the work function (eV)
    bool plainMetal = false;
    for (const Node& f : phys->children) {
        if (!f.enabled || (f.type != QLatin1String("semiconductor/ohmic") && f.type != QLatin1String("semiconductor/gate") && f.type != QLatin1String("semiconductor/schottky")))
            continue;
        const QVector<int> bds = resolveSelection(t, model, Level::Boundary, f.selection(), &why);
        if (bds.isEmpty()) {
            w.note(tr("%1 is on no boundary: left out").arg(f.name()));
            continue;
        }
        QString metal = QStringLiteral("contact metal");
        if (f.type == QLatin1String("semiconductor/gate")) {
            // (Unset: the metal's beside it, as the semiconductor takes it.)
            double phim = energyIn(f.text(QStringLiteral("phim")), scope);
            if (!f.props.contains(QStringLiteral("phim"))) {
                const MaterialScope at300(scope, 300);
                for (int bd : bds)
                    for (int d : {t.boundaries[std::size_t(bd)].up, t.boundaries[std::size_t(bd)].down})
                        if (d >= 0 && !semi.count(d) && mats[std::size_t(d)]) {
                            const QString phi = detail::Problem::materialProperty(*mats[std::size_t(d)], QStringLiteral("phi"), &at300);
                            if (!phi.trimmed().isEmpty() && std::isfinite(energyIn(phi, at300))) phim = energyIn(phi, at300);
                        }
            }
            const double eV = std::isfinite(phim) ? phim : 4.05;
            metal = objectName(QStringLiteral("gate metal %1 eV").arg(QString::number(eV, 'g', 4)));
            gateMetals[metal] = eV;
        } else {
            plainMetal = true;
            if (f.type == QLatin1String("semiconductor/schottky"))
                w.note(tr("%1: a Schottky contact - an electrical contact on a metal here; set its barrier (the metal's work function) there").arg(f.name()));
        }
        int part = 0;
        for (int bd : bds) {
            const Topology::Boundary& B = t.boundaries[std::size_t(bd)];
            // A metal domain beside it (no semiconductor): the contact's solid.
            int metalDomain = -1;
            for (int d : {B.up, B.down})
                if (d >= 0 && !semi.count(d) && mats[std::size_t(d)] && f.type != QLatin1String("semiconductor/gate")) {
                    const QString s = detail::Problem::materialProperty(*mats[std::size_t(d)], QStringLiteral("sigma"), &scope);
                    if (evaluateConstant(s, scope, Dim::of(-3, -1, 3, 2)).value_or(0) > 1e5) metalDomain = d;
                }
            const QString contactName = f.text(QStringLiteral("name")).isEmpty() ? f.name() : f.text(QStringLiteral("name"));
            ContactSolid cs{&f, objectName(bds.size() > 1 ? QStringLiteral("%1 %2").arg(contactName).arg(++part) : contactName), {}, {}, metal};
            if (metalDomain >= 0) {
                cs.solid = structureName(t, metalDomain);
            } else {
                // The side without the device: outside it, along the normal.
                // (Its semiconductor's side, else its material's.)
                const int inside = B.up >= 0 && semi.count(B.up) ? B.up
                                 : B.down >= 0 && semi.count(B.down) ? B.down
                                 : B.up >= 0 && mats[std::size_t(B.up)] ? B.up : B.down;
                const QPointF nrm = t.normalAt(bd, inside);
                std::vector<QPointF> pts = B.polyline(t);
                if (pts.size() < 2) continue;
                std::vector<QPointF> poly = pts;
                for (auto it = pts.rbegin(); it != pts.rend(); ++it) poly.push_back(*it + nrm * strip);
                if (signedArea(poly) < 0) std::reverse(poly.begin(), poly.end());
                cs.strip = poly;
                cs.solid = QStringLiteral("contact ") + cs.name;
                if (B.up >= 0 && B.down >= 0 && mats[std::size_t(B.up)] && mats[std::size_t(B.down)])
                    w.note(tr("%1: between two domains - its metal strip over the one that is not its semiconductor (the strip's mesh order the higher)").arg(f.name()));
                device = device.united(boundsOf(poly));
            }
            contacts.push_back(cs);
        }
    }
    if (plainMetal) modelMaterial(QStringLiteral("contact metal"), QStringLiteral("Al (Aluminium) - CRC"), 0, 0, 0);
    for (const auto& [name, eV] : gateMetals) {
        modelMaterial(name, QStringLiteral("Al (Aluminium) - CRC"), 0, 0, 0);
        w.line(QStringLiteral("try { set(\"work function\", %1); } catch(err); # (its work function, eV; Lumerical's aluminium's otherwise)").arg(num(eV)));
    }
    w.line();
    // The cross-section, in the x-z plane.
    w.comment(tr("The cross-section in the x-z plane (2D y-normal, Lumerical's default): the model's x as x, its y as z; the outer first "
                 "(where they overlap the lower mesh order wins: an inner domain's, a contact's metal's)."));
    // (Each domain its own mesh order - the later drawn the lower -, above
    // the contacts' metal strips', each its own too: none left to the
    // objects' order.)
    const int strips = int(std::count_if(contacts.begin(), contacts.end(), [](const ContactSolid& cs) { return !cs.strip.empty(); }));
    const std::vector<int> chargeOrder = drawingOrder(o, drawn);
    for (std::size_t i = 0; i < chargeOrder.size(); ++i) {
        const int d = chargeOrder[i];
        w.comment(tr("domain %1: %2").arg(d + 1).arg(material.at(mats[std::size_t(d)]).name));
        const ChargeMaterial& cm = material.at(mats[std::size_t(d)]);
        QStringList extra;
        if (cm.fractions == 1) extra << QStringLiteral("try { set(\"mole frac constant\", %1); } catch(err); # (the alloy's, had it no composition of its own)").arg(num(cm.x));
        writeStructure(w, t, o[std::size_t(d)], structureName(t, d), cm.name, 'y', 10 * size * sc, Order::Device, int(chargeOrder.size() - i) + strips, extra);
    }
    int stripOrder = 0;
    for (const ContactSolid& cs : contacts) {
        if (cs.strip.empty()) continue;
        ++stripOrder;
        Outline so;
        so.loops = {cs.strip};
        so.bounds = boundsOf(cs.strip);
        // (A rectangle's four corners.)
        so.rectangle = cs.strip.size() == 4 && std::all_of(cs.strip.begin(), cs.strip.end(), [&](QPointF p) {
                           const double tol = 1e-9 * std::max(so.bounds.width(), so.bounds.height());
                           return (std::abs(p.x() - so.bounds.left()) < tol || std::abs(p.x() - so.bounds.right()) < tol)
                                  && (std::abs(p.y() - so.bounds.top()) < tol || std::abs(p.y() - so.bounds.bottom()) < tol);
                       });
        w.comment(tr("%1's metal").arg(cs.feature->name()));
        writeStructure(w, t, so, cs.solid, cs.metal, 'y', 10 * size * sc, Order::Device, stripOrder);
    }
    w.line();
    // The solver and its region.
    const double T = evaluateConstant(phys->text(QStringLiteral("T")), scope, Dim::temperature()).value_or(300);
    const double thickness = evaluateConstant(model.component().text(QStringLiteral("thickness")), scope, Dim::length()).value_or(1);
    w.comment(tr("The CHARGE solver: a 2D y-normal region over the device, its norm length the model's thickness."));
    w.line(QStringLiteral("addchargesolver;"));
    w.set(QStringLiteral("norm length"), thickness > 0 ? thickness : 1.0);
    w.line(QStringLiteral("try { set(\"simulation temperature\", %1); } catch(err);").arg(num(T)));
    w.line(QStringLiteral("addsimulationregion;"));
    w.setText(QStringLiteral("name"), QStringLiteral("qucs-s region"));
    w.set(QStringLiteral("dimension"), 2.0);
    w.set(QStringLiteral("x"), device.center().x() * sc);
    w.set(QStringLiteral("x span"), device.width() * sc);
    w.set(QStringLiteral("y"), 0.0);
    w.set(QStringLiteral("z"), device.center().y() * sc);
    w.set(QStringLiteral("z span"), device.height() * sc);
    w.line(QStringLiteral("setnamed(\"CHARGE\", \"simulation region\", \"qucs-s region\");"));
    w.line();
    // The doping.
    w.comment(tr("The doping (SI, 1/m3): Lumerical's doping objects add, n against p."));
    int dopings = 0;
    bool restricted = false;
    std::unique_ptr<Solution> sol;
    std::unique_ptr<Locator> locator;
    for (const Node& f : phys->children) {
        if (!f.enabled || f.type != QLatin1String("semiconductor/doping")) continue;
        const QVector<int> sel = resolveSelection(t, model, Level::Domain, f.selection(), &why);
        std::vector<int> on;
        for (int d : sel)
            if (semi.count(d)) on.push_back(d);
        if (on.empty()) continue;
        const QString type = f.text(QStringLiteral("dopant")) == QLatin1String("donor") ? QStringLiteral("n") : QStringLiteral("p");
        const QString profile = f.text(QStringLiteral("profile"));
        const QString fname = objectName(f.name());
        auto restrict = [&](int d) {
            restricted = true;
            w.line(QStringLiteral("try { set(\"volume type\", \"solid\"); set(\"volume solid\", %1); } catch(err); # (its domain alone)").arg(str(structureName(t, d))));
        };
        auto box = [&](const QRectF& r) {
            w.set(QStringLiteral("x min"), r.left() * sc);
            w.set(QStringLiteral("x max"), r.right() * sc);
            w.set(QStringLiteral("z min"), r.top() * sc);
            w.set(QStringLiteral("z max"), r.bottom() * sc);
            w.set(QStringLiteral("y"), 0.0);
            w.set(QStringLiteral("y span"), 10 * size * sc);
            w.line(QStringLiteral("try { set(\"use relative coordinates\", 0); } catch(err);"));
        };
        // Whether a box holds a semiconductor this doping is not on; whether
        // a box over domain d holds more than it.
        auto others = [&](const QRectF& r) {
            for (int e : semi) {
                const QRectF x = r.intersected(t.domains[std::size_t(e)].bounds);
                if (std::find(on.begin(), on.end(), e) == on.end() && x.width() * x.height() > 1e-12 * size * size) return true;
            }
            return false;
        };
        auto overreaches = [&](const QRectF& r, int d) { return !o[std::size_t(d)].rectangle || others(r); };
        if (profile == QLatin1String("uniform") || profile.isEmpty()) {
            const double N0 = evaluateConstant(f.text(QStringLiteral("N0")), scope, Dim::of(-3)).value_or(NAN);
            // (One box when its domains are rectangles that fill one, and
            // nothing else is in it.)
            QRectF all;
            double area = 0;
            bool rectangles = true;
            for (int d : on) {
                const QRectF r = t.domains[std::size_t(d)].bounds;
                all = all.isNull() ? r : all.united(r);
                area += r.width() * r.height();
                rectangles = rectangles && o[std::size_t(d)].rectangle;
            }
            if (on.size() > 1 && rectangles && std::abs(area - all.width() * all.height()) <= 1e-9 * all.width() * all.height() && !others(all)) {
                ++dopings;
                w.line(QStringLiteral("adddope;"));
                w.setText(QStringLiteral("name"), fname);
                w.setText(QStringLiteral("dopant type"), type);
                w.set(QStringLiteral("concentration"), N0);
                box(all);
                continue;
            }
            for (int d : on) {
                ++dopings;
                const QRectF r = t.domains[std::size_t(d)].bounds;
                w.line(QStringLiteral("adddope;"));
                w.setText(QStringLiteral("name"), on.size() > 1 ? QStringLiteral("%1 d%2").arg(fname).arg(d + 1) : fname);
                w.setText(QStringLiteral("dopant type"), type);
                w.set(QStringLiteral("concentration"), N0);
                box(r);
                if (overreaches(r, d)) restrict(d);
            }
        } else if (profile == QLatin1String("box")) {
            // Lumerical's diffusion: N0 inside, a Gaussian over the junction
            // width inside each face but the source's (out of the plane here)
            // to the reference concentration - ours a Gaussian outside the box
            // to the background at the junction depth: its box that much larger.
            auto length = [&](const QJsonValue& v) -> double {
                Expression::Options inUnit;
                inUnit.lengthUnit = sc;
                inUnit.numbersAreLengths = true;
                const Expression e = Expression::compile(v.toVariant().toString(), scope, inUnit);
                return e.isValid() && e.isConstant() ? e.constant() : NAN;
            };
            const QJsonArray at = f.value(QStringLiteral("position")).toArray(), sz = f.value(QStringLiteral("size")).toArray();
            const double x0 = length(at.at(0)), y0 = length(at.at(1)), x1 = x0 + length(sz.at(0)), y1 = y0 + length(sz.at(1));
            const double dj = evaluateConstant(f.text(QStringLiteral("dj")), scope, Dim::length()).value_or(NAN);
            const double N0 = evaluateConstant(f.text(QStringLiteral("N0")), scope, Dim::of(-3)).value_or(NAN);
            const double Nb = evaluateConstant(f.text(QStringLiteral("Nb")), scope, Dim::of(-3)).value_or(NAN);
            if (!std::isfinite(x0 + y0 + x1 + y1 + dj + N0 + Nb)) {
                out.error = tr("%1: its box, junction depth and concentrations must be constants").arg(f.name());
                return out;
            }
            const QRectF r = QRectF(QPointF(std::min(x0, x1) - dj / sc, std::min(y0, y1) - dj / sc), QPointF(std::max(x0, x1) + dj / sc, std::max(y0, y1) + dj / sc));
            ++dopings;
            w.line(QStringLiteral("adddiffusion;"));
            w.setText(QStringLiteral("name"), fname);
            w.setText(QStringLiteral("dopant type"), type);
            w.set(QStringLiteral("concentration"), N0);
            w.set(QStringLiteral("ref concentration"), Nb);
            w.set(QStringLiteral("junction width"), dj);
            w.set(QStringLiteral("source face"), 4.0);
            w.line(QStringLiteral("try { set(\"distribution function\", \"gaussian 3d\"); } catch(err);"));
            box(r);
            if (on.size() == 1 && overreaches(r, on.front())) restrict(on.front());
            w.note(tr("%1: a box with Gaussian tails as Lumerical's diffusion - alike along its sides, its corners Lumerical's products of the two sides' falls where the model's are by the distance").arg(f.name()));
        } else {
            // An implant or an expression: sampled on a grid over its domains.
            QRectF r;
            for (int d : on) r = r.isNull() ? t.domains[std::size_t(d)].bounds : r.united(t.domains[std::size_t(d)].bounds);
            Implant imp;
            Expression::Options inUnit;
            inUnit.lengthUnit = sc;
            inUnit.numbersAreLengths = true;
            QString N;
            if (profile == QLatin1String("implant")) {
                auto length = [&](const QString& text) -> double {
                    if (text.trimmed().isEmpty()) return NAN;
                    const Expression e = Expression::compile(text, scope, inUnit);
                    return e.isValid() && e.isConstant() ? e.constant() * sc : NAN;
                };
                imp.dose = evaluateConstant(f.text(QStringLiteral("dose")), scope, Dim::of(-2)).value_or(0);
                imp.Rp = evaluateConstant(f.text(QStringLiteral("Rp")), scope, Dim::length()).value_or(0);
                const double dRp = evaluateConstant(f.text(QStringLiteral("dRp")), scope, Dim::length()).value_or(0);
                const double dRl = evaluateConstant(f.text(QStringLiteral("dRl")), scope, Dim::length()).value_or(0);
                imp.ys = length(f.text(QStringLiteral("surface")));
                if (!std::isfinite(imp.ys)) imp.ys = 0;
                const double a = length(f.text(QStringLiteral("x1"))), c = length(f.text(QStringLiteral("x2")));
                if (std::isfinite(a)) imp.w1 = a;
                if (std::isfinite(c)) imp.w2 = c;
                if (imp.w2 < imp.w1) std::swap(imp.w1, imp.w2);
                const double D0 = evaluateConstant(f.text(QStringLiteral("D0")), scope, Dim::of(2, 0, -1)).value_or(0);
                const double Ea = energyIn(f.text(QStringLiteral("Ea")), scope) * Q;
                const double Ta = evaluateConstant(f.text(QStringLiteral("Ta")), scope, Dim::temperature()).value_or(0);
                const double ta = evaluateConstant(f.text(QStringLiteral("ta")), scope, Dim::of(0, 0, 1)).value_or(0);
                const double twoDt = ta > 0 && D0 > 0 && Ta > 0 && std::isfinite(Ea) ? 2 * D0 * std::exp(-Ea / (1.380649e-23 * Ta)) * ta : 0.0;
                imp.sd = std::sqrt(dRp * dRp + twoDt);
                imp.sl = std::sqrt(dRl * dRl + twoDt);
                if (!(imp.sd > 0) || !(imp.sl > 0)) {
                    out.error = tr("%1: its straggles must be more than 0").arg(f.name());
                    return out;
                }
            } else {
                N = f.text(QStringLiteral("N"));
                if (!sol) {
                    sol = std::make_unique<Solution>(model, b.parameters, b.mesh);
                    locator = std::make_unique<Locator>(*b.mesh);
                }
            }
            // Its value at a point: the doped domains' from inside them - a
            // point on an edge a hair in -, nought outside them.
            const double extent = std::max(r.width(), r.height()), hair = 1e-6 * extent;
            const Expression expression = N.isEmpty() ? Expression() : Expression::compile(N, sol->scope());
            int missing = 0;
            auto value = [&hair, &t, &on, &profile, &imp, &sc, &sol, &locator, &expression, &missing](QPointF p, bool counted, bool* in = nullptr) {
                const QPointF tries[] = {p, p + QPointF(hair, 0), p - QPointF(hair, 0), p + QPointF(0, hair), p - QPointF(0, hair),
                                         p + QPointF(hair, hair), p + QPointF(-hair, hair), p + QPointF(hair, -hair), p - QPointF(hair, hair)};
                if (in) *in = false;
                for (const QPointF& q : tries) {
                    const int d = t.domainAt(q);
                    if (d < 0 || std::find(on.begin(), on.end(), d) == on.end()) continue;
                    if (in) *in = true;
                    if (profile == QLatin1String("implant")) return imp.at(q.x() * sc, q.y() * sc);
                    const std::optional<double> e = evaluateAt(*sol, *locator, expression, q);
                    if (e) return std::max(0.0, *e);
                    if (counted) ++missing;
                    return 0.0;
                }
                return 0.0;
            };
            // The grid: evenly 20 to 200 lines along each axis (by its
            // aspect), lines at the doped domains' corners and a hair either
            // side (their edges kept sharp), each interval then halved while
            // the profile midway, along lines across it, is more than 1 % off
            // the straight line between its ends (Lumerical's interpolation)
            // - a Gaussian's σ three samples and more -, what is less than
            // 1e-5 of the largest value met let be (a tail's). Across the
            // other axis' lines in turn (an implant's edges, a thin layer's).
            auto lines = [&r, &on, &o, &extent](int axis) {
                const double lo = axis == 0 ? r.left() : r.top(), hi = axis == 0 ? r.right() : r.bottom();
                const double ratio = axis == 0 ? r.width() / r.height() : r.height() / r.width();
                const int even = std::clamp(int(std::ceil(ratio * 80)), 20, 200);
                std::vector<double> at;
                for (int i = 0; i < even; ++i) at.push_back(lo + (hi - lo) * i / (even - 1));
                for (int d : on)
                    for (const auto& loop : o[std::size_t(d)].loops)
                        for (const QPointF& q : loop) {
                            const double c = axis == 0 ? q.x() : q.y();
                            for (double e : {c - 1e-4 * extent, c, c + 1e-4 * extent})
                                if (e >= lo && e <= hi) at.push_back(e);
                        }
                std::sort(at.begin(), at.end());
                at.erase(std::unique(at.begin(), at.end(), [&](double a, double b) { return b - a < 1e-9 * extent; }), at.end());
                return at;
            };
            std::vector<double> xs = lines(0), zs = lines(1);
            double largest = 0;
            for (double a : xs)
                for (double c : zs) largest = std::max(largest, value(QPointF(a, c), false));
            auto refined = [&r, &value, &largest](std::vector<double> along, const std::vector<double>& acrossLines, int axis) {
                // (At most 48 lines across, evenly among the other axis' own.)
                std::vector<double> across;
                const std::size_t m = acrossLines.size();
                for (std::size_t i = 0; i < std::min<std::size_t>(m, 48); ++i) across.push_back(acrossLines[m <= 48 ? i : i * (m - 1) / 47]);
                for (double c : {0.25, 0.5, 0.75}) across.push_back((axis == 0 ? r.top() : r.left()) + c * (axis == 0 ? r.height() : r.width()));
                auto at = [&](double a, double c, bool* in) { return value(axis == 0 ? QPointF(a, c) : QPointF(c, a), false, in); };
                const double least = 1e-5 * (axis == 0 ? r.width() : r.height());
                for (int round = 0; round < 24 && along.size() < 400; ++round) {
                    std::vector<double> next{along.front()};
                    bool split = false;
                    for (std::size_t i = 1; i < along.size(); ++i) {
                        const double a = along[i - 1], b = along[i], mid = (a + b) / 2;
                        bool off = false;
                        if (b - a > 2 * least)
                            for (double c : across) {
                                // (Across the selection's edge a step: its own lines' - not halved for.)
                                bool ia = false, ib = false, im = false;
                                const double va = at(a, c, &ia), vb = at(b, c, &ib), vm = at(mid, c, &im);
                                largest = std::max({largest, va, vb, vm});
                                if (!ia || !ib || !im) continue;
                                if (std::abs(vm - (va + vb) / 2) > 0.01 * std::max({va, vb, vm}) + 1e-5 * largest) {
                                    off = true;
                                    break;
                                }
                            }
                        if (off && next.size() + (along.size() - i) < 400) {
                            next.push_back(mid);
                            split = true;
                        }
                        next.push_back(b);
                    }
                    along = next;
                    if (!split) break;
                }
                return along;
            };
            zs = refined(zs, xs, 1);
            xs = refined(xs, zs, 0);
            zs = refined(zs, xs, 1);
            const int nx = int(xs.size()), nz = int(zs.size());
            std::vector<double> values;   // x fastest, then y (two), then z
            values.reserve(std::size_t(nx * 2 * nz));
            for (int k = 0; k < nz; ++k) {
                std::vector<double> row;
                for (int i = 0; i < nx; ++i) row.push_back(value(QPointF(xs[std::size_t(i)], zs[std::size_t(k)]), true));
                for (int j = 0; j < 2; ++j) values.insert(values.end(), row.begin(), row.end());
            }
            if (missing > 0)
                w.note(tr("%1: its N not had at %2 of the grid's points (another physics' field, not solved here): nought there").arg(f.name()).arg(missing));
            ++dopings;
            w.comment(tr("%1: %2, sampled on %3 x %4 points (finer where it changes: within 1 % between them)")
                          .arg(f.name(), profile == QLatin1String("implant") ? tr("an implant (annealed)") : tr("an expression"))
                          .arg(nx)
                          .arg(nz));
            w.line(QStringLiteral("addimportdope;"));
            w.setText(QStringLiteral("name"), fname);
            w.setText(QStringLiteral("dopant type"), type);
            w.line(QStringLiteral("try { set(\"use relative coordinates\", 0); } catch(err);"));
            // (Its lines to the last digit: where it is steep, a rounded line's
            // value is not the one sampled.)
            auto column = [&](const std::vector<double>& v) {
                QStringList c;
                for (double x : v) c << QString::number(x * sc, 'g', 17);
                return c.join(QLatin1String("; "));
            };
            w.line(QStringLiteral("x = [%1];").arg(column(xs)));
            w.line(QStringLiteral("y = [%1; %2];").arg(num(-5 * size * sc), num(5 * size * sc)));
            w.line(QStringLiteral("z = [%1];").arg(column(zs)));
            QStringList chunks;
            for (std::size_t i = 0; i < values.size(); i += 16) {
                QStringList c;
                for (std::size_t j = i; j < std::min(values.size(), i + 16); ++j) c << num(values[j]);
                chunks << c.join(QLatin1Char(','));
            }
            w.line(QStringLiteral("N = reshape([%1], [%2, 2, %3]);").arg(chunks.join(QStringLiteral(",\n    "))).arg(nx).arg(nz));
            w.line(QStringLiteral("doping = rectilineardataset(\"dope\", x, y, z);"));
            w.line(QStringLiteral("doping.addparameter(\"a\", 1);"));
            w.line(QStringLiteral("doping.addattribute(\"N\", N);"));
            w.line(QStringLiteral("select(%1);").arg(str(QStringLiteral("CHARGE::") + fname)));
            w.line(QStringLiteral("importdataset(doping);"));
        }
    }
    if (restricted) w.note(tr("a doping box reaching past its domain is kept to the domain's solid (its volume type \"solid\", tried)"));
    w.line();
    // The contacts.
    w.comment(tr("The contacts: an electrical contact on each one's metal, at its voltage."));
    const std::pair<const Node*, const Node*> studyAndStep = studyStep(model, node, {QStringLiteral("stationary"), QStringLiteral("sweep")});
    const Node* const study = studyAndStep.first;
    for (const ContactSolid& cs : contacts) {
        const Node& f = *cs.feature;
        const std::vector<double> v = sweptValues(model, f.text(QStringLiteral("V0")), Dim::voltage(), study, w, f.name());
        w.line(QStringLiteral("addelectricalcontact;"));
        w.setText(QStringLiteral("name"), cs.name);
        w.setText(QStringLiteral("bc mode"), QStringLiteral("steady state"));
        if (v.size() > 1) {
            w.setText(QStringLiteral("sweep type"), QStringLiteral("value"));
            w.set(QStringLiteral("value table"), QStringLiteral("[%1]").arg([&] {
                QStringList l;
                for (double x : v) l << num(x);
                return l.join(QStringLiteral(", "));
            }()));
        } else {
            w.setText(QStringLiteral("sweep type"), QStringLiteral("single"));
            w.set(QStringLiteral("voltage"), v.empty() ? 0.0 : v.front());
        }
        w.setText(QStringLiteral("surface type"), QStringLiteral("solid"));
        w.setText(QStringLiteral("solid"), cs.solid);
    }
    // What is not taken.
    QStringList own, left;
    for (const Node& f : phys->children) {
        if (!f.enabled) continue;
        static const QStringList notTaken = {QStringLiteral("semiconductor/impact"), QStringLiteral("semiconductor/btbt"), QStringLiteral("semiconductor/quantum"),
                                             QStringLiteral("semiconductor/generation"), QStringLiteral("semiconductor/optical"), QStringLiteral("semiconductor/charge"),
                                             QStringLiteral("semiconductor/polarization"), QStringLiteral("semiconductor/fn"), QStringLiteral("semiconductor/traps")};
        const QString kind = f.kind() ? f.kind()->title : f.type;
        const QString named = f.name() == kind ? kind : tr("%1 (%2)").arg(f.name(), kind);
        if (notTaken.contains(f.type)) left << named;
        if (f.type == QLatin1String("semiconductor/model") || f.type == QLatin1String("semiconductor/srh") || f.type == QLatin1String("semiconductor/auger")
            || f.type == QLatin1String("semiconductor/radiative"))
            own << named;
    }
    if (!own.isEmpty())
        w.note(tr("the materials' models and parameters (mobility, recombination) Lumerical's own: check them against the model's %1").arg(own.join(QStringLiteral(", "))));
    if (!left.isEmpty()) w.note(tr("not taken - set them there: %1").arg(left.join(QStringLiteral(", "))));
    w.line();
    w.line(QStringLiteral("save(%1);").arg(str(saved)));
    if (node.flag(QStringLiteral("run"))) w.line(QStringLiteral("run(\"CHARGE\");"));
    out.text = compose(header(QStringLiteral("CHARGE"), model, base, version, saved, tr("builds the model in a new project"),
                              tr("Lumerical's Finite Element IDE (CHARGE)")),
                       w);
    out.notes = w.notes;
    out.summary = tr("a CHARGE script: %1 structures, %2 doping objects, %3 contacts").arg(drawn.size()).arg(dopings).arg(contacts.size());
    return out;
}

// --- MQW --------------------------------------------------------------------

/// A compound's name in Lumerical's MQW database and its composition there.
std::optional<Alloy> mqwCompound(const QString& compound, double x, double y)
{
    static const QStringList binaries = {QStringLiteral("GaAs"), QStringLiteral("AlAs"), QStringLiteral("InAs"), QStringLiteral("InP"), QStringLiteral("GaP")};
    static const QStringList ternaries = {QStringLiteral("AlGaAs"), QStringLiteral("InGaAs"), QStringLiteral("InAlAs"), QStringLiteral("GaAsP"),
                                          QStringLiteral("InAsP")};
    if (binaries.contains(compound)) return Alloy{compound, 0, 0, 0, {}};
    if (ternaries.contains(compound)) return Alloy{compound, x, 0, 1, {}};
    if (compound == QLatin1String("AlGaInAs")) return Alloy{compound, x, y, 2, {}};
    // (Ours Ga x In 1-x P and In 1-x Ga x As y P 1-y; Lumerical's In x.)
    if (compound == QLatin1String("GaInP")) return Alloy{QStringLiteral("InGaP"), 1 - x, 0, 1, {}};
    if (compound == QLatin1String("InGaAsP")) return Alloy{QStringLiteral("InGaAsP"), 1 - x, y, 2, {}};
    return std::nullopt;
}

LumericalScript exportMqw(const Model& model, const Node& node, const QString& base, const QString& version)
{
    LumericalScript out;
    Writer w;
    const Node* phys = physicsOf(model, node, QStringLiteral("quantum"));
    if (!phys) {
        out.error = tr("the model has no Quantum Wells physics");
        return out;
    }
    const Built b = build(model, true);
    if (!b.error.isEmpty()) {
        out.error = b.error;
        return out;
    }
    const Topology& t = *b.topology;
    const Scope& scope = b.parameters->scope;
    const double sc = t.unitScale;
    Solution sol(model, b.parameters, b.mesh);
    const int fi = sol.fieldIndex(phys->tag);
    if (fi < 0) {
        out.error = tr("%1 has no field").arg(phys->name());
        return out;
    }
    const detail::QuantumLayers q = detail::quantumLayers(sol, fi);
    if (!q.error.isEmpty()) {
        out.error = q.error;
        return out;
    }
    if (!q.line) {
        out.error = tr("%1 is 2D: mqwindex's is a stack of layers (a 1D well)").arg(phys->name());
        return out;
    }
    // The layers: the physics' domains along its axis, bottom first.
    struct Layer {
        int d;
        double from, to;   // along the axis (m)
    };
    std::vector<Layer> layers;
    for (int d = 0; d < int(q.layers.size()); ++d) {
        if (!q.layers[std::size_t(d)].on) continue;
        const QRectF r = t.domains[std::size_t(d)].bounds;
        layers.push_back({d, (q.axis == 0 ? r.left() : r.top()) * sc, (q.axis == 0 ? r.right() : r.bottom()) * sc});
    }
    std::sort(layers.begin(), layers.end(), [](const Layer& a, const Layer& c) { return a.from < c.from; });
    if (layers.empty()) {
        out.error = tr("%1 is on no domain").arg(phys->name());
        return out;
    }
    const double z0 = layers.front().from, L = layers.back().to - z0;
    for (std::size_t i = 1; i < layers.size(); ++i)
        if (std::abs(layers[i].from - layers[i - 1].to) > 1e-6 * L) {
            out.error = tr("%1's domains are not a stack along its axis (domains %2 and %3)").arg(phys->name()).arg(layers[i - 1].d + 1).arg(layers[i].d + 1);
            return out;
        }
    const std::vector<const Node*> mats = domainMaterials(model, t);
    const bool database = node.text(QStringLiteral("materials")) == QLatin1String("database");
    double gapLeast = INFINITY;
    for (const Layer& l : layers) {
        const detail::QuantumLayer& ql = q.layers[std::size_t(l.d)];
        const QString where = tr("domain %1").arg(l.d + 1);
        if (ql.wurtzite) {
            out.error = tr("%1 is wurtzite: mqwindex is zinc blende's 4x4 - Lumerical's MQW solver object has the 6x6 (an imported .ldev's)").arg(where);
            return out;
        }
        if (!std::isfinite(ql.Ec) || !std::isfinite(ql.Ev)) {
            out.error = tr("%1: its band edges must be constants (mqwindex's layers are uniform)").arg(where);
            return out;
        }
        if (ql.valleys) w.note(tr("%1: its electrons' valleys - mqwindex's conduction band one, Γ's (cbvalley)").arg(where));
        gapLeast = std::min(gapLeast, ql.Ec - ql.Ev);
    }
    if (q.valence == 6 || q.valence == 8)
        w.note(tr("the model's k·p %1: mqwindex's is the 4x4 (Lumerical's MQW solver object has the 6x6 and the 8x8)").arg(q.valence == 6 ? QStringLiteral("6x6") : QStringLiteral("8x8")));
    else if (q.valence == 0)
        w.note(tr("the model's holes parabolic: mqwindex mixes them (the 4x4)"));
    if (q.potential) w.note(tr("%1's potential of another physics left out: the field alone").arg(phys->name()));
    // The studies: the bands step's k, the spectrum step's.
    const std::pair<const Node*, const Node*> bStudyAndBands = studyStep(model, node, {QStringLiteral("bands")});
    const Node* const bStudy = bStudyAndBands.first;
    const Node* const bands = bStudyAndBands.second;
    const std::pair<const Node*, const Node*> sStudyAndSpectrum = studyStep(model, node, {QStringLiteral("spectrum")});
    const Node* const sStudy = sStudyAndSpectrum.first;
    const Node* const spectrum = sStudyAndSpectrum.second;
    QString why;
    std::vector<double> frequencies;
    double gamma = 2 * 6.6e-3, density = 0, nr = NAN;
    bool excitons = false;
    int states = 0;
    if (spectrum) {
        Dim dim;
        const std::optional<std::vector<double>> v = valueList(spectrum->text(QStringLiteral("energies")), scope, &dim, &why);
        if (!v || v->empty()) {
            out.error = tr("%1: its photon energies: %2").arg(spectrum->name(), why);
            return out;
        }
        for (double x : *v) frequencies.push_back(dim == Dim::length() ? C0 / x : dim == Dim::of(0, 0, -1) ? x : dim.isNone() ? x * Q / H : x / H);
        const double hw = energyIn(spectrum->text(QStringLiteral("linewidth")), scope);
        if (std::isfinite(hw)) gamma = 2 * hw;
        if (spectrum->text(QStringLiteral("lineshape")) != QLatin1String("lorentzian") && !spectrum->text(QStringLiteral("lineshape")).isEmpty())
            w.note(tr("%1's line %2: mqwindex's is a Lorentzian").arg(spectrum->name(), spectrum->text(QStringLiteral("lineshape"))));
        if (!spectrum->text(QStringLiteral("dlinewidth")).trimmed().isEmpty()) w.note(tr("%1's broadening's growth with the field left out").arg(spectrum->name()));
        if (spectrum->text(QStringLiteral("continuum")) == QLatin1String("bound")) w.note(tr("%1's bulk continuum left out: mqwindex's states its box's").arg(spectrum->name()));
        density = evaluateConstant(spectrum->text(QStringLiteral("density")), scope, Dim::of(-3)).value_or(0);
        if (!spectrum->text(QStringLiteral("nr")).trimmed().isEmpty()) nr = evaluateConstant(spectrum->text(QStringLiteral("nr")), scope, Dim()).value_or(NAN);
        excitons = spectrum->flag(QStringLiteral("excitons"));
        states = spectrum->integer(QStringLiteral("states"));
    } else {
        w.note(tr("no study with an optical spectrum: the gap's neighbourhood, 6.6 meV broadening, no carriers"));
        for (int i = 0; i < 201; ++i) frequencies.push_back((gapLeast - 0.05 + 0.4 * i / 200.0) * Q / H);
    }
    // The wells: the layers of the least gap; their thickness.
    double wells = 0;
    for (const Layer& l : layers) {
        const detail::QuantumLayer& ql = q.layers[std::size_t(l.d)];
        if (ql.Ec - ql.Ev <= gapLeast + 1e-9) {
            wells += l.to - l.from;
            if (!std::isfinite(nr)) nr = ql.nr;
        }
    }
    if (!std::isfinite(nr)) nr = 3.5;
    // (Lumerical's carrier density over the whole stack.)
    const double cden = wells > 0 ? density * wells / L : density;
    w.comment(tr("The stack: %1 layers, bottom first (the model's %2 from %3 nm).").arg(layers.size()).arg(q.axis == 0 ? QStringLiteral("x") : QStringLiteral("y")).arg(num(z0 * 1e9)));
    w.line(QStringLiteral("stack = struct;"));
    w.line(QStringLiteral("stack.gamma = %1; # (eV, a full width: twice the model's half width)").arg(num(gamma)));
    w.line(QStringLiteral("stack.neff = [%1, %2; %3, %2];").arg(num(*std::min_element(frequencies.begin(), frequencies.end())), num(nr), num(*std::max_element(frequencies.begin(), frequencies.end()))));
    std::vector<double> lengths, strains;
    for (const Layer& l : layers) {
        lengths.push_back(l.to - l.from);
        strains.push_back(q.layers[std::size_t(l.d)].exx);
    }
    w.line(QStringLiteral("stack.length = %1;").arg(column(lengths)));
    w.line(QStringLiteral("stack.strain = %1; # ((a0 - a)/a: the substrate's lattice over the layer's)").arg(column(strains)));
    w.line(QStringLiteral("stack.material = cell(%1);").arg(layers.size()));
    int fromDatabase = 0;
    bool defaultLattice = false;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        const Layer& l = layers[i];
        const detail::QuantumLayer& ql = q.layers[std::size_t(l.d)];
        const Node* mat = mats[std::size_t(l.d)];
        const QString object = t.objectNames.value(t.domains[std::size_t(l.d)].owner);
        w.comment(tr("layer %1: domain %2 (%3), %4 nm").arg(i + 1).arg(l.d + 1).arg(object, num((l.to - l.from) * 1e9)));
        const QString m = QStringLiteral("stack.material{%1}").arg(i + 1);
        w.line(m + QStringLiteral(" = struct;"));
        if (database && !ql.user && mat) {
            const QString compound = detail::Problem::materialProperty(*mat, QStringLiteral("compound")).trimmed();
            const double x = evaluateConstant(detail::Problem::materialProperty(*mat, QStringLiteral("x")), scope, Dim()).value_or(0);
            const double y = evaluateConstant(detail::Problem::materialProperty(*mat, QStringLiteral("y")), scope, Dim()).value_or(0);
            if (const std::optional<Alloy> a = mqwCompound(compound, x, y)) {
                ++fromDatabase;
                w.line(m + QStringLiteral(".database_material = %1;").arg(str(a->name)));
                if (a->fractions >= 1) w.line(m + QStringLiteral(".x = %1;").arg(num(a->x)));
                if (a->fractions == 2) w.line(m + QStringLiteral(".y = %1;").arg(num(a->y)));
                continue;
            }
            w.note(tr("domain %1: no compound of Lumerical's MQW database - the model's parameters").arg(l.d + 1));
        }
        // (The model's own: unstrained, on the valence band's scale; what
        // strain needs nought where there is none.)
        const bool strained = ql.exx != 0;
        auto given = [&](double v, double otherwise) { return std::isfinite(v) ? v : otherwise; };
        if (!std::isfinite(ql.a0)) defaultLattice = true;
        w.line(m + QStringLiteral(".eg = %1;").arg(num(ql.Ec - ql.Ev)));
        w.line(m + QStringLiteral(".ep = %1;").arg(num(ql.Ep)));
        w.line(m + QStringLiteral(".me = %1;").arg(num(ql.me)));
        w.line(m + QStringLiteral(".gamma1 = %1;").arg(num(ql.g1)));
        w.line(m + QStringLiteral(".gamma2 = %1;").arg(num(ql.g2)));
        w.line(m + QStringLiteral(".gamma3 = %1;").arg(num(ql.g3)));
        w.line(m + QStringLiteral(".ac = %1;").arg(num(given(ql.ac, 0))));
        w.line(m + QStringLiteral(".av = %1;").arg(num(given(ql.av, 0))));
        w.line(m + QStringLiteral(".b = %1;").arg(num(given(ql.b, 0))));
        w.line(m + QStringLiteral(".c11 = %1;").arg(num(given(ql.C11, 1.221e11))));
        w.line(m + QStringLiteral(".c12 = %1;").arg(num(given(ql.C12, 0.566e11))));
        w.line(m + QStringLiteral(".lc = %1;").arg(num(given(ql.a0, 5.65325e-10))));
        w.line(m + QStringLiteral(".vb = %1;").arg(num(ql.Ev)));
        w.line(m + QStringLiteral(".eps = %1;").arg(num(given(ql.epsr, ql.nr * ql.nr))));
        if (strained && (!std::isfinite(ql.ac) || !std::isfinite(ql.C11)))
            w.note(tr("domain %1 strained without its deformation potentials or stiffness: GaAs's stiffness, nought for the rest").arg(l.d + 1));
    }
    if (defaultLattice) w.note(tr("a layer without a lattice constant: GaAs's (lc sets only mqwindex's default range of k, given here)"));
    if (fromDatabase > 0) w.note(tr("%1 layers Lumerical's database's by compound and composition - their band offsets Lumerical's (Palankovski's)").arg(fromDatabase));
    w.line();
    // The field's potential: 0 at the stack's middle, as the model has it.
    w.comment(tr("The simulation: %1 K, the field %2 V/m (its potential 0 at the stack's middle: the bands at -q V).").arg(num(q.T), num(q.F)));
    w.line(QStringLiteral("params = struct;"));
    w.line(QStringLiteral("params.T = %1;").arg(num(q.T)));
    // (A study's sweep of the field: a solve for each.)
    const std::vector<double> fields = sweptValues(model, phys->text(QStringLiteral("F")), Dim::of(1, 1, -3, -1), sStudy ? sStudy : bStudy, w, phys->name());
    w.line(QStringLiteral("params.V = [0, %1; %2, %3];").arg(num(q.F * L / 2), num(L), num(-q.F * L / 2)));
    if (bands) {
        const double kmax = evaluateConstant(bands->text(QStringLiteral("kmax")), scope, Dim::of(-1)).value_or(1e9);
        w.line(QStringLiteral("params.kt = linspace(0, %1, %2);").arg(num(kmax)).arg(std::max(2, bands->integer(QStringLiteral("points")))));
    }
    // (A study's sweep of the density: mqwindex's several at once.)
    std::vector<double> densities = spectrum ? sweptValues(model, spectrum->text(QStringLiteral("density")), Dim::of(-3), sStudy, w, spectrum->name()) : std::vector<double>{density};
    if (densities.size() > 1) {
        QStringList v;
        for (double d : densities) v << num(wells > 0 ? d * wells / L : d);
        w.line(QStringLiteral("params.cden = [%1]; # (1/m3 over the stack: the study's sweep, the model's %2 ... in the wells, %3 of %4 nm)")
                   .arg(v.join(QStringLiteral(", ")), num(densities.front()), num(wells * 1e9), num(L * 1e9)));
    } else
        w.line(QStringLiteral("params.cden = %1; # (1/m3 over the stack: the model's %2 in the wells, %3 of %4 nm)").arg(num(cden), num(density), num(wells * 1e9), num(L * 1e9)));
    {
        // (Evenly spaced: a linspace.)
        const std::size_t n = frequencies.size();
        bool even = n > 2;
        for (std::size_t i = 1; even && i < n; ++i)
            even = std::abs(frequencies[i] - (frequencies[0] + (frequencies[n - 1] - frequencies[0]) * double(i) / double(n - 1))) <= 1e-9 * std::abs(frequencies[n - 1] - frequencies[0]);
        QStringList f;
        for (double v : frequencies) f << num(v);
        w.line(even ? QStringLiteral("params.phfreq = linspace(%1, %2, %3);").arg(num(frequencies.front()), num(frequencies.back())).arg(n)
                    : QStringLiteral("params.phfreq = [%1];").arg(f.join(QStringLiteral(", "))));
    }
    w.line();
    w.line(QStringLiteral("config = struct;"));
    w.line(QStringLiteral("config.dz = %1;").arg(num(std::max(1e-10, q.h1d))));
    if (q.h1d < 1e-10) w.note(tr("its element size %1 nm below mqwindex's least grid step: 0.1 nm").arg(num(q.h1d * 1e9)));
    const int eigen = std::max(bands ? 2 * bands->integer(QStringLiteral("states")) : 0, 4 * states);
    if (eigen > 0) w.line(QStringLiteral("config.numeigenvalues = %1;").arg(std::max(eigen, 10)));
    w.line(QStringLiteral("config.exciton = %1;").arg(excitons ? 1 : 0));
    w.line(QStringLiteral("config.cbvalley = \"Gamma\";"));
    w.line(QStringLiteral("config.bcs = struct;"));
    w.line(QStringLiteral("config.bcs.pmlactive = 0;"));
    w.line();
    const QString saved = base + (fields.size() > 1 ? QStringLiteral("_mqw_1.ldf ...") : QStringLiteral("_mqw.ldf"));
    const bool swept = fields.size() > 1;
    if (node.flag(QStringLiteral("run"))) {
        w.comment(tr("Lumerical's 4x4 k·p: the band diagram, the bands, the gain (TE, TM; 1/m over the stack) and the index."));
        QString indent;
        if (swept) {
            QStringList v;
            for (double x : fields) v << num(x);
            w.line(QStringLiteral("fields = [%1]; # (V/m: the study's sweep, a solve and a file for each)").arg(v.join(QStringLiteral(", "))));
            w.line(QStringLiteral("for (i = 1:length(fields)) {"));
            indent = QStringLiteral("    ");
            w.line(indent + QStringLiteral("params.V = [0, fields(i) * %1; %2, -fields(i) * %1];").arg(num(L / 2), num(L)));
        }
        w.line(indent + QStringLiteral("result = mqwindex(stack, params, config);"));
        w.line(indent + QStringLiteral("banddiagram = result.banddiagram;"));
        w.line(indent + QStringLiteral("bandstructure = result.bandstructure;"));
        w.line(indent + QStringLiteral("emission = result.emission;"));
        w.line(indent + QStringLiteral("index = result.index;"));
        if (swept) {
            w.line(indent + QStringLiteral("savedata(%1 + num2str(i), banddiagram, bandstructure, emission, index);").arg(str(base + QStringLiteral("_mqw_"))));
            w.line(QStringLiteral("}"));
        } else {
            w.line(QStringLiteral("savedata(%1, banddiagram, bandstructure, emission, index);").arg(str(base + QStringLiteral("_mqw"))));
        }
        w.line(QStringLiteral("visualize(emission);"));
    } else {
        w.comment(tr("Then: result = mqwindex(stack, params, config);"));
    }
    out.text = compose(header(QStringLiteral("MQW"), model, base, version, node.flag(QStringLiteral("run")) ? saved : QString(),
                              node.flag(QStringLiteral("run")) ? tr("solves the stack with mqwindex") : tr("sets up the stack for mqwindex"),
                              tr("any Lumerical product with an MQW license")),
                       w);
    out.notes = w.notes;
    out.summary = tr("an MQW script: %1 layers (%2 nm), %3 photon energies%4").arg(layers.size()).arg(num(L * 1e9)).arg(frequencies.size())
                      .arg(fields.size() > 1 ? tr(", %1 fields").arg(fields.size()) : QString());
    return out;
}

} // namespace

namespace lumerical {

QString libraryOf(const QString& lumerical, bool electrical)
{
    const QString name = lumerical.trimmed();
    for (const Names& n : Library) {
        const QString theirs = QString::fromLatin1(electrical ? n.ct : n.em);
        if (!theirs.isEmpty() && name.compare(theirs, Qt::CaseInsensitive) == 0) return QString::fromLatin1(n.ours);
    }
    // (Another source of one of them: "Si (Silicon) - Li", "SiO2 (Glass) - Malitson".)
    static const std::pair<const char*, const char*> prefixes[] = {
        {"Si (Silicon)", "Silicon"},       {"SiO2", "Silicon dioxide"},     {"Si3N4", "Silicon nitride"},
        {"Ge (Germanium)", "Germanium"},   {"GaAs", "Gallium arsenide"},    {"InP", "Indium phosphide"},
        {"GaN", "Gallium nitride"},        {"LiNbO3", "Lithium niobate"},   {"Al2O3", "Alumina (96%)"},
        {"Al (Aluminium)", "Aluminium"},   {"Al (Aluminum)", "Aluminium"},  {"Cu (Copper)", "Copper"},
        {"Au (Gold)", "Gold"},             {"Ag (Silver)", "Silver"},       {"H2O (Water)", "Water"},
        {"Air", "Air"},                    {"etch", "Air"}};
    for (const auto& [theirs, ours] : prefixes)
        if (name.startsWith(QLatin1String(theirs), Qt::CaseInsensitive)
            && (name.size() == int(std::strlen(theirs)) || !name.at(int(std::strlen(theirs))).isLetterOrNumber()))
            return QString::fromLatin1(ours);
    return QString();
}

QString compoundOf(const QString& lumerical, double x, double y, double* cx, double* cy)
{
    const QString name = lumerical.section(QLatin1Char(' '), 0, 0).trimmed();
    *cx = x;
    *cy = y;
    static const QStringList same = {QStringLiteral("GaAs"),   QStringLiteral("AlAs"),  QStringLiteral("InAs"),   QStringLiteral("InP"),
                                     QStringLiteral("GaP"),    QStringLiteral("GaSb"),  QStringLiteral("AlSb"),   QStringLiteral("InSb"),
                                     QStringLiteral("GaN"),    QStringLiteral("AlN"),   QStringLiteral("InN"),    QStringLiteral("AlGaAs"),
                                     QStringLiteral("InGaAs"), QStringLiteral("InAlAs"), QStringLiteral("GaAsP"), QStringLiteral("InAsP"),
                                     QStringLiteral("GaAsSb"), QStringLiteral("AlGaN"), QStringLiteral("InGaN"),  QStringLiteral("AlGaInAs")};
    if (same.contains(name)) return name;
    // (Lumerical's In x Ga 1-x P and In x Ga 1-x As y P 1-y, Al x In 1-x N;
    // ours Ga x In 1-x P, In 1-x Ga x As y P 1-y, In x Al 1-x N.)
    if (name == QLatin1String("InGaP") || name == QLatin1String("GaInP")) {
        *cx = name == QLatin1String("InGaP") ? 1 - x : x;
        return QStringLiteral("GaInP");
    }
    if (name == QLatin1String("InGaAsP")) {
        *cx = 1 - x;
        return QStringLiteral("InGaAsP");
    }
    if (name == QLatin1String("AlInN")) {
        *cx = 1 - x;
        return QStringLiteral("InAlN");
    }
    return QString();
}

} // namespace lumerical

LumericalScript lumericalScript(const Model& model, const Node& node, const QString& base, const QString& version)
{
    if (node.type == QLatin1String("lumericalmode")) return exportMode(model, node, base, version);
    if (node.type == QLatin1String("lumericalcharge")) return exportCharge(model, node, base, version);
    if (node.type == QLatin1String("lumericalmqw")) return exportMqw(model, node, base, version);
    LumericalScript out;
    out.error = tr("%1 is not a Lumerical export").arg(node.name());
    return out;
}

} // namespace qucs_s::fem
