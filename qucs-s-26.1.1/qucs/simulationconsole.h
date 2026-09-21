/*
 * simulationconsole.h - the dockable simulation console
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef SIMULATIONCONSOLE_H
#define SIMULATIONCONSOLE_H

#include <QWidget>
#include <QPointer>

#include "extsimkernels/simulationrun.h"   // QPointer<SimulationRun> needs the type

class QucsApp;
class Schematic;
class QDockWidget;
class QPlainTextEdit;
class QListWidget;
class QProgressBar;
class QPushButton;

/*!
 * \brief The simulation console: a dock at the bottom of the main window
 *        that shows the external simulator's output, the status of the
 *        run and a progress bar, with Stop and Save-netlist buttons.
 *
 * It replaces the modal "Simulate with external simulator" dialog: a
 * simulation runs in the background and the application stays usable.
 * One SimulationRun lives here at a time; startRun() refuses a second
 * while the first is going, and the run is deleted once its result has
 * been handled (QucsApp::slotAfterSpiceSimulation()).
 */
class SimulationConsole : public QWidget
{
    Q_OBJECT
public:
    explicit SimulationConsole(QucsApp* app);

    QDockWidget* dock() const { return a_dock; }
    QPlainTextEdit* console() const { return a_console; }
    QListWidget* statusLog() const { return a_statusLog; }

    /// The run in progress, or nullptr.
    SimulationRun* currentRun() const { return a_run; }
    bool isRunning() const;

    /// Creates the run for a schematic, attached to this console, and
    /// shows the dock when \a showDock. Returns nullptr (with a message in
    /// the status log) while another run is in progress. The caller
    /// connects to the run's signals and calls SimulationRun::start().
    SimulationRun* startRun(Schematic* schematic, bool showDock);

public slots:
    /// Shows and raises the dock.
    void showDock();
    void clear();

signals:
    /// The "Save netlist" button while no run is in progress.
    void saveNetlistRequested();

private slots:
    void slotRunStarted();
    void slotRunSimulated(SimulationRun* run);
    void slotStop();
    void slotSaveNetlist();

private:
    void setRunning(bool running);

    QDockWidget* a_dock;
    QPlainTextEdit* a_console;
    QListWidget* a_statusLog;
    QProgressBar* a_progress;
    QPushButton* a_buttonStop;
    QPushButton* a_buttonSaveNetlist;
    QPushButton* a_buttonClear;
    QPointer<SimulationRun> a_run;
};

#endif // SIMULATIONCONSOLE_H
