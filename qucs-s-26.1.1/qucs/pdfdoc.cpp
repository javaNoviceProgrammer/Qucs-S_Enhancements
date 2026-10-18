/*
 * pdfdoc.cpp - a PDF document in a tab of Qucs-S: its pages drawn by Qt's
 *              PDF module, zoomed, searched, selected and copied
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "pdfdoc.h"

#include "gpurendering.h"
#ifdef QUCS_GPU_VIEW
#include "rhicanvas.h"
#endif
#include "links.h"
#include "misc.h"
#include "qucs.h"
#include "settings.h"

#include <QApplication>
#include <QBoxLayout>
#include <QClipboard>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHeaderView>
#include <QIconEngine>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPdfBookmarkModel>
#include <QPdfDocument>
#include <QPdfDocumentRenderOptions>
#include <QPdfLinkModel>
#include <QStatusBar>
#include <QPdfSearchModel>
#include <QPdfSelection>
#include <QPrinter>
#include <QProcess>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>
#include <vector>
#include <functional>
#include <cmath>

namespace qucs_s::pdf {

namespace {

constexpr int Margin = 16;      // around the pages
constexpr int Spacing = 12;     // between them
constexpr int TileSize = 512;   // device pixels
constexpr int PreviewWidth = 240;
constexpr int ThumbnailWidth = 104;
// A zoom's step: its frames over this, eased out; a gesture's tiles
// rendered this long after its last event (or at its end).
constexpr int ZoomStepMs = 150;
constexpr int ZoomSettleMs = 100;
// A paint's rendering, at most: the rest in the next (some 1.7 ms a
// tile of a paper's text at 512 by 512).
constexpr int RenderBudgetMs = 30;

// The toolbar's icons, drawn in the text's color of the moment.
class GlyphIcon : public QIconEngine
{
public:
    enum Kind { Sidebar, Up, Down, Minus, Plus, Search, More, Close };
    explicit GlyphIcon(Kind kind) : a_kind(kind) {}
    QIconEngine* clone() const override { return new GlyphIcon(a_kind); }
    void paint(QPainter* p, const QRect& rect, QIcon::Mode mode, QIcon::State) override
    {
        QColor c = QApplication::palette().color(mode == QIcon::Disabled ? QPalette::Disabled : QPalette::Active, QPalette::WindowText);
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const qreal s = std::min(rect.width(), rect.height());
        p->translate(rect.center().x() - s / 2 + 0.5, rect.center().y() - s / 2 + 0.5);
        p->scale(s / 16.0, s / 16.0);
        QPen pen(c, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p->setPen(pen);
        p->setBrush(Qt::NoBrush);
        switch (a_kind) {
        case Sidebar:
            p->drawRoundedRect(QRectF(2, 3, 12, 10), 1.5, 1.5);
            p->drawLine(QPointF(6, 3), QPointF(6, 13));
            break;
        case Up: p->drawPolyline(QPolygonF{QPointF(4, 10), QPointF(8, 6), QPointF(12, 10)}); break;
        case Down: p->drawPolyline(QPolygonF{QPointF(4, 6), QPointF(8, 10), QPointF(12, 6)}); break;
        case Minus: p->drawLine(QPointF(4, 8), QPointF(12, 8)); break;
        case Plus:
            p->drawLine(QPointF(4, 8), QPointF(12, 8));
            p->drawLine(QPointF(8, 4), QPointF(8, 12));
            break;
        case Search:
            p->drawEllipse(QPointF(7, 7), 4, 4);
            p->drawLine(QPointF(10, 10), QPointF(13.5, 13.5));
            break;
        case More:
            p->setBrush(c);
            p->setPen(Qt::NoPen);
            for (qreal x : {4.0, 8.0, 12.0}) p->drawEllipse(QPointF(x, 8), 1.2, 1.2);
            break;
        case Close:
            p->drawLine(QPointF(5, 5), QPointF(11, 11));
            p->drawLine(QPointF(11, 5), QPointF(5, 11));
            break;
        }
        p->restore();
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        QPixmap pm(size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        paint(&p, QRect(QPoint(0, 0), size), mode, state);
        return pm;
    }

private:
    Kind a_kind;
};

QIcon glyph(GlyphIcon::Kind kind)
{
    return QIcon(new GlyphIcon(kind));
}

QString tileKey(int page, qreal scale, int tx, int ty)
{
    return QStringLiteral("%1:%2:%3:%4").arg(page).arg(qint64(std::lround(scale * 10000))).arg(tx).arg(ty);
}

bool isWordChar(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('-') || c == QLatin1Char('.');
}

} // namespace

// ----------------------------------------------------------------------
// PageView

PageView::PageView(QWidget* parent) : QAbstractScrollArea(parent)
{
    a_tiles.setMaxCost(192 * 1024);   // KB: some 190 MB of tiles
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setMouseTracking(true);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    verticalScrollBar()->setSingleStep(24);
    horizontalScrollBar()->setSingleStep(24);
    a_settle = new QTimer(this);
    a_settle->setSingleShot(true);
    connect(a_settle, &QTimer::timeout, this, &PageView::settle);
    // A step's frames: its zoom eased out, in proportion (each frame the
    // same factor more at an even pace) - from a_zoomFrom to a_zoomTarget.
    a_zoomAnimation = new QVariantAnimation(this);
    a_zoomAnimation->setDuration(ZoomStepMs);
    a_zoomAnimation->setStartValue(0.0);
    a_zoomAnimation->setEndValue(1.0);
    a_zoomAnimation->setEasingCurve(QEasingCurve::OutCubic);
    connect(a_zoomAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        if (a_zoomAnimation->state() != QAbstractAnimation::Running) return;
        const qreal t = value.toDouble();
        if (a_zoomPage < 0) takeAnchor(a_zoomAnchor);   // (scrolled meanwhile)
        placeZoom(t >= 1 ? a_zoomTarget : a_zoomFrom * std::pow(a_zoomTarget / a_zoomFrom, t), a_zoomAnchor, a_zoomPage, a_zoomPoint);
    });
    connect(a_zoomAnimation, &QVariantAnimation::finished, this, [this] {
        if (a_zoomPage < 0) takeAnchor(a_zoomAnchor);
        if (a_zoom != a_zoomTarget) placeZoom(a_zoomTarget, a_zoomAnchor, a_zoomPage, a_zoomPoint);
        // (Its tiles a moment after: a step clicked again soon goes on
        // from the last's scaled tiles, none rendered between.)
        a_settle->start(ZoomSettleMs);
    });
    // Drawn by the GPU as set, and when the setting changes.
    setGpu(gpu::used(gpu::Viewer::Pdf));
    connect(gpu::Notifier::instance(), &gpu::Notifier::changed, this, [this](gpu::Viewer viewer) {
        if (viewer == gpu::Viewer::Pdf) setGpu(gpu::used(gpu::Viewer::Pdf));
    });
}

// What the pages are drawn on: fills, images (a tile at its pixel ratio's
// size, or scaled - smoothly - to a target) and a clip of rectangles.
class PageView::Surface
{
public:
    virtual ~Surface() = default;
    virtual void fillRect(const QRectF& rect, const QColor& color) = 0;
    /// Convex.
    virtual void fillPolygon(const QPolygonF& polygon, const QColor& color) = 0;
    virtual void drawImage(const QPointF& at, const QImage& image) = 0;
    virtual void drawImage(const QRectF& target, const QImage& image) = 0;
    /// Empty: none.
    virtual void setClip(const QRegion& region) = 0;
};

namespace {

// The CPU's: a painter on the viewport.
class PainterSurface : public PageView::Surface
{
public:
    explicit PainterSurface(QPainter& p) : a_p(p) {}
    void fillRect(const QRectF& rect, const QColor& color) override { a_p.fillRect(rect, color); }
    void fillPolygon(const QPolygonF& polygon, const QColor& color) override
    {
        a_p.setPen(Qt::NoPen);
        a_p.setBrush(color);
        a_p.drawPolygon(polygon);
        a_p.setBrush(Qt::NoBrush);
    }
    void drawImage(const QPointF& at, const QImage& image) override
    {
        // At a whole device pixel, a half rounded up, as the GPU's. (An
        // image of a pixel ratio above 1 Qt draws scaled: its left edge
        // rounded, then its width - at a half left of nought, -10.5, a tile
        // 512 pixels wide drawn 513, a column doubled, the text after it a
        // pixel right of the GPU's.)
        const QTransform t = a_p.combinedTransform();
        if (t.type() <= QTransform::TxScale) {
            const QPointF device = t.map(at);
            a_p.drawImage(t.inverted().map(QPointF(std::floor(device.x() + 0.5), std::floor(device.y() + 0.5))), image);
        } else {
            a_p.drawImage(at, image);
        }
    }
    void drawImage(const QRectF& target, const QImage& image) override
    {
        // (Smooth for this image alone: left on, the rest of the paint's
        // tiles - at fractions of a pixel - were drawn smoothed too.)
        const bool smooth = a_p.testRenderHint(QPainter::SmoothPixmapTransform);
        a_p.setRenderHint(QPainter::SmoothPixmapTransform);
        a_p.drawImage(target, image);
        a_p.setRenderHint(QPainter::SmoothPixmapTransform, smooth);
    }
    void setClip(const QRegion& region) override
    {
        if (region.isEmpty()) a_p.setClipping(false);
        else a_p.setClipRegion(region);
    }

private:
    QPainter& a_p;
};

#ifdef QUCS_GPU_VIEW
// The GPU's: its frame, the images textures kept by their cache keys.
class GpuSurface : public PageView::Surface
{
public:
    explicit GpuSurface(gpu::Canvas2D& c) : a_c(c) {}
    void fillRect(const QRectF& rect, const QColor& color) override { a_c.fillRect(rect, color); }
    void fillPolygon(const QPolygonF& polygon, const QColor& color) override { a_c.fillPolygon(polygon, color); }
    void drawImage(const QPointF& at, const QImage& image) override { a_c.drawImage(at, image); }
    void drawImage(const QRectF& target, const QImage& image) override { a_c.drawImage(target, image); }
    void setClip(const QRegion& region) override { a_c.setClip(region); }

private:
    gpu::Canvas2D& a_c;
};
#endif

} // namespace

void PageView::setGpu(bool on)
{
#ifdef QUCS_GPU_VIEW
    gpu::setGpuViewport(
        this, on && gpu::hindrance().isEmpty(),
        [this](gpu::Canvas2D& c) {
            GpuSurface s(c);
            paintPages(s, viewport()->rect());
        },
        [this] { setGpu(false); });
#else
    Q_UNUSED(on);
#endif
}

bool PageView::gpu() const
{
    return gpu::hasGpuViewport(this);
}

void PageView::setDocument(QPdfDocument* document)
{
    a_zoomAnimation->stop();
    a_settle->stop();
    a_zooming = false;
    a_document = document;
    a_tiles.clear();
    a_tileScales.clear();
    a_previews.clear();
    a_links.clear();
    a_pageTexts.clear();
    a_charAts.clear();
    a_charBoxes.clear();
    a_runChars.clear();
    clearSelection();
    a_currentPage = 0;
    relayout();
    verticalScrollBar()->setValue(0);
    updateCurrentPage();
    viewport()->update();
}

qreal PageView::pixelsPerPoint() const
{
    return a_zoom * logicalDpiX() / 72.0;
}

void PageView::setZoom(qreal zoom)
{
    a_fit = Fit::None;
    const qreal z = std::clamp(zoom, MinZoom, MaxZoom);
    if (z == a_zoom && a_zoomAnimation->state() != QAbstractAnimation::Running) return;
    zoomBy(z / a_zoom);
}

void PageView::zoomBy(qreal factor, QPoint anchor)
{
    // (At once, its tiles too: a step under way ends where this is. A
    // gesture's zoom is zooming() again after it.)
    a_zoomAnimation->stop();
    a_settle->stop();
    a_zooming = false;
    const qreal z = std::clamp(a_zoom * factor, MinZoom, MaxZoom);
    a_fit = Fit::None;
    a_zoomTarget = z;
    if (!(z > 0) || z == a_zoom || a_pages.isEmpty()) {
        a_zoom = z;
        relayout();
        emit zoomChanged(a_zoom);
        return;
    }
    // The point under the anchor stays under it.
    if (anchor.x() < 0) anchor = viewport()->rect().center();
    QPointF inPage;
    const int page = anchorAt(anchor, &inPage);
    placeZoom(z, anchor, page, inPage);
}

void PageView::placeZoom(qreal zoom, QPoint anchor, int page, QPointF inPage)
{
    a_zoom = std::clamp(zoom, MinZoom, MaxZoom);
    a_placing = true;
    relayout();
    if (page >= 0 && page < a_pages.size()) {
        const QPointF at = toViewport(page, inPage);
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() + int(std::lround(at.x() - anchor.x())));
        verticalScrollBar()->setValue(verticalScrollBar()->value() + int(std::lround(at.y() - anchor.y())));
    }
    a_placing = false;
    viewport()->update();
    emit zoomChanged(a_zoom);
}

void PageView::zoomSmoothly(qreal factor, QPoint anchor)
{
    const bool running = a_zoomAnimation->state() == QAbstractAnimation::Running;
    const qreal target = std::clamp((running ? a_zoomTarget : a_zoom) * factor, MinZoom, MaxZoom);
    if (a_pages.isEmpty() || (!running && target == a_zoom)) {
        zoomBy(factor, anchor);
        return;
    }
    // From the zoom shown, the point under the anchor kept there all the
    // way (each frame placed from it: no rounding gathered).
    if (anchor.x() < 0) anchor = viewport()->rect().center();
    a_zoomAnimation->stop();
    takeAnchor(anchor);
    a_fit = Fit::None;
    a_zoomFrom = a_zoom;
    a_zoomTarget = target;
    a_zooming = true;
    a_settle->stop();
    a_zoomAnimation->start();
}

void PageView::finishZoom()
{
    if (a_zoomAnimation->state() != QAbstractAnimation::Running) return;
    a_zoomAnimation->stop();
    if (a_zoomPage < 0) takeAnchor(a_zoomAnchor);
    placeZoom(a_zoomTarget, a_zoomAnchor, a_zoomPage, a_zoomPoint);
    a_settle->start(ZoomSettleMs);
}

qreal PageView::targetZoom() const
{
    return a_zoomAnimation->state() == QAbstractAnimation::Running ? a_zoomTarget : a_zoom;
}

void PageView::takeAnchor(QPoint anchor)
{
    a_zoomAnchor = anchor;
    a_zoomPage = anchorAt(anchor, &a_zoomPoint);
}

int PageView::anchorAt(QPoint pos, QPointF* point) const
{
    // (Off every page, the nearest one's point - beyond its edge. It was
    // the page in sight's top left corner: a zoom with the pointer beside
    // a page threw that corner under it, a jump of up to a page.)
    if (a_pages.isEmpty()) return -1;
    const QPointF doc = QPointF(pos) + QPointF(offset());
    const auto it = std::lower_bound(a_pages.cbegin(), a_pages.cend(), doc.y(), [](const QRectF& r, qreal y) { return r.bottom() < y; });
    int page = it == a_pages.cend() ? int(a_pages.size()) - 1 : int(it - a_pages.cbegin());
    // (Between two pages: the nearer.)
    if (page > 0 && doc.y() < a_pages.at(page).top() && doc.y() - a_pages.at(page - 1).bottom() < a_pages.at(page).top() - doc.y()) --page;
    if (point != nullptr) *point = (doc - a_pages.at(page).topLeft()) / pixelsPerPoint();
    return page;
}

void PageView::zoomGesture(qreal factor, QPoint anchor)
{
    if (a_pages.isEmpty()) {
        zoomBy(factor, anchor);
        return;
    }
    // The point under the fingers taken once a gesture (again where they
    // move, or the view is scrolled), each event's zoom placed from it: an
    // event at a time, each scroll rounded to a pixel, it moved 2 pixels
    // in 20.
    a_zoomAnimation->stop();
    if (!a_zooming || anchor != a_zoomAnchor || a_zoomPage < 0) takeAnchor(anchor);
    a_fit = Fit::None;
    a_zoomTarget = std::clamp(a_zoom * factor, MinZoom, MaxZoom);
    placeZoom(a_zoomTarget, anchor, a_zoomPage, a_zoomPoint);
    zoomed(ZoomSettleMs);
}

void PageView::zoomed(int settle)
{
    a_zooming = true;
    a_settle->start(settle);
}

void PageView::settle()
{
    if (a_zoomAnimation->state() == QAbstractAnimation::Running) return;
    a_settle->stop();
    if (!a_zooming) return;
    a_zooming = false;
    viewport()->update();
}

void PageView::setFit(Fit fit)
{
    a_zoomAnimation->stop();
    a_settle->stop();
    a_zooming = false;
    const int page = a_currentPage;
    a_fit = fit;
    relayout();
    if (fit != Fit::None) goToPage(page);
    viewport()->update();
    emit zoomChanged(a_zoom);
}

void PageView::relayout()
{
    a_pages.clear();
    const int n = a_document != nullptr ? a_document->pageCount() : 0;
    if (n <= 0) {
        a_contentSize = QSize();
        updateScrollBars();
        return;
    }
    const qreal dpi = logicalDpiX() / 72.0;
    // The zoom a fit asks for, from the page in sight (the widest for the width).
    if (a_fit != Fit::None) {
        const QSizeF current = a_document->pagePointSize(std::clamp(a_currentPage, 0, n - 1));
        qreal widest = 0;
        for (int i = 0; i < n; ++i) widest = std::max(widest, a_document->pagePointSize(i).width());
        const qreal w = viewport()->width() - 2 * Margin;
        const qreal h = viewport()->height() - 2 * Margin;
        qreal z = widest > 0 ? w / (widest * dpi) : 1;
        if (a_fit == Fit::Page && current.height() > 0) z = std::min(w / (current.width() * dpi), h / (current.height() * dpi));
        a_zoom = std::clamp(z, MinZoom, MaxZoom);
    }
    const qreal ppp = a_zoom * dpi;
    qreal widest = 0;
    QList<QSizeF> sizes;
    for (int i = 0; i < n; ++i) {
        const QSizeF s = a_document->pagePointSize(i) * ppp;
        sizes << s;
        widest = std::max(widest, s.width());
    }
    const qreal width = std::max<qreal>(viewport()->width(), widest + 2 * Margin);
    qreal y = Margin;
    for (const QSizeF& s : std::as_const(sizes)) {
        a_pages << QRectF(std::floor((width - s.width()) / 2), y, s.width(), s.height());
        y += s.height() + Spacing;
    }
    a_contentSize = QSize(int(std::ceil(width)), int(std::ceil(y - Spacing + Margin)));
    updateScrollBars();
}

void PageView::updateScrollBars()
{
    const QSize vp = viewport()->size();
    horizontalScrollBar()->setRange(0, std::max(0, a_contentSize.width() - vp.width()));
    horizontalScrollBar()->setPageStep(vp.width());
    verticalScrollBar()->setRange(0, std::max(0, a_contentSize.height() - vp.height()));
    verticalScrollBar()->setPageStep(vp.height());
}

QPoint PageView::offset() const
{
    return QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value());
}

QRectF PageView::pagePoints(int page) const
{
    return page >= 0 && page < a_pages.size() ? a_pages.at(page) : QRectF();
}

QRect PageView::pageRect(int page) const
{
    return pagePoints(page).translated(-offset()).toAlignedRect();
}

QPointF PageView::toViewport(int page, QPointF point) const
{
    const QRectF r = pagePoints(page);
    return r.topLeft() - QPointF(offset()) + point * pixelsPerPoint();
}

int PageView::pageAt(QPoint pos, QPointF* point) const
{
    const QPointF doc = QPointF(pos) + QPointF(offset());
    // The pages are in order: find the first whose bottom is below.
    const auto it = std::lower_bound(a_pages.cbegin(), a_pages.cend(), doc.y(),
                                     [](const QRectF& r, qreal y) { return r.bottom() < y; });
    if (it == a_pages.cend() || !it->contains(doc)) return -1;
    const int page = int(it - a_pages.cbegin());
    if (point != nullptr) *point = (doc - it->topLeft()) / pixelsPerPoint();
    return page;
}

void PageView::updateCurrentPage()
{
    if (a_pages.isEmpty()) return;
    const qreal y = verticalScrollBar()->value() + viewport()->height() / 3.0;
    int page = int(a_pages.size()) - 1;
    for (int i = 0; i < a_pages.size(); ++i)
        if (a_pages.at(i).bottom() + Spacing / 2.0 >= y) {
            page = i;
            break;
        }
    // At the very end, the last page.
    if (verticalScrollBar()->value() >= verticalScrollBar()->maximum() && verticalScrollBar()->maximum() > 0) page = int(a_pages.size()) - 1;
    if (page != a_currentPage) {
        a_currentPage = page;
        emit currentPageChanged(page);
    }
}

void PageView::goToPage(int page, QPointF location)
{
    if (a_pages.isEmpty()) return;
    page = std::clamp(page, 0, int(a_pages.size()) - 1);
    const QRectF r = a_pages.at(page);
    qreal y = r.top() - Margin / 2.0;
    if (location.y() >= 0) y = r.top() + location.y() * pixelsPerPoint() - Margin;
    verticalScrollBar()->setValue(int(std::lround(y)));
    if (location.x() >= 0) {
        const qreal x = r.left() + location.x() * pixelsPerPoint();
        if (x < horizontalScrollBar()->value() || x > horizontalScrollBar()->value() + viewport()->width())
            horizontalScrollBar()->setValue(int(std::lround(x - Margin)));
    }
    // The page asked for is the current one, even when it cannot come to
    // the top (the last pages).
    if (a_currentPage != page) {
        a_currentPage = page;
        emit currentPageChanged(page);
    }
    viewport()->update();
}

void PageView::showRegion(int page, const QRectF& region)
{
    if (a_pages.isEmpty() || region.isEmpty()) return;
    page = std::clamp(page, 0, int(a_pages.size()) - 1);
    // The zoom that fits it, with the margins; then its middle the view's.
    const qreal dpi = logicalDpiX() / 72.0;
    const qreal w = viewport()->width() - 2 * Margin, h = viewport()->height() - 2 * Margin;
    setZoom(std::min(w / (region.width() * dpi), h / (region.height() * dpi)));
    const QRectF r = a_pages.at(page);
    const QPointF middle = r.topLeft() + region.center() * pixelsPerPoint();
    horizontalScrollBar()->setValue(int(std::lround(middle.x() - viewport()->width() / 2.0)));
    verticalScrollBar()->setValue(int(std::lround(middle.y() - viewport()->height() / 2.0)));
    if (a_currentPage != page) {
        a_currentPage = page;
        emit currentPageChanged(page);
    }
    viewport()->update();
}

void PageView::setSearchModel(QPdfSearchModel* model)
{
    a_search = model;
    a_searchIndex = -1;
    viewport()->update();
}

void PageView::setCurrentSearchResult(int index)
{
    a_searchIndex = index;
    if (a_search != nullptr && index >= 0 && index < a_search->rowCount(QModelIndex())) {
        const QPdfLink result = a_search->resultAtIndex(index);
        const QList<QRectF> rects = result.rectangles();
        if (!rects.isEmpty()) {
            // In sight, with some room above.
            QRectF all = rects.first();
            for (const QRectF& r : rects) all = all.united(r);
            const QPointF top = toViewport(result.page(), all.topLeft());
            const QPointF bottom = toViewport(result.page(), all.bottomRight());
            if (top.y() < 0 || bottom.y() > viewport()->height() || top.x() < 0 || bottom.x() > viewport()->width()) {
                verticalScrollBar()->setValue(verticalScrollBar()->value() + int(top.y() - viewport()->height() / 3.0));
                if (top.x() < 0 || bottom.x() > viewport()->width())
                    horizontalScrollBar()->setValue(horizontalScrollBar()->value() + int(top.x() - viewport()->width() / 3.0));
            }
        } else {
            goToPage(result.page());
        }
    }
    viewport()->update();
}

void PageView::setSelection(int page, const QString& text, const QList<QPolygonF>& bounds)
{
    // (Its characters' range: selectText's alone sets it, after.)
    a_selectionFirst = a_selectionEnd = -1;
    const bool had = hasSelection();
    a_selectionPage = page;
    a_selectionText = text;
    a_selectionBounds = bounds;
    viewport()->update();
    if (had != hasSelection() || hasSelection()) emit selectionChanged(hasSelection());
}

void PageView::clearSelection()
{
    if (a_selectionText.isEmpty() && a_selectionBounds.isEmpty()) return;
    setSelection(-1, QString(), {});
}

QRectF PageView::charBox(int page, int index)
{
    const qint64 key = (qint64(page) << 32) | quint32(index);
    if (auto it = a_charBoxes.constFind(key); it != a_charBoxes.constEnd()) return *it;
    const QRectF box = a_document->getSelectionAtIndex(page, index, 1).boundingRectangle();
    ++a_textCalls;
    if (a_charBoxes.size() > 4096) a_charBoxes.clear();
    a_charBoxes.insert(key, box);
    return box;
}

const PageView::PageText& PageView::pageText(int page)
{
    auto it = a_pageTexts.find(page);
    if (it == a_pageTexts.end()) {
        PageText t;
        const QPdfSelection all = a_document->getAllText(page);
        ++a_textCalls;
        for (const QPolygonF& run : all.bounds()) t.runs << run.boundingRect();
        // (Two anchors for the hit test: a character with a box at each end
        // - the text's first ones may be spaces or a line's end, boxless.)
        const int n = all.endIndex();
        for (int i = 0; i < std::min(n, 64) && t.first < 0; ++i)
            if (const QRectF b = a_document->getSelectionAtIndex(page, i, 1).boundingRectangle(); !b.isEmpty()) {
                t.first = i;
                t.firstAt = b.center();
            }
        for (int i = n - 1; i >= std::max(t.first + 1, n - 64) && t.last < 0; --i)
            if (const QRectF b = a_document->getSelectionAtIndex(page, i, 1).boundingRectangle(); !b.isEmpty()) {
                t.last = i;
                t.lastAt = b.center();
            }
        it = a_pageTexts.insert(page, t);
    }
    return *it;
}

int PageView::hitChar(int page, QPointF point)
{
    // pdfium's own hit test (QPdfDocument::getSelection): from the first
    // character to the point, the point's character the selection's end;
    // the first's own, from it to the last, its start. Nothing there (off
    // the text, beyond its tolerance): neither. (A box for each character
    // took a call each, some 2 ms: a page of 6000 characters 11 s at the
    // first selection on it, the program not answering.)
    const PageText& t = pageText(page);
    if (t.first < 0) return -1;
    QPdfSelection s = a_document->getSelection(page, t.firstAt, point);
    ++a_textCalls;
    if (s.isValid() && s.endIndex() > t.first) return s.endIndex();
    if (t.last > t.first) {
        s = a_document->getSelection(page, point, t.lastAt);
        ++a_textCalls;
        if (s.isValid() && s.startIndex() >= t.first && s.startIndex() < t.last) return s.startIndex();
    }
    return -1;
}

int PageView::charAt(int page, QPointF point, bool* pastMiddle)
{
    for (const CharAt& c : std::as_const(a_charAts))
        if (c.page == page && c.point == point) {
            if (pastMiddle != nullptr) *pastMiddle = c.pastMiddle;
            return c.index;
        }
    // (Each of Qt's calls reads the page's text again - some 12 ms on a
    // page of 12,000 runs, 3 on 3,000 -, so as few as can be: a point in
    // the character found last is it again; one beyond pdfium's reach of
    // every run - some 8 points - is not tried.)
    const auto known = [&](int index, const QRectF& box) {
        const bool past = point.x() > box.center().x();
        a_charAts.prepend({page, point, index, past, box});
        if (a_charAts.size() > 4) a_charAts.removeLast();
        if (pastMiddle != nullptr) *pastMiddle = past;
        return index;
    };
    for (const CharAt& c : std::as_const(a_charAts))
        if (c.page == page && c.index >= 0 && c.box.contains(point)) return known(c.index, c.box);
    for (auto it = a_charBoxes.constBegin(); it != a_charBoxes.constEnd(); ++it)
        if (int(it.key() >> 32) == page && it.value().contains(point)) return known(int(it.key() & 0xffffffff), it.value());
    // The nearest run, and whether the point is within pdfium's reach of
    // one. A run of one character met before is that character's: no call
    // (labels scattered over a drawing, a map - each its own run).
    const QList<QRectF>& runs = pageText(page).runs;
    int nearest = -1;
    qreal best = 0;
    for (int r = 0; r < runs.size(); ++r) {
        const QRectF& box = runs.at(r);
        if (box.isEmpty()) continue;
        const qreal dx = std::max({box.left() - point.x(), 0.0, point.x() - box.right()});
        const qreal dy = std::max({box.top() - point.y(), 0.0, point.y() - box.bottom()});
        const qreal d = dx * dx + dy * dy;
        if (nearest < 0 || d < best) {
            nearest = r;
            best = d;
        }
    }
    const bool inReach = nearest >= 0 && runs.at(nearest).adjusted(-16, -16, 16, 16).contains(point);
    if (inReach)
        if (auto it = a_runChars.constFind((qint64(page) << 32) | quint32(nearest)); it != a_runChars.constEnd())
            return known(*it, runs.at(nearest));
    int index = inReach ? hitChar(page, point) : -1;
    if (index < 0) {
        // Off the text: the nearest run's - a line's, the line first -,
        // the point brought into it; on a gap there (a space, between
        // words), along it a step at a time, out from the point; the next
        // nearest runs after it.
        const PageText& t = pageText(page);
        std::vector<std::pair<qreal, const QRectF*>> near;
        for (const QRectF& box : t.runs) {
            if (box.isEmpty()) continue;
            const qreal dx = std::max({box.left() - point.x(), 0.0, point.x() - box.right()});
            const qreal dy = std::max({box.top() - point.y(), 0.0, point.y() - box.bottom()});
            near.push_back({dx * dx + dy * dy * 4, &box});   // the line first
        }
        const std::size_t tried = std::min<std::size_t>(near.size(), 3);
        std::partial_sort(near.begin(), near.begin() + std::ptrdiff_t(tried), near.end(),
                          [](const auto& a, const auto& b) { return a.first < b.first; });
        // (Six hit tests at most: each reads the page's text again - 51 a
        // point, as it was, 110 ms on a page of 12,000 runs.)
        int tests = 0;
        for (std::size_t r = 0; r < tried && index < 0 && tests < 6; ++r) {
            const QRectF& box = *near[r].second;
            const qreal inset = std::min<qreal>(0.5, box.width() / 4);
            const qreal x0 = std::clamp(point.x(), box.left() + inset, box.right() - inset);
            const qreal step = std::max<qreal>(1.0, box.height() / 2);
            for (int k = 0; k <= 8 && index < 0 && tests < 6; ++k)
                for (const int sign : {1, -1}) {
                    if (k == 0 && sign < 0) continue;
                    const qreal x = x0 + sign * k * step;
                    if (x < box.left() || x > box.right()) continue;
                    ++tests;
                    index = hitChar(page, QPointF(x, box.center().y()));
                    if (index >= 0 || tests >= 6) break;
                }
        }
    }
    bool past = false;
    QRectF box;
    if (index >= 0) {
        box = charBox(page, index);
        past = !box.isEmpty() && point.x() > box.center().x();
        // (Its run one character: the run is it.)
        if (inReach && !box.isEmpty()) {
            const QRectF run = runs.at(nearest);
            if (std::abs(run.left() - box.left()) < 0.5 && std::abs(run.right() - box.right()) < 0.5 && std::abs(run.top() - box.top()) < 0.5
                && std::abs(run.bottom() - box.bottom()) < 0.5) {
                if (a_runChars.size() > 100000) a_runChars.clear();
                a_runChars.insert((qint64(page) << 32) | quint32(nearest), index);
            }
        }
    }
    a_charAts.prepend({page, point, index, past, box});
    if (a_charAts.size() > 4) a_charAts.removeLast();
    if (pastMiddle != nullptr) *pastMiddle = past;
    return index;
}

bool PageView::selectText(int page, QPointF from, QPointF to)
{
    if (a_document == nullptr || page < 0 || page >= a_document->pageCount()) return false;
    // By the characters: the nearest to each end, each taken once the
    // drag is past its middle - as a text editor does.
    bool fromPast = false, toPast = false;
    int a = charAt(page, from, &fromPast);
    int b = charAt(page, to, &toPast);
    if (a < 0 || b < 0) {
        clearSelection();
        return false;
    }
    if (b < a || (a == b && to.x() < from.x())) {
        std::swap(a, b);
        std::swap(fromPast, toPast);
    }
    const int first = fromPast ? a + 1 : a;
    const int end = toPast ? b + 1 : b;
    if (end <= first) {
        clearSelection();
        return false;
    }
    // (A drag's step over the same characters: the selection it has.)
    if (page == a_selectionPage && first == a_selectionFirst && end == a_selectionEnd && hasSelection()) return true;
    const QPdfSelection s = a_document->getSelectionAtIndex(page, first, end - first);
    ++a_textCalls;
    if (!s.isValid() || s.text().isEmpty()) {
        clearSelection();
        return false;
    }
    setSelection(page, s.text(), s.bounds());
    a_selectionFirst = first;
    a_selectionEnd = end;
    return true;
}

void PageView::selectPage(int page)
{
    if (a_document == nullptr || page < 0 || page >= a_document->pageCount()) return;
    const QPdfSelection s = a_document->getAllText(page);
    if (s.text().isEmpty()) {
        clearSelection();
        return;
    }
    setSelection(page, s.text(), s.bounds());
}

void PageView::selectWordAt(int page, QPointF point)
{
    const int index = charAt(page, point, nullptr);
    if (index < 0) return;
    const QString text = a_document->getAllText(page).text();
    if (index >= text.size() || !isWordChar(text.at(index))) {
        const QPdfSelection one = a_document->getSelectionAtIndex(page, index, 1);
        if (one.isValid()) setSelection(page, one.text(), one.bounds());
        return;
    }
    int from = index, to = index;
    while (from > 0 && isWordChar(text.at(from - 1))) --from;
    while (to + 1 < text.size() && isWordChar(text.at(to + 1))) ++to;
    const QPdfSelection s = a_document->getSelectionAtIndex(page, from, to - from + 1);
    if (s.isValid()) setSelection(page, s.text(), s.bounds());
}

void PageView::copySelection() const
{
    if (hasSelection()) QApplication::clipboard()->setText(a_selectionText);
}

const QList<PageView::Link>& PageView::linksOn(int page)
{
    auto it = a_links.find(page);
    if (it == a_links.end()) {
        QList<Link> links;
        QPdfLinkModel model;
        model.setDocument(a_document);
        model.setPage(page);
        for (int i = 0; i < model.rowCount(QModelIndex()); ++i) {
            const QModelIndex index = model.index(i, 0);
            Link l;
            l.rect = model.data(index, int(QPdfLinkModel::Role::Rectangle)).toRectF();
            l.url = model.data(index, int(QPdfLinkModel::Role::Url)).toUrl();
            l.page = model.data(index, int(QPdfLinkModel::Role::Page)).toInt();
            l.location = model.data(index, int(QPdfLinkModel::Role::Location)).toPointF();
            if (!l.rect.isEmpty()) links << l;
        }
        it = a_links.insert(page, links);
    }
    return *it;
}

const PageView::Link* PageView::linkAt(QPoint pos)
{
    QPointF point;
    const int page = pageAt(pos, &point);
    if (page < 0) return nullptr;
    for (const Link& l : linksOn(page))
        if (l.rect.contains(point)) return &l;
    return nullptr;
}

QImage PageView::preview(int page)
{
    auto it = a_previews.find(page);
    if (it != a_previews.end()) return *it;
    const QSizeF points = a_document->pagePointSize(page);
    if (points.width() <= 0) return QImage();
    const QSize size(PreviewWidth, std::max(1, int(PreviewWidth * points.height() / points.width())));
    QPdfDocumentRenderOptions options;
    options.setRenderFlags(QPdfDocumentRenderOptions::RenderFlag::Annotations);
    QImage image = a_document->render(page, size, options);
    if (a_previews.size() > 64) a_previews.clear();
    a_previews.insert(page, image);
    return image;
}

QImage PageView::tile(int page, qreal scale, int tx, int ty, bool* cached)
{
    const QSizeF points = a_document->pagePointSize(page);
    const QSize full(std::max(1, int(std::lround(points.width() * scale))), std::max(1, int(std::lround(points.height() * scale))));
    const QRect rect = QRect(tx * TileSize, ty * TileSize, TileSize, TileSize).intersected(QRect(QPoint(0, 0), full));
    const QString key = tileKey(page, scale, tx, ty);
    if (QImage* hit = a_tiles.object(key)) {
        *cached = true;
        return *hit;
    }
    *cached = false;
    if (rect.isEmpty()) return QImage();
    QPdfDocumentRenderOptions options;
    options.setRenderFlags(QPdfDocumentRenderOptions::RenderFlag::Annotations);
    options.setScaledSize(full);
    options.setScaledClipRect(rect);
    QImage image = a_document->render(page, rect.size(), options);
    // (Its pixel ratio set once: drawn, the same image each time - the GPU's
    // texture of it kept; set at each paint, it was a copy each time.)
    image.setDevicePixelRatio(viewport()->devicePixelRatioF());
    ++a_tilesRendered;
    a_tiles.insert(key, new QImage(image), int(std::max<qsizetype>(1, image.sizeInBytes() / 1024)));
    QList<qreal>& scales = a_tileScales[page];
    if (scales.isEmpty() || scales.last() != scale) {
        scales.removeAll(scale);
        scales << scale;
        if (scales.size() > 6) scales.removeFirst();
    }
    return image;
}

void PageView::paintEvent(QPaintEvent* event)
{
    QPainter p(viewport());
    PainterSurface s(p);
    paintPages(s, event->rect());
}

void PageView::paintPages(Surface& p, const QRect& visible)
{
    const QPalette pal = palette();
    const bool dark = pal.color(QPalette::Window).lightness() < 128;
    p.fillRect(visible, dark ? pal.color(QPalette::Window).darker(135) : QColor(0xe4, 0xe6, 0xea));
    if (a_document == nullptr || a_pages.isEmpty()) return;

    const qreal dpr = viewport()->devicePixelRatioF();
    const qreal scale = pixelsPerPoint() * dpr;
    QElapsedTimer clock;
    clock.start();
    bool pending = false;   // tiles left for the next paint
    const QPoint off = offset();
    for (int i = 0; i < a_pages.size(); ++i) {
        const QRectF r = a_pages.at(i).translated(-QPointF(off));
        if (r.bottom() < visible.top() - 1) continue;
        if (r.top() > visible.bottom() + 1) break;
        // A shadow, the paper.
        p.fillRect(r.translated(1.5, 2).adjusted(-1, -1, 1, 1), QColor(0, 0, 0, dark ? 90 : 36));
        p.fillRect(r, Qt::white);
        const QRectF shown = r.intersected(QRectF(visible));
        if (shown.isEmpty()) continue;
        // The tiles in sight, in device pixels of the page.
        const QRectF local((shown.topLeft() - r.topLeft()) * dpr, shown.size() * dpr);
        const int tx0 = std::max(0, int(local.left()) / TileSize), tx1 = int(local.right()) / TileSize;
        const int ty0 = std::max(0, int(local.top()) / TileSize), ty1 = int(local.bottom()) / TileSize;
        QRegion missing;
        for (int ty = ty0; ty <= ty1; ++ty)
            for (int tx = tx0; tx <= tx1; ++tx) {
                const QRectF target(r.left() + tx * TileSize / dpr, r.top() + ty * TileSize / dpr, TileSize / dpr, TileSize / dpr);
                bool cached = false;
                QImage image;
                // Rendering takes its time: what does not fit in this
                // paint comes in the next. A zoom under way renders none:
                // its frames from the tiles at hand.
                if (a_tiles.contains(tileKey(i, scale, tx, ty)) || (!a_zooming && clock.elapsed() < RenderBudgetMs))
                    image = tile(i, scale, tx, ty, &cached);
                if (!image.isNull()) {
                    image.setDevicePixelRatio(dpr);
                    p.drawImage(target.topLeft(), image);
                } else {
                    if (!a_zooming) pending = true;
                    missing += target.intersected(r).toAlignedRect();
                }
            }
        if (!missing.isEmpty()) paintScaled(p, i, r, missing, scale, dpr);
        // The search's results, the current one more.
        if (a_search != nullptr && !a_search->searchString().isEmpty()) {
            const QPdfLink current = a_searchIndex >= 0 ? a_search->resultAtIndex(a_searchIndex) : QPdfLink();
            for (const QPdfLink& result : a_search->resultsOnPage(i)) {
                const bool isCurrent = current.isValid() && current.page() == i && current.rectangles() == result.rectangles();
                for (const QRectF& box : result.rectangles()) {
                    const QRectF on(toViewport(i, box.topLeft()), toViewport(i, box.bottomRight()));
                    p.fillRect(on.adjusted(-1, -1, 1, 1), isCurrent ? QColor(255, 140, 0, 150) : QColor(255, 225, 0, 110));
                }
            }
        }
        // The selection.
        if (a_selectionPage == i && !a_selectionBounds.isEmpty()) {
            QColor c = pal.color(QPalette::Highlight);
            c.setAlpha(90);
            for (const QPolygonF& poly : std::as_const(a_selectionBounds)) {
                QPolygonF on;
                for (const QPointF& pt : poly) on << toViewport(i, pt);
                p.fillPolygon(on, c);
            }
        }
    }
    if (pending) QTimer::singleShot(0, viewport(), qOverload<>(&QWidget::update));
}

void PageView::paintScaled(Surface& p, int page, const QRectF& r, const QRegion& missing, qreal scale, qreal dpr)
{
    // (A tile is the page's ink alone, transparent between - on the white
    // paper drawn: each part from one source, the nearest scale's tiles,
    // then the next's where they have none, the preview in what is left.
    // Laid over each other the text showed twice, the preview a halo.)
    QList<qreal> scales = a_tileScales.value(page);
    scales.removeAll(scale);
    // Nearest first, in proportion: twice the scale as near as half.
    std::sort(scales.begin(), scales.end(), [scale](qreal a, qreal b) { return std::abs(std::log(a / scale)) < std::abs(std::log(b / scale)); });
    QRegion left = missing;
    for (int n = 0; n < std::min<qsizetype>(scales.size(), 2) && !left.isEmpty(); ++n) {
        const qreal s = scales.at(n);
        // What is left in device pixels of the page at s, and its tiles.
        const qreal k = s / scale;
        const QRectF bounds = QRectF(left.boundingRect()).intersected(r);
        const QRectF at(((bounds.topLeft() - r.topLeft()) * dpr) * k, bounds.size() * dpr * k);
        const int tx0 = std::max(0, int(at.left()) / TileSize), tx1 = int(at.right()) / TileSize;
        const int ty0 = std::max(0, int(at.top()) / TileSize), ty1 = int(at.bottom()) / TileSize;
        QRegion covered;
        p.setClip(left);
        for (int ty = ty0; ty <= ty1; ++ty)
            for (int tx = tx0; tx <= tx1; ++tx) {
                const QImage* image = a_tiles.object(tileKey(page, s, tx, ty));
                if (image == nullptr) continue;
                const QRectF target(r.topLeft() + QPointF(tx, ty) * (TileSize / (k * dpr)), QSizeF(image->size()) / (k * dpr));
                p.drawImage(target, *image);
                covered += target.toAlignedRect();
                ++a_tilesScaled;
            }
        left -= covered;
    }
    // The preview: a part no tile at a near scale has.
    if (!left.isEmpty()) {
        const QImage low = preview(page);
        p.setClip(left);
        if (!low.isNull()) p.drawImage(r, low);
    }
    p.setClip(QRegion());
}

void PageView::resizeEvent(QResizeEvent* event)
{
    QAbstractScrollArea::resizeEvent(event);
    const int page = a_currentPage;
    const QPointF at = a_pages.isEmpty() ? QPointF() : QPointF(0, (verticalScrollBar()->value() - pagePoints(page).top()) / pixelsPerPoint());
    const qreal before = a_zoom;
    relayout();
    if (a_zoom != before && !a_pages.isEmpty()) {
        verticalScrollBar()->setValue(int(std::lround(pagePoints(page).top() + at.y() * pixelsPerPoint())));
        emit zoomChanged(a_zoom);
    }
    updateCurrentPage();
}

void PageView::scrollContentsBy(int, int)
{
    // (Scrolled by the user: a gesture's point taken again.)
    if (!a_placing) a_zoomPage = -1;
    updateCurrentPage();
    viewport()->update();
}

void PageView::wheelEvent(QWheelEvent* event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        const int delta = event->angleDelta().y();
        if (delta != 0) {
            const qreal factor = std::pow(1.0015, delta);
            // A mouse wheel's notch (no phase, no pixels): a step, in
            // frames. A trackpad's scroll: as it comes, its tiles when it
            // stops.
            if (event->phase() == Qt::NoScrollPhase && event->pixelDelta().isNull()) {
                zoomSmoothly(factor, event->position().toPoint());
            } else {
                zoomGesture(factor, event->position().toPoint());
            }
        }
        event->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(event);
}

bool PageView::viewportEvent(QEvent* event)
{
    if (event->type() == QEvent::NativeGesture) {
        auto* g = static_cast<QNativeGestureEvent*>(event);
        if (g->gestureType() == Qt::ZoomNativeGesture) {
            zoomGesture(1.0 + g->value(), g->position().toPoint());
            return true;
        }
        // (The fingers lifted: the tiles at once.)
        if (g->gestureType() == Qt::EndNativeGesture && a_zooming) settle();
    }
    return QAbstractScrollArea::viewportEvent(event);
}

void PageView::mousePressEvent(QMouseEvent* event)
{
    a_pressPos = event->position().toPoint();
    if (event->button() == Qt::MiddleButton) {
        a_drag = Drag::Pan;
        a_panStart = event->globalPosition().toPoint();
        a_panScroll = offset();
        viewport()->setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    if (const Link* l = linkAt(a_pressPos)) {
        a_drag = Drag::Link;
        a_pressedLink = l;
        return;
    }
    a_pressPage = pageAt(a_pressPos, &a_pressPoint);
    if (a_pressPage < 0) {
        // Beside the pages: the view is moved.
        a_drag = Drag::Pan;
        a_panStart = event->globalPosition().toPoint();
        a_panScroll = offset();
        clearSelection();
        return;
    }
    a_drag = Drag::Select;
    clearSelection();
}

void PageView::mouseMoveEvent(QMouseEvent* event)
{
    const QPoint pos = event->position().toPoint();
    switch (a_drag) {
    case Drag::Pan: {
        const QPoint d = event->globalPosition().toPoint() - a_panStart;
        horizontalScrollBar()->setValue(a_panScroll.x() - d.x());
        verticalScrollBar()->setValue(a_panScroll.y() - d.y());
        return;
    }
    case Drag::Select: {
        if ((pos - a_pressPos).manhattanLength() < QApplication::startDragDistance()) return;
        // Within the page it began on.
        const QRectF r = pagePoints(a_pressPage).translated(-QPointF(offset()));
        const QPointF clamped(std::clamp<qreal>(pos.x(), r.left(), r.right()), std::clamp<qreal>(pos.y(), r.top(), r.bottom()));
        const QPointF to = (clamped - r.topLeft()) / pixelsPerPoint();
        selectText(a_pressPage, a_pressPoint, to);
        // Near the edge: scrolled on.
        if (pos.y() < 0) verticalScrollBar()->setValue(verticalScrollBar()->value() + pos.y() / 2);
        else if (pos.y() > viewport()->height()) verticalScrollBar()->setValue(verticalScrollBar()->value() + (pos.y() - viewport()->height()) / 2);
        return;
    }
    default: break;
    }
    // Over a link: a hand, and where it leads.
    const Link* l = linkAt(pos);
    viewport()->setCursor(l != nullptr ? Qt::PointingHandCursor : (pageAt(pos) >= 0 ? Qt::IBeamCursor : Qt::ArrowCursor));
    viewport()->setToolTip(l == nullptr ? QString()
                           : l->url.isValid() ? l->url.toString()
                                              : tr("Page %1").arg(l->page + 1));
}

void PageView::mouseReleaseEvent(QMouseEvent* event)
{
    const Drag drag = a_drag;
    a_drag = Drag::None;
    viewport()->unsetCursor();
    if (drag == Drag::Link && event->button() == Qt::LeftButton) {
        const Link* l = linkAt(event->position().toPoint());
        if (l != nullptr && l == a_pressedLink) {
            if (l->url.isValid()) emit urlActivated(l->url);
            else if (l->page >= 0) goToPage(l->page, l->location);
        }
    }
    a_pressedLink = nullptr;
    QAbstractScrollArea::mouseReleaseEvent(event);
}

void PageView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) return;
    QPointF point;
    const int page = pageAt(event->position().toPoint(), &point);
    if (page >= 0) selectWordAt(page, point);
}

void PageView::keyPressEvent(QKeyEvent* event)
{
    if (a_pages.isEmpty()) {
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }
    switch (event->key()) {
    case Qt::Key_Home: goToPage(0); return;
    case Qt::Key_End: goToPage(int(a_pages.size()) - 1); return;
    case Qt::Key_Space:
        verticalScrollBar()->triggerAction(event->modifiers() & Qt::ShiftModifier ? QAbstractSlider::SliderPageStepSub
                                                                                  : QAbstractSlider::SliderPageStepAdd);
        return;
    case Qt::Key_Escape:
        clearSelection();
        return;
    default: break;
    }
    if (event->matches(QKeySequence::Copy)) {
        copySelection();
        return;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void PageView::contextMenuEvent(QContextMenuEvent* event)
{
    emit menuRequested(event->globalPos());
}

} // namespace qucs_s::pdf

// ----------------------------------------------------------------------
// PdfDoc

using qucs_s::pdf::GlyphIcon;
using qucs_s::pdf::PageView;
using qucs_s::pdf::glyph;
using qucs_s::pdf::ThumbnailWidth;

namespace {

const char* const kSidebarKey = "PdfViewer/sidebar";

QToolButton* toolButton(QWidget* parent, const QIcon& icon, const QString& tip)
{
    auto* b = new QToolButton(parent);
    b->setIcon(icon);
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setIconSize(QSize(16, 16));
    b->setFocusPolicy(Qt::NoFocus);
    b->setProperty("pdfTool", true);
    return b;
}

} // namespace

PdfDoc::PdfDoc(QucsApp* app, const QString& name) : QFrame(), QucsDoc(app, name)
{
    // No dataset, no data display, no script: it is not simulated.
    a_DataSet.clear();
    a_DataDisplay.clear();
    a_Script.clear();
    a_source = a_DocName;
    a_pdf = new QPdfDocument(this);
    a_search = new QPdfSearchModel(this);
    a_search->setDocument(a_pdf);
    a_bookmarks = new QPdfBookmarkModel(this);
    a_bookmarks->setDocument(a_pdf);
    buildUi();
    a_view->setDocument(a_pdf);
    a_view->setSearchModel(a_search);

    a_watcher = new QFileSystemWatcher(this);
    a_reloadTimer = new QTimer(this);
    a_reloadTimer->setSingleShot(true);
    a_reloadTimer->setInterval(400);
    connect(a_reloadTimer, &QTimer::timeout, this, [this] {
        if (QFileInfo::exists(a_DocName)) reload();
    });
    connect(a_watcher, &QFileSystemWatcher::fileChanged, this, [this] {
        // Written anew (maybe replaced): watched again, read when it is done.
        if (QFileInfo::exists(a_DocName)) {
            if (!a_watcher->files().contains(a_DocName)) a_watcher->addPath(a_DocName);
            a_reloadTimer->start();
            return;
        }
        // Deleted - to be written again later, a report made anew: its
        // folder watched until it is there again (a watch of the file
        // itself ends with it). Shown as it was meanwhile, and said so.
        const QString folder = QFileInfo(a_DocName).absolutePath();
        if (!a_watcher->directories().contains(folder)) a_watcher->addPath(folder);
        if (a_App != nullptr)
            a_App->statusBar()->showMessage(tr("%1 was deleted: it is shown as it was, and loaded again when it is written again.")
                                                .arg(QFileInfo(a_DocName).fileName()), 8000);
    });
    connect(a_watcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString& folder) {
        if (!QFileInfo::exists(a_DocName)) return;
        a_watcher->removePath(folder);
        if (!a_watcher->files().contains(a_DocName)) a_watcher->addPath(a_DocName);
        a_reloadTimer->start();
    });
    a_thumbTimer = new QTimer(this);
    a_thumbTimer->setInterval(0);
    connect(a_thumbTimer, &QTimer::timeout, this, &PdfDoc::renderSomeThumbnails);
    connect(a_search, &QAbstractItemModel::rowsInserted, this, &PdfDoc::updateSearchLabel);
    connect(a_search, &QAbstractItemModel::modelReset, this, &PdfDoc::updateSearchLabel);
}

PdfDoc::~PdfDoc()
{
    // The view and the models let go of the document before it goes.
    a_view->setSearchModel(nullptr);
    a_view->setDocument(nullptr);
    // (The search model gone first: handed a null document, Qt's connects
    // to it and warns.)
    delete a_search;
    a_search = nullptr;
    a_bookmarks->setDocument(nullptr);
}

void PdfDoc::buildUi()
{
    setFrameShape(QFrame::NoFrame);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // The toolbar.
    a_toolbar = new QWidget(this);
    a_toolbar->setObjectName(QStringLiteral("pdfToolbar"));
    auto* bar = new QHBoxLayout(a_toolbar);
    bar->setContentsMargins(6, 3, 6, 3);
    bar->setSpacing(2);
    a_sidebarButton = toolButton(a_toolbar, glyph(GlyphIcon::Sidebar), tr("Show or hide the pages and the outline"));
    a_sidebarButton->setCheckable(true);
    a_prevButton = toolButton(a_toolbar, glyph(GlyphIcon::Up), tr("Previous page"));
    a_nextButton = toolButton(a_toolbar, glyph(GlyphIcon::Down), tr("Next page"));
    a_pageField = new QLineEdit(a_toolbar);
    a_pageField->setAlignment(Qt::AlignCenter);
    a_pageField->setFixedWidth(a_pageField->fontMetrics().horizontalAdvance(QStringLiteral("00000")) + 12);
    a_pageField->setToolTip(tr("The page: type one and press Enter"));
    a_pageCount = new QLabel(a_toolbar);
    a_zoomOutButton = toolButton(a_toolbar, glyph(GlyphIcon::Minus), tr("Zoom out"));
    a_zoomInButton = toolButton(a_toolbar, glyph(GlyphIcon::Plus), tr("Zoom in"));
    a_zoomBox = new QComboBox(a_toolbar);
    a_zoomBox->setEditable(true);
    a_zoomBox->setInsertPolicy(QComboBox::NoInsert);
    a_zoomBox->addItem(tr("Fit Width"), -1.0);
    a_zoomBox->addItem(tr("Fit Page"), -2.0);
    for (int z : {50, 75, 100, 125, 150, 200, 300, 400, 800}) a_zoomBox->addItem(QStringLiteral("%1%").arg(z), z / 100.0);
    a_zoomBox->setMinimumContentsLength(8);
    a_zoomBox->setFocusPolicy(Qt::ClickFocus);
    a_findButton = toolButton(a_toolbar, glyph(GlyphIcon::Search), tr("Find in the document"));
    a_menuButton = toolButton(a_toolbar, glyph(GlyphIcon::More), tr("More"));
    a_menuButton->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(a_menuButton);
    menu->addAction(tr("Open with the System's Viewer"), this, &PdfDoc::openExternally);
    menu->addAction(tr("Show in File Manager"), this, &PdfDoc::revealInFileManager);
    menu->addAction(tr("Copy Path"), this, [this] { QApplication::clipboard()->setText(QDir::toNativeSeparators(a_DocName)); });
    menu->addSeparator();
    menu->addAction(tr("Reload"), this, [this] { reload(); });
    a_menuButton->setMenu(menu);

    bar->addWidget(a_sidebarButton);
    bar->addSpacing(8);
    bar->addWidget(a_prevButton);
    bar->addWidget(a_nextButton);
    bar->addWidget(a_pageField);
    bar->addWidget(a_pageCount);
    bar->addSpacing(12);
    bar->addWidget(a_zoomOutButton);
    bar->addWidget(a_zoomBox);
    bar->addWidget(a_zoomInButton);
    bar->addStretch(1);
    bar->addWidget(a_findButton);
    bar->addWidget(a_menuButton);
    layout->addWidget(a_toolbar);

    // The search, under it when asked for.
    a_searchBar = new QWidget(this);
    a_searchBar->setObjectName(QStringLiteral("pdfSearch"));
    auto* findRow = new QHBoxLayout(a_searchBar);
    findRow->setContentsMargins(6, 2, 6, 3);
    findRow->setSpacing(2);
    a_searchField = new QLineEdit(a_searchBar);
    a_searchField->setPlaceholderText(tr("Find in the document"));
    a_searchField->setClearButtonEnabled(true);
    auto* findPrev = toolButton(a_searchBar, glyph(GlyphIcon::Up), tr("Previous match (Shift+Enter)"));
    auto* findNextButton = toolButton(a_searchBar, glyph(GlyphIcon::Down), tr("Next match (Enter)"));
    a_searchLabel = new QLabel(a_searchBar);
    a_searchLabel->setMinimumWidth(a_searchLabel->fontMetrics().horizontalAdvance(QStringLiteral("0000 of 0000")));
    auto* findClose = toolButton(a_searchBar, glyph(GlyphIcon::Close), tr("Close (Escape)"));
    findRow->addWidget(a_searchField, 1);
    findRow->addWidget(findPrev);
    findRow->addWidget(findNextButton);
    findRow->addWidget(a_searchLabel);
    findRow->addWidget(findClose);
    a_searchBar->hide();
    layout->addWidget(a_searchBar);

    // The pages, the sidebar beside them; or why there are none.
    a_stack = new QStackedWidget(this);
    a_splitter = new QSplitter(Qt::Horizontal, a_stack);
    a_splitter->setChildrenCollapsible(false);
    a_side = new QTabWidget(a_splitter);
    a_side->setDocumentMode(true);
    a_thumbnails = new QListWidget(a_side);
    // One under the other, each page's number under it.
    a_thumbnails->setViewMode(QListView::IconMode);
    a_thumbnails->setFlow(QListView::TopToBottom);
    a_thumbnails->setWrapping(false);
    a_thumbnails->setMovement(QListView::Static);
    a_thumbnails->setResizeMode(QListView::Adjust);
    a_thumbnails->setIconSize(QSize(ThumbnailWidth, int(ThumbnailWidth * 1.42)));
    a_thumbnails->setSpacing(6);
    a_thumbnails->setUniformItemSizes(false);
    a_thumbnails->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    a_outline = new QTreeView(a_side);
    a_outline->setModel(a_bookmarks);
    a_outline->setHeaderHidden(true);
    a_outline->setEditTriggers(QAbstractItemView::NoEditTriggers);
    a_side->addTab(a_thumbnails, tr("Pages"));
    a_side->addTab(a_outline, tr("Outline"));
    a_view = new PageView(a_splitter);
    a_splitter->addWidget(a_side);
    a_splitter->addWidget(a_view);
    a_splitter->setStretchFactor(1, 1);
    a_splitter->setSizes({ThumbnailWidth + 60, 800});
    a_stack->addWidget(a_splitter);

    auto* problem = new QWidget(a_stack);
    auto* problemLayout = new QVBoxLayout(problem);
    problemLayout->addStretch(1);
    a_problemLabel = new QLabel(problem);
    a_problemLabel->setWordWrap(true);
    a_problemLabel->setAlignment(Qt::AlignCenter);
    a_problemLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    problemLayout->addWidget(a_problemLabel);
    auto* passwordRow = new QHBoxLayout();
    a_passwordField = new QLineEdit(problem);
    a_passwordField->setEchoMode(QLineEdit::Password);
    a_passwordField->setPlaceholderText(tr("Password"));
    a_passwordField->setMaximumWidth(260);
    auto* unlockButton = new QPushButton(tr("Open"), problem);
    passwordRow->addStretch(1);
    passwordRow->addWidget(a_passwordField);
    passwordRow->addWidget(unlockButton);
    passwordRow->addStretch(1);
    problemLayout->addLayout(passwordRow);
    problemLayout->addStretch(2);
    a_stack->addWidget(problem);
    layout->addWidget(a_stack, 1);
    setFocusProxy(a_view);

    const auto unlockNow = [this] {
        if (!unlock(a_passwordField->text())) a_passwordField->selectAll();
    };
    connect(a_passwordField, &QLineEdit::returnPressed, this, unlockNow);
    connect(unlockButton, &QPushButton::clicked, this, unlockNow);

    // What the controls do.
    const QucsSettingsFile settings;
    setSidebarShown(settings.value(QLatin1String(kSidebarKey), false).toBool());
    connect(a_sidebarButton, &QToolButton::toggled, this, [this](bool on) {
        setSidebarShown(on);
        QucsSettingsFile().setValue(QLatin1String(kSidebarKey), on);
    });
    connect(a_prevButton, &QToolButton::clicked, this, &PdfDoc::previousPage);
    connect(a_nextButton, &QToolButton::clicked, this, &PdfDoc::nextPage);
    connect(a_pageField, &QLineEdit::returnPressed, this, [this] {
        // A number, or a page's label ("iv").
        bool ok = false;
        int page = a_pageField->text().trimmed().toInt(&ok) - 1;
        if (!ok) page = a_pdf->pageIndexForLabel(a_pageField->text().trimmed());
        if (page >= 0 && page < pageCount()) a_view->goToPage(page);
        updatePageField();
        a_view->setFocus();
    });
    // (The steps in frames: zoomSmoothly.)
    connect(a_zoomOutButton, &QToolButton::clicked, this, [this] { a_view->zoomSmoothly(1 / 1.25); });
    connect(a_zoomInButton, &QToolButton::clicked, this, [this] { a_view->zoomSmoothly(1.25); });
    connect(a_zoomBox, &QComboBox::activated, this, [this](int index) {
        const double z = a_zoomBox->itemData(index).toDouble();
        if (z == -1.0) a_view->setFit(PageView::Fit::Width);
        else if (z == -2.0) a_view->setFit(PageView::Fit::Page);
        else a_view->zoomSmoothly(std::clamp(z, PageView::MinZoom, PageView::MaxZoom) / a_view->targetZoom());
        a_view->setFocus();
    });
    connect(a_zoomBox->lineEdit(), &QLineEdit::returnPressed, this, [this] {
        QString t = a_zoomBox->lineEdit()->text().trimmed();
        t.remove(QLatin1Char('%'));
        bool ok = false;
        const double z = t.toDouble(&ok);
        if (ok && z > 0) a_view->zoomSmoothly(std::clamp(z / 100.0, PageView::MinZoom, PageView::MaxZoom) / a_view->targetZoom());
        updateZoomBox();
        a_view->setFocus();
    });
    connect(a_findButton, &QToolButton::clicked, this, [this] { a_searchBar->isVisible() ? hideSearch() : showSearch(); });
    connect(a_searchField, &QLineEdit::textChanged, this, [this](const QString& text) { find(text); });
    connect(a_searchField, &QLineEdit::returnPressed, this, [this] {
        findNext(QApplication::keyboardModifiers() & Qt::ShiftModifier);
    });
    connect(findPrev, &QToolButton::clicked, this, [this] { findNext(true); });
    connect(findNextButton, &QToolButton::clicked, this, [this] { findNext(false); });
    connect(findClose, &QToolButton::clicked, this, &PdfDoc::hideSearch);
    auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), a_searchField);
    escape->setContext(Qt::WidgetShortcut);
    connect(escape, &QShortcut::activated, this, &PdfDoc::hideSearch);

    connect(a_view, &PageView::currentPageChanged, this, [this](int page) {
        updatePageField();
        const QSignalBlocker block(a_thumbnails);
        if (page < a_thumbnails->count()) {
            a_thumbnails->setCurrentRow(page);
            a_thumbnails->scrollToItem(a_thumbnails->item(page));
        }
    });
    connect(a_view, &PageView::zoomChanged, this, &PdfDoc::updateZoomBox);
    // A link of the PDF: a web page or mail to the system's; a document
    // Qucs-S opens, here; anything else - a program would run - only shown
    // in the file manager, or asked about (links.h).
    connect(a_view, &PageView::urlActivated, this, [this](const QUrl& url) {
        qucs_s::links::follow(this, url, QFileInfo(a_DocName).absolutePath(), [this](const QString& path) {
            if (a_App != nullptr) a_App->gotoPage(path);
        });
    });
    connect(a_view, &PageView::menuRequested, this, [this](QPoint at) {
        QMenu menu(this);
        QAction* copy = menu.addAction(tr("Copy"), this, &PdfDoc::copySelection);
        copy->setEnabled(a_view->hasSelection());
        copy->setShortcut(QKeySequence::Copy);
        menu.addAction(tr("Select the Page's Text"), this, &PdfDoc::selectAll);
        menu.addSeparator();
        menu.addAction(tr("Fit Width"), this, [this] { a_view->setFit(PageView::Fit::Width); });
        menu.addAction(tr("Fit Page"), this, [this] { a_view->setFit(PageView::Fit::Page); });
        menu.addAction(tr("Actual Size"), this, [this] { a_view->setZoom(1.0); });
        menu.addSeparator();
        menu.addAction(tr("Find..."), this, &PdfDoc::showSearch);
        menu.addAction(tr("Open with the System's Viewer"), this, &PdfDoc::openExternally);
        menu.exec(at);
    });
    connect(a_thumbnails, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0) a_view->goToPage(row);
    });
    connect(a_outline, &QTreeView::clicked, this, [this](const QModelIndex& index) {
        const int page = index.data(int(QPdfBookmarkModel::Role::Page)).toInt();
        a_view->goToPage(page, index.data(int(QPdfBookmarkModel::Role::Location)).toPointF());
    });
    restyle();
}

void PdfDoc::restyle()
{
    const QPalette pal = palette();
    const QColor line = pal.color(QPalette::Mid);
    const QString sheet = QStringLiteral(
                              "#pdfToolbar, #pdfSearch { border-bottom: 1px solid %1; }"
                              "QToolButton[pdfTool=\"true\"] { padding: 3px; border: none; border-radius: 4px; }"
                              "QToolButton[pdfTool=\"true\"]:hover { background: %2; }"
                              "QToolButton[pdfTool=\"true\"]:checked { background: %3; }"
                              "QToolButton[pdfTool=\"true\"]::menu-indicator { image: none; }")
                              .arg(line.name(), pal.color(QPalette::Midlight).name(), pal.color(QPalette::Mid).name());
    if (sheet != styleSheet()) setStyleSheet(sheet);
    a_view->viewport()->update();
}

void PdfDoc::changeEvent(QEvent* event)
{
    QFrame::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        QTimer::singleShot(0, this, &PdfDoc::restyle);
}

QWidget* PdfDoc::sidebar() const
{
    return a_side;
}

void PdfDoc::setSidebarShown(bool shown)
{
    a_side->setVisible(shown);
    const QSignalBlocker block(a_sidebarButton);
    a_sidebarButton->setChecked(shown);
    if (shown) fillThumbnails();
}

int PdfDoc::pageCount() const
{
    return a_pdf->status() == QPdfDocument::Status::Ready ? a_pdf->pageCount() : 0;
}

QString PdfDoc::title() const
{
    const QString t = a_pdf->metaData(QPdfDocument::MetaDataField::Title).toString().trimmed();
    return t.isEmpty() ? QFileInfo(a_DocName).fileName() : t;
}

void PdfDoc::setName(const QString& name)
{
    // Saved under another name (Save As): the file is copied by save().
    if (!a_DocName.isEmpty()) a_watcher->removePath(a_DocName);
    if (!a_watcher->directories().isEmpty()) a_watcher->removePaths(a_watcher->directories());   // (the old one's, deleted)
    a_DocName = QFileInfo(name).absoluteFilePath();
}

int PdfDoc::save()
{
    // A copy of the file, when it is saved under another name; nothing
    // else to write. The same file under another spelling (Report.pdf for
    // report.pdf on macOS, a path through a link) is not another name.
    if (a_source.isEmpty() || misc::isSameFile(a_source, a_DocName)) {
        if (!a_DocName.isEmpty() && QFileInfo::exists(a_DocName) && !a_watcher->files().contains(a_DocName))
            a_watcher->addPath(a_DocName);
        return 0;
    }
    // Copied beside and renamed over: a copy that fails leaves the file
    // that was there.
    QString error;
    if (!misc::copyFileOver(a_source, a_DocName, &error)) {
        misc::reportError(tr("%1 could not be copied to %2.").arg(QDir::toNativeSeparators(a_source), QDir::toNativeSeparators(a_DocName))
                          + QLatin1Char('\n') + error);
        return -1;
    }
    a_source = a_DocName;
    a_watcher->addPath(a_DocName);
    return 0;
}

bool PdfDoc::load()
{
    a_problem.clear();
    const QPdfDocument::Error error = a_pdf->load(a_DocName);
    if (!a_DocName.isEmpty() && QFileInfo::exists(a_DocName) && !a_watcher->files().contains(a_DocName))
        a_watcher->addPath(a_DocName);
    switch (error) {
    case QPdfDocument::Error::None:
        documentLoaded();
        return true;
    case QPdfDocument::Error::IncorrectPassword:
        showProblem(tr("%1 is protected by a password.").arg(QFileInfo(a_DocName).fileName()), true);
        return true;
    case QPdfDocument::Error::FileNotFound:
        misc::reportError(tr("There is no file %1.").arg(QDir::toNativeSeparators(a_DocName)));
        return false;
    case QPdfDocument::Error::UnsupportedSecurityScheme:
        misc::reportError(tr("%1 is protected in a way that cannot be opened here.").arg(QDir::toNativeSeparators(a_DocName)));
        return false;
    default:
        misc::reportError(tr("%1 cannot be read as a PDF document.").arg(QDir::toNativeSeparators(a_DocName)));
        return false;
    }
}

bool PdfDoc::unlock(const QString& password)
{
    a_pdf->setPassword(password);
    if (a_pdf->load(a_DocName) != QPdfDocument::Error::None) {
        showProblem(tr("That is not its password."), true);
        return false;
    }
    documentLoaded();
    return true;
}

void PdfDoc::showProblem(const QString& text, bool askPassword)
{
    a_problem = text;
    a_problemLabel->setText(text);
    a_passwordField->setVisible(askPassword);
    a_passwordField->parentWidget()->findChild<QPushButton*>()->setVisible(askPassword);
    a_stack->setCurrentIndex(1);
    a_toolbar->setEnabled(false);
    if (askPassword) a_passwordField->setFocus();
}

void PdfDoc::documentLoaded()
{
    a_problem.clear();
    a_stack->setCurrentIndex(0);
    a_toolbar->setEnabled(true);
    a_view->setDocument(a_pdf);
    a_pageCount->setText(tr("of %1").arg(a_pdf->pageCount()));
    a_side->setTabVisible(1, a_bookmarks->rowCount() > 0);
    a_thumbsDone = 0;
    a_thumbnails->clear();
    if (a_side->isVisible()) fillThumbnails();
    updatePageField();
    updateZoomBox();
    if (!a_searchField->text().isEmpty()) find(a_searchField->text());
}

bool PdfDoc::reload()
{
    // (A step's end, not the zoom and place it shows on its way: read
    // again during its frames, a file stayed there.)
    a_view->finishZoom();
    const int page = a_view->currentPage();
    const qreal zoom = a_view->zoom();
    const PageView::Fit fit = a_view->fit();
    const QPoint position(a_view->horizontalScrollBar()->value(), a_view->verticalScrollBar()->value());
    a_view->setDocument(nullptr);
    // (The search model keeps the document - the same one, read again: its
    // results cleared and searched again as its pages change. Handed a null
    // one, Qt's connects to it and warns at each reload.)
    a_bookmarks->setDocument(nullptr);
    const QString password = a_pdf->password();
    a_pdf->close();
    a_pdf->setPassword(password);
    const bool ok = a_pdf->load(a_DocName) == QPdfDocument::Error::None;
    a_bookmarks->setDocument(a_pdf);
    if (!ok) {
        showProblem(tr("%1 cannot be read as a PDF document now.").arg(QFileInfo(a_DocName).fileName()), false);
        return false;
    }
    documentLoaded();
    if (fit == PageView::Fit::None) a_view->setZoom(zoom);
    else a_view->setFit(fit);
    a_view->horizontalScrollBar()->setValue(position.x());
    a_view->verticalScrollBar()->setValue(position.y());
    if (a_view->currentPage() != page && page < pageCount()) a_view->goToPage(page);
    emit reloaded();
    return true;
}

void PdfDoc::updatePageField()
{
    const int page = a_view->currentPage();
    const QString label = pageCount() > 0 ? a_pdf->pageLabel(page) : QString();
    if (!a_pageField->hasFocus()) a_pageField->setText(label.isEmpty() ? QString::number(page + 1) : label);
    a_pageCount->setText(label.isEmpty() || label == QString::number(page + 1)
                             ? tr("of %1").arg(pageCount())
                             : tr("(%1 of %2)").arg(page + 1).arg(pageCount()));
    a_prevButton->setEnabled(page > 0);
    a_nextButton->setEnabled(page + 1 < pageCount());
}

void PdfDoc::updateZoomBox()
{
    const QSignalBlocker block(a_zoomBox);
    const QString text = a_view->fit() == PageView::Fit::Width ? tr("Fit Width")
                         : a_view->fit() == PageView::Fit::Page ? tr("Fit Page")
                                                                : QStringLiteral("%1%").arg(std::lround(a_view->zoom() * 100));
    a_zoomBox->setEditText(text);
    a_zoomOutButton->setEnabled(a_view->zoom() > PageView::MinZoom + 1e-9);
    a_zoomInButton->setEnabled(a_view->zoom() < PageView::MaxZoom - 1e-9);
}

void PdfDoc::fillThumbnails()
{
    const int n = pageCount();
    if (a_thumbnails->count() == n) {
        if (a_thumbsDone < n) a_thumbTimer->start();
        return;
    }
    a_thumbnails->clear();
    a_thumbsDone = 0;
    for (int i = 0; i < n; ++i) {
        const QSizeF s = a_pdf->pagePointSize(i);
        const int h = s.width() > 0 ? int(ThumbnailWidth * s.height() / s.width()) : int(ThumbnailWidth * 1.42);
        QPixmap blank(ThumbnailWidth, std::clamp(h, 8, 4 * ThumbnailWidth));
        blank.fill(Qt::white);
        const QString label = a_pdf->pageLabel(i);
        auto* item = new QListWidgetItem(QIcon(blank), label.isEmpty() ? QString::number(i + 1) : label);
        item->setTextAlignment(Qt::AlignHCenter);
        a_thumbnails->addItem(item);
    }
    const QSignalBlocker block(a_thumbnails);
    a_thumbnails->setCurrentRow(a_view->currentPage());
    a_thumbTimer->start();
}

void PdfDoc::renderSomeThumbnails()
{
    // A few at a time: a document of hundreds of pages opens at once.
    QElapsedTimer clock;
    clock.start();
    while (a_thumbsDone < a_thumbnails->count() && clock.elapsed() < 25) {
        const int i = a_thumbsDone++;
        const QSize size = a_thumbnails->item(i)->icon().availableSizes().value(0, QSize(ThumbnailWidth, ThumbnailWidth));
        const qreal dpr = devicePixelRatioF();
        QPdfDocumentRenderOptions options;
        options.setRenderFlags(QPdfDocumentRenderOptions::RenderFlag::Annotations);
        QImage image = a_pdf->render(i, size * dpr, options);
        QPixmap pm(size * dpr);
        pm.fill(Qt::white);
        {
            QPainter p(&pm);
            p.drawImage(0, 0, image);
        }
        pm.setDevicePixelRatio(dpr);
        a_thumbnails->item(i)->setIcon(QIcon(pm));
    }
    if (a_thumbsDone >= a_thumbnails->count()) a_thumbTimer->stop();
}

void PdfDoc::nextPage()
{
    a_view->goToPage(a_view->currentPage() + 1);
}

void PdfDoc::previousPage()
{
    a_view->goToPage(a_view->currentPage() - 1);
}

void PdfDoc::firstPage()
{
    a_view->goToPage(0);
}

void PdfDoc::lastPage()
{
    a_view->goToPage(pageCount() - 1);
}

void PdfDoc::showSearch()
{
    a_searchBar->show();
    a_searchField->setFocus();
    a_searchField->selectAll();
}

void PdfDoc::hideSearch()
{
    a_searchBar->hide();
    a_search->setSearchString(QString());
    a_view->setCurrentSearchResult(-1);
    a_view->setFocus();
}

void PdfDoc::find(const QString& text)
{
    if (a_searchField->text() != text) {
        const QSignalBlocker block(a_searchField);
        a_searchField->setText(text);
    }
    a_search->setSearchString(text);
    a_view->setCurrentSearchResult(-1);
    updateSearchLabel();
}

QList<PdfDoc::Hit> PdfDoc::findAll(const QString& text)
{
    QList<Hit> hits;
    a_searchBar->show();
    find(text);
    if (text.isEmpty()) return hits;
    // (Each page searched now - the model would go through them in time.)
    for (int p = 0; p < pageCount(); ++p)
        for (const QPdfLink& link : a_search->resultsOnPage(p)) {
            QRectF all;
            for (const QRectF& r : link.rectangles()) all = all.isNull() ? r : all.united(r);
            hits << Hit{p, all};
        }
    if (!hits.isEmpty())
        for (int i = 0; i < a_search->rowCount(QModelIndex()); ++i)
            if (a_search->resultAtIndex(i).page() == hits.first().page) {
                a_view->setCurrentSearchResult(i);
                break;
            }
    updateSearchLabel();
    return hits;
}

int PdfDoc::destination(const QString& title, QPointF* location, QString* found) const
{
    const QString wanted = title.trimmed();
    if (wanted.isEmpty()) return -1;
    QList<QModelIndex> all;
    std::function<void(const QModelIndex&)> walk = [&](const QModelIndex& parent) {
        for (int r = 0; r < a_bookmarks->rowCount(parent); ++r) {
            const QModelIndex i = a_bookmarks->index(r, 0, parent);
            all << i;
            walk(i);
        }
    };
    walk(QModelIndex());
    auto titleOf = [](const QModelIndex& i) { return i.data(int(QPdfBookmarkModel::Role::Title)).toString().simplified(); };
    for (int how = 0; how < 3; ++how)
        for (const QModelIndex& i : std::as_const(all)) {
            const QString t = titleOf(i);
            const bool match = how == 0 ? t.compare(wanted, Qt::CaseInsensitive) == 0
                             : how == 1 ? t.startsWith(wanted, Qt::CaseInsensitive)
                                        : t.contains(wanted, Qt::CaseInsensitive);
            if (!match) continue;
            if (location) *location = i.data(int(QPdfBookmarkModel::Role::Location)).toPointF();
            if (found) *found = t;
            return i.data(int(QPdfBookmarkModel::Role::Page)).toInt();
        }
    return -1;
}

QStringList PdfDoc::outlineTitles(int most) const
{
    QStringList out;
    std::function<void(const QModelIndex&, int)> walk = [&](const QModelIndex& parent, int depth) {
        for (int r = 0; r < a_bookmarks->rowCount(parent) && out.size() < most; ++r) {
            const QModelIndex i = a_bookmarks->index(r, 0, parent);
            out << QString(depth * 2, QLatin1Char(' ')) + i.data(int(QPdfBookmarkModel::Role::Title)).toString().simplified();
            walk(i, depth + 1);
        }
    };
    walk(QModelIndex(), 0);
    return out;
}

QImage PdfDoc::render(int page, const QRectF& region, qreal scale) const
{
    return qucs_s::pdf::renderPage(a_pdf, page, region, scale);
}

QImage qucs_s::pdf::renderPage(QPdfDocument* a_pdf, int page, const QRectF& region, qreal scale)
{
    if (a_pdf == nullptr || page < 0 || page >= a_pdf->pageCount() || !(scale > 0)) return {};
    const QSizeF size = a_pdf->pagePointSize(page) * scale;
    QPdfDocumentRenderOptions options;
    options.setRenderFlags(QPdfDocumentRenderOptions::RenderFlag::Annotations);
    QImage image = a_pdf->render(page, size.toSize(), options);
    if (image.isNull()) return image;
    // On white: a page leaves what it does not paint transparent.
    QImage paper(image.size(), QImage::Format_RGB32);
    paper.fill(Qt::white);
    {
        QPainter painter(&paper);
        painter.drawImage(0, 0, image);
    }
    if (region.isEmpty()) return paper;
    const QRect part = QRectF(region.topLeft() * scale, region.size() * scale).toAlignedRect().intersected(paper.rect());
    return part.isEmpty() ? QImage() : paper.copy(part);
}

QRectF qucs_s::pdf::regionOf(const QList<double>& c, const QSizeF& pageSize)
{
    if (c.size() != 4) return {};
    bool fractions = true;
    for (double v : c) fractions = fractions && v >= 0 && v <= 1;
    const double sx = fractions ? pageSize.width() : 1, sy = fractions ? pageSize.height() : 1;
    return QRectF(QPointF(std::min(c[0], c[2]) * sx, std::min(c[1], c[3]) * sy), QPointF(std::max(c[0], c[2]) * sx, std::max(c[1], c[3]) * sy));
}

int PdfDoc::searchResultCount() const
{
    return a_search->searchString().isEmpty() ? 0 : a_search->rowCount(QModelIndex());
}

int PdfDoc::currentSearchResult() const
{
    return a_view->currentSearchResult();
}

void PdfDoc::findNext(bool backwards)
{
    const int n = searchResultCount();
    if (n == 0) {
        updateSearchLabel();
        return;
    }
    int index = a_view->currentSearchResult();
    if (index < 0) {
        // The first from the page in sight on (or the last before it).
        const int page = a_view->currentPage();
        index = backwards ? n - 1 : 0;
        for (int i = 0; i < n; ++i) {
            const int p = a_search->resultAtIndex(i).page();
            if (!backwards && p >= page) {
                index = i;
                break;
            }
            if (backwards && p <= page) index = i;
        }
    } else {
        index = (index + (backwards ? n - 1 : 1)) % n;
    }
    a_view->setCurrentSearchResult(index);
    updateSearchLabel();
}

void PdfDoc::updateSearchLabel()
{
    const int n = searchResultCount();
    const int current = a_view->currentSearchResult();
    if (a_search->searchString().isEmpty()) a_searchLabel->clear();
    else if (n == 0) a_searchLabel->setText(tr("not found"));
    else if (current < 0) a_searchLabel->setText(n == 1 ? tr("1 match") : tr("%1 matches").arg(n));
    else a_searchLabel->setText(tr("%1 of %2").arg(current + 1).arg(n));
}

void PdfDoc::copySelection()
{
    a_view->copySelection();
}

void PdfDoc::selectAll()
{
    a_view->selectPage(a_view->currentPage());
}

void PdfDoc::openExternally()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(a_DocName));
}

void PdfDoc::revealInFileManager()
{
    qucs_s::links::reveal(a_DocName);
}

void PdfDoc::becomeCurrent(bool)
{
    // Nothing to undo in a document that is only read.
    if (a_App != nullptr) {
        a_App->undo->setEnabled(false);
        a_App->redo->setEnabled(false);
    }
    a_view->setFocus();
}

double PdfDoc::zoomBy(double factor)
{
    // View > Zoom In and Out: a step each, as the toolbar's, in frames -
    // the zoom it reaches.
    a_view->zoomSmoothly(factor > 1 ? 1.25 : factor < 1 ? 1 / 1.25 : 1.0);
    return a_view->targetZoom();
}

void PdfDoc::showAll()
{
    a_view->setFit(PageView::Fit::Page);
}

void PdfDoc::zoomToSelection()
{
    a_view->setFit(PageView::Fit::Width);
}

void PdfDoc::showNoZoom()
{
    a_view->setZoom(1.0);
}

void PdfDoc::print(QPrinter* printer, QPainter* painter, bool printAll, bool)
{
    // Each page on a sheet, as large as it goes; drawn at 300 dpi at
    // most (an A4 page at a printer's 1200 would be half a gigabyte);
    // "all", or the page in sight.
    const int n = pageCount();
    if (n == 0) return;
    const int from = printAll ? 0 : a_view->currentPage();
    const int to = printAll ? n - 1 : from;
    for (int page = from; page <= to; ++page) {
        if (page > from && !printer->newPage()) return;
        const QRect area = painter->viewport();
        const QSizeF points = a_pdf->pagePointSize(page);
        if (points.isEmpty() || area.isEmpty()) continue;
        const qreal scale = std::min(area.width() / points.width(), area.height() / points.height());
        const QSizeF target = points * scale;
        const qreal dpi = std::min<qreal>(300, printer->resolution());
        const QSize size = (points * dpi / 72.0).toSize();
        QPdfDocumentRenderOptions options;
        options.setRenderFlags(QPdfDocumentRenderOptions::RenderFlag::Annotations);
        const QImage image = a_pdf->render(page, size, options);
        QImage paper(size, QImage::Format_RGB32);
        paper.fill(Qt::white);
        {
            QPainter p(&paper);
            p.drawImage(0, 0, image);
        }
        const QRectF where(area.left() + (area.width() - target.width()) / 2, area.top(), target.width(), target.height());
        painter->drawImage(where, paper);
    }
}
