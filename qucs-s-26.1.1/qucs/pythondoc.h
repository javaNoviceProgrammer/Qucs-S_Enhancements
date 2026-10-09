/*
 * pythondoc.h - a Python script in the text editor: checked as it is
 *               typed, completed, indented as Python is, run, run in parts
 *               and debugged from the Python toolbar
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_PYTHONDOC_H
#define QUCS_PYTHONDOC_H

#include "textdoc.h"

#include <QList>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <utility>

class QComboBox;
class QCompleter;
class QLabel;
class QProcess;
class QStandardItemModel;
class QTimer;

namespace qucs_s::python {

/// What a check found at a place: its line and column (1-based; column 0:
/// the whole line), where it ends (the line and column after it; 0: at its
/// line's end), an error - Python would not run the script - or a warning,
/// the message and the checker's code for it (ruff's F401; none from the
/// compiler or pyflakes).
struct Problem {
    int line = 1;
    int column = 0;
    int endLine = 0;
    int endColumn = 0;
    bool error = false;
    QString message;
    QString code;
};

/// A check's answer: the Python that compiled the script ("3.14.7"), the
/// checker besides the compiler ("ruff 0.6.9", "pyflakes 3.2.0"; empty:
/// the compiler alone), what they found - or why there is no answer.
struct Check {
    QString python;
    QString checker;
    QList<Problem> problems;
    QString failure;   ///< empty when it was checked
};

/// The program a check runs, as python -c <it> <file name>: the script on
/// standard input (UTF-8) compiled - not run -, its syntax error and the
/// compiler's warnings (SyntaxWarning, DeprecationWarning) as it gives
/// them; with no syntax error, what ruff (python -m ruff, or ruff on PATH)
/// or else pyflakes finds, when the interpreter has one. One JSON object
/// on standard output.
const QString& checkerProgram();
/// That object read; a failure, with why, when it is not one.
Check readCheck(const QByteArray& output);

/// An interpreter a script can run with: its path and how the Python
/// toolbar names it.
struct Interpreter {
    QString path;
    QString label;
};
/// Those of the script \a script, in this order, each once: a virtual
/// environment's (.venv, venv) beside it, then in \a project (its folder;
/// empty: none), the one Application Settings name, python3 and python on
/// the PATH, and \a chosen (those picked with Browse). Its default is the
/// first.
QList<Interpreter> interpretersFor(const QString& script, const QString& project, const QStringList& chosen);

/// A word that completes the one at the cursor: the whole of it, what it
/// is (module, class, function, keyword, instance, statement, param,
/// path...: jedi's types) and a line about it.
struct Completion {
    QString name;
    QString type;
    QString description;
};
/// An answer of the completer: the request it answers, what made it
/// ("jedi 0.19.2"; empty: the completer's own names), the words.
struct Completions {
    int id = -1;
    QString engine;
    QList<Completion> items;
};
/// The program that completes (python -u -c <it>), running while the
/// script is open: a request a line on standard input - {"id", "source",
/// "line" (from 1), "column" (from 0), "path"} -, its answer a line on
/// standard output. jedi when the interpreter has it; otherwise Python's
/// keywords and builtins and the script's names, the modules after import,
/// the members of a module the script imports (a standard one imported,
/// another read without running it), and after name. the names that follow
/// name. elsewhere in the script.
const QString& completerProgram();
/// A line of its answers read (id -1: none).
Completions readCompletions(const QByteArray& line);

/// The call the cursor is in (the completer's "signature"): its name, its
/// parameters as written ("b=2", "*args"), which of them the argument being
/// written is (-1: none), the first paragraph of its documentation, and
/// where its bracket is (line from 1, column from 0).
struct Signature {
    bool valid = false;
    QString name;
    QStringList params;
    int index = -1;
    QString doc;
    int openLine = 0;
    int openColumn = 0;
};
/// What the name at a place is (the completer's "help"): a line saying it
/// ("math.sqrt(x, /)"), its kind and its documentation.
struct Help {
    bool valid = false;
    QString title;
    QString type;
    QString text;
};
/// Where the name at a place is defined (the completer's "definition"):
/// a file (empty: the script itself), its line (from 1) and column (from
/// 0) - or nowhere to show, built into Python - and whether the file is
/// Python's library or an installed package's (shown read-only).
struct Place {
    bool valid = false;
    QString file;
    int line = 0;
    int column = 0;
    QString name;
    bool builtin = false;
    bool library = false;
};
/// An answer of the completer, of any kind ("complete", "signature",
/// "help", "definition").
struct Answer {
    int id = -1;
    QString kind;
    QString engine;
    QList<Completion> items;
    Signature signature;
    Help help;
    Place place;
};
Answer readAnswer(const QByteArray& line);

/// The folder put first on the PYTHONPATH of what runs a script - Run,
/// Debug, the Python Shell, the completer: the qucs module (a simulation's
/// dataset read in Python, python/module/qucs.py) and the Python Shell's
/// runner of lines, written there from the program's resources.
QString moduleFolder();
/// The environment a script runs in: this one, the output in UTF-8,
/// moduleFolder() first on PYTHONPATH, and - in Qucs-S itself, unless it is
/// set - QUCS_S_EXECUTABLE, the program qucs.simulate() runs.
QProcessEnvironment scriptEnvironment();
/// The same, as the Python Shell is given it: "NAME=value" entries.
QStringList shellEnvironment();
/// What the check, the completer and the formatter run in: a folder of
/// their own, empty - python -c puts the folder it runs in first on
/// sys.path, and a json.py or ast.py of the script's folder would stand in
/// for the modules they import.
QString neutralFolder();
/// A Python program of the resources: ":/python/<name>".
QString programText(const QString& name);
/// The debugger (python/tools/debugger.py), as python -u -c <it> script.
const QString& debuggerProgram();
/// Format Document and Fix Problems (python/tools/formatter.py).
const QString& formatterProgram();

/// Whether \a line begins a cell: # %%, #%%, # In[3]:, # <codecell>.
bool isCellMarker(const QString& line);
/// The cell of \a text around line \a line (from 1): its first and last
/// lines - the whole text when it has no cells.
std::pair<int, int> cellAround(const QString& text, int line);

/// A function or a class of a script, for its outline: its line (from 1),
/// the last line of its body, how deep it is, "class" or "def", its name.
struct OutlineEntry {
    int line = 0;
    int lastLine = 0;
    int depth = 0;
    QString kind;
    QString name;
};
QList<OutlineEntry> outlineOf(const QString& text);
/// The start of the name being typed in \a text (the text before the
/// cursor), \a text's length when there is none.
int wordStart(const QString& text);
/// Whether the cursor after \a text (its line before it) is in a string or
/// a comment: as one types there, nothing is offered.
bool inStringOrComment(const QString& text);

/// One level of indentation in \a text: a tab when its first indented line
/// begins with one, else four spaces (PEP 8).
QString indentStep(const QString& text);
/// The indentation the line after \a line takes (\a line: the text before
/// the cursor): its own, a \a step more after a line ending in ':' (its
/// comment aside), a step less after return, pass, break, continue or
/// raise.
QString nextIndent(const QString& line, const QString& step);

} // namespace qucs_s::python

/*!
 * A Python script (.py, .pyw): a text document - edited, highlighted,
 * searched, reloaded, saved with its encoding and line ends as any - that
 * is checked half a second after the typing stops by the interpreter it
 * runs with (qucs_s::python::checkerProgram()): its errors and warnings
 * drawn under their places, dotted in the line numbers' margin, said over
 * them and listed on the Problems tab. A check still running when the text
 * changes is stopped: only the last one's answer is shown. Return indents
 * as Python does, Tab and Shift+Tab indent and take it back (a line, or
 * every line selected). It has no document settings, and writes no
 * settings file beside it.
 *
 * The completer (qucs_s::python::completerProgram()) also says the call
 * the cursor is in, above it, what a name the mouse rests on is, and where
 * a name is defined (Go to Definition, F12 or Ctrl+click). An outline of
 * its functions and classes is above the text; its cells (# %%) are ruled
 * off; a click in the line numbers' margin sets a breakpoint there; the
 * debugger's line is marked. Format Document and Fix Problems are ruff's
 * (or black's) - see qucs_s::python::formatterProgram().
 */
class PythonDoc : public TextDoc
{
    Q_OBJECT

public:
    PythonDoc(QucsApp* app, const QString& name);
    ~PythonDoc() override;

    bool load() override;
    bool reload() override;
    bool writesSettings() const override { return false; }

    /// The interpreter it runs and is checked with: the one chosen for it,
    /// else the first of qucs_s::python::interpretersFor().
    QString interpreter() const;
    /// Chosen on the Python toolbar (empty: the default again); checked
    /// again with it.
    void setInterpreter(const QString& path);
    bool interpreterChosen() const { return !a_interpreter.isEmpty(); }

    /// Checks it now, a check running stopped.
    void checkNow();
    bool checking() const { return a_process != nullptr; }
    /// Whether a check has answered (or failed) since it was opened.
    bool checked() const { return a_hasCheck; }
    const qucs_s::python::Check& lastCheck() const { return a_check; }
    /// How the toolbar's Check says what checked it: "Python 3.14.7 and
    /// ruff 0.6.9", or why it could not be checked.
    QString checkedBy() const;

    /// The messages written at the ends of their lines in every Python
    /// script, a setting (PythonMessagesAtLineEnds; off by default).
    static bool messagesAtLineEnds();
    static void setMessagesAtLineEnds(bool on);

    /// The delay between the last edit and the check, in milliseconds.
    static constexpr int kCheckDelay = 500;

    /// Asks for what completes the name at the cursor, shown in a list
    /// below it when it comes (Return or Tab takes one, Escape closes it,
    /// typing narrows it). \a asked: Simulation > Python > Show Completions
    /// (Ctrl+Space) - anywhere, after a single letter too; otherwise as one
    /// types (completeAsYouType()).
    void complete(bool asked);
    QCompleter* completer() const { return a_completer; }
    /// Whether the list is shown.
    bool completing() const;
    /// The words of the last answer, as listed.
    QStringList completionNames() const;
    /// What completed it last ("jedi 0.19.2"; empty: the completer's own
    /// names), and how the toolbar says it.
    QString completionEngine() const { return a_completionEngine; }
    QString completedBy() const;
    /// The list offered as one types, after two letters of a name or a
    /// dot - a setting (PythonCompleteAsYouType; on by default).
    static bool completeAsYouType();
    static void setCompleteAsYouType(bool on);
    /// The delay between a letter typed and the question, in milliseconds.
    static constexpr int kCompleteDelay = 120;

    /// Asks for the call the cursor is in, shown above it when it comes -
    /// asked again as the cursor moves in it, gone when it leaves it.
    /// \a asked: Simulation > Python > Show Signature (Ctrl+Shift+Space);
    /// otherwise as one types a bracket or a comma (completeAsYouType()).
    void showSignature(bool asked);
    bool signatureShown() const;
    QLabel* signatureTip() const { return a_signatureTip; }
    const qucs_s::python::Signature& lastSignature() const { return a_signature; }
    /// Asks what the name at \a at (the text's coordinates) is: said there
    /// when it comes, with the line's errors and warnings (the mouse resting
    /// on a name). False when there is no name there.
    bool showHelpAt(const QPoint& at);
    /// What was said last (its line and its documentation, as text).
    QString lastHelp() const { return a_lastHelp; }
    /// Asks where the name at the cursor is defined (F12, Ctrl+click):
    /// definitionAnswered() when it comes, lastDefinition() saying it.
    void goToDefinition();
    const qucs_s::python::Place& lastDefinition() const { return a_definition; }

    /// Shown read-only, with a note, and not checked: a file of Python's
    /// library or an installed package's, opened by Go to Definition.
    void setLibraryFile(bool on);
    bool isLibraryFile() const { return a_library; }

    /// The cell the cursor is in: its first and last lines.
    std::pair<int, int> currentCell() const;

    /// The script formatted by ruff (or black) / what ruff can fix fixed:
    /// one edit, undone at once; formatted() says what was done.
    void format();
    void fixProblems();
    bool formatting() const { return a_formatProcess != nullptr; }

    /// The outline above the text: its functions and classes, the one the
    /// cursor is in chosen; one chosen goes there.
    QComboBox* outlineList() const { return a_outline; }
    QList<qucs_s::python::OutlineEntry> outline() const { return a_outlineEntries; }

    /// The breakpoints: lines (from 1), each once, in order. A click in the
    /// line numbers' margin sets or takes one away; they move with the
    /// text.
    QList<int> breakpoints() const;
    void toggleBreakpoint(int line);
    void setBreakpoints(const QList<int>& lines);
    /// The debugger's line (0: none), the line it stopped at when \a top,
    /// else that of a frame further out, looked at.
    void setExecutionLine(int line, bool top = true);
    int executionLine() const;

signals:
    /// A check answered (or failed): lastCheck() and the diagnostics are
    /// its.
    void checkFinished();
    /// An answer of the completer came (shown, or not: the cursor moved
    /// on).
    void completionsAnswered();
    void signatureAnswered();
    void helpAnswered();
    void definitionAnswered();
    /// Format Document or Fix Problems is done: what was done, or why not.
    void formatted(const QString& said);
    void breakpointsChanged();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    /// The list's keys are its own while it is shown, not the window's
    /// shortcuts (Escape is the window's too).
    bool event(QEvent* event) override;
    /// Ctrl+click: Go to Definition.
    void mousePressEvent(QMouseEvent* event) override;
    /// The mouse resting on a name: what it is.
    bool viewportEvent(QEvent* event) override;
    /// The cells ruled off.
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    QMargins extraMargins() const override;
    int markRoom() const override;
    void paintMark(QPainter& painter, const QTextBlock& block, const QRect& box) override;
    void marginPressed(const QTextBlock& block) override;
    QList<QTextEdit::ExtraSelection> moreSelections() const override;

private:
    void scheduleCheck();
    void stopCheck();
    void finishCheck();
    /// Indents (\a by 1) or takes a level away (-1) from the lines the
    /// cursor's selection touches.
    void shiftLines(int by);
    /// The completer's process: started for the first question, again when
    /// the interpreter changes.
    bool startCompleter();
    void stopCompleter();
    void readCompleter();
    void showCompletions(const qucs_s::python::Completions& answer);
    /// The list shown below the word \a word typed so far, sized to its
    /// words.
    void placeList(const QString& word);
    /// The word at the cursor replaced by \a name.
    void insertCompletion(const QString& name);
    /// After a key typed: the list narrowed, closed, or asked for.
    void afterTyping(const QString& typed);
    /// A question of \a kind about the place \a line, \a column to the
    /// completer; its id (0: none sent).
    int ask(const QString& kind, int line, int column);
    void answerSignature(const qucs_s::python::Answer& answer);
    void answerHelp(const qucs_s::python::Answer& answer);
    void placeSignature();
    void runFormatter(const QString& mode);
    void finishFormat();
    /// \a text in place of the script's, changing only what differs.
    void replaceText(const QString& text);
    void updateOutline();
    void chooseOutlineEntry();
    void placeOutline();
    void placeLibraryNote();

    QTimer* a_delay = nullptr;
    QTimer* a_limit = nullptr;   // a check that takes too long is stopped
    QProcess* a_process = nullptr;
    int a_checkedRevision = -1;    // the text's revision the check running was given
    int a_scheduledRevision = -1;  // the revision a check was last scheduled for
    QString a_interpreter;         // chosen; empty: the default
    qucs_s::python::Check a_check;
    bool a_hasCheck = false;

    QCompleter* a_completer = nullptr;
    QStandardItemModel* a_completions = nullptr;
    QProcess* a_completerProcess = nullptr;
    QString a_completerInterpreter;   // the one it runs
    QByteArray a_completerOutput;     // a line not yet whole
    QTimer* a_completeDelay = nullptr;
    int a_questions = 0;              // the questions asked, of every kind: the next one's id
    int a_request = 0;                // the last completion's id
    int a_requestBlock = -1;          // where its word began: an answer for another word is not shown
    int a_requestStart = -1;
    QString a_completionEngine;

    QLabel* a_signatureTip = nullptr;
    QTimer* a_signatureDelay = nullptr;
    int a_signatureRequest = 0;
    int a_signatureRevision = -1;     // the text it was asked about
    qucs_s::python::Signature a_signature;
    int a_helpRequest = 0;
    QPoint a_helpAt;                  // where the mouse rested (global)
    QRect a_helpRect;                 // its name (the text's coordinates): the tooltip goes when the mouse leaves it
    QString a_helpDiagnostics;        // the line's errors and warnings, said with it
    QString a_lastHelp;
    int a_definitionRequest = 0;
    qucs_s::python::Place a_definition;

    bool a_library = false;
    QLabel* a_libraryNote = nullptr;

    QProcess* a_formatProcess = nullptr;
    int a_formatRevision = -1;
    QString a_formatMode;

    QWidget* a_outlineBar = nullptr;
    QComboBox* a_outline = nullptr;
    QTimer* a_outlineDelay = nullptr;
    QList<qucs_s::python::OutlineEntry> a_outlineEntries;

    QList<QTextCursor> a_breakpoints;   // at their lines' starts: they move with the text
    QTextCursor a_execution;            // the debugger's line (null: none)
    bool a_executionTop = true;
};

#endif // QUCS_PYTHONDOC_H
