/*
 * datadisplay.cpp - a schematic's or data display's page: its toolbar,
 *                   and what the pen drew on it, read against its
 *                   components, wires, labels, paintings and diagrams
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "datadisplay.h"

#include "apptheme.h"
#include "diagrams/diagram.h"
#include "diagrams/graph.h"
#include "diagrams/stackeddiagram.h"
#include "components/component.h"
#include "markup.h"
#include "node.h"
#include "paintings/painting.h"
#include "qucscontrol_p.h"
#include "schematic.h"
#include "wire.h"
#include "wirelabel.h"
#include "statusbar.h"

#include <QApplication>
#include <QColorDialog>
#include <QEvent>
#include <QHBoxLayout>
#include <QIconEngine>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <limits>

namespace qucs_s::datadisplay {

namespace {

// The toolbar's icons, drawn in the text's colour of the moment.
class Glyph : public QIconEngine
{
public:
    enum Kind { Minus, Plus, Fit, Reload };
    explicit Glyph(Kind kind) : a_kind(kind) {}
    QIconEngine* clone() const override { return new Glyph(a_kind); }
    void paint(QPainter* p, const QRect& rect, QIcon::Mode mode, QIcon::State) override
    {
        const QColor c =
            QApplication::palette().color(mode == QIcon::Disabled ? QPalette::Disabled : QPalette::Active, QPalette::WindowText);
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const qreal s = std::min(rect.width(), rect.height());
        p->translate(rect.center().x() - s / 2 + 0.5, rect.center().y() - s / 2 + 0.5);
        p->scale(s / 16.0, s / 16.0);
        p->setPen(QPen(c, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p->setBrush(Qt::NoBrush);
        switch (a_kind) {
        case Minus: p->drawLine(QPointF(4, 8), QPointF(12, 8)); break;
        case Plus:
            p->drawLine(QPointF(4, 8), QPointF(12, 8));
            p->drawLine(QPointF(8, 4), QPointF(8, 12));
            break;
        case Fit:
            // Four corners: all of it in the frame.
            p->drawPolyline(QPolygonF{QPointF(2.5, 6), QPointF(2.5, 2.5), QPointF(6, 2.5)});
            p->drawPolyline(QPolygonF{QPointF(10, 2.5), QPointF(13.5, 2.5), QPointF(13.5, 6)});
            p->drawPolyline(QPolygonF{QPointF(13.5, 10), QPointF(13.5, 13.5), QPointF(10, 13.5)});
            p->drawPolyline(QPolygonF{QPointF(6, 13.5), QPointF(2.5, 13.5), QPointF(2.5, 10)});
            break;
        case Reload: {
            // A circle's arrow: round clockwise from two o'clock to twelve,
            // its filled head at the top pointing on, to the right. (The
            // head was once two strokes off the arc's end at a fixed slant,
            // across the arc's way, and the glyph read as a "G".)
            const QRectF circle(3, 3.5, 10, 10);
            QPainterPath arc;
            arc.arcMoveTo(circle, 30);
            arc.arcTo(circle, 30, -300);
            p->drawPath(arc);
            p->setPen(Qt::NoPen);
            p->setBrush(c);
            p->drawPolygon(QPolygonF{QPointF(11, 3.5), QPointF(7.4, 0.8), QPointF(7.4, 6.2)});
            break;
        }
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
    Kind a_kind;
};

QToolButton* toolButton(QWidget* parent, Glyph::Kind kind, const QString& tip, const char* name)
{
    auto* b = new QToolButton(parent);
    b->setObjectName(QLatin1String(name));
    b->setIcon(QIcon(new Glyph(kind)));
    b->setToolTip(tip);
    b->setAccessibleName(tip.section(QLatin1String(" ("), 0, 0));
    b->setAutoRaise(true);
    b->setIconSize(QSize(16, 16));
    b->setFocusPolicy(Qt::NoFocus);
    b->setProperty("dplTool", true);
    return b;
}

} // namespace

Bar::Bar(Schematic* page) : QWidget(page), a_page(page)
{
    setObjectName(QStringLiteral("dataDisplayBar"));
    setAttribute(Qt::WA_StyledBackground);
    setAutoFillBackground(true);
    auto* bar = new QHBoxLayout(this);
    bar->setContentsMargins(6, 3, 6, 3);
    bar->setSpacing(2);
    a_zoomOut = toolButton(this, Glyph::Minus, tr("Zoom out"), "dplZoomOut");
    a_zoomIn = toolButton(this, Glyph::Plus, tr("Zoom in"), "dplZoomIn");
    a_fit = toolButton(this, Glyph::Fit, tr("Show all of the page"), "dplFit");
    a_reload = toolButton(this, Glyph::Reload, tr("Read the page's simulation data again (its datasets changed on disk)"), "dplReload");
    a_reloadGap = new QWidget(this);
    a_reloadGap->setFixedWidth(12);
    a_pen = new markup::PenButton(tr("Page"), this);
    a_pen->setObjectName(QStringLiteral("dplPen"));
    a_pen->setProperty("dplTool", true);
    bar->addWidget(a_zoomOut);
    bar->addWidget(a_zoomIn);
    bar->addWidget(a_fit);
    bar->addWidget(a_reloadGap);
    bar->addWidget(a_reload);
    bar->addStretch(1);
    bar->addWidget(a_pen);

    connect(a_zoomOut, &QToolButton::clicked, this, [this] {
        a_page->zoomBy(0.5);
        a_page->viewport()->update();
    });
    connect(a_zoomIn, &QToolButton::clicked, this, [this] {
        a_page->zoomBy(2.0);
        a_page->viewport()->update();
    });
    connect(a_fit, &QToolButton::clicked, this, [this] {
        a_page->showAll();
        a_page->viewport()->update();
    });
    connect(a_reload, &QToolButton::clicked, this, [this] {
        a_page->reloadGraphs(true);
        a_page->viewport()->update();
    });
    // The pen taken up and put down; its menu's undo and clearing (a page
    // is one part: "this page" and "every page" are the same).
    connect(a_pen, &QToolButton::toggled, this, [this](bool on) {
        a_page->setPenActive(on);
        a_page->setFocus();
    });
    connect(a_page, &Schematic::penActiveChanged, this, [this](bool on) {
        const QSignalBlocker block(a_pen);
        a_pen->setChecked(on);
    });
    a_pen->setColorChooser([this](const QColor& now) {
        const QucsDoc::InADialog inADialog(a_page);   // (not freed under its own dialog)
        return QColorDialog::getColor(now, this, tr("The Pen's Colour"));
    });
    connect(a_pen, &markup::PenButton::undoRequested, this, [this] { a_page->undoDrawing(); });
    // (The schematic's strokes and its symbol's: the part shown, and both.)
    connect(a_pen, &markup::PenButton::clearPartRequested, this, [this] { a_page->clearDrawing(); });
    connect(a_pen, &markup::PenButton::clearAllRequested, this, [this] { a_page->clearDrawing(true); });
    connect(a_page, &Schematic::drawingChanged, this, &Bar::updatePen);
    updatePen();
    updateKind();
    restyle();
}

void Bar::updatePen()
{
    const markup::Drawing& d = a_page->penDrawing();
    a_pen->setDrawingState(d.canUndo(), d.count(a_page->penPart()), int(d.strokes().size()));
}

void Bar::updateKind()
{
    // Its datasets read again: a data display's.
    const bool data = a_page->isDataDisplayPage();
    a_reload->setVisible(data);
    a_reloadGap->setVisible(data);
}

void Bar::restyle()
{
    const QPalette pal = palette();
    const QString sheet = QStringLiteral(
                              "#dataDisplayBar { border-bottom: 1px solid %1; background: %4; }"
                              "QToolButton[dplTool=\"true\"] { padding: 3px; border: none; border-radius: 4px; }"
                              "QToolButton[dplTool=\"true\"]:hover { background: %2; }"
                              "QToolButton[dplTool=\"true\"]:checked { background: %3; }"
                              "QToolButton[dplTool=\"true\"]::menu-indicator { image: none; }")
                              .arg(pal.color(QPalette::Mid).name(), pal.color(QPalette::Midlight).name(), pal.color(QPalette::Mid).name(),
                                   pal.color(QPalette::Window).name())
        + qucs_s::apptheme::menuButtonRules(QStringLiteral("QToolButton[dplTool=\"true\"]"));
    if (sheet != styleSheet()) setStyleSheet(sheet);
}

void Bar::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        QTimer::singleShot(0, this, &Bar::restyle);
}

// ----------------------------------------------------------------------
// Reading what was drawn

namespace {

/// To four significant digits: what a reading needs, not a float's noise.
double sig(double v)
{
    return std::isfinite(v) ? QString::number(v, 'g', 4).toDouble() : 0.0;
}

QString said(double v)
{
    return QString::number(sig(v), 'g', 4);
}

/// A span [low, high] of values, or one value when they are the same.
QJsonValue span(double low, double high)
{
    if (!(low <= high)) return QJsonValue();
    if (sig(low) == sig(high)) return sig(low);
    return QJsonArray{sig(low), sig(high)};
}

QString spanSaid(const QJsonValue& v)
{
    if (v.isArray()) return QStringLiteral("%1 to %2").arg(said(v.toArray().at(0).toDouble()), said(v.toArray().at(1).toDouble()));
    return said(v.toDouble());
}

struct Range {
    double low = std::numeric_limits<double>::infinity();
    double high = -std::numeric_limits<double>::infinity();
    void take(double v)
    {
        if (!std::isfinite(v)) return;
        low = std::min(low, v);
        high = std::max(high, v);
    }
    bool any() const { return low <= high; }
    QJsonValue json() const { return span(low, high); }
};

/// A diagram's frame and all it draws round it (its title, its axes'
/// numbers), in the page's coordinates.
QRectF wholeOf(const Diagram* d)
{
    return QRectF(d->cx - d->x1, d->cy - d->y2 - d->titleHeight(), d->x1 + d->x3, d->y2 + d->titleHeight() + d->y1);
}

/// Its plot area, in the page's coordinates.
QRectF plotOf(const Diagram* d)
{
    return QRectF(d->cx, d->cy - d->y2, d->x2, d->y2);
}

/// A point of the page in the diagram's own (from its origin, y up).
QPointF localOf(const Diagram* d, QPointF page)
{
    return QPointF(page.x() - d->cx, d->cy - page.y());
}

/// A trace's points as drawn, in the page's coordinates and the diagram's.
struct Drawn {
    const Graph* graph;
    QList<QPointF> page;
    QList<QPointF> local;
};

QList<Drawn> drawnOf(const Diagram* d)
{
    QList<Drawn> out;
    for (const Graph* g : d->Graphs) {
        if (g->hidden) continue;
        Drawn one{g, {}, {}};
        for (auto it = g->begin(); it != g->end(); ++it) {
            if (!it->isPt()) continue;
            const QPointF local(it->getScrX(), it->getScrY());
            one.local << local;
            one.page << QPointF(d->cx + local.x(), d->cy - local.y());
        }
        if (!one.page.isEmpty()) out << one;
    }
    return out;
}

/// The values at \a local of diagram \a d: x, and y on the axis \a g is
/// on (the left one without a trace).
std::pair<double, double> valuesAt(Diagram* d, QPointF local, const Graph* g)
{
    const MappedPoint m = d->pointToValue(local);
    return {m.x, g != nullptr && g->yAxisNo != 0 ? m.y2 : m.y1};
}

double nearestDistance(QPointF p, const QPolygonF& along)
{
    double best = std::numeric_limits<double>::infinity();
    for (const QPointF& a : along) best = std::min(best, QLineF(p, a).length());
    return best;
}

/// What stroke \a s (drawn as \a g, its tip \a tip) is of diagram number
/// \a n: the values it spans, the traces it is round or on, where it
/// points; empty when it is nowhere near it.
QJsonObject inDiagram(const markup::Stroke& s, markup::Gesture g, QPointF tip, Diagram* d, int n)
{
    const QRectF whole = wholeOf(d), plot = plotOf(d);
    const QPolygonF along = markup::alongOf(s);
    QPolygonF ring;
    if (g == markup::Gesture::Ring) {
        ring = s.points;
        ring << ring.first();
    }
    bool near = false;
    for (const QPointF& p : along)
        if (whole.contains(p)) {
            near = true;
            break;
        }
    if (!near && !ring.isEmpty()) near = markup::insideShare(ring, plot) > 0;
    if (!near) return {};

    QJsonObject o{{QStringLiteral("diagram"), n}, {QStringLiteral("type"), d->Name}};
    if (!d->title.isEmpty()) o.insert(QStringLiteral("title"), d->title);
    const bool valued = d->zoomsByRectangle();
    const auto* stacked = dynamic_cast<const StackedDiagram*>(d);
    bool right = false;
    for (const Graph* gr : d->Graphs)
        if (!gr->hidden && gr->yAxisNo != 0) right = true;

    // The values its own points span inside the plot (a ring's, round what
    // it takes in).
    if (valued) {
        Range x, y, y2;
        int pane = -2;
        for (const QPointF& p : along) {
            if (!plot.contains(p)) continue;
            const MappedPoint m = d->pointToValue(localOf(d, p));
            x.take(m.x);
            y.take(m.y1);
            y2.take(m.y2);
            pane = pane == -2 ? m.pane : (pane == m.pane ? pane : -1);
        }
        if (x.any()) {
            if (!d->Graphs.isEmpty() && !d->Graphs.first()->axisName(0).isEmpty())
                o.insert(QStringLiteral("x is"), d->Graphs.first()->axisName(0));
            o.insert(QStringLiteral("x"), x.json());
            o.insert(QStringLiteral("y"), y.json());
            if (right) o.insert(QStringLiteral("y right"), y2.json());
            if (stacked != nullptr && pane >= 0) o.insert(QStringLiteral("pane"), pane + 1);
        }
    }

    // The traces it is round, lies on or crosses: each with the values of
    // its own points it takes in.
    const double reach = std::max(markup::reachOf(s), 4.0);
    const QRectF box = s.bounds().adjusted(-reach, -reach, reach, reach);
    QJsonArray traces;
    for (const Drawn& t : drawnOf(d)) {
        Range x, y;
        int taken = 0;
        for (int i = 0; i < t.page.size(); ++i) {
            const QPointF& p = t.page.at(i);
            if (!box.contains(p)) continue;
            const bool in = !ring.isEmpty() ? ring.containsPoint(p, Qt::OddEvenFill) : nearestDistance(p, along) <= reach;
            if (!in) continue;
            ++taken;
            if (valued) {
                const auto [vx, vy] = valuesAt(d, t.local.at(i), t.graph);
                x.take(vx);
                y.take(vy);
            }
        }
        if (taken == 0) continue;
        QJsonObject one{{QStringLiteral("trace"), t.graph->Var}};
        if (x.any()) {
            one.insert(QStringLiteral("x"), x.json());
            one.insert(QStringLiteral("y"), y.json());
        }
        traces.append(one);
    }
    if (!traces.isEmpty())
        o.insert(!ring.isEmpty() ? QStringLiteral("round") : s.highlighter ? QStringLiteral("highlights") : QStringLiteral("touches"), traces);

    // Where its tip or point is - an arrow's tip, a dot, a corner's point:
    // its values, and the nearest trace's point to it.
    const bool pointing = g == markup::Gesture::Arrow || g == markup::Gesture::Dot || g == markup::Gesture::Corner;
    if (pointing && plot.contains(tip)) {
        if (valued) {
            const MappedPoint m = d->pointToValue(localOf(d, tip));
            QJsonObject at{{QStringLiteral("x"), sig(m.x)}, {QStringLiteral("y"), sig(m.y1)}};
            if (right) at.insert(QStringLiteral("y right"), sig(m.y2));
            if (const QString text = qucs_s::status::readout(d, m); !text.isEmpty()) at.insert(QStringLiteral("readout"), text);
            o.insert(QStringLiteral("at"), at);
        }
        // Within a twentieth of the plot's size, or the stroke's own reach.
        const double within = std::max(reach * 3, 0.05 * std::max(plot.width(), plot.height()));
        double best = within;
        const Drawn* nearest = nullptr;
        int index = -1;
        const QList<Drawn> all = drawnOf(d);
        for (const Drawn& t : all)
            for (int i = 0; i < t.page.size(); ++i)
                if (const double dist = QLineF(t.page.at(i), tip).length(); dist < best) {
                    best = dist;
                    nearest = &t;
                    index = i;
                }
        if (nearest != nullptr) {
            QJsonObject one{{QStringLiteral("trace"), nearest->graph->Var}};
            if (valued) {
                const auto [vx, vy] = valuesAt(d, nearest->local.at(index), nearest->graph);
                one.insert(QStringLiteral("x"), sig(vx));
                one.insert(QStringLiteral("y"), sig(vy));
            }
            o.insert(QStringLiteral("nearest trace"), one);
        }
    }
    return o;
}

} // namespace

namespace {

/// A thin box round a wire, so that a stroke on it or pointing at it finds
/// it, in the page's coordinates.
QPolygonF wireOutline(const Wire* w)
{
    constexpr int half = 3;
    return QPolygonF(QRectF(QPointF(std::min(w->x1, w->x2) - half, std::min(w->y1, w->y2) - half),
                            QPointF(std::max(w->x1, w->x2) + half, std::max(w->y1, w->y2) + half)));
}

/// Each component's ref, as get_schematic gives it: its name; a type's
/// unnamed parts, when there are several, GND#1, GND#2...; a name given
/// twice, R1 then R1#2.
QHash<const Component*, QString> refsOf(const Schematic* page)
{
    QHash<QString, QList<const Component*>> unnamed, named;
    for (const Component* c : page->a_DocComps) {
        if (c->Name.isEmpty() || c->Name == QLatin1String("*")) unnamed[c->Model] << c;
        else named[c->Name] << c;
    }
    QHash<const Component*, QString> refs;
    for (auto it = unnamed.cbegin(); it != unnamed.cend(); ++it)
        for (int k = 0; k < it.value().size(); ++k)
            refs.insert(it.value().at(k), it.value().size() > 1 ? QStringLiteral("%1#%2").arg(it.key()).arg(k + 1) : it.key());
    for (auto it = named.cbegin(); it != named.cend(); ++it)
        for (int k = 0; k < it.value().size(); ++k)
            refs.insert(it.value().at(k), k == 0 ? it.key() : QStringLiteral("%1#%2").arg(it.key()).arg(k + 1));
    return refs;
}

/// A painting as a target: its number, its type, a text's words.
markup::Target paintingTarget(Painting* p, int n)
{
    const QJsonObject o = qucs_s::control::paintingJson(p, n);
    QString text = QStringLiteral("%1 (%2)").arg(n).arg(o.value(QStringLiteral("type")).toString());
    if (const QString words = o.value(QStringLiteral("text")).toString().simplified(); !words.isEmpty())
        text += QStringLiteral(": ") + words;
    return markup::Target{QStringLiteral("painting"), text, QPolygonF(QRectF(p->boundingRect())), {}};
}

} // namespace

QJsonArray readDrawing(const Schematic* page, bool points)
{
    QJsonArray out;
    for (const QString& part : {QStringLiteral("page"), QStringLiteral("symbol")}) {
        const QList<markup::Stroke> strokes = page->penDrawing().on(part);
        if (strokes.isEmpty()) continue;
        // What is on the part, the uppermost last: the symbol's paintings;
        // or the diagrams (by their numbers, as get_schematic gives them),
        // the paintings, the wires, the components and the net labels.
        QList<markup::Target> targets;
        QList<Diagram*> diagrams;
        if (part == QLatin1String("symbol")) {
            int n = 0;
            for (Painting* p : page->a_SymbolPaints) targets << paintingTarget(p, ++n);
        } else {
            for (Diagram* d : page->a_DocDiags) {
                diagrams << d;
                const int n = int(diagrams.size());
                targets << markup::Target{QStringLiteral("diagram"),
                                          d->title.isEmpty() ? QString::number(n) : QStringLiteral("%1: %2").arg(n).arg(d->title),
                                          QPolygonF(wholeOf(d)),
                                          {}};
            }
            int n = 0;
            for (Painting* p : page->a_DocPaints) targets << paintingTarget(p, ++n);
            for (const Wire* w : page->a_DocWires)
                targets << markup::Target{QStringLiteral("wire"),
                                          QStringLiteral("[%1, %2, %3, %4]").arg(w->x1).arg(w->y1).arg(w->x2).arg(w->y2),
                                          wireOutline(w),
                                          {}};
            const QHash<const Component*, QString> refs = refsOf(page);
            for (const Component* c : page->a_DocComps)
                targets << markup::Target{QStringLiteral("component"), refs.value(c), QPolygonF(QRectF(c->boundingRect())), {}};
            for (const Wire* w : page->a_DocWires)
                if (w->hasLabel())
                    targets << markup::Target{QStringLiteral("label"), w->label()->Name, QPolygonF(QRectF(w->label()->boundingRect())), {}};
            for (const Node* nd : page->a_DocNodes)
                if (nd->hasLabel())
                    targets << markup::Target{QStringLiteral("label"), nd->label()->Name, QPolygonF(QRectF(nd->label()->boundingRect())), {}};
        }
        const QJsonArray read = markup::readPage(strokes, targets, points);
        QList<QPointF> tips;
        QList<int> heads;
        const QList<markup::Gesture> gestures = markup::gesturesOf(strokes, &tips, &heads);
        for (int i = 0; i < read.size(); ++i) {
            QJsonObject r = read.at(i).toObject();
            const int id = r.value(QStringLiteral("stroke")).toInt();
            int k = -1;
            for (int j = 0; j < strokes.size(); ++j)
                if (strokes.at(j).id == id) k = j;
            r.insert(QStringLiteral("page"), part);
            // A ring round a part of the circuit: the parts, labels and
            // diagrams it takes in - not each wire between them.
            if (r.contains(QStringLiteral("around"))) {
                QJsonArray kept;
                for (const QJsonValue& v : r.value(QStringLiteral("around")).toArray())
                    if (!v.toObject().contains(QStringLiteral("wire"))) kept.append(v);
                if (kept.isEmpty()) r.remove(QStringLiteral("around"));
                else r.insert(QStringLiteral("around"), kept);
            }
            if (k >= 0 && gestures.at(k) != markup::Gesture::Arrowhead) {
                QJsonArray in;
                for (int n = 0; n < diagrams.size(); ++n)
                    if (const QJsonObject o = inDiagram(strokes.at(k), gestures.at(k), tips.at(k), diagrams.at(n), n + 1); !o.isEmpty())
                        in.append(o);
                if (!in.isEmpty()) r.insert(QStringLiteral("in diagrams"), in);
            }
            out.append(r);
        }
    }
    return out;
}

namespace {

QString tracesSaid(const QJsonArray& traces)
{
    QStringList parts;
    for (const QJsonValue& v : traces) {
        const QJsonObject t = v.toObject();
        QString one = QStringLiteral("%1").arg(t.value(QStringLiteral("trace")).toString());
        if (t.contains(QStringLiteral("x")))
            one += QStringLiteral(" (x %1, y %2)").arg(spanSaid(t.value(QStringLiteral("x"))), spanSaid(t.value(QStringLiteral("y"))));
        parts << one;
    }
    return parts.join(QStringLiteral(", "));
}

QString diagramSaid(const QJsonObject& o)
{
    QString s = QStringLiteral("in diagram %1").arg(o.value(QStringLiteral("diagram")).toInt());
    if (o.contains(QStringLiteral("title"))) s += QStringLiteral(" \"%1\"").arg(o.value(QStringLiteral("title")).toString());
    if (o.contains(QStringLiteral("x"))) {
        const QString xName = o.value(QStringLiteral("x is")).toString();
        s += QStringLiteral(" over %1 %2, y %3")
                 .arg(xName.isEmpty() ? QStringLiteral("x") : xName, spanSaid(o.value(QStringLiteral("x"))), spanSaid(o.value(QStringLiteral("y"))));
        if (o.contains(QStringLiteral("y right"))) s += QStringLiteral(" (right axis %1)").arg(spanSaid(o.value(QStringLiteral("y right"))));
        if (o.contains(QStringLiteral("pane"))) s += QStringLiteral(" in pane %1").arg(o.value(QStringLiteral("pane")).toInt());
    }
    if (o.contains(QStringLiteral("round"))) s += QStringLiteral(", round the trace(s) ") + tracesSaid(o.value(QStringLiteral("round")).toArray());
    if (o.contains(QStringLiteral("highlights"))) s += QStringLiteral(", highlighting the trace(s) ") + tracesSaid(o.value(QStringLiteral("highlights")).toArray());
    if (o.contains(QStringLiteral("touches"))) s += QStringLiteral(", on the trace(s) ") + tracesSaid(o.value(QStringLiteral("touches")).toArray());
    if (o.contains(QStringLiteral("at"))) {
        const QJsonObject at = o.value(QStringLiteral("at")).toObject();
        s += QStringLiteral(", its point at %1").arg(at.contains(QStringLiteral("readout"))
                                                         ? at.value(QStringLiteral("readout")).toString()
                                                         : QStringLiteral("x %1, y %2").arg(said(at.value(QStringLiteral("x")).toDouble()),
                                                                                            said(at.value(QStringLiteral("y")).toDouble())));
    }
    if (o.contains(QStringLiteral("nearest trace"))) {
        const QJsonObject t = o.value(QStringLiteral("nearest trace")).toObject();
        s += QStringLiteral(", nearest the trace %1").arg(t.value(QStringLiteral("trace")).toString());
        if (t.contains(QStringLiteral("x")))
            s += QStringLiteral(" at x %1, y %2").arg(said(t.value(QStringLiteral("x")).toDouble()), said(t.value(QStringLiteral("y")).toDouble()));
    }
    return s;
}

} // namespace

QString sentencesOf(const QJsonArray& readings, int most)
{
    QStringList lines;
    QJsonArray open;
    QStringList addressed;
    for (const QJsonValue& v : readings) {
        const QJsonObject r = v.toObject();
        if (r.value(QStringLiteral("addressed")).toBool()) addressed << QString::number(r.value(QStringLiteral("stroke")).toInt());
        else open.append(r);
    }
    const int from = std::max(0, int(open.size()) - most);
    if (from > 0) lines << QStringLiteral("(%1 earlier stroke(s) left out: read_drawings has them all)").arg(from);
    for (int i = from; i < open.size(); ++i) {
        const QJsonObject r = open.at(i).toObject();
        const bool symbol = r.value(QStringLiteral("page")).toString() == QLatin1String("symbol");
        QString line = markup::sentenceOf(r, symbol ? QStringLiteral("The symbol") : QStringLiteral("The page"),
                                          symbol ? QStringLiteral("in the symbol's coordinates") : QStringLiteral("in the page's coordinates"));
        for (const QJsonValue& v : r.value(QStringLiteral("in diagrams")).toArray()) line += QStringLiteral("; ") + diagramSaid(v.toObject());
        lines << line;
    }
    if (!addressed.isEmpty())
        lines << QStringLiteral("(%1 stroke(s) marked addressed, left out: %2)").arg(addressed.size()).arg(addressed.join(QStringLiteral(", ")));
    return lines.join(QLatin1Char('\n'));
}

} // namespace qucs_s::datadisplay
