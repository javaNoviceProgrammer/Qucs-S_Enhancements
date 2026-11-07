/*
 * pdfvector.cpp - the vector art of a PDF's page (pdfvector.h)
 *
 * The file's objects: by its cross-reference tables or streams, and where
 * those do not hold, by a scan of the file for its objects. A page's content:
 * the path, colour and state operators followed, form XObjects entered, text
 * and shadings passed over. Everything here reads what a stranger may have
 * written, so every length, count and depth is checked, and a read that goes
 * past the file ends in "no" rather than in a crash.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "pdfvector.h"

#include "zipfile.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QMap>
#include <QSet>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace qucs_s::pdf::vec {

namespace {

constexpr int MostNesting = 64;       // arrays and dictionaries within each other
constexpr int MostPagesWalked = 200000;
constexpr qint64 MostMilliseconds = 30000;
constexpr int MostOperands = 4096;

struct Obj;
using Dict = QMap<QByteArray, Obj>;

struct Obj {
    enum Type { Null, Bool, Number, String, Name, Array, Dictionary, Stream, Ref, Keyword };
    Type type = Null;
    double number = 0;           // Bool too; Ref: the object's number
    QByteArray text;             // String, Name, Keyword
    std::shared_ptr<QList<Obj>> array;
    std::shared_ptr<Dict> dict;  // Dictionary, Stream
    qsizetype start = -1;        // Stream: where its data starts
    qsizetype length = -1;       // Stream: its raw length (-1: not yet known)
    // A stream in memory, not in the file (never made here but by tests of it).

    bool isNumber() const { return type == Number; }
    bool isName(const char* n) const { return type == Name && text == n; }
    bool isDict() const { return type == Dictionary || type == Stream; }
};

const Obj kNull;

// PDF's white space and delimiters.
inline bool isSpace(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\0'; }
inline bool isDelimiter(char c) { return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '%'; }
inline bool isRegular(char c) { return !isSpace(c) && !isDelimiter(c); }
inline int hexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// A number the quick way: PDF has no exponents, and a content stream is
// mostly numbers. False when \a from .. \a to is not one.
bool parseNumber(const char* from, const char* to, double* value)
{
    if (from >= to) return false;
    const char* p = from;
    bool negative = false;
    while (p < to && (*p == '+' || *p == '-')) {
        negative = negative != (*p == '-');
        ++p;
    }
    double v = 0;
    int digits = 0;
    for (; p < to && *p >= '0' && *p <= '9'; ++p, ++digits) v = v * 10 + (*p - '0');
    if (p < to && *p == '.') {
        ++p;
        double unit = 0.1;
        for (; p < to && *p >= '0' && *p <= '9'; ++p, ++digits, unit *= 0.1) v += (*p - '0') * unit;
    }
    // (Some writers end a number with a stray minus: "0.25-".)
    while (p < to && *p == '-') ++p;
    if (p != to || digits == 0) return false;
    if (!std::isfinite(v)) return false;
    *value = negative ? -v : v;
    return true;
}

// Reads objects from a run of bytes.
class Lexer
{
public:
    Lexer(const char* begin, const char* end) : a_begin(begin), a_p(begin), a_end(end) {}

    void skipSpace()
    {
        while (a_p < a_end) {
            if (isSpace(*a_p)) ++a_p;
            else if (*a_p == '%') {
                while (a_p < a_end && *a_p != '\n' && *a_p != '\r') ++a_p;
            } else break;
        }
    }
    bool atEnd()
    {
        skipSpace();
        return a_p >= a_end;
    }
    const char* here() const { return a_p; }
    void seek(const char* p) { a_p = std::clamp(p, a_begin, a_end); }
    qsizetype offset() const { return a_p - a_begin; }

    /// The next object; a word that is no object comes as a Keyword. With
    /// \a refs, "12 0 R" is a reference. False at the end or on nonsense.
    bool next(Obj* out, bool refs, int depth = 0)
    {
        skipSpace();
        if (a_p >= a_end || depth > MostNesting) return false;
        const char c = *a_p;
        if (c == '/') return name(out);
        if (c == '(') return literal(out);
        if (c == '<') {
            if (a_p + 1 < a_end && a_p[1] == '<') return dictionary(out, refs, depth);
            return hex(out);
        }
        if (c == '[') {
            ++a_p;
            out->type = Obj::Array;
            out->array = std::make_shared<QList<Obj>>();
            for (;;) {
                skipSpace();
                if (a_p >= a_end) return true;   // (cut short: what there is)
                if (*a_p == ']') {
                    ++a_p;
                    return true;
                }
                Obj item;
                if (!next(&item, refs, depth + 1)) return false;
                if (item.type == Obj::Keyword) continue;   // (a stray word in an array)
                out->array->append(std::move(item));
                if (out->array->size() > 4000000) return false;
            }
        }
        if (c == ')' || c == '>' || c == ']' || c == '{' || c == '}') {
            // Stray: passed over as a word of one, for the caller to ignore.
            ++a_p;
            if (c == '>' && a_p < a_end && *a_p == '>') ++a_p;
            out->type = Obj::Keyword;
            out->text = QByteArray(1, c);
            return true;
        }
        const char* start = a_p;
        while (a_p < a_end && isRegular(*a_p)) ++a_p;
        double v = 0;
        if (parseNumber(start, a_p, &v)) {
            out->type = Obj::Number;
            out->number = v;
            if (refs && v >= 0 && v == std::floor(v)) {
                // "12 0 R"?
                const char* back = a_p;
                skipSpace();
                const char* s2 = a_p;
                while (a_p < a_end && *a_p >= '0' && *a_p <= '9') ++a_p;
                if (a_p > s2) {
                    skipSpace();
                    if (a_p < a_end && *a_p == 'R' && (a_p + 1 == a_end || !isRegular(a_p[1]))) {
                        ++a_p;
                        out->type = Obj::Ref;
                        return true;
                    }
                }
                a_p = back;
            }
            return true;
        }
        const qsizetype size = a_p - start;
        if (size == 4 && std::memcmp(start, "true", 4) == 0) {
            out->type = Obj::Bool;
            out->number = 1;
        } else if (size == 5 && std::memcmp(start, "false", 5) == 0) {
            out->type = Obj::Bool;
            out->number = 0;
        } else if (size == 4 && std::memcmp(start, "null", 4) == 0) {
            out->type = Obj::Null;
        } else {
            out->type = Obj::Keyword;
            out->text = QByteArray(start, size);
        }
        return true;
    }

private:
    bool name(Obj* out)
    {
        ++a_p;
        const char* start = a_p;
        while (a_p < a_end && isRegular(*a_p)) ++a_p;
        out->type = Obj::Name;
        out->text = QByteArray(start, a_p - start);
        // #xx escapes, where there are any.
        if (out->text.contains('#')) {
            QByteArray clean;
            for (qsizetype i = 0; i < out->text.size(); ++i) {
                if (out->text.at(i) == '#' && i + 2 < out->text.size() && hexValue(out->text.at(i + 1)) >= 0 && hexValue(out->text.at(i + 2)) >= 0) {
                    clean.append(char(hexValue(out->text.at(i + 1)) * 16 + hexValue(out->text.at(i + 2))));
                    i += 2;
                } else clean.append(out->text.at(i));
            }
            out->text = clean;
        }
        return true;
    }
    bool literal(Obj* out)
    {
        ++a_p;
        int depth = 1;
        QByteArray s;
        while (a_p < a_end) {
            char c = *a_p++;
            if (c == '\\' && a_p < a_end) {
                c = *a_p++;
                switch (c) {
                case 'n': s.append('\n'); break;
                case 'r': s.append('\r'); break;
                case 't': s.append('\t'); break;
                case 'b': s.append('\b'); break;
                case 'f': s.append('\f'); break;
                case '\r':
                    if (a_p < a_end && *a_p == '\n') ++a_p;
                    break;
                case '\n': break;
                default:
                    if (c >= '0' && c <= '7') {
                        int v = c - '0';
                        for (int k = 0; k < 2 && a_p < a_end && *a_p >= '0' && *a_p <= '7'; ++k) v = v * 8 + (*a_p++ - '0');
                        s.append(char(v & 0xFF));
                    } else s.append(c);
                }
            } else if (c == '(') {
                ++depth;
                s.append(c);
            } else if (c == ')') {
                if (--depth == 0) break;
                s.append(c);
            } else s.append(c);
        }
        out->type = Obj::String;
        out->text = std::move(s);
        return true;
    }
    bool hex(Obj* out)
    {
        ++a_p;
        QByteArray s;
        int high = -1;
        while (a_p < a_end && *a_p != '>') {
            const int v = hexValue(*a_p++);
            if (v < 0) continue;
            if (high < 0) high = v;
            else {
                s.append(char(high * 16 + v));
                high = -1;
            }
        }
        if (a_p < a_end) ++a_p;
        if (high >= 0) s.append(char(high * 16));
        out->type = Obj::String;
        out->text = std::move(s);
        return true;
    }
    bool dictionary(Obj* out, bool refs, int depth)
    {
        a_p += 2;
        out->type = Obj::Dictionary;
        out->dict = std::make_shared<Dict>();
        for (;;) {
            skipSpace();
            if (a_p >= a_end) return true;
            if (*a_p == '>') {
                ++a_p;
                if (a_p < a_end && *a_p == '>') ++a_p;
                return true;
            }
            Obj key;
            if (!next(&key, refs, depth + 1)) return false;
            if (key.type != Obj::Name) {
                // A word where a key should be: "endobj", the dictionary not
                // closed, ends it; anything else is passed over.
                if (key.type == Obj::Keyword && (key.text == "endobj" || key.text == "stream")) {
                    a_p -= key.text.size();
                    return true;
                }
                continue;
            }
            Obj value;
            if (!next(&value, refs, depth + 1)) return false;
            if (value.type == Obj::Keyword) continue;
            out->dict->insert(key.text, std::move(value));
            if (out->dict->size() > 200000) return false;
        }
    }

    const char* a_begin;
    const char* a_p;
    const char* a_end;
};

// LZW, as PDF has it (9 to 12 bits, a clear code, early change).
QByteArray lzwDecode(const QByteArray& in, int earlyChange, bool* ok)
{
    struct Entry {
        int previous;
        int size;
        uchar last;
        uchar first;
    };
    QByteArray out;
    QList<Entry> table(4097);
    for (int i = 0; i < 256; ++i) table[i] = Entry{-1, 1, uchar(i), uchar(i)};
    int next = 258, width = 9, previous = -1;
    quint32 buffer = 0;
    int bits = 0;
    for (const char ch : in) {
        buffer = (buffer << 8) | uchar(ch);
        bits += 8;
        while (bits >= width) {
            const int code = int((buffer >> (bits - width)) & ((1u << width) - 1));
            bits -= width;
            if (code == 256) {
                next = 258;
                width = 9;
                previous = -1;
                continue;
            }
            if (code == 257) {
                *ok = true;
                return out;
            }
            if (code > next || (code == next && previous < 0) || (code >= 258 && code < next && table.at(code).size <= 0)) {
                *ok = false;
                return out;
            }
            if (out.size() > MostDecoded) {
                *ok = false;
                return {};
            }
            const int size = code < next ? table.at(code).size : table.at(previous).size + 1;
            const qsizetype at = out.size();
            out.resize(at + size);
            uchar* dst = reinterpret_cast<uchar*>(out.data()) + at;
            int walk = code < next ? code : previous;
            int pos = size - 1;
            if (code >= next) dst[pos--] = table.at(previous).first;
            while (walk >= 0 && pos >= 0) {
                dst[pos--] = table.at(walk).last;
                walk = table.at(walk).previous;
            }
            if (previous >= 0 && next < 4096) {
                table[next] = Entry{previous, table.at(previous).size + 1, dst[0], table.at(previous).first};
                ++next;
            }
            previous = code;
            if (next + earlyChange >= 2048) width = 12;
            else if (next + earlyChange >= 1024) width = 11;
            else if (next + earlyChange >= 512) width = 10;
            else width = 9;
        }
    }
    *ok = true;   // (no end code: what there is)
    return out;
}

QByteArray ascii85Decode(const QByteArray& in)
{
    QByteArray out;
    quint32 tuple = 0;
    int count = 0;
    for (const char c : in) {
        if (isSpace(c)) continue;
        if (c == '~') break;
        if (c == 'z' && count == 0) {
            out.append(4, '\0');
            continue;
        }
        if (c < '!' || c > 'u') continue;
        tuple = tuple * 85 + quint32(c - '!');
        if (++count == 5) {
            for (int k = 3; k >= 0; --k) out.append(char((tuple >> (8 * k)) & 0xFF));
            tuple = 0;
            count = 0;
        }
    }
    if (count > 1) {
        for (int k = count; k < 5; ++k) tuple = tuple * 85 + 84;
        for (int k = 3; k > 4 - count; --k) out.append(char((tuple >> (8 * k)) & 0xFF));
    }
    return out;
}

QByteArray hexDecode(const QByteArray& in)
{
    QByteArray out;
    int high = -1;
    for (const char c : in) {
        if (c == '>') break;
        const int v = hexValue(c);
        if (v < 0) continue;
        if (high < 0) high = v;
        else {
            out.append(char(high * 16 + v));
            high = -1;
        }
    }
    if (high >= 0) out.append(char(high * 16));
    return out;
}

QByteArray runLengthDecode(const QByteArray& in)
{
    QByteArray out;
    for (qsizetype i = 0; i < in.size();) {
        const int n = uchar(in.at(i++));
        if (n == 128) break;
        if (n < 128) {
            const qsizetype count = std::min<qsizetype>(n + 1, in.size() - i);
            out.append(in.constData() + i, count);
            i += count;
        } else if (i < in.size()) {
            out.append(257 - n, in.at(i++));
        }
        if (out.size() > MostDecoded) return {};
    }
    return out;
}

// The predictors of a Flate or LZW stream: TIFF's 2 and PNG's.
bool unpredict(QByteArray* data, int predictor, int colors, int bits, int columns)
{
    if (predictor <= 1) return true;
    if (colors < 1 || colors > 64 || bits < 1 || bits > 16 || columns < 1 || columns > (1 << 26)) return false;
    const qsizetype bytesPerPixel = std::max<qsizetype>(1, colors * bits / 8);
    const qsizetype rowBytes = (qsizetype(colors) * bits * columns + 7) / 8;
    if (rowBytes <= 0 || rowBytes > (qsizetype(1) << 28)) return false;
    if (predictor == 2) {
        if (bits != 8) return true;   // (only what a cross-reference stream would use)
        uchar* d = reinterpret_cast<uchar*>(data->data());
        const qsizetype rows = data->size() / rowBytes;
        for (qsizetype r = 0; r < rows; ++r)
            for (qsizetype i = bytesPerPixel; i < rowBytes; ++i) d[r * rowBytes + i] = uchar(d[r * rowBytes + i] + d[r * rowBytes + i - bytesPerPixel]);
        return true;
    }
    const qsizetype rows = (data->size() + rowBytes) / (rowBytes + 1);
    QByteArray out(rows * rowBytes, '\0');
    const uchar* in = reinterpret_cast<const uchar*>(data->constData());
    uchar* o = reinterpret_cast<uchar*>(out.data());
    qsizetype written = 0;
    for (qsizetype r = 0; r < rows; ++r) {
        const qsizetype from = r * (rowBytes + 1);
        if (from >= data->size()) break;
        const int type = in[from];
        const qsizetype n = std::min(rowBytes, data->size() - from - 1);
        const uchar* src = in + from + 1;
        uchar* dst = o + r * rowBytes;
        const uchar* up = r > 0 ? dst - rowBytes : nullptr;
        for (qsizetype i = 0; i < n; ++i) {
            const int a = i >= bytesPerPixel ? dst[i - bytesPerPixel] : 0;
            const int b = up != nullptr ? up[i] : 0;
            const int c = up != nullptr && i >= bytesPerPixel ? up[i - bytesPerPixel] : 0;
            int v = src[i];
            switch (type) {
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: {
                const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                v += pa <= pb && pa <= pc ? a : (pb <= pc ? b : c);
                break;
            }
            default: break;
            }
            dst[i] = uchar(v);
        }
        written = r * rowBytes + n;
    }
    out.truncate(written);
    *data = std::move(out);
    return true;
}

} // namespace

struct Reader::Impl {
    struct Entry {
        qsizetype offset = -1;   // in the file; -1: in an object stream
        int stream = -1;         // the object stream
        int index = -1;          // its place there
    };

    QByteArray bytes;
    const char* base = nullptr;   // where the file's offsets count from
    qsizetype size = 0;
    QHash<int, Entry> xref;
    Dict trailer;
    bool rebuilt = false;
    QHash<int, Obj> objects;
    QSet<int> reading;            // being parsed: a /Length that is its own stream
    QHash<int, QHash<int, Obj>> compressed;   // an object stream's objects, by number
    QList<int> pageList;          // the page tree's leaves
    QList<Dict> pageInherited;    // what each inherits, resolved

    // --- The cross-reference ---------------------------------------------

    bool load()
    {
        size = bytes.size();
        base = bytes.constData();
        // (Bytes before %PDF: offsets count from there.)
        const qsizetype header = bytes.left(1024).indexOf("%PDF-");
        if (header > 0) {
            base += header;
            size -= header;
        }
        if (!readXrefChain() || !trailer.contains("Root") || !loadPages()) {
            rebuild();
            if (!loadPages()) return false;
        }
        return true;
    }

    bool readXrefChain()
    {
        const qsizetype from = std::max<qsizetype>(0, size - 2048);
        const QByteArrayView tail(base + from, size - from);
        const qsizetype at = tail.lastIndexOf("startxref");
        if (at < 0) return false;
        Lexer lex(base + from + at + 9, base + size);
        Obj number;
        if (!lex.next(&number, false) || !number.isNumber()) return false;
        qsizetype offset = qsizetype(number.number);
        QSet<qsizetype> seen;
        bool any = false;
        while (offset >= 0 && offset < size && !seen.contains(offset) && seen.size() < 4096) {
            seen.insert(offset);
            Dict part;
            if (!readXrefSection(offset, &part)) return any && trailer.contains("Root");
            any = true;
            for (auto i = part.cbegin(); i != part.cend(); ++i)
                if (!trailer.contains(i.key())) trailer.insert(i.key(), i.value());
            // A hybrid file's stream first.
            if (const auto stm = part.constFind("XRefStm"); stm != part.cend() && stm->isNumber()) {
                const qsizetype at2 = qsizetype(stm->number);
                if (at2 > 0 && at2 < size && !seen.contains(at2)) {
                    seen.insert(at2);
                    Dict ignored;
                    readXrefSection(at2, &ignored);
                }
            }
            const auto prev = part.constFind("Prev");
            if (prev == part.cend() || !prev->isNumber()) break;
            offset = qsizetype(prev->number);
        }
        return any;
    }

    bool readXrefSection(qsizetype offset, Dict* out)
    {
        Lexer lex(base + offset, base + size);
        lex.skipSpace();
        if (lex.here() + 4 <= base + size && std::memcmp(lex.here(), "xref", 4) == 0) {
            lex.seek(lex.here() + 4);
            for (;;) {
                Obj first;
                if (!lex.next(&first, false)) return false;
                if (first.type == Obj::Keyword) {
                    if (first.text != "trailer") return false;
                    break;
                }
                Obj count;
                if (!first.isNumber() || !lex.next(&count, false) || !count.isNumber()) return false;
                const qint64 n = qint64(count.number), start = qint64(first.number);
                if (n < 0 || n > 20000000 || start < 0 || start > (qint64(1) << 31)) return false;
                for (qint64 k = 0; k < n; ++k) {
                    Obj off, gen, kind;
                    if (!lex.next(&off, false) || !lex.next(&gen, false) || !lex.next(&kind, false)) return false;
                    if (!off.isNumber() || kind.type != Obj::Keyword) return false;
                    const int num = int(start + k);
                    if (kind.text == "n" && !xref.contains(num)) {
                        Entry e;
                        e.offset = qsizetype(off.number);
                        xref.insert(num, e);
                    } else if (kind.text == "f" && !xref.contains(num)) {
                        xref.insert(num, Entry{-2, -1, -1});   // free
                    }
                }
            }
            Obj tr;
            if (!lex.next(&tr, true) || tr.type != Obj::Dictionary) return false;
            *out = *tr.dict;
            return true;
        }
        // A cross-reference stream: an object there.
        Obj num, gen, word;
        if (!lex.next(&num, false) || !lex.next(&gen, false) || !lex.next(&word, false)) return false;
        if (word.type != Obj::Keyword || word.text != "obj") return false;
        Obj stream;
        if (!parseBody(lex, &stream) || stream.type != Obj::Stream) return false;
        const Obj& type = get(*stream.dict, "Type");
        if (!type.isName("XRef")) return false;
        const QByteArray data = decoded(stream);
        const Obj widths = resolve(get(*stream.dict, "W"));
        if (widths.type != Obj::Array || widths.array->size() < 3) return false;
        const int w0 = int(widths.array->at(0).number), w1 = int(widths.array->at(1).number), w2 = int(widths.array->at(2).number);
        if (w0 < 0 || w1 < 0 || w2 < 0 || w0 > 8 || w1 > 8 || w2 > 8 || w0 + w1 + w2 <= 0) return false;
        QList<qint64> index;
        const Obj idx = resolve(get(*stream.dict, "Index"));
        if (idx.type == Obj::Array)
            for (const Obj& v : *idx.array) index << qint64(v.number);
        else index << 0 << qint64(resolve(get(*stream.dict, "Size")).number);
        const uchar* p = reinterpret_cast<const uchar*>(data.constData());
        const qsizetype width = w0 + w1 + w2;
        qsizetype at = 0;
        const auto field = [&](int bytes_) {
            qint64 v = 0;
            for (int k = 0; k < bytes_; ++k) v = (v << 8) | p[at++];
            return v;
        };
        for (qsizetype pair = 0; pair + 1 < index.size(); pair += 2) {
            const qint64 start = index.at(pair), n = index.at(pair + 1);
            if (start < 0 || n < 0 || start > (qint64(1) << 31)) return false;
            for (qint64 k = 0; k < n && at + width <= data.size(); ++k) {
                const qint64 type2 = w0 == 0 ? 1 : field(w0);
                const qint64 f1 = field(w1), f2 = field(w2);
                const int number = int(start + k);
                if (xref.contains(number)) continue;
                if (type2 == 1) {
                    Entry e;
                    e.offset = qsizetype(f1);
                    xref.insert(number, e);
                } else if (type2 == 2) {
                    xref.insert(number, Entry{-1, int(f1), int(f2)});
                } else if (type2 == 0) {
                    xref.insert(number, Entry{-2, -1, -1});
                }
            }
        }
        *out = *stream.dict;
        return true;
    }

    // Finds the objects by their "12 0 obj" - for a file whose cross-reference
    // is wrong or gone. The later of two with one number holds.
    void rebuild()
    {
        if (rebuilt) return;
        rebuilt = true;
        xref.clear();
        objects.clear();
        compressed.clear();
        QHash<int, Entry> found;
        Dict last;
        const char* const start = base;
        const char* const end = base + size;
        const char* p = start;
        while (p < end) {
            const char* hit = static_cast<const char*>(std::memchr(p, 'o', end - p));
            if (hit == nullptr) break;
            p = hit + 1;
            if (hit + 3 > end || hit[1] != 'b' || hit[2] != 'j') {
                continue;
            }
            if (hit > start && hit[-1] == 'd') continue;   // endobj
            // Back over "12 0 ".
            const char* q = hit;
            const auto backSpace = [&] {
                while (q > start && isSpace(q[-1])) --q;
            };
            const auto backDigits = [&] {
                const char* e = q;
                while (q > start && q[-1] >= '0' && q[-1] <= '9') --q;
                return q < e;
            };
            backSpace();
            if (!backDigits()) continue;
            backSpace();
            const char* numberEnd = q;
            if (!backDigits()) continue;
            double number = 0;
            if (!parseNumber(q, numberEnd, &number) || number > (1 << 28)) continue;
            Entry e;
            e.offset = q - start;
            const int num = int(number);
            found.insert(num, e);
            // An object stream: what it holds counts from here on.
            Lexer lex(hit + 3, end);
            Obj body;
            xref = found;
            const char* after = hit + 3;
            if (parseBody(lex, &body)) {
                after = lex.here();
                if (body.type == Obj::Stream) {
                    const Obj& type = get(*body.dict, "Type");
                    if (type.isName("ObjStm")) {
                        const QList<int> numbers = streamObjectNumbers(body);
                        for (int i = 0; i < numbers.size(); ++i) found.insert(numbers.at(i), Entry{-1, num, i});
                        xref = found;
                    } else if (type.isName("XRef") && body.dict->contains("Root")) {
                        last = *body.dict;
                    }
                }
            }
            p = std::max(p, after);
        }
        xref = found;
        objects.clear();
        compressed.clear();
        // The newest trailer, else a catalog.
        const QByteArrayView all(start, size);
        for (qsizetype at = all.lastIndexOf("trailer"); at >= 0; at = at > 0 ? all.lastIndexOf("trailer", at - 1) : -1) {
            Lexer lex(start + at + 7, end);
            Obj t;
            if (lex.next(&t, true) && t.type == Obj::Dictionary && t.dict->contains("Root")) {
                last = *t.dict;
                break;
            }
        }
        trailer = last;
        const Obj root = resolve(get(trailer, "Root"));
        if (!root.isDict() || !root.dict->contains("Pages")) {
            trailer.remove("Root");
            for (auto i = xref.cbegin(); i != xref.cend(); ++i) {
                const Obj o = object(i.key());
                if (o.isDict() && get(*o.dict, "Type").isName("Catalog")) {
                    Obj ref;
                    ref.type = Obj::Ref;
                    ref.number = i.key();
                    trailer.insert("Root", ref);
                    break;
                }
            }
        }
    }

    // --- Objects ---------------------------------------------------------

    static const Obj& get(const Dict& d, const char* key)
    {
        const auto i = d.constFind(key);
        return i == d.cend() ? kNull : *i;
    }

    // What follows "obj": the value and, after a dictionary, a stream.
    bool parseBody(Lexer& lex, Obj* out)
    {
        if (!lex.next(out, true)) return false;
        if (out->type == Obj::Keyword) return false;
        if (out->type != Obj::Dictionary) return true;
        lex.skipSpace();
        const char* p = lex.here();
        const char* const end = base + size;
        if (end - p < 6 || std::memcmp(p, "stream", 6) != 0) return true;
        p += 6;
        // One end of line: CR LF or LF (a lone CR is met too).
        if (p < end && *p == '\r') ++p;
        if (p < end && *p == '\n') ++p;
        out->type = Obj::Stream;
        out->start = p - base;
        // Its length: as it says, if "endstream" follows; else found.
        qsizetype length = -1;
        const Obj len = resolve(get(*out->dict, "Length"));
        if (len.isNumber() && len.number >= 0 && len.number < double(size)) {
            const qsizetype n = qsizetype(len.number);
            if (n <= end - p) {
                Lexer after(p + n, end);
                after.skipSpace();
                if (end - after.here() >= 9 && std::memcmp(after.here(), "endstream", 9) == 0) {
                    length = n;
                    lex.seek(after.here() + 9);
                }
            }
        }
        if (length < 0) {
            const qsizetype at = QByteArrayView(p, end - p).indexOf("endstream");
            if (at >= 0) {
                length = at;
                lex.seek(p + at + 9);
                // (Without the end of line before it.)
                if (length > 0 && p[length - 1] == '\n') --length;
                if (length > 0 && p[length - 1] == '\r') --length;
            } else {
                length = end - p;
                lex.seek(end);
            }
        }
        out->length = length;
        return true;
    }

    Obj object(int number)
    {
        if (const auto c = objects.constFind(number); c != objects.cend()) return *c;
        const auto e = xref.constFind(number);
        if (e == xref.cend() || e->offset == -2) return {};
        if (reading.contains(number)) return {};
        reading.insert(number);
        Obj out;
        bool fine = false;
        if (e->offset >= 0) {
            if (e->offset < size) {
                Lexer lex(base + e->offset, base + size);
                Obj n, g, word;
                if (lex.next(&n, false) && lex.next(&g, false) && lex.next(&word, false) && n.isNumber() && int(n.number) == number &&
                    word.type == Obj::Keyword && word.text == "obj")
                    fine = parseBody(lex, &out);
            }
        } else {
            const auto held = streamObjects(e->stream);
            const auto o = held.constFind(number);
            if (o != held.cend()) {
                out = *o;
                fine = true;
            }
        }
        reading.remove(number);
        if (!fine && !rebuilt) {
            // The table does not hold: the file is searched instead, once.
            const QSet<int> keep = reading;
            reading.clear();
            rebuild();
            reading = keep;
            return object(number);
        }
        if (!fine) out = Obj{};
        objects.insert(number, out);
        return out;
    }

    Obj resolve(const Obj& o)
    {
        Obj v = o;
        for (int k = 0; k < 32 && v.type == Obj::Ref; ++k) v = object(int(v.number));
        return v.type == Obj::Ref ? Obj{} : v;
    }

    QList<int> streamObjectNumbers(const Obj& stream)
    {
        QList<int> out;
        const QByteArray data = decoded(stream);
        const int n = int(resolve(get(*stream.dict, "N")).number);
        if (n <= 0 || n > 10000000) return out;
        Lexer lex(data.constData(), data.constData() + data.size());
        for (int i = 0; i < n; ++i) {
            Obj a, b;
            if (!lex.next(&a, false) || !lex.next(&b, false) || !a.isNumber() || !b.isNumber()) break;
            out << int(a.number);
        }
        return out;
    }

    const QHash<int, Obj>& streamObjects(int streamNumber)
    {
        static const QHash<int, Obj> none;
        const auto have = compressed.constFind(streamNumber);
        if (have != compressed.cend()) return *have;
        // (Entered once: a stream that says it is held in itself gets nothing.)
        compressed[streamNumber];
        QHash<int, Obj> filled;
        const Obj stream = object(streamNumber);
        if (stream.type != Obj::Stream) return none;
        const QByteArray data = decoded(stream);
        const int n = int(resolve(get(*stream.dict, "N")).number);
        const qint64 first = qint64(resolve(get(*stream.dict, "First")).number);
        if (n <= 0 || n > 10000000 || first < 0 || first > data.size()) return none;
        Lexer head(data.constData(), data.constData() + first);
        const char* const end = data.constData() + data.size();
        for (int i = 0; i < n; ++i) {
            Obj a, b;
            if (!head.next(&a, false) || !head.next(&b, false) || !a.isNumber() || !b.isNumber()) break;
            const qint64 at = first + qint64(b.number);
            if (at < first || at >= data.size()) continue;
            Lexer lex(data.constData() + at, end);
            Obj value;
            if (lex.next(&value, true) && value.type != Obj::Keyword) filled.insert(int(a.number), value);
        }
        compressed[streamNumber] = filled;
        return compressed[streamNumber];
    }

    QByteArray rawOf(const Obj& stream) const
    {
        if (stream.type != Obj::Stream || stream.start < 0 || stream.length <= 0 || stream.start + stream.length > size) return {};
        return QByteArray::fromRawData(base + stream.start, stream.length);
    }

    QByteArray decoded(const Obj& stream, bool* image = nullptr)
    {
        QByteArray data = rawOf(stream);
        if (data.isEmpty() || !stream.dict) return {};
        Obj filter = resolve(get(*stream.dict, "Filter"));
        if (filter.type == Obj::Null) filter = resolve(get(*stream.dict, "F"));
        Obj parms = resolve(get(*stream.dict, "DecodeParms"));
        if (parms.type == Obj::Null) parms = resolve(get(*stream.dict, "DP"));
        QList<Obj> filters;
        QList<Obj> params;
        if (filter.type == Obj::Name) {
            filters << filter;
            params << parms;
        } else if (filter.type == Obj::Array) {
            filters = *filter.array;
            for (int i = 0; i < filters.size(); ++i)
                params << (parms.type == Obj::Array && i < parms.array->size() ? resolve(parms.array->at(i)) : Obj{});
        }
        for (int i = 0; i < filters.size() && i < 8; ++i) {
            const Obj f = resolve(filters.at(i));
            const Obj p = i < params.size() ? resolve(params.at(i)) : Obj{};
            if (f.isName("FlateDecode") || f.isName("Fl")) {
                if (data.size() < 2) return {};
                // (The two bytes of zlib's header, then DEFLATE; its checksum
                // is not looked at.)
                bool ok = false;
                data = zip::inflate(data.mid(2), &ok, MostDecoded);
                if (!ok) {
                    // Some writers leave the header out.
                    return {};
                }
                if (!applyPredictor(&data, p)) return {};
            } else if (f.isName("LZWDecode") || f.isName("LZW")) {
                bool ok = false;
                int early = 1;
                if (p.isDict()) {
                    const Obj ec = resolve(get(*p.dict, "EarlyChange"));
                    if (ec.isNumber()) early = int(ec.number);
                }
                data = lzwDecode(data, early, &ok);
                if (!ok) return {};
                if (!applyPredictor(&data, p)) return {};
            } else if (f.isName("ASCII85Decode") || f.isName("A85")) {
                data = ascii85Decode(data);
            } else if (f.isName("ASCIIHexDecode") || f.isName("AHx")) {
                data = hexDecode(data);
            } else if (f.isName("RunLengthDecode") || f.isName("RL")) {
                data = runLengthDecode(data);
            } else if (f.isName("Crypt")) {
                // (The identity one: it would be refused with the file.)
            } else {
                // An image's (DCT, JPX, CCITT, JBIG2): not ours to read.
                if (image != nullptr) *image = true;
                return {};
            }
            if (data.size() > MostDecoded) return {};
        }
        // (fromRawData's bytes are the file's, which stay.)
        return data;
    }

    bool applyPredictor(QByteArray* data, const Obj& parms)
    {
        if (!parms.isDict()) return true;
        const auto num = [&](const char* key, int otherwise) {
            const Obj v = resolve(get(*parms.dict, key));
            return v.isNumber() ? int(v.number) : otherwise;
        };
        return unpredict(data, num("Predictor", 1), num("Colors", 1), num("BitsPerComponent", 8), num("Columns", 1));
    }

    // --- Pages -----------------------------------------------------------

    bool loadPages()
    {
        pageList.clear();
        pageInherited.clear();
        const Obj root = resolve(get(trailer, "Root"));
        if (!root.isDict()) return false;
        const Obj pagesRef = get(*root.dict, "Pages");
        QSet<int> visited;
        walkPages(pagesRef, Dict{}, &visited, 0);
        return !pageList.isEmpty();
    }

    void walkPages(const Obj& nodeRef, Dict inherited, QSet<int>* visited, int depth)
    {
        if (depth > 256 || pageList.size() >= MostPagesWalked) return;
        if (nodeRef.type == Obj::Ref) {
            const int n = int(nodeRef.number);
            if (visited->contains(n)) return;
            visited->insert(n);
        }
        const Obj node = resolve(nodeRef);
        if (!node.isDict()) return;
        for (const char* key : {"Resources", "MediaBox", "CropBox", "Rotate"}) {
            const auto at = node.dict->constFind(key);
            if (at != node.dict->cend()) inherited.insert(key, *at);
        }
        const Obj kids = resolve(get(*node.dict, "Kids"));
        if (kids.type == Obj::Array && !get(*node.dict, "Type").isName("Page")) {
            for (const Obj& kid : *kids.array) walkPages(kid, inherited, visited, depth + 1);
            return;
        }
        if (nodeRef.type != Obj::Ref) return;
        pageList << int(nodeRef.number);
        pageInherited << inherited;
    }

    // --- A page's content ------------------------------------------------

    struct GState {
        QTransform ctm;
        QColor stroke = Qt::black, fill = Qt::black;
        double width = 1;
        QList<double> dash;
        QRectF clip;
        int strokeSpace = 0, fillSpace = 0;   // see colorOf
    };

    enum Space { Gray = 1, Rgb = 3, Cmyk = 4, Tint = 10, Lab = 11, Indexed = 12, Pattern = 13 };

    struct Run {
        Graphics* out = nullptr;
        QElapsedTimer clock;
        qint64 vertices = 0;
        qint64 operators = 0;
        bool stop = false;
        QSet<int> inForm;   // the forms being drawn, not to enter again
        int nextForm = 0;
        QHash<int, int> formIds;
    };

    static QColor gray(double g) { return QColor::fromRgbF(float(std::clamp(g, 0.0, 1.0)), float(std::clamp(g, 0.0, 1.0)), float(std::clamp(g, 0.0, 1.0))); }
    static QColor rgb(double r, double g, double b) { return QColor::fromRgbF(float(std::clamp(r, 0.0, 1.0)), float(std::clamp(g, 0.0, 1.0)), float(std::clamp(b, 0.0, 1.0))); }
    static QColor cmyk(double c, double m, double y, double k)
    {
        k = std::clamp(k, 0.0, 1.0);
        return rgb((1 - std::clamp(c, 0.0, 1.0)) * (1 - k), (1 - std::clamp(m, 0.0, 1.0)) * (1 - k), (1 - std::clamp(y, 0.0, 1.0)) * (1 - k));
    }

    int spaceOf(const Obj& name, const Obj& resources)
    {
        if (name.type != Obj::Name) return Gray;
        if (name.text == "DeviceGray" || name.text == "G" || name.text == "CalGray") return Gray;
        if (name.text == "DeviceRGB" || name.text == "RGB" || name.text == "CalRGB") return Rgb;
        if (name.text == "DeviceCMYK" || name.text == "CMYK") return Cmyk;
        if (name.text == "Pattern") return Pattern;
        if (!resources.isDict()) return Gray;
        const Obj spaces = resolve(get(*resources.dict, "ColorSpace"));
        if (!spaces.isDict()) return Gray;
        const Obj cs = resolve(get(*spaces.dict, name.text.constData()));
        if (cs.type == Obj::Name) return spaceOf(cs, Obj{});
        if (cs.type != Obj::Array || cs.array->isEmpty()) return Gray;
        const Obj kind = resolve(cs.array->at(0));
        if (kind.isName("ICCBased") && cs.array->size() > 1) {
            const Obj icc = resolve(cs.array->at(1));
            if (icc.isDict()) {
                const int n = int(resolve(get(*icc.dict, "N")).number);
                return n == 1 ? Gray : (n == 4 ? Cmyk : Rgb);
            }
        }
        if (kind.isName("CalGray")) return Gray;
        if (kind.isName("CalRGB")) return Rgb;
        if (kind.isName("Lab")) return Lab;
        if (kind.isName("Indexed") || kind.isName("I")) return Indexed;
        if (kind.isName("Separation") || kind.isName("DeviceN")) return Tint;
        if (kind.isName("Pattern")) return Pattern;
        return Gray;
    }

    static QColor colorOf(int space, const QList<Obj>& operands, const QColor& otherwise)
    {
        QList<double> v;
        for (const Obj& o : operands)
            if (o.isNumber()) v << o.number;
        if (v.isEmpty()) return otherwise;   // (a pattern's name)
        switch (space) {
        case Gray: return gray(v.at(0));
        case Rgb: return v.size() >= 3 ? rgb(v.at(0), v.at(1), v.at(2)) : gray(v.at(0));
        case Cmyk: return v.size() >= 4 ? cmyk(v.at(0), v.at(1), v.at(2), v.at(3)) : gray(v.at(0));
        case Tint: return gray(1 - std::clamp(v.at(0), 0.0, 1.0));
        case Lab: return gray(std::clamp(v.at(0) / 100.0, 0.0, 1.0));
        case Indexed: {
            // (Its table is not read: one colour for an index, to tell them apart.)
            return QColor::fromHsv((int(std::abs(v.at(0))) * 47) % 360, 200, 180);
        }
        default: return otherwise;
        }
    }

    void content(const QByteArray& data, const Obj& resources, GState gs, Run& run, int depth, int form, QPointF formOrigin)
    {
        if (depth > MostDepth || run.stop || !run.clock.isValid()) return;
        Lexer lex(data.constData(), data.constData() + data.size());
        QList<Obj> ops;
        QList<GState> stack;
        QList<Part> path;
        Part current;
        QPointF last;      // in the stream's own coordinates
        QPointF first;
        bool hasCurrent = false;
        int pendingClip = 0;   // W

        const auto finishPart = [&](bool closed) {
            if (hasCurrent && !current.points.isEmpty()) {
                current.closed = current.closed || closed;
                path.append(std::move(current));
            }
            current = Part{};
            hasCurrent = false;
        };
        const auto addPoint = [&](QPointF p) {
            current.points.append(gs.ctm.map(p));
            ++run.vertices;
        };
        const auto paint = [&](bool stroke, bool fill, bool close) {
            if (close && hasCurrent) {
                current.closed = true;
            }
            finishPart(false);
            if (pendingClip != 0 && !path.isEmpty()) {
                QRectF box;
                for (const Part& part : std::as_const(path)) box = box.united(part.points.boundingRect());
                gs.clip = gs.clip.isNull() ? box : gs.clip.intersected(box);
                if (gs.clip.isNull()) gs.clip = QRectF(box.topLeft(), QSizeF(0, 0));
            }
            pendingClip = 0;
            if ((stroke || fill) && !path.isEmpty()) {
                if (run.out->paths.size() < MostPaths) {
                    Path p;
                    p.parts = path;
                    p.stroked = stroke;
                    p.filled = fill;
                    p.stroke = gs.stroke;
                    p.fill = gs.fill;
                    p.width = gs.width * std::sqrt(std::abs(gs.ctm.determinant()));
                    if (!gs.dash.isEmpty()) {
                        const double k = std::sqrt(std::abs(gs.ctm.determinant()));
                        for (const double d : std::as_const(gs.dash)) p.dash << d * k;
                    }
                    p.clip = gs.clip;
                    p.form = form;
                    p.origin = formOrigin;
                    run.out->paths.append(std::move(p));
                } else {
                    run.out->truncated = true;
                    run.stop = true;
                }
            }
            path.clear();
        };

        Obj word;
        while (!run.stop && lex.next(&word, false)) {
            if (word.type != Obj::Keyword) {
                if (ops.size() < MostOperands) ops.append(std::move(word));
                word = Obj{};
                continue;
            }
            const QByteArray& op = word.text;
            ++run.operators;
            if ((run.operators & 0xFFFF) == 0 && run.clock.elapsed() > MostMilliseconds) {
                run.out->truncated = true;
                run.stop = true;
                break;
            }
            if (run.vertices > MostVertices) {
                run.out->truncated = true;
                run.stop = true;
                break;
            }
            const auto num = [&](int i) { return i >= 0 && i < ops.size() && ops.at(i).isNumber() ? ops.at(i).number : 0.0; };
            const int n = int(ops.size());
            const char* const name = op.constData();
            const int size = int(op.size());
            if (size == 1) {
                switch (name[0]) {
                case 'q':
                    if (stack.size() < 512) stack.append(gs);
                    break;
                case 'Q':
                    if (!stack.isEmpty()) gs = stack.takeLast();
                    break;
                case 'm':
                    if (n >= 2) {
                        finishPart(false);
                        last = first = QPointF(num(n - 2), num(n - 1));
                        hasCurrent = true;
                        addPoint(last);
                    }
                    break;
                case 'l':
                    if (n >= 2) {
                        if (!hasCurrent) {
                            hasCurrent = true;
                            first = QPointF(num(n - 2), num(n - 1));
                        }
                        last = QPointF(num(n - 2), num(n - 1));
                        addPoint(last);
                    }
                    break;
                case 'c':
                case 'v':
                case 'y': {
                    QPointF c1, c2, e;
                    if (name[0] == 'c' && n >= 6) {
                        c1 = QPointF(num(n - 6), num(n - 5));
                        c2 = QPointF(num(n - 4), num(n - 3));
                        e = QPointF(num(n - 2), num(n - 1));
                    } else if (name[0] == 'v' && n >= 4) {
                        c1 = last;
                        c2 = QPointF(num(n - 4), num(n - 3));
                        e = QPointF(num(n - 2), num(n - 1));
                    } else if (name[0] == 'y' && n >= 4) {
                        c1 = QPointF(num(n - 4), num(n - 3));
                        e = c2 = QPointF(num(n - 2), num(n - 1));
                    } else break;
                    if (!hasCurrent) {
                        hasCurrent = true;
                        first = last = c1;
                        addPoint(last);
                    }
                    // Flattened finely enough that no chord leaves the line
                    // by a tenth of a point (the page's own scale).
                    const QPointF a = gs.ctm.map(last), b = gs.ctm.map(c1), c = gs.ctm.map(c2), d = gs.ctm.map(e);
                    const double reach = QLineF(a, b).length() + QLineF(b, c).length() + QLineF(c, d).length();
                    const int steps = std::clamp(int(reach / 1.5) + 2, 2, 48);
                    for (int k = 1; k <= steps; ++k) {
                        const double t = double(k) / steps, u = 1 - t;
                        current.points.append(a * (u * u * u) + b * (3 * u * u * t) + c * (3 * u * t * t) + d * (t * t * t));
                    }
                    run.vertices += steps;
                    current.curved = true;
                    last = e;
                    break;
                }
                case 'h':
                    if (hasCurrent) {
                        current.closed = true;
                        last = first;
                        const QPointF keep = first;
                        finishPart(true);
                        // (What follows starts at the same point.)
                        first = last = keep;
                    }
                    break;
                case 'S': paint(true, false, false); break;
                case 's': paint(true, false, true); break;
                case 'f':
                case 'F': paint(false, true, true); break;
                case 'B': paint(true, true, false); break;
                case 'b': paint(true, true, true); break;
                case 'n': paint(false, false, false); break;
                case 'W': pendingClip = 1; break;
                case 'w':
                    if (n >= 1) gs.width = std::abs(num(n - 1));
                    break;
                case 'd':
                    if (n >= 2 && ops.at(n - 2).type == Obj::Array) {
                        gs.dash.clear();
                        double total = 0;
                        for (const Obj& v : *ops.at(n - 2).array)
                            if (v.isNumber() && gs.dash.size() < 16) {
                                gs.dash << std::abs(v.number);
                                total += std::abs(v.number);
                            }
                        if (total <= 0) gs.dash.clear();
                    }
                    break;
                case 'g':
                    if (n >= 1) gs.fill = gray(num(n - 1));
                    gs.fillSpace = Gray;
                    break;
                case 'G':
                    if (n >= 1) gs.stroke = gray(num(n - 1));
                    gs.strokeSpace = Gray;
                    break;
                case 'k':
                    if (n >= 4) gs.fill = cmyk(num(n - 4), num(n - 3), num(n - 2), num(n - 1));
                    gs.fillSpace = Cmyk;
                    break;
                case 'K':
                    if (n >= 4) gs.stroke = cmyk(num(n - 4), num(n - 3), num(n - 2), num(n - 1));
                    gs.strokeSpace = Cmyk;
                    break;
                default: break;
                }
            } else if (size == 2) {
                const char a = name[0], b = name[1];
                if (a == 'c' && b == 'm') {
                    if (n >= 6) gs.ctm = QTransform(num(n - 6), num(n - 5), num(n - 4), num(n - 3), num(n - 2), num(n - 1)) * gs.ctm;
                } else if (a == 'r' && b == 'e') {
                    if (n >= 4) {
                        finishPart(false);
                        const double x = num(n - 4), y = num(n - 3), w = num(n - 2), h = num(n - 1);
                        hasCurrent = true;
                        addPoint(QPointF(x, y));
                        addPoint(QPointF(x + w, y));
                        addPoint(QPointF(x + w, y + h));
                        addPoint(QPointF(x, y + h));
                        current.rectangle = true;
                        current.closed = true;
                        first = last = QPointF(x, y);
                        finishPart(true);
                    }
                } else if (a == 'r' && b == 'g') {
                    if (n >= 3) gs.fill = rgb(num(n - 3), num(n - 2), num(n - 1));
                    gs.fillSpace = Rgb;
                } else if (a == 'R' && b == 'G') {
                    if (n >= 3) gs.stroke = rgb(num(n - 3), num(n - 2), num(n - 1));
                    gs.strokeSpace = Rgb;
                } else if (a == 'c' && b == 's') {
                    if (n >= 1) {
                        gs.fillSpace = spaceOf(ops.at(n - 1), resources);
                        gs.fill = Qt::black;
                    }
                } else if (a == 'C' && b == 'S') {
                    if (n >= 1) {
                        gs.strokeSpace = spaceOf(ops.at(n - 1), resources);
                        gs.stroke = Qt::black;
                    }
                } else if (a == 's' && b == 'c') {
                    gs.fill = colorOf(gs.fillSpace == 0 ? int(n) : gs.fillSpace, ops, gs.fill);
                } else if (a == 'S' && b == 'C') {
                    gs.stroke = colorOf(gs.strokeSpace == 0 ? int(n) : gs.strokeSpace, ops, gs.stroke);
                } else if (a == 'f' && b == '*') {
                    paint(false, true, true);
                } else if (a == 'B' && b == '*') {
                    paint(true, true, false);
                } else if (a == 'b' && b == '*') {
                    paint(true, true, true);
                } else if (a == 'W' && b == '*') {
                    pendingClip = 1;
                } else if (a == 'g' && b == 's') {
                    if (n >= 1 && ops.at(n - 1).type == Obj::Name && resources.isDict()) {
                        const Obj states = resolve(get(*resources.dict, "ExtGState"));
                        if (states.isDict()) {
                            const Obj state = resolve(get(*states.dict, ops.at(n - 1).text.constData()));
                            if (state.isDict()) {
                                const Obj lw = resolve(get(*state.dict, "LW"));
                                if (lw.isNumber()) gs.width = std::abs(lw.number);
                                const Obj d = resolve(get(*state.dict, "D"));
                                if (d.type == Obj::Array && !d.array->isEmpty()) {
                                    const Obj pattern = resolve(d.array->at(0));
                                    if (pattern.type == Obj::Array) {
                                        gs.dash.clear();
                                        double total = 0;
                                        for (const Obj& v : *pattern.array) {
                                            const Obj rv = resolve(v);
                                            if (rv.isNumber() && gs.dash.size() < 16) {
                                                gs.dash << std::abs(rv.number);
                                                total += std::abs(rv.number);
                                            }
                                        }
                                        if (total <= 0) gs.dash.clear();
                                    }
                                }
                            }
                        }
                    }
                } else if (a == 'D' && b == 'o') {
                    if (n >= 1 && ops.at(n - 1).type == Obj::Name) drawXObject(ops.at(n - 1).text, resources, gs, run, depth);
                } else if (a == 'B' && b == 'I') {
                    // An inline image: passed over, to its "EI".
                    Obj skip;
                    for (;;) {
                        if (!lex.next(&skip, false)) break;
                        if (skip.type == Obj::Keyword && skip.text == "ID") break;
                    }
                    const char* p = lex.here();
                    const char* const end = data.constData() + data.size();
                    const char* endOfImage = end;
                    for (const char* q = p; q + 2 <= end; ++q) {
                        if (q[0] == 'E' && q[1] == 'I' && (q == p || isSpace(q[-1])) && (q + 2 == end || isSpace(q[2]))) {
                            endOfImage = q + 2;
                            break;
                        }
                    }
                    lex.seek(endOfImage);
                }
            } else if (size == 3) {
                if (std::memcmp(name, "scn", 3) == 0) {
                    gs.fill = colorOf(gs.fillSpace == 0 ? int(n) : gs.fillSpace, ops, gs.fill);
                } else if (std::memcmp(name, "SCN", 3) == 0) {
                    gs.stroke = colorOf(gs.strokeSpace == 0 ? int(n) : gs.strokeSpace, ops, gs.stroke);
                }
            }
            ops.clear();
        }
    }

    void drawXObject(const QByteArray& name, const Obj& resources, const GState& gs, Run& run, int depth)
    {
        if (!resources.isDict() || depth >= MostDepth) return;
        const Obj all = resolve(get(*resources.dict, "XObject"));
        if (!all.isDict()) return;
        const Obj ref = get(*all.dict, name.constData());
        const Obj x = resolve(ref);
        if (x.type != Obj::Stream) return;
        const Obj subtype = resolve(get(*x.dict, "Subtype"));
        if (subtype.isName("Image")) {
            Picture pic;
            pic.corners << gs.ctm.map(QPointF(0, 0)) << gs.ctm.map(QPointF(1, 0)) << gs.ctm.map(QPointF(1, 1)) << gs.ctm.map(QPointF(0, 1));
            const Obj w = resolve(get(*x.dict, "Width")), h = resolve(get(*x.dict, "Height"));
            pic.width = w.isNumber() ? int(std::clamp(w.number, 0.0, 1e9)) : 0;
            pic.height = h.isNumber() ? int(std::clamp(h.number, 0.0, 1e9)) : 0;
            if (run.out->pictures.size() < 10000) run.out->pictures.append(pic);
            return;
        }
        if (!subtype.isName("Form")) return;
        const int number = ref.type == Obj::Ref ? int(ref.number) : -1;
        if (number >= 0 && run.inForm.contains(number)) return;
        if (number >= 0) run.inForm.insert(number);
        const QByteArray data = decoded(x);
        GState inner = gs;
        const Obj matrix = resolve(get(*x.dict, "Matrix"));
        if (matrix.type == Obj::Array && matrix.array->size() >= 6) {
            double m[6] = {};
            bool fine = true;
            for (int i = 0; i < 6; ++i) {
                const Obj v = resolve(matrix.array->at(i));
                fine = fine && v.isNumber();
                m[i] = v.number;
            }
            if (fine) inner.ctm = QTransform(m[0], m[1], m[2], m[3], m[4], m[5]) * inner.ctm;
        }
        const Obj bbox = resolve(get(*x.dict, "BBox"));
        if (bbox.type == Obj::Array && bbox.array->size() >= 4) {
            double b[4] = {};
            for (int i = 0; i < 4; ++i) b[i] = resolve(bbox.array->at(i)).number;
            const QRectF box = inner.ctm.mapRect(QRectF(QPointF(b[0], b[1]), QPointF(b[2], b[3])).normalized());
            inner.clip = inner.clip.isNull() ? box : inner.clip.intersected(box);
            if (inner.clip.isNull()) inner.clip = QRectF(box.topLeft(), QSizeF(0, 0));
        }
        Obj own = resolve(get(*x.dict, "Resources"));
        if (!own.isDict()) own = resources;
        const int id = number >= 0 ? run.formIds.value(number, -1) : -1;
        int form = id;
        if (form < 0) {
            form = run.nextForm++;
            if (number >= 0) run.formIds.insert(number, form);
        }
        content(data, own, inner, run, depth + 1, form, inner.ctm.map(QPointF(0, 0)));
        if (number >= 0) run.inForm.remove(number);
    }

    bool page(int index, Graphics* out, QString* error)
    {
        if (index < 0 || index >= pageList.size()) {
            if (error != nullptr) *error = QCoreApplication::translate("PdfVector", "No page %1 in the file.").arg(index + 1);
            return false;
        }
        const Obj page = object(pageList.at(index));
        if (!page.isDict()) {
            if (error != nullptr) *error = QCoreApplication::translate("PdfVector", "Page %1 cannot be read.").arg(index + 1);
            return false;
        }
        const Dict& inherited = pageInherited.at(index);
        const auto boxOf = [&](const char* key) {
            const Obj b = resolve(get(inherited, key));
            if (b.type != Obj::Array || b.array->size() < 4) return QRectF();
            double v[4] = {};
            for (int i = 0; i < 4; ++i) {
                const Obj o = resolve(b.array->at(i));
                if (!o.isNumber()) return QRectF();
                v[i] = o.number;
            }
            return QRectF(QPointF(v[0], v[1]), QPointF(v[2], v[3])).normalized();
        };
        QRectF box = boxOf("MediaBox");
        if (box.isEmpty()) box = QRectF(0, 0, 612, 792);
        const QRectF crop = boxOf("CropBox");
        if (!crop.isEmpty()) {
            const QRectF both = crop.intersected(box);
            if (!both.isEmpty()) box = both;
        }
        int rotate = int(resolve(get(inherited, "Rotate")).number);
        rotate = ((rotate % 360) + 360) % 360;
        QTransform toPage;
        switch (rotate) {
        case 90: toPage = QTransform(0, 1, 1, 0, -box.top(), -box.left()); break;
        case 180: toPage = QTransform(-1, 0, 0, 1, box.right(), -box.top()); break;
        case 270: toPage = QTransform(0, -1, -1, 0, box.bottom(), box.right()); break;
        default: toPage = QTransform(1, 0, 0, -1, -box.left(), box.bottom()); break;
        }
        // (QRectF.bottom() is the box's top in PDF's upward y.)
        out->size = (rotate == 90 || rotate == 270) ? QSizeF(box.height(), box.width()) : box.size();

        // Its content: one stream, or several read as one.
        QByteArray data;
        const Obj contents = resolve(get(*page.dict, "Contents"));
        if (contents.type == Obj::Stream) data = decoded(contents);
        else if (contents.type == Obj::Array)
            for (const Obj& item : *contents.array) {
                const Obj s = resolve(item);
                if (s.type == Obj::Stream) {
                    data += decoded(s);
                    data += '\n';
                    if (data.size() > MostDecoded) break;
                }
            }
        const Obj resources = resolve(get(inherited, "Resources"));
        Run run;
        run.out = out;
        run.clock.start();
        GState gs;
        gs.ctm = toPage;
        content(data, resources, gs, run, 0, -1, QPointF());
        return true;
    }
};

Reader::Reader() : d(std::make_unique<Impl>()) {}
Reader::~Reader() = default;

std::shared_ptr<Reader> Reader::open(const QByteArray& bytes, QString* error, bool* encrypted)
{
    std::shared_ptr<Reader> r(new Reader());
    r->d->bytes = bytes;
    if (encrypted != nullptr) *encrypted = false;
    if (bytes.size() < 16) {
        if (error != nullptr) *error = QCoreApplication::translate("PdfVector", "It is too short to be a PDF.");
        return nullptr;
    }
    if (!r->d->load()) {
        if (error != nullptr) *error = QCoreApplication::translate("PdfVector", "Its pages could not be found.");
        return nullptr;
    }
    // Its strings and streams are ciphered: nothing here can read them.
    if (r->d->trailer.contains("Encrypt")) {
        if (encrypted != nullptr) *encrypted = true;
        if (error != nullptr) *error = QCoreApplication::translate("PdfVector", "It is encrypted: its paths are not read, the picture of its pages is.");
        return nullptr;
    }
    return r;
}

int Reader::pageCount() const { return int(d->pageList.size()); }

bool Reader::page(int page, Graphics* out, QString* error)
{
    *out = Graphics{};
    return d->page(page, out, error);
}

} // namespace qucs_s::pdf::vec
