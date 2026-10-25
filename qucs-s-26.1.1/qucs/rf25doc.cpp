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
#include "rf25/rf25_inductance.h"
#include "rf25/rf25_sparams.h"
#include "qucs.h"
#include "settings.h"

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
#include <QTimer>
#include <QToolButton>
#include <QUndoCommand>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

using namespace qucs_s::rf25;
namespace fem = qucs_s::fem;
using qucs_s::layout::LayerStyle;
using qucs_s::layout::Layout;
using qucs_s::layout::LayoutView;

namespace {

const char* const kShowProcessedKey = "RF25/showProcessed";
const char* const kShowMeshKey = "RF25/showMesh";

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

/// What the solver would be given, made on a thread of its own: the
/// layer map applied and the processing run over the layout as read, and
/// those shapes divided into cells. The model is copied and the layout
/// held, so that an edit while it runs changes neither under it.
struct RF25Doc::PrepareJob {
    Model model;
    std::shared_ptr<const Layout> layout;
    QString called;
    std::shared_ptr<Processed> processed;
    std::shared_ptr<Mesh> mesh;
};

/// A solve on a thread of its own: the shapes and the cells as they
/// stand, and the stack read out of the model, all copied so that an edit
/// while it runs changes nothing under it.
struct RF25Doc::SolveJob {
    Mesh mesh;
    QList<Conductor> conductors;
    Stack stack;
    double length = 0;
    std::shared_ptr<Extraction> result;
    // The loop's inductance, where two ports say what the loop is: the
    // magnetic half of the extraction, solved on the same thread and the
    // same copies as the charge.
    bool loop = false;
    QPointF in, out;
    double frequency = 0;
    QString portsSaid;
    std::shared_ptr<Inductance> inductance;
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
    a_prepareSoon = new QTimer(this);
    a_prepareSoon->setSingleShot(true);
    a_prepareSoon->setInterval(300);
    connect(a_prepareSoon, &QTimer::timeout, this, &RF25Doc::prepare);
    buildUi();
    // The starter model's stack, drawn from the first: only an edit or a
    // read drew it, so a new model's view said "No layer" beside a status
    // line counting three (the phase A check of 24 October).
    a_stack->setStack(a_model.stack());
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
    // The same for the shapes and the mesh being made: the job holds its
    // own copy of the model and of the layout, so it ends on nothing this
    // tab owns.
    a_prepareJob.reset();
}

Model RF25Doc::starterModel()
{
    return Model::blank();
}

QColor RF25Doc::layerColor(int index)
{
    return qucs_s::layout::paletteStyle(std::max(0, index)).fill;
}

QColor RF25Doc::outlineColor(const QColor& fill) const
{
    // Darker on a light canvas, lighter on a dark one: either way a line
    // told from the fill of that very colour under it.
    return a_view != nullptr && a_view->colors().background.lightness() < 128 ? fill.lighter(180) : fill.darker(220);
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
    // What the solver would be given, over the layout it came from.
    const QucsSettingsFile settings;
    a_showProcessed = settings.value(QLatin1String(kShowProcessedKey), true).toBool();
    a_showMesh = settings.value(QLatin1String(kShowMeshKey), false).toBool();
    a_showProcessedButton = button(tr("Processed Shapes"),
                                   tr("Draw the shapes as the processing leaves them over the layout they came from: "
                                      "what the mesher is given"),
                                   "rfShowProcessed");
    a_showProcessedButton->setCheckable(true);
    a_showProcessedButton->setChecked(a_showProcessed);
    connect(a_showProcessedButton, &QToolButton::toggled, this, &RF25Doc::setProcessedShown);
    a_showMeshButton = button(tr("Mesh"), tr("Draw the cells the shapes are divided into"), "rfShowMesh");
    a_showMeshButton->setCheckable(true);
    a_showMeshButton->setChecked(a_showMesh);
    connect(a_showMeshButton, &QToolButton::toggled, this, &RF25Doc::setMeshShown);
    row->addSpacing(10);
    // The one piece of work this build does on being asked rather than at
    // every edit: a dense matrix of the unknowns is cubic.
    a_solveButton = button(tr("Solve"), tr("Find the capacitance of the conductors, and a line's impedance from it"),
                           "rfSolve");
    connect(a_solveButton, &QToolButton::clicked, this, [this] {
        if (isSolving()) stopSolving();
        else solve();
    });
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
    // The canvas went light or dark: the outlines and the cells' ink are
    // told against it, so they are taken again.
    connect(a_view, &LayoutView::colorsChanged, this, &RF25Doc::showOverlays);
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
    a_lastChangeRead = read;
    if (read) contentRead();
    else edited();
    a_stack->setStack(a_model.stack());
    showProblems();
    emit modelChanged();
    // The layout file changed under it (another file, another cell): read
    // again, and what is made of it follows that read rather than this
    // edit.
    if (!read && layoutFile() != layoutBefore) refreshLayout();
    else if (!read) showLayout();
    // What the solver would be given is made of the model and the layout
    // both, so an edit of the stack, the map, the processing or the mesh
    // changes it without the layout being read again. A read just started
    // makes it when it ends.
    if (!a_job) schedulePrepare();
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
    // What the layout gives the solver: the processing's steps and the
    // mesh's, each saying what it did, and what the two of them cannot do.
    const QStringList did = preparedLines();
    if (a_prepareJob) {
        lines << tr("Making the shapes and the mesh…");
    } else if (!did.isEmpty()) {
        lines << tr("What the layout gives the solver:");
        for (const QString& line : did) lines << QStringLiteral("  • ") + line;
    }
    // The solve: what it found, or that it is running, or why it is not.
    if (a_solveJob) {
        lines << tr("Solving…");
    } else if (a_extraction && a_extraction->ok()) {
        lines << tr("What it solves to:");
        for (const QString& line : a_extraction->lines) lines << QStringLiteral("  • ") + line;
        for (const QString& line : lineSummary()) lines << QStringLiteral("  • ") + line;
        for (const QString& line : inductanceSummary()) lines << QStringLiteral("  • ") + line;
    }
    QStringList cannot;
    if (a_processed) cannot += a_processed->problems;
    if (a_mesh) cannot += a_mesh->problems;
    if (!a_solveProblem.isEmpty()) cannot << a_solveProblem;
    if (!cannot.isEmpty()) {
        lines << tr("What stands in the way of a solve:");
        for (const QString& line : cannot) lines << QStringLiteral("  • ") + line;
    }
    a_problemsText->setPlainText(lines.join(QLatin1Char('\n')));
    const Stack s = a_model.stack();
    // (conductors(): the metals and the ground - what the layer map and a
    // port may name. A via is metal too, but is named by its own metals.)
    QString tail = tr("%1 layer(s), %2 of them metals or ground; the stack is %3 high; %4")
                       .arg(s.layers.size())
                       .arg(s.conductors().size())
                       .arg(qucs_s::fem::formatQuantity(s.height(), qucs_s::fem::Dim::length()),
                            said.isEmpty() ? tr("nothing is missing") : tr("%1 thing(s) missing").arg(said.size()));
    // The unknowns, where the mesh has them: what the proposal asks the
    // panel to show before a solve is ever started.
    if (a_prepareJob) tail += tr("; the shapes and the mesh are being made");
    else if (a_mesh && !a_mesh->cells.isEmpty())
        tail += tr("; %1 cell(s), %2").arg(a_mesh->cells.size()).arg(a_mesh->cost.said);
    if (a_solveJob) tail += tr("; solving");
    else if (a_inductance && a_inductance->ok()) tail += tr("; the loop is %1 nH").arg(a_inductance->l * 1e9, 0, 'g', 4);
    else if (a_extraction && a_extraction->ok() && !a_extraction->names.isEmpty())
        tail += tr("; %1").arg(lineSummary().value(0, tr("solved")));
    a_status->setText(tail);
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
    // The shapes and the mesh were made of the layout as it was: a run
    // going on is let go here, and they are made again from what this
    // read leaves.
    a_prepareJob.reset();
    a_prepareAgain = false;
    const QString path = layoutFile();
    a_layoutProblem.clear();
    // Nothing named, or nothing that can be read: what is shown says so,
    // and nothing is read.
    const auto nothing = [this](const QString& why) {
        a_layout.reset();
        a_layoutRead.clear();
        a_layoutProblem = why;
        prepare();   // (nothing read: it clears the shapes and the mesh)
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
    // What the solver would be given, of the layout as it has just been
    // read. It is the work that follows a read, and it waits for none of
    // the 300 ms a burst of edits is given.
    prepare();
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

// ------------------------------- What the solver would be given

void RF25Doc::schedulePrepare()
{
    a_prepareSoon->start();
}

void RF25Doc::prepare()
{
    a_prepareSoon->stop();
    // One run at a time: a model that changed while this one went on is
    // made again when it ends, so that a burst of edits over a slow
    // layout leaves one run waiting, not one an edit.
    if (a_prepareJob) {
        a_prepareAgain = true;
        return;
    }
    a_prepareAgain = false;
    if (!a_layout) {
        // Nothing read: there is nothing to process, and the layout's own
        // problem says why.
        const bool had = a_processed || a_mesh;
        a_processed.reset();
        a_mesh.reset();
        if (had) {
            showOverlays();
            showProblems();
            emit preparedChanged();
        }
        return;
    }
    auto job = std::make_shared<PrepareJob>();
    job->model = a_model;
    job->layout = a_layout;
    job->called = QFileInfo(a_layoutRead).fileName();
    a_prepareJob = job;
    QThread* thread = QThread::create([job] {
        job->processed = std::make_shared<Processed>(processRead(job->model, *job->layout, job->called));
        QStringList said;
        const MeshSettings how = meshSettingsOf(job->model.mesh(), job->model.parameters(), &said);
        Mesh mesh = meshOf(job->processed->conductors, how, job->model.stack(), topFrequency(job->model));
        mesh.problems = said + mesh.problems;
        job->mesh = std::make_shared<Mesh>(std::move(mesh));
    });
    // (The hierarchy of a cell is walked by recursion, as the read does
    // it: a thread's 512 kB stack on macOS is not enough.)
    thread->setStackSize(8u << 20);
    connect(thread, &QThread::finished, this, [this, job] {
        if (job == a_prepareJob) prepareDone();
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
    emit preparedChanged();
}

void RF25Doc::prepareDone()
{
    const std::shared_ptr<PrepareJob> job = a_prepareJob;
    a_prepareJob.reset();
    a_processed = job->processed;
    a_mesh = job->mesh;
    ++a_preparedRuns;
    // The shapes or the cells are not the ones that were solved on, so
    // the capacitance is of something that is no longer there. A solve
    // going on is of them too, and goes with it.
    stopSolving();
    a_extraction.reset();
    a_inductance.reset();
    a_portsSaid.clear();
    a_solvedLength = 0;
    a_solveProblem.clear();
    showOverlays();
    showProblems();
    emit preparedChanged();
    // It changed while that run went on: again, on what it is now.
    if (a_prepareAgain) prepare();
}

bool RF25Doc::waitForPrepared(int ms)
{
    QElapsedTimer t;
    t.start();
    // A run waiting its 300 ms is started at once: a test and a tool ask
    // for what the model says now, not for what it said 300 ms ago.
    if (a_prepareSoon->isActive()) prepare();
    while ((a_prepareJob || a_prepareAgain) && t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return !a_prepareJob;
}

QStringList RF25Doc::preparedLines() const
{
    QStringList lines;
    if (a_processed) lines += a_processed->report.lines;
    if (a_mesh) lines += a_mesh->lines;
    return lines;
}

void RF25Doc::setProcessedShown(bool shown)
{
    if (a_showProcessed == shown) return;
    a_showProcessed = shown;
    if (a_showProcessedButton != nullptr && a_showProcessedButton->isChecked() != shown)
        a_showProcessedButton->setChecked(shown);
    QucsSettingsFile().setValue(QLatin1String(kShowProcessedKey), shown);
    showOverlays();
}

void RF25Doc::setMeshShown(bool shown)
{
    if (a_showMesh == shown) return;
    a_showMesh = shown;
    if (a_showMeshButton != nullptr && a_showMeshButton->isChecked() != shown)
        a_showMeshButton->setChecked(shown);
    QucsSettingsFile().setValue(QLatin1String(kShowMeshKey), shown);
    showOverlays();
}

void RF25Doc::showOverlays()
{
    QList<LayoutView::Overlay> overlays;
    const Stack stack = a_model.stack();
    // (The colour a layer's shapes are drawn in, as the stack and the
    // layout viewer have it: a conductor and its cells are the same
    // colour in every tab.)
    const auto colourOf = [&stack](const QString& layer) {
        for (int i = 0; i < stack.layers.size(); ++i)
            if (stack.layers.at(i).name == layer || stack.layers.at(i).tag == layer) return RF25Doc::layerColor(i);
        return QColor(130, 135, 145);
    };
    if (a_showProcessed && a_processed) {
        // Each layer's own overlay, in its own colour told against the
        // canvas, not a tint of it: the shape under the outline is drawn
        // in that very colour, filled, and a tint of a fill is no line at
        // all. What the processing changed is then plain against the
        // original beneath it.
        QMap<QString, QList<QPolygonF>> byLayer;
        for (const Conductor& c : a_processed->conductors) byLayer[c.layer] << c.outline;
        for (auto it = byLayer.constBegin(); it != byLayer.constEnd(); ++it) {
            LayoutView::Overlay overlay;
            overlay.shapes = it.value();
            overlay.line = outlineColor(colourOf(it.key()));
            overlay.width = 2.0;
            overlay.name = tr("%1, processed").arg(it.key());
            overlays << overlay;
        }
    }
    if (a_showMesh && a_mesh) {
        // The cells are geometry, not metal: the canvas's own ink draws
        // them, which is told from every layer's colour and from the
        // background in either theme. Thin, and let through a little, so
        // that a fine mesh does not black out the shape it divides.
        QColor ink = a_view->colors().ink;
        ink.setAlpha(190);
        QMap<QString, QList<QPolygonF>> byLayer;
        for (const MeshCell& c : a_mesh->cells) byLayer[c.layer] << c.outline;
        for (auto it = byLayer.constBegin(); it != byLayer.constEnd(); ++it) {
            LayoutView::Overlay overlay;
            overlay.shapes = it.value();
            overlay.line = ink;
            overlay.width = 1.0;
            overlay.name = tr("%1, the cells").arg(it.key());
            overlays << overlay;
        }
    }
    a_view->setOverlays(overlays);
}

// ------------------------------------------------------ The solve

void RF25Doc::solve()
{
    stopSolving();
    a_solveProblem.clear();
    const auto nothing = [this](const QString& why) {
        a_extraction.reset();
        a_inductance.reset();
        a_portsSaid.clear();
        a_solvedLength = 0;
        a_solveProblem = why;
        showProblems();
        emit solvedChanged();
    };
    if (isPreparing()) return nothing(tr("The shapes and the mesh are still being made."));
    if (!a_mesh || a_mesh->cells.isEmpty())
        return nothing(tr("There is no mesh to solve on: give the model a layout and a layer map first."));
    if (!a_processed) return nothing(tr("The layout has not been processed."));
    auto job = std::make_shared<SolveJob>();
    job->mesh = *a_mesh;
    job->conductors = a_processed->conductors;
    job->stack = a_model.stack();
    // How long the piece that was solved is: the longer side of what it
    // covers, which is a straight line's own length and what makes a
    // capacitance into one per metre.
    QRectF bounds;
    for (const Conductor& c : std::as_const(job->conductors)) bounds = bounds.united(c.outline.boundingRect());
    job->length = std::max(bounds.width(), bounds.height()) * 1e-6;
    // An inductor's L is of a loop, and what the loop is comes from the
    // ports: the current goes in at one and out at another. Two is what
    // it takes; with one, or none, there is no loop and the quasi-static
    // L of a uniform line is all that can be said.
    QList<QPointF> at;
    for (const Node& port : a_model.ports().children) {
        if (!port.enabled) continue;
        if (port.type != QLatin1String("rf_edge_port") && port.type != QLatin1String("rf_internal_port")) continue;
        bool ok = true;
        const double x = numberOf(port, QStringLiteral("x"), a_model, 0, &ok);
        if (!ok) continue;
        const double y = numberOf(port, QStringLiteral("y"), a_model, 0, &ok);
        if (!ok) continue;
        // (The ports are in metres; the cells are in the layout's own.)
        at << QPointF(x, y) / 1e-6;
    }
    if (at.size() >= 2) {
        job->loop = true;
        job->in = at.at(0);
        job->out = at.at(1);
        job->frequency = topFrequency(a_model);
        if (at.size() > 2)
            job->portsSaid = tr("%1 ports: the loop is taken from the first two.").arg(at.size());
    } else {
        job->portsSaid = at.size() == 1 ? tr("One port: a loop's inductance wants two, so that a current has somewhere "
                                             "to go in and somewhere to come out.")
                                        : tr("No ports: a loop's inductance wants two of them.");
    }
    a_solveJob = job;
    QThread* thread = QThread::create([job] {
        job->result = std::make_shared<Extraction>(extract(job->mesh, job->conductors, job->stack));
        if (job->loop)
            job->inductance = std::make_shared<Inductance>(
                inductanceOf(job->mesh, job->conductors, job->stack, job->in, job->out, 1e-6, job->frequency));
    });
    thread->setStackSize(8u << 20);
    connect(thread, &QThread::finished, this, [this, job] {
        if (job == a_solveJob) solveDone();
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
    if (a_solveButton != nullptr) a_solveButton->setText(tr("Stop"));
    showProblems();
    emit solvedChanged();
}

void RF25Doc::stopSolving()
{
    if (!a_solveJob) return;
    // Let go: it ends on its own copies, and nothing is waiting for it.
    a_solveJob.reset();
    if (a_solveButton != nullptr) a_solveButton->setText(tr("Solve"));
    emit solvedChanged();
}

void RF25Doc::solveDone()
{
    const std::shared_ptr<SolveJob> job = a_solveJob;
    a_solveJob.reset();
    if (a_solveButton != nullptr) a_solveButton->setText(tr("Solve"));
    a_extraction = job->result;
    a_inductance = job->inductance;
    a_portsSaid = job->portsSaid;
    a_solvedLength = job->length;
    a_solveProblem = job->result && !job->result->ok() ? job->result->problems.join(QStringLiteral("; ")) : QString();
    showProblems();
    emit solvedChanged();
}

bool RF25Doc::waitForSolved(int ms)
{
    QElapsedTimer t;
    t.start();
    while (a_solveJob && t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return !a_solveJob;
}

QStringList RF25Doc::lineSummary() const
{
    QStringList out;
    if (!a_extraction || !a_extraction->ok()) return out;
    // At the top of the study's own sweep, which is where a line is
    // worst: the quasi-static values alone where it names none.
    const double f = topFrequency(a_model);
    for (int i = 0; i < a_extraction->names.size(); ++i) {
        const LineParameters lp = lineParameters(*a_extraction, i, a_solvedLength, f);
        if (!lp.ok()) continue;
        out << tr("%1: %2 pF/m, eps_eff %3, Z0 %4 Ohm over %5 µm")
                   .arg(a_extraction->names.at(i))
                   .arg(lp.c * 1e12, 0, 'g', 4)
                   .arg(lp.epsilonEff, 0, 'g', 4)
                   .arg(lp.z0, 0, 'g', 4)
                   .arg(a_solvedLength * 1e6, 0, 'g', 4);
        if (lp.frequency > 0 && (lp.r > 0 || lp.g > 0))
            out << tr("%1 at %2 GHz: R %3 Ohm/m, L %4 nH/m, G %5 S/m, C %6 pF/m; %7 dB/m")
                       .arg(a_extraction->names.at(i))
                       .arg(lp.frequency / 1e9, 0, 'g', 3)
                       .arg(lp.r, 0, 'g', 4)
                       .arg(lp.l * 1e9, 0, 'g', 4)
                       .arg(lp.g, 0, 'g', 4)
                       .arg(lp.c * 1e12, 0, 'g', 4)
                       .arg(lp.alphaDb, 0, 'g', 3);
    }
    return out;
}

QStringList RF25Doc::sParameterSummary() const
{
    QStringList out;
    if (!a_extraction || !a_extraction->ok() || !(a_solvedLength > 0)) return out;
    QString why;
    const QList<double> sweep = studySweep(a_model, &why);
    if (sweep.isEmpty()) {
        if (!why.isEmpty()) out << why;
        return out;
    }
    const double f = sweep.last();
    for (int i = 0; i < a_extraction->names.size(); ++i) {
        const fem::LineRLGC line = asLine(*a_extraction, i, a_solvedLength, f);
        if (!line.error.isEmpty()) continue;
        // Into ports of its own impedance, which is the match a user is
        // looking for; 50 Ohm is in the export's own property.
        const double z = line.Zc.real();
        if (!(z > 0)) continue;
        const std::array<std::complex<double>, 4> sp = fem::lineSParameters(line, a_solvedLength, z);
        const double through = 20 * std::log10(std::max(1e-300, std::abs(sp[1])));
        out << tr("%1 as a two-port into %2 Ohm at %3 GHz: S11 %4 dB, S21 %5 dB at %6°.")
                   .arg(a_extraction->names.at(i))
                   .arg(z, 0, 'g', 4)
                   .arg(f / 1e9, 0, 'g', 3)
                   .arg(20 * std::log10(std::max(1e-300, std::abs(sp[0]))), 0, 'f', 1)
                   .arg(through, 0, 'f', 2)
                   .arg(std::arg(sp[1]) * 180 / M_PI, 0, 'f', 1);
    }
    if (out.isEmpty()) return out;
    // And where the quasi-static answer stops being the whole story,
    // which is the one thing a caller must not have to guess at.
    const LineParameters first = lineParameters(*a_extraction, 0, a_solvedLength);
    const QuasiStaticLimit limit =
        quasiStaticLimit(a_model.stack(), a_mesh ? *a_mesh : Mesh(), 1e-6, first.velocity, a_solvedLength);
    if (limit.frequency > 0) {
        out << tr("Quasi-static to about %1 GHz (%2, eps_r %3): above that a mode other than quasi-TEM appears and "
                  "none of this is the whole story.")
                   .arg(limit.frequency / 1e9, 0, 'g', 3)
                   .arg(limit.what)
                   .arg(limit.epsilonMax, 0, 'g', 3);
        if (f > limit.frequency)
            out << tr("The study's own %1 GHz is above that: the numbers are extrapolated, not solved.")
                       .arg(f / 1e9, 0, 'g', 3);
    }
    if (limit.resonance > 0)
        out << tr("This %1 µm of line is a half wave at %2 GHz, which is where its S parameters first turn over.")
                   .arg(a_solvedLength * 1e6, 0, 'g', 4)
                   .arg(limit.resonance / 1e9, 0, 'g', 3);
    return out;
}

namespace {

/// The Touchstone export of \a model known by \a tag, or null.
const Node* touchstoneNode(const qucs_s::rf25::Model& model, const QString& tag)
{
    const Node* node = model.root.find(tag);
    if (node == nullptr || node->type != QLatin1String("rf_touchstone")) return nullptr;
    return node;
}

} // namespace

QString RF25Doc::exportPath(const QString& tag) const
{
    const Node* node = touchstoneNode(a_model, tag);
    if (node == nullptr) return {};
    const QDir dir = a_DocName.isEmpty() ? QDir(QucsSettings.QucsWorkDir) : QFileInfo(a_DocName).absoluteDir();
    const QString named = textOf(*node, QStringLiteral("file")).trimmed();
    if (!named.isEmpty()) {
        const QFileInfo info(named);
        return info.isAbsolute() ? info.absoluteFilePath() : QDir::cleanPath(dir.absoluteFilePath(named));
    }
    // Named nothing: beside the model, with as many ports as it writes.
    const bool coupled = textOf(*node, QStringLiteral("of")) == QLatin1String("coupled");
    const int nets = a_extraction ? int(a_extraction->names.size()) : 1;
    const int ports = coupled ? 2 * std::max(1, nets) : 2;
    const QString stem = a_DocName.isEmpty() ? QStringLiteral("model") : QFileInfo(a_DocName).completeBaseName();
    return QDir::cleanPath(dir.absoluteFilePath(QStringLiteral("%1_%2.s%3p").arg(stem, tag).arg(ports)));
}

bool RF25Doc::exportRequest(const QString& tag, ExportRequest* what, QString* error) const
{
    if (error != nullptr) error->clear();
    const Node* node = touchstoneNode(a_model, tag);
    if (node == nullptr) {
        if (error != nullptr) *error = tr("There is no Touchstone export called %1 in this model.").arg(tag);
        return false;
    }
    ExportRequest out;
    out.coupled = textOf(*node, QStringLiteral("of")) == QLatin1String("coupled");
    out.net = textOf(*node, QStringLiteral("net")).trimmed();
    bool ok = true;
    out.impedance = numberOf(*node, QStringLiteral("impedance"), a_model, 50.0, &ok);
    if (!ok) {
        if (error != nullptr) *error = tr("The export's reference impedance is not a number.");
        return false;
    }
    out.length = numberOf(*node, QStringLiteral("length"), a_model, 0.0, &ok);
    if (!ok) {
        if (error != nullptr) *error = tr("The export's length is not a number.");
        return false;
    }
    if (what != nullptr) *what = out;
    return true;
}

QString RF25Doc::exportText(const QString& tag, QString* error) const
{
    ExportRequest what;
    if (!exportRequest(tag, &what, error)) return {};
    return exportText(what, error);
}

QString RF25Doc::exportText(const ExportRequest& what, QString* error) const
{
    const auto refuse = [error](const QString& why) {
        if (error != nullptr) *error = why;
        return QString();
    };
    if (error != nullptr) error->clear();
    if (!a_extraction || !a_extraction->ok())
        return refuse(a_solveProblem.isEmpty() ? tr("Nothing is solved yet: Solve the model first.") : a_solveProblem);
    QString why;
    const QList<double> sweep = studySweep(a_model, &why);
    if (sweep.isEmpty()) return refuse(why.isEmpty() ? tr("The study asks for no frequencies to write.") : why);
    if (!(what.impedance > 0))
        return refuse(tr("A reference impedance of %1 Ohm is none.").arg(what.impedance, 0, 'g', 4));
    const double length = what.length > 0 ? what.length : a_solvedLength;
    if (!(length > 0))
        return refuse(tr("A line's S parameters are of a length of it: the solve found none, so give the export one."));

    QStringList comments;
    comments << tr("%1, an RF 2.5D model of Qucs-S").arg(QFileInfo(a_DocName).fileName());
    comments << tr("Quasi-static: C and C0 from a charge solve, L from C0, R from the metal's surface impedance, G "
                   "from the dielectrics' loss");
    for (const QString& line : sParameterSummary()) comments << line;

    if (what.coupled) {
        const fem::CoupledSweep cs = coupledSweep(*a_extraction, length, sweep);
        if (!cs.error.isEmpty()) return refuse(cs.error);
        comments << tr("Ports 1 to %1 are the near ends of %2, in that order; %3 to %4 their far ends")
                        .arg(a_extraction->names.size())
                        .arg(a_extraction->names.join(QStringLiteral(", ")))
                        .arg(a_extraction->names.size() + 1)
                        .arg(2 * a_extraction->names.size());
        return fem::touchstoneCoupled(cs, length, what.impedance, false, comments);
    }
    // One net, as a two-port: the one it names, or the first.
    int index = 0;
    if (!what.net.isEmpty()) {
        index = int(a_extraction->names.indexOf(what.net));
        if (index < 0)
            return refuse(tr("There is no net called %1: the solve found %2.")
                              .arg(what.net, a_extraction->names.join(QStringLiteral(", "))));
    }
    const fem::LineSweep ls = lineSweep(*a_extraction, index, length, sweep);
    if (!ls.error.isEmpty()) return refuse(ls.error);
    comments << tr("Net %1, port 1 at its start").arg(a_extraction->names.value(index));
    return fem::touchstone(ls, length, what.impedance, comments);
}

QString RF25Doc::writeExport(const QString& tag, QString* error) const
{
    QString why;
    const QString text = exportText(tag, &why);
    if (!why.isEmpty()) {
        if (error != nullptr) *error = why;
        return {};
    }
    const QString path = exportPath(tag);
    if (path.isEmpty()) {
        if (error != nullptr) *error = tr("There is nowhere to write it: give the export a file.");
        return {};
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error != nullptr) *error = tr("%1 could not be written: %2.").arg(path, file.errorString());
        return {};
    }
    file.write(text.toUtf8());
    file.close();
    if (error != nullptr) error->clear();
    return path;
}

QStringList RF25Doc::inductanceSummary() const
{
    QStringList out;
    if (!a_extraction || !a_extraction->ok()) return out;
    if (!a_inductance) {
        if (!a_portsSaid.isEmpty()) out << a_portsSaid;
        return out;
    }
    if (!a_inductance->ok()) {
        out << (a_inductance->problems.isEmpty() ? tr("The loop's inductance came to nothing.")
                                                 : a_inductance->problems.join(QStringLiteral(" ")));
        return out;
    }
    if (!a_portsSaid.isEmpty()) out << a_portsSaid;
    out << tr("The loop is %1 nH over %2 run(s) of current, and %3 Ohm at direct current.")
               .arg(a_inductance->l * 1e9, 0, 'g', 4)
               .arg(a_inductance->branches)
               .arg(a_inductance->rDc, 0, 'g', 4);
    if (a_inductance->q > 0)
        out << tr("Q is %1 at %2 GHz, taking the current as the one at direct current.")
                   .arg(a_inductance->q, 0, 'g', 3)
                   .arg(a_inductance->frequency / 1e9, 0, 'g', 3);
    return out;
}

QStringList RF25Doc::solvedLines() const
{
    return a_extraction ? a_extraction->lines : QStringList();
}

double RF25Doc::solvedLength() const
{
    return a_solvedLength;
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
