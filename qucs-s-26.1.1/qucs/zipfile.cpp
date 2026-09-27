/*
 * zipfile.cpp - ZIP archives read and written (the packages of Office's
 *               files: an .xlsx is one)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "zipfile.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace qucs_s::zip {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("zip", text);
}

// ---------------------------------------------------------------------
// Inflate (RFC 1951): stored, fixed and dynamic Huffman blocks. A code is
// read a bit at a time against the counts of the codes of each length
// (canonical Huffman codes, as zlib's "puff" reads them).

struct Bits {
    const uchar* data;
    qsizetype size;
    qsizetype pos = 0;
    quint32 buffer = 0;
    int count = 0;
    bool failed = false;

    int take(int n)
    {
        quint32 value = buffer;
        while (count < n) {
            if (pos >= size) {
                failed = true;
                return 0;
            }
            value |= quint32(data[pos++]) << count;
            count += 8;
        }
        buffer = value >> n;
        count -= n;
        return int(value & ((1u << n) - 1));
    }
};

struct Huffman {
    std::array<short, 16> count{};   // codes of each length
    std::array<short, 320> symbol{};  // the symbols, by code
};

// The codes of \a lengths; false for a set of lengths no code has (too
// many of a length). One short of complete is let be (a single distance).
bool build(Huffman& h, const short* lengths, int n)
{
    h.count.fill(0);
    for (int s = 0; s < n; ++s) h.count[lengths[s]]++;
    if (h.count[0] == n) return true;   // no codes
    int left = 1;
    for (int len = 1; len < 16; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return false;
    }
    std::array<short, 16> offs{};
    for (int len = 1; len < 15; ++len) offs[len + 1] = offs[len] + h.count[len];
    for (int s = 0; s < n; ++s)
        if (lengths[s] != 0) h.symbol[offs[lengths[s]]++] = short(s);
    return true;
}

int decode(Bits& in, const Huffman& h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        code |= in.take(1);
        if (in.failed) return -1;
        const int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

constexpr short kLengthBase[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr short kLengthExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr int kDistanceBase[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                 6145, 8193, 12289, 16385, 24577};
constexpr short kDistanceExtra[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// \a limit: the most bytes \a out may reach (a ZIP archive says how big
// each of its files is: one that inflates past that is not what it says -
// a "bomb" of a few kilobytes that would fill the memory).
bool codes(Bits& in, QByteArray& out, const Huffman& lengths, const Huffman& distances, qsizetype limit)
{
    for (;;) {
        int symbol = decode(in, lengths);
        if (symbol < 0) return false;
        if (symbol < 256) {
            if (out.size() >= limit) return false;
            out.append(char(symbol));
            continue;
        }
        if (symbol == 256) return true;   // the end of the block
        symbol -= 257;
        if (symbol >= 29) return false;
        const int length = kLengthBase[symbol] + in.take(kLengthExtra[symbol]);
        const int d = decode(in, distances);
        if (d < 0 || d >= 30) return false;
        const int distance = kDistanceBase[d] + in.take(kDistanceExtra[d]);
        if (in.failed || distance > out.size() || out.size() + length > limit) return false;
        // Copied within the buffer (a byte at a time through append() took
        // seconds a hundred megabytes); it may overlap what it copies.
        const qsizetype at = out.size();
        out.resize(at + length);
        char* p = out.data();
        const qsizetype from = at - distance;
        if (distance >= length) {
            std::memcpy(p + at, p + from, size_t(length));
        } else {
            for (int k = 0; k < length; ++k) p[at + k] = p[from + k];
        }
    }
}

bool fixedBlock(Bits& in, QByteArray& out, qsizetype limit)
{
    static const auto tables = [] {
        std::pair<Huffman, Huffman> t;
        short lengths[288];
        int s = 0;
        for (; s < 144; ++s) lengths[s] = 8;
        for (; s < 256; ++s) lengths[s] = 9;
        for (; s < 280; ++s) lengths[s] = 7;
        for (; s < 288; ++s) lengths[s] = 8;
        build(t.first, lengths, 288);
        for (s = 0; s < 30; ++s) lengths[s] = 5;
        build(t.second, lengths, 30);
        return t;
    }();
    return codes(in, out, tables.first, tables.second, limit);
}

bool dynamicBlock(Bits& in, QByteArray& out, qsizetype limit)
{
    static constexpr short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    const int nlen = in.take(5) + 257;
    const int ndist = in.take(5) + 1;
    const int ncode = in.take(4) + 4;
    if (in.failed || nlen > 286 || ndist > 30) return false;
    short lengths[320] = {};
    for (int k = 0; k < ncode; ++k) lengths[order[k]] = short(in.take(3));
    Huffman lencode, distcode;
    if (!build(lencode, lengths, 19)) return false;
    int index = 0;
    while (index < nlen + ndist) {
        int symbol = decode(in, lencode);
        if (symbol < 0) return false;
        if (symbol < 16) {
            lengths[index++] = short(symbol);
            continue;
        }
        short len = 0;
        if (symbol == 16) {
            if (index == 0) return false;
            len = lengths[index - 1];
            symbol = 3 + in.take(2);
        } else if (symbol == 17) {
            symbol = 3 + in.take(3);
        } else {
            symbol = 11 + in.take(7);
        }
        if (in.failed || index + symbol > nlen + ndist) return false;
        while (symbol--) lengths[index++] = len;
    }
    if (lengths[256] == 0) return false;   // no end of block
    if (!build(lencode, lengths, nlen) || !build(distcode, lengths + nlen, ndist)) return false;
    return codes(in, out, lencode, distcode, limit);
}

bool storedBlock(Bits& in, QByteArray& out, qsizetype limit)
{
    in.buffer = 0;   // to the next byte
    in.count = 0;
    if (in.pos + 4 > in.size) return false;
    const quint16 len = qFromLittleEndian<quint16>(in.data + in.pos);
    const quint16 nlen = qFromLittleEndian<quint16>(in.data + in.pos + 2);
    in.pos += 4;
    if (quint16(~nlen) != len || in.pos + len > in.size || out.size() + len > limit) return false;
    out.append(reinterpret_cast<const char*>(in.data + in.pos), len);
    in.pos += len;
    return true;
}

// ---------------------------------------------------------------------
quint16 le16(const QByteArray& b, qsizetype at)
{
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(b.constData()) + at);
}

quint32 le32(const QByteArray& b, qsizetype at)
{
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(b.constData()) + at);
}

void put16(QByteArray& b, quint16 v)
{
    uchar raw[2];
    qToLittleEndian(v, raw);
    b.append(reinterpret_cast<const char*>(raw), 2);
}

void put32(QByteArray& b, quint32 v)
{
    uchar raw[4];
    qToLittleEndian(v, raw);
    b.append(reinterpret_cast<const char*>(raw), 4);
}

constexpr quint32 kLocal = 0x04034b50, kCentral = 0x02014b50, kEnd = 0x06054b50;

} // namespace

quint32 crc32(const QByteArray& data)
{
    static const auto table = [] {
        std::array<quint32, 256> t{};
        for (quint32 n = 0; n < 256; ++n) {
            quint32 c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    quint32 c = 0xFFFFFFFFu;
    for (const char ch : data) c = table[(c ^ uchar(ch)) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

QByteArray inflate(const QByteArray& deflated, bool* ok, qsizetype limit)
{
    Bits in{reinterpret_cast<const uchar*>(deflated.constData()), deflated.size()};
    if (limit < 0) limit = std::numeric_limits<qsizetype>::max();
    QByteArray out;
    out.reserve(std::min<qsizetype>(deflated.size() * 4, limit));
    bool fine = true;
    for (bool last = false; !last && fine;) {
        last = in.take(1) == 1;
        const int type = in.take(2);
        if (in.failed) {
            fine = false;
            break;
        }
        switch (type) {
        case 0: fine = storedBlock(in, out, limit); break;
        case 1: fine = fixedBlock(in, out, limit); break;
        case 2: fine = dynamicBlock(in, out, limit); break;
        default: fine = false; break;
        }
    }
    if (ok != nullptr) *ok = fine;
    return fine ? out : QByteArray();
}

QByteArray deflate(const QByteArray& data)
{
    // qCompress: the size (4 bytes), then zlib's stream: a header of two
    // bytes, the DEFLATE data, and a checksum of four.
    const QByteArray z = qCompress(data, 9);
    if (z.size() < 10) return QByteArray();
    return z.mid(6, z.size() - 10);
}



namespace {
// DOS's date and time (to two seconds, local time) and back.
QDateTime fromDos(quint16 date, quint16 time)
{
    const QDate d(1980 + (date >> 9), (date >> 5) & 0x0F, date & 0x1F);
    const QTime t((time >> 11) & 0x1F, (time >> 5) & 0x3F, (time & 0x1F) * 2);
    return d.isValid() && t.isValid() ? QDateTime(d, t) : QDateTime();
}

void toDos(const QDateTime& when, quint16* date, quint16* time)
{
    // Without a time: 1 January 2000, 00:00 - a fixed one, so the same files
    // make the same archive.
    const QDateTime w = when.isValid() && when.date().year() >= 1980 && when.date().year() <= 2107
                            ? when.toLocalTime()
                            : QDateTime(QDate(2000, 1, 1), QTime(0, 0));
    *date = quint16(((w.date().year() - 1980) << 9) | (w.date().month() << 5) | w.date().day());
    *time = quint16((w.time().hour() << 11) | (w.time().minute() << 5) | (w.time().second() / 2));
}
} // namespace

QList<Item> list(const QByteArray& archive, QString* error)
{
    const auto fail = [error](const QString& why) {
        if (error != nullptr) *error = why;
        return QList<Item>();
    };
    // The end of the central directory: at the end, before a comment of at
    // most 64 KB.
    qsizetype end = -1;
    for (qsizetype at = archive.size() - 22; at >= 0 && at >= archive.size() - 22 - 0xFFFF; --at)
        if (le32(archive, at) == kEnd) {
            end = at;
            break;
        }
    if (end < 0) return fail(tr("Not a ZIP archive."));
    const quint16 entries = le16(archive, end + 10);
    const quint32 dirSize = le32(archive, end + 12);
    const quint32 dirStart = le32(archive, end + 16);
    if (dirStart == 0xFFFFFFFFu || entries == 0xFFFF) return fail(tr("A ZIP64 archive (not read)."));
    if (qsizetype(dirStart) + dirSize > end) return fail(tr("The archive's directory is damaged."));

    QList<Item> items;
    qsizetype at = dirStart;
    for (int k = 0; k < entries; ++k) {
        if (at + 46 > end || le32(archive, at) != kCentral) return fail(tr("The archive's directory is damaged."));
        Item item;
        item.madeBy = le16(archive, at + 4);
        item.flags = le16(archive, at + 8);
        item.method = le16(archive, at + 10);
        item.modified = fromDos(le16(archive, at + 14), le16(archive, at + 12));
        item.crc = le32(archive, at + 16);
        item.packed = le32(archive, at + 20);
        item.size = le32(archive, at + 24);
        const quint16 nameLength = le16(archive, at + 28);
        const quint16 extraLength = le16(archive, at + 30);
        const quint16 commentLength = le16(archive, at + 32);
        item.externalAttributes = le32(archive, at + 38);
        const quint32 local = le32(archive, at + 42);
        if (at + 46 + nameLength + extraLength + commentLength > end) return fail(tr("The archive's directory is damaged."));
        const QByteArray rawName = archive.mid(at + 46, nameLength);
        const QByteArray rawComment = archive.mid(at + 46 + nameLength + extraLength, commentLength);
        at += 46 + nameLength + extraLength + commentLength;
        const bool utf8 = item.flags & 0x800;
        item.name = utf8 ? QString::fromUtf8(rawName) : QString::fromLatin1(rawName);
        item.name.replace(QLatin1Char('\\'), QLatin1Char('/'));   // (some Windows tools)
        item.comment = utf8 ? QString::fromUtf8(rawComment) : QString::fromLatin1(rawComment);
        if (item.packed == 0xFFFFFFFFu || item.size == 0xFFFFFFFFu || local == 0xFFFFFFFFu)
            return fail(tr("A ZIP64 archive (not read)."));
        if (qsizetype(local) + 30 > archive.size() || le32(archive, local) != kLocal)
            return fail(tr("%1: its header is damaged.").arg(item.name));
        item.dataOffset = local + 30 + le16(archive, local + 26) + le16(archive, local + 28);
        if (item.dataOffset + item.packed > archive.size()) return fail(tr("%1 is cut short.").arg(item.name));
        items.append(item);
    }
    return items;
}

QByteArray packedData(const QByteArray& archive, const Item& item)
{
    if (item.dataOffset < 0 || item.dataOffset + item.packed > archive.size()) return QByteArray();
    return archive.mid(item.dataOffset, item.packed);
}

QByteArray extract(const QByteArray& archive, const Item& item, QString* error, bool* ok)
{
    const auto fail = [error, ok](const QString& why) {
        if (error != nullptr) *error = why;
        if (ok != nullptr) *ok = false;
        return QByteArray();
    };
    if (item.encrypted()) return fail(tr("%1 is encrypted.").arg(item.name));
    if (item.size > MaxEntrySize)
        return fail(tr("%1 holds more than is opened here (%2 MB inflated; at most %3 MB a file).")
                        .arg(item.name)
                        .arg(item.size / (1024 * 1024))
                        .arg(MaxEntrySize / (1024 * 1024)));
    if (item.dataOffset < 0 || item.dataOffset + item.packed > archive.size())
        return fail(tr("%1 is cut short.").arg(item.name));
    const QByteArray raw = archive.mid(item.dataOffset, item.packed);
    QByteArray data;
    if (item.method == 0) {
        data = raw;
    } else if (item.method == 8) {
        bool inflated = false;
        data = inflate(raw, &inflated, item.size);
        if (!inflated) return fail(tr("%1 cannot be inflated, or is bigger than the archive says.").arg(item.name));
    } else {
        return fail(tr("%1 is compressed in a way not read (method %2).").arg(item.name).arg(item.method));
    }
    if (quint32(data.size()) != item.size || crc32(data) != item.crc)
        return fail(tr("%1 is damaged (its checksum).").arg(item.name));
    if (ok != nullptr) *ok = true;
    return data;
}

QList<Entry> read(const QByteArray& archive, QString* error)
{
    const auto fail = [error](const QString& why) {
        if (error != nullptr) *error = why;
        return QList<Entry>();
    };
    const QList<Item> items = list(archive, error);
    if (items.isEmpty()) return {};
    QList<Entry> files;
    qint64 total = 0;
    for (const Item& item : items) {
        if (item.folder()) continue;
        // What it says it holds, checked before anything is inflated; and
        // it is inflated no further than that.
        total += item.size;
        if (item.size > MaxEntrySize || total > MaxArchiveSize)
            return fail(tr("%1 holds more than is opened here (%2 MB inflated; at most %3 MB a file, %4 MB in all).")
                            .arg(item.name)
                            .arg(item.size / (1024 * 1024))
                            .arg(MaxEntrySize / (1024 * 1024))
                            .arg(MaxArchiveSize / (1024 * 1024)));
        bool ok = false;
        const QByteArray data = extract(archive, item, error, &ok);
        if (!ok) return {};
        files.append(Entry{item.name, data});
    }
    return files;
}

QByteArray write(const QList<Part>& parts, const QString& comment)
{
    QByteArray out, directory;
    for (const Part& part : parts) {
        const Item& item = part.item;
        const QByteArray name = item.name.toUtf8();
        const QByteArray itemComment = item.comment.toUtf8();
        quint16 method = 0, flags = 0x800;   // names in UTF-8
        quint32 crc = 0, size = 0;
        QByteArray packed;
        if (part.copied) {
            // As the archive had it - packed, encrypted or in a way not
            // read -, its sizes in the header here (no descriptor after).
            method = item.method;
            flags = quint16((item.flags & ~0x0808) | 0x800);
            crc = item.crc;
            size = item.size;
            packed = part.raw;
        } else if (!item.folder()) {
            crc = crc32(part.data);
            size = quint32(part.data.size());
            packed = deflate(part.data);
            method = 8;
            if (packed.isEmpty() || packed.size() >= part.data.size()) {
                packed = part.data;
                method = 0;
            }
        }
        quint16 date = 0, time = 0;
        toDos(item.modified, &date, &time);
        const quint32 offset = quint32(out.size());
        put32(out, kLocal);
        put16(out, 20);        // version needed
        put16(out, flags);
        put16(out, method);
        put16(out, time);
        put16(out, date);
        put32(out, crc);
        put32(out, quint32(packed.size()));
        put32(out, size);
        put16(out, quint16(name.size()));
        put16(out, 0);
        out.append(name);
        out.append(packed);

        put32(directory, kCentral);
        put16(directory, item.madeBy != 0 ? item.madeBy : 20);   // made by (its system: a Unix mode kept)
        put16(directory, 20);   // needed
        put16(directory, flags);
        put16(directory, method);
        put16(directory, time);
        put16(directory, date);
        put32(directory, crc);
        put32(directory, quint32(packed.size()));
        put32(directory, size);
        put16(directory, quint16(name.size()));
        put16(directory, 0);   // extra
        put16(directory, quint16(itemComment.size()));
        put16(directory, 0);   // disk
        put16(directory, 0);   // internal attributes
        put32(directory, item.externalAttributes);
        put32(directory, offset);
        directory.append(name);
        directory.append(itemComment);
    }
    const QByteArray archiveComment = comment.toUtf8().left(0xFFFF);
    const quint32 start = quint32(out.size());
    out.append(directory);
    put32(out, kEnd);
    put16(out, 0);
    put16(out, 0);
    put16(out, quint16(parts.size()));
    put16(out, quint16(parts.size()));
    put32(out, quint32(directory.size()));
    put32(out, start);
    put16(out, quint16(archiveComment.size()));
    out.append(archiveComment);
    return out;
}

QByteArray write(const QList<Entry>& entries)
{
    QList<Part> parts;
    for (const Entry& e : entries) {
        Part part;
        part.item.name = e.name;
        part.data = e.data;
        parts.append(part);
    }
    return write(parts);
}

QString comment(const QByteArray& archive)
{
    for (qsizetype at = archive.size() - 22; at >= 0 && at >= archive.size() - 22 - 0xFFFF; --at)
        if (le32(archive, at) == kEnd) {
            const quint16 length = le16(archive, at + 20);
            return QString::fromUtf8(archive.mid(at + 22, length));
        }
    return QString();
}

} // namespace qucs_s::zip
