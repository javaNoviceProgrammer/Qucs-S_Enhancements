/*
 * layout.cpp - a GDSII or OASIS layout, read for its viewer (layoutdoc.h)
 *              and Claude's tools: cells of shapes, paths and texts on
 *              layers, and placements of other cells; read with gdstk,
 *              drawn with QPainter, searched by place
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "layout.h"

#include <QBitArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QMutex>
#include <QPainter>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryFile>
#include <QXmlStreamReader>

#include <gdstk/gdstk.hpp>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace qucs_s::layout {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("Layout", text);
}

// Rectangles as boxes: QRectF's intersects() and united() leave out one of
// no width or height - a text's point, a line of a path.
bool meets(const QRectF& a, const QRectF& b)
{
    return a.left() <= b.right() && b.left() <= a.right() && a.top() <= b.bottom() && b.top() <= a.bottom();
}

struct Box {
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    bool empty = true;
    void add(double x, double y)
    {
        if (empty) {
            x1 = x2 = x;
            y1 = y2 = y;
            empty = false;
            return;
        }
        x1 = std::min(x1, x);
        x2 = std::max(x2, x);
        y1 = std::min(y1, y);
        y2 = std::max(y2, y);
    }
    void add(const QRectF& r)
    {
        add(r.left(), r.top());
        add(r.right(), r.bottom());
    }
    QRectF rect() const { return empty ? QRectF() : QRectF(QPointF(x1, y1), QPointF(x2, y2)); }
};

// ----------------------------------------------------------------------
// The file: what it is, and a path gdstk opens.

enum class Kind { Gds, GzippedGds, Oasis, Unknown };

Kind kindOf(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return Kind::Unknown;
    const QByteArray head = f.read(16);
    if (head.startsWith("%SEMI-OASIS\r\n")) return Kind::Oasis;
    if (head.size() >= 2 && uchar(head[0]) == 0x1f && uchar(head[1]) == 0x8b) return Kind::GzippedGds;
    // A HEADER record: 6 bytes, record 0x00, two-byte integers.
    if (head.size() >= 4 && head[0] == 0 && head[1] == 6 && head[2] == 0 && head[3] == 2) return Kind::Gds;
    return Kind::Unknown;
}

// The bytes of a gzipped file inflated into \a into.
bool gunzip(const QString& path, QFile& into, QString* error)
{
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly)) {
        *error = in.errorString();
        return false;
    }
    z_stream z{};
    if (inflateInit2(&z, 15 + 32) != Z_OK) {   // (+32: a gzip header)
        *error = tr("zlib could not be started.");
        return false;
    }
    QByteArray input;
    QByteArray output(1 << 18, Qt::Uninitialized);
    int status = Z_OK;
    while (status != Z_STREAM_END) {
        if (z.avail_in == 0) {
            input = in.read(1 << 18);
            if (input.isEmpty()) break;
            z.next_in = reinterpret_cast<Bytef*>(input.data());
            z.avail_in = uInt(input.size());
        }
        z.next_out = reinterpret_cast<Bytef*>(output.data());
        z.avail_out = uInt(output.size());
        status = inflate(&z, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) break;
        into.write(output.constData(), output.size() - z.avail_out);
    }
    inflateEnd(&z);
    if (status != Z_STREAM_END) {
        *error = tr("It is not whole: its compressed data ends early or is damaged.");
        return false;
    }
    into.flush();
    return true;
}

// gdstk opens files by a char* name with fopen(): on Windows in the
// local code page, which a name of other letters is not.
bool openableByName(const QString& path)
{
#ifdef Q_OS_WIN
    for (QChar c : path)
        if (c.unicode() > 127) return false;
#else
    Q_UNUSED(path);
#endif
    return true;
}

FILE* openFile(const QString& path, const char* mode)
{
#ifdef Q_OS_WIN
    const QString wideMode = QString::fromLatin1(mode);
    return _wfopen(reinterpret_cast<const wchar_t*>(path.utf16()), reinterpret_cast<const wchar_t*>(wideMode.utf16()));
#else
    return fopen(QFile::encodeName(path).constData(), mode);
#endif
}

// gdstk tells what it reads to one FILE* of its own (error_logger): one
// read at a time, what it said kept.
QMutex& readLock()
{
    static QMutex lock;
    return lock;
}

// ----------------------------------------------------------------------
// gdstk's library taken into ours.

Properties propertiesOf(const gdstk::Property* p)
{
    Properties result;
    for (; p != nullptr; p = p->next) {
        QString name = p->name != nullptr ? QString::fromUtf8(p->name) : QString();
        QStringList values;
        for (const gdstk::PropertyValue* v = p->value; v != nullptr; v = v->next) {
            switch (v->type) {
            case gdstk::PropertyType::UnsignedInteger: values << QString::number(v->unsigned_integer); break;
            case gdstk::PropertyType::Integer: values << QString::number(v->integer); break;
            case gdstk::PropertyType::Real: values << QString::number(v->real, 'g', 15); break;
            case gdstk::PropertyType::String: {
                qsizetype n = qsizetype(v->count);
                while (n > 0 && v->bytes[n - 1] == 0) --n;
                values << QString::fromUtf8(reinterpret_cast<const char*>(v->bytes), n);
                break;
            }
            }
        }
        // A GDSII property: its attribute and its text.
        if (name == QLatin1String("S_GDS_PROPERTY") && values.size() >= 2) {
            name = values.takeFirst();
        }
        result.append({name, values.join(QStringLiteral(", "))});
    }
    return result;
}

qint32 repetitionOf(Cell& cell, const gdstk::Repetition& r)
{
    Repetition out;
    switch (r.type) {
    case gdstk::RepetitionType::None: return -1;
    case gdstk::RepetitionType::Rectangular:
        out.columns = std::max<quint64>(1, r.columns);
        out.rows = std::max<quint64>(1, r.rows);
        out.v1 = QPointF(r.spacing.x, 0);
        out.v2 = QPointF(0, r.spacing.y);
        break;
    case gdstk::RepetitionType::Regular:
        out.columns = std::max<quint64>(1, r.columns);
        out.rows = std::max<quint64>(1, r.rows);
        out.v1 = QPointF(r.v1.x, r.v1.y);
        out.v2 = QPointF(r.v2.x, r.v2.y);
        break;
    case gdstk::RepetitionType::Explicit:
        out.type = Repetition::Type::Explicit;
        out.offsets.reserve(qsizetype(r.offsets.count + 1));
        out.offsets.append(QPointF(0, 0));
        for (uint64_t i = 0; i < r.offsets.count; ++i) out.offsets.append(QPointF(r.offsets[i].x, r.offsets[i].y));
        break;
    case gdstk::RepetitionType::ExplicitX:
    case gdstk::RepetitionType::ExplicitY:
        out.type = Repetition::Type::Explicit;
        out.offsets.reserve(qsizetype(r.coords.count + 1));
        out.offsets.append(QPointF(0, 0));
        for (uint64_t i = 0; i < r.coords.count; ++i)
            out.offsets.append(r.type == gdstk::RepetitionType::ExplicitX ? QPointF(r.coords[i], 0) : QPointF(0, r.coords[i]));
        break;
    }
    cell.repetitions.append(out);
    return qint32(cell.repetitions.size() - 1);
}

qint32 propertiesIndex(Cell& cell, const gdstk::Property* p)
{
    if (p == nullptr) return -1;
    cell.properties.append(propertiesOf(p));
    return qint32(cell.properties.size() - 1);
}

QString endsOf(gdstk::EndType type)
{
    switch (type) {
    case gdstk::EndType::Flush: return QStringLiteral("flush");
    case gdstk::EndType::Round:
    case gdstk::EndType::Smooth: return QStringLiteral("round");
    case gdstk::EndType::HalfWidth: return QStringLiteral("extended by half its width");
    case gdstk::EndType::Extended: return QStringLiteral("extended");
    case gdstk::EndType::Function: return QStringLiteral("custom");
    }
    return QString();
}

// The layers by their tags, numbered once all are known.
struct Layers {
    QHash<gdstk::Tag, quint32> byTag;   // the order they were met; numbered later
    QVector<gdstk::Tag> tags;
    quint32 of(gdstk::Tag tag)
    {
        auto it = byTag.constFind(tag);
        if (it != byTag.constEnd()) return it.value();
        const quint32 i = quint32(tags.size());
        byTag.insert(tag, i);
        tags.append(tag);
        return i;
    }
};

void addOutline(Cell& cell, Shape& shape, const gdstk::Array<gdstk::Vec2>& points)
{
    shape.first = quint32(cell.points.size());
    shape.count = quint32(points.count);
    Box box;
    for (uint64_t i = 0; i < points.count; ++i) {
        cell.points.append(QPointF(points[i].x, points[i].y));
        box.add(points[i].x, points[i].y);
    }
    shape.bounds = box.rect();
}

void takeCell(const gdstk::Cell& from, Cell& cell, const QHash<const gdstk::Cell*, int>& index, Layers& layers,
              QSet<QString>& missing, QStringList& warnings)
{
    cell.name = QString::fromUtf8(from.name);
    cell.own = propertiesOf(from.properties);
    for (uint64_t i = 0; i < from.polygon_array.count; ++i) {
        const gdstk::Polygon* p = from.polygon_array[i];
        if (p->point_array.count == 0) continue;
        Shape s;
        addOutline(cell, s, p->point_array);
        s.layer = layers.of(p->tag);
        s.repetition = repetitionOf(cell, p->repetition);
        s.properties = propertiesIndex(cell, p->properties);
        cell.shapes.append(s);
    }
    const auto addPath = [&](gdstk::Array<gdstk::Polygon*>& outlines, const gdstk::Array<gdstk::Vec2>& spine, double width,
                             const QString& ends, const gdstk::Repetition& repetition, const gdstk::Property* properties) {
        PathInfo info;
        info.width = width;
        info.ends = ends;
        info.first = quint32(cell.points.size());
        info.count = quint32(spine.count);
        for (uint64_t k = 0; k < spine.count; ++k) cell.points.append(QPointF(spine[k].x, spine[k].y));
        cell.paths.append(info);
        const qint32 pathIndex = qint32(cell.paths.size() - 1);
        const qint32 rep = repetitionOf(cell, repetition);
        const qint32 props = propertiesIndex(cell, properties);
        for (uint64_t k = 0; k < outlines.count; ++k) {
            gdstk::Polygon* p = outlines[k];
            if (p->point_array.count > 0) {
                Shape s;
                s.kind = ShapeKind::Path;
                addOutline(cell, s, p->point_array);
                s.layer = layers.of(p->tag);
                s.repetition = rep;
                s.properties = props;
                s.path = pathIndex;
                cell.shapes.append(s);
            }
            p->clear();
            gdstk::free_allocation(p);
        }
        outlines.clear();
    };
    for (uint64_t i = 0; i < from.flexpath_array.count; ++i) {
        gdstk::FlexPath* path = from.flexpath_array[i];
        gdstk::Array<gdstk::Polygon*> outlines = {};
        if (path->to_polygons(false, 0, outlines) != gdstk::ErrorCode::NoError && outlines.count == 0) {
            warnings << tr("A path of %1 could not be drawn: left out.").arg(cell.name);
            continue;
        }
        double width = 0;
        QString ends;
        if (path->num_elements > 0) {
            const gdstk::FlexPathElement& el = path->elements[0];
            if (el.half_width_and_offset.count > 0) width = 2 * el.half_width_and_offset[0].x;
            ends = endsOf(el.end_type);
        }
        addPath(outlines, path->spine.point_array, width, ends, path->repetition, path->properties);
    }
    for (uint64_t i = 0; i < from.robustpath_array.count; ++i) {
        gdstk::RobustPath* path = from.robustpath_array[i];
        gdstk::Array<gdstk::Polygon*> outlines = {};
        if (path->to_polygons(false, 0, outlines) != gdstk::ErrorCode::NoError && outlines.count == 0) {
            warnings << tr("A path of %1 could not be drawn: left out.").arg(cell.name);
            continue;
        }
        gdstk::Array<gdstk::Vec2> spine = {};
        path->spine(spine);
        double width = 0;
        if (path->num_elements > 0 && path->elements[0].width_array.count > 0)
            width = path->elements[0].width_array[0].initial_value * path->width_scale;
        addPath(outlines, spine, width, QString(), path->repetition, path->properties);
        spine.clear();
    }
    for (uint64_t i = 0; i < from.label_array.count; ++i) {
        const gdstk::Label* l = from.label_array[i];
        Label label;
        label.text = l->text != nullptr ? QString::fromUtf8(l->text) : QString();
        label.origin = QPointF(l->origin.x, l->origin.y);
        label.layer = layers.of(l->tag) | 0x80000000u;   // (a text's: told apart below)
        label.rotation = l->rotation * 180.0 / M_PI;
        label.magnification = l->magnification;
        label.mirrored = l->x_reflection;
        label.repetition = repetitionOf(cell, l->repetition);
        label.properties = propertiesIndex(cell, l->properties);
        cell.labels.append(label);
    }
    for (uint64_t i = 0; i < from.reference_array.count; ++i) {
        const gdstk::Reference* r = from.reference_array[i];
        Instance inst;
        if (r->type == gdstk::ReferenceType::Cell) {
            inst.cell = index.value(r->cell, -1);
        } else {
            const char* name = r->type == gdstk::ReferenceType::Name ? r->name : (r->rawcell != nullptr ? r->rawcell->name : nullptr);
            if (name != nullptr) missing.insert(QString::fromUtf8(name));
        }
        inst.origin = QPointF(r->origin.x, r->origin.y);
        inst.rotation = r->rotation * 180.0 / M_PI;
        inst.magnification = r->magnification == 0 ? 1 : r->magnification;
        inst.mirrored = r->x_reflection;
        inst.repetition = repetitionOf(cell, r->repetition);
        inst.properties = propertiesIndex(cell, r->properties);
        cell.instances.append(inst);
    }
}

// The bounds of every cell, bottom up; a placement that makes a cell its
// own ancestor is taken out (the file is wrong).
void finish(Layout& layout)
{
    const int n = int(layout.cells.size());
    QVector<char> state(n, 0);   // 0 not seen, 1 on the way down, 2 done
    std::function<void(int)> visit = [&](int c) {
        state[c] = 1;
        Cell& cell = layout.cells[c];
        Box box;
        for (const Shape& s : cell.shapes) {
            if (s.repetition >= 0) box.add(cell.repetitions[s.repetition].extent(s.bounds));
            else box.add(s.bounds);
        }
        for (const Label& l : cell.labels) {
            const QRectF at(l.origin, QSizeF(0, 0));   // (a point: Box keeps it)
            box.add(l.repetition >= 0 ? cell.repetitions[l.repetition].extent(at) : at);
        }
        cell.ownBounds = box.rect();
        int depth = 0;
        for (Instance& inst : cell.instances) {
            if (inst.cell < 0) continue;
            if (state[inst.cell] == 1) {
                layout.warnings << tr("%1 places %2, which places it: that placement is left out.")
                                       .arg(cell.name, layout.cells[inst.cell].name);
                inst.cell = -1;
                continue;
            }
            if (state[inst.cell] == 0) visit(inst.cell);
            const Cell& child = layout.cells[inst.cell];
            depth = std::max(depth, child.depth + 1);
            if (child.bounds.isNull() && child.shapes.isEmpty() && child.labels.isEmpty() && child.instances.isEmpty()) continue;
            const QRectF one = inst.transform().mapRect(child.bounds);
            inst.bounds = inst.repetition >= 0 ? cell.repetitions[inst.repetition].extent(one) : one;
            box.add(inst.bounds);
        }
        cell.bounds = box.rect();
        cell.depth = depth;
        state[c] = 2;
    };
    for (int c = 0; c < n; ++c)
        if (state[c] == 0) visit(c);

    for (Cell& cell : layout.cells) {
        QSet<int> placed;
        for (const Instance& inst : cell.instances) {
            if (inst.cell < 0) continue;
            placed.insert(inst.cell);
            layout.instanceCount += inst.repetition >= 0 ? cell.repetitions[inst.repetition].count() : 1;
        }
        for (int child : placed) layout.cells[child].parents++;
    }
    for (int c = 0; c < n; ++c)
        if (layout.cells[c].parents == 0) layout.topCells.append(c);
    std::sort(layout.topCells.begin(), layout.topCells.end(), [&](int a, int b) {
        const QRectF& ra = layout.cells[a].bounds;
        const QRectF& rb = layout.cells[b].bounds;
        const double aa = ra.width() * ra.height(), ab = rb.width() * rb.height();
        if (aa != ab) return aa > ab;
        if (layout.cells[a].depth != layout.cells[b].depth) return layout.cells[a].depth > layout.cells[b].depth;
        return layout.cells[a].name < layout.cells[b].name;
    });
}

} // namespace

// ----------------------------------------------------------------------
// Repetitions

QPointF Repetition::offset(quint64 i) const
{
    if (type == Type::Explicit) return i < quint64(offsets.size()) ? offsets.at(qsizetype(i)) : QPointF();
    const quint64 column = i % columns, row = i / columns;
    return double(column) * v1 + double(row) * v2;
}

QRectF Repetition::extent(const QRectF& r) const
{
    Box box;
    if (type == Type::Explicit) {
        for (const QPointF& o : offsets) box.add(r.translated(o));
    } else {
        const QPointF last1 = double(columns - 1) * v1, last2 = double(rows - 1) * v2;
        for (const QPointF& o : {QPointF(0, 0), last1, last2, last1 + last2}) box.add(r.translated(o));
    }
    return box.rect();
}

bool Repetition::forEach(const QRectF& r, const QRectF& visible, const std::function<bool(QPointF)>& each) const
{
    if (type == Type::Explicit) {
        for (const QPointF& o : offsets)
            if (!visible.isValid() || meets(r.translated(o), visible))
                if (!each(o)) return false;
        return true;
    }
    quint64 c0 = 0, c1 = columns - 1, r0 = 0, r1 = rows - 1;
    if (visible.isValid()) {
        // The offsets o that bring r into sight: a rectangle q; the
        // indices whose offsets can be in it, from the inverse of [v1 v2].
        const QRectF q(QPointF(visible.left() - r.right(), visible.top() - r.bottom()),
                       QPointF(visible.right() - r.left(), visible.bottom() - r.top()));
        const double det = v1.x() * v2.y() - v1.y() * v2.x();
        const auto range = [](double lo, double hi, quint64 count, quint64& from, quint64& to) {
            if (hi < 0 || lo > double(count - 1)) return false;
            from = quint64(std::max(0.0, std::floor(lo)));
            to = quint64(std::min(double(count - 1), std::ceil(hi)));
            return true;
        };
        // Along one vector (the other has one copy, or none to go by).
        const auto along = [&](QPointF v, double& lo, double& hi) {
            lo = -1e300;
            hi = 1e300;
            for (int axis = 0; axis < 2; ++axis) {
                const double d = axis == 0 ? v.x() : v.y();
                const double a = axis == 0 ? q.left() : q.top(), b = axis == 0 ? q.right() : q.bottom();
                if (d == 0) {
                    if (a > 0 || b < 0) return false;
                    continue;
                }
                double t0 = a / d, t1 = b / d;
                if (t0 > t1) std::swap(t0, t1);
                lo = std::max(lo, t0);
                hi = std::min(hi, t1);
            }
            return lo <= hi;
        };
        if (rows == 1 || columns == 1) {
            const QPointF v = rows == 1 ? v1 : v2;
            const quint64 count = rows == 1 ? columns : rows;
            double lo, hi;
            if (count > 1) {
                if (!along(v, lo, hi)) return true;
                quint64 from, to;
                if (!range(lo, hi, count, from, to)) return true;
                if (rows == 1) c0 = from, c1 = to;
                else r0 = from, r1 = to;
            }
        } else if (det != 0) {
            double i0 = 1e300, i1 = -1e300, j0 = 1e300, j1 = -1e300;
            for (const QPointF& o : {q.topLeft(), q.topRight(), q.bottomLeft(), q.bottomRight()}) {
                const double i = (o.x() * v2.y() - o.y() * v2.x()) / det;
                const double j = (v1.x() * o.y() - v1.y() * o.x()) / det;
                i0 = std::min(i0, i), i1 = std::max(i1, i), j0 = std::min(j0, j), j1 = std::max(j1, j);
            }
            if (!range(i0, i1, columns, c0, c1) || !range(j0, j1, rows, r0, r1)) return true;
        }
    }
    for (quint64 row = r0; row <= r1; ++row)
        for (quint64 column = c0; column <= c1; ++column) {
            const QPointF o = double(column) * v1 + double(row) * v2;
            if (!visible.isValid() || meets(r.translated(o), visible))
                if (!each(o)) return false;
        }
    return true;
}

QTransform Instance::transform() const
{
    QTransform t;
    t.translate(origin.x(), origin.y());
    t.rotate(rotation);
    t.scale(magnification, magnification);
    if (mirrored) t.scale(1, -1);
    return t;
}

// ----------------------------------------------------------------------
// Reading

bool isLayoutFile(const QString& path)
{
    const QString name = QFileInfo(path).fileName().toLower();
    static const QStringList suffixes{QStringLiteral(".gds"), QStringLiteral(".gds2"), QStringLiteral(".gdsii"),
                                      QStringLiteral(".gds.gz"), QStringLiteral(".oas"), QStringLiteral(".oasis")};
    for (const QString& s : suffixes)
        if (name.endsWith(s) && name.size() > s.size()) return true;
    return false;
}

namespace {
std::atomic_int g_reads{0};
}

int readsGoing()
{
    return g_reads.load();
}

std::shared_ptr<Layout> read(const QString& path, QString* error, const std::atomic_bool* cancelled, Progress* progress)
{
    g_reads++;
    struct Done {
        ~Done() { g_reads--; }
    } done;
    QString dummy;
    if (error == nullptr) error = &dummy;
    const auto stopped = [cancelled] { return cancelled != nullptr && cancelled->load(); };
    if (progress != nullptr) progress->store(-1);
    if (!QFileInfo(path).isFile()) {
        *error = tr("There is no file %1.").arg(QDir::toNativeSeparators(path));
        return nullptr;
    }
    const Kind kind = kindOf(path);
    if (kind == Kind::Unknown) {
        *error = tr("%1 is not a GDSII or OASIS layout: it does not begin as one.").arg(QFileInfo(path).fileName());
        return nullptr;
    }

    // What gdstk opens: the file, or a copy it can open by name (inflated,
    // or of a name Windows' fopen() cannot take).
    QString source = path;
    QTemporaryFile copy(QDir::tempPath() + QStringLiteral("/qucs-layout-XXXXXX") + (kind == Kind::Oasis ? ".oas" : ".gds"));
    if (kind == Kind::GzippedGds || !openableByName(path)) {
        if (!copy.open()) {
            *error = tr("No room for a copy to read: %1").arg(copy.errorString());
            return nullptr;
        }
        if (kind == Kind::GzippedGds) {
            QString why;
            if (!gunzip(path, copy, &why)) {
                *error = tr("%1 could not be uncompressed. %2").arg(QFileInfo(path).fileName(), why);
                return nullptr;
            }
        } else {
            QFile in(path);
            if (!in.open(QIODevice::ReadOnly)) {
                *error = in.errorString();
                return nullptr;
            }
            while (!in.atEnd()) copy.write(in.read(1 << 20));
            copy.flush();
        }
        source = copy.fileName();
        if (!openableByName(source)) {
            *error = tr("%1 cannot be read here: the folder for temporary files has a name gdstk cannot open.")
                         .arg(QFileInfo(path).fileName());
            return nullptr;
        }
        if (kind == Kind::GzippedGds && kindOf(source) != Kind::Gds) {
            *error = tr("%1 is compressed, but what is in it is not a GDSII layout.").arg(QFileInfo(path).fileName());
            return nullptr;
        }
    }
    if (stopped()) return nullptr;

    // Read, in µm, with what gdstk says kept.
    gdstk::Library library = {};
    gdstk::ErrorCode code = gdstk::ErrorCode::NoError;
    QStringList said;
    {
        QMutexLocker locked(&readLock());
        QTemporaryFile log(QDir::tempPath() + QStringLiteral("/qucs-layout-log-XXXXXX.txt"));
        FILE* logFile = log.open() ? openFile(log.fileName(), "w+") : nullptr;
        FILE* before = gdstk::error_logger;
        // (never none: gdstk's debug build writes to it unasked)
        if (logFile != nullptr) gdstk::set_error_logger(logFile);
        const QByteArray name = QFile::encodeName(source);
        if (kind == Kind::Oasis) library = gdstk::read_oas(name.constData(), 1e-6, 0, &code);
        else library = gdstk::read_gds(name.constData(), 1e-6, 0, nullptr, &code);
        gdstk::set_error_logger(before);
        if (logFile != nullptr) {
            fflush(logFile);
            fseek(logFile, 0, SEEK_SET);
            QByteArray text;
            char buffer[4096];
            size_t n;
            while ((n = fread(buffer, 1, sizeof buffer, logFile)) > 0 && text.size() < 65536) text.append(buffer, qsizetype(n));
            fclose(logFile);
            for (const QString& line : QString::fromUtf8(text).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                QString l = line.trimmed();
                l.remove(QStringLiteral("[GDSTK] "));
                if (!l.isEmpty() && !said.contains(l)) said << l;
            }
        }
    }
    const bool failed = code >= gdstk::ErrorCode::ChecksumError;
    if (failed && library.cell_array.count == 0) {
        library.free_all();
        QString why;
        switch (code) {
        case gdstk::ErrorCode::InputFileOpenError: why = tr("It could not be opened."); break;
        case gdstk::ErrorCode::InsufficientMemory: why = tr("There is not memory enough for it."); break;
        case gdstk::ErrorCode::ZlibError: why = tr("A compressed block of it is damaged."); break;
        case gdstk::ErrorCode::ChecksumError: why = tr("Its checksum is wrong: it was changed or damaged."); break;
        default: why = tr("It is damaged or not whole.");
        }
        *error = tr("%1 could not be read. %2").arg(QFileInfo(path).fileName(), why);
        if (!said.isEmpty()) *error += QLatin1Char(' ') + tr("gdstk: %1").arg(said.mid(0, 3).join(QStringLiteral("; ")));
        return nullptr;
    }
    if (stopped()) {
        library.free_all();
        return nullptr;
    }

    auto layout = std::make_shared<Layout>();
    layout->file = QFileInfo(path).absoluteFilePath();
    layout->format = kind == Kind::Oasis ? Layout::Format::Oasis : Layout::Format::Gds;
    layout->compressed = kind == Kind::GzippedGds;
    // (OASIS has no library name: gdstk's "LIB" is its own.)
    layout->library = library.name != nullptr && kind != Kind::Oasis ? QString::fromUtf8(library.name) : QString();
    if (library.precision > 0) layout->dbu = library.precision / 1e-6;
    layout->warnings = said;
    if (failed) layout->warnings.prepend(tr("It ends early or is damaged: what could be read is shown."));

    QHash<const gdstk::Cell*, int> index;
    const int n = int(library.cell_array.count);
    layout->cells.resize(n);
    for (int c = 0; c < n; ++c) index.insert(library.cell_array[uint64_t(c)], c);
    Layers layers;
    QSet<QString> missing;
    for (int c = 0; c < n; ++c) {
        if (stopped()) {
            library.free_all();
            return nullptr;
        }
        takeCell(*library.cell_array[uint64_t(c)], layout->cells[c], index, layers, missing, layout->warnings);
        if (progress != nullptr) progress->store(int(qint64(c + 1) * 1000 / std::max(1, n)));
    }

    // The layers in order of layer and datatype; a text's texttype is its
    // datatype, so a text and a shape of 1/0 share the layer.
    QVector<LayerKey> keys;
    for (gdstk::Tag t : layers.tags) keys.append({gdstk::get_layer(t), gdstk::get_type(t)});
    QVector<int> order(keys.size());
    for (int i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b) { return keys[a] < keys[b]; });
    QVector<quint32> newIndex(keys.size());
    for (int i = 0; i < order.size(); ++i) {
        newIndex[order[i]] = quint32(i);
        LayerInfo info;
        info.key = keys[order[i]];
        layout->layers.append(info);
        layout->byLayer.insert(info.key, i);
    }
    for (Cell& cell : layout->cells) {
        for (Shape& s : cell.shapes) {
            s.layer = newIndex[s.layer];
            const quint64 copies = s.repetition >= 0 ? cell.repetitions[s.repetition].count() : 1;
            layout->layers[s.layer].shapes += copies;
            layout->shapeCount += copies;
        }
        for (Label& l : cell.labels) {
            l.layer = newIndex[l.layer & 0x7fffffffu];
            const quint64 copies = l.repetition >= 0 ? cell.repetitions[l.repetition].count() : 1;
            layout->layers[l.layer].labels += copies;
            layout->labelCount += copies;
        }
        layout->byName.insert(cell.name, int(&cell - layout->cells.data()));
    }
    // The names an OASIS file gives its layers (LAYERNAME).
    for (uint64_t i = 0; i < library.layer_names.count; ++i) {
        const gdstk::LayerName& ln = library.layer_names[i];
        if (ln.name == nullptr) continue;
        const auto within = [](const gdstk::LayerNameInterval& iv, quint32 v) {
            switch (iv.type) {
            case gdstk::OasisInterval::AllValues: return true;
            case gdstk::OasisInterval::UpperBound: return v <= iv.bound_a;
            case gdstk::OasisInterval::LowerBound: return v >= iv.bound_a;
            case gdstk::OasisInterval::SingleValue: return v == iv.bound_a;
            case gdstk::OasisInterval::Bounded: return v >= iv.bound_a && v <= iv.bound_b;
            }
            return false;
        };
        for (LayerInfo& info : layout->layers)
            if (info.name.isEmpty() && within(ln.layer_interval, info.key.layer) && within(ln.type_interval, info.key.datatype))
                info.name = QString::fromUtf8(ln.name);
    }
    library.free_all();

    layout->missing = QStringList(missing.begin(), missing.end());
    layout->missing.sort();
    if (!layout->missing.isEmpty())
        layout->warnings << tr("Placed but not in the file: %1.").arg(layout->missing.mid(0, 10).join(QStringLiteral(", ")))
                                + (layout->missing.size() > 10 ? tr(" and %1 more").arg(layout->missing.size() - 10) : QString());
    finish(*layout);
    if (progress != nullptr) progress->store(1000);
    return layout;
}

// ----------------------------------------------------------------------
// How layers look

LayerStyle paletteStyle(int index)
{
    static const QRgb colours[] = {0xff80a8, 0xc080ff, 0x9580ff, 0x8086ff, 0x80a8ff, 0xff0000, 0xff0080, 0xff00ff,
                                   0x8000ff, 0x0000ff, 0x008080, 0x00c000, 0x80c000, 0xc0a000, 0xff8000, 0x804000};
    static const Qt::BrushStyle patterns[] = {Qt::BDiagPattern, Qt::FDiagPattern, Qt::DiagCrossPattern, Qt::Dense5Pattern,
                                              Qt::HorPattern,   Qt::VerPattern,   Qt::CrossPattern,     Qt::Dense6Pattern};
    LayerStyle s;
    const int i = std::max(0, index);
    s.frame = QColor::fromRgb(colours[i % 16]);
    s.fill = s.frame;
    s.pattern = patterns[(i + i / 16) % 8];
    return s;
}

namespace {

// KLayout's dither patterns, as near as Qt's patterns come.
Qt::BrushStyle patternOf(const QString& dither)
{
    static const QRegularExpression standard(QStringLiteral("^I(\\d+)$"));
    const QRegularExpressionMatch m = standard.match(dither.trimmed());
    if (!m.hasMatch()) return dither.trimmed().isEmpty() ? Qt::SolidPattern : Qt::DiagCrossPattern;
    const int n = m.captured(1).toInt();
    if (n == 0) return Qt::SolidPattern;
    if (n == 1) return Qt::NoBrush;
    if (n <= 3) return Qt::Dense6Pattern;   // dotted
    if (n <= 7) return Qt::FDiagPattern;    // left-hatched
    if (n <= 11) return Qt::BDiagPattern;   // right-hatched
    if (n <= 13 || n == 15) return Qt::DiagCrossPattern;
    if (n == 14 || n == 16) return Qt::Dense4Pattern;   // checkerboards
    if (n == 21) return Qt::CrossPattern;
    if (n == 22) return Qt::HorPattern;
    if (n >= 32 && n <= 35) return Qt::VerPattern;
    if (n >= 36 && n <= 39) return Qt::HorPattern;
    return Qt::Dense5Pattern;
}

bool sourceKey(const QString& source, LayerKey* key)
{
    QString s = source;
    s = s.section(QLatin1Char('@'), 0, 0);
    static const QRegularExpression pair(QStringLiteral("(\\d+)\\s*/\\s*(\\d+)"));
    static const QRegularExpression single(QStringLiteral("(?:^|[\\s(])(\\d+)(?:$|[\\s)])"));
    QRegularExpressionMatch m = pair.match(s);
    if (m.hasMatch()) {
        key->layer = m.captured(1).toUInt();
        key->datatype = m.captured(2).toUInt();
        return true;
    }
    m = single.match(s);
    if (m.hasMatch()) {
        key->layer = m.captured(1).toUInt();
        key->datatype = 0;
        return true;
    }
    return false;
}

} // namespace

QHash<LayerKey, LayerStyle> readLayerProperties(const QString& path, QString* error)
{
    QHash<LayerKey, LayerStyle> result;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = f.errorString();
        return result;
    }
    QXmlStreamReader xml(&f);
    // One layer's properties, to its end; a group's members are layers of
    // their own.
    std::function<void()> layer = [&] {
        LayerStyle style;
        style.pattern = Qt::SolidPattern;
        QString source;
        while (xml.readNextStartElement()) {
            const QString name = xml.name().toString();
            if (name == QLatin1String("group-members")) {
                layer();
                continue;
            }
            const QString text = xml.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
            if (name == QLatin1String("frame-color")) style.frame = QColor(text);
            else if (name == QLatin1String("fill-color")) style.fill = QColor(text);
            else if (name == QLatin1String("dither-pattern")) style.pattern = patternOf(text);
            else if (name == QLatin1String("visible")) style.visible = text != QLatin1String("false");
            else if (name == QLatin1String("name")) style.name = text;
            else if (name == QLatin1String("source")) source = text;
        }
        LayerKey key;
        if (!sourceKey(source, &key)) return;   // (a group's own: */*)
        if (!style.fill.isValid()) style.fill = style.frame;
        if (!style.frame.isValid()) style.frame = style.fill;
        if (!style.frame.isValid()) return;
        // A name from the source when it has none of its own: "Metal1 1/0".
        if (style.name.isEmpty()) {
            QString rest = source.section(QLatin1Char('@'), 0, 0);
            rest.remove(QRegularExpression(QStringLiteral("\\(?\\d+\\s*(/\\s*\\d+)?\\)?")));
            style.name = rest.trimmed();
        }
        if (!result.contains(key)) result.insert(key, style);
    };
    bool any = false;
    if (xml.readNextStartElement() && xml.name() == QLatin1String("layer-properties")) {
        any = true;
        while (xml.readNextStartElement()) {
            if (xml.name() == QLatin1String("properties")) layer();
            else xml.skipCurrentElement();
        }
    }
    if (xml.hasError() || !any || result.isEmpty()) {
        if (error != nullptr)
            *error = xml.hasError() ? tr("It is not a layer properties file: %1 (line %2).").arg(xml.errorString()).arg(xml.lineNumber())
                     : !any         ? tr("It is not a layer properties file: it has no <layer-properties>.")
                                    : tr("It has no layers.");
        return {};
    }
    return result;
}

QString layerPropertiesBeside(const QString& layoutFile)
{
    const QFileInfo info(layoutFile);
    QString base = info.fileName();
    for (const QString& suffix : {QStringLiteral(".gz"), QStringLiteral(".gds"), QStringLiteral(".gds2"), QStringLiteral(".gdsii"),
                                  QStringLiteral(".oas"), QStringLiteral(".oasis")})
        if (base.endsWith(suffix, Qt::CaseInsensitive)) base.chop(suffix.size());
    const QDir dir = info.absoluteDir();
    const QString own = dir.filePath(base + QStringLiteral(".lyp"));
    if (QFileInfo::exists(own)) return own;
    const QStringList all = dir.entryList({QStringLiteral("*.lyp")}, QDir::Files);
    return all.size() == 1 ? dir.filePath(all.first()) : QString();
}

QVector<LayerStyle> stylesFor(const Layout& layout, const QHash<LayerKey, LayerStyle>& lyp)
{
    QVector<LayerStyle> styles;
    styles.reserve(layout.layers.size());
    for (int i = 0; i < layout.layers.size(); ++i) {
        const LayerInfo& info = layout.layers.at(i);
        LayerStyle s = lyp.contains(info.key) ? lyp.value(info.key) : paletteStyle(i);
        if (s.name.isEmpty()) s.name = info.name;
        styles.append(s);
    }
    return styles;
}

// ----------------------------------------------------------------------
// Drawing

QTransform viewTransform(QPointF center, double scale, const QRectF& device)
{
    QTransform t;
    t.translate(device.center().x(), device.center().y());
    t.scale(scale, -scale);
    t.translate(-center.x(), -center.y());
    return t;
}

namespace {

class Renderer
{
public:
    Renderer(const Layout& layout, const RenderOptions& o) : L(layout), O(o)
    {
        const int n = int(layout.layers.size());
        polygons.resize(n);
        dots.resize(n);
        seen.resize(n);
        device = o.device.toAlignedRect();
    }

    bool visible(quint32 layer) const
    {
        return O.styles == nullptr || int(layer) >= O.styles->size() || O.styles->at(int(layer)).visible;
    }
    bool full()
    {
        if (used < O.budget) return false;
        stats.incomplete = true;
        return true;
    }
    // A pixel of a layer, once.
    void dot(quint32 layer, QPointF at)
    {
        const QPoint p = at.toPoint();
        if (!device.contains(p)) return;
        QBitArray& bits = seen[int(layer)];
        if (bits.isEmpty()) bits.resize(device.width() * device.height());
        const qsizetype i = qsizetype(p.y() - device.top()) * device.width() + (p.x() - device.left());
        if (bits.testBit(i)) return;
        bits.setBit(i);
        dots[int(layer)].append(at);
        stats.dots++;
    }

    void cell(int c, const QTransform& T, int level)
    {
        const Cell& cell = L.cells.at(c);
        bool invertible = false;
        const QTransform inverse = T.inverted(&invertible);
        if (!invertible) return;
        const QRectF vis = inverse.mapRect(O.device);
        const double s = std::sqrt(std::abs(T.determinant()));
        for (const Shape& sh : cell.shapes) {
            if (!visible(sh.layer)) continue;
            const auto put = [&](QPointF off) -> bool {
                const QRectF b = sh.bounds.translated(off);
                if (full()) return false;
                ++used;
                if (std::max(b.width(), b.height()) * s < 1.5) {
                    dot(sh.layer, T.map(b.center()));
                    return true;
                }
                QPolygonF poly(sh.count);
                const QPointF* p = cell.points.constData() + sh.first;
                for (quint32 k = 0; k < sh.count; ++k) poly[k] = T.map(p[k] + off);
                polygons[int(sh.layer)].append(poly);
                stats.polygons++;
                return true;
            };
            if (sh.repetition < 0) {
                if (meets(sh.bounds, vis) && !put(QPointF())) return;
                continue;
            }
            const Repetition& r = cell.repetitions.at(sh.repetition);
            // Copies too small to see one by one: where they are, filled.
            if (std::max(sh.bounds.width(), sh.bounds.height()) * s < 1.5 && r.count() > 16) {
                const QRectF e = r.extent(sh.bounds);
                if (!meets(e, vis)) continue;
                if (full()) return;
                ++used;
                polygons[int(sh.layer)].append(T.map(QPolygonF(e)));
                stats.polygons++;
                continue;
            }
            if (!r.forEach(sh.bounds, vis, put)) return;
        }
        if (O.labels) {
            for (const Label& l : cell.labels) {
                if (!visible(l.layer) || texts.size() >= 5000) continue;
                const auto put = [&](QPointF off) -> bool {
                    if (texts.size() >= 5000) return false;
                    texts.append({T.map(l.origin + off), l.text});
                    stats.labels++;
                    return true;
                };
                const QRectF at(l.origin, QSizeF(0, 0));
                if (l.repetition < 0) {
                    if (meets(at, vis)) put(QPointF());
                } else {
                    cell.repetitions.at(l.repetition).forEach(at, vis, put);
                }
            }
        }
        for (const Instance& inst : cell.instances) {
            if (inst.cell < 0 || !meets(inst.bounds, vis)) continue;
            const Cell& child = L.cells.at(inst.cell);
            if (child.bounds.isNull() && child.shapes.isEmpty() && child.labels.isEmpty() && child.instances.isEmpty()) continue;
            const QTransform it = inst.transform();
            const QRectF one = it.mapRect(child.bounds);
            const double px = std::max(one.width(), one.height()) * s;
            const auto put = [&](QPointF off) -> bool {
                if (full()) return false;
                const QTransform ct = it * QTransform::fromTranslate(off.x(), off.y()) * T;
                if (px < 2) {
                    ++used;
                    frameDots.append(T.map(one.center() + off));
                    return true;
                }
                if (level + 1 > O.depth) {
                    ++used;
                    const QPolygonF frame = ct.map(QPolygonF(child.bounds));
                    frames.append(frame);
                    stats.frames++;
                    if (px > 48) names.append({frame.boundingRect(), child.name});
                    return true;
                }
                this->cell(inst.cell, ct, level + 1);
                return !full();
            };
            if (inst.repetition < 0) {
                if (!put(QPointF())) return;
                continue;
            }
            const Repetition& r = cell.repetitions.at(inst.repetition);
            if (px < 2 && r.count() > 16) {
                if (full()) return;
                ++used;
                arrays.append(T.map(QPolygonF(inst.bounds)));
                continue;
            }
            if (!r.forEach(one, vis, put)) return;
        }
    }

    void paint(QPainter& p)
    {
        p.save();
        p.setRenderHint(QPainter::Antialiasing, false);
        for (int i = 0; i < polygons.size(); ++i) {
            if (polygons[i].isEmpty() && dots[i].isEmpty()) continue;
            const LayerStyle style = O.styles != nullptr && i < O.styles->size() ? O.styles->at(i) : paletteStyle(i);
            QPen pen(style.frame, 0);
            pen.setCosmetic(true);
            p.setPen(pen);
            p.setBrush(style.pattern == Qt::NoBrush ? QBrush(Qt::NoBrush) : QBrush(style.fill, style.pattern));
            for (const QPolygonF& poly : polygons[i]) p.drawPolygon(poly);
            if (!dots[i].isEmpty()) p.drawPoints(dots[i].constData(), int(dots[i].size()));
        }
        QPen framePen(O.frames.isValid() ? O.frames : QColor(Qt::gray), 0);
        framePen.setCosmetic(true);
        p.setPen(framePen);
        p.setBrush(Qt::NoBrush);
        for (const QPolygonF& f : frames) p.drawPolygon(f);
        if (!frameDots.isEmpty()) p.drawPoints(frameDots.constData(), int(frameDots.size()));
        if (!arrays.isEmpty()) {
            QColor tint = framePen.color();
            tint.setAlpha(70);
            p.setBrush(tint);
            for (const QPolygonF& a : arrays) p.drawPolygon(a);
            p.setBrush(Qt::NoBrush);
        }
        if (!names.isEmpty()) {
            QFont font = p.font();
            font.setPixelSize(11);
            p.setFont(font);
            const QFontMetricsF fm(font);
            for (const auto& [box, name] : names) {
                const QString shown = fm.elidedText(name, Qt::ElideRight, box.width() - 6);
                if (shown.size() < 2 || box.height() < fm.height() + 4) continue;
                p.drawText(box, Qt::AlignCenter, shown);
            }
        }
        if (!texts.isEmpty()) {
            QFont font = p.font();
            font.setPixelSize(10);
            p.setFont(font);
            p.setPen(O.text.isValid() ? O.text : QColor(Qt::black));
            for (const auto& [at, text] : texts) {
                p.drawLine(at + QPointF(-3, 0), at + QPointF(3, 0));
                p.drawLine(at + QPointF(0, -3), at + QPointF(0, 3));
                p.drawText(at + QPointF(4, -3), text);
            }
        }
        p.restore();
    }

    const Layout& L;
    const RenderOptions& O;
    QRect device;
    RenderStats stats;
    quint64 used = 0;
    QVector<QVector<QPolygonF>> polygons;
    QVector<QVector<QPointF>> dots;
    QVector<QBitArray> seen;
    QVector<QPolygonF> frames;
    QVector<QPointF> frameDots;
    QVector<QPolygonF> arrays;
    QVector<QPair<QRectF, QString>> names;
    QVector<QPair<QPointF, QString>> texts;
};

} // namespace

RenderStats render(QPainter& painter, const Layout& layout, const RenderOptions& options)
{
    if (options.cell < 0 || options.cell >= layout.cells.size() || options.device.isEmpty()) return {};
    Renderer r(layout, options);
    r.cell(options.cell, options.toDevice, 0);
    r.paint(painter);
    return r.stats;
}

// ----------------------------------------------------------------------
// Searching

void search(const Layout& layout, const Query& query, const std::function<bool(const Found&)>& each)
{
    if (query.cell < 0 || query.cell >= layout.cells.size()) return;
    const bool everywhere = !query.region.isValid() && query.region.isNull();
    const auto wanted = [&](quint32 layer) { return query.layers.isEmpty() || (int(layer) < query.layers.size() && query.layers.at(int(layer))); };
    bool going = true;
    QVector<int> via;
    std::function<void(int, const QTransform&, int)> visit = [&](int c, const QTransform& T, int level) {
        const Cell& cell = layout.cells.at(c);
        via.append(c);
        QRectF vis;   // the region in this cell's coordinates; invalid: everywhere
        if (!everywhere) {
            bool ok = false;
            const QTransform inverse = T.inverted(&ok);
            if (!ok) {
                via.removeLast();
                return;
            }
            vis = inverse.mapRect(query.region);
        }
        const auto inSight = [&](const QRectF& r) { return everywhere || meets(r, vis); };
        for (int i = 0; going && i < cell.shapes.size(); ++i) {
            const Shape& sh = cell.shapes.at(i);
            if (!wanted(sh.layer)) continue;
            if (sh.repetition < 0) {
                if (inSight(sh.bounds)) going = each(Found{c, i, -1, 0, T, via});
                continue;
            }
            const Repetition& r = cell.repetitions.at(sh.repetition);
            quint64 copy = 0;
            r.forEach(sh.bounds, everywhere ? QRectF() : vis, [&](QPointF off) {
                // (which copy: its offset's place in the repetition)
                if (r.type == Repetition::Type::Explicit) copy = quint64(r.offsets.indexOf(off));
                else {
                    const double det = r.v1.x() * r.v2.y() - r.v1.y() * r.v2.x();
                    if (det != 0) {
                        const quint64 col = quint64(std::llround((off.x() * r.v2.y() - off.y() * r.v2.x()) / det));
                        const quint64 row = quint64(std::llround((r.v1.x() * off.y() - r.v1.y() * off.x()) / det));
                        copy = row * r.columns + col;
                    } else {
                        const QPointF v = r.rows == 1 ? r.v1 : r.v2;
                        const double len = std::hypot(v.x(), v.y());
                        copy = len > 0 ? quint64(std::llround(std::hypot(off.x(), off.y()) / len)) : 0;
                        if (r.rows != 1) copy *= r.columns;
                    }
                }
                going = each(Found{c, i, -1, copy, QTransform::fromTranslate(off.x(), off.y()) * T, via});
                return going;
            });
        }
        if (query.labels) {
            for (int i = 0; going && i < cell.labels.size(); ++i) {
                const Label& l = cell.labels.at(i);
                if (!wanted(l.layer)) continue;
                const QRectF at(l.origin, QSizeF(0, 0));
                if (l.repetition < 0) {
                    if (inSight(at)) going = each(Found{c, -1, i, 0, T, via});
                    continue;
                }
                quint64 copy = 0;
                cell.repetitions.at(l.repetition).forEach(at, everywhere ? QRectF() : vis, [&](QPointF off) {
                    going = each(Found{c, -1, i, copy++, QTransform::fromTranslate(off.x(), off.y()) * T, via});
                    return going;
                });
            }
        }
        if (level < query.depth) {
            for (const Instance& inst : cell.instances) {
                if (!going) break;
                if (inst.cell < 0 || !inSight(inst.bounds)) continue;
                const QTransform it = inst.transform();
                const QRectF one = it.mapRect(layout.cells.at(inst.cell).bounds);
                const auto down = [&](QPointF off) {
                    visit(inst.cell, it * QTransform::fromTranslate(off.x(), off.y()) * T, level + 1);
                    return going;
                };
                if (inst.repetition < 0) down(QPointF());
                else cell.repetitions.at(inst.repetition).forEach(one, everywhere ? QRectF() : vis, down);
            }
        }
        via.removeLast();
    };
    visit(query.cell, QTransform(), 0);
}

QPolygonF outlineOf(const Layout& layout, const Found& found)
{
    if (found.cell < 0 || found.cell >= layout.cells.size()) return {};
    const Cell& cell = layout.cells.at(found.cell);
    if (found.label >= 0) return QPolygonF{found.transform.map(cell.labels.at(found.label).origin)};
    if (found.shape < 0) return {};
    const Shape& sh = cell.shapes.at(found.shape);
    QPolygonF poly(sh.count);
    for (quint32 k = 0; k < sh.count; ++k) poly[k] = found.transform.map(cell.points.at(sh.first + k));
    return poly;
}

QPolygonF spineOf(const Layout& layout, const Found& found)
{
    if (found.cell < 0 || found.shape < 0) return {};
    const Cell& cell = layout.cells.at(found.cell);
    const Shape& sh = cell.shapes.at(found.shape);
    if (sh.path < 0) return {};
    const PathInfo& path = cell.paths.at(sh.path);
    QPolygonF spine(path.count);
    for (quint32 k = 0; k < path.count; ++k) spine[k] = found.transform.map(cell.points.at(path.first + k));
    return spine;
}

namespace {

double distanceToSegment(QPointF p, QPointF a, QPointF b, QPointF* nearest)
{
    const QPointF d = b - a;
    const double len2 = d.x() * d.x() + d.y() * d.y();
    double t = len2 > 0 ? ((p.x() - a.x()) * d.x() + (p.y() - a.y()) * d.y()) / len2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    const QPointF n = a + t * d;
    if (nearest != nullptr) *nearest = n;
    return std::hypot(p.x() - n.x(), p.y() - n.y());
}

} // namespace

bool hits(const Layout& layout, const Found& found, QPointF point, double tolerance)
{
    const QPolygonF poly = outlineOf(layout, found);
    if (poly.isEmpty()) return false;
    if (found.isText()) return std::hypot(point.x() - poly.first().x(), point.y() - poly.first().y()) <= tolerance;
    if (poly.containsPoint(point, Qt::OddEvenFill)) return true;
    for (int i = 0; i < poly.size(); ++i)
        if (distanceToSegment(point, poly.at(i), poly.at((i + 1) % poly.size()), nullptr) <= tolerance) return true;
    return false;
}

QPointF snap(const Layout& layout, const Query& query, QPointF point, double tolerance)
{
    Query q = query;
    q.region = QRectF(point - QPointF(tolerance, tolerance), QSizeF(2 * tolerance, 2 * tolerance));
    q.labels = false;
    double bestVertex = tolerance, bestEdge = tolerance;
    QPointF vertex, edge;
    bool haveVertex = false, haveEdge = false;
    int looked = 0;
    search(layout, q, [&](const Found& f) {
        const QPolygonF poly = outlineOf(layout, f);
        for (int i = 0; i < poly.size(); ++i) {
            const QPointF v = poly.at(i);
            const double d = std::hypot(point.x() - v.x(), point.y() - v.y());
            if (d <= bestVertex) {
                bestVertex = d;
                vertex = v;
                haveVertex = true;
            }
            QPointF n;
            const double e = distanceToSegment(point, v, poly.at((i + 1) % poly.size()), &n);
            if (e <= bestEdge) {
                bestEdge = e;
                edge = n;
                haveEdge = true;
            }
        }
        return ++looked < 2000;
    });
    if (haveVertex) return vertex;
    if (haveEdge) return edge;
    const double g = layout.dbu > 0 ? layout.dbu : 0.001;
    return QPointF(std::round(point.x() / g) * g, std::round(point.y() / g) * g);
}

QString kindName(const Found& found)
{
    if (found.isText()) return QStringLiteral("text");
    return QStringLiteral("polygon");
}

QString micrometres(const Layout& layout, double value)
{
    const int decimals = layout.dbu > 0 ? std::clamp(int(std::ceil(-std::log10(layout.dbu) - 1e-9)), 0, 9) : 3;
    QString s = QString::number(value, 'f', decimals);
    if (s.contains(QLatin1Char('.'))) {
        while (s.endsWith(QLatin1Char('0'))) s.chop(1);
        if (s.endsWith(QLatin1Char('.'))) s.chop(1);
    }
    if (s == QLatin1String("-0")) s = QStringLiteral("0");
    return s;
}

} // namespace qucs_s::layout
