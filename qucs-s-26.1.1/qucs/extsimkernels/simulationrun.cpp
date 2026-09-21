/***************************************************************************
                           simulationrun.cpp
                           -----------------
    begin                : Sat Jan 10 2015 (as externsimdialog.cpp)
    copyright            : (C) 2015 by Vadim Kuznetsov
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

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QStyle>

#include "settings.h"
#include "misc.h"
#include "simulationrun.h"
#include "main.h"
#include "schematic.h"

SimulationRun::SimulationRun(Schematic* sch, bool netlist2Console, QObject* parent) :
    QObject(parent),
    a_schematic(sch),
    a_console(nullptr),
    a_statusLog(nullptr),
    a_progress(nullptr),
    a_ngspice(new Ngspice(sch,this)),
    a_xyce(new Xyce(sch,this)),
    a_wasSimulated(true),
    a_hasError(false),
    a_netlist2Console(netlist2Console),
    a_running(false)
{
    const QString workdir(misc::scratchDirFor(sch != nullptr ? sch->getDocName() : QString()));
    QFileInfo inf(workdir);
    if (!inf.exists()) {
        QDir dir;
        dir.mkpath(workdir);
    }
    setSimulator();
}

SimulationRun::~SimulationRun()
{
    a_ngspice->killThemAll();
    a_xyce->killThemAll();
}

void SimulationRun::attach(QPlainTextEdit* console, QListWidget* statusLog, QProgressBar* progress)
{
    a_console = console;
    a_statusLog = statusLog;
    a_progress = progress;
    a_ngspice->setConsole(console);
    a_xyce->setConsole(console);
    if (progress != nullptr) {
        connect(a_ngspice,SIGNAL(progress(int)),progress,SLOT(setValue(int)));
        connect(a_xyce,SIGNAL(progress(int)),progress,SLOT(setValue(int)));
    }
}

void SimulationRun::setSimulator()
{
    switch (QucsSettings.DefaultSimulator) {
    case spicecompat::simNgspice: {
        a_xyce->setParallel(false);
        connect(a_ngspice,SIGNAL(started()),this,SLOT(slotNgspiceStarted()));
        connect(a_ngspice,SIGNAL(finished()),this,SLOT(slotProcessOutput()));
        connect(a_ngspice,SIGNAL(errors(QProcess::ProcessError)),this,SLOT(slotNgspiceStartError(QProcess::ProcessError)));
        QString cmd;
        if (QFileInfo(QucsSettings.NgspiceExecutable).isRelative()) { // this check is related to MacOS
            cmd = QFileInfo(QucsSettings.BinDir + QucsSettings.NgspiceExecutable).absoluteFilePath();
        } else {
            cmd = QFileInfo(QucsSettings.NgspiceExecutable).absoluteFilePath();
        }
        if (QFileInfo::exists(cmd)) {
            a_ngspice->setSimulatorCmd(cmd);
        } else {
            a_ngspice->setSimulatorCmd(QucsSettings.NgspiceExecutable); //rely on $PATH
        }
        a_ngspice->setSimulatorParameters(_settings::Get().item<QString>("NgspiceParams"));
    }
        break;
    case spicecompat::simXyce: {
        a_xyce->setParallel(false);
        connect(a_xyce,SIGNAL(started()),this,SLOT(slotNgspiceStarted()));
        connect(a_xyce,SIGNAL(finished()),this,SLOT(slotProcessOutput()));
        connect(a_xyce,SIGNAL(errors(QProcess::ProcessError)),this,SLOT(slotNgspiceStartError(QProcess::ProcessError)));
        a_xyce->setSimulatorParameters(_settings::Get().item<QString>("XyceParams"));
    }
        break;
    case spicecompat::simSpiceOpus: {
        a_xyce->setParallel(false);
        connect(a_ngspice,SIGNAL(started()),this,SLOT(slotNgspiceStarted()),Qt::UniqueConnection);
        connect(a_ngspice,SIGNAL(finished()),this,SLOT(slotProcessOutput()),Qt::UniqueConnection);
        connect(a_ngspice,SIGNAL(errors(QProcess::ProcessError)),this,SLOT(slotNgspiceStartError(QProcess::ProcessError)),Qt::UniqueConnection);
        a_ngspice->setSimulatorCmd(QucsSettings.SpiceOpusExecutable);
        a_ngspice->setSimulatorParameters(_settings::Get().item<QString>("NgspiceParams"));
    }
        break;
    default: break;
    }
}


void SimulationRun::slotProcessOutput()
{
    a_running = false;
    QString out;

    // Set temporary safe output name

    QString ext;
    switch (QucsSettings.DefaultSimulator) {
    case spicecompat::simNgspice:
        ext = ".dat.ngspice";
        out = a_ngspice->getOutput();
        break;
    case spicecompat::simXyce:
        ext = ".dat.xyce";
        out = a_xyce->getOutput();
        break;
    case spicecompat::simSpiceOpus:
        out = a_ngspice->getOutput();
        ext = ".dat.spopus";
        break;
    default:
        out = "dummy";
        ext = ".dat";
        break;
    }

    const QStyle *style = QApplication::style();
    if (a_schematic.isNull()) {
        // The document was closed while the simulator ran (the run is not
        // modal); its result has nowhere to go.
        addLogEntry(tr("The schematic was closed during the simulation; the result is discarded."),
                    style->standardIcon(QStyle::SP_MessageBoxWarning));
        a_hasError = true;
        a_wasSimulated = false;
    } else if (logContainsError(out)) {
        addLogEntry(tr("There were simulation errors. Please check log."),
                    style->standardIcon(QStyle::SP_MessageBoxCritical));
        a_hasError = true;
        a_wasSimulated = false;
        emit warnings();
    } else if (logContainsWarning(out)) {
        addLogEntry(tr("There were simulation warnings. Please check log."),
                    style->standardIcon(QStyle::SP_MessageBoxWarning));
        addLogEntry(tr("Simulation finished. Now place diagram on schematic to plot the result."),
                    QIcon(":/bitmaps/svg/ok_apply.svg"));
        emit warnings();
    } else  {
        if ( !a_hasError ) {
            addLogEntry(tr("Simulation successful. Now place diagram on schematic to plot the result."),
                    QIcon(":/bitmaps/svg/ok_apply.svg"));
            emit success();
        }
    }
    saveLog();
    if (a_console != nullptr)
        a_console->insertPlainText("Simulation finished\n");

    if ( !a_hasError && !a_schematic.isNull() ) {
        QFileInfo inf(a_schematic->getDocName());
        QString qucs_dataset = inf.canonicalPath()+QDir::separator()+inf.completeBaseName()+ext;
        switch (QucsSettings.DefaultSimulator) {
            case spicecompat::simNgspice:
            case spicecompat::simSpiceOpus:
                a_ngspice->convertToQucsData(qucs_dataset);
                break;
            case spicecompat::simXyce:
                a_xyce->convertToQucsData(qucs_dataset);
                break;
            default:
                break;
        }
    }
    emit simulated(this);
}


void SimulationRun::slotNgspiceStarted()
{
    if (a_console != nullptr) {
        a_console->clear();
        QString sim = spicecompat::getDefaultSimulatorName(QucsSettings.DefaultSimulator);
        a_console->insertPlainText(sim + tr(" started...\n"));
    }
    addLogEntry(tr("Simulation started on: ") + QDateTime::currentDateTime().toString(),
                QApplication::style()->standardIcon(QStyle::SP_MessageBoxInformation));
    emit started();
}

void SimulationRun::slotNgspiceStartError(QProcess::ProcessError err)
{
    QString msg;
    AbstractSpiceKernel *kernel = QucsSettings.DefaultSimulator == spicecompat::simXyce
                                      ? static_cast<AbstractSpiceKernel*>(a_xyce)
                                      : static_cast<AbstractSpiceKernel*>(a_ngspice);
    switch (err) {
    case QProcess::FailedToStart:
        msg = tr("Failed to start simulator \"%1\": %2").arg(kernel->simulatorCommand(), kernel->processErrorString());
        break;
    case QProcess::Crashed:
        msg = tr("Simulator crashed!");
        break;
    default:
        msg = tr("Simulator error!");
    }

    addLogEntry(msg,QApplication::style()->standardIcon(QStyle::SP_MessageBoxCritical));

    QString sim = spicecompat::getDefaultSimulatorName(QucsSettings.DefaultSimulator);
    if (a_console != nullptr)
        a_console->insertPlainText(sim + tr(" error..."));

    a_wasSimulated = false;
    a_hasError = true;
    if (err == QProcess::FailedToStart) {
        // No process, so no finished(): the run is over here.
        a_running = false;
        emit simulated(this);
    }
}

void SimulationRun::start()
{
    a_running = true;
    a_wasSimulated = true;
    a_hasError = false;
    if (a_progress != nullptr) a_progress->setValue(0);
    switch (QucsSettings.DefaultSimulator) {
    case spicecompat::simNgspice:
        a_ngspice->slotSimulate();
        break;
    case spicecompat::simXyce:
        a_xyce->slotSimulate();
        break;
    case spicecompat::simSpiceOpus:
        a_ngspice->slotSimulate();
        break;
    default:
        a_running = false;
        break;
    }
}

void SimulationRun::stop()
{
    if (!a_running) return;
    addLogEntry(tr("Simulation stopped."), QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning));
    a_hasError = true;      // no result to convert
    a_wasSimulated = false;
    a_ngspice->killThemAll();   // the kernel's finished() follows and ends the run
    a_xyce->killThemAll();
}

void SimulationRun::saveNetlist()
{
    if (a_schematic.isNull()) return;
    QFileInfo inf(a_schematic->getDocName());
    QString filename;

    if (!a_netlist2Console)
    {
        filename = QFileDialog::getSaveFileName(
                nullptr,
                tr("Save netlist"),
                inf.path() + QDir::separator() + "netlist.cir",
                "All files (*)");
        if (filename.isEmpty())
        {
            return;
        }
    }

    switch (QucsSettings.DefaultSimulator)
    {
        case spicecompat::simNgspice:
        case spicecompat::simSpiceOpus:
            a_ngspice->SaveNetlist(filename, a_netlist2Console);
            break;
        case spicecompat::simXyce:
            a_xyce->SaveNetlist(filename, a_netlist2Console);
            break;
        default:
            break;
    }

    if (!a_netlist2Console && !QFile::exists(filename))
    {
        QMessageBox::critical(
                nullptr,
                QObject::tr("Save netlist"),
                QObject::tr("Disk write error!"), QMessageBox::Ok);
    }
}

void SimulationRun::saveLog()
{
    if (a_console == nullptr) return;
    // Next to the schematic's netlist and raw output, in its Scratch folder.
    QString filename = misc::scratchDirFor(a_schematic != nullptr ? a_schematic->getDocName() : QString()) + QDir::separator() + "log.txt";
    QFile log(filename);
    if (log.open(QIODevice::WriteOnly)) {
        QTextStream ts_log(&log);
        ts_log<<a_console->toPlainText();
        log.flush();
        log.close();
    }
}

void SimulationRun::addLogEntry(const QString &text, const QIcon &icon)
{
    if (a_statusLog == nullptr) return;
    QListWidgetItem *itm = new QListWidgetItem;
    itm->setText(text);
    itm->setIcon(icon);
    a_statusLog->addItem(itm);
    a_statusLog->scrollToBottom();
}

bool SimulationRun::logContainsError(const QString &out)
{
    bool found = false;
    QStringList err_patterns;
    switch (QucsSettings.DefaultSimulator) {
    case spicecompat::simNgspice:
        err_patterns<<"Error:"<<"ERROR"<<"Error "
                    <<"Syntax error:"<<"Expression err:"
                    <<"errors:"<<"simulation(s) aborted"
                    <<"simulation aborted"<<"analysis aborted";
        break;
    case spicecompat::simXyce:
        err_patterns<<"Error:"<<"ERROR"<<"MSG_ERROR"
                    <<"error:"<<"MSG_FATAL";
        break;
    default: err_patterns<<"error";
        break;
    }
    for(const auto &err_str: err_patterns) {
        if (out.contains(err_str)) {
            found = true;
            break;
        }
    }
    return found;
}

bool SimulationRun::logContainsWarning(const QString &out)
{
    bool found = false;
    QStringList warn_patterns;
    switch (QucsSettings.DefaultSimulator) {
    case spicecompat::simNgspice:
        warn_patterns<<"Warning:"<<"WARNING"<<"Warning "
                    <<"warning:";
        break;
    case spicecompat::simXyce:
        warn_patterns<<"Warning:"<<"WARNING"<<"Warning "
                    <<"warning:";
        break;
    default: warn_patterns<<"warning";
        break;
    }
    for(const auto &warn_str: warn_patterns) {
        if (out.contains(warn_str)) {
            found = true;
            break;
        }
    }
    return found;
}
