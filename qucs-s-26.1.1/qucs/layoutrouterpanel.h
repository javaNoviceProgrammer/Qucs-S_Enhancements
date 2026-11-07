/*
 * layoutrouterpanel.h - the layout tab's Route panel: the metal router's
 *                       job for the layout (layoutrouting.h's session,
 *                       shared with Claude's route_* tools) - its
 *                       technology, its nets made by clicking a contact
 *                       and its pads or read from a CSV, their classes,
 *                       guides and keep-outs from the pen's strokes -, a
 *                       run of all, the selected or the changed nets with
 *                       each pass shown and Cancel, the routes over the
 *                       layout before they are written, the failed nets
 *                       marked, the findings to go to, and Commit
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_LAYOUTROUTERPANEL_H
#define QUCS_LAYOUTROUTERPANEL_H

#include <QPointF>
#include <QPointer>
#include <QVector>
#include <QWidget>

class LayoutDoc;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace qucs_s::router {
struct PassRow;
}

namespace qucs_s::routing {

class Session;

class RouterPanel : public QWidget
{
    Q_OBJECT

public:
    explicit RouterPanel(LayoutDoc* doc, QWidget* parent = nullptr);
    ~RouterPanel() override;

    Session* session() const { return a_session; }
    /// Everything shown again from the session: its job, its run, its result.
    void refresh();
    /// The routes, vias and failed nets over the layout (when the cell shown
    /// is the one routed), or nothing.
    void updateOverlay();

    /// Making a net by clicking: the next click on the layout is its start,
    /// those after it its pads, until finishNet().
    void beginNet();
    void finishNet();
    bool makingNet() const { return a_making; }
    /// The click of a net being made (also the tests' way in).
    void addPoint(QPointF point);

    /// The pen's strokes on the cell routed: rings become keep-outs, lines
    /// and arrows guides of the net whose start they begin at (or of the
    /// net selected). How many of each.
    QPair<int, int> takeStrokes();
    /// Nets from a CSV, as route_setup's nets_csv reads it; why not, or empty.
    QString importCsv(const QString& file);
    /// Runs: "all", "selected" or "changed". Why not, or empty.
    QString run(const QString& mode);
    void cancel();
    /// Commits the routes to \a file (the layout itself when it is its own
    /// file). Why not, or empty.
    QString commit(const QString& file, bool allowFindings);
    /// The technology file (relative to the layout's folder, or absolute).
    QString setTechnology(const QString& file);

    // For the tests.
    QTableWidget* netTable() const { return a_nets; }
    QTreeWidget* findingList() const { return a_findings; }
    QListWidget* keepOutList() const { return a_keepOuts; }
    QLabel* statusLabel() const { return a_status; }
    QProgressBar* progressBar() const { return a_progress; }
    QPushButton* runButton() const { return a_run; }
    QPushButton* cancelButton() const { return a_cancel; }
    QPushButton* commitButton() const { return a_commit; }
    QComboBox* modeBox() const { return a_mode; }

signals:
    /// A run ended and its result is shown.
    void runShown();

private:
    void fillNets();
    void fillKeepOuts();
    void fillFindings();
    void saveAndPrepare();
    void showPass(const qucs_s::router::PassRow& row);
    void goToFinding(QTreeWidgetItem* item);
    void chooseTechnology();
    void chooseCsv();
    void chooseCommit();
    void removeSelectedNets();
    void removeSelectedKeepOuts();
    QStringList selectedNets() const;
    QString routedCellName() const;

    QPointer<LayoutDoc> a_doc;
    Session* a_session = nullptr;
    bool a_making = false;
    QVector<QPointF> a_points;
    bool a_filling = false;

    QLineEdit* a_tech = nullptr;
    QTableWidget* a_nets = nullptr;
    QPushButton* a_addNet = nullptr;
    QListWidget* a_keepOuts = nullptr;
    QComboBox* a_mode = nullptr;
    QPushButton* a_run = nullptr;
    QPushButton* a_cancel = nullptr;
    QProgressBar* a_progress = nullptr;
    QLabel* a_status = nullptr;
    QTreeWidget* a_findings = nullptr;
    QPushButton* a_commit = nullptr;
};

} // namespace qucs_s::routing

#endif // QUCS_LAYOUTROUTERPANEL_H
