/*
 * ltspiceimport.cpp - an LTspice schematic (.asc) as a SPICE netlist (see
 * ltspiceimport.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "ltspiceimport.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QRegularExpression>
#include <QStringDecoder>

#include <algorithm>
#include <cmath>
#include <limits>
#include <functional>
#include <map>
#include <optional>

namespace qucs_s::ltspice {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("LTspiceImport", text);
}

struct Symbol {
    QChar prefix;              ///< the SPICE letter its instances take
    QList<QPoint> pins;        ///< in SPICE order, unturned
    bool bulkIsSource = false; ///< a three-pin MOSFET: its bulk on its source
    QString model;             ///< a subcircuit's (Prefix X) SpiceModel
    QString value;             ///< its .asy's Value, when the instance gives none
};

// LTspice's standard symbols, their pins as its lib/sym has them.
const QHash<QString, Symbol>& standard()
{
    static const QHash<QString, Symbol> table = [] {
        QHash<QString, Symbol> t;
        const Symbol twoTerminalLong{QLatin1Char('R'), {QPoint(16, 16), QPoint(16, 96)}};
        t.insert(QStringLiteral("res"), twoTerminalLong);
        t.insert(QStringLiteral("res2"), twoTerminalLong);
        t.insert(QStringLiteral("cap"), Symbol{QLatin1Char('C'), {QPoint(16, 0), QPoint(16, 64)}});
        t.insert(QStringLiteral("polcap"), Symbol{QLatin1Char('C'), {QPoint(16, 0), QPoint(16, 64)}});
        t.insert(QStringLiteral("ind"), Symbol{QLatin1Char('L'), {QPoint(16, 16), QPoint(16, 96)}});
        t.insert(QStringLiteral("ind2"), Symbol{QLatin1Char('L'), {QPoint(16, 16), QPoint(16, 96)}});
        t.insert(QStringLiteral("voltage"), Symbol{QLatin1Char('V'), {QPoint(0, 16), QPoint(0, 96)}});
        t.insert(QStringLiteral("current"), Symbol{QLatin1Char('I'), {QPoint(0, 0), QPoint(0, 80)}});
        t.insert(QStringLiteral("bv"), Symbol{QLatin1Char('B'), {QPoint(0, 16), QPoint(0, 96)}});
        t.insert(QStringLiteral("bi"), Symbol{QLatin1Char('B'), {QPoint(0, 0), QPoint(0, 80)}});
        for (const char* d : {"diode", "zener", "schottky", "led", "varactor"})
            t.insert(QString::fromLatin1(d), Symbol{QLatin1Char('D'), {QPoint(16, 0), QPoint(16, 64)}});
        for (const char* n : {"npn", "npn2", "npn3"})
            t.insert(QString::fromLatin1(n), Symbol{QLatin1Char('Q'), {QPoint(64, 0), QPoint(0, 48), QPoint(64, 96)}});
        for (const char* p : {"pnp", "pnp2", "pnp3"})
            t.insert(QString::fromLatin1(p), Symbol{QLatin1Char('Q'), {QPoint(64, 96), QPoint(0, 48), QPoint(64, 0)}});
        t.insert(QStringLiteral("nmos"), Symbol{QLatin1Char('M'), {QPoint(48, 0), QPoint(0, 80), QPoint(48, 96)}, true});
        t.insert(QStringLiteral("pmos"), Symbol{QLatin1Char('M'), {QPoint(48, 96), QPoint(0, 16), QPoint(48, 0)}, true});
        t.insert(QStringLiteral("nmos4"), Symbol{QLatin1Char('M'), {QPoint(48, 0), QPoint(0, 80), QPoint(48, 96), QPoint(48, 48)}});
        t.insert(QStringLiteral("pmos4"), Symbol{QLatin1Char('M'), {QPoint(48, 96), QPoint(0, 16), QPoint(48, 0), QPoint(48, 48)}});
        return t;
    }();
    return table;
}

// How far a drawing's coordinates reach: as a schematic's model plane
// (Schematic::ModelLimit, 2^23). Beyond, sums of them overflowed an int
// (bug hunt of 2026-10-08, F3).
constexpr qlonglong kFarthest = 1 << 23;

// A coordinate: a whole number within kFarthest, else none.
std::optional<int> coordinate(const QString& text)
{
    bool ok = false;
    const qlonglong v = text.toLongLong(&ok);
    if (!ok || v < -kFarthest || v > kFarthest) return std::nullopt;
    return int(v);
}

std::optional<QPoint> pointAt(const QStringList& f, int i)
{
    const std::optional<int> x = coordinate(f.value(i)), y = coordinate(f.value(i + 1));
    if (!x || !y) return std::nullopt;
    return QPoint(*x, *y);
}

// An .asy's pins (in their SpiceOrder) and its Prefix, SpiceModel and Value;
// what is wrong in it said in \a notes.
std::optional<Symbol> readSymbol(const QString& file, QStringList* notes)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    const QStringList lines = textOf(f.readAll()).split(QLatin1Char('\n'));
    const QString name = QFileInfo(file).fileName();
    Symbol s;
    QList<std::pair<int, QPoint>> pins;
    std::optional<QPoint> pin;
    int unordered = 1000;
    QStringList badOrders;
    QSet<int> orders;
    bool far = false;
    for (QString line : lines) {
        line = line.trimmed();
        const QStringList f = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (f.isEmpty()) continue;
        if (f.first() == QLatin1String("PIN") && f.size() >= 3) {
            if (pin) pins << std::pair{unordered++, *pin};
            pin = pointAt(f, 1);
            if (!pin) far = true;
        } else if (f.first() == QLatin1String("PINATTR") && f.value(1) == QLatin1String("SpiceOrder") && pin) {
            // SPICE's order of the pins: 1, 2, 3 ... each once.
            bool ok = false;
            const int order = f.value(2).toInt(&ok);
            if (!ok || order < 1 || orders.contains(order)) {
                badOrders << QStringLiteral("%1 (the pin at %2, %3)").arg(f.value(2, QStringLiteral("none"))).arg(pin->x()).arg(pin->y());
                pins << std::pair{unordered++, *pin};
            } else {
                orders.insert(order);
                pins << std::pair{order, *pin};
            }
            pin.reset();
        } else if (f.first() == QLatin1String("SYMATTR") && f.size() >= 3) {
            const QString value = line.section(QLatin1Char(' '), 2);
            if (f.at(1) == QLatin1String("Prefix")) s.prefix = value.isEmpty() ? QChar() : value.at(0).toUpper();
            else if (f.at(1) == QLatin1String("SpiceModel")) s.model = value;
            else if (f.at(1) == QLatin1String("Value")) s.value = value;
        }
    }
    if (pin) pins << std::pair{unordered++, *pin};
    if (far) {
        notes->append(tr("%1: a pin beyond %2 of the origin - the symbol is not used").arg(name).arg(kFarthest));
        return std::nullopt;
    }
    if (!badOrders.isEmpty())
        notes->append(tr("%1: SpiceOrder %2 is not 1, 2, 3 ... each once - those pins put last, in the order drawn: check its "
                         "nodes in the netlist")
                          .arg(name, badOrders.join(QStringLiteral(", "))));
    std::stable_sort(pins.begin(), pins.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [order, at] : pins) s.pins << at;
    if (s.pins.isEmpty() || s.prefix.isNull()) return std::nullopt;
    return s;
}

// A value as ngspice reads it: LTspice's µ a u; SINE a SIN.
QString valueText(QString v)
{
    v.replace(QChar(0x00B5), QLatin1Char('u')).replace(QChar(0x03BC), QLatin1Char('u'));
    static const QRegularExpression sine(QStringLiteral("\\bSINE\\s*\\("), QRegularExpression::CaseInsensitiveOption);
    v.replace(sine, QStringLiteral("SIN("));
    return v;
}

// A SPICE number with its scale (5m, 1Meg, 2.2u): NaN when it is none.
double spiceNumber(const QString& text)
{
    static const QRegularExpression number(QStringLiteral("^([-+]?(?:\\d+\\.?\\d*|\\.\\d+)(?:[eE][-+]?\\d+)?)(meg|[fpnumkgt])?[a-z]*$"),
                                           QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = number.match(text.trimmed());
    if (!m.hasMatch()) return std::numeric_limits<double>::quiet_NaN();
    double v = m.captured(1).toDouble();
    const QString scale = m.captured(2).toLower();
    static const QHash<QString, double> scales{{QStringLiteral("f"), 1e-15}, {QStringLiteral("p"), 1e-12}, {QStringLiteral("n"), 1e-9},
                                               {QStringLiteral("u"), 1e-6},  {QStringLiteral("m"), 1e-3},  {QStringLiteral("k"), 1e3},
                                               {QStringLiteral("meg"), 1e6}, {QStringLiteral("g"), 1e9},   {QStringLiteral("t"), 1e12}};
    if (!scale.isEmpty()) v *= scales.value(scale, 1.0);
    return v;
}

// LTspice's .tran with its stop time alone: SPICE's has a step first.
QString directiveText(const QString& d, QStringList* notes)
{
    const QStringList f = d.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (f.size() >= 2 && f.first().compare(QLatin1String(".tran"), Qt::CaseInsensitive) == 0) {
        QStringList values;
        for (int i = 1; i < f.size() && std::isfinite(spiceNumber(f.at(i))); ++i) values << f.at(i);
        if (values.size() == 1) {
            const double stop = spiceNumber(values.first());
            const QString step = QString::number(stop / 1000, 'g', 6);
            notes->append(tr("%1: LTspice's stop time alone - written .tran %2 %3 (a step of a thousandth)").arg(d, step, values.first()));
            return QStringLiteral(".tran %1 %2").arg(step, f.mid(1).join(QLatin1Char(' ')));
        }
    }
    return d;
}

struct Union {
    QHash<QPoint, QPoint> up;
    QPoint find(QPoint p)
    {
        if (!up.contains(p)) up.insert(p, p);
        while (up.value(p) != p) {
            const QPoint next = up.value(p);
            up.insert(p, up.value(next));
            p = next;
        }
        return p;
    }
    void join(const QPoint& a, const QPoint& b) { up.insert(find(a), find(b)); }
};

// Whether \a p is on the segment from \a a to \a b (its ends too).
bool onSegment(const QPoint& p, const QPoint& a, const QPoint& b)
{
    const qint64 cross = (qint64(b.x()) - a.x()) * (qint64(p.y()) - a.y()) - (qint64(b.y()) - a.y()) * (qint64(p.x()) - a.x());
    if (cross != 0) return false;
    return p.x() >= std::min(a.x(), b.x()) && p.x() <= std::max(a.x(), b.x()) && p.y() >= std::min(a.y(), b.y())
           && p.y() <= std::max(a.y(), b.y());
}

} // namespace

QString textOf(const QByteArray& bytes)
{
    // Line ends as Windows (CR LF), Unix (LF) or the old Mac (CR alone)
    // wrote them: each one LF.
    const auto lines = [](QString text) {
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        return text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    };
    // UTF-16, little-endian as LTspice writes it: its mark, or a NUL every
    // other byte; big-endian by its mark (or the NULs first).
    const bool bomLE = bytes.startsWith("\xFF\xFE");
    const bool bomBE = bytes.startsWith("\xFE\xFF");
    const bool nullsLE = bytes.size() >= 4 && bytes.at(1) == '\0' && bytes.at(3) == '\0';
    const bool nullsBE = bytes.size() >= 4 && bytes.at(0) == '\0' && bytes.at(2) == '\0';
    if (bomLE || (nullsLE && !bomBE)) {
        QStringDecoder utf16(QStringDecoder::Utf16LE);
        return lines(utf16(bomLE ? bytes.mid(2) : bytes));
    }
    if (bomBE || nullsBE) {
        QStringDecoder utf16(QStringDecoder::Utf16BE);
        return lines(utf16(bomBE ? bytes.mid(2) : bytes));
    }
    QStringDecoder utf8(QStringDecoder::Utf8);
    QString text = utf8(bytes);
    if (utf8.hasError()) text = QString::fromLatin1(bytes);   // (Windows' 8 bits: µ is 0xB5)
    return lines(text);
}

bool looksLikeAsc(const QString& text)
{
    const QString start = text.left(200).trimmed();
    return start.startsWith(QLatin1String("Version 4")) || start.startsWith(QLatin1String("SHEET "))
           || start.startsWith(QLatin1String("Version 3"));
}

QPoint turned(const QPoint& p, const QString& rotation)
{
    QPoint q = p;
    QString r = rotation.trimmed().toUpper();
    if (r.startsWith(QLatin1Char('M'))) q.setX(-q.x());
    const int degrees = r.mid(1).toInt();
    switch (((degrees / 90) % 4 + 4) % 4) {
    case 1: return QPoint(-q.y(), q.x());
    case 2: return QPoint(-q.x(), -q.y());
    case 3: return QPoint(q.y(), -q.x());
    default: return q;
    }
}

namespace {

// A flag's name as a net label takes one - a letter first, then letters,
// digits and single _ - LTspice's rails as they are usually spelt so:
// +5V P5V, -12V N12V, V+ VP, 3V3 V3V3, a.b a_b. Empty for ground (0).
QString labelName(const QString& flag)
{
    if (flag == QLatin1String("0")) return QString();
    QString s = flag.trimmed();
    if (s.startsWith(QLatin1Char('+'))) s = QLatin1Char('P') + s.mid(1);
    else if (s.startsWith(QLatin1Char('-'))) s = QLatin1Char('N') + s.mid(1);
    if (s.endsWith(QLatin1Char('+'))) s = s.chopped(1) + QLatin1Char('P');
    else if (s.endsWith(QLatin1Char('-'))) s = s.chopped(1) + QLatin1Char('N');
    static const QRegularExpression unsafe(QStringLiteral("[^A-Za-z0-9_]+")), runs(QStringLiteral("_{2,}"));
    s.replace(unsafe, QStringLiteral("_")).replace(runs, QStringLiteral("_"));
    while (s.startsWith(QLatin1Char('_'))) s.remove(0, 1);
    while (s.endsWith(QLatin1Char('_'))) s.chop(1);
    if (s.isEmpty()) return QStringLiteral("N");
    if (s.at(0).isDigit()) s.prepend(s.contains(QLatin1Char('V'), Qt::CaseInsensitive) ? QLatin1Char('V') : QLatin1Char('N'));
    return s;
}

// Instance parameters ngspice takes, of the kinds whose SpiceLine LTspice's
// component database fills with its own (ratings, the maker, parasitics).
const QHash<QChar, QSet<QString>>& ngspiceParameters()
{
    static const QHash<QChar, QSet<QString>> table{
        {QLatin1Char('R'), {"r", "temp", "dtemp", "m", "ac", "scale", "noisy", "tc1", "tc2", "tce", "l", "w"}},
        {QLatin1Char('C'), {"c", "q", "m", "scale", "temp", "dtemp", "tc1", "tc2", "ic", "l", "w"}},
        {QLatin1Char('L'), {"l", "nt", "m", "scale", "temp", "dtemp", "tc1", "tc2", "ic"}},
    };
    return table;
}

// Its parasitics LTspice draws into a part: made parts of their own.
const QHash<QChar, QSet<QString>>& parasiticsOf()
{
    static const QHash<QChar, QSet<QString>> table{
        {QLatin1Char('C'), {"rser", "lser", "rpar", "cpar"}},
        {QLatin1Char('L'), {"rser", "rpar", "cpar"}},
        {QLatin1Char('V'), {"rser", "cpar"}},
        {QLatin1Char('I'), {"rpar", "cpar"}},
    };
    return table;
}

// Directives only LTspice knows: what each was, and what does it here.
QString ltspiceOnly(const QString& d)
{
    const QString word = d.section(QRegularExpression(QStringLiteral("\\s+")), 0, 0).toLower();
    if (word == QLatin1String(".meas") || word == QLatin1String(".measure"))
        return tr("%1: LTspice's measurement - not taken (get_dataset's measure, or a .control script's meas, gives it)");
    if (word == QLatin1String(".wave")) return tr("%1: LTspice's .wav file of a trace - not taken (export_data writes a trace)");
    return QString();
}

} // namespace

Conversion convert(const QString& asc, const QStringList& symbolFolders, const QString& name)
{
    Conversion c;
    if (!looksLikeAsc(asc)) {
        c.error = tr("this is no LTspice schematic (an .asc begins with \"Version 4\")");
        return c;
    }
    struct Part {
        QString symbol, rotation;
        QPoint at;
        QMap<QString, QString> attributes;
    };
    QList<std::pair<QPoint, QPoint>> wires;
    QList<std::pair<QPoint, QString>> flags;
    QList<Part> parts;
    QStringList directives;
    // The words of an .asc: those that draw (LINE, WINDOW, ...) read and
    // passed over; any other line said, not lost unremarked.
    static const QSet<QString> drawing{"Version", "SHEET", "WINDOW", "LINE", "RECTANGLE", "CIRCLE", "ARC", "DATAFLAG", "BUSTAP", "IOPIN"};
    QStringList unread;
    int number = 0;
    for (QString line : asc.split(QLatin1Char('\n'))) {
        ++number;
        line = line.trimmed();
        const QStringList f = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (f.isEmpty()) continue;
        const QString& word = f.first();
        const auto far = [&]() {
            c.error = tr("line %1 (%2) has a coordinate that is not a whole number within %3 of the origin").arg(number).arg(line.left(60)).arg(kFarthest);
        };
        if (word == QLatin1String("WIRE") && f.size() >= 5) {
            const std::optional<QPoint> a = pointAt(f, 1), b = pointAt(f, 3);
            if (!a || !b) return far(), c;
            wires << std::pair{*a, *b};
        } else if (word == QLatin1String("FLAG") && f.size() >= 4) {
            const std::optional<QPoint> at = pointAt(f, 1);
            if (!at) return far(), c;
            flags << std::pair{*at, f.at(3)};
        } else if (word == QLatin1String("SYMBOL") && f.size() >= 4) {
            const std::optional<QPoint> at = pointAt(f, 2);
            if (!at) return far(), c;
            Part p;
            p.symbol = f.at(1);
            p.at = *at;
            p.rotation = f.value(4, QStringLiteral("R0"));
            parts << p;
        } else if (word == QLatin1String("SYMATTR") && f.size() >= 2 && !parts.isEmpty()) {
            parts.last().attributes.insert(f.at(1), line.section(QLatin1Char(' '), 2));
        } else if (word == QLatin1String("TEXT") && f.size() >= 6) {
            // TEXT x y align size !directive (or ;comment); lines as "\n".
            const QString text = line.section(QLatin1Char(' '), 5);
            if (!text.startsWith(QLatin1Char('!'))) continue;
            for (const QString& d : text.mid(1).split(QStringLiteral("\\n"))) {
                const QString t = d.trimmed();
                if (t.isEmpty()) continue;
                if (t.startsWith(QLatin1String(".backanno"), Qt::CaseInsensitive)) continue;   // (LTspice's own bookkeeping)
                if (const QString why = ltspiceOnly(t); !why.isEmpty()) {
                    c.notes << why.arg(t);
                    continue;
                }
                directives << directiveText(valueText(t), &c.notes);
            }
        } else if (!drawing.contains(word) && word != QLatin1String("SYMATTR") && word != QLatin1String("TEXT")
                   && word != QLatin1String("WIRE") && word != QLatin1String("FLAG") && word != QLatin1String("SYMBOL")) {
            unread << tr("line %1 (%2)").arg(number).arg(QString(line.left(40)).replace(QChar(0), QLatin1Char(' ')).simplified());
        }
    }
    if (!unread.isEmpty())
        c.notes << QCoreApplication::translate("LTspiceImport", "%n line(s) not read, none of an .asc's words: %1", nullptr, int(unread.size()))
                       .arg(QStringList(unread.mid(0, 5)).join(QStringLiteral(", ")) + (unread.size() > 5 ? QStringLiteral(", ...") : QString()));
    // The nets: wires joined where they end, on each other part way along
    // too; flags and pins on them.
    Union nets;
    QList<QPoint> points;
    for (const auto& [a, b] : wires) {
        nets.join(a, b);
        points << a << b;
    }
    for (const auto& [at, label] : flags) points << at;
    // Each part's pins, turned and placed (each symbol read once).
    struct Placed {
        Part part;
        Symbol symbol;
        QList<QPoint> pins;
    };
    QList<Placed> placed;
    QStringList unknown;
    QHash<QString, std::optional<Symbol>> symbols;
    for (const Part& p : std::as_const(parts)) {
        const QString base = p.symbol.section(QLatin1Char('\\'), -1).section(QLatin1Char('/'), -1).toLower();
        if (!symbols.contains(p.symbol)) {
            std::optional<Symbol> symbol;
            for (const QString& folder : symbolFolders) {
                const QDir dir(folder);
                for (const QString& candidate : {base + QStringLiteral(".asy"), p.symbol.section(QLatin1Char('\\'), -1) + QStringLiteral(".asy")})
                    if (!symbol && QFileInfo(dir.filePath(candidate)).isFile()) symbol = readSymbol(dir.filePath(candidate), &c.notes);
            }
            if (!symbol && standard().contains(base)) symbol = standard().value(base);
            symbols.insert(p.symbol, symbol);
        }
        const std::optional<Symbol> symbol = symbols.value(p.symbol);
        if (!symbol) {
            unknown << QStringLiteral("%1 (%2)").arg(p.attributes.value(QStringLiteral("InstName"), QStringLiteral("?")), p.symbol);
            continue;
        }
        Placed one{p, *symbol, {}};
        for (const QPoint& pin : symbol->pins) one.pins << p.at + turned(pin, p.rotation);
        points << one.pins;
        placed << one;
    }
    for (const QPoint& p : std::as_const(points))
        for (const auto& [a, b] : wires)
            if (onSegment(p, a, b)) nets.join(p, a);
    // Their names: a flag's (0 is ground) as a label takes it, else N001,
    // N002 ... in order. A flag renamed so is said.
    QHash<QString, QString> asLabel;   // a flag -> its net's name here
    {
        QSet<QString> used;
        for (const auto& [at, label] : flags)
            if (labelName(label) == label) used.insert(label.toLower());
        for (const auto& [at, label] : flags) {
            if (asLabel.contains(label) || label == QLatin1String("0")) continue;
            const QString base = labelName(label);
            if (base == label) {
                asLabel.insert(label, label);
                continue;
            }
            QString name = base;
            for (int k = 2; used.contains(name.toLower()); ++k) name = QStringLiteral("%1_%2").arg(base).arg(k);
            used.insert(name.toLower());
            asLabel.insert(label, name);
            c.notes << tr("the net %1 is %2 here (a net label takes a letter first, then letters, digits and single _)").arg(label, name);
        }
    }
    QHash<QPoint, QString> named;
    QHash<QString, QPoint> byFlag;
    for (const auto& [at, label] : flags) {
        const QPoint root = nets.find(at);
        if (byFlag.contains(label)) nets.join(root, byFlag.value(label));
        else byFlag.insert(label, root);
    }
    for (const auto& [at, label] : flags) {
        const QPoint root = nets.find(at);
        if (!named.contains(root) || label == QLatin1String("0")) named.insert(root, label == QLatin1String("0") ? label : asLabel.value(label, label));
    }
    int next = 1;
    QHash<QPoint, int> pinsOnNet;
    for (const Placed& p : std::as_const(placed))
        for (const QPoint& pin : p.pins) pinsOnNet[nets.find(pin)] += 1;
    QSet<QString> taken;
    for (const QString& n : std::as_const(named)) taken.insert(n.toLower());
    const auto netOf = [&](const QPoint& at) {
        const QPoint root = nets.find(at);
        if (!named.contains(root)) {
            QString n;
            do n = QStringLiteral("N%1").arg(next++, 3, 10, QLatin1Char('0'));
            while (taken.contains(n.toLower()));
            taken.insert(n.toLower());
            named.insert(root, n);
        }
        return named.value(root);
    };
    // The models the directives define, and whether they include others.
    QSet<QString> defined;
    bool includes = false;
    for (const QString& d : std::as_const(directives)) {
        const QStringList f = d.split(QRegularExpression(QStringLiteral("[\\s(]+")), Qt::SkipEmptyParts);
        const QString word = f.value(0).toLower();
        if (word == QLatin1String(".model") || word == QLatin1String(".subckt")) defined.insert(f.value(1).toLower());
        if (word.startsWith(QLatin1String(".lib")) || word.startsWith(QLatin1String(".inc"))) includes = true;
    }
    QStringList undefined;
    // The elements.
    QStringList elements;
    QSet<QString> nameSet;
    for (const Placed& p : std::as_const(placed)) {
        QString inst = p.part.attributes.value(QStringLiteral("InstName"));
        if (inst.isEmpty()) inst = QStringLiteral("%1%2").arg(p.symbol.prefix).arg(elements.size() + 1);
        if (!inst.startsWith(p.symbol.prefix, Qt::CaseInsensitive)) inst = p.symbol.prefix + inst;
        QStringList nodes;
        for (int i = 0; i < p.pins.size(); ++i) {
            const QPoint pin = p.pins.at(i);
            nodes << netOf(pin);
            // A pin on nothing: said (its net is its own).
            bool joined = false;
            for (const auto& [a, b] : wires) joined = joined || onSegment(pin, a, b);
            for (const auto& [at, label] : flags) joined = joined || at == pin;
            if (!joined && pinsOnNet.value(nets.find(pin)) < 2)
                c.notes << tr("pin %1 of %2 at %3, %4 joins nothing").arg(i + 1).arg(inst).arg(pin.x()).arg(pin.y());
        }
        if (p.symbol.bulkIsSource) nodes << nodes.at(2);
        QString value = p.part.attributes.value(QStringLiteral("Value"), p.symbol.value);
        const QString value2 = p.part.attributes.value(QStringLiteral("Value2"));
        QString line = inst + QLatin1Char(' ') + nodes.join(QLatin1Char(' '));
        QStringList extraElements;
        const QChar prefix = p.symbol.prefix;
        if (prefix == QLatin1Char('X')) {
            const QString model = p.part.attributes.value(QStringLiteral("SpiceModel"), p.symbol.model);
            const QString sub = model.isEmpty() ? value : model;
            line += QLatin1Char(' ') + sub;
            if (!model.isEmpty() && !value.isEmpty() && value != model) line += QLatin1Char(' ') + value;
            if (!sub.isEmpty() && !defined.contains(sub.section(QLatin1Char(' '), 0, 0).toLower())) undefined << QStringLiteral("%1 (%2)").arg(inst, sub.section(QLatin1Char(' '), 0, 0));
        } else {
            // Its parameters: in its value (after the value itself), its
            // SpiceLine and SpiceLine2. LTspice's own - a capacitor's
            // V=50 Irms=1, an inductor's Ipk=1 mfg="Coilcraft" - left out
            // and said; its parasitics (Rser, Lser, Rpar, Cpar) made parts.
            QString extra = p.part.attributes.value(QStringLiteral("SpiceLine"));
            const QString extra2 = p.part.attributes.value(QStringLiteral("SpiceLine2"));
            if (!extra2.trimmed().isEmpty()) extra += QLatin1Char(' ') + extra2;
            static const QRegularExpression pair(QStringLiteral("\\b([A-Za-z_][A-Za-z0-9_]*)\\s*=\\s*(\"[^\"]*\"|\\{[^}]*\\}|\\S+)"));
            static const QRegularExpression anyParasitic(QStringLiteral("^(rser|lser|cpar|rpar|cser)$"));
            const bool listed = ngspiceParameters().contains(prefix);   // (R, C, L: what ngspice takes is known)
            const QSet<QString> known = ngspiceParameters().value(prefix);
            const QSet<QString> drawn = parasiticsOf().value(prefix);
            QStringList dropped;
            QList<std::pair<QString, QString>> parasitics;
            for (QString* text : {&value, &extra}) {
                QString rest = *text;
                for (auto it = pair.globalMatch(*text); it.hasNext();) {
                    const QRegularExpressionMatch m = it.next();
                    const QString key = m.captured(1).toLower();
                    if (drawn.contains(key)) parasitics << std::pair{key, m.captured(2)};
                    else if (listed ? !known.contains(key) : anyParasitic.match(key).hasMatch()) dropped << m.captured(0);
                    else continue;
                    rest.replace(m.captured(0), QString());
                }
                *text = rest.simplified();
            }
            if (!dropped.isEmpty())
                c.notes << tr("%1: %2 left out (LTspice's own, not ngspice's)").arg(inst, dropped.join(QLatin1Char(' ')));
            // The parasitics as parts: in series between the part and its
            // second node (Lser, then Rser), across both nodes (Rpar, Cpar).
            // One of 0 is none.
            const QString a = nodes.value(0), b = nodes.value(1);
            QStringList made, names;
            const auto part = [&](const QString& key, const QString& v, const QString& from, const QString& to) {
                const QString element = QStringLiteral("%1%2_%3").arg(key.at(0).toUpper()).arg(inst, key);
                extraElements << QStringLiteral("%1 %2 %3 %4").arg(element, from, to, valueText(v));
                made << QStringLiteral("%1=%2").arg(key, v);
                names << element;
            };
            QString last = b;
            for (const char* series : {"lser", "rser"})
                for (const auto& [key, v] : std::as_const(parasitics))
                    if (key == QLatin1String(series) && spiceNumber(v) != 0.0) {
                        const QString node = QStringLiteral("%1_%2").arg(inst, key);
                        part(key, v, node, last);
                        last = node;
                    }
            if (nodes.size() > 1) nodes[1] = last;
            for (const auto& [key, v] : std::as_const(parasitics))
                if ((key == QLatin1String("rpar") || key == QLatin1String("cpar")) && spiceNumber(v) != 0.0) part(key, v, a, b);
            if (!made.isEmpty())
                c.notes << tr("%1: %2 made parts of their own (%3), as LTspice draws them").arg(inst, made.join(QLatin1Char(' ')), names.join(QStringLiteral(", ")));
            line = inst + QLatin1Char(' ') + nodes.join(QLatin1Char(' '));
            line += QLatin1Char(' ') + valueText(value).simplified();
            if (!value2.isEmpty()) line += QLatin1Char(' ') + valueText(value2);
            if (!extra.simplified().isEmpty()) line += QLatin1Char(' ') + valueText(extra).simplified();
            // A model named in its value, defined nowhere here: LTspice
            // takes it from its own library (standard.dio, standard.bjt ...).
            if (QStringLiteral("DQMJZ").contains(prefix)) {
                const QString model = value.section(QLatin1Char(' '), 0, 0);
                if (!model.isEmpty() && !defined.contains(model.toLower())) undefined << QStringLiteral("%1 (%2)").arg(inst, model);
            }
        }
        if (nameSet.contains(inst.toLower())) c.notes << tr("%1 is named twice").arg(inst);
        nameSet.insert(inst.toLower());
        elements << line.simplified();
        elements << extraElements;
    }
    if (!undefined.isEmpty())
        c.notes << (includes ? tr("%1: models defined in none of its directives - the files it includes must define them (LTspice's own "
                                  "library, standard.dio, standard.bjt ..., ngspice does not have)")
                             : tr("%1: models defined nowhere in it - LTspice takes them from its own library (standard.dio, "
                                  "standard.bjt, standard.mos ...), which ngspice does not have: add their .model (or .lib) lines, "
                                  "or the run fails"))
                       .arg(undefined.join(QStringLiteral(", ")));
    for (const QString& u : std::as_const(unknown))
        c.notes << tr("%1: no symbol of that name here - left out (its .asy beside the .asc, or LTspice's lib/sym folder, gives its pins)").arg(u);
    if (elements.isEmpty()) {
        c.error = unknown.isEmpty() ? tr("the schematic has no parts") : tr("none of its parts' symbols is known: %1").arg(unknown.join(QStringLiteral(", ")));
        // (Why a symbol beside it was not taken, when one was not.)
        QStringList symbolNotes;
        for (const QString& n : std::as_const(c.notes))
            if (n.contains(QLatin1String(".asy:"))) symbolNotes << n;
        if (!symbolNotes.isEmpty()) c.error += QStringLiteral(" (%1)").arg(symbolNotes.join(QStringLiteral("; ")));
        return c;
    }
    c.parts = int(placed.size());
    QSet<QString> netNames;
    for (const QString& n : std::as_const(named)) netNames.insert(n);
    c.nets = int(netNames.size());
    QStringList out;
    out << QStringLiteral("* %1 (LTspice)").arg(name.isEmpty() ? QStringLiteral("converted") : name);
    out << elements << directives << QStringLiteral(".end");
    c.netlist = out.join(QLatin1Char('\n')) + QLatin1Char('\n');
    return c;
}

} // namespace qucs_s::ltspice
