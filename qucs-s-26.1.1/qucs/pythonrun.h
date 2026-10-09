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
 * folder, with nothing on its standard input (input() ends the file). A
 * traceback's lines that name a file and a line (File "x.py", line 3) are
 * links: a click asks for that place (locationRequested()).
 *
 * debug() runs it under the debugger (qucs_s::python::debuggerProgram()):
 * it stops at the breakpoints and where an exception no one catches is
 * raised, and is stepped. While it is stopped a panel beside the output
 * shows the call stack (a frame chosen: its variables, and its line), the
 * variables of the frame - a list's, a dictionary's, an object's insides
 * opened - and a line to evaluate an expression or a statement in it.
 */
class PythonRunConsole : public QWidget
{
    Q_OBJECT

public:
    explicit PythonRunConsole(QWidget* parent = nullptr);
    ~PythonRunConsole() override;

    /// Runs \a script with \a interpreter (a run going is stopped first);
    /// false, said in the console, when it could not be started.
    bool run(const QString& interpreter, const QString& script);
    /// Runs \a script under the debugger, stopping at \a breakpoints (a
    /// file's lines, from 1).
    bool debug(const QString& interpreter, const QString& script, const QHash<QString, QList<int>>& breakpoints);
    bool isRunning() const;
    /// A run under the debugger is going; it is stopped (paused) in it.
    bool isDebugging() const { return a_debugging; }
    bool isPaused() const { return a_paused; }
    /// A frame of the stack where it stopped (the innermost first).
    struct Frame {
        QString file;
        int line = 0;
        QString function;
    };
    QList<Frame> stack() const { return a_stack; }
    /// The frame looked at (0: the innermost).
    int frame() const { return a_frame; }
    /// Why it stopped: "breakpoint", "step", "exception" - and the
    /// exception.
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

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool start(const QString& interpreter, const QString& script, const QStringList& arguments, bool debugging);
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

    QProcess* a_process = nullptr;
    QPlainTextEdit* a_output = nullptr;
    QLabel* a_status = nullptr;
    QPushButton* a_stop = nullptr;
    QPushButton* a_clear = nullptr;
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
};

#endif // QUCS_PYTHONRUN_H
