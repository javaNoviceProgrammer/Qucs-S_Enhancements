/*
 * multiphysicsconvergence.cpp - a model tab's Mesh Convergence page (see
 *                               multiphysicsconvergence.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "multiphysicsconvergence.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace qucs_s::fem {

namespace {

QString cellOf(double value, int digits = 6)
{
    return std::isfinite(value) ? formatNumber(value, digits) : QString();
}

QString kindWord(const QString& kind)
{
    if (kind == QLatin1String("start")) return ConvergencePage::tr("as it is");
    if (kind == QLatin1String("adaptive")) return ConvergencePage::tr("refined");
    if (kind == QLatin1String("goal")) return ConvergencePage::tr("refined for the values");
    if (kind == QLatin1String("uniform")) return ConvergencePage::tr("every element cut");
    if (kind == QLatin1String("check")) return ConvergencePage::tr("check: every element cut");
    return kind;
}

QTableWidget* table(QWidget* parent, const char* name)
{
    auto* t = new QTableWidget(0, 0, parent);
    t->setObjectName(QLatin1String(name));
    t->verticalHeader()->hide();
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    return t;
}

} // namespace

ConvergencePlot::ConvergencePlot(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("mpConvergencePlot"));
    setMinimumSize(180, 110);
    setToolTip(tr("Each value's distance from its estimated limit, relative, against each mesh's unknowns (the mesh size h goes as unknowns^-1/2): a straight "
                  "line of slope -q where the error falls as unknowns^-q. The dashed line is the tolerance."));
}

void ConvergencePlot::setSeries(const std::vector<Series>& series, double tolerance)
{
    a_series = series;
    a_tolerance = tolerance;
    update();
}

void ConvergencePlot::clear()
{
    a_series.clear();
    update();
}

void ConvergencePlot::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette pal = palette();
    p.fillRect(rect(), pal.color(QPalette::Base));
    const QRectF box = QRectF(rect()).adjusted(58, 8, -10, -34);
    p.setPen(pal.color(QPalette::Mid));
    p.drawRect(box);
    p.setPen(pal.color(QPalette::Text));
    double xlo = 1e300, xhi = -1e300, ylo = 1e300, yhi = -1e300;
    for (const Series& s : a_series)
        for (const auto& [x, y] : s.points)
            if (x > 0 && y > 0 && std::isfinite(y)) {
                xlo = std::min(xlo, x);
                xhi = std::max(xhi, x);
                ylo = std::min(ylo, y);
                yhi = std::max(yhi, y);
            }
    if (a_tolerance > 0) {
        ylo = std::min(ylo, a_tolerance);
        yhi = std::max(yhi, a_tolerance);
    }
    if (!(xhi > 0) || !(yhi > 0)) {
        p.drawText(box, Qt::AlignCenter | Qt::TextWordWrap, tr("Each value's distance from its estimated limit, against the unknowns of each mesh"));
        return;
    }
    const double lx0 = std::log10(xlo) - 0.05, lx1 = std::log10(xhi) + 0.05;
    const double ly0 = std::floor(std::log10(ylo)), ly1 = std::ceil(std::log10(yhi)) + (std::log10(yhi) == std::ceil(std::log10(yhi)) ? 0.5 : 0);
    const auto mapped = [&](double x, double y) {
        return QPointF(box.left() + (std::log10(x) - lx0) / std::max(lx1 - lx0, 1e-9) * box.width(),
                       box.bottom() - (std::log10(y) - ly0) / std::max(ly1 - ly0, 1e-9) * box.height());
    };
    // The decades of each axis.
    QFont small = font();
    small.setPointSizeF(std::max(6.0, small.pointSizeF() * 0.85));
    p.setFont(small);
    for (int d = int(ly0); d <= int(ly1); ++d) {
        const double y = box.bottom() - (d - ly0) / std::max(ly1 - ly0, 1e-9) * box.height();
        p.setPen(QPen(pal.color(QPalette::Midlight), 0.6));
        p.drawLine(QPointF(box.left(), y), QPointF(box.right(), y));
        p.setPen(pal.color(QPalette::Text));
        p.drawText(QRectF(0, y - 8, 54, 16), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("1e%1").arg(d));
    }
    for (int d = int(std::ceil(lx0)); d <= int(std::floor(lx1)); ++d) {
        const double x = box.left() + (d - lx0) / std::max(lx1 - lx0, 1e-9) * box.width();
        p.setPen(QPen(pal.color(QPalette::Midlight), 0.6));
        p.drawLine(QPointF(x, box.top()), QPointF(x, box.bottom()));
        p.setPen(pal.color(QPalette::Text));
        p.drawText(QRectF(x - 30, box.bottom() + 2, 60, 14), Qt::AlignCenter, QStringLiteral("1e%1").arg(d));
    }
    p.drawText(QRectF(box.left(), box.bottom() + 16, box.width(), 16), Qt::AlignCenter, tr("unknowns"));
    if (a_tolerance > 0) {
        const QPointF a = mapped(std::pow(10.0, lx0), a_tolerance), b = mapped(std::pow(10.0, lx1), a_tolerance);
        p.setPen(QPen(pal.color(QPalette::Text), 1, Qt::DashLine));
        p.drawLine(QPointF(box.left(), a.y()), QPointF(box.right(), b.y()));
        p.drawText(QRectF(box.left() + 4, a.y() - 15, box.width() - 8, 14), Qt::AlignRight, tr("tolerance"));
    }
    static const QColor colors[] = {QColor(31, 119, 180), QColor(214, 39, 40), QColor(44, 160, 44), QColor(148, 103, 189),
                                    QColor(255, 127, 14), QColor(23, 190, 207), QColor(140, 86, 75), QColor(227, 119, 194)};
    int legend = 0;
    for (std::size_t i = 0; i < a_series.size(); ++i) {
        const Series& s = a_series[i];
        const QColor c = colors[i % (sizeof colors / sizeof colors[0])];
        QPainterPath path;
        bool first = true;
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        for (const auto& [x, y] : s.points) {
            if (!(x > 0 && y > 0 && std::isfinite(y))) continue;
            const QPointF at = mapped(x, y);
            if (first) path.moveTo(at);
            else path.lineTo(at);
            first = false;
            p.drawEllipse(at, 2.5, 2.5);
        }
        p.setPen(QPen(c, 1.4));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        p.drawText(QRectF(box.left() + 6, box.top() + 4 + 14 * legend, box.width() - 12, 14), Qt::AlignLeft,
                   s.settled ? s.name : tr("%1 (not settled)").arg(s.name));
        ++legend;
    }
}

ConvergencePage::ConvergencePage(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("mpConvergencePage"));
    auto* all = new QVBoxLayout(this);
    all->setContentsMargins(4, 4, 4, 4);
    auto* top = new QHBoxLayout;
    a_summary = new QLabel(this);
    a_summary->setObjectName(QStringLiteral("mpConvergenceSummary"));
    a_summary->setWordWrap(true);
    a_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    top->addWidget(a_summary, 1);
    a_showMesh = new QPushButton(tr("Show Refined Mesh"), this);
    a_showMesh->setObjectName(QStringLiteral("mpShowRefinedMeshButton"));
    a_showMesh->setToolTip(tr("The last mesh the check refined, in the view (it is not kept with the model; Keep Refined Mesh keeps its sizes)"));
    connect(a_showMesh, &QPushButton::clicked, this, &ConvergencePage::showMeshRequested);
    top->addWidget(a_showMesh);
    a_keep = new QPushButton(tr("Keep Refined Mesh"), this);
    a_keep->setObjectName(QStringLiteral("mpKeepRefinedMeshButton"));
    a_keep->setToolTip(tr("Write the refined mesh's sizes into the mesh node (an Adapted Sizes feature, one step of Undo): built, it gives about that mesh"));
    connect(a_keep, &QPushButton::clicked, this, &ConvergencePage::keepRequested);
    top->addWidget(a_keep);
    all->addLayout(top);
    auto* split = new QSplitter(Qt::Horizontal, this);
    auto* left = new QWidget(split);
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    a_values = table(left, "mpConvergenceValues");
    a_values->setColumnCount(9);
    a_values->setHorizontalHeaderLabels({tr("Value"), tr("Point"), tr("On the finest mesh"), tr("Estimated limit"), tr("Estimated error"), tr("Relative"),
                                         tr("Rate"), tr("Trust"), tr("Settled")});
    a_values->setMaximumHeight(120);
    leftLayout->addWidget(a_values);
    a_rows = table(left, "mpConvergenceRows");
    leftLayout->addWidget(a_rows, 1);
    split->addWidget(left);
    a_plot = new ConvergencePlot(split);
    split->addWidget(a_plot);
    a_notes = new QPlainTextEdit(split);
    a_notes->setObjectName(QStringLiteral("mpConvergenceNotes"));
    a_notes->setReadOnly(true);
    split->addWidget(a_notes);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setStretchFactor(2, 2);
    all->addWidget(split, 1);
    showNothing(QString());
}

void ConvergencePage::setColumns(const QStringList& columns, const QStringList& units, bool points)
{
    a_columns = columns;
    a_points = points;
    QStringList header{tr("Mesh")};
    if (points) header << tr("Point");
    header << tr("Kind") << tr("Triangles") << tr("Unknowns") << tr("Smallest element") << tr("Seconds");
    for (int i = 0; i < columns.size(); ++i) {
        const QString unit = units.value(i);
        header << (unit.isEmpty() ? columns.at(i) : QStringLiteral("%1 (%2)").arg(columns.at(i), unit));
    }
    a_rows->setRowCount(0);
    a_rows->setColumnCount(int(header.size()));
    a_rows->setHorizontalHeaderLabels(header);
}

void ConvergencePage::appendRow(const ConvergenceRow& row, const QStringList& points)
{
    const int r = a_rows->rowCount();
    a_rows->insertRow(r);
    int c = 0;
    const auto put = [&](const QString& text) { a_rows->setItem(r, c++, new QTableWidgetItem(text)); };
    put(QString::number(row.index));
    if (a_points) put(points.value(row.point));
    put(kindWord(row.kind));
    put(QLocale().toString(row.triangles));
    put(QLocale().toString(qlonglong(row.unknowns)));
    put(cellOf(row.smallest, 4));
    put(cellOf(row.seconds, 3));
    for (double v : row.values) put(std::isfinite(v) ? formatNumber(v, 9) : tr("not read"));
    a_rows->scrollToBottom();
}

void ConvergencePage::beginRun(const QString& name)
{
    a_summary->setText(tr("<b>%1</b>: running…").arg(name.toHtmlEscaped()));
    a_rows->setRowCount(0);
    a_rows->setColumnCount(0);
    a_values->setRowCount(0);
    a_notes->clear();
    a_plot->clear();
    a_keep->setEnabled(false);
    a_showMesh->setEnabled(false);
    a_columns.clear();
    a_live.clear();
}

void ConvergencePage::addRow(const ConvergenceRow& row, const QStringList& columns, const QStringList& points)
{
    if (a_columns != columns || a_rows->columnCount() == 0) setColumns(columns, QStringList(), !points.isEmpty());
    appendRow(row, points);
    // The graph as it goes: each value against the last mesh's, as the limit is
    // not known yet.
    a_live.push_back(row);
    std::vector<ConvergencePlot::Series> series;
    for (int c = 0; c < columns.size(); ++c) {
        ConvergencePlot::Series s;
        s.name = columns.at(c);
        const ConvergenceRow* last = nullptr;
        for (const ConvergenceRow& r : a_live)
            if (r.point == row.point) last = &r;
        for (const ConvergenceRow& r : a_live) {
            if (r.point != row.point || &r == last || c >= int(r.values.size()) || c >= int(last->values.size())) continue;
            const double ref = last->values[std::size_t(c)];
            if (ref != 0 && std::isfinite(ref)) s.points.emplace_back(r.unknowns, std::abs(r.values[std::size_t(c)] - ref) / std::abs(ref));
        }
        s.settled = true;
        series.push_back(s);
    }
    a_plot->setSeries(series, 0);
}

void ConvergencePage::drawPlot(const ConvergenceResult& r)
{
    std::vector<ConvergencePlot::Series> series;
    double tolerance = 0;
    for (const ConvergenceResult::Final& f : r.values) {
        ConvergencePlot::Series s;
        s.name = r.columns.value(f.column);
        if (!r.points.isEmpty()) s.name += QStringLiteral(" (%1)").arg(r.points.value(f.point));
        s.settled = f.settled;
        const double ref = std::isfinite(f.limit) ? f.limit : f.value;
        tolerance = std::max(tolerance, f.tolerance);
        for (const ConvergenceRow& row : r.rows) {
            if (row.point != f.point || f.column >= int(row.values.size())) continue;
            const double v = row.values[std::size_t(f.column)];
            if (ref != 0 && std::isfinite(v)) s.points.emplace_back(row.unknowns, std::abs(v - ref) / std::abs(ref));
        }
        series.push_back(s);
    }
    a_plot->setSeries(series, tolerance);
}

void ConvergencePage::showResult(const QString& name, const ConvergenceResult& r, bool old, bool meshShown)
{
    a_live.clear();
    a_notes->clear();
    a_values->setRowCount(0);
    if (!r.error.isEmpty()) {
        a_summary->setText(tr("<b>%1</b> could not be run: %2").arg(name.toHtmlEscaped(), r.error.toHtmlEscaped()));
        a_rows->setRowCount(0);
        a_plot->clear();
        a_keep->setEnabled(false);
        a_showMesh->setEnabled(false);
        return;
    }
    const QString verdict = r.converged ? tr("<span style=\"color:#2e7d32\"><b>Converged</b></span>") : tr("<span style=\"color:#c62828\"><b>Not converged</b></span>");
    // (The summary's own first word is the verdict's: said once, in colour.)
    QString rest = r.summary;
    for (const QString& lead : {QStringLiteral("Converged: "), QStringLiteral("Not converged: "), QStringLiteral("Cancelled, not converged; ")})
        if (rest.startsWith(lead)) rest = rest.mid(lead.size());
    QString text = tr("<b>%1</b>: %2: %3").arg(name.toHtmlEscaped(), verdict, rest.toHtmlEscaped());
    if (old) text += tr(" <i>(of the model before a change: run it again)</i>");
    a_summary->setText(text);
    setColumns(r.columns, r.units, !r.points.isEmpty());
    for (const ConvergenceRow& row : r.rows) appendRow(row, r.points);
    a_values->setColumnHidden(1, r.points.isEmpty());
    for (const ConvergenceResult::Final& f : r.values) {
        const int row = a_values->rowCount();
        a_values->insertRow(row);
        const QString unit = r.units.value(f.column);
        const auto withUnit = [&](double v) { return std::isfinite(v) ? (unit.isEmpty() ? formatNumber(v, 9) : formatNumber(v, 9) + QLatin1Char(' ') + unit) : QString(); };
        a_values->setItem(row, 0, new QTableWidgetItem(r.columns.value(f.column)));
        a_values->setItem(row, 1, new QTableWidgetItem(r.points.value(f.point)));
        a_values->setItem(row, 2, new QTableWidgetItem(withUnit(f.value)));
        a_values->setItem(row, 3, new QTableWidgetItem(withUnit(f.limit)));
        a_values->setItem(row, 4, new QTableWidgetItem(std::isfinite(f.error) ? withUnit(f.error) : tr("unknown")));
        a_values->setItem(row, 5, new QTableWidgetItem(std::isfinite(f.relative) ? formatNumber(f.relative * 100, 3) + QStringLiteral(" %") : tr("unknown")));
        a_values->setItem(row, 6, new QTableWidgetItem(std::isfinite(f.rate) ? QStringLiteral("N^-%1").arg(formatNumber(f.rate, 3)) : QString()));
        auto* trust = new QTableWidgetItem(f.trust);
        trust->setToolTip(f.why);
        a_values->setItem(row, 7, trust);
        a_values->setItem(row, 8, new QTableWidgetItem(f.settled ? tr("yes, within %1 %").arg(formatNumber(f.tolerance * 100, 3)) : tr("no")));
    }
    QStringList lines;
    lines << tr("Refined by: %1").arg(r.method) << QString();
    lines << tr("What stands behind the verdict:");
    for (const QString& e : r.evidence) lines << QStringLiteral("• ") + e;
    lines << QString() << tr("Each value:");
    for (const ConvergenceResult::Final& f : r.values)
        lines << QStringLiteral("• %1%2: %3").arg(r.columns.value(f.column), r.points.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(r.points.value(f.point)), f.why);
    if (!r.notes.isEmpty()) {
        lines << QString() << tr("Notes:");
        for (const QString& n : r.notes) lines << QStringLiteral("• ") + n;
    }
    lines << QString()
          << tr("%1 solves%2, %3 unknowns solved in all, the largest mesh %4; %5 s.")
                 .arg(r.solves)
                 .arg(r.adjoints ? tr(" and %1 adjoint solves").arg(r.adjoints) : QString())
                 .arg(QLocale().toString(qlonglong(r.unknownsSolved)))
                 .arg(QLocale().toString(qlonglong(r.largest)))
                 .arg(formatNumber(r.seconds, 3));
    a_notes->setPlainText(lines.join(QLatin1Char('\n')));
    drawPlot(r);
    a_keep->setEnabled(!r.sizes.isEmpty() && !old);
    a_showMesh->setEnabled(meshShown);
}

void ConvergencePage::showNothing(const QString& name)
{
    a_live.clear();
    a_summary->setText(name.isEmpty() ? tr("No mesh convergence check.")
                                      : tr("<b>%1</b> has not been run: its Run checks the values it watches as the mesh is refined.").arg(name.toHtmlEscaped()));
    a_rows->setRowCount(0);
    a_rows->setColumnCount(0);
    a_values->setRowCount(0);
    a_notes->clear();
    a_plot->clear();
    a_keep->setEnabled(false);
    a_showMesh->setEnabled(false);
}

} // namespace qucs_s::fem
