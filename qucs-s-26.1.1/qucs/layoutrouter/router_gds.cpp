/*
 * router_gds.cpp - routes committed to a layout as their own cell
 *                  (router_gds.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "router_gds.h"

#include <gdstk/gdstk.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointF>
#include <QTemporaryFile>

namespace qucs_s::router {

QVector<DrawnShape> drawnShapes(const Problem& p, const NetResult& r, int net)
{
    QVector<DrawnShape> out;
    const auto box = [&](const LayerId& layer, const Box& b) {
        DrawnShape s;
        s.layer = layer;
        s.points = {QPointF(p.um(b.x0), p.um(b.y0)), QPointF(p.um(b.x1), p.um(b.y0)), QPointF(p.um(b.x1), p.um(b.y1)), QPointF(p.um(b.x0), p.um(b.y1))};
        out.append(s);
    };
    const Coord w = p.units(p.tech.classes.at(p.nets.at(net).netClass).width);
    for (const Run& run : r.runs)
        if (run.metal >= 0 && run.metal < p.tech.metals.size()) box(p.tech.metals.at(run.metal).layer, run.box());
    for (const auto& [metal, poly] : r.fills) {
        if (metal < 0 || metal >= p.tech.metals.size()) continue;
        DrawnShape s;
        s.layer = p.tech.metals.at(metal).layer;
        for (const Pt& q : poly) s.points.append(QPointF(p.um(q.x), p.um(q.y)));
        out.append(s);
    }
    for (const ViaPlaced& v : r.vias) {
        if (v.via < 0 || v.via >= p.tech.vias.size()) continue;
        const Via& via = p.tech.vias.at(v.via);
        const Coord lo = std::max(p.units(via.landingLower), w), up = std::max(p.units(via.landingUpper), w), c = p.units(via.size);
        box(p.tech.metals.at(via.lower).layer, Box{v.at.x - lo / 2, v.at.y - lo / 2, v.at.x - lo / 2 + lo, v.at.y - lo / 2 + lo});
        box(p.tech.metals.at(via.lower + 1).layer, Box{v.at.x - up / 2, v.at.y - up / 2, v.at.x - up / 2 + up, v.at.y - up / 2 + up});
        // Its cuts: one, or an array of them centred on the via.
        const Coord gap = p.units(via.cutSpacing);
        const Coord x0 = v.at.x - p.units(via.outlineWidth()) / 2, y0 = v.at.y - p.units(via.outlineHeight()) / 2;
        for (int i = 0; i < via.cols; ++i)
            for (int j = 0; j < via.rows; ++j) {
                const Coord x = x0 + i * (c + gap), y = y0 + j * (c + gap);
                box(via.layer, Box{x, y, x + c, y + c});
            }
    }
    return out;
}

namespace {

bool isOasis(const QString& path)
{
    const QString s = path.toLower();
    return s.endsWith(QLatin1String(".oas")) || s.endsWith(QLatin1String(".oasis"));
}

bool isGds(const QString& path)
{
    const QString s = path.toLower();
    return s.endsWith(QLatin1String(".gds")) || s.endsWith(QLatin1String(".gds2")) || s.endsWith(QLatin1String(".gdsii"));
}

bool fatal(gdstk::ErrorCode e)
{
    return int(e) >= int(gdstk::ErrorCode::ChecksumError);
}

} // namespace

bool isWritableLayout(const QString& path)
{
    return isGds(path) || isOasis(path);
}

bool isOasisFile(const QString& path)
{
    return isOasis(path);
}

bool readLibrary(const QString& path, gdstk::Library* lib, QString* error)
{
    gdstk::ErrorCode e = gdstk::ErrorCode::NoError;
    const QByteArray src = QFile::encodeName(path);
    *lib = isOasis(path) ? gdstk::read_oas(src.constData(), 0, 0, &e) : gdstk::read_gds(src.constData(), 0, 0, nullptr, &e);
    if (fatal(e)) {
        lib->free_all();
        *error = QStringLiteral("%1 could not be read (gdstk's error %2)").arg(QFileInfo(path).fileName()).arg(int(e));
        return false;
    }
    if (lib->unit <= 0) lib->unit = 1e-6;
    if (lib->precision <= 0) lib->precision = 1e-9;
    return true;
}

bool writeLibrary(gdstk::Library& lib, const QString& path, QString* error)
{
    // Written beside the target, then renamed over it.
    const QFileInfo target(path);
    if (!QDir().mkpath(target.absolutePath())) {
        *error = QStringLiteral("The folder %1 cannot be made").arg(target.absolutePath());
        return false;
    }
    const QString temporary = target.absolutePath() + QStringLiteral("/.") + target.fileName() + QStringLiteral(".routing");
    QFile::remove(temporary);
    const QByteArray tmp = QFile::encodeName(temporary);
    const gdstk::ErrorCode e = isOasis(path) ? lib.write_oas(tmp.constData(), 0.001, 6, 0) : lib.write_gds(tmp.constData(), 0, nullptr);
    if (fatal(e) || !QFileInfo::exists(temporary)) {
        QFile::remove(temporary);
        *error = QStringLiteral("%1 could not be written (gdstk's error %2)").arg(target.fileName()).arg(int(e));
        return false;
    }
    if (QFileInfo::exists(path) && !QFile::remove(path)) {
        QFile::remove(temporary);
        *error = QStringLiteral("%1 could not be replaced").arg(target.fileName());
        return false;
    }
    if (!QFile::rename(temporary, path)) {
        QFile::remove(temporary);
        *error = QStringLiteral("%1 could not be written").arg(target.fileName());
        return false;
    }
    return true;
}

namespace {

// Takes \a cell's contents away (its polygons, paths, texts, placements).
void empty(gdstk::Cell* cell)
{
    for (uint64_t i = 0; i < cell->polygon_array.count; ++i) {
        cell->polygon_array[i]->clear();
        gdstk::free_allocation(cell->polygon_array[i]);
    }
    cell->polygon_array.clear();
    for (uint64_t i = 0; i < cell->flexpath_array.count; ++i) {
        cell->flexpath_array[i]->clear();
        gdstk::free_allocation(cell->flexpath_array[i]);
    }
    cell->flexpath_array.clear();
    for (uint64_t i = 0; i < cell->robustpath_array.count; ++i) {
        cell->robustpath_array[i]->clear();
        gdstk::free_allocation(cell->robustpath_array[i]);
    }
    cell->robustpath_array.clear();
    for (uint64_t i = 0; i < cell->reference_array.count; ++i) {
        cell->reference_array[i]->clear();
        gdstk::free_allocation(cell->reference_array[i]);
    }
    cell->reference_array.clear();
    for (uint64_t i = 0; i < cell->label_array.count; ++i) {
        cell->label_array[i]->clear();
        gdstk::free_allocation(cell->label_array[i]);
    }
    cell->label_array.clear();
}

bool refersTo(const gdstk::Reference* r, const gdstk::Cell* cell, const QByteArray& name)
{
    if (r->type == gdstk::ReferenceType::Cell) return r->cell == cell;
    if (r->type == gdstk::ReferenceType::Name) return name == r->name;
    return false;
}

} // namespace

CommitReport commitRoutes(const Commit& commit, const Problem& problem, const Result& result)
{
    CommitReport report;
    const auto fail = [&](const QString& why) {
        report.error = why;
        return report;
    };
    if (!(isGds(commit.source) || isOasis(commit.source)))
        return fail(QStringLiteral("Routes are committed to a .gds or .oas file; %1 is neither (a .gds.gz can be saved as a .gds first)")
                        .arg(QFileInfo(commit.source).fileName()));
    if (!(isGds(commit.target) || isOasis(commit.target)))
        return fail(QStringLiteral("The file written is a .gds or an .oas; %1 is neither").arg(QFileInfo(commit.target).fileName()));
    if (!QFileInfo::exists(commit.source)) return fail(QStringLiteral("There is no file %1").arg(commit.source));
    const QByteArray cellName = commit.cell.toUtf8();
    if (cellName.isEmpty()) return fail(QStringLiteral("The route cell needs a name"));

    gdstk::Library lib = {};
    QString why;
    if (!readLibrary(commit.source, &lib, &why)) return fail(why);
    // The top cell: as named, else the largest cell no other places.
    gdstk::Cell* top = nullptr;
    if (!commit.topCell.isEmpty()) {
        top = lib.get_cell(commit.topCell.toUtf8().constData());
        if (!top) {
            lib.free_all();
            return fail(QStringLiteral("%1 has no cell %2").arg(QFileInfo(commit.source).fileName(), commit.topCell));
        }
    } else {
        gdstk::Array<gdstk::Cell*> tops = {};
        gdstk::Array<gdstk::RawCell*> rawTops = {};
        lib.top_level(tops, rawTops);
        uint64_t most = 0;
        for (uint64_t i = 0; i < tops.count; ++i) {
            if (QByteArray(tops[i]->name) == cellName) continue;
            const uint64_t size = tops[i]->polygon_array.count + tops[i]->reference_array.count;
            if (!top || size > most) {
                top = tops[i];
                most = size;
            }
        }
        tops.clear();
        rawTops.clear();
        if (!top) {
            lib.free_all();
            return fail(QStringLiteral("%1 has no top cell to place the routes in").arg(QFileInfo(commit.source).fileName()));
        }
    }
    if (QByteArray(top->name) == cellName) {
        lib.free_all();
        return fail(QStringLiteral("The route cell cannot be the cell routed (%1)").arg(commit.cell));
    }

    gdstk::Cell* cell = lib.get_cell(cellName.constData());
    report.replaced = cell != nullptr;
    if (commit.remove) {
        if (cell) {
            // Its placements, in every cell, then the cell.
            for (uint64_t c = 0; c < lib.cell_array.count; ++c) {
                gdstk::Cell* other = lib.cell_array[c];
                for (uint64_t i = 0; i < other->reference_array.count;) {
                    if (refersTo(other->reference_array[i], cell, cellName)) {
                        other->reference_array[i]->clear();
                        gdstk::free_allocation(other->reference_array[i]);
                        other->reference_array.remove(i);
                    } else {
                        ++i;
                    }
                }
            }
            for (uint64_t c = 0; c < lib.cell_array.count; ++c)
                if (lib.cell_array[c] == cell) {
                    lib.cell_array.remove(c);
                    break;
                }
            cell->free_all();
            gdstk::free_allocation(cell);
            cell = nullptr;
        }
    } else {
        if (cell) {
            empty(cell);
        } else {
            cell = static_cast<gdstk::Cell*>(gdstk::allocate_clear(sizeof(gdstk::Cell)));
            cell->init(cellName.constData());
            lib.cell_array.append(cell);
        }
        bool placed = false;
        for (uint64_t i = 0; i < top->reference_array.count; ++i) placed = placed || refersTo(top->reference_array[i], cell, cellName);
        if (!placed) {
            auto* ref = static_cast<gdstk::Reference*>(gdstk::allocate_clear(sizeof(gdstk::Reference)));
            ref->init(cell);
            top->reference_array.append(ref);
            report.placed = true;
        }
        // µm to the library's user unit.
        const double scale = 1e-6 / lib.unit;
        for (int n = 0; n < result.nets.size() && n < problem.nets.size(); ++n) {
            const NetResult& r = result.nets.at(n);
            if (r.status != NetResult::Status::Routed && r.status != NetResult::Status::Kept) continue;
            if (!commit.nets.isEmpty() && !commit.nets.contains(problem.nets.at(n).name)) continue;
            ++report.nets;
            for (const DrawnShape& s : drawnShapes(problem, r, n)) {
                auto* poly = static_cast<gdstk::Polygon*>(gdstk::allocate_clear(sizeof(gdstk::Polygon)));
                poly->tag = gdstk::make_tag(s.layer.layer, s.layer.datatype);
                for (const QPointF& q : s.points) poly->point_array.append(gdstk::Vec2{q.x() * scale, q.y() * scale});
                cell->polygon_array.append(poly);
                ++report.polygons;
            }
        }
    }

    const bool written = writeLibrary(lib, commit.target, &why);
    lib.free_all();
    if (!written) return fail(why);
    report.ok = true;
    return report;
}

} // namespace qucs_s::router
