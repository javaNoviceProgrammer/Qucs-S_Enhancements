/*
 * imagedoc.h - a picture in a tab of Qucs-S: a PNG, a JPEG, an SVG - what
 *              Qt reads - zoomed and panned, turned, its pixels read, a
 *              part of it selected and copied, its frames played
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_IMAGEDOC_H
#define QUCS_IMAGEDOC_H

#include "qucsdoc.h"

#include <QAbstractScrollArea>
#include <QFrame>
#include <QImage>
#include <QList>
#include <QRect>
#include <QStringList>

#include <memory>

class QComboBox;
class QFileSystemWatcher;
class QLabel;
class QMenu;
class QStackedWidget;
class QSvgRenderer;
class QTimer;
class QToolButton;

namespace qucs_s::image {

/// The suffixes of the files opened as pictures, in lower case: those this
/// Qt reads - PNG, BMP, the portable maps, and what its image plugins add
/// as it was built (JPEG, GIF, TIFF, WebP, icons...) - and SVG; not PDF,
/// which has a viewer of its own.
const QStringList& suffixes();
/// Whether \a name is a picture's, by its suffix.
bool isImageFile(const QString& name);
/// The suffixes a picture can be saved as (converted): those this Qt
/// writes.
const QStringList& writableSuffixes();
/// The size of an SVG drawn by \a svg: its own (width and height), else
/// its view box's, else a web browser's default.
QSize svgSize(const QSvgRenderer& svg);
/// The picture of \a path at most \a side pixels wide and high - read
/// small, an SVG drawn to fit - and in \a size its own size in pixels (as
/// it is shown: a photo turned upright); a null image when it cannot be
/// read.
QImage thumbnail(const QString& path, int side, QSize* size = nullptr);

/*!
 * A picture as a viewer shows it: in the middle of the view, the whole of
 * it at first - a bitmap not enlarged for that; zoomed about the pointer
 * (Ctrl and the wheel, a pinch, a double click between 100% and the fit),
 * moved with a drag, the arrow keys and the scroll bars; turned by
 * quarter turns. Its transparent parts over a checkerboard, or white, or
 * black. A bitmap is drawn smoothly when it is made smaller and in sharp
 * pixels when it is made larger, their grid seen from 800%; an SVG is
 * drawn anew at every scale, sharp at any. A rectangle of it is selected
 * with a drag in the selection mode, or with Shift and a drag.
 *
 * Its coordinates are the picture's pixels as they are in the file, not
 * turned: the selection's, the pointer's.
 */
class ImageView : public QAbstractScrollArea
{
    Q_OBJECT

public:
    /// How the zoom is chosen: as set; the whole picture in sight, a
    /// bitmap never enlarged - an SVG is (Auto) - or as large as it goes
    /// (Window); the width filled.
    enum class Fit { None, Auto, Window, Width };
    /// What is under the picture's transparent parts.
    enum class Background { Checkers, Light, Dark };

    explicit ImageView(QWidget* parent = nullptr);

    /// A bitmap - a frame of an animation: the view is kept when it has
    /// the size of the one before.
    void setImage(const QImage& image);
    /// An SVG (not owned), \a size its own size; drawn anew at each scale.
    void setSvg(QSvgRenderer* svg, QSize size);
    void clear();
    bool isEmpty() const { return a_size.isEmpty(); }
    bool isVector() const { return a_svg != nullptr; }
    /// The bitmap shown (none for an SVG).
    const QImage& image() const { return a_image; }
    /// Its size in pixels, as in the file.
    QSize imageSize() const { return a_size; }
    /// Its size as it is shown: turned.
    QSize shownSize() const;

    /// 1 is 100%: a pixel of the picture on a pixel of the screen (as the
    /// screen counts them: a point on a high-resolution one).
    qreal zoom() const { return a_zoom; }
    void setZoom(qreal zoom);
    /// \a factor times the zoom, the point under \a anchor (in the
    /// viewport; its middle when not given) kept where it is.
    void zoomBy(qreal factor, QPoint anchor = QPoint(-1, -1));
    /// To the next of the usual scales (100%, 200%, 50%...) up or down.
    void zoomStep(bool in);
    Fit fit() const { return a_fit; }
    void setFit(Fit fit);
    /// Shows \a rect (picture pixels) whole, as large as it goes.
    void showRect(const QRectF& rect);

    /// Quarter turns clockwise, 0 to 3: the picture as it is shown, not
    /// the file.
    int turns() const { return a_turns; }
    void setTurns(int turns);

    Background background() const { return a_background; }
    void setBackground(Background background);
    /// Whether the pixels' grid is drawn when they are large (800% on).
    bool pixelGrid() const { return a_grid; }
    void setPixelGrid(bool shown);

    /// Whether Page Up and Down, and Space, are an animation's (its frames,
    /// played) - else they scroll.
    void setFrameKeys(bool on) { a_frameKeys = on; }

    /// Whether a drag selects (else it moves the view).
    bool selecting() const { return a_selecting; }
    void setSelecting(bool on);
    /// The selection, in whole pixels of the picture; empty: none.
    QRect selection() const { return a_selection; }
    void setSelection(const QRect& rect);
    void clearSelection();

    /// The point of the picture (its pixels, not turned) at \a pos of the
    /// viewport, and back.
    QPointF toPicture(QPointF pos) const;
    QPointF toViewport(QPointF point) const;
    /// Where the picture is in the viewport.
    QRectF pictureRect() const;
    /// The pixel at \a pos of the viewport; (-1, -1) off the picture.
    QPoint pixelAt(QPointF pos) const;

    /// How often an SVG was drawn, and at which scale (device pixels per
    /// pixel of it) the last time - for the tests.
    int svgRenders() const { return a_svgRenders; }
    qreal svgScale() const { return a_svgCacheScale; }

    static constexpr qreal MinZoom = 0.01;
    static constexpr qreal MaxZoom = 64.0;
    /// From this zoom on the pixels' grid is drawn.
    static constexpr qreal GridZoom = 8.0;

public slots:
    /// Draws it again: an SVG changed (an animated one).
    void refresh();

signals:
    void zoomChanged(qreal zoom);
    /// The pointer over \a pixel of the picture; (-1, -1): off it.
    void hovered(QPoint pixel);
    void selectionChanged(const QRect& selection);
    /// The view's context menu, at \a globalPos, over \a pixel.
    void menuRequested(QPoint globalPos, QPoint pixel);
    /// Page Down and Page Up: the next frame, the one before.
    void stepRequested(int by);
    /// Space: an animation played or stopped.
    void playRequested();

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    bool viewportEvent(QEvent* event) override;

private:
    void relayout();
    void updateScrollBars();
    /// The picture's top left in the content (before it is scrolled).
    QPointF origin() const;
    QPoint offset() const;
    /// Picture pixels to the pixels it is shown in (turned), and on to
    /// the viewport.
    QTransform turning() const;
    QTransform toViewportTransform() const;
    /// The shown (turned) point at \a pos of the viewport, and the zoom
    /// set so that it stays there.
    QPointF shownAt(QPointF pos) const;
    void zoomTo(qreal zoom, QPointF anchor);
    void keepAt(QPointF shown, QPointF pos);
    void drawBitmap(QPainter& p, const QRectF& visible, qreal dpr);
    void drawSvg(QPainter& p, const QRectF& visible, qreal dpr);
    void drawGrid(QPainter& p, const QRectF& visible);
    /// The bitmap halved \a level times (smooth), made when first asked for.
    const QImage& halved(int level);
    QPixmap checkers() const;
    QRect selectionFrom(QPointF a, QPointF b) const;

    QImage a_image;              // as read
    QImage a_paint;              // in a format drawn fast
    QList<QImage> a_halves;      // a_paint halved, quartered...
    QSvgRenderer* a_svg = nullptr;
    QSize a_size;
    QImage a_svgCache;
    qreal a_svgCacheScale = 0;
    QRectF a_svgCacheRect;       // picture pixels
    int a_svgRenders = 0;

    qreal a_zoom = 1.0;
    Fit a_fit = Fit::Auto;
    int a_turns = 0;
    Background a_background = Background::Checkers;
    bool a_grid = true;
    bool a_selecting = false;
    bool a_frameKeys = false;
    QRect a_selection;
    QSize a_contentSize;

    enum class Drag { None, Pan, Select };
    Drag a_drag = Drag::None;
    QPoint a_pressPos;
    QPoint a_panStart;
    QPoint a_panScroll;
    QPointF a_selectFrom;
    bool a_moved = false;
};

} // namespace qucs_s::image

/*!
 * A picture in a tab: a toolbar (zoom, the turns, the selection mode, an
 * animation's or a file of several pictures' frames, the background, a
 * menu), the picture, and under it what it is - its format, size, colours
 * - with the pixel under the pointer and the selection. It is read, not
 * edited: Save As writes a copy, converted when its suffix is another
 * format's. When its file changes - a plot written again - it is read
 * again, the view kept. Opened for the suffixes of suffixes().
 */
class ImageDoc : public QFrame, public QucsDoc
{
    Q_OBJECT

public:
    ImageDoc(QucsApp* app, const QString& name);
    ~ImageDoc() override;

    // QucsDoc
    void setName(const QString& name) override;
    bool load() override;
    /// Nothing to write, but a copy under a new name (Save As): the file
    /// itself, or converted to the format of the new name's suffix.
    int save() override;
    void print(QPrinter* printer, QPainter* painter, bool printAll, bool fitToPage) override;
    void becomeCurrent(bool) override;
    double zoomBy(double factor) override;
    void showAll() override;
    void zoomToSelection() override;
    void showNoZoom() override;

    qucs_s::image::ImageView* view() const { return a_view; }
    /// The format it was read as ("png", "jpeg", "svg"...).
    QString format() const { return a_format; }
    /// Its size in pixels as in the file (an SVG's own size).
    QSize imageSize() const { return a_view->imageSize(); }
    bool isVector() const { return a_svg != nullptr; }
    /// Its frames: an animation's, the pictures of a file of several (a
    /// TIFF's pages, an icon's sizes); 1 for most.
    int frameCount() const;
    int currentFrame() const { return a_frame; }
    bool isAnimated() const { return a_animated && a_frames.size() > 1; }
    bool isPlaying() const;
    /// How long \a frame is shown, ms.
    int frameDelay(int frame) const;
    /// Why it is not shown (its file cannot be read now), or empty.
    QString problem() const { return a_problem; }
    /// What the bar under it says: what it is; the pixel under the
    /// pointer; the selection.
    QString facts() const;
    QString pointerText() const;
    QString selectionText() const;
    /// The picture as it is shown - turned, the frame in sight, an SVG
    /// drawn at \a svgScale times its size -, of the selection alone when
    /// \a selectionOnly and there is one.
    QImage picture(bool selectionOnly = true, qreal svgScale = 2.0) const;
    /// The picture as it is in the file: the frame in sight, not turned,
    /// an SVG at its own size.
    QImage filePicture() const;

    /// Reads the file again, the view kept.
    bool reload();

    // For the tests.
    QComboBox* zoomBox() const { return a_zoomBox; }
    QWidget* frameBar() const { return a_frameBar; }
    QMenu* moreMenu() const { return a_moreMenu; }

public slots:
    void setFrame(int frame);
    void stepFrame(int by);
    void setPlaying(bool playing);
    void togglePlaying();
    void rotateLeft();
    void rotateRight();
    /// The selection, or the whole picture, into the clipboard.
    void copyImage();
    void copyPath();
    void selectAll();
    void openExternally();
    void revealInFileManager();
    /// An SVG's text in the text editor, in place of the picture.
    void editAsText();

signals:
    void reloaded();
    void frameChanged(int frame);

protected:
    void changeEvent(QEvent* event) override;

private:
    struct Picture;
    static bool read(const QString& path, Picture* into, QString* why);
    void show(Picture& picture, bool keepView);
    void buildUi();
    void restyle();
    void updateZoomBox();
    void updateFrameBar();
    void updatePointer(QPoint pixel);
    void updateSelection();
    void showProblem(const QString& text);
    void scheduleFrame();
    void watch();
    void setBackground(qucs_s::image::ImageView::Background background);
    void setPixelGrid(bool shown);

    QString a_source;   // the file it was read from
    QString a_format;
    QString a_facts;
    QString a_problem;
    QString a_pointer;
    QList<QImage> a_frames;
    QList<int> a_delays;
    bool a_animated = false;
    bool a_playing = false;
    int a_frame = 0;
    int a_skipped = 0;
    QPoint a_hover{-1, -1};
    std::unique_ptr<QSvgRenderer> a_svg;

    QFileSystemWatcher* a_watcher = nullptr;
    QTimer* a_reloadTimer = nullptr;
    QTimer* a_playTimer = nullptr;

    QStackedWidget* a_stack = nullptr;
    QWidget* a_toolbar = nullptr;
    QToolButton* a_zoomOutButton = nullptr;
    QToolButton* a_zoomInButton = nullptr;
    QComboBox* a_zoomBox = nullptr;
    QToolButton* a_selectButton = nullptr;
    QWidget* a_frameBar = nullptr;
    QToolButton* a_prevButton = nullptr;
    QToolButton* a_playButton = nullptr;
    QToolButton* a_nextButton = nullptr;
    QLabel* a_frameLabel = nullptr;
    QToolButton* a_backgroundButton = nullptr;
    QToolButton* a_menuButton = nullptr;
    QMenu* a_moreMenu = nullptr;
    QAction* a_editAsText = nullptr;
    QAction* a_gridAction = nullptr;
    QList<QAction*> a_backgroundActions;
    qucs_s::image::ImageView* a_view = nullptr;
    QWidget* a_infoBar = nullptr;
    QLabel* a_factsLabel = nullptr;
    QLabel* a_selectionLabel = nullptr;
    QLabel* a_pointerLabel = nullptr;
    QLabel* a_problemLabel = nullptr;
};

#endif // QUCS_IMAGEDOC_H
