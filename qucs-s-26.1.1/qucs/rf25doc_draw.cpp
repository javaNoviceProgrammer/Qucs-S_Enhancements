/*
 * rf25doc_draw.cpp - drawing an RF 2.5D model's shapes on its tab:
 *                    rectangles, polygons, paths and vias on a layer of the
 *                    stack, written at once to the layout file the model
 *                    names (begun beside it where it names none) through
 *                    the layer map, each one step of Undo (F4 of the open
 *                    items of 8 November)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "rf25doc.h"

#include "layoutedit.h"
#include "layoutrouter/router_gds.h"
#include "settings.h"

#include <QActionGroup>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineF>
#include <QMenu>
#include <QPointer>
#include <QSaveFile>
#include <QSet>
#include <QSignalBlocker>
#include <QToolButton>
#include <QUndoCommand>
#include <QUndoStack>

#include <algorithm>
#include <cmath>

using namespace qucs_s::rf25;
using qucs_s::layout::Layout;
using qucs_s::layout::LayoutView;

namespace {

const char* const kViaSizeKey = "RF25/viaSize";

/// Undo can put back a layout file as it was up to this size; past it a
/// drawing on it is not undone (it says so).
constexpr qint64 kMostKept = 64ll << 20;

/// A drawing: the model and the layout file's bytes before it and after
/// (no bytes: no file), each put in place by \a apply.
class DrawnCommand : public QUndoCommand
{
public:
    using Apply = std::function<void(const Model&, const QString&, const std::optional<QByteArray>&)>;
    DrawnCommand(Apply apply, QString path, Model before, std::optional<QByteArray> was, Model after,
                 std::optional<QByteArray> now, const QString& what)
        : QUndoCommand(what), a_apply(std::move(apply)), a_path(std::move(path)), a_before(std::move(before)),
          a_after(std::move(after)), a_was(std::move(was)), a_now(std::move(now))
    {}
    void undo() override { a_apply(a_before, a_path, a_was); }
    void redo() override
    {
        // (The first time, it is in place already.)
        if (a_first) a_first = false;
        else a_apply(a_after, a_path, a_now);
    }

private:
    Apply a_apply;
    QString a_path;
    Model a_before, a_after;
    std::optional<QByteArray> a_was, a_now;
    bool a_first = true;
};

std::optional<QByteArray> bytesOf(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    return f.readAll();
}

bool pointOf(const QJsonValue& v, QPointF* out)
{
    const QJsonArray xy = v.toArray();
    if (xy.size() != 2 || !xy.at(0).isDouble() || !xy.at(1).isDouble()) return false;
    const double x = xy.at(0).toDouble(), y = xy.at(1).toDouble();
    if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 1e9 || std::abs(y) > 1e9) return false;
    *out = QPointF(x, y);
    return true;
}

bool boxOf(const QJsonValue& v, QRectF* out)
{
    const QJsonArray b = v.toArray();
    QPointF p, q;
    if (b.size() != 4 || !pointOf(QJsonArray{b.at(0), b.at(1)}, &p) || !pointOf(QJsonArray{b.at(2), b.at(3)}, &q)) return false;
    *out = QRectF(p, q).normalized();
    return true;
}

QJsonArray pointsJson(const QPolygonF& points)
{
    QJsonArray out;
    for (const QPointF& p : points) out.append(QJsonArray{p.x(), p.y()});
    return out;
}

QString um(double v)
{
    return QString::number(v, 'g', 6);
}

QString boxSaid(const QRectF& r)
{
    return QStringLiteral("(%1, %2) to (%3, %4) µm").arg(um(r.left()), um(r.top()), um(r.right()), um(r.bottom()));
}

} // namespace

// ------------------------------------------------------------- The tools

void RF25Doc::setDrawTool(DrawTool tool)
{
    // One tool at a time: the port tools and the pen put down.
    if (tool != DrawTool::None && a_placing != Placing::None) setPlacing(Placing::None);
    a_drawTool = tool;
    if (tool != DrawTool::None) a_lastTool = tool;
    a_polygon.clear();
    if (a_drawButton != nullptr) {
        const QSignalBlocker quiet(a_drawButton);
        a_drawButton->setChecked(tool != DrawTool::None);
    }
    a_view->setMode(tool == DrawTool::Rectangle ? LayoutView::Mode::PickBox
                    : tool == DrawTool::None    ? LayoutView::Mode::Select
                                                : LayoutView::Mode::PickPoint);
    if (tool != DrawTool::None) a_view->setFocus();
    const QString layer = drawLayer();
    if (tool != DrawTool::None && layer.isEmpty())
        a_status->setText(tr("The stack has no metal to draw on: add one to the Stack first."));
    else if (tool == DrawTool::Rectangle)
        a_status->setText(tr("Drag a rectangle on %1, its corners snapped to a shape's or the grid; Escape to stop.").arg(layer));
    else if (tool == DrawTool::Polygon)
        a_status->setText(tr("Click each corner of a polygon on %1; a double click, Enter or the first corner again closes it, "
                             "Backspace takes the last back; Escape to stop.").arg(layer));
    else if (tool == DrawTool::Via)
        a_status->setText(tr("Click where a via of %1 µm is to go; Escape to stop.").arg(um(a_viaSize)));
    showDrawing();
}

void RF25Doc::dropDrawTool()
{
    a_drawTool = DrawTool::None;
    a_polygon.clear();
    if (a_drawButton != nullptr) {
        const QSignalBlocker quiet(a_drawButton);
        a_drawButton->setChecked(false);
    }
    showDrawing();
}

void RF25Doc::drawPicked(QPointF at)
{
    const QPointF p = drawSnapped(at);
    if (a_drawTool == DrawTool::Rectangle) {
        a_status->setText(tr("Drag to draw a rectangle."));
        return;
    }
    if (a_drawTool == DrawTool::Via) {
        QStringList done;
        QString why;
        const QString layer = drawLayer();
        const Layer* chosen = a_model.stack().named(layer);
        QJsonObject via{{QStringLiteral("add"), QStringLiteral("via")}, {QStringLiteral("at"), QJsonArray{p.x(), p.y()}}};
        // (On the via chosen, else the stack's first.)
        if (chosen != nullptr && chosen->kind == Layer::Via) via.insert(QStringLiteral("layer"), layer);
        const bool ok = drawShapes(QJsonArray{via}, &done, &why);
        a_status->setText(ok ? done.join(QLatin1Char(' ')) : why);
        return;
    }
    if (a_drawTool != DrawTool::Polygon) return;
    // The first corner again, within a few pixels: closed.
    const double reach = 6 / std::max(LayoutView::MinScale, a_view->scale());
    if (a_polygon.size() >= 3 && QLineF(p, a_polygon.first()).length() <= reach) {
        QString why;
        if (!finishPolygon(&why)) a_status->setText(why);
        return;
    }
    if (!a_polygon.isEmpty() && QLineF(p, a_polygon.last()).length() <= 1e-9) return;
    a_polygon << p;
    showDrawing();
    a_status->setText(tr("Corner %1 at (%2, %3) µm; a double click, Enter or the first corner again closes it, Backspace takes "
                         "the last back.")
                          .arg(a_polygon.size())
                          .arg(um(p.x()), um(p.y())));
}

QString RF25Doc::drawLayer() const
{
    const Stack stack = a_model.stack();
    // (A dielectric or the substrate drawn on is its extent: there only
    // inside what is drawn - rf25_dielectric.h.)
    const auto drawable = [](const Layer* l) {
        return l != nullptr && (l->kind == Layer::Metal || l->kind == Layer::Via || l->kind == Layer::Dielectric || l->kind == Layer::Substrate);
    };
    if (drawable(stack.named(a_drawLayer))) return stack.named(a_drawLayer)->name;
    // The metal or via chosen in the stack (its tag), else the topmost metal.
    if (drawable(stack.named(a_focus))) return stack.named(a_focus)->name;
    for (auto it = stack.layers.crbegin(); it != stack.layers.crend(); ++it)
        if (it->kind == Layer::Metal) return it->name;
    return {};
}

void RF25Doc::setDrawLayer(const QString& name)
{
    a_drawLayer = name;
    if (a_drawTool != DrawTool::None) setDrawTool(a_drawTool);
}

void RF25Doc::setViaSize(double size)
{
    if (!(size > 0) || !std::isfinite(size)) return;
    a_viaSize = size;
    QucsSettingsFile settings;
    settings.setValue(QLatin1String(kViaSizeKey), size);
}

QPointF RF25Doc::drawSnapped(QPointF at) const
{
    // A shape's corner or edge within a few pixels, as a ruler snaps: told
    // from the database grid the view falls back to, where it found none.
    if (a_layout) {
        const QPointF s = a_view->snapped(at);
        const double dbu = std::max(1e-9, a_layout->dbu);
        const QPointF onGrid(std::round(at.x() / dbu) * dbu, std::round(at.y() / dbu) * dbu);
        if (QLineF(s, onGrid).length() > 0.5 * dbu) return s;
    }
    // Else the grid shown, and nothing finer than a nanometre.
    const double step = std::max(1e-3, a_view->gridStep());
    return QPointF(std::round(at.x() / step) * step, std::round(at.y() / step) * step);
}

bool RF25Doc::finishPolygon(QString* why)
{
    if (a_polygon.size() < 3) {
        if (why != nullptr) *why = tr("A polygon has three corners at least; %1 so far.").arg(a_polygon.size());
        return false;
    }
    const QPolygonF corners = a_polygon;
    a_polygon.clear();
    showDrawing();
    QStringList done;
    QString error;
    const bool ok = drawShapes(QJsonArray{QJsonObject{{QStringLiteral("add"), QStringLiteral("polygon")},
                                                      {QStringLiteral("layer"), drawLayer()},
                                                      {QStringLiteral("points"), pointsJson(corners)}}},
                               &done, &error);
    a_status->setText(ok ? done.join(QLatin1Char(' ')) : error);
    if (!ok && why != nullptr) *why = error;
    return ok;
}

void RF25Doc::showDrawing()
{
    QList<LayoutView::Overlay> overlays = a_overlaysShown;
    if (a_drawTool == DrawTool::Polygon && !a_polygon.isEmpty()) {
        // The corners so far, and from the last to the pointer: in the
        // layer's own colour, dashed by a line of its own.
        LayoutView::Overlay drawn;
        const Stack stack = a_model.stack();
        QColor colour(30, 136, 229);
        for (int i = 0; i < stack.layers.size(); ++i)
            if (stack.layers.at(i).name == drawLayer()) colour = layerColor(i);
        drawn.line = outlineColor(colour);
        drawn.width = 2;
        drawn.name = tr("the polygon being drawn");
        QPolygonF line = a_polygon;
        if (a_drawPointerIn) line << a_drawPointer;
        // (An open line as a shape: there and back again.)
        QPolygonF there = line;
        for (int i = int(line.size()) - 2; i > 0; --i) there << line.at(i);
        drawn.shapes << there;
        for (const QPointF& p : std::as_const(a_polygon)) drawn.labels << LayoutView::Overlay::Label{p, QString(), QPointF()};
        overlays << drawn;
    }
    a_view->setOverlays(overlays);
}

void RF25Doc::fillDrawMenu()
{
    // (Filled as it opens, as things are then; what the last filling made -
    // its groups, its submenu - let go with it.)
    if (a_drawMenu == nullptr) return;
    a_drawMenu->clear();
    if (a_drawMenuParts != nullptr) a_drawMenuParts->deleteLater();
    a_drawMenuParts = new QObject(a_drawMenu);
    auto* tools = new QActionGroup(a_drawMenuParts);
    const auto tool = [&](const QString& text, DrawTool which, const QString& tip) {
        QAction* a = a_drawMenu->addAction(text);
        a->setCheckable(true);
        a->setChecked(a_drawTool == which);
        a->setToolTip(tip);
        a->setActionGroup(tools);
        connect(a, &QAction::triggered, this, [this, which] { setDrawTool(which); });
    };
    tool(tr("Rectangle"), DrawTool::Rectangle, tr("Drag a rectangle"));
    tool(tr("Polygon"), DrawTool::Polygon, tr("Click its corners; a double click closes it"));
    tool(tr("Via"), DrawTool::Via, tr("Click where a square via goes"));
    a_drawMenu->addSeparator();
    // The layers of the stack one draws on: its metals and vias, the top
    // first, as the stack is drawn.
    auto* layers = new QMenu(tr("Layer: %1").arg(drawLayer().isEmpty() ? tr("none") : drawLayer()), a_drawMenu);
    connect(a_drawMenuParts, &QObject::destroyed, layers, &QObject::deleteLater);
    a_drawMenu->addMenu(layers);
    layers->setObjectName(QStringLiteral("rfDrawLayers"));
    auto* group = new QActionGroup(a_drawMenuParts);
    const Stack stack = a_model.stack();
    const QString now = drawLayer();
    for (auto it = stack.layers.crbegin(); it != stack.layers.crend(); ++it) {
        const bool dielectric = it->kind == Layer::Dielectric || it->kind == Layer::Substrate;
        if (it->kind != Layer::Metal && it->kind != Layer::Via && !dielectric) continue;
        QAction* a = layers->addAction(it->kind == Layer::Via ? tr("%1 (a via)").arg(it->name)
                                       : dielectric          ? tr("%1 (where it is)").arg(it->name)
                                                             : it->name);
        a->setCheckable(true);
        a->setChecked(it->name == now);
        a->setActionGroup(group);
        const QString name = it->name;
        connect(a, &QAction::triggered, this, [this, name] { setDrawLayer(name); });
    }
    QAction* size = a_drawMenu->addAction(tr("Via Size: %1 µm…").arg(um(a_viaSize)));
    connect(size, &QAction::triggered, this, [this] {
        bool ok = false;
        const double v = QInputDialog::getDouble(this, tr("Via Size"), tr("The side of a via the Via tool puts, µm:"), a_viaSize, 1e-3, 1e6, 3, &ok);
        if (ok) setViaSize(v);
    });
    a_drawMenu->addSeparator();
    QAction* remove = a_drawMenu->addAction(tr("Delete the Selected Shape"));
    remove->setShortcutVisibleInContextMenu(true);
    remove->setEnabled(a_view != nullptr && a_view->selection().has_value() && !a_view->selection()->isText());
    connect(remove, &QAction::triggered, this, [this] {
        QString why;
        if (!deleteSelectedShape(&why)) a_status->setText(why);
    });
}

bool RF25Doc::eventFilter(QObject* watched, QEvent* event)
{
    // (Return, Backspace and Delete are the application's shortcuts too -
    // Edit > Delete's -: taken here first where they are the tab's own.)
    if (watched == a_view && event->type() == QEvent::ShortcutOverride) {
        auto* key = static_cast<QKeyEvent*>(event);
        const int k = key->key();
        const bool polygon = a_drawTool == DrawTool::Polygon && !a_polygon.isEmpty()
                             && (k == Qt::Key_Return || k == Qt::Key_Enter || k == Qt::Key_Backspace);
        const bool selected = (k == Qt::Key_Delete || k == Qt::Key_Backspace) && a_drawTool == DrawTool::None
                              && a_view->mode() == LayoutView::Mode::Select && a_view->selection().has_value()
                              && !a_view->selection()->isText() && key->modifiers() == Qt::NoModifier;
        if (polygon || selected) {
            key->accept();
            return true;
        }
    }
    if (watched == a_view && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (a_drawTool == DrawTool::Polygon && !a_polygon.isEmpty()) {
            if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
                QString why;
                if (!finishPolygon(&why)) a_status->setText(why);
                return true;
            }
            if (key->key() == Qt::Key_Backspace) {
                a_polygon.removeLast();
                showDrawing();
                return true;
            }
        }
        if ((key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace) && a_drawTool == DrawTool::None
            && a_view->mode() == LayoutView::Mode::Select && a_view->selection().has_value() && !a_view->selection()->isText()) {
            QString why;
            if (!deleteSelectedShape(&why)) a_status->setText(why);
            return true;
        }
    }
    if (watched == a_view && event->type() == QEvent::MouseButtonDblClick && a_drawTool == DrawTool::Polygon) {
        // (Its first click put the last corner.)
        QString why;
        if (!finishPolygon(&why)) a_status->setText(why);
        return true;
    }
    return QFrame::eventFilter(watched, event);
}

// ------------------------------------------------------------ The drawing

void RF25Doc::applyDrawn(const Model& model, const QString& path, const std::optional<QByteArray>& bytes)
{
    if (bytes) {
        QSaveFile f(path);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(*bytes);
            f.commit();
        }
    } else {
        QFile::remove(path);
    }
    a_keepView = true;
    if (model.toObject() != a_model.toObject()) applyModel(model);
    refreshLayout();
}

bool RF25Doc::drawShapes(const QJsonArray& edits, QStringList* done, QString* error)
{
    const auto fail = [error](const QString& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    if (edits.isEmpty() || edits.size() > 10000) return fail(tr("What is drawn is a list of 1 to 10000 shapes."));
    if (a_job) return fail(tr("The layout is still being read: draw once it is."));
    const Stack stack = a_model.stack();
    Model m = a_model;
    QStringList said;
    // The layout file: the one named, else one begun beside the model.
    QString path = layoutFile();
    bool begun = false;
    QString cell = textOf(m.layout(), QStringLiteral("cell")).trimmed();
    if (path.isEmpty()) {
        if (a_DocName.isEmpty())
            return fail(tr("Save the model first: what is drawn is written to a GDSII file beside it, which its Layout then names."));
        const QFileInfo model(a_DocName);
        QString name = model.completeBaseName() + QStringLiteral(".gds");
        for (int n = 2; QFileInfo::exists(model.absoluteDir().absoluteFilePath(name)); ++n)
            name = QStringLiteral("%1_%2.gds").arg(model.completeBaseName()).arg(n);
        path = model.absoluteDir().absoluteFilePath(name);
        m.layout().props.insert(QStringLiteral("file"), name);
        begun = true;
    } else if (!qucs_s::router::isWritableLayout(path)) {
        return fail(tr("%1 cannot be drawn on: shapes are written to a .gds or an .oas (a .gds.gz can be saved as a .gds first).")
                        .arg(QFileInfo(path).fileName()));
    } else if (!QFileInfo::exists(path)) {
        begun = true;
    } else if (a_layout && cell.isEmpty()) {
        // (The cell the tab solves and shows: the largest top cell.)
        const int main = a_layout->mainCell();
        if (main >= 0) cell = a_layout->cells.at(main).name;
    }
    if (begun && cell.isEmpty()) cell = QStringLiteral("TOP");

    // The layer of the file each layer of the stack is drawn on: the first
    // row of the map that sends one to it, else a row added - the next
    // layer number free, datatype 0.
    Node& mapNode = *m.layout().child(QStringLiteral("rf_layer_map"));
    QJsonArray rows = mapNode.props.value(QLatin1String("rows")).toArray();
    const auto gdsLayerOf = [&](const Layer& layer) {
        for (const QJsonValue& row : std::as_const(rows)) {
            const QJsonArray cells = row.toArray();
            if (cells.size() < 3) continue;
            const QString to = cells.at(2).toString().trimmed();
            if (to == layer.name || to == layer.tag)
                return QStringLiteral("%1/%2").arg(cells.at(0).toString().trimmed(), cells.at(1).toString().trimmed());
        }
        int next = 1;
        for (const QJsonValue& row : std::as_const(rows)) next = std::max(next, row.toArray().at(0).toString().toInt() + 1);
        if (a_layout && !begun)
            for (const auto& l : a_layout->layers) next = std::max(next, int(l.key.layer) + 1);
        rows.append(QJsonArray{QString::number(next), QStringLiteral("0"), layer.name});
        said << tr("The layer map sends the file's layer %1/0 to %2, a row added.").arg(next).arg(layer.name);
        return QStringLiteral("%1/0").arg(next);
    };
    // The layers of the file the map sends to a layer of the stack.
    const auto gdsLayersTo = [&rows](const Layer& layer) {
        QSet<QString> keys;
        for (const QJsonValue& row : std::as_const(rows)) {
            const QJsonArray cells = row.toArray();
            if (cells.size() < 3) continue;
            const QString to = cells.at(2).toString().trimmed();
            if (to == layer.name || to == layer.tag)
                keys.insert(QStringLiteral("%1/%2").arg(cells.at(0).toString().trimmed(), cells.at(1).toString().trimmed()));
        }
        return keys;
    };
    const auto layerNamed = [&](const QJsonObject& e, bool via, QString* why) -> const Layer* {
        QString name = e.value(QLatin1String("layer")).toString().trimmed();
        if (name.isEmpty() && via)
            for (const Layer& l : stack.layers)
                if (l.kind == Layer::Via) {
                    name = l.name;
                    break;
                }
        if (name.isEmpty()) name = drawLayer();
        const Layer* l = stack.named(name);
        if (l == nullptr) {
            *why = name.isEmpty() ? tr("the stack has no metal to draw on") : tr("%1 is no layer of the stack").arg(name);
            return nullptr;
        }
        if (via && l->kind != Layer::Via) {
            *why = tr("%1 is no via of the stack: a via is drawn on one").arg(l->name);
            return nullptr;
        }
        if (l->kind != Layer::Metal && l->kind != Layer::Via && l->kind != Layer::Dielectric && l->kind != Layer::Substrate) {
            *why = tr("%1 is no metal, via, dielectric or substrate: shapes are drawn on those (a ground plane has none)").arg(l->name);
            return nullptr;
        }
        return l;
    };

    QJsonArray gds;
    QStringList what;
    for (int i = 0; i < edits.size(); ++i) {
        const QJsonObject e = edits.at(i).toObject();
        QString why;
        const auto bad = [&](const QString& text) { return fail(tr("Shape %1: %2. Nothing was drawn.").arg(i + 1).arg(text)); };
        static const QSet<QString> known{QStringLiteral("add"),   QStringLiteral("delete"), QStringLiteral("layer"),
                                         QStringLiteral("box"),   QStringLiteral("points"), QStringLiteral("width"),
                                         QStringLiteral("ends"),  QStringLiteral("at"),     QStringLiteral("size"),
                                         QStringLiteral("box_is")};
        for (auto it = e.begin(); it != e.end(); ++it)
            if (!known.contains(it.key())) return bad(tr("'%1' is not a key of a shape drawn").arg(it.key()));
        const QString add = e.value(QLatin1String("add")).toString(), remove = e.value(QLatin1String("delete")).toString();
        if (add.isEmpty() == remove.isEmpty())
            return bad(tr("each is {\"add\": rectangle, polygon, path or via, ...} or {\"delete\": \"shape\", ...}"));
        if (!remove.isEmpty()) {
            if (remove != QLatin1String("shape")) return bad(tr("what is deleted is a \"shape\""));
            const Layer* layer = layerNamed(e, false, &why);
            if (layer == nullptr) return bad(why);
            if (!a_layout || begun) return bad(tr("there is no layout to delete from"));
            // (By its own box, "box_is": the shape the view selected.)
            const bool exact = e.contains(QLatin1String("box_is"));
            const bool hasAt = e.contains(QLatin1String("at")), hasBox = e.contains(QLatin1String("box")) || exact;
            QPointF at;
            QRectF box;
            if (hasAt == hasBox || (exact && e.contains(QLatin1String("box"))))
                return bad(tr("a shape deleted is picked by 'at' [x, y] or 'box' [x0, y0, x1, y1], one of them"));
            if (hasAt && !pointOf(e.value(QLatin1String("at")), &at)) return bad(tr("'at' is [x, y] in µm"));
            if (hasBox && !boxOf(e.value(QLatin1String(exact ? "box_is" : "box")), &box)) return bad(tr("'box' is [x0, y0, x1, y1] in µm"));
            const double slack = std::max(1e-3, 1e-6 * std::max(box.width(), box.height()));
            // Picked in the layout as read: the cell's own shapes on the
            // layers of the file the map sends to it.
            const QSet<QString> keys = gdsLayersTo(*layer);
            const int c = a_layout->cellIndex(cell);
            if (c < 0) return bad(tr("the layout has no cell %1").arg(cell));
            const qucs_s::layout::Cell& own = a_layout->cells.at(c);
            int best = -1;
            double bestArea = 0;
            int picked = 0;
            for (int s = 0; s < own.shapes.size(); ++s) {
                const qucs_s::layout::Shape& shape = own.shapes.at(s);
                const QString key = a_layout->layers.at(int(shape.layer)).key.text();
                if (!keys.contains(key)) continue;
                const QPolygonF outline(QList<QPointF>(own.points.cbegin() + shape.first, own.points.cbegin() + shape.first + shape.count));
                if (exact) {
                    const QRectF& b = shape.bounds;
                    if (picked > 0 || std::abs(b.left() - box.left()) > slack || std::abs(b.right() - box.right()) > slack
                        || std::abs(b.top() - box.top()) > slack || std::abs(b.bottom() - box.bottom()) > slack)
                        continue;
                }
                if (hasBox) {
                    if (!exact && !box.contains(shape.bounds)) continue;
                    gds.append(QJsonObject{{QStringLiteral("delete"), QStringLiteral("shape")}, {QStringLiteral("layer"), key},
                                           {QStringLiteral("box_is"), QJsonArray{shape.bounds.left(), shape.bounds.top(), shape.bounds.right(), shape.bounds.bottom()}}});
                    ++picked;
                    continue;
                }
                if (!outline.containsPoint(at, Qt::OddEvenFill)) continue;
                const double area = shape.bounds.width() * shape.bounds.height();
                if (best < 0 || area < bestArea) {
                    best = s;
                    bestArea = area;
                }
            }
            if (hasAt && best >= 0) {
                const qucs_s::layout::Shape& shape = own.shapes.at(best);
                gds.append(QJsonObject{{QStringLiteral("delete"), QStringLiteral("shape")},
                                       {QStringLiteral("layer"), a_layout->layers.at(int(shape.layer)).key.text()},
                                       {QStringLiteral("box_is"), QJsonArray{shape.bounds.left(), shape.bounds.top(), shape.bounds.right(), shape.bounds.bottom()}}});
                ++picked;
                what << tr("deleted the shape of %1 over %2").arg(layer->name, boxSaid(shape.bounds));
            } else if (exact && picked > 0) {
                what << tr("deleted the shape of %1 over %2").arg(layer->name, boxSaid(box));
            } else if (hasBox && picked > 0) {
                what << tr("deleted %n shape(s) of %1 wholly inside %2", nullptr, picked).arg(layer->name, boxSaid(box));
            }
            if (picked == 0)
                return bad(hasAt ? tr("%1 has no shape of %2's own at (%3, %4) µm").arg(cell, layer->name, um(at.x()), um(at.y()))
                                 : tr("%1 has no shape of %2's own wholly inside %3").arg(cell, layer->name, boxSaid(box)));
            continue;
        }
        const bool via = add == QLatin1String("via");
        const Layer* layer = layerNamed(e, via, &why);
        if (layer == nullptr) return bad(why);
        if (add == QLatin1String("rectangle") || via) {
            QRectF r;
            if (via) {
                QPointF at;
                if (!pointOf(e.value(QLatin1String("at")), &at)) return bad(tr("a via is put 'at' [x, y] in µm"));
                const double side = e.contains(QLatin1String("size")) ? e.value(QLatin1String("size")).toDouble(-1) : a_viaSize;
                if (!(side > 0) || side > 1e6) return bad(tr("a via's 'size' is its side in µm, more than 0"));
                r = QRectF(at - QPointF(side / 2, side / 2), QSizeF(side, side));
            } else if (!boxOf(e.value(QLatin1String("box")), &r)) {
                return bad(tr("a rectangle's 'box' is [x0, y0, x1, y1] in µm"));
            }
            if (!(r.width() > 0 && r.height() > 0)) return bad(tr("the rectangle has no area"));
            const QPolygonF corners{r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()};
            gds.append(QJsonObject{{QStringLiteral("add"), QStringLiteral("polygon")}, {QStringLiteral("layer"), gdsLayerOf(*layer)},
                                   {QStringLiteral("points"), pointsJson(corners)}});
            what << (via ? tr("a via of %1 at (%2, %3) µm, %4 µm square").arg(layer->name, um(r.center().x()), um(r.center().y()), um(r.width()))
                         : tr("a rectangle of %1 from %2").arg(layer->name, boxSaid(r)));
        } else if (add == QLatin1String("polygon")) {
            QJsonObject out{{QStringLiteral("add"), QStringLiteral("polygon")}, {QStringLiteral("layer"), gdsLayerOf(*layer)},
                            {QStringLiteral("points"), e.value(QLatin1String("points"))}};
            gds.append(out);
            what << tr("a polygon of %1 of %n corner(s)", nullptr, int(e.value(QLatin1String("points")).toArray().size())).arg(layer->name);
        } else if (add == QLatin1String("path")) {
            QJsonObject out{{QStringLiteral("add"), QStringLiteral("path")}, {QStringLiteral("layer"), gdsLayerOf(*layer)},
                            {QStringLiteral("points"), e.value(QLatin1String("points"))}, {QStringLiteral("width"), e.value(QLatin1String("width"))}};
            if (e.contains(QLatin1String("ends"))) out.insert(QStringLiteral("ends"), e.value(QLatin1String("ends")));
            gds.append(out);
            what << tr("a path of %1, %2 µm wide").arg(layer->name, um(e.value(QLatin1String("width")).toDouble()));
        } else {
            return bad(tr("what is added is a rectangle, a polygon, a path or a via"));
        }
    }
    mapNode.props.insert(QStringLiteral("rows"), rows);

    // The file as it was, for Undo; then begun, and the shapes written.
    const qint64 size = begun ? 0 : QFileInfo(path).size();
    const bool undoable = size <= kMostKept;
    const std::optional<QByteArray> was = begun || !undoable ? std::nullopt : bytesOf(path);
    QString why;
    if (begun && !qucs_s::layout::newLayout(path, cell, &why)) return fail(tr("%1 could not be begun: %2").arg(path, why));
    const qucs_s::layout::EditResult r = qucs_s::layout::editLayout(path, cell, gds);
    if (!r.ok) {
        if (begun) QFile::remove(path);
        QString text = r.error;
        // (Its own words are of the file's layers; the shape's number is the
        // same, one for one, but for a deletion's several.)
        return fail(tr("Nothing was drawn: %1").arg(text));
    }
    const std::optional<QByteArray> now = undoable ? bytesOf(path) : std::nullopt;
    if (begun) said.prepend(tr("Begun %1 beside the model (its Layout names it), cell %2.").arg(QFileInfo(path).fileName(), cell));
    const QString headline = tr("Drew in %1: %2.").arg(QFileInfo(path).fileName(), what.join(QStringLiteral("; ")));
    said.prepend(headline);
    if (!undoable) said << tr("The file is larger than %1 MB: this cannot be undone.").arg(kMostKept >> 20);
    if (done != nullptr) *done = said;

    // One step of Undo: the model as it was and the file's bytes.
    const Model before = a_model;
    if (undoable) {
        QPointer<RF25Doc> self = this;
        a_undo->push(new DrawnCommand(
            [self](const Model& model, const QString& file, const std::optional<QByteArray>& bytes) {
                if (self) self->applyDrawn(model, file, bytes);
            },
            path, before, was, m, now, what.size() == 1 ? what.first() : tr("%n shape(s) drawn", nullptr, int(what.size()))));
    } else if (m.toObject() != a_model.toObject()) {
        setModel(m, tr("a layer map row"));
    }
    a_keepView = true;
    if (undoable && m.toObject() != a_model.toObject()) applyModel(m);
    a_drawnSaid = said.join(QLatin1Char(' '));
    refreshLayout();
    a_status->setText(a_drawnSaid);
    return true;
}

bool RF25Doc::deleteSelectedShape(QString* why)
{
    const auto fail = [why](const QString& text) {
        if (why != nullptr) *why = text;
        return false;
    };
    const std::optional<qucs_s::layout::Found>& found = a_view->selection();
    if (!found || found->isText() || found->shape < 0 || !a_layout) return fail(tr("No shape is selected: click one first."));
    // One of the cell's own: a shape of a cell it places is that cell's.
    if (found->cell != a_view->cell())
        return fail(tr("That shape is %1's, which this cell places: it is deleted there, in the layout's own tab.")
                        .arg(a_layout->cells.at(found->cell).name));
    const qucs_s::layout::Cell& own = a_layout->cells.at(found->cell);
    const qucs_s::layout::Shape& shape = own.shapes.at(found->shape);
    const QString key = a_layout->layers.at(int(shape.layer)).key.text();
    // The layer of the stack its layer of the file is sent to.
    QString to;
    if (const Node* map = a_model.layout().child(QStringLiteral("rf_layer_map")))
        for (const QJsonValue& row : map->props.value(QLatin1String("rows")).toArray()) {
            const QJsonArray cells = row.toArray();
            if (cells.size() >= 3
                && QStringLiteral("%1/%2").arg(cells.at(0).toString().trimmed(), cells.at(1).toString().trimmed()) == key)
                to = cells.at(2).toString().trimmed();
        }
    if (to.isEmpty() || a_model.stack().named(to) == nullptr)
        return fail(tr("That shape is on %1, which the layer map sends to no layer of the stack: it is deleted in the layout's own tab.").arg(key));
    // Picked by its own box.
    QStringList done;
    QString error;
    const QJsonArray box{shape.bounds.left(), shape.bounds.top(), shape.bounds.right(), shape.bounds.bottom()};
    const bool ok = drawShapes(QJsonArray{QJsonObject{{QStringLiteral("delete"), QStringLiteral("shape")},
                                                      {QStringLiteral("layer"), to},
                                                      {QStringLiteral("box_is"), box}}},
                               &done, &error);
    if (!ok) return fail(error);
    a_view->select(std::nullopt);
    a_status->setText(done.join(QLatin1Char(' ')));
    return true;
}
