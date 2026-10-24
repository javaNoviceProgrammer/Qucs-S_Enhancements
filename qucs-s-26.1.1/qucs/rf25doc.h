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
#include "rf25/rf25_model.h"

#include <QFrame>
#include <QWidget>

#include <atomic>
#include <functional>
#include <memory>

class QLabel;
class QPlainTextEdit;
class QSplitter;
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
 * stack and the layout -, so the only work it does in the background is
 * reading the layout file the model names: a large GDSII file never holds
 * the window, and isBusy() is true while it is read.
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
    /// Its layout being read: nothing else runs in this build.
    bool isBusy() const { return a_job != nullptr; }
    /// Waits for that read, \a ms at most; whether it ended.
    bool waitForLayout(int ms = 120000);

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

    /// A new model's content (File > New, the panel's New Model).
    static qucs_s::rf25::Model starterModel();

signals:
    void signalFileChanged(bool);
    /// The model changed (an edit, an undo, a reload).
    void modelChanged();
    /// The layout read, or being read; a file saved.
    void stateChanged();
    void focusChanged(const QString& tag);

private:
    struct Job;
    void buildUi();
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

    QToolButton* a_fit = nullptr;
    QToolButton* a_reread = nullptr;
    QLabel* a_layoutLabel = nullptr;
    QSplitter* a_splitter = nullptr;
    QSplitter* a_sides = nullptr;
    qucs_s::rf25::StackView* a_stack = nullptr;
    qucs_s::layout::LayoutView* a_view = nullptr;
    QPlainTextEdit* a_problemsText = nullptr;
    QLabel* a_status = nullptr;
};

#endif // QUCS_RF25DOC_H
