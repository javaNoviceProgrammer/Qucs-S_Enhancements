/*
 * multiphysicsdoc.h - a multiphysics model (.qfem) in a tab of Qucs-S: its
 *                     Graphics view (geometry, mesh, results), its messages
 *                     and table of derived values; built and solved in the
 *                     background with a Cancel; each change one step of
 *                     Undo. The Multiphysics panel shows its tree.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_MULTIPHYSICSDOC_H
#define QUCS_MULTIPHYSICSDOC_H

#include "fem_material_plot.h"
#include "multiphysicsview.h"
#include "qucsdoc.h"

#include <QDateTime>
#include <QFrame>
#include <QPointer>

#include <atomic>
#include <functional>
#include <map>
#include <memory>

class LayoutDoc;
class QComboBox;
class QFileSystemWatcher;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSplitter;
class QTabWidget;
class QTableWidget;
class QThread;
class QTimer;
class QToolButton;
class QUndoStack;

/*!
 * A multiphysics model in a tab. The model (the tree the Multiphysics
 * panel shows) is changed through setModel() or editNode(): each change a
 * step of Undo, what it touches made stale - the geometry (built again at
 * once: it is quick), the mesh, the solutions (kept on show, marked old,
 * till computed again). The mesh and the studies are built in the
 * background, one job at a time, with progress and a Cancel.
 */
class MultiphysicsDoc : public QFrame, public QucsDoc
{
    Q_OBJECT

public:
    MultiphysicsDoc(QucsApp* app, const QString& name);
    ~MultiphysicsDoc() override;

    // QucsDoc
    void setName(const QString& name) override;
    bool load() override;
    int save() override;
    bool writeTo(const QString& path) override;
    void print(QPrinter* printer, QPainter* painter, bool printAll, bool fitToPage) override;
    void becomeCurrent(bool) override;
    double zoomBy(double factor) override;
    void showAll() override;
    void zoomToSelection() override;
    void showNoZoom() override;
    /// Reads its file again (changed outside Qucs-S): the steps of Undo go.
    bool reload();
    /// What reading its file last noted (a tag taken, a node unknown).
    QString readNotes() const { return a_readNotes; }

    // The model.
    const qucs_s::fem::Model& model() const { return a_model; }
    /// \a model in place of the model, a step of Undo named \a what. False
    /// when nothing changed.
    bool setModel(const qucs_s::fem::Model& model, const QString& what);
    /// Node \a tag changed by \a change: a step of Undo. False and why in
    /// \a error when there is no such node.
    bool editNode(const QString& tag, const std::function<void(qucs_s::fem::Node&)>& change, const QString& what,
                  QString* error = nullptr);
    QUndoStack* undoStack() const { return a_undo; }
    void undo();
    void redo();

    // Building and solving.
    std::shared_ptr<qucs_s::fem::ParameterScope> parameters() const;
    /// The geometry as built now (built when stale); its errors in it.
    const qucs_s::fem::GeometryBuild& geometry();
    bool geometryStale() const { return a_geometryStale; }
    std::shared_ptr<const qucs_s::fem::Mesh> mesh() const { return a_mesh; }
    bool meshStale() const { return a_meshStale || a_geometryStale; }
    /// The solution of study \a study (empty: the first) - its sweep's last
    /// point's - or null.
    std::shared_ptr<qucs_s::fem::Solution> solution(const QString& study = {}) const;
    /// Every solution of study \a study: each point of its sweep (each with
    /// its output times), or null.
    std::shared_ptr<qucs_s::fem::SolutionSet> solutions(const QString& study = {}) const;
    /// Whether the model changed since \a study was solved.
    bool solutionStale(const QString& study = {}) const;
    /// When \a study was last solved (invalid: not since it was opened).
    QDateTime solvedAt(const QString& study = {}) const;
    /// Builds the geometry now; false and its error when it fails.
    bool buildGeometry(QString* error = nullptr);
    /// Starts building the mesh (the geometry first, when stale).
    void buildMesh();
    /// Starts solving \a study (the first: empty) - its geometry and mesh
    /// first, when stale.
    void compute(const QString& study = {});
    /// Every solution let go.
    void clearSolutions();
    bool isBusy() const { return a_job != nullptr; }
    /// The job at work, how far: "61 % - Vc = -9.5 V (point 21 of 34):
    /// Solving Stationary - collector at -9.31 V of -9.5 V (step 412,
    /// 300 µV); Newton's step 6, its residual 2.1e-05" (empty: none).
    QString jobProgress() const;
    /// While a job runs: whether it makes the mesh (Build Mesh, or a study
    /// whose mesh is old), and the study it computes (empty: none).
    bool meshBeingMade() const { return a_job && a_jobMeshes; }
    QString computing() const { return a_job ? a_jobStudy : QString(); }
    /// Waits for the job running to end, \a ms at most; whether it ended.
    bool waitForJob(int ms = 120000);
    void cancelJob();
    /// The last job's error, or empty.
    QString lastError() const { return a_lastError; }
    /// The last job's warnings: a Newton's method that did not converge...
    QStringList lastWarnings() const { return a_lastWarnings; }
    /// A study's tag, or (empty) the first study's.
    QString studyTag(const QString& study) const;
    /// What was said: built, meshed, solved, failed.
    QStringList messages() const { return a_messages; }

    // What the Graphics view shows.
    qucs_s::fem::GraphicsView* view() const { return a_view; }
    /// Shows what node \a tag is about: a geometry feature's objects, a
    /// material's or a condition's selection, the mesh, a plot group. The
    /// node the panel chose.
    void focusNode(const QString& tag);
    QString focusedNode() const { return a_focus; }
    /// Shows a plot group's plots (it is worked out from its study's
    /// solution); false and why when it cannot be.
    bool showPlotGroup(const QString& tag, QString* error = nullptr);
    QString shownPlotGroup() const { return a_plotGroup; }
    std::shared_ptr<const qucs_s::fem::PlotScene> plotScene() const { return a_scene; }
    void showGeometry();
    /// The geometry as a process flow leaves it after its step \a tag (or
    /// as a geometry feature does), shown - its build, empty when it did
    /// not get there (why in \a error). The model's own geometry kept.
    qucs_s::fem::GeometryBuild showBuiltUpTo(const QString& tag, QString* error = nullptr);
    // A process flow and its layout.
    /// The layout a process flow \a flowTag cuts, opened in its tab (not
    /// brought to the front): null, and why in \a error, when it has none.
    LayoutDoc* flowLayout(const QString& flowTag, QString* error = nullptr);
    /// Its cut (µm, the layout's coordinates), when it has a layout.
    std::optional<QLineF> flowCut(const qucs_s::fem::Node& flow) const;
    /// The flow \a flowTag cut along the last ruler of its layout's tab (of
    /// a layout open in front, when it has none): its file, cell, start and
    /// end - a step of Undo.
    bool cutAlongRuler(const QString& flowTag, QString* error = nullptr);
    /// A step's mask, a layout's layer: that layer alone in the layout, its
    /// cut on it, the layout in front.
    bool showMaskInLayout(const QString& stepTag, QString* error = nullptr);
    /// Each material of the flow's regions on them: a node of its own, or
    /// (one of that library entry there) its regions added to it - those
    /// with none yet (one step of Undo); how many materials changed.
    int makeFlowMaterials(const QString& flowTag);
    void showMesh();
    /// Clicks in the view pick the entities of node \a tag's selection
    /// (on: \a pick), each toggled in it.
    void setPicking(bool pick);
    bool picking() const { return a_picking; }
    /// Evaluates the Derived Values node \a tag - at each solution it asks
    /// for (each time, each parameter value) - its values in the table, each
    /// value's name with its solution's ("t = 0.5 s: T at (1, 2)").
    qucs_s::fem::DerivedResult evaluate(const QString& tag);
    /// A 1D Plot Group's graphs: written as a Qucs dataset beside the model
    /// (name_tag.dat; in the cache when it is not saved), shown in a data
    /// display (name_tag.dpl). The display's path; empty and why on failure.
    QString showGraphs(const QString& tag, QString* error = nullptr, bool open = true, QStringList* removed = nullptr);
    /// \a material's properties \a keys along \a sweep (Lumerical's
    /// visualizer's): a dataset beside the model and a display of it; its
    /// path, or empty and why.
    QString plotMaterial(const qucs_s::fem::Node& material, const QStringList& keys, const qucs_s::fem::MaterialSweep& sweep,
                         QString* error = nullptr);
    /// Runs the Export node \a tag: its file written (a Touchstone file, a
    /// SPICE or Qucs subcircuit) where it says, or beside the model as
    /// model_tag.s2p, .cir, .sch (in the cache when it is not saved). The
    /// path; empty and why on failure.
    QString runExport(const QString& tag, QString* error = nullptr);
    /// A Qucs Subcircuit (RLCG) node's part alone on the clipboard, to paste
    /// into a schematic.
    bool copyPart(const QString& tag, QString* error = nullptr);
    /// The time and parameter value the shown plot group's study is shown
    /// at ("w = 2 mm, t = 0.5 s"), empty for one of one solution.
    QString shownInstance() const { return a_instanceLabel; }
    QComboBox* instanceBox() const { return a_instanceBox; }
    /// The values of the table, as shown.
    QTableWidget* table() const { return a_table; }
    QTabWidget* bottomTabs() const { return a_bottom; }
    QPlainTextEdit* messageLog() const { return a_log; }
    QLabel* statusLabel() const { return a_status; }
    QComboBox* showBox() const { return a_showBox; }
    QComboBox* plotBox() const { return a_plotBox; }
    QPushButton* cancelButton() const { return a_cancel; }
    /// A job's progress below the toolbar: its stage, its bar.
    QWidget* progressRow() const { return a_progressRow; }
    QLabel* stageLabel() const { return a_stage; }
    QProgressBar* progressBar() const { return a_progress; }
    /// The Graphics view as a PNG file.
    bool exportImage(const QString& path, QString* error = nullptr);

    /// A new model's content: a geometry, a study (File > New).
    static qucs_s::fem::Model starterModel();

signals:
    void signalFileChanged(bool);
    /// The model changed (an edit, an undo, a reload).
    void modelChanged();
    /// Built, meshed, solved, failed, busy or not.
    void stateChanged();
    void focusChanged(const QString& tag);

protected:
    void changeEvent(QEvent* event) override;

private:
    QString a_readNotes;
    struct Job;
    void buildUi();
    /// The model in place: \a read, its file's content (the first: no edit), else an edit.
    void applyModel(const qucs_s::fem::Model& model, bool read = false);
    void invalidate(const qucs_s::fem::Model& before, const qucs_s::fem::Model& after);
    void say(const QString& text, bool error = false);
    void startJob(const QString& what, std::function<void(Job&)> work, std::function<void(Job&)> done);
    void jobFinished();
    void updateState();
    void updateHighlight();
    void addDefaultPlots(const QString& study);
    void evaluateAll(const QString& study);
    void fillPlotBox();
    void fillInstanceBox();
    void watchFiles();
    void pointerMoved(QPointF point, bool inside);
    void picked(qucs_s::fem::Level level, int entity);

    qucs_s::fem::Model a_model;
    QUndoStack* a_undo = nullptr;
    mutable std::shared_ptr<qucs_s::fem::ParameterScope> a_params;
    mutable bool a_paramsStale = true;
    qucs_s::fem::GeometryBuild a_geometry;
    bool a_viewPartial = false;   // the view a process step's state (showBuiltUpTo), not the whole geometry
    QPointF a_lastPointer;        // where the pointer was in the model (a process flow's cross-probe)
    bool a_geometryStale = true;
    std::shared_ptr<const qucs_s::fem::Mesh> a_mesh;
    bool a_meshStale = true;
    std::map<QString, std::shared_ptr<qucs_s::fem::SolutionSet>> a_solutions;
    QString a_instanceLabel;
    QFileSystemWatcher* a_watcher = nullptr;
    std::map<QString, bool> a_stale;
    std::map<QString, QDateTime> a_solvedAt;
    std::unique_ptr<qucs_s::fem::Locator> a_locator;
    std::shared_ptr<Job> a_job;
    bool a_jobMeshes = false;
    QString a_jobStudy;
    QString a_lastError;
    QStringList a_lastWarnings;
    QStringList a_messages;
    QString a_focus;
    QString a_plotGroup;
    std::shared_ptr<const qucs_s::fem::PlotScene> a_scene;
    bool a_picking = false;
    QTimer* a_geometryTimer = nullptr;
    QTimer* a_progressTimer = nullptr;

    QToolButton* a_buildGeometry = nullptr;
    QToolButton* a_buildMesh = nullptr;
    QToolButton* a_compute = nullptr;
    QComboBox* a_showBox = nullptr;
    QComboBox* a_plotBox = nullptr;
    QComboBox* a_instanceBox = nullptr;
    QToolButton* a_fit = nullptr;
    QToolButton* a_image = nullptr;
    QWidget* a_progressRow = nullptr;
    QLabel* a_stage = nullptr;
    QProgressBar* a_progress = nullptr;
    QPushButton* a_cancel = nullptr;
    QLabel* a_banner = nullptr;
    qucs_s::fem::GraphicsView* a_view = nullptr;
    QSplitter* a_splitter = nullptr;
    QTabWidget* a_bottom = nullptr;
    QPlainTextEdit* a_log = nullptr;
    QTableWidget* a_table = nullptr;
    QLabel* a_status = nullptr;
};

#endif // QUCS_MULTIPHYSICSDOC_H
