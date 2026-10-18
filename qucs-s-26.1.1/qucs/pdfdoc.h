/*
 * pdfdoc.h - a PDF document in a tab of Qucs-S: its pages drawn by Qt's
 *            PDF module, zoomed, searched, selected and copied
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_PDFDOC_H
#define QUCS_PDFDOC_H

#include "qucsdoc.h"

#include <QAbstractScrollArea>
#include <QCache>
#include <QFrame>
#include <QHash>
#include <QImage>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QUrl>

class QComboBox;
class QFileSystemWatcher;
class QLabel;
class QLineEdit;
class QListWidget;
class QPdfDocument;
class QPdfSearchModel;
class QPdfBookmarkModel;
class QSplitter;
class QStackedWidget;
class QTabWidget;
class QTimer;
class QVariantAnimation;
class QToolButton;
class QTreeView;

namespace qucs_s::pdf {

/// Page \a page of \a pdf drawn at \a scale pixels a point on white,
/// \a region of it alone (points from its top left; empty: all of it).
QImage renderPage(QPdfDocument* pdf, int page, const QRectF& region, qreal scale);
/// A region [x1, y1, x2, y2] of page \a pageSize (points): in points from
/// its top left, or as fractions of it when all four are between 0 and 1.
QRectF regionOf(const QList<double>& corners, const QSizeF& pageSize);

/*!
 * The pages of a PDF document one under the other, as a reader scrolls
 * them: each drawn in tiles, at the screen's resolution, as it comes into
 * sight (a page at 800% is not one image of a gigabyte), the tiles kept
 * for a while. At 100% a point of the page is a point of the screen.
 * Links are followed with a click; text is selected with a drag (or a
 * word with a double click) and copied; the results of a search are
 * marked, the current one more.
 */
class PageView : public QAbstractScrollArea
{
    Q_OBJECT

public:
    /// How the zoom is chosen: as set, or so that a page fills the width
    /// or the whole of the view.
    enum class Fit { None, Width, Page };

    explicit PageView(QWidget* parent = nullptr);

    void setDocument(QPdfDocument* document);
    QPdfDocument* document() const { return a_document; }
    /// 1 is 100%.
    qreal zoom() const { return a_zoom; }
    void setZoom(qreal zoom);
    /// \a factor times the zoom, the point under \a anchor (in the
    /// viewport; the middle when not given) kept where it is. At once:
    /// setZoom(), the tools'.
    void zoomBy(qreal factor, QPoint anchor = QPoint(-1, -1));
    /// The same in steps over some 150 ms (the toolbar's, the View menu's,
    /// a mouse wheel's notch, a zoom chosen in the box): zoom() goes there
    /// a frame at a time, targetZoom() where it ends; a step before the end
    /// goes on from there.
    void zoomSmoothly(qreal factor, QPoint anchor = QPoint(-1, -1));
    qreal targetZoom() const;
    /// A step under way ended at once where it was going (its point kept).
    void finishZoom();
    /// A zoom under way: a step's frames, a pinch or a trackpad's scroll
    /// (till 100 ms after its last frame or event, or the pinch's end). Meanwhile the pages are
    /// drawn from the tiles at hand, another zoom's scaled, none rendered:
    /// each frame quick. Then the tiles at the zoom reached.
    bool zooming() const { return a_zooming; }
    Fit fit() const { return a_fit; }
    void setFit(Fit fit);

    /// The page in sight: the one at a third of the view's height.
    int currentPage() const { return a_currentPage; }
    /// Scrolls to \a page, \a location (points from its top left) at the
    /// top of the view; the page's top when \a location is negative.
    void goToPage(int page, QPointF location = QPointF(-1, -1));
    /// Shows \a region (points from \a page's top left) as large as the
    /// view takes it, in its middle.
    void showRegion(int page, const QRectF& region);

    void setSearchModel(QPdfSearchModel* model);
    /// Marks result \a index of the search model as the current one and
    /// scrolls to it (-1: none).
    void setCurrentSearchResult(int index);
    int currentSearchResult() const { return a_searchIndex; }

    bool hasSelection() const { return !a_selectionText.isEmpty(); }
    QString selectedText() const { return a_selectionText; }
    /// Selects the text of the page between \a from and \a to (points);
    /// false when there is none there.
    bool selectText(int page, QPointF from, QPointF to);
    /// All the text of \a page.
    void selectPage(int page);
    void clearSelection();
    void copySelection() const;

    /// Where \a page is in the viewport (after the layout).
    QRect pageRect(int page) const;
    /// The page at \a pos (viewport) and the point of it there (points);
    /// -1 when there is none.
    int pageAt(QPoint pos, QPointF* point = nullptr) const;
    /// How many pixels a point of the page is at this zoom.
    qreal pixelsPerPoint() const;
    /// The tiles drawn since the start (for the tests).
    int tilesRendered() const { return a_tilesRendered; }
    /// The tiles drawn from another zoom's, scaled, while their own are
    /// not there (for the tests).
    int tilesScaled() const { return a_tilesScaled; }
    /// Qt's calls for characters and selections since the start, each
    /// reading the page's text again (for the tests).
    int textCalls() const { return a_textCalls; }
    /// Drawn by the GPU: its viewport Qt's RHI's (Metal, Direct3D, OpenGL),
    /// the tiles textures kept there and scaled by it (Settings… > Viewers:
    /// Viewers/pdfGpu, off unless chosen). The pages themselves are
    /// pdfium's, on the CPU.
    void setGpu(bool on);
    bool gpu() const;
    /// What the pages are drawn on (pdfdoc.cpp): the CPU's painter on the
    /// viewport, or the GPU's frame.
    class Surface;

    static constexpr qreal MinZoom = 0.1;
    static constexpr qreal MaxZoom = 16.0;

signals:
    void currentPageChanged(int page);
    void zoomChanged(qreal zoom);
    void selectionChanged(bool any);
    /// The view's context menu, at \a globalPos.
    void menuRequested(QPoint globalPos);
    /// A link to outside the document was clicked.
    void urlActivated(const QUrl& url);

protected:
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
    struct Link {
        QRectF rect;   // points
        QUrl url;
        int page = -1;
        QPointF location;
    };

    /// The pages in \a visible (the viewport's), on \a s.
    void paintPages(Surface& s, const QRect& visible);
    void relayout();
    /// The zoom \a zoom, \a inPage of \a page (points) at \a anchor.
    void placeZoom(qreal zoom, QPoint anchor, int page, QPointF inPage);
    /// A gesture's zoom (a pinch's, a trackpad's scroll): \a factor about
    /// \a anchor, zooming() till ZoomSettleMs after it.
    void zoomGesture(qreal factor, QPoint anchor);
    /// The page and point under \a anchor, kept by a zoom's frames.
    void takeAnchor(QPoint anchor);
    /// The page nearest \a pos (viewport) and its point there (points),
    /// outside it too - the grey beside or between pages: what a zoom keeps
    /// under the pointer. -1 without pages.
    int anchorAt(QPoint pos, QPointF* point) const;
    void zoomed(int settle);
    void settle();
    /// The parts \a missing (viewport) of page \a page (at \a r) whose
    /// tiles at \a scale are not there: drawn from its tiles at the
    /// nearest other scales, the preview under them.
    void paintScaled(Surface& s, int page, const QRectF& r, const QRegion& missing, qreal scale, qreal dpr);
    void updateScrollBars();
    void updateCurrentPage();
    QRectF pagePoints(int page) const;   // its rect in document pixels
    QPoint offset() const;
    QPointF toViewport(int page, QPointF point) const;
    QImage tile(int page, qreal scale, int tx, int ty, bool* cached);
    QImage preview(int page);
    const QList<Link>& linksOn(int page);
    const Link* linkAt(QPoint pos);
    /// What a page's text gives to find its characters, once a page: its
    /// runs' boxes (a line's, or its part in one font) and two characters
    /// with a box, its first and its last.
    struct PageText {
        QList<QRectF> runs;   // points
        int first = -1, last = -1;
        QPointF firstAt, lastAt;
    };
    const PageText& pageText(int page);
    /// The character under \a point (points), by pdfium's hit test; -1 off
    /// the text.
    int hitChar(int page, QPointF point);
    /// The character at \a point, or the nearest (on its line first);
    /// -1 on a page without text. \a pastMiddle: the point is past its middle.
    int charAt(int page, QPointF point, bool* pastMiddle);
    void selectWordAt(int page, QPointF point);
    void setSelection(int page, const QString& text, const QList<QPolygonF>& bounds);

    QPdfDocument* a_document = nullptr;
    QPdfSearchModel* a_search = nullptr;
    int a_searchIndex = -1;
    qreal a_zoom = 1.0;
    Fit a_fit = Fit::Width;
    QList<QRectF> a_pages;   // document pixels
    QSize a_contentSize;
    int a_currentPage = 0;

    QCache<QString, QImage> a_tiles;
    QHash<int, QList<qreal>> a_tileScales;   // the scales a page has had tiles at, the latest last
    QHash<int, QImage> a_previews;
    int a_tilesRendered = 0;
    int a_tilesScaled = 0;
    int a_textCalls = 0;

    // A zoom under way: a step's animation (0 to 1, from a_zoomFrom to
    // a_zoomTarget), its anchor's page and point kept; a gesture's end.
    QVariantAnimation* a_zoomAnimation = nullptr;
    qreal a_zoomFrom = 1.0;
    qreal a_zoomTarget = 1.0;
    QPoint a_zoomAnchor;
    int a_zoomPage = -1;
    QPointF a_zoomPoint;
    QTimer* a_settle = nullptr;
    bool a_zooming = false;
    bool a_placing = false;   // placeZoom()'s scrolling
    QHash<int, QList<Link>> a_links;
    QHash<int, PageText> a_pageTexts;
    // charAt()'s last answers (the press point asked at each move of a drag).
    struct CharAt {
        int page;
        QPointF point;
        int index;
        bool pastMiddle;
        QRectF box;   // its character's (points), empty when none
    };
    QList<CharAt> a_charAts;
    /// The box of character \a index of \a page (points), kept: each of
    /// Qt's calls reads the page's text again (12 ms a call on a page of
    /// 12,000 runs).
    QRectF charBox(int page, int index);
    QHash<qint64, QRectF> a_charBoxes;
    // A run of one character (its box the run's), by page and run: its
    // character's index, found once.
    QHash<qint64, int> a_runChars;
    // The selection's characters [first, end) on a_selectionPage: a drag's
    // step that keeps them asks nothing again.
    int a_selectionFirst = -1, a_selectionEnd = -1;

    // A drag: a selection, a link pressed, the view panned.
    enum class Drag { None, Select, Link, Pan };
    Drag a_drag = Drag::None;
    QPoint a_pressPos;
    int a_pressPage = -1;
    QPointF a_pressPoint;
    QPoint a_panStart;
    QPoint a_panScroll;
    const Link* a_pressedLink = nullptr;

    int a_selectionPage = -1;
    QString a_selectionText;
    QList<QPolygonF> a_selectionBounds;   // points
};

} // namespace qucs_s::pdf

/*!
 * A PDF document in a tab: a toolbar (the sidebar, the page and the page
 * count, zoom, fit, find, a menu), the pages, and at the side the pages
 * small and the document's outline. It is read, not edited: saving has
 * nothing to do. When its file changes - a report written again - it is
 * read again, where it was. Opened for a .pdf when Qucs-S was built with
 * Qt's PDF module.
 */
class PdfDoc : public QFrame, public QucsDoc
{
    Q_OBJECT

public:
    PdfDoc(QucsApp* app, const QString& name);
    ~PdfDoc() override;

    // QucsDoc
    void setName(const QString& name) override;
    bool load() override;
    /// Nothing to write, but a copy under a new name (Save As).
    int save() override;
    void print(QPrinter* printer, QPainter* painter, bool printAll, bool fitToPage) override;
    void becomeCurrent(bool) override;
    double zoomBy(double factor) override;
    void showAll() override;
    void zoomToSelection() override;
    void showNoZoom() override;

    qucs_s::pdf::PageView* view() const { return a_view; }
    QPdfDocument* document() const { return a_pdf; }
    int pageCount() const;
    /// The document's title (its metadata), or its file's name.
    QString title() const;
    /// Why it is not shown (it could not be read, it wants a password),
    /// or empty.
    QString problem() const { return a_problem; }

    /// Searches for \a text; the first result from the page in sight on
    /// becomes the current one.
    void find(const QString& text);
    /// A search's hit: its page (from 0) and where on it (points).
    struct Hit {
        int page = 0;
        QRectF rect;
    };
    /// Searches for \a text on every page at once, the search bar showing
    /// it: each hit, in page order, the first the current one, in sight.
    QList<Hit> findAll(const QString& text);
    /// The destination of its outline titled \a title - the same, else the
    /// first that begins with it, else the first that has it (case aside):
    /// its page (from 0), where on it (points; negative: its top), its whole
    /// title. -1 when there is none.
    int destination(const QString& title, QPointF* location = nullptr, QString* found = nullptr) const;
    /// The outline's titles, in order, a level deeper indented.
    QStringList outlineTitles(int most = 60) const;
    /// \a page drawn at \a scale pixels a point (on white), \a region of
    /// it alone (points; empty: all of it) - the view left as it is.
    QImage render(int page, const QRectF& region, qreal scale) const;
    void findNext(bool backwards = false);
    int searchResultCount() const;
    int currentSearchResult() const;

    /// Reads the file again, the page and the zoom kept.
    bool reload();
    /// Opens it with \a password (one it asked for).
    bool unlock(const QString& password);

    // For the tests.
    QLineEdit* pageField() const { return a_pageField; }
    QLineEdit* searchField() const { return a_searchField; }
    QListWidget* thumbnails() const { return a_thumbnails; }
    QTreeView* outline() const { return a_outline; }
    QWidget* sidebar() const;

public slots:
    void nextPage();
    void previousPage();
    void firstPage();
    void lastPage();
    void showSearch();
    void hideSearch();
    void setSidebarShown(bool shown);
    void openExternally();
    void revealInFileManager();
    void copySelection();
    void selectAll();

signals:
    void reloaded();

protected:
    void changeEvent(QEvent* event) override;

private:
    void buildUi();
    void restyle();
    void documentLoaded();
    void updatePageField();
    void updateZoomBox();
    void updateSearchLabel();
    void fillThumbnails();
    void renderSomeThumbnails();
    void showProblem(const QString& text, bool askPassword);

    QPdfDocument* a_pdf = nullptr;
    QPdfSearchModel* a_search = nullptr;
    QPdfBookmarkModel* a_bookmarks = nullptr;
    QFileSystemWatcher* a_watcher = nullptr;
    QTimer* a_reloadTimer = nullptr;
    QTimer* a_thumbTimer = nullptr;
    QString a_problem;
    QString a_source;   // the file the document was read from

    QStackedWidget* a_stack = nullptr;
    QWidget* a_toolbar = nullptr;
    QToolButton* a_sidebarButton = nullptr;
    QToolButton* a_prevButton = nullptr;
    QToolButton* a_nextButton = nullptr;
    QLineEdit* a_pageField = nullptr;
    QLabel* a_pageCount = nullptr;
    QToolButton* a_zoomOutButton = nullptr;
    QToolButton* a_zoomInButton = nullptr;
    QComboBox* a_zoomBox = nullptr;
    QToolButton* a_findButton = nullptr;
    QToolButton* a_menuButton = nullptr;
    QWidget* a_searchBar = nullptr;
    QLineEdit* a_searchField = nullptr;
    QLabel* a_searchLabel = nullptr;
    QSplitter* a_splitter = nullptr;
    QTabWidget* a_side = nullptr;
    QListWidget* a_thumbnails = nullptr;
    QTreeView* a_outline = nullptr;
    qucs_s::pdf::PageView* a_view = nullptr;
    QLabel* a_problemLabel = nullptr;
    QLineEdit* a_passwordField = nullptr;
    int a_thumbsDone = 0;
};

#endif // QUCS_PDFDOC_H
