/*
 * multiphysics3ddoc.h - a 3D multiphysics model (.qfem3d) in a tab of
 *                       Qucs-S: its geometry and swept mesh, built in the
 *                       background, drawn in a 3D view; each change one step
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
#include "qucsdoc.h"
#include "view3d.h"

#include <QFrame>

#include <functional>
#include <memory>

class QComboBox;
class QFileSystemWatcher;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QSlider;
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
    /// Lets the work going on go.
    void cancel();
    bool isBusy() const { return a_job != nullptr; }
    /// Waits for the work going on, and a build waiting to start, \a ms at
    /// most; whether it ended.
    bool waitForIdle(int ms = 120000);

    // What is shown.
    enum class Show { Geometry, Mesh };
    Show show() const { return a_show; }
    void showGeometry();
    void showMesh();
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
    void startJob(bool mesh);
    void jobFinished();
    void updateScene(bool fit = false);
    void updateHighlight();
    void updateState();
    void watchFiles();

    qucs_s::fem::Model a_model;
    QUndoStack* a_undo = nullptr;
    QString a_readNotes;
    mutable std::shared_ptr<qucs_s::fem::ParameterScope> a_parameters;
    mutable quint64 a_parametersRevision = 0;
    quint64 a_revision = 1;

    qucs_s::fem::GeometryBuild3 a_geometry;
    bool a_geometryStale = true;
    std::shared_ptr<const qucs_s::fem::Mesh3> a_mesh;
    bool a_meshStale = true;
    bool a_firstShown = false;
    std::shared_ptr<Job> a_job;
    bool a_geometryWanted = false;   // a build asked while another worked
    bool a_meshWanted = false;
    bool a_closed = false;

    Show a_show = Show::Geometry;
    QString a_focus;
    qucs_s::fem::Level a_highlightLevel = qucs_s::fem::Level::None;
    QVector<int> a_highlighted;
    QStringList a_messages;

    View3D* a_view = nullptr;
    QToolButton* a_buildGeometry = nullptr;
    QToolButton* a_buildMesh = nullptr;
    QComboBox* a_showBox = nullptr;
    QComboBox* a_clipBox = nullptr;
    QSlider* a_clipSlider = nullptr;
    QProgressBar* a_progress = nullptr;
    QToolButton* a_cancel = nullptr;
    QLabel* a_stage = nullptr;
    QPlainTextEdit* a_notes = nullptr;
    QTimer* a_geometryTimer = nullptr;
    QTimer* a_progressTimer = nullptr;
    QFileSystemWatcher* a_watcher = nullptr;
};

#endif // QUCS_MULTIPHYSICS3DDOC_H
