/***************************************************************************
                           simulationrun.h
                           ---------------
    begin                : Sun Nov 9 2014 (as ngspicesimdialog.h)
    copyright            : (C) 2014 by Vadim Kuznetsov
    email                : ra3xdh@gmail.com
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef SIMULATIONRUN_H
#define SIMULATIONRUN_H

#include <QObject>
#include <QPointer>
#include <QProcess>

#include "ngspice.h"
#include "xyce.h"

class Schematic;
class QPlainTextEdit;
class QListWidget;
class QProgressBar;

/*!
 * \brief One simulation of one schematic with the external SPICE kernel
 *        from the settings (ngspice, Xyce or SPICE OPUS): the netlist, the
 *        simulator process, its output and the conversion of the result to
 *        a Qucs dataset.
 *
 * This used to be the modal "Simulate with external simulator" dialog
 * (ExternSimDialog). The window is gone: the run writes into the widgets
 * of the SimulationConsole dock it is attached to and reports through
 * signals, so the application stays usable while the simulator works. In
 * netlist mode (no widgets attached) it only produces the netlist.
 */
class SimulationRun : public QObject
{
    Q_OBJECT

private:
    QPointer<Schematic> a_schematic;   // may be closed while the simulator runs

    QPlainTextEdit *a_console;
    QListWidget *a_statusLog;
    QProgressBar *a_progress;

    Ngspice *a_ngspice;
    Xyce *a_xyce;

    bool a_wasSimulated;
    bool a_hasError;
    bool a_netlist2Console;
    bool a_running;

public:
    explicit SimulationRun(Schematic* sch, bool netlist2Console, QObject* parent = nullptr);
    ~SimulationRun();

    /// The widgets that show the simulator's output (the console dock's).
    void attach(QPlainTextEdit* console, QListWidget* statusLog, QProgressBar* progress);

    bool wasSimulated() const { return a_wasSimulated; }
    /// The schematic this run simulates, nullptr once it has been closed.
    Schematic* schematic() const { return a_schematic; }
    bool hasError() const { return a_hasError; }
    /// From start() until the simulator has finished (or failed to start).
    bool isRunning() const { return a_running; }

private:
    void saveLog();
    void addLogEntry(const QString&text, const QIcon &icon);
    bool logContainsError(const QString &out);
    bool logContainsWarning(const QString &out);
    void setSimulator();

signals:
    /// The simulator process is running.
    void started();
    /// The run is over: the result (if any) has been converted to the
    /// dataset. Handled by QucsApp::slotAfterSpiceSimulation().
    void simulated(SimulationRun *);
    void warnings();
    void success();

public:
    /// Writes the netlist of the schematic to \a filename (a SPICE
    /// simulator's: ngspice, SPICE OPUS or Xyce) without asking anything;
    /// false when the simulator is not one of those or the file was not
    /// written.
    bool writeNetlist(const QString& filename);

public slots:
    void saveNetlist();
    void start();
    void stop();

private slots:
    void slotProcessOutput();
    void slotNgspiceStarted();
    void slotNgspiceStartError(QProcess::ProcessError err);
};

#endif // SIMULATIONRUN_H
