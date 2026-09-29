/*
 * workspacesession.h - the workspace as it was when Qucs-S closed: the
 *                      project open, the documents pane by pane, the
 *                      panes' split and sizes, the window's panels - kept
 *                      in the settings and brought back at the next start
 *                      (Application Settings > Workspace).
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_WORKSPACESESSION_H
#define QUCS_WORKSPACESESSION_H

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace qucs_s::session {

/// One of the editor panes: where it is in the grid (up to 2 x 2), its
/// documents' files in tab order, the one in front.
struct Pane {
    int row = 0;
    int column = 0;
    QStringList documents;
    QString current;
};

/// What was open. Documents without a file (untitled) are not in it.
struct Workspace {
    QString project;                 // the open project's folder, or empty
    QList<Pane> panes;               // row by row, left to right
    int activePane = 0;              // (an index of panes)
    QList<int> rowSizes;             // the rows' heights
    QList<QList<int>> columnSizes;   // each row's panes' widths
    int sideTab = -1;                // the left dock's page: Projects, Content, Components ...
    QDateTime saved;

    bool isEmpty() const;
    int documentCount() const;
    QJsonObject toJson() const;
    /// Read back; what does not read (a pane out of the 2 x 2 grid, a size
    /// that is no number) is left out.
    static Workspace fromJson(const QJsonObject &o);
    /// In words: "project amp, 5 documents in 2 panes".
    QString summary() const;
};

/// Keeps \a workspace and the window's \a windowState (its docks and
/// toolbars, QMainWindow::saveState()) in the settings.
void save(const Workspace &workspace, const QByteArray &windowState);
/// What was kept; an empty workspace when nothing was.
Workspace saved();
QByteArray savedWindowState();
/// Forgets what was kept: the next start begins afresh.
void forget();

/// The version of the window's state (QMainWindow::saveState()): a state
/// of another is not restored.
constexpr int kWindowStateVersion = 1;

} // namespace qucs_s::session

#endif // QUCS_WORKSPACESESSION_H
