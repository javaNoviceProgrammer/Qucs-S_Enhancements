/*
 * multiphysics3ddoc.cpp - a 3D multiphysics model in a tab: its file, its
 *                         Undo, its geometry, mesh and solution made in the
 *                         background, the 3D view of them
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "multiphysics3ddoc.h"

#include "dataimport.h"
#include "fem3_export.h"
#include "fem3_results.h"
#include "ink.h"
#include "misc.h"
#include "multiphysicsview.h"
#include "qucs.h"
#include "settings.h"

#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPrinter>
#include <QProgressBar>
#include <QSaveFile>
#include <QSlider>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QUndoCommand>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <new>

using namespace qucs_s::fem;

namespace {

// How long a closed tab waits for its job to stop when told to.
constexpr int kJobStopMs = 2000;

class ModelCommand3 : public QUndoCommand
{
public:
    ModelCommand3(std::function<void(const Model&)> apply, Model before, Model after, const QString& what)
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

QByteArray compact(const QJsonValue& v)
{
    if (v.isObject()) return QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact);
    if (v.isArray()) return QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact);
    return v.toVariant().toString().toUtf8();
}

/// What the geometry is made of (the parameters, the unit, the features),
/// and what the mesh is made of besides (its node, the selections).
struct Parts {
    QByteArray geometry, mesh;
    QByteArray solved;   // all a solve reads: the model but its results, title and description
};

void clearVisibility(Node& n)
{
    n.visibility.clear();
    for (Node& c : n.children) clearVisibility(c);
}

Parts partsOf(const Model& model)
{
    // (What the view draws of each part is the view's alone: hidden, a part
    // is built, meshed and solved as before.)
    Model m = model;
    clearVisibility(m.root);
    const QJsonObject o = m.toObject();
    const QJsonObject c = o.value(QStringLiteral("components")).toArray().at(0).toObject();
    Parts p;
    p.geometry = compact(o.value(QStringLiteral("parameters"))) + compact(o.value(QStringLiteral("functions")))
                 + compact(c.value(QStringLiteral("unit"))) + compact(c.value(QStringLiteral("geometry")));
    p.mesh = compact(c.value(QStringLiteral("mesh"))) + compact(c.value(QStringLiteral("selections")));
    QJsonObject solved = o;
    for (const char* key : {"results", "title", "description"}) solved.remove(QLatin1String(key));
    p.solved = QJsonDocument(solved).toJson(QJsonDocument::Compact);
    return p;
}

const QColor Highlight(31, 111, 235);

} // namespace

struct Multiphysics3DDoc::Job {
    bool mesh = false;                       ///< a mesh asked (its geometry first, when none given)
    bool solve = false;                      ///< a solve asked (its mesh first, when none given)
    std::shared_ptr<const Mesh3> givenMesh;  ///< the mesh a solve is on (none: made by it)
    StudyRun3 study;
    QString studyTag;                        ///< the study solved (empty: the first)
    SolveAsked3 asked;                       ///< its solver, the memory limit, whether forced past it
    std::vector<Estimate3> estimates;        ///< a mesh's, made without a solve
    Model model;
    std::shared_ptr<ParameterScope> parameters;
    std::shared_ptr<Topology3> topology;     ///< the geometry a mesh is of (given: not built again)
    GeometryBuild3 geometry;
    bool built = false;                      ///< the geometry built by it
    Mesh3Build meshBuild;
    quint64 revision = 0;
    std::atomic<bool> cancel{false};
    std::atomic<int> permille{0};
    QThread* thread = nullptr;
    QElapsedTimer started;
};

double Multiphysics3DDoc::memoryLimit()
{
    const QVariant v = QucsSettingsFile().value(QStringLiteral("Multiphysics3D/memoryLimitGB"));
    bool ok = false;
    const double gb = v.toDouble(&ok);
    if (v.isValid() && ok && gb >= 0) return gb * 1024.0 * 1048576.0;
    return physicalMemory() / 2;
}

void Multiphysics3DDoc::setMemoryLimitGB(double gb)
{
    QucsSettingsFile s;
    if (gb < 0) s.remove(QStringLiteral("Multiphysics3D/memoryLimitGB"));
    else s.setValue(QStringLiteral("Multiphysics3D/memoryLimitGB"), gb);
}

QColor Multiphysics3DDoc::domainColor(int domain)
{
    static const QColor palette[] = {QColor(160, 196, 232), QColor(240, 196, 140), QColor(170, 214, 160), QColor(226, 160, 170),
                                     QColor(196, 176, 226), QColor(232, 220, 150), QColor(150, 210, 210), QColor(222, 180, 150),
                                     QColor(186, 200, 160), QColor(214, 170, 214), QColor(170, 186, 214), QColor(200, 200, 200)};
    if (domain < 0) return QColor(190, 190, 190);
    return palette[domain % int(std::size(palette))];
}

Multiphysics3DDoc::Multiphysics3DDoc(QucsApp* app, const QString& name) : QFrame(), QucsDoc(app, name)
{
    setObjectName(QStringLiteral("Multiphysics3DDoc"));
    a_model = Model::blank3d();
    a_model.fileName = name;
    a_undo = new QUndoStack(this);
    connect(a_undo, &QUndoStack::cleanChanged, this, [this](bool clean) {
        setDocChanged(!clean);
        emit signalFileChanged(!clean);
    });
    const auto front = [this] { return a_App != nullptr && a_App->DocumentTab != nullptr && a_App->DocumentTab->currentWidget() == this; };
    connect(a_undo, &QUndoStack::canUndoChanged, this, [this, front](bool can) {
        if (front()) a_App->undo->setEnabled(can);
    });
    connect(a_undo, &QUndoStack::canRedoChanged, this, [this, front](bool can) {
        if (front()) a_App->redo->setEnabled(can);
    });
    a_geometryTimer = new QTimer(this);
    a_geometryTimer->setSingleShot(true);
    a_geometryTimer->setInterval(150);
    connect(a_geometryTimer, &QTimer::timeout, this, [this] {
        if (a_geometryStale) requestGeometry();
    });
    a_progressTimer = new QTimer(this);
    a_progressTimer->setInterval(100);
    connect(a_progressTimer, &QTimer::timeout, this, [this] {
        if (a_job) a_progress->setValue(a_job->permille.load());
    });
    // A drawing or a layout the geometry reads, changed: built again.
    a_watcher = new QFileSystemWatcher(this);
    connect(a_watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString& path) {
        if (QFileInfo::exists(path) && !a_watcher->files().contains(path)) a_watcher->addPath(path);   // (written anew)
        say(tr("%1 changed: the geometry is built again").arg(QFileInfo(path).fileName()));
        ++a_revision;
        ++a_solvedRevision;
        a_geometryStale = true;
        a_meshStale = true;
        a_geometryTimer->start();
        updateState();
    });
    buildUi();
    if (app != nullptr) connect(this, SIGNAL(signalFileChanged(bool)), app, SLOT(slotFileChanged(bool)));
    updateScene(true);
}

Multiphysics3DDoc::~Multiphysics3DDoc()
{
    a_closed = true;
    // (Its undo stack goes with it, emitting its changes: not to this, half
    // gone, nor to the window.)
    disconnect(a_undo, nullptr, this, nullptr);
    // (Nor do its widgets reach it as they go, after this: QWidget's own
    // destructor sends the focus elsewhere as the tab's page goes, and the
    // plot's expression field, losing it, finished its editing - whose slot
    // set the plot on this, half gone: a crash on Cocoa, SIGSEGV in
    // Expression::compile, found 12 November.)
    for (QObject* child : findChildren<QObject*>()) disconnect(child, nullptr, this, nullptr);
    if (a_job && a_job->thread) {
        a_job->cancel = true;
        // (A build that does not ask whether it is to stop - a geometry
        // feature's loop - is not waited for past a moment: closing the tab
        // waited for ever - the hunt of 9 November, H2. Left to end on its
        // own: the thread holds the job, all it reads, and deletes itself.)
        if (a_job->thread->wait(QDeadlineTimer(kJobStopMs))) {
            delete a_job->thread;
        } else {
            QThread* left = a_job->thread;
            QObject::connect(left, &QThread::finished, left, &QObject::deleteLater);
            if (left->isFinished()) left->deleteLater();
        }
        a_job->thread = nullptr;
    }
    // (Calls waiting for its work told it ended without them - after it has
    // gone: their pointers to it null by then.)
    for (const auto& w : a_waiting) QTimer::singleShot(0, qApp, [then = w->then] { then(false); });
    a_waiting.clear();
}

void Multiphysics3DDoc::buildUi()
{
    auto* all = new QVBoxLayout(this);
    all->setContentsMargins(0, 0, 0, 0);
    all->setSpacing(0);
    auto* bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("multiphysics3dToolbar"));
    // Two rows: what is built, solved and shown; under it where the view
    // looks from, its cut and what it draws of each part, and the work going
    // on.
    QHBoxLayout* row = new QHBoxLayout(bar);
    row->setContentsMargins(6, 3, 6, 1);
    row->setSpacing(4);
    auto button = [&](const QString& text, const QString& tip, const char* name) {
        auto* b = new QToolButton(row->parentWidget());
        b->setText(text);
        b->setToolTip(tip);
        b->setObjectName(QString::fromLatin1(name));
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        b->setAutoRaise(true);
        row->addWidget(b);
        return b;
    };
    a_buildGeometry = button(tr("Build Geometry"), tr("Build the solids and make them one (Form Union)"), "mp3BuildGeometry");
    a_buildMesh = button(tr("Build Mesh"), tr("Mesh the geometry: its plan's triangles swept through each slab as prisms"), "mp3BuildMesh");
    connect(a_buildGeometry, &QToolButton::clicked, this, [this] {
        showGeometry();
        requestGeometry();
    });
    connect(a_buildMesh, &QToolButton::clicked, this, &Multiphysics3DDoc::buildMesh);
    a_compute = button(tr("Compute"), tr("Solve the model's study (its geometry and mesh first, where they changed)"), "mp3Compute");
    connect(a_compute, &QToolButton::clicked, this, [this] { compute(); });
    row->addSpacing(8);
    row->addWidget(new QLabel(tr("Show:"), bar));
    a_showBox = new QComboBox(bar);
    a_showBox->setObjectName(QStringLiteral("mp3Show"));
    a_showBox->addItems({tr("Geometry"), tr("Mesh"), tr("Solution")});
    row->addWidget(a_showBox);
    connect(a_showBox, &QComboBox::activated, this, [this](int index) {
        // (After the three: the model's 3D plot groups, by their tags.)
        const QString group = a_showBox->itemData(index).toString();
        if (!group.isEmpty()) showPlotGroup(group);
        else if (index == 2) showSolution();
        else if (index == 1) showMesh();
        else showGeometry();
    });
    row->addWidget(new QLabel(tr("Plot:"), bar));
    a_plotBox = new QComboBox(bar);
    a_plotBox->setObjectName(QStringLiteral("mp3Plot"));
    a_plotBox->setEditable(true);
    a_plotBox->setMinimumContentsLength(10);
    a_plotBox->setToolTip(tr("What the solution is coloured by: a variable (ht.T, es.normE, ec.Qrh, solid.mises...) or an expression of them"));
    a_plotBox->setEnabled(false);
    row->addWidget(a_plotBox);
    connect(a_plotBox, &QComboBox::activated, this, [this](int) { setPlotExpression(a_plotBox->currentText()); });
    connect(a_plotBox->lineEdit(), &QLineEdit::editingFinished, this, [this] { setPlotExpression(a_plotBox->currentText()); });
    // (A solution in time: which of its output times is shown.)
    a_timeLabel = new QLabel(tr("Time:"), bar);
    row->addWidget(a_timeLabel);
    a_timeBox = new QComboBox(bar);
    a_timeBox->setObjectName(QStringLiteral("mp3Time"));
    a_timeBox->setToolTip(tr("The output time shown: the fields, the plot and the globals at that time"));
    row->addWidget(a_timeBox);
    connect(a_timeBox, &QComboBox::activated, this, [this](int index) { setShownTime(index); });
    a_timeLabel->hide();
    a_timeBox->hide();
    // (A sweep: which of its points is shown.)
    a_pointLabel = new QLabel(tr("Point:"), bar);
    row->addWidget(a_pointLabel);
    a_pointBox = new QComboBox(bar);
    a_pointBox->setObjectName(QStringLiteral("mp3Point"));
    a_pointBox->setToolTip(tr("The point of the sweep shown: its parameters' values"));
    row->addWidget(a_pointBox);
    connect(a_pointBox, &QComboBox::activated, this, [this](int index) { setShownPoint(index); });
    a_pointLabel->hide();
    a_pointBox->hide();
    row->addStretch(1);
    all->addWidget(bar);
    auto* viewBar = new QWidget(this);
    viewBar->setObjectName(QStringLiteral("multiphysics3dViewBar"));
    row = new QHBoxLayout(viewBar);
    row->setContentsMargins(6, 1, 6, 3);
    row->setSpacing(4);
    struct Preset {
        const char* name;
        QString text, tip;
        View3D::Preset preset;
    };
    const Preset presets[] = {{"mp3Top", tr("Top"), tr("Look down from above (along -z)"), View3D::Preset::Top},
                              {"mp3Front", tr("Front"), tr("Look from the front (along +y)"), View3D::Preset::Front},
                              {"mp3Side", tr("Side"), tr("Look from the right (along -x)"), View3D::Preset::Side},
                              {"mp3Iso", tr("Iso"), tr("Look from the front right, above"), View3D::Preset::Iso}};
    for (const Preset& p : presets) {
        QToolButton* b = button(p.text, p.tip, p.name);
        const View3D::Preset which = p.preset;
        connect(b, &QToolButton::clicked, this, [this, which] { a_view->viewFrom(which); });
    }
    QToolButton* fit = button(tr("Fit"), tr("Frame the whole model (View All)"), "mp3Fit");
    connect(fit, &QToolButton::clicked, this, [this] { a_view->fit(); });
    row->addSpacing(8);
    row->addWidget(new QLabel(tr("Cut:"), viewBar));
    a_clipBox = new QComboBox(viewBar);
    a_clipBox->setObjectName(QStringLiteral("mp3Clip"));
    a_clipBox->addItem(tr("None"), -1);
    a_clipBox->addItem(QStringLiteral("x"), 0);
    a_clipBox->addItem(QStringLiteral("y"), 1);
    a_clipBox->addItem(QStringLiteral("z"), 2);
    a_clipBox->setToolTip(tr("Cut away what lies beyond a plane across x, y or z, to see inside"));
    row->addWidget(a_clipBox);
    a_clipSlider = new QSlider(Qt::Horizontal, viewBar);
    a_clipSlider->setObjectName(QStringLiteral("mp3ClipAt"));
    a_clipSlider->setRange(0, 100);
    a_clipSlider->setValue(50);
    a_clipSlider->setMinimumWidth(60);
    a_clipSlider->setMaximumWidth(120);
    a_clipSlider->setEnabled(false);
    a_clipSlider->setToolTip(tr("Where the cut is, across the model's extent"));
    row->addWidget(a_clipSlider);
    auto clip = [this] {
        const int axis = a_clipBox->currentData().toInt();
        a_clipSlider->setEnabled(axis >= 0);
        a_view->setClip(axis, a_clipSlider->value() / 100.0);
        // (A solution's slice at the cut.)
        if (a_show == Show::Solution) updateScene();
    };
    connect(a_clipBox, &QComboBox::activated, this, clip);
    connect(a_clipSlider, &QSlider::valueChanged, this, clip);
    // What it draws of the part chosen in the tree: as the tree's menu has it.
    row->addSpacing(8);
    a_visibilityButton = button(tr("Visibility"), tr("How the view draws the part chosen in the tree: shown, hidden or its frame alone"),
                                "mp3Visibility");
    a_visibilityButton->setPopupMode(QToolButton::InstantPopup);
    // (Its arrow in its text, in the theme's colour and with room of its own:
    // a style draws its indicator over a text button's last letters.)
    a_visibilityButton->setText(tr("Visibility") + QStringLiteral(" \u25BE"));
    a_visibilityButton->setAccessibleName(tr("Visibility"));
    a_visibilityButton->setStyleSheet(QStringLiteral("QToolButton#mp3Visibility::menu-indicator { image: none; width: 0px; }"));
    {
        auto* menu = new QMenu(a_visibilityButton);
        menu->setObjectName(QStringLiteral("mp3VisibilityMenu"));
        // (None checked where the node chosen is not a part.)
        auto* group = new QActionGroup(menu);
        group->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);
        auto add = [&](const QString& text, const char* name, Visibility v) {
            QAction* a = menu->addAction(text, this, [this, v] {
                setVisibility(a_focus, v);
                updateVisibilityButton();   // (drawn so already: its check kept)
            });
            a->setObjectName(QString::fromLatin1(name));
            a->setCheckable(true);
            group->addAction(a);
            return a;
        };
        a_showAction = add(tr("Show"), "mp3VisibilityShow", Visibility::Shown);
        a_hideAction = add(tr("Hide"), "mp3VisibilityHide", Visibility::Hidden);
        a_frameAction = add(tr("Frame Only"), "mp3VisibilityFrame", Visibility::Frame);
        menu->addSeparator();
        a_showAllAction = menu->addAction(tr("Show All"), this, [this] { showAllParts(); });
        a_showAllAction->setObjectName(QStringLiteral("mp3VisibilityShowAll"));
        a_visibilityButton->setMenu(menu);
    }
    row->addStretch(1);
    a_stage = new QLabel(viewBar);
    a_stage->setObjectName(QStringLiteral("mp3Stage"));
    row->addWidget(a_stage);
    a_progress = new QProgressBar(viewBar);
    a_progress->setObjectName(QStringLiteral("mp3Progress"));
    a_progress->setRange(0, 1000);
    a_progress->setTextVisible(false);
    a_progress->setMinimumWidth(60);
    a_progress->setMaximumWidth(140);
    a_progress->setMaximumHeight(14);
    row->addWidget(a_progress);
    a_cancel = button(tr("Cancel"), tr("Stop the work going on"), "mp3Cancel");
    connect(a_cancel, &QToolButton::clicked, this, &Multiphysics3DDoc::cancel);
    a_stage->hide();
    a_progress->hide();
    a_cancel->hide();
    all->addWidget(viewBar);
    updateVisibilityButton();

    auto* split = new QSplitter(Qt::Vertical, this);
    split->setObjectName(QStringLiteral("mp3Splitter"));
    a_view = new View3D(split);
    // Below it the messages, and the derived values' table beside them once
    // one is evaluated.
    auto* below = new QSplitter(Qt::Horizontal, split);
    below->setObjectName(QStringLiteral("mp3Below"));
    a_notes = new QPlainTextEdit(below);
    a_notes->setObjectName(QStringLiteral("mp3Messages"));
    a_notes->setReadOnly(true);
    a_notes->setMaximumBlockCount(500);
    a_table = new QTableWidget(0, 3, below);
    a_table->setObjectName(QStringLiteral("mp3Table"));
    a_table->setHorizontalHeaderLabels({tr("Node"), tr("Value of"), tr("Value")});
    a_table->verticalHeader()->hide();
    a_table->horizontalHeader()->setStretchLastSection(true);
    a_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    a_table->hide();
    below->addWidget(a_notes);
    below->addWidget(a_table);
    split->addWidget(a_view);
    split->addWidget(below);
    split->setStretchFactor(0, 5);
    split->setStretchFactor(1, 1);
    split->setSizes({600, 110});
    all->addWidget(split, 1);
}

void Multiphysics3DDoc::changeEvent(QEvent* event)
{
    QFrame::changeEvent(event);
    if (event->type() == QEvent::PaletteChange) updateScene();
}

// ------------------------------------------------------------------ QucsDoc

void Multiphysics3DDoc::setName(const QString& name)
{
    a_DocName = name;
    a_model.fileName = name;
    emit stateChanged();
}

bool Multiphysics3DDoc::load()
{
    const QFileInfo asRead(a_DocName);
    QFile file(a_DocName);
    if (!file.open(QIODevice::ReadOnly)) {
        misc::reportError(tr("Cannot read %1:\n%2").arg(a_DocName, file.errorString()));
        return false;
    }
    QString why;
    std::optional<Model> m = Model::fromJson(file.readAll(), &why);
    if (!m) {
        misc::reportError(tr("Cannot read %1:\n%2").arg(a_DocName, why));
        return false;
    }
    if (!m->is3D()) {
        misc::reportError(tr("Cannot read %1:\nit is a 2D model - Multiphysics opens it, not Multiphysics 3D").arg(a_DocName));
        return false;
    }
    if (!why.isEmpty()) say(tr("Read with notes: %1").arg(why));
    a_readNotes = why;
    m->fileName = a_DocName;
    a_undo->clear();
    applyModel(*m, true);
    a_undo->setClean();
    setDocChanged(false);
    stampedAs(asRead);
    say(tr("Opened %1").arg(QFileInfo(a_DocName).fileName()));
    a_firstShown = false;
    requestGeometry();
    return true;
}

bool Multiphysics3DDoc::reload()
{
    const QFileInfo asRead(a_DocName);
    QFile file(a_DocName);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QString why;
    std::optional<Model> m = Model::fromJson(file.readAll(), &why);
    if (!m || !m->is3D()) return false;
    a_readNotes = why;
    m->fileName = a_DocName;
    a_undo->clear();   // (its steps were of the model as it was)
    applyModel(*m, true);
    setDocChanged(false);
    emit signalFileChanged(false);
    stampedAs(asRead);
    say(tr("Read again: %1 changed on disk").arg(QFileInfo(a_DocName).fileName()));
    requestGeometry();
    return true;
}

bool Multiphysics3DDoc::writeTo(const QString& path)
{
    const QByteArray bytes = a_model.toJson();
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

int Multiphysics3DDoc::save()
{
    if (!writeTo(a_DocName)) {
        misc::reportError(tr("Cannot write %1").arg(a_DocName));
        return -1;
    }
    a_model.fileName = a_DocName;
    a_undo->setClean();
    setDocChanged(false);
    emit signalFileChanged(false);
    stampedFromFile();
    return 0;
}

void Multiphysics3DDoc::print(QPrinter*, QPainter* painter, bool, bool)
{
    const QImage image = a_view->picture(2);
    const QRect page = painter->viewport();
    const QSize fitted = image.size().scaled(page.size(), Qt::KeepAspectRatio);
    painter->drawImage(QRect(page.topLeft(), fitted), image);
}

void Multiphysics3DDoc::becomeCurrent(bool)
{
    if (a_App != nullptr) {
        a_App->undo->setEnabled(a_undo->canUndo());
        a_App->redo->setEnabled(a_undo->canRedo());
    }
}

double Multiphysics3DDoc::zoomBy(double factor)
{
    a_view->zoomBy(factor);
    return a_view->scale();
}

void Multiphysics3DDoc::showAll()
{
    a_view->fit();
}

void Multiphysics3DDoc::zoomToSelection()
{
    a_view->fit();
}

void Multiphysics3DDoc::showNoZoom()
{
    a_view->fit();
}

void Multiphysics3DDoc::undo()
{
    a_undo->undo();
}

void Multiphysics3DDoc::redo()
{
    a_undo->redo();
}

// ------------------------------------------------------------------ The model

bool Multiphysics3DDoc::setModel(const Model& model, const QString& what)
{
    if (model.toObject() == a_model.toObject()) return false;
    const Model before = a_model;
    applyModel(model);
    QPointer<Multiphysics3DDoc> self = this;
    a_undo->push(new ModelCommand3([self](const Model& m) {
        if (self) self->applyModel(m);
    }, before, model, what));
    return true;
}

bool Multiphysics3DDoc::editNode(const QString& tag, const std::function<void(Node&)>& change, const QString& what, QString* error)
{
    Model m = a_model;
    Node* n = m.find(tag);
    if (!n) {
        if (error) *error = tr("there is no node %1").arg(tag);
        return false;
    }
    change(*n);
    setModel(m, what);
    return true;
}

std::shared_ptr<ParameterScope> Multiphysics3DDoc::parameters() const
{
    if (!a_parameters || a_parametersRevision != a_revision) {
        a_parameters = evaluateParameters(a_model);
        a_parametersRevision = a_revision;
    }
    return a_parameters;
}

void Multiphysics3DDoc::applyModel(const Model& model, bool read)
{
    const Parts before = partsOf(a_model), after = partsOf(model);
    a_model = model;
    a_model.fileName = a_DocName;
    ++a_revision;
    const bool geometry = read || before.geometry != after.geometry;
    // (Its results alone changed - a derived value, a plot added -: the
    // solution is of the model as it is still.)
    if (read || before.solved != after.solved) {
        ++a_solvedRevision;
        if (a_solution) a_solutionStale = true;
    }
    if (geometry) {
        a_geometryStale = true;
        a_meshStale = true;
        a_geometryTimer->start();
    } else if (before.mesh != after.mesh) {
        a_meshStale = true;
        if (a_show == Show::Mesh) showGeometry();
    }
    // (Read from its file: what was solved is of another model.)
    if (read) {
        a_solution.reset();
        a_solutions.clear();
        a_plotCache = {};
        fillPointChoices();
        fillTimeChoices();
        if (a_show == Show::Solution) a_show = Show::Geometry;
    }
    if (!a_focus.isEmpty() && !a_model.find(a_focus)) a_focus.clear();
    fillShowChoices();
    // (Its plot group gone, or no longer one: its solution shown.)
    if (a_show == Show::Plot) {
        const Node* g = a_model.find(a_plotGroup);
        if (!g || g->type != QLatin1String("plotgroup3d")) a_show = a_solution ? Show::Solution : Show::Geometry;
    }
    updateHighlight();
    updateScene();
    updateVisibilityButton();
    emit modelChanged();
    updateState();
}

void Multiphysics3DDoc::say(const QString& text, bool error)
{
    a_messages << text;
    if (a_notes) {
        if (error) a_notes->appendHtml(QStringLiteral("<span style='color:#c62828'>%1</span>").arg(text.toHtmlEscaped()));
        else a_notes->appendPlainText(text);
    }
}

// ------------------------------------------------------------------ Geometry and mesh

bool Multiphysics3DDoc::geometryBeingBuilt() const
{
    return a_job && (a_job->built || !a_job->topology);
}

bool Multiphysics3DDoc::meshBeingMade() const
{
    return a_job && (a_job->mesh || (a_job->solve && !a_job->givenMesh));
}

bool Multiphysics3DDoc::solving() const
{
    return a_job && a_job->solve;
}

bool Multiphysics3DDoc::meshing() const
{
    return a_job && a_job->mesh && !a_job->solve;
}

void Multiphysics3DDoc::compute(const QString& study, const QString& solver, bool force)
{
    if (a_closed) return;
    a_study = study;
    a_asked.solver = solver;
    a_asked.force = force;
    if (a_job) {
        a_solveWanted = true;
        return;
    }
    startJob(false, true);
}

void Multiphysics3DDoc::requestGeometry()
{
    a_geometryTimer->stop();
    if (a_closed) return;
    if (a_job) {
        a_geometryWanted = true;
        return;
    }
    startJob(false);
}

void Multiphysics3DDoc::buildMesh()
{
    if (a_closed) return;
    if (a_job) {
        a_meshWanted = true;
        return;
    }
    startJob(true);
}

void Multiphysics3DDoc::cancel()
{
    ++a_stops;
    a_geometryWanted = false;
    a_meshWanted = false;
    a_solveWanted = false;
    if (a_job) a_job->cancel = true;
}

bool Multiphysics3DDoc::idle() const
{
    return !a_job && !a_geometryTimer->isActive() && !a_geometryWanted && !a_meshWanted && !a_solveWanted;
}

QString Multiphysics3DDoc::jobProgress() const
{
    if (!a_job) return {};
    return tr("%1, %2 %").arg(a_stage->text()).arg(a_job->permille.load() / 10);
}

double Multiphysics3DDoc::jobFraction() const
{
    return a_job ? a_job->permille.load() / 1000.0 : -1;
}

void Multiphysics3DDoc::whenJobEnds(int ms, std::function<void(bool)> then)
{
    if (a_closed) {
        QTimer::singleShot(0, qApp, [then] { then(false); });
        return;
    }
    if (idle()) {
        then(true);
        return;
    }
    auto w = std::make_shared<Waiting>();
    w->then = std::move(then);
    w->timer = new QTimer(this);
    w->timer->setSingleShot(true);
    connect(w->timer, &QTimer::timeout, this, [this, w] {
        a_waiting.erase(std::remove(a_waiting.begin(), a_waiting.end(), w), a_waiting.end());
        w->timer->deleteLater();
        w->then(false);
    });
    w->timer->start(std::max(1, ms));
    a_waiting.push_back(w);
}

void Multiphysics3DDoc::wakeWaiting()
{
    if (!idle() || a_waiting.empty()) return;
    const std::vector<std::shared_ptr<Waiting>> ready = std::move(a_waiting);
    a_waiting.clear();
    for (const auto& w : ready) {
        w->timer->stop();
        w->timer->deleteLater();
    }
    for (const auto& w : ready) w->then(true);
}

QThread* Multiphysics3DDoc::jobThread() const
{
    return a_job ? a_job->thread : nullptr;
}

bool Multiphysics3DDoc::waitForIdle(int ms)
{
    // (In an event loop that sleeps, woken as the model's state moves: a
    //  loop of processEvents never slept.)
    misc::waitWhile([this] { return a_job || a_geometryTimer->isActive() || a_geometryWanted || a_meshWanted || a_solveWanted; },
                    ms, [this](QObject* context, const std::function<void()>& look) {
                        connect(this, &Multiphysics3DDoc::stateChanged, context, look, Qt::QueuedConnection);
                    });
    return !a_job && !a_geometryTimer->isActive() && !a_geometryWanted && !a_meshWanted && !a_solveWanted;
}

void Multiphysics3DDoc::startJob(bool mesh, bool solve)
{
    auto job = std::make_shared<Job>();
    job->mesh = mesh;
    job->solve = solve;
    job->studyTag = a_study;
    job->asked = a_asked;
    job->asked.limit = memoryLimit();
    job->model = a_model;
    job->parameters = parameters();
    job->revision = a_solvedRevision;
    if (!a_geometryStale && a_geometry.ok()) job->topology = a_geometry.topology;
    if (solve && job->topology && a_mesh && !a_meshStale) job->givenMesh = a_mesh;
    a_job = job;
    a_stage->setText(solve ? tr("Solving") : mesh ? tr("Building the mesh") : tr("Building the geometry"));
    a_progress->setValue(0);
    a_stage->show();
    a_progress->show();
    a_cancel->show();
    job->thread = QThread::create([job] {
        // (Out of memory in this thread: a failed job, said - uncaught, it
        // aborted Qucs-S, every document with it: the hunt of 9 November,
        // C3.)
        try {
            if (!job->topology) {
                job->geometry = buildGeometry3(job->model, *job->parameters);
                job->built = true;
                if (!job->geometry.ok() || job->cancel) return;
                job->topology = job->geometry.topology;
            }
            // (A solve's mesh made first, where it has none: its first part.)
            const double meshShare = job->solve ? 0.3 : 1.0;
            if ((job->mesh || job->solve) && !job->givenMesh) {
                job->meshBuild = buildMesh3(job->model, *job->parameters, job->topology, [job, meshShare](double fraction, const QString&) {
                    job->permille = int(1000 * meshShare * fraction);
                    return !job->cancel.load();
                });
                if (!job->meshBuild.mesh || job->cancel) return;
                job->givenMesh = job->meshBuild.mesh;
                // (A mesh made alone: what a solve on it would hold and take.)
                if (!job->solve) job->estimates = estimate3(job->model, job->givenMesh);
            }
            if (job->solve)
                job->study = runStudy3(
                    job->model, job->parameters, job->topology, job->givenMesh, job->studyTag,
                    [job, meshShare](double fraction, const QString&) {
                        job->permille = int(1000 * (meshShare + (1 - meshShare) * fraction));
                        return !job->cancel.load();
                    },
                    job->asked);
        } catch (const std::bad_alloc&) {
            const QString why = Multiphysics3DDoc::tr("there was not memory enough for it");
            if (!job->topology) {
                job->built = true;
                job->geometry.failedAt = job->model.geometry().tag;
                job->geometry.errors.insert(job->geometry.failedAt, why);
            } else if ((job->mesh || job->solve) && !job->givenMesh) {
                job->meshBuild.mesh.reset();
                job->meshBuild.error = why;
            } else {
                job->study.error = why;
            }
        }
    });
    // (A main thread's stack: the expressions compile by recursion.)
    job->thread->setStackSize(8u << 20);
    connect(job->thread, &QThread::finished, this, [this, job] {
        if (job == a_job) jobFinished();
    });
    job->started.start();
    a_progressTimer->start();
    job->thread->start();
    updateState();
}

void Multiphysics3DDoc::jobFinished()
{
    std::shared_ptr<Job> job = a_job;
    a_job.reset();
    a_progressTimer->stop();
    a_stage->hide();
    a_progress->hide();
    a_cancel->hide();
    if (!job) return;
    job->thread->deleteLater();
    job->thread = nullptr;
    const bool current = job->revision == a_solvedRevision;
    bool fit = false;
    if (job->cancel) {
        say(tr("Stopped"));
    } else if (current) {
        if (job->built) {
            a_geometry = job->geometry;
            a_geometryStale = false;
            watchFiles();
            const QList<QString> tags = a_geometry.errors.keys() + a_geometry.warnings.keys();
            for (const QString& tag : a_geometry.errors.keys()) {
                const Node* n = a_model.find(tag);
                say(tr("%1: %2").arg(n ? n->name() : tag, a_geometry.errors.value(tag)), true);
            }
            for (const QString& tag : a_geometry.warnings.keys()) {
                const Node* n = a_model.find(tag);
                say(tr("%1: %2").arg(n ? n->name() : tag, a_geometry.warnings.value(tag)));
            }
            if (a_geometry.ok() && a_geometry.topology) {
                const Topology3& t = *a_geometry.topology;
                say(tr("Geometry: %1 domains, %2 faces, %3 edges, %4 points (%5 ms)")
                        .arg(t.domains.size())
                        .arg(t.faces.size())
                        .arg(t.edges.size())
                        .arg(t.points.size())
                        .arg(job->started.elapsed()));
            }
            fit = !a_firstShown && a_geometry.topology && !a_geometry.topology->domains.empty();
        }
        if (job->mesh || (job->solve && job->meshBuild.mesh)) {
            for (const QString& w : job->meshBuild.warnings) say(tr("Mesh: %1").arg(w));
            if (job->meshBuild.mesh) {
                a_mesh = job->meshBuild.mesh;
                a_meshStale = false;
                const Mesh3& m = *a_mesh;
                say(tr("Mesh: %1 prisms, %2 nodes (the plan's %3 triangles through %4 layers); its smallest angle %5°, its worst "
                       "stretch %6 (%7 ms)")
                        .arg(QLocale().toString(qlonglong(m.prisms.size())), QLocale().toString(qlonglong(m.nodes.size())),
                             QLocale().toString(qlonglong(m.plan ? m.plan->triangles.size() : 0)))
                        .arg(m.heights.size() > 1 ? int(m.heights.size()) - 1 : 0)
                        .arg(m.minAngle, 0, 'f', 1)
                        .arg(m.maxStretch, 0, 'g', 3)
                        .arg(job->started.elapsed()));
                // What a solve on it would hold and take, and the limit.
                if (!job->solve) {
                    a_estimates = job->estimates;
                    for (const Estimate3& e : a_estimates) say(tr("Estimated: %1").arg(estimateText(e)));
                    const double limit = memoryLimit();
                    if (const QString past = pastTheLimit(a_estimates, limit); !past.isEmpty()) say(tr("Past the limit: %1").arg(past), true);
                    else if (limit > 0 && !a_estimates.empty())
                        say(tr("Within the memory limit of %1 (Multiphysics 3D > Settings)").arg(bytesText(limit)));
                }
                if (!job->solve) a_show = Show::Mesh;
            } else if (!job->built || job->geometry.ok()) {
                say(tr("Mesh: %1").arg(job->meshBuild.error.isEmpty() ? tr("not made") : job->meshBuild.error), true);
            }
        }
        if (job->solve) {
            if (job->built && !job->geometry.ok()) say(tr("Not solved: the geometry is not built"), true);
            else if (!job->givenMesh) {
                if (job->meshBuild.error.isEmpty() && !job->meshBuild.mesh) say(tr("Not solved: there is no mesh"), true);
            } else {
                if (!job->study.estimates.empty()) a_estimates = job->study.estimates;
                sayFinished(job->study, job->started.elapsed());
            }
        }
    } else if (job->solve) {
        say(tr("Not solved: the model changed while it was - Compute again"), true);
    }
    if (fit) a_firstShown = true;
    updateHighlight();
    updateScene(fit);
    // What was asked meanwhile, or the model changed under it.
    const bool geometryAgain = a_geometryWanted || (a_geometryStale && !job->cancel);
    const bool meshAgain = a_meshWanted;
    const bool solveAgain = a_solveWanted;
    a_geometryWanted = false;
    a_meshWanted = false;
    a_solveWanted = false;
    if (solveAgain) startJob(false, true);
    else if (meshAgain) startJob(true);
    else if (geometryAgain) startJob(false);
    updateState();
    wakeWaiting();
}

void Multiphysics3DDoc::watchFiles()
{
    const QStringList now = a_watcher->files();
    QStringList wanted;
    for (const QString& f : a_geometry.files)
        if (QFileInfo::exists(f)) wanted << f;
    for (const QString& f : now)
        if (!wanted.contains(f)) a_watcher->removePath(f);
    for (const QString& f : wanted)
        if (!now.contains(f)) a_watcher->addPath(f);
}

void Multiphysics3DDoc::updateState()
{
    int shown = a_show == Show::Solution ? 2 : a_show == Show::Mesh ? 1 : 0;
    if (a_show == Show::Plot) {
        const int at = a_showBox->findData(a_plotGroup);
        shown = at >= 0 ? at : 2;
    }
    a_showBox->setCurrentIndex(shown);
    a_plotBox->setEnabled(a_solution != nullptr);
    // (A solution shown as it was before an edit: said beside the bar.)
    if (!a_job) {
        const bool stale = (a_show == Show::Solution || a_show == Show::Plot) && a_solution && a_solutionStale;
        a_stage->setText(stale ? tr("The solution is of the model before its last change: Compute again") : QString());
        a_stage->setVisible(stale);
    }
    emit stateChanged();
}

// ------------------------------------------------------------------ What is shown

void Multiphysics3DDoc::showGeometry()
{
    a_show = Show::Geometry;
    updateScene();
    updateState();
}

void Multiphysics3DDoc::showMesh()
{
    if (!a_mesh || a_meshStale) {
        say(tr("No mesh of the model as it is: Build Mesh first"), true);
        a_show = Show::Geometry;
        updateState();
        return;
    }
    a_show = Show::Mesh;
    updateScene();
    updateState();
}

void Multiphysics3DDoc::fillShowChoices()
{
    if (!a_showBox) return;
    QStringList names, tags;
    for (const Node& c : a_model.results().children)
        if (c.type == QLatin1String("plotgroup3d")) {
            names << c.name();
            tags << c.tag;
        }
    // (Made again where the groups changed alone: a choice open stays.)
    bool same = a_showBox->count() == 3 + names.size();
    for (int i = 0; same && i < names.size(); ++i)
        same = a_showBox->itemText(3 + i) == names[i] && a_showBox->itemData(3 + i).toString() == tags[i];
    if (same) return;
    while (a_showBox->count() > 3) a_showBox->removeItem(3);
    for (int i = 0; i < names.size(); ++i) a_showBox->addItem(names[i], tags[i]);
}

bool Multiphysics3DDoc::showPlotGroup(const QString& tag, QString* error)
{
    auto fail = [this, error](const QString& why) {
        if (error) *error = why;
        say(why, true);
        updateState();
        return false;
    };
    const Node* g = a_model.find(tag);
    if (!g || g->type != QLatin1String("plotgroup3d")) return fail(tr("there is no 3D plot group %1").arg(tag));
    if (!a_solution) return fail(tr("%1: not solved yet - Compute first").arg(g->name()));
    a_plotGroup = tag;
    a_show = Show::Plot;
    updateScene();
    updateState();
    return true;
}

void Multiphysics3DDoc::addDefaultPlots()
{
    if (!a_solution) return;
    Model m = a_model;
    if (addDefaultPlots(m, a_solution->fields())) setModel(m, tr("Default Plots"));
}

bool Multiphysics3DDoc::addDefaultPlots(Model& m, const std::vector<Field3>& fields)
{
    for (const Node& c : m.results().children)
        if (c.type == QLatin1String("plotgroup3d")) return false;
    // Each physics: its variable on the boundaries, and on a slice through the
    // middle across x alone (a surface, opaque, would hide it).
    const QString results = m.results().tag;
    bool any = false;
    // One pair a physics, of its first field: a full-wave solve's fields are
    // normE, Ex, Ey and Ez, and four pairs of one name each ("Electromagnetic
    // Waves, Frequency Domain (ewfd)") read as made again at each plot (the
    // check of 9 November, 5).
    QSet<QString> done;
    for (const Field3& f : fields) {
        if (done.contains(f.tag)) continue;
        done.insert(f.tag);
        const Node* physics = m.find(f.tag);
        const QString name = physics && physics->kind() ? physics->kind()->title : f.type;
        const QString variable = f.tag + QLatin1Char('.') + f.variable;
        for (const QString type : {QStringLiteral("surface"), QStringLiteral("slice")}) {
            Node g = m.make(QStringLiteral("plotgroup3d"));
            g.label = type == QLatin1String("surface") ? tr("%1 (%2)").arg(name, f.tag) : tr("%1, slice (%2)").arg(name, f.tag);
            const Node* added = m.add(results, g);
            if (!added) continue;
            Node plot = m.make(type);
            plot.props.insert(QStringLiteral("expression"), variable);
            if (type == QLatin1String("slice")) plot.props.insert(QStringLiteral("plane"), QStringLiteral("x"));
            m.add(added->tag, plot);
            any = true;
        }
    }
    return any;
}

void Multiphysics3DDoc::showSolution()
{
    if (!a_solution) {
        say(tr("Not solved yet: Compute first"), true);
        updateState();
        return;
    }
    a_show = Show::Solution;
    updateScene();
    updateState();
}

QString Multiphysics3DDoc::plotExpression() const
{
    if (!a_plot.isEmpty()) return a_plot;
    if (a_solution && !a_solution->fields().empty()) {
        const Field3& f = a_solution->fields().front();
        return f.tag + QLatin1Char('.') + f.variable;
    }
    return {};
}

void Multiphysics3DDoc::setPlotExpression(const QString& text)
{
    const QString t = text.trimmed();
    if (t == a_plot) return;
    a_plot = t;
    if (a_solution && !t.isEmpty()) {
        const Expression e = Expression::compile(t, a_solution->scope());
        if (!e.isValid()) say(tr("Plot %1: %2").arg(t, e.error()), true);
    }
    if (a_plotBox && a_plotBox->currentText() != plotExpression()) a_plotBox->setEditText(plotExpression());
    if (a_show == Show::Solution) updateScene();
}

void Multiphysics3DDoc::fillPlotChoices()
{
    if (!a_plotBox || !a_solution) return;
    const QString now = plotExpression();
    a_plotBox->clear();
    for (const Field3& f : a_solution->fields()) {
        const QString p = f.tag + QLatin1Char('.');
        if (f.type == QLatin1String("solid")) {
            for (const char* v : {"disp", "mises", "u", "v", "w", "Ws"}) a_plotBox->addItem(p + QString::fromLatin1(v));
            continue;
        }
        if (f.type == QLatin1String("semiconductor")) {
            for (const char* v : {"V", "n", "p", "Ndop", "normE", "normJ", "Ec", "R"}) a_plotBox->addItem(p + QString::fromLatin1(v));
            continue;
        }
        if (f.type == QLatin1String("magnetic")) {
            for (const char* v : {"normB", "Bx", "By", "Bz", "normH", "Wm", "Az"}) a_plotBox->addItem(p + QString::fromLatin1(v));
            continue;
        }
        a_plotBox->addItem(p + f.variable);
        if (f.type == QLatin1String("heat")) a_plotBox->addItem(p + QStringLiteral("normq"));
        else if (f.type == QLatin1String("electrostatics")) {
            a_plotBox->addItem(p + QStringLiteral("normE"));
            a_plotBox->addItem(p + QStringLiteral("normD"));
        } else if (f.type == QLatin1String("currents")) {
            a_plotBox->addItem(p + QStringLiteral("normJ"));
            a_plotBox->addItem(p + QStringLiteral("Qrh"));
            a_plotBox->addItem(p + QStringLiteral("normE"));
        }
    }
    // (What was plotted kept, where the solution still reads it.)
    if (!a_plot.isEmpty() && !Expression::compile(a_plot, a_solution->scope()).isValid()) a_plot.clear();
    a_plotBox->setEditText(a_plot.isEmpty() ? plotExpression() : now);
}

QString Multiphysics3DDoc::showSpectrum(bool open, QString* error)
{
    auto fail = [error](const QString& why) {
        if (error) *error = why;
        return QString();
    };
    if (!a_solution || !a_solution->spectral || a_solution->optics.empty()) return fail(tr("no light is solved over wavelengths (an eigenmode expansion or a full-wave solve)"));
    const Solution3::Optics& o = a_solution->optics.front();
    if (o.results.size() < 2) return fail(tr("%1 is solved at one wavelength: a spectrum wants two or more").arg(o.physics));
    // By wavelength, rising.
    std::vector<int> order(o.results.size());
    for (std::size_t k = 0; k < order.size(); ++k) order[k] = int(k);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return o.results[std::size_t(a)].frequency > o.results[std::size_t(b)].frequency; });
    const int N = int(o.results.front().portModes.size());
    qucs_s::dataset::Variable lambda;
    lambda.name = QStringLiteral("lambda");
    lambda.independent = true;
    for (int k : order) lambda.re << 299792458.0 / o.results[std::size_t(k)].frequency;
    QList<qucs_s::dataset::Variable> vars{lambda};
    QJsonObject units{{QStringLiteral("lambda"), QStringLiteral("m")}};
    QStringList traces;
    int first = -1;
    for (int j = 0; j < N; ++j) {
        if (!o.device.ports[std::size_t(o.results.front().portModes[std::size_t(j)].port)].excite) continue;
        if (first < 0) first = j;
        for (int i = 0; i < N; ++i) {
            const QString base = N <= 9 ? QStringLiteral("S%1%2").arg(i + 1).arg(j + 1) : QStringLiteral("S_%1_%2").arg(i + 1).arg(j + 1);
            qucs_s::dataset::Variable dB, arg;
            dB.name = base + QStringLiteral("dB");
            arg.name = base + QStringLiteral("_arg");
            dB.dependencies = arg.dependencies = QStringList{QStringLiteral("lambda")};
            for (int k : order) {
                const std::complex<double> v = o.results[std::size_t(k)].S(i, j);
                dB.re << 10 * std::log10(std::max(std::norm(v), 1e-300));
                arg.re << std::arg(v) * 180 / M_PI;
            }
            units.insert(dB.name, QStringLiteral("dB"));
            units.insert(arg.name, QStringLiteral("deg"));
            if (j == first) traces << dB.name;
            vars << dB << arg;
        }
    }
    for (int i = 0; i < N; ++i) {
        qucs_s::dataset::Variable n;
        const EmeResult::PortMode& pm = o.results.front().portModes[std::size_t(i)];
        n.name = QStringLiteral("neff_%1_%2").arg(o.device.ports[std::size_t(pm.port)].name, pm.label);
        n.dependencies = QStringList{QStringLiteral("lambda")};
        for (int k : order) n.re << o.results[std::size_t(k)].portModes[std::size_t(i)].neff.real();
        vars << n;
    }
    if (traces.isEmpty()) return fail(tr("%1 feeds no port mode").arg(o.physics));
    QString base;
    if (getDocName().isEmpty()) {
        const QString dir = misc::cacheDir() + QStringLiteral("/multiphysics3d");
        QDir().mkpath(dir);
        base = dir + QStringLiteral("/untitled");
    } else {
        const QFileInfo fi(getDocName());
        base = fi.absoluteDir().filePath(fi.completeBaseName());
    }
    const QString dat = base + QStringLiteral("_spectrum.dat"), dpl = base + QStringLiteral("_spectrum.dpl");
    qucs_s::dataimport::Data data;
    data.variables = vars;
    qucs_s::dataimport::Origin origin;
    origin.source = getDocName().isEmpty() ? tr("(a model not saved)") : getDocName();
    origin.format = tr("Qucs-S multiphysics 3D model");
    origin.imported = QDateTime::currentDateTime();
    origin.units = units;
    QString why;
    if (!qucs_s::dataimport::writeDataset(dat, data, origin, &why)) return fail(why);
    say(tr("Spectrum: %1 written (each S-parameter's |S|² in dB and phase, each port mode's index, against the wavelength)").arg(QFileInfo(dat).fileName()));
    if (open && a_App != nullptr) {
        const QString shown = a_App->showPythonDisplay(QJsonObject{{QStringLiteral("display"), dpl},
                                                                   {QStringLiteral("dataset"), dat},
                                                                   {QStringLiteral("traces"), QJsonArray::fromStringList(traces)},
                                                                   {QStringLiteral("type"), QStringLiteral("rect")},
                                                                   {QStringLiteral("title"), tr("%1: |S|² of port mode %2 fed").arg(o.physics).arg(first + 1)},
                                                                   {QStringLiteral("replace"), true},
                                                                   {QStringLiteral("ylabel"), tr("|S|² (dB)")},
                                                                   {QStringLiteral("xlabel"), tr("lambda (m)")}});
        if (!shown.isEmpty()) return fail(shown);
        for (QucsDoc* d : a_App->allDocuments())
            if (QFileInfo(d->getDocName()) == QFileInfo(dpl) && d->getDocChanged()) d->save();
    }
    return dat;
}

void Multiphysics3DDoc::fillTimeChoices()
{
    if (!a_timeBox) return;
    a_timeBox->clear();
    // (In time, its output times; over wavelengths, its wavelengths; of an
    // Eigenfrequency step, its modes' frequencies.)
    const bool spectral = a_solution && a_solution->spectral && a_solution->snapshotCount() > 0;
    const bool modes = a_solution && a_solution->eigen && a_solution->snapshotCount() > 0;
    const bool small = a_solution && a_solution->smallSignal && a_solution->snapshotCount() > 0;
    const bool inTime = a_solution && (a_solution->timeDependent || spectral || modes || small) && a_solution->snapshotCount() > 0;
    a_timeLabel->setVisible(inTime);
    a_timeBox->setVisible(inTime);
    if (!inTime) return;
    a_timeLabel->setText(spectral ? tr("Wavelength:") : modes ? tr("Eigenfrequency:") : small ? tr("Frequency:") : tr("Time:"));
    for (int i = 0; i < a_solution->snapshotCount(); ++i)
        a_timeBox->addItem(spectral ? formatQuantity(299792458.0 / a_solution->snapshotFrequency(i), Dim::length())
                           : modes  ? tr("%1: %2").arg(i + 1).arg(formatQuantity(a_solution->snapshotFrequency(i), Dim::of(0, 0, -1)))
                           : small  ? formatQuantity(a_solution->snapshotFrequency(i), Dim::of(0, 0, -1))
                                    : formatQuantity(a_solution->snapshotTime(i), Dim::of(0, 0, 1)));
    a_timeBox->setCurrentIndex(a_solution->selected());
}

DerivedResult Multiphysics3DDoc::evaluate(const QString& tag)
{
    DerivedResult out;
    const Node* n = a_model.find(tag);
    static const QStringList derived = {QStringLiteral("global"), QStringLiteral("integral"), QStringLiteral("average"), QStringLiteral("maximum"),
                                        QStringLiteral("minimum"), QStringLiteral("pointeval")};
    if (!n || !derived.contains(n->type)) {
        out.error = n ? tr("%1 is not a derived value that a 3D model evaluates").arg(n->name()) : tr("there is no node %1").arg(tag);
        return out;
    }
    if (a_solutions.empty()) {
        out.error = tr("%1: the model is not computed yet - Compute first").arg(n->name());
        say(out.error, true);
        return out;
    }
    // At each output time and point it asks for, or at the one shown; that
    // one shown again after.
    const int shown = shownTime();
    std::vector<Instance3> each;
    if (n->text(QStringLiteral("solutions")) != QLatin1String("selected")) {
        each = instances3(a_solutions, true);
    } else {
        Instance3 at;
        at.point = std::max(0, shownPoint());
        at.snapshot = shown;
        each.push_back(at);
    }
    for (const Instance3& at : each) {
        Solution3* s = select3(a_solutions, at);
        if (!s) continue;
        const DerivedResult one = evaluateDerived3(*s, *n);
        for (DerivedValue v : one.values) {
            if (!at.label.isEmpty()) v.name = at.label + QStringLiteral(": ") + v.name;
            out.values << v;
        }
        if (out.error.isEmpty() && !one.error.isEmpty()) out.error = at.label.isEmpty() ? one.error : at.label + QStringLiteral(": ") + one.error;
        if (out.note.isEmpty()) out.note = one.note;
    }
    for (const auto& s : a_solutions)
        if (s->snapshotCount() > 0) s->select(s->snapshotCount() - 1);
    if (a_solution && shown >= 0) a_solution->select(shown);
    // Its rows in the table, in place of its last.
    for (int r = a_table->rowCount() - 1; r >= 0; --r)
        if (a_table->item(r, 0) && a_table->item(r, 0)->data(Qt::UserRole).toString() == tag) a_table->removeRow(r);
    for (const DerivedValue& v : out.values) {
        const int r = a_table->rowCount();
        a_table->insertRow(r);
        auto* name = new QTableWidgetItem(n->name());
        name->setData(Qt::UserRole, tag);
        a_table->setItem(r, 0, name);
        a_table->setItem(r, 1, new QTableWidgetItem(v.name));
        a_table->setItem(r, 2, new QTableWidgetItem(v.text));
    }
    a_table->setVisible(a_table->rowCount() > 0);
    a_table->resizeColumnsToContents();
    for (const DerivedValue& v : out.values) say(tr("%1: %2 = %3").arg(n->name(), v.name, v.text));
    if (!out.error.isEmpty()) say(tr("%1: %2").arg(n->name(), out.error), out.values.isEmpty());
    else if (!out.note.isEmpty()) say(tr("%1: %2").arg(n->name(), out.note));
    return out;
}

QString Multiphysics3DDoc::runExport(const QString& tag, QString* error, const std::function<QString(const QString&)>& beforeWrite)
{
    auto fail = [this, error](const QString& why) {
        if (error) *error = why;
        say(why, true);
        return QString();
    };
    const Node* n = a_model.find(tag);
    if (!n || !n->kind() || n->kind()->group != QLatin1String("export")) return fail(tr("there is no export %1").arg(tag));
    if (a_solutions.empty()) return fail(tr("%1: the model is not computed yet - Compute first").arg(n->name()));
    // Where: its File (relative to the model's folder), else beside the
    // model (in the cache while it has no file) as model_tag.
    QString path = n->text(QStringLiteral("file")).trimmed();
    QString dir, base;
    if (path.isEmpty()) {
        if (getDocName().isEmpty()) {
            dir = misc::cacheDir() + QStringLiteral("/multiphysics3d");
            QDir().mkpath(dir);
            base = QStringLiteral("untitled_") + tag;
        } else {
            dir = QFileInfo(getDocName()).absolutePath();
            base = QFileInfo(getDocName()).completeBaseName() + QLatin1Char('_') + tag;
        }
    } else {
        if (QFileInfo(path).isRelative()) {
            if (getDocName().isEmpty()) return fail(tr("%1: %2 is relative to the model's folder - save the model first").arg(n->name(), path));
            path = QFileInfo(getDocName()).absoluteDir().absoluteFilePath(path);
        }
        dir = QFileInfo(path).absolutePath();
        base = QFileInfo(path).completeBaseName();
    }
    const int shown = shownTime();
    const Export3 out = export3(a_solutions, *n, base);
    for (const auto& s : a_solutions)
        if (s->snapshotCount() > 0) s->select(s->snapshotCount() - 1);
    if (a_solution && shown >= 0) a_solution->select(shown);
    if (!out.main.error.isEmpty()) return fail(tr("%1: %2").arg(n->name(), out.main.error));
    if (path.isEmpty()) path = QDir(dir).filePath(base + QLatin1Char('.') + out.main.suffix);
    if (!QFileInfo(dir).isDir() && !QDir().mkpath(dir))
        return fail(tr("%1: the folder %2 cannot be made, so %3 is not written").arg(n->name(), QDir::toNativeSeparators(dir), QFileInfo(path).fileName()));
    auto write = [&fail, &beforeWrite, n](const QString& to, const QByteArray& bytes) {
        if (beforeWrite)
            if (const QString no = beforeWrite(to); !no.isEmpty()) {
                fail(no);
                return false;
            }
        QSaveFile file(to);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) < 0 || !file.commit()) {
            fail(tr("%1: %2 cannot be written: %3").arg(n->name(), QDir::toNativeSeparators(to), file.errorString()));
            return false;
        }
        return true;
    };
    if (!write(path, out.main.data.isEmpty() ? out.main.text.toUtf8() : out.main.data)) return QString();
    if (!out.main.coordinates.isEmpty() && !write(QDir(dir).filePath(base + QStringLiteral("_xy.npy")), out.main.coordinates)) return QString();
    for (const auto& [end, bytes] : out.parts)
        if (!write(QDir(dir).filePath(base + end), bytes)) return QString();
    say(tr("%1: %2 written - %3").arg(n->name(), QFileInfo(path).fileName(), out.main.summary));
    return path;
}

void Multiphysics3DDoc::fillPointChoices()
{
    if (!a_pointBox) return;
    a_pointBox->clear();
    const bool swept = a_solutions.size() > 1;
    a_pointLabel->setVisible(swept);
    a_pointBox->setVisible(swept);
    if (!swept) return;
    for (const auto& s : a_solutions) a_pointBox->addItem(s->label);
    a_pointBox->setCurrentIndex(shownPoint());
}

int Multiphysics3DDoc::shownPoint() const
{
    for (std::size_t i = 0; i < a_solutions.size(); ++i)
        if (a_solutions[i] == a_solution) return int(i);
    return -1;
}

void Multiphysics3DDoc::setShownPoint(int index)
{
    if (index < 0 || index >= int(a_solutions.size()) || a_solutions[std::size_t(index)] == a_solution) return;
    // (The output time shown kept, where the point has it.)
    const int time = shownTime();
    a_solution = a_solutions[std::size_t(index)];
    if (time >= 0 && time < a_solution->snapshotCount()) a_solution->select(time);
    if (a_pointBox && a_pointBox->currentIndex() != index) a_pointBox->setCurrentIndex(index);
    fillTimeChoices();
    if (a_show == Show::Solution) updateScene();
    emit stateChanged();
}

int Multiphysics3DDoc::shownTime() const
{
    return a_solution && a_solution->snapshotCount() > 0 ? a_solution->selected() : -1;
}

void Multiphysics3DDoc::setShownTime(int index)
{
    if (!a_solution || a_solution->snapshotCount() == 0) return;
    a_solution->select(index);
    if (a_timeBox && a_timeBox->currentIndex() != a_solution->selected()) a_timeBox->setCurrentIndex(a_solution->selected());
    if (a_show == Show::Solution) updateScene();
    emit stateChanged();
}

void Multiphysics3DDoc::sayFinished(const StudyRun3& r, qint64 ms)
{
    for (const QString& line : r.log) say(line);
    if (!r.ok()) {
        say(tr("Not solved: %1").arg(r.error), true);
        return;
    }
    // (A sweep's points each a solution: the last shown.)
    a_solutions = r.solutions;
    a_solution = r.solutions.back();
    a_solutionStale = false;
    for (const QString& w : r.warnings) say(tr("Warning: %1").arg(w));
    if (!r.stopped.isEmpty()) say(r.stopped, true);
    if (a_solutions.size() > 1) say(tr("%1 points of %2: %3, the last, shown").arg(a_solutions.size()).arg(r.swept.join(QLatin1String(", ")), a_solution->label));
    if (a_solution->timeDependent && a_solution->snapshotCount() > 0)
        say(tr("At t = %1, the last of %2 output times:")
                .arg(formatQuantity(a_solution->time(), Dim::of(0, 0, 1)))
                .arg(a_solution->snapshotCount()));
    if (a_solution->spectral && !a_solution->optics.empty()) {
        // An eigenmode expansion's or a full-wave solve's: its ports' modes
        // and |S|² in dB at the last wavelength, a row a fed port mode - not every global (five of
        // each S-parameter).
        say(tr("At λ = %1, the last of %2 wavelengths:")
                .arg(formatQuantity(299792458.0 / a_solution->frequency(), Dim::length()))
                .arg(a_solution->snapshotCount()));
        for (const Solution3::Optics& o : a_solution->optics) {
            if (o.results.empty()) continue;
            const EmeResult& r = o.results.back();
            QStringList modes;
            for (int i = 0; i < int(r.portModes.size()); ++i)
                modes << tr("%1: port %2 %3 (neff %4)")
                             .arg(i + 1)
                             .arg(o.device.ports[std::size_t(r.portModes[std::size_t(i)].port)].name, r.portModes[std::size_t(i)].label,
                                  formatNumber(r.portModes[std::size_t(i)].neff.real(), 6));
            say(tr("%1, its S-parameter ports - %2").arg(o.physics, modes.join(QLatin1String("; "))));
            for (int j = 0; j < int(r.portModes.size()); ++j) {
                if (!o.device.ports[std::size_t(r.portModes[std::size_t(j)].port)].excite) continue;
                QStringList row;
                for (int i = 0; i < int(r.portModes.size()); ++i)
                    row << QStringLiteral("S%1%2 %3 dB").arg(i + 1).arg(j + 1).arg(formatNumber(10 * std::log10(std::norm(r.S(i, j))), 4));
                say(tr("  fed %1: %2; out %3 %%").arg(j + 1).arg(row.join(QLatin1String(", ")), formatNumber(100 * r.balance[std::size_t(j)], 5)));
            }
        }
        say(tr("Each is a global at each wavelength (%1.S21, %1.S21dB, %1.S21_arg, %1.neff_1_TE0, %1.T): a Derived Values node's Global "
               "Evaluation lists them over the wavelengths, a Touchstone export writes the whole S-matrix")
                .arg(a_solution->optics.front().physics));
        // Two wavelengths or more: the spectra's dataset written (shown by
        // Plot Spectrum).
        if (a_solution->snapshotCount() > 1) showSpectrum(false);
    } else
        for (const Variable& g : a_solution->globals())
            say(tr("%1 = %2%3").arg(g.name, a_solution->globalText(g.name),
                                    g.description.isEmpty() ? QString() : QStringLiteral(" (") + g.description + QLatin1Char(')')));
    say(tr("Shown: %1 (%2 ms in all)").arg(plotExpression()).arg(ms));
    fillPlotChoices();
    fillPointChoices();
    fillTimeChoices();
    if (a_show != Show::Plot) a_show = Show::Solution;
    addDefaultPlots();
}

void Multiphysics3DDoc::focusNode(const QString& tag)
{
    if (tag == a_focus) return;
    a_focus = tag;
    updateHighlight();
    updateScene();
    updateVisibilityButton();
}

// ------------------------------------------------------------------ What is drawn of each part

QString Multiphysics3DDoc::visibilityName(Visibility v)
{
    return v == Visibility::Hidden ? QStringLiteral("hidden") : v == Visibility::Frame ? QStringLiteral("frame") : QString();
}

Multiphysics3DDoc::Visibility Multiphysics3DDoc::visibilityOf(const QString& name)
{
    if (name == QLatin1String("hidden")) return Visibility::Hidden;
    if (name == QLatin1String("frame")) return Visibility::Frame;
    return Visibility::Shown;
}

namespace {

/// Hidden below its frame below shown: of two, the lesser drawn.
int drawnRank(Multiphysics3DDoc::Visibility v)
{
    return v == Multiphysics3DDoc::Visibility::Hidden ? 0 : v == Multiphysics3DDoc::Visibility::Frame ? 1 : 2;
}

Multiphysics3DDoc::Visibility leastDrawn(Multiphysics3DDoc::Visibility a, Multiphysics3DDoc::Visibility b)
{
    return drawnRank(a) <= drawnRank(b) ? a : b;
}

bool hideable(const Node& n)
{
    const NodeKind* k = n.kind();
    if (!k) return false;
    if (k->group == QLatin1String("geometry")) return n.type != QLatin1String("form union");
    return k->group == QLatin1String("material");
}

void eachNode(const Node& n, const std::function<void(const Node&)>& f)
{
    f(n);
    for (const Node& c : n.children) eachNode(c, f);
}

} // namespace

bool Multiphysics3DDoc::canHide(const Model& model, const QString& tag)
{
    const Node* n = model.find(tag);
    if (!n || !hideable(*n)) return false;
    // (Under the geometry or the materials: not a material of a physics'.)
    for (const Node* under : {&model.geometry(), &model.materials()}) {
        bool found = false;
        eachNode(*under, [&](const Node& c) { found = found || &c == n; });
        if (found) return true;
    }
    return false;
}

Multiphysics3DDoc::Visibility Multiphysics3DDoc::visibility(const QString& tag) const
{
    const Node* n = a_model.find(tag);
    return n && canHide(tag) ? visibilityOf(n->visibility) : Visibility::Shown;
}

bool Multiphysics3DDoc::setVisibility(const QString& tag, Visibility v)
{
    if (!canHide(tag) || visibility(tag) == v) return false;
    const Node* n = a_model.find(tag);
    const QString what = v == Visibility::Hidden ? tr("Hide %1") : v == Visibility::Frame ? tr("Frame Only %1") : tr("Show %1");
    return editNode(tag, [name = visibilityName(v)](Node& node) { node.visibility = name; }, what.arg(n->name()));
}

bool Multiphysics3DDoc::anyHidden() const
{
    bool any = false;
    eachNode(a_model.root, [&any](const Node& n) { any = any || !n.visibility.isEmpty(); });
    return any;
}

bool Multiphysics3DDoc::showAllParts()
{
    if (!anyHidden()) return false;
    Model m = a_model;
    clearVisibility(m.root);
    return setModel(m, tr("Show All"));
}

std::vector<Multiphysics3DDoc::Visibility> Multiphysics3DDoc::domainVisibilities(const Topology3& t) const
{
    std::vector<Visibility> out(t.domains.size(), Visibility::Shown);
    // The features hidden or framed, by name - a solid's own, or its
    // group's (an extrude's, a stack's) -, as a feature's solids are
    // chosen.
    QHash<QString, Visibility> byName;
    eachNode(a_model.geometry(), [&byName](const Node& n) {
        if (n.visibility.isEmpty() || !hideable(n)) return;
        const QString name = n.label.isEmpty() ? n.tag : n.label;
        byName[name] = leastDrawn(byName.value(name, Visibility::Shown), visibilityOf(n.visibility));
    });
    if (!byName.isEmpty()) {
        std::vector<Visibility> solid(std::size_t(t.objectNames.size()), Visibility::Shown);
        for (int i = 0; i < t.objectNames.size(); ++i)
            solid[std::size_t(i)] = leastDrawn(byName.value(t.objectNames.at(i), Visibility::Shown),
                                               byName.value(t.objectGroups.value(i), Visibility::Shown));
        // A domain as the solid it is of: the smallest that holds it (the
        // core in its cladding the core's).
        for (std::size_t d = 0; d < t.domains.size(); ++d) {
            const int owner = t.domains[d].owner;
            if (owner >= 0 && owner < int(solid.size())) out[d] = solid[std::size_t(owner)];
        }
    }
    for (const Node& m : a_model.materials().children) {
        if (m.visibility.isEmpty() || !hideable(m)) continue;
        const Visibility v = visibilityOf(m.visibility);
        for (int d : resolveSelection3(t, a_model, Level::Domain, m.selection()))
            if (d >= 0 && d < int(out.size())) out[std::size_t(d)] = leastDrawn(out[std::size_t(d)], v);
    }
    return out;
}

void Multiphysics3DDoc::updateVisibilityButton()
{
    if (!a_visibilityButton) return;
    const bool can = canHide(a_focus);
    const Visibility v = visibility(a_focus);
    for (QAction* a : {a_showAction, a_hideAction, a_frameAction}) a->setEnabled(can);
    a_showAction->setChecked(can && v == Visibility::Shown);
    a_hideAction->setChecked(can && v == Visibility::Hidden);
    a_frameAction->setChecked(can && v == Visibility::Frame);
    a_showAllAction->setEnabled(anyHidden());
    a_visibilityButton->setEnabled(can || anyHidden());
    const Node* n = a_model.find(a_focus);
    a_visibilityButton->setToolTip(can && n ? tr("How the view draws %1: shown, hidden or its frame alone").arg(n->name())
                                            : tr("How the view draws the part chosen in the tree (a solid's feature, a material): shown, hidden "
                                                 "or its frame alone"));
}

void Multiphysics3DDoc::updateHighlight()
{
    a_highlightLevel = Level::None;
    a_highlighted.clear();
    const Node* n = a_model.find(a_focus);
    const Topology3* t = a_geometry.topology.get();
    if (!n || !t || t->domains.empty()) return;
    const NodeKind* k = n->kind();
    if (!k) return;
    if (k->property(QStringLiteral("selection"))) {
        Level level = k->selection;
        if (k->property(QStringLiteral("level"))) level = levelOf(n->text(QStringLiteral("level")));
        if (level == Level::None) level = Level::Domain;
        a_highlighted = resolveSelection3(*t, a_model, level, n->selection());
        a_highlightLevel = level;
    } else if (k->group == QLatin1String("geometry") && n->type != QLatin1String("form union")) {
        // A feature's solids: those of its name, or of its group (an
        // extrude's, a stack's).
        const QString name = n->label.isEmpty() ? n->tag : n->label;
        QSet<int> solids;
        for (int i = 0; i < t->objectNames.size(); ++i)
            if (t->objectNames.at(i) == name || t->objectGroups.value(i) == name) solids.insert(i);
        if (solids.isEmpty()) return;
        // The domains it owns - it the smallest solid that holds them -, as
        // Hide hides them and a material on it follows: the oxide's chosen
        // lights its cladding, not the core in it (the check of 9 November,
        // 2). One that owns none - all of it under others - lights what it
        // covers, so choosing it still shows where it is.
        for (int d = 0; d < int(t->domains.size()); ++d)
            if (solids.contains(t->domains[std::size_t(d)].owner)) a_highlighted << d;
        if (a_highlighted.isEmpty())
            for (int d = 0; d < int(t->domains.size()); ++d)
                if (t->domains[std::size_t(d)].objects.intersects(solids)) a_highlighted << d;
        a_highlightLevel = Level::Domain;
    }
}

namespace {

/// \a v's colour between \a lo and \a hi on \a table: grey where it is
/// not finite.
QColor colourIn(const std::vector<QRgb>& table, double lo, double hi, double v)
{
    if (!std::isfinite(v)) return QColor(190, 190, 190);
    const double u = hi > lo ? (v - lo) / (hi - lo) : 0.5;
    return QColor::fromRgb(table[std::size_t(std::clamp(int(u * double(table.size() - 1) + 0.5), 0, int(table.size()) - 1))]);
}

/// The numbers of a list - "1, 2", "range(0, 0.5, 2)", "linspace(...)" -:
/// lengths in the geometry's unit (\a lengths, a unit of length given
/// changed into it), else values in \a unit (given in it), SI out.
std::vector<double> numbersOf(const QString& text, const Scope& scope, bool lengths, double unitScale, const DisplayUnit& unit, QString* why)
{
    Dim dim;
    const std::optional<std::vector<double>> listed = valueList(text, scope, &dim, why);
    if (!listed) return {};
    std::vector<double> out = *listed;
    for (double& v : out) {
        if (lengths) v = dim.isNone() ? v : v / unitScale;
        else if (dim.isNone() && !unit.name.isEmpty()) v = v * unit.scale + unit.offset;
    }
    return out;
}

/// A face's quality: a floor's triangle's, 4√3 A/(a² + b² + c²); a wall's
/// height over its width or the other way, the smaller - 1 at best.
double faceQuality(const Mesh3& m, const Mesh3::Face& f)
{
    auto at = [&m](int k) { return m.nodes[std::size_t(k)]; };
    auto length = [](const Vec3& a, const Vec3& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); };
    if (f.quad) {
        const double w = length(at(f.nodes[0]), at(f.nodes[1])), h = length(at(f.nodes[1]), at(f.nodes[2]));
        return w > 0 && h > 0 ? std::min(w, h) / std::max(w, h) : 0;
    }
    const Vec3 a = at(f.nodes[0]), b = at(f.nodes[1]), c = at(f.nodes[2]);
    const double ab = length(a, b), bc = length(b, c), ca = length(c, a);
    const double area = 0.5 * std::abs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
    const double sum = ab * ab + bc * bc + ca * ca;
    return sum > 0 ? 4 * std::sqrt(3.0) * area / sum : 0;
}

} // namespace

void Multiphysics3DDoc::plotGroupScene(const Node& g, Solution3& solution, View3D::Scene& scene, View3D::Legend& legend, int* clipAxis,
                                       double* clipShare)
{
    a_plotProblems.clear();
    // Its solution: the group's point and time, else the tab's.
    Solution3* sol = &solution;
    const int point = g.integer(QStringLiteral("point"));
    if (point > 0 && point <= int(a_solutions.size())) sol = a_solutions[std::size_t(point) - 1].get();
    const int before = sol->selected();
    const QString time = g.text(QStringLiteral("time")).trimmed();
    if (!time.isEmpty() && sol->snapshotCount() > 0) {
        QString why;
        if (const std::optional<double> t = evaluateConstant(time, sol->parameters().scope, Dim::of(0, 0, 1), &why)) sol->select(sol->snapshotAt(*t));
        else a_plotProblems << tr("%1's time: %2").arg(g.name(), why);
    }
    const Mesh3& m = sol->mesh();
    const Topology3* t = m.topology.get();
    const Box3 box = t ? t->bounds : Box3();
    const double k = m.unitScale;
    // Its clip: the axis, where across the model.
    const QString clip = g.text(QStringLiteral("clip"));
    *clipAxis = clip == QLatin1String("x") ? 0 : clip == QLatin1String("y") ? 1 : clip == QLatin1String("z") ? 2 : -1;
    double clipAt = 0;
    if (*clipAxis >= 0) {
        Expression::Options options;
        options.lengthUnit = k;
        options.numbersAreLengths = true;
        const Expression at = Expression::compile(g.text(QStringLiteral("clipat")), sol->parameters().scope, options);
        clipAt = at.isValid() && at.isConstant() ? at.constant() : 0;
        const double lo = *clipAxis == 0 ? box.x0 : *clipAxis == 1 ? box.y0 : box.z0, hi = *clipAxis == 0 ? box.x1 : *clipAxis == 1 ? box.y1 : box.z1;
        *clipShare = hi > lo ? std::clamp((clipAt - lo) / (hi - lo), 0.0, 1.0) : 0.5;
    }
    // The first coloured plot: the legend's, and the clip's cut face.
    struct Coloured {
        Expression e;
        QString text;
        DisplayUnit unit;
        const std::vector<QRgb>* table = nullptr;
        double lo = 0, hi = 0;
    };
    std::optional<Coloured> first;
    // A coloured plot's range: its values', or its own given.
    auto rangeOf = [&sol](const Node& p, const DisplayUnit& unit, const std::vector<double>& values) {
        double lo = std::numeric_limits<double>::infinity(), hi = -lo;
        for (double v : values)
            if (std::isfinite(v)) {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        if (p.text(QStringLiteral("range")) == QLatin1String("manual")) {
            const std::optional<double> a = evaluateConstant(p.text(QStringLiteral("min")), sol->parameters().scope, Dim());
            const std::optional<double> b = evaluateConstant(p.text(QStringLiteral("max")), sol->parameters().scope, Dim());
            if (a) lo = unit.name.isEmpty() ? *a : *a * unit.scale + unit.offset;
            if (b) hi = unit.name.isEmpty() ? *b : *b * unit.scale + unit.offset;
        }
        if (!(hi >= lo)) lo = hi = 0;
        return std::make_pair(lo, hi);
    };
    const QColor text = palette().color(QPalette::Text);
    for (const Node& p : g.children) {
        if (!p.enabled) continue;
        const QString type = p.type;
        if (type == QLatin1String("meshplot")) {
            const bool quality = p.flag(QStringLiteral("quality"));
            const std::vector<QRgb>& table = colorTable(QStringLiteral("rainbow"));
            for (const Mesh3::Face& f : m.faces) {
                const Topology3::Face* tf = t && f.face >= 0 && f.face < int(t->faces.size()) ? &t->faces[std::size_t(f.face)] : nullptr;
                const QColor colour = quality ? colourIn(table, 0, 1, faceQuality(m, f)) : domainColor(tf ? std::max(tf->back, tf->front) : -1);
                const Vec3 a = m.nodes[std::size_t(f.nodes[0])], b = m.nodes[std::size_t(f.nodes[1])], c = m.nodes[std::size_t(f.nodes[2])];
                scene.triangles.push_back({{a, b, c}, colour, f.face});
                if (f.quad) scene.triangles.push_back({{a, c, m.nodes[std::size_t(f.nodes[3])]}, colour, f.face});
                const int n = f.quad ? 4 : 3;
                for (int i = 0; i < n; ++i)
                    scene.lines.push_back({m.nodes[std::size_t(f.nodes[std::size_t(i)])], m.nodes[std::size_t(f.nodes[std::size_t((i + 1) % n)])], text, 1, -1});
            }
            if (quality && !legend.shown) {
                legend.title = tr("Mesh quality");
                legend.low = QStringLiteral("0");
                legend.high = QStringLiteral("1");
                legend.table = table;
                legend.shown = true;
            }
            continue;
        }
        if (type == QLatin1String("arrowvolume")) {
            const std::array<Expression, 3> v{Expression::compile(p.text(QStringLiteral("x")), sol->scope()),
                                              Expression::compile(p.text(QStringLiteral("y")), sol->scope()),
                                              Expression::compile(p.text(QStringLiteral("z")), sol->scope())};
            bool ok = true;
            for (const Expression& e : v)
                if (!e.isValid()) {
                    a_plotProblems << tr("%1: %2").arg(p.name(), e.error());
                    ok = false;
                    break;
                }
            if (!ok) continue;
            const int nx = std::max(1, p.integer(QStringLiteral("nx"))), ny = std::max(1, p.integer(QStringLiteral("ny"))),
                      nz = std::max(1, p.integer(QStringLiteral("nz")));
            const std::vector<Arrow3> arrows = arrows3(*sol, v, box, nx, ny, nz);
            const double cell = std::min({(box.x1 - box.x0) / nx, (box.y1 - box.y0) / ny, (box.z1 - box.z0) / nz});
            double longest = 0;
            for (const Arrow3& a : arrows) longest = std::max(longest, std::sqrt(a.v.x * a.v.x + a.v.y * a.v.y + a.v.z * a.v.z));
            const bool normalized = p.text(QStringLiteral("scaling")) == QLatin1String("normalized");
            QColor colour(p.text(QStringLiteral("color")));
            if (!colour.isValid()) colour = text;
            for (const Arrow3& a : arrows) {
                const double norm = std::sqrt(a.v.x * a.v.x + a.v.y * a.v.y + a.v.z * a.v.z);
                if (!(norm > 0) || !(longest > 0)) continue;
                const double length = 0.9 * cell * (normalized ? 1 : norm / longest);
                const Vec3 d{a.v.x / norm, a.v.y / norm, a.v.z / norm};
                const Vec3 tip{a.at.x + length * d.x, a.at.y + length * d.y, a.at.z + length * d.z};
                scene.lines.push_back({a.at, tip, colour, 1.5f, -1});
                // Its head: two strokes back from the tip, across it.
                Vec3 across{-d.y, d.x, 0};
                if (std::abs(d.z) > 0.9) across = Vec3{1, 0, 0};
                const double an = std::sqrt(across.x * across.x + across.y * across.y + across.z * across.z);
                across = Vec3{across.x / an, across.y / an, across.z / an};
                const double h = 0.3 * length;
                for (double side : {1.0, -1.0})
                    scene.lines.push_back({tip, Vec3{tip.x - h * d.x + side * 0.5 * h * across.x, tip.y - h * d.y + side * 0.5 * h * across.y,
                                                     tip.z - h * d.z + side * 0.5 * h * across.z},
                                           colour, 1.5f, -1});
            }
            continue;
        }
        if (type != QLatin1String("surface") && type != QLatin1String("slice") && type != QLatin1String("isosurface")) continue;
        const QString what = p.text(QStringLiteral("expression"));
        const Expression e = Expression::compile(what, sol->scope());
        if (!e.isValid()) {
            a_plotProblems << tr("%1: %2").arg(p.name(), e.error());
            continue;
        }
        QString warning;
        const DisplayUnit unit = displayUnit(p.text(QStringLiteral("unit")), e.dim(), &warning);
        if (!warning.isEmpty()) a_plotProblems << tr("%1: %2").arg(p.name(), warning);
        const QString tableName = p.text(QStringLiteral("colors")).isEmpty() ? QStringLiteral("rainbow") : p.text(QStringLiteral("colors"));
        const std::vector<QRgb>& table = colorTable(tableName);
        double lo = 0, hi = 0;
        if (type == QLatin1String("surface")) {
            const std::vector<double> nodal = nodalValues(*sol, e);
            std::vector<double> on;
            for (const Mesh3::Face& f : m.faces)
                for (int i = 0; i < (f.quad ? 4 : 3); ++i) on.push_back(nodal[std::size_t(f.nodes[std::size_t(i)])]);
            std::tie(lo, hi) = rangeOf(p, unit, on);
            for (const Mesh3::Face& f : m.faces) {
                const int n0 = f.nodes[0], n1 = f.nodes[1], n2 = f.nodes[2];
                View3D::Triangle a{{m.nodes[std::size_t(n0)], m.nodes[std::size_t(n1)], m.nodes[std::size_t(n2)]}, QColor(), f.face};
                a.shaded = true;
                a.colors = {colourIn(table, lo, hi, nodal[std::size_t(n0)]), colourIn(table, lo, hi, nodal[std::size_t(n1)]),
                            colourIn(table, lo, hi, nodal[std::size_t(n2)])};
                scene.triangles.push_back(a);
                if (f.quad) {
                    const int n3 = f.nodes[3];
                    View3D::Triangle b{{m.nodes[std::size_t(n0)], m.nodes[std::size_t(n2)], m.nodes[std::size_t(n3)]}, QColor(), f.face};
                    b.shaded = true;
                    b.colors = {colourIn(table, lo, hi, nodal[std::size_t(n0)]), colourIn(table, lo, hi, nodal[std::size_t(n2)]),
                                colourIn(table, lo, hi, nodal[std::size_t(n3)])};
                    scene.triangles.push_back(b);
                }
            }
        } else if (type == QLatin1String("slice")) {
            std::vector<SliceTriangle> cut;
            const QString plane = p.text(QStringLiteral("plane"));
            if (plane == QLatin1String("normal")) {
                auto triple = [&sol](const QString& text, Vec3 fallback) {
                    const QStringList parts = text.split(QLatin1Char(','));
                    if (parts.size() != 3) return fallback;
                    double c[3];
                    for (int i = 0; i < 3; ++i) {
                        const std::optional<double> v = evaluateConstant(parts[i].trimmed(), sol->parameters().scope, Dim());
                        if (!v) return fallback;
                        c[i] = *v;
                    }
                    return Vec3{c[0], c[1], c[2]};
                };
                cut = slicePlane(*sol, e, triple(p.text(QStringLiteral("origin")), Vec3{}), triple(p.text(QStringLiteral("normal")), Vec3{0, 0, 1}));
            } else {
                const int axis = plane == QLatin1String("x") ? 0 : plane == QLatin1String("y") ? 1 : 2;
                const double a0 = axis == 0 ? box.x0 : axis == 1 ? box.y0 : box.z0, a1 = axis == 0 ? box.x1 : axis == 1 ? box.y1 : box.z1;
                std::vector<double> at;
                if (p.text(QStringLiteral("entry")) == QLatin1String("values")) {
                    QString why;
                    at = numbersOf(p.text(QStringLiteral("at")), sol->parameters().scope, true, k, unit, &why);
                    if (!why.isEmpty()) a_plotProblems << tr("%1: %2").arg(p.name(), why);
                } else {
                    const int n = std::clamp(p.integer(QStringLiteral("count")), 1, 100);
                    for (int i = 0; i < n; ++i) at.push_back(a0 + (i + 0.5) * (a1 - a0) / n);
                }
                for (double c : at) {
                    const std::vector<SliceTriangle> one = slice(*sol, e, axis, c);
                    cut.insert(cut.end(), one.begin(), one.end());
                }
            }
            std::vector<double> on;
            for (const SliceTriangle& st : cut) on.insert(on.end(), st.value.begin(), st.value.end());
            std::tie(lo, hi) = rangeOf(p, unit, on);
            for (const SliceTriangle& st : cut) {
                View3D::Triangle c{st.p, QColor(), -1};
                c.shaded = true;
                c.colors = {colourIn(table, lo, hi, st.value[0]), colourIn(table, lo, hi, st.value[1]), colourIn(table, lo, hi, st.value[2])};
                scene.triangles.push_back(c);
            }
        } else {
            const std::vector<double> nodal = nodalValues(*sol, e);
            std::tie(lo, hi) = rangeOf(p, unit, nodal);
            std::vector<double> levels;
            if (p.text(QStringLiteral("entry")) == QLatin1String("values")) {
                QString why;
                levels = numbersOf(p.text(QStringLiteral("values")), sol->parameters().scope, false, k, unit, &why);
                if (!why.isEmpty()) a_plotProblems << tr("%1: %2").arg(p.name(), why);
            } else {
                const int n = std::clamp(p.integer(QStringLiteral("levels")), 1, 100);
                for (int i = 0; i < n; ++i) levels.push_back(lo + (i + 1) * (hi - lo) / (n + 1));
            }
            for (const IsoTriangle& it : isosurfaces(*sol, nodal, levels)) {
                const QColor colour = colourIn(table, lo, hi, levels[std::size_t(it.level)]);
                scene.triangles.push_back({it.p, colour, -1});
            }
        }
        if (!first) first = Coloured{e, what, unit, &table, lo, hi};
    }
    if (first) {
        const QString said = first->unit.name.isEmpty() ? unitName(first->e.dim()) : first->unit.name;
        legend.title = said.isEmpty() ? first->text : QStringLiteral("%1 (%2)").arg(first->text, said);
        legend.low = shown(first->lo, first->e.dim(), first->unit);
        legend.high = shown(first->hi, first->e.dim(), first->unit);
        legend.table = *first->table;
        legend.shown = true;
        a_plotRange = {first->lo, first->hi};
        // The clip's cut face, as the first coloured plot is.
        if (*clipAxis >= 0)
            for (const SliceTriangle& st : slice(*sol, first->e, *clipAxis, clipAt)) {
                View3D::Triangle c{st.p, QColor(), -1};
                c.shaded = true;
                c.colors = {colourIn(*first->table, first->lo, first->hi, st.value[0]), colourIn(*first->table, first->lo, first->hi, st.value[1]),
                            colourIn(*first->table, first->lo, first->hi, st.value[2])};
                scene.triangles.push_back(c);
            }
    }
    if (t) {
        const QColor edge = qucs_s::ink::isDark(palette().color(QPalette::Base)) ? QColor(220, 225, 232) : QColor(40, 44, 52);
        for (int e = 0; e < int(t->edges.size()); ++e) {
            const std::vector<Vec3> line = t->edgePolyline(e);
            for (std::size_t i = 0; i + 1 < line.size(); ++i) scene.lines.push_back({line[i], line[i + 1], edge, 1, e});
        }
        scene.bounds = t->bounds;
    }
    if (sol->selected() != before && before >= 0) sol->select(before);
}

void Multiphysics3DDoc::updateScene(bool fit)
{
    if (!a_view) return;
    // (The tab's Cut; a plot group's own clip below, where it has none.)
    if (a_clipBox) a_view->setClip(a_clipBox->currentData().toInt(), a_clipSlider->value() / 100.0);
    View3D::Scene scene;
    View3D::Legend legend;
    const Topology3* t = a_geometry.topology.get();
    const QColor text = palette().color(QPalette::Text);
    const bool dark = qucs_s::ink::isDark(palette().color(QPalette::Base));
    const QColor edgeColor = dark ? QColor(220, 225, 232) : QColor(40, 44, 52);
    const QColor wireColor = dark ? QColor(110, 116, 126) : QColor(176, 180, 188);
    const bool ghost = a_highlightLevel != Level::None;
    const QSet<int> lit(a_highlighted.begin(), a_highlighted.end());
    // What each face, edge and point of a topology is drawn as, by what its
    // parts are (Visibility): a face filled where a shown domain is on a side
    // of it (2), its edges alone where a framed one is (1), else not at all
    // (0); an edge where a face on it is drawn, a point where an edge is.
    struct Drawn {
        const Topology3* of = nullptr;
        std::vector<Visibility> domain;
        std::vector<char> face, edge, point;
        bool any = false;   ///< something hidden or framed
        bool none = false;  ///< every domain hidden
    } drawn;
    const bool hiding = anyHidden();
    auto drawnOf = [&](const Topology3* top) -> const Drawn& {
        if (drawn.of == top) return drawn;
        drawn = {};
        drawn.of = top;
        if (!top || !hiding) return drawn;
        drawn.domain = domainVisibilities(*top);
        drawn.any = std::any_of(drawn.domain.begin(), drawn.domain.end(), [](Visibility v) { return v != Visibility::Shown; });
        if (!drawn.any) return drawn;
        drawn.none = std::all_of(drawn.domain.begin(), drawn.domain.end(), [](Visibility v) { return v == Visibility::Hidden; });
        auto at = [&](int d) { return d >= 0 && d < int(drawn.domain.size()) ? drawn.domain[std::size_t(d)] : Visibility::Hidden; };
        drawn.face.resize(top->faces.size());
        for (std::size_t f = 0; f < top->faces.size(); ++f) {
            const Visibility a = at(top->faces[f].back), b = at(top->faces[f].front);
            drawn.face[f] = a == Visibility::Shown || b == Visibility::Shown ? 2 : a == Visibility::Frame || b == Visibility::Frame ? 1 : 0;
        }
        drawn.edge.resize(top->edges.size());
        for (std::size_t e = 0; e < top->edges.size(); ++e) {
            const std::vector<int>& on = top->edges[e].faces;
            drawn.edge[e] = on.empty() || std::any_of(on.begin(), on.end(), [&](int f) {
                return f >= 0 && f < int(drawn.face.size()) && drawn.face[std::size_t(f)] > 0;
            });
        }
        drawn.point.resize(top->points.size());
        for (std::size_t p = 0; p < top->points.size(); ++p) {
            const std::vector<int>& on = top->points[p].edges;
            drawn.point[p] = on.empty() || std::any_of(on.begin(), on.end(), [&](int e) {
                return e >= 0 && e < int(drawn.edge.size()) && drawn.edge[std::size_t(e)];
            });
        }
        return drawn;
    };
    // A face lit: one chosen (a boundary's), or one of a domain chosen that is
    // shown - not where that domain is hidden or framed, so the face of the
    // part shown beside it is not lit in its place (the check of 9 November,
    // 2: pmlT chosen and hidden lit the oxide's top under it).
    auto domainAs = [&](int domain) {
        const Drawn& d = drawnOf(t);
        return d.any && domain >= 0 && domain < int(d.domain.size()) ? d.domain[std::size_t(domain)] : Visibility::Shown;
    };
    auto faceLit = [&](int f) {
        if (!t || f < 0) return false;
        const Topology3::Face& face = t->faces[std::size_t(f)];
        if (a_highlightLevel == Level::Domain)
            return (lit.contains(face.back) && domainAs(face.back) == Visibility::Shown)
                || (lit.contains(face.front) && domainAs(face.front) == Visibility::Shown);
        return a_highlightLevel == Level::Boundary && lit.contains(f);
    };
    // A face of a domain chosen that is framed: its frame drawn lit instead.
    auto faceFramedLit = [&](int f) {
        if (!t || f < 0 || a_highlightLevel != Level::Domain) return false;
        const Topology3::Face& face = t->faces[std::size_t(f)];
        return (lit.contains(face.back) && domainAs(face.back) == Visibility::Frame)
            || (lit.contains(face.front) && domainAs(face.front) == Visibility::Frame);
    };
    auto edgeFramedLit = [&](int e) {
        if (!t || e < 0 || e >= int(t->edges.size()) || a_highlightLevel != Level::Domain) return false;
        const std::vector<int>& on = t->edges[std::size_t(e)].faces;
        return std::any_of(on.begin(), on.end(), faceFramedLit);
    };
    auto faceColor = [&](int f) {
        if (faceLit(f)) return Highlight;
        const Topology3::Face& face = t->faces[std::size_t(f)];
        // (Between a part shown and one hidden or framed: the shown one's.)
        const Drawn& d = drawnOf(t);
        if (d.any) {
            auto shown = [&d](int domain) { return domain >= 0 && domain < int(d.domain.size()) && d.domain[std::size_t(domain)] == Visibility::Shown; };
            if (shown(face.back) != shown(face.front)) return domainColor(shown(face.back) ? face.back : face.front);
        }
        return domainColor(std::max(face.back, face.front));
    };
    if (a_show == Show::Solution && a_solution) {
        // Its faces coloured by the expression at their nodes, and the cut's
        // slice of it where the view is cut.
        const Solution3& sol = *a_solution;
        const Mesh3& m = sol.mesh();
        t = m.topology.get();
        const QString text = plotExpression();
        const Expression e = Expression::compile(text, sol.scope());
        const std::vector<QRgb>& table = colorTable(QStringLiteral("rainbow"));
        std::vector<double> nodal(m.nodes.size(), std::numeric_limits<double>::quiet_NaN());
        std::vector<SliceTriangle> cut;
        if (e.isValid()) {
            if (a_plotCache.solution != a_solution || a_plotCache.snapshot != sol.selected() || a_plotCache.text != text)
                a_plotCache = {a_solution, sol.selected(), text, nodalValues(sol, e)};
            nodal = a_plotCache.nodal;
            const int axis = a_clipBox->currentData().toInt();
            if (axis >= 0 && t) {
                const Box3& b = t->bounds;
                const double lo = axis == 0 ? b.x0 : axis == 1 ? b.y0 : b.z0, hi = axis == 0 ? b.x1 : axis == 1 ? b.y1 : b.z1;
                cut = slice(sol, e, axis, lo + (hi - lo) * a_clipSlider->value() / 100.0);
            }
        }
        double lo = std::numeric_limits<double>::infinity(), hi = -lo;
        for (double v : nodal)
            if (std::isfinite(v)) {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        for (const SliceTriangle& st : cut)
            for (double v : st.value)
                if (std::isfinite(v)) {
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
        if (!(hi >= lo)) lo = hi = 0;
        a_plotRange = {lo, hi};
        const QColor none(190, 190, 190);
        auto colourOf = [&table, lo, hi, none](double v) {
            if (!std::isfinite(v)) return none;
            const double u = hi > lo ? (v - lo) / (hi - lo) : 0.5;
            return QColor::fromRgb(table[std::size_t(std::clamp(int(u * double(table.size() - 1) + 0.5), 0, int(table.size()) - 1))]);
        };
        for (const Mesh3::Face& f : m.faces) {
            const int n0 = f.nodes[0], n1 = f.nodes[1], n2 = f.nodes[2];
            View3D::Triangle a{{m.nodes[std::size_t(n0)], m.nodes[std::size_t(n1)], m.nodes[std::size_t(n2)]}, none, f.face};
            a.shaded = true;
            a.colors = {colourOf(nodal[std::size_t(n0)]), colourOf(nodal[std::size_t(n1)]), colourOf(nodal[std::size_t(n2)])};
            scene.triangles.push_back(a);
            if (f.quad) {
                const int n3 = f.nodes[3];
                View3D::Triangle b{{m.nodes[std::size_t(n0)], m.nodes[std::size_t(n2)], m.nodes[std::size_t(n3)]}, none, f.face};
                b.shaded = true;
                b.colors = {colourOf(nodal[std::size_t(n0)]), colourOf(nodal[std::size_t(n2)]), colourOf(nodal[std::size_t(n3)])};
                scene.triangles.push_back(b);
            }
        }
        for (const SliceTriangle& st : cut) {
            View3D::Triangle c{st.p, none, -1};
            c.shaded = true;
            c.colors = {colourOf(st.value[0]), colourOf(st.value[1]), colourOf(st.value[2])};
            scene.triangles.push_back(c);
        }
        if (t) {
            for (int edge = 0; edge < int(t->edges.size()); ++edge) {
                const std::vector<Vec3> line = t->edgePolyline(edge);
                for (std::size_t i = 0; i + 1 < line.size(); ++i) scene.lines.push_back({line[i], line[i + 1], edgeColor, 1, edge});
            }
            scene.bounds = t->bounds;
        }
        if (e.isValid()) {
            const QString unit = unitName(e.dim()).isEmpty() ? (e.dim().isNone() ? QString() : dimName(e.dim())) : unitName(e.dim());
            legend.title = unit.isEmpty() ? text : QStringLiteral("%1 (%2)").arg(text, unit);
            legend.low = formatQuantity(lo, e.dim());
            legend.high = formatQuantity(hi, e.dim());
            legend.table = table;
            legend.shown = true;
        }
    } else if (a_show == Show::Plot && a_solution && a_model.find(a_plotGroup)) {
        int axis = -1;
        double share = 0.5;
        plotGroupScene(*a_model.find(a_plotGroup), *a_solution, scene, legend, &axis, &share);
        t = a_solution->mesh().topology.get();
        // (The group's own clip, where the tab's Cut is none.)
        if (a_clipBox->currentData().toInt() < 0) a_view->setClip(axis, share);
    } else if (a_show == Show::Mesh && a_mesh && !a_meshStale && a_mesh->topology) {
        const Mesh3& m = *a_mesh;
        t = m.topology.get();
        const Drawn& d = drawnOf(t);
        for (const Mesh3::Face& f : m.faces) {
            const bool on = !ghost || faceLit(f.face);
            if (!on && !faceFramedLit(f.face)) continue;
            // (A part framed: its faces' mesh lines, not their fill - and a
            // framed part chosen, its lines alone.)
            int as = d.any && f.face >= 0 && f.face < int(d.face.size()) ? d.face[std::size_t(f.face)] : 2;
            if (!on) as = std::min(as, 1);
            if (as == 0) continue;
            const QColor color = faceColor(f.face);
            const Vec3 a = m.nodes[std::size_t(f.nodes[0])], b = m.nodes[std::size_t(f.nodes[1])], c = m.nodes[std::size_t(f.nodes[2])];
            if (as == 2) {
                scene.triangles.push_back({{a, b, c}, color, f.face});
                if (f.quad) scene.triangles.push_back({{a, c, m.nodes[std::size_t(f.nodes[3])]}, color, f.face});
            }
            const int n = f.quad ? 4 : 3;
            for (int i = 0; i < n; ++i)
                scene.lines.push_back({m.nodes[std::size_t(f.nodes[std::size_t(i)])], m.nodes[std::size_t(f.nodes[std::size_t((i + 1) % n)])], edgeColor, 1, -1});
        }
        if (ghost)
            for (int e = 0; e < int(t->edges.size()); ++e) {
                const bool framed = edgeFramedLit(e);
                const std::vector<Vec3> line = t->edgePolyline(e);
                for (std::size_t i = 0; i + 1 < line.size(); ++i)
                    scene.lines.push_back({line[i], line[i + 1], framed ? Highlight : wireColor, framed ? 2.0f : 1.0f, e});
            }
        scene.bounds = t->bounds;
    } else if (t && !t->domains.empty()) {
        for (int f = 0; f < int(t->faces.size()); ++f) {
            if (ghost && !faceLit(f)) continue;
            const QColor color = faceColor(f);
            for (const std::array<Vec3, 3>& tri : t->faceTriangles(f)) scene.triangles.push_back({tri, color, f});
        }
        for (int e = 0; e < int(t->edges.size()); ++e) {
            const bool on = a_highlightLevel == Level::Edge && lit.contains(e);
            const bool framed = !on && edgeFramedLit(e);
            const QColor color = on || framed ? Highlight : ghost ? wireColor : edgeColor;
            const std::vector<Vec3> line = t->edgePolyline(e);
            for (std::size_t i = 0; i + 1 < line.size(); ++i)
                scene.lines.push_back({line[i], line[i + 1], color, on ? 3.0f : framed ? 2.0f : 1.0f, e});
        }
        for (int p = 0; p < int(t->points.size()); ++p) {
            const bool on = a_highlightLevel == Level::Point && lit.contains(p);
            if (on) scene.dots.push_back({t->points[std::size_t(p)].position, Highlight, 4, p});
            else if (!ghost) scene.dots.push_back({t->points[std::size_t(p)].position, edgeColor, 1.5f, p});
        }
        scene.bounds = t->bounds;
    }
    // The parts hidden or framed: their faces (each triangle's id a face's),
    // edges and points left out where nothing drawn is on them - in each
    // view, the solution's and a plot group's too; a cut's slice kept.
    const Drawn& d = drawnOf(t);
    if (d.any) {
        auto& tris = scene.triangles;
        tris.erase(std::remove_if(tris.begin(), tris.end(),
                                  [&d](const View3D::Triangle& x) { return x.id >= 0 && x.id < int(d.face.size()) && d.face[std::size_t(x.id)] < 2; }),
                   tris.end());
        auto& lines = scene.lines;
        lines.erase(std::remove_if(lines.begin(), lines.end(),
                                   [&d](const View3D::Line& x) { return x.id >= 0 && x.id < int(d.edge.size()) && !d.edge[std::size_t(x.id)]; }),
                    lines.end());
        auto& dots = scene.dots;
        dots.erase(std::remove_if(dots.begin(), dots.end(),
                                  [&d](const View3D::Dot& x) { return x.id >= 0 && x.id < int(d.point.size()) && !d.point[std::size_t(x.id)]; }),
                   dots.end());
    }
    Q_UNUSED(text);
    a_view->setEmptyText(d.none ? tr("Every part is hidden: Visibility > Show All draws them again.")
                         : t && !t->domains.empty() ? QString()
                         : a_geometryStale && a_job ? tr("Building the geometry…")
                                                    : tr("No solids yet: add a Box, a Cylinder, an Extrude of a sketch or a Layer Stack to "
                                                         "the Geometry (the Multiphysics 3D panel's tree)."));
    a_view->setLegend(std::move(legend));
    a_view->setScene(std::move(scene), fit);
}
