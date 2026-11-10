/*
 * qucscontrol_showschematic.cpp - show_schematic: a schematic's or data
 *                                 display's page as its tab and toolbar
 *                                 show it, and the user's pen on it
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "qucscontrol.h"
#include "qucscontrol_p.h"
#include "datadisplay.h"
#include "diagrams/diagram.h"
#include "diagrams/graph.h"
#include "markup.h"
#include "qucs.h"
#include "schematic.h"

#include <QJsonArray>
#include <QJsonObject>
#include <cmath>

using namespace qucs_s::control;

QJsonObject QucsControl::showSchematic(const QJsonObject& args)
{
    QString error;
    Schematic* page = schematic(args, &error, false);
    if (page == nullptr) return errorResult(error);

    // Every argument checked before anything is done.
    const QJsonValue pen = args.value(QLatin1String("pen"));
    if (args.contains(QLatin1String("pen")) && !pen.isBool())
        return errorResult(tr("'pen' is true (the user's pen taken up: a drag on the page draws on it) or false (put down)."));
    const QString drawings = args.value(QLatin1String("drawings")).toString().trimmed().toLower();
    if (args.contains(QLatin1String("drawings")) && drawings != QLatin1String("clear"))
        return errorResult(tr("'drawings' is clear: what the pen drew on the page taken away (the pen's Undo gives it back)."));
    const QJsonValue symbol = args.value(QLatin1String("symbol"));
    if (args.contains(QLatin1String("symbol")) && !symbol.isBool())
        return errorResult(tr("'symbol' is true: the strokes 'addressed' numbers are the symbol's (read_drawings' page: symbol)."));
    const QString part = symbol.toBool() ? QStringLiteral("symbol") : QStringLiteral("page");
    const QJsonValue addressed = args.value(QLatin1String("addressed"));
    const bool addressedNone = addressed.isString() && addressed.toString().trimmed().compare(QLatin1String("none"), Qt::CaseInsensitive) == 0;
    QList<int> addressedIds;
    if (args.contains(QLatin1String("addressed")) && !addressedNone) {
        bool good = addressed.isArray() && !addressed.toArray().isEmpty();
        for (const QJsonValue& v : addressed.toArray()) {
            good = good && v.isDouble() && v.toDouble() == std::floor(v.toDouble()) && v.toDouble() >= 1 && v.toDouble() <= 1e9;
            if (good) addressedIds << v.toInt();
        }
        if (!good)
            return errorResult(tr("'addressed' is a list of the pen's stroke numbers, as read_drawings numbers them (marked dealt "
                                  "with: faded, and left out of what a prompt tells), or none."));
    }

    a_app->showDocument(QucsApp::documentWidget(page));
    const bool data = page->isDataDisplayPage();
    QJsonObject o{{QStringLiteral("document"), titleOf(page)},
                  {QStringLiteral("page"), data                     ? tr("a data display")
                                           : page->getSymbolMode() ? tr("a schematic, its symbol in the editor (what the pen draws now is the symbol's)")
                                                                   : tr("a schematic")}};
    QStringList done;
    if (drawings == QLatin1String("clear"))
        done << (page->clearDrawing(true) ? tr("what the pen drew taken away") : tr("nothing was drawn to take away"));
    if (addressedNone) {
        int changed = 0;
        for (const QString& on : page->penDrawing().parts()) {
            QList<int> all;
            for (const qucs_s::markup::Stroke& s : page->penDrawing().on(on)) all << s.id;
            changed += page->setStrokesAddressed(all, false, on);
        }
        done << tr("%n stroke(s) no longer marked addressed", "", changed);
    } else if (!addressedIds.isEmpty()) {
        QStringList missing;
        for (const int id : std::as_const(addressedIds))
            if (page->penDrawing().find(part, id) == nullptr) missing << QString::number(id);
        done << tr("%n stroke(s) marked addressed", "", page->setStrokesAddressed(addressedIds, true, part));
        if (!missing.isEmpty())
            o.insert(QStringLiteral("not drawn"), tr("no stroke numbered %1 on the %2").arg(missing.join(QStringLiteral(", ")), symbol.toBool() ? tr("symbol") : tr("page")));
    }
    if (pen.isBool()) {
        page->setPenActive(pen.toBool());
        done << (pen.toBool() ? tr("the pen taken up") : tr("the pen put down"));
    }
    if (!done.isEmpty()) o.insert(QStringLiteral("done"), done.join(QStringLiteral("; ")));

    // What the page shows: its diagrams and their traces, by the numbers
    // the other tools take; a schematic's parts counted (get_schematic
    // lists them).
    if (!data) {
        o.insert(QStringLiteral("components"), int(page->a_DocComps.size()));
        o.insert(QStringLiteral("wires"), int(page->a_DocWires.size()));
    }
    QJsonArray diagrams;
    int n = 0;
    for (const Diagram* d : page->a_DocDiags) {
        QJsonObject one{{QStringLiteral("diagram"), ++n}, {QStringLiteral("type"), d->Name}};
        if (!d->title.isEmpty()) one.insert(QStringLiteral("title"), d->title);
        QJsonArray traces;
        for (const Graph* g : d->Graphs) traces.append(g->Var);
        one.insert(QStringLiteral("traces"), traces);
        diagrams.append(one);
    }
    if (data || !diagrams.isEmpty()) o.insert(QStringLiteral("diagrams"), diagrams);
    o.insert(QStringLiteral("pen"), page->penActive() ? tr("taken up: a drag on the page draws on it (Escape puts it down)")
                                                      : tr("put down: its toolbar's pen takes it up"));
    // What the user drew, read stroke by stroke - what each is, what it
    // marks, its box in the page's coordinates - the latest 40.
    if (!page->penDrawing().isEmpty()) insertDrawings(o, qucs_s::datadisplay::readDrawing(page, false));
    else o.insert(QStringLiteral("drawings"), tr("nothing is drawn on the page"));
    return jsonResult(o);
}
