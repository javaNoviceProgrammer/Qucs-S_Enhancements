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

#include <list>

class Component;
class Painting;
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
    /// A call by the conversation \a caller: its result also says what
    /// changed since that conversation's last call that it did not change
    /// itself (the user's edits, another conversation's, a simulation the
    /// user ran, documents opened or closed); edits the call makes are
    /// the conversation's (QucsDoc::editor()).
    void callToolFor(quint64 caller, const QString& tool, const QJsonObject& arguments,
                     std::function<void(const QJsonObject&)> done) override;
    /// The tools that take the document they act on as 'path' (the one in
    /// front when not given) are given \a document, and get_state names
    /// it; open_document and show_document name theirs, reload_data
    /// without one reads every document's data.
    QJsonObject forDocument(const QString& tool, const QJsonObject& arguments, const QString& document) const override;

    /// The result of a call, the event loop run until it comes (for the
    /// tests); an error result after \a timeoutMs.
    QJsonObject callNow(const QString& tool, const QJsonObject& arguments, int timeoutMs = 30000, quint64 caller = 0);
    /// The text of a result (its text parts, joined).
    static QString textOf(const QJsonObject& result);

private:
    using Done = std::function<void(const QJsonObject&)>;

    // What each conversation saw of each document at the end of its last
    // call: its revision, and when its dataset was written.
    struct Seen {
        quint64 revision = 0;
        QDateTime dataset;
        QString state;   // a schematic as it was (snapshot()): what changed since is told part by part
    };
    QHash<quint64, QHash<QString, Seen>> a_seen;   // by conversation, by document (seenKey())
    QList<quint64> a_callers;   // the conversations whose calls run now: the last edits
    static QString seenKey(QucsDoc* doc);
    /// When the dataset a simulation of \a doc writes was written, if it is.
    static QDateTime datasetWritten(QucsDoc* doc);
    /// What changed since \a caller's last call that it did not change:
    /// a line for each document.
    QStringList changesSince(quint64 caller) const;
    void noteSeen(quint64 caller);
    /// Who made an edit, as \a caller is told: "you", "the user",
    /// "another conversation".
    QString whoMade(quint64 by, quint64 caller) const;

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
    QJsonObject checkSchematic(const QJsonObject& args);
    QJsonObject addComponent(const QJsonObject& args);
    QJsonObject editComponent(const QJsonObject& args);
    QJsonObject remove(const QJsonObject& args);
    QJsonObject moveGroup(const QJsonObject& args);
    QJsonObject addAnalysis(const QJsonObject& args);
    QJsonObject createSubcircuit(const QJsonObject& args);
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
    void buildVerilogA(const QJsonObject& args, const Done& done);
    void tune(const QJsonObject& args, const Done& done);
    QJsonObject readPdf(const QJsonObject& args);
    QJsonObject undoHistory(const QJsonObject& args);
    QJsonObject newProject(const QJsonObject& args);
    QJsonObject openProject(const QJsonObject& args);
    QJsonObject copyDocument(const QJsonObject& args);
    QJsonObject cleanScratch(const QJsonObject& args);
    QJsonObject makeSymbol(const QJsonObject& args);
    QJsonObject importNetlist(const QJsonObject& args);
    QJsonObject findLibraryComponent(const QJsonObject& args);
    QJsonObject datasetOfRun(Schematic* doc, int simulator, const QDateTime& started, const QString& keepAs, bool* written);
    QJsonObject getNetlist(const QJsonObject& args);
    QJsonObject getDataset(const QJsonObject& args);
    QJsonObject reloadData(const QJsonObject& args);
    // Diagrams and their traces.
    QJsonObject addDiagram(const QJsonObject& args);
    QJsonObject editDiagram(const QJsonObject& args);
    QJsonObject addTrace(const QJsonObject& args);
    QJsonObject editTrace(const QJsonObject& args);
    QJsonObject addMarker(const QJsonObject& args);
    QJsonObject moveToPane(const QJsonObject& args);
    QJsonObject describeFormat(const QJsonObject& args);
    QJsonObject editMarker(const QJsonObject& args);
    QJsonObject deleteMarker(const QJsonObject& args);
    QJsonObject renameNet(const QJsonObject& args);
    /// What names a net or a part - the traces of the schematic's diagrams
    /// and of its data displays (a closed one's file rewritten), its
    /// equations - passed through \a rename; what changed told.
    void renameEverywhere(Schematic* sch, const std::function<QString(const QString&)>& rename, QStringList* changed,
                          QString* rewritten);
    // Paintings, of the schematic or its symbol.
    QJsonObject addPainting(const QJsonObject& args);
    QJsonObject editPainting(const QJsonObject& args);
    // The workspace, pictures, the simulator.
    QJsonObject listDocuments(const QJsonObject& args);
    QJsonObject exportImage(const QJsonObject& args);
    QJsonObject setSimulator(const QJsonObject& args);
    /// The document \a args name and the paintings they are about: its
    /// symbol's when 'symbol' says so or it shows its symbol (switched to
    /// what is asked for, which \a note then says), else its schematic's.
    Schematic* paintingsOf(const QJsonObject& args, std::list<Painting*>** list, QString* note, QString* error);
    QJsonObject describeComponentType(const QJsonObject& args);

    /// The dataset get_dataset reads for \a args: a dataset file, or the
    /// one of a schematic or data display (open or not) for a simulator.
    QString datasetPath(const QJsonObject& args, QString* error) const;
    /// The open schematic whose simulations write the dataset \a file (the
    /// document \a args name first), or nullptr.
    Schematic* schematicOfDataset(const QString& file, const QJsonObject& args) const;
    /// The open documents that show \a sch's dataset: itself and its data
    /// displays.
    QList<Schematic*> showingDataOf(Schematic* sch) const;
    QucsDoc* document(const QJsonObject& args, QString* error) const;
    /// \a args of \a tool with the user's selection in them ("selection":
    /// true): its parts' names, its diagrams, paintings and wires - or, for
    /// add_painting ("around": "selection"), a place about it.
    QJsonObject withSelection(const QString& tool, const QJsonObject& args, QString* error) const;
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
