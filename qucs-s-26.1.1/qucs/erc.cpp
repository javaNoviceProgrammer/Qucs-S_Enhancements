/*
 * erc.cpp - electrical rule check of a schematic, before a simulation
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "erc.h"

#include "schematic.h"
#include "node.h"
#include "wire.h"
#include "wirelabel.h"
#include "components/component.h"
#include "main.h"
#include "extsimkernels/spicecompat.h"
#include "optimization.h"
#include "ngoptimize.h"
#include "osdiselection.h"
#include "misc.h"
#include "qucs.h"
#include "components/vacomponent.h"

#include <QCoreApplication>
#include <QDir>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <algorithm>

namespace qucs_s::erc {

namespace {

QString tr(const char* s) { return QCoreApplication::translate("ERC", s); }

bool isGround(const Component* c) { return c->Model == QLatin1String("GND"); }
bool isPort(const Component* c) { return c->Model == QLatin1String("Port"); }
// Simulation blocks: .AC, .TR, .DC, .SP, .HB, ... (Model starts with a dot).
bool isSimulation(const Component* c) { return c->Model.startsWith(QLatin1Char('.')); }
bool inCircuit(const Component* c) { return c->isActive == COMP_IS_ACTIVE; }
bool spiceSimulator(int simulator) { return (simulator & spicecompat::simSpice) != 0; }
bool forSimulator(const Component* c, int simulator) { return (c->Simulator & simulator) == simulator; }

// Whether a node has a name of its own (a label on it or on one of its
// wires): a stub that ends there is a named net, not a loose end.
bool named(const Node* n)
{
    if (n->hasLabel()) return true;
    for (const Wire* w : n->wires())
        if (w->hasLabel()) return true;
    return false;
}

} // namespace

QList<Issue> check(Schematic* doc)
{
    QList<Issue> errors, warnings;
    if (doc == nullptr) return errors;

    bool ground = false, port = false, simulation = false;
    // A circuit with a digital simulation block goes to the digital
    // (VHDL/Verilog) flow whatever the simulator setting says.
    const int simulator = doc->isDigitalCircuit() ? int(spicecompat::simNotSpecified) : QucsSettings.DefaultSimulator;
    QHash<QString, const Component*> byName;
    // The Verilog-A libraries and sources of the open project, read when a
    // Verilog-A component asks: ngspice gets its modules from them.
    QStringList vaLibraries, vaSources;
    bool vaListed = false;
    const auto moduleInProject = [&](const QString& module) {
        if (QucsMain == nullptr || QucsMain->ProjName.isEmpty()) return true;   // nowhere to look
        if (!vaListed) {
            const QDir project(QucsSettings.QucsWorkDir);
            for (const QString& file : misc::projectFiles(project, {"*.osdi"}))
                vaLibraries << project.absoluteFilePath(file);
            for (const QString& file : misc::projectFiles(project, {"*.va"}))
                vaSources << project.absoluteFilePath(file);
            vaListed = true;
        }
        return std::any_of(vaLibraries.cbegin(), vaLibraries.cend(),
                           [&](const QString& file) { return osdi::defines(file, module); })
            || std::any_of(vaSources.cbegin(), vaSources.cend(),
                           [&](const QString& file) { return osdi::sourceDefines(file, module); });
    };
    for (Component* c : doc->a_DocComps) {
        if (!inCircuit(c)) continue;
        if (isGround(c)) ground = true;
        if (isPort(c)) port = true;
        if (isSimulation(c)) simulation = true;

        // Two components of one name: the netlist would merge them.
        if (!c->Name.isEmpty() && !isGround(c)) {
            if (const Component* first = byName.value(c->Name)) {
                errors << Issue{Severity::Error,
                                tr("%1: the name is used twice (also at %2, %3)").arg(c->Name).arg(first->cx).arg(first->cy),
                                QPoint(c->cx, c->cy), c->Name};
            } else {
                byName.insert(c->Name, c);
            }
        }

        // What the simulator in use cannot take: the netlist would leave
        // the component out, or carry text the simulator rejects.
        if (simulator != spicecompat::simNotSpecified) {
            const QString simName = spicecompat::getDefaultSimulatorName(simulator);
            if (!forSimulator(c, simulator)) {
                errors << Issue{Severity::Error, tr("%1: not available for %2").arg(c->Name, simName),
                                QPoint(c->cx, c->cy), c->Name};
            } else if (spiceSimulator(simulator) && c->SpiceModel.isEmpty() && !c->isEquation && !c->isProbe
                       && !isGround(c) && c->Model != QLatin1String(".Opt")) {   // Qucs runs an optimization
                errors << Issue{Severity::Error, tr("%1: has no SPICE model, %2 cannot simulate it").arg(c->Name, simName),
                                QPoint(c->cx, c->cy), c->Name};
            } else if (spiceSimulator(simulator) && c->Model == QLatin1String("EDD")
                       && !c->Props.isEmpty() && c->Props.first()->Value == QLatin1String("implicit")) {
                errors << Issue{Severity::Error,
                                tr("%1: an implicit equation-defined device has no SPICE form (use the explicit type)").arg(c->Name),
                                QPoint(c->cx, c->cy), c->Name};
            }
        }
        // An optimization Qucs runs (ngspice): what it cannot start with.
        if (c->Model == QLatin1String(".Opt") && simulator == spicecompat::simNgspice) {
            optimization::Problem problem;
            QString why;
            if (!optimization::Problem::read(c, &problem, &why)) {
                errors << Issue{Severity::Error, why, QPoint(c->cx, c->cy), c->Name};
            } else if (!problem.simulation.isEmpty()
                       && std::none_of(doc->a_DocComps.begin(), doc->a_DocComps.end(), [&](const Component* o) {
                              return isSimulation(o) && o != c
                                     && o->Name.compare(problem.simulation, Qt::CaseInsensitive) == 0;
                          })) {
                errors << Issue{Severity::Error,
                                tr("%1 optimizes %2, which is not in the schematic").arg(c->Name, problem.simulation),
                                QPoint(c->cx, c->cy), c->Name};
            }
        }
        // NgOpt: an optimize line the netlist cannot write.
        if (c->Model == QLatin1String(".NGOPT") && simulator == spicecompat::simNgspice) {
            QString line, why;
            if (!ngopt::commandLine(ngopt::Command::read(c), doc, &line, &why))
                errors << Issue{Severity::Error, tr("%1: %2").arg(c->Name, why), QPoint(c->cx, c->cy), c->Name};
        }
        // A Verilog-A component: its module in a library of the project, or
        // in a source compiled before the simulation.
        if (simulator == spicecompat::simNgspice && dynamic_cast<const vacomponent*>(c) != nullptr
            && !moduleInProject(c->Model))
            warnings << Issue{Severity::Warning,
                              tr("%1: the Verilog-A module %2 is in no library (.osdi) or source (.va) of the project")
                                  .arg(c->Name, c->Model),
                              QPoint(c->cx, c->cy), c->Name};
        // A winding refers to its magnetic core by name.
        if (c->Model == QLatin1String("WINDING")) {
            const QString core = c->getProperty("CORE") ? c->getProperty("CORE")->Value : QString();
            const bool found = std::any_of(doc->a_DocComps.begin(), doc->a_DocComps.end(), [&](const Component* o) {
                return o->Model == QLatin1String("CORE") && o->Name == core && inCircuit(o);
            });
            if (!found)
                errors << Issue{Severity::Error, tr("%1: no magnetic core named %2 in the schematic").arg(c->Name, core),
                                QPoint(c->cx, c->cy), c->Name};
        }

        // Pins connected to nothing.
        int pin = 0;
        for (const Port* p : c->Ports) {
            ++pin;
            if (!p->avail) continue;
            const Node* n = p->Connection;
            if (n == nullptr || n->conn_count() <= 1) {
                const QPoint where = n != nullptr ? QPoint(n->x(), n->y()) : QPoint(c->cx + p->x, c->cy + p->y);
                const QString what = isGround(c) ? tr("the ground at %1, %2 is connected to nothing").arg(where.x()).arg(where.y())
                                                 : tr("%1: pin %2 is connected to nothing").arg(c->Name).arg(pin);
                warnings << Issue{Severity::Warning, what, where, c->Name};
            }
        }
    }

    // A subcircuit port on a net without a label lends its own name to
    // that net, so the pin of the subcircuit is called after the port. A
    // label of that name on some other net takes it first, and the pin
    // reaches the netlist under a generated name instead.
    if (port) {
        QSet<QString> labels;
        for (const Node* n : doc->a_DocNodes)
            if (n->hasLabel()) labels.insert(n->label()->Name);
        for (const Wire* w : doc->a_DocWires)
            if (w->hasLabel()) labels.insert(w->label()->Name);

        for (Component* c : doc->a_DocComps) {
            if (!isPort(c) || !inCircuit(c) || c->Ports.isEmpty()) continue;
            if (!doc->netLabelOf(c->Ports.first()->Connection).isEmpty()) continue;
            if (!labels.contains(c->Name)) continue;

            warnings << Issue{Severity::Warning,
                              tr("%1: another net is labelled %1, so this pin gets a generated name").arg(c->Name),
                              QPoint(c->cx, c->cy), c->Name};
        }
    }

    // Wire ends connected to nothing: a node with one wire and no
    // component, unless it names a net.
    for (Node* n : doc->a_DocNodes) {
        if (n->conn_count() != 1 || n->wires().empty() || named(n)) continue;
        warnings << Issue{Severity::Warning,
                          tr("the wire end at %1, %2 is connected to nothing").arg(n->x()).arg(n->y()),
                          QPoint(n->x(), n->y()), QString()};
    }

    // A circuit (not a subcircuit: those have ports) needs a ground and
    // a simulation to be simulated.
    if (!port && !doc->a_DocComps.empty()) {
        const Component* first = *doc->a_DocComps.begin();
        const QPoint where(first->cx, first->cy);
        // An error unless the settings leave the ground to the user; then
        // a warning (a net named 0 or a SPICE part may bring node 0).
        if (!ground && QucsSettings.RequireGround)
            errors << Issue{Severity::Error, tr("no ground: the circuit has no reference node"), where, QString()};
        else if (!ground)
            warnings << Issue{Severity::Warning,
                              tr("no ground symbol: node 0 comes only from a net named 0 or a component that brings it"),
                              where, QString()};
        if (!simulation)
            warnings << Issue{Severity::Warning, tr("no simulation: no .AC, .TR, .DC, .SP, ... block"), where, QString()};
    }

    QList<Issue> all = errors + warnings;
    for (Issue& i : all) i.file = doc->getDocName();
    return all;
}

QStringList subcircuitFiles(Schematic* doc)
{
    QStringList files;
    if (doc == nullptr) return files;
    for (Component* c : doc->a_DocComps) {
        if (c->Model != QLatin1String("Sub")) continue;
        const QString file = c->getSubcircuitFile();
        if (!file.isEmpty() && !files.contains(file)) files << file;
    }
    return files;
}

int errorCount(const QList<Issue>& issues)
{
    return int(std::count_if(issues.begin(), issues.end(),
                             [](const Issue& i) { return i.severity == Severity::Error; }));
}

} // namespace qucs_s::erc
