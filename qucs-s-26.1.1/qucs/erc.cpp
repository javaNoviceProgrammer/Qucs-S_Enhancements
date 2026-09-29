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
#include "ngstatistics.h"
#include "ngsweep.h"
#include "osdiselection.h"
#include "misc.h"
#include "qucs.h"
#include "components/vacomponent.h"
#include "valuereading.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <algorithm>
#include <numeric>
#include <vector>

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


// The nets as the netlist makes them: wires join their ends, one label
// name is one net, the grounds are one net (and a net named 0 or gnd is
// ground too). Each node's net, a name to tell each by - a label, gnd, or
// its first pin - and the pins on each.
struct Nets {
    QHash<const Node*, int> of;
    QHash<int, QString> name;
    QHash<int, QStringList> pins;   // "R1.2", ...
    QSet<int> labelled;
    int ground = -1;
};

Nets netsOf(Schematic* doc)
{
    Nets nets;
    std::vector<int> up;
    QHash<const Node*, int> index;
    for (const Node* n : doc->a_DocNodes) {
        index.insert(n, int(up.size()));
        up.push_back(int(up.size()));
    }
    const auto find = [&up](int i) {
        while (up[i] != i) i = up[i] = up[up[i]];
        return i;
    };
    const auto join = [&](const Node* a, const Node* b) {
        if (a == nullptr || b == nullptr || !index.contains(a) || !index.contains(b)) return;
        up[find(index.value(a))] = find(index.value(b));
    };
    for (const Wire* w : doc->a_DocWires) join(w->Port1, w->Port2);
    QHash<QString, const Node*> byLabel;
    const Node* ground = nullptr;
    const auto label = [&](const QString& name, const Node* n) {
        if (name.compare(QLatin1String("0")) == 0 || name.compare(QLatin1String("gnd"), Qt::CaseInsensitive) == 0) {
            if (ground != nullptr) join(n, ground);
            else ground = n;
        }
        if (const Node* first = byLabel.value(name)) join(n, first);
        else byLabel.insert(name, n);
    };
    for (const Node* n : doc->a_DocNodes)
        if (n->hasLabel()) label(n->label()->Name, n);
    for (const Wire* w : doc->a_DocWires)
        if (w->hasLabel()) label(w->label()->Name, w->Port1);
    for (const Component* c : doc->a_DocComps)
        if (isGround(c) && inCircuit(c))
            for (const Port* p : c->Ports) {
                if (ground != nullptr) join(p->Connection, ground);
                else ground = p->Connection;
            }
    for (auto it = index.cbegin(); it != index.cend(); ++it) nets.of.insert(it.key(), find(it.value()));
    if (ground != nullptr) nets.ground = nets.of.value(ground, -1);
    for (auto it = byLabel.cbegin(); it != byLabel.cend(); ++it) {
        const int net = nets.of.value(it.value(), -1);
        nets.labelled.insert(net);
        if (!nets.name.contains(net)) nets.name.insert(net, it.key());
    }
    if (nets.ground >= 0) nets.name.insert(nets.ground, QStringLiteral("gnd"));
    for (const Component* c : doc->a_DocComps) {
        if (!inCircuit(c)) continue;
        for (int i = 0; i < c->Ports.size(); ++i) {
            const Node* n = c->Ports.at(i)->Connection;
            if (n == nullptr || !nets.of.contains(n)) continue;
            const int net = nets.of.value(n);
            const QString pin = QStringLiteral("%1.%2").arg(c->Name.isEmpty() ? c->Model : c->Name).arg(i + 1);
            nets.pins[net] << pin;
            if (!nets.name.contains(net)) nets.name.insert(net, pin);
        }
    }
    return nets;
}

QString netName(const Nets& nets, int net)
{
    return nets.name.value(net, tr("a net of wires only"));
}

// A straight wire: along x (fixed y) or along y (fixed x), lo < hi.
struct Piece {
    int fixed, lo, hi;
    const Wire* wire;
    int net;
};

QString wireText(const Wire* w)
{
    return QStringLiteral("%1, %2 - %3, %4").arg(w->x1).arg(w->y1).arg(w->x2).arg(w->y2);
}

// What wires and pins show and do not do: a wire's end or a pin on
// another net's wire mid-way (not joined), two nets' wires on top of each
// other; with \a crossings, wires of two nets crossing (no junction: no
// connection - often meant, so a note of its own).
void wiringIssues(Schematic* doc, const Nets& nets, QList<Issue>& out, bool crossings)
{
    std::vector<Piece> across, down;   // along x, along y
    for (const Wire* w : doc->a_DocWires) {
        const int net = nets.of.value(w->Port1, -1);
        if (w->y1 == w->y2 && w->x1 != w->x2) across.push_back({w->y1, std::min(w->x1, w->x2), std::max(w->x1, w->x2), w, net});
        else if (w->x1 == w->x2 && w->y1 != w->y2) down.push_back({w->x1, std::min(w->y1, w->y2), std::max(w->y1, w->y2), w, net});
    }
    const auto order = [](const Piece& a, const Piece& b) { return a.fixed != b.fixed ? a.fixed < b.fixed : a.lo < b.lo; };
    std::sort(across.begin(), across.end(), order);
    std::sort(down.begin(), down.end(), order);
    const auto fromFixed = [](const std::vector<Piece>& list, int fixed) {
        return std::lower_bound(list.begin(), list.end(), fixed, [](const Piece& p, int f) { return p.fixed < f; });
    };
    constexpr int kMost = 40;   // of each kind: a drawing full of them says so soon enough
    // Crossings, inside both wires.
    int crossed = 0;
    for (const Piece& v : crossings ? down : std::vector<Piece>())
        for (auto it = fromFixed(across, v.lo + 1); it != across.end() && it->fixed < v.hi; ++it) {
            if (it->net == v.net || !(it->lo < v.fixed && v.fixed < it->hi)) continue;
            if (++crossed > kMost) break;
            out << Issue{Severity::Warning,
                         tr("the wires at %1, %2 cross without a junction: nets %3 and %4 are not connected there")
                             .arg(v.fixed).arg(it->fixed).arg(netName(nets, v.net), netName(nets, it->net)),
                         QPoint(v.fixed, it->fixed), QString()};
        }
    // A node - a wire's end, a pin - on another net's wire, mid-way.
    int touches = 0;
    for (const Node* n : doc->a_DocNodes) {
        const int net = nets.of.value(n, -1);
        const auto onPiece = [&](const std::vector<Piece>& list, int fixed, int along) -> const Piece* {
            for (auto it = fromFixed(list, fixed); it != list.end() && it->fixed == fixed && it->lo < along; ++it)
                if (along < it->hi && it->net != net) return &*it;
            return nullptr;
        };
        const Piece* under = onPiece(across, n->cy, n->cx);
        if (under == nullptr) under = onPiece(down, n->cx, n->cy);
        if (under == nullptr) continue;
        if (++touches > kMost) break;
        const Component* part = n->anyComp();
        QString what;
        if (part != nullptr) {
            int pin = 0;
            for (int i = 0; i < part->Ports.size(); ++i)
                if (part->Ports.at(i)->Connection == n) pin = i + 1;
            what = tr("%1: pin %2 at %3, %4 is on the wire %5 of net %6 without being connected to it (a wire must end at a pin to join it)")
                       .arg(part->Name.isEmpty() ? part->Model : part->Name).arg(pin).arg(n->cx).arg(n->cy)
                       .arg(wireText(under->wire), netName(nets, under->net));
        } else {
            what = tr("the wire end at %1, %2 touches the wire %3 mid-way without a junction: nets %4 and %5 are not connected there")
                       .arg(n->cx).arg(n->cy).arg(wireText(under->wire), netName(nets, net), netName(nets, under->net));
        }
        out << Issue{Severity::Warning, what, n->center(), part != nullptr ? part->Name : QString()};
    }
    // Two nets' wires along one line, over each other.
    int overlaps = 0;
    for (const std::vector<Piece>* list : {&across, &down})
        for (std::size_t i = 0; i < list->size() && overlaps <= kMost; ++i)
            for (std::size_t j = i + 1; j < list->size(); ++j) {
                const Piece& a = (*list)[i];
                const Piece& b = (*list)[j];
                if (b.fixed != a.fixed || b.lo >= a.hi) break;
                if (a.net == b.net) continue;
                if (++overlaps > kMost) break;
                out << Issue{Severity::Warning,
                             tr("the wires %1 and %2 lie over each other but are two nets (%3, %4): they are not connected")
                                 .arg(wireText(a.wire), wireText(b.wire), netName(nets, a.net), netName(nets, b.net)),
                             QPoint(list == &across ? b.lo : a.fixed, list == &across ? a.fixed : b.lo), QString()};
            }
}

// What the circuit hangs from: parts on nets that reach no ground (nor a
// port, in a subcircuit) through any part - floating; nets that reach
// ground only through capacitors and current sources - no DC path, and
// no operating point (ngspice: a singular matrix).
void topologyIssues(Schematic* doc, const Nets& nets, bool subcircuit, QList<Issue>& out)
{
    QSet<int> ids;
    for (int net : nets.of) ids.insert(net);
    std::vector<int> any, dc;
    QHash<int, int> slot;
    for (int net : std::as_const(ids)) {
        slot.insert(net, int(any.size()));
        any.push_back(int(any.size()));
        dc.push_back(int(dc.size()));
    }
    const auto find = [](std::vector<int>& up, int i) {
        while (up[i] != i) i = up[i] = up[up[i]];
        return i;
    };
    // References: ground, and in a subcircuit its ports.
    QSet<int> reference;
    if (nets.ground >= 0) reference.insert(nets.ground);
    QList<const Component*> parts;
    for (const Component* c : doc->a_DocComps) {
        if (!inCircuit(c) || isSimulation(c) || c->isEquation || isGround(c)) continue;
        QList<int> on;
        for (const Port* p : c->Ports)
            if (p->Connection != nullptr && nets.of.contains(p->Connection)) on << slot.value(nets.of.value(p->Connection));
        if (on.isEmpty()) continue;
        if (isPort(c)) {
            reference.insert(nets.of.value(c->Ports.first()->Connection));
            continue;
        }
        parts << c;
        // A voltage probe is across the circuit and joins nothing; a
        // current probe is in it, a 0 V source.
        if (c->isProbe && c->Model != QLatin1String("IProbe")) continue;
        const bool conducts = c->SpiceModel != QLatin1String("C") && c->SpiceModel != QLatin1String("I");
        for (int k = 1; k < on.size(); ++k) {
            any[find(any, on.at(k))] = find(any, on.at(0));
            if (conducts) dc[find(dc, on.at(k))] = find(dc, on.at(0));
        }
    }
    if (reference.isEmpty()) return;   // (no ground: said already)
    QSet<int> anyReached, dcReached;
    for (int net : std::as_const(reference)) {
        anyReached.insert(find(any, slot.value(net)));
        dcReached.insert(find(dc, slot.value(net)));
    }
    // Floating: a group of parts touching no reference. A part with all
    // its pins open is told of pin by pin already.
    QHash<int, QStringList> floating;
    QHash<int, QPoint> floatingAt;
    for (const Component* c : std::as_const(parts)) {
        int connected = 0, group = -1;
        for (const Port* p : c->Ports) {
            if (p->Connection == nullptr || !nets.of.contains(p->Connection)) continue;
            group = find(any, slot.value(nets.of.value(p->Connection)));
            if (p->Connection->conn_count() > 1) ++connected;
        }
        if (group < 0 || anyReached.contains(group) || connected == 0) continue;
        if (!floatingAt.contains(group)) floatingAt.insert(group, QPoint(c->cx, c->cy));
        floating[group] << (c->Name.isEmpty() ? c->Model : c->Name);
    }
    for (auto it = floating.cbegin(); it != floating.cend(); ++it) {
        QStringList names = it.value();
        const int more = int(names.size()) - 6;
        names = names.mid(0, 6);
        out << Issue{Severity::Warning,
                     tr("%1%2: not connected to %3 or to the rest of the circuit (floating)")
                         .arg(names.join(QStringLiteral(", ")), more > 0 ? tr(" and %1 more").arg(more) : QString(),
                              subcircuit ? tr("a port or ground") : tr("ground")),
                     floatingAt.value(it.key()), names.first()};
    }
    // No DC path: reached, but only through capacitors or current sources.
    if (!subcircuit) {
        QHash<int, int> netOfDcGroup;   // a DC group -> a net of it with pins
        for (int net : std::as_const(ids)) {
            const int group = find(dc, slot.value(net));
            if (dcReached.contains(group) || !anyReached.contains(find(any, slot.value(net)))) continue;
            if (nets.pins.value(net).size() < 2) continue;   // (an open pin, a stub: told already)
            if (!netOfDcGroup.contains(group)) netOfDcGroup.insert(group, net);
        }
        for (auto it = netOfDcGroup.cbegin(); it != netOfDcGroup.cend(); ++it) {
            // What blocks it: the capacitors and current sources on its nets.
            QStringList blocking;
            QPoint at;
            for (const Component* c : std::as_const(parts)) {
                if (c->SpiceModel != QLatin1String("C") && c->SpiceModel != QLatin1String("I")) continue;
                for (const Port* p : c->Ports)
                    if (p->Connection != nullptr && find(dc, slot.value(nets.of.value(p->Connection, -1), 0)) == it.key()) {
                        if (!blocking.contains(c->Name)) blocking << c->Name;
                        if (at.isNull()) at = QPoint(c->cx, c->cy);
                    }
            }
            out << Issue{Severity::Warning,
                         tr("net %1 reaches ground only through capacitors or current sources (%2): no DC path, so no "
                            "operating point (a large resistor to ground gives it one)")
                             .arg(netName(nets, it.value()), blocking.mid(0, 6).join(QStringLiteral(", "))),
                         at, blocking.value(0)};
        }
    }
}

// A pin named as a supply's (VCC, VEE, VDD, VSS, V+, V-) on a net with no
// other pin but supply pins: nothing powers the part - an op-amp's VCC and
// VEE left open read as a part that does nothing, every answer green.
void supplyIssues(Schematic* doc, const Nets& nets, QList<Issue>& out)
{
    static const QRegularExpression supply(QStringLiteral("^(v(cc|dd|ee|ss|s[+-]|[+-]|pos|neg)\\d*|avdd|dvdd)$"),
                                           QRegularExpression::CaseInsensitiveOption);
    QHash<QString, bool> supplyPin;   // "U1.4": a supply pin
    for (const Component* c : doc->a_DocComps)
        for (int i = 0; i < c->Ports.size(); ++i)
            supplyPin.insert(QStringLiteral("%1.%2").arg(c->Name.isEmpty() ? c->Model : c->Name).arg(i + 1),
                             supply.match(c->Ports.at(i)->Name).hasMatch());
    for (const Component* c : doc->a_DocComps) {
        if (!inCircuit(c)) continue;
        QStringList unpowered;
        for (int i = 0; i < c->Ports.size(); ++i) {
            const Port* p = c->Ports.at(i);
            if (!supply.match(p->Name).hasMatch() || p->Connection == nullptr) continue;
            const int net = nets.of.value(p->Connection, -1);
            if (net < 0 || net == nets.ground) continue;
            const QStringList on = nets.pins.value(net);
            if (std::all_of(on.cbegin(), on.cend(), [&](const QString& pin) { return supplyPin.value(pin); }))
                unpowered << p->Name;
        }
        if (!unpowered.isEmpty())
            out << Issue{Severity::Warning,
                         tr("%1: its supply pin%2 %3 %4 on nothing that powers it (no source, no other part): the part is "
                            "unpowered - wire a supply to it (a Vdc to ground)")
                             .arg(c->Name, unpowered.size() > 1 ? QStringLiteral("s") : QString(), unpowered.join(QStringLiteral(", ")),
                                  unpowered.size() > 1 ? tr("are") : tr("is")),
                         QPoint(c->cx, c->cy), c->Name};
    }
}

// A DC source whose + is on ground (or whose value is negative with its -
// there): the net at its other pin is below ground. A negative supply is
// so; one meant to be positive, drawn the other way round, is not.
void polarityNotes(Schematic* doc, const Nets& nets, QList<Issue>& out)
{
    for (const Component* c : doc->a_DocComps) {
        if (!inCircuit(c) || c->Model != QLatin1String("Vdc") || c->Ports.size() != 2 || c->Props.isEmpty()) continue;
        const int plus = c->Ports.at(0)->Connection != nullptr ? nets.of.value(c->Ports.at(0)->Connection, -1) : -1;
        const int minus = c->Ports.at(1)->Connection != nullptr ? nets.of.value(c->Ports.at(1)->Connection, -1) : -1;
        if (nets.ground < 0 || (plus == nets.ground) == (minus == nets.ground)) continue;
        const qucs_s::units::Reading r = qucs_s::units::read(c->Props.first()->Value);
        if (r.kind != qucs_s::units::Reading::Number || r.value == 0) continue;
        const int other = plus == nets.ground ? minus : plus;
        const double level = plus == nets.ground ? -r.value : r.value;
        if (level >= 0 || nets.pins.value(other).size() < 2) continue;
        out << Issue{Severity::Warning,
                     tr("%1: its %2 is on ground, so %3 is at %4 V - a negative supply is so; if it was to be positive, turn it round "
                        "(edit_component with rotation) or give it %5")
                         .arg(c->Name, plus == nets.ground ? tr("+") : tr("- (its value negative)"), netName(nets, other))
                         .arg(level)
                         .arg(plus == nets.ground ? tr("its + on the net") : tr("a positive value")),
                     QPoint(c->cx, c->cy), c->Name};
    }
}

// A net label on one pin alone: a node named to be plotted or read by an
// expression, or a label meant to match another - a note.
void labelNotes(Schematic* doc, const Nets& nets, QList<Issue>& out)
{
    for (int net : std::as_const(nets.labelled)) {
        if (net == nets.ground || nets.pins.value(net).size() != 1) continue;
        QPoint at;
        for (const Node* n : doc->a_DocNodes)
            if (nets.of.value(n, -1) == net && n->hasLabel()) at = n->center();
        out << Issue{Severity::Warning,
                     tr("the net %1 has one pin only (%2): nothing else is on it - a label of the same name elsewhere would join it")
                         .arg(netName(nets, net), nets.pins.value(net).first()),
                     at, QString()};
    }
}

} // namespace

QList<Issue> wiring(Schematic* doc)
{
    QList<Issue> out;
    if (doc == nullptr) return out;
    wiringIssues(doc, netsOf(doc), out, true);
    for (Issue& i : out) i.file = doc->getDocName();
    return out;
}

QList<Issue> notes(Schematic* doc)
{
    QList<Issue> out;
    if (doc == nullptr) return out;
    const Nets nets = netsOf(doc);
    QList<Issue> all;
    wiringIssues(doc, nets, all, true);
    for (const Issue& i : std::as_const(all))
        if (i.message.contains(QLatin1String(" cross without a junction"))) out << i;
    labelNotes(doc, nets, out);
    polarityNotes(doc, nets, out);
    for (Issue& i : out) i.file = doc->getDocName();
    return out;
}

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
        // A library part or a subcircuit that could not be loaded: drawn as
        // a box without pins, so the wires that met its pins end on nothing
        // - which alone was told, never why (a library not found).
        if (c->Model == QLatin1String("Lib") && c->Ports.isEmpty() && c->Props.size() >= 2)
            errors << Issue{Severity::Error,
                            tr("%1: the library part %2 of %3 could not be loaded (not in the libraries of %4, nor the "
                               "project's user_lib): it has no pins, and what was wired to them is on nothing")
                                .arg(c->Name, c->Props.at(1)->Value, c->Props.at(0)->Value, QDir::toNativeSeparators(QucsSettings.LibDir)),
                            QPoint(c->cx, c->cy), c->Name};
        if (c->Model == QLatin1String("Sub") && !c->Props.isEmpty() && !QFileInfo::exists(c->getSubcircuitFile()))
            errors << Issue{Severity::Error,
                            tr("%1: its subcircuit %2 is not found (beside the schematic, in the project or its user_lib): "
                               "it has no pins, and what was wired to them is on nothing").arg(c->Name, c->Props.at(0)->Value),
                            QPoint(c->cx, c->cy), c->Name};
        // NgOpt: an optimize line the netlist cannot write.
        if (c->Model == QLatin1String(".NGOPT") && simulator == spicecompat::simNgspice) {
            QString line, why;
            if (!ngopt::commandLine(ngopt::Command::read(c), doc, &line, &why))
                errors << Issue{Severity::Error, tr("%1: %2").arg(c->Name, why), QPoint(c->cx, c->cy), c->Name};
        }
        // NgMonteCarlo, NgCorners: a montecarlo or corners line the netlist
        // cannot write.
        if (ngstats::isStatistics(c) && simulator == spicecompat::simNgspice) {
            QString line, why;
            if (!ngstats::commandLine(c, doc, &line, &why))
                errors << Issue{Severity::Error, tr("%1: %2").arg(c->Name, why), QPoint(c->cx, c->cy), c->Name};
        }
        // NgSweep: a sweep line the netlist cannot write.
        if (ngsweep::isSweep(c) && simulator == spicecompat::simNgspice) {
            QString line, why;
            if (!ngsweep::commandLine(c, doc, QString(), &line, &why))
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
            // (A net label on it joins it by its name: it is not alone.)
            if (n == nullptr || (n->conn_count() <= 1 && !named(n))) {
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

    // What the wires show and do not do; what hangs from nothing.
    {
        const Nets nets = netsOf(doc);
        wiringIssues(doc, nets, warnings, false);
        topologyIssues(doc, nets, port, warnings);
        supplyIssues(doc, nets, warnings);
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
