/*
 * qucscontrol_layout.cpp - Claude's tools for GDSII and OASIS layouts:
 *                          get_layout reads one, find_shapes finds its
 *                          shapes by place and layer, show_layout shows a
 *                          cell, a region or layers of it in its tab
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "qucscontrol.h"
#include "qucscontrol_p.h"

#include "graphicsexport.h"
#include "layoutdoc.h"
#include "misc.h"
#include "qucs.h"
#include "rf25doc.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImageWriter>
#include <QJsonArray>
#include <QMap>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QPointer>
#include <QRegularExpression>
#include <QSvgGenerator>
#include <QThread>

#include <cmath>
#include <memory>

using namespace qucs_s::control;
using namespace qucs_s::layout;

namespace {

// The layouts the tools read without a tab, kept while their files are
// as they were: find_shapes after get_layout reads the file once.
struct Kept {
    QString file;
    QDateTime modified;
    qint64 size = 0;
    std::shared_ptr<const Layout> layout;
};
QList<Kept>& keptLayouts()
{
    static QList<Kept> kept;
    return kept;
}

std::shared_ptr<const Layout> keptLayout(const QString& file)
{
    const QFileInfo info(file);
    for (const Kept& k : keptLayouts())
        if (k.file == info.absoluteFilePath() && k.modified == info.lastModified() && k.size == info.size()) return k.layout;
    return nullptr;
}

void keepLayout(const QString& file, const std::shared_ptr<const Layout>& layout)
{
    const QFileInfo info(file);
    QList<Kept>& kept = keptLayouts();
    kept.removeIf([&](const Kept& k) { return k.file == info.absoluteFilePath(); });
    kept.prepend({info.absoluteFilePath(), info.lastModified(), info.size(), layout});
    while (kept.size() > 2) kept.removeLast();
}

// A number of µm on the database grid, as JSON.
double onGrid(const Layout& layout, double v)
{
    const double g = layout.dbu > 0 ? layout.dbu : 0.001;
    const double r = std::round(v / g) * g;
    // (to the grid's decimals: 0.1 + 0.2 is 0.3, not 0.30000000000000004)
    return micrometres(layout, r).toDouble();
}

QJsonArray pointJson(const Layout& layout, QPointF p)
{
    return {onGrid(layout, p.x()), onGrid(layout, p.y())};
}

QJsonArray boxJson(const Layout& layout, const QRectF& r)
{
    return {onGrid(layout, r.left()), onGrid(layout, r.top()), onGrid(layout, r.right()), onGrid(layout, r.bottom())};
}

QString layerName(const Layout& layout, const QVector<LayerStyle>& styles, int layer)
{
    const QString name = layer < styles.size() ? styles.at(layer).name : QString();
    return name.isEmpty() ? layout.layers.at(layer).name : name;
}

// Which layers \a spec names: "1/0", "1" (any datatype), a name, "*"; or
// a list of them. Empty, and why in \a error, when one names none.
QVector<bool> layersOf(const Layout& layout, const QVector<LayerStyle>& styles, const QJsonValue& spec, QString* error)
{
    QVector<bool> on(layout.layers.size(), false);
    QStringList asked;
    if (spec.isArray())
        for (const QJsonValue& v : spec.toArray()) asked << (v.isDouble() ? QString::number(v.toInt()) : v.toString().trimmed());
    else asked << (spec.isDouble() ? QString::number(spec.toInt()) : spec.toString().trimmed());
    for (const QString& a : std::as_const(asked)) {
        if (a.isEmpty()) continue;
        bool any = false;
        const QString layerPart = a.section(QLatin1Char('/'), 0, 0).trimmed();
        const QString typePart = a.contains(QLatin1Char('/')) ? a.section(QLatin1Char('/'), 1).trimmed() : QStringLiteral("*");
        bool numL = false, numT = false;
        const uint l = layerPart.toUInt(&numL);
        const uint t = typePart.toUInt(&numT);
        for (int i = 0; i < layout.layers.size(); ++i) {
            const LayerKey& k = layout.layers.at(i).key;
            bool match = a == QLatin1String("*");
            if (numL && (numT || typePart == QLatin1String("*"))) match = match || (k.layer == l && (!numT || k.datatype == t));
            const QString name = layerName(layout, styles, i);
            if (!name.isEmpty() && name.compare(a, Qt::CaseInsensitive) == 0) match = true;
            if (match) on[i] = any = true;
        }
        if (!any) {
            QStringList have;
            for (int i = 0; i < layout.layers.size() && i < 30; ++i) {
                const QString name = layerName(layout, styles, i);
                have << (name.isEmpty() ? layout.layers.at(i).key.text() : QStringLiteral("%1 %2").arg(layout.layers.at(i).key.text(), name));
            }
            *error = tr("No layer is %1. The layers: %2%3.").arg(a, have.join(QStringLiteral(", ")),
                                                                  layout.layers.size() > 30 ? tr(" and %1 more").arg(layout.layers.size() - 30) : QString());
            return {};
        }
    }
    return on;
}

QString formatName(const Layout& layout)
{
    if (layout.format == Layout::Format::Oasis) return QStringLiteral("OASIS");
    return layout.compressed ? QStringLiteral("GDSII (gzipped)") : QStringLiteral("GDSII");
}

// The cell named \a name, else the one the tab shows, else the main one.
int cellOf(const Layout& layout, const QString& name, int shown, QString* error)
{
    if (name.isEmpty()) return shown >= 0 ? shown : layout.mainCell();
    const int c = layout.cellIndex(name);
    if (c >= 0) return c;
    // Any case, when that is one cell.
    int match = -1;
    for (int i = 0; i < layout.cells.size(); ++i)
        if (layout.cells.at(i).name.compare(name, Qt::CaseInsensitive) == 0) {
            if (match >= 0) {
                match = -2;
                break;
            }
            match = i;
        }
    if (match >= 0) return match;
    QStringList top;
    for (int t : layout.topCells.mid(0, 10)) top << layout.cells.at(t).name;
    *error = tr("There is no cell %1 in %2 (its top cells: %3).").arg(name, QFileInfo(layout.file).fileName(), top.join(QStringLiteral(", ")));
    return -1;
}

// A shape or a text as find_shapes gives it - and the selection, alike: its
// kind, layer, the cell it is in and the cells it is placed through, its
// points (a path's width, spine and ends; a text's text and place), which
// copy of a repetition, its properties.
QJsonObject foundJson(const Layout& layout, const QVector<LayerStyle>& styles, const Found& f)
{
    const Cell& c = layout.cells.at(f.cell);
    QJsonObject s{{QStringLiteral("kind"), kindName(f)}};
    const quint32 layer = f.isText() ? c.labels.at(f.label).layer : c.shapes.at(f.shape).layer;
    s.insert(QStringLiteral("layer"), layout.layers.at(int(layer)).key.text());
    const QString name = layerName(layout, styles, int(layer));
    if (!name.isEmpty()) s.insert(QStringLiteral("layer name"), name);
    s.insert(QStringLiteral("in"), c.name);
    if (f.via.size() > 1) {
        QStringList via;
        for (int v : f.via) via << layout.cells.at(v).name;
        s.insert(QStringLiteral("via"), via.join(QStringLiteral(" > ")));
    }
    qint32 props = -1;
    qint32 rep = -1;
    if (f.isText()) {
        const Label& l = c.labels.at(f.label);
        s.insert(QStringLiteral("kind"), QStringLiteral("text"));
        s.insert(QStringLiteral("text"), l.text);
        s.insert(QStringLiteral("at"), pointJson(layout, f.transform.map(l.origin)));
        props = l.properties;
        rep = l.repetition;
    } else {
        const qucs_s::layout::Shape& sh = c.shapes.at(f.shape);
        const QPolygonF outline = outlineOf(layout, f);
        s.insert(QStringLiteral("kind"), sh.kind == ShapeKind::Path ? QStringLiteral("path") : QStringLiteral("polygon"));
        s.insert(QStringLiteral("bounds"), boxJson(layout, outline.boundingRect()));
        QJsonArray points;
        for (int k = 0; k < outline.size() && k < 200; ++k) points.append(pointJson(layout, outline.at(k)));
        s.insert(QStringLiteral("points"), points);
        if (outline.size() > 200) s.insert(QStringLiteral("points not listed"), int(outline.size() - 200));
        if (sh.kind == ShapeKind::Path && sh.path >= 0) {
            const PathInfo& p = c.paths.at(sh.path);
            s.insert(QStringLiteral("width"), onGrid(layout, p.width * std::sqrt(std::abs(f.transform.determinant()))));
            QJsonArray spine;
            for (const QPointF& pt : spineOf(layout, f)) spine.append(pointJson(layout, pt));
            s.insert(QStringLiteral("spine"), spine);
            if (!p.ends.isEmpty()) s.insert(QStringLiteral("ends"), p.ends);
        }
        props = sh.properties;
        rep = sh.repetition;
    }
    if (rep >= 0) s.insert(QStringLiteral("copy"), QStringLiteral("%1 of %2").arg(f.copy + 1).arg(c.repetitions.at(rep).count()));
    if (props >= 0) {
        QJsonObject p;
        for (const auto& [n, v] : c.properties.at(props)) p.insert(n, v);
        s.insert(QStringLiteral("properties"), p);
    }
    return s;
}

// A ruler: its ends, its length and its run across and up, its angle from
// the x axis (counterclockwise, degrees) - µm, the cell shown's.
QJsonObject rulerJson(const Layout& layout, const QLineF& r)
{
    const QPointF d = r.p2() - r.p1();
    double angle = std::atan2(d.y(), d.x()) * 180 / M_PI;
    if (angle < 0) angle += 360;
    return {{QStringLiteral("from"), pointJson(layout, r.p1())},
            {QStringLiteral("to"), pointJson(layout, r.p2())},
            {QStringLiteral("length"), onGrid(layout, std::hypot(d.x(), d.y()))},
            {QStringLiteral("across"), onGrid(layout, d.x())},
            {QStringLiteral("up"), onGrid(layout, d.y())},
            {QStringLiteral("angle"), std::round(angle * 1000) / 1000}};
}

// What a layout's tab has selected and measured - the user's or Claude's -
// into \a o: 'selected' as find_shapes gives a shape, 'rulers'.
void insertSelectionAndRulers(QJsonObject& o, const Layout& layout, const LayoutView* view)
{
    if (view->selection()) o.insert(QStringLiteral("selected"), foundJson(layout, view->styles(), *view->selection()));
    if (!view->rulers().isEmpty()) {
        QJsonArray rulers;
        for (const QLineF& r : view->rulers())
            if (r.length() > 0) rulers.append(rulerJson(layout, r));
        if (!rulers.isEmpty()) o.insert(QStringLiteral("rulers"), rulers);
    }
}

QString themeName(LayoutView::Theme theme)
{
    switch (theme) {
    case LayoutView::Theme::Light: return QStringLiteral("light");
    case LayoutView::Theme::Dark: return QStringLiteral("dark");
    case LayoutView::Theme::Application: break;
    }
    return QStringLiteral("application");
}

QString gridStyleName(LayoutView::GridStyle style)
{
    switch (style) {
    case LayoutView::GridStyle::Lines: return QStringLiteral("lines");
    case LayoutView::GridStyle::Crosses: return QStringLiteral("crosses");
    case LayoutView::GridStyle::Dots: break;
    }
    return QStringLiteral("dots");
}

/// The grid as \a view draws it: points, lines or crosses - it said "a
/// point every" of lines too (the check of 3 November, 6) - and the stronger
/// ones where it has them.
QString gridSaid(const Layout& layout, const LayoutView* view)
{
    const double step = view->gridStep(), major = view->gridMajorStep();
    const QString each = micrometres(layout, step);
    QString what;
    switch (view->gridStyle()) {
    case LayoutView::GridStyle::Lines: what = QObject::tr("a line every %1 µm across and up").arg(each); break;
    case LayoutView::GridStyle::Crosses: what = QObject::tr("a cross every %1 µm").arg(each); break;
    case LayoutView::GridStyle::Dots: what = QObject::tr("a point every %1 µm").arg(each); break;
    }
    // (None stronger when the step is the database unit and that is not 1, 2
    // or 5 of a power of ten: LayoutView::paintGrid.)
    const double ratio = major / step;
    if (std::abs(ratio - std::round(ratio)) < 1e-6 && std::llround(ratio) > 1)
        what += QObject::tr(", stronger every %1 µm").arg(micrometres(layout, major));
    return what + QObject::tr("; a scale in the lower left");
}

} // namespace

// ----------------------------------------------------------------------
// The layout of a call: its tab's, a kept one, or read now.

void QucsControl::withLayout(const QJsonObject& args, bool open,
                             std::function<void(std::shared_ptr<const Layout>, LayoutDoc*)> then, const Done& done)
{
    QString file = args.value(QLatin1String("path")).toString().trimmed();
    if (file.isEmpty()) {
        if (auto* doc = qobject_cast<LayoutDoc*>(a_app->DocumentTab->count() > 0 ? a_app->DocumentTab->currentWidget() : nullptr))
            file = doc->getDocName();
        if (file.isEmpty()) {
            done(errorResult(tr("Which layout? 'path' (the document in front is none).")));
            return;
        }
    }
    file = absolute(file);
    if (!QFileInfo(file).isFile()) {
        done(errorResult(tr("There is no file %1.").arg(QDir::toNativeSeparators(file))));
        return;
    }
    LayoutDoc* doc = nullptr;
    for (QucsDoc* d : a_app->allDocuments())
        if (auto* l = dynamic_cast<LayoutDoc*>(d); l != nullptr && sameFile(l->getDocName(), file)) doc = l;
    if (doc == nullptr && open) {
        QStringList said;
        bool opened = false;
        {
            misc::ErrorCapture capture;
            opened = a_app->gotoPage(file, false, false);
            said = capture.errors();
        }
        doc = opened ? qobject_cast<LayoutDoc*>(a_app->DocumentTab->currentWidget()) : nullptr;
        if (doc == nullptr) {
            done(errorResult(tr("%1 could not be opened as a layout%2").arg(QDir::toNativeSeparators(file),
                                                                              said.isEmpty() ? QStringLiteral(".") : QStringLiteral(": ") + said.join(QStringLiteral("; ")))));
            return;
        }
    }
    if (doc != nullptr) {
        if (open) a_app->showDocument(doc);
        if (!doc->isLoading()) {
            if (!doc->layout()) {
                done(errorResult(doc->problem().isEmpty() ? tr("%1 could not be read.").arg(QFileInfo(file).fileName()) : doc->problem()));
                return;
            }
            then(doc->layout(), doc);
            return;
        }
        // Still being read: when it is.
        QPointer<LayoutDoc> waiting = doc;
        auto connection = std::make_shared<QMetaObject::Connection>();
        *connection = connect(doc, &LayoutDoc::loaded, this, [waiting, connection, then, done, file](bool ok) {
            QObject::disconnect(*connection);
            if (!waiting) return;
            if (!ok || !waiting->layout()) {
                done(errorResult(waiting->problem().isEmpty() ? tr("%1 could not be read.").arg(QFileInfo(file).fileName()) : waiting->problem()));
                return;
            }
            then(waiting->layout(), waiting);
        });
        return;
    }
    if (auto kept = keptLayout(file)) {
        then(kept, nullptr);
        return;
    }
    // Read in the background: a large file holds nothing up.
    struct Reading {
        std::shared_ptr<Layout> layout;
        QString error;
    };
    auto reading = std::make_shared<Reading>();
    QThread* thread = QThread::create([reading, file] { reading->layout = read(file, &reading->error); });
    thread->setStackSize(8u << 20);   // (a main thread's: a thread's 512 kB on macOS - the fuzz of 21 October, E3)
    connect(thread, &QThread::finished, this, [reading, file, then, done] {
        if (!reading->layout) {
            done(errorResult(reading->error));
            return;
        }
        keepLayout(file, reading->layout);
        then(reading->layout, nullptr);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

// ----------------------------------------------------------------------
// get_layout

void QucsControl::getLayout(const QJsonObject& args, const Done& done)
{
    const QString root = args.value(QLatin1String("cell")).toString().trimmed();
    const int levels = std::clamp(args.value(QLatin1String("depth")).toInt(3), 0, 50);
    withLayout(args, false, [root, levels, done](std::shared_ptr<const Layout> layout, LayoutDoc* doc) {
        const QVector<LayerStyle> styles = doc != nullptr ? doc->view()->styles()
                                                          : stylesFor(*layout, readLayerProperties(layerPropertiesBeside(layout->file), nullptr));
        QString error;
        const int start = root.isEmpty() ? -1 : cellOf(*layout, root, -1, &error);
        if (!root.isEmpty() && start < 0) {
            done(errorResult(error));
            return;
        }
        QJsonObject result{{QStringLiteral("file"), QDir::toNativeSeparators(layout->file)},
                           {QStringLiteral("format"), formatName(*layout)},
                           {QStringLiteral("database unit"), micrometres(*layout, layout->dbu) + QStringLiteral(" µm")},
                           {QStringLiteral("cells"), layout->cells.size()},
                           {QStringLiteral("shapes"), double(layout->shapeCount)},
                           {QStringLiteral("texts"), double(layout->labelCount)},
                           {QStringLiteral("placements"), double(layout->instanceCount)},
                           {QStringLiteral("counted"), tr("in the cells' definitions: a cell placed twice counts once; each copy of a "
                                                          "repetition or array counts")}};
        if (!layout->library.isEmpty()) result.insert(QStringLiteral("library"), layout->library);
        QJsonArray tops;
        for (int t : layout->topCells) {
            const Cell& c = layout->cells.at(t);
            tops.append(QJsonObject{{QStringLiteral("cell"), c.name},
                                    {QStringLiteral("bounds"), boxJson(*layout, c.bounds)},
                                    {QStringLiteral("size"), QStringLiteral("%1 × %2 µm").arg(micrometres(*layout, c.bounds.width()),
                                                                                             micrometres(*layout, c.bounds.height()))},
                                    {QStringLiteral("levels"), c.depth}});
            if (tops.size() >= 50) break;
        }
        result.insert(QStringLiteral("top cells"), tops);
        if (layout->topCells.size() > 50) result.insert(QStringLiteral("top cells not listed"), layout->topCells.size() - 50);
        // The tree: each cell's cells, with how often each is placed.
        int nodes = 0;
        bool cut = false;
        std::function<QJsonObject(int, int)> tree = [&](int c, int level) {
            const Cell& cell = layout->cells.at(c);
            QJsonObject node{{QStringLiteral("cell"), cell.name}};
            QMap<QString, QPair<int, quint64>> placed;
            for (const Instance& inst : cell.instances) {
                if (inst.cell < 0) continue;
                auto& e = placed[layout->cells.at(inst.cell).name];
                e.first = inst.cell;
                e.second += inst.repetition >= 0 ? cell.repetitions.at(inst.repetition).count() : 1;
            }
            if (placed.isEmpty()) return node;
            if (level >= levels || nodes >= 300) {
                node.insert(QStringLiteral("places"), tr("%n cell(s), not listed", "", int(placed.size())));
                cut = true;
                return node;
            }
            QJsonArray children;
            for (auto it = placed.cbegin(); it != placed.cend(); ++it) {
                ++nodes;
                QJsonObject child = tree(it.value().first, level + 1);
                child.insert(QStringLiteral("placed"), double(it.value().second));
                children.append(child);
            }
            node.insert(QStringLiteral("cells"), children);
            return node;
        };
        QJsonArray trees;
        if (start >= 0) trees.append(tree(start, 0));
        else
            for (int t : layout->topCells.mid(0, 10)) trees.append(tree(t, 0));
        result.insert(QStringLiteral("tree"), trees);
        if (cut) result.insert(QStringLiteral("tree note"), tr("cut at %1 level(s) (or 300 cells): 'depth' and 'cell' list more").arg(levels));
        QJsonArray layers;
        for (int i = 0; i < layout->layers.size(); ++i) {
            const LayerInfo& info = layout->layers.at(i);
            QJsonObject l{{QStringLiteral("layer"), info.key.text()}, {QStringLiteral("shapes"), double(info.shapes)}};
            if (info.labels > 0) l.insert(QStringLiteral("texts"), double(info.labels));
            const QString name = layerName(*layout, styles, i);
            if (!name.isEmpty()) l.insert(QStringLiteral("name"), name);
            // (What it is drawn in, to name it by: "the red layer".)
            if (i < styles.size() && styles.at(i).frame.isValid()) l.insert(QStringLiteral("colour"), styles.at(i).frame.name());
            if (i < styles.size() && !styles.at(i).visible) l.insert(QStringLiteral("hidden"), true);
            layers.append(l);
        }
        result.insert(QStringLiteral("layers"), layers);
        if (!layout->missing.isEmpty()) result.insert(QStringLiteral("missing cells"), QJsonArray::fromStringList(layout->missing.mid(0, 50)));
        if (!layout->warnings.isEmpty()) result.insert(QStringLiteral("warnings"), QJsonArray::fromStringList(layout->warnings.mid(0, 20)));
        if (doc != nullptr) {
            const LayoutView* v = doc->view();
            QJsonObject shown{{QStringLiteral("cell"), v->cell() >= 0 ? layout->cells.at(v->cell()).name : QString()},
                              {QStringLiteral("region"), boxJson(*layout, v->visibleRegion())},
                              {QStringLiteral("levels"), doc->depth() < 0 ? QJsonValue(QStringLiteral("all")) : QJsonValue(doc->depth())}};
            QStringList hidden;
            for (int i = 0; i < v->styles().size(); ++i)
                if (!v->styles().at(i).visible) hidden << layout->layers.at(i).key.text();
            if (!hidden.isEmpty()) shown.insert(QStringLiteral("hidden layers"), QJsonArray::fromStringList(hidden));
            if (!doc->layerPropertiesFile().isEmpty()) shown.insert(QStringLiteral("layer properties"), QDir::toNativeSeparators(doc->layerPropertiesFile()));
            if (v->gridShown()) shown.insert(QStringLiteral("grid"), micrometres(*layout, v->gridStep()) + QStringLiteral(" µm"));
            if (v->theme() != LayoutView::Theme::Application) shown.insert(QStringLiteral("theme"), themeName(v->theme()));
            // What the user (or Claude) selected and measured there.
            insertSelectionAndRulers(shown, *layout, v);
            result.insert(QStringLiteral("tab"), shown);
        }
        done(jsonResult(result));
    }, done);
}

// ----------------------------------------------------------------------
// find_shapes

void QucsControl::findShapes(const QJsonObject& args, const Done& done)
{
    const QJsonArray region = args.value(QLatin1String("region")).toArray();
    const QJsonArray at = args.value(QLatin1String("at")).toArray();
    if (args.contains(QLatin1String("region")) && region.size() != 4) {
        done(errorResult(tr("'region' is [x1, y1, x2, y2] in µm.")));
        return;
    }
    if (args.contains(QLatin1String("at")) && at.size() != 2) {
        done(errorResult(tr("'at' is [x, y] in µm.")));
        return;
    }
    const QString cellName = args.value(QLatin1String("cell")).toString().trimmed();
    const bool byLayer = args.contains(QLatin1String("layer"));
    const QJsonValue layerSpec = args.value(QLatin1String("layer"));
    const QJsonValue depthGiven = args.value(QLatin1String("depth"));
    const bool texts = args.value(QLatin1String("texts")).toBool(true);
    // The texts by what they say: one that has it, or matches it with * and
    // ? - as the cell panel's find matches cells' names, in any case.
    const bool byText = args.contains(QLatin1String("text"));
    const QString textWanted = args.value(QLatin1String("text")).toString().trimmed();
    if (byText && textWanted.isEmpty())
        return done(errorResult(tr("'text' is what a text says, or a pattern of it with * and ?: \"VDD\", \"net_*\".")));
    if (byText && !texts) return done(errorResult(tr("'text' finds texts, and 'texts' false finds none: leave one out.")));
    const bool wild = textWanted.contains(QLatin1Char('*')) || textWanted.contains(QLatin1Char('?'));
    const QRegularExpression pattern(wild ? QRegularExpression::wildcardToRegularExpression(textWanted, QRegularExpression::UnanchoredWildcardConversion)
                                          : QRegularExpression::escape(textWanted),
                                     QRegularExpression::CaseInsensitiveOption);
    // Clamped in silence before: 'limit': 0 listed one shape of the four
    // there were, and said "1 listed of 4" as though that was asked (B5).
    if (args.contains(QLatin1String("limit")) && !(args.value(QLatin1String("limit")).toInt(0) >= 1
                                                   && args.value(QLatin1String("limit")).toInt(0) <= 1000))
        return done(errorResult(tr("'limit' is how many shapes to list at most, 1 to 1000 (50 when not given): %1 is none of "
                                   "them.").arg(args.value(QLatin1String("limit")).toInt(0))));
    const int limit = std::clamp(args.value(QLatin1String("limit")).toInt(50), 1, 1000);
    withLayout(args, false, [=](std::shared_ptr<const Layout> layout, LayoutDoc* doc) {
        const QVector<LayerStyle> styles = doc != nullptr ? doc->view()->styles()
                                                          : stylesFor(*layout, readLayerProperties(layerPropertiesBeside(layout->file), nullptr));
        QString error;
        const int cell = cellOf(*layout, cellName, doc != nullptr ? doc->shownCell() : -1, &error);
        if (cell < 0) {
            done(errorResult(error.isEmpty() ? tr("%1 has no cells.").arg(QFileInfo(layout->file).fileName()) : error));
            return;
        }
        Query q;
        q.cell = cell;
        if (byLayer) {
            q.layers = layersOf(*layout, styles, layerSpec, &error);
            if (q.layers.isEmpty()) {
                done(errorResult(error));
                return;
            }
        }
        if (depthGiven.isDouble()) q.depth = std::max(0, depthGiven.toInt());
        q.labels = texts || byText;
        QPointF point;
        const bool atPoint = at.size() == 2;
        const double tolerance = layout->dbu;
        if (atPoint) {
            point = QPointF(at.at(0).toDouble(), at.at(1).toDouble());
            q.region = QRectF(point - QPointF(tolerance, tolerance), QSizeF(2 * tolerance, 2 * tolerance));
        } else if (region.size() == 4) {
            q.region = QRectF(QPointF(region.at(0).toDouble(), region.at(1).toDouble()), QPointF(region.at(2).toDouble(), region.at(3).toDouble()))
                           .normalized();
        }
        constexpr quint64 kCountUpTo = 100000;
        quint64 count = 0;
        QJsonArray shapes;
        search(*layout, q, [&](const Found& f) {
            if (atPoint && !hits(*layout, f, point, tolerance)) return true;
            // (By their text: a text that has it, or matches it with * and ?.)
            if (byText && (!f.isText() || !pattern.match(layout->cells.at(f.cell).labels.at(f.label).text).hasMatch())) return true;
            ++count;
            if (shapes.size() < limit) shapes.append(foundJson(*layout, styles, f));
            return count < kCountUpTo;
        });
        QJsonObject result{{QStringLiteral("cell"), layout->cells.at(cell).name},
                           {QStringLiteral("found"), count >= kCountUpTo ? QJsonValue(tr("%1 or more").arg(kCountUpTo)) : QJsonValue(double(count))},
                           {QStringLiteral("units"), tr("µm, in %1's coordinates").arg(layout->cells.at(cell).name)},
                           {QStringLiteral("shapes"), shapes}};
        if (count > quint64(shapes.size()))
            result.insert(QStringLiteral("note"), tr("%1 listed of %2: 'limit' (up to 1000), a smaller 'region' or a 'layer' for the rest.")
                                                      .arg(shapes.size()).arg(count >= kCountUpTo ? tr("%1 or more").arg(kCountUpTo) : QString::number(count)));
        done(jsonResult(result));
    }, done);
}

// ----------------------------------------------------------------------
// show_layout

void QucsControl::showLayout(const QJsonObject& args, const Done& done)
{
    const QJsonArray region = args.value(QLatin1String("region")).toArray();
    if (args.contains(QLatin1String("region")) && region.size() != 4) {
        done(errorResult(tr("'region' is [x1, y1, x2, y2] in µm.")));
        return;
    }
    const QString cellName = args.value(QLatin1String("cell")).toString().trimmed();
    const bool byLayers = args.contains(QLatin1String("layers"));
    const QJsonValue layersSpec = args.value(QLatin1String("layers"));
    const QJsonValue depth = args.value(QLatin1String("depth"));
    const bool textsGiven = args.contains(QLatin1String("texts"));
    const bool texts = args.value(QLatin1String("texts")).toBool();
    const QJsonValue grid = args.value(QLatin1String("grid"));
    const QJsonValue pen = args.value(QLatin1String("pen"));
    const QString drawings = args.value(QLatin1String("drawings")).toString().trimmed().toLower();
    if (args.contains(QLatin1String("drawings")) && drawings != QLatin1String("clear")) {
        done(errorResult(tr("'drawings' is clear: what the pen drew on the layout taken away (the pen's Undo gives it back).")));
        return;
    }
    // What is selected: what a click at a point selects - the smallest
    // shape or text there, of the layers shown, to the depth drawn; at the
    // same point again, the next one under it -, or none.
    const QJsonValue select = args.value(QLatin1String("select"));
    const bool selectNone = select.isString() && select.toString().trimmed().compare(QLatin1String("none"), Qt::CaseInsensitive) == 0;
    const QJsonArray selectAt = select.toArray();
    if (args.contains(QLatin1String("select")) && !selectNone
        && !(selectAt.size() == 2 && selectAt.at(0).isDouble() && selectAt.at(1).isDouble()))
        return done(errorResult(tr("'select' is [x, y] in µm - what a click there selects (at the same point again: the next "
                                   "one under it) - or none.")));
    // Rulers added - each end snapped as a ruler's drag snaps it, to a
    // shape's corner or edge within a few pixels - or taken away.
    const QJsonValue rulers = args.value(QLatin1String("rulers"));
    const bool clearRulers = rulers.isString() && rulers.toString().trimmed().compare(QLatin1String("clear"), Qt::CaseInsensitive) == 0;
    QList<QLineF> newRulers;
    if (args.contains(QLatin1String("rulers")) && !clearRulers) {
        bool good = rulers.isArray() && !rulers.toArray().isEmpty();
        for (const QJsonValue& r : rulers.toArray()) {
            const QJsonArray a = r.toArray();
            good = good && a.size() == 4 && a.at(0).isDouble() && a.at(1).isDouble() && a.at(2).isDouble() && a.at(3).isDouble();
            if (good) newRulers << QLineF(a.at(0).toDouble(), a.at(1).toDouble(), a.at(2).toDouble(), a.at(3).toDouble());
        }
        if (!good)
            return done(errorResult(tr("'rulers' is a list of [x1, y1, x2, y2] in µm - a ruler from each first point to its "
                                       "second - or clear.")));
    }
    // How the layers look: a colour for a layer, a KLayout .lyp or the
    // palette; the canvas light or dark, the layers' colours in contrast to
    // it or not, the grid's points, lines or crosses (in every layout tab,
    // as its menu does); the file read again first.
    const QJsonObject colours = args.value(QLatin1String("colours")).toObject();
    for (auto it = colours.constBegin(); it != colours.constEnd(); ++it)
        if (!it.value().isString() || !QColor::fromString(it.value().toString().trimmed()).isValid())
            return done(errorResult(tr("'colours': %1 is no colour - a name (\"orange\") or #rrggbb.").arg(it.value().toVariant().toString())));
    const QString lyp = args.value(QLatin1String("layer_properties")).toString().trimmed();
    if (args.contains(QLatin1String("layer_properties")) && lyp.isEmpty())
        return done(errorResult(tr("'layer_properties' is a KLayout .lyp file - beside the layout, or a path - or palette.")));
    const QString lypGiven = lyp.compare(QLatin1String("palette"), Qt::CaseInsensitive) == 0 || QFileInfo(lyp).isAbsolute() ? lyp : absolute(lyp);
    const QString theme = args.value(QLatin1String("theme")).toString().trimmed().toLower();
    if (args.contains(QLatin1String("theme")) && theme != QLatin1String("application") && theme != QLatin1String("light")
        && theme != QLatin1String("dark"))
        return done(errorResult(tr("'theme' is application (the canvas as Qucs-S's theme), light or dark.")));
    const QJsonValue contrast = args.value(QLatin1String("contrast"));
    const QString gridStyle = args.value(QLatin1String("grid_style")).toString().trimmed().toLower();
    if (args.contains(QLatin1String("grid_style")) && gridStyle != QLatin1String("dots") && gridStyle != QLatin1String("lines")
        && gridStyle != QLatin1String("crosses"))
        return done(errorResult(tr("'grid_style' is dots, lines or crosses.")));
    const bool reload = args.value(QLatin1String("reload")).toBool();

    // An RF 2.5D model's own layout, framed in its own tab: it was refused
    // as "could not be opened as a layout", so the model's view could only
    // be zoomed in steps about its middle (the phase A check of 24
    // October). The model is named by its file, or is the one in front.
    {
        const QString path = args.value(QLatin1String("path")).toString().trimmed();
        RF25Doc* model = nullptr;
        if (path.endsWith(QLatin1String(".qrf25"), Qt::CaseInsensitive)) {
            const QJsonObject opened = openDocument(QJsonObject{{QStringLiteral("path"), path}});
            if (opened.value(QStringLiteral("isError")).toBool()) {
                done(opened);
                return;
            }
            model = dynamic_cast<RF25Doc*>(a_app->getDoc());
        } else if (path.isEmpty()) {
            model = dynamic_cast<RF25Doc*>(a_app->getDoc());
        }
        if (model != nullptr) {
            // Which cell, and which layers, are the model's to say - its
            // Layout node and its layer map - and are not set from here.
            for (const char* key : {"cell", "layers", "depth", "pen", "drawings", "select", "rulers", "colours", "layer_properties",
                                    "theme", "contrast", "grid_style", "reload"})
                if (args.contains(QLatin1String(key))) {
                    done(errorResult(tr("'%1' is of a layout's own tab: an RF 2.5D model's cell is its Layout node's and "
                                        "its layers are its layer map's. 'region', 'texts', 'grid', 'processed' and "
                                        "'mesh' frame what it shows.").arg(QLatin1String(key))));
                    return;
                }
            if (!model->waitForLayout(60000)) {
                done(errorResult(tr("%1's layout is still being read.").arg(titleOf(model))));
                return;
            }
            const std::shared_ptr<const Layout> layout = model->layout();
            if (!layout) {
                done(errorResult(model->layoutProblem().isEmpty() ? tr("%1 names no layout: its Layout node's file says which.").arg(titleOf(model))
                                                                  : model->layoutProblem()));
                return;
            }
            a_app->showDocument(QucsApp::documentWidget(model));
            if (args.contains(QLatin1String("processed"))) model->setProcessedShown(args.value(QLatin1String("processed")).toBool());
            if (args.contains(QLatin1String("mesh"))) model->setMeshShown(args.value(QLatin1String("mesh")).toBool());
            LayoutView* view = model->view();
            if (textsGiven) view->setLabelsShown(texts);
            if (grid.isBool()) view->setGridShown(grid.toBool());
            if (region.size() == 4)
                view->showRegion(QRectF(QPointF(region.at(0).toDouble(), region.at(1).toDouble()),
                                        QPointF(region.at(2).toDouble(), region.at(3).toDouble())).normalized());
            view->repaint();
            const int shown = view->cell();
            QJsonObject result{{QStringLiteral("model"), titleOf(model)},
                               {QStringLiteral("file"), QDir::toNativeSeparators(layout->file)},
                               {QStringLiteral("cell"), shown >= 0 && shown < layout->cells.size() ? layout->cells.at(shown).name : QString()},
                               {QStringLiteral("region"), boxJson(*layout, view->visibleRegion())},
                               {QStringLiteral("processed shapes drawn"), model->processedShown()},
                               {QStringLiteral("mesh drawn"), model->meshShown()},
                               {QStringLiteral("drawn"), QJsonObject{{QStringLiteral("shapes"), double(view->lastStats().polygons)},
                                                                     {QStringLiteral("too small to see, as dots"), double(view->lastStats().dots)},
                                                                     {QStringLiteral("texts"), double(view->lastStats().labels)}}},
                               {QStringLiteral("note"), tr("screenshot shows the tab as the user sees it.")}};
            if (view->gridShown()) result.insert(QStringLiteral("grid"), gridSaid(*layout, view));
            if (view->lastStats().incomplete)
                result.insert(QStringLiteral("incomplete"), tr("too much in sight: drawn in part - a smaller region draws all of it"));
            done(jsonResult(result));
            return;
        }
    }

    // Read again first, where it is open: its read goes on in the
    // background, and what is asked is done once it has ended.
    if (reload) {
        const QString named = args.value(QLatin1String("path")).toString().trimmed();
        LayoutDoc* open = named.isEmpty() ? qobject_cast<LayoutDoc*>(a_app->DocumentTab->count() > 0 ? a_app->DocumentTab->currentWidget() : nullptr)
                                          : nullptr;
        for (QucsDoc* d : a_app->allDocuments())
            if (auto* l = dynamic_cast<LayoutDoc*>(d); !named.isEmpty() && l != nullptr && sameFile(l->getDocName(), absolute(named))) open = l;
        if (open != nullptr && !open->isLoading()) open->reload();
    }

    withLayout(args, true, [=](std::shared_ptr<const Layout> layout, LayoutDoc* doc) {
        if (doc == nullptr) {
            done(errorResult(tr("%1 is not open as a layout.").arg(QFileInfo(layout->file).fileName())));
            return;
        }
        LayoutView* view = doc->view();
        QString error;
        // Its layers' colours from a .lyp - beside the layout when that is
        // where it is -, or the palette's.
        if (!lyp.isEmpty()) {
            if (lyp.compare(QLatin1String("palette"), Qt::CaseInsensitive) == 0) {
                doc->usePalette();
            } else {
                const QString beside = QDir(QFileInfo(layout->file).absolutePath()).filePath(lyp);
                const QString file = QFileInfo(lyp).isRelative() && QFileInfo::exists(beside) ? beside : lypGiven;
                if (!doc->loadLayerProperties(file, &error)) {
                    done(errorResult(tr("%1: %2").arg(QDir::toNativeSeparators(file), error)));
                    return;
                }
            }
        }
        // A colour for each layer named: all of them found before any is set.
        QList<QPair<int, QColor>> recoloured;
        for (auto it = colours.constBegin(); it != colours.constEnd(); ++it) {
            const QColor colour = QColor::fromString(it.value().toString().trimmed());
            const QVector<bool> on = layersOf(*layout, view->styles(), QJsonValue(it.key()), &error);
            if (on.isEmpty()) {
                done(errorResult(error));
                return;
            }
            for (int i = 0; i < on.size(); ++i)
                if (on.at(i)) recoloured.append({i, colour});
        }
        for (const auto& [layer, colour] : std::as_const(recoloured)) doc->setLayerColour(layer, colour);
        // The canvas, in every layout tab as its menu sets it.
        if (!theme.isEmpty())
            doc->setCanvasTheme(theme == QLatin1String("light")  ? LayoutView::Theme::Light
                                : theme == QLatin1String("dark") ? LayoutView::Theme::Dark
                                                                 : LayoutView::Theme::Application);
        if (contrast.isBool()) doc->setColorsAdapted(contrast.toBool());
        if (!gridStyle.isEmpty())
            doc->setGridStyle(gridStyle == QLatin1String("lines")     ? LayoutView::GridStyle::Lines
                              : gridStyle == QLatin1String("crosses") ? LayoutView::GridStyle::Crosses
                                                                      : LayoutView::GridStyle::Dots);
        if (!cellName.isEmpty()) {
            const int cell = cellOf(*layout, cellName, -1, &error);
            if (cell < 0) {
                done(errorResult(error));
                return;
            }
            doc->showCell(cell);
        }
        if (byLayers) {
            const QVector<bool> on = layersSpec.toString() == QLatin1String("all") ? QVector<bool>(layout->layers.size(), true)
                                                                                  : layersOf(*layout, view->styles(), layersSpec, &error);
            if (on.isEmpty() && !layout->layers.isEmpty()) {
                done(errorResult(error));
                return;
            }
            for (int i = 0; i < on.size(); ++i) doc->setLayerVisible(i, on.at(i));
        }
        if (depth.isDouble()) doc->setDepth(depth.toInt());
        else if (depth.toString() == QLatin1String("all")) doc->setDepth(-1);
        if (textsGiven) doc->setLabelsShown(texts);
        if (grid.isBool()) doc->setGridShown(grid.toBool());
        // The pen taken up or put down, and what it drew taken away.
        if (pen.isBool()) doc->penButton()->setChecked(pen.toBool());
        const bool cleared = drawings == QLatin1String("clear") && view->clearDrawing(true);
        if (region.size() == 4) {
            const QRectF r = QRectF(QPointF(region.at(0).toDouble(), region.at(1).toDouble()), QPointF(region.at(2).toDouble(), region.at(3).toDouble()))
                                 .normalized();
            view->showRegion(r);
        } else if (!cellName.isEmpty()) {
            view->fit();
        }
        // What is selected: as a click there selects at the zoom shown; out of
        // sight, brought into view at that zoom (unless a region was asked).
        bool nothingThere = false;
        if (selectNone) {
            view->select(std::nullopt);
        } else if (selectAt.size() == 2) {
            const QPointF at(selectAt.at(0).toDouble(), selectAt.at(1).toDouble());
            nothingThere = !view->selectAt(view->toPixel(at));
            if (!nothingThere && region.size() != 4) {
                const double pad = layout->dbu;
                const QRectF b = outlineOf(*layout, *view->selection()).boundingRect().adjusted(-pad, -pad, pad, pad);
                if (!view->visibleRegion().intersects(b)) view->setView(b.center(), view->scale());
            }
        }
        // The rulers: each end snapped as the ruler's drag snaps it, at the
        // zoom shown.
        if (clearRulers) view->clearRulers();
        for (const QLineF& r : newRulers) view->addRuler(QLineF(view->snapped(r.p1()), view->snapped(r.p2())));
        view->repaint();
        const int shown = view->cell();
        QStringList visible, hidden;
        for (int i = 0; i < view->styles().size(); ++i) (view->styles().at(i).visible ? visible : hidden) << layout->layers.at(i).key.text();
        QJsonObject result{{QStringLiteral("file"), QDir::toNativeSeparators(layout->file)},
                           {QStringLiteral("cell"), shown >= 0 ? layout->cells.at(shown).name : QString()},
                           {QStringLiteral("region"), boxJson(*layout, view->visibleRegion())},
                           {QStringLiteral("levels"), doc->depth() < 0 ? QJsonValue(QStringLiteral("all")) : QJsonValue(doc->depth())},
                           {QStringLiteral("layers shown"), hidden.isEmpty() ? QJsonValue(QStringLiteral("all")) : QJsonValue(QJsonArray::fromStringList(visible))},
                           {QStringLiteral("drawn"), QJsonObject{{QStringLiteral("shapes"), double(view->lastStats().polygons)},
                                                                 {QStringLiteral("too small to see, as dots"), double(view->lastStats().dots)},
                                                                 {QStringLiteral("cells as frames"), double(view->lastStats().frames)},
                                                                 {QStringLiteral("texts"), double(view->lastStats().labels)}}},
                           {QStringLiteral("note"), tr("screenshot shows the tab as the user sees it.")}};
        if (view->gridShown()) result.insert(QStringLiteral("grid"), gridSaid(*layout, view));
        // (Texts in the file, none drawn: the tab's Texts button, which is
        // kept from one session to the next, is off - the check of 3
        // November, 6, read "texts: 0" of a file with texts.)
        if (!view->labelsShown() && layout->labelCount > 0)
            result.insert(QStringLiteral("texts hidden"), tr("the file's cells hold %1 texts; the tab's Texts button is off (the user's choice, "
                                                             "kept for the next tab) - 'texts': true shows them")
                                                              .arg(layout->labelCount));
        if (view->lastStats().incomplete) result.insert(QStringLiteral("incomplete"), tr("too much in sight: drawn in part - a smaller region draws all of it"));
        // What the user drew with the pen, read stroke by stroke - what each
        // is and what it marks, its box in µm - the latest 40; and whether
        // the pen is taken up.
        if (!view->drawing().isEmpty()) insertDrawings(result, view->readDrawing(false));
        if (view->mode() == LayoutView::Mode::Draw) result.insert(QStringLiteral("pen"), tr("taken up: a drag draws on the cell shown"));
        if (cleared) result.insert(QStringLiteral("done"), tr("What the pen drew taken away (the pen's Undo gives it back)."));
        // What is selected and measured there now - the user's or Claude's.
        insertSelectionAndRulers(result, *layout, view);
        if (nothingThere)
            result.insert(QStringLiteral("not selected"), tr("nothing at %1, %2 µm of the layers shown, to the depth drawn")
                                                              .arg(micrometres(*layout, selectAt.at(0).toDouble()),
                                                                   micrometres(*layout, selectAt.at(1).toDouble())));
        // How it looks, where not as it comes.
        if (!doc->layerPropertiesFile().isEmpty())
            result.insert(QStringLiteral("layer properties"), QDir::toNativeSeparators(doc->layerPropertiesFile()));
        else if (!lyp.isEmpty())
            result.insert(QStringLiteral("layer properties"), tr("the palette"));
        if (view->theme() != LayoutView::Theme::Application) result.insert(QStringLiteral("theme"), themeName(view->theme()));
        if (!view->colorsAdapted()) result.insert(QStringLiteral("colours in contrast"), false);
        if (view->gridShown()) result.insert(QStringLiteral("grid style"), gridStyleName(view->gridStyle()));
        if (reload) result.insert(QStringLiteral("read again"), true);
        done(jsonResult(result));
    }, done);
}

// ----------------------------------------------------------------------
// find_placements

void QucsControl::findPlacements(const QJsonObject& args, const Done& done)
{
    const QString placedName = args.value(QLatin1String("cell")).toString().trimmed();
    if (placedName.isEmpty()) return done(errorResult(tr("'cell' names the cell whose placements are found.")));
    const QString inName = args.value(QLatin1String("in")).toString().trimmed();
    const QJsonArray region = args.value(QLatin1String("region")).toArray();
    if (args.contains(QLatin1String("region")) && region.size() != 4) return done(errorResult(tr("'region' is [x1, y1, x2, y2] in µm.")));
    const QJsonValue depthGiven = args.value(QLatin1String("depth"));
    if (args.contains(QLatin1String("limit")) && !(args.value(QLatin1String("limit")).toInt(0) >= 1
                                                   && args.value(QLatin1String("limit")).toInt(0) <= 1000))
        return done(errorResult(tr("'limit' is how many placements to list at most, 1 to 1000 (50 when not given): %1 is none of "
                                   "them.").arg(args.value(QLatin1String("limit")).toInt(0))));
    const int limit = std::clamp(args.value(QLatin1String("limit")).toInt(50), 1, 1000);
    withLayout(args, false, [=](std::shared_ptr<const Layout> layout, LayoutDoc* doc) {
        QString error;
        const int placed = cellOf(*layout, placedName, -1, &error);
        if (placed < 0) {
            done(errorResult(error));
            return;
        }
        const int in = cellOf(*layout, inName, doc != nullptr ? doc->shownCell() : -1, &error);
        if (in < 0) {
            done(errorResult(error.isEmpty() ? tr("%1 has no cells.").arg(QFileInfo(layout->file).fileName()) : error));
            return;
        }
        Query q;
        q.cell = in;
        if (depthGiven.isDouble()) q.depth = std::max(0, depthGiven.toInt());
        if (region.size() == 4)
            q.region = QRectF(QPointF(region.at(0).toDouble(), region.at(1).toDouble()), QPointF(region.at(2).toDouble(), region.at(3).toDouble()))
                           .normalized();
        constexpr quint64 kCountUpTo = 100000;
        quint64 count = 0;
        QJsonArray list;
        const QRectF bounds = layout->cells.at(placed).bounds;
        // (Its captures named, as a long lambda's must be for GCC 12 and 13.)
        placements(*layout, q, placed, [&count, &list, &layout, &bounds, limit](const Placement& p) {
            ++count;
            if (list.size() < limit) {
                const Cell& parent = layout->cells.at(p.parent);
                QJsonObject o{{QStringLiteral("in"), parent.name}};
                if (p.via.size() > 1) {
                    QStringList via;
                    for (int v : p.via) via << layout->cells.at(v).name;
                    o.insert(QStringLiteral("via"), via.join(QStringLiteral(" > ")));
                }
                // Its origin, turn, mirror and magnification as placed in the
                // cell searched - every level's together.
                const QTransform& t = p.transform;
                o.insert(QStringLiteral("origin"), pointJson(*layout, t.map(QPointF(0, 0))));
                double turn = std::atan2(t.m12(), t.m11()) * 180 / M_PI;
                if (turn < 0) turn += 360;
                o.insert(QStringLiteral("rotation"), std::round(turn * 1e6) / 1e6 >= 360 ? 0.0 : std::round(turn * 1e6) / 1e6);
                const double det = t.m11() * t.m22() - t.m12() * t.m21();
                if (det < 0) o.insert(QStringLiteral("mirrored"), true);
                const double magnification = std::sqrt(std::abs(det));
                if (std::abs(magnification - 1) > 1e-9) o.insert(QStringLiteral("magnification"), std::round(magnification * 1e9) / 1e9);
                o.insert(QStringLiteral("bounds"), boxJson(*layout, t.mapRect(bounds)));
                const Instance& inst = parent.instances.at(p.instance);
                if (inst.repetition >= 0)
                    o.insert(QStringLiteral("copy"), QStringLiteral("%1 of %2").arg(p.copy + 1).arg(parent.repetitions.at(inst.repetition).count()));
                list.append(o);
            }
            return count < kCountUpTo;
        });
        QJsonObject result{{QStringLiteral("cell"), layout->cells.at(placed).name},
                           {QStringLiteral("in"), layout->cells.at(in).name},
                           {QStringLiteral("found"), count >= kCountUpTo ? QJsonValue(tr("%1 or more").arg(kCountUpTo)) : QJsonValue(double(count))},
                           {QStringLiteral("units"), tr("µm, in %1's coordinates; rotation in degrees, counterclockwise, after the mirror "
                                                        "about x").arg(layout->cells.at(in).name)},
                           {QStringLiteral("placements"), list}};
        if (count > quint64(list.size()))
            result.insert(QStringLiteral("note"), tr("%1 listed of %2: 'limit' (up to 1000), a 'region' or a smaller 'depth' for the rest.")
                                                      .arg(list.size()).arg(count >= kCountUpTo ? tr("%1 or more").arg(kCountUpTo) : QString::number(count)));
        else if (count == 0)
            result.insert(QStringLiteral("note"), tr("%1 is not placed in %2%3.").arg(layout->cells.at(placed).name, layout->cells.at(in).name,
                                                                                    region.size() == 4 ? tr(" within the region") : QString()));
        done(jsonResult(result));
    }, done);
}

// ----------------------------------------------------------------------
// export_image of a layout: its tab's view, as the tab shows it

bool QucsControl::isLayoutExport(const QJsonObject& args) const
{
    const QString path = args.value(QLatin1String("path")).toString().trimmed();
    if (path.isEmpty()) return qobject_cast<LayoutDoc*>(a_app->DocumentTab->count() > 0 ? a_app->DocumentTab->currentWidget() : nullptr) != nullptr;
    if (isLayoutFile(path)) return true;
    QString error;
    QucsDoc* doc = document(args, &error);   // (its tab's title)
    return doc != nullptr && qobject_cast<LayoutDoc*>(QucsApp::documentWidget(doc)) != nullptr;
}

QJsonObject QucsControl::exportLayout(const QJsonObject& args)
{
    namespace gx = qucs_s::graphicsexport;
    // What is a schematic's or a presentation's alone.
    if (args.contains(QLatin1String("diagram")) || args.contains(QLatin1String("selection")) || args.contains(QLatin1String("slides")))
        return errorResult(tr("'diagram', 'selection' and 'slides' are a schematic's and a presentation's: a layout is written "
                              "as its tab shows it (show_layout sets what that is)."));
    const QString colours = args.value(QLatin1String("colours")).toString(QStringLiteral("colour")).trimmed().toLower();
    if (colours != QLatin1String("colour") && colours != QLatin1String("color"))
        return errorResult(tr("A layout is written in its layers' colours: 'colours' grayscale and monochrome are a schematic's."));
    QString file = args.value(QLatin1String("save_as")).toString().trimmed();
    if (file.isEmpty()) return errorResult(tr("'save_as' names the file to write."));
    if (const QString bad = badFileName(QFileInfo(file).fileName()); !bad.isEmpty()) return errorResult(tr("'save_as': %1.").arg(bad));
    file = absolute(file);
    // Its format: as given, else the file's suffix - a picture, an SVG or a PDF.
    static const QList<QPair<QString, gx::Format>> names{
        {QStringLiteral("png"), gx::Format::Png},   {QStringLiteral("jpeg"), gx::Format::Jpeg}, {QStringLiteral("jpg"), gx::Format::Jpeg},
        {QStringLiteral("bmp"), gx::Format::Bmp},   {QStringLiteral("tiff"), gx::Format::Tiff}, {QStringLiteral("webp"), gx::Format::Webp},
        {QStringLiteral("svg"), gx::Format::Svg},   {QStringLiteral("pdf"), gx::Format::Pdf}};
    const QString wanted = tr("A layout is written as png, jpeg, bmp, tiff, webp, svg or pdf.");
    std::optional<gx::Format> format;
    const QString formatName = args.value(QLatin1String("format")).toString().trimmed().toLower();
    if (!formatName.isEmpty()) {
        for (const auto& [name, f] : names)
            if (name == formatName) format = f;
        if (!format) return errorResult(wanted);
    } else {
        format = gx::formatOf(file);
        if (!format) return errorResult(tr("%1 has no suffix of a picture's format: give 'format', or a name ending in .png, .svg, .pdf, ...")
                                            .arg(QFileInfo(file).fileName()));
        if (*format == gx::Format::Eps || *format == gx::Format::PdfTex) return errorResult(wanted);
    }
    if (!gx::formats().contains(*format)) return errorResult(tr("This build of Qucs-S does not write %1.").arg(gx::description(*format)));
    file = gx::withSuffix(file, *format);
    if (!QFileInfo(QFileInfo(file).absolutePath()).isDir()) return errorResult(tr("There is no folder %1.").arg(QFileInfo(file).absolutePath()));
    const QJsonValue scale = args.value(QLatin1String("scale"));
    if (!scale.isUndefined() && (!scale.isDouble() || scale.toDouble() < 0.1 || scale.toDouble() > 20))
        return errorResult(tr("'scale' is a layout's picture's pixels per pixel of its view, 0.1 to 20 (2 by default)."));
    double factor = scale.isDouble() ? scale.toDouble() : 2.0;
    bool transparent = args.value(QLatin1String("transparent")).toBool();
    QStringList notes;
    if (transparent && !gx::hasTransparency(*format)) {
        transparent = false;
        notes << tr("%1 cannot be transparent: it has its canvas.").arg(gx::description(*format));
    }

    // The layout's tab: open, else opened, and read.
    const QString path = args.value(QLatin1String("path")).toString().trimmed();
    LayoutDoc* doc = nullptr;
    if (path.isEmpty()) {
        doc = qobject_cast<LayoutDoc*>(a_app->DocumentTab->count() > 0 ? a_app->DocumentTab->currentWidget() : nullptr);
    } else {
        QString none;
        if (QucsDoc* d = document(args, &none)) doc = qobject_cast<LayoutDoc*>(QucsApp::documentWidget(d));
        for (QucsDoc* d : a_app->allDocuments())
            if (auto* l = dynamic_cast<LayoutDoc*>(d); doc == nullptr && l != nullptr && sameFile(l->getDocName(), absolute(path))) doc = l;
        if (doc == nullptr && QFileInfo(absolute(path)).isFile() && a_app->gotoPage(absolute(path), false, false))
            doc = qobject_cast<LayoutDoc*>(a_app->DocumentTab->currentWidget());
    }
    if (doc == nullptr) return errorResult(tr("Which layout? 'path' names none that is open, or a file of one."));
    if (doc->isLoading() && !doc->waitForLoaded(60000)) return errorResult(tr("%1 is still being read.").arg(titleOf(doc)));
    const std::shared_ptr<const Layout> layout = doc->layout();
    if (!layout) return errorResult(doc->problem().isEmpty() ? tr("%1 could not be read.").arg(titleOf(doc)) : doc->problem());
    LayoutView* view = doc->view();
    const QSize size = view->size();
    if (size.isEmpty()) return errorResult(tr("%1's view has no size to draw.").arg(titleOf(doc)));
    if (const QString no = aboutToWrite(file); !no.isEmpty()) return errorResult(no);
    const QString cell = view->cell() >= 0 ? layout->cells.at(view->cell()).name : QString();
    const QString title = cell.isEmpty() ? QFileInfo(layout->file).fileName() : tr("%1, %2").arg(QFileInfo(layout->file).fileName(), cell);
    const QString creator = tr("Qucs-S %1").arg(misc::buildName());

    QJsonObject result;
    if (*format == gx::Format::Svg) {
        QSvgGenerator generator;
        generator.setFileName(file);
        generator.setSize(size);
        generator.setViewBox(QRect(QPoint(0, 0), size));
        generator.setResolution(96);
        generator.setTitle(title);
        generator.setDescription(creator);
        QPainter painter;
        if (!painter.begin(&generator)) return errorResult(tr("%1 could not be written.").arg(QDir::toNativeSeparators(file)));
        // (A font of this system's alone is named as the reader's own
        //  sans-serif, as the schematics' SVG names it.)
        if (QFont own = painter.font(); own.family().startsWith(QLatin1Char('.'))) {
            own.setFamilies({QStringLiteral("sans-serif")});
            painter.setFont(own);
        }
        view->paintOnto(painter, !transparent);
        painter.end();
        result.insert(QStringLiteral("size"), QJsonArray{size.width(), size.height()});
    } else if (*format == gx::Format::Pdf) {
        // A page of the view's size, its pixels at 96 dpi; as vectors.
        QPdfWriter writer(file);
        writer.setResolution(96);
        writer.setPageLayout(QPageLayout(QPageSize(QSizeF(size) * 0.75, QPageSize::Point, QString(), QPageSize::ExactMatch),
                                         QPageLayout::Portrait, QMarginsF()));
        writer.setTitle(title);
        writer.setCreator(creator);
        QPainter painter;
        if (!painter.begin(&writer)) return errorResult(tr("%1 could not be written.").arg(QDir::toNativeSeparators(file)));
        view->paintOnto(painter, true);
        painter.end();
        result.insert(QStringLiteral("size in points"), QJsonArray{std::round(size.width() * 0.75 * 100) / 100, std::round(size.height() * 0.75 * 100) / 100});
    } else {
        // At most the schematics' largest picture: scaled down to fit.
        const double w = size.width(), h = size.height();
        const double most = std::min(std::sqrt(gx::MaxImagePixels / (w * h)), gx::MaxImageSide / std::max(w, h));
        if (factor > most) {
            notes << tr("scaled to %1 to stay within %2 megapixels and %3 pixels a side").arg(most, 0, 'g', 3).arg(gx::MaxImagePixels / 1e6).arg(gx::MaxImageSide);
            factor = most;
        }
        QImage image((QSizeF(size) * factor).toSize(), QImage::Format_ARGB32_Premultiplied);
        if (image.isNull()) return errorResult(tr("A picture of %1 by %2 pixels could not be made.").arg(image.width()).arg(image.height()));
        image.setDevicePixelRatio(factor);
        image.fill(Qt::transparent);
        {
            QPainter painter(&image);
            view->paintOnto(painter, !transparent);
        }
        const int dpm = qRound(factor * 96 / 0.0254);
        image.setDotsPerMeterX(dpm);
        image.setDotsPerMeterY(dpm);
        QImageWriter writer(file, gx::suffix(*format).toLatin1());
        if (gx::hasQuality(*format)) writer.setQuality(90);
        if (!writer.write(image)) return errorResult(tr("%1 was not written: %2").arg(QDir::toNativeSeparators(file), writer.errorString()));
        result.insert(QStringLiteral("pixels"), QJsonArray{image.width(), image.height()});
    }
    result.insert(QStringLiteral("written"), QDir::toNativeSeparators(file));
    result.insert(QStringLiteral("format"), gx::description(*format));
    result.insert(QStringLiteral("bytes"), qint64(QFileInfo(file).size()));
    result.insert(QStringLiteral("cell"), cell);
    result.insert(QStringLiteral("region"), boxJson(*layout, view->visibleRegion()));
    if (!notes.isEmpty()) result.insert(QStringLiteral("note"), notes.join(QStringLiteral("; ")));
    return jsonResult(result);
}
