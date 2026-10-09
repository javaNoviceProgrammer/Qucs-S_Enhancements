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

#include <QHash>
#include <QList>
#include <QPointer>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <functional>
#include <utility>

class QComboBox;
class QCompleter;
class QIcon;
class QJsonObject;
class QLabel;
class QMenu;
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
    /// The checker's fix of it (ruff's): what it does, whether it is safe
    /// ("safe", "unsafe": it may change what the code does), its edits -
    /// none when it has no fix.
    struct Edit {
        int line = 1;      ///< from 1
        int column = 1;    ///< from 1
        int endLine = 1;
        int endColumn = 1;   ///< the character after it
        QString text;
    };
    QString fixMessage;
    QString fixApplicability;
    QList<Edit> fixEdits;
};
/// \a text with \a edits made (positions of the text as it is; any
/// order).
QString applyEdits(const QString& text, QList<Problem::Edit> edits);
/// The name an "undefined name" message is about (ruff's, pyflakes', mypy's,
/// pyright's); empty for any other.
QString undefinedName(const QString& message);
/// The comment that has \a code ignored on its line by its checker -
/// `# noqa: F401`, `# type: ignore[assignment]` (mypy), `# pyright:
/// ignore[rule]` - for \a line (its text), \a source the checker ("" the
/// check's, mypy, pyright): the line as it is then.
QString ignoredOnLine(const QString& line, const QString& code, const QString& source);

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
/// What a name is, as its completion and the outline show it: a letter on
/// a colour of its own - f a function or method, C a class, m a module, k a
/// keyword, p a parameter, v anything else.
QIcon completionIcon(const QString& type);

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
/// A place a name is used or defined at (the completer's "references"): a
/// file (empty: the script itself), its line (from 1), the name's columns
/// (from 0; the end after it), the line's text, and whether it defines it.
struct Reference {
    QString file;
    int line = 0;
    int column = 0;
    int end = 0;
    QString text;
    bool definition = false;
};
/// A file a rename changes (the completer's "rename"): its path (empty: the
/// script itself) and its text after.
struct FileChange {
    QString file;
    QString text;
};
/// An answer of the completer, of any kind ("complete", "signature",
/// "help", "definition", "references", "rename").
struct Answer {
    int id = -1;
    QString kind;
    QString engine;
    QList<Completion> items;
    Signature signature;
    Help help;
    Place place;
    /// references and rename: the name, and where it was looked for
    /// ("module", "function", "class", "attribute", "builtin", "jedi");
    /// valid: there was a name at the place.
    bool valid = false;
    QString name;
    QString scope;
    QList<Reference> references;
    /// rename: the files changed, the places renamed - or why not.
    QList<FileChange> changes;
    int count = 0;
    QString refusal;
    /// imports: the imports that would define a name, each its line and
    /// where it goes (before the line, from 1).
    struct Import {
        QString title;
        int line = 1;
        QString text;
    };
    QList<Import> imports;
};
Answer readAnswer(const QByteArray& line);

/// A breakpoint, as the line numbers' margin sets it: its line (from 1),
/// an expression that must hold for it to stop, its hits ("5": from the
/// fifth on, "== 5", "> 5", "% 5": every fifth), a logpoint's message (the
/// {expressions} in it evaluated; written, not stopped at), and whether it
/// is on.
struct Breakpoint {
    int line = 0;
    QString condition;
    QString hit;
    QString log;
    bool enabled = true;
    /// Neither a condition, hits nor a message: a plain one.
    bool plain() const { return condition.trimmed().isEmpty() && hit.trimmed().isEmpty() && log.isEmpty(); }
    bool operator==(const Breakpoint& o) const
    {
        return line == o.line && condition == o.condition && hit == o.hit && log == o.log && enabled == o.enabled;
    }
};
/// How the debugger is given one ({"line", "condition", "hit", "log",
/// "enabled"}).
QJsonObject toJson(const Breakpoint& b);

/// A name Go to Symbol lists: a class, a function or method, a variable of
/// the module; its line (from 1) and its name's column (from 0); the class
/// it is in ("": none).
struct Symbol {
    int line = 0;
    int column = 0;
    QString kind;
    QString name;
    QString container;
};
/// The classes, functions, methods and the module's variables of \a text
/// (a variable once, where it is first given a value).
QList<Symbol> symbolsOf(const QString& text);
/// Whether \a pattern picks \a name (its letters in order, any case) and
/// how well (higher: better; -1: not).
int symbolScore(const QString& name, const QString& pattern);

/// The program of the type check (python/tools/typecheck.py), as python -c
/// <it> auto|mypy|pyright <the script's path> <a cache folder>.
const QString& typeCheckProgram();
/// Its answer read: the tool ("mypy 1.13.0"; empty: none installed), what
/// it found (as warnings) - or why it could not check.
struct TypeCheck {
    QString tool;
    QList<Problem> problems;
    QString failure;
};
TypeCheck readTypeCheck(const QByteArray& output);

/// Where each bracket of \a text pairs (strings and comments left out):
/// a bracket's position and its partner's - -1 for one left open or one
/// that closes nothing.
QHash<int, int> bracketPairs(const QString& text);
/// The lines a fold at \a line (from 1) of \a lines hides: those below it
/// that are indented more (blank lines among them; the last ones after them
/// not), or a cell's (# %%) up to the next - {0, 0} for none.
std::pair<int, int> foldRange(const QStringList& lines, int line);
/// The dotted name (self.gain, math.pi) of \a text that the character at
/// \a position is in, and where it begins; empty when it is no name.
std::pair<QString, int> dottedNameAt(const QString& text, int position);

/// The folder put first on the PYTHONPATH of what runs a script - Run,
/// Debug, the Python Shell, the completer: the qucs module (a simulation's
/// dataset read in Python, python/module/qucs.py), the Python Shell's
/// runner of lines and its start-up file, the matplotlib backend of the
/// Python Plots pane and the tables of the Data Viewer, written there from
/// the program's resources. Its folders plots/ (the figures shown), shell/
/// (the Python Shell's variables, and what is asked of them) and requests/
/// (qucs.display()'s) are how scripts reach Qucs-S (pythonviews.h).
QString moduleFolder();
QString plotsFolder();
QString shellFolder();
QString requestsFolder();
/// The figures of matplotlib shown in the Python Plots pane, not windows of
/// their own - a setting (PythonInlinePlots; on by default): new runs, and
/// the Python Shell once it is started again.
bool inlinePlots();
void setInlinePlots(bool on);
/// The environment a script runs in: this one, the output in UTF-8,
/// moduleFolder() first on PYTHONPATH, those folders (QUCS_S_PLOTS,
/// QUCS_S_REQUESTS), matplotlib's backend the pane's (MPLBACKEND, while
/// inlinePlots()), and - in Qucs-S itself, unless it is set -
/// QUCS_S_EXECUTABLE, the program qucs.simulate() runs.
QProcessEnvironment scriptEnvironment();
/// The same, as the Python Shell is given it: "NAME=value" entries - and
/// its start-up file (PYTHONSTARTUP; the user's own run after it), which
/// shows its variables (QUCS_S_SHELL).
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
/// Format Document and Fix Problems, Organize Imports, Format Selection
/// (python/tools/formatter.py).
const QString& formatterProgram();

/// How a script is run and debugged (Run Settings): its arguments - split
/// as a shell splits them -, its working folder (empty: its own),
/// environment variables (NAME=value, a line each) and a .env file of
/// more (relative: to its folder). Kept for each script, by its path.
struct RunSettings {
    QString arguments;
    QString folder;
    QString environment;
    QString envFile;
    bool isEmpty() const { return arguments.trimmed().isEmpty() && folder.trimmed().isEmpty() && environment.trimmed().isEmpty() && envFile.trimmed().isEmpty(); }
    bool operator==(const RunSettings& o) const
    {
        return arguments == o.arguments && folder == o.folder && environment == o.environment && envFile == o.envFile;
    }
};
RunSettings runSettingsFor(const QString& script);
void setRunSettingsFor(const QString& script, const RunSettings& settings);
/// \a text split into arguments as a shell would (quotes, escapes).
QStringList splitArguments(const QString& text);
/// The variables of a .env file's text (or of Run Settings' lines):
/// NAME=value lines - "export" before one allowed, quotes taken off,
/// # comments and blank lines left out, ${NAME} the value of \a base's
/// or of a line before.
QList<std::pair<QString, QString>> readEnvironment(const QString& text, const QProcessEnvironment& base);
/// What a run of \a script is given: scriptEnvironment() with its Run
/// Settings' variables (the .env file's, then the lines'), its folder and
/// its arguments - or why it cannot run so (a .env file not there, a
/// folder that is none).
struct RunPlan {
    QStringList arguments;
    QString folder;
    QProcessEnvironment environment;
    QString failure;
};
RunPlan runPlanFor(const QString& script);

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
 * the cursor is in, above it, what a name the mouse rests on is, where a
 * name is defined (Go to Definition, F12 or Ctrl+click), where it is used
 * (Find All References) - and renames it there (Rename Symbol). An outline
 * of its functions and classes is above the text; its cells (# %%) are
 * ruled off; a click in the line numbers' margin sets a breakpoint there,
 * a right-click one with a condition, hits or a message; the debugger's
 * line is marked, and while it is stopped the mouse resting on a name says
 * its value. Format Document and Fix Problems are ruff's (or black's) -
 * see qucs_s::python::formatterProgram(); a type checker installed for its
 * Python (mypy, pyright) checks it after the check.
 *
 * Brackets and quotes are closed as they are typed (a selection wrapped),
 * the bracket matching the one at the cursor is marked, and a function, a
 * class, any block and a cell fold away - the triangles beside the line
 * numbers, or Fold and Unfold.
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

    /// The script formatted by ruff (or black) / what ruff can fix fixed /
    /// its imports sorted (ruff's isort rules, else isort) / the lines of
    /// the selection (the cursor's line) formatted: one edit, undone at
    /// once; formatted() says what was done.
    void format();
    void fixProblems();
    void organizeImports();
    void formatSelection();
    bool formatting() const { return a_formatProcess != nullptr; }
    /// Formatted before each save (a setting, PythonFormatOnSave; off by
    /// default) - waiting for it a few seconds at most.
    static bool formatOnSave();
    static void setFormatOnSave(bool on);
    int save() override;

    /// The fixes of the problems on \a line (from 1): ruff's fix of each
    /// (while the check's answer is of the text as it is), an import for a
    /// name not defined (the completer's: fixesAnswered() when they come),
    /// and each one ignored on the line (# noqa: F401, # type: ignore[...]).
    /// \a showAt: a menu of them shown there when they are all in (on the
    /// screen; null: none shown).
    void quickFix(int line, const QPoint& showAt = QPoint());
    /// Those of the last quickFix(), by their titles; one made (one edit).
    QStringList fixTitles() const;
    bool applyFix(int index);
    QMenu* fixMenu() const;
    /// The line the light bulb is on - the cursor's, when it has a problem
    /// (0: none).
    int bulbLine() const;

    /// The outline above the text: its functions and classes, the one the
    /// cursor is in chosen; one chosen goes there.
    QComboBox* outlineList() const { return a_outline; }
    /// An entry's kind, as its letter says it ("class", "function",
    /// "module" for the top level): the outline's item data.
    static constexpr int kOutlineKindRole = Qt::UserRole + 1;
    QList<qucs_s::python::OutlineEntry> outline() const { return a_outlineEntries; }

    /// The breakpoints: lines (from 1), each once, in order. A click in the
    /// line numbers' margin sets or takes one away; they move with the
    /// text.
    QList<int> breakpoints() const;
    void toggleBreakpoint(int line);
    void setBreakpoints(const QList<int>& lines);
    /// The same with their conditions, hits, messages, in their lines'
    /// order; the one at \a line (line 0: none).
    QList<qucs_s::python::Breakpoint> breakpointList() const;
    qucs_s::python::Breakpoint breakpointAt(int line) const;
    /// \a b set at its line (one there replaced); taken away.
    void setBreakpoint(const qucs_s::python::Breakpoint& b);
    void removeBreakpoint(int line);
    /// The margin's menu at \a line: Add Breakpoint, Add Conditional
    /// Breakpoint..., Add Logpoint... - or Edit Breakpoint..., Disable or
    /// Enable, Remove -, Fold or Unfold. (A right-click in the margin.)
    QMenu* marginMenuAt(int line);
    /// The breakpoint at \a line edited in a dialog (one made when there is
    /// none; \a field: 0 its condition, 1 its hits, 2 its message, focused).
    void editBreakpoint(int line, int field = 0);

    /// Where the name at the cursor is used, and defined: referencesAnswered()
    /// when it comes, lastReferences() saying it - in the script, and the
    /// modules beside it with jedi.
    void findReferences();
    const qucs_s::python::Answer& lastReferences() const { return a_references; }
    /// The name at the cursor renamed \a name wherever it is that name
    /// (renameAnswered() when it comes; lastRename(): the files it changes,
    /// or why not). The script's text changed in one edit (Undo takes it
    /// back); the other files' changes are QucsApp's to make.
    void renameSymbol(const QString& name);
    const qucs_s::python::Answer& lastRename() const { return a_rename; }
    /// The name at the cursor (empty: none).
    QString nameAtCursor() const;

    /// While the debugger is stopped: asked for the value of the name the
    /// mouse rests on - \a lookup says whether it will answer (with
    /// showValue()); otherwise what the name is is said.
    void setValueLookup(std::function<bool(const QString& expression)> lookup) { a_valueLookup = std::move(lookup); }
    void showValue(const QString& expression, const QString& said);
    QString lastValue() const { return a_lastValue; }
    /// While the debugger is stopped in it: the values of the variables
    /// each line uses, written faintly at its end - from the frame's code's
    /// first line \a first (a function's def; 0: the module's) to the line
    /// \a line it is at, the functions and classes it defines left out (they
    /// run in frames of their own), \a values the frame's (by name) - a
    /// setting (PythonInlineValues, on by default). Cleared as it goes on.
    static bool inlineValuesShown();
    static void setInlineValuesShown(bool on);
    void setInlineValues(int first, int line, const QHash<QString, QString>& values);
    void clearInlineValues();
    /// Those written: a line's (from 1) text.
    QHash<int, QString> inlineValues() const { return a_inlineValues; }

    /// Brackets and quotes closed as they are typed, a selection wrapped in
    /// them - a setting (PythonAutoClose; on by default).
    static bool autoClose();
    static void setAutoClose(bool on);
    /// The bracket at the cursor and its partner, marked (-1, -1: none;
    /// the second -1 alone: one unmatched).
    std::pair<int, int> bracketMarks() const { return a_bracketMarks; }

    /// Folding: whether a fold can begin at \a line, whether it is folded;
    /// folded (its lines hidden) or opened.
    bool isFoldable(int line) const;
    bool isFolded(int line) const;
    void fold(int line);
    void unfold(int line);
    void toggleFold(int line);
    void foldAll();
    void unfoldAll();
    QList<int> foldedLines() const;
    /// The fold of \a line (its first line; 0: none): to fold, the line's
    /// own block or the innermost it is in; to open, the line's own.
    int foldAround(int line, bool folding) const;

    /// The type checker run after each check: "auto" (mypy, else pyright),
    /// "mypy", "pyright" or "off" - a setting (PythonTypeChecker).
    static QString typeChecker();
    static void setTypeChecker(const QString& checker);
    bool typeChecking() const { return a_typeProcess != nullptr; }
    const qucs_s::python::TypeCheck& lastTypeCheck() const { return a_typeCheck; }
    bool typeChecked() const { return a_typeRevision >= 0; }
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
    void referencesAnswered();
    void renameAnswered();
    /// quickFix()'s list is whole (the imports in).
    void fixesAnswered();
    /// The type check answered (or failed): its findings are shown.
    void typeCheckFinished();
    void valueShown();

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
    void marginPressed(const QTextBlock& block, int x, const QPoint& global) override;
    void marginMenu(const QTextBlock& block, const QPoint& global) override;
    int foldRoom() const override;
    void paintFold(QPainter& painter, const QTextBlock& block, const QRect& box) override;
    void foldPressed(const QTextBlock& block) override;
    QList<QTextEdit::ExtraSelection> moreSelections() const override;

private:
    /// The Python scripts open (a setting reaches each).
    static QList<PythonDoc*> openDocuments();
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
    void runFormatter(const QString& mode, const QString& lines = QString());
    void finishFormat();
    /// The menu of the fixes, shown at a_fixAt.
    void showFixMenu();
    /// \a text in place of the script's, changing only what differs.
    void replaceText(const QString& text);
    void updateOutline();
    void chooseOutlineEntry();
    void placeOutline();
    void placeLibraryNote();
    /// A key typed with brackets and quotes closed (autoClose()): true when
    /// it was taken.
    bool closePair(QKeyEvent* event);
    /// The bracket at the cursor and its partner, marked again; the marks.
    void markBrackets();
    QList<QTextEdit::ExtraSelection> bracketSelections() const;
    /// The lines of the folds hidden, every other shown.
    void applyFolds();
    void scheduleTypeCheck();
    void startTypeCheck();
    void finishTypeCheck();
    /// The check's findings and the type check's together in the text.
    void showProblems(const QList<TextDoc::Diagnostic>& basic, const QList<TextDoc::Diagnostic>& typed);

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
    QString a_formatLines;
    int a_checkRevision = -1;   // the text lastCheck() is of

    struct Fix {
        QString title;
        QList<qucs_s::python::Problem::Edit> edits;   // on the text as it is (one edit, applied)
    };
    QList<Fix> a_fixes;
    int a_fixesRevision = -1;
    int a_fixRequest = 0;       // the imports asked for
    int a_fixLine = 0;
    QPoint a_fixAt;             // where its menu goes (null: none)
    QPointer<QMenu> a_fixMenu;   // (cleared as it goes: closed, or with this editor)
    int a_bulbLine = 0;         // where the bulb was drawn

    QWidget* a_outlineBar = nullptr;
    QComboBox* a_outline = nullptr;
    QTimer* a_outlineDelay = nullptr;
    QList<qucs_s::python::OutlineEntry> a_outlineEntries;

    struct BreakpointMark {
        QTextCursor at;                  // at its line's start: it moves with the text
        qucs_s::python::Breakpoint spec;
    };
    QList<BreakpointMark> a_breakpoints;
    QTextCursor a_execution;            // the debugger's line (null: none)
    bool a_executionTop = true;

    int a_referencesRequest = 0;
    qucs_s::python::Answer a_references;
    int a_renameRequest = 0;
    int a_renameRevision = -1;          // the text it was asked about
    qucs_s::python::Answer a_rename;

    QHash<int, QString> a_inlineValues;   // a line's (from 1) values written at its end
    std::function<bool(const QString&)> a_valueLookup;
    QString a_valueExpression;          // asked of the debugger, its answer awaited
    QPoint a_valueAt;                   // where the mouse rests (the viewport's): help there when it has no value
    QString a_lastValue;

    int a_bracketRevision = -1;         // the text a_brackets is of
    QHash<int, int> a_brackets;
    std::pair<int, int> a_bracketMarks{-1, -1};

    struct Fold {
        QTextCursor at;                  // its line's start
        QString header;                  // its line as it was folded: gone with another
    };
    QList<Fold> a_folds;
    bool a_applyingFolds = false;

    QProcess* a_typeProcess = nullptr;
    QTimer* a_typeDelay = nullptr;
    int a_typeRunning = -1;             // the revision the type check running was given
    int a_typeRevision = -1;            // the revision the last answer was of
    QString a_typeRunningWith;          // the checker the one running is ("auto", ...)
    qucs_s::python::TypeCheck a_typeCheck;
};

#endif // QUCS_PYTHONDOC_H
