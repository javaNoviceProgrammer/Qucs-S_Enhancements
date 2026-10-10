/*
 * multiphysicsdoc.cpp - a multiphysics model in a tab of Qucs-S
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "multiphysicsdoc.h"

#include "dataimport.h"
#include "ink.h"
#include "misc.h"
#include "qucs.h"

#include <QComboBox>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPrinter>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QUndoCommand>
#include <QUndoStack>
#include <QVBoxLayout>

#include <mutex>

using namespace qucs_s::fem;

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

QByteArray compact(const QJsonValue& v)
{
    if (v.isObject()) return QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact);
    if (v.isArray()) return QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact);
    return v.toVariant().toString().toUtf8();
}

struct Sections {
    QByteArray parameters, geometry, mesh, physics, studies, results;
};

Sections sections(const Model& m)
{
    const QJsonObject o = m.toObject();
    const QJsonObject c = o.value(QStringLiteral("components")).toArray().first().toObject();
    Sections s;
    s.parameters = compact(o.value(QStringLiteral("parameters"))) + compact(o.value(QStringLiteral("functions")));
    s.geometry = compact(c.value(QStringLiteral("geometry"))) + compact(c.value(QStringLiteral("unit")));
    s.mesh = compact(c.value(QStringLiteral("mesh"))) + compact(c.value(QStringLiteral("selections")));
    s.physics = compact(c.value(QStringLiteral("materials"))) + compact(c.value(QStringLiteral("physics")))
                + compact(c.value(QStringLiteral("thickness"))) + compact(c.value(QStringLiteral("selections")));
    s.studies = compact(o.value(QStringLiteral("studies")));
    s.results = compact(o.value(QStringLiteral("results")));
    return s;
}

QColor colorOf(const QString& text, const QColor& fallback)
{
    const QColor c = QColor::fromString(text.trimmed());
    return c.isValid() ? c : fallback;
}

} // namespace

struct MultiphysicsDoc::Job {
    QString what;
    std::atomic<bool> cancel{false};
    std::atomic<double> fraction{0};
    std::mutex mutex;
    QString stage;
    QThread* thread = nullptr;
    Model model;
    std::shared_ptr<ParameterScope> parameters;
    std::shared_ptr<const Topology> topology;
    std::shared_ptr<const Mesh> mesh;
    bool meshIt = false;
    QString study;
    StudyRun run;
    QString error;
    QStringList warnings;
    std::function<void(Job&)> done;

    Progress progress(double from, double to)
    {
        return [this, from, to](double f, const QString& what) {
            fraction = from + (to - from) * std::clamp(f, 0.0, 1.0);
            {
                std::lock_guard<std::mutex> lock(mutex);
                stage = what;
            }
            return !cancel.load();
        };
    }
};

MultiphysicsDoc::MultiphysicsDoc(QucsApp* app, const QString& name) : QFrame(), QucsDoc(app, name)
{
    setObjectName(QStringLiteral("MultiphysicsDoc"));
    a_model = starterModel();
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
    a_geometryTimer->setInterval(120);
    connect(a_geometryTimer, &QTimer::timeout, this, [this] {
        if (a_geometryStale && !isBusy()) {
            QString why;
            buildGeometry(&why);
            if (a_view->show() != GraphicsView::Show::Results || a_plotGroup.isEmpty()) showGeometry();
            updateHighlight();
        }
    });
    // A drawing an Import reads, changed: the geometry made again.
    a_watcher = new QFileSystemWatcher(this);
    connect(a_watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString& path) {
        if (QFileInfo::exists(path) && !a_watcher->files().contains(path)) a_watcher->addPath(path);   // (written anew)
        say(tr("%1 changed: the geometry is built again").arg(QFileInfo(path).fileName()));
        a_geometryStale = true;
        a_meshStale = true;
        for (auto& [tag, stale] : a_stale) stale = true;
        a_geometryTimer->start();
        updateState();
        emit stateChanged();
    });
    buildUi();
    if (app != nullptr) connect(this, SIGNAL(signalFileChanged(bool)), app, SLOT(slotFileChanged(bool)));
}

MultiphysicsDoc::~MultiphysicsDoc()
{
    // Its undo stack goes with it, emitting its clean and can-undo changes:
    // not to this, half gone (a cast UBSan calls undefined), nor to the
    // window, which may be going too.
    disconnect(a_undo, nullptr, this, nullptr);
    if (a_job && a_job->thread) {
        a_job->cancel = true;
        a_job->thread->wait();
        delete a_job->thread;
    }
}

Model MultiphysicsDoc::starterModel()
{
    Model m = Model::blank();
    return m;
}

void MultiphysicsDoc::buildUi()
{
    auto* all = new QVBoxLayout(this);
    all->setContentsMargins(0, 0, 0, 0);
    all->setSpacing(0);
    auto* bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("multiphysicsToolbar"));
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
    a_buildGeometry = button(tr("Build Geometry"), tr("Build the geometry's features and make them one (Form Union)"), "mpBuildGeometry");
    a_buildMesh = button(tr("Build Mesh"), tr("Mesh the geometry as the Mesh node says"), "mpBuildMesh");
    a_compute = button(tr("Compute"), tr("Solve the first study: its geometry and mesh first, when they changed (F2)"), "mpCompute");
    connect(a_buildGeometry, &QToolButton::clicked, this, [this] {
        QString why;
        if (buildGeometry(&why)) showGeometry();
    });
    connect(a_buildMesh, &QToolButton::clicked, this, &MultiphysicsDoc::buildMesh);
    connect(a_compute, &QToolButton::clicked, this, [this] { compute(); });
    row->addSpacing(10);
    row->addWidget(new QLabel(tr("Show:"), bar));
    a_showBox = new QComboBox(bar);
    a_showBox->setObjectName(QStringLiteral("mpShow"));
    a_showBox->addItems({tr("Geometry"), tr("Mesh"), tr("Results")});
    row->addWidget(a_showBox);
    a_plotBox = new QComboBox(bar);
    a_plotBox->setObjectName(QStringLiteral("mpPlotGroup"));
    a_plotBox->setMinimumContentsLength(14);
    a_plotBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    row->addWidget(a_plotBox);
    connect(a_showBox, &QComboBox::activated, this, [this](int index) {
        if (index == 0) showGeometry();
        else if (index == 1) showMesh();
        else if (a_plotBox->count() > 0) {
            QString why;
            if (!showPlotGroup(a_plotBox->currentData().toString(), &why)) say(why, true);
        } else {
            say(tr("No plot group yet: Compute a study first."), true);
            showGeometry();
        }
    });
    connect(a_plotBox, &QComboBox::activated, this, [this](int) {
        QString why;
        if (!showPlotGroup(a_plotBox->currentData().toString(), &why)) say(why, true);
    });
    // The time and parameter value the plot group shows (a study in time,
    // a sweep): its settings changed, a step of Undo.
    a_instanceBox = new QComboBox(bar);
    a_instanceBox->setObjectName(QStringLiteral("mpInstance"));
    a_instanceBox->setMinimumContentsLength(12);
    a_instanceBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    a_instanceBox->setToolTip(tr("The output time and parameter value the plot group shows"));
    a_instanceBox->hide();
    row->addWidget(a_instanceBox);
    connect(a_instanceBox, &QComboBox::activated, this, [this](int index) {
        const QStringList v = a_instanceBox->itemData(index).toStringList();
        const QString group = a_plotGroup;
        if (group.isEmpty() || v.size() < 2) return;
        editNode(group, [v](Node& n) {
            n.props.insert(QStringLiteral("point"), v[0].toInt());
            n.props.insert(QStringLiteral("time"), v[1]);
        }, tr("Show %1").arg(a_instanceBox->itemText(index)));
    });
    row->addSpacing(10);
    a_fit = button(tr("Zoom Extents"), tr("Show it all (F)"), "mpFit");
    connect(a_fit, &QToolButton::clicked, this, [this] { a_view->fit(); });
    a_image = button(tr("Image…"), tr("Save the Graphics view as a PNG image"), "mpImage");
    connect(a_image, &QToolButton::clicked, this, [this] {
        const QString base = getDocName().isEmpty() ? QStringLiteral("model.png") : QFileInfo(getDocName()).completeBaseName() + QStringLiteral(".png");
        const QString path = QFileDialog::getSaveFileName(this, tr("Save Image"), QFileInfo(getDocName()).absoluteDir().filePath(base),
                                                          tr("PNG images (*.png)"));
        if (path.isEmpty()) return;
        QString why;
        if (!exportImage(path, &why)) QMessageBox::warning(this, tr("Save Image"), why);
    });
    row->addStretch(1);
    a_progress = new QProgressBar(bar);
    a_progress->setObjectName(QStringLiteral("mpProgress"));
    a_progress->setRange(0, 1000);
    a_progress->setMaximumWidth(200);
    a_progress->setTextVisible(true);
    a_progress->hide();
    row->addWidget(a_progress);
    a_cancel = new QPushButton(tr("Cancel"), bar);
    a_cancel->setObjectName(QStringLiteral("mpCancel"));
    a_cancel->hide();
    connect(a_cancel, &QPushButton::clicked, this, &MultiphysicsDoc::cancelJob);
    row->addWidget(a_cancel);
    all->addWidget(bar);

    a_banner = new QLabel(this);
    a_banner->setObjectName(QStringLiteral("mpBanner"));
    a_banner->setWordWrap(true);
    a_banner->setContentsMargins(8, 3, 8, 3);
    a_banner->setStyleSheet(QStringLiteral("QLabel { background: rgba(230, 160, 20, 60); }"));
    a_banner->hide();
    all->addWidget(a_banner);

    a_splitter = new QSplitter(Qt::Vertical, this);
    a_splitter->setObjectName(QStringLiteral("mpSplitter"));
    a_view = new GraphicsView(a_splitter);
    connect(a_view, &GraphicsView::pointerMoved, this, &MultiphysicsDoc::pointerMoved);
    connect(a_view, &GraphicsView::picked, this, &MultiphysicsDoc::picked);
    a_bottom = new QTabWidget(a_splitter);
    a_bottom->setObjectName(QStringLiteral("mpBottom"));
    a_bottom->setDocumentMode(true);
    a_log = new QPlainTextEdit(a_bottom);
    a_log->setObjectName(QStringLiteral("mpMessages"));
    a_log->setReadOnly(true);
    a_log->setMaximumBlockCount(2000);
    a_bottom->addTab(a_log, tr("Messages"));
    a_table = new QTableWidget(0, 3, a_bottom);
    a_table->setObjectName(QStringLiteral("mpTable"));
    a_table->setHorizontalHeaderLabels({tr("Derived value"), tr("Expression"), tr("Value")});
    a_table->horizontalHeader()->setStretchLastSection(true);
    a_table->verticalHeader()->hide();
    a_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    a_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    a_bottom->addTab(a_table, tr("Table"));
    a_splitter->addWidget(a_view);
    a_splitter->addWidget(a_bottom);
    a_splitter->setStretchFactor(0, 5);
    a_splitter->setStretchFactor(1, 1);
    a_splitter->setSizes({500, 140});
    all->addWidget(a_splitter, 1);
    a_status = new QLabel(this);
    a_status->setObjectName(QStringLiteral("mpStatus"));
    a_status->setContentsMargins(8, 2, 8, 2);
    a_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    all->addWidget(a_status);

    a_progressTimer = new QTimer(this);
    a_progressTimer->setInterval(100);
    connect(a_progressTimer, &QTimer::timeout, this, [this] {
        if (!a_job) return;
        a_progress->setValue(int(a_job->fraction * 1000));
        std::lock_guard<std::mutex> lock(a_job->mutex);
        a_progress->setFormat(a_job->stage.isEmpty() ? a_job->what : a_job->stage + QStringLiteral(" %p%"));
    });
    updateState();
}

void MultiphysicsDoc::changeEvent(QEvent* event)
{
    QFrame::changeEvent(event);
    if (event->type() == QEvent::PaletteChange) a_view->update();
}

// ------------------------------------------------------------------ QucsDoc

void MultiphysicsDoc::setName(const QString& name)
{
    a_DocName = name;
    a_model.fileName = name;
}

bool MultiphysicsDoc::load()
{
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
    if (!why.isEmpty()) say(tr("Read with notes: %1").arg(why));
    m->fileName = a_DocName;
    a_undo->clear();
    applyModel(*m);
    a_undo->setClean();
    setDocChanged(false);
    a_lastSaved = QFileInfo(a_DocName).lastModified();
    QString error;
    buildGeometry(&error);
    showGeometry();
    a_view->fit();
    say(tr("Opened %1").arg(QFileInfo(a_DocName).fileName()));
    return true;
}

bool MultiphysicsDoc::reload()
{
    QFile file(a_DocName);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QString why;
    std::optional<Model> m = Model::fromJson(file.readAll(), &why);
    if (!m) return false;
    m->fileName = a_DocName;
    const QString focus = a_focus;
    a_undo->clear();   // (its steps were of the model as it was)
    applyModel(*m);
    setDocChanged(false);
    emit signalFileChanged(false);
    a_lastSaved = QFileInfo(a_DocName).lastModified();
    if (!focus.isEmpty() && a_model.find(focus)) focusNode(focus);
    say(tr("Read again: %1 changed on disk").arg(QFileInfo(a_DocName).fileName()));
    return true;
}

bool MultiphysicsDoc::writeTo(const QString& path)
{
    const QByteArray bytes = a_model.toJson();
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

int MultiphysicsDoc::save()
{
    if (!writeTo(a_DocName)) {
        misc::reportError(tr("Cannot write %1").arg(a_DocName));
        return -1;
    }
    a_model.fileName = a_DocName;
    a_undo->setClean();
    setDocChanged(false);
    emit signalFileChanged(false);
    a_lastSaved = QDateTime::currentDateTime();
    return 0;
}

void MultiphysicsDoc::print(QPrinter*, QPainter* painter, bool, bool)
{
    const QImage image = a_view->picture();
    const QRect page = painter->viewport();
    const QSize fitted = image.size().scaled(page.size(), Qt::KeepAspectRatio);
    painter->drawImage(QRect(page.topLeft(), fitted), image);
}

void MultiphysicsDoc::becomeCurrent(bool)
{
    if (a_App != nullptr) {
        a_App->undo->setEnabled(a_undo->canUndo());
        a_App->redo->setEnabled(a_undo->canRedo());
    }
}

double MultiphysicsDoc::zoomBy(double factor)
{
    a_view->zoomBy(factor);
    return a_view->scale();
}

void MultiphysicsDoc::showAll()
{
    a_view->fit();
}

void MultiphysicsDoc::zoomToSelection()
{
    a_view->fit();
}

void MultiphysicsDoc::showNoZoom()
{
    a_view->fit();
}

void MultiphysicsDoc::undo()
{
    a_undo->undo();
}

void MultiphysicsDoc::redo()
{
    a_undo->redo();
}

// ------------------------------------------------------------------ The model

bool MultiphysicsDoc::setModel(const Model& model, const QString& what)
{
    if (model.toObject() == a_model.toObject()) return false;
    const Model before = a_model;
    applyModel(model);
    QPointer<MultiphysicsDoc> self = this;
    a_undo->push(new ModelCommand([self](const Model& m) {
        if (self) self->applyModel(m);
    }, before, model, what));
    return true;
}

bool MultiphysicsDoc::editNode(const QString& tag, const std::function<void(Node&)>& change, const QString& what, QString* error)
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

void MultiphysicsDoc::applyModel(const Model& model)
{
    const Model before = a_model;
    a_model = model;
    a_model.fileName = a_DocName;
    a_paramsStale = true;
    a_view->setAxisymmetric(a_model.axisymmetric());
    invalidate(before, a_model);
    edited();
    fillPlotBox();
    emit modelChanged();
    updateState();
    updateHighlight();
}

void MultiphysicsDoc::invalidate(const Model& before, const Model& after)
{
    const Sections a = sections(before), b = sections(after);
    const bool geometry = a.parameters != b.parameters || a.geometry != b.geometry;
    const bool mesh = geometry || a.mesh != b.mesh;
    const bool physics = mesh || a.physics != b.physics || a.studies != b.studies;
    if (geometry && !a_geometryStale) {
        a_geometryStale = true;
        a_geometryTimer->start();
    } else if (geometry) {
        a_geometryTimer->start();
    }
    if (mesh) a_meshStale = true;
    if (physics)
        for (auto& [tag, stale] : a_stale) stale = true;
    // A plot changed: drawn again.
    if (a.results != b.results && !a_plotGroup.isEmpty() && a_view->show() == GraphicsView::Show::Results) {
        if (a_model.find(a_plotGroup)) {
            QString why;
            showPlotGroup(a_plotGroup, &why);
        } else {
            a_plotGroup.clear();
            showGeometry();
        }
    }
}

std::shared_ptr<ParameterScope> MultiphysicsDoc::parameters() const
{
    if (a_paramsStale || !a_params) {
        a_params = evaluateParameters(a_model);
        a_paramsStale = false;
    }
    return a_params;
}

// ------------------------------------------------------------------ Building

const GeometryBuild& MultiphysicsDoc::geometry()
{
    if (a_geometryStale) buildGeometry();
    return a_geometry;
}

bool MultiphysicsDoc::buildGeometry(QString* error)
{
    a_geometryTimer->stop();
    const std::shared_ptr<ParameterScope> ps = parameters();
    for (auto it = ps->errors.cbegin(); it != ps->errors.cend(); ++it) say(tr("Parameter %1: %2").arg(it.key(), it.value()), true);
    QElapsedTimer timer;
    timer.start();
    a_geometry = qucs_s::fem::buildGeometry(a_model, *ps);
    a_geometryStale = false;
    a_meshStale = true;
    a_locator.reset();
    for (auto it = a_geometry.warnings.cbegin(); it != a_geometry.warnings.cend(); ++it) {
        const Node* n = a_model.find(it.key());
        say(tr("%1: %2").arg(n ? n->name() : it.key(), it.value()));
    }
    bool ok = a_geometry.ok();
    if (!ok) {
        const Node* n = a_model.find(a_geometry.failedAt);
        const QString why = tr("%1: %2").arg(n ? n->name() : a_geometry.failedAt, a_geometry.errors.value(a_geometry.failedAt));
        say(tr("The geometry stopped at %1").arg(why), true);
        if (error) *error = why;
    } else {
        const Topology& t = *a_geometry.topology;
        say(tr("Geometry: %1 domains, %2 boundaries, %3 points (%4 ms)")
                .arg(t.domains.size())
                .arg(t.boundaries.size())
                .arg(t.points.size())
                .arg(timer.elapsed()));
    }
    a_view->setTopology(a_geometry.topology);
    if (a_view->show() == GraphicsView::Show::Mesh) a_view->setMesh(nullptr);
    watchFiles();
    updateState();
    updateHighlight();
    return ok;
}

void MultiphysicsDoc::watchFiles()
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

void MultiphysicsDoc::startJob(const QString& what, std::function<void(Job&)> work, std::function<void(Job&)> done)
{
    if (a_job) return;
    a_lastError.clear();
    auto job = std::make_shared<Job>();
    job->what = what;
    job->done = std::move(done);
    job->model = a_model;
    job->parameters = parameters();
    job->topology = a_geometry.topology;
    job->mesh = a_meshStale ? nullptr : a_mesh;
    a_job = job;
    job->thread = QThread::create([job, work] { work(*job); });
    connect(job->thread, &QThread::finished, this, &MultiphysicsDoc::jobFinished);
    a_progress->setValue(0);
    a_progress->setFormat(what);
    a_progress->show();
    a_cancel->show();
    a_progressTimer->start();
    job->thread->start();
    updateState();
    emit stateChanged();
}

void MultiphysicsDoc::jobFinished()
{
    std::shared_ptr<Job> job = a_job;
    a_job.reset();
    a_progressTimer->stop();
    a_progress->hide();
    a_cancel->hide();
    if (!job) return;
    job->thread->deleteLater();
    for (const QString& w : job->warnings) say(w);
    if (!job->error.isEmpty()) {
        a_lastError = job->error;
        say(job->error, true);
    }
    if (job->done) job->done(*job);
    updateState();
    emit stateChanged();
}

void MultiphysicsDoc::buildMesh()
{
    if (isBusy()) return;
    if (a_geometryStale) {
        QString why;
        if (!buildGeometry(&why)) {
            a_lastError = why;
            emit stateChanged();
            return;
        }
    }
    if (!a_geometry.ok()) {
        a_lastError = tr("the geometry did not build");
        say(a_lastError, true);
        emit stateChanged();
        return;
    }
    say(tr("Meshing…"));
    startJob(tr("Meshing"), [](Job& job) {
        QElapsedTimer timer;
        timer.start();
        const MeshBuild m = qucs_s::fem::buildMesh(job.model, *job.parameters, job.topology, job.progress(0, 1));
        job.mesh = m.mesh;
        job.error = m.mesh ? QString() : QCoreApplication::translate("MultiphysicsDoc", "The mesh failed: %1").arg(m.error);
        job.warnings = m.warnings;
        if (m.mesh)
            job.warnings << QCoreApplication::translate("MultiphysicsDoc", "Mesh: %1 triangles, %2 nodes; smallest angle %3°, mean quality %4 (%5 ms)")
                                .arg(m.mesh->triangles.size())
                                .arg(m.mesh->nodes.size())
                                .arg(m.mesh->minAngle, 0, 'f', 1)
                                .arg(m.mesh->meanQuality, 0, 'f', 3)
                                .arg(timer.elapsed());
    }, [this](Job& job) {
        if (!job.mesh) return;
        a_mesh = job.mesh;
        a_meshStale = false;
        a_locator.reset();
        showMesh();
    });
}

QString MultiphysicsDoc::studyTag(const QString& study) const
{
    if (!study.isEmpty()) return study;
    const std::vector<const Node*> studies = a_model.studies();
    return studies.empty() ? QString() : studies.front()->tag;
}

void MultiphysicsDoc::compute(const QString& study)
{
    if (isBusy()) return;
    const QString tag = studyTag(study);
    if (tag.isEmpty()) {
        a_lastError = tr("The model has no study: add one (the model's menu).");
        say(a_lastError, true);
        emit stateChanged();
        return;
    }
    if (a_geometryStale || !a_geometry.ok()) {
        QString why;
        if (!buildGeometry(&why)) {
            a_lastError = why;
            emit stateChanged();
            return;
        }
    }
    const Node* node = a_model.find(tag);
    say(tr("Computing %1…").arg(node ? node->name() : tag));
    const bool meshIt = a_meshStale || !a_mesh;
    startJob(tr("Computing"), [meshIt, tag](Job& job) {
        job.study = tag;
        // (Its mesh made first when it is old: runStudy makes it.)
        job.run = runStudy(job.model, job.parameters, job.topology, meshIt ? nullptr : job.mesh, tag, job.progress(0, 1));
        job.meshIt = meshIt;
        job.warnings << job.run.warnings;
        if (!job.run.ok())
            job.error = QCoreApplication::translate("MultiphysicsDoc", "The study failed: %1").arg(job.run.error);
    }, [this](Job& job) {
        if (job.meshIt && job.run.mesh) {
            a_mesh = job.run.mesh;
            a_meshStale = false;
            a_locator.reset();
        }
        if (!job.run.ok()) return;
        auto set = std::make_shared<SolutionSet>();
        set->solutions = job.run.solutions;
        set->swept = job.run.swept;
        a_solutions[job.study] = set;
        a_stale[job.study] = false;
        for (const QString& line : job.run.log) say(line);
        if (set->solutions.size() > 1) say(tr("Swept %1: %2 solutions").arg(set->swept.join(QLatin1String(", "))).arg(set->solutions.size()));
        addDefaultPlots(job.study);
        evaluateAll(job.study);
        // Its first plot group shown.
        for (const Node& g : a_model.results().children)
            if (g.type == QLatin1String("plotgroup") && g.enabled
                && (g.text(QStringLiteral("study")) == job.study || (g.text(QStringLiteral("study")).isEmpty() && studyTag({}) == job.study))) {
                QString why;
                if (!showPlotGroup(g.tag, &why)) say(why, true);
                break;
            }
    });
}

void MultiphysicsDoc::clearSolutions()
{
    a_solutions.clear();
    a_stale.clear();
    a_scene.reset();
    a_plotGroup.clear();
    a_table->setRowCount(0);
    showGeometry();
    say(tr("The solutions are cleared."));
    updateState();
    emit stateChanged();
}

std::shared_ptr<Solution> MultiphysicsDoc::solution(const QString& study) const
{
    auto it = a_solutions.find(studyTag(study));
    return it == a_solutions.end() || it->second->solutions.empty() ? nullptr : it->second->solutions.back();
}

std::shared_ptr<SolutionSet> MultiphysicsDoc::solutions(const QString& study) const
{
    auto it = a_solutions.find(studyTag(study));
    return it == a_solutions.end() ? nullptr : it->second;
}

bool MultiphysicsDoc::solutionStale(const QString& study) const
{
    auto it = a_stale.find(studyTag(study));
    return it != a_stale.end() && it->second;
}

bool MultiphysicsDoc::waitForJob(int ms)
{
    if (!a_job) return true;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(this, &MultiphysicsDoc::stateChanged, &loop, [this, &loop] {
        if (!a_job) loop.quit();
    });
    timeout.start(ms);
    while (a_job && timeout.isActive()) loop.exec();
    return !a_job;
}

void MultiphysicsDoc::cancelJob()
{
    if (a_job) {
        a_job->cancel = true;
        say(tr("Cancelling…"));
    }
}

void MultiphysicsDoc::say(const QString& text, bool error)
{
    const QString stamp = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
    a_messages << (error ? QStringLiteral("Error: ") : QString()) + text;
    if (a_messages.size() > 500) a_messages.removeFirst();
    const QString escaped = text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    a_log->appendHtml(error ? QStringLiteral("<span style='color:#c62828'>%1 %2</span>").arg(stamp, escaped)
                            : QStringLiteral("<span>%1 %2</span>").arg(stamp, escaped));
    if (error) a_bottom->setCurrentWidget(a_log);
}

void MultiphysicsDoc::updateState()
{
    const bool busy = isBusy();
    a_buildGeometry->setEnabled(!busy);
    a_buildMesh->setEnabled(!busy);
    a_compute->setEnabled(!busy);
    QString banner;
    const QString shownStudy = a_plotGroup.isEmpty() ? QString() : [this] {
        const Node* g = a_model.find(a_plotGroup);
        return g ? studyTag(g->text(QStringLiteral("study"))) : QString();
    }();
    if (a_view->show() == GraphicsView::Show::Results && !shownStudy.isEmpty() && solutionStale(shownStudy))
        banner = tr("The model changed since this solution was computed: Compute again to see it as it is.");
    a_banner->setText(banner);
    a_banner->setVisible(!banner.isEmpty());
    a_showBox->blockSignals(true);
    a_showBox->setCurrentIndex(int(a_view->show()));
    a_showBox->blockSignals(false);
}

// ------------------------------------------------------------------ What is shown

void MultiphysicsDoc::showGeometry()
{
    a_view->setTopology(a_geometry.topology);
    a_view->setShow(GraphicsView::Show::Geometry);
    updateState();
    updateHighlight();
}

void MultiphysicsDoc::showMesh()
{
    a_view->setTopology(a_geometry.topology);
    a_view->setMesh(a_meshStale ? nullptr : a_mesh);
    a_view->setShow(a_meshStale || !a_mesh ? GraphicsView::Show::Geometry : GraphicsView::Show::Mesh);
    if (a_meshStale || !a_mesh) say(tr("No mesh yet: Build Mesh."));
    updateState();
    updateHighlight();
}

void MultiphysicsDoc::fillPlotBox()
{
    const QString current = a_plotBox->currentData().toString();
    a_plotBox->blockSignals(true);
    a_plotBox->clear();
    for (const Node& g : a_model.results().children)
        if (g.type == QLatin1String("plotgroup") || g.type == QLatin1String("plotgroup1d")) a_plotBox->addItem(g.name(), g.tag);
    const int at = a_plotBox->findData(a_plotGroup.isEmpty() ? current : a_plotGroup);
    if (at >= 0) a_plotBox->setCurrentIndex(at);
    a_plotBox->blockSignals(false);
    a_plotBox->setVisible(a_plotBox->count() > 0);
}

bool MultiphysicsDoc::showPlotGroup(const QString& tag, QString* error)
{
    const Node* g = a_model.find(tag);
    if (g && g->type == QLatin1String("plotgroup1d")) return !showGraphs(tag, error).isEmpty();
    if (!g || g->type != QLatin1String("plotgroup")) {
        if (error) *error = tr("there is no plot group %1").arg(tag);
        return false;
    }
    const QString study = studyTag(g->text(QStringLiteral("study")));
    const std::shared_ptr<SolutionSet> set = solutions(study);
    if (!set || set->solutions.empty()) {
        if (error) *error = tr("%1: its study is not computed yet - Compute it").arg(g->name());
        return false;
    }
    // The solution it shows: its time and parameter value.
    const std::vector<Instance> chosen = instances(*set, false, g->integer(QStringLiteral("point")), g->text(QStringLiteral("time")));
    if (chosen.empty()) {
        if (error) *error = tr("%1: no solution to show").arg(g->name());
        return false;
    }
    Solution* sol = select(*set, chosen.front());
    const QString label = chosen.front().label;
    auto scene = std::make_shared<PlotScene>();
    scene->title = g->text(QStringLiteral("title")).isEmpty() ? g->name() : g->text(QStringLiteral("title"));
    const QRectF region = sol->topology().bounds;
    const QColor ink = palette().color(QPalette::Text);
    // Its plots moved by a displacement.
    std::optional<Deformation> deformation;
    for (const Node& p : g->children)
        if (p.type == QLatin1String("deformation") && p.enabled) {
            Deformation d;
            d.x = p.text(QStringLiteral("x"));
            d.y = p.text(QStringLiteral("y"));
            d.scale = 0;
            if (p.text(QStringLiteral("scaling")) == QLatin1String("manual"))
                d.scale = evaluateConstant(p.text(QStringLiteral("scale")), sol->parameters().scope, Dim()).value_or(1);
            deformation = d;
        }
    Deformation* deform = deformation ? &*deformation : nullptr;
    for (const Node& p : g->children) {
        if (!p.enabled) continue;
        if (p.type == QLatin1String("surface")) {
            const QString expr = p.text(QStringLiteral("expression"));
            scene->values = surfaceData(*sol, expr, p.text(QStringLiteral("unit")), deform);
            if (!scene->values.error.isEmpty()) {
                scene->problems << tr("%1: %2").arg(p.name(), scene->values.error);
                continue;
            }
            if (!scene->values.warning.isEmpty()) scene->problems << tr("%1: %2").arg(p.name(), scene->values.warning);
            scene->surface = true;
            scene->colors = p.text(QStringLiteral("colors"));
            scene->min = scene->values.min;
            scene->max = scene->values.max;
            if (p.text(QStringLiteral("range")) == QLatin1String("manual")) {
                const std::optional<double> lo = evaluateConstant(p.text(QStringLiteral("min")), sol->parameters().scope, Dim());
                const std::optional<double> hi = evaluateConstant(p.text(QStringLiteral("max")), sol->parameters().scope, Dim());
                if (lo && hi && *hi > *lo) {
                    scene->min = *lo;
                    scene->max = *hi;
                }
            }
            const QString unit = scene->values.unit.name.isEmpty() ? dimName(scene->values.dim) : scene->values.unit.name;
            scene->surfaceLabel = scene->values.dim.isNone() && scene->values.unit.name.isEmpty() ? expr : tr("%1 (%2)").arg(expr, unit);
            if (scene->title == g->name()) scene->title = tr("%1: %2").arg(g->name(), scene->surfaceLabel);
        } else if (p.type == QLatin1String("contour")) {
            const SurfaceData values = surfaceData(*sol, p.text(QStringLiteral("expression")), p.text(QStringLiteral("unit")), deform);
            if (!values.error.isEmpty()) {
                scene->problems << tr("%1: %2").arg(p.name(), values.error);
                continue;
            }
            PlotScene::Contours c;
            c.data = contourData(values, p.integer(QStringLiteral("levels")));
            c.colors = p.text(QStringLiteral("colors"));
            c.color = colorOf(p.text(QStringLiteral("color")), ink);
            if (c.color == QColor(Qt::black) && qucs_s::ink::isDark(palette().color(QPalette::Base))) c.color = ink;
            c.min = values.min;
            c.max = values.max;
            scene->contours.push_back(c);
        } else if (p.type == QLatin1String("arrow")) {
            const QStringList n = p.pair(QStringLiteral("points"));
            const int nx = std::clamp(n.value(0).toInt(), 1, 200), ny = std::clamp(n.value(1).toInt(), 1, 200);
            PlotScene::Arrows a;
            a.data = arrowData(*sol, p.text(QStringLiteral("x")), p.text(QStringLiteral("y")), region, nx, ny);
            if (!a.data.error.isEmpty()) {
                scene->problems << tr("%1: %2").arg(p.name(), a.data.error);
                continue;
            }
            a.color = colorOf(p.text(QStringLiteral("color")), ink);
            if (a.color == QColor(Qt::black) && qucs_s::ink::isDark(palette().color(QPalette::Base))) a.color = ink;
            a.normalized = p.text(QStringLiteral("scaling")) == QLatin1String("normalized");
            a.spacing = std::min(region.width() / nx, region.height() / ny);
            scene->arrows.push_back(a);
        } else if (p.type == QLatin1String("meshplot")) {
            scene->mesh = true;
            scene->quality = p.flag(QStringLiteral("quality"));
        }
    }
    if (!label.isEmpty()) scene->title += QStringLiteral(" - ") + label;
    if (deform && deform->used > 0) scene->title += tr(" (deformed %1×)").arg(formatNumber(deform->used, 3));
    if (solutionStale(study)) scene->title += tr(" (an old solution)");
    a_scene = scene;
    a_plotGroup = tag;
    a_instanceLabel = label;
    a_view->setTopology(sol->meshPtr()->topology);
    a_view->setMesh(sol->meshPtr());
    a_view->setPlot(scene);
    a_view->setShow(GraphicsView::Show::Results);
    a_locator.reset();
    fillPlotBox();
    fillInstanceBox();
    updateState();
    updateHighlight();
    return true;
}

void MultiphysicsDoc::fillInstanceBox()
{
    a_instanceBox->blockSignals(true);
    a_instanceBox->clear();
    const Node* g = a_plotGroup.isEmpty() ? nullptr : a_model.find(a_plotGroup);
    const std::shared_ptr<SolutionSet> set = g ? solutions(g->text(QStringLiteral("study"))) : nullptr;
    if (set && a_view->show() == GraphicsView::Show::Results) {
        const std::vector<Instance> all = instances(*set, true);
        if (all.size() > 1)
            for (const Instance& in : all) {
                // (Its settings: the point from 1, the time in seconds.)
                const QString time = in.snapshot >= 0 ? QString::number(in.time, 'g', 17) : QString();
                a_instanceBox->addItem(in.label, QStringList{QString::number(in.point + 1), time});
                if (in.label == a_instanceLabel) a_instanceBox->setCurrentIndex(a_instanceBox->count() - 1);
            }
    }
    a_instanceBox->blockSignals(false);
    a_instanceBox->setVisible(a_instanceBox->count() > 1);
}

void MultiphysicsDoc::focusNode(const QString& tag)
{
    a_focus = tag;
    const Node* n = a_model.find(tag);
    if (n) {
        const NodeKind* k = n->kind();
        const QString group = k ? k->group : QString();
        if (n->type == QLatin1String("plotgroup")) {
            QString why;
            if (!showPlotGroup(tag, &why)) {
                say(why);
                showGeometry();
            }
        } else if (group == QLatin1String("plot")) {
            int at = -1;
            Model copy = a_model;
            if (Node* parent = copy.parentOf(tag, &at)) {
                QString why;
                if (!showPlotGroup(parent->tag, &why)) say(why);
            }
        } else if (n->type == QLatin1String("mesh") || group == QLatin1String("mesh")) {
            showMesh();
        } else if (group == QLatin1String("derived")) {
            if (a_view->show() != GraphicsView::Show::Results) showGeometry();
            evaluate(tag);
            a_bottom->setCurrentWidget(a_table);
        } else if (a_view->show() != GraphicsView::Show::Geometry
                   && (group == QLatin1String("geometry") || group == QLatin1String("material") || group == QLatin1String("physics")
                       || group == QLatin1String("es") || group == QLatin1String("ec") || group == QLatin1String("ht")
                       || group == QLatin1String("selection") || n->type == QLatin1String("geometry")
                       || n->type == QLatin1String("materials"))) {
            showGeometry();
        }
    }
    updateHighlight();
    emit focusChanged(tag);
}

void MultiphysicsDoc::updateHighlight()
{
    const Node* n = a_model.find(a_focus);
    std::shared_ptr<const Topology> topology = a_view->topology();
    if (!n || !topology) {
        a_view->setHighlight(Level::None, {});
        a_view->setPickLevel(Level::None);
        return;
    }
    const NodeKind* k = n->kind();
    Level level = k ? k->selection : Level::None;
    QJsonObject rule = n->selection();
    if (n->props.contains(QStringLiteral("level")) || (k && k->property(QStringLiteral("level"))))
        if (k && k->property(QStringLiteral("selection"))) level = levelOf(n->text(QStringLiteral("level")));
    if (k && k->group == QLatin1String("geometry") && !n->label.isEmpty() && topology->objectNames.contains(n->label)) {
        // A geometry feature: its object's domains.
        level = Level::Domain;
        rule = QJsonObject{{QStringLiteral("objects"), QJsonArray{n->label}}};
    }
    if (!k || !k->property(QStringLiteral("selection"))) {
        if (!(k && k->group == QLatin1String("geometry"))) level = Level::None;
    }
    QVector<int> picked;
    if (level != Level::None) {
        QString why;
        picked = resolveSelection(*topology, a_model, level, rule, &why);
    }
    a_view->setHighlight(level, picked);
    const bool pickable = a_picking && k && k->property(QStringLiteral("selection")) && level != Level::None;
    a_view->setPickLevel(pickable ? level : Level::None);
}

void MultiphysicsDoc::setPicking(bool pick)
{
    a_picking = pick;
    updateHighlight();
}

void MultiphysicsDoc::picked(Level level, int entity)
{
    if (entity < 0 || a_focus.isEmpty()) return;
    const Node* n = a_model.find(a_focus);
    std::shared_ptr<const Topology> topology = a_view->topology();
    if (!n || !topology) return;
    // The selection as numbers, the one clicked in or out.
    QString why;
    QVector<int> now = resolveSelection(*topology, a_model, level, n->selection(), &why);
    if (now.contains(entity)) now.removeAll(entity);
    else now.append(entity);
    std::sort(now.begin(), now.end());
    QJsonArray numbers;
    for (int e : now) numbers.append(e + 1);
    const QString what = levelName(level);
    editNode(a_focus, [numbers](Node& node) { node.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("numbers"), numbers}}); },
             tr("Pick %1 %2").arg(what).arg(entity + 1));
}

DerivedResult MultiphysicsDoc::evaluate(const QString& tag)
{
    DerivedResult out;
    const Node* n = a_model.find(tag);
    if (!n) {
        out.error = tr("there is no node %1").arg(tag);
        return out;
    }
    const std::shared_ptr<SolutionSet> set = solutions(n->text(QStringLiteral("study")));
    if (!set || set->solutions.empty()) {
        out.error = tr("%1: its study is not computed yet").arg(n->name());
        return out;
    }
    // At each solution it asks for: each output time, each parameter value.
    const bool all = n->text(QStringLiteral("solutions")) != QLatin1String("selected");
    for (const Instance& in : instances(*set, all)) {
        Solution* sol = select(*set, in);
        if (!sol) continue;
        DerivedResult one = evaluateDerived(*sol, *n);
        for (DerivedValue v : one.values) {
            if (!in.label.isEmpty()) v.name = in.label + QStringLiteral(": ") + v.name;
            out.values << v;
        }
        if (out.error.isEmpty() && !one.error.isEmpty()) out.error = in.label.isEmpty() ? one.error : in.label + QStringLiteral(": ") + one.error;
    }
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
    if (!out.error.isEmpty()) say(tr("%1: %2").arg(n->name(), out.error), out.values.isEmpty());
    a_table->resizeColumnsToContents();
    return out;
}

namespace {

/// A coordinate of the geometry: an expression in its unit (a plain number
/// is one), of the parameters.
std::optional<double> coordinate(const QJsonValue& v, const Solution& s, QString* why)
{
    const QString text = v.isDouble() ? formatNumber(v.toDouble(), 15) : v.toString();
    Expression::Options options;
    options.lengthUnit = s.topology().unitScale;
    options.numbersAreLengths = true;
    const Expression e = Expression::compile(text, s.parameters().scope, options);
    if (!e.isValid() || !e.isConstant()) {
        *why = QCoreApplication::translate("MultiphysicsDoc", "the coordinate %1: %2").arg(text, e.isValid() ? QCoreApplication::translate("MultiphysicsDoc", "not a constant") : e.error());
        return std::nullopt;
    }
    return e.constant();
}

} // namespace

QString MultiphysicsDoc::showGraphs(const QString& tag, QString* error, bool open)
{
    auto fail = [&](const QString& why) {
        if (error) *error = why;
        return QString();
    };
    const Node* g = a_model.find(tag);
    if (!g || g->type != QLatin1String("plotgroup1d")) return fail(tr("there is no 1D plot group %1").arg(tag));
    const std::shared_ptr<SolutionSet> set = solutions(g->text(QStringLiteral("study")));
    if (!set || set->solutions.empty()) return fail(tr("%1: its study is not computed yet - Compute it").arg(g->name()));
    using qucs_s::dataset::Variable;
    QList<Variable> vars;
    QSet<QString> names;
    QStringList traces;
    auto unique = [&](const QString& wanted) {
        const QString base = qucs_s::dataimport::safeName(wanted);
        QString n = base;
        for (int i = 2; names.contains(n); ++i) n = base + QLatin1Char('_') + QString::number(i);
        names.insert(n);
        return n;
    };
    // What the graphs run over: the output times, the sweep (time the faster).
    const std::vector<Instance> every = instances(*set, true);
    QStringList outer;
    const Solution& first = *set->solutions.front();
    if (first.snapshotCount() > 0) {
        Variable t;
        t.name = unique(QStringLiteral("time"));
        t.independent = true;
        for (int k = 0; k < first.snapshotCount(); ++k) t.re << first.snapshotTime(k);
        outer << t.name;
        vars << t;
    }
    if (set->solutions.size() > 1) {
        Variable p;
        const bool one = set->swept.size() == 1;
        p.name = unique(one ? set->swept.front() : QStringLiteral("sweep"));
        p.independent = true;
        for (std::size_t i = 0; i < set->solutions.size(); ++i)
            p.re << (one ? set->solutions[i]->sweptValues.value(set->swept.front()) : double(i + 1));
        outer << p.name;
        vars << p;
    }
    // (Of one solution at rest: an index of one.)
    auto outerOrIndex = [&]() {
        if (!outer.isEmpty()) return outer;
        if (!names.contains(QStringLiteral("solution"))) {
            Variable i;
            i.name = unique(QStringLiteral("solution"));
            i.independent = true;
            i.re << 1;
            vars << i;
        }
        return QStringList{QStringLiteral("solution")};
    };
    const std::vector<Instance> chosen = instances(*set, false, g->integer(QStringLiteral("point")), g->text(QStringLiteral("time")));
    QStringList problems;
    for (const Node& graph : g->children) {
        if (!graph.enabled) continue;
        const QString unitText = graph.text(QStringLiteral("unit"));
        if (graph.type == QLatin1String("globalgraph")) {
            const QStringList exprs = graph.text(QStringLiteral("expression")).split(QLatin1Char(';'), Qt::SkipEmptyParts);
            const QStringList deps = outerOrIndex();
            for (const QString& raw : exprs) {
                const QString text = raw.trimmed();
                // A Derived Values node: each of its values (tag), or one (tag.name).
                const Node* derivedNode = a_model.find(text.section(QLatin1Char('.'), 0, 0));
                if (derivedNode && derivedNode->kind() && derivedNode->kind()->group == QLatin1String("derived")) {
                    const QString only = text.section(QLatin1Char('.'), 1);
                    QMap<QString, Variable> byName;
                    QStringList order;
                    for (const Instance& in : every) {
                        Solution* sol = select(*set, in);
                        const DerivedResult r = evaluateDerived(*sol, *derivedNode);
                        if (!r.error.isEmpty()) problems << tr("%1: %2").arg(derivedNode->name(), r.error);
                        QSet<QString> seen;
                        for (const DerivedValue& dv : r.values) {
                            if (!only.isEmpty() && dv.name != only) continue;
                            if (!byName.contains(dv.name)) {
                                Variable v;
                                v.name = unique(dv.name);
                                v.dependencies = deps;
                                for (int k = 0; k < int(&in - &every.front()); ++k) v.re << std::numeric_limits<double>::quiet_NaN();
                                byName.insert(dv.name, v);
                                order << dv.name;
                            }
                            const DisplayUnit du = displayUnit(unitText, dv.dim);
                            byName[dv.name].re << (dv.value - du.offset) / du.scale;
                            seen.insert(dv.name);
                        }
                        for (const QString& n : order)
                            if (!seen.contains(n)) byName[n].re << std::numeric_limits<double>::quiet_NaN();
                    }
                    if (order.isEmpty()) problems << tr("%1 has no value %2").arg(derivedNode->name(), only);
                    for (const QString& n : order) {
                        vars << byName[n];
                        traces << byName[n].name;
                    }
                    continue;
                }
                Variable v;
                v.name = unique(text);
                v.dependencies = deps;
                for (const Instance& in : every) {
                    Solution* sol = select(*set, in);
                    const Expression e = Expression::compile(text, sol->scope());
                    double value = std::numeric_limits<double>::quiet_NaN();
                    if (!e.isValid()) problems << tr("%1: %2").arg(text, e.error());
                    else if (!e.isConstant()) problems << tr("%1 varies over the model: a Point Graph shows it at a point").arg(text);
                    else value = (e.constant() - displayUnit(unitText, e.dim()).offset) / displayUnit(unitText, e.dim()).scale;
                    v.re << value;
                }
                vars << v;
                traces << v.name;
            }
        } else if (graph.type == QLatin1String("pointgraph") || graph.type == QLatin1String("linegraph")) {
            const Node* data = a_model.find(graph.text(QStringLiteral("data")));
            const bool line = graph.type == QLatin1String("linegraph");
            if (!data || data->type != QLatin1String(line ? "cutline" : "cutpoint")) {
                problems << tr("%1: choose its dataset, a %2").arg(graph.name(), line ? tr("Cut Line 2D") : tr("Cut Point 2D"));
                continue;
            }
            const QString text = graph.text(QStringLiteral("expression"));
            const bool all = !line || graph.text(QStringLiteral("solutions")) != QLatin1String("selected");
            const std::vector<Instance>& over = all ? every : chosen;
            if (line) {
                QString why;
                const QStringList a = data->pair(QStringLiteral("start")), b = data->pair(QStringLiteral("end"));
                const int count = std::clamp(data->integer(QStringLiteral("points")), 2, 100000);
                Variable axis;
                const QString xaxis = graph.text(QStringLiteral("xaxis"));
                axis.name = unique(xaxis == QLatin1String("x") ? QStringLiteral("x") : xaxis == QLatin1String("y") ? QStringLiteral("y") : QStringLiteral("s_") + data->tag);
                axis.independent = true;
                Variable v;
                v.name = unique(text);
                v.dependencies = QStringList{axis.name} + (all ? outer : QStringList());
                bool axisMade = false;
                for (const Instance& in : over) {
                    Solution* sol = select(*set, in);
                    const std::optional<double> x0 = coordinate(a.value(0), *sol, &why), y0 = coordinate(a.value(1), *sol, &why);
                    const std::optional<double> x1 = coordinate(b.value(0), *sol, &why), y1 = coordinate(b.value(1), *sol, &why);
                    if (!x0 || !y0 || !x1 || !y1) {
                        problems << tr("%1: %2").arg(data->name(), why);
                        break;
                    }
                    const Locator locator(sol->mesh());
                    const LineData ld = lineData(*sol, locator, text, {*x0, *y0}, {*x1, *y1}, count);
                    if (!ld.error.isEmpty()) {
                        problems << tr("%1: %2").arg(text, ld.error);
                        break;
                    }
                    if (!axisMade) {
                        for (int i = 0; i < count; ++i)
                            axis.re << (xaxis == QLatin1String("x")   ? ld.points[std::size_t(i)].x() * sol->topology().unitScale
                                        : xaxis == QLatin1String("y") ? ld.points[std::size_t(i)].y() * sol->topology().unitScale
                                                                      : ld.along[std::size_t(i)]);
                        axisMade = true;
                    }
                    const DisplayUnit du = displayUnit(unitText, ld.dim);
                    for (double value : ld.values) v.re << (value - du.offset) / du.scale;
                }
                if (!axisMade) continue;
                vars << axis << v;
                traces << v.name;
            } else {
                // Each point's value over the solutions.
                const QJsonArray points = data->value(QStringLiteral("coordinates")).toArray();
                const QStringList deps = outerOrIndex();
                int index = 0;
                for (const QJsonValue& pv : points) {
                    ++index;
                    Variable v;
                    v.name = unique(points.size() > 1 ? text + QStringLiteral("_") + QString::number(index) : text);
                    v.dependencies = deps;
                    for (const Instance& in : every) {
                        Solution* sol = select(*set, in);
                        QString why;
                        const std::optional<double> x = coordinate(pv.toArray().at(0), *sol, &why), y = coordinate(pv.toArray().at(1), *sol, &why);
                        double value = std::numeric_limits<double>::quiet_NaN();
                        Dim dim;
                        if (x && y) {
                            const Locator locator(sol->mesh());
                            if (const std::optional<double> got = evaluateAt(*sol, locator, text, {*x, *y}, &why, &dim)) {
                                const DisplayUnit du = displayUnit(unitText, dim);
                                value = (*got - du.offset) / du.scale;
                            }
                        }
                        if (std::isnan(value) && !why.isEmpty() && problems.size() < 10) problems << why;
                        v.re << value;
                    }
                    vars << v;
                    traces << v.name;
                }
            }
        }
    }
    problems.removeDuplicates();
    if (traces.isEmpty()) return fail(problems.isEmpty() ? tr("%1 has no graph: add a Line Graph, a Global or a Point Graph").arg(g->name())
                                                         : problems.join(QLatin1String("; ")));
    for (const QString& p : problems) say(tr("%1: %2").arg(g->name(), p), true);
    // Beside the model (in the cache while it is not saved).
    QString base;
    if (getDocName().isEmpty()) {
        const QString dir = misc::cacheDir() + QStringLiteral("/multiphysics");
        QDir().mkpath(dir);
        base = dir + QStringLiteral("/untitled");
    } else {
        const QFileInfo fi(getDocName());
        base = fi.absoluteDir().filePath(fi.completeBaseName());
    }
    const QString dat = base + QLatin1Char('_') + tag + QStringLiteral(".dat");
    const QString dpl = base + QLatin1Char('_') + tag + QStringLiteral(".dpl");
    qucs_s::dataimport::Data data;
    data.variables = vars;
    qucs_s::dataimport::Origin origin;
    origin.source = getDocName().isEmpty() ? tr("(a model not saved)") : getDocName();
    origin.format = tr("Qucs-S multiphysics model");
    origin.imported = QDateTime::currentDateTime();
    QString why;
    if (!qucs_s::dataimport::writeDataset(dat, data, origin, &why)) return fail(why);
    say(tr("%1: %2 written (%3)").arg(g->name(), QFileInfo(dat).fileName(), traces.join(QLatin1String(", "))));
    if (open && a_App != nullptr) {
        const QString title = g->text(QStringLiteral("title")).isEmpty() ? g->name() : g->text(QStringLiteral("title"));
        const QString shown = a_App->showPythonDisplay(QJsonObject{{QStringLiteral("display"), dpl},
                                                                   {QStringLiteral("dataset"), dat},
                                                                   {QStringLiteral("traces"), QJsonArray::fromStringList(traces)},
                                                                   {QStringLiteral("type"), g->text(QStringLiteral("diagram"))},
                                                                   {QStringLiteral("title"), title}});
        if (!shown.isEmpty()) return fail(shown);
        // Its diagram kept: the display saved as it is now.
        for (QucsDoc* d : a_App->allDocuments())
            if (QFileInfo(d->getDocName()) == QFileInfo(dpl) && d->getDocChanged()) d->save();
    }
    return dpl;
}

void MultiphysicsDoc::evaluateAll(const QString& study)
{
    const Node* derived = a_model.results().child(QStringLiteral("derived"));
    if (!derived) return;
    for (const Node& d : derived->children)
        if (d.enabled && studyTag(d.text(QStringLiteral("study"))) == study) evaluate(d.tag);
}

void MultiphysicsDoc::addDefaultPlots(const QString& study)
{
    for (const Node& g : a_model.results().children)
        if (g.type == QLatin1String("plotgroup") && studyTag(g.text(QStringLiteral("study"))) == study) return;
    const std::shared_ptr<Solution> sol = solution(study);
    if (!sol) return;
    Model m = a_model;
    auto group = [&](const QString& title) {
        Node g = m.make(QStringLiteral("plotgroup"));
        g.label = title;
        g.props.insert(QStringLiteral("study"), study);
        return g;
    };
    auto plot = [&](Node& g, const QString& type, std::initializer_list<std::pair<const char*, QJsonValue>> props) {
        Node p = m.make(type);
        for (const auto& [k, v] : props) p.props.insert(QString::fromLatin1(k), v);
        // (tags unique among the new ones too)
        p.tag = p.tag + QLatin1Char('_') + g.tag;
        g.children.push_back(p);
    };
    for (const Field& f : sol->fields()) {
        if (!f.solved) continue;
        const QString t = f.tag;
        if (f.type == QLatin1String("electrostatics") || f.type == QLatin1String("currents")) {
            Node g = group(tr("Electric Potential (%1)").arg(t));
            plot(g, QStringLiteral("surface"), {{"expression", t + QStringLiteral(".V")}});
            plot(g, QStringLiteral("contour"), {{"expression", t + QStringLiteral(".V")}, {"levels", 15}});
            m.add(m.results().tag, g);
            const bool es = f.type == QLatin1String("electrostatics");
            Node g2 = group(es ? tr("Electric Field (%1)").arg(t) : tr("Current Density (%1)").arg(t));
            plot(g2, QStringLiteral("surface"), {{"expression", t + (es ? QStringLiteral(".normE") : QStringLiteral(".normJ"))}});
            plot(g2, QStringLiteral("arrow"), {{"x", t + (es ? QStringLiteral(".Ex") : QStringLiteral(".Jx"))},
                                               {"y", t + (es ? QStringLiteral(".Ey") : QStringLiteral(".Jy"))},
                                               {"scaling", QStringLiteral("normalized")}});
            m.add(m.results().tag, g2);
        } else if (f.type == QLatin1String("heat")) {
            Node g = group(tr("Temperature (%1)").arg(t));
            plot(g, QStringLiteral("surface"), {{"expression", t + QStringLiteral(".T")}, {"colors", QStringLiteral("thermal")}});
            plot(g, QStringLiteral("contour"), {{"expression", t + QStringLiteral(".T")}, {"levels", 12}});
            m.add(m.results().tag, g);
            Node g2 = group(tr("Heat Flux (%1)").arg(t));
            plot(g2, QStringLiteral("surface"), {{"expression", t + QStringLiteral(".normq")}, {"colors", QStringLiteral("thermal")}});
            plot(g2, QStringLiteral("arrow"), {{"x", t + QStringLiteral(".qx")}, {"y", t + QStringLiteral(".qy")}, {"scaling", QStringLiteral("normalized")}});
            m.add(m.results().tag, g2);
        } else if (f.type == QLatin1String("solid")) {
            // Its stress and displacement, drawn where it has moved.
            const bool axi = sol->axisymmetric();
            Node g = group(tr("Stress (%1)").arg(t));
            plot(g, QStringLiteral("surface"), {{"expression", t + QStringLiteral(".mises")}, {"unit", QStringLiteral("MPa")}});
            plot(g, QStringLiteral("deformation"), {{"x", t + QStringLiteral(".u")}, {"y", t + QStringLiteral(".v")}});
            m.add(m.results().tag, g);
            Node g2 = group(tr("Displacement (%1)").arg(t));
            plot(g2, QStringLiteral("surface"), {{"expression", t + QStringLiteral(".disp")}, {"colors", QStringLiteral("viridis")}});
            plot(g2, QStringLiteral("deformation"), {{"x", t + QStringLiteral(".u")}, {"y", t + QStringLiteral(".v")}});
            m.add(m.results().tag, g2);
            Q_UNUSED(axi);
        }
    }
    // In time, or swept: the chief globals over it, as a graph.
    const std::shared_ptr<SolutionSet> set = solutions(study);
    if (set && (sol->snapshotCount() > 1 || set->solutions.size() > 1)) {
        QStringList globals;
        for (const Field& f : sol->fields()) {
            if (!f.solved) continue;
            const QString t = f.tag + QLatin1Char('.');
            for (const QString& name : {t + QStringLiteral("Tmax"), t + QStringLiteral("C11"), t + QStringLiteral("R"), t + QStringLiteral("P"),
                                        t + QStringLiteral("dmax")})
                if (sol->global(name)) {
                    globals << name;
                    break;
                }
        }
        if (!globals.isEmpty()) {
            Node g = m.make(QStringLiteral("plotgroup1d"));
            g.label = sol->snapshotCount() > 1 ? tr("Over Time") : tr("Over the Sweep");
            g.props.insert(QStringLiteral("study"), study);
            plot(g, QStringLiteral("globalgraph"), {{"expression", globals.join(QLatin1String("; "))}});
            m.add(m.results().tag, g);
        }
    }
    setModel(m, tr("Default Plots"));
}

void MultiphysicsDoc::pointerMoved(QPointF point, bool inside)
{
    if (!inside) {
        a_status->clear();
        return;
    }
    std::shared_ptr<const Topology> t = a_view->topology();
    const QString unit = t ? (t->unitName == QLatin1String("um") ? QStringLiteral("µm") : t->unitName) : QString();
    QString text = tr("x = %1, y = %2 %3").arg(formatNumber(point.x(), 6), formatNumber(point.y(), 6), unit);
    if (t) {
        const int d = t->domainAt(point);
        if (d >= 0) text += tr("   domain %1 (%2)").arg(d + 1).arg(t->objectNames.value(t->domains[std::size_t(d)].owner));
    }
    if (a_view->show() == GraphicsView::Show::Results && a_scene && a_scene->surface && !a_plotGroup.isEmpty()) {
        const Node* g = a_model.find(a_plotGroup);
        const std::shared_ptr<SolutionSet> set = g ? solutions(g->text(QStringLiteral("study"))) : nullptr;
        Solution* sol = nullptr;
        if (set) {
            const std::vector<Instance> chosen = instances(*set, false, g->integer(QStringLiteral("point")), g->text(QStringLiteral("time")));
            if (!chosen.empty()) sol = select(*set, chosen.front());
        }
        const Node* surface = nullptr;
        if (g)
            for (const Node& p : g->children)
                if (p.type == QLatin1String("surface") && p.enabled) surface = &p;
        if (sol && surface) {
            if (!a_locator) a_locator = std::make_unique<Locator>(sol->mesh());
            QString why;
            Dim dim;
            const std::optional<double> v = evaluateAt(*sol, *a_locator, surface->text(QStringLiteral("expression")), point, &why, &dim);
            if (v) {
                const DisplayUnit du = displayUnit(surface->text(QStringLiteral("unit")), dim);
                text += QStringLiteral("   %1 = %2").arg(surface->text(QStringLiteral("expression")),
                                                        du.name.isEmpty() ? formatQuantity(*v, dim) : formatNumber((*v - du.offset) / du.scale, 5) + QLatin1Char(' ') + du.name);
            }
        }
    }
    a_status->setText(text);
}

bool MultiphysicsDoc::exportImage(const QString& path, QString* error)
{
    const QImage image = a_view->picture();
    if (!image.save(path, "PNG")) {
        if (error) *error = tr("Cannot write %1").arg(path);
        return false;
    }
    return true;
}
