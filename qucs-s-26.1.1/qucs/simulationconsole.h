/*
 * simulationconsole.h - the simulation console, in a dock or a window
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
class QAction;
class QDialog;
class QDockWidget;
class QPlainTextEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QSplitter;

/*!
 * \brief The simulation console: the external simulator's output, the
 *        status of the run and a progress bar, with Stop and Save-netlist
 *        buttons.
 *
 * It lives in one of two hosts, as QucsSettings.SimulationConsoleHost
 * says: the "Simulation" dock at the bottom of the main window, or the
 * "Simulate with external simulator" window - either as a window of its
 * own, with the simulation in the background and the application usable,
 * or as the legacy window of earlier versions, which is run modally and
 * stops the simulation when it is closed.
 * One SimulationRun lives here at a time; startRun() refuses a second
 * while the first is going, and the run is deleted once its result has
 * been handled (QucsApp::slotAfterSpiceSimulation()).
 */
class SimulationConsole : public QWidget
{
    Q_OBJECT
public:
    explicit SimulationConsole(QucsApp* app);

    /// The dock host, tabified with the build-message dock.
    QDockWidget* dock() const { return a_dock.data(); }
    /// The window host.
    QDialog* window() const { return a_window.data(); }
    /// The host the console is in now.
    QWidget* host() const;
    bool inDock() const;
    /// The window, in the legacy (modal) mode.
    bool isLegacyWindow() const;
    /// View > Simulation Console: shows or hides the current host, and
    /// is checked while that host is shown.
    QAction* viewAction() const { return a_viewAction; }

    QPlainTextEdit* console() const { return a_console; }
    QListWidget* statusLog() const { return a_statusLog; }

    /// The run in progress, or nullptr.
    SimulationRun* currentRun() const { return a_run; }
    bool isRunning() const;

    /// Creates the run for a schematic, attached to this console, and
    /// brings the console up when \a showConsoleNow - except in the
    /// legacy-window mode, where runLegacyWindow() does that once the run
    /// was started. Returns nullptr (with a message in the status log)
    /// while another run is in progress. The caller connects to the run's
    /// signals and calls SimulationRun::start().
    SimulationRun* startRun(Schematic* schematic, bool showConsoleNow);
    /// Legacy-window mode, after startRun(..., true) and the start of the
    /// run: runs the window modally until it is closed, which stops a run
    /// still going - as the dialog of earlier versions did. A no-op in the
    /// other modes, and when the run was not to be shown.
    void runLegacyWindow();

public slots:
    /// Shows and raises the host.
    void showConsole();
    /// Moves the console into the dock or the window, whichever
    /// QucsSettings.SimulationConsoleHost names, and sets the window up
    /// for the plain or the legacy mode; a run in progress comes along.
    /// Called after the simulator settings were changed.
    void applyHostSetting();
    void clear();

signals:
    /// The "Save netlist" button while no run is in progress.
    void saveNetlistRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void slotRunStarted();
    void slotRunSimulated(SimulationRun* run);
    void slotStop();
    void slotSaveNetlist();
    void slotToggleView(bool show);
    void slotWindowFinished();

private:
    void setRunning(bool running);
    void syncViewAction();
    void setUpWindow();

    QSplitter* a_split;

    // Children of the main window, which deletes them before the console
    // when the console is in the one deleted last: QPointers, so that the
    // Hide event of a host going down can be told apart from a real one.
    QPointer<QDockWidget> a_dock;
    QPointer<QDialog> a_window;
    QAction* a_viewAction;
    QPlainTextEdit* a_console;
    QListWidget* a_statusLog;
    QProgressBar* a_progress;
    QPushButton* a_buttonStop;
    QPushButton* a_buttonSaveNetlist;
    QPushButton* a_buttonClear;
    QPushButton* a_buttonClose;   // window host only: "Close", or "Exit" in the legacy mode
    QPointer<SimulationRun> a_run;
    bool a_legacyShowPending = false;   // startRun() asked for the window; runLegacyWindow() shows it
};

#endif // SIMULATIONCONSOLE_H
