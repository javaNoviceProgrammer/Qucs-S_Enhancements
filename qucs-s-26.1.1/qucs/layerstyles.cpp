/*
 * layerstyles.cpp - how a layout's layers look: KLayout's layer properties
 *                   files (.lyp) read whole - colours and their brightness,
 *                   dither patterns (KLayout's own and a file's), line
 *                   styles, widths, transparency, crosses and marks,
 *                   groups, the order the file lists them in, and which
 *                   layers a source names - for the layout viewer to draw
 *                   them as KLayout does
 *
 * The file's format is KLayout's, as its documentation describes it. The
 * built-in patterns and line styles, the default colours and the
 * brightness are measured from the pictures KLayout draws. No KLayout
 * source or file is in here.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "layout.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QFile>
#include <QRegularExpression>
#include <QXmlStreamReader>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace qucs_s::layout {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("Layout", text);
}

int wrap(int a, int n)
{
    return ((a % n) + n) % n;
}

// ----------------------------------------------------------------------
// KLayout's patterns by name, each a tile of rows from the top ('*' the
// pixels drawn), tiled from the screen's lower left - as KLayout draws
// them for the first layer in its list (each further one a row lower):
// read from its pictures, each on a box 32 pixels inside, at 1 and 2
// pixels a micrometre.

struct Rule {
    const char* name;
    const char* rows;   // its rows, top first, a space between
};

const Rule kStipples[] = {
    {"solid", "*"},
    {"hollow", "."},
    {"dotted", "*. .*"},
    {"coarsely dotted", "*... .... ..*. ...."},
    {"left-hatched", "*... .*.. ..*. ...*"},
    {"lightly left-hatched", "*....... .*...... ..*..... ...*.... ....*... .....*.. ......*. .......*"},
    {"strongly left-hatched dense", "**.. .**. ..** *..*"},
    {"strongly left-hatched sparse", "**...... .**..... ..**.... ...**... ....**.. .....**. ......** *......*"},
    {"right-hatched", "*... ...* ..*. .*.."},
    {"lightly right-hatched", "*....... .......* ......*. .....*.. ....*... ...*.... ..*..... .*......"},
    {"strongly right-hatched dense", "**.. *..* ..** .**."},
    {"strongly right-hatched sparse", "**...... *......* ......** .....**. ....**.. ...**... ..**.... .**....."},
    {"cross-hatched", "*... .*.* ..*. .*.*"},
    {"lightly cross-hatched", "*....... .*.....* ..*...*. ...*.*.. ....*... ...*.*.. ..*...*. .*.....*"},
    {"checkerboard 2px", "**.. **.. ..** ..**"},
    {"strongly cross-hatched sparse", "**...... ***....* ..**..** ...****. ....**.. ...****. ..**..** ***....*"},
    {"heavy checkerboard", "****.... ****.... ****.... ****.... ....**** ....**** ....**** ....****"},
    {"hollow bubbles", ".*...*.. *.*..... .*...*.. ....*.*."},
    {"solid bubbles", ".*...*.. ***..... .*...*.. ....***."},
    {"pyramids", ".*...... *.*..... ****...* ........ ....*... ...*.*.. ..*****. ........"},
    {"turned pyramids", "****...* *.*..... .*...... ........ ..*****. ...*.*.. ....*... ........"},
    {"plus", "..*...*. ..*..... *****... ..*..... ..*...*. ......*. *...**** ......*."},
    {"minus", "........ ........ *****... ........ ........ ........ *...**** ........"},
    {"22.5 degree down", "*......* .**..... ...**... .....**."},
    {"22.5 degree up", "*......* .....**. ...**... .**....."},
    {"67.5 degree down", "*... .*.. .*.. ..*. ..*. ...* ...* *..."},
    {"67.5 degree up", "...* ..*. ..*. .*.. .*.. *... *... ...*"},
    {"22.5 degree cross hatched", "*......* .**..**. ...**... .**..**."},
    {"zig zag", "..*. .*.* *... ...."},
    {"sine", "..***... .*...*.. *.....** ........"},
    {"heavy unordered", "****.*.* **.****. *.**.*** *****.*. .**.**** **.***.* .****.** *.*.****"},
    {"light unordered", "....*.*. ..*....* .*..*... .....*.* *..*.... ..*...*. *....*.. .*.*...."},
    {"vertical dense", "*."},
    {"vertical", ".*.."},
    {"vertical thick", ".**."},
    {"vertical sparse", "...*...."},
    {"vertical sparse, thick", "...**..."},
    {"horizontal dense", "* ."},
    {"horizontal", ". * . ."},
    {"horizontal thick", ". * * ."},
    {"horizontal sparse", ". . . * . . . ."},
    {"horizontal sparse, thick", ". . . * * . . ."},
    {"grid dense", "** *."},
    {"grid", ".*.. **** .*.. .*.."},
    {"grid thick", ".**. **** **** .**."},
    {"grid sparse", "...*.... ...*.... ...*.... ******** ...*.... ...*.... ...*.... ...*...."},
    {"grid sparse, thick", "...**... ...**... ...**... ******** ******** ...**... ...**... ...**..."},
};

// KLayout's line styles by name: the pixels drawn ('*') and left ('.')
// along a line, repeated from the screen's left (or bottom) - as KLayout
// draws them, read from its pictures.
const std::pair<const char*, const char*> kLines[] = {
    {"solid", ""},
    {"dotted", "*."},
    {"dashed", "**..**"},
    {"dash-dotted", "***..**..***"},
    {"short dashed", "*..*"},
    {"short dash-dotted", "**.*.*"},
    {"long dashed", "*****..*****"},
    {"dash-double-dotted", "***..*.*..**"},
};

// KLayout's colours for a layer given none, by its layer number - 1 the
// first, 36 the last, 37 the first again (0 the last), whatever its
// datatype - read from the pictures it draws of layers given none; its
// pattern lightly right-hatched at an even place in the list, lightly
// left-hatched at an odd one.
const QRgb kDefaultColours[] = {0xff80a8, 0xc080ff, 0x9580ff, 0x8086ff, 0x80a8ff, 0xff0000, 0xff0080, 0xff00ff, 0x8000ff,
                                0x0000ff, 0x0080ff, 0x800000, 0x800057, 0x800080, 0x500080, 0x000080, 0x004080, 0x80fffb,
                                0x80ff8d, 0xafff80, 0xf3ff80, 0xffc280, 0xffa080, 0x00ffff, 0x01ff6b, 0x91ff00, 0xddff00,
                                0xffae00, 0xff8000, 0x008080, 0x008050, 0x008000, 0x508000, 0x808000, 0x805000, 0xff9d9d};

QColor defaultColour(quint32 layer)
{
    const quint32 n = quint32(std::size(kDefaultColours));
    return QColor::fromRgb(kDefaultColours[(layer % n + n - 1) % n]);
}

int defaultStipple(int place)
{
    return std::max(0, place) % 2 == 0 ? 9 : 5;
}

LineStyle lineFrom(const QString& pattern, const QString& name)
{
    LineStyle l;
    l.name = name;
    const QString p = pattern.trimmed().left(32);
    for (int t = 0; t < p.size(); ++t)
        if (p.at(t) != QLatin1Char('.') && p.at(t) != QLatin1Char(' ')) l.bits |= 1u << t;
    l.length = l.bits == 0 ? 0 : int(p.size());
    // All drawn: solid.
    if (l.length > 0 && l.bits == (l.length == 32 ? 0xffffffffu : ((1u << l.length) - 1))) l.length = 0;
    return l;
}

// What a pattern or a line style refers to: "I5" (KLayout's own fifth),
// "C0" (the file's own first); -1 for anything else.
bool reference(const QString& text, QChar* kind, int* index)
{
    static const QRegularExpression re(QStringLiteral("^\\s*([ICic])\\s*(\\d+)\\s*$"));
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch()) return false;
    *kind = m.captured(1).at(0).toUpper();
    bool ok = false;
    *index = m.captured(2).toInt(&ok);
    return ok;
}

std::shared_ptr<const Stipple> sharedStipple(int index)
{
    static std::vector<std::shared_ptr<const Stipple>> standard;
    if (standard.empty())
        for (int i = 0; i < standardStippleCount(); ++i) standard.push_back(std::make_shared<Stipple>(standardStipple(i)));
    return standard[std::size_t(index >= 0 && index < int(standard.size()) ? index : 0)];
}

// The fill's bitmap a .lyp's value names; none given, KLayout's default
// for the layer's \a place in the list.
std::shared_ptr<const Stipple> stippleNamed(const QString& dither, const LayerProperties& lyp, int place)
{
    if (dither.trimmed().isEmpty()) return sharedStipple(defaultStipple(place));
    QChar kind;
    int index = -1;
    if (!reference(dither, &kind, &index)) return sharedStipple(defaultStipple(place));
    if (kind == QLatin1Char('C')) {
        if (index >= 0 && index < lyp.stipples.size()) return std::make_shared<Stipple>(lyp.stipples.at(index));
        return sharedStipple(0);
    }
    return sharedStipple(index);
}

LineStyle lineNamed(const QString& line, const LayerProperties& lyp)
{
    QChar kind;
    int index = -1;
    if (line.trimmed().isEmpty() || !reference(line, &kind, &index)) return LineStyle();
    if (kind == QLatin1Char('C')) return index >= 0 && index < lyp.lines.size() ? lyp.lines.at(index) : LineStyle();
    return standardLineStyle(index);
}

// KLayout's dither patterns, as near as Qt's patterns come (the viewer's
// own look).
Qt::BrushStyle patternOf(const QString& dither)
{
    QChar kind;
    int n = -1;
    if (dither.trimmed().isEmpty()) return Qt::SolidPattern;
    if (!reference(dither, &kind, &n) || kind == QLatin1Char('C')) return Qt::DiagCrossPattern;
    if (n == 0) return Qt::SolidPattern;
    if (n == 1) return Qt::NoBrush;
    if (n <= 3) return Qt::Dense6Pattern;   // dotted
    if (n <= 7) return Qt::FDiagPattern;    // left-hatched
    if (n <= 11) return Qt::BDiagPattern;   // right-hatched
    if (n <= 13 || n == 15) return Qt::DiagCrossPattern;
    if (n == 14 || n == 16) return Qt::Dense4Pattern;   // checkerboards
    if (n == 21) return Qt::CrossPattern;
    if (n == 22) return Qt::HorPattern;
    if (n >= 32 && n <= 36) return Qt::VerPattern;
    if (n >= 37 && n <= 41) return Qt::HorPattern;
    if (n >= 42 && n <= 46) return Qt::CrossPattern;
    return Qt::Dense5Pattern;
}

// ----------------------------------------------------------------------
// Reading a .lyp

bool truth(const QString& text)
{
    return text.trimmed().compare(QLatin1String("true"), Qt::CaseInsensitive) == 0 || text.trimmed() == QLatin1String("1");
}

QColor colourOf(const QString& text)
{
    const QString t = text.trimmed();
    if (t.isEmpty()) return QColor();
    QColor c(t);
    if (c.isValid()) c.setAlpha(255);   // (#aarrggbb: KLayout draws opaque)
    return c;
}

int integer(const QString& text, int otherwise)
{
    bool ok = false;
    const int v = text.trimmed().toInt(&ok);
    return ok ? v : otherwise;
}

LayerEntry readEntry(QXmlStreamReader& xml)
{
    LayerEntry e;
    while (xml.readNextStartElement()) {
        const QStringView name = xml.name();
        if (name == QLatin1String("group-members")) {
            e.group = true;
            e.members.append(readEntry(xml));
            continue;
        }
        const QString text = xml.readElementText(QXmlStreamReader::SkipChildElements);
        if (name == QLatin1String("frame-color")) e.frame = colourOf(text);
        else if (name == QLatin1String("fill-color")) e.fill = colourOf(text);
        else if (name == QLatin1String("frame-brightness")) e.frameBrightness = std::clamp(integer(text, 0), -255, 255);
        else if (name == QLatin1String("fill-brightness")) e.fillBrightness = std::clamp(integer(text, 0), -255, 255);
        else if (name == QLatin1String("dither-pattern")) e.dither = text.trimmed();
        else if (name == QLatin1String("line-style")) e.line = text.trimmed();
        else if (name == QLatin1String("visible")) e.visible = truth(text);
        else if (name == QLatin1String("valid")) e.valid = truth(text);
        else if (name == QLatin1String("transparent")) e.transparent = truth(text);
        else if (name == QLatin1String("xfill")) e.xfill = truth(text);
        else if (name == QLatin1String("marked")) e.marked = truth(text);
        else if (name == QLatin1String("width") && !text.trimmed().isEmpty()) e.width = std::clamp(integer(text, 1), 0, 32);
        else if (name == QLatin1String("animation") && !text.trimmed().isEmpty()) e.animation = std::clamp(integer(text, 0), 0, 3);
        else if (name == QLatin1String("name")) e.name = text.trimmed();
        else if (name == QLatin1String("source")) e.source = text.trimmed();
    }
    return e;
}

// A file's own pattern or line style: its rows (a pattern's <line>s, or a
// line style's one), its order and name.
void readCustom(QXmlStreamReader& xml, QStringList* rows, int* order, QString* name)
{
    while (xml.readNextStartElement()) {
        if (xml.name() == QLatin1String("pattern")) {
            // A pattern's <line>s, or a line style's text.
            QString text;
            while (!xml.atEnd()) {
                xml.readNext();
                if (xml.isCharacters()) text += xml.text();
                else if (xml.isStartElement() && xml.name() == QLatin1String("line")) rows->append(xml.readElementText(QXmlStreamReader::SkipChildElements).trimmed());
                else if (xml.isStartElement()) xml.skipCurrentElement();
                else if (xml.isEndElement()) break;
            }
            if (rows->isEmpty() && !text.trimmed().isEmpty()) rows->append(text.trimmed());
        } else if (xml.name() == QLatin1String("order")) {
            *order = integer(xml.readElementText(QXmlStreamReader::SkipChildElements), *order);
        } else if (xml.name() == QLatin1String("name")) {
            *name = xml.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
        } else {
            xml.skipCurrentElement();
        }
    }
}

Stipple stippleFrom(const QStringList& rows, const QString& name)
{
    Stipple s;
    s.name = name;
    s.height = std::clamp(int(rows.size()), 1, 32);
    s.width = 1;
    for (int y = 0; y < s.height && y < rows.size(); ++y) {
        const QString row = rows.at(y).left(32);
        s.width = std::max(s.width, int(row.size()));
        for (int x = 0; x < row.size(); ++x)
            if (row.at(x) != QLatin1Char('.') && row.at(x) != QLatin1Char(' ')) s.rows[std::size_t(y)] |= 1u << x;
    }
    return s;
}

void readTab(QXmlStreamReader& xml, LayerProperties* lyp, QVector<QPair<int, Stipple>>* stipples, QVector<QPair<int, LineStyle>>* lines)
{
    while (xml.readNextStartElement()) {
        const QStringView name = xml.name();
        if (name == QLatin1String("properties")) {
            lyp->entries.append(readEntry(xml));
        } else if (name == QLatin1String("custom-dither-pattern") || name == QLatin1String("custom-line-style")) {
            const bool pattern = name == QLatin1String("custom-dither-pattern");
            QStringList rows;
            int order = int(pattern ? stipples->size() : lines->size()) + 1;
            QString title;
            readCustom(xml, &rows, &order, &title);
            if (pattern) stipples->append({order, stippleFrom(rows, title)});
            else lines->append({order, lineFrom(rows.isEmpty() ? QString() : rows.first(), title)});
        } else {
            xml.skipCurrentElement();
        }
    }
}

LayerProperties parse(QIODevice* device, QString* error)
{
    LayerProperties lyp;
    QXmlStreamReader xml(device);
    QVector<QPair<int, Stipple>> stipples;
    QVector<QPair<int, LineStyle>> lines;
    bool any = false;
    if (xml.readNextStartElement()) {
        if (xml.name() == QLatin1String("layer-properties")) {
            any = true;
            readTab(xml, &lyp, &stipples, &lines);
        } else if (xml.name() == QLatin1String("layer-properties-tabs")) {
            // Its last tab, the one KLayout shows when it has read them;
            // the patterns of every tab.
            while (xml.readNextStartElement()) {
                if (xml.name() != QLatin1String("layer-properties")) {
                    xml.skipCurrentElement();
                    continue;
                }
                any = true;
                LayerProperties tab;
                readTab(xml, &tab, &stipples, &lines);
                lyp.entries = tab.entries;
            }
        }
    }
    if (xml.hasError() || !any || lyp.entries.isEmpty()) {
        if (error != nullptr)
            *error = xml.hasError() ? tr("It is not a layer properties file: %1 (line %2).").arg(xml.errorString()).arg(xml.lineNumber())
                     : !any         ? tr("It is not a layer properties file: it has no <layer-properties>.")
                                    : tr("It has no layers.");
        return {};
    }
    // The file's own, C0 the first by their order.
    std::stable_sort(stipples.begin(), stipples.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::stable_sort(lines.begin(), lines.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& s : std::as_const(stipples)) lyp.stipples.append(s.second);
    for (const auto& l : std::as_const(lines)) lyp.lines.append(l.second);
    return lyp;
}

// ----------------------------------------------------------------------
// Resolving: each layer the file lists, with what its groups give it.

struct Leaf {
    LayerEntry entry;      // with its groups' settings where it has none
    LayerSource source;
    bool visible = true;   // and its groups'
    QVector<int> path;     // the groups it is in, from the top: their indices in groups
    int place = 0;         // its row in KLayout's list, a group's row counted too
    int nth = 0;           // its place among the layers' rows alone (groups' not counted)
    bool everything = false;   // */* (or no source): not KLayout's (it lists the layers instead)
};

bool namesEverything(const LayerSource& s)
{
    return s.name.isEmpty() && (!s.numbered || (s.layerFrom < 0 && s.datatypeFrom < 0));
}

// Whether KLayout draws an entry of \a s on a layer: one layer named, by
// its numbers or its name - a wildcard's (*/0, 1/*) draws none.
bool bindsOne(const LayerSource& s)
{
    return s.numbered ? s.layerFrom >= 0 && s.datatypeFrom >= 0 : !s.name.isEmpty();
}

struct Resolved {
    QVector<Leaf> leaves;
    int rows = 0;     // the rows of KLayout's list, groups' and layers'
    int layers = 0;   // the layers' rows alone
    struct Group {
        QString name;
        int parent = -1;
    };
    QVector<Group> groups;
};

void resolve(const QVector<LayerEntry>& entries, const LayerEntry* parent, const LayerSource& parentSource, bool parentVisible,
             QVector<int> path, Resolved* out)
{
    for (const LayerEntry& given : entries) {
        LayerEntry e = given;
        if (parent != nullptr) {
            // As KLayout combines them (read from what its layer list
            // says of a member): what the group sets over the member's
            // own - the outermost group's over all; brightness added;
            // transparent, crossed and marked where either is; valid
            // where both are; the wider frame.
            if (parent->frame.isValid()) e.frame = parent->frame;
            if (parent->fill.isValid()) e.fill = parent->fill;
            e.frameBrightness = std::clamp(e.frameBrightness + parent->frameBrightness, -255, 255);
            e.fillBrightness = std::clamp(e.fillBrightness + parent->fillBrightness, -255, 255);
            if (!parent->dither.isEmpty()) e.dither = parent->dither;
            if (!parent->line.isEmpty()) e.line = parent->line;
            const auto either = [](std::optional<bool> a, std::optional<bool> b) -> std::optional<bool> {
                if (!a && !b) return std::nullopt;
                return a.value_or(false) || b.value_or(false);
            };
            e.transparent = either(e.transparent, parent->transparent);
            e.xfill = either(e.xfill, parent->xfill);
            e.marked = either(e.marked, parent->marked);
            if (e.valid || parent->valid) e.valid = e.valid.value_or(true) && parent->valid.value_or(true);
            if (parent->width) e.width = e.width ? std::max(*e.width, *parent->width) : *parent->width;
            if (parent->animation.value_or(0) != 0) e.animation = parent->animation;
        }
        const LayerSource source = parent != nullptr ? withinGroup(parseLayerSource(e.source), parentSource) : parseLayerSource(e.source);
        const bool visible = parentVisible && e.visible.value_or(true);
        const int place = out->rows++;
        if (e.group) {
            Resolved::Group g;
            g.name = !e.name.isEmpty() ? e.name : e.source;
            g.parent = path.isEmpty() ? -1 : path.last();
            out->groups.append(g);
            QVector<int> inner = path;
            inner.append(int(out->groups.size()) - 1);
            resolve(e.members, &e, source, visible, inner, out);
            continue;
        }
        Leaf leaf;
        leaf.entry = e;
        leaf.source = source;
        leaf.visible = visible;
        leaf.path = path;
        leaf.place = place;
        leaf.nth = out->layers++;
        out->leaves.append(leaf);
        // A layer for every layer (no source, or */*): KLayout lists every
        // layer in its place instead - here, its row not counted.
        if (namesEverything(source)) {
            out->leaves.last().everything = true;
            --out->rows;
            --out->layers;
        }
    }
}

// A name from the source when the entry has none: "Metal1 1/0" -> Metal1.
QString nameOf(const LayerEntry& e)
{
    if (!e.name.isEmpty()) return e.name;
    QString rest = e.source.section(QLatin1Char('@'), 0, 0);
    static const QRegularExpression numbers(QStringLiteral("\\(?\\s*(\\*|\\d+)(\\s*-\\s*\\d+)?\\s*(/\\s*(\\*|\\d+)(\\s*-\\s*\\d+)?)?\\s*\\)?"));
    rest.remove(numbers);
    return rest.trimmed();
}

LayerStyle klayoutStyle(const Leaf& leaf, const LayerInfo& layer, const LayerProperties& lyp)
{
    const LayerEntry& e = leaf.entry;
    const int index = leaf.nth;
    LayerStyle s;
    s.klayout = true;
    s.order = leaf.place;
    // A colour not given: KLayout's default for the layer's number; a
    // pattern, for its place among the list's layers.
    const QColor otherwise = defaultColour(layer.key.layer);
    s.frame = brightened(e.frame.isValid() ? e.frame : otherwise, e.frameBrightness);
    s.fill = brightened(e.fill.isValid() ? e.fill : otherwise, e.fillBrightness);
    s.stipple = stippleNamed(e.dither, lyp, index);
    s.pattern = s.stipple->isSolid()   ? Qt::SolidPattern
                : s.stipple->isEmpty() ? Qt::NoBrush
                                       : patternOf(e.dither.isEmpty() ? QStringLiteral("I%1").arg(defaultStipple(index)) : e.dither);
    s.line = lineNamed(e.line, lyp);
    s.width = std::max(0, e.width.value_or(1));   // (0: no frame, as KLayout draws it)
    s.transparent = e.transparent.value_or(false);
    s.xfill = e.xfill.value_or(false);
    s.marked = e.marked.value_or(false);
    s.valid = e.valid.value_or(true);
    s.animation = e.animation.value_or(0);
    s.visible = leaf.visible;
    s.name = nameOf(e);
    return s;
}

// The viewer's own look of an entry: its colours (one for the other when
// it gives one), names and visibility on the palette's patterns.
LayerStyle viewerStyle(const Leaf& leaf, int layer)
{
    const LayerEntry& e = leaf.entry;
    LayerStyle s = paletteStyle(layer);
    QColor frame = e.frame, fill = e.fill;
    if (!fill.isValid()) fill = frame;
    if (!frame.isValid()) frame = fill;
    if (frame.isValid()) {
        s.frame = frame;
        s.fill = fill;
    }
    s.pattern = patternOf(e.dither);
    s.visible = leaf.visible;
    s.name = nameOf(e);
    return s;
}

} // namespace

// ----------------------------------------------------------------------

bool Stipple::at(int x, int y) const
{
    const int w = std::clamp(width, 1, 32), h = std::clamp(height, 1, 32);
    return ((rows[std::size_t(wrap(y, h))] >> wrap(x, w)) & 1u) != 0;
}

bool Stipple::isSolid() const
{
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            if (!at(x, y)) return false;
    return true;
}

bool Stipple::isEmpty() const
{
    for (int y = 0; y < height; ++y)
        if ((rows[std::size_t(y)] & (width >= 32 ? 0xffffffffu : (1u << width) - 1)) != 0) return false;
    return true;
}

int standardStippleCount()
{
    return int(std::size(kStipples));
}

Stipple standardStipple(int index)
{
    const Rule& r = kStipples[index >= 0 && index < standardStippleCount() ? index : 0];
    Stipple s = stippleFrom(QString::fromLatin1(r.rows).split(QLatin1Char(' ')), QCoreApplication::translate("Layout", r.name));
    return s;
}

int standardLineStyleCount()
{
    return int(std::size(kLines));
}

LineStyle standardLineStyle(int index)
{
    if (index < 0 || index >= standardLineStyleCount()) return LineStyle();
    return lineFrom(QString::fromLatin1(kLines[index].second), QCoreApplication::translate("Layout", kLines[index].first));
}

QColor brightened(const QColor& colour, int brightness)
{
    if (brightness == 0 || !colour.isValid()) return colour;
    // (The factor in 256ths, as KLayout's pixels have it: each of 144
    // channels it drew at brightnesses from -255 to 255 so, none other.)
    const int m = int(std::lround(256 * std::exp2(-std::abs(brightness) / 128.0)));
    const auto channel = [brightness, m](int v) { return brightness < 0 ? (v * m) >> 8 : 255 - (((255 - v) * m) >> 8); };
    const QColor c = colour.toRgb();
    return QColor(channel(c.red()), channel(c.green()), channel(c.blue()), c.alpha());
}

int LayerProperties::leafCount() const
{
    const std::function<int(const QVector<LayerEntry>&)> count = [&count](const QVector<LayerEntry>& entries) {
        int n = 0;
        for (const LayerEntry& e : entries) n += e.group ? count(e.members) : 1;
        return n;
    };
    return count(entries);
}

LayerProperties readLayerPropertiesFile(const QString& path, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = f.errorString();
        return {};
    }
    LayerProperties lyp = parse(&f, error);
    if (!lyp.isEmpty()) lyp.file = path;
    return lyp;
}

LayerProperties parseLayerProperties(const QByteArray& text, QString* error)
{
    QBuffer buffer;
    buffer.setData(text);
    buffer.open(QIODevice::ReadOnly);
    return parse(&buffer, error);
}

LayerSource parseLayerSource(const QString& text)
{
    LayerSource s;
    QString rest = text.trimmed();
    // @n: the layout (@*: any).
    static const QRegularExpression at(QStringLiteral("@\\s*(\\*|\\d+)"));
    if (const QRegularExpressionMatch m = at.match(rest); m.hasMatch()) {
        s.layout = m.captured(1) == QLatin1String("*") ? 0 : m.captured(1).toInt();
        rest.remove(m.capturedStart(), m.capturedLength());
    }
    // What KLayout's sources say beyond that - #levels, %properties,
    // [selections], (transformations) - is not a layer's.
    static const QRegularExpression extras(QStringLiteral("#\\S*|%\\S*|\\[[^\\]]*\\]|\\((?![\\s\\d*/-]*\\))[^)]*\\)"));
    rest.remove(extras);
    const auto range = [](const QString& t, int* from, int* to) {
        if (t.trimmed() == QLatin1String("*")) {
            *from = *to = -1;
            return;
        }
        // (a-b: a - KLayout's sources have no ranges; it reads the first
        // number and takes the rest for a name)
        *from = *to = t.section(QLatin1Char('-'), 0, 0).trimmed().toInt();
    };
    static const QRegularExpression pair(QStringLiteral("(\\*|\\d+(?:\\s*-\\s*\\d+)?)\\s*/\\s*(\\*|\\d+(?:\\s*-\\s*\\d+)?)"));
    static const QRegularExpression single(QStringLiteral("(?:^|[\\s(])(\\*|\\d+)(?=$|[\\s)])"));
    if (const QRegularExpressionMatch m = pair.match(rest); m.hasMatch()) {
        s.numbered = true;
        range(m.captured(1), &s.layerFrom, &s.layerTo);
        range(m.captured(2), &s.datatypeFrom, &s.datatypeTo);
        rest.remove(m.capturedStart(), m.capturedLength());
    } else if (const QRegularExpressionMatch m1 = single.match(rest); m1.hasMatch()) {
        s.numbered = true;
        range(m1.captured(1), &s.layerFrom, &s.layerTo);
        s.datatypeFrom = s.datatypeTo = m1.captured(1) == QLatin1String("*") ? -1 : 0;
        rest.remove(m1.capturedStart(1), m1.capturedLength(1));
    }
    rest.remove(QRegularExpression(QStringLiteral("[()]")));
    rest = rest.trimmed();
    if (rest.size() >= 2 && (rest.startsWith(QLatin1Char('\'')) || rest.startsWith(QLatin1Char('"')))) rest = rest.mid(1, rest.size() - 2);
    s.name = rest;
    return s;
}

LayerSource withinGroup(const LayerSource& member, const LayerSource& group)
{
    LayerSource s = member;
    if (!member.numbered && member.name.isEmpty()) {
        s.numbered = group.numbered;
        s.layerFrom = group.layerFrom;
        s.layerTo = group.layerTo;
        s.datatypeFrom = group.datatypeFrom;
        s.datatypeTo = group.datatypeTo;
        s.name = group.name;
    } else if (member.numbered && group.numbered) {
        // Each number the member leaves open (*) the group's: */5 in a
        // group of 1/* is 1/5, as KLayout has it.
        if (member.layerFrom < 0) {
            s.layerFrom = group.layerFrom;
            s.layerTo = group.layerTo;
        }
        if (member.datatypeFrom < 0) {
            s.datatypeFrom = group.datatypeFrom;
            s.datatypeTo = group.datatypeTo;
        }
    }
    if (member.layout == 0) s.layout = group.layout;
    return s;
}

bool sourceMatches(const LayerSource& source, const LayerInfo& info)
{
    if (source.layout > 1) return false;
    if (source.numbered) {
        const auto in = [](quint32 v, int from, int to) { return from < 0 || (qint64(v) >= from && qint64(v) <= to); };
        return in(info.key.layer, source.layerFrom, source.layerTo) && in(info.key.datatype, source.datatypeFrom, source.datatypeTo);
    }
    return !source.name.isEmpty() && source.name == info.name;
}

LayerPlan planLayers(const Layout& layout, const LayerProperties& lyp, bool klayoutLook)
{
    LayerPlan plan;
    klayoutLook = klayoutLook && !lyp.isEmpty();   // (no .lyp: the palette's, as ever)
    const int n = int(layout.layers.size());
    plan.styles.resize(n);
    Resolved r;
    resolve(lyp.entries, nullptr, LayerSource(), true, {}, &r);
    // Each layer's entries: as KLayout draws them, every one naming it
    // alone (not a wildcard's, not */*), each in its place - the last over
    // the others, the layer's own row; the viewer's own look, the first
    // naming it, wildcards too, as before.
    QVector<int> leafOf(n, -1);
    QVector<QVector<int>> leavesOf(n);
    for (int i = 0; i < n; ++i) {
        const LayerInfo& info = layout.layers.at(i);
        for (int k = 0; k < r.leaves.size(); ++k) {
            const Leaf& leaf = r.leaves.at(k);
            if (klayoutLook && (leaf.everything || !bindsOne(leaf.source))) continue;
            if (!sourceMatches(leaf.source, info)) continue;
            leafOf[i] = k;
            leavesOf[i].append(k);
            if (!klayoutLook) break;
        }
    }
    int unnamed = 0;
    for (int i = 0; i < n; ++i) {
        const int k = leafOf.at(i);
        LayerStyle s;
        if (k < 0) {
            s = paletteStyle(i);
            if (klayoutLook) {
                // A layer the file names none of: after its rows, in order
                // of layer and datatype, as KLayout adds one - its colour
                // KLayout's default for its number, its pattern for its
                // place among the list's layers.
                const int nth = r.layers + unnamed;
                s.klayout = true;
                s.frame = s.fill = defaultColour(layout.layers.at(i).key.layer);
                s.stipple = sharedStipple(defaultStipple(nth));
                s.pattern = nth % 2 == 0 ? Qt::BDiagPattern : Qt::FDiagPattern;
                s.order = r.rows + unnamed++;
            }
        } else if (!klayoutLook) {
            s = viewerStyle(r.leaves.at(k), i);
        } else {
            s = klayoutStyle(r.leaves.at(k), layout.layers.at(i), lyp);
            if (leavesOf.at(i).size() > 1) {
                // Named more than once: each entry drawn, the layer shown
                // where any is (its row hides or shows them all).
                auto passes = std::make_shared<std::vector<LayerStyle>>();
                for (int each : std::as_const(leavesOf.at(i))) {
                    passes->push_back(klayoutStyle(r.leaves.at(each), layout.layers.at(i), lyp));
                    s.visible = s.visible || passes->back().visible;
                }
                s.passes = passes;
            }
        }
        if (s.name.isEmpty()) s.name = layout.layers.at(i).name;
        plan.styles[i] = s;
    }
    if (!klayoutLook) return plan;
    // The list: the file's groups and layers in its order, each layer of
    // the layout under the entry it is drawn by; then the layers it names
    // none of.
    QVector<LayerPlan::Row> groupRows(r.groups.size());
    for (int g = 0; g < r.groups.size(); ++g) groupRows[g].name = r.groups.at(g).name;
    // (Rows built bottom up, in the file's order: a group placed where its
    // first layer shown comes.)
    struct Slot {
        int group;   // -1: a layer
        int layer;
    };
    QVector<QVector<Slot>> inGroup(r.groups.size());
    QVector<Slot> top;
    QVector<bool> placed(r.groups.size(), false);
    for (int k = 0; k < r.leaves.size(); ++k) {
        for (int i = 0; i < n; ++i) {
            if (leafOf.at(i) != k) continue;
            const QVector<int>& path = r.leaves.at(k).path;
            // Each group on its way, placed in its parent once.
            for (int d = 0; d < path.size(); ++d) {
                const int g = path.at(d);
                if (placed.at(g)) continue;
                placed[g] = true;
                (d == 0 ? top : inGroup[path.at(d - 1)]).append({g, -1});
            }
            (path.isEmpty() ? top : inGroup[path.last()]).append({-1, i});
        }
    }
    const std::function<LayerPlan::Row(const Slot&)> build = [&](const Slot& slot) {
        LayerPlan::Row row;
        if (slot.group < 0) {
            row.layer = slot.layer;
            row.name = plan.styles.at(slot.layer).name;
            return row;
        }
        row.name = groupRows.at(slot.group).name;
        if (row.name.isEmpty()) row.name = tr("Group");
        for (const Slot& s : std::as_const(inGroup[slot.group])) row.children.append(build(s));
        return row;
    };
    for (const Slot& s : std::as_const(top)) plan.rows.append(build(s));
    for (int i = 0; i < n; ++i)
        if (leafOf.at(i) < 0) {
            LayerPlan::Row row;
            row.layer = i;
            row.name = plan.styles.at(i).name;
            plan.rows.append(row);
        }
    return plan;
}

QVector<LayerStyle> stylesFor(const Layout& layout, const LayerProperties& lyp, bool klayoutLook)
{
    return planLayers(layout, lyp, klayoutLook).styles;
}

QHash<LayerKey, LayerStyle> readLayerProperties(const QString& path, QString* error)
{
    QHash<LayerKey, LayerStyle> result;
    const LayerProperties lyp = readLayerPropertiesFile(path, error);
    if (lyp.isEmpty()) return result;
    Resolved r;
    resolve(lyp.entries, nullptr, LayerSource(), true, {}, &r);
    // Each layer a source names one of, by its number: the first entry's
    // (wildcards and ranges name none here).
    for (const Leaf& leaf : std::as_const(r.leaves)) {
        const LayerSource& s = leaf.source;
        if (!s.numbered || s.layerFrom < 0 || s.layerFrom != s.layerTo || s.datatypeFrom < 0 || s.datatypeFrom != s.datatypeTo) continue;
        const LayerKey key{quint32(s.layerFrom), quint32(s.datatypeFrom)};
        if (result.contains(key)) continue;
        LayerStyle style = viewerStyle(leaf, 0);
        if (!leaf.entry.frame.isValid() && !leaf.entry.fill.isValid()) continue;   // (no colour: the palette's)
        result.insert(key, style);
    }
    if (result.isEmpty() && error != nullptr) *error = tr("It has no layers.");
    return result;
}

int stipplePixels(qreal ratio)
{
    return std::max(1, int(std::lround(ratio)));
}

int stippleOffset(const LayerStyle& style)
{
    return style.klayout && style.order > 0 ? style.order % 32 : 0;
}

bool mixesByAnd(const QColor& canvas)
{
    return !canvas.isValid() || qGray(canvas.rgb()) >= 128;
}

QVector<int> drawingOrder(const QVector<LayerStyle>& styles, int layers)
{
    QVector<int> order(layers);
    std::iota(order.begin(), order.end(), 0);
    const auto key = [&styles](int i) { return i < styles.size() && styles.at(i).order >= 0 ? styles.at(i).order : i; };
    std::stable_sort(order.begin(), order.end(), [&key](int a, int b) { return key(a) < key(b); });
    return order;
}

std::vector<DrawPass> drawingPasses(const QVector<LayerStyle>& styles, int layers)
{
    std::vector<std::pair<int, DrawPass>> all;
    for (int i = 0; i < layers; ++i) {
        const LayerStyle* s = i < styles.size() ? &styles.at(i) : nullptr;
        if (s != nullptr && s->passes) {
            for (const LayerStyle& p : *s->passes)
                if (p.visible) all.push_back({p.order >= 0 ? p.order : i, {i, &p}});
            continue;
        }
        all.push_back({s != nullptr && s->order >= 0 ? s->order : i, {i, s}});
    }
    std::stable_sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<DrawPass> out;
    out.reserve(all.size());
    for (const auto& [key, pass] : all) out.push_back(pass);
    return out;
}

bool shownAtTick(const LayerStyle& style, int tick)
{
    if (!style.klayout) return true;
    const bool even = (tick / 2) % 2 == 0;   // (a tick a quarter second: a second's blink)
    if (style.animation == 2) return even;
    if (style.animation == 3) return !even;
    return true;
}

} // namespace qucs_s::layout
