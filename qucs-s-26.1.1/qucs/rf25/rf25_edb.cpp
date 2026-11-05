/*
 * rf25_edb.cpp - an HFSS™ 3D Layout project read without any Ansys® software:
 *                its .aedb's edb.def, and the .aedt beside it
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "rf25_edb.h"

#include "multiphysics/fem_expr.h"
#include "rf25_layout.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <memory>

namespace qucs_s::rf25 {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::rf25", text);
}

// -------------------------------------------------------------- The records

/// A value of the file: one of the types of its table.
struct Value {
    int type = -1;                  ///< as the table says; -1: nothing (a record of class -1)
    int integer = 0;                ///< 0, 1; 5: its class
    double number = 0;              ///< 2, 3
    QString text;                   ///< 3: the expression; 4
    QList<Value> items;             ///< 5: its fields; 6, 7: its items

    /// A record's field \a k, through what it derives from where that is its
    /// first; nothing where there is none.
    const Value& at(int k) const
    {
        static const Value none;
        return k >= 0 && k < items.size() ? items.at(k) : none;
    }
};

struct Stopped {
    QString why;
};

/// Reads records by the file's own table of classes. Strict: see rf25_edb.h.
class Reader
{
public:
    explicit Reader(const QByteArray& bytes) : a_data(bytes.constData()), a_size(bytes.size()) {}

    qsizetype at = 0;
    QHash<int, QList<int>> classes;

    qint32 integer()
    {
        need(4);
        qint32 v = 0;
        std::memcpy(&v, a_data + at, 4);
        at += 4;
        return v;
    }
    quint8 byte()
    {
        need(1);
        return quint8(a_data[at++]);
    }
    double real()
    {
        need(8);
        double v = 0;
        std::memcpy(&v, a_data + at, 8);
        at += 8;
        return v;
    }
    QString string()
    {
        const qint32 n = integer();
        if (n < 0 || n > a_size - at) throw Stopped{tr("a string of %1 bytes at %2").arg(n).arg(at - 4)};
        const QString s = QString::fromLatin1(a_data + at, n);
        at += n;
        return s;
    }
    Value field(int type, int depth = 0)
    {
        if (depth > 64) throw Stopped{tr("records nested too deep at %1").arg(at)};
        Value v;
        v.type = type;
        switch (type) {
        case 0: v.integer = byte(); break;
        case 1: v.integer = integer(); break;
        case 2: v.number = real(); break;
        case 3:
            v.number = real();
            v.text = string();
            break;
        case 4: v.text = string(); break;
        case 5: return record(depth);
        case 6: {
            const qint32 size = integer();
            const qint32 of = integer();
            if (size < 0 || size > a_size - at) throw Stopped{tr("a list of %1 at %2").arg(size).arg(at - 8)};
            v.items.reserve(std::min(size, kMostReserved));
            for (qint32 k = 0; k < size; ++k) v.items << field(of, depth + 1);
            break;
        }
        case 7: {
            const qint32 size = integer();
            if (size < 0 || size > a_size - at) throw Stopped{tr("an array of %1 at %2").arg(size).arg(at - 4)};
            v.items.reserve(std::min(size, kMostReserved));
            for (qint32 k = 0; k < size; ++k) v.items << field(integer(), depth + 1);
            break;
        }
        default: throw Stopped{tr("a value of type %1 at %2").arg(type).arg(at)};
        }
        return v;
    }
    Value record(int depth = 0)
    {
        Value v;
        v.type = 5;
        const qint32 c = integer();
        v.integer = c;
        if (c == -1) return v;
        const auto found = classes.constFind(c);
        if (found == classes.constEnd()) throw Stopped{tr("a record of class %1 at %2, which the file's own table has not").arg(c).arg(at - 4)};
        v.items.reserve(found->size());
        for (const int t : *found) v.items << field(t, depth + 1);
        return v;
    }
    qsizetype size() const { return a_size; }

private:
    /// (A size is judged by the bytes left, and a damaged one can be most of a
    /// large file: not all of that at sixty-odd bytes an item before it is read.)
    static constexpr qint32 kMostReserved = 1024;

    void need(qsizetype n) const
    {
        if (at < 0 || n > a_size - at) throw Stopped{tr("the end of the file at %1").arg(at)};
    }

    const char* a_data;
    qsizetype a_size;
};

/// Every string under \a v, in order.
void stringsUnder(const Value& v, QStringList* out)
{
    if (v.type == 4) out->append(v.text);
    for (const Value& item : v.items) stringsUnder(item, out);
}

// --------------------------------------------------------------------- Text

/// A value out of "key='value'" among a block's lines.
QString quoted(const QString& text, const QString& key)
{
    const QRegularExpression re(QStringLiteral("(?:^|[\\s,(])%1='((?:[^'\\\\]|\\\\.)*)'").arg(QRegularExpression::escape(key)));
    return re.match(text).captured(1);
}

double numberIn(const QString& text, double otherwise = 0)
{
    bool ok = false;
    const double v = text.trimmed().toDouble(&ok);
    return ok ? v : otherwise;
}

/// A frequency ("1.5GHz", "2e9", "10 MHz") in Hz; nought where it is none.
double frequencyOf(const QString& text)
{
    static const QRegularExpression re(QStringLiteral("^\\s*([-+0-9.eE]+)\\s*([a-zA-Z]*)\\s*$"));
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch()) return 0;
    bool ok = false;
    const double v = m.captured(1).toDouble(&ok);
    if (!ok) return 0;
    const QString unit = m.captured(2).toLower();
    if (unit.isEmpty() || unit == QLatin1String("hz")) return v;
    if (unit == QLatin1String("khz")) return v * 1e3;
    if (unit == QLatin1String("mhz")) return v * 1e6;
    if (unit == QLatin1String("ghz")) return v * 1e9;
    if (unit == QLatin1String("thz")) return v * 1e12;
    return 0;
}

/// An impedance ("50ohm", "50ohm + 0i ohm", "75"): its real part, Ohm.
double ohmsOf(const QString& text, double otherwise)
{
    static const QRegularExpression re(QStringLiteral("^\\s*([-+0-9.eE]+)\\s*(k|m)?(ohm)?"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch()) return otherwise;
    bool ok = false;
    double v = m.captured(1).toDouble(&ok);
    if (!ok) return otherwise;
    const QString scale = m.captured(2).toLower();
    if (scale == QLatin1String("k")) v *= 1e3;
    if (scale == QLatin1String("m")) v *= 1e-3;
    return v;
}

/// The blocks named \a name in \a text, each with what is between its begin
/// and its end.
QStringList blocksOf(const QString& text, const QString& name)
{
    QStringList out;
    const QString open = QStringLiteral("$begin '%1'").arg(name), close = QStringLiteral("$end '%1'").arg(name);
    qsizetype from = 0;
    while (true) {
        const qsizetype a = text.indexOf(open, from);
        if (a < 0) break;
        const qsizetype b = text.indexOf(close, a);
        if (b < 0) break;
        out << text.mid(a + open.size(), b - a - open.size());
        from = b + close.size();
    }
    return out;
}

/// The materials of a "Materials" block.
void materialsIn(const QString& text, QHash<QString, EdbMaterial>* out)
{
    for (const QString& block : blocksOf(text, QStringLiteral("Materials"))) {
        static const QRegularExpression open(QStringLiteral("\\$begin '([^']+)'"));
        qsizetype from = 0;
        while (true) {
            const QRegularExpressionMatch m = open.match(block, from);
            if (!m.hasMatch()) break;
            const QString name = m.captured(1);
            const QString close = QStringLiteral("$end '%1'").arg(name);
            const qsizetype end = block.indexOf(close, m.capturedEnd());
            if (end < 0) break;
            const QString body = block.mid(m.capturedEnd(), end - m.capturedEnd());
            EdbMaterial material;
            material.epsr = numberIn(quoted(body, QStringLiteral("permittivity")), 1);
            material.tand = numberIn(quoted(body, QStringLiteral("dielectric_loss_tangent")), 0);
            material.sigma = numberIn(quoted(body, QStringLiteral("conductivity")), 0);
            out->insert(name.toLower(), material);
            from = end + close.size();
        }
    }
}

/// The sweeps of a setup's text. A setup held inside edb.def has it with its
/// quotes escaped; \a text has them taken off.
void sweepsIn(const QString& setup, const QString& text, QList<EdbSweep>* out)
{
    double adaptive = 0;
    for (const QString& key : {QStringLiteral("AdaptiveFrequency"), QStringLiteral("Frequency")}) {
        adaptive = frequencyOf(quoted(text, key));
        if (adaptive > 0) break;
    }
    static const QRegularExpression stringRe(QStringLiteral("(?:FrequencyString|Data)='([^']*)'"));
    static const QRegularExpression nameRe(QStringLiteral("SweepName='([^']*)'"));
    static const QRegularExpression listRe(QStringLiteral("Frequencies\\(([^)]*)\\)"));
    static const QRegularExpression oneRe(QStringLiteral("'([^']*)'"));
    QStringList parts = text.split(QStringLiteral("$begin 'SweepData'"));
    if (parts.size() < 2) parts = text.split(QStringLiteral("$begin 'Sweeps'"));
    for (int k = 1; k < parts.size(); ++k) {
        EdbSweep s;
        s.setup = setup;
        s.adaptive = adaptive;
        s.name = nameRe.match(parts.at(k)).captured(1);
        s.data = stringRe.match(parts.at(k)).captured(1);
        const QRegularExpressionMatch list = listRe.match(parts.at(k));
        if (list.hasMatch()) {
            auto each = oneRe.globalMatch(list.captured(1));
            while (each.hasNext()) {
                const double f = frequencyOf(each.next().captured(1));
                if (f > 0) s.frequencies << f;
            }
        }
        if (!s.data.isEmpty() || !s.frequencies.isEmpty()) out->append(s);
    }
    if (out->isEmpty() && adaptive > 0) {
        EdbSweep s;
        s.setup = setup;
        s.adaptive = adaptive;
        out->append(s);
    }
}

// ------------------------------------------------------------ The shapes

/// The chord points of an arc from \a p to \a q, \a height its sagitta: bowed
/// to the right of the way from p to q for a height above nought, to the left
/// below it. (As a bend's two sides have it: going counter-clockwise round a
/// strip, its outer arc's height is above nought and its inner arc's below.)
void arcPoints(const QPointF& p, const QPointF& q, double height, QPolygonF* out)
{
    constexpr int kChords = 16;
    const QPointF chord = q - p;
    const double length = std::hypot(chord.x(), chord.y());
    const double h = std::abs(height);
    if (!(length > 0) || !(h > 0)) return;
    const double a = length / 2;
    const double r = (a * a + h * h) / (2 * h);
    const QPointF right(chord.y() / length, -chord.x() / length);
    const double side = height > 0 ? 1 : -1;
    // The centre: from the top of the arc, back along the normal by the radius.
    const QPointF centre = (p + q) / 2 + right * (side * (h - r));
    const double start = std::atan2(p.y() - centre.y(), p.x() - centre.x());
    // The centre on its left, it turns counter-clockwise; and the other way.
    const double sweep = 4 * std::atan2(h, a);
    for (int k = 1; k < kChords; ++k) {
        const double angle = start + side * sweep * k / kChords;
        *out << QPointF(centre.x() + r * std::cos(angle), centre.y() + r * std::sin(angle));
    }
}

/// A record of class 36 - its doubles, or its values each with an expression -
/// as points in µm. An arc is a pair whose y is the largest a double holds,
/// its x the arc's height.
QPolygonF pointsOf(const Value& data)
{
    QPolygonF out;
    if (data.integer != 36) return out;
    const Value& list = data.at(2);
    const int n = int(list.items.size());
    for (int k = 0; k + 1 < n; k += 2) {
        const double x = list.items.at(k).number, y = list.items.at(k + 1).number;
        if (std::abs(y) > 1e100) {
            if (!out.isEmpty() && k + 3 < n) {
                const QPointF next(list.items.at(k + 2).number * 1e6, list.items.at(k + 3).number * 1e6);
                arcPoints(out.last(), next, x * 1e6, &out);
            }
            continue;
        }
        out << QPointF(x * 1e6, y * 1e6);
    }
    return out;
}

/// A path as the polygon of its width: each stretch a rectangle, each bend
/// mitred or rounded, each end flat, extended or rounded (0 round, 1 flat, 2
/// extended, as the Scripting Guide says of a line's caps).
QPolygonF pathOutline(const QPolygonF& line, double width, int startCap, int endCap, int corner)
{
    QPolygonF spine;
    for (const QPointF& p : line)
        if (spine.isEmpty() || p != spine.last()) spine << p;
    if (spine.size() < 2 || !(width > 0)) return {};
    const double half = width / 2;
    QList<QPolygonF> pieces;
    const auto disc = [half](const QPointF& c) {
        QPolygonF d;
        constexpr int kSides = 24;
        for (int k = 0; k < kSides; ++k) {
            const double a = 2 * M_PI * k / kSides;
            d << QPointF(c.x() + half * std::cos(a), c.y() + half * std::sin(a));
        }
        return d;
    };
    const auto unit = [](const QPointF& d) {
        const double len = std::hypot(d.x(), d.y());
        return len > 0 ? d / len : QPointF();
    };
    const int n = int(spine.size());
    for (int k = 0; k + 1 < n; ++k) {
        QPointF a = spine.at(k), b = spine.at(k + 1);
        const QPointF along = unit(b - a), across(-along.y() * half, along.x() * half);
        if (k == 0 && startCap == 2) a -= along * half;
        if (k + 2 == n && endCap == 2) b += along * half;
        pieces << QPolygonF(QList<QPointF>{a + across, a - across, b - across, b + across});
    }
    if (startCap == 0) pieces << disc(spine.first());
    if (endCap == 0) pieces << disc(spine.last());
    for (int k = 1; k + 1 < n; ++k) {
        if (corner == 0) {
            pieces << disc(spine.at(k));
            continue;
        }
        // The mitre on both sides: the inner one lies inside the two stretches.
        const QPointF u = unit(spine.at(k) - spine.at(k - 1)), v = unit(spine.at(k + 1) - spine.at(k));
        const QPointF nu(-u.y(), u.x()), nv(-v.y(), v.x());
        const double denominator = 1 + nu.x() * nv.x() + nu.y() * nv.y();
        if (!(denominator > 1e-3)) continue;
        const QPointF mitre = (nu + nv) * (half / denominator);
        const QPointF c = spine.at(k);
        pieces << QPolygonF(QList<QPointF>{c, c + nu * half, c + mitre, c + nv * half});
        pieces << QPolygonF(QList<QPointF>{c, c - nv * half, c - mitre, c - nu * half});
    }
    const QList<QPolygonF> joined = unionOf(pieces);
    return joined.isEmpty() ? QPolygonF() : joined.first();
}

/// Reads the stack lines of a cell's text.
void layersIn(const QString& text, const QHash<QString, double>& variables, EdbDesign* out)
{
    static const QRegularExpression line(QStringLiteral("SLayer\\(Layer\\(([^\\n]*)"));
    auto each = line.globalMatch(text);
    while (each.hasNext()) {
        const QString body = each.next().captured(1);
        EdbLayer l;
        l.name = quoted(body, QStringLiteral("N"));
        static const QRegularExpression idRe(QStringLiteral("(?:^|[\\s,(])ID=(-?\\d+)"));
        l.id = idRe.match(body).captured(1).toInt();
        l.signal = quoted(body, QStringLiteral("T")) != QLatin1String("dielectric");
        l.material = quoted(body, QStringLiteral("Mat"));
        l.fill = quoted(body, QStringLiteral("FilMat"));
        const QString th = quoted(body, QStringLiteral("Th")), el = quoted(body, QStringLiteral("LElev"));
        if (!lengthOf(th, variables, &l.thickness))
            out->notes << tr("Layer %1: its thickness %2 does not read as a length: taken as nought.").arg(l.name, th);
        if (!lengthOf(el, variables, &l.elevation))
            out->notes << tr("Layer %1: its elevation %2 does not read as a length: taken as nought.").arg(l.name, el);
        out->layers << l;
    }
}

/// The padstacks of the project's text.
void padstacksIn(const QString& text, const QHash<QString, double>& variables, EdbDesign* out)
{
    for (const QString& pd : blocksOf(text, QStringLiteral("pd"))) {
        EdbPadstack p;
        p.name = quoted(pd, QStringLiteral("nam"));
        p.material = quoted(pd, QStringLiteral("mat"));
        p.plating = numberIn(quoted(pd, QStringLiteral("plt")), 100);
        static const QRegularExpression hole(QStringLiteral("hle\\(shp='([^']*)', Szs\\(([^)]*)\\)"));
        const QRegularExpressionMatch h = hole.match(pd);
        p.hole = h.captured(1);
        static const QRegularExpression size(QStringLiteral("'([^']*)'"));
        auto sizes = size.globalMatch(h.captured(2));
        while (sizes.hasNext()) {
            double v = 0;
            if (lengthOf(sizes.next().captured(1), variables, &v)) p.sizes << v;
        }
        static const QRegularExpression lay(QStringLiteral("lay='([^']*)'"));
        auto layers = lay.globalMatch(pd);
        while (layers.hasNext()) p.layers << layers.next().captured(1);
        // What it has on each.
        const auto shapeOf = [&variables](const QString& lgm, const char* key, QString* shape, QList<double>* into) {
            const QRegularExpression re(QStringLiteral("(?:^|\\s)%1\\(shp='([^']*)', Szs\\(((?:'[^']*'(?:,\\s*)?)*)\\)").arg(QLatin1String(key)));
            const QRegularExpressionMatch m = re.match(lgm);
            *shape = m.hasMatch() ? m.captured(1) : QStringLiteral("No");
            auto each = size.globalMatch(m.captured(2));
            while (each.hasNext()) {
                double v = 0;
                if (lengthOf(each.next().captured(1), variables, &v)) into->append(v);
            }
        };
        for (const QString& lgm : blocksOf(pd, QStringLiteral("lgm"))) {
            EdbPadLayer one;
            one.layer = quoted(lgm, QStringLiteral("lay"));
            shapeOf(lgm, "pad", &one.padShape, &one.padSizes);
            shapeOf(lgm, "ant", &one.antiShape, &one.antiSizes);
            p.onLayers << one;
        }
        if (!p.name.isEmpty()) out->padstacks.insert(p.name, p);
    }
}

/// A text block's length before it: the file writes each so.
struct Block {
    qsizetype begin = 0, end = 0;   ///< of its text
    QString text;
};

QList<Block> textBlocksOf(const QByteArray& bytes)
{
    QList<Block> out;
    qsizetype from = 0;
    while (true) {
        const qsizetype k = bytes.indexOf("$begin '", from);
        if (k < 0) break;
        from = k + 8;
        if (k < 4) continue;
        qint32 n = 0;
        std::memcpy(&n, bytes.constData() + k - 4, 4);
        if (n <= 0 || n > bytes.size() - k) continue;
        // A whole block: it ends in a $end of the same name, with a newline.
        const qsizetype nameEnd = bytes.indexOf('\'', k + 8);
        if (nameEnd < 0 || nameEnd > k + n) continue;
        const QByteArray close = "$end '" + bytes.mid(k + 8, nameEnd - k - 8) + "'\n";
        if (!bytes.mid(k, n).endsWith(close)) continue;
        Block b;
        b.begin = k;
        b.end = k + n;
        b.text = QString::fromLatin1(bytes.constData() + k, n);
        out << b;
        from = k + n;
    }
    return out;
}

/// A place on a layout, in µm: no shape of a file that is whole is further off
/// than a kilometre, or no number.
bool sensible(const QPointF& q)
{
    constexpr double kFar = 1e9;
    return std::isfinite(q.x()) && std::isfinite(q.y()) && std::abs(q.x()) < kFar && std::abs(q.y()) < kFar;
}

/// Each terminal, shape, via and net of a cell's records, into \a out.
void takeRecords(const QList<Value>& records, const QHash<int, Value>& edges, EdbDesign* out)
{
    // A record's chain: what it derives from is its first field.
    const auto idOf = [](const Value& r) {
        // (…, 10, 5: class 5's first field.)
        const Value* v = &r;
        while (v->integer != 5 && v->type == 5 && !v->items.isEmpty()) v = &v->items.first();
        return v->type == 5 && v->integer == 5 ? v->at(0).integer : -1;
    };
    const auto nameFrom = [](const Value& r) {
        // The name kept among its properties: a pair of 1 and the name.
        const Value* v = &r;
        while (v->integer != 5 && v->type == 5 && !v->items.isEmpty()) v = &v->items.first();
        if (v->integer != 5) return QString();
        for (const Value& list : v->at(1).items)
            for (const Value& array : list.items)
                for (int k = 0; k + 1 < array.items.size(); k += 2)
                    if (array.items.at(k).type == 1 && array.items.at(k).integer == 1 && array.items.at(k + 1).type == 4)
                        return array.items.at(k + 1).text;
        return QString();
    };
    const auto chain = [](const Value& r, int c) -> const Value* {
        const Value* v = &r;
        while (v->type == 5) {
            if (v->integer == c) return v;
            if (v->items.isEmpty()) return nullptr;
            v = &v->items.first();
        }
        return nullptr;
    };
    for (const Value& r : records) {
        const int c = r.integer;
        const Value* c10 = chain(r, 10);
        if (c == 7) {
            out->nets.insert(idOf(r), r.at(1).text);
            continue;
        }
        const Value* c11 = chain(r, 11);
        if (c11 != nullptr && c10 != nullptr && (c == 12 || c == 13 || c == 14 || c == 15)) {
            EdbShape s;
            s.id = idOf(r);
            s.net = c10->at(1).integer;
            s.layer = c11->at(1).integer;
            s.owner = c11->at(4).integer;
            s.name = nameFrom(r);
            if (c == 12) {
                s.kind = QStringLiteral("rectangle");
                const int rep = r.at(1).integer;
                const double a = r.at(2).number * 1e6, b = r.at(3).number * 1e6, cc = r.at(4).number * 1e6, d = r.at(5).number * 1e6;
                QRectF box = rep == 1 ? QRectF(a - cc / 2, b - d / 2, cc, d) : QRectF(QPointF(a, b), QPointF(cc, d)).normalized();
                if (const QString note = tr("A rectangle's rounded corners are not kept: square here."); r.at(6).number != 0 && !out->notes.contains(note)) out->notes << note;
                QPolygonF p(box);
                while (p.size() > 1 && p.first() == p.last()) p.removeLast();
                const double angle = r.at(7).number;
                if (angle != 0) {
                    const QPointF centre = box.center();
                    for (QPointF& q : p) {
                        const QPointF o = q - centre;
                        q = centre + QPointF(o.x() * std::cos(angle) - o.y() * std::sin(angle), o.x() * std::sin(angle) + o.y() * std::cos(angle));
                    }
                }
                s.outline = p;
            } else if (c == 13) {
                s.kind = QStringLiteral("circle");
                const QPointF centre(r.at(1).number * 1e6, r.at(2).number * 1e6);
                const double radius = r.at(3).number * 1e6;
                constexpr int kSides = 64;
                for (int k = 0; k < kSides; ++k) {
                    const double a = 2 * M_PI * k / kSides;
                    s.outline << QPointF(centre.x() + radius * std::cos(a), centre.y() + radius * std::sin(a));
                }
            } else if (c == 15) {
                s.kind = QStringLiteral("polygon");
                s.outline = pointsOf(r.at(1));
            } else {
                s.kind = QStringLiteral("path");
                s.outline = pathOutline(pointsOf(r.at(6)), r.at(4).number * 1e6, r.at(1).integer, r.at(2).integer, r.at(3).integer);
            }
            for (const QPointF& q : std::as_const(s.outline))
                if (!sensible(q)) throw Stopped{tr("a shape at (%1, %2) µm").arg(q.x()).arg(q.y())};
            if (s.outline.size() >= 3) out->shapes << s;
            continue;
        }
        if (c == 19 && c10 != nullptr) {
            EdbVia v;
            v.id = idOf(r);
            v.net = c10->at(1).integer;
            v.at = QPointF(r.at(2).number * 1e6, r.at(3).number * 1e6);
            if (!sensible(v.at)) throw Stopped{tr("a via at (%1, %2) µm").arg(v.at.x()).arg(v.at.y())};
            v.rotation = r.at(4).number;
            v.hole = r.at(5).number;
            v.name = nameFrom(r);
            // How it uses its definition: a block of its own, by this number.
            v.usage = r.at(1).integer;
            out->vias << v;
            continue;
        }
        const Value* c29 = chain(r, 29);
        if (c29 != nullptr && c >= 30 && c <= 35) {
            EdbPort p;
            p.id = idOf(r);
            p.name = c29->at(1).text;
            p.layer = c29->at(2).integer;
            p.reference = c29->at(10).integer;
            p.impedanceText = c29->at(11).text;
            p.impedance = ohmsOf(p.impedanceText, 50);
            p.isPort = !p.impedanceText.isEmpty();
            // Its settings, as the properties it keeps by the solver's name.
            QStringList kept;
            stringsUnder(c29->at(9), &kept);
            for (const QString& one : std::as_const(kept)) {
                if (!one.startsWith(QLatin1String("HFSS("))) continue;
                static const QRegularExpression typeRe(QStringLiteral("'HFSS Type'='([^']*)'"));
                p.hfssType = typeRe.match(one).captured(1);
                p.referenceName = quoted(one, QStringLiteral("ReferenceName"));
            }
            for (const QString& one : std::as_const(kept))
                if (one.startsWith(QLatin1String("PlanarEM("))) {
                    static const QRegularExpression solverRe(QStringLiteral("PortSolver=(true|false)"));
                    p.portSolver = solverRe.match(one).captured(1) != QLatin1String("false");
                }
            const QString excitation = c29->at(17).text;
            if (excitation.startsWith(QLatin1String("PortExcitationProps("))) {
                static const QRegularExpression deembedRe(QStringLiteral("DoDeembed=(true|false)"));
                if (deembedRe.match(excitation).captured(1) == QLatin1String("true"))
                    lengthOf(quoted(excitation, QStringLiteral("DoDeembedLength")), out->variables, &p.deembed);
                static const QRegularExpression renormRe(QStringLiteral("DoRenormalize=(true|false)"));
                if (renormRe.match(excitation).captured(1) == QLatin1String("true"))
                    p.impedance = ohmsOf(quoted(excitation, QStringLiteral("RenormalizionZ0")), p.impedance);
            }
            if (c == 30) {
                p.kind = QStringLiteral("edge");
                const Value& list = r.at(1);
                if (!list.items.isEmpty()) {
                    const Value edge = edges.value(list.items.first().integer);
                    if (edge.integer == 39) {
                        p.at = QPointF(edge.at(1).number * 1e6, edge.at(2).number * 1e6);
                        // (Its shape's id last: diff_via has 3 before them all,
                        // and 2, 3, 1, 0 the lines its four ports are on.)
                        p.primitive = edge.at(4).integer;
                    }
                }
            } else if (c == 31) {
                p.kind = QStringLiteral("via");
                p.primitive = r.at(2).integer;
            } else if (c == 32) {
                p.kind = QStringLiteral("point");
                p.layer = r.at(1).integer;
                const QString where = r.at(2).text;
                double x = 0, y = 0;
                lengthOf(quoted(where, QStringLiteral("x")), out->variables, &x);
                lengthOf(quoted(where, QStringLiteral("y")), out->variables, &y);
                p.at = QPointF(x * 1e6, y * 1e6);
            } else {
                p.kind = QStringLiteral("other");
            }
            out->ports << p;
            continue;
        }
        if (c != 8 && c != 39) ++out->unread;
    }
}

} // namespace

bool lengthOf(const QString& text, const QHash<QString, double>& variables, double* metres)
{
    const QString t = text.trimmed();
    if (t.isEmpty()) {
        *metres = 0;
        return true;
    }
    static const QRegularExpression plain(QStringLiteral("^([-+]?(?:\\d+\\.?\\d*|\\.\\d+)(?:[eE][-+]?\\d+)?)\\s*([a-zA-Z]*)$"));
    const auto scaleOf = [](const QString& unit, double* scale) {
        const QString u = unit.toLower();
        if (u.isEmpty() || u == QLatin1String("m") || u == QLatin1String("meter") || u == QLatin1String("meters")) *scale = 1;
        else if (u == QLatin1String("mm")) *scale = 1e-3;
        else if (u == QLatin1String("um")) *scale = 1e-6;
        else if (u == QLatin1String("nm")) *scale = 1e-9;
        else if (u == QLatin1String("cm")) *scale = 1e-2;
        else if (u == QLatin1String("mil")) *scale = 25.4e-6;
        else if (u == QLatin1String("in")) *scale = 25.4e-3;
        else return false;
        return true;
    };
    if (const QRegularExpressionMatch m = plain.match(t); m.hasMatch()) {
        double scale = 1;
        if (!scaleOf(m.captured(2), &scale)) return false;
        *metres = m.captured(1).toDouble() * scale;
        return true;
    }
    // An expression: its numbers with units as metres, its variables as they
    // are kept, for the engine's own expressions to read.
    fem::Scope scope(&fem::Scope::builtins());
    for (auto it = variables.constBegin(); it != variables.constEnd(); ++it) {
        QString name = it.key();
        name.replace(QLatin1Char('$'), QStringLiteral("v_"));
        scope.setConstant(name, it.value());
    }
    QString e = t;
    e.replace(QLatin1Char('$'), QStringLiteral("v_"));
    static const QRegularExpression withUnit(QStringLiteral("((?:\\d+\\.?\\d*|\\.\\d+)(?:[eE][-+]?\\d+)?)\\s*(meter|mm|um|nm|cm|mil|in|m)\\b"));
    QString rewritten;
    qsizetype from = 0;
    auto each = withUnit.globalMatch(e);
    while (each.hasNext()) {
        const QRegularExpressionMatch m = each.next();
        double scale = 1;
        scaleOf(m.captured(2), &scale);
        rewritten += e.mid(from, m.capturedStart() - from);
        rewritten += QStringLiteral("(%1)").arg(m.captured(1).toDouble() * scale, 0, 'g', 17);
        from = m.capturedEnd();
    }
    rewritten += e.mid(from);
    const fem::Expression compiled = fem::Expression::compile(rewritten, scope);
    if (!compiled.isValid() || !compiled.isConstant()) return false;
    *metres = compiled.constant();
    return true;
}

bool readEdb(const QByteArray& bytes, const QString& wanted, EdbDesign* out, QString* error)
{
    const auto fail = [error](const QString& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    static const QByteArray hdrEnd = "$end 'Hdr'\n";
    const qsizetype hdr = bytes.indexOf(hdrEnd);
    if (hdr < 0 || hdr > 4096 || bytes.indexOf("$begin 'Hdr'") < 0) return fail(tr("It is no edb.def: it has not the header one begins with."));
    const QString header = QString::fromLatin1(bytes.left(hdr));
    out->version = quoted(header, QStringLiteral("Version"));
    static const QRegularExpression encrypted(QStringLiteral("Encrypted=(true|false)"));
    if (encrypted.match(header).captured(1) == QLatin1String("true"))
        return fail(tr("It is encrypted, as a protected component is: Electronics Desktop alone can read it."));

    const QList<Block> blocks = textBlocksOf(bytes);
    QString project;
    for (const Block& b : blocks)
        if (b.text.startsWith(QLatin1String("$begin 'EDB'"))) {
            project = b.text;
            break;
        }
    // The project's variables, its materials and padstacks.
    static const QRegularExpression variable(QStringLiteral("VariableProp\\(n='([^']*)', v='([^']*)'"));
    const auto takeVariables = [out](const QString& text) {
        // (Each may name those before it: as many passes as it takes.)
        QList<QPair<QString, QString>> left;
        auto each = variable.globalMatch(text);
        while (each.hasNext()) {
            const QRegularExpressionMatch m = each.next();
            left << qMakePair(m.captured(1), m.captured(2));
        }
        for (int pass = 0; pass < 8 && !left.isEmpty(); ++pass) {
            QList<QPair<QString, QString>> still;
            for (const auto& [name, value] : left) {
                double v = 0;
                if (lengthOf(value, out->variables, &v)) out->variables.insert(name, v);
                else still << qMakePair(name, value);
            }
            if (still.size() == left.size()) break;
            left = still;
        }
    };
    takeVariables(project);
    materialsIn(project, &out->materials);
    padstacksIn(project, out->variables, out);

    // The table of classes.
    Reader reader(bytes);
    reader.at = hdr + hdrEnd.size();
    try {
        if (reader.integer() != -1) return fail(tr("Its table of records does not begin as it should: the format may be newer than this reader."));
        const qint32 count = reader.integer();
        if (count <= 0 || count > 4096) return fail(tr("Its table of records says %1 classes.").arg(count));
        for (qint32 k = 0; k < count; ++k) {
            const qint32 id = reader.integer();
            const qint32 fields = reader.integer();
            if (fields < 0 || fields > 4096) return fail(tr("Its table of records says %1 fields for class %2.").arg(fields).arg(id));
            QList<int> types;
            types.reserve(fields);
            for (qint32 f = 0; f < fields; ++f) {
                reader.integer();
                types << reader.integer();
            }
            reader.classes.insert(id, types);
        }
    } catch (const Stopped& s) {
        return fail(tr("Its table of records stops at %1.").arg(s.why));
    }

    // How each via uses its definition: "def=", its layers from and to.
    struct Usage {
        int definition = -1, from = -1, to = -1;
    };
    QHash<int, Usage> usages;
    for (const Block& b : blocks) {
        static const QRegularExpression defRe(QStringLiteral("(?:^|\\n)\\s*def=(\\d+)\\n"));
        const QRegularExpressionMatch m = defRe.match(b.text);
        if (!m.hasMatch() || !b.text.startsWith(QLatin1String("$begin ''")) || b.begin < 8) continue;
        static const QRegularExpression flRe(QStringLiteral("(?:^|\\n)\\s*fl=(-?\\d+)\\n")), tlRe(QStringLiteral("(?:^|\\n)\\s*tl=(-?\\d+)\\n"));
        qint32 id = 0;
        std::memcpy(&id, bytes.constData() + b.begin - 8, 4);
        usages.insert(id, Usage{m.captured(1).toInt(), flRe.match(b.text).captured(1).toInt(), tlRe.match(b.text).captured(1).toInt()});
    }

    // The cells: a name, a stack, and after a third block their records.
    struct Cell {
        QString name, text, stack;
        qsizetype records = -1;
        int shapes = 0;
    };
    QList<Cell> cells;
    for (int k = 0; k < blocks.size(); ++k) {
        const Block& b = blocks.at(k);
        if (!b.text.startsWith(QLatin1String("$begin ''"))) continue;
        const QString name = quoted(b.text, QStringLiteral("dn"));
        if (!name.isEmpty()) {
            Cell c;
            c.name = name;
            c.text = b.text;
            cells << c;
            continue;
        }
        if (cells.isEmpty()) continue;
        if (b.text.contains(QLatin1String("Mode='")) && cells.last().stack.isEmpty()) {
            cells.last().stack = b.text;
            cells.last().records = b.end;
            continue;
        }
        // A via's use of its definition: the last before the records is theirs
        // (a file of an old version has none, and they follow the stack).
        static const QRegularExpression defRe(QStringLiteral("(?:^|\\n)\\s*def=\\d+"));
        if (defRe.match(b.text).hasMatch()) cells.last().records = b.end;
    }
    if (cells.isEmpty()) return fail(tr("It holds no cell of a layout."));

    // Which cell: as asked, else the one with the most shapes, a symbol's
    // footprint (a project keeps it as a cell too) only where there is no other.
    const auto readCell = [&reader, &bytes](const Cell& cell, QList<Value>* records, QHash<int, Value>* edges, QString* why) {
        if (cell.records < 0) return true;
        reader.at = cell.records;
        try {
            const auto peek = [&reader, &bytes](qint32* a, qint32* b) {
                if (reader.at + 8 > bytes.size()) return false;
                std::memcpy(a, bytes.constData() + reader.at, 4);
                std::memcpy(b, bytes.constData() + reader.at + 4, 4);
                return true;
            };
            qint32 a = 0, b = 0;
            // ((0, 5): the list before; then two numbers of the cell's own.)
            if (peek(&a, &b) && a == 0 && b == 5) reader.at += 8;
            reader.integer();
            reader.integer();
            while (peek(&a, &b) && b == 5 && a >= 0 && a <= bytes.size() - reader.at) {
                reader.at += 8;
                for (qint32 k = 0; k < a; ++k) {
                    const Value v = reader.record();
                    if (v.integer == 39) edges->insert(v.at(0).integer, v);
                    else records->append(v);
                }
            }
        } catch (const Stopped& s) {
            *why = s.why;
            return false;
        }
        return true;
    };
    int chosen = -1, most = -1;
    QHash<int, QString> stops;
    for (int k = 0; k < cells.size(); ++k) {
        QList<Value> records;
        QHash<int, Value> edges;
        QString why;
        const bool whole = readCell(cells.at(k), &records, &edges, &why);
        if (!whole) stops.insert(k, why);
        int shapes = 0;
        for (const Value& v : std::as_const(records)) shapes += v.integer == 12 || v.integer == 13 || v.integer == 14 || v.integer == 15;
        cells[k].shapes = shapes;
        const bool footprint = cells.at(k).name.startsWith(QLatin1String("_symbolFootprint_"));
        const int worth = footprint ? -1 : shapes;
        if (!wanted.isEmpty() && cells.at(k).name == wanted) {
            chosen = k;
            most = INT_MAX;
        } else if (worth > most || chosen < 0) {
            chosen = k;
            most = worth;
        }
    }
    const Cell& cell = cells.at(chosen);
    out->name = cell.name;
    static const QRegularExpression modeRe(QStringLiteral("Mode='([^']*)'"));
    out->mode = modeRe.match(cell.stack).captured(1);
    takeVariables(cell.text);
    layersIn(cell.stack, out->variables, out);
    materialsIn(cell.stack, &out->materials);
    // A setup kept with the cell, its quotes escaped as text within text.
    {
        static const QRegularExpression setupRe(QStringLiteral("SimSetup\\(id=\\d+, n='([^']*)', t=\\d+, v='(.*?)\\$end \\\\'SimSetupInfo\\\\'"),
                                                QRegularExpression::DotMatchesEverythingOption);
        auto each = setupRe.globalMatch(cell.text);
        while (each.hasNext()) {
            const QRegularExpressionMatch m = each.next();
            QString body = m.captured(2);
            body.replace(QStringLiteral("\\'"), QStringLiteral("'"));
            sweepsIn(m.captured(1), body, &out->sweeps);
        }
    }
    if (stops.contains(chosen)) {
        out->notes << tr("Its records stop at %1: the shapes of %2 are not read. It may be a version of the format this reader has not seen (it is %3).")
                          .arg(stops.value(chosen), cell.name, out->version);
        return fail(out->notes.last());
    }
    QList<Value> records;
    QHash<int, Value> edges;
    QString why;
    readCell(cell, &records, &edges, &why);
    try {
        takeRecords(records, edges, out);
    } catch (const Stopped& s) {
        out->shapes.clear();
        out->vias.clear();
        out->ports.clear();
        out->notes << tr("Its records stop at %1: the shapes of %2 are not read. It may be a version of the format this reader has not seen (it is %3).").arg(s.why, cell.name, out->version);
        return fail(out->notes.last());
    }
    // A via's definition and its layers, by the use it names.
    QHash<int, QString> padstackById;
    for (const QString& pd : blocksOf(project, QStringLiteral("pd"))) {
        static const QRegularExpression idRe(QStringLiteral("^\\s*id=(\\d+)\\n"));
        const QRegularExpressionMatch m = idRe.match(pd);
        if (m.hasMatch()) padstackById.insert(m.captured(1).toInt(), quoted(pd, QStringLiteral("nam")));
    }
    int unnamed = 0;
    for (EdbVia& v : out->vias) {
        const auto usage = usages.constFind(v.usage);
        if (usage == usages.constEnd() || !padstackById.contains(usage->definition)) {
            ++unnamed;
            continue;
        }
        v.padstack = padstackById.value(usage->definition);
        v.fromLayer = usage->from;
        v.toLayer = usage->to;
    }
    if (unnamed > 0)
        out->notes << tr("%1 via(s) name a use of a padstack that is not there: their holes are their own, and they span the whole stack.").arg(unnamed);
    return true;
}

AedtProject readAedt(const QString& text)
{
    AedtProject out;
    materialsIn(text, &out.materials);
    const qsizetype design = text.indexOf(QLatin1String("$begin 'PlanarEMCircuit'"));
    if (design < 0) return out;
    const qsizetype designEnd = text.indexOf(QLatin1String("$end 'PlanarEMCircuit'"), design);
    const QString body = text.mid(design, designEnd < 0 ? -1 : designEnd - design);
    out.found = true;
    out.design = quoted(body, QStringLiteral("Name"));
    // Its ports: Port > Data > each.
    for (const QString& port : blocksOf(body, QStringLiteral("Port"))) {
        const QStringList data = blocksOf(port, QStringLiteral("Data"));
        if (data.isEmpty()) continue;
        static const QRegularExpression open(QStringLiteral("\\$begin '([^']+)'\\n\\s*DataId='"));
        qsizetype from = 0;
        while (true) {
            const QRegularExpressionMatch m = open.match(data.first(), from);
            if (!m.hasMatch()) break;
            const QString name = m.captured(1);
            const QString close = QStringLiteral("$end '%1'").arg(name);
            const qsizetype end = data.first().indexOf(close, m.capturedEnd());
            if (end < 0) break;
            const QString one = data.first().mid(m.capturedEnd(), end - m.capturedEnd());
            QHash<QString, QString> properties;
            static const QRegularExpression line(QStringLiteral("(?:^|\\n)\\s*'?([^'=\\n]+)'?='([^'\\n]*)'"));
            auto each = line.globalMatch(one);
            while (each.hasNext()) {
                const QRegularExpressionMatch p = each.next();
                if (!properties.contains(p.captured(1))) properties.insert(p.captured(1), p.captured(2));
            }
            out.ports.insert(name, properties);
            out.portOrder << name;
            from = end + close.size();
        }
        break;
    }
    // Its setups, each with its sweeps: Setup > Data > each.
    for (const QString& setup : blocksOf(body, QStringLiteral("Setup"))) {
        const qsizetype data = setup.indexOf(QLatin1String("$begin 'Data'"));
        if (data < 0) continue;
        static const QRegularExpression open(QStringLiteral("\\$begin '([^']+)'\\n\\s*\\$begin 'Properties'"));
        const QRegularExpressionMatch m = open.match(setup, data);
        if (m.hasMatch()) sweepsIn(m.captured(1), setup.mid(m.capturedEnd()), &out.sweeps);
        break;
    }
    return out;
}

} // namespace qucs_s::rf25
