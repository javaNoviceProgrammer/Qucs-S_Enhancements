/*
 * plotdigitizerpanel.cpp - the PDF viewer's plot digitizer (plotdigitizerpanel.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "plotdigitizerpanel.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPdfDocument>
#include <QPixmap>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace qucs_s::digitizer {

namespace {

constexpr int kMostUndo = 64;
constexpr double kReachPixels = 9;      // how near a click must be to a point
constexpr int kLensPixels = 170;        // the magnifier's side, device pixels
constexpr double kLensFactor = 5;

QString valueText(double v)
{
    return std::isfinite(v) ? QString::number(v, 'g', 7) : QStringLiteral("-");
}

QToolButton* modeTool(QWidget* parent, const QString& text, const QString& tip)
{
    auto* b = new QToolButton(parent);
    b->setText(text);
    b->setToolTip(tip);
    b->setCheckable(true);
    b->setToolButtonStyle(Qt::ToolButtonTextOnly);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return b;
}

QTableWidget* table(QWidget* parent, const QStringList& headers, int height)
{
    auto* t = new QTableWidget(0, int(headers.size()), parent);
    t->setHorizontalHeaderLabels(headers);
    t->verticalHeader()->setVisible(false);
    t->verticalHeader()->setDefaultSectionSize(20);
    t->horizontalHeader()->setStretchLastSection(true);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->setMinimumHeight(height);
    return t;
}

QPixmap swatch(const QColor& colour)
{
    QPixmap p(12, 12);
    p.fill(colour);
    return p;
}

} // namespace

Panel::Panel(PdfDoc* document, QWidget* parent) : QWidget(parent), a_doc(document)
{
    buildUi();
}

Panel::~Panel() = default;

void Panel::buildUi()
{
    setObjectName(QStringLiteral("plotDigitizer"));
    setMinimumWidth(280);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);
    auto* inner = new QWidget(scroll);
    scroll->setWidget(inner);
    auto* layout = new QVBoxLayout(inner);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    // Which plot.
    auto* plots = new QHBoxLayout;
    a_plotBox = new QComboBox(inner);
    a_plotBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    a_plotBox->setMinimumContentsLength(14);
    a_plotBox->setToolTip(tr("The plots digitized in this document"));
    plots->addWidget(a_plotBox, 1);
    a_deletePlot = new QToolButton(inner);
    a_deletePlot->setText(tr("Remove"));
    a_deletePlot->setToolTip(tr("Removes this plot and what was digitized of it"));
    plots->addWidget(a_deletePlot);
    layout->addLayout(plots);
    connect(a_plotBox, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (a_refreshing) return;
        a_current = index;
        a_series = a_current >= 0 && a_current < a_plots.size() && !a_plots.at(a_current).series.isEmpty() ? 0 : -1;
        a_selectedPoint = -1;
        ++a_revision;
        if (const Plot* p = plot()) a_doc->view()->showRegion(p->page, p->region.isNull() ? p->pageBox() : p->region);
        refreshAll();
        updateView();
    });
    connect(a_deletePlot, &QToolButton::clicked, this, [this] {
        if (a_current < 0 || a_current >= a_plots.size()) return;
        remember();
        a_plots.removeAt(a_current);
        a_current = std::min<int>(a_current, int(a_plots.size()) - 1);
        a_series = a_current >= 0 && !a_plots.at(a_current).series.isEmpty() ? 0 : -1;
        changed();
    });

    // The mouse's modes.
    a_findButton = new QPushButton(tr("Find Plots on This Page"), inner);
    a_findButton->setToolTip(tr("Finds the plots on the page in view, calibrates them from their axes and extracts their curves"));
    layout->addWidget(a_findButton);
    connect(a_findButton, &QPushButton::clicked, this, [this] { findOnPage(); });
    a_modes = new QButtonGroup(this);
    a_modes->setExclusive(false);
    auto* modes = new QGridLayout;
    modes->setSpacing(2);
    const struct {
        Mode mode;
        const char* text;
        const char* tip;
    } buttons[] = {
        {Mode::Region, QT_TR_NOOP("Mark Region"), QT_TR_NOOP("Drag over a plot: it is framed, calibrated and extracted")},
        {Mode::Reference, QT_TR_NOOP("Add Reference"), QT_TR_NOOP("Click a point whose value you know, then type it in the table")},
        {Mode::Trace, QT_TR_NOOP("Follow Line"), QT_TR_NOOP("Click on a curve: it is followed both ways")},
        {Mode::PickColour, QT_TR_NOOP("Pick Colour"), QT_TR_NOOP("Click on a curve: everything of its colour is traced from the picture")},
        {Mode::AddPoint, QT_TR_NOOP("Add Point"), QT_TR_NOOP("Each click adds a point to the series selected")},
        {Mode::MovePoint, QT_TR_NOOP("Move Point"), QT_TR_NOOP("Drag a point")},
        {Mode::DeletePoint, QT_TR_NOOP("Delete Point"), QT_TR_NOOP("Click a point to delete it")},
    };
    int cell = 0;
    for (const auto& b : buttons) {
        auto* tool = modeTool(inner, tr(b.text), tr(b.tip));
        a_modes->addButton(tool, int(b.mode));
        modes->addWidget(tool, cell / 2, cell % 2);
        ++cell;
    }
    layout->addLayout(modes);
    connect(a_modes, &QButtonGroup::idToggled, this, [this](int id, bool on) {
        if (a_refreshing) return;
        if (on) setMode(Mode(id));
        else if (int(a_mode) == id) setMode(Mode::None);
    });
    a_magnifier = new QCheckBox(tr("Magnifier by the pointer"), inner);
    a_magnifier->setChecked(true);
    layout->addWidget(a_magnifier);
    connect(a_magnifier, &QCheckBox::toggled, this, [this] {
        ++a_revision;
        updateView();
    });

    // The calibration.
    auto* calibration = new QGroupBox(tr("Calibration"), inner);
    auto* calLayout = new QVBoxLayout(calibration);
    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    a_kind = new QComboBox(calibration);
    a_kind->addItem(tr("Rectangular"));
    a_kind->addItem(tr("Polar (x is r, y the angle in degrees)"));
    form->addRow(tr("Axes"), a_kind);
    a_xScale = new QComboBox(calibration);
    a_xScale->addItems({tr("Linear"), tr("Log")});
    form->addRow(tr("X (or r)"), a_xScale);
    a_yScale = new QComboBox(calibration);
    a_yScale->addItems({tr("Linear"), tr("Log")});
    form->addRow(tr("Y"), a_yScale);
    a_model = new QComboBox(calibration);
    a_model->addItem(tr("Turn and shear (2 to 3 points or more)"));
    a_model->addItem(tr("Perspective (4 points or more)"));
    form->addRow(tr("Page"), a_model);
    calLayout->addLayout(form);
    const auto scaleChanged = [this] {
        if (a_refreshing || plot() == nullptr) return;
        remember();
        Calibration& c = plot()->calibration;
        c.kind = a_kind->currentIndex() == 1 ? Kind::Polar : Kind::Cartesian;
        c.xScale = a_xScale->currentIndex() == 1 ? Scale::Log : Scale::Linear;
        c.yScale = a_yScale->currentIndex() == 1 ? Scale::Log : Scale::Linear;
        c.model = a_model->currentIndex() == 1 ? Model::Projective : Model::Affine;
        plot()->xAxis.scale = c.xScale;
        plot()->yAxis.scale = c.yScale;
        plot()->xAxis.scaleGuessed = plot()->yAxis.scaleGuessed = false;
        refit();
        changed();
    };
    connect(a_kind, &QComboBox::currentIndexChanged, this, scaleChanged);
    connect(a_xScale, &QComboBox::currentIndexChanged, this, scaleChanged);
    connect(a_yScale, &QComboBox::currentIndexChanged, this, scaleChanged);
    connect(a_model, &QComboBox::currentIndexChanged, this, scaleChanged);
    a_refTable = table(calibration, {tr("Page (pt)"), tr("x"), tr("y"), tr("Off (pt)")}, 90);
    calLayout->addWidget(a_refTable);
    connect(a_refTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (a_refreshing || plot() == nullptr || item->row() >= plot()->calibration.references.size()) return;
        if (item->column() != 1 && item->column() != 2) return;
        double value = 0;
        const bool ok = parseTickLabel(item->text().trimmed(), &value);
        remember();
        Reference& r = plot()->calibration.references[item->row()];
        if (item->column() == 1) {
            r.hasX = ok;
            r.x = ok ? value : 0;
        } else {
            r.hasY = ok;
            r.y = ok ? value : 0;
        }
        refit();
        changed();
    });
    auto* calButtons = new QHBoxLayout;
    a_fromAxes = new QPushButton(tr("Read from Axes"), calibration);
    a_fromAxes->setToolTip(tr("The references from the axes' tick marks and labels"));
    a_removeRef = new QPushButton(tr("Remove"), calibration);
    a_removeRef->setToolTip(tr("Removes the reference selected"));
    calButtons->addWidget(a_fromAxes);
    calButtons->addWidget(a_removeRef);
    calLayout->addLayout(calButtons);
    connect(a_fromAxes, &QPushButton::clicked, this, [this] { calibrateCurrent(); });
    connect(a_removeRef, &QPushButton::clicked, this, [this] {
        if (plot() == nullptr) return;
        const int row = a_refTable->currentRow();
        if (row < 0 || row >= plot()->calibration.references.size()) return;
        remember();
        plot()->calibration.references.removeAt(row);
        refit();
        changed();
    });
    a_calStatus = new QLabel(calibration);
    a_calStatus->setWordWrap(true);
    calLayout->addWidget(a_calStatus);
    layout->addWidget(calibration);

    // The series.
    auto* series = new QGroupBox(tr("Series"), inner);
    auto* seriesLayout = new QVBoxLayout(series);
    auto* extractRow = new QHBoxLayout;
    a_extractButton = new QPushButton(tr("Extract"), series);
    a_extractButton->setToolTip(tr("Takes this plot's curves and markers anew, replacing what was extracted"));
    a_route = new QComboBox(series);
    a_route->addItems({tr("Best route"), tr("The file's paths"), tr("The picture")});
    a_route->setToolTip(tr("Where from: the file's own vertices where the plot is vector art, else a trace of a picture of the page"));
    extractRow->addWidget(a_extractButton);
    extractRow->addWidget(a_route, 1);
    seriesLayout->addLayout(extractRow);
    connect(a_extractButton, &QPushButton::clicked, this, [this] { extractCurrent(); });
    a_seriesTable = table(series, {QString(), tr("Name"), tr("What"), tr("Points")}, 90);
    a_seriesTable->setColumnWidth(0, 22);
    seriesLayout->addWidget(a_seriesTable);
    connect(a_seriesTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (a_refreshing || plot() == nullptr || item->column() != 1 || item->row() >= plot()->series.size()) return;
        remember();
        plot()->series[item->row()].name = item->text().left(200);
        changed();
    });
    connect(a_seriesTable, &QTableWidget::itemSelectionChanged, this, [this] {
        if (a_refreshing) return;
        a_series = a_seriesTable->currentRow();
        a_selectedPoint = -1;
        ++a_revision;
        refreshPoints();
        updateView();
    });
    auto* seriesButtons = new QHBoxLayout;
    a_newSeries = new QPushButton(tr("New by Hand"), series);
    a_newSeries->setToolTip(tr("A new series whose points you place with Add Point"));
    a_removeSeries = new QPushButton(tr("Remove"), series);
    a_removeSeries->setToolTip(tr("Removes the series selected"));
    seriesButtons->addWidget(a_newSeries);
    seriesButtons->addWidget(a_removeSeries);
    seriesLayout->addLayout(seriesButtons);
    connect(a_newSeries, &QPushButton::clicked, this, [this] {
        if (plot() == nullptr) {
            a_message->setText(tr("Find or mark a plot first."));
            return;
        }
        remember();
        Series s;
        s.name = tr("By hand %1").arg(plot()->series.size() + 1);
        s.shape = Series::Shape::ByHand;
        s.origin = Series::Origin::Hand;
        s.colour = QColor(200, 0, 160);
        plot()->series << s;
        a_series = int(plot()->series.size()) - 1;
        setMode(Mode::AddPoint);
        changed();
    });
    connect(a_removeSeries, &QPushButton::clicked, this, [this] {
        if (plot() == nullptr || a_series < 0 || a_series >= plot()->series.size()) return;
        remember();
        plot()->series.removeAt(a_series);
        a_series = std::min<int>(a_series, int(plot()->series.size()) - 1);
        changed();
    });
    layout->addWidget(series);

    // Its points.
    auto* points = new QGroupBox(tr("Points of the Series"), inner);
    auto* pointsLayout = new QVBoxLayout(points);
    a_pointsTable = table(points, {tr("x"), tr("y")}, 120);
    a_pointsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    pointsLayout->addWidget(a_pointsTable);
    connect(a_pointsTable, &QTableWidget::itemSelectionChanged, this, [this] {
        if (a_refreshing) return;
        a_selectedPoint = a_pointsTable->currentRow();
        ++a_revision;
        updateView();
    });
    layout->addWidget(points);

    // How far to trust it, and getting it out.
    a_trust = new QLabel(inner);
    a_trust->setWordWrap(true);
    a_trust->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(a_trust);
    auto* out = new QGridLayout;
    auto* copy = new QPushButton(tr("Copy"), inner);
    copy->setToolTip(tr("The numbers on the clipboard, as CSV"));
    auto* csvButton = new QPushButton(tr("CSV..."), inner);
    auto* dataButton = new QPushButton(tr("Dataset..."), inner);
    dataButton->setToolTip(tr("A Qucs dataset (.dat): the curves can be plotted in a diagram beside a simulation's"));
    a_undoButton = new QPushButton(tr("Undo"), inner);
    a_undoButton->setToolTip(tr("Undoes the last change (Ctrl+Z, with the pages in focus)"));
    out->addWidget(copy, 0, 0);
    out->addWidget(csvButton, 0, 1);
    out->addWidget(dataButton, 1, 0);
    out->addWidget(a_undoButton, 1, 1);
    layout->addLayout(out);
    connect(copy, &QPushButton::clicked, this, [this] { copyToClipboard(); });
    connect(csvButton, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getSaveFileName(this, tr("Write the Numbers"), QFileInfo(a_doc->getDocName()).absolutePath(), tr("CSV (*.csv)"));
        if (!file.isEmpty()) writeCsv(file);
    });
    connect(dataButton, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getSaveFileName(this, tr("Write a Dataset"), QFileInfo(a_doc->getDocName()).absolutePath(), tr("Qucs dataset (*.dat)"));
        if (!file.isEmpty()) writeDataset(file);
    });
    connect(a_undoButton, &QPushButton::clicked, this, &Panel::undo);
    a_message = new QLabel(inner);
    a_message->setWordWrap(true);
    layout->addWidget(a_message);
    layout->addStretch(1);
}

QToolButton* Panel::modeButton(Mode mode) const { return qobject_cast<QToolButton*>(a_modes->button(int(mode))); }
bool Panel::magnifierOn() const { return a_magnifier->isChecked(); }
QString Panel::status() const { return a_message->text(); }

Source* Panel::source()
{
    if (a_source == nullptr && a_doc->document() != nullptr) a_source = std::make_unique<Source>(a_doc->document(), a_doc->getDocName());
    return a_source.get();
}

Plot* Panel::plot() { return a_current >= 0 && a_current < a_plots.size() ? &a_plots[a_current] : nullptr; }
const Plot* Panel::plot() const { return a_current >= 0 && a_current < a_plots.size() ? &a_plots.at(a_current) : nullptr; }

void Panel::opened()
{
    loadKept();
    refreshAll();
    updateView();
}

void Panel::documentChanged()
{
    a_source.reset();
    ++a_revision;
    a_cached = Cached{};
    updateView();
}

void Panel::reloadKept()
{
    loadKept();
    refreshAll();
    updateView();
}

void Panel::loadKept()
{
    QString why;
    QList<Plot> kept;
    bool stale = false;
    a_problemKept = false;
    if (!loadSidecar(a_doc->getDocName(), &kept, &why, &stale)) {
        // It is there and is not ours to overwrite: said, and left.
        a_problemKept = true;
        a_problem = tr("What is kept with this PDF could not be read, and is left as it is: %1").arg(why);
        a_message->setText(a_problem);
        return;
    }
    a_plots = kept;
    a_undo.clear();
    a_current = a_plots.isEmpty() ? -1 : std::clamp(a_current, 0, int(a_plots.size()) - 1);
    a_series = a_current >= 0 && !a_plots.at(a_current).series.isEmpty() ? std::clamp(a_series, 0, int(a_plots.at(a_current).series.size()) - 1) : -1;
    ++a_revision;
    a_message->setText(stale ? tr("The PDF has changed since these were kept: its pages may no longer be where they were.") : QString());
}

void Panel::selectPlot(int index)
{
    if (index < 0 || index >= a_plots.size()) return;
    a_current = index;
    a_series = a_plots.at(index).series.isEmpty() ? -1 : 0;
    a_selectedPoint = -1;
    ++a_revision;
    refreshAll();
    const Plot& p = a_plots.at(index);
    a_doc->view()->showRegion(p.page, p.region.isNull() ? p.pageBox() : p.region);
    updateView();
}

void Panel::setCurrentSeries(int index)
{
    if (plot() == nullptr || index < 0 || index >= plot()->series.size()) return;
    a_series = index;
    a_selectedPoint = -1;
    ++a_revision;
    refreshAll();
    updateView();
}

void Panel::setMode(Mode mode)
{
    a_mode = mode;
    a_refreshing = true;
    for (QAbstractButton* b : a_modes->buttons()) b->setChecked(a_modes->id(b) == int(mode));
    a_refreshing = false;
    a_doc->view()->setFocus(Qt::OtherFocusReason);
    ++a_revision;
    updateView();
}

void Panel::remember()
{
    a_undo.append(State{a_plots, a_current, a_series});
    if (a_undo.size() > kMostUndo) a_undo.removeFirst();
}

void Panel::undo()
{
    if (a_undo.isEmpty()) return;
    const State s = a_undo.takeLast();
    a_plots = s.plots;
    a_current = s.current;
    a_series = s.series;
    a_selectedPoint = -1;
    changed();
}

void Panel::saveNow()
{
    if (a_problemKept) return;
    QString why;
    if (!saveSidecar(a_doc->getDocName(), a_plots, &why)) a_message->setText(why);
}

void Panel::changed(bool save)
{
    ++a_revision;
    if (save) saveNow();
    refreshAll();
    updateView();
}

void Panel::updateView() { a_doc->view()->viewport()->update(); }

void Panel::refreshAll()
{
    refreshPlots();
    refreshCalibration();
    refreshSeries();
    refreshPoints();
    refreshTrust();
    a_undoButton->setEnabled(!a_undo.isEmpty());
}

void Panel::refreshPlots()
{
    a_refreshing = true;
    a_plotBox->clear();
    for (int i = 0; i < a_plots.size(); ++i) {
        const Plot& p = a_plots.at(i);
        a_plotBox->addItem(p.name.isEmpty() ? tr("Plot on page %1").arg(p.page + 1) : p.name);
    }
    a_plotBox->setCurrentIndex(a_current);
    a_deletePlot->setEnabled(a_current >= 0);
    a_refreshing = false;
}

void Panel::refreshCalibration()
{
    a_refreshing = true;
    const Plot* p = plot();
    const Calibration c = p != nullptr ? p->calibration : Calibration{};
    a_kind->setCurrentIndex(c.kind == Kind::Polar ? 1 : 0);
    a_xScale->setCurrentIndex(c.xScale == Scale::Log ? 1 : 0);
    a_yScale->setCurrentIndex(c.yScale == Scale::Log ? 1 : 0);
    a_model->setCurrentIndex(c.model == Model::Projective ? 1 : 0);
    a_refTable->setRowCount(int(c.references.size()));
    for (int i = 0; i < c.references.size(); ++i) {
        const Reference& r = c.references.at(i);
        const auto cell = [&](int column, const QString& text, bool editable) {
            auto* item = new QTableWidgetItem(text);
            if (!editable) item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            a_refTable->setItem(i, column, item);
        };
        cell(0, QStringLiteral("%1, %2").arg(r.at.x(), 0, 'f', 1).arg(r.at.y(), 0, 'f', 1), false);
        cell(1, r.hasX ? valueText(r.x) : QString(), true);
        cell(2, r.hasY ? valueText(r.y) : QString(), true);
        cell(3, c.valid() && i < c.residuals().size() ? QString::number(c.residuals().at(i), 'f', 2) : QString(), false);
    }
    if (p == nullptr) a_calStatus->setText(tr("Find a plot on the page, or mark one."));
    else if (c.valid()) a_calStatus->setText(tr("Fitted to %1 references: %2 points off on average, %3 at most.").arg(c.references.size()).arg(c.rms(), 0, 'f', 2).arg(c.worst(), 0, 'f', 2));
    else {
        QString why;
        Calibration attempt = c;
        attempt.fit(&why);
        a_calStatus->setText(c.references.isEmpty() ? tr("No references yet: Read from Axes, or Add Reference.") : why);
    }
    a_refreshing = false;
}

void Panel::refreshSeries()
{
    a_refreshing = true;
    const Plot* p = plot();
    const int rows = p != nullptr ? int(p->series.size()) : 0;
    a_seriesTable->setRowCount(rows);
    for (int i = 0; i < rows; ++i) {
        const Series& s = p->series.at(i);
        auto* colour = new QTableWidgetItem;
        colour->setIcon(QIcon(swatch(s.colour)));
        colour->setFlags(colour->flags() & ~Qt::ItemIsEditable);
        a_seriesTable->setItem(i, 0, colour);
        a_seriesTable->setItem(i, 1, new QTableWidgetItem(s.name));
        QString what;
        switch (s.shape) {
        case Series::Shape::Markers: what = tr("markers"); break;
        case Series::Shape::Area: what = tr("area"); break;
        case Series::Shape::Bars: what = tr("bars"); break;
        case Series::Shape::ByHand: what = tr("by hand"); break;
        default: what = s.dashed ? tr("dashed") : tr("line"); break;
        }
        auto* whatItem = new QTableWidgetItem(what);
        whatItem->setFlags(whatItem->flags() & ~Qt::ItemIsEditable);
        whatItem->setToolTip(s.origin == Series::Origin::Vector ? tr("The file's own vertices") : (s.origin == Series::Origin::Raster ? tr("Traced from the picture") : tr("Placed by hand")));
        a_seriesTable->setItem(i, 2, whatItem);
        auto* count = new QTableWidgetItem(QString::number(s.points.size()));
        count->setFlags(count->flags() & ~Qt::ItemIsEditable);
        a_seriesTable->setItem(i, 3, count);
    }
    if (a_series >= rows) a_series = rows - 1;
    a_seriesTable->setCurrentCell(a_series, 1);
    a_removeSeries->setEnabled(a_series >= 0);
    a_refreshing = false;
}

void Panel::refreshPoints()
{
    a_refreshing = true;
    const Plot* p = plot();
    const Series* s = p != nullptr && a_series >= 0 && a_series < p->series.size() ? &p->series.at(a_series) : nullptr;
    // (Not by the hundred thousand: the numbers are in the CSV.)
    constexpr int kMostRows = 5000;
    const int rows = s != nullptr ? std::min<int>(int(s->points.size()), kMostRows) : 0;
    a_pointsTable->setRowCount(rows);
    a_pointsTable->setHorizontalHeaderLabels(p != nullptr && p->calibration.valid() ? QStringList{tr("x"), tr("y")} : QStringList{tr("Page x (pt)"), tr("Page y (pt)")});
    for (int i = 0; i < rows; ++i) {
        const QPointF v = p->calibration.valid() ? p->calibration.toValue(s->points.at(i)) : s->points.at(i);
        a_pointsTable->setItem(i, 0, new QTableWidgetItem(valueText(v.x())));
        a_pointsTable->setItem(i, 1, new QTableWidgetItem(valueText(v.y())));
    }
    a_refreshing = false;
}

void Panel::refreshTrust()
{
    const Plot* p = plot();
    if (p == nullptr) {
        a_trust->clear();
        return;
    }
    const Trust t = assess(*p);
    QString text = tr("<b>Trust: %1.</b>").arg(t.level.toHtmlEscaped());
    if (p->calibration.valid()) {
        const bool xLog = p->calibration.xScale == Scale::Log && p->calibration.kind == Kind::Cartesian;
        const bool yLog = p->calibration.yScale == Scale::Log && p->calibration.kind == Kind::Cartesian;
        text += QLatin1Char(' ') + tr("A point of the page is %1 in x, %2 in y; each value is good to about %3 points.")
                                       .arg(valueText(t.unitsPerPoint.x()) + (xLog ? tr(" decades") : QString()), valueText(t.unitsPerPoint.y()) + (yLog ? tr(" decades") : QString()))
                                       .arg(t.totalPoints, 0, 'f', 2);
    }
    for (const QString& reason : t.reasons) text += QStringLiteral("<br>") + reason.toHtmlEscaped();
    for (const QString& note : p->notes) text += QStringLiteral("<br>") + note.toHtmlEscaped();
    a_trust->setText(text);
}

void Panel::refit()
{
    Plot* p = plot();
    if (p == nullptr) return;
    QString why;
    p->calibration.fit(&why);
}

bool Panel::calibrateCurrent()
{
    Plot* p = plot();
    if (p == nullptr) return false;
    if (!p->xAxis.found || !p->yAxis.found) {
        // Its axes were not read this session (kept plots keep the references,
        // not the axes): read now.
        Source* s = source();
        if (s == nullptr) return false;
        Plot read = plotInRegion(*s, p->page, p->region.isNull() ? p->pageBox() : p->region);
        if (read.frame.isNull()) {
            a_message->setText(tr("No frame was found here: add references by hand."));
            return false;
        }
        if (!read.xAxis.found || !read.yAxis.found) {
            a_message->setText(tr("The axes' labels could not be read: add references by hand."));
            return false;
        }
        p->frame = read.frame;
        p->work = read.work;
        p->angle = read.angle;
        p->xAxis = read.xAxis;
        p->yAxis = read.yAxis;
    }
    remember();
    QString why;
    const bool fine = calibrateFromAxes(*p, &why);
    if (!fine) a_message->setText(why);
    changed();
    return fine;
}

int Panel::findOnPage()
{
    Source* s = source();
    if (s == nullptr) return 0;
    const int page = a_doc->view()->currentPage();
    QList<Plot> found = findPlots(*s, page);
    if (found.isEmpty()) {
        a_message->setText(tr("No plot with a frame and axes was found on page %1. Mark Region takes one by hand.").arg(page + 1));
        return 0;
    }
    remember();
    int added = 0;
    for (Plot& p : found) {
        // One already here keeps what was done to it.
        bool have = false;
        for (const Plot& o : std::as_const(a_plots))
            have = have || (o.page == p.page && QLineF(o.pageBox().center(), p.pageBox().center()).length() < 5);
        if (have) continue;
        QString why;
        extract(*s, p, Options{}, &why);
        a_plots << p;
        ++added;
    }
    if (added > 0) {
        a_current = int(a_plots.size()) - added;
        a_series = a_plots.at(a_current).series.isEmpty() ? -1 : 0;
    }
    a_message->setText(added > 0 ? tr("%1 plots found on page %2.").arg(added).arg(page + 1) : tr("The plots on page %1 are here already.").arg(page + 1));
    changed();
    return added;
}

bool Panel::extractCurrent()
{
    Plot* p = plot();
    Source* s = source();
    if (p == nullptr || s == nullptr) {
        a_message->setText(tr("Find or mark a plot first."));
        return false;
    }
    remember();
    Options o;
    o.route = a_route->currentIndex() == 1 ? Options::Route::Vector : (a_route->currentIndex() == 2 ? Options::Route::Raster : Options::Route::Auto);
    QString why;
    const QList<Series> hand = [&] {
        QList<Series> kept;
        for (const Series& series : std::as_const(p->series))
            if (series.origin == Series::Origin::Hand) kept << series;
        return kept;
    }();
    p->notes.clear();
    const bool fine = extract(*s, *p, o, &why);
    p->series << hand;
    if (!fine) a_message->setText(why);
    else a_message->setText(tr("%1 series, from %2.").arg(p->series.size()).arg(p->route == QLatin1String("vector") ? tr("the file's own paths") : tr("a picture of the page")));
    a_series = p->series.isEmpty() ? -1 : 0;
    changed();
    return fine;
}

void Panel::setReferenceValue(int row, bool x, double value)
{
    Plot* p = plot();
    if (p == nullptr || row < 0 || row >= p->calibration.references.size()) return;
    remember();
    Reference& r = p->calibration.references[row];
    (x ? r.hasX : r.hasY) = true;
    (x ? r.x : r.y) = value;
    refit();
    changed();
}

void Panel::setScales(Scale x, Scale y)
{
    Plot* p = plot();
    if (p == nullptr) return;
    remember();
    p->calibration.xScale = x;
    p->calibration.yScale = y;
    refit();
    changed();
}

void Panel::setKind(Kind kind, double rAtCentre)
{
    Plot* p = plot();
    if (p == nullptr) return;
    remember();
    p->calibration.kind = kind;
    p->calibration.rAtCentre = rAtCentre;
    refit();
    changed();
}

void Panel::addReference(QPointF point)
{
    Plot* p = plot();
    if (p == nullptr) return;
    remember();
    Reference r;
    r.at = point;
    p->calibration.references << r;
    refit();
    changed();
    // On to typing its value.
    const int row = int(p->calibration.references.size()) - 1;
    a_refTable->setCurrentCell(row, 1);
    a_refTable->editItem(a_refTable->item(row, 1));
}

bool Panel::nearestPoint(int page, QPointF point, int* series, int* index) const
{
    const Plot* p = plot();
    if (p == nullptr || p->page != page) return false;
    const double ppp = a_doc->view()->pixelsPerPoint();
    double best = kReachPixels / std::max(0.05, ppp);
    bool found = false;
    const auto look = [&](int s) {
        const QList<QPointF>& points = p->series.at(s).points;
        for (int i = 0; i < points.size(); ++i) {
            const double d = std::hypot(points.at(i).x() - point.x(), points.at(i).y() - point.y());
            if (d < best) {
                best = d;
                *series = s;
                *index = i;
                found = true;
            }
        }
    };
    // The series selected first: among curves that cross, it is the one meant.
    if (a_series >= 0 && a_series < p->series.size()) look(a_series);
    if (found) return true;
    for (int s = 0; s < p->series.size(); ++s) look(s);
    return found;
}

void Panel::pressed(int page, QPointF point)
{
    a_down = true;
    a_pressedAt = point;
    a_pressedPage = page;
    a_pointerPage = page;
    a_pointer = point;
    switch (a_mode) {
    case Mode::MovePoint:
        if (nearestPoint(page, point, &a_dragSeries, &a_dragPoint)) {
            remember();
            a_series = a_dragSeries;
            a_selectedPoint = a_dragPoint;
            refreshSeries();
            refreshPoints();
            a_pointsTable->setCurrentCell(a_dragPoint, 0);
            ++a_revision;
            updateView();
        } else a_dragSeries = a_dragPoint = -1;
        break;
    default: break;
    }
}

void Panel::moved(int page, QPointF point, bool down)
{
    a_pointerPage = page;
    a_pointer = point;
    if (down && a_mode == Mode::MovePoint && a_dragSeries >= 0 && plot() != nullptr && page == plot()->page && a_dragSeries < plot()->series.size() && a_dragPoint < plot()->series.at(a_dragSeries).points.size()) {
        plot()->series[a_dragSeries].points[a_dragPoint] = point;
        ++a_revision;
    } else if (down && a_mode == Mode::Region) {
        ++a_revision;
    }
    updateView();
}

void Panel::released(int page, QPointF point)
{
    a_down = false;
    const QPointF from = a_pressedAt;
    switch (a_mode) {
    case Mode::Region: {
        Source* s = source();
        if (s == nullptr || page != a_pressedPage || page < 0) break;
        const QRectF region = QRectF(from, point).normalized();
        if (region.width() < 8 || region.height() < 8) break;
        Plot made = plotInRegion(*s, page, region);
        remember();
        QString why;
        extract(*s, made, Options{}, &why);
        if (made.name.isEmpty()) made.name = tr("Plot on page %1").arg(page + 1);
        a_plots << made;
        a_current = int(a_plots.size()) - 1;
        a_series = made.series.isEmpty() ? -1 : 0;
        a_message->setText(made.calibration.valid() ? tr("Framed and calibrated from its axes.") : tr("No axes could be read here: add references by hand."));
        changed();
        setMode(Mode::None);
        return;
    }
    case Mode::Reference:
        if (page >= 0 && plot() != nullptr && page == plot()->page) addReference(point);
        else if (page >= 0) {
            // No plot on this page yet: the page's region is the place to start.
            Plot made;
            made.page = page;
            made.region = QRectF(QPointF(0, 0), a_doc->document()->pagePointSize(page));
            made.route = QStringLiteral("hand");
            made.name = tr("Plot on page %1").arg(page + 1);
            remember();
            a_plots << made;
            a_current = int(a_plots.size()) - 1;
            a_series = -1;
            addReference(point);
        }
        break;
    case Mode::AddPoint: {
        if (page < 0) break;
        if (plot() == nullptr || plot()->page != page) {
            a_message->setText(tr("Find or mark a plot on this page first."));
            break;
        }
        remember();
        Plot* p = plot();
        if (a_series < 0 || a_series >= p->series.size()) {
            Series s;
            s.name = tr("By hand %1").arg(p->series.size() + 1);
            s.shape = Series::Shape::ByHand;
            s.origin = Series::Origin::Hand;
            s.colour = QColor(200, 0, 160);
            p->series << s;
            a_series = int(p->series.size()) - 1;
        }
        p->series[a_series].points << point;
        changed();
        break;
    }
    case Mode::MovePoint:
        if (a_dragSeries >= 0) {
            a_dragSeries = a_dragPoint = -1;
            changed();
        }
        break;
    case Mode::DeletePoint: {
        int s = -1, i = -1;
        if (nearestPoint(page, point, &s, &i)) {
            remember();
            plot()->series[s].points.removeAt(i);
            // Its pieces stay where they were.
            QList<int>& breaks = plot()->series[s].breaks;
            for (int k = int(breaks.size()) - 1; k >= 0; --k) {
                if (breaks[k] > i) --breaks[k];
                if (breaks[k] <= 0 || breaks[k] >= plot()->series[s].points.size()) breaks.removeAt(k);
            }
            a_series = s;
            a_selectedPoint = -1;
            changed();
        }
        break;
    }
    case Mode::Trace: {
        Source* s = source();
        Plot* p = plot();
        if (s == nullptr || p == nullptr || p->page != page) {
            a_message->setText(tr("Find or mark a plot on this page first."));
            break;
        }
        Series traced;
        QString why;
        Plot copy = *p;
        if (!traceFrom(*s, copy, point, &traced, &why)) {
            a_message->setText(why);
            break;
        }
        remember();
        p = plot();
        traced.name = tr("Traced %1").arg(p->series.size() + 1);
        p->renderScale = copy.renderScale;
        p->series << traced;
        a_series = int(p->series.size()) - 1;
        changed();
        break;
    }
    case Mode::PickColour: {
        Source* s = source();
        Plot* p = plot();
        if (s == nullptr || p == nullptr || p->page != page) {
            a_message->setText(tr("Find or mark a plot on this page first."));
            break;
        }
        // The colour under the pointer: the strongest pixel near it - of a
        // colour, if any is there, a frame's black line being as near.
        const double reach = std::max(2.0, 4 / std::max(0.05, a_doc->view()->pixelsPerPoint()));
        const QRectF around(point - QPointF(reach, reach), QSizeF(2 * reach, 2 * reach));
        const QImage image = s->render(page, around, 6);
        QColor colour;
        int strongest = 40;
        int bestChroma = 50;
        QColor colourful;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const QRgb v = image.pixel(x, y);
                const int low = std::min({qRed(v), qGreen(v), qBlue(v)}), high = std::max({qRed(v), qGreen(v), qBlue(v)});
                if (high - low > bestChroma) {
                    bestChroma = high - low;
                    colourful = QColor(v);
                }
                if (255 - low > strongest) {
                    strongest = 255 - low;
                    colour = QColor(v);
                }
            }
        if (colourful.isValid()) colour = colourful;
        if (!colour.isValid()) {
            a_message->setText(tr("There is no colour there."));
            break;
        }
        remember();
        p = plot();
        Options o;
        o.route = Options::Route::Raster;
        o.colours << colour;
        Plot copy = *p;
        QString why;
        if (!extract(*s, copy, o, &why)) {
            a_undo.removeLast();
            a_message->setText(why);
            break;
        }
        for (Series& one : copy.series) {
            one.name = tr("%1 %2").arg(colour.name()).arg(p->series.size() + 1);
            p->series << one;
        }
        p->renderScale = copy.renderScale;
        a_series = int(p->series.size()) - 1;
        changed();
        break;
    }
    default: break;
    }
    updateView();
}

bool Panel::wantsKey(QKeyEvent* event) const
{
    if (event->key() == Qt::Key_Escape && a_mode != Mode::None) return true;
    if (event->matches(QKeySequence::Undo) && !a_undo.isEmpty()) return true;
    return false;
}

bool Panel::keyPressed(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && a_mode != Mode::None) {
        setMode(Mode::None);
        return true;
    }
    if (event->matches(QKeySequence::Undo) && !a_undo.isEmpty()) {
        undo();
        return true;
    }
    return false;
}

QImage Panel::painted(int page, const QRectF& visible, qreal scale, QPointF* at)
{
    // Everything on this page, in the part of it in sight.
    QRectF area;
    for (const Plot& p : std::as_const(a_plots))
        if (p.page == page) area = area.isNull() ? p.pageBox().adjusted(-12, -12, 12, 12) : area.united(p.pageBox().adjusted(-12, -12, 12, 12));
    const bool drawingRegion = a_mode == Mode::Region && a_down && a_pressedPage == page;
    const bool lens = a_magnifier->isChecked() && a_mode != Mode::None && a_pointerPage == page;
    if (drawingRegion || lens) {
        // (The drag, and the lens, go where the pointer goes.)
        area = area.isNull() ? visible : area.united(visible);
    }
    area = area.intersected(visible);
    if (area.isEmpty()) return {};
    // (Whole pixels, so that it lands as the page does.)
    const QPointF corner(std::floor(area.left() * scale) / scale, std::floor(area.top() * scale) / scale);
    const QSize size(int(std::ceil((area.right() - corner.x()) * scale)) + 1, int(std::ceil((area.bottom() - corner.y()) * scale)) + 1);
    if (size.isEmpty() || qint64(size.width()) * size.height() > 40000000) return {};
    *at = corner;
    QPointF lensAt = a_pointer;
    int lensRevision = a_revision;
    if (lens) lensRevision += int(std::round(lensAt.x() * 8)) * 16381 + int(std::round(lensAt.y() * 8)) * 8191;
    if (!drawingRegion && a_cached.page == page && a_cached.revision == lensRevision && a_cached.scale == scale && a_cached.area == QRectF(corner, QSizeF(size))) return a_cached.image;
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(scale, scale);
    painter.translate(-corner);
    for (int i = 0; i < a_plots.size(); ++i) {
        const Plot& p = a_plots.at(i);
        if (p.page != page) continue;
        paintMarks(painter, p, scale, i == a_current ? a_series : -1, i == a_current ? a_selectedPoint : -1);
    }
    if (drawingRegion) {
        painter.setPen(QPen(QColor(0, 120, 255), 1.5 / scale, Qt::DashLine));
        painter.setBrush(QColor(0, 120, 255, 30));
        painter.drawRect(QRectF(a_pressedAt, a_pointer).normalized());
    }
    if (lens) {
        // The page under the pointer, enlarged, with the marks, off to its side.
        Source* s = source();
        if (s != nullptr) {
            const double lensScale = scale * kLensFactor;
            const double half = kLensPixels / 2.0 / lensScale;
            const QRectF seen(lensAt - QPointF(half, half), QSizeF(2 * half, 2 * half));
            QImage piece = s->render(page, seen, lensScale);
            if (!piece.isNull()) {
                // Marks over it, in the same way.
                piece = piece.convertToFormat(QImage::Format_ARGB32_Premultiplied);
                QPainter on(&piece);
                on.setRenderHint(QPainter::Antialiasing);
                on.scale(lensScale, lensScale);
                on.translate(-QPointF(std::floor(seen.left() * lensScale) / lensScale, std::floor(seen.top() * lensScale) / lensScale));
                for (int i = 0; i < a_plots.size(); ++i)
                    if (a_plots.at(i).page == page) paintMarks(on, a_plots.at(i), lensScale, i == a_current ? a_series : -1, i == a_current ? a_selectedPoint : -1);
                on.end();
                const double sidePoints = kLensPixels / scale;
                QPointF where = lensAt + QPointF(14 / scale * 3, -sidePoints - 14 / scale * 3);
                if (where.y() < visible.top()) where.setY(lensAt.y() + 40 / scale);
                if (where.x() + sidePoints > visible.right()) where.setX(lensAt.x() - sidePoints - 40 / scale);
                const QRectF box(where, QSizeF(sidePoints, sidePoints));
                painter.drawImage(box, piece);
                painter.setBrush(Qt::NoBrush);
                painter.setPen(QPen(QColor(40, 40, 40), 1.5 / scale));
                painter.drawRect(box);
                // Where the pointer is in it.
                painter.setPen(QPen(QColor(255, 0, 0, 200), 1 / scale));
                const QPointF middle = box.center();
                const double arm = 14 / scale;
                painter.drawLine(middle - QPointF(arm, 0), middle - QPointF(3 / scale, 0));
                painter.drawLine(middle + QPointF(3 / scale, 0), middle + QPointF(arm, 0));
                painter.drawLine(middle - QPointF(0, arm), middle - QPointF(0, 3 / scale));
                painter.drawLine(middle + QPointF(0, 3 / scale), middle + QPointF(0, arm));
                // What the point is worth, under it.
                if (const Plot* p = plot(); p != nullptr && p->page == page && p->calibration.valid()) {
                    const QPointF v = p->calibration.toValue(lensAt);
                    painter.resetTransform();
                    painter.setPen(Qt::white);
                    QRectF text((box.bottomLeft() - corner) * scale, QSizeF(kLensPixels, 20));
                    painter.fillRect(text, QColor(30, 30, 30, 200));
                    painter.drawText(text.adjusted(4, 0, -2, 0), Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("%1, %2").arg(valueText(v.x()), valueText(v.y())));
                }
            }
        }
    }
    painter.end();
    if (!drawingRegion) {
        a_cached.page = page;
        a_cached.revision = lensRevision;
        a_cached.scale = scale;
        a_cached.area = QRectF(corner, QSizeF(size));
        a_cached.image = image;
    }
    return image;
}

QString Panel::currentCsv() const
{
    const Plot* p = plot();
    return p != nullptr ? csv(*p) : QString();
}

void Panel::copyToClipboard()
{
    const QString text = currentCsv();
    if (text.isEmpty()) {
        a_message->setText(tr("Nothing to copy yet."));
        return;
    }
    QApplication::clipboard()->setText(text);
    a_message->setText(tr("The numbers are on the clipboard."));
}

bool Panel::writeCsv(const QString& file)
{
    const Plot* p = plot();
    if (p == nullptr) return false;
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly) || out.write(csv(*p).toUtf8()) < 0 || !out.commit()) {
        a_message->setText(tr("%1 could not be written: %2").arg(file, out.errorString()));
        return false;
    }
    a_message->setText(tr("Written to %1.").arg(QFileInfo(file).fileName()));
    return true;
}

bool Panel::writeDataset(const QString& file)
{
    const Plot* p = plot();
    if (p == nullptr) return false;
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly) || out.write(dataset(*p)) < 0 || !out.commit()) {
        a_message->setText(tr("%1 could not be written: %2").arg(file, out.errorString()));
        return false;
    }
    a_message->setText(tr("Written to %1: plot it in a diagram by its variables.").arg(QFileInfo(file).fileName()));
    return true;
}

} // namespace qucs_s::digitizer
