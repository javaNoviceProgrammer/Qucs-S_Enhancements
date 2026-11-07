/*
 * multiphysicsoptimization.cpp - a model tab's Optimization page (see
 *                                multiphysicsoptimization.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "multiphysicsoptimization.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace qucs_s::fem {

namespace {

// What a long run's table shows at most: the file and fem_optimize have the rest.
constexpr int kMostShownRows = 5000;

QString cellOf(double value)
{
    return std::isfinite(value) ? formatNumber(value, 6) : QString();
}

} // namespace

OptimizationPlot::OptimizationPlot(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("mpOptimizationPlot"));
    setMinimumSize(160, 90);
}

void OptimizationPlot::clear()
{
    a_best.clear();
    a_front.clear();
    update();
}

void OptimizationPlot::addCost(double cost)
{
    if (!std::isfinite(cost)) {
        // (Did not solve: the best stays what it was.)
        if (!a_best.empty()) a_best.push_back(a_best.back());
    } else {
        a_best.push_back(a_best.empty() ? cost : std::min(a_best.back(), cost));
    }
    update();
}

void OptimizationPlot::setFront(const std::vector<std::pair<double, double>>& points, const QString& x, const QString& y)
{
    a_front = points;
    a_x = x;
    a_y = y;
    update();
}

void OptimizationPlot::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette pal = palette();
    p.fillRect(rect(), pal.color(QPalette::Base));
    const QRectF box = QRectF(rect()).adjusted(60, 8, -8, -22);
    p.setPen(pal.color(QPalette::Mid));
    p.drawRect(box);
    p.setPen(pal.color(QPalette::Text));
    const auto mapped = [&box](double fx, double fy) { return QPointF(box.left() + fx * box.width(), box.bottom() - fy * box.height()); };
    const auto bounds = [](const std::vector<double>& v) {
        double lo = 1e300, hi = -1e300;
        for (double x : v)
            if (std::isfinite(x)) {
                lo = std::min(lo, x);
                hi = std::max(hi, x);
            }
        if (hi < lo) lo = hi = 0;
        if (hi == lo) {
            lo -= 0.5;
            hi += 0.5;
        }
        return std::make_pair(lo, hi);
    };
    if (!a_front.empty()) {
        std::vector<double> xs, ys;
        for (const auto& [x, y] : a_front) {
            xs.push_back(x);
            ys.push_back(y);
        }
        const auto [xlo, xhi] = bounds(xs);
        const auto [ylo, yhi] = bounds(ys);
        p.setBrush(pal.color(QPalette::Highlight));
        p.setPen(Qt::NoPen);
        for (const auto& [x, y] : a_front) p.drawEllipse(mapped((x - xlo) / (xhi - xlo), (y - ylo) / (yhi - ylo)), 3, 3);
        p.setPen(pal.color(QPalette::Text));
        p.drawText(QRectF(box.left(), box.bottom() + 3, box.width(), 18), Qt::AlignCenter, a_x);
        p.drawText(QRectF(0, box.top(), 56, 16), Qt::AlignRight, formatNumber(yhi, 3));
        p.drawText(QRectF(0, box.bottom() - 16, 56, 16), Qt::AlignRight, formatNumber(ylo, 3));
        return;
    }
    if (a_best.empty()) {
        p.drawText(box, Qt::AlignCenter, tr("The best cost, as it falls"));
        return;
    }
    // On a logarithmic scale where it falls by decades and stays above 0.
    const auto [lo, hi] = bounds(a_best);
    const bool logScale = lo > 0 && hi / lo >= 100;
    const double ylo = logScale ? std::log10(lo) : lo, yhi = logScale ? std::log10(hi) : hi;
    QPainterPath path;
    const double last = std::max(1.0, double(a_best.size() - 1));
    for (std::size_t i = 0; i < a_best.size(); ++i) {
        const double v = logScale ? std::log10(a_best[i]) : a_best[i];
        const QPointF point = mapped(double(i) / last, (v - ylo) / (yhi - ylo));
        if (i == 0) path.moveTo(point);
        else path.lineTo(point);
    }
    p.setPen(QPen(pal.color(QPalette::Highlight), 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
    p.setPen(pal.color(QPalette::Text));
    p.drawText(QRectF(box.left(), box.bottom() + 3, box.width(), 18), Qt::AlignCenter, tr("%1 solves").arg(a_best.size()));
    p.drawText(QRectF(0, box.top(), 56, 16), Qt::AlignRight, formatNumber(hi, 3));
    p.drawText(QRectF(0, box.bottom() - 16, 56, 16), Qt::AlignRight, formatNumber(lo, 3));
}

OptimizationPage::OptimizationPage(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("mpOptimizationPage"));
    auto* all = new QVBoxLayout(this);
    all->setContentsMargins(6, 4, 6, 4);
    auto* top = new QHBoxLayout;
    a_summary = new QLabel(this);
    a_summary->setObjectName(QStringLiteral("mpOptimizationSummary"));
    a_summary->setWordWrap(true);
    a_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    top->addWidget(a_summary, 1);
    a_apply = new QPushButton(tr("Apply Optimum"), this);
    a_apply->setObjectName(QStringLiteral("mpOptimizationApply"));
    a_apply->setToolTip(tr("Write the best values into the model's parameters, a step of Undo, and compute its study there. A Pareto front: the design chosen."));
    a_apply->setEnabled(false);
    connect(a_apply, &QPushButton::clicked, this, [this] { emit applyRequested(chosenDesign()); });
    top->addWidget(a_apply, 0, Qt::AlignTop);
    all->addLayout(top);
    auto* split = new QSplitter(Qt::Horizontal, this);
    // The optimum's values, or a Pareto front's designs to choose from, in one place.
    auto* left = new QStackedWidget(split);
    a_left = left;
    auto table = [split](const char* name) {
        auto* t = new QTableWidget(split);
        t->setObjectName(QString::fromLatin1(name));
        t->verticalHeader()->hide();
        t->setEditTriggers(QAbstractItemView::NoEditTriggers);
        t->setSelectionBehavior(QAbstractItemView::SelectRows);
        t->setSelectionMode(QAbstractItemView::SingleSelection);
        return t;
    };
    a_bestTable = table("mpOptimizationBest");
    left->addWidget(a_bestTable);
    a_bestTable->setColumnCount(2);
    a_bestTable->setHorizontalHeaderLabels({tr("At the optimum"), tr("Value")});
    a_bestTable->horizontalHeader()->setStretchLastSection(true);
    a_front = table("mpOptimizationFront");
    left->addWidget(a_front);
    left->setCurrentWidget(a_bestTable);
    connect(a_front, &QTableWidget::itemSelectionChanged, this, [this] {
        if (a_left->currentWidget() == a_front) a_apply->setEnabled(chosenDesign() > 0);
    });
    a_plot = new OptimizationPlot(split);
    a_history = table("mpOptimizationHistory");
    a_notes = new QPlainTextEdit(split);
    a_notes->setObjectName(QStringLiteral("mpOptimizationNotes"));
    a_notes->setReadOnly(true);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setStretchFactor(2, 4);
    split->setStretchFactor(3, 2);
    all->addWidget(split, 1);
}

int OptimizationPage::chosenDesign() const
{
    if (a_left->currentWidget() != a_front) return 0;
    const QList<QTableWidgetItem*> items = a_front->selectedItems();
    return items.isEmpty() ? 0 : items.first()->row() + 1;
}

void OptimizationPage::setColumns(const QStringList& columns, const QStringList& units)
{
    a_columns = columns;
    QStringList headers{tr("Solve")};
    for (int i = 0; i < columns.size(); ++i) {
        const QString unit = units.value(i);
        headers << (unit.isEmpty() || unit == QLatin1String("1") ? columns.at(i) : QStringLiteral("%1 [%2]").arg(columns.at(i), unit));
    }
    headers << tr("Cost") << tr("Why it did not solve");
    a_history->clear();
    a_history->setRowCount(0);
    a_history->setColumnCount(int(headers.size()));
    a_history->setHorizontalHeaderLabels(headers);
    a_history->horizontalHeader()->setStretchLastSection(true);
}

void OptimizationPage::appendRow(const OptimizationRow& row)
{
    if (a_history->rowCount() >= kMostShownRows && !row.best) return;
    const int r = a_history->rowCount();
    a_history->setRowCount(r + 1);
    int c = 0;
    QFont font = a_history->font();
    font.setBold(row.best);
    auto cell = [&](const QString& text) {
        auto* item = new QTableWidgetItem(text);
        item->setFont(font);
        a_history->setItem(r, c++, item);
    };
    cell(QString::number(row.index));
    for (double v : row.values) cell(cellOf(v));
    for (double v : row.measured) cell(cellOf(v));
    cell(cellOf(row.cost));
    cell(row.why.section(QLatin1Char('\n'), 0, 0).left(300));
    if (row.best) a_history->scrollToItem(a_history->item(r, 0));
}

void OptimizationPage::beginRun(const QString& name)
{
    a_summary->setText(tr("<b>%1</b>: running...").arg(name.toHtmlEscaped()));
    a_bestTable->setRowCount(0);
    a_left->setCurrentWidget(a_bestTable);
    a_front->setRowCount(0);
    a_history->setRowCount(0);
    a_columns.clear();
    a_notes->clear();
    a_plot->clear();
    a_apply->setEnabled(false);
    a_apply->setText(tr("Apply Optimum"));
}

void OptimizationPage::addRow(const OptimizationRow& row, const QStringList& columns)
{
    if (a_columns != columns) setColumns(columns, {});
    appendRow(row);
    a_plot->addCost(row.cost);
}

void OptimizationPage::showNothing(const QString& name)
{
    beginRun(name);
    a_summary->setText(tr("<b>%1</b>: not run yet. Its Run button, the toolbar's Optimize, or fem_optimize runs it.").arg(name.toHtmlEscaped()));
}

void OptimizationPage::showResult(const QString& name, const OptimizationResult& result, bool old)
{
    beginRun(name);
    if (!result.error.isEmpty()) {
        a_summary->setText(tr("<b>%1</b> could not be run: %2").arg(name.toHtmlEscaped(), result.error.toHtmlEscaped()));
        return;
    }
    QString text = tr("<b>%1</b> (%2): %3").arg(name.toHtmlEscaped(), result.method.toHtmlEscaped(), result.summary.toHtmlEscaped());
    if (result.solves != result.evaluations) text += tr(" - %1 solves").arg(result.solves);
    text += tr(", %1 s").arg(formatNumber(result.seconds, 3));
    if (old) text += tr("<br><span style='color:#b26a00'>The model changed since this run: run it again to see it as it is.</span>");
    a_summary->setText(text);
    // The optimum.
    QList<OptimizationResult::Value> rows = result.parameters;
    rows << result.measured;
    a_bestTable->setRowCount(int(rows.size()));
    for (int i = 0; i < rows.size(); ++i) {
        a_bestTable->setItem(i, 0, new QTableWidgetItem(rows.at(i).name));
        a_bestTable->setItem(i, 1, new QTableWidgetItem(rows.at(i).text));
    }
    a_bestTable->resizeColumnToContents(0);
    // Why it stopped, what it noticed.
    QStringList notes = result.constraints;
    notes << result.notes << result.search;
    if (std::isfinite(result.yield)) notes.prepend(tr("Yield %1 %, worst-case Cpk %2").arg(formatNumber(100 * result.yield, 4), formatNumber(result.cpk, 4)));
    a_notes->setPlainText(notes.join(QLatin1Char('\n')));
    // Its history.
    setColumns(result.columns, result.units);
    a_history->setUpdatesEnabled(false);
    for (const OptimizationRow& row : result.history) {
        appendRow(row);
        a_plot->addCost(row.cost);
    }
    a_history->setUpdatesEnabled(true);
    a_history->resizeColumnsToContents();
    // A Pareto front: a table of its designs to choose from.
    if (!result.front.empty()) {
        a_left->setCurrentWidget(a_front);
        const int count = int(result.columns.size());
        QStringList headers;
        for (int i = 0; i < count; ++i) {
            const QString unit = result.units.value(i);
            headers << (unit.isEmpty() || unit == QLatin1String("1") ? result.columns.at(i) : QStringLiteral("%1 [%2]").arg(result.columns.at(i), unit));
        }
        a_front->setColumnCount(count);
        a_front->setHorizontalHeaderLabels(headers);
        a_front->setRowCount(int(result.front.size()));
        std::vector<std::pair<double, double>> points;
        const int parameters = count - int(result.kinds.size());
        for (int r = 0; r < int(result.front.size()); ++r) {
            const OptimizationResult::FrontPoint& p = result.front[std::size_t(r)];
            for (int c = 0; c < count; ++c) {
                const double v = c < parameters ? (std::size_t(c) < p.values.size() ? p.values[std::size_t(c)] : NAN)
                                                : (std::size_t(c - parameters) < p.measured.size() ? p.measured[std::size_t(c - parameters)] : NAN);
                a_front->setItem(r, c, new QTableWidgetItem(cellOf(v)));
            }
            if (p.measured.size() >= 2) points.emplace_back(p.measured[0], p.measured[1]);
        }
        a_front->resizeColumnsToContents();
        if (result.kinds.size() >= 2) a_plot->setFront(points, result.columns.value(parameters), result.columns.value(parameters + 1));
        a_apply->setText(tr("Apply Design"));
        a_apply->setEnabled(false);
        return;
    }
    a_apply->setEnabled(result.canApply());
}

} // namespace qucs_s::fem
