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

#include <QCoreApplication>
#include <QHash>
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
    QHash<QString, const Component*> byName;
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
        if (!ground)
            errors << Issue{Severity::Error, tr("no ground: the circuit has no reference node"), where, QString()};
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
