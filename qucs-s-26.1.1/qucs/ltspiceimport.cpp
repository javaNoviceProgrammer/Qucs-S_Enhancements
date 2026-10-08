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

// An .asy's pins (in their SpiceOrder) and its Prefix, SpiceModel and Value.
std::optional<Symbol> readSymbol(const QString& file)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    const QStringList lines = textOf(f.readAll()).split(QLatin1Char('\n'));
    Symbol s;
    QList<std::pair<int, QPoint>> pins;
    std::optional<QPoint> pin;
    int unordered = 1000;
    for (QString line : lines) {
        line = line.trimmed();
        const QStringList f = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (f.isEmpty()) continue;
        if (f.first() == QLatin1String("PIN") && f.size() >= 3) {
            if (pin) pins << std::pair{unordered++, *pin};
            pin = QPoint(f.at(1).toInt(), f.at(2).toInt());
        } else if (f.first() == QLatin1String("PINATTR") && f.value(1) == QLatin1String("SpiceOrder") && pin) {
            pins << std::pair{f.value(2).toInt(), *pin};
            pin.reset();
        } else if (f.first() == QLatin1String("SYMATTR") && f.size() >= 3) {
            const QString value = line.section(QLatin1Char(' '), 2);
            if (f.at(1) == QLatin1String("Prefix")) s.prefix = value.isEmpty() ? QChar() : value.at(0).toUpper();
            else if (f.at(1) == QLatin1String("SpiceModel")) s.model = value;
            else if (f.at(1) == QLatin1String("Value")) s.value = value;
        }
    }
    if (pin) pins << std::pair{unordered++, *pin};
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
    const qint64 cross = qint64(b.x() - a.x()) * (p.y() - a.y()) - qint64(b.y() - a.y()) * (p.x() - a.x());
    if (cross != 0) return false;
    return p.x() >= std::min(a.x(), b.x()) && p.x() <= std::max(a.x(), b.x()) && p.y() >= std::min(a.y(), b.y())
           && p.y() <= std::max(a.y(), b.y());
}

} // namespace

QString textOf(const QByteArray& bytes)
{
    // UTF-16, little-endian as LTspice writes it: its mark, or a NUL every
    // other byte.
    const bool bom = bytes.startsWith("\xFF\xFE");
    const bool nulls = bytes.size() >= 4 && bytes.at(1) == '\0' && bytes.at(3) == '\0';
    if (bom || nulls) {
        QStringDecoder utf16(QStringDecoder::Utf16LE);
        QString text = utf16(bom ? bytes.mid(2) : bytes);
        return text.remove(QLatin1Char('\r'));
    }
    QStringDecoder utf8(QStringDecoder::Utf8);
    QString text = utf8(bytes);
    if (utf8.hasError()) text = QString::fromLatin1(bytes);   // (Windows' 8 bits: µ is 0xB5)
    return text.remove(QLatin1Char('\r'));
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
    for (QString line : asc.split(QLatin1Char('\n'))) {
        line = line.trimmed();
        const QStringList f = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (f.isEmpty()) continue;
        const QString& word = f.first();
        if (word == QLatin1String("WIRE") && f.size() >= 5) {
            wires << std::pair{QPoint(f.at(1).toInt(), f.at(2).toInt()), QPoint(f.at(3).toInt(), f.at(4).toInt())};
        } else if (word == QLatin1String("FLAG") && f.size() >= 4) {
            flags << std::pair{QPoint(f.at(1).toInt(), f.at(2).toInt()), f.at(3)};
        } else if (word == QLatin1String("SYMBOL") && f.size() >= 4) {
            Part p;
            p.symbol = f.at(1);
            p.at = QPoint(f.at(2).toInt(), f.at(3).toInt());
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
                if (t.startsWith(QLatin1String(".backanno"), Qt::CaseInsensitive)) continue;
                directives << directiveText(valueText(t), &c.notes);
            }
        }
    }
    // The nets: wires joined where they end, on each other part way along
    // too; flags and pins on them.
    Union nets;
    QList<QPoint> points;
    for (const auto& [a, b] : wires) {
        nets.join(a, b);
        points << a << b;
    }
    for (const auto& [at, label] : flags) points << at;
    // Each part's pins, turned and placed.
    struct Placed {
        Part part;
        Symbol symbol;
        QList<QPoint> pins;
    };
    QList<Placed> placed;
    QStringList unknown;
    for (const Part& p : std::as_const(parts)) {
        const QString base = p.symbol.section(QLatin1Char('\\'), -1).section(QLatin1Char('/'), -1).toLower();
        std::optional<Symbol> symbol;
        for (const QString& folder : symbolFolders) {
            const QDir dir(folder);
            for (const QString& candidate : {base + QStringLiteral(".asy"), p.symbol.section(QLatin1Char('\\'), -1) + QStringLiteral(".asy")})
                if (!symbol) symbol = readSymbol(dir.filePath(candidate));
        }
        if (!symbol && standard().contains(base)) symbol = standard().value(base);
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
    // Their names: a flag's (0 is ground), else N001, N002 ... in order.
    QHash<QPoint, QString> named;
    QHash<QString, QPoint> byFlag;
    for (const auto& [at, label] : flags) {
        const QPoint root = nets.find(at);
        if (byFlag.contains(label)) nets.join(root, byFlag.value(label));
        else byFlag.insert(label, root);
    }
    for (const auto& [at, label] : flags) {
        const QPoint root = nets.find(at);
        if (!named.contains(root) || label == QLatin1String("0")) named.insert(root, label);
    }
    int next = 1;
    QHash<QPoint, int> pinsOnNet;
    for (const Placed& p : std::as_const(placed))
        for (const QPoint& pin : p.pins) pinsOnNet[nets.find(pin)] += 1;
    const auto netOf = [&](const QPoint& at) {
        const QPoint root = nets.find(at);
        if (!named.contains(root)) named.insert(root, QStringLiteral("N%1").arg(next++, 3, 10, QLatin1Char('0')));
        return named.value(root);
    };
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
        if (p.symbol.prefix == QLatin1Char('X')) {
            const QString model = p.part.attributes.value(QStringLiteral("SpiceModel"), p.symbol.model);
            line += QLatin1Char(' ') + (model.isEmpty() ? value : model);
            if (!model.isEmpty() && !value.isEmpty() && value != model) line += QLatin1Char(' ') + value;
        } else {
            // LTspice's own: a series resistance of a source or a capacitor.
            QString extra = p.part.attributes.value(QStringLiteral("SpiceLine"));
            static const QRegularExpression parasitic(QStringLiteral("\\b(Rser|Lser|Cpar|Rpar|Cser)=\\S+"), QRegularExpression::CaseInsensitiveOption);
            for (QString* s : {&value, &extra})
                if (s->contains(parasitic)) {
                    c.notes << tr("%1: %2 left out (LTspice's own, not ngspice's)").arg(inst, s->trimmed());
                    s->remove(parasitic);
                }
            line += QLatin1Char(' ') + valueText(value).simplified();
            if (!value2.isEmpty()) line += QLatin1Char(' ') + valueText(value2);
            if (!extra.simplified().isEmpty()) line += QLatin1Char(' ') + valueText(extra).simplified();
        }
        if (nameSet.contains(inst.toLower())) c.notes << tr("%1 is named twice").arg(inst);
        nameSet.insert(inst.toLower());
        elements << line.simplified();
    }
    for (const QString& u : std::as_const(unknown))
        c.notes << tr("%1: no symbol of that name here - left out (its .asy beside the .asc, or LTspice's lib/sym folder, gives its pins)").arg(u);
    if (elements.isEmpty()) {
        c.error = unknown.isEmpty() ? tr("the schematic has no parts") : tr("none of its parts' symbols is known: %1").arg(unknown.join(QStringLiteral(", ")));
        return c;
    }
    c.parts = int(elements.size());
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
