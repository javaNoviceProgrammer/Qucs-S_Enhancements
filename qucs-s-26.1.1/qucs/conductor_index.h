/***************************************************************************
                              conductor_index.h
                             -------------------
    Finding nodes and wires by place without comparing each with each.
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef QUCS_CONDUCTOR_INDEX_H
#define QUCS_CONDUCTOR_INDEX_H

#include <QPoint>

#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <vector>

class Node;
class Wire;

namespace qucs_s {

struct PointHash {
    std::size_t operator()(const QPoint& p) const noexcept
    {
        std::uint64_t k = (std::uint64_t(std::uint32_t(p.x())) << 32) | std::uint32_t(p.y());
        k ^= k >> 33;
        k *= 0xff51afd7ed558ccdULL;
        k ^= k >> 33;
        return std::size_t(k);
    }
};

// The nodes of a schematic sorted by place, to find the nodes that lie on a
// wire - in the sense of geom::is_between() - in about log(N) rather than N.
// A snapshot: it holds while no node is made, deleted or moved.
class NodesByPlace {
public:
    explicit NodesByPlace(const std::list<Node*>& nodes);

    // The nodes strictly between a and b, as geom::is_between() decides
    std::vector<Node*> between(const QPoint& a, const QPoint& b) const;

private:
    std::vector<Node*> m_byRow;     // by y, then x, then list order
    std::vector<Node*> m_byColumn;  // by x, then y, then list order
};

// Nodes by place and wires by the stretch of plane they cover, kept up to
// date while elements are only added: for loading a document, where each
// pin and wire end asks for the node at its place, and a new node splits the
// wires it lies on (Schematic::provideNode()), and for the healer's node
// replacements. Moving or deleting a node or a wire while it is in use
// leaves it wrong.
class InsertionIndex {
public:
    InsertionIndex(const std::list<Node*>& nodes, const std::list<Wire*>& wires);

    // The first node at p in the order they were added, or nullptr
    Node* nodeAt(const QPoint& p) const;
    void add(Node* node);   // keeps the node already at its place, if any
    void add(Wire* wire);

    // The wires a node at p may lie on: every one that does, and others.
    // Wires that got shorter since they were added are still among them.
    std::vector<Wire*> wiresNear(const QPoint& p) const;

private:
    static constexpr int CellShift = 7;   // cells of 128 x 128
    static constexpr long long MaxCells = 64;

    static QPoint cellOf(const QPoint& p) { return {p.x() >> CellShift, p.y() >> CellShift}; }

    std::unordered_map<QPoint, Node*, PointHash> m_nodes;
    std::unordered_map<QPoint, std::vector<Wire*>, PointHash> m_cells;
    std::vector<Wire*> m_wide;   // wires covering more than MaxCells cells
};

} // namespace qucs_s

#endif // QUCS_CONDUCTOR_INDEX_H
