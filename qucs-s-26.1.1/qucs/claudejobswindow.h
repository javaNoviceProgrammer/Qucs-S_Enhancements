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

#include <functional>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTimer;

namespace qucs_s::claude::jobs {
struct Job;
}

/*!
 * The jobs of every conversation (qucs_s::claude::jobs::Registry) and the
 * user's own runs (simulationruns.h) in a window of its own, not modal, one
 * at a time: asked for again, it comes forward.
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
    ~ClaudeJobsWindow() override;

    QTableWidget* table() const { return a_table; }
    QPushButton* clearButton() const { return a_clear; }
    QPlainTextEdit* details() const { return a_details; }
    QLabel* summary() const { return a_summary; }
    QPushButton* showButton() const { return a_show; }
    QPushButton* killButton() const { return a_kill; }
    QComboBox* filter() const { return a_filter; }
    /// The rows shown: every job, the simulations alone (runs, solves,
    /// sweeps, optimizations, tuning), the user's own, Claude's.
    enum Filter { AllJobs, Simulations, Yours, Claudes };
    void setFilter(Filter filter);
    /// Whether \a job is shown under \a filter.
    static bool passes(const qucs_s::claude::jobs::Job& job, Filter filter);
    /// What Show does with a job: its document in front (a closed tab brought
    /// back); with false, only whether it could. Set by the application.
    using DocumentShower = std::function<bool(const qucs_s::claude::jobs::Job& job, bool show)>;
    static void setDocumentShower(DocumentShower shower);
    /// Show for the chosen row, as its button does; whether it was shown.
    bool showChosenDocument();
    /// Kill for the chosen row, as its button does: the job stopped as its
    /// row's Stop stops it (a simulator's or a command's process killed, a
    /// solve in Qucs-S cancelled at its next step); whether it was asked.
    bool killChosen();
    /// The table's row of the job \a id, or -1.
    int rowOf(const QString& id) const;
    /// Whether the Job column takes the room the others leave, as it does
    /// until the user drags its edge (each column's edge can be dragged; the
    /// widths are kept for the next time the window opens).
    bool nameFillsTheRoom() const { return a_nameFills; }

    enum Column { Name, Kind, Conversation, Status, Progress, Began, Time, Stop, Columns };
    /// (The Conversation column is "Started by": You, or Claude's conversation.)
    static constexpr Column StartedBy = Conversation;

protected:
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

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
    /// The Job column's width, the room the others leave (while it fills it).
    void fitName();
    void saveWidths() const;
    void applyFilter();
    /// Show and Kill on or off, for the chosen row.
    void updateButtons();

    QTableWidget* a_table = nullptr;
    QPlainTextEdit* a_details = nullptr;
    QLabel* a_summary = nullptr;
    QPushButton* a_clear = nullptr;
    QPushButton* a_show = nullptr;
    QPushButton* a_kill = nullptr;
    QComboBox* a_filter = nullptr;
    QTimer* a_clock = nullptr;
    bool a_nameFills = true;   // the Job column follows the window, until the user sizes it
    bool a_fitting = false;    // (fitName() setting it: no user's doing)
};

#endif // QUCS_CLAUDEJOBSWINDOW_H
