/*
 * designedstyle.cpp - the style the designed themes draw with: Fusion, its
 * controls rounded and flat
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "designedstyle.h"
#include "apptheme.h"
#include "ink.h"

#include <QAbstractButton>
#include <QPainter>
#include <QPainterPath>
#include <QStyleFactory>
#include <QStyleOption>
#include <QToolBar>
#include <QWidget>

#include <algorithm>

namespace qucs_s::apptheme {

namespace {

constexpr qreal kRadius = 5.0;

// A fill a little darker (on light colours) or lighter (on dark ones).
QColor shade(const QColor& fill, double amount)
{
    const bool darkFill = fill.lightnessF() < 0.5;
    return mix(fill, darkFill ? QColor(Qt::white) : QColor(Qt::black), darkFill ? amount * 1.2 : amount * 0.7);
}

QRectF inner(const QRect& rect)
{
    return QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5);
}

void roundedPanel(QPainter* painter, const QRectF& r, const QColor& fill, const QColor& border,
                  qreal borderWidth = 1.0)
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(border.isValid() ? QPen(border, borderWidth) : QPen(Qt::NoPen));
    painter->setBrush(fill.isValid() ? QBrush(fill) : QBrush(Qt::NoBrush));
    painter->drawRoundedRect(r, kRadius, kRadius);
    painter->restore();
}

// A button's panel: the button colour, hovered or pressed; its frame the
// Mid colour, the highlight when it has the focus, part of it for the
// default button.
void buttonPanel(const QStyleOption* option, QPainter* painter, bool flat, bool isDefault)
{
    const bool enabled = option->state & QStyle::State_Enabled;
    const bool down = option->state & (QStyle::State_Sunken | QStyle::State_On);
    const bool hover = enabled && (option->state & QStyle::State_MouseOver);
    if (flat && !down && !hover) return;
    const QPalette& pal = option->palette;
    QColor fill = pal.color(QPalette::Button);
    if (down) fill = shade(fill, 0.14);
    else if (hover) fill = shade(fill, 0.07);
    QColor border = pal.color(QPalette::Mid);
    if (enabled && (option->state & QStyle::State_HasFocus)) border = pal.color(QPalette::Highlight);
    else if (enabled && isDefault) border = mix(border, pal.color(QPalette::Highlight), 0.65);
    roundedPanel(painter, inner(option->rect), fill, border);
}

} // namespace

DesignedStyle::DesignedStyle() : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion")))
{
}

void DesignedStyle::drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                                  const QWidget* widget) const
{
    const QPalette& pal = option->palette;
    const bool enabled = option->state & State_Enabled;
    switch (element) {
    case PE_PanelButtonCommand: {
        const auto* button = qstyleoption_cast<const QStyleOptionButton*>(option);
        buttonPanel(option, painter, button && (button->features & QStyleOptionButton::Flat),
                    button && (button->features & QStyleOptionButton::DefaultButton));
        return;
    }
    case PE_PanelButtonTool:
        // A tool button outside a tool bar (a "..." beside a field); an
        // auto-raised one only when hovered or pressed.
        if (option->state & (State_Raised | State_Sunken | State_On | State_MouseOver))
            buttonPanel(option, painter, !(option->state & State_Raised), false);
        return;
    case PE_PanelLineEdit: {
        const auto* frame = qstyleoption_cast<const QStyleOptionFrame*>(option);
        if (frame == nullptr || frame->lineWidth <= 0) break;   // frameless: the base colour, as Fusion
        roundedPanel(painter, inner(option->rect), pal.color(QPalette::Base), QColor());
        drawPrimitive(PE_FrameLineEdit, option, painter, widget);
        return;
    }
    case PE_FrameLineEdit: {
        const bool focus = enabled && (option->state & State_HasFocus);
        roundedPanel(painter, inner(option->rect), QColor(),
                     focus ? pal.color(QPalette::Highlight) : pal.color(QPalette::Mid), focus ? 1.5 : 1.0);
        return;
    }
    case PE_FrameFocusRect:
        // Buttons and boxes show their focus in their frames.
        if (qobject_cast<const QAbstractButton*>(widget) != nullptr) return;
        break;
    case PE_IndicatorCheckBox: {
        const QRectF box = QRectF(option->rect).adjusted(1.5, 1.5, -1.5, -1.5);
        const qreal side = std::min(box.width(), box.height());
        const QRectF r(box.center().x() - side / 2, box.center().y() - side / 2, side, side);
        const bool on = option->state & State_On, partial = option->state & State_NoChange;
        const QColor accent = pal.color(enabled ? QPalette::Active : QPalette::Disabled, QPalette::Highlight);
        QColor border = pal.color(QPalette::Mid);
        if (enabled && (option->state & (State_MouseOver | State_HasFocus))) border = accent;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(on || partial ? accent : border, 1.0));
        painter->setBrush(on || partial ? accent : pal.color(QPalette::Base));
        painter->drawRoundedRect(r, 3.0, 3.0);
        const QColor mark = pal.color(enabled ? QPalette::Active : QPalette::Disabled, QPalette::HighlightedText);
        painter->setPen(QPen(mark, std::max(1.5, side / 7.0), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->setBrush(Qt::NoBrush);
        if (on) {
            QPainterPath tick;
            tick.moveTo(r.left() + side * 0.24, r.top() + side * 0.52);
            tick.lineTo(r.left() + side * 0.43, r.top() + side * 0.70);
            tick.lineTo(r.left() + side * 0.77, r.top() + side * 0.32);
            painter->drawPath(tick);
        } else if (partial) {
            painter->drawLine(QPointF(r.left() + side * 0.27, r.center().y()),
                              QPointF(r.right() - side * 0.27, r.center().y()));
        }
        painter->restore();
        return;
    }
    case PE_IndicatorRadioButton: {
        const QRectF box = QRectF(option->rect).adjusted(1.5, 1.5, -1.5, -1.5);
        const qreal side = std::min(box.width(), box.height());
        const QRectF r(box.center().x() - side / 2, box.center().y() - side / 2, side, side);
        const bool on = option->state & State_On;
        const QColor accent = pal.color(enabled ? QPalette::Active : QPalette::Disabled, QPalette::Highlight);
        QColor border = pal.color(QPalette::Mid);
        if (enabled && (option->state & (State_MouseOver | State_HasFocus))) border = accent;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(on ? accent : border, 1.0));
        painter->setBrush(on ? accent : pal.color(QPalette::Base));
        painter->drawEllipse(r);
        if (on) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(pal.color(enabled ? QPalette::Active : QPalette::Disabled, QPalette::HighlightedText));
            painter->drawEllipse(r.center(), side * 0.2, side * 0.2);
        }
        painter->restore();
        return;
    }
    default:
        break;
    }
    QProxyStyle::drawPrimitive(element, option, painter, widget);
}

void DesignedStyle::drawControl(ControlElement element, const QStyleOption* option, QPainter* painter,
                                const QWidget* widget) const
{
    if (element == CE_ToolButtonLabel && widget != nullptr
        && qobject_cast<const QToolBar*>(widget->parentWidget()) != nullptr) {
        const auto* tool = qstyleoption_cast<const QStyleOptionToolButton*>(option);
        const QColor paper = option->palette.color(QPalette::Window);
        if (tool != nullptr && !tool->icon.isNull() && ink::isDark(paper)) {
            QStyleOptionToolButton inkedTool(*tool);
            inkedTool.icon = ink::inked(tool->icon, tool->iconSize, widget->devicePixelRatio(), paper);
            QProxyStyle::drawControl(element, &inkedTool, painter, widget);
            return;
        }
    }
    QProxyStyle::drawControl(element, option, painter, widget);
}

int DesignedStyle::pixelMetric(PixelMetric metric, const QStyleOption* option, const QWidget* widget) const
{
    switch (metric) {
    case PM_ButtonShiftHorizontal:
    case PM_ButtonShiftVertical:
        return 0;   // a flat button's label stays where it is when pressed
    default:
        return QProxyStyle::pixelMetric(metric, option, widget);
    }
}

} // namespace qucs_s::apptheme
