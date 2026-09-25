/*
 * mathtypeset.cpp - TeX math typeset into an image, and found in Markdown
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "mathtypeset.h"

#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QHash>
#include <QPainter>
#include <QAbstractTextDocumentLayout>
#include <QPainterPath>
#include <QRegularExpression>
#include <QTextDocument>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace qucs_s::math {

namespace {

// ----------------------------------------------------------------------
// Styles, kinds, fonts

// TeX's four styles: display (between $$), text (between $), and the
// sizes of scripts and of scripts on scripts.
enum class Style { Display, Text, Script, ScriptScript };

qreal factorOf(Style s)
{
    switch (s) {
    case Style::Display:
    case Style::Text: return 1.0;
    case Style::Script: return 0.71;
    case Style::ScriptScript: return 0.55;
    }
    return 1.0;
}

Style scriptOf(Style s)
{
    return s <= Style::Text ? Style::Script : Style::ScriptScript;
}

// The numerator's and denominator's style.
Style fractionOf(Style s)
{
    return s == Style::Display ? Style::Text : s == Style::Text ? Style::Script : Style::ScriptScript;
}

// What an atom is, for the space around it (TeX's classes).
enum class Kind { Ord, Op, Bin, Rel, Open, Close, Punct, Inner, None };

// Space between atoms, in mu (1/18 em): 1 thin, 2 medium, 3 thick;
// negative only in display and text style.
int spaceBetween(Kind l, Kind r, Style s)
{
    static const int table[8][8] = {
        //  Ord Op Bin Rel Open Close Punct Inner
        {0, 1, -2, -3, 0, 0, 0, -1},       // Ord
        {1, 1, 0, -3, 0, 0, 0, -1},        // Op
        {-2, -2, 0, 0, -2, 0, 0, -2},      // Bin
        {-3, -3, 0, 0, -3, 0, 0, -3},      // Rel
        {0, 0, 0, 0, 0, 0, 0, 0},          // Open
        {0, 1, -2, -3, 0, 0, 0, -1},       // Close
        {-1, -1, 0, -1, -1, -1, -1, -1},   // Punct
        {-1, 1, -2, -3, -1, 0, -1, -1},    // Inner
    };
    if (l == Kind::None || r == Kind::None) return 0;
    const int v = table[int(l)][int(r)];
    if (v >= 0) return v == 1 ? 3 : v == 2 ? 4 : v == 3 ? 5 : 0;
    if (s >= Style::Script) return 0;
    return v == -1 ? 3 : v == -2 ? 4 : 5;
}

// Symbol: operators, relations, arrows - in the math font, which has them
// all, spaced as they should be.
enum class Variant { Italic, Upright, Bold, BoldItalic, Sans, Mono, Symbol };

// A serif for the letters, as TeX's are; the first there is.
QString serifFamily()
{
    static const QString family = [] {
        const QStringList have = QFontDatabase::families();
        for (const char* f : {"STIX Two Text", "STIX Two Math", "STIXGeneral", "Latin Modern Roman", "Latin Modern Math",
                              "Cambria", "Cambria Math", "Times New Roman", "Times", "Liberation Serif", "Nimbus Roman",
                              "DejaVu Serif", "FreeSerif"})
            if (have.contains(QString::fromLatin1(f))) return QString::fromLatin1(f);
        return QString();
    }();
    return family;
}

// A math font for the symbols, when there is one.
QString mathFamily()
{
    static const QString family = [] {
        const QStringList have = QFontDatabase::families();
        for (const char* f : {"STIX Two Math", "Cambria Math", "Latin Modern Math", "STIXGeneral", "DejaVu Math TeX Gyre",
                              "TeX Gyre Termes Math", "XITS Math"})
            if (have.contains(QString::fromLatin1(f))) return QString::fromLatin1(f);
        return serifFamily();
    }();
    return family;
}

struct Ctx {
    qreal em = 16.0;   // the size of text style, in pixels
    QString serif;
    QString math;
    QFont base;        // the text's own font (for sans and fallbacks)
    qreal dpr = 1.0;
    mutable QHash<int, qreal> axes;

    QFont font(Variant v, Style s, qreal scale = 1.0) const
    {
        QFont f = base;
        if (v == Variant::Mono) {
            f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        } else if (v == Variant::Symbol && !math.isEmpty()) {
            f.setFamily(math);
            f.setFamilies({math});
        } else if (v != Variant::Sans && !serif.isEmpty()) {
            f.setFamily(serif);
            f.setFamilies({serif});
        }
        f.setPixelSize(std::max(1, qRound(em * factorOf(s) * scale)));
        f.setItalic(v == Variant::Italic || v == Variant::BoldItalic);
        f.setBold(v == Variant::Bold || v == Variant::BoldItalic);
        f.setUnderline(false);
        f.setStrikeOut(false);
        f.setKerning(true);
        return f;
    }
    qreal size(Style s) const { return em * factorOf(s); }
    // The line thickness of fraction bars and roots.
    qreal rule(Style s) const { return std::max(size(s) * 0.05, 0.8 / dpr); }
    // The math axis: where a minus sign's middle is, above the baseline.
    qreal axis(Style s) const
    {
        const auto found = axes.constFind(int(s));
        if (found != axes.cend()) return *found;
        const QFontMetricsF fm(font(Variant::Upright, s));
        const QRectF minus = fm.tightBoundingRect(QString(QChar(0x2212)));
        const qreal a = minus.isValid() && minus.height() < fm.height() * 0.5 ? -minus.center().y() : fm.xHeight() * 0.5;
        axes.insert(int(s), a);
        return a;
    }
    qreal xHeight(Style s) const { return QFontMetricsF(font(Variant::Italic, s)).xHeight(); }
};

// ----------------------------------------------------------------------
// Boxes

struct Box {
    Kind kind = Kind::Ord;
    qreal w = 0.0, a = 0.0, d = 0.0;   // width, height above and depth below the baseline
    QColor colour;                     // invalid: as around it
    virtual ~Box() = default;
    virtual void layout(const Ctx& ctx, Style style) = 0;
    virtual void paint(QPainter& p, const Ctx& ctx, qreal x, qreal y) const = 0;
    // Room a slanted letter needs on its right (for a superscript).
    virtual qreal italic() const { return 0.0; }
    // A large operator or a function like lim: limits above and below in
    // display style.
    virtual bool takesLimits() const { return false; }
    // A lone character: scripts sit by its own height.
    virtual bool isChar() const { return false; }

    void draw(QPainter& p, const Ctx& ctx, qreal x, qreal y) const
    {
        if (!colour.isValid()) {
            paint(p, ctx, x, y);
            return;
        }
        p.save();
        p.setPen(QPen(colour));
        p.setBrush(colour);
        paint(p, ctx, x, y);
        p.restore();
    }
};
using Ptr = std::unique_ptr<Box>;

struct Empty : Box {
    void layout(const Ctx&, Style) override { w = a = d = 0.0; }
    void paint(QPainter&, const Ctx&, qreal, qreal) const override {}
};

// Characters in one font: a letter, a number, a symbol, a function's name,
// a text; or a large operator, centred on the axis.
struct Glyphs : Box {
    QString text;
    Variant variant = Variant::Italic;
    bool big = false;        // a large operator (sum, integral)
    bool integral = false;
    bool limits = false;     // limits above and below in display style
    bool single = false;     // one character
    QFont f;
    qreal shift = 0.0;       // down, for a large operator on the axis
    qreal slant = 0.0;

    Glyphs(QString t, Variant v, Kind k) : text(std::move(t)), variant(v) { kind = k; }

    void layout(const Ctx& ctx, Style style) override
    {
        f = ctx.font(variant, style);
        if (big) {
            // A sum is a little taller than the text beside it, and much
            // taller in display; an integral more so.
            const QRectF ink = QFontMetricsF(f).tightBoundingRect(text);
            const qreal want = ctx.size(style)
                               * (integral ? (style == Style::Display ? 2.0 : 1.3) : (style == Style::Display ? 1.45 : 1.02));
            if (ink.height() > 0.0) f.setPixelSize(std::max(1, qRound(f.pixelSize() * want / ink.height())));
        }
        const QFontMetricsF fm(f);
        w = fm.horizontalAdvance(text);
        const QRectF ink = fm.tightBoundingRect(text);
        a = ink.isValid() ? std::max(-ink.top(), 0.0) : 0.0;
        d = ink.isValid() ? std::max(ink.bottom(), 0.0) : 0.0;
        if (!ink.isValid() || text.trimmed().isEmpty()) {
            a = fm.xHeight();
            d = 0.0;
        }
        slant = ink.isValid() ? std::max(0.0, ink.right() - w) : 0.0;
        if (f.italic() && !big) slant = std::max(slant, fm.xHeight() * 0.08);
        shift = 0.0;
        if (big) {
            // The middle of the ink on the axis.
            const qreal target = -ctx.axis(style);
            const qreal middle = (d - a) / 2.0;
            shift = target - middle;
            a -= shift;
            d += shift;
            w += ctx.size(style) * (integral ? 0.08 : 0.04);
        }
    }
    void paint(QPainter& p, const Ctx&, qreal x, qreal y) const override
    {
        p.setFont(f);
        p.drawText(QPointF(x, y + shift), text);
    }
    qreal italic() const override { return big && !integral ? 0.0 : slant; }
    bool takesLimits() const override { return limits; }
    bool isChar() const override { return single && !big; }
};

// Space: \, \quad, ~ (in em of the style it is in).
struct Space : Box {
    qreal ems;
    explicit Space(qreal e) : ems(e) { kind = Kind::None; }
    void layout(const Ctx& ctx, Style style) override
    {
        w = ems * ctx.size(style);
        a = d = 0.0;
    }
    void paint(QPainter&, const Ctx&, qreal, qreal) const override {}
};

// Atoms in a row, with TeX's space between them.
struct HList : Box {
    std::vector<Ptr> items;
    std::vector<qreal> xs;
    int forced = -1;   // a style of its own (\displaystyle, \dfrac)

    void layout(const Ctx& ctx, Style style) override
    {
        const Style s = forced >= 0 ? Style(forced) : style;
        // A binary operator with nothing to join is an ordinary atom:
        // a leading minus, a sign after a relation.
        std::vector<Kind> kinds(items.size(), Kind::None);
        Kind before = Kind::None;
        for (size_t i = 0; i < items.size(); ++i) {
            Kind k = items[i]->kind;
            if (k == Kind::Bin
                && (before == Kind::None || before == Kind::Bin || before == Kind::Op || before == Kind::Rel
                    || before == Kind::Open || before == Kind::Punct))
                k = Kind::Ord;
            if ((k == Kind::Rel || k == Kind::Close || k == Kind::Punct) && before == Kind::Bin)
                for (size_t j = i; j-- > 0;)
                    if (kinds[j] != Kind::None) {
                        kinds[j] = Kind::Ord;
                        break;
                    }
            kinds[i] = k;
            if (k != Kind::None) before = k;
        }
        for (size_t j = kinds.size(); j-- > 0;)
            if (kinds[j] != Kind::None) {
                if (kinds[j] == Kind::Bin) kinds[j] = Kind::Ord;
                break;
            }

        xs.assign(items.size(), 0.0);
        qreal x = 0.0;
        a = d = 0.0;
        Kind previous = Kind::None;
        const qreal mu = ctx.size(s) / 18.0;
        for (size_t i = 0; i < items.size(); ++i) {
            Box& b = *items[i];
            b.layout(ctx, s);
            if (kinds[i] != Kind::None) {
                if (previous != Kind::None) x += spaceBetween(previous, kinds[i], s) * mu;
                previous = kinds[i];
            }
            xs[i] = x;
            x += b.w;
            a = std::max(a, b.a);
            d = std::max(d, b.d);
        }
        w = x;
    }
    void paint(QPainter& p, const Ctx& ctx, qreal x, qreal y) const override
    {
        for (size_t i = 0; i < items.size(); ++i) items[i]->draw(p, ctx, x + xs[i], y);
    }
    qreal italic() const override { return items.empty() ? 0.0 : items.back()->italic(); }
    bool takesLimits() const override { return items.size() == 1 && items.front()->takesLimits(); }
    bool isChar() const override { return items.size() == 1 && items.front()->isChar(); }
};

// A fraction, or a binomial's two lines without the bar.
struct Frac : Box {
    Ptr num, den;
    bool bar = true;
    int forced = -1;
    qreal shiftUp = 0.0, shiftDown = 0.0, pad = 0.0, thickness = 0.0, axisAt = 0.0;

    Frac(Ptr n, Ptr dd) : num(std::move(n)), den(std::move(dd)) { kind = Kind::Inner; }
    void layout(const Ctx& ctx, Style style) override
    {
        const Style s = forced >= 0 ? Style(forced) : style;
        const Style inner = fractionOf(s);
        num->layout(ctx, inner);
        den->layout(ctx, inner);
        const qreal em = ctx.size(s);
        thickness = bar ? ctx.rule(s) : 0.0;
        axisAt = ctx.axis(s);
        const bool display = s == Style::Display;
        if (bar) {
            const qreal gap = display ? 3.0 * thickness : thickness * 1.2;
            shiftUp = std::max(axisAt + thickness / 2 + gap + num->d, display ? 0.68 * em : 0.40 * em);
            shiftDown = std::max(-axisAt + thickness / 2 + gap + den->a, display ? 0.69 * em : 0.35 * em);
        } else {
            shiftUp = display ? 0.68 * em : 0.40 * em;
            shiftDown = display ? 0.69 * em : 0.35 * em;
            const qreal clearance = (shiftUp - num->d) - (den->a - shiftDown);
            const qreal want = (display ? 7.0 : 3.0) * ctx.rule(s);
            if (clearance < want) {
                shiftUp += (want - clearance) / 2;
                shiftDown += (want - clearance) / 2;
            }
        }
        pad = em * 0.12;
        w = std::max(num->w, den->w) + 2 * pad;
        a = shiftUp + num->a;
        d = shiftDown + den->d;
    }
    void paint(QPainter& p, const Ctx& ctx, qreal x, qreal y) const override
    {
        num->draw(p, ctx, x + (w - num->w) / 2, y - shiftUp);
        den->draw(p, ctx, x + (w - den->w) / 2, y + shiftDown);
        if (bar) p.fillRect(QRectF(x + pad * 0.5, y - axisAt - thickness / 2, w - pad, thickness), p.pen().color());
    }
};

// A root: the sign drawn to the body's height, an index in its crook.
struct Sqrt : Box {
    Ptr body, index;
    qreal sign = 0.0, top = 0.0, bottom = 0.0, thickness = 0.0, lead = 0.0, tickAt = 0.0, indexRaise = 0.0;

    void layout(const Ctx& ctx, Style style) override
    {
        body->layout(ctx, style);
        const qreal em = ctx.size(style);
        thickness = ctx.rule(style);
        const qreal gap = thickness + (style == Style::Display ? 0.12 : 0.07) * em;
        top = std::max(body->a, ctx.xHeight(style)) + gap + thickness;
        bottom = std::max(body->d, 0.12 * em);
        const qreal total = top + bottom;
        sign = em * std::min(0.95, 0.52 + 0.1 * std::max(0.0, total / em - 1.0));
        tickAt = std::min(total * 0.55, em * 0.6);   // the tick's height above the bottom
        lead = 0.0;
        indexRaise = 0.0;
        if (index) {
            index->layout(ctx, Style::ScriptScript);
            // Its foot a little above the tick (negative: above the baseline).
            indexRaise = bottom - tickAt - 0.08 * em - index->d;
            lead = std::max(0.0, index->w - sign * 0.45 + 0.05 * em);
        }
        w = lead + sign + body->w + 0.08 * em;
        a = top;
        if (index) a = std::max(a, -indexRaise + index->a);
        d = bottom;
    }
    void paint(QPainter& p, const Ctx& ctx, qreal x, qreal y) const override
    {
        const qreal xs = x + lead;
        const qreal yb = y + bottom;
        const qreal yt = y - top + thickness / 2;
        const QPointF p1(xs, yb - tickAt + thickness);
        const QPointF p2(xs + sign * 0.2, yb - tickAt - thickness * 0.8);
        const QPointF p3(xs + sign * 0.5, yb);
        const QPointF p4(xs + sign, yt);
        const QPointF p5(xs + sign + body->w + (w - lead - sign - body->w) * 0.5, yt);
        const QColor c = p.pen().color();
        p.save();
        p.setRenderHint(QPainter::Antialiasing);
        QPen pen(c, thickness, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin);
        p.setPen(pen);
        p.drawLine(p1, p2);
        pen.setWidthF(thickness * 2.1);
        p.setPen(pen);
        p.drawLine(p2, p3);
        pen.setWidthF(thickness);
        p.setPen(pen);
        QPainterPath up;
        up.moveTo(p3);
        up.lineTo(p4);
        up.lineTo(p5);
        p.setBrush(Qt::NoBrush);
        p.drawPath(up);
        p.restore();
        body->draw(p, ctx, xs + sign, y);
        if (index) index->draw(p, ctx, xs + sign * 0.45 - index->w, y + indexRaise);
    }
};

// Scripts on a base: beside it, or above and below a large operator in
// display style (limits).
struct Scripts : Box {
    Ptr base, sup, sub;
    int limits = -1;   // -1: as the base likes; 0 beside; 1 above and below
    bool stacked = false;
    qreal supX = 0, subX = 0, supY = 0, subY = 0, baseX = 0;

    void layout(const Ctx& ctx, Style style) override
    {
        if (!base) base = std::make_unique<Empty>();
        base->layout(ctx, style);
        const Style ss = scriptOf(style);
        if (sup) sup->layout(ctx, ss);
        if (sub) sub->layout(ctx, ss);
        kind = base->kind;
        const qreal em = ctx.size(style);
        stacked = limits == 1 || (limits == -1 && base->takesLimits() && style == Style::Display);
        if (stacked) {
            const qreal gap = 0.14 * em;
            w = std::max({base->w, sup ? sup->w : 0.0, sub ? sub->w : 0.0});
            baseX = (w - base->w) / 2;
            a = base->a;
            d = base->d;
            if (sup) {
                supX = (w - sup->w) / 2 + base->italic() / 2;
                supY = -(base->a + gap + sup->d);
                a = base->a + gap + sup->d + sup->a;
            }
            if (sub) {
                subX = (w - sub->w) / 2 - base->italic() / 2;
                subY = base->d + gap + sub->a;
                d = base->d + gap + sub->a + sub->d;
            }
            return;
        }
        const qreal ssize = ctx.size(ss);
        const qreal xh = ctx.xHeight(style);
        const bool chr = base->isChar();
        const qreal ic = base->italic();
        baseX = 0.0;
        qreal up = chr ? 0.0 : base->a - 0.39 * ssize;
        qreal down = chr ? 0.0 : base->d + 0.05 * ssize;
        if (sup) {
            up = std::max({up, (style == Style::Display ? 0.41 : 0.36) * em, sup->d + xh / 4});
        }
        if (sub) {
            down = std::max({down, 0.15 * em, sub->a - xh * 0.8});
            if (sup) {
                down = std::max(down, 0.25 * em);
                const qreal gap = (up - sup->d) - (sub->a - down);
                const qreal want = 4.0 * ctx.rule(style);
                if (gap < want) down += want - gap;
            }
        }
        const qreal space = 0.05 * em;
        supX = base->w + ic + (base->kind == Kind::Op && ic == 0.0 ? 0.02 * em : 0.0);
        subX = base->w;
        if (auto* g = dynamic_cast<Glyphs*>(base.get()); g != nullptr && g->integral) {
            // A slanted integral: the upper limit by its top, the lower
            // under its foot.
            supX = base->w * 0.9;
            subX = base->w * 0.45;
        }
        supY = -up;
        subY = down;
        w = std::max(sup ? supX + sup->w : base->w, sub ? subX + sub->w : base->w) + space;
        a = std::max(base->a, sup ? up + sup->a : 0.0);
        d = std::max(base->d, sub ? down + sub->d : 0.0);
    }
    void paint(QPainter& p, const Ctx& ctx, qreal x, qreal y) const override
    {
        base->draw(p, ctx, x + baseX, y);
        if (sup) sup->draw(p, ctx, x + supX, y + supY);
        if (sub) sub->draw(p, ctx, x + subX, y + subY);
    }
    qreal italic() const override { return sup || sub ? 0.0 : base ? base->italic() : 0.0; }
};

// A delimiter drawn to a height: ( [ { | ‖ ⟨ ⌊ ⌈ and their mates. A small
// one is the font's glyph; a tall one is drawn.
qreal delimiterWidth(const QString& which, qreal height, qreal em)
{
    if (which.isEmpty() || which == QLatin1String(".")) return em * 0.12;
    const qreal grow = std::max(0.0, height / em - 1.2);
    if (which == QLatin1String("|")) return em * 0.28;
    if (which == QString(QChar(0x2016))) return em * 0.42;
    if (which == QLatin1String("{") || which == QLatin1String("}")) return em * (0.5 + 0.03 * grow);
    if (which == QLatin1String("[") || which == QLatin1String("]") || which == QString(QChar(0x230A))
        || which == QString(QChar(0x230B)) || which == QString(QChar(0x2308)) || which == QString(QChar(0x2309)))
        return em * (0.36 + 0.02 * grow);
    return em * std::min(0.75, 0.4 + 0.05 * grow);
}

void drawDelimiter(QPainter& p, const Ctx& ctx, Style style, const QString& which, qreal x, qreal top, qreal height,
                   qreal width)
{
    if (which.isEmpty() || which == QLatin1String(".")) return;
    const qreal em = ctx.size(style);
    const QColor c = p.pen().color();
    const qreal mid = top + height / 2;
    const qreal bottom = top + height;
    if (height <= em * 1.22) {
        // The font's own, its ink's middle at the middle asked for.
        const QFont f = ctx.font(Variant::Symbol, style);
        const QFontMetricsF fm(f);
        const QRectF ink = fm.tightBoundingRect(which);
        const qreal gw = fm.horizontalAdvance(which);
        p.setFont(f);
        p.drawText(QPointF(x + (width - gw) / 2, mid - ink.center().y()), which);
        return;
    }
    const qreal t = std::max(ctx.rule(style), em * 0.055 + height * 0.004);
    const qreal inset = width * 0.18;
    const qreal xl = x + inset, xr = x + width - inset;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const auto fill = [&](const QPainterPath& path) {
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawPath(path);
    };
    const auto stroke = [&](const QPainterPath& path, qreal pw) {
        p.setPen(QPen(c, pw, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    };
    const QChar ch = which.at(0);
    if (ch == QLatin1Char('(') || ch == QLatin1Char(')')) {
        // A crescent: thick in the middle, thin at its ends.
        const bool left = ch == QLatin1Char('(');
        const qreal outer = left ? xl : xr, end = left ? xr : xl, sign = left ? 1.0 : -1.0;
        const qreal control = 2 * outer - end;
        const qreal tm = t * 1.5, te = t * 0.35;
        QPainterPath path;
        path.moveTo(end, top);
        path.quadTo(control, mid, end, bottom);
        path.lineTo(end - sign * te, bottom);
        path.quadTo(control + sign * 2 * tm, mid, end - sign * te, top);
        path.closeSubpath();
        fill(path);
    } else if (ch == QLatin1Char('[') || ch == QLatin1Char(']') || ch == QChar(0x230A) || ch == QChar(0x230B)
               || ch == QChar(0x2308) || ch == QChar(0x2309)) {
        const bool left = ch == QLatin1Char('[') || ch == QChar(0x230A) || ch == QChar(0x2308);
        const bool topArm = ch == QLatin1Char('[') || ch == QLatin1Char(']') || ch == QChar(0x2308) || ch == QChar(0x2309);
        const bool bottomArm = ch == QLatin1Char('[') || ch == QLatin1Char(']') || ch == QChar(0x230A) || ch == QChar(0x230B);
        const qreal bar = t * 1.2;
        const qreal xb = left ? xl : xr - bar;
        p.fillRect(QRectF(xb, top, bar, height), c);
        const qreal arm = xr - xl;
        if (topArm) p.fillRect(QRectF(left ? xl : xr - arm, top, arm, t * 0.9), c);
        if (bottomArm) p.fillRect(QRectF(left ? xl : xr - arm, bottom - t * 0.9, arm, t * 0.9), c);
    } else if (ch == QLatin1Char('|') || ch == QChar(0x2223)) {
        p.fillRect(QRectF(x + width / 2 - t / 2, top, t, height), c);
    } else if (ch == QChar(0x2016)) {
        p.fillRect(QRectF(x + width * 0.32 - t / 2, top, t, height), c);
        p.fillRect(QRectF(x + width * 0.68 - t / 2, top, t, height), c);
    } else if (ch == QLatin1Char('{') || ch == QLatin1Char('}')) {
        const bool left = ch == QLatin1Char('{');
        const qreal xm = (xl + xr) / 2;
        const qreal tip = left ? xl : xr, arm = left ? xr : xl;
        const qreal r = std::min(height * 0.1, (xr - xl) * 0.9);
        QPainterPath path;
        path.moveTo(arm, top);
        path.quadTo(xm, top, xm, top + r);
        path.lineTo(xm, mid - r);
        path.quadTo(xm, mid, tip, mid);
        path.quadTo(xm, mid, xm, mid + r);
        path.lineTo(xm, bottom - r);
        path.quadTo(xm, bottom, arm, bottom);
        stroke(path, t * 1.25);
    } else if (ch == QChar(0x27E8) || ch == QChar(0x27E9)) {
        const bool left = ch == QChar(0x27E8);
        QPainterPath path;
        path.moveTo(left ? xr : xl, top);
        path.lineTo(left ? xl : xr, mid);
        path.lineTo(left ? xr : xl, bottom);
        stroke(path, t);
    } else if (ch == QLatin1Char('/') || ch == QLatin1Char('\\')) {
        QPainterPath path;
        path.moveTo(ch == QLatin1Char('/') ? xr : xl, top);
        path.lineTo(ch == QLatin1Char('/') ? xl : xr, bottom);
        stroke(path, t);
    } else {
        // Anything else: the glyph, stretched.
        const QFont f = ctx.font(Variant::Upright, style);
        const QFontMetricsF fm(f);
        const QRectF ink = fm.tightBoundingRect(which);
        if (ink.height() > 0) {
            p.setFont(f);
            p.translate(x + (width - fm.horizontalAdvance(which)) / 2, top);
            p.scale(1.0, height / ink.height());
            p.drawText(QPointF(0, -ink.top()), which);
        }
    }
    p.restore();
}

// \left( ... \right), a matrix's brackets, \big(.
struct Delimited : Box {
    QString left, right;
    Ptr body;
    qreal fixed = 0.0;   // \big and kin: this many em tall; 0: as the body
    qreal height = 0.0, lw = 0.0, rw = 0.0, axisAt = 0.0;
    Style used = Style::Text;

    void layout(const Ctx& ctx, Style style) override
    {
        used = style;
        const qreal em = ctx.size(style);
        axisAt = ctx.axis(style);
        qreal ba = 0.0, bd = 0.0, bw = 0.0;
        if (body) {
            body->layout(ctx, style);
            ba = body->a;
            bd = body->d;
            bw = body->w;
        }
        if (fixed > 0.0) {
            height = fixed * em;
        } else {
            const qreal half = std::max(ba - axisAt, bd + axisAt);
            height = std::max({2 * half * 0.901, 2 * half - 0.5 * em, em * 1.0});
        }
        lw = left.isEmpty() ? 0.0 : delimiterWidth(left, height, em);
        rw = right.isEmpty() ? 0.0 : delimiterWidth(right, height, em);
        w = lw + bw + rw;
        a = std::max(ba, axisAt + height / 2);
        d = std::max(bd, height / 2 - axisAt);
    }
    void paint(QPainter& p, const Ctx& ctx, qreal x, qreal y) const override
    {
        const qreal top = y - axisAt - height / 2;
        drawDelimiter(p, ctx, used, left, x, top, height, lw);
        if (body) body->draw(p, ctx, x + lw, y);
        drawDelimiter(p, ctx, used, right, x + lw + (body ? body->w : 0.0), top, height, rw);
    }
};

// An accent over (or a line under, a box around, a stroke through) a body.
enum class Mark {
    Hat, WideHat, Check, Tilde, WideTilde, Bar, Overline, Vec, RightArrow, LeftArrow, BothArrow, Dot, DDot, DDDot,
    Breve, Acute, Grave, Ring, Underline, Cancel, Boxed, OverBrace, UnderBrace
};

struct Accent : Box {
    Mark mark;
    Ptr body;
    qreal markTop = 0.0, markHeight = 0.0, pad = 0.0, skew = 0.0, thickness = 0.0, bodyTop = 0.0;
    Style used = Style::Text;

    Accent(Mark m, Ptr b) : mark(m), body(std::move(b)) {}
    void layout(const Ctx& ctx, Style style) override
    {
        used = style;
        body->layout(ctx, style);
        kind = body->kind;
        const qreal em = ctx.size(style);
        thickness = ctx.rule(style);
        pad = 0.0;
        skew = body->isChar() ? body->italic() * 0.6 + 0.02 * em : 0.0;
        bodyTop = std::max(body->a, ctx.xHeight(style));
        w = body->w;
        a = body->a;
        d = body->d;
        const qreal gap = 0.1 * em;
        switch (mark) {
        case Mark::Underline:
            d = body->d + gap + thickness;
            return;
        case Mark::UnderBrace:
            d = body->d + gap + 0.3 * em;
            return;
        case Mark::Cancel:
            return;
        case Mark::Boxed:
            pad = 0.22 * em;
            w = body->w + 2 * pad;
            a = body->a + pad;
            d = body->d + pad;
            return;
        case Mark::Hat:
        case Mark::Check: markHeight = 0.16 * em; break;
        case Mark::WideHat: markHeight = std::min(0.3, 0.14 + 0.02 * body->w / em) * em; break;
        case Mark::Tilde:
        case Mark::WideTilde: markHeight = 0.12 * em; break;
        case Mark::Bar:
        case Mark::Overline: markHeight = thickness; break;
        case Mark::Vec:
        case Mark::RightArrow:
        case Mark::LeftArrow:
        case Mark::BothArrow: markHeight = 0.22 * em; break;
        case Mark::Dot:
        case Mark::DDot:
        case Mark::DDDot: markHeight = 0.11 * em; break;
        case Mark::Breve:
        case Mark::Acute:
        case Mark::Grave: markHeight = 0.15 * em; break;
        case Mark::Ring: markHeight = 0.16 * em; break;
        case Mark::OverBrace: markHeight = 0.3 * em; break;
        }
        if (mark == Mark::Overline || mark == Mark::Bar) bodyTop = std::max(body->a, ctx.xHeight(style)) + gap * 0.3;
        markTop = bodyTop + gap + markHeight;
        a = markTop;
        if (mark == Mark::RightArrow || mark == Mark::LeftArrow || mark == Mark::BothArrow)
            w = std::max(body->w, 0.8 * em);
    }
    void paint(QPainter& p, const Ctx& ctx, qreal x, qreal y) const override
    {
        const qreal em = ctx.size(used);
        body->draw(p, ctx, x + pad + (w - 2 * pad - body->w) / 2, y);
        const QColor c = p.pen().color();
        p.save();
        p.setRenderHint(QPainter::Antialiasing);
        QPen pen(c, thickness * 1.1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        const qreal cx = x + w / 2 + skew;
        const qreal yb = y - bodyTop - 0.1 * em;   // the mark's foot
        const qreal yt = y - markTop;              // its top
        const qreal narrow = std::max(0.32 * em, std::min(body->w * 0.9, 0.42 * em));
        switch (mark) {
        case Mark::Hat:
        case Mark::WideHat:
        case Mark::Check: {
            const qreal hw = (mark == Mark::WideHat ? std::max(body->w * 0.95, narrow) : narrow * 0.85) / 2;
            QPainterPath path;
            const bool check = mark == Mark::Check;
            path.moveTo(cx - hw, check ? yt : yb);
            path.lineTo(cx, check ? yb : yt);
            path.lineTo(cx + hw, check ? yt : yb);
            p.drawPath(path);
            break;
        }
        case Mark::Tilde:
        case Mark::WideTilde: {
            const qreal hw = (mark == Mark::WideTilde ? std::max(body->w * 0.95, narrow) : narrow * 0.9) / 2;
            const qreal h = yb - yt;
            QPainterPath path;
            path.moveTo(cx - hw, yb - h * 0.2);
            path.cubicTo(cx - hw * 0.5, yt - h * 0.3, cx + hw * 0.3, yb + h * 0.4, cx + hw, yt + h * 0.1);
            p.drawPath(path);
            break;
        }
        case Mark::Bar:
        case Mark::Overline: {
            const qreal x0 = mark == Mark::Overline ? x : cx - narrow / 2;
            const qreal x1 = mark == Mark::Overline ? x + w : cx + narrow / 2;
            p.fillRect(QRectF(x0, yt, x1 - x0, thickness), c);
            break;
        }
        case Mark::Vec:
        case Mark::RightArrow:
        case Mark::LeftArrow:
        case Mark::BothArrow: {
            const bool wide = mark != Mark::Vec;
            const qreal x0 = wide ? x : cx - narrow / 2, x1 = wide ? x + w : cx + narrow / 2;
            const qreal ym = (yt + yb) / 2, hs = 0.12 * em;
            p.drawLine(QPointF(x0, ym), QPointF(x1, ym));
            if (mark != Mark::LeftArrow) {
                p.drawLine(QPointF(x1 - hs, ym - hs * 0.7), QPointF(x1, ym));
                p.drawLine(QPointF(x1 - hs, ym + hs * 0.7), QPointF(x1, ym));
            }
            if (mark == Mark::LeftArrow || mark == Mark::BothArrow) {
                p.drawLine(QPointF(x0 + hs, ym - hs * 0.7), QPointF(x0, ym));
                p.drawLine(QPointF(x0 + hs, ym + hs * 0.7), QPointF(x0, ym));
            }
            break;
        }
        case Mark::Dot:
        case Mark::DDot:
        case Mark::DDDot: {
            const int n = mark == Mark::Dot ? 1 : mark == Mark::DDot ? 2 : 3;
            const qreal r = 0.05 * em, step = 0.2 * em;
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            for (int i = 0; i < n; ++i)
                p.drawEllipse(QPointF(cx + (i - (n - 1) / 2.0) * step, (yt + yb) / 2), r, r);
            break;
        }
        case Mark::Breve: {
            QPainterPath path;
            path.moveTo(cx - narrow * 0.4, yt);
            path.quadTo(cx, yb + (yb - yt) * 0.6, cx + narrow * 0.4, yt);
            p.drawPath(path);
            break;
        }
        case Mark::Acute: p.drawLine(QPointF(cx - 0.05 * em, yb), QPointF(cx + 0.12 * em, yt)); break;
        case Mark::Grave: p.drawLine(QPointF(cx + 0.05 * em, yb), QPointF(cx - 0.12 * em, yt)); break;
        case Mark::Ring: {
            const qreal r = (yb - yt) / 2;
            p.drawEllipse(QPointF(cx, yt + r), r, r);
            break;
        }
        case Mark::Underline: p.fillRect(QRectF(x, y + body->d + 0.1 * em, w, thickness), c); break;
        case Mark::Cancel: p.drawLine(QPointF(x, y + body->d), QPointF(x + w, y - body->a)); break;
        case Mark::Boxed:
            p.setPen(QPen(c, thickness));
            p.drawRect(QRectF(x + thickness / 2, y - a + thickness / 2, w - thickness, a + d - thickness));
            break;
        case Mark::OverBrace:
        case Mark::UnderBrace: {
            // The brace on its side.
            const bool over = mark == Mark::OverBrace;
            const qreal h = 0.22 * em;
            const qreal y0 = over ? yt + h : y + body->d + 0.1 * em;   // the arms' line
            const qreal tipY = over ? y0 - h : y0 + h;
            const qreal xm = x + w / 2, r = std::min(w * 0.1, h);
            const qreal ym = (y0 + tipY) / 2;
            QPainterPath path;
            path.moveTo(x, y0);
            path.quadTo(x, ym, x + r, ym);
            path.lineTo(xm - r, ym);
            path.quadTo(xm, ym, xm, tipY);
            path.quadTo(xm, ym, xm + r, ym);
            path.lineTo(x + w - r, ym);
            path.quadTo(x + w, ym, x + w, y0);
            p.setPen(QPen(c, thickness * 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPath(path);
            break;
        }
        }
        p.restore();
    }
    bool takesLimits() const override { return mark == Mark::OverBrace || mark == Mark::UnderBrace; }
    qreal italic() const override { return mark == Mark::Boxed ? 0.0 : body->italic(); }
};

// Something over and/or under a body: \overset, \underset, \stackrel.
struct OverUnder : Box {
    Ptr body, over, under;
    qreal overY = 0, underY = 0;

    void layout(const Ctx& ctx, Style style) override
    {
        body->layout(ctx, style);
        kind = body->kind;
        const qreal gap = 0.12 * ctx.size(style);
        w = body->w;
        a = body->a;
        d = body->d;
        if (over) {
            over->layout(ctx, scriptOf(style));
            w = std::max(w, over->w);
            overY = -(body->a + gap + over->d);
            a = body->a + gap + over->d + over->a;
        }
        if (under) {
            under->layout(ctx, scriptOf(style));
            w = std::max(w, under->w);
            underY = body->d + gap + under->a;
            d = underY + under->d;
        }
    }
    void paint(QPainter& p, const Ctx& ctx, qreal x, qreal y) const override
    {
        body->draw(p, ctx, x + (w - body->w) / 2, y);
        if (over) over->draw(p, ctx, x + (w - over->w) / 2, y + overY);
        if (under) under->draw(p, ctx, x + (w - under->w) / 2, y + underY);
    }
};

// An arrow as long as the words over it: \xrightarrow{...}.
struct XArrow : Box {
    Ptr over, under;
    bool leftward = false;
    qreal axisAt = 0.0, thickness = 0.0, em = 0.0;

    void layout(const Ctx& ctx, Style style) override
    {
        kind = Kind::Rel;
        em = ctx.size(style);
        axisAt = ctx.axis(style);
        thickness = ctx.rule(style);
        const Style ss = scriptOf(style);
        qreal label = 0.0;
        if (over) {
            over->layout(ctx, ss);
            label = over->w;
        }
        if (under) {
            under->layout(ctx, ss);
            label = std::max(label, under->w);
        }
        w = std::max(label + 0.7 * em, 1.4 * em);
        a = axisAt + 0.18 * em + (over ? 0.1 * em + over->d + over->a : 0.0);
        d = std::max(0.0, -axisAt + 0.18 * em + (under ? 0.1 * em + under->a + under->d : 0.0));
    }
    void paint(QPainter& p, const Ctx& ctx, qreal x, qreal y) const override
    {
        const qreal ym = y - axisAt;
        const qreal hs = 0.14 * em;
        p.save();
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(p.pen().color(), thickness * 1.1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawLine(QPointF(x + 0.05 * em, ym), QPointF(x + w - 0.05 * em, ym));
        const qreal tip = leftward ? x + 0.05 * em : x + w - 0.05 * em;
        const qreal back = leftward ? tip + hs : tip - hs;
        p.drawLine(QPointF(back, ym - hs * 0.7), QPointF(tip, ym));
        p.drawLine(QPointF(back, ym + hs * 0.7), QPointF(tip, ym));
        p.restore();
        if (over) over->draw(p, ctx, x + (w - over->w) / 2, ym - 0.18 * em - 0.1 * em - over->d);
        if (under) under->draw(p, ctx, x + (w - under->w) / 2, ym + 0.18 * em + 0.1 * em + under->a);
    }
};

// A box with the size of its body, drawn or not (\phantom).
struct Phantom : Box {
    Ptr body;
    bool width = true, height = true;
    void layout(const Ctx& ctx, Style style) override
    {
        body->layout(ctx, style);
        w = width ? body->w : 0.0;
        a = height ? body->a : 0.0;
        d = height ? body->d : 0.0;
    }
    void paint(QPainter&, const Ctx&, qreal, qreal) const override {}
};

// Rows and columns: matrices, cases, aligned equations, lines of display
// math.
struct Matrix : Box {
    std::vector<std::vector<Ptr>> rows;
    QString align;           // a letter per column: l, c, r; the last repeats
    bool pairs = false;      // aligned: r l, r l ... with room between the pairs
    int cellStyle = -1;      // -1: text style for display (as matrices are); else this
    bool keepStyle = false;  // aligned, gathered: the style they are in
    qreal colGap = 1.0;      // em
    qreal rowGap = 0.3;      // em
    std::vector<qreal> widths, rowA, rowD;
    qreal axisAt = 0.0;
    Style used = Style::Text;

    QChar alignOf(size_t col) const
    {
        if (pairs) return col % 2 == 0 ? QLatin1Char('r') : QLatin1Char('l');
        if (align.isEmpty()) return QLatin1Char('c');
        return align.at(int(std::min<size_t>(col, size_t(align.size()) - 1)));
    }
    qreal gapAfter(size_t col, qreal em) const
    {
        if (pairs) return col % 2 == 0 ? 0.0 : 2.0 * em;
        return colGap * em;
    }
    void layout(const Ctx& ctx, Style style) override
    {
        Style s = style;
        if (cellStyle >= 0) s = Style(cellStyle);
        else if (!keepStyle && style == Style::Display) s = Style::Text;
        used = s;
        const qreal em = ctx.size(s);
        axisAt = ctx.axis(style);
        size_t cols = 0;
        for (const auto& row : rows) cols = std::max(cols, row.size());
        widths.assign(cols, 0.0);
        rowA.assign(rows.size(), 0.0);
        rowD.assign(rows.size(), 0.0);
        for (size_t r = 0; r < rows.size(); ++r) {
            // A strut: rows of letters without ascenders keep their height.
            rowA[r] = 0.7 * em;
            rowD[r] = 0.28 * em;
            for (size_t c = 0; c < rows[r].size(); ++c) {
                Box& cell = *rows[r][c];
                cell.layout(ctx, s);
                widths[c] = std::max(widths[c], cell.w);
                rowA[r] = std::max(rowA[r], cell.a);
                rowD[r] = std::max(rowD[r], cell.d);
            }
        }
        w = 0.0;
        for (size_t c = 0; c < cols; ++c) w += widths[c] + (c + 1 < cols ? gapAfter(c, em) : 0.0);
        qreal h = 0.0;
        for (size_t r = 0; r < rows.size(); ++r) h += rowA[r] + rowD[r] + (r + 1 < rows.size() ? rowGap * em : 0.0);
        a = h / 2 + axisAt;
        d = h / 2 - axisAt;
    }
    void paint(QPainter& p, const Ctx& ctx, qreal x, qreal y) const override
    {
        const qreal em = ctx.size(used);
        qreal top = y - a;
        for (size_t r = 0; r < rows.size(); ++r) {
            const qreal baseline = top + rowA[r];
            qreal cx = x;
            for (size_t c = 0; c < widths.size(); ++c) {
                if (c < rows[r].size()) {
                    const Box& cell = *rows[r][c];
                    const QChar al = alignOf(c);
                    const qreal off = al == QLatin1Char('l') ? 0.0 : al == QLatin1Char('r') ? widths[c] - cell.w : (widths[c] - cell.w) / 2;
                    cell.draw(p, ctx, cx + off, baseline);
                }
                cx += widths[c] + gapAfter(c, em);
            }
            top += rowA[r] + rowD[r] + rowGap * em;
        }
    }
};

// A style for what it holds: \displaystyle, \dfrac's parts.
struct Styled : Box {
    Ptr body;
    Style style;
    Styled(Ptr b, Style s) : body(std::move(b)), style(s) {}
    void layout(const Ctx& ctx, Style) override
    {
        body->layout(ctx, style);
        kind = body->kind;
        w = body->w;
        a = body->a;
        d = body->d;
    }
    void paint(QPainter& p, const Ctx& ctx, qreal x, qreal y) const override { body->draw(p, ctx, x, y); }
    qreal italic() const override { return body->italic(); }
};

// ----------------------------------------------------------------------
// What the commands are

struct Symbol {
    const char* name;
    const char* text;   // UTF-8
    Kind kind;
};

// Lower-case Greek is slanted, as TeX sets it.
const Symbol kGreek[] = {
    {"alpha", "α", Kind::Ord},    {"beta", "β", Kind::Ord},      {"gamma", "γ", Kind::Ord},
    {"delta", "δ", Kind::Ord},    {"epsilon", "ϵ", Kind::Ord},   {"varepsilon", "ε", Kind::Ord},
    {"zeta", "ζ", Kind::Ord},     {"eta", "η", Kind::Ord},       {"theta", "θ", Kind::Ord},
    {"vartheta", "ϑ", Kind::Ord}, {"iota", "ι", Kind::Ord},      {"kappa", "κ", Kind::Ord},
    {"varkappa", "ϰ", Kind::Ord}, {"lambda", "λ", Kind::Ord},    {"mu", "μ", Kind::Ord},
    {"nu", "ν", Kind::Ord},       {"xi", "ξ", Kind::Ord},        {"omicron", "ο", Kind::Ord},
    {"pi", "π", Kind::Ord},       {"varpi", "ϖ", Kind::Ord},     {"rho", "ρ", Kind::Ord},
    {"varrho", "ϱ", Kind::Ord},   {"sigma", "σ", Kind::Ord},     {"varsigma", "ς", Kind::Ord},
    {"tau", "τ", Kind::Ord},      {"upsilon", "υ", Kind::Ord},   {"phi", "ϕ", Kind::Ord},
    {"varphi", "φ", Kind::Ord},   {"chi", "χ", Kind::Ord},       {"psi", "ψ", Kind::Ord},
    {"omega", "ω", Kind::Ord},    {"digamma", "ϝ", Kind::Ord},
};

const Symbol kSymbols[] = {
    // Upper-case Greek, upright.
    {"Gamma", "Γ", Kind::Ord}, {"Delta", "Δ", Kind::Ord}, {"Theta", "Θ", Kind::Ord}, {"Lambda", "Λ", Kind::Ord},
    {"Xi", "Ξ", Kind::Ord}, {"Pi", "Π", Kind::Ord}, {"Sigma", "Σ", Kind::Ord}, {"Upsilon", "Υ", Kind::Ord},
    {"Phi", "Φ", Kind::Ord}, {"Psi", "Ψ", Kind::Ord}, {"Omega", "Ω", Kind::Ord},
    // Ordinary symbols.
    {"infty", "∞", Kind::Ord}, {"partial", "∂", Kind::Ord}, {"nabla", "∇", Kind::Ord}, {"forall", "∀", Kind::Ord},
    {"exists", "∃", Kind::Ord}, {"nexists", "∄", Kind::Ord}, {"emptyset", "∅", Kind::Ord}, {"varnothing", "∅", Kind::Ord},
    {"neg", "¬", Kind::Ord}, {"lnot", "¬", Kind::Ord}, {"angle", "∠", Kind::Ord}, {"measuredangle", "∡", Kind::Ord},
    {"triangle", "△", Kind::Ord}, {"square", "□", Kind::Ord}, {"Box", "□", Kind::Ord}, {"blacksquare", "■", Kind::Ord},
    {"Diamond", "◇", Kind::Ord}, {"clubsuit", "♣", Kind::Ord}, {"diamondsuit", "♢", Kind::Ord},
    {"heartsuit", "♡", Kind::Ord}, {"spadesuit", "♠", Kind::Ord}, {"flat", "♭", Kind::Ord}, {"natural", "♮", Kind::Ord},
    {"sharp", "♯", Kind::Ord}, {"aleph", "ℵ", Kind::Ord}, {"beth", "ℶ", Kind::Ord}, {"hbar", "ℏ", Kind::Ord},
    {"hslash", "ℏ", Kind::Ord}, {"ell", "ℓ", Kind::Ord}, {"wp", "℘", Kind::Ord}, {"Re", "ℜ", Kind::Ord},
    {"Im", "ℑ", Kind::Ord}, {"imath", "ı", Kind::Ord}, {"jmath", "ȷ", Kind::Ord}, {"prime", "′", Kind::Ord},
    {"top", "⊤", Kind::Ord}, {"bot", "⊥", Kind::Ord}, {"degree", "°", Kind::Ord}, {"checkmark", "✓", Kind::Ord},
    {"S", "§", Kind::Ord}, {"P", "¶", Kind::Ord}, {"copyright", "©", Kind::Ord}, {"pounds", "£", Kind::Ord},
    {"euro", "€", Kind::Ord}, {"yen", "¥", Kind::Ord}, {"backslash", "\\", Kind::Ord}, {"vert", "|", Kind::Ord},
    {"Vert", "‖", Kind::Ord}, {"surd", "√", Kind::Ord}, {"mho", "℧", Kind::Ord}, {"eth", "ð", Kind::Ord},
    {"complement", "∁", Kind::Ord}, {"circledR", "®", Kind::Ord}, {"vdots", "⋮", Kind::Ord}, {"ohm", "Ω", Kind::Ord},
    {"micro", "µ", Kind::Ord}, {"textdegree", "°", Kind::Ord}, {"dag", "†", Kind::Ord}, {"ddag", "‡", Kind::Ord},
    // Binary operators.
    {"pm", "±", Kind::Bin}, {"mp", "∓", Kind::Bin}, {"times", "×", Kind::Bin}, {"div", "÷", Kind::Bin},
    {"cdot", "⋅", Kind::Bin}, {"centerdot", "⋅", Kind::Bin}, {"ast", "∗", Kind::Bin}, {"star", "⋆", Kind::Bin},
    {"circ", "∘", Kind::Bin}, {"bullet", "∙", Kind::Bin}, {"oplus", "⊕", Kind::Bin}, {"ominus", "⊖", Kind::Bin},
    {"otimes", "⊗", Kind::Bin}, {"oslash", "⊘", Kind::Bin}, {"odot", "⊙", Kind::Bin}, {"cup", "∪", Kind::Bin},
    {"cap", "∩", Kind::Bin}, {"sqcup", "⊔", Kind::Bin}, {"sqcap", "⊓", Kind::Bin}, {"uplus", "⊎", Kind::Bin},
    {"setminus", "∖", Kind::Bin}, {"smallsetminus", "∖", Kind::Bin}, {"wedge", "∧", Kind::Bin}, {"land", "∧", Kind::Bin},
    {"vee", "∨", Kind::Bin}, {"lor", "∨", Kind::Bin}, {"dagger", "†", Kind::Bin}, {"ddagger", "‡", Kind::Bin},
    {"wr", "≀", Kind::Bin}, {"amalg", "⨿", Kind::Bin}, {"diamond", "⋄", Kind::Bin}, {"triangleleft", "◁", Kind::Bin},
    {"triangleright", "▷", Kind::Bin}, {"bigtriangleup", "△", Kind::Bin}, {"bigtriangledown", "▽", Kind::Bin},
    {"lhd", "⊲", Kind::Bin}, {"rhd", "⊳", Kind::Bin}, {"unlhd", "⊴", Kind::Bin}, {"unrhd", "⊵", Kind::Bin},
    {"ltimes", "⋉", Kind::Bin}, {"rtimes", "⋊", Kind::Bin}, {"dotplus", "∔", Kind::Bin}, {"boxplus", "⊞", Kind::Bin},
    {"boxminus", "⊟", Kind::Bin}, {"boxtimes", "⊠", Kind::Bin}, {"intercal", "⊺", Kind::Bin},
    // Relations.
    {"le", "≤", Kind::Rel}, {"leq", "≤", Kind::Rel}, {"ge", "≥", Kind::Rel}, {"geq", "≥", Kind::Rel},
    {"ne", "≠", Kind::Rel}, {"neq", "≠", Kind::Rel}, {"approx", "≈", Kind::Rel}, {"approxeq", "≊", Kind::Rel},
    {"equiv", "≡", Kind::Rel}, {"sim", "∼", Kind::Rel}, {"simeq", "≃", Kind::Rel}, {"cong", "≅", Kind::Rel},
    {"propto", "∝", Kind::Rel}, {"varpropto", "∝", Kind::Rel}, {"ll", "≪", Kind::Rel}, {"gg", "≫", Kind::Rel},
    {"lll", "⋘", Kind::Rel}, {"ggg", "⋙", Kind::Rel}, {"in", "∈", Kind::Rel}, {"notin", "∉", Kind::Rel},
    {"ni", "∋", Kind::Rel}, {"owns", "∋", Kind::Rel}, {"subset", "⊂", Kind::Rel}, {"supset", "⊃", Kind::Rel},
    {"subseteq", "⊆", Kind::Rel}, {"supseteq", "⊇", Kind::Rel}, {"subsetneq", "⊊", Kind::Rel},
    {"supsetneq", "⊋", Kind::Rel}, {"sqsubset", "⊏", Kind::Rel}, {"sqsupset", "⊐", Kind::Rel},
    {"sqsubseteq", "⊑", Kind::Rel}, {"sqsupseteq", "⊒", Kind::Rel}, {"to", "→", Kind::Rel},
    {"rightarrow", "→", Kind::Rel}, {"leftarrow", "←", Kind::Rel}, {"gets", "←", Kind::Rel},
    {"leftrightarrow", "↔", Kind::Rel}, {"Rightarrow", "⇒", Kind::Rel}, {"Leftarrow", "⇐", Kind::Rel},
    {"Leftrightarrow", "⇔", Kind::Rel}, {"iff", "⟺", Kind::Rel}, {"implies", "⟹", Kind::Rel},
    {"impliedby", "⟸", Kind::Rel}, {"mapsto", "↦", Kind::Rel}, {"longmapsto", "⟼", Kind::Rel},
    {"longrightarrow", "⟶", Kind::Rel}, {"longleftarrow", "⟵", Kind::Rel}, {"longleftrightarrow", "⟷", Kind::Rel},
    {"Longrightarrow", "⟹", Kind::Rel}, {"Longleftarrow", "⟸", Kind::Rel}, {"Longleftrightarrow", "⟺", Kind::Rel},
    {"uparrow", "↑", Kind::Rel}, {"downarrow", "↓", Kind::Rel}, {"updownarrow", "↕", Kind::Rel},
    {"Uparrow", "⇑", Kind::Rel}, {"Downarrow", "⇓", Kind::Rel}, {"Updownarrow", "⇕", Kind::Rel},
    {"nearrow", "↗", Kind::Rel}, {"searrow", "↘", Kind::Rel}, {"swarrow", "↙", Kind::Rel}, {"nwarrow", "↖", Kind::Rel},
    {"hookrightarrow", "↪", Kind::Rel}, {"hookleftarrow", "↩", Kind::Rel}, {"rightharpoonup", "⇀", Kind::Rel},
    {"rightharpoondown", "⇁", Kind::Rel}, {"leftharpoonup", "↼", Kind::Rel}, {"leftharpoondown", "↽", Kind::Rel},
    {"rightleftharpoons", "⇌", Kind::Rel}, {"leftrightharpoons", "⇋", Kind::Rel}, {"rightleftarrows", "⇄", Kind::Rel},
    {"leftrightarrows", "⇆", Kind::Rel}, {"twoheadrightarrow", "↠", Kind::Rel}, {"leadsto", "⇝", Kind::Rel},
    {"parallel", "∥", Kind::Rel}, {"nparallel", "∦", Kind::Rel}, {"perp", "⊥", Kind::Rel}, {"mid", "∣", Kind::Rel},
    {"nmid", "∤", Kind::Rel}, {"vdash", "⊢", Kind::Rel}, {"dashv", "⊣", Kind::Rel}, {"models", "⊨", Kind::Rel},
    {"vDash", "⊨", Kind::Rel}, {"asymp", "≍", Kind::Rel}, {"doteq", "≐", Kind::Rel}, {"doteqdot", "≑", Kind::Rel},
    {"prec", "≺", Kind::Rel}, {"succ", "≻", Kind::Rel}, {"preceq", "⪯", Kind::Rel}, {"succeq", "⪰", Kind::Rel},
    {"leqslant", "⩽", Kind::Rel}, {"geqslant", "⩾", Kind::Rel}, {"lesssim", "≲", Kind::Rel}, {"gtrsim", "≳", Kind::Rel},
    {"lessgtr", "≶", Kind::Rel}, {"gtrless", "≷", Kind::Rel}, {"triangleq", "≜", Kind::Rel},
    {"coloneqq", "≔", Kind::Rel}, {"coloneq", "≔", Kind::Rel}, {"eqqcolon", "≕", Kind::Rel},
    {"bowtie", "⋈", Kind::Rel}, {"smile", "⌣", Kind::Rel}, {"frown", "⌢", Kind::Rel}, {"nleq", "≰", Kind::Rel},
    {"ngeq", "≱", Kind::Rel}, {"nless", "≮", Kind::Rel}, {"ngtr", "≯", Kind::Rel}, {"nsim", "≁", Kind::Rel},
    {"ncong", "≇", Kind::Rel}, {"backsim", "∽", Kind::Rel}, {"thicksim", "∼", Kind::Rel},
    {"thickapprox", "≈", Kind::Rel}, {"eqsim", "≂", Kind::Rel}, {"circeq", "≗", Kind::Rel},
    {"therefore", "∴", Kind::Rel}, {"because", "∵", Kind::Rel}, {"between", "≬", Kind::Rel},
    {"pitchfork", "⋔", Kind::Rel}, {"Join", "⨝", Kind::Rel}, {"sqsubsetneq", "⊏", Kind::Rel},
    // Delimiters.
    {"langle", "⟨", Kind::Open}, {"rangle", "⟩", Kind::Close}, {"lfloor", "⌊", Kind::Open},
    {"rfloor", "⌋", Kind::Close}, {"lceil", "⌈", Kind::Open}, {"rceil", "⌉", Kind::Close},
    {"lbrace", "{", Kind::Open}, {"rbrace", "}", Kind::Close}, {"lbrack", "[", Kind::Open},
    {"rbrack", "]", Kind::Close}, {"lvert", "|", Kind::Open}, {"rvert", "|", Kind::Close},
    {"lVert", "‖", Kind::Open}, {"rVert", "‖", Kind::Close}, {"lgroup", "⟮", Kind::Open}, {"rgroup", "⟯", Kind::Close},
    {"ulcorner", "⌜", Kind::Open}, {"urcorner", "⌝", Kind::Close}, {"llcorner", "⌞", Kind::Open},
    {"lrcorner", "⌟", Kind::Close},
    // Punctuation and dots.
    {"colon", ":", Kind::Punct}, {"ldotp", ".", Kind::Punct}, {"cdotp", "⋅", Kind::Punct},
    {"ldots", "…", Kind::Inner}, {"dots", "…", Kind::Inner}, {"dotsc", "…", Kind::Inner}, {"dotso", "…", Kind::Inner},
    {"cdots", "⋯", Kind::Inner}, {"dotsb", "⋯", Kind::Inner}, {"dotsm", "⋯", Kind::Inner}, {"dotsi", "⋯", Kind::Inner},
    {"ddots", "⋱", Kind::Inner}, {"iddots", "⋰", Kind::Inner},
};

struct BigOperator {
    const char* name;
    const char* text;
    bool integral;
};
const BigOperator kBigOperators[] = {
    {"sum", "∑", false},      {"prod", "∏", false},       {"coprod", "∐", false},    {"bigcup", "⋃", false},
    {"bigcap", "⋂", false},   {"bigvee", "⋁", false},     {"bigwedge", "⋀", false},  {"bigoplus", "⨁", false},
    {"bigotimes", "⨂", false}, {"bigodot", "⨀", false},    {"biguplus", "⨄", false},  {"bigsqcup", "⨆", false},
    {"int", "∫", true},       {"intop", "∫", true},       {"iint", "∬", true},       {"iiint", "∭", true},
    {"oint", "∮", true},      {"oiint", "∯", true},       {"oiiint", "∰", true},
};

// Functions set upright; those with limits under them in display style.
const struct {
    const char* name;
    bool limits;
} kFunctions[] = {
    {"arccos", false}, {"arcsin", false}, {"arctan", false}, {"arccot", false}, {"arcsec", false},
    {"arccsc", false}, {"arg", false},    {"cos", false},    {"cosh", false},   {"cot", false},
    {"coth", false},   {"csc", false},    {"deg", false},    {"det", true},     {"dim", false},
    {"exp", false},    {"gcd", true},     {"hom", false},    {"inf", true},     {"ker", false},
    {"lg", false},     {"lim", true},     {"liminf", true},  {"limsup", true},  {"ln", false},
    {"log", false},    {"max", true},     {"min", true},     {"Pr", true},      {"sec", false},
    {"sin", false},    {"sinh", false},   {"sup", true},     {"tan", false},    {"tanh", false},
    {"sgn", false},    {"sign", false},   {"erf", false},    {"erfc", false},   {"sinc", false},
    {"Tr", false},     {"tr", false},     {"rank", false},   {"diag", false},   {"argmax", true},
    {"argmin", true},  {"lcm", false},    {"cis", false},
};

const struct {
    const char* name;
    Mark mark;
} kAccents[] = {
    {"hat", Mark::Hat},           {"widehat", Mark::WideHat},      {"check", Mark::Check},
    {"widecheck", Mark::Check},   {"tilde", Mark::Tilde},          {"widetilde", Mark::WideTilde},
    {"bar", Mark::Bar},           {"overline", Mark::Overline},    {"vec", Mark::Vec},
    {"overrightarrow", Mark::RightArrow}, {"overleftarrow", Mark::LeftArrow},
    {"overleftrightarrow", Mark::BothArrow}, {"dot", Mark::Dot},   {"ddot", Mark::DDot},
    {"dddot", Mark::DDDot},       {"breve", Mark::Breve},          {"acute", Mark::Acute},
    {"grave", Mark::Grave},       {"mathring", Mark::Ring},        {"underline", Mark::Underline},
    {"cancel", Mark::Cancel},     {"bcancel", Mark::Cancel},       {"xcancel", Mark::Cancel},
    {"boxed", Mark::Boxed},       {"fbox", Mark::Boxed},           {"overbrace", Mark::OverBrace},
    {"underbrace", Mark::UnderBrace},
};

// siunitx's units and prefixes, for \SI, \si, \qty, \unit.
const struct {
    const char* name;
    const char* text;
} kUnits[] = {
    {"ohm", "Ω"},      {"volt", "V"},     {"ampere", "A"},  {"amp", "A"},      {"farad", "F"},    {"henry", "H"},
    {"hertz", "Hz"},   {"watt", "W"},     {"second", "s"},  {"meter", "m"},    {"metre", "m"},    {"gram", "g"},
    {"kelvin", "K"},   {"joule", "J"},    {"coulomb", "C"}, {"siemens", "S"},  {"tesla", "T"},    {"weber", "Wb"},
    {"decibel", "dB"}, {"bel", "B"},      {"degreeCelsius", "°C"}, {"celsius", "°C"}, {"percent", "%"},
    {"radian", "rad"}, {"degree", "°"},   {"minute", "min"}, {"hour", "h"},    {"newton", "N"},   {"pascal", "Pa"},
    {"dBm", "dBm"},    {"bit", "bit"},    {"byte", "B"},    {"voltampere", "VA"},
    {"yocto", "y"},    {"zepto", "z"},    {"atto", "a"},    {"femto", "f"},    {"pico", "p"},     {"nano", "n"},
    {"micro", "µ"},    {"milli", "m"},    {"centi", "c"},   {"deci", "d"},     {"kilo", "k"},     {"mega", "M"},
    {"giga", "G"},     {"tera", "T"},     {"peta", "P"},
};

template <typename Table>
const auto* lookup(const Table& table, const QString& name)
{
    for (const auto& entry : table)
        if (name == QLatin1String(entry.name)) return &entry;
    return static_cast<decltype(&table[0])>(nullptr);
}

// A character's class when it is typed, not named.
Kind kindOfChar(QChar c)
{
    switch (c.unicode()) {
    case '+': case '-': case '*': return Kind::Bin;
    case '=': case '<': case '>': case ':': return Kind::Rel;
    case ',': case ';': return Kind::Punct;
    case '(': case '[': return Kind::Open;
    case ')': case ']': case '!': case '?': return Kind::Close;
    default: break;
    }
    if (c.unicode() < 0x80) return Kind::Ord;   // . / | ... as TeX has them
    // Typed as such: the class of the first command that names it.
    static QHash<ushort, Kind> known = [] {
        QHash<ushort, Kind> h;
        for (const Symbol& s : kSymbols) {
            const QString t = QString::fromUtf8(s.text);
            if (t.size() == 1 && s.kind != Kind::Ord && !h.contains(t.at(0).unicode())) h.insert(t.at(0).unicode(), s.kind);
        }
        return h;
    }();
    return known.value(c.unicode(), Kind::Ord);
}

// \mathbb, \mathcal, \mathfrak: the letters of those alphabets.
QString alphabet(QChar c, int which)
{
    const char16_t u = c.unicode();
    const bool upper = u >= 'A' && u <= 'Z', lower = u >= 'a' && u <= 'z';
    if (!upper && !lower) return QString(c);
    if (which == 1) {   // double-struck
        switch (u) {
        case 'C': return QStringLiteral("ℂ");
        case 'H': return QStringLiteral("ℍ");
        case 'N': return QStringLiteral("ℕ");
        case 'P': return QStringLiteral("ℙ");
        case 'Q': return QStringLiteral("ℚ");
        case 'R': return QStringLiteral("ℝ");
        case 'Z': return QStringLiteral("ℤ");
        default: break;
        }
        const char32_t code = upper ? 0x1D538 + (u - 'A') : 0x1D552 + (u - 'a');
        return QString::fromUcs4(&code, 1);
    }
    if (which == 2) {   // script
        switch (u) {
        case 'B': return QStringLiteral("ℬ");
        case 'E': return QStringLiteral("ℰ");
        case 'F': return QStringLiteral("ℱ");
        case 'H': return QStringLiteral("ℋ");
        case 'I': return QStringLiteral("ℐ");
        case 'L': return QStringLiteral("ℒ");
        case 'M': return QStringLiteral("ℳ");
        case 'R': return QStringLiteral("ℛ");
        case 'e': return QStringLiteral("ℯ");
        case 'g': return QStringLiteral("ℊ");
        case 'o': return QStringLiteral("ℴ");
        default: break;
        }
        const char32_t code = upper ? 0x1D49C + (u - 'A') : 0x1D4B6 + (u - 'a');
        return QString::fromUcs4(&code, 1);
    }
    if (which == 3) {   // fraktur
        switch (u) {
        case 'C': return QStringLiteral("ℭ");
        case 'H': return QStringLiteral("ℌ");
        case 'I': return QStringLiteral("ℑ");
        case 'R': return QStringLiteral("ℜ");
        case 'Z': return QStringLiteral("ℨ");
        default: break;
        }
        const char32_t code = upper ? 0x1D504 + (u - 'A') : 0x1D51E + (u - 'a');
        return QString::fromUcs4(&code, 1);
    }
    return QString(c);
}

// A relation struck through: \not=.
QString negated(const QString& text)
{
    static const QHash<QString, QString> map = {
        {"=", "≠"}, {"∈", "∉"}, {"≡", "≢"}, {"<", "≮"}, {">", "≯"}, {"≤", "≰"}, {"≥", "≱"}, {"∼", "≁"},
        {"≃", "≄"}, {"≈", "≉"}, {"≅", "≇"}, {"⊂", "⊄"}, {"⊃", "⊅"}, {"⊆", "⊈"}, {"⊇", "⊉"}, {"∣", "∤"},
        {"∥", "∦"}, {"→", "↛"}, {"←", "↚"}, {"↔", "↮"}, {"⇒", "⇏"}, {"⇔", "⇎"}, {"∃", "∄"}, {"∋", "∌"},
    };
    const auto found = map.constFind(text);
    return found != map.cend() ? *found : text + QChar(0x0338);
}

// ----------------------------------------------------------------------
// The parser: TeX to boxes. It never fails: what it does not know it sets
// as written.

class Parser
{
public:
    Parser(const QString& tex, bool display) : s(tex), display(display) {}

    Ptr formula()
    {
        std::vector<std::vector<Ptr>> rows = parseRows(End::Input);
        Ptr body;
        bool any = false;
        for (const auto& row : rows)
            if (row.size() > 1) any = true;
        if (rows.size() == 1 && !any) {
            body = std::move(rows.front().front());
        } else {
            // Lines of display math, or aligned at their &s.
            auto m = std::make_unique<Matrix>();
            m->keepStyle = true;
            m->pairs = any;
            m->align = QStringLiteral("c");
            m->rowGap = 0.35;
            if (any) alignPairs(rows);
            m->rows = std::move(rows);
            body = std::move(m);
        }
        if (!tag.isEmpty()) {
            auto list = std::make_unique<HList>();
            list->items.push_back(std::move(body));
            list->items.push_back(std::make_unique<Space>(2.0));
            list->items.push_back(std::make_unique<Glyphs>(QLatin1Char('(') + tag + QLatin1Char(')'), Variant::Upright, Kind::Ord));
            body = std::move(list);
        }
        return body;
    }
    bool ok = true;

private:
    enum class End { Input, Group, Right, Environment };

    QString s;
    qsizetype i = 0;
    bool display;
    QString tag;
    Variant variant = Variant::Italic;
    int letters = 0;       // 1 double-struck, 2 script, 3 fraktur
    bool upright = false;  // \mathrm and kin: letters upright
    int depth = 0;

    bool atEnd() const { return i >= s.size(); }
    QChar peek() const { return atEnd() ? QChar() : s.at(i); }
    void skipSpace()
    {
        while (!atEnd() && s.at(i).isSpace()) ++i;
    }
    // At a command: its name (letters, or the one character after \).
    QString peekCommand() const
    {
        if (atEnd() || s.at(i) != QLatin1Char('\\') || i + 1 >= s.size()) return {};
        qsizetype j = i + 1;
        if (!s.at(j).isLetter()) return QString(s.at(j));
        while (j < s.size() && s.at(j).isLetter()) ++j;
        return s.mid(i + 1, j - i - 1);
    }
    QString takeCommand()
    {
        const QString name = peekCommand();
        i += 1 + name.size();
        if (name.size() > 0 && name.at(0).isLetter()) skipSpace();
        return name;
    }
    // The text of a {...} group as it is written; or the next character.
    QString rawGroup()
    {
        skipSpace();
        if (peek() != QLatin1Char('{')) {
            if (atEnd()) return {};
            if (peek() == QLatin1Char('\\')) return QLatin1Char('\\') + takeCommand();
            return QString(s.at(i++));
        }
        ++i;
        int level = 1;
        const qsizetype from = i;
        while (!atEnd()) {
            const QChar c = s.at(i);
            if (c == QLatin1Char('\\') && i + 1 < s.size()) {
                i += 2;
                continue;
            }
            if (c == QLatin1Char('{')) ++level;
            if (c == QLatin1Char('}') && --level == 0) break;
            ++i;
        }
        const QString text = s.mid(from, i - from);
        if (!atEnd()) ++i;
        else ok = false;
        return text;
    }
    // [...], when there.
    QString optional()
    {
        skipSpace();
        if (peek() != QLatin1Char('[')) return {};
        ++i;
        int level = 0;
        const qsizetype from = i;
        while (!atEnd()) {
            const QChar c = s.at(i);
            if (c == QLatin1Char('{')) ++level;
            if (c == QLatin1Char('}')) --level;
            if (c == QLatin1Char(']') && level <= 0) break;
            ++i;
        }
        const QString text = s.mid(from, i - from);
        if (!atEnd()) ++i;
        return text;
    }
    // Something parsed as math from text of its own (an optional argument).
    Ptr sub(const QString& text)
    {
        Parser inner(text, display);
        inner.variant = variant;
        inner.letters = letters;
        inner.upright = upright;
        inner.depth = depth + 1;
        std::vector<std::vector<Ptr>> rows = inner.parseRows(End::Input);
        ok = ok && inner.ok;
        return std::move(rows.front().front());
    }

    // An argument: a group, or the one atom that follows.
    Ptr argument()
    {
        skipSpace();
        if (peek() == QLatin1Char('{')) {
            ++i;
            Ptr list = parseList(End::Group);
            if (peek() == QLatin1Char('}')) ++i;
            else ok = false;
            return list;
        }
        if (atEnd()) {
            ok = false;
            return std::make_unique<Empty>();
        }
        Ptr atom = parseAtom();
        return atom ? std::move(atom) : std::make_unique<Empty>();
    }
    // An argument in a font of its own.
    Ptr argumentIn(Variant v, int alphabetOf, bool uprightLetters)
    {
        const Variant v0 = variant;
        const int l0 = letters;
        const bool u0 = upright;
        variant = v;
        letters = alphabetOf;
        upright = uprightLetters;
        Ptr arg = argument();
        variant = v0;
        letters = l0;
        upright = u0;
        return arg;
    }

    // Rows of cells: & between the cells, \\ between the rows.
    std::vector<std::vector<Ptr>> parseRows(End end)
    {
        std::vector<std::vector<Ptr>> rows(1);
        for (;;) {
            rows.back().push_back(parseList(end, true));
            skipSpace();
            if (peek() == QLatin1Char('&')) {
                ++i;
                continue;
            }
            if (peekCommand() == QLatin1String("\\")) {
                takeCommand();
                optional();   // \\[2pt]
                skipSpace();
                if (peekCommand() == QLatin1String("hline")) takeCommand();
                // A last \\ makes no empty row.
                skipSpace();
                if (atEnd() || peekCommand() == QLatin1String("end")) break;
                rows.emplace_back();
                continue;
            }
            break;
        }
        return rows;
    }

    // aligned: the cells after an & begin with an empty atom, so that the
    // relation they begin with is spaced as in the middle of a formula.
    static void alignPairs(std::vector<std::vector<Ptr>>& rows)
    {
        for (auto& row : rows)
            for (size_t c = 1; c < row.size(); c += 2) {
                if (auto* list = dynamic_cast<HList*>(row[c].get())) {
                    list->items.insert(list->items.begin(), std::make_unique<Empty>());
                } else {
                    auto wrap = std::make_unique<HList>();
                    wrap->items.push_back(std::make_unique<Empty>());
                    wrap->items.push_back(std::move(row[c]));
                    row[c] = std::move(wrap);
                }
            }
    }

    // Atoms until the end of the group (or of the input, a \right, an
    // \end; with rows: an & or a \\).
    Ptr parseList(End end, bool rows = false)
    {
        auto list = std::make_unique<HList>();
        if (++depth > 64) {   // nested beyond reason
            --depth;
            ok = false;
            i = s.size();
            return list;
        }
        while (true) {
            skipSpace();
            if (atEnd()) {
                if (end == End::Group || end == End::Right || end == End::Environment) ok = false;
                break;
            }
            const QChar c = peek();
            if (c == QLatin1Char('}')) {
                if (end == End::Group) break;
                ++i;   // a stray }
                ok = false;
                continue;
            }
            if (c == QLatin1Char('&')) {
                if (rows) break;
                ++i;
                continue;
            }
            if (c == QLatin1Char('^') || c == QLatin1Char('_')) {
                ++i;
                attachScript(*list, c == QLatin1Char('^'));
                continue;
            }
            if (c == QLatin1Char('\'')) {
                int n = 0;
                while (peek() == QLatin1Char('\'')) {
                    ++n;
                    ++i;
                }
                auto primes = std::make_unique<Glyphs>(QString(n, QChar(0x2032)), Variant::Symbol, Kind::Ord);
                attachPrimes(*list, std::move(primes));
                continue;
            }
            if (c == QLatin1Char('\\')) {
                const QString name = peekCommand();
                if (name == QLatin1String("\\")) {
                    if (rows) break;
                    takeCommand();
                    optional();
                    continue;
                }
                if (name == QLatin1String("right")) {
                    if (end == End::Right) break;
                    takeCommand();
                    delimiterName();
                    ok = false;
                    continue;
                }
                if (name == QLatin1String("end")) {
                    if (end == End::Environment) break;
                    takeCommand();
                    rawGroup();
                    ok = false;
                    continue;
                }
                if (name == QLatin1String("limits") || name == QLatin1String("nolimits")
                    || name == QLatin1String("displaylimits")) {
                    takeCommand();
                    setLimits(*list, name == QLatin1String("nolimits") ? 0 : 1);
                    continue;
                }
                if (name == QLatin1String("over") || name == QLatin1String("choose") || name == QLatin1String("atop")) {
                    takeCommand();
                    auto num = std::move(list);
                    Ptr den = parseList(end, rows);
                    auto frac = std::make_unique<Frac>(std::move(num), std::move(den));
                    frac->bar = name == QLatin1String("over");
                    list = std::make_unique<HList>();
                    if (name == QLatin1String("choose")) {
                        auto d = std::make_unique<Delimited>();
                        d->left = QStringLiteral("(");
                        d->right = QStringLiteral(")");
                        d->body = std::move(frac);
                        list->items.push_back(std::move(d));
                    } else {
                        list->items.push_back(std::move(frac));
                    }
                    break;
                }
                if (name == QLatin1String("displaystyle") || name == QLatin1String("textstyle")
                    || name == QLatin1String("scriptstyle") || name == QLatin1String("scriptscriptstyle")) {
                    takeCommand();
                    const Style st = name == QLatin1String("displaystyle") ? Style::Display
                                     : name == QLatin1String("textstyle")  ? Style::Text
                                     : name == QLatin1String("scriptstyle") ? Style::Script
                                                                            : Style::ScriptScript;
                    Ptr rest = parseList(end, rows);
                    list->items.push_back(std::make_unique<Styled>(std::move(rest), st));
                    break;
                }
                if (name == QLatin1String("color")) {
                    takeCommand();
                    const QColor colour(rawGroup().trimmed());
                    Ptr rest = parseList(end, rows);
                    if (colour.isValid()) rest->colour = colour;
                    list->items.push_back(std::move(rest));
                    break;
                }
                if (name == QLatin1String("tag") || name == QLatin1String("tag*")) {
                    takeCommand();
                    tag = rawGroup();
                    continue;
                }
                if (name == QLatin1String("label") || name == QLatin1String("nonumber") || name == QLatin1String("notag")
                    || name == QLatin1String("hline") || name == QLatin1String("allowbreak")
                    || name == QLatin1String("nobreak") || name == QLatin1String("relax")) {
                    takeCommand();
                    if (name == QLatin1String("label")) rawGroup();
                    continue;
                }
            }
            Ptr atom = parseAtom();
            if (atom) list->items.push_back(std::move(atom));
        }
        --depth;
        return list;
    }

    void attachScript(HList& list, bool up)
    {
        Ptr script = argument();
        auto* last = list.items.empty() ? nullptr : dynamic_cast<Scripts*>(list.items.back().get());
        if (last != nullptr && !(up ? last->sup : last->sub)) {
            (up ? last->sup : last->sub) = std::move(script);
            return;
        }
        auto scripts = std::make_unique<Scripts>();
        if (!list.items.empty() && last == nullptr) {
            scripts->base = std::move(list.items.back());
            list.items.pop_back();
        } else if (last != nullptr) {
            // x^a^b: a double script; TeX refuses, we set it on an empty base.
            ok = false;
        }
        (up ? scripts->sup : scripts->sub) = std::move(script);
        if (scripts->base && scripts->base->takesLimits() && dynamic_cast<Accent*>(scripts->base.get()) != nullptr)
            scripts->limits = 1;   // \underbrace{...}_{...}: under the brace
        list.items.push_back(std::move(scripts));
    }

    // f': the prime (a raised glyph already) beside the base; f'^2: the 2
    // on the both of them.
    void attachPrimes(HList& list, Ptr primes)
    {
        auto primed = std::make_unique<HList>();
        if (!list.items.empty()) {
            primed->items.push_back(std::move(list.items.back()));
            list.items.pop_back();
        }
        primed->items.push_back(std::move(primes));
        list.items.push_back(std::move(primed));
    }

    static void setLimits(HList& list, int limits)
    {
        if (list.items.empty()) return;
        Box* last = list.items.back().get();
        if (auto* scripts = dynamic_cast<Scripts*>(last)) {
            scripts->limits = limits;
            return;
        }
        auto scripts = std::make_unique<Scripts>();
        scripts->limits = limits;
        scripts->base = std::move(list.items.back());
        list.items.back() = std::move(scripts);
    }

    // The delimiter after \left, \right, \big: a character or a command.
    QString delimiterName()
    {
        skipSpace();
        if (atEnd()) return {};
        if (peek() == QLatin1Char('\\')) {
            const QString name = takeCommand();
            if (name == QLatin1String("{") || name == QLatin1String("lbrace")) return QStringLiteral("{");
            if (name == QLatin1String("}") || name == QLatin1String("rbrace")) return QStringLiteral("}");
            if (name == QLatin1String("|") || name == QLatin1String("Vert") || name == QLatin1String("lVert")
                || name == QLatin1String("rVert"))
                return QString(QChar(0x2016));
            if (name == QLatin1String("vert") || name == QLatin1String("lvert") || name == QLatin1String("rvert")
                || name == QLatin1String("mid"))
                return QStringLiteral("|");
            if (name == QLatin1String("backslash")) return QStringLiteral("\\");
            if (const auto* sym = lookup(kSymbols, name)) return QString::fromUtf8(sym->text);
            return {};
        }
        const QChar c = s.at(i++);
        if (c == QLatin1Char('<')) return QString(QChar(0x27E8));
        if (c == QLatin1Char('>')) return QString(QChar(0x27E9));
        return QString(c);
    }

    // One atom: a character, a group, a command with its arguments.
    Ptr parseAtom()
    {
        skipSpace();
        if (atEnd()) return nullptr;
        // \not\not\not...: atoms in atoms, not beyond reason.
        struct Deeper {
            int& d;
            explicit Deeper(int& depth) : d(++depth) {}
            ~Deeper() { --d; }
        } deeper(depth);
        if (depth > 200) {
            ok = false;
            i = s.size();
            return nullptr;
        }
        const QChar c = s.at(i);
        if (c == QLatin1Char('{')) {
            ++i;
            Ptr list = parseList(End::Group);
            if (peek() == QLatin1Char('}')) ++i;
            else ok = false;
            return list;
        }
        if (c == QLatin1Char('\\')) {
            if (i + 1 >= s.size()) {
                ++i;
                return nullptr;
            }
            return command(takeCommand());
        }
        if (c == QLatin1Char('~')) {
            ++i;
            return std::make_unique<Space>(0.33);
        }
        // One character (a surrogate pair is one).
        QString text(c);
        ++i;
        if (c.isHighSurrogate() && !atEnd() && s.at(i).isLowSurrogate()) text += s.at(i++);
        return character(text);
    }

    Ptr character(const QString& text)
    {
        const QChar c = text.at(0);
        if (c.isLetter() && c.unicode() < 0x80) {
            QString t = letters > 0 ? alphabet(c, letters) : text;
            Variant v = variant;
            if (letters > 0 || upright) v = (v == Variant::Bold || v == Variant::BoldItalic) ? Variant::Bold : v == Variant::Italic ? Variant::Upright : v;
            auto g = std::make_unique<Glyphs>(t, v, Kind::Ord);
            g->single = true;
            return g;
        }
        Kind k = kindOfChar(c);
        QString t = text;
        if (c == QLatin1Char('-')) t = QString(QChar(0x2212));
        else if (c == QLatin1Char('*')) t = QString(QChar(0x2217));
        Variant v = variant == Variant::Bold || variant == Variant::BoldItalic ? Variant::Bold
                    : variant == Variant::Sans || variant == Variant::Mono ? variant
                                                                          : Variant::Upright;
        if (c.isLetter()) v = variant;   // Greek typed as such, and other letters
        else if (!c.isDigit() && c != QLatin1Char('.') && v == Variant::Upright) v = Variant::Symbol;
        auto g = std::make_unique<Glyphs>(t, v, k);
        g->single = true;
        return g;
    }

    Ptr symbol(const QString& text, Kind kind, Variant v)
    {
        auto g = std::make_unique<Glyphs>(text, v, kind);
        g->single = true;
        return g;
    }

    Ptr function(const QString& name, bool limits)
    {
        auto g = std::make_unique<Glyphs>(name, variant == Variant::Bold ? Variant::Bold : Variant::Upright, Kind::Op);
        g->limits = limits;
        return g;
    }

    Ptr text(const QString& raw, Variant v)
    {
        // Text as written: spaces kept, a few escapes undone.
        QString t = raw;
        t.replace(QLatin1String("\\%"), QLatin1String("%"))
            .replace(QLatin1String("\\&"), QLatin1String("&"))
            .replace(QLatin1String("\\$"), QLatin1String("$"))
            .replace(QLatin1String("\\_"), QLatin1String("_"))
            .replace(QLatin1String("\\#"), QLatin1String("#"))
            .replace(QLatin1String("\\{"), QLatin1String("{"))
            .replace(QLatin1String("\\}"), QLatin1String("}"))
            .replace(QLatin1String("~"), QString(QChar(0x00A0)))
            .replace(QLatin1String("\\ "), QLatin1String(" "))
            .replace(QLatin1String("--"), QString(QChar(0x2013)));
        t.remove(QLatin1Char('{')).remove(QLatin1Char('}'));
        // $...$ inside text: its TeX, set as math would be would need more;
        // the dollars go.
        t.remove(QLatin1Char('$'));
        return std::make_unique<Glyphs>(t, v, Kind::Ord);
    }

    // siunitx: \kilo\ohm is kΩ, \per is a slash.
    Ptr unit(const QString& raw)
    {
        QString out;
        qsizetype k = 0;
        while (k < raw.size()) {
            const QChar c = raw.at(k);
            if (c == QLatin1Char('\\')) {
                qsizetype j = k + 1;
                while (j < raw.size() && raw.at(j).isLetter()) ++j;
                const QString name = raw.mid(k + 1, j - k - 1);
                k = j;
                if (name == QLatin1String("per")) out += QLatin1Char('/');
                else if (name == QLatin1String("squared")) out += QChar(0x00B2);
                else if (name == QLatin1String("cubed")) out += QChar(0x00B3);
                else if (name == QLatin1String("square")) continue;
                else if (const auto* u = lookup(kUnits, name)) out += QString::fromUtf8(u->text);
                else if (const auto* g = lookup(kSymbols, name)) out += QString::fromUtf8(g->text);
                else if (const auto* g2 = lookup(kGreek, name)) out += QString::fromUtf8(g2->text);
                else out += name;
                continue;
            }
            if (c == QLatin1Char('.') || c == QLatin1Char('~')) out += QChar(0x22C5);
            else if (c != QLatin1Char('{') && c != QLatin1Char('}')) out += c;
            ++k;
        }
        return std::make_unique<Glyphs>(out, Variant::Upright, Kind::Ord);
    }

    qreal length(const QString& raw)
    {
        // 3pt, 0.5em, 2mu, 1ex.
        static const QRegularExpression re(QStringLiteral("^\\s*(-?[0-9]*\\.?[0-9]+)\\s*([a-z]*)"));
        const auto m = re.match(raw);
        if (!m.hasMatch()) return 0.0;
        const qreal v = m.captured(1).toDouble();
        const QString u = m.captured(2);
        if (u == QLatin1String("em") || u == QLatin1String("quad")) return v;
        if (u == QLatin1String("ex")) return v * 0.45;
        if (u == QLatin1String("mu")) return v / 18.0;
        if (u == QLatin1String("pt")) return v / 10.0;
        if (u == QLatin1String("mm")) return v * 0.28;
        if (u == QLatin1String("cm")) return v * 2.8;
        if (u == QLatin1String("in")) return v * 7.2;
        if (u == QLatin1String("px")) return v / 16.0;
        return v / 10.0;
    }

    Ptr environment(const QString& name)
    {
        QString cols;
        if (name == QLatin1String("array") || name == QLatin1String("subarray")) cols = rawGroup();
        if (name == QLatin1String("alignat") || name == QLatin1String("alignat*") || name == QLatin1String("alignedat"))
            rawGroup();
        auto m = std::make_unique<Matrix>();
        std::vector<std::vector<Ptr>> rows = parseRows(End::Environment);
        // \end{name}
        if (peekCommand() == QLatin1String("end")) {
            takeCommand();
            rawGroup();
        } else {
            ok = false;
        }
        const QString base = QString(name).remove(QLatin1Char('*'));
        QString left, right;
        if (base == QLatin1String("pmatrix")) left = QStringLiteral("("), right = QStringLiteral(")");
        else if (base == QLatin1String("bmatrix")) left = QStringLiteral("["), right = QStringLiteral("]");
        else if (base == QLatin1String("Bmatrix")) left = QStringLiteral("{"), right = QStringLiteral("}");
        else if (base == QLatin1String("vmatrix")) left = right = QStringLiteral("|");
        else if (base == QLatin1String("Vmatrix")) left = right = QString(QChar(0x2016));
        else if (base == QLatin1String("cases") || base == QLatin1String("dcases")) left = QStringLiteral("{"), right = QStringLiteral(".");
        else if (base == QLatin1String("rcases")) left = QStringLiteral("."), right = QStringLiteral("}");

        if (base == QLatin1String("aligned") || base == QLatin1String("align") || base == QLatin1String("alignat")
            || base == QLatin1String("alignedat") || base == QLatin1String("split") || base == QLatin1String("eqnarray")
            || base == QLatin1String("flalign")) {
            m->pairs = true;
            m->keepStyle = true;
            m->rowGap = 0.45;
            alignPairs(rows);
        } else if (base == QLatin1String("gathered") || base == QLatin1String("gather") || base == QLatin1String("equation")
                   || base == QLatin1String("multline")) {
            m->align = QStringLiteral("c");
            m->keepStyle = true;
            m->rowGap = 0.45;
        } else if (base == QLatin1String("cases") || base == QLatin1String("dcases") || base == QLatin1String("rcases")) {
            m->align = QStringLiteral("l");
            if (base == QLatin1String("dcases")) m->keepStyle = true;
        } else if (base == QLatin1String("smallmatrix") || base == QLatin1String("subarray")) {
            m->cellStyle = int(Style::Script);
            m->colGap = 0.5;
            m->rowGap = 0.15;
            m->align = cols.isEmpty() ? QStringLiteral("c") : QString(cols).remove(QRegularExpression(QStringLiteral("[^lcr]")));
        } else if (base == QLatin1String("array")) {
            m->align = QString(cols).remove(QRegularExpression(QStringLiteral("[^lcr]")));
            m->colGap = 0.8;
        } else {
            m->align = QStringLiteral("c");
        }
        m->rows = std::move(rows);
        if (left.isEmpty() && right.isEmpty()) return m;
        auto d = std::make_unique<Delimited>();
        d->left = left;
        d->right = right;
        d->body = std::move(m);
        return d;
    }

    Ptr command(const QString& name)
    {
        if (name.isEmpty()) return nullptr;
        // Control symbols.
        if (name.size() == 1 && !name.at(0).isLetter()) {
            const QChar c = name.at(0);
            switch (c.unicode()) {
            case ',': return std::make_unique<Space>(3.0 / 18);
            case ':': case '>': return std::make_unique<Space>(4.0 / 18);
            case ';': return std::make_unique<Space>(5.0 / 18);
            case '!': return std::make_unique<Space>(-3.0 / 18);
            case ' ': return std::make_unique<Space>(0.33);
            case '{': return symbol(QStringLiteral("{"), Kind::Open, Variant::Upright);
            case '}': return symbol(QStringLiteral("}"), Kind::Close, Variant::Upright);
            case '|': return symbol(QString(QChar(0x2016)), Kind::Ord, Variant::Upright);
            case '%': case '$': case '&': case '#': case '_':
                return symbol(QString(c), Kind::Ord, Variant::Upright);
            default: return symbol(QString(c), Kind::Ord, Variant::Upright);
            }
        }
        if (const auto* g = lookup(kGreek, name))
            return symbol(QString::fromUtf8(g->text), g->kind,
                          upright || variant == Variant::Upright ? Variant::Upright
                          : variant == Variant::Bold || variant == Variant::BoldItalic ? Variant::BoldItalic
                                                                                        : Variant::Italic);
        if (const auto* sym = lookup(kSymbols, name)) {
            // Upper-case Greek and letter-like symbols in the text's serif.
            const QString t = QString::fromUtf8(sym->text);
            const bool letter = t.size() == 1 && t.at(0).isLetter();
            return symbol(t, sym->kind, letter ? (variant == Variant::Bold || variant == Variant::BoldItalic ? Variant::Bold : Variant::Upright) : Variant::Symbol);
        }
        if (const auto* op = lookup(kBigOperators, name)) {
            auto g = std::make_unique<Glyphs>(QString::fromUtf8(op->text), Variant::Symbol, Kind::Op);
            g->big = true;
            g->integral = op->integral;
            g->limits = !op->integral;
            return g;
        }
        if (const auto* f = lookup(kFunctions, name)) {
            QString shown = QString::fromLatin1(f->name);
            if (name == QLatin1String("liminf")) shown = QStringLiteral("lim inf");
            else if (name == QLatin1String("limsup")) shown = QStringLiteral("lim sup");
            else if (name == QLatin1String("argmax")) shown = QStringLiteral("arg max");
            else if (name == QLatin1String("argmin")) shown = QStringLiteral("arg min");
            return function(shown, f->limits);
        }
        if (const auto* acc = lookup(kAccents, name)) return std::make_unique<Accent>(acc->mark, argument());

        // Fractions, roots.
        if (name == QLatin1String("frac") || name == QLatin1String("dfrac") || name == QLatin1String("tfrac")
            || name == QLatin1String("cfrac")) {
            Ptr num = argument();
            Ptr den = argument();
            auto f = std::make_unique<Frac>(std::move(num), std::move(den));
            if (name == QLatin1String("dfrac") || name == QLatin1String("cfrac")) f->forced = int(Style::Display);
            if (name == QLatin1String("tfrac")) f->forced = int(Style::Text);
            return f;
        }
        if (name == QLatin1String("binom") || name == QLatin1String("dbinom") || name == QLatin1String("tbinom")) {
            Ptr num = argument();
            Ptr den = argument();
            auto f = std::make_unique<Frac>(std::move(num), std::move(den));
            f->bar = false;
            auto d = std::make_unique<Delimited>();
            d->left = QStringLiteral("(");
            d->right = QStringLiteral(")");
            d->body = std::move(f);
            if (name == QLatin1String("dbinom")) return std::make_unique<Styled>(std::move(d), Style::Display);
            if (name == QLatin1String("tbinom")) return std::make_unique<Styled>(std::move(d), Style::Text);
            return d;
        }
        if (name == QLatin1String("sqrt")) {
            auto r = std::make_unique<Sqrt>();
            const QString index = optional();
            if (!index.isEmpty()) r->index = sub(index);
            r->body = argument();
            return r;
        }

        // Fonts.
        if (name == QLatin1String("mathrm") || name == QLatin1String("rm") || name == QLatin1String("mathup"))
            return argumentIn(Variant::Upright, 0, true);
        if (name == QLatin1String("mathnormal")) return argumentIn(Variant::Italic, 0, false);
        if (name == QLatin1String("mathit")) return argumentIn(Variant::Italic, 0, false);
        if (name == QLatin1String("mathbf") || name == QLatin1String("bf") || name == QLatin1String("mathbfup"))
            return argumentIn(Variant::Bold, 0, true);
        if (name == QLatin1String("boldsymbol") || name == QLatin1String("bm") || name == QLatin1String("mathbfit"))
            return argumentIn(Variant::BoldItalic, 0, false);
        if (name == QLatin1String("mathsf")) return argumentIn(Variant::Sans, 0, true);
        if (name == QLatin1String("mathtt")) return argumentIn(Variant::Mono, 0, true);
        if (name == QLatin1String("mathbb") || name == QLatin1String("Bbb")) return argumentIn(Variant::Upright, 1, true);
        if (name == QLatin1String("mathcal") || name == QLatin1String("mathscr") || name == QLatin1String("cal"))
            return argumentIn(Variant::Upright, 2, true);
        if (name == QLatin1String("mathfrak") || name == QLatin1String("frak")) return argumentIn(Variant::Upright, 3, true);
        if (name == QLatin1String("text") || name == QLatin1String("textrm") || name == QLatin1String("textnormal")
            || name == QLatin1String("mbox") || name == QLatin1String("hbox") || name == QLatin1String("textup"))
            return text(rawGroup(), Variant::Upright);
        if (name == QLatin1String("textit") || name == QLatin1String("emph") || name == QLatin1String("textsl"))
            return text(rawGroup(), Variant::Italic);
        if (name == QLatin1String("textbf")) return text(rawGroup(), Variant::Bold);
        if (name == QLatin1String("texttt")) return text(rawGroup(), Variant::Mono);
        if (name == QLatin1String("textsf")) return text(rawGroup(), Variant::Sans);
        if (name == QLatin1String("operatorname") || name == QLatin1String("operatorname*")
            || name == QLatin1String("mathop")) {
            bool star = false;
            if (peek() == QLatin1Char('*')) {
                ++i;
                star = true;
            }
            if (name == QLatin1String("mathop")) {
                Ptr arg = argument();
                arg->kind = Kind::Op;
                return arg;
            }
            QString t = rawGroup();
            t.remove(QLatin1Char('\\')).remove(QLatin1Char('{')).remove(QLatin1Char('}'));
            return function(t, star);
        }
        if (name == QLatin1String("mathrel") || name == QLatin1String("mathbin") || name == QLatin1String("mathord")
            || name == QLatin1String("mathopen") || name == QLatin1String("mathclose") || name == QLatin1String("mathpunct")
            || name == QLatin1String("mathinner")) {
            Ptr arg = argument();
            arg->kind = name == QLatin1String("mathrel")    ? Kind::Rel
                        : name == QLatin1String("mathbin")  ? Kind::Bin
                        : name == QLatin1String("mathopen") ? Kind::Open
                        : name == QLatin1String("mathclose") ? Kind::Close
                        : name == QLatin1String("mathpunct") ? Kind::Punct
                        : name == QLatin1String("mathinner") ? Kind::Inner
                                                             : Kind::Ord;
            return arg;
        }

        // Over, under, beside.
        if (name == QLatin1String("overset") || name == QLatin1String("stackrel") || name == QLatin1String("underset")) {
            Ptr first = argument();
            Ptr body = argument();
            auto ou = std::make_unique<OverUnder>();
            ou->body = std::move(body);
            (name == QLatin1String("underset") ? ou->under : ou->over) = std::move(first);
            if (name == QLatin1String("stackrel")) ou->body->kind = Kind::Rel;
            return ou;
        }
        if (name == QLatin1String("xrightarrow") || name == QLatin1String("xleftarrow")
            || name == QLatin1String("xlongrightarrow") || name == QLatin1String("xlongleftarrow")) {
            auto x = std::make_unique<XArrow>();
            x->leftward = name.contains(QLatin1String("left"));
            const QString below = optional();
            if (!below.isEmpty()) x->under = sub(below);
            x->over = argument();
            return x;
        }
        if (name == QLatin1String("phantom") || name == QLatin1String("hphantom") || name == QLatin1String("vphantom")) {
            auto ph = std::make_unique<Phantom>();
            ph->body = argument();
            ph->width = name != QLatin1String("vphantom");
            ph->height = name != QLatin1String("hphantom");
            return ph;
        }
        if (name == QLatin1String("mathstrut") || name == QLatin1String("strut")) {
            auto ph = std::make_unique<Phantom>();
            ph->body = symbol(QStringLiteral("("), Kind::Ord, Variant::Upright);
            ph->width = false;
            return ph;
        }
        if (name == QLatin1String("not")) {
            Ptr next = parseAtom();
            if (auto* g = dynamic_cast<Glyphs*>(next.get()); g != nullptr && g->single) {
                g->text = negated(g->text);
                return next;
            }
            return next ? std::move(next) : symbol(QString(QChar(0x0338)), Kind::Rel, Variant::Upright);
        }
        if (name == QLatin1String("textcolor") || name == QLatin1String("colorbox")) {
            const QColor colour(rawGroup().trimmed());
            Ptr body = name == QLatin1String("colorbox") ? text(rawGroup(), Variant::Upright) : argument();
            if (colour.isValid() && name == QLatin1String("textcolor")) body->colour = colour;
            return body;
        }
        if (name == QLatin1String("substack")) {
            skipSpace();
            if (peek() != QLatin1Char('{')) return argument();
            ++i;
            auto m = std::make_unique<Matrix>();
            m->rows = parseRows(End::Group);
            if (peek() == QLatin1Char('}')) ++i;
            m->align = QStringLiteral("c");
            m->keepStyle = true;
            m->rowGap = 0.05;
            return m;
        }

        // Spaces.
        if (name == QLatin1String("quad")) return std::make_unique<Space>(1.0);
        if (name == QLatin1String("qquad")) return std::make_unique<Space>(2.0);
        if (name == QLatin1String("enspace") || name == QLatin1String("enskip")) return std::make_unique<Space>(0.5);
        if (name == QLatin1String("thinspace")) return std::make_unique<Space>(3.0 / 18);
        if (name == QLatin1String("medspace")) return std::make_unique<Space>(4.0 / 18);
        if (name == QLatin1String("thickspace")) return std::make_unique<Space>(5.0 / 18);
        if (name == QLatin1String("negthinspace")) return std::make_unique<Space>(-3.0 / 18);
        if (name == QLatin1String("negmedspace")) return std::make_unique<Space>(-4.0 / 18);
        if (name == QLatin1String("negthickspace")) return std::make_unique<Space>(-5.0 / 18);
        if (name == QLatin1String("hspace") || name == QLatin1String("hspace*") || name == QLatin1String("mspace")
            || name == QLatin1String("hskip") || name == QLatin1String("kern") || name == QLatin1String("mkern")) {
            skipSpace();
            if (peek() == QLatin1Char('*')) ++i;
            QString raw;
            if (peek() == QLatin1Char('{')) {
                raw = rawGroup();
            } else {
                const qsizetype from = i;
                while (!atEnd() && (s.at(i).isDigit() || s.at(i) == QLatin1Char('.') || s.at(i) == QLatin1Char('-')
                                    || s.at(i).isLetter()))
                    ++i;
                raw = s.mid(from, i - from);
            }
            return std::make_unique<Space>(length(raw));
        }

        // Delimiters at a size.
        if (name == QLatin1String("left")) {
            auto d = std::make_unique<Delimited>();
            d->left = delimiterName();
            d->body = parseList(End::Right);
            if (peekCommand() == QLatin1String("right")) {
                takeCommand();
                d->right = delimiterName();
            } else {
                ok = false;
            }
            d->kind = Kind::Inner;
            return d;
        }
        if (name == QLatin1String("middle")) return symbol(delimiterName(), Kind::Rel, Variant::Upright);
        static const QRegularExpression big(QStringLiteral("^([Bb]igg?)([lrm]?)$"));
        if (const auto m = big.match(name); m.hasMatch()) {
            const QString size = m.captured(1);
            auto d = std::make_unique<Delimited>();
            d->left = delimiterName();
            d->fixed = size == QLatin1String("big") ? 1.2 : size == QLatin1String("Big") ? 1.8 : size == QLatin1String("bigg") ? 2.4 : 3.0;
            const QString side = m.captured(2);
            d->kind = side == QLatin1String("l") ? Kind::Open : side == QLatin1String("r") ? Kind::Close : side == QLatin1String("m") ? Kind::Rel : Kind::Ord;
            return d;
        }

        if (name == QLatin1String("begin")) return environment(rawGroup().trimmed());

        // Modulo.
        if (name == QLatin1String("bmod")) {
            Ptr mod = function(QStringLiteral("mod"), false);
            mod->kind = Kind::Bin;
            return mod;
        }
        if (name == QLatin1String("pmod") || name == QLatin1String("mod") || name == QLatin1String("pod")) {
            auto list = std::make_unique<HList>();
            list->items.push_back(std::make_unique<Space>(name == QLatin1String("mod") ? 0.67 : 1.0));
            if (name != QLatin1String("mod")) list->items.push_back(symbol(QStringLiteral("("), Kind::Open, Variant::Upright));
            if (name != QLatin1String("pod")) {
                list->items.push_back(std::make_unique<Glyphs>(QStringLiteral("mod"), Variant::Upright, Kind::Ord));
                list->items.push_back(std::make_unique<Space>(0.33));
            }
            list->items.push_back(argument());
            if (name != QLatin1String("mod")) list->items.push_back(symbol(QStringLiteral(")"), Kind::Close, Variant::Upright));
            return list;
        }

        // Units.
        if (name == QLatin1String("SI") || name == QLatin1String("qty")) {
            auto list = std::make_unique<HList>();
            optional();
            list->items.push_back(text(rawGroup(), Variant::Upright));
            list->items.push_back(std::make_unique<Space>(3.0 / 18));
            list->items.push_back(unit(rawGroup()));
            return list;
        }
        if (name == QLatin1String("si") || name == QLatin1String("unit")) {
            optional();
            return unit(rawGroup());
        }
        if (name == QLatin1String("num")) {
            optional();
            return text(rawGroup(), Variant::Upright);
        }
        if (name == QLatin1String("ang")) return text(rawGroup() + QStringLiteral("°"), Variant::Upright);
        if (name == QLatin1String("eqref") || name == QLatin1String("ref")) {
            const QString r = rawGroup();
            return text(name == QLatin1String("eqref") ? QLatin1Char('(') + r + QLatin1Char(')') : r, Variant::Upright);
        }

        // Not known: as written.
        ok = false;
        return std::make_unique<Glyphs>(QLatin1Char('\\') + name, Variant::Upright, Kind::Ord);
    }
};

} // namespace

// ----------------------------------------------------------------------
Typeset typeset(const QString& tex, const QFont& font, const QColor& colour, bool display, qreal devicePixelRatio)
{
    Ctx ctx;
    ctx.base = font;
    int px = QFontInfo(font).pixelSize();
    if (px <= 0) px = 13;
    ctx.serif = serifFamily();
    ctx.math = mathFamily();
    // A serif's letters are smaller than a sans's of the same size: the
    // math as big as the text by their x-heights (as KaTeX does).
    ctx.em = px;
    const qreal textX = QFontMetricsF(font).xHeight();
    const qreal mathX = QFontMetricsF(ctx.font(Variant::Italic, Style::Text)).xHeight();
    if (textX > 0.0 && mathX > 0.0) ctx.em = px * std::clamp(textX / mathX, 1.0, 1.25);
    ctx.dpr = devicePixelRatio > 0 ? devicePixelRatio : 1.0;

    Parser parser(tex, display);
    Ptr root = parser.formula();
    root->layout(ctx, display ? Style::Display : Style::Text);

    Typeset t;
    t.ok = parser.ok;
    const qreal margin = 1.0;
    t.width = root->w + root->italic() + 2 * margin;
    t.ascent = root->a + margin;
    t.descent = root->d + margin;
    const QSize size(std::max(1, int(std::ceil(t.width * ctx.dpr))),
                     std::max(1, int(std::ceil((t.ascent + t.descent) * ctx.dpr))));
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::TextAntialiasing);
        p.scale(ctx.dpr, ctx.dpr);
        p.setPen(QPen(colour));
        p.setBrush(colour);
        root->draw(p, ctx, margin, t.ascent);
    }
    image.setDevicePixelRatio(ctx.dpr);
    t.image = image;
    return t;
}

QImage centredOnAxis(const Typeset& math, const QFont& font, qreal* height)
{
    // Qt sets an inline object with AlignMiddle at (height + x/2) / 2 above
    // the baseline, x the x-height of its format's font: space above or
    // below puts the math's own baseline there.
    const qreal halfX = QFontMetrics(font).xHeight() / 2.0;
    const qreal above = math.ascent, below = math.descent;
    qreal top = 0.0, bottom = 0.0;
    if (above >= below + halfX) bottom = above - below - halfX;
    else top = below + halfX - above;
    const qreal h = top + above + below + bottom;
    if (height != nullptr) *height = h;
    const qreal dpr = math.image.devicePixelRatio();
    QImage image(QSize(math.image.width(), std::max(1, int(std::ceil(h * dpr)))), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    image.setDevicePixelRatio(dpr);
    {
        QPainter p(&image);
        p.drawImage(QPointF(0, top), math.image);
    }
    return image;
}

// ----------------------------------------------------------------------
void MathObject::install(QTextDocument* document)
{
    QAbstractTextDocumentLayout* layout = document->documentLayout();
    if (layout->handlerForObject(Type) == nullptr) layout->registerHandler(Type, new MathObject(document));
}

QTextCharFormat MathObject::format(const Typeset& math, const QFont& font, const QString& tex)
{
    qreal height = 0.0;
    const QImage image = centredOnAxis(math, font, &height);
    QTextCharFormat f;
    f.setObjectType(Type);
    f.setProperty(ImageProperty, image);
    f.setProperty(SizeProperty, QSizeF(math.width, height));
    f.setVerticalAlignment(QTextCharFormat::AlignMiddle);
    f.setFont(font);   // (its x-height places it)
    f.setToolTip(tex);
    return f;
}

QSizeF MathObject::intrinsicSize(QTextDocument*, int, const QTextFormat& format)
{
    return format.property(SizeProperty).toSizeF();
}

void MathObject::drawObject(QPainter* painter, const QRectF& rect, QTextDocument*, int, const QTextFormat& format)
{
    const QImage image = qvariant_cast<QImage>(format.property(ImageProperty));
    if (image.isNull()) return;
    painter->save();
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    painter->drawImage(rect, image);
    painter->restore();
}

// ----------------------------------------------------------------------
QList<Span> findMath(const QString& md)
{
    QList<Span> spans;
    const qsizetype n = md.size();
    qsizetype i = 0;
    bool lineStart = true;
    const auto escaped = [&md](qsizetype at) {
        int backslashes = 0;
        for (qsizetype k = at - 1; k >= 0 && md.at(k) == QLatin1Char('\\'); --k) ++backslashes;
        return backslashes % 2 == 1;
    };
    while (i < n) {
        const QChar c = md.at(i);
        // A fenced code block: to its fence.
        if (lineStart) {
            qsizetype k = i;
            while (k < n && k - i < 4 && md.at(k) == QLatin1Char(' ')) ++k;
            if (k + 2 < n && (md.mid(k, 3) == QLatin1String("```") || md.mid(k, 3) == QLatin1String("~~~"))) {
                const QString fence = md.mid(k, 3);
                qsizetype close = md.indexOf(QLatin1Char('\n'), k);
                while (close >= 0) {
                    qsizetype line = close + 1;
                    while (line < n && md.at(line) == QLatin1Char(' ')) ++line;
                    if (md.mid(line, 3) == fence) {
                        close = md.indexOf(QLatin1Char('\n'), line);
                        break;
                    }
                    close = md.indexOf(QLatin1Char('\n'), close + 1);
                }
                i = close < 0 ? n : close + 1;
                lineStart = true;
                continue;
            }
        }
        lineStart = c == QLatin1Char('\n');
        // Inline code: to the run of backticks as long.
        if (c == QLatin1Char('`')) {
            qsizetype run = 0;
            while (i + run < n && md.at(i + run) == QLatin1Char('`')) ++run;
            const QString ticks(run, QLatin1Char('`'));
            qsizetype close = md.indexOf(ticks, i + run);
            while (close >= 0 && close + run < n && md.at(close + run) == QLatin1Char('`'))
                close = md.indexOf(ticks, close + run + 1);
            i = close < 0 ? i + run : close + run;
            continue;
        }
        if (c == QLatin1Char('\\') && i + 1 < n) {
            const QChar next = md.at(i + 1);
            if (next == QLatin1Char('[') || next == QLatin1Char('(')) {
                const QString closing = next == QLatin1Char('[') ? QStringLiteral("\\]") : QStringLiteral("\\)");
                const qsizetype close = md.indexOf(closing, i + 2);
                if (close > i + 2) {
                    spans.append({i, close + 2 - i, md.mid(i + 2, close - i - 2).trimmed(), next == QLatin1Char('[')});
                    i = close + 2;
                    continue;
                }
            }
            i += 2;   // an escape: \$ is a dollar
            continue;
        }
        if (c == QLatin1Char('$') && !escaped(i)) {
            if (i + 1 < n && md.at(i + 1) == QLatin1Char('$')) {
                qsizetype close = md.indexOf(QLatin1String("$$"), i + 2);
                while (close >= 0 && escaped(close)) close = md.indexOf(QLatin1String("$$"), close + 1);
                if (close > i + 2) {
                    const QString tex = md.mid(i + 2, close - i - 2).trimmed();
                    if (!tex.isEmpty()) spans.append({i, close + 2 - i, tex, true});
                    i = close + 2;
                    continue;
                }
                i += 2;
                continue;
            }
            // $...$: not before a space; ends before a $ that follows no
            // space and precedes no digit; not across a blank line.
            if (i + 1 < n && !md.at(i + 1).isSpace()) {
                qsizetype k = i + 1;
                qsizetype close = -1;
                while (k < n) {
                    const QChar ck = md.at(k);
                    if (ck == QLatin1Char('\\')) {
                        k += 2;
                        continue;
                    }
                    if (ck == QLatin1Char('\n') && k + 1 < n && md.at(k + 1) == QLatin1Char('\n')) break;
                    if (ck == QLatin1Char('$')) {
                        if (!md.at(k - 1).isSpace() && !(k + 1 < n && md.at(k + 1).isDigit())) close = k;
                        break;
                    }
                    ++k;
                }
                if (close > i + 1) {
                    spans.append({i, close + 1 - i, md.mid(i + 1, close - i - 1), false});
                    i = close + 1;
                    continue;
                }
            }
        }
        ++i;
    }
    return spans;
}

} // namespace qucs_s::math
