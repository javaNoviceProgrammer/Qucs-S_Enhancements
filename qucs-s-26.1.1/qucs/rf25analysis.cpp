/*
 * rf25analysis.cpp - an RF 2.5D model's Parametric Sweep on its tab
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "rf25analysis.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace qucs_s::rf25 {

namespace {

/// A column's title with its unit.
QString titled(const SweepResult& r, int column)
{
    const QString unit = r.units.value(column);
    return unit.isEmpty() ? r.columns.value(column) : QStringLiteral("%1 [%2]").arg(r.columns.value(column), unit);
}

QString number(double v)
{
    return std::isfinite(v) ? QString::number(v, 'g', 6) : QStringLiteral("-");
}

} // namespace

SweepPlot::SweepPlot(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("rfSweepPlot"));
    setMinimumHeight(140);
}

void SweepPlot::setResult(const SweepResult& r, int quantity)
{
    a_lines.clear();
    a_names.clear();
    const int column = r.parameters + quantity;
    if (r.columns.isEmpty()) {
        update();
        return;
    }
    a_x = titled(r, 0);
    a_y = titled(r, column);
    // A line for each value of the second parameter, in the order met.
    QList<double> seconds;
    for (const SweepPoint& p : r.points) {
        if (!p.solved || p.values.isEmpty()) continue;
        const double y = p.measured.value(quantity, NAN);
        if (!std::isfinite(y)) continue;
        const double second = p.values.size() > 1 ? p.values.at(1) : 0;
        int line = int(seconds.indexOf(second));
        if (line < 0) {
            seconds << second;
            a_lines << QList<QPointF>();
            a_names << (p.values.size() > 1 ? QStringLiteral("%1 = %2").arg(r.columns.value(1), number(second)) : QString());
            line = int(seconds.size()) - 1;
        }
        a_lines[line] << QPointF(p.values.at(0), y);
    }
    for (QList<QPointF>& l : a_lines) std::sort(l.begin(), l.end(), [](QPointF a, QPointF b) { return a.x() < b.x(); });
    update();
}

void SweepPlot::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette pal = palette();
    p.fillRect(rect(), pal.color(QPalette::Base));
    const QRectF area = QRectF(rect()).adjusted(64, 12, -16, -40);
    p.setPen(pal.color(QPalette::Mid));
    p.drawRect(area);
    QRectF bounds;
    for (const QList<QPointF>& l : std::as_const(a_lines))
        for (const QPointF& q : l) bounds = bounds.isNull() ? QRectF(q, QSizeF(0, 0)) : bounds.united(QRectF(q, QSizeF(0, 0)));
    p.setPen(pal.color(QPalette::Text));
    if (a_lines.isEmpty()) {
        p.drawText(area, Qt::AlignCenter, tr("Nothing to plot yet"));
        return;
    }
    if (bounds.width() <= 0) bounds.adjust(-0.5, 0, 0.5, 0);
    if (bounds.height() <= 0) bounds.adjust(0, -0.5, 0, 0.5);
    const auto toPixel = [&](QPointF q) {
        return QPointF(area.left() + (q.x() - bounds.left()) / bounds.width() * area.width(),
                       area.bottom() - (q.y() - bounds.top()) / bounds.height() * area.height());
    };
    // The axes' ends and titles.
    p.drawText(QRectF(area.left(), area.bottom() + 2, 120, 16), Qt::AlignLeft, number(bounds.left()));
    p.drawText(QRectF(area.right() - 120, area.bottom() + 2, 120, 16), Qt::AlignRight, number(bounds.right()));
    p.drawText(QRectF(area.left(), area.bottom() + 18, area.width(), 16), Qt::AlignCenter, a_x);
    p.drawText(QRectF(0, area.top(), 60, 16), Qt::AlignRight, number(bounds.bottom()));
    p.drawText(QRectF(0, area.bottom() - 16, 60, 16), Qt::AlignRight, number(bounds.top()));
    p.save();
    p.translate(12, area.center().y());
    p.rotate(-90);
    p.drawText(QRectF(-area.height() / 2, -10, area.height(), 16), Qt::AlignCenter, a_y);
    p.restore();
    static const QList<QColor> colours{QColor(31, 119, 180), QColor(255, 127, 14), QColor(44, 160, 44), QColor(214, 39, 40),
                                       QColor(148, 103, 189), QColor(140, 86, 75)};
    for (int i = 0; i < a_lines.size(); ++i) {
        const QColor c = colours.at(i % colours.size());
        p.setPen(QPen(c, 2));
        QPainterPath path;
        for (int k = 0; k < a_lines.at(i).size(); ++k) {
            const QPointF q = toPixel(a_lines.at(i).at(k));
            if (k == 0) path.moveTo(q);
            else path.lineTo(q);
            p.drawEllipse(q, 2.5, 2.5);
        }
        p.drawPath(path);
        if (!a_names.at(i).isEmpty()) p.drawText(QPointF(area.right() - 150, area.top() + 14 + 14 * i), a_names.at(i));
    }
}

SweepPage::SweepPage(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("rfSweepPage"));
    auto* rows = new QVBoxLayout(this);
    rows->setContentsMargins(6, 4, 6, 4);
    auto* top = new QHBoxLayout();
    a_summary = new QLabel(this);
    a_summary->setObjectName(QStringLiteral("rfSweepSummary"));
    a_summary->setWordWrap(true);
    top->addWidget(a_summary, 1);
    top->addWidget(new QLabel(tr("Plot:"), this));
    a_quantity = new QComboBox(this);
    a_quantity->setObjectName(QStringLiteral("rfSweepQuantity"));
    top->addWidget(a_quantity);
    a_touchstone = new QPushButton(tr("Write Touchstone Files…"), this);
    a_touchstone->setObjectName(QStringLiteral("rfSweepTouchstone"));
    a_touchstone->setToolTip(tr("Each point's S, full wave, as a Touchstone file of its own beside the model"));
    top->addWidget(a_touchstone);
    rows->addLayout(top);
    auto* split = new QSplitter(Qt::Horizontal, this);
    a_plot = new SweepPlot(split);
    a_table = new QTableWidget(split);
    a_table->setObjectName(QStringLiteral("rfSweepTable"));
    a_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    a_table->verticalHeader()->setVisible(false);
    split->addWidget(a_plot);
    split->addWidget(a_table);
    rows->addWidget(split, 1);
    connect(a_quantity, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (i >= 0) a_plot->setResult(a_result, i);
    });
    connect(a_touchstone, &QPushButton::clicked, this, &SweepPage::touchstoneRequested);
}

void SweepPage::beginRun(const QString& name)
{
    a_summary->setText(tr("%1: running…").arg(name));
    a_table->clear();
    a_table->setRowCount(0);
    a_table->setColumnCount(0);
    a_quantity->clear();
    a_result = SweepResult();
    a_plot->setResult(a_result, 0);
    a_touchstone->setEnabled(false);
}

void SweepPage::showResult(const QString& name, const SweepResult& r, bool old)
{
    a_result = r;
    QString summary = r.error.isEmpty() ? tr("%1: %2 point(s), %3 solved").arg(name).arg(r.points.size()).arg(r.solved)
                                        : tr("%1: %2").arg(name, r.error);
    if (r.failed > 0) summary += tr(", %1 did not").arg(r.failed);
    if (r.stopped) summary += tr(" - stopped");
    if (r.seconds > 0) summary += tr(", in %1 s").arg(r.seconds, 0, 'g', 3);
    if (old) summary += tr(". Of the model before a change: run it again for the model as it is.");
    a_summary->setText(summary);
    a_table->clear();
    a_table->setColumnCount(int(r.columns.size()) + 1);
    QStringList headers;
    for (int c = 0; c < r.columns.size(); ++c) headers << titled(r, c);
    headers << tr("Note");
    a_table->setHorizontalHeaderLabels(headers);
    a_table->setRowCount(int(r.points.size()));
    for (int i = 0; i < r.points.size(); ++i) {
        const SweepPoint& p = r.points.at(i);
        for (int c = 0; c < r.parameters && c < p.values.size(); ++c) a_table->setItem(i, c, new QTableWidgetItem(number(p.values.at(c))));
        for (int q = 0; q < p.measured.size(); ++q) a_table->setItem(i, r.parameters + q, new QTableWidgetItem(number(p.measured.at(q))));
        auto* note = new QTableWidgetItem(p.solved ? QString() : p.why);
        note->setToolTip(p.why);
        a_table->setItem(i, int(r.columns.size()), note);
    }
    a_table->resizeColumnsToContents();
    {
        const QSignalBlocker quiet(a_quantity);
        a_quantity->clear();
        for (int c = r.parameters; c < r.columns.size(); ++c) a_quantity->addItem(r.columns.at(c));
    }
    a_plot->setResult(r, 0);
    bool waves = false;
    for (const SweepPoint& p : r.points) waves = waves || !p.s.isEmpty();
    a_touchstone->setEnabled(waves);
}

} // namespace qucs_s::rf25
