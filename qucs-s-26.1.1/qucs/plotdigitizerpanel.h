/*
 * plotdigitizerpanel.h - the PDF viewer's plot digitizer, beside the pages:
 *                        plots found or marked, calibrated, their curves
 *                        extracted, points placed and moved by hand under a
 *                        magnifier, everything drawn over the page, the numbers
 *                        copied or written out; kept with the PDF
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_PLOTDIGITIZERPANEL_H
#define QUCS_PLOTDIGITIZERPANEL_H

#include "pdfdoc.h"
#include "plotdigitizer.h"

#include <QWidget>
#include <memory>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;
class QToolButton;

namespace qucs_s::digitizer {

/*!
 * The panel and what it does with the mouse on the pages. Everything it holds
 * is written to the file beside the PDF at each change, so the tab can go at
 * any time; Claude's get_digitized reads the same file.
 */
class Panel : public QWidget, public pdf::PageTool
{
    Q_OBJECT

public:
    /// What the left button does on the pages.
    enum class Mode { None, Region, Reference, AddPoint, MovePoint, DeletePoint, Trace, PickColour, LeaveOut };

    Panel(PdfDoc* document, QWidget* parent = nullptr);
    ~Panel() override;

    // pdf::PageTool
    bool takesMouse() const override { return a_mode != Mode::None; }
    void pressed(int page, QPointF point) override;
    void moved(int page, QPointF point, bool down) override;
    void released(int page, QPointF point) override;
    bool keyPressed(QKeyEvent* event) override;
    bool wantsKey(QKeyEvent* event) const override;
    QImage painted(int page, const QRectF& visible, qreal scale, QPointF* at) override;

    void opened();
    /// The file was read again: the same plots over what it now shows.
    void documentChanged();
    /// What is kept with the file, read again.
    void reloadKept();
    void selectPlot(int index);
    /// \a plot shown, not kept until it is changed.
    void addTransient(const Plot& plot);
    /// Insets and legends' boxes found and left out at each extraction.
    bool findsLeftOut() const;
    void setFindsLeftOut(bool on);
    /// The current plot's regions left out, by hand and found, all taken back.
    void clearLeftOut();

    Mode mode() const { return a_mode; }
    void setMode(Mode mode);
    const QList<Plot>& plots() const { return a_plots; }
    int currentPlot() const { return a_current; }
    int currentSeries() const { return a_series; }
    void setCurrentSeries(int index);
    bool canUndo() const { return !a_undo.isEmpty(); }
    void undo();

    /// The plots on the page in view, found (none yet of them kept are kept).
    int findOnPage();
    /// The series of the current plot taken anew.
    bool extractCurrent();
    /// Calibrated from what its axes say.
    bool calibrateCurrent();
    /// The current plot's numbers, as Copy gives them and CSV writes them.
    QString currentCsv() const;
    /// Written to a file (the two buttons' second half, for the tests).
    bool writeCsv(const QString& file);
    bool writeDataset(const QString& file);
    /// The reference being added takes these values, by the table's own cells.
    void setReferenceValue(int row, bool x, double value);
    void setScales(Scale x, Scale y);
    void setKind(Kind kind, double rAtCentre = 0);
    QString status() const;
    bool magnifierOn() const;

    // For the tests.
    QPushButton* findButton() const { return a_findButton; }
    QPushButton* extractButton() const { return a_extractButton; }
    QTableWidget* referenceTable() const { return a_refTable; }
    QTableWidget* seriesTable() const { return a_seriesTable; }
    QTableWidget* pointsTable() const { return a_pointsTable; }
    QComboBox* plotBox() const { return a_plotBox; }
    QToolButton* modeButton(Mode mode) const;
    QCheckBox* magnifierBox() const { return a_magnifier; }
    /// The file's own reading of the document, for what tests look at.
    Source* source();

private:
    struct State {
        QList<Plot> plots;
        int current = -1;
        int series = -1;
    };

    void buildUi();
    void remember();
    void changed(bool save = true);
    void refreshAll();
    void refreshPlots();
    void refreshCalibration();
    void refreshSeries();
    void refreshPoints();
    void refreshTrust();
    void saveNow();
    void loadKept();
    Plot* plot();
    const Plot* plot() const;
    /// The point of the current plot nearest \a point on \a page, within reach.
    bool nearestPoint(int page, QPointF point, int* series, int* index) const;
    void addReference(QPointF point);
    /// A plot with no calibration gets four references at its frame's corners,
    /// their values to be typed: the ends of its axes.
    void prefillReferences(Plot& plot);
    Options extractOptions() const;
    void refit();
    void updateView();
    void copyToClipboard();

    PdfDoc* a_doc;
    std::unique_ptr<Source> a_source;
    QList<Plot> a_plots;
    int a_current = -1;
    int a_series = -1;
    int a_selectedPoint = -1;
    QList<State> a_undo;
    Mode a_mode = Mode::None;
    bool a_problemKept = false;     // what is kept could not be read: not overwritten
    QString a_problem;
    bool a_refreshing = false;

    // The pointer, for the magnifier and a drag.
    int a_pointerPage = -1;
    QPointF a_pointer;
    bool a_down = false;
    QPointF a_pressedAt;
    int a_pressedPage = -1;
    int a_dragSeries = -1, a_dragPoint = -1;
    int a_revision = 0;

    // The overlay, kept while nothing has changed.
    struct Cached {
        int page = -1;
        int revision = -1;
        qreal scale = 0;
        QRectF area;
        QImage image;
    };
    Cached a_cached;

    QComboBox* a_plotBox = nullptr;
    QPushButton* a_findButton = nullptr;
    QToolButton* a_deletePlot = nullptr;
    QButtonGroup* a_modes = nullptr;
    QCheckBox* a_magnifier = nullptr;
    QCheckBox* a_findLeftOut = nullptr;
    QComboBox* a_kind = nullptr;
    QComboBox* a_xScale = nullptr;
    QComboBox* a_yScale = nullptr;
    QComboBox* a_model = nullptr;
    QTableWidget* a_refTable = nullptr;
    QPushButton* a_fromAxes = nullptr;
    QPushButton* a_removeRef = nullptr;
    QLabel* a_calStatus = nullptr;
    QComboBox* a_route = nullptr;
    QPushButton* a_extractButton = nullptr;
    QPushButton* a_newSeries = nullptr;
    QPushButton* a_removeSeries = nullptr;
    QTableWidget* a_seriesTable = nullptr;
    QTableWidget* a_pointsTable = nullptr;
    QLabel* a_trust = nullptr;
    QPushButton* a_undoButton = nullptr;
    QLabel* a_message = nullptr;
};

} // namespace qucs_s::digitizer

#endif // QUCS_PLOTDIGITIZERPANEL_H
