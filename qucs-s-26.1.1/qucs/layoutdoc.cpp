/*
 * layoutdoc.cpp - a GDSII or OASIS layout in a tab of Qucs-S: its cells
 *                 and layers beside it, panned and zoomed, measured and
 *                 searched
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "layoutdoc.h"

#include "links.h"
#include "misc.h"
#include "qucs.h"
#include "settings.h"

#include <QAction>
#include <QApplication>
#include <QBoxLayout>
#include <QClipboard>
#include <QColorDialog>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHeaderView>
#include <QIconEngine>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPrinter>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

using namespace qucs_s::layout;

namespace {

const char* const kSidebarKey = "LayoutViewer/sidebar";
const char* const kLabelsKey = "LayoutViewer/labels";
constexpr int AllLevels = 100;   // the depth box's "all"

// The toolbar's icons, drawn in the text's colour of the moment.
class GlyphIcon : public QIconEngine
{
public:
    enum Kind { Sidebar, Minus, Plus, Fit, Ruler, Text, Search, More, Close, Up, Down };
    explicit GlyphIcon(Kind kind) : a_kind(kind) {}
    QIconEngine* clone() const override { return new GlyphIcon(a_kind); }
    void paint(QPainter* p, const QRect& rect, QIcon::Mode mode, QIcon::State) override
    {
        const QColor c = QApplication::palette().color(mode == QIcon::Disabled ? QPalette::Disabled : QPalette::Active,
                                                       QPalette::WindowText);
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const qreal s = std::min(rect.width(), rect.height());
        p->translate(rect.center().x() - s / 2 + 0.5, rect.center().y() - s / 2 + 0.5);
        p->scale(s / 16.0, s / 16.0);
        p->setPen(QPen(c, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p->setBrush(Qt::NoBrush);
        switch (a_kind) {
        case Sidebar:
            p->drawRoundedRect(QRectF(2, 3, 12, 10), 1.5, 1.5);
            p->drawLine(QPointF(6, 3), QPointF(6, 13));
            break;
        case Minus: p->drawLine(QPointF(4, 8), QPointF(12, 8)); break;
        case Plus:
            p->drawLine(QPointF(4, 8), QPointF(12, 8));
            p->drawLine(QPointF(8, 4), QPointF(8, 12));
            break;
        case Fit:
            for (const QPolygonF& corner : {QPolygonF{QPointF(2, 6), QPointF(2, 2), QPointF(6, 2)},
                                            QPolygonF{QPointF(10, 2), QPointF(14, 2), QPointF(14, 6)},
                                            QPolygonF{QPointF(14, 10), QPointF(14, 14), QPointF(10, 14)},
                                            QPolygonF{QPointF(6, 14), QPointF(2, 14), QPointF(2, 10)}})
                p->drawPolyline(corner);
            p->drawRect(QRectF(5.5, 5.5, 5, 5));
            break;
        case Ruler:
            p->drawLine(QPointF(2, 12), QPointF(12, 2));
            p->drawLine(QPointF(2, 12), QPointF(4, 14));
            p->drawLine(QPointF(12, 2), QPointF(14, 4));
            for (qreal t : {0.3, 0.5, 0.7}) {
                const QPointF at(2 + 10 * t, 12 - 10 * t);
                p->drawLine(at, at + QPointF(1.5, 1.5));
            }
            break;
        case Text:
            p->drawLine(QPointF(3, 3), QPointF(13, 3));
            p->drawLine(QPointF(8, 3), QPointF(8, 13));
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
        case Up: p->drawPolyline(QPolygonF{QPointF(4, 10), QPointF(8, 6), QPointF(12, 10)}); break;
        case Down: p->drawPolyline(QPolygonF{QPointF(4, 6), QPointF(8, 10), QPointF(12, 6)}); break;
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

QToolButton* toolButton(QWidget* parent, const QIcon& icon, const QString& tip)
{
    auto* b = new QToolButton(parent);
    b->setIcon(icon);
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setIconSize(QSize(16, 16));
    b->setFocusPolicy(Qt::NoFocus);
    b->setProperty("layoutTool", true);
    return b;
}

// The depth: levels, or all of them.
class DepthBox : public QSpinBox
{
public:
    explicit DepthBox(QWidget* parent) : QSpinBox(parent)
    {
        setRange(0, AllLevels);
        setValue(AllLevels);
        setFocusPolicy(Qt::ClickFocus);
        setAccelerated(true);
    }

protected:
    QString textFromValue(int value) const override
    {
        return value >= AllLevels ? QCoreApplication::translate("LayoutDoc", "all") : QString::number(value);
    }
    int valueFromText(const QString& text) const override
    {
        bool ok = false;
        const int v = text.trimmed().toInt(&ok);
        return ok ? v : AllLevels;
    }
    QValidator::State validate(QString& text, int&) const override
    {
        const QString t = text.trimmed();
        if (t.isEmpty() || QCoreApplication::translate("LayoutDoc", "all").startsWith(t, Qt::CaseInsensitive))
            return QValidator::Intermediate;
        bool ok = false;
        const int v = t.toInt(&ok);
        return ok && v >= 0 && v <= AllLevels ? QValidator::Acceptable : QValidator::Invalid;
    }
};

// "1 text", "14 shapes": a number of things, in the locale's digits.
QString counted(quint64 n, const char* one, const char* many)
{
    return QCoreApplication::translate("LayoutDoc", n == 1 ? one : many).arg(QLocale().toString(n));
}

bool sameFound(const Found& a, const Found& b)
{
    return a.cell == b.cell && a.shape == b.shape && a.label == b.label && a.copy == b.copy && a.transform == b.transform
           && a.via == b.via;
}

// Where KLayout is, when it is installed; empty otherwise.
QString kLayoutProgram()
{
    QString found = QStandardPaths::findExecutable(QStringLiteral("klayout"));
    if (!found.isEmpty()) return found;
#if defined(Q_OS_MACOS)
    for (const QString& app : {QStringLiteral("/Applications/klayout.app"), QDir::homePath() + QStringLiteral("/Applications/klayout.app"),
                               QStringLiteral("/Applications/KLayout/klayout.app")}) {
        const QString exe = app + QStringLiteral("/Contents/MacOS/klayout");
        if (QFileInfo(exe).isExecutable()) return exe;
    }
#elif defined(Q_OS_WIN)
    found = QStandardPaths::findExecutable(QStringLiteral("klayout_app"));
    if (!found.isEmpty()) return found;
    for (const QString& dir : {qEnvironmentVariable("APPDATA") + QStringLiteral("/KLayout"),
                               qEnvironmentVariable("ProgramFiles") + QStringLiteral("/KLayout"),
                               qEnvironmentVariable("ProgramFiles(x86)") + QStringLiteral("/KLayout")}) {
        const QString exe = dir + QStringLiteral("/klayout_app.exe");
        if (QFileInfo(exe).isExecutable()) return exe;
    }
#endif
    return QString();
}

} // namespace

// ----------------------------------------------------------------------
// LayoutView

namespace qucs_s::layout {

LayoutView::LayoutView(QWidget* parent) : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(120, 80);
}

void LayoutView::setLayout(std::shared_ptr<const Layout> layout)
{
    a_layout = std::move(layout);
    a_selection.reset();
    a_selectionOutline.clear();
    a_rulers.clear();
    a_cell = a_layout ? a_layout->mainCell() : -1;
    invalidate();
}

void LayoutView::setStyles(const QVector<LayerStyle>& styles)
{
    a_styles = styles;
    invalidate();
}

void LayoutView::setCell(int cell)
{
    if (!a_layout || cell < 0 || cell >= a_layout->cells.size()) return;
    a_cell = cell;
    a_selection.reset();
    a_selectionOutline.clear();
    emit selectionChanged();
    invalidate();
}

void LayoutView::setDepth(int depth)
{
    a_depth = std::max(0, depth);
    invalidate();
}

void LayoutView::setLabelsShown(bool shown)
{
    a_labels = shown;
    invalidate();
}

void LayoutView::invalidate()
{
    a_dirty = true;
    update();
}

void LayoutView::setView(QPointF center, double scale)
{
    a_center = center;
    a_scale = std::clamp(scale, MinScale, MaxScale);
    a_fitted = false;
    invalidate();
    emit viewChanged();
}

void LayoutView::showRegion(const QRectF& region)
{
    if (region.isNull() && region.topLeft().isNull()) return;
    const double minSide = a_layout ? a_layout->dbu * 10 : 0.01;
    const double w = std::max(region.width(), minSide), h = std::max(region.height(), minSide);
    const double sx = std::max(1, width() - 24) / w, sy = std::max(1, height() - 24) / h;
    a_center = region.center();
    a_scale = std::clamp(std::min(sx, sy), MinScale, MaxScale);
    a_fitted = false;
    invalidate();
    emit viewChanged();
}

void LayoutView::fit()
{
    if (!a_layout || a_cell < 0) return;
    showRegion(a_layout->cells.at(a_cell).bounds);
    a_fitted = true;
}

void LayoutView::zoomBy(double factor, QPointF anchor)
{
    if (anchor.x() < 0) anchor = QPointF(width() / 2.0, height() / 2.0);
    const QPointF at = toLayout(anchor);
    const double scale = std::clamp(a_scale * factor, MinScale, MaxScale);
    const QPointF d((anchor.x() - width() / 2.0) / scale, -(anchor.y() - height() / 2.0) / scale);
    a_center = at - d;
    a_scale = scale;
    a_fitted = false;
    invalidate();
    emit viewChanged();
}

QRectF LayoutView::visibleRegion() const
{
    return QRectF(toLayout(QPointF(0, height())), toLayout(QPointF(width(), 0))).normalized();
}

QPointF LayoutView::toLayout(QPointF pixel) const
{
    return QPointF(a_center.x() + (pixel.x() - width() / 2.0) / a_scale, a_center.y() - (pixel.y() - height() / 2.0) / a_scale);
}

QPointF LayoutView::toPixel(QPointF point) const
{
    return QPointF(width() / 2.0 + (point.x() - a_center.x()) * a_scale, height() / 2.0 - (point.y() - a_center.y()) * a_scale);
}

void LayoutView::setMode(Mode mode)
{
    if (mode == a_mode) return;
    a_mode = mode;
    a_measuring = false;
    setCursor(mode == Mode::Ruler ? Qt::CrossCursor : Qt::ArrowCursor);
    emit modeChanged(mode);
}

void LayoutView::addRuler(const QLineF& ruler)
{
    a_rulers.append(ruler);
    update();
    emit rulersChanged();
}

void LayoutView::clearRulers()
{
    a_rulers.clear();
    a_measuring = false;
    update();
    emit rulersChanged();
}

void LayoutView::select(const std::optional<Found>& found)
{
    a_selection = found;
    a_selectionOutline = found && a_layout ? outlineOf(*a_layout, *found) : QPolygonF();
    update();
    emit selectionChanged();
}

double LayoutView::tolerance() const
{
    return 4.0 / a_scale;
}

Query LayoutView::queryHere() const
{
    Query q;
    q.cell = a_cell;
    q.depth = a_depth;
    q.labels = a_labels;
    q.layers.resize(a_layout ? a_layout->layers.size() : 0);
    for (int i = 0; i < q.layers.size(); ++i) q.layers[i] = i >= a_styles.size() || a_styles.at(i).visible;
    return q;
}

bool LayoutView::selectAt(QPointF pixel)
{
    if (!a_layout || a_cell < 0) return false;
    const QPointF at = toLayout(pixel);
    const double tol = tolerance();
    Query q = queryHere();
    q.region = QRectF(at - QPointF(tol, tol), QSizeF(2 * tol, 2 * tol));
    QList<QPair<double, Found>> under;
    int looked = 0;
    search(*a_layout, q, [&](const Found& f) {
        if (hits(*a_layout, f, at, tol)) {
            const QRectF b = outlineOf(*a_layout, f).boundingRect();
            under.append({f.isText() ? -1.0 : b.width() * b.height(), f});
        }
        return ++looked < 200000;
    });
    if (under.isEmpty()) {
        select(std::nullopt);
        return false;
    }
    // The smallest first; clicked again, the next one under it.
    std::stable_sort(under.begin(), under.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    int pick = 0;
    if (a_selection)
        for (int i = 0; i < under.size(); ++i)
            if (sameFound(under.at(i).second, *a_selection)) {
                pick = (i + 1) % under.size();
                break;
            }
    select(under.at(pick).second);
    return true;
}

QString LayoutView::rulerText(const QLineF& ruler) const
{
    if (!a_layout) return QString();
    const QPointF d = ruler.p2() - ruler.p1();
    return QCoreApplication::translate("LayoutDoc", "%1 µm").arg(micrometres(*a_layout, std::hypot(d.x(), d.y())));
}

QImage LayoutView::picture()
{
    return grab().toImage();
}

void LayoutView::paintEvent(QPaintEvent*)
{
    const qreal ratio = devicePixelRatioF();
    const QSize pixels = (QSizeF(size()) * ratio).toSize();
    const QColor background = palette().color(QPalette::Base);
    if (a_dirty || a_cache.size() != pixels) {
        a_cache = QImage(pixels, QImage::Format_ARGB32_Premultiplied);
        a_cache.setDevicePixelRatio(ratio);
        a_cache.fill(background);
        if (a_layout && a_cell >= 0) {
            QPainter p(&a_cache);
            RenderOptions o;
            o.cell = a_cell;
            o.depth = a_depth;
            o.device = QRectF(rect());
            o.toDevice = viewTransform(a_center, a_scale, o.device);
            o.styles = &a_styles;
            o.labels = a_labels;
            QColor frames = palette().color(QPalette::Text);
            frames.setAlpha(150);
            o.frames = frames;
            o.text = palette().color(QPalette::Text);
            a_stats = qucs_s::layout::render(p, *a_layout, o);
            ++a_renders;
        }
        a_dirty = false;
    }
    QPainter p(this);
    p.drawImage(QPointF(0, 0), a_cache);
    p.setRenderHint(QPainter::Antialiasing);
    // What is selected.
    if (!a_selectionOutline.isEmpty()) {
        QPen pen(palette().color(QPalette::Highlight), 2);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        QPolygonF shown;
        for (const QPointF& pt : a_selectionOutline) shown << toPixel(pt);
        if (a_selection && a_selection->isText()) p.drawEllipse(shown.first(), 6, 6);
        else p.drawPolygon(shown);
    }
    // The rulers.
    if (!a_rulers.isEmpty()) {
        const QColor ink = palette().color(QPalette::Text);
        QFont font = p.font();
        font.setPixelSize(11);
        p.setFont(font);
        const QFontMetricsF fm(font);
        for (const QLineF& r : a_rulers) {
            const QPointF a = toPixel(r.p1()), b = toPixel(r.p2());
            p.setPen(QPen(ink, 1.2));
            p.drawLine(a, b);
            const QLineF line(a, b);
            const QPointF normal = line.length() > 0 ? QPointF(-(b.y() - a.y()), b.x() - a.x()) / line.length() * 5 : QPointF(0, 5);
            p.drawLine(a - normal, a + normal);
            p.drawLine(b - normal, b + normal);
            const QString text = rulerText(r);
            const QRectF box = fm.boundingRect(text).adjusted(-3, -1, 3, 1);
            const QPointF mid = (a + b) / 2 + QPointF(6, -6);
            const QRectF at(mid, box.size());
            QColor paper = palette().color(QPalette::Base);
            paper.setAlpha(220);
            p.fillRect(at, paper);
            p.drawText(at, Qt::AlignCenter, text);
        }
    }
    if (a_drag == Drag::Zoom && !a_zoomBox.isNull()) {
        p.setPen(QPen(palette().color(QPalette::Highlight), 1, Qt::DashLine));
        QColor tint = palette().color(QPalette::Highlight);
        tint.setAlpha(40);
        p.setBrush(tint);
        p.drawRect(a_zoomBox);
    }
}

void LayoutView::resizeEvent(QResizeEvent*)
{
    if (a_fitted) {
        fit();
        a_fitted = true;
    }
    invalidate();
}

bool LayoutView::event(QEvent* event)
{
    // Escape is the window's too (it ends a schematic's mode): here, while
    // there is something to let go of, it is the view's.
    if (event->type() == QEvent::ShortcutOverride) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape && key->modifiers() == Qt::NoModifier
            && (a_measuring || a_selection || !a_rulers.isEmpty() || a_mode == Mode::Ruler)) {
            event->accept();
            return true;
        }
    }
    if (event->type() == QEvent::NativeGesture) {
        auto* g = static_cast<QNativeGestureEvent*>(event);
        if (g->gestureType() == Qt::ZoomNativeGesture) {
            zoomBy(1.0 + g->value(), g->position());
            return true;
        }
    }
    return QWidget::event(event);
}

void LayoutView::wheelEvent(QWheelEvent* event)
{
    // A trackpad's two fingers pan; a wheel - or either with Ctrl - zooms.
    const bool zoom = event->modifiers() & Qt::ControlModifier || event->pixelDelta().isNull();
    if (zoom) {
        const QPoint d = event->angleDelta();
        const double steps = (d.y() != 0 ? d.y() : d.x()) / 120.0;
        if (steps != 0) zoomBy(std::pow(1.25, steps), event->position());
    } else {
        const QPoint d = event->pixelDelta();
        setView(a_center + QPointF(-d.x() / a_scale, d.y() / a_scale), a_scale);
    }
    event->accept();
}

void LayoutView::mousePressEvent(QMouseEvent* event)
{
    setFocus();
    a_pressPos = a_lastPos = event->position().toPoint();
    a_moved = false;
    if (event->button() == Qt::LeftButton && a_mode == Mode::Ruler && a_layout) {
        const QPointF start = snap(*a_layout, queryHere(), toLayout(event->position()), tolerance() * 1.5);
        a_rulers.append(QLineF(start, start));
        a_measuring = true;
        a_drag = Drag::Ruler;
        update();
        return;
    }
    if (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) a_drag = Drag::Pan;
    else if (event->button() == Qt::RightButton) a_drag = Drag::Zoom;
}

void LayoutView::mouseMoveEvent(QMouseEvent* event)
{
    const QPoint pos = event->position().toPoint();
    emit pointerMoved(toLayout(event->position()), true);
    if (a_drag == Drag::None) return;
    if (!a_moved && (pos - a_pressPos).manhattanLength() >= 4) a_moved = true;
    if (!a_moved) return;
    switch (a_drag) {
    case Drag::Pan: {
        const QPoint d = pos - a_lastPos;
        a_center -= QPointF(d.x() / a_scale, -d.y() / a_scale);
        a_fitted = false;
        invalidate();
        emit viewChanged();
        break;
    }
    case Drag::Zoom:
        a_zoomBox = QRect(a_pressPos, pos).normalized();
        update();
        break;
    case Drag::Ruler:
        if (!a_rulers.isEmpty() && a_layout) {
            QPointF end = snap(*a_layout, queryHere(), toLayout(event->position()), tolerance() * 1.5);
            // With Shift: across or up only.
            if (event->modifiers() & Qt::ShiftModifier) {
                const QPointF start = a_rulers.last().p1();
                if (std::abs(end.x() - start.x()) >= std::abs(end.y() - start.y())) end.setY(start.y());
                else end.setX(start.x());
            }
            a_rulers.last().setP2(end);
            update();
            emit rulersChanged();
        }
        break;
    case Drag::None: break;
    }
    a_lastPos = pos;
}

void LayoutView::mouseReleaseEvent(QMouseEvent* event)
{
    const Drag drag = a_drag;
    a_drag = Drag::None;
    switch (drag) {
    case Drag::Pan:
        if (!a_moved && event->button() == Qt::LeftButton) selectAt(event->position());
        break;
    case Drag::Zoom:
        if (a_moved && a_zoomBox.width() > 4 && a_zoomBox.height() > 4) {
            showRegion(QRectF(toLayout(a_zoomBox.bottomLeft()), toLayout(a_zoomBox.topRight())).normalized());
        } else if (!a_moved) {
            emit menuRequested(event->globalPosition().toPoint());
        }
        a_zoomBox = QRect();
        update();
        break;
    case Drag::Ruler:
        a_measuring = false;
        if (!a_rulers.isEmpty() && a_rulers.last().length() == 0) a_rulers.removeLast();
        update();
        emit rulersChanged();
        break;
    case Drag::None: break;
    }
}

void LayoutView::mouseDoubleClickEvent(QMouseEvent* event)
{
    // A cell the shown one places, under the pointer: shown instead (the
    // smallest, when they overlap).
    if (!a_layout || a_cell < 0 || event->button() != Qt::LeftButton || a_mode != Mode::Select) return;
    const QPointF at = toLayout(event->position());
    const Cell& cell = a_layout->cells.at(a_cell);
    int best = -1;
    double bestArea = 0;
    for (const Instance& inst : cell.instances) {
        if (inst.cell < 0) continue;
        const QRectF one = inst.transform().mapRect(a_layout->cells.at(inst.cell).bounds);
        const auto check = [&](QPointF off) {
            const QRectF b = one.translated(off);
            if (b.contains(at) && (best < 0 || b.width() * b.height() < bestArea)) {
                best = inst.cell;
                bestArea = b.width() * b.height();
            }
            return true;
        };
        if (inst.repetition < 0) check(QPointF());
        else cell.repetitions.at(inst.repetition).forEach(one, QRectF(at, QSizeF(0, 0)), check);
    }
    if (best >= 0) emit cellRequested(best);
}

void LayoutView::keyPressEvent(QKeyEvent* event)
{
    const double step = (event->modifiers() & Qt::ShiftModifier ? 0.05 : 0.25);
    const double dx = width() * step / a_scale, dy = height() * step / a_scale;
    switch (event->key()) {
    case Qt::Key_Left: setView(a_center - QPointF(dx, 0), a_scale); break;
    case Qt::Key_Right: setView(a_center + QPointF(dx, 0), a_scale); break;
    case Qt::Key_Up: setView(a_center + QPointF(0, dy), a_scale); break;
    case Qt::Key_Down: setView(a_center - QPointF(0, dy), a_scale); break;
    case Qt::Key_Plus:
    case Qt::Key_Equal: zoomBy(1.25); break;
    case Qt::Key_Minus: zoomBy(1 / 1.25); break;
    case Qt::Key_F:
    case Qt::Key_Home: fit(); break;
    case Qt::Key_Escape:
        // One thing at a time: the ruler being drawn, the selection, the
        // rulers, ruler mode.
        if (a_measuring && !a_rulers.isEmpty()) {
            a_rulers.removeLast();
            a_measuring = false;
            a_drag = Drag::None;
            update();
            emit rulersChanged();
        } else if (a_selection) {
            select(std::nullopt);
        } else if (!a_rulers.isEmpty()) {
            clearRulers();
        } else if (a_mode == Mode::Ruler) {
            setMode(Mode::Select);
        } else {
            event->ignore();
        }
        return;
    default: QWidget::keyPressEvent(event); return;
    }
    event->accept();
}

void LayoutView::leaveEvent(QEvent*)
{
    emit pointerMoved(QPointF(), false);
}

} // namespace qucs_s::layout

// ----------------------------------------------------------------------
// LayoutDoc

struct LayoutDoc::Job {
    QString path;
    std::atomic_bool cancelled{false};
    Progress progress{-1};
    std::shared_ptr<Layout> result;
    QString error;
    qint64 size = 0;
};

LayoutDoc::LayoutDoc(QucsApp* app, const QString& name) : QFrame(), QucsDoc(app, name)
{
    // No dataset, no data display, no script: it is not simulated.
    a_DataSet.clear();
    a_DataDisplay.clear();
    a_Script.clear();
    a_source = a_DocName;
    buildUi();

    a_progressTimer = new QTimer(this);
    a_progressTimer->setInterval(100);
    connect(a_progressTimer, &QTimer::timeout, this, [this] {
        if (!a_job) return;
        const int p = a_job->progress.load();
        if (p < 0) a_progress->setRange(0, 0);
        else {
            a_progress->setRange(0, 1000);
            a_progress->setValue(p);
        }
    });
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
        // Deleted - to be written again, a layout made anew: its folder
        // watched until it is there again. Shown as it was meanwhile.
        const QString folder = QFileInfo(a_DocName).absolutePath();
        if (!a_watcher->directories().contains(folder)) a_watcher->addPath(folder);
        if (a_App != nullptr)
            a_App->statusBar()->showMessage(tr("%1 was deleted: it is shown as it was, and read again when it is written again.")
                                                .arg(QFileInfo(a_DocName).fileName()), 8000);
    });
    connect(a_watcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString& folder) {
        if (!QFileInfo::exists(a_DocName)) return;
        a_watcher->removePath(folder);
        if (!a_watcher->files().contains(a_DocName)) a_watcher->addPath(a_DocName);
        a_reloadTimer->start();
    });
}

LayoutDoc::~LayoutDoc()
{
    // A read still going: it stops at its next step, and nothing waits for it.
    if (a_job) a_job->cancelled = true;
}

void LayoutDoc::buildUi()
{
    setFrameShape(QFrame::NoFrame);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // The toolbar.
    a_toolbar = new QWidget(this);
    a_toolbar->setObjectName(QStringLiteral("layoutToolbar"));
    auto* bar = new QHBoxLayout(a_toolbar);
    bar->setContentsMargins(6, 3, 6, 3);
    bar->setSpacing(2);
    a_sidebarButton = toolButton(a_toolbar, glyph(GlyphIcon::Sidebar), tr("Show or hide the cells and the layers"));
    a_sidebarButton->setCheckable(true);
    a_cellLabel = new QLabel(a_toolbar);
    a_cellLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    a_fitButton = toolButton(a_toolbar, glyph(GlyphIcon::Fit), tr("Show the whole cell (F)"));
    a_zoomOutButton = toolButton(a_toolbar, glyph(GlyphIcon::Minus), tr("Zoom out (-)"));
    a_zoomInButton = toolButton(a_toolbar, glyph(GlyphIcon::Plus), tr("Zoom in (+)"));
    auto* depthLabel = new QLabel(tr("Levels"), a_toolbar);
    a_depthBox = new DepthBox(a_toolbar);
    a_depthBox->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    a_depthBox->setToolTip(tr("How many levels of the cell's hierarchy are drawn; the cells below them as their frames"));
    depthLabel->setBuddy(a_depthBox);
    a_rulerButton = toolButton(a_toolbar, glyph(GlyphIcon::Ruler), tr("Ruler: drag to measure, snapped to corners and edges (Shift: across or up only; Escape clears)"));
    a_rulerButton->setCheckable(true);
    a_labelsButton = toolButton(a_toolbar, glyph(GlyphIcon::Text), tr("Show the texts"));
    a_labelsButton->setCheckable(true);
    a_findButton = toolButton(a_toolbar, glyph(GlyphIcon::Search), tr("Find a cell"));
    a_menuButton = toolButton(a_toolbar, glyph(GlyphIcon::More), tr("More"));
    a_menuButton->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(a_menuButton);
    QAction* kLayout = menu->addAction(tr("Open in KLayout"), this, &LayoutDoc::openInKLayout);
    connect(menu, &QMenu::aboutToShow, this, [kLayout] { kLayout->setEnabled(!kLayoutProgram().isEmpty()); });
    menu->addAction(tr("Open with the System's Application"), this, &LayoutDoc::openExternally);
    menu->addAction(tr("Show in File Manager"), this, &LayoutDoc::revealInFileManager);
    menu->addAction(tr("Copy Path"), this, [this] { QApplication::clipboard()->setText(QDir::toNativeSeparators(a_DocName)); });
    menu->addSeparator();
    menu->addAction(tr("Load Layer Properties…"), this, &LayoutDoc::chooseLayerProperties);
    menu->addAction(tr("Colours of the Palette"), this, &LayoutDoc::usePalette);
    menu->addSeparator();
    menu->addAction(tr("Clear the Rulers"), this, [this] { a_view->clearRulers(); });
    menu->addAction(tr("Reload"), this, [this] { reload(); });
    a_menuButton->setMenu(menu);

    bar->addWidget(a_sidebarButton);
    bar->addSpacing(8);
    bar->addWidget(a_cellLabel);
    bar->addSpacing(12);
    bar->addWidget(a_fitButton);
    bar->addWidget(a_zoomOutButton);
    bar->addWidget(a_zoomInButton);
    bar->addSpacing(12);
    bar->addWidget(depthLabel);
    bar->addWidget(a_depthBox);
    bar->addSpacing(12);
    bar->addWidget(a_rulerButton);
    bar->addWidget(a_labelsButton);
    bar->addStretch(1);
    bar->addWidget(a_findButton);
    bar->addWidget(a_menuButton);
    layout->addWidget(a_toolbar);

    // The search for a cell, under it when asked for.
    a_searchBar = new QWidget(this);
    a_searchBar->setObjectName(QStringLiteral("layoutSearch"));
    auto* findRow = new QHBoxLayout(a_searchBar);
    findRow->setContentsMargins(6, 2, 6, 3);
    findRow->setSpacing(2);
    a_findField = new QLineEdit(a_searchBar);
    a_findField->setPlaceholderText(tr("Find a cell by its name (* and ? as wildcards)"));
    a_findField->setClearButtonEnabled(true);
    auto* findPrev = toolButton(a_searchBar, glyph(GlyphIcon::Up), tr("Previous cell (Shift+Enter)"));
    auto* findNextButton = toolButton(a_searchBar, glyph(GlyphIcon::Down), tr("Next cell (Enter)"));
    a_findLabel = new QLabel(a_searchBar);
    a_findLabel->setMinimumWidth(a_findLabel->fontMetrics().horizontalAdvance(QStringLiteral("0000 of 0000")));
    auto* findClose = toolButton(a_searchBar, glyph(GlyphIcon::Close), tr("Close (Escape)"));
    findRow->addWidget(a_findField, 1);
    findRow->addWidget(findPrev);
    findRow->addWidget(findNextButton);
    findRow->addWidget(a_findLabel);
    findRow->addWidget(findClose);
    a_searchBar->hide();
    layout->addWidget(a_searchBar);

    // The layout, its cells and layers beside it; while it is read, how
    // far; or why there is none.
    a_stack = new QStackedWidget(this);
    a_splitter = new QSplitter(Qt::Horizontal, a_stack);
    a_splitter->setChildrenCollapsible(false);
    a_side = new QSplitter(Qt::Vertical, a_splitter);
    a_side->setChildrenCollapsible(false);
    a_cells = new QTreeWidget(a_side);
    a_cells->setHeaderLabels({tr("Cell"), tr("Placed")});
    a_cells->setToolTip(tr("The cells: the top ones, and in each what it places and how often. A click shows one."));
    a_cells->header()->setStretchLastSection(false);
    a_cells->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    a_cells->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    a_cells->setUniformRowHeights(true);
    a_layers = new QTreeWidget(a_side);
    a_layers->setHeaderLabels({tr("Layer"), tr("Shapes")});
    a_layers->setToolTip(tr("The layers: tick to show. Right-click for more."));
    a_layers->setRootIsDecorated(false);
    a_layers->header()->setStretchLastSection(false);
    a_layers->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    a_layers->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    a_layers->setUniformRowHeights(true);
    a_layers->setContextMenuPolicy(Qt::CustomContextMenu);
    a_side->addWidget(a_cells);
    a_side->addWidget(a_layers);

    auto* right = new QWidget(a_splitter);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);
    a_view = new LayoutView(right);
    rightLayout->addWidget(a_view, 1);
    auto* infoBar = new QWidget(right);
    infoBar->setObjectName(QStringLiteral("layoutInfo"));
    auto* infoRow = new QHBoxLayout(infoBar);
    infoRow->setContentsMargins(6, 2, 6, 2);
    // What is selected or measured (else what the layout has), and at the
    // right where the pointer is: the first takes the room there is.
    a_info = new QLabel(infoBar);
    a_info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    a_info->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    a_position = new QLabel(infoBar);
    a_position->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    infoRow->addWidget(a_info, 1);
    infoRow->addWidget(a_position);
    rightLayout->addWidget(infoBar);
    a_splitter->addWidget(a_side);
    a_splitter->addWidget(right);
    a_splitter->setStretchFactor(1, 1);
    a_splitter->setSizes({220, 800});
    a_stack->addWidget(a_splitter);

    auto* loading = new QWidget(a_stack);
    auto* loadingLayout = new QVBoxLayout(loading);
    loadingLayout->addStretch(1);
    a_loadingLabel = new QLabel(loading);
    a_loadingLabel->setAlignment(Qt::AlignCenter);
    a_loadingLabel->setWordWrap(true);
    loadingLayout->addWidget(a_loadingLabel);
    auto* progressRow = new QHBoxLayout();
    a_progress = new QProgressBar(loading);
    a_progress->setMaximumWidth(360);
    a_progress->setTextVisible(false);
    a_cancel = new QPushButton(tr("Cancel"), loading);
    progressRow->addStretch(1);
    progressRow->addWidget(a_progress, 1);
    progressRow->addWidget(a_cancel);
    progressRow->addStretch(1);
    loadingLayout->addLayout(progressRow);
    loadingLayout->addStretch(2);
    a_stack->addWidget(loading);

    auto* problem = new QWidget(a_stack);
    auto* problemLayout = new QVBoxLayout(problem);
    problemLayout->addStretch(1);
    a_problemLabel = new QLabel(problem);
    a_problemLabel->setWordWrap(true);
    a_problemLabel->setAlignment(Qt::AlignCenter);
    a_problemLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    problemLayout->addWidget(a_problemLabel);
    auto* retryRow = new QHBoxLayout();
    a_retry = new QPushButton(tr("Read It Again"), problem);
    retryRow->addStretch(1);
    retryRow->addWidget(a_retry);
    retryRow->addStretch(1);
    problemLayout->addLayout(retryRow);
    problemLayout->addStretch(2);
    a_stack->addWidget(problem);
    layout->addWidget(a_stack, 1);
    setFocusProxy(a_view);

    // What the controls do.
    const QucsSettingsFile settings;
    setSidebarShown(settings.value(QLatin1String(kSidebarKey), true).toBool());
    const bool labels = settings.value(QLatin1String(kLabelsKey), true).toBool();
    a_labelsButton->setChecked(labels);
    a_view->setLabelsShown(labels);
    connect(a_sidebarButton, &QToolButton::toggled, this, [this](bool on) {
        setSidebarShown(on);
        QucsSettingsFile().setValue(QLatin1String(kSidebarKey), on);
    });
    connect(a_labelsButton, &QToolButton::toggled, this, [this](bool on) {
        a_view->setLabelsShown(on);
        QucsSettingsFile().setValue(QLatin1String(kLabelsKey), on);
    });
    connect(a_fitButton, &QToolButton::clicked, this, [this] { a_view->fit(); });
    connect(a_zoomOutButton, &QToolButton::clicked, this, [this] { a_view->zoomBy(1 / 1.25); });
    connect(a_zoomInButton, &QToolButton::clicked, this, [this] { a_view->zoomBy(1.25); });
    connect(a_depthBox, &QSpinBox::valueChanged, this, [this](int v) {
        a_view->setDepth(v >= AllLevels ? (1 << 20) : v);
    });
    connect(a_rulerButton, &QToolButton::toggled, this, [this](bool on) {
        a_view->setMode(on ? LayoutView::Mode::Ruler : LayoutView::Mode::Select);
        a_view->setFocus();
    });
    connect(a_view, &LayoutView::modeChanged, this, [this](LayoutView::Mode mode) {
        const QSignalBlocker block(a_rulerButton);
        a_rulerButton->setChecked(mode == LayoutView::Mode::Ruler);
    });
    connect(a_findButton, &QToolButton::clicked, this, [this] { a_searchBar->isVisible() ? hideSearch() : showSearch(); });
    connect(a_findField, &QLineEdit::textChanged, this, [this](const QString& text) { find(text); });
    connect(a_findField, &QLineEdit::returnPressed, this, [this] {
        findNext(QApplication::keyboardModifiers() & Qt::ShiftModifier);
    });
    connect(findPrev, &QToolButton::clicked, this, [this] { findNext(true); });
    connect(findNextButton, &QToolButton::clicked, this, [this] { findNext(false); });
    connect(findClose, &QToolButton::clicked, this, &LayoutDoc::hideSearch);
    auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), a_findField);
    escape->setContext(Qt::WidgetShortcut);
    connect(escape, &QShortcut::activated, this, &LayoutDoc::hideSearch);
    connect(a_cancel, &QPushButton::clicked, this, &LayoutDoc::cancelLoading);
    connect(a_retry, &QPushButton::clicked, this, [this] {
        a_keepView = a_layout != nullptr;
        startReading();
    });

    connect(a_cells, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) { fillCellChildren(item); });
    connect(a_cells, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
        const int cell = item->data(0, Qt::UserRole).toInt();
        if (cell != shownCell()) showCell(cell);
    });
    connect(a_layers, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (column != 0) return;
        const int layer = item->data(0, Qt::UserRole).toInt();
        const bool on = item->checkState(0) == Qt::Checked;
        if (layer >= 0 && layer < a_view->styles().size() && a_view->styles().at(layer).visible != on) setLayerVisible(layer, on);
    });
    connect(a_layers, &QTreeWidget::customContextMenuRequested, this, [this](QPoint at) {
        QTreeWidgetItem* item = a_layers->itemAt(at);
        const int layer = item != nullptr ? item->data(0, Qt::UserRole).toInt() : -1;
        QMenu menu(this);
        menu.addAction(tr("Show All"), this, [this] {
            for (int i = 0; i < a_view->styles().size(); ++i) setLayerVisible(i, true);
        });
        menu.addAction(tr("Hide All"), this, [this] {
            for (int i = 0; i < a_view->styles().size(); ++i) setLayerVisible(i, false);
        });
        if (layer >= 0) {
            menu.addAction(tr("Show Only This"), this, [this, layer] {
                for (int i = 0; i < a_view->styles().size(); ++i) setLayerVisible(i, i == layer);
            });
            menu.addAction(tr("Colour…"), this, [this, layer] {
                QVector<LayerStyle> styles = a_view->styles();
                const QColor c = QColorDialog::getColor(styles[layer].frame, this, tr("The Colour of %1").arg(a_layout->layers.at(layer).key.text()));
                if (!c.isValid()) return;
                styles[layer].frame = styles[layer].fill = c;
                a_view->setStyles(styles);
                updateLayerItem(layer);
            });
        }
        menu.addSeparator();
        menu.addAction(tr("Load Layer Properties…"), this, &LayoutDoc::chooseLayerProperties);
        menu.addAction(tr("Colours of the Palette"), this, &LayoutDoc::usePalette);
        menu.exec(a_layers->viewport()->mapToGlobal(at));
    });

    connect(a_view, &LayoutView::pointerMoved, this, [this](QPointF at, bool inside) {
        if (!inside || !a_layout) {
            a_position->clear();
            return;
        }
        a_position->setText(tr("x %1  y %2 µm").arg(micrometres(*a_layout, at.x()), micrometres(*a_layout, at.y())));
    });
    connect(a_view, &LayoutView::selectionChanged, this, &LayoutDoc::updateInfo);
    connect(a_view, &LayoutView::rulersChanged, this, &LayoutDoc::updateInfo);
    connect(a_view, &LayoutView::viewChanged, this, [this] {
        if (a_view->lastStats().incomplete) updateInfo();
    });
    connect(a_view, &LayoutView::cellRequested, this, [this](int cell) { showCell(cell); });
    connect(a_view, &LayoutView::menuRequested, this, [this](QPoint at) {
        QMenu menu(this);
        QAction* copy = menu.addAction(tr("Copy"), this, &LayoutDoc::copySelection);
        copy->setShortcut(QKeySequence::Copy);
        menu.addAction(tr("Show the Whole Cell"), this, [this] { a_view->fit(); });
        if (a_view->selection()) menu.addAction(tr("Zoom to the Selection"), this, &LayoutDoc::zoomToSelection);
        menu.addSeparator();
        QAction* ruler = menu.addAction(tr("Ruler"), this, [this](bool on) { a_rulerButton->setChecked(on); });
        ruler->setCheckable(true);
        ruler->setChecked(a_view->mode() == LayoutView::Mode::Ruler);
        if (!a_view->rulers().isEmpty()) menu.addAction(tr("Clear the Rulers"), this, [this] { a_view->clearRulers(); });
        menu.addSeparator();
        menu.addAction(tr("Find a Cell…"), this, &LayoutDoc::showSearch);
        if (a_layout && shownCell() >= 0 && a_layout->cells.at(shownCell()).parents > 0) {
            const int top = a_layout->mainCell();
            menu.addAction(tr("Show %1").arg(a_layout->cells.at(top).name), this, [this, top] { showCell(top); });
        }
        menu.exec(at);
    });
    restyle();
}

void LayoutDoc::restyle()
{
    const QPalette pal = palette();
    const QColor line = pal.color(QPalette::Mid);
    const QString sheet = QStringLiteral(
                              "#layoutToolbar, #layoutSearch { border-bottom: 1px solid %1; }"
                              "#layoutInfo { border-top: 1px solid %1; }"
                              "QToolButton[layoutTool=\"true\"] { padding: 3px; border: none; border-radius: 4px; }"
                              "QToolButton[layoutTool=\"true\"]:hover { background: %2; }"
                              "QToolButton[layoutTool=\"true\"]:checked { background: %3; }"
                              "QToolButton[layoutTool=\"true\"]::menu-indicator { image: none; }")
                              .arg(line.name(), pal.color(QPalette::Midlight).name(), pal.color(QPalette::Mid).name());
    if (sheet != styleSheet()) setStyleSheet(sheet);
    a_view->update();
}

void LayoutDoc::changeEvent(QEvent* event)
{
    QFrame::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        QTimer::singleShot(0, this, [this] {
            restyle();
            a_view->setStyles(a_view->styles());   // (drawn again in the theme's colours)
        });
}

QWidget* LayoutDoc::sidebar() const
{
    return a_side;
}

void LayoutDoc::setSidebarShown(bool shown)
{
    a_side->setVisible(shown);
    const QSignalBlocker block(a_sidebarButton);
    a_sidebarButton->setChecked(shown);
}

void LayoutDoc::setName(const QString& name)
{
    // Saved under another name (Save As): the file is copied by save().
    if (!a_DocName.isEmpty()) a_watcher->removePath(a_DocName);
    if (!a_watcher->directories().isEmpty()) a_watcher->removePaths(a_watcher->directories());
    a_DocName = QFileInfo(name).absoluteFilePath();
}

int LayoutDoc::save()
{
    // A copy of the file, when it is saved under another name; nothing
    // else to write.
    if (a_source.isEmpty() || misc::isSameFile(a_source, a_DocName)) {
        if (!a_DocName.isEmpty() && QFileInfo::exists(a_DocName) && !a_watcher->files().contains(a_DocName))
            a_watcher->addPath(a_DocName);
        return 0;
    }
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

bool LayoutDoc::load()
{
    if (!QFileInfo(a_DocName).isFile()) {
        misc::reportError(tr("There is no file %1.").arg(QDir::toNativeSeparators(a_DocName)));
        return false;
    }
    if (!a_watcher->files().contains(a_DocName)) a_watcher->addPath(a_DocName);
    // Its colours: a .lyp beside it.
    a_lyp.clear();
    a_lypFile = layerPropertiesBeside(a_DocName);
    if (!a_lypFile.isEmpty()) {
        QString error;
        a_lyp = readLayerProperties(a_lypFile, &error);
        if (a_lyp.isEmpty()) a_lypFile.clear();
    }
    a_keepView = false;
    startReading();
    return true;
}

void LayoutDoc::startReading()
{
    if (a_job) a_job->cancelled = true;
    auto job = std::make_shared<Job>();
    job->path = a_DocName;
    job->size = QFileInfo(a_DocName).size();
    a_job = job;
    a_problem.clear();
    a_loadingLabel->setText(tr("Reading %1 (%2)…").arg(QFileInfo(a_DocName).fileName(), QLocale().formattedDataSize(job->size)));
    a_progress->setRange(0, 0);
    // A reload keeps what is shown until the new one is there.
    if (!a_keepView || !a_layout) {
        a_stack->setCurrentIndex(1);
        a_toolbar->setEnabled(false);
    }
    QThread* thread = QThread::create([job] {
        job->result = read(job->path, &job->error, &job->cancelled, &job->progress);
    });
    connect(thread, &QThread::finished, this, [this, job] {
        if (job == a_job) readingDone();
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
    a_progressTimer->start();
}

bool LayoutDoc::waitForLoaded(int ms)
{
    QElapsedTimer t;
    t.start();
    while (a_job && t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return !a_job;
}

void LayoutDoc::cancelLoading()
{
    if (!a_job) return;
    a_job->cancelled = true;
    a_job.reset();
    a_progressTimer->stop();
    if (a_layout) {   // (a reload: what was there stays)
        a_stack->setCurrentIndex(0);
        a_toolbar->setEnabled(true);
        return;
    }
    showProblem(tr("The reading of %1 was cancelled.").arg(QFileInfo(a_DocName).fileName()));
    emit loaded(false);
}

void LayoutDoc::readingDone()
{
    const std::shared_ptr<Job> job = a_job;
    a_job.reset();
    a_progressTimer->stop();
    if (!job->result) {
        if (a_keepView && a_layout) {
            a_stack->setCurrentIndex(0);
            a_toolbar->setEnabled(true);
            a_info->setText(tr("%1 could not be read again: %2 It is shown as it was.").arg(QFileInfo(a_DocName).fileName(), job->error));
            a_problem = job->error;
            emit loaded(false);
            return;
        }
        showProblem(job->error.isEmpty() ? tr("%1 could not be read.").arg(QFileInfo(a_DocName).fileName()) : job->error);
        emit loaded(false);
        return;
    }
    const bool keep = a_keepView && a_layout;
    a_layout = job->result;
    layoutReady(keep);
    emit loaded(true);
    if (keep) emit reloaded();
}

void LayoutDoc::layoutReady(bool keepView)
{
    const QString oldCell = keepView && a_view->layout() && a_view->cell() >= 0 ? a_view->layout()->cells.at(a_view->cell()).name : QString();
    const QPointF center = a_view->center();
    const double scale = a_view->scale();
    QHash<LayerKey, bool> shown;
    if (keepView && a_view->layout())
        for (int i = 0; i < a_view->layout()->layers.size() && i < a_view->styles().size(); ++i)
            shown.insert(a_view->layout()->layers.at(i).key, a_view->styles().at(i).visible);
    QVector<LayerStyle> styles = stylesFor(*a_layout, a_lyp);
    for (int i = 0; i < styles.size(); ++i)
        if (shown.contains(a_layout->layers.at(i).key)) styles[i].visible = shown.value(a_layout->layers.at(i).key);
    a_view->setLayout(a_layout);
    a_view->setStyles(styles);
    a_stack->setCurrentIndex(0);
    a_toolbar->setEnabled(true);
    const int again = oldCell.isEmpty() ? -1 : a_layout->cellIndex(oldCell);
    if (keepView && again >= 0) {
        showCell(again, false);
        a_view->setView(center, scale);
    } else {
        showCell(a_layout->mainCell(), true);
    }
    fillLayers();
    if (a_searchBar->isVisible() && !a_findField->text().isEmpty()) find(a_findField->text());
    updateInfo();
}

int LayoutDoc::shownCell() const
{
    return a_view->cell();
}

void LayoutDoc::showCell(int cell, bool fit)
{
    if (!a_layout || cell < 0 || cell >= a_layout->cells.size()) {
        a_cellLabel->setText(a_layout && a_layout->cells.isEmpty() ? tr("No cells") : QString());
        return;
    }
    a_view->setCell(cell);
    if (fit) a_view->fit();
    const Cell& c = a_layout->cells.at(cell);
    a_cellLabel->setText(tr("Cell %1").arg(c.name));
    a_cellLabel->setToolTip(tr("%1: %2 × %3 µm").arg(c.name, micrometres(*a_layout, c.bounds.width()), micrometres(*a_layout, c.bounds.height())));
    if (a_found.isEmpty()) fillCells();
    // Its row, when it is in sight in the tree.
    for (int i = 0; i < a_cells->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = a_cells->topLevelItem(i);
        QFont f = item->font(0);
        f.setBold(item->data(0, Qt::UserRole).toInt() == cell);
        item->setFont(0, f);
        if (f.bold()) a_cells->setCurrentItem(item);
    }
    updateInfo();
}

int LayoutDoc::depth() const
{
    return a_view->depth() >= (1 << 20) ? -1 : a_view->depth();
}

void LayoutDoc::setDepth(int depth)
{
    a_depthBox->setValue(depth < 0 || depth >= AllLevels ? AllLevels : depth);
    a_view->setDepth(depth < 0 || depth >= AllLevels ? (1 << 20) : depth);
}

void LayoutDoc::fillCells()
{
    const QSignalBlocker block(a_cells);
    a_cells->clear();
    if (!a_layout) return;
    const auto row = [this](int cell, const QString& placed) {
        auto* item = new QTreeWidgetItem({a_layout->cells.at(cell).name, placed});
        item->setData(0, Qt::UserRole, cell);
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        if (!a_layout->cells.at(cell).instances.isEmpty()) item->addChild(new QTreeWidgetItem({QStringLiteral("…")}));
        return item;
    };
    if (!a_found.isEmpty() || (a_searchBar->isVisible() && !a_findField->text().isEmpty())) {
        // A search: its cells, flat.
        for (int cell : a_found) {
            QTreeWidgetItem* item = row(cell, QString());
            qDeleteAll(item->takeChildren());
            a_cells->addTopLevelItem(item);
        }
        return;
    }
    for (int cell : a_layout->topCells) a_cells->addTopLevelItem(row(cell, QString()));
}

void LayoutDoc::fillCellChildren(QTreeWidgetItem* item)
{
    if (!a_layout || item->childCount() != 1 || item->child(0)->data(0, Qt::UserRole).isValid()) return;
    const QSignalBlocker block(a_cells);
    qDeleteAll(item->takeChildren());
    const Cell& cell = a_layout->cells.at(item->data(0, Qt::UserRole).toInt());
    // Each cell it places once, with how many times; by name.
    QMap<QString, QPair<int, quint64>> placed;
    for (const Instance& inst : cell.instances) {
        if (inst.cell < 0) continue;
        auto& entry = placed[a_layout->cells.at(inst.cell).name];
        entry.first = inst.cell;
        entry.second += inst.repetition >= 0 ? cell.repetitions.at(inst.repetition).count() : 1;
    }
    for (auto it = placed.cbegin(); it != placed.cend(); ++it) {
        auto* child = new QTreeWidgetItem({it.key(), QStringLiteral("×%1").arg(QLocale().toString(it.value().second))});
        child->setData(0, Qt::UserRole, it.value().first);
        child->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        if (!a_layout->cells.at(it.value().first).instances.isEmpty()) child->addChild(new QTreeWidgetItem({QStringLiteral("…")}));
        item->addChild(child);
    }
}

QIcon LayoutDoc::swatch(const LayerStyle& style) const
{
    QPixmap pm(QSize(16, 12) * devicePixelRatioF());
    pm.setDevicePixelRatio(devicePixelRatioF());
    pm.fill(palette().color(QPalette::Base));
    QPainter p(&pm);
    p.setPen(QPen(style.frame, 1));
    p.setBrush(style.pattern == Qt::NoBrush ? QBrush(Qt::NoBrush) : QBrush(style.fill, style.pattern));
    p.drawRect(QRectF(0.5, 0.5, 15, 11));
    return QIcon(pm);
}

void LayoutDoc::fillLayers()
{
    const QSignalBlocker block(a_layers);
    a_layers->clear();
    if (!a_layout) return;
    for (int i = 0; i < a_layout->layers.size(); ++i) {
        auto* item = new QTreeWidgetItem();
        item->setData(0, Qt::UserRole, i);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        a_layers->addTopLevelItem(item);
        updateLayerItem(i);
    }
}

void LayoutDoc::updateLayerItem(int layer)
{
    if (!a_layout || layer < 0 || layer >= a_layers->topLevelItemCount() || layer >= a_view->styles().size()) return;
    const QSignalBlocker block(a_layers);
    QTreeWidgetItem* item = a_layers->topLevelItem(layer);
    const LayerInfo& info = a_layout->layers.at(layer);
    const LayerStyle& style = a_view->styles().at(layer);
    item->setText(0, style.name.isEmpty() ? info.key.text() : QStringLiteral("%1  %2").arg(info.key.text(), style.name));
    item->setText(1, QLocale().toString(info.shapes + info.labels));
    item->setToolTip(1, tr("%1 shapes, %2 texts").arg(QLocale().toString(info.shapes), QLocale().toString(info.labels)));
    item->setIcon(0, swatch(style));
    item->setCheckState(0, style.visible ? Qt::Checked : Qt::Unchecked);
}

void LayoutDoc::setLayerVisible(int layer, bool visible)
{
    QVector<LayerStyle> styles = a_view->styles();
    if (layer < 0 || layer >= styles.size()) return;
    styles[layer].visible = visible;
    a_view->setStyles(styles);
    updateLayerItem(layer);
}

bool LayoutDoc::loadLayerProperties(const QString& path, QString* error)
{
    QString why;
    const QHash<LayerKey, LayerStyle> lyp = readLayerProperties(path, &why);
    if (lyp.isEmpty()) {
        if (error != nullptr) *error = why;
        return false;
    }
    a_lyp = lyp;
    a_lypFile = QFileInfo(path).absoluteFilePath();
    if (a_layout) {
        QVector<LayerStyle> styles = stylesFor(*a_layout, a_lyp);
        a_view->setStyles(styles);
        fillLayers();
    }
    return true;
}

void LayoutDoc::usePalette()
{
    a_lyp.clear();
    a_lypFile.clear();
    if (!a_layout) return;
    a_view->setStyles(stylesFor(*a_layout, a_lyp));
    fillLayers();
}

void LayoutDoc::chooseLayerProperties()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Layer Properties"), QFileInfo(a_DocName).absolutePath(),
                                                      tr("KLayout Layer Properties") + QStringLiteral(" (*.lyp);;") + tr("Any File") + QStringLiteral(" (*)"));
    if (path.isEmpty()) return;
    QString error;
    if (!loadLayerProperties(path, &error))
        QMessageBox::warning(this, tr("Layer Properties"), tr("%1 could not be read: %2").arg(QFileInfo(path).fileName(), error));
}

QString LayoutDoc::describe(const Found& found) const
{
    if (!a_layout) return QString();
    const Cell& cell = a_layout->cells.at(found.cell);
    const auto um = [this](double v) { return micrometres(*a_layout, v); };
    const auto layerText = [this](quint32 layer) {
        const LayerInfo& info = a_layout->layers.at(int(layer));
        const QString name = int(layer) < a_view->styles().size() ? a_view->styles().at(int(layer)).name : info.name;
        return name.isEmpty() ? info.key.text() : QStringLiteral("%1 (%2)").arg(info.key.text(), name);
    };
    QStringList via;
    for (int c : found.via) via << a_layout->cells.at(c).name;
    const QString where = found.via.size() > 1 ? tr("in %1 (%2)").arg(cell.name, via.join(QStringLiteral(" › "))) : tr("in %1").arg(cell.name);
    if (found.isText()) {
        const Label& l = cell.labels.at(found.label);
        const QPointF at = found.transform.map(l.origin);
        return tr("Text \"%1\" on %2 %3, at %4, %5 µm").arg(l.text, layerText(l.layer), where, um(at.x()), um(at.y()));
    }
    const qucs_s::layout::Shape& sh = cell.shapes.at(found.shape);
    const QRectF b = outlineOf(*a_layout, found).boundingRect();
    QString text;
    if (sh.kind == ShapeKind::Path && sh.path >= 0) {
        const PathInfo& path = cell.paths.at(sh.path);
        text = tr("Path on %1 %2: %3 points, %4 µm wide%5").arg(layerText(sh.layer), where).arg(path.count).arg(um(path.width))
                   .arg(path.ends.isEmpty() ? QString() : tr(", ends %1").arg(path.ends));
    } else {
        text = tr("Polygon on %1 %2: %3 points").arg(layerText(sh.layer), where).arg(sh.count);
    }
    text += tr(", %1 × %2 µm from %3, %4 to %5, %6")
                .arg(um(b.width()), um(b.height()), um(b.left()), um(b.top()), um(b.right()), um(b.bottom()));
    if (sh.properties >= 0) {
        QStringList props;
        for (const auto& [name, value] : cell.properties.at(sh.properties)) props << QStringLiteral("%1 = %2").arg(name, value);
        text += tr("; properties: %1").arg(props.join(QStringLiteral(", ")));
    }
    return text;
}

void LayoutDoc::updateInfo()
{
    if (!a_layout) {
        a_info->clear();
        return;
    }
    if (a_view->selection()) {
        a_info->setText(describe(*a_view->selection()));
        a_info->setToolTip(a_info->text());
        return;
    }
    if (!a_view->rulers().isEmpty()) {
        const QLineF r = a_view->rulers().last();
        const QPointF d = r.p2() - r.p1();
        a_info->setText(tr("Ruler: %1 µm (dx %2, dy %3) from %4, %5 to %6, %7")
                            .arg(micrometres(*a_layout, std::hypot(d.x(), d.y())), micrometres(*a_layout, d.x()), micrometres(*a_layout, d.y()),
                                 micrometres(*a_layout, r.p1().x()), micrometres(*a_layout, r.p1().y()), micrometres(*a_layout, r.p2().x()))
                            .arg(micrometres(*a_layout, r.p2().y())));
        a_info->setToolTip(QString());
        return;
    }
    QString text = tr("%1, %2 and %3 on %4; a database unit of %5 µm")
                       .arg(counted(quint64(a_layout->cells.size()), QT_TRANSLATE_NOOP("LayoutDoc", "%1 cell"), QT_TRANSLATE_NOOP("LayoutDoc", "%1 cells")),
                            counted(a_layout->shapeCount, QT_TRANSLATE_NOOP("LayoutDoc", "%1 shape"), QT_TRANSLATE_NOOP("LayoutDoc", "%1 shapes")),
                            counted(a_layout->labelCount, QT_TRANSLATE_NOOP("LayoutDoc", "%1 text"), QT_TRANSLATE_NOOP("LayoutDoc", "%1 texts")),
                            counted(quint64(a_layout->layers.size()), QT_TRANSLATE_NOOP("LayoutDoc", "%1 layer"), QT_TRANSLATE_NOOP("LayoutDoc", "%1 layers")),
                            micrometres(*a_layout, a_layout->dbu));
    if (a_view->lastStats().incomplete) text += tr(" - drawn in part: too much in sight, zoom in");
    if (!a_layout->warnings.isEmpty()) text += tr(" - %1").arg(a_layout->warnings.first());
    a_info->setText(text);
    a_info->setToolTip(a_layout->warnings.join(QLatin1Char('\n')));
}

void LayoutDoc::showProblem(const QString& text)
{
    a_problem = text;
    a_problemLabel->setText(text);
    a_stack->setCurrentIndex(2);
    a_toolbar->setEnabled(false);
}

bool LayoutDoc::reload()
{
    a_keepView = true;
    // Its .lyp too, which may have changed with it.
    if (!a_lypFile.isEmpty()) {
        QString error;
        const auto lyp = readLayerProperties(a_lypFile, &error);
        if (!lyp.isEmpty()) a_lyp = lyp;
    }
    startReading();
    return true;
}

void LayoutDoc::showSearch()
{
    a_searchBar->show();
    a_findField->setFocus();
    a_findField->selectAll();
    if (!a_side->isVisible()) setSidebarShown(true);
    if (!a_findField->text().isEmpty()) find(a_findField->text());
}

void LayoutDoc::hideSearch()
{
    a_searchBar->hide();
    a_found.clear();
    a_foundAt = -1;
    fillCells();
    showCell(shownCell(), false);
    a_view->setFocus();
}

void LayoutDoc::find(const QString& text)
{
    a_found.clear();
    a_foundAt = -1;
    const QString t = text.trimmed();
    if (a_layout && !t.isEmpty()) {
        const bool wild = t.contains(QLatin1Char('*')) || t.contains(QLatin1Char('?'));
        const QRegularExpression pattern(wild ? QRegularExpression::wildcardToRegularExpression(t, QRegularExpression::UnanchoredWildcardConversion)
                                              : QRegularExpression::escape(t),
                                         QRegularExpression::CaseInsensitiveOption);
        for (int c = 0; c < a_layout->cells.size(); ++c)
            if (pattern.match(a_layout->cells.at(c).name).hasMatch()) a_found.append(c);
        std::sort(a_found.begin(), a_found.end(), [this](int a, int b) { return a_layout->cells.at(a).name < a_layout->cells.at(b).name; });
    }
    fillCells();
    if (!a_found.isEmpty()) {
        a_foundAt = 0;
        showCell(a_found.first());
        a_cells->setCurrentItem(a_cells->topLevelItem(0));
    }
    a_findLabel->setText(t.isEmpty() ? QString() : a_found.isEmpty() ? tr("none") : tr("%1 of %2").arg(1).arg(a_found.size()));
}

void LayoutDoc::findNext(bool backwards)
{
    if (a_found.isEmpty()) return;
    a_foundAt = (a_foundAt + (backwards ? a_found.size() - 1 : 1)) % a_found.size();
    showCell(a_found.at(a_foundAt));
    a_cells->setCurrentItem(a_cells->topLevelItem(a_foundAt));
    a_findLabel->setText(tr("%1 of %2").arg(a_foundAt + 1).arg(a_found.size()));
}

void LayoutDoc::copySelection()
{
    if (!a_layout) return;
    if (a_view->selection()) {
        const QPolygonF outline = outlineOf(*a_layout, *a_view->selection());
        QStringList points;
        for (const QPointF& p : outline) points << QStringLiteral("%1, %2").arg(micrometres(*a_layout, p.x()), micrometres(*a_layout, p.y()));
        QApplication::clipboard()->setText(describe(*a_view->selection()) + QLatin1Char('\n') + tr("points (µm): %1").arg(points.join(QStringLiteral("; "))));
        return;
    }
    if (!a_view->rulers().isEmpty()) {
        QApplication::clipboard()->setText(a_info->text());
        return;
    }
    QApplication::clipboard()->setImage(a_view->picture());
}

void LayoutDoc::openExternally()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(a_DocName));
}

void LayoutDoc::openInKLayout()
{
    const QString program = kLayoutProgram();
    if (program.isEmpty()) {
        QMessageBox::information(this, tr("Open in KLayout"), tr("KLayout is not installed here (klayout.de)."));
        return;
    }
    if (!QProcess::startDetached(program, {a_DocName}))
        QMessageBox::warning(this, tr("Open in KLayout"), tr("KLayout could not be started: %1").arg(QDir::toNativeSeparators(program)));
}

void LayoutDoc::revealInFileManager()
{
    qucs_s::links::reveal(a_DocName);
}

void LayoutDoc::becomeCurrent(bool)
{
    // Nothing to undo in a document that is only read.
    if (a_App != nullptr) {
        a_App->undo->setEnabled(false);
        a_App->redo->setEnabled(false);
    }
    a_view->setFocus();
}

double LayoutDoc::zoomBy(double factor)
{
    a_view->zoomBy(factor > 1 ? 1.25 : factor < 1 ? 1 / 1.25 : 1.0);
    return a_view->scale();
}

void LayoutDoc::showAll()
{
    a_view->fit();
}

void LayoutDoc::zoomToSelection()
{
    if (a_view->selection() && a_layout) {
        const QRectF b = outlineOf(*a_layout, *a_view->selection()).boundingRect();
        const double m = std::max(b.width(), b.height()) * 0.25 + a_layout->dbu * 10;
        a_view->showRegion(b.adjusted(-m, -m, m, m));
        return;
    }
    a_view->fit();
}

void LayoutDoc::showNoZoom()
{
    a_view->fit();
}

void LayoutDoc::print(QPrinter*, QPainter* painter, bool, bool)
{
    // What is in sight, as large as the sheet takes it, on white.
    if (!a_layout || shownCell() < 0) return;
    const QRect area = painter->viewport();
    const QRectF region = a_view->visibleRegion();
    if (area.isEmpty() || region.isEmpty()) return;
    const double scale = std::min(area.width() / region.width(), area.height() / region.height());
    painter->save();
    painter->fillRect(area, Qt::white);
    RenderOptions o;
    o.cell = shownCell();
    o.depth = a_view->depth();
    o.device = QRectF(area);
    o.toDevice = viewTransform(region.center(), scale, o.device);
    o.styles = &a_view->styles();
    o.labels = a_view->labelsShown();
    o.frames = QColor(90, 90, 90);
    o.text = Qt::black;
    qucs_s::layout::render(*painter, *a_layout, o);
    painter->restore();
}
