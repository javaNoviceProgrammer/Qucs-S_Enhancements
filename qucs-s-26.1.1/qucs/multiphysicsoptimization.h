/*
 * multiphysicsoptimization.h - a model tab's Optimization page: what a run of an
 *                              optimization (fem_optimize.h) found - its best
 *                              values, why it stopped, its history as it goes,
 *                              a Pareto front to choose from - and the button
 *                              that writes the optimum in
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_MULTIPHYSICSOPTIMIZATION_H
#define QUCS_MULTIPHYSICSOPTIMIZATION_H

#include "fem_optimize.h"

#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTableWidget;

namespace qucs_s::fem {

/// The cost as it falls over the solves; a Pareto front's designs by their first
/// two objectives.
class OptimizationPlot : public QWidget
{
    Q_OBJECT
public:
    explicit OptimizationPlot(QWidget* parent = nullptr);
    void clear();
    void addCost(double cost);
    void setFront(const std::vector<std::pair<double, double>>& points, const QString& x, const QString& y);
    /// What it holds: the best so far at each solve; the front's designs.
    const std::vector<double>& best() const { return a_best; }
    const std::vector<std::pair<double, double>>& front() const { return a_front; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    std::vector<double> a_best;
    std::vector<std::pair<double, double>> a_front;
    QString a_x, a_y;
};

class OptimizationPage : public QWidget
{
    Q_OBJECT
public:
    explicit OptimizationPage(QWidget* parent = nullptr);

    /// A run begins: its history from nothing.
    void beginRun(const QString& name);
    /// A solve of the run under way.
    void addRow(const OptimizationRow& row, const QStringList& columns);
    /// \a name's last result; \a old: of the model before a change.
    void showResult(const QString& name, const OptimizationResult& result, bool old);
    /// Nothing run yet.
    void showNothing(const QString& name);

    // For the tests.
    QLabel* summaryLabel() const { return a_summary; }
    QTableWidget* bestTable() const { return a_bestTable; }
    QTableWidget* historyTable() const { return a_history; }
    QTableWidget* frontTable() const { return a_front; }
    QPushButton* applyButton() const { return a_apply; }
    QPlainTextEdit* notesEdit() const { return a_notes; }
    OptimizationPlot* plot() const { return a_plot; }
    /// The front's design chosen (from 1); 0: none.
    int chosenDesign() const;

signals:
    /// The optimum to be written in: the front's design \a point, or 0.
    void applyRequested(int point);

private:
    void setColumns(const QStringList& columns, const QStringList& units);
    void appendRow(const OptimizationRow& row);

    QLabel* a_summary = nullptr;
    QStackedWidget* a_left = nullptr;
    QTableWidget* a_bestTable = nullptr;
    QTableWidget* a_front = nullptr;
    QTableWidget* a_history = nullptr;
    QPlainTextEdit* a_notes = nullptr;
    QPushButton* a_apply = nullptr;
    OptimizationPlot* a_plot = nullptr;
    double a_bestCost = 0;
    bool a_hasBest = false;
    QStringList a_columns;
};

} // namespace qucs_s::fem

#endif // QUCS_MULTIPHYSICSOPTIMIZATION_H
