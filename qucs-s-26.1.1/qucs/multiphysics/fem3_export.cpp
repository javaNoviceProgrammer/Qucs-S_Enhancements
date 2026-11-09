/*
 * fem3_export.cpp - a 3D model's exports: its fields for ParaView (VTU, a PVD
 *                   collection of them over output times and a sweep's
 *                   points), and Field on a Grid in 3D (CSV, NumPy, columns)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem3_export.h"

#include "fem3_geometry.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace qucs_s::fem {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("Export3", text);
}

/// NumPy's .npy (format 1.0) of \a values, doubles little-endian in C's
/// order, of \a shape.
QByteArray npy(const std::vector<double>& values, const std::vector<std::size_t>& shape)
{
    QByteArray dims;
    for (std::size_t i = 0; i < shape.size(); ++i) dims += (i ? ", " : "") + QByteArray::number(qulonglong(shape[i]));
    if (shape.size() == 1) dims += ',';
    QByteArray header = "{'descr': '<f8', 'fortran_order': False, 'shape': (" + dims + "), }";
    // (Magic, version, its length, the header and a newline: a multiple of 64.)
    const int pad = (64 - (10 + header.size() + 1) % 64) % 64;
    header += QByteArray(pad, ' ') + '\n';
    QByteArray out("\x93NUMPY\x01\x00", 8);
    out += char(header.size() & 0xff);
    out += char((header.size() >> 8) & 0xff);
    out += header;
    out.reserve(out.size() + qsizetype(8 * values.size()));
    for (double v : values) {
        quint64 bits;
        std::memcpy(&bits, &v, 8);
        for (int b = 0; b < 8; ++b) out += char((bits >> (8 * b)) & 0xff);
    }
    return out;
}

/// A number as a file has it: nan where it is not one.
QByteArray number(double v)
{
    return std::isfinite(v) ? QByteArray::number(v, 'g', 12) : QByteArrayLiteral("nan");
}

/// The expressions of an export, each named ("E = emw.normE"), its own
/// text its name where it has none.
std::vector<NamedExpression> itemsOf(const Node& node, QString* error)
{
    std::vector<NamedExpression> items;
    for (const QString& raw : node.text(QStringLiteral("expressions")).split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        NamedExpression item = namedExpression(raw);
        if (item.expression.isEmpty()) continue;
        if (item.name.isEmpty()) item.name = item.expression;
        items.push_back(item);
    }
    if (items.empty() && error) *error = tr("give it an expression (ht.T; es.normE...)");
    return items;
}

/// Its solutions: the one asked - its point and time -, or each.
std::vector<Instance3> instancesOf(const Solutions3& set, const Node& node, QString* error)
{
    const QString which = node.text(QStringLiteral("solutions"));
    const bool one = which == QLatin1String("selected") || which.isEmpty();
    QString asked;
    std::vector<Instance3> at = one ? instances3(set, false, node.integer(QStringLiteral("point")), node.text(QStringLiteral("time")), &asked)
                                    : instances3(set, true);
    if (at.empty() && error) *error = asked.isEmpty() ? tr("no solution to export") : tr("its %1").arg(asked);
    return at;
}

/// A prism's corners as VTK takes a wedge: its floor turned so its
/// right-hand normal points away from its top (the floor clockwise seen
/// from above), its top above them.
std::array<int, 6> vtkWedge(const Mesh3& m, int p)
{
    const std::array<int, 6>& c = m.prisms[std::size_t(p)];
    const Vec3& a = m.nodes[std::size_t(c[0])];
    const Vec3& b = m.nodes[std::size_t(c[1])];
    const Vec3& d = m.nodes[std::size_t(c[2])];
    const double turn = (b.x - a.x) * (d.y - a.y) - (b.y - a.y) * (d.x - a.x);
    if (turn > 0) return {c[0], c[2], c[1], c[3], c[5], c[4]};
    return c;
}

/// The reference coordinates (ξ, η, ζ) of a prism's corner, Mesh3's order.
constexpr double Corner[6][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}};

/// VTK's quadratic wedge's edges after its six corners, as corners of
/// vtkWedge's: (0 1) (1 2) (2 0), (3 4) (4 5) (5 3), (0 3) (1 4) (2 5).
constexpr int WedgeEdge[9][2] = {{0, 1}, {1, 2}, {2, 0}, {3, 4}, {4, 5}, {5, 3}, {0, 3}, {1, 4}, {2, 5}};

} // namespace

QByteArray vtu3(const Solution3& s, const std::vector<NamedExpression>& items, bool quadratic, QString* error)
{
    const Mesh3& m = s.mesh();
    std::vector<Expression> exprs;
    for (const NamedExpression& it : items) {
        const Expression e = Expression::compile(it.expression, s.scope());
        if (!e.isValid()) {
            if (error) *error = tr("%1: %2").arg(it.expression, e.error());
            return {};
        }
        exprs.push_back(e);
    }
    // Its points: the mesh's nodes, then (quadratic) each edge's middle once,
    // by its two nodes; each expression at each.
    std::vector<Vec3> points(m.nodes.begin(), m.nodes.end());
    std::vector<std::vector<double>> values(exprs.size());
    for (std::size_t k = 0; k < exprs.size(); ++k) values[k] = nodalValues(s, exprs[k]);
    const Space3* sp = nullptr;
    for (const Field3& f : s.fields())
        if (f.space && (!sp || f.space->order() > sp->order())) sp = f.space.get();
    PointEvaluator3 ev(s);
    std::map<std::pair<int, int>, int> middle;
    std::vector<long long> connectivity;
    connectivity.reserve(m.prisms.size() * (quadratic ? 15 : 6));
    for (int p = 0; p < int(m.prisms.size()); ++p) {
        const std::array<int, 6> w = vtkWedge(m, p);
        for (int n : w) connectivity.push_back(n);
        if (!quadratic) continue;
        const std::array<int, 6>& c = m.prisms[std::size_t(p)];
        auto local = [&c](int node) {
            for (int i = 0; i < 6; ++i)
                if (c[std::size_t(i)] == node) return i;
            return 0;
        };
        for (const auto& e : WedgeEdge) {
            const int a = w[std::size_t(e[0])], b = w[std::size_t(e[1])];
            const std::pair<int, int> key = std::minmax(a, b);
            auto it = middle.find(key);
            if (it == middle.end()) {
                const int at = int(points.size());
                it = middle.emplace(key, at).first;
                const int la = local(a), lb = local(b);
                const double xi = 0.5 * (Corner[la][0] + Corner[lb][0]), eta = 0.5 * (Corner[la][1] + Corner[lb][1]),
                             zeta = 0.5 * (Corner[la][2] + Corner[lb][2]);
                Vec3 x;
                if (sp) x = sp->map(p, xi, eta, zeta).x;
                else {
                    const Vec3& pa = m.nodes[std::size_t(a)];
                    const Vec3& pb = m.nodes[std::size_t(b)];
                    x = Vec3{0.5 * (pa.x + pb.x) * m.unitScale, 0.5 * (pa.y + pb.y) * m.unitScale, 0.5 * (pa.z + pb.z) * m.unitScale};
                }
                points.push_back(Vec3{x.x / m.unitScale, x.y / m.unitScale, x.z / m.unitScale});
                ev.at(p, xi, eta, zeta, x);
                for (std::size_t k = 0; k < exprs.size(); ++k) values[k].push_back(ev.eval(exprs[k]));
            }
            connectivity.push_back(it->second);
        }
    }
    QByteArray out;
    out.reserve(qsizetype(points.size() * (40 + 14 * exprs.size()) + connectivity.size() * 8));
    out += "<?xml version=\"1.0\"?>\n"
           "<VTKFile type=\"UnstructuredGrid\" version=\"1.0\" byte_order=\"LittleEndian\" header_type=\"UInt64\">\n"
           "  <UnstructuredGrid>\n";
    out += "    <Piece NumberOfPoints=\"" + QByteArray::number(qulonglong(points.size())) + "\" NumberOfCells=\""
           + QByteArray::number(qulonglong(m.prisms.size())) + "\">\n";
    out += "      <PointData>\n";
    for (std::size_t k = 0; k < exprs.size(); ++k) {
        out += "        <DataArray type=\"Float64\" Name=\"" + items[k].name.toHtmlEscaped().toUtf8() + "\" format=\"ascii\">\n";
        for (std::size_t i = 0; i < values[k].size(); ++i) out += number(values[k][i]) + ((i + 1) % 6 == 0 || i + 1 == values[k].size() ? "\n" : " ");
        out += "        </DataArray>\n";
    }
    out += "      </PointData>\n      <CellData>\n        <DataArray type=\"Int32\" Name=\"dom\" format=\"ascii\">\n";
    for (std::size_t p = 0; p < m.prisms.size(); ++p)
        out += QByteArray::number(m.domain[p] + 1) + ((p + 1) % 20 == 0 || p + 1 == m.prisms.size() ? "\n" : " ");
    out += "        </DataArray>\n      </CellData>\n      <Points>\n        <DataArray type=\"Float64\" NumberOfComponents=\"3\" format=\"ascii\">\n";
    for (const Vec3& x : points) out += number(x.x) + ' ' + number(x.y) + ' ' + number(x.z) + '\n';
    out += "        </DataArray>\n      </Points>\n      <Cells>\n        <DataArray type=\"Int64\" Name=\"connectivity\" format=\"ascii\">\n";
    const std::size_t per = quadratic ? 15 : 6;
    for (std::size_t i = 0; i < connectivity.size(); ++i) out += QByteArray::number(connectivity[i]) + ((i + 1) % per == 0 ? "\n" : " ");
    out += "        </DataArray>\n        <DataArray type=\"Int64\" Name=\"offsets\" format=\"ascii\">\n";
    for (std::size_t p = 0; p < m.prisms.size(); ++p)
        out += QByteArray::number(qulonglong((p + 1) * per)) + ((p + 1) % 20 == 0 || p + 1 == m.prisms.size() ? "\n" : " ");
    out += "        </DataArray>\n        <DataArray type=\"UInt8\" Name=\"types\" format=\"ascii\">\n";
    for (std::size_t p = 0; p < m.prisms.size(); ++p) out += (quadratic ? "26" : "13") + QByteArray((p + 1) % 40 == 0 || p + 1 == m.prisms.size() ? "\n" : " ");
    out += "        </DataArray>\n      </Cells>\n    </Piece>\n  </UnstructuredGrid>\n</VTKFile>\n";
    return out;
}

namespace {

/// A VTU export: one solution's file, or each's beside a PVD collection of
/// them by time (and part, a sweep's point).
Export3 vtuExport(const Solutions3& set, const Node& node, const QString& base)
{
    Export3 out;
    QString why;
    const std::vector<NamedExpression> items = itemsOf(node, &why);
    if (items.empty()) {
        out.main.error = why;
        return out;
    }
    const std::vector<Instance3> at = instancesOf(set, node, &why);
    if (at.empty()) {
        out.main.error = why;
        return out;
    }
    // Quadratic wedges where asked, or (automatic) where a field is of order 2.
    const QString order = node.text(QStringLiteral("order"));
    auto quadraticFor = [&order](const Solution3& s) {
        if (order == QLatin1String("linear")) return false;
        if (order == QLatin1String("quadratic")) return true;
        for (const Field3& f : s.fields())
            if (f.space && f.space->order() == 2) return true;
        return false;
    };
    QStringList names;
    for (const NamedExpression& it : items) names << it.name;
    if (at.size() == 1) {
        Solution3* s = select3(set, at.front());
        const bool quadratic = quadraticFor(*s);
        out.main.data = vtu3(*s, items, quadratic, &why);
        if (out.main.data.isEmpty()) {
            out.main.error = why;
            return out;
        }
        out.main.suffix = QStringLiteral("vtu");
        out.main.summary = tr("%1 at %2 points of %3 %4 (points in %5)")
                               .arg(names.join(QLatin1String(", ")))
                               .arg(s->mesh().nodes.size())
                               .arg(s->mesh().prisms.size())
                               .arg(quadratic ? tr("quadratic wedges") : tr("wedges"), s->topology().unitName.isEmpty() ? QStringLiteral("m") : s->topology().unitName);
        return out;
    }
    QByteArray pvd = "<?xml version=\"1.0\"?>\n<VTKFile type=\"Collection\" version=\"0.1\" byte_order=\"LittleEndian\">\n  <Collection>\n";
    const bool swept = set.size() > 1;
    for (std::size_t i = 0; i < at.size(); ++i) {
        Solution3* s = select3(set, at[i]);
        const QByteArray one = vtu3(*s, items, quadraticFor(*s), &why);
        if (one.isEmpty()) {
            out.main.error = at[i].label.isEmpty() ? why : at[i].label + QStringLiteral(": ") + why;
            return out;
        }
        const QString end = QStringLiteral("_%1.vtu").arg(int(i), 4, 10, QLatin1Char('0'));
        out.parts.emplace_back(end, one);
        // (A time series's time; a sweep's points each a part of its own, at
        // its number where it has no time.)
        const double step = at[i].snapshot >= 0 ? at[i].time : (swept ? double(at[i].point) : double(i));
        pvd += "    <DataSet timestep=\"" + QByteArray::number(step, 'g', 12) + "\" group=\"\" part=\"" + QByteArray::number(swept && at[i].snapshot >= 0 ? at[i].point : 0)
               + "\" file=\"" + (base + end).toHtmlEscaped().toUtf8() + "\"/>\n";
    }
    pvd += "  </Collection>\n</VTKFile>\n";
    out.main.data = pvd;
    out.main.suffix = QStringLiteral("pvd");
    out.main.summary = tr("%1 at %2 solutions, a VTU each beside it (%3_0000.vtu ...)").arg(names.join(QLatin1String(", "))).arg(at.size()).arg(base);
    return out;
}

/// A Field on a Grid export in 3D: its expressions at each of its
/// instances, on a regular grid of x, y and z or at the mesh's nodes.
Export3 fieldExport3(const Solutions3& set, const Node& node)
{
    Export3 out;
    ExportText& t = out.main;
    QString why;
    const std::vector<NamedExpression> items = itemsOf(node, &why);
    if (items.empty()) {
        t.error = why;
        return out;
    }
    const std::vector<Instance3> at = instancesOf(set, node, &why);
    if (at.empty()) {
        t.error = why;
        return out;
    }
    const Solution3& first = *set[std::size_t(at.front().point)];
    const Mesh3& mesh = first.mesh();
    const double scale = mesh.unitScale;
    const QString unit = first.topology().unitName.isEmpty() ? QStringLiteral("m") : first.topology().unitName;
    const bool grid = node.text(QStringLiteral("where")) != QLatin1String("nodes");
    std::vector<Vec3> points;   // the geometry's unit
    int nx = 0, ny = 0, nz = 0;
    if (grid) {
        nx = std::clamp(node.integer(QStringLiteral("nx")), 1, 2000);
        ny = std::clamp(node.integer(QStringLiteral("ny")), 1, 2000);
        nz = std::clamp(node.integer(QStringLiteral("nz")), 1, 2000);
        if (qint64(nx) * ny * nz > 8000000) {
            t.error = tr("%1 by %2 by %3 points: eight million at most").arg(nx).arg(ny).arg(nz);
            return out;
        }
        // Its corners: the model's extent, where none is given.
        const Box3 b = first.topology().bounds;
        double lo[3] = {b.x0, b.y0, b.z0}, hi[3] = {b.x1, b.y1, b.z1};
        const QStringList from = node.pair(QStringLiteral("from")), to = node.pair(QStringLiteral("to")), z = node.pair(QStringLiteral("zrange"));
        Expression::Options options;
        options.lengthUnit = scale;
        options.numbersAreLengths = true;
        const QString texts[6] = {from.value(0), from.value(1), z.value(0), to.value(0), to.value(1), z.value(1)};
        for (int c = 0; c < 6; ++c) {
            if (texts[c].trimmed().isEmpty()) continue;
            const Expression e = Expression::compile(texts[c], first.parameters().scope, options);
            if (!e.isValid() || !e.isConstant()) {
                t.error = tr("the corner %1: %2").arg(texts[c], e.isValid() ? tr("not a constant") : e.error());
                return out;
            }
            (c < 3 ? lo[c] : hi[c - 3]) = e.constant();
        }
        const int n[3] = {nx, ny, nz};
        for (int k = 0; k < 3; ++k)
            if (!(n[k] == 1 ? hi[k] >= lo[k] : hi[k] > lo[k])) {
                t.error = tr("its corners: the second beyond the first along each axis (a grid of one point along one: the same coordinate)");
                return out;
            }
        auto along = [](double a, double b, int count, int i) { return count == 1 ? 0.5 * (a + b) : a + (b - a) * i / (count - 1); };
        for (int l = 0; l < nz; ++l)
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i) points.push_back(Vec3{along(lo[0], hi[0], nx, i), along(lo[1], hi[1], ny, j), along(lo[2], hi[2], nz, l)});
    } else {
        points = mesh.nodes;
        for (const Instance3& in : at)
            if (set[std::size_t(in.point)]->mesh().nodes.size() != points.size()) {
                t.error = tr("the mesh is not the same at each solution (a swept geometry): export them on a grid, or one alone");
                return out;
            }
    }
    // values[(i K + k) N + n]: instance i, expression k, point n.
    const std::size_t N = points.size(), K = items.size(), I = at.size();
    std::vector<double> values(I * K * N, std::nan(""));
    QStringList names;
    for (std::size_t i = 0; i < I; ++i) {
        Solution3* s = select3(set, at[i]);
        PointEvaluator3 ev(*s);
        for (std::size_t k = 0; k < K; ++k) {
            const Expression e = Expression::compile(items[k].expression, s->scope());
            if (!e.isValid()) {
                t.error = tr("%1: %2").arg(items[k].expression, e.error());
                return out;
            }
            names << (I > 1 && !at[i].label.isEmpty() ? at[i].label + QStringLiteral(": ") : QString()) + items[k].name
                         + (e.dim().isNone() ? QString() : QStringLiteral(" (%1)").arg(dimName(e.dim())));
            if (!grid) {
                const std::vector<double> nodal = nodalValues(*s, e);
                for (std::size_t n = 0; n < N && n < nodal.size(); ++n) values[(i * K + k) * N + n] = nodal[n];
                continue;
            }
            for (std::size_t n = 0; n < N; ++n) {
                const Vec3& p = points[n];
                if (ev.at(Vec3{p.x * scale, p.y * scale, p.z * scale})) values[(i * K + k) * N + n] = ev.eval(e);
            }
        }
    }
    const QString format = node.text(QStringLiteral("format"));
    const QString where = grid ? tr("a %1 by %2 by %3 grid").arg(nx).arg(ny).arg(nz) : tr("the mesh's %1 nodes").arg(N);
    if (format == QLatin1String("npy")) {
        // A grid: (instances,) expressions, z, y, x, its points' x, y, z
        // beside it; nodes: rows of x, y, z, then each value.
        t.suffix = QStringLiteral("npy");
        if (grid) {
            std::vector<std::size_t> shape;
            if (I > 1) shape.push_back(I);
            shape.insert(shape.end(), {K, std::size_t(nz), std::size_t(ny), std::size_t(nx)});
            t.data = npy(values, shape);
            std::vector<double> xyz(3 * N);
            for (std::size_t n = 0; n < N; ++n) {
                xyz[n] = points[n].x;
                xyz[N + n] = points[n].y;
                xyz[2 * N + n] = points[n].z;
            }
            t.coordinates = npy(xyz, {3, std::size_t(nz), std::size_t(ny), std::size_t(nx)});
            t.summary = tr("%1 at %2 (shape %3; x, y, z in %4 in its _xy.npy beside it, shape (3, %5, %6, %7))")
                            .arg(names.join(QLatin1String(", ")), where,
                                 QStringLiteral("(%1%2, %3, %4, %5)").arg(I > 1 ? QString::number(I) + QStringLiteral(", ") : QString()).arg(K).arg(nz).arg(ny).arg(nx),
                                 unit)
                            .arg(nz)
                            .arg(ny)
                            .arg(nx);
        } else {
            std::vector<double> rows;
            rows.reserve(N * (3 + I * K));
            for (std::size_t n = 0; n < N; ++n) {
                rows.insert(rows.end(), {points[n].x, points[n].y, points[n].z});
                for (std::size_t v = 0; v < I * K; ++v) rows.push_back(values[v * N + n]);
            }
            t.data = npy(rows, {N, 3 + I * K});
            t.summary = tr("x, y, z (%1), %2 at %3 (shape (%4, %5))").arg(unit, names.join(QLatin1String(", ")), where).arg(N).arg(3 + I * K);
        }
        return out;
    }
    // CSV: a row a point; columns: blank lines after each row of x, two
    // after each plane of z (gnuplot's).
    const bool csv = format != QLatin1String("dat");
    t.suffix = csv ? QStringLiteral("csv") : QStringLiteral("dat");
    QByteArray text;
    const QByteArray sep = csv ? "," : " ";
    text += csv ? QByteArray() : QByteArrayLiteral("# ");
    text += "x (" + unit.toUtf8() + ")" + sep + "y (" + unit.toUtf8() + ")" + sep + "z (" + unit.toUtf8() + ")";
    for (const QString& nm : names) text += sep + (csv && nm.contains(QLatin1Char(',')) ? '"' + nm.toUtf8() + '"' : nm.toUtf8());
    text += '\n';
    for (std::size_t n = 0; n < N; ++n) {
        text += number(points[n].x) + sep + number(points[n].y) + sep + number(points[n].z);
        for (std::size_t v = 0; v < I * K; ++v) text += sep + number(values[v * N + n]);
        text += '\n';
        if (!csv && grid && (n + 1) % std::size_t(nx) == 0) text += (n + 1) % (std::size_t(nx) * std::size_t(ny)) == 0 ? "\n\n" : "\n";
    }
    t.text = QString::fromUtf8(text);
    t.summary = tr("%1 at %2 (x, y, z in %3)").arg(names.join(QLatin1String(", ")), where, unit);
    return out;
}

} // namespace

/// An eigenmode expansion's S-matrix at each wavelength of a point of the
/// sweep (its "point", from 1; 0 the last), a Touchstone file.
Export3 touchstone3(const Solutions3& set, const Node& node)
{
    Export3 out;
    if (set.empty()) {
        out.main.error = tr("nothing is solved");
        return out;
    }
    const int n = int(set.size()), point = node.integer(QStringLiteral("point"));
    const Solution3& s = *set[std::size_t(point <= 0 || point > n ? n - 1 : point - 1)];
    const QStringList chosen = node.list(QStringLiteral("optics"));
    const Solution3::Optics* o = nullptr;
    for (const Solution3::Optics& c : s.optics)
        if (chosen.isEmpty() || chosen.contains(c.physics)) {
            o = &c;
            break;
        }
    if (!o) {
        out.main.error = chosen.isEmpty() ? tr("no light is solved: its Touchstone file is of an Electromagnetic Waves, Eigenmode Expansion's, "
                                               "Frequency Domain's or Time Domain's S-matrix, solved by a Wavelength Domain step")
                                          : tr("there is no eigenmode expansion or full-wave solve %1 solved").arg(chosen.join(QLatin1String(", ")));
        return out;
    }
    QStringList comments{tr("%1, %2 of %3").arg(o->physics, s.studyTag, s.model().fileName.isEmpty() ? tr("a model") : s.model().fileName)};
    if (!s.label.isEmpty()) comments << s.label;
    int ports = 0;
    QString why;
    out.main.text = emeTouchstone(o->device, o->results, comments, &ports, &why);
    if (out.main.text.isEmpty()) {
        out.main.error = why;
        return out;
    }
    out.main.suffix = QStringLiteral("s%1p").arg(ports);
    out.main.summary = tr("%1 ports' S-matrix at %2 wavelengths (%3 to %4), Touchstone 1.1")
                           .arg(ports)
                           .arg(o->results.size())
                           .arg(formatQuantity(299792458.0 / o->results.front().frequency, Dim::length()),
                                formatQuantity(299792458.0 / o->results.back().frequency, Dim::length()));
    return out;
}

Export3 export3(const Solutions3& set, const Node& node, const QString& base)
{
    if (node.type == QLatin1String("vtu")) return vtuExport(set, node, base);
    if (node.type == QLatin1String("fieldexport")) return fieldExport3(set, node);
    if (node.type == QLatin1String("touchstone")) return touchstone3(set, node);
    Export3 out;
    out.main.error = tr("a %1 is not exported from a 3D model").arg(node.kind() ? node.kind()->title : node.type);
    return out;
}

} // namespace qucs_s::fem
