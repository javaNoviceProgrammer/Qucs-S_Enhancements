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
#include <optional>
#include <vector>

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
 *
 * A geometry with a process flow is built in the background too - its
 * anneals and oxidations take seconds to minutes -, beside that job, with
 * the same progress and Cancel; so is a flow's step's state (shown, or as a
 * solution). What needs the geometry built waits for it: Build Mesh and
 * Compute go on once it is; buildGeometry() and geometry(), as the tools
 * call them, wait with the window going on. A change of the model while it
 * is built stops that build for one of the model as it is.
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
    /// The geometry as built now (built when stale; a process flow's in the
    /// background, waited for - the window going on); its errors in it.
    const qucs_s::fem::GeometryBuild& geometry();
    /// The geometry as last built, stale or not: not built here.
    const qucs_s::fem::GeometryBuild& builtGeometry() const { return a_geometry; }
    /// The geometry for a call that does not wait: as built now (built at
    /// once when stale), but a process flow's, to be built in the background,
    /// null - \a pending then how far it is. Nothing is started: a build
    /// stopped stays so.
    const qucs_s::fem::GeometryBuild* geometryWithoutWaiting(QString* pending);
    /// The geometry after a change or an opening: built at once, or a process
    /// flow's asked for in the background; what it is (empty: built).
    QString geometryAfterChange();
    bool geometryStale() const { return a_geometryStale; }
    /// The geometry being built in the background (a process flow's).
    bool geometryBeingBuilt() const { return a_build && a_buildWhole; }
    /// A geometry, or a flow's step's state, being built in the background.
    bool building() const { return a_build != nullptr; }
    std::shared_ptr<const qucs_s::fem::Mesh> mesh() const { return a_mesh; }
    bool meshStale() const { return a_meshStale || a_geometryStale; }
    /// The solution of study \a study (empty: the first) - its sweep's last
    /// point's - or null.
    std::shared_ptr<qucs_s::fem::Solution> solution(const QString& study = {}) const;
    /// Every solution of study \a study: each point of its sweep (each with
    /// its output times), or null.
    std::shared_ptr<qucs_s::fem::SolutionSet> solutions(const QString& study = {}) const;
    /// A process flow's step (or a flow: its last step) as a solution set to
    /// plot and evaluate - its dopants' variables, the values derived down
    /// its column at \a x (the window's middle by default) - made when
    /// first asked, kept until the model changes; null, and why. A step's
    /// tag given as a study is this.
    std::shared_ptr<qucs_s::fem::SolutionSet> processSolutions(const QString& tag, std::optional<double> x = {}, QString* error = nullptr) const;
    /// A process flow's step's state as a plot group made (one step of
    /// Undo): a colour map of \a expression (its net doping when empty), or
    /// with \a profile its species down x (\a species, all when empty; \a
    /// active their active parts too) from the top \a depth down on a log
    /// axis - SIMS's form. The group's tag, or empty and why.
    struct ProcessPlot {
        bool profile = false;
        QString expression, title, unit, colors;
        std::optional<double> x, depth;
        QStringList species;
        bool active = false;
    };
    QString makeProcessPlot(const QString& tag, const ProcessPlot& plot, QString* error = nullptr);
    /// Whether the model changed since \a study was solved.
    bool solutionStale(const QString& study = {}) const;
    /// When \a study was last solved (invalid: not since it was opened).
    QDateTime solvedAt(const QString& study = {}) const;
    /// Builds the geometry now; false and its error when it fails. A process
    /// flow's on a thread of its own, waited for with the window going on:
    /// cancelled, false and "cancelled".
    bool buildGeometry(QString* error = nullptr);
    /// Builds the geometry: at once, or with a process flow in the
    /// background (its progress and Cancel below the toolbar); \a then once
    /// it is built (true), or not (failed, cancelled: false).
    void requestGeometry(std::function<void(bool built)> then = {});
    /// The geometry as it is built, built again only when stale: false, and
    /// why, when it stopped.
    bool currentGeometry(QString* error = nullptr);
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
    /// Waits for the job running - and a geometry being built in the
    /// background, and what waits for it - to end, \a ms at most; whether
    /// they ended.
    bool waitForJob(int ms = 120000);
    /// As waitForJob(), but returning at once: \a then is told when they
    /// ended (true), or \a ms passed or the model went (false) - once. Held
    /// meanwhile as a wait is (closeWhenFree()). \a runningNow: the job and
    /// the build running now alone, not one started after them.
    void whenJobEnds(int ms, std::function<void(bool ended)> then, bool runningNow = false);
    /// Stops the job running, else what is built in the background.
    void cancelJob();
    /// The stops so far (cancelJob(), its tab closed): a call waiting behind
    /// a job, a stop meanwhile, starts nothing.
    quint64 stops() const { return a_stops; }
    /// Waited for now (a tool waiting for its geometry or its solve, the
    /// window going on): not to be deleted under the one waiting.
    bool beingWaitedFor() const { return a_waiting > 0; }
    /// Its tab closed while it is waited for: its work stopped, none started,
    /// deleted once the waiting is over.
    void closeWhenFree();
    bool closedWhenFree() const { return a_closed; }
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
    /// not get there (why in \a error). The model's own geometry kept. A
    /// flow's built in the background, waited for.
    qucs_s::fem::GeometryBuild showBuiltUpTo(const QString& tag, QString* error = nullptr);
    /// The same, not waited for: shown once built - when \a whileFocused, if
    /// the panel's node is still \a tag. A step asked for before it is shown
    /// stops it.
    void showStep(const QString& tag, bool whileFocused = false);
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
    /// The mode plot group \a tag shows - its study's mode at its point and
    /// frequency - as a Mode Profile export would write it, to \a path: its
    /// format by its suffix (.mat, .npz, .csv), a 201 by 201 grid over the
    /// mode's domains. What it wrote, said; empty and why in \a error.
    QString exportModeProfile(const QString& tag, const QString& path, QString* error = nullptr);
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
    void launch(const std::shared_ptr<Job>& job, std::function<void(Job&)> work);
    void jobFinished();
    // In the background: the geometry (whole), or a flow's step's state.
    void startBuild();
    std::shared_ptr<Job> startStep(const QString& tag, bool solution, std::optional<double> x);
    void startAside(const std::shared_ptr<Job>& job, std::function<void(Job&)> work);
    std::shared_ptr<qucs_s::fem::SolutionSet> stepSolutions(const QString& tag, std::optional<double> x, QString* error, bool background) const;
    bool waitForBuildSlot();
    bool waitFor(const std::shared_ptr<Job>& job);
    void buildFinished();
    bool buildNow(QString* error);
    bool installGeometry(const qucs_s::fem::GeometryBuild& built, qint64 ms, QString* error);
    QString geometryError() const;
    qucs_s::fem::GeometryBuild presentStep(const QString& tag, const qucs_s::fem::GeometryBuild& partial, QString* error);
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
    // Built in the background (a process flow's): the geometry, or a step's
    // state; whether whole; what waits for the whole; a whole one wanted
    // after the one at work, a step to show. Counted: the geometry's
    // changes, the model's.
    std::shared_ptr<Job> a_build;
    bool a_buildWhole = false;
    std::vector<std::function<void(bool)>> a_afterBuild;
    bool a_buildWanted = false;
    QString a_stepWanted;
    bool a_stepWantedFocused = false;
    quint64 a_geometryRevision = 0, a_revision = 0, a_builtRevision = ~quint64(0);
    int a_waiting = 0;        // waits under way (waitFor, waitForJob, whenJobEnds)
    // whenJobEnds's waits under way, each to be told if this goes first.
    std::map<quint64, std::function<void()>> a_jobWaits;
    quint64 a_jobWaitIds = 0;
    quint64 a_stops = 0;
    bool a_closed = false;    // its tab closed: deleted when no wait is
    bool a_viewPartial = false;   // the view a process step's state (showBuiltUpTo), not the whole geometry
    QPointF a_lastPointer;        // where the pointer was in the model (a process flow's cross-probe)
    bool a_geometryStale = true;
    std::shared_ptr<const qucs_s::fem::Mesh> a_mesh;
    bool a_meshStale = true;
    std::map<QString, std::shared_ptr<qucs_s::fem::SolutionSet>> a_solutions;
    mutable std::map<QString, std::shared_ptr<qucs_s::fem::SolutionSet>> a_processSolutions;
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
