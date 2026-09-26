/*
 * qucscontrol.h - the Qucs-S window as tools for Claude
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_QUCSCONTROL_H
#define QUCS_QUCSCONTROL_H

#include "claudecode.h"

#include <QHash>
#include <QJsonArray>
#include <QObject>
#include <QPointer>

class Component;
class QAction;
class QucsApp;
class QucsDoc;
class Schematic;
class QWidget;

/*!
 * The Qucs-S window as tools for Claude (qucs_s::claude::ToolHost, an MCP
 * server named "qucs" that the Claude Code dock's sessions offer): the
 * documents - their state, opening, showing, saving, closing, new ones;
 * a schematic read as a list of its parts and their pins or as its file's
 * text, and changed as text or part by part (add, edit, connect, wire,
 * label, delete), each change one step to undo; a picture of it; the
 * menus' actions and the dialogs they open, read, filled in and closed;
 * a simulation run and its outcome - its errors each with its netlist
 * line and part - the netlist, the dataset read as numbers and measured,
 * and the diagrams that show it, made and changed by their named fields.
 *
 * Everything is done as the user would do it, in the window in front of
 * them: a document being changed comes to the front, and what goes wrong
 * is told to Claude (misc::ErrorCapture) rather than put in a message box.
 */
class QucsControl : public QObject, public qucs_s::claude::ToolHost
{
    Q_OBJECT

public:
    explicit QucsControl(QucsApp* app);

    QString serverName() const override { return QStringLiteral("qucs"); }
    QJsonArray tools() const override;
    QStringList readOnlyTools() const override;
    QString actionOf(const QString& tool) const override;
    QString subjectOf(const QString& tool, const QJsonObject& arguments) const override;
    QString instructions() const override;
    void callTool(const QString& tool, const QJsonObject& arguments,
                  std::function<void(const QJsonObject&)> done) override;
    /// The tools that take the document they act on as 'path' (the one in
    /// front when not given) are given \a document, and get_state names
    /// it; open_document and show_document name theirs, reload_data
    /// without one reads every document's data.
    QJsonObject forDocument(const QString& tool, const QJsonObject& arguments, const QString& document) const override;

    /// The result of a call, the event loop run until it comes (for the
    /// tests); an error result after \a timeoutMs.
    QJsonObject callNow(const QString& tool, const QJsonObject& arguments, int timeoutMs = 30000);
    /// The text of a result (its text parts, joined).
    static QString textOf(const QJsonObject& result);

private:
    using Done = std::function<void(const QJsonObject&)>;

    QJsonObject call(const QString& tool, const QJsonObject& args, const Done& done, bool& async);

    // Documents.
    QJsonObject getState(const QJsonObject& args);
    QJsonObject openDocument(const QJsonObject& args);
    QJsonObject newDocument(const QJsonObject& args);
    QJsonObject showDocument(const QJsonObject& args);
    QJsonObject saveDocument(const QJsonObject& args);
    QJsonObject closeDocument(const QJsonObject& args);
    // A schematic.
    QJsonObject getSchematic(const QJsonObject& args);
    QJsonObject setSchematic(const QJsonObject& args);
    QJsonObject addComponent(const QJsonObject& args);
    QJsonObject editComponent(const QJsonObject& args);
    QJsonObject remove(const QJsonObject& args);
    QJsonObject connectPins(const QJsonObject& args);
    QJsonObject addWire(const QJsonObject& args);
    QJsonObject setLabel(const QJsonObject& args);
    QJsonObject select(const QJsonObject& args);
    QJsonObject zoom(const QJsonObject& args);
    QJsonObject undoRedo(const QJsonObject& args, bool redo);
    QJsonObject screenshot(const QJsonObject& args);
    QJsonObject listComponentTypes(const QJsonObject& args);
    // Menus and dialogs.
    QJsonObject listActions(const QJsonObject& args);
    void triggerAction(const QJsonObject& args, const Done& done);
    /// batch: the calls one after another (each may answer later), their
    /// results together.
    void runBatch(const QJsonObject& args, const Done& done);
    QJsonObject getDialog();
    void setDialog(const QJsonObject& args, const Done& done);
    // Simulation and its results.
    void simulate(const QJsonObject& args, const Done& done);
    QJsonObject getNetlist(const QJsonObject& args);
    QJsonObject getDataset(const QJsonObject& args);
    QJsonObject reloadData(const QJsonObject& args);
    // Diagrams and their traces.
    QJsonObject addDiagram(const QJsonObject& args);
    QJsonObject editDiagram(const QJsonObject& args);
    QJsonObject addTrace(const QJsonObject& args);
    QJsonObject editTrace(const QJsonObject& args);
    QJsonObject renameNet(const QJsonObject& args);
    QJsonObject describeComponentType(const QJsonObject& args);

    /// The dataset get_dataset reads for \a args: a dataset file, or the
    /// one of a schematic or data display (open or not) for a simulator.
    QString datasetPath(const QJsonObject& args, QString* error) const;
    /// The open documents that show \a sch's dataset: itself and its data
    /// displays.
    QList<Schematic*> showingDataOf(Schematic* sch) const;
    QucsDoc* document(const QJsonObject& args, QString* error) const;
    Schematic* schematic(const QJsonObject& args, QString* error, bool forChange) const;
    /// The document in front, the property editor closed: before a change.
    void prepare(Schematic* sch);
    /// A change made: one step to undo, drawn - and \a where in sight,
    /// the view moved to it when it is out of it.
    void finish(Schematic* sch, const QList<QPoint>& where = {});
    /// "R1.1", "R1.out", [x, y], {"x":..,"y":..}: a place in \a sch.
    bool pointOf(Schematic* sch, const QJsonValue& at, QPoint* point, QString* error) const;
    QString titleOf(QucsDoc* doc) const;
    QList<QAction*> menuActions(QStringList* paths) const;
    QWidget* openDialog() const;
    /// The controls of \a dialog that get_dialog tells of, in a fixed order.
    QList<QWidget*> dialogControls(QWidget* dialog) const;

    QucsApp* a_app;
    QJsonArray a_tools;
    QHash<QString, QString> a_actions;   // tool -> "add a component"
    QStringList a_readOnly;
};

#endif // QUCS_QUCSCONTROL_H
