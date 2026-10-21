/*
 * fem_modeprofile.cpp - a mode analysis's mode on a regular grid, as files
 *                       and read back
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_modeprofile.h"

#include "fem_problem.h"
#include "fem_results.h"
#include "fem_solver.h"

#include <QCoreApplication>
#include <QDateTime>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace qucs_s::fem {

namespace {

using cd = std::complex<double>;
constexpr double Eta0 = 376.730313412;   // Ω, the vacuum's impedance
constexpr const char* Names[6] = {"Ex", "Ey", "Ez", "Hx", "Hy", "Hz"};

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::fem::ModeProfile", text);
}

// Little-endian bytes.
void put16(QByteArray& b, quint16 v)
{
    b += char(v & 0xff);
    b += char((v >> 8) & 0xff);
}
void put32(QByteArray& b, quint32 v)
{
    for (int i = 0; i < 4; ++i) b += char((v >> (8 * i)) & 0xff);
}
void putDouble(QByteArray& b, double v)
{
    quint64 bits;
    std::memcpy(&bits, &v, 8);
    for (int i = 0; i < 8; ++i) b += char((bits >> (8 * i)) & 0xff);
}
void pad8(QByteArray& b)
{
    while (b.size() % 8 != 0) b += char(0);
}

// ---------------------------------------------------------------- MATLAB 5

enum : quint32 {
    miINT8 = 1, miUINT8 = 2, miINT16 = 3, miUINT16 = 4, miINT32 = 5, miUINT32 = 6, miSINGLE = 7, miDOUBLE = 9,
    miINT64 = 12, miUINT64 = 13, miMATRIX = 14, miCOMPRESSED = 15, miUTF8 = 16, miUTF16 = 17, miUTF32 = 18
};
enum : quint32 { mxCHAR = 4, mxDOUBLE = 6 };

// A data element: its tag, its data, padded to 8.
void element(QByteArray& b, quint32 type, const QByteArray& data)
{
    put32(b, type);
    put32(b, quint32(data.size()));
    b += data;
    pad8(b);
}

QByteArray matrix(const QString& name, quint32 cls, const std::vector<int>& dims, const QByteArray& real, quint32 realType, const QByteArray* imag)
{
    QByteArray m;
    QByteArray flags;
    put32(flags, cls | (imag ? 0x0800u : 0u));
    put32(flags, 0);
    element(m, miUINT32, flags);
    QByteArray d;
    for (int v : dims) put32(d, quint32(v));
    element(m, miINT32, d);
    element(m, miINT8, name.toLatin1());
    element(m, realType, real);
    if (imag) element(m, realType, *imag);
    QByteArray out;
    element(out, miMATRIX, m);
    return out;
}

QByteArray doubles(const std::vector<double>& v)
{
    QByteArray b;
    b.reserve(qsizetype(8 * v.size()));
    for (double x : v) putDouble(b, x);
    return b;
}

QByteArray matReal(const QString& name, const std::vector<int>& dims, const std::vector<double>& v)
{
    return matrix(name, mxDOUBLE, dims, doubles(v), miDOUBLE, nullptr);
}

QByteArray matComplex(const QString& name, const std::vector<int>& dims, const std::vector<cd>& v)
{
    std::vector<double> re(v.size()), im(v.size());
    for (std::size_t i = 0; i < v.size(); ++i) {
        re[i] = v[i].real();
        im[i] = v[i].imag();
    }
    const QByteArray imag = doubles(im);
    return matrix(name, mxDOUBLE, dims, doubles(re), miDOUBLE, &imag);
}

QByteArray matText(const QString& name, const QString& text)
{
    QByteArray utf16;
    for (const QChar c : text) put16(utf16, c.unicode());
    return matrix(name, mxCHAR, {1, int(text.size())}, utf16, miUTF16, nullptr);
}

// ------------------------------------------------------------------- NumPy

QByteArray npyHeader(const QByteArray& descr, const std::vector<std::size_t>& shape)
{
    QByteArray dims;
    for (std::size_t i = 0; i < shape.size(); ++i) dims += (i ? ", " : "") + QByteArray::number(qulonglong(shape[i]));
    if (shape.size() == 1) dims += ',';
    QByteArray header = "{'descr': '" + descr + "', 'fortran_order': False, 'shape': (" + dims + "), }";
    const int pad = (64 - (10 + header.size() + 1) % 64) % 64;
    header += QByteArray(pad, ' ') + '\n';
    QByteArray out("\x93NUMPY\x01\x00", 8);
    out += char(header.size() & 0xff);
    out += char((header.size() >> 8) & 0xff);
    out += header;
    return out;
}

QByteArray npyReal(const std::vector<double>& v, const std::vector<std::size_t>& shape)
{
    return npyHeader("<f8", shape) + doubles(v);
}

QByteArray npyComplex(const std::vector<cd>& v, const std::vector<std::size_t>& shape)
{
    QByteArray b = npyHeader("<c16", shape);
    for (const cd& z : v) {
        putDouble(b, z.real());
        putDouble(b, z.imag());
    }
    return b;
}

QByteArray npyText(const QString& text)
{
    // (A 0-d array of one string, UTF-32: no pickle to load it.)
    const QList<uint> ucs4 = text.toUcs4();
    QByteArray b = npyHeader("<U" + QByteArray::number(qsizetype(std::max<qsizetype>(1, ucs4.size()))), {});
    for (uint c : ucs4) put32(b, c);
    if (ucs4.isEmpty()) put32(b, 0);
    return b;
}

quint32 crc32(const QByteArray& data)
{
    static quint32 table[256];
    static bool made = false;
    if (!made) {
        for (quint32 n = 0; n < 256; ++n) {
            quint32 c = n;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        made = true;
    }
    quint32 c = 0xFFFFFFFFu;
    for (const char ch : data) c = table[(c ^ quint8(ch)) & 0xff] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// A zip of files stored as they are (no compression).
QByteArray zipStored(const std::vector<std::pair<QByteArray, QByteArray>>& files)
{
    QByteArray out, central;
    for (const auto& file : files) {
        const QByteArray& name = file.first;
        const QByteArray& data = file.second;
        const quint32 crc = crc32(data), offset = quint32(out.size());
        auto header = [&name, &data, crc, offset](QByteArray& b, bool inCentral) {
            put32(b, inCentral ? 0x02014b50u : 0x04034b50u);
            if (inCentral) put16(b, 20);
            put16(b, 20);   // (version needed)
            put16(b, 0);    // (flags)
            put16(b, 0);    // (stored)
            put16(b, 0);    // (time)
            put16(b, 0x21); // (date: 1980-01-01)
            put32(b, crc);
            put32(b, quint32(data.size()));
            put32(b, quint32(data.size()));
            put16(b, quint16(name.size()));
            put16(b, 0);
            if (inCentral) {
                put16(b, 0);   // (comment)
                put16(b, 0);   // (disk)
                put16(b, 0);   // (internal attributes)
                put32(b, 0);   // (external attributes)
                put32(b, offset);
            }
            b += name;
        };
        header(out, false);
        out += data;
        header(central, true);
    }
    const quint32 at = quint32(out.size());
    out += central;
    put32(out, 0x06054b50u);
    put16(out, 0);
    put16(out, 0);
    put16(out, quint16(files.size()));
    put16(out, quint16(files.size()));
    put32(out, quint32(central.size()));
    put32(out, at);
    put16(out, 0);
    return out;
}

// --------------------------------------------------------------- reading

struct MatArray {
    std::vector<int> dims;
    std::vector<double> re, im;
    QString text;   // a char array's
    bool isText = false;
};

double readNumber(const char* p, quint32 type)
{
    auto u = [p](int n) {
        quint64 v = 0;
        for (int i = 0; i < n; ++i) v |= quint64(quint8(p[i])) << (8 * i);
        return v;
    };
    switch (type) {
    case miINT8: return double(qint8(u(1)));
    case miUINT8: return double(u(1));
    case miINT16: return double(qint16(u(2)));
    case miUINT16: return double(u(2));
    case miINT32: return double(qint32(u(4)));
    case miUINT32: return double(u(4));
    case miINT64: return double(qint64(u(8)));
    case miUINT64: return double(u(8));
    case miSINGLE: {
        const quint32 bits = quint32(u(4));
        float f;
        std::memcpy(&f, &bits, 4);
        return double(f);
    }
    case miDOUBLE: {
        const quint64 bits = u(8);
        double d;
        std::memcpy(&d, &bits, 8);
        return d;
    }
    default: return NAN;
    }
}

int sizeOf(quint32 type)
{
    switch (type) {
    case miINT8:
    case miUINT8: return 1;
    case miINT16:
    case miUINT16: return 2;
    case miINT32:
    case miUINT32:
    case miSINGLE: return 4;
    case miDOUBLE:
    case miINT64:
    case miUINT64: return 8;
    default: return 0;
    }
}

// A data element at \a at: its type, its data's bytes and where they are
// (the small format's in its tag); where the next starts.
struct Tag {
    quint32 type = 0, bytes = 0;
    qsizetype data = 0, next = 0;
};
bool tagAt(const QByteArray& b, qsizetype at, Tag* t)
{
    if (at + 8 > b.size()) return false;
    auto u32 = [&b](qsizetype i) {
        quint32 v = 0;
        for (int k = 0; k < 4; ++k) v |= quint32(quint8(b[i + k])) << (8 * k);
        return v;
    };
    const quint32 first = u32(at);
    if ((first >> 16) != 0) {   // (the small format: 4 bytes of data in the tag)
        t->type = first & 0xffff;
        t->bytes = first >> 16;
        t->data = at + 4;
        t->next = at + 8;
    } else {
        t->type = first;
        t->bytes = u32(at + 4);
        t->data = at + 8;
        t->next = at + 8 + ((qsizetype(t->bytes) + 7) / 8) * 8;
    }
    return t->data + qsizetype(t->bytes) <= b.size();
}

bool readMatrix(const QByteArray& b, const Tag& m, QString* name, MatArray* out, QString* error)
{
    qsizetype at = m.data;
    const qsizetype end = m.data + qsizetype(m.bytes);
    Tag t;
    if (!tagAt(b, at, &t) || t.bytes < 8) return false;
    const quint32 flags = quint8(b[t.data]) | quint32(quint8(b[t.data + 1])) << 8;
    const quint32 cls = flags & 0xff;
    const bool complex = (flags & 0x0800) != 0;
    at = t.next;
    if (!tagAt(b, at, &t)) return false;
    out->dims.clear();
    for (quint32 i = 0; i + 4 <= t.bytes; i += 4) out->dims.push_back(int(readNumber(b.constData() + t.data + i, miINT32)));
    at = t.next;
    if (!tagAt(b, at, &t)) return false;
    *name = QString::fromLatin1(b.constData() + t.data, qsizetype(t.bytes));
    at = t.next;
    if (cls == mxCHAR) {
        if (!tagAt(b, at, &t)) return false;
        const int w = t.type == miUTF8 || t.type == miUINT8 || t.type == miINT8 ? 1 : t.type == miUTF32 || t.type == miUINT32 ? 4 : 2;
        QString s;
        for (quint32 i = 0; i + quint32(w) <= t.bytes; i += quint32(w)) {
            // (A character past 16 bits, a UTF-32 text's: two of UTF-16's;
            // past Unicode's - a damaged one's - the replacement character.
            // Cast to char16_t, undefined - the fuzz of 21 October, P1.)
            const double c = readNumber(b.constData() + t.data + i, w == 1 ? miUINT8 : w == 2 ? miUINT16 : miUINT32);
            if (c >= 0 && c <= 0xffff) s += QChar(char16_t(c));
            else if (c > 0xffff && c <= 0x10ffff) s += QStringView(QChar::fromUcs4(char32_t(c)));
            else s += QChar(QChar::ReplacementCharacter);
        }
        out->text = s;
        out->isText = true;
        return true;
    }
    if (cls < 6 || cls > 15) {   // (numeric classes only: double, single, the integers)
        out->dims.clear();
        return true;
    }
    for (int part = 0; part < (complex ? 2 : 1); ++part) {
        if (at >= end || !tagAt(b, at, &t)) return false;
        const int w = sizeOf(t.type);
        if (w == 0) {
            if (error) *error = tr("%1: a type of data it does not read (%2)").arg(*name).arg(t.type);
            return false;
        }
        std::vector<double>& v = part == 0 ? out->re : out->im;
        v.resize(t.bytes / quint32(w));
        for (std::size_t i = 0; i < v.size(); ++i) v[i] = readNumber(b.constData() + t.data + qsizetype(i) * w, t.type);
        at = t.next;
    }
    if (!complex) out->im.clear();
    return true;
}

} // namespace

bool ModeProfile::at(QPointF p, cd* E, cd* H) const
{
    for (int k = 0; k < 3; ++k) E[k] = H[k] = 0;
    const int n = nx(), m = ny();
    if (n < 2 || m < 2) return false;
    const double px = p.x(), py = p.y();
    if (px < x.front() || px > x.back() || py < y.front() || py > y.back()) return false;
    const int i = std::clamp(int(std::upper_bound(x.begin(), x.end(), px) - x.begin()) - 1, 0, n - 2);
    const int j = std::clamp(int(std::upper_bound(y.begin(), y.end(), py) - y.begin()) - 1, 0, m - 2);
    const double u = (px - x[std::size_t(i)]) / (x[std::size_t(i + 1)] - x[std::size_t(i)]);
    const double v = (py - y[std::size_t(j)]) / (y[std::size_t(j + 1)] - y[std::size_t(j)]);
    const std::size_t a = std::size_t(j) * std::size_t(n) + std::size_t(i);
    const double w[4] = {(1 - u) * (1 - v), u * (1 - v), (1 - u) * v, u * v};
    const std::size_t at[4] = {a, a + 1, a + std::size_t(n), a + std::size_t(n) + 1};
    for (int c = 0; c < 6; ++c) {
        cd s = 0;
        for (int q = 0; q < 4; ++q) s += w[q] * F[std::size_t(c)][at[q]];
        (c < 3 ? E[c] : H[c - 3]) = s;
    }
    return true;
}

cd ModeProfile::flux() const
{
    const int n = nx(), m = ny();
    if (n < 2 || m < 2) return 0;
    cd sum = 0;
    for (int j = 0; j < m; ++j) {
        const double wy = 0.5 * ((j > 0 ? y[std::size_t(j)] - y[std::size_t(j - 1)] : 0) + (j + 1 < m ? y[std::size_t(j + 1)] - y[std::size_t(j)] : 0));
        for (int i = 0; i < n; ++i) {
            const double wx = 0.5 * ((i > 0 ? x[std::size_t(i)] - x[std::size_t(i - 1)] : 0) + (i + 1 < n ? x[std::size_t(i + 1)] - x[std::size_t(i)] : 0));
            const std::size_t k = std::size_t(j) * std::size_t(n) + std::size_t(i);
            sum += wx * wy * (F[0][k] * std::conj(F[4][k]) - F[1][k] * std::conj(F[3][k]));
        }
    }
    return sum;
}

ModeProfile sampleMode(const Solution& solution, int field, const std::vector<int>& pmlDomains, const std::vector<double>& x,
                       const std::vector<double>& y)
{
    ModeProfile out;
    out.x = x;
    out.y = y;
    const std::size_t N = x.size() * y.size();
    for (auto& c : out.F) c.assign(N, 0.0);
    const Locator locator(solution.mesh());
    const double scale = solution.topology().unitScale;
    PointEvaluator ev(solution);
    for (std::size_t j = 0; j < y.size(); ++j)
        for (std::size_t i = 0; i < x.size(); ++i) {
            int t = -1;
            double xi = 0, eta = 0;
            if (!locator.locate(QPointF(x[i] / scale, y[j] / scale), &t, &xi, &eta)) continue;
            ev.moveTo(t, xi, eta);
            if (std::find(pmlDomains.begin(), pmlDomains.end(), ev.domain()) != pmlDomains.end()) continue;
            const detail::ModeFields m = detail::modeFieldsAt(ev, field);
            if (!m.defined) continue;
            const std::size_t k = j * x.size() + i;
            for (int c = 0; c < 3; ++c) {
                out.F[std::size_t(c)][k] = m.E[c];
                out.F[std::size_t(c + 3)][k] = m.H[c];
            }
        }
    out.mode = solution.mode();
    return out;
}

QByteArray profilesMat(const std::vector<ModeProfile>& modes, const QString& about)
{
    QByteArray b;
    QByteArray head = QStringLiteral("MATLAB 5.0 MAT-file, Platform: Qucs-S, Created on: %1")
                          .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate))
                          .toLatin1()
                          .left(116);
    head += QByteArray(116 - head.size(), ' ');
    b += head;
    b += QByteArray(8, char(0));   // (no subsystem data)
    put16(b, 0x0100);
    b += 'I';
    b += 'M';
    if (modes.empty()) return b;
    const ModeProfile& first = modes.front();
    const int nx = first.nx(), ny = first.ny(), M = int(modes.size());
    b += matReal(QStringLiteral("x"), {nx, 1}, first.x);
    b += matReal(QStringLiteral("y"), {ny, 1}, first.y);
    const std::vector<int> dims = M > 1 ? std::vector<int>{nx, ny, M} : std::vector<int>{nx, ny};
    for (int c = 0; c < 6; ++c) {
        std::vector<cd> all;
        all.reserve(std::size_t(nx) * std::size_t(ny) * std::size_t(M));
        for (const ModeProfile& m : modes) all.insert(all.end(), m.F[std::size_t(c)].begin(), m.F[std::size_t(c)].end());
        b += matComplex(QString::fromLatin1(Names[c]), dims, all);
    }
    std::vector<cd> neff;
    std::vector<double> lambda, number, power;
    for (const ModeProfile& m : modes) {
        neff.push_back(m.neff);
        lambda.push_back(m.lambda0);
        number.push_back(m.mode);
        power.push_back(0.5 * m.flux().real());
    }
    b += matComplex(QStringLiteral("neff"), {1, M}, neff);
    b += matReal(QStringLiteral("lambda0"), {1, M}, lambda);
    b += matReal(QStringLiteral("mode"), {1, M}, number);
    b += matReal(QStringLiteral("power"), {1, M}, power);
    b += matText(QStringLiteral("about"), about);
    return b;
}

QByteArray profilesNpz(const std::vector<ModeProfile>& modes, const QString& about)
{
    std::vector<std::pair<QByteArray, QByteArray>> files;
    if (modes.empty()) return zipStored(files);
    const ModeProfile& first = modes.front();
    const std::size_t nx = std::size_t(first.nx()), ny = std::size_t(first.ny()), M = modes.size();
    files.emplace_back("x.npy", npyReal(first.x, {nx}));
    files.emplace_back("y.npy", npyReal(first.y, {ny}));
    const std::vector<std::size_t> shape = M > 1 ? std::vector<std::size_t>{M, ny, nx} : std::vector<std::size_t>{ny, nx};
    for (int c = 0; c < 6; ++c) {
        std::vector<cd> all;
        for (const ModeProfile& m : modes) all.insert(all.end(), m.F[std::size_t(c)].begin(), m.F[std::size_t(c)].end());
        files.emplace_back(QByteArray(Names[c]) + ".npy", npyComplex(all, shape));
    }
    std::vector<cd> neff;
    std::vector<double> lambda, number, power;
    for (const ModeProfile& m : modes) {
        neff.push_back(m.neff);
        lambda.push_back(m.lambda0);
        number.push_back(m.mode);
        power.push_back(0.5 * m.flux().real());
    }
    files.emplace_back("neff.npy", npyComplex(neff, {M}));
    files.emplace_back("lambda0.npy", npyReal(lambda, {M}));
    files.emplace_back("mode.npy", npyReal(number, {M}));
    files.emplace_back("power.npy", npyReal(power, {M}));
    files.emplace_back("about.npy", npyText(about));
    return zipStored(files);
}

QString profilesCsv(const std::vector<ModeProfile>& modes, const QString& about)
{
    QString out;
    for (const QString& line : about.split(QLatin1Char('\n'))) out += QStringLiteral("# %1\n").arg(line);
    auto number = [](double v) { return QString::number(v, 'g', 12); };
    for (const ModeProfile& m : modes)
        out += QStringLiteral("# mode %1: neff %2, neff_im %3 (beta/k0 = neff - j neff_im), lambda0 %4 m, power %5 W (%6)\n")
                   .arg(m.mode)
                   .arg(number(m.neff.real()), number(-m.neff.imag()), number(m.lambda0), number(0.5 * m.flux().real()), m.label);
    QStringList header = {QStringLiteral("mode"), QStringLiteral("x (m)"), QStringLiteral("y (m)")};
    for (int c = 0; c < 6; ++c) {
        const QString unit = c < 3 ? QStringLiteral("V/m") : QStringLiteral("A/m");
        header << QStringLiteral("Re %1 (%2)").arg(QString::fromLatin1(Names[c]), unit) << QStringLiteral("Im %1 (%2)").arg(QString::fromLatin1(Names[c]), unit);
    }
    out += header.join(QLatin1Char(',')) + QLatin1Char('\n');
    for (const ModeProfile& m : modes)
        for (int j = 0; j < m.ny(); ++j)
            for (int i = 0; i < m.nx(); ++i) {
                const std::size_t k = std::size_t(j) * std::size_t(m.nx()) + std::size_t(i);
                QStringList row = {QString::number(m.mode), number(m.x[std::size_t(i)]), number(m.y[std::size_t(j)])};
                for (int c = 0; c < 6; ++c) row << number(m.F[std::size_t(c)][k].real()) << number(m.F[std::size_t(c)][k].imag());
                out += row.join(QLatin1Char(',')) + QLatin1Char('\n');
            }
    return out;
}

std::optional<ModeProfile> readProfileMat(const QByteArray& data, int index, QString* error)
{
    auto fail = [error](const QString& why) -> std::optional<ModeProfile> {
        if (error) *error = why;
        return std::nullopt;
    };
    if (data.size() < 128 || data.mid(126, 2) != "IM")
        return fail(data.size() >= 128 && data.mid(126, 2) == "MI" ? tr("a big-endian .mat: not read") : tr("not a MATLAB 5 .mat file"));
    std::map<QString, MatArray> vars;
    qsizetype at = 128;
    while (at < data.size()) {
        Tag t;
        if (!tagAt(data, at, &t)) break;
        if (t.type == miCOMPRESSED)
            return fail(tr("its variables are compressed (MATLAB's save default): save it with -v6, or SciPy's savemat"));
        if (t.type == miMATRIX) {
            QString name;
            MatArray a;
            QString why;
            if (!readMatrix(data, t, &name, &a, &why)) return fail(why.isEmpty() ? tr("a variable it cannot read") : why);
            vars[name] = a;
        }
        at = t.next;
    }
    auto have = [&vars](const char* name) { return vars.count(QString::fromLatin1(name)) && !vars[QString::fromLatin1(name)].re.empty(); };
    if (!have("x") || !have("y") || !have("Ex") || !have("Ey"))
        return fail(tr("it wants x, y (m), Ex and Ey at the least"));
    ModeProfile out;
    out.x = vars[QStringLiteral("x")].re;
    out.y = vars[QStringLiteral("y")].re;
    const std::size_t nx = out.x.size(), ny = out.y.size(), N = nx * ny;
    if (nx < 2 || ny < 2) return fail(tr("its grid wants two points along x and y at the least"));
    for (std::size_t i = 1; i < nx; ++i)
        if (!(out.x[i] > out.x[i - 1])) return fail(tr("its x must rise"));
    for (std::size_t j = 1; j < ny; ++j)
        if (!(out.y[j] > out.y[j - 1])) return fail(tr("its y must rise"));
    const MatArray& ex = vars[QStringLiteral("Ex")];
    const std::size_t M = ex.re.size() / std::max<std::size_t>(1, N);
    if (ex.re.size() != M * N || M == 0) return fail(tr("Ex is not of x's and y's points (%1 by %2)").arg(nx).arg(ny));
    if (index < 1 || std::size_t(index) > M) return fail(tr("there is no mode %1: it holds %2").arg(index).arg(M));
    const std::size_t from = std::size_t(index - 1) * N;
    if (vars.count(QStringLiteral("neff"))) {
        const MatArray& n = vars[QStringLiteral("neff")];
        const std::size_t k = n.re.size() >= M ? std::size_t(index - 1) : 0;
        if (k < n.re.size()) out.neff = cd(n.re[k], k < n.im.size() ? n.im[k] : 0.0);
    }
    if (vars.count(QStringLiteral("lambda0")) && !vars[QStringLiteral("lambda0")].re.empty()) {
        const std::vector<double>& l = vars[QStringLiteral("lambda0")].re;
        out.lambda0 = l[std::min(l.size() - 1, std::size_t(index - 1))];
    }
    // (Its number as the file says, else its place.)
    out.mode = index;
    if (vars.count(QStringLiteral("mode")) && vars[QStringLiteral("mode")].re.size() >= M) {
        const double number = vars[QStringLiteral("mode")].re[std::size_t(index - 1)];
        if (number >= 1 && number < 1e6 && number == std::floor(number)) out.mode = int(number);
    }
    if (vars.count(QStringLiteral("about"))) out.label = vars[QStringLiteral("about")].text;
    bool hasH = true;
    for (int c = 0; c < 6; ++c) {
        const QString name = QString::fromLatin1(Names[c]);
        std::vector<cd>& f = out.F[std::size_t(c)];
        f.assign(N, 0.0);
        if (!vars.count(name) || vars[name].re.empty()) {
            if (c >= 3) hasH = false;
            continue;
        }
        const MatArray& a = vars[name];
        if (a.re.size() != M * N) return fail(tr("%1 is not of x's and y's points").arg(name));
        // [nx ny (M)], x the first index, as written; [ny nx (M)] - a NumPy
        // (ny, nx) array as SciPy saves it - turned round; a column as it is.
        const std::size_t d0 = a.dims.size() > 0 ? std::size_t(a.dims[0]) : 0, d1 = a.dims.size() > 1 ? std::size_t(a.dims[1]) : 0;
        const bool turned = d0 == ny && d1 == nx && nx != ny;
        if (!(d0 == nx && d1 == ny) && !turned && d0 != 1 && d1 != 1)
            return fail(tr("%1 is %2 by %3: not x's by y's points (%4 by %5)").arg(name).arg(d0).arg(d1).arg(nx).arg(ny));
        for (std::size_t j = 0; j < ny; ++j)
            for (std::size_t i = 0; i < nx; ++i) {
                const std::size_t k = from + (turned ? i * ny + j : j * nx + i);
                f[j * nx + i] = cd(a.re[k], k < a.im.size() ? a.im[k] : 0.0);
            }
    }
    if (!hasH)
        // A plane wave's: H = (neff/η0) z × E.
        for (std::size_t k = 0; k < N; ++k) {
            out.F[3][k] = -out.neff / Eta0 * out.F[1][k];
            out.F[4][k] = out.neff / Eta0 * out.F[0][k];
            out.F[5][k] = 0;
        }
    return out;
}

} // namespace qucs_s::fem
