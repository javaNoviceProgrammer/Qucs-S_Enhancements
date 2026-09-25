/***************************************************************************
                             conductor_index.cpp
                            ---------------------
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

#include "conductor_index.h"

#include "geometry/multi_point.h"
#include "node.h"
#include "wire.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <utility>

namespace qucs_s {

namespace {

bool rowOrder(const Node* a, const Node* b)
{
    return a->y() != b->y() ? a->y() < b->y() : a->x() < b->x();
}

bool columnOrder(const Node* a, const Node* b)
{
    return a->x() != b->x() ? a->x() < b->x() : a->y() < b->y();
}

// Where (major, minor) would go among nodes sorted by (major, minor), for
// lower_bound (strict = false) or upper_bound (strict = true)
template <typename Major, typename Minor>
auto boundOf(const std::vector<Node*>& sorted, int major, int minor, bool strict, Major getMajor, Minor getMinor)
{
    const auto before = [&](const Node* n) {
        const int nm = getMajor(n);
        if (nm != major) return nm < major;
        return strict ? getMinor(n) <= minor : getMinor(n) < minor;
    };
    return std::partition_point(sorted.begin(), sorted.end(), before);
}

} // namespace

NodesByPlace::NodesByPlace(const std::list<Node*>& nodes)
    : m_byRow(nodes.begin(), nodes.end()), m_byColumn(nodes.begin(), nodes.end())
{
    std::ranges::stable_sort(m_byRow, rowOrder);
    std::ranges::stable_sort(m_byColumn, columnOrder);
}

std::vector<Node*> NodesByPlace::between(const QPoint& a, const QPoint& b) const
{
    constexpr int Lowest = std::numeric_limits<int>::min();
    constexpr int Highest = std::numeric_limits<int>::max();
    const auto x = [](const Node* n) { return n->x(); };
    const auto y = [](const Node* n) { return n->y(); };
    const auto [x0, x1] = std::minmax({a.x(), b.x()});
    const auto [y0, y1] = std::minmax({a.y(), b.y()});

    std::vector<Node*>::const_iterator first, last;
    if (a.y() == b.y()) {
        // one row, strictly between the ends
        first = boundOf(m_byRow, a.y(), x0, true, y, x);
        last = boundOf(m_byRow, a.y(), x1, false, y, x);
    } else if (a.x() == b.x()) {
        first = boundOf(m_byColumn, a.x(), y0, true, x, y);
        last = boundOf(m_byColumn, a.x(), y1, false, x, y);
    } else {
        // Strictly inside the box: the rows between its edges or the columns,
        // whichever holds fewer nodes
        const auto rowsFirst = boundOf(m_byRow, y0, Highest, true, y, x);
        const auto rowsLast = boundOf(m_byRow, y1, Lowest, false, y, x);
        const auto colsFirst = boundOf(m_byColumn, x0, Highest, true, x, y);
        const auto colsLast = boundOf(m_byColumn, x1, Lowest, false, x, y);
        if (std::distance(rowsFirst, rowsLast) <= std::distance(colsFirst, colsLast)) {
            first = rowsFirst;
            last = rowsLast;
        } else {
            first = colsFirst;
            last = colsLast;
        }
    }

    std::vector<Node*> found;
    for (auto it = first; it < last; ++it) {
        if (geom::is_between(*it, a, b)) {
            found.push_back(*it);
        }
    }
    return found;
}

InsertionIndex::InsertionIndex(const std::list<Node*>& nodes, const std::list<Wire*>& wires)
{
    m_nodes.reserve(nodes.size());
    for (auto* n : nodes) {
        add(n);
    }
    for (auto* w : wires) {
        add(w);
    }
}

Node* InsertionIndex::nodeAt(const QPoint& p) const
{
    const auto it = m_nodes.find(p);
    return it == m_nodes.end() ? nullptr : it->second;
}

void InsertionIndex::add(Node* node)
{
    m_nodes.try_emplace(node->center(), node);
}

void InsertionIndex::add(Wire* wire)
{
    // A node lies on a wire only inside the wire's bounding box: file the
    // wire under every cell the box touches
    const QPoint c1 = cellOf(wire->P1());
    const QPoint c2 = cellOf(wire->P2());
    const auto [cx0, cx1] = std::minmax({c1.x(), c2.x()});
    const auto [cy0, cy1] = std::minmax({c1.y(), c2.y()});
    const long long cells = (static_cast<long long>(cx1) - cx0 + 1) * (static_cast<long long>(cy1) - cy0 + 1);
    if (cells > MaxCells) {
        m_wide.push_back(wire);
        return;
    }
    for (int cx = cx0; cx <= cx1; ++cx) {
        for (int cy = cy0; cy <= cy1; ++cy) {
            m_cells[QPoint{cx, cy}].push_back(wire);
        }
    }
}

std::vector<Wire*> InsertionIndex::wiresNear(const QPoint& p) const
{
    std::vector<Wire*> near;
    if (const auto it = m_cells.find(cellOf(p)); it != m_cells.end()) {
        near = it->second;
    }
    near.insert(near.end(), m_wide.begin(), m_wide.end());
    return near;
}

} // namespace qucs_s
