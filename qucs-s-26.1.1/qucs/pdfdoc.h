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
class QToolButton;
class QTreeView;

namespace qucs_s::pdf {

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
    /// viewport; the middle when not given) kept where it is.
    void zoomBy(qreal factor, QPoint anchor = QPoint(-1, -1));
    Fit fit() const { return a_fit; }
    void setFit(Fit fit);

    /// The page in sight: the one at a third of the view's height.
    int currentPage() const { return a_currentPage; }
    /// Scrolls to \a page, \a location (points from its top left) at the
    /// top of the view; the page's top when \a location is negative.
    void goToPage(int page, QPointF location = QPointF(-1, -1));

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

    void relayout();
    void updateScrollBars();
    void updateCurrentPage();
    QRectF pagePoints(int page) const;   // its rect in document pixels
    QPoint offset() const;
    QPointF toViewport(int page, QPointF point) const;
    QImage tile(int page, qreal scale, int tx, int ty, bool* cached);
    QImage preview(int page);
    const QList<Link>& linksOn(int page);
    const Link* linkAt(QPoint pos);
    const QList<QRectF>& charBoxes(int page);
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
    QHash<int, QImage> a_previews;
    int a_tilesRendered = 0;
    QHash<int, QList<Link>> a_links;
    QHash<int, QList<QRectF>> a_charBoxes;   // points, by the text's index

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
