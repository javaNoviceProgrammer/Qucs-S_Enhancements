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
#include <QtEndian>

#include <array>

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

bool codes(Bits& in, QByteArray& out, const Huffman& lengths, const Huffman& distances)
{
    for (;;) {
        int symbol = decode(in, lengths);
        if (symbol < 0) return false;
        if (symbol < 256) {
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
        if (in.failed || distance > out.size()) return false;
        const qsizetype from = out.size() - distance;
        for (int k = 0; k < length; ++k) out.append(out.at(from + k));   // it may overlap
    }
}

bool fixedBlock(Bits& in, QByteArray& out)
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
    return codes(in, out, tables.first, tables.second);
}

bool dynamicBlock(Bits& in, QByteArray& out)
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
    return codes(in, out, lencode, distcode);
}

bool storedBlock(Bits& in, QByteArray& out)
{
    in.buffer = 0;   // to the next byte
    in.count = 0;
    if (in.pos + 4 > in.size) return false;
    const quint16 len = qFromLittleEndian<quint16>(in.data + in.pos);
    const quint16 nlen = qFromLittleEndian<quint16>(in.data + in.pos + 2);
    in.pos += 4;
    if (quint16(~nlen) != len || in.pos + len > in.size) return false;
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

QByteArray inflate(const QByteArray& deflated, bool* ok)
{
    Bits in{reinterpret_cast<const uchar*>(deflated.constData()), deflated.size()};
    QByteArray out;
    out.reserve(deflated.size() * 4);
    bool fine = true;
    for (bool last = false; !last && fine;) {
        last = in.take(1) == 1;
        const int type = in.take(2);
        if (in.failed) {
            fine = false;
            break;
        }
        switch (type) {
        case 0: fine = storedBlock(in, out); break;
        case 1: fine = fixedBlock(in, out); break;
        case 2: fine = dynamicBlock(in, out); break;
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

QList<Entry> read(const QByteArray& archive, QString* error)
{
    const auto fail = [error](const QString& why) {
        if (error != nullptr) *error = why;
        return QList<Entry>();
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

    QList<Entry> files;
    qsizetype at = dirStart;
    for (int k = 0; k < entries; ++k) {
        if (at + 46 > end || le32(archive, at) != kCentral) return fail(tr("The archive's directory is damaged."));
        const quint16 flags = le16(archive, at + 8);
        const quint16 method = le16(archive, at + 10);
        const quint32 crc = le32(archive, at + 16);
        const quint32 packed = le32(archive, at + 20);
        const quint32 size = le32(archive, at + 24);
        const quint16 nameLength = le16(archive, at + 28);
        const quint16 extraLength = le16(archive, at + 30);
        const quint16 commentLength = le16(archive, at + 32);
        const quint32 local = le32(archive, at + 42);
        const QByteArray rawName = archive.mid(at + 46, nameLength);
        at += 46 + nameLength + extraLength + commentLength;

        Entry entry;
        entry.name = (flags & 0x800) ? QString::fromUtf8(rawName) : QString::fromLatin1(rawName);
        if (entry.name.endsWith(QLatin1Char('/'))) continue;   // a folder
        if (flags & 0x1) return fail(tr("%1 is encrypted.").arg(entry.name));
        if (packed == 0xFFFFFFFFu || size == 0xFFFFFFFFu || local == 0xFFFFFFFFu)
            return fail(tr("A ZIP64 archive (not read)."));
        if (qsizetype(local) + 30 > archive.size() || le32(archive, local) != kLocal)
            return fail(tr("%1: its header is damaged.").arg(entry.name));
        const qsizetype data = local + 30 + le16(archive, local + 26) + le16(archive, local + 28);
        if (data + packed > archive.size()) return fail(tr("%1 is cut short.").arg(entry.name));
        const QByteArray raw = archive.mid(data, packed);
        if (method == 0) {
            entry.data = raw;
        } else if (method == 8) {
            bool ok = false;
            entry.data = inflate(raw, &ok);
            if (!ok) return fail(tr("%1 cannot be inflated.").arg(entry.name));
        } else {
            return fail(tr("%1 is compressed in a way not read (method %2).").arg(entry.name).arg(method));
        }
        if (quint32(entry.data.size()) != size || crc32(entry.data) != crc)
            return fail(tr("%1 is damaged (its checksum).").arg(entry.name));
        files.append(entry);
    }
    return files;
}

QByteArray write(const QList<Entry>& entries)
{
    QByteArray out, directory;
    // 1 January 2000, 00:00, in DOS's format: a fixed time, so the same
    // files make the same archive.
    const quint16 time = 0, date = quint16(((2000 - 1980) << 9) | (1 << 5) | 1);
    for (const Entry& e : entries) {
        const QByteArray name = e.name.toUtf8();
        const quint32 crc = crc32(e.data);
        QByteArray packed = deflate(e.data);
        quint16 method = 8;
        if (packed.isEmpty() || packed.size() >= e.data.size()) {
            packed = e.data;
            method = 0;
        }
        const quint32 offset = quint32(out.size());
        put32(out, kLocal);
        put16(out, 20);        // version needed
        put16(out, 0x800);     // names in UTF-8
        put16(out, method);
        put16(out, time);
        put16(out, date);
        put32(out, crc);
        put32(out, quint32(packed.size()));
        put32(out, quint32(e.data.size()));
        put16(out, quint16(name.size()));
        put16(out, 0);
        out.append(name);
        out.append(packed);

        put32(directory, kCentral);
        put16(directory, 20);   // made by
        put16(directory, 20);   // needed
        put16(directory, 0x800);
        put16(directory, method);
        put16(directory, time);
        put16(directory, date);
        put32(directory, crc);
        put32(directory, quint32(packed.size()));
        put32(directory, quint32(e.data.size()));
        put16(directory, quint16(name.size()));
        put16(directory, 0);   // extra
        put16(directory, 0);   // comment
        put16(directory, 0);   // disk
        put16(directory, 0);   // internal attributes
        put32(directory, 0);   // external attributes
        put32(directory, offset);
        directory.append(name);
    }
    const quint32 start = quint32(out.size());
    out.append(directory);
    put32(out, kEnd);
    put16(out, 0);
    put16(out, 0);
    put16(out, quint16(entries.size()));
    put16(out, quint16(entries.size()));
    put32(out, quint32(directory.size()));
    put32(out, start);
    put16(out, 0);
    return out;
}

} // namespace qucs_s::zip
