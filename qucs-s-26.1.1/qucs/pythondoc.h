/*
 * pythondoc.h - a Python script in the text editor: checked as it is
 *               typed, indented as Python is, run from the Python toolbar
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
#include <QString>
#include <QStringList>

class QCompleter;
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
 */
class PythonDoc : public TextDoc
{
    Q_OBJECT

public:
    PythonDoc(QucsApp* app, const QString& name);
    ~PythonDoc() override;

    bool load() override;
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

signals:
    /// A check answered (or failed): lastCheck() and the diagnostics are
    /// its.
    void checkFinished();
    /// An answer of the completer came (shown, or not: the cursor moved
    /// on).
    void completionsAnswered();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    /// The list's keys are its own while it is shown, not the window's
    /// shortcuts (Escape is the window's too).
    bool event(QEvent* event) override;

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
    int a_request = 0;                // the last question's id
    int a_requestBlock = -1;          // where its word began: an answer for another word is not shown
    int a_requestStart = -1;
    QString a_completionEngine;
};

#endif // QUCS_PYTHONDOC_H
