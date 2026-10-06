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

#include "layoutdoc.h"
#include "misc.h"
#include "qucs.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QMap>
#include <QPointer>
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
        q.labels = texts;
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
            ++count;
            if (shapes.size() < limit) {
                const Cell& c = layout->cells.at(f.cell);
                QJsonObject s{{QStringLiteral("kind"), kindName(f)}};
                const quint32 layer = f.isText() ? c.labels.at(f.label).layer : c.shapes.at(f.shape).layer;
                s.insert(QStringLiteral("layer"), layout->layers.at(int(layer)).key.text());
                const QString name = layerName(*layout, styles, int(layer));
                if (!name.isEmpty()) s.insert(QStringLiteral("layer name"), name);
                s.insert(QStringLiteral("in"), c.name);
                if (f.via.size() > 1) {
                    QStringList via;
                    for (int v : f.via) via << layout->cells.at(v).name;
                    s.insert(QStringLiteral("via"), via.join(QStringLiteral(" > ")));
                }
                qint32 props = -1;
                qint32 rep = -1;
                if (f.isText()) {
                    const Label& l = c.labels.at(f.label);
                    s.insert(QStringLiteral("kind"), QStringLiteral("text"));
                    s.insert(QStringLiteral("text"), l.text);
                    s.insert(QStringLiteral("at"), pointJson(*layout, f.transform.map(l.origin)));
                    props = l.properties;
                    rep = l.repetition;
                } else {
                    const qucs_s::layout::Shape& sh = c.shapes.at(f.shape);
                    const QPolygonF outline = outlineOf(*layout, f);
                    s.insert(QStringLiteral("kind"), sh.kind == ShapeKind::Path ? QStringLiteral("path") : QStringLiteral("polygon"));
                    s.insert(QStringLiteral("bounds"), boxJson(*layout, outline.boundingRect()));
                    QJsonArray points;
                    for (int k = 0; k < outline.size() && k < 200; ++k) points.append(pointJson(*layout, outline.at(k)));
                    s.insert(QStringLiteral("points"), points);
                    if (outline.size() > 200) s.insert(QStringLiteral("points not listed"), int(outline.size() - 200));
                    if (sh.kind == ShapeKind::Path && sh.path >= 0) {
                        const PathInfo& p = c.paths.at(sh.path);
                        s.insert(QStringLiteral("width"), onGrid(*layout, p.width * std::sqrt(std::abs(f.transform.determinant()))));
                        QJsonArray spine;
                        for (const QPointF& pt : spineOf(*layout, f)) spine.append(pointJson(*layout, pt));
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
                shapes.append(s);
            }
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
    withLayout(args, true, [=](std::shared_ptr<const Layout> layout, LayoutDoc* doc) {
        if (doc == nullptr) {
            done(errorResult(tr("%1 is not open as a layout.").arg(QFileInfo(layout->file).fileName())));
            return;
        }
        LayoutView* view = doc->view();
        QString error;
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
        if (textsGiven) view->setLabelsShown(texts);
        if (region.size() == 4) {
            const QRectF r = QRectF(QPointF(region.at(0).toDouble(), region.at(1).toDouble()), QPointF(region.at(2).toDouble(), region.at(3).toDouble()))
                                 .normalized();
            view->showRegion(r);
        } else if (!cellName.isEmpty()) {
            view->fit();
        }
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
        if (view->lastStats().incomplete) result.insert(QStringLiteral("incomplete"), tr("too much in sight: drawn in part - a smaller region draws all of it"));
        done(jsonResult(result));
    }, done);
}
