/*
 * rf25doc.cpp - an RF 2.5D model in a tab of Qucs-S
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "rf25doc.h"

#include "main.h"
#include "misc.h"
#include "multiphysics/fem_expr.h"
#include "qucs.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPrinter>
#include <QSaveFile>
#include <QSplitter>
#include <QThread>
#include <QToolButton>
#include <QUndoCommand>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

using namespace qucs_s::rf25;
using qucs_s::layout::LayerStyle;
using qucs_s::layout::Layout;
using qucs_s::layout::LayoutView;

namespace {

/// A change of the model: the model before and after, whole (it is small).
class ModelCommand : public QUndoCommand
{
public:
    ModelCommand(std::function<void(const Model&)> apply, Model before, Model after, const QString& what)
        : QUndoCommand(what), a_apply(std::move(apply)), a_before(std::move(before)), a_after(std::move(after))
    {}
    void undo() override { a_apply(a_before); }
    void redo() override
    {
        // (The first time, it is applied already.)
        if (a_first) a_first = false;
        else a_apply(a_after);
    }

private:
    std::function<void(const Model&)> a_apply;
    Model a_before, a_after;
    bool a_first = true;
};

} // namespace

// ------------------------------------------------------------------ The stack

namespace qucs_s::rf25 {

StackView::StackView(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("rfStackView"));
    setMinimumWidth(90);
    setToolTip(tr("The stack from the bottom up, each layer as tall as it is thick. A click chooses it."));
}

void StackView::setStack(const Stack& stack)
{
    a_stack = stack;
    update();
}

void StackView::setFocused(const QString& tag)
{
    if (a_focus == tag) return;
    a_focus = tag;
    update();
}

QList<QPair<QString, QRectF>> StackView::places() const
{
    QList<QPair<QString, QRectF>> out;
    if (a_stack.layers.isEmpty()) return out;
    const double top = 6, bottom = height() - 6.0, left = 6, right = width() - 6.0;
    const double room = std::max(20.0, bottom - top);
    // The thick layers share the room by thickness, with a floor under
    // each: a 0.6 µm metal beside a 300 µm substrate would be a hair
    // otherwise. The planes and the vias take no room of their own.
    double total = 0;
    int thick = 0;
    for (const Layer& l : a_stack.layers)
        if (l.z1 > l.z0) {
            total += l.z1 - l.z0;
            ++thick;
        }
    const double floorEach = thick > 0 ? std::min(14.0, room / (thick * 2.0)) : 0;
    const double share = std::max(0.0, room - floorEach * thick);
    double y = bottom;
    for (const Layer& l : a_stack.layers) {
        if (l.z1 <= l.z0) continue;
        const double h = floorEach + (total > 0 ? share * (l.z1 - l.z0) / total : 0);
        out.append({l.tag, QRectF(left, y - h, std::max(10.0, right - left), h)});
        y -= h;
    }
    // A ground plane under the stack, a cover over it where they have no
    // thickness, and the vias at the right edge over the room between
    // their metals.
    for (const Layer& l : a_stack.layers) {
        if (l.z1 > l.z0) continue;
        if (l.kind == Layer::Via) {
            const Layer* from = a_stack.named(l.from);
            const Layer* to = a_stack.named(l.to);
            double y0 = l.z0, y1 = l.z0;
            if (from != nullptr && to != nullptr) {
                y0 = std::min(from->z1, to->z1);
                y1 = std::max(from->z0, to->z0);
            }
            // (The z of the layers, into pixels: through the thick layers' places.)
            const auto pixelOf = [&out, this](double z) {
                for (const auto& [tag, place] : out) {
                    const Layer* layer = nullptr;
                    for (const Layer& x : a_stack.layers)
                        if (x.tag == tag) layer = &x;
                    if (layer == nullptr || !(layer->z1 > layer->z0)) continue;
                    if (z >= layer->z0 && z <= layer->z1) {
                        const double part = (z - layer->z0) / (layer->z1 - layer->z0);
                        return place.bottom() - part * place.height();
                    }
                }
                return out.isEmpty() ? 0.0 : out.last().second.top();
            };
            const double a = pixelOf(std::min(y0, y1)), b = pixelOf(std::max(y0, y1));
            out.append({l.tag, QRectF(right - 16, std::min(a, b), 10, std::max(6.0, std::abs(b - a)))});
        } else if (l.kind == Layer::Ground) {
            out.append({l.tag, QRectF(left, bottom, std::max(10.0, right - left), 5)});
        } else {
            out.append({l.tag, QRectF(left, std::max(top - 5, 0.0), std::max(10.0, right - left), 5)});
        }
    }
    return out;
}

void StackView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), palette().color(QPalette::Base));
    if (a_stack.layers.isEmpty()) {
        p.setPen(palette().color(QPalette::Disabled, QPalette::Text));
        p.drawText(rect().adjusted(6, 6, -6, -6), Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, tr("No layer"));
        return;
    }
    QFont small = QApplication::font();
    small.setPixelSize(std::max(8, small.pixelSize() > 0 ? small.pixelSize() - 2 : 10));
    p.setFont(small);
    const QList<QPair<QString, QRectF>> drawn = places();
    for (const auto& [tag, place] : drawn) {
        int index = 0;
        const Layer* layer = nullptr;
        for (int i = 0; i < a_stack.layers.size(); ++i)
            if (a_stack.layers.at(i).tag == tag) {
                layer = &a_stack.layers.at(i);
                index = i;
            }
        if (layer == nullptr) continue;
        const QColor colour = RF25Doc::layerColor(index);
        p.setBrush(layer->isConductor() ? colour : QColor(colour.red(), colour.green(), colour.blue(), 90));
        p.setPen(QPen(colour.darker(140), tag == a_focus ? 2.0 : 1.0));
        p.drawRect(place);
        if (place.height() >= 11 && place.width() > 30) {
            p.setPen(palette().color(QPalette::Text));
            p.drawText(place.adjusted(4, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft,
                       p.fontMetrics().elidedText(layer->name, Qt::ElideRight, int(place.width()) - 8));
        }
    }
}

void StackView::mousePressEvent(QMouseEvent* event)
{
    for (const auto& [tag, place] : places())
        if (place.contains(event->position())) {
            emit layerChosen(tag);
            return;
        }
    QWidget::mousePressEvent(event);
}

} // namespace qucs_s::rf25

// ------------------------------------------------------------------ The tab

/// The layout file being read: on a thread of its own, as a layout's tab
/// reads it, so that a large GDSII file never holds the window.
struct RF25Doc::Job {
    QString path;
    std::shared_ptr<Layout> result;
    QString error;
    std::atomic_bool cancelled{false};
    qucs_s::layout::Progress progress{-1};
};

RF25Doc::RF25Doc(QucsApp* app, const QString& name) : QFrame(), QucsDoc(app, name)
{
    setObjectName(QStringLiteral("RF25Doc"));
    a_model = starterModel();
    a_undo = new QUndoStack(this);
    connect(a_undo, &QUndoStack::cleanChanged, this, [this](bool clean) {
        setDocChanged(!clean);
        emit signalFileChanged(!clean);
    });
    const auto front = [this] {
        return a_App != nullptr && a_App->DocumentTab != nullptr && a_App->DocumentTab->currentWidget() == this;
    };
    connect(a_undo, &QUndoStack::canUndoChanged, this, [this, front](bool can) {
        if (front()) a_App->undo->setEnabled(can);
    });
    connect(a_undo, &QUndoStack::canRedoChanged, this, [this, front](bool can) {
        if (front()) a_App->redo->setEnabled(can);
    });
    buildUi();
    showProblems();
    if (app != nullptr) connect(this, SIGNAL(signalFileChanged(bool)), app, SLOT(slotFileChanged(bool)));
}

RF25Doc::~RF25Doc()
{
    // Its undo stack goes with it, emitting its clean and can-undo changes:
    // not to this, half gone (a cast UBSan calls undefined), nor to the
    // window, which may be going too.
    disconnect(a_undo, nullptr, this, nullptr);
    // The read going on is let go: it ends at its next step and drops its
    // result, which nothing holds any more.
    if (a_job) a_job->cancelled = true;
}

Model RF25Doc::starterModel()
{
    return Model::blank();
}

QColor RF25Doc::layerColor(int index)
{
    return qucs_s::layout::paletteStyle(std::max(0, index)).fill;
}

void RF25Doc::buildUi()
{
    auto* all = new QVBoxLayout(this);
    all->setContentsMargins(0, 0, 0, 0);
    all->setSpacing(0);

    auto* bar = new QWidget(this);
    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(6, 4, 6, 4);
    row->setSpacing(6);
    const auto button = [&](const QString& text, const QString& tip, const char* name) {
        auto* b = new QToolButton(bar);
        b->setText(text);
        b->setToolTip(tip);
        b->setObjectName(QLatin1String(name));
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        row->addWidget(b);
        return b;
    };
    a_fit = button(tr("Zoom Extents"), tr("Show the whole layout"), "rfFit");
    connect(a_fit, &QToolButton::clicked, this, [this] { a_view->fit(); });
    a_reread = button(tr("Read the Layout Again"), tr("Read the layout file the model names once more"), "rfReread");
    connect(a_reread, &QToolButton::clicked, this, &RF25Doc::refreshLayout);
    row->addSpacing(10);
    a_layoutLabel = new QLabel(bar);
    a_layoutLabel->setObjectName(QStringLiteral("rfLayoutLabel"));
    a_layoutLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    row->addWidget(a_layoutLabel, 1);
    all->addWidget(bar);

    a_splitter = new QSplitter(Qt::Vertical, this);
    a_splitter->setObjectName(QStringLiteral("rfSplitter"));
    a_sides = new QSplitter(Qt::Horizontal, a_splitter);
    a_sides->setObjectName(QStringLiteral("rfSides"));
    a_stack = new StackView(a_sides);
    a_view = new LayoutView(a_sides);
    a_view->setObjectName(QStringLiteral("rfView"));
    connect(a_stack, &StackView::layerChosen, this, &RF25Doc::focusNode);
    connect(a_view, &LayoutView::pointerMoved, this, &RF25Doc::pointerMoved);
    a_sides->addWidget(a_stack);
    a_sides->addWidget(a_view);
    a_sides->setStretchFactor(0, 0);
    a_sides->setStretchFactor(1, 1);
    a_sides->setSizes({150, 850});
    a_problemsText = new QPlainTextEdit(a_splitter);
    a_problemsText->setObjectName(QStringLiteral("rfProblems"));
    a_problemsText->setReadOnly(true);
    a_problemsText->setMaximumBlockCount(400);
    a_splitter->addWidget(a_sides);
    a_splitter->addWidget(a_problemsText);
    a_splitter->setStretchFactor(0, 5);
    a_splitter->setStretchFactor(1, 1);
    a_splitter->setSizes({560, 150});
    all->addWidget(a_splitter, 1);

    a_status = new QLabel(this);
    a_status->setObjectName(QStringLiteral("rfStatus"));
    a_status->setContentsMargins(8, 2, 8, 2);
    a_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    all->addWidget(a_status);
}

// ------------------------------------------------------------------ QucsDoc

void RF25Doc::setName(const QString& name)
{
    a_DocName = name;
    emit stateChanged();
}

bool RF25Doc::load()
{
    // The file as it is now, taken before it is read: a file written again
    // while this read goes on must not be stamped as the one held (the
    // rule of 24 October, qucsdoc.h).
    const QFileInfo asRead(a_DocName);
    QFile file(a_DocName);
    if (!file.open(QIODevice::ReadOnly)) {
        a_readNotes = file.errorString();
        misc::reportError(tr("Cannot read %1:\n%2").arg(a_DocName, file.errorString()));
        return false;
    }
    QString why;
    const std::optional<Model> m = Model::fromJson(file.readAll(), &why);
    if (!m) {
        a_readNotes = why;
        misc::reportError(tr("Cannot read %1:\n%2").arg(a_DocName, why));
        return false;
    }
    a_readNotes = why;
    a_undo->clear();
    applyModel(*m, true);
    a_undo->setClean();
    setDocChanged(false);
    stampedAs(asRead);
    refreshLayout();
    return true;
}

bool RF25Doc::reload()
{
    const QFileInfo asRead(a_DocName);
    QFile file(a_DocName);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QString why;
    const std::optional<Model> m = Model::fromJson(file.readAll(), &why);
    if (!m) return false;
    a_readNotes = why;
    const QString focus = a_focus;
    a_undo->clear();   // (its steps were of the model as it was)
    applyModel(*m, true);
    setDocChanged(false);
    emit signalFileChanged(false);
    stampedAs(asRead);
    if (!focus.isEmpty() && a_model.find(focus) != nullptr) focusNode(focus);
    refreshLayout();
    return true;
}

bool RF25Doc::writeTo(const QString& path)
{
    const QByteArray bytes = a_model.toJson();
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

int RF25Doc::save()
{
    if (!writeTo(a_DocName)) {
        misc::reportError(tr("Cannot write %1").arg(a_DocName));
        return -1;
    }
    a_undo->setClean();
    setDocChanged(false);
    emit signalFileChanged(false);
    // The date and size the file itself has, not the clock's reading of
    // this moment: the two differ by a little, and changedOnDisk() would
    // then take the file this just wrote for another program's.
    stampedFromFile();
    // A layout named beside the model is another file now that the model
    // has moved (Save As), so it is read again.
    refreshLayout();
    emit stateChanged();
    return 0;
}

void RF25Doc::print(QPrinter*, QPainter* painter, bool, bool)
{
    const QImage image = a_view->picture();
    if (image.isNull()) return;
    const QRect page = painter->viewport();
    const QSize fitted = image.size().scaled(page.size(), Qt::KeepAspectRatio);
    painter->drawImage(QRect(page.topLeft(), fitted), image);
}

void RF25Doc::becomeCurrent(bool)
{
    if (a_App != nullptr) {
        a_App->undo->setEnabled(a_undo->canUndo());
        a_App->redo->setEnabled(a_undo->canRedo());
    }
}

double RF25Doc::zoomBy(double factor)
{
    a_view->zoomBy(factor);
    return a_view->scale();
}

void RF25Doc::showAll()
{
    a_view->fit();
}

void RF25Doc::zoomToSelection()
{
    a_view->fit();
}

void RF25Doc::showNoZoom()
{
    a_view->fit();
}

void RF25Doc::undo()
{
    a_undo->undo();
}

void RF25Doc::redo()
{
    a_undo->redo();
}

// ------------------------------------------------------------------ The model

bool RF25Doc::setModel(const Model& model, const QString& what)
{
    if (model.toObject() == a_model.toObject()) return false;
    const Model before = a_model;
    applyModel(model);
    QPointer<RF25Doc> self = this;
    a_undo->push(new ModelCommand([self](const Model& m) {
        if (self) self->applyModel(m);
    }, before, model, what));
    return true;
}

bool RF25Doc::editNode(const QString& tag, const std::function<void(Node&)>& change, const QString& what, QString* error)
{
    Model m = a_model;
    Node* n = m.find(tag);
    if (n == nullptr) {
        if (error != nullptr) *error = tr("there is no node %1").arg(tag);
        return false;
    }
    change(*n);
    setModel(m, what);
    return true;
}

void RF25Doc::applyModel(const Model& model, bool read)
{
    const QString layoutBefore = layoutFile();
    a_model = model;
    if (read) contentRead();
    else edited();
    a_stack->setStack(a_model.stack());
    showProblems();
    emit modelChanged();
    // The layout file changed under it (another file, another cell): read
    // again. Reading it is the only work this build does, so an edit of
    // anything else costs nothing.
    if (!read && layoutFile() != layoutBefore) refreshLayout();
    else if (!read) showLayout();
}

void RF25Doc::showProblems()
{
    const QStringList said = problems();
    QStringList lines;
    if (said.isEmpty()) {
        lines << tr("The model is whole: a stack, a layout with its map, and ports. There is no solver in this build "
                    "(phase A), so nothing is computed from it yet.");
    } else {
        lines << tr("What is still missing:");
        for (const QString& s : said) lines << QStringLiteral("  • ") + s;
    }
    if (!a_layoutProblem.isEmpty()) lines << a_layoutProblem;
    if (!a_readNotes.isEmpty()) lines << tr("Read with notes: %1").arg(a_readNotes);
    a_problemsText->setPlainText(lines.join(QLatin1Char('\n')));
    const Stack s = a_model.stack();
    // (conductors(): the metals and the ground - what the layer map and a
    // port may name. A via is metal too, but is named by its own metals.)
    a_status->setText(tr("%1 layer(s), %2 of them metals or ground; the stack is %3 high; %4")
                          .arg(s.layers.size())
                          .arg(s.conductors().size())
                          .arg(qucs_s::fem::formatQuantity(s.height(), qucs_s::fem::Dim::length()),
                               said.isEmpty() ? tr("nothing is missing") : tr("%1 thing(s) missing").arg(said.size())));
}

// ------------------------------------------------------------------ The layout

QString RF25Doc::layoutFile() const
{
    const QString named = textOf(a_model.layout(), QStringLiteral("file")).trimmed();
    if (named.isEmpty()) return {};
    const QFileInfo info(named);
    if (info.isAbsolute()) return info.absoluteFilePath();
    // Beside the model (the property says so); beside nothing while it has
    // no file of its own, where the workspace is what a relative name is
    // read against.
    const QDir dir = a_DocName.isEmpty() ? QDir(QucsSettings.QucsWorkDir) : QFileInfo(a_DocName).absoluteDir();
    return QDir::cleanPath(dir.absoluteFilePath(named));
}

void RF25Doc::refreshLayout()
{
    if (a_job) {
        a_job->cancelled = true;
        a_job.reset();
    }
    const QString path = layoutFile();
    a_layoutProblem.clear();
    // Nothing named, or nothing that can be read: what is shown says so,
    // and nothing is read.
    const auto nothing = [this](const QString& why) {
        a_layout.reset();
        a_layoutRead.clear();
        a_layoutProblem = why;
        showLayout();
        showProblems();
        emit stateChanged();
    };
    if (path.isEmpty()) return nothing(QString());
    if (!QFileInfo::exists(path)) return nothing(tr("The layout file %1 is not there.").arg(QDir::toNativeSeparators(path)));
    if (!qucs_s::layout::isLayoutFile(path))
        return nothing(tr("%1 is not a GDSII or OASIS layout.").arg(QFileInfo(path).fileName()));
    auto job = std::make_shared<Job>();
    job->path = path;
    a_job = job;
    QThread* thread = QThread::create([job] {
        job->result = qucs_s::layout::read(job->path, &job->error, &job->cancelled, &job->progress);
    });
    // (A main thread's stack is 8 MB and a thread's 512 kB on macOS, where
    // a layout's hierarchy is walked by recursion: layoutdoc.cpp's note.)
    thread->setStackSize(8u << 20);
    connect(thread, &QThread::finished, this, [this, job] {
        if (job == a_job) readingDone();
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
    showLayout();
    emit stateChanged();
}

void RF25Doc::readingDone()
{
    const std::shared_ptr<Job> job = a_job;
    a_job.reset();
    if (!job->result) {
        a_layout.reset();
        a_layoutRead.clear();
        a_layoutProblem = job->error.isEmpty() ? tr("%1 could not be read.").arg(QFileInfo(job->path).fileName()) : job->error;
    } else {
        a_layout = job->result;
        a_layoutRead = job->path;
        a_layoutProblem.clear();
    }
    showLayout();
    showProblems();
    emit stateChanged();
}

bool RF25Doc::waitForLayout(int ms)
{
    QElapsedTimer t;
    t.start();
    while (a_job && t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return !a_job;
}

QVector<LayerStyle> RF25Doc::stylesFor(const Layout& layout) const
{
    // Which layer of the file goes to which layer of the stack: the map's
    // rows, "layer datatype stack-layer".
    QHash<QString, QString> mapped;   // "layer/datatype" to the stack layer's name
    if (const Node* map = a_model.layout().child(QStringLiteral("rf_layer_map")))
        for (const QJsonValue& row : map->props.value(QLatin1String("rows")).toArray()) {
            const QJsonArray cells = row.toArray();
            if (cells.size() < 3) continue;
            const QString to = cells.at(2).toString().trimmed();
            if (to.isEmpty()) continue;
            const QString key = QStringLiteral("%1/%2").arg(cells.at(0).toString().trimmed(), cells.at(1).toString().trimmed());
            mapped.insert(key, to);
        }
    const Stack stack = a_model.stack();
    QVector<LayerStyle> styles;
    styles.reserve(layout.layers.size());
    for (int i = 0; i < layout.layers.size(); ++i) {
        const qucs_s::layout::LayerKey key = layout.layers.at(i).key;
        const QString to = mapped.value(key.text());
        int at = -1;
        for (int j = 0; j < stack.layers.size(); ++j)
            if (!to.isEmpty() && (stack.layers.at(j).name == to || stack.layers.at(j).tag == to)) at = j;
        LayerStyle style;
        if (at >= 0) {
            const QColor colour = layerColor(at);
            style.frame = colour.darker(140);
            style.fill = colour;
            style.pattern = stack.layers.at(at).isConductor() ? Qt::SolidPattern : Qt::Dense4Pattern;
            style.name = tr("%1 (%2)").arg(key.text(), stack.layers.at(at).name);
        } else {
            // Not sent to the stack: drawn hollow and grey, so that what is
            // left out of the solve is plain to see.
            style.frame = QColor(130, 135, 145);
            style.fill = QColor(130, 135, 145);
            style.pattern = Qt::NoBrush;
            style.name = tr("%1 (not mapped)").arg(key.text());
        }
        styles.append(style);
    }
    return styles;
}

void RF25Doc::showLayout()
{
    const QString named = textOf(a_model.layout(), QStringLiteral("file")).trimmed();
    if (a_job) {
        a_layoutLabel->setText(tr("Reading %1 (%2)…")
                                   .arg(QFileInfo(a_job->path).fileName(),
                                        QLocale().formattedDataSize(QFileInfo(a_job->path).size())));
    } else if (!a_layoutProblem.isEmpty()) {
        a_layoutLabel->setText(a_layoutProblem);
    } else if (named.isEmpty()) {
        a_layoutLabel->setText(tr("The layout names no file yet: its Layout node takes a GDSII or OASIS file."));
    } else if (a_layout) {
        a_layoutLabel->setText(tr("%1: %2 cell(s), %3 layer(s), %4 shape(s)")
                                   .arg(QFileInfo(a_layoutRead).fileName())
                                   .arg(a_layout->cells.size())
                                   .arg(a_layout->layers.size())
                                   .arg(QLocale().toString(qlonglong(a_layout->shapeCount))));
    }
    if (!a_layout) {
        a_view->setLayout(nullptr);
        return;
    }
    const bool again = a_view->layout() == a_layout;
    const QPointF center = a_view->center();
    const double scale = a_view->scale();
    a_view->setLayout(a_layout);
    a_view->setStyles(stylesFor(*a_layout));
    const QString cell = textOf(a_model.layout(), QStringLiteral("cell")).trimmed();
    const int at = cell.isEmpty() ? a_layout->mainCell() : a_layout->cellIndex(cell);
    a_view->setCell(at >= 0 ? at : a_layout->mainCell());
    // The same layout with another map or another colour: the view stays
    // where the user left it; a layout just read is fitted.
    if (again) a_view->setView(center, scale);
    else a_view->fit();
}

void RF25Doc::pointerMoved(QPointF point, bool inside)
{
    if (!inside || !a_layout) {
        showProblems();
        return;
    }
    a_status->setText(tr("x = %1, y = %2")
                          .arg(qucs_s::layout::micrometres(*a_layout, point.x()),
                               qucs_s::layout::micrometres(*a_layout, point.y())));
}

void RF25Doc::focusNode(const QString& tag)
{
    a_focus = tag;
    a_stack->setFocused(tag);
    emit focusChanged(tag);
}
