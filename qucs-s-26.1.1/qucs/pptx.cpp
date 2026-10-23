/*
 * pptx.cpp - PowerPoint presentations (.pptx) read and drawn
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * A presentation is a ZIP archive of XML parts (Office Open XML, ECMA-376):
 * ppt/presentation.xml lists the slides; each slide follows a layout, each
 * layout a master, the master a theme (its colours, fonts and line and
 * fill styles). A slide's shapes take from the layout's and the master's
 * placeholders of their kind what they do not say themselves - where they
 * are, their text's styles - and the master and the layout draw their own
 * shapes under the slide's. Everything is read here into what is drawn:
 * each element with its outline, fill, line, text and transform in points.
 */

#include "pptx.h"
#include "svgguard.h"
#include "zipfile.h"

#include <QAbstractTextDocumentLayout>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QHash>
#include <QLocale>
#include <QMap>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QSet>
#include <QSvgRenderer>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QUrl>
#include <QXmlStreamReader>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <limits>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace qucs_s::pptx {

namespace {

constexpr qreal kEmuPerPoint = 12700.0;
constexpr qreal kPi = 3.14159265358979323846;

qreal pt(double emu)
{
    return emu / kEmuPerPoint;
}

double number(const QString& s, double fallback = 0)
{
    bool ok = false;
    const double v = s.toDouble(&ok);
    return ok && std::isfinite(v) ? v : fallback;
}

// ---------------------------------------------------------------------------
// XML: a tree of elements by their local names (the prefixes - p:, a:, dsp:
// for a diagram's drawing - left aside), mc:AlternateContent replaced by
// what its fallback holds (a picture for an equation, a chart of a newer
// kind), as an older reader takes it.

struct Node {
    QString name;
    QXmlStreamAttributes attrs;
    QString text;
    std::vector<std::unique_ptr<Node>> kids;

    QString attr(QAnyStringView local, const QString& fallback = QString()) const
    {
        for (const QXmlStreamAttribute& a : attrs)
            if (a.namespaceUri().isEmpty() && QAnyStringView::equal(a.name(), local)) return a.value().toString();
        return fallback;
    }
    bool has(QAnyStringView local) const
    {
        for (const QXmlStreamAttribute& a : attrs)
            if (a.namespaceUri().isEmpty() && QAnyStringView::equal(a.name(), local)) return true;
        return false;
    }
    double num(QAnyStringView local, double fallback = 0) const { return has(local) ? number(attr(local), fallback) : fallback; }
    /// An attribute as a whole number, held within \a lo and \a hi before
    /// it is converted: converting a double past int's range is undefined
    /// (bug hunt 2026-10-23 B5), and no attribute of a deck needs more.
    int whole(QAnyStringView local, int fallback = 0, int lo = std::numeric_limits<int>::lowest(),
              int hi = std::numeric_limits<int>::max()) const
    {
        if (!has(local)) return fallback;
        return int(std::clamp(num(local, fallback), double(lo), double(hi)));
    }
    bool flag(QAnyStringView local, bool fallback = false) const
    {
        if (!has(local)) return fallback;
        const QString v = attr(local);
        return v == QLatin1String("1") || v == QLatin1String("true") || v == QLatin1String("on");
    }
    /// A relationship's id (r:id, r:embed, r:link, r:dm ...).
    QString rel(QAnyStringView local) const
    {
        for (const QXmlStreamAttribute& a : attrs)
            if (QAnyStringView::equal(a.name(), local) && a.namespaceUri().endsWith(QLatin1String("relationships"))) return a.value().toString();
        return QString();
    }
    const Node* child(QAnyStringView local) const
    {
        for (const auto& k : kids)
            if (QAnyStringView::equal(k->name, local)) return k.get();
        return nullptr;
    }
    QList<const Node*> children(QAnyStringView local) const
    {
        QList<const Node*> out;
        for (const auto& k : kids)
            if (QAnyStringView::equal(k->name, local)) out << k.get();
        return out;
    }
    const Node* at(std::initializer_list<const char*> path) const
    {
        const Node* n = this;
        for (const char* step : path) {
            if (n == nullptr) return nullptr;
            n = n->child(QLatin1String(step));
        }
        return n;
    }
    const Node* descendant(QAnyStringView local) const
    {
        for (const auto& k : kids) {
            if (QAnyStringView::equal(k->name, local)) return k.get();
            if (const Node* d = k->descendant(local)) return d;
        }
        return nullptr;
    }
    /// The text of all a:t below (a paragraph's, a title's).
    QString allText() const
    {
        if (name == QLatin1String("t") || name == QLatin1String("v")) return text;
        QString out;
        for (const auto& k : kids) out += k->allText();
        return out;
    }
};

// How deep a part's elements may nest. PowerPoint's own nest a few dozen
// deep (groups in groups, a table's cells' text); the tree is walked by
// recursion here (AlternateContent, groups, notes, ~Node), so a part
// nesting past this is not read (bug hunt 2026-10-23 A2: 50,000 levels in
// a 157 kB deck overflowed the stack).
constexpr std::size_t kMaxXmlDepth = 256;

std::unique_ptr<Node> parseXml(const QByteArray& bytes, bool* tooDeep = nullptr)
{
    QXmlStreamReader xml(bytes);
    xml.setNamespaceProcessing(true);
    auto root = std::make_unique<Node>();
    root->name = QStringLiteral("#document");
    std::vector<Node*> stack{root.get()};
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            if (stack.size() > kMaxXmlDepth) {
                if (tooDeep != nullptr) *tooDeep = true;
                return nullptr;
            }
            auto n = std::make_unique<Node>();
            n->name = xml.name().toString();
            n->attrs = xml.attributes();
            Node* raw = n.get();
            stack.back()->kids.push_back(std::move(n));
            stack.push_back(raw);
        } else if (xml.isEndElement()) {
            if (stack.size() > 1) stack.pop_back();
        } else if (xml.isCharacters()) {
            stack.back()->text += xml.text();
        }
    }
    if (xml.hasError() && root->kids.empty()) return nullptr;
    // mc:AlternateContent: its fallback's elements in its place (else its
    // first choice's).
    std::function<void(Node*)> alternate = [&alternate](Node* n) {
        for (std::size_t i = 0; i < n->kids.size(); ++i) {
            Node* k = n->kids[i].get();
            if (k->name == QLatin1String("AlternateContent")) {
                Node* pick = nullptr;
                for (const auto& c : k->kids)
                    if (c->name == QLatin1String("Fallback")) pick = c.get();
                if (pick == nullptr)
                    for (const auto& c : k->kids)
                        if (c->name == QLatin1String("Choice") && pick == nullptr) pick = c.get();
                std::vector<std::unique_ptr<Node>> taken;
                if (pick != nullptr) taken = std::move(pick->kids);
                n->kids.erase(n->kids.begin() + std::ptrdiff_t(i));
                for (std::size_t j = 0; j < taken.size(); ++j) n->kids.insert(n->kids.begin() + std::ptrdiff_t(i + j), std::move(taken[j]));
                --i;   // (what came in looked at in turn)
                continue;
            }
            alternate(k);
        }
    };
    alternate(root.get());
    return root;
}

// ---------------------------------------------------------------------------
// The package: its parts, and the relationships of each.

struct Rel {
    QString id, type, target;
    bool external = false;
};

class Package
{
public:
    QHash<QString, QByteArray> parts;

    bool has(const QString& part) const { return parts.contains(part); }
    QByteArray bytes(const QString& part) const { return parts.value(part); }

    /// \a part's XML, its root element (null when there is none).
    const Node* xml(const QString& part) const
    {
        auto it = a_xml.find(part);
        if (it == a_xml.end()) {
            bool tooDeep = false;
            std::unique_ptr<Node> doc = parts.contains(part) ? parseXml(parts.value(part), &tooDeep) : nullptr;
            if (tooDeep) a_tooDeep << part;
            it = a_xml.emplace(part, std::move(doc)).first;
        }
        const Node* doc = it->second.get();
        return doc != nullptr && !doc->kids.empty() ? doc->kids.front().get() : nullptr;
    }

    QList<Rel> rels(const QString& part) const
    {
        if (auto it = a_rels.constFind(part); it != a_rels.constEnd()) return *it;
        QList<Rel> out;
        const int slash = int(part.lastIndexOf(QLatin1Char('/')));
        const QString folder = slash < 0 ? QString() : part.left(slash);
        const QString relsPart = (folder.isEmpty() ? QString() : folder + QLatin1Char('/')) + QStringLiteral("_rels/")
                                 + part.mid(slash + 1) + QStringLiteral(".rels");
        if (const Node* root = xml(relsPart))
            for (const Node* r : root->children(u"Relationship")) {
                Rel rel;
                rel.id = r->attr(u"Id");
                rel.type = r->attr(u"Type");
                rel.external = r->attr(u"TargetMode") == QLatin1String("External");
                const QString target = r->attr(u"Target");
                rel.target = rel.external ? target : resolve(folder, target);
                out << rel;
            }
        a_rels.insert(part, out);
        return out;
    }
    std::optional<Rel> rel(const QString& part, const QString& id) const
    {
        if (id.isEmpty()) return std::nullopt;
        for (const Rel& r : rels(part))
            if (r.id == id) return r;
        return std::nullopt;
    }
    /// The first of \a part's relationships whose type ends in \a kind
    /// ("/slideLayout").
    std::optional<Rel> relOfType(const QString& part, QStringView kind) const
    {
        for (const Rel& r : rels(part))
            if (r.type.endsWith(kind)) return r;
        return std::nullopt;
    }

    /// The parts not read for nesting deeper than kMaxXmlDepth.
    const QStringList& tooDeep() const { return a_tooDeep; }

private:
    static QString resolve(const QString& folder, const QString& target)
    {
        QString t = QUrl::fromPercentEncoding(target.toUtf8());
        if (t.startsWith(QLatin1Char('/'))) return QDir::cleanPath(t.mid(1));
        return QDir::cleanPath(folder.isEmpty() ? t : folder + QLatin1Char('/') + t);
    }
    mutable std::map<QString, std::unique_ptr<Node>> a_xml;
    mutable QHash<QString, QList<Rel>> a_rels;
    mutable QStringList a_tooDeep;
};

// ---------------------------------------------------------------------------
// The theme, and colours.

struct Theme {
    QHash<QString, QColor> colors;
    QString major = QStringLiteral("Calibri Light"), minor = QStringLiteral("Calibri");
    QList<const Node*> fills, lines, bgFills, effects;
};

Theme readTheme(const Node* root)
{
    Theme t;
    // Office's own, where the theme says nothing.
    const std::pair<const char*, const char*> office[] = {{"dk1", "000000"},     {"lt1", "FFFFFF"},     {"dk2", "44546A"},
                                                          {"lt2", "E7E6E6"},     {"accent1", "4472C4"}, {"accent2", "ED7D31"},
                                                          {"accent3", "A5A5A5"}, {"accent4", "FFC000"}, {"accent5", "5B9BD5"},
                                                          {"accent6", "70AD47"}, {"hlink", "0563C1"},   {"folHlink", "954F72"}};
    for (const auto& [name, rgb] : office) t.colors.insert(QLatin1String(name), QColor(QLatin1Char('#') + QLatin1String(rgb)));
    if (root == nullptr) return t;
    const Node* elements = root->child(u"themeElements");
    if (elements == nullptr) return t;
    if (const Node* scheme = elements->child(u"clrScheme"))
        for (const auto& c : scheme->kids) {
            for (const auto& v : c->kids) {
                QString hex;
                if (v->name == QLatin1String("srgbClr")) hex = v->attr(u"val");
                else if (v->name == QLatin1String("sysClr")) hex = v->attr(u"lastClr", v->attr(u"val") == QLatin1String("window") ? QStringLiteral("FFFFFF") : QStringLiteral("000000"));
                if (hex.size() == 6) t.colors.insert(c->name, QColor(QLatin1Char('#') + hex));
            }
        }
    if (const Node* fonts = elements->child(u"fontScheme")) {
        if (const Node* latin = fonts->at({"majorFont", "latin"}); latin != nullptr && !latin->attr(u"typeface").isEmpty())
            t.major = latin->attr(u"typeface");
        if (const Node* latin = fonts->at({"minorFont", "latin"}); latin != nullptr && !latin->attr(u"typeface").isEmpty())
            t.minor = latin->attr(u"typeface");
    }
    if (const Node* fmt = elements->child(u"fmtScheme")) {
        if (const Node* l = fmt->child(u"fillStyleLst"))
            for (const auto& k : l->kids) t.fills << k.get();
        if (const Node* l = fmt->child(u"lnStyleLst"))
            for (const auto& k : l->kids) t.lines << k.get();
        if (const Node* l = fmt->child(u"bgFillStyleLst"))
            for (const auto& k : l->kids) t.bgFills << k.get();
        if (const Node* l = fmt->child(u"effectStyleLst"))
            for (const auto& k : l->kids) t.effects << k.get();
    }
    return t;
}

qreal toLinear(qreal c)
{
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}
qreal fromLinear(qreal c)
{
    c = std::clamp(c, 0.0, 1.0);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1 / 2.4) - 0.055;
}

/// \a c towards white (\a tint) or black (\a shade) by \a v, in linear light
/// as Office mixes.
QColor mixed(const QColor& c, qreal v, bool tint)
{
    const auto mix = [&](qreal ch) {
        const qreal lin = toLinear(ch);
        return fromLinear(tint ? lin * v + (1 - v) : lin * v);
    };
    return QColor::fromRgbF(float(mix(c.redF())), float(mix(c.greenF())), float(mix(c.blueF())), c.alphaF());
}

/// The modifiers under a colour element (lumMod, tint, alpha ...) applied
/// to \a c, in their order.
QColor modified(QColor c, const Node* clr)
{
    for (const auto& m : clr->kids) {
        const qreal v = m->num(u"val") / 100000.0;
        const QString& n = m->name;
        if (n == QLatin1String("alpha")) c.setAlphaF(std::clamp(v, 0.0, 1.0));
        else if (n == QLatin1String("alphaMod")) c.setAlphaF(std::clamp(c.alphaF() * v, 0.0, 1.0));
        else if (n == QLatin1String("alphaOff")) c.setAlphaF(std::clamp(c.alphaF() + v, 0.0, 1.0));
        else if (n == QLatin1String("lumMod") || n == QLatin1String("lumOff") || n == QLatin1String("satMod") || n == QLatin1String("satOff")
                 || n == QLatin1String("hueMod") || n == QLatin1String("hueOff")) {
            float h, s, l, a;
            c.getHslF(&h, &s, &l, &a);
            if (h < 0) h = 0;
            if (n == QLatin1String("lumMod")) l = float(l * v);
            else if (n == QLatin1String("lumOff")) l = float(l + v);
            else if (n == QLatin1String("satMod")) s = float(s * v);
            else if (n == QLatin1String("satOff")) s = float(s + v);
            else if (n == QLatin1String("hueMod")) h = float(std::fmod(h * v, 1.0));
            else h = float(std::fmod(h + m->num(u"val") / 21600000.0 + 1.0, 1.0));
            c = QColor::fromHslF(h, std::clamp(s, 0.0f, 1.0f), std::clamp(l, 0.0f, 1.0f), a);
        } else if (n == QLatin1String("tint") || n == QLatin1String("shade")) {
            c = mixed(c, v, n == QLatin1String("tint"));
        } else if (n == QLatin1String("comp")) {
            float h, s, l, a;
            c.getHslF(&h, &s, &l, &a);
            c = QColor::fromHslF(std::fmod(std::max(h, 0.0f) + 0.5f, 1.0f), s, l, a);
        } else if (n == QLatin1String("inv")) {
            c = QColor::fromRgbF(1 - c.redF(), 1 - c.greenF(), 1 - c.blueF(), c.alphaF());
        } else if (n == QLatin1String("gray")) {
            const int g = qGray(c.rgb());
            c = QColor(g, g, g, c.alpha());
        }
    }
    return c;
}

// ---------------------------------------------------------------------------
// Pictures: decoded once, when first drawn.

struct Media {
    QString name;
    QByteArray bytes;
    QString suffix;
    mutable bool decoded = false;
    mutable QImage image;
    mutable std::shared_ptr<QSvgRenderer> svg;
    mutable bool tooFar = false;   ///< an SVG whose <use> references ask for too much

    void decode() const
    {
        if (decoded) return;
        decoded = true;
        static const QSet<QString> raster{QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("jpe"),
                                          QStringLiteral("gif"), QStringLiteral("bmp"), QStringLiteral("tif"), QStringLiteral("tiff"),
                                          QStringLiteral("webp")};
        // By its bytes where its name says nothing known (image1.tmp).
        QString kind = suffix;
        if (!raster.contains(kind) && kind != QLatin1String("svg")) {
            if (bytes.startsWith("\x89PNG")) kind = QStringLiteral("png");
            else if (bytes.startsWith("\xFF\xD8\xFF")) kind = QStringLiteral("jpeg");
            else if (bytes.startsWith("GIF8")) kind = QStringLiteral("gif");
            else if (bytes.startsWith("BM")) kind = QStringLiteral("bmp");
            else if (bytes.startsWith("II*") || bytes.startsWith("MM\x00*")) kind = QStringLiteral("tiff");
            else if (bytes.left(512).contains("<svg")) kind = QStringLiteral("svg");
        }
        // QSvgRenderer and QImage::fromData both read by what the bytes
        // are, not by what the name says, and QtSvg expands every <use>
        // reference as it reads: an SVG asking for too many is not drawn,
        // whatever it is called.
        if (qucs_s::svg::expandsTooFar(bytes)) {
            tooFar = true;
            return;
        }
        if (kind == QLatin1String("svg")) {
            if (auto r = std::make_shared<QSvgRenderer>(bytes); r->isValid()) svg = r;
        } else if (raster.contains(kind)) {
            image = QImage::fromData(bytes);
        }
    }
    bool drawable() const
    {
        decode();
        return svg != nullptr || !image.isNull();
    }
    /// What it is, when it is not drawn ("an EMF picture").
    QString kind() const
    {
        if (suffix == QLatin1String("emf") || suffix == QLatin1String("emz") || bytes.mid(40, 4) == " EMF") return QStringLiteral("EMF");
        if (suffix == QLatin1String("wmf") || suffix == QLatin1String("wmz") || bytes.startsWith("\xD7\xCD\xC6\x9A")) return QStringLiteral("WMF");
        return suffix.toUpper();
    }
};

// ---------------------------------------------------------------------------
// Fills and lines.

struct Fill {
    enum Kind { Unset, None, Solid, Gradient, Picture };
    Kind kind = Unset;
    QColor color;
    QGradientStops stops;
    qreal angle = 0;   // degrees, clockwise from left to right
    bool radial = false;
    std::shared_ptr<const Media> media;
    bool tile = false;

    bool visible() const { return kind == Solid || kind == Gradient || kind == Picture; }
};

struct Arrow {
    QString type;   // none, triangle, stealth, arrow, oval, diamond
    int w = 1, len = 1;   // sm 0, med 1, lg 2
};

struct Stroke {
    enum Kind { Unset, None, Solid };
    Kind kind = Unset;
    QColor color = Qt::black;
    qreal width = 0.75;
    Qt::PenStyle style = Qt::SolidLine;
    QList<qreal> dashes;
    Qt::PenCapStyle cap = Qt::FlatCap;
    Qt::PenJoinStyle join = Qt::RoundJoin;
    Arrow head, tail;

    bool visible() const { return kind == Solid && width > 0; }
    QPen pen() const
    {
        QPen p(color, std::max(width, 0.25), style, cap, join);
        if (style == Qt::CustomDashLine) p.setDashPattern(dashes);
        return p;
    }
};

// ---------------------------------------------------------------------------
// Text: what a text body draws, its formats resolved.

struct TextRun {
    QString text;
    QTextCharFormat format;
    bool lineBreak = false;
};

struct TextPara {
    QTextBlockFormat block;
    QList<TextRun> runs;
    QTextCharFormat endFormat;
    QString bullet;
    QTextCharFormat bulletFormat;
    bool hanging = false;   // the bullet at marL + indent, the text at marL
    qreal marL = 0, indent = 0;
};

/// A device of 72 dots an inch: a font's points are its units, and a text's
/// layout comes out in points on any platform.
QPaintDevice* pointDevice()
{
    static QImage device = [] {
        QImage i(4, 4, QImage::Format_ARGB32_Premultiplied);
        i.setDotsPerMeterX(qRound(72 / 0.0254));
        i.setDotsPerMeterY(qRound(72 / 0.0254));
        return i;
    }();
    return &device;
}

struct TextFrame {
    qreal l = 7.2, t = 3.6, r = 7.2, b = 3.6;
    QString anchor = QStringLiteral("t");
    bool wrap = true;
    int vertical = 0;   // 90, 270: turned
    std::optional<QRectF> box;   // a diagram's own text box (txXfrm), in the element's local coordinates
    QList<TextPara> paras;

    bool empty() const
    {
        for (const TextPara& p : paras)
            for (const TextRun& r : p.runs)
                if (!r.text.trimmed().isEmpty()) return false;
        return true;
    }

    /// Its layout at \a width (points; < 0: as wide as it is).
    QTextDocument* document(qreal width) const
    {
        if (a_doc != nullptr && qFuzzyCompare(a_width + 1, width + 1)) return a_doc.get();
        a_doc = std::make_unique<QTextDocument>();
        a_width = width;
        QTextDocument* doc = a_doc.get();
        doc->documentLayout()->setPaintDevice(pointDevice());
        doc->setDocumentMargin(0);
        QTextOption option = doc->defaultTextOption();
        option.setWrapMode(wrap && width > 0 ? QTextOption::WrapAtWordBoundaryOrAnywhere : QTextOption::NoWrap);
        doc->setDefaultTextOption(option);
        // Nothing laid out until all of it is in: QTextDocument lays the
        // text out again at every insertion, so a paragraph of thousands of
        // runs (a listing with its colours) cost the square of them - 5,000
        // runs took 36 s a slide (bug hunt 2026-10-23 A5). Nothing here
        // undoes anything either.
        doc->setUndoRedoEnabled(false);
        QTextCursor c(doc);
        c.beginEditBlock();
        for (int i = 0; i < paras.size(); ++i) {
            const TextPara& p = paras.at(i);
            if (i == 0) {
                c.setBlockFormat(p.block);
                c.setBlockCharFormat(p.endFormat);
            } else {
                c.insertBlock(p.block, p.endFormat);
            }
            if (!p.hanging && !p.bullet.isEmpty()) c.insertText(p.bullet + QStringLiteral(" "), p.bulletFormat);
            for (const TextRun& r : p.runs) {
                if (r.lineBreak) c.insertText(QString(QChar::LineSeparator), r.format);
                else if (!r.text.isEmpty()) c.insertText(r.text, r.format);
            }
        }
        c.endEditBlock();
        doc->setTextWidth(wrap && width > 0 ? width : -1);
        return doc;
    }

    /// Its text's height in a box \a width wide (points), insets too.
    qreal height(qreal width) const
    {
        QTextDocument* doc = document(std::max(1.0, width - l - r));
        return doc->size().height() + t + b;
    }

    void paint(QPainter& p, QRectF frame) const
    {
        if (paras.isEmpty()) return;
        if (box) frame = *box;
        p.save();
        if (vertical != 0) {
            // Turned about its middle, laid out in the box turned.
            const QPointF mid = frame.center();
            p.translate(mid);
            p.rotate(vertical);
            frame = QRectF(-frame.height() / 2, -frame.width() / 2, frame.height(), frame.width());
        }
        const QRectF inner = frame.adjusted(l, t, -r, -b);
        QTextDocument* doc = document(inner.width());
        const qreal h = doc->size().height();
        qreal y = inner.top();
        if (anchor == QLatin1String("ctr")) y += (inner.height() - h) / 2;
        else if (anchor == QLatin1String("b")) y += inner.height() - h;
        qreal x = inner.left();
        if (!wrap || inner.width() <= 0) {
            // Not wrapped: placed in the box as its first paragraph's
            // alignment says.
            const qreal w = doc->idealWidth();
            const Qt::Alignment a = paras.first().block.alignment();
            if (a & Qt::AlignHCenter) x += (inner.width() - w) / 2;
            else if (a & Qt::AlignRight) x += inner.width() - w;
        }
        p.translate(x, y);
        QAbstractTextDocumentLayout::PaintContext context;
        context.palette.setColor(QPalette::Text, Qt::black);
        doc->documentLayout()->draw(&p, context);
        // Hanging bullets, at marL + indent on their paragraph's first line.
        QTextBlock block = doc->begin();
        for (int i = 0; i < paras.size() && block.isValid(); ++i, block = block.next()) {
            const TextPara& para = paras.at(i);
            if (!para.hanging || para.bullet.isEmpty() || block.layout() == nullptr || block.layout()->lineCount() == 0) continue;
            const QTextLine line = block.layout()->lineAt(0);
            const QPointF at(para.marL + para.indent, block.layout()->position().y() + line.y() + line.ascent());
            QFont f = para.bulletFormat.font();
            p.setFont(f);
            p.setPen(para.bulletFormat.foreground().color());
            p.drawText(at, para.bullet);
        }
        p.restore();
    }

    mutable std::unique_ptr<QTextDocument> a_doc;
    mutable qreal a_width = -2;
};

// ---------------------------------------------------------------------------
// Tables and charts, as drawn.

struct Cell {
    int row = 0, col = 0, rowSpan = 1, colSpan = 1;
    bool covered = false;   // merged into another
    Fill fill;
    Stroke left, right, top, bottom;
    std::shared_ptr<TextFrame> text;
};

struct Table {
    QList<qreal> cols, rows;   // points; the rows' least heights
    QList<Cell> cells;
};

struct ChartSeries {
    QString name;
    QList<double> x, y;
    QStringList cats;
    Fill fill;
    Stroke line;
    QString marker = QStringLiteral("auto");
    qreal markerSize = 5;
    Fill markerFill;
    Stroke markerLine;
    bool smooth = false;
    QHash<int, Fill> pointFills;
};

struct ChartAxis {
    QString id, crossAx, pos = QStringLiteral("b");
    bool deleted = false, category = false, grid = false, reversed = false;
    double logBase = 0;
    std::optional<double> min, max, major;
    QString format = QStringLiteral("General");
    QString title;
    Stroke line, gridLine;
    QColor textColor = QColor(0x59, 0x59, 0x59);
    qreal fontSize = 9;
    bool labels = true;
};

struct ChartPlot {
    QString kind;   // scatter, line, bar, area, pie, doughnut, radar
    QString grouping = QStringLiteral("standard");
    QString scatterStyle = QStringLiteral("lineMarker");
    bool horizontal = false, vary = false;
    int gap = 150, overlap = 0, hole = 50;
    qreal firstSlice = 0;
    QList<ChartSeries> series;
    QStringList axes;
};

struct Chart {
    QString title;
    bool hasTitle = false;
    qreal titleSize = 14;
    QColor titleColor = QColor(0x59, 0x59, 0x59);
    QList<ChartPlot> plots;
    QList<ChartAxis> axes;
    bool legend = false;
    QString legendPos = QStringLiteral("r");
    qreal legendSize = 10;
    QColor legendColor = QColor(0x59, 0x59, 0x59);
    qreal fontSize = 10;
    QColor textColor = QColor(0x59, 0x59, 0x59);
    QString family;
    int stretch = 100;   // a stand-in's, as wide as the font asked
    qreal lineFactor = 1;   // its lines' height against the font asked's
    Fill area, plotArea;
    Stroke border;
    QList<QColor> accents;
};

// ---------------------------------------------------------------------------
// What is drawn.

/// An outer shadow (a:outerShdw): the element's silhouette under it,
/// offset and blurred.
struct Shadow {
    bool on = false;
    QColor color;          // its alpha too
    qreal blur = 0;        // points
    qreal dist = 0;        // points
    qreal dir = 0;         // degrees, clockwise from the right
    qreal sx = 1, sy = 1;  // its size against the element's
    QString align = QStringLiteral("b");   // what stays put as it is scaled
    bool rotWithShape = true;
};

struct Element {
    enum Kind { Shape, Picture, Group, TableFrame, ChartFrame, Missing };
    Kind kind = Shape;
    QRectF rect;   // points, in its parent's coordinates
    qreal rotation = 0;
    bool flipH = false, flipV = false;
    QPainterPath path;   // its outline, in its own coordinates (0, 0, width, height)
    QPainterPath strokeOnly;   // stroked, not filled (a cloud's inner arcs)
    bool open = false;   // a line, an arc: stroked, not filled
    Fill fill;
    Stroke stroke;
    Shadow shadow;
    std::shared_ptr<TextFrame> text;
    std::shared_ptr<const Media> media;
    QRectF crop;   // a picture's part left out on each side: fractions (l, t, r, b)
    QRectF childSpace;   // a group's
    QList<Element> children;
    std::shared_ptr<Table> table;
    std::shared_ptr<Chart> chart;
    QString missing;   // what is in its place, said

    QTransform transform() const
    {
        const qreal w = rect.width(), h = rect.height();
        QTransform t;
        t.translate(rect.x() + w / 2, rect.y() + h / 2);
        if (rotation != 0) t.rotate(rotation);
        t.scale(flipH ? -1 : 1, flipV ? -1 : 1);
        t.translate(-w / 2, -h / 2);
        return t;
    }
};

struct Slide {
    Fill background;
    QList<Element> elements;
    SlideInfo info;
};

} // namespace

// ---------------------------------------------------------------------------

struct Presentation::Data {
    Package pkg;
    QSizeF size{720, 540};
    QString title;
    QList<Slide> slides;
    QHash<QString, std::shared_ptr<Media>> media;

    std::shared_ptr<const Media> mediaOf(const QString& part)
    {
        if (!pkg.has(part)) return nullptr;
        auto it = media.find(part);
        if (it == media.end()) {
            auto m = std::make_shared<Media>();
            m->name = part;
            m->bytes = pkg.bytes(part);
            m->suffix = QFileInfo(part).suffix().toLower();
            it = media.insert(part, m);
        }
        return *it;
    }
};

namespace {

using Data = Presentation::Data;

// ---------------------------------------------------------------------------
// Reading a slide: the context it is read in.

struct Placeholder {
    QString type, idx;
    const Node* sp = nullptr;
};

struct Sheet {
    QString part;
    const Node* root = nullptr;
    const Node* tree = nullptr;
    QList<Placeholder> placeholders;
};

Sheet readSheet(const Package& pkg, const QString& part)
{
    Sheet s;
    s.part = part;
    s.root = pkg.xml(part);
    if (s.root == nullptr) return s;
    s.tree = s.root->at({"cSld", "spTree"});
    std::function<void(const Node*)> collect = [&](const Node* tree) {
        if (tree == nullptr) return;
        for (const auto& k : tree->kids) {
            if (k->name == QLatin1String("grpSp")) {
                collect(k.get());
                continue;
            }
            const Node* nv = nullptr;
            for (const auto& c : k->kids)
                if (c->name.startsWith(QLatin1String("nv"))) nv = c.get();
            const Node* ph = nv != nullptr ? nv->at({"nvPr", "ph"}) : nullptr;
            if (ph != nullptr) s.placeholders << Placeholder{ph->attr(u"type", QStringLiteral("obj")), ph->attr(u"idx"), k.get()};
        }
    };
    collect(s.tree);
    return s;
}

QString masterType(const QString& type)
{
    if (type == QLatin1String("ctrTitle") || type == QLatin1String("title")) return QStringLiteral("title");
    if (type == QLatin1String("dt") || type == QLatin1String("ftr") || type == QLatin1String("sldNum") || type == QLatin1String("hdr"))
        return type;
    return QStringLiteral("body");
}

const Placeholder* findPlaceholder(const Sheet& sheet, const QString& type, const QString& idx, bool byIdx)
{
    if (byIdx && !idx.isEmpty())
        for (const Placeholder& p : sheet.placeholders)
            if (p.idx == idx) return &p;
    for (const Placeholder& p : sheet.placeholders)
        if (p.type == type) return &p;
    const QString kind = masterType(type);
    for (const Placeholder& p : sheet.placeholders)
        if (masterType(p.type) == kind && (kind != QLatin1String("body") || p.type == QLatin1String("body") || p.type == QLatin1String("obj")))
            return &p;
    return nullptr;
}

struct Ctx {
    Data* d = nullptr;
    const Theme* theme = nullptr;
    QHash<QString, QString> map;   // bg1 -> lt1 ...
    QString part;   // whose relationships pictures are named by
    const Node* txStyles = nullptr;   // the master's
    const Node* defaultTextStyle = nullptr;   // the presentation's
    Sheet master, layout;
    int number = 1;   // the slide's number
    bool counting = false;   // the slide's own shapes: their text and pictures counted
    SlideInfo* info = nullptr;
    QSet<QString> approximated;   // shapes of kinds drawn as rectangles
};

QColor schemeColor(const QString& name, const Ctx& c, const std::optional<QColor>& ph)
{
    if (name == QLatin1String("phClr")) return ph.value_or(QColor(Qt::black));
    QString n = c.map.value(name);
    if (n.isEmpty()) {
        if (name == QLatin1String("tx1")) n = QStringLiteral("dk1");
        else if (name == QLatin1String("bg1")) n = QStringLiteral("lt1");
        else if (name == QLatin1String("tx2")) n = QStringLiteral("dk2");
        else if (name == QLatin1String("bg2")) n = QStringLiteral("lt2");
        else n = name;
    }
    return c.theme->colors.value(n, QColor(Qt::black));
}

/// The colour \a holder holds (its srgbClr, schemeClr ... child), its
/// modifiers applied; none when it holds none.
std::optional<QColor> colorOf(const Node* holder, const Ctx& c, const std::optional<QColor>& ph = std::nullopt)
{
    if (holder == nullptr) return std::nullopt;
    for (const auto& k : holder->kids) {
        QColor base;
        const QString& n = k->name;
        if (n == QLatin1String("srgbClr")) base = QColor(QLatin1Char('#') + k->attr(u"val"));
        else if (n == QLatin1String("schemeClr")) base = schemeColor(k->attr(u"val"), c, ph);
        else if (n == QLatin1String("sysClr"))
            base = QColor(QLatin1Char('#') + k->attr(u"lastClr", k->attr(u"val") == QLatin1String("window") ? QStringLiteral("FFFFFF") : QStringLiteral("000000")));
        else if (n == QLatin1String("prstClr")) base = QColor(k->attr(u"val").toLower());
        else if (n == QLatin1String("scrgbClr"))
            base = QColor::fromRgbF(float(k->num(u"r") / 100000), float(k->num(u"g") / 100000), float(k->num(u"b") / 100000));
        else if (n == QLatin1String("hslClr"))
            base = QColor::fromHslF(float(std::fmod(k->num(u"hue") / 21600000.0, 1.0)), float(std::clamp(k->num(u"sat") / 100000, 0.0, 1.0)),
                                    float(std::clamp(k->num(u"lum") / 100000, 0.0, 1.0)));
        else continue;
        if (!base.isValid()) base = Qt::black;
        return modified(base, k.get());
    }
    return std::nullopt;
}

/// A fill element (solidFill, gradFill, blipFill ...) read; \a known false
/// for an element that is no fill.
Fill fillElement(const Node* k, const Ctx& c, const std::optional<QColor>& ph, bool* known = nullptr)
{
    Fill f;
    if (known != nullptr) *known = true;
    const QString& n = k->name;
    if (n == QLatin1String("noFill")) {
        f.kind = Fill::None;
    } else if (n == QLatin1String("solidFill")) {
        f.kind = Fill::Solid;
        f.color = colorOf(k, c, ph).value_or(ph.value_or(QColor(Qt::black)));
    } else if (n == QLatin1String("gradFill")) {
        f.kind = Fill::Gradient;
        if (const Node* list = k->child(u"gsLst"))
            for (const Node* gs : list->children(u"gs"))
                f.stops << QGradientStop(std::clamp(gs->num(u"pos") / 100000, 0.0, 1.0), colorOf(gs, c, ph).value_or(QColor(Qt::black)));
        std::sort(f.stops.begin(), f.stops.end(), [](const QGradientStop& a, const QGradientStop& b) { return a.first < b.first; });
        if (f.stops.isEmpty()) f.kind = Fill::None;
        if (const Node* lin = k->child(u"lin")) f.angle = lin->num(u"ang") / 60000.0;
        else if (k->child(u"path") != nullptr) f.radial = true;
        else f.angle = 90;
    } else if (n == QLatin1String("pattFill")) {
        // Between its colours (a pattern's dots, at a distance).
        const QColor fore = colorOf(k->child(u"fgClr"), c, ph).value_or(QColor(Qt::black));
        const QColor back = colorOf(k->child(u"bgClr"), c, ph).value_or(QColor(Qt::white));
        f.kind = Fill::Solid;
        f.color = QColor((fore.red() + back.red()) / 2, (fore.green() + back.green()) / 2, (fore.blue() + back.blue()) / 2);
    } else if (n == QLatin1String("blipFill")) {
        const Node* blip = k->child(u"blip");
        const auto r = blip != nullptr ? c.d->pkg.rel(c.part, blip->rel(u"embed")) : std::nullopt;
        if (r && !r->external) {
            f.kind = Fill::Picture;
            f.media = c.d->mediaOf(r->target);
            f.tile = k->child(u"tile") != nullptr;
        }
        if (f.media == nullptr) f.kind = Fill::None;
    } else if (n == QLatin1String("grpFill")) {
        // (A group's fill: none of its own.)
    } else if (known != nullptr) {
        *known = false;
    }
    return f;
}

/// The fill \a holder (spPr, bgPr, tcPr, rPr ...) holds; Unset when none.
Fill fillOf(const Node* holder, const Ctx& c, const std::optional<QColor>& ph = std::nullopt)
{
    if (holder == nullptr) return Fill();
    for (const auto& k : holder->kids) {
        bool known = false;
        const Fill f = fillElement(k.get(), c, ph, &known);
        if (known) return f;
    }
    return Fill();
}

int arrowSize(const QString& s)
{
    return s == QLatin1String("sm") ? 0 : s == QLatin1String("lg") ? 2 : 1;
}

/// What \a ln says, over \a s: its width, its colour (or none), dashes,
/// ends.
void applyLine(Stroke& s, const Node* ln, const Ctx& c, const std::optional<QColor>& ph = std::nullopt)
{
    if (ln == nullptr) return;
    if (ln->has(u"w")) s.width = pt(ln->num(u"w"));
    const QString cap = ln->attr(u"cap");
    if (cap == QLatin1String("rnd")) s.cap = Qt::RoundCap;
    else if (cap == QLatin1String("sq")) s.cap = Qt::SquareCap;
    else if (!cap.isEmpty()) s.cap = Qt::FlatCap;
    for (const auto& k : ln->kids) {
        const QString& n = k->name;
        if (n == QLatin1String("noFill")) s.kind = Stroke::None;
        else if (n == QLatin1String("solidFill")) {
            s.kind = Stroke::Solid;
            s.color = colorOf(k.get(), c, ph).value_or(QColor(Qt::black));
        } else if (n == QLatin1String("gradFill")) {
            const Fill f = fillElement(k.get(), c, ph);
            s.kind = Stroke::Solid;
            s.color = f.stops.isEmpty() ? QColor(Qt::black) : f.stops.first().second;
        } else if (n == QLatin1String("prstDash")) {
            const QString v = k->attr(u"val");
            const std::pair<const char*, QList<qreal>> dashes[] = {{"dash", {4, 3}},        {"sysDash", {3, 1}},     {"dot", {1, 3}},
                                                                   {"sysDot", {1, 1}},      {"lgDash", {8, 3}},      {"dashDot", {4, 3, 1, 3}},
                                                                   {"lgDashDot", {8, 3, 1, 3}}, {"lgDashDotDot", {8, 3, 1, 3, 1, 3}},
                                                                   {"sysDashDot", {3, 1, 1, 1}}, {"sysDashDotDot", {3, 1, 1, 1, 1, 1}}};
            s.style = Qt::SolidLine;
            for (const auto& [name, pattern] : dashes)
                if (v == QLatin1String(name)) {
                    s.style = Qt::CustomDashLine;
                    s.dashes = pattern;
                }
        } else if (n == QLatin1String("round")) s.join = Qt::RoundJoin;
        else if (n == QLatin1String("bevel")) s.join = Qt::BevelJoin;
        else if (n == QLatin1String("miter")) s.join = Qt::MiterJoin;
        else if (n == QLatin1String("headEnd") || n == QLatin1String("tailEnd")) {
            Arrow& a = n == QLatin1String("headEnd") ? s.head : s.tail;
            a.type = k->attr(u"type", QStringLiteral("none"));
            a.w = arrowSize(k->attr(u"w"));
            a.len = arrowSize(k->attr(u"len"));
        }
    }
}

/// A shape's p:style: its line, fill and text colour and font by the
/// theme's lists.
struct StyleRefs {
    Stroke line;
    Fill fill;
    std::optional<QColor> fontColor;
    QString font;
    const Node* effects = nullptr;   // the theme's effect style's effectLst
    std::optional<QColor> effectColor;
};

StyleRefs styleRefs(const Node* style, const Ctx& c)
{
    StyleRefs r;
    if (style == nullptr) return r;
    if (const Node* ln = style->child(u"lnRef")) {
        const int i = ln->whole(u"idx");
        const std::optional<QColor> ph = colorOf(ln, c);
        if (i >= 1 && i <= c.theme->lines.size()) {
            r.line.kind = Stroke::Solid;
            applyLine(r.line, c.theme->lines.at(i - 1), c, ph);
        }
    }
    if (const Node* fl = style->child(u"fillRef")) {
        const int i = fl->whole(u"idx");
        const std::optional<QColor> ph = colorOf(fl, c);
        if (i == 0) r.fill.kind = Fill::None;
        else if (i >= 1001 && i - 1001 < c.theme->bgFills.size()) r.fill = fillElement(c.theme->bgFills.at(i - 1001), c, ph);
        else if (i >= 1 && i <= c.theme->fills.size()) r.fill = fillElement(c.theme->fills.at(i - 1), c, ph);
    }
    if (const Node* font = style->child(u"fontRef")) {
        r.fontColor = colorOf(font, c);
        r.font = font->attr(u"idx") == QLatin1String("major") ? c.theme->major : c.theme->minor;
    }
    if (const Node* ef = style->child(u"effectRef")) {
        const int i = ef->whole(u"idx");
        if (i >= 1 && i <= c.theme->effects.size()) r.effects = c.theme->effects.at(i - 1)->child(u"effectLst");
        r.effectColor = colorOf(ef, c);
    }
    return r;
}

/// The outer shadow an effect list holds (none: off).
Shadow shadowOf(const Node* effectLst, const Ctx& c, const std::optional<QColor>& ph = std::nullopt)
{
    Shadow s;
    const Node* o = effectLst != nullptr ? effectLst->child(u"outerShdw") : nullptr;
    if (o == nullptr) return s;
    s.on = true;
    s.color = colorOf(o, c, ph).value_or(QColor(0, 0, 0, 128));
    s.blur = pt(o->num(u"blurRad"));
    s.dist = pt(o->num(u"dist"));
    s.dir = o->num(u"dir") / 60000;
    s.sx = o->num(u"sx", 100000) / 100000;
    s.sy = o->num(u"sy", 100000) / 100000;
    s.align = o->attr(u"algn", QStringLiteral("b"));
    s.rotWithShape = o->flag(u"rotWithShape", true);
    return s;
}

// ---------------------------------------------------------------------------
// Outlines: the preset shapes (presetShapeDefinitions.xml's, those decks use
// most, their adjustments read) and a custom geometry's paths.

struct Outline {
    QPainterPath path;
    QPainterPath strokeOnly;   // stroked, not filled: a cloud's inner arcs
    bool open = false;
    std::optional<QRectF> text;   // where its text goes, where not its whole box
};

QPainterPath polygon(std::initializer_list<QPointF> points)
{
    QPainterPath p;
    p.addPolygon(QPolygonF(points));
    p.closeSubpath();
    return p;
}

QPainterPath star(int points, qreal w, qreal h, qreal inner)
{
    QPolygonF poly;
    for (int i = 0; i < points * 2; ++i) {
        const qreal a = -kPi / 2 + i * kPi / points;
        const qreal r = i % 2 == 0 ? 1.0 : inner;
        poly << QPointF(w / 2 + std::cos(a) * w / 2 * r, h / 2 + std::sin(a) * h / 2 * r);
    }
    QPainterPath p;
    p.addPolygon(poly);
    p.closeSubpath();
    return p;
}

/// An arc as DrawingML's arcTo draws it, from \a p's current point: of the
/// ellipse of radii \a wr and \a hr (its path's units), from the angle \a st
/// sweeping \a sw (degrees, clockwise) - true angles on the ellipse, not
/// its parameter's, which QPainterPath::arcTo takes (a cloud's arcs close
/// so, and not otherwise). \a sx, \a sy: the path's units in points.
void ooxmlArc(QPainterPath& p, qreal wr, qreal hr, qreal st, qreal sw, qreal sx = 1, qreal sy = 1)
{
    const auto param = [wr, hr](qreal deg) {
        const qreal a = qDegreesToRadians(deg);
        return qRadiansToDegrees(std::atan2(wr * std::sin(a), hr * std::cos(a)));
    };
    const qreal t0 = param(st);
    // The end's parameter, the sweep's direction and turns kept.
    qreal dt = param(st + sw) - t0;
    while (dt - sw > 180) dt -= 360;
    while (dt - sw <= -180) dt += 360;
    const qreal rx = wr * sx, ry = hr * sy;
    const QPointF cur = p.currentPosition();
    const qreal r0 = qDegreesToRadians(t0);
    const QPointF centre(cur.x() - rx * std::cos(r0), cur.y() - ry * std::sin(r0));
    p.arcTo(QRectF(centre.x() - rx, centre.y() - ry, 2 * rx, 2 * ry), -t0, -dt);
}

/// The cloud of presetShapeDefinitions.xml in a box \a w by \a h: its
/// outline of eleven arcs, and the arcs inside it, stroked alone.
Outline cloud(qreal w, qreal h)
{
    Outline o;
    const qreal sx = w / 43200, sy = h / 43200;
    struct Arc {
        qreal wr, hr, st, sw;
    };
    static const Arc outline[] = {{6753, 9190, -11429249, 7426832}, {5333, 7267, -8646143, 5396714}, {4365, 5945, -8748475, 5983381},
                                  {4857, 6595, -7859164, 7034504},  {5333, 7273, -4722533, 6541615}, {6775, 9220, -2776035, 7816140},
                                  {5785, 7867, 37501, 6842000},     {6752, 9215, 1347096, 6910353},  {7720, 10543, 3974558, 4542661},
                                  {4360, 5918, -16496525, 8804134}, {4345, 5945, -14809710, 9151131}};
    o.path.moveTo(3900 * sx, 14370 * sy);
    for (const Arc& a : outline) ooxmlArc(o.path, a.wr, a.hr, a.st / 60000, a.sw / 60000, sx, sy);
    o.path.closeSubpath();
    o.text = QRectF(QPointF(w * 2977 / 21600, h * 3262 / 21600), QPointF(w * 17087 / 21600, h * 17337 / 21600));
    struct Inner {
        qreal x, y, wr, hr, st, sw;
    };
    static const Inner inner[] = {{4693, 26177, 4345, 5945, 5204520, 1585770},   {6928, 34899, 4360, 5918, 4416628, 686848},
                                  {16478, 39090, 6752, 9215, 8257449, 844866},   {28827, 34751, 6752, 9215, 387196, 959901},
                                  {34129, 22954, 5785, 7867, -4217541, 4255042}, {41798, 15354, 5333, 7273, 1819082, 1665090},
                                  {38324, 5426, 4857, 6595, -824660, 891534},    {29078, 3952, 4857, 6595, -8950887, 1091722},
                                  {22141, 4720, 4365, 5945, -9809656, 1061181},  {14000, 5192, 6753, 9190, -4002417, 739161},
                                  {4127, 15789, 6753, 9190, 9459261, 711490}};
    for (const Inner& a : inner) {
        o.strokeOnly.moveTo(a.x * sx, a.y * sy);
        ooxmlArc(o.strokeOnly, a.wr, a.hr, a.st / 60000, a.sw / 60000, sx, sy);
    }
    return o;
}

/// An arc of the ellipse in \a box from \a start sweeping \a sweep (degrees,
/// clockwise as Office measures), added to \a p.
void arcOf(QPainterPath& p, const QRectF& box, qreal start, qreal sweep, bool move)
{
    if (move) p.arcMoveTo(box, -start);
    p.arcTo(box, -start, -sweep);
}

Outline preset(const QString& prst, qreal w, qreal h, const QHash<QString, qreal>& av, bool* known)
{
    *known = true;
    const qreal ss = std::min(w, h);
    const auto a = [&av](const char* name, qreal fallback) { return av.value(QLatin1String(name), fallback); };
    Outline o;
    QPainterPath& p = o.path;
    const QString& s = prst;
    if (s == QLatin1String("rect") || s == QLatin1String("textNoShape") || s == QLatin1String("flowChartProcess")) {
        p.addRect(0, 0, w, h);
    } else if (s == QLatin1String("roundRect") || s == QLatin1String("flowChartAlternateProcess")) {
        const qreal r = ss * std::clamp(a("adj", 16667), 0.0, 50000.0) / 100000;
        p.addRoundedRect(QRectF(0, 0, w, h), r, r);
        const qreal in = r * 0.29289;   // (its text clear of the corners)
        o.text = QRectF(in, in, w - 2 * in, h - 2 * in);
    } else if (s == QLatin1String("flowChartTerminator")) {
        p.addRoundedRect(QRectF(0, 0, w, h), ss / 2, ss / 2);
    } else if (s == QLatin1String("ellipse") || s == QLatin1String("flowChartConnector") || s == QLatin1String("flowChartOr")
               || s == QLatin1String("flowChartSummingJunction")) {
        p.addEllipse(QRectF(0, 0, w, h));
        // (Its text in the square at 45 degrees.)
        o.text = QRectF(w * 0.14645, h * 0.14645, w * 0.7071, h * 0.7071);
    } else if (s == QLatin1String("cloud")) {
        o = cloud(w, h);
    } else if (s == QLatin1String("cloudCallout")) {
        // The cloud, and three bubbles from it towards the callout's point
        // (adj1, adj2: from the middle, in 1/100000 of the width and height).
        o = cloud(w, h);
        const qreal dx = w * a("adj1", -20833) / 100000, dy = h * a("adj2", 62500) / 100000;
        const QPointF tip(w / 2 + dx, h / 2 + dy);
        // Where the line from the middle to the point leaves the ellipse.
        const qreal t = std::atan2(w / 2 * dy, h / 2 * dx);
        const QPointF edge(w / 2 + w / 2 * std::cos(t), h / 2 + h / 2 * std::sin(t));
        const QPointF v = edge - tip;
        const qreal l = std::hypot(v.x(), v.y());
        const qreal g11 = (l - ss * 6600 / 21600) / 3;
        const qreal r1 = ss * 600 / 21600, r2 = ss * 1200 / 21600, r3 = ss * 1800 / 21600;
        const QPointF u = l > 0 ? v / l : QPointF(0, 0);
        QPainterPath bubbles;
        bubbles.setFillRule(Qt::WindingFill);   // (where they meet, filled)
        bubbles.addEllipse(tip, r1, r1);
        bubbles.addEllipse(tip + u * (g11 + r3), r2, r2);
        bubbles.addEllipse(tip + u * (ss * 4800 / 21600 + 2 * g11), r3, r3);
        o.path = o.path.united(bubbles);
    } else if (s == QLatin1String("line") || s == QLatin1String("straightConnector1")) {
        p.moveTo(0, 0);
        p.lineTo(w, h);
        o.open = true;
    } else if (s == QLatin1String("bentConnector2")) {
        p.moveTo(0, 0);
        p.lineTo(w, 0);
        p.lineTo(w, h);
        o.open = true;
    } else if (s.startsWith(QLatin1String("bentConnector"))) {
        const qreal x1 = w * a("adj1", 50000) / 100000;
        p.moveTo(0, 0);
        p.lineTo(x1, 0);
        p.lineTo(x1, h);
        p.lineTo(w, h);
        o.open = true;
    } else if (s == QLatin1String("curvedConnector2")) {
        p.moveTo(0, 0);
        p.quadTo(w, 0, w, h);
        o.open = true;
    } else if (s.startsWith(QLatin1String("curvedConnector"))) {
        const qreal x1 = w * a("adj1", 50000) / 100000;
        p.moveTo(0, 0);
        p.cubicTo(x1 / 2, 0, x1, h / 4, x1, h / 2);
        p.cubicTo(x1, 3 * h / 4, (x1 + w) / 2, h, w, h);
        o.open = true;
    } else if (s == QLatin1String("triangle") || s == QLatin1String("flowChartExtract")) {
        const qreal x = w * a("adj", 50000) / 100000;
        p = polygon({QPointF(x, 0), QPointF(w, h), QPointF(0, h)});
    } else if (s == QLatin1String("flowChartMerge")) {
        p = polygon({QPointF(0, 0), QPointF(w, 0), QPointF(w / 2, h)});
    } else if (s == QLatin1String("rtTriangle")) {
        p = polygon({QPointF(0, 0), QPointF(w, h), QPointF(0, h)});
    } else if (s == QLatin1String("diamond") || s == QLatin1String("flowChartDecision")) {
        p = polygon({QPointF(w / 2, 0), QPointF(w, h / 2), QPointF(w / 2, h), QPointF(0, h / 2)});
        o.text = QRectF(w / 4, h / 4, w / 2, h / 2);
    } else if (s == QLatin1String("parallelogram") || s == QLatin1String("flowChartInputOutput")) {
        const qreal x = s == QLatin1String("parallelogram") ? ss * a("adj", 25000) / 100000 : w / 5;
        p = polygon({QPointF(x, 0), QPointF(w, 0), QPointF(w - x, h), QPointF(0, h)});
    } else if (s == QLatin1String("trapezoid") || s == QLatin1String("flowChartManualOperation")) {
        const qreal x = s == QLatin1String("trapezoid") ? ss * a("adj", 25000) / 100000 : w / 5;
        if (s == QLatin1String("trapezoid")) p = polygon({QPointF(0, h), QPointF(x, 0), QPointF(w - x, 0), QPointF(w, h)});
        else p = polygon({QPointF(0, 0), QPointF(w, 0), QPointF(w - x, h), QPointF(x, h)});
    } else if (s == QLatin1String("pentagon")) {
        p = polygon({QPointF(w / 2, 0), QPointF(w, h * 0.382), QPointF(w * 0.809, h), QPointF(w * 0.191, h), QPointF(0, h * 0.382)});
    } else if (s == QLatin1String("hexagon") || s == QLatin1String("flowChartPreparation")) {
        const qreal x = s == QLatin1String("hexagon") ? ss * a("adj", 25000) / 100000 : w / 5;
        p = polygon({QPointF(x, 0), QPointF(w - x, 0), QPointF(w, h / 2), QPointF(w - x, h), QPointF(x, h), QPointF(0, h / 2)});
    } else if (s == QLatin1String("octagon")) {
        const qreal x = ss * a("adj", 29289) / 100000;
        p = polygon({QPointF(x, 0), QPointF(w - x, 0), QPointF(w, x), QPointF(w, h - x), QPointF(w - x, h), QPointF(x, h), QPointF(0, h - x),
                     QPointF(0, x)});
    } else if (s == QLatin1String("homePlate") || s == QLatin1String("flowChartOffpageConnector")) {
        const qreal x = w - ss * a("adj", 50000) / 100000;
        if (s == QLatin1String("homePlate")) p = polygon({QPointF(0, 0), QPointF(x, 0), QPointF(w, h / 2), QPointF(x, h), QPointF(0, h)});
        else p = polygon({QPointF(0, 0), QPointF(w, 0), QPointF(w, h * 0.8), QPointF(w / 2, h), QPointF(0, h * 0.8)});
    } else if (s == QLatin1String("chevron")) {
        const qreal x = ss * a("adj", 50000) / 100000;
        p = polygon({QPointF(0, 0), QPointF(w - x, 0), QPointF(w, h / 2), QPointF(w - x, h), QPointF(0, h), QPointF(x, h / 2)});
    } else if (s == QLatin1String("rightArrow") || s == QLatin1String("leftArrow") || s == QLatin1String("stripedRightArrow")
               || s == QLatin1String("notchedRightArrow")) {
        const qreal y1 = h / 2 - h * a("adj1", 50000) / 200000, y2 = h / 2 + h * a("adj1", 50000) / 200000;
        const qreal head = ss * a("adj2", 50000) / 100000;
        const qreal x1 = w - head;
        p = polygon({QPointF(0, y1), QPointF(x1, y1), QPointF(x1, 0), QPointF(w, h / 2), QPointF(x1, h), QPointF(x1, y2), QPointF(0, y2)});
        if (s == QLatin1String("leftArrow")) p = QTransform(-1, 0, 0, 1, w, 0).map(p);
    } else if (s == QLatin1String("upArrow") || s == QLatin1String("downArrow")) {
        const qreal x1 = w / 2 - w * a("adj1", 50000) / 200000, x2 = w / 2 + w * a("adj1", 50000) / 200000;
        const qreal y1 = ss * a("adj2", 50000) / 100000;
        p = polygon({QPointF(x1, h), QPointF(x1, y1), QPointF(0, y1), QPointF(w / 2, 0), QPointF(w, y1), QPointF(x2, y1), QPointF(x2, h)});
        if (s == QLatin1String("downArrow")) p = QTransform(1, 0, 0, -1, 0, h).map(p);
    } else if (s == QLatin1String("leftRightArrow")) {
        const qreal y1 = h / 2 - h * a("adj1", 50000) / 200000, y2 = h / 2 + h * a("adj1", 50000) / 200000;
        const qreal x = ss * a("adj2", 50000) / 100000;
        p = polygon({QPointF(0, h / 2), QPointF(x, 0), QPointF(x, y1), QPointF(w - x, y1), QPointF(w - x, 0), QPointF(w, h / 2),
                     QPointF(w - x, h), QPointF(w - x, y2), QPointF(x, y2), QPointF(x, h)});
    } else if (s == QLatin1String("upDownArrow")) {
        const qreal x1 = w / 2 - w * a("adj1", 50000) / 200000, x2 = w / 2 + w * a("adj1", 50000) / 200000;
        const qreal y = ss * a("adj2", 50000) / 100000;
        p = polygon({QPointF(w / 2, 0), QPointF(w, y), QPointF(x2, y), QPointF(x2, h - y), QPointF(w, h - y), QPointF(w / 2, h),
                     QPointF(0, h - y), QPointF(x1, h - y), QPointF(x1, y), QPointF(0, y)});
    } else if (s == QLatin1String("bentUpArrow")) {
        const qreal t = ss * a("adj1", 25000) / 100000, hw = ss * a("adj2", 25000) / 100000, hl = ss * a("adj3", 25000) / 100000;
        const qreal xs = w - hw;   // the shaft's middle, up
        p = polygon({QPointF(0, h - t), QPointF(xs - t / 2, h - t), QPointF(xs - t / 2, hl), QPointF(xs - hw, hl), QPointF(xs, 0),
                     QPointF(std::min(w, xs + hw), hl), QPointF(xs + t / 2, hl), QPointF(xs + t / 2, h), QPointF(0, h)});
    } else if (s == QLatin1String("bentArrow")) {
        const qreal t = ss * a("adj1", 25000) / 100000, hw = ss * a("adj2", 25000) / 100000, hl = ss * a("adj3", 25000) / 100000;
        const qreal yc = hw;   // the shaft's middle, right
        p = polygon({QPointF(0, h), QPointF(0, yc + t / 2 + 0.0001), QPointF(0, yc - t / 2), QPointF(w - hl, yc - t / 2), QPointF(w - hl, 0),
                     QPointF(w, yc), QPointF(w - hl, std::min(h, 2 * hw)), QPointF(w - hl, yc + t / 2), QPointF(t, yc + t / 2), QPointF(t, h)});
    } else if (s == QLatin1String("plus") || s == QLatin1String("mathPlus") || s == QLatin1String("flowChartPredefinedProcess")) {
        if (s == QLatin1String("flowChartPredefinedProcess")) {
            p.addRect(0, 0, w, h);
            p.moveTo(w / 8, 0);
            p.lineTo(w / 8, h);
            p.moveTo(7 * w / 8, 0);
            p.lineTo(7 * w / 8, h);
        } else {
            const qreal x = s == QLatin1String("plus") ? ss * a("adj", 25000) / 100000 : ss * (0.5 - a("adj1", 23520) / 200000);
            const qreal x2 = s == QLatin1String("plus") ? x : x;
            p = polygon({QPointF(x, 0), QPointF(w - x, 0), QPointF(w - x, x2), QPointF(w, x2), QPointF(w, h - x2), QPointF(w - x, h - x2),
                         QPointF(w - x, h), QPointF(x, h), QPointF(x, h - x2), QPointF(0, h - x2), QPointF(0, x2), QPointF(x, x2)});
            if (s == QLatin1String("mathPlus")) {
                const qreal t = ss * a("adj1", 23520) / 100000;
                p = QPainterPath();
                p.addRect(QRectF(w * 0.1, h / 2 - t / 2, w * 0.8, t));
                p.addRect(QRectF(w / 2 - t / 2, h * 0.1, t, h * 0.8));
                p.setFillRule(Qt::WindingFill);
                p = p.simplified();
            }
        }
    } else if (s == QLatin1String("mathMultiply")) {
        const qreal t = ss * a("adj1", 23520) / 100000;
        QPainterPath bar;
        bar.addRect(QRectF(-ss * 0.45, -t / 2, ss * 0.9, t));
        QPainterPath x = QTransform().translate(w / 2, h / 2).rotate(45).map(bar);
        x.addPath(QTransform().translate(w / 2, h / 2).rotate(-45).map(bar));
        x.setFillRule(Qt::WindingFill);
        p = x.simplified();
    } else if (s == QLatin1String("mathEqual")) {
        const qreal t = h * a("adj1", 23520) / 100000, gap = h * a("adj2", 11760) / 100000;
        p.addRect(QRectF(w * 0.1, h / 2 - gap / 2 - t, w * 0.8, t));
        p.addRect(QRectF(w * 0.1, h / 2 + gap / 2, w * 0.8, t));
    } else if (s == QLatin1String("mathMinus")) {
        const qreal t = h * a("adj1", 23520) / 100000;
        p.addRect(QRectF(w * 0.1, h / 2 - t / 2, w * 0.8, t));
    } else if (s.startsWith(QLatin1String("star"))) {
        bool ok = false;
        const int n = s.mid(4).toInt(&ok);
        const qreal inner = n == 5 ? 0.382 : n == 4 ? 0.38 : 0.5 + std::min(n, 32) * 0.01;
        p = star(ok && n >= 4 && n <= 32 ? n : 5, w, h, std::clamp(av.contains(QStringLiteral("adj")) ? a("adj", 19098) / 50000 : inner, 0.05, 0.95));
    } else if (s == QLatin1String("can") || s == QLatin1String("flowChartMagneticDisk")) {
        const qreal ry = s == QLatin1String("can") ? ss * a("adj", 25000) / 200000 : h / 6;
        p.moveTo(0, ry);
        p.arcTo(QRectF(0, 0, w, 2 * ry), 180, -180);
        p.lineTo(w, h - ry);
        p.arcTo(QRectF(0, h - 2 * ry, w, 2 * ry), 0, -180);
        p.closeSubpath();
        p.addEllipse(QRectF(0, 0, w, 2 * ry));
    } else if (s == QLatin1String("cube")) {
        const qreal d = ss * a("adj", 25000) / 100000;
        p = polygon({QPointF(0, d), QPointF(d, 0), QPointF(w, 0), QPointF(w, h - d), QPointF(w - d, h), QPointF(0, h)});
        p.moveTo(0, d);
        p.lineTo(w - d, d);
        p.lineTo(w, 0);
        p.moveTo(w - d, d);
        p.lineTo(w - d, h);
    } else if (s == QLatin1String("donut")) {
        const qreal t = ss * a("adj", 25000) / 100000;
        p.addEllipse(QRectF(0, 0, w, h));
        p.addEllipse(QRectF(t, t, w - 2 * t, h - 2 * t));
        p.setFillRule(Qt::OddEvenFill);
    } else if (s == QLatin1String("frame")) {
        const qreal t = ss * a("adj1", 12500) / 100000;
        p.addRect(0, 0, w, h);
        p.addRect(QRectF(t, t, w - 2 * t, h - 2 * t));
        p.setFillRule(Qt::OddEvenFill);
    } else if (s == QLatin1String("arc")) {
        const qreal st = a("adj1", 16200000) / 60000, en = a("adj2", 0) / 60000;
        qreal sw = std::fmod(en - st + 720, 360);
        if (sw == 0) sw = 360;
        arcOf(p, QRectF(0, 0, w, h), st, sw, true);
        o.open = true;
    } else if (s == QLatin1String("pie") || s == QLatin1String("chord")) {
        const qreal st = a("adj1", s == QLatin1String("pie") ? 0 : 2700000) / 60000, en = a("adj2", 16200000) / 60000;
        qreal sw = std::fmod(en - st + 720, 360);
        if (sw == 0) sw = 360;
        if (s == QLatin1String("pie")) p.moveTo(w / 2, h / 2);
        arcOf(p, QRectF(0, 0, w, h), st, sw, s != QLatin1String("pie"));
        p.closeSubpath();
    } else if (s == QLatin1String("blockArc")) {
        const qreal st = a("adj1", 10800000) / 60000, en = a("adj2", 0) / 60000;
        const qreal t = ss * a("adj3", 25000) / 100000;
        qreal sw = std::fmod(en - st + 720, 360);
        if (sw == 0) sw = 360;
        arcOf(p, QRectF(0, 0, w, h), st, sw, true);
        const QRectF in(t, t, std::max(0.0, w - 2 * t), std::max(0.0, h - 2 * t));
        p.arcTo(in, -(st + sw), sw);
        p.closeSubpath();
    } else if (s == QLatin1String("leftBrace") || s == QLatin1String("rightBrace")) {
        const qreal r = std::min(w / 2, h * a("adj1", 8333) / 100000);
        const qreal y = h * a("adj2", 50000) / 100000;
        // A left brace: its tip at the left, at y.
        p.moveTo(w, 0);
        p.cubicTo(w / 2, 0, w / 2, 0, w / 2, std::min(r, y));
        p.lineTo(w / 2, std::max(y - r, std::min(r, y)));
        p.cubicTo(w / 2, y, w / 2, y, 0, y);
        p.cubicTo(w / 2, y, w / 2, y, w / 2, std::min(y + r, h));
        p.lineTo(w / 2, std::max(h - r, y + r));
        p.cubicTo(w / 2, h, w / 2, h, w, h);
        if (s == QLatin1String("rightBrace")) p = QTransform(-1, 0, 0, 1, w, 0).map(p);
        o.open = true;
    } else if (s == QLatin1String("leftBracket") || s == QLatin1String("rightBracket")) {
        const qreal r = std::min(w, h * a("adj", 8333) / 100000);
        p.moveTo(w, 0);
        p.quadTo(0, 0, 0, r);
        p.lineTo(0, h - r);
        p.quadTo(0, h, w, h);
        if (s == QLatin1String("rightBracket")) p = QTransform(-1, 0, 0, 1, w, 0).map(p);
        o.open = true;
    } else if (s == QLatin1String("bracketPair") || s == QLatin1String("bracePair")) {
        const qreal r = ss * a("adj", 16667) / 100000;
        p.moveTo(r, h);
        p.quadTo(0, h, 0, h - r);
        p.lineTo(0, r);
        p.quadTo(0, 0, r, 0);
        p.moveTo(w - r, 0);
        p.quadTo(w, 0, w, r);
        p.lineTo(w, h - r);
        p.quadTo(w, h, w - r, h);
        o.open = true;
    } else if (s == QLatin1String("snip1Rect")) {
        const qreal d = ss * a("adj", 16667) / 100000;
        p = polygon({QPointF(0, 0), QPointF(w - d, 0), QPointF(w, d), QPointF(w, h), QPointF(0, h)});
    } else if (s == QLatin1String("snip2SameRect")) {
        const qreal d = ss * a("adj1", 16667) / 100000;
        p = polygon({QPointF(d, 0), QPointF(w - d, 0), QPointF(w, d), QPointF(w, h), QPointF(0, h), QPointF(0, d)});
    } else if (s == QLatin1String("round1Rect") || s == QLatin1String("round2SameRect")) {
        const qreal r = ss * a(s == QLatin1String("round1Rect") ? "adj" : "adj1", 16667) / 100000;
        p.moveTo(s == QLatin1String("round1Rect") ? 0 : r, 0);
        if (s == QLatin1String("round2SameRect")) p.moveTo(r, 0);
        p.lineTo(w - r, 0);
        p.quadTo(w, 0, w, r);
        p.lineTo(w, h);
        p.lineTo(0, h);
        if (s == QLatin1String("round2SameRect")) {
            p.lineTo(0, r);
            p.quadTo(0, 0, r, 0);
        } else {
            p.lineTo(0, 0);
        }
        p.closeSubpath();
    } else if (s == QLatin1String("wedgeRectCallout") || s == QLatin1String("wedgeRoundRectCallout") || s == QLatin1String("wedgeEllipseCallout")) {
        const QPointF tip(w / 2 + w * a("adj1", -20833) / 100000, h / 2 + h * a("adj2", 62500) / 100000);
        QPainterPath body;
        if (s == QLatin1String("wedgeEllipseCallout")) body.addEllipse(QRectF(0, 0, w, h));
        else if (s == QLatin1String("wedgeRoundRectCallout")) body.addRoundedRect(QRectF(0, 0, w, h), ss / 6, ss / 6);
        else body.addRect(0, 0, w, h);
        // The tail: from the body's middle, a wedge a fifth of its smaller side wide.
        const QPointF mid(w / 2, h / 2);
        const QPointF d = tip - mid;
        const qreal len = std::hypot(d.x(), d.y());
        const QPointF n = len > 0 ? QPointF(-d.y(), d.x()) / len * (ss / 10) : QPointF();
        QPainterPath tail = polygon({mid + n, tip, mid - n});
        p = body.united(tail);
    } else if (s == QLatin1String("heart")) {
        p.moveTo(w / 2, h / 4);
        p.cubicTo(w / 2, 0, 0, 0, 0, h / 3);
        p.cubicTo(0, 2 * h / 3, w / 2, 0.85 * h, w / 2, h);
        p.cubicTo(w / 2, 0.85 * h, w, 2 * h / 3, w, h / 3);
        p.cubicTo(w, 0, w / 2, 0, w / 2, h / 4);
        p.closeSubpath();
    } else if (s == QLatin1String("lightningBolt")) {
        p = polygon({QPointF(w * 0.39, 0), QPointF(w * 0.58, h * 0.29), QPointF(w * 0.5, h * 0.33), QPointF(w * 0.76, h * 0.6),
                     QPointF(w * 0.68, h * 0.64), QPointF(w, h), QPointF(w * 0.44, h * 0.71), QPointF(w * 0.53, h * 0.66),
                     QPointF(w * 0.24, h * 0.42), QPointF(w * 0.33, h * 0.37), QPointF(0, h * 0.18)});
    } else if (s == QLatin1String("smileyFace")) {
        p.addEllipse(QRectF(0, 0, w, h));
        p.addEllipse(QRectF(w * 0.3, h * 0.3, w * 0.1, h * 0.1));
        p.addEllipse(QRectF(w * 0.6, h * 0.3, w * 0.1, h * 0.1));
        p.moveTo(w * 0.28, h * 0.65);
        p.quadTo(w / 2, h * 0.85, w * 0.72, h * 0.65);
    } else if (s == QLatin1String("flowChartDocument")) {
        p.moveTo(0, 0);
        p.lineTo(w, 0);
        p.lineTo(w, h * 0.83);
        p.cubicTo(w * 0.75, h * 0.7, w * 0.5, h * 1.05, 0, h * 0.92);
        p.closeSubpath();
    } else if (s == QLatin1String("flowChartDelay")) {
        p.moveTo(0, 0);
        p.lineTo(w / 2, 0);
        p.arcTo(QRectF(0, 0, w, h), 90, -180);
        p.lineTo(0, h);
        p.closeSubpath();
    } else {
        // Not one of these: its box, said (the kinds counted).
        *known = false;
        p.addRect(0, 0, w, h);
    }
    return o;
}

/// A custom geometry's paths in a box \a w by \a h (points).
Outline custom(const Node* cg, qreal w, qreal h)
{
    Outline o;
    const Node* list = cg->child(u"pathLst");
    if (list == nullptr) return o;
    bool anyClosed = false, anyOpen = false;
    QPainterPath unfilled;
    for (const Node* path : list->children(u"path")) {
        const double pw = path->num(u"w"), ph = path->num(u"h");
        const qreal sx = pw > 0 ? w / pw : 1 / kEmuPerPoint, sy = ph > 0 ? h / ph : 1 / kEmuPerPoint;
        const auto point = [&](const Node* p) { return QPointF(p->num(u"x") * sx, p->num(u"y") * sy); };
        QPainterPath sub;
        bool closed = false;
        for (const auto& cmd : path->kids) {
            const QString& n = cmd->name;
            const QList<const Node*> pts = cmd->children(u"pt");
            if (n == QLatin1String("moveTo") && !pts.isEmpty()) sub.moveTo(point(pts.at(0)));
            else if (n == QLatin1String("lnTo") && !pts.isEmpty()) sub.lineTo(point(pts.at(0)));
            else if (n == QLatin1String("cubicBezTo") && pts.size() >= 3) sub.cubicTo(point(pts.at(0)), point(pts.at(1)), point(pts.at(2)));
            else if (n == QLatin1String("quadBezTo") && pts.size() >= 2) sub.quadTo(point(pts.at(0)), point(pts.at(1)));
            else if (n == QLatin1String("arcTo"))
                ooxmlArc(sub, cmd->num(u"wR"), cmd->num(u"hR"), cmd->num(u"stAng") / 60000, cmd->num(u"swAng") / 60000, sx, sy);
            else if (n == QLatin1String("close")) {
                sub.closeSubpath();
                closed = true;
            }
        }
        const bool filled = path->attr(u"fill") != QLatin1String("none");
        (filled && closed ? anyClosed : anyOpen) = true;
        (filled ? o.path : unfilled).addPath(sub);
    }
    o.open = anyOpen && !anyClosed;
    // (A path not filled beside one that is: stroked alone, over it.)
    if (anyClosed) o.strokeOnly = unfilled;
    else o.path.addPath(unfilled);
    return o;
}

// ---------------------------------------------------------------------------
// Text formats: what each level of a list style, a paragraph and a run says,
// the unset left to what comes under it.

struct RunSpec {
    std::optional<qreal> size;
    std::optional<bool> bold, italic, strike;
    std::optional<QString> underline, caps, face;
    std::optional<QColor> color, highlight;
    std::optional<int> baseline;
    std::optional<qreal> spacing;

    void over(const RunSpec& o)
    {
        if (o.size) size = o.size;
        if (o.bold) bold = o.bold;
        if (o.italic) italic = o.italic;
        if (o.strike) strike = o.strike;
        if (o.underline) underline = o.underline;
        if (o.caps) caps = o.caps;
        if (o.face) face = o.face;
        if (o.color) color = o.color;
        if (o.highlight) highlight = o.highlight;
        if (o.baseline) baseline = o.baseline;
        if (o.spacing) spacing = o.spacing;
    }
};

struct Spacing {
    bool percent = true;
    qreal value = 1;   // a fraction of the line, or points
};

struct ParaSpec {
    std::optional<QString> align;
    std::optional<qreal> marL, indent;
    std::optional<Spacing> line, before, after;
    std::optional<QString> bulletKind;   // none, char, autoNum, blip
    std::optional<QString> bulletChar, bulletScheme, bulletFont;
    std::optional<int> bulletStart;
    std::optional<QColor> bulletColor;
    std::optional<qreal> bulletPct, bulletPts;
    bool bulletColorText = false, bulletFontText = false, bulletSizeText = false;
    RunSpec run;

    void over(const ParaSpec& o)
    {
        if (o.align) align = o.align;
        if (o.marL) marL = o.marL;
        if (o.indent) indent = o.indent;
        if (o.line) line = o.line;
        if (o.before) before = o.before;
        if (o.after) after = o.after;
        if (o.bulletKind) bulletKind = o.bulletKind;
        if (o.bulletChar) bulletChar = o.bulletChar;
        if (o.bulletScheme) bulletScheme = o.bulletScheme;
        if (o.bulletStart) bulletStart = o.bulletStart;
        if (o.bulletFontText) {
            bulletFont.reset();
            bulletFontText = true;
        } else if (o.bulletFont) {
            bulletFont = o.bulletFont;
            bulletFontText = false;
        }
        if (o.bulletColorText) {
            bulletColor.reset();
            bulletColorText = true;
        } else if (o.bulletColor) {
            bulletColor = o.bulletColor;
            bulletColorText = false;
        }
        if (o.bulletSizeText) {
            bulletPct.reset();
            bulletPts.reset();
        }
        if (o.bulletPct) {
            bulletPct = o.bulletPct;
            bulletPts.reset();
        }
        if (o.bulletPts) {
            bulletPts = o.bulletPts;
            bulletPct.reset();
        }
        run.over(o.run);
    }
};

RunSpec readRun(const Node* rPr, const Ctx& c)
{
    RunSpec r;
    if (rPr == nullptr) return r;
    if (rPr->has(u"sz")) r.size = std::clamp(rPr->num(u"sz") / 100, 1.0, 4000.0);
    if (rPr->has(u"b")) r.bold = rPr->flag(u"b");
    if (rPr->has(u"i")) r.italic = rPr->flag(u"i");
    if (rPr->has(u"u")) r.underline = rPr->attr(u"u");
    if (rPr->has(u"strike")) r.strike = rPr->attr(u"strike") != QLatin1String("noStrike");
    if (rPr->has(u"cap")) r.caps = rPr->attr(u"cap");
    if (rPr->has(u"baseline")) r.baseline = rPr->whole(u"baseline", 0, -100000, 100000) / 1000;
    if (rPr->has(u"spc")) r.spacing = rPr->num(u"spc") / 100;
    const Fill f = fillOf(rPr, c);
    if (f.kind == Fill::Solid) r.color = f.color;
    else if (f.kind == Fill::Gradient && !f.stops.isEmpty()) r.color = f.stops.first().second;
    if (const Node* h = rPr->child(u"highlight")) r.highlight = colorOf(h, c);
    if (const Node* latin = rPr->child(u"latin"); latin != nullptr && !latin->attr(u"typeface").isEmpty()) r.face = latin->attr(u"typeface");
    return r;
}

Spacing readSpacing(const Node* n)
{
    Spacing s;
    if (const Node* pct = n->child(u"spcPct")) s.value = pct->num(u"val") / 100000;
    else if (const Node* pts = n->child(u"spcPts")) {
        s.percent = false;
        s.value = pts->num(u"val") / 100;
    }
    return s;
}

ParaSpec readPara(const Node* pPr, const Ctx& c)
{
    ParaSpec p;
    if (pPr == nullptr) return p;
    if (pPr->has(u"algn")) p.align = pPr->attr(u"algn");
    if (pPr->has(u"marL")) p.marL = pt(pPr->num(u"marL"));
    if (pPr->has(u"indent")) p.indent = pt(pPr->num(u"indent"));
    for (const auto& k : pPr->kids) {
        const QString& n = k->name;
        if (n == QLatin1String("lnSpc")) p.line = readSpacing(k.get());
        else if (n == QLatin1String("spcBef")) p.before = readSpacing(k.get());
        else if (n == QLatin1String("spcAft")) p.after = readSpacing(k.get());
        else if (n == QLatin1String("buNone")) p.bulletKind = QStringLiteral("none");
        else if (n == QLatin1String("buChar")) {
            p.bulletKind = QStringLiteral("char");
            p.bulletChar = k->attr(u"char");
        } else if (n == QLatin1String("buAutoNum")) {
            p.bulletKind = QStringLiteral("autoNum");
            p.bulletScheme = k->attr(u"type", QStringLiteral("arabicPeriod"));
            p.bulletStart = k->whole(u"startAt", 1, 1, 32767);
        } else if (n == QLatin1String("buBlip")) {
            p.bulletKind = QStringLiteral("char");
            p.bulletChar = QStringLiteral("•");
        } else if (n == QLatin1String("buFont")) p.bulletFont = k->attr(u"typeface");
        else if (n == QLatin1String("buFontTx")) p.bulletFontText = true;
        else if (n == QLatin1String("buClr")) p.bulletColor = colorOf(k.get(), c);
        else if (n == QLatin1String("buClrTx")) p.bulletColorText = true;
        else if (n == QLatin1String("buSzPct")) p.bulletPct = k->num(u"val") / 100000;
        else if (n == QLatin1String("buSzPts")) p.bulletPts = k->num(u"val") / 100;
        else if (n == QLatin1String("buSzTx")) p.bulletSizeText = true;
        else if (n == QLatin1String("defRPr")) p.run = readRun(k.get(), c);
    }
    return p;
}

/// A list style's level \a level (from 0): its defPPr, then its lvlNpPr.
ParaSpec levelOf(const Node* lst, int level, const Ctx& c)
{
    ParaSpec p;
    if (lst == nullptr) return p;
    p.over(readPara(lst->child(u"defPPr"), c));
    p.over(readPara(lst->child(QStringLiteral("lvl%1pPr").arg(level + 1)), c));
    return p;
}

/// The family a typeface comes out in: the theme's for +mj-lt and +mn-lt,
/// one with its metrics or its look where it is not here.
/// How a font a deck asks for is drawn here: by itself, or by a stand-in
/// - as wide as the font asked and its lines as far apart, where that
/// font's measures are known (lines then break where PowerPoint breaks
/// them).
struct FontChoice {
    QString family;
    int stretch[4] = {100, 100, 100, 100};   // regular, bold, italic, bold italic
    qreal lineFactor = 1;
    bool substituted = false;
    bool kept = false;   // as wide as the font asked
};

/// What Office's fonts make of a sample - its width (regular, bold, italic,
/// bold italic) and their line height, in ems - measured on the fonts
/// themselves (none of them is here): a stand-in is drawn as wide in each,
/// its lines as far apart.
const char* const kFontSample = "The quick brown fox jumps over the lazy dog. Measured loss 3.2 dB at 1550 nm; ABCDEFGHIJ 0123456789";
struct FontMeasure {
    double width[4];
    double line;
};
const QHash<QString, FontMeasure>& officeFonts()
{
    // (Calibri Light is Calibri's, near enough; Aptos Display Aptos's.)
    static const QHash<QString, FontMeasure> measured{
        {QStringLiteral("Calibri"), {{42.832, 43.603, 42.659, 43.509}, 1.221}},
        {QStringLiteral("Calibri Light"), {{42.832, 43.603, 42.659, 43.509}, 1.221}},
        {QStringLiteral("Aptos"), {{45.095, 46.899, 45.051, 46.921}, 1.221}},
        {QStringLiteral("Aptos Display"), {{45.095, 46.899, 45.051, 46.921}, 1.221}},
        {QStringLiteral("Aptos Narrow"), {{41.647, 42.683, 41.535, 42.404}, 1.221}},
        {QStringLiteral("Cambria"), {{45.310, 48.351, 43.754, 46.945}, 1.172}},
        {QStringLiteral("Candara"), {{43.981, 44.265, 42.053, 43.071}, 1.221}},
        {QStringLiteral("Corbel"), {{43.069, 44.977, 41.853, 43.884}, 1.208}},
        {QStringLiteral("Constantia"), {{44.831, 47.892, 44.033, 47.541}, 1.221}},
        {QStringLiteral("Consolas"), {{54.430, 54.430, 54.430, 54.430}, 1.171}},
        {QStringLiteral("Century Gothic"), {{50.190, 50.377, 50.190, 50.377}, 1.226}},
        {QStringLiteral("Franklin Gothic Book"), {{45.384, 45.384, 45.521, 45.521}, 1.134}},
        {QStringLiteral("Franklin Gothic Medium"), {{45.947, 45.947, 45.859, 45.859}, 1.134}},
        {QStringLiteral("Gill Sans MT"), {{43.436, 49.306, 40.958, 46.342}, 1.160}},
        {QStringLiteral("Garamond"), {{42.465, 45.123, 38.489, 43.278}, 1.125}},
        {QStringLiteral("Rockwell"), {{48.046, 51.317, 46.441, 49.605}, 1.174}},
        {QStringLiteral("Perpetua"), {{39.162, 44.117, 35.189, 40.967}, 1.146}},
        {QStringLiteral("Century Schoolbook"), {{49.806, 54.565, 49.520, 53.747}, 1.202}},
        {QStringLiteral("Bookman Old Style"), {{53.078, 56.082, 51.978, 56.763}, 1.174}},
        {QStringLiteral("Book Antiqua"), {{47.047, 48.331, 43.831, 46.932}, 1.206}},
        {QStringLiteral("Tw Cen MT"), {{43.350, 44.835, 42.387, 41.674}, 1.089}},
        {QStringLiteral("Lucida Sans"), {{52.127, 54.541, 51.941, 54.939}, 1.178}},
        {QStringLiteral("News Gothic MT"), {{50.771, 51.669, 48.985, 51.475}, 1.209}},
        {QStringLiteral("Goudy Old Style"), {{42.839, 43.893, 39.577, 39.577}, 1.200}},
        {QStringLiteral("Century"), {{49.751, 49.751, 49.751, 49.751}, 1.202}},
    };
    return measured;
}

/// The stretch (QFont's) that makes \a family \a ratio as wide: as it does
/// it to that family, measured - Qt applies a stretch twice over to a font
/// it stretches itself on macOS (90: 0.81 as wide), once to one that has
/// narrower widths of its own.
int stretchFor(qreal ratio, const QString& family)
{
    if (std::abs(ratio - 1) < 0.01) return 100;
    QFont f(family);
    f.setPixelSize(1000);
    const qreal plain = QFontMetricsF(f).horizontalAdvance(QLatin1String(kFontSample));
    f.setStretch(80);
    const qreal narrow = QFontMetricsF(f).horizontalAdvance(QLatin1String(kFontSample));
    qreal power = plain > 0 && narrow > 0 ? std::log(narrow / plain) / std::log(0.8) : 1;
    if (!(power > 0.3 && power < 3)) power = 1;
    return std::clamp(int(std::lround(100 * std::pow(ratio, 1 / power))), 50, 200);
}

const FontChoice& choiceOf(const QString& face)
{
    static QHash<QString, FontChoice> chosen;
    if (auto it = chosen.constFind(face); it != chosen.constEnd()) return *it;
    static const QStringList families = QFontDatabase::families();
    const auto here = [](const QString& f) { return families.contains(f, Qt::CaseInsensitive); };
    FontChoice ch;
    ch.family = face;
    if (!here(face)) {
        // Metric-compatible fonts first (Carlito is Calibri's widths), then
        // what looks most alike of macOS's (measured: PT Sans is Calibri's
        // and Aptos's kind, near their widths), then Linux's.
        static const QHash<QString, QStringList> alike{
            {QStringLiteral("Calibri"), {"Carlito", "PT Sans", "Helvetica Neue", "Liberation Sans", "DejaVu Sans", "Arial"}},
            {QStringLiteral("Calibri Light"), {"Carlito", "PT Sans", "Helvetica Neue", "Liberation Sans", "DejaVu Sans", "Arial"}},
            {QStringLiteral("Aptos"), {"PT Sans", "Helvetica Neue", "Liberation Sans", "DejaVu Sans", "Arial"}},
            {QStringLiteral("Aptos Display"), {"PT Sans", "Helvetica Neue", "Liberation Sans", "DejaVu Sans", "Arial"}},
            {QStringLiteral("Aptos Narrow"), {"PT Sans Narrow", "Arial Narrow", "Liberation Sans Narrow", "PT Sans", "Arial"}},
            {QStringLiteral("Cambria"), {"Caladea", "Charter", "PT Serif", "Georgia", "Liberation Serif", "DejaVu Serif", "Times New Roman"}},
            {QStringLiteral("Cambria Math"), {"STIX Two Math", "Cambria", "Times New Roman"}},
            {QStringLiteral("Candara"), {"PT Sans", "Helvetica Neue", "DejaVu Sans", "Arial"}},
            {QStringLiteral("Corbel"), {"PT Sans", "Helvetica Neue", "DejaVu Sans", "Arial"}},
            {QStringLiteral("Constantia"), {"Charter", "PT Serif", "Georgia", "DejaVu Serif", "Times New Roman"}},
            {QStringLiteral("Segoe UI"), {"Selawik", "Helvetica Neue", "Liberation Sans", "DejaVu Sans", "Arial"}},
            {QStringLiteral("Consolas"), {"Inconsolata", "Menlo", "Monaco", "DejaVu Sans Mono", "Liberation Mono", "Courier New"}},
            {QStringLiteral("Century Gothic"), {"Avenir Next", "Futura", "URW Gothic", "DejaVu Sans"}},
            {QStringLiteral("Tahoma"), {"Verdana", "DejaVu Sans", "Arial"}},
            {QStringLiteral("Franklin Gothic Book"), {"Libre Franklin", "Helvetica Neue", "Liberation Sans", "Arial"}},
            {QStringLiteral("Franklin Gothic Medium"), {"Libre Franklin", "Helvetica Neue", "Liberation Sans", "Arial"}},
            {QStringLiteral("Gill Sans MT"), {"Gill Sans", "PT Sans", "Helvetica Neue", "Arial"}},
            {QStringLiteral("Garamond"), {"EB Garamond", "Baskerville", "Georgia", "Times New Roman"}},
            {QStringLiteral("Rockwell"), {"Roboto Slab", "Georgia", "Times New Roman"}},
            {QStringLiteral("Perpetua"), {"Baskerville", "Georgia", "Times New Roman"}},
            {QStringLiteral("Century Schoolbook"), {"TeX Gyre Schola", "New Century Schoolbook", "Georgia", "Times New Roman"}},
            {QStringLiteral("Century"), {"TeX Gyre Schola", "New Century Schoolbook", "Georgia", "Times New Roman"}},
            {QStringLiteral("Bookman Old Style"), {"TeX Gyre Bonum", "Bookman", "Georgia", "Times New Roman"}},
            {QStringLiteral("Book Antiqua"), {"Palatino", "Palatino Linotype", "TeX Gyre Pagella", "Georgia"}},
            {QStringLiteral("Tw Cen MT"), {"Futura", "Avenir Next", "DejaVu Sans"}},
            {QStringLiteral("Lucida Sans"), {"Lucida Grande", "DejaVu Sans", "Arial"}},
            {QStringLiteral("News Gothic MT"), {"Helvetica Neue", "Liberation Sans", "Arial"}},
            {QStringLiteral("Goudy Old Style"), {"Baskerville", "Georgia", "Times New Roman"}},
        };
        ch.substituted = true;
        ch.family = QStringLiteral("Helvetica Neue");
        if (!here(ch.family)) ch.family = QStringLiteral("Arial");
        for (const QString& f : alike.value(face))
            if (here(f)) {
                ch.family = f;
                break;
            }
        // As wide as the font asked, in each style; its lines as far apart.
        if (const auto m = officeFonts().constFind(face); m != officeFonts().constEnd()) {
            bool measured = true;
            for (int style = 0; style < 4; ++style) {
                QFont f(ch.family);
                f.setPixelSize(1000);
                f.setBold(style & 1);
                f.setItalic(style & 2);
                const QFontMetricsF fm(f);
                const qreal width = fm.horizontalAdvance(QLatin1String(kFontSample)) / 1000;
                if (style == 0 && fm.lineSpacing() > 0) ch.lineFactor = m->line / (fm.lineSpacing() / 1000);
                if (width > 0) ch.stretch[style] = stretchFor(m->width[style] / width, ch.family);
                else measured = false;
            }
            ch.kept = measured;
        }
    }
    return *chosen.insert(face, ch);
}

/// The font \a face (a theme's +mn-lt, +mj-lt too) is drawn with - a
/// stand-in said among what the slide does not draw as PowerPoint does.
FontChoice fontOf(QString face, const Ctx& c)
{
    if (face.isEmpty() || face.startsWith(QLatin1String("+mn"))) face = c.theme->minor;
    else if (face.startsWith(QLatin1String("+mj"))) face = c.theme->major;
    const FontChoice& ch = choiceOf(face);
    if (ch.substituted && c.counting && c.info != nullptr) {
        const QString said = ch.kept ? QObject::tr("the font %1, not installed: drawn as %2, as wide (lines break as in PowerPoint)").arg(face, ch.family)
                                     : QObject::tr("the font %1, not installed: drawn as %2 (lines may break elsewhere)").arg(face, ch.family);
        if (!c.info->notShown.contains(said)) c.info->notShown << said;
    }
    return ch;
}

QString familyOf(const QString& face, const Ctx& c)
{
    return fontOf(face, c).family;
}

/// A bullet character in a symbol font that is not here: what it stands for.
QString bulletChar(const QString& ch, const QString& font)
{
    if (ch.isEmpty()) return QStringLiteral("•");
    static const QStringList families = QFontDatabase::families();
    if (font.isEmpty() || families.contains(font, Qt::CaseInsensitive)) return ch;
    const ushort u = ch.at(0).unicode();
    if (font.startsWith(QLatin1String("Wingdings"), Qt::CaseInsensitive)) {
        switch (u) {
        case 0xA7: return QStringLiteral("▪");
        case 0xD8: return QStringLiteral("➢");
        case 0xFC: return QStringLiteral("✓");
        case 0x71: return QStringLiteral("❑");
        case 0x76: return QStringLiteral("❖");
        case 0x6E: return QStringLiteral("■");
        case 0x6C: return QStringLiteral("●");
        case 0x6F: return QStringLiteral("□");
        case 0xE0: return QStringLiteral("➡");
        case 0xE8: return QStringLiteral("➞");
        case 0x9F: return QStringLiteral("•");
        default: return QStringLiteral("•");
        }
    }
    if (font.compare(QLatin1String("Symbol"), Qt::CaseInsensitive) == 0 && (u == 0xB7 || u == 0xF0B7)) return QStringLiteral("•");
    return ch;
}

QString roman(int n, bool upper)
{
    static const std::pair<int, const char*> parts[] = {{1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"}, {100, "c"}, {90, "xc"},
                                                        {50, "l"},   {40, "xl"},  {10, "x"},  {9, "ix"},   {5, "v"},   {4, "iv"}, {1, "i"}};
    QString out;
    for (const auto& [v, s] : parts)
        while (n >= v) {
            out += QLatin1String(s);
            n -= v;
        }
    return upper ? out.toUpper() : out;
}

QString autoNumber(const QString& scheme, int n)
{
    QString body;
    if (scheme.startsWith(QLatin1String("roman"))) body = roman(std::max(1, n), scheme.startsWith(QLatin1String("romanUc")));
    else if (scheme.startsWith(QLatin1String("alpha"))) {
        QString letters;
        for (int v = std::max(1, n); v > 0; v = (v - 1) / 26) letters.prepend(QChar(QLatin1Char('a').unicode() + (v - 1) % 26));
        body = scheme.startsWith(QLatin1String("alphaUc")) ? letters.toUpper() : letters;
    } else body = QString::number(n);
    if (scheme.endsWith(QLatin1String("ParenBoth"))) return QLatin1Char('(') + body + QLatin1Char(')');
    if (scheme.endsWith(QLatin1String("ParenR"))) return body + QLatin1Char(')');
    if (scheme.endsWith(QLatin1String("Plain"))) return body;
    if (scheme.endsWith(QLatin1String("Minus"))) return QStringLiteral("- ") + body + QStringLiteral(" -");
    return body + QLatin1Char('.');
}

QTextCharFormat charFormat(const RunSpec& r, qreal scale, const Ctx& c)
{
    QTextCharFormat f;
    const FontChoice choice = fontOf(r.face.value_or(QString()), c);
    QFont font(choice.family);
    if (const int stretch = choice.stretch[(r.bold.value_or(false) ? 1 : 0) + (r.italic.value_or(false) ? 2 : 0)]; stretch != 100)
        font.setStretch(stretch);
    font.setPointSizeF(std::max(1.0, r.size.value_or(18) * scale));
    font.setBold(r.bold.value_or(false));
    font.setItalic(r.italic.value_or(false));
    font.setStrikeOut(r.strike.value_or(false));
    const QString caps = r.caps.value_or(QString());
    if (caps == QLatin1String("all")) font.setCapitalization(QFont::AllUppercase);
    else if (caps == QLatin1String("small")) font.setCapitalization(QFont::SmallCaps);
    if (r.spacing && *r.spacing != 0) font.setLetterSpacing(QFont::AbsoluteSpacing, *r.spacing);
    f.setFont(font, QTextCharFormat::FontPropertiesAll);
    const QString u = r.underline.value_or(QStringLiteral("none"));
    if (u != QLatin1String("none")) {
        f.setFontUnderline(true);
        if (u.contains(QLatin1String("wav"))) f.setUnderlineStyle(QTextCharFormat::WaveUnderline);
        else if (u.contains(QLatin1String("dot")) && u.contains(QLatin1String("Dash"))) f.setUnderlineStyle(QTextCharFormat::DashDotLine);
        else if (u.contains(QLatin1String("dash"), Qt::CaseInsensitive)) f.setUnderlineStyle(QTextCharFormat::DashUnderline);
        else if (u.contains(QLatin1String("dot"))) f.setUnderlineStyle(QTextCharFormat::DotLine);
    }
    f.setForeground(r.color.value_or(schemeColor(QStringLiteral("tx1"), c, std::nullopt)));
    if (r.highlight) f.setBackground(*r.highlight);
    if (const int b = r.baseline.value_or(0); b > 0) f.setVerticalAlignment(QTextCharFormat::AlignSuperScript);
    else if (b < 0) f.setVerticalAlignment(QTextCharFormat::AlignSubScript);
    return f;
}

/// What a text body takes from under it: list styles (lowest first) and
/// body properties, the shape's style's font and colour.
struct TextChain {
    QList<const Node*> lists;
    QList<const Node*> bodies;
    std::optional<QColor> fontColor;
    QString font;
    QStringList* paragraphs = nullptr;   // its text, told
};

std::shared_ptr<TextFrame> buildText(const Node* txBody, const TextChain& chain, Ctx& c)
{
    auto frame = std::make_shared<TextFrame>();
    if (txBody == nullptr) return frame;
    // Body properties: the master's, the layout's, its own.
    QList<const Node*> bodies = chain.bodies;
    bodies << txBody->child(u"bodyPr");
    qreal fontScale = 1, lineReduction = 0;
    for (const Node* b : bodies) {
        if (b == nullptr) continue;
        if (b->has(u"lIns")) frame->l = pt(b->num(u"lIns"));
        if (b->has(u"tIns")) frame->t = pt(b->num(u"tIns"));
        if (b->has(u"rIns")) frame->r = pt(b->num(u"rIns"));
        if (b->has(u"bIns")) frame->b = pt(b->num(u"bIns"));
        if (b->has(u"anchor")) frame->anchor = b->attr(u"anchor");
        if (b->has(u"wrap")) frame->wrap = b->attr(u"wrap") != QLatin1String("none");
        if (b->has(u"vert")) {
            const QString v = b->attr(u"vert");
            frame->vertical = v == QLatin1String("vert") || v == QLatin1String("eaVert") ? 90 : v == QLatin1String("vert270") ? 270 : 0;
        }
        if (const Node* fit = b->child(u"normAutofit")) {
            fontScale = fit->num(u"fontScale", 100000) / 100000;
            lineReduction = fit->num(u"lnSpcReduction", 0) / 100000;
        } else if (b->child(u"noAutofit") != nullptr || b->child(u"spAutoFit") != nullptr) {
            fontScale = 1;
            lineReduction = 0;
        }
    }
    QList<const Node*> lists = chain.lists;
    lists << txBody->child(u"lstStyle");
    QHash<int, std::pair<QString, int>> numbering;   // by level: the scheme and the count
    bool first = true;
    for (const Node* p : txBody->children(u"p")) {
        const Node* pPr = p->child(u"pPr");
        const int level = pPr != nullptr ? pPr->whole(u"lvl", 0, 0, 8) : 0;
        ParaSpec spec;
        spec.run.size = 18;
        for (int i = 0; i < lists.size(); ++i) {
            spec.over(levelOf(lists.at(i), level, c));
            // The shape's style's font and colour over the presentation's or
            // the master's text style (its first list), under the rest.
            if (i == 0) {
                if (chain.fontColor) spec.run.color = chain.fontColor;
                if (!chain.font.isEmpty()) spec.run.face = chain.font;
            }
        }
        if (lists.isEmpty()) {
            if (chain.fontColor) spec.run.color = chain.fontColor;
            if (!chain.font.isEmpty()) spec.run.face = chain.font;
        }
        spec.over(readPara(pPr, c));
        TextPara para;
        QString said;
        RunSpec firstRun = spec.run;   // (its font: the paragraph's lines)
        // Runs.
        for (const auto& k : p->kids) {
            if (k->name == QLatin1String("r") || k->name == QLatin1String("fld")) {
                RunSpec r = spec.run;
                r.over(readRun(k->child(u"rPr"), c));
                if (para.runs.isEmpty()) firstRun = r;
                QString text = k->child(u"t") != nullptr ? k->child(u"t")->text : QString();
                if (k->name == QLatin1String("fld") && k->attr(u"type") == QLatin1String("slidenum")) text = QString::number(c.number);
                para.runs << TextRun{text, charFormat(r, fontScale, c), false};
                said += text;
            } else if (k->name == QLatin1String("br")) {
                RunSpec r = spec.run;
                r.over(readRun(k->child(u"rPr"), c));
                para.runs << TextRun{QString(), charFormat(r, fontScale, c), true};
                said += QLatin1Char('\n');
            }
        }
        RunSpec end = spec.run;
        end.over(readRun(p->child(u"endParaRPr"), c));
        para.endFormat = charFormat(end, fontScale, c);
        const qreal size = (para.runs.isEmpty() ? end.size : spec.run.size).value_or(18) * fontScale;
        const qreal runSize = para.runs.isEmpty() ? para.endFormat.font().pointSizeF() : para.runs.first().format.font().pointSizeF();
        // The block: alignment, margins, spacing.
        QTextBlockFormat& b = para.block;
        const QString align = spec.align.value_or(QStringLiteral("l"));
        b.setAlignment(align == QLatin1String("ctr") ? Qt::AlignHCenter
                       : align == QLatin1String("r") ? Qt::AlignRight
                       : align == QLatin1String("just") || align == QLatin1String("dist") ? Qt::AlignJustify
                                                                                          : Qt::AlignLeft);
        para.marL = spec.marL.value_or(0);
        para.indent = spec.indent.value_or(0);
        b.setLeftMargin(para.marL);
        if (const Spacing line = spec.line.value_or(Spacing()); line.percent) {
            // (A stand-in's lines as far apart as the font asked has them.)
            const qreal pct = std::max(0.1, line.value - lineReduction) * 100 * fontOf((para.runs.isEmpty() ? end : firstRun).face.value_or(QString()), c).lineFactor;
            if (std::abs(pct - 100) > 0.5) b.setLineHeight(pct, QTextBlockFormat::ProportionalHeight);
        } else {
            b.setLineHeight(line.value * fontScale, QTextBlockFormat::FixedHeight);
        }
        const auto space = [&](const std::optional<Spacing>& s) {
            if (!s) return 0.0;
            return s->percent ? s->value * size * 1.2 : s->value;
        };
        if (!first) b.setTopMargin(space(spec.before));
        b.setBottomMargin(space(spec.after));
        // The bullet.
        const QString kind = spec.bulletKind.value_or(QStringLiteral("none"));
        const bool hasText = !said.trimmed().isEmpty();
        for (auto it = numbering.begin(); it != numbering.end();)
            it = it.key() > level ? numbering.erase(it) : std::next(it);
        if (kind == QLatin1String("autoNum") && hasText) {
            const QString scheme = spec.bulletScheme.value_or(QStringLiteral("arabicPeriod"));
            auto& [was, count] = numbering[level];
            if (was != scheme) {
                was = scheme;
                count = spec.bulletStart.value_or(1);
            } else {
                ++count;
            }
            para.bullet = autoNumber(scheme, count);
        } else {
            numbering.remove(level);
            if (kind == QLatin1String("char") && hasText) para.bullet = bulletChar(spec.bulletChar.value_or(QString()), spec.bulletFont.value_or(QString()));
        }
        if (!para.bullet.isEmpty()) {
            RunSpec br = para.runs.isEmpty() ? end : spec.run;
            if (!para.runs.isEmpty()) br.over(readRun(p->child(u"r") != nullptr ? p->child(u"r")->child(u"rPr") : nullptr, c));
            if (spec.bulletColor && !spec.bulletColorText) br.color = spec.bulletColor;
            if (spec.bulletFont && !spec.bulletFontText && kind == QLatin1String("char")) {
                const QString bf = *spec.bulletFont;
                static const QStringList families = QFontDatabase::families();
                if (families.contains(bf, Qt::CaseInsensitive)) br.face = bf;
            }
            qreal bsize = runSize / std::max(0.01, fontScale);
            if (spec.bulletPct) bsize *= *spec.bulletPct;
            else if (spec.bulletPts) bsize = *spec.bulletPts;
            br.size = bsize;
            br.underline = QStringLiteral("none");
            para.bulletFormat = charFormat(br, fontScale, c);
            para.hanging = para.indent < 0;
            b.setTextIndent(para.hanging ? 0 : para.indent);
        } else {
            b.setTextIndent(para.indent);
        }
        frame->paras << para;
        if (chain.paragraphs != nullptr && hasText)
            *chain.paragraphs << (para.bullet.isEmpty() ? said.trimmed() : para.bullet + QLatin1Char(' ') + said.trimmed());
        first = false;
    }
    return frame;
}

// ---------------------------------------------------------------------------
// Charts: their parts read.

// The most points of a series read: a cache saying more (bug hunt
// 2026-10-23 A4: ptCount="2000000000" asked for a list of two thousand
// million) is read as this many, and a point's index past it is left.
constexpr int kMaxPoints = 1000000;

QList<double> numbers(const Node* ref, int* count = nullptr)
{
    QList<double> out;
    if (ref == nullptr) return out;
    const Node* cache = ref->descendant(u"numCache");
    if (cache == nullptr) cache = ref->descendant(u"numLit");
    if (cache == nullptr) return out;
    const int n = cache->child(u"ptCount") != nullptr ? cache->child(u"ptCount")->whole(u"val", 0, 0, kMaxPoints) : 0;
    QMap<int, double> by;
    int most = -1;
    for (const Node* p : cache->children(u"pt")) {
        const int i = p->whole(u"idx");
        const Node* v = p->child(u"v");
        if (v == nullptr || i < 0 || i > kMaxPoints) continue;
        bool ok = false;
        const double d = v->text.trimmed().toDouble(&ok);
        if (ok) by.insert(i, d);
        most = std::max(most, i);
    }
    const int size = std::max(n, most + 1);
    for (int i = 0; i < size; ++i) out << by.value(i, qQNaN());
    if (count != nullptr) *count = size;
    return out;
}

QStringList strings(const Node* ref)
{
    QStringList out;
    if (ref == nullptr) return out;
    const Node* cache = ref->descendant(u"strCache");
    if (cache == nullptr) cache = ref->descendant(u"strLit");
    // Categories of several levels: the innermost (its first), each label's
    // own.
    if (const Node* multi = cache == nullptr ? ref->descendant(u"multiLvlStrCache") : nullptr) {
        const Node* level = multi->child(u"lvl");
        const int n = multi->child(u"ptCount") != nullptr ? multi->child(u"ptCount")->whole(u"val", 0, 0, kMaxPoints) : 0;
        QMap<int, QString> by;
        int most = -1;
        if (level != nullptr)
            for (const Node* p : level->children(u"pt")) {
                const int i = p->whole(u"idx");
                if (i < 0 || i > kMaxPoints) continue;
                by.insert(i, p->child(u"v") != nullptr ? p->child(u"v")->text : QString());
                most = std::max(most, i);
            }
        for (int i = 0; i < std::max(n, most + 1); ++i) out << by.value(i);
        return out;
    }
    if (cache == nullptr) {
        // Categories that are numbers.
        for (double d : numbers(ref)) out << (std::isnan(d) ? QString() : QLocale::c().toString(d, 'g', 6));
        return out;
    }
    const int n = cache->child(u"ptCount") != nullptr ? cache->child(u"ptCount")->whole(u"val", 0, 0, kMaxPoints) : 0;
    QMap<int, QString> by;
    int most = -1;
    for (const Node* p : cache->children(u"pt")) {
        const int i = p->whole(u"idx");
        if (i < 0 || i > kMaxPoints) continue;
        by.insert(i, p->child(u"v") != nullptr ? p->child(u"v")->text : QString());
        most = std::max(most, i);
    }
    for (int i = 0; i < std::max(n, most + 1); ++i) out << by.value(i);
    return out;
}

QString richText(const Node* tx)
{
    if (tx == nullptr) return QString();
    if (const Node* rich = tx->descendant(u"rich")) {
        QStringList paras;
        for (const Node* p : rich->children(u"p")) paras << p->allText();
        return paras.join(QLatin1Char('\n'));
    }
    const QStringList s = strings(tx);
    return s.isEmpty() ? QString() : s.first();
}

/// A chart text's size and colour (its txPr's or a title's rich text's
/// first run).
void textStyle(const Node* holder, const Ctx& c, qreal* size, QColor* color)
{
    if (holder == nullptr) return;
    const Node* rPr = holder->descendant(u"defRPr");
    if (const Node* r = holder->descendant(u"rPr")) rPr = r;
    if (rPr == nullptr) return;
    // (ST_TextFontSize: 1 to 4000 points.)
    if (rPr->has(u"sz")) *size = std::clamp(rPr->num(u"sz") / 100, 1.0, 4000.0);
    const Fill f = fillOf(rPr, c);
    if (f.kind == Fill::Solid) *color = f.color;
}

std::shared_ptr<Chart> readChart(const Node* space, Ctx& c)
{
    auto chart = std::make_shared<Chart>();
    for (int i = 1; i <= 6; ++i) chart->accents << schemeColor(QStringLiteral("accent%1").arg(i), c, std::nullopt);
    const FontChoice font = fontOf(QString(), c);
    chart->family = font.family;
    chart->stretch = font.stretch[0];
    chart->lineFactor = font.lineFactor;
    if (space == nullptr) return chart;
    textStyle(space->child(u"txPr"), c, &chart->fontSize, &chart->textColor);
    // (PowerPoint's own where the chart says nothing: its title 1.2 times its
    // text, its axes' labels as large as its text.)
    chart->titleSize = chart->fontSize * 1.2;
    chart->titleColor = chart->textColor;
    if (const Node* sp = space->child(u"spPr")) {
        chart->area = fillOf(sp, c);
        applyLine(chart->border, sp->child(u"ln"), c);
    }
    const Node* ch = space->child(u"chart");
    if (ch == nullptr) return chart;
    if (const Node* title = ch->child(u"title")) {
        chart->hasTitle = true;
        chart->title = richText(title->child(u"tx"));
        textStyle(title, c, &chart->titleSize, &chart->titleColor);
        if (title->child(u"tx") == nullptr) textStyle(title->child(u"txPr"), c, &chart->titleSize, &chart->titleColor);
    }
    const bool autoDeleted = ch->child(u"autoTitleDeleted") != nullptr && ch->child(u"autoTitleDeleted")->flag(u"val", true);
    chart->legendSize = chart->fontSize;
    chart->legendColor = chart->textColor;
    if (const Node* legend = ch->child(u"legend")) {
        chart->legend = true;
        if (const Node* pos = legend->child(u"legendPos")) chart->legendPos = pos->attr(u"val", QStringLiteral("r"));
        textStyle(legend->child(u"txPr"), c, &chart->legendSize, &chart->legendColor);
    }
    const Node* plot = ch->child(u"plotArea");
    if (plot == nullptr) return chart;
    if (const Node* sp = plot->child(u"spPr")) chart->plotArea = fillOf(sp, c);
    for (const auto& k : plot->kids) {
        const QString& n = k->name;
        if (n.endsWith(QLatin1String("Chart"))) {
            ChartPlot p;
            p.kind = n.left(n.size() - 5);
            if (p.kind.endsWith(QLatin1String("3D"))) p.kind.chop(2);
            if (p.kind == QLatin1String("ofPie")) p.kind = QStringLiteral("pie");
            if (const Node* g = k->child(u"grouping")) p.grouping = g->attr(u"val", QStringLiteral("standard"));
            if (const Node* d = k->child(u"barDir")) p.horizontal = d->attr(u"val") == QLatin1String("bar");
            if (const Node* s = k->child(u"scatterStyle")) p.scatterStyle = s->attr(u"val", QStringLiteral("lineMarker"));
            if (const Node* v = k->child(u"varyColors")) p.vary = v->flag(u"val", true);
            if (const Node* g = k->child(u"gapWidth")) p.gap = g->whole(u"val", 150, 0, 500);
            if (const Node* o = k->child(u"overlap")) p.overlap = o->whole(u"val", 0, -100, 100);
            if (const Node* h = k->child(u"holeSize")) p.hole = h->whole(u"val", 50, 0, 90);
            if (const Node* f = k->child(u"firstSliceAng")) p.firstSlice = f->num(u"val");
            for (const Node* a : k->children(u"axId")) p.axes << a->attr(u"val");
            if (p.grouping == QLatin1String("stacked") || p.grouping == QLatin1String("percentStacked")) p.overlap = 100;
            for (const Node* s : k->children(u"ser")) {
                ChartSeries series;
                series.name = richText(s->child(u"tx"));
                if (const Node* sp = s->child(u"spPr")) {
                    series.fill = fillOf(sp, c);
                    applyLine(series.line, sp->child(u"ln"), c);
                }
                if (const Node* m = s->child(u"marker")) {
                    if (const Node* sym = m->child(u"symbol")) series.marker = sym->attr(u"val", QStringLiteral("auto"));
                    if (const Node* size = m->child(u"size")) series.markerSize = std::clamp(size->num(u"val", 5), 2.0, 72.0);
                    if (const Node* sp = m->child(u"spPr")) {
                        series.markerFill = fillOf(sp, c);
                        applyLine(series.markerLine, sp->child(u"ln"), c);
                    }
                }
                if (const Node* sm = s->child(u"smooth")) series.smooth = sm->flag(u"val", true);
                for (const Node* dpt : s->children(u"dPt")) {
                    const int i = dpt->child(u"idx") != nullptr ? dpt->child(u"idx")->whole(u"val", -1) : -1;
                    if (i >= 0)
                        if (const Node* sp = dpt->child(u"spPr")) series.pointFills.insert(i, fillOf(sp, c));
                }
                if (p.kind == QLatin1String("scatter") || p.kind == QLatin1String("bubble")) {
                    series.y = numbers(s->child(u"yVal"));
                    if (const Node* xv = s->child(u"xVal")) {
                        series.x = numbers(xv);
                        if (series.x.isEmpty())
                            for (int i = 0; i < strings(xv).size(); ++i) series.x << i + 1;
                    }
                    if (series.x.isEmpty())
                        for (int i = 0; i < series.y.size(); ++i) series.x << i + 1;
                } else {
                    series.y = numbers(s->child(u"val"));
                    series.cats = strings(s->child(u"cat"));
                }
                p.series << series;
            }
            if (!autoDeleted && !chart->hasTitle && p.series.size() == 1 && (p.kind == QLatin1String("pie") || p.kind == QLatin1String("doughnut"))) {
                chart->hasTitle = !p.series.first().name.isEmpty();
                chart->title = p.series.first().name;
            }
            chart->plots << p;
        } else if (n == QLatin1String("valAx") || n == QLatin1String("catAx") || n == QLatin1String("dateAx") || n == QLatin1String("serAx")) {
            ChartAxis a;
            a.category = n != QLatin1String("valAx");
            a.id = k->child(u"axId") != nullptr ? k->child(u"axId")->attr(u"val") : QString();
            a.crossAx = k->child(u"crossAx") != nullptr ? k->child(u"crossAx")->attr(u"val") : QString();
            if (const Node* d = k->child(u"delete")) a.deleted = d->flag(u"val", true);
            if (const Node* p = k->child(u"axPos")) a.pos = p->attr(u"val", QStringLiteral("b"));
            if (const Node* s = k->child(u"scaling")) {
                if (const Node* lb = s->child(u"logBase")) a.logBase = lb->num(u"val", 10);
                if (const Node* o = s->child(u"orientation")) a.reversed = o->attr(u"val") == QLatin1String("maxMin");
                if (const Node* mn = s->child(u"min")) a.min = mn->num(u"val");
                if (const Node* mx = s->child(u"max")) a.max = mx->num(u"val");
            }
            if (const Node* mu = k->child(u"majorUnit")) a.major = mu->num(u"val");
            a.grid = k->child(u"majorGridlines") != nullptr;
            if (const Node* g = k->child(u"majorGridlines")) {
                a.gridLine.kind = Stroke::Solid;
                a.gridLine.color = QColor(0xD9, 0xD9, 0xD9);
                applyLine(a.gridLine, g->at({"spPr", "ln"}), c);
            }
            if (const Node* f = k->child(u"numFmt")) a.format = f->attr(u"formatCode", QStringLiteral("General"));
            if (const Node* t = k->child(u"title")) a.title = richText(t->child(u"tx"));
            if (const Node* tl = k->child(u"tickLblPos")) a.labels = tl->attr(u"val") != QLatin1String("none");
            a.line.kind = Stroke::Solid;
            a.line.color = QColor(0xBF, 0xBF, 0xBF);
            applyLine(a.line, k->at({"spPr", "ln"}), c);
            a.textColor = chart->textColor;
            a.fontSize = chart->fontSize;
            textStyle(k->child(u"txPr"), c, &a.fontSize, &a.textColor);
            chart->axes << a;
        }
    }
    return chart;
}

// ---------------------------------------------------------------------------
// Reading a slide's elements.

QRectF xfrmRect(const Node* xfrm, qreal* rot = nullptr, bool* flipH = nullptr, bool* flipV = nullptr)
{
    if (xfrm == nullptr) return QRectF();
    const Node* off = xfrm->child(u"off");
    const Node* ext = xfrm->child(u"ext");
    if (rot != nullptr) *rot = xfrm->num(u"rot") / 60000;
    if (flipH != nullptr) *flipH = xfrm->flag(u"flipH");
    if (flipV != nullptr) *flipV = xfrm->flag(u"flipV");
    return QRectF(off != nullptr ? pt(off->num(u"x")) : 0, off != nullptr ? pt(off->num(u"y")) : 0, ext != nullptr ? pt(ext->num(u"cx")) : 0,
                  ext != nullptr ? pt(ext->num(u"cy")) : 0);
}

enum class Layer { Master, Layout, Slide };

/// A shape's placeholder, and the layout's and master's of its kind.
struct PlaceholderChain {
    const Node* ph = nullptr;
    QString type, idx;
    const Node* layoutSp = nullptr;
    const Node* masterSp = nullptr;
};

PlaceholderChain placeholderOf(const Node* nvPr, Layer layer, const Ctx& c)
{
    PlaceholderChain p;
    p.ph = nvPr != nullptr ? nvPr->child(u"ph") : nullptr;
    if (p.ph == nullptr) return p;
    p.type = p.ph->attr(u"type", QStringLiteral("obj"));
    p.idx = p.ph->attr(u"idx");
    if (layer == Layer::Slide) {
        if (const Placeholder* l = findPlaceholder(c.layout, p.type, p.idx, true)) {
            p.layoutSp = l->sp;
            if (const Placeholder* m = findPlaceholder(c.master, l->type, l->idx, false)) p.masterSp = m->sp;
        } else if (const Placeholder* m = findPlaceholder(c.master, p.type, p.idx, false)) {
            p.masterSp = m->sp;
        }
    } else if (layer == Layer::Layout) {
        if (const Placeholder* m = findPlaceholder(c.master, p.type, p.idx, false)) p.masterSp = m->sp;
    }
    return p;
}

/// The master's text style a placeholder's text takes.
const Node* textStyleFor(const QString& type, const Ctx& c)
{
    if (c.txStyles == nullptr) return nullptr;
    const QString kind = masterType(type);
    if (kind == QLatin1String("title")) return c.txStyles->child(u"titleStyle");
    if (kind == QLatin1String("body")) return c.txStyles->child(u"bodyStyle");
    return c.txStyles->child(u"otherStyle");
}

void buildTree(const Node* tree, Layer layer, Ctx& c, QList<Element>& out);

void noteMissing(Ctx& c, const QString& what)
{
    if (c.counting && c.info != nullptr && !c.info->notShown.contains(what)) c.info->notShown << what;
}

Element buildShape(const Node* sp, Layer layer, Ctx& c, bool* skip)
{
    Element e;
    *skip = false;
    const Node* nv = sp->child(u"nvSpPr");
    if (nv == nullptr) nv = sp->child(u"nvCxnSpPr");
    const Node* cNvPr = nv != nullptr ? nv->child(u"cNvPr") : nullptr;
    if (cNvPr != nullptr && cNvPr->flag(u"hidden")) {
        *skip = true;
        return e;
    }
    const PlaceholderChain ph = placeholderOf(nv != nullptr ? nv->child(u"nvPr") : nullptr, layer, c);
    // A master's or a layout's placeholders are where its slides' go, not
    // drawn themselves.
    if (ph.ph != nullptr && layer != Layer::Slide) {
        *skip = true;
        return e;
    }
    const Node* spPr = sp->child(u"spPr");
    const Node* xfrm = spPr != nullptr ? spPr->child(u"xfrm") : nullptr;
    // Where it is: its own, else its layout's placeholder's, else the master's.
    for (const Node* from : {ph.layoutSp, ph.masterSp})
        if (xfrm == nullptr && from != nullptr && from->child(u"spPr") != nullptr) xfrm = from->child(u"spPr")->child(u"xfrm");
    e.rect = xfrmRect(xfrm, &e.rotation, &e.flipH, &e.flipV);
    // Its style's line and fill, under its own; a placeholder's from the
    // master's and the layout's, under its own.
    const StyleRefs refs = styleRefs(sp->child(u"style"), c);
    e.stroke = refs.line;
    e.fill = refs.fill;
    for (const Node* from : {ph.masterSp, ph.layoutSp, sp}) {
        if (from == nullptr) continue;
        const Node* pr = from->child(u"spPr");
        if (pr == nullptr) continue;
        const Fill f = fillOf(pr, c);
        if (f.kind != Fill::Unset) e.fill = f;
        applyLine(e.stroke, pr->child(u"ln"), c);
    }
    if (sp->flag(u"useBgFill")) e.fill.kind = Fill::None;   // (the slide's own background, under it)
    // Its shadow: its style's, under the master's and the layout's
    // placeholder's effects and its own (an empty list: none).
    e.shadow = shadowOf(refs.effects, c, refs.effectColor);
    for (const Node* from : {ph.masterSp, ph.layoutSp, sp})
        if (const Node* pr = from != nullptr ? from->child(u"spPr") : nullptr; pr != nullptr && pr->child(u"effectLst") != nullptr)
            e.shadow = shadowOf(pr->child(u"effectLst"), c);
    // Its outline.
    const QRectF box(0, 0, e.rect.width(), e.rect.height());
    std::optional<QRectF> textBox;   // its preset's own, for its text
    const Node* geomHolder = spPr;
    for (const Node* from : {ph.layoutSp, ph.masterSp})
        if ((geomHolder == nullptr || (geomHolder->child(u"prstGeom") == nullptr && geomHolder->child(u"custGeom") == nullptr)) && from != nullptr
            && from->child(u"spPr") != nullptr)
            geomHolder = from->child(u"spPr");
    if (const Node* cg = geomHolder != nullptr ? geomHolder->child(u"custGeom") : nullptr) {
        const Outline o = custom(cg, box.width(), box.height());
        e.path = o.path;
        e.strokeOnly = o.strokeOnly;
        e.open = o.open;
    } else {
        const Node* pg = geomHolder != nullptr ? geomHolder->child(u"prstGeom") : nullptr;
        const QString prst = pg != nullptr ? pg->attr(u"prst", QStringLiteral("rect")) : QStringLiteral("rect");
        QHash<QString, qreal> av;
        if (const Node* list = pg != nullptr ? pg->child(u"avLst") : nullptr)
            for (const Node* gd : list->children(u"gd")) {
                const QString f = gd->attr(u"fmla");
                if (f.startsWith(QLatin1String("val "))) av.insert(gd->attr(u"name"), number(f.mid(4)));
            }
        bool known = true;
        const Outline o = preset(prst, box.width(), box.height(), av, &known);
        e.path = o.path;
        e.strokeOnly = o.strokeOnly;
        e.open = o.open;
        textBox = o.text;
        if (!known) c.approximated.insert(prst);
    }
    if (e.open) e.fill.kind = Fill::None;
    if (e.stroke.kind == Stroke::Unset) e.stroke.kind = Stroke::None;
    // Its text: the master's text style for its kind (the presentation's for
    // a shape that is no placeholder), the master's and the layout's
    // placeholder's list styles and body properties, its own.
    if (const Node* tx = sp->child(u"txBody")) {
        TextChain chain;
        if (ph.ph != nullptr) chain.lists << textStyleFor(ph.type, c);
        else chain.lists << c.defaultTextStyle;
        for (const Node* from : {ph.masterSp, ph.layoutSp}) {
            if (from == nullptr || from->child(u"txBody") == nullptr) continue;
            chain.lists << from->child(u"txBody")->child(u"lstStyle");
            chain.bodies << from->child(u"txBody")->child(u"bodyPr");
        }
        chain.fontColor = refs.fontColor;
        chain.font = refs.font;
        QStringList said;
        chain.paragraphs = layer == Layer::Slide && c.counting ? &said : nullptr;
        e.text = buildText(tx, chain, c);
        if (textBox) e.text->box = textBox;
        if (const Node* txX = sp->child(u"txXfrm")) {
            // A diagram's own text box, in the drawing's coordinates: in the
            // shape's.
            const QRectF r = xfrmRect(txX);
            e.text->box = r.translated(-e.rect.topLeft());
        }
        if (chain.paragraphs != nullptr && c.info != nullptr) {
            c.info->paragraphs << said;
            if (ph.ph != nullptr && masterType(ph.type) == QLatin1String("title") && c.info->title.isEmpty())
                c.info->title = said.join(QLatin1Char(' ')).simplified();
        }
        if (e.text->empty()) e.text.reset();
    }
    return e;
}

Element buildPicture(const Node* pic, Layer layer, Ctx& c, bool* skip)
{
    Element e;
    e.kind = Element::Picture;
    *skip = false;
    const Node* nv = pic->child(u"nvPicPr");
    if (nv != nullptr && nv->child(u"cNvPr") != nullptr && nv->child(u"cNvPr")->flag(u"hidden")) {
        *skip = true;
        return e;
    }
    const PlaceholderChain ph = placeholderOf(nv != nullptr ? nv->child(u"nvPr") : nullptr, layer, c);
    if (ph.ph != nullptr && layer != Layer::Slide) {
        *skip = true;
        return e;
    }
    const Node* spPr = pic->child(u"spPr");
    const Node* xfrm = spPr != nullptr ? spPr->child(u"xfrm") : nullptr;
    for (const Node* from : {ph.layoutSp, ph.masterSp})
        if (xfrm == nullptr && from != nullptr && from->child(u"spPr") != nullptr) xfrm = from->child(u"spPr")->child(u"xfrm");
    e.rect = xfrmRect(xfrm, &e.rotation, &e.flipH, &e.flipV);
    const QRectF box(0, 0, e.rect.width(), e.rect.height());
    bool known = true;
    const Node* pg = spPr != nullptr ? spPr->child(u"prstGeom") : nullptr;
    if (const Node* cg = spPr != nullptr ? spPr->child(u"custGeom") : nullptr) e.path = custom(cg, box.width(), box.height()).path;
    else e.path = preset(pg != nullptr ? pg->attr(u"prst", QStringLiteral("rect")) : QStringLiteral("rect"), box.width(), box.height(), {}, &known).path;
    const StyleRefs refs = styleRefs(pic->child(u"style"), c);
    e.stroke = refs.line;
    if (spPr != nullptr) applyLine(e.stroke, spPr->child(u"ln"), c);
    e.shadow = shadowOf(refs.effects, c, refs.effectColor);
    if (spPr != nullptr && spPr->child(u"effectLst") != nullptr) e.shadow = shadowOf(spPr->child(u"effectLst"), c);
    if (e.stroke.kind == Stroke::Unset) e.stroke.kind = Stroke::None;
    const Node* blipFill = pic->child(u"blipFill");
    const Node* blip = blipFill != nullptr ? blipFill->child(u"blip") : nullptr;
    const auto r = blip != nullptr ? c.d->pkg.rel(c.part, blip->rel(u"embed")) : std::nullopt;
    if (r && !r->external) e.media = c.d->mediaOf(r->target);
    if (const Node* src = blipFill != nullptr ? blipFill->child(u"srcRect") : nullptr)
        e.crop = QRectF(src->num(u"l") / 100000, src->num(u"t") / 100000, src->num(u"r") / 100000, src->num(u"b") / 100000);
    if (layer == Layer::Slide && c.counting && c.info != nullptr) ++c.info->pictures;
    if (e.media == nullptr || !e.media->drawable()) {
        e.kind = Element::Missing;
        const QString kind = e.media != nullptr ? e.media->kind() : QString();
        if (e.media != nullptr && e.media->tooFar) {
            e.missing = QObject::tr("an SVG picture, not drawn");
            noteMissing(c, QObject::tr("an SVG picture is not drawn: %1 (a box in its place)").arg(qucs_s::svg::tooFarSaid(0)));
        } else {
            e.missing = kind.isEmpty() ? QObject::tr("a picture not in the file") : QObject::tr("a picture in %1").arg(kind);
            noteMissing(c, kind.isEmpty() ? QObject::tr("a picture linked from outside the file, or not in it")
                                          : QObject::tr("pictures in %1, not drawn (a box in their place)").arg(kind));
        }
    }
    return e;
}

Element buildTable(const Node* frame, const Node* tbl, Ctx& c)
{
    Element e;
    e.kind = Element::TableFrame;
    auto table = std::make_shared<Table>();
    const Node* grid = tbl->child(u"tblGrid");
    if (grid != nullptr)
        for (const Node* col : grid->children(u"gridCol")) table->cols << pt(col->num(u"w"));
    const Node* tblPr = tbl->child(u"tblPr");
    const bool firstRow = tblPr != nullptr && tblPr->flag(u"firstRow");
    const bool bandRow = tblPr != nullptr && tblPr->flag(u"bandRow");
    const bool lastRow = tblPr != nullptr && tblPr->flag(u"lastRow");
    const bool firstCol = tblPr != nullptr && tblPr->flag(u"firstCol");
    // The table's style: one of tableStyles.xml, else the built-in it names
    // (Medium Style 2 - Accent 1 when it names none): its whole table, its
    // banded rows, its header and last rows, its first column.
    const QString styleId = tblPr != nullptr && tblPr->child(u"tableStyleId") != nullptr ? tblPr->child(u"tableStyleId")->text.trimmed()
                                                                                         : QStringLiteral("{5C22544A-7EE6-4342-B048-85BDC9FD1C3A}");
    const Node* style = nullptr;
    if (const Node* styles = c.d->pkg.xml(QStringLiteral("ppt/tableStyles.xml")))
        for (const Node* s : styles->children(u"tblStyle"))
            if (s->attr(u"styleId") == styleId) style = s;
    struct Part {
        Fill fill;
        std::optional<QColor> text;
        bool bold = false;
        Stroke inner, outer;
    };
    // (Its captures named: GCC 12 and 13 crash on a long lambda that takes
    // all by reference, d7ae967.)
    const auto partOf = [&style, &styleId, &c](const char* name) {
        Part p;
        if (style == nullptr) {
            // Medium Style 2 - Accent 1, as PowerPoint draws it; another
            // built-in style the same in its accent.
            static const QHash<QString, int> accents{{QStringLiteral("{5C22544A-7EE6-4342-B048-85BDC9FD1C3A}"), 1},
                                                     {QStringLiteral("{21E4AEA4-8DFA-4A89-87EB-49C32662AFE8}"), 2},
                                                     {QStringLiteral("{F5AB1C69-6EDB-4FF4-983F-18BD219EF322}"), 3},
                                                     {QStringLiteral("{00A15C55-8517-42AA-B614-E9B94910E393}"), 4},
                                                     {QStringLiteral("{7DF18680-E054-41AD-8BC1-D1AEF772440D}"), 5},
                                                     {QStringLiteral("{93296810-A885-4BE3-A3E7-6D5BEEA58F35}"), 6}};
            if (styleId == QLatin1String("{2D5ABB26-0587-4C30-8999-92F81FD0307C}") || styleId == QLatin1String("{5940675A-B579-460E-94D1-54222C63F5DA}")) {
                // No Style, No Grid / Table Grid: no fills; the grid's lines.
                if (styleId.startsWith(QLatin1String("{5940"))) {
                    p.inner.kind = p.outer.kind = Stroke::Solid;
                    p.inner.color = p.outer.color = schemeColor(QStringLiteral("tx1"), c, std::nullopt);
                }
                return p;
            }
            const QColor accent = schemeColor(QStringLiteral("accent%1").arg(accents.value(styleId, 1)), c, std::nullopt);
            const auto tinted = [&accent](qreal t) { return mixed(accent, t, true); };
            p.inner.kind = Stroke::Solid;
            p.inner.width = 1;
            p.inner.color = schemeColor(QStringLiteral("lt1"), c, std::nullopt);
            p.outer = p.inner;
            const QString n = QLatin1String(name);
            p.fill.kind = Fill::Solid;
            if (n == QLatin1String("wholeTbl")) p.fill.color = tinted(0.2);
            else if (n == QLatin1String("band1H")) p.fill.color = tinted(0.4);
            else if (n == QLatin1String("firstRow") || n == QLatin1String("lastRow")) {
                p.fill.color = accent;
                p.text = schemeColor(QStringLiteral("lt1"), c, std::nullopt);
                p.bold = true;
            } else if (n == QLatin1String("firstCol")) {
                p.fill.color = accent;
                p.text = schemeColor(QStringLiteral("lt1"), c, std::nullopt);
                p.bold = true;
            } else {
                p.fill.kind = Fill::Unset;
            }
            return p;
        }
        const Node* part = style->child(QLatin1String(name));
        if (part == nullptr) return p;
        if (const Node* tc = part->child(u"tcStyle")) {
            if (const Node* f = tc->child(u"fill")) p.fill = fillOf(f, c);
            if (const Node* b = tc->child(u"tcBdr"))
                for (const auto& side : b->kids) {
                    Stroke s;
                    s.kind = Stroke::Solid;
                    applyLine(s, side->child(u"ln"), c);
                    if (side->name == QLatin1String("insideH") || side->name == QLatin1String("insideV")) p.inner = s;
                    else p.outer = s;
                }
        }
        if (const Node* tx = part->child(u"tcTxStyle")) {
            p.text = colorOf(tx, c);
            p.bold = tx->attr(u"b") == QLatin1String("on");
        }
        return p;
    };
    const Part whole = partOf("wholeTbl"), band = partOf("band1H"), header = partOf("firstRow"), last = partOf("lastRow"), firstColumn = partOf("firstCol");
    const QList<const Node*> rows = tbl->children(u"tr");
    QStringList said;
    for (int r = 0; r < rows.size(); ++r) {
        table->rows << pt(rows.at(r)->num(u"h"));
        QStringList cellsSaid;
        int col = 0;
        for (const Node* tc : rows.at(r)->children(u"tc")) {
            Cell cell;
            cell.row = r;
            cell.col = col++;
            cell.colSpan = tc->whole(u"gridSpan", 1, 1, 16384);
            cell.rowSpan = tc->whole(u"rowSpan", 1, 1, 16384);
            cell.covered = tc->flag(u"hMerge") || tc->flag(u"vMerge");
            // The style's parts that apply, in their order.
            QList<const Part*> parts{&whole};
            const bool isHeader = firstRow && r == 0, isLast = lastRow && r == rows.size() - 1;
            if (bandRow && !isHeader && !isLast && ((r - (firstRow ? 1 : 0)) % 2 == 0)) parts << &band;
            if (firstCol && cell.col == 0 && !isHeader) parts << &firstColumn;
            if (isHeader) parts << &header;
            if (isLast) parts << &last;
            std::optional<QColor> textColor;
            bool bold = false;
            for (const Part* p : parts) {
                if (p->fill.kind != Fill::Unset) cell.fill = p->fill;
                if (p->text) textColor = p->text;
                bold = bold || p->bold;
            }
            cell.left = cell.right = cell.top = cell.bottom = whole.inner;
            if (r == 0) cell.top = whole.outer;
            if (cell.col == 0) cell.left = whole.outer;
            const Node* tcPr = tc->child(u"tcPr");
            if (tcPr != nullptr) {
                const Fill f = fillOf(tcPr, c);
                if (f.kind != Fill::Unset) cell.fill = f;
                const std::pair<const char*, Stroke*> sides[] = {{"lnL", &cell.left}, {"lnR", &cell.right}, {"lnT", &cell.top}, {"lnB", &cell.bottom}};
                for (const auto& [name, side] : sides)
                    applyLine(*side, tcPr->child(QLatin1String(name)), c);
            }
            if (const Node* tx = tc->child(u"txBody")) {
                TextChain chain;
                chain.lists << c.defaultTextStyle;
                chain.fontColor = textColor;
                QStringList cellText;
                chain.paragraphs = c.counting ? &cellText : nullptr;
                auto frame = buildText(tx, chain, c);
                if (bold)
                    for (TextPara& p : frame->paras)
                        for (TextRun& run : p.runs)
                            if (!tx->descendant(u"rPr") || !tx->descendant(u"rPr")->has(u"b")) {
                                QFont f = run.format.font();
                                f.setBold(true);
                                run.format.setFont(f);
                            }
                frame->l = tcPr != nullptr && tcPr->has(u"marL") ? pt(tcPr->num(u"marL")) : 7.2;
                frame->r = tcPr != nullptr && tcPr->has(u"marR") ? pt(tcPr->num(u"marR")) : 7.2;
                frame->t = tcPr != nullptr && tcPr->has(u"marT") ? pt(tcPr->num(u"marT")) : 3.6;
                frame->b = tcPr != nullptr && tcPr->has(u"marB") ? pt(tcPr->num(u"marB")) : 3.6;
                frame->anchor = tcPr != nullptr ? tcPr->attr(u"anchor", QStringLiteral("t")) : QStringLiteral("t");
                frame->wrap = true;
                cell.text = frame;
                if (!cell.covered) cellsSaid << cellText.join(QLatin1Char(' '));
            }
            table->cells << cell;
        }
        if (!cellsSaid.isEmpty() && c.counting) said << cellsSaid.join(QStringLiteral(" | "));
    }
    e.table = table;
    const Node* xfrm = frame->child(u"xfrm");
    e.rect = xfrmRect(xfrm);
    if (c.counting && c.info != nullptr) {
        ++c.info->tables;
        c.info->paragraphs << said;
    }
    return e;
}

void buildFrame(const Node* frame, Layer layer, Ctx& c, QList<Element>& out)
{
    const Node* nv = frame->child(u"nvGraphicFramePr");
    if (nv != nullptr && nv->child(u"cNvPr") != nullptr && nv->child(u"cNvPr")->flag(u"hidden")) return;
    const Node* data = frame->at({"graphic", "graphicData"});
    if (data == nullptr) return;
    const QString uri = data->attr(u"uri");
    const QRectF rect = xfrmRect(frame->child(u"xfrm"));
    if (const Node* tbl = data->child(u"tbl")) {
        out << buildTable(frame, tbl, c);
        return;
    }
    if (uri.endsWith(QLatin1String("/chart"))) {
        Element e;
        e.kind = Element::ChartFrame;
        e.rect = rect;
        const Node* ref = data->child(u"chart");
        const auto r = ref != nullptr ? c.d->pkg.rel(c.part, ref->rel(u"id")) : std::nullopt;
        e.chart = readChart(r ? c.d->pkg.xml(r->target) : nullptr, c);
        if (layer == Layer::Slide && c.counting && c.info != nullptr) {
            ++c.info->charts;
            if (e.chart->hasTitle && !e.chart->title.isEmpty()) c.info->paragraphs << QObject::tr("Chart: %1").arg(e.chart->title);
            static const QSet<QString> drawn{QStringLiteral("scatter"), QStringLiteral("line"), QStringLiteral("bar"), QStringLiteral("area"),
                                             QStringLiteral("pie"), QStringLiteral("doughnut")};
            for (const ChartPlot& p : e.chart->plots)
                if (!drawn.contains(p.kind)) noteMissing(c, QObject::tr("a chart of the kind %1, not drawn").arg(p.kind));
        }
        out << e;
        return;
    }
    if (uri.endsWith(QLatin1String("/diagram"))) {
        // SmartArt: the drawing PowerPoint keeps of it (its shapes), as a
        // group in the frame.
        const Node* ids = data->child(u"relIds");
        QString drawing;
        if (ids != nullptr) {
            const auto dm = c.d->pkg.rel(c.part, ids->rel(u"dm"));
            const Node* model = dm ? c.d->pkg.xml(dm->target) : nullptr;
            const Node* ext = model != nullptr ? model->descendant(u"dataModelExt") : nullptr;
            if (ext != nullptr)
                if (const auto dr = c.d->pkg.rel(c.part, ext->attr(u"relId"))) drawing = dr->target;
        }
        if (drawing.isEmpty())
            if (const auto dr = c.d->pkg.relOfType(c.part, u"/diagramDrawing")) drawing = dr->target;
        const Node* root = drawing.isEmpty() ? nullptr : c.d->pkg.xml(drawing);
        const Node* tree = root != nullptr ? root->child(u"spTree") : nullptr;
        Element g;
        g.kind = Element::Group;
        g.rect = rect;
        g.childSpace = QRectF(0, 0, rect.width(), rect.height());
        if (tree == nullptr) {
            g.kind = Element::Missing;
            g.missing = QObject::tr("a SmartArt diagram");
            noteMissing(c, QObject::tr("a SmartArt diagram without its drawing, not drawn"));
        } else {
            const QString part = c.part;
            c.part = drawing;
            buildTree(tree, Layer::Slide, c, g.children);
            c.part = part;
        }
        out << g;
        return;
    }
    // An OLE object (an equation, a spreadsheet): the picture it shows.
    if (const Node* pic = data->descendant(u"pic")) {
        bool skip = false;
        Element e = buildPicture(pic, layer, c, &skip);
        if (!skip) {
            e.rect = rect;
            e.path = QPainterPath();
            e.path.addRect(QRectF(0, 0, rect.width(), rect.height()));
            out << e;
        }
        return;
    }
    Element e;
    e.kind = Element::Missing;
    e.rect = rect;
    e.missing = QObject::tr("an object");
    noteMissing(c, QObject::tr("an embedded object without a picture, not drawn"));
    out << e;
}

void buildTree(const Node* tree, Layer layer, Ctx& c, QList<Element>& out)
{
    if (tree == nullptr) return;
    for (const auto& k : tree->kids) {
        const QString& n = k->name;
        bool skip = false;
        if (n == QLatin1String("sp") || n == QLatin1String("cxnSp")) {
            Element e = buildShape(k.get(), layer, c, &skip);
            if (!skip) out << e;
        } else if (n == QLatin1String("pic")) {
            Element e = buildPicture(k.get(), layer, c, &skip);
            if (!skip) out << e;
        } else if (n == QLatin1String("grpSp")) {
            const Node* nv = k->child(u"nvGrpSpPr");
            if (nv != nullptr && nv->child(u"cNvPr") != nullptr && nv->child(u"cNvPr")->flag(u"hidden")) continue;
            Element g;
            g.kind = Element::Group;
            const Node* xfrm = k->at({"grpSpPr", "xfrm"});
            g.rect = xfrmRect(xfrm, &g.rotation, &g.flipH, &g.flipV);
            const Node* chOff = xfrm != nullptr ? xfrm->child(u"chOff") : nullptr;
            const Node* chExt = xfrm != nullptr ? xfrm->child(u"chExt") : nullptr;
            g.childSpace = QRectF(chOff != nullptr ? pt(chOff->num(u"x")) : g.rect.x(), chOff != nullptr ? pt(chOff->num(u"y")) : g.rect.y(),
                                  chExt != nullptr ? pt(chExt->num(u"cx")) : g.rect.width(), chExt != nullptr ? pt(chExt->num(u"cy")) : g.rect.height());
            buildTree(k.get(), layer, c, g.children);
            out << g;
        } else if (n == QLatin1String("graphicFrame")) {
            buildFrame(k.get(), layer, c, out);
        }
    }
}

// ---------------------------------------------------------------------------
// Drawing.

QBrush brushOf(const Fill& f, const QRectF& box)
{
    if (f.kind == Fill::Solid) return QBrush(f.color);
    if (f.kind == Fill::Gradient) {
        if (f.radial) {
            QRadialGradient g(box.center(), std::hypot(box.width(), box.height()) / 2);
            // (Office's path gradients run from the middle out; its stops from
            // the outside in.)
            QGradientStops stops;
            for (const QGradientStop& s : f.stops) stops.prepend(QGradientStop(1 - s.first, s.second));
            g.setStops(stops);
            return QBrush(g);
        }
        const qreal a = qDegreesToRadians(f.angle);
        const QPointF d(std::cos(a), std::sin(a));
        const qreal half = (std::abs(box.width() * d.x()) + std::abs(box.height() * d.y())) / 2;
        QLinearGradient g(box.center() - d * half, box.center() + d * half);
        g.setStops(f.stops);
        return QBrush(g);
    }
    return QBrush(Qt::NoBrush);
}

void drawMedia(QPainter& p, const Media& m, const QRectF& target, const QRectF& crop)
{
    m.decode();
    if (m.svg != nullptr) {
        // The part left of it after the crop, stretched into the target.
        const QRectF kept(crop.x(), crop.y(), 1 - crop.x() - crop.width(), 1 - crop.y() - crop.height());
        if (kept.width() <= 0 || kept.height() <= 0) return;
        const QRectF drawn(target.x() - kept.x() / kept.width() * target.width(), target.y() - kept.y() / kept.height() * target.height(),
                           target.width() / kept.width(), target.height() / kept.height());
        p.save();
        p.setClipRect(target, Qt::IntersectClip);
        m.svg->render(&p, drawn);
        p.restore();
        return;
    }
    if (m.image.isNull()) return;
    const QSizeF s = m.image.size();
    QRectF source(s.width() * crop.x(), s.height() * crop.y(), s.width() * (1 - crop.x() - crop.width()), s.height() * (1 - crop.y() - crop.height()));
    QRectF dest = target;
    // A negative crop is a margin: the picture smaller in its box.
    if (source.left() < 0) {
        dest.setLeft(dest.left() + (-source.left()) / source.width() * target.width());
        source.setLeft(0);
    }
    if (source.top() < 0) {
        dest.setTop(dest.top() + (-source.top()) / source.height() * target.height());
        source.setTop(0);
    }
    if (source.right() > s.width()) {
        dest.setRight(dest.right() - (source.right() - s.width()) / source.width() * target.width());
        source.setRight(s.width());
    }
    if (source.bottom() > s.height()) {
        dest.setBottom(dest.bottom() - (source.bottom() - s.height()) / source.height() * target.height());
        source.setBottom(s.height());
    }
    if (source.isEmpty() || dest.isEmpty()) return;
    p.drawImage(dest, m.image, source);
}

void fillPath(QPainter& p, const QPainterPath& path, const Fill& f, const QRectF& box)
{
    if (!f.visible()) return;
    if (f.kind == Fill::Picture && f.media != nullptr) {
        p.save();
        p.setClipPath(path, Qt::IntersectClip);
        f.media->decode();
        if (f.tile && !f.media->image.isNull()) p.fillRect(box, QBrush(f.media->image));
        else drawMedia(p, *f.media, box, QRectF());
        p.restore();
        return;
    }
    p.fillPath(path, brushOf(f, box));
}

/// An arrowhead at \a tip, pointing along \a dir (a unit vector), for a
/// line \a width wide.
void drawArrow(QPainter& p, const Arrow& a, QPointF tip, QPointF dir, const Stroke& s)
{
    if (a.type.isEmpty() || a.type == QLatin1String("none")) return;
    const qreal w = std::max(s.width, 0.75);
    const qreal scales[] = {2, 3, 5};
    const qreal len = scales[std::clamp(a.len, 0, 2)] * w, half = scales[std::clamp(a.w, 0, 2)] * w / 2;
    const QPointF n(-dir.y(), dir.x());
    const QPointF base = tip - dir * len;
    p.save();
    p.setBrush(s.color);
    p.setPen(QPen(s.color, w * 0.5, Qt::SolidLine, Qt::RoundCap, Qt::MiterJoin));
    if (a.type == QLatin1String("oval")) p.drawEllipse(tip - dir * len / 2, len / 2, half);
    else if (a.type == QLatin1String("diamond")) p.drawPolygon(QPolygonF{tip, tip - dir * len / 2 + n * half, tip - dir * len, tip - dir * len / 2 - n * half});
    else if (a.type == QLatin1String("arrow")) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(s.color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(QPolygonF{base + n * half, tip, base - n * half});
    } else if (a.type == QLatin1String("stealth"))
        p.drawPolygon(QPolygonF{tip, base + n * half, tip - dir * len * 0.6, base - n * half});
    else p.drawPolygon(QPolygonF{tip, base + n * half, base - n * half});
    p.restore();
}

void drawStroke(QPainter& p, const Element& e)
{
    if (!e.stroke.visible()) return;
    p.strokePath(e.path, e.stroke.pen());
    if (!e.strokeOnly.isEmpty()) p.strokePath(e.strokeOnly, e.stroke.pen());
    const bool head = !e.stroke.head.type.isEmpty() && e.stroke.head.type != QLatin1String("none");
    const bool tail = !e.stroke.tail.type.isEmpty() && e.stroke.tail.type != QLatin1String("none");
    if (!(head || tail) || e.path.elementCount() < 2) return;
    const auto unit = [](QPointF v) {
        const qreal l = std::hypot(v.x(), v.y());
        return l > 0 ? v / l : QPointF(1, 0);
    };
    if (head) {
        const qreal t = 0.0;
        const QPointF tip = e.path.pointAtPercent(t);
        const QPointF dir = unit(tip - e.path.pointAtPercent(std::min(1.0, 0.02)));
        drawArrow(p, e.stroke.head, tip, dir, e.stroke);
    }
    if (tail) {
        const QPointF tip = e.path.pointAtPercent(1.0);
        const QPointF dir = unit(tip - e.path.pointAtPercent(0.98));
        drawArrow(p, e.stroke.tail, tip, dir, e.stroke);
    }
}

/// \a e's outer shadow, under it: its silhouette - what its fill, its
/// line or its picture covers - in the shadow's colour, scaled, offset and
/// blurred (a box blur three times over, near enough a Gaussian), drawn
/// through an image at the device's resolution.
void drawShadow(QPainter& p, const Element& e, const QRectF& box)
{
    const Shadow& s = e.shadow;
    QPainterPath silhouette;
    if (e.kind == Element::Picture) silhouette = e.path;
    else if (!e.open && e.fill.visible()) silhouette = e.path;
    if (e.stroke.visible()) {
        QPainterPathStroker stroker(e.stroke.pen());
        silhouette = silhouette.united(stroker.createStroke(e.path));
    }
    if (silhouette.isEmpty() || s.color.alpha() == 0) return;
    // Scaled about its alignment's point of the box.
    if (!qFuzzyCompare(s.sx, 1.0) || !qFuzzyCompare(s.sy, 1.0)) {
        const QString& a = s.align;
        const qreal ax = a.endsWith(QLatin1Char('l')) ? box.left() : a.endsWith(QLatin1Char('r')) ? box.right() : box.center().x();
        const qreal ay = a.startsWith(QLatin1Char('t')) ? box.top() : a.startsWith(QLatin1Char('b')) ? box.bottom() : box.center().y();
        QTransform t;
        t.translate(ax, ay);
        t.scale(s.sx, s.sy);
        t.translate(-ax, -ay);
        silhouette = t.map(silhouette);
    }
    // Offset: along its direction on the slide - turned with the shape
    // only where it says so.
    const qreal rad = qDegreesToRadians(s.dir);
    QPointF offset(s.dist * std::cos(rad), s.dist * std::sin(rad));
    const QTransform world = p.worldTransform();
    QTransform linear(world.m11(), world.m12(), world.m21(), world.m22(), 0, 0);
    const qreal scale = std::sqrt(std::abs(linear.determinant()));
    if (scale <= 0) return;
    // (Not turned: on the slide, at the device's scale - the slide's own
    // transform a scale alone.)
    const QPointF deviceOffset = s.rotWithShape ? linear.map(offset) : offset * scale;
    // The silhouette in the device's pixels, room around it for the blur.
    const qreal blur = s.blur * scale;
    QRectF device = world.map(silhouette).boundingRect().adjusted(-blur - 2, -blur - 2, blur + 2, blur + 2);
    // Only as much of it as can be seen: the shadow is drawn in the
    // device's own pixels, so what falls outside the device - or outside
    // what the painter clips to - need not be blurred at all. A blur's
    // margin is kept around what is drawn, so each pixel of it still has
    // its neighbours.
    QRectF visible(0, 0, p.device()->width(), p.device()->height());
    if (p.hasClipping())
        if (const QRectF clip = world.mapRect(p.clipBoundingRect()); clip.isValid()) visible &= clip;
    if (visible.isValid()) device &= visible.translated(-deviceOffset).adjusted(-blur - 2, -blur - 2, blur + 2, blur + 2);
    const QRect area = device.toAlignedRect();
    if (area.width() < 1 || area.height() < 1) return;
    // A mask of more pixels than this is built smaller and drawn scaled up
    // - a blurred shadow shows nothing of it - rather than not drawn at
    // all, which is what a shape larger than the view used to get (bug
    // hunt 2026-10-23 B1: a slide's own shape lost its shadow at an export
    // of 8 pixels a point, and at 800 % in the tab).
    constexpr qint64 kMaxMaskPixels = 16ll * 1000 * 1000;
    const qint64 wanted = qint64(area.width()) * area.height();
    const qreal shrink = wanted > kMaxMaskPixels ? std::sqrt(double(kMaxMaskPixels) / double(wanted)) : 1.0;
    QImage mask(QSizeF(area.width() * shrink, area.height() * shrink).toSize().expandedTo(QSize(1, 1)), QImage::Format_Alpha8);
    mask.fill(0);
    {
        QPainter m(&mask);
        m.setRenderHint(QPainter::Antialiasing);
        m.setTransform(world * QTransform::fromTranslate(-area.left(), -area.top()) * QTransform::fromScale(shrink, shrink));
        m.fillPath(silhouette, Qt::black);
    }
    // Three box blurs, each across and down.
    const int r = std::max(0, int(std::lround(blur * shrink / 2)));
    if (r > 0) {
        const int w = mask.width(), h = mask.height();
        std::vector<int> line(std::max(w, h));
        const auto pass = [&mask, &line, r](int count, int length, bool across) {
            for (int i = 0; i < count; ++i) {
                uchar* base = across ? mask.scanLine(i) : mask.bits() + i;
                const qsizetype step = across ? 1 : mask.bytesPerLine();
                int sum = 0;
                for (int k = -r; k <= r; ++k) sum += base[std::clamp(k, 0, length - 1) * step];
                for (int k = 0; k < length; ++k) {
                    line[k] = sum / (2 * r + 1);
                    sum += base[std::min(k + r + 1, length - 1) * step] - base[std::max(k - r, 0) * step];
                }
                for (int k = 0; k < length; ++k) base[k * step] = uchar(line[k]);
            }
        };
        for (int n = 0; n < 3; ++n) {
            pass(h, w, true);
            pass(w, h, false);
        }
    }
    // In its colour.
    QImage shadow(area.size(), QImage::Format_ARGB32_Premultiplied);
    const QRgb colour = s.color.rgba();
    for (int y = 0; y < shadow.height(); ++y) {
        const uchar* a = mask.constScanLine(y);
        auto* out = reinterpret_cast<QRgb*>(shadow.scanLine(y));
        for (int x = 0; x < shadow.width(); ++x) {
            const int alpha = qAlpha(colour) * a[x] / 255;
            out[x] = qRgba(qRed(colour) * alpha / 255, qGreen(colour) * alpha / 255, qBlue(colour) * alpha / 255, alpha);
        }
    }
    p.save();
    p.resetTransform();
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(QRectF(QPointF(area.topLeft()) + deviceOffset, QSizeF(area.size())), shadow,
                QRectF(QPointF(0, 0), QSizeF(shadow.size())));
    p.restore();
}

void drawTable(QPainter& p, const Element& e)
{
    const Table& t = *e.table;
    if (t.cols.isEmpty() || t.rows.isEmpty()) return;
    // Rows as tall as their text needs, at least as tall as they say.
    QList<qreal> rows = t.rows;
    QList<qreal> xs{0};
    for (qreal w : t.cols) xs << xs.last() + w;
    const auto spanWidth = [&](const Cell& c) {
        const int to = std::min(int(t.cols.size()), c.col + c.colSpan);
        return xs.value(to) - xs.value(std::min(c.col, int(t.cols.size())));
    };
    for (const Cell& c : t.cells)
        if (!c.covered && c.rowSpan == 1 && c.text != nullptr && c.row < rows.size())
            rows[c.row] = std::max(rows[c.row], c.text->height(spanWidth(c)));
    QList<qreal> ys{0};
    for (qreal h : rows) ys << ys.last() + h;
    const auto cellRect = [&](const Cell& c) {
        const int c2 = std::min(int(t.cols.size()), c.col + c.colSpan), r2 = std::min(int(rows.size()), c.row + c.rowSpan);
        const int c1 = std::min(c.col, int(t.cols.size())), r1 = std::min(c.row, int(rows.size()));
        return QRectF(xs.value(c1), ys.value(r1), xs.value(c2) - xs.value(c1), ys.value(r2) - ys.value(r1));
    };
    for (const Cell& c : t.cells) {
        if (c.covered) continue;
        const QRectF r = cellRect(c);
        if (c.fill.visible()) {
            QPainterPath path;
            path.addRect(r);
            fillPath(p, path, c.fill, r);
        }
    }
    for (const Cell& c : t.cells) {
        if (c.covered) continue;
        const QRectF r = cellRect(c);
        if (c.text != nullptr) c.text->paint(p, r);
    }
    for (const Cell& c : t.cells) {
        if (c.covered) continue;
        const QRectF r = cellRect(c);
        const std::pair<const Stroke*, QLineF> sides[] = {{&c.top, QLineF(r.topLeft(), r.topRight())},
                                                           {&c.bottom, QLineF(r.bottomLeft(), r.bottomRight())},
                                                           {&c.left, QLineF(r.topLeft(), r.bottomLeft())},
                                                           {&c.right, QLineF(r.topRight(), r.bottomRight())}};
        for (const auto& [s, line] : sides)
            if (s->visible()) {
                p.setPen(s->pen());
                p.drawLine(line);
            }
    }
}

// --- charts

QString formatted(double v, const QString& code)
{
    if (std::isnan(v)) return QString();
    QString f = code.section(QLatin1Char(';'), 0, 0);
    if (f.isEmpty() || f.compare(QLatin1String("General"), Qt::CaseInsensitive) == 0) {
        if (v != 0 && (std::abs(v) >= 1e7 || std::abs(v) < 1e-4)) return QLocale::c().toString(v, 'E', 2).replace(QStringLiteral("E+0"), QStringLiteral("E+")).replace(QStringLiteral("E-0"), QStringLiteral("E-"));
        QString s = QLocale::c().toString(v, 'f', 10);
        if (s.contains(QLatin1Char('.'))) {
            while (s.endsWith(QLatin1Char('0'))) s.chop(1);
            if (s.endsWith(QLatin1Char('.'))) s.chop(1);
        }
        if (s == QLatin1String("-0")) s = QStringLiteral("0");
        // (Ten digits after the point: a float's dust - 0.30000000000000004.)
        const QString g = QLocale::c().toString(v, 'g', 10);
        return g.size() < s.size() && !g.contains(QLatin1Char('e')) ? g : s;
    }
    f.remove(QRegularExpression(QStringLiteral("\"[^\"]*\"|\\[[^\\]]*\\]|[\\\\_*].")));
    const bool percent = f.contains(QLatin1Char('%'));
    if (percent) v *= 100;
    if (f.contains(QLatin1Char('E'), Qt::CaseInsensitive)) {
        const int dot = int(f.indexOf(QLatin1Char('.')));
        int decimals = 0;
        if (dot >= 0)
            for (int i = dot + 1; i < f.size() && (f.at(i) == QLatin1Char('0') || f.at(i) == QLatin1Char('#')); ++i) ++decimals;
        QString s = QLocale::c().toString(v, 'E', decimals);
        s.replace(QStringLiteral("E+0"), QStringLiteral("E+")).replace(QStringLiteral("E-0"), QStringLiteral("E-"));
        return s;
    }
    const int dot = int(f.indexOf(QLatin1Char('.')));
    int decimals = 0;
    if (dot >= 0)
        for (int i = dot + 1; i < f.size() && (f.at(i) == QLatin1Char('0') || f.at(i) == QLatin1Char('#')); ++i) ++decimals;
    QString s = f.contains(QLatin1Char(',')) ? QLocale(QLocale::English).toString(v, 'f', decimals) : QLocale::c().toString(v, 'f', decimals);
    if (s.startsWith(QLatin1String("-")) && s.mid(1).count(QLatin1Char('0')) + s.mid(1).count(QLatin1Char('.')) == s.size() - 1) s = s.mid(1);
    return percent ? s + QLatin1Char('%') : s;
}

/// An axis's scale: its ends and its ticks.
struct Scale {
    double lo = 0, hi = 1;
    QList<double> ticks;
    bool log = false;
    double base = 10;
    bool reversed = false;
    double fraction(double v) const
    {
        double f;
        if (log) f = v > 0 && lo > 0 ? (std::log(v) - std::log(lo)) / (std::log(hi) - std::log(lo)) : qQNaN();
        else f = (v - lo) / (hi - lo);
        return reversed ? 1 - f : f;
    }
};

/// An axis's scale for values \a lo to \a hi, as PowerPoint's own: from
/// zero when that is near; a little room above the highest (and below the
/// lowest); a step of 1, 2 or 5 times a power of ten, the smallest that
/// keeps to \a most steps (what the axis's length gives its labels); the
/// ends whole steps. What the axis sets itself (min, max, majorUnit, a log
/// base) kept.
Scale scaleOf(double lo, double hi, const ChartAxis* axis, bool fromZero, int most = 5)
{
    Scale s;
    s.reversed = axis != nullptr && axis->reversed;
    if (!std::isfinite(lo) || !std::isfinite(hi)) {
        lo = 0;
        hi = 1;
    }
    if (axis != nullptr && axis->logBase > 1) {
        s.log = true;
        s.base = axis->logBase;
        const double lb = std::log(s.base);
        double a = axis->min ? *axis->min : std::pow(s.base, std::floor(std::log(std::max(lo, 1e-300)) / lb));
        double b = axis->max ? *axis->max : std::pow(s.base, std::ceil(std::log(std::max(hi, 1e-300)) / lb));
        if (!(a > 0)) a = 1;
        if (!(b > a)) b = a * s.base;
        s.lo = a;
        s.hi = b;
        for (double t = std::pow(s.base, std::ceil(std::log(a) / lb - 1e-9)); t <= b * (1 + 1e-9) && s.ticks.size() < 60; t *= s.base) s.ticks << t;
        return s;
    }
    if (fromZero && lo > 0 && lo < hi * 5 / 6) lo = 0;
    if (fromZero && hi < 0 && hi > lo * 5 / 6) hi = 0;
    const bool fixedLo = axis != nullptr && axis->min, fixedHi = axis != nullptr && axis->max;
    if (fixedLo) lo = *axis->min;
    if (fixedHi) hi = *axis->max;
    if (hi <= lo) {
        hi = lo + (lo == 0 ? 1 : std::abs(lo) * 0.1);
        lo = lo - (lo == 0 ? 0 : std::abs(lo) * 0.1);
    }
    // (Room: 4 at the top of 0 to 4 is not the axis's end.)
    const double room = (hi - lo) * 0.05;
    if (!fixedHi && hi > 0) hi += room;
    if (!fixedLo && lo < 0) lo -= room;
    double step = axis != nullptr && axis->major && *axis->major > 0 ? *axis->major : 0;
    if (step <= 0) {
        most = std::max(1, most);
        const double from = std::pow(10, std::floor(std::log10((hi - lo) / most)) - 1);
        step = from;
        for (int i = 0; i < 40; ++i) {
            step = from * std::pow(10, i / 3) * (i % 3 == 0 ? 1 : i % 3 == 1 ? 2 : 5);
            const double a = fixedLo ? lo : std::floor(lo / step + 1e-9) * step, b = fixedHi ? hi : std::ceil(hi / step - 1e-9) * step;
            if ((b - a) / step <= most + 1e-9) break;
        }
    }
    if (!(axis != nullptr && axis->min)) lo = std::floor(lo / step + 1e-9) * step;
    if (!(axis != nullptr && axis->max)) hi = std::ceil(hi / step - 1e-9) * step;
    if (hi <= lo) hi = lo + step;
    s.lo = lo;
    s.hi = hi;
    for (double t = std::ceil(lo / step - 1e-9) * step; t <= hi + step * 1e-9 && s.ticks.size() < 200; t += step) s.ticks << (std::abs(t) < step * 1e-9 ? 0 : t);
    return s;
}

void drawMarker(QPainter& p, const QString& symbol, QPointF at, qreal size, const QColor& fill, const QPen& line)
{
    const qreal r = size / 2;
    p.setPen(line);
    p.setBrush(fill);
    if (symbol == QLatin1String("circle") || symbol == QLatin1String("dot")) p.drawEllipse(at, r, r);
    else if (symbol == QLatin1String("square")) p.drawRect(QRectF(at.x() - r, at.y() - r, size, size));
    else if (symbol == QLatin1String("diamond")) p.drawPolygon(QPolygonF{at + QPointF(0, -r), at + QPointF(r, 0), at + QPointF(0, r), at + QPointF(-r, 0)});
    else if (symbol == QLatin1String("triangle")) p.drawPolygon(QPolygonF{at + QPointF(0, -r), at + QPointF(r, r), at + QPointF(-r, r)});
    else if (symbol == QLatin1String("x") || symbol == QLatin1String("star")) {
        p.drawLine(at + QPointF(-r, -r), at + QPointF(r, r));
        p.drawLine(at + QPointF(-r, r), at + QPointF(r, -r));
        if (symbol == QLatin1String("star")) p.drawLine(at + QPointF(0, -r), at + QPointF(0, r));
    } else if (symbol == QLatin1String("plus")) {
        p.drawLine(at + QPointF(-r, 0), at + QPointF(r, 0));
        p.drawLine(at + QPointF(0, -r), at + QPointF(0, r));
    } else if (symbol == QLatin1String("dash")) p.drawLine(at + QPointF(-r, 0), at + QPointF(r, 0));
    else p.drawEllipse(at, r, r);
}

QPainterPath smoothed(const QList<QPointF>& pts)
{
    QPainterPath path;
    if (pts.isEmpty()) return path;
    path.moveTo(pts.first());
    for (int i = 0; i + 1 < pts.size(); ++i) {
        const QPointF p0 = pts.value(i - 1, pts.at(i)), p1 = pts.at(i), p2 = pts.at(i + 1), p3 = pts.value(i + 2, pts.at(i + 1));
        path.cubicTo(p1 + (p2 - p0) / 6, p2 - (p3 - p1) / 6, p2);
    }
    return path;
}

/// A chart's text's font: its family, a stand-in as wide as the font asked.
QFont chartFont(const Chart& chart)
{
    QFont f(chart.family);
    if (chart.stretch != 100) f.setStretch(chart.stretch);
    return f;
}

void drawChart(QPainter& p, const Chart& chart, const QRectF& box)
{
    p.save();
    p.setClipRect(box.adjusted(-2, -2, 2, 2), Qt::IntersectClip);
    if (chart.area.visible()) {
        QPainterPath path;
        path.addRect(box);
        fillPath(p, path, chart.area, box);
    }
    if (chart.border.visible()) {
        p.setPen(chart.border.pen());
        p.setBrush(Qt::NoBrush);
        p.drawRect(box);
    }
    QFont font = chartFont(chart);
    font.setPointSizeF(chart.fontSize);
    // Its margins near PowerPoint's (some 5 points, whatever its text's size).
    const qreal marginX = std::min(chart.fontSize * 0.8, 7.0), marginY = std::min(chart.fontSize * 0.6, 5.0);
    QRectF area = box.adjusted(marginX, marginY, -marginX, -marginY);
    // The title.
    if (chart.hasTitle && !chart.title.isEmpty()) {
        QFont tf = chartFont(chart);
        tf.setPointSizeF(chart.titleSize);
        p.setFont(tf);
        p.setPen(chart.titleColor);
        const QFontMetricsF fm(tf, pointDevice());
        const QRectF r = fm.boundingRect(area, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, chart.title);
        p.drawText(QRectF(area.left(), area.top(), area.width(), r.height()), Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, chart.title);
        area.setTop(area.top() + r.height() + std::min(chart.fontSize * 0.4, 4.0));
    }
    // Series in order, each with its colour.
    struct Drawn {
        const ChartSeries* s;
        const ChartPlot* plot;
        QColor color;
        bool line, markers;
        QString symbol;
    };
    QList<Drawn> all;
    static const char* symbols[] = {"diamond", "square", "triangle", "x", "star", "circle", "plus", "dash"};
    int index = 0;
    for (const ChartPlot& plot : chart.plots)
        for (const ChartSeries& s : plot.series) {
            Drawn d{&s, &plot, chart.accents.isEmpty() ? QColor(Qt::blue) : chart.accents.at(index % chart.accents.size()), true, false, QString()};
            if (index >= 6 && !chart.accents.isEmpty()) d.color = d.color.darker(100 + 40 * (index / 6));
            if (s.line.kind == Stroke::Solid) d.color = s.line.color;
            else if (s.fill.kind == Fill::Solid && plot.kind != QLatin1String("line") && plot.kind != QLatin1String("scatter")) d.color = s.fill.color;
            const QString style = plot.scatterStyle;
            d.line = plot.kind == QLatin1String("line") || (plot.kind == QLatin1String("scatter") && style != QLatin1String("marker") && style != QLatin1String("none"));
            if (s.line.kind == Stroke::None) d.line = false;
            const bool styleMarkers = plot.kind == QLatin1String("line") || (plot.kind == QLatin1String("scatter") && (style == QLatin1String("lineMarker") || style == QLatin1String("marker") || style == QLatin1String("smoothMarker")));
            d.symbol = s.marker == QLatin1String("auto") ? QLatin1String(symbols[index % 8]) : s.marker;
            d.markers = styleMarkers && d.symbol != QLatin1String("none");
            all << d;
            ++index;
        }
    // The legend.
    const bool pieLike = !chart.plots.isEmpty() && (chart.plots.first().kind == QLatin1String("pie") || chart.plots.first().kind == QLatin1String("doughnut"));
    QList<std::pair<QString, QColor>> entries;
    if (pieLike && !chart.plots.first().series.isEmpty()) {
        const ChartSeries& s = chart.plots.first().series.first();
        for (int i = 0; i < s.y.size(); ++i) {
            QColor col = chart.accents.value(i % std::max(1, int(chart.accents.size())));
            if (s.pointFills.contains(i) && s.pointFills.value(i).kind == Fill::Solid) col = s.pointFills.value(i).color;
            entries << std::make_pair(s.cats.value(i, QString::number(i + 1)), col);
        }
    } else {
        for (const Drawn& d : all) entries << std::make_pair(d.s->name, d.color);
    }
    QFont legendFont = chartFont(chart);
    legendFont.setPointSizeF(chart.legendSize);
    p.setFont(legendFont);
    const QFontMetricsF fm(legendFont, pointDevice());
    if (chart.legend && !entries.isEmpty()) {
        const qreal swatch = chart.legendSize * 1.6, gap = chart.legendSize * 0.5, lineH = fm.height() * 1.2;
        qreal widest = 0;
        for (const auto& [name, col] : entries) widest = std::max(widest, fm.horizontalAdvance(name));
        const auto drawEntry = [&entries, &all, &p, &chart, &fm, pieLike, swatch, gap, lineH](QPointF at, int i) {
            const auto& [name, col] = entries.at(i);
            const qreal y = at.y() + lineH / 2;
            if (!pieLike && i < all.size() && all.at(i).plot->kind != QLatin1String("bar") && all.at(i).plot->kind != QLatin1String("area")) {
                if (all.at(i).line) {
                    p.setPen(QPen(col, std::min(2.5, std::max(1.0, all.at(i).s->line.width))));
                    p.drawLine(QPointF(at.x(), y), QPointF(at.x() + swatch, y));
                }
                if (all.at(i).markers)
                    drawMarker(p, all.at(i).symbol, QPointF(at.x() + swatch / 2, y), std::min(7.0, all.at(i).s->markerSize), col, QPen(col, 0.75));
            } else {
                p.fillRect(QRectF(at.x() + swatch / 4, y - swatch / 4, swatch / 2, swatch / 2), col);
            }
            p.setPen(chart.legendColor);
            p.drawText(QPointF(at.x() + swatch + gap / 2, y + fm.ascent() / 2 - fm.descent() / 2), name);
        };
        if (chart.legendPos == QLatin1String("b") || chart.legendPos == QLatin1String("t")) {
            const qreal each = swatch + gap + widest + gap * 2;
            const int perRow = std::max(1, int(area.width() / each));
            const int lines = int((entries.size() + perRow - 1) / perRow);
            const qreal h = lines * lineH;
            const qreal top = chart.legendPos == QLatin1String("b") ? area.bottom() - h : area.top();
            for (int i = 0; i < entries.size(); ++i) {
                const int row = i / perRow, col = i % perRow;
                const int inRow = std::min(perRow, int(entries.size()) - row * perRow);
                const qreal left = area.left() + (area.width() - inRow * each) / 2;
                drawEntry(QPointF(left + col * each, top + row * lineH), i);
            }
            if (chart.legendPos == QLatin1String("b")) area.setBottom(area.bottom() - h - gap);
            else area.setTop(area.top() + h + gap);
        } else {
            const qreal w = swatch + gap + widest;
            const qreal h = entries.size() * lineH;
            const qreal left = chart.legendPos == QLatin1String("l") ? area.left() : area.right() - w;
            const qreal top = chart.legendPos == QLatin1String("tr") ? area.top() : area.top() + (area.height() - h) / 2;
            for (int i = 0; i < entries.size(); ++i) drawEntry(QPointF(left, top + i * lineH), i);
            if (chart.legendPos == QLatin1String("l")) area.setLeft(area.left() + w + gap * 2);
            else area.setRight(area.right() - w - gap * 2);
        }
    }
    if (chart.plots.isEmpty() || area.width() < 10 || area.height() < 10) {
        p.restore();
        return;
    }
    // Pies.
    if (pieLike) {
        const ChartPlot& plot = chart.plots.first();
        if (plot.series.isEmpty()) {
            p.restore();
            return;
        }
        const ChartSeries& s = plot.series.first();
        double total = 0;
        for (double v : s.y)
            if (v > 0) total += v;
        const qreal d = std::min(area.width(), area.height()) * 0.9;
        const QRectF circle(area.center().x() - d / 2, area.center().y() - d / 2, d, d);
        qreal angle = plot.firstSlice;
        for (int i = 0; i < s.y.size() && total > 0; ++i) {
            if (!(s.y.at(i) > 0)) continue;
            const qreal sweep = s.y.at(i) / total * 360;
            QPainterPath slice;
            slice.moveTo(circle.center());
            arcOf(slice, circle, angle - 90, sweep, false);
            slice.closeSubpath();
            QColor col = chart.accents.value(i % std::max(1, int(chart.accents.size())));
            if (s.pointFills.contains(i) && s.pointFills.value(i).kind == Fill::Solid) col = s.pointFills.value(i).color;
            p.setBrush(col);
            p.setPen(QPen(Qt::white, 1));
            p.drawPath(slice);
            angle += sweep;
        }
        if (plot.kind == QLatin1String("doughnut")) {
            const qreal h = d * std::clamp(plot.hole, 10, 90) / 100.0;
            p.setCompositionMode(QPainter::CompositionMode_Clear);
            p.setBrush(Qt::black);
            p.setPen(Qt::NoPen);
            p.drawEllipse(circle.center(), h / 2, h / 2);
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
            if (chart.area.kind == Fill::Solid) {
                p.setBrush(chart.area.color);
                p.drawEllipse(circle.center(), h / 2, h / 2);
            }
        }
        p.restore();
        return;
    }
    // The axes of the first plot: x (categories, or a scatter's values) and y.
    const ChartPlot& first = chart.plots.first();
    const auto axis = [&](const QString& id) -> const ChartAxis* {
        for (const ChartAxis& a : chart.axes)
            if (a.id == id) return &a;
        return nullptr;
    };
    const ChartAxis* ax = axis(first.axes.value(0));
    const ChartAxis* ay = axis(first.axes.value(1));
    const bool horizontal = first.kind == QLatin1String("bar") && first.horizontal;
    const bool scatter = first.kind == QLatin1String("scatter") || first.kind == QLatin1String("bubble");
    // Data extents.
    double xlo = qInf(), xhi = -qInf(), ylo = qInf(), yhi = -qInf();
    int categories = 0;
    QStringList catNames;
    for (const ChartPlot& plot : chart.plots) {
        const bool stacked = plot.grouping == QLatin1String("stacked");
        const bool percent = plot.grouping == QLatin1String("percentStacked");
        QList<double> pos, neg;
        for (const ChartSeries& s : plot.series) {
            categories = std::max(categories, int(std::max(s.y.size(), s.cats.size())));
            if (catNames.size() < s.cats.size()) catNames = s.cats;
            for (int i = 0; i < s.y.size(); ++i) {
                const double v = s.y.at(i);
                if (std::isnan(v)) continue;
                if (stacked || percent) {
                    while (pos.size() <= i) {
                        pos << 0;
                        neg << 0;
                    }
                    (v >= 0 ? pos[i] : neg[i]) += v;
                } else {
                    if (!(ay && ay->logBase > 1 && v <= 0)) {
                        ylo = std::min(ylo, v);
                        yhi = std::max(yhi, v);
                    }
                }
                if (scatter && i < s.x.size() && !std::isnan(s.x.at(i)) && !(ax && ax->logBase > 1 && s.x.at(i) <= 0)) {
                    xlo = std::min(xlo, s.x.at(i));
                    xhi = std::max(xhi, s.x.at(i));
                }
            }
        }
        if (percent) {
            ylo = std::min(ylo, 0.0);
            yhi = std::max(yhi, 1.0);
        } else
            for (int i = 0; i < pos.size(); ++i) {
                ylo = std::min({ylo, neg[i], 0.0});
                yhi = std::max(yhi, pos[i]);
            }
    }
    const bool percentAxis = first.grouping == QLatin1String("percentStacked");
    // Room for the labels and the titles.
    QFont lf = chartFont(chart);
    lf.setPointSizeF(ay != nullptr ? ay->fontSize : chart.fontSize);
    const QFontMetricsF lm(lf, pointDevice());
    // As many steps as the axis's length gives labels room for (each 1.2
    // lines high, or a label and its gap wide), five at most - as
    // PowerPoint's own: 0 to 4 in a plot of 116 points with labels of 18
    // points, 0 to 6 by 2 (five steps, 1.05 lines each, were too close;
    // three, 1.77 lines, were not).
    const auto steps = [](qreal length, qreal label) { return std::clamp(int(length / std::max(1.0, label)), 1, 5); };
    const qreal bottomGuess = lm.height() * 1.3 + (ax != nullptr && !ax->title.isEmpty() && !ax->deleted ? lm.height() * 1.4 : 0);
    const qreal valueLength = horizontal ? area.width() * 0.8 : area.height() - bottomGuess - lm.height() * 0.5;
    // (The font asked's line spacing, its leading too: the steps PowerPoint's,
    // whatever stands in.)
    const qreal valueLabel = horizontal ? lm.horizontalAdvance(QStringLiteral("0.00")) + lm.height() * 2 : lm.lineSpacing() * chart.lineFactor * 1.2;
    const Scale ys = scaleOf(ylo, yhi, ay, true, steps(valueLength, valueLabel));
    const auto yLabel = [&](double v) { return formatted(v, ay != nullptr ? (percentAxis && ay->format == QLatin1String("General") ? QStringLiteral("0%") : ay->format) : QStringLiteral("General")); };
    const auto xLabel = [&](double v) { return formatted(v, ax != nullptr ? ax->format : QStringLiteral("General")); };
    qreal left = 0, bottom = lm.height() * 1.3;
    const bool yShown = ay == nullptr || (!ay->deleted && ay->labels);
    const bool xShown = ax == nullptr || (!ax->deleted && ax->labels);
    if (yShown) {
        if (horizontal) {
            for (const QString& c : catNames) left = std::max(left, lm.horizontalAdvance(c));
        } else {
            for (double t : ys.ticks) left = std::max(left, lm.horizontalAdvance(yLabel(t)));
        }
        left += lm.height() * 0.5;
    }
    if (!xShown) bottom = lm.height() * 0.3;
    if (ay != nullptr && !ay->title.isEmpty() && !ay->deleted) left += lm.height() * 1.4;
    if (ax != nullptr && !ax->title.isEmpty() && !ax->deleted) bottom += lm.height() * 1.4;
    // A scatter's x axis: its steps for the width left.
    const Scale xs = scatter ? scaleOf(xlo, xhi, ax, true,
                                       steps(area.width() - left - lm.height() * 0.8, lm.horizontalAdvance(formatted(xhi, ax != nullptr ? ax->format : QStringLiteral("General")) + QStringLiteral("0")) + lm.height() * 2))
                             : Scale();
    QRectF plotBox(area.left() + left, area.top() + lm.height() * 0.5, area.width() - left - lm.height() * 0.8, area.height() - bottom - lm.height() * 0.5);
    if (plotBox.width() < 5 || plotBox.height() < 5) {
        p.restore();
        return;
    }
    if (chart.plotArea.visible()) {
        QPainterPath path;
        path.addRect(plotBox);
        fillPath(p, path, chart.plotArea, plotBox);
    }
    // Where a value and a category go.
    const auto yAt = [&](double v) { return plotBox.bottom() - ys.fraction(v) * plotBox.height(); };
    const auto xAt = [&](double v) { return plotBox.left() + xs.fraction(v) * plotBox.width(); };
    const auto band = [&](int i) {
        const qreal n = std::max(1, categories);
        return horizontal ? plotBox.top() + (i + 0.5) * plotBox.height() / n : plotBox.left() + (i + 0.5) * plotBox.width() / n;
    };
    const auto valuePos = [&](double v) { return horizontal ? plotBox.left() + ys.fraction(v) * plotBox.width() : yAt(v); };
    // Gridlines, then the axis lines.
    p.setFont(lf);
    const ChartAxis* va = ay;
    if (va != nullptr && va->grid && va->gridLine.visible()) {
        p.setPen(va->gridLine.pen());
        for (double t : ys.ticks) {
            const qreal v = valuePos(t);
            if (horizontal) p.drawLine(QPointF(v, plotBox.top()), QPointF(v, plotBox.bottom()));
            else p.drawLine(QPointF(plotBox.left(), v), QPointF(plotBox.right(), v));
        }
    }
    if (scatter && ax != nullptr && ax->grid && ax->gridLine.visible()) {
        p.setPen(ax->gridLine.pen());
        for (double t : xs.ticks) p.drawLine(QPointF(xAt(t), plotBox.top()), QPointF(xAt(t), plotBox.bottom()));
    }
    // The series.
    p.setRenderHint(QPainter::Antialiasing);
    for (const ChartPlot& plot : chart.plots) {
        const bool stacked = plot.grouping == QLatin1String("stacked") || plot.grouping == QLatin1String("percentStacked");
        const bool percent = plot.grouping == QLatin1String("percentStacked");
        QList<double> totals;
        if (percent)
            for (const ChartSeries& s : plot.series)
                for (int i = 0; i < s.y.size(); ++i) {
                    while (totals.size() <= i) totals << 0;
                    if (!std::isnan(s.y.at(i))) totals[i] += std::abs(s.y.at(i));
                }
        QList<double> basePos(categories, 0), baseNeg(categories, 0);
        const int n = int(plot.series.size());
        for (int si = 0; si < n; ++si) {
            const ChartSeries& s = plot.series.at(si);
            const Drawn* d = nullptr;
            for (const Drawn& x : all)
                if (x.s == &s) d = &x;
            if (d == nullptr) continue;
            const qreal width = s.line.kind == Stroke::Solid ? std::max(0.5, s.line.width) : 2.25;
            if (plot.kind == QLatin1String("bar")) {
                const qreal bandW = (horizontal ? plotBox.height() : plotBox.width()) / std::max(1, categories);
                const qreal groupW = bandW / (1 + plot.gap / 100.0);
                const int places = stacked ? 1 : n;
                const qreal overlap = std::clamp(plot.overlap, -100, 100) / 100.0;
                const qreal barW = places == 1 ? groupW : groupW / (places - overlap * (places - 1));
                for (int i = 0; i < s.y.size() && i < categories; ++i) {
                    double v = s.y.at(i);
                    if (std::isnan(v)) continue;
                    if (percent && totals.value(i) > 0) v /= totals.at(i);
                    double from = 0, to = v;
                    if (stacked) {
                        double& base = v >= 0 ? basePos[i] : baseNeg[i];
                        from = base;
                        to = base + v;
                        base = to;
                    }
                    const qreal centre = band(i);
                    const qreal start = centre - groupW / 2 + (stacked ? 0 : si * barW * (1 - overlap));
                    const qreal a = valuePos(ys.log ? std::max(from, ys.lo) : from), b = valuePos(to);
                    const QRectF bar = horizontal ? QRectF(QPointF(std::min(a, b), start), QPointF(std::max(a, b), start + barW))
                                                  : QRectF(QPointF(start, std::min(a, b)), QPointF(start + barW, std::max(a, b)));
                    Fill f = s.fill;
                    if (s.pointFills.contains(i)) f = s.pointFills.value(i);
                    if (f.kind == Fill::Unset) {
                        f.kind = Fill::Solid;
                        f.color = plot.vary && n == 1 ? chart.accents.value(i % 6) : d->color;
                    }
                    QPainterPath path;
                    path.addRect(bar);
                    fillPath(p, path, f, bar);
                    if (s.line.visible()) p.strokePath(path, s.line.pen());
                }
                continue;
            }
            QList<QPointF> points;
            for (int i = 0; i < s.y.size(); ++i) {
                double v = s.y.at(i);
                if (std::isnan(v) || (ys.log && v <= 0)) {
                    points << QPointF(qQNaN(), qQNaN());
                    continue;
                }
                if (percent && totals.value(i) > 0) v /= totals.at(i);
                if (stacked && i < basePos.size()) {
                    basePos[i] += v;
                    v = basePos[i];
                }
                const qreal x = scatter ? (i < s.x.size() && !std::isnan(s.x.at(i)) && !(xs.log && s.x.at(i) <= 0) ? xAt(s.x.at(i)) : qQNaN()) : band(i);
                points << QPointF(x, yAt(v));
            }
            if (plot.kind == QLatin1String("area")) {
                QPolygonF poly;
                for (const QPointF& q : points)
                    if (!std::isnan(q.x()) && !std::isnan(q.y())) poly << q;
                if (poly.size() >= 2) {
                    poly << QPointF(poly.last().x(), yAt(std::max(ys.lo, 0.0))) << QPointF(poly.first().x(), yAt(std::max(ys.lo, 0.0)));
                    QColor col = s.fill.kind == Fill::Solid ? s.fill.color : d->color;
                    p.setPen(Qt::NoPen);
                    p.setBrush(col);
                    p.drawPolygon(poly);
                }
                continue;
            }
            if (d->line) {
                QPen pen(d->color, width, s.line.style, Qt::RoundCap, Qt::RoundJoin);
                if (s.line.style == Qt::CustomDashLine) pen.setDashPattern(s.line.dashes);
                p.setPen(pen);
                p.setBrush(Qt::NoBrush);
                // Runs of points between gaps.
                QList<QPointF> run;
                const auto flush = [&] {
                    if (run.size() >= 2) {
                        if (s.smooth || plot.scatterStyle.startsWith(QLatin1String("smooth"))) p.drawPath(smoothed(run));
                        else p.drawPolyline(QPolygonF(run));
                    }
                    run.clear();
                };
                for (const QPointF& q : points) {
                    if (std::isnan(q.x()) || std::isnan(q.y())) flush();
                    else run << q;
                }
                flush();
            }
            if (d->markers) {
                const QColor fill = s.markerFill.kind == Fill::Solid ? s.markerFill.color : d->color;
                const QPen line(s.markerLine.kind == Stroke::Solid ? s.markerLine.color : d->color, 0.75);
                const qreal size = std::clamp(s.markerSize, 2.0, 72.0);
                for (const QPointF& q : points)
                    if (!std::isnan(q.x()) && !std::isnan(q.y()) && plotBox.adjusted(-size, -size, size, size).contains(q))
                        drawMarker(p, d->symbol, q, size, s.markerFill.kind == Fill::None ? QColor(Qt::transparent) : fill, line);
            }
        }
    }
    p.setRenderHint(QPainter::Antialiasing);
    // Axis lines and labels.
    const auto drawAxisLine = [&](const ChartAxis* a, QLineF line) {
        if (a == nullptr || a->deleted || !a->line.visible()) return;
        p.setPen(a->line.pen());
        p.drawLine(line);
    };
    if (horizontal) {
        drawAxisLine(ax, QLineF(plotBox.topLeft(), plotBox.bottomLeft()));
        drawAxisLine(ay, QLineF(plotBox.bottomLeft(), plotBox.bottomRight()));
    } else {
        double zero = ys.log ? ys.lo : std::clamp(0.0, ys.lo, ys.hi);
        drawAxisLine(ax, QLineF(QPointF(plotBox.left(), yAt(zero)), QPointF(plotBox.right(), yAt(zero))));
        drawAxisLine(ay, QLineF(plotBox.topLeft(), plotBox.bottomLeft()));
    }
    if (yShown) {
        p.setPen(ay != nullptr ? ay->textColor : chart.textColor);
        if (horizontal) {
            for (int i = 0; i < catNames.size(); ++i)
                p.drawText(QRectF(area.left(), band(i) - lm.height(), plotBox.left() - area.left() - lm.height() * 0.3, lm.height() * 2),
                           Qt::AlignRight | Qt::AlignVCenter, catNames.at(i));
        } else {
            for (double t : ys.ticks)
                p.drawText(QRectF(area.left(), yAt(t) - lm.height(), plotBox.left() - area.left() - lm.height() * 0.3, lm.height() * 2),
                           Qt::AlignRight | Qt::AlignVCenter, yLabel(t));
        }
    }
    if (xShown) {
        p.setPen(ax != nullptr ? ax->textColor : chart.textColor);
        const qreal top = plotBox.bottom() + lm.height() * 0.2;
        if (horizontal) {
            for (double t : ys.ticks) {
                const QString s = yLabel(t);
                const qreal w = lm.horizontalAdvance(s);
                p.drawText(QPointF(valuePos(t) - w / 2, top + lm.ascent()), s);
            }
        } else if (scatter) {
            for (double t : xs.ticks) {
                const QString s = xLabel(t);
                const qreal w = lm.horizontalAdvance(s);
                p.drawText(QPointF(xAt(t) - w / 2, top + lm.ascent()), s);
            }
        } else {
            // Categories: every one where they fit, else every so many.
            qreal widest = 1;
            for (const QString& c : catNames) widest = std::max(widest, lm.horizontalAdvance(c));
            const qreal each = plotBox.width() / std::max(1, categories);
            const int step = std::max(1, int(std::ceil((widest + lm.height() * 0.5) / std::max(1.0, each))));
            for (int i = 0; i < catNames.size(); i += step) {
                const QString& s = catNames.at(i);
                const qreal w = lm.horizontalAdvance(s);
                p.drawText(QPointF(band(i) - w / 2, top + lm.ascent()), s);
            }
        }
    }
    // Axis titles.
    if (ay != nullptr && !ay->title.isEmpty() && !ay->deleted) {
        p.save();
        p.setPen(ay->textColor);
        p.translate(area.left() + lm.height() * 0.6, plotBox.center().y());
        p.rotate(-90);
        const qreal w = lm.horizontalAdvance(ay->title);
        p.drawText(QPointF(-w / 2, lm.ascent() / 2), ay->title);
        p.restore();
    }
    if (ax != nullptr && !ax->title.isEmpty() && !ax->deleted) {
        p.setPen(ax->textColor);
        const qreal w = lm.horizontalAdvance(ax->title);
        p.drawText(QPointF(plotBox.center().x() - w / 2, area.bottom() - lm.descent()), ax->title);
    }
    p.restore();
}

void paintElement(QPainter& p, const Element& e)
{
    if (e.rect.width() < 0 || e.rect.height() < 0) return;
    p.save();
    p.setTransform(e.transform(), true);
    const QRectF box(0, 0, e.rect.width(), e.rect.height());
    if (e.shadow.on && (e.kind == Element::Shape || e.kind == Element::Picture)) drawShadow(p, e, box);
    switch (e.kind) {
    case Element::Shape:
        fillPath(p, e.path, e.fill, box);
        drawStroke(p, e);
        if (e.text != nullptr) {
            // Text turns with its shape, not flipped: a shape upside down
            // (flipped vertically) has its text turned half way round.
            p.save();
            if (e.flipH || e.flipV) {
                p.translate(box.center());
                p.scale(e.flipH ? -1 : 1, e.flipV ? -1 : 1);
                if (e.flipV) p.rotate(180);
                p.translate(-box.center());
            }
            e.text->paint(p, box);
            p.restore();
        }
        break;
    case Element::Picture:
        if (e.media != nullptr) {
            p.save();
            p.setClipPath(e.path, Qt::IntersectClip);
            drawMedia(p, *e.media, box, e.crop);
            p.restore();
        }
        drawStroke(p, e);
        break;
    case Element::Group: {
        if (e.childSpace.width() > 0 && e.childSpace.height() > 0) {
            p.scale(box.width() / e.childSpace.width(), box.height() / e.childSpace.height());
            p.translate(-e.childSpace.topLeft());
        }
        for (const Element& c : e.children) paintElement(p, c);
        break;
    }
    case Element::TableFrame: drawTable(p, e); break;
    case Element::ChartFrame:
        if (e.chart != nullptr) drawChart(p, *e.chart, box);
        break;
    case Element::Missing: {
        // What is not drawn: a light box with what it is.
        p.setPen(QPen(QColor(150, 150, 150), 0.75, Qt::DashLine));
        p.setBrush(QColor(240, 240, 240, 200));
        p.drawRect(box);
        QFont f(QStringLiteral("Helvetica"));
        f.setPointSizeF(std::clamp(std::min(box.width(), box.height()) / 8, 4.0, 12.0));
        p.setFont(f);
        p.setPen(QColor(110, 110, 110));
        p.drawText(box, Qt::AlignCenter | Qt::TextWordWrap, e.missing);
        break;
    }
    }
    p.restore();
}

// ---------------------------------------------------------------------------

QHash<QString, QString> colorMap(const Node* map)
{
    QHash<QString, QString> m;
    if (map == nullptr) return m;
    for (const QXmlStreamAttribute& a : map->attrs)
        if (a.namespaceUri().isEmpty()) m.insert(a.name().toString(), a.value().toString());
    return m;
}

Fill backgroundOf(const Node* root, const Ctx& c)
{
    const Node* bg = root != nullptr ? root->at({"cSld", "bg"}) : nullptr;
    Fill f;
    if (bg == nullptr) return f;
    if (const Node* pr = bg->child(u"bgPr")) return fillOf(pr, c);
    if (const Node* ref = bg->child(u"bgRef")) {
        const int i = ref->whole(u"idx");
        const std::optional<QColor> ph = colorOf(ref, c);
        const QList<const Node*>& list = i >= 1001 ? c.theme->bgFills : c.theme->fills;
        const int at = i >= 1001 ? i - 1001 : i - 1;
        if (at >= 0 && at < list.size()) {
            f = fillElement(list.at(at), c, ph);
            if (f.kind == Fill::Unset && ph) {
                f.kind = Fill::Solid;
                f.color = *ph;
            }
        } else if (ph) {
            f.kind = Fill::Solid;
            f.color = *ph;
        }
    }
    return f;
}

QString notesOf(const Package& pkg, const QString& slidePart)
{
    const auto rel = pkg.relOfType(slidePart, u"/notesSlide");
    if (!rel) return QString();
    const Node* root = pkg.xml(rel->target);
    const Node* tree = root != nullptr ? root->at({"cSld", "spTree"}) : nullptr;
    if (tree == nullptr) return QString();
    QStringList out;
    std::function<void(const Node*)> walk = [&](const Node* t) {
        for (const auto& k : t->kids) {
            if (k->name == QLatin1String("grpSp")) {
                walk(k.get());
                continue;
            }
            if (k->name != QLatin1String("sp")) continue;
            const Node* ph = k->at({"nvSpPr", "nvPr", "ph"});
            if (ph == nullptr || ph->attr(u"type") != QLatin1String("body")) continue;
            if (const Node* tx = k->child(u"txBody"))
                for (const Node* p : tx->children(u"p")) out << p->allText();
        }
    };
    walk(tree);
    while (!out.isEmpty() && out.last().trimmed().isEmpty()) out.removeLast();
    return out.join(QLatin1Char('\n')).trimmed();
}

} // namespace

// ---------------------------------------------------------------------------

bool isPresentationFile(const QString& name)
{
    static const QStringList suffixes{QStringLiteral("pptx"), QStringLiteral("pptm"), QStringLiteral("ppsx"),
                                      QStringLiteral("ppsm"), QStringLiteral("potx"), QStringLiteral("potm")};
    return suffixes.contains(QFileInfo(name).suffix().toLower());
}

Presentation::Presentation() : d(std::make_unique<Data>()) {}
Presentation::~Presentation() = default;

std::shared_ptr<Presentation> Presentation::read(const QByteArray& bytes, QString* error)
{
    const auto fail = [error](const QString& why) {
        if (error != nullptr) *error = why;
        return std::shared_ptr<Presentation>();
    };
    if (bytes.isEmpty()) return fail(QObject::tr("the file is empty (one kept online, not downloaded?)"));
    QString zipError;
    const QList<zip::Entry> entries = zip::read(bytes, &zipError);
    if (entries.isEmpty()) return fail(zipError.isEmpty() ? QObject::tr("it is not a ZIP archive") : zipError);
    std::shared_ptr<Presentation> pres(new Presentation());
    Data& d = *pres->d;
    for (const zip::Entry& e : entries)
        if (!e.name.endsWith(QLatin1Char('/'))) d.pkg.parts.insert(e.name, e.data);
    // The presentation part, as the package's relationships name it.
    QString main = QStringLiteral("ppt/presentation.xml");
    if (const auto r = d.pkg.relOfType(QString(), u"/officeDocument")) main = r->target;
    const Node* presentation = d.pkg.xml(main);
    if (presentation == nullptr || presentation->name != QLatin1String("presentation"))
        return fail(QObject::tr("it has no presentation in it (a Word document or a workbook?)"));
    if (const Node* size = presentation->child(u"sldSz")) d.size = QSizeF(pt(size->num(u"cx", 9144000)), pt(size->num(u"cy", 6858000)));
    if (d.size.width() <= 0 || d.size.height() <= 0) d.size = QSizeF(720, 540);
    if (const Node* core = d.pkg.xml(QStringLiteral("docProps/core.xml")))
        if (const Node* t = core->child(u"title")) d.title = t->text.trimmed();
    const int firstNumber = presentation->whole(u"firstSlideNum", 1, 0, 100000);
    const Node* defaultText = presentation->child(u"defaultTextStyle");
    QHash<QString, std::shared_ptr<Theme>> themes;
    const QList<const Node*> ids = presentation->child(u"sldIdLst") != nullptr ? presentation->child(u"sldIdLst")->children(u"sldId") : QList<const Node*>();
    for (int i = 0; i < ids.size(); ++i) {
        const auto rel = d.pkg.rel(main, ids.at(i)->rel(u"id"));
        if (!rel || !d.pkg.has(rel->target)) continue;
        const QString slidePart = rel->target;
        Slide slide;
        Ctx c;
        c.d = &d;
        c.defaultTextStyle = defaultText;
        c.number = firstNumber + int(d.slides.size());
        c.info = &slide.info;
        const Node* sld = d.pkg.xml(slidePart);
        if (sld == nullptr) continue;
        slide.info.hidden = sld->has(u"show") && !sld->flag(u"show", true);
        const auto layoutRel = d.pkg.relOfType(slidePart, u"/slideLayout");
        c.layout = layoutRel ? readSheet(d.pkg, layoutRel->target) : Sheet();
        const auto masterRel = layoutRel ? d.pkg.relOfType(layoutRel->target, u"/slideMaster") : std::nullopt;
        c.master = masterRel ? readSheet(d.pkg, masterRel->target) : Sheet();
        const auto themeRel = masterRel ? d.pkg.relOfType(masterRel->target, u"/theme") : std::nullopt;
        const QString themePart = themeRel ? themeRel->target : QString();
        if (!themes.contains(themePart)) themes.insert(themePart, std::make_shared<Theme>(readTheme(d.pkg.xml(themePart))));
        c.theme = themes.value(themePart).get();
        c.txStyles = c.master.root != nullptr ? c.master.root->child(u"txStyles") : nullptr;
        // Colours by the master's map, unless the layout's or the slide's
        // override it.
        c.map = colorMap(c.master.root != nullptr ? c.master.root->child(u"clrMap") : nullptr);
        for (const Node* root : {c.layout.root, static_cast<const Node*>(sld)})
            if (const Node* over = root != nullptr ? root->at({"clrMapOvr", "overrideClrMapping"}) : nullptr) c.map = colorMap(over);
        // The background: the slide's, else the layout's, else the master's.
        for (const auto& [root, part] : {std::make_pair(static_cast<const Node*>(sld), slidePart), std::make_pair(c.layout.root, c.layout.part),
                                         std::make_pair(c.master.root, c.master.part)}) {
            if (slide.background.kind != Fill::Unset || root == nullptr) continue;
            c.part = part;
            slide.background = backgroundOf(root, c);
        }
        if (slide.background.kind == Fill::Unset || slide.background.kind == Fill::None) {
            slide.background.kind = Fill::Solid;
            slide.background.color = schemeColor(QStringLiteral("bg1"), c, std::nullopt);
        }
        // The master's shapes and the layout's (unless one says not), then
        // the slide's own.
        const bool layoutShowsMaster = c.layout.root == nullptr || c.layout.root->flag(u"showMasterSp", true);
        const bool slideShowsMaster = sld->flag(u"showMasterSp", true);
        if (slideShowsMaster && layoutShowsMaster && c.master.tree != nullptr) {
            c.part = c.master.part;
            buildTree(c.master.tree, Layer::Master, c, slide.elements);
        }
        if (slideShowsMaster && c.layout.tree != nullptr) {
            c.part = c.layout.part;
            buildTree(c.layout.tree, Layer::Layout, c, slide.elements);
        }
        c.part = slidePart;
        c.counting = true;
        buildTree(sld->at({"cSld", "spTree"}), Layer::Slide, c, slide.elements);
        if (!c.approximated.isEmpty()) {
            QStringList kinds(c.approximated.cbegin(), c.approximated.cend());
            kinds.sort();
            slide.info.notShown << QObject::tr("shapes of the kinds %1, drawn as rectangles").arg(kinds.join(QStringLiteral(", ")));
        }
        slide.info.notes = notesOf(d.pkg, slidePart);
        d.slides << slide;
    }
    if (!d.pkg.tooDeep().isEmpty())
        return fail(QObject::tr("%1 nests its elements more than %2 deep (PowerPoint's own nest a few dozen)")
                        .arg(d.pkg.tooDeep().first())
                        .arg(kMaxXmlDepth));
    if (d.slides.isEmpty() && !ids.isEmpty()) return fail(QObject::tr("none of its %1 slides could be read").arg(ids.size()));
    return pres;
}

QSizeF Presentation::slideSize() const
{
    return d->size;
}

int Presentation::slideCount() const
{
    return int(d->slides.size());
}

const SlideInfo& Presentation::slide(int index) const
{
    static const SlideInfo none;
    return index >= 0 && index < d->slides.size() ? d->slides.at(index).info : none;
}

QString Presentation::title() const
{
    return d->title;
}

QImage Presentation::render(int index, qreal scale) const
{
    if (index < 0 || index >= d->slides.size() || !(scale > 0)) return QImage();
    const Slide& slide = d->slides.at(index);
    const QSize px(std::max(1, int(std::ceil(d->size.width() * scale))), std::max(1, int(std::ceil(d->size.height() * scale))));
    if (qint64(px.width()) * px.height() > 120ll * 1000 * 1000) return QImage();
    QImage image(px, QImage::Format_ARGB32_Premultiplied);
    // (72 dots an inch while it is drawn: a font's points are units.)
    image.setDotsPerMeterX(qRound(72 / 0.0254));
    image.setDotsPerMeterY(qRound(72 / 0.0254));
    image.fill(Qt::white);
    {
        QPainter p(&image);
        p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
        p.scale(scale, scale);
        const QRectF page(QPointF(0, 0), d->size);
        QPainterPath all;
        all.addRect(page);
        fillPath(p, all, slide.background, page);
        p.setClipRect(page);
        for (const Element& e : slide.elements) paintElement(p, e);
    }
    image.setDotsPerMeterX(qRound(72 * scale / 0.0254));
    image.setDotsPerMeterY(qRound(72 * scale / 0.0254));
    return image;
}

void Presentation::render(int index, QPainter* painter, const QRectF& target) const
{
    if (painter == nullptr || index < 0 || index >= d->slides.size() || target.isEmpty()) return;
    // The slide's aspect, in the middle of the target.
    const qreal fit = std::min(target.width() / d->size.width(), target.height() / d->size.height());
    const QSizeF shown = d->size * fit;
    const QRectF at(target.center().x() - shown.width() / 2, target.center().y() - shown.height() / 2, shown.width(), shown.height());
    // As many pixels as the device has there.
    const QRectF device = painter->deviceTransform().mapRect(at);
    const qreal scale = std::clamp(device.width() / d->size.width(), 0.05, 12.0);
    const QImage image = render(index, scale);
    painter->save();
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    painter->drawImage(at, image);
    painter->restore();
}

QList<std::pair<int, int>> Presentation::find(const QString& text) const
{
    QList<std::pair<int, int>> out;
    const QString wanted = text.simplified();
    if (wanted.isEmpty()) return out;
    for (int i = 0; i < d->slides.size(); ++i) {
        const SlideInfo& s = d->slides.at(i).info;
        int count = 0;
        for (const QString& p : s.paragraphs) count += int(p.count(wanted, Qt::CaseInsensitive));
        count += int(s.notes.count(wanted, Qt::CaseInsensitive));
        if (count > 0) out << std::make_pair(i, count);
    }
    return out;
}

} // namespace qucs_s::pptx
