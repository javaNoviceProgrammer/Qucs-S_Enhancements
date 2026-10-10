/*
 * gitgraph.h - a repository's commits laid out as a graph, as VS Code's
 *              and Eclipse's history draw it: each commit a dot in a lane,
 *              each branch's line in a colour of its own, merges and
 *              forks curving from lane to lane
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_GITGRAPH_H
#define QUCS_GITGRAPH_H

#include <QColor>
#include <QString>
#include <QStringList>
#include <QVector>

class QPalette;

namespace qucs_s::git {

/// A line of a row's graph: from lane \a from to lane \a to - in the
/// row's upper half from its top to its middle, in its lower half from its
/// middle to its bottom. A line from or to the commit's own lane at the
/// middle meets its dot.
struct GraphLine {
    int from = 0;
    int to = 0;
    int colour = 0;       ///< an index: laneColour()
    bool dashed = false;  ///< to a commit not made yet (the changes not committed)
};

/// A commit's row: its dot's lane and colour, the lines through the row.
struct GraphRow {
    int lane = 0;
    int colour = 0;
    QVector<GraphLine> above;   ///< from the top: into the dot, or past it
    QVector<GraphLine> below;   ///< to the bottom: out of the dot to its parents, or past it
    int width = 1;              ///< the lanes the row spans
    bool merge = false;         ///< more than one parent
    bool tip = true;            ///< nothing above leads to it: a branch's newest commit
};

/*!
 * Lays commits out in lanes, row after row: newest first, each commit
 * after all of its children (git log --date-order, --topo-order). A
 * commit takes the lane that waits for it (the leftmost, when several
 * children's do: the others end in it), else the first free one - a
 * branch's tip. Its first parent goes on in its lane (as git log --graph
 * draws it: two lines to one commit meet at it); another parent takes the
 * lane that waits for it, else a lane of its own, in a colour of its own.
 * Lanes keep their place: a line runs straight down until its commit.
 * Rows are added as commits come (a page more: the lanes go on).
 */
class GraphLayout
{
public:
    /// The row of the commit \a hash with \a parents; \a dashed: the
    /// lines from it are dashed (the changes not committed, to HEAD).
    GraphRow next(const QString& hash, const QStringList& parents, bool dashed = false);
    /// From the start again.
    void clear();
    /// The lanes waiting for a commit below the rows laid out.
    int openLanes() const;

private:
    struct Lane {
        QString waiting;      // the commit it leads to; empty: free
        int colour = 0;
        bool dashed = false;
    };
    int freeLane(int except = -1);
    int newColour() { return a_colours++; }

    QVector<Lane> a_lanes;
    int a_colours = 0;
};

/// The colour of lane colour \a index on \a palette's base: one of ten,
/// each standing out from a light base and from a dark one.
QColor laneColour(int index, const QPalette& palette);
/// How many colours there are before they come round again.
int laneColourCount();

} // namespace qucs_s::git

#endif // QUCS_GITGRAPH_H
