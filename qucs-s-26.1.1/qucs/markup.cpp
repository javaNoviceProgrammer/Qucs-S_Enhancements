/*
 * markup.cpp - pens to draw with on what a viewer shows
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "markup.h"

#include "settings.h"
#include "strokeshape.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QColorDialog>
#include <QIconEngine>
#include <QJsonObject>
#include <QMenu>
#include <QPainter>
#include <QPalette>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <tuple>

namespace qucs_s::markup {

namespace {

/// Every drawing's revisions, from one count: none is had twice.
quint64 nextRevision()
{
    static std::atomic<quint64> last{0};
    return ++last;
}

const char* const kToolKey = "Viewers/penTool";
const char* const kColorKey = "Viewers/penColor";
const char* const kWidthKey = "Viewers/penWidth";
const char* const kHighlighterKey = "Viewers/highlighterWidth";

/// How much of a highlighter's colour covers what is under it.
constexpr double HighlighterCover = 0.4;
/// How much of its colour a stroke marked addressed keeps.
constexpr double AddressedCover = 0.3;
/// The broadest a stroke is drawn, pixels: zoomed far in, one covers the
/// view at that already, and the stroker is spared lines of a million.
constexpr double BroadestPixels = 1e4;

/// The widths the menu offers, pixels: a pen's, then a highlighter's.
constexpr double kPenWidths[3] = {2, 4, 8};
constexpr double kHighlighterWidths[3] = {10, 16, 24};

QString nameOf(Tool tool)
{
    switch (tool) {
    case Tool::Highlighter: return QStringLiteral("highlighter");
    case Tool::Eraser: return QStringLiteral("eraser");
    case Tool::Pen: break;
    }
    return QStringLiteral("pen");
}

Tool toolNamed(const QString& name)
{
    if (name == QLatin1String("highlighter")) return Tool::Highlighter;
    if (name == QLatin1String("eraser")) return Tool::Eraser;
    return Tool::Pen;
}

/// The distance from \a p to the segment from \a a to \a b.
double distance(QPointF p, QPointF a, QPointF b)
{
    const QPointF d = b - a;
    const double length2 = d.x() * d.x() + d.y() * d.y();
    const double t = length2 > 0 ? std::clamp(((p - a).x() * d.x() + (p - a).y() * d.y()) / length2, 0.0, 1.0) : 0.0;
    const QPointF off = a + t * d - p;
    return std::hypot(off.x(), off.y());
}

// The pen's icon: its body in the text's colour of the moment, its tip and
// the line under it in its own.
class PenGlyph : public QIconEngine
{
public:
    explicit PenGlyph(const Pen& pen) : a_pen(pen) {}
    QIconEngine* clone() const override { return new PenGlyph(a_pen); }
    void paint(QPainter* p, const QRect& rect, QIcon::Mode mode, QIcon::State) override
    {
        const QColor ink =
            QApplication::palette().color(mode == QIcon::Disabled ? QPalette::Disabled : QPalette::Active, QPalette::WindowText);
        QColor tip = a_pen.color;
        if (mode == QIcon::Disabled) tip.setAlphaF(0.4);
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const qreal s = std::min(rect.width(), rect.height());
        p->translate(rect.center().x() - s / 2 + 0.5, rect.center().y() - s / 2 + 0.5);
        p->scale(s / 16.0, s / 16.0);
        p->setPen(QPen(ink, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p->setBrush(Qt::NoBrush);
        switch (a_pen.tool) {
        case Tool::Pen:
            // A pen leaning right, its tip at the lower left.
            p->drawPolygon(QPolygonF{QPointF(5.5, 10.5), QPointF(11.5, 4.5), QPointF(13.5, 6.5), QPointF(7.5, 12.5)});
            p->setBrush(tip);
            p->drawPolygon(QPolygonF{QPointF(5.5, 10.5), QPointF(7.5, 12.5), QPointF(3.5, 13.5)});
            p->setPen(QPen(tip, 2, Qt::SolidLine, Qt::RoundCap));
            p->drawLine(QPointF(2.5, 15), QPointF(13.5, 15));
            break;
        case Tool::Highlighter: {
            // A broad marker, a chisel tip, a band under it that shows
            // through.
            p->drawPolygon(QPolygonF{QPointF(6, 8.5), QPointF(10.5, 4), QPointF(14, 7.5), QPointF(9.5, 12)});
            p->setBrush(tip);
            p->drawPolygon(QPolygonF{QPointF(6, 8.5), QPointF(9.5, 12), QPointF(5.5, 12.5), QPointF(4, 11)});
            QColor band = tip;
            band.setAlphaF(band.alphaF() * 0.55);
            p->setPen(Qt::NoPen);
            p->setBrush(band);
            p->drawRect(QRectF(1.5, 13.5, 13, 2.5));
            break;
        }
        case Tool::Eraser:
            // A block leaning right, its foot the darker half.
            p->drawPolygon(QPolygonF{QPointF(2.5, 10.5), QPointF(8.5, 4.5), QPointF(13.5, 9.5), QPointF(7.5, 15.5)});
            p->drawLine(QPointF(5, 8), QPointF(10, 13));
            p->setBrush(ink);
            p->drawPolygon(QPolygonF{QPointF(2.5, 10.5), QPointF(5, 8), QPointF(10, 13), QPointF(7.5, 15.5)});
            break;
        }
        p->restore();
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        QPixmap pm(size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        paint(&p, QRect(QPoint(0, 0), size), mode, state);
        return pm;
    }

private:
    Pen a_pen;
};

/// A colour's swatch for the menu.
QIcon swatch(const QColor& color)
{
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QApplication::palette().color(QPalette::WindowText), 1));
    p.setBrush(color);
    p.drawEllipse(QRectF(2.5, 2.5, 11, 11));
    return QIcon(pm);
}

} // namespace

// ----------------------------------------------------------------------
// Strokes

QRectF Stroke::bounds() const
{
    if (points.isEmpty()) return QRectF();
    const double half = width / 2;
    return points.boundingRect().adjusted(-half, -half, half, half);
}

QList<Stroke> Drawing::on(const QString& part) const
{
    QList<Stroke> out;
    for (const Stroke& s : a_strokes)
        if (s.on == part) out << s;
    return out;
}

int Drawing::count(const QString& part) const
{
    return int(std::count_if(a_strokes.begin(), a_strokes.end(), [&part](const Stroke& s) { return s.on == part; }));
}

QStringList Drawing::parts() const
{
    QStringList out;
    for (const Stroke& s : a_strokes)
        if (!out.contains(s.on)) out << s.on;
    return out;
}

void Drawing::remember()
{
    a_undo << a_strokes;
    if (a_undo.size() > MostUndone) a_undo.removeFirst();
    a_revision = nextRevision();
}

void Drawing::add(const Stroke& stroke)
{
    remember();
    a_strokes << stroke;
    a_strokes.last().id = ++a_lastId[stroke.on];
}

int Drawing::setAddressed(const QString& part, const QList<int>& ids, bool addressed)
{
    QList<int> which;
    for (int i = 0; i < a_strokes.size(); ++i)
        if (a_strokes.at(i).on == part && ids.contains(a_strokes.at(i).id) && a_strokes.at(i).addressed != addressed) which << i;
    if (which.isEmpty()) return 0;
    remember();
    for (const int i : which) a_strokes[i].addressed = addressed;
    return int(which.size());
}

const Stroke* Drawing::find(const QString& part, int id) const
{
    for (const Stroke& s : a_strokes)
        if (s.on == part && s.id == id) return &s;
    return nullptr;
}

bool Drawing::eraseAt(const QString& part, QPointF point, double reach)
{
    for (int i = int(a_strokes.size()) - 1; i >= 0; --i) {
        const Stroke& s = a_strokes.at(i);
        if (s.on != part || s.points.isEmpty()) continue;
        const double near = reach + s.width / 2;
        bool hit = s.points.size() == 1 && distance(point, s.points.first(), s.points.first()) <= near;
        for (int k = 1; k < s.points.size() && !hit; ++k) hit = distance(point, s.points.at(k - 1), s.points.at(k)) <= near;
        if (hit) {
            a_strokes.removeAt(i);
            a_revision = nextRevision();
            return true;
        }
    }
    return false;
}

void Drawing::changed(const QList<Stroke>& before)
{
    a_undo << before;
    if (a_undo.size() > MostUndone) a_undo.removeFirst();
}

bool Drawing::clear(const QString& part)
{
    QList<Stroke> kept;
    for (const Stroke& s : a_strokes)
        if (!part.isEmpty() && s.on != part) kept << s;
    if (kept.size() == a_strokes.size()) return false;
    remember();
    a_strokes = kept;
    return true;
}

bool Drawing::undo()
{
    if (a_undo.isEmpty()) return false;
    a_strokes = a_undo.takeLast();
    a_revision = nextRevision();
    return true;
}

void paint(QPainter& p, const QList<Stroke>& strokes, const std::function<QPointF(QPointF)>& toPixel, double pixelsPerUnit)
{
    if (strokes.isEmpty()) return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(Qt::NoBrush);
    QPolygonF shown;
    for (const Stroke& s : strokes) {
        if (s.points.isEmpty()) continue;
        // (One marked addressed, faded: what was done is still seen.)
        QColor color = s.color;
        if (s.addressed) color.setAlphaF(color.alphaF() * AddressedCover);
        p.setPen(QPen(color, std::clamp(s.width * pixelsPerUnit, 1.0, BroadestPixels), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        // (A click is a dot, as broad as the pen.)
        if (s.points.size() == 1) {
            p.drawPoint(toPixel(s.points.first()));
            continue;
        }
        shown.clear();
        shown.reserve(s.points.size());
        for (const QPointF& pt : s.points) shown << toPixel(pt);
        // One polyline, so that a highlighter's stroke over itself lets as
        // much through as anywhere else along it.
        p.drawPolyline(shown);
    }
    p.restore();
}

Stroke begin(const Pen& pen, const QString& part, QPointF point, double pixelsPerUnit)
{
    Stroke s;
    s.on = part;
    s.points << point;
    s.highlighter = pen.tool == Tool::Highlighter;
    s.color = pen.color;
    if (s.highlighter) s.color.setAlphaF(s.color.alphaF() * HighlighterCover);
    s.width = pen.drawingWidth() / std::max(pixelsPerUnit, 1e-300);
    return s;
}

// ----------------------------------------------------------------------
// Reading what was drawn

namespace {

double lengthOf(const QPolygonF& p, int from = 0, int to = -1)
{
    if (to < 0) to = int(p.size()) - 1;
    double l = 0;
    for (int i = from + 1; i <= to; ++i) l += QLineF(p.at(i - 1), p.at(i)).length();
    return l;
}

/// The furthest any point of \a p between \a from and \a to lies from the
/// segment between those two.
double bulge(const QPolygonF& p, int from, int to)
{
    double most = 0;
    for (int i = from + 1; i < to; ++i) most = std::max(most, distance(p.at(i), p.at(from), p.at(to)));
    return most;
}

/// The area the closed outline \a p holds.
double areaOf(const QPolygonF& p)
{
    double a = 0;
    for (int i = 0; i < p.size(); ++i) {
        const QPointF& u = p.at(i);
        const QPointF& v = p.at((i + 1) % p.size());
        a += u.x() * v.y() - v.x() * u.y();
    }
    return std::abs(a) / 2;
}

double sizeOf(const QPolygonF& p)
{
    const QRectF b = p.boundingRect();
    return std::hypot(b.width(), b.height());
}

/// The points along \a p every \a step, its own among them.
QPolygonF sampledOf(const QPolygonF& p, double step)
{
    if (p.size() < 2 || !(step > 0)) return p;
    QPolygonF out{p.first()};
    for (int i = 1; i < p.size(); ++i) {
        const QLineF seg(p.at(i - 1), p.at(i));
        const int n = int(std::ceil(seg.length() / step));
        for (int k = 1; k <= n; ++k) out << seg.pointAt(double(k) / n);
    }
    return out;
}

QString shortened(const QString& text, int most = 160)
{
    const QString t = text.simplified();
    return t.size() <= most ? t : t.left(most - 1) + QChar(0x2026);
}

double rounded(double v)
{
    return std::round(v * 1000) / 1000;
}

QJsonArray boxJson(const QRectF& b)
{
    return {rounded(b.left()), rounded(b.top()), rounded(b.right()), rounded(b.bottom())};
}

/// A target, or some of its words (in order), as a reading names it.
QJsonObject refOf(const Target& t, const QList<int>& words = {})
{
    if (words.isEmpty() || words.size() == t.words.size()) return QJsonObject{{t.kind, shortened(t.text)}};
    QStringList said;
    for (const int w : words) said << t.words.at(w).first;
    return QJsonObject{{QStringLiteral("words"), said.join(QLatin1Char(' '))}, {QStringLiteral("in ") + t.kind, shortened(t.text)}};
}

/// What is at \a point: the word under it and its target, else the target
/// under it - the uppermost - else nothing (-1).
std::pair<int, int> under(const QList<Target>& targets, QPointF point)
{
    for (int t = int(targets.size()) - 1; t >= 0; --t)
        for (int w = 0; w < targets.at(t).words.size(); ++w)
            if (targets.at(t).words.at(w).second.containsPoint(point, Qt::OddEvenFill)) return {t, w};
    for (int t = int(targets.size()) - 1; t >= 0; --t)
        if (targets.at(t).outline.containsPoint(point, Qt::OddEvenFill)) return {t, -1};
    return {-1, -1};
}

/// What a stroke reaching \a end points at: what is there, else the first
/// thing on ahead of it - \a along, within \a ahead - else the nearest
/// within \a reach of its end.
QJsonObject pointedAt(const QList<Target>& targets, QPointF end, QPointF along, double ahead, double reach)
{
    auto [t, w] = under(targets, end);
    for (int k = 1; t < 0 && ahead > 0 && k <= 16; ++k) std::tie(t, w) = under(targets, end + along * (ahead * k / 16));
    if (t < 0) {
        double best = reach;
        for (int i = 0; i < targets.size(); ++i) {
            const QPolygonF& o = targets.at(i).outline;
            for (int k = 1; k < o.size(); ++k)
                if (const double d = distance(end, o.at(k - 1), o.at(k)); d < best) {
                    best = d;
                    t = i;
                }
        }
    }
    if (t < 0) return {};
    return refOf(targets.at(t), w >= 0 ? QList<int>{w} : QList<int>{});
}

/// What \a s lies on: the words its middle line passes through, and the
/// other targets it crosses.
QJsonArray coveredBy(const QList<Target>& targets, const Stroke& s)
{
    const QPolygonF along = alongOf(s);
    QJsonArray out;
    for (const Target& t : targets) {
        if (!t.words.isEmpty()) {
            QList<int> on;
            for (int w = 0; w < t.words.size(); ++w)
                for (const QPointF& pt : along)
                    if (t.words.at(w).second.containsPoint(pt, Qt::OddEvenFill)) {
                        on << w;
                        break;
                    }
            if (!on.isEmpty()) out.append(refOf(t, on));
            continue;
        }
        for (const QPointF& pt : along)
            if (t.outline.containsPoint(pt, Qt::OddEvenFill)) {
                out.append(refOf(t));
                break;
            }
    }
    return out;
}

/// The words a line along a line of text underlines (\a below) or strikes
/// through: as long as half a word over it, just under its foot or across
/// its middle.
QJsonArray alongText(const QList<Target>& targets, const Stroke& s, bool below)
{
    const QPointF a = s.points.first(), b = s.points.last();
    const double dx = b.x() - a.x(), dy = b.y() - a.y();
    QJsonArray out;
    if (dx == 0 || std::abs(dy) > 0.3 * std::abs(dx)) return out;
    const double left = std::min(a.x(), b.x()), right = std::max(a.x(), b.x());
    for (const Target& t : targets) {
        QList<int> picked;
        for (int w = 0; w < t.words.size(); ++w) {
            const QRectF r = t.words.at(w).second.boundingRect();
            const double over = std::min(right, r.right()) - std::max(left, r.left());
            if (!(r.width() > 0) || over < 0.5 * r.width()) continue;
            const double y = a.y() + dy * ((r.center().x() - a.x()) / dx), h = r.height();
            const bool foot = y >= r.bottom() - 0.2 * h && y <= r.bottom() + 0.6 * h;
            const bool middle = y >= r.top() + 0.3 * h && y < r.bottom() - 0.2 * h;
            if (below ? foot : middle) picked << w;
        }
        if (!picked.isEmpty()) out.append(refOf(t, picked));
    }
    return out;
}

/// A colour as a reading says it: its name among the pen's, else its code.
QString colourNamed(const QString& code)
{
    for (const auto& [color, name] : colors())
        if (color.name(QColor::HexRgb) == code) return name.toLower();
    return code;
}

/// What a reading's field names, in words.
QString said(const QJsonValue& v)
{
    if (v.isArray()) {
        QStringList parts;
        for (const QJsonValue& one : v.toArray()) parts << said(one);
        return parts.join(QStringLiteral("; "));
    }
    const QJsonObject o = v.toObject();
    QStringList parts;
    if (o.contains(QStringLiteral("words"))) {
        for (auto it = o.begin(); it != o.end(); ++it)
            if (it.key().startsWith(QLatin1String("in ")))
                parts << QStringLiteral("\"%1\" in the %2 \"%3\"").arg(o.value(QStringLiteral("words")).toString(), it.key().mid(3), it.value().toString());
        return parts.join(QStringLiteral(", "));
    }
    // A layout's: the shapes by layer, the texts, the cells placed.
    if (o.contains(QStringLiteral("shapes")) || o.contains(QStringLiteral("texts")) || o.contains(QStringLiteral("cells"))) {
        const QJsonObject shapes = o.value(QStringLiteral("shapes")).toObject();
        for (auto it = shapes.begin(); it != shapes.end(); ++it)
            parts << QStringLiteral("%1 shape(s) on %2").arg(it.value().toInt()).arg(it.key());
        for (const QJsonValue& t : o.value(QStringLiteral("texts")).toArray()) parts << QStringLiteral("the text %1").arg(t.toString());
        const QJsonObject cells = o.value(QStringLiteral("cells")).toObject();
        for (auto it = cells.begin(); it != cells.end(); ++it)
            parts << QStringLiteral("%1 placement(s) of %2").arg(it.value().toInt()).arg(it.key());
        return parts.isEmpty() ? QStringLiteral("nothing") : parts.join(QStringLiteral(", "));
    }
    for (auto it = o.begin(); it != o.end(); ++it)
        parts << (it.value().toString().isEmpty() ? QStringLiteral("a %1").arg(it.key())
                                                  : QStringLiteral("the %1 \"%2\"").arg(it.key(), it.value().toString()));
    return parts.join(QStringLiteral(", "));
}

/// A feature of a layout's shape, or a text, as a reading has it (a
/// vertex, an end, a corner, a segment or an edge of a shape on a layer),
/// in words: "the vertex at (0, 40) of the 2/0 path in TRACE, a 53° corner".
QString featureSaid(const QJsonObject& o)
{
    const auto at = [](const QJsonValue& v) { return pointSaid(QPointF(v.toArray().at(0).toDouble(), v.toArray().at(1).toDouble())); };
    if (o.contains(QStringLiteral("text"))) return QStringLiteral("the text %1 at %2").arg(o.value(QStringLiteral("text")).toString(), at(o.value(QStringLiteral("at"))));
    QString of = QStringLiteral("the %1 %2").arg(o.value(QStringLiteral("layer")).toString(), o.value(QStringLiteral("kind")).toString());
    if (o.contains(QStringLiteral("in"))) of += QStringLiteral(" in ") + o.value(QStringLiteral("in")).toString();
    const QString angle = o.contains(QStringLiteral("angle")) ? QStringLiteral(", a %1° corner").arg(numberSaid(o.value(QStringLiteral("angle")).toDouble())) : QString();
    QString side = o.value(QStringLiteral("side")).toString();
    if (!side.isEmpty()) side = QStringLiteral(", from %1").arg(side == QLatin1String("left") || side == QLatin1String("right") ? QStringLiteral("the ") + side : side);
    for (const char* key : {"vertex", "end", "corner"})
        if (o.contains(QLatin1String(key))) return QStringLiteral("the %1 at %2 of %3%4").arg(QLatin1String(key), at(o.value(QLatin1String(key))), of, angle);
    for (const char* key : {"segment", "edge"})
        if (o.contains(QLatin1String(key))) {
            const QJsonArray e = o.value(QLatin1String(key)).toArray();
            return QStringLiteral("the %1 from %2 to %3 of %4%5").arg(QLatin1String(key), at(e.at(0)), at(e.at(1)), of, side);
        }
    return of;
}

QString featuresSaid(const QJsonValue& v)
{
    QStringList parts;
    for (const QJsonValue& one : v.toArray()) parts << featureSaid(one.toObject());
    return parts.join(QStringLiteral("; "));
}

/// How a line or a mark follows a shape, in words: "following the 2/0 path
/// in TRACE from (-50, 0) to (50, 0), leaving it between (-33, 0) and
/// (36, 0) to go 40 µm above it".
QString followSaid(const QJsonObject& o)
{
    const auto at = [](const QJsonValue& v) { return pointSaid(QPointF(v.toArray().at(0).toDouble(), v.toArray().at(1).toDouble())); };
    QString s = QStringLiteral("following the %1 %2").arg(o.value(QStringLiteral("layer")).toString(), o.value(QStringLiteral("kind")).toString());
    if (o.contains(QStringLiteral("in"))) s += QStringLiteral(" in ") + o.value(QStringLiteral("in")).toString();
    s += QStringLiteral(" from %1 to %2").arg(at(o.value(QStringLiteral("from"))), at(o.value(QStringLiteral("to"))));
    QStringList leaves;
    for (const QJsonValue& v : o.value(QStringLiteral("leaves")).toArray()) {
        const QJsonObject l = v.toObject();
        const QString side = l.value(QStringLiteral("side")).toString();
        QString leave = QStringLiteral("leaving it at %1 and coming back at %2, %3 µm %4 it")
                            .arg(at(l.value(QStringLiteral("at"))), at(l.value(QStringLiteral("back"))), numberSaid(l.value(QStringLiteral("by")).toDouble()),
                                 side == QLatin1String("left") || side == QLatin1String("right") ? QStringLiteral("to the %1 of").arg(side) : side);
        QStringList turns;
        for (const QJsonValue& t : l.value(QStringLiteral("its own line turning at")).toArray()) turns << at(t);
        if (!turns.isEmpty()) leave += QStringLiteral(" (the %1 itself turning at %2)").arg(o.value(QStringLiteral("kind")).toString(), turns.join(QStringLiteral(" and ")));
        leaves << leave;
    }
    if (!leaves.isEmpty()) s += QStringLiteral(", ") + leaves.join(QStringLiteral(", then "));
    return s;
}

} // namespace

QString nameOf(Gesture gesture)
{
    switch (gesture) {
    case Gesture::Dot: return QStringLiteral("dot");
    case Gesture::Ring: return QStringLiteral("ring");
    case Gesture::Line: return QStringLiteral("line");
    case Gesture::Arrow: return QStringLiteral("arrow");
    case Gesture::Arrowhead: return QStringLiteral("arrowhead");
    case Gesture::Corner: return QStringLiteral("corner");
    case Gesture::Mark: break;
    }
    return QStringLiteral("mark");
}

Gesture gestureOf(const Stroke& stroke, QPointF* tip)
{
    const QPolygonF& p = stroke.points;
    if (tip != nullptr) *tip = p.isEmpty() ? QPointF() : p.last();
    const double size = sizeOf(p);
    if (p.size() < 2 || size <= std::max(2.5 * stroke.width, 1e-12)) return Gesture::Dot;
    const int last = int(p.size()) - 1;
    const QRectF box = p.boundingRect();
    const double chord = QLineF(p.first(), p.last()).length();
    // A ring: back near where it began, round a fair share of its box.
    if (chord <= 0.25 * size && areaOf(p) >= 0.2 * box.width() * box.height()) return Gesture::Ring;
    // An arrow drawn at one go: straight out to the point furthest from
    // where it began, its head what comes back from there - more than the
    // pen's wobble as it lifts, and close by the point: one side of a V, or
    // both drawn out and back, or a triangle - never out as far as half the
    // shaft. (Held to its length, at most three fifths of the shaft, a V of
    // both sides drawn at one go read as a mark: the check of 2 November.)
    int far = 0;
    double reach = 0;
    for (int k = 1; k <= last; ++k)
        if (const double d = QLineF(p.first(), p.at(k)).length(); d > reach) {
            reach = d;
            far = k;
        }
    const double back = lengthOf(p, far, last);
    double away = 0;   // how far the head goes from the point
    for (int k = far + 1; k <= last; ++k) away = std::max(away, QLineF(p.at(far), p.at(k)).length());
    if (far > 0 && far < last && reach >= 0.6 * size && bulge(p, 0, far) <= 0.1 * reach && back >= std::max(0.05 * reach, 2 * stroke.width)
        && away <= 0.45 * reach && back <= 2 * reach) {
        if (tip != nullptr) *tip = p.at(far);
        return Gesture::Arrow;
    }
    // A line: nowhere further from its chord than a tenth of it.
    if (chord > 0 && bulge(p, 0, last) <= 0.1 * chord) return Gesture::Line;
    // A corner: two straight arms at an angle of 45 to 135 degrees, the
    // shorter two fifths of the longer at least (a tick's is less) - an L,
    // a V, a caret.
    const QPolygonF arms = simplified(p, std::max(0.06 * size, stroke.width));
    if (arms.size() == 3) {
        const QLineF a(arms.at(1), arms.at(0)), b(arms.at(1), arms.at(2));
        const double angle = std::min(a.angleTo(b), 360 - a.angleTo(b));
        if (std::min(a.length(), b.length()) >= 0.4 * std::max(a.length(), b.length()) && angle >= 45 && angle <= 135) {
            if (tip != nullptr) *tip = arms.at(1);
            return Gesture::Corner;
        }
    }
    return Gesture::Mark;
}

QList<Gesture> gesturesOf(const QList<Stroke>& strokes, QList<QPointF>* tips, QList<int>* headOf)
{
    const int n = int(strokes.size());
    QList<Gesture> out(n);
    QList<QPointF> tip(n);
    QList<int> head(n, -1);
    for (int i = 0; i < n; ++i) out[i] = gestureOf(strokes.at(i), &tip[i]);
    // One or two short strokes just after a line, at one of its ends, are
    // its head: an arrow pointing there. (Not a dot: a pin at its end.)
    for (int i = 0; i < n; ++i) {
        if (out.at(i) != Gesture::Line) continue;
        const QPolygonF& shaft = strokes.at(i).points;
        const double length = QLineF(shaft.first(), shaft.last()).length();
        for (int j = i + 1; j < std::min(n, i + 3); ++j) {
            if (out.at(j) != Gesture::Line && out.at(j) != Gesture::Mark && out.at(j) != Gesture::Corner) continue;
            const QPolygonF& h = strokes.at(j).points;
            if (h.isEmpty() || sizeOf(h) > 0.45 * length) continue;
            const auto nearest = [&h](QPointF end) {
                double d = std::numeric_limits<double>::infinity();
                for (const QPointF& q : h) d = std::min(d, QLineF(q, end).length());
                return d;
            };
            const double toEnd = nearest(shaft.last()), toStart = nearest(shaft.first());
            if (std::min(toEnd, toStart) > 0.2 * length) continue;
            if (out.at(i) == Gesture::Line) tip[i] = toEnd <= toStart ? shaft.last() : shaft.first();
            out[i] = Gesture::Arrow;
            out[j] = Gesture::Arrowhead;
            head[j] = i;
        }
    }
    if (tips != nullptr) *tips = tip;
    if (headOf != nullptr) *headOf = head;
    return out;
}

QPolygonF simplified(const QPolygonF& points, double tolerance)
{
    if (points.size() < 3) return points;
    QList<bool> keep(points.size(), false);
    keep.first() = keep.last() = true;
    QList<std::pair<int, int>> todo{{0, int(points.size()) - 1}};
    while (!todo.isEmpty()) {
        const auto [a, b] = todo.takeLast();
        int far = -1;
        double most = tolerance;
        for (int i = a + 1; i < b; ++i)
            if (const double d = distance(points.at(i), points.at(a), points.at(b)); d > most) {
                most = d;
                far = i;
            }
        if (far >= 0) {
            keep[far] = true;
            todo << std::make_pair(a, far) << std::make_pair(far, b);
        }
    }
    QPolygonF out;
    for (int i = 0; i < points.size(); ++i)
        if (keep.at(i)) out << points.at(i);
    return out;
}

double insideShare(const QPolygonF& ring, const QRectF& box)
{
    if (ring.size() < 3) return 0;
    if (!(box.width() > 0) || !(box.height() > 0)) return ring.containsPoint(box.center(), Qt::OddEvenFill) ? 1 : 0;
    constexpr int n = 5;
    int in = 0;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            if (ring.containsPoint(QPointF(box.left() + box.width() * (i + 0.5) / n, box.top() + box.height() * (j + 0.5) / n), Qt::OddEvenFill)) ++in;
    return double(in) / (n * n);
}

QPolygonF alongOf(const Stroke& stroke)
{
    return sampledOf(stroke.points, std::max(stroke.width / 2, lengthOf(stroke.points) / 400 + 1e-12));
}

double reachOf(const Stroke& stroke)
{
    return std::max(1.5 * stroke.width, 0.06 * sizeOf(stroke.points));
}

QJsonObject readingOf(const Stroke& stroke, int number, Gesture gesture, bool points)
{
    QJsonObject o{{QStringLiteral("stroke"), stroke.id > 0 ? stroke.id : number},
                  {QStringLiteral("drawn as"), nameOf(gesture)},
                  {QStringLiteral("tool"), stroke.highlighter ? QStringLiteral("highlighter") : QStringLiteral("pen")},
                  {QStringLiteral("colour"), stroke.color.name(QColor::HexRgb)},
                  {QStringLiteral("box"), boxJson(stroke.bounds())}};
    if (stroke.addressed) o.insert(QStringLiteral("addressed"), true);
    if (points && !stroke.points.isEmpty()) {
        // A few dozen points at most: its shape, not its every wobble.
        double tolerance = std::max(stroke.width / 2, 0.01 * sizeOf(stroke.points));
        QPolygonF few = simplified(stroke.points, tolerance);
        while (few.size() > 32) few = simplified(stroke.points, tolerance *= 1.6);
        QJsonArray list;
        for (const QPointF& pt : few) list.append(QJsonArray{rounded(pt.x()), rounded(pt.y())});
        o.insert(QStringLiteral("points"), list);
    }
    return o;
}

QJsonArray readPage(const QList<Stroke>& strokes, const QList<Target>& targets, bool points)
{
    QJsonArray out;
    QList<QPointF> tips;
    QList<int> heads;
    const QList<Gesture> gestures = gesturesOf(strokes, &tips, &heads);
    for (int i = 0; i < strokes.size(); ++i) {
        const Stroke& s = strokes.at(i);
        const Gesture g = gestures.at(i);
        QJsonObject o = readingOf(s, i + 1, g, points);
        if (s.points.isEmpty()) continue;
        const double reach = reachOf(s);
        switch (g) {
        case Gesture::Arrowhead: {
            const Stroke& shaft = strokes.at(heads.at(i));
            o.insert(QStringLiteral("of stroke"), shaft.id > 0 ? shaft.id : heads.at(i) + 1);
            break;
        }
        case Gesture::Dot:
            if (const QJsonObject on = pointedAt(targets, s.points.first(), QPointF(), 0, reach); !on.isEmpty())
                o.insert(QStringLiteral("on"), on);
            break;
        case Gesture::Corner:
            // What its point is at.
            if (const QJsonObject at = pointedAt(targets, tips.at(i), QPointF(), 0, reach); !at.isEmpty()) o.insert(QStringLiteral("at"), at);
            o.insert(QStringLiteral("point"), QJsonArray{rounded(tips.at(i).x()), rounded(tips.at(i).y())});
            break;
        case Gesture::Ring: {
            // Round each target whole, or some of its words: three fifths
            // of a word inside it, half of a target with none.
            QPolygonF ring = s.points;
            ring << ring.first();
            QJsonArray around;
            for (const Target& t : targets) {
                if (t.words.isEmpty()) {
                    if (insideShare(ring, t.outline.boundingRect()) >= 0.5) around.append(refOf(t));
                    continue;
                }
                QList<int> in;
                for (int w = 0; w < t.words.size(); ++w)
                    if (insideShare(ring, t.words.at(w).second.boundingRect()) >= 0.6) in << w;
                if (!in.isEmpty()) around.append(refOf(t, in));
            }
            if (!around.isEmpty()) o.insert(QStringLiteral("around"), around);
            break;
        }
        case Gesture::Line:
        case Gesture::Arrow:
        case Gesture::Mark: {
            // A highlighter's stroke, or a mark: what it lies on.
            if (s.highlighter || g == Gesture::Mark) {
                if (const QJsonArray on = coveredBy(targets, s); !on.isEmpty())
                    o.insert(s.highlighter ? QStringLiteral("highlights") : QStringLiteral("on"), on);
                if (g != Gesture::Arrow) break;
            }
            // A line along a line of text: under its words, or through them.
            if (g == Gesture::Line) {
                const QJsonArray foot = alongText(targets, s, true), middle = alongText(targets, s, false);
                if (!foot.isEmpty()) o.insert(QStringLiteral("underlines"), foot);
                if (!middle.isEmpty()) o.insert(QStringLiteral("strikes through"), middle);
                if (!foot.isEmpty() || !middle.isEmpty()) break;
            }
            // Else from something, to what it points at.
            const QPointF start = s.points.first(), end = tips.at(i);
            const double length = QLineF(start, end).length();
            const QPointF along = length > 0 ? (end - start) / length : QPointF();
            if (const QJsonObject from = pointedAt(targets, start, -along, 0, reach); !from.isEmpty()) o.insert(QStringLiteral("from"), from);
            if (const QJsonObject to = pointedAt(targets, end, along, 0.3 * length, reach); !to.isEmpty())
                o.insert(QStringLiteral("points at"), to);
            break;
        }
        }
        out.append(o);
    }
    return out;
}

QString sentenceOf(const QJsonObject& reading, const QString& where, const QString& unit)
{
    const QString drawn = reading.value(QStringLiteral("drawn as")).toString();
    const QString colour = colourNamed(reading.value(QStringLiteral("colour")).toString());
    QString s = QStringLiteral("%1, stroke %2: %3 %4 %5 %6")
                    .arg(where)
                    .arg(reading.value(QStringLiteral("stroke")).toInt())
                    .arg(QStringLiteral("aeiou").contains(colour.left(1)) ? QStringLiteral("an") : QStringLiteral("a"), colour,
                         reading.value(QStringLiteral("tool")).toString(), drawn);
    if (drawn == QLatin1String("arrowhead")) return s + QStringLiteral(" of stroke %1").arg(reading.value(QStringLiteral("of stroke")).toInt());
    const auto has = [&reading](const char* key) { return reading.contains(QLatin1String(key)); };
    const auto value = [&reading](const char* key) { return reading.value(QLatin1String(key)); };
    const std::pair<const char*, const char*> fields[] = {{"around", "around"},       {"underlines", "under"}, {"strikes through", "through"},
                                                          {"highlights", "over"},     {"on", "on"},            {"from", "from"},
                                                          {"points at", "pointing at"}, {"at", "at"}};
    for (const auto& [key, words] : fields) {
        // (What a small mark is on: the one shape that holds it.)
        if (QLatin1String(key) == QLatin1String("on") && has("on shapes")) {
            QStringList parts;
            for (const QJsonValue& v : value("on shapes").toArray()) {
                const QJsonObject on = v.toObject();
                const QJsonArray b = on.value(QStringLiteral("box")).toArray();
                parts << QStringLiteral("the %1 %2%3 (its box %4 to %5)")
                             .arg(on.value(QStringLiteral("layer")).toString(), on.value(QStringLiteral("kind")).toString(),
                                  on.contains(QStringLiteral("in")) ? QStringLiteral(" in ") + on.value(QStringLiteral("in")).toString() : QString(),
                                  pointSaid(QPointF(b.at(0).toDouble(), b.at(1).toDouble())), pointSaid(QPointF(b.at(2).toDouble(), b.at(3).toDouble())));
            }
            s += QStringLiteral(" on ") + parts.join(QStringLiteral(" and "));
            continue;
        }
        if (has(key)) s += QStringLiteral(" %1 %2").arg(QLatin1String(words), said(value(key)));
        // (A ring round part of something large: which part.)
        if (QLatin1String(key) == QLatin1String("around") && has("holds part of"))
            s += QStringLiteral(" %1 %2").arg(has("around") ? QStringLiteral("and") : QStringLiteral("around"), featuresSaid(value("holds part of")));
    }
    // An arrow's tip: the nearest feature to it, the way it comes from, and
    // its tip and tail.
    if (has("nearest its tip")) s += QStringLiteral(", nearest its tip %1").arg(featureSaid(value("nearest its tip").toObject()));
    if (has("tip") && has("tail")) {
        const QJsonArray tip = value("tip").toArray(), tail = value("tail").toArray();
        s += QStringLiteral(", %1its tip at (%2, %3), its tail at (%4, %5)")
                 .arg(has("comes from") ? QStringLiteral("coming from %1, ").arg(value("comes from").toString()) : QString())
                 .arg(tip.at(0).toDouble()).arg(tip.at(1).toDouble()).arg(tail.at(0).toDouble()).arg(tail.at(1).toDouble());
    }
    // A line or a mark along a shape: where it follows it and where it
    // leaves it.
    if (has("follows")) {
        QStringList parts;
        for (const QJsonValue& v : value("follows").toArray()) parts << followSaid(v.toObject());
        s += QStringLiteral(", ") + parts.join(QStringLiteral("; "));
    }
    // What lies under all of it, much larger than it: a frame, a die, a
    // well - seldom what it is about.
    if (has("over")) s += QStringLiteral(", all of it over %1 that covers it").arg(said(value("over")));
    // A freehand stroke as the polyline it was meant to be.
    if (has("polyline")) s += QStringLiteral(", ") + polylineSaid(value("polyline").toObject());
    const QJsonArray b = value("box").toArray();
    if (b.size() == 4)
        s += QStringLiteral(" (its box %1, %2 to %3, %4 %5)")
                 .arg(b.at(0).toDouble()).arg(b.at(1).toDouble()).arg(b.at(2).toDouble()).arg(b.at(3).toDouble()).arg(unit);
    // Its points, when they are few (and no polyline gave its corners).
    const QJsonArray points = value("points").toArray();
    if (!has("polyline") && !points.isEmpty() && points.size() <= MostPointsSaid) {
        QStringList said;
        for (const QJsonValue& v : points) said << QStringLiteral("(%1, %2)").arg(v.toArray().at(0).toDouble()).arg(v.toArray().at(1).toDouble());
        s += QStringLiteral("; its points %1").arg(said.join(QLatin1Char(' ')));
    }
    return s;
}

QString sentencesOf(const QJsonArray& readings, bool slides, int most)
{
    QStringList lines;
    QJsonArray open;
    QStringList addressed;
    for (const QJsonValue& v : readings) {
        const QJsonObject r = v.toObject();
        if (!r.value(QStringLiteral("addressed")).toBool()) {
            open.append(r);
            continue;
        }
        addressed << QStringLiteral("%1 %2").arg(slides ? QStringLiteral("slide %1").arg(r.value(QStringLiteral("slide")).toInt())
                                                        : QStringLiteral("cell %1").arg(r.value(QStringLiteral("cell")).toString()))
                                             .arg(r.value(QStringLiteral("stroke")).toInt());
    }
    const int from = std::max(0, int(open.size()) - most);
    if (from > 0) lines << QStringLiteral("(%1 earlier stroke(s) left out: read_drawings has them all)").arg(from);
    for (int i = from; i < open.size(); ++i) {
        const QJsonObject r = open.at(i).toObject();
        lines << sentenceOf(r, slides ? QStringLiteral("slide %1").arg(r.value(QStringLiteral("slide")).toInt())
                                      : QStringLiteral("cell %1").arg(r.value(QStringLiteral("cell")).toString()),
                            slides ? QStringLiteral("points") : QStringLiteral("µm"));
    }
    if (!addressed.isEmpty())
        lines << QStringLiteral("(%1 stroke(s) marked addressed, left out: %2)").arg(addressed.size()).arg(addressed.join(QStringLiteral(", ")));
    return lines.join(QLatin1Char('\n'));
}

QList<std::pair<QColor, QString>> colors()
{
    return {{QColor(0xe0, 0x1b, 0x24), QApplication::translate("Markup", "Red")},
            {QColor(0xff, 0x78, 0x00), QApplication::translate("Markup", "Orange")},
            {QColor(0xf6, 0xd3, 0x2d), QApplication::translate("Markup", "Yellow")},
            {QColor(0x2e, 0xc2, 0x7e), QApplication::translate("Markup", "Green")},
            {QColor(0x1c, 0x71, 0xd8), QApplication::translate("Markup", "Blue")},
            {QColor(0x91, 0x41, 0xac), QApplication::translate("Markup", "Purple")},
            {QColor(0x00, 0x00, 0x00), QApplication::translate("Markup", "Black")},
            {QColor(0xff, 0xff, 0xff), QApplication::translate("Markup", "White")}};
}

Pen currentPen()
{
    const QucsSettingsFile settings;
    Pen pen;
    pen.tool = toolNamed(settings.value(QLatin1String(kToolKey)).toString());
    const QColor color(settings.value(QLatin1String(kColorKey)).toString());
    if (color.isValid()) pen.color = color;
    pen.width = std::clamp(settings.value(QLatin1String(kWidthKey), pen.width).toDouble(), 1.0, 64.0);
    pen.highlighterWidth = std::clamp(settings.value(QLatin1String(kHighlighterKey), pen.highlighterWidth).toDouble(), 1.0, 96.0);
    return pen;
}

void setCurrentPen(const Pen& pen)
{
    QucsSettingsFile settings;
    settings.setValue(QLatin1String(kToolKey), nameOf(pen.tool));
    settings.setValue(QLatin1String(kColorKey), pen.color.name(QColor::HexRgb));
    settings.setValue(QLatin1String(kWidthKey), pen.width);
    settings.setValue(QLatin1String(kHighlighterKey), pen.highlighterWidth);
    emit Pens::instance()->changed();
}

Pens* Pens::instance()
{
    static Pens* pens = new Pens;
    return pens;
}

// ----------------------------------------------------------------------
// The button

PenButton::PenButton(const QString& part, QWidget* parent) : QToolButton(parent), a_part(part)
{
    setObjectName(QStringLiteral("penButton"));
    setCheckable(true);
    setAutoRaise(true);
    setIconSize(QSize(16, 16));
    setFocusPolicy(Qt::NoFocus);
    setPopupMode(QToolButton::MenuButtonPopup);
    setToolTip(tr("Draw on the %1 with the pen in hand - its menu, the arrow beside it, picks the pen, the highlighter or "
                  "the eraser, the colour and how broad (Escape puts it down; Ctrl+Z undoes the last stroke)")
                   .arg(part.toLower()));
    setAccessibleName(tr("Pen"));
    a_menu = new QMenu(this);
    a_menu->setObjectName(QStringLiteral("penMenu"));
    setMenu(a_menu);
    fillMenu();
    connect(Pens::instance(), &Pens::changed, this, &PenButton::penChanged);
    penChanged();
}

void PenButton::setColorChooser(std::function<QColor(const QColor&)> chooser)
{
    a_chooser = std::move(chooser);
}

void PenButton::fillMenu()
{
    // Made once, and kept as the pen and the drawing are: Claude's get_ui
    // reads a button's menu without opening it.
    const auto pick = [this](const std::function<void(Pen&)>& change) {
        Pen pen = currentPen();
        change(pen);
        setCurrentPen(pen);
        // (Picking a pen is taking it up.)
        if (!isChecked()) setChecked(true);
    };
    auto* tools = new QActionGroup(a_menu);
    for (const auto& [tool, text] : {std::pair{Tool::Pen, tr("Pen")}, std::pair{Tool::Highlighter, tr("Highlighter")},
                                     std::pair{Tool::Eraser, tr("Eraser")}}) {
        QAction* a = a_menu->addAction(icon(Pen{tool, QColor(0x80, 0x80, 0x80)}), text);
        a->setCheckable(true);
        a->setData(int(tool));
        tools->addAction(a);
        connect(a, &QAction::triggered, this, [pick, tool = tool] { pick([tool](Pen& pen) { pen.tool = tool; }); });
        a_tools << a;
    }
    a_menu->addSeparator();
    auto* shades = new QActionGroup(a_menu);
    shades->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);
    for (const auto& [color, text] : colors()) {
        QAction* a = a_menu->addAction(swatch(color), text);
        a->setCheckable(true);
        a->setData(color);
        shades->addAction(a);
        connect(a, &QAction::triggered, this, [pick, color = color] {
            pick([color](Pen& pen) {
                pen.color = color;
                if (pen.tool == Tool::Eraser) pen.tool = Tool::Pen;
            });
        });
        a_colors << a;
    }
    a_other = a_menu->addAction(tr("Other Colour…"), this, [this, pick] {
        const Pen now = currentPen();
        const QColor chosen = a_chooser ? a_chooser(now.color)
                                        : QColorDialog::getColor(now.color, window(), tr("The Pen's Colour"));
        if (!chosen.isValid()) return;
        pick([chosen](Pen& pen) {
            pen.color = chosen;
            if (pen.tool == Tool::Eraser) pen.tool = Tool::Pen;
        });
    });
    a_menu->addSeparator();
    auto* widths = new QActionGroup(a_menu);
    for (int i = 0; i < 3; ++i) {
        QAction* a = a_menu->addAction(i == 0 ? tr("Fine") : i == 1 ? tr("Medium") : tr("Bold"));
        a->setCheckable(true);
        widths->addAction(a);
        connect(a, &QAction::triggered, this, [pick, i] {
            pick([i](Pen& pen) {
                if (pen.tool == Tool::Highlighter) pen.highlighterWidth = kHighlighterWidths[i];
                else {
                    pen.width = kPenWidths[i];
                    pen.tool = Tool::Pen;
                }
            });
        });
        a_widths << a;
    }
    a_menu->addSeparator();
    a_undo = a_menu->addAction(tr("Undo the Last Change"), this, &PenButton::undoRequested);
    a_clearHere = a_menu->addAction(tr("Clear the Drawings on This %1").arg(a_part), this, &PenButton::clearPartRequested);
    a_clearAll = a_menu->addAction(tr("Clear All the Drawings"), this, &PenButton::clearAllRequested);
    updateMenu();
}

void PenButton::updateMenu()
{
    const Pen pen = currentPen();
    for (QAction* a : std::as_const(a_tools)) a->setChecked(a->data().toInt() == int(pen.tool));
    for (QAction* a : std::as_const(a_colors)) a->setChecked(a->data().value<QColor>().rgb() == pen.color.rgb());
    // The widths of the tool in hand - a highlighter's its own; the eraser's
    // none.
    const bool highlighter = pen.tool == Tool::Highlighter;
    const double* offered = highlighter ? kHighlighterWidths : kPenWidths;
    const double now = highlighter ? pen.highlighterWidth : pen.width;
    for (int i = 0; i < a_widths.size(); ++i) {
        QAction* a = a_widths.at(i);
        a->setEnabled(pen.tool != Tool::Eraser);
        a->setChecked(pen.tool != Tool::Eraser && std::abs(offered[i] - now) < 1e-9);
        a->setToolTip(tr("%1 px").arg(offered[i]));
    }
    a_undo->setEnabled(a_canUndo);
    a_clearHere->setEnabled(a_here > 0);
    a_clearAll->setEnabled(a_all > 0);
}

void PenButton::setDrawingState(bool canUndo, int here, int all)
{
    a_canUndo = canUndo;
    a_here = here;
    a_all = all;
    updateMenu();
}

void PenButton::penChanged()
{
    setIcon(icon(currentPen()));
    updateMenu();
}

QIcon PenButton::icon(const Pen& pen)
{
    return QIcon(new PenGlyph(pen));
}

} // namespace qucs_s::markup
