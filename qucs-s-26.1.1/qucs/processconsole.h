/*
 * processconsole.h - a console around an interactive program, for the
 *                    Terminal and Python Shell docks
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef PROCESSCONSOLE_H
#define PROCESSCONSOLE_H

#include <QObject>
#include <QStringDecoder>
#include <QStringList>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QSocketNotifier;
class QTimer;

/*!
 * \brief An interactive program - a shell, a Python interpreter - with
 *        its output as a byte stream and its input as bytes written.
 *
 * On Unix the program runs on a pseudo-terminal, so it behaves as it
 * does in a terminal window: prompts, echo of the input, Ctrl-C to the
 * foreground job. On Windows it runs on pipes (QProcess) and reads its
 * commands from standard input.
 */
class ConsoleProcess : public QObject
{
    Q_OBJECT
public:
    explicit ConsoleProcess(QObject* parent = nullptr);
    ~ConsoleProcess() override;

    /// Starts \a program (a name on PATH or a path) with \a args in
    /// \a workDir, with \a extraEnv ("NAME=value" entries) on top of the
    /// application's environment. False, with the reason in
    /// errorString(), when it cannot be started.
    bool start(const QString& program, const QStringList& args,
               const QString& workDir, const QStringList& extraEnv = {});
    bool isRunning() const;
    qint64 processId() const;
    QString errorString() const { return a_error; }

    void write(const QByteArray& data);
    /// Ctrl-C to the program (its foreground job, on Unix).
    void interrupt();
    /// Ends the program: a hang-up, then a kill if it does not go.
    void stop();

signals:
    void output(const QByteArray& data);
    /// The program is gone; \a exitCode is -1 when it was stopped or killed.
    void finished(int exitCode);

private:
    void endProcess();   // stop() without the finished() signal
    void cleanUp();
#ifdef Q_OS_WIN
    QProcess* a_process = nullptr;
#else
    void readFromPty();
    bool reap(bool wait);
    int a_master = -1;
    qint64 a_pid = -1;
    QSocketNotifier* a_notifier = nullptr;
    QTimer* a_reaper = nullptr;
#endif
    QString a_error;
};

/*!
 * \brief The console widget: the program's output above, a command line
 *        below (Enter sends, Up/Down browse the history, Ctrl-C
 *        interrupts, Ctrl-D sends end-of-file, Ctrl-L clears), with
 *        Restart, Clear and Interrupt buttons and one that changes to the
 *        project directory.
 *
 * The output is shown as text: escape sequences are dropped, a carriage
 * return starts the line over and a backspace takes a character back, so
 * that prompts, progress lines and simple editing look right; a full
 * terminal emulator this is not.
 */
class ProcessConsole : public QWidget
{
    Q_OBJECT
public:
    explicit ProcessConsole(QWidget* parent = nullptr);
    ~ProcessConsole() override;

    /// How the "Project dir" command quotes its path.
    enum Quoting { ShellQuoting, PythonQuoting };

    /// What start() runs. The program is started when the console is
    /// first shown, or by Restart.
    void setProgram(const QString& program, const QStringList& args,
                    const QStringList& extraEnv = {});
    /// The directory the program starts in; empty: the project directory
    /// when a project is open, otherwise the home directory.
    void setWorkingDirectory(const QString& dir);
    /// The program's command for changing its directory, with %1 for the
    /// path quoted the given way - "cd %1", or "import os; os.chdir(%1)" -
    /// behind the "Project dir" button. Empty hides the button.
    void setChangeDirectoryCommand(const QString& command, Quoting quoting = ShellQuoting);

    QString program() const { return a_program; }
    bool isRunning() const;
    QPlainTextEdit* outputView() const { return a_output; }
    QLineEdit* inputLine() const { return a_input; }
    QString outputText() const;
    ConsoleProcess* process() const { return a_process; }

    /// The directory the program starts in, resolved (see
    /// setWorkingDirectory()).
    QString startDirectory() const;
    /// A path quoted for the program's command line, as the change-
    /// directory command needs it: single quotes for a shell, a string
    /// literal for Python.
    static QString quotedForShell(const QString& path);
    static QString quotedForPython(const QString& path);

public slots:
    /// Starts the program unless it is running; true when it runs.
    bool start();
    void stop();
    void restart();
    void interrupt();
    void clear();
    /// Sends a line of input, newline appended; starts the program first
    /// when it is not running.
    void sendLine(const QString& line);
    void changeToProjectDirectory();

signals:
    void started();
    void finished(int exitCode);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;

private slots:
    void slotOutput(const QByteArray& data);
    void slotFinished(int exitCode);
    void slotSend();

private:
    void appendOutput(const QString& text);
    void appendNote(const QString& text);
    void setStatus(const QString& text);
    void historyStep(int direction);

    ConsoleProcess* a_process;
    QPlainTextEdit* a_output;
    QLineEdit* a_input;
    QLabel* a_status;
    QPushButton* a_buttonInterrupt;
    QPushButton* a_buttonRestart;
    QPushButton* a_buttonClear;
    QPushButton* a_buttonProjectDir;

    QString a_program;
    QStringList a_args;
    QStringList a_env;
    QString a_workDir;
    QString a_cdCommand;
    Quoting a_cdQuoting = ShellQuoting;

    QStringList a_history;
    int a_historyPos = 0;
    QString a_historyDraft;      // what was being typed before Up

    QStringDecoder a_decoder;
    enum EscapeState { Plain, Escape, Csi, Osc, OscEscape };
    EscapeState a_escape = Plain;
    bool a_overwriteLine = false;   // a carriage return: the next text replaces the line
};

#endif // PROCESSCONSOLE_H
