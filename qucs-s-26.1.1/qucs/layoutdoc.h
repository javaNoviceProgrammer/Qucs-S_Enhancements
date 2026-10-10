/*
 * layoutdoc.h - a GDSII or OASIS layout in a tab of Qucs-S: its cells and
 *               layers beside it, panned and zoomed, measured and searched
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_LAYOUTDOC_H
#define QUCS_LAYOUTDOC_H

#include "layout.h"
#include "qucsdoc.h"

#include <QFrame>
#include <QImage>
#include <QLineF>
#include <QPointer>
#include <QWidget>

#include <functional>
#include <optional>

class QCheckBox;
class QFileSystemWatcher;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QSplitter;
class QStackedWidget;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace qucs_s::layout {

/*!
 * A cell of a layout drawn as a layout viewer draws it: each layer in its
 * colour and fill, the cells it places down to a depth and deeper ones as
 * their frames, y upwards. Zoomed about the pointer (the wheel, a pinch,
 * a box dragged with the right button), panned with a drag or the arrow
 * keys. A click selects the shape under the pointer (again: the one under
 * it); in ruler mode a drag measures, snapped to the shapes' corners and
 * edges. Behind the layout a grid, when it is shown; the layers' colours
 * made to stand out from the canvas, light or dark.
 */
class LayoutView : public QWidget
{
    Q_OBJECT

public:
    explicit LayoutView(QWidget* parent = nullptr);

    void setLayout(std::shared_ptr<const Layout> layout);
    std::shared_ptr<const Layout> layout() const { return a_layout; }
    void setStyles(const QVector<LayerStyle>& styles);
    const QVector<LayerStyle>& styles() const { return a_styles; }

    /// The cell shown.
    int cell() const { return a_cell; }
    void setCell(int cell);
    /// Levels of the cell's hierarchy drawn; deeper cells as frames.
    int depth() const { return a_depth; }
    void setDepth(int depth);
    bool labelsShown() const { return a_labels; }
    void setLabelsShown(bool shown);

    /// The canvas's colours: the application's theme's, or light or dark
    /// whatever its theme.
    enum class Theme { Application, Light, Dark };
    Theme theme() const { return a_theme; }
    void setTheme(Theme theme);
    /// What the canvas is drawn in: its background, the texts and rulers,
    /// the cells drawn as frames, and what is selected.
    struct Colors {
        QColor background, ink, frames, highlight;
    };
    Colors colors() const;
    /// The layers' colours darkened on a light canvas, lightened on a dark
    /// one, till they stand out from it (standingOut()); else as their
    /// styles have them.
    bool colorsAdapted() const { return a_adapted; }
    void setColorsAdapted(bool adapted);
    /// \a style as the canvas draws it.
    LayerStyle shownStyle(const LayerStyle& style) const;

    /// The grid behind the layout, as KLayout has it: a point (a line, a
    /// cross) every step - 1, 2 or 5 times a power of ten µm, MinGridPixels
    /// apart at least - those on a power of ten stronger; in the lower left
    /// a scale of it.
    enum class GridStyle { Dots, Lines, Crosses };
    bool gridShown() const { return a_grid; }
    void setGridShown(bool shown);
    GridStyle gridStyle() const { return a_gridStyle; }
    void setGridStyle(GridStyle style);
    /// The grid's step at the scale shown, µm (no finer than the database
    /// unit), and its stronger one: the power of ten above it.
    double gridStep() const;
    double gridMajorStep() const;
    static constexpr double MinGridPixels = 16;

    /// Pixels per µm, and the point (µm) in the middle.
    double scale() const { return a_scale; }
    QPointF center() const { return a_center; }
    void setView(QPointF center, double scale);
    /// Shows \a region (µm) whole, as large as it goes.
    void showRegion(const QRectF& region);
    /// The whole cell.
    void fit();
    void zoomBy(double factor, QPointF anchor = QPointF(-1, -1));
    /// What is in sight, µm.
    QRectF visibleRegion() const;
    QPointF toLayout(QPointF pixel) const;
    QPointF toPixel(QPointF point) const;

    enum class Mode { Select, Ruler };
    Mode mode() const { return a_mode; }
    void setMode(Mode mode);
    /// The rulers, µm; the last one is being drawn while a drag lasts.
    const QList<QLineF>& rulers() const { return a_rulers; }
    void addRuler(const QLineF& ruler);
    void clearRulers();

    /// The shape or text selected, in the coordinates of the cell shown.
    const std::optional<Found>& selection() const { return a_selection; }
    void select(const std::optional<Found>& found);
    /// Selects what is at \a pixel (the next one under it when it is
    /// selected already); false when there is nothing.
    bool selectAt(QPointF pixel);

    const RenderStats& lastStats() const { return a_stats; }
    int renders() const { return a_renders; }
    /// The view drawn now, as the screen shows it.
    QImage picture();

    static constexpr double MinScale = 1e-6;   // px per µm: 1 m in a pixel
    static constexpr double MaxScale = 1e6;    // a pm in a pixel

signals:
    void viewChanged();
    /// The pointer is at \a point (µm); \a inside false when it left.
    void pointerMoved(QPointF point, bool inside);
    void selectionChanged();
    void rulersChanged();
    void modeChanged(Mode mode);
    /// A double click on a frame: show that cell.
    void cellRequested(int cell);
    void menuRequested(QPoint globalPos);
    /// The canvas's colours changed: its theme, the layers' adapted or not.
    void colorsChanged();
    /// G pressed: the grid shown, or hidden.
    void gridToggleRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;
    bool event(QEvent* event) override;

private:
    void invalidate();
    void paintGrid(QPainter& p, const Colors& c) const;
    void paintScale(QPainter& p, const Colors& c) const;
    Query queryHere() const;
    double tolerance() const;   // µm: a few pixels
    QString rulerText(const QLineF& ruler) const;

    std::shared_ptr<const Layout> a_layout;
    QVector<LayerStyle> a_styles;
    int a_cell = -1;
    int a_depth = 1 << 20;
    bool a_labels = true;
    Theme a_theme = Theme::Application;
    bool a_adapted = true;
    bool a_grid = true;
    GridStyle a_gridStyle = GridStyle::Dots;
    QPointF a_center;
    double a_scale = 1;
    bool a_fitted = false;      // fitted since the layout or the cell came
    Mode a_mode = Mode::Select;

    QImage a_cache;             // the layout drawn, under the overlays
    bool a_dirty = true;
    RenderStats a_stats;
    int a_renders = 0;

    QList<QLineF> a_rulers;
    bool a_measuring = false;
    std::optional<Found> a_selection;
    QPolygonF a_selectionOutline;   // µm, the cell shown's

    enum class Drag { None, Pan, Zoom, Ruler };
    Drag a_drag = Drag::None;
    QPoint a_pressPos;
    QPoint a_lastPos;
    bool a_moved = false;
    QRect a_zoomBox;
};

} // namespace qucs_s::layout

/*!
 * A layout in a tab: a toolbar, the cells and the layers at the side, the
 * view, and under it where the pointer is and what is selected or
 * measured. Read in the background, with a Cancel, so a large file never
 * holds the window; read again, where it was, when its file changes. Its
 * layers' colours come from a KLayout .lyp beside it, else a palette. It
 * is read, not edited.
 */
class LayoutDoc : public QFrame, public QucsDoc
{
    Q_OBJECT

public:
    LayoutDoc(QucsApp* app, const QString& name);
    ~LayoutDoc() override;

    // QucsDoc
    void setName(const QString& name) override;
    /// Starts reading the file; false when there is none.
    bool load() override;
    /// Nothing to write, but a copy under a new name (Save As).
    int save() override;
    void print(QPrinter* printer, QPainter* painter, bool printAll, bool fitToPage) override;
    void becomeCurrent(bool) override;
    double zoomBy(double factor) override;
    void showAll() override;
    void zoomToSelection() override;
    void showNoZoom() override;

    bool isLoading() const { return a_job != nullptr; }
    /// Waits for a read to end, \a ms at most (for Claude's tools and the
    /// tests); whether it ended.
    bool waitForLoaded(int ms = 60000);
    std::shared_ptr<const qucs_s::layout::Layout> layout() const { return a_layout; }
    /// Why it is not shown (it could not be read, the read was cancelled),
    /// or empty.
    QString problem() const { return a_problem; }
    qucs_s::layout::LayoutView* view() const { return a_view; }

    /// The cell shown, by its index.
    int shownCell() const;
    void showCell(int cell, bool fit = true);
    int depth() const;
    void setDepth(int depth);
    void setLayerVisible(int layer, bool visible);
    /// The .lyp the colours come from, or empty (the palette).
    QString layerPropertiesFile() const { return a_lypFile; }
    bool loadLayerProperties(const QString& path, QString* error = nullptr);
    void usePalette();
    /// The canvas's theme, here and in every layout tab, kept for the next.
    void setCanvasTheme(qucs_s::layout::LayoutView::Theme theme);
    /// The layers' colours standing out from the canvas or not, the grid
    /// shown or not and how: alike in every layout tab, kept for the next.
    void setColorsAdapted(bool adapted);
    void setGridShown(bool shown);
    void setGridStyle(qucs_s::layout::LayoutView::GridStyle style);

    /// The cells whose names have \a text (a wildcard: * ?) listed in the
    /// cell panel; the first shown.
    void find(const QString& text);
    void findNext(bool backwards = false);
    int findCount() const { return a_found.size(); }

    /// Reads the file again, the cell, view and layers kept.
    bool reload();

    // For the tests.
    QTreeWidget* cellTree() const { return a_cells; }
    QTreeWidget* layerList() const { return a_layers; }
    QLineEdit* findField() const { return a_findField; }
    QLabel* infoLabel() const { return a_info; }
    QLabel* positionLabel() const { return a_position; }
    QSpinBox* depthBox() const { return a_depthBox; }
    QPushButton* cancelButton() const { return a_cancel; }
    QToolButton* menuButton() const { return a_menuButton; }
    QToolButton* gridButton() const { return a_gridButton; }
    QWidget* sidebar() const;

public slots:
    void showSearch();
    void hideSearch();
    void setSidebarShown(bool shown);
    void openExternally();
    void openInKLayout();
    void revealInFileManager();
    void copySelection();
    void cancelLoading();
    void chooseLayerProperties();

signals:
    void loaded(bool ok);
    void reloaded();

protected:
    void changeEvent(QEvent* event) override;

private:
    struct Job;
    void buildUi();
    void restyle();
    void startReading();
    void readingDone();
    void layoutReady(bool keepView);
    void fillCells();
    void fillCellChildren(QTreeWidgetItem* item);
    void fillLayers();
    void updateLayerItem(int layer);
    void updateSwatches();
    /// \a apply to this tab and to every other layout tab.
    void everyLayoutTab(const std::function<void(LayoutDoc*)>& apply);
    void updateInfo();
    void showProblem(const QString& text);
    QIcon swatch(const qucs_s::layout::LayerStyle& style) const;
    QString describe(const qucs_s::layout::Found& found) const;

    std::shared_ptr<const qucs_s::layout::Layout> a_layout;
    QString a_problem;
    QString a_source;           // the file it was read from
    QString a_lypFile;
    QHash<qucs_s::layout::LayerKey, qucs_s::layout::LayerStyle> a_lyp;
    std::shared_ptr<Job> a_job;
    bool a_keepView = false;
    QTimer* a_progressTimer = nullptr;
    QFileSystemWatcher* a_watcher = nullptr;
    QTimer* a_reloadTimer = nullptr;
    QList<int> a_found;
    int a_foundAt = -1;

    QWidget* a_toolbar = nullptr;
    QToolButton* a_sidebarButton = nullptr;
    QToolButton* a_fitButton = nullptr;
    QToolButton* a_zoomOutButton = nullptr;
    QToolButton* a_zoomInButton = nullptr;
    QSpinBox* a_depthBox = nullptr;
    QToolButton* a_rulerButton = nullptr;
    QToolButton* a_labelsButton = nullptr;
    QToolButton* a_gridButton = nullptr;
    QToolButton* a_findButton = nullptr;
    QToolButton* a_menuButton = nullptr;
    QLabel* a_cellLabel = nullptr;
    QWidget* a_searchBar = nullptr;
    QLineEdit* a_findField = nullptr;
    QLabel* a_findLabel = nullptr;
    QStackedWidget* a_stack = nullptr;
    QSplitter* a_splitter = nullptr;
    QSplitter* a_side = nullptr;
    QTreeWidget* a_cells = nullptr;
    QTreeWidget* a_layers = nullptr;
    qucs_s::layout::LayoutView* a_view = nullptr;
    QLabel* a_position = nullptr;
    QLabel* a_info = nullptr;
    QLabel* a_loadingLabel = nullptr;
    QProgressBar* a_progress = nullptr;
    QPushButton* a_cancel = nullptr;
    QLabel* a_problemLabel = nullptr;
    QPushButton* a_retry = nullptr;
};

#endif // QUCS_LAYOUTDOC_H
