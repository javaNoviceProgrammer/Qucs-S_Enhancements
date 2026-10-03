/*
 * datasetfile.cpp - a dataset's file: written as text or binary, a binary
 *                   one read, and the header of a simulator's raw file
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "datasetfile.h"
#include "config.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileDevice>
#include <QIODevice>
#include <QLocale>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace qucs_s::datasetfile {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("DatasetFile", text);
}

constexpr qint64 kDataStart = 64;   // where the values begin, after the first line
constexpr qint64 kFooterSize = 43;  // "QDSINDEX " + 16 + " " + 16 + "\n"
constexpr char kBinaryFirst[] = "<Qucs Dataset " PACKAGE_VERSION " binary>\n";

constexpr bool kLittleEndian = Q_BYTE_ORDER == Q_LITTLE_ENDIAN;

// Doubles from the file's little-endian ones (in place).
void fromLittle(double* values, qint64 count)
{
    if constexpr (!kLittleEndian)
        for (qint64 i = 0; i < count; ++i) values[i] = qFromLittleEndian(values[i]);
    Q_UNUSED(values);
    Q_UNUSED(count);
}

} // namespace

bool isBinaryHead(QByteArrayView head)
{
    const qsizetype eol = head.indexOf('\n');
    const QByteArrayView first = (eol < 0 ? head : head.first(eol)).trimmed();
    return first.startsWith("<Qucs Dataset ") && first.endsWith(" binary>");
}

bool isBinary(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    return isBinaryHead(f.read(kDataStart));
}

QByteArray numberText(double v)
{
    if (v == 0 && std::signbit(v)) return QByteArrayLiteral("-0e+00");   // (its sign kept, as a binary one keeps it)
    return QByteArray::number(v, 'e', QLocale::FloatingPointShortest);
}

bool readValue(QByteArrayView text, double* re, double* im, bool* complex)
{
    *im = 0;
    bool ok = false;
    const qsizetype j = text.indexOf('j');
    *complex = j >= 0;
    if (j < 0) {
        *re = text.toDouble(&ok);
        return ok;
    }
    if (j == 0) return false;
    const char sign = text.at(j - 1);
    if (sign != '+' && sign != '-') return false;
    const QByteArrayView rePart = text.first(j - 1);
    *re = rePart.isEmpty() ? 0 : rePart.toDouble(&ok);
    if (!rePart.isEmpty() && !ok) return false;
    const double i = text.sliced(j + 1).toDouble(&ok);
    if (!ok) return false;
    *im = sign == '-' ? -i : i;
    return true;
}

// ---------------------------------------------------------------------
void Writer::blocks(const QString& written)
{
    for (QStringView line : QStringView(written).split(QLatin1Char('\n'))) {
        line = line.trimmed();
        if (line.isEmpty()) continue;
        if (line.startsWith(QLatin1String("</"))) {
            end();
        } else if (line.startsWith(QLatin1Char('<')) && line.endsWith(QLatin1Char('>'))) {
            begin(line.sliced(1, line.size() - 2).toString());
        } else {
            text(line.toUtf8());
        }
    }
}

// ---------------------------------------------------------------------
TextWriter::TextWriter(QIODevice* out) : a_out(out)
{
    a_buffer = "<Qucs Dataset " PACKAGE_VERSION ">\n";
}

void TextWriter::begin(const QString& header)
{
    if (!a_close.isEmpty()) end();
    a_buffer += '<';
    a_buffer += header.toUtf8();
    a_buffer += ">\n";
    a_close = header.startsWith(QLatin1String("indep")) ? "</indep>\n" : "</dep>\n";
    ++a_blocks;
}

void TextWriter::real(double v)
{
    a_buffer += numberText(v);
    a_buffer += '\n';
    spill();
}

void TextWriter::complex(double re, double im)
{
    a_buffer += numberText(re);
    a_buffer += std::signbit(im) ? "-j" : "+j";
    a_buffer += numberText(std::fabs(im));
    a_buffer += '\n';
    spill();
}

void TextWriter::text(QByteArrayView value)
{
    a_buffer += value;
    a_buffer += '\n';
    spill();
}

void TextWriter::blocks(const QString& text)
{
    if (!a_close.isEmpty()) end();
    const QByteArray bytes = text.toUtf8();
    if (bytes.contains("<indep ") || bytes.contains("<dep ")) ++a_blocks;
    a_buffer += bytes;
    spill();
}

void TextWriter::end()
{
    a_buffer += a_close;
    a_close.clear();
}

void TextWriter::spill()
{
    if (a_buffer.size() < (1 << 20)) return;
    if (a_out->write(a_buffer) != a_buffer.size()) a_failed = true;
    a_buffer.clear();
}

bool TextWriter::finish(QString* error)
{
    if (!a_close.isEmpty()) end();
    if (!a_buffer.isEmpty() && a_out->write(a_buffer) != a_buffer.size()) a_failed = true;
    a_buffer.clear();
    if (a_failed && error != nullptr) *error = a_out->errorString();
    return !a_failed;
}

// ---------------------------------------------------------------------
BinaryWriter::BinaryWriter(QFileDevice* out) : a_out(out)
{
    QByteArray first(kBinaryFirst);
    first.append(QByteArray(kDataStart - first.size(), '\0'));
    if (a_out->write(first) != first.size()) {
        a_failed = true;
        a_error = a_out->errorString();
    }
    a_end = kDataStart;
}

void BinaryWriter::begin(const QString& header)
{
    if (a_open) end();
    a_header = header;
    a_open = true;
    a_complex = false;
    a_re.clear();
    a_im.clear();
}

void BinaryWriter::real(double v)
{
    if (!a_open) return;
    a_re.append(v);
    if (a_complex) a_im.append(0);
}

void BinaryWriter::complex(double re, double im)
{
    if (!a_open) return;
    // The first complex value makes the block complex, the ones before it
    // real (as text is read).
    if (!a_complex) {
        a_complex = true;
        a_im.fill(0, a_re.size());
    }
    a_re.append(re);
    a_im.append(im);
}

void BinaryWriter::text(QByteArrayView value)
{
    double re = 0, im = 0;
    bool isComplex = false;
    if (!readValue(value.trimmed(), &re, &im, &isComplex)) {
        real(std::numeric_limits<double>::quiet_NaN());   // as text is read: no number
        return;
    }
    if (isComplex)
        complex(re, im);
    else
        real(re);
}

void BinaryWriter::end()
{
    if (!a_open) return;
    a_open = false;
    const qint64 count = a_re.size();
    const qint64 offset = a_end;
    if (a_complex) {
        std::vector<double> pairs(size_t(2 * count));
        for (qint64 i = 0; i < count; ++i) {
            pairs[size_t(2 * i)] = a_re.at(i);
            pairs[size_t(2 * i + 1)] = a_im.at(i);
        }
        writeAt(offset, pairs.data(), 2 * count);
    } else {
        writeAt(offset, a_re.constData(), count);
    }
    a_index.append(Entry{a_header, offset, count, a_complex});
    a_end += count * (a_complex ? 16 : 8);
    a_re.clear();
    a_im.clear();
}

qint64 BinaryWriter::reserve(const QString& header, qint64 count, bool complex)
{
    if (a_open) end();
    const qint64 offset = a_end;
    a_index.append(Entry{header, offset, count, complex});
    a_end += count * (complex ? 16 : 8);
    return offset;
}

bool BinaryWriter::writeAt(qint64 offset, const double* values, qint64 count)
{
    if (a_failed) return false;
    if (count <= 0) return true;
    if (!a_out->seek(offset)) {
        a_failed = true;
        a_error = a_out->errorString();
        return false;
    }
    const auto put = [this](const char* bytes, qint64 size) {
        if (a_out->write(bytes, size) == size) return true;
        a_failed = true;
        a_error = a_out->errorString();
        return false;
    };
    if constexpr (kLittleEndian) return put(reinterpret_cast<const char*>(values), count * 8);
    std::vector<double> little(size_t(std::min<qint64>(count, 1 << 16)));
    for (qint64 done = 0; done < count;) {
        const qint64 n = std::min<qint64>(count - done, qint64(little.size()));
        for (qint64 i = 0; i < n; ++i) little[size_t(i)] = qToLittleEndian(values[done + i]);
        if (!put(reinterpret_cast<const char*>(little.data()), n * 8)) return false;
        done += n;
    }
    return true;
}

bool BinaryWriter::finish(QString* error)
{
    if (a_open) end();
    QByteArray index = "<index>\n";
    for (const Entry& e : std::as_const(a_index)) {
        index += '<';
        index += e.header.toUtf8();
        index += "> ";
        index += QByteArray::number(e.offset);
        index += ' ';
        index += QByteArray::number(e.count);
        index += e.complex ? " c\n" : " r\n";
    }
    index += "</index>\n";
    if (!a_failed && (!a_out->seek(a_end) || a_out->write(index) != index.size())) {
        a_failed = true;
        a_error = a_out->errorString();
    }
    const QByteArray footer = QByteArray("QDSINDEX ") + QByteArray::number(a_end, 16).rightJustified(16, '0') + ' '
                              + QByteArray::number(index.size(), 16).rightJustified(16, '0') + '\n';
    Q_ASSERT(footer.size() == kFooterSize);
    if (!a_failed && a_out->write(footer) != footer.size()) {
        a_failed = true;
        a_error = a_out->errorString();
    }
    if (a_failed && error != nullptr) *error = a_error;
    return !a_failed;
}

// ---------------------------------------------------------------------
bool BinaryReader::open(const QString& path, QString* error)
{
    close();
    const auto fail = [this, error, &path](const QString& why) {
        close();
        if (error != nullptr) *error = why.arg(QDir::toNativeSeparators(path));
        return false;
    };
    const QString damaged = tr("%1 is a binary dataset that is damaged: ");
    a_file.setFileName(path);
    if (!a_file.open(QIODevice::ReadOnly)) return fail(tr("%1 cannot be read."));
    a_size = a_file.size();
    if (a_size < kDataStart + kFooterSize) return fail(tr("%1 is not a binary dataset."));
    a_map = a_file.map(0, a_size);   // (read in pieces when it cannot be mapped)
    QByteArray head(int(kDataStart), '\0');
    if (!read(0, head.data(), kDataStart) || !isBinaryHead(head)) return fail(tr("%1 is not a binary dataset."));

    // The index: where the last line says, filling the file up to that line.
    QByteArray footer(int(kFooterSize), '\0');
    if (!read(a_size - kFooterSize, footer.data(), kFooterSize) || !footer.startsWith("QDSINDEX ") || footer.at(25) != ' '
        || !footer.endsWith('\n'))
        return fail(damaged + tr("its last line is not its index's place."));
    bool okAt = false, okLength = false;
    const qint64 indexAt = footer.mid(9, 16).toLongLong(&okAt, 16);
    const qint64 indexLength = footer.mid(26, 16).toLongLong(&okLength, 16);
    if (!okAt || !okLength || indexAt < kDataStart || indexLength < 16 || indexAt > a_size - kFooterSize
        || indexLength != a_size - kFooterSize - indexAt)
        return fail(damaged + tr("its index is not where it says."));
    QByteArray index(indexLength, '\0');
    if (!read(indexAt, index.data(), indexLength) || !index.startsWith("<index>\n") || !index.endsWith("</index>\n"))
        return fail(damaged + tr("its index cannot be read."));

    // Each block: its header, where its values are, how many, of which kind -
    // all of it within the values, before the index.
    const QList<QByteArray> lines = index.sliced(8, index.size() - 8 - 9).split('\n');
    for (const QByteArray& line : lines) {
        if (line.isEmpty()) continue;
        const qsizetype close = line.lastIndexOf('>');
        if (!line.startsWith('<') || close < 2) return fail(damaged + tr("a line of its index is no block's."));
        const QList<QByteArray> rest = line.sliced(close + 1).trimmed().split(' ');
        if (rest.size() != 3 || (rest.at(2) != "r" && rest.at(2) != "c")) return fail(damaged + tr("a line of its index is no block's."));
        Block b;
        b.header = QString::fromUtf8(line.sliced(1, close - 1));
        b.words = b.header.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        bool okOffset = false, okCount = false;
        b.offset = rest.at(0).toLongLong(&okOffset);
        b.count = rest.at(1).toLongLong(&okCount);
        b.complex = rest.at(2) == "c";
        const qint64 size = b.complex ? 16 : 8;
        if (!okOffset || !okCount || b.words.size() < 2
            || (b.words.first() != QLatin1String("indep") && b.words.first() != QLatin1String("dep")))
            return fail(damaged + tr("a line of its index is no block's."));
        if (b.offset < kDataStart || b.offset % 8 != 0 || b.offset > indexAt || b.count < 0 || b.count > (indexAt - b.offset) / size)
            return fail(damaged + tr("the values of %1 are not within it.").arg(b.name()));
        if (!a_first.contains(b.name())) a_first.insert(b.name(), int(a_blocks.size()));
        a_index.insert(b.name(), int(a_blocks.size()));
        a_blocks.append(b);
    }
    return true;
}

void BinaryReader::close()
{
    if (a_map != nullptr) a_file.unmap(const_cast<uchar*>(a_map));
    a_map = nullptr;
    a_file.close();
    a_size = 0;
    a_blocks.clear();
    a_index.clear();
    a_first.clear();
}

bool BinaryReader::read(qint64 offset, void* into, qint64 bytes) const
{
    if (offset < 0 || bytes < 0 || offset > a_size || bytes > a_size - offset) return false;
    if (a_map != nullptr) {
        std::memcpy(into, a_map + offset, size_t(bytes));
        return true;
    }
    return a_file.seek(offset) && a_file.read(static_cast<char*>(into), bytes) == bytes;
}

int BinaryReader::find(const QString& name, bool first) const
{
    const QHash<QString, int>& index = first ? a_first : a_index;
    const auto it = index.constFind(name);
    return it == index.constEnd() ? -1 : *it;
}

bool BinaryReader::values(const Block& block, QVector<double>* re, QVector<double>* im) const
{
    if (block.count > std::numeric_limits<qsizetype>::max() / 16) return false;
    if (!block.complex) {
        re->resize(block.count);
        if (im != nullptr) im->clear();
        if (!read(block.offset, re->data(), block.count * 8)) return false;
        fromLittle(re->data(), block.count);
        return true;
    }
    std::vector<double> pairs(size_t(2 * block.count));
    if (!read(block.offset, pairs.data(), block.count * 16)) return false;
    fromLittle(pairs.data(), 2 * block.count);
    re->resize(block.count);
    if (im != nullptr) im->resize(block.count);
    for (qint64 i = 0; i < block.count; ++i) {
        (*re)[i] = pairs[size_t(2 * i)];
        if (im != nullptr) (*im)[i] = pairs[size_t(2 * i + 1)];
    }
    return true;
}

bool BinaryReader::pairs(const Block& block, double* out, qint64 count) const
{
    if (count < 0 || count > block.count) return false;
    if (block.complex) {
        if (!read(block.offset, out, count * 16)) return false;
        fromLittle(out, 2 * count);
        return true;
    }
    // Real: read into the second half, then spread out, an imaginary 0 each.
    double* const half = out + count;
    if (!read(block.offset, half, count * 8)) return false;
    fromLittle(half, count);
    for (qint64 i = 0; i < count; ++i) {
        out[2 * i] = half[i];
        out[2 * i + 1] = 0;
    }
    return true;
}

// ---------------------------------------------------------------------
bool writeText(const QString& binaryPath, QIODevice* out, QString* error)
{
    BinaryReader data;
    if (!data.open(binaryPath, error)) return false;
    TextWriter text(out);
    QVector<double> re, im;
    for (const Block& b : data.blocks()) {
        if (!data.values(b, &re, &im)) {
            if (error != nullptr) *error = tr("The values of %1 cannot be read.").arg(b.name());
            return false;
        }
        text.begin(b.header);
        for (qsizetype i = 0; i < re.size(); ++i) {
            if (b.complex)
                text.complex(re.at(i), im.at(i));
            else
                text.real(re.at(i));
        }
        text.end();
    }
    return text.finish(error);
}

QStringList dependentNames(const QString& path)
{
    BinaryReader data;
    if (!data.open(path)) return {};
    QStringList names;
    for (const Block& b : data.blocks())
        if (!b.independent()) names << b.name();
    return names;
}

// ---------------------------------------------------------------------
bool readRawHeader(const QString& path, RawHeader* header)
{
    *header = RawHeader();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    // Line by line, up to the values (a header of thousands of variables
    // is long, but text).
    int declared = -1;
    bool inVariables = false;
    constexpr qint64 most = qint64(64) << 20;
    while (!f.atEnd() && f.pos() < most) {
        QByteArray line = f.readLine(1 << 16);
        if (line.endsWith('\n')) line.chop(1);
        if (line.endsWith('\r')) line.chop(1);
        if (inVariables) {
            if (header->variables.size() < declared) {
                const QList<QByteArray> words = line.simplified().split(' ');
                if (words.size() < 2) return false;
                header->variables << QString::fromUtf8(words.at(1));
                if (header->variables.size() > 1 && line.contains("dims=")) header->dims = true;
                if (header->variables.size() == declared) inVariables = false;
                continue;
            }
            inVariables = false;
        }
        if (line.contains("Flags") && line.contains("complex")) {
            header->complex = true;
        } else if (line.contains("No. Variables")) {
            const QList<QByteArray> words = line.simplified().split(' ');
            declared = words.value(2).toInt();
        } else if (line.contains("No. Points:")) {
            const QList<QByteArray> words = line.simplified().split(' ');
            header->points = words.value(2).toLongLong();
        } else if (line == "Variables:") {
            inVariables = declared > 0;
        } else if (line == "Binary:") {
            header->binary = true;
            header->dataOffset = f.pos();
            break;
        } else if (line == "Values:") {
            break;
        }
    }
    return declared > 0 && header->variables.size() == declared;
}

bool isPlainBinaryRaw(const RawHeader& header, qint64 fileSize)
{
    if (!header.binary || header.dims || header.points <= 0 || header.variables.isEmpty()) return false;
    const qint64 point = header.pointBytes();
    if (header.points > (fileSize - header.dataOffset) / point) return false;
    return header.dataOffset + header.points * point == fileSize;
}

bool transposeRaw(const QString& rawPath, const RawHeader& header, BinaryWriter& out, const QList<qint64>& offsets,
                  QString* error, qint64 bufferBytes)
{
    const int vars = int(header.variables.size());
    const int w = header.complex ? 2 : 1;
    const qint64 points = header.points;
    if (offsets.size() != vars || points <= 0) return false;
    QFile raw(rawPath);
    if (!raw.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = raw.errorString();
        return false;
    }
    const qint64 point = header.pointBytes();
    const qint64 block = points * 8 * w;   // a dependent variable's values

    // Mapped, as many variables as the buffer holds at a time, read from
    // every point - 64 of them at a time, a few pages - then each written
    // whole: the dataset written in its order, a large piece at a time.
    // (Scattered, a variable's few points at a time, a wide run's 1.2 GB
    // took 6 s to write.)
    const uchar* map = raw.map(0, raw.size());
    if (map != nullptr && block <= bufferBytes) {
        const qint64 group = std::min<qint64>(vars, std::max<qint64>(1, bufferBytes / block));
        std::vector<double> values(static_cast<size_t>(group * points * w));
        double slice[128];
        for (int g0 = 0; g0 < vars; g0 += int(group)) {
            const int g1 = int(std::min<qint64>(vars, g0 + group));
            for (int j0 = g0; j0 < g1; j0 += 64) {
                const int j1 = std::min(g1, j0 + 64);
                const uchar* at = map + header.dataOffset + qint64(j0) * 8 * w;
                for (qint64 k = 0; k < points; ++k, at += point) {
                    std::memcpy(slice, at, size_t(j1 - j0) * 8 * w);
                    fromLittle(slice, qint64(j1 - j0) * w);
                    double* to = values.data() + (qint64(j0 - g0) * points + k) * w;
                    if (w == 1) {
                        for (int j = 0; j < j1 - j0; ++j, to += points) *to = slice[j];
                    } else {
                        for (int j = 0; j < j1 - j0; ++j, to += 2 * points) {
                            to[0] = slice[2 * j];
                            to[1] = slice[2 * j + 1];
                        }
                    }
                }
            }
            for (int j = g0; j < g1; ++j) {
                double* v = values.data() + qint64(j - g0) * points * w;
                if (j == 0 && w == 2) {   // the independent variable real: its real parts
                    for (qint64 k = 0; k < points; ++k) v[k] = v[2 * k];
                    if (!out.writeAt(offsets.at(0), v, points)) return false;
                } else if (!out.writeAt(offsets.at(j), v, points * (j == 0 ? 1 : w))) {
                    return false;
                }
            }
        }
        return true;
    }

    // Else (a variable larger than the buffer, or no mapping): a few points
    // at a time, each variable's values of those points written where its
    // block has them.
    if (!raw.seek(header.dataOffset)) {
        if (error != nullptr) *error = raw.errorString();
        return false;
    }
    const qint64 tile = std::clamp<qint64>(bufferBytes / point, 1, points);
    std::vector<double> in(static_cast<size_t>(tile * vars * w));
    std::vector<double> blocks(static_cast<size_t>(tile * vars * w));
    std::vector<double> indep(static_cast<size_t>(tile));
    for (qint64 first = 0; first < points; first += tile) {
        const qint64 n = std::min(tile, points - first);
        if (raw.read(reinterpret_cast<char*>(in.data()), n * point) != n * point) {
            if (error != nullptr) *error = raw.errorString();
            return false;
        }
        fromLittle(in.data(), n * vars * w);
        for (int j0 = 0; j0 < vars; j0 += 64) {
            const int j1 = std::min(vars, j0 + 64);
            for (qint64 k = 0; k < n; ++k) {
                const double* row = in.data() + k * vars * w;
                for (int j = j0; j < j1; ++j)
                    for (int c = 0; c < w; ++c) blocks[size_t((j * tile + k) * w + c)] = row[j * w + c];
            }
        }
        // The independent variable is real (a complex one's imaginary part dropped).
        for (qint64 k = 0; k < n; ++k) indep[size_t(k)] = blocks[size_t(k * w)];
        if (!out.writeAt(offsets.at(0) + first * 8, indep.data(), n)) return false;
        for (int j = 1; j < vars; ++j)
            if (!out.writeAt(offsets.at(j) + first * 8 * w, blocks.data() + j * tile * w, n * w)) return false;
    }
    return true;
}

} // namespace qucs_s::datasetfile
