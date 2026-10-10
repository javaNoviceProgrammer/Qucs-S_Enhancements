/*
 * layout.h - a GDSII or OASIS layout, read for its viewer (layoutdoc.h)
 *            and Claude's tools: cells of shapes, paths and texts on
 *            layers, and placements of other cells; read with gdstk,
 *            drawn with QPainter, searched by place
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_LAYOUT_H
#define QUCS_LAYOUT_H

#include <QColor>
#include <QHash>
#include <QList>
#include <QPair>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QTransform>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

class QPainter;

namespace qucs_s::layout {

/// A layer and its datatype (a text's texttype), as GDSII and OASIS
/// number them: 1/0.
struct LayerKey {
    quint32 layer = 0;
    quint32 datatype = 0;
    bool operator==(const LayerKey& o) const { return layer == o.layer && datatype == o.datatype; }
    bool operator!=(const LayerKey& o) const { return !(*this == o); }
    bool operator<(const LayerKey& o) const { return layer != o.layer ? layer < o.layer : datatype < o.datatype; }
    QString text() const { return QStringLiteral("%1/%2").arg(layer).arg(datatype); }
};
inline size_t qHash(const LayerKey& key, size_t seed = 0) { return ::qHash((quint64(key.layer) << 32) | key.datatype, seed); }

/// An element's copies (an OASIS repetition, a GDSII array): \a columns
/// along v1 by \a rows along v2, or at \a offsets. The element itself is
/// the first copy, at (0, 0).
struct Repetition {
    enum class Type : quint8 { Grid, Explicit };
    Type type = Type::Grid;
    quint64 columns = 1;
    quint64 rows = 1;
    QPointF v1, v2;
    QVector<QPointF> offsets;   ///< Explicit: every copy's, (0, 0) first

    quint64 count() const { return type == Type::Grid ? columns * rows : quint64(offsets.size()); }
    QPointF offset(quint64 i) const;
    /// What \a r covers with all its copies.
    QRectF extent(const QRectF& r) const;
    /// Calls \a each with the offset of every copy of \a r that may meet
    /// \a visible - a grid's found without going through all of them -
    /// until it returns false (then false).
    bool forEach(const QRectF& r, const QRectF& visible, const std::function<bool(QPointF)>& each) const;
};

/// Properties of an element or a cell: names and values, as text. A
/// GDSII property is named by its attribute number.
using Properties = QList<QPair<QString, QString>>;

enum class ShapeKind : quint8 { Polygon, Path };

/// A polygon, or a path drawn as its outline: its points are its cell's
/// (Cell::points), in µm.
struct Shape {
    QRectF bounds;              ///< of its outline, without its copies
    quint32 first = 0;          ///< its outline: Cell::points[first, first + count)
    quint32 count = 0;
    quint32 layer = 0;          ///< Layout::layers' index
    ShapeKind kind = ShapeKind::Polygon;
    qint32 repetition = -1;     ///< Cell::repetitions' index
    qint32 properties = -1;     ///< Cell::properties' index
    qint32 path = -1;           ///< a path's: Cell::paths' index
};

/// What a path is besides its outline: its width and its spine.
struct PathInfo {
    double width = 0;           ///< µm
    quint32 first = 0;          ///< its spine: Cell::points[first, first + count)
    quint32 count = 0;
    QString ends;               ///< flush, round, extended or custom
};

/// A text (a GDSII TEXT, an OASIS TEXT): its origin, layer and look.
struct Label {
    QString text;
    QPointF origin;             ///< µm
    quint32 layer = 0;          ///< Layout::layers' index (its texttype as datatype)
    double rotation = 0;        ///< degrees, counterclockwise
    double magnification = 1;
    bool mirrored = false;
    qint32 repetition = -1;
    qint32 properties = -1;
};

/// A placement of another cell (an SREF, an AREF, an OASIS PLACEMENT).
struct Instance {
    qint32 cell = -1;           ///< Layout::cells' index; -1: one the file has not (Layout::missing)
    QPointF origin;             ///< µm
    double rotation = 0;        ///< degrees, counterclockwise
    double magnification = 1;
    bool mirrored = false;      ///< about the x axis, before the rotation
    qint32 repetition = -1;
    qint32 properties = -1;
    QRectF bounds;              ///< in its parent's coordinates, all its copies

    /// The placed cell's coordinates to its parent's (one copy: without
    /// the repetition's offset).
    QTransform transform() const;
};

struct Cell {
    QString name;
    QVector<QPointF> points;    ///< every outline's and spine's, in µm
    QVector<Shape> shapes;
    QVector<PathInfo> paths;
    QVector<Label> labels;
    QVector<Instance> instances;
    QVector<Repetition> repetitions;
    QVector<Properties> properties;
    Properties own;             ///< the cell's own
    QRectF ownBounds;           ///< of its shapes and texts
    QRectF bounds;              ///< and of what it places, to the bottom
    int parents = 0;            ///< how many cells place it
    int depth = 0;              ///< the levels below it, to its deepest cell
};

/// A layer as the file has it: its shapes and texts in the cells'
/// definitions (a cell placed twice counts once; a repetition's copies each).
struct LayerInfo {
    LayerKey key;
    QString name;               ///< the file's (OASIS LAYERNAME), or empty
    quint64 shapes = 0;
    quint64 labels = 0;
};

struct Layout {
    enum class Format { Gds, Oasis };
    QString file;
    Format format = Format::Gds;
    bool compressed = false;    ///< a .gds.gz
    QString library;            ///< its name (GDSII LIBNAME), or empty
    double dbu = 0.001;         ///< the database unit, in µm
    QVector<Cell> cells;
    QVector<int> topCells;      ///< cells no other places, the largest first
    QVector<LayerInfo> layers;  ///< by layer and datatype
    QStringList missing;        ///< cells placed that the file has not
    QStringList warnings;       ///< what gdstk said as it read
    quint64 shapeCount = 0;     ///< in the definitions, as LayerInfo counts
    quint64 labelCount = 0;
    quint64 instanceCount = 0;  ///< placements, each copy of an array one

    int cellIndex(const QString& name) const { return byName.value(name, -1); }
    int layerIndex(LayerKey key) const { return byLayer.value(key, -1); }
    /// The cell a view shows first: the largest top cell.
    int mainCell() const { return topCells.isEmpty() ? (cells.isEmpty() ? -1 : 0) : topCells.first(); }

    QHash<QString, int> byName;     ///< the cells by name
    QHash<LayerKey, int> byLayer;   ///< the layers by layer and datatype
};

/// Whether \a path names a layout this reads, by its suffix: .gds, .gds2,
/// .gdsii, .gds.gz, .oas, .oasis.
bool isLayoutFile(const QString& path);

/// How far a read has come, for a progress bar: -1 while gdstk reads the
/// file (it tells nothing), then 0 to 1000 as its cells are taken.
using Progress = std::atomic<int>;

/// Reads \a path: GDSII, gzipped GDSII or OASIS, told by its first bytes.
/// Null, and why in \a error, when it cannot be read - or when
/// \a cancelled was set (checked between its steps).
std::shared_ptr<Layout> read(const QString& path, QString* error, const std::atomic_bool* cancelled = nullptr,
                             Progress* progress = nullptr);
/// How many reads are going on, in any thread: one cancelled ends at its
/// next step, and the tests wait for it.
int readsGoing();

// ----------------------------------------------------------------------
// How layers look

/// How a layer is drawn: its frame and fill colours, the fill's pattern
/// (Qt::NoBrush: hollow), shown or hidden, and a name (a .lyp's).
struct LayerStyle {
    QColor frame;
    QColor fill;
    Qt::BrushStyle pattern = Qt::SolidPattern;
    bool visible = true;
    QString name;
};

/// The style of the \a index'th layer when nothing says otherwise: one of
/// sixteen colours, and of eight patterns as the colours come round again.
LayerStyle paletteStyle(int index);

/// How far apart two colours are in lightness, as WCAG measures it: from
/// 1 (alike) to 21 (black on white).
double contrastRatio(const QColor& a, const QColor& b);
/// \a colour standing out from \a background by \a ratio at least:
/// darkened on a light background, lightened on a dark one - its hue and
/// saturation kept, and no more than it takes; as it is when it stands out
/// already.
QColor standingOut(const QColor& colour, const QColor& background, double ratio);
/// The contrast a layer is given on the canvas: between WCAG's 3 (for
/// graphics) and 4.5 (for text) - the palette's pastels, made for
/// KLayout's black, stand out on white, and a gold is still gold.
constexpr double LayerContrast = 3.5;
/// \a style with its frame and fill standing out from \a background by
/// LayerContrast; as it is when \a background is none.
LayerStyle standingOut(LayerStyle style, const QColor& background);

/// A KLayout layer properties file (.lyp): each layer's style, by its
/// layer and datatype (a source "1/0@1", "1/0" or "Metal1 1/0"). Empty, and
/// why in \a error, when it cannot be read.
QHash<LayerKey, LayerStyle> readLayerProperties(const QString& path, QString* error);

/// The .lyp that goes with \a layoutFile: its own (chip.lyp beside
/// chip.gds), else the only one in its folder; empty when there is none.
QString layerPropertiesBeside(const QString& layoutFile);

/// The styles of \a layout's layers in order: from \a lyp where it has
/// one, the palette's otherwise; a layer named by the file keeps its name
/// unless the .lyp gives one.
QVector<LayerStyle> stylesFor(const Layout& layout, const QHash<LayerKey, LayerStyle>& lyp);

// ----------------------------------------------------------------------
// Drawing

struct RenderOptions {
    int cell = -1;              ///< the cell shown
    int depth = 1 << 20;        ///< levels drawn below it; deeper cells as frames
    QTransform toDevice;        ///< µm to the device's pixels
    QRectF device;              ///< what is drawn, in the device's pixels
    const QVector<LayerStyle>* styles = nullptr;   ///< by Layout::layers' index
    bool labels = true;
    QColor frames;              ///< cells not drawn, and their names
    QColor text;                ///< the texts
    QColor background;          ///< when given, each layer's colours made to stand out from it (standingOut())
    quint64 budget = 4000000;   ///< shapes at most; beyond them the drawing is incomplete
};

struct RenderStats {
    quint64 polygons = 0;       ///< drawn as outlines
    quint64 dots = 0;           ///< too small: a pixel each
    quint64 frames = 0;         ///< cells deeper than the depth
    quint64 labels = 0;
    bool incomplete = false;    ///< the budget ran out
};

/// Draws \a options.cell of \a layout as \a options say.
RenderStats render(QPainter& painter, const Layout& layout, const RenderOptions& options);

/// toDevice for a view of \a scale pixels per µm with \a center (µm) in
/// the middle of \a device - y upwards, as layouts have it.
QTransform viewTransform(QPointF center, double scale, const QRectF& device);

// ----------------------------------------------------------------------
// Searching

/// A shape or a text found in a cell, or in a cell it places: where it is
/// defined and how it is placed in the cell searched.
struct Found {
    int cell = -1;              ///< the cell it is defined in
    int shape = -1;             ///< Cell::shapes' index, or -1
    int label = -1;             ///< Cell::labels' index, or -1
    quint64 copy = 0;           ///< which copy of its repetition
    QTransform transform;       ///< its cell's coordinates to the searched cell's
    QVector<int> via;           ///< the cells from the searched one down to its own

    bool isText() const { return label >= 0; }
};

struct Query {
    int cell = -1;
    QRectF region;              ///< µm, the cell's coordinates; empty: everywhere
    int depth = 1 << 20;        ///< levels below the cell looked into
    QVector<bool> layers;       ///< by Layout::layers' index; empty: all
    bool labels = true;
};

/// Calls \a each with every shape and text of \a query.cell (and of the
/// cells it places, to the depth) whose bounds meet the region, until it
/// returns false.
void search(const Layout& layout, const Query& query, const std::function<bool(const Found&)>& each);

/// \a found's outline (a text: its origin), in the searched cell's µm.
QPolygonF outlineOf(const Layout& layout, const Found& found);
/// Its spine, for a path; empty otherwise.
QPolygonF spineOf(const Layout& layout, const Found& found);
/// Whether \a point (µm, the searched cell's) is in it, or within
/// \a tolerance of its outline.
bool hits(const Layout& layout, const Found& found, QPointF point, double tolerance);

/// The nearest vertex, else the nearest point of an edge, of a shape of
/// \a query within \a tolerance (µm) of \a point; \a point itself, on
/// the database grid, when there is none. For a ruler.
QPointF snap(const Layout& layout, const Query& query, QPointF point, double tolerance);

/// "polygon", "path" or "text".
QString kindName(const Found& found);
/// A number of µm with as many decimals as the database unit has.
QString micrometres(const Layout& layout, double value);

} // namespace qucs_s::layout

#endif // QUCS_LAYOUT_H
