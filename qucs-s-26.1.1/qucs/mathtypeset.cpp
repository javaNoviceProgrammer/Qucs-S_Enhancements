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
#include <QScopeGuard>
#include <QSet>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextList>
#include <QTextTable>
#include <QVarLengthArray>

#include <functional>

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
namespace {
Typeset typesetOrMeasure(const QString& tex, const QFont& font, const QColor& colour, bool display,
                         qreal devicePixelRatio, bool draw)
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
    if (!draw) return t;
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
} // namespace

Typeset typeset(const QString& tex, const QFont& font, const QColor& colour, bool display, qreal devicePixelRatio)
{
    return typesetOrMeasure(tex, font, colour, display, devicePixelRatio, true);
}

Typeset measure(const QString& tex, const QFont& font, bool display)
{
    return typesetOrMeasure(tex, font, QColor(0, 0, 0), display, 1.0, false);
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
namespace {

// \a md with each lone surrogate (not UTF-16) made U+FFFD, as Qt's
// importer reads it (toUtf8()). The text the walks below are given.
QString validUtf16(const QString& md)
{
    if (md.isValidUtf16()) return md;
    QString out = md;
    for (qsizetype k = 0; k < out.size(); ++k) {
        if (out.at(k).isHighSurrogate() && k + 1 < out.size() && out.at(k + 1).isLowSurrogate()) ++k;
        else if (out.at(k).isSurrogate()) out[k] = QChar::ReplacementCharacter;
    }
    return out;
}

// \a re matched at \a at in \a md - UTF-16 (validUtf16()): match() checks
// the whole text again at each call else, and the walks call it at each
// line, at each tag. (A reply of 130,000 characters took 1.6 s.)
QRegularExpressionMatch matchAt(const QRegularExpression& re, const QString& md, qsizetype at)
{
    return re.match(md, at, QRegularExpression::NormalMatch, QRegularExpression::DontCheckSubjectStringMatchOption);
}

// The same, in \a md to \a end only: a tag within its line, paragraph or
// block. (A pattern that needs a character far on - a tag's '>' - starts
// (*NO_START_OPT): PCRE looks for it through the rest of the text else,
// from each place matched at.)
QRegularExpressionMatch matchAt(const QRegularExpression& re, const QString& md, qsizetype at, qsizetype end)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    return re.matchView(QStringView(md).left(end), at, QRegularExpression::NormalMatch, QRegularExpression::DontCheckSubjectStringMatchOption);
#else
    return re.match(QStringView(md).left(end), at, QRegularExpression::NormalMatch, QRegularExpression::DontCheckSubjectStringMatchOption);
#endif
}

// \a re matched anywhere in \a view (match() on a view is deprecated).
QRegularExpressionMatch matchIn(const QRegularExpression& re, QStringView view)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    return re.matchView(view);
#else
    return re.match(view);
#endif
}

// Where a search for each closer - </pre>, </script>, </style>,
// </textarea>, </code>, -->, ]]>, ?>, a declaration's '>' - found none
// from, in the text outsideCode() walks, while it does: a text of many
// unclosed <!-- or <pre> is not searched to its end again from each.
struct Unclosed {
    const QString* text = nullptr;
    qsizetype from[9] = {};
};
thread_local Unclosed unclosed;

// The first \a closer (of the \a kind above) in \a md from \a from, or -1.
qsizetype closerFrom(const QString& md, const QString& closer, int kind, qsizetype from)
{
    const bool kept = unclosed.text == &md && kind >= 0;
    if (kept && unclosed.from[kind] >= 0 && from >= unclosed.from[kind]) return -1;
    const qsizetype at = md.indexOf(closer, from, Qt::CaseInsensitive);
    if (at < 0 && kept) unclosed.from[kind] = unclosed.from[kind] < 0 ? from : std::min(unclosed.from[kind], from);
    return at;
}

// The kind of the closer of a <pre>, <script>, <style>, <textarea>, <code>.
int rawKind(const QString& name)
{
    static const QStringList names{QStringLiteral("pre"), QStringLiteral("script"), QStringLiteral("style"), QStringLiteral("textarea"),
                                   QStringLiteral("code")};
    return int(names.indexOf(name.toLower()));
}

// The comment, or its like, that starts at \a i: the length of its
// opening, and its closing - <!-- -->, <![CDATA[ ]]>, <? ?>, a declaration
// <!DOCTYPE > - with which of them (0 to 3); none (0) if none starts there.
struct Like {
    qsizetype opener;
    QLatin1StringView closer;
    int kind;
};
Like likeAt(const QString& md, qsizetype i)
{
    const QStringView at = QStringView(md).mid(i);
    if (at.startsWith(QLatin1String("<!--"))) return {4, QLatin1StringView("-->"), 0};
    if (at.startsWith(QLatin1String("<![CDATA["))) return {9, QLatin1StringView("]]>"), 1};
    if (at.startsWith(QLatin1String("<?"))) return {2, QLatin1StringView("?>"), 2};
    if (at.size() > 2 && at.startsWith(QLatin1String("<!")) && at.at(2).isLetter() && at.at(2).unicode() < 128)
        return {2, QLatin1StringView(">"), 3};
    return {0, QLatin1StringView(), 0};
}

// The end of the HTML block (CommonMark) whose line starts with the '<' at
// \a k, or -1 when none starts there: <pre>, <script>, <style> or
// <textarea> to the line of its closing tag; a comment, or its like, to
// the line of its "-->" (likeAt()); a block element's tag (<div>, <hr>,
// <details>...), or - not after a paragraph's line - a whole tag alone on
// its line, to a blank line. What is in one is HTML: no code, no escape. (One not
// closed is none: the tag is made text.)
qsizetype htmlBlockEnd(const QString& md, qsizetype k, bool noParagraph)
{
    static const QRegularExpression raw(QStringLiteral("\\G<(pre|script|style|textarea)(?=[\\s>]|$)"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression block(
        QStringLiteral("\\G</?(?:address|article|aside|base|basefont|blockquote|body|caption|center|col|colgroup|dd|details|dialog|dir|"
                       "div|dl|dt|fieldset|figcaption|figure|footer|form|frame|frameset|h[1-6]|head|header|hr|html|iframe|legend|li|"
                       "link|main|menu|menuitem|nav|noframes|ol|optgroup|option|p|param|search|section|summary|table|tbody|td|"
                       "tfoot|th|thead|title|tr|track|ul)(?=[\\s>]|/>|$)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression whole(QStringLiteral(
        "\\G(?:<[A-Za-z][A-Za-z0-9-]*(?:\\s+[A-Za-z_:][A-Za-z0-9_.:-]*(?:\\s*=\\s*(?:[^\\s\"'=<>`]+|'[^'\\n]*'|\"[^\"\\n]*\"))?)*\\s*/?>|</[A-Za-z][A-Za-z0-9-]*\\s*>)[ \\t]*(?=\\r?\\n|$)"));
    static const QRegularExpression blankLine(QStringLiteral("\\n[ \\t]*\\r?\\n"));
    const qsizetype n = md.size();
    const auto lineEnd = [&md, n](qsizetype from) {
        const qsizetype end = md.indexOf(QLatin1Char('\n'), from);
        return end < 0 ? n : end + 1;
    };
    if (const QRegularExpressionMatch m = matchAt(raw, md, k); m.hasMatch()) {
        const qsizetype close = closerFrom(md, QStringLiteral("</%1>").arg(m.captured(1)), rawKind(m.captured(1)), m.capturedEnd());
        return close < 0 ? -1 : lineEnd(close);
    }
    if (const Like like = likeAt(md, k); like.opener > 0) {
        const qsizetype close = closerFrom(md, QString(like.closer), 5 + like.kind, k + like.opener);
        return close < 0 ? -1 : lineEnd(close);
    }
    // (A whole tag on its line: md4c looks at the line.)
    if (matchAt(block, md, k).hasMatch() || (noParagraph && matchAt(whole, md, k, lineEnd(k)).hasMatch())) {
        const QRegularExpressionMatch m = matchAt(blankLine, md, k);
        return m.hasMatch() ? m.capturedStart() + 1 : n;
    }
    return -1;
}

// Where what starts at \a k in the line at \a line - after the marks of
// quotes and of lists' items, or indented \a inList - ends with them (a
// fence, an HTML block; no lazy line goes on with either): at the first
// line after it that does not go on in each of them, as md4c reads it - a
// quote's line has its '>', an item's is blank or indented to its content;
// the end of the text when no marks. \a matched, if given, is where the
// marks that line still has end (those of the containers it goes on in).
qsizetype containerEnd(const QString& md, qsizetype line, qsizetype k, bool inList = false, qsizetype* matched = nullptr)
{
    const qsizetype n = md.size();
    const auto isSpace = [&md](qsizetype at) { return md.at(at) == QLatin1Char(' ') || md.at(at) == QLatin1Char('\t'); };
    const auto advance = [&md](qsizetype at, int col) { return col + (md.at(at) == QLatin1Char('\t') ? 4 - col % 4 : 1); };
    // The containers, the outermost first: a quote (0), a list's item (the
    // columns from where it starts to its content).
    QVarLengthArray<int, 8> containers;
    qsizetype p = line;
    for (int col = 0; p < k;) {
        int c = col;
        qsizetype q = p;
        for (; q < k && isSpace(q); ++q) c = advance(q, c);
        if (q >= k) {
            if (containers.isEmpty() && inList && c > col) containers.append(c - col);
            break;
        }
        if (md.at(q) == QLatin1Char('>')) {
            containers.append(0);
            col = c + 1;
            p = q + 1;
            if (p < k && md.at(p) == QLatin1Char(' ')) ++col, ++p;
            continue;
        }
        qsizetype m = q;
        while (m < k && md.at(m) >= QLatin1Char('0') && md.at(m) <= QLatin1Char('9')) ++m;
        ++m;   // (the marker, or the '.' or ')' after its number)
        const int after = c + int(m - q);
        int to = after;
        qsizetype r = m;
        for (; r < k && isSpace(r); ++r) to = advance(r, to);
        const int content = to - after >= 1 && to - after <= 4 ? to : after + 1;
        containers.append(content - col);
        col = content;
        p = r;
    }
    if (containers.isEmpty()) return n;
    for (qsizetype at = md.indexOf(QLatin1Char('\n'), line); at >= 0; at = md.indexOf(QLatin1Char('\n'), at + 1)) {
        const qsizetype from = at + 1;
        qsizetype eol = md.indexOf(QLatin1Char('\n'), from);
        if (eol < 0) eol = n;
        p = from;
        int col = 0;
        for (const int width : containers) {
            int c = col;
            qsizetype q = p;
            for (; q < eol && isSpace(q); ++q) c = advance(q, c);
            const bool blank = q >= eol || md.at(q) == QLatin1Char('\r');
            if (width == 0) {
                if (blank || c - col > 3 || md.at(q) != QLatin1Char('>')) {
                    if (matched) *matched = p;
                    return from;
                }
                col = c + 1;
                p = q + 1;
                if (p < eol && md.at(p) == QLatin1Char(' ')) ++col, ++p;
            } else if (!blank) {
                if (c - col < width) {
                    if (matched) *matched = p;
                    return from;
                }
                // (Its columns taken - a tab may go past them -, the rest
                // the next one's.)
                for (const int to = col + width; col < to; ++p) col = advance(p, col);
            }
        }
    }
    return n;
}

// The marks of a quote and of a list's item a line starts with: where its
// content does.
qsizetype contentAt(const QString& md, qsizetype line)
{
    static const QRegularExpression marks(QStringLiteral("\\G[ ]{0,3}(?:(?:>[ ]?|(?:[-*+]|\\d{1,9}[.)])[ ]+)[ ]{0,3})*"));
    const QRegularExpressionMatch m = matchAt(marks, md, line);
    return m.hasMatch() ? m.capturedEnd() : line;
}

// The length of the fence that starts at \a k - three backticks or tildes
// or more, a backtick fence's line holding no other backtick (else it is
// inline code, or text) -; 0 if none starts there.
qsizetype fenceAt(const QString& md, qsizetype k)
{
    const qsizetype n = md.size();
    if (k >= n || (md.at(k) != QLatin1Char('`') && md.at(k) != QLatin1Char('~'))) return 0;
    qsizetype run = 0;
    while (k + run < n && md.at(k + run) == md.at(k)) ++run;
    if (run < 3 || md.at(k) != QLatin1Char('`')) return run < 3 ? 0 : run;
    const qsizetype eol = md.indexOf(QLatin1Char('\n'), k);
    return QStringView(md).mid(k + run, (eol < 0 ? n : eol) - k - run).contains(QLatin1Char('`')) ? 0 : run;
}

// Whether the line whose content starts with the '<' at \a k starts an HTML
// block that may break into a paragraph - one of <pre>, <script>,
// <style>, <textarea>, a comment or its like (<?x?>, <![CDATA[,
// <!DOCTYPE), a block element's tag - closed or not.
bool startsHtmlBlock(const QString& md, qsizetype k)
{
    static const QRegularExpression raw(QStringLiteral("\\G<(?:pre|script|style|textarea)(?=[\\s>]|$)"), QRegularExpression::CaseInsensitiveOption);
    return matchAt(raw, md, k).hasMatch() || likeAt(md, k).opener > 0 || htmlBlockEnd(md, k, false) > 0;
}

// How far (in columns) the content of the line at \a line, to \a eol, is
// indented past its marks - a quote's '>' and a column after it, a list's
// marker and the spaces after it (one only, when five or more: the rest is
// code) -, as md4c reads them; -1 when the line has none. \a item is the
// column a list's item begun on the line has its content at, else -1;
// \a opened, if given, has each begun before a quote's '>' (- 1. x two).
int innerIndent(const QString& md, qsizetype line, qsizetype eol, int* item, QVarLengthArray<int, 8>* opened = nullptr)
{
    *item = -1;
    int col = 0;
    bool marked = false, space = false, quoted = false;   // (after a '>': a column of what follows is its space)
    for (qsizetype p = line;;) {
        int c = col;
        qsizetype q = p;
        for (; q < eol && (md.at(q) == QLatin1Char(' ') || md.at(q) == QLatin1Char('\t')); ++q)
            c += md.at(q) == QLatin1Char('\t') ? 4 - c % 4 : 1;
        const int indent = c - col - (space && c > col ? 1 : 0);
        if (q >= eol || indent >= 4) return marked ? indent : -1;
        if (md.at(q) == QLatin1Char('>')) {
            col = c + 1;
            p = q + 1;
            marked = space = quoted = true;
            continue;
        }
        qsizetype m = q;
        if (md.at(m) == QLatin1Char('-') || md.at(m) == QLatin1Char('*') || md.at(m) == QLatin1Char('+')) {
            ++m;
        } else {
            while (m < eol && m - q < 9 && md.at(m) >= QLatin1Char('0') && md.at(m) <= QLatin1Char('9')) ++m;
            if (m == q || m >= eol || (md.at(m) != QLatin1Char('.') && md.at(m) != QLatin1Char(')'))) return marked ? indent : -1;
            ++m;
        }
        if (m < eol && md.at(m) != QLatin1Char(' ') && md.at(m) != QLatin1Char('\t')) return marked ? indent : -1;
        const int after = c + int(m - q);
        int to = after;
        qsizetype r = m;
        for (; r < eol && (md.at(r) == QLatin1Char(' ') || md.at(r) == QLatin1Char('\t')); ++r)
            to += md.at(r) == QLatin1Char('\t') ? 4 - to % 4 : 1;
        *item = r >= eol || to - after >= 5 ? after + 1 : to;
        if (opened && !quoted) opened->append(*item);
        if (r >= eol || to - after >= 5) return r >= eol ? 0 : to - after - 1;
        col = to;
        p = r;
        marked = true;
        space = false;
    }
}

// Where the paragraph the place \a at is in ends: at a blank line, or at a
// line that starts another block - a quote, a heading, a fence, a list's
// item, an HTML block that may break into a paragraph (indented however
// far: so md4c reads it).
qsizetype paragraphEnd(const QString& md, qsizetype at)
{
    const qsizetype n = md.size();
    static const QRegularExpression starts(QStringLiteral("\\G[ ]{0,3}(?:>|#{1,6}(?:[ \\t]|$)|[-*+][ \\t]|1[.)][ \\t])"));
    static const QRegularExpression heading(QStringLiteral("\\G#{1,6}(?:[ \\t]|$)"));
    // (A heading is its line.)
    const qsizetype lineStart = at == 0 ? 0 : md.lastIndexOf(QLatin1Char('\n'), at - 1) + 1;
    if (matchAt(heading, md, contentAt(md, lineStart)).hasMatch()) {
        const qsizetype eol = md.indexOf(QLatin1Char('\n'), at);
        return eol < 0 ? n : eol;
    }
    for (qsizetype line = md.indexOf(QLatin1Char('\n'), at); line >= 0; line = md.indexOf(QLatin1Char('\n'), line + 1)) {
        qsizetype p = line + 1;
        while (p < n && (md.at(p) == QLatin1Char(' ') || md.at(p) == QLatin1Char('\t'))) ++p;
        if (p >= n || md.at(p) == QLatin1Char('\n') || md.at(p) == QLatin1Char('\r')) return line;
        if (matchAt(starts, md, line + 1).hasMatch() || (p - line - 1 < 4 && fenceAt(md, p) > 0)) return line;
        if (md.at(p) == QLatin1Char('<') && startsHtmlBlock(md, p)) return line;
    }
    return n;
}

// Walks the Markdown \a md past its code - fenced and indented blocks,
// inline code, and with \a htmlCode <code> and <pre> too (as GitHub shows
// them; to Qt's importer what is in them is HTML still) - and past what a
// backslash escapes. \a at is called at each other position - with
// whether it is in an HTML block - and returns where to go on from (past
// what it took there), or -1 to go on with the next character. \a block,
// if given, is called with where each HTML block starts (its '<') and ends.
// \a rowEnd, if given, is kept at the end of the line the walk is in when
// that is a table's row, and -1 else: md4c reads each row on its own.
void outsideCode(const QString& md, bool htmlCode, const std::function<qsizetype(qsizetype, bool)>& at,
                 const std::function<void(qsizetype, qsizetype)>& block = {}, qsizetype* rowEnd = nullptr)
{
    const qsizetype n = md.size();
    // (The closers found none of: of this text, while it is walked.)
    const Unclosed outer = unclosed;
    unclosed = Unclosed{&md, {-1, -1, -1, -1, -1, -1, -1, -1, -1}};
    const auto restore = qScopeGuard([&outer] { unclosed = outer; });
    qsizetype i = 0;
    bool lineStart = true;
    // An indented code block (CommonMark): lines indented four columns or
    // more past their quote's or list's item's content - in an item, its
    // first line five past its marker - after a line no paragraph's - blank,
    // a heading, a rule, a fence's end, a table's row - or another such line.
    // What is in it is code, as in a fence.
    bool inIndentedCode = false, inList = false;
    QVarLengthArray<int, 8> items;   // (the columns the list's items the walk is in have their content at)
    bool itemEmpty = false;          // (the line before an item's, nothing after its marker)
    bool paragraph = false;   // the line a paragraph's (not a heading's, a rule's, code, HTML, empty in its marks)
    static const QRegularExpression notParagraph(QStringLiteral(
        "\\G(?:#{1,6}(?=[ \\t\\r\\n]|$)|(?:(?:\\*[ \\t]*){3,}|(?:-[ \\t]*){3,}|(?:_[ \\t]*){3,}|=+[ \\t]*)(?=\\r?\\n|$)|[ \\t]*(?=\\r?\\n|$))"));
    // A table (md4c's): a paragraph's first line - after no paragraph's, or
    // a list's item's, or in a quote begun (one less is the paragraph's lazy
    // line) -, then in the same quote a line of its columns' dashes and a
    // '|'; then each line is a row - an HTML tag's, a fence's, a heading's
    // too - till a blank line, another quote, a list's item, indented code or
    // a rule.
    bool inTable = false, paragraphStart = false;   // (the line its paragraph's first)
    int quotes = 0, tableQuotes = 0;                // (the line's quote marks; the table's)
    static const QRegularExpression itemMark(QStringLiteral("(?:[-*+]|\\d{1,9}[.)])(?:[ \\t]|$)"));
    static const QRegularExpression dashes(QStringLiteral("\\G\\|?[ \\t]*:?-+:?[ \\t]*(?:\\|[ \\t]*:?-+:?[ \\t]*)*\\|?[ \\t]*(?=\\r?\\n|$)"));
    static const QRegularExpression rule(QStringLiteral("\\G(?:(?:\\*[ \\t]*){3,}|(?:-[ \\t]*){3,}|(?:_[ \\t]*){3,})(?=\\r?\\n|$)"));
    // (A line of a table's dashes: its first character looked at first - PCRE
    // looks ahead for the '-' the pattern needs, a long way in a long
    // paragraph.)
    const auto dashesAt = [&md](qsizetype line, qsizetype eol) {
        const qsizetype d = contentAt(md, line);
        return d < eol && (md.at(d) == QLatin1Char('|') || md.at(d) == QLatin1Char('-') || md.at(d) == QLatin1Char(':'))
               && matchAt(dashes, md, d).hasMatch() && QStringView(md).mid(line, eol - line).contains(QLatin1Char('|'));
    };
    qsizetype row = -1;   // (the end of the line, when a table's row)
    // (The paragraph inline code was last looked for in, from where to its
    // end: a long paragraph, a table's rows, are not looked over to their
    // end again from each backtick.)
    qsizetype paragraphFrom = -1, paragraphTo = -1;
    // (The line read from past a fence's run, from where - see below.)
    qsizetype restLine = -1, restAt = -1;
    qsizetype greaterAt = -2;   // (the first '>' from the last <code, <pre looked at; -1 none)
    while (i < n) {
        const QChar c = md.at(i);
        const qsizetype rest = lineStart && i == restLine ? restAt : -1;
        const bool afterParagraph = paragraph, afterStart = paragraphStart;
        const int afterQuotes = quotes;
        if (lineStart) {
            const QStringView marks = QStringView(md).mid(i, contentAt(md, i) - i);
            quotes = int(marks.count(QLatin1Char('>')));
            qsizetype eol = md.indexOf(QLatin1Char('\n'), i);
            if (eol < 0) eol = n;
            int indent = 0;
            qsizetype k = i;
            for (; k < eol && (md.at(k) == QLatin1Char(' ') || md.at(k) == QLatin1Char('\t')); ++k)
                indent += md.at(k) == QLatin1Char('\t') ? 4 - indent % 4 : 1;
            const bool blank = k == eol || (k + 1 == eol && md.at(k) == QLatin1Char('\r'));
            const bool tableEnds = inTable && (blank || indent >= 4 || quotes != tableQuotes || matchIn(itemMark, marks).hasMatch()
                                               || matchAt(rule, md, contentAt(md, i)).hasMatch());
            if (tableEnds) inTable = false;
            // (In a quote, a list's item: four columns past their content,
            // or a list's item's first line five past its marker. A quote,
            // an item begun on the line starts its code anew.)
            int itemAt = -1;
            QVarLengthArray<int, 8> opened;
            const int inner = innerIndent(md, i, eol, &itemAt, &opened);
            static const QRegularExpression item(QStringLiteral("^(?:[-*+]|\\d{1,9}[.)])(?:[ \\t]|$)"));
            static const QRegularExpression emptyItem(QStringLiteral("^(?:[-*+]|\\d{1,9}[.)])[ \\t]*\\r?$"));
            const bool ruled = !blank && indent < 4 && matchAt(rule, md, k).hasMatch();   // (* * *, - ---: a rule, no list's item)
            const bool startsItem = !blank && !ruled && matchIn(item, QStringView(md).mid(k, eol - k)).hasMatch();
            // (The list's items the line goes on in, as md4c has them: those
            // it is indented to the content of - a paragraph's lazy line goes
            // on in all. One begun with nothing on its line ends at a blank
            // line after it.)
            if (blank) {
                if (itemEmpty && !items.isEmpty()) items.pop_back();
            } else if (!afterParagraph || startsItem || quotes > 0
                       || (indent < 4 && (matchAt(notParagraph, md, k).hasMatch() || fenceAt(md, k) > 0
                                          || (md.at(k) == QLatin1Char('<') && startsHtmlBlock(md, k))))) {
                while (!items.isEmpty() && indent < items.back()) items.pop_back();
            }
            itemEmpty = startsItem && matchIn(emptyItem, QStringView(md).mid(k, eol - k)).hasMatch();
            inList = !items.isEmpty();
            bool code = false;
            if (!blank && (quotes > 0 || (itemAt >= 0 && indent < 4)))
                code = inner >= 4 && (quotes > afterQuotes || itemAt >= 0 || !afterParagraph || inIndentedCode);
            else if (!blank && inList)
                code = indent - items.back() >= 4 && (!afterParagraph || inIndentedCode);
            else if (!blank)
                code = indent >= 4 && (!afterParagraph || inIndentedCode || tableEnds);
            if (startsItem && !(code && itemAt < 0)) {
                if (opened.isEmpty()) opened.append(indent + int(QStringView(md).mid(k, eol - k).indexOf(QLatin1Char(' ')) + 1));
                items.append(opened.constData(), opened.size());
                inList = true;
            }
            if (code) {
                inIndentedCode = true;
                paragraph = false;
                i = eol < n ? eol + 1 : n;
                continue;
            }
            if (!blank && !inTable && afterParagraph && afterStart && quotes == afterQuotes && dashesAt(i, eol)) {
                inTable = true;
                tableQuotes = quotes;
            }
            if (!blank) inIndentedCode = false;
            paragraph = !blank && !ruled && !matchAt(notParagraph, md, rest >= 0 ? rest : contentAt(md, i)).hasMatch();
            paragraphStart = paragraph && (!afterParagraph || quotes > afterQuotes || matchIn(itemMark, marks).hasMatch());
            // (A table's row: its header too, a paragraph's first line before
            // its dashes in the same quote.)
            row = inTable ? eol : -1;
            if (!inTable && paragraphStart && eol < n) {
                const qsizetype next = eol + 1;
                qsizetype nextEol = md.indexOf(QLatin1Char('\n'), next);
                if (nextEol < 0) nextEol = n;
                if (QStringView(md).mid(next, contentAt(md, next) - next).count(QLatin1Char('>')) == quotes && dashesAt(next, nextEol)) row = eol;
            }
            if (rowEnd) *rowEnd = row;
        }
        // An HTML block, read as Qt's importer reads it: each place in it,
        // there being no code and no escape in it. (It may start in a quote
        // or a list's item: after their marks.)
        if (!htmlCode && lineStart && !inTable) {
            const qsizetype marked = contentAt(md, i);
            qsizetype k = rest >= 0 ? rest : marked;
            if (afterParagraph) {
                // (After a paragraph's line: one that starts an HTML block
                // ends it, indented however far - md4c's reading.)
                qsizetype p = k;
                while (p < n && (md.at(p) == QLatin1Char(' ') || md.at(p) == QLatin1Char('\t'))) ++p;
                if (p < n && md.at(p) == QLatin1Char('<') && startsHtmlBlock(md, p)) k = p;
            }
            qsizetype end = k < n && md.at(k) == QLatin1Char('<') ? htmlBlockEnd(md, k, !afterParagraph) : -1;
            if (end > 0) end = std::min(end, containerEnd(md, i, rest >= 0 ? marked : k, inList));
            if (end > 0) {
                if (block) block(k, end);
                qsizetype p = i;
                while (p < end) {
                    const qsizetype to = at(p, true);
                    p = to > p ? to : p + 1;
                }
                // (On from what it took: a comment past the block's end.
                // Indented code may follow, as after a blank line.)
                i = std::max(end, p);
                lineStart = i == 0 || md.at(i - 1) == QLatin1Char('\n');
                paragraph = false;
                continue;
            }
        }
        // A fenced code block (in a quote or a list's item too): to its
        // closing fence - as long or longer, nothing after it -, or to the
        // quote's end. (A backtick fence's line holds no other backtick: else
        // it is inline code, or text.)
        if (lineStart && !inTable) {
            const qsizetype k = rest >= 0 ? rest : contentAt(md, i);
            const qsizetype eol = md.indexOf(QLatin1Char('\n'), k);
            if (const qsizetype run = fenceAt(md, k); run > 0) {
                const QChar mark = md.at(k);
                qsizetype matched = -1;
                const qsizetype limit = containerEnd(md, i, rest >= 0 ? contentAt(md, i) : k, false, &matched);
                qsizetype close = limit;
                bool closed = false;
                for (qsizetype line = eol < 0 ? n : eol + 1; line < limit;) {
                    const qsizetype next = md.indexOf(QLatin1Char('\n'), line);
                    const qsizetype end = next < 0 ? n : next;
                    qsizetype p = contentAt(md, line), length = 0;
                    while (p < end && md.at(p) == mark) ++p, ++length;
                    while (p < end && md.at(p).isSpace()) ++p;
                    if (length >= run && p == end) {
                        close = next < 0 ? n : next + 1;
                        closed = true;
                        break;
                    }
                    line = next < 0 ? n : next + 1;
                }
                i = close;
                lineStart = true;
                paragraph = false;
                // (md4c looks at the line after a fence its quote or list's
                // item ended as at the fence's closing first, and reads what
                // follows the run of the fence's character - and its spaces,
                // a run as long - as a line: ">```" then "```<n>" an HTML
                // block, "`<p" one too; "``" a blank line.)
                if (!closed && limit < n) {
                    qsizetype p = matched;   // (past the marks of the containers it goes on in)
                    int columns = 0;
                    for (; p < n && (md.at(p) == QLatin1Char(' ') || md.at(p) == QLatin1Char('\t')); ++p)
                        columns += md.at(p) == QLatin1Char('\t') ? 4 - columns % 4 : 1;
                    qsizetype q = p;
                    while (q < n && md.at(q) == mark) ++q;
                    if (columns < 4 && q > p) {
                        if (q - p >= run)
                            while (q < n && md.at(q) == QLatin1Char(' ')) ++q;
                        if (q >= n || md.at(q) == QLatin1Char('\n') || md.at(q) == QLatin1Char('\r')) {
                            const qsizetype next = md.indexOf(QLatin1Char('\n'), q);
                            i = next < 0 ? n : next + 1;
                        } else {
                            restLine = limit;
                            restAt = q;
                        }
                    }
                }
                continue;
            }
        }
        lineStart = c == QLatin1Char('\n');
        // HTML code: <code>...</code> and <pre>...</pre>, as GitHub leaves them.
        // (To the next '>': no further than it, none if none.)
        if (htmlCode && c == QLatin1Char('<') && i + 4 < n) {
            static const QRegularExpression open(QStringLiteral("(*NO_START_OPT)\\G<(code|pre)(?=[\\s>])[^>]*>"), QRegularExpression::CaseInsensitiveOption);
            if (greaterAt != -1 && greaterAt < i) greaterAt = md.indexOf(QLatin1Char('>'), i);
            const QRegularExpressionMatch m = greaterAt < 0 ? QRegularExpressionMatch() : matchAt(open, md, i, greaterAt + 1);
            if (m.hasMatch()) {
                const qsizetype close = closerFrom(md, QStringLiteral("</%1>").arg(m.captured(1)), rawKind(m.captured(1)), m.capturedEnd());
                if (close >= 0) {
                    i = close + m.captured(1).size() + 3;
                    continue;
                }
            }
        }
        // Inline code: to the next run of backticks as long, in the same
        // paragraph (else the backticks are text).
        if (c == QLatin1Char('`')) {
            qsizetype run = 0;
            while (i + run < n && md.at(i + run) == QLatin1Char('`')) ++run;
            if (i < paragraphFrom || i >= paragraphTo) {
                paragraphFrom = i;
                paragraphTo = paragraphEnd(md, i);
            }
            const qsizetype limit = row >= 0 ? std::min(row, paragraphTo) : paragraphTo;
            qsizetype close = -1;
            for (qsizetype k = i + run; k < limit && close < 0;) {
                if (md.at(k) != QLatin1Char('`')) {
                    ++k;
                    continue;
                }
                qsizetype length = 0;
                while (k + length < n && md.at(k + length) == QLatin1Char('`')) ++length;
                if (length == run) close = k;
                k += length;
            }
            i = close < 0 ? i + run : close + run;
            continue;
        }
        if (const qsizetype to = at(i, false); to > i) {
            i = to;
            continue;
        }
        // (An escape: \$ is a dollar, \<br> no tag; a backslash at a line's
        // end is a line break, and the line after it starts as any does.)
        i += c == QLatin1Char('\\') && i + 1 < n && md.at(i + 1) != QLatin1Char('\n') ? 2 : 1;
    }
}

} // namespace

QList<Span> findMath(const QString& markdown)
{
    const QString md = validUtf16(markdown);   // (as long: the places are the same)
    QList<Span> spans;
    const qsizetype n = md.size();
    const auto escaped = [&md](qsizetype at) {
        int backslashes = 0;
        for (qsizetype k = at - 1; k >= 0 && md.at(k) == QLatin1Char('\\'); --k) ++backslashes;
        return backslashes % 2 == 1;
    };
    outsideCode(md, true, [&](qsizetype i, bool) -> qsizetype {
        const QChar c = md.at(i);
        if (c == QLatin1Char('\\') && i + 1 < n) {
            const QChar next = md.at(i + 1);
            if (next == QLatin1Char('[') || next == QLatin1Char('(')) {
                const QString closing = next == QLatin1Char('[') ? QStringLiteral("\\]") : QStringLiteral("\\)");
                const qsizetype close = md.indexOf(closing, i + 2);
                if (close > i + 2) {
                    spans.append({i, close + 2 - i, md.mid(i + 2, close - i - 2).trimmed(), next == QLatin1Char('[')});
                    return close + 2;
                }
            }
            return -1;
        }
        if (c == QLatin1Char('$') && !escaped(i)) {
            if (i + 1 < n && md.at(i + 1) == QLatin1Char('$')) {
                qsizetype close = md.indexOf(QLatin1String("$$"), i + 2);
                while (close >= 0 && escaped(close)) close = md.indexOf(QLatin1String("$$"), close + 1);
                if (close > i + 2) {
                    const QString tex = md.mid(i + 2, close - i - 2).trimmed();
                    if (!tex.isEmpty()) spans.append({i, close + 2 - i, tex, true});
                    return close + 2;
                }
                return i + 2;
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
                    return close + 1;
                }
            }
        }
        return -1;
    });
    return spans;
}

} // namespace qucs_s::math

namespace qucs_s::markdown {

namespace {

// Whether \a name (in lower case) is an HTML void element: br, hr, img...
bool isVoid(const QString& name)
{
    static const QSet<QString> names{QStringLiteral("area"), QStringLiteral("base"), QStringLiteral("br"), QStringLiteral("col"),
                                     QStringLiteral("embed"), QStringLiteral("hr"), QStringLiteral("img"), QStringLiteral("input"),
                                     QStringLiteral("link"), QStringLiteral("meta"), QStringLiteral("source"), QStringLiteral("track"),
                                     QStringLiteral("wbr")};
    return names.contains(name);
}

// The elements of HTML (Qt's importer draws them, or leaves their text);
// a tag of another name - <name>, <T>, <int> - is none, and text.
bool isElement(const QString& name)
{
    static const QSet<QString> names{
        QStringLiteral("a"), QStringLiteral("abbr"), QStringLiteral("address"), QStringLiteral("article"), QStringLiteral("aside"),
        QStringLiteral("audio"), QStringLiteral("b"), QStringLiteral("bdi"), QStringLiteral("bdo"), QStringLiteral("big"),
        QStringLiteral("blockquote"), QStringLiteral("body"), QStringLiteral("button"), QStringLiteral("canvas"), QStringLiteral("caption"),
        QStringLiteral("center"), QStringLiteral("cite"), QStringLiteral("code"), QStringLiteral("colgroup"), QStringLiteral("data"),
        QStringLiteral("dd"), QStringLiteral("del"), QStringLiteral("details"), QStringLiteral("dfn"), QStringLiteral("dialog"),
        QStringLiteral("div"), QStringLiteral("dl"), QStringLiteral("dt"), QStringLiteral("em"), QStringLiteral("fieldset"),
        QStringLiteral("figcaption"), QStringLiteral("figure"), QStringLiteral("font"), QStringLiteral("footer"), QStringLiteral("form"),
        QStringLiteral("h1"), QStringLiteral("h2"), QStringLiteral("h3"), QStringLiteral("h4"), QStringLiteral("h5"), QStringLiteral("h6"),
        QStringLiteral("head"), QStringLiteral("header"), QStringLiteral("html"), QStringLiteral("i"), QStringLiteral("iframe"),
        QStringLiteral("ins"), QStringLiteral("kbd"), QStringLiteral("label"), QStringLiteral("legend"), QStringLiteral("li"),
        QStringLiteral("main"), QStringLiteral("mark"), QStringLiteral("menu"), QStringLiteral("nav"), QStringLiteral("nobr"),
        QStringLiteral("noscript"), QStringLiteral("object"), QStringLiteral("ol"), QStringLiteral("optgroup"), QStringLiteral("option"),
        QStringLiteral("output"), QStringLiteral("p"), QStringLiteral("picture"), QStringLiteral("pre"), QStringLiteral("q"),
        QStringLiteral("rp"), QStringLiteral("rt"), QStringLiteral("ruby"), QStringLiteral("s"), QStringLiteral("samp"),
        QStringLiteral("script"), QStringLiteral("section"), QStringLiteral("select"), QStringLiteral("small"), QStringLiteral("span"),
        QStringLiteral("strike"), QStringLiteral("strong"), QStringLiteral("style"), QStringLiteral("sub"), QStringLiteral("summary"),
        QStringLiteral("sup"), QStringLiteral("svg"), QStringLiteral("table"), QStringLiteral("tbody"), QStringLiteral("td"),
        QStringLiteral("template"), QStringLiteral("textarea"), QStringLiteral("tfoot"), QStringLiteral("th"), QStringLiteral("thead"),
        QStringLiteral("time"), QStringLiteral("title"), QStringLiteral("tr"), QStringLiteral("tt"), QStringLiteral("u"),
        QStringLiteral("ul"), QStringLiteral("var"), QStringLiteral("video")};
    return names.contains(name) || isVoid(name);
}

} // namespace

namespace {

QString htmlBalancedOnce(const QString& md);

} // namespace

QString htmlBalanced(const QString& md)
{
    // Again till nothing changes: a tag made text can make a line no HTML
    // block any more (a lone <p> it began with), and what follows it is
    // Markdown then - its escapes and code count again. (Each round only
    // takes tags away: it ends.)
    QString out = htmlBalancedOnce(math::validUtf16(md));
    for (int round = 0; round < 8; ++round) {
        const QString again = htmlBalancedOnce(out);
        if (again == out) break;
        out = again;
    }
    return out;
}

namespace {

QString htmlBalancedOnce(const QString& md)
{
    // The tags outside code, as CommonMark has them (md4c too): <name
    // attributes>, </name>, <name/> - an attribute a name, then perhaps '='
    // and a value, one in quotes holding any but a line's end (<img
    // alt="a>b">). <b "x">, <b/x>, <a href=x?y=z> are text; </b/> is a tag
    // to md4c. (Not an autolink, <https://...> or <me@example.org>, either.)
    static const QRegularExpression tagAt(
        QStringLiteral("(*NO_START_OPT)\\G<(?:(/)([A-Za-z][A-Za-z0-9-]*)\\s*/?|([A-Za-z][A-Za-z0-9-]*)(?:\\s+[A-Za-z_:][A-Za-z0-9_.:-]*(?:\\s*=\\s*(?:[^\\s\"'=<>`]+|'[^'\\n]*'|\"[^\"\\n]*\"))?)*\\s*/?)>"));
    static const QRegularExpression itemAt(QStringLiteral("\\G(?:[-*+]|\\d{1,9}[.)])(?:[ \\t]|$)"));
    static const QRegularExpression autolinkAt(QStringLiteral(
        "(*NO_START_OPT)\\G<(?:[A-Za-z][A-Za-z0-9+.-]{1,31}:[^\\s<>]*|[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]+@[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?"
        "(?:\\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*)>"));
    static const QRegularExpression destinationAt(
        QStringLiteral("(*NO_START_OPT)\\G<(?:[^<>\\n\\\\]|\\\\.)*>(?=[ \\t]*(?:\\r?\\n|$)|[ \\t]*(?:\\r?\\n[ \\t]*)?[)\"'(])"));
    static const QRegularExpression headingAt(QStringLiteral("\\G#{1,6}(?=[ \\t\\r\\n]|$)"));   // (#x is text)
    struct Tag {
        qsizetype at, end;
        QString name;   // in lower case
        bool closing, selfClosed;
        int block;
    };
    // What the text is cut into by Qt's importer, which counts tags across
    // them all: paragraphs, list items, headings, quotes, a table's cells,
    // HTML blocks (each one: a <pre> to its closing tag, blank lines in it).
    // A tag is matched in its own; a comment, and its like - <?x?>,
    // <![CDATA[x]]>, <!DOCTYPE x> - is taken out (Qt draws none of them, and
    // miscounted on a tag in one); one not closed in its paragraph or HTML
    // block is text, as Qt shows it.
    int block = 0;
    qsizetype htmlEnd = 0;   // the end of the last HTML block
    qsizetype row = -1;      // the end of the table's row the walk is in, or -1
    // (The paragraph last looked at, from where to its end; for each like,
    // where none closes from, till where: a long paragraph is not looked
    // over again for each.)
    qsizetype paragraphFrom = -1, paragraphTo = -1;
    qsizetype noneFrom[4] = {-1, -1, -1, -1}, noneTill[4] = {-1, -1, -1, -1};
    QList<Tag> tags;
    QList<std::pair<qsizetype, qsizetype>> comments;   // where, how long
    QList<qsizetype> literal;   // a '<' that is text: a comment not closed, one of no tag in an HTML block
    QList<qsizetype> greater;   // a '>' that is text: of a "/>" in an HTML block, not a tag's end
    QList<qsizetype> stray;     // a '<' that is text in a paragraph (x < y, \<), not code
    const qsizetype n = md.size();
    math::outsideCode(md, false, [&](qsizetype i, bool raw) -> qsizetype {
        const QChar c = md.at(i);
        if (!raw && c == QLatin1Char('\\') && i + 1 < n && md.at(i + 1) == QLatin1Char('<')) {
            stray << i + 1;
            return -1;
        }
        if (raw && (c == QLatin1Char('|') || c == QLatin1Char('\n'))) return -1;
        if (c == QLatin1Char('|')) {
            ++block;
            return -1;
        }
        if (c == QLatin1Char('\n')) {
            // The next line a block of its own: blank, a list's item, a
            // heading, a quote - or after a heading.
            qsizetype k = i + 1;
            while (k < n && (md.at(k) == QLatin1Char(' ') || md.at(k) == QLatin1Char('\t'))) ++k;
            qsizetype line = i == 0 ? 0 : md.lastIndexOf(QLatin1Char('\n'), i - 1) + 1;   // (from -1: from the end)
            while (line < i && (md.at(line) == QLatin1Char(' ') || md.at(line) == QLatin1Char('\t'))) ++line;
            if (k >= n || md.at(k) == QLatin1Char('\n') || md.at(k) == QLatin1Char('\r') || math::matchAt(headingAt, md, k).hasMatch()
                || md.at(k) == QLatin1Char('>') || math::matchAt(itemAt, md, k).hasMatch() || math::matchAt(headingAt, md, line).hasMatch())
                ++block;
            return -1;
        }
        if (raw && c == QLatin1Char('/') && i + 1 < n && md.at(i + 1) == QLatin1Char('>')) {
            greater << i + 1;   // (Qt counts each "/>" in an HTML block as a tag closed: <hr/>x/>)
            return i + 2;
        }
        if (c != QLatin1Char('<')) return -1;
        // (A link's <destination> - [l](<a b>), [l]: <a b> - is none of
        // them: md4c reads the link first.)
        if (!raw) {
            qsizetype b = i;
            while (b > 0 && (md.at(b - 1) == QLatin1Char(' ') || md.at(b - 1) == QLatin1Char('\t'))) --b;
            const bool link = b >= 2 && md.at(b - 1) == QLatin1Char('(') && md.at(b - 2) == QLatin1Char(']');
            bool definition = b >= 2 && md.at(b - 1) == QLatin1Char(':') && md.at(b - 2) == QLatin1Char(']');
            if (definition) {
                const qsizetype content = math::contentAt(md, b == 0 ? 0 : md.lastIndexOf(QLatin1Char('\n'), b - 1) + 1);
                definition = content < b && md.at(content) == QLatin1Char('[');
            }
            if (link || definition)
                if (const QRegularExpressionMatch d = math::matchAt(destinationAt, md, i); d.hasMatch()) return d.capturedEnd();
        }
        if (const auto [opener, closer, kind] = math::likeAt(md, i); opener > 0) {
            if (!raw && (i < paragraphFrom || i >= paragraphTo)) {
                paragraphFrom = i;
                paragraphTo = math::paragraphEnd(md, i);
            }
            const qsizetype from = i + opener, limit = raw ? htmlEnd : row >= 0 ? std::min(row, paragraphTo) : paragraphTo;
            qsizetype close = -1;
            if (from < noneFrom[kind] || limit != noneTill[kind]) {
                close = QStringView(md).mid(from, std::max<qsizetype>(0, limit - from)).indexOf(closer);
                if (close < 0) {
                    noneFrom[kind] = from;
                    noneTill[kind] = limit;
                }
            }
            if (close < 0) {
                literal << i;   // (at a line's start it took the rest for a comment)
                return i + opener;
            }
            const qsizetype end = from + close + closer.size();
            comments << std::pair{i, end - i};
            return end;
        }
        // (A tag over lines: within its HTML block, paragraph or table's
        // row - not to a '>' that marks the next line's quote.)
        if (!raw && (i < paragraphFrom || i >= paragraphTo)) {
            paragraphFrom = i;
            paragraphTo = math::paragraphEnd(md, i);
        }
        const qsizetype limit = raw ? htmlEnd : row >= 0 ? std::min(row, paragraphTo) : paragraphTo;
        QRegularExpressionMatch m = math::matchAt(tagAt, md, i, limit);
        if (!m.hasMatch()) {
            // (In an HTML block Qt counts each '<' as a tag's: x < y, <x. Not
            // an autolink's.)
            if (raw) literal << i;
            else if (!math::matchAt(autolinkAt, md, i).hasMatch()) stray << i;
            return -1;
        }
        const bool closing = !m.captured(1).isEmpty();
        tags << Tag{i, m.capturedEnd(), m.captured(closing ? 2 : 3).toLower(), closing, md.at(m.capturedEnd() - 2) == QLatin1Char('/'), block};
        return m.capturedEnd();
    }, [&](qsizetype, qsizetype end) {
        ++block;
        htmlEnd = end;
    }, &row);
    // Qt's importer counts open tags and drops the text after one never
    // closed (or a closing one with nothing open): such a tag is text (its
    // '<' written &lt;, which an HTML block - a line that starts with a tag
    // such as <hr> or <details> - reads as text too, where a backslash is
    // one) - a tag of no element, an element left open, a closing
    // tag that closes nothing. An element closed in another block of the
    // text (a list's next item, a cell beside it, <details> over blank
    // lines) is taken out, with its closing tag: Qt moved text between the
    // blocks. A void element written open is closed.
    QList<qsizetype> escapes = literal, closes;   // a '<' written &lt;; a '/' before the '>'
    QList<qsizetype> open, pending;                    // elements open in the block; left open in one before (indices)
    QList<qsizetype> dropped;                          // tags taken out (indices)
    QList<std::pair<qsizetype, qsizetype>> within;     // from an element's opening tag's end to its closing tag
    int current = -1;
    const auto blockEnds = [&] {
        pending << open;
        open.clear();
    };
    for (qsizetype k = 0; k < tags.size(); ++k) {
        const Tag& t = tags.at(k);
        if (t.block != current) {
            blockEnds();
            current = t.block;
        }
        if (!isElement(t.name)) {
            escapes << t.at;   // <name>, <T>, QList<Span>'s <Span> is one
            continue;
        }
        if (t.selfClosed) {
            if (t.closing) escapes << t.at;   // (</br/>: Qt counts two tags closed)
            continue;
        }
        if (isVoid(t.name)) {
            if (t.closing) escapes << t.at;   // (</br>: a closing tag with nothing open)
            else closes << t.end - 1;
            continue;
        }
        if (!t.closing) {
            open << k;
            continue;
        }
        qsizetype found = -1;
        for (qsizetype j = open.size(); j-- > 0 && found < 0;)
            if (tags.at(open.at(j)).name == t.name) found = j;
        if (found >= 0) {
            for (qsizetype j = found + 1; j < open.size(); ++j) escapes << tags.at(open.at(j)).at;   // (left open within it)
            within << std::pair{tags.at(open.at(found)).end, t.at};
            open.resize(found);
            continue;
        }
        // Opened in a block before: both taken out.
        qsizetype before = -1;
        for (qsizetype j = pending.size(); j-- > 0 && before < 0;)
            if (tags.at(pending.at(j)).name == t.name) before = j;
        if (before >= 0) {
            dropped << pending.at(before) << k;
            pending.removeAt(before);
            continue;
        }
        escapes << t.at;   // it closes nothing
    }
    blockEnds();
    for (const qsizetype k : std::as_const(pending)) escapes << tags.at(k).at;
    // Text within an element Qt reads as HTML, with the element: a '<' in it
    // is written &lt; (<b>a < b</b> showed "a"; inline code is not so read).
    std::sort(within.begin(), within.end());
    {
        qsizetype w = 0, reach = -1;   // (the elements begun before, how far they reach)
        for (const qsizetype at : std::as_const(stray)) {
            for (; w < within.size() && within.at(w).first <= at; ++w) reach = std::max(reach, within.at(w).second);
            if (at < reach) escapes << at;
        }
    }
    // A kept tag's values in quotes: a '<' or '>' in one is written &lt;,
    // &gt; (Qt counts the <x and the /> in one: <a title="x/>y">).
    {
        const QSet<qsizetype> made(escapes.cbegin(), escapes.cend());
        const QSet<qsizetype> out(dropped.cbegin(), dropped.cend());
        for (qsizetype k = 0; k < tags.size(); ++k) {
            const Tag& t = tags.at(k);
            if (made.contains(t.at) || out.contains(k)) continue;
            QChar quote;
            for (qsizetype j = t.at + 1; j < t.end - 1; ++j) {
                const QChar c = md.at(j);
                if (quote.isNull()) {
                    if (c == QLatin1Char('"') || c == QLatin1Char('\'')) quote = c;
                } else if (c == quote) {
                    quote = QChar();
                } else if (c == QLatin1Char('<')) {
                    escapes << j;
                } else if (c == QLatin1Char('>')) {
                    greater << j;
                }
            }
        }
    }
    // The edits: (where, how much is taken out, what is put in). Made in
    // one pass from the start (each replace() would move the rest).
    struct Edit {
        qsizetype at, length;
        QString put;
    };
    QList<Edit> edits;
    for (const qsizetype at : std::as_const(escapes)) edits << Edit{at, 1, QStringLiteral("&lt;")};
    for (const qsizetype at : std::as_const(greater)) edits << Edit{at, 1, QStringLiteral("&gt;")};
    for (const qsizetype at : std::as_const(closes)) edits << Edit{at, 0, QStringLiteral("/")};
    for (const qsizetype k : std::as_const(dropped)) edits << Edit{tags.at(k).at, tags.at(k).end - tags.at(k).at, QString()};
    for (const auto& [at, length] : std::as_const(comments)) edits << Edit{at, length, QString()};
    if (edits.isEmpty()) return md;
    std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.at < b.at; });
    QString out;
    out.reserve(md.size() + 4 * edits.size());
    qsizetype from = 0;
    for (const Edit& e : std::as_const(edits)) {
        if (e.at < from) continue;   // (none overlap: a tag's edits are one)
        out += QStringView(md).mid(from, e.at - from);
        out += e.put;
        from = e.at + e.length;
    }
    out += QStringView(md).mid(from);
    return out;
}

// Qt's importer puts an HTML block into the block before it - a
// paragraph, a heading, a list's item: "para" then <div>x</div> read
// "parax", and the <summary> of a <details> taken out joined the paragraph
// before. Each HTML block starts with a paragraph of a mark, which takes
// its place there (a <pre>'s text, with it); blocksApart() takes them out.
// One in a quote, or indented in a list's item, has marks of its own, at its
// start and its end: its blocks are set in as the quote's or the item's
// paragraphs are. (Noncharacters: U+FDD0 and U+FDD1 are Qt's frames'.)
constexpr char16_t kBlockMark = 0xFDD2, kInnerBlockMark = 0xFDD3, kInnerBlockEnd = 0xFDD4;

QString htmlBlocksMarked(const QString& md)
{
    static const QRegularExpression raw(QStringLiteral("\\G<(?:pre|script|style|textarea)(?=[\\s>]|$)"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression item(QStringLiteral("(?:[-*+]|\\d{1,9}[.)])(?:[ \\t]|$)"));
    // A line that ends with a block's closing tag: Qt puts the line after it
    // into that block ("x" then "next line" read "xnext line", a quote's or
    // a list's line drawn outside them); a browser draws it below.
    static const QRegularExpression closed(
        QStringLiteral("(?:</(?:address|article|aside|blockquote|center|details|dialog|div|dl|fieldset|figcaption|figure|footer|form|h[1-6]|"
                       "header|main|menu|nav|ol|p|pre|section|summary|table|ul)\\s*>|<hr(?:\\s[^<>]*)?/?>)[ \\t]*\\r?$"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tagAt(QStringLiteral("\\G<(/?)([A-Za-z][A-Za-z0-9-]*)(?:\\s[^<>]*?)?(/?)>"));
    QList<std::pair<qsizetype, QString>> marks;   // where, what
    math::outsideCode(
        md, false, [](qsizetype, bool) -> qsizetype { return -1; },
        [&](qsizetype k, qsizetype end) {
            // (Not a list item's first: Qt keeps it in the item, the mark's
            // paragraph would not. Where md4c reads code the walk did not,
            // the mark shows as text: markOut() takes it out.)
            const qsizetype line = k == 0 ? 0 : md.lastIndexOf(QLatin1Char('\n'), k - 1) + 1;
            const QStringView before = QStringView(md).mid(line, k - line);
            if (math::matchIn(item, before).hasMatch()) return;
            // (In a quote; or indented past its quote's marks and their
            // space: in a list's item, if there is one. Its end marked at its
            // last line's end - not where that is in a tag, <div with no '>'.)
            qsizetype spaces = 0;
            while (spaces < before.size() && before.at(before.size() - 1 - spaces) == QLatin1Char(' ')) ++spaces;
            qsizetype last = end;
            while (last > k && md.at(last - 1).isSpace()) --last;
            const qsizetype lastLine = std::max(k, md.lastIndexOf(QLatin1Char('\n'), last - 1) + 1);
            qsizetype open = -1, shut = -1;
            for (qsizetype p = lastLine; p < last; ++p) {
                if (md.at(p) == QLatin1Char('>')) shut = p;
                else if (md.at(p) == QLatin1Char('<') && p + 1 < last && (md.at(p + 1).isLetter() || md.at(p + 1) == QLatin1Char('/'))) open = p;
            }
            // (Nor where an element of it is left open: Qt reads on to its
            // closing tag - a mark in a <script> hid the text after it.)
            int depth = 0;
            for (qsizetype p = md.indexOf(QLatin1Char('<'), k); p >= 0 && p < last; p = md.indexOf(QLatin1Char('<'), p + 1)) {
                const QRegularExpressionMatch t = math::matchAt(tagAt, md, p, last);
                if (!t.hasMatch()) continue;
                if (!t.captured(1).isEmpty()) --depth;
                else if (t.captured(3).isEmpty() && !isVoid(t.captured(2).toLower())) ++depth;
            }
            const bool inner = (before.contains(QLatin1Char('>')) || spaces > (spaces < before.size() ? 1 : 0)) && last > k && open <= shut && depth <= 0;
            const QChar mark(inner ? kInnerBlockMark : kBlockMark);
            // (A <pre> is to its closing tag, blank lines in it: a <p> before
            // would make it a block to a blank line. Its text starts with the
            // mark.)
            if (!math::matchAt(raw, md, k).hasMatch()) {
                marks << std::pair{k, QStringLiteral("<p>%1</p>").arg(mark)};
                // (Each line after one closed so: a mark before it, past the
                // block's quote marks and spaces - a list's mark is its text.)
                const qsizetype depth = before.count(QLatin1Char('>'));
                for (qsizetype from = k, eol = md.indexOf(QLatin1Char('\n'), from); eol >= 0 && eol + 1 < end;
                     from = eol + 1, eol = md.indexOf(QLatin1Char('\n'), from)) {
                    if (!math::matchIn(closed, QStringView(md).mid(from, eol - from)).hasMatch()) continue;
                    qsizetype c = eol + 1;
                    for (qsizetype q = 0; c < end;) {
                        if (md.at(c) == QLatin1Char('>') && q < depth) ++q;
                        else if (md.at(c) != QLatin1Char(' ') && md.at(c) != QLatin1Char('\t')) break;
                        ++c;
                    }
                    if (c < end && md.at(c) != QLatin1Char('\n') && md.at(c) != QLatin1Char('\r'))
                        marks << std::pair{c, QStringLiteral("<p>%1</p>").arg(QChar(kBlockMark))};
                }
            } else if (const qsizetype close = md.indexOf(QLatin1Char('>'), k); close >= 0) {
                marks << std::pair{close + 1, QString(mark)};
            }
            if (inner) marks << std::pair{last, QStringLiteral("<p>%1</p>").arg(QChar(kInnerBlockEnd))};
        });
    if (marks.isEmpty()) return md;
    QString out;
    out.reserve(md.size() + 8 * marks.size());
    qsizetype from = 0;
    for (const auto& [at, put] : std::as_const(marks)) {
        out += QStringView(md).mid(from, at - from);
        out += put;
        from = at;
    }
    out += QStringView(md).mid(from);
    return out;
}

// Takes the empty block \a b out, with the break after it - the next block
// keeps its format -, or at the end with the one before it. (One before a
// table, in another frame, is kept, as Qt keeps one; one last in a table's
// cell too.)
void takeOut(const QTextBlock& b)
{
    QTextCursor c(b);
    // (In one frame - in a table, one cell: a selection over two takes the
    // text of each cell in it out.)
    const auto together = [&c](const QTextBlock& other) {
        const QTextCursor o(other);
        if (o.currentFrame() != c.currentFrame()) return false;
        const QTextTable* table = c.currentTable();
        return table == nullptr || table->cellAt(c) == table->cellAt(o);
    };
    if (const QTextBlock next = b.next(); next.isValid() && together(next)) {
        c.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor);
        c.removeSelectedText();
    } else if (const QTextBlock previous = b.previous(); !next.isValid() && previous.isValid() && together(previous)) {
        c.setPosition(previous.position() + previous.length() - 1);
        c.setPosition(b.position(), QTextCursor::KeepAnchor);
        c.removeSelectedText();
    }
}

// The mark \a hit selects, taken out - with the "<p>" and "</p>" around it
// when md4c read them as text, not HTML (code the walk took for none).
void markOut(QTextCursor& hit)
{
    const QTextBlock b = hit.block();
    const QString text = b.text();
    const qsizetype at = hit.selectionStart() - b.position();
    if (at >= 3 && QStringView(text).mid(at - 3, 3) == QLatin1String("<p>") && QStringView(text).mid(at + 1, 4) == QLatin1String("</p>")) {
        hit.setPosition(b.position() + int(at) - 3);
        hit.setPosition(b.position() + int(at) + 5, QTextCursor::KeepAnchor);
    }
    hit.removeSelectedText();
}

// What Qt's importer put into one block, apart: the text after an <hr>
// inside a paragraph, which it draws above the rule (moved to a block of
// its own after the rule; in a table's cell, a line more in it); and an
// HTML block, at htmlBlocksMarked()'s marks - each taken out, what follows
// it in its block moved to a block of its own, a block it alone was taken
// out; one in a quote or a list's item set in as their paragraphs are.
void blocksApart(QTextDocument* document)
{
    for (QTextBlock b = document->begin(); b.isValid(); b = b.next()) {
        QTextBlockFormat f = b.blockFormat();
        if (!f.hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth) || b.text().isEmpty()) continue;
        f.clearProperty(QTextFormat::BlockTrailingHorizontalRulerWidth);
        QTextCursor c(b);
        c.insertBlock(f, b.charFormat());
        b = b.next();   // (the text, after the rule's block, now empty)
    }
    static const QRegularExpression marks(QStringLiteral("[\\x{FDD2}\\x{FDD3}]"));
    const QString innerEnd{QChar(kInnerBlockEnd)};
    for (QTextCursor hit = document->find(marks); !hit.isNull(); hit = document->find(marks, hit.position())) {
        const bool inner = hit.selectedText() == QString(QChar(kInnerBlockMark));
        const QTextBlock b = hit.block();
        // How the quote's or the item's paragraphs are set in: as the block
        // the HTML follows, its list's indent if it is an item.
        const QTextBlockFormat container = b.blockFormat();
        const int indent = b.textList() ? b.textList()->format().indent() : container.indent();
        markOut(hit);
        const qsizetype at = hit.position() - b.position();
        const QString text = b.text();
        QTextBlock first = b.next();   // the HTML's first block
        if (at > 0 && at < text.size() && !QStringView(text).mid(at).trimmed().isEmpty()) {
            // The HTML's text after the block's: a block of its own (not a
            // heading's, nor in a list).
            QTextBlockFormat f = b.blockFormat();
            f.clearProperty(QTextFormat::HeadingLevel);
            f.clearProperty(QTextFormat::ObjectIndex);
            hit.insertBlock(f);
            first = hit.block();
        } else if (at == 0) {
            first = b;
            if (text.trimmed().isEmpty()) {   // (a line's end in the HTML, a space to Qt: none)
                const int position = b.position();
                takeOut(b);
                first = document->findBlock(position);
            }
        }
        if (!inner) continue;
        // (To its end: after the '>' its last line ends with, in no element.)
        const QTextCursor end = document->find(innerEnd, hit.position());
        for (QTextBlock h = first; h.isValid() && (end.isNull() ? h == first : h.position() < end.block().position()); h = h.next()) {
            if (QTextCursor(h).currentTable() != nullptr || h.textList() != nullptr) continue;
            QTextBlockFormat f = h.blockFormat();
            f.setIndent(indent);
            f.setLeftMargin(container.leftMargin());
            f.setRightMargin(container.rightMargin());
            f.setProperty(QTextFormat::BlockQuoteLevel, container.intProperty(QTextFormat::BlockQuoteLevel));
            QTextCursor(h).setBlockFormat(f);
        }
    }
    // The ends: taken out, a block alone too.
    for (QTextCursor hit = document->find(innerEnd); !hit.isNull(); hit = document->find(innerEnd, hit.position())) {
        const QTextBlock b = hit.block();
        markOut(hit);
        if (b.text().isEmpty()) takeOut(b);
    }
}

} // namespace

void setMarkdown(QTextDocument* document, const QString& markdown)
{
    // (The marks are its own: noncharacters in the text are taken out.)
    QString md = markdown;
    for (const char16_t mark : {kBlockMark, kInnerBlockMark, kInnerBlockEnd}) md.remove(QChar(mark));
    document->setMarkdown(htmlBlocksMarked(htmlBalanced(md)), QTextDocument::MarkdownDialectGitHub);
    blocksApart(document);
}

} // namespace qucs_s::markdown
