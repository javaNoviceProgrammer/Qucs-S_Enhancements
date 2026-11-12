/*
 * rf25analysis.h - an RF 2.5D model's Parametric Sweep on its tab: what it
 *                  found as a table of its points and a plot of a quantity
 *                  against the first parameter swept, a line for each value of
 *                  the second
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_RF25ANALYSIS_H
#define QUCS_RF25ANALYSIS_H

#include "rf25/rf25_optimize.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;

namespace qucs_s::rf25 {

/// A quantity of a sweep against its first parameter: a line for each value
/// of its second (crossed), else one; the points that did not solve left out.
class SweepPlot : public QWidget
{
    Q_OBJECT
public:
    explicit SweepPlot(QWidget* parent = nullptr);
    /// \a result's column \a quantity (from its first quantity, 0).
    void setResult(const SweepResult& result, int quantity);
    /// What it draws: each line's points (x the first parameter's value).
    const QList<QList<QPointF>>& lines() const { return a_lines; }
    QSize sizeHint() const override { return QSize(420, 220); }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QList<QList<QPointF>> a_lines;
    QStringList a_names;        ///< each line's: the second parameter's value
    QString a_x, a_y;           ///< the axes' titles
};

/// The Parametric Sweep's page: its summary, the plot of a quantity chosen,
/// the table of its points (a point that did not solve, why), and the
/// Touchstone files of its points.
class SweepPage : public QWidget
{
    Q_OBJECT
public:
    explicit SweepPage(QWidget* parent = nullptr);
    /// A run begins: nothing shown but that it runs.
    void beginRun(const QString& name);
    /// \a name's result; \a old: of the model before a change.
    void showResult(const QString& name, const SweepResult& result, bool old);
    QLabel* summaryLabel() const { return a_summary; }
    QTableWidget* table() const { return a_table; }
    QComboBox* quantityBox() const { return a_quantity; }
    SweepPlot* plot() const { return a_plot; }
    QPushButton* touchstoneButton() const { return a_touchstone; }

signals:
    /// Its points' Touchstone files to be written.
    void touchstoneRequested();

private:
    QLabel* a_summary = nullptr;
    QComboBox* a_quantity = nullptr;
    SweepPlot* a_plot = nullptr;
    QTableWidget* a_table = nullptr;
    QPushButton* a_touchstone = nullptr;
    SweepResult a_result;
};

} // namespace qucs_s::rf25

#endif
