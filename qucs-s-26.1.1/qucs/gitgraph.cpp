/*
 * gitgraph.cpp - a repository's commits laid out as a graph: lanes, their
 *                colours, the lines between the rows
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "gitgraph.h"

#include "ink.h"

#include <QPalette>

#include <algorithm>

namespace qucs_s::git {

int GraphLayout::freeLane(int except)
{
    for (int i = 0; i < a_lanes.size(); ++i)
        if (i != except && a_lanes.at(i).waiting.isEmpty()) return i;
    a_lanes.append(Lane());
    return int(a_lanes.size()) - 1;
}

GraphRow GraphLayout::next(const QString& hash, const QStringList& parents, bool dashed)
{
    GraphRow row;
    row.merge = parents.size() > 1;
    // The lanes that wait for it: its children's lines end in its dot.
    QVector<int> mine;
    for (int i = 0; i < a_lanes.size(); ++i)
        if (a_lanes.at(i).waiting == hash) mine << i;
    int width = 0;
    for (int i = 0; i < a_lanes.size(); ++i)
        if (!a_lanes.at(i).waiting.isEmpty()) width = i + 1;
    if (mine.isEmpty()) {
        row.lane = freeLane();
        row.colour = newColour();
        row.tip = true;
    } else {
        row.lane = mine.first();
        row.colour = a_lanes.at(row.lane).colour;
        row.tip = false;
    }
    // Above: each line into the dot, the others past it.
    for (int i = 0; i < a_lanes.size(); ++i) {
        const Lane& l = a_lanes.at(i);
        if (l.waiting.isEmpty()) continue;
        row.above << GraphLine{i, mine.contains(i) ? row.lane : i, l.colour, l.dashed};
    }
    for (int i : mine) a_lanes[i] = Lane();
    // Below: the first parent straight down in this lane (another lane
    // that waits for it too ends in it there, the leftmost taking it -
    // a line moved early would take the branch's line out of its lane);
    // another parent into the lane that waits for it, else a new one.
    QVector<int> fromDot;
    for (int k = 0; k < parents.size(); ++k) {
        const QString& p = parents.at(k);
        if (k == 0) {
            a_lanes[row.lane] = Lane{p, row.colour, dashed};
            row.below << GraphLine{row.lane, row.lane, row.colour, dashed};
            fromDot << row.lane;
            continue;
        }
        int waiting = -1;
        for (int i = 0; i < a_lanes.size(); ++i)
            if (a_lanes.at(i).waiting == p) {
                waiting = i;
                break;
            }
        if (waiting >= 0) {
            row.below << GraphLine{row.lane, waiting, a_lanes.at(waiting).colour, dashed};
            fromDot << waiting;
            continue;
        }
        const int lane = freeLane(row.lane);
        const int colour = newColour();
        a_lanes[lane] = Lane{p, colour, dashed};
        row.below << GraphLine{row.lane, lane, colour, dashed};
        fromDot << lane;
    }
    // ...and the lines that go past it.
    for (int i = 0; i < a_lanes.size(); ++i) {
        const Lane& l = a_lanes.at(i);
        if (l.waiting.isEmpty() || fromDot.contains(i)) continue;
        row.below << GraphLine{i, i, l.colour, l.dashed};
    }
    for (int i = 0; i < a_lanes.size(); ++i)
        if (!a_lanes.at(i).waiting.isEmpty()) width = std::max(width, i + 1);
    row.width = std::max(width, row.lane + 1);
    while (!a_lanes.isEmpty() && a_lanes.last().waiting.isEmpty()) a_lanes.removeLast();
    return row;
}

void GraphLayout::clear()
{
    a_lanes.clear();
    a_colours = 0;
}

int GraphLayout::openLanes() const
{
    return int(std::count_if(a_lanes.cbegin(), a_lanes.cend(), [](const Lane& l) { return !l.waiting.isEmpty(); }));
}

int laneColourCount()
{
    return 10;
}

QColor laneColour(int index, const QPalette& palette)
{
    // Blue, red, green, purple, orange, teal, pink, brown, olive, indigo.
    static const QRgb light[] = {0x1f6fb4, 0xc62828, 0x2e7d32, 0x7b4fbf, 0xd35400, 0x00838f, 0xc2185b, 0x8d5a3b, 0x6b7b00, 0x3949ab};
    static const QRgb dark[] = {0x5ab0f0, 0xef6461, 0x6cc070, 0xb69ae6, 0xffa552, 0x4dd0e1, 0xf06ea0, 0xd1a07a, 0xc6d15a, 0x8c9eff};
    const int i = ((index % laneColourCount()) + laneColourCount()) % laneColourCount();
    return QColor::fromRgb(ink::isDark(palette.color(QPalette::Base)) ? dark[i] : light[i]);
}

} // namespace qucs_s::git
