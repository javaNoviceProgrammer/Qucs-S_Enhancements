/***************************************************************************
                               ngspice.cpp
                             ----------------
    begin                : Sat Jan 10 2015
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


#include "ngspice.h"
#include "probe.h"

#include <algorithm>
#include <functional>
#include <QStandardPaths>
#include "osdiselection.h"
#include "projectlibraries.h"
#include "ngoptimize.h"
#include "ngstatistics.h"
#include "ngsweep.h"
#include "components/iprobe.h"
#include "components/vprobe.h"
#include "components/equation.h"
#include "components/param_sweep.h"
#include "components/subcircuit.h"
#include "spicecomponents/sp_spiceinit.h"
#include "spicecomponents/xsp_cmlib.h"
#include "main.h"
#include "misc.h"
#include "qucs.h"
#include "settings.h"
#include "node.h"
#include "wire.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <QScopedPointer>

#include <iostream>

/*!
  \file ngspice.cpp
  \brief Implementation of the Ngspice class
*/

/*!
 * \brief Ngspice::Ngspice Class constructor
 * \param schematic Schematic that need to be simulated with Ngspice.
 * \param parent Parent object
 */
Ngspice::Ngspice(Schematic* schematic, QObject *parent) :
    AbstractSpiceKernel(schematic, parent),
    a_spinit_name()
{
    if (QFileInfo(QucsSettings.NgspiceExecutable).isRelative()) { // this check is related to MacOS
        a_simulator_cmd = QFileInfo(QucsSettings.BinDir + QucsSettings.NgspiceExecutable).absoluteFilePath();
    } else {
        a_simulator_cmd = QFileInfo(QucsSettings.NgspiceExecutable).absoluteFilePath();
    }
    if (!QFileInfo::exists(a_simulator_cmd)) {
        a_simulator_cmd = QucsSettings.NgspiceExecutable; //rely on $PATH
    }
    a_simulator_parameters = "";
    a_spinit_name = QDir::toNativeSeparators(a_workdir+"/.spiceinit");
}

namespace {
// The ngspice program itself - through PATH and links (Homebrew's
// bin/ngspice) - for what it can load.
QString programFile(const QString& command)
{
    QString program = command;
    if (!QFileInfo(program).isAbsolute()) {
        const QString found = QStandardPaths::findExecutable(program);
        if (!found.isEmpty()) program = found;
    }
    const QString real = QFileInfo(program).canonicalFilePath();
    return real.isEmpty() ? program : real;
}
} // namespace

/*!
 * \brief Ngspice::besideSchematic The files of \a patterns in the schematic's
 *        own folder (not its subfolders): a schematic's Verilog-A is found
 *        there when no project is open. None for an untitled one.
 */
QStringList Ngspice::besideSchematic(const QStringList& patterns) const
{
    QStringList files;
    const QString name = a_schematic != nullptr ? a_schematic->getDocName() : QString();
    if (name.isEmpty()) return files;
    const QDir folder = QFileInfo(name).absoluteDir();
    for (const QString& file : folder.entryList(patterns, QDir::Files, QDir::Name))
        files << folder.absoluteFilePath(file);
    return files;
}

namespace {
// Whether \a files has \a file, by where it is (a project's folder may be
// written another way).
bool containsFile(const QStringList& files, const QString& file)
{
    const QString real = QFileInfo(file).canonicalFilePath();
    return std::any_of(files.cbegin(), files.cend(), [&](const QString& f) {
        return f == file || (!real.isEmpty() && QFileInfo(f).canonicalFilePath() == real);
    });
}
} // namespace

/*!
 * \brief Ngspice::verilogAFiles The Verilog-A sources (.va) and libraries
 *        (.osdi) a simulation of the schematic may use: of the project -
 *        its folder and subfolders, as the Content panel lists them -,
 *        those beside the schematic (with no project open, the only ones),
 *        and those the libraries of the circuit's components bring (the
 *        sources Create Library embeds, and what they were compiled to).
 */
void Ngspice::verilogAFiles(QStringList* sources, QStringList* libraries) const
{
    if (QucsMain != nullptr && !QucsMain->ProjName.isEmpty()) {
        const QDir project(QucsSettings.QucsWorkDir);
        for (const QString& file : misc::projectFiles(project, {"*.va"}))
            *sources << project.absoluteFilePath(file);
        for (const QString& file : misc::projectFiles(project, {"*.osdi"}))
            *libraries << project.absoluteFilePath(file);
    }
    for (const QString& file : besideSchematic({"*.va"}))
        if (!containsFile(*sources, file)) *sources << file;
    for (const QString& file : besideSchematic({"*.osdi"}))
        if (!containsFile(*libraries, file)) *libraries << file;
    // A library's source the project has a link to (or a copy of: Windows)
    // is the project's: compiled beside that, not where the library is -
    // nor is a model there, compiled from it before, loaded.
    QSet<QString> linked;
    for (const QString& source : std::as_const(*sources)) {
        linked.insert(QFileInfo(source).canonicalFilePath());
        if (const auto entry = qucs_s::projectlibraries::entryOf(source); !entry.isEmpty())
            linked.insert(QFileInfo(entry.original).canonicalFilePath());
    }
    linked.remove(QString());
    for (const QString& file : collectVerilogAFiles(a_schematic)) {
        const QFileInfo info(file);
        if (!info.isFile()) continue;
        const QString source = info.absoluteDir().absoluteFilePath(info.completeBaseName() + QStringLiteral(".va"));
        if (linked.contains(QFileInfo(source).canonicalFilePath())) continue;
        if (file.endsWith(QLatin1String(".va"), Qt::CaseInsensitive) && !sources->contains(file)) *sources << file;
        else if (file.endsWith(QLatin1String(".osdi"), Qt::CaseInsensitive) && !libraries->contains(file)) *libraries << file;
    }
}

/*!
 * \brief Ngspice::osdiLoads The pre_osdi lines of the OSDI libraries
 *        (compiled Verilog-A) the netlist needs, of verilogAFiles() - for a
 *        source, what osdi::modelOf() says: the library compiled into the
 *        cache for it in place of the one beside it when that one is
 *        another platform's or its folder could not be written to -, those
 *        that define a module a .model card of \a netlist, or of a file it
 *        includes, names; one library for each module. One built for
 *        another platform is left out, and said.
 */
QString Ngspice::osdiLoads(const QString& netlist) const
{
    QStringList sources, files;
    verilogAFiles(&sources, &files);
    const QString simulator = programFile(a_simulator_cmd);
    const QString cache = misc::cacheDir();
    // What the circuit's library parts bring, each its own library's: a
    // module of theirs is loaded from it, not from a project file that
    // happens to define one of that name (osdi::needed()'s preferred).
    QSet<QString> brought;
    for (const QString& file : collectVerilogAFiles(a_schematic)) brought.insert(QFileInfo(file).canonicalFilePath());
    brought.remove(QString());
    const auto ofParts = [&brought](const QString& source) {
        if (brought.contains(QFileInfo(source).canonicalFilePath())) return true;
        const auto entry = qucs_s::projectlibraries::entryOf(source);   // (a project's copy of one: Windows)
        return !entry.isEmpty() && brought.contains(QFileInfo(entry.original).canonicalFilePath());
    };
    QStringList preferred;
    for (const QString& file : std::as_const(files))
        if (ofParts(file)) preferred << file;
    for (const QString& source : std::as_const(sources)) {
        const QString model = qucs_s::osdi::modelOf(source, cache, simulator);
        const QFileInfo va(source);
        const QString beside = va.absoluteDir().absoluteFilePath(va.completeBaseName() + QStringLiteral(".osdi"));
        if (!model.isEmpty() && ofParts(source)) preferred << model;
        if (model.isEmpty() || model == beside) continue;
        files.erase(std::remove_if(files.begin(), files.end(), [&](const QString& f) { return containsFile({beside}, f); }),
                    files.end());
        if (!files.contains(model)) files << model;
    }
    const QSet<QString> types = modelTypesOf(netlist);
    QStringList notes, loadable;
    // A module two sources define differently - two libraries' vx_res, a
    // project file and a part's library -: ngspice loads one definition and
    // every device of the module runs it, which no line said (one of them
    // never compiled, nothing was even left out). Said.
    QStringList sortedTypes(types.cbegin(), types.cend());
    sortedTypes.sort();
    for (const QString& type : std::as_const(sortedTypes)) {
        // Each content once (a project's copy of a library's source is
        // that source), a part's when any file of it is.
        struct Content {
            QByteArray bytes;
            QString shown;
            bool ofParts = false;
        };
        QList<Content> contents;
        for (const QString& source : std::as_const(sources)) {
            if (!qucs_s::osdi::sourceDefines(source, type)) continue;
            QFile f(source);
            const QByteArray bytes = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
            const bool part = ofParts(source);
            auto it = std::find_if(contents.begin(), contents.end(), [&](const Content& c) { return c.bytes == bytes; });
            if (it == contents.end()) contents.append({bytes, QDir::toNativeSeparators(source), part});
            else if (part && !it->ofParts) *it = {bytes, QDir::toNativeSeparators(source), true};
        }
        QStringList distinct, distinctOfParts;
        for (const Content& c : std::as_const(contents)) {
            distinct << c.shown;
            if (c.ofParts) distinctOfParts << c.shown;
        }
        if (distinct.size() < 2) continue;
        if (distinctOfParts.size() == 1) {
            QStringList others = distinct;
            others.removeAll(distinctOfParts.first());
            notes << QStringLiteral("%1 from %2, the library of the part that uses it - not from %3, which defines it differently")
                         .arg(type, distinctOfParts.first(), others.join(QStringLiteral(", ")));
        } else {
            notes << QStringLiteral("%1 is defined differently by %2: one of them is loaded, and every device of %1 runs it")
                         .arg(type, (distinctOfParts.isEmpty() ? distinct : distinctOfParts).join(QStringLiteral(", ")));
        }
    }
    // (Said whether a compiled model is there or not.)
    if (files.isEmpty()) {
        QString out;
        for (const QString& note : std::as_const(notes)) out += QStringLiteral("* OSDI: %1\n").arg(note);
        return out;
    }
    for (const QString& file : std::as_const(files)) {
        if (qucs_s::osdi::builtForAnotherPlatform(file, simulator))
            notes << QStringLiteral("%1 was built for another platform: not loaded").arg(QDir::toNativeSeparators(file));
        else
            loadable << file;
    }
    QString out;
    for (const QString& file : qucs_s::osdi::needed(loadable, types, &notes, preferred))
        out += QStringLiteral("pre_osdi '%1'\n").arg(file);
    for (const QString& note : notes)
        out += QStringLiteral("* OSDI: %1\n").arg(note);
    return out;
}

QList<qucs_s::osdi::Build> Ngspice::verilogABuilds()
{
    QStringList sources, libraries;
    verilogAFiles(&sources, &libraries);
    if (sources.isEmpty())
        return {};
    // The netlist the simulation will write, for the modules it uses.
    const QString output = a_output;   // what a broken netlist adds is the simulation's to say
    QString netlist;
    {
        QTextStream stream(&netlist);
        QStringList simulations, vars, outputs;
        createNetlist(stream, simulations, vars, outputs);
    }
    a_output = output;
    return qucs_s::osdi::builds(sources, libraries, modelTypesOf(netlist), programFile(a_simulator_cmd), misc::cacheDir());
}

/*!
 * \brief Ngspice::modelTypesOf The device types \a netlist uses (its .model
 *        cards' and those of the files it includes: osdi::usedModelTypes())
 *        - and, for a circuit of the open project, the modules of the
 *        library parts marked to be loaded in all its circuits, placed or
 *        not (projectlibraries::alwaysLoadedModules()). The Verilog-A
 *        compiled and loaded for it are those defining these.
 */
QSet<QString> Ngspice::modelTypesOf(const QString& netlist) const
{
    const QString name = a_schematic->getDocName();
    QSet<QString> types = qucs_s::osdi::usedModelTypes(netlist, QFileInfo(name).absolutePath());
    if (const QString project = projectOfCircuit(); !project.isEmpty()) {
        if (!a_alwaysLoaded) a_alwaysLoaded = qucs_s::projectlibraries::alwaysLoadedModules(project);
        types.unite(*a_alwaysLoaded);
    }
    return types;
}

/*!
 * \brief Ngspice::projectOfCircuit The open project's folder (its real
 *        path) when the schematic is one of its circuits - an untitled one
 *        is: it is saved there -, else empty.
 */
QString Ngspice::projectOfCircuit() const
{
    if (QucsMain == nullptr || QucsMain->ProjName.isEmpty()) return {};
    const QString name = a_schematic->getDocName();
    const QString project = QFileInfo(QucsSettings.QucsWorkDir.absolutePath()).canonicalFilePath();
    const QString file = name.isEmpty() ? QString() : QFileInfo(name).canonicalFilePath();
    if (project.isEmpty() || (!name.isEmpty() && !file.startsWith(project + QLatin1Char('/')))) return {};
    return project;
}

/*!
 * \brief Ngspice::createNetlist Output Ngspice-style netlist to text stream.
 *        Netlist contains sections necessary for Ngspice.
 * \param[out] stream QTextStream that associated with spice netlist file
 * \param[out] simulations The list of simulations used by schematic.
 * \param[out] vars The list of output variables and node names.
 * \param[out] outputs The list of spice output raw text files.
 */
void Ngspice::createNetlist(
        QTextStream& stream,
        QStringList& simulations,
        QStringList& vars,
        QStringList& outputs)
{
    Q_UNUSED(simulations);

    stream << "* Qucs " << PACKAGE_VERSION << "  " << a_schematic->getDocName() << "\n";

    // include math. functions for inter-simulator compat. - unless the
    // user leaves them out (Simulator Settings > Netlist)
    QString mathf_inc;
    bool found = QucsSettings.NgspiceMathFuncs && findMathFuncInc(mathf_inc);
    // Let to simulate schematic without mathfunc.inc file
    if (found && QucsSettings.DefaultSimulator != spicecompat::simSpiceOpus)
        stream<<QStringLiteral(".INCLUDE \"%1\"\n").arg(mathf_inc);

    const QString libraries = collectSpiceLibs(a_schematic); // collect libraries on the top of netlist
    stream<<libraries;
    // The subcircuits and components go through a string first: the OSDI
    // libraries loaded are the ones its .model cards (and the libraries'
    // cards) need.
    QString body;
    QTextStream bodyStream(&body);
    const bool prepared = prepareSpiceNetlist(bodyStream);
    if (prepared)
        startNetlist(bodyStream); // output .PARAM and components
    bodyStream.flush();
    // The .model cards of the project's library parts marked to be in all
    // its circuits, placed or not (Document Settings > Library), at its top
    // level - not those a placed part's model wrote already.
    if (const QString project = prepared ? projectOfCircuit() : QString(); !project.isEmpty()) {
        if (!a_alwaysCards) a_alwaysCards = qucs_s::projectlibraries::alwaysWrittenModelCards(project);
        for (const QString& cards : std::as_const(*a_alwaysCards))
            if (!body.contains(cards)) body += cards;
    }
    stream<<body;
    if (!prepared) return; // Unable to perform spice simulation
    const QString osdi = osdiLoads(libraries + body);

    if (a_DC_OP_only) {
        stream<<".control\n"  // Execute only DC OP analysis
              <<osdi
              <<"set filetype=ascii\n" // Ignore all other simulations
              <<"op\n"
              <<"print all > spice4qucs.cir.dc_op\n";
        // The operating point of every device - gm, vth, id, cd, ... - for
        // the Operating Point tab and the components' tooltips (oppoint.h).
        if (QucsSettings.DefaultSimulator == spicecompat::simNgspice)
            stream<<"show all > spice4qucs.cir.dc_op_dev\n";
        stream<<"destroy all\n"
              <<"quit\n"
              <<".endc\n"
              <<".end\n";
        outputs.clear();
        outputs.append("spice4qucs.cir.dc_op");
        return;
    }

    // set variable names for named nodes and wires
    QSet<QString> validNets = getValidNets();
    vars = QStringList(validNets.begin(), validNets.end());
    vars.sort();

    // The currents and powers probed (probe.h): every device's current
    // kept, the powers asked for saved beside all the rest.
    const QStringList probed = a_schematic->getProbeSaves();
    if (std::any_of(probed.cbegin(), probed.cend(), qucs_s::probe::isCurrentVector)) stream << ".options savecurrents\n";
    QStringList powers;
    for (const QString& v : probed)
        if (qucs_s::probe::isPowerVector(v)) powers << v;
    if (!powers.isEmpty()) stream << ".save all " << powers.join(QLatin1Char(' ')) << "\n";

    stream << "\n.control\n\n";          //execute simulations

    stream<<osdi;

    // NgOpt: ngspice's optimize, ahead of the simulations - it leaves the
    // circuit at the optimum, so they show it. A .param it changed stays
    // across the reset after each simulation; an alter or altermod does
    // not, so those knobs are kept in variables and set again.
    a_optimizations.clear();
    QString reapply;
    for (Component* pc : a_schematic->a_DocComps) {
        if (pc->Model != ".NGOPT" || pc->isActive != COMP_IS_ACTIVE) continue;
        const qucs_s::ngopt::Command command = qucs_s::ngopt::Command::read(pc);
        QString line, why;
        if (!qucs_s::ngopt::commandLine(command, a_schematic, &line, &why)) {
            stream << QStringLiteral("echo \"Error: %1: %2\"\n").arg(pc->Name, why.replace('"', '\''));
            continue;
        }
        a_optimizations.append(pc->Name);
        const int index = a_optimizations.size();
        stream << line << "\n" << qucs_s::ngopt::carryLines(command, index) << "\n";
        reapply += qucs_s::ngopt::reapplyLines(command, index);
    }

    // determine which simulations are in use
    unsigned int dcSims = 0;
    unsigned int freqSims = 0;
    unsigned int timeSims = 0;
    unsigned int fourSims = 0;
    unsigned int pzSims = 0;
    // Each simulation's kind (0 DC, 1 frequency, 2 time, 3 Fourier, 4
    // pole-zero; -1 a custom one's, of any), by its name: the vectors of a
    // kind two of them write are prefixed with their simulation's name.
    QHash<QString, int> kindOf;

    outputs.clear();
    for (Component* pc : a_schematic->a_DocComps) {
        if ( !pc->isSimulation ) continue;
        if ( pc->isActive != COMP_IS_ACTIVE ) continue;

        QString sim_typ = pc->Model;
        QString sim_name = pc->Name.toLower();
        QString spiceNetlist;

        bool hasParSWP = false;
        bool hasDblSWP = false;
        QString cnt_var;

        // track whether we want to netlist equations
        bool netlist_equations = true;
        // track whether we want the dependent vars (e.g. equation variables)
        // to be included in the save node statement
        bool write_dep_vars = true;
        // whether saves set for this analysis are cleared after it
        bool clearSaves = false;
        // whether equations read this analysis' plot (let lines after it)
        bool equationsRead = false;

        // Duplicate .PARAM in .control section. They may be used in euqations
        for (Component* pc1 : a_schematic->a_DocComps) {
            if ( pc1->isActive != COMP_IS_ACTIVE ) continue;
            if ( pc1->Model == "Eqn" ) {
                spiceNetlist.append((reinterpret_cast<Equation *>(pc1))->getNgspiceScript());
            }
        }

        QString nods;
        for (const QString& nod : vars) {
            if ( nod.endsWith("#branch") )
                nods.append(QStringLiteral("i(%1) ").arg(nod.section('#', 0, 0)));
            else
                nods.append(QStringLiteral("v(%1) ").arg(nod));
        }
        for (const QString& v : probed) nods.append(v + QLatin1Char(' '));   // (the probes')
        // (The nodes' voltages and the probes' currents: what a save before
        // the analysis can name - an equation's variable is made after it.)
        const QString nodeVectors = nods;

        for (Component* pc1 : a_schematic->a_DocComps) {
            if ( !pc1->isSimulation ) continue;
            if ( pc1->isActive != COMP_IS_ACTIVE ) continue;
            QString sim_typ = pc1->Model;
            if ( sim_typ == ".SW" ) {
                QString SwpSim = pc1->Props.at(0)->Value.toLower();
                if ( SwpSim == sim_name ) {
                    cnt_var = (reinterpret_cast<Param_Sweep *>(pc1))->getCounterVar();
                    if ( !sim_name.startsWith("dc") ) {
                        spiceNetlist.append(getParentSWPscript(pc1, sim_name, true, hasDblSWP));
                        spiceNetlist.append(pc1->getNgspiceBeforeSim(sim_name));
                        hasParSWP = true;
                    }
                }
            }
        }

        if ( sim_typ == ".AC" ) {
            freqSims++;
            kindOf.insert(sim_name, 1);
            spiceNetlist.append(pc->getSpiceNetlist());
        } else if ( sim_typ == ".TR" ) {
            timeSims++;
            kindOf.insert(sim_name, 2);
            spiceNetlist.append(pc->getSpiceNetlist());
            for (Component* pc1 : a_schematic->a_DocComps) {
                if ( !pc1->isSimulation ) continue;
                if ( pc1->isActive != COMP_IS_ACTIVE ) continue;
                if ( pc1->Model == ".FOURIER" ) {
                    if ( pc1->Props.at(0)->Value.toLower() == sim_name ) {
                        fourSims++;
                        kindOf.insert(pc1->Name.toLower(), 3);
                        // Add it twice for THD
                        outputs.append("spice4qucs." + pc1->Name.toLower() + ".four");
                        outputs.append("spice4qucs." + pc1->Name.toLower() + ".four");
                        spiceNetlist.append(pc1->getSpiceNetlist());
                    }
                }
            }
        } else if ( sim_typ == ".CUSTOMSIM" ) {
            kindOf.insert(sim_name, -1);
            spiceNetlist.append(pc->getSpiceNetlist());
            nods = pc->Props.at(1)->Value;
            nods.replace(';', ' ');
            outputs.append(pc->Props.at(2)->Value.split(';', Qt::SkipEmptyParts));

            QRegularExpression ac_rx("^\\s*ac\\s.*", QRegularExpression::CaseInsensitiveOption);
            QRegularExpression sp_rx("^\\s*sp\\s.*", QRegularExpression::CaseInsensitiveOption);
            QRegularExpression noise_rx("^\\s*noise\\s.*", QRegularExpression::CaseInsensitiveOption);
            QRegularExpression disto_rx("^\\s*disto\\s.*", QRegularExpression::CaseInsensitiveOption);
            QRegularExpression fft_rx("^\\s*fft\\s.*", QRegularExpression::CaseInsensitiveOption);
            QRegularExpression four_rx("^\\s*(four|fourier)\\s.*", QRegularExpression::CaseInsensitiveOption);
            QRegularExpression dc_rx("^\\s*dc\\s.*", QRegularExpression::CaseInsensitiveOption);
            QRegularExpression op_rx("^\\s*op\\s*", QRegularExpression::CaseInsensitiveOption);
            QRegularExpression tran_rx("^\\s*tran\\s.*", QRegularExpression::CaseInsensitiveOption);
            QRegularExpression sens_ac_rx("^\\s*sens\\s.*ac\\s.*", QRegularExpression::CaseInsensitiveOption);
            QRegularExpression sens_dc_rx("^\\s*sens\\s.*", QRegularExpression::CaseInsensitiveOption);
            QRegularExpression pz_rx("^\\s*pz\\s.*", QRegularExpression::CaseInsensitiveOption);

            QStringList lines = pc->getSpiceNetlist().split('\n');
            for ( const QString& line : lines ) {
                if      ( ac_rx.match(line).hasMatch() )      freqSims++ ;
                else if ( sp_rx.match(line).hasMatch() )      freqSims++ ;
                else if ( noise_rx.match(line).hasMatch() )   freqSims++ ;
                else if ( disto_rx.match(line).hasMatch() )   freqSims++ ;
                else if ( fft_rx.match(line).hasMatch() )     freqSims++ ;
                else if ( four_rx.match(line).hasMatch() )    fourSims++ ;
                else if ( dc_rx.match(line).hasMatch() )      dcSims++ ;
                else if ( op_rx.match(line).hasMatch() )      dcSims++ ;
                else if ( tran_rx.match(line).hasMatch() )    timeSims++ ;
                else if ( sens_ac_rx.match(line).hasMatch() ) freqSims++ ;
                else if ( sens_dc_rx.match(line).hasMatch() ) dcSims++ ;
                else if ( pz_rx.match(line).hasMatch() )      pzSims++ ;
            }
        } else if ( sim_typ == ".DISTO" ) {
            freqSims++;
            kindOf.insert(sim_name, 1);
            spiceNetlist.append(pc->getSpiceNetlist());
            nods.clear();
        } else if ( sim_typ == ".NOISE" ) {
            freqSims++;
            kindOf.insert(sim_name, 1);
            spiceNetlist.append(pc->getSpiceNetlist());
            outputs.append("spice4qucs." + sim_name + ".cir.noise");
            if ( hasParSWP ) {  // Set necessary plot number to output Noise spectrum
                // each step of parameter sweep creates new couple of noise plots
                spiceNetlist.append(QStringLiteral("let noise_%1 = 2*%1+1\n").arg(cnt_var));
                spiceNetlist.append(QStringLiteral("setplot noise$&noise_%1\n").arg(cnt_var));
            } else {  // Set Noise1 plot to output noise spectrum
                spiceNetlist.append("setplot noise1\n");
            }
            // NOTE: if we set 'setplot noiseX', and we use 'all'
            // it will be the equivalent of noiseX.all
            nods = "all";
            // since 'all' is included, we don't need to write the dependent variables explicitly
            write_dep_vars = false;
        } else if ( sim_typ == ".PZ" ) {
            pzSims++;
            kindOf.insert(sim_name, 4);
            netlist_equations = false;
            spiceNetlist.append(pc->getSpiceNetlist());
            QString out = "spice4qucs." + sim_name + ".cir.pz";
            // Add it twice for poles and zeros
            outputs.append(out);
            outputs.append(out);
        } else if ( sim_typ == ".SENS" ) {
            dcSims++;
            kindOf.insert(sim_name, 0);
            netlist_equations = false;
            spiceNetlist.append(pc->getSpiceNetlist());
            outputs.append("spice4qucs." + sim_name + ".ngspice.sens.dc.prn");
        } else if ( sim_typ == ".SENS_AC" ) {
            freqSims++;
            kindOf.insert(sim_name, 1);
            netlist_equations = false;
            spiceNetlist.append(pc->getSpiceNetlist());
            outputs.append("spice4qucs." + sim_name + ".sens.prn");
        } else if ( sim_typ == ".SP" ) {
            freqSims++;
            kindOf.insert(sim_name, 1);
            spiceNetlist.append(pc->getSpiceNetlist());
            nods.clear();
            nods.append(' ' + pc->getExtraVariables().join(' '));
        } else if ( sim_typ == ".FFT" ) {
            freqSims++;
            kindOf.insert(sim_name, 1);
            spiceNetlist.append(pc->getSpiceNetlist());
            spiceNetlist.append(QStringLiteral("linearize %1\n").arg(nods));
            spiceNetlist.append(QStringLiteral("fft %1\n").arg(nods));
        } else if ( sim_typ == ".DC" ) {
            dcSims++;
            kindOf.insert(sim_name, 0);
            spiceNetlist.append(pc->getSpiceNetlist());
        } else if ( sim_typ == ".SW" ) {
            QString SwpSim = pc->Props.at(0)->Value.toLower();
            if ( SwpSim.startsWith("dc") ) {
                dcSims++;
                kindOf.insert(sim_name, 0);
                spiceNetlist.append(pc->getSpiceNetlist());
            } else
                continue;
        } else
            continue;

        if (netlist_equations) {
            QStringList dep_vars;
            for (Component* pc1 : a_schematic->a_DocComps) {
                if ( pc1->isActive != COMP_IS_ACTIVE ) continue;
                if ( pc1->Model == "Eqn" || pc1->Model == "NutmegEq" )
                    spiceNetlist.append(pc1->getEquations(sim_name, dep_vars));
            }
            equationsRead = !dep_vars.isEmpty();
            if (write_dep_vars) {
                nods.append(' ' + dep_vars.join(' '));
            }
        }

        if ( sim_typ == ".DC" ) {
            QString out = "spice4qucs." + sim_name + ".ngspice.dc.print";
            // Printed in commands ngspice takes whole, into one file. None
            // with no node named: a print of nothing failed ("print: too
            // few args."), the devices' values below being the results.
            const QStringList chunks = spicecompat::vectorChunks(nods);
            for (qsizetype k = 0; k < chunks.size(); ++k)
                spiceNetlist.append(QStringLiteral("print %1 %2 %3\n").arg(chunks.at(k), k == 0 ? QStringLiteral(">") : QStringLiteral(">>"), out));
            if (!chunks.isEmpty()) outputs.append(out);
            // The operating point of every device too - id, gm, vgs, ... -
            // for the dataset, beside the node values (convertToQucsData()).
            if (QucsSettings.DefaultSimulator == spicecompat::simNgspice)
                spiceNetlist.append(QStringLiteral("show all > spice4qucs.%1.ngspice.op_dev\n").arg(sim_name));
        } 
        else if (sim_typ == ".NOISE") {
            nods = nods.simplified();
            if ( !nods.isEmpty() ) {
                QString basenam = "spice4qucs";
                QString filename;
                if ( hasParSWP && hasDblSWP )
                    filename = QStringLiteral("%1.%2._swp_swp.raw").arg(basenam).arg(sim_name);
                else if ( hasParSWP )
                    filename = QStringLiteral("%1.%2._swp.raw").arg(basenam).arg(sim_name);
                else
                    filename = QStringLiteral("%1.%2.raw").arg(basenam).arg(sim_name);
                filename.replace(' ', '_'); // Ngspice cannot understand spaces in filename
                spiceNetlist.append(QStringLiteral("write %1 %2\n").arg(filename).arg(nods));
                outputs.append(filename);
            }
        }
        else if ( (sim_typ != ".PZ") && (sim_typ != ".SENS") && (sim_typ != ".SENS_AC") ) {
            nods = nods.simplified();
            if ( !nods.isEmpty() ) {
                QString basenam = "spice4qucs";
                QString filename;
                if ( hasParSWP && hasDblSWP )
                    filename = QStringLiteral("%1.%2._swp_swp.plot").arg(basenam).arg(sim_name);
                else if ( hasParSWP )
                    filename = QStringLiteral("%1.%2._swp.plot").arg(basenam).arg(sim_name);
                else
                    filename = QStringLiteral("%1.%2.plot").arg(basenam).arg(sim_name);
                filename.replace(' ', '_'); // Ngspice cannot understand spaces in filename
                if (spicecompat::tooManyVectors(nods)) {
                    // More than a command takes: its plot written whole
                    // (with the equations' variables). The nodes saved
                    // before the analysis, so it holds them alone, and the
                    // saves cleared for the next one - unless equations
                    // read the plot: what a save leaves out (a
                    // subcircuit's inner node) they would not find.
                    if (!equationsRead) {
                        spiceNetlist.prepend(spicecompat::saveLines(nodeVectors));
                        clearSaves = true;
                    }
                    spiceNetlist.append(QStringLiteral("write %1\n").arg(filename));
                } else {
                    spiceNetlist.append(QStringLiteral("write %1 %2\n").arg(filename).arg(nods));
                }
                outputs.append(filename);
            }
        }

        for (Component* pc1 : a_schematic->a_DocComps) {
            if ( !pc1->isSimulation ) continue;
            if ( pc1->isActive != COMP_IS_ACTIVE ) continue;
            QString sim_typ = pc1->Model;
            if ( sim_typ == ".SW" ) {
                QString SwpSim = pc1->Props.at(0)->Value.toLower();
                if ( SwpSim == sim_name ) {
                    if ( !sim_name.startsWith("dc") ) {
                        spiceNetlist.append(pc1->getNgspiceAfterSim(sim_name));
                        spiceNetlist.append(getParentSWPscript(pc1, sim_name, false, hasDblSWP));
                    }
                }
            }
        }

        spiceNetlist.append("destroy all\n");
        if (clearSaves) spiceNetlist.append("delete all\n");
        spiceNetlist.append("reset\n");
        spiceNetlist.append(reapply);
        spiceNetlist.append("\n");
        stream << spiceNetlist;
    }

    // The voltages and currents the simulations save.
    QString saved;
    for (const QString& nod : vars)
        saved += nod.endsWith("#branch") ? QStringLiteral("i(%1) ").arg(nod.section('#', 0, 0))
                                         : QStringLiteral("v(%1) ").arg(nod);

    // NgSweep: ngspice's sweep, after the simulations, writing its values
    // and every point's plot into files of its own.
    a_sweeps.clear();
    for (Component* pc : a_schematic->a_DocComps) {
        if (!qucs_s::ngsweep::isSweep(pc) || pc->isActive != COMP_IS_ACTIVE) continue;
        QString why;
        const QString block = qucs_s::ngsweep::controlBlock(pc, a_schematic, saved, &outputs, &why);
        if (block.isEmpty()) {
            stream << QStringLiteral("echo \"Error: %1: %2\"\n").arg(pc->Name, why.replace('"', '\''));
            continue;
        }
        a_sweeps.append(pc->Name);
        stream << block << "destroy all\n" << "reset\n" << reapply << "\n";
    }

    // NgMonteCarlo and NgCorners: ngspice's montecarlo and corners, after
    // the simulations, each writing its results into files of its own.
    a_statistics.clear();
    for (Component* pc : a_schematic->a_DocComps) {
        if (!qucs_s::ngstats::isStatistics(pc) || pc->isActive != COMP_IS_ACTIVE) continue;
        QString why;
        const QString block = qucs_s::ngstats::controlBlock(pc, a_schematic, saved, &outputs, &why);
        if (block.isEmpty()) {
            stream << QStringLiteral("echo \"Error: %1: %2\"\n").arg(pc->Name, why.replace('"', '\''));
            continue;
        }
        a_statistics.append(pc->Name);
        stream << block << "destroy all\n" << "reset\n" << reapply << "\n";
    }

    stream << "exit\n"
           << ".endc\n";
    stream << ".END\n";

    // Only the kinds two simulations write: an .AC beside an .FFT (both
    // write "ac") prefixed every vector, the transient's too - tran.v(out)
    // became tr1.tran.v(out), and every diagram of it went blank (bug hunt
    // of 2026-10-08, B1). A custom simulation's, when any kind is shared.
    const unsigned int counts[] = {dcSims, freqSims, timeSims, fourSims, pzSims};
    const bool anyShared = std::any_of(std::begin(counts), std::end(counts), [](unsigned int n) { return n > 1; });
    a_prefixedSims.clear();
    for (auto it = kindOf.cbegin(); it != kindOf.cend(); ++it)
        if (it.value() < 0 ? anyShared : counts[it.value()] > 1) a_prefixedSims.insert(it.key());

    qDebug() << '\n'
             << "Simulations:\n"
             << "DC:        " << dcSims << '\n'
             << "Frequency: " << freqSims << '\n'
             << "Time:      " << timeSims << '\n'
             << "Fourier:   " << fourSims << '\n'
             << "Pole-Zero: " << pzSims << '\n'
             << '\n';
}

/*!
 * \brief Ngspice::getParentSWPscript
 * \param pc_swp
 * \param sim
 * \param before
 * \return
 */
QString Ngspice::getParentSWPscript(Component *pc_swp, QString sim, bool before, bool &hasDblSwp)
{
    hasDblSwp = false;
    QString swp = pc_swp->Name.toLower();
    for (Component* pc : a_schematic->a_DocComps) {
        if ( !pc->isSimulation ) continue;
        if ( pc->isActive != COMP_IS_ACTIVE ) continue;
        if ( pc->Model == ".SW" ) {
            if ( pc->Props.at(0)->Value.toLower() == swp ) {
                if (before) {
                    hasDblSwp = true;
                    return pc->getNgspiceBeforeSim(sim, 1);
                } else {
                    hasDblSwp = true;
                    return pc->getNgspiceAfterSim(sim, 1);
                }
            }
        }
    }
    return QString();
}

/*!
 * \brief Ngspice::slotSimulate Create netlist and execute Ngspice simulator. Netlist
 *        is saved at $HOME/.qucs/spice4qucs/spice4qucs.cir
 */
void Ngspice::slotSimulate()
{
    a_output.clear();
    a_refusal.clear();

    QString mathf_inc; // drain
    if (QucsSettings.NgspiceMathFuncs && !findMathFuncInc(mathf_inc)) {
        a_output.append("[Warning!] " + mathf_inc + " file not found!\n");
    }

    bool checker_error = false;
    const qsizetype checked = a_output.size();   // (the checks' text after it)
    QStringList incompat;
    if (!checkSchematic(incompat)) {
        QString s = incompat.join("; ");
        a_output.append("There were SPICE-incompatible components. Simulator cannot proceed.\n");
        a_output.append("Incompatible components are: " + s + "\n");
        checker_error = true;
    }

    if (!checkGround()) {
        a_output.append("No Ground found. Please add at least one ground!\n"
                      "Press Insert->Ground in the main menu and connect ground to one "
                      "of the schematic nodes.\n"
                      "(If node 0 comes from a net named 0 or from a component, turn off "
                      "\"A schematic must have a ground symbol\" in Simulation > Simulators Settings.)\n");
        checker_error = true;
    }

    if (!checkSimulations()) {
        a_output.append("No simulation found. Please add at least one simulation!\n"
                      "Navigate to the \"simulations\" group in the components panel (left)"
                      " and drag simulation to the schematic sheet. Then define its parameters.\n");
        checker_error = true;
    }

    if (!checkDCSimulation()) {
        a_output.append("Only DC simulation found in the schematic. It has no effect!"
                      " Add TRAN, AC, or Sweep simulation to proceed.\n");
        checker_error = true;
    }

    incompat.clear();
    if (!checkNodeNames(incompat)) {
        QString s = incompat.join("; ");
        a_output.append("There were Nutmeg-incompatible node names. Simulator cannot proceed.\n");
        a_output.append("Incompatible node names are: " + s + "\n");
        a_output.append("(Nutmeg reads " + spicecompat::nutmegKeywords().join(", ")
                        + " as operators, in any case: rename the net.)\n");
        checker_error = true;
    }

    if (checker_error) {
        // (Said as the run's error: the process it never started has none.)
        a_refusal = a_output.mid(checked).trimmed();
        if (a_console != nullptr)
            a_console->insertPlainText(a_output);
        //emit finished();
        emit errors(QProcess::FailedToStart);
        return;
    }

    QString netfile = "spice4qucs.cir";
    QString tmp_path = QDir::toNativeSeparators(a_workdir+QDir::separator()+netfile);
    SaveNetlist(tmp_path, false);

    removeAllSimulatorOutputs();

    /*XSPICE_CMbuilder *CMbuilder = new XSPICE_CMbuilder(a_schematic);
    CMbuilder->cleanSpiceinit();
    CMbuilder->createSpiceinit(collectSpiceinit(a_schematic));
    if (CMbuilder->needCompile()) {
        CMbuilder->cleanCModelTree();
        CMbuilder->createCModelTree(a_output);
        CMbuilder->compileCMlib(a_output);
    }
    delete CMbuilder;*/
    cleanSpiceinit();
    createSpiceinit(/*initial_spiceinit=*/collectSpiceinit(a_schematic));

    //startNgSpice(tmp_path);
    a_simProcess->setWorkingDirectory(a_workdir);
    qDebug()<<a_workdir;
    QString cmd = QStringLiteral("\"%1\" %2 %3").arg(a_simulator_cmd,a_simulator_parameters,netfile);
    QStringList cmd_args = misc::parseCmdArgs(cmd);
    QString ngsp_cmd = cmd_args.at(0);
    cmd_args.removeAt(0);
    a_simProcess->start(ngsp_cmd,cmd_args);
    if (QucsMain != nullptr)
    emit started();
}

/*!
 * \brief Ngspice::checkNodeNames Check schematic node names on reserved Nutmeg keywords.
 * \param incompat
 * \return
 */
bool Ngspice::checkNodeNames(QStringList &incompat)
{
    bool result = true;
    for(Node *pn : a_schematic->a_DocNodes) {
      if(pn->hasLabel()) {
          if (!spicecompat::check_nodename(pn->label()->Name)) {
              incompat.append(pn->label()->Name);
              result = false;
          }
      }
    }
    for(Wire *pw : a_schematic->a_DocWires) {
      if(pw->hasLabel()) {
          if (!spicecompat::check_nodename(pw->label()->Name)) {
              incompat.append(pw->label()->Name);
              result = false;
          }
      }
    }
    return result;
}

/*!
 * \brief Ngspice::collectSpiceinit Collects user-specified .spiceinit data.
 * \param incompat
 * \return
 */
QString Ngspice::collectSpiceinit(Schematic* sch)
{
    QSet<QString> visited = hierarchyStart(sch);
    QStringList collected_spiceinit;
    // Each subcircuit once (firstVisit()): one that includes itself
    // recursed until the stack ran out.
    std::function<void(Schematic*)> collect = [&](Schematic* doc) {
        for (Component *pc : doc->a_DocComps) {
            if (pc->Model == "SPICEINIT") {
                collected_spiceinit += ((SpiceSpiceinit*)pc)->getSpiceinit();
            } else if (pc->Model == "Sub") {
                const QString file = ((Subcircuit *)pc)->getSubcircuitFile();
                if (!firstVisit(file, visited)) continue;
                Schematic sub(nullptr, file);
                if (!sub.loadDocument()) continue;   // load document if possible
                collect(&sub);
            }
        }
    };
    if (sch != nullptr) collect(sch);
    return collected_spiceinit.join("");
}

/*!
 * \brief Ngspice::findMathFuncInc Find the ngspice_mathfunc.inc file. This file
 *        contains math.functions definitions for Ngspice. It's need to let to simulate
 *        circuit if it is not found.
 * \param mathf_inc[out] The filename of include file
 * \return True if found. False otherwise
 */
bool Ngspice::findMathFuncInc(QString &mathf_inc)
{
    QDir qucs_root(QucsSettings.BinDir);
    qucs_root.cdUp();
    mathf_inc = QStringLiteral("%1/share/" QUCS_NAME "/xspice_cmlib/include/ngspice_mathfunc.inc")
            .arg(qucs_root.absolutePath());
    return QFile::exists(mathf_inc);
}

/*!
 * \brief Ngspice::slotProcessOutput Process Ngspice output and report completion
 *        percentage.
 */
void Ngspice::slotProcessOutput()
{
    QString s = a_simProcess->readAllStandardOutput();
    QRegularExpression percentage_pattern("^%\\d\\d*\\.\\d\\d.*$");
    if (percentage_pattern.match(s).hasMatch()) {
        int percent = round(s.mid(1,5).toFloat());
        emit progress(percent);
    }
    a_output += s;
    if (a_console != nullptr) {
        a_console->insertPlainText(s);
        a_console->moveCursor(QTextCursor::End);
    }
}

/*!
 * \brief Ngspice::SaveNetlist Create netlist and save it to file without execution
 *        of simulator.
 * \param[in] filename Absolute path to netlist
 * \param[in] netlist2Console Whether netlist to console instead to file
 */
void Ngspice::SaveNetlist(QString filename, bool netlist2Console)
{
    a_sims.clear();
    a_vars.clear();

    QScopedPointer<QString> netlistString;
    QScopedPointer<QTextStream> netlistStream;
    QScopedPointer<QFile> netlistFile;

    if (netlist2Console)
    {
        netlistString.reset(new QString);
        netlistStream.reset(new QTextStream(netlistString.get()));
    }
    else
    {
        netlistFile.reset(new QFile(filename));
        if (netlistFile->open(QFile::WriteOnly))
        {
            netlistStream.reset(new QTextStream(netlistFile.get()));
        }
        else
        {
            QString msg = QStringLiteral("Tried to save netlist \nin %1\n(could not open for writing!)").arg(filename);
            QString final_msg = QStringLiteral("%1\n This could be an error in the QSettings settings file\n(usually in ~/.config/qucs/qucs_s.conf)\nThe value for S4Q_workdir (default:/spice4qucs) needs to be writeable!\nFor a Simulation Simulation will raise error! (most likely S4Q_workdir does not exists)").arg(msg);
            QMessageBox::critical(nullptr,tr("Problem with SaveNetlist"), final_msg, QMessageBox::Ok);
            return;
        }
    }

    createNetlist(*netlistStream, a_sims, a_vars, a_output_files);

    if (netlist2Console)
    {
        std::cout << netlistString->toUtf8().constData() << std::endl;
    }
}

void Ngspice::setSimulatorCmd(QString cmd)
{
    if (cmd.contains(QRegularExpression("spiceopus(....|)$"))) {
        // spiceopus needs English locale to produce correct decimal point (dot symbol)
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove("LANG");
        env.insert("LANG","en_US");
        a_simProcess->setProcessEnvironment(env);
        a_simulator_parameters = a_simulator_parameters + "-c";
    } else { // restore system environment
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        a_simProcess->setProcessEnvironment(env);
    }

    a_simulator_cmd = cmd;
}

void Ngspice::setSimulatorParameters(QString parameters)
{
    a_simulator_parameters = parameters;
}

void Ngspice::cleanSpiceinit()
{
    QFileInfo inf(a_spinit_name);
    if (inf.exists()) QFile::remove(a_spinit_name);
}

void Ngspice::createSpiceinit(const QString &initial_spiceinit)
{
  auto compat_mode = _settings::Get().item<int>("NgspiceCompatMode");
  QString compat_str;
  switch(compat_mode) {
  case spicecompat::NgspLTspice:
    compat_str = "set ngbehavior=ltpsa\n";
    break;
  case spicecompat::NgspHSPICE:
    compat_str = "set ngbehavior=hsa\n";
    break;
  case spicecompat::NgspS3:
    compat_str = "set ngbehavior=s3\n";
    break;
  default: break;
  }
  if (initial_spiceinit.isEmpty() &&
      compat_str.isEmpty()) {
    return;
  }
  QFile spinit(a_spinit_name);
  if (spinit.open(QIODevice::WriteOnly)) {
    QTextStream stream(&spinit);
    stream << compat_str << '\n';
    stream << initial_spiceinit << '\n';
    spinit.close();
  }
}
