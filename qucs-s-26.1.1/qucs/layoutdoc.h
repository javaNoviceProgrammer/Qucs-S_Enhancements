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

#include "fem_materials.h"
#include "gpurendering.h"
#include "layout.h"
#include "markup.h"
#include "qucsdoc.h"

#include <QFrame>
#include <QImage>
#include <QLineF>
#include <QPointer>
#include <QSet>
#include <QWidget>

#include <functional>
#include <optional>

class MultiphysicsDoc;
class QCheckBox;
class QFileSystemWatcher;
class QLabel;
class QLineEdit;
class QMenu;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QSplitter;
class QStackedWidget;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace qucs_s::routing {
class RouterPanel;
}

namespace qucs_s::layout {

class LayoutGpu;

/*!
 * A cell of a layout drawn as a layout viewer draws it: each layer in its
 * colour and fill, the cells it places down to a depth and deeper ones as
 * their frames, y upwards. Zoomed about the pointer (the wheel, a pinch,
 * a box dragged with the right button), panned with a drag or the arrow
 * keys. A click selects the shape under the pointer (again: the one under
 * it); in ruler mode a drag measures, snapped to the shapes' corners and
 * edges; in drawing mode a drag draws with the pen in hand (markup.h), and
 * the middle button pans. Behind the layout a grid, when it is shown; the
 * layers' colours made to stand out from the canvas, light or dark.
 */
class LayoutView : public QWidget
{
    Q_OBJECT

public:
    /// \a gpuViewer: whose setting chooses the GPU for it (setGpuViewer()).
    explicit LayoutView(QWidget* parent = nullptr, gpu::Viewer gpuViewer = gpu::Viewer::Layout);

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
    /// KLayout's animations (a .lyp's scrolling and blinking layers): the
    /// step they are at, a quarter second each while one is shown.
    int animationTick() const { return a_tick; }
    bool animating() const;
    static constexpr int AnimationStep = 250;   // ms

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

    /// Select: a click selects; Ruler: a drag measures. And for a tab that
    /// places things on the layout, two picking modes the view knows
    /// nothing more of: PickPoint, a click is picked() and a drag pans;
    /// PickBox, a drag draws a box, which is boxPicked(), and a click is
    /// picked(). Draw: a drag draws with the pen in hand, or erases with
    /// the eraser, and Ctrl+Z undoes. Escape goes back to Select.
    enum class Mode { Select, Ruler, PickPoint, PickBox, Draw };
    Mode mode() const { return a_mode; }
    /// The keys held as the last pick was made (picked(), boxPicked()): a
    /// tab that places things by them - Shift, a port's return.
    Qt::KeyboardModifiers pickModifiers() const { return a_pickModifiers; }
    void setMode(Mode mode);
    /// The rulers, µm; the last one is being drawn while a drag lasts.
    const QList<QLineF>& rulers() const { return a_rulers; }
    void addRuler(const QLineF& ruler);
    void clearRulers();
    /// \a point (µm) snapped as a ruler's drag snaps it: to the nearest
    /// corner, else edge, of a shape shown within a few pixels at the zoom
    /// shown; on the database grid where there is none.
    QPointF snapped(QPointF point) const;

    /// What is drawn on the layout with the pen: strokes in µm, each on the
    /// cell it was drawn on, by name - shown with that cell, and kept when
    /// the file is read again. Not part of what the view reads: a
    /// selection, a ruler and the stats are as without it.
    const qucs_s::markup::Drawing& drawing() const { return a_drawing; }
    /// What the strokes on the cell shown are kept on: its name (empty:
    /// nothing shown).
    QString drawingPart() const;
    /// A stroke added as though it was drawn (Claude's tools, the tests).
    void addStroke(const qucs_s::markup::Stroke& stroke);
    /// The last change to the drawing undone; whether there was one.
    bool undoDrawing();
    /// The strokes on the cell shown taken away, or (\a all) every one;
    /// whether there were any.
    bool clearDrawing(bool all = false);
    /// What is drawn, read as one would read it: each stroke, cell by cell
    /// in the order drawn, how it was drawn (markup::gesturesOf()) and what
    /// it marks - the shapes by layer, the texts and the placed cells a ring
    /// is round, what a line or an arrow begins at and points at, what a
    /// mark or a highlighter lies on, what a dot is on - of the layers shown,
    /// to the depth drawn; with \a points, its points simplified, in µm.
    QJsonArray readDrawing(bool points = false) const;
    /// The strokes on the cell shown numbered \a ids marked addressed (drawn
    /// faded, left out of what a prompt tells), or not; how many changed.
    int setAddressed(const QList<int>& ids, bool addressed = true);

    /*!
     * What Claude draws over a cell to show what it means to do - "here is
     * the step I am about to make, 5 µm wide" - in µm, in a colour of its
     * own: polygons, paths as broad as they would be, texts, arrows. Kept
     * apart from the pen's strokes and from another tab's overlays, shown
     * with its cell, and not part of the layout or of what is read.
     */
    struct Sketch {
        enum class Kind { Polygon, Path, Text, Arrow };
        Kind kind = Kind::Polygon;
        QPolygonF points;    ///< an outline, a spine, an arrow's tail and tip, a text's place
        double width = 0;    ///< a path's, µm
        QString text;        ///< a text's, or an arrow's words at its tail
        QColor color;        ///< invalid: Claude's own (SketchColor)
    };
    static const QColor SketchColor;
    /// \a items drawn over \a cell (empty: its sketch taken away).
    void setSketch(const QString& cell, const QList<Sketch>& items);
    QList<Sketch> sketch(const QString& cell) const { return a_sketch.value(cell); }
    /// Every cell's sketch taken away; whether there was one.
    bool clearSketches();
    bool hasSketch() const { return !a_sketch.isEmpty(); }

    /*!
     * Polygons another tab draws over the layout, in the µm of the cell
     * shown: the RF 2.5D tab's processed shapes and its mesh cells. The
     * view knows nothing of what they mean - it draws them over the
     * layout's picture, in the order given, the first underneath - and
     * they are not part of what it reads, so a selection, a ruler and the
     * stats are unchanged by them.
     *
     * They are drawn every frame, unlike the layout, which is cached: a
     * mesh of tens of thousands of cells is kept cheap by drawing only
     * what is in sight, by drawing a shape under two pixels as a point,
     * and by stopping at MostOverlayShapes.
     */
    struct Overlay {
        QList<QPolygonF> shapes;   ///< µm, the cell shown's
        QColor line;               ///< the outline's colour (invalid: none)
        double width = 1.0;        ///< its width, in pixels
        QString name;              ///< what it is, for a tooltip
        QColor fill;               ///< what the shapes are filled with (invalid: nothing)
        /// A name written over the layout at a point: an arrow of a fixed
        /// size from \a at the way \a towards points (µm's way, y up), the
        /// text at its head; with no way, a ring at \a at and the text by it.
        struct Label {
            QPointF at;
            QString text;
            QPointF towards;
        };
        QList<Label> labels;       ///< in the line's colour, over the shapes
    };
    void setOverlays(const QList<Overlay>& overlays);
    const QList<Overlay>& overlays() const { return a_overlays; }
    /// A scale drawn in the corner, for overlays that colour a quantity:
    /// its title, the colours from low to high, and what each end is.
    struct Legend {
        QString title;
        QList<QColor> ramp;
        QString low, high;
        bool isEmpty() const { return ramp.isEmpty(); }
    };
    void setLegend(const Legend& legend);
    const Legend& legend() const { return a_legend; }
    /// How many of the overlays' shapes the last frame drew, and how many
    /// it left out (in sight but past the budget).
    int overlayShapesDrawn() const { return a_overlayDrawn; }
    int overlayShapesLeftOut() const { return a_overlayLeftOut; }
    static constexpr int MostOverlayShapes = 40000;

    /// The shape or text selected, in the coordinates of the cell shown.
    const std::optional<Found>& selection() const { return a_selection; }
    void select(const std::optional<Found>& found);
    /// Selects what is at \a pixel (the next one under it when it is
    /// selected already); false when there is nothing.
    bool selectAt(QPointF pixel);

    /// What the view draws as it stands - shapes, dots, frames, texts in
    /// sight -, the CPU's picture's counts: counted again when the view
    /// changed since (the GPU's canvas draws every shape, painted when it
    /// will: its counts are those of the CPU's picture of the same view).
    const RenderStats& lastStats() const;
    /// Whether the picture shown is drawn in part (the CPU's budget ran
    /// out; the GPU draws all - past its own budget the CPU draws). Not
    /// counted again: the last picture's.
    bool drawnInPart() const;
    int renders() const { return a_renders; }
    /// The view drawn now, as the screen shows it.
    QImage picture();
    /// The view as it stands - the grid, the layout, what is selected, the
    /// overlays, the rulers, what the pen drew - drawn afresh onto
    /// \a painter in the view's own pixels, at its device's resolution (an
    /// export's picture of more pixels, a PDF, an SVG); the canvas's
    /// background first unless \a background is false. Nothing is cached.
    void paintOnto(QPainter& painter, bool background = true);

    /// Drawn by the GPU (layoutviewgpu.h): its canvas over the view, the
    /// layers from buffers made once. The setting (Settings… > Viewers:
    /// Viewers/layoutGpu, off unless chosen) chooses; where the RHI has no backend, or
    /// QUCS_GPU=0, the CPU draws whatever it says.
    void setGpu(bool on);
    bool gpu() const;
    /// Whose setting chooses the GPU for it: the layout viewer's
    /// (Viewers/layoutGpu) unless a tab gives it its own - an RF 2.5D
    /// model's, Viewers/rf25Gpu. Drawn as that one says, now and when it
    /// changes.
    void setGpuViewer(gpu::Viewer viewer);
    gpu::Viewer gpuViewer() const { return a_gpuViewer; }
    LayoutGpu* canvas() const { return a_canvas; }
    /// The view's painting - the canvas's when \a gl, which draws the
    /// layers; else the CPU's, into its picture.
    void paintFrame(QPainter& p, LayoutGpu* gl);
    /// The canvas could not draw (no context, its shaders): the CPU's again.
    void canvasFailed();

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
    /// In a picking mode: a click at \a point (µm), and a box dragged
    /// (µm, normalised).
    void picked(QPointF point);
    void boxPicked(QRectF box);
    void menuRequested(QPoint globalPos);
    /// The canvas's colours changed: its theme, the layers' adapted or not.
    void colorsChanged();
    /// G pressed: the grid shown, or hidden.
    void gridToggleRequested();
    /// A stroke drawn, erased, undone or cleared.
    void drawingChanged();
    /// Claude's sketch was drawn or taken away.
    void sketchChanged();

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
    friend class LayoutGpu;
    void invalidate();
    /// What is drawn over the layout's picture: the scale, the selection,
    /// the overlays and their legend, the rulers, the pen's strokes.
    void paintOver(QPainter& p, const Colors& c);
    void paintGrid(QPainter& p, const Colors& c) const;
    void paintLabels(QPainter& p, const Colors& c, const Overlay& overlay) const;
    void paintSketch(QPainter& p, const Colors& c) const;
    void paintScale(QPainter& p, const Colors& c) const;
    Query queryHere() const;
    double tolerance() const;   // µm: a few pixels
    QString rulerText(const QLineF& ruler) const;

    std::shared_ptr<const Layout> a_layout;
    // Counts the layouts set: the GPU's canvas knows a layout by it (by its
    // address, a layout read again where a freed one was would be taken
    // for it).
    quint64 a_layoutGeneration = 0;
    QVector<LayerStyle> a_styles;
    int a_cell = -1;
    int a_depth = 1 << 20;
    bool a_labels = true;
    Theme a_theme = Theme::Application;
    bool a_adapted = true;
    int a_tick = 0;
    QTimer* a_animation = nullptr;
    bool a_grid = true;
    GridStyle a_gridStyle = GridStyle::Dots;
    QPointF a_center;
    double a_scale = 1;
    bool a_fitted = false;      // fitted since the layout or the cell came
    Mode a_mode = Mode::Select;

    QImage a_cache;             // the layout drawn, under the overlays
    bool a_dirty = true;
    LayoutGpu* a_canvas = nullptr;
    bool a_gpuBroken = false;
    gpu::Viewer a_gpuViewer = gpu::Viewer::Layout;
    RenderStats a_stats;        // the CPU's last picture's
    mutable RenderStats a_counted;   // lastStats(): the view as it stands
    mutable bool a_countStale = true;
    mutable QSize a_countedSize;
    int a_renders = 0;
    /// The CPU's drawing of the view as it stands.
    RenderOptions renderOptions() const;

    QList<Overlay> a_overlays;
    QHash<QString, QList<Sketch>> a_sketch;   // Claude's, by cell
    Legend a_legend;
    int a_overlayDrawn = 0;
    int a_overlayLeftOut = 0;

    QList<QLineF> a_rulers;
    bool a_measuring = false;
    qucs_s::markup::Drawing a_drawing;
    qucs_s::markup::Stroke a_stroke;               // the one being drawn
    QList<qucs_s::markup::Stroke> a_beforeErase;  // the strokes as an eraser's drag found them
    bool a_erased = false;
    std::optional<Found> a_selection;
    QPolygonF a_selectionOutline;   // µm, the cell shown's

    enum class Drag { None, Pan, Zoom, Ruler, Box, Ink, Erase };
    Drag a_drag = Drag::None;
    QPoint a_pressPos;
    QPoint a_lastPos;
    bool a_moved = false;
    QRect a_zoomBox;
    Qt::KeyboardModifiers a_pickModifiers;
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
    /// A layer drawn in \a colour, its frame and fill, as the layer list's
    /// Colour… does (until other layer properties are loaded).
    void setLayerColour(int layer, const QColor& colour);
    /// The .lyp the colours come from, or empty (the palette).
    QString layerPropertiesFile() const { return a_lypFile; }
    const qucs_s::layout::LayerProperties& layerProperties() const { return a_lyp; }
    bool loadLayerProperties(const QString& path, QString* error = nullptr);
    void usePalette();

    /// A .lyp drawn as KLayout draws it (its patterns, widths, dashes,
    /// transparency, groups and order) - else the viewer's own way (its
    /// colours and names on the palette's patterns, adapted to the
    /// background). On at first; kept, and the same in every layout tab.
    static bool klayoutLook();
    void setKLayoutLook(bool on);
    /// Whether this tab draws its layers in KLayout's look now (it has a
    /// .lyp and the look is on).
    bool drawsKLayoutLook() const;

    /// The layouts opened lately, the latest first, as the tab's Recent
    /// menu lists them (kept across sessions); MostRecent at most.
    static QStringList recentLayouts();
    static void noteRecentLayout(const QString& file);
    static void clearRecentLayouts();
    static constexpr int MostRecent = 12;
    /// Opens \a file from the Recent menu: in a tab of its own - or, open
    /// already, its tab brought to the front, as File > Open does.
    bool openRecent(const QString& file);
    /// The canvas's theme, here and in every layout tab, kept for the next.
    void setCanvasTheme(qucs_s::layout::LayoutView::Theme theme);
    /// The layers' colours standing out from the canvas or not, the grid
    /// shown or not and how: alike in every layout tab, kept for the next.
    void setColorsAdapted(bool adapted);
    void setGridShown(bool shown);
    void setGridStyle(qucs_s::layout::LayoutView::GridStyle style);
    /// Texts drawn or not, as the tab's Texts button does it - kept for the
    /// next tab (it was the canvas's alone, the button left as it was).
    void setLabelsShown(bool shown);

    /// The cells whose names have \a text (a wildcard: * ?) listed in the
    /// cell panel; the first shown.
    void find(const QString& text);
    void findNext(bool backwards = false);
    int findCount() const { return a_found.size(); }

    /// Reads the file again, the cell, view and layers kept.
    bool reload();

    /// A new multiphysics model of the cell shown cut along \a cut (µm), in a
    /// tab of its own: its layer stack \a rows, or (none) the one a dialog
    /// asks for. Null, and why in \a error, when there is none.
    MultiphysicsDoc* crossSection(const QLineF& cut, const QList<qucs_s::fem::StackRow>* rows = nullptr, QString* error = nullptr);
    /// A new multiphysics model of a process flow along \a cut (µm) of the
    /// cell shown, in a tab of its own: a silicon substrate to begin with.
    /// Null, and why in \a error, when there is none.
    MultiphysicsDoc* processFlow(const QLineF& cut, QString* error = nullptr);
    /// The cell shown's name (empty: none yet).
    QString shownCellName() const;
    /// \a cut shown: the only ruler, the view round it.
    void showCut(const QLineF& cut);
    /// The layer \a key alone shown - the others hidden - and \a cut with
    /// it; false when the layout has no such layer.
    bool showLayerAlone(const qucs_s::layout::LayerKey& key, const QLineF& cut);
    /// A point of a cut: \a cut shown, the point marked by a short ruler
    /// across it, the shape there selected.
    void probe(QPointF point, const QLineF& cut);

    /// A layer's swatch as the layer list shows it, \a ratio device pixels
    /// a pixel: 16 by 12, its frame the outermost pixel all round in \a
    /// style's frame colour, inside it \a background and the style's fill.
    static QPixmap swatchPixmap(const qucs_s::layout::LayerStyle& style, const QColor& background, qreal ratio);

    // For the tests.
    QTreeWidget* cellTree() const { return a_cells; }
    QTreeWidget* layerList() const { return a_layers; }
    QLineEdit* findField() const { return a_findField; }
    QLabel* infoLabel() const { return a_info; }
    QLabel* positionLabel() const { return a_position; }
    QSpinBox* depthBox() const { return a_depthBox; }
    QPushButton* cancelButton() const { return a_cancel; }
    QToolButton* menuButton() const { return a_menuButton; }
    QMenu* recentMenu() const { return a_recentMenu; }
    /// The layer list's row of \a layer.
    QTreeWidgetItem* layerItem(int layer) const { return layer >= 0 && layer < a_layerItems.size() ? a_layerItems.at(layer) : nullptr; }
    QToolButton* gridButton() const { return a_gridButton; }
    QToolButton* labelsButton() const { return a_labelsButton; }
    qucs_s::markup::PenButton* penButton() const { return a_penButton; }
    /// Takes Claude's sketch away (LayoutView::Sketch): shown while there
    /// is one.
    QToolButton* sketchButton() const { return a_sketchButton; }
    QToolButton* routeButton() const { return a_routeButton; }
    /// The Route panel (layoutrouterpanel.h), made when first shown; null before.
    qucs_s::routing::RouterPanel* routerPanel() const { return a_routerPanel; }
    QWidget* sidebar() const;

public slots:
    void showSearch();
    void hideSearch();
    void setSidebarShown(bool shown);
    void openExternally();
    void openInKLayout();
    void revealInFileManager();
    void copySelection();
    /// A new multiphysics model of the cell shown cut along the last ruler
    /// (its layer stack asked for).
    void crossSectionFromRuler();
    /// A new multiphysics model of a process flow along the last ruler.
    void processFlowFromRuler();
    void cancelLoading();
    void chooseLayerProperties();
    /// The Route panel shown beside the layout (made the first time), its
    /// session's job and last routes with it.
    void showRouting();
    void setRoutingShown(bool shown);

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
    /// The shown cell's rows in the tree bold, one of them in sight current.
    void markShownCell(int cell);
    static QString rowPath(const QTreeWidgetItem* item);
    void fillLayers();
    void updateLayerItem(int layer);
    /// The styles of the .lyp (or the palette) in the look chosen, the
    /// layers shown kept as they are when \a keepShown.
    void applyStyles(bool keepShown);
    void fillRecentMenu();
    /// A group's row checked as its layers are.
    void updateGroupItems();
    void setLayersVisible(const QList<int>& layers, bool visible);
    void updateSwatches();
    /// \a apply to this tab and to every other layout tab.
    void everyLayoutTab(const std::function<void(LayoutDoc*)>& apply);
    void updateInfo();
    /// The pen's menu as the drawing is.
    void updatePen();
    void showProblem(const QString& text);
    QIcon swatch(const qucs_s::layout::LayerStyle& style) const;
    QString describe(const qucs_s::layout::Found& found) const;

    std::shared_ptr<const qucs_s::layout::Layout> a_layout;
    QString a_problem;
    QString a_source;           // the file it was read from
    QString a_lypFile;
    qucs_s::layout::LayerProperties a_lyp;
    qucs_s::layout::LayerPlan a_plan;
    QVector<QTreeWidgetItem*> a_layerItems;   // by layer
    std::shared_ptr<Job> a_job;
    bool a_keepView = false;
    QTimer* a_progressTimer = nullptr;
    QFileSystemWatcher* a_watcher = nullptr;
    QTimer* a_reloadTimer = nullptr;
    QList<int> a_found;
    int a_foundAt = -1;
    /// The tree's open rows, each by its cells' names from the top: kept
    /// when the tree is filled again (a reload, a search closed).
    QSet<QString> a_openRows;

    QWidget* a_toolbar = nullptr;
    QToolButton* a_sidebarButton = nullptr;
    QToolButton* a_fitButton = nullptr;
    QToolButton* a_zoomOutButton = nullptr;
    QToolButton* a_zoomInButton = nullptr;
    QSpinBox* a_depthBox = nullptr;
    QToolButton* a_rulerButton = nullptr;
    qucs_s::markup::PenButton* a_penButton = nullptr;
    QToolButton* a_labelsButton = nullptr;
    QToolButton* a_gridButton = nullptr;
    QToolButton* a_routeButton = nullptr;
    QToolButton* a_sketchButton = nullptr;   // Claude's sketch taken away; shown while there is one
    qucs_s::routing::RouterPanel* a_routerPanel = nullptr;
    QToolButton* a_findButton = nullptr;
    QToolButton* a_menuButton = nullptr;
    QMenu* a_recentMenu = nullptr;
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
