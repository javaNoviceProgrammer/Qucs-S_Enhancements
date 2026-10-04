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
#include "components/libcomp.h"
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
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <utility>
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
    // Labels that are one net only because the simulator reads names
    // without regard to case (Out, out): their names, and where one is.
    QList<QPair<QStringList, QPoint>> caseJoins;
};

// Whether the simulator in use reads names without regard to case: the
// SPICE ones (ngspice, Xyce, SPICE OPUS), for an analog circuit.
// A name that makes a net ground in the netlist: 0, and gnd, which the
// netlist writes as 0 - and gnd in any case under ngspice, which reads it
// so. SPICE OPUS and Xyce read GND as a node of its own.
bool groundName(Schematic* doc, const QString& name)
{
    if (name == QLatin1String("0") || name == QLatin1String("gnd")) return true;
    // The name first: whether the circuit is digital looks at every part,
    // and asked for every net's name it made a check of 37,500 parts take
    // minutes.
    return name.compare(QLatin1String("gnd"), Qt::CaseInsensitive) == 0
           && QucsSettings.DefaultSimulator == spicecompat::simNgspice && !doc->isDigitalCircuit();
}

bool namesWithoutCase(Schematic* doc)
{
    return spiceSimulator(QucsSettings.DefaultSimulator) && !doc->isDigitalCircuit();
}

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
        if (groundName(doc, name)) {
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
    // Labels whose names differ only in case, for a simulator that reads
    // them without: one net. Where the wires and the labels as written
    // keep them apart, that is told.
    if (namesWithoutCase(doc)) {
        std::map<QString, QStringList> spellings;   // (in order: the same findings every run)
        for (auto it = byLabel.cbegin(); it != byLabel.cend(); ++it) spellings[it.key().toLower()] << it.key();
        for (auto it = spellings.begin(); it != spellings.end(); ++it) {
            QStringList& names = it->second;
            if (names.size() < 2) continue;
            names.sort();
            const Node* first = byLabel.value(names.first());
            bool apart = false;
            for (const QString& other : std::as_const(names)) {
                const Node* n = byLabel.value(other);
                if (index.contains(n) && index.contains(first) && find(index.value(n)) != find(index.value(first))) apart = true;
                join(n, first);
            }
            if (apart) {
                const Node* at = byLabel.value(names.at(1));
                nets.caseJoins << qMakePair(names, QPoint(at->x(), at->y()));
            }
        }
    }
    for (auto it = index.cbegin(); it != index.cend(); ++it) nets.of.insert(it.key(), find(it.value()));
    if (ground != nullptr) nets.ground = nets.of.value(ground, -1);
    // A net of several labels is named after the first in order, the same
    // every run (a hash's order is not).
    QStringList labels = byLabel.keys();
    labels.sort();
    for (const QString& label : std::as_const(labels)) {
        const int net = nets.of.value(byLabel.value(label), -1);
        nets.labelled.insert(net);
        if (!nets.name.contains(net)) nets.name.insert(net, label);
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

// A subcircuit's file that is there: a file, not a folder, by a path that
// is whole (a relative one would be read from the process's working folder).
bool foundFile(const QString& file)
{
    const QFileInfo fi(file);
    return fi.isAbsolute() && fi.isFile();
}

// Whether two paths are one file (through a link: /tmp and /private/tmp).
bool sameFile(const QString& a, const QString& b)
{
    if (a.isEmpty() || b.isEmpty()) return false;
    const QString one = QFileInfo(a).canonicalFilePath();
    return !one.isEmpty() && one == QFileInfo(b).canonicalFilePath();
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
    // In order: the groups, and what is told of them, the same every run.
    QList<int> sortedIds(ids.cbegin(), ids.cend());
    std::sort(sortedIds.begin(), sortedIds.end());
    for (int net : std::as_const(sortedIds)) {
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
        // A part switched to shorted is in the netlist as next to no
        // resistance from its first pin to each other one: it joins its
        // nets, and is no part of its own. (A ground so is no ground.)
        const bool shorted = c->isActive == COMP_IS_SHORTEN && !isSimulation(c) && !c->isEquation && !isGround(c);
        if (!shorted && (!inCircuit(c) || isSimulation(c) || c->isEquation || isGround(c))) continue;
        QList<int> on;
        for (const Port* p : c->Ports)
            if (p->Connection != nullptr && nets.of.contains(p->Connection)) on << slot.value(nets.of.value(p->Connection));
        if (on.isEmpty()) continue;
        if (shorted) {
            for (int k = 1; k < on.size(); ++k) {
                any[find(any, on.at(k))] = find(any, on.at(0));
                dc[find(dc, on.at(k))] = find(dc, on.at(0));
            }
            continue;
        }
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
            // (A pin with a label is on its net by the name.)
            if (p->Connection->conn_count() > 1 || named(p->Connection)) ++connected;
        }
        if (group < 0 || anyReached.contains(group) || connected == 0) continue;
        if (!floatingAt.contains(group)) floatingAt.insert(group, QPoint(c->cx, c->cy));
        floating[group] << (c->Name.isEmpty() ? c->Model : c->Name);
    }
    QList<int> groups = floating.keys();
    std::sort(groups.begin(), groups.end());
    for (int group : std::as_const(groups)) {
        const auto it = floating.constFind(group);
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
        std::map<int, int> netOfDcGroup;   // a DC group -> a net of it with pins
        for (int net : std::as_const(sortedIds)) {
            const int group = find(dc, slot.value(net));
            if (dcReached.contains(group) || !anyReached.contains(find(any, slot.value(net)))) continue;
            if (nets.pins.value(net).size() < 2) continue;   // (an open pin, a stub: told already)
            netOfDcGroup.emplace(group, net);
        }
        for (auto it = netOfDcGroup.cbegin(); it != netOfDcGroup.cend(); ++it) {
            // What blocks it: the capacitors and current sources on its nets.
            QStringList blocking;
            QPoint at;
            for (const Component* c : std::as_const(parts)) {
                if (c->SpiceModel != QLatin1String("C") && c->SpiceModel != QLatin1String("I")) continue;
                for (const Port* p : c->Ports)
                    if (p->Connection != nullptr && find(dc, slot.value(nets.of.value(p->Connection, -1), 0)) == it->first) {
                        if (!blocking.contains(c->Name)) blocking << c->Name;
                        if (at.isNull()) at = QPoint(c->cx, c->cy);
                    }
            }
            out << Issue{Severity::Warning,
                         tr("net %1 reaches ground only through capacitors or current sources (%2): no DC path, so no "
                            "operating point (a large resistor to ground gives it one)")
                             .arg(netName(nets, it->second), blocking.mid(0, 6).join(QStringLiteral(", "))),
                         at, blocking.value(0)};
        }
    }
}

// A pin name that is a supply's: VCC, VEE, VDD, VSS, V+, V-, POSRAIL ...
bool supplyName(const QString& name)
{
    static const QRegularExpression supply(QStringLiteral("^(v(cc|dd|ee|ss|s[+-]|[+-]|pos|neg)\\d*|(pos|neg)rail|avdd|dvdd)$"),
                                           QRegularExpression::CaseInsensitiveOption);
    return supply.match(name).hasMatch();
}

// An op-amp's input by its pin's name: IN+, INP, NONINV, POSIN ... - and a
// dual's, numbered or lettered: IN1+, -INA, INBN, NONINV2.
bool inputName(const QString& name)
{
    static const QRegularExpression input(QStringLiteral("^(in\\d*[a-d]?[+-]|[+-]in\\d*[a-d]?|in\\d*[a-d]?_?(p|n|pos|neg)|"
                                                         "(pos|neg)in\\d*[a-d]?|non_?inv\\d*[a-d]?|inv\\d*[a-d]?|"
                                                         "vin\\d*[a-d]?[+-]|vin\\d*[a-d]?[pn])$"),
                                          QRegularExpression::CaseInsensitiveOption);
    return input.match(name).hasMatch();
}

// An input of one pin, not an op-amp's pair: IN, IN1, INA, VIN, INPUT. (A
// regulator's IN, a coupler's: a role, but no bias for the check to ask of.)
bool plainInputName(const QString& name)
{
    static const QRegularExpression input(QStringLiteral("^(v?in\\d*[a-d]?|input\\d*[a-d]?)$"), QRegularExpression::CaseInsensitiveOption);
    return input.match(name).hasMatch();
}

// An output by its pin's name: OUT, VOUT, OUTPUT - numbered or lettered
// (OUT1, OUTA: a dual's), or one of a pair (OUT+, OUTN).
bool outputName(const QString& name)
{
    static const QRegularExpression output(QStringLiteral("^(v?out\\d*[a-d]?[+pn-]?|output\\d*[a-d]?)$"),
                                           QRegularExpression::CaseInsensitiveOption);
    return output.match(name).hasMatch();
}

// A pin named as a supply's on a net with no other pin but supply pins:
// nothing powers the part - an op-amp's VCC and VEE left open read as a
// part that does nothing, every answer green.
void supplyIssues(Schematic* doc, const Nets& nets, QList<Issue>& out)
{
    QHash<QString, bool> supplyPin;   // "U1.4": a supply pin
    for (const Component* c : doc->a_DocComps)
        for (int i = 0; i < c->Ports.size(); ++i)
            supplyPin.insert(QStringLiteral("%1.%2").arg(c->Name.isEmpty() ? c->Model : c->Name).arg(i + 1),
                             supplyName(c->Ports.at(i)->Name));
    for (const Component* c : doc->a_DocComps) {
        if (!inCircuit(c)) continue;
        QStringList unpowered;
        for (int i = 0; i < c->Ports.size(); ++i) {
            const Port* p = c->Ports.at(i);
            if (!supplyName(p->Name) || p->Connection == nullptr) continue;
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

// A supply by its name: +1 a positive one (VCC, VDD, V+, POSRAIL), -1 a
// negative one (VEE, VSS, V-, NEGRAIL), 0 another.
int supplySign(const QString& name)
{
    static const QRegularExpression positive(QStringLiteral("^(v(cc|dd|s\\+|\\+|pos)\\d*|posrail|avdd|dvdd)$"),
                                             QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression negative(QStringLiteral("^(v(ee|ss|s-|-|neg)\\d*|negrail|avss|dvss)$"),
                                             QRegularExpression::CaseInsensitiveOption);
    return positive.match(name).hasMatch() ? 1 : negative.match(name).hasMatch() ? -1 : 0;
}

// A DC source with one pin on ground: the level of the net at its other
// pin, and what the names on that net say it is to be. A negative level
// on a net of a negative supply's name (a label vee, an op-amp's VEE pin,
// the source VEE itself) is as meant - nothing said. On a positive
// supply's pin (VCC at -15 V), or a negative supply's at a positive level
// (VEE at +15 V), the source is the wrong way round: a warning. A negative
// level on a net nothing names: a note, as it may be either.
void polarityIssues(Schematic* doc, const Nets& nets, QList<Issue>* warnings, QList<Issue>* notes)
{
    QHash<QString, QString> pinName;   // "U1.4": VCC
    for (const Component* c : doc->a_DocComps)
        for (int i = 0; i < c->Ports.size(); ++i)
            if (!c->Ports.at(i)->Name.isEmpty())
                pinName.insert(QStringLiteral("%1.%2").arg(c->Name.isEmpty() ? c->Model : c->Name).arg(i + 1), c->Ports.at(i)->Name);
    for (const Component* c : doc->a_DocComps) {
        if (!inCircuit(c) || c->Model != QLatin1String("Vdc") || c->Ports.size() != 2 || c->Props.isEmpty()) continue;
        const int plus = c->Ports.at(0)->Connection != nullptr ? nets.of.value(c->Ports.at(0)->Connection, -1) : -1;
        const int minus = c->Ports.at(1)->Connection != nullptr ? nets.of.value(c->Ports.at(1)->Connection, -1) : -1;
        if (nets.ground < 0 || (plus == nets.ground) == (minus == nets.ground)) continue;
        const qucs_s::units::Reading r = qucs_s::units::read(c->Props.first()->Value);
        if (r.kind != qucs_s::units::Reading::Number || r.value == 0) continue;
        const int other = plus == nets.ground ? minus : plus;
        const double level = plus == nets.ground ? -r.value : r.value;
        if (nets.pins.value(other).size() < 2) continue;
        // The supply pins on the net, by sign; its label; the source's name.
        QStringList positivePins, negativePins;
        const QString self = QStringLiteral("%1.").arg(c->Name);
        for (const QString& pin : nets.pins.value(other)) {
            if (pin.startsWith(self)) continue;
            const int sign = supplySign(pinName.value(pin));
            if (sign > 0) positivePins << QStringLiteral("%1 (%2)").arg(pin, pinName.value(pin));
            else if (sign < 0) negativePins << QStringLiteral("%1 (%2)").arg(pin, pinName.value(pin));
        }
        // The net's name: its label, or in a subcircuit a port on it (VEE:
        // the net's name to the outside) when the label says no supply.
        int labelSign = nets.labelled.contains(other) ? supplySign(nets.name.value(other)) : 0;
        QString net = netName(nets, other);
        if (labelSign == 0)
            for (const Component* port : doc->a_DocComps)
                if (isPort(port) && inCircuit(port) && !port->Ports.isEmpty() && port->Ports.first()->Connection != nullptr
                    && nets.of.value(port->Ports.first()->Connection, -1) == other && supplySign(port->Name) != 0) {
                    labelSign = supplySign(port->Name);
                    net = tr("the port %1").arg(port->Name);
                    break;
                }
        // The fix: its value the other sign (a turn is refused while wires
        // would join other nets). "+15 V" is "-15 V" so, not "-+15 V".
        const QString value = c->Props.first()->Value.trimmed();
        const QString flipped = value.startsWith(QLatin1Char('-'))   ? value.mid(1).trimmed()
                                : value.startsWith(QLatin1Char('+')) ? QLatin1Char('-') + value.mid(1).trimmed()
                                                                     : QLatin1Char('-') + value;
        const QString flip = tr("set %1 to %2 (edit_component)").arg(c->Props.first()->Name, flipped);
        // "VCC puts U1.4 (VCC) at -15 V, though it is a positive supply's
        // pin"; "... puts vcc at -15 V, though U1.4 (VCC) on it is ...".
        const auto wrongWay = [&](const QStringList& pins, const QString& sign, const QString& fix) {
            const QString kind = pins.size() > 1 ? tr("%1 supply's pins").arg(sign) : tr("a %1 supply's pin").arg(sign);
            const QString why = pins.isEmpty() ? tr("%1 at %2 V, though that is a %3 supply's name").arg(net).arg(level).arg(sign)
                                : nets.labelled.contains(other)
                                    ? tr("%1 at %2 V, though %3 on it %4 %5").arg(net).arg(level)
                                          .arg(pins.join(QStringLiteral(", ")), pins.size() > 1 ? tr("are") : tr("is"), kind)
                                    : tr("%1 at %2 V, though %3 %4").arg(pins.join(QStringLiteral(", "))).arg(level)
                                          .arg(pins.size() > 1 ? tr("they are") : tr("it is"), kind);
            return tr("%1 puts %2: the source is the wrong way round - %3").arg(c->Name, why, fix);
        };
        if (level < 0 && (!positivePins.isEmpty() || (negativePins.isEmpty() && labelSign > 0))) {
            if (warnings) *warnings << Issue{Severity::Warning, wrongWay(positivePins, tr("positive"), flip), QPoint(c->cx, c->cy), c->Name};
            continue;
        }
        if (level > 0 && (!negativePins.isEmpty() || (positivePins.isEmpty() && labelSign < 0))) {
            if (warnings)
                *warnings << Issue{Severity::Warning,
                                   wrongWay(negativePins, tr("negative"), flip),
                                   QPoint(c->cx, c->cy), c->Name};
            continue;
        }
        if (level >= 0 || !negativePins.isEmpty() || labelSign < 0 || supplySign(c->Name) < 0 || !notes) continue;
        *notes << Issue{Severity::Warning,
                        tr("%1: its %2 is on ground, so %3 is at %4 V - a negative supply is so; if it was to be positive, %5")
                            .arg(c->Name, plus == nets.ground ? tr("+") : tr("- (its value negative)"), net)
                            .arg(level)
                            .arg(flip),
                        QPoint(c->cx, c->cy), c->Name};
    }
}

// A net label on one pin alone: a node named to be plotted or read by an
// expression, or a label meant to match another - a note.
void labelNotes(Schematic* doc, const Nets& nets, QList<Issue>& out)
{
    QList<int> labelled(nets.labelled.cbegin(), nets.labelled.cend());
    std::sort(labelled.begin(), labelled.end());
    for (int net : std::as_const(labelled)) {
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

// ---- What the parts do, as far as the drawing tells it: sources and
// the loops they make, values, what drives an AC analysis, names that
// plot nothing - and, as notes, what a design review would ask.

bool twoPins(const Component* c) { return c->Ports.size() == 2; }
bool voltageSource(const Component* c) { return c->SpiceModel == QLatin1String("V") && twoPins(c); }
bool inductor(const Component* c) { return c->SpiceModel == QLatin1String("L") && twoPins(c); }
bool capacitor(const Component* c) { return c->SpiceModel == QLatin1String("C") && twoPins(c); }
bool resistor(const Component* c) { return c->SpiceModel == QLatin1String("R") && twoPins(c); }

// The net of a part's pin \a i; -1 on none.
int netOfPin(const Nets& nets, const Component* c, int i)
{
    const Node* n = i < c->Ports.size() ? c->Ports.at(i)->Connection : nullptr;
    return n != nullptr ? nets.of.value(n, -1) : -1;
}

// A part's value property (R, C, L; the first of a SPICE one), as read.
qucs_s::units::Reading valueOf(const Component* c, QString* text)
{
    for (const Property* p : c->Props)
        if (p->Name == c->SpiceModel) {
            *text = p->Value;
            return qucs_s::units::read(p->Value);
        }
    return {};
}

// Parts whose inside the check does not see: a subcircuit, a library
// part, a SPICE netlist or text of the user's - an AC source or a node
// may be in there.
bool opaque(const Component* c)
{
    static const QStringList models{QStringLiteral("Sub"), QStringLiteral("Lib"), QStringLiteral("SpLib"),
                                    QStringLiteral("SPICE"), QStringLiteral("SPICE_dev"), QStringLiteral("SpiceInclude"),
                                    QStringLiteral("INCLSCR"), QStringLiteral(".CUSTOMSIM"), QStringLiteral(".XYCESCR")};
    return models.contains(c->Model);
}

// "V1 and V2", "V1, V2 and L1".
QString listed(const QStringList& names)
{
    if (names.size() < 2) return names.value(0);
    return tr("%1 and %2").arg(names.mid(0, names.size() - 1).join(QStringLiteral(", ")), names.last());
}

// A branch that fixes a voltage, as the netlist has it: an independent
// voltage source; a current probe (a 0 V source); a controlled voltage
// source's output (E, H, B); a current-controlled source's input (the 0 V
// source the netlist adds to sense its current); an ideal op-amp's output
// (a voltage to ground). Its two pins (-1: ground), what to call it, and
// what ngspice calls it shorted.
struct Branch {
    int a, b;
    QString what;      // "V1", "Pr1 (a current probe: a 0 V source)"
    QString shorted;   // "VSRC"
};

QList<Branch> voltageBranches(const Component* c)
{
    const QString name = c->Name;
    if (c->Model == QLatin1String("IProbe") && twoPins(c))
        return {{0, 1, tr("%1 (a current probe: a 0 V source)").arg(name), QStringLiteral("VSRC")}};
    if (voltageSource(c)) return {{0, 1, name, QStringLiteral("VSRC")}};
    if (c->Model == QLatin1String("eNL") && twoPins(c))
        return {{0, 1, tr("%1 (a controlled voltage source)").arg(name), QStringLiteral("VCVS")}};
    if (c->Model == QLatin1String("src_eqndef") && twoPins(c))
        return {{0, 1, tr("%1 (a controlled voltage source)").arg(name), QStringLiteral("ASRC")}};
    if (c->Model == QLatin1String("OpAmp") && c->Ports.size() == 3)
        return {{2, -1, tr("%1's output (an ideal op-amp's: a voltage to ground)").arg(name), QStringLiteral("ASRC")}};
    if (c->Ports.size() == 4) {   // in+ out+ out- in-
        const Branch sense{0, 3, tr("%1's input (a 0 V source that senses its current)").arg(name), QStringLiteral("VSRC")};
        if (c->Model == QLatin1String("VCVS")) return {{1, 2, tr("%1's output").arg(name), QStringLiteral("VCVS")}};
        if (c->Model == QLatin1String("CCVS")) return {{1, 2, tr("%1's output").arg(name), QStringLiteral("CCVS")}, sense};
        if (c->Model == QLatin1String("CCCS")) return {sense};
    }
    return {};
}

// Voltage sources and inductors in loops. A voltage source across one net
// (a wire from pin to pin) stops ngspice ("shorted VSRC"); voltage sources
// in a loop fix one difference twice, and the operating point fails
// (singular matrix); with inductors in the loop the same happens at DC,
// where an inductor is a short. A voltage source is any branch that fixes
// a voltage (voltageBranches).
void sourceLoops(Schematic* doc, const Nets& nets, QList<Issue>& errors, QList<Issue>& warnings)
{
    QHash<int, int> up;
    const auto find = [&up](int i) {
        int root = i;
        while (up.value(root, root) != root) root = up.value(root, root);
        while (up.value(i, i) != root) {
            const int next = up.value(i, i);
            up.insert(i, root);
            i = next;
        }
        return root;
    };
    struct Edge {
        int a, b;
        QString what;
        bool inductor;
    };
    std::vector<Edge> taken;
    // The branches on a way from net \a from to net \a to over the edges taken.
    const auto way = [&taken](int from, int to) {
        QHash<int, int> cameBy;   // net -> index of the edge it was reached by
        QList<int> todo{from};
        cameBy.insert(from, -1);
        while (!todo.isEmpty() && !cameBy.contains(to)) {
            const int at = todo.takeFirst();
            for (int k = 0; k < int(taken.size()); ++k) {
                const Edge& e = taken.at(k);
                const int next = e.a == at ? e.b : e.b == at ? e.a : -1;
                if (next < 0 || cameBy.contains(next)) continue;
                cameBy.insert(next, k);
                todo << next;
            }
        }
        QList<const Edge*> edges;
        for (int at = to; cameBy.value(at, -1) >= 0;) {
            const Edge& e = taken.at(cameBy.value(at));
            edges.prepend(&e);
            at = e.a == at ? e.b : e.a;
        }
        return edges;
    };
    int told = 0;
    for (const bool sources : {true, false})
        for (const Component* c : doc->a_DocComps) {
            if (!inCircuit(c)) continue;
            QList<Branch> branches;
            if (sources) branches = voltageBranches(c);
            else if (inductor(c)) branches = {{0, 1, c->Name, QString()}};
            for (const Branch& branch : std::as_const(branches)) {
                const int a = branch.a < 0 ? nets.ground : netOfPin(nets, c, branch.a);
                const int b = branch.b < 0 ? nets.ground : netOfPin(nets, c, branch.b);
                if (a < 0 || b < 0) continue;
                if (a == b) {
                    if (sources)
                        errors << Issue{Severity::Error,
                                        branch.what == c->Name
                                            ? tr("%1 is shorted: both its pins are on %2, and a voltage source across a wire has "
                                                 "no solution (ngspice stops: \"shorted %3\")")
                                                  .arg(c->Name, netName(nets, a), branch.shorted)
                                            : tr("%1 is shorted: both its ends are on %2, and a voltage source across a wire has "
                                                 "no solution (ngspice stops: \"shorted %3\")")
                                                  .arg(branch.what, netName(nets, a), branch.shorted),
                                        QPoint(c->cx, c->cy), c->Name};
                    continue;
                }
                if (find(a) == find(b)) {
                    if (++told > 20) continue;
                    QList<const Edge*> loop = way(a, b);
                    const Edge self{a, b, branch.what, !sources};
                    loop << &self;
                    QStringList names;
                    bool withInductor = false;
                    for (const Edge* e : std::as_const(loop)) {
                        names << e->what;
                        if (e->inductor) withInductor = true;
                    }
                    const QString how = loop.size() == 2 ? tr("%1 are in parallel").arg(listed(names))
                                                         : tr("%1 are in a loop").arg(listed(names));
                    if (!withInductor)
                        errors << Issue{Severity::Error,
                                        tr("%1: voltage sources in a loop fix one voltage twice, and the operating point fails "
                                           "(ngspice: singular matrix) - keep one, or put a resistor in the loop")
                                            .arg(how),
                                        QPoint(c->cx, c->cy), c->Name};
                    else
                        warnings << Issue{Severity::Warning,
                                          tr("%1: at DC an inductor is a short, so this loop of voltage sources and inductors has "
                                             "no operating point (ngspice: singular matrix) - a resistor in series with the "
                                             "inductor gives it one")
                                              .arg(how),
                                          QPoint(c->cx, c->cy), c->Name};
                    continue;
                }
                up.insert(find(a), find(b));
                taken.push_back({a, b, branch.what, !sources});
            }
        }
}

// Values a simulator takes but no one means. \a warnings: a negative
// capacitance (a transient runs away with it: -9e8 V in a test). \a notes,
// fine if meant: a resistor or an inductor of nothing (a short, a jumper
// kept to be set later), a negative resistance or inductance.
void valueIssues(Schematic* doc, QList<Issue>* warnings, QList<Issue>* notes)
{
    for (const Component* c : doc->a_DocComps) {
        if (!inCircuit(c) || !(resistor(c) || capacitor(c) || inductor(c))) continue;
        QString text;
        const qucs_s::units::Reading r = valueOf(c, &text);
        if (r.kind != qucs_s::units::Reading::Number) continue;
        const QPoint at(c->cx, c->cy);
        if (capacitor(c) && r.value < 0 && warnings != nullptr)
            *warnings << Issue{Severity::Warning,
                               tr("%1 is %2: a negative capacitance - a transient runs away with it").arg(c->Name, text.trimmed()),
                               at, c->Name};
        if (notes == nullptr || capacitor(c)) continue;
        if (r.value == 0)
            *notes << Issue{Severity::Warning,
                            resistor(c) ? tr("%1 is 0 Ohm, a short (ngspice takes it as 1e-12 Ohm): a jumper, or a value still to "
                                             "be set?").arg(c->Name)
                                        : tr("%1 is 0 H, a short: a jumper, or a value still to be set?").arg(c->Name),
                            at, c->Name};
        else if (r.value < 0)
            *notes << Issue{Severity::Warning, tr("%1 is %2: a negative value - meant?").arg(c->Name, text.trimmed()), at, c->Name};
    }
}

// An AC analysis with nothing to drive it: every voltage and current of
// it is 0. Not told when a part the check cannot see into may hold the
// source. A Vac or Iac of AC magnitude 0 drives nothing either: its
// magnitude is its amplitude U (I) unless ACmag says - U = 0 to silence a
// transient silences the AC analysis too.
void acIssues(Schematic* doc, QList<Issue>& out)
{
    static const QStringList acSources{QStringLiteral("Vac"), QStringLiteral("Iac"), QStringLiteral("Pac"),
                                       QStringLiteral("Vac_SPICE"), QStringLiteral("AM_Mod"), QStringLiteral("PM_Mod")};
    static const QRegularExpression acWord(QStringLiteral("\\bac\\b"), QRegularExpression::CaseInsensitiveOption);
    const Component* analysis = nullptr;
    QStringList silent;   // Vac and Iac of AC magnitude 0
    for (const Component* c : doc->a_DocComps) {
        if (!inCircuit(c)) continue;
        if (c->Model == QLatin1String(".AC") && analysis == nullptr) analysis = c;
        if ((c->Model == QLatin1String("Vac") || c->Model == QLatin1String("Iac")) && !c->Props.isEmpty()) {
            const Property* acmag = nullptr;
            for (const Property* p : c->Props)
                if (p->Name == QLatin1String("ACmag")) acmag = p;
            const bool own = acmag != nullptr && !acmag->Value.trimmed().isEmpty();
            const qucs_s::units::Reading r = qucs_s::units::read(own ? acmag->Value : c->Props.first()->Value);
            if (r.kind == qucs_s::units::Reading::Number && r.value == 0) {
                silent << (own ? tr("%1 (its ACmag is %2)").arg(c->Name, acmag->Value.trimmed())
                               : tr("%1 (its %2 is %3, which with no ACmag is its AC magnitude too)")
                                     .arg(c->Name, c->Props.first()->Name, c->Props.first()->Value.trimmed()));
                continue;
            }
        }
        // (Vac_SPICE is the user's text too: told by it, as S4Q_V is.)
        if (opaque(c) || (acSources.contains(c->Model) && c->Model != QLatin1String("Vac_SPICE"))) return;
        // A SPICE source of the user's text: "DC 0 AC 1".
        if ((c->Model == QLatin1String("S4Q_V") || c->Model == QLatin1String("S4Q_I") || c->Model == QLatin1String("Vac_SPICE"))
            && std::any_of(c->Props.cbegin(), c->Props.cend(), [](const Property* p) { return acWord.match(p->Value).hasMatch(); }))
            return;
    }
    if (analysis == nullptr) return;
    if (!silent.isEmpty())
        out << Issue{Severity::Warning,
                     tr("%1's only AC source%2 %3 of AC magnitude 0, so every voltage and current of it is 0: an "
                        "ACmag (1 for a transfer function) sets the AC magnitude apart from the transient's amplitude")
                         .arg(analysis->Name, silent.size() == 1 ? tr(" is") : tr("s are"), silent.join(tr(" and "))),
                     QPoint(analysis->cx, analysis->cy), analysis->Name};
    else
        out << Issue{Severity::Warning,
                     tr("%1 has nothing to drive it: no Vac, Iac, Pac or SPICE source with an AC value, so every voltage "
                        "and current of it is 0")
                         .arg(analysis->Name),
                     QPoint(analysis->cx, analysis->cy), analysis->Name};
}

// Nodes named in a NutmegEq, v(out), that no net has: the equation reads
// nothing. For a SPICE simulator, which reads the names without case; a
// probe's name and an equation's variable count as names (v(pr1) reads a
// probe, v(out) a vector out=pos-neg). Not told when a text of the user's
// may make the node. (A diagram's traces are not looked at: v(s_1_1),
// v(nf), a sensitivity's names are the analysis's own.)
void nameIssues(Schematic* doc, QList<Issue>& out)
{
    static const QStringList texts{QStringLiteral("SpiceInclude"), QStringLiteral("INCLSCR"), QStringLiteral(".CUSTOMSIM"),
                                   QStringLiteral(".XYCESCR")};
    QSet<QString> known{QStringLiteral("0"), QStringLiteral("gnd")};
    for (const Component* c : doc->a_DocComps) {
        if (inCircuit(c) && texts.contains(c->Model)) return;
        if (isPort(c) || c->isProbe) known.insert(c->Name.toLower());
        if (c->isEquation)   // (a NutmegEq's "out=pos-neg" is the property out)
            for (const Property* p : c->Props) known.insert(p->Name.toLower());
    }
    for (const Node* n : doc->a_DocNodes)
        if (n->hasLabel()) known.insert(n->label()->Name.toLower());
    for (const Wire* w : doc->a_DocWires)
        if (w->hasLabel()) known.insert(w->label()->Name.toLower());
    static const QRegularExpression ref(QStringLiteral("(?<![A-Za-z0-9_])v\\s*\\(\\s*([^,()\\s]+)\\s*(?:,\\s*([^,()\\s]+)\\s*)?\\)"),
                                        QRegularExpression::CaseInsensitiveOption);
    // The names \a text reads that are no node.
    const auto unknown = [&](const QString& text) {
        QStringList names;
        for (auto it = ref.globalMatch(text); it.hasNext();) {
            const QRegularExpressionMatch m = it.next();
            for (int g : {1, 2}) {
                const QString name = m.captured(g);
                // A node inside a subcircuit (x1.out), a generated one, a device's.
                if (name.isEmpty() || name.contains(QLatin1Char('.')) || name.contains(QLatin1Char(':'))
                    || name.contains(QLatin1Char('#')) || name.startsWith(QLatin1String("_net"), Qt::CaseInsensitive))
                    continue;
                if (!known.contains(name.toLower()) && !names.contains(name)) names << name;
            }
        }
        return names;
    };
    for (const Component* c : doc->a_DocComps) {
        if (!inCircuit(c) || c->Model != QLatin1String("NutmegEq")) continue;
        QStringList names;
        for (int i = 1; i < c->Props.size(); ++i)
            for (const QString& name : unknown(c->Props.at(i)->Value))
                if (!names.contains(name)) names << name;
        if (!names.isEmpty())
            out << Issue{Severity::Warning,
                         tr("%1 reads v(%2), but no net is labelled %3: the equation reads nothing")
                             .arg(c->Name, names.first(), listed(names)),
                         QPoint(c->cx, c->cy), c->Name};
    }
}

// An equation's variable named as a net (Tj1 = v(tj1) + 27 beside the
// net tj1), a probe, or the analysis's axis (time, frequency). Under a
// SPICE simulator the equation is a 'let' after the run, and a vector of
// that name is the node's: it writes over the node's voltage - v(tj1)
// then reads the equation's result - and its variable never appears. An
// Eqn that reads no voltage or current is a .param, apart from the nodes.
void shadowIssues(Schematic* doc, QList<Issue>& out)
{
    QHash<QString, QString> nets;   // lower case: as written
    for (const Node* n : doc->a_DocNodes)
        if (n->hasLabel()) nets.insert(n->label()->Name.toLower(), n->label()->Name);
    for (const Wire* w : doc->a_DocWires)
        if (w->hasLabel()) nets.insert(w->label()->Name.toLower(), w->label()->Name);
    QHash<QString, QString> probes;
    for (const Component* c : doc->a_DocComps)
        if (inCircuit(c) && c->isProbe) probes.insert(c->Name.toLower(), c->Name);
    static const QStringList axes{QStringLiteral("time"), QStringLiteral("frequency")};
    static const QRegularExpression simulated(QStringLiteral("(?<![A-Za-z0-9_])[vi]\\s*\\("), QRegularExpression::CaseInsensitiveOption);
    for (const Component* c : doc->a_DocComps) {
        const bool nutmeg = c->Model == QLatin1String("NutmegEq");
        if (!inCircuit(c) || !(nutmeg || c->Model == QLatin1String("Eqn"))) continue;
        for (int i = nutmeg ? 1 : 0; i < c->Props.size(); ++i) {
            const Property* p = c->Props.at(i);
            if (!nutmeg && p->Name == QLatin1String("Export")) continue;
            if (!nutmeg && !simulated.match(p->Value).hasMatch()) continue;
            const QString name = p->Name.toLower();
            QString clash;
            if (nets.contains(name))
                clash = tr("the net %1: under ngspice the equation writes over the node's voltage (v(%1) reads its result "
                           "instead)").arg(nets.value(name));
            else if (probes.contains(name))
                clash = tr("the probe %1: under ngspice the equation writes over the probe's vector").arg(probes.value(name));
            else if (axes.contains(name))
                clash = tr("the analysis's axis %1: under ngspice the equation writes over it").arg(name);
            if (clash.isEmpty()) continue;
            out << Issue{Severity::Warning,
                         tr("%1: its variable %2 is named as %3, and %2 is not in the dataset - name it otherwise (%2_eq)")
                             .arg(c->Name, p->Name, clash),
                         QPoint(c->cx, c->cy), c->Name};
        }
    }
}

// Notes. A net with two names (labels in and vin): the netlist keeps one,
// and a plot or an equation of the other finds nothing.
void twoNamesNotes(Schematic* doc, const Nets& nets, QList<Issue>& out)
{
    const bool caseless = namesWithoutCase(doc);
    std::map<int, QStringList> names;
    QHash<int, QPoint> at;
    QStringList groundLabels;
    QPoint groundAt;
    const auto note = [&](const QString& name, const Node* n, QPoint where) {
        const int net = nets.of.value(n, -1);
        if (net < 0) return;
        if (net == nets.ground) {
            // A label on ground: the netlist calls the net 0.
            if (!groundName(doc, name) && !groundLabels.contains(name)) {
                if (groundLabels.isEmpty()) groundAt = where;
                groundLabels << name;
            }
            return;
        }
        QStringList& list = names[net];
        const auto same = [&](const QString& other) {
            return other.compare(name, caseless ? Qt::CaseInsensitive : Qt::CaseSensitive) == 0;
        };
        if (std::none_of(list.cbegin(), list.cend(), same)) list << name;
        if (!at.contains(net)) at.insert(net, where);
    };
    for (const Node* n : doc->a_DocNodes)
        if (n->hasLabel()) note(n->label()->Name, n, n->center());
    for (const Wire* w : doc->a_DocWires)
        if (w->hasLabel()) note(w->label()->Name, w->Port1, QPoint(w->x1, w->y1));
    for (auto& [net, list] : names) {
        if (list.size() < 2) continue;
        list.sort();   // (the netlister keeps the first, Schematic::unifyNamedNets)
        out << Issue{Severity::Warning,
                     tr("one net has %1 names, %2: the netlist calls it %3, and a plot or an equation of %4 finds "
                        "nothing - keep one label")
                         .arg(list.size())
                         .arg(listed(list), list.first(), listed(list.mid(1))),
                     at.value(net), QString()};
    }
    if (!groundLabels.isEmpty()) {
        groundLabels.sort();
        out << Issue{Severity::Warning,
                     tr("the label%1 %2 %3 on ground (a ground symbol is on the net): the netlist calls it 0, and a plot "
                        "or an equation of %2 finds nothing - take the label off")
                         .arg(groundLabels.size() > 1 ? QStringLiteral("s") : QString(), listed(groundLabels),
                              groundLabels.size() > 1 ? tr("are") : tr("is")),
                     groundAt, QString()};
    }
}

// A source's edges, as far as its values tell them: the step of its
// fastest edge and that edge's time (0: a step with no rise time given).
struct Edges {
    enum { None, Unknown, Known } kind = Unknown;   // None: no edges (a flat pulse)
    double step = 0, time = 0;
    double shortest = 0;   // the shortest edge that takes time (0: none) - a pulse's 1 ns fall, its rise 0
};

// The steepest segment of a PWL list, "0 0 10n 5 (...)": its pairs of time
// and value, up to the first word that is no number (r=, td=).
Edges pwlEdges(QString text)
{
    text.replace(QLatin1Char('('), QLatin1Char(' ')).replace(QLatin1Char(')'), QLatin1Char(' ')).replace(QLatin1Char(','), QLatin1Char(' '));
    QList<double> numbers;
    for (const QString& word : text.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        bool ok = false;
        const double n = qucs_s::units::spiceNumber(word, &ok);
        if (!ok || word.contains(QLatin1Char('='))) break;
        numbers << n;
    }
    Edges e;
    e.kind = Edges::None;
    double steepest = 0, shortest = 0;
    for (int i = 2; i + 1 < numbers.size(); i += 2) {
        const double dv = std::abs(numbers.at(i + 1) - numbers.at(i - 1)), dt = numbers.at(i) - numbers.at(i - 2);
        if (dv == 0 || dt < 0) continue;
        if (dt > 0 && (shortest == 0 || dt < shortest)) shortest = dt;
        const double slope = dt == 0 ? std::numeric_limits<double>::infinity() : dv / dt;
        if (e.kind == Edges::None || slope > steepest) {
            e = Edges{Edges::Known, dv, dt};
            steepest = slope;
        }
    }
    e.shortest = shortest;
    return e;
}

Edges edgesOf(const Component* v)
{
    // Qucs's sources: their values by name, each a number or nothing known.
    const auto value = [v](const char* name, double* out) {
        for (const Property* p : v->Props)
            if (p->Name == QLatin1String(name)) {
                const qucs_s::units::Reading r = qucs_s::units::read(p->Value);
                *out = r.value;
                return r.kind == qucs_s::units::Reading::Number && std::isfinite(r.value);
            }
        return false;
    };
    const auto edge = [](double step, double rise, double fall) {
        if (step == 0) return Edges{Edges::None, 0, 0};
        if (rise < 0 || fall < 0) return Edges{};
        const double shortest = rise > 0 && fall > 0 ? std::min(rise, fall) : std::max(rise, fall);
        return Edges{Edges::Known, step, std::min(rise, fall), shortest};
    };
    double u1, u2, rise, fall;
    if (v->Model == QLatin1String("Vpulse"))
        return value("U1", &u1) && value("U2", &u2) && value("Tr", &rise) && value("Tf", &fall) ? edge(std::abs(u2 - u1), rise, fall)
                                                                                                 : Edges{};
    if (v->Model == QLatin1String("Vrect"))
        return value("U", &u2) && value("U0", &u1) && value("Tr", &rise) && value("Tf", &fall) ? edge(std::abs(u2 - u1), rise, fall)
                                                                                                : Edges{};
    if (v->Model == QLatin1String("vPRBS")) {
        // Its edges left empty are ngspice's default, the time step.
        const auto time = [&](const char* name, double* out) {
            *out = 0;
            for (const Property* p : v->Props)
                if (p->Name == QLatin1String(name) && p->Value.trimmed().isEmpty()) return true;
            return value(name, out);
        };
        return value("U1", &u1) && value("U2", &u2) && time("Tr", &rise) && time("Tf", &fall) ? edge(std::abs(u2 - u1), rise, fall)
                                                                                               : Edges{};
    }
    // The SPICE ones: their text, continuation lines and all.
    QStringList lines;
    for (const Property* p : v->Props) lines << p->Value;
    const QString text = lines.join(QLatin1Char(' '));
    if (v->Model == QLatin1String("vPWL")) return pwlEdges(text);
    static const QRegularExpression pulse(QStringLiteral("\\bpulse\\s*\\(?([^)]*)"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression pwl(QStringLiteral("\\bpwl\\s*\\(?([^)]*)"), QRegularExpression::CaseInsensitiveOption);
    if (const QRegularExpressionMatch m = pwl.match(text); m.hasMatch()) return pwlEdges(m.captured(1));
    if (const QRegularExpressionMatch m = pulse.match(text); m.hasMatch()) {
        // PULSE(V1 V2 TD TR TF PW PER): TR and TF left out are the time step.
        const QStringList words = m.captured(1).simplified().replace(QLatin1Char(','), QLatin1Char(' ')).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        QList<double> n;
        for (const QString& word : words) {
            bool ok = false;
            const double x = qucs_s::units::spiceNumber(word, &ok);
            if (!ok) break;
            n << x;
        }
        if (n.size() < 2) return Edges{};
        return edge(std::abs(n.at(1) - n.at(0)), n.value(3, 0), n.value(4, 0));
    }
    return Edges{Edges::None, 0, 0};
}

// The sources edgesOf() reads.
const QStringList& edgy()
{
    static const QStringList models{QStringLiteral("Vpulse"), QStringLiteral("Vrect"), QStringLiteral("vPWL"), QStringLiteral("S4Q_V"),
                                    QStringLiteral("vPRBS")};
    return models;
}

// A capacitor straight across a source with edges (a pulse, a PWL, and the
// SPICE source's PULSE and PWL): nothing limits the current at the edges -
// C dV/dt, worked out from the values when they are numbers.
void edgeNotes(Schematic* doc, const Nets& nets, QList<Issue>& out)
{
    for (const Component* v : doc->a_DocComps) {
        if (!inCircuit(v) || !voltageSource(v) || !edgy().contains(v->Model)) continue;
        const int a = netOfPin(nets, v, 0), b = netOfPin(nets, v, 1);
        if (a < 0 || b < 0 || a == b) continue;
        const Edges edges = edgesOf(v);
        if (edges.kind == Edges::None) continue;
        for (const Component* c : doc->a_DocComps) {
            if (!inCircuit(c) || !capacitor(c)) continue;
            const int x = netOfPin(nets, c, 0), y = netOfPin(nets, c, 1);
            if (!((x == a && y == b) || (x == b && y == a))) continue;
            QString text;
            const qucs_s::units::Reading r = valueOf(c, &text);
            const bool numbers = edges.kind == Edges::Known && r.kind == qucs_s::units::Reading::Number && r.value > 0
                                 && std::isfinite(r.value);
            const double current = numbers && edges.time > 0 ? r.value * edges.step / edges.time : 0;
            QString how;
            if (numbers && edges.time == 0)
                how = tr(" - its steps of %1 have no rise time, so only the simulator's time step limits C dV/dt")
                          .arg(qucs_s::units::engineering(edges.step, QStringLiteral("V")));
            else if (numbers && std::isfinite(current))
                how = tr(" - C dV/dt is %1 here (%2, a step of %3 in %4)")
                          .arg(qucs_s::units::engineering(current, QStringLiteral("A")),
                               qucs_s::units::engineering(r.value, QStringLiteral("F")),
                               qucs_s::units::engineering(edges.step, QStringLiteral("V")),
                               qucs_s::units::engineering(edges.time, QStringLiteral("s")));
            else
                how = tr(" (C dV/dt)");
            out << Issue{Severity::Warning,
                         tr("%1 is straight across %2: nothing limits its current at %2's edges%3; a resistor in series "
                            "stands for the source's own")
                             .arg(c->Name, v->Name, how),
                         QPoint(c->cx, c->cy), c->Name};
        }
    }
}

// A transient whose step is long against the sources' fastest edge.
// ngspice's steps are at most (Stop - Start)/(Points - 1) - or (Stop -
// Start)/50 when that is less - and MaxStep when it is set; an edge crossed
// in fewer than five of them comes out coarse: a chain of inverters driven
// by 0.5 ns edges, run in 0.5 ns steps, had its delay 12% too long. Not
// when steps that short would be more than a million over the run: such an
// edge is a step to it (a relay switched in 1 ns, run for 20 ms - and
// ngspice breaks at a pulse's corners).
void stepNotes(Schematic* doc, QList<Issue>& out)
{
    if (QucsSettings.DefaultSimulator != spicecompat::simNgspice) return;
    const Component* fastest = nullptr;
    double edge = std::numeric_limits<double>::infinity();
    for (const Component* v : doc->a_DocComps) {
        if (!inCircuit(v) || !voltageSource(v) || !edgy().contains(v->Model)) continue;
        // Edges of no rise time are the step itself: nothing to compare -
        // but its other edges (a pulse's fall of 1 ns, its rise 0).
        if (const Edges e = edgesOf(v); e.kind == Edges::Known && e.shortest > 0 && e.shortest < edge) {
            edge = e.shortest;
            fastest = v;
        }
    }
    if (fastest == nullptr) return;
    const auto number = [](const Component* c, const char* name, double* out) {
        for (const Property* p : c->Props)
            if (p->Name == QLatin1String(name)) {
                const qucs_s::units::Reading r = qucs_s::units::read(p->Value);
                *out = r.value;
                return r.kind == qucs_s::units::Reading::Number && std::isfinite(r.value);
            }
        return false;
    };
    const auto shown = [](double t) { return qucs_s::units::engineering(t, QStringLiteral("s")); };
    // Five steps an edge, rounding aside: 0.1 ns times 5 is a little over 0.5 ns.
    for (const Component* t : doc->a_DocComps) {
        if (!inCircuit(t) || t->Model != QLatin1String(".TR") || t->Props.isEmpty() || t->Props.at(0)->Value != QLatin1String("lin"))
            continue;
        double start, stop, points, most = 0;
        if (!number(t, "Start", &start) || !number(t, "Stop", &stop) || !number(t, "Points", &points) || points < 2 || stop <= start)
            continue;
        if ((stop - start) / (edge / 5) > 1e6) continue;
        const double step = (stop - start) / (points - 1);
        QString why, cure;
        if (number(t, "MaxStep", &most) && most > 0) {
            if (most * 5 <= edge * (1 + 1e-9)) continue;
            why = tr("its MaxStep is %1").arg(shown(most));
            // (With MaxStep, ngspice's step is at most it alone: more
            // Points do not shorten it.)
            cure = tr("A MaxStep of %1 or less resolves them").arg(shown(edge / 5));
        } else {
            const double longest = std::min(step, (stop - start) / 50);
            if (longest * 5 <= edge * (1 + 1e-9)) continue;
            why = tr("its step is %1 (%2 to %3 in %4 points)").arg(shown(longest), shown(start), shown(stop)).arg(points);
            cure = tr("A MaxStep of %1 or less (or more Points) resolves them").arg(shown(edge / 5));
        }
        out << Issue{Severity::Warning,
                     tr("%1's time step is long for %2's edges of %3: %4, and an edge crossed in so few steps comes out "
                        "coarse - delays and rise times off by 10% or more. %5")
                         .arg(t->Name, fastest->Name, shown(edge), why, cure),
                     QPoint(t->cx, t->cy), t->Name};
    }
}

// A transient's integration method other than the trapezoidal rule is
// Qucsator's: ngspice integrates by its own (the trapezoidal rule) unless
// a .OPTIONS part says method=gear - the sawtooth example asked for Gear,
// ran trapezoidal, and stopped ("Timestep too small").
void methodNotes(Schematic* doc, QList<Issue>& out)
{
    if (QucsSettings.DefaultSimulator != spicecompat::simNgspice) return;
    const auto value = [](const Component* c, const char* name) {
        for (const Property* p : c->Props)
            if (p->Name == QLatin1String(name)) return p->Value.trimmed();
        return QString();
    };
    bool optioned = false;
    for (const Component* c : doc->a_DocComps)
        if (inCircuit(c) && c->Model == QLatin1String("SpiceOptions"))
            for (const Property* p : c->Props)
                if (p->Value.simplified().remove(QLatin1Char(' ')).startsWith(QLatin1String("method="), Qt::CaseInsensitive)
                    || p->Name.compare(QLatin1String("method"), Qt::CaseInsensitive) == 0)
                    optioned = true;
    if (optioned) return;
    for (const Component* t : doc->a_DocComps) {
        if (!inCircuit(t) || t->Model != QLatin1String(".TR")) continue;
        const QString method = value(t, "IntegrationMethod");
        if (method.isEmpty() || method == QLatin1String("Trapezoidal")) continue;
        out << Issue{Severity::Warning,
                     tr("%1's integration method %2 is Qucsator's: ngspice integrates by the trapezoidal rule. A "
                        ".OPTIONS part with method=gear (its only other) makes it Gear")
                         .arg(t->Name, method),
                     QPoint(t->cx, t->cy), t->Name};
    }
}

// A transistor's base or gate, an op-amp's input, with no DC path but
// through its own part: no bias - the transistor sits off, the gate's
// level is undefined, the input's bias current has nowhere to go. Driven
// through a capacitor only, as an AC-coupled stage that forgot its bias.
// (A net reached by nothing at all is told as floating.)
void biasNotes(Schematic* doc, const Nets& nets, QList<Issue>& out)
{
    static const QStringList firstPin{QStringLiteral("BJT"), QStringLiteral("_BJT"), QStringLiteral("JFET"),
                                      QStringLiteral("MOSFET"), QStringLiteral("_MOSFET")};
    static const QStringList secondPin{QStringLiteral("NPN_SPICE"), QStringLiteral("PNP_SPICE"), QStringLiteral("NJF_SPICE"),
                                       QStringLiteral("PJF_SPICE"), QStringLiteral("NMOS_SPICE"), QStringLiteral("PMOS_SPICE")};
    enum class Kind { Base, Gate, Input };
    struct Control {
        const Component* part;
        int pin;
        Kind kind;
        QString input;   // an op-amp's input: its pin's name, or - and + (the ideal one's)
    };
    QList<Control> controls;
    QSet<const Component*> controlled;
    for (const Component* c : doc->a_DocComps) {
        if (!inCircuit(c)) continue;
        const Kind own = c->SpiceModel == QLatin1String("Q") ? Kind::Base : Kind::Gate;
        const qsizetype before = controls.size();
        if (firstPin.contains(c->Model)) controls << Control{c, 0, own, QString()};
        else if (secondPin.contains(c->Model)) controls << Control{c, 1, own, QString()};
        else if (c->Model == QLatin1String("OpAmp"))
            controls << Control{c, 0, Kind::Input, QStringLiteral("-")} << Control{c, 1, Kind::Input, QStringLiteral("+")};
        else
            for (int i = 0; i < c->Ports.size(); ++i)
                if (inputName(c->Ports.at(i)->Name)) controls << Control{c, i, Kind::Input, c->Ports.at(i)->Name};
        if (controls.size() > before) controlled.insert(c);
    }
    if (controls.isEmpty() || controlled.size() > 300) return;   // (a drawing of thousands: not worth the time)

    // The nets joined by the parts that are not controlled, at DC (not by
    // capacitors; a current source biases) and at all.
    QHash<int, int> dcUp, anyUp;
    const auto find = [](const QHash<int, int>& up, int i) {
        while (up.value(i, i) != i) i = up.value(i, i);
        return i;
    };
    const auto join = [&find](QHash<int, int>& up, const QList<int>& on) {
        for (int k = 1; k < on.size(); ++k) {
            const int a = find(up, on.at(k)), b = find(up, on.at(0));
            if (a != b) up.insert(a, b);
        }
    };
    const auto pinsOn = [&nets](const Component* c) {
        QList<int> on;
        for (const Port* p : c->Ports)
            if (p->Connection != nullptr && nets.of.contains(p->Connection)) on << nets.of.value(p->Connection);
        return on;
    };
    const auto joins = [](const Component* c) {
        return (inCircuit(c) || c->isActive == COMP_IS_SHORTEN) && !isSimulation(c) && !c->isEquation && !isGround(c)
               && !isPort(c) && !(c->isProbe && c->Model != QLatin1String("IProbe"));
    };
    QSet<int> reference;
    if (nets.ground >= 0) reference.insert(nets.ground);
    for (const Component* c : doc->a_DocComps) {
        if (isPort(c) && inCircuit(c)) {
            const int net = netOfPin(nets, c, 0);
            if (net >= 0) reference.insert(net);
        }
        if (!joins(c) || controlled.contains(c)) continue;
        const QList<int> on = pinsOn(c);
        join(anyUp, on);
        if (c->SpiceModel != QLatin1String("C")) join(dcUp, on);
    }
    if (reference.isEmpty()) return;
    for (const Control& k : std::as_const(controls)) {
        const Port* pin = k.part->Ports.value(k.pin);
        // (A pin on nothing is told as such; one with a label is on its net.)
        if (pin == nullptr || pin->Connection == nullptr || (pin->Connection->conn_count() <= 1 && !named(pin->Connection)))
            continue;
        const int net = nets.of.value(pin->Connection, -1);
        if (net < 0) continue;
        // An op-amp's output (and its supplies) drive: feedback to the input
        // is a DC path. Its other inputs do not.
        QSet<int> drivers = reference;
        if (k.kind == Kind::Input)
            for (int i = 0; i < k.part->Ports.size(); ++i) {
                const bool anInput = std::any_of(controls.cbegin(), controls.cend(), [&](const Control& o) {
                    return o.part == k.part && o.pin == i;
                });
                const int other = netOfPin(nets, k.part, i);
                if (!anInput && other >= 0) drivers.insert(other);
            }
        // The other controlled parts join their pins as the rest do.
        QHash<int, int> dc = dcUp, any = anyUp;
        for (const Component* c : std::as_const(controlled))
            if (c != k.part) {
                join(any, pinsOn(c));
                join(dc, pinsOn(c));
            }
        const auto reaches = [&](const QHash<int, int>& up) {
            return std::any_of(drivers.cbegin(), drivers.cend(), [&](int r) { return find(up, r) == find(up, net); });
        };
        if (reaches(dc) || !reaches(any)) continue;
        QString what;
        switch (k.kind) {
        case Kind::Base:
            what = tr("%1: its base has no DC path but through %1 itself - no bias current reaches it, so %1 sits off "
                      "(a resistor from a supply, or a divider, biases it)").arg(k.part->Name);
            break;
        case Kind::Gate:
            what = tr("%1: its gate has no DC path but through %1 itself - its DC level is undefined (a resistor to "
                      "ground or to a divider sets it)").arg(k.part->Name);
            break;
        case Kind::Input:
            what = tr("%1: its input %2 has no DC path but through %1 itself - its bias current has nowhere to go (a "
                      "resistor to ground gives it a path)").arg(k.part->Name, k.input);
            break;
        }
        out << Issue{Severity::Warning, what, QPoint(k.part->cx, k.part->cy), k.part->Name};
    }
}

// A library part that its library calls a power amplifier (LM3886: "68W
// audio amplifier"): made to drive a speaker.
bool powerAmplifier(const Component* u)
{
    static const QRegularExpression power(QStringLiteral("power\\s+(op-?\\s*)?amp|audio\\s+(power\\s+)?amp|\\d\\s*W\\b"),
                                          QRegularExpression::CaseInsensitiveOption);
    auto* part = dynamic_cast<LibComp*>(const_cast<Component*>(u));
    return part != nullptr && power.match(part->description()).hasMatch();
}

// A load smaller than an op-amp drives: a resistor of less than 1 kOhm
// from the output of a part with supply pins (a real op-amp's model) to
// ground. Not a power amplifier's.
void loadNotes(Schematic* doc, const Nets& nets, QList<Issue>& out)
{
    if (nets.ground < 0) return;
    for (const Component* u : doc->a_DocComps) {
        if (!inCircuit(u)) continue;
        QSet<int> outNets;   // (a dual's OUTA and OUTB)
        bool powered = false;
        for (int i = 0; i < u->Ports.size(); ++i) {
            const int net = netOfPin(nets, u, i);
            if (outputName(u->Ports.at(i)->Name) && net >= 0 && net != nets.ground) outNets.insert(net);
            if (supplyName(u->Ports.at(i)->Name)) powered = true;
        }
        if (outNets.isEmpty() || !powered) continue;
        bool amplifierKnown = false, amplifier = false;   // (its library read once, when a load is small)
        for (const Component* r : doc->a_DocComps) {
            if (!inCircuit(r) || !resistor(r)) continue;
            const int a = netOfPin(nets, r, 0), b = netOfPin(nets, r, 1);
            if (!((outNets.contains(a) && b == nets.ground) || (outNets.contains(b) && a == nets.ground))) continue;
            QString text;
            const qucs_s::units::Reading v = valueOf(r, &text);
            if (v.kind != qucs_s::units::Reading::Number || v.value <= 0 || v.value >= 1000) continue;
            if (!amplifierKnown) {
                amplifier = powerAmplifier(u);
                amplifierKnown = true;
            }
            if (amplifier) break;
            out << Issue{Severity::Warning,
                         tr("%1 (%2) loads %3's output to ground: op-amps are specified into 2 kOhm or so, and many limit "
                            "their current below 1 kOhm (a 741 near 25 mA)")
                             .arg(r->Name, text.trimmed(), u->Name),
                         QPoint(r->cx, r->cy), r->Name};
        }
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
    polarityIssues(doc, nets, nullptr, &out);
    if (!doc->isDigitalCircuit()) {
        valueIssues(doc, nullptr, &out);
        twoNamesNotes(doc, nets, out);
        edgeNotes(doc, nets, out);
        stepNotes(doc, out);
        methodNotes(doc, out);
        biasNotes(doc, nets, out);
        loadNotes(doc, nets, out);
    }
    for (Issue& i : out) i.file = doc->getDocName();
    return out;
}

namespace {

// A command as it is shown: its first line, and how many more.
QString commandShown(const QStringList& lines)
{
    QString first = lines.first();
    if (first.size() > 120) first = first.left(117) + QStringLiteral("...");
    return lines.size() == 1 ? first : tr("%1 (and %2 more lines)").arg(first).arg(lines.size() - 1);
}

// What a simulation of \a doc runs besides the simulator (commandsRun()),
// each with the part that holds it (none for the Octave script).
QList<std::pair<QString, const Component*>> commandsOf(Schematic* doc)
{
    QList<std::pair<QString, const Component*>> list;
    // ngspice's commands that start a program: shell, system, and !.
    static const QRegularExpression runs(QStringLiteral("^\\s*(shell|system)(\\s|$)|^\\s*!"), QRegularExpression::CaseInsensitiveOption);
    for (const Component* c : doc->a_DocComps) {
        if (!inCircuit(c)) continue;
        if (c->Model == QLatin1String("CMD")) {
            // As runPostSimCommands runs it: its lines but blank ones and
            // comments (#).
            QStringList lines;
            if (!c->Props.isEmpty())
                for (const QString& l : c->Props.at(0)->Value.split(QLatin1Char('\n')))
                    if (const QString t = l.trimmed(); !t.isEmpty() && !t.startsWith(QLatin1Char('#'))) lines << t;
            if (!lines.isEmpty())
                list.append({tr("%1 runs a command in a shell after each simulation: %2").arg(c->Name, commandShown(lines)), c});
        } else if (c->Model == QLatin1String(".CUSTOMSIM") || c->Model == QLatin1String("NutmegEq") || c->Model == QLatin1String("SPICEINIT")) {
            QStringList lines;
            for (const Property* p : c->Props)
                for (const QString& l : p->Value.split(QLatin1Char('\n')))
                    if (runs.match(l).hasMatch()) lines << l.trimmed();
            if (!lines.isEmpty())
                list.append({tr("%1's ngspice text runs a command in a shell: %2").arg(c->Name, commandShown(lines)), c});
        }
    }
    if (doc->getSimRunScript() && !doc->getScript().trimmed().isEmpty())
        list.append({tr("Octave runs the script %1 after each simulation (Document Settings: run script after simulation)")
                         .arg(doc->getScript().trimmed()),
                     nullptr});
    return list;
}

} // namespace

QStringList commandsRun(Schematic* doc)
{
    QStringList said;
    if (doc != nullptr)
        for (const auto& [what, part] : commandsOf(doc)) said << what;
    return said;
}

namespace {

// The warnings of commandsRun(), at their parts.
void commandIssues(Schematic* doc, QList<Issue>& out)
{
    for (const auto& [what, part] : commandsOf(doc)) {
        const QPoint where = part != nullptr ? QPoint(part->cx, part->cy)
                                             : doc->a_DocComps.empty() ? QPoint() : QPoint((*doc->a_DocComps.begin())->cx, (*doc->a_DocComps.begin())->cy);
        out << Issue{Severity::Warning, what, where, part != nullptr ? part->Name : QString()};
    }
}

} // namespace

namespace {

// A V(PRBS)'s values that ngspice refuses (errors: "prbs order 40 is not a
// register length between 2 and 31"), and those it takes without a word
// though they are not what was meant (warnings). A parameter or an
// expression is left to the run.
void prbsIssues(const Component* c, QList<Issue>& errors, QList<Issue>& warnings)
{
    const auto text = [c](const char* name) {
        for (const Property* p : c->Props)
            if (p->Name == QLatin1String(name)) return p->Value.trimmed();
        return QString();
    };
    // A number, or nothing known (empty: ngspice's default).
    const auto number = [&](const char* name, double* out) {
        const qucs_s::units::Reading r = qucs_s::units::read(text(name));
        *out = r.value;
        return r.kind == qucs_s::units::Reading::Number && std::isfinite(r.value);
    };
    const QPoint at(c->cx, c->cy);
    const auto error = [&](const QString& what) { errors << Issue{Severity::Error, c->Name + QStringLiteral(": ") + what, at, c->Name}; };
    const auto warning = [&](const QString& what) { warnings << Issue{Severity::Warning, c->Name + QStringLiteral(": ") + what, at, c->Name}; };
    const auto shown = [](double t) { return qucs_s::units::engineering(t, QStringLiteral("s")); };
    double tbit = 0, td = 0, order = 0, seed = 0, edge = 0;
    const bool bits = number("Tbit", &tbit);
    if (bits && !(tbit > 0)) error(tr("its Tbit is %1 - a bit time is above 0 (ngspice refuses it)").arg(text("Tbit")));
    if (number("Td", &td) && td < 0) error(tr("its delay Td is %1 - a delay is 0 or more (ngspice refuses it)").arg(text("Td")));
    const bool ordered = number("Order", &order);
    const bool registerLength = ordered && order == std::floor(order) && order >= 2 && order <= 31;
    if (ordered && !registerLength)
        error(tr("its Order is %1 - the register's length, a whole number from 2 to 31 (ngspice refuses it)").arg(text("Order")));
    if (!text("Seed").isEmpty() && number("Seed", &seed)) {
        const bool whole = seed == std::floor(seed) && seed > 0 && seed < 9.007e15;
        if (!whole)
            error(tr("its Seed is %1 - the register's first contents, a whole number above 0, or empty for all ones (ngspice refuses it)")
                      .arg(text("Seed")));
        else if (registerLength && (qint64(seed) & ((qint64(1) << qint64(order)) - 1)) == 0)
            error(tr("its Seed %1 leaves the register's %2 bits all zero - their contents would never change (ngspice refuses it)")
                      .arg(text("Seed")).arg(int(order)));
    }
    for (const char* name : {"Tr", "Tf"}) {
        if (!number(name, &edge)) continue;
        if (bits && tbit > 0 && edge > tbit)
            error(tr("its %1 of %2 is longer than its Tbit of %3 - an edge is a bit at most (ngspice refuses it)")
                      .arg(QLatin1String(name), shown(edge), shown(tbit)));
        else if (edge < 0)
            warning(tr("its %1 is %2, below 0 - taken without a word, as no %3").arg(QLatin1String(name), text(name),
                                                                                     QLatin1String(name) == QLatin1String("Tr") ? tr("rise time")
                                                                                                                                : tr("fall time")));
    }
    const QString coding = text("Coding");
    if (!coding.isEmpty() && coding.compare(QLatin1String("NRZ"), Qt::CaseInsensitive) != 0
        && coding.compare(QLatin1String("PAM4"), Qt::CaseInsensitive) != 0)
        warning(tr("its Coding is %1, neither NRZ nor PAM4 - netlisted as NRZ").arg(coding));
}

// A transient's values ngspice refuses (a MaxStep below 0: "TMAX is
// invalid"), or reads otherwise than written (Points of 2001.5: a step of
// (Stop - Start)/2000.5).
void transientIssues(const Component* c, QList<Issue>& errors, QList<Issue>& warnings)
{
    const QPoint at(c->cx, c->cy);
    for (const Property* p : c->Props) {
        const qucs_s::units::Reading r = qucs_s::units::read(p->Value);
        if (r.kind != qucs_s::units::Reading::Number || !std::isfinite(r.value)) continue;
        if (p->Name == QLatin1String("MaxStep") && r.value < 0)
            errors << Issue{Severity::Error,
                            tr("%1: its MaxStep is %2 - the longest step, 0 or more (0: ngspice's own; ngspice refuses it)")
                                .arg(c->Name, p->Value.trimmed()),
                            at, c->Name};
        if (p->Name == QLatin1String("Points") && (r.value != std::floor(r.value) || r.value < 2))
            warnings << Issue{Severity::Warning,
                              tr("%1: its Points are %2 - a count of points, a whole number from 2 on (the step is (Stop - "
                                 "Start)/(Points - 1))").arg(c->Name, p->Value.trimmed()),
                              at, c->Name};
    }
}

// A parameter sweep over a DC analysis is ngspice's own dc sweep, which
// sweeps a voltage or current source, a resistor or the temperature: of
// anything else - a .param above all - "dc simulation(s) aborted", and no
// result, with nothing said before.
void dcSweepIssues(Schematic* doc, int simulator, QList<Issue>& errors)
{
    if (simulator != spicecompat::simNgspice && simulator != spicecompat::simSpiceOpus) return;
    const auto value = [](const Component* c, const char* name) {
        for (const Property* p : c->Props)
            if (p->Name == QLatin1String(name)) return p->Value.trimmed();
        return QString();
    };
    for (const Component* sw : doc->a_DocComps) {
        if (!inCircuit(sw) || sw->Model != QLatin1String(".SW")) continue;
        // (Its analysis by name, as the netlist reads it: a DC one.)
        const QString sim = value(sw, "Sim");
        const Component* analysis = doc->getComponentByName(sim);
        const bool dc = analysis != nullptr ? analysis->Model == QLatin1String(".DC") : sim.startsWith(QLatin1String("dc"), Qt::CaseInsensitive);
        if (!dc) continue;
        const QString param = value(sw, "Param");
        if (param.isEmpty() || param.compare(QLatin1String("temp"), Qt::CaseInsensitive) == 0
            || param.compare(QLatin1String("temper"), Qt::CaseInsensitive) == 0)
            continue;
        const Component* swept = doc->getComponentByName(param);
        if (swept != nullptr && (swept->SpiceModel == QLatin1String("V") || swept->SpiceModel == QLatin1String("I")
                                 || swept->SpiceModel == QLatin1String("R")))
            continue;
        const QString what = swept != nullptr ? tr("%1 (no source or resistor)").arg(param) : tr("%1 (a parameter)").arg(param);
        errors << Issue{Severity::Error,
                        tr("%1 sweeps %2 over the DC analysis %3: ngspice's dc sweeps a voltage or current source, a "
                           "resistor or the temperature - the run aborts (\"dc simulation(s) aborted\"). Sweep the source "
                           "or resistor it sets, or sweep it at the operating point with an NgSweep (Analysis op)")
                            .arg(sw->Name, what, sim),
                        QPoint(sw->cx, sw->cy), sw->Name};
    }
}

// A pulse's edge below 0: taken without a word, as no edge time (the time
// step).
void negativeEdgeIssues(const Component* c, QList<Issue>& warnings)
{
    for (const Property* p : c->Props) {
        if (p->Name != QLatin1String("Tr") && p->Name != QLatin1String("Tf")) continue;
        const qucs_s::units::Reading r = qucs_s::units::read(p->Value);
        if (r.kind == qucs_s::units::Reading::Number && std::isfinite(r.value) && r.value < 0)
            warnings << Issue{Severity::Warning,
                              tr("%1: its %2 is %3, below 0 - taken without a word, as no %4")
                                  .arg(c->Name, p->Name, p->Value.trimmed(), p->Name == QLatin1String("Tr") ? tr("rise time") : tr("fall time")),
                              QPoint(c->cx, c->cy), c->Name};
    }
}

} // namespace

QHash<const Component*, QString> refs(const Schematic* doc)
{
    const auto unnamed = [](const Component* c) { return c->Name.isEmpty() || c->Name == QLatin1String("*"); };
    QHash<QString, int> count;
    for (const Component* c : doc->a_DocComps)
        if (unnamed(c)) ++count[c->Model.toLower()];
    QHash<QString, int> nth, again;
    QHash<const Component*, QString> out;
    for (const Component* c : doc->a_DocComps) {
        if (!unnamed(c)) {
            const int k = ++again[c->Name];
            out.insert(c, k > 1 ? QStringLiteral("%1#%2").arg(c->Name).arg(k) : c->Name);
            continue;
        }
        const QString key = c->Model.toLower();
        const int k = ++nth[key];
        out.insert(c, count.value(key) > 1 ? QStringLiteral("%1#%2").arg(c->Model).arg(k) : c->Model);
    }
    return out;
}

QList<Issue> check(Schematic* doc)
{
    QList<Issue> errors, warnings;
    if (doc == nullptr) return errors;
    // (Made when an issue needs one: a pass over all the parts.)
    QHash<const Component*, QString> refsOf;
    const auto refFor = [&](const Component* c) {
        if (refsOf.isEmpty()) refsOf = refs(doc);
        return refsOf.value(c);
    };

    bool ground = false, port = false, simulation = false;
    // A circuit with a digital simulation block goes to the digital
    // (VHDL/Verilog) flow whatever the simulator setting says.
    const int simulator = doc->isDigitalCircuit() ? int(spicecompat::simNotSpecified) : QucsSettings.DefaultSimulator;
    QHash<QString, const Component*> byName;
    QHash<QString, const Component*> byNameWithoutCase;   // "r r1": SpiceModel and name, lower case
    // The libraries a library part's Lib names, with its Comp and without: each looked for once.
    QHash<QString, std::pair<QStringList, QStringList>> librariesOfPart;
    const bool caseless = spiceSimulator(simulator) && simulator != spicecompat::simNotSpecified;
    // The Verilog-A libraries and sources of the open project and those
    // beside the schematic (with no project, the only ones), read when a
    // Verilog-A component asks: ngspice gets its modules from them.
    QStringList vaLibraries, vaSources;
    bool vaListed = false;
    const bool inAProject = QucsMain != nullptr && !QucsMain->ProjName.isEmpty();
    const auto moduleInProject = [&](const QString& module) {
        if (!inAProject && doc->getDocName().isEmpty()) return true;   // nowhere to look
        if (!vaListed) {
            if (inAProject) {
                const QDir project(QucsSettings.QucsWorkDir);
                for (const QString& file : misc::projectFiles(project, {"*.osdi"}))
                    vaLibraries << project.absoluteFilePath(file);
                for (const QString& file : misc::projectFiles(project, {"*.va"}))
                    vaSources << project.absoluteFilePath(file);
            }
            if (!doc->getDocName().isEmpty()) {
                const QDir beside = QFileInfo(doc->getDocName()).absoluteDir();
                for (const QString& file : beside.entryList({"*.osdi"}, QDir::Files)) vaLibraries << beside.absoluteFilePath(file);
                for (const QString& file : beside.entryList({"*.va"}, QDir::Files)) vaSources << beside.absoluteFilePath(file);
            }
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
        if (c->Model == QLatin1String("vPRBS")) prbsIssues(c, errors, warnings);
        if (c->Model == QLatin1String(".TR")) transientIssues(c, errors, warnings);
        if (spiceSimulator(simulator)
            && (c->Model == QLatin1String("Vpulse") || c->Model == QLatin1String("Ipulse") || c->Model == QLatin1String("Vrect")
                || c->Model == QLatin1String("Irect")))
            negativeEdgeIssues(c, warnings);

        // Two components of one name: the netlist would merge them.
        if (!c->Name.isEmpty() && !isGround(c)) {
            if (const Component* first = byName.value(c->Name)) {
                Issue twice{Severity::Error,
                            tr("%1: the name is used twice (also at %2, %3)").arg(c->Name).arg(first->cx).arg(first->cy),
                            QPoint(c->cx, c->cy), c->Name};
                twice.ref = refFor(c);
                errors << twice;
            } else {
                byName.insert(c->Name, c);
                // A SPICE simulator reads names without regard to case:
                // r1 and R1 of one kind are one device there, and it stops.
                if (caseless && !c->SpiceModel.isEmpty()) {
                    const QString key = c->SpiceModel.toLower() + QLatin1Char(' ') + c->Name.toLower();
                    if (const Component* same = byNameWithoutCase.value(key))
                        errors << Issue{Severity::Error,
                                        tr("%1: the same name as %2 (at %3, %4) for %5, which reads names without regard "
                                           "to case")
                                            .arg(c->Name, same->Name)
                                            .arg(same->cx)
                                            .arg(same->cy)
                                            .arg(spicecompat::getDefaultSimulatorName(simulator)),
                                        QPoint(c->cx, c->cy), c->Name};
                    else
                        byNameWithoutCase.insert(key, c);
                }
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
        // The libraries of its name, with the part and without: which there
        // are when it is not loaded, which is used when two have it.
        if (c->Model == QLatin1String("Lib") && c->Props.size() >= 2) {
            const QString lib = c->Props.at(0)->Value, comp = c->Props.at(1)->Value;
            const QString key = lib + QLatin1Char('\n') + comp;
            if (!librariesOfPart.contains(key)) {
                QStringList with, without;
                for (const QString& library : LibComp::librariesNamed(lib, doc->getFileInfo().dir().path()))
                    (LibComp::hasComponent(library, comp) ? with : without) << library;
                librariesOfPart.insert(key, {with, without});
            }
            const auto& [with, without] = librariesOfPart.value(key);
            const auto shown = [](const QStringList& files) {
                QStringList native;
                for (const QString& f : files) native << QDir::toNativeSeparators(f);
                return native.join(QStringLiteral(", "));
            };
            const QString name = QFileInfo(lib).fileName();
            const auto* part = dynamic_cast<const LibComp*>(c);
            if (c->Ports.isEmpty()) {
                QString why;
                if (with.isEmpty() && without.isEmpty())
                    why = tr("not in the libraries of %1, the project or its user_lib, nor a folder of the library search paths")
                              .arg(QDir::toNativeSeparators(QucsSettings.LibDir));
                else if (with.isEmpty())
                    why = without.size() == 1 ? tr("the library %1 there is, %2, has no part %3").arg(name, shown(without), comp)
                                              : tr("the libraries %1 there are - %2 - have no part %3").arg(name, shown(without), comp);
                else
                    why = tr("it is in %1, which it could not be read from: a library of a later Qucs-S, or damaged")
                              .arg(shown({part != nullptr ? part->libraryFile() : with.first()}));
                errors << Issue{Severity::Error,
                                tr("%1: the library part %2 of %3 could not be loaded (%4): it has no pins, and what was wired to "
                                   "them is on nothing")
                                    .arg(c->Name, comp, lib, why),
                                QPoint(c->cx, c->cy), c->Name};
            } else if (part != nullptr && with.size() > 1
                       && !(QFileInfo(lib).isAbsolute() && QFileInfo::exists(lib + QStringLiteral(".lib")))) {
                // Named by its name, and two libraries of it have the part:
                // which is used is said (a path in Lib chooses).
                const QString used = part->libraryFile();
                QStringList others = with;
                others.removeAll(used);
                warnings << Issue{Severity::Warning,
                                  tr("%1: more than one library %2 has a part %3: %4 is used%5, not %6 - a path in the part's Lib "
                                     "names the one meant")
                                      .arg(c->Name, name, comp, QDir::toNativeSeparators(used),
                                           used == with.first() ? tr(" (found first)")
                                                                : tr(" (the project's Verilog-A of it is linked from there)"),
                                           shown(others)),
                                  QPoint(c->cx, c->cy), c->Name};
            }
        }
        // A subcircuit: its file given, found (a file, not a folder), and
        // not this schematic itself - a subcircuit that holds itself nests
        // in the netlist until ngspice gives up.
        if (c->Model == QLatin1String("Sub") && !c->Props.isEmpty()) {
            const QString file = c->getSubcircuitFile();
            if (c->Props.at(0)->Value.trimmed().isEmpty())
                errors << Issue{Severity::Error,
                                tr("%1: no subcircuit file is given: it has no pins, and what was wired to them is on nothing")
                                    .arg(c->Name),
                                QPoint(c->cx, c->cy), c->Name};
            else if (!foundFile(file))
                errors << Issue{Severity::Error,
                                tr("%1: its subcircuit %2 is not found (beside the schematic, in the project or its user_lib): "
                                   "it has no pins, and what was wired to them is on nothing").arg(c->Name, c->Props.at(0)->Value),
                                QPoint(c->cx, c->cy), c->Name};
            else if (sameFile(file, doc->getDocName()))
                errors << Issue{Severity::Error,
                                tr("%1 uses this schematic itself (%2): a subcircuit cannot hold itself - ngspice nests it until "
                                   "it gives up (\"unknown subckt\")")
                                    .arg(c->Name, QFileInfo(file).fileName()),
                                QPoint(c->cx, c->cy), c->Name};
        }
        // A SPICE library part whose library is nowhere (the netlist
        // includes a file the simulator cannot read; with the automatic
        // symbol, a box without pins), or whose library does not define
        // the subcircuit it names (a box without pins).
        if (c->Model == QLatin1String("SpLib") && c->Props.size() >= 2) {
            const QString library = c->Props.at(0)->Value.trimmed();
            const QString noPins = c->Ports.isEmpty() ? tr(": it has no pins, and what was wired to them is on nothing")
                                                      : QString();
            if (library.isEmpty())
                errors << Issue{Severity::Error, tr("%1: no SPICE library file is given%2").arg(c->Name, noPins),
                                QPoint(c->cx, c->cy), c->Name};
            else if (!QFileInfo::exists(misc::properAbsFileName(library, doc)))
                errors << Issue{Severity::Error,
                                tr("%1: its SPICE library %2 is not found (beside the schematic, in the project or its "
                                   "user_lib%4, nor in the library of Qucs-S)%3")
                                    .arg(c->Name, library, noPins,
                                         // (A .lib is looked for in the library search paths too.)
                                         library.endsWith(QLatin1String(".lib"), Qt::CaseInsensitive) && !QucsSettings.LibraryPaths.isEmpty()
                                             ? tr(", in a folder of the library search paths")
                                             : QString()),
                                QPoint(c->cx, c->cy), c->Name};
            else if (c->Ports.isEmpty())
                errors << Issue{Severity::Error,
                                tr("%1: its SPICE library %2 defines no subcircuit %3%4")
                                    .arg(c->Name, library, c->Props.at(1)->Value, noPins),
                                QPoint(c->cx, c->cy), c->Name};
        }
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
                              inAProject ? tr("%1: the Verilog-A module %2 is in no library (.osdi) or source (.va) of the project, nor "
                                              "beside the schematic").arg(c->Name, c->Model)
                                         : tr("%1: the Verilog-A module %2 is in no library (.osdi) or source (.va) beside the "
                                              "schematic - with no project open, those are the ones loaded").arg(c->Name, c->Model),
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
                Issue alone{Severity::Warning, what, where, c->Name};
                if (const QString ref = refFor(c); ref != c->Name) alone.ref = ref;
                warnings << alone;
            }
        }
    }

    dcSweepIssues(doc, simulator, errors);

    // A subcircuit port on a net without a label lends its own name to
    // that net, so the pin of the subcircuit is called after the port. A
    // label of that name on some other net takes it first, and the pin
    // reaches the netlist under a generated name instead.
    if (port) {
        // (In any case: the simulator reads names so, and the netlister
        // compares them so - Schematic::nameUnlabelledPortNets.)
        QSet<QString> labels;
        for (const Node* n : doc->a_DocNodes)
            if (n->hasLabel()) labels.insert(n->label()->Name.toLower());
        for (const Wire* w : doc->a_DocWires)
            if (w->hasLabel()) labels.insert(w->label()->Name.toLower());

        for (Component* c : doc->a_DocComps) {
            if (!isPort(c) || !inCircuit(c) || c->Ports.isEmpty()) continue;
            if (!doc->netLabelOf(c->Ports.first()->Connection).isEmpty()) continue;
            // A port named as ground's net: the pin is not made ground.
            if (c->Name.compare(QLatin1String("gnd"), Qt::CaseInsensitive) == 0) {
                warnings << Issue{Severity::Warning,
                                  tr("%1: gnd is ground's name, so this pin gets a generated name (named so, it was ground "
                                     "inside, whatever the parent wired to it)")
                                      .arg(c->Name),
                                  QPoint(c->cx, c->cy), c->Name};
                continue;
            }
            if (!labels.contains(c->Name.toLower())) continue;

            warnings << Issue{Severity::Warning,
                              tr("%1: another net is labelled %1, so this pin gets a generated name").arg(c->Name),
                              QPoint(c->cx, c->cy), c->Name};
        }
    }

    // Ports of one number: the instances' pins and the subcircuit's do not
    // match (and the netlister crashed on it).
    {
        std::map<int, QList<const Component*>> byNumber;
        for (const Component* c : doc->a_DocComps)
            if (isPort(c) && inCircuit(c) && !c->Props.isEmpty()) byNumber[c->Props.first()->Value.toInt()] << c;
        for (const auto& [number, ports] : byNumber) {
            if (ports.size() < 2) continue;
            QStringList names;
            for (const Component* c : ports) names << c->Name;
            errors << Issue{Severity::Error,
                            tr("%1 are all port %2: each port needs a number of its own, or an instance's pins and the "
                               "subcircuit's do not match")
                                .arg(listed(names))
                                .arg(number),
                            QPoint(ports.at(1)->cx, ports.at(1)->cy), ports.at(1)->Name};
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
    bool groundByName = false;   // a net named 0 or gnd, and no ground symbol
    {
        const Nets nets = netsOf(doc);
        groundByName = !ground && nets.ground >= 0;
        for (const auto& join : nets.caseJoins)
            warnings << Issue{Severity::Warning,
                              tr("the labels %1 are one net for %2, which reads names without regard to case")
                                  .arg(join.first.join(QStringLiteral(", ")),
                                       spicecompat::getDefaultSimulatorName(QucsSettings.DefaultSimulator)),
                              join.second, QString()};
        wiringIssues(doc, nets, warnings, false);
        topologyIssues(doc, nets, port, warnings);
        supplyIssues(doc, nets, warnings);
        polarityIssues(doc, nets, &warnings, nullptr);
        // What the parts do: for an analog simulation.
        if (simulator != spicecompat::simNotSpecified) {
            sourceLoops(doc, nets, errors, warnings);
            valueIssues(doc, &warnings, nullptr);
            acIssues(doc, warnings);
            if (spiceSimulator(simulator)) {
                nameIssues(doc, warnings);
                shadowIssues(doc, warnings);
            }
        }
    }

    // What a run executes besides the simulator: said, whatever else is,
    // when the settings look for it.
    if (QucsSettings.CheckCommands) commandIssues(doc, warnings);

    // A circuit (not a subcircuit: those have ports) needs a simulation,
    // and a ground symbol unless the settings leave node 0 to the user (a
    // net named 0, a SPICE part that brings it): then nothing is said of
    // it, and the simulator decides.
    if (!port && !doc->a_DocComps.empty()) {
        const Component* first = *doc->a_DocComps.begin();
        const QPoint where(first->cx, first->cy);
        if (!ground && QucsSettings.RequireGround)
            errors << Issue{Severity::Error,
                            groundByName ? tr("no ground symbol: the Simulators Settings require one (a net named 0 or gnd "
                                              "does not count)")
                                         : tr("no ground: the circuit has no reference node"),
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
        // (Only a file that is there: a name not found is no path to read,
        // and an empty one gave the schematic's folder.)
        const QString file = c->getSubcircuitFile();
        if (foundFile(file) && !files.contains(file)) files << file;
    }
    return files;
}

QString pinRole(const QString& name)
{
    return supplyName(name)                           ? QStringLiteral("supply")
           : inputName(name) || plainInputName(name) ? QStringLiteral("input")
           : outputName(name)                         ? QStringLiteral("output")
                                                      : QString();
}

QList<SubcircuitFindings> checkSubcircuits(Schematic* doc, const std::function<Schematic*(const QString&)>& open)
{
    QList<SubcircuitFindings> found;
    if (doc == nullptr) return found;
    const auto canonical = [](const QString& file) {
        const QString path = QFileInfo(file).canonicalFilePath();
        return path.isEmpty() ? file : path;
    };
    QSet<QString> done{canonical(doc->getDocName())};
    QStringList path{canonical(doc->getDocName())};   // the files from the top down to the one read
    // Depth first, each file checked once, in the order met. A file that
    // uses one on its own way down from the top is a cycle: an error of
    // the file that closes it (\a index: its findings; a file that uses
    // itself is told by its own check).
    std::function<void(Schematic*, int)> walk = [&](Schematic* sch, int index) {
        for (Component* c : sch->a_DocComps) {
            if (c->Model != QLatin1String("Sub")) continue;
            const QString file = canonical(c->getSubcircuitFile());
            if (!foundFile(file) || file == path.last()) continue;
            if (const qsizetype at = path.indexOf(file); at >= 0) {
                if (index < 0 || !inCircuit(c)) continue;   // (one left out of the netlist nests nothing)
                QStringList chain{QFileInfo(path.last()).fileName()};
                for (const QString& f : path.mid(at)) chain << QFileInfo(f).fileName();
                found[index].issues << Issue{Severity::Error,
                                             tr("%1 uses %2, which uses this schematic again (%3): a subcircuit cannot hold "
                                                "itself - ngspice nests it until it gives up (\"unknown subckt\")")
                                                 .arg(c->Name, QFileInfo(file).fileName(), chain.join(QStringLiteral(" -> "))),
                                             QPoint(c->cx, c->cy), c->Name, sch->getDocName()};
                continue;
            }
            if (done.contains(file)) continue;
            done.insert(file);
            Schematic* sub = open ? open(file) : nullptr;
            std::unique_ptr<Schematic> loaded;
            if (sub == nullptr) {
                loaded.reset(new Schematic(nullptr, file));
                if (!loaded->load()) {
                    found << SubcircuitFindings{file, {Issue{Severity::Error, tr("the subcircuit file could not be loaded"),
                                                             QPoint(), QString(), file}}};
                    continue;
                }
                sub = loaded.get();
            }
            found << SubcircuitFindings{file, check(sub)};
            path << file;
            walk(sub, int(found.size()) - 1);
            path.removeLast();
        }
    };
    walk(doc, -1);
    return found;
}

int errorCount(const QList<Issue>& issues)
{
    return int(std::count_if(issues.begin(), issues.end(),
                             [](const Issue& i) { return i.severity == Severity::Error; }));
}

} // namespace qucs_s::erc
