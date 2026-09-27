/*
 * qucscontrol_paintings.cpp - the tools for Claude that draw on a
 *                             schematic or its symbol: paintings (texts,
 *                             arrows, lines, shapes, text boxes, tables,
 *                             dimensions, formulas) read, made and changed
 *                             by their named fields
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "qucscontrol.h"
#include "qucscontrol_p.h"

#include "dataset.h"
#include "misc.h"
#include "paintings/paintings.h"
#include "qucs.h"
#include "schematic.h"

#include <QColor>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <memory>

using namespace qucs_s::control;
using qucs_s::dataset::rounded;

namespace {

// The types add_painting takes, and the word each one's line begins with.
struct PaintingType {
    const char* type;
    const char* word;
    const char* what;
};
const PaintingType kTypes[] = {
    {"text", "Text", "a text"},
    {"line", "Line", "a straight line"},
    {"arrow", "Arrow", "an arrow"},
    {"rectangle", "Rectangle", "a rectangle"},
    {"ellipse", "Ellipse", "an ellipse (a circle)"},
    {"arc", "EArc", "an arc of an ellipse"},
    {"polyline", "Polyline", "lines through points; closed, a polygon"},
    {"image", "ImagePainting", "a picture, from a file"},
    {"rounded_rectangle", "RoundRect", "a rectangle with round corners"},
    {"polygon", "RegPolygon", "a regular polygon, or a star"},
    {"brace", "Brace", "a curly brace, a square bracket or a parenthesis"},
    {"waveform", "Waveform", "a waveform's picture: sine, square, triangle, sawtooth, pulse, damped sine"},
    {"text_box", "TextBox", "text in a box: a block, a note, or a callout pointing at something"},
    {"table", "Table", "a table of texts"},
    {"dimension", "Dimension", "a dimension: the distance between two points, written along a line"},
    {"formula", "Formula", "a formula in TeX, typeset"},
    {"port", ".PortSym", "a pin of a symbol (moved, and given a label - what the instances show beside the pin instead of its name, \"\" for nothing: the schematic's Port components make them)"},
    {"id", ".ID", "the symbol's name text (only moved)"},
};

const char* const kPenStyles[] = {"none", "solid", "dash", "dot", "dash_dot", "dash_dot_dot"};
const char* const kBrushStyles[] = {"none",   "solid",      "dense1",   "dense2", "dense3",
                                    "dense4", "dense5",     "dense6",   "dense7", "horizontal",
                                    "vertical", "cross", "backward_diagonal", "forward_diagonal", "diagonal_cross"};
const char* const kTextBoxKinds[] = {"block", "note", "callout"};

QString wordOf(const Painting* p)
{
    return p->Name.trimmed();
}

QString typeOfPainting(const Painting* p)
{
    const QString word = wordOf(p);
    for (const PaintingType& t : kTypes)
        if (word == QLatin1String(t.word)) return QString::fromLatin1(t.type);
    return word.toLower();
}

bool isLegacy(const QString& type)
{
    static const QStringList legacy{QStringLiteral("text"),     QStringLiteral("line"),     QStringLiteral("arrow"),
                                    QStringLiteral("rectangle"), QStringLiteral("ellipse"), QStringLiteral("arc"),
                                    QStringLiteral("polyline"),  QStringLiteral("image"),   QStringLiteral("port"),
                                    QStringLiteral("id")};
    return legacy.contains(type);
}

bool isShape(const QString& type)
{
    static const QStringList shapes{QStringLiteral("rounded_rectangle"), QStringLiteral("polygon"), QStringLiteral("brace"),
                                    QStringLiteral("waveform"), QStringLiteral("text_box"), QStringLiteral("table")};
    return shapes.contains(type);
}

// A new painting of a type; the ports and the name text of a symbol too
// when \a evenFixed (to be read from a line: they are not made anew).
Painting* newPainting(const QString& type, bool evenFixed = false)
{
    if (type == QLatin1String("text")) return new GraphicText();
    if (type == QLatin1String("line")) return new GraphicLine();
    if (type == QLatin1String("arrow")) return new Arrow();
    if (type == QLatin1String("rectangle")) return new qucs::Rectangle();
    if (type == QLatin1String("ellipse")) return new qucs::Ellipse();
    if (type == QLatin1String("arc")) return new EllipseArc();
    if (type == QLatin1String("polyline")) return new PolylinePainting();
    if (type == QLatin1String("image")) return new ImagePainting();
    if (type == QLatin1String("port")) return evenFixed ? new PortSymbol() : nullptr;
    if (type == QLatin1String("id")) return evenFixed ? new ID_Text() : nullptr;
    for (const PaintingType& t : kTypes)
        if (type == QLatin1String(t.type)) return Painting::newNamed(QString::fromLatin1(t.word));
    return nullptr;
}

QString colourText(const QColor& c)
{
    return c.alpha() == 255 ? c.name() : c.name(QColor::HexArgb);
}

QString penName(int style)
{
    return style >= 0 && style < int(std::size(kPenStyles)) ? QString::fromLatin1(kPenStyles[style]) : QString::number(style);
}

QString brushName(int style)
{
    return style >= 0 && style < int(std::size(kBrushStyles)) ? QString::fromLatin1(kBrushStyles[style]) : QString::number(style);
}

// A name of a list, or its index; -1 when it is neither.
template <std::size_t N>
int indexIn(const char* const (&names)[N], const QJsonValue& v)
{
    if (v.isDouble()) {
        const double d = v.toDouble();
        return d >= 0 && d < double(N) && d == std::floor(d) ? int(d) : -1;
    }
    const QString s = v.toString().trimmed().toLower().replace(QLatin1Char(' '), QLatin1Char('_')).replace(QLatin1Char('-'), QLatin1Char('_'));
    for (std::size_t i = 0; i < N; ++i)
        if (s == QLatin1String(names[i])) return int(i);
    return -1;
}

template <std::size_t N>
QString namesOf(const char* const (&names)[N])
{
    QStringList list;
    for (const char* n : names) list << QString::fromLatin1(n);
    return list.join(QStringLiteral(", "));
}

// A choice's text as a word: "curly brace" is curly_brace.
QString choiceWord(const QString& choice)
{
    return choice.trimmed().toLower().replace(QLatin1Char(' '), QLatin1Char('_'));
}

// A field of a painting's dialog by the name the tools give it.
QString propName(const QString& key)
{
    static const QHash<QString, QString> named{
        {QStringLiteral("lineColour"), QStringLiteral("color")},     {QStringLiteral("colour"), QStringLiteral("color")},
        {QStringLiteral("lineWidth"), QStringLiteral("thickness")},  {QStringLiteral("lineStyle"), QStringLiteral("style")},
        {QStringLiteral("fillColour"), QStringLiteral("fill_color")}, {QStringLiteral("fillStyle"), QStringLiteral("fill_style")},
        {QStringLiteral("textColour"), QStringLiteral("text_color")}, {QStringLiteral("headerColour"), QStringLiteral("header_color")},
        {QStringLiteral("fontSize"), QStringLiteral("font_size")}};
    if (const auto it = named.constFind(key); it != named.cend()) return *it;
    QString out;
    for (const QChar c : key) {
        if (c.isUpper()) out += QLatin1Char('_') + QString(c.toLower());
        else out += c;
    }
    return out;
}

QJsonValue fieldValue(const PaintingField& f)
{
    switch (f.kind) {
    case PaintingField::Colour: return colourText(f.value.value<QColor>());
    case PaintingField::Int: return f.value.toInt();
    case PaintingField::Real: return rounded(f.value.toDouble());
    case PaintingField::Choice: {
        const int i = f.value.toInt();
        return i >= 0 && i < f.choices.size() ? QJsonValue(choiceWord(f.choices.at(i))) : QJsonValue(i);
    }
    case PaintingField::Text:
    case PaintingField::LongText: return f.value.toString();
    case PaintingField::Check: return f.value.toBool();
    case PaintingField::PenStyle: return penName(f.value.toInt());
    case PaintingField::BrushStyle: return brushName(f.value.toInt());
    case PaintingField::Cells: {
        QJsonArray rows;
        for (const QVariant& row : f.value.toList()) rows.append(QJsonArray::fromStringList(row.toStringList()));
        return rows;
    }
    }
    return {};
}

QString kindText(const PaintingField& f)
{
    switch (f.kind) {
    case PaintingField::Colour: return QStringLiteral("a colour");
    case PaintingField::Int: return QStringLiteral("a whole number, %1 to %2").arg(f.min).arg(f.max);
    case PaintingField::Real: return QStringLiteral("a number, %1 to %2").arg(f.min).arg(f.max);
    case PaintingField::Choice: {
        QStringList words;
        for (const QString& c : f.choices) words << choiceWord(c);
        return words.join(QStringLiteral(" | "));
    }
    case PaintingField::Text: return QStringLiteral("text");
    case PaintingField::LongText: return QStringLiteral("text (lines)");
    case PaintingField::Check: return QStringLiteral("true | false");
    case PaintingField::PenStyle: return QStringLiteral("a line style");
    case PaintingField::BrushStyle: return QStringLiteral("a fill style");
    case PaintingField::Cells: return QStringLiteral("rows of texts, [[\"a\", \"b\"], ...]");
    }
    return {};
}

// \a v as the field \a f takes it; false and why in \a error.
bool fieldFrom(const PaintingField& f, const QString& name, const QJsonValue& v, QVariant* out, QString* error)
{
    switch (f.kind) {
    case PaintingField::Colour: {
        const QColor c = v.isString() ? misc::ColorFromString(v.toString().trimmed()) : QColor();
        if (!c.isValid()) {
            *error = tr("'%1' is a colour: #rrggbb, #aarrggbb or a name.").arg(name);
            return false;
        }
        *out = QVariant::fromValue(c);
        return true;
    }
    case PaintingField::Int:
    case PaintingField::Real: {
        const double d = v.toDouble(qQNaN());
        if (!v.isDouble() || !std::isfinite(d) || d < f.min || d > f.max
            || (f.kind == PaintingField::Int && d != std::floor(d))) {
            *error = tr("'%1' is %2.").arg(name, kindText(f));
            return false;
        }
        *out = f.kind == PaintingField::Int ? QVariant(int(d)) : QVariant(d);
        return true;
    }
    case PaintingField::Choice: {
        int i = -1;
        if (v.isDouble() && v.toDouble() == std::floor(v.toDouble()) && v.toDouble() >= 0 && v.toDouble() < f.choices.size())
            i = int(v.toDouble());
        for (int k = 0; k < f.choices.size() && i < 0; ++k)
            if (choiceWord(f.choices.at(k)) == choiceWord(v.toString())) i = k;
        if (i < 0) {
            *error = tr("'%1' is one of: %2.").arg(name, kindText(f));
            return false;
        }
        *out = i;
        return true;
    }
    case PaintingField::Text:
    case PaintingField::LongText:
        if (!v.isString()) {
            *error = tr("'%1' is text.").arg(name);
            return false;
        }
        *out = v.toString();
        return true;
    case PaintingField::Check:
        if (!v.isBool()) {
            *error = tr("'%1' is true or false.").arg(name);
            return false;
        }
        *out = v.toBool();
        return true;
    case PaintingField::PenStyle: {
        const int i = indexIn(kPenStyles, v);
        if (i < 1) {
            *error = tr("'%1' is one of: %2.").arg(name, QStringList{QStringLiteral("solid"), QStringLiteral("dash"), QStringLiteral("dot"),
                                                                    QStringLiteral("dash_dot"), QStringLiteral("dash_dot_dot")}.join(QStringLiteral(", ")));
            return false;
        }
        *out = i;
        return true;
    }
    case PaintingField::BrushStyle: {
        const int i = indexIn(kBrushStyles, v);
        if (i < 0) {
            *error = tr("'%1' is one of: %2.").arg(name, namesOf(kBrushStyles));
            return false;
        }
        *out = i;
        return true;
    }
    case PaintingField::Cells: {
        if (!v.isArray()) {
            *error = tr("'%1' is rows of texts: [[\"a\", \"b\"], [\"c\", \"d\"]].").arg(name);
            return false;
        }
        // Rows of texts; a flat list of texts is one row, the first. A row
        // that is not a list is refused, not taken as an empty one.
        const QJsonArray given = v.toArray();
        const bool flat = !given.isEmpty() && std::none_of(given.begin(), given.end(), [](const QJsonValue& r) { return r.isArray(); });
        const auto text = [](const QJsonValue& c) {
            return c.isString() ? c.toString() : c.isDouble() ? QString::number(c.toDouble(), 'g', 12) : QString();
        };
        QVariantList rows;
        if (flat) {
            QStringList row;
            for (const QJsonValue& c : given) row << text(c);
            rows << QVariant(row);
        } else {
            for (const QJsonValue& r : given) {
                if (!r.isArray()) {
                    *error = tr("'%1' is rows of texts, each a list: [[\"a\", \"b\"], [\"c\", \"d\"]].").arg(name);
                    return false;
                }
                QStringList row;
                for (const QJsonValue& c : r.toArray()) row << text(c);
                rows << QVariant(row);
            }
        }
        *out = rows;
        return true;
    }
    }
    return false;
}

// ----------------------------------------------------------------------
// A painting's line, field by field

QStringList tokensOf(const QString& line)
{
    return line.split(QLatin1Char(' '));
}

int tokenInt(const QStringList& t, int i)
{
    return t.value(i).toInt();
}

QJsonArray pointJson(int x, int y)
{
    return QJsonArray{x, y};
}

// The text of a Text line: between its first quote and the last character.
QString textOfLine(const QString& line)
{
    QString text = line.mid(line.indexOf(QLatin1Char('"')) + 1);
    text.chop(1);
    misc::convert2Unicode(text);
    return text;
}

// A painting as the tools give it: its place and look by name.
QJsonObject propsOf(Painting* p)
{
    const QString type = typeOfPainting(p);
    QJsonObject o;
    if (type == QLatin1String("image")) {
        // (Not its line: the picture's bytes are in it.)
        auto* image = static_cast<ImagePainting*>(p);
        const QRect b = p->boundingRect();
        o.insert(QStringLiteral("x"), b.left());
        o.insert(QStringLiteral("y"), b.top());
        o.insert(QStringLiteral("width"), b.width());
        o.insert(QStringLiteral("height"), b.height());
        o.insert(QStringLiteral("picture"), image->embeddedImage().isNull()
                                                ? QStringLiteral("none")
                                                : QStringLiteral("%1, %2 x %3 pixels").arg(image->embeddedImage().format())
                                                      .arg(image->embeddedImage().size().width()).arg(image->embeddedImage().size().height()));
        return o;
    }
    const QString line = p->save();
    const QStringList t = tokensOf(line);
    const auto colour = [&t](int i) { return colourText(misc::ColorFromString(t.value(i))); };
    if (type == QLatin1String("text")) {
        o = {{QStringLiteral("x"), tokenInt(t, 1)}, {QStringLiteral("y"), tokenInt(t, 2)}, {QStringLiteral("size"), tokenInt(t, 3)},
             {QStringLiteral("color"), colour(4)}, {QStringLiteral("angle"), tokenInt(t, 5)}, {QStringLiteral("text"), textOfLine(line)}};
    } else if (type == QLatin1String("line") || type == QLatin1String("arrow")) {
        const int x = tokenInt(t, 1), y = tokenInt(t, 2);
        o = {{QStringLiteral("from"), pointJson(x, y)}, {QStringLiteral("to"), pointJson(x + tokenInt(t, 3), y + tokenInt(t, 4))}};
        const int k = type == QLatin1String("arrow") ? 7 : 5;
        if (type == QLatin1String("arrow")) {
            o.insert(QStringLiteral("head_length"), tokenInt(t, 5));
            o.insert(QStringLiteral("head_width"), tokenInt(t, 6));
            o.insert(QStringLiteral("head"), tokenInt(t, 10) == 1 ? QStringLiteral("filled") : QStringLiteral("open"));
        }
        o.insert(QStringLiteral("color"), colour(k));
        o.insert(QStringLiteral("thickness"), tokenInt(t, k + 1));
        o.insert(QStringLiteral("style"), penName(tokenInt(t, k + 2)));
    } else if (type == QLatin1String("rectangle") || type == QLatin1String("ellipse")) {
        o = {{QStringLiteral("x"), tokenInt(t, 1)},          {QStringLiteral("y"), tokenInt(t, 2)},
             {QStringLiteral("width"), tokenInt(t, 3)},      {QStringLiteral("height"), tokenInt(t, 4)},
             {QStringLiteral("color"), colour(5)},           {QStringLiteral("thickness"), tokenInt(t, 6)},
             {QStringLiteral("style"), penName(tokenInt(t, 7))}, {QStringLiteral("fill_color"), colour(8)},
             {QStringLiteral("fill_style"), brushName(tokenInt(t, 9))}, {QStringLiteral("filled"), tokenInt(t, 10) != 0}};
    } else if (type == QLatin1String("arc")) {
        o = {{QStringLiteral("x"), tokenInt(t, 1)},     {QStringLiteral("y"), tokenInt(t, 2)},
             {QStringLiteral("width"), tokenInt(t, 3)}, {QStringLiteral("height"), tokenInt(t, 4)},
             {QStringLiteral("start_angle"), rounded(tokenInt(t, 5) / 16.0)},
             {QStringLiteral("span_angle"), rounded(tokenInt(t, 6) / 16.0)},
             {QStringLiteral("color"), colour(7)}, {QStringLiteral("thickness"), tokenInt(t, 8)},
             {QStringLiteral("style"), penName(tokenInt(t, 9))}};
    } else if (type == QLatin1String("polyline")) {
        const int n = tokenInt(t, 1);
        QJsonArray points;
        for (int i = 0; i < n; ++i) points.append(pointJson(tokenInt(t, 2 + 2 * i), tokenInt(t, 3 + 2 * i)));
        const int k = 2 + 2 * n;
        o = {{QStringLiteral("points"), points},
             {QStringLiteral("color"), colour(k)},        {QStringLiteral("thickness"), tokenInt(t, k + 1)},
             {QStringLiteral("style"), penName(tokenInt(t, k + 2))}, {QStringLiteral("fill_color"), colour(k + 3)},
             {QStringLiteral("fill_style"), brushName(tokenInt(t, k + 4))}, {QStringLiteral("filled"), tokenInt(t, k + 5) != 0},
             {QStringLiteral("closed"), tokenInt(t, k + 6) != 0}};
    } else if (type == QLatin1String("port")) {
        o = {{QStringLiteral("x"), tokenInt(t, 1)}, {QStringLiteral("y"), tokenInt(t, 2)}, {QStringLiteral("number"), t.value(3)}};
        if (t.size() > 5) {
            // Its name (the net's), and what its instances show beside the
            // pin instead, when it says: a label, or "" for nothing.
            const qucs_s::portsym::Name named = qucs_s::portsym::read(line.section(QLatin1Char(' '), 5));
            o.insert(QStringLiteral("name"), named.name);
            o.insert(QStringLiteral("label"), named.labelSet ? QJsonValue(named.label) : QJsonValue(QJsonValue::Null));
        }
    } else if (type == QLatin1String("id")) {
        // A subcircuit's name text: its prefix (SUB1, SUB2, ...) and the
        // parameters its instances take, each with its default.
        auto* id = static_cast<ID_Text*>(p);
        QJsonArray parameters;
        for (const auto& sp : id->subParameters)
            parameters.append(QJsonObject{{QStringLiteral("name"), sp->name.section(QLatin1Char('='), 0, 0)},
                                          {QStringLiteral("default"), sp->name.section(QLatin1Char('='), 1)},
                                          {QStringLiteral("description"), sp->description},
                                          {QStringLiteral("type"), sp->type},
                                          {QStringLiteral("shown"), sp->display}});
        o = {{QStringLiteral("x"), tokenInt(t, 1)}, {QStringLiteral("y"), tokenInt(t, 2)}, {QStringLiteral("prefix"), id->prefix},
             {QStringLiteral("parameters"), parameters}};
    } else if (isShape(type)) {
        o = {{QStringLiteral("x"), tokenInt(t, 1)},     {QStringLiteral("y"), tokenInt(t, 2)},
             {QStringLiteral("width"), tokenInt(t, 3)}, {QStringLiteral("height"), tokenInt(t, 4)},
             {QStringLiteral("angle"), tokenInt(t, 11)}, {QStringLiteral("mirrored"), tokenInt(t, 12) != 0}};
        if (type == QLatin1String("text_box")) {
            const int kind = std::clamp(tokenInt(t, 13), 0, 2);
            o.insert(QStringLiteral("kind"), QString::fromLatin1(kTextBoxKinds[kind]));
            if (tokenInt(t, 21) != 0) o.insert(QStringLiteral("tip"), pointJson(tokenInt(t, 22), tokenInt(t, 23)));
        }
    } else if (type == QLatin1String("dimension")) {
        o = {{QStringLiteral("from"), pointJson(tokenInt(t, 1), tokenInt(t, 2))}, {QStringLiteral("to"), pointJson(tokenInt(t, 3), tokenInt(t, 4))},
             {QStringLiteral("style"), penName(tokenInt(t, 8))}};
    } else if (type == QLatin1String("formula")) {
        o = {{QStringLiteral("x"), tokenInt(t, 1)}, {QStringLiteral("y"), tokenInt(t, 2)}, {QStringLiteral("angle"), tokenInt(t, 5)}};
    }
    if (const auto* editable = dynamic_cast<const FieldEditable*>(p))
        for (const PaintingField& f : editable->fields()) o.insert(propName(f.key), fieldValue(f));
    return o;
}

// The names a type takes (all its fields, and for a new one what places it).
QStringList propsOfType(const QString& type)
{
    std::unique_ptr<Painting> fresh(newPainting(type, true));
    if (!fresh) return {};
    QStringList keys = propsOf(fresh.get()).keys();
    if (type == QLatin1String("image")) {
        keys.removeAll(QStringLiteral("picture"));
        keys << QStringLiteral("file");
    }
    if (type == QLatin1String("text_box")) keys << QStringLiteral("tip");
    if (type == QLatin1String("port")) keys = QStringList{QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("label")};
    if (type == QLatin1String("id"))
        keys = QStringList{QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("prefix"), QStringLiteral("parameters")};
    keys.removeDuplicates();
    keys.sort();
    return keys;
}

bool intArg(const QJsonValue& v, const QString& name, int* out, QString* error, int lo = -1000000, int hi = 1000000)
{
    const double d = v.toDouble(qQNaN());
    if (!v.isDouble() || !std::isfinite(d) || d != std::floor(d) || d < lo || d > hi) {
        *error = tr("'%1' is a whole number from %2 to %3.").arg(name).arg(lo).arg(hi);
        return false;
    }
    *out = int(d);
    return true;
}

bool pointArg(const QJsonValue& v, const QString& name, QPoint* out, QString* error)
{
    const QJsonArray a = v.toArray();
    int x = 0, y = 0;
    if (!v.isArray() || a.size() != 2 || !intArg(a.at(0), name, &x, error) || !intArg(a.at(1), name, &y, error)) {
        *error = tr("'%1' is a place, [x, y].").arg(name);
        return false;
    }
    *out = QPoint(x, y);
    return true;
}

bool colourArg(const QJsonValue& v, const QString& name, QString* out, QString* error)
{
    const QColor c = v.isString() ? misc::ColorFromString(v.toString().trimmed()) : QColor();
    if (!c.isValid()) {
        *error = tr("'%1' is a colour: #rrggbb, #aarrggbb or a name.").arg(name);
        return false;
    }
    *out = colourText(c);
    return true;
}

bool penArg(const QJsonValue& v, const QString& name, int* out, QString* error)
{
    const int i = indexIn(kPenStyles, v);
    if (i < 0) {
        *error = tr("'%1' is one of: %2.").arg(name, namesOf(kPenStyles));
        return false;
    }
    *out = i;
    return true;
}

bool brushArg(const QJsonValue& v, const QString& name, int* out, QString* error)
{
    const int i = indexIn(kBrushStyles, v);
    if (i < 0) {
        *error = tr("'%1' is one of: %2.").arg(name, namesOf(kBrushStyles));
        return false;
    }
    *out = i;
    return true;
}

bool boolArg(const QJsonValue& v, const QString& name, bool* out, QString* error)
{
    if (!v.isBool()) {
        *error = tr("'%1' is true or false.").arg(name);
        return false;
    }
    *out = v.toBool();
    return true;
}

// The line of a painting of the old kinds (text, line, arrow, rectangle,
// ellipse, arc, polyline, and a symbol's ports and name) with \a changes
// made to \a line; empty and why in \a error when one does not do.
QString changedLine(const QString& type, const QString& line, const QJsonObject& changes, QString* error)
{
    QStringList t = tokensOf(line);
    const auto has = [&changes](const char* key) { return changes.contains(QLatin1String(key)); };
    const auto get = [&changes](const char* key) { return changes.value(QLatin1String(key)); };
    const auto setInt = [&](int index, const char* key, int lo = -1000000, int hi = 1000000) {
        if (!has(key)) return true;
        int v = 0;
        if (!intArg(get(key), QLatin1String(key), &v, error, lo, hi)) return false;
        t[index] = QString::number(v);
        return true;
    };
    const auto setColour = [&](int index, const char* key) {
        if (!has(key)) return true;
        QString v;
        if (!colourArg(get(key), QLatin1String(key), &v, error)) return false;
        t[index] = v;
        return true;
    };
    const auto setPen = [&](int index, const char* key) {
        if (!has(key)) return true;
        int v = 0;
        if (!penArg(get(key), QLatin1String(key), &v, error)) return false;
        t[index] = QString::number(v);
        return true;
    };
    const auto setBrush = [&](int index, const char* key) {
        if (!has(key)) return true;
        int v = 0;
        if (!brushArg(get(key), QLatin1String(key), &v, error)) return false;
        t[index] = QString::number(v);
        return true;
    };
    const auto setBool = [&](int index, const char* key) {
        if (!has(key)) return true;
        bool v = false;
        if (!boolArg(get(key), QLatin1String(key), &v, error)) return false;
        t[index] = v ? QStringLiteral("1") : QStringLiteral("0");
        return true;
    };
    // From and to, the second written from the first.
    const auto setEnds = [&]() {
        QPoint from(tokenInt(t, 1), tokenInt(t, 2));
        QPoint to = from + QPoint(tokenInt(t, 3), tokenInt(t, 4));
        if (has("from") && !pointArg(get("from"), QStringLiteral("from"), &from, error)) return false;
        if (has("to") && !pointArg(get("to"), QStringLiteral("to"), &to, error)) return false;
        t[1] = QString::number(from.x());
        t[2] = QString::number(from.y());
        t[3] = QString::number(to.x() - from.x());
        t[4] = QString::number(to.y() - from.y());
        return true;
    };
    bool ok = true;
    if (type == QLatin1String("text")) {
        ok = setInt(1, "x") && setInt(2, "y") && setInt(3, "size", 1, 400) && setColour(4, "color") && setInt(5, "angle", -360, 360);
        if (!ok) return {};
        QString text = textOfLine(line);
        if (has("text")) {
            if (!get("text").isString() || get("text").toString().isEmpty()) {
                *error = tr("'text' is the text to write (not empty).");
                return {};
            }
            text = get("text").toString();
        }
        misc::convert2ASCII(text);
        return QStringList(t.mid(0, 6)).join(QLatin1Char(' ')) + QStringLiteral(" \"") + text + QLatin1Char('"');
    }
    if (type == QLatin1String("line"))
        ok = setEnds() && setColour(5, "color") && setInt(6, "thickness", 0, 100) && setPen(7, "style");
    else if (type == QLatin1String("arrow")) {
        ok = setEnds() && setInt(5, "head_length", 0, 1000) && setInt(6, "head_width", 0, 1000) && setColour(7, "color")
          && setInt(8, "thickness", 0, 100) && setPen(9, "style");
        if (ok && has("head")) {
            const QString head = get("head").toString();
            if (head != QLatin1String("open") && head != QLatin1String("filled")) {
                *error = tr("'head' is open or filled.");
                return {};
            }
            t[10] = head == QLatin1String("filled") ? QStringLiteral("1") : QStringLiteral("0");
        }
    } else if (type == QLatin1String("rectangle") || type == QLatin1String("ellipse"))
        ok = setInt(1, "x") && setInt(2, "y") && setInt(3, "width", 0) && setInt(4, "height", 0) && setColour(5, "color")
          && setInt(6, "thickness", 0, 100) && setPen(7, "style") && setColour(8, "fill_color") && setBrush(9, "fill_style") && setBool(10, "filled");
    else if (type == QLatin1String("arc")) {
        ok = setInt(1, "x") && setInt(2, "y") && setInt(3, "width", 0) && setInt(4, "height", 0) && setColour(7, "color")
          && setInt(8, "thickness", 0, 100) && setPen(9, "style");
        for (const auto& [index, key] : {std::pair{5, "start_angle"}, std::pair{6, "span_angle"}}) {
            if (!ok || !has(key)) continue;
            const double d = get(key).toDouble(qQNaN());
            if (!get(key).isDouble() || !std::isfinite(d) || std::abs(d) > 360) {
                *error = tr("'%1' is in degrees, -360 to 360.").arg(QLatin1String(key));
                return {};
            }
            t[index] = QString::number(int(std::lround(d * 16)));
        }
    } else if (type == QLatin1String("polyline")) {
        const int n = tokenInt(t, 1);
        QStringList points;
        for (int i = 0; i < n; ++i) points << t.value(2 + 2 * i) << t.value(3 + 2 * i);
        QStringList rest = t.mid(2 + 2 * n);
        while (rest.size() < 7) rest << QStringLiteral("0");
        if (has("points")) {
            const QJsonArray list = get("points").toArray();
            if (list.size() < 2) {
                *error = tr("'points' is two places or more, [[x, y], [x, y], ...].");
                return {};
            }
            points.clear();
            for (const QJsonValue& v : list) {
                QPoint p;
                if (!pointArg(v, QStringLiteral("points"), &p, error)) return {};
                points << QString::number(p.x()) << QString::number(p.y());
            }
        }
        // Its look, after the points: the same setters on this part.
        QStringList head{t.value(0), QString::number(points.size() / 2)};
        t = head + points + rest;
        const int k = 2 + int(points.size());
        ok = setColour(k, "color") && setInt(k + 1, "thickness", 0, 100) && setPen(k + 2, "style") && setColour(k + 3, "fill_color")
          && setBrush(k + 4, "fill_style") && setBool(k + 5, "filled") && setBool(k + 6, "closed");
    } else if (type == QLatin1String("image")) {
        ok = setInt(1, "x") && setInt(2, "y");
    } else if (type == QLatin1String("port")) {
        ok = setInt(1, "x") && setInt(2, "y");
        if (!ok) return {};
        // What its instances show beside the pin instead of its name: a
        // label, "" for nothing, null for the name again.
        if (has("label")) {
            qucs_s::portsym::Name named = qucs_s::portsym::read(line.section(QLatin1Char(' '), 5));
            if (get("label").isNull()) {
                named.labelSet = false;
                named.label.clear();
            } else if (get("label").isString() && !get("label").toString().contains(QLatin1Char('"'))
                       && !get("label").toString().contains(QLatin1Char('\n'))) {
                named.labelSet = true;
                named.label = get("label").toString().trimmed();
            } else {
                *error = tr("'label' is what the instances show beside the pin instead of its name: a short text (no quotes), "
                            "\"\" for nothing, null for its name again.");
                return {};
            }
            t = t.mid(0, 5);
            while (t.size() < 5) t << QStringLiteral("0");
            t << qucs_s::portsym::write(named);
        }
    } else if (type == QLatin1String("id")) {
        // Its place, prefix and parameters - the line written again from
        // them ("1=R=1k=resistance=" each: shown, name=default, what, type).
        ok = setInt(1, "x") && setInt(2, "y");
        if (!ok) return {};
        QString prefix = t.value(3);
        if (has("prefix")) {
            prefix = get("prefix").toString().trimmed();
            if (prefix.isEmpty() || prefix.contains(QRegularExpression(QStringLiteral("[\\s\"=]")))) {
                *error = tr("'prefix' is a word: what the instances' names begin with (SUB gives SUB1, SUB2, ...).");
                return {};
            }
        }
        QStringList parameters;
        if (has("parameters")) {
            if (!get("parameters").isArray()) {
                *error = tr("'parameters' is [{\"name\": \"R\", \"default\": \"1k\", \"description\": ..., \"type\": ..., \"shown\": true}, ...].");
                return {};
            }
            for (const QJsonValue& v : get("parameters").toArray()) {
                const QJsonObject o = v.toObject();
                const QString name = o.value(QLatin1String("name")).toString().trimmed();
                const QString value = o.value(QLatin1String("default")).isString() ? o.value(QLatin1String("default")).toString()
                                    : o.value(QLatin1String("default")).isDouble() ? QString::number(o.value(QLatin1String("default")).toDouble(), 'g', 12)
                                                                                   : QString();
                const QString what = o.value(QLatin1String("description")).toString();
                const QString kind = o.value(QLatin1String("type")).toString();
                static const QRegularExpression bad(QStringLiteral("[\"=]"));
                if (name.isEmpty() || name.contains(QRegularExpression(QStringLiteral("\\s"))) || name.contains(bad)
                    || value.contains(bad) || what.contains(bad) || kind.contains(bad)) {
                    *error = tr("A parameter has a 'name' (a word) and may have a 'default', 'description' and 'type' - none with = or \" in it.");
                    return {};
                }
                const bool shown = !o.contains(QLatin1String("shown")) || o.value(QLatin1String("shown")).toBool();
                parameters << QStringLiteral("\"%1=%2=%3=%4=%5\"").arg(shown ? QStringLiteral("1") : QStringLiteral("0"), name, value, what, kind);
            }
        } else {
            for (int i = 1;; i += 2) {
                const QString sub = line.section(QLatin1Char('"'), i, i);
                if (sub.isEmpty()) break;
                parameters << QLatin1Char('"') + sub + QLatin1Char('"');
            }
        }
        QStringList head{t.value(0), t.value(1), t.value(2), prefix};
        return (head + parameters).join(QLatin1Char(' '));
    }
    return ok ? t.join(QLatin1Char(' ')) : QString();
}

// A painting of the kinds made of fields (shapes, a dimension, a formula)
// with \a changes made to \a p: first what places it (in its line), then
// its fields one by one, as its dialog sets them.
bool changeFields(Painting* p, const QString& type, const QJsonObject& changes, bool isNew, QString* error)
{
    QStringList t = tokensOf(p->save());
    const auto has = [&changes](const char* key) { return changes.contains(QLatin1String(key)); };
    const auto setInt = [&](int index, const char* key, int lo = -1000000, int hi = 1000000) {
        if (!has(key)) return true;
        int v = 0;
        if (!intArg(changes.value(QLatin1String(key)), QLatin1String(key), &v, error, lo, hi)) return false;
        t[index] = QString::number(v);
        return true;
    };
    const auto setAngle = [&](int index) {
        if (!has("angle")) return true;
        int v = 0;
        if (!intArg(changes.value(QLatin1String("angle")), QStringLiteral("angle"), &v, error, -270, 270) || v % 90 != 0) {
            *error = tr("'angle' is 0, 90, 180 or 270: quarter turns counter-clockwise.");
            return false;
        }
        t[index] = QString::number((v % 360 + 360) % 360);
        return true;
    };
    bool ok = true;
    if (isShape(type)) {
        ok = setInt(1, "x") && setInt(2, "y") && setInt(3, "width", 0) && setInt(4, "height", 0) && setAngle(11);
        if (ok && has("mirrored")) {
            bool m = false;
            ok = boolArg(changes.value(QLatin1String("mirrored")), QStringLiteral("mirrored"), &m, error);
            t[12] = m ? QStringLiteral("1") : QStringLiteral("0");
        }
        if (ok && type == QLatin1String("text_box")) {
            if (has("kind")) {
                const int kind = indexIn(kTextBoxKinds, changes.value(QLatin1String("kind")));
                if (kind < 0) {
                    *error = tr("'kind' is one of: %1.").arg(namesOf(kTextBoxKinds));
                    return false;
                }
                t[13] = QString::number(kind);
                // (A callout points: its pointer is on.)
                if (kind == 2 && !has("pointer")) t[21] = QStringLiteral("1");
            }
            if (has("pointer") && changes.value(QLatin1String("pointer")).isBool())
                t[21] = changes.value(QLatin1String("pointer")).toBool() ? QStringLiteral("1") : QStringLiteral("0");
            QPoint tip(tokenInt(t, 22), tokenInt(t, 23));
            if (has("tip")) {
                if (!pointArg(changes.value(QLatin1String("tip")), QStringLiteral("tip"), &tip, error)) return false;
            } else if (isNew && tokenInt(t, 21) != 0) {
                // Below left of it, as a click places a callout's tip.
                tip = QPoint(tokenInt(t, 1) - 40, tokenInt(t, 2) + tokenInt(t, 4) + 40);
            }
            t[22] = QString::number(tip.x());
            t[23] = QString::number(tip.y());
        }
    } else if (type == QLatin1String("dimension")) {
        QPoint from(tokenInt(t, 1), tokenInt(t, 2)), to(tokenInt(t, 3), tokenInt(t, 4));
        if (has("from")) ok = pointArg(changes.value(QLatin1String("from")), QStringLiteral("from"), &from, error);
        if (ok && has("to")) ok = pointArg(changes.value(QLatin1String("to")), QStringLiteral("to"), &to, error);
        t[1] = QString::number(from.x());
        t[2] = QString::number(from.y());
        t[3] = QString::number(to.x());
        t[4] = QString::number(to.y());
        if (ok && has("style")) {
            int v = 0;
            ok = penArg(changes.value(QLatin1String("style")), QStringLiteral("style"), &v, error);
            if (ok && v == 0) {
                *error = tr("'style' of a dimension is solid, dash, dot, dash_dot or dash_dot_dot.");
                return false;
            }
            t[8] = QString::number(v);
        }
    } else if (type == QLatin1String("formula")) {
        ok = setInt(1, "x") && setInt(2, "y") && setAngle(5);
    }
    if (!ok) return false;
    if (!p->load(t.join(QLatin1Char(' ')))) {
        *error = tr("That does not make a %1.").arg(type);
        return false;
    }
    // A new shape given no size: its own, as a click places one.
    if (isNew && isShape(type)) {
        p->MousePressing(nullptr);
        p->MousePressing(nullptr);
    }
    auto* editable = dynamic_cast<FieldEditable*>(p);
    if (editable == nullptr) return true;
    // The fields in their order: a table's rows and columns before its cells.
    for (const PaintingField& f : editable->fields()) {
        const QString name = propName(f.key);
        if (!changes.contains(name)) continue;
        if (name == QLatin1String("style") && type == QLatin1String("dimension")) continue;   // (in its line)
        QVariant v;
        if (!fieldFrom(f, name, changes.value(name), &v, error)) return false;
        editable->setField(f.key, v);
    }
    // (A field set before the size it depends on: cells after rows again.)
    if (changes.contains(QLatin1String("cells")))
        for (const PaintingField& f : editable->fields())
            if (f.key == QLatin1String("cells")) {
                QVariant v;
                if (fieldFrom(f, QStringLiteral("cells"), changes.value(QLatin1String("cells")), &v, error)) editable->setField(f.key, v);
            }
    return true;
}

// The painting \a type with \a changes: a new one (\a base null) or \a base
// changed; nullptr and why in \a error.
Painting* madePainting(const QString& type, Painting* base, const QJsonObject& changes, QString* error)
{
    const bool isNew = base == nullptr;
    std::unique_ptr<Painting> p(newPainting(type, !isNew));
    if (!p) {
        if (type == QLatin1String("port") || type == QLatin1String("id"))
            *error = tr("A symbol's %1 is not made by hand: its ports follow the schematic's Port components, and it has one name text.")
                         .arg(type == QLatin1String("port") ? tr("port") : tr("name text"));
        else {
            QStringList types;
            for (const PaintingType& t : kTypes)
                if (qstrcmp(t.type, "port") != 0 && qstrcmp(t.type, "id") != 0) types << QString::fromLatin1(t.type);
            *error = tr("There is no painting %1: %2.").arg(type, types.join(QStringLiteral(", ")));
        }
        return nullptr;
    }
    // What it does not have: refused, not left out without a word.
    const QStringList known = propsOfType(type);
    QStringList unknown;
    for (auto it = changes.begin(); it != changes.end(); ++it)
        if (!known.contains(it.key())) unknown << it.key();
    if (!unknown.isEmpty()) {
        *error = tr("A %1 has no %2; it takes: %3.").arg(type, unknown.join(QStringLiteral(", ")), known.join(QStringLiteral(", ")));
        return nullptr;
    }
    if (type == QLatin1String("image")) {
        auto* image = static_cast<ImagePainting*>(p.get());
        if (base != nullptr) image->load(base->save());
        if (changes.contains(QLatin1String("file"))) {
            const QString file = absolute(changes.value(QLatin1String("file")).toString().trimmed());
            if (!QFileInfo(file).isFile()) {
                *error = tr("There is no picture %1.").arg(file);
                return nullptr;
            }
            image->setImageFromPath(file);
            if (image->embeddedImage().isNull()) {
                *error = tr("%1 does not read as a picture (PNG, JPEG, SVG, ...).").arg(file);
                return nullptr;
            }
        } else if (isNew) {
            *error = tr("'file' names the picture.");
            return nullptr;
        }
        const QRect b = base != nullptr ? base->boundingRect() : QRect(QPoint(0, 0), image->embeddedImage().size());
        int x = b.left(), y = b.top(), w = b.width(), h = b.height();
        if (isNew || changes.contains(QLatin1String("file"))) {
            w = image->embeddedImage().size().width();
            h = image->embeddedImage().size().height();
        }
        const auto take = [&](const char* key, int* v, int lo) {
            return !changes.contains(QLatin1String(key)) || intArg(changes.value(QLatin1String(key)), QLatin1String(key), v, error, lo);
        };
        if (!take("x", &x, -1000000) || !take("y", &y, -1000000) || !take("width", &w, 1) || !take("height", &h, 1)) return nullptr;
        if (isNew && (!changes.contains(QLatin1String("x")) || !changes.contains(QLatin1String("y")))) {
            *error = tr("'x' and 'y' place it: its top left corner.");
            return nullptr;
        }
        image->setPlacement(x, y, x + w, y + h);
        return p.release();
    }
    if (isLegacy(type)) {
        if (isNew) {
            // What places it, which no default can.
            const QStringList places = type == QLatin1String("line") || type == QLatin1String("arrow") ? QStringList{QStringLiteral("from"), QStringLiteral("to")}
                                     : type == QLatin1String("polyline") ? QStringList{QStringLiteral("points")}
                                     : type == QLatin1String("text") ? QStringList{QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("text")}
                                                                      : QStringList{QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("width"), QStringLiteral("height")};
            for (const QString& k : places)
                if (!changes.contains(k)) {
                    *error = tr("A new %1 needs %2.").arg(type, places.join(QStringLiteral(", ")));
                    return nullptr;
                }
        }
        const QString line = changedLine(type, base != nullptr ? base->save() : p->save(), changes, error);
        if (line.isEmpty()) return nullptr;
        if (!p->load(line)) {
            *error = tr("That does not make a %1.").arg(type);
            return nullptr;
        }
        return p.release();
    }
    if (isNew) {
        const QStringList places = type == QLatin1String("dimension") ? QStringList{QStringLiteral("from"), QStringLiteral("to")}
                                                                       : QStringList{QStringLiteral("x"), QStringLiteral("y")};
        for (const QString& k : places)
            if (!changes.contains(k)) {
                *error = tr("A new %1 needs %2.").arg(type, places.join(QStringLiteral(", ")));
                return nullptr;
            }
        if (type == QLatin1String("formula") && !changes.value(QLatin1String("tex")).isString()) {
            *error = tr("A new formula needs 'tex': TeX math, without dollars.");
            return nullptr;
        }
    } else if (!p->load(base->save())) {
        *error = tr("That %1 could not be read again.").arg(type);
        return nullptr;
    }
    if (!changeFields(p.get(), type, changes, isNew, error)) return nullptr;
    return p.release();
}

} // namespace

namespace qucs_s::control {

QJsonObject paintingJson(Painting* p, int index)
{
    QJsonObject o = propsOf(p);
    o.insert(QStringLiteral("painting"), index);
    o.insert(QStringLiteral("type"), typeOfPainting(p));
    return o;
}

QJsonArray paintingsJson(const std::list<Painting*>& paintings, int most)
{
    QJsonArray list;
    int n = 0;
    for (Painting* p : paintings) {
        ++n;
        if (most >= 0 && list.size() >= most) break;
        list.append(paintingJson(p, n));
    }
    return list;
}

Painting* paintingOf(const std::list<Painting*>& paintings, const QJsonValue& which, QString* error)
{
    const int count = int(paintings.size());
    if (count == 0) {
        *error = tr("There is no painting there.");
        return nullptr;
    }
    const double d = which.toDouble(qQNaN());
    if (!which.isDouble() || d != std::floor(d) || d < 1 || d > count) {
        *error = tr("'painting' is its number, 1 to %1, as get_schematic lists them.").arg(count);
        return nullptr;
    }
    auto it = paintings.begin();
    std::advance(it, int(d) - 1);
    return *it;
}

bool isFixedPainting(const Painting* p)
{
    const QString type = typeOfPainting(p);
    return type == QLatin1String("port") || type == QLatin1String("id");
}

QString paintingFieldsText()
{
    QStringList lines;
    lines << QStringLiteral(
        "add_painting and edit_painting take these by name (colours #rrggbb, #aarrggbb or a name; styles: none, solid, dash, "
        "dot, dash_dot, dash_dot_dot; fill styles: %1; places in the schematic's units, y down):")
                 .arg(namesOf(kBrushStyles));
    for (const PaintingType& t : kTypes) {
        const QString type = QString::fromLatin1(t.type);
        QStringList parts;
        std::unique_ptr<Painting> fresh(newPainting(type, true));
        if (!fresh) continue;
        QHash<QString, QString> kinds;
        if (const auto* editable = dynamic_cast<const FieldEditable*>(fresh.get()))
            for (const PaintingField& f : editable->fields()) kinds.insert(propName(f.key), kindText(f));
        for (const QString& k : propsOfType(type))
            parts << (kinds.contains(k) ? QStringLiteral("%1 (%2)").arg(k, kinds.value(k)) : k);
        lines << QStringLiteral("%1 - %2: %3").arg(type, QString::fromLatin1(t.what), parts.join(QStringLiteral(", ")));
    }
    lines << QStringLiteral(
        "text: x y is where it begins, size in points, angle in degrees. line, arrow: from and to [x, y]; an arrow's head "
        "open or filled. rectangle, ellipse, arc, image and the shapes: x y the top left corner, width, height; an arc's "
        "angles in degrees (0 is 3 o'clock, counter-clockwise). The shapes turn in quarter turns ('angle' 0, 90, 180, 270) "
        "and mirror; a new one given no width and height gets its own size. text_box: kind block, note or callout, and "
        "'tip' [x, y] where a callout points. dimension: from and to, the points it measures; its text, when empty, is the "
        "distance times 'scale' with 'unit'. formula: x y its top left corner, 'tex' the formula.");
    return lines.join(QLatin1Char('\n'));
}

} // namespace qucs_s::control

// ----------------------------------------------------------------------
// The tools

Schematic* QucsControl::paintingsOf(const QJsonObject& args, std::list<Painting*>** list, QString* note, QString* error)
{
    Schematic* sch = schematic(args, error, false);
    if (sch == nullptr) return nullptr;
    // The symbol when asked, or when it is what the document shows (a
    // .sym file always does).
    const bool symbol = args.value(QLatin1String("symbol")).isBool() ? args.value(QLatin1String("symbol")).toBool() : sch->getSymbolMode();
    if (!symbol && sch->getIsSymbolOnly()) {
        *error = tr("%1 is a symbol file: it has only a symbol.").arg(titleOf(sch));
        return nullptr;
    }
    prepare(sch);
    // What the change is made on is shown, as Edit Circuit Symbol shows it.
    if (symbol != sch->getSymbolMode()) {
        QMetaObject::invokeMethod(a_app, "slotSymbolEdit", Qt::DirectConnection);
        if (sch->getSymbolMode() != symbol) {
            *error = tr("%1 could not be switched to its %2.").arg(titleOf(sch), symbol ? tr("symbol") : tr("schematic"));
            return nullptr;
        }
        *note = symbol ? tr("%1 now shows its symbol (as File > Edit Circuit Symbol, F9, does; the schematic's tools switch back "
                            "by themselves, and so does symbol: false here).").arg(titleOf(sch))
                       : tr("%1 now shows its schematic again.").arg(titleOf(sch));
    }
    *list = symbol ? &sch->a_SymbolPaints : &sch->a_DocPaints;
    return sch;
}

namespace {

// The changes a call asks for: its arguments less those that say where.
QJsonObject changesIn(const QJsonObject& args)
{
    QJsonObject changes = args;
    for (const char* key : {"path", "symbol", "painting", "type"}) changes.remove(QLatin1String(key));
    return changes;
}

QList<QPoint> cornersOf(const Painting* p)
{
    const QRect b = p->boundingRect();
    return {b.topLeft(), b.bottomRight()};
}

} // namespace

QJsonObject QucsControl::addPainting(const QJsonObject& args)
{
    const QString type = args.value(QLatin1String("type")).toString().trimmed().toLower();
    if (type.isEmpty()) return errorResult(tr("'type' is what to draw: text, arrow, line, rectangle, text_box, ... (describe_format "
                                              "with element painting lists them and their fields)."));
    QString error, note;
    std::list<Painting*>* list = nullptr;
    Schematic* sch = paintingsOf(args, &list, &note, &error);
    if (sch == nullptr) return errorResult(error);
    Painting* p = madePainting(type, nullptr, changesIn(args), &error);
    if (p == nullptr) return errorResult(error);
    list->push_back(p);
    finish(sch, cornersOf(p));
    QJsonObject result{{QStringLiteral("added"), paintingJson(p, int(list->size()))}, {QStringLiteral("one step to undo"), true}};
    // (A text painting takes _ and ^ as TeX does: a subscript, a superscript.)
    if (type == QLatin1String("text") && (changesIn(args).value(QLatin1String("text")).toString().contains(QLatin1Char('_'))
                                          || changesIn(args).value(QLatin1String("text")).toString().contains(QLatin1Char('^'))))
        note = (note.isEmpty() ? QString() : note + QLatin1Char(' '))
             + tr("In a text, _x writes x as a subscript and _{xy} several characters; ^ likewise a superscript: V_{out} "
                  "for a subscripted out. There is no escape - for a plain underscore use a text_box.");
    if (!note.isEmpty()) result.insert(QStringLiteral("note"), note);
    return jsonResult(result);
}

QJsonObject QucsControl::editPainting(const QJsonObject& args)
{
    QString error, note;
    std::list<Painting*>* list = nullptr;
    Schematic* sch = paintingsOf(args, &list, &note, &error);
    if (sch == nullptr) return errorResult(error);
    Painting* old = paintingOf(*list, args.value(QLatin1String("painting")), &error);
    if (old == nullptr) return errorResult(error);
    const QJsonObject changes = changesIn(args);
    if (changes.isEmpty()) return errorResult(tr("Nothing to change: give what changes by name (get_schematic lists its fields)."));
    const QString type = typeOfPainting(old);
    if (args.contains(QLatin1String("type")) && args.value(QLatin1String("type")).toString() != type)
        return errorResult(tr("Painting %1 is a %2: a painting keeps its type (delete it and add another).")
                               .arg(args.value(QLatin1String("painting")).toInt()).arg(type));
    Painting* p = madePainting(type, old, changes, &error);
    if (p == nullptr) return errorResult(error);
    const auto it = std::find(list->begin(), list->end(), old);
    const int index = int(std::distance(list->begin(), it)) + 1;
    *it = p;
    // (Whatever points at the old one - the view's focus - lets go of it.)
    sch->deselectElements(nullptr);
    delete old;
    finish(sch, cornersOf(p));
    QJsonObject result{{QStringLiteral("changed"), paintingJson(p, index)}, {QStringLiteral("one step to undo"), true}};
    if (!note.isEmpty()) result.insert(QStringLiteral("note"), note);
    return jsonResult(result);
}
