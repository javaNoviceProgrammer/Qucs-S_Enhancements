/*
 * probe.cpp - cross-probing: what is clicked on the schematic (a net, a
 * part's pin, a part) put into a diagram as its voltage, its current or
 * its power, and the net a trace shows found again on the schematic
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "probe.h"

#include "components/component.h"
#include "dataset.h"
#include "diagrams/graph.h"
#include "diagrams/rectdiagram.h"
#include "diagrams/stackeddiagram.h"
#include "extsimkernels/simulationrun.h"
#include "extsimkernels/spicecompat.h"
#include "main.h"
#include "node.h"
#include "schematic.h"
#include "settings.h"
#include "wire.h"
#include "wirelabel.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QTemporaryDir>

namespace qucs_s::probe {

namespace {

namespace ds = qucs_s::dataset;

QString tr(const char* text) { return QObject::tr(text); }

// A part a probe passes over: what is no device of the circuit.
bool notProbed(const Component* c)
{
    return c->isSimulation || c->isEquation || c->Model == QLatin1String("GND") || c->Model == QLatin1String("Port")
           || c->Model.startsWith(QLatin1Char('.')) || c->Ports.isEmpty() || c->isActive != COMP_IS_ACTIVE;
}

// The node a target's net is reached from.
Node* nodeOf(const Target& t)
{
    if (t.component != nullptr && t.pin >= 0 && t.pin < t.component->Ports.size()) return t.component->Ports.at(t.pin)->Connection;
    if (t.node != nullptr) return t.node;
    return t.wire != nullptr ? t.wire->Port1 : nullptr;
}

// The SPICE netlist of \a sch as a simulation would write it now, its
// nodes' names as it named them (in \a names), the schematic's nodes
// given back the names they had.
bool writtenNetlist(Schematic* sch, QString* text, QHash<Node*, QString>* names, QString* error)
{
    QHash<Node*, QString> kept;
    for (Node* n : sch->a_DocNodes) kept.insert(n, n->Name);
    QTemporaryDir dir;
    const QString file = dir.filePath(QStringLiteral("probe.cir"));
    bool ok = false;
    {
        SimulationRun run(sch, false);
        ok = run.writeNetlist(file);
    }
    for (Node* n : sch->a_DocNodes) {
        if (names) names->insert(n, n->Name);
        n->Name = kept.value(n);
    }
    if (!ok) {
        *error = tr("The netlist could not be written (Check Schematic says why).");
        return false;
    }
    QFile f(file);
    if (text && f.open(QIODevice::ReadOnly | QIODevice::Text)) *text = QString::fromUtf8(f.readAll());
    return true;
}

// The simulator's prefix of a trace's variable ("ngspice/"), and the
// suffix of its dataset file (".ngspice").
QString simulatorName()
{
    switch (QucsSettings.DefaultSimulator) {
    case spicecompat::simNgspice: return QStringLiteral("ngspice");
    case spicecompat::simXyce: return QStringLiteral("xyce");
    case spicecompat::simSpiceOpus: return QStringLiteral("spopus");
    default: return QString();
    }
}

QString datasetPath(Schematic* sch)
{
    return QFileInfo(sch->getDocName()).absoluteDir().filePath(sch->getDataSet());
}

// The net name of a voltage's trace in this simulator's dataset, as an
// analysis writes it: v(out) (SPICE), out.Vt or out.v (Qucsator).
QString voltageName(const QString& node, const QString& analysis)
{
    if (QucsSettings.DefaultSimulator == spicecompat::simQucsator)
        return node + (analysis == QLatin1String("tran") ? QStringLiteral(".Vt") : QStringLiteral(".v"));
    return QStringLiteral("v(%1)").arg(node.toLower());
}

// The analyses a trace may be of: the schematic's simulations, in order
// (tran, ac, dc - SPICE's names of Qucs-S's .TR, .AC, .DC).
QStringList analysesOf(Schematic* sch)
{
    QStringList list;
    for (Component* c : sch->a_DocComps) {
        if (!c->isSimulation || c->isActive != COMP_IS_ACTIVE) continue;
        const QString a = c->Model == QLatin1String(".TR")   ? QStringLiteral("tran")
                          : c->Model == QLatin1String(".AC") ? QStringLiteral("ac")
                          : c->Model == QLatin1String(".DC") ? QStringLiteral("dc")
                                                            : QString();
        if (!a.isEmpty() && !list.contains(a)) list << a;
    }
    return list;
}

// A fresh net name: none of the schematic's labels, none of its nodes'.
QString freshNetName(Schematic* sch, const QHash<Node*, QString>& netlistNames)
{
    QSet<QString> used;
    for (Wire* w : sch->a_DocWires)
        if (w->hasLabel()) used.insert(w->label()->Name.toLower());
    for (Node* n : sch->a_DocNodes)
        if (n->hasLabel()) used.insert(n->label()->Name.toLower());
    for (const QString& name : netlistNames) used.insert(name.toLower());
    for (int i = 1;; ++i) {
        const QString name = QStringLiteral("net%1").arg(i);
        if (!used.contains(name)) return name;
    }
}

// The last diagram of each schematic probed into.
QHash<const Schematic*, const Diagram*>& lastProbed()
{
    static QHash<const Schematic*, const Diagram*> last;
    return last;
}

bool holds(const Schematic* sch, const Diagram* d)
{
    for (const Diagram* x : sch->a_DocDiags)
        if (x == d) return true;
    return false;
}

bool drawsAgainstX(const Diagram* d)
{
    return d->Name == QLatin1String("Rect") || d->Name == QLatin1String("Stacked");
}

} // namespace

std::optional<Target> at(Schematic* sch, const QPoint& p)
{
    // A pin's end first: a current.
    for (Component* c : sch->a_DocComps) {
        if (notProbed(c)) continue;
        for (int i = 0; i < c->Ports.size(); ++i) {
            const QPoint pin(c->cx + c->Ports.at(i)->x, c->cy + c->Ports.at(i)->y);
            if ((pin - p).manhattanLength() <= 4) return Target{Kind::Current, nullptr, nullptr, c, i};
        }
    }
    // A net: a node, a wire, a net label's text.
    if (Node* n = sch->findNode(p)) return Target{Kind::Voltage, nullptr, n};
    if (Wire* w = sch->selectedWire(p.x(), p.y())) return Target{Kind::Voltage, w};
    for (Wire* w : sch->a_DocWires)
        if (w->hasLabel() && w->label()->boundingRect().contains(p)) return Target{Kind::Voltage, w};
    for (Node* n : sch->a_DocNodes)
        if (n->hasLabel() && n->label()->boundingRect().contains(p)) return Target{Kind::Voltage, nullptr, n};
    // A part: its power.
    for (Component* c : sch->a_DocComps)
        if (!notProbed(c) && c->getSelected(p.x(), p.y())) return Target{Kind::Power, nullptr, nullptr, c};
    return std::nullopt;
}

QString netName(Schematic* sch, const Target& target, QString* error)
{
    Node* node = nodeOf(target);
    if (node == nullptr) {
        *error = tr("The pin is connected to nothing.");
        return QString();
    }
    QHash<Node*, QString> names;
    if (!writtenNetlist(sch, nullptr, &names, error)) return QString();
    const QString name = names.value(node);
    if (name.isEmpty()) *error = tr("The netlist gives the net no name.");
    return name == QLatin1String("gnd") ? QStringLiteral("0") : name;
}

QString vectorOf(Schematic* sch, const Target& target, QString* error)
{
    if (target.kind == Kind::Voltage) {
        const QString name = netName(sch, target, error);
        return name.isEmpty() ? QString() : QStringLiteral("v(%1)").arg(name.toLower());
    }
    Component* c = target.component;
    if (c == nullptr) {
        *error = tr("Which part?");
        return QString();
    }
    if (QucsSettings.DefaultSimulator == spicecompat::simQucsator) {
        *error = tr("Qucsator saves no device's current or power: under ngspice a pin's current and a part's power are "
                    "saved when probed (or put a current probe, iProbe, in the wire).");
        return QString();
    }
    // The device's line in the netlist, and its nodes.
    QString text;
    QHash<Node*, QString> names;
    if (!writtenNetlist(sch, &text, &names, error)) return QString();
    const QString ref = spicecompat::check_refdes(c->Name, c->SpiceModel).toLower();
    QStringList tokens;
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        const QStringList t = line.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (!t.isEmpty() && t.first().toLower() == ref) {
            tokens = t;
            break;
        }
        if (!t.isEmpty() && tokens.isEmpty() && t.first().toLower() == c->Name.toLower()) tokens = t;
    }
    if (tokens.isEmpty()) {
        *error = tr("%1 writes no device of its own into the netlist.").arg(c->Name);
        return QString();
    }
    const QString device = tokens.first().toLower();
    const QChar letter = device.at(0);
    if (letter == QLatin1Char('x')) {
        *error = tr("%1 is a subcircuit: its devices are inside it - probe a net of it, or put a current probe (iProbe) "
                    "in the wire.").arg(c->Name);
        return QString();
    }
    if (target.kind == Kind::Power) return QStringLiteral("@%1[p]").arg(device);

    // The current at the terminal of the pin, as SPICE orders the device's.
    QStringList currents;
    switch (letter.toLatin1()) {
    case 'v': case 'l': case 'e': case 'h': return QStringLiteral("i(%1)").arg(device);   // its branch
    case 'r': case 'c': case 'f': case 'g': case 'b': return QStringLiteral("@%1[i]").arg(device);
    case 'i': return QStringLiteral("@%1[current]").arg(device);
    case 'd': return QStringLiteral("@%1[id]").arg(device);
    case 'q': currents = {"ic", "ib", "ie", "is"}; break;
    case 'm': currents = {"id", "ig", "is", "ib"}; break;
    case 'j': case 'z': currents = {"id", "ig", "is"}; break;
    default:
        *error = tr("%1 (%2) has no current ngspice can save.").arg(c->Name, device);
        return QString();
    }
    QString node = names.value(nodeOf(target)).toLower();
    if (node == QLatin1String("gnd")) node = QStringLiteral("0");
    for (int k = 1; k < tokens.size() && k <= currents.size(); ++k)
        if (tokens.at(k).toLower() == node) return QStringLiteral("@%1[%2]").arg(device, currents.at(k - 1));
    *error = tr("%1's pin %2 is none of %3's terminals in the netlist.").arg(c->Name).arg(target.pin + 1).arg(device);
    return QString();
}

Diagram* frontDiagram(Schematic* sch)
{
    for (Diagram* d : sch->a_DocDiags)
        if (d->isSelected && drawsAgainstX(d)) return d;
    if (const Diagram* last = lastProbed().value(sch); last && holds(sch, last)) return const_cast<Diagram*>(last);
    for (Diagram* d : sch->a_DocDiags)
        if (drawsAgainstX(d)) return d;
    return nullptr;
}

QString Result::text() const
{
    if (!error.isEmpty()) return error;
    QStringList said;
    if (!labelled.isEmpty()) said << tr("The net is labelled %1 (ngspice saves the nets that have a name).").arg(labelled);
    said << (already ? tr("The diagram shows %1 already.") : newDiagram ? tr("%1 is in a new diagram.") : tr("%1 is added."))
                .arg(variable);
    if (!hasData)
        said << (saved.isEmpty() ? tr("Simulate to see it.") : tr("The next simulation saves %1: simulate to see it.").arg(saved));
    return said.join(QLatin1Char(' '));
}

Result probe(Schematic* sch, const Target& target, Diagram* into)
{
    Result r;
    const int simulator = QucsSettings.DefaultSimulator;
    if (simulator == spicecompat::simNotSpecified) {
        r.error = tr("No simulator is chosen.");
        return r;
    }
    bool changed = false;

    // The vector (and the net's name, labelled when it has none).
    QString vector, node;
    if (target.kind == Kind::Voltage) {
        node = netName(sch, target, &r.error);
        if (node.isEmpty()) return r;
        if (node == QLatin1String("0")) {
            r.error = tr("That is ground: 0 V, the reference of the others.");
            return r;
        }
        // An unnamed net ("_net3", which ngspice does not save, and which
        // another edit may number otherwise): labelled first.
        if (node.startsWith(QLatin1String("_net"))) {
            QHash<Node*, QString> names;
            QString error;
            writtenNetlist(sch, nullptr, &names, &error);
            node = freshNetName(sch, names);
            if (target.wire != nullptr) {
                Wire* w = target.wire;
                const int x = (w->x1 + w->x2) / 2, y = (w->y1 + w->y2) / 2;
                int xl = x + 20, yl = y - 30;
                sch->setOnGrid(xl, yl);
                w->setName(node, QString(), x, y, xl, yl);
            } else {
                Node* n = nodeOf(target);
                int xl = n->cx + 20, yl = n->cy - 30;
                sch->setOnGrid(xl, yl);
                n->setName(node, QString(), xl, yl);
            }
            r.labelled = node;
            changed = true;
        }
    } else {
        vector = vectorOf(sch, target, &r.error);
        if (vector.isEmpty()) return r;
    }

    // The diagram: given, in front, or a new one below the circuit.
    Diagram* d = into ? into : frontDiagram(sch);
    if (d == nullptr) {
        d = new RectDiagram();
        const QRect used = sch->allBoundingRect();
        const bool empty = sch->a_DocComps.empty() && sch->a_DocWires.empty() && sch->a_DocDiags.empty();
        d->cx = empty ? 60 : used.left() + 60;
        d->cy = (empty ? 0 : used.bottom()) + 80 + d->y2;
        sch->a_DocDiags.push_back(d);
        r.newDiagram = true;
        changed = true;
    }
    r.diagram = d;

    // The analysis: the diagram's traces', else the first of the dataset
    // that has the vector, else the schematic's first simulation's.
    const QString prefix = simulatorName();
    ds::Dataset data;
    const QString file = datasetPath(sch) + (prefix.isEmpty() ? QString() : QLatin1Char('.') + prefix);
    const bool read = QFileInfo::exists(file) && data.read(file);
    QString analysis;
    for (const Graph* g : d->Graphs) {
        const QString bare = ds::withoutSimulator(g->Var);
        analysis = ds::analysisOf(bare);
        if (!analysis.isEmpty()) break;
    }
    const auto nameIn = [&](const QString& a) {
        if (target.kind == Kind::Voltage) {
            const QString v = voltageName(node, a);
            return simulator == spicecompat::simQucsator ? v : a + QLatin1Char('.') + v;
        }
        return a + QLatin1Char('.') + vector;
    };
    const auto inData = [&](const QString& name) -> QString {
        if (!read) return QString();
        for (const ds::Variable& v : data.variables())
            if (v.name.compare(name, Qt::CaseInsensitive) == 0) return v.name;
        return QString();
    };
    QString variable;
    if (!analysis.isEmpty()) {
        variable = inData(nameIn(analysis));
        if (variable.isEmpty()) variable = nameIn(analysis);
    } else {
        QStringList candidates = analysesOf(sch);
        for (const QString& a : {QStringLiteral("tran"), QStringLiteral("ac"), QStringLiteral("dc")})
            if (!candidates.contains(a)) candidates << a;
        for (const QString& a : std::as_const(candidates))
            if (const QString found = inData(nameIn(a)); !found.isEmpty()) {
                variable = found;
                break;
            }
        if (variable.isEmpty()) variable = nameIn(candidates.first());
    }
    r.hasData = !inData(variable).isEmpty();
    r.variable = prefix.isEmpty() ? variable : prefix + QLatin1Char('/') + variable;

    // A current or a power the dataset lacks: saved by the next run.
    if (!r.hasData && !vector.isEmpty() && !sch->getProbeSaves().contains(vector)) {
        QStringList saves = sch->getProbeSaves();
        saves << vector;
        sch->setProbeSaves(saves);
        r.saved = vector;
        changed = true;
    }

    // The trace, unless it is there.
    for (Graph* g : d->Graphs)
        if (g->Var.compare(r.variable, Qt::CaseInsensitive) == 0) {
            r.already = true;
            r.graph = g;
        }
    if (!r.already) {
        auto* g = new Graph(d, r.variable);
        const QList<QColor>& palette = Graph::autoPalette();
        g->Color = palette.at(d->Graphs.size() % palette.size());
        g->Thick = _settings::Get().item<QString>("DefaultGraphLineWidth").toInt();
        if (auto* stacked = dynamic_cast<StackedDiagram*>(d)) g->pane = stacked->paneCount() - 1;
        d->Graphs.append(g);
        r.graph = g;
        changed = true;
        d->loadGraphData(datasetPath(sch));
    }
    lastProbed().insert(sch, d);
    if (changed) sch->setChanged(true, true);
    return r;
}

QString netOfVariable(const QString& variable)
{
    // "ngspice/tran.v(out)": out; Qucsator's out.Vt, out.v.
    static const QRegularExpression spice(QStringLiteral("(?:^|\\.)v\\(([^)]+)\\)$"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression qucsator(QStringLiteral("^([^./]+)\\.(?:Vt|v)$"));
    const QString bare = ds::withoutSimulator(variable);
    if (const QRegularExpressionMatch m = spice.match(bare); m.hasMatch()) return m.captured(1);
    if (const QRegularExpressionMatch m = qucsator.match(bare); m.hasMatch()) return m.captured(1);
    return QString();
}

QString partOfVariable(const QString& variable)
{
    // i(v1), @r1[i], @q1[ic], @r1[p]: the device (as the netlist names it).
    static const QRegularExpression current(QStringLiteral("(?:^|\\.)(?:i\\(([^)]+)\\)|@([^\\[]+)\\[[a-z]+\\])$"),
                                            QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = current.match(ds::withoutSimulator(variable));
    if (!m.hasMatch()) return QString();
    return m.captured(1).isEmpty() ? m.captured(2) : m.captured(1);
}

} // namespace qucs_s::probe
