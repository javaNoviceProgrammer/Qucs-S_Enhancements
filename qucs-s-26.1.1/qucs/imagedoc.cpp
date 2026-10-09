/*
 * imagedoc.cpp - a picture in a tab of Qucs-S: a PNG, a JPEG, an SVG - what
 *                Qt reads - zoomed and panned, turned, its pixels read, a
 *                part of it selected and copied, its frames played
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "imagedoc.h"

#include "links.h"
#include "misc.h"
#include "qucs.h"
#include "settings.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QBoxLayout>
#include <QClipboard>
#include <QColorSpace>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QIconEngine>
#include <QImageReader>
#include <QImageWriter>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMimeData>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPrinter>
#include <QSaveFile>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QSvgRenderer>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace qucs_s::image {

namespace {

constexpr int Margin = 12;   // around the picture, when it is larger than the view

// The scales zoomStep() goes through.
constexpr qreal kSteps[] = {0.01, 0.02, 0.03,  0.05, 0.0625, 0.08, 0.1, 0.125, 0.167, 0.25, 0.333, 0.5, 0.667, 0.75,
                            1.0,  1.25, 1.5,   2.0,  3.0,    4.0,  6.0, 8.0,   12.0,  16.0, 24.0,  32.0, 48.0, 64.0};

// A bitmap of more pixels than this is drawn small from its halves (one
// at most twice the size it is drawn at): drawn whole and smooth each
// time, a photo of 24 megapixels took its time at every scroll.
constexpr qint64 kHalvesFrom = qint64(1) << 20;
// An SVG is drawn whole at a scale up to this many device pixels; at a
// larger one, the part in sight (and some around it).
constexpr qint64 kSvgWhole = qint64(16) << 20;
// What a read may take: QImageReader's own limit is 256 MB, a photo of
// 64 megapixels.
constexpr int kReadLimitMB = 1024;
// The frames of an animation kept, at most - the first ones.
constexpr qint64 kFramesBytes = qint64(512) << 20;

// QImageReader's limit raised for the time of a read.
class AllocationLimit
{
public:
    AllocationLimit() : a_was(QImageReader::allocationLimit())
    {
        if (a_was != 0) QImageReader::setAllocationLimit(std::max(a_was, kReadLimitMB));
    }
    ~AllocationLimit() { QImageReader::setAllocationLimit(a_was); }
    AllocationLimit(const AllocationLimit&) = delete;
    AllocationLimit& operator=(const AllocationLimit&) = delete;

private:
    int a_was;
};

// One format's suffixes as one: "jpg" and "jpeg", "tif" and "tiff".
QString canonical(const QString& suffix)
{
    const QString s = suffix.toLower();
    if (s == QLatin1String("jpg") || s == QLatin1String("jpe") || s == QLatin1String("jfif")) return QStringLiteral("jpeg");
    if (s == QLatin1String("tif")) return QStringLiteral("tiff");
    return s;
}

// The formats without transparency: a picture with it is put on white.
bool opaqueFormat(const QString& suffix)
{
    static const QStringList opaque = {QStringLiteral("jpeg"), QStringLiteral("bmp"), QStringLiteral("ppm"),
                                       QStringLiteral("pgm"),  QStringLiteral("pbm"), QStringLiteral("xbm"),
                                       QStringLiteral("wbmp")};
    return opaque.contains(canonical(suffix));
}

QString text(const char* source)
{
    return QCoreApplication::translate("ImageDoc", source);
}

// What a format is called.
QString formatName(const QString& format)
{
    const QString f = format.toLower();
    if (f == QLatin1String("svgz")) return text(QT_TRANSLATE_NOOP("ImageDoc", "SVG, compressed"));
    if (f == QLatin1String("jpg")) return QStringLiteral("JPEG");
    if (f == QLatin1String("tif")) return QStringLiteral("TIFF");
    if (f == QLatin1String("heic") || f == QLatin1String("heif")) return QStringLiteral("HEIF");
    return f.toUpper();
}

// What its pixels are.
QString colourOf(const QImage& image)
{
    switch (image.format()) {
    case QImage::Format_Mono:
    case QImage::Format_MonoLSB: return text(QT_TRANSLATE_NOOP("ImageDoc", "black and white"));
    case QImage::Format_Indexed8:
        return image.hasAlphaChannel() ? text(QT_TRANSLATE_NOOP("ImageDoc", "%1 colours, transparent")).arg(image.colorCount())
                                       : text(QT_TRANSLATE_NOOP("ImageDoc", "%1 colours")).arg(image.colorCount());
    case QImage::Format_Grayscale8: return text(QT_TRANSLATE_NOOP("ImageDoc", "grey"));
    case QImage::Format_Grayscale16: return text(QT_TRANSLATE_NOOP("ImageDoc", "grey, 16 bits"));
    default: break;
    }
    const QPixelFormat pixel = image.pixelFormat();
    const bool alpha = image.hasAlphaChannel();
    if (pixel.typeInterpretation() == QPixelFormat::FloatingPoint) return alpha ? text(QT_TRANSLATE_NOOP("ImageDoc", "RGBA, floating point")) : text(QT_TRANSLATE_NOOP("ImageDoc", "RGB, floating point"));
    if (pixel.bitsPerPixel() >= 48) return alpha ? text(QT_TRANSLATE_NOOP("ImageDoc", "RGBA, 16 bits")) : text(QT_TRANSLATE_NOOP("ImageDoc", "RGB, 16 bits"));
    return alpha ? text(QT_TRANSLATE_NOOP("ImageDoc", "RGBA")) : text(QT_TRANSLATE_NOOP("ImageDoc", "RGB"));
}

// The toolbar's icons, drawn in the text's colour of the moment.
class Glyph : public QIconEngine
{
public:
    enum Kind { Minus, Plus, TurnLeft, TurnRight, Select, Previous, Next, Play, Pause, Checkers, More };
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
        QPen pen(c, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p->setPen(pen);
        p->setBrush(Qt::NoBrush);
        switch (a_kind) {
        case Minus: p->drawLine(QPointF(4, 8), QPointF(12, 8)); break;
        case Plus:
            p->drawLine(QPointF(4, 8), QPointF(12, 8));
            p->drawLine(QPointF(8, 4), QPointF(8, 12));
            break;
        case TurnLeft:
        case TurnRight: {
            if (a_kind == TurnRight) {
                p->translate(16, 0);
                p->scale(-1, 1);
            }
            // An arc anticlockwise, its head at the left pointing down.
            const QRectF circle(3.5, 4, 9, 9);
            QPainterPath arc;
            arc.arcMoveTo(circle, -45);
            arc.arcTo(circle, -45, 225);
            p->drawPath(arc);
            const QPointF end = arc.currentPosition();
            p->drawPolyline(QPolygonF{end + QPointF(-2.2, -2), end + QPointF(0, 0.4), end + QPointF(2.2, -2)});
            break;
        }
        case Select: {
            QPen dashed(c, 1.2, Qt::CustomDashLine, Qt::FlatCap);
            dashed.setDashPattern({2.0, 1.6});
            p->setPen(dashed);
            p->drawRect(QRectF(2.5, 3.5, 11, 9));
            break;
        }
        case Previous: p->drawPolyline(QPolygonF{QPointF(10, 4), QPointF(6, 8), QPointF(10, 12)}); break;
        case Next: p->drawPolyline(QPolygonF{QPointF(6, 4), QPointF(10, 8), QPointF(6, 12)}); break;
        case Play:
            p->setBrush(c);
            p->drawPolygon(QPolygonF{QPointF(5, 3.5), QPointF(12.5, 8), QPointF(5, 12.5)});
            break;
        case Pause:
            p->setPen(Qt::NoPen);
            p->setBrush(c);
            p->drawRoundedRect(QRectF(4, 3.5, 2.8, 9), 0.6, 0.6);
            p->drawRoundedRect(QRectF(9.2, 3.5, 2.8, 9), 0.6, 0.6);
            break;
        case Checkers:
            p->drawRect(QRectF(2.5, 2.5, 11, 11));
            p->setPen(Qt::NoPen);
            p->setBrush(c);
            p->drawRect(QRectF(2.5, 2.5, 5.5, 5.5));
            p->drawRect(QRectF(8, 8, 5.5, 5.5));
            break;
        case More:
            p->setBrush(c);
            p->setPen(Qt::NoPen);
            for (qreal x : {4.0, 8.0, 12.0}) p->drawEllipse(QPointF(x, 8), 1.2, 1.2);
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

// Pixels of a picture of \a size to those it is shown in, turned \a turns
// quarter turns clockwise.
QTransform turnTransform(QSizeF size, int turns)
{
    const qreal w = size.width(), h = size.height();
    switch (turns) {
    case 1: return QTransform(0, 1, -1, 0, h, 0);    // (x, y) -> (h - y, x)
    case 2: return QTransform(-1, 0, 0, -1, w, h);   // (x, y) -> (w - x, h - y)
    case 3: return QTransform(0, -1, 1, 0, 0, w);    // (x, y) -> (y, w - x)
    default: return QTransform();
    }
}

} // namespace

const QStringList& suffixes()
{
    static const QStringList list = [] {
        QStringList s;
        for (const QByteArray& format : QImageReader::supportedImageFormats()) {
            const QString name = QString::fromLatin1(format).toLower();
            // A PDF has a viewer of its own (or the system's).
            if (name == QLatin1String("pdf") || s.contains(name)) continue;
            s << name;
        }
        for (const char* svg : {"svg", "svgz"})
            if (!s.contains(QLatin1String(svg))) s << QLatin1String(svg);
        std::sort(s.begin(), s.end());
        return s;
    }();
    return list;
}

bool isImageFile(const QString& name)
{
    return suffixes().contains(QFileInfo(name).suffix().toLower());
}

const QStringList& writableSuffixes()
{
    static const QStringList list = [] {
        QStringList s;
        for (const QByteArray& format : QImageWriter::supportedImageFormats()) {
            const QString name = QString::fromLatin1(format).toLower();
            if (name == QLatin1String("pdf") || name == QLatin1String("svg") || name == QLatin1String("svgz") || s.contains(name)) continue;
            s << name;
        }
        std::sort(s.begin(), s.end());
        return s;
    }();
    return list;
}

QSize svgSize(const QSvgRenderer& svg)
{
    const QSize own = svg.defaultSize();
    if (own.width() > 0 && own.height() > 0) return own;
    const QSize box = svg.viewBoxF().size().toSize();
    if (box.width() > 0 && box.height() > 0) return box;
    return QSize(300, 150);
}

QImage thumbnail(const QString& path, int side, QSize* size)
{
    if (side <= 0) return {};
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("svg") || suffix == QLatin1String("svgz")) {
        QSvgRenderer svg;
        if (!svg.load(path) || !svg.isValid()) return {};
        const QSize own = svgSize(svg);
        if (size != nullptr) *size = own;
        // Drawn to fit: it is as sharp at any size.
        const QSize target = own.scaled(side, side, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
        QImage image(target, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter p(&image);
        p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
        svg.render(&p, QRectF(QPointF(0, 0), QSizeF(target)));
        return image;
    }
    const AllocationLimit limit;
    QImageReader reader(path);
    reader.setAutoTransform(true);
    reader.setDecideFormatFromContent(true);
    QSize own = reader.size();
    // Read small where the format can (a JPEG decodes at an eighth).
    if (own.isValid() && (own.width() > side || own.height() > side))
        reader.setScaledSize(own.scaled(side, side, Qt::KeepAspectRatio).expandedTo(QSize(1, 1)));
    const bool turned = reader.transformation().testFlag(QImageIOHandler::TransformationRotate90);
    QImage image = reader.read();
    if (image.isNull()) return {};
    if (!own.isValid()) own = image.size();
    else if (turned) own.transpose();
    if (size != nullptr) *size = own;
    if (image.width() > side || image.height() > side) image = image.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

// ----------------------------------------------------------------------
// ImageView

ImageView::ImageView(QWidget* parent) : QAbstractScrollArea(parent)
{
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setMouseTracking(true);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    horizontalScrollBar()->setSingleStep(24);
    verticalScrollBar()->setSingleStep(24);
}

void ImageView::setImage(const QImage& image)
{
    const bool same = a_svg == nullptr && !a_size.isEmpty() && image.size() == a_size;
    a_svg = nullptr;
    a_svgCache = QImage();
    a_svgCacheScale = 0;
    a_image = image;
    // Drawn in the screen's formats (fast); read - the pointer's colour - as it is.
    a_paint = image;
    if (!image.isNull()) {
        const QImage::Format fast = image.hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32;
        if (image.format() != fast) a_paint = image.convertToFormat(fast);
    }
    a_halves.clear();
    if (!same) {
        a_size = image.size();
        clearSelection();
        relayout();
        horizontalScrollBar()->setValue(horizontalScrollBar()->maximum() / 2);
        verticalScrollBar()->setValue(verticalScrollBar()->maximum() / 2);
        emit zoomChanged(a_zoom);
    }
    viewport()->update();
}

void ImageView::setSvg(QSvgRenderer* svg, QSize size)
{
    const bool same = a_svg != nullptr && size == a_size;
    a_svg = svg;
    a_image = QImage();
    a_paint = QImage();
    a_halves.clear();
    a_svgCache = QImage();
    a_svgCacheScale = 0;
    if (!same) {
        a_size = svg != nullptr ? size : QSize();
        clearSelection();
        relayout();
        horizontalScrollBar()->setValue(horizontalScrollBar()->maximum() / 2);
        verticalScrollBar()->setValue(verticalScrollBar()->maximum() / 2);
        emit zoomChanged(a_zoom);
    }
    viewport()->update();
}

void ImageView::clear()
{
    a_svg = nullptr;
    a_image = QImage();
    a_paint = QImage();
    a_halves.clear();
    a_svgCache = QImage();
    a_svgCacheScale = 0;
    a_size = QSize();
    clearSelection();
    relayout();
    viewport()->update();
}

void ImageView::refresh()
{
    a_svgCache = QImage();
    a_svgCacheScale = 0;
    viewport()->update();
}

QSize ImageView::shownSize() const
{
    return a_turns % 2 != 0 ? a_size.transposed() : a_size;
}

QPoint ImageView::offset() const
{
    return QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value());
}

QPointF ImageView::origin() const
{
    const QSizeF shown = QSizeF(shownSize()) * a_zoom;
    return QPointF(std::floor((a_contentSize.width() - shown.width()) / 2), std::floor((a_contentSize.height() - shown.height()) / 2));
}

QTransform ImageView::turning() const
{
    return turnTransform(QSizeF(a_size), a_turns);
}

QTransform ImageView::toViewportTransform() const
{
    const QPointF at = origin() - QPointF(offset());
    return turning() * QTransform::fromScale(a_zoom, a_zoom) * QTransform::fromTranslate(at.x(), at.y());
}

QPointF ImageView::toPicture(QPointF pos) const
{
    return toViewportTransform().inverted().map(pos);
}

QPointF ImageView::toViewport(QPointF point) const
{
    return toViewportTransform().map(point);
}

QRectF ImageView::pictureRect() const
{
    return toViewportTransform().mapRect(QRectF(QPointF(0, 0), QSizeF(a_size)));
}

QPoint ImageView::pixelAt(QPointF pos) const
{
    if (isEmpty()) return QPoint(-1, -1);
    const QPointF p = toPicture(pos);
    const QPoint pixel(int(std::floor(p.x())), int(std::floor(p.y())));
    return QRect(QPoint(0, 0), a_size).contains(pixel) ? pixel : QPoint(-1, -1);
}

QPointF ImageView::shownAt(QPointF pos) const
{
    return (pos + QPointF(offset()) - origin()) / a_zoom;
}

void ImageView::keepAt(QPointF shown, QPointF pos)
{
    const QPointF want = origin() + shown * a_zoom - pos;
    horizontalScrollBar()->setValue(int(std::lround(want.x())));
    verticalScrollBar()->setValue(int(std::lround(want.y())));
}

void ImageView::relayout()
{
    const QSize vp = viewport()->size();
    const QSize shown = shownSize();
    if (a_fit != Fit::None && !shown.isEmpty()) {
        qreal z = 1.0;
        if (a_fit == Fit::Width) {
            // As wide as the view without a scroll bar when the picture
            // needs none then, else beside one: no coming and going of it.
            const bool transient = style()->styleHint(QStyle::SH_ScrollBar_Transient, nullptr, verticalScrollBar()) != 0;
            const int bar = transient ? 0 : style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, verticalScrollBar());
            const int full = vp.width() + (verticalScrollBar()->isVisible() ? bar : 0);
            z = std::max(1, full - 2 * Margin) / qreal(shown.width());
            if (shown.height() * z + 2 * Margin > vp.height() + (horizontalScrollBar()->isVisible() ? bar : 0))
                z = std::max(1, full - bar - 2 * Margin) / qreal(shown.width());
        } else {
            const qreal w = std::max(1, vp.width() - 2 * Margin), h = std::max(1, vp.height() - 2 * Margin);
            z = std::min(w / shown.width(), h / shown.height());
            // A bitmap is not enlarged to fit (its pixels would show); an
            // SVG is as sharp at any size.
            if (a_fit == Fit::Auto && a_svg == nullptr) z = std::min<qreal>(z, 1.0);
        }
        a_zoom = std::clamp(z, MinZoom, MaxZoom);
    }
    const QSizeF size = QSizeF(shown) * a_zoom;
    a_contentSize = QSize(std::max(vp.width(), int(std::ceil(size.width())) + 2 * Margin),
                          std::max(vp.height(), int(std::ceil(size.height())) + 2 * Margin));
    if (shown.isEmpty()) a_contentSize = vp;
    updateScrollBars();
}

void ImageView::updateScrollBars()
{
    const QSize vp = viewport()->size();
    horizontalScrollBar()->setRange(0, std::max(0, a_contentSize.width() - vp.width()));
    horizontalScrollBar()->setPageStep(vp.width());
    verticalScrollBar()->setRange(0, std::max(0, a_contentSize.height() - vp.height()));
    verticalScrollBar()->setPageStep(vp.height());
}

void ImageView::zoomTo(qreal zoom, QPointF anchor)
{
    const qreal z = std::clamp(zoom, MinZoom, MaxZoom);
    if (anchor.x() < 0) anchor = QRectF(viewport()->rect()).center();
    const QPointF at = shownAt(anchor);
    a_fit = Fit::None;
    a_zoom = z;
    relayout();
    if (!isEmpty()) keepAt(at, anchor);
    viewport()->update();
    emit zoomChanged(a_zoom);
}

void ImageView::setZoom(qreal zoom)
{
    zoomTo(zoom, QPointF(-1, -1));
}

void ImageView::zoomBy(qreal factor, QPoint anchor)
{
    zoomTo(a_zoom * factor, anchor.x() < 0 ? QPointF(-1, -1) : QPointF(anchor));
}

void ImageView::zoomStep(bool in)
{
    qreal next = in ? MaxZoom : MinZoom;
    for (const qreal s : kSteps) {
        if (in && s > a_zoom * 1.0001) {
            next = s;
            break;
        }
        if (!in && s < a_zoom * 0.9999) next = s;
    }
    zoomTo(next, QPointF(-1, -1));
}

void ImageView::setFit(Fit fit)
{
    const QPointF centre = QRectF(viewport()->rect()).center();
    const QPointF at = shownAt(centre);
    a_fit = fit;
    relayout();
    if (!isEmpty()) keepAt(at, centre);
    viewport()->update();
    emit zoomChanged(a_zoom);
}

void ImageView::showRect(const QRectF& rect)
{
    const QRectF shown = turning().mapRect(rect.intersected(QRectF(QPointF(0, 0), QSizeF(a_size))));
    if (shown.isEmpty()) return;
    const QSize vp = viewport()->size();
    const qreal z = std::min(std::max(1, vp.width() - 2 * Margin) / shown.width(), std::max(1, vp.height() - 2 * Margin) / shown.height());
    a_fit = Fit::None;
    a_zoom = std::clamp(z, MinZoom, MaxZoom);
    relayout();
    keepAt(shown.center(), QRectF(viewport()->rect()).center());
    viewport()->update();
    emit zoomChanged(a_zoom);
}

void ImageView::setTurns(int turns)
{
    turns = ((turns % 4) + 4) % 4;
    if (turns == a_turns) return;
    const QPointF centre = QRectF(viewport()->rect()).center();
    const QPointF at = isEmpty() ? QPointF() : toPicture(centre);
    a_turns = turns;
    relayout();
    if (!isEmpty()) keepAt(turning().map(at), centre);
    viewport()->update();
    emit zoomChanged(a_zoom);
}

void ImageView::setBackground(Background background)
{
    a_background = background;
    viewport()->update();
}

void ImageView::setPixelGrid(bool shown)
{
    a_grid = shown;
    viewport()->update();
}

void ImageView::setSelecting(bool on)
{
    a_selecting = on;
    viewport()->setCursor(on ? Qt::CrossCursor : Qt::ArrowCursor);
}

void ImageView::setSelection(const QRect& rect)
{
    const QRect r = rect.normalized().intersected(QRect(QPoint(0, 0), a_size));
    if (r == a_selection) return;
    a_selection = r;
    viewport()->update();
    emit selectionChanged(a_selection);
}

void ImageView::clearSelection()
{
    setSelection(QRect());
}

QRect ImageView::selectionFrom(QPointF a, QPointF b) const
{
    const qreal w = a_size.width(), h = a_size.height();
    const int x0 = int(std::floor(std::clamp(std::min(a.x(), b.x()), 0.0, w)));
    const int y0 = int(std::floor(std::clamp(std::min(a.y(), b.y()), 0.0, h)));
    const int x1 = int(std::ceil(std::clamp(std::max(a.x(), b.x()), 0.0, w)));
    const int y1 = int(std::ceil(std::clamp(std::max(a.y(), b.y()), 0.0, h)));
    if (x1 <= x0 || y1 <= y0) return QRect();
    return QRect(QPoint(x0, y0), QPoint(x1 - 1, y1 - 1));
}

QPixmap ImageView::checkers() const
{
    const bool dark = palette().color(QPalette::Window).lightness() < 128;
    constexpr int s = 8;
    QPixmap pm(2 * s, 2 * s);
    pm.fill(dark ? QColor(0x3c, 0x3c, 0x3c) : QColor(0xff, 0xff, 0xff));
    QPainter q(&pm);
    const QColor other = dark ? QColor(0x2f, 0x2f, 0x2f) : QColor(0xe2, 0xe2, 0xe2);
    q.fillRect(0, 0, s, s, other);
    q.fillRect(s, s, s, s, other);
    return pm;
}

const QImage& ImageView::halved(int level)
{
    if (level <= 0) return a_paint;
    while (a_halves.size() < level) {
        const QImage from = a_halves.isEmpty() ? a_paint : a_halves.last();
        a_halves << from.scaled(std::max(1, from.width() / 2), std::max(1, from.height() / 2), Qt::IgnoreAspectRatio,
                                Qt::SmoothTransformation);
    }
    return a_halves.at(level - 1);
}

void ImageView::paintEvent(QPaintEvent* event)
{
    QPainter p(viewport());
    const QPalette pal = palette();
    const bool dark = pal.color(QPalette::Window).lightness() < 128;
    p.fillRect(event->rect(), dark ? pal.color(QPalette::Window).darker(135) : QColor(0xe4, 0xe6, 0xea));
    if (isEmpty()) return;

    const QRectF shown = pictureRect();
    const QRectF visible = shown.intersected(QRectF(event->rect()));
    if (!visible.isEmpty()) {
        // Under its transparent parts.
        p.save();
        p.setClipRect(visible);
        switch (a_background) {
        case Background::Light: p.fillRect(shown, Qt::white); break;
        case Background::Dark: p.fillRect(shown, QColor(0x1a, 0x1a, 0x1a)); break;
        case Background::Checkers:
            p.setBrushOrigin(shown.topLeft());
            p.fillRect(shown, QBrush(checkers()));
            break;
        }
        p.restore();
        const qreal dpr = viewport()->devicePixelRatioF();
        if (a_svg != nullptr) drawSvg(p, visible, dpr);
        else drawBitmap(p, visible, dpr);
        if (a_grid && a_svg == nullptr && a_zoom >= GridZoom - 1e-9) drawGrid(p, visible);
    }
    if (!a_selection.isEmpty()) {
        const QRectF r = toViewportTransform().mapRect(QRectF(a_selection)).adjusted(0.5, 0.5, -0.5, -0.5);
        QColor fill = pal.color(QPalette::Highlight);
        fill.setAlpha(45);
        p.fillRect(r, fill);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(Qt::white, 1));
        p.drawRect(r);
        p.setPen(QPen(QColor(0x20, 0x20, 0x20), 1, Qt::DashLine));
        p.drawRect(r);
    }
}

void ImageView::drawBitmap(QPainter& p, const QRectF& visible, qreal dpr)
{
    const QTransform v = toViewportTransform();
    // The pixels in sight and a few around: drawn smooth, the edge of a
    // part painted alone (a scroll's) matches the part beside it.
    const QRect src = v.inverted().mapRect(visible).toAlignedRect().adjusted(-2, -2, 2, 2).intersected(QRect(QPoint(0, 0), a_size));
    if (src.isEmpty()) return;
    const qreal scale = a_zoom * dpr;   // device pixels for one of its
    p.save();
    p.setClipRect(visible);
    if (scale >= 1.0 - 1e-9 || qint64(a_size.width()) * a_size.height() < kHalvesFrom) {
        // Larger: sharp pixels; smaller: smooth.
        p.setRenderHint(QPainter::SmoothPixmapTransform, scale < 1.0 - 1e-9);
        p.setWorldTransform(v);
        p.drawImage(QRectF(src), a_paint, QRectF(src));
    } else {
        int level = 0;
        while (level < 20 && scale * qreal(qint64(1) << (level + 1)) <= 1.0) ++level;
        const QImage& half = halved(level);
        const qreal fx = qreal(a_size.width()) / half.width(), fy = qreal(a_size.height()) / half.height();
        const QRect part = QRectF(src.x() / fx, src.y() / fy, src.width() / fx, src.height() / fy)
                               .toAlignedRect()
                               .adjusted(-1, -1, 1, 1)
                               .intersected(half.rect());
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.setWorldTransform(QTransform::fromScale(fx, fy) * v);
        p.drawImage(QRectF(part), half, QRectF(part));
    }
    p.restore();
}

void ImageView::drawSvg(QPainter& p, const QRectF& visible, qreal dpr)
{
    const qreal k = a_zoom * dpr;   // device pixels for one of its
    const QTransform v = toViewportTransform();
    const QRectF all(QPointF(0, 0), QSizeF(a_size));
    const bool whole = qint64(std::ceil(a_size.width() * k)) * qint64(std::ceil(a_size.height() * k)) <= kSvgWhole;
    QRectF wanted = whole ? all : v.inverted().mapRect(visible).intersected(all);
    if (wanted.isEmpty()) return;
    const bool fresh = !a_svgCache.isNull() && std::abs(a_svgCacheScale - k) < 1e-9 && a_svgCacheRect.contains(wanted);
    if (!fresh) {
        if (!whole) {
            // The part in sight and some around it: a little scrolling
            // draws nothing new.
            const QRectF around = v.inverted().mapRect(QRectF(viewport()->rect())).intersected(all);
            wanted = around.adjusted(-around.width() / 4, -around.height() / 4, around.width() / 4, around.height() / 4).intersected(all);
        }
        const QRect pixels = QRectF(wanted.topLeft() * k, wanted.size() * k).toAlignedRect();
        if (pixels.isEmpty()) return;
        QImage cache(pixels.size(), QImage::Format_ARGB32_Premultiplied);
        cache.fill(Qt::transparent);
        {
            QPainter q(&cache);
            q.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform | QPainter::TextAntialiasing);
            q.translate(-pixels.topLeft());
            q.scale(k, k);
            a_svg->render(&q, all);
        }
        a_svgCache = cache;
        a_svgCacheScale = k;
        a_svgCacheRect = QRectF(QPointF(pixels.topLeft()) / k, QSizeF(pixels.size()) / k);
        ++a_svgRenders;
    }
    p.save();
    p.setClipRect(visible);
    // A pixel of the drawing on a pixel of the screen.
    p.setWorldTransform(QTransform::fromScale(1 / k, 1 / k) * QTransform::fromTranslate(a_svgCacheRect.left(), a_svgCacheRect.top()) * v);
    p.drawImage(QPointF(0, 0), a_svgCache);
    p.restore();
}

void ImageView::drawGrid(QPainter& p, const QRectF& visible)
{
    const QTransform v = toViewportTransform();
    const QRect src = v.inverted().mapRect(visible).toAlignedRect().intersected(QRect(QPoint(0, 0), a_size));
    if (src.isEmpty()) return;
    p.save();
    p.setClipRect(visible);
    p.setWorldTransform(v);
    QPen pen(QColor(128, 128, 128, 110), 0);
    pen.setCosmetic(true);
    p.setPen(pen);
    for (int x = src.left(); x <= src.right() + 1; ++x) p.drawLine(QPointF(x, src.top()), QPointF(x, src.bottom() + 1));
    for (int y = src.top(); y <= src.bottom() + 1; ++y) p.drawLine(QPointF(src.left(), y), QPointF(src.right() + 1, y));
    p.restore();
}

void ImageView::resizeEvent(QResizeEvent* event)
{
    // What was in the middle stays there.
    const QSize old = event->oldSize();
    const bool keep = old.isValid() && !old.isEmpty() && !isEmpty();
    const QPointF at = keep ? shownAt(QPointF(old.width() / 2.0, old.height() / 2.0)) : QPointF();
    const qreal before = a_zoom;
    QAbstractScrollArea::resizeEvent(event);
    relayout();
    if (keep) keepAt(at, QRectF(viewport()->rect()).center());
    if (a_zoom != before) emit zoomChanged(a_zoom);
}

void ImageView::scrollContentsBy(int, int)
{
    viewport()->update();
}

void ImageView::wheelEvent(QWheelEvent* event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        const int delta = event->angleDelta().y();
        if (delta != 0) zoomBy(std::pow(1.0015, delta), event->position().toPoint());
        event->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(event);
}

bool ImageView::viewportEvent(QEvent* event)
{
    switch (event->type()) {
    case QEvent::NativeGesture: {
        auto* g = static_cast<QNativeGestureEvent*>(event);
        if (g->gestureType() == Qt::ZoomNativeGesture) {
            zoomBy(1.0 + g->value(), g->position().toPoint());
            return true;
        }
        break;
    }
    case QEvent::Leave: emit hovered(QPoint(-1, -1)); break;
    default: break;
    }
    return QAbstractScrollArea::viewportEvent(event);
}

void ImageView::mousePressEvent(QMouseEvent* event)
{
    a_pressPos = event->position().toPoint();
    a_moved = false;
    const bool select = event->button() == Qt::LeftButton && (a_selecting || (event->modifiers() & Qt::ShiftModifier));
    if (event->button() == Qt::MiddleButton || (event->button() == Qt::LeftButton && !select)) {
        setFocus(Qt::MouseFocusReason);
        a_drag = Drag::Pan;
        a_panStart = event->globalPosition().toPoint();
        a_panScroll = offset();
        if (horizontalScrollBar()->maximum() > 0 || verticalScrollBar()->maximum() > 0) viewport()->setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (select && !isEmpty()) {
        setFocus(Qt::MouseFocusReason);
        a_drag = Drag::Select;
        a_selectFrom = toPicture(event->position());
        return;
    }
    QAbstractScrollArea::mousePressEvent(event);
}

void ImageView::mouseMoveEvent(QMouseEvent* event)
{
    const QPointF pos = event->position();
    switch (a_drag) {
    case Drag::Pan: {
        const QPoint d = event->globalPosition().toPoint() - a_panStart;
        if (d.manhattanLength() >= QApplication::startDragDistance()) a_moved = true;
        horizontalScrollBar()->setValue(a_panScroll.x() - d.x());
        verticalScrollBar()->setValue(a_panScroll.y() - d.y());
        break;
    }
    case Drag::Select:
        if ((pos.toPoint() - a_pressPos).manhattanLength() >= QApplication::startDragDistance()) a_moved = true;
        if (a_moved) setSelection(selectionFrom(a_selectFrom, toPicture(pos)));
        break;
    case Drag::None: {
        const bool movable = horizontalScrollBar()->maximum() > 0 || verticalScrollBar()->maximum() > 0;
        viewport()->setCursor(a_selecting || (event->modifiers() & Qt::ShiftModifier) ? Qt::CrossCursor
                              : movable                                                 ? Qt::OpenHandCursor
                                                                                        : Qt::ArrowCursor);
        break;
    }
    }
    emit hovered(pixelAt(pos));
}

void ImageView::mouseReleaseEvent(QMouseEvent* event)
{
    const Drag drag = a_drag;
    a_drag = Drag::None;
    if (drag == Drag::Pan) viewport()->setCursor(horizontalScrollBar()->maximum() > 0 || verticalScrollBar()->maximum() > 0
                                                     ? Qt::OpenHandCursor
                                                     : Qt::ArrowCursor);
    // A click (no drag): the selection goes.
    if (drag != Drag::None && !a_moved && event->button() == Qt::LeftButton) clearSelection();
    QAbstractScrollArea::mouseReleaseEvent(event);
}

void ImageView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || isEmpty()) return;
    // Between 100% at the pointer and the whole picture.
    if (a_fit == Fit::None && std::abs(a_zoom - 1.0) < 1e-9) setFit(Fit::Auto);
    else zoomTo(1.0, event->position());
}

void ImageView::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_PageDown:
        if (a_frameKeys) {
            emit stepRequested(1);
            return;
        }
        break;
    case Qt::Key_PageUp:
        if (a_frameKeys) {
            emit stepRequested(-1);
            return;
        }
        break;
    case Qt::Key_Space:
        if (a_frameKeys) {
            emit playRequested();
            return;
        }
        break;
    case Qt::Key_Escape:
        if (!a_selection.isEmpty()) {
            clearSelection();
            return;
        }
        break;
    case Qt::Key_Home:
        horizontalScrollBar()->setValue(0);
        verticalScrollBar()->setValue(0);
        return;
    case Qt::Key_End:
        horizontalScrollBar()->setValue(horizontalScrollBar()->maximum());
        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
        return;
    default: break;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

bool ImageView::event(QEvent* event)
{
    // Its keys before the window's shortcuts: Escape (Select) clears the
    // selection; Page Up and Down, Space go through the frames.
    if (event->type() == QEvent::ShortcutOverride) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->modifiers() == Qt::NoModifier
            && ((key->key() == Qt::Key_Escape && !a_selection.isEmpty())
                || (a_frameKeys && (key->key() == Qt::Key_PageUp || key->key() == Qt::Key_PageDown || key->key() == Qt::Key_Space)))) {
            event->accept();
            return true;
        }
    }
    return QAbstractScrollArea::event(event);
}

void ImageView::contextMenuEvent(QContextMenuEvent* event)
{
    emit menuRequested(event->globalPos(), pixelAt(QPointF(event->pos())));
}

} // namespace qucs_s::image

// ----------------------------------------------------------------------
// ImageDoc

using qucs_s::image::Glyph;
using qucs_s::image::ImageView;
using qucs_s::image::glyph;

namespace {

const char* const kBackgroundKey = "ImageViewer/background";
const char* const kGridKey = "ImageViewer/pixelGrid";

QToolButton* toolButton(QWidget* parent, const QIcon& icon, const QString& tip)
{
    auto* b = new QToolButton(parent);
    b->setIcon(icon);
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setIconSize(QSize(16, 16));
    b->setFocusPolicy(Qt::NoFocus);
    b->setProperty("imageTool", true);
    return b;
}

QString fitName(ImageView::Fit fit)
{
    switch (fit) {
    case ImageView::Fit::Auto: return ImageDoc::tr("Best Fit");
    case ImageView::Fit::Window: return ImageDoc::tr("Fit Window");
    case ImageView::Fit::Width: return ImageDoc::tr("Fit Width");
    default: return QString();
    }
}

} // namespace

struct ImageDoc::Picture {
    QString format;
    QSize size;   // an SVG's
    QList<QImage> frames;
    QList<int> delays;
    bool animated = false;
    int first = 0;     // the frame shown first: an icon's largest size
    int skipped = 0;   // frames not read: they would not fit in memory
    std::unique_ptr<QSvgRenderer> svg;
};

ImageDoc::ImageDoc(QucsApp* app, const QString& name) : QFrame(), QucsDoc(app, name)
{
    // No dataset, no data display, no script: it is not simulated.
    a_DataSet.clear();
    a_DataDisplay.clear();
    a_Script.clear();
    a_source = a_DocName;
    a_playTimer = new QTimer(this);
    a_playTimer->setSingleShot(true);
    connect(a_playTimer, &QTimer::timeout, this, [this] {
        setFrame(a_frame + 1);
        scheduleFrame();
    });
    buildUi();

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
        // Deleted - to be written again later, a plot made anew: its
        // folder watched until it is there again. Shown as it was
        // meanwhile, and said so.
        const QString folder = QFileInfo(a_DocName).absolutePath();
        if (!a_watcher->directories().contains(folder)) a_watcher->addPath(folder);
        if (a_App != nullptr)
            a_App->statusBar()->showMessage(tr("%1 was deleted: it is shown as it was, and loaded again when it is written again.")
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

ImageDoc::~ImageDoc()
{
    // The view lets go of the SVG before it goes.
    a_playTimer->stop();
    a_view->clear();
}

void ImageDoc::buildUi()
{
    setFrameShape(QFrame::NoFrame);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // The toolbar.
    a_toolbar = new QWidget(this);
    a_toolbar->setObjectName(QStringLiteral("imageToolbar"));
    auto* bar = new QHBoxLayout(a_toolbar);
    bar->setContentsMargins(6, 3, 6, 3);
    bar->setSpacing(2);
    a_zoomOutButton = toolButton(a_toolbar, glyph(Glyph::Minus), tr("Zoom out"));
    a_zoomInButton = toolButton(a_toolbar, glyph(Glyph::Plus), tr("Zoom in"));
    a_zoomBox = new QComboBox(a_toolbar);
    a_zoomBox->setEditable(true);
    a_zoomBox->setInsertPolicy(QComboBox::NoInsert);
    a_zoomBox->addItem(fitName(ImageView::Fit::Auto), -1.0);
    a_zoomBox->setItemData(0, tr("The whole picture: a bitmap never enlarged, an SVG as large as it goes"), Qt::ToolTipRole);
    a_zoomBox->addItem(fitName(ImageView::Fit::Window), -2.0);
    a_zoomBox->setItemData(1, tr("The whole picture, as large as it goes"), Qt::ToolTipRole);
    a_zoomBox->addItem(fitName(ImageView::Fit::Width), -3.0);
    for (int z : {10, 25, 50, 75, 100, 150, 200, 400, 800, 1600, 3200}) a_zoomBox->addItem(QStringLiteral("%1%").arg(z), z / 100.0);
    a_zoomBox->setMinimumContentsLength(15);
    a_zoomBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    a_zoomBox->setFocusPolicy(Qt::ClickFocus);
    a_zoomBox->setToolTip(tr("The zoom: pick a fit or a scale, or type one"));
    auto* turnLeft = toolButton(a_toolbar, glyph(Glyph::TurnLeft), tr("Turn left (the view; the file is left as it is)"));
    auto* turnRight = toolButton(a_toolbar, glyph(Glyph::TurnRight), tr("Turn right (the view; the file is left as it is)"));
    a_selectButton = toolButton(a_toolbar, glyph(Glyph::Select), tr("Select a part of it: drag over the part (or drag with Shift)"));
    a_selectButton->setCheckable(true);

    // An animation's frames, the pictures of a file of several.
    a_frameBar = new QWidget(a_toolbar);
    auto* frames = new QHBoxLayout(a_frameBar);
    frames->setContentsMargins(0, 0, 0, 0);
    frames->setSpacing(2);
    a_prevButton = toolButton(a_frameBar, glyph(Glyph::Previous), tr("The frame before (Page Up)"));
    a_playButton = toolButton(a_frameBar, glyph(Glyph::Play), tr("Play (Space)"));
    a_playButton->setCheckable(true);
    a_nextButton = toolButton(a_frameBar, glyph(Glyph::Next), tr("The next frame (Page Down)"));
    a_frameLabel = new QLabel(a_frameBar);
    frames->addWidget(a_prevButton);
    frames->addWidget(a_playButton);
    frames->addWidget(a_nextButton);
    frames->addWidget(a_frameLabel);
    a_frameBar->hide();

    a_backgroundButton = toolButton(a_toolbar, glyph(Glyph::Checkers), tr("What is under its transparent parts; the pixels' grid"));
    a_backgroundButton->setPopupMode(QToolButton::InstantPopup);
    auto* backgrounds = new QMenu(a_backgroundButton);
    auto* group = new QActionGroup(backgrounds);
    const QList<QPair<QString, ImageView::Background>> kinds = {{tr("Checkerboard"), ImageView::Background::Checkers},
                                                                {tr("White"), ImageView::Background::Light},
                                                                {tr("Black"), ImageView::Background::Dark}};
    for (const auto& [name, kind] : kinds) {
        QAction* a = backgrounds->addAction(name, this, [this, kind = kind] { setBackground(kind); });
        a->setCheckable(true);
        a->setData(int(kind));
        group->addAction(a);
        a_backgroundActions << a;
    }
    backgrounds->addSeparator();
    a_gridAction = backgrounds->addAction(tr("Pixel Grid (from 800%)"), this, [this](bool on) { setPixelGrid(on); });
    a_gridAction->setCheckable(true);
    a_backgroundButton->setMenu(backgrounds);

    a_menuButton = toolButton(a_toolbar, glyph(Glyph::More), tr("More"));
    a_menuButton->setPopupMode(QToolButton::InstantPopup);
    a_moreMenu = new QMenu(a_menuButton);
    QAction* copy = a_moreMenu->addAction(tr("Copy"), this, &ImageDoc::copyImage);
    copy->setShortcut(QKeySequence::Copy);
    copy->setShortcutVisibleInContextMenu(true);
    a_moreMenu->addAction(tr("Copy Path"), this, &ImageDoc::copyPath);
    a_moreMenu->addAction(tr("Select All"), this, &ImageDoc::selectAll);
    a_moreMenu->addSeparator();
    a_editAsText = a_moreMenu->addAction(tr("Edit as Text"), this, &ImageDoc::editAsText);
    a_moreMenu->addAction(tr("Open with the System's Viewer"), this, &ImageDoc::openExternally);
    a_moreMenu->addAction(tr("Show in File Manager"), this, &ImageDoc::revealInFileManager);
    a_moreMenu->addSeparator();
    a_moreMenu->addAction(tr("Reload"), this, [this] { reload(); });
    a_menuButton->setMenu(a_moreMenu);

    bar->addWidget(a_zoomOutButton);
    bar->addWidget(a_zoomBox);
    bar->addWidget(a_zoomInButton);
    bar->addSpacing(12);
    bar->addWidget(turnLeft);
    bar->addWidget(turnRight);
    bar->addSpacing(8);
    bar->addWidget(a_selectButton);
    bar->addSpacing(12);
    bar->addWidget(a_frameBar);
    bar->addStretch(1);
    bar->addWidget(a_backgroundButton);
    bar->addWidget(a_menuButton);
    layout->addWidget(a_toolbar);

    // The picture; or why it is not shown.
    a_stack = new QStackedWidget(this);
    a_view = new ImageView(a_stack);
    a_stack->addWidget(a_view);
    auto* problem = new QWidget(a_stack);
    auto* problemLayout = new QVBoxLayout(problem);
    a_problemLabel = new QLabel(problem);
    a_problemLabel->setWordWrap(true);
    a_problemLabel->setAlignment(Qt::AlignCenter);
    a_problemLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    problemLayout->addStretch(1);
    problemLayout->addWidget(a_problemLabel);
    problemLayout->addStretch(2);
    a_stack->addWidget(problem);
    layout->addWidget(a_stack, 1);

    // What it is, the pixel under the pointer, the selection.
    a_infoBar = new QWidget(this);
    a_infoBar->setObjectName(QStringLiteral("imageInfo"));
    auto* info = new QHBoxLayout(a_infoBar);
    info->setContentsMargins(8, 3, 8, 3);
    info->setSpacing(16);
    a_factsLabel = new QLabel(a_infoBar);
    a_factsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    a_factsLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    a_selectionLabel = new QLabel(a_infoBar);
    a_pointerLabel = new QLabel(a_infoBar);
    a_pointerLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    info->addWidget(a_factsLabel, 1);
    info->addWidget(a_selectionLabel);
    info->addWidget(a_pointerLabel);
    layout->addWidget(a_infoBar);
    setFocusProxy(a_view);

    // As the last picture had them.
    const QucsSettingsFile settings;
    const int background = std::clamp(settings.value(QLatin1String(kBackgroundKey), 0).toInt(), 0, 2);
    setBackground(ImageView::Background(background));
    setPixelGrid(settings.value(QLatin1String(kGridKey), true).toBool());

    // What the controls do.
    connect(a_zoomOutButton, &QToolButton::clicked, this, [this] { a_view->zoomStep(false); });
    connect(a_zoomInButton, &QToolButton::clicked, this, [this] { a_view->zoomStep(true); });
    connect(a_zoomBox, &QComboBox::activated, this, [this](int index) {
        const double z = a_zoomBox->itemData(index).toDouble();
        if (z == -1.0) a_view->setFit(ImageView::Fit::Auto);
        else if (z == -2.0) a_view->setFit(ImageView::Fit::Window);
        else if (z == -3.0) a_view->setFit(ImageView::Fit::Width);
        else a_view->setZoom(z);
        updateZoomBox();
        a_view->setFocus();
    });
    connect(a_zoomBox->lineEdit(), &QLineEdit::returnPressed, this, [this] {
        QString t = a_zoomBox->lineEdit()->text().trimmed();
        for (ImageView::Fit fit : {ImageView::Fit::Auto, ImageView::Fit::Window, ImageView::Fit::Width})
            if (t.startsWith(fitName(fit), Qt::CaseInsensitive)) {
                a_view->setFit(fit);
                updateZoomBox();
                a_view->setFocus();
                return;
            }
        t.remove(QLatin1Char('%'));
        bool ok = false;
        const double z = QLocale().toDouble(t, &ok);
        const double c = ok ? z : t.toDouble(&ok);
        if (ok && c > 0) a_view->setZoom(c / 100.0);
        updateZoomBox();
        a_view->setFocus();
    });
    connect(turnLeft, &QToolButton::clicked, this, &ImageDoc::rotateLeft);
    connect(turnRight, &QToolButton::clicked, this, &ImageDoc::rotateRight);
    connect(a_selectButton, &QToolButton::toggled, a_view, &ImageView::setSelecting);
    connect(a_prevButton, &QToolButton::clicked, this, [this] { stepFrame(-1); });
    connect(a_nextButton, &QToolButton::clicked, this, [this] { stepFrame(1); });
    connect(a_playButton, &QToolButton::clicked, this, [this](bool on) { setPlaying(on); });
    connect(a_view, &ImageView::zoomChanged, this, &ImageDoc::updateZoomBox);
    connect(a_view, &ImageView::hovered, this, &ImageDoc::updatePointer);
    connect(a_view, &ImageView::selectionChanged, this, &ImageDoc::updateSelection);
    connect(a_view, &ImageView::stepRequested, this, &ImageDoc::stepFrame);
    connect(a_view, &ImageView::playRequested, this, &ImageDoc::togglePlaying);
    connect(a_view, &ImageView::menuRequested, this, [this](QPoint at, QPoint pixel) {
        QMenu menu(this);
        const bool selected = !a_view->selection().isEmpty();
        QAction* copy = menu.addAction(selected ? tr("Copy the Selection") : tr("Copy"), this, &ImageDoc::copyImage);
        copy->setShortcut(QKeySequence::Copy);
        copy->setShortcutVisibleInContextMenu(true);
        if (pixel.x() >= 0 && a_svg == nullptr && !a_frames.isEmpty()) {
            const QString colour = a_frames.at(a_frame).pixelColor(pixel).name().toUpper();
            menu.addAction(tr("Copy the Colour %1").arg(colour), this, [colour] { QApplication::clipboard()->setText(colour); });
        }
        menu.addAction(tr("Select All"), this, &ImageDoc::selectAll);
        if (selected) {
            menu.addAction(tr("Zoom to the Selection"), this, [this] { a_view->showRect(QRectF(a_view->selection())); });
            menu.addAction(tr("Clear the Selection"), a_view, &ImageView::clearSelection);
        }
        menu.addSeparator();
        menu.addAction(fitName(ImageView::Fit::Auto), this, [this] { a_view->setFit(ImageView::Fit::Auto); });
        menu.addAction(fitName(ImageView::Fit::Window), this, [this] { a_view->setFit(ImageView::Fit::Window); });
        menu.addAction(tr("Actual Size"), this, [this] { a_view->setZoom(1.0); });
        menu.addSeparator();
        menu.addAction(tr("Turn Left"), this, &ImageDoc::rotateLeft);
        menu.addAction(tr("Turn Right"), this, &ImageDoc::rotateRight);
        if (frameCount() > 1) {
            menu.addSeparator();
            if (isAnimated()) menu.addAction(isPlaying() ? tr("Pause") : tr("Play"), this, &ImageDoc::togglePlaying);
            menu.addAction(tr("The Next Frame"), this, [this] { stepFrame(1); });
            menu.addAction(tr("The Frame Before"), this, [this] { stepFrame(-1); });
        }
        menu.addSeparator();
        if (a_svg != nullptr && a_format == QLatin1String("svg")) menu.addAction(tr("Edit as Text"), this, &ImageDoc::editAsText);
        menu.addAction(tr("Open with the System's Viewer"), this, &ImageDoc::openExternally);
        menu.addAction(tr("Show in File Manager"), this, &ImageDoc::revealInFileManager);
        menu.exec(at);
    });
    restyle();
}

void ImageDoc::restyle()
{
    const QPalette pal = palette();
    const QColor line = pal.color(QPalette::Mid);
    const QString sheet = QStringLiteral(
                              "#imageToolbar { border-bottom: 1px solid %1; }"
                              "#imageInfo { border-top: 1px solid %1; }"
                              "QToolButton[imageTool=\"true\"] { padding: 3px; border: none; border-radius: 4px; }"
                              "QToolButton[imageTool=\"true\"]:hover { background: %2; }"
                              "QToolButton[imageTool=\"true\"]:checked { background: %3; }"
                              "QToolButton[imageTool=\"true\"]::menu-indicator { image: none; }")
                              .arg(line.name(), pal.color(QPalette::Midlight).name(), pal.color(QPalette::Mid).name());
    if (sheet != styleSheet()) setStyleSheet(sheet);
    a_view->viewport()->update();
}

void ImageDoc::changeEvent(QEvent* event)
{
    QFrame::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        QTimer::singleShot(0, this, &ImageDoc::restyle);
}

void ImageDoc::setBackground(ImageView::Background background)
{
    a_view->setBackground(background);
    for (QAction* a : std::as_const(a_backgroundActions)) a->setChecked(a->data().toInt() == int(background));
    QucsSettingsFile().setValue(QLatin1String(kBackgroundKey), int(background));
}

void ImageDoc::setPixelGrid(bool shown)
{
    a_view->setPixelGrid(shown);
    const QSignalBlocker block(a_gridAction);
    a_gridAction->setChecked(shown);
    QucsSettingsFile().setValue(QLatin1String(kGridKey), shown);
}

// ----------------------------------------------------------------------
// Reading

bool ImageDoc::read(const QString& path, Picture* into, QString* why)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("svg") || suffix == QLatin1String("svgz")) {
        auto svg = std::make_unique<QSvgRenderer>();
        if (!svg->load(path) || !svg->isValid()) {
            *why = tr("it is no SVG that can be drawn");
            return false;
        }
        into->format = suffix;
        into->size = qucs_s::image::svgSize(*svg);
        into->svg = std::move(svg);
        return true;
    }

    const qucs_s::image::AllocationLimit limit;
    QImageReader reader(path);
    reader.setAutoTransform(true);          // a photo upright, as its camera said
    reader.setDecideFormatFromContent(true);   // a JPEG named .png read all the same
    if (!reader.canRead()) {
        *why = reader.errorString();
        return false;
    }
    into->format = QString::fromLatin1(reader.format()).toLower();
    const QSize declared = reader.size();
    const int count = reader.imageCount();
    const bool animated = reader.supportsAnimation() && count != 1;
    qint64 bytes = 0;
    const auto keep = [&](const QImage& frame, int delay) {
        into->frames << frame;
        into->delays << delay;
        bytes += frame.sizeInBytes();
        return bytes <= qucs_s::image::kFramesBytes;
    };
    if (animated) {
        // Each frame as a player shows it, the delays with them.
        for (;;) {
            const QImage frame = reader.read();
            if (frame.isNull()) break;
            if (!keep(frame, reader.nextImageDelay())) break;
            if ((count > 0 && into->frames.size() >= count) || !reader.canRead()) break;
        }
        if (count > into->frames.size()) into->skipped = count - int(into->frames.size());
    } else if (count > 1) {
        // A TIFF's pages, an icon's sizes.
        for (int i = 0; i < count; ++i) {
            if (i > 0 && !reader.jumpToImage(i)) break;
            const QImage page = reader.read();
            if (page.isNull()) break;
            if (!keep(page, 0)) break;
        }
        if (count > into->frames.size() && !into->frames.isEmpty()) into->skipped = count - int(into->frames.size());
    } else {
        const QImage image = reader.read();
        if (!image.isNull()) keep(image, 0);
    }
    if (into->frames.isEmpty()) {
        *why = reader.errorString();
        if (declared.isValid() && qint64(declared.width()) * declared.height() * 4 > (qint64(qucs_s::image::kReadLimitMB) << 20))
            *why = tr("it is %1 × %2 pixels, too many to be read here").arg(declared.width()).arg(declared.height());
        return false;
    }
    into->animated = animated && into->frames.size() > 1;
    // An icon: its largest size first.
    static const QStringList icons = {QStringLiteral("ico"), QStringLiteral("icns"), QStringLiteral("cur")};
    if (!into->animated && icons.contains(into->format)) {
        qint64 largest = -1;
        for (int i = 0; i < into->frames.size(); ++i) {
            const qint64 area = qint64(into->frames.at(i).width()) * into->frames.at(i).height();
            if (area > largest) {
                largest = area;
                into->first = i;
            }
        }
    }
    return true;
}

void ImageDoc::show(Picture& picture, bool keepView)
{
    a_problem.clear();
    a_stack->setCurrentIndex(0);
    a_toolbar->setEnabled(true);
    a_playTimer->stop();
    const bool play = keepView ? a_playing : true;
    const int frame = keepView ? a_frame : picture.first;
    a_format = picture.format;
    a_frames = picture.frames;
    a_delays = picture.delays;
    a_animated = picture.animated;
    a_skipped = picture.skipped;
    // The view lets go of an SVG before it goes.
    std::unique_ptr<QSvgRenderer> old = std::move(a_svg);
    a_svg = std::move(picture.svg);
    if (a_svg != nullptr) {
        a_frame = 0;
        a_view->setSvg(a_svg.get(), picture.size);
        // (An animated one is drawn again as it goes.)
        connect(a_svg.get(), &QSvgRenderer::repaintNeeded, a_view, &ImageView::refresh);
    } else {
        a_frame = std::clamp(frame, 0, int(a_frames.size()) - 1);
        a_view->setImage(a_frames.at(a_frame));
    }
    old.reset();
    a_view->setFrameKeys(frameCount() > 1);
    a_editAsText->setVisible(a_svg != nullptr && a_format == QLatin1String("svg"));
    a_factsLabel->setText(facts());
    a_factsLabel->setToolTip(facts());
    updateFrameBar();
    updateZoomBox();
    updatePointer(a_hover);
    updateSelection();
    a_playing = false;
    setPlaying(play);
}

bool ImageDoc::load()
{
    watch();
    if (!QFileInfo::exists(a_DocName)) {
        misc::reportError(tr("There is no file %1.").arg(QDir::toNativeSeparators(a_DocName)));
        return false;
    }
    Picture picture;
    QString why;
    if (!read(a_DocName, &picture, &why)) {
        misc::reportError(tr("%1 cannot be read as a picture: %2").arg(QDir::toNativeSeparators(a_DocName), why));
        return false;
    }
    show(picture, false);
    return true;
}

bool ImageDoc::reload()
{
    Picture picture;
    QString why;
    if (!read(a_DocName, &picture, &why)) {
        showProblem(tr("%1 cannot be read as a picture now: %2").arg(QFileInfo(a_DocName).fileName(), why));
        return false;
    }
    show(picture, true);
    emit reloaded();
    return true;
}

void ImageDoc::showProblem(const QString& text)
{
    a_problem = text;
    a_problemLabel->setText(text);
    a_playTimer->stop();
    a_stack->setCurrentIndex(1);
    a_toolbar->setEnabled(false);
}

void ImageDoc::watch()
{
    if (!a_DocName.isEmpty() && QFileInfo::exists(a_DocName) && !a_watcher->files().contains(a_DocName))
        a_watcher->addPath(a_DocName);
}

// ----------------------------------------------------------------------
// What it is

int ImageDoc::frameCount() const
{
    return a_svg != nullptr ? 1 : int(a_frames.size());
}

int ImageDoc::frameDelay(int frame) const
{
    // As web browsers have it: none, or a hundredth of a second, is a tenth.
    const int d = a_delays.value(frame);
    return d < 20 ? 100 : d;
}

bool ImageDoc::isPlaying() const
{
    return a_playing && isAnimated();
}

QString ImageDoc::facts() const
{
    if (a_view->isEmpty()) return QString();
    QStringList parts{qucs_s::image::formatName(a_format)};
    const QSize size = imageSize();
    parts << tr("%1 × %2 pixels").arg(size.width()).arg(size.height());
    if (a_svg != nullptr) {
        parts << tr("vector");
        if (a_svg->animated()) parts << tr("animated");
    } else {
        const QImage& frame = a_frames.at(a_frame);
        parts << qucs_s::image::colourOf(frame);
        if (frame.colorSpace().isValid() && !frame.colorSpace().description().isEmpty()) parts << frame.colorSpace().description();
    }
    if (isAnimated()) {
        qint64 total = 0;
        for (int i = 0; i < a_frames.size(); ++i) total += frameDelay(i);
        parts << tr("%1 frames, %2 s").arg(a_frames.size()).arg(QLocale().toString(total / 1000.0, 'f', 1));
    } else if (frameCount() > 1) {
        parts << tr("%1 pictures in the file").arg(a_frames.size());
    }
    if (a_skipped > 0) parts << (a_skipped == 1 ? tr("1 more not read: too large") : tr("%1 more not read: too large").arg(a_skipped));
    parts << QLocale().formattedDataSize(QFileInfo(a_DocName).size());
    return parts.join(QStringLiteral(" · "));
}

QString ImageDoc::pointerText() const
{
    return a_pointer;
}

QString ImageDoc::selectionText() const
{
    const QRect s = a_view->selection();
    if (s.isEmpty()) return QString();
    return tr("Selection %1 × %2 at %3, %4").arg(s.width()).arg(s.height()).arg(s.x()).arg(s.y());
}

void ImageDoc::updatePointer(QPoint pixel)
{
    a_hover = pixel;
    if (pixel.x() < 0 || a_view->isEmpty() || !QRect(QPoint(0, 0), imageSize()).contains(pixel)) {
        a_pointer.clear();
    } else if (a_svg != nullptr) {
        a_pointer = tr("x %1  y %2").arg(pixel.x()).arg(pixel.y());
    } else {
        const QColor c = a_frames.at(a_frame).pixelColor(pixel);
        a_pointer = tr("x %1  y %2   %3  (%4, %5, %6)")
                        .arg(pixel.x())
                        .arg(pixel.y())
                        .arg(c.name().toUpper())
                        .arg(c.red())
                        .arg(c.green())
                        .arg(c.blue());
        if (c.alpha() < 255) a_pointer += QLatin1Char(' ') + tr("alpha %1%").arg(std::lround(c.alphaF() * 100));
    }
    a_pointerLabel->setText(a_pointer);
}

void ImageDoc::updateSelection()
{
    a_selectionLabel->setText(selectionText());
}

void ImageDoc::updateZoomBox()
{
    const QSignalBlocker block(a_zoomBox);
    const QString percent = QStringLiteral("%1%").arg(QLocale().toString(a_view->zoom() * 100, 'f', a_view->zoom() < 0.1 ? 1 : 0));
    const QString fit = fitName(a_view->fit());
    a_zoomBox->setEditText(fit.isEmpty() ? percent : QStringLiteral("%1 (%2)").arg(fit, percent));
    a_zoomOutButton->setEnabled(a_view->zoom() > ImageView::MinZoom + 1e-9);
    a_zoomInButton->setEnabled(a_view->zoom() < ImageView::MaxZoom - 1e-9);
}

void ImageDoc::updateFrameBar()
{
    const int n = frameCount();
    a_frameBar->setVisible(n > 1);
    a_playButton->setVisible(isAnimated());
    a_frameLabel->setText(isAnimated() ? tr("%1 / %2").arg(a_frame + 1).arg(n) : tr("Picture %1 of %2").arg(a_frame + 1).arg(n));
    const QSignalBlocker block(a_playButton);
    a_playButton->setChecked(isPlaying());
    a_playButton->setIcon(glyph(isPlaying() ? Glyph::Pause : Glyph::Play));
    a_playButton->setToolTip(isPlaying() ? tr("Pause (Space)") : tr("Play (Space)"));
}

// ----------------------------------------------------------------------
// Frames

void ImageDoc::setFrame(int frame)
{
    const int n = int(a_frames.size());
    if (a_svg != nullptr || n == 0) return;
    frame = ((frame % n) + n) % n;
    if (frame == a_frame) return;
    a_frame = frame;
    a_view->setImage(a_frames.at(frame));
    updateFrameBar();
    updatePointer(a_hover);
    // A file of several pictures: each its size, its colours.
    if (!isAnimated()) {
        a_factsLabel->setText(facts());
        a_factsLabel->setToolTip(facts());
    }
    emit frameChanged(frame);
}

void ImageDoc::stepFrame(int by)
{
    if (frameCount() < 2) return;
    setPlaying(false);
    setFrame(a_frame + by);
}

void ImageDoc::setPlaying(bool playing)
{
    a_playing = playing;
    if (isPlaying()) scheduleFrame();
    else a_playTimer->stop();
    updateFrameBar();
}

void ImageDoc::togglePlaying()
{
    if (isAnimated()) setPlaying(!a_playing);
}

void ImageDoc::scheduleFrame()
{
    if (!isPlaying()) return;
    a_playTimer->start(frameDelay(a_frame));
}

// ----------------------------------------------------------------------
// The view

void ImageDoc::rotateLeft()
{
    a_view->setTurns(a_view->turns() + 3);
}

void ImageDoc::rotateRight()
{
    a_view->setTurns(a_view->turns() + 1);
}

void ImageDoc::selectAll()
{
    a_view->setSelection(QRect(QPoint(0, 0), imageSize()));
}

void ImageDoc::becomeCurrent(bool)
{
    // Nothing to undo in a document that is only read.
    if (a_App != nullptr) {
        a_App->undo->setEnabled(false);
        a_App->redo->setEnabled(false);
    }
    a_view->setFocus();
}

double ImageDoc::zoomBy(double factor)
{
    // View > Zoom In and Out: to the next of the usual scales.
    if (factor > 1) a_view->zoomStep(true);
    else if (factor < 1) a_view->zoomStep(false);
    return a_view->zoom();
}

void ImageDoc::showAll()
{
    a_view->setFit(ImageView::Fit::Window);
}

void ImageDoc::zoomToSelection()
{
    if (!a_view->selection().isEmpty()) a_view->showRect(QRectF(a_view->selection()));
    else a_view->setFit(ImageView::Fit::Width);
}

void ImageDoc::showNoZoom()
{
    a_view->setZoom(1.0);
}

// ----------------------------------------------------------------------
// The picture, out

QImage ImageDoc::picture(bool selectionOnly, qreal svgScale) const
{
    QImage image;
    const QRect selection = selectionOnly ? a_view->selection() : QRect();
    if (a_svg != nullptr) {
        const QSize own = imageSize();
        const QRectF part = selection.isEmpty() ? QRectF(QPointF(0, 0), QSizeF(own)) : QRectF(selection);
        // At most 16384 pixels a side.
        const qreal k = std::min({std::max(svgScale, 0.01), 16384.0 / part.width(), 16384.0 / part.height()});
        image = QImage(QSizeF(part.size() * k).toSize().expandedTo(QSize(1, 1)), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter q(&image);
        q.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform | QPainter::TextAntialiasing);
        q.scale(k, k);
        q.translate(-part.topLeft());
        a_svg->render(&q, QRectF(QPointF(0, 0), QSizeF(own)));
    } else if (!a_frames.isEmpty()) {
        image = a_frames.at(a_frame);
        if (!selection.isEmpty()) image = image.copy(selection);
    }
    if (!image.isNull() && a_view->turns() != 0) image = image.transformed(QTransform().rotate(90.0 * a_view->turns()));
    return image;
}

QImage ImageDoc::filePicture() const
{
    if (a_svg != nullptr) {
        QImage image(imageSize().expandedTo(QSize(1, 1)), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter q(&image);
        q.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform | QPainter::TextAntialiasing);
        a_svg->render(&q, QRectF(QPointF(0, 0), QSizeF(imageSize())));
        return image;
    }
    return a_frames.isEmpty() ? QImage() : a_frames.at(a_frame);
}

void ImageDoc::copyImage()
{
    const QImage image = picture(true);
    if (image.isNull()) return;
    auto* data = new QMimeData;
    data->setImageData(image);
    // An SVG whole and upright: its text too, for the programs that take it.
    if (a_svg != nullptr && a_format == QLatin1String("svg") && a_view->selection().isEmpty() && a_view->turns() == 0) {
        QFile file(a_DocName);
        if (file.open(QIODevice::ReadOnly)) data->setData(QStringLiteral("image/svg+xml"), file.readAll());
    }
    QApplication::clipboard()->setMimeData(data);
    if (a_App != nullptr)
        a_App->statusBar()->showMessage((a_view->selection().isEmpty() ? tr("The picture is in the clipboard, %1 × %2 pixels")
                                                                       : tr("The selection is in the clipboard, %1 × %2 pixels"))
                                            .arg(image.width())
                                            .arg(image.height()),
                                        3000);
}

void ImageDoc::copyPath()
{
    QApplication::clipboard()->setText(QDir::toNativeSeparators(a_DocName));
}

void ImageDoc::openExternally()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(a_DocName));
}

void ImageDoc::revealInFileManager()
{
    qucs_s::links::reveal(a_DocName);
}

void ImageDoc::editAsText()
{
    if (a_svg == nullptr || a_format != QLatin1String("svg") || a_App == nullptr) return;
    // Once this has returned: the tab goes, and this with it.
    QTimer::singleShot(0, a_App, [app = a_App, path = a_DocName] { app->openAsText(path); });
}

// ----------------------------------------------------------------------
// Saved, printed

void ImageDoc::setName(const QString& name)
{
    // Saved under another name (Save As): the file is written by save().
    if (!a_DocName.isEmpty()) a_watcher->removePath(a_DocName);
    if (!a_watcher->directories().isEmpty()) a_watcher->removePaths(a_watcher->directories());   // (the old one's, deleted)
    a_DocName = QFileInfo(name).absoluteFilePath();
}

int ImageDoc::save()
{
    // A copy of the file when it is saved under another name - converted
    // when that is another format's; nothing else to write. The same file
    // under another spelling (Plot.png for plot.png on macOS, a path
    // through a link) is not another name.
    if (a_source.isEmpty() || misc::isSameFile(a_source, a_DocName)) {
        watch();
        return 0;
    }
    const QString from = QFileInfo(a_source).suffix(), to = QFileInfo(a_DocName).suffix().toLower();
    const bool same = qucs_s::image::canonical(from) == qucs_s::image::canonical(to);
    if (same) {
        // Copied beside and renamed over: a copy that fails leaves the
        // file that was there.
        QString error;
        if (!misc::copyFileOver(a_source, a_DocName, &error)) {
            misc::reportError(tr("%1 could not be copied to %2.").arg(QDir::toNativeSeparators(a_source), QDir::toNativeSeparators(a_DocName))
                              + QLatin1Char('\n') + error);
            return -1;
        }
    } else {
        if (!qucs_s::image::writableSuffixes().contains(to)) {
            QStringList kinds;
            for (const QString& s : qucs_s::image::writableSuffixes()) kinds << s.toUpper();
            misc::reportError(tr("%1 cannot be written: a picture is saved as one of %2.")
                                  .arg(QDir::toNativeSeparators(a_DocName), kinds.join(QStringLiteral(", "))));
            return -1;
        }
        QImage image = filePicture();
        if (qucs_s::image::opaqueFormat(to) && image.hasAlphaChannel()) {
            // On white: its transparent parts would be black.
            QImage flat(image.size(), QImage::Format_RGB32);
            flat.fill(Qt::white);
            QPainter p(&flat);
            p.drawImage(0, 0, image);
            p.end();
            image = flat;
        }
        QSaveFile file(a_DocName);
        QImageWriter writer(&file, to.toLatin1());
        if (qucs_s::image::canonical(to) == QLatin1String("jpeg") || to == QLatin1String("webp")) writer.setQuality(92);
        if (!file.open(QIODevice::WriteOnly) || !writer.write(image) || !file.commit()) {
            const QString why = writer.error() != QImageWriter::UnknownError ? writer.errorString() : file.errorString();
            misc::reportError(tr("%1 could not be written: %2").arg(QDir::toNativeSeparators(a_DocName), why));
            return -1;
        }
    }
    a_source = a_DocName;
    watch();
    // Converted: shown as it is now - its format, its colours.
    if (!same) reload();
    return 0;
}

void ImageDoc::print(QPrinter* printer, QPainter* painter, bool printAll, bool fitToPage)
{
    // The picture as it is shown (turned; the selection alone when that is
    // what is printed), at its own size - 96 pixels to the inch, or what
    // the file says - when that fits the page, else as large as it goes.
    if (a_view->isEmpty() || printer == nullptr || painter == nullptr) return;
    const QRect area = painter->viewport();
    if (area.isEmpty()) return;
    const QRect selection = a_view->selection();
    const QRectF piece = !printAll && !selection.isEmpty() ? QRectF(selection) : QRectF(QPointF(0, 0), QSizeF(imageSize()));
    const int turns = a_view->turns();
    const QSizeF shown = turns % 2 != 0 ? piece.size().transposed() : piece.size();
    qreal dpi = 96;
    if (a_svg == nullptr) {
        const qreal own = a_frames.at(a_frame).dotsPerMeterX() * 0.0254;
        if (own >= 50 && own <= 2400) dpi = own;
    }
    QSizeF size = shown * (printer->resolution() / dpi);
    const qreal fits = std::min(area.width() / shown.width(), area.height() / shown.height());
    if (fitToPage || size.width() > area.width() || size.height() > area.height()) size = shown * fits;
    const QPointF at(area.left() + (area.width() - size.width()) / 2, area.top());
    painter->save();
    painter->setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform | QPainter::TextAntialiasing);
    painter->setWorldTransform(QTransform::fromTranslate(-piece.x(), -piece.y())
                                   * qucs_s::image::turnTransform(piece.size(), turns)
                                   * QTransform::fromScale(size.width() / shown.width(), size.height() / shown.height())
                                   * QTransform::fromTranslate(at.x(), at.y()),
                               true);
    painter->setClipRect(piece);
    if (a_svg != nullptr) a_svg->render(painter, QRectF(QPointF(0, 0), QSizeF(imageSize())));
    else painter->drawImage(piece, a_frames.at(a_frame), piece);
    painter->restore();
}
