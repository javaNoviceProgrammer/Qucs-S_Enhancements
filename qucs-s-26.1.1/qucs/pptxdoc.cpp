/*
 * pptxdoc.cpp - a PowerPoint presentation (.pptx) in a tab
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "pptxdoc.h"

#include "links.h"
#include "main.h"
#include "misc.h"
#include "pptx.h"
#include "qucs.h"
#include "settings.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QIconEngine>
#include <QImageWriter>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPrinter>
#include <QSaveFile>
#include <QScrollBar>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>

#include <algorithm>
#include <cmath>

using qucs_s::pptx::Presentation;
using qucs_s::pptx::SlideShow;
using qucs_s::pptx::SlideView;

namespace {

constexpr int ThumbnailWidth = 150;
constexpr int Gap = 16;   // around and between slides (pixels)
const char* const kNotesKey = "Viewers/pptxNotes";
const char* const kSidebarKey = "Viewers/pptxSidebar";

// The toolbar's icons, drawn in the text's colour of the moment.
class Glyph : public QIconEngine
{
public:
    enum Kind { Sidebar, Previous, Next, Minus, Plus, Notes, Play, Find, More, Close };
    explicit Glyph(Kind kind) : a_kind(kind) {}
    QIconEngine* clone() const override { return new Glyph(a_kind); }
    void paint(QPainter* p, const QRect& rect, QIcon::Mode mode, QIcon::State) override
    {
        const QColor c =
            QApplication::palette().color(mode == QIcon::Disabled ? QPalette::Disabled : QPalette::Active, QPalette::WindowText);
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const qreal s = std::min(rect.width(), rect.height());
        p->translate(rect.center().x() - s / 2 + 0.5, rect.center().y() - s / 2 + 0.5);
        p->scale(s / 16.0, s / 16.0);
        p->setPen(QPen(c, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p->setBrush(Qt::NoBrush);
        switch (a_kind) {
        case Sidebar:
            p->drawRoundedRect(QRectF(2.5, 3.5, 11, 9), 1.2, 1.2);
            p->drawLine(QPointF(6, 3.5), QPointF(6, 12.5));
            break;
        case Previous: p->drawPolyline(QPolygonF{QPointF(10, 4), QPointF(6, 8), QPointF(10, 12)}); break;
        case Next: p->drawPolyline(QPolygonF{QPointF(6, 4), QPointF(10, 8), QPointF(6, 12)}); break;
        case Minus: p->drawLine(QPointF(4, 8), QPointF(12, 8)); break;
        case Plus:
            p->drawLine(QPointF(4, 8), QPointF(12, 8));
            p->drawLine(QPointF(8, 4), QPointF(8, 12));
            break;
        case Notes:
            p->drawRoundedRect(QRectF(2.5, 2.5, 11, 11), 1.2, 1.2);
            p->drawLine(QPointF(5, 9), QPointF(11, 9));
            p->drawLine(QPointF(5, 11.2), QPointF(9, 11.2));
            p->drawLine(QPointF(2.5, 6.5), QPointF(13.5, 6.5));
            break;
        case Play:
            p->setBrush(c);
            p->drawPolygon(QPolygonF{QPointF(5, 3.5), QPointF(12.5, 8), QPointF(5, 12.5)});
            break;
        case Find:
            p->drawEllipse(QPointF(7, 7), 3.8, 3.8);
            p->drawLine(QPointF(9.8, 9.8), QPointF(13, 13));
            break;
        case More:
            p->setBrush(c);
            p->setPen(Qt::NoPen);
            for (qreal x : {4.0, 8.0, 12.0}) p->drawEllipse(QPointF(x, 8), 1.2, 1.2);
            break;
        case Close:
            p->drawLine(QPointF(4.5, 4.5), QPointF(11.5, 11.5));
            p->drawLine(QPointF(11.5, 4.5), QPointF(4.5, 11.5));
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

QIcon glyph(Glyph::Kind kind)
{
    return QIcon(new Glyph(kind));
}

QToolButton* toolButton(QWidget* parent, const QIcon& icon, const QString& tip)
{
    auto* b = new QToolButton(parent);
    b->setIcon(icon);
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setIconSize(QSize(16, 16));
    b->setFocusPolicy(Qt::NoFocus);
    b->setProperty("pptxTool", true);
    return b;
}

// The usual scales, as the zoom box lists them.
const QList<int> kScales{10, 25, 50, 75, 100, 125, 150, 200, 300, 400, 600, 800};

} // namespace

// ---------------------------------------------------------------------------
// SlideView

namespace qucs_s::pptx {

SlideView::SlideView(QWidget* parent) : QAbstractScrollArea(parent)
{
    setObjectName(QStringLiteral("pptxSlides"));
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    // (Some 300 MB of drawn slides at most, the least recent let go.)
    a_cache.setMaxCost(300 * 1024);
    viewport()->setAutoFillBackground(false);
    verticalScrollBar()->setSingleStep(24);
    horizontalScrollBar()->setSingleStep(24);
}

void SlideView::setPresentation(std::shared_ptr<const Presentation> presentation)
{
    a_presentation = std::move(presentation);
    clearCache();
    relayout();
    updateCurrent();
    viewport()->update();
}

void SlideView::clearCache()
{
    a_cache.clear();
}

QRectF SlideView::contentRect(int index) const
{
    if (a_presentation == nullptr) return QRectF();
    const QSizeF s = a_presentation->slideSize() * a_zoom;
    const qreal x = std::max(qreal(Gap), (a_content.width() - s.width()) / 2);
    return QRectF(x, Gap + index * (s.height() + Gap), s.width(), s.height());
}

QRect SlideView::slideRect(int index) const
{
    return contentRect(index).translated(-horizontalScrollBar()->value(), -verticalScrollBar()->value()).toAlignedRect();
}

int SlideView::slideAt(QPoint pos) const
{
    if (a_presentation == nullptr) return -1;
    for (int i = 0; i < a_presentation->slideCount(); ++i)
        if (slideRect(i).contains(pos)) return i;
    return -1;
}

void SlideView::relayout()
{
    if (a_relaying) return;
    a_relaying = true;
    const QSize view = viewport()->size();
    if (a_presentation != nullptr && a_fit != Fit::None) {
        const QSizeF s = a_presentation->slideSize();
        // The scroll bar's width left aside, as it comes with many slides.
        const qreal width = std::max(40, view.width() - 2 * Gap);
        qreal zoom = width / s.width();
        if (a_fit == Fit::Slide) zoom = std::min(zoom, std::max(40, view.height() - 2 * Gap) / s.height());
        zoom = std::clamp(zoom, MinZoom, MaxZoom);
        if (!qFuzzyCompare(zoom, a_zoom)) {
            a_zoom = zoom;
            emit zoomChanged(a_zoom);
        }
    }
    const int n = a_presentation != nullptr ? a_presentation->slideCount() : 0;
    const QSizeF s = a_presentation != nullptr ? a_presentation->slideSize() * a_zoom : QSizeF();
    a_content = QSize(std::max(view.width(), int(std::ceil(s.width())) + 2 * Gap), Gap + int(std::ceil(n * (s.height() + Gap))));
    horizontalScrollBar()->setRange(0, std::max(0, a_content.width() - view.width()));
    horizontalScrollBar()->setPageStep(view.width());
    verticalScrollBar()->setRange(0, std::max(0, a_content.height() - view.height()));
    verticalScrollBar()->setPageStep(view.height());
    a_relaying = false;
}

void SlideView::setZoom(qreal zoom)
{
    zoom = std::clamp(zoom, MinZoom, MaxZoom);
    const int keep = a_current;
    // The slide in sight kept in sight, where it was.
    const QRect before = slideRect(keep);
    const qreal offset = before.isValid() && before.height() > 0 ? qreal(-before.top()) / before.height() : 0;
    a_fit = Fit::None;
    const bool changed = !qFuzzyCompare(zoom, a_zoom);
    a_zoom = zoom;
    relayout();
    const QRectF after = contentRect(keep);
    verticalScrollBar()->setValue(int(after.top() + offset * after.height()));
    if (changed) emit zoomChanged(a_zoom);
    viewport()->update();
}

void SlideView::zoomStep(bool in)
{
    const qreal now = a_zoom * 100;
    int next = in ? kScales.last() : kScales.first();
    if (in) {
        for (int s : kScales)
            if (s > now + 0.5) {
                next = s;
                break;
            }
    } else {
        for (int i = int(kScales.size()) - 1; i >= 0; --i)
            if (kScales.at(i) < now - 0.5) {
                next = kScales.at(i);
                break;
            }
    }
    setZoom(next / 100.0);
}

void SlideView::setFit(Fit fit)
{
    a_fit = fit;
    const int keep = a_current;
    relayout();
    goToSlide(keep);
    viewport()->update();
}

void SlideView::goToSlide(int index)
{
    if (a_presentation == nullptr || a_presentation->slideCount() == 0) return;
    index = std::clamp(index, 0, a_presentation->slideCount() - 1);
    verticalScrollBar()->setValue(int(contentRect(index).top() - Gap));
    // (At the end of the scroll the slide may be lower down: it is the one.)
    if (a_current != index) {
        a_current = index;
        emit currentSlideChanged(index);
    }
    viewport()->update();
}

void SlideView::showRegion(int index, const QRectF& region)
{
    if (a_presentation == nullptr || a_presentation->slideCount() == 0 || region.isEmpty()) return;
    index = std::clamp(index, 0, a_presentation->slideCount() - 1);
    const QSize view = viewport()->size();
    const qreal zoom = std::clamp(std::min((view.width() - 2.0 * Gap) / region.width(), (view.height() - 2.0 * Gap) / region.height()), MinZoom, MaxZoom);
    a_fit = Fit::None;
    const bool changed = !qFuzzyCompare(zoom, a_zoom);
    a_zoom = zoom;
    relayout();
    // Its middle at the view's.
    const QRectF slide = contentRect(index);
    const QPointF middle = slide.topLeft() + region.center() * a_zoom;
    horizontalScrollBar()->setValue(int(std::lround(middle.x() - view.width() / 2.0)));
    verticalScrollBar()->setValue(int(std::lround(middle.y() - view.height() / 2.0)));
    // (The slide shown is the one asked, wherever a third of the view is.)
    if (a_current != index) {
        a_current = index;
        emit currentSlideChanged(index);
    }
    if (changed) emit zoomChanged(a_zoom);
    viewport()->update();
}

void SlideView::updateCurrent()
{
    if (a_presentation == nullptr || a_presentation->slideCount() == 0) return;
    const int y = verticalScrollBar()->value() + viewport()->height() / 3;
    const qreal each = a_presentation->slideSize().height() * a_zoom + Gap;
    const int index = std::clamp(int((y - Gap / 2) / std::max(1.0, each)), 0, a_presentation->slideCount() - 1);
    if (index != a_current) {
        a_current = index;
        emit currentSlideChanged(index);
    }
}

QImage SlideView::drawn(int index, qreal scale, bool* fresh)
{
    const QString key = QStringLiteral("%1:%2").arg(index).arg(scale, 0, 'f', 4);
    *fresh = false;
    if (QImage* kept = a_cache.object(key)) return *kept;
    QImage image = a_presentation->render(index, scale);
    *fresh = true;
    ++a_drawn;
    if (!image.isNull()) a_cache.insert(key, new QImage(image), std::max<qsizetype>(1, image.sizeInBytes() / 1024));
    return image;
}

void SlideView::paintEvent(QPaintEvent*)
{
    QPainter p(viewport());
    const QColor back = palette().color(QPalette::Window).darker(palette().color(QPalette::Window).lightness() > 128 ? 112 : 70);
    p.fillRect(viewport()->rect(), back);
    if (a_presentation == nullptr) return;
    const qreal dpr = devicePixelRatioF();
    const qreal scale = a_zoom * dpr;
    QElapsedTimer clock;
    clock.start();
    bool later = false;
    for (int i = 0; i < a_presentation->slideCount(); ++i) {
        const QRect r = slideRect(i);
        if (r.bottom() < 0) continue;
        if (r.top() > viewport()->height()) break;
        // A shadow, the slide on it.
        p.fillRect(r.translated(2, 3), QColor(0, 0, 0, 50));
        QImage image;
        bool fresh = false;
        const QString key = QStringLiteral("%1:%2").arg(i).arg(scale, 0, 'f', 4);
        // (Slides drawn a quarter second at most a frame: the rest the next.)
        if (a_cache.contains(key) || clock.elapsed() < 250) image = drawn(i, scale, &fresh);
        if (image.isNull()) {
            p.fillRect(r, Qt::white);
            later = true;
            continue;
        }
        p.drawImage(QRectF(r), image);
    }
    if (later) QTimer::singleShot(0, viewport(), qOverload<>(&QWidget::update));
}

void SlideView::resizeEvent(QResizeEvent* event)
{
    QAbstractScrollArea::resizeEvent(event);
    const int keep = a_current;
    relayout();
    if (a_fit != Fit::None) goToSlide(keep);
}

void SlideView::scrollContentsBy(int, int)
{
    updateCurrent();
    viewport()->update();
}

void SlideView::wheelEvent(QWheelEvent* event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        const int steps = event->angleDelta().y();
        if (steps != 0) zoomStep(steps > 0);
        event->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(event);
}

void SlideView::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_PageDown:
    case Qt::Key_Space: goToSlide(a_current + 1); return;
    case Qt::Key_PageUp:
    case Qt::Key_Backspace: goToSlide(a_current - 1); return;
    case Qt::Key_Home: goToSlide(0); return;
    case Qt::Key_End: goToSlide(a_presentation != nullptr ? a_presentation->slideCount() - 1 : 0); return;
    default: break;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void SlideView::contextMenuEvent(QContextMenuEvent* event)
{
    emit menuRequested(event->globalPos(), slideAt(event->pos()));
}

void SlideView::mouseDoubleClickEvent(QMouseEvent* event)
{
    const int slide = slideAt(event->position().toPoint());
    if (slide >= 0) emit slideDoubleClicked(slide);
}

bool SlideView::viewportEvent(QEvent* event)
{
    // A pinch zooms.
    if (event->type() == QEvent::NativeGesture) {
        auto* g = static_cast<QNativeGestureEvent*>(event);
        if (g->gestureType() == Qt::ZoomNativeGesture) {
            setZoom(a_zoom * (1 + g->value()));
            return true;
        }
    }
    return QAbstractScrollArea::viewportEvent(event);
}

// ---------------------------------------------------------------------------
// SlideShow

SlideShow::SlideShow(std::shared_ptr<const Presentation> presentation, int from, QWidget* parent)
    : QWidget(parent, Qt::Window), a_presentation(std::move(presentation))
{
    setObjectName(QStringLiteral("pptxShow"));
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setCursor(Qt::BlankCursor);
    setFocusPolicy(Qt::StrongFocus);
    setWindowTitle(tr("Slide Show"));
    const int n = a_presentation != nullptr ? a_presentation->slideCount() : 0;
    a_current = std::clamp(from, 0, std::max(0, n - 1));
    // Not on a hidden slide: the next one shown.
    if (n > 0 && a_presentation->slide(a_current).hidden) a_current = step(a_current, 1);
}

int SlideShow::step(int from, int by) const
{
    const int n = a_presentation != nullptr ? a_presentation->slideCount() : 0;
    for (int i = from + by; i >= 0 && i < n; i += by)
        if (!a_presentation->slide(i).hidden) return i;
    return by > 0 ? n : -1;   // past the end, before the start
}

void SlideShow::next()
{
    if (a_done) {
        close();
        return;
    }
    const int to = step(a_current, 1);
    if (a_presentation == nullptr || to >= a_presentation->slideCount()) {
        // The end: black, then a click more ends it.
        a_done = true;
        update();
        return;
    }
    a_current = to;
    update();
}

void SlideShow::previous()
{
    if (a_done) {
        a_done = false;
        update();
        return;
    }
    const int to = step(a_current, -1);
    if (to >= 0) {
        a_current = to;
        update();
    }
}

void SlideShow::goTo(int index)
{
    if (a_presentation == nullptr || index < 0 || index >= a_presentation->slideCount()) return;
    a_done = false;
    a_current = index;
    update();
}

void SlideShow::first()
{
    goTo(step(-1, 1));
}

void SlideShow::last()
{
    goTo(step(a_presentation != nullptr ? a_presentation->slideCount() : 0, -1));
}

void SlideShow::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (a_presentation == nullptr || a_done) {
        if (a_done) {
            p.setPen(QColor(160, 160, 160));
            p.drawText(rect().adjusted(0, 0, 0, -rect().height() / 2), Qt::AlignHCenter | Qt::AlignBottom,
                       tr("End of the slide show: click or press a key to leave it."));
        }
        return;
    }
    const QSizeF s = a_presentation->slideSize();
    const qreal fit = std::min(width() / s.width(), height() / s.height());
    const QSizeF shown = s * fit;
    const QRectF at((width() - shown.width()) / 2, (height() - shown.height()) / 2, shown.width(), shown.height());
    const qreal dpr = devicePixelRatioF();
    if (a_imageIndex != a_current || a_imageSize != size()) {
        a_image = a_presentation->render(a_current, fit * dpr);
        a_imageIndex = a_current;
        a_imageSize = size();
    }
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(at, a_image);
}

void SlideShow::keyPressEvent(QKeyEvent* event)
{
    const int key = event->key();
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        a_typed += QChar(QLatin1Char('0' + (key - Qt::Key_0)));
        return;
    }
    if ((key == Qt::Key_Return || key == Qt::Key_Enter) && !a_typed.isEmpty()) {
        goTo(a_typed.toInt() - 1);
        a_typed.clear();
        return;
    }
    a_typed.clear();
    switch (key) {
    case Qt::Key_Escape: close(); return;
    case Qt::Key_Right:
    case Qt::Key_Down:
    case Qt::Key_PageDown:
    case Qt::Key_Space:
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_N: next(); return;
    case Qt::Key_Left:
    case Qt::Key_Up:
    case Qt::Key_PageUp:
    case Qt::Key_Backspace:
    case Qt::Key_P: previous(); return;
    case Qt::Key_Home: first(); return;
    case Qt::Key_End: last(); return;
    default: break;
    }
    QWidget::keyPressEvent(event);
}

void SlideShow::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::RightButton) previous();
    else next();
}

void SlideShow::wheelEvent(QWheelEvent* event)
{
    if (event->angleDelta().y() < 0) next();
    else if (event->angleDelta().y() > 0) previous();
}

void SlideShow::closeEvent(QCloseEvent* event)
{
    emit ended(a_current);
    QWidget::closeEvent(event);
}

} // namespace qucs_s::pptx

// ---------------------------------------------------------------------------
// PptxDoc

PptxDoc::PptxDoc(QucsApp* app, const QString& name) : QFrame(), QucsDoc(app, name)
{
    // No dataset, no data display, no script: it is not simulated.
    a_DataSet.clear();
    a_DataDisplay.clear();
    a_Script.clear();
    a_source = a_DocName;
    buildUi();
    a_thumbTimer = new QTimer(this);
    a_thumbTimer->setInterval(0);
    connect(a_thumbTimer, &QTimer::timeout, this, &PptxDoc::renderSomeThumbnails);

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
        // Deleted - to be written again (a deck made anew): its folder
        // watched until it is there again, shown as it was meanwhile.
        const QString folder = QFileInfo(a_DocName).absolutePath();
        if (!a_watcher->directories().contains(folder)) a_watcher->addPath(folder);
        if (a_App != nullptr)
            a_App->statusBar()->showMessage(tr("%1 was deleted: it is shown as it was, and read again when it is written again.")
                                                .arg(QFileInfo(a_DocName).fileName()),
                                            8000);
    });
    connect(a_watcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString& folder) {
        if (!QFileInfo::exists(a_DocName)) return;
        a_watcher->removePath(folder);
        if (!a_watcher->files().contains(a_DocName)) a_watcher->addPath(a_DocName);
        a_reloadTimer->start();
    });
}

PptxDoc::~PptxDoc()
{
    if (a_show) a_show->close();
}

void PptxDoc::buildUi()
{
    setFrameShape(QFrame::NoFrame);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // The toolbar.
    a_toolbar = new QWidget(this);
    a_toolbar->setObjectName(QStringLiteral("pptxToolbar"));
    auto* bar = new QHBoxLayout(a_toolbar);
    bar->setContentsMargins(6, 3, 6, 3);
    bar->setSpacing(2);
    a_sidebarButton = toolButton(a_toolbar, glyph(Glyph::Sidebar), tr("The slides' thumbnails"));
    a_sidebarButton->setCheckable(true);
    a_prevButton = toolButton(a_toolbar, glyph(Glyph::Previous), tr("The slide before (Page Up)"));
    a_nextButton = toolButton(a_toolbar, glyph(Glyph::Next), tr("The next slide (Page Down)"));
    a_slideField = new QLineEdit(a_toolbar);
    a_slideField->setObjectName(QStringLiteral("pptxSlideField"));
    a_slideField->setFixedWidth(fontMetrics().horizontalAdvance(QStringLiteral("0000")) + 12);
    a_slideField->setAlignment(Qt::AlignCenter);
    a_slideField->setToolTip(tr("The slide in sight: type a number and press Return to go to it"));
    a_slideCount = new QLabel(a_toolbar);
    // (Named for Claude's get_ui and a screen reader: by labels of their
    // own, hidden - not the "of N" beside them.)
    a_slideField->setAccessibleName(tr("Slide"));
    auto* slideName = new QLabel(tr("Slide"), a_toolbar);
    slideName->setBuddy(a_slideField);
    slideName->hide();
    a_zoomOutButton = toolButton(a_toolbar, glyph(Glyph::Minus), tr("Zoom out"));
    a_zoomInButton = toolButton(a_toolbar, glyph(Glyph::Plus), tr("Zoom in"));
    a_zoomBox = new QComboBox(a_toolbar);
    a_zoomBox->setEditable(true);
    a_zoomBox->setInsertPolicy(QComboBox::NoInsert);
    a_zoomBox->addItem(tr("Fit Width"), -1.0);
    a_zoomBox->addItem(tr("Fit Slide"), -2.0);
    for (int z : kScales) a_zoomBox->addItem(QStringLiteral("%1%").arg(z), z / 100.0);
    a_zoomBox->setMinimumContentsLength(9);
    a_zoomBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    a_zoomBox->setFocusPolicy(Qt::ClickFocus);
    a_zoomBox->setToolTip(tr("The zoom: pick a fit or a scale, or type one"));
    a_zoomBox->setAccessibleName(tr("Zoom"));
    auto* zoomName = new QLabel(tr("Zoom"), a_toolbar);
    zoomName->setBuddy(a_zoomBox);
    zoomName->hide();
    a_notesButton = toolButton(a_toolbar, glyph(Glyph::Notes), tr("The speaker notes of the slide in sight"));
    a_notesButton->setCheckable(true);
    a_playButton = toolButton(a_toolbar, glyph(Glyph::Play), tr("Play the slides full screen from the one in sight (F5 from the first)"));
    a_findButton = toolButton(a_toolbar, glyph(Glyph::Find), tr("Find in the slides' text and notes (Ctrl+F)"));
    a_menuButton = toolButton(a_toolbar, glyph(Glyph::More), tr("More"));
    a_menuButton->setPopupMode(QToolButton::InstantPopup);
    a_moreMenu = new QMenu(a_menuButton);
    fillMenu(a_moreMenu);
    a_menuButton->setMenu(a_moreMenu);

    bar->addWidget(a_sidebarButton);
    bar->addSpacing(8);
    bar->addWidget(a_prevButton);
    bar->addWidget(a_slideField);
    bar->addWidget(a_slideCount);
    bar->addWidget(a_nextButton);
    bar->addSpacing(12);
    bar->addWidget(a_zoomOutButton);
    bar->addWidget(a_zoomBox);
    bar->addWidget(a_zoomInButton);
    bar->addStretch(1);
    bar->addWidget(a_notesButton);
    bar->addWidget(a_playButton);
    bar->addWidget(a_findButton);
    bar->addWidget(a_menuButton);
    layout->addWidget(a_toolbar);

    // The search bar, below it while searching.
    a_searchBar = new QWidget(this);
    a_searchBar->setObjectName(QStringLiteral("pptxSearch"));
    auto* search = new QHBoxLayout(a_searchBar);
    search->setContentsMargins(6, 3, 6, 3);
    a_searchField = new QLineEdit(a_searchBar);
    a_searchField->setPlaceholderText(tr("Find in the slides and notes"));
    a_searchField->setAccessibleName(tr("Find"));
    a_searchField->setClearButtonEnabled(true);
    a_searchLabel = new QLabel(a_searchBar);
    auto* up = toolButton(a_searchBar, glyph(Glyph::Previous), tr("The slide with it before (Shift+Return)"));
    auto* down = toolButton(a_searchBar, glyph(Glyph::Next), tr("The next slide with it (Return)"));
    auto* closeSearch = toolButton(a_searchBar, glyph(Glyph::Close), tr("Close the find bar (Escape)"));
    search->addWidget(a_searchField, 1);
    search->addWidget(a_searchLabel);
    search->addWidget(up);
    search->addWidget(down);
    search->addWidget(closeSearch);
    a_searchBar->hide();
    layout->addWidget(a_searchBar);
    connect(a_searchField, &QLineEdit::textChanged, this, [this](const QString& text) { find(text); });
    connect(a_searchField, &QLineEdit::returnPressed, this, [this] { findNext(QApplication::keyboardModifiers() & Qt::ShiftModifier); });
    connect(up, &QToolButton::clicked, this, [this] { findNext(true); });
    connect(down, &QToolButton::clicked, this, [this] { findNext(false); });
    connect(closeSearch, &QToolButton::clicked, this, &PptxDoc::hideSearch);
    a_searchField->installEventFilter(this);

    // The body: thumbnails | slides, the notes under them.
    a_stack = new QStackedWidget(this);
    a_splitter = new QSplitter(Qt::Horizontal, a_stack);
    a_splitter->setChildrenCollapsible(false);
    a_thumbnails = new QListWidget(a_splitter);
    a_thumbnails->setObjectName(QStringLiteral("pptxThumbnails"));
    a_thumbnails->setAccessibleName(tr("Slides"));
    a_thumbnails->setViewMode(QListView::IconMode);
    a_thumbnails->setFlow(QListView::TopToBottom);
    a_thumbnails->setWrapping(false);
    a_thumbnails->setMovement(QListView::Static);
    a_thumbnails->setResizeMode(QListView::Adjust);
    a_thumbnails->setIconSize(QSize(ThumbnailWidth, ThumbnailWidth * 9 / 16));
    a_thumbnails->setSpacing(6);
    a_thumbnails->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    a_thumbnails->setMinimumWidth(ThumbnailWidth + 40);
    a_thumbnails->setMaximumWidth(ThumbnailWidth + 60);
    a_body = new QSplitter(Qt::Vertical, a_splitter);
    a_body->setChildrenCollapsible(false);
    a_view = new SlideView(a_body);
    a_notesPane = new QWidget(a_body);
    auto* notesLayout = new QVBoxLayout(a_notesPane);
    notesLayout->setContentsMargins(8, 4, 8, 4);
    notesLayout->setSpacing(2);
    a_notesTitle = new QLabel(a_notesPane);
    a_notesTitle->setObjectName(QStringLiteral("pptxNotesTitle"));
    a_notes = new QTextBrowser(a_notesPane);
    a_notes->setObjectName(QStringLiteral("pptxNotes"));
    a_notes->setFrameShape(QFrame::NoFrame);
    a_notes->setOpenExternalLinks(true);
    notesLayout->addWidget(a_notesTitle);
    notesLayout->addWidget(a_notes, 1);
    a_body->addWidget(a_view);
    a_body->addWidget(a_notesPane);
    a_body->setStretchFactor(0, 4);
    a_body->setStretchFactor(1, 1);
    a_body->setSizes({600, 140});
    a_splitter->addWidget(a_thumbnails);
    a_splitter->addWidget(a_body);
    a_splitter->setStretchFactor(1, 1);
    a_problemLabel = new QLabel(a_stack);
    a_problemLabel->setAlignment(Qt::AlignCenter);
    a_problemLabel->setWordWrap(true);
    a_problemLabel->setMargin(24);
    a_stack->addWidget(a_splitter);
    a_stack->addWidget(a_problemLabel);
    layout->addWidget(a_stack, 1);

    const QucsSettingsFile settings;
    setNotesShown(settings.value(QLatin1String(kNotesKey), true).toBool());
    setSidebarShown(settings.value(QLatin1String(kSidebarKey), true).toBool());

    connect(a_sidebarButton, &QToolButton::toggled, this, [this](bool on) {
        setSidebarShown(on);
        QucsSettingsFile().setValue(QLatin1String(kSidebarKey), on);
    });
    connect(a_notesButton, &QToolButton::toggled, this, [this](bool on) {
        setNotesShown(on);
        QucsSettingsFile().setValue(QLatin1String(kNotesKey), on);
    });
    connect(a_prevButton, &QToolButton::clicked, this, &PptxDoc::previousSlide);
    connect(a_nextButton, &QToolButton::clicked, this, &PptxDoc::nextSlide);
    connect(a_slideField, &QLineEdit::returnPressed, this, [this] {
        bool ok = false;
        const int n = a_slideField->text().trimmed().toInt(&ok);
        if (ok) goToSlide(n - 1);
        updateSlideField();
        a_view->setFocus();
    });
    connect(a_zoomOutButton, &QToolButton::clicked, this, [this] { a_view->zoomStep(false); });
    connect(a_zoomInButton, &QToolButton::clicked, this, [this] { a_view->zoomStep(true); });
    connect(a_zoomBox, &QComboBox::activated, this, [this](int i) {
        const double v = a_zoomBox->itemData(i).toDouble();
        if (v == -1.0) a_view->setFit(SlideView::Fit::Width);
        else if (v == -2.0) a_view->setFit(SlideView::Fit::Slide);
        else if (v > 0) a_view->setZoom(v);
        updateZoomBox();
        a_view->setFocus();
    });
    connect(a_zoomBox->lineEdit(), &QLineEdit::returnPressed, this, [this] {
        QString t = a_zoomBox->currentText().trimmed();
        t.remove(QLatin1Char('%'));
        bool ok = false;
        const double v = t.toDouble(&ok);
        if (ok && v > 0) a_view->setZoom(v / 100);
        updateZoomBox();
        a_view->setFocus();
    });
    connect(a_playButton, &QToolButton::clicked, this, [this] { startShow(-1); });
    connect(a_findButton, &QToolButton::clicked, this, &PptxDoc::showSearch);
    connect(a_view, &SlideView::currentSlideChanged, this, [this](int index) {
        updateSlideField();
        updateNotes();
        if (a_thumbnails->currentRow() != index) {
            const QSignalBlocker block(a_thumbnails);
            a_thumbnails->setCurrentRow(index);
        }
        emit currentSlideChanged(index);
    });
    connect(a_view, &SlideView::zoomChanged, this, &PptxDoc::updateZoomBox);
    connect(a_view, &SlideView::menuRequested, this, &PptxDoc::showMenu);
    connect(a_view, &SlideView::slideDoubleClicked, this, [this](int slide) { startShow(slide); });
    connect(a_thumbnails, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0 && row != a_view->currentSlide()) a_view->goToSlide(row);
    });
    connect(a_thumbnails, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) { startShow(a_thumbnails->row(item)); });

    // (Ctrl+F is the window's Find, which shows the search bar here; F5
    // and Shift+F5 are taken in event() while the focus is in the tab.)
    restyle();
}

void PptxDoc::restyle()
{
    const QPalette pal = palette();
    const QColor line = pal.color(QPalette::Mid);
    const QString sheet = QStringLiteral(
                              "#pptxToolbar { border-bottom: 1px solid %1; }"
                              "#pptxSearch { border-bottom: 1px solid %1; }"
                              "#pptxNotesTitle { color: %4; }"
                              "QToolButton[pptxTool=\"true\"] { padding: 3px; border: none; border-radius: 4px; }"
                              "QToolButton[pptxTool=\"true\"]:hover { background: %2; }"
                              "QToolButton[pptxTool=\"true\"]:checked { background: %3; }"
                              "QToolButton[pptxTool=\"true\"]::menu-indicator { image: none; }")
                              .arg(line.name(), pal.color(QPalette::Midlight).name(), pal.color(QPalette::Mid).name(),
                                   pal.color(QPalette::PlaceholderText).name());
    if (sheet != styleSheet()) setStyleSheet(sheet);
    if (a_view != nullptr) a_view->viewport()->update();
}

void PptxDoc::changeEvent(QEvent* event)
{
    QFrame::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        QTimer::singleShot(0, this, &PptxDoc::restyle);
}

namespace {

/// F5 (from the first slide) or Shift+F5 (from the one in sight): the
/// slide show's keys, as in PowerPoint.
bool isPlayKey(const QKeyEvent* key)
{
    return key->key() == Qt::Key_F5 && (key->modifiers() & ~(Qt::ShiftModifier | Qt::KeypadModifier)) == Qt::NoModifier;
}

} // namespace

bool PptxDoc::event(QEvent* event)
{
    // The window's F5 is Show Last Messages: an action of the same keys
    // here would leave both undone (ambiguous), so the keys are claimed
    // before the shortcuts are looked at, while the focus is in the tab.
    if (event->type() == QEvent::ShortcutOverride && isPlayKey(static_cast<QKeyEvent*>(event))) {
        event->accept();
        return true;
    }
    return QFrame::event(event);
}

void PptxDoc::keyPressEvent(QKeyEvent* event)
{
    if (isPlayKey(event)) {
        startShow(event->modifiers() & Qt::ShiftModifier ? -1 : 0);
        return;
    }
    QFrame::keyPressEvent(event);
}

bool PptxDoc::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == a_searchField && event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        hideSearch();
        return true;
    }
    return QFrame::eventFilter(watched, event);
}

// ----------------------------------------------------------------------
// Reading

bool PptxDoc::load()
{
    watch();
    if (!QFileInfo::exists(a_DocName)) {
        misc::reportError(tr("There is no file %1.").arg(QDir::toNativeSeparators(a_DocName)));
        return false;
    }
    QFile f(a_DocName);
    if (!f.open(QIODevice::ReadOnly)) {
        misc::reportError(tr("%1 cannot be read: %2").arg(QDir::toNativeSeparators(a_DocName), f.errorString()));
        return false;
    }
    const QByteArray bytes = f.readAll();
    if (bytes.isEmpty()) {
        misc::reportError(tr("%1 is empty (a file kept online, not downloaded?).").arg(QDir::toNativeSeparators(a_DocName)));
        return false;
    }
    QString why;
    auto presentation = Presentation::read(bytes, &why);
    if (presentation == nullptr) {
        misc::reportError(tr("%1 cannot be read as a presentation: %2").arg(QDir::toNativeSeparators(a_DocName), why));
        return false;
    }
    showPresentation(presentation, false);
    return true;
}

bool PptxDoc::reload()
{
    QFile f(a_DocName);
    QString why;
    std::shared_ptr<Presentation> presentation;
    if (!f.open(QIODevice::ReadOnly)) why = f.errorString();
    else {
        const QByteArray bytes = f.readAll();
        if (bytes.isEmpty()) why = tr("it is empty");
        else presentation = Presentation::read(bytes, &why);
    }
    if (presentation == nullptr) {
        showProblem(tr("%1 cannot be read as a presentation now: %2").arg(QFileInfo(a_DocName).fileName(), why));
        return false;
    }
    showPresentation(presentation, true);
    emit reloaded();
    return true;
}

void PptxDoc::showPresentation(std::shared_ptr<Presentation> presentation, bool keepView)
{
    const int slide = keepView ? a_view->currentSlide() : 0;
    a_presentation = std::move(presentation);
    a_problem.clear();
    a_stack->setCurrentIndex(0);
    a_toolbar->setEnabled(true);
    a_view->setPresentation(a_presentation);
    a_slideCount->setText(tr("of %1").arg(a_presentation->slideCount()));
    fillThumbnails();
    a_view->goToSlide(slide);
    updateSlideField();
    updateZoomBox();
    updateNotes();
    if (!a_searchField->text().isEmpty()) find(a_searchField->text());
}

void PptxDoc::showProblem(const QString& text)
{
    a_problem = text;
    a_problemLabel->setText(text);
    a_stack->setCurrentIndex(1);
    a_toolbar->setEnabled(false);
}

void PptxDoc::watch()
{
    if (!a_DocName.isEmpty() && QFileInfo::exists(a_DocName) && !a_watcher->files().contains(a_DocName)) a_watcher->addPath(a_DocName);
}

void PptxDoc::fillThumbnails()
{
    a_thumbTimer->stop();
    const QSignalBlocker block(a_thumbnails);
    a_thumbnails->clear();
    a_thumbsDone = 0;
    if (a_presentation == nullptr) return;
    const QSizeF s = a_presentation->slideSize();
    const QSize icon(ThumbnailWidth, std::max(20, int(ThumbnailWidth * s.height() / s.width())));
    a_thumbnails->setIconSize(icon);
    QPixmap blank(icon);
    blank.fill(Qt::white);
    for (int i = 0; i < a_presentation->slideCount(); ++i) {
        const qucs_s::pptx::SlideInfo& info = a_presentation->slide(i);
        auto* item = new QListWidgetItem(QIcon(blank), info.hidden ? tr("%1 (hidden)").arg(i + 1) : QString::number(i + 1));
        item->setToolTip(info.title.isEmpty() ? tr("Slide %1").arg(i + 1) : tr("Slide %1: %2").arg(i + 1).arg(info.title));
        item->setTextAlignment(Qt::AlignHCenter);
        a_thumbnails->addItem(item);
    }
    a_thumbnails->setCurrentRow(a_view->currentSlide());
    a_thumbTimer->start();
}

void PptxDoc::renderSomeThumbnails()
{
    // A few at a time: a deck of a hundred slides opens at once.
    QElapsedTimer clock;
    clock.start();
    const qreal dpr = devicePixelRatioF();
    while (a_presentation != nullptr && a_thumbsDone < a_thumbnails->count() && clock.elapsed() < 25) {
        const int i = a_thumbsDone++;
        const QSize size = a_thumbnails->iconSize();
        QImage image = a_presentation->render(i, size.width() * dpr / a_presentation->slideSize().width());
        if (a_presentation->slide(i).hidden) {
            // Hidden: dimmed.
            QPainter p(&image);
            p.fillRect(image.rect(), QColor(255, 255, 255, 140));
        }
        QPixmap pm = QPixmap::fromImage(image);
        pm.setDevicePixelRatio(dpr);
        a_thumbnails->item(i)->setIcon(QIcon(pm));
    }
    if (a_presentation == nullptr || a_thumbsDone >= a_thumbnails->count()) a_thumbTimer->stop();
}

void PptxDoc::updateSlideField()
{
    if (!a_slideField->hasFocus()) a_slideField->setText(a_presentation != nullptr && a_presentation->slideCount() > 0 ? QString::number(a_view->currentSlide() + 1) : QString());
    a_prevButton->setEnabled(a_presentation != nullptr && a_view->currentSlide() > 0);
    a_nextButton->setEnabled(a_presentation != nullptr && a_view->currentSlide() + 1 < a_presentation->slideCount());
}

void PptxDoc::updateZoomBox()
{
    const QSignalBlocker block(a_zoomBox);
    if (a_view->fit() == SlideView::Fit::Width) a_zoomBox->setCurrentIndex(0);
    else if (a_view->fit() == SlideView::Fit::Slide) a_zoomBox->setCurrentIndex(1);
    else {
        a_zoomBox->setCurrentIndex(-1);
        a_zoomBox->setEditText(QStringLiteral("%1%").arg(qRound(a_view->zoom() * 100)));
    }
    if (a_view->fit() != SlideView::Fit::None) a_zoomBox->setEditText(a_zoomBox->currentText());
}

void PptxDoc::updateNotes()
{
    if (a_presentation == nullptr || a_presentation->slideCount() == 0) {
        a_notesTitle->clear();
        a_notes->clear();
        return;
    }
    const int i = a_view->currentSlide();
    const qucs_s::pptx::SlideInfo& info = a_presentation->slide(i);
    QString title = info.title.isEmpty() ? tr("Slide %1").arg(i + 1) : tr("Slide %1: %2").arg(i + 1).arg(info.title);
    if (info.hidden) title += QLatin1Char(' ') + tr("(hidden in a slide show)");
    a_notesTitle->setText(title);
    QString html;
    if (info.notes.isEmpty()) html = QStringLiteral("<p style=\"color:gray\">%1</p>").arg(tr("No speaker notes.").toHtmlEscaped());
    else
        for (const QString& line : info.notes.split(QLatin1Char('\n'))) html += QStringLiteral("<p>%1</p>").arg(line.toHtmlEscaped());
    if (!info.notShown.isEmpty())
        html += QStringLiteral("<p style=\"color:gray\">%1</p>").arg(tr("Not drawn as PowerPoint draws it: %1.").arg(info.notShown.join(QStringLiteral("; "))).toHtmlEscaped());
    a_notes->setHtml(html);
}

void PptxDoc::showMenu(QPoint globalPos, int slide)
{
    if (a_presentation == nullptr) return;
    if (slide >= 0 && slide != a_view->currentSlide()) a_view->goToSlide(slide);
    QMenu menu(this);
    fillMenu(&menu);
    menu.exec(globalPos);
}

void PptxDoc::fillMenu(QMenu* menu)
{
    // The ⋯ menu and the right-click one alike: all the tab does that its
    // bar does not.
    QAction* copy = menu->addAction(tr("Copy the Slide as a Picture"), this, &PptxDoc::copySlide);
    // Shown, its keys the window's Copy's (which copies the slide here):
    // taken while the menu is open alone, not twice (ambiguous) otherwise.
    copy->setShortcut(QKeySequence::Copy);
    copy->setShortcutContext(Qt::WidgetShortcut);
    copy->setShortcutVisibleInContextMenu(true);
    menu->addAction(tr("Copy the Slide's Text"), this, &PptxDoc::copySlideText);
    menu->addAction(tr("Export the Slide as a Picture…"), this, &PptxDoc::exportSlide);
    menu->addAction(tr("Copy Path"), this, &PptxDoc::copyPath);
    menu->addSeparator();
    menu->addAction(tr("Play from This Slide"), this, [this] { startShow(-1); });
    menu->addAction(tr("Play from the First Slide"), this, [this] { startShow(0); });
    menu->addSeparator();
    menu->addAction(tr("Fit Width"), this, [this] { a_view->setFit(SlideView::Fit::Width); });
    menu->addAction(tr("Fit Slide"), this, [this] { a_view->setFit(SlideView::Fit::Slide); });
    menu->addSeparator();
    menu->addAction(tr("Open with the System's Viewer"), this, &PptxDoc::openExternally);
    menu->addAction(tr("Show in File Manager"), this, &PptxDoc::revealInFileManager);
    menu->addSeparator();
    menu->addAction(tr("Reload"), this, [this] { reload(); });
}

// ----------------------------------------------------------------------
// What it is, where it is

int PptxDoc::slideCount() const
{
    return a_presentation != nullptr ? a_presentation->slideCount() : 0;
}

int PptxDoc::currentSlide() const
{
    return a_view->currentSlide();
}

void PptxDoc::goToSlide(int index)
{
    a_view->goToSlide(index);
}

void PptxDoc::nextSlide()
{
    a_view->goToSlide(a_view->currentSlide() + 1);
}

void PptxDoc::previousSlide()
{
    a_view->goToSlide(a_view->currentSlide() - 1);
}

void PptxDoc::firstSlide()
{
    a_view->goToSlide(0);
}

void PptxDoc::lastSlide()
{
    a_view->goToSlide(slideCount() - 1);
}

QImage PptxDoc::slideImage(int index, qreal scale) const
{
    return a_presentation != nullptr ? a_presentation->render(index, scale) : QImage();
}

bool PptxDoc::notesShown() const
{
    return !a_notesPane->isHidden();
}

void PptxDoc::setNotesShown(bool shown)
{
    a_notesPane->setVisible(shown);
    const QSignalBlocker block(a_notesButton);
    a_notesButton->setChecked(shown);
}

bool PptxDoc::sidebarShown() const
{
    return !a_thumbnails->isHidden();
}

void PptxDoc::setSidebarShown(bool shown)
{
    a_thumbnails->setVisible(shown);
    const QSignalBlocker block(a_sidebarButton);
    a_sidebarButton->setChecked(shown);
}

// ----------------------------------------------------------------------
// Finding

QList<int> PptxDoc::find(const QString& text)
{
    a_hits.clear();
    a_hit = -1;
    if (a_presentation == nullptr || text.trimmed().isEmpty()) {
        a_searchLabel->clear();
        return a_hits;
    }
    for (const auto& [slide, count] : a_presentation->find(text)) a_hits << slide;
    if (a_hits.isEmpty()) {
        a_searchLabel->setText(tr("Not found"));
        return a_hits;
    }
    // The first from the slide in sight on.
    const int from = a_view->currentSlide();
    a_hit = 0;
    for (int i = 0; i < a_hits.size(); ++i)
        if (a_hits.at(i) >= from) {
            a_hit = i;
            break;
        }
    a_view->goToSlide(a_hits.at(a_hit));
    a_searchLabel->setText(a_hits.size() == 1 ? tr("On slide %1 alone").arg(a_hits.first() + 1)
                                              : tr("Slide %1: %2 of %3 slides with it").arg(a_hits.at(a_hit) + 1).arg(a_hit + 1).arg(a_hits.size()));
    return a_hits;
}

void PptxDoc::findNext(bool backwards)
{
    if (a_hits.isEmpty()) return;
    a_hit = (a_hit + (backwards ? -1 : 1) + int(a_hits.size())) % int(a_hits.size());
    a_view->goToSlide(a_hits.at(a_hit));
    a_searchLabel->setText(a_hits.size() == 1 ? tr("On slide %1 alone").arg(a_hits.first() + 1)
                                              : tr("Slide %1: %2 of %3 slides with it").arg(a_hits.at(a_hit) + 1).arg(a_hit + 1).arg(a_hits.size()));
}

void PptxDoc::setFindText(const QString& text)
{
    a_searchBar->show();
    if (a_searchField->text() == text) find(text);
    else a_searchField->setText(text);   // (found as typed: textChanged)
}

bool PptxDoc::findShown() const
{
    return !a_searchBar->isHidden();
}

QString PptxDoc::findText() const
{
    return a_searchField->text();
}

void PptxDoc::showRegion(int index, const QRectF& region)
{
    a_view->showRegion(index, region);
}

void PptxDoc::showSearch()
{
    a_searchBar->show();
    a_searchField->setFocus();
    a_searchField->selectAll();
}

void PptxDoc::hideSearch()
{
    // (The focus to the slides from the field alone: closed by Claude, it
    // stays where the user has it.)
    const bool had = a_searchField->hasFocus();
    a_searchBar->hide();
    if (had) a_view->setFocus();
}

// ----------------------------------------------------------------------
// Played

void PptxDoc::startShow(int from)
{
    if (a_presentation == nullptr || a_presentation->slideCount() == 0) return;
    if (a_show) a_show->close();
    auto* show = new SlideShow(a_presentation, from < 0 ? a_view->currentSlide() : from, nullptr);
    a_show = show;
    connect(show, &SlideShow::ended, this, [this](int last) { a_view->goToSlide(last); });
    // On the screen the window is on.
    if (QWindow* w = window()->windowHandle(); w != nullptr && w->screen() != nullptr) show->setScreen(w->screen());
    show->showFullScreen();
    show->raise();
    show->activateWindow();
    show->setFocus();
}

// ----------------------------------------------------------------------
// The slide, out

void PptxDoc::copySlide()
{
    if (a_presentation == nullptr) return;
    // As large as a slide is on a screen: 1920 pixels wide.
    const QImage image = a_presentation->render(a_view->currentSlide(), 1920 / a_presentation->slideSize().width());
    if (!image.isNull()) QApplication::clipboard()->setImage(image);
}

void PptxDoc::copySlideText()
{
    if (a_presentation == nullptr) return;
    const qucs_s::pptx::SlideInfo& info = a_presentation->slide(a_view->currentSlide());
    QString text = info.paragraphs.join(QLatin1Char('\n'));
    if (!info.notes.isEmpty()) text += QStringLiteral("\n\n") + tr("Notes:") + QLatin1Char('\n') + info.notes;
    QApplication::clipboard()->setText(text);
}

void PptxDoc::exportSlide()
{
    if (a_presentation == nullptr) return;
    const int i = a_view->currentSlide();
    const QString suggested = QFileInfo(a_DocName).absoluteDir().filePath(QStringLiteral("%1-slide%2.png").arg(QFileInfo(a_DocName).completeBaseName()).arg(i + 1));
    const QString file = QFileDialog::getSaveFileName(this, tr("Export the Slide"), suggested, tr("Pictures (*.png *.jpg *.jpeg *.bmp *.tiff *.webp)"));
    if (file.isEmpty()) return;
    const QImage image = a_presentation->render(i, 1920 / a_presentation->slideSize().width());
    QSaveFile out(file);
    QImageWriter writer(&out, QFileInfo(file).suffix().toLower().toLatin1());
    if (!out.open(QIODevice::WriteOnly) || !writer.write(image) || !out.commit())
        misc::reportError(tr("%1 could not be written: %2").arg(QDir::toNativeSeparators(file), writer.errorString()));
}

void PptxDoc::copyPath()
{
    QApplication::clipboard()->setText(QDir::toNativeSeparators(a_DocName));
}

void PptxDoc::openExternally()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(a_DocName));
}

void PptxDoc::revealInFileManager()
{
    qucs_s::links::reveal(a_DocName);
}

// ----------------------------------------------------------------------
// QucsDoc

void PptxDoc::becomeCurrent(bool)
{
    // Nothing to undo in a document that is only read.
    if (a_App != nullptr) {
        a_App->undo->setEnabled(false);
        a_App->redo->setEnabled(false);
    }
    a_view->setFocus();
}

double PptxDoc::zoomBy(double factor)
{
    // View > Zoom In and Out: to the next of the usual scales.
    if (factor > 1) a_view->zoomStep(true);
    else if (factor < 1) a_view->zoomStep(false);
    return a_view->zoom();
}

void PptxDoc::showAll()
{
    a_view->setFit(SlideView::Fit::Slide);
}

void PptxDoc::zoomToSelection()
{
    a_view->setFit(SlideView::Fit::Width);
}

void PptxDoc::showNoZoom()
{
    a_view->setZoom(1.0);
}

void PptxDoc::setName(const QString& name)
{
    // Saved under another name (Save As): the file is copied by save().
    if (!a_DocName.isEmpty()) a_watcher->removePath(a_DocName);
    if (!a_watcher->directories().isEmpty()) a_watcher->removePaths(a_watcher->directories());
    a_DocName = QFileInfo(name).absoluteFilePath();
}

int PptxDoc::save()
{
    // Nothing is changed in it: saved under another name, a copy.
    if (a_source.isEmpty() || misc::isSameFile(a_source, a_DocName)) {
        watch();
        return 0;
    }
    QString error;
    if (!misc::copyFileOver(a_source, a_DocName, &error)) {
        misc::reportError(tr("%1 could not be copied to %2.").arg(QDir::toNativeSeparators(a_source), QDir::toNativeSeparators(a_DocName))
                          + QLatin1Char('\n') + error);
        return -1;
    }
    a_source = a_DocName;
    watch();
    return 0;
}

void PptxDoc::print(QPrinter* printer, QPainter* painter, bool printAll, bool)
{
    // Each slide on a page of its own, as large as it goes (the slide in
    // sight alone unless all are printed).
    if (a_presentation == nullptr || printer == nullptr || painter == nullptr) return;
    const int first = printAll ? 0 : a_view->currentSlide();
    const int last = printAll ? a_presentation->slideCount() - 1 : a_view->currentSlide();
    for (int i = first; i <= last; ++i) {
        if (i > first) printer->newPage();
        const QRect area = painter->viewport();
        a_presentation->render(i, painter, QRectF(area));
    }
}
