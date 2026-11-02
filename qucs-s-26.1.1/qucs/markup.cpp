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
#include <cmath>

namespace qucs_s::markup {

namespace {

const char* const kToolKey = "Viewers/penTool";
const char* const kColorKey = "Viewers/penColor";
const char* const kWidthKey = "Viewers/penWidth";
const char* const kHighlighterKey = "Viewers/highlighterWidth";

/// How much of a highlighter's colour covers what is under it.
constexpr double HighlighterCover = 0.4;
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
}

void Drawing::add(const Stroke& stroke)
{
    remember();
    a_strokes << stroke;
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
    return true;
}

QJsonArray describe(const Drawing& drawing, const QString& key)
{
    const auto rounded = [](double v) { return std::round(v * 1000) / 1000; };
    QJsonArray out;
    for (const QString& part : drawing.parts()) {
        QRectF box;
        bool first = true;
        QStringList shades;
        int strokes = 0;
        for (const Stroke& s : drawing.strokes()) {
            if (s.on != part) continue;
            ++strokes;
            box = first ? s.bounds() : box.united(s.bounds());
            first = false;
            const QString shade = s.highlighter ? QApplication::translate("Markup", "%1 highlighter").arg(s.color.name(QColor::HexRgb))
                                                : s.color.name(QColor::HexRgb);
            if (!shades.contains(shade)) shades << shade;
        }
        bool numbered = false;
        const int number = part.toInt(&numbered);
        out.append(QJsonObject{{key, numbered ? QJsonValue(number) : QJsonValue(part)},
                               {QStringLiteral("strokes"), strokes},
                               {QStringLiteral("box"), QJsonArray{rounded(box.left()), rounded(box.top()), rounded(box.right()), rounded(box.bottom())}},
                               {QStringLiteral("colours"), QJsonArray::fromStringList(shades)}});
    }
    return out;
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
        p.setPen(QPen(s.color, std::clamp(s.width * pixelsPerUnit, 1.0, BroadestPixels), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
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
