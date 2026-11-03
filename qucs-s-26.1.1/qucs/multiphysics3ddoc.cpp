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

#include "fem3_results.h"
#include "ink.h"
#include "misc.h"
#include "multiphysicsview.h"
#include "qucs.h"

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPrinter>
#include <QProgressBar>
#include <QSaveFile>
#include <QSlider>
#include <QSplitter>
#include <QTabWidget>
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

using namespace qucs_s::fem;

namespace {

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
};

Parts partsOf(const Model& m)
{
    const QJsonObject o = m.toObject();
    const QJsonObject c = o.value(QStringLiteral("components")).toArray().at(0).toObject();
    Parts p;
    p.geometry = compact(o.value(QStringLiteral("parameters"))) + compact(o.value(QStringLiteral("functions")))
                 + compact(c.value(QStringLiteral("unit"))) + compact(c.value(QStringLiteral("geometry")));
    p.mesh = compact(c.value(QStringLiteral("mesh"))) + compact(c.value(QStringLiteral("selections")));
    return p;
}

const QColor Highlight(31, 111, 235);

} // namespace

struct Multiphysics3DDoc::Job {
    bool mesh = false;                       ///< a mesh asked (its geometry first, when none given)
    bool solve = false;                      ///< a solve asked (its mesh first, when none given)
    std::shared_ptr<const Mesh3> givenMesh;  ///< the mesh a solve is on (none: made by it)
    StudyResult3 study;
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
    if (a_job && a_job->thread) {
        a_job->cancel = true;
        a_job->thread->wait();
        delete a_job->thread;
        a_job->thread = nullptr;
    }
}

void Multiphysics3DDoc::buildUi()
{
    auto* all = new QVBoxLayout(this);
    all->setContentsMargins(0, 0, 0, 0);
    all->setSpacing(0);
    auto* bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("multiphysics3dToolbar"));
    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(6, 3, 6, 3);
    row->setSpacing(4);
    auto button = [&](const QString& text, const QString& tip, const char* name) {
        auto* b = new QToolButton(bar);
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
    connect(a_compute, &QToolButton::clicked, this, &Multiphysics3DDoc::compute);
    row->addSpacing(8);
    row->addWidget(new QLabel(tr("Show:"), bar));
    a_showBox = new QComboBox(bar);
    a_showBox->setObjectName(QStringLiteral("mp3Show"));
    a_showBox->addItems({tr("Geometry"), tr("Mesh"), tr("Solution")});
    row->addWidget(a_showBox);
    connect(a_showBox, &QComboBox::activated, this, [this](int index) {
        if (index == 2) showSolution();
        else if (index == 1) showMesh();
        else showGeometry();
    });
    row->addWidget(new QLabel(tr("Plot:"), bar));
    a_plotBox = new QComboBox(bar);
    a_plotBox->setObjectName(QStringLiteral("mp3Plot"));
    a_plotBox->setEditable(true);
    a_plotBox->setMinimumContentsLength(10);
    a_plotBox->setToolTip(tr("What the solution is coloured by: a variable (ht.T, es.normE, ec.Qrh...) or an expression of them"));
    a_plotBox->setEnabled(false);
    row->addWidget(a_plotBox);
    connect(a_plotBox, &QComboBox::activated, this, [this](int) { setPlotExpression(a_plotBox->currentText()); });
    connect(a_plotBox->lineEdit(), &QLineEdit::editingFinished, this, [this] { setPlotExpression(a_plotBox->currentText()); });
    row->addSpacing(8);
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
    row->addWidget(new QLabel(tr("Cut:"), bar));
    a_clipBox = new QComboBox(bar);
    a_clipBox->setObjectName(QStringLiteral("mp3Clip"));
    a_clipBox->addItem(tr("None"), -1);
    a_clipBox->addItem(QStringLiteral("x"), 0);
    a_clipBox->addItem(QStringLiteral("y"), 1);
    a_clipBox->addItem(QStringLiteral("z"), 2);
    a_clipBox->setToolTip(tr("Cut away what lies beyond a plane across x, y or z, to see inside"));
    row->addWidget(a_clipBox);
    a_clipSlider = new QSlider(Qt::Horizontal, bar);
    a_clipSlider->setObjectName(QStringLiteral("mp3ClipAt"));
    a_clipSlider->setRange(0, 100);
    a_clipSlider->setValue(50);
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
    row->addStretch(1);
    a_stage = new QLabel(bar);
    a_stage->setObjectName(QStringLiteral("mp3Stage"));
    row->addWidget(a_stage);
    a_progress = new QProgressBar(bar);
    a_progress->setObjectName(QStringLiteral("mp3Progress"));
    a_progress->setRange(0, 1000);
    a_progress->setTextVisible(false);
    a_progress->setMaximumWidth(140);
    a_progress->setMaximumHeight(14);
    row->addWidget(a_progress);
    a_cancel = button(tr("Cancel"), tr("Stop the work going on"), "mp3Cancel");
    connect(a_cancel, &QToolButton::clicked, this, &Multiphysics3DDoc::cancel);
    a_stage->hide();
    a_progress->hide();
    a_cancel->hide();
    all->addWidget(bar);

    auto* split = new QSplitter(Qt::Vertical, this);
    split->setObjectName(QStringLiteral("mp3Splitter"));
    a_view = new View3D(split);
    a_notes = new QPlainTextEdit(split);
    a_notes->setObjectName(QStringLiteral("mp3Messages"));
    a_notes->setReadOnly(true);
    a_notes->setMaximumBlockCount(500);
    split->addWidget(a_view);
    split->addWidget(a_notes);
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
    if (a_solution) a_solutionStale = true;
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
        a_plotCache = {};
        if (a_show == Show::Solution) a_show = Show::Geometry;
    }
    if (!a_focus.isEmpty() && !a_model.find(a_focus)) a_focus.clear();
    updateHighlight();
    updateScene();
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

void Multiphysics3DDoc::compute()
{
    if (a_closed) return;
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
    a_geometryWanted = false;
    a_meshWanted = false;
    a_solveWanted = false;
    if (a_job) a_job->cancel = true;
}

bool Multiphysics3DDoc::waitForIdle(int ms)
{
    QElapsedTimer t;
    t.start();
    while ((a_job || a_geometryTimer->isActive() || a_geometryWanted || a_meshWanted || a_solveWanted) && t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    return !a_job && !a_geometryTimer->isActive() && !a_geometryWanted && !a_meshWanted && !a_solveWanted;
}

void Multiphysics3DDoc::startJob(bool mesh, bool solve)
{
    auto job = std::make_shared<Job>();
    job->mesh = mesh;
    job->solve = solve;
    job->model = a_model;
    job->parameters = parameters();
    job->revision = a_revision;
    if (!a_geometryStale && a_geometry.ok()) job->topology = a_geometry.topology;
    if (solve && job->topology && a_mesh && !a_meshStale) job->givenMesh = a_mesh;
    a_job = job;
    a_stage->setText(solve ? tr("Solving") : mesh ? tr("Building the mesh") : tr("Building the geometry"));
    a_progress->setValue(0);
    a_stage->show();
    a_progress->show();
    a_cancel->show();
    job->thread = QThread::create([job] {
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
        }
        if (job->solve)
            job->study = solveStudy3(job->model, job->parameters, job->givenMesh, QString(), [job, meshShare](double fraction, const QString&) {
                job->permille = int(1000 * (meshShare + (1 - meshShare) * fraction));
                return !job->cancel.load();
            });
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
    const bool current = job->revision == a_revision;
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
                if (!job->solve) a_show = Show::Mesh;
            } else if (!job->built || job->geometry.ok()) {
                say(tr("Mesh: %1").arg(job->meshBuild.error.isEmpty() ? tr("not made") : job->meshBuild.error), true);
            }
        }
        if (job->solve) {
            if (job->built && !job->geometry.ok()) say(tr("Not solved: the geometry is not built"), true);
            else if (!job->givenMesh) {
                if (job->meshBuild.error.isEmpty() && !job->meshBuild.mesh) say(tr("Not solved: there is no mesh"), true);
            } else sayFinished(job->study, job->started.elapsed());
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
    a_showBox->setCurrentIndex(a_show == Show::Solution ? 2 : a_show == Show::Mesh ? 1 : 0);
    a_plotBox->setEnabled(a_solution != nullptr);
    // (A solution shown as it was before an edit: said beside the bar.)
    if (!a_job) {
        const bool stale = a_show == Show::Solution && a_solution && a_solutionStale;
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

void Multiphysics3DDoc::sayFinished(const StudyResult3& r, qint64 ms)
{
    for (const QString& line : r.log) say(line);
    if (!r.solution) {
        say(tr("Not solved: %1").arg(r.error), true);
        return;
    }
    a_solution = r.solution;
    a_solutionStale = false;
    for (const Variable& g : a_solution->globals())
        say(tr("%1 = %2%3").arg(g.name, a_solution->globalText(g.name),
                                g.description.isEmpty() ? QString() : QStringLiteral(" (") + g.description + QLatin1Char(')')));
    say(tr("Shown: %1 (%2 ms in all)").arg(plotExpression()).arg(ms));
    fillPlotChoices();
    a_show = Show::Solution;
}

void Multiphysics3DDoc::focusNode(const QString& tag)
{
    if (tag == a_focus) return;
    a_focus = tag;
    updateHighlight();
    updateScene();
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
        for (int d = 0; d < int(t->domains.size()); ++d)
            if (t->domains[std::size_t(d)].objects.intersects(solids)) a_highlighted << d;
        a_highlightLevel = Level::Domain;
    }
}

void Multiphysics3DDoc::updateScene(bool fit)
{
    if (!a_view) return;
    View3D::Scene scene;
    View3D::Legend legend;
    const Topology3* t = a_geometry.topology.get();
    const QColor text = palette().color(QPalette::Text);
    const bool dark = qucs_s::ink::isDark(palette().color(QPalette::Base));
    const QColor edgeColor = dark ? QColor(220, 225, 232) : QColor(40, 44, 52);
    const QColor wireColor = dark ? QColor(110, 116, 126) : QColor(176, 180, 188);
    const bool ghost = a_highlightLevel != Level::None;
    const QSet<int> lit(a_highlighted.begin(), a_highlighted.end());
    auto faceLit = [&](int f) {
        if (!t || f < 0) return false;
        const Topology3::Face& face = t->faces[std::size_t(f)];
        if (a_highlightLevel == Level::Domain) return lit.contains(face.back) || lit.contains(face.front);
        return a_highlightLevel == Level::Boundary && lit.contains(f);
    };
    auto faceColor = [&](int f) {
        if (faceLit(f)) return Highlight;
        const Topology3::Face& face = t->faces[std::size_t(f)];
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
            if (a_plotCache.solution != a_solution || a_plotCache.text != text) a_plotCache = {a_solution, text, nodalValues(sol, e)};
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
    } else if (a_show == Show::Mesh && a_mesh && !a_meshStale && a_mesh->topology) {
        const Mesh3& m = *a_mesh;
        t = m.topology.get();
        for (const Mesh3::Face& f : m.faces) {
            if (ghost && !faceLit(f.face)) continue;
            const QColor color = faceColor(f.face);
            const Vec3 a = m.nodes[std::size_t(f.nodes[0])], b = m.nodes[std::size_t(f.nodes[1])], c = m.nodes[std::size_t(f.nodes[2])];
            scene.triangles.push_back({{a, b, c}, color, f.face});
            if (f.quad) scene.triangles.push_back({{a, c, m.nodes[std::size_t(f.nodes[3])]}, color, f.face});
            const int n = f.quad ? 4 : 3;
            for (int i = 0; i < n; ++i)
                scene.lines.push_back({m.nodes[std::size_t(f.nodes[std::size_t(i)])], m.nodes[std::size_t(f.nodes[std::size_t((i + 1) % n)])], edgeColor, 1, -1});
        }
        if (ghost)
            for (int e = 0; e < int(t->edges.size()); ++e) {
                const std::vector<Vec3> line = t->edgePolyline(e);
                for (std::size_t i = 0; i + 1 < line.size(); ++i) scene.lines.push_back({line[i], line[i + 1], wireColor, 1, e});
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
            const QColor color = on ? Highlight : ghost ? wireColor : edgeColor;
            const std::vector<Vec3> line = t->edgePolyline(e);
            for (std::size_t i = 0; i + 1 < line.size(); ++i) scene.lines.push_back({line[i], line[i + 1], color, on ? 3.0f : 1.0f, e});
        }
        for (int p = 0; p < int(t->points.size()); ++p) {
            const bool on = a_highlightLevel == Level::Point && lit.contains(p);
            if (on) scene.dots.push_back({t->points[std::size_t(p)].position, Highlight, 4, p});
            else if (!ghost) scene.dots.push_back({t->points[std::size_t(p)].position, edgeColor, 1.5f, p});
        }
        scene.bounds = t->bounds;
    }
    Q_UNUSED(text);
    a_view->setEmptyText(t && !t->domains.empty() ? QString()
                         : a_geometryStale && a_job ? tr("Building the geometry…")
                                                    : tr("No solids yet: add a Box, a Cylinder, an Extrude of a sketch or a Layer Stack to "
                                                         "the Geometry (the Multiphysics 3D panel's tree)."));
    a_view->setLegend(std::move(legend));
    a_view->setScene(std::move(scene), fit);
}
