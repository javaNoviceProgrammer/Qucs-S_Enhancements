/*
 * pythonrun.h - a Python script run as a program of its own, its output in
 *               a console (the Python toolbar's Run) - or debugged
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_PYTHONRUN_H
#define QUCS_PYTHONRUN_H

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringDecoder>
#include <QWidget>

#include "pythondoc.h"

#include <utility>

class QJsonArray;
class QJsonObject;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QSplitter;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

/*!
 * \brief The console of a script run: its output as it comes - standard
 *        output and error together, in their order -, a line at the end
 *        with its exit code and its time, and Stop and Clear.
 *
 * The script runs with the interpreter given, unbuffered (-u), in its own
 * folder; what is typed in the line below the output while it runs is its
 * standard input (echoed in the output), and End Input (Ctrl+D) ends it -
 * input() reads the end of the file then. A traceback's lines that name a
 * file and a line (File "x.py", line 3) are links: a click asks for that
 * place (locationRequested()).
 *
 * debug() runs it under the debugger (qucs_s::python::debuggerProgram()):
 * it stops at the breakpoints - when their conditions hold and their hits
 * are as asked; a logpoint writes its message -, where an exception no one
 * catches is raised (and where any is raised, when asked), where it is
 * paused and where it was run to, and is stepped. While it is stopped a
 * panel beside the output shows the call stack (a frame chosen: its
 * variables, and its line), the variables of the frame - a list's, a
 * dictionary's, an object's insides opened; an array, a list, a DataFrame
 * shown as a table (View as Table) - and a line to evaluate an expression
 * or a statement in it.
 */
class PythonRunConsole : public QWidget
{
    Q_OBJECT

public:
    explicit PythonRunConsole(QWidget* parent = nullptr);
    ~PythonRunConsole() override;

    /// Runs \a script with \a interpreter (a run going is stopped first),
    /// as its Run Settings say - its arguments, its folder, its environment
    /// (qucs_s::python::runPlanFor()); false, said in the console, when it
    /// could not be started.
    bool run(const QString& interpreter, const QString& script);
    /// Runs \a script under the debugger, stopping at \a breakpoints (a
    /// file's lines, from 1) - or those with their conditions, hits and
    /// messages -, where an exception is raised when \a raised, and once at
    /// \a runTo (a file and its line; none when the file is empty).
    bool debug(const QString& interpreter, const QString& script, const QHash<QString, QList<int>>& breakpoints);
    bool debug(const QString& interpreter, const QString& script,
               const QHash<QString, QList<qucs_s::python::Breakpoint>>& breakpoints, bool raised,
               const std::pair<QString, int>& runTo = {});
    bool isRunning() const;
    /// A run under the debugger is going; it is stopped (paused) in it.
    bool isDebugging() const { return a_debugging; }
    bool isPaused() const { return a_paused; }
    /// A frame of the stack where it stopped (the innermost first).
    struct Frame {
        QString file;
        int line = 0;
        QString function;
        bool library = false;   ///< Python's own code, or a package's
    };
    QList<Frame> stack() const { return a_stack; }
    /// The frame looked at (0: the innermost).
    int frame() const { return a_frame; }
    /// Why it stopped: "breakpoint", "step", "pause", "raised" (where an
    /// exception was raised, asked to), "exception" (one no one catches) -
    /// and the exception.
    QString stopReason() const { return a_reason; }
    QString stopException() const { return a_exception; }
    QWidget* debugPanel() const { return a_debugPanel; }
    QListWidget* stackView() const { return a_stackView; }
    QTreeWidget* variablesView() const { return a_variables; }
    QLineEdit* evaluateLine() const { return a_evaluate; }
    /// The variables shown, "name: type = value" each (those opened, below
    /// their own, indented).
    QStringList variableRows() const;
    /// What the last evaluation gave: the value, or the error.
    QString lastEvaluated() const { return a_lastEvaluated; }
    /// The script of the last run.
    QString script() const { return a_script; }
    /// The last run's exit code; -1 while it runs, when it was stopped or
    /// crashed, or when it could not be started.
    int exitCode() const { return a_exitCode; }
    bool wasStopped() const { return a_stopped; }

    QPlainTextEdit* outputView() const { return a_output; }
    QString outputText() const;
    /// The line below the output for what the script reads (input()).
    QLineEdit* inputLine() const { return a_input; }
    /// Its standard input still open (End Input closes it).
    bool inputOpen() const { return a_inputOpen; }
    /// Pause asked for: it stops at the next line of the script's.
    bool pausing() const { return a_pausing; }
    /// The value of \a expression in the frame looked at, while it is
    /// stopped: inspected() with the id returned (0: not stopped).
    int inspect(const QString& expression);
    /// A value's rows: of the variable at \a handle (as variablesView()
    /// has it), or of \a expression in the frame looked at (handle -1);
    /// dataArrived() with the id returned (0: not stopped).
    int requestData(int handle, const QString& expression, int start, int count);
    /// Its debugger told to stop where an exception is raised (caught or
    /// not), or not - at once, while it runs.
    void setBreakOnRaised(bool on);
    /// Python's library and packages stepped into too (not stepped over) -
    /// for the next debug run, and at once in one going.
    void setDebugLibrary(bool on);
    bool debugsLibrary() const { return a_library; }
    /// Whether Interrupt works here (not on Windows).
    static bool canInterrupt();

    /// The most lines kept: earlier ones go.
    static constexpr int kMostLines = 20000;

public slots:
    /// Ends the run: asked to stop, then killed if it does not.
    void stop();
    void clear();
    /// On to the next breakpoint; to the next line; into the call; out of
    /// the function (while it is stopped).
    void continueRun();
    void stepOver();
    void stepInto();
    void stepOut();
    /// The frame \a index of the stack looked at: its variables, its line.
    void selectFrame(int index);
    /// \a expression (or a statement) in the frame looked at.
    void evaluate(const QString& expression);
    /// \a file's breakpoints now (taken while it runs, too).
    void setBreakpoints(const QString& file, const QList<int>& lines);
    void setBreakpoints(const QString& file, const QList<qucs_s::python::Breakpoint>& breakpoints);
    /// While it runs: stopped at the next line of the script's (Pause).
    void pause();
    /// A KeyboardInterrupt in the script (SIGINT, as Ctrl+C in a terminal)
    /// - in a long call of C too, where Pause waits; debugged, it stops
    /// where it was, as an exception no one catches does.
    void interrupt();
    /// While it is stopped: on to \a line of \a file - once - or to a
    /// breakpoint before it (Run to Cursor).
    void runTo(const QString& file, int line);
    /// \a text and a line's end written to the script's standard input,
    /// echoed; false when nothing runs or its input was ended.
    bool sendInput(const QString& text);
    /// Its standard input ended: input() reads the end of the file.
    void endInput();

signals:
    void started();
    void finished(int exitCode);
    /// A traceback's place was clicked: \a line (1-based) of \a file.
    void locationRequested(const QString& file, int line);
    /// Stopped there - the innermost frame when \a top -, or a frame
    /// further out looked at.
    void locationShown(const QString& file, int line, bool top);
    void paused();
    void resumed();
    /// A debug run started, stopped, went on or ended.
    void debuggingChanged();
    /// Variables came: a frame's, a value's insides, after an evaluation.
    void variablesShown();
    void evaluated();
    /// inspect()'s answer: the value as repr() says it, or the error.
    void inspected(int id, const QString& expression, const QString& value, const QString& error);
    /// requestData()'s answer (_qucs_data.table()'s, or {"error"}).
    void dataArrived(int id, const QJsonObject& table);
    /// View as Table on a variable: its handle and its name.
    void tableRequested(int handle, const QString& name);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool start(const QString& interpreter, const QString& script, const QStringList& arguments, bool debugging,
               const qucs_s::python::RunPlan& plan);
    void readOutput();
    void readEvents();
    void handleEvent(const QJsonObject& event);
    void command(const QJsonObject& command);
    void fillVariables(QTreeWidgetItem* parent, const QJsonArray& items);
    void setPaused(bool paused);
    void runFinished();
    /// \a text at the end of the output: lines completed are made links
    /// where they name a file and a line.
    void append(const QString& text);
    void endLine();
    void note(const QString& text);
    void setStatus(const QString& text);
    void setInputOpen(bool open);

    QProcess* a_process = nullptr;
    QPlainTextEdit* a_output = nullptr;
    QLabel* a_status = nullptr;
    QPushButton* a_stop = nullptr;
    QPushButton* a_interrupt = nullptr;
    QPushButton* a_clear = nullptr;
    QLineEdit* a_input = nullptr;
    QPushButton* a_endInput = nullptr;
    bool a_inputOpen = false;
    QStringDecoder a_decoder;
    QElapsedTimer a_clock;
    QString a_script;
    QString a_interpreter;
    int a_exitCode = -1;
    bool a_stopped = false;
    bool a_lineOpen = false;      // the output's last line is the run's, not yet ended
    bool a_overwrite = false;     // a carriage return: the line's next text replaces it

    // The debugger.
    QSplitter* a_split = nullptr;
    QWidget* a_debugPanel = nullptr;
    QToolButton* a_continue = nullptr;
    QToolButton* a_pause = nullptr;
    QToolButton* a_stepOver = nullptr;
    QToolButton* a_stepInto = nullptr;
    QToolButton* a_stepOut = nullptr;
    QListWidget* a_stackView = nullptr;
    QTreeWidget* a_variables = nullptr;
    QLineEdit* a_evaluate = nullptr;
    QByteArray a_events;          // a line of the debugger's not yet whole
    bool a_debugging = false;
    bool a_paused = false;
    QList<Frame> a_stack;
    int a_frame = 0;
    QString a_reason;
    QString a_exception;
    QString a_lastEvaluated;
    QHash<int, QTreeWidgetItem*> a_opening;   // a value's insides asked for: its row, by its handle
    bool a_pausing = false;
    bool a_library = false;                   // setDebugLibrary()
    bool a_interrupted = false;               // the run's end said so
    int a_requests = 0;                       // inspect() and requestData(): the next id
};

#endif // QUCS_PYTHONRUN_H
