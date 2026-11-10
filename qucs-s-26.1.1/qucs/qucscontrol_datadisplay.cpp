/*
 * qucscontrol_datadisplay.cpp - show_data_display: a data display's page
 *                               (.dpl) as its tab and toolbar show it, and
 *                               the user's pen on it
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

QJsonObject QucsControl::showDataDisplay(const QJsonObject& args)
{
    QString error;
    Schematic* page = schematic(args, &error, false);
    if (page == nullptr) return errorResult(error);
    if (!page->isDataDisplayPage())
        return errorResult(tr("%1 is not a data display's page (.dpl): the pen and its toolbar are a data display's. A schematic's "
                              "data display is its .dpl (open_document opens it, new_document's data_display makes one).")
                               .arg(titleOf(page)));

    // Every argument checked before anything is done.
    const QJsonValue pen = args.value(QLatin1String("pen"));
    if (args.contains(QLatin1String("pen")) && !pen.isBool())
        return errorResult(tr("'pen' is true (the user's pen taken up: a drag on the page draws on it) or false (put down)."));
    const QString drawings = args.value(QLatin1String("drawings")).toString().trimmed().toLower();
    if (args.contains(QLatin1String("drawings")) && drawings != QLatin1String("clear"))
        return errorResult(tr("'drawings' is clear: what the pen drew on the page taken away (the pen's Undo gives it back)."));
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
    QJsonObject o{{QStringLiteral("document"), titleOf(page)}};
    QStringList done;
    if (drawings == QLatin1String("clear"))
        done << (page->clearDrawing() ? tr("what the pen drew taken away") : tr("nothing was drawn to take away"));
    if (addressedNone) {
        QList<int> all;
        for (const qucs_s::markup::Stroke& s : page->penDrawing().strokes()) all << s.id;
        done << tr("%n stroke(s) no longer marked addressed", "", page->setStrokesAddressed(all, false));
    } else if (!addressedIds.isEmpty()) {
        QStringList missing;
        for (const int id : std::as_const(addressedIds))
            if (page->penDrawing().find(Schematic::penPart(), id) == nullptr) missing << QString::number(id);
        done << tr("%n stroke(s) marked addressed", "", page->setStrokesAddressed(addressedIds, true));
        if (!missing.isEmpty()) o.insert(QStringLiteral("not drawn"), tr("no stroke numbered %1 on the page").arg(missing.join(QStringLiteral(", "))));
    }
    if (pen.isBool()) {
        page->setPenActive(pen.toBool());
        done << (pen.toBool() ? tr("the pen taken up") : tr("the pen put down"));
    }
    if (!done.isEmpty()) o.insert(QStringLiteral("done"), done.join(QStringLiteral("; ")));

    // What the page shows: its diagrams and their traces, by the numbers
    // the other tools take.
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
    o.insert(QStringLiteral("diagrams"), diagrams);
    o.insert(QStringLiteral("pen"), page->penActive() ? tr("taken up: a drag on the page draws on it (Escape puts it down)")
                                                      : tr("put down: its toolbar's pen takes it up"));
    // What the user drew, read stroke by stroke - what each is, what of the
    // diagrams it marks, its box in the page's coordinates - the latest 40.
    if (!page->penDrawing().isEmpty()) insertDrawings(o, qucs_s::datadisplay::readDrawing(page, false));
    else o.insert(QStringLiteral("drawings"), tr("nothing is drawn on the page"));
    return jsonResult(o);
}
