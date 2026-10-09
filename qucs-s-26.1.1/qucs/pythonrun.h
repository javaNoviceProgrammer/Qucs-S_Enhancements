/*
 * pythonrun.h - a Python script run as a program of its own, its output in
 *               a console (the Python toolbar's Run)
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
#include <QString>
#include <QStringDecoder>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QProcess;
class QPushButton;

/*!
 * \brief The console of a script run: its output as it comes - standard
 *        output and error together, in their order -, a line at the end
 *        with its exit code and its time, and Stop and Clear.
 *
 * The script runs with the interpreter given, unbuffered (-u), in its own
 * folder, with nothing on its standard input (input() ends the file). A
 * traceback's lines that name a file and a line (File "x.py", line 3) are
 * links: a click asks for that place (locationRequested()).
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
    bool isRunning() const;
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

signals:
    void started();
    void finished(int exitCode);
    /// A traceback's place was clicked: \a line (1-based) of \a file.
    void locationRequested(const QString& file, int line);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void readOutput();
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
};

#endif // QUCS_PYTHONRUN_H
