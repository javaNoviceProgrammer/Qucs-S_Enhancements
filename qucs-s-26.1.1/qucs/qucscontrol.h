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

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QObject>
#include <QPoint>
#include <QPointer>

#include <list>
#include <optional>

class Component;
class Painting;
class QAction;
class QucsApp;
class QucsDoc;
class Schematic;
class TextDoc;
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
    /// As callToolFor(); \a waited: one that waited its turn, run now.
    void callToolFor(quint64 caller, const QString& tool, const QJsonObject& arguments,
                     std::function<void(const QJsonObject&)> done, bool waited);
    /// The tools that take the document they act on as 'path' (the one in
    /// front when not given) are given \a document, and get_state names
    /// it; open_document and show_document name theirs, reload_data
    /// without one reads every document's data.
    QJsonObject forDocument(const QString& tool, const QJsonObject& arguments, const QString& document) const override;
    /// MCP resources (qucscontrol_resources.cpp): qucs://state, and of
    /// each open schematic qucs://schematic/<path>, netlist, netlist-map
    /// and dataset (<path> its file's, percent-encoded); a document's
    /// resources change with its revision, a dataset when it is written.
    QJsonArray resources() const override;
    QJsonArray resourceTemplates() const override;
    QJsonArray readResource(const QString& uri, QString* error) override;
    QString resourceVersion(const QString& uri) const override;
    /// Files deleted or written over, unsaved changes discarded.
    bool irreversible(const QString& tool, const QJsonObject& arguments) const override;
    QString askedEachTime(const QString& tool, const QJsonObject& arguments) const override;
    void setAsker(quint64 caller, Asker asker) override;

    /// The result of a call, the event loop run until it comes (for the
    /// tests); an error result after \a timeoutMs.
    QJsonObject callNow(const QString& tool, const QJsonObject& arguments, int timeoutMs = 30000, quint64 caller = 0);
    /// The text of a result (its text parts, joined).
    static QString textOf(const QJsonObject& result);
    /// Whether run_script is in this build (Qt's Qml module found).
    static bool scriptingBuilt();
    /// Something every conversation is told with its next call's result
    /// ("Since your last call: ..."), for half an hour: a file Claude
    /// changed on disk that was not loaded again, say.
    void noteForConversations(const QString& text);
    /// Served with no window (qucs-s --mcp-server): no one answers a
    /// message box at all.
    void setHeadless(bool headless) { a_headless = headless; }

protected:
    /// A message box that opens while a tool call runs (one no one would
    /// answer until the call ends - and under --mcp-server no one at all)
    /// is closed with its safe button (No, Cancel, OK), and what it said
    /// goes into the call's answer.
    bool eventFilter(QObject* watched, QEvent* event) override;

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
    // What a tool did beside what was asked (a document switched from its
    // symbol to its schematic): told after its result.
    mutable QStringList a_callNotes;
    // How each conversation asks its user (its MCP server's elicitation).
    QHash<quint64, Asker> a_askers;
    // Each tool as it was written, its description in full (describe_tool).
    QHash<QString, QJsonObject> a_details;
    QHash<QString, QString> a_summaries;   // each tool's summary (the core tools' list description)
    /// Asks the user of the conversation whose call runs (MCP elicitation):
    /// its answer, {action: accept|decline|cancel, content}; cancel when
    /// it cannot be asked.
    QJsonObject askUser(const QString& message, const QJsonObject& schema);
    /// The user asked yes or no (true only for a yes).
    bool confirmed(const QString& question);
    /// The user asked to choose one of \a options (empty: declined, or
    /// cannot be asked).
    QString choice(const QString& question, const QStringList& options);
    bool resourceOf(const QString& uri, QString* kind, QucsDoc** doc, QString* error) const;
    /// \a tool run with \a args (their 'preview' taken out) and every open
    /// schematic put back after: what it would change, and its answer.
    QJsonObject preview(const QString& tool, const QJsonObject& args, const Done& done, bool& async);
    // A preview under way, and the files its tool wrote (create_subcircuit's):
    // each with what it held, or none when it was not there - put back after.
    int a_previewing = 0;
    QList<QPair<QString, std::optional<QByteArray>>> a_previewFiles;
    /// Each open document's revision and whether it had unsaved changes,
    /// while a preview runs: what a subscriber to its resource reads.
    QHash<const QucsDoc*, QPair<quint64, bool>> a_revisionsKept;
    /// \a file, which held \a before (or was not there), is written: a
    /// preview puts it back.
    void written(const QString& file, const std::optional<QByteArray>& before);
    /// The files the tools wrote, call by call - each as it was before,
    /// and (a hash of) what the call left - for undo's 'files' to put back.
    struct FileStep {
        QString tool;
        QDateTime when;
        QList<QPair<QString, std::optional<QByteArray>>> before;
        QHash<QString, QByteArray> after;   // (empty: not there after)
        QList<QPair<QString, QString>> moved;   // (from, to): renamed, moved or trashed - put back by moving it
    };
    QList<FileStep> a_fileSteps;
    FileStep a_openStep;   // the call under way's
    int a_callDepth = 0;   // (a call inside another - import_netlist's save - is part of it)
    /// \a file is about to be written by the call under way: as it is, kept
    /// (once a call).
    void aboutToWrite(const QString& file);
    /// \a from was moved to \a to (renamed, or to the trash) by the call
    /// under way: undo's 'files' moves it back.
    void movedFile(const QString& from, const QString& to);
    void openFileStep(const QString& tool);
    void closeFileStep();
    /// undo's 'files': the last \a steps calls' files put back.
    QJsonObject undoFiles(int steps);
    /// diff: a schematic against steps back, another file, or its file.
    QJsonObject diffTool(const QJsonObject& args);
    /// An untitled schematic, to be simulated: saved in the scratch folder
    /// of the project (or of the workspace). What was done in \a note;
    /// false, and why in \a error, when it could not be.
    bool saveInScratch(Schematic* sch, QString* note, QString* error);
    /// Closes the untitled schematics nothing was ever done in (the one
    /// Qucs-S opens at start) but \a keep, which is then in front.
    void closeUntouched(QucsDoc* keep);
    /// connect with "ground" at one end: a ground symbol of the pin's own.
    QJsonObject groundPin(Schematic* sch, const QJsonValue& pin);
    static QString stateOfText(const QString& text);
    static const QSet<QString>& previewTools();
    /// run_script (qucscontrol_script.cpp): with Qt's JavaScript engine,
    /// when the build has it.
    QJsonObject runScript(const QJsonObject& args);
    static QString seenKey(QucsDoc* doc);
    /// When the dataset a simulation of \a doc writes was written, if it is.
    static QDateTime datasetWritten(QucsDoc* doc);
    /// What changed since \a caller's last call that it did not change:
    /// a line for each document.
    QStringList changesSince(quint64 caller) const;
    void noteSeen(quint64 caller);
    /// The notes for everyone (noteForConversations()) \a caller has not
    /// been told yet.
    QStringList notesFor(quint64 caller);
    struct Broadcast {
        qint64 serial;
        QDateTime at;
        QString text;
    };
    QList<Broadcast> a_broadcasts;
    qint64 a_broadcastSerial = 0;
    QHash<quint64, qint64> a_broadcastSeen;   // each conversation: the last it was told
    /// Who made an edit, as \a caller is told: "you", "the user",
    /// "another conversation".
    QString whoMade(quint64 by, quint64 caller) const;

    QJsonObject call(const QString& tool, const QJsonObject& args, const Done& done, bool& async);

    // A call that runs over more than one turn of the event loop and puts
    // schematics back, or must not have others' changes land in its midst -
    // a batch, a script, tune, a preview - runs alone: calls that come
    // meanwhile (pipelined by the client, another conversation's) wait, and
    // run in turn after it. Else a preview's rollback took their changes
    // with it, and a batch's calls followed a document another call closed.
    static bool runsAlone(const QString& tool, const QJsonObject& arguments);
    struct Waiting {
        quint64 caller;
        QString tool;
        QJsonObject arguments;
        Done done;
    };
    QList<Waiting> a_waiting;
    int a_alone = 0;   // calls that run alone, under way
    void runWaiting();
    bool a_headless = false;
    // Event loops a call runs while it waits (a script's call of a
    // simulation, a question to the user): a box the user opens meanwhile
    // is theirs, not the call's.
    int a_spinning = 0;

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
    QJsonObject replaceComponent(const QJsonObject& args);
    QJsonObject remove(const QJsonObject& args);
    QJsonObject moveGroup(const QJsonObject& args);
    /// arrange: the schematic laid out again by signal flow, every net kept.
    QJsonObject arrange(const QJsonObject& args);
    QJsonObject addAnalysis(const QJsonObject& args);
    /// \a expressions (db(v(out))) as variables of a NutmegEq run after the
    /// analysis \a simulated: an equation of one there is, else of a new
    /// NutmegEq beside the part \a beside (one step to undo; \a made its
    /// answer). \a variableOf: each expression's variable. False, and why.
    bool nutmegVariables(Schematic* sch, const QString& beside, const QString& simulated, const QStringList& expressions,
                         const QJsonValue& path, QHash<QString, QString>* variableOf, QJsonObject* made, QString* error);
    /// A trace's variable that is an expression (ac.db(v(out)), v(out)/2):
    /// the name, with its simulator and analysis, of a NutmegEq's variable
    /// that computes it (made unless \a dryRun, and said in \a note). Empty,
    /// \a error empty too, when \a wanted is no expression; refused (why in
    /// \a error) under a simulator with no Nutmeg, or when it does not read.
    QString expressionTrace(Schematic* sch, const QString& wanted, const QJsonValue& path, bool dryRun, QString* note, QString* error);
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
    // A part of the window (qucscontrol_ui.cpp): docks, toolbars, the
    // status bar, the documents' tabs; right-click menus.
    QJsonObject getUi(const QJsonObject& args);
    void setUi(const QJsonObject& args, const Done& done);
    void contextMenu(const QJsonObject& args, const Done& done);
    // A console dock typed into (qucscontrol_console.cpp).
    void console(const QJsonObject& args, const Done& done);
    /// The part of the window \a area names, and what it is called; null
    /// and why when there is none, or it is not Claude's (the Claude Code
    /// panel). \a forChange: also not a console's (typing runs commands).
    QWidget* uiArea(const QString& area, QString* name, QString* error, bool forChange) const;
    /// The parts of the window get_ui reads.
    QJsonArray uiAreas() const;
    // Settings, typed (qucscontrol_settings.cpp): a settings dialog opened
    // as its menu action opens it, read or set by keys "Tab/Label".
    void getSettings(const QJsonObject& args, const Done& done);
    void setSettings(const QJsonObject& args, const Done& done);
    /// The settings dialog of args' 'scope', made but not shown (to read).
    QWidget* settingsDialogFor(const QJsonObject& args, QString* error);
    /// Opens the settings dialog of args' 'scope' as its action does, and
    /// hands it to \a with once it is up; done() with why when it is not.
    void withSettingsDialog(const QJsonObject& args, std::function<void(QWidget* dialog)> with, const Done& done);
    /// A settings dialog's settings: each control by its key ("Tab/Label"),
    /// its type, value and what it takes. \a byKey: the controls by key.
    QJsonArray typedSettings(QWidget* dialog, QHash<QString, QWidget*>* byKey) const;
    /// A control's value, typed (text, true or false, a number).
    QJsonValue typedValue(QWidget* w) const;
    /// Sets a control of \a dialog as typed; false and why.
    bool setTyped(QWidget* dialog, QWidget* w, const QJsonValue& value, QString* why);
    /// Where on \a sch's canvas context_menu's \a on points ('part',
    /// 'diagram' or 'canvas'), in the schematic's coordinates, and what is
    /// there; false and why.
    bool canvasPoint(Schematic* sch, const QJsonObject& on, QPoint* point, QString* what, QString* error) const;
    // Simulation and its results.
    void simulate(const QJsonObject& args, const Done& given);
    // Simulations followed after their call has answered
    // (qucscontrol_watch.cpp): one simulate began with 'background', or one
    // that ran past its 'timeout' - each by its id, its outcome kept once it
    // has ended.
    struct SimRun {
        int id = 0;
        QString schematic;
        QString simulator;
        QDateTime began, ended;    // (ended: not valid while it runs)
        QJsonObject result;        // simulate's answer, once it has ended
        QPointer<QObject> process;   // its SimulationRun, or Qucsator's SimMessage
        QString stoppedBy;         // stop_simulation, or its timeout
    };
    QList<SimRun> a_simRuns;
    int a_simRunSerial = 0;
    int beginSimRun(const QString& schematic, const QString& simulator);
    void endSimRun(int id, const QJsonObject& result);
    SimRun* simRun(int id);
    /// A run's process stopped (its SimulationRun's stop, Qucsator's abort).
    static void stopProcess(QObject* process);
    QJsonObject simulationStatus(const QJsonObject& args);
    void stopSimulation(const QJsonObject& args, const Done& done);
    /// wait_for: done once the event has happened, or at its timeout.
    void waitFor(const QJsonObject& args, const Done& done);
    // Raw input (qucscontrol_input.cpp): a click, a drag, keys.
    void sendInput(const QJsonObject& args, const Done& done);
    /// Why Claude does not use \a action (File > Exit, ...); empty when it may.
    QString refusedAction(QAction* action) const;
    // The help this build has (qucscontrol_help.cpp).
    QJsonObject readHelp(const QJsonObject& args);
    void buildVerilogA(const QJsonObject& args, const Done& done);
    void tune(const QJsonObject& args, const Done& done);
    /// tune with several knobs, for as many targets.
    void tuneKnobs(const QJsonObject& args, const Done& done);
    /// What \a spec (tune's 'measure') measures after a run: of the
    /// operating point in \a simulated (simulate's answer), or read from the
    /// dataset of \a path. NaN, with \a why, when it cannot be.
    double measureRun(const QJsonObject& spec, const QJsonObject& simulated, const QString& path, const QJsonObject& args,
                      QString* used, QString* why);
    QJsonObject readPdf(const QJsonObject& args);
    QJsonObject undoHistory(const QJsonObject& args);
    QJsonObject newProject(const QJsonObject& args);
    QJsonObject openProject(const QJsonObject& args);
    QJsonObject copyDocument(const QJsonObject& args);
    QJsonObject cleanScratch(const QJsonObject& args);
    QJsonObject renameFile(const QJsonObject& args);
    QJsonObject getText(const QJsonObject& args);
    QJsonObject editText(const QJsonObject& args);
    QJsonObject gotoLine(const QJsonObject& args);
    /// The text document \a args names ('path'; the one in front unless
    /// given), or null and why in \a error.
    TextDoc* textDocument(const QJsonObject& args, QString* error) const;
    QJsonObject trashFile(const QJsonObject& args);
    QJsonObject makeSymbol(const QJsonObject& args);
    QJsonObject setSubcircuitParameters(const QJsonObject& args);
    QJsonObject importNetlist(const QJsonObject& args);
    QJsonObject importData(const QJsonObject& args);
    QJsonObject exportData(const QJsonObject& args);
    QJsonObject findLibraryComponent(const QJsonObject& args);
    QJsonObject describePart(const QJsonObject& args);
    /// What a run of \a doc wrote: its dataset - written when it is newer than
    /// \a before (its time before the run; invalid when there was none) -
    /// its variables, the copy \a keepAs, and the traces left blank.
    /// \a compare's measurements ({"with": a kept run, "measure": [...]})
    /// on the run just made and on that one: a table of before, after and
    /// the change.
    QJsonObject comparedRuns(Schematic* doc, const QJsonObject& compare, const QJsonValue& simulator);
    QJsonObject datasetOfRun(Schematic* doc, int simulator, const QDateTime& before, const QString& keepAs, bool* written);
    QJsonObject getNetlist(const QJsonObject& args);
    QJsonObject getDataset(const QJsonObject& args);
    /// Why \a args has arguments \a tool does not take (its schema's
    /// fields), and the ones it does; empty when it has none.
    QString unknownArguments(const QString& tool, const QJsonObject& args) const;
    /// The first argument of \a args (inside them too) whose JSON type is
    /// not its schema's - 1 for a boolean, an object for a list - said, or
    /// empty: it was read as its default, silently. A null is not given.
    QString wrongTypes(const QString& tool, const QJsonObject& args) const;
    /// Why \a file, the dataset of \a sch, is not of the circuit as it is -
    /// the last run failed after it, the netlist a run would be given now is
    /// not the one it ran, or an edit after it - or empty. \a certain: the
    /// first two, not the edit's time alone (which may be of no value).
    QString staleness(Schematic* sch, const QString& file, bool* certain = nullptr);
    QJsonObject reloadData(const QJsonObject& args);
    // Diagrams and their traces.
    QJsonObject addDiagram(const QJsonObject& args);
    QJsonObject editDiagram(const QJsonObject& args);
    QJsonObject addTrace(const QJsonObject& args);
    QJsonObject editTrace(const QJsonObject& args);
    QJsonObject addMarker(const QJsonObject& args);
    QJsonObject moveToPane(const QJsonObject& args);
    QJsonObject describeFormat(const QJsonObject& args);
    /// ngspice's commands, summed up by category - and which the ngspice
    /// of the settings has (qucscontrol_ngspice.cpp).
    QJsonObject ngspiceCommands(const QJsonObject& args);
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
    /// The dataset beside \a args' document that \a wanted names when each
    /// is name:variable of one dataset (m:gain imported, ngspice/run1:v(out)
    /// kept); empty when they are not, and why in \a error when they are
    /// but cannot be read (of two datasets, none of that name). \a tool is
    /// the one that reads them.
    QString datasetNamedBy(const QJsonObject& args, const QJsonArray& wanted, const QString& tool, QString* error) const;
    /// The open schematic whose simulations write the dataset \a file (the
    /// document \a args name first), or nullptr.
    Schematic* schematicOfDataset(const QString& file, const QJsonObject& args) const;
    /// Why \a sch is not simulated: the commands it runs besides the
    /// simulator (erc::commandsRun), unless 'allow_commands' in \a args;
    /// empty when it is.
    QString commandsRefused(Schematic* sch, const QJsonObject& args) const;
    /// What each active .NGOPT block of \a sch found in ngspice's \a output:
    /// its summary and the knobs' values, written into the parameters' and
    /// parts' definitions when \a apply (one undo step; what cannot be is
    /// said). Empty when it has none.
    QJsonArray optimumOf(Schematic* sch, const QString& output, bool apply);
    /// The open documents that show \a sch's dataset: itself and its data
    /// displays.
    QList<Schematic*> showingDataOf(Schematic* sch) const;
    QucsDoc* document(const QJsonObject& args, QString* error) const;
    /// \a args of \a tool with the user's selection in them ("selection":
    /// true): its parts' names, its diagrams, paintings and wires - or, for
    /// add_painting ("around": "selection"), a place about it.
    QJsonObject withSelection(const QString& tool, const QJsonObject& args, QString* error) const;
    /// 'document': "data_display", or a data display in 'path' not open:
    /// \a args with the .dpl in 'path', open - made for its schematic when
    /// it has none. False and why when it cannot be.
    bool toDataDisplay(QJsonObject* args, QString* error);
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
    /// The controls of \a dialog get_dialog lists and set_dialog fills, in
    /// order. \a ui (get_ui, set_ui: a part of the window): item views,
    /// sliders and read-only texts (logs) too.
    QList<QWidget*> dialogControls(QWidget* dialog, bool ui = false) const;
    /// get_dialog's answer for \a root: its texts and controls.
    QJsonObject describeControls(QWidget* root, bool ui) const;
    /// set_dialog's work on \a root's controls ('set', then 'press').
    void fillControls(QWidget* root, const QJsonObject& args, const Done& done, bool ui);

    QucsApp* a_app;
    QJsonArray a_tools;
    QHash<QString, QString> a_actions;   // tool -> "add a component"
    QStringList a_readOnly;
};

#endif // QUCS_QUCSCONTROL_H
