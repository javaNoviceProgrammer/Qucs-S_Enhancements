/*
 * fem_hdf5.cpp - a read-only view of an HDF5 file
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_hdf5.h"

#include <QCoreApplication>
#include <QFile>

#include <algorithm>
#include <cstring>
#include <functional>

namespace qucs_s::fem {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("Hdf5File", text);
}

/// The bytes that hold numbers up to \a n.
int bytesFor(quint64 n)
{
    int b = 1;
    while (b < 8 && (n >> (8 * b)) != 0) ++b;
    return b;
}

int log2Of(quint64 n)
{
    int k = 0;
    while (n > 1) {
        n >>= 1;
        ++k;
    }
    return k;
}

} // namespace

quint64 Hdf5File::u(qint64 at, int bytes) const
{
    if (!fits(at, bytes)) return 0;
    quint64 v = 0;
    for (int i = bytes - 1; i >= 0; --i) v = (v << 8) | quint8(a_data[at + i]);
    return v;
}

bool Hdf5File::open(const QString& path, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = tr("%1 cannot be read").arg(path);
        return false;
    }
    return openData(f.readAll(), error);
}

bool Hdf5File::openData(const QByteArray& data, QString* error)
{
    a_data = data;
    auto fail = [&](const QString& why) {
        if (error) *error = why;
        return false;
    };
    static const char signature[8] = {'\x89', 'H', 'D', 'F', '\r', '\n', '\x1a', '\n'};
    // (Its superblock at 0, 512, 1024... after a user block.)
    qint64 at = -1;
    for (qint64 p = 0; p + 8 <= a_data.size() && p <= (1 << 20); p = p == 0 ? 512 : 2 * p)
        if (std::memcmp(a_data.constData() + p, signature, 8) == 0) {
            at = p;
            break;
        }
    if (at < 0) return fail(tr("it is not an HDF5 file"));
    const int version = quint8(a_data[at + 8]);
    if (version != 2 && version != 3) return fail(tr("its HDF5 superblock is of version %1: 2 and 3 are read").arg(version));
    a_O = quint8(a_data[at + 9]);
    a_L = quint8(a_data[at + 10]);
    if (a_O < 2 || a_O > 8 || a_L < 2 || a_L > 8) return fail(tr("its HDF5 offsets are %1 bytes, its lengths %2").arg(a_O).arg(a_L));
    const quint64 base = u(at + 12, a_O);
    if (base != 0 && base != quint64(at)) return fail(tr("its HDF5 base address is not its start"));
    a_root = u(at + 12 + 3 * a_O, a_O) + quint64(at);
    if (messages(a_root).empty()) return fail(tr("its HDF5 root group is not read: %1").arg(a_error));
    return true;
}

std::vector<Hdf5File::Message> Hdf5File::messages(quint64 object) const
{
    std::vector<Message> out;
    const qint64 at = qint64(object);
    if (!fits(at, 6) || std::memcmp(a_data.constData() + at, "OHDR", 4) != 0) {
        a_error = tr("an object header of version 1 or none at %1").arg(object);
        return out;
    }
    const int flags = quint8(a_data[at + 5]);
    qint64 p = at + 6 + ((flags & 0x20) ? 16 : 0) + ((flags & 0x10) ? 4 : 0);
    const int sizeBytes = 1 << (flags & 3);
    const qint64 size = qint64(u(p, sizeBytes));
    p += sizeBytes;
    const int prefix = 4 + ((flags & 0x04) ? 2 : 0);
    // Its first chunk, then each continuation's.
    std::vector<std::pair<qint64, qint64>> chunks{{p, p + size}};
    for (std::size_t c = 0; c < chunks.size() && c < 10000; ++c) {
        auto [q, end] = chunks[c];
        if (!fits(q, end - q)) {
            a_error = tr("an object header past the file's end");
            return {};
        }
        while (q + prefix <= end) {
            Message m;
            m.type = quint8(a_data[q]);
            m.size = qint64(u(q + 1, 2));
            m.at = q + prefix;
            q = m.at + m.size;
            if (q > end) break;
            if (m.type == 0x10) {
                const qint64 next = qint64(u(m.at, a_O)), length = qint64(u(m.at + a_O, a_L));
                if (fits(next, length) && std::memcmp(a_data.constData() + next, "OCHK", 4) == 0) chunks.emplace_back(next + 4, next + length - 4);
                continue;
            }
            if (m.type != 0) out.push_back(m);
        }
    }
    return out;
}

quint64 Hdf5File::find(const QString& path, quint64 from) const
{
    quint64 at = from ? from : a_root;
    for (const QString& part : path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        quint64 next = 0;
        for (const auto& [name, object] : links(at))
            if (name == part) {
                next = object;
                break;
            }
        if (!next) return 0;
        at = next;
    }
    return at;
}

std::optional<std::pair<QString, quint64>> Hdf5File::link(qint64 at, qint64 size) const
{
    if (!fits(at, size) || size < 3 || quint8(a_data[at]) != 1) return std::nullopt;
    const int flags = quint8(a_data[at + 1]);
    qint64 p = at + 2;
    int type = 0;
    if (flags & 0x08) type = quint8(a_data[p++]);
    if (flags & 0x04) p += 8;
    if (flags & 0x10) ++p;
    const int lengthBytes = 1 << (flags & 3);
    const qint64 length = qint64(u(p, lengthBytes));
    p += lengthBytes;
    if (!fits(p, length)) return std::nullopt;
    const QString name = QString::fromUtf8(a_data.constData() + p, int(length));
    p += length;
    if (type != 0) return std::nullopt;   // (soft and external links: not followed)
    return std::pair(name, u(p, a_O));
}

std::vector<std::pair<QString, quint64>> Hdf5File::links(quint64 object) const
{
    std::vector<std::pair<QString, quint64>> out;
    for (const Message& m : messages(object)) {
        if (m.type == 0x06) {
            if (const auto l = link(m.at, m.size)) out.push_back(*l);
        } else if (m.type == 0x02 && fits(m.at, 2)) {
            // Dense: a fractal heap of link messages, a B-tree of their names.
            const int flags = quint8(a_data[m.at + 1]);
            const qint64 p = m.at + 2 + ((flags & 1) ? 8 : 0);
            const quint64 heap = u(p, a_O), tree = u(p + a_O, a_O);
            if (heap == ~quint64(0) >> (64 - 8 * a_O) || tree == ~quint64(0) >> (64 - 8 * a_O)) continue;
            for (const QByteArray& record : treeRecords(tree)) {
                // (A name index's: its hash, then the heap ID.)
                const QByteArray object = heapObject(heap, record.mid(4));
                if (object.isEmpty()) continue;
                Hdf5File view;
                view.a_data = object;
                view.a_O = a_O;
                view.a_L = a_L;
                if (const auto l = view.link(0, object.size())) out.push_back(*l);
            }
        }
    }
    return out;
}

std::optional<Hdf5File::Value> Hdf5File::decode(qint64 type, qint64 space, qint64 data, qint64 available) const
{
    if (!fits(type, 8) || !fits(space, 2)) return std::nullopt;
    const int cls = quint8(a_data[type]) & 0x0F;
    const int bits = quint8(a_data[type + 1]);
    const qint64 size = qint64(u(type + 4, 4));
    Value v;
    // Its shape: version 1 (dims after 6 more bytes) or 2 (a type byte).
    const int version = quint8(a_data[space]), rank = quint8(a_data[space + 1]);
    qint64 dims = space + (version == 1 ? 8 : 4);
    quint64 count = 1;
    if (version == 2 && quint8(a_data[space + 3]) == 2) count = 0;   // (null)
    for (int i = 0; i < rank; ++i) {
        const quint64 d = u(dims + qint64(i) * a_L, a_L);
        v.dims.push_back(d);
        count *= d;
    }
    if (size <= 0 || count > quint64(available / size) + 1 || !fits(data, qint64(count) * size)) return std::nullopt;
    v.raw = a_data.mid(data, int(count * quint64(size)));
    for (quint64 i = 0; i < count; ++i) {
        const qint64 at = data + qint64(i) * size;
        if (cls == 0) {   // fixed point (little-endian)
            if (bits & 1) return std::nullopt;
            quint64 x = u(at, int(std::min<qint64>(size, 8)));
            if ((bits & 0x08) && size < 8 && (x >> (8 * size - 1)) & 1) x |= ~quint64(0) << (8 * size);
            v.numbers.push_back((bits & 0x08) ? double(qint64(x)) : double(x));
        } else if (cls == 1) {   // floating point
            if (size == 8) {
                const quint64 x = u(at, 8);
                double d;
                std::memcpy(&d, &x, 8);
                v.numbers.push_back(d);
            } else if (size == 4) {
                const quint32 x = quint32(u(at, 4));
                float f;
                std::memcpy(&f, &x, 4);
                v.numbers.push_back(f);
            } else
                return std::nullopt;
        } else if (cls == 3) {   // a fixed-length string
            v.text = true;
            QByteArray b = a_data.mid(at, int(size));
            const int nul = b.indexOf('\0');
            if (nul >= 0) b.truncate(nul);
            v.texts << QString::fromUtf8(b).trimmed();
        } else
            return std::nullopt;
    }
    return v;
}

std::optional<std::pair<QString, Hdf5File::Value>> Hdf5File::attribute(qint64 at, qint64 size) const
{
    if (!fits(at, 8)) return std::nullopt;
    const int version = quint8(a_data[at]), flags = quint8(a_data[at + 1]);
    if (flags & 3) return std::nullopt;   // (a shared datatype or dataspace)
    const qint64 nameSize = qint64(u(at + 2, 2)), typeSize = qint64(u(at + 4, 2)), spaceSize = qint64(u(at + 6, 2));
    auto pad = [&](qint64 n) { return version == 1 ? (n + 7) / 8 * 8 : n; };
    qint64 p = at + 8 + (version == 3 ? 1 : 0);
    if (!fits(p, nameSize)) return std::nullopt;
    QByteArray name = a_data.mid(p, int(nameSize));
    const int nul = name.indexOf('\0');
    if (nul >= 0) name.truncate(nul);
    p += pad(nameSize);
    const qint64 type = p;
    p += pad(typeSize);
    const qint64 space = p;
    p += pad(spaceSize);
    const std::optional<Value> v = decode(type, space, p, at + size - p);
    if (!v) return std::nullopt;
    return std::pair(QString::fromUtf8(name), *v);
}

std::map<QString, Hdf5File::Value> Hdf5File::attributes(quint64 object) const
{
    std::map<QString, Value> out;
    for (const Message& m : messages(object)) {
        if (m.type == 0x0C) {
            if (auto a = attribute(m.at, m.size)) out.insert(std::move(*a));
        } else if (m.type == 0x15 && fits(m.at, 2)) {
            const int flags = quint8(a_data[m.at + 1]);
            const qint64 p = m.at + 2 + ((flags & 1) ? 2 : 0);
            const quint64 heap = u(p, a_O), tree = u(p + a_O, a_O);
            if (heap == ~quint64(0) >> (64 - 8 * a_O) || tree == ~quint64(0) >> (64 - 8 * a_O)) continue;
            for (const QByteArray& record : treeRecords(tree)) {
                // (An attribute name index's record: the heap ID first, 8 bytes.)
                const QByteArray object = heapObject(heap, record.left(8));
                if (object.isEmpty()) continue;
                Hdf5File view;
                view.a_data = object;
                view.a_O = a_O;
                view.a_L = a_L;
                if (auto a = view.attribute(0, object.size())) out.insert(std::move(*a));
            }
        }
    }
    return out;
}

std::optional<Hdf5File::Value> Hdf5File::dataset(quint64 object) const
{
    qint64 type = -1, space = -1, data = -1, size = 0;
    for (const Message& m : messages(object)) {
        if (m.type == 0x03) type = m.at;
        else if (m.type == 0x01) space = m.at;
        else if (m.type == 0x0B) return std::nullopt;   // (filtered)
        else if (m.type == 0x08 && fits(m.at, 2)) {
            const int version = quint8(a_data[m.at]), cls = quint8(a_data[m.at + 1]);
            if (version < 3) return std::nullopt;
            if (cls == 0) {
                size = qint64(u(m.at + 2, 2));
                data = m.at + 4;
            } else if (cls == 1) {
                data = qint64(u(m.at + 2, a_O));
                size = qint64(u(m.at + 2 + a_O, a_L));
            } else
                return std::nullopt;   // (chunked, virtual)
        }
    }
    if (type < 0 || space < 0 || data < 0) return std::nullopt;
    return decode(type, space, data, size);
}

QByteArray Hdf5File::heapObject(quint64 heap, const QByteArray& id) const
{
    const qint64 h = qint64(heap);
    if (id.isEmpty() || !fits(h, 4 + 1 + 2 + 2 + 1 + 4) || std::memcmp(a_data.constData() + h, "FRHP", 4) != 0) return {};
    const int filters = int(u(h + 7, 2));
    const quint64 maxManaged = u(h + 10, 4);
    qint64 p = h + 14 + a_L + a_O + a_L + a_O + 8 * a_L;   // (past the counts, to the doubling table)
    const int width = int(u(p, 2));
    const quint64 start = u(p + 2, a_L), maxDirect = u(p + 2 + a_L, a_L);
    const int maxHeapBits = int(u(p + 2 + 2 * a_L, 2));
    const quint64 rootBlock = u(p + 2 + 2 * a_L + 4, a_O);
    const int rows = int(u(p + 2 + 2 * a_L + 4 + a_O, 2));
    const int offsetBytes = (maxHeapBits + 7) / 8, lengthBytes = bytesFor(std::min(maxDirect, maxManaged));
    // (A managed object: huge ones and tiny ones - Lumerical's files have none
    // - not read.)
    const int kind = (quint8(id[0]) >> 4) & 3;
    if (kind != 0 || id.size() < 1 + offsetBytes + lengthBytes) return {};
    quint64 offset = 0, length = 0;
    for (int i = offsetBytes; i >= 1; --i) offset = (offset << 8) | quint8(id[i]);
    for (int i = lengthBytes; i >= 1; --i) length = (length << 8) | quint8(id[offsetBytes + i]);
    // Its block: the root direct, or one an indirect block's rows hold.
    const int maxDirectRows = log2Of(maxDirect) - log2Of(start) + 2;
    const int entry = a_O + (filters > 0 ? a_L + 4 : 0);
    std::function<qint64(quint64, int, quint64)> locate = [&](quint64 block, int nrows, quint64 blockOffset) -> qint64 {
        const qint64 b = qint64(block);
        if (!fits(b, 4) || std::memcmp(a_data.constData() + b, "FHIB", 4) != 0) return -1;
        const qint64 entries = b + 5 + a_O + offsetBytes;
        quint64 rowStart = blockOffset;
        const int directRows = std::min(nrows, maxDirectRows);
        for (int r = 0; r < nrows; ++r) {
            const quint64 s = start << std::max(0, r - 1);
            const quint64 span = quint64(width) * s;
            if (offset < rowStart + span) {
                const quint64 col = (offset - rowStart) / s, childOffset = rowStart + col * s;
                if (r < maxDirectRows) {
                    const quint64 child = u(entries + qint64(r * width + int(col)) * entry, a_O);
                    return qint64(child + (offset - childOffset));
                }
                const qint64 at = entries + qint64(directRows * width) * entry + qint64((r - maxDirectRows) * width + int(col)) * a_O;
                const int childRows = log2Of(s) - log2Of(start * quint64(width)) + 1;
                return locate(u(at, a_O), childRows, childOffset);
            }
            rowStart += span;
        }
        return -1;
    };
    const qint64 at = rows == 0 ? qint64(rootBlock + offset) : locate(rootBlock, rows, 0);
    if (at < 0 || !fits(at, qint64(length))) return {};
    return a_data.mid(at, int(length));
}

std::vector<QByteArray> Hdf5File::treeRecords(quint64 tree) const
{
    std::vector<QByteArray> out;
    const qint64 t = qint64(tree);
    if (!fits(t, 16) || std::memcmp(a_data.constData() + t, "BTHD", 4) != 0) return out;
    const quint64 nodeSize = u(t + 6, 4);
    const int recordSize = int(u(t + 10, 2)), depth = int(u(t + 12, 2));
    const quint64 rootNode = u(t + 16, a_O);
    const int rootRecords = int(u(t + 16 + a_O, 2));
    if (recordSize <= 0 || nodeSize <= 10) return out;
    // The child pointers' sizes by depth: their records' and totals' bytes.
    std::vector<quint64> maxRecords(std::size_t(depth) + 1), maxTotal(std::size_t(depth) + 1);
    std::vector<int> pointerSize(std::size_t(depth) + 1, 0);
    maxRecords[0] = (nodeSize - 10) / quint64(recordSize);
    maxTotal[0] = maxRecords[0];
    for (int d = 1; d <= depth; ++d) {
        pointerSize[std::size_t(d)] = a_O + bytesFor(maxRecords[std::size_t(d) - 1]) + (d > 1 ? bytesFor(maxTotal[std::size_t(d) - 1]) : 0);
        maxRecords[std::size_t(d)] = (nodeSize - 10 - quint64(pointerSize[std::size_t(d)])) / quint64(recordSize + pointerSize[std::size_t(d)]);
        maxTotal[std::size_t(d)] = maxRecords[std::size_t(d)] + (maxRecords[std::size_t(d)] + 1) * maxTotal[std::size_t(d) - 1];
    }
    std::function<void(quint64, int, int)> walk = [&](quint64 node, int records, int d) {
        const qint64 n = qint64(node);
        if (!fits(n, 6 + qint64(records) * recordSize) || out.size() > 1000000) return;
        const qint64 first = n + 6;
        for (int i = 0; i < records; ++i) out.push_back(a_data.mid(first + qint64(i) * recordSize, recordSize));
        if (d == 0) return;
        const int recBytes = bytesFor(maxRecords[std::size_t(d) - 1]);
        qint64 p = first + qint64(records) * recordSize;
        for (int c = 0; c <= records; ++c) {
            const quint64 child = u(p, a_O);
            const int childRecords = int(u(p + a_O, recBytes));
            walk(child, childRecords, d - 1);
            p += pointerSize[std::size_t(d)];
        }
    };
    walk(rootNode, rootRecords, depth);
    return out;
}

} // namespace qucs_s::fem
