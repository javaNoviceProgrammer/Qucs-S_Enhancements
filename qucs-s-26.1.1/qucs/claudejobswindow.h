/*
 * claudejobswindow.h - the Background Jobs window: what Claude has run, how
 *                      each stands and how far it has got, with Stop
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_CLAUDEJOBSWINDOW_H
#define QUCS_CLAUDEJOBSWINDOW_H

#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTimer;

namespace qucs_s::claude::jobs {
struct Job;
}

/*!
 * The jobs of every conversation (qucs_s::claude::jobs::Registry) in a window
 * of its own, not modal, one at a time: asked for again, it comes forward.
 * A plain QWidget, not a dialog: tools that wait on an open dialog
 * (QucsControl::openDialog()) are not to take it for one.
 */
class ClaudeJobsWindow : public QWidget
{
    Q_OBJECT

public:
    /// The window, opened or brought forward; \a from's main window is its
    /// parent when it is opened.
    static ClaudeJobsWindow* showWindow(QWidget* from);
    /// The window when it exists, shown or not; null when not.
    static ClaudeJobsWindow* existing();

    QTableWidget* table() const { return a_table; }
    QPushButton* clearButton() const { return a_clear; }
    QPlainTextEdit* details() const { return a_details; }
    QLabel* summary() const { return a_summary; }
    /// The table's row of the job \a id, or -1.
    int rowOf(const QString& id) const;

    enum Column { Name, Kind, Conversation, Status, Progress, Began, Time, Stop, Columns };

protected:
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    explicit ClaudeJobsWindow(QWidget* parent);
    void addRow(const qucs_s::claude::jobs::Job& job);
    void showJob(const qucs_s::claude::jobs::Job& job);
    void removeJob(const QString& id);
    void reload();
    void tick();
    void showSummary();
    void showDetails();
    void setWidths();

    QTableWidget* a_table = nullptr;
    QPlainTextEdit* a_details = nullptr;
    QLabel* a_summary = nullptr;
    QPushButton* a_clear = nullptr;
    QTimer* a_clock = nullptr;
};

#endif // QUCS_CLAUDEJOBSWINDOW_H
