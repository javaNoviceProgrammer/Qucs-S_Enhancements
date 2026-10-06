/*
 * textplacement.cpp - a schematic's texts, what they are drawn over, and
 *                     free spots for them (see textplacement.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "textplacement.h"

#include "components/component.h"
#include "diagrams/diagram.h"
#include "erc.h"
#include "main.h"
#include "node.h"
#include "paintings/painting.h"
#include "schematic.h"
#include "wire.h"
#include "wirelabel.h"

#include <QCoreApplication>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QHash>
#include <QSet>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

namespace qucs_s::textplace {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("TextPlacement", text);
}

// The paintings that are text, or as solid as one: what a part's text
// drawn over them hides. (A line, a box's outline, an arrow are left out:
// a frame drawn round a part of the circuit has texts inside it.)
bool solidPainting(const Painting* p)
{
    static const QStringList kinds{QStringLiteral("Text"),  QStringLiteral("Formula"),       QStringLiteral("TextBox"),
                                   QStringLiteral("Table"), QStringLiteral("ImagePainting"), QStringLiteral("Dimension")};
    return kinds.contains(p->Name.trimmed());
}

// A diagram as it is drawn: its frame and axes, its title, its axes'
// numbers and labels (an eye's measurements beside it).
QRect drawnRect(const Diagram* d)
{
    const QFontMetricsF metrics(QucsSettings.font);
    return d->boundingRect() | d->paintedRect(metrics).toAlignedRect();
}

QString partName(const Component* c, const QHash<const Component*, QString>& refs)
{
    const QString ref = refs.value(c);
    if (!ref.isEmpty()) return ref;
    return c->Name.isEmpty() || c->Name == QLatin1String("*") ? c->Model : c->Name;
}

// \a points round the outline of an ellipse in \a r from \a angle over
// \a span (in 1/16 degree, counterclockwise from 3 o'clock, as Qt draws
// arcs): a segment each 1/24 of a turn.
QList<QPointF> arcPoints(const QRectF& r, int angle, int span)
{
    QList<QPointF> points;
    const int steps = std::max(2, int(std::ceil(std::abs(span) / (16.0 * 15.0))) + 1);
    for (int i = 0; i < steps; ++i) {
        const double a = (angle + double(span) * i / (steps - 1)) / 16.0 * 3.14159265358979323846 / 180.0;
        points << QPointF(r.center().x() + r.width() / 2 * std::cos(a), r.center().y() - r.height() / 2 * std::sin(a));
    }
    return points;
}

// What a part's symbol draws, on the schematic: its strokes (lines, arcs,
// outlines, pins' ends) and what it fills (a filled shape, a text, an
// image).
void drawn(const Component* c, QList<QLineF>& strokes, QList<QPolygonF>& solids)
{
    const QPointF at(c->cx, c->cy);
    const auto outline = [&](const QList<QPointF>& points, bool closed, bool filled) {
        for (int i = 1; i < points.size(); ++i) strokes << QLineF(points.at(i - 1) + at, points.at(i) + at);
        if (closed && points.size() > 2) strokes << QLineF(points.last() + at, points.first() + at);
        if (filled && points.size() > 2) {
            QPolygonF p;
            for (const QPointF& q : points) p << q + at;
            solids << p;
        }
    };
    for (const qucs::Line* l : c->Lines) strokes << QLineF(l->x1, l->y1, l->x2, l->y2).translated(at);
    for (const qucs::Polyline* p : c->Polylines)
        outline(QList<QPointF>(p->points.cbegin(), p->points.cend()), p->closed, p->closed && p->brush.style() != Qt::NoBrush);
    for (const qucs::Arc* a : c->Arcs) outline(arcPoints(QRectF(a->x, a->y, a->w, a->h), a->angle, a->arclen), false, false);
    for (const qucs::Rect* r : c->Rects) {
        const QRectF b(r->x, r->y, r->w, r->h);
        outline({b.topLeft(), b.topRight(), b.bottomRight(), b.bottomLeft()}, true, r->Brush.style() != Qt::NoBrush);
    }
    for (const qucs::Ellips* e : c->Ellipses)
        outline(arcPoints(QRectF(e->x, e->y, e->w, e->h), 0, 16 * 360), true, e->Brush.style() != Qt::NoBrush);
    for (const qucs::Image* i : c->Images) solids << QPolygonF(QRectF(i->x, i->y, i->w, i->h).translated(at));
    for (const Text* t : c->Texts) {
        QFont font = QucsSettings.font;
        font.setPixelSize(std::max(1, int(t->Size)));
        const QFontMetricsF metrics(font);
        const QRectF box(0, 0, metrics.horizontalAdvance(t->s), metrics.height());
        solids << QTransform().translate(t->x + at.x(), t->y + at.y()).rotate(t->angle()).map(QPolygonF(box));
    }
    // (A pin's end: a wire meets it there.)
    for (const Port* p : c->Ports) solids << QPolygonF(QRectF(c->cx + p->x - 2, c->cy + p->y - 2, 4, 4));
}

// Where two boxes meet, its middle.
QPoint meeting(const QRect& a, const QRect& b)
{
    const QRect both = a.intersected(b);
    return both.isValid() ? both.center() : a.center();
}

} // namespace

namespace {

// Whether the segment a-b runs into the rectangle \a r (Liang-Barsky).
bool segmentMeets(const QLineF& l, const QRectF& r)
{
    const double x0 = l.x1(), y0 = l.y1(), dx = l.x2() - x0, dy = l.y2() - y0;
    const double p[4] = {-dx, dx, -dy, dy};
    const double q[4] = {x0 - r.left(), r.right() - x0, y0 - r.top(), r.bottom() - y0};
    double t0 = 0.0, t1 = 1.0;
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0.0) {
            if (q[i] < 0.0) return false;
            continue;
        }
        const double t = q[i] / p[i];
        if (p[i] < 0.0) t0 = std::max(t0, t);
        else t1 = std::min(t1, t);
        if (t0 > t1) return false;
    }
    return true;
}

} // namespace

bool Thing::meets(const QRect& r) const
{
    // (Touching is not meeting: one row of pixels in common is two boxes
    // side by side, as a part's texts are drawn.)
    const QRect inner = r.adjusted(1, 1, -1, -1);
    if (!inner.isValid()) return false;
    // (A wire through it: across it, not along an edge.)
    const QRectF area(inner.left(), inner.top(), inner.width() - 1, inner.height() - 1);
    if (kind == Wire) return segmentMeets(QLineF(line), area);
    if (kind == Text && ink.isValid()) return ink.intersects(inner);
    if (kind != Symbol || !box.intersects(inner)) return box.intersects(inner);
    for (const QLineF& l : strokes)
        if (segmentMeets(l, area)) return true;
    const QPolygonF rect(area);
    for (const QPolygonF& p : solids)
        if (p.intersects(rect)) return true;
    return false;
}

QList<QRect> textBoxes(const Component* c)
{
    QList<QRect> boxes;
    const QFontMetrics metrics(QucsSettings.font, nullptr);
    QRect line(c->tx, c->ty, 0, 0);
    const auto add = [&](const QString& text, int top) {
        const QSize size = metrics.size(0, text);
        line = QRect(c->tx, top, size.width(), size.height());
        boxes << line.translated(c->cx, c->cy);
    };
    if (c->showName && !c->Name.isEmpty()) add(c->Name, c->ty);
    for (const Property* p : c->Props) {
        if (!p->display) continue;
        if ((p->simulators & QucsSettings.DefaultSimulator) != QucsSettings.DefaultSimulator) continue;
        add(p->displayText(), line.bottom());
    }
    return boxes;
}

QList<QRect> inkBoxes(const Component* c)
{
    QList<QRect> ink;
    const QFontMetrics metrics(QucsSettings.font, nullptr);
    QStringList shown;
    if (c->showName && !c->Name.isEmpty()) shown << c->Name;
    for (const Property* p : c->Props)
        if (p->display && (p->simulators & QucsSettings.DefaultSimulator) == QucsSettings.DefaultSimulator) shown << p->displayText();
    const QList<QRect> boxes = textBoxes(c);
    for (int i = 0; i < boxes.size() && i < shown.size(); ++i) {
        // (A text of several lines: its box.)
        if (shown.at(i).contains(QLatin1Char('\n'))) {
            ink << boxes.at(i);
            continue;
        }
        const QRect tight = metrics.tightBoundingRect(shown.at(i)).translated(boxes.at(i).left(), boxes.at(i).top() + metrics.ascent());
        ink << (tight.isValid() ? tight : boxes.at(i));
    }
    return ink;
}

QRect textBlock(const Component* c)
{
    QRect all;
    for (const QRect& r : textBoxes(c)) all |= r;
    return all;
}

QRect labelBox(const WireLabel* l)
{
    const QFontMetrics metrics(QucsSettings.font, nullptr);
    return QRect(QPoint(l->x1, l->y1), metrics.size(0, l->Name)).marginsAdded(QMargins(3, 3, 3, 3));
}

QList<Thing> things(Schematic* sch)
{
    QList<Thing> all;
    const QHash<const Component*, QString> refs = erc::refs(sch);
    for (const ::Wire* w : sch->a_DocWires) {
        Thing t;
        t.kind = Thing::Wire;
        t.line = QLine(w->x1, w->y1, w->x2, w->y2);
        t.box = QRect(w->P1(), w->P2()).normalized();
        t.owner = w;
        t.name = tr("the wire %1,%2-%3,%4").arg(w->x1).arg(w->y1).arg(w->x2).arg(w->y2);
        all << t;
    }
    for (const Component* c : sch->a_DocComps) {
        const QString name = partName(c, refs);
        Thing s;
        s.kind = Thing::Symbol;
        s.box = c->boundingRect();
        s.owner = c;
        s.name = tr("%1's symbol").arg(name);
        drawn(c, s.strokes, s.solids);
        all << s;
        const QList<QRect> boxes = textBoxes(c), ink = inkBoxes(c);
        for (int i = 0; i < boxes.size(); ++i) {
            Thing t;
            t.kind = Thing::Text;
            t.box = boxes.at(i);
            t.ink = ink.value(i);
            t.owner = c;
            t.name = tr("%1's text").arg(name);
            all << t;
        }
    }
    const auto addLabel = [&all](const WireLabel* l) {
        if (l == nullptr) return;
        Thing t;
        t.kind = Thing::Label;
        t.box = labelBox(l);
        t.owner = l;
        t.name = tr("the label %1").arg(l->Name);
        all << t;
    };
    for (const ::Wire* w : sch->a_DocWires) addLabel(w->label());
    for (const Node* n : sch->a_DocNodes) addLabel(n->label());
    int n = 0;
    for (const ::Diagram* d : sch->a_DocDiags) {
        Thing t;
        t.kind = Thing::Diagram;
        t.box = drawnRect(d);
        t.owner = d;
        t.name = tr("diagram %1").arg(++n);
        all << t;
    }
    n = 0;
    for (const ::Painting* p : sch->a_DocPaints) {
        ++n;
        if (!solidPainting(p)) continue;
        Thing t;
        t.kind = Thing::Painting;
        t.box = p->boundingRect();
        t.owner = p;
        t.name = tr("painting %1 (%2)").arg(n).arg(p->Name.trimmed().toLower());
        all << t;
    }
    return all;
}

QList<Thing> without(const QList<Thing>& all, const void* owner, bool textOnly)
{
    QList<Thing> out;
    out.reserve(all.size());
    for (const Thing& t : all)
        if (t.owner != owner || (textOnly && t.kind != Thing::Text && t.kind != Thing::Label)) out << t;
    return out;
}

bool textOverlaps(const Component* c, const QList<Thing>& all)
{
    const QList<QRect> boxes = inkBoxes(c);
    for (const Thing& t : all) {
        if (t.owner == c && t.kind == Thing::Text) continue;
        for (const QRect& b : boxes)
            if (t.meets(b)) return true;
    }
    return false;
}

bool labelOverlaps(const WireLabel* l, const QList<Thing>& all)
{
    const QRect box = labelBox(l);
    for (const Thing& t : all)
        if (t.owner != l && t.meets(box)) return true;
    return false;
}

QList<Overlap> overlaps(Schematic* sch)
{
    QList<Overlap> out;
    const QList<Thing> all = things(sch);
    // (Two texts over each other said once: by the first met - a text, a
    // label, and what it overlaps of which kind.)
    std::set<std::tuple<const void*, const void*, int>> said;
    const QHash<const Component*, QString> refs = erc::refs(sch);
    for (const Component* c : sch->a_DocComps) {
        const QList<QRect> boxes = textBoxes(c), ink = inkBoxes(c);
        if (boxes.isEmpty()) continue;
        const QString name = partName(c, refs);
        for (const Thing& t : all) {
            if (t.owner == c && t.kind == Thing::Text) continue;
            // (Each of a text's lines and each of the other's: said once.)
            if (said.count({c, t.owner, t.kind}) || said.count({t.owner, c, Thing::Text})) continue;
            for (int i = 0; i < ink.size(); ++i) {
                if (!t.meets(ink.at(i))) continue;
                said.insert({c, t.owner, t.kind});
                const QRect other = t.kind == Thing::Wire ? t.box.adjusted(-1, -1, 1, 1) : t.box;
                out << Overlap{tr("%1's text overlaps %2").arg(name, t.owner == c ? tr("its own symbol") : t.name), c->Name, boxes.at(i),
                               other, meeting(ink.at(i), other)};
                break;
            }
        }
    }
    // Diagrams drawn over each other: a title over the axis label of the
    // one above.
    {
        QList<const ::Diagram*> diagrams(sch->a_DocDiags.cbegin(), sch->a_DocDiags.cend());
        for (int i = 0; i < diagrams.size(); ++i)
            for (int j = i + 1; j < diagrams.size(); ++j) {
                const QString clash = diagramClash(diagrams.at(i), diagrams.at(j), j + 1);
                if (clash.isEmpty()) continue;
                const QRect a = drawnRect(diagrams.at(i)), b = drawnRect(diagrams.at(j));
                out << Overlap{tr("diagram %1: %2").arg(i + 1).arg(clash), QString(), a, b, meeting(a, b)};
            }
    }
    const auto labelsOf = [sch] {
        QList<const WireLabel*> labels;
        for (const ::Wire* w : sch->a_DocWires)
            if (w->label() != nullptr) labels << w->label();
        for (const Node* n : sch->a_DocNodes)
            if (n->label() != nullptr) labels << n->label();
        return labels;
    };
    for (const WireLabel* l : labelsOf()) {
        const QRect box = labelBox(l);
        for (const Thing& t : all) {
            if (t.owner == l || !t.meets(box) || said.count({t.owner, l, Thing::Label}) || said.count({l, t.owner, t.kind})) continue;
            said.insert({l, t.owner, t.kind});
            const QRect other = t.kind == Thing::Wire ? t.box.adjusted(-1, -1, 1, 1) : t.box;
            out << Overlap{tr("the label %1 overlaps %2").arg(l->Name, t.name), QString(), box, other, meeting(box, other)};
        }
    }
    return out;
}

QStringList overlapped(const Component* c, const QList<Thing>& all)
{
    QStringList names;
    const QList<QRect> ink = inkBoxes(c);
    for (const Thing& t : all) {
        if (t.owner == c && t.kind == Thing::Text) continue;
        for (const QRect& b : ink)
            if (t.meets(b)) {
                const QString name = t.owner == c ? tr("its own symbol") : t.name;
                if (!names.contains(name)) names << name;
                break;
            }
    }
    return names;
}

QString diagramClash(const ::Diagram* a, const ::Diagram* b, int nb, int* below)
{
    const QRect frameA(a->cx, a->cy - a->y2, a->x2, a->y2), frameB(b->cx, b->cy - b->y2, b->x2, b->y2);
    const QRect drawnA = drawnRect(a), drawnB = drawnRect(b);
    if (!drawnA.intersects(drawnB)) return {};
    // The y that clears it, a below b: its top under all b draws, a gap
    // between them.
    if (below != nullptr && frameA.top() > frameB.top()) *below = a->cy + (drawnB.bottom() - drawnA.top()) + 20;
    if (frameA.intersects(frameB)) return tr("it lies over diagram %1").arg(nb);
    // The title, above the frame: what of the other it meets - below that
    // one's frame are its x-axis's numbers and label.
    const auto titleOf = [](const ::Diagram* d) {
        if (d->title.isEmpty()) return QRect();
        const QFontMetricsF metrics(d->titleFont());
        const int w = int(std::ceil(metrics.horizontalAdvance(d->title)));
        return QRect(d->cx + d->x2 / 2 - w / 2, d->cy - d->y2 - d->titleHeight(), w, d->titleHeight());
    };
    const auto part = [](const QRect& hit, const QRect& frame) {
        return hit.top() > frame.bottom() ? tr("x-axis label") : hit.bottom() < frame.top() ? tr("title") : tr("axis labels");
    };
    if (const QRect t = titleOf(a); t.isValid() && t.intersects(drawnB))
        return tr("its title runs into diagram %1's %2").arg(nb).arg(part(t.intersected(drawnB), frameB));
    if (const QRect t = titleOf(b); t.isValid() && t.intersects(drawnA))
        return tr("diagram %1's title runs into its %2").arg(nb).arg(part(t.intersected(drawnA), frameA));
    return tr("its labels run into diagram %1's").arg(nb);
}

namespace {

// The first of \a candidates (corners of a block of \a boxes, given from
// its corner) where each box is clear of \a near.
std::optional<int> firstClear(const QList<QPoint>& candidates, const QList<QRect>& boxes, const QList<Thing>& near)
{
    for (int i = 0; i < candidates.size(); ++i) {
        bool clear = true;
        for (const QRect& b : boxes) {
            const QRect at = b.translated(candidates.at(i)).adjusted(-2, -2, 2, 2);
            for (const Thing& t : near)
                if (t.meets(at)) {
                    clear = false;
                    break;
                }
            if (!clear) break;
        }
        if (clear) return i;
    }
    return std::nullopt;
}

// Those of \a all within \a area.
QList<Thing> within(const QList<Thing>& all, const QRect& area)
{
    QList<Thing> out;
    for (const Thing& t : all)
        if (t.box.adjusted(-1, -1, 1, 1).intersects(area)) out << t;
    return out;
}

} // namespace

std::optional<QPoint> freeSpot(const Component* c, const QList<Thing>& all, QString* side)
{
    const QList<QRect> boxes = textBoxes(c);
    if (boxes.isEmpty()) return std::nullopt;
    // Each line from the block's corner.
    QRect block;
    for (const QRect& r : boxes) block |= r;
    QList<QRect> lines;
    for (const QRect& r : boxes) lines << r.translated(-block.topLeft());
    const int w = block.width(), h = block.height();
    const QRect body = c->boundingRect();
    constexpr int gap = 4;
    // Beside it, a side at a time in this order where they are as near; a
    // grid step further out, or along the side, is further.
    struct Candidate {
        int cost, side, slide;
        QPoint at;
    };
    QList<Candidate> list;
    static const char* const sides[] = {QT_TRANSLATE_NOOP("TextPlacement", "right"), QT_TRANSLATE_NOOP("TextPlacement", "left"),
                                        QT_TRANSLATE_NOOP("TextPlacement", "below"), QT_TRANSLATE_NOOP("TextPlacement", "above")};
    const int reach = std::max({40, h + 20, w / 2 + 20});
    for (int d = 0; d <= 40; d += 10)
        for (int s = -reach; s <= reach; s += 10) {
            const int across = body.center().y() - h / 2 + s, along = body.center().x() - w / 2 + s;
            const QPoint at[] = {{body.right() + 1 + gap + d, across}, {body.left() - gap - w - d, across},
                                 {along, body.bottom() + 1 + gap + d}, {along, body.top() - gap - h - d}};
            for (int k = 0; k < 4; ++k) list << Candidate{d + std::abs(s) / 2, k, std::abs(s), at[k]};
        }
    std::stable_sort(list.begin(), list.end(), [](const Candidate& a, const Candidate& b) {
        return std::tie(a.cost, a.side, a.slide) < std::tie(b.cost, b.side, b.slide);
    });
    QList<QPoint> corners;
    for (const Candidate& k : std::as_const(list)) corners << k.at;
    const QRect area = body.adjusted(-w - reach - 60, -h - reach - 60, w + reach + 60, h + reach + 60);
    QList<Thing> near;
    for (const Thing& t : within(all, area))
        if (!(t.owner == c && t.kind == Thing::Text)) near << t;
    const std::optional<int> found = firstClear(corners, lines, near);
    if (!found) return std::nullopt;
    if (side != nullptr) *side = tr(sides[list.at(*found).side]);
    return list.at(*found).at - QPoint(c->cx, c->cy);
}

std::optional<QPoint> freeLabelSpot(const WireLabel* l, const QList<Thing>& all)
{
    const QRect box = labelBox(l);
    const QSize size = box.size();
    const QPoint root(l->cx, l->cy);
    const QRect area = QRect(root, QSize()).adjusted(-size.width() - 90, -size.height() - 90, size.width() + 90, size.height() + 90);
    QList<Thing> near;
    for (const Thing& t : within(all, area))
        if (t.owner != l) near << t;
    // Away from the part whose pin it names, first: a label at the left
    // pin of a resistor lying down goes up and to the left, not over it.
    QPointF away(1.0, -1.0);
    for (const Thing& t : std::as_const(near))
        if (t.kind == Thing::Symbol && t.box.adjusted(-1, -1, 1, 1).contains(root)) {
            away = QPointF(root - t.box.center());
            break;
        }
    // Its box's corner about the point it names - the corners, then beside
    // it - a grid step further out each round.
    struct Candidate {
        int d, behind, order;
        QPoint at;
    };
    QList<Candidate> list;
    const int wd = size.width(), ht = size.height(), x = root.x(), y = root.y();
    for (int d = 0; d <= 40; d += 10) {
        const QPoint at[] = {{x + 10 + d, y - 10 - ht - d}, {x - 10 - wd - d, y - 10 - ht - d}, {x + 10 + d, y + 10 + d},
                             {x - 10 - wd - d, y + 10 + d}, {x + 10 + d, y - ht / 2}, {x - 10 - wd - d, y - ht / 2},
                             {x - wd / 2, y - 10 - ht - d}, {x - wd / 2, y + 10 + d}};
        for (int k = 0; k < 8; ++k) {
            const QPointF centre = QRectF(at[k], size).center() - QPointF(root);
            const bool behind = centre.x() * away.x() + centre.y() * away.y() <= 0.0;
            list << Candidate{d, behind ? 1 : 0, k, at[k]};
        }
    }
    std::stable_sort(list.begin(), list.end(), [](const Candidate& a, const Candidate& b) {
        return std::tie(a.d, a.behind, a.order) < std::tie(b.d, b.behind, b.order);
    });
    for (const Candidate& k : std::as_const(list)) {
        const QRect at(k.at, size);
        bool clear = true;
        for (const Thing& t : std::as_const(near))
            if (t.meets(at.adjusted(-3, -3, 3, 3))) {
                clear = false;
                break;
            }
        if (!clear) continue;
        if (clear) return k.at + QPoint(3, 3);
    }
    return std::nullopt;
}

} // namespace qucs_s::textplace
