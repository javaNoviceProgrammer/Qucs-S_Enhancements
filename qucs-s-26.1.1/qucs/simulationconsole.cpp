/*
 * simulationconsole.cpp - the dockable simulation console
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
#include "schematic.h"
#include "messagedock.h"
#include "extsimkernels/simulationrun.h"

#include <QApplication>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QStyle>
#include <QVBoxLayout>

SimulationConsole::SimulationConsole(QucsApp* app)
    : QWidget(),
      a_dock(new QDockWidget(tr("Simulation"), app)),
      a_console(new QPlainTextEdit(this)),
      a_statusLog(new QListWidget(this)),
      a_progress(new QProgressBar(this)),
      a_buttonStop(new QPushButton(tr("Stop"), this)),
      a_buttonSaveNetlist(new QPushButton(tr("Save netlist"), this)),
      a_buttonClear(new QPushButton(tr("Clear"), this))
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

    auto* split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(a_console);
    split->addWidget(a_statusLog);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(a_buttonStop);
    buttons->addWidget(a_buttonSaveNetlist);
    buttons->addWidget(a_buttonClear);
    buttons->addStretch();
    buttons->addWidget(a_progress, 1);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(split, 1);
    layout->addLayout(buttons);

    connect(a_buttonStop, &QPushButton::clicked, this, &SimulationConsole::slotStop);
    connect(a_buttonSaveNetlist, &QPushButton::clicked, this, &SimulationConsole::slotSaveNetlist);
    connect(a_buttonClear, &QPushButton::clicked, this, &SimulationConsole::clear);
    setRunning(false);

    a_dock->setObjectName(QStringLiteral("SimulationConsoleDock"));
    a_dock->setWidget(this);
    a_dock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    app->addDockWidget(Qt::BottomDockWidgetArea, a_dock);
    // Share the bottom with the build-message dock as tabs rather than
    // stacking two docks there.
    if (app->messages() != nullptr && app->messages()->msgDock != nullptr)
        app->tabifyDockWidget(app->messages()->msgDock, a_dock);
    a_dock->hide();   // until the first simulation, or View > Simulation Console
}

bool SimulationConsole::isRunning() const
{
    return !a_run.isNull() && a_run->isRunning();
}

SimulationRun* SimulationConsole::startRun(Schematic* schematic, bool showDockNow)
{
    if (!a_run.isNull()) {
        // A run is still going, or its result is still being handled.
        auto* item = new QListWidgetItem(QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning),
                                         tr("A simulation is already running; stop it or wait for it to finish."));
        a_statusLog->addItem(item);
        a_statusLog->scrollToBottom();
        if (showDockNow) showDock();
        return nullptr;
    }
    a_run = new SimulationRun(schematic, false, this);
    a_run->attach(a_console, a_statusLog, a_progress);
    connect(a_run, &SimulationRun::started, this, &SimulationConsole::slotRunStarted);
    connect(a_run, &SimulationRun::simulated, this, &SimulationConsole::slotRunSimulated);
    // A closed document takes its run down with it.
    connect(schematic, &QObject::destroyed, a_run, &SimulationRun::stop);
    setRunning(true);
    if (showDockNow) showDock();
    return a_run;
}

void SimulationConsole::showDock()
{
    a_dock->show();
    a_dock->raise();
}

void SimulationConsole::clear()
{
    a_console->clear();
    a_statusLog->clear();
    a_progress->setValue(0);
}

void SimulationConsole::slotRunStarted()
{
    setRunning(true);
}

void SimulationConsole::slotRunSimulated(SimulationRun* run)
{
    // QucsApp handles the result in its own slot (connected before this
    // one is reached? no: connection order is creation order, so the app's
    // handler, connected after startRun(), runs after this). Free the run
    // once every handler is done.
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
