/*
 * simulationconsole.cpp - the simulation console, in a dock or a window
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "simulationconsole.h"

#include "qucs.h"
#include "main.h"
#include "settings.h"
#include "schematic.h"
#include "messagedock.h"
#include "extsimkernels/simulationrun.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDockWidget>
#include <QEvent>
#include <QHBoxLayout>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStyle>
#include <QVBoxLayout>

namespace {
// The window's size and position, under the key the old dialog used.
const char* const WindowGeometryKey = "ExternSimDialog/geometry";
}

SimulationConsole::SimulationConsole(QucsApp* app)
    : QWidget(),
      a_dock(new QDockWidget(tr("Simulation"), app)),
      a_window(new QDialog(app)),
      a_viewAction(new QAction(tr("&Simulation Console"), this)),
      a_console(new QPlainTextEdit(this)),
      a_statusLog(new QListWidget(this)),
      a_progress(new QProgressBar(this)),
      a_buttonStop(new QPushButton(tr("Stop"), this)),
      a_buttonSaveNetlist(new QPushButton(tr("Save netlist"), this)),
      a_buttonClear(new QPushButton(tr("Clear"), this)),
      a_buttonClose(new QPushButton(tr("Close"), this))
{
    QFont font;
    font.setFamily("monospace");
    font.setStyleHint(QFont::Monospace);
    font.setPointSize(10);
    a_console->setFont(font);
    a_console->setReadOnly(true);
    a_console->setLineWrapMode(QPlainTextEdit::NoWrap);
    a_statusLog->setSelectionMode(QAbstractItemView::NoSelection);
    a_statusLog->setWordWrap(true);
    a_progress->setRange(0, 100);
    a_progress->setValue(0);

    a_split = new QSplitter(Qt::Horizontal, this);
    a_split->addWidget(a_console);
    a_split->addWidget(a_statusLog);
    a_split->setStretchFactor(0, 3);
    a_split->setStretchFactor(1, 2);

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(a_buttonStop);
    buttons->addWidget(a_buttonSaveNetlist);
    buttons->addWidget(a_buttonClear);
    buttons->addStretch();
    buttons->addWidget(a_progress, 1);
    buttons->addWidget(a_buttonClose);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(a_split, 1);
    layout->addLayout(buttons);

    connect(a_buttonStop, &QPushButton::clicked, this, &SimulationConsole::slotStop);
    connect(a_buttonSaveNetlist, &QPushButton::clicked, this, &SimulationConsole::slotSaveNetlist);
    connect(a_buttonClear, &QPushButton::clicked, this, &SimulationConsole::clear);
    setRunning(false);

    // The dock host: at the bottom, sharing the space with the
    // build-message dock as tabs rather than stacking two docks there.
    a_dock->setObjectName(QStringLiteral("SimulationConsoleDock"));
    a_dock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    app->addDockWidget(Qt::BottomDockWidgetArea, a_dock);
    if (app->messages() != nullptr && app->messages()->msgDock != nullptr)
        app->tabifyDockWidget(app->messages()->msgDock, a_dock);
    a_dock->hide();   // until the first simulation, or View > Simulation Console
    a_dock->installEventFilter(this);

    // The window host: the simulation dialog of earlier versions - run
    // modally in the legacy mode, a window of its own otherwise.
    a_window->setObjectName(QStringLiteral("SimulationConsoleWindow"));
    a_window->setWindowTitle(tr("Simulate with external simulator"));
    a_window->setMinimumWidth(500);
    a_window->setSizeGripEnabled(true);
    auto* windowLayout = new QVBoxLayout(a_window);
    windowLayout->setContentsMargins(6, 6, 6, 6);
    a_window->resize(720, 420);
    a_window->restoreGeometry(QucsSettingsFile().value(QLatin1String(WindowGeometryKey)).toByteArray());
    a_window->installEventFilter(this);
    // Close / Exit, Escape and the title bar all end in reject(): the
    // window hides, and in the legacy mode the run is stopped with it.
    connect(a_buttonClose, &QPushButton::clicked, a_window, &QDialog::reject);
    connect(a_window, &QDialog::finished, this, &SimulationConsole::slotWindowFinished);

    a_viewAction->setCheckable(true);
    a_viewAction->setStatusTip(tr("Shows/hides the simulation console"));
    connect(a_viewAction, &QAction::triggered, this, &SimulationConsole::slotToggleView);

    // Into the host the setting names.
    if (QucsSettings.SimulationConsoleHost == tQucsSettings::SimConsoleDock)
        a_dock->setWidget(this);
    else
        windowLayout->addWidget(this);
    setUpWindow();
    syncViewAction();
}

bool SimulationConsole::inDock() const
{
    return a_dock->widget() == this;
}

bool SimulationConsole::isLegacyWindow() const
{
    return !inDock() && QucsSettings.SimulationConsoleHost == tQucsSettings::SimConsoleLegacyWindow;
}

QWidget* SimulationConsole::host() const
{
    return inDock() ? static_cast<QWidget*>(a_dock) : static_cast<QWidget*>(a_window);
}

bool SimulationConsole::isRunning() const
{
    return !a_run.isNull() && a_run->isRunning();
}

SimulationRun* SimulationConsole::startRun(Schematic* schematic, bool showConsoleNow)
{
    if (!a_run.isNull()) {
        // A run is still going, or its result is still being handled.
        auto* item = new QListWidgetItem(QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning),
                                         tr("A simulation is already running; stop it or wait for it to finish."));
        a_statusLog->addItem(item);
        a_statusLog->scrollToBottom();
        if (showConsoleNow) showConsole();
        return nullptr;
    }
    a_run = new SimulationRun(schematic, false, this);
    a_run->attach(a_console, a_statusLog, a_progress);
    connect(a_run, &SimulationRun::started, this, &SimulationConsole::slotRunStarted);
    connect(a_run, &SimulationRun::simulated, this, &SimulationConsole::slotRunSimulated);
    // A closed document takes its run down with it.
    connect(schematic, &QObject::destroyed, a_run, &SimulationRun::stop);
    setRunning(true);
    if (isLegacyWindow())
        a_legacyShowPending = showConsoleNow;   // runLegacyWindow(), once the run goes
    else if (showConsoleNow)
        showConsole();
    return a_run;
}

void SimulationConsole::runLegacyWindow()
{
    if (!isLegacyWindow() || !a_legacyShowPending) return;
    a_legacyShowPending = false;
    a_window->exec();   // until closed; slotWindowFinished() stops a run still going
}

void SimulationConsole::showConsole()
{
    QWidget* h = host();
    h->show();
    h->raise();
    if (h == a_window) a_window->activateWindow();
}

void SimulationConsole::applyHostSetting()
{
    const bool toDock = QucsSettings.SimulationConsoleHost == tQucsSettings::SimConsoleDock;
    if (toDock == inDock()) {
        setUpWindow();   // plain or legacy window
        return;
    }
    const bool wasShown = !host()->isHidden();
    if (toDock) {
        a_window->hide();
        a_window->layout()->removeWidget(this);
        a_dock->setWidget(this);        // reparents and shows the console
    } else {
        a_dock->hide();
        a_dock->setWidget(nullptr);     // hides the console as it lets go
        a_window->layout()->addWidget(this);
        setVisible(true);
    }
    setUpWindow();
    if (wasShown) showConsole();
    syncViewAction();
}

void SimulationConsole::setUpWindow()
{
    const bool legacy = isLegacyWindow();
    a_buttonClose->setVisible(!inDock());
    a_buttonClose->setText(legacy ? tr("Exit") : tr("Close"));
    // The legacy dialog had the status list under the console.
    a_split->setOrientation(legacy ? Qt::Vertical : Qt::Horizontal);
    if (!legacy) a_legacyShowPending = false;
}

void SimulationConsole::slotWindowFinished()
{
    // The legacy dialog took the simulation down with it.
    if (isLegacyWindow() && isRunning()) a_run->stop();
}

void SimulationConsole::clear()
{
    a_console->clear();
    a_statusLog->clear();
    a_progress->setValue(0);
}

bool SimulationConsole::eventFilter(QObject* watched, QEvent* event)
{
    // The hosts are deleted with the main window, the console with one of
    // them; a host's last Hide, on its way out, is not of interest.
    if (!a_dock.isNull() && !a_window.isNull()
        && (watched == a_dock || watched == a_window)
        && (event->type() == QEvent::Show || event->type() == QEvent::Hide)) {
        if (watched == a_window && event->type() == QEvent::Hide)
            QucsSettingsFile().setValue(QLatin1String(WindowGeometryKey), a_window->saveGeometry());
        syncViewAction();
    }
    return QWidget::eventFilter(watched, event);
}

void SimulationConsole::slotToggleView(bool show)
{
    if (show)
        showConsole();
    else
        host()->hide();
}

void SimulationConsole::syncViewAction()
{
    // As a dock's own toggle action does: checked unless explicitly hidden
    // (a dock behind another tab is not hidden).
    a_viewAction->setChecked(!host()->isHidden());
}

void SimulationConsole::slotRunStarted()
{
    setRunning(true);
}

void SimulationConsole::slotRunSimulated(SimulationRun* run)
{
    // QucsApp handles the result in its own slot (connected after
    // startRun(), so it runs after this one). Free the run once every
    // handler is done.
    setRunning(false);
    if (run == a_run) {
        run->deleteLater();
        a_run.clear();
    }
}

void SimulationConsole::slotStop()
{
    if (!a_run.isNull()) a_run->stop();
}

void SimulationConsole::slotSaveNetlist()
{
    if (!a_run.isNull())
        a_run->saveNetlist();
    else
        emit saveNetlistRequested();
}

void SimulationConsole::setRunning(bool running)
{
    a_buttonStop->setEnabled(running);
}
