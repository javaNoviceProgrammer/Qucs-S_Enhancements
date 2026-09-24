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
#include <QTextCursor>

#include <algorithm>

#include "settings.h"
#include "misc.h"
#include "simulationrun.h"
#include "main.h"
#include "schematic.h"
#include "ngoptimize.h"
#include "ngstatistics.h"
#include "qucs.h"
#include "textdoc.h"

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
    if (a_compiler != nullptr) {
        a_compiler->disconnect(this);
        a_compiler->kill();
        a_compiler->waitForFinished(2000);
    }
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
        configureNgspice(a_ngspice);
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


void SimulationRun::configureNgspice(Ngspice* kernel)
{
    QString cmd;
    if (QFileInfo(QucsSettings.NgspiceExecutable).isRelative()) { // this check is related to MacOS
        cmd = QFileInfo(QucsSettings.BinDir + QucsSettings.NgspiceExecutable).absoluteFilePath();
    } else {
        cmd = QFileInfo(QucsSettings.NgspiceExecutable).absoluteFilePath();
    }
    if (QFileInfo::exists(cmd)) {
        kernel->setSimulatorCmd(cmd);
    } else {
        kernel->setSimulatorCmd(QucsSettings.NgspiceExecutable); //rely on $PATH
    }
    kernel->setSimulatorParameters(_settings::Get().item<QString>("NgspiceParams"));
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

    a_warningCount = countWarnings(out);
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
    if (QucsSettings.DefaultSimulator == spicecompat::simNgspice && !a_schematic.isNull()) {
        reportNgOptimizations(out);
        reportNgStatistics(out);
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
        // After an optimization, or a compilation, the console keeps its
        // account of it.
        if (!a_afterOptimization && !a_keepConsole) a_console->clear();
        a_keepConsole = false;
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
    a_stopped = false;
    a_warningCount = 0;
    if (a_progress != nullptr) a_progress->setValue(0);
    // The Verilog-A modules the netlist uses whose library is missing or
    // out of date are compiled first; compileNext() starts again then.
    if (!a_compiled && !a_schematic.isNull() && QucsSettings.DefaultSimulator == spicecompat::simNgspice
        && startBuilds())
        return;
    a_compiled = false;
    if (a_optimizationAllowed && !a_schematic.isNull()
        && QucsSettings.DefaultSimulator == spicecompat::simNgspice) {
        for (Component* pc : a_schematic->a_DocComps)
            if (pc->Model == ".Opt" && pc->isActive == COMP_IS_ACTIVE) {
                startOptimization(pc);
                return;
            }
    }
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
    if (a_compiler != nullptr) {
        // slotCompiled() ends the run.
        a_builds.clear();
        a_compileStopped = true;
        a_stopped = true;
        addLogEntry(tr("Simulation stopped."), QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning));
        a_compiler->kill();
        return;
    }
    if (a_optimizer != nullptr && a_optimizer->isRunning()) {
        // The best point so far is still simulated (slotOptimized()); a
        // second stop stops that.
        addLogEntry(tr("Optimization stopped."), QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning));
        a_optimizer->stop();
        return;
    }
    addLogEntry(tr("Simulation stopped."), QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning));
    a_stopped = true;
    a_hasError = true;      // no result to convert
    a_wasSimulated = false;
    a_ngspice->killThemAll();   // the kernel's finished() follows and ends the run
    a_xyce->killThemAll();
}

bool SimulationRun::startBuilds()
{
    a_builds = a_ngspice->verilogABuilds();
    if (a_builds.isEmpty()) return false;
    const QStyle *style = QApplication::style();
    const QString openVAF = QucsSettings.OpenVAFExecutable.trimmed();
    if (openVAF.isEmpty() || !QFileInfo(openVAF).isExecutable()) {
        // Simulated with what there is; ngspice says what it misses.
        for (const qucs_s::osdi::Build& build : std::as_const(a_builds))
            addLogEntry(build.missing
                            ? tr("%1 is not compiled, and there is no OpenVAF to compile it "
                                 "(Application Settings > Locations > OpenVAF Path)")
                                  .arg(QDir::toNativeSeparators(build.source))
                            : tr("%1 is older than %2; with OpenVAF set (Application Settings > "
                                 "Locations > OpenVAF Path) it is compiled before a simulation")
                                  .arg(QDir::toNativeSeparators(build.library),
                                       QDir::toNativeSeparators(build.source)),
                        style->standardIcon(QStyle::SP_MessageBoxWarning));
        a_builds.clear();
        return false;
    }
    if (QucsMain != nullptr)
        for (const qucs_s::osdi::Build& build : std::as_const(a_builds))
            if (TextDoc *doc = QucsMain->findTextDoc(build.source); doc != nullptr && doc->getDocChanged())
                addLogEntry(tr("%1 has unsaved changes: compiled as saved").arg(QDir::toNativeSeparators(build.source)),
                            style->standardIcon(QStyle::SP_MessageBoxWarning));
    if (a_console != nullptr) {
        a_console->clear();
        a_console->insertPlainText(tr("Compiling the Verilog-A the circuit uses with OpenVAF:\n"));
    }
    compileNext();
    return true;
}

void SimulationRun::compileNext()
{
    if (a_builds.isEmpty()) {
        a_compiled = true;
        a_keepConsole = true;
        start();
        return;
    }
    const qucs_s::osdi::Build build = a_builds.takeFirst();
    const QString openVAF = QucsSettings.OpenVAFExecutable.trimmed();
    if (a_console != nullptr)
        a_console->insertPlainText(QStringLiteral("%1 %2\n").arg(openVAF, QDir::toNativeSeparators(build.source)));
    addLogEntry(build.missing ? tr("Compiling %1 (%2 has no library yet)")
                                    .arg(QDir::toNativeSeparators(build.source), build.modules.join(QStringLiteral(", ")))
                              : tr("Compiling %1 (changed since %2 was built)")
                                    .arg(QDir::toNativeSeparators(build.source),
                                         QFileInfo(build.library).fileName()),
                QApplication::style()->standardIcon(QStyle::SP_MessageBoxInformation));
    a_compiler = new QProcess(this);
    a_compiler->setProcessChannelMode(QProcess::MergedChannels);
    a_compiler->setWorkingDirectory(QFileInfo(build.source).absolutePath());
    connect(a_compiler, &QProcess::readyRead, this, &SimulationRun::slotCompilerOutput);
    connect(a_compiler, &QProcess::finished, this, &SimulationRun::slotCompiled);
    connect(a_compiler, &QProcess::errorOccurred, this, &SimulationRun::slotCompilerError);
    a_compiler->setProperty("source", build.source);
    a_compiler->start(openVAF, {build.source});
}

void SimulationRun::slotCompilerOutput()
{
    if (a_compiler == nullptr) return;
    const QString out = QString::fromLocal8Bit(a_compiler->readAll());
    if (a_console != nullptr && !out.isEmpty()) {
        a_console->moveCursor(QTextCursor::End);
        a_console->insertPlainText(out);
    }
}

void SimulationRun::slotCompiled(int exitCode, QProcess::ExitStatus status)
{
    slotCompilerOutput();
    const QString source = QDir::toNativeSeparators(a_compiler->property("source").toString());
    a_compiler->deleteLater();
    a_compiler = nullptr;
    if (a_compileStopped) {
        a_compileStopped = false;
        a_hasError = true;
        a_wasSimulated = false;
        a_running = false;
        emit simulated(this);
        return;
    }
    if (status != QProcess::NormalExit || exitCode != 0) {
        compileFailed(status == QProcess::NormalExit
                          ? tr("OpenVAF could not compile %1 (exit code %2); the simulation did not run.")
                                .arg(source).arg(exitCode)
                          : tr("OpenVAF crashed on %1; the simulation did not run.").arg(source));
        return;
    }
    compileNext();
}

void SimulationRun::slotCompilerError(QProcess::ProcessError error)
{
    // finished() does not follow a start failure.
    if (error != QProcess::FailedToStart || a_compiler == nullptr) return;
    const QString why = a_compiler->errorString();
    a_compiler->deleteLater();
    a_compiler = nullptr;
    compileFailed(tr("OpenVAF could not be started: %1").arg(why));
}

void SimulationRun::compileFailed(const QString& why)
{
    a_builds.clear();
    addLogEntry(why, QApplication::style()->standardIcon(QStyle::SP_MessageBoxCritical));
    if (a_console != nullptr) {
        a_console->moveCursor(QTextCursor::End);
        a_console->insertPlainText(QLatin1Char('\n') + why + QLatin1Char('\n'));
    }
    a_hasError = true;
    a_wasSimulated = false;
    a_running = false;
    emit simulated(this);
}

void SimulationRun::startOptimization(Component* optimization)
{
    using namespace qucs_s::optimization;
    const QStyle *style = QApplication::style();
    if (a_console != nullptr) a_console->clear();
    Problem problem;
    QString why;
    bool ok = Problem::read(optimization, &problem, &why);
    if (ok && !problem.simulation.isEmpty()) {
        ok = std::any_of(a_schematic->a_DocComps.begin(), a_schematic->a_DocComps.end(), [&](Component* c) {
            return c->isSimulation && c->Model != ".Opt" && c->Name.compare(problem.simulation, Qt::CaseInsensitive) == 0;
        });
        if (!ok) why = tr("%1 optimizes %2, which is not in the schematic").arg(problem.component, problem.simulation);
    }
    if (!ok) {
        addLogEntry(tr("Optimization impossible: %1").arg(why), style->standardIcon(QStyle::SP_MessageBoxCritical));
        if (a_console != nullptr) a_console->insertPlainText(tr("Optimization impossible: %1\n").arg(why));
        a_hasError = true;
        a_wasSimulated = false;
        a_running = false;
        saveLog();
        emit simulated(this);
        return;
    }

    a_optimizer = new Optimizer(a_schematic, problem, this);
    connect(a_optimizer, &Optimizer::message, this, [this](const QString& line) {
        if (a_console == nullptr) return;
        a_console->moveCursor(QTextCursor::End);
        a_console->insertPlainText(line + "\n");
        a_console->moveCursor(QTextCursor::End);
    });
    if (a_progress != nullptr) connect(a_optimizer, &Optimizer::progress, a_progress, &QProgressBar::setValue);
    connect(a_optimizer, &Optimizer::finished, this, &SimulationRun::slotOptimized);

    int goals = 0;
    for (const Goal& g : problem.goals) goals += g.type != GoalType::Monitor;
    int variables = 0;
    for (const Variable& v : problem.variables) variables += v.active;
    if (a_console != nullptr) {
        a_console->insertPlainText(
            tr("Optimization %1 of %2 with ngspice: %3, %4\n")
                .arg(problem.component,
                     problem.simulation.isEmpty() ? tr("all simulations") : problem.simulation,
                     variables == 1 ? tr("1 variable") : tr("%1 variables").arg(variables),
                     goals == 1 ? tr("1 goal") : tr("%1 goals").arg(goals)));
        a_console->insertPlainText(tr("%1, a population of %2, up to %3 generations, %4 simulations at a time\n")
                                       .arg(problem.settings.methodName())
                                       .arg(std::max(problem.settings.population, 6))
                                       .arg(problem.settings.generations)
                                       .arg(a_optimizer->parallel()));
    }
    addLogEntry(tr("Optimization started on: ") + QDateTime::currentDateTime().toString(),
                style->standardIcon(QStyle::SP_MessageBoxInformation));
    emit started();
    a_optimizer->start();
}

void SimulationRun::slotOptimized(bool ok)
{
    const QStyle *style = QApplication::style();
    if (!ok || a_schematic.isNull()) {
        addLogEntry(a_schematic.isNull() ? tr("The schematic was closed during the optimization.")
                                         : tr("Optimization failed. Please check log."),
                    style->standardIcon(QStyle::SP_MessageBoxCritical));
        a_hasError = true;
        a_wasSimulated = false;
        a_running = false;
        saveLog();
        emit simulated(this);
        return;
    }
    writeBackOptimum();
    addLogEntry(tr("Optimization finished: cost %1 after %2 simulations; simulating the best point.")
                    .arg(qucs_s::optimization::number(a_optimizer->bestCost()))
                    .arg(a_optimizer->simulations()),
                QIcon(":/bitmaps/svg/ok_apply.svg"));
    if (a_console != nullptr) a_console->insertPlainText(tr("Simulating the best point...\n"));
    a_afterOptimization = true;
    if (a_progress != nullptr) a_progress->setValue(0);
    // Every simulation of the schematic, with the best values: the
    // diagrams of the others keep theirs.
    const qucs_s::optimization::NetlistScope scope(a_schematic, a_optimizer->problem(), a_optimizer->bestValues(),
                                                   /*allSimulations=*/true);
    a_ngspice->setExtraParameters(scope.extraParameters());
    a_ngspice->setOperatingPointOnly(false);
    a_ngspice->slotSimulate();
}

void SimulationRun::writeBackOptimum()
{
    // The best values become the variables' initial values, as ASCO's do
    // with qucsator: the next run starts from them.
    const QMap<QString, double> best = a_optimizer->bestValues();
    Component* opt = nullptr;
    for (Component* c : a_schematic->a_DocComps)
        if (c->Model == ".Opt" && c->Name == a_optimizer->problem().component) opt = c;
    if (opt == nullptr) return;
    bool changed = false;
    for (int i = 2; i < opt->Props.size(); ++i) {
        Property* p = opt->Props.at(i);
        if (p->Name != "Var") continue;
        QStringList f = p->Value.split('|');
        if (f.size() < 3 || f.value(1).trimmed() == "no" || !best.contains(f.at(0).trimmed())) continue;
        const QString value = qucs_s::optimization::number(best.value(f.at(0).trimmed()));
        if (f.at(2) == value) continue;
        f[2] = value;
        p->Value = f.join('|');
        changed = true;
    }
    if (changed) a_schematic->setChanged(true, true);
}

void SimulationRun::reportNgOptimizations(const QString& out)
{
    using namespace qucs_s::ngopt;
    const QStringList names = a_ngspice->optimizations();
    if (names.isEmpty()) return;
    const QStyle *style = QApplication::style();
    if (unsupported(out)) {
        addLogEntry(tr("This ngspice has no optimize command: NgOpt needs an ngspice built with it "
                       "(Ngspice_OpenVAF_Enhancements). The simulations ran with the initial values."),
                    style->standardIcon(QStyle::SP_MessageBoxCritical));
        return;
    }
    const QList<Result> results = parseResults(out);
    bool changed = false;
    for (int i = 0; i < names.size(); ++i) {
        Component* c = nullptr;
        for (Component* pc : a_schematic->a_DocComps)
            if (pc->Model == ".NGOPT" && pc->Name == names.at(i)) c = pc;
        if (c == nullptr) continue;
        if (i >= results.size()) {
            addLogEntry(tr("%1: ngspice reported no optimum. Please check log.").arg(names.at(i)),
                        style->standardIcon(QStyle::SP_MessageBoxWarning));
            continue;
        }
        // The values found become the knobs' initial values, as the
        // Optimization component's do: the next run starts from them.
        const Result& r = results.at(i);
        Command command = Command::read(c);
        if (r.values.size() == command.knobs.size()) {
            bool mine = false;
            for (int k = 0; k < command.knobs.size(); ++k) {
                const QString value = misc::num2str(r.values.at(k).second, -1, QString());
                if (command.knobs.at(k).init == value) continue;
                command.knobs[k].init = value;
                mine = true;
            }
            if (mine) {
                command.write(c);
                changed = true;
            }
        }
        QString text = QStringLiteral("%1: %2").arg(names.at(i), r.summary);
        for (const QString& note : r.notes) text += QStringLiteral("\n") + note;
        const bool doubtful = r.interrupted || !r.notes.isEmpty();
        addLogEntry(text, doubtful ? style->standardIcon(QStyle::SP_MessageBoxWarning)
                                   : QIcon(":/bitmaps/svg/ok_apply.svg"));
    }
    if (changed) a_schematic->setChanged(true, true);
}

void SimulationRun::reportNgStatistics(const QString& out)
{
    using namespace qucs_s::ngstats;
    const QStyle *style = QApplication::style();
    for (const QString& name : a_ngspice->statistics()) {
        Component* c = nullptr;
        for (Component* pc : a_schematic->a_DocComps)
            if (isStatistics(pc) && pc->Name == name) c = pc;
        if (c == nullptr) continue;
        const bool corners = c->Model == QLatin1String(kCornersModel);
        if (unsupported(out, corners)) {
            addLogEntry(tr("This ngspice has no %1 command: %2 needs an ngspice built with it "
                           "(Ngspice_OpenVAF_Enhancements).")
                            .arg(corners ? QStringLiteral("corners") : QStringLiteral("montecarlo"), name),
                        style->standardIcon(QStyle::SP_MessageBoxCritical));
            continue;
        }
        const Summary summary = summarize(c, out, a_ngspice->workdir());
        addLogEntry(summary.text, summary.warning ? style->standardIcon(QStyle::SP_MessageBoxWarning)
                                                  : QIcon(":/bitmaps/svg/ok_apply.svg"));
    }
}

bool SimulationRun::writeNetlist(const QString& filename)
{
    if (a_schematic.isNull() || filename.isEmpty()) return false;
    switch (QucsSettings.DefaultSimulator)
    {
        case spicecompat::simNgspice:
        case spicecompat::simSpiceOpus:
            a_ngspice->SaveNetlist(filename, false);
            break;
        case spicecompat::simXyce:
            a_xyce->SaveNetlist(filename, false);
            break;
        default:
            return false;
    }
    return QFile::exists(filename);
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

int SimulationRun::countWarnings(const QString &out)
{
    int count = 0;
    for (const QStringView line : QStringView(out).split(QLatin1Char('\n')))
        if (logContainsWarning(line.toString())) ++count;
    return count;
}
