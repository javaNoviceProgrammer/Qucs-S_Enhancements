/*
 * rf25doc.h - an RF 2.5D model (.qrf25) in a tab of Qucs-S: its layout
 *             drawn in the layout viewer's canvas, the stack beside it in
 *             z, and what is still missing under both; each change one
 *             step of Undo. The RF 2.5D panel shows its tree.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_RF25DOC_H
#define QUCS_RF25DOC_H

#include "layout.h"
#include "layoutdoc.h"
#include "qucsdoc.h"
#include "rf25/rf25_adaptive.h"
#include "rf25/rf25_circuit.h"
#include "rf25/rf25_fullwave.h"
#include "rf25/rf25_hfss.h"
#include "rf25/rf25_stackfile.h"
#include "rf25/rf25_inductance.h"
#include "rf25/rf25_kept.h"
#include "rf25/rf25_layout.h"
#include "rf25/rf25_mesh.h"
#include "rf25/rf25_static.h"
#include "rf25/rf25_model.h"
#include "rf25/rf25_solve.h"

#include <QElapsedTimer>
#include <QFrame>
#include <QPointer>
#include <QWidget>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

class QJsonArray;
class QLabel;
class QMenu;
class QPlainTextEdit;
class QProgressBar;
class QSplitter;
class QTimer;
class QToolButton;
class QUndoStack;

namespace qucs_s::rf25 {

/*!
 * The stack in z: its layers from the bottom up, each as tall as it is
 * thick (a plane and a via, which take no room of their own, as a line and
 * a bar at the side), in the colour the layout's shapes on it are drawn
 * in. The layer the panel chose is marked. It is what the layout's canvas
 * cannot show: the third direction.
 */
class StackView : public QWidget
{
    Q_OBJECT

public:
    explicit StackView(QWidget* parent = nullptr);

    void setStack(const Stack& stack);
    const Stack& stack() const { return a_stack; }
    /// The layer marked: a tag of the tree (empty: none).
    void setFocused(const QString& tag);
    QString focused() const { return a_focus; }
    QSize minimumSizeHint() const override { return QSize(90, 120); }
    QSize sizeHint() const override { return QSize(150, 320); }

signals:
    /// A click on a layer: its tag, for the panel to choose.
    void layerChosen(const QString& tag);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    /// Where each layer is drawn, top to bottom, with its tag.
    QList<QPair<QString, QRectF>> places() const;

    Stack a_stack;
    QString a_focus;
};

} // namespace qucs_s::rf25

/*!
 * An RF 2.5D model in a tab. The model (the tree the RF 2.5D panel shows)
 * is changed through setModel() or editNode(): each change a step of Undo.
 * The work it does in the background is reading the layout file the model
 * names and then making of it what the solver is given - the shapes
 * processed and divided into cells, with the unknowns counted - at every
 * edit, and the solve when asked: the quasi-static extraction or the
 * full-wave one, as the study says. A large GDSII file never holds the
 * window, and isBusy() is true while the first two run.
 */
class RF25Doc : public QFrame, public QucsDoc
{
    Q_OBJECT

public:
    RF25Doc(QucsApp* app, const QString& name);
    ~RF25Doc() override;

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
    /// What reading its file last noted, or why it could not be read.
    QString readNotes() const { return a_readNotes; }

    // The model.
    const qucs_s::rf25::Model& model() const { return a_model; }
    /// \a model in place of the model, a step of Undo named \a what. False
    /// when nothing changed.
    bool setModel(const qucs_s::rf25::Model& model, const QString& what);
    /// Node \a tag changed by \a change: a step of Undo. False and why in
    /// \a error when there is no such node.
    bool editNode(const QString& tag, const std::function<void(qucs_s::rf25::Node&)>& change, const QString& what,
                  QString* error = nullptr);
    QUndoStack* undoStack() const { return a_undo; }
    void undo();
    void redo();
    /// Everything the model says is missing, as the panel and the tab show
    /// it (Model::problems()).
    QStringList problems() const { return a_model.problems(); }
    /// Whether the model last changed by a read (the file opened or read
    /// again) rather than an edit: a read comes from no field of the
    /// panel's form, so the panel may make that form again at once.
    bool lastChangeWasRead() const { return a_lastChangeRead; }

    // The layout behind it.
    /// The layout file the model names, as this tab reads it: beside the
    /// model unless the name is absolute. Empty when it names none.
    QString layoutFile() const;
    /// Reads that file again, in the background. A read going on is
    /// stopped; nothing is read when the model names no file.
    void refreshLayout();
    /// The layout as read, or null (none named, not read yet, failed).
    std::shared_ptr<const qucs_s::layout::Layout> layout() const { return a_layout; }
    /// Why there is none: the file is not there, gdstk could not read it,
    /// the read was stopped. Empty when there is one or none is named.
    QString layoutProblem() const { return a_layoutProblem; }
    /// Its layout being read, or what the solver would be given made of
    /// it: the two pieces of work this build does.
    bool isBusy() const { return a_job != nullptr || a_prepareJob != nullptr; }
    /// Waits for that read, \a ms at most; whether it ended.
    bool waitForLayout(int ms = 120000);

    // What the solver would be given: the shapes processed, and the cells
    // they are divided into. Made in the background from the layout as
    // read, so that the window is never held by it, and made again
    // whenever the model or the layout changes.
    /// The shapes as the processing leaves them, or null (nothing read
    /// yet, nothing to process).
    std::shared_ptr<const qucs_s::rf25::Processed> processed() const { return a_processed; }
    /// Those shapes divided into cells, with the unknowns counted, or null.
    std::shared_ptr<const qucs_s::rf25::Mesh> mesh() const { return a_mesh; }
    /// Makes both again, in the background. A run going on is stopped.
    void prepare();
    /// Makes both again in a moment, coalescing the edits of a burst (a
    /// name typed letter by letter is one run, not one a letter).
    void schedulePrepare();
    bool isPreparing() const { return a_prepareJob != nullptr; }
    /// How many runs have ended since this tab opened. For the tests: it
    /// is how they tell a run that happened from one whose result was
    /// already there.
    int preparedRuns() const { return a_preparedRuns; }
    /// Waits for that work, \a ms at most, a run waiting to start
    /// included; whether it ended.
    bool waitForPrepared(int ms = 120000);
    /// What the processing and the mesh said they did, in order: for the
    /// panel's Processing and Mesh nodes and the tab's own notes.
    QStringList preparedLines() const;

    // The solve. Unlike the shapes and the mesh, which are made again at
    // every edit because they cost nothing, this is asked for: a dense
    // matrix of the unknowns is cubic work, and a user says when.
    /// The capacitance of the model as last solved, or null (never
    /// solved, solved full-wave, or the model changed since).
    std::shared_ptr<const qucs_s::rf25::Extraction> extraction() const { return a_extraction; }
    /// The ports' Z and S at each of the study's frequencies, and the
    /// currents, as last solved full-wave; or null (never solved so, solved
    /// quasi-statically, or the model changed since). One of this and
    /// extraction() is what a solve leaves, never both.
    std::shared_ptr<const qucs_s::rf25::FullWave> fullWave() const { return a_fullWave; }
    /// The last mesh convergence check (SolveRequest::levels), or null: none,
    /// or the model changed since.
    std::shared_ptr<const qucs_s::rf25::MeshConvergence> convergence() const { return a_convergence; }
    /// Whether the solve going on is a mesh convergence check.
    bool isCheckingConvergence() const;
    /// What a convergence check is asked for where nothing else is: the
    /// meshes defaultLevels() says of the model's own mesh's cells across.
    QList<int> convergenceLevels() const;
    /// Whether the study asks for the full-wave solve (its Solver).
    bool studyIsFullWave() const { return qucs_s::rf25::studyIsFullWave(a_model); }
    /// Solves it, in the background, on the shapes and the mesh as they
    /// stand, by the solver the study names. A solve going on is let go.
    /// Nothing happens where there is no mesh to solve on; solveProblem()
    /// then says why.
    /// While the shapes and the mesh are still to be made of an edit, or
    /// being made, it waits for them and starts when they are: a solve of
    /// the mesh before would be thrown away when they were done.
    void solve();
    /// What a solve may be asked to be in place of what the study says - a
    /// tool's question, which must leave the model as it was (rf25_solve.h).
    using SolveRequest = qucs_s::rf25::SolveRequest;
    /// solve(), as \a request says.
    void solveWith(const SolveRequest& request);
    /// How far a full-wave solve going on has got: frequencies solved, of
    /// how many. Nought of nought where none is going on.
    int frequenciesSolved() const;
    int frequenciesToSolve() const;
    /// Lets a solve going on go, or one waiting for the mesh: its result is
    /// dropped, and it is told to stop - a quasi-static one at its next row
    /// of a matrix or its next Green's function's next product, a
    /// full-wave one at its next frequency.
    void stopSolving();
    bool isSolving() const { return a_solveJob != nullptr || a_solveWhenPrepared; }
    /// What a solve under way is at, in words, and the count its bar shows:
    /// \a done of \a of (nought of nought: it cannot be counted now).
    QString solveCount(int* done, int* of) const;
    /// Whether a solve is going on, or waiting for its mesh, that \a request
    /// asks for too - the same solver, the study's when it names none, and
    /// a full-wave one's frequencies and matrix: rf25_solve and
    /// rf25_inductance join it rather than starting it again.
    bool solvingAlike(const SolveRequest& request) const;
    /// What it is doing, in words, as the bar's tip says it; empty where
    /// nothing is solving.
    QString solvingSaid() const;
    /// The step of the quasi-static solve going on (ExtractControl::Step),
    /// else -1.
    int quasiStaticStep() const;
    /// The solves still at work on their threads: the one going on, and
    /// any let go that have not yet come to their next check. A stopped
    /// solve let go ran on to its end before, beside the next.
    int solvesAtWork() const { return a_solvesAtWork->load(); }
    /// How many solves this tab has begun on their threads: one joined by
    /// rf25_solve is not begun again.
    int solvesBegun() const { return a_solvesBegun; }
    /// Waits \a ms at most for every solve's thread to end; whether they
    /// had.
    bool waitForSolvesToEnd(int ms);
    /// Waits for it, a solve waiting for the mesh included, \a ms at most;
    /// whether it ended.
    bool waitForSolved(int ms = 600000);
    /// Held by what waits on it with the window going on - a tool's call,
    /// from its first wait to its answer, and each waitFor...() -: its tab
    /// closed meanwhile (Stop and Close, a close_document, a tab kept running
    /// in the background and let go as its solve ended), it is not deleted
    /// under the one waiting but closed when free. Each wait ends at once
    /// once it is closed. It was deleted under rf25_solve, which read the
    /// freed model as it went on (B6 of the open items of 8 November).
    class Held
    {
    public:
        explicit Held(RF25Doc* doc);
        ~Held();
        Held(const Held&) = delete;
        Held& operator=(const Held&) = delete;

    private:
        QPointer<RF25Doc> a_doc;
    };
    bool beingWaitedFor() const override { return a_waiting > 0; }
    /// Its tab closed while it is held: its read, its mesh and its solve
    /// stopped, none started, and deleted once the last hold is let go.
    void closeWhenFree() override;
    bool closedWhenFree() const { return a_closed; }
    /// Tests: while \a held, a solve waits before it computes anything, and
    /// gives up at once if it is let go meanwhile - so that what happens to
    /// a solve going on, an edit's mesh overtaking it or a Stop, is seen in
    /// any build, not only of a solve slow enough to be caught at it.
    void holdSolves(bool held) { a_solvesHeld->store(held); }
    /// Tests: while held, every model's shapes and cells wait before they
    /// are made (a layout small enough is made before anything can look).
    static void holdPreparing(bool held) { preparingHeld().store(held); }
    static std::atomic_bool& preparingHeld();
    /// Tests: a full-wave solve begins only \a count more of its
    /// frequencies - its sweep's, then its calibration strips' - and waits
    /// until it is allowed more; below nought, as many as it has, as it
    /// does unless told. How far a solve has got, seen part way in any
    /// build.
    void allowFrequencies(int count) { a_frequenciesAllowed->store(count); }
    /// Why there is no capacitance: what the extraction refused, or what
    /// stood in the way of starting it. Empty when there is one.
    QString solveProblem() const { return a_solveProblem; }
    /// What became of the solution kept beside the model, "<model>.solution"
    /// (rf25_kept.h): shown - solved by what build and when -, or why not
    /// (another version of the model or its layout, a mesh made otherwise);
    /// empty where none was, and once the model is solved again.
    QString keptSaid() const { return a_keptSaid; }
    /// Whether the solution shown is the one kept beside the model.
    bool showsKeptSolution() const { return a_showsKept; }
    /// Where the solution shown is kept beside the model - written after
    /// its solve or its save, or read when it was opened -; empty where it
    /// is kept nowhere.
    QString keptFile() const { return a_keptFile; }
    /// What the solve said it did, for the Results node and the notes.
    QStringList solvedLines() const;
    /// One net as a line: its capacitance per metre, its effective
    /// permittivity and its impedance over the length that was solved; or
    /// of several, their modes, since a conductor of a coupled set is no
    /// line by itself. Empty where nothing was solved.
    QStringList lineSummary() const;
    /// The length a line's parameters are taken over, m: the longer side
    /// of what was solved, which is what a straight line's length is.
    /// Zero where nothing was solved.
    double solvedLength() const;
    QToolButton* solveButton() const { return a_solveButton; }
    /// The S-parameters at the top of the study's sweep - one net's as a
    /// two-port, several as their 2N-port - and where a quasi-static
    /// answer stops being the whole story. Into \a reference Ohm at every
    /// port, which a Touchstone's own comment must say since its data are
    /// referred to it; at zero, a lone line into its own impedance and a
    /// coupled set into 50 Ohm. Empty where nothing was solved.
    QStringList sParameterSummary(double reference = 0) const;
    /// What the full-wave solve found, for the Results node and the notes:
    /// its ports, each port's S at the top of the sweep in its own
    /// reference, and whether the answer is reciprocal and passive. Empty
    /// where nothing was solved full-wave.
    QStringList fullWaveSummary() const;
    /// The loop's inductance as last solved, or null: nothing solved, the
    /// model changed since, or it has not the two ports a loop wants.
    std::shared_ptr<const qucs_s::rf25::Inductance> inductance() const { return a_inductance; }
    /// The loop's L, its resistance at direct current and its Q, for the
    /// Results node and the notes; what the ports said where there is no
    /// loop to find.
    QStringList inductanceSummary() const;

    // What is written out of a solve. The engine's own Touchstone writers
    // do the writing (multiphysics/fem_export.h); what is here is the
    // reading of the node that asks for it.
    /// What a Touchstone export is of. An export node is read into one of
    /// these and a tool may give one outright, so that the two have one
    /// path to the file between them and cannot drift apart.
    struct ExportRequest {
        bool coupled = false;   ///< every net as a 2N-port, rather than one as a two-port (a quasi-static solve's)
        QString net;            ///< which net, for a two-port (empty: the first)
        double length = 0;      ///< m (0: the length that was solved)
        double impedance = 50;  ///< the ports' reference, Ohm
        bool version2 = false;  ///< Touchstone 2.0: each full-wave port into its own reference, a quasi-static solve's every port into impedance
        bool mixed = false;     ///< a full-wave solve's differential pairs as their modes, Touchstone 2.0 with [Mixed-Mode Order]
    };
    /// The Touchstone file \a what asks for, as text. Empty and why in
    /// \a error where it cannot be made.
    QString exportText(const ExportRequest& what, QString* error = nullptr) const;
    /// The export node \a tag read into a request. False and why in
    /// \a error where there is no such node or it says something that is
    /// no number.
    bool exportRequest(const QString& tag, ExportRequest* what, QString* error = nullptr) const;
    /// The file the export node \a tag asks for, as text: a Touchstone
    /// node's .sNp, or an Equivalent circuit node's part or subcircuit.
    QString exportText(const QString& tag, QString* error = nullptr) const;
    /// A lumped circuit fitted to the full-wave solve shown (rf25_circuit.h):
    /// its one or two ports'; none and why in \a error where it has none.
    std::optional<qucs_s::rf25::EquivalentCircuit> equivalentCircuit(QString* error = nullptr) const;
    /// \a circuit as \a path asks: a Qucs-S part for .sch, else a SPICE
    /// subcircuit named \a name (empty: the model's) - what it was fitted
    /// to, by what, and how well said with it.
    QString circuitText(const qucs_s::rf25::EquivalentCircuit& circuit, const QString& path, const QString& name) const;
    /// That text written to the file the node names - or, where it names
    /// none, to model_<tag>.sNp beside the model. The path written, or
    /// empty and why in \a error.
    QString writeExport(const QString& tag, QString* error = nullptr) const;
    /// Where writeExport() would put it: the node's file, made absolute
    /// against the model, or the default beside it. Empty where there is
    /// no such node.
    QString exportPath(const QString& tag) const;
    /// The model and its shapes as they stand, written for HFSS™ 3D Layout
    /// (rf25_hfss.h): the problems say why not where its shapes are not made.
    qucs_s::rf25::HfssExport hfssExport(const qucs_s::rf25::HfssOptions& options = {}) const;
    /// The stack written to \a path as the stack file its suffix says: .xml an
    /// HFSS™ 3D Layout XML stackup, .proc an EMX process file
    /// (rf25_stackfile.h). False and why in \a error; what was written, and
    /// where it is not what RF 2.5D solves, in \a written.
    bool exportStack(const QString& path, qucs_s::rf25::StackWritten* written = nullptr, QString* error = nullptr) const;
    /// \a path, an XML stackup or a process file, read as the stack: the
    /// Stack's children in place of these, and the layer map's rows for its
    /// layers, one step of Undo. False and why in \a error; what was read in
    /// \a read, and what is not as it was, or is now named but gone, in
    /// \a notes.
    bool importStack(const QString& path, qucs_s::rf25::StackRead* read = nullptr, QStringList* notes = nullptr,
                     QString* error = nullptr);
    /// Which of the processed shapes a coplanar port returns through, as the
    /// full-wave solve finds them, one for each; empty where it finds none or
    /// there is no mesh to tell by.
    QList<bool> returnShapes() const;
    /// Whether the processed shapes are drawn over the layout, and whether
    /// the mesh is. Kept for the next model opened.
    bool processedShown() const { return a_showProcessed; }
    void setProcessedShown(bool shown);
    bool meshShown() const { return a_showMesh; }
    void setMeshShown(bool shown);
    QToolButton* processedButton() const { return a_showProcessedButton; }
    QToolButton* meshButton() const { return a_showMeshButton; }
    /// Whether a full-wave solve's current density is drawn over the
    /// layout: with port \a currentsPort() driven by a source of its own
    /// reference and the others ended in theirs, at the
    /// currentsFrequency()'th of its frequencies (-1: the last). Kept for
    /// the next model opened, the port and the frequency for this one.
    bool currentsShown() const { return a_showCurrents; }
    void setCurrentsShown(bool shown);
    int currentsPort() const { return a_currentsPort; }
    int currentsFrequency() const { return a_currentsFrequency; }
    void setCurrentsOf(int port, int frequency);
    QToolButton* currentsButton() const { return a_showCurrentsButton; }
    /// What the current density drawn is of, and its range: for the panel.
    /// Empty where none is drawn.
    QString currentsSaid() const { return a_currentsSaid; }
    /// Whether the ports are drawn over the layout, each by its name: an
    /// edge port's face - the whole edge it drives - with an arrow out of
    /// it, a coplanar one's grounds beside it, an internal port's
    /// rectangle or point. Kept for the next model opened.
    bool portsShown() const { return a_showPorts; }
    void setPortsShown(bool shown);
    QToolButton* portsButton() const { return a_showPortsButton; }

    /// Placing a port on the layout by hand: with the Edge Port tool a
    /// click on (or by) an edge of a metal's shape puts an edge port on
    /// that edge, facing out of it; with the Internal Port tool a rectangle
    /// dragged over a metal puts an internal port there, driving its cells
    /// in the rectangle against the ground - a click, the one cell under
    /// it -, and one dragged with Shift a terminal among the returns of the
    /// port put last (placeTerminal()). Escape, or the tool's button again,
    /// puts it down.
    enum class Placing { None, EdgePort, InternalPort };
    Placing placing() const { return a_placing; }
    void setPlacing(Placing placing);
    QToolButton* edgePortButton() const { return a_edgePortButton; }
    QToolButton* internalPortButton() const { return a_internalPortButton; }
    /// The Ruler, as a layout's tab has it: taken up, a drag over the layout
    /// measures, snapped to the shapes' corners and edges (Shift: across or
    /// up only), each ruler labelled with its length and the last one said
    /// in the status line - its length, dx and dy, its ends. Escape takes
    /// back the one being drawn, then clears them, then puts the Ruler
    /// down; the port tools, the drawing tools, the pen and it are one at a
    /// time. The rulers stay while the layout is read again in place (a
    /// drawing, another map), not for another layout.
    QToolButton* rulerButton() const { return a_rulerButton; }
    /// The last ruler as the status line says it; empty where there is none.
    QString rulerSaid() const;
    /// The pen, at the toolbar's right end, as a layout's tab has it: taken
    /// up, a drag over the layout draws on its cell, for Claude to read
    /// (read_drawings, and each prompt about this model); it and the port
    /// tools are one at a time.
    qucs_s::markup::PenButton* penButton() const { return a_penButton; }
    /// An edge port put on the edge of a metal's processed shape nearest
    /// \a at (µm), within \a reach µm (nought: ten pixels as the view is
    /// zoomed): at its middle, facing out of it, its conductor that metal -
    /// an edit that can be undone, and the panel shows it. Its tag, or
    /// empty and why in \a why: no edge in reach, or a port drives it.
    QString placeEdgePortAt(QPointF at, double reach = 0, QString* why = nullptr);
    /// An internal port over \a box (µm) - a point where it is empty - to
    /// the topmost metal with a shape under it, from the ground. Its tag,
    /// or empty and why.
    QString placeInternalPort(const QRectF& box, QString* why = nullptr);
    /// A terminal over \a box (µm) - a point where it is empty - on the
    /// topmost metal with a shape under it, among the returns of the port
    /// put on the layout last: that port then returns through the terminals
    /// it names, this one added - one edit, undone together. Its tag, or
    /// empty and why: no port put yet, no metal under it.
    QString placeTerminal(const QRectF& box, QString* why = nullptr);

    /*!
     * Drawing the shapes themselves (F4 of the open items of 8 November):
     * with the Rectangle tool a drag draws a rectangle, with the Polygon
     * tool each click a corner - a double click, Enter or a click on the
     * first corner closes it, Backspace takes the last back -, with the Via
     * tool a click a square via of viaSize() there; each on drawLayer(),
     * its corners snapped to a shape's corner or edge within reach, else to
     * the grid shown. In Select, Delete deletes the shape selected. What is
     * drawn is written at once to the layout file the model names - a
     * GDSII file begun beside the model where it names none -, on the
     * layer of the file the layer map sends to that layer of the stack (a
     * row added for one it has none for); each shape one step of Undo, the
     * file as it was put back. The port tools, the pen and these are one
     * at a time; Escape puts it down.
     */
    enum class DrawTool { None, Rectangle, Polygon, Via };
    DrawTool drawTool() const { return a_drawTool; }
    void setDrawTool(DrawTool tool);
    /// The Draw button (its arrow, the menu: the tools, the layer, the
    /// via's size, Delete).
    QToolButton* drawButton() const { return a_drawButton; }
    /// The layer of the stack drawn on, by name: as chosen in the Draw menu
    /// (or setDrawLayer()), else the metal or via chosen in the stack, else
    /// the topmost metal; empty where the stack has no metal.
    QString drawLayer() const;
    void setDrawLayer(const QString& name);
    /// A via's side, µm (kept for the next model opened); 1 where unset.
    double viaSize() const { return a_viaSize; }
    void setViaSize(double um);
    /// \a at (µm) as the drawing tools snap it: to the corner, else the
    /// edge, of a shape shown within a few pixels; else to the grid shown.
    QPointF drawSnapped(QPointF at) const;
    /// The corners of the polygon being drawn, µm (empty: none is).
    const QPolygonF& polygonDrawn() const { return a_polygon; }
    /// The polygon being drawn closed and drawn (three corners at least);
    /// whether it was.
    bool finishPolygon(QString* why = nullptr);
    /*!
     * \a edits drawn, all of them or none, as one step of Undo; what each
     * did in \a done, else why in \a error. Each is an object:
     *
     *   {"add": "rectangle", "layer": L, "box": [x0, y0, x1, y1]}
     *   {"add": "polygon", "layer": L, "points": [[x, y], ...]}
     *   {"add": "path", "layer": L, "points": [...], "width": w, "ends": "flush"|"round"|"extended"}
     *   {"add": "via", "layer": V, "at": [x, y], "size": s}    (V: a via; the stack's first where not given; s: viaSize())
     *   {"delete": "shape", "layer": L, "at": [x, y]}         the smallest of L's there
     *   {"delete": "shape", "layer": L, "box": [x0, y0, x1, y1]}   every one of L's wholly inside
     *
     * L a metal or a via of the stack by name or tag, µm in the cell
     * solved. The layout is written at once (begun beside the model, which
     * must be saved, where it names none), then read again.
     */
    bool drawShapes(const QJsonArray& edits, QStringList* done = nullptr, QString* error = nullptr);
    /// The shape selected in the view deleted from the layout file (one of
    /// the cell solved's own, on a layer the map sends to the stack).
    bool deleteSelectedShape(QString* why = nullptr);

    // What is shown.
    qucs_s::layout::LayoutView* view() const { return a_view; }
    qucs_s::rf25::StackView* stackView() const { return a_stack; }
    QPlainTextEdit* problemsText() const { return a_problemsText; }
    QLabel* statusLabel() const { return a_status; }
    /// How far a solve has got, beside the status line: hidden while none
    /// goes on; busy where it cannot be counted - the quasi-static
    /// extraction, a solve waiting for its mesh -, else the full-wave
    /// solve's frequencies solved of those to solve, then its ports'
    /// calibration strips'.
    QProgressBar* progressBar() const { return a_progressBar; }
    /// Beside the bar, shown with it: stops the solve, as Solve's Stop does.
    QToolButton* progressStop() const { return a_progressStop; }
    /// How long the solve's count should take yet, at the rate it has gone
    /// so far - "about 40 s left" -, once it has counted one and run a
    /// second; empty where it cannot be said (busy, nothing counted yet).
    QString timeLeft() const;
    /// \a seconds left, as the tab says them: "a few seconds left", "about
    /// 40 s left" (to five), "about 7 min left", "about 1 h 30 min left".
    static QString leftSaid(double seconds);
    /// Shows what node \a tag is about: a layer marked in the stack, the
    /// layout fitted. The node the panel chose.
    void focusNode(const QString& tag);
    QString focusedNode() const { return a_focus; }
    /// How each layer of the layout file is drawn: the colour of the stack
    /// layer the map sends it to, and grey and hollow for one the map
    /// leaves out of the solve.
    QVector<qucs_s::layout::LayerStyle> stylesFor(const qucs_s::layout::Layout& layout) const;
    /// The colour of the \a index'th layer of the stack, from the bottom
    /// up: the layout viewer's own palette, so that a shape and its layer
    /// are the same colour in both tabs.
    static QColor layerColor(int index);
    /// \a fill as an outline over a shape drawn in \a fill itself:
    /// darkened on a light canvas, lightened on a dark one.
    QColor outlineColor(const QColor& fill) const;

    /// A new model's content (File > New, the panel's New Model).
    static qucs_s::rf25::Model starterModel();

signals:
    void signalFileChanged(bool);
    /// The model changed (an edit, an undo, a reload).
    void modelChanged();
    /// The layout read, or being read; a file saved.
    void stateChanged();
    /// The shapes and the mesh made again, or being made.
    void preparedChanged();
    /// A solve started, ended, or let go.
    void solvedChanged();
    void focusChanged(const QString& tag);

private:
    struct Job;
    struct PrepareJob;
    int a_waiting = 0;      // (Held)
    bool a_closed = false;  // (closeWhenFree())
    using SolveJob = qucs_s::rf25::SolveJob;   ///< (rf25_solve.h)
    void buildUi();
    void prepareDone();
    void solveDone();
    /// stopSolving() but for the bar: a solve let go to be started again at
    /// once - on an edit's mesh, or as solveWith() asks - keeps it.
    void letSolveGo();
    /// The bar as the solve now is, and the timer that keeps it so running
    /// while one goes on.
    void showProgress();
    /// The solution kept beside the model, read with it: held until its
    /// mesh is made, and taken then where it is the one solved on.
    void readKeptSolution();
    /// The solution shown kept beside the model, where it is of the model
    /// as it is on disk: after a solve of it, and when it is saved.
    void keepSolution();
    /// SHA-1 of the layout file's bytes; empty where it names none.
    QByteArray layoutStamp() const;
    /// What the bar counts now: of how many, how many done (nought of
    /// nought: busy), what it is in words, and which count it is - the
    /// sweep's frequencies, or one feed's calibration strips -, whose clock
    /// starts again when it changes.
    struct Count {
        int of = 0, done = 0;
        QString said, key;
    };
    Count countNow() const;
    /// The processed shapes and the mesh over the layout, as the two
    /// buttons have them.
    void showOverlays();
    /// The topmost metal with a processed shape under \a box (µm): under its
    /// point where it is empty, else overlapping it; empty where none is.
    QString metalUnder(const QRectF& box) const;
    /// The model in place: \a read, its file's content (no edit), else an edit.
    void applyModel(const qucs_s::rf25::Model& model, bool read = false);
    void showProblems();
    void showLayout();
    void readingDone();
    void pointerMoved(QPointF point, bool inside);

    qucs_s::rf25::Model a_model;
    QUndoStack* a_undo = nullptr;
    QString a_readNotes;
    QString a_focus;
    std::shared_ptr<Job> a_job;
    std::shared_ptr<const qucs_s::layout::Layout> a_layout;
    QString a_layoutRead;       ///< the file a_layout was read from
    QString a_layoutProblem;
    std::shared_ptr<PrepareJob> a_prepareJob;
    bool a_prepareAgain = false;   ///< it changed while that run went on
    int a_preparedRuns = 0;
    std::shared_ptr<const qucs_s::rf25::Processed> a_processed;
    bool a_showPorts = true;
    QToolButton* a_showPortsButton = nullptr;
    Placing a_placing = Placing::None;
    QString a_lastPlaced;          ///< the port put on the layout last: a Shift-drag's terminal returns it
    QToolButton* a_edgePortButton = nullptr;
    QToolButton* a_internalPortButton = nullptr;
    qucs_s::markup::PenButton* a_penButton = nullptr;
    void updatePen();
    DrawTool a_drawTool = DrawTool::None;
    DrawTool a_lastTool = DrawTool::Rectangle;   ///< the button's click takes it up again
    QString a_drawLayer;                         ///< chosen in the menu; empty: drawLayer()'s own choice
    double a_viaSize = 1;
    QPolygonF a_polygon;                         ///< the polygon being drawn
    QPointF a_drawPointer;                       ///< where the pointer is, snapped
    bool a_drawPointerIn = false;
    QToolButton* a_drawButton = nullptr;
    QMenu* a_drawMenu = nullptr;
    QObject* a_drawMenuParts = nullptr;          ///< what the menu's last filling made beside its actions
    QString a_drawnSaid;                         ///< what the last drawing did, said till the pointer moves
    bool a_keepView = false;                     ///< the layout read next is shown where the view is: a drawing's
    QList<qucs_s::layout::LayoutView::Overlay> a_overlaysShown;   ///< showOverlays()' own, under what is being drawn
    /// The overlays shown again: showOverlays()' and what is being drawn.
    void showDrawing();
    void fillDrawMenu();
    /// The drawing tool put down, the view's mode as it is (the pen, a port
    /// tool, taken up).
    void dropDrawTool();
    /// A click with a drawing tool: a polygon's corner, a via.
    void drawPicked(QPointF at);
    /// The model and the layout file's bytes \a bytes (none: no file) in
    /// place, the layout read again: a drawing's Undo and Redo.
    void applyDrawn(const qucs_s::rf25::Model& model, const QString& path, const std::optional<QByteArray>& bytes);
    bool eventFilter(QObject* watched, QEvent* event) override;
    std::shared_ptr<const qucs_s::rf25::Mesh> a_mesh;
    std::shared_ptr<SolveJob> a_solveJob;
    std::shared_ptr<const qucs_s::rf25::Extraction> a_extraction;
    std::shared_ptr<const qucs_s::rf25::FullWave> a_fullWave;
    std::shared_ptr<const qucs_s::rf25::MeshConvergence> a_convergence;
    QByteArray a_convergenceModel, a_convergenceLayout;   ///< what it checked: solveStampOf(), layoutStamp()
    QStringList a_waveNotes;       ///< what reading the ports said (a de-embedding not done...)
    SolveRequest a_solveRequest;   ///< the last asked for, for one waiting for the mesh
    QTimer* a_progress = nullptr;  ///< a solve's bar, and a full-wave one's frequencies, counted while it runs
    QElapsedTimer a_solvingSince;  ///< since a solve was asked for; invalid while none goes on
    QElapsedTimer a_countSince;    ///< since the count the bar shows began (Count::key)
    std::optional<qucs_s::rf25::KeptSolution> a_kept;   ///< read with the model, till its mesh is made
    QString a_keptSaid;
    QString a_keptFile;
    bool a_showsKept = false;
    // What the solution shown was solved from and by - its stamps, the
    // build, when - for keeping it beside the model.
    QByteArray a_solvedModel, a_solvedLayout;
    int a_solvedCells = 0;
    QString a_solvedBy;
    QDateTime a_solvedAt;
    QString a_countKey;
    std::shared_ptr<const qucs_s::rf25::Inductance> a_inductance;
    bool a_lastChangeRead = false;
    QString a_portsSaid;
    QString a_solveProblem;
    double a_solvedLength = 0;
    QTimer* a_prepareSoon = nullptr;
    /// A Solve asked for while the shapes and the mesh were being made: it
    /// starts when they are.
    bool a_solveWhenPrepared = false;
    /// holdSolves(): each solve job holds it too, so it outlives the tab.
    std::shared_ptr<std::atomic_bool> a_solvesHeld = std::make_shared<std::atomic_bool>(false);
    /// allowFrequencies(): held by each solve job, as that is.
    std::shared_ptr<std::atomic_int> a_frequenciesAllowed = std::make_shared<std::atomic_int>(-1);
    /// solvesAtWork(): each solve's thread holds it and counts itself out.
    std::shared_ptr<std::atomic_int> a_solvesAtWork = std::make_shared<std::atomic_int>(0);
    int a_solvesBegun = 0;
    bool a_showProcessed = true;
    bool a_showMesh = false;
    bool a_showCurrents = false;
    int a_currentsPort = 0;
    int a_currentsFrequency = -1;
    QString a_currentsSaid;

    QToolButton* a_fit = nullptr;
    QToolButton* a_reread = nullptr;
    QToolButton* a_rulerButton = nullptr;
    QToolButton* a_showProcessedButton = nullptr;
    QToolButton* a_showMeshButton = nullptr;
    QToolButton* a_showCurrentsButton = nullptr;
    QToolButton* a_solveButton = nullptr;
    QLabel* a_layoutLabel = nullptr;
    QSplitter* a_splitter = nullptr;
    QSplitter* a_sides = nullptr;
    qucs_s::rf25::StackView* a_stack = nullptr;
    qucs_s::layout::LayoutView* a_view = nullptr;
    QPlainTextEdit* a_problemsText = nullptr;
    QLabel* a_status = nullptr;
    QProgressBar* a_progressBar = nullptr;
    QToolButton* a_progressStop = nullptr;
};

#endif // QUCS_RF25DOC_H
