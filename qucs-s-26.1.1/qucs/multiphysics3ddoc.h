/*
 * multiphysics3ddoc.h - a 3D multiphysics model (.qfem3d) in a tab of
 *                       Qucs-S: its geometry and swept mesh, built in the
 *                       background, and its solution - heat, electrostatics,
 *                       currents -, drawn in a 3D view; each change one step
 *                       of Undo. The Multiphysics 3D panel shows its tree.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_MULTIPHYSICS3DDOC_H
#define QUCS_MULTIPHYSICS3DDOC_H

#include "fem3_geometry.h"
#include "fem3_mesh.h"
#include "fem3_results.h"
#include "fem3_solver.h"
#include "qucsdoc.h"
#include "view3d.h"

#include <QFrame>

#include <functional>
#include <memory>

class QAction;
class QComboBox;
class QFileSystemWatcher;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QSlider;
class QTableWidget;
class QTimer;
class QToolButton;
class QUndoStack;

/*!
 * A 3D model in a tab. The model - the tree the Multiphysics 3D panel shows -
 * is changed through setModel() or editNode(), each change a step of Undo.
 * Its geometry is built again in the background after each change that
 * touches it, and its mesh when asked (Build Mesh); neither holds the window.
 * The view shows the geometry or the mesh, and the selection of the node the
 * panel has chosen: the domains, faces, edges or points it picks drawn solid,
 * the rest as its edges.
 */
class Multiphysics3DDoc : public QFrame, public QucsDoc
{
    Q_OBJECT

public:
    Multiphysics3DDoc(QucsApp* app, const QString& name);
    ~Multiphysics3DDoc() override;

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
    /// What reading its file last noted.
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
    /// The parameters evaluated (once per change of the model).
    std::shared_ptr<qucs_s::fem::ParameterScope> parameters() const;

    // The geometry and the mesh.
    const qucs_s::fem::GeometryBuild3& geometry() const { return a_geometry; }
    /// Whether the geometry shown is of the model as it was before an edit.
    bool geometryStale() const { return a_geometryStale; }
    bool geometryBeingBuilt() const;
    std::shared_ptr<const qucs_s::fem::Mesh3> mesh() const { return a_mesh; }
    bool meshStale() const { return a_meshStale; }
    bool meshBeingMade() const;
    /// Builds the geometry in the background (now: not waiting for the
    /// edits of a burst to end).
    void requestGeometry();
    /// Builds the mesh in the background, the geometry first where it is
    /// stale; shown when made.
    void buildMesh();
    /// Solves study \a study (the first when empty) in the background - its
    /// geometry and mesh first where they are stale -; shown when solved.
    /// \a solver (auto, direct, iterative) for this solve alone; a study
    /// estimated past the memory limit is not solved unless \a force.
    void compute(const QString& study = {}, const QString& solver = {}, bool force = false);
    /// What the last mesh made or solve was estimated to hold and take, each
    /// physics' (none before).
    const std::vector<qucs_s::fem::Estimate3>& estimates() const { return a_estimates; }
    /// The memory a solve is held to (bytes; 0: no limit): Multiphysics 3D >
    /// Settings (Multiphysics3D/memoryLimitGB), half of this machine's when
    /// not set.
    static double memoryLimit();
    /// The setting, in GB (0: no limit; less than 0: the default again).
    static void setMemoryLimitGB(double gb);
    std::shared_ptr<const qucs_s::fem::Solution3> solution() const { return a_solution; }
    /// Whether the solution is of the model as it was before an edit.
    bool solutionStale() const { return a_solutionStale; }
    /// A solution in time: the output time shown, by its number (from 0; -1:
    /// at rest) - its fields and globals those of that time.
    int shownTime() const;
    void setShownTime(int index);
    /// A sweep's solutions, a point each (one without a sweep; none before a
    /// solve), and the one shown (from 0; -1: none).
    const std::vector<std::shared_ptr<qucs_s::fem::Solution3>>& solutions() const { return a_solutions; }
    int shownPoint() const;
    void setShownPoint(int index);
    /// Evaluates Derived Values node \a tag - at each output time and point
    /// of the solution, or (its Solutions "selected") at the one shown -, its
    /// rows in the table in place of its last: what it found, or why not.
    qucs_s::fem::DerivedResult evaluate(const QString& tag);
    /// The derived values' table: a node, a value's name, the value.
    QTableWidget* table() const { return a_table; }
    /// Runs export \a tag (VTU, Field on a Grid) on the solution: written
    /// where its File says (relative to the model's folder) or beside the
    /// model as model_tag (in the cache while the model has no file), a
    /// collection's parts and a grid's coordinates beside it. Its path, or
    /// empty and why. \a beforeWrite, given, is asked of each file first: a
    /// reason not to write it, or empty.
    QString runExport(const QString& tag, QString* error = nullptr, const std::function<QString(const QString&)>& beforeWrite = {});
    bool solving() const;
    /// A mesh being made because one was asked for (Build Mesh, or a solve's
    /// first step) - not the geometry made again after an edit.
    bool meshing() const;
    /// Lets the work going on go.
    void cancel();
    bool isBusy() const { return a_job != nullptr; }
    /// Waits for the work going on, and a build waiting to start, \a ms at
    /// most; whether it ended.
    bool waitForIdle(int ms = 120000);
    /// Calls \a then once the work going on - a build, a solve, what waits
    /// behind it - has ended (true), or \a ms have gone by (false); nothing
    /// at work, at once. No loop of its own: a tool's call answers from it.
    /// Closed first: false, after.
    void whenJobEnds(int ms, std::function<void(bool)> then);
    /// Whether nothing is at work or waiting to be.
    bool idle() const;
    /// How far the work going on is ("Solving, 42 %"); empty: none.
    QString jobProgress() const;
    /// How much of what it works at is done, 0 to 1, as its bar shows it; -1
    /// when it works at nothing.
    double jobFraction() const;
    /// How many times its work was asked to stop (Cancel, cancel()).
    quint64 stops() const { return a_stops; }

    // What is shown.
    enum class Show { Geometry, Mesh, Solution, Plot };
    Show show() const { return a_show; }
    void showGeometry();
    void showMesh();
    void showSolution();
    /// 3D Plot Group \a tag shown: its plots over the solution - at the
    /// tab's time and point, or the group's own -, its first coloured one's
    /// legend, its clip. False and why where there is no such group or no
    /// solution.
    bool showPlotGroup(const QString& tag, QString* error = nullptr);
    /// The plot group shown (Show::Plot), or the last.
    QString plotGroup() const { return a_plotGroup; }
    /// An eigenmode expansion solved over two or more wavelengths: its
    /// spectra written as a dataset beside the model (model_spectrum.dat:
    /// against the wavelength, each S-parameter's |S|² in dB and its phase,
    /// each port mode's index) and, \a open, shown - |S|² in dB of the first
    /// fed port mode against the wavelength. The dataset's path, or empty and
    /// why.
    QString showSpectrum(bool open = true, QString* error = nullptr);
    /// What drawing the plot group last said (an expression that does not
    /// read, a unit not of its value), one line a plot; empty: nothing.
    QStringList plotProblems() const { return a_plotProblems; }
    /// What the solution is coloured by: an expression of its variables
    /// (empty: its first field's own, ht.T or V); its faces, and the cut's
    /// slice where there is one.
    QString plotExpression() const;
    void setPlotExpression(const QString& text);
    /// The values coloured now, least and greatest (SI): the legend's - the
    /// solution's, or a plot group's first coloured plot's.
    std::pair<double, double> plotRange() const { return a_plotRange; }
    /// The node the panel chose: its selection (a feature's: its solids)
    /// shown.
    void focusNode(const QString& tag);
    QString focus() const { return a_focus; }
    /// What is drawn solid: the level and the numbers (from 0).
    qucs_s::fem::Level highlightLevel() const { return a_highlightLevel; }
    QVector<int> highlighted() const { return a_highlighted; }
    View3D* view() const { return a_view; }
    /// What it said, in order (its messages pane's lines).
    QStringList messages() const { return a_messages; }
    /// The domains' colours, by number: what the view draws them in.
    static QColor domainColor(int domain);

    // What the view draws of each part: the view's alone - the geometry,
    // the mesh and a solve read none of it -, kept in the file
    // (Node::visibility) and a step of Undo.
    enum class Visibility { Shown, Hidden, Frame };
    /// Whether node \a tag may be hidden: a geometry feature (Form Union
    /// not), or a material (its domains).
    bool canHide(const QString& tag) const { return canHide(a_model, tag); }
    /// \a model's default 3D Plot Groups for a solution's \a fields, where it
    /// has none: two a physics - its first field on the boundaries, and on
    /// a slice through the middle across x. Whether any was added.
    static bool addDefaultPlots(qucs_s::fem::Model& model, const std::vector<qucs_s::fem::Field3>& fields);
    /// ... in model \a model (fem_edit's, before it is set).
    static bool canHide(const qucs_s::fem::Model& model, const QString& tag);
    /// How node \a tag is drawn (shown where it may not be hidden).
    Visibility visibility(const QString& tag) const;
    /// Node \a tag drawn so: a step of Undo. False where it may not be
    /// hidden, or is drawn so already.
    bool setVisibility(const QString& tag, Visibility visibility);
    /// Each part hidden or framed shown again: one step of Undo. False where
    /// none was.
    bool showAllParts();
    /// Whether some part is hidden or framed.
    bool anyHidden() const;
    /// How domain \a domain of \a topology is drawn: as its solid's
    /// feature (the smallest that holds it, as its material is), the least
    /// of that and its materials'.
    std::vector<Visibility> domainVisibilities(const qucs_s::fem::Topology3& topology) const;
    static QString visibilityName(Visibility visibility);   // "", "hidden", "frame"
    static Visibility visibilityOf(const QString& name);
    /// The view bar's Visibility drop-down: Show, Hide, Frame Only for the
    /// node chosen in the tree, Show All.
    QToolButton* visibilityButton() const { return a_visibilityButton; }

signals:
    void modelChanged();
    /// Built, being built, stale: what the panel's status column shows.
    void stateChanged();
    void signalFileChanged(bool);

protected:
    void changeEvent(QEvent* event) override;

private:
    struct Job;
    void buildUi();
    void applyModel(const qucs_s::fem::Model& model, bool read = false);
    void say(const QString& text, bool error = false);
    void startJob(bool mesh, bool solve = false);
    void sayFinished(const qucs_s::fem::StudyRun3& run, qint64 ms);
    void fillPlotChoices();
    void fillPointChoices();
    void fillTimeChoices();
    /// The Show box's items: the geometry, the mesh, the solution, and each
    /// 3D Plot Group by name.
    void fillShowChoices();
    /// After a solve, a model without a 3D Plot Group: two for each physics -
    /// its variable on the boundaries, and on a slice through the middle
    /// across x.
    void addDefaultPlots();
    /// Plot group \a group's plots over \a solution into \a scene and \a
    /// legend; its clip's axis (-1: none) and share across the model.
    void plotGroupScene(const qucs_s::fem::Node& group, qucs_s::fem::Solution3& solution, View3D::Scene& scene, View3D::Legend& legend,
                        int* clipAxis, double* clipShare);
    void jobFinished();
    void updateScene(bool fit = false);
    void updateHighlight();
    void updateState();
    /// The Visibility drop-down as the node chosen is drawn.
    void updateVisibilityButton();
    void watchFiles();

    qucs_s::fem::Model a_model;
    QUndoStack* a_undo = nullptr;
    QString a_readNotes;
    mutable std::shared_ptr<qucs_s::fem::ParameterScope> a_parameters;
    mutable quint64 a_parametersRevision = 0;
    quint64 a_revision = 1;
    quint64 a_solvedRevision = 1;   // of what a solve reads: a job of another, not kept

    qucs_s::fem::GeometryBuild3 a_geometry;
    bool a_geometryStale = true;
    std::shared_ptr<const qucs_s::fem::Mesh3> a_mesh;
    bool a_meshStale = true;
    bool a_firstShown = false;
    std::shared_ptr<Job> a_job;
    bool a_geometryWanted = false;   // a build asked while another worked
    bool a_meshWanted = false;
    bool a_solveWanted = false;
    QString a_study;                 // the study a solve asked for (empty: the first)
    qucs_s::fem::SolveAsked3 a_asked;   // and its solver and force (the limit set as it starts)
    std::vector<qucs_s::fem::Estimate3> a_estimates;
    quint64 a_stops = 0;
    struct Waiting {
        std::function<void(bool)> then;
        QTimer* timer = nullptr;
    };
    std::vector<std::shared_ptr<Waiting>> a_waiting;
    /// Each call waiting for the work to end told, where it has.
    void wakeWaiting();
    std::shared_ptr<qucs_s::fem::Solution3> a_solution;
    std::vector<std::shared_ptr<qucs_s::fem::Solution3>> a_solutions;
    bool a_solutionStale = true;
    QString a_plot;
    std::pair<double, double> a_plotRange{0, 0};
    // The plot's values at the nodes, of the solution, output time and
    // expression they are of: a redraw - a turn of the view, a focus - not
    // reading them again.
    struct PlotCache {
        std::shared_ptr<const qucs_s::fem::Solution3> solution;
        int snapshot = -1;
        QString text;
        std::vector<double> nodal;
    };
    PlotCache a_plotCache;
    bool a_closed = false;

    Show a_show = Show::Geometry;
    QString a_plotGroup;
    QStringList a_plotProblems;
    QString a_focus;
    qucs_s::fem::Level a_highlightLevel = qucs_s::fem::Level::None;
    QVector<int> a_highlighted;
    QStringList a_messages;

    View3D* a_view = nullptr;
    QToolButton* a_buildGeometry = nullptr;
    QToolButton* a_buildMesh = nullptr;
    QToolButton* a_compute = nullptr;
    QComboBox* a_showBox = nullptr;
    QComboBox* a_plotBox = nullptr;
    QLabel* a_timeLabel = nullptr;
    QComboBox* a_timeBox = nullptr;
    QLabel* a_pointLabel = nullptr;
    QComboBox* a_pointBox = nullptr;
    QComboBox* a_clipBox = nullptr;
    QSlider* a_clipSlider = nullptr;
    QProgressBar* a_progress = nullptr;
    QToolButton* a_cancel = nullptr;
    QToolButton* a_visibilityButton = nullptr;
    QAction* a_showAction = nullptr;
    QAction* a_hideAction = nullptr;
    QAction* a_frameAction = nullptr;
    QAction* a_showAllAction = nullptr;
    QLabel* a_stage = nullptr;
    QPlainTextEdit* a_notes = nullptr;
    QTableWidget* a_table = nullptr;
    QTimer* a_geometryTimer = nullptr;
    QTimer* a_progressTimer = nullptr;
    QFileSystemWatcher* a_watcher = nullptr;
};

#endif // QUCS_MULTIPHYSICS3DDOC_H
