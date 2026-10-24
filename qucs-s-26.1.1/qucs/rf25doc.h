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
#include "rf25/rf25_inductance.h"
#include "rf25/rf25_layout.h"
#include "rf25/rf25_mesh.h"
#include "rf25/rf25_static.h"
#include "rf25/rf25_model.h"

#include <QFrame>
#include <QWidget>

#include <atomic>
#include <functional>
#include <memory>

class QLabel;
class QPlainTextEdit;
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
 * Nothing is solved in this build - phase A of
 * docs/proposals/2026-10-23-rf-2p5d-moment-method.md is the document, the
 * stack and the layout -, so the work it does in the background is reading
 * the layout file the model names and then making of it what the solver
 * would be given: the shapes processed and divided into cells, with the
 * unknowns counted. A large GDSII file never holds the window, and
 * isBusy() is true while either runs.
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
    /// solved, or the model changed since).
    std::shared_ptr<const qucs_s::rf25::Extraction> extraction() const { return a_extraction; }
    /// Solves it, in the background, on the shapes and the mesh as they
    /// stand. A solve going on is let go. Nothing happens where there is
    /// no mesh to solve on; solveProblem() then says why.
    void solve();
    /// Lets a solve going on go: its result is dropped.
    void stopSolving();
    bool isSolving() const { return a_solveJob != nullptr; }
    /// Waits for it, \a ms at most; whether it ended.
    bool waitForSolved(int ms = 600000);
    /// Why there is no capacitance: what the extraction refused, or what
    /// stood in the way of starting it. Empty when there is one.
    QString solveProblem() const { return a_solveProblem; }
    /// What the solve said it did, for the Results node and the notes.
    QStringList solvedLines() const;
    /// Each net as a line: its capacitance per metre, its effective
    /// permittivity and its impedance over the length that was solved.
    /// Empty where nothing was solved.
    QStringList lineSummary() const;
    /// The length a line's parameters are taken over, m: the longer side
    /// of what was solved, which is what a straight line's length is.
    /// Zero where nothing was solved.
    double solvedLength() const;
    QToolButton* solveButton() const { return a_solveButton; }
    /// Each net's S-parameters at the top of the study's sweep, and where
    /// a quasi-static answer stops being the whole story. Empty where
    /// nothing was solved.
    QStringList sParameterSummary() const;
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
        bool coupled = false;   ///< every net as a 2N-port, rather than one as a two-port
        QString net;            ///< which net, for a two-port (empty: the first)
        double length = 0;      ///< m (0: the length that was solved)
        double impedance = 50;  ///< the ports' reference, Ohm
    };
    /// The Touchstone file \a what asks for, as text. Empty and why in
    /// \a error where it cannot be made.
    QString exportText(const ExportRequest& what, QString* error = nullptr) const;
    /// The export node \a tag read into a request. False and why in
    /// \a error where there is no such node or it says something that is
    /// no number.
    bool exportRequest(const QString& tag, ExportRequest* what, QString* error = nullptr) const;
    /// The Touchstone file the export node \a tag asks for, as text.
    QString exportText(const QString& tag, QString* error = nullptr) const;
    /// That text written to the file the node names - or, where it names
    /// none, to model_<tag>.sNp beside the model. The path written, or
    /// empty and why in \a error.
    QString writeExport(const QString& tag, QString* error = nullptr) const;
    /// Where writeExport() would put it: the node's file, made absolute
    /// against the model, or the default beside it. Empty where there is
    /// no such node.
    QString exportPath(const QString& tag) const;
    /// Whether the processed shapes are drawn over the layout, and whether
    /// the mesh is. Kept for the next model opened.
    bool processedShown() const { return a_showProcessed; }
    void setProcessedShown(bool shown);
    bool meshShown() const { return a_showMesh; }
    void setMeshShown(bool shown);
    QToolButton* processedButton() const { return a_showProcessedButton; }
    QToolButton* meshButton() const { return a_showMeshButton; }

    // What is shown.
    qucs_s::layout::LayoutView* view() const { return a_view; }
    qucs_s::rf25::StackView* stackView() const { return a_stack; }
    QPlainTextEdit* problemsText() const { return a_problemsText; }
    QLabel* statusLabel() const { return a_status; }
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
    struct SolveJob;
    void buildUi();
    void prepareDone();
    void solveDone();
    /// The processed shapes and the mesh over the layout, as the two
    /// buttons have them.
    void showOverlays();
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
    std::shared_ptr<const qucs_s::rf25::Mesh> a_mesh;
    std::shared_ptr<SolveJob> a_solveJob;
    std::shared_ptr<const qucs_s::rf25::Extraction> a_extraction;
    std::shared_ptr<const qucs_s::rf25::Inductance> a_inductance;
    QString a_portsSaid;
    QString a_solveProblem;
    double a_solvedLength = 0;
    QTimer* a_prepareSoon = nullptr;
    bool a_showProcessed = true;
    bool a_showMesh = false;

    QToolButton* a_fit = nullptr;
    QToolButton* a_reread = nullptr;
    QToolButton* a_showProcessedButton = nullptr;
    QToolButton* a_showMeshButton = nullptr;
    QToolButton* a_solveButton = nullptr;
    QLabel* a_layoutLabel = nullptr;
    QSplitter* a_splitter = nullptr;
    QSplitter* a_sides = nullptr;
    qucs_s::rf25::StackView* a_stack = nullptr;
    qucs_s::layout::LayoutView* a_view = nullptr;
    QPlainTextEdit* a_problemsText = nullptr;
    QLabel* a_status = nullptr;
};

#endif // QUCS_RF25DOC_H
