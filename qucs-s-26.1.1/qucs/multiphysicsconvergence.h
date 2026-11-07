/*
 * multiphysicsconvergence.h - a model tab's Mesh Convergence page: what a run
 *                             of a mesh convergence check (fem_adapt.h) found -
 *                             its verdict and what stands behind it, each mesh
 *                             as it is solved, each value's estimated limit and
 *                             error, a graph of the values against the mesh -
 *                             and the buttons that show and keep the refined
 *                             mesh
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_MULTIPHYSICSCONVERGENCE_H
#define QUCS_MULTIPHYSICSCONVERGENCE_H

#include "fem_adapt.h"

#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;

namespace qucs_s::fem {

/// Each value's distance from its estimated limit, relative, against the
/// unknowns of each mesh (both scales logarithmic: a straight line of slope -q
/// where it converges as unknowns^-q, the mesh size h going as unknowns^-1/2),
/// the tolerance a line across.
class ConvergencePlot : public QWidget
{
    Q_OBJECT
public:
    explicit ConvergencePlot(QWidget* parent = nullptr);
    struct Series {
        QString name;
        std::vector<std::pair<double, double>> points;   ///< (unknowns, relative distance)
        bool settled = false;
    };
    void setSeries(const std::vector<Series>& series, double tolerance);
    void clear();
    const std::vector<Series>& series() const { return a_series; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    std::vector<Series> a_series;
    double a_tolerance = 0;
};

class ConvergencePage : public QWidget
{
    Q_OBJECT
public:
    explicit ConvergencePage(QWidget* parent = nullptr);

    /// A run begins: its table from nothing.
    void beginRun(const QString& name);
    /// A mesh of the run under way, solved.
    void addRow(const ConvergenceRow& row, const QStringList& columns, const QStringList& points);
    /// \a name's last result; \a old: of the model before a change; \a meshShown:
    /// its refined mesh is in memory, to be shown.
    void showResult(const QString& name, const ConvergenceResult& result, bool old, bool meshShown);
    /// Nothing run yet.
    void showNothing(const QString& name);

    // For the tests.
    QLabel* summaryLabel() const { return a_summary; }
    QTableWidget* rowsTable() const { return a_rows; }
    QTableWidget* valuesTable() const { return a_values; }
    QPlainTextEdit* notesEdit() const { return a_notes; }
    QPushButton* keepButton() const { return a_keep; }
    QPushButton* showMeshButton() const { return a_showMesh; }
    ConvergencePlot* plot() const { return a_plot; }

signals:
    void keepRequested();
    void showMeshRequested();

private:
    void setColumns(const QStringList& columns, const QStringList& units, bool points);
    void appendRow(const ConvergenceRow& row, const QStringList& points);
    void drawPlot(const ConvergenceResult& result);

    QLabel* a_summary = nullptr;
    QTableWidget* a_rows = nullptr;
    QTableWidget* a_values = nullptr;
    QPlainTextEdit* a_notes = nullptr;
    QPushButton* a_keep = nullptr;
    QPushButton* a_showMesh = nullptr;
    ConvergencePlot* a_plot = nullptr;
    QStringList a_columns;
    bool a_points = false;
    std::vector<ConvergenceRow> a_live;   // the run under way's rows, for its plot
};

} // namespace qucs_s::fem

#endif // QUCS_MULTIPHYSICSCONVERGENCE_H
