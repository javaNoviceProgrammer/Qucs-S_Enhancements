/*
 * prbssource.cpp - the PRBS source a trace's signal comes from (see
 * prbssource.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "prbssource.h"

#include "main.h"
#include "misc.h"
#include "node.h"
#include "qucs.h"
#include "schematic.h"
#include "valuereading.h"
#include "wire.h"
#include "wirelabel.h"
#include "components/component.h"
#include "diagrams/eyediagram.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <vector>

namespace qucs_s::prbs {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("PrbsSource", text);
}

bool isPrbs(const Component* c)
{
    return c->Model == QLatin1String("vPRBS") && c->isActive == COMP_IS_ACTIVE;
}

QString valueOf(const Component* c, const char* name)
{
    for (const Property* p : c->Props)
        if (p->Name == QLatin1String(name)) return p->Value.trimmed();
    return QString();
}

double secondsOf(const QString& text)
{
    const qucs_s::units::Reading r = qucs_s::units::read(text);
    return r.kind == qucs_s::units::Reading::Number && std::isfinite(r.value) && r.value > 0.0 ? r.value
                                                                                                 : std::numeric_limits<double>::quiet_NaN();
}

QString shown(double seconds)
{
    return EyeDiagram::engineering(seconds, QStringLiteral("s"));
}

// A source's bits: its Tbit and levels, and what changed since the run.
struct Bits {
    double ui = std::numeric_limits<double>::quiet_NaN();
    int levels = 2;
    QString note, why;
};

// \a c's bits as \a run (a netlist; empty: none kept) gave them - its line,
// "V1 rx 0 PRBS(0 1 100p 0 15p 15p 7)" - else as they are now.
Bits bitsOf(const Component* c, const QString& run)
{
    Bits b;
    const QString tbit = valueOf(c, "Tbit");
    const double now = secondsOf(tbit);
    const int levelsNow = valueOf(c, "Coding").compare(QLatin1String("PAM4"), Qt::CaseInsensitive) == 0 ? 4 : 2;
    if (!run.isEmpty()) {
        // (Its name in the netlist: V1, or a V before one that has none.)
        const QString ref = c->Name.startsWith(QLatin1Char('V'), Qt::CaseInsensitive) ? c->Name : QStringLiteral("V") + c->Name;
        const QRegularExpression line(QStringLiteral(R"(^\s*%1\s+\S+\s+\S+\s+(PRBS|PAM4)\s*\(([^)]*)\))")
                                          .arg(QRegularExpression::escape(ref)),
                                      QRegularExpression::MultilineOption | QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch m = line.match(run);
        if (!m.hasMatch()) {
            b.why = tr("%1 was not in the run the data is of: simulate again").arg(c->Name);
            return b;
        }
        const double then = secondsOf(m.captured(2).split(QRegularExpression(QStringLiteral(R"(\s+)")), Qt::SkipEmptyParts).value(2));
        if (std::isfinite(then)) {
            b.ui = then;
            b.levels = m.captured(1).compare(QLatin1String("PAM4"), Qt::CaseInsensitive) == 0 ? 4 : 2;
            QStringList changed;
            if (std::isfinite(now) && std::abs(now - then) > 1e-9 * then)
                changed << tr("%1's Tbit is %2 now, %3 in the run the data is of").arg(c->Name, shown(now), shown(then));
            if (levelsNow != b.levels)
                changed << tr("%1's Coding is %2 now, %3 in the run the data is of")
                               .arg(c->Name, levelsNow == 4 ? QStringLiteral("PAM4") : QStringLiteral("NRZ"),
                                    b.levels == 4 ? QStringLiteral("PAM4") : QStringLiteral("NRZ"));
            if (!changed.isEmpty()) b.note = changed.join(QStringLiteral("; ")) + tr(": simulate again to see it");
            return b;
        }
    }
    if (!std::isfinite(now)) {
        b.why = tr("%1's Tbit is %2, no number").arg(c->Name, tbit.isEmpty() ? tr("empty") : tbit);
        return b;
    }
    b.ui = now;
    b.levels = levelsNow;
    return b;
}

} // namespace

QString nodeOf(const QString& variable)
{
    // Without its simulator ("ngspice/" - only that: v(out)/v(in) is no
    // node's).
    static const QRegularExpression simulator(QStringLiteral(R"(^[A-Za-z0-9_]+/)"));
    QString v = variable.trimmed();
    v.remove(simulator);
    // v(rx), with its analysis (tran.v(rx)) or not - and no other dataset's
    // (a run kept, run1:v(out); an import, m:gain: not the circuit as it is).
    static const QRegularExpression voltage(QStringLiteral(R"(^(?:[A-Za-z0-9_]+\.)?v\(\s*([^,()\s/]+)\s*\)$)"),
                                            QRegularExpression::CaseInsensitiveOption);
    if (const QRegularExpressionMatch m = voltage.match(v); m.hasMatch()) return m.captured(1);
    static const QRegularExpression qucsator(QStringLiteral(R"(^([^.()\s]+)\.Vt$)"));
    if (const QRegularExpressionMatch m = qucsator.match(v); m.hasMatch()) return m.captured(1);
    return QString();
}

QString datasetOf(const Schematic* sch, const QString& variable)
{
    if (sch == nullptr || sch->getDocName().isEmpty() || sch->getDataSet().isEmpty()) return QString();
    const qsizetype slash = variable.indexOf(QLatin1Char('/'));
    const QString ending = slash > 0 ? QLatin1Char('.') + variable.left(slash) : QString();
    return QFileInfo(sch->getDocName()).absolutePath() + QLatin1Char('/') + sch->getDataSet() + ending;
}

Source sourceOf(const Schematic* sch, const QString& variable, const QString& dataset)
{
    Source none;
    if (sch == nullptr) return none;
    const QString node = nodeOf(variable);
    if (node.isEmpty() || std::none_of(sch->a_DocComps.cbegin(), sch->a_DocComps.cend(), isPrbs)) return none;

    // The nets: nodes joined by wires, by labels of one name; the grounds
    // one net.
    std::vector<int> up;
    QHash<const Node*, int> ids;
    QHash<QString, int> labels;
    const auto make = [&up] {
        up.push_back(int(up.size()));
        return int(up.size()) - 1;
    };
    const auto root = [&up](int i) {
        while (up[i] != i) i = up[i] = up[up[i]];
        return i;
    };
    const auto join = [&](int a, int b) { up[root(a)] = root(b); };
    const auto nodeId = [&](const Node* n) {
        const auto it = ids.constFind(n);
        return it != ids.constEnd() ? *it : *ids.insert(n, make());
    };
    const int ground = make();
    const auto labelled = [&](const Conductor* c, int id) {
        if (!c->hasLabel()) return;
        const QString name = c->label()->Name;
        if (name.compare(QLatin1String("gnd"), Qt::CaseInsensitive) == 0) {
            join(id, ground);
            return;
        }
        const auto it = labels.constFind(name);
        join(id, it != labels.constEnd() ? *it : *labels.insert(name, make()));
    };
    for (const Wire* w : sch->a_DocWires) {
        if (w->Port1 == nullptr || w->Port2 == nullptr) continue;
        join(nodeId(w->Port1), nodeId(w->Port2));
        labelled(w, nodeId(w->Port1));
    }
    for (const Node* n : sch->a_DocNodes) labelled(n, nodeId(n));
    for (const Component* c : sch->a_DocComps)
        if (c->Model == QLatin1String("GND") && c->isActive == COMP_IS_ACTIVE)
            for (const Port* p : c->Ports)
                if (p->Connection != nullptr) join(nodeId(p->Connection), ground);

    // The variable's net: a label of its name, or a node the netlist named
    // so (ngspice writes its names in lower case).
    int start = -1;
    for (auto it = labels.constBegin(); it != labels.constEnd() && start < 0; ++it)
        if (it.key().compare(node, Qt::CaseInsensitive) == 0) start = root(it.value());
    for (const Node* n : sch->a_DocNodes)
        if (start < 0 && !n->Name.isEmpty() && n->Name.compare(node, Qt::CaseInsensitive) == 0) start = root(nodeId(n));
    const int earth = root(ground);
    if (start < 0 || start == earth) return none;

    // The parts on each net (those in the simulation).
    QHash<int, QList<const Component*>> on;
    for (const Component* c : sch->a_DocComps) {
        if (c->isActive != COMP_IS_ACTIVE || c->Model == QLatin1String("GND")) continue;
        QSet<int> nets;
        for (const Port* p : c->Ports)
            if (p->Connection != nullptr) nets << root(nodeId(p->Connection));
        for (int net : nets) on[net] << c;
    }

    // Out from the net a part at a time, never through ground: the PRBS
    // sources first met.
    QSet<int> reached{start};
    QSet<const Component*> crossed;
    QList<int> level{start};
    while (!level.isEmpty()) {
        QList<const Component*> found;
        for (int net : level)
            for (const Component* c : on.value(net))
                if (c->Model == QLatin1String("vPRBS") && !found.contains(c)) found << c;
        if (!found.isEmpty()) {
            // (The run's netlist, when one is kept for the dataset as it is.)
            const QString run = dataset.isEmpty() ? QString() : misc::runNetlistOf(dataset);
            QStringList names, unread, notes;
            QList<Bits> bits;
            for (const Component* c : found) {
                const Bits b = bitsOf(c, run);
                if (!b.why.isEmpty()) {
                    unread << b.why;
                    continue;
                }
                names << c->Name;
                bits << b;
                if (!b.note.isEmpty()) notes << b.note;
            }
            if (!unread.isEmpty()) {
                none.why = unread.join(QStringLiteral("; "));
                return none;
            }
            for (const Bits& b : bits)
                if (std::abs(b.ui - bits.first().ui) > 1e-12 * bits.first().ui || b.levels != bits.first().levels) {
                    QStringList each;
                    for (int i = 0; i < names.size(); ++i)
                        each << QStringLiteral("%1 %2 s%3").arg(names.at(i)).arg(bits.at(i).ui, 0, 'g', 6)
                                    .arg(bits.at(i).levels == 4 ? QStringLiteral(" PAM4") : QString());
                    none.why = tr("as near, %1: bits of different lengths or codings").arg(each.join(QStringLiteral(", ")));
                    return none;
                }
            Source s;
            s.name = names.join(QStringLiteral(" and "));
            s.ui = bits.first().ui;
            s.levels = bits.first().levels;
            s.note = notes.join(QStringLiteral("; "));
            return s;
        }
        QList<int> next;
        for (int net : level)
            for (const Component* c : on.value(net)) {
                if (crossed.contains(c)) continue;
                crossed.insert(c);
                for (const Port* p : c->Ports) {
                    if (p->Connection == nullptr) continue;
                    const int r = root(nodeId(p->Connection));
                    if (r == earth || reached.contains(r)) continue;
                    reached.insert(r);
                    next << r;
                }
            }
        level = next;
    }
    return none;
}

void installForEyeDiagrams()
{
    EyeDiagram::setSourceFinder([](const EyeDiagram* d, const QString& variable) -> Source {
        if (QucsMain == nullptr) return {};
        const QList<QucsDoc*> docs = QucsMain->allDocuments();
        const Schematic* owner = nullptr;
        for (QucsDoc* doc : docs)
            if (const auto* s = dynamic_cast<const Schematic*>(doc))
                if (std::find(s->a_DocDiags.cbegin(), s->a_DocDiags.cend(), d) != s->a_DocDiags.cend()) owner = s;
        if (owner == nullptr) return {};
        // A data display's diagram: the circuit of its dataset, if open.
        if (owner->getDocName().endsWith(QLatin1String(".dpl"), Qt::CaseInsensitive)) {
            const QString folder = QFileInfo(owner->getDocName()).absolutePath();
            const Schematic* circuit = nullptr;
            for (QucsDoc* doc : docs)
                if (const auto* s = dynamic_cast<const Schematic*>(doc))
                    if (s != owner && !s->getDocName().endsWith(QLatin1String(".dpl"), Qt::CaseInsensitive)
                        && s->getDataSet() == owner->getDataSet() && QFileInfo(s->getDocName()).absolutePath() == folder)
                        circuit = s;
            owner = circuit;
        }
        return sourceOf(owner, variable, datasetOf(owner, variable));
    });
}

} // namespace qucs_s::prbs
